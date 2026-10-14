#include "album_art.h"

#include "http_client.h"
#include "process_util.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace muisc {

namespace {

// This key becomes a filename and arrives from the network, so it gets a
// stricter scrub than CacheManager::sanitize() gives song titles:
// allow-list rather than deny-list.
std::string safe_key(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (keep) out += static_cast<char>(c);
    }
    if (out.empty()) out = "art";
    if (out.size() > 64) out.resize(64);
    return out;
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const fs::path& p, const std::string& data) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    return o.good();
}

} // namespace

bool fetch_album_art(const std::string& url,
                     const std::string& cache_key,
                     const fs::path& cache_dir,
                     int size,
                     AlbumArtImage& out,
                     std::string* error_out) {
    auto fail = [&](const std::string& why) {
        if (error_out) *error_out = why;
        return false;
    };

    if (url.empty()) return fail("this track has no cover art");
    if (size <= 0) return fail("bad cover art target size");

    const size_t expected = static_cast<size_t>(size) * static_cast<size_t>(size);

    std::error_code ec;
    const fs::path dir = cache_dir / "art";
    fs::create_directories(dir, ec);

    const std::string stem = safe_key(cache_key.empty() ? url : cache_key);
    const fs::path gray_path = dir / (stem + "_" + std::to_string(size) + ".gray");

    // Cache hit. The size check doubles as the validity check -- a run
    // killed mid-decode leaves a short file, and a short file is the one
    // failure mode that would otherwise be cached forever.
    std::string cached;
    if (read_file(gray_path, cached) && cached.size() == expected) {
        out.size = size;
        out.gray.assign(cached.begin(), cached.end());
        return true;
    }

    // i.scdn.co serves covers unauthenticated, so this is a plain GET --
    // no token, and deliberately no Authorization header (sending one to
    // a CDN that doesn't want it is how tokens leak into access logs).
    HttpResponse res = http_request("GET", url);
    if (!res.transport_ok) return fail("cover art download failed: " + res.err);
    if (res.status != 200) return fail("cover art download failed: HTTP " + std::to_string(res.status));
    if (res.body.empty()) return fail("cover art came back empty");

    const fs::path src_path = dir / (stem + ".img");
    if (!write_file(src_path, res.body)) return fail("could not write " + src_path.string());

    // flags=area is a box filter, which is what a 300->60 reduction
    // wants: bilinear at that ratio reads roughly every fifth source
    // pixel and aliases the rest of the cover into noise, which then
    // dithers into static. Averaging the whole footprint doesn't.
    const std::string scale = "scale=" + std::to_string(size) + ":" + std::to_string(size) + ":flags=area";
    const std::string cmd =
        "ffmpeg -nostdin -v error -y -i " + shell_quote(src_path.string()) +
        " -an -frames:v 1 -vf " + shell_quote(scale) +
        " -pix_fmt gray -f rawvideo " + shell_quote(gray_path.string());

    ProcResult r = run_capture(cmd, true);
    fs::remove(src_path, ec);

    std::string raw;
    if (!r.ok() || !read_file(gray_path, raw) || raw.size() != expected) {
        fs::remove(gray_path, ec); // never leave a truncated frame behind to be "cached"
        return fail("ffmpeg could not decode the cover art");
    }

    out.size = size;
    out.gray.assign(raw.begin(), raw.end());
    return true;
}

} // namespace muisc
