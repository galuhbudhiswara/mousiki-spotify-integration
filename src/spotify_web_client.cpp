#include "spotify_web_client.h"

#include <algorithm>
#include <cmath>

#include "console_log.h"
#include "http_client.h"
#include "tiny_json.h"

namespace muisc {

SpotifyWebClient::SpotifyWebClient(std::string client_id, SpotifyTokens tokens)
    : client_id_(std::move(client_id)), tokens_(std::move(tokens)) {}

// ---------------------------------------------------------------------
// Request layer
// ---------------------------------------------------------------------

bool SpotifyWebClient::request_with_refresh(const std::string& method,
                                            const std::string& path,
                                            const std::string& body,
                                            std::string& body_out,
                                            std::string* error_out) {
    // Refresh ahead of the 401: expires_at is persisted, so a token
    // already past (or within the skew of) its expiry can be renewed
    // without spending a round-trip to be told.
    if (tokens_.expired() && !tokens_.refresh_token.empty()) {
        std::string ignored;
        SpotifyAuth::refresh(client_id_, tokens_, &ignored);
    }

    const std::string url = std::string(kApiBase) + path;

    auto perform = [&](const std::string& token) {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer " + token},
            {"Content-Type", "application/json; charset=utf-8"},
        };
        return http_request(method, url, headers, body);
    };

    HttpResponse resp = perform(tokens_.access_token);
    if (!resp.transport_ok) {
        if (error_out) *error_out = "network error: " + resp.err;
        return false;
    }

    // 401 -> refresh once, replay once. Never more than once.
    if (resp.status == 401) {
        std::string refresh_err;
        if (!SpotifyAuth::refresh(client_id_, tokens_, &refresh_err)) {
            if (error_out) *error_out = "session expired and refresh failed: " + refresh_err;
            return false;
        }
        resp = perform(tokens_.access_token);
        if (!resp.transport_ok) {
            if (error_out) *error_out = "network error: " + resp.err;
            return false;
        }
        if (resp.status == 401) {
            if (error_out) *error_out = "Spotify rejected the refreshed token -- sign in again";
            return false;
        }
    }

    if (resp.status == 204) { // transport endpoints' success case
        body_out.clear();
        return true;
    }
    if (resp.status >= 200 && resp.status < 300) {
        body_out = resp.body;
        return true;
    }

    // Map the statuses a user can actually act on to plain language.
    if (error_out) {
        if (resp.status == 403) {
            *error_out = "Spotify refused this (playback control requires Premium)";
        } else if (resp.status == 404) {
            *error_out = "no active Spotify device -- start playback on a device first";
        } else if (resp.status == 429) {
            *error_out = "rate limited by Spotify -- try again shortly";
        } else {
            std::string detail;
            tinyjson::Value v;
            if (tinyjson::parse(resp.body, v)) {
                if (const auto* e = v.find("error")) {
                    if (const auto* m = e->find("message")) detail = m->as_string();
                }
            }
            *error_out = "Spotify HTTP " + std::to_string(resp.status) +
                         (detail.empty() ? "" : ": " + detail);
        }
    }
    return false;
}

bool SpotifyWebClient::get_all_pages(const std::string& path_base, int limit,
                                     std::vector<std::string>& page_bodies_out,
                                     std::string* error_out) {
    int offset = 0;
    int pages_fetched = 0;
    constexpr int kMaxPages = 40; // 40 * 100 = 4000 items, plenty, and bounded

    while (pages_fetched < kMaxPages) {
        const char sep = (path_base.find('?') == std::string::npos) ? '?' : '&';
        const std::string path = path_base + sep +
                                 "limit=" + std::to_string(limit) +
                                 "&offset=" + std::to_string(offset);
        std::string body;
        if (!request_with_refresh("GET", path, "", body, error_out)) return false;

        tinyjson::Value v;
        if (!tinyjson::parse(body, v)) {
            if (error_out) *error_out = "could not parse Spotify response";
            return false;
        }
        page_bodies_out.push_back(body);
        ++pages_fetched;

        double total = 0;
        if (const auto* t = v.find("total")) total = t->as_number(0);
        offset += limit;
        if (offset >= static_cast<int>(total)) break;
    }
    return true;
}

