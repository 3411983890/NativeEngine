// Core mixer: fixed-rate clock, lock-free command ring, voice manager, VBAP,
// procedural binaural, occlusion, object metadata, metering and downmix.
//
// The renderer is deterministic: same command stream + same seeds -> same PCM.
// The device layer is the only place where wall-clock timing enters, and it never
// feeds back into the mixer.

#include "audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::audio {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSpeedOfSound = 343.0f;
constexpr uint32_t kMaxVoices = 2048;
constexpr uint32_t kRingSize = 1024;
constexpr uint32_t kMaxOccluders = 256;
constexpr uint32_t kHrtfDelay = 64;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    if (length < 1e-8f) return {0.0f, 0.0f, 1.0f};
    return {v.x / length, v.y / length, v.z / length};
}
float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
float gainToDb(float gain) { return gain > 1e-9f ? 20.0f * std::log10(gain) : -120.0f; }

// One-pole low-pass used for occlusion and head shadow. `a` is the per-sample
// coefficient; callers derive it from a cutoff and a sample rate.
struct OnePole {
    float y = 0.0f;
    float a = 1.0f;
    void setCutoff(float hz, float sampleRate) {
        const float clamped = clampf(hz, 20.0f, sampleRate * 0.45f);
        a = 1.0f - std::exp(-2.0f * kPi * clamped / sampleRate);
    }
    float process(float x) {
        y += a * (x - y);
        return y;
    }
    void reset() { y = 0.0f; }
};

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
    void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};

// ---- layout / VBAP -----------------------------------------------------------

struct Speaker {
    Vec3 direction;
    bool lfe;
    bool height;
};

constexpr int kMaxSpeakers = 10;

struct PanTable {
    Speaker speakers[kMaxSpeakers];
    int count = 0;
    // Floor ring in azimuth order; panning walks adjacent pairs, which is the
    // non-overlapping 2D VBAP triangulation for a ring layout.
    int floorOrder[kMaxSpeakers]{};
    int floorCount = 0;
    struct Pair { int i, j; float inv[4]; };
    Pair pairs[kMaxSpeakers]{};
    int pairCount = 0;
    int topLeft = -1, topRight = -1;
    bool hasHeight = false;
};

Vec3 speakerDirection(float azimuthDegrees, float elevationDegrees) {
    const float az = azimuthDegrees * kPi / 180.0f;
    const float el = elevationDegrees * kPi / 180.0f;
    return {std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)};
}

float speakerAzimuth(const Vec3& direction) {
    return std::atan2(direction.x, direction.z) * 180.0f / kPi;
}

void buildPanTable(Layout layout, PanTable& table) {
    struct Entry { float az, el; bool lfe; };
    static const Entry stereo[] = {{-90.0f, 0.0f, false}, {90.0f, 0.0f, false}};
    static const Entry surround51[] = {
        {-30.0f, 0.0f, false}, {30.0f, 0.0f, false}, {0.0f, 0.0f, false}, {0.0f, 0.0f, true},
        {-110.0f, 0.0f, false}, {110.0f, 0.0f, false}};
    static const Entry surround71[] = {
        {-30.0f, 0.0f, false}, {30.0f, 0.0f, false}, {0.0f, 0.0f, false}, {0.0f, 0.0f, true},
        {-110.0f, 0.0f, false}, {110.0f, 0.0f, false}, {-150.0f, 0.0f, false}, {150.0f, 0.0f, false}};
    static const Entry surround712[] = {
        {-30.0f, 0.0f, false}, {30.0f, 0.0f, false}, {0.0f, 0.0f, false}, {0.0f, 0.0f, true},
        {-110.0f, 0.0f, false}, {110.0f, 0.0f, false}, {-150.0f, 0.0f, false}, {150.0f, 0.0f, false},
        {-45.0f, 45.0f, false}, {45.0f, 45.0f, false}};
    const Entry* entries = surround712;
    int n = 10;
    if (layout == Layout::Stereo) { entries = stereo; n = 2; }
    else if (layout == Layout::Surround51) { entries = surround51; n = 6; }
    else if (layout == Layout::Surround71) { entries = surround71; n = 8; }
    table.count = n;
    table.floorCount = table.pairCount = 0;
    table.hasHeight = false;
    table.topLeft = table.topRight = -1;
    for (int i = 0; i < n; ++i) {
        table.speakers[i].direction = speakerDirection(entries[i].az, entries[i].el);
        table.speakers[i].lfe = entries[i].lfe;
        table.speakers[i].height = entries[i].el > 10.0f;
        if (table.speakers[i].height) {
            table.hasHeight = true;
            if (entries[i].az < 0.0f) table.topLeft = i;
            else table.topRight = i;
        } else if (!entries[i].lfe) {
            table.floorOrder[table.floorCount++] = i;
        }
    }
    // Sort the floor ring by azimuth so adjacent pairs wrap around correctly.
    for (int i = 0; i < table.floorCount; ++i) {
        for (int j = i + 1; j < table.floorCount; ++j) {
            if (speakerAzimuth(table.speakers[table.floorOrder[j]].direction) <
                speakerAzimuth(table.speakers[table.floorOrder[i]].direction)) {
                std::swap(table.floorOrder[i], table.floorOrder[j]);
            }
        }
    }
    for (int i = 0; i < table.floorCount; ++i) {
        const int a = table.floorOrder[i];
        const int b = table.floorOrder[(i + 1) % table.floorCount];
        const Vec3 va = table.speakers[a].direction;
        const Vec3 vb = table.speakers[b].direction;
        const float det = va.x * vb.z - vb.x * va.z;
        if (std::abs(det) < 1e-4f) continue;
        PanTable::Pair pair;
        pair.i = a; pair.j = b;
        const float invDet = 1.0f / det;
        pair.inv[0] = vb.z * invDet;
        pair.inv[1] = -vb.x * invDet;
        pair.inv[2] = -va.z * invDet;
        pair.inv[3] = va.x * invDet;
        table.pairs[table.pairCount++] = pair;
    }
}

void normalizeGains(float* gains, int count) {
    float energy = 0.0f;
    for (int i = 0; i < count; ++i) energy += gains[i] * gains[i];
    if (energy <= 1e-12f) return;
    const float scale = 1.0f / std::sqrt(energy);
    for (int i = 0; i < count; ++i) gains[i] *= scale;
}

// ---- voices ------------------------------------------------------------------

struct Command {
    enum Type : uint32_t { Play = 0, Stop = 1, StopAll = 2, ListenerUpdate = 3, BusGain = 4,
                           MasterGain = 5, Pause = 6, VoicePosition = 7 };
    uint64_t targetSample = 0;
    uint32_t type = Play;
    uint32_t id = 0;        // sound handle or voice id
    uint32_t voiceId = 0;
    PlayParams params{};
    Listener listener{};
    float value = 0.0f;
    uint32_t bus = 0;
};

