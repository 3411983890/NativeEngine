#include "asset.hpp"

#include "../assets/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace engine::vegetation {
using engine::assets::Package;
using engine::assets::SectionKind;

namespace {
[[noreturn]] void fail(const std::string& code, const std::string& message) { throw Error(code, message); }

const nlohmann::json& field(const nlohmann::json& value, const char* key, const char* where) {
    if (!value.is_object() || !value.contains(key)) fail("NG-E-VEG-FORMAT", std::string(where) + " is missing " + key);
    return value.at(key);
}

std::uint32_t unsignedField(const nlohmann::json& value, const char* key, std::uint32_t maximum, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_number()) fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " is not numeric");
    const auto result = entry.get<double>();
    if (!std::isfinite(result) || result < 0 || result > maximum || std::floor(result) != result) {
        fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " is out of range");
    }
    return static_cast<std::uint32_t>(result);
}

float floatField(const nlohmann::json& value, const char* key, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_number()) fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " is not numeric");
    const auto result = entry.get<double>();
    if (!std::isfinite(result) || std::fabs(result) > 1e7) fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " is not finite");
    return static_cast<float>(result);
}

std::string textField(const nlohmann::json& value, const char* key, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_string()) fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " is not a string");
    return entry.get<std::string>();
}

std::vector<std::string> stringArray(const nlohmann::json& value, const char* key, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_array() || entry.size() > 32) fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " is not a bounded array");
    std::vector<std::string> result;
    for (const auto& item : entry) {
        if (!item.is_string()) fail("NG-E-VEG-FORMAT", std::string(where) + "." + key + " contains a non-string");
        result.push_back(item.get<std::string>());
    }
    return result;
}

void requireSize(std::size_t actual, std::size_t expected, const char* what) {
    if (actual != expected) fail("NG-E-VEG-FORMAT", std::string(what) + " has " + std::to_string(actual) + " entries, expected " + std::to_string(expected));
}

template <class T>
std::vector<T> copyRecords(const engine::assets::Package& package, SectionKind kind, std::uint32_t index) {
    const auto& section = package.require(kind, index);
    std::vector<T> result(section.count);
    if (!result.empty()) std::memcpy(result.data(), section.data, section.bytes);
    return result;
}

std::vector<SpeciesRef> readSpeciesRefs(const nlohmann::json& meta) {
    const auto& list = field(meta, "species", "region meta");
    if (!list.is_array() || list.empty() || list.size() > 255) fail("NG-E-VEG-FORMAT", "region species table must have 1–255 entries");
    std::vector<SpeciesRef> result;
    for (const auto& entry : list) {
        SpeciesRef ref;
        ref.id = textField(entry, "id", "region species");
        ref.digest = textField(entry, "digest", "region species");
        const auto kind = textField(entry, "kind", "region species");
        ref.kind = kind == "grass" ? SpeciesKind::Grass : kind == "shrub" ? SpeciesKind::Shrub : SpeciesKind::Tree;
        const auto& triangles = field(entry, "lodTriangles", "region species");
        const auto& errors = field(entry, "lodErrorMetres", "region species");
        if (!triangles.is_array() || !errors.is_array() || triangles.size() != errors.size() || triangles.size() > 4) {
            fail("NG-E-VEG-FORMAT", "region species LOD tables disagree");
        }
        for (std::size_t level = 0; level < triangles.size(); ++level) {
            ref.lodTriangles.push_back(triangles[level].get<std::uint32_t>());
            ref.lodErrorMetres.push_back(errors[level].get<float>());
        }
        ref.heightMetres = floatField(entry, "heightMetres", "region species");
        ref.radiusMetres = floatField(entry, "radiusMetres", "region species");
        result.push_back(std::move(ref));
    }
    return result;
}

std::vector<PageRecord> readPages(const engine::assets::Package& package) {
    auto pages = copyRecords<PageRecord>(package, SectionKind::Page, 0);
    std::uint64_t expected = 0;
    for (std::size_t index = 0; index < pages.size(); ++index) {
        const auto& page = pages[index];
        if (page.firstInstance != expected) fail("NG-E-VEG-PAGE", "pages are not contiguous");
        if (page.instanceCount == 0) fail("NG-E-VEG-PAGE", "page has no instances");
        if (!(page.maxX >= page.minX) || !(page.maxZ >= page.minZ) || !(page.heightRange > 0)) {
            fail("NG-E-VEG-PAGE", "page bounds are inverted or have no height range");
        }
        if (page.pageIndex != index) fail("NG-E-VEG-PAGE", "page index disagrees with its position");
        expected += page.instanceCount;
    }
    return pages;
}

