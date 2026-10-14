#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disk_art.h"
#include "fft_visualizer.h"
#include "local_source.h"
#include "lyrics_fetcher.h"
#include "metadata_probe.h"
#include "native_duration.h"
#include "online_source.h"
#include "player.h"
#include "settings.h"
#include "snapshot.h"
#include "spotify_client.h"
#include "spotify_home.h"
#include "sphere_visualizer.h"
#include "streaming_pcm.h"
#include "terminal_ui.h"
#include "waveform.h"
#include "youtube_source.h"
#include "cache_manager.h"

namespace muisc {

enum class Mode { Browse, Search, Settings, ColorEdit, Console, Cheatsheet, BulkAdd, RetryLyrics };
enum class ListSource { Local, Online, Spotify };

// Where inside the Spotify view the list panel currently sits. Unlike
// Local and Online, the Spotify view is a two-deep browser: Home is a
// menu of sections, and every section resolves to one of the four list
// shapes below.
//
// Each level renders from its own vector (spotify_tracks_,
// spotify_playlists_, ...) because the columns differ per level -- a
// track has a duration, a playlist a track count, an album a year.
enum class SpotifyLevel {
    Home,       // the section menu
    Tracks,     // a flat track list -- daily mix, top tracks, liked songs,
                // search results, a playlist's/album's/artist's tracks
    Playlists,
    Artists,
    Albums,
};

struct QueueItem {
    bool is_local;
    std::string title;
    std::string artist;
    fs::path local_path;   // valid if is_local
    std::string video_id;  // valid if !is_local
};

class App {
public:
    App();
    int run();

private:
    // --- infrastructure ---
    CacheManager cache_;
    YoutubeSource youtube_{cache_};
    OnlineSource online_;
    LocalSource local_source_;
    DiskArt disk_;
    mutable Player player_;
    // (lyrics_script_ was removed -- lyrics fetching is native C++/libcurl
    // now, see lyrics_fetcher.cpp/http_client.cpp, no script to locate.)

    // --- lists / navigation ---
    Mode mode_ = Mode::Browse;
    ListSource list_source_ = ListSource::Local;
    std::vector<LocalTrack> all_local_tracks_;
    std::vector<LocalTrack> local_view_;     // filtered
    std::vector<OnlineResult> online_view_;

    // --- Spotify view ---------------------------------------------------
    // A remote control, not a second decoder -- see the Spotify section
    // of app.cpp for what follows from that.
    std::shared_ptr<SpotifyClient> spotify_;
    // Why the client couldn't be built, shown in the list panel in place
    // of an empty list: "not signed in" and "no client id in config.txt"
    // are both fixable, so they have to be visible rather than looking
    // like an account with no playlists.
    std::string spotify_unavailable_reason_;
    SpotifyLevel spotify_level_ = SpotifyLevel::Home;
    std::vector<SpotifyHomeRow> spotify_home_;
    std::vector<SpotifyTrack> spotify_tracks_;
    std::vector<SpotifyPlaylist> spotify_playlists_;
    std::vector<SpotifyArtist> spotify_artists_;
    std::vector<SpotifyAlbum> spotify_albums_;
    // Label for the current Tracks level ("sunday morning", "daily mix
    // 14 oct", ...) -- the level alone can't name itself, since five
    // different sections all land on Tracks.
    std::string spotify_list_label_;
    // Remembered so Esc from a track list returns to the section the
    // user drilled in from rather than all the way to Home.
    SpotifyLevel spotify_parent_level_ = SpotifyLevel::Home;
    std::string spotify_search_query_;
    // The account's real up-next list, mirrored from Spotify rather than
    // owned by mousiki -- see build_queue_panel().
    std::vector<SpotifyTrack> spotify_queue_;
    // Level drilled in from, captured when a fetch launches and applied
    // only once it succeeds, so a failed fetch doesn't rewrite the
    // back-navigation target.
    SpotifyLevel spotify_fetch_parent_ = SpotifyLevel::Home;
    // Search prefix. Not "s:", which belongs to the YouTube search path.
    static constexpr const char* kSpotifySearchPrefix = "sp:";

