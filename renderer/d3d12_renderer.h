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

namespace nativegpu {

// Matrices are column-major, with column vectors: clip = viewProjection * model * position.
struct Vertex { float x, y, z, nx, ny, nz, u, v; };
struct Mat4 { float m[16]; };
struct DrawItem {
    Mat4 model;
    float color[4];
    // Bounded material texture table index. 0 checker, 1 brick, 2 metal.
    unsigned texture = 0;
    float roughness = 0.72f;
    float metalness = 0.0f;
    unsigned padding = 0;
};
struct LightDesc {
    float position[3];
    float color[3];
    float intensity = 1.0f;
    float range = 10.0f;
};
struct FrameStats {
    double cpuMs = 0.0;
    double gpuMs = 0.0;
    bool gpuValid = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(HWND window, unsigned width, unsigned height, std::string& error);
    bool resize(unsigned width, unsigned height, std::string& error);
    bool render(const Mat4& viewProjection, const std::vector<DrawItem>& items,
                const std::vector<LightDesc>& lights, std::string& error);
    void shutdown();
    std::string capabilitiesJson() const;
    std::string probesJson() const;
    FrameStats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace nativegpu