// Picks which copy of an album cover to download from the `images`
// array. Spotify documents these as ordered widest-first (640, 300, 64)
// but guarantees nothing, so this scans rather than indexing. The disk
// renders at 60x60 dots, which the 300px copy feeds with detail to
// spare at a fraction of the 640's bytes -- so: the smallest image still
// at or above the target, falling back to the largest of whatever is
// left when every copy is smaller than that.
static std::string pick_album_art(const tinyjson::Value& album) {
    const auto* images = album.find("images");
    if (!images || images->type != tinyjson::Type::Array) return "";

    constexpr double kPreferredPx = 300.0;
    std::string best, fallback;
    double best_w = 0.0, fallback_w = -1.0;

    for (const auto& im : images->arr) {
        const auto* u = im.find("url");
        if (!u) continue;
        const std::string url = u->as_string();
        if (url.empty()) continue;
        const auto* w_val = im.find("width");
        const double w = w_val ? w_val->as_number(0.0) : 0.0;

        if (w >= kPreferredPx && (best.empty() || w < best_w)) { best = url; best_w = w; }
        if (w > fallback_w) { fallback = url; fallback_w = w; }
    }
    return best.empty() ? fallback : best;
}

// ---------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------

bool SpotifyWebClient::status(SpotifyStatus& out, std::string* error_out) {
    std::string body;
    if (!request_with_refresh("GET", "me/player", "", body, error_out)) return false;

    // GET /me/player answers 204 with an empty body when nothing is
    // playing anywhere. That is "not running", not an error.
    if (body.empty()) {
        out = SpotifyStatus{};
        out.is_running = false;
        if (error_out) *error_out = "Spotify isn't playing on any device";
        return false;
    }

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse playback state";
        return false;
    }

    out = SpotifyStatus{};
    out.is_running = true;

    if (const auto* dev = v.find("device")) {
        if (const auto* vol = dev->find("volume_percent")) out.player.volume = static_cast<int>(vol->as_number(0));
        if (const auto* dn = dev->find("name")) out.device = dn->as_string();
    }
    if (const auto* p = v.find("progress_ms")) out.player.position_ms = static_cast<long long>(p->as_number(0));
    if (const auto* playing = v.find("is_playing")) out.player.state = playing->as_bool(false) ? "playing" : "paused";
    else out.player.state = "paused";
    if (const auto* r = v.find("repeat_state")) out.player.is_repeating = (r->as_string("off") != "off");
    if (const auto* s = v.find("shuffle_state")) out.player.is_shuffling = s->as_bool(false);

    if (const auto* item = v.find("item")) {
        if (const auto* n = item->find("name")) out.track.name = n->as_string();
        if (const auto* u = item->find("uri")) out.track.uri = u->as_string();
        if (const auto* d = item->find("duration_ms")) out.player.duration_ms = static_cast<long long>(d->as_number(0));
        if (const auto* al = item->find("album")) {
            if (const auto* an = al->find("name")) out.track.album = an->as_string();
            out.track.art_url = pick_album_art(*al);
        }
        // Spotify returns every credited artist; join them the way the
        // extension's artistsToArtist() helper did.
        if (const auto* arts = item->find("artists")) {
            std::string joined;
            for (const auto& a : arts->arr) {
                if (const auto* an = a.find("name")) {
                    if (!joined.empty()) joined += ", ";
                    joined += an->as_string();
                }
            }
            out.track.artist = joined;
        }
    }

    last_known_player_ = out.player;
    have_last_known_ = true;
    return true;
}

// ---------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------

bool SpotifyWebClient::play(std::string* error_out) {
    std::string body;
    return request_with_refresh("PUT", "me/player/play", "{}", body, error_out);
}

bool SpotifyWebClient::pause(std::string* error_out) {
    std::string body;
    return request_with_refresh("PUT", "me/player/pause", "", body, error_out);
}

bool SpotifyWebClient::play_pause(std::string* error_out) {
    // The Web API has no toggle endpoint, so the direction has to come
    // from known state. Prefer the cached snapshot; fall back to asking.
    if (!have_last_known_) {
        SpotifyStatus st;
        if (!status(st, error_out)) return false;
    }
    return last_known_player_.state == "playing" ? pause(error_out) : play(error_out);
}

bool SpotifyWebClient::next(std::string* error_out) {
    std::string body;
    return request_with_refresh("POST", "me/player/next", "", body, error_out);
}

bool SpotifyWebClient::previous(std::string* error_out) {
    std::string body;
    return request_with_refresh("POST", "me/player/previous", "", body, error_out);
}