std::vector<SpatialCell> readSpatial(const engine::assets::Package& package, const std::vector<PageRecord>& pages) {
    auto cells = copyRecords<SpatialCell>(package, SectionKind::Spatial, 0);
    std::uint32_t covered = 0;
    for (const auto& cell : cells) {
        if (cell.pageCount == 0) fail("NG-E-VEG-PAGE", "spatial cell has no pages");
        if (cell.firstPage != covered) fail("NG-E-VEG-PAGE", "spatial cells do not tile the page table");
        covered += cell.pageCount;
    }
    if (covered != pages.size()) fail("NG-E-VEG-PAGE", "spatial index does not cover every page");
    return cells;
}

std::vector<PackedInstance> readInstances(const engine::assets::Package& package, std::uint64_t expected) {
    const auto& section = package.require(SectionKind::InstancePacked, 0);
    if (section.count != expected) fail("NG-E-VEG-PAGE", "instance section size disagrees with the page table");
    return copyRecords<PackedInstance>(package, SectionKind::InstancePacked, 0);
}

void validateInstances(const std::vector<PackedInstance>& instances, std::size_t speciesCount) {
    std::set<std::uint32_t> ids;
    for (const auto& instance : instances) {
        if (instance.species >= speciesCount) fail("NG-E-VEG-SPECIES", "instance references a species outside the region table");
        if (instance.reserved != 0) fail("NG-E-VEG-FORMAT", "instance reserved field must be zero");
        if (!ids.insert(instance.stableId).second) fail("NG-E-VEG-FORMAT", "duplicate stable instance id");
    }
}
}

Error::Error(std::string codeValue, std::string message) : std::runtime_error(std::move(message)), code(std::move(codeValue)) {}

bool Species::has(const std::string& name) const {
    return std::find(representations.begin(), representations.end(), name) != representations.end();
}

const Mesh& Species::meshForLod(std::uint32_t level) const {
    if (level >= meshes.size()) fail("NG-E-VEG-FORMAT", "LOD index outside the species mesh table");
    return meshes[level];
}