struct Voice {
    bool active = false;
    bool physical = false;
    bool dying = false;
    bool loop = false;
    bool spatial = true;
    bool object = false;
    bool tracePending = false;
    uint32_t id = 0;
    uint32_t sound = 0;
    Bus bus = Bus::Sfx;
    Vec3 position{};
    Vec3 velocity{};
    float baseGain = 1.0f;
    float priority = 128.0f;
    float minDistance = 1.5f;
    float maxDistance = 28.0f;
    float occlusion = 1.0f;
    float pitch = 1.0f;
    float pitchEffective = 1.0f;
    float itd = 0.0f;
    float elevationGain = 1.0f;
    float shadowGainLeft = 1.0f;
    float shadowGainRight = 1.0f;
    bool stopping = false;
    double playhead = 0.0;
    float env = 0.0f;
    float envTarget = 1.0f;
    float envStep = 0.0f;
    float gainCurrent = 1.0f;
    float gainTarget = 1.0f;
    float cutoffCurrent = 20000.0f;
    float cutoffTarget = 20000.0f;
    float pan[kMaxSpeakers]{};
    float distance = 0.0f;
    bool occluded = false;
    uint32_t lastTraceBlock = 0;
    OnePole lowpass;
    int objectSlot = -1;
};

struct BinauralState {
    float delayLeft[kHrtfDelay]{};
    float delayRight[kHrtfDelay]{};
    uint32_t write = 0;
    OnePole shadowLeft;
    OnePole shadowRight;
    bool primed = false;
};

// A small Schroeder reverb on the send bus: four damped combs and two allpasses.
struct Reverb {
    static constexpr int kCombs = 4;
    std::array<std::vector<float>, kCombs> comb;
    std::array<uint32_t, kCombs> combIndex{};
    std::array<float, kCombs> combFilter{};
    std::array<std::vector<float>, 2> allpass;
    std::array<uint32_t, 2> allpassIndex{};
    float feedback = 0.82f;
    float damping = 0.25f;
    float wet = 0.0f;

    void configure(float sampleRate) {
        const int lengths[kCombs] = {1557, 1617, 1491, 1422};
        for (int i = 0; i < kCombs; ++i) {
            comb[i].assign(static_cast<size_t>(lengths[i] * sampleRate / 44100.0f), 0.0f);
            combIndex[i] = 0;
            combFilter[i] = 0.0f;
        }
        const int apLengths[2] = {225, 556};
        for (int i = 0; i < 2; ++i) {
            allpass[i].assign(static_cast<size_t>(apLengths[i] * sampleRate / 44100.0f), 0.0f);
            allpassIndex[i] = 0;
        }
    }
    float process(float input) {
        float sum = 0.0f;
        for (int i = 0; i < kCombs; ++i) {
            if (comb[i].empty()) continue;
            float& slot = comb[i][combIndex[i]];
            const float out = slot;
            combFilter[i] = out * (1.0f - damping) + combFilter[i] * damping;
            slot = input + combFilter[i] * feedback;
            if (++combIndex[i] >= comb[i].size()) combIndex[i] = 0;
            sum += out;
        }
        sum *= 0.25f;
        for (int i = 0; i < 2; ++i) {
            if (allpass[i].empty()) continue;
            float& slot = allpass[i][allpassIndex[i]];
            const float buffered = slot;
            const float out = -sum + buffered;
            slot = sum + buffered * 0.5f;
            if (++allpassIndex[i] >= allpass[i].size()) allpassIndex[i] = 0;
            sum = out;
        }
        return sum;
    }
};

// 100 ms loudness windows (BS.1770 gating is applied over these).
constexpr uint32_t kLoudnessWindow = kSampleRate / 10;

struct Meter {
    Biquad shelf[10];
    Biquad highpass[10];
    double energy = 0.0;
    uint32_t frames = 0;
    std::vector<float> windows;
    float peak = 0.0f;
    float rms = 0.0f;
    // Peak-hold on the short-term meter. A single quiet ambient leaves the integrated
    // value at -43 LUFS forever, so "was this mix ever audible" needs a maximum, not an
    // average over silence.
    float shortTermMax = -70.0f;

    void configure(uint32_t channels) {
        float b0, b1, b2, a1, a2;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            kWeightingCoefficients(b0, b1, b2, a1, a2, 1);
            shelf[ch].b0 = b0; shelf[ch].b1 = b1; shelf[ch].b2 = b2;
            shelf[ch].a1 = a1; shelf[ch].a2 = a2;
            kWeightingCoefficients(b0, b1, b2, a1, a2, 2);
            highpass[ch].b0 = b0; highpass[ch].b1 = b1; highpass[ch].b2 = b2;
            highpass[ch].a1 = a1; highpass[ch].a2 = a2;
        }
        windows.reserve(36000);
    }
    void reset() {
        energy = 0.0;
        frames = 0;
        windows.clear();
        peak = 0.0f;
        rms = 0.0f;
        shortTermMax = -70.0f;
        for (auto& f : shelf) f.reset();
        for (auto& f : highpass) f.reset();
    }
    void push(const float* block, uint32_t count, uint32_t channels, uint32_t lfeChannel) {
        double blockEnergy = 0.0;
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const float sample = block[static_cast<size_t>(i) * channels + ch];
                peak = std::max(peak, std::abs(sample));
                blockEnergy += static_cast<double>(sample) * sample;
                if (static_cast<int>(ch) == static_cast<int>(lfeChannel)) continue;
                const float weighted = highpass[ch].process(shelf[ch].process(sample));
                energy += static_cast<double>(weighted) * weighted;
            }
        }
        frames += count;
        const double samples = static_cast<double>(count) * static_cast<double>(channels);
        if (samples > 0.0) rms = static_cast<float>(std::sqrt(blockEnergy / samples));
        if (frames >= kLoudnessWindow) {
            const double mean = energy / static_cast<double>(frames);
            const float lufs = mean > 0.0 ? static_cast<float>(-0.691 + 10.0 * std::log10(mean)) : -70.0f;
            windows.push_back(lufs);
            energy = 0.0;
            frames = 0;
            if (windows.size() > 1) shortTermMax = std::max(shortTermMax, shortTerm());
        }
    }
    float shortTerm() const {
        if (windows.empty()) return -70.0f;
        const size_t count = std::min<size_t>(windows.size(), 30);
        double sum = 0.0;
        for (size_t i = windows.size() - count; i < windows.size(); ++i)
            sum += std::pow(10.0, windows[i] / 10.0);
        return static_cast<float>(10.0 * std::log10(std::max(1e-12, sum / count)));
    }
    float integrated() const {
        if (windows.empty()) return -70.0f;
        double sum = 0.0;
        int count = 0;
        for (float value : windows) {
            if (value <= -70.0f) continue;
            sum += std::pow(10.0, value / 10.0);
            ++count;
        }
        if (count == 0) return -70.0f;
        const double mean = sum / count;
        const double gate = 10.0 * std::log10(mean) - 10.0;
        double gatedSum = 0.0;
        int gatedCount = 0;
        for (float value : windows) {
            if (value <= -70.0f || value <= gate) continue;
            gatedSum += std::pow(10.0, value / 10.0);
            ++gatedCount;
        }
        if (gatedCount == 0) return static_cast<float>(10.0 * std::log10(mean));
        return static_cast<float>(10.0 * std::log10(gatedSum / gatedCount));
    }
};

} // namespace

// ---- public helpers ----------------------------------------------------------

