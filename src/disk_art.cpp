#include "disk_art.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace muisc {

namespace {

constexpr double PI = 3.14159265358979323846;

// Braille dot layout:
//   1 4
//   2 5
//   3 6
//   7 8
// Unicode Braille: U+2800 + dot bitmask

uint32_t decodeUTF8(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0) {
        uint32_t r = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        i += 2; return r;
    }
    if ((c & 0xF0) == 0xE0) {
        uint32_t r = ((c & 0x0F) << 12) |
                     ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                     (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        i += 3; return r;
    }
    uint32_t r = ((c & 0x07) << 18) |
                 ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    i += 4; return r;
}

bool brailleDots(uint32_t cp, bool dots[4][2]) {
    if (cp < 0x2800 || cp > 0x28FF) return false;
    uint8_t bits = static_cast<uint8_t>(cp - 0x2800);
    dots[0][0] = bits & (1 << 0);
    dots[1][0] = bits & (1 << 1);
    dots[2][0] = bits & (1 << 2);
    dots[0][1] = bits & (1 << 3);
    dots[1][1] = bits & (1 << 4);
    dots[2][1] = bits & (1 << 5);
    dots[3][0] = bits & (1 << 6);
    dots[3][1] = bits & (1 << 7);
    return true;
}

std::string encodeBraille(bool dots[4][2]) {
    uint8_t bits = 0;
    if (dots[0][0]) bits |= 1 << 0;
    if (dots[1][0]) bits |= 1 << 1;
    if (dots[2][0]) bits |= 1 << 2;
    if (dots[0][1]) bits |= 1 << 3;
    if (dots[1][1]) bits |= 1 << 4;
    if (dots[2][1]) bits |= 1 << 5;
    if (dots[3][0]) bits |= 1 << 6;
    if (dots[3][1]) bits |= 1 << 7;

    uint32_t cp = 0x2800 + bits;
    std::string out;
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
    return out;
}

// The circular artwork from beta-ui.txt's disk rotation demo.
const char* kSourceArt =
"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣠⣤⣤⣤⣤⣄⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⣠⣴⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⠃⠀⠀⢀⠀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⢀⣴⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡏⠀⠀⣠⣿⣿⣦⡀⠀⠀⠀⠀\n"
"⠀⠀⠀⣠⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠁⠀⣴⣿⣿⣿⣿⣿⣄⠀⠀⠀\n"
"⠀⠀⣰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠿⢿⠃⢀⣾⣿⣿⣿⣿⣿⣿⣿⣆⠀⠀\n"
"⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⡿⠋⣡⣤⣶⣶⣤⣄⠘⢿⣿⣿⣿⣿⣿⣿⣿⣿⡆⠀\n"
"⠀⣾⣿⣿⣿⣿⣿⣿⣿⡿⠀⣾⣿⠟⠉⠉⠻⣿⣷⠀⢿⣿⣿⣿⣿⣿⣿⣿⣷⠀\n"
"⠀⣿⣿⣿⣿⣿⣿⣿⣿⡇⢸⣿⡇⠀⠀⠀⠀⢸⣿⡇⢸⣿⣿⣿⣿⣿⣿⣿⣿⠀\n"
"⠀⠿⠿⠟⠛⠛⢉⣉⣡⡤⠀⢿⣿⣤⣀⣀⣤⣿⡿⠀⣾⣿⣿⣿⣿⣿⣿⣿⡿⠀\n"
"⠀⠀⣤⣴⣶⡿⠟⢋⣡⣶⣶⣄⠙⠛⠿⠿⠛⠋⣠⣾⣿⣿⣿⣿⣿⣿⣿⣿⠇⠀\n"
"⠀⠀⠙⠋⢁⣠⣶⣿⣿⣿⣿⣿⣿⣷⣶⣶⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠏⠀⠀\n"
"⠀⠀⠀⠰⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠏⠀⠀⠀\n"
"⠀⠀⠀⠀⠈⠻⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠟⠁⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠈⠙⠻⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⠋⠁⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠉⠙⛛⛛⛛⛛⛛⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀";

std::vector<std::string> split_lines(const std::string& source) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) { lines.push_back(source.substr(start)); break; }
        lines.push_back(source.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// Pulls a sampled cover colour into a band a terminal can actually show
// on a dark background: saturation and lightness both get floors, the
// hue is left alone.
//
// The s < 0.12 escape hatch is load-bearing. A white t-shirt or a grey
// slab carries a meaningless residual hue, and dragging that up to the
// floor invents a colour the sleeve does not contain -- DAMN.'s white
// shirt came out lavender before this was here.
DiskRgb boost_color(double r, double g, double b) {
    r /= 255.0; g /= 255.0; b /= 255.0;

    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    double h = 0.0;
    const double l = (mx + mn) / 2.0;
    double sat = 0.0;
    if (mx > mn) {
        const double d = mx - mn;
        sat = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == r)      h = (g - b) / d + (g < b ? 6.0 : 0.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else              h = (r - g) / d + 4.0;
        h /= 6.0;
    }

    constexpr double kSatMul = 1.6, kSatFloor = 0.35, kSatCeil = 1.0;
    constexpr double kLightFloor = 0.42, kLightCeil = 0.80;
    sat = sat < 0.12 ? sat * 0.6 : std::clamp(sat * kSatMul, kSatFloor, kSatCeil);
    const double lo = std::clamp(l, kLightFloor, kLightCeil);

    auto hue2rgb = [](double p, double q, double t) {
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
        if (t < 1.0 / 2.0) return q;
        if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        return p;
    };
    double orr = lo, og = lo, ob = lo;
    if (sat > 0.0) {
        const double q = lo < 0.5 ? lo * (1.0 + sat) : lo + sat - lo * sat;
        const double p = 2.0 * lo - q;
        orr = hue2rgb(p, q, h + 1.0 / 3.0);
        og  = hue2rgb(p, q, h);
        ob  = hue2rgb(p, q, h - 1.0 / 3.0);
    }
    auto byte = [](double v) {
        return static_cast<unsigned char>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
    };
    return DiskRgb{byte(orr), byte(og), byte(ob)};
}

} // namespace

