#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "album_art.h"

namespace muisc {

// One Braille cell's colour. A cell is the smallest thing a terminal can
// colour -- it holds 2x4 dots but only one foreground colour -- so this
// is the real resolution of the disk's colour, a quarter of its shape
// resolution in each axis.
struct DiskRgb {
    unsigned char r = 0, g = 0, b = 0;
    bool operator==(const DiskRgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const DiskRgb& o) const { return !(*this == o); }
};

// Renders the rotating Braille-dot "disk" seen in the mockup. This is a
// direct port of the rotation algorithm supplied in beta-ui.txt (decode
// the artwork into individual dots once, then rotate the dot cloud around
// its center each frame and re-encode to Braille) — logic unchanged, just
// wrapped in a reusable class instead of a standalone main().
//
// set_album_art() swaps the built-in vinyl for the cover of whatever is
// playing. Everything else about the panel — size, rotation, colouring —
// is identical either way, so nothing downstream has to know which one
// is on screen.
class DiskArt {
public:
    DiskArt();

    // One rotated frame at `angle_radians`, as printable lines (no trailing
    // newlines). Always `height()` lines of `width()` Braille cells each.
    //
    // Pass `out_cell_colors` to also get the cover's own colour per cell,
    // sampled from the same rotated position the dots came from so the
    // colour turns with the art. Filled to height() rows of width()
    // entries, or cleared when the built-in vinyl is showing -- line art
    // has no colour of its own, and the caller should keep using the
    // configured gradient there.
    std::vector<std::string> frame(double angle_radians,
                                   std::vector<std::vector<DiskRgb>>* out_cell_colors = nullptr) const;

    // Replaces the vinyl with `img`, dithered down to the 1-bit grid
    // Braille can actually show and masked into the same circular
    // silhouette (spindle hole included) so the panel still reads as a
    // record rather than a square photo. Dimensions never change, so no
    // caller's layout moves. An invalid image clears back to the vinyl
    // instead of leaving a half-built disk.
    void set_album_art(const AlbumArtImage& img);

    // Back to the built-in vinyl.
    void clear_album_art();

    bool showing_album_art() const { return !art_.empty(); }

    int width() const { return width_; }
    int height() const { return height_; }

    // The square edge, in dots, that set_album_art() wants its input at.
    // Braille cells are 2 dots wide and 4 tall, so this is the largest
    // square that fits the panel.
    int art_resolution() const { return std::min(width_ * 2, height_ * 4); }

private:
    struct Dot { double x; double y; };

    std::vector<std::string> frame_dots(double angle) const;
    std::vector<std::string> frame_art(double angle, std::vector<std::vector<DiskRgb>>* colors) const;

    std::vector<Dot> dots_;         // the built-in vinyl, as a sparse dot cloud
    std::vector<uint8_t> art_;      // (width_*2) x (height_*4), 1 = dot; empty = show the vinyl
    std::vector<uint8_t> art_rgb_;  // same grid, 3 bytes per dot position; parallel to art_
    int width_ = 0;
    int height_ = 0;
};

} // namespace muisc
