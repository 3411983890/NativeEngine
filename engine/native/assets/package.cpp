#include "package.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <limits>

namespace engine::assets {
namespace {
constexpr char kMagic[8] = {'N', 'G', 'P', 'K', 'G', '\0', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kHeaderBytes = 32;
constexpr std::uint32_t kSectionBytes = 24;
constexpr std::uint32_t kAlignment = 16;
constexpr std::uint32_t kMaxSections = 4096;

[[noreturn]] void fail(const std::string& message) { throw Error("NG-E-VEG-FORMAT", message); }

std::uint32_t readU32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::string readFile(const std::string& file, std::size_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > maximum) fail("package missing or larger than the load limit: " + file);
    std::ifstream stream(file, std::ios::binary);
    if (!stream) fail("package cannot be opened: " + file);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) fail("package changed while being read: " + file);
    return bytes;
}
}

Error::Error(std::string codeValue, std::string message) : std::runtime_error(std::move(message)), code(std::move(codeValue)) {}

std::uint32_t sectionStride(std::uint32_t kind) {
    switch (static_cast<SectionKind>(kind)) {
        case SectionKind::PositionF32x3: return 12;
        case SectionKind::NormalF32x3: return 12;
        case SectionKind::UvF32x2: return 8;
        case SectionKind::ColorU8x4: return 4;
        case SectionKind::IndexU32: return 4;
        case SectionKind::Primitive: return 12;
        case SectionKind::Lod: return 40;
        case SectionKind::InstancePacked: return 32;
        case SectionKind::Page: return 64;
        case SectionKind::Spatial: return 16;
        case SectionKind::CurveHeader: return 16;
        case SectionKind::CurvePointF32x3: return 12;
        case SectionKind::CurveRadiusF32: return 4;
        case SectionKind::CurveAttribute: return 16;
        case SectionKind::Binding: return 32;
        case SectionKind::Card: return 32;
        case SectionKind::ImpostorRgba8: return 4;
        case SectionKind::ImpostorDepthF32: return 4;
        case SectionKind::WindVertex: return 4;
        case SectionKind::Collision: return 32;
        case SectionKind::ShadowProxy: return 24;
        case SectionKind::GuideWeight: return 32;
        case SectionKind::MetaJson: return 0;
    }
    return 0;
}

const char* sectionKindName(std::uint32_t kind) {
    switch (static_cast<SectionKind>(kind)) {
        case SectionKind::MetaJson: return "META_JSON";
        case SectionKind::PositionF32x3: return "POSITION_F32X3";
        case SectionKind::NormalF32x3: return "NORMAL_F32X3";
        case SectionKind::UvF32x2: return "UV_F32X2";
        case SectionKind::ColorU8x4: return "COLOR_U8X4";
        case SectionKind::IndexU32: return "INDEX_U32";
        case SectionKind::Primitive: return "PRIMITIVE";
        case SectionKind::Lod: return "LOD";
        case SectionKind::InstancePacked: return "INSTANCE_PACKED";
        case SectionKind::Page: return "PAGE";
        case SectionKind::Spatial: return "SPATIAL";
        case SectionKind::CurveHeader: return "CURVE_HEADER";
        case SectionKind::CurvePointF32x3: return "CURVE_POINT_F32X3";
        case SectionKind::CurveRadiusF32: return "CURVE_RADIUS_F32";
        case SectionKind::CurveAttribute: return "CURVE_ATTRIBUTE";
        case SectionKind::Binding: return "BINDING";
        case SectionKind::Card: return "CARD";
        case SectionKind::ImpostorRgba8: return "IMPOSTOR_RGBA8";
        case SectionKind::ImpostorDepthF32: return "IMPOSTOR_DEPTH_F32";
        case SectionKind::WindVertex: return "WIND_VERTEX";
        case SectionKind::Collision: return "COLLISION";
        case SectionKind::ShadowProxy: return "SHADOW_PROXY";
        case SectionKind::GuideWeight: return "GUIDE_WEIGHT";
    }
    return "UNKNOWN";
}

const Section* Package::find(SectionKind kind, std::uint32_t index) const {
    for (const auto& section : sections) if (section.kind == static_cast<std::uint32_t>(kind) && section.index == index) return &section;
    return nullptr;
}

const Section& Package::require(SectionKind kind, std::uint32_t index) const {
    const auto* section = find(kind, index);
    if (!section) fail(std::string("package is missing ") + sectionKindName(static_cast<std::uint32_t>(kind)) + "#" + std::to_string(index));
    if (!section->data) fail(std::string("package section was not loaded: ") + sectionKindName(section->kind));
    return *section;
}

