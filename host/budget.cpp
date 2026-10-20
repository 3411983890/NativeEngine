#include "budget.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace host {
namespace {

} // namespace

void BudgetGovernor::configure(BudgetConfig config) {
    config_ = std::move(config);
    gpu_.clear();
    cpu_.clear();
    vram_.clear();
    consecutive_ = 0;
    overFrames_ = 0;
    level_ = 0;
    steps_.clear();
}

void BudgetGovernor::trim() {
    const std::size_t window = std::max<std::size_t>(1, config_.windowFrames);
    while (gpu_.size() > window) gpu_.pop_front();
    while (cpu_.size() > window) cpu_.pop_front();
    while (vram_.size() > window) vram_.pop_front();
}

double BudgetGovernor::percentile(const std::deque<double>& values, double p) {
    if (values.empty()) return 0.0;
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil((sorted.size() - 1) * p));
    return sorted[std::min(index, sorted.size() - 1)];
}

double BudgetGovernor::gpuP95() const {
    return percentile(gpu_, 0.95);
}

double BudgetGovernor::cpuP95() const {
    return percentile(cpu_, 0.95);
}

double BudgetGovernor::gpuMedian() const {
    return percentile(gpu_, 0.5);
}

double BudgetGovernor::cpuMedian() const {
    return percentile(cpu_, 0.5);
}

unsigned long long BudgetGovernor::vramPeak() const {
    unsigned long long peak = 0;
    for (const auto value : vram_) peak = std::max(peak, value);
    return peak;
}

bool BudgetGovernor::overBudget(double gpuMedianValue, double cpuMedianValue,
                                unsigned long long vramPeakValue,
                                std::string& reason) const {
    if (config_.targetGpuMs > 0.0 && gpuMedianValue > config_.targetGpuMs) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "gpuP50 %.3f ms over target %.3f ms", gpuMedianValue,
                      config_.targetGpuMs);
        reason = buffer;
        return true;
    }
    if (config_.targetCpuSubmitMs > 0.0 && cpuMedianValue > config_.targetCpuSubmitMs) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "cpuSubmitP50 %.3f ms over target %.3f ms", cpuMedianValue,
                      config_.targetCpuSubmitMs);
        reason = buffer;
        return true;
    }
    if (config_.targetVramBytes > 0 && vramPeakValue > config_.targetVramBytes) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "video memory %llu bytes over target %llu bytes",
                      vramPeakValue, config_.targetVramBytes);
        reason = buffer;
        return true;
    }
    return false;
}

const char* BudgetGovernor::observe(unsigned frame, double gpuMs, bool gpuValid,
                                    double cpuSubmitMs, unsigned long long vramBytes) {
    (void)frame;
    if (!config_.enabled) return "";
    if (gpuValid) gpu_.push_back(gpuMs);
    cpu_.push_back(cpuSubmitMs);
    vram_.push_back(vramBytes);
    trim();

    if (level_ >= config_.steps.size()) return "";
    // Wait for a full sustain window so one slow frame cannot trigger a downgrade.
    if (cpu_.size() < config_.sustainFrames) return "";

    std::string reason;
    if (!overBudget(gpuMedian(), cpuMedian(), vramPeak(), reason)) {
        consecutive_ = 0;
        return "";
    }
    ++consecutive_;
    ++overFrames_;
    if (consecutive_ < config_.sustainFrames) return "";
    consecutive_ = 0;
    pendingReason_ = reason;
    return config_.steps[level_].c_str();
}

void BudgetGovernor::noteApplied(const std::string& step, unsigned frame,
                                 const std::string& reason) {
    if (level_ >= config_.steps.size()) return;
    BudgetStep applied;
    applied.name = step;
    applied.frame = frame;
    applied.gpuMedianBefore = gpuMedian();
    applied.cpuMedianBefore = cpuMedian();
    applied.reason = reason;
    steps_.push_back(applied);
    ++level_;
    // The ladder is monotonic: a step is never applied twice, even if the window stays
    // over budget after it.
    consecutive_ = 0;
}

std::string BudgetGovernor::status() const {
    if (!config_.enabled) return "disabled";
    if (!config_.steps.empty() && level_ >= config_.steps.size()) return "exhausted";
    if (level_ > 0) return "downgraded";
    return "within";
}

} // namespace host
