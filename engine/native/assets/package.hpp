#pragma once

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../third_party/json.hpp"

// Native reader for the content-addressed package container written by
// engine/assets/binary-package.mjs.
//
// The reader never trusts a length, an offset or an element count: every one of them
// is checked against the file size and the declared record stride before a single byte
// is interpreted. A truncated or edited package is rejected, not partially loaded.
//
// Two access modes exist on purpose. A species or groom package is small and is read
// whole. A 30M-instance region is not: `PackageFile` reads only the header and then
// seeks to the sections a page actually needs, so residency stays bounded by the
// working set instead of by the world index.

namespace engine::assets {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string codeValue, std::string message);
};

enum class SectionKind : std::uint32_t {
    MetaJson = 1,
    PositionF32x3 = 2,
    NormalF32x3 = 3,
    UvF32x2 = 4,
    ColorU8x4 = 5,
    IndexU32 = 6,
    Primitive = 7,
    Lod = 8,
    InstancePacked = 9,
    Page = 10,
    Spatial = 11,
    CurveHeader = 12,
    CurvePointF32x3 = 13,
    CurveRadiusF32 = 14,
    CurveAttribute = 15,
    Binding = 16,
    Card = 17,
    ImpostorRgba8 = 18,
    ImpostorDepthF32 = 19,
    WindVertex = 20,
    Collision = 21,
    ShadowProxy = 22,
    GuideWeight = 23,
};

struct Section {
    std::uint32_t kind = 0;
    std::uint32_t index = 0;
    std::uint32_t offset = 0;
    std::uint32_t bytes = 0;
    std::uint32_t count = 0;
    const unsigned char* data = nullptr; // null for a header-only read
};

class Package {
public:
    std::string schema;
    nlohmann::json meta;
    std::vector<Section> sections;
    std::uint32_t totalBytes = 0;

    const Section* find(SectionKind kind, std::uint32_t index = 0) const;
    const Section& require(SectionKind kind, std::uint32_t index = 0) const;
    bool has(SectionKind kind, std::uint32_t index = 0) const { return find(kind, index) != nullptr; }
    // Typed copies. The package owns no payload for a header-only read, so these are
    // copies rather than views; callers that need aliasing use Section::data directly.
    std::vector<float> floats(SectionKind kind, std::uint32_t index = 0) const;
    std::vector<std::uint32_t> uints(SectionKind kind, std::uint32_t index = 0) const;
    std::vector<std::uint8_t> bytes(SectionKind kind, std::uint32_t index = 0) const;
};

// Whole-file read. `maximumBytes` bounds the allocation a hostile file can cause.
Package readPackage(const std::string& bytes, std::size_t maximumBytes = 256ull * 1024 * 1024);
Package loadPackage(const std::string& file, std::size_t maximumBytes = 256ull * 1024 * 1024);

// Random-access reader: header first, then only the sections a consumer asks for.
class PackageFile {
public:
    explicit PackageFile(const std::string& file);
    const Package& header() const { return package_; }
    // Reads one section's bytes. `expectedBytes` is the exact size the caller expects
    // (0 means "whatever the table declares"), so a layout change fails loudly.
    std::vector<std::uint8_t> readSection(SectionKind kind, std::uint32_t index = 0, std::uint32_t expectedBytes = 0) const;
    // Reads a byte range inside one section. This is what makes page-level streaming
    // possible: a 960 MB instance section is never resident, only the requested page.
    std::vector<std::uint8_t> readSectionRange(SectionKind kind, std::uint32_t index, std::uint64_t byteOffset, std::uint32_t bytes) const;
    std::vector<float> readFloats(SectionKind kind, std::uint32_t index = 0) const;
    std::vector<std::uint32_t> readUints(SectionKind kind, std::uint32_t index = 0) const;
    const std::string& path() const { return path_; }
private:
    std::string path_;
    std::uint64_t fileBytes_ = 0;
    Package package_;
    mutable std::ifstream stream_;
};

const char* sectionKindName(std::uint32_t kind);
// Element stride for fixed-layout kinds, 0 for variable-length ones.
std::uint32_t sectionStride(std::uint32_t kind);

} // namespace engine::assets