Species parseSpecies(const std::string& bytes, const std::string& expectedDigest) {
    if (!expectedDigest.empty() && engine::assets::sha256(bytes) != expectedDigest) {
        fail("NG-E-VEG-HASH", "species bytes do not match the expected digest");
    }
    const auto package = engine::assets::readPackage(bytes);
    if (package.schema != "nativegpu.vegetation-species-cook/1") fail("NG-E-VEG-FORMAT", "unsupported species schema " + package.schema);
    const auto& meta = package.meta;

    Species species;
    species.schema = package.schema;
    species.id = textField(meta, "id", "species meta");
    species.seed = unsignedField(meta, "seed", 0xFFFFFFFFu, "species meta");
    const auto kind = textField(meta, "kind", "species meta");
    species.kind = kind == "grass" ? SpeciesKind::Grass : kind == "shrub" ? SpeciesKind::Shrub : SpeciesKind::Tree;
    const auto profile = textField(meta, "profile", "species meta");
    species.profile = profile == "conifer" ? 1u : profile == "shrub" ? 2u : profile == "grass" ? 3u : 0u;
    species.representations = stringArray(meta, "representations", "species meta");

    const auto& bounds = field(meta, "bounds", "species meta");
    const auto& boundsMin = field(bounds, "min", "species bounds");
    const auto& boundsMax = field(bounds, "max", "species bounds");
    if (!boundsMin.is_array() || boundsMin.size() != 3 || !boundsMax.is_array() || boundsMax.size() != 3) {
        fail("NG-E-VEG-FORMAT", "species bounds need three values per side");
    }
    for (int axis = 0; axis < 3; ++axis) {
        species.boundsMin[axis] = boundsMin[axis].get<float>();
        species.boundsMax[axis] = boundsMax[axis].get<float>();
        if (!std::isfinite(species.boundsMin[axis]) || !std::isfinite(species.boundsMax[axis])) {
            fail("NG-E-VEG-FORMAT", "species bounds are not finite");
        }
        if (!(species.boundsMax[axis] > species.boundsMin[axis])) fail("NG-E-VEG-FORMAT", "species bounds are empty");
    }

    const auto& lodList = field(meta, "lod", "species meta");
    if (!lodList.is_array() || lodList.empty() || lodList.size() > 4) fail("NG-E-VEG-FORMAT", "species LOD table must have 1–4 entries");
    for (std::size_t index = 0; index < lodList.size(); ++index) {
        const auto& entry = lodList[index];
        Lod lod;
        lod.level = unsignedField(entry, "level", 3, "species lod");
        lod.vertices = unsignedField(entry, "vertices", 400000, "species lod");
        lod.triangles = unsignedField(entry, "triangles", 800000, "species lod");
        lod.firstPrimitive = unsignedField(entry, "firstPrimitive", 0xFFFFFFFFu, "species lod");
        lod.primitiveCount = unsignedField(entry, "primitiveCount", 0xFFFFu, "species lod");
        lod.worldErrorMetres = floatField(entry, "worldErrorMetres", "species lod");
        lod.errorP95Metres = floatField(entry, "errorP95Metres", "species lod");
        lod.screenErrorPixels = floatField(entry, "screenErrorPixels", "species lod");
        lod.heightMetres = floatField(entry, "heightMetres", "species lod");
        lod.radiusMetres = floatField(entry, "radiusMetres", "species lod");
        if (lod.level != index) fail("NG-E-VEG-FORMAT", "species LOD levels must be ordered from 0");
        if (index > 0) {
            if (lod.triangles > species.lod.back().triangles) fail("NG-E-VEG-FORMAT", "a coarser LOD adds triangles");
            if (lod.worldErrorMetres < species.lod.back().worldErrorMetres * 0.98f) fail("NG-E-VEG-FORMAT", "a coarser LOD reports a smaller error");
        }
        species.lod.push_back(lod);
    }

    const auto& materials = field(meta, "materials", "species meta");
    if (!materials.is_array() || materials.empty() || materials.size() > 8) fail("NG-E-VEG-FORMAT", "species materials must have 1–8 entries");
    for (const auto& entry : materials) {
        Material material;
        material.id = textField(entry, "id", "species material");
        const auto kindName = textField(entry, "kind", "species material");
        material.kind = kindName == "leaf" ? MaterialKind::Leaf : kindName == "blade" ? MaterialKind::Blade : MaterialKind::Bark;
        material.alphaMasked = entry.contains("alphaMasked")
            ? entry.at("alphaMasked").get<bool>()
            : (entry.value("alphaMode", std::string("opaque")) == "mask");
        material.doubleSided = entry.value("doubleSided", material.kind != MaterialKind::Bark);
        material.roughness = entry.contains("roughnessFactor")
            ? entry.at("roughnessFactor").get<float>()
            : floatField(entry, "roughness", "species material");
        material.thicknessMetres = entry.contains("thicknessScaleMetres")
            ? entry.at("thicknessScaleMetres").get<float>()
            : floatField(entry, "thicknessMetres", "species material");
        material.normalScale = entry.value("normalScale", 1.0f);
        material.absorptionPerMetre = entry.value("absorptionPerMetre", 900.0f);
        material.occlusionStrength = entry.value("occlusionStrength", 1.0f);
        material.heightScaleMetres = entry.value("heightScaleMetres", 0.0f);
        material.alphaCutoff = entry.value("alphaCutoff", 0.35f);
        // Normal-map convention. The name says what the author's image does, not what D3D wants:
        // `directX-` keeps the green channel as stored, `openGL+` flips it once at sample time.
        // An unknown spelling is a hard error because guessing would silently invert the lighting
        // on one of the two materials.
        if (entry.contains("normalConvention")) {
            const auto convention = entry.at("normalConvention").get<std::string>();
            if (convention == "openGL+") material.flipNormalGreen = true;
            else if (convention == "directX-") material.flipNormalGreen = false;
            else fail("NG-E-VEG-FORMAT", std::string("unsupported normalConvention '") + convention
                + "' (expected directX- or openGL+)");
        }
        if (entry.contains("baseColorFactor") && entry.at("baseColorFactor").is_array()) {
            const auto& factor = entry.at("baseColorFactor");
            for (int channel = 0; channel < 3 && channel < static_cast<int>(factor.size()); ++channel) {
                material.baseColorFactor[channel] = factor[channel].get<float>();
            }
        }
        const auto& transmission = field(entry, "transmission", "species material");
        if (!transmission.is_array() || transmission.size() != 3) fail("NG-E-VEG-FORMAT", "material transmission needs 3 values");
        for (int channel = 0; channel < 3; ++channel) material.transmission[channel] = transmission[channel].get<float>();
        // The material contract the renderer relies on: bark is opaque and thick,
        // foliage is masked, double-sided and thin. A cook that swaps these produces a
        // forest of solid leaves or a see-through trunk.
        if (material.kind == MaterialKind::Bark && material.alphaMasked) fail("NG-E-VEG-FORMAT", "bark must be opaque");
        if (material.kind != MaterialKind::Bark && !material.alphaMasked) fail("NG-E-VEG-FORMAT", "foliage must be alpha masked");
        if (material.kind != MaterialKind::Bark && !material.doubleSided) fail("NG-E-VEG-FORMAT", "foliage must be double sided");
        species.materials.push_back(std::move(material));
    }

    species.primitives = copyRecords<Primitive>(package, SectionKind::Primitive, 0);
    if (species.primitives.empty()) fail("NG-E-VEG-FORMAT", "species has no primitives");
    for (const auto& primitive : species.primitives) {
        if (primitive.material >= species.materials.size()) fail("NG-E-VEG-FORMAT", "primitive references an unknown material");
        if (primitive.lod >= species.lod.size()) fail("NG-E-VEG-FORMAT", "primitive references an unknown LOD");
    }

    for (std::uint32_t level = 0; level < species.lod.size(); ++level) {
        Mesh mesh;
        mesh.positions = package.floats(SectionKind::PositionF32x3, level);
        mesh.normals = package.floats(SectionKind::NormalF32x3, level);
        mesh.uvs = package.floats(SectionKind::UvF32x2, level);
        mesh.colors = package.bytes(SectionKind::ColorU8x4, level);
        mesh.windWeights = package.floats(SectionKind::WindVertex, level);
        mesh.indices = package.uints(SectionKind::IndexU32, level);
        const auto vertices = mesh.positions.size() / 3;
        requireSize(mesh.positions.size(), vertices * 3, "positions");
        requireSize(mesh.normals.size(), vertices * 3, "normals");
        requireSize(mesh.uvs.size(), vertices * 2, "uvs");
        requireSize(mesh.colors.size(), vertices * 4, "colors");
        requireSize(mesh.windWeights.size(), vertices, "wind weights");
        requireSize(mesh.indices.size(), species.lod[level].triangles * 3, "indices");
        requireSize(vertices, species.lod[level].vertices, "vertices");
        for (const auto value : mesh.positions) if (!std::isfinite(value)) fail("NG-E-VEG-FORMAT", "species position is not finite");
        for (const auto index : mesh.indices) if (index >= vertices) fail("NG-E-VEG-FORMAT", "species index outside its vertex buffer");
        for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
            if (mesh.indices[index] == mesh.indices[index + 1] || mesh.indices[index + 1] == mesh.indices[index + 2]
                || mesh.indices[index] == mesh.indices[index + 2]) fail("NG-E-VEG-FORMAT", "species has a degenerate triangle");
        }
        for (const auto weight : mesh.windWeights) if (!(weight >= 0 && weight <= 1)) fail("NG-E-VEG-FORMAT", "wind weight outside [0,1]");
        species.meshes.push_back(std::move(mesh));
    }

    if (species.has("impostor")) {
        const auto& impostor = field(meta, "impostor", "species meta");
        species.impostorViews = unsignedField(impostor, "views", 16, "species impostor");
        species.impostorSize = unsignedField(impostor, "size", 512, "species impostor");
        species.impostorAtlas = package.bytes(SectionKind::ImpostorRgba8, 0);
        species.impostorDepth = package.floats(SectionKind::ImpostorDepthF32, 0);
        const auto texels = static_cast<std::size_t>(species.impostorViews) * species.impostorSize * species.impostorSize;
        requireSize(species.impostorAtlas.size(), texels * 4, "impostor atlas");
        requireSize(species.impostorDepth.size(), texels, "impostor depth");
    }
    if (species.has("shadowProxy")) {
        const auto proxy = copyRecords<ShadowProxy>(package, SectionKind::ShadowProxy, 0);
        if (proxy.empty()) fail("NG-E-VEG-FORMAT", "shadowProxy is declared but absent");
        species.shadowProxy = proxy.front();
        if (!(species.shadowProxy.extent[1] > 0)) fail("NG-E-VEG-FORMAT", "shadow proxy has no vertical extent");
    }
    if (species.has("collision")) {
        species.collision = copyRecords<CollisionCylinder>(package, SectionKind::Collision, 0);
        if (species.collision.empty()) fail("NG-E-VEG-FORMAT", "collision is declared but absent");
        for (const auto& cylinder : species.collision) if (!(cylinder.radius > 0)) fail("NG-E-VEG-FORMAT", "collision radius must be positive");
    }
    if (species.has("mesh") == false) fail("NG-E-VEG-FORMAT", "species declares no mesh representation");
    if (meta.contains("textures") && !meta.at("textures").is_null()) {
        species.textures = meta.at("textures");
        // Validate the shape now: a renderer that trusts a role without a digest would bind
        // whatever file happens to be on disk.
        for (const auto group : {"bark", "leaf"}) {
            if (!species.textures.contains(group) || species.textures.at(group).is_null()) continue;
            const auto& entry = species.textures.at(group);
            if (!entry.contains("roles") || !entry.at("roles").is_object()) fail("NG-E-VEG-FORMAT", std::string("species.textures.") + group + " has no roles");
            for (const auto& role : entry.at("roles").items()) {
                if (!role.value().is_object() || !role.value().contains("digest") || !role.value().at("digest").is_string()) {
                    fail("NG-E-VEG-FORMAT", std::string("species.textures.") + group + "." + role.key() + " has no digest");
                }
                // Slot names are the shader's inputs, not free text. `albedo` was the old name for
                // `baseColor`; accepting both would let two spellings of one slot drift apart
                // again, so an unknown slot is a hard error that names the allowed set.
                const bool known = role.key() == "baseColor" || role.key() == "normal"
                    || role.key() == "orm" || role.key() == "height"
                    || role.key() == "thickness" || role.key() == "opacity";
                if (!known) {
                    fail("NG-E-VEG-FORMAT", std::string("species.textures.") + group + "." + role.key()
                        + " is not a texture slot (baseColor, normal, orm, height, thickness, opacity)");
                }
            }
            if (!entry.at("roles").contains("baseColor")) {
                fail("NG-E-VEG-FORMAT", std::string("species.textures.") + group + " has no baseColor");
            }
        }
    }
    return species;
}

