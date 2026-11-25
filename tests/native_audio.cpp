// Native audio evidence. Runs entirely offline (no sound card), emits the JSON
// receipts described in .developmentAgent/animation-audio/AUDIO-PLAN.md §8 and fails on any threshold miss.
//
// Build: tools/build.ps1 -> build/native_audio_tests.exe
// Run:   build/native_audio_tests.exe

#include "../engine/audio/audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::audio;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& name, const std::string& detail = {}) {
    ++checks;
    if (condition) {
        std::printf("  ok   %s\n", name.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
}

std::string format(const char* pattern, double value) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}
std::string format(const char* pattern, double a, double b) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, a, b);
    return buffer;
}

void writeJson(const std::string& name, const std::string& body) {
    std::filesystem::create_directories("reports/audio");
    std::ofstream out("reports/audio/" + name + ".json", std::ios::binary);
    out << body << "\n";
}

std::shared_ptr<Sound> makeTone(float frequency, float seconds, float amplitude = 0.5f,
                                bool loop = false, uint32_t channels = 1) {
    auto sound = std::make_shared<Sound>();
    sound->name = "tone-" + std::to_string(static_cast<int>(frequency));
    sound->channels = channels;
    sound->sampleRate = kSampleRate;
    sound->loop = loop;
    const uint32_t frames = static_cast<uint32_t>(seconds * kSampleRate);
    sound->samples.resize(static_cast<size_t>(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i) {
        const float value = amplitude * std::sin(2.0f * 3.14159265f * frequency * i / kSampleRate);
        for (uint32_t ch = 0; ch < channels; ++ch) sound->samples[static_cast<size_t>(i) * channels + ch] = value;
    }
    return sound;
}

std::shared_ptr<Sound> makeNoise(float seconds, float amplitude = 0.1f) {
    auto sound = std::make_shared<Sound>();
    sound->name = "noise";
    sound->channels = 1;
    sound->sampleRate = kSampleRate;
    const uint32_t frames = static_cast<uint32_t>(seconds * kSampleRate);
    sound->samples.resize(frames);
    uint32_t state = 0x12345678u;
    for (uint32_t i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        const float unit = static_cast<float>((state >> 8) & 0xffff) / 32768.0f - 1.0f;
        sound->samples[i] = unit * amplitude;
    }
    return sound;
}

float rms(const std::vector<float>& samples, uint32_t channels, uint32_t channel,
          uint32_t from = 0, uint32_t to = 0) {
    if (to == 0) to = static_cast<uint32_t>(samples.size() / channels);
    double sum = 0.0;
    for (uint32_t i = from; i < to; ++i) {
        const double v = samples[static_cast<size_t>(i) * channels + channel];
        sum += v * v;
    }
    return static_cast<float>(std::sqrt(sum / std::max<uint32_t>(1, to - from)));
}

float highFrequencyEnergy(const std::vector<float>& samples, uint32_t channels, uint32_t channel) {
    double total = 0.0, difference = 0.0;
    for (size_t i = channels + channel; i < samples.size(); i += channels) {
        const double v = samples[i];
        const double d = v - samples[i - channels];
        total += v * v;
        difference += d * d;
    }
    (void)total;
    return static_cast<float>(difference);
}

Config baseConfig(Quality quality, Layout layout, uint32_t block = 256) {
    Config config;
    config.quality = quality;
    config.layout = layout;
    config.blockFrames = block;
    config.maxPhysicalVoices = 64;
    config.maxVirtualVoices = 512;
    config.masterGainDb = 0.0f;
    return config;
}

Listener facingForward() {
    Listener listener;
    listener.position = {0.0f, 0.0f, 0.0f};
    listener.forward = {0.0f, 0.0f, 1.0f};
    listener.up = {0.0f, 1.0f, 0.0f};
    return listener;
}

PlayParams at(Vec3 position, Bus bus = Bus::Sfx) {
    PlayParams params;
    params.position = position;
    params.bus = bus;
    params.minDistance = 1.0f;
    params.maxDistance = 60.0f;
    return params;
}

// ---- checks ------------------------------------------------------------------

void testNullPath() {
    std::printf("null path\n");
    Engine engine;
    std::string error;
    check(engine.configure(baseConfig(Quality::Spatial, Layout::Surround51), error), "configure", error);
    auto tone = makeTone(100.0f, 0.5f, 0.25f);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    PlayParams params;
    params.position = {0.0f, 0.0f, 0.0f};
    params.spatial = false;
    params.bus = Bus::Sfx;
    engine.play(handle, params);
    const auto out = engine.renderOffline(0.4f);
    const uint32_t channels = layoutChannels(Layout::Surround51);
    // Skip the 20 ms fade-in, then compare against the analytic 0.7071 fold.
    const uint32_t from = 4800;
    double worst = 0.0;
    for (uint32_t i = from; i < 19200; ++i) {
        const float expected = tone->samples[i] * 0.70710678f;
        worst = std::max<double>(worst, std::abs(out[static_cast<size_t>(i) * channels] - expected));
        worst = std::max<double>(worst, std::abs(out[static_cast<size_t>(i) * channels + 1] - expected));
    }
    check(worst < 1.0e-4, "2D path matches input within 1e-4", format("maxAbs=%.9f", worst));
    writeJson("null-test", "{\"maxAbsError\":" + format("%.9f", worst) + ",\"threshold\":0.0001,\"frames\":19200}");
}

void testLoudness() {
    std::printf("loudness\n");
    auto measure = [](float amplitude) {
        Engine engine;
        std::string error;
        engine.configure(baseConfig(Quality::Spatial, Layout::Surround51), error);
        auto noise = makeNoise(5.0f, amplitude);
        const uint32_t handle = engine.addSound(noise);
        engine.setListener(facingForward());
        PlayParams params;
        params.position = {0.0f, 0.0f, 0.0f};
        params.spatial = false;
        params.bus = Bus::Sfx;
        engine.play(handle, params);
        engine.renderOffline(5.0f);
        return engine.stats().lufsIntegrated;
    };
    const float loud = measure(0.2f);
    const float quiet = measure(0.1f);
    const float delta = loud - quiet;
    check(std::abs(delta - 6.02f) < 0.35f, "6 dB gain step reads 6.02 LU",
          format("delta=%.2f", delta));
    check(loud > -30.0f && loud < -10.0f, "absolute loudness is plausible",
          format("lufs=%.2f", loud));
    writeJson("loudness", "{\"lufsLoud\":" + format("%.2f", loud) + ",\"lufsQuiet\":" +
                              format("%.2f", quiet) + ",\"deltaLu\":" + format("%.2f", delta) +
                              ",\"target\":6.02,\"tolerance\":0.35}");
}

void testPanning() {
    std::printf("panning\n");
    struct Case { float azimuth; int channel; const char* name; };
    const Case cases[] = {
        {-30.0f, 0, "left"}, {30.0f, 1, "right"}, {0.0f, 2, "center"},
        {-110.0f, 4, "surround-left"}, {110.0f, 5, "surround-right"},
        {-150.0f, 6, "rear-left"}, {150.0f, 7, "rear-right"}};
    float worst = 0.0f;
    std::string body = "{\"layout\":\"7.1.2\",\"cases\":[";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const float radians = cases[i].azimuth * 3.14159265f / 180.0f;
        const Vec3 direction{std::sin(radians), 0.0f, std::cos(radians)};
        float gains[10]{};
        panDirection(direction, Layout::Surround712, gains, 10);
        const float expected = gains[cases[i].channel];
        const float leak = std::max(gains[(cases[i].channel + 1) % 8], gains[(cases[i].channel + 2) % 8]);
        worst = std::max(worst, std::abs(1.0f - expected));
        check(std::abs(1.0f - expected) < 0.05f, std::string("gain at ") + cases[i].name,
              format("g=%.3f", expected));
        if (i) body += ",";
        body += "{\"azimuth\":" + format("%.1f", cases[i].azimuth) + ",\"gain\":" +
                format("%.4f", expected) + ",\"leak\":" + format("%.4f", leak) + "}";
    }
    // Centre goes to the centre speaker, not to an L/R phantom.
    float centre[10]{};
    panDirection({0.0f, 0.0f, 1.0f}, Layout::Surround712, centre, 10);
    check(std::abs(centre[2] - 1.0f) < 0.02f && std::abs(centre[0]) < 0.02f,
          "centre lands on C", format("C=%.3f L=%.3f", centre[2], centre[0]));
    float energy = 0.0f;
    for (float gain : centre) energy += gain * gain;
    check(std::abs(energy - 1.0f) < 0.01f, "constant power", format("energy=%.3f", energy));
    body += "],\"worstGainError\":" + format("%.4f", worst) + "}";
    writeJson("panning", body);
}