DiskArt::DiskArt() {
    std::vector<std::string> lines = split_lines(kSourceArt);

    height_ = static_cast<int>(lines.size());
    width_ = 0;
    for (const auto& line : lines) {
        int cells = 0;
        for (size_t i = 0; i < line.size();) { decodeUTF8(line, i); ++cells; }
        width_ = std::max(width_, cells);
    }

    for (int row = 0; row < height_; ++row) {
        int col = 0;
        for (size_t i = 0; i < lines[row].size();) {
            uint32_t cp = decodeUTF8(lines[row], i);
            bool b[4][2]{};
            if (brailleDots(cp, b)) {
                for (int dy = 0; dy < 4; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        if (!b[dy][dx]) continue;
                        dots_.push_back({col * 2.0 + dx, row * 4.0 + dy});
                    }
                }
            }
            ++col;
        }
    }
}

std::vector<std::string> DiskArt::frame_dots(double angle) const {
    const int pixelWidth = width_ * 2;
    const int pixelHeight = height_ * 4;

    std::vector<std::vector<bool>> bitmap(pixelHeight, std::vector<bool>(pixelWidth, false));

    const double cx = (pixelWidth - 1) / 2.0;
    const double cy = (pixelHeight - 1) / 2.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);

    for (const auto& p : dots_) {
        double x = p.x - cx;
        double y = p.y - cy;
        // Clockwise in terminal coords (Y points down).
        double rx = x * c - y * s;
        double ry = x * s + y * c;
        int nx = static_cast<int>(std::round(rx + cx));
        int ny = static_cast<int>(std::round(ry + cy));
        if (nx >= 0 && nx < pixelWidth && ny >= 0 && ny < pixelHeight) {
            bitmap[ny][nx] = true;
        }
    }

    std::vector<std::string> result;
    result.reserve(height_);
    for (int row = 0; row < height_; ++row) {
        std::string line;
        for (int col = 0; col < width_; ++col) {
            bool dots[4][2]{};
            for (int dy = 0; dy < 4; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int x = col * 2 + dx;
                    int y = row * 4 + dy;
                    if (x < pixelWidth && y < pixelHeight) dots[dy][dx] = bitmap[y][x];
                }
            }
            line += encodeBraille(dots);
        }
        result.push_back(std::move(line));
    }
    return result;
}