const char* qualityName(Quality quality) {
    switch (quality) {
        case Quality::Off: return "off";
        case Quality::Stereo: return "stereo";
        case Quality::Spatial: return "spatial";
        case Quality::Binaural: return "binaural";
        case Quality::Atmos: return "atmos";
    }
    return "off";
}
const char* busName(Bus bus) {
    switch (bus) {
        case Bus::Master: return "master";
        case Bus::Sfx: return "sfx";
        case Bus::Ambience: return "ambience";
        case Bus::Music: return "music";
        case Bus::Voice: return "voice";
        case Bus::Ui: return "ui";
        default: return "unknown";
    }
}
const char* layoutName(Layout layout) {
    switch (layout) {
        case Layout::Stereo: return "2.0";
        case Layout::Surround51: return "5.1";
        case Layout::Surround71: return "7.1";
        case Layout::Surround712: return "7.1.2";
    }
    return "2.0";
}
uint32_t layoutChannels(Layout layout) {
    switch (layout) {
        case Layout::Stereo: return 2;
        case Layout::Surround51: return 6;
        case Layout::Surround71: return 8;
        case Layout::Surround712: return 10;
    }
    return 2;
}

void kWeightingCoefficients(float& b0, float& b1, float& b2, float& a1, float& a2, int stage) {
    if (stage == 1) {
        b0 = 1.53512485958697f; b1 = -2.69169618940638f; b2 = 1.19839281085285f;
        a1 = -1.69065929318241f; a2 = 0.73248077421585f;
    } else {
        b0 = 1.0f; b1 = -2.0f; b2 = 1.0f;
        a1 = -1.99004745483398f; a2 = 0.99007225036621f;
    }
}

float dopplerRatio(Vec3 sourceVelocity, Vec3 listenerVelocity, Vec3 direction) {
    const Vec3 relative = sourceVelocity - listenerVelocity;
    const float radial = dot(relative, normalize(direction));
    const float ratio = kSpeedOfSound / (kSpeedOfSound + radial);
    return clampf(ratio, 0.5f, 2.0f);
}

float hrtfItdSeconds(float azimuthRadians) {
    // Woodworth-style spherical head: maximum ~0.65 ms at 90 degrees.
    return 0.00065f * std::sin(clampf(azimuthRadians, -1.5707963f, 1.5707963f));
}

float hrtfShadowCutoff(float lateral) {
    const float amount = clampf(std::abs(lateral), 0.0f, 1.0f);
    return 20000.0f * std::exp(-2.2f * amount) + 600.0f;
}

bool panDirection(const Vec3& direction, Layout layout, float* gains, uint32_t channels) {
    static PanTable tables[4];
    static bool built[4]{};
    const int slot = layout == Layout::Stereo ? 0 : layout == Layout::Surround51 ? 1 :
                     layout == Layout::Surround71 ? 2 : 3;
    if (!built[slot]) { buildPanTable(layout, tables[slot]); built[slot] = true; }
    const PanTable& table = tables[slot];
    for (uint32_t i = 0; i < channels; ++i) gains[i] = 0.0f;
    const Vec3 d = normalize(direction);
    if (layout == Layout::Stereo) {
        const float x = clampf(d.x, -1.0f, 1.0f);
        gains[0] = std::sqrt(0.5f * (1.0f - x));
        gains[1] = std::sqrt(0.5f * (1.0f + x));
        return true;
    }
    const float elevation = std::asin(clampf(d.y, -1.0f, 1.0f));
    Vec3 floorDirection{d.x, 0.0f, d.z};
    const float floorLength = std::sqrt(floorDirection.x * floorDirection.x +
                                        floorDirection.z * floorDirection.z);
    if (floorLength < 1e-4f) floorDirection = {0.0f, 0.0f, 1.0f};
    else floorDirection = floorDirection / floorLength;

    float floorGains[kMaxSpeakers]{};
    bool found = false;
    for (int p = 0; p < table.pairCount; ++p) {
        const PanTable::Pair& pair = table.pairs[p];
        const float g0 = pair.inv[0] * floorDirection.x + pair.inv[1] * floorDirection.z;
        const float g1 = pair.inv[2] * floorDirection.x + pair.inv[3] * floorDirection.z;
        if (g0 < -1e-4f || g1 < -1e-4f) continue;
        floorGains[pair.i] = std::max(0.0f, g0);
        floorGains[pair.j] = std::max(0.0f, g1);
        found = true;
        break;
    }
    if (!found) {
        int nearest = table.floorOrder[0];
        float bestDot = -2.0f;
        for (int i = 0; i < table.floorCount; ++i) {
            const int index = table.floorOrder[i];
            const float value = dot(floorDirection, table.speakers[index].direction);
            if (value > bestDot) { bestDot = value; nearest = index; }
        }
        floorGains[nearest] = 1.0f;
    }
    float scale = 1.0f;
    if (table.hasHeight && elevation > 0.01f) {
        const int top = d.x >= 0.0f ? table.topRight : table.topLeft;
        const float up = std::sin(elevation);
        if (top >= 0) {
            floorGains[top] = up;
            scale = std::cos(elevation);
        }
    }
    for (int i = 0; i < table.count; ++i) gains[i] = floorGains[i] * (i == table.topLeft || i == table.topRight
                                                                          ? 1.0f : scale);
    // Tops were written with the unscaled value above; scaling the floor only keeps
    // the height gain intact.
    normalizeGains(gains, static_cast<int>(channels));
    return true;
}

void downmix(const float* in, uint32_t inChannels, float* out, uint32_t outChannels,
             uint32_t frames) {
    if (inChannels == outChannels) {
        if (in != out) std::memcpy(out, in, sizeof(float) * frames * inChannels);
        return;
    }
    // Channel order: L R C LFE Ls Rs [Lrs Rrs] [Tfl Tfr]
    for (uint32_t f = 0; f < frames; ++f) {
        const float* src = in + static_cast<size_t>(f) * inChannels;
        float* dst = out + static_cast<size_t>(f) * outChannels;
        if (outChannels == 2) {
            const float l = src[0];
            const float r = src[1];
            const float c = inChannels > 2 ? src[2] : 0.0f;
            const float ls = inChannels > 4 ? src[4] : 0.0f;
            const float rs = inChannels > 5 ? src[5] : 0.0f;
            const float lrs = inChannels > 6 ? src[6] : 0.0f;
            const float rrs = inChannels > 7 ? src[7] : 0.0f;
            dst[0] = l + 0.7071f * c + 0.7071f * ls + 0.5f * lrs;
            dst[1] = r + 0.7071f * c + 0.7071f * rs + 0.5f * rrs;
        } else if (outChannels == 6) {
            const float lrs = inChannels > 6 ? src[6] : 0.0f;
            const float rrs = inChannels > 7 ? src[7] : 0.0f;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = inChannels > 2 ? src[2] : 0.0f;
            dst[3] = inChannels > 3 ? src[3] : 0.0f;
            dst[4] = (inChannels > 4 ? src[4] : 0.0f) + 0.7071f * lrs;
            dst[5] = (inChannels > 5 ? src[5] : 0.0f) + 0.7071f * rrs;
        } else {
            for (uint32_t ch = 0; ch < outChannels; ++ch)
                dst[ch] = ch < inChannels ? src[ch] : 0.0f;
        }
    }
}

// ---- Engine::Impl ------------------------------------------------------------

struct Engine::Impl {
    Config cfg{};
    std::vector<std::shared_ptr<const Sound>> sounds;
    std::vector<Voice> voices;
    std::vector<BinauralState> binaural;
    struct OccluderBatch {std::array<Occluder,kMaxOccluders> boxes{};uint32_t count=0;};
    std::array<OccluderBatch,4> occluderRing{};
    std::atomic<uint32_t> occluderHead{0},occluderTail{0};
    std::array<Occluder, kMaxOccluders> occluderShadow{};
    uint32_t shadowCount = 0;

