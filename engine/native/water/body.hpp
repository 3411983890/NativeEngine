#pragma once

#include "../vec3.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::water {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string code, std::string message);
};

enum class Backend { Analytic, Shallow, Spectrum };
enum class BathymetryType { Flat, Slope, Basin, Channel, Mounds };
// A raised bed bump in water-local metres. Used to author riverbed variation without
// shipping a heightmap that would dwarf the asset.
struct Mound { float x = 0, z = 0, radiusMetres = 0.5f, heightMetres = 0; };
struct Bathymetry {
    BathymetryType type = BathymetryType::Flat;
    float heightMetres = 0, slopeX = 0, slopeZ = 0, depthMetres = 0;
    // Channel: half-width banks rise at channelBankSlope per metre beyond the channel.
    float channelWidthMetres = 2, channelBankSlope = 1;
    std::vector<Mound> mounds;
};
struct Waves {
    float amplitudeMetres = 0, wavelengthMetres = 1, speedMetresPerSecond = 0;
    float directionX = 1, directionZ = 0;
    std::uint32_t components = 1;
};
struct Material { float refractionIndex = 1.333f, absorptionPerMetre = 0.3f, foamThreshold = 0.5f; };
// A local opening. `flowCubicMetresPerSecond` is the requested volume rate; the
// direction/speed pair sets the momentum the injected volume carries. Positions are
// water-local metres.
struct FlowOpening {
    float x = 0, z = 0, radiusMetres = 0.5f, flowCubicMetresPerSecond = 0;
    float speedMetresPerSecond = 0, directionX = 1, directionZ = 0;
};
// Static obstacle in water-local metres. `topHeightMetres` is compared against the
// still level: below it the obstacle raises the bed (water flows over), at or above it
// the footprint is solid and blocks normal flux. A bed is always present underneath.
struct Obstacle { float x = 0, z = 0, radiusMetres = 0.5f, topHeightMetres = 0; };

struct Asset {
    std::string id, hash;
    std::uint32_t revision = 0, seed = 0;
    Backend backend = Backend::Analytic;
    float extentX = 1, extentZ = 1, cellMetres = 0.1f, stillHeightMetres = 0;
    Bathymetry bathymetry;
    bool openBoundaries = false;
    // Uniform domain-wide source/sink kept for backwards compatibility. Localised
    // openings take precedence by adding to the same ledger.
    float inflowCubicMetresPerSecond = 0, outflowCubicMetresPerSecond = 0;
    std::vector<FlowOpening> sources, drains;
    std::vector<Obstacle> obstacles;
    Waves waves;
    Material material;
    std::uint32_t maxCells = 1, maxSubsteps = 1;
    std::uint32_t columns = 0, rows = 0;
};

std::shared_ptr<const Asset> parseCookedWater(const std::string& bytes);
std::shared_ptr<const Asset> loadWater(const std::string& file);
struct Library {
    std::map<std::string, std::shared_ptr<const Asset>> bodies;
    std::string hash;
};
Library loadWaterLibrary(const std::string& manifestFile, const std::string& assetRoot);

// A query always states whether the answer exists. Unknown regions are never reported
// as infinitely deep water.
struct Query {
    bool valid = false;
    float surfaceHeightMetres = 0, depthMetres = 0;
    float velocityX = 0, velocityZ = 0;
    Vec3 normal{0, 1, 0};
    std::uint64_t tick = 0;
    float errorBoundMetres = 0;
};
struct Buoyancy {
    bool valid = false;
    float submergedVolumeCubicMetres = 0, displacedMassKg = 0, netBuoyantForceN = 0;
    Vec3 centre{};
    std::uint64_t tick = 0;
    float errorBoundMetres = 0;
};

enum class ContactKind : std::uint8_t {
    Touch = 0,      // generic surface disturbance
    FootPlant = 1,  // a foot arrives while already standing in water
    Drag = 2,       // a submerged foot travels with the body
    Landing = 3,    // a body crosses the surface from above
    RigidImpact = 4,
    Leaving = 5,    // a foot/body leaves the water (weak, never entry strength)
};
enum class ContactSource : std::uint8_t {
    ActorRoot = 0, LeftFoot = 1, RightFoot = 2, RigidBody = 3, Flow = 4,
};

