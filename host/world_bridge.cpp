#include "world_bridge.hpp"

#include "../engine/native/assets/sha256.hpp"
#include "../third_party/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace host {
namespace {

using Json = nlohmann::json;

std::string readText(const std::filesystem::path& path, bool& ok) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { ok = false; return {}; }
    ok = true;
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

float numberOr(const Json& value, const char* key, float fallback) {
    if (!value.contains(key) || !value.at(key).is_number()) return fallback;
    const double parsed = value.at(key).get<double>();
    return std::isfinite(parsed) ? static_cast<float>(parsed) : fallback;
}

} // namespace

struct WorldBridge::Impl {
    std::unique_ptr<engine::world::WorldRuntime> runtime;
    std::unique_ptr<engine::world::DirectoryChunkSource> source;
    std::unique_ptr<nativegpu::WorldDraw> draw;
    engine::world::Bounds2 bounds{};
    std::string worldId;
    std::string manifestHash;
    std::string lastError;
    bool open = false;
    bool componentsUploaded = false;
    std::vector<nativegpu::WorldComponentDesc> pendingComponents;
    float focus[3] = {0.0f, 0.0f, 0.0f};
    float seconds = 0.0f;
};

WorldBridge::WorldBridge() : impl_(std::make_unique<Impl>()) {}
WorldBridge::~WorldBridge() = default;

bool WorldBridge::open(ID3D12Device* device, const std::filesystem::path& cookedDirectory,
                       DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, std::string& error) {
    auto& state = *impl_;
    state.open = false;

    bool ok = false;
    const std::string text = readText(cookedDirectory / "manifest.json", ok);
    if (!ok) {
        error = "NG-E-WORLD: cannot read " + (cookedDirectory / "manifest.json").string();
        return false;
    }
    Json manifest;
    try {
        manifest = Json::parse(text);
    } catch (const std::exception& exception) {
        error = std::string("NG-E-WORLD: manifest is not valid JSON: ") + exception.what();
        return false;
    }
    if (!manifest.is_object() || manifest.value("schema", std::string()) != "nativegpu.world-manifest/1") {
        error = "NG-E-WORLD: manifest schema is not nativegpu.world-manifest/1";
        return false;
    }
    state.worldId = manifest.value("worldId", std::string());
    state.manifestHash = manifest.value("manifestHash", std::string());
    if (state.manifestHash.size() != 64) {
        error = "NG-E-WORLD: manifest hash is missing";
        return false;
    }

    const float chunkSize = numberOr(manifest, "chunkSizeMetres", 128.0f);
    const float sampleStep = numberOr(manifest, "sampleStepMetres", 1.0f);
    float extent[2] = {256.0f, 256.0f};
    float origin[3] = {-128.0f, 0.0f, -128.0f};
    if (manifest.contains("extentMetres") && manifest.at("extentMetres").is_array()
        && manifest.at("extentMetres").size() == 2) {
        extent[0] = manifest.at("extentMetres")[0].get<float>();
        extent[1] = manifest.at("extentMetres")[1].get<float>();
    }
    if (manifest.contains("originMetres") && manifest.at("originMetres").is_array()
        && manifest.at("originMetres").size() == 3) {
        for (int i = 0; i < 3; ++i) origin[i] = manifest.at("originMetres")[i].get<float>();
    }
    state.bounds = {origin[0], origin[2], origin[0] + extent[0], origin[2] + extent[1]};

    engine::world::RuntimeConfig config;
    config.chunkSizeMetres = chunkSize;
    config.sampleStepMetres = sampleStep;
    // Bands are in metres and scale with the chunk size so a 128 m chunk world behaves the
    // same as a 512 m chunk world at the same view distance.
    config.nearDistanceMetres = chunkSize * 2.5f;
    config.midDistanceMetres = chunkSize * 7.0f;
    config.farDistanceMetres = chunkSize * 14.0f;
    config.maxResidentChunks = 256;
    config.memoryBudgetBytes = 768ull * 1024ull * 1024ull;

    state.runtime = std::make_unique<engine::world::WorldRuntime>();
    if (!state.runtime->configure(config, error)) return false;
    state.runtime->setManifestBounds(state.bounds);
    state.source = std::make_unique<engine::world::DirectoryChunkSource>(cookedDirectory.string());
    state.runtime->setSource(state.source.get());

    state.draw = std::make_unique<nativegpu::WorldDraw>(device, colorFormat, depthFormat);

    std::vector<nativegpu::WorldComponentDesc> components;
    if (manifest.contains("components") && manifest.at("components").is_array()) {
        for (const auto& entry : manifest.at("components")) {
            if (!entry.is_object()) continue;
            nativegpu::WorldComponentDesc desc;
            desc.id = entry.value("id", std::string());
            desc.shape = entry.value("shape", std::string("box"));
            if (entry.contains("sizeMetres") && entry.at("sizeMetres").is_array()
                && entry.at("sizeMetres").size() == 3) {
                for (int i = 0; i < 3; ++i) desc.sizeMetres[i] = entry.at("sizeMetres")[i].get<float>();
            }
            if (entry.contains("albedo") && entry.at("albedo").is_array() && entry.at("albedo").size() == 3) {
                for (int i = 0; i < 3; ++i) desc.albedo[i] = entry.at("albedo")[i].get<float>();
            }
            desc.roughness = numberOr(entry, "roughness", 0.85f);
            components.push_back(std::move(desc));
        }
    }

    // The component meshes are uploaded on the first frame, when there is a command list.
    // open() deliberately does not take one: the caller may want to validate a world before
    // it has started recording.
    state.open = true;
    state.pendingComponents = std::move(components);
    return true;
}