    std::vector<float> busBuffer[static_cast<int>(Bus::Count)];
    std::vector<float> master;
    std::vector<float> send;
    std::vector<float> lastBlock;
    float busGain[static_cast<int>(Bus::Count)]{};
    float masterGain = 1.0f;
    Listener listener{};
    Reverb reverb;
    Meter meter;

    Command ring[kRingSize]{};
    std::atomic<uint32_t> ringHead{0};
    std::atomic<uint32_t> ringTail{0};
    std::atomic<uint64_t> nowSample{0};
    // Stamped into every command so a backend that advances the clock in whole device
    // periods cannot hand the mixer a command that is already in the past.
    std::atomic<uint32_t> commandLead{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> lateCommands{0};
    std::atomic<bool> paused{false};

    uint32_t channels = 2;
    uint32_t nextVoiceId = 1; // producer only; id is stamped before queuing
    uint32_t started = 0;
    uint32_t steals = 0;
    uint32_t physicalCount = 0;
    uint32_t virtualCount = 0;
    uint64_t blocks = 0;
    uint64_t pcmHash = 0xcbf29ce484222325ull;
    uint64_t framesRendered = 0;
    float lastPeak = 0.0f;
    // `meter.peak` is cleared per block because it drives a live meter; this keeps the
    // session maximum so a gate can assert on it after the run.
    float peakMax = 0.0f;
    float lastRms = 0.0f;
    float lastCpuMs = 0.0f;
    int objectSlots[32]{};
    bool configured = false;
    uint64_t nextTraceBlock = 0;

    Impl() {
        for (int i = 0; i < 32; ++i) objectSlots[i] = -1;
    }

    // ---- ring -------------------------------------------------------------
    bool push(const Command& command) {
        const uint32_t head = ringHead.load(std::memory_order_relaxed);
        const uint32_t tail = ringTail.load(std::memory_order_acquire);
        if (head - tail >= kRingSize) return false;
        ring[head % kRingSize] = command;
        ringHead.store(head + 1, std::memory_order_release);
        return true;
    }
    bool peekTarget(uint64_t& target) const {
        const uint32_t tail = ringTail.load(std::memory_order_relaxed);
        const uint32_t head = ringHead.load(std::memory_order_acquire);
        if (tail == head) return false;
        target = ring[tail % kRingSize].targetSample;
        return true;
    }
    bool pop(Command& command) {
        const uint32_t tail = ringTail.load(std::memory_order_relaxed);
        const uint32_t head = ringHead.load(std::memory_order_acquire);
        if (tail == head) return false;
        command = ring[tail % kRingSize];
        ringTail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ---- setup ------------------------------------------------------------
    void configureBuffers() {
        channels = layoutChannels(cfg.layout);
        if (cfg.quality == Quality::Binaural) {
            cfg.layout = Layout::Stereo;
            channels = 2;
        }
        for (auto& buffer : busBuffer)
            buffer.assign(static_cast<size_t>(channels) * cfg.blockFrames, 0.0f);
        master.assign(static_cast<size_t>(channels) * cfg.blockFrames, 0.0f);
        send.assign(static_cast<size_t>(channels) * cfg.blockFrames, 0.0f);
        lastBlock.assign(static_cast<size_t>(channels) * cfg.blockFrames, 0.0f);
        voices.assign(cfg.maxPhysicalVoices + cfg.maxVirtualVoices, Voice{});
        binaural.assign(cfg.maxPhysicalVoices, BinauralState{});
        for (int i = 0; i < static_cast<int>(Bus::Count); ++i) busGain[i] = 1.0f;
        masterGain = dbToGain(cfg.masterGainDb);
        busGain[static_cast<int>(Bus::Ui)] = dbToGain(-2.0f);
        // Build the VBAP tables on the configuring thread; the render path must not
        // lazily construct them while the device thread is running.
        float probe[kMaxSpeakers]{};
        panDirection({0.0f, 0.0f, 1.0f}, cfg.layout, probe, channels);
        reverb.configure(static_cast<float>(cfg.sampleRate));
        reverb.wet = 0.0f;
        meter.configure(channels);
    }

    // ---- listener / commands ---------------------------------------------
    void applyCommands(uint64_t upTo) {
        Command command;
        while (peekTarget(command.targetSample) && command.targetSample <= upTo) {
            if (!pop(command)) break;
            if (command.targetSample + 1 < nowSample.load(std::memory_order_relaxed))
                lateCommands.fetch_add(1, std::memory_order_relaxed);
            switch (command.type) {
                case Command::Play: startVoice(command.id, command.params, command.voiceId); break;
                case Command::VoicePosition:
                    for(auto& voice:voices)if(voice.active&&voice.id==command.id)voice.position=command.params.position;
                    break;
                case Command::Stop: stopVoice(command.id, command.value); break;
                case Command::StopAll: stopAllVoices(command.value); break;
                case Command::ListenerUpdate: listener = command.listener; break;
                case Command::BusGain:
                    if (command.bus < static_cast<uint32_t>(Bus::Count))
                        busGain[command.bus] = dbToGain(command.value);
                    break;
                case Command::MasterGain: masterGain = dbToGain(command.value); break;
                case Command::Pause: paused.store(command.value > 0.5f); break;
                default: break;
            }
        }
    }

    void syncOccluders() {
        auto tail=occluderTail.load(std::memory_order_relaxed);const auto head=occluderHead.load(std::memory_order_acquire);
        while(tail!=head){const auto& batch=occluderRing[tail%occluderRing.size()];shadowCount=batch.count;
            std::copy_n(batch.boxes.begin(),shadowCount,occluderShadow.begin());++tail;}
        occluderTail.store(tail,std::memory_order_release);
    }

    // ---- voice lifecycle --------------------------------------------------
    float voiceScore(const Voice& voice) const {
        const float gain = std::max(voice.env, 0.0f);
        return voice.priority * 8.0f + gain * 60.0f - voice.distance * 1.5f;
    }

    bool isProtected(const Voice& voice) const {
        return voice.bus == Bus::Music || voice.bus == Bus::Voice;
    }

    int findFreePhysical() const {
        for (uint32_t i = 0; i < cfg.maxPhysicalVoices; ++i)
            if (!voices[i].active) return static_cast<int>(i);
        return -1;
    }

    void startVoice(uint32_t soundHandle, const PlayParams& params, uint32_t voiceId) {
        if (soundHandle >= sounds.size()) return;
        const Sound& sound = *sounds[soundHandle];
        if (sound.samples.empty()) return;
        int slot = findFreePhysical();
        if (slot < 0) {
            // Steal by fading the lowest-scoring unprotected voice out in place. The new
            // voice waits in a virtual slot and is promoted once the fade releases a slot,
            // so the hand-off is a 20 ms crossfade instead of a click.
            int worst = -1;
            float worstScore = std::numeric_limits<float>::max();
            for (uint32_t i = 0; i < cfg.maxPhysicalVoices; ++i) {
                const Voice& voice = voices[i];
                if (!voice.active || isProtected(voice) || voice.stopping) continue;
                const float score = voiceScore(voice);
                if (score < worstScore) { worstScore = score; worst = static_cast<int>(i); }
            }
            if (worst >= 0) {
                voices[worst].envTarget = 0.0f;
                voices[worst].envStep = 1.0f / (0.02f * cfg.sampleRate);
                voices[worst].stopping = true;
                ++steals;
            }
            for (uint32_t i = cfg.maxPhysicalVoices; i < voices.size(); ++i) {
                if (!voices[i].active) { slot = static_cast<int>(i); break; }
            }
            if (slot < 0) return;
        }
        Voice& voice = voices[slot];
        voice = Voice{};
        voice.active = true;
        voice.physical = slot < static_cast<int>(cfg.maxPhysicalVoices);
        voice.id = voiceId;
        voice.sound = soundHandle;
        voice.bus = params.bus;
        voice.loop = params.loop || (!params.overrideLoop && sound.loop);
        voice.spatial = params.spatial && cfg.quality != Quality::Stereo;
        voice.object = voice.spatial && params.object && cfg.quality == Quality::Atmos;
        voice.position = params.position;
        // Loudness normalisation: the bank records each asset's measured LUFS, so a
        // voice starts at the target level instead of at whatever the file happened to
        // be mastered at. Without this a -25 LUFS effect next to a -16 LUFS bed is
        // inaudible no matter how the bus gains are set. A sound that declares no
        // measurement (loudnessLufs <= -69) is left at its file level.
        float normaliseDb = 0.0f;
        if (sound.loudnessLufs > -69.0f && cfg.loudnessTargetLufs > -69.0f) {
            normaliseDb = cfg.loudnessTargetLufs - sound.loudnessLufs;
            normaliseDb = std::clamp(normaliseDb, -24.0f, 24.0f);
        }
        voice.baseGain = dbToGain(params.gainDb + normaliseDb);
        voice.priority = static_cast<float>(params.priority);
        voice.minDistance = std::max(0.01f, params.minDistance);
        voice.maxDistance = std::max(voice.minDistance + 0.01f, params.maxDistance);
        voice.occlusion = clampf(params.occlusion, 0.0f, 1.0f);
        voice.playhead = 0.0;
        voice.env = voice.physical ? 0.0f : 1.0f;
        voice.envTarget = 1.0f;
        voice.envStep = 1.0f / (0.02f * cfg.sampleRate);
        voice.gainCurrent = 1.0f;
        voice.cutoffCurrent = 20000.0f;
        voice.cutoffTarget = 20000.0f;
        voice.distance = params.spatial ? std::sqrt(dot(params.position - listener.position,
                                                        params.position - listener.position)) : 0.0f;
        if (voice.physical) {
            BinauralState& state = binaural[slot];
            state = BinauralState{};
            state.primed = true;
            ++physicalCount;
        } else {
            ++virtualCount;
        }
        ++started;
        if (voice.object) {
            for (int i = 0; i < 32; ++i) {
                if (objectSlots[i] < 0) { objectSlots[i] = static_cast<int>(voice.id); voice.objectSlot = i; break; }
            }
            if (voice.objectSlot < 0) voice.object = false;
        }
        voice.pitch = std::max(0.1f, params.pitch);
        voice.pitchEffective = voice.pitch;
    }

    void releaseVoice(Voice& voice) {
        if (!voice.active) return;
        if (voice.physical) {
            if (physicalCount > 0) --physicalCount;
            const uint32_t slot = static_cast<uint32_t>(&voice - voices.data());
            if (slot < binaural.size()) binaural[slot] = BinauralState{};
        } else if (virtualCount > 0) {
            --virtualCount;
        }
        if (voice.objectSlot >= 0) objectSlots[voice.objectSlot] = -1;
        voice.active = false;
        voice.objectSlot = -1;
    }

    void stopVoice(uint32_t voiceId, float fadeSeconds) {
        for (auto& voice : voices) {
            if (voice.active && voice.id == voiceId) {
                voice.envTarget = 0.0f;
                voice.envStep = 1.0f / std::max(1.0f, fadeSeconds * cfg.sampleRate);
            }
        }
    }
    void stopAllVoices(float fadeSeconds) {
        for (auto& voice : voices) {
            if (!voice.active) continue;
            voice.envTarget = 0.0f;
            voice.envStep = 1.0f / std::max(1.0f, fadeSeconds * cfg.sampleRate);
        }
    }

    // ---- occlusion --------------------------------------------------------
    bool rayBox(const Vec3& origin, const Vec3& direction, const Vec3& min, const Vec3& max,
                float maxDistance, float& hitDistance) const {
        float tmin = 0.0f, tmax = maxDistance;
        for (int axis = 0; axis < 3; ++axis) {
            const float o = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
            const float d = axis == 0 ? direction.x : axis == 1 ? direction.y : direction.z;
            const float lo = axis == 0 ? min.x : axis == 1 ? min.y : min.z;
            const float hi = axis == 0 ? max.x : axis == 1 ? max.y : max.z;
            if (std::abs(d) < 1e-6f) {
                if (o < lo || o > hi) return false;
                continue;
            }
            float t1 = (lo - o) / d;
            float t2 = (hi - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
        hitDistance = tmin;
        return tmin <= maxDistance;
    }

    void traceVoice(Voice& voice, uint64_t block) {
        if (voice.occlusion <= 0.0f || !voice.spatial) {
            voice.gainTarget = 1.0f;
            voice.cutoffTarget = 20000.0f;
            voice.occluded = false;
            return;
        }
        const uint32_t count = shadowCount;
        if (count == 0) {
            voice.gainTarget = 1.0f;
            voice.cutoffTarget = 20000.0f;
            voice.occluded = false;
            return;
        }
        const Vec3 toSource = voice.position - listener.position;
        const float distance = std::sqrt(dot(toSource, toSource));
        if (distance < 0.05f || distance > 60.0f) {
            voice.gainTarget = 1.0f;
            voice.cutoffTarget = 20000.0f;
            voice.occluded = false;
            return;
        }
        const Vec3 direction = toSource / distance;
        float nearest = distance;
        int hitIndex = -1;
        for (uint32_t i = 0; i < count && i < kMaxOccluders; ++i) {
            float t = 0.0f;
            if (rayBox(listener.position, direction, occluderShadow[i].min, occluderShadow[i].max,
                       nearest, t)) {
                if (t < nearest) { nearest = t; hitIndex = static_cast<int>(i); }
            }
        }
        if (hitIndex >= 0) {
            const Occluder& hit = occluderShadow[hitIndex];
            voice.gainTarget = dbToGain(hit.gainDb * voice.occlusion);
            voice.cutoffTarget = std::max(120.0f, hit.cutoffHz);
            voice.occluded = true;
        } else {
            voice.gainTarget = 1.0f;
            voice.cutoffTarget = 20000.0f;
            voice.occluded = false;
        }
        voice.lastTraceBlock = static_cast<uint32_t>(block);
    }

    // ---- render -----------------------------------------------------------
    float sampleVoice(const Voice& voice, const Sound& sound, double& playhead, float pitch) const {
        const uint32_t frames = static_cast<uint32_t>(sound.samples.size() / sound.channels);
        if (frames == 0) return 0.0f;
        double position = playhead;
        if (voice.loop) {
            position = std::fmod(position, static_cast<double>(frames));
            if (position < 0.0) position += frames;
        } else if (position >= frames) {
            return 0.0f;
        }
        const uint32_t index = static_cast<uint32_t>(position);
        const float frac = static_cast<float>(position - index);
        const uint32_t next = voice.loop ? (index + 1) % frames : std::min(index + 1, frames - 1);
        float value = 0.0f;
        if (sound.channels == 1) {
            const float a = sound.samples[index];
            const float b = sound.samples[next];
            value = a + (b - a) * frac;
        } else {
            const float a = 0.5f * (sound.samples[static_cast<size_t>(index) * 2] +
                                    sound.samples[static_cast<size_t>(index) * 2 + 1]);
            const float b = 0.5f * (sound.samples[static_cast<size_t>(next) * 2] +
                                    sound.samples[static_cast<size_t>(next) * 2 + 1]);
            value = a + (b - a) * frac;
        }
        playhead += static_cast<double>(pitch) * sound.sampleRate / cfg.sampleRate;
        return value;
    }

    void renderChunk(float* out, uint32_t frames) {
        const uint32_t channelsNow = channels;
        for (int bus = 0; bus < static_cast<int>(Bus::Count); ++bus)
            std::fill(busBuffer[bus].begin(), busBuffer[bus].begin() + static_cast<size_t>(channelsNow) * frames, 0.0f);
        std::fill(master.begin(), master.begin() + static_cast<size_t>(channelsNow) * frames, 0.0f);
        std::fill(send.begin(), send.begin() + static_cast<size_t>(channelsNow) * frames, 0.0f);

        const float smoothing = 1.0f - std::exp(-static_cast<float>(frames) /
                                                (0.08f * static_cast<float>(cfg.sampleRate)));
        const Vec3 forward = normalize(listener.forward);
        const Vec3 right = normalize(cross(listener.up, forward));
        const Vec3 up = normalize(cross(forward, right));
        const uint64_t blockIndex = blocks;

        for (uint32_t i = 0; i < voices.size(); ++i) {
            Voice& voice = voices[i];
            if (!voice.active) continue;
            const Sound& sound = *sounds[voice.sound];
            if (voice.physical) {
                if ((blockIndex + i) % 8 == 0) traceVoice(voice, blockIndex);
                voice.gainCurrent += (voice.gainTarget - voice.gainCurrent) * smoothing;
                voice.cutoffCurrent += (voice.cutoffTarget - voice.cutoffCurrent) * smoothing;
                if (voice.spatial) {
                    const Vec3 delta = voice.position - listener.position;
                    voice.distance = std::sqrt(dot(delta, delta));
                    const Vec3 direction = normalize(delta);
                    voice.pitchEffective = voice.pitch *
                        dopplerRatio(voice.velocity, listener.velocity, direction);
                    if (cfg.quality == Quality::Binaural) {
                        const float azimuth = std::atan2(dot(direction, right), dot(direction, forward));
                        const float elevation = std::asin(clampf(dot(direction, up), -1.0f, 1.0f));
                        const float itd = hrtfItdSeconds(azimuth) * static_cast<float>(cfg.sampleRate);
                        BinauralState& state = binaural[i];
                        const float lateral = std::sin(azimuth);
                        state.shadowLeft.setCutoff(hrtfShadowCutoff(-lateral), static_cast<float>(cfg.sampleRate));
                        state.shadowRight.setCutoff(hrtfShadowCutoff(lateral), static_cast<float>(cfg.sampleRate));
                        voice.itd = itd;
                        voice.elevationGain = 1.0f + 0.15f * std::sin(elevation);
                        // The one-pole shadow is weak at low frequencies; a broadband
                        // contralateral trim keeps 1 kHz ILD in the 3..6 dB range.
                        voice.shadowGainLeft = 1.0f - 0.42f * std::max(0.0f, lateral);
                        voice.shadowGainRight = 1.0f - 0.42f * std::max(0.0f, -lateral);
                    } else {
                        panDirection(direction, cfg.layout, voice.pan, channelsNow);
                    }
                }
                voice.lowpass.setCutoff(voice.cutoffCurrent, static_cast<float>(cfg.sampleRate));
            }
            if (!voice.physical) {
                // Virtual voice: keep time and distance moving so promotion resumes in place.
                const Vec3 delta = voice.position - listener.position;
                voice.distance = std::sqrt(dot(delta, delta));
                const double rate = static_cast<double>(std::max(0.1f, voice.pitch)) *
                                    sound.sampleRate / cfg.sampleRate;
                voice.playhead += rate * frames;
                const uint32_t total = static_cast<uint32_t>(sound.samples.size() / sound.channels);
                if (!voice.loop && voice.playhead >= total) releaseVoice(voice);
                continue;
            }

            const float distanceGain = voice.spatial ? distanceAttenuation(voice) : 1.0f;
            const float gain = voice.baseGain * distanceGain * voice.gainCurrent * masterGain *
                               busGain[static_cast<int>(voice.bus)];
            float* bus = busBuffer[static_cast<int>(voice.bus)].data();
            const float pitch = std::max(0.1f, voice.pitchEffective);
            BinauralState* state = voice.physical ? &binaural[i] : nullptr;
            for (uint32_t f = 0; f < frames; ++f) {
                float sample = sampleVoice(voice, sound, voice.playhead, pitch);
                if (voice.cutoffCurrent < 19900.0f) sample = voice.lowpass.process(sample);
                if (voice.env < voice.envTarget) voice.env = std::min(voice.envTarget, voice.env + voice.envStep);
                else if (voice.env > voice.envTarget) voice.env = std::max(voice.envTarget, voice.env - voice.envStep);
                sample *= gain * voice.env;
                if (!voice.spatial || cfg.quality == Quality::Stereo) {
                    bus[static_cast<size_t>(f) * channelsNow] += sample * 0.7071f;
                    if (channelsNow > 1) bus[static_cast<size_t>(f) * channelsNow + 1] += sample * 0.7071f;
                } else if (cfg.quality == Quality::Binaural && state) {
                    const float itd = voice.itd;
                    const float elevationGain = voice.elevationGain;
                    state->delayLeft[state->write] = sample;
                    state->delayRight[state->write] = sample;
                    const float readLeft = static_cast<float>(state->write) - std::max(0.0f, itd);
                    const float readRight = static_cast<float>(state->write) - std::max(0.0f, -itd);
                    auto read = [&](const float* line, float position) {
                        float wrapped = std::fmod(position, static_cast<float>(kHrtfDelay));
                        if (wrapped < 0.0f) wrapped += kHrtfDelay;
                        const uint32_t i0 = static_cast<uint32_t>(wrapped);
                        const uint32_t i1 = (i0 + 1) % kHrtfDelay;
                        const float frac = wrapped - static_cast<float>(i0);
                        return line[i0] + (line[i1] - line[i0]) * frac;
                    };
                    const float left = state->shadowLeft.process(read(state->delayLeft, readLeft));
                    const float rightEar = state->shadowRight.process(read(state->delayRight, readRight));
                    state->write = (state->write + 1) % kHrtfDelay;
                    bus[static_cast<size_t>(f) * 2] += left * elevationGain * voice.shadowGainLeft;
                    bus[static_cast<size_t>(f) * 2 + 1] += rightEar * elevationGain * voice.shadowGainRight;
                } else {
                    for (uint32_t ch = 0; ch < channelsNow; ++ch)
                        bus[static_cast<size_t>(f) * channelsNow + ch] += sample * voice.pan[ch];
                }
            }
            // Occlusion also drives the reverb send: a blocked source is heard mostly
            // through the room, which is what makes "behind a wall" read as distance.
            const float sendAmount = voice.spatial ? (0.05f + 0.25f * (1.0f - voice.gainCurrent)) : 0.0f;
            if (sendAmount > 0.0f) {
                float* target = send.data();
                for (uint32_t f = 0; f < frames; ++f) {
                    const float mono = bus[static_cast<size_t>(f) * channelsNow];
                    target[static_cast<size_t>(f) * channelsNow] += mono * sendAmount;
                }
            }
            const uint32_t total = static_cast<uint32_t>(sound.samples.size() / sound.channels);
            if (!voice.loop && voice.playhead >= total && voice.envTarget > 0.0f) {
                voice.envTarget = 0.0f;
                voice.envStep = 1.0f / (0.01f * cfg.sampleRate);
            }
            if (voice.env <= 0.0001f && voice.envTarget <= 0.0f) releaseVoice(voice);
        }

        // Reverb send.
        if (!send.empty()) {
            float* bus = busBuffer[static_cast<int>(Bus::Master)].data();
            for (uint32_t f = 0; f < frames; ++f) {
                const float input = send[static_cast<size_t>(f) * channelsNow];
                const float wet = reverb.process(input);
                bus[static_cast<size_t>(f) * channelsNow] += wet * 0.6f;
                if (channelsNow > 1) bus[static_cast<size_t>(f) * channelsNow + 1] += wet * 0.6f;
            }
        }

        // Sum buses into master with a soft limiter at -1 dBFS.
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t ch = 0; ch < channelsNow; ++ch) {
                float sum = 0.0f;
                for (int bus = 0; bus < static_cast<int>(Bus::Count); ++bus)
                    sum += busBuffer[bus][static_cast<size_t>(f) * channelsNow + ch] *
                           (bus == static_cast<int>(Bus::Master) ? 1.0f : 1.0f);
                const float magnitude = std::abs(sum);
                if (magnitude > 0.89f) sum = (sum > 0.0f ? 1.0f : -1.0f) * (0.89f + (magnitude - 0.89f) * 0.12f);
                master[static_cast<size_t>(f) * channelsNow + ch] = sum;
            }
        }
        std::memcpy(out, master.data(), sizeof(float) * frames * channelsNow);
        std::memcpy(lastBlock.data(), master.data(), sizeof(float) * frames * channelsNow);
        meter.push(master.data(), frames, channelsNow, 3);
        lastPeak = meter.peak;
        lastRms = meter.rms;
        peakMax = std::max(peakMax, lastPeak);
        meter.peak = 0.0f;
        // FNV-1a over quantised output: the audio-side determinism digest.
        for (size_t i = 0; i < static_cast<size_t>(frames) * channelsNow; ++i) {
            const int16_t quantised = static_cast<int16_t>(clampf(master[i], -1.0f, 1.0f) * 32767.0f);
            pcmHash ^= static_cast<uint8_t>(quantised & 0xff);
            pcmHash *= 0x100000001b3ull;
            pcmHash ^= static_cast<uint8_t>((quantised >> 8) & 0xff);
            pcmHash *= 0x100000001b3ull;
        }
        framesRendered += frames;
        ++blocks;
    }

    float distanceAttenuation(const Voice& voice) const {
        const float d = voice.distance;
        if (d >= voice.maxDistance) return 0.0f;
        float gain = voice.minDistance / std::max(voice.minDistance, d);
        if (d > voice.maxDistance * 0.8f) {
            const float t = (d - voice.maxDistance * 0.8f) / (voice.maxDistance * 0.2f);
            gain *= 1.0f - t * t * (3.0f - 2.0f * t);
        }
        return gain;
    }

    void promoteVirtual() {
        while (physicalCount < cfg.maxPhysicalVoices) {
            int best = -1;
            float bestScore = -std::numeric_limits<float>::max();
            for (uint32_t i = cfg.maxPhysicalVoices; i < voices.size(); ++i) {
                Voice& voice = voices[i];
                if (!voice.active) continue;
                const float score = voiceScore(voice);
                if (score > bestScore) { bestScore = score; best = static_cast<int>(i); }
            }
            if (best < 0) return;
            Voice promoted = voices[best];
            voices[best] = Voice{};
            if (virtualCount > 0) --virtualCount;
            int slot = findFreePhysical();
            if (slot < 0) { voices[best] = promoted; return; }
            Voice& target = voices[slot];
            target = promoted;
            target.physical = true;
            target.env = 0.0f;
            target.envTarget = 1.0f;
            target.envStep = 1.0f / (0.02f * cfg.sampleRate);
            binaural[slot] = BinauralState{};
            binaural[slot].primed = true;
            ++physicalCount;
            if (target.object) {
                for (int i = 0; i < 32; ++i)
                    if (objectSlots[i] < 0) { objectSlots[i] = static_cast<int>(target.id); target.objectSlot = i; break; }
            }
        }
    }

    void render(float* out, uint32_t frames) {
        uint32_t done = 0;
        while (done < frames) {
            const uint64_t at = nowSample.load(std::memory_order_relaxed) + done;
            applyCommands(at);
            uint32_t chunk = frames - done;
            uint64_t next = 0;
            if (peekTarget(next) && next > at && next < at + chunk)
                chunk = static_cast<uint32_t>(next - at);
            if (paused.load(std::memory_order_relaxed)) {
                std::fill(out + static_cast<size_t>(done) * channels,
                          out + static_cast<size_t>(done + chunk) * channels, 0.0f);
                meter.push(out + static_cast<size_t>(done) * channels, chunk, channels, 3);
            } else {
                promoteVirtual();
                renderChunk(out + static_cast<size_t>(done) * channels, chunk);
            }
            done += chunk;
        }
        nowSample.fetch_add(frames, std::memory_order_relaxed);
        syncOccluders();
    }
};

// ---- Engine ------------------------------------------------------------------

Engine::Engine() : impl_(new Impl()) {}
Engine::~Engine() = default;
void Engine::reset() { impl_.reset(new Impl()); }

bool Engine::configure(const Config& config, std::string& error) {
    if (impl_->configured) { error = "audio engine already configured"; return false; }
    if (config.sampleRate != kSampleRate) { error = "audio engine requires 48 kHz"; return false; }
    if (config.blockFrames < 32 || config.blockFrames > 4096) { error = "block size out of range"; return false; }
    if (config.quality == Quality::Off) { error = "audio quality is off"; return false; }
    if (config.maxPhysicalVoices == 0 || config.maxPhysicalVoices > 512 ||
        config.maxVirtualVoices > kMaxVoices) { error = "voice budget out of range"; return false; }
    impl_->cfg = config;
    impl_->commandLead.store(config.commandLeadSamples, std::memory_order_relaxed);
    if (impl_->cfg.quality == Quality::Stereo) impl_->cfg.layout = Layout::Stereo;
    if (impl_->cfg.quality == Quality::Binaural) impl_->cfg.layout = Layout::Stereo;
    if (impl_->cfg.quality == Quality::Spatial && impl_->cfg.layout == Layout::Stereo)
        impl_->cfg.layout = Layout::Surround51;
    impl_->configureBuffers();
    impl_->configured = true;
    return true;
}

const Config& Engine::config() const { return impl_->cfg; }

void Engine::setCommandLead(uint32_t samples) {
    impl_->commandLead.store(samples, std::memory_order_relaxed);
    impl_->cfg.commandLeadSamples = samples;
}

uint32_t Engine::addSound(std::shared_ptr<const Sound> sound) {
    if (!sound) return 0xffffffffu;
    impl_->sounds.push_back(std::move(sound));
    return static_cast<uint32_t>(impl_->sounds.size() - 1);
}

uint32_t Engine::findSound(const std::string& name) const {
    for (size_t i = 0; i < impl_->sounds.size(); ++i) {
        if (impl_->sounds[i] && impl_->sounds[i]->name == name) return static_cast<uint32_t>(i);
    }
    return 0xffffffffu;
}

uint32_t Engine::soundCount() const { return static_cast<uint32_t>(impl_->sounds.size()); }

void Engine::setListener(const Listener& listener) {
    Command command;
    command.type = Command::ListenerUpdate;
    command.listener = listener;
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    impl_->push(command);
}

void Engine::setBusGain(Bus bus, float db) {
    Command command;
    command.type = Command::BusGain;
    command.bus = static_cast<uint32_t>(bus);
    command.value = db;
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    impl_->push(command);
}

void Engine::setMasterGain(float db) {
    Command command;
    command.type = Command::MasterGain;
    command.value = db;
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    impl_->push(command);
}

void Engine::setOccluders(std::vector<Occluder> occluders) {
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(occluders.size(), kMaxOccluders));
    const auto head=impl_->occluderHead.load(std::memory_order_relaxed),tail=impl_->occluderTail.load(std::memory_order_acquire);
    if(head-tail>=impl_->occluderRing.size())return; // producer retries next frame
    auto& batch=impl_->occluderRing[head%impl_->occluderRing.size()];batch.count=count;
    std::copy_n(occluders.begin(),count,batch.boxes.begin());
    impl_->occluderHead.store(head+1,std::memory_order_release);
}

void Engine::setPaused(bool paused) {
    Command command;
    command.type = Command::Pause;
    command.value = paused ? 1.0f : 0.0f;
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    impl_->push(command);
}

uint32_t Engine::play(uint32_t sound, const PlayParams& params) {
    if (sound >= impl_->sounds.size()) return 0;
    Command command;
    command.type = Command::Play;
    command.id = sound;
    if(!impl_->nextVoiceId)++impl_->nextVoiceId;
    command.voiceId=impl_->nextVoiceId++;
    command.params = params;
    command.params.position = params.position;
    // The command lands at the next block boundary unless the caller queued it
    // earlier; `targetSample` is what makes scheduling frame-rate independent.
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    return impl_->push(command)?command.voiceId:0;
}

bool Engine::setVoicePosition(uint32_t voice, Vec3 position) {
    if(!voice||!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z))return false;
    Command command;command.type=Command::VoicePosition;command.id=voice;command.params.position=position;
    command.targetSample=impl_->nowSample.load(std::memory_order_relaxed)+impl_->commandLead.load(std::memory_order_relaxed);
    return impl_->push(command);
}

