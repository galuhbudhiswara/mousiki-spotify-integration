#include "spotify_source.h"

#include "console_log.h"

namespace muisc {

bool SpotifySource::playlists(std::vector<SpotifyPlaylist>& out, std::string* error_out) {
    out.clear();
    if (!client_) {
        if (error_out) *error_out = "Spotify is not configured";
        return false;
    }
    if (!client_->supports_library()) {
        if (error_out)
            *error_out = std::string("the ") + client_->backend_name() +
                         " backend can't browse your library -- set SpotifyForceWebApi=true and sign in";
        return false;
    }

    // Saved tracks aren't a real playlist in Spotify's model, so surface
    // them as a synthetic first row the same way the Spotify clients do.
    SpotifyPlaylist liked;
    liked.id = liked_songs_id();
    liked.name = "Liked Songs";
    liked.track_count = -1; // unknown until opened; avoids an extra request
    out.push_back(std::move(liked));

    std::vector<SpotifyPlaylist> real;
    if (!client_->playlists(real, error_out)) {
        out.clear();
        return false;
    }
    out.insert(out.end(), real.begin(), real.end());
    return true;
}

bool SpotifySource::tracks(const SpotifyPlaylist& pl, std::vector<SpotifyTrack>& out,
                           std::string* error_out) {
    out.clear();
    if (!client_) {
        if (error_out) *error_out = "Spotify is not configured";
        return false;
    }
    if (pl.id == liked_songs_id()) return client_->saved_tracks(out, error_out);
    return client_->playlist_tracks(pl, out, error_out);
}

std::optional<SongResult> SpotifySource::resolve(const SpotifyTrack& track, std::string* error_out) {
    if (track.name.empty()) {
        if (error_out) *error_out = "track has no title to search for";
        return std::nullopt;
    }

    const std::string query = track.search_query();
    ConsoleLog::instance().log_basic("spotify: resolving audio for \"" + query + "\"");

    auto result = youtube_.resolve(query, error_out);
    if (!result) return std::nullopt;

    // Prefer Spotify's own metadata over whatever the video title parsed
    // to -- it's the authoritative tagging, and it keeps the metadata
    // panel and lyrics lookup consistent with what the user picked.
    result->title = track.name;
    if (!track.artist.empty()) result->artist = track.artist;
    return result;
}

} // namespace muisc
