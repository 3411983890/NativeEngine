#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

#include "matrix_math.h"

namespace nativegpu {

// Matrices are column-major, with column vectors: clip = viewProjection * model * position.
struct Vertex { float x, y, z, nx, ny, nz, u, v; };
struct DrawItem {
    Mat4 model;
    float color[4];
    // Bounded material texture table index. 0 checker, 1 brick, 2 metal.
    unsigned texture = 0;
    // Bounded material table generation. 0 skips the generation check (compatibility).
    unsigned textureGeneration = 0;
    float roughness = 0.72f;
    float metalness = 0.0f;
};
struct LightDesc {
    float position[3];
    float color[3];
    float intensity = 1.0f;
    float range = 10.0f;
};
struct FrameStats {
    // cpuMs is measured around the whole submit including Present; cpuSubmitMs stops
    // before Present so it is not dominated by the vsync wait.
    double cpuMs = 0.0;
    double cpuSubmitMs = 0.0;
    double gpuMs = 0.0;
    bool gpuValid = false;
    // Time spent inside the DXR shadow dispatch for the most recent completed frame.
    double gpuRtMs = 0.0;
    bool gpuRtValid = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Enables the D3D12 validation layer for the next initialize(). Must be called first.
    void setValidationEnabled(bool enabled);

    bool initialize(HWND window, unsigned width, unsigned height, std::string& error);
    bool resize(unsigned width, unsigned height, std::string& error);
    // `cameraPosition` is the world-space eye used by the DXR shadow pass to orient
    // reconstructed normals; it has no effect on the raster path.
    bool render(const Mat4& viewProjection, const float cameraPosition[3],
                const std::vector<DrawItem>& items, const std::vector<LightDesc>& lights,
                std::string& error);
    void shutdown();

    // Runtime toggle for the DXR static shadow pass. When disabled (or when the pass
    // cannot be built) rendering falls back to the raster path with a full visibility
    // mask, and every report field records that fallback.
    void setRtShadows(bool enabled);
    bool rtShadowsEnabled() const;
    bool rtShadowsImplemented() const;

    // Mesh shader sample path. Falls back to the vertex path when unavailable/disabled.
    void setMeshShaders(bool enabled);
    bool meshShadersEnabled() const;
    bool meshShadersImplemented() const;
    bool meshPathActive() const;

    // 0 final, 1 shadow mask, 2 key light, 3 ambient surface. Diagnostic evidence only.
    void setCompositeDebugView(unsigned view);

    // Screenshots are captured in two steps so the copy happens inside the frame
    // submission: request before render(), capture after render().
    bool requestScreenshot(const std::string& path, std::string& error);
    bool captureScreenshot(const std::string& path, std::string& error);

    // Reliability surface used by the G1 evidence suite.
    void injectDeviceRemovalAtFrame(unsigned frame);
    unsigned injectedRemovalFrame() const;
    static long liveObjectCount();
    static unsigned long long objectsCreatedCount();
    unsigned long long videoMemoryBytes();
    // Compiles a deliberately broken shader to prove the invalid-pipeline diagnostic and
    // that the failure is reported instead of crashing.
    bool selfTestInvalidPipeline(std::string& code, std::string& text);

    std::string capabilitiesJson() const;
    std::string probesJson() const;
    FrameStats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace nativegpu
