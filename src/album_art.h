#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// A decoded album cover: square, 8-bit grayscale, already at the
// resolution the disk wants. Plain bytes rather than anything
// image-library-shaped, because the only consumer is DiskArt's ditherer.
struct AlbumArtImage {
    int size = 0;                // width == height, in pixels
    std::vector<uint8_t> gray;   // size*size, row-major, 0 = black, 255 = white

    bool valid() const {
        return size > 0 && gray.size() == static_cast<size_t>(size) * static_cast<size_t>(size);
    }
};

// Downloads `url` and decodes it to a `size`x`size` grayscale image.
//
// Decoding goes through ffmpeg rather than a bundled image library:
// ffmpeg is already a hard runtime dependency (it decodes every track
// the miniaudio path can't, see waveform.cpp), and one invocation does
// the JPEG decode, the box-filtered downscale and the colour->grey
// conversion together. Vendoring stb_image to do the same would add
// thousands of lines for a capability every user already has installed.
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
