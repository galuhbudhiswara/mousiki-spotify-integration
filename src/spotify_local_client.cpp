#include "spotify_local_client.h"

#include <algorithm>
#include <cctype>   // std::isdigit / std::tolower
#include <cmath>
#include <cstring>  // std::strlen

#include "console_log.h"
#include "process_util.h"
#include "tiny_json.h"

namespace muisc {

namespace {

#if defined(MUISC_PLATFORM_LINUX) || defined(MUISC_PLATFORM_ANDROID)
constexpr bool kIsLinux = true;
#else
constexpr bool kIsLinux = false;
#endif

#if defined(MUISC_PLATFORM_MACOS)
constexpr bool kIsMac = true;
#else
constexpr bool kIsMac = false;
#endif

// --- Linux / MPRIS ----------------------------------------------------
constexpr const char* kSpDest = "org.mpris.MediaPlayer2.spotify";
constexpr const char* kSpPath = "/org/mpris/MediaPlayer2";
constexpr const char* kSpIface = "org.mpris.MediaPlayer2.Player";

std::string mpris_call(const std::string& method) {
    return std::string("dbus-send --print-reply --dest=") + kSpDest + " " + kSpPath +
           " " + kSpIface + "." + method;
}

std::string mpris_get(const std::string& property) {
    return std::string("dbus-send --print-reply --dest=") + kSpDest + " " + kSpPath +
           " org.freedesktop.DBus.Properties.Get" +
           " string:" + shell_quote(kSpIface) + " string:" + shell_quote(property);
}

// dbus-send's reply for a property is a typed variant, e.g.
//   variant       string "Playing"
// Pulls the first quoted string out of it.
std::string first_quoted(const std::string& s) {
    size_t a = s.find('"');
    if (a == std::string::npos) return "";
    size_t b = s.find('"', a + 1);
    if (b == std::string::npos) return "";
    return s.substr(a + 1, b - a - 1);
}

// Metadata comes back as a dict of entries:
//   dict entry( string "xesam:title"  variant string "The War" )
// Find the entry whose key matches, then take the first quoted string
// after it.
std::string mpris_metadata_field(const std::string& reply, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = reply.find(needle);
    if (pos == std::string::npos) return "";
    size_t after = pos + needle.size();

    // For xesam:artist the value is an array of strings; either way the
    // first quoted token after the key is the value we want.
    size_t a = reply.find('"', after);
    if (a == std::string::npos) return "";
    size_t b = reply.find('"', a + 1);
    if (b == std::string::npos) return "";
    return reply.substr(a + 1, b - a - 1);
}

// mpris:length is an int64 variant in microseconds.
long long mpris_metadata_int(const std::string& reply, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = reply.find(needle);
    if (pos == std::string::npos) return 0;
    size_t i = pos + needle.size();
    // skip to the first digit on the same logical entry
    while (i < reply.size() && !std::isdigit(static_cast<unsigned char>(reply[i]))) {
        if (reply[i] == '\n' && reply.compare(i, 12, "\ndict entry(") == 0) return 0;
        ++i;
    }
    long long v = 0;
    while (i < reply.size() && std::isdigit(static_cast<unsigned char>(reply[i]))) {
        v = v * 10 + (reply[i] - '0');
        ++i;
    }
    return v;
}

// --- macOS / AppleScript ---------------------------------------------
std::string osascript(const std::string& script) {
    return "osascript -e " + shell_quote(script);
}

// State, track, repeat and shuffle in one script, so a status refresh
// costs a single osascript spawn instead of four.
constexpr const char* kMacStatusScript = R"APPLESCRIPT(
on esc(s)
  set AppleScript's text item delimiters to "\""
  set l to every text item of s
  set AppleScript's text item delimiters to "\\\""
  set s to l as string
  set AppleScript's text item delimiters to ""
  return s
end esc

if application "Spotify" is not running then
  return "{\"running\": false}"
end if

tell application "Spotify"
  set out to "{"
  set out to out & "\"running\": true"
  set out to out & ",\"state\": \"" & player state & "\""
  set out to out & ",\"volume\": " & sound volume
  set out to out & ",\"position\": " & (player position as integer)
  set out to out & ",\"repeating\": " & (repeating as string)
  set out to out & ",\"shuffling\": " & (shuffling as string)
  try
    set out to out & ",\"name\": \"" & my esc(current track's name) & "\""
    set out to out & ",\"artist\": \"" & my esc(current track's artist) & "\""
    set out to out & ",\"album\": \"" & my esc(current track's album) & "\""
    set out to out & ",\"duration\": " & current track's duration
    set out to out & ",\"id\": \"" & current track's id & "\""
  end try
  set out to out & "}"
  return out
end tell
)APPLESCRIPT";

} // namespace

bool SpotifyLocalClient::platform_supported() {
    return kIsLinux || kIsMac;
}

bool SpotifyLocalClient::spotify_running() {
    if (kIsMac) {
        ProcResult r = run_capture(osascript("get running of application \"Spotify\""));
        return r.ok() && r.out.find("true") != std::string::npos;
    }
    if (kIsLinux) {
        // Ask the bus whether the MPRIS name is owned -- cheaper and more
        // accurate than scanning the process table (covers Flatpak/snap
        // installs too, which run under different process names).
        ProcResult r = run_capture(
            "dbus-send --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus "
            "org.freedesktop.DBus.NameHasOwner string:" + std::string(shell_quote(kSpDest)),
            true);
        return r.ok() && r.out.find("true") != std::string::npos;
    }
    return false;
}

const char* SpotifyLocalClient::backend_name() const {
    if (kIsMac) return "applescript";
    if (kIsLinux) return "mpris";
    return "local-unsupported";
}

bool SpotifyLocalClient::dispatch(const std::string& mpris_method,
                                  const std::string& applescript,
                                  std::string* error_out) {
    std::string cmd;
    if (kIsMac) {
        if (applescript.empty()) {
            if (error_out) *error_out = "not supported on macOS";
            return false;
        }
        cmd = osascript(applescript);
    } else if (kIsLinux) {
        if (mpris_method.empty()) {
            if (error_out) *error_out = "not supported over MPRIS";
            return false;
        }
        cmd = mpris_call(mpris_method);
    } else {
        if (error_out) *error_out = "no local Spotify backend on this platform";
        return false;
    }

    ProcResult r = run_capture(cmd, true);
    ConsoleLog::instance().log_command(cmd, r.out, r.exit_code);
    if (!r.ok()) {
        if (error_out) *error_out = "Spotify desktop app is not responding";
        return false;
    }
    return true;
}

bool SpotifyLocalClient::status(SpotifyStatus& out, std::string* error_out) {
    out = SpotifyStatus{};

    if (kIsMac) {
        ProcResult r = run_capture(osascript(kMacStatusScript), true);
        ConsoleLog::instance().log_command("osascript <spotify status>", r.out, r.exit_code);
        if (!r.ok()) {
            if (error_out) *error_out = "Spotify isn't running";
            return false;
        }
        tinyjson::Value v;
        if (!tinyjson::parse(r.out, v)) {
            if (error_out) *error_out = "could not parse AppleScript status";
            return false;
        }
        const auto* running = v.find("running");
        if (!running || !running->as_bool(false)) {
            if (error_out) *error_out = "Spotify isn't running";
            return false;
        }
        out.is_running = true;
        if (const auto* x = v.find("state")) out.player.state = x->as_string("paused");
        if (const auto* x = v.find("volume")) out.player.volume = static_cast<int>(x->as_number(0));
        if (const auto* x = v.find("position")) out.player.position_ms = static_cast<long long>(x->as_number(0)) * 1000;
        if (const auto* x = v.find("duration")) out.player.duration_ms = static_cast<long long>(x->as_number(0));
        // AppleScript emits bare true/false for these, which tiny_json
        // reads as Bool.
        if (const auto* x = v.find("repeating")) out.player.is_repeating = x->as_bool(false);
        if (const auto* x = v.find("shuffling")) out.player.is_shuffling = x->as_bool(false);
        if (const auto* x = v.find("name")) out.track.name = x->as_string();
        if (const auto* x = v.find("artist")) out.track.artist = x->as_string();
        if (const auto* x = v.find("album")) out.track.album = x->as_string();
        if (const auto* x = v.find("id")) out.track.uri = x->as_string();
        return true;
    }

    if (kIsLinux) {
        ProcResult ps = run_capture(mpris_get("PlaybackStatus"), true);
        ConsoleLog::instance().log_command("dbus-send PlaybackStatus", ps.out, ps.exit_code);
        if (!ps.ok()) {
            if (error_out) *error_out = "Spotify isn't running";
            return false;
        }
        ProcResult md = run_capture(mpris_get("Metadata"), true);
        ConsoleLog::instance().log_command("dbus-send Metadata", md.out, md.exit_code);
        if (!md.ok()) {
            if (error_out) *error_out = "Spotify isn't running";
            return false;
        }

        const std::string playback = first_quoted(ps.out); // "Playing"/"Paused"/"Stopped"
        if (playback.empty()) {
            if (error_out) *error_out = "Spotify isn't running";
            return false;
        }

        out.is_running = true;
        if (playback == "Playing") out.player.state = "playing";
        else if (playback == "Paused") out.player.state = "paused";
        else out.player.state = "stopped";

        out.track.name = mpris_metadata_field(md.out, "xesam:title");
        out.track.artist = mpris_metadata_field(md.out, "xesam:artist");
        out.track.album = mpris_metadata_field(md.out, "xesam:album");
        out.track.uri = mpris_metadata_field(md.out, "mpris:trackid");
        out.player.duration_ms = mpris_metadata_int(md.out, "mpris:length") / 1000; // us -> ms

        // MPRIS exposes Volume and Position, but Spotify's Linux client
        // implements neither. Left at 0 so the UI can tell "unknown"
        // from a real value; set_volume() goes through PulseAudio.
        out.player.volume = 0;
        out.player.position_ms = 0;
        return true;
    }

    if (error_out) *error_out = "no local Spotify backend on this platform";
    return false;
}

bool SpotifyLocalClient::play(std::string* error_out) {
    return dispatch("Play", "tell application \"Spotify\" to play", error_out);
}

bool SpotifyLocalClient::pause(std::string* error_out) {
    return dispatch("Pause", "tell application \"Spotify\" to pause", error_out);
}

bool SpotifyLocalClient::play_pause(std::string* error_out) {
    return dispatch("PlayPause", "tell application \"Spotify\" to playpause", error_out);
}

bool SpotifyLocalClient::next(std::string* error_out) {
    return dispatch("Next", "tell application \"Spotify\" to next track", error_out);
}

bool SpotifyLocalClient::previous(std::string* error_out) {
    return dispatch("Previous", "tell application \"Spotify\" to previous track", error_out);
}

bool SpotifyLocalClient::set_volume(int percent, std::string* error_out) {
    const int clamped = std::clamp(percent, 0, 100);
    if (kIsMac) {
        return dispatch("", "tell application \"Spotify\" to set sound volume to " +
                                std::to_string(clamped), error_out);
    }
    if (kIsLinux) {
        // Spotify's Linux client ignores MPRIS Volume, so route through
        // PulseAudio/PipeWire's sink input instead.
        ProcResult list = run_capture("pactl list sink-inputs", true);
        ConsoleLog::instance().log_command("pactl list sink-inputs", list.out, list.exit_code);
        if (!list.ok()) {
            if (error_out) *error_out = "pactl not available -- cannot set Spotify volume on Linux";
            return false;
        }

        // Find the sink-input index whose block mentions Spotify.
        std::string index;
        size_t pos = 0;
        while (true) {
            size_t start = list.out.find("Sink Input #", pos);
            if (start == std::string::npos) break;
            size_t end = list.out.find("Sink Input #", start + 1);
            const std::string block = list.out.substr(
                start, end == std::string::npos ? std::string::npos : end - start);

            std::string lowered = block;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lowered.find("spotify") != std::string::npos) {
                size_t h = start + std::strlen("Sink Input #");
                while (h < list.out.size() && std::isdigit(static_cast<unsigned char>(list.out[h]))) {
                    index += list.out[h];
                    ++h;
                }
                break;
            }
            if (end == std::string::npos) break;
            pos = end;
        }

        if (index.empty()) {
            if (error_out) *error_out = "no Spotify audio stream found in PulseAudio";
            return false;
        }

        const std::string cmd = "pactl set-sink-input-volume " + index + " " +
                                std::to_string(clamped) + "%";
        ProcResult r = run_capture(cmd, true);
        ConsoleLog::instance().log_command(cmd, r.out, r.exit_code);
        if (!r.ok()) {
            if (error_out) *error_out = "pactl failed to set volume";
            return false;
        }
        return true;
    }
    if (error_out) *error_out = "no local Spotify backend on this platform";
    return false;
}

bool SpotifyLocalClient::seek(long long position_ms, std::string* error_out) {
    if (position_ms < 0) position_ms = 0;
    if (kIsMac) {
        return dispatch("", "tell application \"Spotify\" to set player position to " +
                                std::to_string(position_ms / 1000), error_out);
    }
    if (kIsLinux) {
        // MPRIS SetPosition needs the track's object path, which makes it
        // awkward; Spotify does honour the simpler relative Seek(offset),
        // so turn an absolute target into an offset from the current
        // position. Spotify's Linux client reports Position as 0, so this
        // is only reliable as a relative nudge -- documented here rather
        // than silently misbehaving.
        const std::string cmd = std::string("dbus-send --print-reply --dest=") + kSpDest + " " +
                                kSpPath + " " + kSpIface + ".Seek int64:" +
                                std::to_string(position_ms * 1000);
        ProcResult r = run_capture(cmd, true);
        ConsoleLog::instance().log_command(cmd, r.out, r.exit_code);
        if (!r.ok()) {
            if (error_out) *error_out = "Spotify did not accept the seek";
            return false;
        }
        return true;
    }
    if (error_out) *error_out = "no local Spotify backend on this platform";
    return false;
}

bool SpotifyLocalClient::toggle_shuffle(std::string* error_out) {
    if (kIsMac) {
        return dispatch("", "tell application \"Spotify\" to set shuffling to not shuffling", error_out);
    }
    // MPRIS exposes Shuffle as a writable property, but Spotify's Linux
    // client does not implement the setter. Refuse rather than no-op.
    if (error_out) *error_out = "Spotify's Linux client doesn't expose shuffle over MPRIS";
    return false;
}

bool SpotifyLocalClient::toggle_repeat(std::string* error_out) {
    if (kIsMac) {
        return dispatch("", "tell application \"Spotify\" to set repeating to not repeating", error_out);
    }
    if (error_out) *error_out = "Spotify's Linux client doesn't expose repeat over MPRIS";
    return false;
}

} // namespace muisc