Species loadSpecies(const std::string& file, const std::string& expectedDigest) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) fail("NG-E-VEG-FORMAT", "species cannot be opened: " + file);
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parseSpecies(bytes, expectedDigest);
}

DecodedInstance decodeInstance(const PackedInstance& packed, const PageRecord& page) {
    const auto spanX = page.maxX - page.minX, spanZ = page.maxZ - page.minZ;
    DecodedInstance decoded;
    decoded.position = {
        page.minX + (packed.qx / 65535.0f) * spanX,
        page.baseY + (packed.qy / 65535.0f) * page.heightRange,
        page.minZ + (packed.qz / 65535.0f) * spanZ,
    };
    decoded.yaw = (packed.yaw / 65536.0f) * 6.28318530718f;
    decoded.tiltX = (packed.tiltX / 32767.0f) * 1.57079632679f;
    decoded.tiltZ = (packed.tiltZ / 32767.0f) * 1.57079632679f;
    decoded.scale = std::pow(2.0f, (packed.scale / 255.0f) * 8.0f - 4.0f);
    decoded.species = packed.species;
    decoded.stableId = packed.stableId;
    decoded.lodBias = packed.lodBias;
    decoded.flags = packed.flags;
    decoded.palette = packed.palette;
    decoded.windPhase = packed.windPhase;
    return decoded;
}

