// WAV / .ngsnd decoding and the one-time resampler used at import.
//
// Everything downstream of this file assumes 48 kHz float samples: the voice path
// only interpolates for pitch, never for sample-rate conversion. That keeps the
// realtime thread free of a filter bank per voice and makes the offline renderer
// easy to reason about.

#include "audio.hpp"
#include "asset_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace engine::audio {
namespace {

constexpr uint32_t kPolyPhases = 64;
constexpr uint32_t kPolyTaps = 24;
constexpr double kPi = 3.14159265358979323846;

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void writeU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}
void writeU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
}
void writeF32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(out, bits);
}
float readF32(const uint8_t* p) {
    const uint32_t bits = readU32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Blackman-windowed sinc, normalised per phase so a DC signal passes unchanged.
struct PolyTable {
    float taps[kPolyPhases][kPolyTaps]{};
    bool ready = false;
};

const PolyTable& polyTable(double ratio) {
    // The table depends on the conversion ratio (cutoff = min(1, 1/ratio)), and the
    // repository only ever converts 22.05/44.1/48 kHz material, so two tables are
    // enough and both are built once.
    static PolyTable tables[2];
    static double ratios[2] = {0.0, 0.0};
    int slot = -1;
    for (int i = 0; i < 2; ++i) {
        if (ratios[i] == ratio) { slot = i; break; }
        if (ratios[i] == 0.0 && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;
    if (ratios[slot] != ratio || !tables[slot].ready) {
        ratios[slot] = ratio;
        PolyTable& table = tables[slot];
        const double cutoff = 0.5 * std::min(1.0, 1.0 / ratio) * 0.94;
        for (uint32_t phase = 0; phase < kPolyPhases; ++phase) {
            const double frac = static_cast<double>(phase) / kPolyPhases;
            double sum = 0.0;
            for (uint32_t tap = 0; tap < kPolyTaps; ++tap) {
                const double x = static_cast<double>(tap) - (kPolyTaps / 2 - 1) - frac;
                const double sinc = (std::abs(x) < 1e-9) ? 1.0 : std::sin(2.0 * kPi * cutoff * x) / (2.0 * kPi * cutoff * x);
                const double window = 0.42 - 0.5 * std::cos(2.0 * kPi * (tap + frac) / kPolyTaps) +
                                      0.08 * std::cos(4.0 * kPi * (tap + frac) / kPolyTaps);
                table.taps[phase][tap] = static_cast<float>(sinc * window);
                sum += table.taps[phase][tap];
            }
            if (std::abs(sum) > 1e-9) {
                for (uint32_t tap = 0; tap < kPolyTaps; ++tap)
                    table.taps[phase][tap] = static_cast<float>(table.taps[phase][tap] / sum);
            }
        }
        table.ready = true;
    }
    return tables[slot];
}

} // namespace

bool resampleTo48k(Sound& sound, std::string& error) {
    if (sound.sampleRate == kSampleRate) return true;
    if (sound.sampleRate == 0 || sound.channels == 0 || sound.channels > 2) {
        error = "unsupported source format for resampling";
        return false;
    }
    const double ratio = static_cast<double>(kSampleRate) / static_cast<double>(sound.sampleRate);
    const PolyTable& table = polyTable(ratio);
    const uint32_t inFrames = static_cast<uint32_t>(sound.samples.size() / sound.channels);
    if (inFrames == 0) { error = "empty sound"; return false; }
    const uint32_t outFrames = static_cast<uint32_t>(std::ceil(inFrames * ratio));
    std::vector<float> out(static_cast<size_t>(outFrames) * sound.channels, 0.0f);
    const int half = static_cast<int>(kPolyTaps / 2);
    for (uint32_t n = 0; n < outFrames; ++n) {
        const double position = static_cast<double>(n) / ratio;
        const int base = static_cast<int>(position);
        const double frac = position - base;
        const uint32_t phase = std::min<uint32_t>(kPolyPhases - 1,
            static_cast<uint32_t>(frac * kPolyPhases + 0.5));
        for (uint32_t ch = 0; ch < sound.channels; ++ch) {
            float sum = 0.0f;
            for (uint32_t tap = 0; tap < kPolyTaps; ++tap) {
                const int index = base - half + 1 + static_cast<int>(tap);
                if (index < 0 || index >= static_cast<int>(inFrames)) continue;
                sum += table.taps[phase][tap] *
                       sound.samples[static_cast<size_t>(index) * sound.channels + ch];
            }
            out[static_cast<size_t>(n) * sound.channels + ch] = sum;
        }
    }
    sound.samples.swap(out);
    sound.sampleRate = kSampleRate;
    return true;
}

bool loadWav(const std::string& path, Sound& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open wav: " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file: " + path;
        return false;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    const uint8_t* data = nullptr;
    uint32_t dataBytes = 0;
    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const uint32_t size = readU32(bytes.data() + offset + 4);
        const size_t body = offset + 8;
        if (body + size > bytes.size()) break;
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && size >= 16) {
            format = readU16(bytes.data() + body);
            channels = readU16(bytes.data() + body + 2);
            sampleRate = readU32(bytes.data() + body + 4);
            bits = readU16(bytes.data() + body + 14);
            if (format == 0xfffe && size >= 26) {
                // WAVE_FORMAT_EXTENSIBLE: the real format lives in the sub-format GUID.
                format = readU16(bytes.data() + body + 24);
            }
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            data = bytes.data() + body;
            dataBytes = size;
        }
        offset = body + size + (size & 1u);
    }
    if (!data || channels == 0 || channels > 2 || sampleRate == 0) {
        error = "wav is missing fmt/data or has unsupported channels: " + path;
        return false;
    }
    const uint32_t bytesPerSample = bits / 8;
    if (bytesPerSample == 0 || (format != 1 && format != 3)) {
        error = "unsupported wav encoding: " + path;
        return false;
    }
    const uint32_t frames = dataBytes / (bytesPerSample * channels);
    out.samples.assign(static_cast<size_t>(frames) * channels, 0.0f);
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.name = path;
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames * channels; ++i) {
        float value = 0.0f;
        const uint8_t* p = data + static_cast<size_t>(i) * bytesPerSample;
        if (format == 3 && bits == 32) value = readF32(p);
        else if (format == 1 && bits == 16) value = static_cast<float>(static_cast<int16_t>(readU16(p))) / 32768.0f;
        else if (format == 1 && bits == 24) {
            int32_t v = (static_cast<int32_t>(p[2]) << 24) | (static_cast<int32_t>(p[1]) << 16) |
                        (static_cast<int32_t>(p[0]) << 8);
            value = static_cast<float>(v >> 8) / 8388608.0f;
        } else if (format == 1 && bits == 32) value = static_cast<float>(static_cast<int32_t>(readU32(p))) / 2147483648.0f;
        else if (format == 1 && bits == 8) value = (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        else { error = "unsupported wav bit depth: " + path; return false; }
        out.samples[i] = value;
        peak = std::max(peak, std::abs(value));
    }
    out.peakDbtp = peak > 0.0f ? 20.0f * std::log10(peak) : -120.0f;
    if (!resampleTo48k(out, error)) return false;
    return true;
}

