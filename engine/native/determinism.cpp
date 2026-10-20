#include "determinism.hpp"

#include <cmath>
#include <cstdio>

namespace engine {
namespace determinism {
namespace {

std::uint32_t rotl(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

} // namespace

// ---------------------------------------------------------------- Hasher

void Hasher::u8(std::uint8_t value) {
    value_ ^= value;
    value_ *= kPrime;
}

void Hasher::u32(std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
    }
}

void Hasher::u64(std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFull));
    }
}

void Hasher::i32(std::int32_t value) {
    u32(static_cast<std::uint32_t>(value));
}

void Hasher::quantized(float value, float scale) {
    if (!std::isfinite(value)) {
        // 0xFF is not a valid first byte for any quantized value below, because every
        // tag is written as a zero byte plus the payload.
        u8(0xFF);
        return;
    }
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    const std::int64_t fixed = static_cast<std::int64_t>(std::llround(scaled));
    u8(0x00);
    u64(static_cast<std::uint64_t>(fixed));
}

void Hasher::text(const std::string& value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (const char character : value) {
        u8(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
}

std::string Hasher::hex() const {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value_));
    return std::string(buffer);
}

// ---------------------------------------------------------------- Stream

Stream::Stream(std::string name, std::uint64_t seed) : name_(std::move(name)) {
    reseed(seed);
}

void Stream::reseed(std::uint64_t seed) {
    std::uint64_t state = mix64(seed);
    do {
        state_[0] = static_cast<std::uint32_t>(state & 0xFFFFFFFFull);
        state = mix64(state);
        state_[1] = static_cast<std::uint32_t>(state & 0xFFFFFFFFull);
        state = mix64(state);
        state_[2] = static_cast<std::uint32_t>(state & 0xFFFFFFFFull);
        state = mix64(state);
        state_[3] = static_cast<std::uint32_t>(state & 0xFFFFFFFFull);
        state = mix64(state);
    } while (state_[0] == 0 && state_[1] == 0 && state_[2] == 0 && state_[3] == 0);
    counter_ = 0;
}

std::uint32_t Stream::next() {
    const std::uint32_t result = rotl(state_[1] * 5u, 7) * 9u;
    const std::uint32_t t = state_[1] << 9;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 11);
    ++counter_;
    return result;
}

float Stream::unit() {
    // 24 bits is exactly the mantissa width of a float, so the result is exact.
    return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
}

std::uint32_t Stream::range(std::uint32_t bound) {
    if (bound == 0) return 0;
    const std::uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % bound);
    for (;;) {
        const std::uint32_t value = next();
        if (value < limit) return value % bound;
    }
}

// ---------------------------------------------------------------- Registry

std::uint64_t mix64(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

std::uint64_t deriveSeed(std::uint64_t base, const std::string& name) {
    Hasher hasher;
    hasher.u64(base);
    hasher.text(name);
    return hasher.value();
}

void Registry::reset(std::uint64_t seed) {
    seed_ = seed;
    streams_.clear();
}

Stream& Registry::stream(const std::string& name) {
    for (auto& candidate : streams_) {
        if (candidate.name() == name) return candidate;
    }
    streams_.emplace_back(name, deriveSeed(seed_, name));
    return streams_.back();
}

bool Registry::has(const std::string& name) const {
    for (const auto& candidate : streams_) {
        if (candidate.name() == name) return true;
    }
    return false;
}

std::uint64_t Registry::fingerprint() const {
    Hasher hasher;
    hasher.text("nativegpu.rng/1");
    hasher.u64(seed_);
    hasher.u32(static_cast<std::uint32_t>(streams_.size()));
    for (const auto& stream : streams_) {
        hasher.text(stream.name());
        hasher.u64(stream.counter());
    }
    return hasher.value();
}

Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace determinism
} // namespace engine