Region parseRegion(const std::string& bytes, const std::string& expectedDigest) {
    if (!expectedDigest.empty() && engine::assets::sha256(bytes) != expectedDigest) {
        fail("NG-E-VEG-HASH", "region bytes do not match the expected digest");
    }
    const auto package = engine::assets::readPackage(bytes);
    if (package.schema != "nativegpu.vegetation-region-cook/1") fail("NG-E-VEG-FORMAT", "unsupported region schema " + package.schema);
    const auto& meta = package.meta;
    Region region;
    region.schema = package.schema;
    region.id = textField(meta, "id", "region meta");
    region.seed = unsignedField(meta, "seed", 0xFFFFFFFFu, "region meta");
    const auto& bounds = field(meta, "bounds", "region meta");
    region.boundsMinX = bounds.at("minX").get<float>();
    region.boundsMinZ = bounds.at("minZ").get<float>();
    region.boundsMaxX = bounds.at("maxX").get<float>();
    region.boundsMaxZ = bounds.at("maxZ").get<float>();
    if (!(region.boundsMaxX > region.boundsMinX) || !(region.boundsMaxZ > region.boundsMinZ)) fail("NG-E-VEG-FORMAT", "region bounds are empty");
    region.cellSizeMetres = floatField(meta, "cellSizeMetres", "region meta");
    region.pageCellMetres = floatField(meta, "pageCellMetres", "region meta");
    region.pageSize = unsignedField(meta, "pageSize", 65536, "region meta");
    const auto& surface = field(meta, "surface", "region meta");
    region.surfaceKind = textField(surface, "kind", "region surface");
    region.surfaceVersion = textField(surface, "version", "region surface");
    region.species = readSpeciesRefs(meta);
    region.quantization = meta.value("quantization", nlohmann::json::object());
    region.pages = readPages(package);
    region.spatial = readSpatial(package, region.pages);
    region.instances = readInstances(package, region.pages.back().firstInstance + region.pages.back().instanceCount);
    validateInstances(region.instances, region.species.size());
    region.worldInstances = region.instances.size();
    const auto& representations = meta.value("representations", nlohmann::json::array());
    for (const auto& entry : representations) if (entry.is_string() && entry.get<std::string>() == "aggregate") region.hasAggregateRepresentation = true;
    // The format version does not carry aggregate nodes; a manifest that claims them
    // would make the runtime expect a section that cannot exist.
    if (region.hasAggregateRepresentation) fail("NG-E-VEG-FORMAT", "aggregate representation is not supported by this format version");
    return region;
}