void Engine::stop(uint32_t voice, float fadeSeconds) {
    Command command;
    command.type = Command::Stop;
    command.id = voice;
    command.value = fadeSeconds;
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    impl_->push(command);
}

void Engine::stopAll(float fadeSeconds) {
    Command command;
    command.type = Command::StopAll;
    command.value = fadeSeconds;
    command.targetSample = impl_->nowSample.load(std::memory_order_relaxed) +
                           impl_->commandLead.load(std::memory_order_relaxed);
    impl_->push(command);
}

void Engine::render(float* out, uint32_t frames) {
    // The device hands us a whole period (often 4-8 mixer blocks). Split it so every
    // internal buffer stays block-sized; the command ring still applies at exact
    // sample positions across the split.
    const auto begin = std::chrono::steady_clock::now();
    const uint32_t block = std::max<uint32_t>(32, impl_->cfg.blockFrames);
    const uint32_t channels = impl_->channels;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t chunk = std::min(block, frames - done);
        impl_->render(out + static_cast<size_t>(done) * channels, chunk);
        done += chunk;
    }
    const auto end = std::chrono::steady_clock::now();
    impl_->lastCpuMs = std::chrono::duration<double, std::milli>(end - begin).count();
}

std::vector<float> Engine::renderOffline(float seconds) {
    const uint32_t frames = static_cast<uint32_t>(seconds * kSampleRate);
    std::vector<float> out(static_cast<size_t>(frames) * impl_->channels, 0.0f);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t chunk = std::min(impl_->cfg.blockFrames, frames - done);
        impl_->render(out.data() + static_cast<size_t>(done) * impl_->channels, chunk);
        done += chunk;
    }
    return out;
}

