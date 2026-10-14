#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "spotify_client.h"

namespace muisc {

// ---------------------------------------------------------------------
// The Spotify home screen's model.
//
// Spotify's own Home is a recommendation feed, and every endpoint behind
// it closed to newly registered apps on 2024-11-27 (see SpotifyClient).
// So this screen is assembled from what still answers, all of it about
// the account rather than the catalogue: top tracks/artists, recently
// played, saved tracks and albums, own playlists. Daily Mix is the one
// entry that looks like a recommendation, and build_daily_mix() computes
// it locally.
// ---------------------------------------------------------------------

enum class SpotifyHomeSection {
    DailyMix,
    JumpBackIn,
    TopTracks,
    TopArtists,
    LikedSongs,
    Playlists,
    SavedAlbums,
    Search,
};

// One row of the home menu. `detail` is the right-hand column; it starts
// out as a static hint ("recently played") and is replaced with a real
// count once that section has actually been fetched, so the menu doesn't
// claim numbers it hasn't loaded yet.
struct SpotifyHomeRow {
    SpotifyHomeSection section;
    std::string label;
    std::string detail;
};

// The home menu in display order: Daily Mix first because it's the "just
// press enter" entry, Search last because it has its own hotkey.
std::vector<SpotifyHomeRow> spotify_home_rows();

// Human-readable heading for a section, used as the list panel's label
// after drilling into it.
const char* spotify_section_name(SpotifyHomeSection s);

// ---------------------------------------------------------------------
// Daily Mix
// ---------------------------------------------------------------------

// Today's date in local time as YYYYMMDD -- the mix's seed. One number
// per local day means the same shuffle all day and a new one tomorrow,
// with no state to persist. Local rather than UTC so the rollover
// happens at the user's own midnight.
int spotify_daily_seed();

// One input to the mix, and how much of it to fill. `weight` is relative
// (quota is weight/sum(weights) of `target`), so a source can be added
// or dropped without rebalancing the others.
struct DailyMixSource {
    std::vector<SpotifyTrack> tracks;
    int weight = 1;
};

// Picks `target` tracks out of `sources`, deterministically for a given
// `seed`.
//
// Sources are weighted rather than merged flat because each one alone
// makes a poor mix: recent favourites barely change day to day, all-time
// favourites ignore the present, recently-played just replays the last
// session. The source list is generic so the caller decides which
// endpoints feed the mix -- Liked Songs stays out of the default set,
// for instance, because reading it pages the whole library.
//
// A source that comes back short is backfilled from the others, so an
// account with no listening history still gets a full mix. Duplicates
// are removed by uri across all sources.
std::vector<SpotifyTrack> build_daily_mix(const std::vector<DailyMixSource>& sources,
                                          int seed,
                                          std::size_t target = 30);

} // namespace muisc
