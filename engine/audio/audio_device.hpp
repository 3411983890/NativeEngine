#pragma once

// WASAPI device backend. Kept out of the core so the offline tests can link the
// mixer without any COM dependency.
//
// Scope: shared-mode event-driven rendering at the device mix format. If the
// endpoint is not 48 kHz the device thread does a linear conversion at the boundary;
// the mixer itself always runs at 48 kHz.

#include "audio.hpp"

#include <memory>
#include <string>

namespace engine::audio {

struct DeviceInfo {
    std::string name = "default";
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bufferFrames = 0;
    bool floatFormat = true;
};

class WasapiDevice {
public:
    WasapiDevice();
    ~WasapiDevice();
    WasapiDevice(const WasapiDevice&) = delete;
    WasapiDevice& operator=(const WasapiDevice&) = delete;

    bool start(Engine& engine, std::string& error);
    void stop();
    bool running() const;
    const DeviceInfo& info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::audio
