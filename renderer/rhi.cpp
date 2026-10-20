#include "rhi.hpp"

namespace nativegpu {
namespace rhi {
namespace {

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

} // namespace

std::unique_ptr<Device> wrap(const std::string& backend, Renderer& renderer, std::string& error) {
    if (supports(backend)) {
        return detail::createD3D12(renderer);
    }
    error = "NG-E-RHI-BACKEND: unsupported backend '" + backend + "'";
    return nullptr;
}

std::vector<std::string> backends() {
    return {"d3d12"};
}

bool supports(const std::string& backend) {
    return backend == "d3d12";
}

bool conformance(Device& device, std::vector<std::string>& failures) {
    failures.clear();

    if (std::string(device.backend()).empty()) {
        failures.push_back("backend name is empty");
    }
    if (device.initialized()) {
        failures.push_back("fresh device reports initialized");
    }

    const Mat4 identity = identityMatrix();
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    std::vector<DrawItem> items;
    std::vector<LightDesc> lights;
    std::string error;

    if (device.submit(identity, eye, items, lights, error)) {
        failures.push_back("submit before initialize was accepted");
    } else if (!startsWith(error, "NG-E-RHI-")) {
        failures.push_back("submit before initialize returned a non-RHI error: " + error);
    }
    error.clear();

    if (device.resize(0, 0, error)) {
        failures.push_back("zero-dimension resize was accepted");
    } else if (!startsWith(error, "NG-E-RHI-")) {
        failures.push_back("zero-dimension resize returned a non-RHI error: " + error);
    }
    error.clear();

    Descriptor missing;
    if (device.initialize(missing, error)) {
        failures.push_back("initialize without a window was accepted");
    }
    error.clear();

    Descriptor descriptor;
    // The contract suite never opens a real surface: a backend may require one, so this
    // step is skipped and only the state machine above is asserted.
    (void)descriptor;

    if (device.initialized()) {
        failures.push_back("device became initialized without a successful initialize");
    }
    return failures.empty();
}

} // namespace rhi
} // namespace nativegpu
