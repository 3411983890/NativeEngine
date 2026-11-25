#pragma once

// Audio runtime for the native host.
//
// Design contract (.developmentAgent/animation-audio/AUDIO-PLAN.md):
//  - the mixer runs at a fixed 48 kHz and a fixed block size, independent of the
//    render frame rate; commands carry a target sample, never a frame number;
//  - the realtime path allocates nothing, takes no locks and does no file IO;
//  - gameplay state is read-only for audio: nothing here feeds back into the
//    determinism digest;
//  - the offline renderer is the only backend that promises bit-identical output.
//
// The device backends live in audio_device.cpp and are only compiled into the
// host; tests link audio.cpp + audio_asset.cpp and use the offline path.

#include "../native/vec3.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::audio {

using engine::Vec3;

constexpr uint32_t kSampleRate = 48000;

enum class Quality : uint8_t { Off = 0, Stereo = 1, Spatial = 2, Binaural = 3, Atmos = 4 };
enum class Bus : uint8_t { Master = 0, Sfx, Ambience, Music, Voice, Ui, Count };
enum class Layout : uint8_t { Stereo = 2, Surround51 = 6, Surround71 = 8, Surround712 = 10 };

const char* qualityName(Quality quality);
const char* busName(Bus bus);
const char* layoutName(Layout layout);
uint32_t layoutChannels(Layout layout);

struct Config {
    uint32_t sampleRate = kSampleRate;
    uint32_t blockFrames = 256;
    Quality quality = Quality::Spatial;
    Layout layout = Layout::Surround712;
    uint32_t maxObjects = 32;
    uint32_t maxPhysicalVoices = 64;
    uint32_t maxVirtualVoices = 1024;
    // How far ahead of the mixer's write position a command is stamped. A backend
    // that renders whole device periods advances the clock in one step, so a command
    // submitted at "now" can already be in the past by the time the mixer drains it.
    // One device period of lead makes every submission land in the future; 0 keeps the
    // strictly-immediate behaviour the offline tests rely on.
    uint32_t commandLeadSamples = 0;
    float masterGainDb = 0.0f;
    float loudnessTargetLufs = -16.0f;
    bool headTracking = false;
};

// A fully decoded, already resampled mono/stereo sound. Import converts to 48 kHz
// so the voice path only has to interpolate for pitch.
struct Sound {
    std::string name;
    std::vector<float> samples;  // interleaved
    uint32_t channels = 1;
    uint32_t sampleRate = kSampleRate;
    bool loop = false;
    float loudnessLufs = -16.0f;
    float peakDbtp = -1.0f;
};

struct PlayParams {
    Vec3 position{};
    bool spatial = true;
    Bus bus = Bus::Sfx;
    float gainDb = 0.0f;
    float pitch = 1.0f;
    bool loop = false;
    float minDistance = 1.5f;
    bool overrideLoop = false; // scene authoring can explicitly disable an asset loop
    float maxDistance = 28.0f;
    float occlusion = 1.0f;
    uint32_t priority = 128;
    bool object = true;
};

struct Listener {
    Vec3 position{};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 velocity{};
};

// Axis-aligned scene occluder. `gainDb`/`cutoffHz` describe the material behind it.
struct Occluder {
    Vec3 min{};
    Vec3 max{};
    float gainDb = -12.0f;
    float cutoffHz = 800.0f;
};

struct ObjectMeta {
    Vec3 position{};      // listener-relative, metres
    float gainDb = 0.0f;
    float spread = 0.0f;
    uint16_t priority = 128;
    uint8_t snap = 0;
    uint8_t binaural = 0;
};

struct VoiceReceipt {
    uint32_t id = 0;
    uint32_t sound = 0;
    Bus bus = Bus::Sfx;
    bool physical = false;
    bool occluded = false;
    bool object = false;
    uint16_t priority = 0;
    float distance = 0.0f;
    float gainDb = 0.0f;
    float cutoffHz = 20000.0f;
};

