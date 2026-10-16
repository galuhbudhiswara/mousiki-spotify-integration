#include "app.h"

#include "spotify_auth.h"
#include "console_log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

// Standard edit distance (single-char insert/delete/substitute cost 1).
// Small strings only (track titles/artists/search words) — the O(n*m)
// DP table is negligible at this scale.
int levenshtein(const std::string& a, const std::string& b) {
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
        }
    }
    return dp[n][m];
}

// Typo-tolerant match score: higher is better, negative means "not a
// match at all" (excluded from results). An exact literal substring
// match always outranks every fuzzy match, regardless of how good the
// fuzzy quality is — so "another love" typed exactly always sits above
// a merely-close fuzzy hit. Within the fuzzy tier, each query word must
// find a reasonably close word somewhere in the target (a query word
// that's wildly different from everything in the target means this
// isn't really a match, even if some OTHER query word happens to fit)
// — this is what lets "anogher lobe" (typo'd "another love") still find
// the track: word-level edit distance tolerates the substituted
// characters that a plain substring or subsequence check would miss
// entirely (neither "anogher" nor "lobe" appears anywhere in "another
// love" as literal text).
double fuzzy_score(const std::string& query, const std::string& target) {
    std::string q = lower(query), t = lower(target);
    if (q.empty()) return 0.0;

    size_t pos = t.find(q);
    if (pos != std::string::npos) {
        return 1000.0 - std::min<double>(static_cast<double>(pos), 900.0); // tier 1: exact substring, earlier position ranks higher
    }

    auto q_words = split_words(q);
    auto t_words = split_words(t);
    if (q_words.empty() || t_words.empty()) return -1.0;

    double total_quality = 0.0;
    for (auto& qw : q_words) {
        double best_quality = -1.0;
        for (auto& tw : t_words) {
            size_t max_len = std::max(qw.size(), tw.size());
            if (max_len == 0) continue;
            double normalized = static_cast<double>(levenshtein(qw, tw)) / static_cast<double>(max_len);
            double quality = 1.0 - normalized;
            if (quality > best_quality) best_quality = quality;
        }
        if (best_quality < 0.55) return -1.0; // this query word doesn't fit anywhere close enough -- not a real match
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(q_words.size())) * 100.0; // tier 2: fuzzy, always below tier 1's range
}

std::string fmt_mmss(double seconds) {
    if (seconds < 0) return "--:--";
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss.width(2); oss.fill('0'); oss << (total / 60) << ":";
    oss.width(2); oss.fill('0'); oss << (total % 60);
    return oss.str();
}

// --- shared box-drawing helpers ------------------------------------------
// Every panel is built against `total_width` (the FULL visual width of the
// box, borders included) so two boxes placed side by side on the same row
// always sum to exactly the width the caller asked for, and single-box
// rows line up with everything above/below them. Content is always
// total_width-4 (for the "│ X │" pattern), padded with the codepoint-safe
// helpers from terminal_ui.cpp so multi-byte glyphs can't throw off the
// column count the way byte-length padding did before.

} // namespace

std::string App::box_top(const std::string& label, int total_width, const std::string& border_ansi) const {
    std::string lbl = label.empty() ? "" : (" " + label + " ");
    std::string prefix = settings_.box_upper_left + settings_.box_horizontal + lbl;
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_upper_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_bottom(int total_width, const std::string& footer, const std::string& border_ansi) const {
    std::string prefix = footer.empty() ? (settings_.box_lower_left + settings_.box_horizontal) : (settings_.box_lower_left + settings_.box_horizontal + " " + footer + " ");
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_lower_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_line(const std::string& content, int total_width, const std::string& border_ansi) const {
    int inner = std::max(0, total_width - 4);
    std::string padded = pad_right(truncate_str(content, inner), inner);
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

namespace {

// Center-aligns plain (no-ANSI) text within `width` visual columns.
std::string center_pad(const std::string& text, int width) {
    std::string t = truncate_str(text, width);
    int pad = std::max(0, width - display_width(t));
    int left = pad / 2;
    int right = pad - left;
    return std::string(left, ' ') + t + std::string(right, ' ');
}

// Visualizer bars use ONLY this glyph set (per explicit instruction) —
// distinct from WaveformQuantizer's 6-level top/mid/bot triples used by
// the progress bar.
const char* fft_glyph(int level) {
    switch (std::clamp(level, 0, 4)) {
        case 0: return " ";
        case 1: return "\u28C0"; // ⣀
        case 2: return "\u28E4"; // ⣤
        case 3: return "\u28F6"; // ⣶
        default: return "\u28FF"; // ⣿
    }
}

// Word-wraps one lyric line to `width` visual columns (breaking on word
// boundaries, never mid-word), center-aligning each resulting row, and
// bakes in ANSI highlighting for words already "sung" (their timestamp
// <= elapsed) when this is the active line. Width/centering math is done
// on PLAIN text first — color codes are spliced in afterwards, since they
// don't occupy display columns but would otherwise confuse the
// codepoint-counting padding helpers.
// Counts UTF-8 codepoints (not bytes) -- used for the letter-by-letter
// lyrics reveal below. terminal_ui.cpp has an equivalent utf8_take(), but
// it's `static` (file-local) so it isn't reachable from here.


std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width,
                                                     bool is_active, const Settings& settings) {
    struct W { std::string text; double t; bool has_ts; };
    std::vector<W> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) words.push_back({wt.second, wt.first, true});
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({w, line.start_time, false});
    }
    if (words.empty() || width <= 0) return {std::string(std::max(0, width), ' ')};

    std::vector<std::vector<W>> rows;
    std::vector<W> cur;
    int cur_len = 0;
    for (auto& w : words) {
        std::string remain = w.text;
        while (!remain.empty()) {
            int r_wlen = display_width(remain);
            int available = cur.empty() ? width : (width - cur_len - 1);
            
            if (r_wlen <= available) {
                cur.push_back({remain, w.t, w.has_ts});
                cur_len += (cur.empty() ? r_wlen : 1 + r_wlen);
                break;
            }
            
            if (!cur.empty()) {
                rows.push_back(cur);
                cur.clear();
                cur_len = 0;
                continue; // retry fitting on a new line
            }
            
            // The word exceeds the full width of a line, must split it.
            std::string chunk = utf8_take(remain, width);
            if (chunk.empty()) {
                // Failsafe: width is too small (e.g., 1) to fit a wide character (width 2).
                // Force-take 2 columns so we at least make progress (1 grapheme cluster).
                chunk = utf8_take(remain, 2);
            }
            
            cur.push_back({chunk, w.t, w.has_ts});
            rows.push_back(cur);
            cur.clear();
            cur_len = 0;
            remain = remain.substr(chunk.size());
        }
    }
    if (!cur.empty()) rows.push_back(cur);

    std::vector<std::string> out;
    for (auto& row : rows) {
        std::vector<std::string> mapped_words;
        int plain_len = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            std::string mapped = apply_font_map(row[i].text, settings.font_map);
            mapped_words.push_back(mapped);
            plain_len += display_width(mapped);
            if (i > 0) plain_len += 1;
        }
        int total_pad = std::max(0, width - plain_len);
        int left_pad, right_pad;
        if (settings.lyrics_alignment == 1) { left_pad = 0; right_pad = total_pad; }           // left
        else if (settings.lyrics_alignment == 2) { left_pad = total_pad; right_pad = 0; }       // right
        else { left_pad = total_pad / 2; right_pad = total_pad - left_pad; }                    // center (default)

        // Word-level karaoke highlight: find the word currently being sung
        // (the last word whose timestamp has passed but the *next* one
        // hasn't) and render just that one underlined on top of the
        // normal active-word color -- gives the highlight a moving
        // "leading edge" instead of every already-sung word looking
        // identical. (A continuous per-frame pulse used to live here too,
        // applied to whole line-synced-only lines with no way to turn it
        // off -- removed; that wasn't requested and had no toggle.)
        int currently_singing = -1;
        for (size_t i = 0; i < row.size(); ++i) {
            if (is_active && row[i].has_ts && row[i].t <= elapsed) currently_singing = static_cast<int>(i);
        }
        const char* kUnderline = "\x1b[4m";

        std::string s(left_pad, ' ');
        std::string word_ansi = ansi_for(settings.active_word_color) + bg_ansi_for(settings.active_word_bg_color);
        std::string active_line_ansi = ansi_for(settings.active_line_color) + bg_ansi_for(settings.active_line_bg_color);
        std::string inactive_ansi = ansi_for(settings.inactive_line_color) + bg_ansi_for(settings.inactive_line_bg_color);
        // Word-by-word / letter-by-letter modes hide not-yet-sung words in
        // the active line entirely (blanked to spaces, same width, so the
        // alignment doesn't jump around) instead of showing them in the
        // active-line color right away -- that's the actual "progressive
        // reveal" that was asked for, as opposed to the old always-fully-
        // visible line with just a moving color highlight.
        bool progressive = is_active && (settings.lyrics_animation == 1 || settings.lyrics_animation == 2);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) s += ' ';
            bool sung = is_active && row[i].has_ts && row[i].t <= elapsed;
            bool no_word_ts_but_active = is_active && !row[i].has_ts;
            bool is_current = sung && static_cast<int>(i) == currently_singing;
            if (is_current && settings.lyrics_animation == 2) {
                // Letter-by-letter: interpolate how many characters of the
                // CURRENT word are revealed so far. Prefer the next word's
                // timestamp (within this wrapped row) as the end bound; if
                // this is the last word in the row (no next timestamp to
                // interpolate toward), fall back to the previous word's
                // pace instead of just popping the whole word in at once --
                // that "spawns out of nowhere" look was the bug. With no
                // timing context at all (a single-word line), use a
                // reasonable fixed pace.
                std::string full = mapped_words[i];
                int nchars = display_width(full);
                double dur = -1.0;
                if (i + 1 < row.size() && row[i + 1].has_ts && row[i + 1].t > row[i].t) {
                    dur = row[i + 1].t - row[i].t;
                } else if (i > 0 && row[i - 1].has_ts && row[i].t > row[i - 1].t) {
                    dur = row[i].t - row[i - 1].t;
                } else {
                    dur = 0.4;
                }
                double frac = std::clamp((elapsed - row[i].t) / dur, 0.0, 1.0);
                int revealed = static_cast<int>(frac * nchars);
                std::string shown = utf8_take(full, revealed);
                int hidden_cols = display_width(full) - display_width(shown);
                s += word_ansi + kUnderline + shown + "\x1b[0m" + std::string(std::max(0, hidden_cols), ' ');
            } else if (is_current) {
                s += word_ansi + kUnderline + mapped_words[i] + "\x1b[0m";
            } else if (sung) {
                s += word_ansi + mapped_words[i] + "\x1b[0m";
            } else if (progressive && row[i].has_ts) {
                s += std::string(display_width(mapped_words[i]), ' '); // not sung yet -- hidden, not just dimmed
            } else if (no_word_ts_but_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else if (is_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else {
                s += inactive_ansi + mapped_words[i] + "\x1b[0m";
            }
        }
        s += std::string(right_pad, ' ');
        out.push_back(s);
    }
    return out;
}

} // namespace

App::App() {
    settings_ = load_settings();

    // Inject the cache directory into local music paths so streamed songs
    // automatically appear in the local view for seamless offline playback.
    // save_settings() persists local_music_paths back to config.txt, so
    // an unconditional push_back() here duplicated the cache dir a little
    // more every single session (load already-saved copy -> push another
    // -> save both -> next launch loads two, pushes a third, ...). Only
    // add it if it isn't already there.
    std::string cache_dir_str = cache_.cache_dir().string();
    bool cache_dir_present = std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(),
                                        cache_dir_str) != settings_.local_music_paths.end();
    if (!cache_dir_present) settings_.local_music_paths.push_back(cache_dir_str);

    all_local_tracks_ = local_source_.scan(settings_.local_music_paths);
    local_view_ = all_local_tracks_;
    launch_row_meta_resolver();
}


int App::hotkey_string_to_key(const std::string& s) {
    if (s == "ARROW_KEY_UP") return 'A';
    if (s == "ARROW_KEY_DOWN") return 'B';
    if (s == "ARROW_KEY_RIGHT") return 'C';
    if (s == "ARROW_KEY_LEFT") return 'D';
    if (s == "ENTER") return '\n';
    if (s == "TAB") return 9;
    if (s == "SPACE") return ' ';
    if (s == "ESC") return 27;
    if (s == "BACKSPACE") return 127;
    if (s.size() == 1) return static_cast<int>(s[0]);
    return 0;
}

std::string App::resolve_hotkey_action(int key) const {
    for (const auto& [action, key_str] : settings_.hotkeys) {
        if (hotkey_string_to_key(key_str) == key) return action;
    }
    return "";
}

std::string App::hotkey_conflict(const std::string& key_str, const std::string& except_action) const {
    if (key_str.empty()) return "";
    for (const auto& [action, val] : settings_.hotkeys) {
        if (action == except_action) continue;
        if (val == key_str) return action;
    }
    return "";
}

// Pushes a message into both the one-line status area (existing
// behavior) and the persistent console log (both the in-memory buffer
// the Console overlay reads and, via ConsoleLog, the on-disk
// console.log) -- so events are still visible after they've scrolled
// off the status line, after a terminal-session switch redraw wiped
// the screen, or after the session itself has ended.
void App::log_event(const std::string& msg) {
    if (settings_.show_status_messages) status_line_ = msg;
    ConsoleLog::instance().log_basic(msg);
}

// ---------------------------------------------------------------------
// Search / list state
// ---------------------------------------------------------------------

// Shared by refresh_local_view() (final, committed query) and the live
// incremental-search preview (whatever's currently typed, before Enter)
// so both behave identically -- what you see while typing is exactly
// what you'll get if you confirm it.
//
// An empty query returns the library sorted per local_sort_mode_ (see
// apply_local_sort()). A real query filters to fuzzy-matching tracks
// only and sorts PURELY by match quality, highest first -- this
// deliberately throws away the sort order entirely rather than using it
// as a tiebreak, so the best match always sits at the top regardless of
// where it happened to fall alphabetically/by-folder.
//
// Matches against both title AND artist -- using the real probed artist
// tag (row_meta_cache_) where it's already been resolved for that row,
// falling back to the cheap parent-folder guess otherwise. This is why
// "search by artist name" gets more accurate the more of the library
// you've scrolled past (each visible row lazily resolves its real tag).
std::vector<LocalTrack> App::filter_and_rank_local(const std::string& query) const {
    if (query.empty()) {
        auto result = all_local_tracks_;
        apply_local_sort(result);
        return result;
    }

    std::vector<std::pair<double, const LocalTrack*>> scored;
    scored.reserve(all_local_tracks_.size());
    for (const auto& t : all_local_tracks_) {
        std::string artist = t.folder_artist;
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            auto it = row_meta_cache_.find(t.path.string());
            if (it != row_meta_cache_.end() && !it->second.artist.empty()) artist = it->second.artist;
        }
        double title_score = fuzzy_score(query, t.title);
        double artist_score = fuzzy_score(query, artist);
        double score = std::max(title_score, artist_score);
        if (score > 0.0) scored.emplace_back(score, &t);
    }
    std::stable_sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<LocalTrack> result;
    result.reserve(scored.size());
    for (auto& [score, t] : scored) result.push_back(*t);
    return result;
}

const char* App::sort_mode_name(int mode) {
    switch (mode) {
        case 1: return "title A-Z";
        case 2: return "artist A-Z";
        default: return "folder order";
    }
}

// Applied only when browsing with no active search query -- a fuzzy
// search's relevance ranking always wins over the manual sort mode.
// Duration isn't a sort option (yet): most rows only get a real duration
// once they've been lazily probed for display, so sorting by it up front
// would show mostly-unprobed rows in an arbitrary order until the
// background resolver catches up.
void App::apply_local_sort(std::vector<LocalTrack>& tracks) const {
    if (local_sort_mode_ == 1) {
        std::stable_sort(tracks.begin(), tracks.end(), [](const LocalTrack& a, const LocalTrack& b) {
            return lower(a.title) < lower(b.title);
        });
    } else if (local_sort_mode_ == 2) {
        std::stable_sort(tracks.begin(), tracks.end(), [this](const LocalTrack& a, const LocalTrack& b) {
            auto artist_of = [this](const LocalTrack& t) {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(t.path.string());
                return (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
            };
            return lower(artist_of(a)) < lower(artist_of(b));
        });
    }
    // mode 0: leave as scanned (folder order) -- no-op
}

void App::refresh_local_view() {
    local_view_ = filter_and_rank_local(last_local_query_);
    // Folder filter (HKeyFilterForFolder) stacks on top of the search/sort
    // result rather than replacing it, so filtering-by-folder while a
    // search is active narrows to just that folder's matches.
    if (!folder_filter_.empty()) {
        std::vector<LocalTrack> filtered;
        filtered.reserve(local_view_.size());
        for (auto& t : local_view_) {
            if (t.path.parent_path().string() == folder_filter_) filtered.push_back(t);
        }
        local_view_ = std::move(filtered);
    }
    selected_ = 0;
    scroll_ = 0;
}

// Called on every keystroke while typing in the search box, before
// Enter is pressed — this is the "incremental search" behavior: the
// list updates live as you type instead of only after confirming. Local
// queries get filtered+ranked immediately via the same fuzzy logic
// submit_search() will commit on Enter. An "s:" (online search) prefix
// is left alone here — firing a network request on every keystroke
// would be wasteful and slow, so online search still only fires on
// Enter — but the view is reset back to whatever it was before '/' was
// pressed so a stale local preview doesn't linger behind the search box
// while an online query is being typed.
void App::update_live_search_preview() {
    std::string buf = search_buffer_;
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    // Both the YouTube ("s:") and Spotify ("sp:") prefixes mean "this
    // isn't a local query" -- leave the view as it was rather than
    // filtering the local library by the literal prefix text.
    const std::string sp_pfx = kSpotifySearchPrefix;
    const bool remote_prefix =
        (buf.size() >= 2 && lower(buf.substr(0, 2)) == "s:") ||
        (buf.size() >= sp_pfx.size() && lower(buf.substr(0, sp_pfx.size())) == sp_pfx) ||
        // While "sp:" is still being typed ("s", "sp") it is already not
        // a local query -- filtering on the half-typed prefix would make
        // the list flicker through unrelated matches on every keystroke.
        (list_source_ == ListSource::Spotify && !buf.empty() && buf.size() <= sp_pfx.size() &&
         lower(sp_pfx).compare(0, buf.size(), lower(buf)) == 0);
    if (remote_prefix) {
        if (list_source_ != ListSource::Spotify) {
            list_source_ = pre_search_list_source_;
            local_view_ = filter_and_rank_local(pre_search_local_query_);
            selected_ = 0;
            scroll_ = 0;
        }
        return;
    }

    list_source_ = ListSource::Local;
    local_view_ = filter_and_rank_local(buf);
    selected_ = 0;
    scroll_ = 0;
}

void App::submit_search() {
    std::string buf = search_buffer_;
    // trim
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    // Checked before the YouTube "s:" prefix purely for readability --
    // "sp:" can't collide with it anyway ('p' is not ':').
    {
        const std::string pfx = kSpotifySearchPrefix;
        if (buf.size() >= pfx.size() && lower(buf.substr(0, pfx.size())) == pfx) {
            std::string query = buf.substr(pfx.size());
            while (!query.empty() && query.front() == ' ') query.erase(query.begin());
            if (query.empty()) {
                status_line_ = "type something after /sp: to search spotify";
                return;
            }
            launch_spotify_search(query);
            return;
        }
    }

    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "s:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_online_query_ = query;
        list_source_ = ListSource::Online;
        if (search_in_progress_.load()) {
            status_line_ = "still searching, hang on ...";
            return;
        }
        launch_search_async(query.empty() ? "music" : query);
    } else {
        last_local_query_ = buf;
        list_source_ = ListSource::Local;
        refresh_local_view();
    }
}

// ---------------------------------------------------------------------
// Playback start
// ---------------------------------------------------------------------

void App::write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                 double t_probe, double t_total, const std::string& error) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    fs::path dir = fs::path(home) / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream log(dir / "load_timing.log", std::ios::app);
    if (!log.is_open()) return;

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log << timebuf << " track=\"" << title << "\" source=" << (is_local ? "local" : "online");
    if (!is_local) log << " resolve=" << t_resolve << "s";
    // This is now "time to first sound", not "time to fully decoded" —
    // decode itself streams in after this point, off the critical path.
    log << " probe=" << t_probe << "s time_to_playback=" << t_total << "s";
    if (!error.empty()) log << " ERROR=\"" << error << "\"";
    log << "\n";
}