bool SpotifyWebClient::set_volume(int percent, std::string* error_out) {
    const int clamped = std::clamp(percent, 0, 100);
    if (clamped != 0) pre_mute_volume_ = clamped;
    std::string body;
    return request_with_refresh(
        "PUT", "me/player/volume?volume_percent=" + std::to_string(clamped), "", body, error_out);
}

bool SpotifyWebClient::seek(long long position_ms, std::string* error_out) {
    if (position_ms < 0) position_ms = 0;
    std::string body;
    return request_with_refresh(
        "PUT", "me/player/seek?position_ms=" + std::to_string(position_ms), "", body, error_out);
}

bool SpotifyWebClient::toggle_shuffle(std::string* error_out) {
    if (!have_last_known_) {
        SpotifyStatus st;
        if (!status(st, error_out)) return false;
    }
    const bool next_state = !last_known_player_.is_shuffling;
    std::string body;
    if (!request_with_refresh("PUT",
            std::string("me/player/shuffle?state=") + (next_state ? "true" : "false"),
            "", body, error_out))
        return false;
    last_known_player_.is_shuffling = next_state;
    return true;
}

bool SpotifyWebClient::toggle_repeat(std::string* error_out) {
    if (!have_last_known_) {
        SpotifyStatus st;
        if (!status(st, error_out)) return false;
    }
    // Spotify has three repeat states (off/context/track), but mousiki's
    // own play_mode is two-state, so this flips off<->context only.
    const char* next_state = last_known_player_.is_repeating ? "off" : "context";
    std::string body;
    if (!request_with_refresh("PUT", std::string("me/player/repeat?state=") + next_state,
                              "", body, error_out))
        return false;
    last_known_player_.is_repeating = !last_known_player_.is_repeating;
    return true;
}

bool SpotifyWebClient::devices(std::vector<SpotifyDevice>& out, std::string* error_out) {
    out.clear();
    std::string body;
    if (!request_with_refresh("GET", "me/player/devices", "", body, error_out)) return false;
    if (body.empty()) return true;

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse device list";
        return false;
    }
    const auto* arr = v.find("devices");
    if (!arr) return true;
    for (const auto& d : arr->arr) {
        SpotifyDevice dev;
        if (const auto* x = d.find("id")) dev.id = x->as_string();
        if (const auto* x = d.find("name")) dev.name = x->as_string();
        if (const auto* x = d.find("type")) dev.type = x->as_string();
        if (const auto* x = d.find("is_active")) dev.is_active = x->as_bool(false);
        if (const auto* x = d.find("is_restricted")) dev.is_restricted = x->as_bool(false);
        if (const auto* x = d.find("volume_percent")) dev.volume_percent = static_cast<int>(x->as_number(0));
        // A device with no id cannot be targeted, so listing it would
        // only offer the user something that can't work.
        if (!dev.id.empty()) out.push_back(std::move(dev));
    }
    return true;
}

bool SpotifyWebClient::play_uri(const std::string& uri, std::string* error_out,
                                std::string* device_used_out) {
    // A track URI goes in "uris"; a playlist/album/artist URI is a
    // "context_uri" -- sending the wrong one is a 400.
    const bool is_track = uri.rfind("spotify:track:", 0) == 0;
    std::string json;
    if (is_track) {
        json = "{\"uris\":[\"" + tinyjson::escape(uri) + "\"]}";
    } else {
        json = "{\"context_uri\":\"" + tinyjson::escape(uri) + "\"}";
    }
    return start_playback(json, error_out, device_used_out);
}

bool SpotifyWebClient::play_uris(const std::vector<std::string>& uris, int offset_index,
                                 std::string* error_out, std::string* device_used_out) {
    if (uris.empty()) {
        if (error_out) *error_out = "no tracks to play";
        return false;
    }
    // Spotify rejects a list longer than 100 outright rather than
    // truncating it, and an offset outside the list is a 400 too.
    const std::size_t kMaxUris = 100;
    const std::size_t count = std::min(uris.size(), kMaxUris);
    const int offset = std::clamp(offset_index, 0, static_cast<int>(count) - 1);

    std::string json = "{\"uris\":[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i) json += ",";
        json += "\"" + tinyjson::escape(uris[i]) + "\"";
    }
    json += "],\"offset\":{\"position\":" + std::to_string(offset) + "}}";
    return start_playback(json, error_out, device_used_out);
}

