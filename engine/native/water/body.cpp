#include "body.hpp"
#include "../determinism.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace engine::water {
namespace {
constexpr float Gravity = 9.81f;
constexpr float Pi = 3.14159265358979323846f;
constexpr float DryDepth = 1e-5f;
constexpr float WaterDensity = 1000.0f;
// A violent transient may need far more substeps than the asset budget allows.
// Instead of integrating with an unstable step, the unresolved time is carried
// up to this cap and then discarded explicitly through diagnostics.
constexpr double MaxTimeDebtSeconds = 0.25;
// The history is a presentation feed, not a queue with back-pressure. It is large
// enough that a renderer that missed a frame still receives every contact it can draw;
// anything evicted is counted instead of silently vanishing.
constexpr std::size_t MaxInteractionEvents = 128;
// A single transfer/drain call may not leave a cell completely dry, which would turn a
// neighbour's reconstruction into a discontinuity.
constexpr float MaxWithdrawalFraction = 0.95f;

bool finite(float value) { return std::isfinite(value); }

} // namespace

Body::Body(std::shared_ptr<const Asset> asset, Vec3 origin) : asset_(std::move(asset)), origin_(origin) {
    if (!asset_ || asset_->columns < 4 || asset_->rows < 4 || asset_->cellMetres <= 0)
        throw Error("NG-E-WATER-ASSET", "Invalid native water asset");
    if (!finite(origin.x) || !finite(origin.y) || !finite(origin.z))
        throw Error("NG-E-WATER-ASSET", "Water origin is not finite");
    buildGrid();
    reset();
}

void Body::buildGrid() {
    const auto cells = static_cast<std::size_t>(asset_->columns) * asset_->rows;
    bed_.assign(cells, 0);
    solid_.assign(cells, 0);
    depth_.assign(cells, 0);
    momentumX_.assign(cells, 0);
    momentumZ_.assign(cells, 0);
    nextDepth_.assign(cells, 0);
    nextMomentumX_.assign(cells, 0);
    nextMomentumZ_.assign(cells, 0);
    referenceDepth_.assign(cells, 0);
    const auto facesX = static_cast<std::size_t>(asset_->columns + 1) * asset_->rows;
    const auto facesZ = static_cast<std::size_t>(asset_->columns) * (asset_->rows + 1);
    fluxXH_.assign(facesX, 0); fluxXMomX_.assign(facesX, 0); fluxXMomZ_.assign(facesX, 0);
    fluxZH_.assign(facesZ, 0); fluxZMomX_.assign(facesZ, 0); fluxZMomZ_.assign(facesZ, 0);
    rebuildBed();
}

void Body::rebuildBed() {
    solidCells_ = 0;
    for (std::uint32_t row = 0; row < asset_->rows; ++row)
        for (std::uint32_t column = 0; column < asset_->columns; ++column) {
            const float x = -asset_->extentX * 0.5f + (column + 0.5f) * asset_->cellMetres;
            const float z = -asset_->extentZ * 0.5f + (row + 0.5f) * asset_->cellMetres;
            bed_[index(column, row)] = bedAtLocal(x, z);
            solid_[index(column, row)] = 0;
        }
    // Obstacles are applied after the analytic bed. A submerged rock simply raises the
    // bed, so the finite-volume update lets water pass over its top; only a footprint
    // that reaches the still level becomes impermeable.
    for (const auto& obstacle : asset_->obstacles) {
        if (!(obstacle.radiusMetres > 0)) continue;
        const float r2 = obstacle.radiusMetres * obstacle.radiusMetres;
        for (std::uint32_t row = 0; row < asset_->rows; ++row)
            for (std::uint32_t column = 0; column < asset_->columns; ++column) {
                const float x = -asset_->extentX * 0.5f + (column + 0.5f) * asset_->cellMetres;
                const float z = -asset_->extentZ * 0.5f + (row + 0.5f) * asset_->cellMetres;
                const float dx = x - obstacle.x, dz = z - obstacle.z;
                if (dx * dx + dz * dz > r2) continue;
                const auto cell = index(column, row);
                bed_[cell] = std::max(bed_[cell], obstacle.topHeightMetres);
                if (obstacle.topHeightMetres >= asset_->stillHeightMetres) {
                    solid_[cell] = 1;
                    ++solidCells_;
                }
            }
    }
}

float Body::bedAtLocal(float x, float z) const {
    const auto& bathymetry = asset_->bathymetry;
    if (bathymetry.type == BathymetryType::Slope)
        return bathymetry.heightMetres + bathymetry.slopeX * x + bathymetry.slopeZ * z;
    if (bathymetry.type == BathymetryType::Basin) {
        const float nx = x / (asset_->extentX * 0.5f), nz = z / (asset_->extentZ * 0.5f);
        const float radial = std::min(1.0f, nx * nx + nz * nz);
        return bathymetry.heightMetres - bathymetry.depthMetres * (1.0f - radial);
    }
    if (bathymetry.type == BathymetryType::Channel) {
        const float half = std::max(bathymetry.channelWidthMetres * 0.5f, 1e-3f);
        const float across = std::fabs(z);
        if (across <= half) {
            const float t = across / half;
            return bathymetry.heightMetres - bathymetry.depthMetres * (1.0f - t * t);
        }
        return bathymetry.heightMetres + bathymetry.channelBankSlope * (across - half);
    }
    float bed = bathymetry.heightMetres;
    for (const auto& mound : bathymetry.mounds) {
        if (!(mound.radiusMetres > 0)) continue;
        const float dx = x - mound.x, dz = z - mound.z;
        const float r2 = (dx * dx + dz * dz) / (mound.radiusMetres * mound.radiusMetres);
        if (r2 >= 1) continue;
        bed = std::max(bed, bathymetry.heightMetres + mound.heightMetres * (1.0f - r2));
    }
    return bed;
}

float Body::bedAt(float x, float z) const {
    // Shallow bodies carry their bed in the grid: obstacles and mounds live there, so
    // recomputing the analytic bathymetry would report a rock as if it were not present.
    if (asset_->backend == Backend::Shallow && inside(x, z)) {
        const float fx = std::clamp((x - origin_.x + asset_->extentX * 0.5f) / asset_->cellMetres - 0.5f,
                                    0.0f, static_cast<float>(asset_->columns - 1));
        const float fz = std::clamp((z - origin_.z + asset_->extentZ * 0.5f) / asset_->cellMetres - 0.5f,
                                    0.0f, static_cast<float>(asset_->rows - 1));
        const auto x0 = static_cast<std::uint32_t>(fx), z0 = static_cast<std::uint32_t>(fz);
        const auto x1 = std::min(x0 + 1, asset_->columns - 1), z1 = std::min(z0 + 1, asset_->rows - 1);
        const float tx = fx - x0, tz = fz - z0;
        const auto at = [&](std::uint32_t column, std::uint32_t row) { return bed_[index(column, row)]; };
        const float bed = (at(x0, z0) * (1 - tx) + at(x1, z0) * tx) * (1 - tz) +
                          (at(x0, z1) * (1 - tx) + at(x1, z1) * tx) * tz;
        return origin_.y + bed;
    }
    return origin_.y+bedAtLocal(x - origin_.x, z - origin_.z);
}