std::vector<std::string> DiskArt::frame(double angle,
                                        std::vector<std::vector<DiskRgb>>* out_cell_colors) const {
    if (art_.empty()) {
        if (out_cell_colors) out_cell_colors->clear();
        return frame_dots(angle);
    }
    return frame_art(angle, out_cell_colors);
}

void DiskArt::clear_album_art() {
    art_.clear();
    art_rgb_.clear();
}

void DiskArt::set_album_art(const AlbumArtImage& img) {
    if (!img.valid()) { clear_album_art(); return; }

    const int pw = width_ * 2;
    const int ph = height_ * 4;
    if (pw <= 0 || ph <= 0) { clear_album_art(); return; }

    const double cx = (pw - 1) / 2.0;
    const double cy = (ph - 1) / 2.0;
    const double radius = std::min(pw, ph) / 2.0 - 0.5;
    // The spindle hole, sized as a fraction of the disk so it survives
    // any future resize. It earns its place: without it a dark cover
    // renders as one featureless blob and stops reading as a record at
    // all — the hole is the feature that says "this thing is spinning".
    const double hole = radius * 0.17;
    const double r2 = radius * radius;
    const double h2 = hole * hole;

    auto masked = [&](int x, int y) {
        const double dx = x - cx, dy = y - cy;
        const double d2 = dx * dx + dy * dy;
        return d2 > r2 || d2 < h2;
    };

    // --- resample the cover onto the dot grid ------------------------
    // Nearest-neighbour is enough here: album_art.cpp already asked
    // ffmpeg for exactly art_resolution() pixels with a box filter, so
    // in practice this is a 1:1 copy. The general form only matters if
    // a cached frame from an older build comes back at another size.
    std::vector<double> lum(static_cast<size_t>(pw) * static_cast<size_t>(ph), 0.0);
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const int sx = std::clamp(static_cast<int>((x + 0.5) / pw * img.size), 0, img.size - 1);
            const int sy = std::clamp(static_cast<int>((y + 0.5) / ph * img.size), 0, img.size - 1);
            lum[static_cast<size_t>(y) * pw + x] =
                static_cast<double>(img.gray[static_cast<size_t>(sy) * img.size + sx]);
        }
    }

    // --- auto-levels, over the visible disk only ---------------------
    // Album covers are mastered for a backlit screen, not for a 1-bit
    // grid. A dark sleeve lands almost entirely below any fixed
    // threshold and dithers to nothing; a blown-out one dithers to a
    // solid blob. Stretching the 2nd-98th percentile of what is
    // actually inside the circle is what keeps both legible — and the
    // percentiles, rather than plain min/max, are what stop one
    // specular highlight or one black corner from eating the range.
    int hist[256] = {0};
    int inside = 0;
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            if (masked(x, y)) continue;
            ++hist[static_cast<int>(lum[static_cast<size_t>(y) * pw + x])];
            ++inside;
        }
    }
    double lo = 0.0, hi = 255.0;
    if (inside > 0) {
        const int lo_target = std::max(1, inside * 2 / 100);
        const int hi_target = std::max(1, inside - inside * 2 / 100);
        int acc = 0;
        for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc >= lo_target) { lo = i; break; } }
        acc = 0;
        for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc >= hi_target) { hi = i; break; } }
    }
    // A near-flat cover (a solid-colour sleeve) has no range to stretch;
    // amplifying its noise by 30x would turn it into static.
    if (hi - lo < 8.0) { lo = 0.0; hi = 255.0; }
    for (double& v : lum) v = std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * 255.0;

    // --- the colour plane, resampled the same way --------------------
    // Kept raw, not boosted: boosting here then averaging 8 of them per
    // cell is not the same as averaging first and boosting once, and the
    // second is the one that keeps a cell's colour faithful to its patch
    // of the sleeve.
    art_rgb_.assign(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 3, 0);
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const int sx = std::clamp(static_cast<int>((x + 0.5) / pw * img.size), 0, img.size - 1);
            const int sy = std::clamp(static_cast<int>((y + 0.5) / ph * img.size), 0, img.size - 1);
            const size_t src = (static_cast<size_t>(sy) * img.size + sx) * 3;
            const size_t dst = (static_cast<size_t>(y) * pw + x) * 3;
            art_rgb_[dst]     = img.rgb[src];
            art_rgb_[dst + 1] = img.rgb[src + 1];
            art_rgb_[dst + 2] = img.rgb[src + 2];
        }
    }

    // --- Floyd-Steinberg, masked to the disk -------------------------
    art_.assign(static_cast<size_t>(pw) * static_cast<size_t>(ph), 0);
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const size_t i = static_cast<size_t>(y) * pw + x;
            if (masked(x, y)) continue;  // art_ is already 0 here; dropped error is fine
            const double old_v = lum[i];
            const double new_v = old_v >= 128.0 ? 255.0 : 0.0;
            art_[i] = new_v > 0.0 ? 1 : 0;
            const double err = old_v - new_v;
            auto spill = [&](int ox, int oy, double w) {
                const int nx = x + ox, ny = y + oy;
                if (nx < 0 || nx >= pw || ny < 0 || ny >= ph) return;
                lum[static_cast<size_t>(ny) * pw + nx] += err * w;
            };
            spill(1, 0, 7.0 / 16.0);
            spill(-1, 1, 3.0 / 16.0);
            spill(0, 1, 5.0 / 16.0);
            spill(1, 1, 1.0 / 16.0);
        }
    }
}

