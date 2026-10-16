#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// One LRC line, optionally with per-word timestamps (enhanced/A2 LRC),
// which is what makes word-level highlighting possible during playback.
struct LyricLine {
    double start_time = 0.0;
    std::string full_text;
    std::vector<std::pair<double, std::string>> words; // empty if not word-synced
};

enum class LyricsStatus {
    Ok,
    NotFound,        // a provider answered: this track has no lyrics
    Error,           // nobody answered (server busy, offline) -- worth retrying
};

struct LyricsResult {
    LyricsStatus status = LyricsStatus::Error;
    std::vector<LyricLine> lines;
    std::string message;   // human-readable status/error, shown in the lyrics panel
    std::string source;    // "local" | "better-lyrics" | "lrclib" | ""
    std::string raw_lrc;   // the raw LRC text, kept so it can be cached to a sidecar file
};

// Priority chain: a local sidecar .lrc file next to `track_path` (checked
// first, no network at all) -> Better Lyrics (word-level, via TTML,
// converted to enhanced LRC) -> KuGou (word-level, via KRC) -> LRCLIB's
// exact lookup -> LRCLIB's fuzzy search. All of them are called directly
// over HTTP (libcurl, see http_client.h) -- no external process, no
// Python. Whatever comes back from a network fetch is written back to the
// sidecar file, so the next time this track plays (even offline) it's a
// local-file hit.
//
// The lookups are tried against more than one spelling of the track,
// because the exact-match providers index one primary artist and a title
// without the parenthesised half, while Spotify hands over every
// credited artist and the full title.
//
// Retries matter here: LRCLIB answers 503 ServerOverloaded for a good
// share of requests when it is busy, and reporting that as NotFound is
// what made lyrics come and go for tracks that have them. Each request
// is retried, and a chain where nobody ever actually answered comes back
// as Error (which App retries again, further apart) rather than as an
// absence.
LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const fs::path& track_path = fs::path(),
                                  bool force_network = false);

} // namespace muisc