bool Body::inside(float x, float z) const {
    return std::fabs(x - origin_.x) <= asset_->extentX * 0.5f && std::fabs(z - origin_.z) <= asset_->extentZ * 0.5f;
}

void Body::reset() {
    ++timelineEpoch_;
    sequence_ = 0;
    for (std::size_t cell = 0; cell < depth_.size(); ++cell) {
        depth_[cell] = solid_[cell] ? 0.0f : std::max(0.0f, asset_->stillHeightMetres - bed_[cell]);
        momentumX_[cell] = 0;
        momentumZ_[cell] = 0;
        referenceDepth_[cell] = depth_[cell];
    }
    tick_ = 0;
    time_ = 0;
    substeps_ = 0;
    cflClamped_ = false;
    lastCfl_ = 0;
    timeDebt_ = 0;
    droppedTime_ = 0;
    sanitizedCells_ = 0;
    events_.clear();
    added_ = 0;
    removed_ = 0;
    initialVolume_ = 0;
    inflowRequested_ = inflowActual_ = outflowRequested_ = outflowActual_ = outflowLimited_ = 0;
    reactionImpulse_ = 0;
    for (std::size_t cell = 0; cell < depth_.size(); ++cell)
        initialVolume_ += depth_[cell] * asset_->cellMetres * asset_->cellMetres;
}

void Body::setLevel(float stillHeightMetres) {
    if (!finite(stillHeightMetres) || stillHeightMetres < -50 || stillHeightMetres > 200)
        throw Error("NG-E-WATER-LEVEL", "Water level is outside the supported range");
    auto copy = std::make_shared<Asset>(*asset_);
    copy->stillHeightMetres = stillHeightMetres;
    asset_ = copy;
    // A level edit can turn a submerged obstacle into a solid one (or back), so the
    // occupancy mask is rebuilt before the new baseline is measured.
    rebuildBed();
    ++timelineEpoch_;
    sequence_ = 0;
    for (std::size_t cell = 0; cell < depth_.size(); ++cell) {
        depth_[cell] = solid_[cell] ? 0.0f : std::max(0.0f, stillHeightMetres - bed_[cell]);
        momentumX_[cell] = 0;
        momentumZ_[cell] = 0;
        referenceDepth_[cell] = depth_[cell];
    }
    added_ = 0;
    removed_ = 0;
    initialVolume_ = 0;
    // A level edit teleports the surface; any time the solver failed to resolve
    // belongs to the discarded state, not to the new one.
    timeDebt_ = 0;
    lastCfl_ = 0;
    events_.clear();
    for (std::size_t cell = 0; cell < depth_.size(); ++cell)
        initialVolume_ += depth_[cell] * asset_->cellMetres * asset_->cellMetres;
}

void Body::addObstacle(const Obstacle& obstacle) {
    // Obstacles added at runtime are not part of the cooked asset hash, so the body owns
    // a shallow copy of the asset. Geometry identity stays in the diagnostics instead of
    // silently changing what the hash addresses.
    auto copy = std::make_shared<Asset>(*asset_);
    copy->obstacles.push_back(obstacle);
    asset_ = copy;
    rebuildBed();
    reset();
}

void Body::setDepth(std::uint32_t column, std::uint32_t row, float depthMetres) {
    if (column >= asset_->columns || row >= asset_->rows) throw Error("NG-E-WATER-QUERY", "Water cell out of range");
    if (!finite(depthMetres) || depthMetres < 0 || depthMetres > 200)
        throw Error("NG-E-WATER-LEVEL", "Water depth is outside the supported range");
    const auto cell = index(column, row);
    const float target = solid_[cell] ? 0.0f : depthMetres;
    const float delta = (target - depth_[cell]) * asset_->cellMetres * asset_->cellMetres;
    depth_[cell] = target;
    referenceDepth_[cell] = target;
    momentumX_[cell] = 0;
    momentumZ_[cell] = 0;
    initialVolume_ += delta;
}

bool Body::interact(float x,float z,float radius,float displacement,float impulseX,float impulseZ,float impactSpeed) {
    Contact contact;
    contact.x = x; contact.z = z; contact.radius = radius; contact.displacement = displacement;
    contact.impulseXNs = impulseX; contact.impulseZNs = impulseZ;
    contact.horizontalSpeed = impactSpeed;
    return interact(contact);
}