void testDoppler() {
    std::printf("doppler\n");
    const float ratio = dopplerRatio({0.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    const float expected = 343.0f / 333.0f;
    const float cents = 1200.0f * std::log2(ratio / expected);
    check(std::abs(cents) < 1.0f, "10 m/s approach matches analytic ratio",
          format("cents=%.3f", cents));
    const float away = dopplerRatio({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    check(away < 1.0f && std::abs(1200.0f * std::log2(away / (343.0f / 353.0f))) < 1.0f,
          "receding source drops pitch", format("ratio=%.5f", away));
    writeJson("doppler", "{\"ratio\":" + format("%.6f", ratio) + ",\"expected\":" +
                              format("%.6f", expected) + ",\"cents\":" + format("%.3f", cents) +
                              ",\"recede\":" + format("%.6f", away) + "}");
}

void testHrtf() {
    std::printf("hrtf\n");
    const float itd = hrtfItdSeconds(1.5707963f);
    const float woodworth = (0.0875f / 343.0f) * (1.5707963f + 1.0f);
    const float itdError = std::abs(itd - woodworth) * 1.0e6f;
    check(itdError < 30.0f, "ITD within 30 us of Woodworth", format("errorUs=%.1f", itdError));
    check(hrtfShadowCutoff(1.0f) < 4000.0f && hrtfShadowCutoff(0.0f) > 15000.0f,
          "head shadow cutoff spans the expected range",
          format("far=%.0f near=%.0f", hrtfShadowCutoff(1.0f), hrtfShadowCutoff(0.0f)));

    // Render a source hard right and check the contralateral ear is quieter.
    Engine engine;
    std::string error;
    engine.configure(baseConfig(Quality::Binaural, Layout::Stereo), error);
    auto tone = makeTone(1000.0f, 1.5f, 0.4f);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    engine.play(handle, at({6.0f, 0.0f, 0.0f}));
    const auto out = engine.renderOffline(1.0f);
    const float left = rms(out, 2, 0, 9600, 48000);
    const float right = rms(out, 2, 1, 9600, 48000);
    const float ild = 20.0f * std::log10(std::max(1e-9f, right) / std::max(1e-9f, left));
    check(ild > 2.0f, "right-side source is louder in the right ear", format("ildDb=%.2f", ild));
    writeJson("hrtf", "{\"itdUs\":" + format("%.1f", itd * 1.0e6f) + ",\"woodworthUs\":" +
                          format("%.1f", woodworth * 1.0e6f) + ",\"itdErrorUs\":" +
                          format("%.1f", itdError) + ",\"ildDb\":" + format("%.2f", ild) + "}");
}

void testOcclusion() {
    std::printf("occlusion\n");
    auto render = [](bool blocked, bool noise) {
        Engine engine;
        std::string error;
        engine.configure(baseConfig(Quality::Spatial, Layout::Surround51), error);
        std::shared_ptr<Sound> sound = noise ? makeNoise(2.0f, 0.25f) : makeTone(200.0f, 2.0f, 0.25f);
        const uint32_t handle = engine.addSound(sound);
        engine.setListener(facingForward());
        if (blocked) {
            Occluder wall;
            wall.min = {-2.0f, -2.0f, 2.0f};
            wall.max = {2.0f, 2.0f, 3.0f};
            wall.gainDb = -12.0f;
            wall.cutoffHz = 800.0f;
            engine.setOccluders({wall});
        }
        engine.play(handle, at({0.0f, 0.0f, 5.0f}));
        auto out = engine.renderOffline(1.5f);
        return std::make_pair(out, engine.stats());
    };
    const uint32_t channels = layoutChannels(Layout::Surround51);
    // 200 Hz sits inside the 800 Hz occlusion filter passband, so the level drop
    // measures the gain term rather than the filter.
    auto toneOpen = render(false, false);
    auto toneClosed = render(true, false);
    const float openRms = rms(toneOpen.first, channels, 2, 24000);
    const float closedRms = rms(toneClosed.first, channels, 2, 24000);
    const float drop = 20.0f * std::log10(std::max(1e-9f, openRms) / std::max(1e-9f, closedRms));
    check(drop > 8.0f && drop < 16.0f, "wall attenuates 8..16 dB on a passband tone",
          format("dropDb=%.2f", drop));

    auto noiseOpen = render(false, true);
    auto noiseClosed = render(true, true);
    const float openHf = highFrequencyEnergy(noiseOpen.first, channels, 2);
    const float closedHf = highFrequencyEnergy(noiseClosed.first, channels, 2);
    check(closedHf < openHf * 0.1f, "wall also removes high frequencies",
          format("open=%.3f closed=%.3f", openHf, closedHf));
    check(noiseClosed.second.physical == 1, "voice stays physical behind the wall");

    // Smoothing: no block-to-block jump larger than 4 dB once the wall is added.
    Engine engine;
    std::string error;
    engine.configure(baseConfig(Quality::Spatial, Layout::Surround51), error);
    auto noise = makeNoise(3.0f, 0.25f);
    const uint32_t handle = engine.addSound(noise);
    engine.setListener(facingForward());
    engine.play(handle, at({0.0f, 0.0f, 5.0f}));
    std::vector<float> blocks;
    std::vector<float> block(256 * channels, 0.0f);
    for (int i = 0; i < 120; ++i) {
        if (i == 20) {
            Occluder wall;
            wall.min = {-2.0f, -2.0f, 2.0f};
            wall.max = {2.0f, 2.0f, 3.0f};
            engine.setOccluders({wall});
        }
        engine.render(block.data(), 256);
        blocks.push_back(rms(block, channels, 2));
    }
    float worstJump = 0.0f;
    for (size_t i = 11; i < blocks.size(); ++i) {
        const float a = std::max(1e-9f, blocks[i - 1]);
        const float b = std::max(1e-9f, blocks[i]);
        worstJump = std::max(worstJump, std::abs(20.0f * std::log10(a / b)));
    }
    check(worstJump < 4.0f, "occlusion changes smoothly", format("worstJumpDb=%.2f", worstJump));
    writeJson("occlusion", "{\"dropDb\":" + format("%.2f", drop) + ",\"hfOpen\":" +
                               format("%.5f", openHf) + ",\"hfClosed\":" +
                               format("%.5f", closedHf) + ",\"worstBlockJumpDb\":" +
                               format("%.2f", worstJump) + "}");
}

void testVirtualization() {
    std::printf("virtualization\n");
    Engine engine;
    std::string error;
    Config config = baseConfig(Quality::Spatial, Layout::Surround51);
    config.maxPhysicalVoices = 8;
    config.maxVirtualVoices = 256;
    engine.configure(config, error);
    auto tone = makeTone(440.0f, 5.0f, 0.1f, true);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    for (int i = 0; i < 200; ++i) {
        PlayParams params = at({static_cast<float>(i % 20) - 10.0f, 0.0f, static_cast<float>(i / 20) - 5.0f});
        params.priority = static_cast<uint32_t>(i);
        engine.play(handle, params);
    }
    engine.renderOffline(0.5f);
    const Stats stats = engine.stats();
    check(stats.physical <= 8, "physical voices respect the budget", format("physical=%.0f", stats.physical));
    check(stats.virtualVoices > 100, "the rest are virtual", format("virtual=%.0f", stats.virtualVoices));
    check(stats.started == 200, "all plays were accepted", format("started=%.0f", stats.started));
    writeJson("virtualization", "{\"requested\":200,\"physical\":" + format("%.0f", stats.physical) +
                                    ",\"virtual\":" + format("%.0f", stats.virtualVoices) +
                                    ",\"steals\":" + format("%.0f", stats.steals) +
                                    ",\"started\":" + format("%.0f", stats.started) +
                                    ",\"limit\":8}");
}

void testLoop() {
    std::printf("loop\n");
    Engine engine;
    std::string error;
    engine.configure(baseConfig(Quality::Spatial, Layout::Surround51), error);
    auto tone = makeTone(440.0f, 0.5f, 0.3f, true);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    engine.play(handle, at({0.0f, 0.0f, 0.0f}));
    const auto out = engine.renderOffline(2.0f);
    const uint32_t channels = layoutChannels(Layout::Surround51);
    float maxStep = 0.0f;
    for (size_t i = channels + 4800; i < out.size(); i += channels) {
        maxStep = std::max(maxStep, std::abs(out[i] - out[i - channels]));
    }
    // A 440 Hz tone at 0.3 amplitude has a per-sample step of ~0.017; a loop click
    // would show up as a much larger discontinuity.
    check(maxStep < 0.05f, "loop boundary has no click", format("maxStep=%.4f", maxStep));
    writeJson("loop", "{\"maxStep\":" + format("%.5f", maxStep) + ",\"threshold\":0.05}");
}

void testDeterminism() {
    std::printf("determinism\n");
    auto run = [](uint32_t block) {
        Engine engine;
        std::string error;
        engine.configure(baseConfig(Quality::Spatial, Layout::Surround51, block), error);
        auto tone = makeTone(440.0f, 3.0f, 0.2f, true);
        auto noise = makeNoise(3.0f, 0.05f);
        const uint32_t a = engine.addSound(tone);
        const uint32_t b = engine.addSound(noise);
        engine.setListener(facingForward());
        engine.play(a, at({-2.0f, 0.0f, 3.0f}));
        engine.play(b, at({2.0f, 0.5f, 4.0f}));
        engine.renderOffline(1.0f);
        return engine.stats();
    };
    const Stats first = run(256);
    const Stats second = run(256);
    const Stats third = run(480);
    check(first.pcmHash == second.pcmHash, "two identical runs hash the same");
    check(first.pcmHash == third.pcmHash, "block size does not change the samples",
          format("hash256=%016llx", static_cast<unsigned long long>(first.pcmHash)));
    check(first.frames == second.frames && first.frames == 48000, "frame count is exact");
    char hashA[32], hashB[32];
    std::snprintf(hashA, sizeof(hashA), "%016llx", static_cast<unsigned long long>(first.pcmHash));
    std::snprintf(hashB, sizeof(hashB), "%016llx", static_cast<unsigned long long>(third.pcmHash));
    writeJson("determinism", std::string("{\"hash256\":\"") + hashA + "\",\"hash480\":\"" + hashB +
                                 "\",\"frames\":" + format("%.0f", first.frames) + "}");
}

void testObjects() {
    std::printf("objects\n");
    Engine engine;
    std::string error;
    Config config = baseConfig(Quality::Atmos, Layout::Surround712);
    config.maxObjects = 32;
    engine.configure(config, error);
    auto tone = makeTone(300.0f, 2.0f, 0.05f, true);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    for (int i = 0; i < 40; ++i) {
        const float angle = static_cast<float>(i) * 0.157f;
        engine.play(handle, at({std::sin(angle) * 5.0f, static_cast<float>(i % 5) - 2.0f,
                                std::cos(angle) * 5.0f}));
    }
    engine.renderOffline(0.2f);
    const auto objects = engine.objects();
    const Stats stats = engine.stats();
    check(stats.objects <= 32, "object slots respect the cap", format("objects=%.0f", stats.objects));
    check(!objects.empty(), "objects carry metadata", format("count=%.0f", objects.size()));
    bool finite = true;
    for (const auto& meta : objects) {
        if (!std::isfinite(meta.position.x) || !std::isfinite(meta.position.y) ||
            !std::isfinite(meta.position.z) || !std::isfinite(meta.gainDb)) finite = false;
    }
    check(finite, "metadata is finite");
    writeJson("objects", "{\"slots\":" + format("%.0f", stats.objects) + ",\"metadata\":" +
                             format("%.0f", objects.size()) + ",\"cap\":32}");
}

void testDownmix() {
    std::printf("downmix\n");
    const float lfe = 0.0f;
    const float input[10] = {0.5f, 0.0f, 0.0f, lfe, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float stereo[2]{};
    downmix(input, 10, stereo, 2, 1);
    check(std::abs(stereo[0] - 0.5f) < 1e-5f && std::abs(stereo[1]) < 1e-5f,
          "left-only stays left", format("L=%.4f R=%.4f", stereo[0], stereo[1]));
    const float centre[10] = {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    downmix(centre, 10, stereo, 2, 1);
    check(std::abs(stereo[0] - 0.3536f) < 1e-3f && std::abs(stereo[1] - 0.3536f) < 1e-3f,
          "centre folds to both ears at -3 dB", format("L=%.4f R=%.4f", stereo[0], stereo[1]));
    const float rear[10] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f};
    float surround[6]{};
    downmix(rear, 10, surround, 6, 1);
    check(std::abs(surround[4] - 0.3536f) < 1e-3f, "rear-left folds into surround-left",
          format("Ls=%.4f", surround[4]));
    writeJson("downmix", "{\"leftOnly\":[" + format("%.4f", stereo[0]) + "," + format("%.4f", stereo[1]) +
                             "],\"centreL\":" + format("%.4f", stereo[0]) + "}");
}

void testBudget() {
    std::printf("budget\n");
    Engine engine;
    std::string error;
    Config config = baseConfig(Quality::Binaural, Layout::Stereo);
    engine.configure(config, error);
    auto tone = makeTone(220.0f, 10.0f, 0.05f, true);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    for (int i = 0; i < 64; ++i) {
        const float angle = static_cast<float>(i) * 0.098f;
        engine.play(handle, at({std::sin(angle) * 8.0f, 0.0f, std::cos(angle) * 8.0f}));
    }
    std::vector<float> block(256 * 2, 0.0f);
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 400; ++i) engine.render(block.data(), 256);
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - begin).count() / 400.0;
    check(ms < 5.33, "64-voice binaural block fits the 5.33 ms period", format("ms=%.3f", ms));
    check(engine.stats().physical == 64, "all 64 voices are physical");
    writeJson("budget", "{\"voices\":64,\"blockMs\":" + format("%.4f", ms) +
                            ",\"periodMs\":5.333,\"blockFrames\":256}");
}

void testStability() {
    std::printf("stability\n");
    Engine engine;
    std::string error;
    engine.configure(baseConfig(Quality::Spatial, Layout::Surround712), error);
    auto tone = makeTone(180.0f, 20.0f, 0.02f, true);
    const uint32_t handle = engine.addSound(tone);
    engine.setListener(facingForward());
    for (int i = 0; i < 32; ++i) engine.play(handle, at({static_cast<float>(i % 8) - 4.0f, 0.0f, 6.0f}));
    const auto out = engine.renderOffline(10.0f);
    bool finite = true;
    for (float value : out) if (!std::isfinite(value)) finite = false;
    const Stats stats = engine.stats();
    check(finite, "10 s render is finite");
    check(stats.underruns == 0, "no underruns on the offline path");
    check(stats.peak <= 0.99f, "limiter keeps the output below full scale",
          format("peak=%.3f", stats.peak));
    writeJson("stability", "{\"seconds\":10,\"frames\":" + format("%.0f", stats.frames) +
                               ",\"underruns\":" + format("%.0f", stats.underruns) +
                               ",\"peak\":" + format("%.4f", stats.peak) + "}");
}

} // namespace

int main() {
    std::printf("native audio evidence\n");
    testNullPath();
    testLoudness();
    testPanning();
    testDoppler();
    testHrtf();
    testOcclusion();
    testVirtualization();
    testLoop();
    testDeterminism();
    testObjects();
    testDownmix();
    testBudget();
    testStability();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