struct Stats {
    uint32_t physical = 0;
    uint32_t virtualVoices = 0;
    uint32_t objects = 0;
    uint32_t started = 0;
    uint32_t steals = 0;
    uint64_t blocks = 0;
    uint64_t underruns = 0;
    uint64_t lateCommands = 0;
    float peak = 0.0f;          // sample peak in the last block
    // Sample peak over the whole session. `peak` is per block, which is what a meter
    // draws; this is what a gate can assert on.
    float peakMax = 0.0f;
    float rms = 0.0f;
    float lufsShort = -70.0f;
    // Peak-hold of the short-term meter: the loudest 3 s window seen so far.
    float lufsShortMax = -70.0f;
    float lufsIntegrated = -70.0f;
    float cpuMs = 0.0f;
    uint64_t pcmHash = 0xcbf29ce484222325ull;
    uint64_t frames = 0;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool configure(const Config& config, std::string& error);
    // Call only after the device/render consumer is stopped. Releases old banks/voices.
    void reset();
    const Config& config() const;

    // Sounds must be registered before the first render call. Returns a handle.
    uint32_t addSound(std::shared_ptr<const Sound> sound);
    uint32_t findSound(const std::string& name) const;
    uint32_t soundCount() const;

    void setListener(const Listener& listener);
    // Runtime override for Config::commandLeadSamples; the host calls it once the
    // device period is known.
    void setCommandLead(uint32_t samples);
    void setBusGain(Bus bus, float db);
    void setMasterGain(float db);
    void setOccluders(std::vector<Occluder> occluders);
    void setPaused(bool paused);

    uint32_t play(uint32_t sound, const PlayParams& params);
    // Commands retain the play receipt id, including before the first render block.
    bool setVoicePosition(uint32_t voice, Vec3 position);
    void stop(uint32_t voice, float fadeSeconds = 0.05f);
    void stopAll(float fadeSeconds = 0.05f);

    // Renders interleaved frames in config().layout channels. Thread-safe against the
    // producer side (play/stop/listener) through the command ring.
    void render(float* out, uint32_t frames);

    // Deterministic helper used by tests and evidence. Not for realtime use.
    std::vector<float> renderOffline(float seconds);

    Stats stats() const;
    std::vector<VoiceReceipt> receipts() const;
    std::vector<ObjectMeta> objects() const;
    void resetStats();

    // Reports a backend underrun; the device thread calls this when it misses a period.
    void noteUnderrun();
    // Snapshot of the last rendered block for tests (interleaved).
    const std::vector<float>& lastBlock() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- assets -----------------------------------------------------------------

bool loadWav(const std::string& path, Sound& out, std::string& error);
bool loadNgsnd(const std::string& path, Sound& out, std::string& error);
bool saveNgsnd(const std::string& path, const Sound& sound, std::string& error);
// Resamples to kSampleRate with a windowed-sinc polyphase filter.
bool resampleTo48k(Sound& sound, std::string& error);

// ---- spatial helpers (exposed for tests) ------------------------------------

// Constant-power VBAP gains for a unit direction in listener space. Writes
// `channels` gains (LFE is left at zero). Returns false when the direction falls
// outside the layout (caller should fall back to the nearest speaker).
bool panDirection(const Vec3& direction, Layout layout, float* gains, uint32_t channels);

// Doppler ratio for a radial relative velocity (343 m/s), clamped to [0.5, 2].
float dopplerRatio(Vec3 sourceVelocity, Vec3 listenerVelocity, Vec3 direction);

// Procedural HRTF: interaural time difference in seconds for a lateral angle.
float hrtfItdSeconds(float azimuthRadians);
// Head-shadow cutoff for one ear; `lateral` is +1 for the ipsilateral side.
float hrtfShadowCutoff(float lateral);

// 7.1.2 -> 5.1 -> stereo. `in`/`out` may alias when outChannels <= inChannels.
void downmix(const float* in, uint32_t inChannels, float* out, uint32_t outChannels,
             uint32_t frames);

// ITU-R BS.1770 K-weighting at 48 kHz (stage 1 shelf + stage 2 high-pass).
void kWeightingCoefficients(float& b0, float& b1, float& b2,
                            float& a1, float& a2, int stage);

} // namespace engine::audio
