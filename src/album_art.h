#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// A decoded album cover: square, already at the resolution the disk
// wants. Plain bytes rather than anything image-library-shaped, because
// the only consumers are DiskArt's ditherer and its colour sampler.
//
// Both planes come out of one decode. `gray` is derived from `rgb` in
// this process rather than asked of ffmpeg separately -- a second
// invocation would cost another subprocess for a value that is three
// multiplies away, and deriving it guarantees the two planes agree
// pixel for pixel, which matters because the dither decides WHERE dots
// go and the colour sampler decides what colour they are.
struct AlbumArtImage {
    int size = 0;                // width == height, in pixels
    std::vector<uint8_t> gray;   // size*size, row-major, 0 = black, 255 = white
    std::vector<uint8_t> rgb;    // size*size*3, row-major, r,g,b interleaved

    bool valid() const {
        const size_t px = static_cast<size_t>(size) * static_cast<size_t>(size);
        return size > 0 && gray.size() == px && rgb.size() == px * 3;
    }
};

// Downloads `url` and decodes it to a `size`x`size` image, grayscale
// and RGB planes both.
//
// Decoding goes through ffmpeg rather than a bundled image library:
// ffmpeg is already a hard runtime dependency (it decodes every track
// the miniaudio path can't, see waveform.cpp), and one invocation does
// the JPEG decode and the box-filtered downscale together. Vendoring
// stb_image to do the same would add thousands of lines for a
// capability every user already has installed.
//
// BLOCKING -- an HTTP round-trip plus a subprocess. Call it off the
// render thread.
//
// Decoded frames are cached under `cache_dir`/art, keyed by `cache_key`
// (the Spotify track id), so replaying a track costs no network at all.
bool fetch_album_art(const std::string& url,
                     const std::string& cache_key,
                     const fs::path& cache_dir,
                     int size,
                     AlbumArtImage& out,
                     std::string* error_out = nullptr);

} // namespace muisc