namespace {
constexpr char kMagic[4] = {'N', 'G', 'S', '1'};
}

bool saveNgsnd(const std::string& path, const Sound& sound, std::string& error) {
    if (sound.sampleRate != kSampleRate) { error = "ngsnd requires 48 kHz samples"; return false; }
    const uint32_t frames = static_cast<uint32_t>(sound.samples.size() / std::max<uint32_t>(1, sound.channels));
    std::vector<uint8_t> header;
    header.insert(header.end(), kMagic, kMagic + 4);
    writeU16(header, 1);                                   // version
    writeU16(header, static_cast<uint16_t>(sound.channels));
    writeU32(header, sound.sampleRate);
    writeU32(header, frames);
    writeU16(header, 2);                                   // 0=i16,1=i24,2=f32
    writeU16(header, sound.loop ? 1u : 0u);
    writeU32(header, 0);                                   // loopStart
    writeU32(header, sound.loop ? frames : 0);             // loopEnd
    writeF32(header, sound.loudnessLufs);
    writeF32(header, sound.peakDbtp);
    writeU32(header, static_cast<uint32_t>(header.size() + 8)); // dataOffset
    writeU32(header, frames * sound.channels * 4);
    std::vector<uint8_t> bytes = header;
    for (float sample : sound.samples) writeF32(bytes, sample);
    std::ofstream out(path, std::ios::binary);
    if (!out) { error = "cannot write ngsnd: " + path; return false; }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) { error = "ngsnd write failed: " + path; return false; }
    return true;
}

bool loadNgsnd(const std::string& path, Sound& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open ngsnd: " + path; return false; }
    in.seekg(0,std::ios::end);const auto size=in.tellg();
    if(size<0||size>64*1024*1024){error="ngsnd exceeds 64 MiB: "+path;return false;}in.seekg(0);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if(!decodeNgsndData(bytes,out,error)){error+=": "+path;return false;}
    out.name = path;
    return true;
}

} // namespace engine::audio