    int selected_ = 0;
    int scroll_ = 0;
    std::string search_buffer_;
    std::string last_local_query_;
    ListSource pre_search_list_source_ = ListSource::Local;
    std::string pre_search_local_query_;
    std::string last_online_query_;
    int local_sort_mode_ = 0; // 0=folder order, 1=title A-Z, 2=artist A-Z
    static constexpr int kListVisibleRows = 8; // the *maximum*/preferred list height when there's room for it

    // --- terminal-height awareness --------------------------------------
    // The render loop used to only ever look at term.cols() (see the
    // comment on the width clamp in render_frame()) and unconditionally
    // emitted a fixed-height frame — metadata panel + progress + search
    // bar + a hardcoded kListVisibleRows-row list/queue panel + status
    // line — redrawn purely with "\x1b[H" + "\x1b[0J" and no alternate-
    // screen buffer. On a terminal shorter than that fixed height, each
    // frame overflows the viewport and scrolls; the next frame's
    // "\x1b[H" then homes to the top of the *new* scrolled-into-view
    // position rather than the top of the previous frame, so it draws a
    // fresh copy further down, which overflows again, forever — visible
    // as the panel endlessly re-duplicating itself downward.
    //
    // term_rows_ is the real, current terminal row count (from
    // TerminalIO::rows(), which does read the OS via ioctl(TIOCGWINSZ)
    // correctly -- it just wasn't being consulted anywhere). Refreshed
    // once per frame at the top of render_frame(). list_visible_rows_ is
    // how many list/queue rows *actually* fit this frame -- clamped
    // between 0 and kListVisibleRows based on how much room term_rows_
    // leaves after the fixed chrome (metadata/progress/search bar/status
    // line). Every place that used to scroll-clamp or size against the
    // kListVisibleRows constant now uses this instead, so what's
    // rendered and what the scroll math thinks is visible never
    // disagree. render_frame() also applies a hard line-count safety net
    // (see clamp_output_rows()) on top of this, so even a terminal too
    // short for the fixed chrome alone (metadata+progress+search bar)
    // still can never scroll — the two mechanisms are independent, not
    // "either/or".
    int term_rows_ = 24;
    int list_visible_rows_ = kListVisibleRows;
    std::string clamp_output_rows(const std::string& frame, int term_rows) const;

    mutable std::mutex row_meta_mutex_;
    std::unordered_map<std::string, RowMeta> row_meta_cache_;
    std::atomic<bool> row_meta_resolver_started_{false};
    void launch_row_meta_resolver();

    // --- queue ---
    std::vector<QueueItem> queue_;
    int queue_selected_ = 0;   // cursor/"hovering" row, only meaningful once queue_focus_ has been used
    int queue_scroll_ = 0;
    bool queue_focus_ = false; // Tab toggles which panel Up/Down navigates

    // --- now playing ---
    bool has_track_ = false;
    fs::path current_path_;
    bool current_is_local_ = true;  // for snapshot identity -- current_path_ alone is ambiguous
                                     // (online tracks resolve to a cache path too)
    std::string current_video_id_;  // valid when !current_is_local_

    // --- Spotify now-playing -------------------------------------------
    // While this is true, spotify_status_ is the authority on what's
    // playing, not `player_`/`metadata_`. Separate from has_track_
    // because has_track_ gates the finished()/advance machinery in run(),
    // and advancing past a finished Spotify track is Spotify's job.
    bool spotify_playing_ = false;
    SpotifyStatus spotify_status_;
    // When spotify_status_ was captured. The Web API is polled no more
    // often than every 5s (rate limits), so position_ms is always stale
    // by up to that long; spotify_elapsed_sec() interpolates forward
    // from here instead of letting the progress bar jump.
    std::chrono::steady_clock::time_point spotify_status_at_;
    double spotify_elapsed_sec() const;

    TrackMetadata metadata_;
    std::vector<float> waveform_envelope_;
    std::chrono::steady_clock::time_point waveform_reveal_start_;
    bool waveform_ready_ = false;
    std::shared_ptr<StreamingPcm> current_pcm_;
    size_t total_sec_ = 0;
    double angle_ = 0.0;
    std::chrono::steady_clock::time_point last_frame_time_;
    static constexpr double kAngularVelocity = (2.0 * 3.14159265358979323846 / 48.0) / 0.035;
    mutable FftVisualizer fft_;
    mutable SphereVisualizer sphere_;
    mutable std::string last_lyrics_status_;
    mutable std::chrono::steady_clock::time_point lyrics_status_shown_at_;
    mutable double viz_dt_ = 0.08;

