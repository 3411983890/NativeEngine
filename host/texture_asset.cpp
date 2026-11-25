#include "texture_asset.hpp"
#include "../engine/native/assets/file_path.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace host {
namespace {

std::uint16_t readU16(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}
std::uint32_t readU32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool loadOne(const std::filesystem::path& path, TextureAsset& texture, std::string& error) {
    std::ifstream input(engine::assets::nativeFilePath(path), std::ios::binary);
    if (!input.good()) {
        error = "cannot open " + path.filename().u8string();
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 32) {
        error = path.filename().u8string() + ": truncated header";
        return false;
    }
    if (!(bytes[0] == 'N' && bytes[1] == 'G' && bytes[2] == 'T' && bytes[3] == 'X')) {
        error = path.filename().u8string() + ": bad magic";
        return false;
    }
    if (readU16(bytes, 4) != 1) {
        error = path.filename().u8string() + ": unsupported container version";
        return false;
    }
    const unsigned semantic = readU16(bytes, 6);
    const unsigned width = readU16(bytes, 8);
    const unsigned height = readU16(bytes, 10);
    const unsigned mips = bytes[12];
    const unsigned format = bytes[13];
    const unsigned flags = readU16(bytes, 14);
    const std::uint32_t payloadBytes = readU32(bytes, 16);
    const std::uint32_t dataStart = readU32(bytes, 20);
    if (semantic > 11 || !width || !height || !mips || format > 5 || (semantic >= 9 && (flags & 1u))) {
        error = path.filename().u8string() + ": invalid header fields";
        return false;
    }
    // R16 is a linear height source only; treating other semantics as 16-bit
    // would silently reinterpret authored colour data.
    if (format == 5 && semantic != 9) {
        error = path.filename().u8string() + ": R16 requires the height semantic";
        return false;
    }
    if (dataStart != 32 + mips * 8 || dataStart + payloadBytes != bytes.size()) {
        error = path.filename().u8string() + ": payload layout mismatch";
        return false;
    }
    auto mipBytes = [](unsigned mipWidth, unsigned mipHeight, unsigned mipFormat) -> std::uint64_t {
        if (mipFormat == 0) {
            return static_cast<std::uint64_t>(mipWidth) * mipHeight * 4ull;
        }
        if (mipFormat == 5) {
            return static_cast<std::uint64_t>(mipWidth) * mipHeight * 2ull;
        }
        const std::uint64_t blocksX = (mipWidth + 3) / 4;
        const std::uint64_t blocksY = (mipHeight + 3) / 4;
        return blocksX * blocksY * (mipFormat == 1 ? 8ull : 16ull);
    };
    std::uint64_t expected = 0;
    unsigned mipWidth = width;
    unsigned mipHeight = height;
    for (unsigned mip = 0; mip < mips; ++mip) {
        const std::uint32_t offset = readU32(bytes, 32 + mip * 8);
        const std::uint32_t size = readU32(bytes, 32 + mip * 8 + 4);
        const std::uint64_t expectedSize = mipBytes(mipWidth, mipHeight, format);
        if (offset != dataStart + expected || size != expectedSize ||
            offset + size > bytes.size()) {
            error = path.filename().u8string() + ": mip " + std::to_string(mip) + " mismatch";
            return false;
        }
        expected += expectedSize;
        mipWidth = std::max(1u, mipWidth >> 1);
        mipHeight = std::max(1u, mipHeight >> 1);
    }
    if (expected != payloadBytes) {
        error = path.filename().u8string() + ": mip chain size mismatch";
        return false;
    }
    texture.name = path.stem().u8string();
    texture.width = width;
    texture.height = height;
    texture.mips = mips;
    texture.format = format;
    texture.srgb = (flags & 1u) != 0;
    texture.semantic = semantic;
    texture.pixels.assign(bytes.begin() + dataStart, bytes.end());
    return true;
}

} // namespace

bool loadTextureDirectory(const std::filesystem::path& inputDirectory,
                          std::vector<TextureAsset>& textures, std::string& error) {
    const auto directory=engine::assets::nativeFilePath(inputDirectory);
    textures.clear();
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        error = "texture directory not found: " + directory.u8string();
        return false;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".ngtex") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        error = "no .ngtex files in " + directory.u8string();
        return false;
    }
    textures.reserve(files.size());
    for (const auto& file : files) {
        TextureAsset texture;
        if (!loadOne(file, texture, error)) return false;
        textures.push_back(std::move(texture));
    }
    return true;
}

} // namespace host
