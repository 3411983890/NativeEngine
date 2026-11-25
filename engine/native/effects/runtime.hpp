#pragma once

#include "../vec3.hpp"
#include "../assets/sha256.hpp"
#include "../smoke/effect_config.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::effects {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string code, std::string message);
};
using Color = std::array<float,4>;
using Size = std::array<float,2>;
struct CurveKey { float t = 0; std::array<float,4> value{}; };
enum class Shape { Point, Sphere, Cone, Box, Line, Mesh };
enum class Blend { Additive, Premultiplied, Cutout };
enum class ModuleKind { Initialize, Acceleration, Drag, ColorCurve, SizeCurve, CurlNoise, FieldCoupling, VelocityField, FieldDeposit };
struct Module {
    ModuleKind kind = ModuleKind::Initialize;
    Vec3 vector{};
    Color color{1,1,1,1};
    Size size{1,1};
    float coefficient = 0, frequency = 1;
    std::string fieldChannel;
    std::uint32_t fieldQuantum = 0;
    std::vector<CurveKey> keys;
};
struct Renderer {
    std::string type, texture, mesh, material;
    Blend blend = Blend::Additive;
    bool lit = false;
};
struct Burst { std::uint32_t tick = 0, count = 0; };
enum class EventKind { Spawn, Collision, Death, Threshold, Timeline };
struct EventRule {
    EventKind on = EventKind::Spawn;
    std::string spawn;
    std::uint32_t limit = 1, delayTicks = 0;
};
// One emitted event. Events are produced in a fixed order and are never replayed:
// a consumer that drains them owns that tick's records.
struct Event {
    EventKind kind = EventKind::Spawn;
    std::string source;
    Vec3 position{}, normal{};
    float energy = 0;
    std::uint64_t tick = 0, sequence = 0;
    std::uint32_t depth = 0;
};
enum class ColliderKind { Sphere, Box };
struct Collider {
    ColliderKind kind = ColliderKind::Sphere;
    Vec3 centre{}, extent{1,1,1};
    float radius = 1;
};
struct Emitter {
    std::string id;
    Shape shape = Shape::Point;
    Vec3 position{}, extent{1,1,1}, lineEnd{};
    float radius = 0, angle = 0;
    std::uint64_t rateMilli = 0;
    std::uint32_t lifeMin = 1, lifeMax = 1;
    bool localSpace = false;
    std::vector<Burst> bursts;
    std::vector<Module> modules;
    std::vector<Renderer> renderers;
};
struct Asset {
    std::string id, hash, layoutHash;
    std::uint32_t revision = 0, seed = 0, duration = 1, maxAlive = 1, maxSpawnPerTick = 1;
    std::uint32_t maxEventsPerTick = 0, maxDepth = 0;
    bool loop = false, hasEvents = false, hasField = false;
    std::string fieldLevel;
    std::vector<std::string> fieldChannels;
    float fieldVoxelMetres=0;
    std::array<std::uint32_t,3> fieldDimensions{};
    std::optional<smoke::EffectConfig> smoke;
    std::vector<EventRule> rules;
    Vec3 boundsMin{}, boundsMax{};
    std::vector<Emitter> emitters;
};

// Native asset loader accepts the authenticated /2 cook payload only. File I/O
// and texture dependency loading are separate from simulation and rendering.
inline std::string sha256(const std::string& bytes) { return assets::sha256(bytes); }
std::shared_ptr<const Asset> loadCooked(const std::string& file);
std::shared_ptr<const Asset> parseCooked(const std::string& bytes);
// Empty on success. Uses conservative per-emitter pool capacities, matching
// GPU LEF admission; no alive-count readback can relax the UInt32 bound.
std::string fieldDepositError(const Asset& asset);
struct Library {
    std::map<std::string, std::shared_ptr<const Asset>> assets;
    std::map<std::string, std::string> dependencies;
    std::string hash;
};
Library loadLibrary(const std::string& manifestFile, const std::string& assetRoot);