    // --- lyrics (background-fetched) ---
    mutable std::mutex lyrics_mutex_;
    mutable LyricsResult lyrics_result_;
    std::atomic<bool> lyrics_ready_{false};
    std::atomic<int> lyrics_epoch_{0};
    void launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network = false);

    std::string status_line_;
    // Cleared automatically 5s after last changing (see the check at the
    // top of render_frame()) -- notifications like "ui refreshed" or
    // "saved session snapshot" are meant to be a brief pulse, not a
    // permanent fixture at the bottom of the screen until something else
    // happens to overwrite them. last_seen_status_line_ is how that
    // check detects "did this actually just change" across the dozens of
    // call sites that set status_line_ directly, without requiring each
    // of them to remember to stamp a timestamp themselves.
    std::string last_seen_status_line_;
    std::chrono::steady_clock::time_point status_line_set_at_;
    static constexpr double kStatusLineLifetimeSec = 5.0;
    bool quit_ = false;
    bool force_redraw_ = false;
    int last_render_w_ = -1;
    Mode last_render_mode_ = Mode::Browse;

    // --- mute (volume forced to 0 without touching pause state) --------
    bool muted_ = false;
    int pre_mute_volume_ = 70;

    // --- folder filter (HKeyFilterForFolder / HKeyClearFilter) ---------
    // Parent-directory path of the currently filtered folder, or empty
    // for "no folder filter". Applied on top of whatever the search/sort
    // already produced, in refresh_local_view().
    std::string folder_filter_;

