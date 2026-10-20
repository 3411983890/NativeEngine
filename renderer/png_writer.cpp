#include "png_writer.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace nativegpu {
namespace {

std::uint32_t crc32(const unsigned char* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFu) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            table[index] = value;
        }
        ready = true;
    }
    for (std::size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

void appendBigEndian32(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>(value & 0xFF));
}

void appendChunk(std::vector<unsigned char>& out, const char type[4],
                 const std::vector<unsigned char>& payload) {
    appendBigEndian32(out, static_cast<std::uint32_t>(payload.size()));
    std::vector<unsigned char> crcInput;
    crcInput.reserve(4 + payload.size());
    for (int index = 0; index < 4; ++index) {
        crcInput.push_back(static_cast<unsigned char>(type[index]));
    }
    crcInput.insert(crcInput.end(), payload.begin(), payload.end());
    out.insert(out.end(), crcInput.begin(), crcInput.end());
    const std::uint32_t checksum = crc32(crcInput.data(), crcInput.size()) ^ 0xFFFFFFFFu;
    appendBigEndian32(out, checksum);
}

std::uint32_t adler32(const std::vector<unsigned char>& data) {
    constexpr std::uint32_t base = 65521u;
    std::uint32_t a = 1, b = 0;
    for (unsigned char byte : data) {
        a = (a + byte) % base;
        b = (b + a) % base;
    }
    return (b << 16) | a;
}

} // namespace

bool writePng(const std::string& path, unsigned width, unsigned height,
              const unsigned char* rgba, std::size_t rowPitch, std::string& error) {
    if (!rgba || width == 0 || height == 0) {
        error = "Screenshot buffer is empty";
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    if (rowPitch < rowBytes) {
        error = "Screenshot row pitch is smaller than the pixel row";
        return false;
    }

    // Raw scanlines with filter byte 0 in front of every row.
    std::vector<unsigned char> raw;
    raw.reserve((rowBytes + 1u) * height);
    for (unsigned row = 0; row < height; ++row) {
        raw.push_back(0);
        const unsigned char* source = rgba + static_cast<std::size_t>(row) * rowPitch;
        raw.insert(raw.end(), source, source + rowBytes);
    }

    // zlib stream: 2 byte header, stored deflate blocks, 4 byte Adler-32 of the input.
    std::vector<unsigned char> zlib;
    zlib.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    constexpr std::size_t maxBlock = 65535u;
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const std::size_t remaining = raw.size() - offset;
        const std::size_t blockSize = remaining < maxBlock ? remaining : maxBlock;
        const bool finalBlock = (offset + blockSize) >= raw.size();
        zlib.push_back(finalBlock ? 1 : 0);
        zlib.push_back(static_cast<unsigned char>(blockSize & 0xFF));
        zlib.push_back(static_cast<unsigned char>((blockSize >> 8) & 0xFF));
        zlib.push_back(static_cast<unsigned char>(~blockSize & 0xFF));
        zlib.push_back(static_cast<unsigned char>((~blockSize >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));
        offset += blockSize;
    }
    appendBigEndian32(zlib, adler32(raw));

    std::vector<unsigned char> png;
    const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), signature, signature + 8);

    std::vector<unsigned char> header;
    appendBigEndian32(header, width);
    appendBigEndian32(header, height);
    header.push_back(8);  // bit depth
    header.push_back(6);  // colour type: RGBA
    header.push_back(0);  // deflate
    header.push_back(0);  // adaptive filtering
    header.push_back(0);  // no interlace
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", zlib);
    appendChunk(png, "IEND", {});

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Cannot open screenshot file for writing: " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        error = "Failed while writing screenshot: " + path;
        return false;
    }
    return true;
}

} // namespace nativegpu
