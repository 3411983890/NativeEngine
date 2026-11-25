#pragma once

#include "../assets/package.hpp"
#include "../vec3.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::vegetation {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string codeValue, std::string message);
};

enum class SpeciesKind : std::uint32_t { Tree = 0, Shrub = 1, Grass = 2 };
enum class MaterialKind : std::uint32_t { Bark = 0, Leaf = 1, Blade = 2 };

struct Lod {
    std::uint32_t level = 0, vertices = 0, triangles = 0, firstPrimitive = 0, primitiveCount = 0;
    float worldErrorMetres = 0, errorP95Metres = 0, screenErrorPixels = 0, heightMetres = 0, radiusMetres = 0;
};

struct Material {
    std::string id;
    MaterialKind kind = MaterialKind::Bark;
    bool alphaMasked = false, doubleSided = false;
    // Resolved factor and metres. The source may spell them roughnessFactor /
    // thicknessScaleMetres; the loader normalises both spellings into these two fields.
    float roughness = 0.8f, thicknessMetres = 0.001f;
    float transmission[3] = {0, 0, 0};
    // Contract fields the shader consumes. Each one has a consumer: normalScale scales the
    // tangent-space normal, absorptionPerMetre and thicknessMetres drive transmission,
    // occlusionStrength scales ORM.R, heightScaleMetres drives bark parallax, alphaCutoff is
    // the discard threshold that must equal the cook's coverageCutoff.
    float normalScale = 1.0f;
    float absorptionPerMetre = 900.0f;
    float occlusionStrength = 1.0f;
    float heightScaleMetres = 0.0f;
    float alphaCutoff = 0.35f;
    // True when the authored normal sheet is OpenGL-convention (green points up the surface).
    // The shader flips the green channel once when this is set: the sign belongs to the source
    // image, not to the renderer's coordinate system.
    bool flipNormalGreen = false;
    float baseColorFactor[3] = {1.0f, 1.0f, 1.0f};
};

struct Primitive {
    std::uint16_t material = 0, lod = 0;
    std::uint32_t firstIndex = 0, indexCount = 0;
};

// One LOD's drawable geometry. Positions/normals/uvs are parallel arrays; `colors`
// is RGBA8 and `windWeights` is the bounded procedural sway weight per vertex.
struct Mesh {
    std::vector<float> positions, normals, uvs, windWeights;
    std::vector<std::uint8_t> colors;
    std::vector<std::uint32_t> indices;
    std::size_t vertexCount() const { return positions.size() / 3; }
    std::size_t triangleCount() const { return indices.size() / 3; }
};

struct ShadowProxy {
    float centre[3] = {0, 0, 0};
    float extent[3] = {0, 0, 0};
};

struct CollisionCylinder {
    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    float radius = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(CollisionCylinder) == 32, "CollisionCylinder must match the 32-byte package record");

struct Species {
    std::string id, schema;
    SpeciesKind kind = SpeciesKind::Tree;
    std::uint32_t profile = 0, seed = 0;
    std::array<float, 3> boundsMin{}, boundsMax{};
    std::vector<Lod> lod;
    std::vector<Material> materials;
    std::vector<Primitive> primitives;
    std::vector<Mesh> meshes;
    std::uint32_t impostorViews = 0, impostorSize = 0;
    std::vector<std::uint8_t> impostorAtlas;
    std::vector<float> impostorDepth;
    ShadowProxy shadowProxy;
    std::vector<CollisionCylinder> collision;
    std::vector<std::string> representations;
    // Role -> digest map exactly as cooked. The renderer resolves the digest to a file and
    // never guesses a texture by material name.
    nlohmann::json textures;

    // A representation that the manifest claims but the file does not contain is a
    // cook bug. The loader rejects it here so no renderer can silently draw nothing.
    bool has(const std::string& name) const;
    const Mesh& meshForLod(std::uint32_t level) const;
};

// Whole-package parse. `expectedDigest` empty skips authentication; a non-empty value
// must match the SHA-256 of the exact bytes or the load fails.
Species parseSpecies(const std::string& bytes, const std::string& expectedDigest = {});
Species loadSpecies(const std::string& file, const std::string& expectedDigest = {});

