#include "local_source.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

#include "console_log.h"

namespace muisc {

bool LocalSource::is_audio_file(const fs::path& p) {
    static const std::set<std::string> exts = {".wav", ".mp3", ".opus", ".flac", ".ogg", ".m4a", ".aac", ".webm"};
    return exts.count(p.extension().string()) > 0;
}

std::vector<LocalTrack> LocalSource::scan(const std::vector<std::string>& custom_paths) const {
    std::vector<LocalTrack> tracks;
    std::vector<fs::path> roots;

    if (!custom_paths.empty()) {
        for (const auto& cp : custom_paths) {
            roots.push_back(fs::path(cp));
        }
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            roots.push_back(fs::path(home) / "Music");
            roots.push_back(fs::path(home) / "disk" / "Music");
        }
    }

    if (roots.empty()) return tracks;

    // Dedupe by resolved (symlink-following) path — if one root is a
    // symlink that overlaps with the other (common on Android, e.g.
    // "disk" pointing into shared storage that also contains "Music"),
    // recursive_directory_iterator would otherwise walk and list the
    // exact same file twice, once per root.
    std::set<std::string> seen_canonical;

    // Roots get the same treatment, for a different reason: on a
    // case-insensitive filesystem (APFS by default) "~/music" and
    // "~/Music" are one directory, and config.txt ships both. Deduping
    // files alone still walked the whole tree twice.
    std::set<std::string> seen_roots;

    // Directories whose contents are an application's private database
    // rather than loose audio. Descending into them is pointless, slow,
    // and — for Music.app's library in particular — the thing macOS
    // refuses access to partway through.
    auto is_opaque_bundle = [](const fs::path& p) {
        const std::string ext = p.extension().string();
        return ext == ".musiclibrary" || ext == ".photoslibrary" ||
               ext == ".tvlibrary"    || ext == ".bundle"        || ext == ".app";
    };

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        {
            std::error_code rec;
            fs::path canon = fs::canonical(root, rec);
            if (!seen_roots.insert(rec ? root.string() : canon.string()).second) continue;
        }

        // Hand-rolled walk instead of a range-for over
        // recursive_directory_iterator.
        //
        // THE CRASH THIS FIXES: a range-for advances with the THROWING
        // operator++. The error_code handed to the constructor only
        // covers construction, so any failure during traversal escaped
        // as a filesystem_error, and nothing caught it -- std::terminate,
        // the whole player gone. Reported from
        // "~/music/Music" (Music.app's library), mid-scan.
        //
        // skip_permission_denied does NOT cover it either: that option
        // only swallows EACCES (permission_denied), while macOS hands
        // back EPERM (operation_not_permitted) for a TCC-protected
        // directory. Different error code, option never applies -- which
        // is why it was already passed here and the crash still happened.
        //
        // So: probe each directory before the iterator tries to descend,
        // and advance with the error_code overload. One unreadable folder
        // now costs that folder, not the session.
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            ConsoleLog::instance().log_verbose("scan: cannot open " + root.string() + " -- " + ec.message());
            continue;
        }
        const fs::recursive_directory_iterator end;

        while (it != end) {
            const fs::path p = it->path();

            std::error_code dec;
            if (it->is_directory(dec) && !dec) {
                if (is_opaque_bundle(p)) {
                    it.disable_recursion_pending();
                } else {
                    // Can this actually be opened? If not, stop the
                    // iterator from trying -- the attempt is what threw.
                    std::error_code oec;
                    fs::directory_iterator probe(p, oec);
                    if (oec) {
                        it.disable_recursion_pending();
                        ConsoleLog::instance().log_verbose("scan: skipping " + p.string() + " -- " + oec.message());
                    }
                }
            } else {
                std::error_code fec;
                if (it->is_regular_file(fec) && !fec && is_audio_file(p)) {
                    std::error_code cec;
                    fs::path canon = fs::canonical(p, cec);
                    std::string key = cec ? p.string() : canon.string();
                    if (seen_canonical.insert(key).second) {
                        std::string folder = p.parent_path().filename().string();
                        if (folder.empty() || fs::path(folder) == root.filename()) folder = "-";
                        tracks.push_back({p.stem().string(), p, folder});
                    }
                }
            }

            it.increment(ec);
            if (ec) {
                // Belt and braces: the probe above should have prevented
                // this, but a traversal error must never be allowed to
                // take the process down. Abandon this root, keep what was
                // already found, carry on with the next one.
                ConsoleLog::instance().log_verbose("scan: stopped walking " + root.string() + " -- " + ec.message());
                break;
            }
        }
    }
    return tracks;
}

std::optional<LocalTrack> LocalSource::find(const std::string& query, const std::vector<std::string>& custom_paths) const {
    std::string needle = query;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    for (const auto& track : scan(custom_paths)) {
        std::string hay = track.title;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (hay.find(needle) != std::string::npos) return track;
    }
    return std::nullopt;
}

} // namespace muisc