bool WorldBridge::frame(ID3D12GraphicsCommandList* list, const float viewProjection[16],
                        const nativegpu::WorldDrawLighting& lighting, std::string& error) {
    auto& state = *impl_;
    if (!state.open || !state.runtime || !state.draw) return true;

    if (!state.componentsUploaded) {
        if (!state.draw->setComponents(list, state.pendingComponents, error)) {
            state.lastError = error;
            return false;
        }
        state.componentsUploaded = true;
    }

    // Stream from the camera. A frame without a pump is a frame that never loads anything.
    state.runtime->pump(lighting.camera[0], lighting.camera[2], 6);

    // Upload anything the runtime has made CPU-ready since the last frame.
    auto uploads = state.runtime->beginUpload(6);
    std::vector<engine::world::ChunkCoord> uploaded;
    uploaded.reserve(uploads.size());
    for (const auto& chunk : uploads) {
        if (!chunk) continue;
        if (!state.draw->uploadChunk(list, *chunk, error)) {
            state.lastError = error;
            return false;
        }
        uploaded.push_back(chunk->coord);
    }
    state.runtime->completeUpload(uploaded);

    if (std::getenv("NG_WORLD_DEBUG")) {
        static int printed = 0;
        if (printed < 2) {
            printed += 1;
            std::fprintf(stderr, "[world] camera=(%.1f %.1f %.1f) vp0=%.3f vp5=%.3f vp10=%.3f vp14=%.1f chunks=%zu\n",
                         lighting.camera[0], lighting.camera[1], lighting.camera[2],
                         viewProjection[0], viewProjection[5], viewProjection[10], viewProjection[14],
                         state.draw->stats().chunksResident);
            for (const auto& entry : state.runtime->chunks()) {
                if (!entry.data) continue;
                std::fprintf(stderr, "[world] chunk %d,%d verts=%u h=[%.1f %.1f] inst=%zu bounds=[%.0f %.0f %.0f %.0f]\n",
                             entry.coord.x, entry.coord.z, entry.data->vertexCount,
                             entry.data->minHeight, entry.data->maxHeight, entry.data->instances.size(),
                             entry.data->bounds.minX, entry.data->bounds.minZ,
                             entry.data->bounds.maxX, entry.data->bounds.maxZ);
                break;
            }
        }
    }
    const float camera[3] = {lighting.camera[0], lighting.camera[1], lighting.camera[2]};
    if (!state.draw->draw(list, viewProjection, lighting, 0.0f, error)) {
        state.lastError = error;
        return false;
    }
    (void)camera;
    return true;
}

void WorldBridge::releaseRetired() {
    auto& state = *impl_;
    if (!state.runtime || !state.draw) return;
    const auto retired = state.runtime->takeRetired();
    for (const auto& coord : retired) state.draw->releaseChunk(coord.x, coord.z);
    state.runtime->releaseRetired(retired);
}

bool WorldBridge::isOpen() const { return impl_->open; }

void WorldBridge::setFocus(float x, float y, float z) {
    impl_->focus[0] = x;
    impl_->focus[1] = y;
    impl_->focus[2] = z;
    if (impl_->runtime) impl_->runtime->pump(x, z, 6);
}

bool WorldBridge::heightAt(float x, float z, float& height) const {
    if (!impl_->runtime) return false;
    return impl_->runtime->queryHeight(x, z, height);
}

WorldBridgeStats WorldBridge::stats() const {
    WorldBridgeStats out;
    const auto& state = *impl_;
    out.open = state.open;
    out.worldId = state.worldId;
    out.manifestHash = state.manifestHash;
    out.lastError = state.lastError;
    if (state.runtime) {
        const auto runtime = state.runtime->stats();
        out.chunksResident = runtime.chunksResident;
        out.chunksCpuReady = runtime.chunksCpuReady;
        out.instancesResident = runtime.instancesResident;
        out.buildingsResident = runtime.buildingsResident;
        out.bytesResident = runtime.bytesResident;
        out.epoch = runtime.epoch;
        out.loadErrors = runtime.loadErrors;
        out.lastPumpMillis = runtime.lastPumpMillis;
        if (out.lastError.empty()) out.lastError = runtime.lastError;
    }
    if (state.draw) {
        const auto draw = state.draw->stats();
        out.chunksDrawn = draw.chunksDrawn;
        out.instancesDrawn = draw.instancesDrawn;
        out.triangles = draw.triangles;
    }
    return out;
}

} // namespace host
