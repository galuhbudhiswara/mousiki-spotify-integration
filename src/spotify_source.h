#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "spotify_client.h"
#include "youtube_source.h"

namespace muisc {

// Bridges Spotify into mousiki's existing source/playback pipeline,
// alongside LocalSource and OnlineSource.
//
// Spotify's streams are DRM-protected, and the only sanctioned route to
// decoded PCM is Spotify's own closed SDK. So this uses the Web API as a
// catalogue only: browse the account's playlists and saved tracks, then
// resolve a chosen track to decodable audio through the yt-dlp path
// OnlineSource already uses. The bytes arrive the same way OnlineSource's
// do, which is what keeps the visualizers and synced lyrics working.
//
// Transport control and now-playing are SpotifyClient's job instead.
class SpotifySource {
public:
    SpotifySource(std::shared_ptr<SpotifyClient> client, CacheManager& cache)
        : client_(std::move(client)), youtube_(cache) {}

    bool available() const { return client_ && client_->supports_library(); }

    // Lists the account's playlists, plus a synthetic "Liked Songs"
    // entry at the front (Spotify models saved tracks as /me/tracks
    // rather than a real playlist, so it has no id of its own).
    bool playlists(std::vector<SpotifyPlaylist>& out, std::string* error_out = nullptr);

    // Uniform identifier for the synthetic Liked Songs playlist.
    static const char* liked_songs_id() { return "__liked__"; }

    // Lists the tracks of a playlist, transparently handling the
    // synthetic Liked Songs entry.
    bool tracks(const SpotifyPlaylist& pl, std::vector<SpotifyTrack>& out,
                std::string* error_out = nullptr);

    // Resolves a Spotify track to a locally cached, decodable audio
    // file by handing "Artist - Title" to the existing yt-dlp path.
    // Already-cached tracks come back without any network access (see
    // YoutubeSource::resolve).
    std::optional<SongResult> resolve(const SpotifyTrack& track, std::string* error_out = nullptr);

private:
    std::shared_ptr<SpotifyClient> client_;
    YoutubeSource youtube_;
};

} // namespace muisc