bool Body::interact(const Contact& contact) {
    const float x = contact.x, z = contact.z, radius = contact.radius, displacement = contact.displacement;
    const float impulseX = contact.impulseXNs, impulseZ = contact.impulseZNs;
    const float impactSpeed = contact.horizontalSpeed;
    if(!finite(x)||!finite(z)||!finite(radius)||radius<=0||radius>20||!finite(displacement)||std::fabs(displacement)>1||
       !finite(impulseX)||!finite(impulseZ)||std::fabs(impulseX)>1.e5f||std::fabs(impulseZ)>1.e5f||
       !finite(impactSpeed)||impactSpeed<0||impactSpeed>50||
       !finite(contact.normalSpeed)||contact.normalSpeed<0||contact.normalSpeed>50||
       !finite(contact.displacedVolumeProxy)||contact.displacedVolumeProxy<0||contact.displacedVolumeProxy>100)
        throw Error("NG-E-WATER-INTERACTION","Invalid water interaction input");
    if(asset_->backend!=Backend::Shallow||!inside(x,z))return false;
    // A contact that is travelling leaves a bow wave, not a perfect circle: the kernel is
    // stretched along the impulse direction by an amount that grows with the recorded
    // contact speed. A stomp (no horizontal impulse) stays radial, so the same routine
    // still describes standing still in the water.
    const float impulseMagnitude=std::sqrt(impulseX*impulseX+impulseZ*impulseZ);
    const bool directed=impulseMagnitude>1.e-4f;
    const float alongX=directed?impulseX/impulseMagnitude:0.f;
    const float alongZ=directed?impulseZ/impulseMagnitude:0.f;
    const float motionSpeed=std::max(impactSpeed, contact.normalSpeed);
    const float stretch=directed?1.f/(1.f+std::min(.55f,motionSpeed*.12f)):1.f;
    struct Cell {std::size_t i;float inner,outer;};std::vector<Cell> cells;
    double sumInner=0,sumOuter=0;
    for(std::uint32_t row=0;row<asset_->rows;++row)for(std::uint32_t col=0;col<asset_->columns;++col){
        const auto i=index(col,row);if(depth_[i]<=DryDepth||solid_[i])continue;
        const float dx=origin_.x-asset_->extentX*.5f+(col+.5f)*asset_->cellMetres-x;
        const float dz=origin_.z-asset_->extentZ*.5f+(row+.5f)*asset_->cellMetres-z;
        float r;
        if(directed){const float along=(dx*alongX+dz*alongZ)*stretch,across=dx*alongZ-dz*alongX;
            r=(along*along+across*across)/(radius*radius);}
        else r=(dx*dx+dz*dz)/(radius*radius);
        if(r>1)continue;
        const float inner=std::exp(-12*r),outer=std::exp(-3*r);cells.push_back({i,inner,outer});sumInner+=inner;sumOuter+=outer;
    }
    if(cells.size()<3||sumInner<=0||sumOuter<=0)return false;
    const float ratio=float(sumInner/sumOuter);float scale=1;
    for(const auto& c:cells){const float delta=displacement*(c.inner-ratio*c.outer);if(delta<0)scale=std::min(scale,(depth_[c.i]-DryDepth)*.95f/-delta);}
    const float area=asset_->cellMetres*asset_->cellMetres;
    for(const auto& c:cells){depth_[c.i]+=scale*displacement*(c.inner-ratio*c.outer);const float weight=float(c.inner/sumInner)/(WaterDensity*area);momentumX_[c.i]+=impulseX*weight;momentumZ_[c.i]+=impulseZ*weight;}
    InteractionEvent event;
    event.x=x;event.z=z;event.radius=radius;event.displacement=scale*displacement;event.impactSpeed=impactSpeed;event.tick=tick_+1;
    event.directionX=alongX;event.directionZ=alongZ;event.stretch=stretch;
    // Sequences start at 1 so a fresh cursor (0) cannot mistake the first contact for one
    // it has already delivered.
    event.sequence=++sequence_;
    event.timelineEpoch=timelineEpoch_;
    event.contactId=contact.contactId;
    event.actorId=contact.actorId;
    event.kind=static_cast<std::uint8_t>(contact.kind);
    event.source=static_cast<std::uint8_t>(contact.source);
    event.normalSpeed=contact.normalSpeed;
    event.displacedVolumeProxy=contact.displacedVolumeProxy;
    events_.push_back(event);
    if(events_.size()>MaxInteractionEvents){
        const auto evicted=events_.size()-MaxInteractionEvents;
        droppedEvents_+=evicted;
        events_.erase(events_.begin(),events_.begin()+static_cast<std::ptrdiff_t>(evicted));
    }
    return true;
}

float Body::applyImpulse(float x, float z, float radius, float impulseX, float impulseZ) {
    if (!finite(x) || !finite(z) || !finite(radius) || radius <= 0 || radius > 20 ||
        !finite(impulseX) || !finite(impulseZ) || std::fabs(impulseX) > 1e6f || std::fabs(impulseZ) > 1e6f)
        throw Error("NG-E-WATER-INTERACTION", "Invalid water reaction input");
    if (asset_->backend != Backend::Shallow || !inside(x, z)) return 0;
    double weightSum = 0;
    forCellsInDisc(x, z, radius, [&](std::size_t cell, float) {
        if (depth_[cell] > DryDepth) weightSum += depth_[cell];
    });
    if (weightSum <= 0) return 0;
    const float area = asset_->cellMetres * asset_->cellMetres;
    forCellsInDisc(x, z, radius, [&](std::size_t cell, float) {
        if (depth_[cell] <= DryDepth) return;
        const float share = float(depth_[cell] / weightSum);
        momentumX_[cell] += impulseX * share / (WaterDensity * area);
        momentumZ_[cell] += impulseZ * share / (WaterDensity * area);
    });
    const float magnitude = std::sqrt(impulseX * impulseX + impulseZ * impulseZ);
    reactionImpulse_ += magnitude;
    return magnitude;
}

float Body::withdrawVolume(float x, float z, float radius, float volume) {
    if (!finite(x) || !finite(z) || !finite(radius) || radius <= 0 || radius > 20 ||
        !finite(volume) || volume < 0 || volume > 1e6f)
        throw Error("NG-E-WATER-TRANSFER", "Invalid water withdrawal");
    if (asset_->backend != Backend::Shallow || volume <= 0 || !inside(x, z)) return 0;
    const float area = asset_->cellMetres * asset_->cellMetres;
    double available = 0;
    forCellsInDisc(x, z, radius, [&](std::size_t cell, float) {
        if (depth_[cell] > DryDepth) available += double(depth_[cell]) * area;
    });
    if (available <= 1e-9) return 0;
    const float fraction = std::min(MaxWithdrawalFraction, float(volume / available));
    const float actual = float(available) * fraction;
    forCellsInDisc(x, z, radius, [&](std::size_t cell, float) {
        if (depth_[cell] <= DryDepth) return;
        depth_[cell] *= 1.0f - fraction;
        momentumX_[cell] *= 1.0f - fraction;
        momentumZ_[cell] *= 1.0f - fraction;
    });
    removed_ += actual;
    return actual;
}

float Body::depositVolume(float x, float z, float radius, float volume, float velocityX, float velocityZ) {
    if (!finite(x) || !finite(z) || !finite(radius) || radius <= 0 || radius > 20 ||
        !finite(volume) || volume <= 0 || volume > 1e6f ||
        !finite(velocityX) || !finite(velocityZ))
        throw Error("NG-E-WATER-TRANSFER", "Invalid water deposit");
    if (asset_->backend != Backend::Shallow || !inside(x, z)) return 0;
    std::uint32_t count = 0;
    forCellsInDisc(x, z, radius, [&](std::size_t, float) { ++count; });
    if (count == 0) return 0;
    const float addedDepth = volume / (static_cast<float>(count) * asset_->cellMetres * asset_->cellMetres);
    forCellsInDisc(x, z, radius, [&](std::size_t cell, float) {
        depth_[cell] += addedDepth;
        momentumX_[cell] += addedDepth * velocityX;
        momentumZ_[cell] += addedDepth * velocityZ;
    });
    added_ += volume;
    return volume;
}

template <class Visit>
void Body::forCellsInDisc(float x, float z, float radius, Visit&& visit) {
    for (std::uint32_t row = 0; row < asset_->rows; ++row)
        for (std::uint32_t column = 0; column < asset_->columns; ++column) {
            const float px = origin_.x - asset_->extentX * 0.5f + (column + 0.5f) * asset_->cellMetres;
            const float pz = origin_.z - asset_->extentZ * 0.5f + (row + 0.5f) * asset_->cellMetres;
            const float dx = px - x, dz = pz - z;
            const float r2 = dx * dx + dz * dz;
            if (r2 > radius * radius) continue;
            const auto cell = index(column, row);
            if (solid_[cell]) continue;
            visit(cell, r2);
        }
}

