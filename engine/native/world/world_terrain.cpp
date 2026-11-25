#include "world_terrain.hpp"

#include "world_noise.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine {
namespace world {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

float smoothstep01(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

float distanceToSegment(float ax, float az, float bx, float bz, float px, float pz) {
    const float dx = bx - ax;
    const float dz = bz - az;
    const float lengthSquared = dx * dx + dz * dz;
    if (lengthSquared <= 1.0e-12f) return std::sqrt((px - ax) * (px - ax) + (pz - az) * (pz - az));
    float t = ((px - ax) * dx + (pz - az) * dz) / lengthSquared;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float cx = ax + dx * t;
    const float cz = az + dz * t;
    return std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
}

bool pointInPolygon(const std::vector<PathPoint>& polygon, float x, float z) {
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i++) {
        const float xi = polygon[i].x, zi = polygon[i].z;
        const float xj = polygon[previous].x, zj = polygon[previous].z;
        if (((zi > z) != (zj > z))
            && (x < (xj - xi) * (z - zi) / ((zj - zi) == 0.0f ? 1.0e-12f : (zj - zi)) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace

// ---------------------------------------------------------------- TerrainField

bool TerrainField::configure(const TerrainParams& params, std::string& error) {
    params_ = params;
    if (!std::isfinite(params_.amplitudeMetres) || params_.amplitudeMetres < 0.0f || params_.amplitudeMetres > 1200.0f) {
        error = "terrain amplitude must be in [0, 1200] metres";
        return false;
    }
    if (!std::isfinite(params_.baseHeightMetres) || params_.baseHeightMetres < -500.0f || params_.baseHeightMetres > 4000.0f) {
        error = "terrain base height must be in [-500, 4000] metres";
        return false;
    }
    if (!std::isfinite(params_.ridgeStrength) || params_.ridgeStrength < 0.0f || params_.ridgeStrength > 1.0f) {
        error = "terrain ridge strength must be in [0, 1]";
        return false;
    }
    if (!std::isfinite(params_.sampleStepMetres) || params_.sampleStepMetres < 0.25f || params_.sampleStepMetres > 8.0f) {
        error = "terrain sample step must be in [0.25, 8] metres";
        return false;
    }
    const std::string& preset = params_.preset;
    if (preset != "flat" && preset != "mountain-valley" && preset != "rolling-hills"
        && preset != "ridge-basin" && preset != "river-plain") {
        error = "unknown terrain preset '" + preset + "'";
        return false;
    }

    // Coarse height probe. Ecological weights need a real range, not a guessed one: a
    // mountain preset at amplitude 20 would otherwise classify everything as a summit.
    float minimum = 1.0e30f, maximum = -1.0e30f;
    const float probe = 32.0f;
    for (int iz = 0; iz < 33; ++iz) {
        for (int ix = 0; ix < 33; ++ix) {
            const float x = (static_cast<float>(ix) - 16.0f) * probe * 4.0f;
            const float z = (static_cast<float>(iz) - 16.0f) * probe * 4.0f;
            const float h = height(x, z);
            if (h < minimum) minimum = h;
            if (h > maximum) maximum = h;
        }
    }
    minHeight_ = minimum;
    maxHeight_ = maximum > minimum + 0.01f ? maximum : minimum + 1.0f;
    return true;
}

float TerrainField::height(float x, float z) const {
    const std::uint64_t seed = params_.seed;
    const float base = params_.baseHeightMetres;
    const float amplitude = params_.amplitudeMetres;
    const std::string& preset = params_.preset;

    if (preset == "flat" || amplitude <= 0.0f) return base;

    // Feature scale: one noise unit is roughly 640 m, which puts three or four ridges
    // across a 2 km world. The factor is a constant, not a per-world knob, so two worlds
    // with the same amplitude are comparable.
    const float nx = x * 0.0015625f;
    const float nz = z * 0.0015625f;

    if (preset == "rolling-hills") {
        return base + amplitude * 0.55f * noise::fbm(nx * 1.7f, nz * 1.7f, seed, 4, 2.01f, 0.5f);
    }
    if (preset == "ridge-basin") {
        const float ridge = noise::ridged(nx, nz, seed, 4, 1.7f);
        const float detail = noise::fbm(nx * 3.0f, nz * 3.0f, seed ^ 0x51edull, 3) * 0.12f;
        return base + amplitude * (0.22f + 0.78f * ridge + detail);
    }
    if (preset == "river-plain") {
        return base + amplitude * 0.16f * noise::fbm(nx * 3.2f, nz * 3.2f, seed, 3);
    }

    // mountain-valley: a meandering valley floor with ridges rising away from it. The
    // valley centre is the zero crossing of a low-frequency noise field, so it meanders
    // instead of running straight.
    const float meander = noise::fbm(nx * 0.42f, nz * 0.42f, seed ^ 0x9e37ull, 3);
    const float valleyDistance = std::fabs(meander);
    const float wall = smoothstep01((valleyDistance - 0.06f) / 0.42f);
    const float ridge = noise::ridged(nx, nz, seed, 4, 1.8f);
    const float rolling = noise::fbm(nx * 1.35f, nz * 1.35f, seed ^ 0x1234ull, 4) * 0.5f + 0.5f;
    const float profile = ridge * params_.ridgeStrength + rolling * (1.0f - params_.ridgeStrength);
    // The valley floor still undulates slightly; a perfectly flat floor reads as a bug.
    const float floor = noise::fbm(nx * 2.6f, nz * 2.6f, seed ^ 0xabcdull, 3) * 0.05f;
    return base + amplitude * (floor + profile * wall);
}

float TerrainField::gradientMagnitude(float x, float z) const {
    const float step = std::max(0.5f, params_.sampleStepMetres);
    const float left = height(x - step, z);
    const float right = height(x + step, z);
    const float back = height(x, z - step);
    const float front = height(x, z + step);
    const float gx = (right - left) / (2.0f * step);
    const float gz = (front - back) / (2.0f * step);
    return std::sqrt(gx * gx + gz * gz);
}

float TerrainField::slopeDegrees(float x, float z) const {
    return std::atan(gradientMagnitude(x, z)) * 180.0f / kPi;
}

float TerrainField::moisture(float x, float z) const {
    const float h = height(x, z);
    const float normalized = clamp01((h - minHeight_) / (maxHeight_ - minHeight_));
    // Low ground is wet; a slow noise field breaks up the contour bands so the moisture
    // map does not look like a topographic printout.
    const float lowland = 1.0f - normalized;
    const float variation = noise::fbm(x * 0.0031f, z * 0.0031f, params_.seed ^ 0x77ull, 3) * 0.22f;
    return clamp01(lowland * 0.85f + variation + 0.08f);
}

float TerrainField::rockiness(float x, float z) const {
    const float slope = clamp01(gradientMagnitude(x, z) / 1.4f);
    const float variation = noise::fbm(x * 0.0042f, z * 0.0042f, params_.seed ^ 0x99ull, 2) * 0.18f;
    return clamp01(slope * 0.9f + variation);
}

// ---------------------------------------------------------------- TerrainEdit

float TerrainEdit::weight(float x, float z) const {
    if (!bounds.contains(x, z)) return 0.0f;
    float distance = 0.0f;
    switch (shape) {
        case TerrainEditShape::Circle: {
            const float dx = x - centreX;
            const float dz = z - centreZ;
            distance = std::sqrt(dx * dx + dz * dz);
            if (distance >= radiusMetres) return 0.0f;
            if (falloffMetres <= 0.0f) return 1.0f;
            const float inner = std::max(0.0f, radiusMetres - falloffMetres);
            if (distance <= inner) return 1.0f;
            return 1.0f - smoothstep01((distance - inner) / (radiusMetres - inner));
        }
        case TerrainEditShape::Rectangle: {
            if (falloffMetres <= 0.0f) return 1.0f;
            const float edge = std::min(std::min(x - bounds.minX, bounds.maxX - x),
                                        std::min(z - bounds.minZ, bounds.maxZ - z));
            return smoothstep01(edge / falloffMetres);
        }
        case TerrainEditShape::Polygon: {
            if (!pointInPolygon(polygon, x, z)) {
                if (falloffMetres <= 0.0f) return 0.0f;
                float nearest = 1.0e30f;
                for (std::size_t i = 0; i < polygon.size(); ++i) {
                    const auto& a = polygon[i];
                    const auto& b = polygon[(i + 1) % polygon.size()];
                    nearest = std::min(nearest, distanceToSegment(a.x, a.z, b.x, b.z, x, z));
                }
                if (nearest >= falloffMetres) return 0.0f;
                return 1.0f - smoothstep01(nearest / falloffMetres);
            }
            if (falloffMetres <= 0.0f) return 1.0f;
            float nearest = 1.0e30f;
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const auto& a = polygon[i];
                const auto& b = polygon[(i + 1) % polygon.size()];
                nearest = std::min(nearest, distanceToSegment(a.x, a.z, b.x, b.z, x, z));
            }
            return smoothstep01(nearest / falloffMetres);
        }
    }
    return 0.0f;
}

// ---------------------------------------------------------------- TerrainSurface

void TerrainSurface::configure(const TerrainField* base, const PathNetwork* paths,
                               const std::vector<TerrainEdit>& edits) {
    base_ = base;
    paths_ = paths;
    edits_ = edits;
}

float TerrainSurface::applyEdits(float x, float z, float height) const {
    if (edits_.empty()) return height;
    float result = height;
    for (const auto& edit : edits_) {
        const float w = edit.weight(x, z);
        if (w <= 0.0f) continue;
        const float amount = w * edit.strength;
        if (edit.erase) {
            // Erasing an edit blends back toward the untouched field. It does not "subtract
            // the opposite amount", which would overshoot wherever two edits overlapped.
            const float original = base_ ? base_->height(x, z) : height;
            result = result + (original - result) * amount;
            continue;
        }
        switch (edit.mode) {
            case TerrainEditMode::Raise:
                result += edit.deltaMetres * amount;
                break;
            case TerrainEditMode::Lower:
                result -= edit.deltaMetres * amount;
                break;
            case TerrainEditMode::Smooth: {
                // Averaging the *base* field is an approximation, but it is a stable one:
                // smoothing the already-edited result would recurse through the edit list.
                if (!base_) break;
                const float radius = std::max(0.5f, edit.radiusMetres * 0.35f);
                const float average = (base_->height(x + radius, z) + base_->height(x - radius, z)
                                     + base_->height(x, z + radius) + base_->height(x, z - radius)) * 0.25f;
                result += (average - result) * amount;
                break;
            }
            case TerrainEditMode::Flatten:
                result += (edit.targetHeight - result) * amount;
                break;
            case TerrainEditMode::Water:
                // The water edit lowers the ground toward a water level; the surface itself
                // is drawn by the river path, not by this edit.
                result = std::min(result, result + (edit.targetHeight - result) * amount);
                break;
            case TerrainEditMode::Paint:
                // Material-only; the height is unchanged.
                break;
        }
    }
    return result;
}

float TerrainSurface::heightWithHit(float x, float z, const PathHit& hit) const {
    float h = base_ ? base_->height(x, z) : 0.0f;
    h = applyEdits(x, z, h);
    if (!hit.valid()) return h;

    const float halfWidth = hit.width * 0.5f;

    if (hit.kind == PathKind::River) {
        const float bank = std::max(1.0f, hit.width * 0.6f);
        if (hit.distance <= halfWidth) {
            // Parabolic channel: deepest at the centre line, exactly at the water surface
            // at the bank. A flat-bottomed trench would read as a canal.
            const float t = hit.distance / std::max(0.001f, halfWidth);
            const float bed = hit.waterHeight - hit.bedDepth * (1.0f - t * t);
            h = std::min(h, bed);
        } else if (hit.distance < halfWidth + bank) {
            const float t = (hit.distance - halfWidth) / bank;
            const float edge = hit.waterHeight;
            h = edge + (h - edge) * smoothstep01(t);
        }
        return h;
    }

    // Road: flatten to the compiled profile, then blend out over the shoulder. A road on a
    // bridge deck does not touch the terrain at all — the deck is drawn and collided
    // separately, and cutting the ground under it would leave a hole in the river.
    if (hit.onDeck) return h;
    const float shoulder = std::max(0.25f, hit.width * 0.25f + 0.5f);
    if (hit.distance <= halfWidth) {
        h = hit.centreHeight;
    } else if (hit.distance < halfWidth + shoulder) {
        const float t = (hit.distance - halfWidth) / shoulder;
        h = hit.centreHeight + (h - hit.centreHeight) * smoothstep01(t);
    }
    return h;
}

float TerrainSurface::height(float x, float z) const {
    const PathHit hit = (paths_ && !paths_->empty()) ? paths_->nearest(x, z, 96.0f) : PathHit{};
    return heightWithHit(x, z, hit);
}

SurfaceSample TerrainSurface::sample(float x, float z, float stepMetres) const {
    SurfaceSample out;
    const PathHit hit = (paths_ && !paths_->empty()) ? paths_->nearest(x, z, 96.0f) : PathHit{};
    const float step = std::max(0.25f, stepMetres);

    out.height = heightWithHit(x, z, hit);
    out.hasPath = hit.valid();
    out.pathDistance = hit.valid() ? hit.distance : 1.0e9f;
    out.pathKind = hit.kind;

    // Central differences through the same height function, so the normal describes the
    // surface the mesh is actually built from rather than a coarser field.
    const float left = heightWithHit(x - step, z, paths_ ? paths_->nearest(x - step, z, 96.0f) : PathHit{});
    const float right = heightWithHit(x + step, z, paths_ ? paths_->nearest(x + step, z, 96.0f) : PathHit{});
    const float back = heightWithHit(x, z - step, paths_ ? paths_->nearest(x, z - step, 96.0f) : PathHit{});
    const float front = heightWithHit(x, z + step, paths_ ? paths_->nearest(x, z + step, 96.0f) : PathHit{});
    const float gx = (right - left) / (2.0f * step);
    const float gz = (front - back) / (2.0f * step);
    const float inverse = 1.0f / std::sqrt(gx * gx + 1.0f + gz * gz);
    out.normal[0] = -gx * inverse;
    out.normal[1] = inverse;
    out.normal[2] = -gz * inverse;
    out.normalY = inverse;
    const float clamped = inverse < 1.0e-6f ? 1.0e-6f : (inverse > 1.0f ? 1.0f : inverse);
    out.slopeDegrees = std::acos(clamped) * 180.0f / kPi;

    out.moisture = base_ ? base_->moisture(x, z) : 0.5f;
    out.rockiness = base_ ? base_->rockiness(x, z) : 0.2f;

    if (hit.valid() && hit.kind == PathKind::River) {
        out.waterHeight = hit.waterHeight;
        out.submerged = out.height < hit.waterHeight;
    }

    // A bridge deck is a surface of its own. Asking the span directly keeps the answer
    // correct even when the nearest path sample is the river underneath.
    const BridgeSpan* deck = paths_ ? paths_->bridgeAt(x, z) : nullptr;
    out.onBridge = deck != nullptr;
    if (deck) out.height = deck->deckHeight;
    out.onRoad = out.onBridge
        || (hit.valid() && hit.kind == PathKind::Road && hit.distance <= hit.width * 0.5f);

    const float slope = out.slopeDegrees;
    float rockWeight = clamp01((slope - 26.0f) / 26.0f) * 0.75f + out.rockiness * 0.35f;
    float sandWeight = 0.0f;
    if (hit.valid() && hit.kind == PathKind::River) {
        const float halfWidth = hit.width * 0.5f;
        const float bank = std::max(1.0f, hit.width * 0.6f);
        if (hit.distance > halfWidth && hit.distance < halfWidth + bank * 2.0f) {
            sandWeight = 1.0f - smoothstep01((hit.distance - halfWidth) / (bank * 2.0f));
        }
    }
    const float grassWeight = clamp01(out.moisture * 1.15f) * clamp01(1.0f - slope / 40.0f);
    const float dirtWeight = clamp01(0.35f - out.moisture) * 0.8f + clamp01((slope - 18.0f) / 30.0f) * 0.4f;
    out.weights.grass = grassWeight;
    out.weights.rock = rockWeight;
    out.weights.dirt = dirtWeight;
    out.weights.sand = sandWeight;
    out.weights.water = out.submerged ? 1.0f : 0.0f;
    out.weights.normalize();
    return out;
}

float TerrainSurface::normalY(float x, float z) const {
    return sample(x, z, base_ ? base_->params().sampleStepMetres : 1.0f).normalY;
}

void TerrainSurface::normal(float x, float z, float out[3]) const {
    const SurfaceSample value = sample(x, z, base_ ? base_->params().sampleStepMetres : 1.0f);
    out[0] = value.normal[0];
    out[1] = value.normal[1];
    out[2] = value.normal[2];
}

float TerrainSurface::slopeDegrees(float x, float z) const {
    return sample(x, z, base_ ? base_->params().sampleStepMetres : 1.0f).slopeDegrees;
}

SurfaceWeights TerrainSurface::weights(float x, float z) const {
    return sample(x, z, base_ ? base_->params().sampleStepMetres : 1.0f).weights;
}

float TerrainSurface::waterHeight(float x, float z) const {
    if (!paths_) return -1.0e9f;
    return paths_->waterHeightAt(x, z);
}

bool TerrainSurface::submerged(float x, float z) const {
    return sample(x, z).submerged;
}

bool TerrainSurface::onRoad(float x, float z) const {
    return sample(x, z).onRoad;
}

bool TerrainSurface::onBridge(float x, float z) const {
    return sample(x, z).onBridge;
}

void SurfaceWeights::normalize() {
    grass = std::max(0.0f, grass);
    rock = std::max(0.0f, rock);
    dirt = std::max(0.0f, dirt);
    sand = std::max(0.0f, sand);
    const float total = grass + rock + dirt + sand;
    if (total <= 1.0e-6f) { grass = 1.0f; return; }
    const float inverse = 1.0f / total;
    grass *= inverse;
    rock *= inverse;
    dirt *= inverse;
    sand *= inverse;
}

} // namespace world
} // namespace engine