Stats Engine::stats() const {
    Stats stats;
    stats.physical = impl_->physicalCount;
    stats.virtualVoices = impl_->virtualCount;
    stats.objects = 0;
    for (int slot : impl_->objectSlots) if (slot >= 0) ++stats.objects;
    stats.started = impl_->started;
    stats.steals = impl_->steals;
    stats.blocks = impl_->blocks;
    stats.underruns = impl_->underruns.load(std::memory_order_relaxed);
    stats.lateCommands = impl_->lateCommands.load(std::memory_order_relaxed);
    stats.peak = impl_->lastPeak;
    stats.peakMax = impl_->peakMax;
    stats.rms = impl_->lastRms;
    stats.lufsShort = impl_->meter.shortTerm();
    stats.lufsShortMax = impl_->meter.shortTermMax;
    stats.lufsIntegrated = impl_->meter.integrated();
    stats.cpuMs = impl_->lastCpuMs;
    stats.pcmHash = impl_->pcmHash;
    stats.frames = impl_->framesRendered;
    return stats;
}

std::vector<VoiceReceipt> Engine::receipts() const {
    std::vector<VoiceReceipt> out;
    for (const auto& voice : impl_->voices) {
        if (!voice.active) continue;
        VoiceReceipt receipt;
        receipt.id = voice.id;
        receipt.sound = voice.sound;
        receipt.bus = voice.bus;
        receipt.physical = voice.physical;
        receipt.occluded = voice.occluded;
        receipt.object = voice.object;
        receipt.priority = static_cast<uint16_t>(voice.priority);
        receipt.distance = voice.distance;
        receipt.gainDb = gainToDb(voice.gainCurrent);
        receipt.cutoffHz = voice.cutoffCurrent;
        out.push_back(receipt);
    }
    return out;
}