float Body::depthAt(std::uint32_t column, std::uint32_t row) const {
    if (column >= asset_->columns || row >= asset_->rows) throw Error("NG-E-WATER-QUERY", "Water cell out of range");
    return depth_[index(column, row)];
}

bool Body::solidAt(std::uint32_t column, std::uint32_t row) const {
    if (column >= asset_->columns || row >= asset_->rows) throw Error("NG-E-WATER-QUERY", "Water cell out of range");
    return solid_[index(column, row)] != 0;
}

void Body::step(float dtSeconds) {
    lastStepMilliseconds_ = 0;
    if (!std::isfinite(dtSeconds) || dtSeconds <= 0 || dtSeconds > 0.25f)
        throw Error("NG-E-WATER-TIME", "Water step requires a finite tick in (0, 0.25] seconds");
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&] {
        lastStepMilliseconds_ = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    };
    inflowRequested_ = inflowActual_ = outflowRequested_ = outflowActual_ = outflowLimited_ = 0;
    sourceCells_ = drainCells_ = 0;
    if (asset_->backend != Backend::Shallow) {
        // Analytic and spectral surfaces are query-only: advancing them moves the phase,
        // it does not integrate a volume.
        time_ += dtSeconds;
        ++tick_;
        substeps_ = 0;
        lastCfl_ = 0;
        finish();
        return;
    }
    // A velocity whose CFL bound falls below this floor cannot be resolved by any
    // meaningful substep count; the kinetic part of that cell is outside the model.
    constexpr double MinSubstepSeconds = 1e-6;
    const double maxResolvableSpeed = 0.4 * double(asset_->cellMetres) / MinSubstepSeconds;
    double maxSpeed = 0;
    for (std::size_t cell = 0; cell < depth_.size(); ++cell) {
        if (depth_[cell] <= DryDepth) continue;
        const double u = double(momentumX_[cell]) / depth_[cell];
        const double v = double(momentumZ_[cell]) / depth_[cell];
        const double wave = std::sqrt(double(Gravity) * depth_[cell]);
        double speed = std::sqrt(u * u + v * v) + wave;
        if (!std::isfinite(speed) || speed > maxResolvableSpeed) {
            momentumX_[cell] = momentumZ_[cell] = 0;
            ++sanitizedCells_;
            speed = wave;
        }
        maxSpeed = std::max(maxSpeed, speed);
    }
    // The cell-centre estimate bounds the reconstructed face speeds, so the same
    // bound covers the Rusanov flux. Calm water has no meaningful acoustic limit.
    const bool bounded = maxSpeed > 1e-4;
    const float stableDt = bounded ? static_cast<float>(0.4 * double(asset_->cellMetres) / maxSpeed)
                                   : std::numeric_limits<float>::max();
    const float capacity = bounded ? stableDt * static_cast<float>(asset_->maxSubsteps)
                                   : std::numeric_limits<float>::max();
    // Never simulate more than this tick plus one tick of outstanding debt, and
    // never more than the substep budget can resolve stably.
    const float requested = dtSeconds + std::min(static_cast<float>(timeDebt_), dtSeconds);
    float budgetDt = std::min(requested, capacity);
    const auto required = static_cast<std::uint32_t>(std::max(1.0f, std::ceil(budgetDt / stableDt)));
    substeps_ = std::min(asset_->maxSubsteps, required);
    // Clamping the substep count can leave the requested span marginally above the
    // CFL bound; shrink the span instead of integrating past the bound.
    const float h = bounded ? std::min(stableDt, budgetDt / static_cast<float>(substeps_))
                            : budgetDt / static_cast<float>(substeps_);
    budgetDt = h * static_cast<float>(substeps_);
    lastCfl_ = bounded ? h / stableDt : 0;
    for (std::uint32_t substep = 0; substep < substeps_; ++substep) advance(h);
    cflClamped_ = budgetDt + 1e-6f < dtSeconds;
    timeDebt_ += double(dtSeconds) - double(budgetDt);
    if (timeDebt_ < 0) timeDebt_ = 0;
    if (timeDebt_ > MaxTimeDebtSeconds) {
        droppedTime_ += timeDebt_ - MaxTimeDebtSeconds;
        timeDebt_ = MaxTimeDebtSeconds;
    }
    time_ += budgetDt;
    ++tick_;
    finish();
}