void App::launch_load_async(fs::path local_path, std::string title, std::string artist,
                             std::string location_label, bool is_local, std::string video_id) {
    if (load_thread_.joinable()) load_thread_.join(); // previous job already signaled done, safe to reap
    load_in_progress_ = true;
    load_ready_ = false;
    load_stage_ = is_local ? 4 : 1;
    load_started_at_ = std::chrono::steady_clock::now();
    if (!is_local) status_line_ = "resolving \"" + title + "\" ...";

    // This thread ONLY resolves (online) and probes metadata/duration —
    // both fast, no full decode. It publishes a result and returns. Full
    // decode is a SEPARATE, detached thread spawned at the bottom, so
    // this thread (the one the main loop's next launch_load_async call
    // will join()) is never blocked waiting on decode — that's what
    // makes it safe to join from launch_load_async without risking a
    // freeze if the user switches tracks again quickly.
    load_thread_ = std::thread([this, local_path, title, artist, location_label, is_local, video_id]() {
        using clock = std::chrono::steady_clock;
        auto t_start = clock::now();
        auto elapsed_s = [](clock::time_point from) {
            return std::chrono::duration<double>(clock::now() - from).count();
        };

        PendingLoad pl;
        pl.title = title;
        pl.artist = artist;
        pl.location_label = location_label;
        pl.is_local = is_local;
        pl.video_id = video_id;

        double t_resolve = 0.0, t_probe = 0.0;

        fs::path path = local_path;
        if (!is_local) {
            load_stage_ = 1;
            auto t0 = clock::now();
            std::string err;
            auto resolved = youtube_.resolve_by_id(video_id, title, artist, &err);
            t_resolve = elapsed_s(t0);
            if (!resolved) {
                pl.error = "download failed: " + err;
                write_load_timing_log(title, is_local, t_resolve, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
            path = resolved->cached_path;
            pl.title = resolved->title;
            pl.artist = resolved->artist;
        }
        pl.path = path;

        load_stage_ = 4;
        auto t2 = clock::now();
        pl.metadata = probe_metadata(path, pl.title, pl.artist, pl.location_label);
        
        // Ensure lyrics fetch uses the real metadata tags instead of the filename/folder
        if (is_local) {
            if (!pl.metadata.name.empty() && pl.metadata.name != "-") {
                pl.title = pl.metadata.name;
            }
            if (!pl.metadata.artist.empty() && pl.metadata.artist != "-") {
                pl.artist = pl.metadata.artist;
            }
        }
        
        double duration = probe_duration_seconds(path);
        t_probe = elapsed_s(t2);
        pl.total_sec = duration > 0 ? static_cast<size_t>(duration) : 0;

        pl.pcm = std::make_shared<StreamingPcm>();
        pl.pcm->reserve_for_seconds(duration > 0 ? duration : 300.0, 44100);
        pl.success = true;

        write_load_timing_log(pl.title, is_local, t_resolve, t_probe, elapsed_s(t_start), "");

        {
            std::lock_guard<std::mutex> lk(load_mutex_);
            pending_load_ = pl;
            load_ready_ = true;
        }

        // Decode continues independently from here — detached because it
        // may still be running when the user switches to a different
        // track, and the StreamingPcm it's filling stays alive via the
        // shared_ptr captured below (and via Player's own reference, if
        // this track is still the one playing) for exactly as long as it
        // needs to. Known tradeoff: if the app quits while a decode is
        // still in flight, that ffmpeg subprocess can be orphaned rather
        // than cleanly killed — worth fixing with real process-group
        // tracking later, not a correctness or crash risk today.
        std::shared_ptr<StreamingPcm> pcm = pl.pcm;
        fs::path decode_path = path;
        std::string wtitle = pl.title, wartist = pl.artist;
        bool waveform_smooth = settings_.waveform_smooth; // captured by value — see below, avoids a cross-thread read of settings_
        std::thread([this, decode_path, pcm, waveform_smooth]() {
            stream_decode_ffmpeg(decode_path, *pcm);

            // Deferred mini-waveform pass — only starts once decode is
            // fully done, never gates playback.
            if (!pcm->decode_failed.load()) {
                // PERF: decode is done — pcm->data is no longer being
                // written to, so we pass it directly as a const-ref
                // instead of making a full snapshot copy.  A 5-minute
                // track at 44100 Hz is ~50 MB; that copy was the single
                // biggest reason the waveform appeared so late after
                // playback started, because it doubled the working-set
                // size and stalled the RMS pass behind a large memcpy.
                auto envelope = WaveformQuantizer::generate_high_res_envelope(pcm->data, 4096, waveform_smooth);
                std::lock_guard<std::mutex> lk(waveform_mutex_);
                pending_waveform_envelope_ = std::move(envelope);
                waveform_pending_ready_ = true;
            }
        }).detach();
    });
}

// Used to block here waiting for the previous track's fetch thread to
// finish (join()) before starting a new one. That's a real main-thread
// stall: whenever lyrics aren't quickly available (still mid network-call
// chain) and the user skips to another track before it resolves,
// switching songs would hang until the abandoned fetch finished.
// A busy provider is retried from here, spaced out: fetch_synced_lyrics()
// already retries each request a few hundred milliseconds apart, so
// these are the slower "come back to it" attempts.
static constexpr int kLyricsBusyRetries = 3;
static constexpr int kLyricsBusyRetryMs = 5000;

// Detaching instead, with an epoch guard so a late-arriving stale result
// just gets discarded rather than clobbering the new/retried track's
// lyrics. Shared by the initial per-track fetch (poll_pending_load) and
// the manual retry hotkey (handle_key's 'l' case).
void App::launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network) {
    lyrics_ready_ = false;
    int my_epoch = ++lyrics_epoch_;
    std::thread([this, title, artist, path, force_network, my_epoch]() {
        LyricsResult r = fetch_synced_lyrics(title, artist, path, force_network);

        // LyricsStatus::Error means no provider ever actually answered --
        // LRCLIB is busy (it serves 503 ServerOverloaded a good fraction
        // of the time), the connection dropped. That is not this track
        // being lyric-less, and the same request a few seconds later
        // usually lands, so come back to it rather than leaving the panel
        // claiming there are none. NotFound gets no retry: that is a real
        // answer. Slept in slices so changing track ends it immediately.
        for (int attempt = 0; r.status == LyricsStatus::Error && attempt < kLyricsBusyRetries; ++attempt) {
            for (int slept = 0; slept < kLyricsBusyRetryMs; slept += 100) {
                if (my_epoch != lyrics_epoch_.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            r = fetch_synced_lyrics(title, artist, path, force_network);
        }
        if (r.status == LyricsStatus::Error) {
            r.message = "lyrics server busy -- press l to try again";
        }

        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (my_epoch != lyrics_epoch_.load()) return; // a newer/retried fetch has since started — discard
        lyrics_result_ = std::move(r);
        lyrics_ready_ = true;
    }).detach();
}

void App::poll_pending_load() {
    if (!load_ready_.load()) return;
    PendingLoad pl;
    {
        std::lock_guard<std::mutex> lk(load_mutex_);
        // The flag is cleared while still holding load_mutex_, so the
        // load thread cannot race-write pending_load_ in the window
        // between the copy and the reset.
        if (!load_ready_.load()) return; // re-check under lock (spurious wakeup guard)
        pl = pending_load_;
        load_ready_ = false;
    }
    load_in_progress_ = false;
    load_stage_ = 0;

    if (!pl.success) {
        status_line_ = pl.error;
        return;
    }

    // Player::play() stops whatever it was previously playing as its own
    // first step, so no separate explicit stop() call is needed here —
    // and doing it inside play() (below, off the main thread) is what
    // lets this whole switch never touch the main thread.
    current_pcm_ = pl.pcm;
    total_sec_ = pl.total_sec;
    metadata_ = pl.metadata;
    current_path_ = pl.path;
    current_is_local_ = pl.is_local;
    current_video_id_ = pl.video_id;
    has_track_ = true;
    // A local file is in the speakers now, so a Spotify cover left
    // spinning would be advertising the wrong song.
    set_disk_album_art("", "");
    player_.clear_finished(); // see clear_finished()'s comment — closes the race that caused the double-skip bug
    waveform_envelope_.clear();
    waveform_ready_ = false;
    waveform_pending_ready_ = false;
    ++waveform_epoch_; // invalidate any in-flight waveform from the previous track
    last_lyrics_status_.clear();
    fft_.reset(); // don't let the previous track's spectrum tail linger into this one's first frame

    launch_lyrics_fetch(pl.title, pl.artist, pl.path);

    // This is the whole point of the redesign: play() is handed a
    // StreamingPcm that may have zero frames decoded yet. The audio
    // callback plays silence for anything past what's been decoded and
    // self-corrects the instant more arrives — so sound starts the
    // moment decode produces its first chunk, not after the whole track.
    // Dispatched off the main thread — see launch_device_play_async().
    launch_device_play_async();
    status_line_.clear();
}

void App::launch_device_play_async() {
    // Never join here — that would risk blocking whichever thread calls
    // this (poll_pending_load / advance_track, both on the main thread)
    // on however long the OLD device op takes to finish. Detach it: the
    // old attempt just finishes on its own (Player::play() stops the
    // previous device as its first step anyway, so an old in-flight
    // play() call safely becomes a no-op-ish teardown once it gets to
    // run, even if a newer one has already taken over by then).
    //
    // Without the generation counter a detached old thread can call
    // ma_device_start() on a device the new thread already tore down
    // inside play() → stop() — undefined behaviour, and audible as
    // glitches on fast track-switching. The counter lets the old thread
    // notice it has been superseded and bail out before touching the
    // device.
    if (device_thread_.joinable()) device_thread_.detach();
    int my_gen = ++device_gen_;
    auto pcm = current_pcm_;
    int vol = player_.volume() > 0 ? player_.volume() : 70;
    // One-shot resume position from a restored snapshot -- consumed
    // here exactly once, then zeroed so every subsequent track change
    // (skip, search-and-play, queue advance, ...) starts at 0 like
    // always. Reading+clearing it up front (still on the main thread,
    // before the lambda captures it by value) avoids any race with a
    // second restore attempt -- there isn't one, but this keeps that
    // invariant obvious rather than implicit.
    double start_sec = resume_start_sec_;
    resume_start_sec_ = 0.0;
    // Same one-shot read-and-clear as start_sec: only the track this
    // call was launched for comes up paused.
    bool start_paused = start_paused_;
    start_paused_ = false;
    device_thread_ = std::thread([this, pcm, vol, my_gen, start_sec, start_paused]() {
        std::lock_guard<std::mutex> lk(device_mutex_);
        if (my_gen != device_gen_.load()) return; // superseded — a newer play request won
        player_.play(pcm, start_sec, vol, &fft_, start_paused);
    });
}

void App::poll_pending_waveform() {
    if (!waveform_pending_ready_.load()) return;
    std::vector<float> envelope;
    {
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        envelope = std::move(pending_waveform_envelope_);
    }
    waveform_pending_ready_ = false;
    waveform_envelope_ = std::move(envelope);
    waveform_ready_ = true;
    waveform_reveal_start_ = std::chrono::steady_clock::now(); // starts the 700ms left-to-right reveal
}

void App::launch_search_async(const std::string& query) {
    if (search_thread_.joinable()) search_thread_.join();
    search_in_progress_ = true;
    search_ready_ = false;
    status_line_ = "searching online for \"" + query + "\" ...";

    search_thread_ = std::thread([this, query]() {
        auto results = online_.search(query);
        std::lock_guard<std::mutex> lk(search_mutex_);
        pending_search_results_ = std::move(results);
        search_ready_ = true;
    });
}

void App::poll_pending_search() {
    if (!search_ready_.load()) return;
    std::vector<OnlineResult> results;
    {
        std::lock_guard<std::mutex> lk(search_mutex_);
        results = std::move(pending_search_results_);
    }
    search_ready_ = false;
    search_in_progress_ = false;

    online_view_ = std::move(results);
    selected_ = 0;
    scroll_ = 0;
    status_line_ = online_view_.empty() ? "no online results" : "";
}

// =====================================================================
// Spotify
//
// This section drives a remote control. Spotify's audio is DRM'd and
// mousiki never receives its bytes: choosing a track calls play_uri()
// and Spotify's own player renders it on whichever device is active.
// Hence the inert visualizer and waveform while Spotify owns playback --
// both are fed from Player's audio callback (Player::play's fft_sink),
// and there is no callback when we aren't decoding. Local playback is
// untouched.
// =====================================================================

bool App::ensure_spotify_client() {
    if (spotify_) return true;
    spotify_unavailable_reason_.clear();

    if (!settings_.spotify_enabled) {
        spotify_unavailable_reason_ = "Spotify is off -- set SpotifyEnabled=true in config.txt";
        return false;
    }
    std::string reason;
    spotify_ = SpotifyClientFactory::get(settings_.spotify_force_web_api,
                                         settings_.spotify_client_id, &reason);
    if (!spotify_) {
        spotify_unavailable_reason_ = reason.empty() ? "Spotify is unavailable" : reason;
        return false;
    }
    // A backend can exist and still be useless for browsing: the local
    // MPRIS/AppleScript backends control the desktop app, which gives no
    // view of the account at all. Keep the client (transport control
    // still works through it) but say so, because an empty home menu
    // would otherwise read as "your account has nothing in it".
    if (!spotify_->supports_library()) {
        spotify_unavailable_reason_ =
            std::string("the ") + spotify_->backend_name() +
            " backend can't browse your library -- set SpotifyForceWebApi=true in config.txt";
        return true;
    }
    // Load the auto-mix pool now, in the background, so the first track
    // started already has a continuation to send with it (see
    // spotify_continuation_uris). Runs once: this function builds the
    // client exactly once per session.
    warm_spotify_mix_pool();
    return true;
}

void App::enter_spotify() {
    list_source_ = ListSource::Spotify;
    spotify_level_ = SpotifyLevel::Home;
    queue_focus_ = false;
    selected_ = 0;
    scroll_ = 0;

    if (!ensure_spotify_client() || !spotify_->supports_library()) {
        // Leave the menu empty on purpose: build_spotify_list_rows()
        // renders spotify_unavailable_reason_ in its place, so the
        // blocker is on screen rather than only in the status line that
        // clears itself after five seconds.
        spotify_home_.clear();
        status_line_ = spotify_unavailable_reason_;
        log_event("spotify: " + spotify_unavailable_reason_);
        return;
    }
    spotify_load_home();
    start_spotify_poll();
    log_event("spotify: home");
}

void App::spotify_load_home() {
    spotify_home_ = spotify_home_rows();
    spotify_list_label_.clear();
}

size_t App::spotify_level_len() const {
    switch (spotify_level_) {
        case SpotifyLevel::Home:      return spotify_home_.size();
        case SpotifyLevel::Tracks:    return spotify_tracks_.size();
        case SpotifyLevel::Playlists: return spotify_playlists_.size();
        case SpotifyLevel::Artists:   return spotify_artists_.size();
        case SpotifyLevel::Albums:    return spotify_albums_.size();
    }
    return 0;
}

void App::launch_spotify_fetch(std::function<void(SpotifyFetch&)> work) {
    if (spotify_fetch_in_progress_.load()) {
        status_line_ = "still loading from spotify ...";
        return;
    }
    if (spotify_thread_.joinable()) spotify_thread_.join();
    spotify_fetch_parent_ = spotify_level_; // applied only on success, see poll
    spotify_fetch_in_progress_ = true;
    spotify_fetch_ready_ = false;

    spotify_thread_ = std::thread([this, work = std::move(work)]() {
        SpotifyFetch f;
        work(f);
        std::lock_guard<std::mutex> lk(spotify_mutex_);
        pending_spotify_fetch_ = std::move(f);
        spotify_fetch_ready_ = true;
    });
}

void App::poll_pending_spotify_fetch() {
    if (!spotify_fetch_ready_.load()) return;
    SpotifyFetch f;
    {
        std::lock_guard<std::mutex> lk(spotify_mutex_);
        f = std::move(pending_spotify_fetch_);
        spotify_fetch_ready_ = false;
    }
    spotify_fetch_in_progress_ = false;

    if (!f.success) {
        status_line_ = f.error.empty() ? "spotify request failed" : f.error;
        log_event("spotify: " + status_line_);
        return; // stay exactly where we were -- a failed drill-in is a no-op
    }

    // Only the arriving level's vector is replaced. Overwriting all four
    // would wipe the parent level's rows, and Esc back out of a track
    // list would land on an empty playlist chooser.
    switch (f.level) {
        case SpotifyLevel::Tracks:    spotify_tracks_    = std::move(f.tracks);    break;
        case SpotifyLevel::Playlists: spotify_playlists_ = std::move(f.playlists); break;
        case SpotifyLevel::Artists:   spotify_artists_   = std::move(f.artists);   break;
        case SpotifyLevel::Albums:    spotify_albums_    = std::move(f.albums);    break;
        case SpotifyLevel::Home:      break;
    }
    spotify_parent_level_ = spotify_fetch_parent_;
    spotify_level_ = f.level;
    spotify_list_label_ = f.label;
    selected_ = 0;
    scroll_ = 0;
    status_line_ = spotify_level_len() == 0 ? "nothing here" : "";
}

void App::spotify_open_section(SpotifyHomeSection section) {
    if (!ensure_spotify_client()) { status_line_ = spotify_unavailable_reason_; return; }
    // Captured by value: the worker thread must not read spotify_, which
    // the main thread may reassign (sign-in, factory reset) while the
    // fetch is in flight.
    auto client = spotify_;

    switch (section) {
        case SpotifyHomeSection::DailyMix: {
            const int seed = spotify_daily_seed();
            status_line_ = "building today's daily mix ...";
            launch_spotify_fetch([client, seed](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                f.label = "DAILY MIX";
                // Three cheap, bounded reads. Not Liked Songs, which
                // pages the whole library and would turn opening the
                // home screen into dozens of requests.
                //
                // Partial failure is tolerated: the mix is built from
                // whatever came back, and only all three failing is an
                // error. A fresh account has no top tracks yet, and that
                // shouldn't mean no daily mix.
                std::vector<SpotifyTrack> now_favs, all_time, recent;
                std::string err;
                std::vector<DailyMixSource> sources;
                if (client->top_tracks("short_term", 50, now_favs, &err) && !now_favs.empty())
                    sources.push_back({std::move(now_favs), 5});
                if (client->top_tracks("long_term", 50, all_time, &err) && !all_time.empty())
                    sources.push_back({std::move(all_time), 3});
                if (client->recently_played(50, recent, &err) && !recent.empty())
                    sources.push_back({std::move(recent), 2});
                if (sources.empty()) {
                    f.error = err.empty() ? "no listening history to build a mix from" : err;
                    return;
                }
                f.tracks = build_daily_mix(sources, seed);
                f.success = true;
            });
            break;
        }
        case SpotifyHomeSection::JumpBackIn:
            launch_spotify_fetch([client](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                f.label = "JUMP BACK IN";
                f.success = client->recently_played(50, f.tracks, &f.error);
            });
            break;
        case SpotifyHomeSection::TopTracks:
            launch_spotify_fetch([client](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                f.label = "YOUR TOP TRACKS";
                f.success = client->top_tracks("short_term", 50, f.tracks, &f.error);
            });
            break;
        case SpotifyHomeSection::TopArtists:
            launch_spotify_fetch([client](SpotifyFetch& f) {
                f.level = SpotifyLevel::Artists;
                f.label = "YOUR TOP ARTISTS";
                f.success = client->top_artists("short_term", 50, f.artists, &f.error);
            });
            break;
        case SpotifyHomeSection::LikedSongs:
            // Honest warning: this one really does page the whole
            // library, which is slow for a large one.
            status_line_ = "loading liked songs (reads your whole library) ...";
            launch_spotify_fetch([client](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                f.label = "LIKED SONGS";
                f.success = client->saved_tracks(f.tracks, &f.error);
            });
            break;
        case SpotifyHomeSection::Playlists:
            launch_spotify_fetch([client](SpotifyFetch& f) {
                f.level = SpotifyLevel::Playlists;
                f.label = "YOUR PLAYLISTS";
                f.success = client->playlists(f.playlists, &f.error);
            });
            break;
        case SpotifyHomeSection::SavedAlbums:
            launch_spotify_fetch([client](SpotifyFetch& f) {
                f.level = SpotifyLevel::Albums;
                f.label = "SAVED ALBUMS";
                f.success = client->saved_albums(f.albums, &f.error);
            });
            break;
        case SpotifyHomeSection::Search:
            // Opens the normal search bar pre-filled with the Spotify
            // prefix, so there is one search affordance rather than a
            // separate Spotify-only input.
            mode_ = Mode::Search;
            pre_search_list_source_ = list_source_;
            pre_search_local_query_ = last_local_query_;
            search_buffer_ = kSpotifySearchPrefix;
            break;
    }
}

void App::launch_spotify_search(const std::string& query) {
    if (!ensure_spotify_client()) { status_line_ = spotify_unavailable_reason_; return; }
    auto client = spotify_;
    spotify_search_query_ = query;
    list_source_ = ListSource::Spotify;
    status_line_ = "searching spotify for \"" + query + "\" ...";
    launch_spotify_fetch([client, query](SpotifyFetch& f) {
        f.level = SpotifyLevel::Tracks;
        f.label = "SEARCH: " + query;
        f.success = client->search_tracks(query, 50, f.tracks, &f.error);
    });
}

void App::spotify_activate_selected() {
    const int idx = selected_;

    // The one case where the home level has no rows: we never got a
    // usable client. Enter then means "fix it", which for a missing
    // token is the sign-in.
    if (spotify_level_ == SpotifyLevel::Home && spotify_home_.empty()) {
        launch_spotify_sign_in();
        return;
    }
    if (idx < 0) return;

    switch (spotify_level_) {
        case SpotifyLevel::Home:
            if (idx < static_cast<int>(spotify_home_.size()))
                spotify_open_section(spotify_home_[idx].section);
            return;

        case SpotifyLevel::Tracks:
            if (idx < static_cast<int>(spotify_tracks_.size()))
                spotify_play_track(spotify_tracks_[idx]);
            return;

        case SpotifyLevel::Playlists: {
            if (idx >= static_cast<int>(spotify_playlists_.size())) return;
            auto client = spotify_;
            const SpotifyPlaylist pl = spotify_playlists_[idx];
            status_line_ = "loading \"" + pl.name + "\" ...";
            launch_spotify_fetch([client, pl](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                f.label = pl.name;
                f.success = client->playlist_tracks(pl, f.tracks, &f.error);
            });
            return;
        }
        case SpotifyLevel::Albums: {
            if (idx >= static_cast<int>(spotify_albums_.size())) return;
            auto client = spotify_;
            const SpotifyAlbum ab = spotify_albums_[idx];
            status_line_ = "loading \"" + ab.name + "\" ...";
            launch_spotify_fetch([client, ab](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                f.label = ab.name;
                f.success = client->album_tracks(ab, f.tracks, &f.error);
            });
            return;
        }
        case SpotifyLevel::Artists: {
            if (idx >= static_cast<int>(spotify_artists_.size())) return;
            auto client = spotify_;
            const SpotifyArtist ar = spotify_artists_[idx];
            status_line_ = "loading \"" + ar.name + "\" ...";
            launch_spotify_fetch([client, ar](SpotifyFetch& f) {
                f.level = SpotifyLevel::Tracks;
                // An artist's own top tracks is as deep as artist
                // browsing can go -- Related Artists is one of the
                // endpoints Spotify restricted for new apps.
                f.label = ar.name + " (top tracks)";
                f.success = client->artist_top_tracks(ar, f.tracks, &f.error);
            });
            return;
        }
    }
}

void App::spotify_enqueue_selected() {
    if (spotify_level_ != SpotifyLevel::Tracks) {
        status_line_ = "only tracks can be queued -- open a playlist first";
        return;
    }
    if (selected_ < 0 || selected_ >= static_cast<int>(spotify_tracks_.size())) return;
    if (!ensure_spotify_client()) { status_line_ = spotify_unavailable_reason_; return; }

    const SpotifyTrack t = spotify_tracks_[selected_];
    // Appended locally right away so the panel reacts on this frame; the
    // next poll replaces the list with Spotify's authoritative version.
    spotify_queue_.push_back(t);
    status_line_ = "queued on spotify: " + t.search_query();

    auto client = spotify_;
    const std::string uri = t.uri;
    const std::string label = t.search_query();
    spotify_run_command([this, client, uri, label]() {
        std::string err;
        if (!client->enqueue(uri, &err)) {
            spotify_post_message(err.empty() ? "spotify refused the queue add" : err);
            return;
        }
        spotify_post_message("queued on spotify: " + label);
    });
}

bool App::spotify_back() {
    switch (spotify_level_) {
        case SpotifyLevel::Home:
            return false; // let the normal Esc handler take us back to Local
        case SpotifyLevel::Tracks:
            // Back to whichever chooser we drilled in from, which for a
            // section opened straight off the menu is the menu itself.
            spotify_level_ = spotify_parent_level_;
            break;
        case SpotifyLevel::Playlists:
        case SpotifyLevel::Artists:
        case SpotifyLevel::Albums:
            spotify_level_ = SpotifyLevel::Home;
            break;
    }
    if (spotify_level_ == SpotifyLevel::Home && spotify_home_.empty()) spotify_load_home();
    spotify_list_label_.clear();
    selected_ = 0;
    scroll_ = 0;
    return true;
}

void App::yield_playback_to_spotify() {
    // Exactly one decoder may be running. mousiki's player has to be
    // stopped outright rather than paused: a paused device still holds
    // the audio hardware, and nothing would ever resume it.
    if (has_track_) {
        player_.stop();
        has_track_ = false;
    }
    current_pcm_.reset();
    waveform_envelope_.clear();
    waveform_ready_ = false;
    ++waveform_epoch_;  // discard any in-flight waveform for the local track
    fft_.reset();       // stop the previous track's spectrum tail from lingering
}

// --- Spotify auto-mix pool --------------------------------------------
// An account that answers nothing here -- no listening history, or a
// backend without the scopes -- must not be asked again on every single
// play, so the attempts run out.
static constexpr int kSpotifyMixPoolAttempts = 3;

void App::store_spotify_mix_pool(std::vector<SpotifyTrack> top_recent,
                                 std::vector<SpotifyTrack> top_all_time,
                                 std::vector<SpotifyTrack> recently_played) {
    const bool any = !top_recent.empty() || !top_all_time.empty() || !recently_played.empty();
    {
        std::lock_guard<std::mutex> lk(spotify_mix_mutex_);
        spotify_mix_top_recent_ = std::move(top_recent);
        spotify_mix_top_all_time_ = std::move(top_all_time);
        spotify_mix_recently_played_ = std::move(recently_played);
    }
    if (any) spotify_mix_pool_ready_ = true;
}

void App::warm_spotify_mix_pool() {
    if (spotify_mix_pool_ready_.load()) return;
    if (spotify_mix_pool_loading_.exchange(true)) return; // one fetch in flight at a time
    if (spotify_mix_pool_attempts_.fetch_add(1) >= kSpotifyMixPoolAttempts ||
        !spotify_ || !spotify_->supports_library()) {
        spotify_mix_pool_loading_ = false;
        return;
    }

    auto client = spotify_;
    // Its own thread rather than the command worker: that queue is FIFO,
    // and three catalogue requests sitting in front of the next transport
    // command would make pressing play feel stuck.
    std::thread([this, client]() {
        std::vector<SpotifyTrack> top_recent, top_all_time, recent;
        std::string ignored;
        client->top_tracks("short_term", 50, top_recent, &ignored);
        client->top_tracks("long_term", 50, top_all_time, &ignored);
        client->recently_played(50, recent, &ignored);
        store_spotify_mix_pool(std::move(top_recent), std::move(top_all_time), std::move(recent));
        spotify_mix_pool_loading_ = false;
    }).detach();
}

std::vector<std::string> App::spotify_continuation_uris(const std::string& seed_uri,
                                                        int seed, int size) {
    if (size <= 0 || seed_uri.empty() || !spotify_mix_pool_ready_.load()) return {};

    std::vector<DailyMixSource> sources;
    {
        std::lock_guard<std::mutex> lk(spotify_mix_mutex_);
        // Same weighting the home screen's Daily Mix uses: what the
        // listener is on right now ahead of all-time favourites, and the
        // last session's tracks behind both.
        if (!spotify_mix_top_recent_.empty())      sources.push_back({spotify_mix_top_recent_, 5});
        if (!spotify_mix_top_all_time_.empty())    sources.push_back({spotify_mix_top_all_time_, 3});
        if (!spotify_mix_recently_played_.empty()) sources.push_back({spotify_mix_recently_played_, 2});
    }
    if (sources.empty()) return {};

    // One extra, because the seed track is very likely in the pool itself
    // (it was probably picked from exactly these lists) and gets dropped.
    std::vector<SpotifyTrack> mix =
        build_daily_mix(sources, seed, static_cast<std::size_t>(size) + 1);

    std::vector<std::string> uris{seed_uri};
    for (const auto& m : mix) {
        if (static_cast<int>(uris.size()) > size) break; // seed + size
        if (m.uri.empty() || m.uri == seed_uri) continue;
        uris.push_back(m.uri);
    }
    return uris.size() > 1 ? uris : std::vector<std::string>{};
}

bool App::spotify_queue_from(const std::string& uri, std::vector<std::string>& out_uris) const {
    out_uris.clear();
    if (uri.empty()) return false;
    auto it = std::find_if(spotify_queue_.begin(), spotify_queue_.end(),
                           [&uri](const SpotifyTrack& q) { return q.uri == uri; });
    if (it == spotify_queue_.end()) return false;
    // The picked track first, then everything that was behind it. What
    // was ahead of it drops off, which is what picking a row out of
    // "Next in queue" does in Spotify's own clients.
    for (; it != spotify_queue_.end(); ++it) {
        if (!it->uri.empty()) out_uris.push_back(it->uri);
    }
    return !out_uris.empty();
}

void App::spotify_play_track(const SpotifyTrack& t) {
    if (!ensure_spotify_client()) { status_line_ = spotify_unavailable_reason_; return; }

    // Is this already the playing track? Then Enter is a restart, not a
    // change, and the derived work below (lyrics) would recompute an
    // identical answer.
    const bool already_current =
        spotify_playing_ && !t.uri.empty() && spotify_status_.track.uri == t.uri;

    // Hand the UI over FIRST and send the command afterwards. The call
    // itself is 400-1100ms per round-trip and play_uri() may need three
    // of them, so doing it the other way round froze the render loop for
    // over two seconds on every track change.
    //
    // Bumping the generation here is what invalidates any status()
    // request already in flight: it describes the previous track, and
    // applying it would flip this panel straight back.
    ++spotify_status_gen_;
    spotify_expected_uri_ = t.uri;
    spotify_expected_until_ =
        std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(kSpotifyExpectSeconds));

    yield_playback_to_spotify();
    spotify_playing_ = true;

    // Seeded from what we already know, so metadata and the progress bar
    // are correct on the very next frame rather than after a poll.
    spotify_status_ = SpotifyStatus{};
    spotify_status_.is_running = true;
    spotify_status_.player.state = "playing";
    spotify_status_.player.position_ms = 0;
    spotify_status_.player.duration_ms = t.duration_ms;
    spotify_status_.track.name = t.name;
    spotify_status_.track.artist = t.artist;
    spotify_status_.track.album = t.album;
    spotify_status_.track.uri = t.uri;
    spotify_status_.track.art_url = t.art_url;
    spotify_status_at_ = std::chrono::steady_clock::now();
    apply_spotify_metadata();
    // From the list row rather than the poll, so the disk turns over at
    // the same moment the rest of the panel does instead of a poll
    // interval later.
    set_disk_album_art(t.art_url, t.id);

    // Lyrics work here, off better tags than the yt-dlp path's: these are
    // Spotify's own rather than a guess from an uploader's video title.
    // Fired once, from here -- the expected-uri guard above is what keeps
    // the poll path from firing another for the same track. No path, so
    // no .lrc sidecar to read or write.
    //
    // Skipped when this is the track already playing (Enter on it just
    // restarts it from the top): the lyrics on screen are already the
    // right ones, and refetching costs three more provider requests for
    // a result that cannot differ.
    if (spotify_lyrics_uri_ != t.uri) {
        spotify_lyrics_uri_ = t.uri;
        last_lyrics_status_.clear();
        launch_lyrics_fetch(t.name, t.artist, fs::path());
    }

    status_line_ = (already_current ? "restarting on spotify: " : "starting on spotify: ")
                 + t.search_query();
    start_spotify_poll();

    auto client = spotify_;
    const std::string uri = t.uri;
    const std::string label = t.search_query();
    const bool auto_mix = settings_.spotify_auto_mix && settings_.spotify_auto_mix_size > 0;
    const int mix_size = settings_.spotify_auto_mix_size;
    // Seed is the day combined with the track, so starting the same song
    // twice today continues the same way, while a different song gets a
    // different continuation. Keyed on the uri rather than a counter so
    // it survives a restart.
    const int mix_seed = spotify_daily_seed() ^ static_cast<int>(std::hash<std::string>{}(t.uri));

    // Spotify's own shuffle semantics, which is what was asked for:
    //
    //   - a track that is already in the up-next queue continues that
    //     queue from it, the way clicking a row in Spotify's "Next in
    //     queue" does (and, like there, the rows above it drop off);
    //   - anything else starts a new continuation, re-randomised, and
    //     that continuation REPLACES the queue.
    //
    // Both go out as a single play command carrying the whole uri list,
    // which is the only way to replace a queue: POST /me/player/queue
    // can only append, so the previous continuation used to survive
    // every new pick and the queue grew by ten tracks per song started.
    std::vector<std::string> play_list;
    bool continuing_queue = false;
    if (spotify_queue_from(uri, play_list)) {
        continuing_queue = true;
    } else if (auto_mix) {
        play_list = spotify_continuation_uris(uri, mix_seed, mix_size);
    }
    // Pool not loaded yet (the first play of a session, before the warm
    // started at sign-in has landed) -- fall back to starting the one
    // track and appending a continuation behind it. That path keeps the
    // pool it fetches, so the next pick replaces the queue properly.
    const bool append_fallback = !continuing_queue && auto_mix && play_list.empty();

    spotify_run_command([this, client, uri, label, mix_size, mix_seed,
                         play_list, continuing_queue, append_fallback]() {
        std::string err, device;
        const bool started = play_list.empty()
            ? client->play_uri(uri, &err, &device)
            : client->play_uris(play_list, 0, &err, &device);
        if (!started) {
            spotify_post_message(err.empty() ? "spotify refused to play that" : err);
            return;
        }
        // Naming the device matters in a way it wouldn't for a local
        // file: play_uri() may have had to wake an idle client, and that
        // client can be a different machine. "Nothing happened" and
        // "it's playing in the other room" look identical from here.
        //
        // One message rather than two: only the newest reaches the status
        // line, and both halves matter -- where it is playing, and what
        // it is going to play next.
        std::string note = device.empty() ? "playing on spotify: " + label
                                          : "playing on " + device + ": " + label;
        if (continuing_queue) {
            note += " -- continuing the queue, " +
                    std::to_string(play_list.size() - 1) + " up next";
        } else if (!play_list.empty()) {
            note += " -- auto-mix: " + std::to_string(play_list.size() - 1) + " up next";
        }
        spotify_post_message(note);
        if (!append_fallback) return;

        // Everything below runs AFTER playback has started, on the worker
        // -- the track is already audible, and the queue fills in behind
        // it. Nothing here can delay the music. It is only reached with a
        // cold mix pool, where there was no list to hand the play command
        // -- so it also keeps what it fetches, and the next pick takes
        // the replace-the-queue path above instead of this one.
        //
        // Not Spotify's radio: /v1/recommendations is closed to apps
        // registered after 2024-11-27, so there is no "more like this
        // track" to ask for. This draws from the listener's own history
        // instead, the same pool the home screen's Daily Mix uses.
        std::vector<SpotifyTrack> now_favs, all_time, recent;
        std::string ignored;
        client->top_tracks("short_term", 50, now_favs, &ignored);
        client->top_tracks("long_term", 50, all_time, &ignored);
        client->recently_played(50, recent, &ignored);

        std::vector<DailyMixSource> sources;
        if (!now_favs.empty())  sources.push_back({now_favs, 5});
        if (!all_time.empty())  sources.push_back({all_time, 3});
        if (!recent.empty())    sources.push_back({recent, 2});
        // Kept, so the next pick needs none of these three requests and
        // can hand its whole continuation to the play command.
        store_spotify_mix_pool(std::move(now_favs), std::move(all_time), std::move(recent));
        if (sources.empty()) return; // nothing to mix from; the track still plays

        // One extra, because the seed track is very likely in the pool
        // itself (it was probably picked from exactly these lists) and
        // gets dropped below.
        std::vector<SpotifyTrack> mix =
            build_daily_mix(sources, mix_seed, static_cast<std::size_t>(mix_size) + 1);

        int queued = 0;
        for (const auto& m : mix) {
            if (queued >= mix_size) break;
            if (m.uri == uri) continue; // don't queue the track that is playing
            std::string qerr;
            if (!client->enqueue(m.uri, &qerr)) {
                // One failure is the whole queue failing (no active
                // device, a revoked token), so stop rather than firing
                // another dozen requests at the same error.
                spotify_post_message("auto-mix stopped: " + (qerr.empty() ? std::string("queue add refused") : qerr));
                break;
            }
            ++queued;
        }
        if (queued > 0) {
            spotify_post_message("auto-mix: queued " + std::to_string(queued) +
                                 " more from your listening history");
        }
    });
}

void App::apply_spotify_metadata() {
    metadata_ = TrackMetadata{};
    metadata_.name = spotify_status_.track.name;
    metadata_.artist = spotify_status_.track.artist.empty() ? "-" : spotify_status_.track.artist;
    metadata_.year = "-";
    // These three describe a decoded file, and there is no file. Saying
    // so beats a dash, which reads as "we tried and failed".
    metadata_.sampling = "n/a (remote)";
    metadata_.format = "spotify stream";
    metadata_.file_size = "n/a (remote)";
    metadata_.type = "audio only";
    metadata_.location = spotify_status_.device.empty() ? "spotify" : spotify_status_.device;
    if (!spotify_status_.track.album.empty()) {
        metadata_.extra_label = "album";
        metadata_.extra_value = spotify_status_.track.album;
    }
}

double App::spotify_elapsed_sec() const {
    if (!spotify_playing_) return 0.0;
    const double base = static_cast<double>(spotify_status_.player.position_ms) / 1000.0;
    // Paused: frozen at whatever the last poll saw. Interpolating would
    // walk the bar forward while the music isn't moving.
    if (spotify_status_.player.state != "playing") return base;

    const double since =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - spotify_status_at_).count();
    double out = base + since;
    // Never run past the end: the poll interval is up to 5s, so without
    // this the bar would overshoot into the next track's first seconds.
    const double dur = static_cast<double>(spotify_status_.player.duration_ms) / 1000.0;
    if (dur > 0.0) out = std::min(out, dur);
    return out;
}

double App::display_elapsed_sec() const {
    if (spotify_playing_) return spotify_elapsed_sec();
    return has_track_ ? player_.poll_elapsed() : 0.0;
}

size_t App::display_total_sec() const {
    if (spotify_playing_)
        return static_cast<size_t>(std::max<long long>(0, spotify_status_.player.duration_ms / 1000));
    return total_sec_;
}

// ---------------------------------------------------------------------
// Now-playing poll
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Command worker -- keeps every Spotify round-trip off the render thread
// ---------------------------------------------------------------------

void App::spotify_run_command(std::function<void()> work) {
    if (!spotify_cmd_started_.exchange(true)) {
        spotify_cmd_thread_ = std::thread([this]() {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(spotify_cmd_mutex_);
                    spotify_cmd_cv_.wait(lk, [this]() {
                        return spotify_cmd_stop_.load() || !spotify_cmd_queue_.empty();
                    });
                    if (spotify_cmd_stop_.load() && spotify_cmd_queue_.empty()) return;
                    job = std::move(spotify_cmd_queue_.front());
                    spotify_cmd_queue_.pop_front();
                }
                job(); // outside the lock: this blocks on the network
            }
        });
    }
    {
        std::lock_guard<std::mutex> lk(spotify_cmd_mutex_);
        spotify_cmd_queue_.push_back(std::move(work));
    }
    spotify_cmd_cv_.notify_one();
}

