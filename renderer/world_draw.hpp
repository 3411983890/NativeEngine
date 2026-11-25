#pragma once

// World rendering: terrain chunks and instanced components.
//
// What this module owns:
//   * one vertex/index buffer per resident chunk (terrain) and per component (procedural
//     stand-in geometry);
//   * one instance buffer per chunk, uploaded once when the chunk becomes resident;
//   * a single shader pair shared by both draws, so terrain and the grass on it cannot
//     disagree about the light.
//
// What it deliberately does not own:
//   * residency. The chunk lifecycle belongs to WorldRuntime; this module is told what to
//     upload and what to release, and it never decides that a chunk should be loaded.
//   * the frame. It records draws into a command list the caller has already set up,
//     including the render target and depth target. It does not change the viewport.
//
// Uploads are all-or-nothing per chunk: a chunk whose terrain buffer succeeded but whose
// instance buffer failed is rolled back, because a chunk that draws its ground without its
// trees looks like a bug rather than a partial load.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "../engine/native/world/world_chunk.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nativegpu {

struct WorldComponentDesc {
    std::string id;
    std::string shape;
    float sizeMetres[3] = {1.0f, 1.0f, 1.0f};
    float albedo[3] = {0.7f, 0.7f, 0.7f};
    float roughness = 0.85f;
};

struct WorldDrawLighting {
    float keyDirection[3] = {0.35f, 0.85f, 0.4f};
    float keyColor[3] = {1.0f, 0.97f, 0.9f};
    float keyIntensity = 1.1f;
    float ambient = 0.32f;
    float fogColor[3] = {0.62f, 0.7f, 0.78f};
    float fogDensity = 0.00035f;
    float camera[3] = {0.0f, 0.0f, 0.0f};
    float seconds = 0.0f;
};

struct WorldDrawStats {
    std::uint64_t frames = 0;
    std::uint64_t chunksResident = 0;
    std::uint64_t chunksDrawn = 0;
    std::uint64_t instancesDrawn = 0;
    std::uint64_t triangles = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t releasedChunks = 0;
    std::uint32_t terrainVertices = 0;
    std::uint32_t components = 0;
};

class WorldDraw {
public:
    WorldDraw(ID3D12Device* device, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
    ~WorldDraw();
    WorldDraw(const WorldDraw&) = delete;
    WorldDraw& operator=(const WorldDraw&) = delete;

    // Builds the procedural meshes for the component palette. Called once per manifest.
    bool setComponents(ID3D12GraphicsCommandList* list, const std::vector<WorldComponentDesc>& components,
                       std::string& error);

    // Uploads one chunk. Re-uploading the same key replaces the previous content, which is
    // what a regeneration of a single chunk needs.
    bool uploadChunk(ID3D12GraphicsCommandList* list, const engine::world::ChunkData& chunk, std::string& error);
    void releaseChunk(std::int32_t cx, std::int32_t cz);

    // Records every resident chunk. `drawDistance` culls whole chunks by their centre.
    bool draw(ID3D12GraphicsCommandList* list, const float viewProjection[16],
              const WorldDrawLighting& lighting, float drawDistance, std::string& error);

    // 0 shaded, 1 albedo, 2 normal, 3 grid.
    void setDebugMode(int mode) { debugMode_ = mode; }
    int debugMode() const { return debugMode_; }

    WorldDrawStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int debugMode_ = 0;
};

} // namespace nativegpu