struct SpeciesRef {
    std::string id, digest;
    SpeciesKind kind = SpeciesKind::Tree;
    std::vector<std::uint32_t> lodTriangles;
    std::vector<float> lodErrorMetres;
    float heightMetres = 0, radiusMetres = 0;
};

static_assert(sizeof(Primitive) == 12, "Primitive must match the 12-byte package record");

struct PageRecord {
    std::uint32_t firstInstance = 0, instanceCount = 0;
    float minX = 0, minZ = 0, maxX = 0, maxZ = 0, baseY = 0, heightRange = 0;
    std::uint32_t cellX = 0, cellZ = 0, flags = 0, pageIndex = 0, lodMask = 0, reserved = 0;
    std::uint32_t stableLo = 0, stableHi = 0;
};

// 32-byte packed instance. The field order is the ABI shared with vegetation.hlsl and
// with engine/vegetation/region.mjs; a static_assert pins the size.
struct PackedInstance {
    std::uint16_t qx = 0, qz = 0, qy = 0, yaw = 0;
    std::int16_t tiltX = 0, tiltZ = 0;
    std::uint8_t scale = 0, species = 0;
    std::uint16_t windPhase = 0;
    std::uint8_t lodBias = 0, flags = 0;
    std::uint16_t palette = 0;
    std::uint32_t stableId = 0, aggregate = 0, reserved = 0;
};
static_assert(sizeof(PackedInstance) == 32, "PackedInstance must stay 32 bytes");

static_assert(sizeof(PageRecord) == 64, "PageRecord must match the 64-byte package record");

struct SpatialCell { std::uint32_t cellX = 0, cellZ = 0, firstPage = 0, pageCount = 0; };
static_assert(sizeof(SpatialCell) == 16, "SpatialCell must match the 16-byte package record");

struct DecodedInstance {
    Vec3 position;
    float yaw = 0, tiltX = 0, tiltZ = 0, scale = 1;
    std::uint32_t species = 0, stableId = 0, lodBias = 0, flags = 0, palette = 0, windPhase = 0;
};

DecodedInstance decodeInstance(const PackedInstance& packed, const PageRecord& page);

struct Region {
    std::string id, schema, surfaceKind, surfaceVersion;
    std::uint32_t seed = 0;
    float boundsMinX = 0, boundsMinZ = 0, boundsMaxX = 0, boundsMaxZ = 0;
    float cellSizeMetres = 0, pageCellMetres = 0;
    std::uint32_t pageSize = 0;
    std::vector<SpeciesRef> species;
    std::vector<PageRecord> pages;
    std::vector<SpatialCell> spatial;
    std::vector<PackedInstance> instances;
    nlohmann::json quantization;
    std::uint64_t worldInstances = 0;
    bool hasAggregateRepresentation = false;
};

// Whole-region load. Intended for editor/verification paths; the streaming runtime
// uses RegionFile so residency is bounded by pages, not by the world index.
Region parseRegion(const std::string& bytes, const std::string& expectedDigest = {});
Region loadRegion(const std::string& file, const std::string& expectedDigest = {});

// Paged region reader: the header, page table and spatial index are small and loaded
// once; instance records are read a page at a time, so a 30M-instance world index does
// not need to be resident to draw one valley.
class RegionFile {
public:
    explicit RegionFile(const std::string& file);
    const std::string& id() const { return id_; }
    const std::vector<SpeciesRef>& species() const { return species_; }
    std::uint32_t pageCount() const { return static_cast<std::uint32_t>(pages_.size()); }
    const PageRecord& page(std::uint32_t index) const;
    const std::vector<SpatialCell>& spatial() const { return spatial_; }
    std::uint64_t worldInstances() const { return worldInstances_; }
    std::vector<PackedInstance> readPage(std::uint32_t index) const;
    const std::string& path() const { return file_.path(); }
private:
    engine::assets::PackageFile file_;
    std::string id_;
    std::vector<SpeciesRef> species_;
    std::vector<PageRecord> pages_;
    std::vector<SpatialCell> spatial_;
    std::uint64_t worldInstances_ = 0;
};

} // namespace engine::vegetation
