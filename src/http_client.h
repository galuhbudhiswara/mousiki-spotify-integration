#pragma once
#include <string>
#include <utility>
#include <vector>

namespace muisc {

// Minimal libcurl-backed HTTP GET -- deliberately narrow (GET + query
// params only, no POST/custom headers/auth) since that's all mousiki's
// two lyrics APIs (Better Lyrics, LRCLIB) actually need. Not a general
// HTTP client; this is what replaced Python's `requests` for those two
// calls specifically.
//
// `params` are URL-encoded (via curl_easy_escape) and appended as a
// query string onto `base_url`. Returns true with `out_body` populated
// on HTTP 200; false (with `err` set to a short human-readable reason)
// on anything else -- network failure, timeout, non-200 status.
bool http_get(const std::string& base_url,
              const std::vector<std::pair<std::string, std::string>>& params,
              std::string& out_body, std::string& err, long timeout_sec = 10);

// Called once at startup (see main.cpp) / shutdown -- libcurl technically
// self-initializes lazily on first use, but that lazy init is documented
// as not thread-safe, and mousiki does make its first HTTP call from a
// background thread (lyrics fetching runs off the main thread). Doing
// the global init explicitly, early, on the main thread before any
// worker thread could possibly race it, sidesteps that entirely.
void http_client_global_init();
void http_client_global_cleanup();

// ---------------------------------------------------------------------
// General request. Three things http_get() doesn't do, all of which the
// Spotify Web API needs: request headers, verbs other than GET, and the
// status code itself -- a 401 there means "refresh the token and retry",
// which success/failure alone can't express.
struct HttpResponse {
    long status = 0;          // HTTP status code; 0 if the request never completed
    std::string body;
    bool transport_ok = false; // false = never reached the server (DNS/TLS/timeout)
    std::string err;           // transport error description when transport_ok is false
};

// `method` is "GET"/"POST"/"PUT"/"DELETE". `headers` entries are sent
// verbatim as "Name: Value". `body` is sent as-is for POST/PUT (set the
// matching Content-Type in `headers`); pass an empty string for none.
// Unlike http_get(), a non-2xx response is NOT an error here -- it comes
// back with transport_ok=true and the real status, because callers need
// to branch on it.
HttpResponse http_request(const std::string& method,
                          const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers = {},
                          const std::string& body = "",
                          long timeout_sec = 15);

// Percent-encodes `s` for use in a query string or an
// application/x-www-form-urlencoded body.
std::string http_url_encode(const std::string& s);

// Builds "k=v&k=v" with both sides percent-encoded. Used for both query
// strings and form bodies (the OAuth token endpoint wants the latter).
std::string http_encode_form(const std::vector<std::pair<std::string, std::string>>& params);

} // namespace muisc