std::vector<float> Package::floats(SectionKind kind, std::uint32_t index) const {
    const auto& section = require(kind, index);
    if (section.bytes % 4 != 0) fail("float section is not 4-byte aligned");
    std::vector<float> result(section.bytes / 4);
    std::memcpy(result.data(), section.data, section.bytes);
    return result;
}

std::vector<std::uint32_t> Package::uints(SectionKind kind, std::uint32_t index) const {
    const auto& section = require(kind, index);
    if (section.bytes % 4 != 0) fail("uint section is not 4-byte aligned");
    std::vector<std::uint32_t> result(section.bytes / 4);
    std::memcpy(result.data(), section.data, section.bytes);
    return result;
}

std::vector<std::uint8_t> Package::bytes(SectionKind kind, std::uint32_t index) const {
    const auto& section = require(kind, index);
    return std::vector<std::uint8_t>(section.data, section.data + section.bytes);
}

Package readPackage(const std::string& bytes, std::size_t maximumBytes) {
    if (bytes.size() < kHeaderBytes) fail("package shorter than its header");
    if (bytes.size() > maximumBytes) fail("package exceeds the load limit");
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (std::memcmp(data, kMagic, 8) != 0) fail("package magic mismatch");
    const auto version = readU32(data + 8);
    if (version != kVersion) fail("unsupported package version " + std::to_string(version));
    const auto headerBytes = readU32(data + 12);
    const auto sectionCount = readU32(data + 16);
    if (readU32(data + 20) != 0 || readU32(data + 28) != 0) fail("package reserved header fields must be zero");
    if (sectionCount == 0 || sectionCount > kMaxSections) fail("package section count out of range");
    if (headerBytes != kHeaderBytes + kSectionBytes * sectionCount) fail("package header size disagrees with its section count");
    if (readU32(data + 24) != bytes.size()) fail("package declares a different total size than it occupies");

    Package package;
    package.totalBytes = static_cast<std::uint32_t>(bytes.size());
    std::uint32_t previousKind = 0, previousIndex = 0;
    bool havePrevious = false;
    for (std::uint32_t position = 0; position < sectionCount; ++position) {
        const auto* entry = data + kHeaderBytes + kSectionBytes * position;
        Section section;
        section.kind = readU32(entry);
        section.index = readU32(entry + 4);
        section.offset = readU32(entry + 8);
        section.bytes = readU32(entry + 12);
        section.count = readU32(entry + 16);
        if (readU32(entry + 20) != 0) fail("section reserved field must be zero");
        if (section.kind == 0 || section.kind > 0xFFFF) fail("section kind out of range");
        if (section.offset < headerBytes || section.offset % kAlignment != 0) fail("section offset is not aligned or lies inside the header");
        if (static_cast<std::uint64_t>(section.offset) + section.bytes > bytes.size()) fail("section extends past the end of the package");
        const auto stride = sectionStride(section.kind);
        if (stride != 0 && (section.bytes % stride != 0 || section.count != section.bytes / stride)) {
            fail(std::string(sectionKindName(section.kind)) + " record size disagrees with its element count");
        }
        if (havePrevious && section.kind == previousKind && section.index == previousIndex) fail("duplicate section");
        havePrevious = true; previousKind = section.kind; previousIndex = section.index;
        section.data = data + section.offset;
        package.sections.push_back(section);
    }

    const auto* meta = package.find(SectionKind::MetaJson, 0);
    if (!meta) fail("package has no META_JSON section");
    try {
        package.meta = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(meta->data), meta->bytes));
    } catch (const std::exception&) {
        throw Error("NG-E-VEG-FORMAT", "package meta is not valid JSON");
    }
    if (!package.meta.is_object() || !package.meta.contains("schema") || !package.meta["schema"].is_string()) {
        fail("package meta has no schema");
    }
    package.schema = package.meta["schema"].get<std::string>();
    return package;
}

Package loadPackage(const std::string& file, std::size_t maximumBytes) {
    return readPackage(readFile(file, maximumBytes), maximumBytes);
}

