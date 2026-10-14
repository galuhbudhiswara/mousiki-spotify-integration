#include "spotify_auth.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>  // select() -- portable accept() timeout
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>   // struct timeval (SO_RCVTIMEO) -- not transitive on glibc
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>  // std::getenv
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "console_log.h"
#include "http_client.h"
#include "process_util.h"
#include "tiny_json.h"

namespace muisc {

namespace fs = std::filesystem;

static constexpr const char* kAuthorizeUrl = "https://accounts.spotify.com/authorize";
static constexpr const char* kTokenUrl = "https://accounts.spotify.com/api/token";

// One scope per feature, nothing spare:
//   user-read-playback-state   -> now-playing panel
//   user-modify-playback-state -> transport controls
//   user-read-currently-playing-> current track while polling
//   playlist-read-private      -> the user's own playlists
//   playlist-read-collaborative-> ...including collaborative ones
//   user-library-read          -> Liked Songs and saved albums
//   user-top-read              -> top tracks/artists, two Daily Mix sources
//   user-read-recently-played  -> Jump back in, the third Daily Mix source
//
// Scopes are baked into a token when it is minted, so widening this list
// does not upgrade an already-issued token: anyone who signed in earlier
// has to sign in again. Catalogue search needs no scope, and the queue
// endpoints ride on the two playback scopes.
static constexpr const char* kScopes =
    "user-read-playback-state user-modify-playback-state "
    "user-read-currently-playing playlist-read-private "
    "playlist-read-collaborative user-library-read "
    "user-top-read user-read-recently-played";

// ---------------------------------------------------------------------
// SHA-256 + base64url, needed for the PKCE code_challenge.
//
// Inline (standard FIPS 180-4) rather than from OpenSSL: mousiki links
// only libcurl and zlib, and one 32-byte hash is not worth an OpenSSL
// dependency in every packager's build.
// ---------------------------------------------------------------------
namespace {

struct Sha256 {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t buf[64];
    size_t buf_len = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    Sha256() {
        static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h, init, sizeof(init));
    }

    void compress(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = 64 - buf_len;
            if (take > n) take = n;
            std::memcpy(buf + buf_len, p, take);
            buf_len += take; p += take; n -= take;
            if (buf_len == 64) { compress(buf); buf_len = 0; }
        }
    }

    void final(uint8_t out[32]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buf_len != 56) update(&zero, 1);
        uint8_t be[8];
        for (int i = 0; i < 8; ++i) be[i] = uint8_t(bits >> (56 - 8 * i));
        update(be, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4]     = uint8_t(h[i] >> 24);
            out[i * 4 + 1] = uint8_t(h[i] >> 16);
            out[i * 4 + 2] = uint8_t(h[i] >> 8);
            out[i * 4 + 3] = uint8_t(h[i]);
        }
    }
};

std::string base64url(const uint8_t* data, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < n) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < n) v |= uint32_t(data[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        if (i + 1 < n) out += tbl[(v >> 6) & 63];
        if (i + 2 < n) out += tbl[v & 63];
    }
    return out; // no '=' padding -- base64url for PKCE is unpadded
}

// 64-char verifier from the unreserved PKCE alphabet (43..128 allowed).
std::string random_verifier() {
    static const char* alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 65);
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 64; ++i) s += alpha[dist(rd)];
    return s;
}

std::string challenge_for(const std::string& verifier) {
    Sha256 sha;
    sha.update(reinterpret_cast<const uint8_t*>(verifier.data()), verifier.size());
    uint8_t digest[32];
    sha.final(digest);
    return base64url(digest, 32);
}

long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Minimal URL-decode for the callback query string (Spotify sends the
// code percent-encoded).
std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += char(hi * 16 + lo); i += 2; continue; }
        }
        if (s[i] == '+') { out += ' '; continue; }
        out += s[i];
    }
    return out;
}

