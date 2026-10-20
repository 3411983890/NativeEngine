#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace host {

// A frame budget is a target, not a measurement: when the rolling window stays above
// the target the governor walks down an ordered ladder of quality steps, one at a time,
// and records why. It never silently changes quality without a trace in the report.
struct BudgetConfig {
    bool enabled = false;
    // GPU time target for the rolling window. 0 disables that dimension.
    double targetGpuMs = 16.6;
    // CPU submit target (excludes Present). 0 disables that dimension.
    double targetCpuSubmitMs = 0.0;
    // Video memory ceiling. 0 disables that dimension.
    unsigned long long targetVramBytes = 0;
    // Rolling window used for the percentile.
    unsigned windowFrames = 120;
    // Consecutive over-budget frames required before a step is applied.
    unsigned sustainFrames = 30;
    // Ordered ladder. Names are applied by the host; the governor only decides when.
    std::vector<std::string> steps;
};

struct BudgetStep {
    std::string name;
    unsigned frame = 0;
    // Median frame cost at the moment the step was applied: the decision metric.
    double gpuMedianBefore = 0.0;
    double cpuMedianBefore = 0.0;
    std::string reason;
};

class BudgetGovernor {
public:
    void configure(BudgetConfig config);
    const BudgetConfig& config() const { return config_; }

    // Feeds one completed frame. Returns the ladder step that must be applied now, or
    // "" when nothing should change. Call noteApplied() for every returned step, even
    // if the host could not apply it: consuming the level keeps the ladder monotonic.
    const char* observe(unsigned frame, double gpuMs, bool gpuValid,
                        double cpuSubmitMs, unsigned long long vramBytes);
    void noteApplied(const std::string& step, unsigned frame, const std::string& reason);
    // Reason attached to the most recent observe() that returned a step.
    const std::string& pendingReason() const { return pendingReason_; }

    double gpuP95() const;
    double cpuP95() const;
    // Median of the rolling window. This is the downgrade decision metric: a p95 in a
    // short window is dominated by a single spike and would downgrade on one slow frame.
    double gpuMedian() const;
    double cpuMedian() const;
    unsigned long long vramPeak() const;
    // Frames observed (the CPU series is always populated, GPU timings can be invalid).
    unsigned samples() const { return static_cast<unsigned>(cpu_.size()); }
    unsigned gpuSamples() const { return static_cast<unsigned>(gpu_.size()); }
    unsigned level() const { return level_; }
    unsigned overBudgetFrames() const { return overFrames_; }
    bool exhausted() const { return level_ >= config_.steps.size(); }
    const std::vector<BudgetStep>& steps() const { return steps_; }
    // "disabled" | "within" | "downgraded" | "exhausted"
    std::string status() const;

private:
    static double percentile(const std::deque<double>& values, double p);
    void trim();
    // The decision uses the window median and the peak video memory.
    bool overBudget(double gpuMedian, double cpuMedian, unsigned long long vramPeak,
                    std::string& reason) const;

    BudgetConfig config_{};
    std::deque<double> gpu_;
    std::deque<double> cpu_;
    std::deque<unsigned long long> vram_;
    unsigned consecutive_ = 0;
    unsigned overFrames_ = 0;
    unsigned level_ = 0;
    std::vector<BudgetStep> steps_;
    std::string pendingReason_;
};

} // namespace host
