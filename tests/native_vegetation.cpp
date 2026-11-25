// Native vegetation evidence: package parsing, page streaming, culling and LOD.
//
// The test reads the packages produced by tools/vegetation.mjs, so it exercises the
// exact bytes the runtime loads rather than a hand-written fixture that could drift.

#include "../engine/native/vegetation/asset.hpp"
#include "../engine/native/vegetation/lod.hpp"
#include "../engine/native/vegetation/world.hpp"
#include "../engine/native/assets/package.hpp"
#include "../renderer/hzb.hpp"
#include "../third_party/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace engine::vegetation;
namespace hzb = nativegpu;

namespace {
unsigned checks = 0, groups = 0;

void require(bool ok, const std::string& why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}

std::string readText(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + file);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

struct Library {
    nlohmann::json json;
    std::string directory;
};

Library loadLibrary(const std::string& directory) {
    Library library;
    library.directory = directory;
    library.json = nlohmann::json::parse(readText(directory + "/library.json"));
    return library;
}

std::string speciesPath(const Library& library, const std::string& id) {
    for (const auto& entry : library.json.at("species")) if (entry.at("id").get<std::string>() == id) return library.directory + "/" + entry.at("file").get<std::string>();
    throw std::runtime_error("species " + id + " is not in the library");
}

std::string speciesDigest(const Library& library, const std::string& id) {
    for (const auto& entry : library.json.at("species")) if (entry.at("id").get<std::string>() == id) return entry.at("hash").get<std::string>();
    throw std::runtime_error("species " + id + " is not in the library");
}

std::string regionPath(const Library& library, const std::string& id) {
    for (const auto& entry : library.json.at("regions")) if (entry.at("id").get<std::string>() == id) return library.directory + "/" + entry.at("file").get<std::string>();
    throw std::runtime_error("region " + id + " is not in the library");
}

// Identity view-projection: a unit frustum around the origin looking down +Z with a
// 0..1 depth range. Enough to test the frustum extraction and the HZB rectangle maths
// without depending on the renderer's camera.
void identityProjection(float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

// Orthographic projection covering +/-200 m in X/Y and -200..200 m in Z, depth 0..1.
// The identity matrix would only cover a 2 m cube, which is not a useful "everything is
// visible" camera for a 160 m region.
void wideProjection(float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = 1.0f / 200.0f;
    out[5] = 1.0f / 200.0f;
    out[10] = 1.0f / 400.0f;
    out[14] = 0.5f;
    out[15] = 1.0f;
}

void speciesRoundTrip(const Library& library) {
    const auto path = speciesPath(library, "oak_broadleaf");
    const auto digest = speciesDigest(library, "oak_broadleaf");
    const auto bytes = readText(path);
    const auto species = parseSpecies(bytes, digest);
    require(species.id == "oak_broadleaf", "species id was not preserved");
    require(species.kind == SpeciesKind::Tree, "species kind is not tree");
    require(species.lod.size() == 3, "expected three LOD levels");
    require(species.meshes.size() == species.lod.size(), "one mesh per LOD is required");
    require(species.has("mesh") && species.has("impostor") && species.has("shadowProxy") && species.has("collision"),
        "species must declare every representation it baked");
    for (std::size_t level = 0; level < species.meshes.size(); ++level) {
        const auto& mesh = species.meshes[level];
        require(mesh.triangleCount() == species.lod[level].triangles, "triangle count disagrees with the LOD table");
        require(mesh.vertexCount() == species.lod[level].vertices, "vertex count disagrees with the LOD table");
        for (const auto weight : mesh.windWeights) require(weight >= 0 && weight <= 1, "wind weight outside [0,1]");
        // Every LOD must keep its geometry inside the species bounds: a culler that
        // trusts the bounds must not miss a branch that pokes out.
        for (std::size_t vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
            for (int axis = 0; axis < 3; ++axis) {
                const auto value = mesh.positions[vertex * 3 + axis];
                require(value >= species.boundsMin[axis] - 0.01f && value <= species.boundsMax[axis] + 0.01f,
                    "vertex outside the species bounds");
            }
        }
    }
    require(species.lod[0].worldErrorMetres == 0, "LOD0 error must be zero");
    for (std::size_t level = 1; level < species.lod.size(); ++level) {
        require(species.lod[level].worldErrorMetres > 0, "a reduced LOD must have a measured error");
        require(species.lod[level].worldErrorMetres >= species.lod[level - 1].worldErrorMetres * 0.98f, "LOD error must not shrink");
    }
    require(species.materials.size() >= 2, "oak must have bark and leaf materials");
    bool sawLeaf = false;
    for (const auto& material : species.materials) {
        if (material.kind == MaterialKind::Leaf) {
            sawLeaf = true;
            require(material.alphaMasked && material.doubleSided, "leaf material must be masked and double sided");
            require(material.thicknessMetres > 0 && material.thicknessMetres < 0.01f, "leaf thickness must be thin");
        }
        if (material.kind == MaterialKind::Bark) require(!material.alphaMasked, "bark must be opaque");
    }
    require(sawLeaf, "oak has no leaf material");
    const auto texels = static_cast<std::size_t>(species.impostorViews) * species.impostorSize * species.impostorSize;
    require(species.impostorAtlas.size() == texels * 4, "impostor atlas size disagrees with views x size");
    std::size_t covered = 0;
    for (std::size_t texel = 0; texel < texels; ++texel) if (species.impostorAtlas[texel * 4 + 3] > 0) ++covered;
    require(covered > texels / 50, "impostor atlas is almost empty");
    require(covered < texels, "impostor atlas has no background");
    ++groups;
}

void speciesAuthentication(const Library& library) {
    const auto path = speciesPath(library, "pine_conifer");
    const auto digest = speciesDigest(library, "pine_conifer");
    auto bytes = readText(path);
    // One flipped byte in the index section must fail authentication before any data is
    // interpreted.
    bytes[bytes.size() / 2] ^= 0x5A;
    bool rejected = false;
    try { (void)parseSpecies(bytes, digest); } catch (const Error& error) { rejected = error.code == "NG-E-VEG-HASH"; }
    require(rejected, "a tampered species package was accepted");

    // A truncated package must fail structurally even when authentication is skipped.
    bool truncated = false;
    try { (void)parseSpecies(bytes.substr(0, bytes.size() - 64)); } catch (const engine::assets::Error&) { truncated = true; }
    require(truncated, "a truncated species package was accepted");
    ++groups;
}

void regionPages(const Library& library) {
    const auto region = loadRegion(regionPath(library, "meadow_ridge"));
    require(region.id == "meadow_ridge", "region id was not preserved");
    require(region.worldInstances > 1000, "region has too few instances to be meaningful");
    require(region.instances.size() == region.worldInstances, "instance count disagrees with the page table");
    require(region.pages.size() >= 4, "region must be split into several pages");
    require(region.spatial.size() >= 1, "region has no spatial index");
    require(region.species.size() == 4, "region must reference four species");
    require(!region.hasAggregateRepresentation, "this format version does not declare aggregate nodes");

    std::uint64_t total = 0;
    std::uint32_t maxCount = 0;
    for (const auto& page : region.pages) {
        total += page.instanceCount;
        maxCount = std::max(maxCount, page.instanceCount);
    }
    require(total == region.worldInstances, "page counts do not sum to the world instance count");
    require(maxCount <= region.pageSize, "a page exceeds the declared page size");

    // Decode every instance and check it lands inside its page. A quantized record that
    // decodes outside its own bounds means the page frame and the record disagree.
    std::size_t pageCursor = 0;
    for (std::size_t index = 0; index < region.instances.size(); ++index) {
        while (pageCursor + 1 < region.pages.size() && region.pages[pageCursor + 1].firstInstance <= index) ++pageCursor;
        const auto& page = region.pages[pageCursor];
        const auto decoded = decodeInstance(region.instances[index], page);
        require(std::isfinite(decoded.position.x) && std::isfinite(decoded.position.y) && std::isfinite(decoded.position.z),
            "decoded instance is not finite");
        require(decoded.position.x >= page.minX - 1e-3f && decoded.position.x <= page.maxX + 1e-3f, "decoded X outside its page");
        require(decoded.position.z >= page.minZ - 1e-3f && decoded.position.z <= page.maxZ + 1e-3f, "decoded Z outside its page");
        require(decoded.position.y >= page.baseY - 1e-3f && decoded.position.y <= page.baseY + page.heightRange + 1e-3f, "decoded Y outside its page");
        require(decoded.scale >= 0.05f && decoded.scale <= 17.0f, "decoded scale outside the quantization domain");
        require(decoded.species < region.species.size(), "decoded species index outside the region table");
    }
    const auto positionError = region.quantization.at("positionErrorMetres").get<float>();
    require(positionError < 0.01f, "position quantization error is above a centimetre");
    ++groups;
}

void pagedRegion(const Library& library) {
    const auto path = regionPath(library, "meadow_ridge");
    RegionFile file(path);
    require(file.pageCount() > 0, "paged reader found no pages");
    require(file.worldInstances() == loadRegion(path).worldInstances, "paged and whole-file world counts disagree");
    const auto whole = loadRegion(path);
    for (std::uint32_t index = 0; index < file.pageCount(); ++index) {
        const auto page = file.readPage(index);
        const auto& record = file.page(index);
        require(page.size() == record.instanceCount, "paged read returned the wrong instance count");
        // The paged path must produce byte-identical records to the whole-file path.
        require(std::memcmp(page.data(), whole.instances.data() + record.firstInstance, page.size() * sizeof(PackedInstance)) == 0,
            "paged read differs from the whole-file read");
    }
    // Reading one page must not require the instance section to be resident; the reader
    // seeks, so a second read of the last page is still exact.
    const auto last = file.readPage(file.pageCount() - 1);
    require(!last.empty(), "last page is empty");
    ++groups;
}

void streaming(const Library& library) {
    auto region = std::make_shared<RegionFile>(regionPath(library, "meadow_ridge"));
    World world;
    world.setLoader([&region](std::uint32_t, std::uint32_t page) { return region->readPage(page); });
    StreamBudget budget;
    budget.readBytesPerFrame = 1u << 20;
    budget.maxPagesPerFrame = 2;
    world.setBudget(budget);
    world.addRegion(region);
    require(world.stats().worldInstances == region->worldInstances(), "world instance count is not the region count");
    require(world.stats().residentInstances == 0, "a fresh world must not report resident instances");

    std::uint64_t frames = 0;
    while (world.stats().residentPages < region->pageCount() && frames < 512) {
        world.beginFrame(frames++);
        world.requestAround({0, 0, 0}, {0, 0, 0}, 200.0f);
        world.tick();
    }
    require(world.stats().residentPages == region->pageCount(), "streaming did not reach every page");
    require(world.stats().residentInstances == region->worldInstances(), "resident instances do not match the world index");
    require(world.stats().peakBytesIncludingInflight <= budget.hardResidentBytes, "residency exceeded the hard cap");

    // Page faults: a fresh world that is culled before streaming reports the missing
    // representation instead of pretending the page is visible.
    World cold;
    cold.addRegion(region);
    cold.beginFrame(0);
    float vp[16]; identityProjection(vp);
    const auto frustum = hzb::Frustum::fromViewProjection(vp);
    const auto visible = cold.cull(frustum, nullptr, {0, 0, 0}, 200.0f, vp, 128, 128);
    require(!visible.empty(), "coarse culling found no pages");
    require(cold.stats().pageFaults > 0, "a cold world reported no page faults");
    require(cold.stats().visibleInstances == 0, "a cold world reported visible instances");
    for (const auto& page : visible) require(!page.resident, "a cold world reported a resident page");

    // A loader that returns the wrong count must be recorded as a failure, not a
    // silently short page.
    World broken;
    broken.setLoader([](std::uint32_t, std::uint32_t) { return std::vector<PackedInstance>{}; });
    broken.addRegion(region);
    // Requested -> Reading -> Failed: the state machine needs a tick per transition, and
    // a loader that returns the wrong count must end in Failed, never in Resident.
    for (int frame = 0; frame < 4; ++frame) {
        broken.beginFrame(frame);
        broken.requestAround({0, 0, 0}, {0, 0, 0}, 200.0f);
        broken.tick();
    }
    require(broken.stats().failedPages > 0, "a short page was not recorded as a failure");
    require(broken.stats().residentPages == 0, "a short page became resident");
    ++groups;
}

void cullingAndLod(const Library& library) {
    auto region = std::make_shared<RegionFile>(regionPath(library, "meadow_ridge"));
    World world;
    world.setLoader([&region](std::uint32_t, std::uint32_t page) { return region->readPage(page); });
    StreamBudget budget;
    budget.readBytesPerFrame = 64u << 20;
    budget.maxPagesPerFrame = 64;
    world.setBudget(budget);
    world.addRegion(region);
    world.beginFrame(1);
    world.requestAround({0, 0, 0}, {0, 0, 0}, 200.0f);
    for (int frame = 0; frame < 8 && world.stats().residentPages < region->pageCount(); ++frame) world.tick();
    require(world.stats().residentPages == region->pageCount(), "culling test needs every page resident");

    float vp[16]; wideProjection(vp);
    const auto frustum = hzb::Frustum::fromViewProjection(vp);
    const auto all = world.cull(frustum, nullptr, {0, 0, 0}, 1e6f, vp, 256, 256);
    require(world.stats().candidateInstances == region->worldInstances(), "an unbounded cull must consider every instance");
    require(world.stats().visibleInstances > 0, "an unbounded cull found nothing visible");
    require(world.stats().visibleInstances <= world.stats().candidateInstances, "visible exceeds candidate");

    // An empty depth pyramid (everything at the far plane) must not reject anything:
    // that is the conservative direction.
    std::vector<float> far(256 * 256, 1.0f);
    hzb::DepthPyramid pyramid;
    pyramid.build(far.data(), 256, 256);
    const auto noneOccluded = world.cull(frustum, &pyramid, {0, 0, 0}, 1e6f, vp, 256, 256);
    require(world.stats().visibleInstances == region->worldInstances(), "a far-plane HZB rejected geometry");
    require(!noneOccluded.empty(), "HZB cull returned no pages");

    // A depth buffer that is entirely at the near plane hides everything behind it.
    std::vector<float> near(256 * 256, 0.0f);
    hzb::DepthPyramid occluder;
    occluder.build(near.data(), 256, 256);
    world.cull(frustum, &occluder, {0, 0, 0}, 1e6f, vp, 256, 256);
    require(world.stats().visibleInstances == 0, "a near-plane occluder did not hide anything");

    // LOD projection: a 10 m tree at 100 m with a 60-degree vertical FOV and a 1080-pixel
    // viewport projects its 0.2 m error to a measurable pixel count.
    const auto pixels = projectErrorPixels(0.2f, 1.0f, 1080.0f, std::tan(0.5f), 100.0f);
    require(pixels > 0.1f && pixels < 10.0f, "projected error is outside a plausible range");
    require(projectErrorPixels(0.2f, 1.0f, 1080.0f, std::tan(0.5f), 10.0f) > pixels * 9.0f, "projected error does not scale with 1/depth");
    const float thresholds[3] = {0.4f, 2.5f, 9.0f};
    require(selectLod(thresholds, 3, 0.1f, -1).level == 0, "a sub-pixel error must select LOD0");
    require(selectLod(thresholds, 3, 2.0f, -1).level == 1, "a 2-pixel error must select LOD1");
    require(selectLod(thresholds, 3, 3.0f, -1).level == 2, "a 3-pixel error exceeds LOD1's 2.5 px threshold");
    require(selectLod(thresholds, 3, 40.0f, -1).level == 2, "a 40-pixel error must select LOD2");
    // Hysteresis: at the boundary the current level wins, which removes flicker.
    require(selectLod(thresholds, 3, 2.6f, 1).level == 1, "hysteresis did not keep the current level");
    require(selectLod(thresholds, 3, 2.6f, 0).level == 2, "without history the boundary must promote");
    // Density LOD is a nested subset, so reducing grass never re-shuffles it.
    const float fractions[3] = {1.0f, 0.5f, 0.25f};
    std::uint32_t keptFine = 0, keptCoarse = 0;
    for (std::uint32_t id = 0; id < 10000; ++id) {
        if (densityKeeps(id, 1, fractions, 3)) ++keptFine;
        if (densityKeeps(id, 2, fractions, 3)) ++keptCoarse;
    }
    require(keptCoarse < keptFine && keptFine < 10000, "density LOD is not a nested reduction");
    require(keptFine > 4000 && keptFine < 6000, "density LOD does not keep approximately half");
    ++groups;
}

void overrides(const Library& library) {
    auto region = std::make_shared<RegionFile>(regionPath(library, "meadow_ridge"));
    World world;
    world.setLoader([&region](std::uint32_t, std::uint32_t page) { return region->readPage(page); });
    StreamBudget budget;
    budget.readBytesPerFrame = 64u << 20;
    budget.maxPagesPerFrame = 64;
    world.setBudget(budget);
    world.addRegion(region);
    world.beginFrame(1);
    world.requestAround({0, 0, 0}, {0, 0, 0}, 200.0f);
    for (int frame = 0; frame < 8 && world.stats().residentPages < region->pageCount(); ++frame) world.tick();
    float vp[16]; wideProjection(vp);
    const auto frustum = hzb::Frustum::fromViewProjection(vp);
    world.cull(frustum, nullptr, {0, 0, 0}, 1e6f, vp, 256, 256);
    const auto before = world.stats().visibleInstances;
    // Harvest every instance in the first page through the sparse overlay. The world
    // index is untouched; only the override layer changes.
    const auto page = region->readPage(0);
    for (const auto& instance : page) world.setOverride(instance.stableId, InstanceOverride{true, true, 1.0f});
    world.cull(frustum, nullptr, {0, 0, 0}, 1e6f, vp, 256, 256);
    require(world.stats().visibleInstances == before - page.size(), "harvesting did not remove exactly the page instances");
    require(world.stats().worldInstances == region->worldInstances(), "an overlay must not change the world index");
    ++groups;
}
}

int main() {
    try {
        const auto library = loadLibrary("build/vegetation/cooked");
        speciesRoundTrip(library);
        speciesAuthentication(library);
        regionPages(library);
        pagedRegion(library);
        streaming(library);
        cullingAndLod(library);
        overrides(library);
        std::cout << nlohmann::json{{"schema", "nativegpu.vegetation-evidence/1"}, {"ok", true}, {"groups", groups}, {"checks", checks}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vegetation tests failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
