// Contract checks for the renderer boundary. The suite runs against a mock backend, which
// is the point: a second backend (Vulkan, X2) must pass exactly these rules before it is
// allowed to claim it implements the interface. The D3D12 factory itself is exercised by
// every native smoke run (report.rhi.backend), not here, because it needs a window.
#include "../renderer/rhi.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& name, const std::string& detail = {}) {
    ++checks;
    if (condition) {
        std::printf("  ok   %s\n", name.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
}

// Minimal backend that follows the interface and nothing else.
class MockDevice : public nativegpu::rhi::Device {
public:
    const char* backend() const override { return name_.c_str(); }
    bool initialized() const override { return initialized_; }
    bool initialize(const nativegpu::rhi::Descriptor& descriptor, std::string& error) override {
        if (descriptor.window == nullptr || descriptor.width == 0 || descriptor.height == 0) {
            error = "NG-E-RHI-SIZE: mock needs a window and a size";
            return false;
        }
        initialized_ = true;
        ++initializeCalls_;
        return true;
    }
    void shutdown() override { initialized_ = false; }
    bool resize(unsigned width, unsigned height, std::string& error) override {
        if (width == 0 || height == 0) {
            error = "NG-E-RHI-SIZE: zero surface dimension";
            return false;
        }
        if (!initialized_) {
            error = "NG-E-RHI-STATE: resize before initialize";
            return false;
        }
        width_ = width;
        height_ = height;
        return true;
    }
    bool submit(const nativegpu::Mat4&, const float[3],
                const std::vector<nativegpu::DrawItem>&,
                const std::vector<nativegpu::LightDesc>&, std::string& error) override {
        if (!initialized_) {
            error = "NG-E-RHI-STATE: submit before initialize";
            return false;
        }
        ++frames_;
        return true;
    }
    void setRtShadows(bool enabled) override { rtShadows_ = enabled; }
    bool rtShadowsEnabled() const override { return rtShadows_; }
    void setMeshShaders(bool enabled) override { mesh_ = enabled; }
    bool meshShadersEnabled() const override { return mesh_; }
    void setCompositeDebugView(unsigned view) override { view_ = view; }
    void setCompute(bool enabled, bool corrupt) override { compute_ = enabled; computeCorrupt_ = corrupt; }
    void setParticles(bool enabled) override { particles_ = enabled; }
    bool setEffectSprites(std::vector<nativegpu::EffectSprite>, std::string&) override { return true; }
    void setFieldColliders(const std::vector<nativegpu::FieldCollider>& colliders) override {
        colliders_ = static_cast<unsigned>(colliders.size());
    }
    void setFireEffect(bool firePreset, unsigned explodeAtFrame) override {
        firePreset_ = firePreset;
        explodeAt_ = explodeAtFrame;
    }
    bool requestScreenshot(const std::string&, std::string&) override { return true; }
    bool captureScreenshot(const std::string&, std::string&) override { return true; }
    nativegpu::FrameStats stats() const override {
        nativegpu::FrameStats stats;
        stats.cpuMs = 1.0;
        return stats;
    }
    std::string capabilitiesJson() const override { return "{\"backend\":\"mock\"}"; }
    std::string probesJson() const override { return "{}"; }
    unsigned long long videoMemoryBytes() override { return 0; }
    void* nativeHandle() override { return nullptr; }

    unsigned frames() const { return frames_; }
    unsigned initializeCalls() const { return initializeCalls_; }

private:
    std::string name_ = "mock";
    bool initialized_ = false;
    bool rtShadows_ = false;
    bool mesh_ = false;
    unsigned view_ = 0;
    unsigned width_ = 0, height_ = 0;
    bool compute_ = false;
    bool computeCorrupt_ = false;
    bool particles_ = false;
    unsigned colliders_ = 0;
    bool firePreset_ = false;
    unsigned explodeAt_ = 0;
    unsigned frames_ = 0;
    unsigned initializeCalls_ = 0;
};

} // namespace

// The D3D12 factory lives in rhi_d3d12.cpp, which needs the whole renderer. The contract
// suite does not, so it links against this stub and never asks for "d3d12".
namespace nativegpu {
namespace rhi {
namespace detail {
std::unique_ptr<Device> createD3D12(Renderer&) { return nullptr; }
} // namespace detail
} // namespace rhi
} // namespace nativegpu

int main() {
    using nativegpu::rhi::conformance;
    std::printf("rhi boundary checks\n");

    check(nativegpu::rhi::backends().size() == 1, "exactly one backend is registered");
    check(nativegpu::rhi::backends()[0] == "d3d12", "the registered backend is d3d12",
          nativegpu::rhi::backends()[0]);

    {
        MockDevice device;
        std::vector<std::string> problems;
        check(conformance(device, problems), "mock backend satisfies the device contract",
              problems.empty() ? "" : problems[0]);
        check(problems.empty(), "conformance reports no failures for a correct backend");
    }

    // A backend that accepts submit before initialize must be rejected by the suite.
    class BrokenDevice final : public MockDevice {
    public:
        bool submit(const nativegpu::Mat4&, const float[3],
                    const std::vector<nativegpu::DrawItem>&,
                    const std::vector<nativegpu::LightDesc>&, std::string&) override {
            return true;
        }
    };
    {
        BrokenDevice device;
        std::vector<std::string> problems;
        check(!conformance(device, problems), "suite rejects a backend that accepts early submit");
        check(!problems.empty(), "suite explains the rejected rule");
    }

    // A backend that reports initialized from the start must be rejected too.
    class PrematureDevice final : public MockDevice {
    public:
        bool initialized() const override { return true; }
    };
    {
        PrematureDevice device;
        std::vector<std::string> problems;
        check(!conformance(device, problems), "suite rejects a device that starts initialized");
    }

    // Factory behaviour that does not need the renderer.
    {
        check(nativegpu::rhi::supports("d3d12"), "factory supports d3d12");
        check(!nativegpu::rhi::supports("vulkan"), "factory rejects vulkan until X2");
        check(!nativegpu::rhi::supports(""), "factory rejects an empty backend name");
    }

    // Mock state machine details that the suite relies on.
    {
        MockDevice device;
        nativegpu::rhi::Descriptor descriptor;
        descriptor.width = 640;
        descriptor.height = 360;
        std::string error;
        check(!device.initialize(descriptor, error), "mock rejects a descriptor without a window");
        check(error.rfind("NG-E-RHI-", 0) == 0, "mock rejection uses an RHI code", error);
        check(device.initializeCalls() == 0, "rejected initialize is not counted");
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
