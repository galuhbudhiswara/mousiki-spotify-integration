#pragma once
#include <string>

#include "spotify_client.h"

namespace muisc {

// Controls the locally running Spotify desktop app rather than the
// account, through whatever remote-control surface the OS offers:
//
//   Linux -> `dbus-send` against org.mpris.MediaPlayer2.spotify
//   macOS -> osascript against the Spotify application
//
// Needs no account, client ID, network or Premium -- but only works while
// the desktop app is running on this machine, and cannot enumerate
// playlists (supports_library() stays false).
class SpotifyLocalClient : public SpotifyClient {
public:
    // True when this platform has a local backend at all (Linux/macOS).
    static bool platform_supported();

    // True when the Spotify desktop app appears to be running right now.
    // Used both for backend selection and to give a useful error rather
    // than a silent no-op.
    static bool spotify_running();

    const char* backend_name() const override;

    bool status(SpotifyStatus& out, std::string* error_out = nullptr) override;

    bool play(std::string* error_out = nullptr) override;
    bool pause(std::string* error_out = nullptr) override;
    bool play_pause(std::string* error_out = nullptr) override;
    bool next(std::string* error_out = nullptr) override;
    bool previous(std::string* error_out = nullptr) override;

    bool set_volume(int percent, std::string* error_out = nullptr) override;
    bool seek(long long position_ms, std::string* error_out = nullptr) override;
    bool toggle_shuffle(std::string* error_out = nullptr) override;
    bool toggle_repeat(std::string* error_out = nullptr) override;

private:
    // Sends one MPRIS method / AppleScript command. Returns false (with
    // a reason) when the app isn't reachable.
    bool dispatch(const std::string& mpris_method,
                  const std::string& applescript,
                  std::string* error_out);
};

} // namespace muisc