// The rich CPU-side record. It exists so every consumer (GPU detail, splash, audio,
// debug views) sees one history instead of inventing strengths. `sequence` is the
// identity used for idempotent delivery; `tick` is only the time of occurrence and may
// repeat. `timelineEpoch` changes whenever the body is reset or teleported, so a
// rewound timeline cannot mask new events as already delivered.
struct InteractionEvent {
    float x = 0, z = 0, radius = 0, displacement = 0;
    // Horizontal contact speed. Kept for the existing visual consumers; it is the
    // tangential relative speed, never the vertical arrival speed.
    float impactSpeed = 0;
    float directionX = 0, directionZ = 0, stretch = 1;
    std::uint64_t tick = 0;
    std::uint32_t sequence = 0, timelineEpoch = 0, contactId = 0, actorId = 0;
    std::uint8_t kind = 0, source = 0;
    // Downward relative speed at the contact (m/s). This is the quantity that makes a
    // jump into the pond stronger than a careful step; zero when unknown.
    float normalSpeed = 0;
    // Proxy of the volume the contact pushed aside, m³. Presentation only.
    float displacedVolumeProxy = 0;
};

// Everything a caller can state about one contact. The displacement is still a visual
// zero-net-volume height change; the impulse is an integrated N·s kick.
struct Contact {
    float x = 0, z = 0, radius = 0, displacement = 0;
    float impulseXNs = 0, impulseZNs = 0;
    float horizontalSpeed = 0, normalSpeed = 0;
    std::uint32_t contactId = 0, actorId = 0;
    ContactKind kind = ContactKind::Touch;
    ContactSource source = ContactSource::ActorRoot;
    float displacedVolumeProxy = 0;
};

struct Diagnostics {
    std::uint64_t tick = 0;
    // Changes on reset/teleport. Consumers use it to invalidate presentation state that
    // belongs to a discarded timeline.
    std::uint32_t timelineEpoch = 0;
    std::uint32_t substeps = 0, nonFinite = 0;
    float volumeCubicMetres = 0, initialVolumeCubicMetres = 0;
    float addedCubicMetres = 0, removedCubicMetres = 0;
    float massErrorFraction = 0, maxDepthMetres = 0, maxSpeedMetresPerSecond = 0;
    float maxSurfaceDeviationMetres = 0;
    // True when this tick could not advance the full requested step within the
    // substep budget. The outstanding time is carried as debt, not integrated
    // with an unstable substep.
    bool cflClamped = false;
    // Ratio of the last substep to its CFL bound; never above 1 by construction.
    float lastCflNumber = 0;
    // Seconds requested but not yet simulated, and seconds permanently discarded
    // once the debt cap is reached.
    float timeDebtSeconds = 0, droppedSeconds = 0;
    // Cells whose non-finite state had to be zeroed. A fitness signal, not a
    // silent substitute for the CFL bookkeeping above.
    std::uint64_t sanitizedCells = 0;
    // Flow ledger. Requested is what the asset asked for this tick; actual is what the
    // solver could deliver/remove. `outflowLimited` is the shortfall when a drain would
    // have withdrawn more water than the region holds.
    float inflowRequestedCubicMetres = 0, inflowActualCubicMetres = 0;
    float outflowRequestedCubicMetres = 0, outflowActualCubicMetres = 0;
    float outflowLimitedCubicMetres = 0;
    // Cumulative horizontal impulse returned to the water by rigid-body reactions since
    // reset. (All other ledger fields above are per tick.)
    float reactionImpulseNs = 0;
    std::uint32_t solidCells = 0, sourceCells = 0, drainCells = 0;
    // Events evicted by the bounded history ring before any consumer could read them.
    std::uint64_t droppedEvents = 0;
    float lastStepMilliseconds = 0;
};