void App::stop_spotify_cmd_worker() {
    if (!spotify_cmd_started_.load()) return;
    spotify_cmd_stop_ = true;
    spotify_cmd_cv_.notify_all();
    if (spotify_cmd_thread_.joinable()) spotify_cmd_thread_.join();
}

void App::spotify_post_message(const std::string& msg) {
    std::lock_guard<std::mutex> lk(spotify_msg_mutex_);
    spotify_msgs_.push_back(msg);
}

void App::poll_pending_spotify_messages() {
    std::vector<std::string> msgs;
    {
        std::lock_guard<std::mutex> lk(spotify_msg_mutex_);
        if (spotify_msgs_.empty()) return;
        msgs.swap(spotify_msgs_);
    }
    // Only the newest reaches the status line (it is one line), but all
    // of them reach the log so nothing is silently lost.
    for (const auto& m : msgs) log_event(m);
    status_line_ = msgs.back();
}

void App::start_spotify_poll() {
    if (!spotify_) return;
    // A thread that is running but has already been asked to stop is not
    // a reason to skip starting: it is about to exit, and returning early
    // here would leave nothing polling at all. Join it out first instead.
    if (spotify_poll_running_.load() && !spotify_poll_stop_.load()) return;
    if (spotify_poll_running_.load()) stop_spotify_poll();
    if (spotify_poll_thread_.joinable()) spotify_poll_thread_.join();

    auto client = spotify_;
    // settings.cpp floors the configured value at 1000ms; the Web API
    // backend needs a higher floor to stay inside Spotify's rate limits,
    // since each tick costs two requests (state + queue).
    const int interval = std::max(settings_.spotify_poll_interval_ms,
                                  client->supports_library() ? 5000 : 1000);
    spotify_poll_stop_ = false;
    spotify_poll_running_ = true;

    spotify_poll_thread_ = std::thread([this, client, interval]() {
        while (!spotify_poll_stop_.load()) {
            // Captured BEFORE the request: this is the world-state this
            // reading describes. If a local action bumps the counter
            // while the request is in flight, the reading is already
            // obsolete and gets dropped rather than applied.
            const int gen = spotify_status_gen_.load();

            SpotifyStatus st;
            std::string err;
            const bool ok = client->status(st, &err);

            std::vector<SpotifyTrack> up_next;
            // Only worth asking when something is actually loaded --
            // with no active device the queue endpoint has nothing to
            // say and would just burn a request every tick.
            if (ok && st.is_running) client->queue(up_next, nullptr);

            if (ok) {
                std::lock_guard<std::mutex> lk(spotify_status_mutex_);
                if (gen == spotify_status_gen_.load()) { // still current
                    pending_spotify_status_ = std::move(st);
                    pending_spotify_queue_ = std::move(up_next);
                    pending_spotify_status_gen_ = gen;
                    spotify_status_ready_ = true;
                }
            }

            // Slept in slices so stop_spotify_poll() doesn't have to
            // wait out a whole interval (up to 5s) on quit -- and so a
            // local action can cut the wait short: after pressing play
            // the user expects the panel to catch up in a moment, not
            // on the next five-second tick.
            for (int slept = 0; slept < interval && !spotify_poll_stop_.load(); slept += 100) {
                if (spotify_status_gen_.load() != gen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        spotify_poll_running_ = false;
    });
}

void App::stop_spotify_poll() {
    spotify_poll_stop_ = true;
    if (spotify_poll_thread_.joinable()) spotify_poll_thread_.join();
    spotify_poll_running_ = false;
}

void App::poll_pending_spotify_status() {
    if (!spotify_status_ready_.load()) return;
    SpotifyStatus st;
    std::vector<SpotifyTrack> up_next;
    int st_gen = 0;
    {
        std::lock_guard<std::mutex> lk(spotify_status_mutex_);
        st = std::move(pending_spotify_status_);
        up_next = std::move(pending_spotify_queue_);
        st_gen = pending_spotify_status_gen_;
        spotify_status_ready_ = false;
    }
    // Consume-side half of the generation guard. The publish-side check
    // cannot catch a reading that was already in the slot when a local
    // action happened -- this one can.
    if (st_gen != spotify_status_gen_.load()) return;

    const std::string prev_uri = spotify_status_.track.uri;

    // Waiting on a track we just asked for? Then a reading that still
    // shows something else is Spotify not having switched yet, not the
    // user changing tracks elsewhere. Take the live numbers from it but
    // keep the track identity we are expecting, so the panel doesn't
    // flash back to the previous song and no lyrics fetch fires for it.
    const bool expecting = !spotify_expected_uri_.empty() &&
                           std::chrono::steady_clock::now() < spotify_expected_until_;
    if (expecting && st.is_running && st.track.uri != spotify_expected_uri_) {
        spotify_status_.player.volume = st.player.volume;
        spotify_status_.player.is_shuffling = st.player.is_shuffling;
        spotify_status_.player.is_repeating = st.player.is_repeating;
        if (!st.device.empty()) spotify_status_.device = st.device;
        spotify_queue_ = std::move(up_next);
        return;
    }
    // Confirmed (or the expectation timed out) -- stop second-guessing
    // the poll from here on.
    if (!spotify_expected_uri_.empty() &&
        (!expecting || st.track.uri == spotify_expected_uri_)) {
        spotify_expected_uri_.clear();
    }

    spotify_status_ = std::move(st);
    spotify_status_at_ = std::chrono::steady_clock::now();
    spotify_queue_ = std::move(up_next);

    // Spotify stopped (or moved to a device we can't see): hand the
    // "what's playing" role back rather than leaving a frozen panel
    // claiming a track is on.
    if (!spotify_status_.is_running) {
        if (spotify_playing_) {
            spotify_playing_ = false;
            spotify_lyrics_uri_.clear(); // coming back to this track later should refetch
            set_disk_album_art("", "");  // nothing is playing: back to the vinyl
            status_line_ = "spotify stopped";
        }
        return;
    }

    // Spotify advanced to the next track on its own (its queue, its
    // repeat/shuffle -- mousiki deliberately doesn't drive that). Refresh
    // what's derived from the track identity.
    if (spotify_status_.track.uri != prev_uri && !spotify_status_.track.uri.empty()) {
        // has_track_ guard: Spotify playing on the user's phone while
        // mousiki decodes a local file is a normal state, and without
        // this the panels would follow the phone's track while local
        // audio is what's in the speakers. Whatever mousiki is decoding
        // wins; Spotify takes the role only when spotify_play_track()
        // hands it over through yield_playback_to_spotify().
        if (has_track_) return;
        spotify_playing_ = true;
        apply_spotify_metadata();
        // Only if nobody has already fetched these -- the local play path
        // normally gets here first, and Spotify's URI relinking means
        // the uri it reports back is not always the one we asked for.
        if (spotify_lyrics_uri_ != spotify_status_.track.uri) {
            spotify_lyrics_uri_ = spotify_status_.track.uri;
            last_lyrics_status_.clear();
            launch_lyrics_fetch(spotify_status_.track.name, spotify_status_.track.artist, fs::path());
        }
        // The uri is "spotify:track:<id>"; the id alone is the cache key.
        set_disk_album_art(spotify_status_.track.art_url,
                           spotify_status_.track.uri.substr(spotify_status_.track.uri.rfind(':') + 1));
        log_event("spotify: now playing " + spotify_status_.display());
    } else if (spotify_playing_) {
        // Same track: the volume/device/state rows can still have moved.
        apply_spotify_metadata();
    }
}

// ---------------------------------------------------------------------
// Album art on the disk
// ---------------------------------------------------------------------

// Points the disk at `url`'s cover. Returns immediately: the download
// and the ffmpeg decode happen on a detached thread (same shape as
// launch_lyrics_fetch), and poll_pending_album_art() installs the result
// on the next frame.
//
// `cache_key` is the Spotify track id, which is what the decoded frame
// is filed under on disk -- so coming back to a song later costs no
// network and the cover appears on the very first frame.
void App::set_disk_album_art(const std::string& url, const std::string& cache_key) {
    if (!settings_.disk_album_art) {
        // Turned off mid-session, with a cover still spinning.
        if (!disk_art_url_.empty()) {
            disk_art_url_.clear();
            disk_.clear_album_art();
        }
        return;
    }
    // Already showing this cover, or already fetching it. Track changes
    // come in through both spotify_play_track() and the status poll, and
    // the poll re-reports the same track every few seconds forever.
    if (url == disk_art_url_) return;

    disk_art_url_ = url;
    if (url.empty()) { disk_.clear_album_art(); return; }

    const int resolution = disk_.art_resolution();
    const fs::path cache_dir = cache_.cache_dir();
    std::thread([this, url, cache_key, resolution, cache_dir]() {
        AlbumArtImage img;
        std::string err;
        if (!fetch_album_art(url, cache_key, cache_dir, resolution, img, &err)) {
            // Logged, not surfaced in the status line: a missing cover
            // is cosmetic, and the line is where playback errors go.
            ConsoleLog::instance().log_basic("spotify: " + err);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(album_art_mutex_);
            pending_album_art_ = std::move(img);
            pending_album_art_url_ = url;
        }
        album_art_ready_ = true;
    }).detach();
}

void App::poll_pending_album_art() {
    if (!album_art_ready_.load()) return;

    AlbumArtImage img;
    std::string url;
    {
        std::lock_guard<std::mutex> lk(album_art_mutex_);
        img = std::move(pending_album_art_);
        url = std::move(pending_album_art_url_);
        pending_album_art_ = AlbumArtImage{};
        pending_album_art_url_.clear();
        album_art_ready_ = false;
    }

    // Skipped past while this was downloading.
    if (url != disk_art_url_) return;
    disk_.set_album_art(img);
}

// ---------------------------------------------------------------------
// Sign-in
// ---------------------------------------------------------------------

void App::launch_spotify_sign_in() {
    if (spotify_auth_in_progress_.load()) {
        status_line_ = "sign-in already in progress -- check your browser";
        return;
    }
    if (settings_.spotify_client_id.empty()) {
        status_line_ = "set SpotifyClientId in config.txt first (developer.spotify.com/dashboard)";
        return;
    }
    if (spotify_auth_thread_.joinable()) spotify_auth_thread_.join();

    spotify_auth_in_progress_ = true;
    spotify_auth_done_ = false;
    status_line_ = "opening browser for Spotify sign-in ...";
    log_event("spotify: sign-in started");

    const std::string client_id = settings_.spotify_client_id;
    spotify_auth_thread_ = std::thread([this, client_id]() {
        SpotifyTokens tokens;
        std::string err;
        // Blocks on a loopback socket until the browser redirects back,
        // giving up after its own timeout. Hence the thread.
        const bool ok = SpotifyAuth::interactive_login(client_id, tokens, &err);
        {
            std::lock_guard<std::mutex> lk(spotify_auth_mutex_);
            spotify_auth_success_ = ok;
            spotify_auth_error_ = err;
        }
        spotify_auth_done_ = true;
    });
}

void App::poll_pending_spotify_auth() {
    if (!spotify_auth_done_.load()) return;
    bool ok = false;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(spotify_auth_mutex_);
        ok = spotify_auth_success_;
        err = spotify_auth_error_;
    }
    spotify_auth_done_ = false;
    spotify_auth_in_progress_ = false;
    if (spotify_auth_thread_.joinable()) spotify_auth_thread_.join();

    if (!ok) {
        status_line_ = err.empty() ? "Spotify sign-in failed" : err;
        log_event("spotify: sign-in failed -- " + status_line_);
        return;
    }
    // The cached client was built (or refused) against the old token
    // state, so it has to be dropped before anything asks for one again.
    SpotifyClientFactory::reset();
    spotify_.reset();
    status_line_ = "signed in to Spotify";
    log_event("spotify: signed in");
    if (list_source_ == ListSource::Spotify) enter_spotify();
}

// Builds one display string per row for whichever Spotify level is
// showing, each exactly as wide as the caller's `inner` allows. Column
// widths mirror the local-files list where the data lines up (index,
// title, artist, mm:ss) so switching between the two doesn't feel like
// a different application.
std::vector<std::string> App::build_spotify_list_rows(int inner) const {
    std::vector<std::string> rows;
    const std::string& sep = settings_.list_separator;
    const int idx_w = 3;
    auto fm = [&](const std::string& t) { return apply_font_map(t, settings_.font_map); };

    // No usable backend: explain it in the panel instead of showing an
    // empty list, which would read as "this account has nothing in it".
    if (spotify_level_ == SpotifyLevel::Home && spotify_home_.empty()) {
        rows.push_back("");
        rows.push_back("  spotify is not available:");
        rows.push_back("  " + (spotify_unavailable_reason_.empty() ? std::string("unknown reason")
                                                                  : spotify_unavailable_reason_));
        rows.push_back("");
        if (!settings_.spotify_client_id.empty()) {
            rows.push_back("  press ENTER to sign in");
            rows.push_back("  (opens accounts.spotify.com in your browser)");
        } else {
            rows.push_back("  add SpotifyClientId to $HOME/.config/mousiki/config.txt");
            rows.push_back("  get one free at developer.spotify.com/dashboard");
        }
        return rows;
    }

    switch (spotify_level_) {
        case SpotifyLevel::Home: {
            const int detail_w = std::min(16, std::max(0, inner / 3));
            const int label_w = std::max(5, inner - 2 - 2 - 2 - detail_w);
            for (const auto& r : spotify_home_) {
                rows.push_back("▸ " + pad_right(truncate_str(fm(r.label), label_w), label_w)
                               + sep + " " + truncate_str(fm(r.detail), detail_w));
            }
            return rows;
        }
        case SpotifyLevel::Tracks: {
            const int artist_w = 16;
            const int dur_w = 5;
            const int title_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - dur_w);
            for (size_t i = 0; i < spotify_tracks_.size(); ++i) {
                const auto& t = spotify_tracks_[i];
                // Duration comes straight from Spotify's duration_ms, so
                // unlike the local list there is nothing to probe and no
                // row that sits blank waiting for ffprobe.
                const std::string dur = fmt_mmss(static_cast<double>(t.duration_ms) / 1000.0);
                rows.push_back(pad_right(fm(std::to_string(i + 1)), idx_w) + sep + " "
                               + pad_right(truncate_str(fm(t.name), title_w), title_w) + sep + " "
                               + pad_right(truncate_str(fm(t.artist), artist_w), artist_w) + sep + " "
                               + fm(dur));
            }
            return rows;
        }
        case SpotifyLevel::Playlists: {
            const int count_w = 11; // "1234 tracks"
            const int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
            for (size_t i = 0; i < spotify_playlists_.size(); ++i) {
                const auto& pl = spotify_playlists_[i];
                rows.push_back(pad_right(fm(std::to_string(i + 1)), idx_w) + sep + " "
                               + pad_right(truncate_str(fm(pl.name), name_w), name_w) + sep + " "
                               + fm(std::to_string(pl.track_count) + " tracks"));
            }
            return rows;
        }
        case SpotifyLevel::Artists: {
            const int genre_w = std::min(24, std::max(0, inner / 3));
            const int name_w = std::max(5, inner - idx_w - 2 - 2 - genre_w);
            for (size_t i = 0; i < spotify_artists_.size(); ++i) {
                const auto& ar = spotify_artists_[i];
                rows.push_back(pad_right(fm(std::to_string(i + 1)), idx_w) + sep + " "
                               + pad_right(truncate_str(fm(ar.name), name_w), name_w) + sep + " "
                               + truncate_str(fm(ar.genres), genre_w));
            }
            return rows;
        }
        case SpotifyLevel::Albums: {
            const int artist_w = 16;
            const int year_w = 4;
            const int name_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - year_w);
            for (size_t i = 0; i < spotify_albums_.size(); ++i) {
                const auto& ab = spotify_albums_[i];
                rows.push_back(pad_right(fm(std::to_string(i + 1)), idx_w) + sep + " "
                               + pad_right(truncate_str(fm(ab.name), name_w), name_w) + sep + " "
                               + pad_right(truncate_str(fm(ab.artist), artist_w), artist_w) + sep + " "
                               + fm(ab.year()));
            }
            return rows;
        }
    }
    return rows;
}

size_t App::current_list_len() const {
    switch (list_source_) {
        case ListSource::Local:   return local_view_.size();
        case ListSource::Online:  return online_view_.size();
        case ListSource::Spotify: return spotify_level_len();
    }
    return 0;
}

void App::play_selected() {
    // Spotify first: its rows aren't all tracks (the home menu, playlist
    // and album rows all drill in rather than play), so Enter there is a
    // navigation action, not a playback one.
    if (list_source_ == ListSource::Spotify) { spotify_activate_selected(); return; }
    size_t list_len = current_list_len();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) start_local_track(local_view_[selected_]);
    else start_online_track(online_view_[selected_]);
}

// Plays the track at `idx` in the current list source directly, without
// touching selected_/scroll_ at all -- the hover cursor is a browsing
// concept, "what's playing" is a playback concept, and they used to get
// conflated (play_relative() would drag the cursor along to wherever
// playback moved). If you're hovering track A while track F plays and
// you skip forward, the next track (G) should just start playing --
// your cursor should still be on A, not silently dragged to G. Only
// play_selected() (an explicit Enter-on-the-hovered-row action) is
// supposed to make cursor and playback coincide; everything else that
// moves playback goes through this instead.
void App::play_at_list_index(int idx) {
    size_t list_len = current_list_len();
    if (idx < 0 || idx >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Spotify) {
        // Only a track list has anything to play at an index; the menu
        // and the playlist/album/artist choosers don't.
        if (spotify_level_ == SpotifyLevel::Tracks && idx < static_cast<int>(spotify_tracks_.size())) {
            spotify_play_track(spotify_tracks_[idx]);
        }
        return;
    }
    if (list_source_ == ListSource::Local) start_local_track(local_view_[idx]);
    else start_online_track(online_view_[idx]);
}