Region loadRegion(const std::string& file, const std::string& expectedDigest) {
    const auto package = engine::assets::loadPackage(file);
    (void)package;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) fail("NG-E-VEG-FORMAT", "region cannot be opened: " + file);
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parseRegion(bytes, expectedDigest);
}

RegionFile::RegionFile(const std::string& file) : file_(file) {
    const auto& meta = file_.header().meta;
    if (file_.header().schema != "nativegpu.vegetation-region-cook/1") fail("NG-E-VEG-FORMAT", "unsupported region schema " + file_.header().schema);
    id_ = textField(meta, "id", "region meta");
    species_ = readSpeciesRefs(meta);
    const auto pageBytes = file_.readSection(SectionKind::Page, 0);
    pages_.resize(pageBytes.size() / sizeof(PageRecord));
    std::memcpy(pages_.data(), pageBytes.data(), pageBytes.size());
    std::uint64_t expected = 0;
    for (std::size_t index = 0; index < pages_.size(); ++index) {
        if (pages_[index].firstInstance != expected || pages_[index].instanceCount == 0 || pages_[index].pageIndex != index) {
            fail("NG-E-VEG-PAGE", "region page table is inconsistent");
        }
        expected += pages_[index].instanceCount;
    }
    const auto spatialBytes = file_.readSection(SectionKind::Spatial, 0);
    spatial_.resize(spatialBytes.size() / sizeof(SpatialCell));
    std::memcpy(spatial_.data(), spatialBytes.data(), spatialBytes.size());
    worldInstances_ = expected;
}

const PageRecord& RegionFile::page(std::uint32_t index) const {
    if (index >= pages_.size()) fail("NG-E-VEG-PAGE", "page index outside the region");
    return pages_[index];
}

std::vector<PackedInstance> RegionFile::readPage(std::uint32_t index) const {
    const auto& record = page(index);
    // Only this page's byte range is read. A 30M-instance region is 960 MB of records
    // and a page is at most 65536 x 32 B = 2 MB, so residency follows the working set.
    const auto offset = static_cast<std::uint64_t>(record.firstInstance) * sizeof(PackedInstance);
    const auto bytes = file_.readSectionRange(SectionKind::InstancePacked, 0, offset, record.instanceCount * static_cast<std::uint32_t>(sizeof(PackedInstance)));
    std::vector<PackedInstance> result(record.instanceCount);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    for (const auto& instance : result) {
        if (instance.species >= species_.size()) fail("NG-E-VEG-SPECIES", "page instance references an unknown species");
    }
    return result;
}

} // namespace engine::vegetation
