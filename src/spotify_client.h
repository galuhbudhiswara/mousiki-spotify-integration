#pragma once
#include <memory>
#include <string>
#include <vector>

namespace muisc {

// ---------------------------------------------------------------------
// Playback snapshot
//
// The shape every backend normalizes to before the UI sees it, which is
// what makes the backends interchangeable.
// ---------------------------------------------------------------------
struct SpotifyPlayerState {
    std::string state;        // "playing" | "paused" | "stopped"
    int volume = 0;           // 0..100
    long long position_ms = 0;
    long long duration_ms = 0;
    bool is_repeating = false;
    bool is_shuffling = false;
};

struct SpotifyTrackInfo {
    std::string name;
    std::string artist;
    std::string album;
    std::string uri;          // spotify:track:... (empty on some backends)

    // Cover art for `album`, as a CDN URL. Only the Web API backend
    // fills this in -- the MPRIS/AppleScript backends drive the desktop
    // app and never see the catalogue metadata it came from. Empty means
    // "no art", which the disk panel reads as "keep showing the vinyl".
    std::string art_url;
};

struct SpotifyStatus {
    bool is_running = false;  // Spotify reachable AND something is loaded
    SpotifyPlayerState player;
    SpotifyTrackInfo track;

    // Name of the device actually rendering the audio ("Galuh's iPhone",
    // "MacBook Pro", a speaker, ...). "Which box is this coming out of"
    // is the one fact a remote control has to show. Empty on the
    // MPRIS/AppleScript backends, which only ever drive this machine.
    std::string device;

    std::string display() const {
        if (!is_running) return "";
        if (track.name.empty()) return "";
        return track.artist.empty() ? track.name : track.artist + " - " + track.name;
    }
};

// A playback device the account can reach, from /me/player/devices.
//
// `is_active` carries the remote-control model: Spotify routes a play
// command with no device_id to the active device, and a client that is
// merely open -- not playing -- is listed with is_active=false. Playing
// in that state answers 404 NO_ACTIVE_DEVICE, so play_uri() names a
// device explicitly.
//
// `is_restricted` devices (some speakers, cars, consoles) accept no API
// control at all and must never be chosen as a target.
struct SpotifyDevice {
    std::string id;
    std::string name;
    std::string type;         // "Computer", "Smartphone", "Speaker", ...
    bool is_active = false;
    bool is_restricted = false;
    int volume_percent = 0;
};

// A playlist as listed by the Web API. The local (MPRIS/AppleScript)
// backends cannot enumerate playlists: they control the desktop app, not
// the account.
struct SpotifyPlaylist {
    std::string id;
    std::string name;
    std::string uri;
    std::string owner_id;
    int track_count = 0;
};

struct SpotifyTrack {
    std::string id;
    std::string uri;
    std::string name;
    std::string artist;
    std::string album;
    long long duration_ms = 0;

    // Album cover URL, same as SpotifyTrackInfo::art_url. Carried on the
    // list rows too so pressing Enter can put the right cover on the
    // disk immediately, instead of the panel sitting on the previous
    // track's artwork until the next status poll lands.
    std::string art_url;

    // "Artist - Title", the search string mousiki hands to yt-dlp when
    // it needs actual decodable audio for this track.
    std::string search_query() const {
        return artist.empty() ? name : artist + " - " + name;
    }
};

// An artist, as listed by /me/top/artists and /me/following. `id` is
// what artist_top_tracks() needs to drill in. Related Artists is one of
// the endpoints closed to new apps (see SpotifyClient below), so an
// artist's own top tracks is as far as artist browsing goes.
struct SpotifyArtist {
    std::string id;
    std::string uri;
    std::string name;
    std::string genres;   // comma-joined; "" when Spotify sends none
    int popularity = 0;
};

// A saved album from /me/albums.
struct SpotifyAlbum {
    std::string id;
    std::string uri;
    std::string name;
    std::string artist;
    std::string release_date; // ISO-ish; Spotify's precision varies (year / year-month / full)
    int track_count = 0;

    // Just the year, for the list's narrow date column -- release_date
    // may be "2014", "2014-03" or "2014-03-17" depending on how much
    // Spotify knows, and all three start with the year.
    std::string year() const { return release_date.substr(0, 4); }
};

// ---------------------------------------------------------------------
// Backend interface
//
// Transport commands are fire-and-forget: they report whether the
// command was dispatched, not whether Spotify finished acting on it.
// Real state always comes back via status().
//
// Backends declare up front what they can do via supports_library(), so
// the UI can hide what isn't available instead of offering a no-op.
// ---------------------------------------------------------------------
class SpotifyClient {
public:
    virtual ~SpotifyClient() = default;

    virtual const char* backend_name() const = 0;

    // True when this backend can enumerate playlists / saved tracks.
    // Only the Web API backend can.
    virtual bool supports_library() const { return false; }

    // Current playback snapshot. Returns false when Spotify is not
    // running / no active device, with `error_out` set.
    virtual bool status(SpotifyStatus& out, std::string* error_out = nullptr) = 0;