std::vector<std::string> DiskArt::frame_art(double angle,
                                            std::vector<std::vector<DiskRgb>>* colors) const {
    const int pw = width_ * 2;
    const int ph = height_ * 4;

    // Inverse mapping, unlike frame_dots(): that one pushes each source
    // dot to a rounded destination, which is fine for sparse line art
    // but tears holes through a ~50%-dense dithered cover — two source
    // dots round onto the same destination and the cell they vacated
    // stays empty, so the disk visibly sparkles as it turns. Pulling
    // instead (one source lookup per destination dot) can neither
    // collide nor leave a gap.
    const double cx = (pw - 1) / 2.0;
    const double cy = (ph - 1) / 2.0;
    const double c = std::cos(-angle);
    const double s = std::sin(-angle);

    if (colors) {
        colors->assign(height_, std::vector<DiskRgb>(width_, DiskRgb{}));
    }

    std::vector<std::string> result;
    result.reserve(height_);
    for (int row = 0; row < height_; ++row) {
        std::string line;
        for (int col = 0; col < width_; ++col) {
            bool dots[4][2]{};
            // Averaged over all 8 dot positions, not just the lit ones: a
            // cell with two dots on still sits on a patch of sleeve, and
            // colouring it from that whole patch is what keeps a sparse,
            // dark region the colour it is instead of letting one stray
            // bright dot decide for it.
            double acc_r = 0.0, acc_g = 0.0, acc_b = 0.0;
            int sampled = 0;
            for (int dy = 0; dy < 4; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = col * 2 + dx;
                    const int y = row * 4 + dy;
                    if (x >= pw || y >= ph) continue;
                    const double ox = x - cx, oy = y - cy;
                    const int sx = static_cast<int>(std::lround(ox * c - oy * s + cx));
                    const int sy = static_cast<int>(std::lround(ox * s + oy * c + cy));
                    if (sx < 0 || sx >= pw || sy < 0 || sy >= ph) continue;
                    const size_t i = static_cast<size_t>(sy) * pw + sx;
                    dots[dy][dx] = art_[i] != 0;
                    if (colors) {
                        acc_r += art_rgb_[i * 3];
                        acc_g += art_rgb_[i * 3 + 1];
                        acc_b += art_rgb_[i * 3 + 2];
                        ++sampled;
                    }
                }
            }
            if (colors && sampled > 0) {
                (*colors)[row][col] = boost_color(acc_r / sampled, acc_g / sampled, acc_b / sampled);
            }
            line += encodeBraille(dots);
        }
        result.push_back(std::move(line));
    }
    return result;
}

} // namespace muisc