// CPU authority reference. Shallow water uses a well-balanced finite-volume update with
// hydrostatic reconstruction; analytic and spectrum are query-only surfaces that never
// pretend to conserve volume.
class Body {
public:
    explicit Body(std::shared_ptr<const Asset> asset, Vec3 origin = {});
    void reset();
    void step(float dtSeconds);
    Query query(float x, float z) const;
    Buoyancy buoyancy(Vec3 boxCentre, Vec3 boxExtent, float densityKgM3) const;
    Diagnostics diagnostics() const;
    std::uint64_t digest() const;
    // Level edits keep the bed and drop horizontal momentum; a water level change is a
    // teleport of the surface, not a simulation step.
    void setLevel(float stillHeightMetres);
    // Authoring/test hook: writes one cell and moves the conservation baseline with it,
    // so a hand-authored initial condition is not reported as a mass leak.
    void setDepth(std::uint32_t column, std::uint32_t row, float depthMetres);
    // Runtime disturbance: a smooth zero-volume displacement and an explicitly
    // integrated horizontal impulse (N s). Preserves the initial mass baseline and
    // existing momentum. Returns false for an unsupported/outside/dry patch.
    bool interact(float x,float z,float radiusMetres,float displacementMetres,float impulseXNs,float impulseZNs,
                  float impactSpeedMetresPerSecond=0);
    bool interact(const Contact& contact);
    // Rigid-body reaction: distributes an integrated horizontal impulse over the wet
    // cells under the footprint. Returns the applied impulse magnitude (same units).
    float applyImpulse(float x, float z, float radiusMetres, float impulseXNs, float impulseZNs);
    // Waterfall/transfer primitives. Withdrawal never dries a region completely in one
    // call; the returned value is the volume actually moved. Both adjust the ledger.
    float withdrawVolume(float x, float z, float radiusMetres, float volumeCubicMetres);
    float depositVolume(float x, float z, float radiusMetres, float volumeCubicMetres,
                        float velocityX, float velocityZ);
    // Adds a static obstacle and rebuilds the bed/solid mask. Only valid before the
    // first step; the water volume the obstacle displaces is baked into the baseline.
    void addObstacle(const Obstacle& obstacle);
    float bedAt(float x, float z) const;
    std::uint64_t tick() const { return tick_; }
    // Most recent successful interactions, oldest first (bounded ring).
    const std::vector<InteractionEvent>& events() const { return events_; }
    std::uint32_t timelineEpoch() const { return timelineEpoch_; }
    std::uint64_t droppedEvents() const { return droppedEvents_; }
    const Asset& asset() const { return *asset_; }
    // Water footprint centre in world metres. Runtime primitives that are authored in
    // water-local metres (openings, obstacles, transfer regions) convert with this.
    Vec3 origin() const { return origin_; }
    std::uint32_t columns() const { return asset_->columns; }
    std::uint32_t rows() const { return asset_->rows; }
    float depthAt(std::uint32_t column, std::uint32_t row) const;
    bool solidAt(std::uint32_t column, std::uint32_t row) const;

private:
    void buildGrid();
    void rebuildBed();
    void advance(float h);
    float surfaceAt(float x, float z) const;
    float bedAtLocal(float x, float z) const;
    float waveHeight(float x, float z, float& slopeX, float& slopeZ) const;
    std::size_t index(std::uint32_t column, std::uint32_t row) const {
        return static_cast<std::size_t>(row) * asset_->columns + column;
    }
    bool inside(float x, float z) const;
    template <class Visit> void forCellsInDisc(float x, float z, float radius, Visit&& visit);

    std::shared_ptr<const Asset> asset_;
    Vec3 origin_{};
    std::vector<float> bed_, depth_, momentumX_, momentumZ_;
    std::vector<float> nextDepth_, nextMomentumX_, nextMomentumZ_;
    std::vector<float> fluxXH_, fluxXMomX_, fluxXMomZ_, fluxZH_, fluxZMomX_, fluxZMomZ_;
    std::vector<float> referenceDepth_;
    std::vector<std::uint8_t> solid_;
    std::vector<InteractionEvent> events_;
    std::uint64_t tick_ = 0;
    std::uint32_t sequence_ = 0, timelineEpoch_ = 0;
    std::uint64_t droppedEvents_ = 0;
    float time_ = 0;
    std::uint32_t substeps_ = 0;
    bool cflClamped_ = false;
    float lastCfl_ = 0;
    double timeDebt_ = 0, droppedTime_ = 0;
    std::uint64_t sanitizedCells_ = 0;
    float added_ = 0, removed_ = 0, initialVolume_ = 0;
    double inflowRequested_ = 0, inflowActual_ = 0, outflowRequested_ = 0, outflowActual_ = 0, outflowLimited_ = 0;
    double reactionImpulse_ = 0;
    std::uint32_t solidCells_ = 0, sourceCells_ = 0, drainCells_ = 0;
    float lastStepMilliseconds_ = 0;
};

} // namespace engine::water
