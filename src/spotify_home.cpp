#include "spotify_home.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <random>
#include <unordered_set>

namespace muisc {

std::vector<SpotifyHomeRow> spotify_home_rows() {
    return {
        {SpotifyHomeSection::DailyMix,   "Daily Mix",        "30 tracks"},
        {SpotifyHomeSection::JumpBackIn, "Jump back in",     "recently played"},
        {SpotifyHomeSection::TopTracks,  "Your top tracks",  "4 weeks"},
        {SpotifyHomeSection::TopArtists, "Your top artists", "4 weeks"},
        {SpotifyHomeSection::LikedSongs, "Liked Songs",      ""},
        {SpotifyHomeSection::Playlists,  "Your playlists",   ""},
        {SpotifyHomeSection::SavedAlbums,"Saved albums",     ""},
        {SpotifyHomeSection::Search,     "Search Spotify",   "/"},
    };
}

const char* spotify_section_name(SpotifyHomeSection s) {
    switch (s) {
        case SpotifyHomeSection::DailyMix:    return "DAILY MIX";
        case SpotifyHomeSection::JumpBackIn:  return "JUMP BACK IN";
        case SpotifyHomeSection::TopTracks:   return "YOUR TOP TRACKS";
        case SpotifyHomeSection::TopArtists:  return "YOUR TOP ARTISTS";
        case SpotifyHomeSection::LikedSongs:  return "LIKED SONGS";
        case SpotifyHomeSection::Playlists:   return "YOUR PLAYLISTS";
        case SpotifyHomeSection::SavedAlbums: return "SAVED ALBUMS";
        case SpotifyHomeSection::Search:      return "SPOTIFY SEARCH";
    }
    return "SPOTIFY";
}

int spotify_daily_seed() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    // tm_year is years since 1900, tm_mon is 0-based.
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

namespace {

// Appends up to `quota` not-yet-seen tracks from `src` into `out`,
// starting at `cursor` and advancing it, so a later backfill pass
// continues where the first pass stopped instead of rescanning the
// tracks it already rejected.
void take_from(const std::vector<SpotifyTrack>& src, std::size_t& cursor, std::size_t quota,
               std::unordered_set<std::string>& seen, std::vector<SpotifyTrack>& out) {
    std::size_t taken = 0;
    while (cursor < src.size() && taken < quota) {
        const SpotifyTrack& t = src[cursor++];
        if (t.uri.empty()) continue;
        if (!seen.insert(t.uri).second) continue; // already in the mix from another source
        out.push_back(t);
        ++taken;
    }
}

} // namespace

std::vector<SpotifyTrack> build_daily_mix(const std::vector<DailyMixSource>& sources,
                                          int seed,
                                          std::size_t target) {
    std::vector<SpotifyTrack> out;
    if (target == 0 || sources.empty()) return out;
    out.reserve(target);

    // One generator for the whole construction, so the result is a pure
    // function of (seed, inputs). mt19937 rather than rand(), whose
    // sequence is implementation-defined: the same seed has to give the
    // same mix across restarts and across libcs.
    std::mt19937 rng(static_cast<std::uint_fast32_t>(seed));

    // Shuffle each source before slicing: the sources arrive in a stable
    // order (top tracks ranked, recently played chronological), so taking
    // the first N would ignore the seed entirely.
    std::vector<std::vector<SpotifyTrack>> pools;
    pools.reserve(sources.size());
    int weight_sum = 0;
    for (const auto& src : sources) {
        pools.push_back(src.tracks);
        std::shuffle(pools.back().begin(), pools.back().end(), rng);
        weight_sum += std::max(0, src.weight);
    }
    if (weight_sum <= 0) weight_sum = static_cast<int>(sources.size()); // all weights zero -> share equally

    std::unordered_set<std::string> seen;
    seen.reserve(target * 2);
    // Per-pool read cursors; see take_unseen().
    std::vector<std::size_t> cursors(pools.size(), 0);

    for (std::size_t i = 0; i < pools.size() && out.size() < target; ++i) {
        const int w = std::max(0, sources[i].weight);
        const std::size_t quota = target * static_cast<std::size_t>(w) / static_cast<std::size_t>(weight_sum);
        take_from(pools[i], cursors[i], quota, seen, out);
    }

    // Backfill. The quota pass leaves the mix short when integer
    // division drops a remainder, or when a source has fewer tracks than
    // its quota (the normal case for a new account). Each pass asks for
    // exactly the shortfall, so neither the target nor a pool's
    // remaining tracks can be overshot.
    for (std::size_t i = 0; i < pools.size() && out.size() < target; ++i) {
        take_from(pools[i], cursors[i], target - out.size(), seen, out);
    }

    // Final shuffle, so the mix isn't audibly one source's block after
    // another's.
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

} // namespace muisc