void Body::advance(float h) {
    const auto& asset = *asset_;
    const float dx = asset.cellMetres, dz = asset.cellMetres;
    const auto velocity = [](float depth, float momentum) { return depth > DryDepth ? momentum / depth : 0.0f; };
    const auto faceX = [&](std::uint32_t column, std::uint32_t row) {
        return static_cast<std::size_t>(row) * (asset.columns + 1) + column;
    };
    const auto faceZ = [&](std::uint32_t column, std::uint32_t row) {
        return static_cast<std::size_t>(row) * asset.columns + column;
    };

    for (std::uint32_t row = 0; row < asset.rows; ++row)
        for (std::uint32_t column = 0; column <= asset.columns; ++column) {
            const auto face = faceX(column, row);
            fluxXH_[face] = fluxXMomX_[face] = fluxXMomZ_[face] = 0;
            if (column == 0 || column == asset.columns) {
                if (!asset.openBoundaries) continue; // Closed wall: mass flux stays zero.
                const std::uint32_t cell = index(column == 0 ? 0 : asset.columns - 1, row);
                const float depth = depth_[cell];
                const float u = velocity(depth, momentumX_[cell]), v = velocity(depth, momentumZ_[cell]);
                fluxXH_[face] = depth * u;
                fluxXMomX_[face] = depth * u * u;
                fluxXMomZ_[face] = depth * u * v;
                continue;
            }
            const std::uint32_t left = index(column - 1, row), right = index(column, row);
            // An impermeable cell blocks normal flux. Pressure on the wet side is applied
            // in the cell update, not as a face flux, so the wall cannot pump the pool.
            if (solid_[left] || solid_[right]) continue;
            const float bedLeft = bed_[left], bedRight = bed_[right], bedMax = std::max(bedLeft, bedRight);
            const float hLeft = std::max(0.0f, depth_[left] + bedLeft - bedMax);
            const float hRight = std::max(0.0f, depth_[right] + bedRight - bedMax);
            const float uLeft = velocity(depth_[left], momentumX_[left]);
            const float uRight = velocity(depth_[right], momentumX_[right]);
            const float speed = std::max(std::fabs(uLeft) + std::sqrt(Gravity * hLeft),
                                         std::fabs(uRight) + std::sqrt(Gravity * hRight));
            const float massLeft = hLeft * uLeft, massRight = hRight * uRight;
            const float advectLeft = massLeft * uLeft, advectRight = massRight * uRight;
            fluxXH_[face] = 0.5f * (massLeft + massRight) - 0.5f * speed * (hRight - hLeft);
            // The hydrostatic pressure is applied as a surface-gradient force in the cell
            // update. Combining it into this flux and adding a separate bed source is what
            // made a sloped-but-still pool accelerate downhill; the surface form cancels by
            // construction whenever eta is level.
            fluxXMomX_[face] = 0.5f * (advectLeft + advectRight) - 0.5f * speed * (massRight - massLeft);
            const float transverse = fluxXH_[face] >= 0 ? velocity(depth_[left], momentumZ_[left])
                                                        : velocity(depth_[right], momentumZ_[right]);
            fluxXMomZ_[face] = transverse * fluxXH_[face];
        }

    for (std::uint32_t row = 0; row <= asset.rows; ++row)
        for (std::uint32_t column = 0; column < asset.columns; ++column) {
            const auto face = faceZ(column, row);
            fluxZH_[face] = fluxZMomX_[face] = fluxZMomZ_[face] = 0;
            if (row == 0 || row == asset.rows) {
                if (!asset.openBoundaries) continue;
                const std::uint32_t cell = index(column, row == 0 ? 0 : asset.rows - 1);
                const float depth = depth_[cell];
                const float u = velocity(depth, momentumX_[cell]), v = velocity(depth, momentumZ_[cell]);
                fluxZH_[face] = depth * v;
                fluxZMomX_[face] = depth * u * v;
                fluxZMomZ_[face] = depth * v * v;
                continue;
            }
            const std::uint32_t back = index(column, row - 1), front = index(column, row);
            if (solid_[back] || solid_[front]) continue;
            const float bedBack = bed_[back], bedFront = bed_[front], bedMax = std::max(bedBack, bedFront);
            const float hBack = std::max(0.0f, depth_[back] + bedBack - bedMax);
            const float hFront = std::max(0.0f, depth_[front] + bedFront - bedMax);
            const float vBack = velocity(depth_[back], momentumZ_[back]);
            const float vFront = velocity(depth_[front], momentumZ_[front]);
            const float speed = std::max(std::fabs(vBack) + std::sqrt(Gravity * hBack),
                                         std::fabs(vFront) + std::sqrt(Gravity * hFront));
            const float massBack = hBack * vBack, massFront = hFront * vFront;
            const float advectBack = massBack * vBack, advectFront = massFront * vFront;
            fluxZH_[face] = 0.5f * (massBack + massFront) - 0.5f * speed * (hFront - hBack);
            fluxZMomZ_[face] = 0.5f * (advectBack + advectFront) - 0.5f * speed * (massFront - massBack);
            const float transverse = fluxZH_[face] >= 0 ? velocity(depth_[back], momentumX_[back])
                                                        : velocity(depth_[front], momentumX_[front]);
            fluxZMomX_[face] = transverse * fluxZH_[face];
        }

    const auto pressureForce = [&](std::uint32_t column, std::uint32_t row, int dc, int dr,
                                   float depth, float eta, float& force) {
        const int neighborColumn = int(column) + dc, neighborRow = int(row) + dr;
        if (neighborColumn < 0 || neighborRow < 0 || neighborColumn >= int(asset.columns) ||
            neighborRow >= int(asset.rows)) {
            // A closed boundary carrying no mass is also a zero surface gradient in this
            // formulation. Adding a separate 0.5 g h^2 wall push (which the old pressure
            // flux needed) would accelerate a still pool at every boundary cell.
            return;
        }
        const auto neighbor = index(static_cast<std::uint32_t>(neighborColumn), static_cast<std::uint32_t>(neighborRow));
        if (solid_[neighbor]) return;
        const float neighborDepth = depth_[neighbor];
        if (neighborDepth <= DryDepth) return;
        const float neighborEta = bed_[neighbor] + neighborDepth;
        const float faceDepth = 0.5f * (depth + neighborDepth);
        // +x/+z orientation: a higher surface on the -x/-z side pushes the cell forward.
        force += (dc < 0 || dr < 0 ? 1.0f : -1.0f) * Gravity * faceDepth * (neighborEta - eta);
    };

    for (std::uint32_t row = 0; row < asset.rows; ++row)
        for (std::uint32_t column = 0; column < asset.columns; ++column) {
            const auto cell = index(column, row);
            if (solid_[cell]) {
                nextDepth_[cell] = 0;
                nextMomentumX_[cell] = 0;
                nextMomentumZ_[cell] = 0;
                continue;
            }
            const float massWest = fluxXH_[faceX(column, row)], massEast = fluxXH_[faceX(column + 1, row)];
            const float momentumWest = fluxXMomX_[faceX(column, row)], momentumEast = fluxXMomX_[faceX(column + 1, row)];
            const float transverseWest = fluxXMomZ_[faceX(column, row)], transverseEast = fluxXMomZ_[faceX(column + 1, row)];
            const float massSouth = fluxZH_[faceZ(column, row)], massNorth = fluxZH_[faceZ(column, row + 1)];
            const float momentumSouth = fluxZMomZ_[faceZ(column, row)], momentumNorth = fluxZMomZ_[faceZ(column, row + 1)];
            const float transverseSouth = fluxZMomX_[faceZ(column, row)], transverseNorth = fluxZMomX_[faceZ(column, row + 1)];
            const float depth = depth_[cell], eta = bed_[cell] + depth;
            float forceX = 0, forceZ = 0;
            pressureForce(column, row, -1, 0, depth, eta, forceX);
            pressureForce(column, row, 1, 0, depth, eta, forceX);
            pressureForce(column, row, 0, -1, depth, eta, forceZ);
            pressureForce(column, row, 0, 1, depth, eta, forceZ);
            nextDepth_[cell] = depth + h * ((massWest - massEast) / dx + (massSouth - massNorth) / dz);
            nextMomentumX_[cell] = momentumX_[cell] + h *
                ((momentumWest - momentumEast) / dx + (transverseSouth - transverseNorth) / dz + forceX / dx);
            nextMomentumZ_[cell] = momentumZ_[cell] + h *
                ((transverseWest - transverseEast) / dx + (momentumSouth - momentumNorth) / dz + forceZ / dz);
        }

    // Sources and sinks are applied after the conservative update and are recorded in the
    // ledger, so a closed pool has to balance to the last cubic metre.
    const float cellArea = dx * dz;
    const std::uint32_t fluidCells = static_cast<std::uint32_t>(depth_.size()) - solidCells_;
    const float totalArea = cellArea * static_cast<float>(fluidCells);
    if (asset.inflowCubicMetresPerSecond > 0 && totalArea > 0) {
        const float added = asset.inflowCubicMetresPerSecond * h;
        const float addedDepth = added / totalArea;
        added_ += added;
        inflowRequested_ += added;
        inflowActual_ += added;
        for (std::size_t cell = 0; cell < nextDepth_.size(); ++cell)
            if (!solid_[cell]) nextDepth_[cell] += addedDepth;
    }
    if (asset.outflowCubicMetresPerSecond > 0) {
        float volume = 0;
        for (std::size_t cell = 0; cell < nextDepth_.size(); ++cell) volume += std::max(0.0f, nextDepth_[cell]) * cellArea;
        const float requestedVolume = asset.outflowCubicMetresPerSecond * h;
        const float removed = std::min(requestedVolume, volume * 0.5f);
        outflowRequested_ += requestedVolume;
        outflowActual_ += removed;
        outflowLimited_ += std::max(0.0f, requestedVolume - removed);
        removed_ += removed;
        const float fraction = volume > 0 ? std::min(0.5f, removed / volume) : 0.0f;
        if (fraction > 0)
            for (std::size_t cell = 0; cell < nextDepth_.size(); ++cell) {
                nextDepth_[cell] *= (1.0f - fraction);
                nextMomentumX_[cell] *= (1.0f - fraction);
                nextMomentumZ_[cell] *= (1.0f - fraction);
            }
    }
    // Localised openings use the same ledger. Volume is normalised by the opening's
    // effective (non-solid) area, so changing the grid resolution does not change Q.
    for (const auto& source : asset.sources) {
        if (!(source.flowCubicMetresPerSecond > 0) || !(source.radiusMetres > 0)) continue;
        const float sx = origin_.x + source.x, sz = origin_.z + source.z;
        std::uint32_t count = 0;
        for (std::uint32_t row = 0; row < asset.rows; ++row)
            for (std::uint32_t column = 0; column < asset.columns; ++column) {
                const auto cell = index(column, row);
                if (solid_[cell]) continue;
                const float px = origin_.x - asset.extentX * 0.5f + (column + 0.5f) * dx;
                const float pz = origin_.z - asset.extentZ * 0.5f + (row + 0.5f) * dz;
                const float ddx = px - sx, ddz = pz - sz;
                if (ddx * ddx + ddz * ddz <= source.radiusMetres * source.radiusMetres) ++count;
            }
        sourceCells_ = std::max(sourceCells_, count);
        const float requestedVolume = source.flowCubicMetresPerSecond * h;
        inflowRequested_ += requestedVolume;
        if (count == 0) continue;
        const float addedDepth = requestedVolume / (static_cast<float>(count) * cellArea);
        added_ += requestedVolume;
        inflowActual_ += requestedVolume;
        const float velocityX = source.directionX * source.speedMetresPerSecond;
        const float velocityZ = source.directionZ * source.speedMetresPerSecond;
        for (std::uint32_t row = 0; row < asset.rows; ++row)
            for (std::uint32_t column = 0; column < asset.columns; ++column) {
                const auto cell = index(column, row);
                if (solid_[cell]) continue;
                const float px = origin_.x - asset.extentX * 0.5f + (column + 0.5f) * dx;
                const float pz = origin_.z - asset.extentZ * 0.5f + (row + 0.5f) * dz;
                const float ddx = px - sx, ddz = pz - sz;
                if (ddx * ddx + ddz * ddz > source.radiusMetres * source.radiusMetres) continue;
                nextDepth_[cell] += addedDepth;
                nextMomentumX_[cell] += addedDepth * velocityX;
                nextMomentumZ_[cell] += addedDepth * velocityZ;
            }
    }
    for (const auto& drain : asset.drains) {
        if (!(drain.flowCubicMetresPerSecond > 0) || !(drain.radiusMetres > 0)) continue;
        const float dxWorld = origin_.x + drain.x, dzWorld = origin_.z + drain.z;
        double available = 0;
        std::uint32_t count = 0;
        for (std::uint32_t row = 0; row < asset.rows; ++row)
            for (std::uint32_t column = 0; column < asset.columns; ++column) {
                const auto cell = index(column, row);
                if (solid_[cell]) continue;
                const float px = origin_.x - asset.extentX * 0.5f + (column + 0.5f) * dx;
                const float pz = origin_.z - asset.extentZ * 0.5f + (row + 0.5f) * dz;
                const float ddx = px - dxWorld, ddz = pz - dzWorld;
                if (ddx * ddx + ddz * ddz > drain.radiusMetres * drain.radiusMetres) continue;
                ++count;
                available += std::max(0.0f, nextDepth_[cell]) * cellArea;
            }
        drainCells_ = std::max(drainCells_, count);
        const float requestedVolume = drain.flowCubicMetresPerSecond * h;
        outflowRequested_ += requestedVolume;
        const float actual = std::min(requestedVolume, static_cast<float>(available));
        outflowLimited_ += std::max(0.0f, requestedVolume - actual);
        if (actual <= 0 || available <= 1e-9) continue;
        const float fraction = float(actual / available);
        outflowActual_ += actual;
        removed_ += actual;
        for (std::uint32_t row = 0; row < asset.rows; ++row)
            for (std::uint32_t column = 0; column < asset.columns; ++column) {
                const auto cell = index(column, row);
                if (solid_[cell]) continue;
                const float px = origin_.x - asset.extentX * 0.5f + (column + 0.5f) * dx;
                const float pz = origin_.z - asset.extentZ * 0.5f + (row + 0.5f) * dz;
                const float ddx = px - dxWorld, ddz = pz - dzWorld;
                if (ddx * ddx + ddz * ddz > drain.radiusMetres * drain.radiusMetres) continue;
                nextDepth_[cell] *= 1.0f - fraction;
                nextMomentumX_[cell] *= 1.0f - fraction;
                nextMomentumZ_[cell] *= 1.0f - fraction;
            }
    }

    for (std::size_t cell = 0; cell < nextDepth_.size(); ++cell) {
        if (solid_[cell]) {
            nextDepth_[cell] = 0;
            nextMomentumX_[cell] = 0;
            nextMomentumZ_[cell] = 0;
            continue;
        }
        float depth = nextDepth_[cell];
        float momentumX = nextMomentumX_[cell], momentumZ = nextMomentumZ_[cell];
        if (!finite(depth) || !finite(momentumX) || !finite(momentumZ)) {
            ++sanitizedCells_;
            depth = 0;
            momentumX = momentumZ = 0;
        }
        if (depth < DryDepth) { depth = 0; momentumX = momentumZ = 0; }
        else {
            const float speed = std::sqrt(momentumX * momentumX + momentumZ * momentumZ) / depth;
            const float friction = 1.0f / (1.0f + h * 0.02f * speed / std::max(depth, 0.05f));
            momentumX *= friction; momentumZ *= friction;
        }
        nextDepth_[cell] = depth;
        nextMomentumX_[cell] = momentumX;
        nextMomentumZ_[cell] = momentumZ;
    }
    depth_.swap(nextDepth_);
    momentumX_.swap(nextMomentumX_);
    momentumZ_.swap(nextMomentumZ_);
}