struct Handle { std::uint32_t slot = 0, generation = 0; };
enum class SpawnStatus { Accepted, UnknownAsset, UnsupportedFeature, Capacity, InvalidPosition, InvalidParameter };
// Emission strength for one spawn. Visual only: it scales how many particles a rate
// or a burst emits, so a weak contact produces a sparse splash instead of a full
// one. It never changes particle velocity, lifetime or any simulation value, and it
// is clamped to [0, 8] so one caller cannot exhaust a shared pool.
struct SpawnParams { float intensity = 1.0f; };
struct SpawnResult { SpawnStatus status = SpawnStatus::UnknownAsset; Handle handle; std::string reason; };
struct Sprite {
    Vec3 position;
    Color color;
    Size size;
    float rotation = 0;
    std::string texture;
    Blend blend;
    std::uint64_t stableId = 0;
};
struct Stats {
    std::uint64_t tick = 0, emitted = 0, expired = 0, killed = 0, dropped = 0;
    std::uint64_t events = 0, subeffects = 0, eventsDropped = 0, collisions = 0;
    std::size_t alive = 0, instances = 0;
};
enum class ReloadResult { Installed, Preserved, ResetRequired, Reset };

// CPU reference/fallback for the native instance contract. All emissions occur
// at fixed 120 Hz. This is deliberately not labelled GPU particle simulation.
class Runtime {
public:
    explicit Runtime(std::size_t maxParticles = 100000, std::size_t maxInstances = 128);
    void install(std::shared_ptr<const Asset> asset);
    ReloadResult reload(std::shared_ptr<const Asset> asset, bool resetIncompatible = false);
    SpawnResult spawn(const std::string& assetId, Vec3 origin = {}, std::uint64_t seed = 0,
                      SpawnParams params = {});
    bool stop(Handle handle, bool immediate = false);
    bool pause(Handle handle, bool paused);
    bool move(Handle handle, Vec3 origin);
    bool valid(Handle handle) const;
    // Static colliders are part of the CPU reference only; the GPU host supplies none.
    void setColliders(std::vector<Collider> colliders);
    const std::vector<Event>& events() const { return events_; }
    void clearEvents();
    void step();
    std::vector<Sprite> sprites() const;
    Stats stats() const;
    std::uint64_t digest() const;
    void clear();
private:
    struct EmitterState { std::uint64_t phase = 0, sequence = 0; };
    struct Pool {
        std::vector<Vec3> positions, velocities;
        std::vector<Color> colors;
        std::vector<Size> sizes;
        std::vector<std::uint32_t> ages, lives, emitters;
        std::vector<std::uint64_t> ids;
        std::size_t size() const { return positions.size(); }
        void erase(std::size_t index);
    };
    struct Pending {
        std::string asset;
        Vec3 position;
        std::uint64_t dueTick = 0, seed = 0;
        std::uint32_t depth = 0;
        float intensity = 1.0f;
    };
    struct Instance {
        std::shared_ptr<const Asset> asset;
        std::uint32_t generation = 1;
        bool active = false, paused = false, emitting = true;
        std::uint64_t age = 0, serial = 0, seed = 0;
        std::uint32_t depth = 0;
        std::uint32_t spawnsThisTick = 0;
        float intensity = 1.0f;
        std::vector<std::uint32_t> ruleSpawns;
        Vec3 origin;
        std::vector<EmitterState> emitters;
        Pool pool;
    };
    Instance* find(Handle handle);
    const Instance* find(Handle handle) const;
    void retire(Instance& instance);
    SpawnResult spawnInternal(const std::string& id, Vec3 origin, std::uint64_t seed, std::uint32_t depth,
                              float intensity);
    void emit(Instance& instance, EventKind kind, Vec3 position, Vec3 normal, float energy);
    void dispatchPending();
    void collide(Instance& instance);
    static std::string unsupported(const Asset& asset);
    std::map<std::string, std::shared_ptr<const Asset>> assets_;
    std::vector<Instance> instances_;
    std::size_t maxParticles_, maxInstances_;
    std::uint64_t serial_ = 0, eventSerial_ = 0;
    std::vector<Collider> colliders_;
    std::vector<Pending> pending_;
    std::vector<Event> events_;
    Stats counters_;
};
} // namespace engine::effects
