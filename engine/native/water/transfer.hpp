#pragma once

// Authoritative water transfer between two bodies.
//
// A waterfall is not a visual effect that adds water: the upstream body loses exactly
// what the downstream body (or the in-transit ledger) gains. The visual sheet and
// droplets sample this ledger; they never decide it. A drop is represented by removing
// volume from the source region, holding it in transit for the configured flight time,
// then depositing it at the landing region and emitting one landing contact for the
// ripple/splash consumers.

#include "body.hpp"

#include <vector>

namespace engine::water {

struct TransferSettings {
    // Water-local positions. The source region is drained; the landing region is filled.
    float sourceX = 0, sourceZ = 0, sourceRadiusMetres = 0.6f;
    float landingX = 0, landingZ = 0, landingRadiusMetres = 0.8f;
    float requestedFlowCubicMetresPerSecond = 0.1f;
    // Horizontal velocity carried into the downstream body and used by the landing
    // contact. Vertical speed is the downward arrival speed (m/s).
    float exitVelocityX = 0, exitVelocityZ = 0, exitNormalSpeed = 3.0f;
    // 0 delivers within the same step; otherwise the volume sits in the in-transit
    // ledger and is delivered when its arrival time is reached.
    float transitSeconds = 0.0f;
    // Zero-net-volume visual displacement of the landing contact.
    float landingDisplacementMetres = -0.06f;
    // Visual landing contacts are coalesced; every deposit still happens. A waterfall
    // lands every fixed tick, and one droplet burst per tick would exhaust the particle
    // runtime while adding no information.
    float landingEventIntervalSeconds = 0.08f;
};

struct TransferDiagnostics {
    bool enabled = true;
    double requestedVolumeCubicMetres = 0;
    double withdrawnVolumeCubicMetres = 0;
    double deliveredVolumeCubicMetres = 0;
    double inTransitVolumeCubicMetres = 0;
    // Flow actually moved in the last step, m³/s. Presentation uses this for sheet
    // opacity and scroll speed; it is not the requested value.
    float lastActualFlowCubicMetresPerSecond = 0;
    // Upstream had less water than requested; the difference was not invented.
    double shortfallVolumeCubicMetres = 0;
    // The landing region could not accept the volume (outside the body/solid).
    double lostVolumeCubicMetres = 0;
    // In-transit volume returned to the source by reset().
    double returnedVolumeCubicMetres = 0;
    std::uint32_t arrivals = 0, landingEvents = 0;
};

class TransferLink {
public:
    TransferLink(Body* upstream, Body* downstream, TransferSettings settings = {});
    // Returns in-transit water to the upstream body and clears the ledger. Used on scene
    // reload so a restart cannot deliver the same volume twice.
    void reset();
    void setEnabled(bool enabled) { ledger_.enabled = enabled; }
    bool enabled() const { return ledger_.enabled; }
    void setSettings(const TransferSettings& settings) { settings_ = settings; }
    const TransferSettings& settings() const { return settings_; }
    void step(float dtSeconds);
    TransferDiagnostics diagnostics() const;
    // Upstream + in transit + downstream. A conservation test compares this before and
    // after a session; the transfer must never create or destroy water.
    double totalVolumeCubicMetres() const;
    bool valid() const { return upstream_ && downstream_ && upstream_ != downstream_; }

private:
    struct Pending {
        double arrivalTimeSeconds = 0;
        float volumeCubicMetres = 0;
        float velocityX = 0, velocityZ = 0, normalSpeed = 0;
    };
    void deliver(const Pending& arrival);

    Body* upstream_ = nullptr;
    Body* downstream_ = nullptr;
    TransferSettings settings_{};
    double time_ = 0;
    // Time of the last visual landing contact; deposits are never dropped.
    double lastLandingEventTime_ = -1.0;
    std::vector<Pending> pending_;
    TransferDiagnostics ledger_{};
};

} // namespace engine::water