std::string query_param(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return url_decode(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

// Opens a URL in the user's browser, best-effort. A failure here is not
// fatal -- interactive_login() always prints the URL too, which is the
// only thing that works over SSH anyway.
void open_in_browser(const std::string& url) {
#if defined(MUISC_PLATFORM_MACOS)
    const char* opener = "open";
#elif defined(MUISC_PLATFORM_ANDROID)
    const char* opener = "termux-open-url";
#else
    const char* opener = "xdg-open";
#endif
    run_capture(std::string(opener) + " " + shell_quote(url) + " >/dev/null 2>&1");
}

// One-shot loopback HTTP server. Waits for Spotify to redirect the
// browser to http://127.0.0.1:8350/callback?code=..., answers with a
// human-readable page, and returns the raw query string.
//
// What arrives is only a short-lived authorization `code`, useless
// without the PKCE verifier held in this process.
bool wait_for_callback(int port, int timeout_sec, std::string& out_query, std::string& err) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { err = "socket() failed"; return false; }

    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only, never 0.0.0.0

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "port " + std::to_string(port) + " is already in use";
        ::close(srv);
        return false;
    }
    if (::listen(srv, 1) < 0) {
        err = "listen() failed";
        ::close(srv);
        return false;
    }

    // Gate accept() behind select() rather than SO_RCVTIMEO: a receive
    // timeout on a listening socket is not specified to affect accept(),
    // and on macOS/BSD it does not -- accept() blocks forever, pinning
    // this thread and leaking the socket whenever a user closes the
    // consent tab without approving.
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(srv, &readfds);
    timeval tv{};
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    int ready = ::select(srv + 1, &readfds, nullptr, nullptr, &tv);
    if (ready == 0) {
        err = "timed out waiting for the Spotify redirect";
        ::close(srv);
        return false;
    }
    if (ready < 0) {
        err = "select() failed while waiting for the redirect";
        ::close(srv);
        return false;
    }

    int cli = ::accept(srv, nullptr, nullptr);
    if (cli < 0) {
        err = "failed to accept the Spotify redirect";
        ::close(srv);
        return false;
    }

    // On the accepted socket SO_RCVTIMEO *is* honoured, so a client that
    // connects and then says nothing can't wedge the read loop below.
    timeval rtv{};
    rtv.tv_sec = 10;
    rtv.tv_usec = 0;
    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    // Request line is all we need; it carries the query string.
    std::string req;
    char buf[2048];
    ssize_t n;
    while ((n = ::recv(cli, buf, sizeof(buf), 0)) > 0) {
        req.append(buf, static_cast<size_t>(n));
        if (req.find("\r\n\r\n") != std::string::npos) break;
        if (req.size() > 16384) break; // don't let a rogue client grow this unbounded
    }

    size_t sp1 = req.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    std::string target = (sp1 != std::string::npos && sp2 != std::string::npos)
                             ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "";
    size_t q = target.find('?');
    out_query = (q == std::string::npos) ? "" : target.substr(q + 1);

    const char* body =
        "<!doctype html><meta charset=utf-8>"
        "<title>mousiki</title>"
        "<body style=\"font-family:system-ui;background:#111;color:#eee;"
        "display:grid;place-items:center;height:100vh;margin:0\">"
        "<div style=\"text-align:center\"><h1>&#127925; mousiki</h1>"
        "<p>Spotify connected. You can close this tab.</p></div>";
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                       "Content-Length: " + std::to_string(std::strlen(body)) +
                       "\r\nConnection: close\r\n\r\n" + body;
    ::send(cli, resp.data(), resp.size(), 0);
    ::close(cli);
    ::close(srv);
    return !out_query.empty();
}

} // namespace

// ---------------------------------------------------------------------
// SpotifyTokens / persistence
// ---------------------------------------------------------------------

bool SpotifyTokens::expired() const {
    // 60s skew so we refresh just before the server would start rejecting.
    return expires_at_unix != 0 && now_unix() >= expires_at_unix - 60;
}

std::string SpotifyAuth::redirect_uri() {
    return "http://127.0.0.1:" + std::to_string(kCallbackPort) + "/callback";
}

std::string SpotifyAuth::token_path() {
    const char* home = std::getenv("HOME");
    fs::path dir = fs::path(home ? home : ".") / ".config" / "mousiki";
    return (dir / "spotify_token.json").string();
}

SpotifyTokens SpotifyAuth::load() {
    SpotifyTokens t;
    std::ifstream in(token_path());
    if (!in) return t;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    tinyjson::Value v;
    if (!tinyjson::parse(text, v)) return t;
    if (const auto* a = v.find("access_token")) t.access_token = a->as_string();
    if (const auto* r = v.find("refresh_token")) t.refresh_token = r->as_string();
    if (const auto* e = v.find("expires_at")) t.expires_at_unix = static_cast<long long>(e->as_number());
    return t;
}