bool SpotifyWebClient::start_playback(const std::string& json, std::string* error_out,
                                      std::string* device_used_out) {
    std::string body;
    std::string first_err;
    if (request_with_refresh("PUT", "me/player/play", json, body, &first_err)) return true;

    // 404 NO_ACTIVE_DEVICE is the usual answer, and it isn't a real
    // failure: a device-less play command goes to the active device, and
    // a Spotify client that is open but idle isn't active -- which is
    // how a terminal remote control finds the account most of the time.
    //
    // So name a device explicitly, the way "transfer playback to this
    // device" does in Spotify's own clients.
    if (first_err.find("no active Spotify device") == std::string::npos) {
        if (error_out) *error_out = first_err; // a genuine failure (403 Premium, 400, network)
        return false;
    }

    std::vector<SpotifyDevice> devs;
    std::string dev_err;
    if (!devices(devs, &dev_err) || devs.empty()) {
        if (error_out) {
            *error_out = devs.empty() && dev_err.empty()
                ? "no Spotify device found -- open Spotify on your phone or desktop first"
                : dev_err;
        }
        return false;
    }

    // Preference order: whatever Spotify already considers active, then
    // a general-purpose client, then anything controllable. Restricted
    // devices accept no API control, so they are skipped outright.
    const SpotifyDevice* pick = nullptr;
    for (const auto& d : devs) {
        if (d.is_restricted) continue;
        if (d.is_active) { pick = &d; break; }
        const bool general = (d.type == "Computer" || d.type == "Smartphone" || d.type == "Tablet");
        if (!pick || (general && !(pick->type == "Computer" || pick->type == "Smartphone" || pick->type == "Tablet"))) {
            pick = &d;
        }
    }
    if (!pick) {
        if (error_out)
            *error_out = "found " + std::to_string(devs.size()) +
                         " Spotify device(s) but none accept remote control";
        return false;
    }

    body.clear();
    if (!request_with_refresh("PUT", "me/player/play?device_id=" + http_url_encode(pick->id),
                              json, body, error_out)) {
        return false;
    }
    if (device_used_out) *device_used_out = pick->name;
    return true;
}

// ---------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------

bool SpotifyWebClient::playlists(std::vector<SpotifyPlaylist>& out, std::string* error_out) {
    out.clear();
    std::vector<std::string> pages;
    if (!get_all_pages("me/playlists", 50, pages, error_out)) return false;

    for (const auto& page : pages) {
        tinyjson::Value v;
        if (!tinyjson::parse(page, v)) continue;
        const auto* items = v.find("items");
        if (!items) continue;
        for (const auto& it : items->arr) {
            SpotifyPlaylist pl;
            if (const auto* x = it.find("id")) pl.id = x->as_string();
            if (const auto* x = it.find("name")) pl.name = x->as_string();
            if (const auto* x = it.find("uri")) pl.uri = x->as_string();
            if (const auto* ow = it.find("owner")) {
                if (const auto* x = ow->find("id")) pl.owner_id = x->as_string();
            }
            // The track-count stub was renamed "tracks" -> "items" in
            // the February 2026 revision. Read both so the count never
            // silently renders as 0.
            const auto* stub = it.find("items");
            if (!stub) stub = it.find("tracks");
            if (stub) {
                if (const auto* x = stub->find("total")) pl.track_count = static_cast<int>(x->as_number(0));
            }
            if (!pl.id.empty()) out.push_back(std::move(pl));
        }
    }
    return true;
}

// Joins an "artists" array into "A, B, C". Spotify always sends an array
// here even for a single artist.
static std::string join_artist_names(const tinyjson::Value& artists) {
    std::string joined;
    for (const auto& a : artists.arr) {
        if (const auto* an = a.find("name")) {
            if (!joined.empty()) joined += ", ";
            joined += an->as_string();
        }
    }
    return joined;
}

