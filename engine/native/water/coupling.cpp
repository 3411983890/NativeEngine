#include "coupling.hpp"

#include "../determinism.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::water {
namespace {

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

// Deterministic per-contact noise in [0,1). The seed is the quantized contact position
// plus the stride index, so replaying the same walk reproduces every wave exactly while
// two consecutive footsteps from nearby positions still differ.
float contactNoise(Vec3 feet, std::uint32_t stride, std::uint32_t channel) {
    std::uint64_t seed = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(feet.x * 1000.0f)));
    seed ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(feet.z * 1000.0f))) * 0xC2B2AE3D27D4EB4Full;
    seed ^= static_cast<std::uint64_t>(stride) * 0x165667B19E3779F9ull;
    seed ^= static_cast<std::uint64_t>(channel) * 0x27D4EB2F165667C5ull;
    return static_cast<float>(determinism::mix64(seed) >> 40) * (1.0f / 16777216.0f);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float magnitude(Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }

Vec3 scaledTo(Vec3 value, float length) {
    const float current = magnitude(value);
    return current > 1.0e-6f ? value * (length / current) : Vec3{};
}

} // namespace

WadingState wadingState(const Body& water, Vec3 feet, float heightMetres,
                        const WadingSettings& settings) {
    WadingState result;
    if (heightMetres <= 0) return result;
    const Query query = water.query(feet.x, feet.z);
    if (!query.valid) return result;
    result.depthMetres = query.depthMetres;
    const float immersion = std::min(std::max(query.surfaceHeightMetres - feet.y, 0.0f), heightMetres);
    result.immersionMetres = immersion;
    result.immersionFraction = immersion / heightMetres;
    result.wading = query.depthMetres > settings.minimumDepthMetres &&
                    immersion > settings.minimumImmersionMetres;
    if (!result.wading) return result;
    const float reference = std::max(1.0e-4f, settings.referenceImmersionFraction);
    const float t = clamp01(result.immersionFraction / reference);
    result.speedScale = std::max(0.0f, 1.0f - settings.speedLoss * t);
    result.verticalDampingPerSecond = settings.verticalDamping * t;
    result.jumpScale = std::max(0.0f, 1.0f - settings.jumpLoss * t);
    return result;
}