    // --- floating panels (Bulk Add, Retry Lyrics) ------------------------
    // Unlike Console/Settings/Cheatsheet (full-screen overlays that
    // replace the whole view), these render *on top of* the still-live
    // Browse view behind them: render_frame() draws the normal
    // metadata/progress/search/list/status background exactly as always,
    // then this stamps a pre-built block of lines over it at an absolute
    // screen position via "\x1b[{row};{col}H" writes -- no clear, so
    // whatever was already drawn underneath stays visible around the
    // panel's edges. `lines` must already be exactly `panel_w` display
    // columns wide (pad_right them before calling) since this does no
    // width accounting of its own, just placement.
    //
    // Horizontally centered against the background's own width (W), not
    // the raw terminal width -- if the terminal's wider than W the
    // background content itself is left-anchored, and centering against
    // the full terminal would visually detach the panel from the
    // content it's supposed to be floating over. Vertically centered
    // against term_rows_, nudged up a few rows rather than dead-center.
    void draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W) const;
    static constexpr int kFloatingPanelUpShift = 3; // rows nudged above true vertical center

    // --- console / log overlay (HKeyConsole) ----------------------------
    // Backed by the global ConsoleLog (console_log.h/.cpp), which owns
    // both the on-disk $HOME/.cache/mousiki/logs/console.log and the
    // in-memory buffer this overlay renders -- see log_event() and
    // build_console_screen().
    void log_event(const std::string& msg);
    void build_console_screen(std::ostringstream& frame, int W, int target_height) const;

    // --- cheatsheet overlay (HKeyCheatsheet) ----------------------------
    void build_cheatsheet_screen(std::ostringstream& frame, int W) const;

    // The Console and Settings overlays must always be exactly as tall as
    // the Browse-mode player view actually renders at right now -- not
    // just "whatever fits the terminal" (that's term_rows_, an upper
    // bound, not the target). Rebuilds the same panels Browse mode would
    // and sums their line counts, using the *current* list_visible_rows_
    // (itself already term_rows_-aware) for the list/queue panel's share,
    // so this always matches frame-for-frame regardless of which panels
    // are currently enabled/how tall lyrics or disk art are configured.
    int player_view_height(int w) const;

    // --- hotkey rebinding conflict check --------------------------------
    // Returns the action name already bound to key_str (excluding
    // except_action), or "" if key_str is free. Used by the Settings
    // Reference tab so rebinding a hotkey to a key another action already
    // owns is rejected instead of silently creating an overlap.
    std::string hotkey_conflict(const std::string& key_str, const std::string& except_action) const;

    // --- autosave / session snapshot ------------------------------------
    // Consumed exactly once, right after a startup restore: the very
    // first launch_device_play_async() call for the restored track uses
    // this as its start position instead of 0.0, then zeroes it out so
    // every normal track change afterwards starts at 0 like always.
    double resume_start_sec_ = 0.0;
    // One-shot, like resume_start_sec_ above: the restored track comes up
    // paused, everything started afterwards plays normally.
    bool start_paused_ = false;
    std::chrono::steady_clock::time_point last_autosave_at_;
    // Drives the indicator's brief "something just saved" animation --
    // ticks for kAutosavePulseSeconds after each save, then goes idle.
    std::chrono::steady_clock::time_point autosave_pulse_started_at_;
    bool autosave_pulse_active_ = false;
    static constexpr double kAutosavePulseSeconds = 1.2;
    SnapshotData build_snapshot() const;
    // Applies a loaded snapshot: queue, play_mode, mute/volume, and kicks
    // off loading the saved "now playing" track at resume_start_sec_.
    // Called once at startup, before the render loop starts.
    void restore_snapshot(const SnapshotData& snap);
    // Called every frame from run(); saves (and pulses the indicator)
    // once settings_.autosave_delay_sec has elapsed since the last save.
    void maybe_autosave();
    // Builds the little "•" (or configured glyph) indicator string for
    // the volume-bar row, honoring AutoSave/AutoSaveIndicator/
    // AutoSaveIndicatorType. Returns "" when there's no room or the
    // feature's fully off (see build_progress_panel()'s comment on why
    // that also collapses the gap rather than just hiding a char in it).
    std::string autosave_indicator_glyph() const;

    // --- bulk add (paste a YouTube playlist link while Queue is
    // focused; hovering-song add on 'a' stays the List-panel behavior) --
    // Floating panel (see draw_floating_panel()), fixed total line count
    // across both phases -- unused rows are just blank-padded rather
    // than the panel changing size -- so its on-screen footprint never
    // moves/resizes frame to frame while open. Two phases:
    //   1. Input: bulk_add_results_ready_ == false -- typing the link.
    //   2. Results: fetch succeeded -- a compact starred checklist,
    //      navigable with Up/Down, toggled per-row with Space. "a" adds
    //      every fetched track regardless of star state ("ALL"); Enter
    //      adds only the starred ones ("[SELECT]").
    std::string bulk_add_buffer_;
    bool bulk_add_results_ready_ = false;
    std::vector<bool> bulk_add_selected_;   // parallel to pending_bulk_add_.items, default all-starred
    int bulk_add_cursor_ = 0;
    int bulk_add_scroll_ = 0;
    static constexpr int kBulkAddVisibleRows = 9; // preview list height cap -- this is what keeps the panel "tiny"
    struct BulkAddResult {
        bool success = false;
        std::string error;
        std::vector<OnlineResult> items;
    };
    std::thread bulk_add_thread_;
    std::mutex bulk_add_mutex_;
    std::atomic<bool> bulk_add_in_progress_{false};
    std::atomic<bool> bulk_add_ready_{false};
    BulkAddResult pending_bulk_add_;
    void launch_bulk_add_async(const std::string& url);
    void poll_pending_bulk_add();
    void commit_bulk_add(bool all); // all=true -> every fetched track; all=false -> only starred ones
    static constexpr int kBulkAddPanelWidth = 62; // matches the reference design exactly
    std::vector<std::string> build_bulk_add_panel() const; // returns fixed-width, fixed-height lines for draw_floating_panel()

    // --- retry lyrics (HKeyRetryLyrics, 'l') ------------------------------
    // A manual override form: rather than instantly re-fetching with the
    // track's own metadata, this lets the person edit the title/artist
    // mousiki searches with and tack on edit-qualifier tags (slowed,
    // reverb, ...) -- for tracks whose auto-fetched lyrics are wrong or
    // missing because the real upload's title doesn't match what's
    // playing. Opens pre-filled from the current track's metadata_ (with
    // a best-effort "ft./feat." split out of the title into its own
    // field) rather than blank.
    enum class RLField {
        Title, Artist, Ft,
        TypeReverb, TypeSlowed, TypeUltraSlowed, TypeSpedup, TypeRemix, TypeOther,
        RemixText, OtherText,
    };
    std::string rl_title_, rl_artist_, rl_ft_;
    std::string rl_remix_text_, rl_other_text_;
    // TypeSlowed/TypeUltraSlowed/TypeSpedup are mutually exclusive (a
    // radio group -- at most one true); TypeReverb/TypeRemix/TypeOther
    // are independent toggles, any combination.
    bool rl_reverb_ = false, rl_slowed_ = false, rl_ultra_slowed_ = false;
    bool rl_spedup_ = false, rl_remix_ = false, rl_other_ = false;
    RLField rl_focus_ = RLField::Title;
    static constexpr int kRetryLyricsPanelWidth = 62; // matches the reference design exactly
    // The fields actually navigable right now, in on-screen order --
    // RemixText/OtherText only appear in this list once their checkbox
    // is on, which is what makes them "dynamically available".
    std::vector<RLField> rl_visible_fields() const;
    void rl_open_from_current_track();     // pre-fill + reset state, called when 'l' opens the panel
    void rl_submit();                       // builds the override query and launches the fetch
    bool* rl_bool_ptr(RLField f);           // nullptr for non-checkbox fields
    std::string* rl_text_ptr(RLField f);    // nullptr for checkbox fields
    std::vector<std::string> build_retry_lyrics_panel() const; // fixed-width, fixed-height lines for draw_floating_panel()

    // --- async track loading ---
    struct PendingLoad {
        bool success = false;
        std::string title, artist, location_label, error;
        fs::path path;
        std::shared_ptr<StreamingPcm> pcm;
        size_t total_sec = 0;
        TrackMetadata metadata;
        bool is_local = true;   // for snapshot/resume identity -- which of path/video_id is authoritative
        std::string video_id;   // valid if !is_local
    };
    std::thread load_thread_;
    std::mutex load_mutex_;
    std::atomic<bool> load_ready_{false};
    std::atomic<bool> load_in_progress_{false};
    std::atomic<int> load_stage_{0};
    std::chrono::steady_clock::time_point load_started_at_;
    std::thread device_thread_;
    std::mutex device_mutex_;
    std::atomic<int> device_gen_{0}; // incremented each launch; stale threads abort when their gen != current
    void launch_device_play_async();

    PendingLoad pending_load_;
    void launch_load_async(fs::path local_path, std::string title, std::string artist,
                            std::string location_label, bool is_local, std::string video_id);
    void poll_pending_load();
    static void write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                       double t_probe, double t_total, const std::string& error);

    // --- deferred mini-waveform pass ---
    std::mutex waveform_mutex_;
    std::atomic<bool> waveform_pending_ready_{false};
    std::atomic<int> waveform_epoch_{0}; // incremented on each recompute; stale threads discard their result
    std::vector<float> pending_waveform_envelope_;
    void poll_pending_waveform();

    // --- async online search ---
    std::thread search_thread_;
    std::mutex search_mutex_;
    std::atomic<bool> search_ready_{false};
    std::atomic<bool> search_in_progress_{false};
    std::vector<OnlineResult> pending_search_results_;
    void launch_search_async(const std::string& query);
    void poll_pending_search();

    // --- async Spotify fetch --------------------------------------------
    // Every Spotify list is at least one network round-trip (playlists
    // and saved albums are paged until exhausted), so none of it belongs
    // on the render thread. One thread and one result slot serve every
    // level: only one list is on screen at a time, so a second fetch
    // starting means the first one's result is already unwanted.
    //
    // The result names the level it is for, so a fetch still in flight
    // when the user navigates away can't fill the wrong vector.
    struct SpotifyFetch {
        bool success = false;
        std::string error;
        SpotifyLevel level = SpotifyLevel::Tracks;
        std::string label;
        std::vector<SpotifyTrack> tracks;
        std::vector<SpotifyPlaylist> playlists;
        std::vector<SpotifyArtist> artists;
        std::vector<SpotifyAlbum> albums;
    };
    std::thread spotify_thread_;
    std::mutex spotify_mutex_;
    std::atomic<bool> spotify_fetch_ready_{false};
    std::atomic<bool> spotify_fetch_in_progress_{false};
    SpotifyFetch pending_spotify_fetch_;
    // Runs `work` on the fetch thread and publishes what it fills in, so
    // the eight sections don't need eight near-identical launchers.
    void launch_spotify_fetch(std::function<void(SpotifyFetch&)> work);
    void poll_pending_spotify_fetch();

    // --- Spotify now-playing poll ---------------------------------------
    // A heartbeat for as long as the Spotify view/playback is active,
    // sleeping settings_.spotify_poll_interval_ms between reads. Its own
    // thread because the fetch thread's work is one-shot.
    std::thread spotify_poll_thread_;
    std::mutex spotify_status_mutex_;
    std::atomic<bool> spotify_poll_stop_{false};
    std::atomic<bool> spotify_poll_running_{false};
    std::atomic<bool> spotify_status_ready_{false};
    SpotifyStatus pending_spotify_status_;
    std::vector<SpotifyTrack> pending_spotify_queue_;
    // Generation counter for polled state, incremented by every LOCAL
    // action that changes what should be playing. A status() request
    // started before such an action describes the world as it was and
    // must be thrown away -- applying it reports the PREVIOUS track as
    // newly playing, which flips the metadata panel back and fires a
    // lyrics fetch for the wrong song. Same guard the codebase already
    // uses for lyrics (lyrics_epoch_), waveforms (waveform_epoch_) and
    // audio device handoff (device_gen_).
    std::atomic<int> spotify_status_gen_{0};
    // The generation a published-but-not-yet-consumed reading was taken
    // under. The publish-side check alone leaves a hole: a reading
    // published microseconds BEFORE a local action is still sitting in
    // the slot afterwards, and gets consumed as if it were current.
    int pending_spotify_status_gen_ = 0;

    // The uri we asked Spotify to play, and how long to keep expecting
    // it. A generation guard alone isn't enough: a poll started AFTER
    // the command can still read Spotify's state before Spotify has
    // finished switching tracks, which looks identical to "the user
    // changed tracks on their phone". While this is set, polled track
    // identity is ignored (position/volume/device are still taken) until
    // Spotify confirms this uri. The deadline exists so a play that
    // never lands can't wedge the UI into ignoring reality forever.
    // The track whose lyrics are loaded or currently being fetched.
    //
    // Both the local play path and the poll path can legitimately decide
    // "the track changed", and reasoning about which fires first (and
    // about prev_uri, and about Spotify's own URI relinking, which can
    // hand back a different uri than the one requested) proved to be a
    // losing game -- it produced two fetches of the same lyrics, three
    // provider requests each. Keying the decision on identity instead of
    // on ordering makes a duplicate structurally impossible: whoever
    // gets there first records the uri, and nobody refetches it.
    std::string spotify_lyrics_uri_;

    std::string spotify_expected_uri_;
    std::chrono::steady_clock::time_point spotify_expected_until_;
    static constexpr double kSpotifyExpectSeconds = 12.0;

    // --- async Spotify commands ------------------------------------------
    // Measured on a real account: PUT /me/player/play takes 400-1100ms,
    // and play_uri() can need three round-trips (play -> 404 -> list
    // devices -> play with device_id), over two seconds in total. On the
    // render thread that would freeze the UI for every track change.
    //
    // A queue rather than a single slot, because transport keys get
    // mashed (volume, seek) and dropping presses while one is in flight
    // feels broken. One long-lived worker drains it in order, so
    // commands can't race each other either.
    std::thread spotify_cmd_thread_;
    std::mutex spotify_cmd_mutex_;
    std::condition_variable spotify_cmd_cv_;
    std::deque<std::function<void()>> spotify_cmd_queue_;
    std::atomic<bool> spotify_cmd_stop_{false};
    std::atomic<bool> spotify_cmd_started_{false};
    // Worker -> UI channel. The worker must never touch status_line_ or
    // log_event() directly; both are read by the render thread.
    std::mutex spotify_msg_mutex_;
    std::vector<std::string> spotify_msgs_;
    void spotify_post_message(const std::string& msg);
    void poll_pending_spotify_messages();
    void spotify_run_command(std::function<void()> work);
    void stop_spotify_cmd_worker();

    void start_spotify_poll();
    void stop_spotify_poll();
    void poll_pending_spotify_status();

    // --- Spotify sign-in -------------------------------------------------
    // SpotifyAuth::interactive_login() blocks on a loopback socket for up
    // to five minutes waiting for the browser redirect, so it never runs
    // on the UI thread.
    std::thread spotify_auth_thread_;
    std::atomic<bool> spotify_auth_in_progress_{false};
    std::atomic<bool> spotify_auth_done_{false};
    std::mutex spotify_auth_mutex_;
    std::string spotify_auth_error_;
    bool spotify_auth_success_ = false;
    void launch_spotify_sign_in();
    void poll_pending_spotify_auth();

    // --- Spotify actions -------------------------------------------------
    // Builds the backend if it isn't built yet. False means it can't be
    // built right now, with spotify_unavailable_reason_ set to why.
    bool ensure_spotify_client();
    void enter_spotify();              // the Spotify hotkey
    void spotify_load_home();          // (re)populate the section menu
    void spotify_open_section(SpotifyHomeSection section);
    void spotify_activate_selected();  // Enter
    void spotify_enqueue_selected();   // the add-to-queue hotkey
    bool spotify_back();               // Esc -- true when it consumed the key
    void spotify_play_track(const SpotifyTrack& t);
    // Mirrors spotify_status_ into metadata_ so the metadata panel needs
    // no Spotify branch. The file-shaped rows (sampling, format, size)
    // say there is no file, rather than showing a dash that would read
    // as "we failed to read it".
    void apply_spotify_metadata();
    // True while Spotify, not player_, is the authority on what's
    // playing. Both panels and the progress bar read through these.
    bool showing_track() const { return has_track_ || spotify_playing_; }
    double display_elapsed_sec() const;
    size_t display_total_sec() const;
    // Hands playback to Spotify. Only one of the two may play at a time,
    // and the Spotify path can't pause mousiki's device itself.
    void yield_playback_to_spotify();
    void launch_spotify_search(const std::string& query);
    // Row count of whichever Spotify level is showing.
    size_t spotify_level_len() const;
    std::vector<std::string> build_spotify_list_rows(int inner) const;

    // --- settings panel (5 tabs: Colors, On/Off, Animation, Reference, About App) ---
    // Rendering uses absolute cursor positioning (\x1b[y;xH) rather than
    // building padded strings line by line -- each field goes exactly
    // where it's told regardless of what else is on that row, which is
    // what actually fixes the truncation-corrupts-everything fragility
    // class of bug (a mis-sized pad on one row used to bleed into
    // whatever the next escape code was).
    Settings settings_;
    static constexpr int kSettingsTabCount = 5; // Colors, On/Off, Animation, Reference, About App
    int settings_tab_ = 0;
    int settings_row_ = 0;   // resets to 0 on every tab switch
    int settings_col_ = 0;   // 0 or 1 -- only the Colors tab has 2-cell rows
    std::string color_edit_buffer_;      // live text while mode_==ColorEdit
    // Returns the current value of (tab, row, col) as plain text, for
    // display and as the starting buffer when editing.
    // Returns a pointer to the color field for (row, col) on the Colors
    // tab (tab 0), or nullptr if that row/col isn't a real cell.
    std::string* color_field_ptr(int row, int col);
    std::string settings_get_value(int row, int col) const;
    // Commits color_edit_buffer_ into (settings_tab_, settings_row_,
    // settings_col_). Colors are clamped/validated as a 0-255 code;
    // everything else is stored close to verbatim.
    void settings_commit_edit();
    // Left/Right quick-cycle for rows with a fixed set of options (bools,
    // enums). No-op for rows that don't have one (colors, hotkeys) --
    // those are Enter-to-type only.
    void settings_cycle(int dir);
    std::vector<std::string> settings_options_for(int tab, int row) const;
    int settings_max_row() const; // last valid row index for the current tab
    // (main_frame_height() was removed -- see the comment where it used to
    // live in app.cpp, right before build_settings_screen(). Every overlay
    // now sizes off term_rows_ directly instead.)
    void build_settings_screen(std::ostringstream& frame, int W, int player_h) const;
    void handle_settings_key(int key);

    // Max row count per tab (set in build_settings_screen)


    // --- hotkey support ---
    // Resolves a key code from poll_key() to the hotkey action name.
    // Returns empty string if no match.
    std::string resolve_hotkey_action(int key) const;
    // Returns the key code that a hotkey string maps to for poll_key().
    static int hotkey_string_to_key(const std::string& s);

    // --- helpers ---
    void refresh_local_view();
    void update_live_search_preview();
    std::vector<LocalTrack> filter_and_rank_local(const std::string& query) const;
    void apply_local_sort(std::vector<LocalTrack>& tracks) const;
    static const char* sort_mode_name(int mode);
    void submit_search();
    void start_local_track(const LocalTrack& track);
    void start_online_track(const OnlineResult& result);
    void play_selected();
    // `precomputed_base` lets a caller supply the "advance from" index
    // directly instead of having this look it up itself via
    // current_track_list_index() -- needed by advance_track(), which
    // must clear has_track_ before calling this (see its own comment for
    // why), but current_track_list_index() requires has_track_ still be
    // true to identify anything. kNoPrecomputedBase (the default) means
    // "look it up yourself" -- used by every other caller (the manual
    // "n"/"b" keys), where has_track_ is still perfectly valid.
    static constexpr int kNoPrecomputedBase = -2;
    void play_relative(int delta, int precomputed_base = kNoPrecomputedBase);
    void play_relative_random(int precomputed_base = kNoPrecomputedBase);
    void play_at_list_index(int idx); // plays without moving the hover cursor -- see its own comment in app.cpp
    void advance_track();
    // Where the currently-playing track sits within *this list source's*
    // current view (local_view_ or online_view_, whichever list_source_
    // is showing), by identity match (path for local, video_id for
    // online) rather than by whatever row happens to be hovered. -1 if
    // nothing's playing, or what's playing came from a different source
    // than the one currently displayed (e.g. playing local while
    // browsing online results) -- there's no meaningful "relative to
    // current" position in that case. This is what play_relative() uses
    // instead of the hover cursor, so "next" always means "next after
    // what's actually playing", not "next after wherever you happen to
    // be looking".
    int current_track_list_index() const;
    // Row count of the list panel's current source, in one place: the
    // "local_view_ : online_view_" ternary it replaces would have treated
    // Spotify as Online at all eight of its call sites.
    size_t current_list_len() const;
    // Pops (or, in Repeat Queue mode, rotates to the back instead of
    // discarding) the next item to play from queue_, honoring the
    // current play_mode: Shuffle picks a random queue item rather than
    // strictly FIFO order, Repeat Queue keeps the queue looping
    // indefinitely instead of draining it. Shared by advance_track()
    // (auto-advance on finish) and the manual "n" key (explicit skip),
    // so both respect the queue exactly the same way. Caller must check
    // !queue_.empty() first.
    void play_next_from_queue();
    // Single letter for the mode-indicator button after the search bar:
    // L=list, R=repeat, S=shuffle, Q=repeat queue, O=stop (play-and-stop
    // -- not "S", that's shuffle's letter already).
    char play_mode_letter() const;
    void queue_add_selected();
    void queue_remove_last();
    void queue_remove_hovering();
    void queue_move_hovering(int dir); // dir=-1 up, +1 down
    void clamp_queue_selected();
    void handle_key(int key);
    void ensure_visible_row_meta();
    void recompute_waveform_for_current_track();
    std::string render_frame(TerminalIO& term);

    // --- box drawing helpers (use configured border chars) ---
    std::string box_top(const std::string& label, int total_width, const std::string& border_ansi = "") const;
    std::string box_bottom(int total_width, const std::string& footer = "", const std::string& border_ansi = "") const;
    std::string box_line(const std::string& content, int total_width, const std::string& border_ansi = "") const;

    // panel builders
    std::vector<std::string> build_metadata_panel(int width) const;
    std::vector<std::string> build_progress_panel(int width) const;
    std::vector<std::string> build_search_bar(int width) const;
    std::vector<std::string> build_list_panel(int width, int height) const;
    std::vector<std::string> build_queue_panel(int width, int height) const;
};

} // namespace muisc