int App::current_track_list_index() const {
    // Spotify's "what's playing" lives in spotify_status_, not in
    // current_path_/current_video_id_, and is matched by track uri.
    if (list_source_ == ListSource::Spotify) {
        if (!spotify_playing_ || spotify_level_ != SpotifyLevel::Tracks) return -1;
        const std::string& uri = spotify_status_.track.uri;
        if (uri.empty()) return -1;
        for (size_t i = 0; i < spotify_tracks_.size(); ++i) {
            if (spotify_tracks_[i].uri == uri) return static_cast<int>(i);
        }
        return -1;
    }
    if (!has_track_) return -1;
    if (list_source_ == ListSource::Local) {
        if (!current_is_local_) return -1; // playing an online track while browsing the local list
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == current_path_) return static_cast<int>(i);
        }
        return -1;
    } else {
        if (current_is_local_) return -1; // playing a local track while browsing online results
        for (size_t i = 0; i < online_view_.size(); ++i) {
            if (online_view_[i].video_id == current_video_id_) return static_cast<int>(i);
        }
        return -1;
    }
}

void App::play_relative(int delta, int precomputed_base) {
    size_t list_len = current_list_len();
    if (list_len == 0) return;
    // Relative to what's actually *playing*, not wherever the hover
    // cursor happens to be sitting -- falls back to the hover cursor
    // only when there's no sensible "current" position in this list
    // (nothing playing yet, or what's playing is from a different
    // source/isn't in this view at all). Either way, only playback
    // moves -- see play_at_list_index()'s comment for why selected_/
    // scroll_ are deliberately left untouched here.
    int base = (precomputed_base != kNoPrecomputedBase) ? precomputed_base : current_track_list_index();
    if (base < 0) base = selected_;
    int target = std::clamp(base + delta, 0, static_cast<int>(list_len) - 1);
    play_at_list_index(target);
}

void App::play_relative_random(int precomputed_base) {
    size_t list_len = current_list_len();
    if (list_len == 0) return;
    if (list_len == 1) { play_at_list_index(0); return; }
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(list_len) - 1);
    int base = (precomputed_base != kNoPrecomputedBase) ? precomputed_base : current_track_list_index();
    if (base < 0) base = selected_;
    int next;
    do { next = dist(rng); } while (next == base);
    play_at_list_index(next);
}

void App::play_next_from_queue() {
    // Shuffle: pick a random queue item instead of strictly FIFO order.
    // Repeat Queue: rotate the played item to the back instead of
    // discarding it, so the whole queue loops indefinitely rather than
    // draining to empty. Both apply here (not just to library playback)
    // -- this is exactly the "queue mode won't respect shuffle or
    // repeat" bug: previously advance_track()'s queue branch always did
    // plain FIFO regardless of play_mode.
    int idx = 0;
    if (settings_.play_mode == 2 /*shuffle*/ && queue_.size() > 1) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, static_cast<int>(queue_.size()) - 1);
        idx = dist(rng);
    }
    QueueItem item = queue_[idx];
    queue_.erase(queue_.begin() + idx);
    if (settings_.play_mode == 4 /*repeat queue*/) {
        queue_.push_back(item); // rotate to the back instead of discarding -- keeps the queue looping
    }
    if (queue_selected_ >= idx && queue_selected_ > 0) --queue_selected_; // index shifted down by the erase
    clamp_queue_selected();
    if (item.is_local) {
        LocalTrack t{fs::path(item.local_path).stem().string(), item.local_path, item.artist};
        start_local_track(t);
    } else {
        OnlineResult r{item.video_id, item.title, item.artist};
        start_online_track(r);
    }
}

void App::advance_track() {
    // Captured *before* clearing has_track_ below: current_track_list_
    // index() requires has_track_ still be true to identify anything,
    // and has_track_ has to be cleared here regardless (run()'s main
    // loop only polls player_.finished() while has_track_ is true --
    // leaving it set would make this get called again every frame,
    // repeatedly skipping further, while the next track's async load is
    // still in flight). Passing this through explicitly is what lets
    // sequential/shuffle auto-advance still resolve "next" from the
    // track that actually just finished rather than falling back to
    // wherever the hover cursor happens to be sitting.
    int base_idx = current_track_list_index();
    has_track_ = false;

    // Repeat: keep replaying whatever just finished -- whether it came
    // from the queue or the library -- without touching the queue or
    // advancing through any list at all. Stop: don't auto-advance into
    // anything, queue or not. Both apply uniformly regardless of the
    // queue's state now -- previously these were only ever consulted
    // once the queue was already empty, which was the other half of the
    // "queue mode won't respect repeat" bug.
    if (settings_.play_mode == 1 /*loop*/) {
        launch_device_play_async(); // same track, already fully decoded, no reload needed
        has_track_ = true;
        player_.clear_finished();
        return;
    }
    if (settings_.play_mode == 3 /*stop*/) {
        return; // leave has_track_ false, no auto-advance -- queue or not
    }

    // The queue always takes priority over the library — it's an
    // explicit user-built-up-next list.
    if (!queue_.empty()) {
        play_next_from_queue();
        return;
    }

    switch (settings_.play_mode) {
        case 2: // shuffle
            play_relative_random(base_idx);
            break;
        default: // list (sequential) -- also where Repeat Queue (4) lands
                 // once the queue's actually empty; there's nothing left
                 // to "repeat queue" without one, so it just falls back
                 // to normal sequential playback.
            play_relative(1, base_idx);
            break;
    }
}

char App::play_mode_letter() const {
    switch (settings_.play_mode) {
        case 1: return 'R';  // repeat (loop current track)
        case 2: return 'S';  // shuffle
        case 3: return 'O';  // stop (play, then stop -- not "S", shuffle already owns that)
        case 4: return 'Q';  // repeat queue
        default: return 'L'; // list (normal sequential)
    }
}

void App::queue_add_selected() {
    // Spotify's queue is the account's, shared with every other client
    // signed in to it -- adding there is an API call, not a push_back.
    if (list_source_ == ListSource::Spotify) { spotify_enqueue_selected(); return; }
    size_t list_len = current_list_len();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) {
        const auto& t = local_view_[selected_];
        queue_.push_back({true, t.title, t.folder_artist, t.path, ""});
    } else {
        const auto& r = online_view_[selected_];
        queue_.push_back({false, r.title, r.uploader, {}, r.video_id});
    }
    clamp_queue_selected();
}

void App::queue_remove_last() {
    if (!queue_.empty()) queue_.pop_back();
    clamp_queue_selected();
}

void App::clamp_queue_selected() {
    if (queue_.empty()) { queue_selected_ = 0; queue_scroll_ = 0; return; }
    queue_selected_ = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    if (queue_selected_ >= queue_scroll_ + list_visible_rows_) queue_scroll_ = queue_selected_ - list_visible_rows_ + 1;
    if (queue_selected_ < queue_scroll_) queue_scroll_ = queue_selected_;
}

void App::queue_remove_hovering() {
    if (queue_.empty() || queue_selected_ < 0 || queue_selected_ >= static_cast<int>(queue_.size())) return;
    queue_.erase(queue_.begin() + queue_selected_);
    clamp_queue_selected();
}

void App::queue_move_hovering(int dir) {
    if (queue_.empty()) return;
    int target = queue_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(queue_.size())) return; // already at an edge
    std::swap(queue_[queue_selected_], queue_[target]);
    queue_selected_ = target;
    clamp_queue_selected();
}

// ---------------------------------------------------------------------
// Autosave / session snapshot
// ---------------------------------------------------------------------

SnapshotData App::build_snapshot() const {
    SnapshotData snap;
    snap.play_mode = settings_.play_mode;
    snap.muted = muted_;
    // Save the *real* volume, not the forced-0 muted value, so unmuting
    // next session restores to what it actually was, not silence.
    snap.volume = muted_ ? pre_mute_volume_ : player_.volume();

    if (has_track_) {
        snap.has_now_playing = true;
        snap.now_playing.is_local = current_is_local_;
        snap.now_playing.path = current_is_local_ ? current_path_.string() : std::string();
        snap.now_playing.video_id = current_is_local_ ? std::string() : current_video_id_;
        snap.now_playing.title = metadata_.name;
        snap.now_playing.artist = metadata_.artist;
        snap.position_sec = player_.poll_elapsed();
    }

    for (const auto& item : queue_) {
        SnapshotTrack t;
        t.is_local = item.is_local;
        t.path = item.is_local ? item.local_path.string() : std::string();
        t.video_id = item.is_local ? std::string() : item.video_id;
        t.title = item.title;
        t.artist = item.artist;
        snap.queue.push_back(std::move(t));
    }

    // Hover cursor -- independent of now_playing (see
    // current_track_list_index()'s comment in app.h). Online results
    // are a fresh search every session, so only a local-list cursor is
    // actually meaningful to restore -- see restore_snapshot().
    snap.cursor_is_local = (list_source_ == ListSource::Local);
    snap.cursor_index = selected_;
    snap.cursor_scroll = scroll_;

    return snap;
}

void App::restore_snapshot(const SnapshotData& snap) {
    settings_.play_mode = std::clamp(snap.play_mode, 0, 4);
    // Apply the saved volume first, then re-apply mute on top of it --
    // mirrors what pressing 'x' does at runtime (force 0, remember the
    // real value), just seeded from the snapshot instead of live state.
    pre_mute_volume_ = std::clamp(snap.volume, 0, 100);
    player_.set_volume(pre_mute_volume_);
    if (snap.muted) {
        player_.set_volume(0);
        muted_ = true;
    }

    queue_.clear();
    for (const auto& t : snap.queue) {
        queue_.push_back({t.is_local, t.title, t.artist, t.is_local ? fs::path(t.path) : fs::path(), t.video_id});
    }
    clamp_queue_selected();

    if (snap.has_now_playing) {
        resume_start_sec_ = std::max(0.0, snap.position_sec);
        // The track is still fully loaded and seeked to where it was --
        // metadata, waveform, lyrics and the progress bar all come back
        // -- it simply doesn't start itself. One keypress resumes it.
        start_paused_ = !settings_.autoplay_on_start;
        if (snap.now_playing.is_local) {
            fs::path p(snap.now_playing.path);
            std::error_code ec;
            if (fs::exists(p, ec)) {
                LocalTrack t{p.stem().string(), p, snap.now_playing.artist};
                start_local_track(t);
                log_event(settings_.autoplay_on_start
                              ? "resuming: " + t.title
                              : "restored (paused): " + t.title + " -- press p to play");
            } else {
                resume_start_sec_ = 0.0; // file's gone -- nothing to resume into
            }
        } else if (!snap.now_playing.video_id.empty()) {
            OnlineResult r{snap.now_playing.video_id, snap.now_playing.title, snap.now_playing.artist};
            start_online_track(r);
        } else {
            resume_start_sec_ = 0.0;
        }
    }

    // Cursor restore happens last and independent of now_playing/queue --
    // it's a browsing-position concept, not a playback one. Only
    // restored when it was pointing at the local list (see the field's
    // comment in snapshot.h); an online-list cursor is left at the
    // default since there's no saved search to put it back into.
    // list_visible_rows_ hasn't been computed from the real terminal
    // yet at this point in startup (that only happens once render_frame
    // runs its first frame), so scroll_ is clamped against a
    // conservative bound here -- the normal up/down-key scroll-follow
    // logic will correct it on the first actual navigation regardless.
    if (snap.cursor_is_local && !local_view_.empty()) {
        selected_ = std::clamp(snap.cursor_index, 0, static_cast<int>(local_view_.size()) - 1);
        scroll_ = std::clamp(snap.cursor_scroll, 0, selected_);
    }
}

void App::maybe_autosave() {
    if (!settings_.autosave_enabled) return;
    double delay = std::max(1, settings_.autosave_delay_sec);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_autosave_at_).count();
    if (elapsed < delay) return;

    last_autosave_at_ = std::chrono::steady_clock::now();
    save_snapshot(build_snapshot());
    autosave_pulse_active_ = true;
    autosave_pulse_started_at_ = last_autosave_at_;
    ConsoleLog::instance().log_basic("autosaved session snapshot");
}

std::string App::autosave_indicator_glyph() const {
    if (!settings_.autosave_enabled || !settings_.autosave_indicator) return "";

    double t = autosave_pulse_active_
             ? std::chrono::duration<double>(std::chrono::steady_clock::now() - autosave_pulse_started_at_).count()
             : kAutosavePulseSeconds + 1.0; // idle -- well past the pulse window

    std::string glyph = settings_.autosave_chr.empty() ? "\u2022" : settings_.autosave_chr;

    if (settings_.autosave_indicator_type == 0) {
        // blink: appear/disappear twice (4 half-cycles) right after a
        // save, then settle back to hidden until the next one.
        if (t >= kAutosavePulseSeconds) return ""; // idle: hidden between saves
        int half_cycle = static_cast<int>(t / (kAutosavePulseSeconds / 4.0));
        bool visible = (half_cycle % 2) == 0;
        return visible ? glyph : " ";
    }

    // color: always visible, pulses C1 -> C2 -> C1 (heartbeat) right
    // after a save, then settles to a steady C1.
    std::string c1 = ansi_for(settings_.autosave_c1.empty() ? settings_.border_color : settings_.autosave_c1, false);
    std::string c2 = ansi_for(settings_.autosave_c2.empty() ? settings_.visualizer_color : settings_.autosave_c2, false);
    if (t >= kAutosavePulseSeconds) return c1 + glyph + "\x1b[0m"; // idle: steady C1
    // Two full heartbeats across the pulse window: C1->C2->C1->C2->C1.
    double phase = std::fmod(t, kAutosavePulseSeconds / 2.0) / (kAutosavePulseSeconds / 2.0); // 0..1 within each half-beat
    bool towards_c2 = phase < 0.5;
    return (towards_c2 ? c2 : c1) + glyph + "\x1b[0m";
}

// ---------------------------------------------------------------------
// Bulk add (paste a YouTube playlist link while Queue is focused)
// ---------------------------------------------------------------------

void App::launch_bulk_add_async(const std::string& url) {
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    bulk_add_in_progress_ = true;
    bulk_add_ready_ = false;
    bulk_add_thread_ = std::thread([this, url]() {
        BulkAddResult res;
        res.items = online_.list_playlist(url, &res.error);
        res.success = res.error.empty() && !res.items.empty();
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        pending_bulk_add_ = std::move(res);
        bulk_add_ready_ = true;
    });
}

void App::poll_pending_bulk_add() {
    if (!bulk_add_ready_.load()) return;
    BulkAddResult res;
    {
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        if (!bulk_add_ready_.load()) return;
        res = std::move(pending_bulk_add_);
        bulk_add_ready_ = false;
    }
    bulk_add_in_progress_ = false;
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();

    if (!res.success) {
        status_line_ = res.error.empty() ? "couldn't load that playlist" : res.error;
        return; // stay in Mode::BulkAdd, phase 1 -- let the person edit the link and retry
    }

    // Enter phase 2: show the checklist rather than committing
    // immediately. Starts fully de-selected -- "SELECT" is meant for
    // picking your own favorites out of the playlist, so nothing is
    // pre-starred; "ALL" (the "a" key) still adds every fetched track
    // regardless of star state, unaffected by this default.
    pending_bulk_add_ = std::move(res);
    bulk_add_selected_.assign(pending_bulk_add_.items.size(), false);
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
    bulk_add_results_ready_ = true;
    status_line_.clear();
}

void App::commit_bulk_add(bool all) {
    int added = 0;
    for (size_t i = 0; i < pending_bulk_add_.items.size(); ++i) {
        if (!all && (i >= bulk_add_selected_.size() || !bulk_add_selected_[i])) continue;
        const auto& item = pending_bulk_add_.items[i];
        queue_.push_back({false, item.title, item.uploader, {}, item.video_id});
        ++added;
    }
    clamp_queue_selected();
    log_event("added " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " to queue");

    mode_ = Mode::Browse;
    bulk_add_results_ready_ = false;
    bulk_add_buffer_.clear();
    pending_bulk_add_ = BulkAddResult{};
    bulk_add_selected_.clear();
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
}

// Tab layout: 0=Colors, 1=On/Off, 2=Animation, 3=Reference, 4=About App.
// Reference-tab rows map to specific well-known hotkey action keys in
// settings_.hotkeys (a plain string->string map already), so they don't
// need their own struct fields the way Colors/On-Off/Animation do.
static const char* kRefHotkeyNames[] = {
    "HKeySetting", "HKeyNavigateUp", "HKeyNavigateDown", "HKeyPlay", "HKeyPlayNextSong",
    "HKeyPlayPreviousSong", "HKeyCyclePlayMode", "HKeySearch",
    "HKeySearchOnline", "HKeyQuit",
    // Appended so the newer default hotkeys are editable/rebindable from
    // the Settings > Reference tab too, not just settable via config.txt.
    "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume",
    "HKeyAddHoveringSongToQueue", "HKeyRemoveHoveringSongFromQueue", "HKeySwitchBetweenCards",
    "HKeyFilterForFolder", "HKeyClearFilter", "HKeyDownloadStream",
    "HKeyRefreshUi", "HKeyConsole", "HKeyToggleMute", "HKeyCheatsheet", "HKeyRetryLyrics",
};
static constexpr int kRefRowCount = 25;

std::string* App::color_field_ptr(int row, int col) {
    switch (row) {
        case 0: return col == 0 ? &settings_.border_color : &settings_.border_color_bottom;
        case 1: return col == 0 ? &settings_.disk_color : &settings_.disk_color_end;
        case 2: return col == 0 ? &settings_.meta_key_color : &settings_.meta_val_color;
        case 3: return col == 0 ? &settings_.visualizer_color : &settings_.visualizer_color_end;
        case 4: return col == 0 ? &settings_.progress_played_color : &settings_.progress_remaining_color;
        case 5: return col == 0 ? &settings_.list_color : &settings_.list_inactive_bg_color;
        case 6: return col == 0 ? &settings_.list_playing_color : &settings_.list_playing_bg_color;
        case 7: return col == 0 ? &settings_.list_cursor_color : &settings_.list_cursor_bg_color;
        case 8: return col == 0 ? &settings_.queue_color : &settings_.queue_inactive_bg_color;
        case 9: return col == 0 ? &settings_.queue_playing_color : &settings_.queue_playing_bg_color;
        case 10: return col == 0 ? &settings_.queue_cursor_color : &settings_.queue_cursor_bg_color;
        case 11: return col == 0 ? &settings_.inactive_line_color : &settings_.inactive_line_bg_color;
        case 12: return col == 0 ? &settings_.active_line_color : &settings_.active_line_bg_color;
        case 13: return col == 0 ? &settings_.active_word_color : &settings_.active_word_bg_color;
        default: return nullptr;
    }
}

int App::settings_max_row() const {
    // Matches get_max_row(): SCHEMA.size() - 1 for each tab, extended for
    // the Reference tab's appended font-map rows and the About tab's
    // scrollable text (both computed dynamically, not hardcoded, so they
    // track the actual font_map/about_app_lines content).
    switch (settings_tab_) {
        case 0: return 13; // COLOR_SCHEMA: 14 rows
        case 1: return 6;  // ONOFF_SCHEMA: 7 rows
        case 2: return 7;  // ANIM_SCHEMA: 8 rows
        case 3: {
            int letters = 0;
            for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) ++letters;
            return kRefRowCount + letters - 1; // 11 hotkeys + N font-map rows
        }
        case 4: {
            int MAX_Y = std::max(term_rows_ - 2, 10);
            int visible = std::max(1, MAX_Y - 3);
            int total = static_cast<int>(settings_.about_app_lines.size());
            return std::max(0, total - visible); // scroll range, not a field cursor
        }
        default: return 0;
    }
}

std::string App::settings_get_value(int row, int col) const {
    // Matches getVal(k): config value if set, else the hardcoded default
    // for the 7 keys that have one, else "___" (unset). Colors format
    // their own "___"-equivalent as "none" at render time instead.
    if (settings_tab_ == 0) {
        std::string* p = const_cast<App*>(this)->color_field_ptr(row, col);
        return p ? *p : "";
    }
    if (settings_tab_ == 1) {
        bool v = false;
        switch (row) {
            case 0: v = settings_.element_disk; break;
            case 1: v = settings_.element_dummy_buttons; break;
            case 2: v = settings_.element_queue; break;
            case 3: v = settings_.element_waveform; break;
            case 4: v = settings_.element_lyrics; break;
            case 5: v = settings_.element_lyrics_placeholder_ball; break;
            case 6: v = settings_.element_visualizer; break;
        }
        return v ? "true" : "false";
    }
    if (settings_tab_ == 2) {
        switch (row) {
            case 0: return std::to_string(settings_.visualizer_fluidity);
            case 1: return settings_.waveform_smooth ? "smooth" : "raw";
            case 2: return std::to_string(settings_.disk_rotation_speed).substr(0, 4);
            case 3: {
                static const char* names[] = {"list", "loop", "shuffle", "stop", "repeat queue"};
                return names[std::clamp(settings_.play_mode, 0, 4)];
            }
            case 4: return std::to_string(settings_.visualizer_degradation_speed);
            case 5: return std::to_string(settings_.visualizer_viscosity);
            case 6: return settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
            case 7: {
                static const char* names[] = {"full", "word by word", "letter by letter", "active line only", "active word only", "line by line"};
                return names[std::clamp(settings_.lyrics_animation, 0, 5)];
            }
        }
    }
    if (settings_tab_ == 3 && row >= 0 && row < kRefRowCount) {
        auto it = settings_.hotkeys.find(kRefHotkeyNames[row]);
        return it != settings_.hotkeys.end() ? it->second : "";
    }
    return "";
}

// Matches g_options: the fixed value lists that Left/Right cycles
// through. Empty return = not cyclable (Colors and Reference rows,
// exactly like the reference's g_options map has no entries for those).
std::vector<std::string> App::settings_options_for(int tab, int row) const {
    if (tab == 1) return {"true", "false"};
    if (tab == 2) {
        switch (row) {
            case 0: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 1: return {"raw", "smooth"};
            case 2: return {"0.01", "0.05", "0.10", "0.17", "0.25", "0.50", "0.75", "1.00"};
            case 3: return {"list", "loop", "shuffle", "stop", "repeat queue"};
            case 4: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 5: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 6: return {"left", "center", "right"};
            case 7: return {"full", "word by word", "line by line", "letter by letter", "active line only", "active word only"};
        }
    }
    return {};
}

// Matches g_config[k] = edit_buffer -- stored close to verbatim, no
// clamping. A bool/enum field that doesn't recognize the typed text
// just leaves the setting unchanged, since there's no way to store
// arbitrary text in a typed field the way the reference's string map can.
void App::settings_commit_edit() {
    const std::string& buf = color_edit_buffer_;
    auto to_lower = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };

    if (settings_tab_ == 0) {
        std::string* p = color_field_ptr(settings_row_, settings_col_);
        if (p) *p = buf;
    } else if (settings_tab_ == 1) {
        std::string v = to_lower(buf);
        bool is_true = (v == "true"), is_false = (v == "false");
        if (!is_true && !is_false) return;
        switch (settings_row_) {
            case 0: settings_.element_disk = is_true; break;
            case 1: settings_.element_dummy_buttons = is_true; break;
            case 2: settings_.element_queue = is_true; break;
            case 3: settings_.element_waveform = is_true; break;
            case 4: settings_.element_lyrics = is_true; break;
            case 5: settings_.element_lyrics_placeholder_ball = is_true; break;
            case 6: settings_.element_visualizer = is_true; break;
        }
    } else if (settings_tab_ == 2) {
        std::string v = to_lower(buf);
        switch (settings_row_) {
            case 0: try { settings_.visualizer_fluidity = std::stoi(buf); } catch (...) {} break;
            case 1: settings_.waveform_smooth = (v == "smooth"); break;
            case 2: try { settings_.disk_rotation_speed = std::stod(buf); } catch (...) {} break;
            case 3: settings_.play_mode = (v == "loop") ? 1 : (v == "shuffle") ? 2 : (v == "stop") ? 3 : (v == "repeat queue") ? 4 : 0; break;
            case 4: try { settings_.visualizer_degradation_speed = std::stoi(buf); } catch (...) {} break;
            case 5: try { settings_.visualizer_viscosity = std::stoi(buf); } catch (...) {} break;
            case 6: settings_.lyrics_alignment = (v == "left") ? 1 : (v == "right") ? 2 : 0; break;
            case 7:
                if (v == "word by word") settings_.lyrics_animation = 1;
                else if (v == "letter by letter") settings_.lyrics_animation = 2;
                else if (v == "active line only") settings_.lyrics_animation = 3;
                else if (v == "active word only") settings_.lyrics_animation = 4;
                else if (v == "line by line") settings_.lyrics_animation = 5;
                else settings_.lyrics_animation = 0;
                break;
        }
    } else if (settings_tab_ == 3 && settings_row_ >= 0 && settings_row_ < kRefRowCount) {
        settings_.hotkeys[kRefHotkeyNames[settings_row_]] = buf;
    }
}

// Matches cycle_option(): find the current value's index in its options
// list and step by `dir`, wrapping. No-op if this row has no options
// list at all.
void App::settings_cycle(int dir) {
    auto opts = settings_options_for(settings_tab_, settings_row_);
    if (opts.empty()) return;
    std::string cur = settings_get_value(settings_row_, 0);
    int idx = -1;
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i] == cur) { idx = static_cast<int>(i); break; }
    idx = (idx == -1) ? 0 : (idx + dir + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
    color_edit_buffer_ = opts[idx];
    settings_commit_edit();
    status_line_ = "TOGGLED -> " + opts[idx];
    if (settings_tab_ == 2 && settings_row_ == 1) recompute_waveform_for_current_track();
}