FootprintResult updateFootprint(Body& water, Vec3 feet, float heightMetres,
                                float dx, float dz, float dtSeconds,
                                FootprintState& state,
                                const FootprintSettings& settings,
                                float verticalSpeedMetresPerSecond) {
    FootprintResult result;
    if (!state.observed) {
        state.previous = feet;
        state.observed = true;
    }
    const float distance = std::sqrt(dx * dx + dz * dz);
    // A jump across the map is not a stride; it must not fabricate a wake.
    const bool teleport = distance >= settings.maximumStepMetres;
    state.travel += teleport ? 0.0f : distance;
    const Query query = water.query(feet.x, feet.z);
    const bool contact = query.valid &&
                         query.depthMetres > settings.minimumDepthMetres &&
                         feet.y < query.surfaceHeightMetres + settings.surfaceAboveFeetMetres &&
                         feet.y + heightMetres > query.surfaceHeightMetres;
    if (contact && (!state.contact || state.travel > settings.strideMetres)) {
        const float invDt = dtSeconds > 0 ? 1.0f / dtSeconds : 0.0f;
        // The footprint path already carries the stride displacement and its duration,
        // so the contact speed falls out of the same numbers. A teleport is not travel
        // and reports 0 instead of a fabricated 40 m/s landing.
        const float impactSpeed = teleport ? 0.0f
                                           : std::min(distance * invDt, settings.maximumImpactSpeed);
        const float speed01 = std::min(1.0f, impactSpeed / 4.0f);
        // A drop into the pond is not a step: the vertical arrival speed widens and
        // deepens the same kernel instead of adding a second, unrelated effect.
        const float normalSpeed = state.contact || teleport ? 0.0f
                                                            : std::max(0.0f, verticalSpeedMetresPerSecond);
        const float normal01 = std::min(1.0f, normalSpeed / 5.0f);
        const ContactKind kind = state.contact
                                     ? ContactKind::Drag
                                     : (normalSpeed > 2.0f ? ContactKind::Landing : ContactKind::FootPlant);
        // Every contact gets its own size and weight. Without this the pond received the
        // same ring for every step, which reads as a repeating animation rather than as
        // water reacting to this foot.
        const float radius = std::max(
            0.05f, settings.radiusMetres *
                       (1.0f + settings.radiusJitter * (2.0f * contactNoise(feet, state.strides, 0) - 1.0f) +
                        settings.speedRadiusGain * speed01 + 0.9f * normal01));
        const float depth = state.contact ? settings.moveDisplacementMetres
                                          : settings.entryDisplacementMetres;
        const float displacement = std::max(
            -0.9f, depth * (1.0f + settings.displacementJitter * (2.0f * contactNoise(feet, state.strides, 1) - 1.0f) +
                            settings.speedDisplacementGain * speed01 + 1.2f * normal01));
        Contact contactInput;
        contactInput.x = feet.x; contactInput.z = feet.z;
        contactInput.radius = radius; contactInput.displacement = displacement;
        contactInput.impulseXNs = dx * invDt * settings.impulseScale;
        contactInput.impulseZNs = dz * invDt * settings.impulseScale;
        contactInput.horizontalSpeed = impactSpeed;
        contactInput.normalSpeed = normalSpeed;
        contactInput.kind = kind;
        contactInput.source = ContactSource::ActorRoot;
        contactInput.displacedVolumeProxy =
            std::fabs(displacement) * 3.14159265f * radius * radius * 0.25f;
        if (water.interact(contactInput)) {
            result.interacted = true;
            result.event = water.events().back();
            result.radiusMetres = radius;
            result.displacementMetres = displacement;
            ++state.strides;
        }
        state.travel = 0;
    }
    state.contact = contact;
    state.previous = feet;
    return result;
}

