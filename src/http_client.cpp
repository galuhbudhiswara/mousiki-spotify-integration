#include "http_client.h"
#include <curl/curl.h>

namespace muisc {

void http_client_global_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_client_global_cleanup() {
    curl_global_cleanup();
}

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string url_encode(CURL* curl, const std::string& s) {
    char* escaped = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    std::string result = escaped ? escaped : "";
    if (escaped) curl_free(escaped);
    return result;
}

bool http_get(const std::string& base_url,
              const std::vector<std::pair<std::string, std::string>>& params,
              std::string& out_body, std::string& err, long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return false;
    }

    std::string url = base_url;
    if (!params.empty()) {
        url += "?";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) url += "&";
            url += url_encode(curl, params[i].first) + "=" + url_encode(curl, params[i].second);
        }
    }

    out_body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&out_body));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mousiki/1.0 (+https://github.com/)");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // accept whatever the server offers (gzip/deflate/br), decoded automatically
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // safe to call from a background thread (no SIGALRM-based timeout)

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        err = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
        err = "HTTP " + std::to_string(http_code);
        return false;
    }
    return true;
}

std::string http_url_encode(const std::string& s) {
    // curl_easy_escape needs a handle, but it never touches its state for
    // escaping -- a throwaway handle keeps this a free function so callers
    // don't have to own one just to build a query string.
    CURL* curl = curl_easy_init();
    if (!curl) return s;
    std::string out = url_encode(curl, s);
    curl_easy_cleanup(curl);
    return out;
}

std::string http_encode_form(const std::vector<std::pair<std::string, std::string>>& params) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out += "&";
        out += url_encode(curl, params[i].first) + "=" + url_encode(curl, params[i].second);
    }
    curl_easy_cleanup(curl);
    return out;
}

HttpResponse http_request(const std::string& method,
                          const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& body,
                          long timeout_sec) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.err = "curl_easy_init failed";
        return resp;
    }

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) {
        hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&resp.body));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mousiki/1.0 (+https://github.com/)");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // same background-thread safety rationale as http_get
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        // PUT/DELETE: CUSTOMREQUEST + POSTFIELDS rather than CURLOPT_UPLOAD,
        // so we can send a small in-memory body without a read callback.
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.err = curl_easy_strerror(res);
    } else {
        resp.transport_ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

} // namespace muisc