void App::handle_settings_key(int key) {
    if (mode_ == Mode::ColorEdit) {
        if (key == 27) { mode_ = Mode::Settings; return; } // cancel, discard buffer
        if (key == '\r' || key == '\n') {
            std::string key_name = (settings_tab_ == 3 && settings_row_ >= 0 && settings_row_ < kRefRowCount)
                                  ? kRefHotkeyNames[settings_row_] : "";
            // Hotkey overlap fix: if this is a Reference-tab hotkey being
            // rebound and the typed key is already owned by a different
            // action, reject the commit instead of silently creating a
            // collision where two actions fire on the same key. Clear the
            // buffer, force a redraw, and stay in ColorEdit mode so the
            // user can just repeat entry with a different key -- same
            // flow as a normal edit, just not accepted yet.
            if (!key_name.empty()) {
                std::string conflict = hotkey_conflict(color_edit_buffer_, key_name);
                if (!conflict.empty()) {
                    status_line_ = "KEY \"" + color_edit_buffer_ + "\" ALREADY USED BY " + conflict + " -- try another key";
                    color_edit_buffer_.clear();
                    force_redraw_ = true;
                    return; // stay in ColorEdit: redraw and repeat
                }
            }
            settings_commit_edit();
            status_line_ = key_name.empty() ? "UPDATED" : ("UPDATED " + key_name);
            mode_ = Mode::Settings;
            return;
        }
        if (key == 127 || key == 8) { if (!color_edit_buffer_.empty()) color_edit_buffer_.pop_back(); return; }
        if (key >= 32 && key < 127 && color_edit_buffer_.size() < 18) color_edit_buffer_ += static_cast<char>(key);
        return;
    }

    if (key == 9) { // Tab
        settings_tab_ = (settings_tab_ + 1) % kSettingsTabCount;
        settings_row_ = 0;
        settings_col_ = 0;
        return;
    }
    // Explicit spec from the user, overriding the reference's own
    // key semantics for this exact case (reference's 's' saves without
    // closing; here 's' saves AND exits, Esc/q just exits without saving).
    if (key == 's' || key == 'S') { save_settings(settings_); status_line_ = "SAVED"; mode_ = Mode::Browse; return; }
    if (key == 27 || key == 'q' || key == 'Q') {
        mode_ = Mode::Browse;
        return;
    }
    if (settings_tab_ == 4) {
        // About App: no fields to edit, but Up/Down still scroll the text.
        if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
        if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
        return;
    }

    if (key == '\r' || key == '\n') {
        if (settings_tab_ == 3 && settings_row_ >= kRefRowCount) return; // font-map rows are read-only display
        color_edit_buffer_ = settings_get_value(settings_row_, settings_col_);
        mode_ = Mode::ColorEdit;
        return;
    }
    if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
    if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
    if (key == 'C') { // right
        if (settings_tab_ == 0) { if (color_field_ptr(settings_row_, 1)) settings_col_ = 1; }
        else settings_cycle(1);
        return;
    }
    if (key == 'D') { // left
        if (settings_tab_ == 0) settings_col_ = 0;
        else settings_cycle(-1);
        return;
    }
}

void App::start_local_track(const LocalTrack& track) {
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    fs::path parent = track.path.parent_path().filename();
    launch_load_async(track.path, track.title, track.folder_artist == "-" ? "" : track.folder_artist,
                       parent.string() + "/", /*is_local=*/true, /*video_id=*/"");
}

void App::start_online_track(const OnlineResult& result) {
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    // Pass "" for artist so the lyrics search queries just the YouTube video title
    // (which usually contains "Artist - Song Name" perfectly), instead of appending the channel name.
    launch_load_async({}, result.title, "", "youtube", /*is_local=*/false, result.video_id);
}

// ---------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------

void App::handle_key(int key) {
    if (key == 0) return;

    if (mode_ == Mode::ColorEdit || mode_ == Mode::Settings) {
        handle_settings_key(key);
        return;
    }

    if (mode_ == Mode::Console) {
        // ESC, or the console hotkey again, closes it. Anything else is
        // ignored -- this is a read-only log view.
        if (key == 27 || key == 't' || key == 'T') mode_ = Mode::Browse;
        return;
    }

    if (mode_ == Mode::Cheatsheet) {
        if (key == 27 || key == '?') mode_ = Mode::Browse;
        return;
    }

    if (mode_ == Mode::BulkAdd) {
        if (key == 27) { // cancel entirely -- discard buffer, results, and selection state
            mode_ = Mode::Browse;
            status_line_.clear();
            bulk_add_buffer_.clear();
            bulk_add_results_ready_ = false;
            pending_bulk_add_ = BulkAddResult{};
            bulk_add_selected_.clear();
            return;
        }

        if (bulk_add_results_ready_) {
            // --- Phase 2: navigate/star the fetched checklist. ---
            int total = static_cast<int>(pending_bulk_add_.items.size());
            if (key == 'A' && total > 0) { // up
                bulk_add_cursor_ = std::max(0, bulk_add_cursor_ - 1);
                if (bulk_add_cursor_ < bulk_add_scroll_) bulk_add_scroll_ = bulk_add_cursor_;
                return;
            }
            if (key == 'B' && total > 0) { // down
                bulk_add_cursor_ = std::min(total - 1, bulk_add_cursor_ + 1);
                if (bulk_add_cursor_ >= bulk_add_scroll_ + kBulkAddVisibleRows) bulk_add_scroll_ = bulk_add_cursor_ - kBulkAddVisibleRows + 1;
                return;
            }
            if (key == ' ' && total > 0) { // toggle star on the hovered row
                if (bulk_add_cursor_ < static_cast<int>(bulk_add_selected_.size())) {
                    bulk_add_selected_[bulk_add_cursor_] = !bulk_add_selected_[bulk_add_cursor_];
                }
                return;
            }
            if (key == 'a') { commit_bulk_add(/*all=*/true); return; }        // "ALL"
            if (key == '\r' || key == '\n') { commit_bulk_add(/*all=*/false); return; } // "[SELECT]"
            return;
        }

        // --- Phase 1: typing the link. ---
        if (key == '\r' || key == '\n') {
            if (!bulk_add_buffer_.empty() && !bulk_add_in_progress_.load()) {
                status_line_ = "fetching playlist ...";
                launch_bulk_add_async(bulk_add_buffer_);
            }
            return; // stays open -- poll_pending_bulk_add() moves to phase 2 once the fetch resolves
        }
        if (key == 127 || key == 8) { if (!bulk_add_buffer_.empty()) bulk_add_buffer_.pop_back(); return; }
        if (key >= 32 && key < 127 && bulk_add_buffer_.size() < 200) bulk_add_buffer_ += static_cast<char>(key);
        return;
    }

    if (mode_ == Mode::RetryLyrics) {
        if (key == 27) { mode_ = Mode::Browse; return; } // cancel entirely, nothing submitted

        std::vector<RLField> fields = rl_visible_fields();
        auto cur_pos = std::find(fields.begin(), fields.end(), rl_focus_);
        int idx = (cur_pos != fields.end()) ? static_cast<int>(cur_pos - fields.begin()) : 0;

        if (key == 9 || key == 'B') { // Tab / down -- next field
            idx = (idx + 1) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            return;
        }
        if (key == 'A') { // up -- previous field
            idx = (idx - 1 + static_cast<int>(fields.size())) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            return;
        }
        if (key == '\r' || key == '\n') { rl_submit(); return; } // "enter to fetch", works from any field

        if (bool* b = rl_bool_ptr(rl_focus_)) {
            if (key == ' ') { // toggle -- checkboxes are the only thing Space does anything to
                bool new_val = !*b;
                // Enforce the radio group: turning one of
                // slowed/ultra-slowed/spedup on clears the other two.
                // Reverb/remix/other stay independent of this and of
                // each other.
                if (rl_focus_ == RLField::TypeSlowed || rl_focus_ == RLField::TypeUltraSlowed || rl_focus_ == RLField::TypeSpedup) {
                    rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = false;
                }
                *b = new_val;
                // Turning RemixText/OtherText off (unchecking) drops
                // them from rl_visible_fields() -- if focus was sitting
                // on one of those now-hidden text fields, land back on
                // the checkbox that just hid it instead of a field that
                // no longer exists in the nav order.
                return;
            }
            return; // typing/backspace do nothing on a checkbox field
        }

        if (std::string* t = rl_text_ptr(rl_focus_)) {
            if (key == 127 || key == 8) { if (!t->empty()) t->pop_back(); return; }
            if (key >= 32 && key < 127 && t->size() < 200) *t += static_cast<char>(key);
            return;
        }
        return;
    }

    if (mode_ == Mode::Search) {
        if (key == 27) {
            // Cancel: put the view back exactly as it was before '/' was
            // pressed, discarding whatever the live preview below was
            // showing.
            list_source_ = pre_search_list_source_;
            last_local_query_ = pre_search_local_query_;
            refresh_local_view();
            mode_ = Mode::Browse;
            return;
        }
        if (key == '\r' || key == '\n') { submit_search(); mode_ = Mode::Browse; return; }
        if (key == 127 || key == 8) {
            if (!search_buffer_.empty()) search_buffer_.pop_back();
            update_live_search_preview();
            return;
        }
        if (key >= 32 && key < 127) {
            search_buffer_ += static_cast<char>(key);
            update_live_search_preview();
            return;
        }
        return;
    }

    // Mode::Browse
    size_t list_len = current_list_len();

    switch (key) {
        case 's':
            mode_ = Mode::Settings;
            settings_tab_ = 0;
            settings_row_ = 0;
            settings_col_ = 0;
            break;
        case 'S': // Spotify view. Split off from Settings, which used to
                  // answer both cases -- config.txt has always documented
                  // the Settings key as lowercase "s" (HKeySetting="s"),
                  // so the uppercase half was a spare.
            enter_spotify();
            break;
        case 9: // Tab: toggle Up/Down + reorder focus between the list and the queue
            queue_focus_ = !queue_focus_;
            break;
        case 'A': // up
            if (queue_focus_) {
                if (queue_selected_ > 0) --queue_selected_;
                clamp_queue_selected();
            } else {
                if (selected_ > 0) --selected_;
                if (selected_ < scroll_) scroll_ = selected_;
            }
            break;
        case 'B': // down
            if (queue_focus_) {
                if (!queue_.empty() && queue_selected_ < static_cast<int>(queue_.size()) - 1) ++queue_selected_;
                clamp_queue_selected();
            } else {
                if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
                // BUGFIX: selection could move past the visible window without
                // the window ever following it, leaving the highlighted row
                // invisible below row 8 instead of the list scrolling up.
                if (selected_ >= scroll_ + list_visible_rows_) scroll_ = selected_ - list_visible_rows_ + 1;
            }
            break;
        case 'C': // right = seek forward
            // Seeking a Spotify track is a request to Spotify, not a
            // buffer offset -- and it needs an absolute target, so the
            // relative nudge is applied to the interpolated position.
            if (spotify_playing_ && spotify_) {
                // Applied locally first so the bar moves on this frame;
                // the request goes out on the worker. Bumping the
                // generation stops an in-flight poll from reporting the
                // pre-seek position and yanking the bar backwards.
                const long long target =
                    static_cast<long long>(std::max(0.0, spotify_elapsed_sec() + 5.0) * 1000.0);
                spotify_status_.player.position_ms = target;
                spotify_status_at_ = std::chrono::steady_clock::now();
                ++spotify_status_gen_;
                auto client = spotify_;
                spotify_run_command([this, client, target]() {
                    std::string err;
                    if (!client->seek(target, &err)) spotify_post_message(err);
                });
            } else if (has_track_) {
                player_.seek_relative(5.0);
            }
            break;
        case 'D': // left = seek back ... OR, while queue-focused, move the hovering queue item down.
            // Left-arrow and Shift+D are indistinguishable at the terminal-
            // input layer (see TerminalIO::poll_key) — reusing this case
            // for reordering while queue-focused means seeking is
            // unavailable during that time, but that's an acceptable
            // trade since you're not usually seeking while reordering a
            // queue anyway.
            if (queue_focus_) queue_move_hovering(1);
            else if (spotify_playing_ && spotify_) {
                const long long target =
                    static_cast<long long>(std::max(0.0, spotify_elapsed_sec() - 5.0) * 1000.0);
                spotify_status_.player.position_ms = target;
                spotify_status_at_ = std::chrono::steady_clock::now();
                ++spotify_status_gen_;
                auto client = spotify_;
                spotify_run_command([this, client, target]() {
                    std::string err;
                    if (!client->seek(target, &err)) spotify_post_message(err);
                });
            } else if (has_track_) {
                player_.seek_relative(-5.0);
            }
            break;
        case 'u': case 'U': // move the hovering queue item up (only meaningful once you've Tab'd into the queue)
            queue_move_hovering(-1);
            break;
        case 'p': case 'P': // play/pause
            if (spotify_playing_ && spotify_) {
                // Flip locally first so the bar stops/starts
                // interpolating on this frame rather than at the next
                // poll, and so the key feels instant despite the
                // round-trip behind it.
                const bool was_playing = (spotify_status_.player.state == "playing");
                spotify_status_.player.position_ms =
                    static_cast<long long>(spotify_elapsed_sec() * 1000.0);
                spotify_status_.player.state = was_playing ? "paused" : "playing";
                spotify_status_at_ = std::chrono::steady_clock::now();
                ++spotify_status_gen_;
                auto client = spotify_;
                spotify_run_command([this, client, was_playing]() {
                    std::string err;
                    const bool ok = was_playing ? client->pause(&err) : client->play(&err);
                    if (!ok) spotify_post_message(err.empty() ? "spotify refused that" : err);
                });
            } else if (has_track_) {
                if (player_.is_paused()) player_.resume(); else player_.pause();
            }
            break;
        case '1': // volume up
            if (spotify_playing_ && spotify_) {
                const int v = std::min(100, spotify_status_.player.volume + 5);
                spotify_status_.player.volume = v;
                ++spotify_status_gen_;
                auto client = spotify_;
                spotify_run_command([this, client, v]() {
                    std::string err;
                    if (!client->set_volume(v, &err)) spotify_post_message(err);
                });
            } else if (has_track_) {
                player_.set_volume(std::min(100, player_.volume() + 5));
            }
            break;
        case '2': // volume down
            if (spotify_playing_ && spotify_) {
                const int v = std::max(0, spotify_status_.player.volume - 5);
                spotify_status_.player.volume = v;
                ++spotify_status_gen_;
                auto client = spotify_;
                spotify_run_command([this, client, v]() {
                    std::string err;
                    if (!client->set_volume(v, &err)) spotify_post_message(err);
                });
            } else if (has_track_) {
                player_.set_volume(std::max(0, player_.volume() - 5));
            }
            break;
        case 'n': case 'N': // next -- the queue (if any) takes priority,
                             // same as auto-advance-on-finish does, and
                             // respects Shuffle/Repeat Queue via
                             // play_next_from_queue() (a manual skip
                             // still always actually skips, though --
                             // Repeat/Stop only govern *automatic*
                             // advance, not an explicit "n" press).
            // Spotify advances through its own queue, with its own
            // shuffle/repeat. mousiki just asks for the skip.
            if (spotify_playing_ && spotify_) {
                // No expected-uri here: mousiki doesn't know what
                // Spotify will advance to (its queue, its shuffle). The
                // generation bump is still needed so the pre-skip
                // reading is discarded, and the shortened sleep means
                // the new track shows up in about a second.
                ++spotify_status_gen_;
                status_line_ = "skipping ...";
                auto client = spotify_;
                spotify_run_command([this, client]() {
                    std::string err;
                    if (!client->next(&err)) spotify_post_message(err);
                });
            } else if (!queue_.empty()) {
                play_next_from_queue();
            } else {
                play_relative(1);
            }
            break;
        case 'b': // prev -- relative to what's actually playing (see
                  // current_track_list_index()), not the hover cursor.
                  // No queue equivalent: a FIFO queue has no well-defined
                  // "previous" once an item's been consumed.
            if (spotify_playing_ && spotify_) {
                ++spotify_status_gen_;
                status_line_ = "going back ...";
                auto client = spotify_;
                spotify_run_command([this, client]() {
                    std::string err;
                    if (!client->previous(&err)) spotify_post_message(err);
                });
            } else {
                play_relative(-1);
            }
            break;
        case 'a': // add hovering song to queue (List focus) -- or, when
                  // the Queue panel itself is focused, "a" has nothing
                  // hovering-in-the-list to add, so it opens the bulk-add
                  // panel instead (paste a YouTube playlist link, queue
                  // everything in it).
            if (queue_focus_) {
                mode_ = Mode::BulkAdd;
                bulk_add_buffer_.clear();
                bulk_add_results_ready_ = false;
                pending_bulk_add_ = BulkAddResult{};
                bulk_add_selected_.clear();
                bulk_add_cursor_ = 0;
                bulk_add_scroll_ = 0;
                status_line_.clear();
            } else {
                queue_add_selected();
                log_event("added to queue");
            }
            break;
        case 'd': // remove hovering queue item
            if (list_source_ == ListSource::Spotify) {
                // The Web API can append to the account queue but has
                // no reorder or remove, so there is nothing to call.
                status_line_ = "spotify's queue can't be edited from the api -- only added to";
            } else {
                queue_remove_hovering();
                log_event("removed from queue");
            }
            break;
        case 'm': case 'M': // cycle play mode: list -> repeat -> shuffle
                             // -> repeat queue -> stop -> list -- one key
                             // for all five instead of a separate toggle
                             // per mode.
            settings_.play_mode = (settings_.play_mode + 1) % 5;
            {
                // Indexed 0=list,1=repeat,2=shuffle,3=stop,4=repeat queue,
                // matching play_mode's own numbering (not cycle order).
                static const char* mode_names[] = {"list", "repeat", "shuffle", "stop", "repeat queue"};
                log_event(std::string("play mode: ") + mode_names[settings_.play_mode]);
            }
            break;
        case 'k': case 'K': // refresh ui -- force a full redraw, for when a
                             // resize or terminal-session switch raced the
                             // render loop and left a torn/stale frame on
                             // screen. hard_clear is normally only set on a
                             // detected width or mode change; this forces
                             // it once unconditionally on the very next
                             // frame.
            force_redraw_ = true;
            log_event("ui refreshed");
            break;
        case 't': // console -- overlay showing recent status/log events
            mode_ = Mode::Console;
            break;
        case 'x': case 'X': // mute -- force volume to 0 without touching pause state
            if (spotify_playing_ && spotify_) {
                const int target = muted_ ? pre_mute_volume_ : 0;
                if (!muted_) pre_mute_volume_ = spotify_status_.player.volume;
                spotify_status_.player.volume = target;
                muted_ = !muted_;
                log_event(muted_ ? "muted (spotify)" : "unmuted (spotify)");
                ++spotify_status_gen_;
                auto client = spotify_;
                spotify_run_command([this, client, target]() {
                    std::string err;
                    if (!client->set_volume(target, &err)) spotify_post_message(err);
                });
                break;
            }
            if (!muted_) {
                pre_mute_volume_ = player_.volume();
                player_.set_volume(0);
                muted_ = true;
                log_event("muted");
            } else {
                player_.set_volume(pre_mute_volume_);
                muted_ = false;
                log_event("unmuted");
            }
            break;
        case '?': // cheatsheet overlay
            mode_ = Mode::Cheatsheet;
            break;
        case 'f': case 'F': // filter local list to the hovering track's folder
            if (list_source_ == ListSource::Local && !local_view_.empty() &&
                selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
                folder_filter_ = local_view_[selected_].path.parent_path().string();
                refresh_local_view();
                log_event("filtered: " + fs::path(folder_filter_).filename().string());
            }
            break;
        case 'c': // clear folder filter
            if (!folder_filter_.empty()) {
                folder_filter_.clear();
                refresh_local_view();
                log_event("filter cleared");
            }
            break;
        case 'l': case 'L': // retry lyrics -- opens the manual title/artist override form
            if (has_track_) {
                rl_open_from_current_track();
                mode_ = Mode::RetryLyrics;
            }
            break;
        case 'w': case 'W': // toggle waveform style (raw/smooth) directly, without going into Settings
            settings_.waveform_smooth = !settings_.waveform_smooth;
            recompute_waveform_for_current_track();
            log_event(settings_.waveform_smooth ? "waveform: smooth" : "waveform: raw");
            break;
        case 'y': case 'Y': // save cached stream to local music path
            if (has_track_) {
                if (current_path_.string().find(".cache") != std::string::npos || metadata_.location == "youtube") {
                    std::string dest_dir;
                    if (!settings_.local_music_paths.empty()) {
                        dest_dir = settings_.local_music_paths[0];
                    } else {
                        const char* home = std::getenv("HOME");
                        dest_dir = home ? std::string(home) + "/Music" : "./Music";
                    }
                    std::error_code ec;
                    fs::create_directories(dest_dir, ec);
                    
                    std::string safe_name = metadata_.name;
                    for (char& c : safe_name) if (c == '/' || c == '\\') c = '_';
                    std::string safe_artist = (metadata_.artist == "-" ? "" : metadata_.artist);
                    for (char& c : safe_artist) if (c == '/' || c == '\\') c = '_';
                    
                    std::string filename = safe_artist.empty() ? safe_name : safe_name + " - " + safe_artist;
                    filename += current_path_.extension().string();
                    
                    fs::path dest_path = fs::path(dest_dir) / filename;
                    if (fs::exists(dest_path, ec)) {
                        status_line_ = "already saved: " + dest_path.filename().string();
                    } else {
                        fs::copy_file(current_path_, dest_path, fs::copy_options::overwrite_existing, ec);
                        if (!ec) {
                            fs::remove(current_path_, ec);
                            current_path_ = dest_path; // update so sidecar lyrics go to the new folder
                            metadata_.location = dest_dir;
                            status_line_ = "saved to " + dest_path.string();
                            refresh_local_view();
                        } else {
                            status_line_ = "failed to save: " + ec.message();
                        }
                    }
                } else {
                    status_line_ = "not a cached stream";
                }
            }
            break;
        case 'T': // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z)
            local_sort_mode_ = (local_sort_mode_ + 1) % 3;
            refresh_local_view();
            log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_));
            break;
        case '\r': case '\n':
            play_selected();
            break;
        case '/':
            mode_ = Mode::Search;
            search_buffer_.clear();
            pre_search_list_source_ = list_source_;
            pre_search_local_query_ = last_local_query_;
            break;
        case 27: // ESC -- back to the home view: full local library, no
                 // filter, from the top. Same destination regardless of
                 // how buried you are (mid search results, viewing
                 // online results, scrolled deep into the list).
            // In the Spotify view Esc first walks back up its own
            // hierarchy (tracks -> chooser -> home) and only falls
            // through to "leave Spotify" once it's at the top.
            if (list_source_ == ListSource::Spotify && spotify_back()) break;
            if (list_source_ == ListSource::Spotify && !spotify_playing_) {
                // Leaving Spotify with nothing playing there: stop the
                // heartbeat rather than polling the API forever in the
                // background. Requested rather than joined -- a join on a
                // keypress would stall the render thread for up to one
                // sleep slice.
                spotify_poll_stop_ = true;
            }
            list_source_ = ListSource::Local;
            last_local_query_.clear();
            folder_filter_.clear();
            refresh_local_view();
            status_line_.clear();
            break;
        case 'q': case 'Q':
            quit_ = true;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------
// Lazy metadata probing for whatever's currently visible in the list
// ---------------------------------------------------------------------

// This used to call probe_row_meta() here directly — which spawns a real
// ffprobe SUBPROCESS, synchronously, on the render thread, once per
// newly-visible row, every single frame a new row scrolled into view.
// Scroll through a big library quickly (or hit one slow/hanging file —
// weird encode, flaky storage) and the whole UI thread — rendering AND
// input — blocks for however long those subprocess calls take, with no
// way to even press 'q' to get out of it. That's what was crashing the
// whole player.
//
// Fix: use the native in-process binary-header parser (native_duration.h
// — pure pread() syscalls, no subprocess, effectively can't hang) for
// duration here, directly on the render thread — genuinely safe now.
// Anything that parser can't handle (unsupported format, corrupt file)
// gets picked up by a ONE-TIME background sweep (launch_row_meta_resolver,
// started once after the initial scan) that walks the whole library
// sequentially and falls back to ffprobe there — off the main thread
// entirely, never gating rendering or input.
void App::ensure_visible_row_meta() {
    if (list_source_ != ListSource::Local) return;
    for (int i = scroll_; i < std::min<int>(local_view_.size(), scroll_ + list_visible_rows_); ++i) {
        const auto& t = local_view_[i];
        std::string key = t.path.string();
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.count(key)) continue; // already resolved (native path or background sweep)
        }
        uint32_t dur = probe_duration_native(t.path);
        if (dur > 0) {
            RowMeta rm;
            rm.duration_sec = static_cast<double>(dur);
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            // Capped so the cache can't grow with the library: a 50k
            // track collection would otherwise accumulate tens of MB
            // that are never freed.
            if (row_meta_cache_.size() < 4096)
                row_meta_cache_[key] = rm;
        }
        // else: leave unresolved — native parsing is cheap enough to just
        // retry next frame, and the background sweep will fill it in via
        // ffprobe regardless, so there's no real cost to not caching a miss.
    }
}

// Re-runs quantization on the already-decoded PCM for whatever's
// currently playing — used when the smooth/raw toggle changes mid-track,
// so the effect shows up right away instead of only on the next track.
// No re-decode needed: current_pcm_ already holds everything decoded so
// far (streaming decode may still be filling it in, hence the acquire
// load of `available` rather than assuming it's complete).
void App::recompute_waveform_for_current_track() {
    if (!has_track_ || !current_pcm_) return;
    std::shared_ptr<StreamingPcm> pcm = current_pcm_;
    bool smooth = settings_.waveform_smooth;
    // Increment the epoch before spawning — any already-running
    // waveform thread will see its own epoch is stale and discard its
    // result instead of racing to overwrite pending_waveform_envelope_.
    int my_epoch = ++waveform_epoch_;
    std::thread([this, pcm, smooth, my_epoch]() {
        size_t n = pcm->available.load(std::memory_order_acquire);
        if (n == 0) return;
        std::vector<float> snapshot(pcm->data.begin(), pcm->data.begin() + static_cast<long>(n));
        auto envelope = WaveformQuantizer::generate_high_res_envelope(snapshot, 4096, smooth);
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        if (my_epoch != waveform_epoch_.load()) return; // superseded — discard
        pending_waveform_envelope_ = std::move(envelope);
        waveform_pending_ready_ = true;
    }).detach();
}

void App::launch_row_meta_resolver() {
    if (row_meta_resolver_started_.exchange(true)) return; // only ever runs once per app session
    std::vector<fs::path> paths;
    paths.reserve(all_local_tracks_.size());
    for (auto& t : all_local_tracks_) paths.push_back(t.path);

    // Detached, not tracked-and-joined: a whole-library ffprobe sweep
    // could take a while on a big collection, and joining it at shutdown
    // would just trade one flavor of "blocked and can't do anything" for
    // another. Same tradeoff already accepted for decode threads —
    // worst case on quit is one orphaned ffprobe call, not a crash.
    std::thread([this, paths]() {
        for (auto& p : paths) {
            std::string key = p.string();
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                if (row_meta_cache_.count(key)) continue; // native parse (or an earlier pass) already got it
            }
            RowMeta rm = probe_row_meta(p); // ffprobe fallback — slow, but background-thread-only now
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.size() < 4096) // same cap as ensure_visible_row_meta
                row_meta_cache_[key] = rm;
        }
    }).detach();
}

// ---------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------