FootContactResult updateFootContact(Body& water, Vec3 sole, float dtSeconds,
                                    std::uint32_t contactId, FootContactState& state,
                                    const FootContactSettings& settings,
                                    std::uint32_t actorId, ContactSource source) {
    FootContactResult result;
    if (!std::isfinite(sole.x) || !std::isfinite(sole.y) || !std::isfinite(sole.z) ||
        std::fabs(sole.x) > 1e5f || std::fabs(sole.y) > 1e5f || std::fabs(sole.z) > 1e5f)
        return result;
    if (!state.observed) {
        state.previous = sole;
        state.observed = true;
        const Query probe = water.query(sole.x, sole.z);
        state.submerged = probe.valid && probe.depthMetres > settings.minimumDepthMetres &&
                          sole.y < probe.surfaceHeightMetres - settings.contactMarginMetres;
        return result;
    }
    const float dx = sole.x - state.previous.x, dz = sole.z - state.previous.z;
    const float distance = std::sqrt(dx * dx + dz * dz);
    // A teleport (animation reset, respawn, scene reload) is not a stroke.
    const bool teleport = distance >= settings.maximumStepMetres;
    if (!teleport) state.travel += distance;
    const Query probe = water.query(sole.x, sole.z);
    const float surface = probe.valid ? probe.surfaceHeightMetres : std::numeric_limits<float>::lowest();
    const bool inContact = probe.valid && probe.depthMetres > settings.minimumDepthMetres &&
                           sole.y < surface - settings.contactMarginMetres;
    // Leaving needs a margin: a sole bobbing on the surface must not alternate between
    // entry and exit every tick.
    const bool left = state.submerged &&
                      (!inContact && (!probe.valid || sole.y > surface + settings.hysteresisMetres));
    // A teleport is a state reset, not a stroke: it adopts the new submergence without
    // fabricating an entry, a drag or a landing at the destination.
    if (teleport) {
        state.submerged = inContact;
        state.previous = sole;
        state.travel = 0;
        return result;
    }
    const float invDt = dtSeconds > 0 ? 1.0f / dtSeconds : 0.0f;
    const float horizontalSpeed = teleport ? 0.0f
                                           : std::min(distance * invDt, settings.maximumImpactSpeed);
    // The descent rate is the relative vertical speed of the sole: this is what makes a
    // jump read as a landing rather than as a very fast step.
    const float normalSpeed = (state.submerged || teleport || state.previous.y <= sole.y)
                                  ? 0.0f
                                  : std::min((state.previous.y - sole.y) * invDt, settings.maximumImpactSpeed);
    const float speed01 = std::min(1.0f, horizontalSpeed / 4.0f);
    const float normal01 = std::min(1.0f, normalSpeed / 5.0f);
    bool emit = false;
    ContactKind kind = ContactKind::Touch;
    float baseRadius = settings.radiusMetres;
    float baseDisplacement = settings.dragDisplacementMetres;
    float impulseX = dx * invDt * settings.impulseScale;
    float impulseZ = dz * invDt * settings.impulseScale;
    if (inContact && !state.submerged) {
        emit = true;
        kind = normalSpeed > 2.0f ? ContactKind::Landing : ContactKind::FootPlant;
        baseDisplacement = settings.entryDisplacementMetres;
    } else if (inContact && state.travel > settings.strideMetres) {
        // Dragging: integrated over distance, not one pulse per frame. A standing foot
        // accumulates no travel and therefore emits nothing.
        emit = true;
        kind = ContactKind::Drag;
        baseRadius = settings.radiusMetres * 0.82f;
    } else if (left) {
        // Leaving is deliberately weak and undirected; it must never reuse entry energy.
        emit = true;
        kind = ContactKind::Leaving;
        baseRadius = settings.radiusMetres * 0.7f;
        baseDisplacement = settings.leaveDisplacementMetres;
        impulseX = impulseZ = 0;
    }
    if (emit) {
        const float radius = std::max(
            0.03f, baseRadius *
                       (1.0f + settings.radiusJitter * (2.0f * contactNoise(sole, state.contacts, 0) - 1.0f) +
                        settings.speedRadiusGain * speed01 + settings.normalRadiusGain * normal01));
        const float displacement = std::max(
            -0.9f, baseDisplacement *
                       (1.0f + settings.displacementJitter * (2.0f * contactNoise(sole, state.contacts, 1) - 1.0f) +
                        settings.speedDisplacementGain * speed01 + settings.normalDisplacementGain * normal01));
        Contact input;
        input.x = sole.x; input.z = sole.z;
        input.radius = radius; input.displacement = displacement;
        input.impulseXNs = impulseX; input.impulseZNs = impulseZ;
        input.horizontalSpeed = horizontalSpeed;
        input.normalSpeed = normalSpeed;
        input.contactId = contactId;
        input.actorId = actorId;
        input.kind = kind;
        input.source = source;
        input.displacedVolumeProxy = std::fabs(displacement) * 3.14159265f * radius * radius * 0.25f;
        if (water.interact(input)) {
            result.interacted = true;
            result.event = water.events().back();
            result.radiusMetres = radius;
            result.displacementMetres = displacement;
            result.normalSpeed = normalSpeed;
            ++state.contacts;
        }
        state.travel = 0;
    }
    state.submerged = inContact;
    state.previous = sole;
    return result;
}

