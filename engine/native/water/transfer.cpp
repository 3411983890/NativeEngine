#include "transfer.hpp"

#include <algorithm>
#include <cmath>

namespace engine::water {
namespace {
// Transfer settings are authored in water-local metres; the body primitives take world
// metres. A translated water body must move its source and landing with it.
Vec3 worldOf(const Body& body, float localX, float localZ) {
    const Vec3 origin = body.origin();
    return {origin.x + localX, origin.y, origin.z + localZ};
}
}

TransferLink::TransferLink(Body* upstream, Body* downstream, TransferSettings settings)
    : upstream_(upstream), downstream_(downstream), settings_(settings) {
    if (settings_.requestedFlowCubicMetresPerSecond < 0)
        throw Error("NG-E-WATER-TRANSFER", "Transfer flow must not be negative");
    if (settings_.transitSeconds < 0 || settings_.transitSeconds > 60)
        throw Error("NG-E-WATER-TRANSFER", "Transfer transit time is outside [0, 60] s");
    if (settings_.landingEventIntervalSeconds < 0 || settings_.landingEventIntervalSeconds > 2)
        throw Error("NG-E-WATER-TRANSFER", "Transfer landing event interval is outside [0, 2] s");
    if (!(settings_.sourceRadiusMetres > 0) || !(settings_.landingRadiusMetres > 0))
        throw Error("NG-E-WATER-TRANSFER", "Transfer regions need a positive radius");
}

void TransferLink::reset() {
    if (upstream_) {
        const Vec3 source = worldOf(*upstream_, settings_.sourceX, settings_.sourceZ);
        for (const auto& pending : pending_) {
            const float returned = upstream_->depositVolume(
                source.x, source.z, settings_.sourceRadiusMetres,
                pending.volumeCubicMetres, 0, 0);
            ledger_.returnedVolumeCubicMetres += returned;
            if (returned + 1e-9f < pending.volumeCubicMetres) {
                // The source region is the only legal owner; if it cannot take the water
                // back the volume is reported, not silently dropped.
                ledger_.lostVolumeCubicMetres += pending.volumeCubicMetres - returned;
            }
        }
    }
    pending_.clear();
    time_ = 0;
    lastLandingEventTime_ = -1.0;
}

void TransferLink::deliver(const Pending& arrival) {
    if (!downstream_) return;
    const Vec3 landing = worldOf(*downstream_, settings_.landingX, settings_.landingZ);
    const float accepted = downstream_->depositVolume(
        landing.x, landing.z, settings_.landingRadiusMetres,
        arrival.volumeCubicMetres, arrival.velocityX, arrival.velocityZ);
    ledger_.deliveredVolumeCubicMetres += accepted;
    if (accepted + 1e-9f < arrival.volumeCubicMetres)
        ledger_.lostVolumeCubicMetres += arrival.volumeCubicMetres - accepted;
    if (accepted <= 0) return;
    ++ledger_.arrivals;
    // Coalesce the visual contact. Every deposit above is still authoritative; the
    // droplet burst samples the flow instead of counting every arrival.
    const bool emit = settings_.landingEventIntervalSeconds <= 0 ||
        time_ - lastLandingEventTime_ >= double(settings_.landingEventIntervalSeconds);
    if (!emit) return;
    lastLandingEventTime_ = time_;
    ++ledger_.landingEvents;
    // The landing contact drives the ripple, foam and splash consumers. Its impulse is
    // zero: the momentum was already deposited with the volume, and adding it again
    // would double-count the impact.
    Contact contact;
    contact.x = landing.x;
    contact.z = landing.z;
    contact.radius = settings_.landingRadiusMetres;
    contact.displacement = settings_.landingDisplacementMetres;
    contact.horizontalSpeed = std::sqrt(arrival.velocityX * arrival.velocityX +
                                        arrival.velocityZ * arrival.velocityZ);
    contact.normalSpeed = std::max(0.0f, arrival.normalSpeed);
    contact.kind = ContactKind::Landing;
    contact.source = ContactSource::Flow;
    contact.displacedVolumeProxy = arrival.volumeCubicMetres;
    downstream_->interact(contact);
}

void TransferLink::step(float dtSeconds) {
    if (!valid()) return;
    if (!std::isfinite(dtSeconds) || dtSeconds <= 0 || dtSeconds > 0.25f)
        throw Error("NG-E-WATER-TIME", "Transfer step requires a finite tick in (0, 0.25] seconds");
    time_ += double(dtSeconds);
    // Arrivals are delivered before new withdrawal so a zero-transit link moves water in
    // the same tick it leaves. Volumes that left the upstream in an earlier tick are
    // still delivered when the link is disabled.
    std::vector<Pending> remaining;
    remaining.reserve(pending_.size());
    for (const auto& arrival : pending_) {
        if (arrival.arrivalTimeSeconds <= time_ + 1e-9) deliver(arrival);
        else remaining.push_back(arrival);
    }
    pending_.swap(remaining);

    if (!ledger_.enabled || !(settings_.requestedFlowCubicMetresPerSecond > 0)) {
        ledger_.inTransitVolumeCubicMetres = 0;
        for (const auto& arrival : pending_) ledger_.inTransitVolumeCubicMetres += arrival.volumeCubicMetres;
        return;
    }
    const double requested = double(settings_.requestedFlowCubicMetresPerSecond) * dtSeconds;
    ledger_.requestedVolumeCubicMetres += requested;
    const Vec3 source = worldOf(*upstream_, settings_.sourceX, settings_.sourceZ);
    const float actual = upstream_->withdrawVolume(source.x, source.z,
                                                   settings_.sourceRadiusMetres,
                                                   static_cast<float>(requested));
    ledger_.withdrawnVolumeCubicMetres += actual;
    ledger_.lastActualFlowCubicMetresPerSecond = actual / dtSeconds;
    ledger_.shortfallVolumeCubicMetres += std::max(0.0, requested - double(actual));
    if (actual > 0) {
        Pending pending;
        pending.volumeCubicMetres = actual;
        pending.velocityX = settings_.exitVelocityX;
        pending.velocityZ = settings_.exitVelocityZ;
        pending.normalSpeed = settings_.exitNormalSpeed;
        if (settings_.transitSeconds <= 0) {
            deliver(pending);
        } else {
            pending.arrivalTimeSeconds = time_ + double(settings_.transitSeconds);
            pending_.push_back(pending);
        }
    }
    ledger_.inTransitVolumeCubicMetres = 0;
    for (const auto& arrival : pending_) ledger_.inTransitVolumeCubicMetres += arrival.volumeCubicMetres;
}

TransferDiagnostics TransferLink::diagnostics() const {
    TransferDiagnostics result = ledger_;
    result.inTransitVolumeCubicMetres = 0;
    for (const auto& arrival : pending_) result.inTransitVolumeCubicMetres += arrival.volumeCubicMetres;
    return result;
}

double TransferLink::totalVolumeCubicMetres() const {
    double total = 0;
    if (upstream_) total += upstream_->diagnostics().volumeCubicMetres;
    if (downstream_) total += downstream_->diagnostics().volumeCubicMetres;
    for (const auto& arrival : pending_) total += arrival.volumeCubicMetres;
    return total;
}

} // namespace engine::water
