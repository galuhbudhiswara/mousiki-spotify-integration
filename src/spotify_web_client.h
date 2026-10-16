#pragma once
#include <string>
#include <vector>

#include "spotify_auth.h"
#include "spotify_client.h"

namespace muisc {

// Spotify Web API backend. Controls whatever device the account is
// currently playing on, so it works with no local Spotify desktop app at
// all (phone, speaker, another machine) -- and is the only backend that
// can enumerate the account's playlists and saved tracks.
//
// Transport control needs Premium: the /me/player/* write endpoints
// answer 403 for free accounts. Reading state works on free accounts.
class SpotifyWebClient : public SpotifyClient {
public:
    SpotifyWebClient(std::string client_id, SpotifyTokens tokens);

    const char* backend_name() const override { return "web-api"; }
    bool supports_library() const override { return true; }

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

    bool playlists(std::vector<SpotifyPlaylist>& out, std::string* error_out = nullptr) override;
    bool playlist_tracks(const SpotifyPlaylist& pl, std::vector<SpotifyTrack>& out,
                         std::string* error_out = nullptr) override;
    bool saved_tracks(std::vector<SpotifyTrack>& out, std::string* error_out = nullptr) override;

    bool devices(std::vector<SpotifyDevice>& out, std::string* error_out = nullptr) override;
    bool play_uri(const std::string& uri, std::string* error_out = nullptr,
                  std::string* device_used_out = nullptr) override;
    bool play_uris(const std::vector<std::string>& uris, int offset_index = 0,
                   std::string* error_out = nullptr,
                   std::string* device_used_out = nullptr) override;

    bool search_tracks(const std::string& query, int limit,
                       std::vector<SpotifyTrack>& out, std::string* error_out = nullptr) override;
    bool queue(std::vector<SpotifyTrack>& out, std::string* error_out = nullptr) override;
    bool enqueue(const std::string& uri, std::string* error_out = nullptr) override;
    bool recently_played(int limit, std::vector<SpotifyTrack>& out,
                         std::string* error_out = nullptr) override;
    bool top_tracks(const std::string& time_range, int limit,
                    std::vector<SpotifyTrack>& out, std::string* error_out = nullptr) override;
    bool top_artists(const std::string& time_range, int limit,
                     std::vector<SpotifyArtist>& out, std::string* error_out = nullptr) override;
    bool saved_albums(std::vector<SpotifyAlbum>& out, std::string* error_out = nullptr) override;
    bool album_tracks(const SpotifyAlbum& album, std::vector<SpotifyTrack>& out,
                      std::string* error_out = nullptr) override;
    bool artist_top_tracks(const SpotifyArtist& artist, std::vector<SpotifyTrack>& out,
                           std::string* error_out = nullptr) override;

private:
    // PUT /me/player/play with an already-built body, including the
    // "no active device" recovery both play_uri() and play_uris() need:
    // a device-less play command goes to the active device, and an open
    // but idle Spotify client is not active -- which is how a terminal
    // remote control finds the account most of the time.
    bool start_playback(const std::string& json, std::string* error_out,
                        std::string* device_used_out);

    static constexpr const char* kApiBase = "https://api.spotify.com/v1/";
    // Hard ceiling imposed by Spotify's February 2026 revision, which cut
    // the search limit maximum from 50 to 10. Exceeding it is a 400, not
    // a silent clamp, so every search path routes through this.
    static constexpr int kSearchLimitMax = 10;

    // Performs `method api_base+path`. On a 401, refreshes the access
    // token and replays the request once; the single-shot guard keeps a
    // permanently-invalid token from looping forever.
    //
    // A 204 (No Content), which every transport endpoint returns on
    // success, comes back as true with an empty `body_out`.
    bool request_with_refresh(const std::string& method,
                              const std::string& path,
                              const std::string& body,
                              std::string& body_out,
                              std::string* error_out);

    // Pages through a Spotify "paging object" collection, following
    // offset/total until everything is collected. Caps total pages so a
    // surprising `total` can't spin.
    bool get_all_pages(const std::string& path_base,
                       int limit,
                       std::vector<std::string>& page_bodies_out,
                       std::string* error_out);

    std::string client_id_;
    SpotifyTokens tokens_;

    // Cached from the last status() call so play_pause()/toggle_* can
    // decide which way to flip without an extra round-trip.
    SpotifyPlayerState last_known_player_;
    bool have_last_known_ = false;
    int pre_mute_volume_ = 0;
};

} // namespace muisc