// Paints one disk row a cell at a time from the cover's own colours.
//
// Two things keep this from flooding the terminal. An escape sequence is
// only emitted when the colour actually changes from the previous cell,
// and a blank cell doesn't change it at all -- there is nothing to paint,
// and the mask leaves a lot of blank cells outside the circle. Covers are
// smooth at this scale, so a row collapses to a handful of runs. Emitting
// all 30 unconditionally would put ~10KB per frame down the pipe 30 times
// a second, which is fine locally and visibly laggy over ssh.
static std::string colorize_disk_row(const std::string& cells,
                                     const std::vector<DiskRgb>& colors) {
    // U+2800, the all-dots-off Braille cell, as UTF-8.
    static const char kBlank[3] = {'\xe2', '\xa0', '\x80'};

    std::string out;
    out.reserve(cells.size() * 2);
    DiskRgb last{};
    bool have_last = false;
    size_t cell = 0;
    // encodeBraille() always emits exactly three bytes per cell (the
    // U+28xx block is three bytes in UTF-8), so stepping by three walks
    // cells without needing to decode anything.
    for (size_t i = 0; i + 2 < cells.size() && cell < colors.size(); i += 3, ++cell) {
        const bool blank = cells.compare(i, 3, kBlank, 3) == 0;
        if (!blank) {
            const DiskRgb& c = colors[cell];
            if (!have_last || c != last) {
                out += "\x1b[38;2;" + std::to_string(c.r) + ";" +
                       std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
                last = c;
                have_last = true;
            }
        }
        out.append(cells, i, 3);
    }
    out += "\x1b[0m";
    return out;
}

// Decides whether this frame renders compact, from the real terminal
// height. Called once per frame before any panel is built.
//
// Deliberately height-only, with no host sniffing: a VS Code panel
// dragged tall deserves the full layout, and a small window in any other
// terminal has exactly the same problem a VS Code panel does. The two
// thresholds differ so that dragging a terminal across the boundary
// settles instead of flickering -- once compact, it takes a clear two
// extra rows to go back.
void App::update_compact_mode() {
    if (settings_.compact_mode == 1) { compact_ = true;  return; }
    if (settings_.compact_mode == 2) { compact_ = false; return; }
    if (compact_) {
        if (term_rows_ >= kCompactLeaveRows) compact_ = false;
    } else {
        if (term_rows_ < kCompactEnterRows) compact_ = true;
    }
}

std::vector<std::string> App::build_metadata_panel(int total_width) const {
    const int inner = total_width - 4;
    // The disk is what makes this panel 15 rows tall, so compact has to
    // drop it rather than just narrow it -- panel_h is the disk's height
    // in the full layout, and trimming the text rows alone would change
    // nothing.
    const bool show_disk = settings_.element_disk && !compact_;
    const int disk_w = show_disk ? disk_.width() : 0;
    const int panel_h = compact_ ? kCompactMetaRows : disk_.height();
    
    std::string sep = "  " + settings_.meta_separator + "  ";
    int sep_w = display_width(sep);
    // Without the disk there is no ":  :" separator between the metadata
    // and lyrics columns, and the two columns end up touching -- a long
    // title runs straight into the lyrics text and reads as one line.
    // Compact reserves 4: two for the left indent, two for a gutter
    // between the columns. Taken out of `avail` below so the total width
    // still adds up.
    const int fixed_extra = show_disk ? sep_w : (compact_ ? 4 : 2);

    int avail = std::max(10, inner - disk_w - fixed_extra);
    int meta_w = avail;
    int lyrics_w = 0;
    if (settings_.element_lyrics) {
        meta_w = std::min(42, std::max(10, avail - 10));
        meta_w = std::min(meta_w, avail);
        lyrics_w = std::max(0, avail - meta_w);
    }

    std::vector<std::string> disk_frame;
    if (show_disk) {
        // cell_colors comes back empty unless a cover is actually on the
        // disk, which is what makes the vinyl keep the configured
        // gradient without a second branch down here.
        std::vector<std::vector<DiskRgb>> cell_colors;
        const bool want_cover_color = settings_.disk_album_art && settings_.disk_album_color;
        disk_frame = disk_.frame(angle_, want_cover_color ? &cell_colors : nullptr);
        while (static_cast<int>(disk_frame.size()) < panel_h) disk_frame.emplace_back(std::string(disk_w, ' '));
        for (size_t row_i = 0; row_i < disk_frame.size(); ++row_i) {
            if (row_i < cell_colors.size()) {
                disk_frame[row_i] = colorize_disk_row(disk_frame[row_i], cell_colors[row_i]);
                continue;
            }
            float t = disk_frame.size() > 1 ? static_cast<float>(row_i) / static_cast<float>(disk_frame.size() - 1) : 0.0f;
            std::string disk_end = settings_.disk_color_end;
            std::string row_ansi = gradient_ansi(settings_.disk_color, disk_end, t);
            disk_frame[row_i] = row_ansi + disk_frame[row_i] + "\x1b[0m";
        }
    }

    // display_elapsed_sec() rather than player_ directly: while Spotify
    // owns playback, player_ isn't playing anything and the real
    // position comes from the polled Spotify state, interpolated forward
    // between polls (see spotify_elapsed_sec()).
    double elapsed = display_elapsed_sec();
    fft_.set_fluidity(settings_.visualizer_fluidity);
    fft_.set_degradation_speed(settings_.visualizer_degradation_speed);
    fft_.set_viscosity(settings_.visualizer_viscosity);

    // meta content rows
    std::vector<std::string> meta_rows(panel_h, std::string());
    bool viz_rows_colored = false;
    std::vector<int> bars; // computed once below, reused by the sphere visualizer fallback further down
    // showing_track(), not has_track_: a Spotify track is playing even
    // though mousiki's own player is idle. The spectrum strip still
    // renders, with its bars at zero -- fft_ is fed from the audio
    // callback, and Spotify's decoding produces none.
    if (showing_track()) {
        std::string k_col = settings_.meta_key_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_key_color);
        std::string v_col = settings_.meta_val_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_val_color);
        auto kv = [&](int row, const std::string& label, const std::string& value) {
            std::string mapped_label = apply_font_map(label, settings_.font_map);
            std::string mapped_val = apply_font_map(value, settings_.font_map);
            int avail_v = std::max(0, meta_w - 12);
            std::string l_pad = pad_right(mapped_label, 10);
            std::string v_tr = truncate_str(mapped_val, avail_v);
            std::string plain = l_pad + ": " + v_tr;
            std::string ansi = k_col + l_pad + "\x1b[0m" + ": " + v_col + v_tr + "\x1b[0m";
            ansi += std::string(std::max(0, meta_w - display_width(plain)), ' ');
            // Guarded because panel_h is no longer a constant: compact
            // shrinks it to kCompactMetaRows, and a row index past the
            // end would be a buffer overrun rather than a visual bug.
            if (row >= 0 && row < static_cast<int>(meta_rows.size())) meta_rows[row] = ansi;
        };
        if (compact_) {
            // Four rows, chosen for what you still want at a glance when
            // there is no room: what is playing, by whom, off what, and
            // where the sound is coming out. The dropped rows (sampling,
            // type, format, file size) describe the file, which is the
            // least useful thing on screen while something is playing --
            // and is meaningless outright on the Spotify path, where
            // mousiki never sees a file.
            kv(0, "Name", metadata_.name);
            kv(1, "Artist", metadata_.artist);
            // Prefer the extra row (album, on the Spotify path) over the
            // year, which is usually "-" for anything not locally tagged.
            if (!metadata_.extra_label.empty()) kv(2, metadata_.extra_label, metadata_.extra_value);
            else                                kv(2, "year", metadata_.year);
            kv(3, "location", metadata_.location);
        } else {
            kv(1, "Name", metadata_.name);
            kv(2, "Artist", metadata_.artist);
            kv(3, "year", metadata_.year);
            kv(4, "sampling", metadata_.sampling);
            kv(5, "type", metadata_.type);
            kv(6, "format", metadata_.format);
            kv(7, "file size", metadata_.file_size);
            kv(8, "location", metadata_.location);
            if (!metadata_.extra_label.empty()) kv(9, metadata_.extra_label, metadata_.extra_value);
        }

        // Real spectrum visualizer (KISS FFT), not a copy of the progress
        // bar's RMS envelope. Two rows: bottom row is the base level
        // (0-4), top row is whatever's left over above that (0-4) so
        // taller peaks build upward — attached to the panel's bottom row
        // per instruction, with the second row directly above it.
        if (settings_.element_visualizer) {
            int viz_w = std::min(meta_w, 48);
            bars = fft_.compute_bars(viz_w, viz_dt_);
            std::string viz_top, viz_bottom;
            int nbars = static_cast<int>(bars.size());
            for (int i = 0; i < nbars; ++i) {
                int level = bars[i];
                float t = nbars > 1 ? static_cast<float>(i) / static_cast<float>(nbars - 1) : 0.0f;
                std::string bar_ansi;
                if (!settings_.viz_center_color.empty()) {
                    bar_ansi = multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t);
                } else {
                    bar_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                }
                viz_bottom += bar_ansi;
                viz_bottom += fft_glyph(std::min(level, 4));
                viz_top += bar_ansi;
                viz_top += fft_glyph(std::max(0, level - 4));
            }
            viz_top += "\x1b[0m";
            viz_bottom += "\x1b[0m";
            if (viz_w < meta_w) {
                std::string tail(meta_w - viz_w, ' ');
                viz_top += tail;
                viz_bottom += tail;
            }
            // Compact keeps only the base row: two rows out of five is
            // too much of the panel, and the upper row only carries the
            // peaks above level 4.
            if (!compact_ && panel_h >= 2) meta_rows[panel_h - 2] = viz_top;
            if (panel_h >= 1) meta_rows[panel_h - 1] = viz_bottom;
            viz_rows_colored = true;
        } else {
            bars = fft_.compute_bars(48, viz_dt_);
        }
    } else {
        meta_rows[0] = "no track loaded - press / to search, Enter to play";
    }
    for (int i = 0; i < static_cast<int>(meta_rows.size()); ++i) {
        if (meta_rows[i].empty()) {
            meta_rows[i] = std::string(meta_w, ' ');
        } else if (!showing_track() && i == 0) {
            meta_rows[i] = pad_right(meta_rows[i], meta_w);
        }
        // all other rows (kv data, visualizer) are already perfectly padded
        // by their respective builders, and padding them again would miscount 
        // their ANSI color escapes as visible columns, truncating them.
    }

    // lyrics window: word-wrapped, center-aligned, word-level highlight on
    // the active line — windowed so the active line's wrapped block is
    // always vertically centered, blank-padded at the edges.
    std::vector<std::string> lyric_rows(panel_h, std::string(lyrics_w, ' '));
    bool lyrics_avail = false;
    std::vector<LyricLine> lines_copy;
    std::string lyrics_status;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_ready_) {
            lines_copy = lyrics_result_.lines;
            lyrics_status = lyrics_result_.message;
            lyrics_avail = !lines_copy.empty();
        } else if (showing_track()) {
            lyrics_status = "fetching lyrics ...";
        }
    }

    if (!lyrics_avail) {
        // A status message ("fetching...", "no lyrics found", etc.) is
        // only shown for the first 10s after it appears — after that the
        // sphere gets the whole panel to itself instead of a permanently
        // stuck caption line. Each distinct message content gets its own
        // fresh 10s window (so "fetching..." showing, then later
        // changing to "no lyrics found", each get their moment) rather
        // than one timer for the whole track.
        if (lyrics_status != last_lyrics_status_) {
            last_lyrics_status_ = lyrics_status;
            lyrics_status_shown_at_ = std::chrono::steady_clock::now();
        }
        double status_age = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lyrics_status_shown_at_).count();
        bool show_caption = status_age < 10.0 && !lyrics_status.empty();

        if (showing_track() && lyrics_w >= 6 && panel_h >= 3 && settings_.element_lyrics_placeholder_ball) {
            // Fill the panel with the audio-reactive sphere instead of
            // leaving it blank — reuses `bars` (already computed for the
            // main spectrum strip above, same frame) rather than running
            // a second independent audio analysis.
            int sphere_rows_h = show_caption ? panel_h - 1 : panel_h;
            auto sphere_rows = sphere_.render(lyrics_w, sphere_rows_h, bars, viz_dt_);
            std::string sphere_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
            for (int i = 0; i < static_cast<int>(sphere_rows.size()) && i < panel_h; ++i) {
                lyric_rows[i] = sphere_ansi + pad_right(sphere_rows[i], lyrics_w) + "\x1b[0m";
            }
            if (show_caption) {
                int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
                lyric_rows[panel_h - 1] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
            }
        } else if (show_caption) {
            int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
            lyric_rows[0] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
        }
    } else {
        int active = 0;
        for (size_t i = 0; i < lines_copy.size(); ++i) {
            if (lines_copy[i].start_time <= elapsed) active = static_cast<int>(i);
            else break;
        }

        if (settings_.lyrics_animation == 4) {
            // Only active word: show nothing but whichever single word is
            // currently being sung, centered alone in the panel. Falls
            // back to the whole line if this song has no word-level sync
            // data at all (nothing finer to show).
            const LyricLine& al = lines_copy[active];
            std::string word = al.full_text;
            if (!al.words.empty()) {
                word = al.words.front().second;
                for (const auto& wt : al.words) if (wt.first <= elapsed) word = wt.second; // last one <= elapsed
            }
            word = apply_font_map(word, settings_.font_map);
            int pad = std::max(0, (lyrics_w - display_width(word)) / 2);
            std::string line = std::string(pad, ' ') + word;
            std::string colored = ansi_for(settings_.active_word_color) + pad_right(truncate_str(line, lyrics_w), lyrics_w) + "\x1b[0m";
            lyric_rows[panel_h / 2] = colored;
        } else if (settings_.lyrics_animation == 3) {
            // Only active line: same per-word rendering as the default
            // view, just without the scrolling context lines around it.
            auto wrapped = render_lyric_line_wrapped(lines_copy[active], elapsed, lyrics_w, true, settings_);
            int start_row = std::max(0, (panel_h - static_cast<int>(wrapped.size())) / 2);
            for (size_t i = 0; i < wrapped.size() && start_row + static_cast<int>(i) < panel_h; ++i) {
                lyric_rows[start_row + i] = wrapped[i];
            }
        } else {
            // Full (default) and Word-by-word/Letter-by-letter (which only
            // change render_lyric_line_wrapped's *content*, not this
            // scrolling layout) all share the same multi-line context view.
            //
            // Only wrap lines actually near the visible window — wrapping
            // the whole song every frame would be wasted work.
            int context = 6;
            int lo = std::max(0, active - context);
            int hi = std::min(static_cast<int>(lines_copy.size()) - 1, active + context);

            std::vector<std::string> flat_rows;
            int active_row_start = 0, active_row_count = 1;
            for (int li = lo; li <= hi; ++li) {
                auto wrapped = render_lyric_line_wrapped(lines_copy[li], elapsed, lyrics_w, li == active, settings_);
                if (li == active) {
                    active_row_start = static_cast<int>(flat_rows.size());
                    active_row_count = static_cast<int>(wrapped.size());
                }
                for (auto& r : wrapped) flat_rows.push_back(std::move(r));
            }

            int active_mid = active_row_start + active_row_count / 2;
            int start = active_mid - panel_h / 2;
            for (int row = 0; row < panel_h; ++row) {
                int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat_rows.size())) lyric_rows[row] = flat_rows[idx];
            }
        }
    }

    // --- assemble bordered block ---
    // NOTE: lyric_rows may contain ANSI color codes (word-highlighting),
    // so this assembles rows by direct concatenation of pre-padded pieces
    // rather than routing through box_line()/pad_right() — those count
    // UTF-8 codepoints for width, and ANSI escape bytes would be
    // miscounted as visible columns, throwing off alignment. Every piece
    // here (disk_frame/meta_rows/lyric_rows) is already padded to its own
    // exact width, so the concatenation is guaranteed to equal `inner`.
    std::vector<std::string> out;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    out.push_back(box_top("", total_width, border_ansi));

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string sep_ansi = ansi_for(settings_.border_color, false) + sep + "\x1b[0m";
    for (int row = 0; row < panel_h; ++row) {
        std::string content = "";
        // show_disk, NOT settings_.element_disk: compact leaves the
        // setting alone and simply doesn't build disk_frame, so keying
        // this off the setting indexes an empty vector.
        if (show_disk) {
            content += disk_frame[row] + sep_ansi;
        } else {
            content += "  ";
        }
        content += meta_rows[row];
        // The gutter reserved by fixed_extra above. Only in compact: the
        // full layout gets its separation from sep_ansi after the disk.
        if (compact_ && settings_.element_lyrics && lyrics_w > 0) content += "  ";
        if (settings_.element_lyrics) {
            content += lyric_rows[row];
        }
        out.push_back(bar + " " + content + " " + bar);
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_progress_panel(int total_width) const {
    const int button_content_w = 9;
    const int button_total_w = button_content_w + 4; // "│ X │"
    // The <<< PLAY >>> buttons are decorative and cost three rows of
    // structure plus a third of the width; compact has neither to spare.
    const bool show_buttons = settings_.element_dummy_buttons && !compact_;
    int side_panel_w = show_buttons ? (button_total_w * 3) : 38;
    int main_total_w = std::max(24, total_width - side_panel_w);
    int wave_w = main_total_w - 4;

    double elapsed = display_elapsed_sec();
    const size_t shown_total_sec = display_total_sec();
    int active_cols = (shown_total_sec > 0) ? static_cast<int>((elapsed / static_cast<double>(shown_total_sec)) * wave_w) : 0;
    active_cols = std::clamp(active_cols, 0, wave_w);

    int reveal_cols = wave_w;
    if (waveform_ready_) {
        double reveal_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - waveform_reveal_start_).count();
        if (reveal_sec < 0.7) {
            reveal_cols = static_cast<int>((reveal_sec / 0.7) * wave_w);
        }
    } else {
        reveal_cols = 0;
    }

    std::string color_played = ansi_for(settings_.progress_played_color);
    std::string color_unplayed = ansi_for(settings_.progress_remaining_color);
    std::string color_reset = "\x1b[0m";

    std::string top_wave, mid_wave, bot_wave;
    std::vector<int> waveform_levels;
    if (!waveform_envelope_.empty()) {
        waveform_levels = WaveformQuantizer::resample_for_ui(waveform_envelope_, wave_w);
    } else {
        waveform_levels.assign(wave_w, 0);
    }

    for (int i = 0; i < wave_w; ++i) {
        bool revealed = i < reveal_cols;
        int level = (revealed && i < static_cast<int>(waveform_levels.size())) ? waveform_levels[i] : 0;
        BrailleColumn col = WaveformQuantizer::get_column(level);
        if (i == 0) {
            std::string c = (i < active_cols) ? color_played : color_unplayed;
            top_wave += c; mid_wave += c; bot_wave += c;
        } else if (i == active_cols) {
            top_wave += color_unplayed; mid_wave += color_unplayed; bot_wave += color_unplayed;
        }
        top_wave += col.top; mid_wave += col.mid; bot_wave += col.bot;
    }
    top_wave += color_reset; mid_wave += color_reset; bot_wave += color_reset;

    // Generic fallback bar for when the waveform element is switched
    // off: a plain "[#####-------]" fill on the middle row, blank above
    // and below it, instead of leaving the panel showing a stray braille
    // waveform on two of its three rows (top_wave/mid_wave used to render
    // unconditionally regardless of this setting -- only the bottom row
    // respected the toggle, which is the "turning off waveform doesn't
    // actually turn it off" bug).
    std::string generic_blank(wave_w, ' ');
    std::string generic_bar;
    if (!settings_.element_waveform) {
        int inner_w = std::max(0, wave_w - 2); // account for the '[' and ']'
        int filled = (shown_total_sec > 0) ? static_cast<int>((elapsed / static_cast<double>(shown_total_sec)) * inner_w) : 0;
        filled = std::clamp(filled, 0, inner_w);
        generic_bar = "[" + color_played + std::string(filled, '#') + color_reset
                     + color_unplayed + std::string(inner_w - filled, '-') + color_reset + "]";
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    // Buttons (<<< PLAY >>>) and the volume bar now match the border
    // color rather than the separate (and, for these two elements,
    // effectively unused/inert) button_color field.
    std::string button_ansi = border_ansi;

    auto button_mid = [&](const std::string& text) {
        std::string centered = button_ansi + center_pad(text, button_content_w) + "\x1b[0m";
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        return bar + " " + centered + " " + bar;
    };
    // Reflects whichever player actually owns playback: for Spotify that
    // is the polled remote state, not player_'s (which is stopped).
    bool paused_now = spotify_playing_ ? (spotify_status_.player.state != "playing")
                                       : player_.is_paused();
    std::string play_label = (showing_track() && !paused_now) ? "PAUSE" : "PLAY";

    std::vector<std::string> out;
    if (show_buttons) {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi) + box_top("", button_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi));
    } else {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi));
    }
    
    std::string row1_content = settings_.element_waveform ? top_wave : generic_blank;
    std::string row2_content = settings_.element_waveform ? mid_wave : generic_bar;
    std::string row3_content = settings_.element_waveform ? bot_wave : generic_blank;
    // Compact renders the middle row only. It is the one carrying the
    // played/unplayed colour split, so the bar still reads as a progress
    // bar with the other two gone.

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    if (show_buttons) {
        out.push_back(bar + " " + row1_content + " " + bar
                      + button_mid("<<<") + button_mid(play_label) + button_mid(">>>"));
        out.push_back(bar + " " + row2_content + " " + bar
                      + box_bottom(button_total_w, "", border_ansi_bottom) + box_bottom(button_total_w, "", border_ansi_bottom)
                      + box_bottom(button_total_w, "", border_ansi_bottom));
    } else if (compact_) {
        out.push_back(bar + " " + row2_content + " " + bar);
    } else {
        out.push_back(bar + " " + row1_content + " " + bar);
        out.push_back(bar + " " + row2_content + " " + bar);
    }

    // Spotify's volume lives on the remote device, so it comes from the
    // polled state rather than from mousiki's own output stage.
    int vol = spotify_playing_ ? spotify_status_.player.volume : player_.volume();
    int vol_hashes = (vol * 20) / 100;
    // "#" (filled) matches the waveform's played color, "-" (empty)
    // matches its unplayed/remaining color -- color_played/color_unplayed
    // are already computed above from progress_played_color/
    // progress_remaining_color for the waveform itself, reused here so
    // the volume bar visually reads as the same kind of fill. The
    // "VOLUME BAR:[...]" label and brackets use the border color, same
    // as the buttons above.
    std::string vol_bar_colored = color_played + std::string(vol_hashes, '#') + "\x1b[0m"
                                 + color_unplayed + std::string(20 - vol_hashes, '-') + "\x1b[0m";
    std::string vol_bar_text = std::string(vol_hashes, '#') + std::string(20 - vol_hashes, '-'); // plain, for width math only
    std::string vol_tail = border_ansi + "VOLUME BAR:[" + "\x1b[0m" + vol_bar_colored
                          + border_ansi + "]" + "\x1b[0m" + " " + std::to_string(vol) + "%";
    int side_w = total_width - main_total_w;
    // pad_left counts raw bytes, so it can't be used once vol_tail carries
    // ANSI bytes -- pad by the *visible* width instead (the volume-bar
    // color fix below is what introduced the mismatch).
    int visible_w = display_width("VOLUME BAR:[" + vol_bar_text + "] " + std::to_string(vol) + "%");
    int gap = std::max(0, side_w - visible_w);

    if (!settings_.autosave_enabled) {
        // AutoSave fully off: no indicator, and the blank gap moves to
        // the *right* of the text instead of sitting between the
        // border and it -- "│VOLUME BAR" touching the border directly,
        // same total line width either way so nothing else has to
        // change to stay aligned.
        if (gap > 0) vol_tail = vol_tail + std::string(gap, ' ');
    } else {
        // AutoSave on: keep the existing gap (│  VOLUME BAR), but let
        // the indicator glyph occupy the first character of it when
        // enabled -- │• VOLUME BAR with room to spare, or │•VOLUME BAR
        // once the gap is down to exactly one column.
        std::string pad(gap, ' ');
        if (gap >= 1) {
            std::string glyph = autosave_indicator_glyph();
            if (!glyph.empty()) pad = glyph + pad.substr(1);
        }
        vol_tail = pad + vol_tail;
    }

    std::string time_plain = "[ " + fmt_mmss(elapsed) + " ]" + settings_.box_horizontal + "[ " + fmt_mmss(static_cast<double>(shown_total_sec)) + " ]";
    // Manual box-bottom construction (rather than the shared box_bottom()
    // helper) so the timestamp text can carry its own color: box_bottom()
    // measures/pads the footer as plain text, and embedding ANSI bytes
    // into that path would get miscounted as visible columns.
    std::string ts_ansi = ansi_for(settings_.progress_timestamp_color.empty() ? settings_.border_color : settings_.progress_timestamp_color, false);
    std::string time_colored = ts_ansi + time_plain + "\x1b[0m";
    std::string prefix_plain = settings_.box_lower_left + settings_.box_horizontal + " " + time_plain + " ";
    int used = display_width(prefix_plain);
    int dashes_n = std::max(0, main_total_w - used - 1);
    std::string bottom_line = border_ansi_bottom + settings_.box_lower_left + settings_.box_horizontal + " "
                             + time_colored + border_ansi_bottom + " "; // re-apply border color -- time_colored's own reset above would otherwise leave the rest of this line uncolored
    for (int i = 0; i < dashes_n; ++i) bottom_line += settings_.box_horizontal;
    bottom_line += settings_.box_lower_right;
    bottom_line += "\x1b[0m";
    out.push_back(bar + " " + row3_content + " " + bar + vol_tail);
    out.push_back(bottom_line);
    return out;
}
std::vector<std::string> App::build_search_bar(int total_width) const {
    std::string label = "SEARCH LOCAL";
    if (list_source_ == ListSource::Online) label = "SEARCH ONLINE";
    else if (list_source_ == ListSource::Spotify) label = "SEARCH SPOTIFY";

    std::string content;
    if (mode_ == Mode::Search) {
        content = "/" + search_buffer_ + "\u2588"; // block cursor
    } else if (list_source_ == ListSource::Online) {
        content = "/s:" + last_online_query_;
    } else if (list_source_ == ListSource::Spotify) {
        content = std::string("/") + kSpotifySearchPrefix + spotify_search_query_;
    } else {
        content = "/l:" + last_local_query_;
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    int search_w = total_width - 5;
    out.push_back(box_top(label, search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
    // Play-mode indicator: L=list, R=repeat, S=shuffle, Q=repeat queue,
    // O=stop -- one letter for whichever of the five settings_.play_mode
    // states is active, cycled with a single "m" press
    // (HKeyCyclePlayMode) rather than a separate toggle per mode.
    std::string mode_letter(1, play_mode_letter());
    out.push_back(box_line(content, search_w, border_ansi) + border_ansi + settings_.box_vertical
                  + " " + mode_letter + " " + settings_.box_vertical + "\x1b[0m");
    out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    return out;
}

std::vector<std::string> App::build_list_panel(int total_width, int height) const {
    const bool online = (list_source_ == ListSource::Online);
    const bool spotify = (list_source_ == ListSource::Spotify);
    int inner = total_width - 4;

    // Spotify rows are built up front by level rather than inline in the
    // loop below: five levels with different columns (a track has a
    // duration, a playlist a count, an album a year) don't fit the
    // two-branch shape the loop was written around.
    std::vector<std::string> spotify_rows;
    // False while the panel is showing an explanation instead of rows
    // (not signed in, wrong backend) -- there is nothing to put a cursor
    // on, and highlighting a sentence reads as a selectable item.
    bool rows_selectable = true;
    if (spotify) {
        spotify_rows = build_spotify_list_rows(inner);
        rows_selectable = (spotify_level_len() > 0);
    }

    std::string label;
    if (spotify) {
        label = "SPOTIFY";
        if (spotify_level_ == SpotifyLevel::Home) {
            label = rows_selectable ? "SPOTIFY HOME" : "SPOTIFY \u2014 NOT AVAILABLE";
        } else if (!spotify_list_label_.empty()) {
            label = "SPOTIFY \u00b7 " + spotify_list_label_;
        }
    } else if (online) {
        label = "ONLINE RESULTS";
    } else {
        label = "LOCAL AUDIO FILES (sort: " + std::string(sort_mode_name(local_sort_mode_)) + ")";
    }

    size_t total = spotify ? spotify_rows.size()
                           : (online ? online_view_.size() : local_view_.size());
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);

    std::vector<std::string> out;
    out.push_back(box_top(label, total_width, border_ansi));

    int now_playing_idx = current_track_list_index(); // computed once -- O(list_len), not per-row

    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll_ + row;
        std::string content;
        if (idx < static_cast<int>(total)) {
            if (spotify) {
                content = spotify_rows[idx];
            } else if (online) {
                const auto& r = online_view_[idx];
                const int uploader_w = 18;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - uploader_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(r.title, settings_.font_map);
                std::string t_uploader = apply_font_map(r.uploader, settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_uploader, uploader_w), uploader_w);
            } else {
                const auto& t = local_view_[idx];
                const int artist_w = 16;
                const int dur_w = 5;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - dur_w);
                double dur = -1;
                // BUGFIX: this was always the parent-folder name, even
                // though the metadata panel already reads the real ffprobe
                // artist tag for the loaded track — now the list uses that
                // same real tag (probed lazily for visible rows), falling
                // back to the folder guess only until it's been probed.
                std::string artist = t.folder_artist;
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(t.path.string());
                    if (it != row_meta_cache_.end()) {
                        dur = it->second.duration_sec;
                        if (!it->second.artist.empty()) artist = it->second.artist;
                    }
                }
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(t.title, settings_.font_map);
                std::string t_artist = apply_font_map(artist, settings_.font_map);
                std::string t_dur = apply_font_map(fmt_mmss(dur), settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_artist, artist_w), artist_w) + settings_.list_separator + " "
                        + t_dur;
            }
        }
        bool sel = rows_selectable && (idx == selected_) && idx < static_cast<int>(total);
        // Was local-only ("!online && ..."), so a playing *online* track
        // never highlighted in its own results list at all -- fixed to
        // use the same source-aware identity match current_track_list_
        // index() already does (path for local, video_id for online).
        bool is_playing_row = (idx == now_playing_idx) && idx < static_cast<int>(total);
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = ansi_for(settings_.list_cursor_color) + bg_ansi_for(settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else if (is_playing_row) {
            std::string playing_ansi = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
            out.push_back(bar + " " + playing_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }

    std::string footer;
    int remaining = static_cast<int>(total) - (scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_queue_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // The right-hand panel changes meaning with the list source, because
    // the two queues are different objects: in Local/Online it is
    // mousiki's own queue_, drained by mousiki; in Spotify it mirrors the
    // account's queue, shared live with the phone and desktop.
    //
    // Read-only on the Spotify side -- the Web API can append (POST
    // /me/player/queue) but offers no reorder or remove, so a cursor
    // there would imply edits that can't be made.
    if (list_source_ == ListSource::Spotify) {
        out.push_back(box_top("UP NEXT (spotify)", total_width, border_ansi));
        const std::string queue_ansi = ansi_for(settings_.queue_color, false);
        for (int row = 0; row < height; ++row) {
            const int idx = row;
            std::string content;
            if (idx < static_cast<int>(spotify_queue_.size())) {
                const auto& t = spotify_queue_[idx];
                const int artist_w = std::min(16, std::max(0, inner / 3));
                const int title_w = std::max(5, inner - 3 - 2 - 2 - artist_w);
                content = pad_right(apply_font_map(std::to_string(idx + 1), settings_.font_map), 3)
                        + settings_.list_separator + " "
                        + pad_right(truncate_str(apply_font_map(t.name, settings_.font_map), title_w), title_w)
                        + settings_.list_separator + " "
                        + truncate_str(apply_font_map(t.artist, settings_.font_map), artist_w);
            } else if (spotify_queue_.empty() && row == height / 2) {
                const std::string text = apply_font_map("NOTHING UP NEXT", settings_.font_map);
                content = std::string(std::max(0, (inner - display_width(text)) / 2), ' ') + text;
            }
            out.push_back(bar + " " + queue_ansi + pad_right(truncate_str(content, inner), inner) + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    std::string title = queue_focus_ ? "QUEUE (focused)" : "QUEUE";
    out.push_back(box_top(title, total_width, border_ansi));

    if (queue_.empty()) {
        int mid_row = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid_row) {
                std::string text = apply_font_map("ADD TRACKS TO QUEUE", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string queue_ansi = ansi_for(settings_.queue_color, false);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    } else {
        for (int row = 0; row < height; ++row) {
            int idx = queue_scroll_ + row;
            std::string content;
            bool is_row_playing = false;
            bool is_row_hovering = false;
            if (idx < static_cast<int>(queue_.size())) {
                const auto& q = queue_[idx];
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(q.title, settings_.font_map);
                content = pad_right(t_idx, 3) + settings_.list_separator + " " + t_title;
                is_row_playing = has_track_ &&
                                 (q.is_local ? (current_is_local_ && q.local_path == current_path_)
                                             : (!current_is_local_ && q.video_id == current_video_id_));
                is_row_hovering = queue_focus_ && (idx == queue_selected_);
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string color_ansi;
            if (is_row_hovering) color_ansi = ansi_for(settings_.queue_cursor_color) + bg_ansi_for(settings_.queue_cursor_bg_color);
            else if (is_row_playing) color_ansi = ansi_for(settings_.queue_playing_color, true) + bg_ansi_for(settings_.queue_playing_bg_color);
            else color_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + color_ansi + padded + "\x1b[0m " + bar);
        }
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------

// main_frame_height() used to live here -- it estimated the Browse view's
// total line count (from the fixed kListVisibleRows constant, among
// other things) so the Settings/Console/Cheatsheet/BulkAdd overlays
// could size themselves to "roughly the same height as the player
// view". That was never actually tied to the real terminal size, which
// is exactly what let all of those overlays overflow a short terminal
// and scroll-duplicate just like the Browse view did (see term_rows_'s
// comment in app.h). Every caller now sizes directly off term_rows_
// (the real, current ioctl-reported row count) instead, so this
// function no longer has a reason to exist.

void App::build_settings_screen(std::ostringstream& frame, int W, int player_h) const {
    // Literal port of the reference SettingsEngine::render() -- same
    // columns, same labels, same schema text, same group-blanking, same
    // divider, same tab-wrap algorithm, same gradient border (applied to
    // this panel's own chrome too, not just the preview swatches), same
    // preview formulas, same bottom hint/status lines, same cursor
    // placement formula. Deliberately not "improved" or restructured.
    if (W < 80) W = 80;
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    // MAX_Y is the row of this panel's own bottom border; the hint line
    // and status line render below it (rows MAX_Y+1, MAX_Y+2), so the
    // total screen rows used here is MAX_Y+2 -- set to exactly match
    // player_h (total rows the Browse-mode view renders), never taller,
    // never shorter, per explicit requirement. Floored modestly so a
    // pathologically small player view still leaves room to render the
    // tab bar/hint/status at all.
    int MAX_Y = std::max(player_h - 2, 10);
    auto B = [&](int y) {
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                              (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    auto repeat = [](const std::string& s, int n) {
        std::string r; r.reserve(s.size() * static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) r += s;
        return r;
    };
    // Matches pad(): no truncation if s is already >= width, just like
    // the reference -- a longer-than-expected value overflows into the
    // next column rather than getting cut off. Values here are always
    // short in practice (numbers, true/false, single-char hotkeys).
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        int ulen = display_width(s);
        if (ulen >= width) return s;
        std::string spaces(width - ulen, ' ');
        return left_align ? (s + spaces) : (spaces + s);
    };

    static const char* kTabNames[] = {"COLORS", "ON/OFF", "ANIMATION", "REFERENCE", "ABOUT APP"};

    // 1. Tab-wrap algorithm.
    std::vector<int> top_tabs, bot_tabs;
    int w_track = 22;
    for (int i = 0; i < 5; ++i) {
        int t_len = static_cast<int>(std::string(kTabNames[i]).size()) + 10;
        if (w_track + t_len < W - 2) { top_tabs.push_back(i); w_track += t_len; }
        else bot_tabs.push_back(i);
    }
    int w = 0;
    std::string l1, l2;
    auto add = [&](const std::string& t, const std::string& b, int width) { l1 += t; l2 += b; w += width; };
    add("\u250c\u2500 SETTINGS \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510", "\u2502                    \u2514", 22);
    for (size_t idx = 0; idx < top_tabs.size(); ++idx) {
        int i = top_tabs[idx];
        std::string lab = (i == settings_tab_) ? ("[" + std::string(kTabNames[i]) + "]") : kTabNames[i];
        int lab_len = static_cast<int>(lab.size());
        bool is_last = (idx == top_tabs.size() - 1);
        add("  " + lab + "  \u250c", repeat("\u2500", 4 + lab_len) + "\u2518", 5 + lab_len);
        if (is_last) add("\u2500", " ", 1);
        else add("\u2500\u2500\u2510", "  \u2514", 3);
    }
    if (w < W - 1) add(repeat("\u2500", W - 1 - w), repeat(" ", W - 1 - w), W - 1 - w);
    add("\u2510", "\u2502", 1);
    pos(1, 1, B(1) + l1 + R);
    pos(2, 1, B(2) + l2 + R);

    // 2. Content.
    int y = 3;
    if (settings_tab_ == 0) {
        static const char* grp[14]  = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                        "LIST", "", "", "QUEUE", "", "", "LYRICS", "", ""};
        static const char* l1n[14]  = {"TOP", "TOP", "KEY", "LEFT", "PLAYED",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG"};
        static const char* l2n[14]  = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING",
                                        "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG"};
        for (int i = 0; i < 14; ++i) {
            if (i == 5) { pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++; }
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);

            pos(y, 3, pad(grp[i], 15)); pos(y, 18, ":");

            pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 0 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 0 && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 7) : pad(settings_get_value(i, 0), 7);
                pos(y, 38, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            pos(y, 48, pad(l2n[i], 7, false)); pos(y, 56, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 1 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 1 && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 7) : pad(settings_get_value(i, 1), 7);
                pos(y, 59, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            std::string valA = settings_get_value(i, 0), valB = settings_get_value(i, 1);
            std::string rt;
            if (i == 0) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 1) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 2) rt = ansi_for(valA, false) + "NAME : " + R + ansi_for(valB, false) + "SONG.MP3" + R;
            else if (i == 3) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 4) rt = ansi_for(valA, false) + "[###########" + R + ansi_for(valB, false) + "----------]" + R;
            else if (i >= 5 && i <= 12) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "THIS IS AN EXAMPLE TEXT" + R;
            else if (i == 13) {
                std::string aL_F = ansi_for(settings_.active_line_color, false), aL_B = bg_ansi_for(settings_.active_line_bg_color);
                std::string aW_F = ansi_for(valA, false), aW_B = bg_ansi_for(valB);
                std::string iN_F = ansi_for(settings_.inactive_line_color, false), iN_B = bg_ansi_for(settings_.inactive_line_bg_color);
                rt = aL_B + aL_F + "THIS IS " + R + aW_B + aW_F + "AN " + R + iN_B + iN_F + "EXAMPLE TEXT" + R;
            }
            if (!rt.empty()) pos(y, 72, rt);
            y++;
        }
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
        static const char* onoff_l[7] = {"Eliment Disk", "Dummy Buttons", "Queue Display", "WaveForm",
                                          "Lyrics Engine", "Lyric Ball", "Visualizer"};
        static const char* anim_l[8] = {"Vis. Fluidity", "Waveform Style", "Disk Speed", "Playback Mode",
                                         "Vis. Degradation", "Vis. Viscosity", "Lyrics Alignment", "Lyrics Animation"};
        int count = (settings_tab_ == 1) ? 7 : 8;
        const char* const* labels = (settings_tab_ == 1) ? onoff_l : anim_l;
        for (int i = 0; i < count; ++i) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, pad(labels[i], 25)); pos(y, 32, ":");

            bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
            bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
            std::string v = ed ? pad(color_edit_buffer_, 20) : pad(settings_get_value(i, 0), 20);
            pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);

            if (sel && !settings_options_for(settings_tab_, i).empty()) pos(y, 57, "\x1b[90m< \u2194 >\x1b[0m");
            y++;
        }
    } else if (settings_tab_ == 3) {
        // Reference tab: the 11 editable hotkeys, then a blank divider,
        // then a read-only display of the font-mapping table (section 4
        // of the config, "A={A,a}" style) loaded from config.txt -- as
        // "A = A, a" rows. Combined they're usually taller than the
        // player view, so this scrolls as one list (viewport follows
        // settings_row_, centered) rather than ever growing the panel
        // past player_h.
        static const char* ref_l[kRefRowCount] = {"Open Settings", "Navigate Up", "Navigate Down", "Play / Pause",
                                                    "Next Track", "Prev Track", "Cycle Play Mode",
                                                    "Search Local", "Search Online", "Quit Application",
                                                    "Seek Forward", "Seek Backward", "Volume Up", "Volume Down",
                                                    "Add To Queue", "Remove From Queue", "Switch Cards",
                                                    "Filter By Folder", "Clear Filter", "Download Stream",
                                                    "Refresh UI", "Console / Logs", "Toggle Mute", "Cheatsheet",
                                                    "Retry Lyrics"};
        std::vector<char> letters;
        for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
        int display_count = kRefRowCount + 1 + static_cast<int>(letters.size()); // +1 for the divider row
        int visible = std::max(1, MAX_Y - 3);
        auto to_display_row = [&](int selectable_row) {
            return (selectable_row < kRefRowCount) ? selectable_row : selectable_row + 1;
        };
        int cur_display = to_display_row(settings_row_);
        int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

        for (int r = 0; r < visible; ++r) {
            int disp = scroll + r;
            if (disp >= display_count) break;
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            if (disp == kRefRowCount) { y++; continue; } // blank divider row
            if (disp < kRefRowCount) {
                int i = disp;
                pos(y, 6, pad(ref_l[i], 25)); pos(y, 32, ":");
                bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 20) : pad(settings_get_value(i, 0), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            } else {
                int li = disp - kRefRowCount - 1;
                char c = letters[li];
                const auto& pair = settings_.font_map.at(c);
                int selectable_row = kRefRowCount + li;
                bool sel = (selectable_row == settings_row_);
                std::string line = std::string(1, c) + " = " + pair.first + ", " + pair.second;
                pos(y, 6, (sel ? HI : "") + line + R);
            }
            y++;
        }
    } else if (settings_tab_ == 4) {
        // About App: shows settings_.about_app_lines (loaded verbatim
        // from config.txt's trailing ClassTextAboutApp={...}; block, not
        // a hardcoded string), scrolled so it never exceeds player_h.
        int visible = std::max(1, MAX_Y - 3);
        int total = static_cast<int>(settings_.about_app_lines.size());
        int scroll = std::clamp(settings_row_, 0, std::max(0, total - visible));
        for (int r = 0; r < visible; ++r) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            int idx = scroll + r;
            if (idx < total) pos(y, 6, settings_.about_app_lines[idx]);
            y++;
        }
    }

    while (y < MAX_Y) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }

    // 3. Bottom frame & overflow tabs.
    if (bot_tabs.empty()) {
        pos(y, 1, B(y) + "\u2514" + repeat("\u2500", W - 2) + "\u2518" + R);
    } else {
        std::string bot = "\u2514\u2500";
        for (int i : bot_tabs) bot += (i == settings_tab_ ? " [" + std::string(kTabNames[i]) + "] \u2500" : "  " + std::string(kTabNames[i]) + "  \u2500");
        int rem_bot = W - static_cast<int>(bot.size()); if (rem_bot < 1) rem_bot = 1;
        pos(y, 1, B(y) + bot + repeat("\u2500", rem_bot - 1) + "\u2518" + R);
    }
    y++;

    pos(y, 1, "\x1b[90m[TAB] Switch | [\u2191\u2193\u2190\u2192] Navigate/Cycle | [ENTER] Edit | [S] Save | [Q] Quit\x1b[0m");
    y++;
    if (!status_line_.empty()) pos(y, 1, "\x1b[32m" + status_line_ + "\x1b[0m");

    // 4. In-place text editing cursor placement.
    if (mode_ == Mode::ColorEdit) {
        if (settings_tab_ == 0) {
            int cy = 3 + settings_row_ + (settings_row_ >= 5 ? 1 : 0);
            int cx = (settings_col_ == 0) ? 38 : 59;
            frame << "\x1b[" << cy << ";" << (cx + static_cast<int>(color_edit_buffer_.size())) << "H\x1b[?25h";
        } else {
            int cy = 3 + settings_row_;
            if (settings_tab_ == 3) {
                // Reference tab scrolls once its row list exceeds the
                // visible window -- now the common case, since it holds
                // 26 hotkey rows plus any font-map rows. Recompute the
                // same scroll offset used when rendering (see the
                // settings_tab_==3 branch above) so the text cursor
                // lands on the row actually drawn there instead of one
                // that's already scrolled off-screen.
                int visible = std::max(1, MAX_Y - 3);
                auto to_display_row = [](int selectable_row) {
                    return (selectable_row < kRefRowCount) ? selectable_row : selectable_row + 1;
                };
                int cur_display = to_display_row(settings_row_);
                std::vector<char> letters;
                for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
                int display_count = kRefRowCount + 1 + static_cast<int>(letters.size());
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            }
            frame << "\x1b[" << cy << ";" << (35 + static_cast<int>(color_edit_buffer_.size())) << "H\x1b[?25h";
        }
    }
}
// ---------------------------------------------------------------------
// Console / log overlay (HKeyConsole)
// ---------------------------------------------------------------------

