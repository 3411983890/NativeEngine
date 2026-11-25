// WASAPI shared-mode backend.
//
// The device thread is the only place in the audio system that touches wall-clock
// time. It renders whole blocks through Engine::render (which never allocates) and
// then converts to the endpoint format. Every failure path either falls back to a
// clear error or counts an underrun; nothing here can block the game thread.

#include "audio_device.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace engine::audio {
namespace {

template <typename T>
void release(T*& pointer) {
    if (pointer) { pointer->Release(); pointer = nullptr; }
}

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT has Data1 == 3. Comparing the tag avoids
        // pulling the GUID symbol (and ksuser.dll) into the executable.
        return extended->SubFormat.Data1 == 3 && format->wBitsPerSample == 32;
    }
    return false;
}

uint32_t channelCount(uint32_t device, uint32_t source) {
    return device < source ? device : source;
}

} // namespace

struct WasapiDevice::Impl {
    Engine* engine = nullptr;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* renderer = nullptr;
    HANDLE event = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> quit{false};
    bool comInitialized = false;
    DeviceInfo info;
    WAVEFORMATEX* format = nullptr;

    std::vector<float> mix;       // engine-layout block
    std::vector<float> deviceOut; // endpoint-layout block
    std::vector<float> resampleState;
    double resamplePosition = 0.0;
    bool haveResampleState = false;

    void threadMain() {
        const uint32_t engineChannels = layoutChannels(engine->config().layout);
        const uint32_t deviceChannels = info.channels;
        const double ratio = static_cast<double>(info.sampleRate) / kSampleRate;
        std::vector<float> tail(engineChannels, 0.0f);
        while (!quit.load(std::memory_order_relaxed)) {
            const DWORD wait = WaitForSingleObject(event, 200);
            if (quit.load(std::memory_order_relaxed)) break;
            if (wait != WAIT_OBJECT_0) {
                engine->noteUnderrun();
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                engine->noteUnderrun();
                continue;
            }
            const UINT32 available = info.bufferFrames - padding;
            if (available == 0) continue;
            BYTE* buffer = nullptr;
            if (FAILED(renderer->GetBuffer(available, &buffer)) || !buffer) {
                engine->noteUnderrun();
                continue;
            }
            engine->render(mix.data(), available);
            // Channel map first, then optional sample-rate conversion.
            const uint32_t mapped = channelCount(deviceChannels, engineChannels);
            const size_t outSamples = static_cast<size_t>(available) * deviceChannels;
            if (deviceChannels == 2 && engineChannels > 2) {
                downmix(mix.data(), engineChannels, deviceOut.data(), 2, available);
            } else if (deviceChannels == 6 && engineChannels > 6) {
                downmix(mix.data(), engineChannels, deviceOut.data(), 6, available);
            } else {
                for (uint32_t f = 0; f < available; ++f) {
                    for (uint32_t ch = 0; ch < deviceChannels; ++ch) {
                        deviceOut[static_cast<size_t>(f) * deviceChannels + ch] =
                            ch < mapped ? mix[static_cast<size_t>(f) * engineChannels + ch] : 0.0f;
                    }
                }
            }
            if (info.floatFormat && info.sampleRate == kSampleRate) {
                std::memcpy(buffer, deviceOut.data(), outSamples * sizeof(float));
            } else if (info.floatFormat) {
                // Linear conversion at the device boundary. The mixer never sees it.
                float* out = reinterpret_cast<float*>(buffer);
                const uint32_t produced = static_cast<uint32_t>(available * ratio);
                for (uint32_t i = 0; i < produced && i < info.bufferFrames; ++i) {
                    const double position = resamplePosition + i / ratio;
                    const uint32_t index = static_cast<uint32_t>(position);
                    const float frac = static_cast<float>(position - index);
                    for (uint32_t ch = 0; ch < deviceChannels; ++ch) {
                        const float a = index < available ? deviceOut[static_cast<size_t>(index) * deviceChannels + ch]
                                                          : tail[ch % engineChannels];
                        const float b = index + 1 < available
                            ? deviceOut[static_cast<size_t>(index + 1) * deviceChannels + ch]
                            : a;
                        out[static_cast<size_t>(i) * deviceChannels + ch] = a + (b - a) * frac;
                    }
                }
                for (uint32_t ch = 0; ch < engineChannels; ++ch)
                    tail[ch] = mix[static_cast<size_t>(available - 1) * engineChannels + ch];
                resamplePosition = std::fmod(resamplePosition + available * ratio, 1.0);
            } else {
                int16_t* out = reinterpret_cast<int16_t*>(buffer);
                for (size_t i = 0; i < outSamples; ++i) {
                    const float value = deviceOut[i];
                    out[i] = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, value)) * 32767.0f);
                }
            }
            renderer->ReleaseBuffer(available, 0);
        }
    }

    void teardown() {
        // Signal the thread first: stopping the client before the thread has left the
        // wait loop can leave the render callback racing the device teardown.
        if (thread) {
            quit.store(true);
            if (event) SetEvent(event);
            WaitForSingleObject(thread, 2000);
            CloseHandle(thread);
            thread = nullptr;
        }
        if (client) client->Stop();
        if (event) { CloseHandle(event); event = nullptr; }
        release(renderer);
        release(client);
        release(endpoint);
        release(enumerator);
        if (format) { CoTaskMemFree(format); format = nullptr; }
        // COM stays initialized for the process lifetime. Calling CoUninitialize here
        // tears down audio DLLs that the CRT still has atexit hooks into, which shows
        // up as ERROR_PROC_NOT_FOUND on some Windows/mingw combinations.
        comInitialized = false;
        running.store(false);
    }
};