// Parses one *bare* track object. Spotify is inconsistent about this:
// some collections hand back the track directly (search results,
// /me/top/tracks, /me/player/queue, an album's tracks) and others wrap
// it in an item envelope (/playlists/{id}/tracks, /me/tracks,
// /me/player/recently-played all nest it under "track"). This handles
// the inner object; collect_tracks_from_items() below unwraps the
// envelope form and delegates here, so there is one parser either way.
//
// Returns false for anything not actually playable -- local-only files
// and tracks removed from the catalogue come back with no uri, and
// listing them would produce rows that silently do nothing on Enter.
static bool parse_track(const tinyjson::Value& t, SpotifyTrack& tr) {
    if (t.type != tinyjson::Type::Object) return false;
    if (const auto* x = t.find("id")) tr.id = x->as_string();
    if (const auto* x = t.find("uri")) tr.uri = x->as_string();
    if (const auto* x = t.find("name")) tr.name = x->as_string();
    if (const auto* x = t.find("duration_ms")) tr.duration_ms = static_cast<long long>(x->as_number(0));
    if (const auto* al = t.find("album")) {
        if (const auto* x = al->find("name")) tr.album = x->as_string();
        tr.art_url = pick_album_art(*al);
    }
    if (const auto* arts = t.find("artists")) tr.artist = join_artist_names(*arts);
    // Playlists can hold podcast episodes, and local-only files and
    // delisted tracks come back with no uri. Requiring a spotify:track:
    // uri rejects all three in one check -- an episode would otherwise
    // list fine and then fail on Enter, since play_uri() sends it as a
    // track uri.
    return !tr.name.empty() && tr.uri.rfind("spotify:track:", 0) == 0;
}

// Walks a collection whose items nest the real track one level down.
//
// The envelope key is NOT the same across endpoints any more. Spotify's
// February 2026 revision renamed the playlist item endpoints
// (/playlists/{id}/tracks -> /items) and renamed the nested object with
// them: "track" became "item". /me/tracks, /me/albums and
// /me/player/recently-played were NOT changed and still nest under
// "track" (verified against the live API, not inferred from the
// changelog). So both spellings are live simultaneously.
//
// "item" is tried first because the inner track object itself also has a
// boolean field literally named "track" (its track-vs-episode
// discriminator); on a renamed payload, looking for "track" at the
// envelope level finds nothing, but order still matters for clarity.
static void collect_tracks_from_items(const tinyjson::Value& items, std::vector<SpotifyTrack>& out) {
    for (const auto& it : items.arr) {
        const tinyjson::Value* t = it.find("item");
        if (!t || t->type != tinyjson::Type::Object) t = it.find("track");
        if (!t || t->type != tinyjson::Type::Object) continue;
        SpotifyTrack tr;
        if (parse_track(*t, tr)) out.push_back(std::move(tr));
    }
}

// Walks a collection of bare track objects.
static void collect_bare_tracks(const tinyjson::Value& arr, std::vector<SpotifyTrack>& out) {
    for (const auto& t : arr.arr) {
        SpotifyTrack tr;
        if (parse_track(t, tr)) out.push_back(std::move(tr));
    }
}

bool SpotifyWebClient::playlist_tracks(const SpotifyPlaylist& pl, std::vector<SpotifyTrack>& out,
                                       std::string* error_out) {
    out.clear();
    if (pl.id.empty()) {
        if (error_out) *error_out = "playlist has no id";
        return false;
    }

    // Path and `fields` spelling both follow Spotify's February 2026
    // rename: /playlists/{id}/tracks now answers 403, and the nested
    // object inside each item is "item" rather than "track" -- so a
    // fields mask written against the old name trims the response down
    // to empty objects instead of failing loudly.
    //
    // `fields` itself still matters: these playlists run to thousands of
    // entries and the unfiltered payload is enormous.
    const std::string base =
        "playlists/" + pl.id + "/items?fields=" +
        http_url_encode("items(item(id,name,uri,duration_ms,album(name,images),artists(name))),limit,offset,total");

    std::vector<std::string> pages;
    if (!get_all_pages(base, 100, pages, error_out)) return false;

    for (const auto& page : pages) {
        tinyjson::Value v;
        if (!tinyjson::parse(page, v)) continue;
        if (const auto* items = v.find("items")) collect_tracks_from_items(*items, out);
    }
    return true;
}

bool SpotifyWebClient::saved_tracks(std::vector<SpotifyTrack>& out, std::string* error_out) {
    out.clear();
    const std::string base =
        "me/tracks?fields=" +
        http_url_encode("items(track(id,name,uri,duration_ms,album(name,images),artists(name))),limit,offset,total");

    std::vector<std::string> pages;
    if (!get_all_pages(base, 50, pages, error_out)) return false;

    for (const auto& page : pages) {
        tinyjson::Value v;
        if (!tinyjson::parse(page, v)) continue;
        if (const auto* items = v.find("items")) collect_tracks_from_items(*items, out);
    }
    return true;
}


// ---------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------