void App::build_console_screen(std::ostringstream& frame, int W, int target_height) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CONSOLE / LOGS", W, border) << "\n";

    std::vector<std::string> log_lines = ConsoleLog::instance().lines();
    // Must always equal the player view's own height (target_height,
    // computed by player_view_height() -- see its comment in app.h),
    // never just "whatever the terminal happens to fit". A terminal much
    // taller than the actual Browse-mode view would otherwise leave this
    // overlay awkwardly mismatched from the view it's standing in for.
    int visible = std::max(1, target_height - 2); // minus this overlay's own top/bottom border rows
    int total = static_cast<int>(log_lines.size());
    int start = std::max(0, total - visible); // always shows the tail, newest at the bottom

    for (int r = 0; r < visible; ++r) {
        int idx = start + r;
        std::string line = (idx < total) ? log_lines[idx] : "";
        frame << box_line(line, W, border) << "\n";
    }
    frame << box_bottom(W, "[t / ESC] close", border) << "\n";
}

// ---------------------------------------------------------------------
// Cheatsheet overlay (HKeyCheatsheet)
// ---------------------------------------------------------------------

void App::build_cheatsheet_screen(std::ostringstream& frame, int W) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CHEATSHEET", W, border) << "\n";

    // action, human-readable description -- key shown is whatever the
    // user actually has bound (config.txt / rebound in Settings), not a
    // hardcoded assumption, so this stays accurate after remapping.
    static const std::pair<const char*, const char*> rows[] = {
        {"HKeySearch",                      "Search local folder"},
        {"HKeySearchOnline",                "Search online (YouTube)"},
        {"HKeySpotify",                     "Spotify home (playlists, daily mix, top tracks)"},
        {"HKeySearchSpotify",               "Search Spotify catalogue"},
        {"HKeyDownloadStream",              "Download stream to 1st local path"},
        {"HKeyTogglePlayPause",             "Play / pause"},
        {"HKeyPlayNextSong",                "Play next in list/queue"},
        {"HKeyPlayPreviousSong",            "Play previous in list"},
        {"HKeySeekForward",                 "Seek forward 5s"},
        {"HKeySeekBackward",                "Seek backward 5s"},
        {"HKeyIncreaseVolume",              "Volume up"},
        {"HKeyDecreaseVolume",              "Volume down"},
        {"HKeyCyclePlayMode",               "Cycle play mode (list/repeat/shuffle/repeat queue/stop)"},
        {"HKeyRefreshUi",                   "Refresh UI (redraw)"},
        {"HKeyConsole",                     "Console / logs"},
        {"HKeySwitchBetweenCards",          "Switch between panels"},
        {"HKeyAddHoveringSongToQueue",      "Add hovering track to queue"},
        {"HKeyRemoveHoveringSongFromQueue", "Remove hovering track from queue"},
        {"HKeyFilterForFolder",             "Filter by folder"},
        {"HKeyClearFilter",                 "Clear filter"},
        {"HKeyQuit",                        "Quit"},
        {"HKeySetting",                     "Settings panel"},
        {"HKeyNavigateUp",                  "Explore list (up)"},
        {"HKeyNavigateDown",                "Explore list (down)"},
        {"HKeyToggleMute",                  "Mute (without pausing)"},
        {"HKeyCheatsheet",                  "This cheatsheet"},
        {"HKeyRetryLyrics",                 "Retry lyrics"},
    };

    int height = std::max(term_rows_ - 4, 8); // real terminal height, minus this overlay's own top/bottom border rows
    int visible = std::max(1, height - 2);
    int total = static_cast<int>(std::size(rows));
    for (int r = 0; r < visible; ++r) {
        if (r >= total) { frame << box_line("", W, border) << "\n"; continue; }
        auto it = settings_.hotkeys.find(rows[r].first);
        std::string key = (it != settings_.hotkeys.end() && !it->second.empty()) ? it->second : "-";
        std::string line = pad_right(key, 14) + rows[r].second;
        frame << box_line(line, W, border) << "\n";
    }
    frame << box_bottom(W, "[? / ESC] close", border) << "\n";
}

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------
// Deliberately NOT sized like Console/Settings/Cheatsheet -- those match
// the player view or the terminal on purpose (real overlays meant to
// take over the screen). This one is a small floating panel that only
// takes as many rows as it actually has content for: an input row while
// typing, then a short starred checklist once results come in. It never
// grows to fill the terminal.

// ---------------------------------------------------------------------
// Floating panels (Bulk Add, Retry Lyrics) -- see app.h's comment on
// draw_floating_panel() for why these don't clear the screen.
// ---------------------------------------------------------------------

void App::draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W) const {
    int panel_h = static_cast<int>(lines.size());
    int start_col = 1 + std::max(0, (W - panel_w) / 2);
    int start_row = 1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift);
    start_row = std::clamp(start_row, 1, std::max(1, term_rows_ - panel_h));
    for (int i = 0; i < panel_h; ++i) {
        frame << "\x1b[" << (start_row + i) << ";" << start_col << "H" << lines[i];
    }
}