std::vector<ObjectMeta> Engine::objects() const {
    std::vector<ObjectMeta> out;
    for (const auto& voice : impl_->voices) {
        if (!voice.active || !voice.object || voice.objectSlot < 0) continue;
        ObjectMeta meta;
        meta.position = voice.position - impl_->listener.position;
        meta.gainDb = gainToDb(voice.baseGain * voice.gainCurrent);
        meta.spread = 0.0f;
        meta.priority = static_cast<uint16_t>(voice.priority);
        meta.snap = 0;
        meta.binaural = impl_->cfg.quality == Quality::Binaural ? 1 : 0;
        out.push_back(meta);
    }
    return out;
}

void Engine::resetStats() {
    impl_->started = 0;
    impl_->steals = 0;
    impl_->blocks = 0;
    impl_->pcmHash = 0xcbf29ce484222325ull;
    impl_->framesRendered = 0;
    impl_->peakMax = 0.0f;
    impl_->lastPeak = 0.0f;
    impl_->meter.reset();
    impl_->underruns.store(0);
    impl_->lateCommands.store(0);
}

void Engine::noteUnderrun() { impl_->underruns.fetch_add(1, std::memory_order_relaxed); }

const std::vector<float>& Engine::lastBlock() const { return impl_->lastBlock; }

} // namespace engine::audio
