#pragma once

// World bridge: the piece that connects a cooked world directory to the viewport host.
//
// It owns exactly three things and nothing else:
//   * a WorldRuntime, which decides which chunks should be resident;
//   * a WorldDraw, which owns the GPU buffers;
//   * the handshake between them (beginUpload -> upload -> completeUpload, and
//     takeRetired -> releaseRetired after the frame fence).
//
// The bridge is deliberately independent of the scene preview path. A world is not a scene:
// it has no entities, no gameplay document and no revision, and pretending otherwise would
// have meant threading world state through every scene call.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "../engine/native/world/world_runtime.hpp"
#include "../renderer/world_draw.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace host {

struct WorldBridgeStats {
    bool open = false;
    std::string worldId;
    std::string manifestHash;
    std::uint64_t chunksResident = 0;
    std::uint64_t chunksCpuReady = 0;
    std::uint64_t chunksDrawn = 0;
    std::uint64_t instancesResident = 0;
    std::uint64_t instancesDrawn = 0;
    std::uint64_t buildingsResident = 0;
    std::uint64_t triangles = 0;
    std::uint64_t bytesResident = 0;
    std::uint64_t epoch = 0;
    std::uint64_t loadErrors = 0;
    double lastPumpMillis = 0.0;
    std::string lastError;
};

class WorldBridge {
public:
    WorldBridge();
    ~WorldBridge();
    WorldBridge(const WorldBridge&) = delete;
    WorldBridge& operator=(const WorldBridge&) = delete;

    // Opens a cooked world directory (the one containing manifest.json).
    bool open(ID3D12Device* device, const std::filesystem::path& cookedDirectory,
              DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, std::string& error);

    // Pumps streaming, uploads new chunks and records the draw. Safe to call every frame.
    bool frame(ID3D12GraphicsCommandList* list, const float viewProjection[16],
               const nativegpu::WorldDrawLighting& lighting, std::string& error);

    // Releases chunks the runtime retired. Must be called after the frame fence has drained,
    // or a chunk still referenced by an in-flight command list is freed underneath the GPU.
    void releaseRetired();

    bool isOpen() const;
    WorldBridgeStats stats() const;
    // Terrain height at a world position, or false when the covering chunk is not resident.
    bool heightAt(float x, float z, float& height) const;
    // Moves the streaming focus. The viewport calls this with the orbit target.
    void setFocus(float x, float y, float z);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace host