std::vector<std::string> App::build_bulk_add_panel() const {
    const int W = kBulkAddPanelWidth;   // 62, matches the reference design
    const int inner_w = W - 2;          // 60 -- nested box width, flush against the outer border (no gap)
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& inner_line) { return obar + inner_line + obar; };

    std::vector<std::string> lines;
    lines.push_back(box_top("BULK ADD", W, border));

    // --- input box (nested) ---
    lines.push_back(wrap(box_top("", inner_w, border)));
    std::string cursor_line = bulk_add_buffer_.empty()
        ? "//:paste yt playlist link here"
        : bulk_add_buffer_ + "\u2588"; // block cursor once typing starts, matches the Search bar's style
    lines.push_back(wrap(box_line(cursor_line, inner_w, border)));
    lines.push_back(wrap(box_bottom(inner_w, "", border)));

    // --- results box (nested) -- always present at a fixed row count so
    // the panel's total footprint never changes between phase 1 (typing)
    // and phase 2 (results in) -- unused rows are just blank, not
    // omitted, which is what keeps draw_floating_panel()'s fixed-
    // rectangle overwrite artifact-free without ever needing a clear. ---
    lines.push_back(wrap(box_top("", inner_w, border)));

    const auto& items = pending_bulk_add_.items;
    int total = static_cast<int>(items.size());
    int show = bulk_add_results_ready_ ? std::min(total, kBulkAddVisibleRows) : 0;
    int scroll = bulk_add_results_ready_ ? std::clamp(bulk_add_scroll_, 0, std::max(0, total - show)) : 0;

    // Column widths measured directly from the reference design at its
    // 56-column content width (inner_w - 4): title=27, channel=15, the
    // rest is mark + " | " separators + the unpadded time text.
    const int title_w = 27, chan_w = 15;

    for (int r = 0; r < kBulkAddVisibleRows; ++r) {
        if (r >= show) { lines.push_back(wrap(box_line("", inner_w, border))); continue; }
        int idx = scroll + r;
        const auto& it = items[idx];
        bool starred = idx < static_cast<int>(bulk_add_selected_.size()) && bulk_add_selected_[idx];
        bool hovering = idx == bulk_add_cursor_;

        std::string mark = starred ? "*" : " ";
        std::string title = truncate_str(it.title, title_w);
        std::string chan = truncate_str(it.uploader.empty() ? "-" : it.uploader, chan_w);
        std::string time_str = "-";
        if (it.duration_sec >= 0) {
            int secs = static_cast<int>(it.duration_sec);
            time_str = std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
        }

        std::string line = mark + " | " + pad_right(title, title_w) + " | " + pad_right(chan, chan_w) + " | " + time_str;
        // Built plain first, then box_line() pads/truncates it (its
        // truncate_str/pad_right count raw bytes, not display columns --
        // they don't skip ANSI escapes), and only *after* that do we
        // splice the reverse-video hover highlight into the already-
        // finished bar+content+bar string. Wrapping `line` in "\x1b[7m"
        // before handing it to box_line() would get its own length
        // miscounted against the ANSI bytes and risk truncate_str
        // slicing straight through the trailing "\x1b[0m" reset,
        // leaking reverse-video onto every line after it -- exactly the
        // bug build_list_panel avoids by coloring after padding, not
        // before (see its own hover/cursor rendering).
        std::string boxed = box_line(line, inner_w, border);
        if (hovering) {
            std::string vbar_len = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
            size_t start = vbar_len.size() + 1; // past "bar + ' '"
            size_t end = boxed.size() - vbar_len.size() - 1; // before "' ' + bar"
            boxed = boxed.substr(0, start) + "\x1b[7m" + boxed.substr(start, end - start) + "\x1b[0m" + boxed.substr(end);
        }
        lines.push_back(wrap(boxed));
    }

    // Bottom border of the results box carries two plain-text labels --
    // "[ N more ]" on the left (only once there's more than fits), "ALL"
    // and "SELECT" on the right as the two commit actions. Just text, no
    // button/tab border art.
    int more = bulk_add_results_ready_ ? (total - show) : 0;
    std::string left_label = more > 0 ? ("[ " + std::to_string(more) + " more ]") : "";
    std::string right_label = bulk_add_results_ready_ ? "ALL   SELECT" : "";
    std::string prefix = settings_.box_lower_left + settings_.box_horizontal;
    if (!left_label.empty()) prefix += " " + left_label + " ";
    std::string suffix = right_label.empty() ? "" : (" " + right_label + " ");
    suffix += settings_.box_lower_right;
    int used = display_width(prefix) + display_width(suffix);
    int dashes = std::max(0, inner_w - used);
    std::string bottom = prefix;
    for (int i = 0; i < dashes; ++i) bottom += settings_.box_horizontal;
    bottom += suffix;
    bottom = pad_right(bottom, inner_w);
    lines.push_back(wrap(border.empty() ? bottom : (border + bottom + "\x1b[0m")));

    // Outer box's own bottom border carries "[ESC] cancel" directly.
    lines.push_back(box_bottom(W, "[ESC] cancel", border));
    return lines;
}

// ---------------------------------------------------------------------
// Retry Lyrics (HKeyRetryLyrics, 'l') -- manual title/artist override
// ---------------------------------------------------------------------

std::vector<App::RLField> App::rl_visible_fields() const {
    std::vector<RLField> f = {
        RLField::Title, RLField::Artist, RLField::Ft,
        RLField::TypeReverb, RLField::TypeSlowed, RLField::TypeUltraSlowed,
        RLField::TypeSpedup, RLField::TypeRemix, RLField::TypeOther,
    };
    if (rl_remix_) f.push_back(RLField::RemixText);
    if (rl_other_) f.push_back(RLField::OtherText);
    return f;
}

bool* App::rl_bool_ptr(RLField f) {
    switch (f) {
        case RLField::TypeReverb: return &rl_reverb_;
        case RLField::TypeSlowed: return &rl_slowed_;
        case RLField::TypeUltraSlowed: return &rl_ultra_slowed_;
        case RLField::TypeSpedup: return &rl_spedup_;
        case RLField::TypeRemix: return &rl_remix_;
        case RLField::TypeOther: return &rl_other_;
        default: return nullptr;
    }
}

std::string* App::rl_text_ptr(RLField f) {
    switch (f) {
        case RLField::Title: return &rl_title_;
        case RLField::Artist: return &rl_artist_;
        case RLField::Ft: return &rl_ft_;
        case RLField::RemixText: return &rl_remix_text_;
        case RLField::OtherText: return &rl_other_text_;
        default: return nullptr;
    }
}

// Pre-fills from the current track's metadata_ rather than opening
// blank, and resets every other bit of state -- so reopening after a
// previous override (or a previous cancel) never leaks stale values
// from last time.
void App::rl_open_from_current_track() {
    rl_title_ = metadata_.name;
    rl_artist_ = (metadata_.artist == "-") ? "" : metadata_.artist;
    rl_ft_.clear();

    // Best-effort ft./feat. split: if the title itself carries a
    // "feat."/"ft." tag, pull it out into its own field rather than
    // leaving it embedded (so it doesn't get double-appended once the
    // TYPE tags get tacked on after the title at submit time).
    static const std::vector<std::string> markers = {" feat. ", " feat ", " ft. ", " ft "};
    std::string lower_title = metadata_.name;
    std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    for (const auto& marker : markers) {
        size_t pos = lower_title.find(marker);
        if (pos != std::string::npos) {
            rl_title_ = metadata_.name.substr(0, pos);
            rl_ft_ = metadata_.name.substr(pos + marker.size());
            // trim a trailing ')' if the split landed inside "(feat. X)"
            if (!rl_ft_.empty() && rl_ft_.back() == ')') rl_ft_.pop_back();
            while (!rl_title_.empty() && (rl_title_.back() == ' ' || rl_title_.back() == '(')) rl_title_.pop_back();
            break;
        }
    }

    rl_remix_text_.clear();
    rl_other_text_.clear();
    rl_reverb_ = rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = rl_remix_ = rl_other_ = false;
    rl_focus_ = RLField::Title;
}

// Builds "title [ft. X] [tags...]" and launches the override fetch --
// this is the actual point of the whole form: feeding a corrected
// query into fetch_synced_lyrics() instead of the track's real
// metadata, for tracks whose auto-fetched lyrics are wrong/missing.
void App::rl_submit() {
    std::string query = rl_title_;
    if (!rl_ft_.empty()) query += " ft. " + rl_ft_;

    // Tags go after the title (per your note: "you usually need to place
    // them after title in url"). Slowed/Ultra Slowed/Spedup are mutually
    // exclusive so at most one of these three contributes; Reverb/Remix/
    // Other are independent and can stack with it and each other.
    if (rl_slowed_) query += " slowed";
    else if (rl_ultra_slowed_) query += " ultra slowed";
    else if (rl_spedup_) query += " sped up";
    if (rl_reverb_) query += " reverb";
    if (rl_remix_) query += rl_remix_text_.empty() ? " remix" : (" " + rl_remix_text_ + " remix");
    if (rl_other_ && !rl_other_text_.empty()) query += " " + rl_other_text_;

    launch_lyrics_fetch(query, rl_artist_, current_path_, /*force_network=*/true);
    log_event("retrying lyrics: \"" + query + "\"");

    mode_ = Mode::Browse;
}

std::vector<std::string> App::build_retry_lyrics_panel() const {
    const int W = kRetryLyricsPanelWidth; // 62, matches the reference design
    const int label_w = 14;               // left label column, blank on box top/bottom rows
    const int box_w = W - 2 - label_w;    // 46 -- nested input box width
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& left, const std::string& right) { return obar + left + right + obar; };
    auto label = [&](const std::string& text) { return pad_left(text, label_w - 2) + " :"; };
    auto blank_label = [&] { return std::string(label_w, ' '); };

    // A text field: label appears only on the middle (content) row, the
    // nested box's own top/bottom rows get a blank label column.
    // `placeholder` shows only when the field is both empty AND not
    // currently focused -- e.g. REMIX/OTHER's "NOT SELECTED". While
    // actively editing an empty field, show just the cursor rather than
    // the placeholder text glued to it (which would otherwise look like
    // "NOT SELECTED" was real, already-typed content).
    auto text_field = [&](RLField f, const std::string& lbl, const std::string& value,
                           const std::string& placeholder, std::vector<std::string>& out) {
        out.push_back(wrap(blank_label(), box_top("", box_w, border)));
        bool focused = (rl_focus_ == f);
        std::string shown = (value.empty() && !focused) ? placeholder : value;
        if (focused) shown += "\u2588"; // block cursor, same convention as Search/Bulk Add
        out.push_back(wrap(label(lbl), box_line(shown, box_w, border)));
        out.push_back(wrap(blank_label(), box_bottom(box_w, "", border)));
    };

    std::vector<std::string> lines;
    lines.push_back(box_top("Retry Lyrics", W, border));

    text_field(RLField::Title, "SONG TITLE", rl_title_, "", lines);
    text_field(RLField::Artist, "ARTIST NAME", rl_artist_, "", lines);
    text_field(RLField::Ft, "FT  ( opt )", rl_ft_, "", lines);

    // TYPE: two rows of plain (non-boxed) checkbox-style toggles.
    // Slowed/Ultra Slowed/Spedup are a radio group (only one can show
    // "[o]" at a time); Reverb/Remix/Other are independent.
    //
    // Built and measured as plain ASCII first, then the focus highlight
    // is spliced in by byte offset *after* pad_right/truncate_str has
    // already run -- same reasoning as the bulk-add hover fix above:
    // those two don't skip ANSI escapes when counting width, so
    // wrapping an option in "\x1b[7m" before truncating/padding the row
    // risks slicing through the reset code and leaking reverse-video
    // onto the rest of the panel. Every word here is plain ASCII, so a
    // byte offset is also a column offset -- no UTF-8 width subtleties
    // to worry about.
    auto opt_plain = [&](bool checked, const std::string& word) { return (checked ? "[o] " : " o  ") + word; };
    auto type_row = [&](std::initializer_list<std::tuple<RLField, bool, std::string>> opts) {
        std::string plain = " ";
        int focus_start = -1, focus_len = 0;
        for (const auto& [f, checked, word] : opts) {
            std::string s = opt_plain(checked, word);
            if (rl_focus_ == f) { focus_start = static_cast<int>(plain.size()); focus_len = static_cast<int>(s.size()); }
            plain += s;
            plain += " ";
        }
        std::string padded = pad_right(truncate_str(plain, box_w), box_w);
        if (focus_start >= 0 && focus_start + focus_len <= static_cast<int>(padded.size())) {
            padded = padded.substr(0, focus_start) + "\x1b[7m" + padded.substr(focus_start, focus_len) +
                     "\x1b[0m" + padded.substr(focus_start + focus_len);
        }
        return padded;
    };
    lines.push_back(wrap(label("TYPE"),
        type_row({{RLField::TypeReverb, rl_reverb_, "reverb"},
                  {RLField::TypeSlowed, rl_slowed_, "slowed"},
                  {RLField::TypeUltraSlowed, rl_ultra_slowed_, "ultra slowed"}})));
    lines.push_back(wrap(blank_label(),
        type_row({{RLField::TypeSpedup, rl_spedup_, "spedup"},
                  {RLField::TypeRemix, rl_remix_, "remix"},
                  {RLField::TypeOther, rl_other_, "other /pls specify"}})));

    // REMIX / OTHER: dynamically present -- only when their TYPE toggle
    // is on. When hidden, still reserve their 3 rows as blank (not
    // omitted) so the panel's total height never changes frame to frame
    // -- draw_floating_panel() stamps a fixed rectangle every frame with
    // no clear, so a shrinking panel would otherwise leave stale
    // characters behind at the edges it used to cover.
    if (rl_remix_) {
        text_field(RLField::RemixText, "REMIX", rl_remix_text_, "NOT SELECTED", lines);
    } else {
        for (int i = 0; i < 3; ++i) lines.push_back(wrap(blank_label(), std::string(box_w, ' ')));
    }
    if (rl_other_) {
        text_field(RLField::OtherText, "OTHER", rl_other_text_, "NOT SELECTED", lines);
    } else {
        for (int i = 0; i < 3; ++i) lines.push_back(wrap(blank_label(), std::string(box_w, ' ')));
    }

    std::string fetch_word = "enter to fetch";
    std::string hint = pad_left(fetch_word, W - 2 - 2); // 2 trailing spaces before the border, matching the reference
    lines.push_back(wrap("", pad_right(hint, W - 2)));

    lines.push_back(box_bottom(W, "[ESC] cancel", border));
    return lines;
}

// The Console and Settings overlays must always be exactly as tall as
// the Browse-mode player view -- see the comment on this declaration in
// app.h. This recomputes the same panel line counts render_frame()'s
// Browse-mode branch does, plus list_visible_rows_ (already kept
// up to date each frame -- see render_frame()) for the list/queue
// panel's share.
int App::player_view_height(int w) const {
    int h = static_cast<int>(build_metadata_panel(w).size());
    h += static_cast<int>(build_progress_panel(w).size());
    h += static_cast<int>(build_search_bar(w).size());
    h += list_visible_rows_;
    h += 1; // blank separator line
    h += 1; // status/loading line -- reserved even when currently empty, so this doesn't jitter frame to frame
    return h;
}

// ---------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------

std::string App::render_frame(TerminalIO& term) {
    // Notifications ("ui refreshed", "saved session snapshot", ...) are
    // meant to be a brief pulse, not a permanent fixture. status_line_ is
    // set from dozens of call sites throughout this file (log_event(),
    // plus many direct "status_line_ = ..." assignments for errors/
    // inline feedback) -- rather than requiring every one of them to
    // remember to also stamp a timestamp, this detects the *change*
    // itself: whenever status_line_ differs from what was seen last
    // frame, the clock restarts; once it's held the same text for
    // kStatusLineLifetimeSec, it's cleared.
    if (status_line_ != last_seen_status_line_) {
        status_line_set_at_ = std::chrono::steady_clock::now();
        last_seen_status_line_ = status_line_;
    } else if (!status_line_.empty() &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - status_line_set_at_).count() >= kStatusLineLifetimeSec) {
        status_line_.clear();
        last_seen_status_line_.clear();
    }

    int term_cols = term.cols();
    // Was clamped to a minimum of 80 regardless of the real terminal
    // width -- on a narrower phone terminal (the screenshots suggest
    // something closer to 40-46 visible columns), every line rendered
    // here would already be wider than the physical screen and get
    // wrapped by the terminal itself before the next frame's cursor-home
    // redraw overwrites it. That reads exactly like "truncated mid-word"
    // or "garbled" text even though nothing in this file actually cut it
    // off -- the terminal did, one line later than expected. Lowering the
    // floor so the app actually renders to the real width instead of
    // always assuming at least 80 columns are available.
    int W = std::clamp(term_cols, 40, 200);

    // Real terminal row count -- see term_rows_'s comment in app.h for
    // the full story on why this now actually gets consulted. rows()
    // itself already falls back to a sane default (40) if the ioctl
    // fails, so no extra guarding needed here beyond a floor against
    // truly pathological values feeding into subtraction below.
    term_rows_ = std::max(term.rows(), 4);

    // Height-driven, so a live resize re-lays-out on the next frame.
    const bool was_compact = compact_;
    update_compact_mode();

    // Browse/BulkAdd/RetryLyrics all share the same live background (the
    // latter two float a small panel on top of it -- see
    // draw_floating_panel()'s comment in app.h), so switching between
    // them never needs a full clear for the *background* itself, only a
    // redraw. But the floating panel's own footprint is positioned via
    // absolute cursor writes that can land on cells the background's
    // normal top-to-bottom sequential draw never touches at all (rows
    // below its last line, or columns past what list/queue naturally
    // fill) -- so entering *or leaving* one of those specifically still
    // needs a hard clear, or its content (or an old copy of it) is left
    // on screen forever with nothing ever overwriting it again. Settings/
    // Console/Cheatsheet are still genuine full-screen takeovers, so
    // entering or leaving any of *those* still forces one too, same as a
    // real mode change always has.
    auto mode_family = [](Mode m) {
        switch (m) {
            case Mode::Browse: case Mode::Search: return 0;
            case Mode::BulkAdd: return 1;
            case Mode::RetryLyrics: return 2;
            case Mode::Settings: case Mode::ColorEdit: return 3;
            case Mode::Console: return 4;
            case Mode::Cheatsheet: return 5;
        }
        return 0;
    };
    // compact_ changing is a height change, not just a content change:
    // the full layout is 12 rows taller, so going compact leaves rows
    // below the new frame that nothing would ever overwrite. Same class
    // of leftover a width change causes.
    bool hard_clear = (W != last_render_w_) || (mode_family(mode_) != mode_family(last_render_mode_))
                   || (was_compact != compact_) || force_redraw_;
    if (last_render_w_ != -1 && W != last_render_w_) {
        // Verbose-only: raw ioctl terminal size alongside the clamped
        // app-usable width, i.e. "what the OS actually told us" versus
        // what we did with it.
        ConsoleLog::instance().log_verbose(
            "terminal resized: " + std::to_string(last_render_w_) + " -> " + std::to_string(W) +
            " cols (raw ioctl cols=" + std::to_string(term_cols) + ", rows=" + std::to_string(term_rows_) + ")");
    }
    force_redraw_ = false; // one-shot -- consumed by this frame
    last_render_w_ = W;
    last_render_mode_ = mode_;
    const char* clear_prefix = hard_clear ? "\x1b[2J\x1b[H" : "\x1b[H";

    if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_settings_screen(frame, W, player_view_height(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::Console) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_console_screen(frame, W, player_view_height(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::Cheatsheet) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_cheatsheet_screen(frame, W);
        return clamp_output_rows(frame.str(), term_rows_);
    }

    // --- Browse mode (and the background behind BulkAdd/RetryLyrics):
    // figure out how many list/queue rows actually fit before building
    // anything, so the panel is sized right the first time instead of
    // being built tall and then chopped.
    auto metadata_lines = build_metadata_panel(W);
    auto progress_lines = build_progress_panel(W);
    auto search_lines = build_search_bar(W);
    int fixed_h = static_cast<int>(metadata_lines.size() + progress_lines.size() + search_lines.size())
                + 1  // blank separator line
                + 1; // status/loading line (reserved even when empty, so it doesn't jitter frame to frame)
    // -1 extra margin: leave the terminal's very last row untouched so a
    // trailing '\n' after the final printed line can never itself force
    // a scroll (see clamp_output_rows()'s comment for the same reasoning
    // applied as a hard backstop).
    int available_for_list = term_rows_ - fixed_h - 1;
    list_visible_rows_ = std::clamp(available_for_list, 0, kListVisibleRows);

    ensure_visible_row_meta();

    std::ostringstream frame;
    frame << clear_prefix;

    for (auto& l : metadata_lines) frame << l << "\n";
    for (auto& l : progress_lines) frame << l << "\n";
    for (auto& l : search_lines) frame << l << "\n";

    int list_h = list_visible_rows_;
    if (settings_.element_queue) {
        int list_w = W / 2;
        int queue_w = W - list_w; // exact 50/50, remainder (odd W) goes to queue
        auto list_lines = build_list_panel(list_w, list_h);
        auto queue_lines = build_queue_panel(queue_w, list_h);
        size_t rows = std::max(list_lines.size(), queue_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < list_lines.size()) ? list_lines[i] : std::string(list_w, ' ');
            std::string r = (i < queue_lines.size()) ? queue_lines[i] : std::string(queue_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_list_panel(W, list_h)) frame << l << "\n";
    }

    frame << "\n";
    if (load_in_progress_.load() && load_stage_.load() == 1) {
        // Only the online resolve/download step shows a live status —
        // local loads are probe-only now (near-instant) and deliberately
        // silent, no "loading..." flash.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started_at_).count();
        frame << "  resolving/downloading... (" << static_cast<int>(secs) << "s)\n";
    } else if (!status_line_.empty()) {
        frame << "  " << status_line_ << "\n";
    }

    frame << "\x1b[0J";

    // Bulk Add / Retry Lyrics: stamp their floating panel on top of the
    // still-live background just built above, rather than replacing it.
    // Uses absolute positioning (draw_floating_panel()), so it's simply
    // appended after the background's own sequential top-to-bottom
    // writes -- whichever content lands on a given screen cell last in
    // the stream wins, and the panel is emitted after, so it draws over
    // the background wherever they overlap without needing a clear.
    if (mode_ == Mode::BulkAdd) {
        draw_floating_panel(frame, build_bulk_add_panel(), kBulkAddPanelWidth, W);
    } else if (mode_ == Mode::RetryLyrics) {
        draw_floating_panel(frame, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W);
    }

    // Hard safety net on top of the list_visible_rows_ sizing above: even
    // if the fixed chrome alone (metadata+progress+search bar) is taller
    // than the terminal -- a case list_visible_rows_ can't do anything
    // about, since it only controls the list panel -- this guarantees
    // the actual byte stream handed to the terminal never contains more
    // rows than the terminal has, so it structurally cannot scroll no
    // matter what future panels/config combinations produce. Only
    // applied to the background portion's line count implicitly (the
    // floating panel's absolute-positioned writes come after and are
    // already bounds-checked by draw_floating_panel() itself, so
    // clamping here by counting trailing '\n's is still correct -- the
    // panel's writes don't add any that would trip this).
    return clamp_output_rows(frame.str(), term_rows_);
}

// Keeps at most (term_rows - 1) lines of `frame` (the -1 leaves the
// terminal's last row untouched, so the final line's trailing '\n' can
// never itself trigger a scroll) and drops everything after that,
// escape-code prefixes and all -- this is the hard backstop described
// in term_rows_'s comment in app.h: whatever the panel-sizing logic
// above computed, the actual printed output can never exceed what the
// real terminal can show without scrolling. Content past the cutoff is
// simply not drawn this frame rather than causing any corruption.
std::string App::clamp_output_rows(const std::string& frame, int term_rows) const {
    int max_lines = std::max(1, term_rows - 1);
    int newlines_seen = 0;
    for (size_t i = 0; i < frame.size(); ++i) {
        if (frame[i] == '\n') {
            ++newlines_seen;
            if (newlines_seen >= max_lines) return frame.substr(0, i + 1);
        }
    }
    return frame; // already within budget
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------

int App::run() {
    ConsoleLog::instance().init(settings_.console_verbosity == 1 ? LogVerbosity::Verbose : LogVerbosity::Basic);
    {
        // Verbose-only startup facts -- "what the OS provided" at the
        // very start of the session, before anything else has run.
        struct utsname uts{};
        if (uname(&uts) == 0) {
            ConsoleLog::instance().log_verbose(std::string("os: ") + uts.sysname + " " + uts.release + " " + uts.machine);
        }
        ConsoleLog::instance().log_verbose("home: " + std::string(std::getenv("HOME") ? std::getenv("HOME") : "(unset)"));
    }

    // Session snapshot restore -- only when the feature's on. A missing
    // or corrupt snapshot.json is treated identically to "no snapshot at
    // all" (load_snapshot() already guards that), so this always falls
    // back to the normal cold-start behavior on any failure.
    bool restored = false;
    if (settings_.autosave_enabled) {
        SnapshotData snap;
        if (load_snapshot(snap)) {
            restore_snapshot(snap);
            restored = true;
            // Consumed exactly once -- see snapshot.h's delete_snapshot()
            // comment for why this happens right after a successful
            // restore rather than only at the next autosave/exit.
            delete_snapshot();
            log_event("restored previous session");
        }
    }
    // Cold start used to immediately play local_view_[0] -- whichever
    // file happened to sort first, chosen by nobody. Now the cursor is
    // just parked there and nothing plays until the user picks something
    // (see Settings::autoplay_on_start).
    if (!restored && !local_view_.empty()) {
        selected_ = 0;
        if (settings_.autoplay_on_start) start_local_track(local_view_[0]);
    }

    TerminalIO term;
    last_frame_time_ = std::chrono::steady_clock::now();
    last_autosave_at_ = std::chrono::steady_clock::now();
    ConsoleLog::instance().log_verbose("terminal: " + std::to_string(term.rows()) + "x" + std::to_string(term.cols()) + " (rows x cols, raw ioctl)");

    while (!quit_) {
        int key = term.poll_key();
        handle_key(key);

        poll_pending_search();
        poll_pending_load();
        poll_pending_waveform();
        poll_pending_bulk_add();
        poll_pending_spotify_fetch();
        poll_pending_spotify_status();
        poll_pending_album_art();
        poll_pending_spotify_auth();
        poll_pending_spotify_messages();
        maybe_autosave();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            if (player_.finished()) advance_track();
        }
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        // The disk is time-driven rather than audio-driven, so it keeps
        // spinning for a Spotify track even though the audio-fed elements
        // (visualizer, waveform) are flat.
        const bool spinning = spotify_playing_ ? (spotify_status_.player.state == "playing")
                                               : (has_track_ && !player_.is_paused());
        if (spinning) {
            angle_ = std::fmod(angle_ + kAngularVelocity * settings_.disk_rotation_speed * dt,
                               2.0 * 3.14159265358979323846);
        }

        std::cout << render_frame(term) << std::flush;
        // 25fps (was 12.5fps) — the 700ms waveform reveal animation only
        // got ~9 frames to work with at the old 80ms cadence, which
        // showed as a handful of visible ~11% jumps rather than a smooth
        // continuous expansion. Also smooths disk rotation and the
        // visualizer's motion generally. Text-frame rendering is cheap
        // enough that doubling the rate here is not a meaningful CPU/
        // battery concern.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    player_.stop();
    term.restore();
    save_settings(settings_);
    // Final snapshot on a clean quit -- same single-canonical-file
    // save_snapshot() the 30s autosave tick uses, just with the exact
    // position at the moment of quitting rather than up to 30s stale.
    if (settings_.autosave_enabled) {
        save_snapshot(build_snapshot());
        ConsoleLog::instance().log_basic("saved session snapshot on exit");
    }
    if (load_thread_.joinable()) load_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    if (device_thread_.joinable()) device_thread_.join();
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    // Signalled first, then joined -- the poll loop sleeps in 100ms
    // slices so this doesn't block for a whole interval on quit.
    stop_spotify_poll();
    stop_spotify_cmd_worker();
    if (spotify_thread_.joinable()) spotify_thread_.join();
    // Detached, not joined: interactive_login() blocks on a loopback
    // socket for up to five minutes, and quitting must not wait on a
    // browser tab the user already abandoned. The leak lasts the few
    // remaining milliseconds of process life.
    if (spotify_auth_thread_.joinable()) spotify_auth_thread_.detach();
    std::cout << "\nbye.\n";
    return 0;
}

} // namespace muisc