float Body::waveHeight(float x, float z, float& slopeX, float& slopeZ) const {
    const auto& waves = asset_->waves;
    slopeX = slopeZ = 0;
    if (waves.amplitudeMetres <= 0 || waves.components == 0) return 0;
    const bool spectral = asset_->backend == Backend::Spectrum;
    float height = 0;
    for (std::uint32_t component = 0; component < waves.components; ++component) {
        const float angle = std::atan2(waves.directionZ, waves.directionX) +
            (spectral ? static_cast<float>(component) * 2.399963f : static_cast<float>(component) * 0.7f);
        const float dirX = std::cos(angle), dirZ = std::sin(angle);
        const float wavelength = waves.wavelengthMetres / (1.0f + 0.35f * static_cast<float>(component));
        const float amplitude = spectral
            ? waves.amplitudeMetres * std::sqrt(1.0f / (1.0f + static_cast<float>(component)))
            : waves.amplitudeMetres / (1.0f + 0.6f * static_cast<float>(component));
        const float k = 2.0f * Pi / wavelength;
        const float omega = k * waves.speedMetresPerSecond;
        const float phase = k * (dirX * x + dirZ * z) - omega * time_ + static_cast<float>(component) * 1.7f;
        height += amplitude * std::sin(phase);
        const float derivative = amplitude * k * std::cos(phase);
        slopeX += derivative * dirX;
        slopeZ += derivative * dirZ;
    }
    return height;
}

