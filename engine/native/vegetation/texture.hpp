#pragma once

#include "asset.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Reader for the cooked NGT3 texture package.
//
// The native side has no PNG decoder on purpose: decoding a PNG in the load path would
// pull a deflate implementation and its error surface into the frame. The cook writes a
// 32-byte header plus a concatenated RGBA8 mip chain, and this reader validates the header
// and every level length before touching a pixel.
//
// NGT2 had no mip chain. A single level is not merely blurrier at distance: the sampler
// minifies the leaf mask into holes, so leaves fall out of the canopy as the camera pulls
// back, and the same texel answers differently every frame. NGT3 stores the chain the cook
// produced, including the coverage-corrected mask levels.

namespace engine::vegetation {

struct TextureLevel {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
};

struct TextureImage {
    std::uint32_t width = 0, height = 0;
    bool srgb = false;
    // levels[0] is the authored resolution; each following level is half the extent, and the
    // chain stops at 1x1. Never empty for a successful parse.
    std::vector<TextureLevel> levels;

    std::uint32_t mipCount() const { return static_cast<std::uint32_t>(levels.size()); }
    bool valid() const { return !levels.empty() && width > 0 && height > 0; }
};

inline TextureImage parseTexture(const std::string& bytes) {
    if (bytes.size() < 32) throw Error("NG-E-VEG-FORMAT", "texture package shorter than its header");
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (std::memcmp(data, "NGT3", 4) != 0) throw Error("NG-E-VEG-FORMAT", "texture magic mismatch");
    const auto read = [data](int offset) {
        return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(data[offset + 2]) << 16) | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    };
    if (read(4) != 2) throw Error("NG-E-VEG-FORMAT", "unsupported texture version");
    TextureImage image;
    image.width = read(8);
    image.height = read(12);
    if (read(16) != 0) throw Error("NG-E-VEG-FORMAT", "unsupported texture format");
    image.srgb = (read(20) & 1u) != 0;
    const auto mipCount = read(24);
    const auto payload = read(28);
    if (image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096) {
        throw Error("NG-E-VEG-FORMAT", "texture dimensions out of range");
    }
    if (mipCount == 0 || mipCount > 12) throw Error("NG-E-VEG-FORMAT", "texture declares an implausible mip count");
    // Lengths are derived from the header, never trusted from it: a corrupted count must fail
    // here rather than let the uploader walk off the end of the buffer.
    std::uint64_t expected = 0;
    std::size_t offset = 32;
    for (std::uint32_t level = 0; level < mipCount; ++level) {
        TextureLevel entry;
        entry.width = image.width >> level; if (entry.width == 0) entry.width = 1;
        entry.height = image.height >> level; if (entry.height == 0) entry.height = 1;
        const std::uint64_t length = static_cast<std::uint64_t>(entry.width) * entry.height * 4;
        expected += length;
        if (offset + length > bytes.size()) throw Error("NG-E-VEG-FORMAT", "texture mip chain runs past its payload");
        entry.pixels.assign(data + offset, data + offset + length);
        offset += static_cast<std::size_t>(length);
        image.levels.push_back(std::move(entry));
    }
    if (payload != expected || bytes.size() != 32ull + expected) {
        throw Error("NG-E-VEG-FORMAT", "texture payload size disagrees with its header");
    }
    return image;
}

inline TextureImage loadTexture(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw Error("NG-E-VEG-FORMAT", "texture cannot be opened: " + file);
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parseTexture(bytes);
}

} // namespace engine::vegetation
