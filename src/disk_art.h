#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "album_art.h"

namespace muisc {

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
    std::vector<std::string> frame(double angle_radians) const;

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
    std::vector<std::string> frame_art(double angle) const;

    std::vector<Dot> dots_;      // the built-in vinyl, as a sparse dot cloud
    std::vector<uint8_t> art_;   // (width_*2) x (height_*4), 1 = dot; empty = show the vinyl
    int width_ = 0;
    int height_ = 0;
};

} // namespace muisc