float Body::surfaceAt(float x, float z) const {
    if (asset_->backend == Backend::Shallow) {
        const float fx = (x - origin_.x + asset_->extentX * 0.5f) / asset_->cellMetres - 0.5f;
        const float fz = (z - origin_.z + asset_->extentZ * 0.5f) / asset_->cellMetres - 0.5f;
        const float cx = std::clamp(fx, 0.0f, static_cast<float>(asset_->columns - 1));
        const float cz = std::clamp(fz, 0.0f, static_cast<float>(asset_->rows - 1));
        const auto x0 = static_cast<std::uint32_t>(cx), z0 = static_cast<std::uint32_t>(cz);
        const auto x1 = std::min(x0 + 1, asset_->columns - 1), z1 = std::min(z0 + 1, asset_->rows - 1);
        const float tx = cx - x0, tz = cz - z0;
        const auto surface = [&](std::uint32_t column, std::uint32_t row) {
            const auto cell = index(column, row);
            return bed_[cell] + depth_[cell];
        };
        const float bottom = surface(x0, z0) * (1 - tx) * (1 - tz) + surface(x1, z0) * tx * (1 - tz) +
                             surface(x0, z1) * (1 - tx) * tz + surface(x1, z1) * tx * tz;
        return origin_.y+bottom;
    }
    float slopeX = 0, slopeZ = 0;
    return origin_.y+asset_->stillHeightMetres + waveHeight(x-origin_.x, z-origin_.z, slopeX, slopeZ);
}

Query Body::query(float x, float z) const {
    Query result;
    result.tick = tick_;
    if (!finite(x) || !finite(z) || !inside(x, z)) return result;
    result.valid = true;
    result.errorBoundMetres = asset_->backend == Backend::Shallow ? asset_->cellMetres : asset_->waves.wavelengthMetres / 32.0f;
    if (asset_->backend == Backend::Shallow) {
        const float fx = (x - origin_.x + asset_->extentX * 0.5f) / asset_->cellMetres - 0.5f;
        const float fz = (z - origin_.z + asset_->extentZ * 0.5f) / asset_->cellMetres - 0.5f;
        const float cx = std::clamp(fx, 0.0f, static_cast<float>(asset_->columns - 1));
        const float cz = std::clamp(fz, 0.0f, static_cast<float>(asset_->rows - 1));
        const auto x0 = static_cast<std::uint32_t>(cx), z0 = static_cast<std::uint32_t>(cz);
        const auto x1 = std::min(x0 + 1, asset_->columns - 1), z1 = std::min(z0 + 1, asset_->rows - 1);
        const float tx = cx - x0, tz = cz - z0;
        const auto sample = [&](std::uint32_t column, std::uint32_t row, float& height, float& depth, float& u, float& v) {
            const auto cell = index(column, row);
            depth = solid_[cell] ? 0.0f : depth_[cell];
            height = bed_[cell] + depth;
            u = depth > DryDepth ? momentumX_[cell] / depth : 0;
            v = depth > DryDepth ? momentumZ_[cell] / depth : 0;
        };
        float h00, h10, h01, h11, d00, d10, d01, d11, u00, u10, u01, u11, v00, v10, v01, v11;
        sample(x0, z0, h00, d00, u00, v00); sample(x1, z0, h10, d10, u10, v10);
        sample(x0, z1, h01, d01, u01, v01); sample(x1, z1, h11, d11, u11, v11);
        const auto blend = [&](float a, float b, float c, float d) {
            return a * (1 - tx) * (1 - tz) + b * tx * (1 - tz) + c * (1 - tx) * tz + d * tx * tz;
        };
        result.surfaceHeightMetres = origin_.y+blend(h00, h10, h01, h11);
        result.depthMetres = blend(d00, d10, d01, d11);
        result.velocityX = blend(u00, u10, u01, u11);
        result.velocityZ = blend(v00, v10, v01, v11);
        const float e = asset_->cellMetres;
        const float dhx = (surfaceAt(x + e, z) - surfaceAt(x - e, z)) / (2 * e);
        const float dhz = (surfaceAt(x, z + e) - surfaceAt(x, z - e)) / (2 * e);
        const float length = std::sqrt(dhx * dhx + 1 + dhz * dhz);
        result.normal = {-dhx / length, 1.0f / length, -dhz / length};
        return result;
    }
    float slopeX = 0, slopeZ = 0;
    const float height = waveHeight(x-origin_.x, z-origin_.z, slopeX, slopeZ);
    result.surfaceHeightMetres = origin_.y+asset_->stillHeightMetres + height;
    result.depthMetres = std::max(0.0f, result.surfaceHeightMetres - bedAt(x, z));
    const float length = std::sqrt(slopeX * slopeX + 1 + slopeZ * slopeZ);
    result.normal = {-slopeX / length, 1.0f / length, -slopeZ / length};
    return result;
}