bool SpotifyWebClient::search_tracks(const std::string& query, int limit,
                                     std::vector<SpotifyTrack>& out, std::string* error_out) {
    out.clear();
    if (query.empty()) {
        if (error_out) *error_out = "empty search query";
        return false;
    }
    // Spotify's February 2026 revision cut the search limit maximum from
    // 50 to 10 (and the default from 20 to 5). Anything above 10 is a
    // hard 400 "Invalid limit", so this clamp is what keeps search
    // working at all -- it is not a politeness measure.
    const int capped = std::clamp(limit, 1, kSearchLimitMax);
    const std::string path = "search?q=" + http_url_encode(query) +
                             "&type=track&limit=" + std::to_string(capped);

    std::string body;
    if (!request_with_refresh("GET", path, "", body, error_out)) return false;

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse Spotify search response";
        return false;
    }
    // Results are nested one level deeper than every other collection:
    // the response is keyed by the requested type(s).
    if (const auto* tracks = v.find("tracks")) {
        if (const auto* items = tracks->find("items")) collect_bare_tracks(*items, out);
    }
    return true;
}

// ---------------------------------------------------------------------
// Queue -- the account's real up-next list, shared with every other
// Spotify client signed in to it.
// ---------------------------------------------------------------------

bool SpotifyWebClient::queue(std::vector<SpotifyTrack>& out, std::string* error_out) {
    out.clear();
    std::string body;
    if (!request_with_refresh("GET", "me/player/queue", "", body, error_out)) return false;
    if (body.empty()) return true; // 204: nothing playing, so nothing queued

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse Spotify queue response";
        return false;
    }
    // Deliberately only "queue", not "currently_playing": this feeds the
    // UP NEXT panel, and the now-playing track already has its own home
    // in the metadata panel via status(). Including it would show the
    // current track as the first thing up next.
    if (const auto* q = v.find("queue")) collect_bare_tracks(*q, out);
    return true;
}

bool SpotifyWebClient::enqueue(const std::string& uri, std::string* error_out) {
    if (uri.empty()) {
        if (error_out) *error_out = "no track uri to enqueue";
        return false;
    }
    std::string body;
    return request_with_refresh("POST", "me/player/queue?uri=" + http_url_encode(uri),
                                "", body, error_out);
}

// ---------------------------------------------------------------------
// Home-screen collections
// ---------------------------------------------------------------------

bool SpotifyWebClient::recently_played(int limit, std::vector<SpotifyTrack>& out,
                                       std::string* error_out) {
    out.clear();
    // Hard-capped at 50 by Spotify, and cursor-paged rather than
    // offset-paged -- get_all_pages() can't walk it, and 50 is more than
    // a "jump back in" row needs anyway.
    const int capped = std::clamp(limit, 1, 50);
    std::string body;
    if (!request_with_refresh("GET", "me/player/recently-played?limit=" + std::to_string(capped),
                              "", body, error_out)) return false;
    if (body.empty()) return true;

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse recently-played response";
        return false;
    }
    if (const auto* items = v.find("items")) collect_tracks_from_items(*items, out);
    return true;
}

// Spotify rejects any time_range outside these three, so an unrecognised
// value is corrected here rather than turned into a 400 the user would
// see as an opaque "Spotify HTTP 400".
static std::string valid_time_range(const std::string& r) {
    if (r == "short_term" || r == "medium_term" || r == "long_term") return r;
    return "medium_term";
}

bool SpotifyWebClient::top_tracks(const std::string& time_range, int limit,
                                  std::vector<SpotifyTrack>& out, std::string* error_out) {
    out.clear();
    const int capped = std::clamp(limit, 1, 50);
    const std::string path = "me/top/tracks?time_range=" + valid_time_range(time_range) +
                             "&limit=" + std::to_string(capped);
    std::string body;
    if (!request_with_refresh("GET", path, "", body, error_out)) return false;

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse top-tracks response";
        return false;
    }
    // Bare tracks here, unlike /me/tracks which wraps them.
    if (const auto* items = v.find("items")) collect_bare_tracks(*items, out);
    return true;
}