    virtual bool play(std::string* error_out = nullptr) = 0;
    virtual bool pause(std::string* error_out = nullptr) = 0;
    virtual bool play_pause(std::string* error_out = nullptr) = 0;
    virtual bool next(std::string* error_out = nullptr) = 0;
    virtual bool previous(std::string* error_out = nullptr) = 0;

    virtual bool set_volume(int percent, std::string* error_out = nullptr) = 0;
    virtual bool seek(long long position_ms, std::string* error_out = nullptr) = 0;
    virtual bool toggle_shuffle(std::string* error_out = nullptr) = 0;
    virtual bool toggle_repeat(std::string* error_out = nullptr) = 0;

    // Library browsing -- only meaningful when supports_library().
    // Default implementations fail with a clear reason instead of
    // silently doing nothing.
    virtual bool playlists(std::vector<SpotifyPlaylist>& out, std::string* error_out = nullptr);
    virtual bool playlist_tracks(const SpotifyPlaylist& pl, std::vector<SpotifyTrack>& out,
                                 std::string* error_out = nullptr);
    virtual bool saved_tracks(std::vector<SpotifyTrack>& out, std::string* error_out = nullptr);

    // --- Home / search / queue -------------------------------------
    // Web-API-only, and what the browsable Spotify home screen is built
    // from.
    //
    // There is no recommendations(), featured_playlists(),
    // category_playlists(), related_artists() or audio_features(), and
    // there cannot be: Spotify closed all of those -- plus preview URLs
    // and its own algorithmic playlists (Discover Weekly, Release Radar,
    // Daily Mix) -- to applications registered on or after 2024-11-27.
    // Each mousiki user registers their own client id, so none of it is
    // reachable.
    //
    // Hence a home screen built only from the user's own data, and a
    // locally generated "Daily Mix" (see spotify_home.h).

    // Catalogue search. `limit` is capped at 50 by Spotify per request.
    virtual bool search_tracks(const std::string& query, int limit,
                               std::vector<SpotifyTrack>& out, std::string* error_out = nullptr);

    // The account's real up-next queue, as the Spotify clients show it.
    // Shared state: what mousiki enqueues appears on the phone/desktop
    // and vice versa.
    virtual bool queue(std::vector<SpotifyTrack>& out, std::string* error_out = nullptr);

    // Appends one track to that same shared queue.
    virtual bool enqueue(const std::string& uri, std::string* error_out = nullptr);

    // Most recently played tracks, newest first. Spotify caps this at 50
    // and does not paginate it by offset, so there is no limit argument
    // beyond that cap.
    virtual bool recently_played(int limit, std::vector<SpotifyTrack>& out,
                                 std::string* error_out = nullptr);

    // `time_range` is "short_term" (~4 weeks), "medium_term" (~6 months)
    // or "long_term" (years). Anything else is rejected by Spotify.
    virtual bool top_tracks(const std::string& time_range, int limit,
                            std::vector<SpotifyTrack>& out, std::string* error_out = nullptr);
    virtual bool top_artists(const std::string& time_range, int limit,
                             std::vector<SpotifyArtist>& out, std::string* error_out = nullptr);

    virtual bool saved_albums(std::vector<SpotifyAlbum>& out, std::string* error_out = nullptr);
    virtual bool album_tracks(const SpotifyAlbum& album, std::vector<SpotifyTrack>& out,
                              std::string* error_out = nullptr);
    virtual bool artist_top_tracks(const SpotifyArtist& artist, std::vector<SpotifyTrack>& out,
                                   std::string* error_out = nullptr);

    // Every device the account can currently reach. Web API only.
    virtual bool devices(std::vector<SpotifyDevice>& out, std::string* error_out = nullptr);

    // Starts playback of a specific track/context on Spotify itself.
    // Web API only.
    //
    // Falls back to explicitly targeting a device when none is active.
    // `device_used_out`, if given, receives the name of the device
    // playback was handed to, so the UI can say where the sound went --
    // it may well be a different machine.
    virtual bool play_uri(const std::string& uri, std::string* error_out = nullptr,
                          std::string* device_used_out = nullptr);
};

// ---------------------------------------------------------------------
// Backend selection
//
//   - an explicit "force web API" config flag wins outright;
//   - otherwise macOS -> AppleScript, Linux -> MPRIS/D-Bus;
//   - anything else -> Web API.
//
// A platform backend that exists but has no Spotify desktop app behind
// it falls back to the Web API too -- a terminal player is often the
// only Spotify client on the machine.
// ---------------------------------------------------------------------
class SpotifyClientFactory {
public:
    // `client_id` is only needed for the Web API backend. Returns null
    // when no backend can be constructed (e.g. Web API selected but the
    // user has not signed in), with `reason_out` explaining why.
    static std::shared_ptr<SpotifyClient> create(bool force_web_api,
                                                 const std::string& client_id,
                                                 std::string* reason_out = nullptr);

    // Process-wide cached instance, created on first use.
    static std::shared_ptr<SpotifyClient> get(bool force_web_api,
                                              const std::string& client_id,
                                              std::string* reason_out = nullptr);

    // Drops the cached instance (after sign-in/sign-out or a config change).
    static void reset();
};

} // namespace muisc
