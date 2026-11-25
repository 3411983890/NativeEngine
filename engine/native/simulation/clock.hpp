#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace engine::simulation {

// Integer rational clock: one nanosecond adds hz phase units, one tick consumes
// 1e9. A frame work limit never discards authority time. Debt is observable and
// remains in the snapshot until explicitly consumed. Paused time is excluded.
class Clock {
public:
    static constexpr std::uint64_t PhasePerTick = 1000000000ull;
    struct Snapshot {
        std::uint32_t hz = 120;
        std::uint64_t tick = 0;
        std::uint64_t phase = 0;
        bool paused = false;
    };

    explicit Clock(std::uint32_t hz = 120) { state_.hz = hz; validate(state_); }
    void addNanoseconds(std::uint64_t ns) {
        if (state_.paused) return;
        const auto capacity = std::numeric_limits<std::uint64_t>::max() - state_.phase;
        if (ns > capacity / state_.hz) throw std::overflow_error("NG-E-SIM-CLOCK: time capacity exceeded");
        state_.phase += ns * state_.hz;
    }
    std::uint64_t pendingTicks() const { return state_.phase / PhasePerTick; }
    std::uint64_t tick() const { return state_.tick; }
    std::uint32_t hz() const { return state_.hz; }
    double secondsPerTick() const { return 1.0 / state_.hz; }
    double fraction() const { return static_cast<double>(state_.phase % PhasePerTick) / PhasePerTick; }
    bool consumeTick() {
        if (state_.paused || pendingTicks() == 0) return false;
        if (state_.tick == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("NG-E-SIM-CLOCK: tick capacity exceeded");
        state_.phase -= PhasePerTick;
        ++state_.tick;
        return true;
    }
    void setPaused(bool paused) { state_.paused = paused; }
    Snapshot snapshot() const { return state_; }
    void restore(Snapshot state) { validate(state); state_ = state; }
private:
    static void validate(const Snapshot& state) {
        if (state.hz == 0 || state.hz > 1000000)
            throw std::invalid_argument("NG-E-SIM-CLOCK: frequency must be in [1,1000000]");
    }
    Snapshot state_;
};

} // namespace engine::simulation
