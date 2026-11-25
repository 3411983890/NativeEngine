#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace host {

// One cooked .ngtex file: an RGBA8/BC1/BC3/BC5/BC7/R16 mip chain, already
// validated by tools/textures.mjs. R16 (code 5) is linear single-channel
// 16-bit UNORM and is only valid for the height semantic.
struct TextureAsset {
    // File stem, e.g. "brick_red.albedo". Material references resolve against this name.
    std::string name;
    unsigned width = 0;
    unsigned height = 0;
    unsigned mips = 0;
    // NGTX format code: 0 RGBA8, 1 BC1, 2 BC3, 3 BC5, 4 BC7, 5 R16.
    unsigned format = 0;
    bool srgb = false;
    // Semantic codes 9–11 are linear height, roughness and metallic data.
    unsigned semantic = 0;
    std::vector<unsigned char> pixels;
};

// Loads every *.ngtex in the directory. The order is sorted by file name so two runs of
// the same asset directory produce the same slice indices.
bool loadTextureDirectory(const std::filesystem::path& directory,
                          std::vector<TextureAsset>& textures, std::string& error);

} // namespace host