WasapiDevice::WasapiDevice() : impl_(new Impl()) {}
WasapiDevice::~WasapiDevice() { stop(); }

bool WasapiDevice::start(Engine& engine, std::string& error) {
    if (impl_->running.load()) { error = "device already running"; return false; }
    impl_->engine = &engine;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { error = "CoInitializeEx failed"; return false; }
    impl_->comInitialized = true;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&impl_->enumerator)))) {
        error = "no audio device enumerator";
        impl_->teardown();
        return false;
    }
    if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->endpoint)) ||
        !impl_->endpoint) {
        error = "no default render endpoint";
        impl_->teardown();
        return false;
    }
    if (FAILED(impl_->endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(&impl_->client)))) {
        error = "cannot activate IAudioClient";
        impl_->teardown();
        return false;
    }
    if (FAILED(impl_->client->GetMixFormat(&impl_->format)) || !impl_->format) {
        error = "cannot read the device mix format";
        impl_->teardown();
        return false;
    }
    const WAVEFORMATEX* format = impl_->format;
    impl_->info.sampleRate = format->nSamplesPerSec;
    impl_->info.channels = format->nChannels;
    impl_->info.floatFormat = isFloatFormat(format);
    if (!impl_->info.floatFormat && format->wBitsPerSample != 16) {
        error = "unsupported device sample format";
        impl_->teardown();
        return false;
    }
    // 20 ms shared-mode buffer keeps latency in the documented range without
    // fighting other applications for the endpoint.
    if (FAILED(impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         200000, 0, format, nullptr))) {
        error = "IAudioClient::Initialize failed";
        impl_->teardown();
        return false;
    }
    UINT32 bufferFrames = 0;
    if (FAILED(impl_->client->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
        error = "device buffer size is zero";
        impl_->teardown();
        return false;
    }
    impl_->info.bufferFrames = bufferFrames;
    if (FAILED(impl_->client->GetService(__uuidof(IAudioRenderClient),
                                         reinterpret_cast<void**>(&impl_->renderer)))) {
        error = "cannot get the render client";
        impl_->teardown();
        return false;
    }
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event || FAILED(impl_->client->SetEventHandle(impl_->event))) {
        error = "cannot create the device event";
        impl_->teardown();
        return false;
    }
    const uint32_t engineChannels = layoutChannels(engine.config().layout);
    impl_->mix.assign(static_cast<size_t>(bufferFrames) * engineChannels, 0.0f);
    impl_->deviceOut.assign(static_cast<size_t>(bufferFrames) * impl_->info.channels, 0.0f);
    if (FAILED(impl_->client->Start())) {
        error = "IAudioClient::Start failed";
        impl_->teardown();
        return false;
    }
    impl_->quit.store(false);
    impl_->running.store(true);
    impl_->thread = CreateThread(nullptr, 0, [](void* context) -> DWORD {
        static_cast<Impl*>(context)->threadMain();
        return 0;
    }, impl_.get(), 0, nullptr);
    if (!impl_->thread) {
        error = "cannot start the device thread";
        impl_->teardown();
        return false;
    }
    SetThreadPriority(impl_->thread, THREAD_PRIORITY_TIME_CRITICAL);
    return true;
}

void WasapiDevice::stop() {
    if (impl_) impl_->teardown();
}

bool WasapiDevice::running() const { return impl_->running.load(); }
const DeviceInfo& WasapiDevice::info() const { return impl_->info; }

} // namespace engine::audio