PackageFile::PackageFile(const std::string& file) : path_(file) {
    std::error_code error;
    fileBytes_ = std::filesystem::file_size(file, error);
    if (error) throw Error("NG-E-VEG-FORMAT", "package cannot be opened: " + file);
    if (fileBytes_ < kHeaderBytes) throw Error("NG-E-VEG-FORMAT", "package shorter than its header");
    stream_.open(file, std::ios::binary);
    if (!stream_) throw Error("NG-E-VEG-FORMAT", "package cannot be opened: " + file);
    std::array<unsigned char, kHeaderBytes> header{};
    stream_.read(reinterpret_cast<char*>(header.data()), header.size());
    if (std::memcmp(header.data(), kMagic, 8) != 0) throw Error("NG-E-VEG-FORMAT", "package magic mismatch");
    if (readU32(header.data() + 8) != kVersion) throw Error("NG-E-VEG-FORMAT", "unsupported package version");
    const auto headerBytes = readU32(header.data() + 12);
    const auto sectionCount = readU32(header.data() + 16);
    if (sectionCount == 0 || sectionCount > kMaxSections) throw Error("NG-E-VEG-FORMAT", "package section count out of range");
    if (headerBytes != kHeaderBytes + kSectionBytes * sectionCount) throw Error("NG-E-VEG-FORMAT", "package header size disagrees with its section count");
    if (readU32(header.data() + 24) != fileBytes_) throw Error("NG-E-VEG-FORMAT", "package declares a different total size than it occupies");

    std::vector<unsigned char> table(headerBytes - kHeaderBytes);
    stream_.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
    package_.totalBytes = static_cast<std::uint32_t>(fileBytes_);
    for (std::uint32_t position = 0; position < sectionCount; ++position) {
        const auto* entry = table.data() + kSectionBytes * position;
        Section section;
        section.kind = readU32(entry);
        section.index = readU32(entry + 4);
        section.offset = readU32(entry + 8);
        section.bytes = readU32(entry + 12);
        section.count = readU32(entry + 16);
        if (static_cast<std::uint64_t>(section.offset) + section.bytes > fileBytes_) throw Error("NG-E-VEG-FORMAT", "section extends past the end of the file");
        const auto stride = sectionStride(section.kind);
        if (stride != 0 && (section.bytes % stride != 0 || section.count != section.bytes / stride)) throw Error("NG-E-VEG-FORMAT", "section record size disagrees with its element count");
        package_.sections.push_back(section);
    }
    const auto* meta = package_.find(SectionKind::MetaJson, 0);
    if (!meta) throw Error("NG-E-VEG-FORMAT", "package has no META_JSON section");
    auto bytes = readSection(SectionKind::MetaJson, 0);
    package_.meta = nlohmann::json::parse(std::string(bytes.begin(), bytes.end()));
    package_.schema = package_.meta.value("schema", std::string());
}

std::vector<std::uint8_t> PackageFile::readSection(SectionKind kind, std::uint32_t index, std::uint32_t expectedBytes) const {
    const auto* section = package_.find(kind, index);
    if (!section) throw Error("NG-E-VEG-FORMAT", std::string("package is missing ") + sectionKindName(static_cast<std::uint32_t>(kind)));
    if (expectedBytes != 0 && section->bytes != expectedBytes) {
        throw Error("NG-E-VEG-FORMAT", std::string("section ") + sectionKindName(section->kind) + "#" + std::to_string(index)
            + " is " + std::to_string(section->bytes) + " bytes, expected " + std::to_string(expectedBytes));
    }
    std::vector<std::uint8_t> result(section->bytes);
    stream_.clear();
    stream_.seekg(section->offset, std::ios::beg);
    if (!stream_) throw Error("NG-E-VEG-FORMAT", "package seek failed");
    stream_.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(result.size())) throw Error("NG-E-VEG-FORMAT", "package section read was short");
    return result;
}

std::vector<std::uint8_t> PackageFile::readSectionRange(SectionKind kind, std::uint32_t index, std::uint64_t byteOffset, std::uint32_t bytes) const {
    const auto* section = package_.find(kind, index);
    if (!section) throw Error("NG-E-VEG-FORMAT", std::string("package is missing ") + sectionKindName(static_cast<std::uint32_t>(kind)));
    if (byteOffset + bytes > section->bytes) throw Error("NG-E-VEG-PAGE", "section range extends past the section");
    std::vector<std::uint8_t> result(bytes);
    if (bytes == 0) return result;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(section->offset + byteOffset), std::ios::beg);
    if (!stream_) throw Error("NG-E-VEG-FORMAT", "package seek failed");
    stream_.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(result.size())) throw Error("NG-E-VEG-FORMAT", "package section read was short");
    return result;
}

std::vector<float> PackageFile::readFloats(SectionKind kind, std::uint32_t index) const {    const auto* section = package_.find(kind, index);
    if (!section || section->bytes % 4 != 0) throw Error("NG-E-VEG-FORMAT", "float section missing or misaligned");
    const auto bytes = readSection(kind, index);
    std::vector<float> result(bytes.size() / 4);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

std::vector<std::uint32_t> PackageFile::readUints(SectionKind kind, std::uint32_t index) const {
    const auto* section = package_.find(kind, index);
    if (!section || section->bytes % 4 != 0) throw Error("NG-E-VEG-FORMAT", "uint section missing or misaligned");
    const auto bytes = readSection(kind, index);
    std::vector<std::uint32_t> result(bytes.size() / 4);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

} // namespace engine::assets
