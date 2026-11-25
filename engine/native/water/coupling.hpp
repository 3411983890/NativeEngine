#pragma once

// Deterministic water <-> character/rigid-body coupling.
//
// Buoyancy uses a small local voxel proxy grid: each proxy contributes the part of
// its volume below the height field, so a tilted body gets an off-centre centre of
// buoyancy and therefore a restoring torque. Drag opposes the relative water
// velocity. The CPU water body remains the only authority; this module only reads
// it and produces forces for the caller to apply.

#include "body.hpp"

namespace engine::water {

struct WadingSettings {
    float minimumDepthMetres = 0.015f;
    float minimumImmersionMetres = 0.02f;
    // Fraction of horizontal speed removed when the immersion fraction reaches
    // referenceImmersionFraction.
    float speedLoss = 0.55f;
    float referenceImmersionFraction = 0.55f;
    // Vertical velocity damping rate (1/s) at the reference immersion.
    float verticalDamping = 2.5f;
    float jumpLoss = 0.4f;
};

struct WadingState {
    bool wading = false;
    float depthMetres = 0, immersionMetres = 0, immersionFraction = 0;
    float speedScale = 1, verticalDampingPerSecond = 0, jumpScale = 1;
};

WadingState wadingState(const Body& water, Vec3 feet, float heightMetres,
                        const WadingSettings& settings = {});

// Footprint state machine shared by the fixed-step Game and the standalone
// PondDemo harness. It turns enter/move/stop into zero-volume displacement plus a
// horizontal impulse at most once per stride. The body records the event itself.
struct FootprintSettings {
    float radiusMetres = 0.65f;
    float strideMetres = 0.24f;
    float entryDisplacementMetres = -0.09f;
    float moveDisplacementMetres = -0.045f;
    float impulseScale = 0.7f;
    float minimumDepthMetres = 0.015f;
    float surfaceAboveFeetMetres = 0.14f;
    // A larger apparent movement is a teleport, not travel.
    float maximumStepMetres = 0.5f;
    // Upper bound for the recorded contact speed. A footstep cannot arrive faster than
    // this, and the clamp keeps a large stride from being reported as a landing.
    float maximumImpactSpeed = 20.0f;
    // Consecutive strides must not produce the same wave. The radius and the displaced
    // depth are jittered by a deterministic function of the contact position and the
    // stride index, and both grow with the contact speed: a fast step pushes more water
    // over a wider footprint than a careful one. Replay is exact because the seed is the
    // same quantized state, not a global random stream.
    float radiusJitter = 0.28f;
    float displacementJitter = 0.4f;
    float speedRadiusGain = 0.3f;
    float speedDisplacementGain = 0.6f;
};

struct FootprintState {
    bool observed = false;
    bool contact = false;
    float travel = 0;
    // Successful contacts so far. Seeds the per-contact variation, so the sequence of
    // waves is reproducible without a global random stream.
    std::uint32_t strides = 0;
    Vec3 previous{};
};

struct FootprintResult {
    bool interacted = false;
    InteractionEvent event{};
    // Echo of what the contact actually asked for, so a test or a debug view can see the
    // variation without reading the solver back.
    float radiusMetres = 0, displacementMetres = 0;
};

// `verticalSpeedMetresPerSecond` is the downward speed of the contact (positive when the
// body is falling). It turns a fall into a stronger landing without inventing a second
// strength model; zero keeps the old horizontal-only behaviour.
FootprintResult updateFootprint(Body& water, Vec3 feet, float heightMetres,
                                float dx, float dz, float dtSeconds,
                                FootprintState& state,
                                const FootprintSettings& settings = {},
                                float verticalSpeedMetresPerSecond = 0);

// Per-foot contact state machine used when the rig exposes sole positions. It replaces
// the single root footprint so left and right feet can disturb the water at their own
// locations with their own relative speed. The phases are Dry -> SurfaceCrossing ->
// SubmergedContact -> Dragging -> Leaving -> Dry; standing still emits nothing.
struct FootContactSettings {
    float radiusMetres = 0.22f;
    float strideMetres = 0.16f;
    float entryDisplacementMetres = -0.055f;
    float dragDisplacementMetres = -0.028f;
    float leaveDisplacementMetres = -0.010f;
    float impulseScale = 0.45f;
    float minimumDepthMetres = 0.015f;
    // The sole is in contact once it is this far below the surface, which keeps a shoe
    // skimming the surface from switching state every frame.
    float contactMarginMetres = 0.01f;
    float hysteresisMetres = 0.02f;
    float maximumStepMetres = 0.6f;
    float maximumImpactSpeed = 20.0f;
    float radiusJitter = 0.25f;
    float displacementJitter = 0.35f;
    float speedRadiusGain = 0.3f;
    float speedDisplacementGain = 0.5f;
    float normalRadiusGain = 0.7f;
    float normalDisplacementGain = 0.9f;
};
struct FootContactState {
    bool observed = false;
    bool submerged = false;
    float travel = 0;
    std::uint32_t contacts = 0;
    Vec3 previous{};
};
struct FootContactResult {
    bool interacted = false;
    InteractionEvent event{};
    float radiusMetres = 0, displacementMetres = 0, normalSpeed = 0;
};
FootContactResult updateFootContact(Body& water, Vec3 sole, float dtSeconds,
                                    std::uint32_t contactId, FootContactState& state,
                                    const FootContactSettings& settings = {},
                                    std::uint32_t actorId = 0,
                                    ContactSource source = ContactSource::LeftFoot);

struct CouplingSettings {
    float densityKgM3 = 1000.0f;
    // Horizontal proxy columns per axis and vertical proxy layers. 4x4x4 is 16
    // height-field queries per body and resolves tilt without a full shape query.
    unsigned proxyColumns = 4;
    unsigned proxyLayers = 4;
    float linearDrag = 0.8f;      // 1/s * displaced mass
    float quadraticDrag = 1.0f;   // Cd against the horizontal footprint
    float angularDrag = 1.5f;     // 1/s against an approximated inertia
    // Force/volume change thresholds that wake a sleeping body.
    float wakeForceFraction = 0.02f;
    float wakeVolumeFraction = 0.01f;
    // Safety caps so one substep cannot reverse a fast body.
    float maxDragAcceleration = 50.0f;
    float maxAngularAcceleration = 30.0f;
};

struct CouplingInput {
    Vec3 centre{};
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    // World-space body axes. Defaults are the identity basis.
    Vec3 axisX{1, 0, 0}, axisY{0, 1, 0}, axisZ{0, 0, 1};
    Vec3 velocity{};
    Vec3 angularVelocity{};
    float massKg = 1.0f;
    // Submerged volume observed last tick, used only for the wake decision.
    float previousSubmergedVolume = 0.0f;
};

struct CouplingResult {
    bool valid = false;
    bool wake = false;
    float submergedVolume = 0, submergedFraction = 0;
    Vec3 centre{};   // centre of buoyancy
    Vec3 force{};    // buoyancy + drag
    Vec3 torque{};   // buoyancy lever arm + angular drag
};

CouplingResult coupleBox(const Body& water, const CouplingInput& input, Vec3 gravity,
                         const CouplingSettings& settings = {});

} // namespace engine::water