CouplingResult coupleBox(const Body& water, const CouplingInput& input, Vec3 gravity,
                         const CouplingSettings& settings) {
    CouplingResult result;
    const Vec3 half = input.halfExtents;
    if (half.x <= 0 || half.y <= 0 || half.z <= 0 || input.massKg <= 0) return result;
    const float totalVolume = 8.0f * half.x * half.y * half.z;
    const unsigned columns = std::max(1u, settings.proxyColumns);
    const unsigned layers = std::max(1u, settings.proxyLayers);
    const float voxelVolume = totalVolume / static_cast<float>(columns * columns * layers);
    const float worldHalfY = std::fabs(input.axisX.y) * half.x +
                             std::fabs(input.axisY.y) * half.y +
                             std::fabs(input.axisZ.y) * half.z;
    const float layerHalfY = std::max(1.0e-4f, worldHalfY / static_cast<float>(layers));

    Vec3 weighted{};
    float volume = 0, waterVelocityX = 0, waterVelocityZ = 0;
    unsigned validColumns = 0;
    for (unsigned iz = 0; iz < columns; ++iz)
        for (unsigned ix = 0; ix < columns; ++ix) {
            const float localX = -half.x + (static_cast<float>(ix) + 0.5f) * 2.0f * half.x /
                                             static_cast<float>(columns);
            const float localZ = -half.z + (static_cast<float>(iz) + 0.5f) * 2.0f * half.z /
                                             static_cast<float>(columns);
            const Vec3 base = input.centre + input.axisX * localX + input.axisZ * localZ;
            const Query query = water.query(base.x, base.z);
            if (query.valid) {
                waterVelocityX += query.velocityX;
                waterVelocityZ += query.velocityZ;
                ++validColumns;
            }
            const float bed = query.valid ? water.bedAt(base.x, base.z) : 0.0f;
            for (unsigned iy = 0; iy < layers; ++iy) {
                const float localY = -half.y + (static_cast<float>(iy) + 0.5f) * 2.0f * half.y /
                                                 static_cast<float>(layers);
                const Vec3 point = base + input.axisY * localY;
                float fraction = 0;
                if (query.valid && query.depthMetres > 0) {
                    const float bottom = std::max(point.y - layerHalfY, bed);
                    const float top = std::min(point.y + layerHalfY, query.surfaceHeightMetres);
                    if (top > bottom) fraction = clamp01((top - bottom) / (2.0f * layerHalfY));
                }
                if (fraction <= 0) continue;
                const float part = voxelVolume * fraction;
                volume += part;
                const float centreY = std::max(point.y - layerHalfY, bed) * 0.5f +
                                      std::min(point.y + layerHalfY, query.surfaceHeightMetres) * 0.5f;
                weighted += Vec3{point.x, centreY, point.z} * part;
            }
        }
    if (volume <= 1.0e-6f) return result;

    result.valid = true;
    result.submergedVolume = volume;
    result.submergedFraction = clamp01(volume / totalVolume);
    result.centre = weighted / volume;

    const Vec3 buoyant = gravity * (-settings.densityKgM3 * volume);
    const float inverseColumns = validColumns > 0 ? 1.0f / static_cast<float>(validColumns) : 0.0f;
    const Vec3 waterVelocity{waterVelocityX * inverseColumns, 0.0f, waterVelocityZ * inverseColumns};
    const Vec3 relative = input.velocity - waterVelocity;
    const float speed = magnitude(relative);
    const float horizontalArea = 4.0f * half.x * half.z;
    Vec3 drag = relative * (-0.5f * settings.densityKgM3 * settings.quadraticDrag *
                            horizontalArea * speed * result.submergedFraction);
    drag += relative * (-settings.linearDrag * settings.densityKgM3 * volume);
    const float maxDrag = input.massKg * settings.maxDragAcceleration;
    if (magnitude(drag) > maxDrag) drag = scaledTo(drag, maxDrag);

    result.force = buoyant + drag;
    result.torque = cross(result.centre - input.centre, buoyant);

    const float inertia = input.massKg * (half.x * half.x + half.y * half.y + half.z * half.z) / 3.0f;
    Vec3 angular = input.angularVelocity * (-settings.angularDrag * inertia * result.submergedFraction);
    const float maxAngularTorque = inertia * settings.maxAngularAcceleration;
    if (magnitude(angular) > maxAngularTorque) angular = scaledTo(angular, maxAngularTorque);
    result.torque += angular;

    const float weight = std::max(1.0e-3f, input.massKg * std::fabs(gravity.y));
    const float imbalance = std::fabs(result.force.y + gravity.y * input.massKg);
    const float horizontalForce = std::sqrt(result.force.x * result.force.x +
                                            result.force.z * result.force.z);
    const bool changed = std::fabs(volume - input.previousSubmergedVolume) >
                         settings.wakeVolumeFraction * totalVolume;
    result.wake = changed || imbalance > settings.wakeForceFraction * weight ||
                  horizontalForce > settings.wakeForceFraction * weight;
    return result;
}

} // namespace engine::water