bool SpotifyAuth::save(const SpotifyTokens& t) {
    fs::path p(token_path());
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    tinyjson::Value v = tinyjson::Value::make_obj();
    v.set("access_token", tinyjson::Value::make_str(t.access_token));
    v.set("refresh_token", tinyjson::Value::make_str(t.refresh_token));
    v.set("expires_at", tinyjson::Value::make_num(static_cast<double>(t.expires_at_unix)));

    std::ofstream out(p, std::ios::trunc);
    if (!out) return false;
    out << tinyjson::write(v);
    out.close();

    // These are live credentials -- never leave them group/world readable.
    ::chmod(p.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

bool SpotifyAuth::clear() {
    std::error_code ec;
    return fs::remove(fs::path(token_path()), ec);
}

// ---------------------------------------------------------------------
// Token exchange
// ---------------------------------------------------------------------

// Parses an accounts.spotify.com token response into `tokens`, keeping
// the previous refresh_token when the response omits one (Spotify only
// re-issues it sometimes).
static bool apply_token_response(const std::string& body, SpotifyTokens& tokens, std::string* error_out) {
    tinyjson::Value v;
    if (!tinyjson::parse(body, v)) {
        if (error_out) *error_out = "could not parse token response";
        return false;
    }
    const auto* at = v.find("access_token");
    if (!at || at->as_string().empty()) {
        std::string desc;
        if (const auto* e = v.find("error_description")) desc = e->as_string();
        else if (const auto* e2 = v.find("error")) desc = e2->as_string();
        if (error_out) *error_out = desc.empty() ? "token response had no access_token" : desc;
        return false;
    }
    tokens.access_token = at->as_string();
    if (const auto* rt = v.find("refresh_token")) {
        if (!rt->as_string().empty()) tokens.refresh_token = rt->as_string();
    }
    double expires_in = 3600;
    if (const auto* ei = v.find("expires_in")) expires_in = ei->as_number(3600);
    tokens.expires_at_unix = now_unix() + static_cast<long long>(expires_in);
    return true;
}

bool SpotifyAuth::interactive_login(const std::string& client_id, SpotifyTokens& out,
                                    std::string* error_out, int timeout_sec) {
    if (client_id.empty()) {
        if (error_out) *error_out = "SpotifyClientId is not set in config.txt";
        return false;
    }

    const std::string verifier = random_verifier();
    const std::string challenge = challenge_for(verifier);
    const std::string state = random_verifier().substr(0, 16);

    std::string authorize_url = std::string(kAuthorizeUrl) + "?" + http_encode_form({
        {"client_id", client_id},
        {"response_type", "code"},
        {"redirect_uri", redirect_uri()},
        {"code_challenge_method", "S256"},
        {"code_challenge", challenge},
        {"state", state},
        {"scope", kScopes},
    });

    ConsoleLog::instance().log_basic("spotify: open this URL to authorize:");
    ConsoleLog::instance().log_basic(authorize_url);
    open_in_browser(authorize_url);

    std::string query, err;
    if (!wait_for_callback(kCallbackPort, timeout_sec, query, err)) {
        if (error_out) *error_out = err;
        return false;
    }

    if (const std::string e = query_param(query, "error"); !e.empty()) {
        if (error_out) *error_out = "authorization denied: " + e;
        return false;
    }
    // CSRF check: the state we generated must come back untouched.
    if (query_param(query, "state") != state) {
        if (error_out) *error_out = "state mismatch on callback (possible CSRF) -- login aborted";
        return false;
    }
    const std::string code = query_param(query, "code");
    if (code.empty()) {
        if (error_out) *error_out = "callback carried no authorization code";
        return false;
    }

    const std::string form = http_encode_form({
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", redirect_uri()},
        {"client_id", client_id},
        {"code_verifier", verifier},
    });

    HttpResponse resp = http_request(
        "POST", kTokenUrl,
        {{"Content-Type", "application/x-www-form-urlencoded"}}, form);

    if (!resp.transport_ok) {
        if (error_out) *error_out = "token request failed: " + resp.err;
        return false;
    }
    if (!apply_token_response(resp.body, out, error_out)) return false;

    save(out);
    return true;
}

bool SpotifyAuth::refresh(const std::string& client_id, SpotifyTokens& tokens, std::string* error_out) {
    if (tokens.refresh_token.empty()) {
        if (error_out) *error_out = "no refresh token stored -- sign in again";
        return false;
    }
    if (client_id.empty()) {
        if (error_out) *error_out = "SpotifyClientId is not set in config.txt";
        return false;
    }

    const std::string form = http_encode_form({
        {"grant_type", "refresh_token"},
        {"refresh_token", tokens.refresh_token},
        {"client_id", client_id},
    });

    HttpResponse resp = http_request(
        "POST", kTokenUrl,
        {{"Content-Type", "application/x-www-form-urlencoded"}}, form);

    if (!resp.transport_ok) {
        if (error_out) *error_out = "refresh request failed: " + resp.err;
        return false;
    }
    if (!apply_token_response(resp.body, tokens, error_out)) return false;

    save(tokens);
    return true;
}

} // namespace muisc
