#pragma once

#include <memory>
#include <string>
#include <vector>

#include "d3d12_renderer.h"

// Minimal renderer boundary. The point is not to hide D3D12 behind a grand abstraction;
// it is to make the frame path depend on an interface that a second backend can implement
// later (X2 Vulkan), and to have one conformance suite both backends must pass.
//
// What is inside the boundary: device init/shutdown, resize, per-frame submit, quality
// toggles, screenshots, stats and capability/probe JSON.
// What is deliberately outside: reliability fault injection and the static object
// counters. Those are test hooks for the D3D12 backend, reached through nativeHandle(),
// and a future backend is allowed not to implement them.
namespace nativegpu {
namespace rhi {

struct Descriptor {
    void* window = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    bool validation = false;
};

class Device {
public:
    virtual ~Device() = default;

    // Backend name as reported in evidence ("d3d12" today).
    virtual const char* backend() const = 0;
    virtual bool initialized() const = 0;

    virtual bool initialize(const Descriptor& descriptor, std::string& error) = 0;
    virtual void shutdown() = 0;
    // A zero dimension is rejected: the surface has no meaningful size.
    virtual bool resize(unsigned width, unsigned height, std::string& error) = 0;

    virtual bool submit(const Mat4& viewProjection, const float cameraPosition[3],
                        const std::vector<DrawItem>& items,
                        const std::vector<LightDesc>& lights, std::string& error) = 0;

    virtual void setRtShadows(bool enabled) = 0;
    virtual bool rtShadowsEnabled() const = 0;
    virtual void setMeshShaders(bool enabled) = 0;
    virtual bool meshShadersEnabled() const = 0;
    virtual void setCompositeDebugView(unsigned view) = 0;

    virtual bool requestScreenshot(const std::string& path, std::string& error) = 0;
    virtual bool captureScreenshot(const std::string& path, std::string& error) = 0;

    virtual FrameStats stats() const = 0;
    virtual std::string capabilitiesJson() const = 0;
    virtual std::string probesJson() const = 0;
    virtual unsigned long long videoMemoryBytes() = 0;

    // Backend-specific handle for test hooks. Null when the backend has none.
    virtual void* nativeHandle() = 0;
};

// Wraps an existing D3D12 renderer. Ownership stays with the caller so the host can keep
// using its test hooks; the device only forwards the frame path.
std::unique_ptr<Device> wrap(const std::string& backend, Renderer& renderer, std::string& error);

// Backends this build can construct. "d3d12" only, until X2.
std::vector<std::string> backends();

// True when this build can construct the named backend.
bool supports(const std::string& backend);

// Backend-agnostic contract checks: a Device must reject submit before initialize, reject
// a zero dimension, report itself initialized only after a successful initialize, and go
// back to uninitialized after shutdown. `failures` receives one line per violated rule.
bool conformance(Device& device, std::vector<std::string>& failures);

namespace detail {
// Implemented by the D3D12 translation unit; kept separate so the contract checks can be
// linked without the renderer.
std::unique_ptr<Device> createD3D12(Renderer& renderer);
} // namespace detail

} // namespace rhi
} // namespace nativegpu
