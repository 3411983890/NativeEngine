#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Deterministic simulation support: an integer-only state digest and a registry of
// named random streams.
//
// Rules this file exists to enforce:
//  * The simulation may not use std::rand, std::random_device, wall-clock time or
//    std::default_random_engine. Those are process-global and order-dependent, so two
//    runs of the same input diverge.
//  * Every random consumer owns a named stream. Adding a new consumer therefore cannot
//    shift the sequence another consumer already depends on.
//  * Floats are never hashed bit-for-bit; they are quantized to a fixed grid first so a
//    libm or compiler difference cannot silently change the digest.
namespace engine {
namespace determinism {

// FNV-1a 64. Integer-only, so the digest is identical across compilers and platforms.
class Hasher {
public:
    void u8(std::uint8_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i32(std::int32_t value);
    // Quantizes to 1/scale and hashes the integer. Non-finite values hash to a sentinel
    // instead of relying on conversion behaviour that differs between compilers.
    void quantized(float value, float scale = 10000.0f);
    void text(const std::string& value);
    std::uint64_t value() const { return value_; }
    void reset() { value_ = kOffset; }
    std::string hex() const;
private:
    static constexpr std::uint64_t kOffset = 1469598103934665603ull;
    static constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t value_ = kOffset;
};

// xoshiro128** with splitmix64 seeding. Small, fast, and fully specified by 32-bit
// integer arithmetic, so it is reproducible anywhere.
class Stream {
public:
    Stream() = default;
    Stream(std::string name, std::uint64_t seed);
    void reseed(std::uint64_t seed);
    std::uint32_t next();
    // [0, 1). Derived from the top 24 bits so the value is exactly representable.
    float unit();
    // [0, bound) without modulo bias; bound 0 returns 0.
    std::uint32_t range(std::uint32_t bound);
    const std::string& name() const { return name_; }
    std::uint64_t counter() const { return counter_; }
private:
    std::string name_;
    std::uint32_t state_[4]{};
    std::uint64_t counter_ = 0;
};

// Owns one Stream per name. References stay valid for the lifetime of the registry
// because streams are stored in a deque.
class Registry {
public:
    void reset(std::uint64_t seed);
    Stream& stream(const std::string& name);
    bool has(const std::string& name) const;
    std::size_t size() const { return streams_.size(); }
    const std::deque<Stream>& streams() const { return streams_; }
    std::uint64_t seed() const { return seed_; }
    // Covers seed, stream names and per-stream counters: a report field that proves the
    // RNG state, not just the configuration.
    std::uint64_t fingerprint() const;
private:
    std::uint64_t seed_ = 0;
    std::deque<Stream> streams_;
};

Registry& registry();

// splitmix64, also used to derive independent per-stream seeds from one run seed.
std::uint64_t mix64(std::uint64_t value);
std::uint64_t deriveSeed(std::uint64_t base, const std::string& name);

} // namespace determinism
} // namespace engine
