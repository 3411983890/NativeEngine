#include "rhi.hpp"

#include <algorithm>

namespace nativegpu {
namespace rhi {
namespace {

// Forwards the frame path to the D3D12 renderer. Every method is a straight call: the
// adapter must not add policy, otherwise the two backends stop being comparable.
class D3D12Device final : public Device {
public:
    explicit D3D12Device(Renderer& renderer) : renderer_(renderer) {}

    const char* backend() const override { return "d3d12"; }
    bool initialized() const override { return initialized_; }

    bool initialize(const Descriptor& descriptor, std::string& error) override {
        if (descriptor.window == nullptr) {
            error = "NG-E-RHI-WINDOW: no window handle";
            return false;
        }
        if (descriptor.width == 0 || descriptor.height == 0) {
            error = "NG-E-RHI-SIZE: zero surface dimension";
            return false;
        }
        renderer_.setValidationEnabled(descriptor.validation);
        initialized_ = renderer_.initialize(static_cast<HWND>(descriptor.window),
                                           descriptor.width, descriptor.height, error);
        return initialized_;
    }

    void shutdown() override {
        renderer_.shutdown();
        initialized_ = false;
    }

    bool resize(unsigned width, unsigned height, std::string& error) override {
        if (width == 0 || height == 0) {
            error = "NG-E-RHI-SIZE: zero surface dimension";
            return false;
        }
        if (!initialized_) {
            error = "NG-E-RHI-STATE: resize before initialize";
            return false;
        }
        return renderer_.resize(width, height, error);
    }

    bool submit(const Mat4& viewProjection, const float cameraPosition[3],
                const std::vector<DrawItem>& items, const std::vector<LightDesc>& lights,
                std::string& error) override {
        if (!initialized_) {
            error = "NG-E-RHI-STATE: submit before initialize";
            return false;
        }
        return renderer_.render(viewProjection, cameraPosition, items, lights, error);
    }

    void setRtShadows(bool enabled) override { renderer_.setRtShadows(enabled); }
    bool rtShadowsEnabled() const override { return renderer_.rtShadowsEnabled(); }
    void setMeshShaders(bool enabled) override { renderer_.setMeshShaders(enabled); }
    bool meshShadersEnabled() const override { return renderer_.meshShadersEnabled(); }
    void setCompositeDebugView(unsigned view) override { renderer_.setCompositeDebugView(view); }

    bool requestScreenshot(const std::string& path, std::string& error) override {
        return renderer_.requestScreenshot(path, error);
    }
    bool captureScreenshot(const std::string& path, std::string& error) override {
        return renderer_.captureScreenshot(path, error);
    }

    FrameStats stats() const override { return renderer_.stats(); }
    std::string capabilitiesJson() const override { return renderer_.capabilitiesJson(); }
    std::string probesJson() const override { return renderer_.probesJson(); }
    unsigned long long videoMemoryBytes() override { return renderer_.videoMemoryBytes(); }
    void* nativeHandle() override { return &renderer_; }

private:
    Renderer& renderer_;
    bool initialized_ = false;
};

} // namespace

namespace detail {
std::unique_ptr<Device> createD3D12(Renderer& renderer) {
    return std::unique_ptr<Device>(new D3D12Device(renderer));
}
} // namespace detail

} // namespace rhi
} // namespace nativegpu