bool SpotifyWebClient::top_artists(const std::string& time_range, int limit,
                                   std::vector<SpotifyArtist>& out, std::string* error_out) {
    out.clear();
    const int capped = std::clamp(limit, 1, 50);
    const std::string path = "me/top/artists?time_range=" + valid_time_range(time_range) +
                             "&limit=" + std::to_string(capped);
    std::string body;
    if (!request_with_refresh("GET", path, "", body, error_out)) return false;

    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse top-artists response";
        return false;
    }
    const auto* items = v.find("items");
    if (!items) return true;
    for (const auto& it : items->arr) {
        SpotifyArtist ar;
        if (const auto* x = it.find("id")) ar.id = x->as_string();
        if (const auto* x = it.find("uri")) ar.uri = x->as_string();
        if (const auto* x = it.find("name")) ar.name = x->as_string();
        if (const auto* x = it.find("popularity")) ar.popularity = static_cast<int>(x->as_number(0));
        if (const auto* g = it.find("genres")) {
            for (const auto& gv : g->arr) {
                if (gv.type != tinyjson::Type::String) continue;
                if (!ar.genres.empty()) ar.genres += ", ";
                ar.genres += gv.as_string();
            }
        }
        if (!ar.id.empty()) out.push_back(std::move(ar));
    }
    return true;
}

bool SpotifyWebClient::saved_albums(std::vector<SpotifyAlbum>& out, std::string* error_out) {
    out.clear();
    std::vector<std::string> pages;
    if (!get_all_pages("me/albums", 50, pages, error_out)) return false;

    for (const auto& page : pages) {
        tinyjson::Value v;
        if (!tinyjson::parse(page, v)) continue;
        const auto* items = v.find("items");
        if (!items) continue;
        for (const auto& it : items->arr) {
            // Envelope form: the album sits under "album" alongside
            // "added_at", same shape as /me/tracks uses for tracks.
            const auto* al = it.find("album");
            if (!al) continue;
            SpotifyAlbum ab;
            if (const auto* x = al->find("id")) ab.id = x->as_string();
            if (const auto* x = al->find("uri")) ab.uri = x->as_string();
            if (const auto* x = al->find("name")) ab.name = x->as_string();
            if (const auto* x = al->find("release_date")) ab.release_date = x->as_string();
            if (const auto* x = al->find("total_tracks")) ab.track_count = static_cast<int>(x->as_number(0));
            if (const auto* arts = al->find("artists")) ab.artist = join_artist_names(*arts);
            if (!ab.id.empty()) out.push_back(std::move(ab));
        }
    }
    return true;
}

bool SpotifyWebClient::album_tracks(const SpotifyAlbum& album, std::vector<SpotifyTrack>& out,
                                    std::string* error_out) {
    out.clear();
    if (album.id.empty()) {
        if (error_out) *error_out = "album has no id";
        return false;
    }
    // No market parameter: GET /me no longer returns a "country" field
    // (it needs the user-read-private scope, which mousiki does not ask
    // for -- and asking would widen the consent screen for one optional
    // filter). This endpoint answers 200 without it, so the only cost is
    // that region-unavailable tracks aren't pre-filtered out.
    const std::string base = "albums/" + album.id + "/tracks";

    std::vector<std::string> pages;
    if (!get_all_pages(base, 50, pages, error_out)) return false;

    for (const auto& page : pages) {
        tinyjson::Value v;
        if (!tinyjson::parse(page, v)) continue;
        if (const auto* items = v.find("items")) collect_bare_tracks(*items, out);
    }
    // An album's track objects omit the album they belong to -- it would
    // be redundant in that response, but mousiki's list renders an album
    // column, so fill it in from what the caller already knows.
    for (auto& t : out) {
        if (t.album.empty()) t.album = album.name;
    }
    return true;
}

bool SpotifyWebClient::artist_top_tracks(const SpotifyArtist& artist, std::vector<SpotifyTrack>& out,
                                         std::string* error_out) {
    out.clear();
    if (artist.name.empty()) {
        if (error_out) *error_out = "artist has no name to search for";
        return false;
    }
    // GET /artists/{id}/top-tracks answers 403 now -- it was already on
    // the November 2024 restricted list in spirit and the February 2026
    // revision finished the job. There is no replacement endpoint, so
    // this is a search scoped to the artist instead.
    //
    // Honest about what that costs: these are the catalogue's best
    // string matches for the artist, not Spotify's popularity ranking,
    // and a search can pull in collaborations or same-name artists. It
    // is the closest thing still reachable, and it beats a dead row.
    const std::string q = "artist:\"" + artist.name + "\"";
    if (!search_tracks(q, kSearchLimitMax, out, error_out)) return false;
    return true;
}

} // namespace muisc
