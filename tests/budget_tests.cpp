// Unit checks for the frame budget governor. These run without a GPU: the governor is
// pure policy, so a broken ladder or a mis-signed comparison must fail here instead of
// only showing up as a quality drop during a budget run.
#include "../host/budget.hpp"

#include <cstdio>
#include <string>

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

host::BudgetConfig makeConfig() {
    host::BudgetConfig config;
    config.enabled = true;
    config.targetGpuMs = 1.0;
    config.windowFrames = 8;
    config.sustainFrames = 4;
    config.steps = {"rt-shadows-off", "mesh-path-off"};
    return config;
}

// Feeds `frames` frames at a fixed cost and returns the FIRST step the governor asked
// for (later steps triggered in the same call are recorded but not returned).
const char* feed(host::BudgetGovernor& governor, unsigned frames, double gpuMs,
                 double cpuMs = 0.1, unsigned long long vram = 1024) {
    const char* first = "";
    for (unsigned frame = 0; frame < frames; ++frame) {
        const char* step = governor.observe(frame, gpuMs, true, cpuMs, vram);
        if (step[0] != '\0') {
            if (first[0] == '\0') first = step;
            governor.noteApplied(step, frame, governor.pendingReason());
        }
    }
    return first;
}

void testDisabledDoesNothing() {
    host::BudgetGovernor governor;
    host::BudgetConfig config = makeConfig();
    config.enabled = false;
    governor.configure(config);
    const char* step = feed(governor, 40, 50.0);
    check(step[0] == '\0', "disabled governor never requests a step");
    check(governor.status() == "disabled", "disabled status is reported");
    check(governor.level() == 0, "disabled governor stays at level 0");
}

void testWithinBudget() {
    host::BudgetGovernor governor;
    governor.configure(makeConfig());
    const char* step = feed(governor, 40, 0.2);
    check(step[0] == '\0', "under-budget frames never trigger a step");
    check(governor.status() == "within", "status stays within");
    check(governor.steps().empty(), "no steps recorded while within budget");
    check(governor.samples() <= 8, "window is trimmed to windowFrames");
}

void testSustainedOverBudgetWalksLadder() {
    host::BudgetGovernor governor;
    governor.configure(makeConfig());
    // 7 frames is the shortest run that fills the 4-sample sustain window and then
    // reaches 4 consecutive over-budget frames.
    const char* first = feed(governor, 7, 5.0);
    check(std::string(first) == "rt-shadows-off", "over budget requests the first step");
    check(governor.level() == 1, "level advanced to 1");
    check(governor.steps().size() == 1, "one step recorded");
    check(governor.steps()[0].gpuMedianBefore > 1.0, "step records the over-budget median");
    check(governor.status() == "downgraded", "status reports downgraded");

    const char* second = feed(governor, 8, 5.0);
    check(std::string(second) == "mesh-path-off", "still over budget requests the second step");
    check(governor.level() == 2, "level advanced to 2");
    check(governor.status() == "exhausted", "status reports exhausted after the ladder");

    const char* third = feed(governor, 20, 5.0);
    check(third[0] == '\0', "exhausted ladder never requests another step");
    check(governor.steps().size() == 2, "each ladder step is applied exactly once");
}

void testShortSpikeDoesNotTrigger() {
    host::BudgetGovernor governor;
    governor.configure(makeConfig());
    // Two frames above target, then comfortably below: the sustain window never fills.
    const char* spike = feed(governor, 2, 9.0);
    check(spike[0] == '\0', "short spike does not trigger a step");
    const char* calm = feed(governor, 20, 0.1);
    check(calm[0] == '\0', "recovery keeps the governor at level 0");
    check(governor.level() == 0, "no downgrade after a spike");
}

void testNeedsSustainFramesOfSamples() {
    host::BudgetGovernor governor;
    governor.configure(makeConfig());
    // Only 3 samples exist and sustainFrames is 4: nothing may be decided yet.
    const char* step = feed(governor, 3, 9.0);
    check(step[0] == '\0', "no decision before the sustain window has enough samples");
}

void testCpuTarget() {
    host::BudgetGovernor governor;
    host::BudgetConfig config = makeConfig();
    config.targetGpuMs = 0.0;
    config.targetCpuSubmitMs = 0.5;
    governor.configure(config);
    const char* after = feed(governor, 7, 0.1, 4.0);
    check(std::string(after) == "rt-shadows-off", "cpu over-budget triggers the ladder");
    check(governor.steps()[0].reason.find("cpuSubmitP50") != std::string::npos,
          "reason names the cpu dimension");
}

void testVramTarget() {
    host::BudgetGovernor governor;
    host::BudgetConfig config = makeConfig();
    config.targetGpuMs = 0.0;
    config.targetVramBytes = 4096;
    governor.configure(config);
    const char* step = feed(governor, 7, 0.1, 0.1, 8192);
    check(std::string(step) == "rt-shadows-off", "vram over target triggers the ladder");
    check(governor.steps()[0].reason.find("video memory") != std::string::npos,
          "reason names the vram dimension");
}

void testInvalidGpuSamplesIgnored() {
    host::BudgetGovernor governor;
    governor.configure(makeConfig());
    bool requested = false;
    for (unsigned frame = 0; frame < 20; ++frame) {
        // gpuValid=false: the window never fills, so nothing may be decided.
        const char* step = governor.observe(frame, 99.0, false, 0.1, 1024);
        if (step[0] != '\0') requested = true;
    }
    check(!requested, "invalid gpu samples never trigger a step");
    check(governor.gpuSamples() == 0, "invalid gpu samples are not counted");
    check(governor.samples() == 8, "frames are still observed without gpu timings");
}

} // namespace

int main() {
    std::printf("budget governor checks\n");
    testDisabledDoesNothing();
    testWithinBudget();
    testSustainedOverBudgetWalksLadder();
    testShortSpikeDoesNotTrigger();
    testNeedsSustainFramesOfSamples();
    testCpuTarget();
    testVramTarget();
    testInvalidGpuSamplesIgnored();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