Buoyancy Body::buoyancy(Vec3 boxCentre, Vec3 boxExtent, float densityKgM3) const {
    Buoyancy result;
    result.tick = tick_;
    result.errorBoundMetres = asset_->cellMetres;
    if (!finite(boxCentre.x) || !finite(boxCentre.z) || boxExtent.x <= 0 || boxExtent.z <= 0 || boxExtent.y <= 0) return result;
    if (!inside(boxCentre.x, boxCentre.z)) return result;
    const float halfX = boxExtent.x * 0.5f, halfZ = boxExtent.z * 0.5f, halfY = boxExtent.y * 0.5f;
    const float footprint = boxExtent.x * boxExtent.z;
    std::uint32_t firstColumn = 0, lastColumn = asset_->columns - 1, firstRow = 0, lastRow = asset_->rows - 1;
    if (asset_->backend == Backend::Shallow) {
        const float minX = (boxCentre.x - halfX - origin_.x + asset_->extentX * 0.5f) / asset_->cellMetres - 0.5f;
        const float maxX = (boxCentre.x + halfX - origin_.x + asset_->extentX * 0.5f) / asset_->cellMetres - 0.5f;
        const float minZ = (boxCentre.z - halfZ - origin_.z + asset_->extentZ * 0.5f) / asset_->cellMetres - 0.5f;
        const float maxZ = (boxCentre.z + halfZ - origin_.z + asset_->extentZ * 0.5f) / asset_->cellMetres - 0.5f;
        firstColumn = static_cast<std::uint32_t>(std::clamp(std::floor(minX), 0.0f, static_cast<float>(asset_->columns - 1)));
        lastColumn = static_cast<std::uint32_t>(std::clamp(std::ceil(maxX), 0.0f, static_cast<float>(asset_->columns - 1)));
        firstRow = static_cast<std::uint32_t>(std::clamp(std::floor(minZ), 0.0f, static_cast<float>(asset_->rows - 1)));
        lastRow = static_cast<std::uint32_t>(std::clamp(std::ceil(maxZ), 0.0f, static_cast<float>(asset_->rows - 1)));
    }
    const auto samples = static_cast<float>((lastColumn - firstColumn + 1) * (lastRow - firstRow + 1));
    const float sampleArea = footprint / samples;
    float volume = 0;
    Vec3 weighted{};
    for (std::uint32_t row = firstRow; row <= lastRow; ++row)
        for (std::uint32_t column = firstColumn; column <= lastColumn; ++column) {
            const float x = origin_.x - asset_->extentX * 0.5f + (column + 0.5f) * asset_->cellMetres;
            const float z = origin_.z - asset_->extentZ * 0.5f + (row + 0.5f) * asset_->cellMetres;
            const auto sample = query(x, z);
            if (!sample.valid) continue;
            const float bottom = std::max(bedAt(x, z), boxCentre.y - halfY);
            const float top = boxCentre.y + halfY;
            const float submerged = std::clamp(sample.surfaceHeightMetres - bottom, 0.0f, top - bottom);
            if (submerged <= 0) continue;
            volume += submerged * sampleArea;
            weighted += Vec3{x, bottom + submerged * 0.5f, z} * (submerged * sampleArea);
        }
    if (volume <= 0) return result;
    result.valid = true;
    result.submergedVolumeCubicMetres = volume;
    result.displacedMassKg = volume * WaterDensity;
    // Net upward force for the caller's own mass; the query still does not decide
    // whether the object floats, it only reports the displacement it can observe.
    result.netBuoyantForceN = (WaterDensity - densityKgM3) * volume * Gravity;
    result.centre = weighted / volume;
    return result;
}

Diagnostics Body::diagnostics() const {
    Diagnostics result;
    result.tick = tick_;
    result.timelineEpoch = timelineEpoch_;
    result.substeps = substeps_;
    result.cflClamped = cflClamped_;
    result.lastCflNumber = lastCfl_;
    result.timeDebtSeconds = static_cast<float>(timeDebt_);
    result.droppedSeconds = static_cast<float>(droppedTime_);
    result.sanitizedCells = sanitizedCells_;
    result.inflowRequestedCubicMetres = static_cast<float>(inflowRequested_);
    result.inflowActualCubicMetres = static_cast<float>(inflowActual_);
    result.outflowRequestedCubicMetres = static_cast<float>(outflowRequested_);
    result.outflowActualCubicMetres = static_cast<float>(outflowActual_);
    result.outflowLimitedCubicMetres = static_cast<float>(outflowLimited_);
    result.reactionImpulseNs = static_cast<float>(reactionImpulse_);
    result.solidCells = solidCells_;
    result.sourceCells = sourceCells_;
    result.drainCells = drainCells_;
    result.droppedEvents = droppedEvents_;
    result.lastStepMilliseconds = lastStepMilliseconds_;
    const float cellArea = asset_->cellMetres * asset_->cellMetres;
    float volume = 0;
    for (std::size_t cell = 0; cell < depth_.size(); ++cell) {
        const float depth = depth_[cell];
        if (!finite(depth) || !finite(momentumX_[cell]) || !finite(momentumZ_[cell])) { ++result.nonFinite; continue; }
        volume += depth * cellArea;
        result.maxDepthMetres = std::max(result.maxDepthMetres, depth);
        if (depth > DryDepth) {
            const float speed = std::sqrt(momentumX_[cell] * momentumX_[cell] + momentumZ_[cell] * momentumZ_[cell]) / depth;
            result.maxSpeedMetresPerSecond = std::max(result.maxSpeedMetresPerSecond, speed);
        }
        result.maxSurfaceDeviationMetres = std::max(result.maxSurfaceDeviationMetres, std::fabs(depth - referenceDepth_[cell]));
    }
    result.volumeCubicMetres = volume;
    result.initialVolumeCubicMetres = initialVolume_;
    result.addedCubicMetres = added_;
    result.removedCubicMetres = removed_;
    const float expected = initialVolume_ + added_ - removed_;
    result.massErrorFraction = std::fabs(volume - expected) / std::max(initialVolume_, 1e-9f);
    return result;
}

std::uint64_t Body::digest() const {
    determinism::Hasher hash;
    hash.text("nativegpu.water-cpu-reference/2");
    hash.text(asset_->hash);
    hash.u64(tick_);
    hash.u32(timelineEpoch_);
    hash.u32(sequence_);
    hash.u64(static_cast<std::uint64_t>(solidCells_));
    hash.quantized(time_);
    hash.quantized(static_cast<float>(timeDebt_));
    hash.quantized(origin_.x); hash.quantized(origin_.z);
    for (std::size_t cell = 0; cell < depth_.size(); ++cell) {
        // Runtime obstacles are not part of the cooked asset hash, so the bed and the
        // occupancy mask are hashed directly; two different stone layouts must not share
        // a digest just because they have the same water state.
        hash.quantized(bed_[cell]);
        hash.u32(solid_[cell]);
        hash.quantized(depth_[cell]);
        hash.quantized(momentumX_[cell]);
        hash.quantized(momentumZ_[cell]);
    }
    return hash.value();
}

} // namespace engine::water
