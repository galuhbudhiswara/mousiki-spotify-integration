#include "spotify_client.h"

#include <mutex>

#include "spotify_auth.h"
#include "spotify_local_client.h"
#include "spotify_web_client.h"

namespace muisc {

// Default library implementations, inherited by the local backends:
// controlling the desktop app gives no view of the account, so these fail
// with a reason rather than returning an empty list (which would read as
// "you have no playlists").
bool SpotifyClient::playlists(std::vector<SpotifyPlaylist>&, std::string* error_out) {
    if (error_out)
        *error_out = std::string("the ") + backend_name() +
                     " backend can't list playlists -- set SpotifyForceWebApi=true and sign in";
    return false;
}

bool SpotifyClient::playlist_tracks(const SpotifyPlaylist&, std::vector<SpotifyTrack>&,
                                    std::string* error_out) {
    if (error_out)
        *error_out = std::string("the ") + backend_name() +
                     " backend can't list playlist tracks -- set SpotifyForceWebApi=true and sign in";
    return false;
}

bool SpotifyClient::saved_tracks(std::vector<SpotifyTrack>&, std::string* error_out) {
    if (error_out)
        *error_out = std::string("the ") + backend_name() +
                     " backend can't list saved tracks -- set SpotifyForceWebApi=true and sign in";
    return false;
}

// Same contract for the home-screen/search/queue surface. One shared
// phrasing keyed on the capability name, rather than a hand-written
// message per method.
static bool unsupported(const char* backend, const char* capability, std::string* error_out) {
    if (error_out)
        *error_out = std::string("the ") + backend + " backend can't " + capability +
                     " -- set SpotifyForceWebApi=true and sign in";
    return false;
}

bool SpotifyClient::search_tracks(const std::string&, int, std::vector<SpotifyTrack>&,
                                  std::string* error_out) {
    return unsupported(backend_name(), "search the Spotify catalogue", error_out);
}

bool SpotifyClient::queue(std::vector<SpotifyTrack>&, std::string* error_out) {
    return unsupported(backend_name(), "read the playback queue", error_out);
}

bool SpotifyClient::enqueue(const std::string&, std::string* error_out) {
    return unsupported(backend_name(), "add to the playback queue", error_out);
}

bool SpotifyClient::recently_played(int, std::vector<SpotifyTrack>&, std::string* error_out) {
    return unsupported(backend_name(), "list recently played tracks", error_out);
}

bool SpotifyClient::top_tracks(const std::string&, int, std::vector<SpotifyTrack>&,
                               std::string* error_out) {
    return unsupported(backend_name(), "list your top tracks", error_out);
}

bool SpotifyClient::top_artists(const std::string&, int, std::vector<SpotifyArtist>&,
                                std::string* error_out) {
    return unsupported(backend_name(), "list your top artists", error_out);
}

bool SpotifyClient::saved_albums(std::vector<SpotifyAlbum>&, std::string* error_out) {
    return unsupported(backend_name(), "list your saved albums", error_out);
}

bool SpotifyClient::album_tracks(const SpotifyAlbum&, std::vector<SpotifyTrack>&,
                                 std::string* error_out) {
    return unsupported(backend_name(), "list an album's tracks", error_out);
}

bool SpotifyClient::artist_top_tracks(const SpotifyArtist&, std::vector<SpotifyTrack>&,
                                      std::string* error_out) {
    return unsupported(backend_name(), "list an artist's top tracks", error_out);
}

bool SpotifyClient::devices(std::vector<SpotifyDevice>&, std::string* error_out) {
    return unsupported(backend_name(), "list your Spotify devices", error_out);
}

bool SpotifyClient::play_uri(const std::string&, std::string* error_out, std::string*) {
    if (error_out)
        *error_out = std::string("the ") + backend_name() + " backend can't start a specific track";
    return false;
}

// ---------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------

namespace {
std::mutex g_mutex;
std::shared_ptr<SpotifyClient> g_cached;
} // namespace

std::shared_ptr<SpotifyClient> SpotifyClientFactory::create(bool force_web_api,
                                                            const std::string& client_id,
                                                            std::string* reason_out) {
    // Build the Web API backend only when there is a usable session.
    auto try_web = [&](std::string* why) -> std::shared_ptr<SpotifyClient> {
        // Client id is checked FIRST because it is the earlier blocker:
        // without one you cannot even start a sign-in, so reporting "not
        // signed in" to someone who hasn't registered an app yet points
        // them at the wrong step.
        if (client_id.empty()) {
            if (why) *why = "SpotifyClientId is not set in $HOME/.config/mousiki/config.txt (get one free at developer.spotify.com/dashboard)";
            return nullptr;
        }
        SpotifyTokens tokens = SpotifyAuth::load();
        if (!tokens.valid()) {
            if (why) *why = "not signed in to Spotify";
            return nullptr;
        }
        return std::make_shared<SpotifyWebClient>(client_id, std::move(tokens));
    };

    if (force_web_api) return try_web(reason_out);

    // Platform backend next -- no account, no network, no Premium.
    if (SpotifyLocalClient::platform_supported() && SpotifyLocalClient::spotify_running()) {
        return std::make_shared<SpotifyLocalClient>();
    }

    // Platform backend present but the desktop app isn't running: fall
    // through to the Web API rather than hand back a client whose every
    // call fails. A terminal player is often the only Spotify client
    // around.
    std::string web_why;
    if (auto web = try_web(&web_why)) return web;

    if (reason_out) {
        if (SpotifyLocalClient::platform_supported()) {
            *reason_out = "Spotify desktop app isn't running, and " + web_why;
        } else {
            *reason_out = web_why;
        }
    }
    return nullptr;
}

std::shared_ptr<SpotifyClient> SpotifyClientFactory::get(bool force_web_api,
                                                         const std::string& client_id,
                                                         std::string* reason_out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cached) g_cached = create(force_web_api, client_id, reason_out);
    return g_cached;
}

void SpotifyClientFactory::reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cached.reset();
}

} // namespace muisc
