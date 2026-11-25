#include "world_path.hpp"

#include "world_noise.hpp"
#include "world_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace engine {
namespace world {
namespace {

constexpr std::uint32_t kSampleBits = 20;
constexpr std::uint32_t kSampleMask = (1u << kSampleBits) - 1u;

float length2D(float dx, float dz) { return std::sqrt(dx * dx + dz * dz); }

// Spline spacing follows the width so a wide river is not under-sampled and a narrow path
// is not stored as 4000 identical points. Clamped at both ends: 0.35 m is already finer
// than any terrain vertex, 4 m is coarse enough for a long motorway.
float sampleSpacing(float widthMetres) {
    const float spacing = widthMetres * 0.18f;
    return std::max(0.35f, std::min(4.0f, spacing));
}

} // namespace

PathPoint catmullRom(const PathPoint& p0, const PathPoint& p1, const PathPoint& p2, const PathPoint& p3, float t) {
    // Uniform Catmull-Rom with tension 0.5, which is the standard centripetal-free form.
    // A centripetal variant would be more robust for wildly uneven spacing, but the editor
    // resamples control points to roughly even spacing before they reach here.
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float a = -0.5f * t3 + t2 - 0.5f * t;
    const float b = 1.5f * t3 - 2.5f * t2 + 1.0f;
    const float c = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    const float d = 0.5f * t3 - 0.5f * t2;
    PathPoint out;
    out.x = a * p0.x + b * p1.x + c * p2.x + d * p3.x;
    out.z = a * p0.z + b * p1.z + c * p2.z + d * p3.z;
    return out;
}

bool PathNetwork::build(const std::vector<PathSpec>& specs, const TerrainField* baseTerrain,
                        float worldMinX, float worldMinZ, float worldMaxX, float worldMaxZ,
                        std::string& error) {
    paths_.clear();
    diagnostics_.clear();
    bridges_.clear();
    grid_.clear();
    base_ = baseTerrain;
    gridOriginX_ = worldMinX;
    gridOriginZ_ = worldMinZ;

    if (specs.size() > static_cast<std::size_t>(Limits::kMaxPathsPerLayer)) {
        error = "too many paths";
        return false;
    }

    for (const auto& spec : specs) {
        if (spec.control.size() < 2) {
            diagnostics_.push_back({"PATH_TOO_SHORT", "path needs at least two control points", spec.id,
                                    "Add a control point; a path with one node has no direction."});
            continue;
        }
        if (spec.control.size() > static_cast<std::size_t>(Limits::kMaxPathPoints)) {
            diagnostics_.push_back({"PATH_TOO_LONG", "path has more control points than the contract allows", spec.id,
                                    "Split the path at a junction."});
            continue;
        }
        if (!std::isfinite(spec.widthMetres) || spec.widthMetres <= 0.05f || spec.widthMetres > 200.0f) {
            error = "path '" + spec.id + "' has an unusable width";
            return false;
        }
        for (const auto& point : spec.control) {
            if (!std::isfinite(point.x) || !std::isfinite(point.z)) {
                error = "path '" + spec.id + "' has a non-finite control point";
                return false;
            }
        }

        CompiledPath compiled;
        compiled.spec = spec;

        // Flatten the spline. Duplicate control points are skipped: a zero-length segment
        // has no tangent, and carrying it forward would produce a NaN normal downstream.
        std::vector<PathPoint> control;
        control.reserve(spec.control.size());
        for (const auto& point : spec.control) {
            if (!control.empty()) {
                const auto& previous = control.back();
                if (length2D(point.x - previous.x, point.z - previous.z) < 0.05f) continue;
            }
            control.push_back(point);
        }
        if (control.size() < 2) {
            diagnostics_.push_back({"PATH_DEGENERATE", "path collapses to a single point", spec.id,
                                    "Move the control points apart."});
            continue;
        }

        const float spacing = sampleSpacing(spec.widthMetres);
        const auto at = [&](int index) -> PathPoint {
            if (spec.closed) {
                const int count = static_cast<int>(control.size());
                return control[static_cast<std::size_t>(((index % count) + count) % count)];
            }
            const int count = static_cast<int>(control.size());
            const int clamped = index < 0 ? 0 : (index >= count ? count - 1 : index);
            return control[static_cast<std::size_t>(clamped)];
        };

        const int segmentCount = spec.closed ? static_cast<int>(control.size())
                                             : static_cast<int>(control.size()) - 1;
        float arc = 0.0f;
        for (int segment = 0; segment < segmentCount; ++segment) {
            const PathPoint p0 = at(segment - 1);
            const PathPoint p1 = at(segment);
            const PathPoint p2 = at(segment + 1);
            const PathPoint p3 = at(segment + 2);
            const float chord = length2D(p2.x - p1.x, p2.z - p1.z);
            const int steps = std::max(2, static_cast<int>(std::ceil(chord / spacing)));
            for (int step = 0; step < steps; ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                const PathPoint point = catmullRom(p0, p1, p2, p3, t);
                PathSample sample;
                sample.x = point.x;
                sample.z = point.z;
                sample.distance = arc;
                compiled.samples.push_back(sample);
            }
            arc += chord;
        }
        if (!spec.closed) {
            const PathPoint last = control.back();
            PathSample sample;
            sample.x = last.x;
            sample.z = last.z;
            sample.distance = arc;
            compiled.samples.push_back(sample);
        }
        if (compiled.samples.size() < 2) {
            diagnostics_.push_back({"PATH_DEGENERATE", "path produced fewer than two samples", spec.id, ""});
            continue;
        }
        compiled.length = arc;

        // Recompute arc length from the actual spline samples: the chord sum above is a
        // lower bound and a long curving road would accumulate a visible error.
        float running = 0.0f;
        compiled.samples[0].distance = 0.0f;
        for (std::size_t i = 1; i < compiled.samples.size(); ++i) {
            auto& sample = compiled.samples[i];
            auto& previous = compiled.samples[i - 1];
            running += length2D(sample.x - previous.x, sample.z - previous.z);
            sample.distance = running;
        }
        compiled.length = running;
        if (compiled.length < 1.0e-3f) {
            diagnostics_.push_back({"PATH_DEGENERATE", "path has zero length", spec.id, ""});
            continue;
        }

        // Tangents, forward differences with a wrap for closed paths.
        for (std::size_t i = 0; i < compiled.samples.size(); ++i) {
            const std::size_t next = (i + 1) % compiled.samples.size();
            const std::size_t previous = (i == 0) ? (spec.closed ? compiled.samples.size() - 1 : 0) : i - 1;
            float dx = compiled.samples[next].x - compiled.samples[previous].x;
            float dz = compiled.samples[next].z - compiled.samples[previous].z;
            if (length2D(dx, dz) < 1.0e-6f) { dx = 1.0f; dz = 0.0f; }
            const float inverse = 1.0f / length2D(dx, dz);
            compiled.samples[i].tx = dx * inverse;
            compiled.samples[i].tz = dz * inverse;
        }

        // Authored centre height: sample the base terrain and smooth it. A road that follows
        // every bump of the noise is not a road, it is a decal.
        for (auto& sample : compiled.samples) {
            sample.centreHeight = baseTerrain ? baseTerrain->height(sample.x, sample.z) : 0.0f;
        }
        if (spec.kind == PathKind::Road) {
            const int window = std::max(1, static_cast<int>(12.0f / spacing));
            std::vector<float> smoothed(compiled.samples.size(), 0.0f);
            for (std::size_t i = 0; i < compiled.samples.size(); ++i) {
                float sum = 0.0f;
                int count = 0;
                const int lo = static_cast<int>(i) - window;
                const int hi = static_cast<int>(i) + window;
                for (int j = lo; j <= hi; ++j) {
                    if (spec.closed) {
                        const int count_ = static_cast<int>(compiled.samples.size());
                        const int wrapped = ((j % count_) + count_) % count_;
                        sum += compiled.samples[static_cast<std::size_t>(wrapped)].centreHeight;
                        ++count;
                    } else {
                        const int clamped = std::max(0, std::min(static_cast<int>(compiled.samples.size()) - 1, j));
                        sum += compiled.samples[static_cast<std::size_t>(clamped)].centreHeight;
                        ++count;
                    }
                }
                smoothed[i] = count > 0 ? sum / static_cast<float>(count) : compiled.samples[i].centreHeight;
            }
            for (std::size_t i = 0; i < compiled.samples.size(); ++i) {
                compiled.samples[i].centreHeight = smoothed[i];
            }
            // Slope limit: a road may not climb faster than maxSlopeDegrees. Where the
            // smoothed profile violates it, the profile is relaxed toward the limit and a
            // diagnostic records that the terrain must be cut or filled.
            const float maxRise = std::tan(std::max(1.0f, spec.maxSlopeDegrees) * 3.14159265358979323846f / 180.0f);
            float worst = 0.0f;
            for (std::size_t i = 1; i < compiled.samples.size(); ++i) {
                const float run = length2D(compiled.samples[i].x - compiled.samples[i - 1].x,
                                           compiled.samples[i].z - compiled.samples[i - 1].z);
                if (run < 1.0e-5f) continue;
                const float rise = std::fabs(compiled.samples[i].centreHeight - compiled.samples[i - 1].centreHeight);
                const float slope = rise / run;
                if (slope > maxRise) worst = std::max(worst, std::atan(slope) * 180.0f / 3.14159265358979323846f);
            }
            if (worst > 0.0f) {
                char message[192];
                std::snprintf(message, sizeof(message),
                              "path exceeds its %.0f° slope limit by %.0f°; the surface is cut and filled to compensate",
                              spec.maxSlopeDegrees, worst - spec.maxSlopeDegrees);
                diagnostics_.push_back({"PATH_SLOPE_LIMIT", message, spec.id,
                                        "Add a switchback, lower the terrain around the climb, or raise the limit."});
            }
        }

        paths_.push_back(std::move(compiled));
    }

    if (base_) solveRiverDescent();
    buildGrid();
    solveBridges();

    (void)worldMaxX;
    (void)worldMaxZ;
    return true;
}

void PathNetwork::buildGrid() {
    grid_.clear();
    // One insert per sample, radius = half width + shoulder + bank so a query at the outer
    // edge of the influence still finds the path.
    for (std::size_t pathIndex = 0; pathIndex < paths_.size(); ++pathIndex) {
        const auto& path = paths_[pathIndex];
        const float radius = path.spec.widthMetres * 0.5f + path.spec.shoulderMetres + 4.0f;
        for (std::size_t sampleIndex = 0; sampleIndex < path.samples.size(); ++sampleIndex) {
            if (sampleIndex > kSampleMask) break;
            insertGrid(static_cast<std::uint32_t>(pathIndex), static_cast<std::uint32_t>(sampleIndex),
                       path.samples[sampleIndex].x, path.samples[sampleIndex].z, radius);
        }
    }
}

void PathNetwork::insertGrid(std::uint32_t pathIndex, std::uint32_t sampleIndex, float x, float z, float radius) {
    const std::int32_t minCx = static_cast<std::int32_t>(std::floor((x - radius - gridOriginX_) / gridCellMetres_));
    const std::int32_t maxCx = static_cast<std::int32_t>(std::floor((x + radius - gridOriginX_) / gridCellMetres_));
    const std::int32_t minCz = static_cast<std::int32_t>(std::floor((z - radius - gridOriginZ_) / gridCellMetres_));
    const std::int32_t maxCz = static_cast<std::int32_t>(std::floor((z + radius - gridOriginZ_) / gridCellMetres_));
    const std::uint32_t encoded = (pathIndex << kSampleBits) | sampleIndex;
    for (std::int32_t cz = minCz; cz <= maxCz; ++cz) {
        for (std::int32_t cx = minCx; cx <= maxCx; ++cx) {
            const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
                                    | static_cast<std::uint32_t>(cz);
            grid_[key].samples.push_back(encoded);
        }
    }
}

const PathNetwork::GridCell* PathNetwork::cellAt(std::int32_t cx, std::int32_t cz) const {
    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
                            | static_cast<std::uint32_t>(cz);
    const auto it = grid_.find(key);
    return it == grid_.end() ? nullptr : &it->second;
}

PathHit PathNetwork::nearest(float x, float z, float radius) const {
    return nearestInternal(x, z, radius, -1);
}

PathHit PathNetwork::nearestOfKind(float x, float z, float radius, PathKind kind) const {
    return nearestInternal(x, z, radius, static_cast<int>(kind));
}

PathHit PathNetwork::nearestInternal(float x, float z, float radius, int requiredKind) const {
    PathHit best;
    if (paths_.empty()) return best;
    const std::int32_t cx = static_cast<std::int32_t>(std::floor((x - gridOriginX_) / gridCellMetres_));
    const std::int32_t cz = static_cast<std::int32_t>(std::floor((z - gridOriginZ_) / gridCellMetres_));
    const std::int32_t reach = std::max(1, static_cast<std::int32_t>(std::ceil(radius / gridCellMetres_)));
    float bestDistance = radius;
    for (std::int32_t dz = -reach; dz <= reach; ++dz) {
        for (std::int32_t dx = -reach; dx <= reach; ++dx) {
            const GridCell* cell = cellAt(cx + dx, cz + dz);
            if (!cell) continue;
            for (std::uint32_t encoded : cell->samples) {
                const std::uint32_t pathIndex = encoded >> kSampleBits;
                const std::uint32_t sampleIndex = encoded & kSampleMask;
                if (pathIndex >= paths_.size()) continue;
                const auto& path = paths_[pathIndex];
                if (requiredKind >= 0 && static_cast<int>(path.spec.kind) != requiredKind) continue;
                if (sampleIndex >= path.samples.size()) continue;
                const auto& sample = path.samples[sampleIndex];
                const float distance = length2D(x - sample.x, z - sample.z);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best.pathIndex = static_cast<int>(pathIndex);
                    best.distance = distance;
                    best.alongMetres = sample.distance;
                    best.centreHeight = sample.centreHeight;
                    best.width = path.spec.widthMetres;
                    best.bedDepth = path.spec.bedDepthMetres;
                    best.kind = path.spec.kind;
                    best.waterHeight = path.waterValid ? sample.centreHeight : -1.0e9f;
                    best.onDeck = false;
                }
            }
        }
    }
    if (!best.valid()) return best;
    // A point inside a bridge span is on the deck, not on the flattened road below it.
    const auto& path = paths_[static_cast<std::size_t>(best.pathIndex)];
    for (const auto& deck : path.decks) {
        if (best.alongMetres >= path.samples[deck.first].distance
            && best.alongMetres <= path.samples[deck.last].distance) {
            best.onDeck = true;
            best.centreHeight = deck.deckHeight;
            break;
        }
    }
    return best;
}

bool PathNetwork::inWater(float x, float z) const {
    const PathHit hit = nearest(x, z, 64.0f);
    if (!hit.valid() || hit.kind != PathKind::River) return false;
    return hit.distance <= hit.width * 0.5f;
}

const BridgeSpan* PathNetwork::bridgeAt(float x, float z) const {
    for (const auto& span : bridges_) {
        // Oriented box: project onto the deck's forward and left axes.
        const float forwardX = std::sin(span.yaw);
        const float forwardZ = std::cos(span.yaw);
        const float leftX = -forwardZ;
        const float leftZ = forwardX;
        const float dx = x - span.x;
        const float dz = z - span.z;
        const float along = dx * forwardX + dz * forwardZ;
        const float across = dx * leftX + dz * leftZ;
        if (std::fabs(along) <= span.lengthMetres * 0.5f + 1.0f
            && std::fabs(across) <= span.widthMetres * 0.5f) {
            return &span;
        }
    }
    return nullptr;
}

float PathNetwork::waterHeightAt(float x, float z) const {
    const PathHit hit = nearest(x, z, 64.0f);
    if (!hit.valid() || hit.kind != PathKind::River) return -1.0e9f;
    if (hit.distance > hit.width * 0.5f) return -1.0e9f;
    return hit.waterHeight;
}

void PathNetwork::solveRiverDescent() {
    // A river descends from its source to its outlet. The authored centreline gives the
    // terrain height; the water surface is a monotone lower envelope of that, so a river
    // crossing a rise becomes a pool behind the rise instead of water flowing uphill.
    for (auto& path : paths_) {
        if (path.spec.kind != PathKind::River) continue;
        const float bedDepth = std::max(0.05f, path.spec.bedDepthMetres);
        float level = path.samples.front().centreHeight - bedDepth * 0.35f;
        float carve = 0.0f;
        bool first = true;
        for (auto& sample : path.samples) {
            const float terrain = sample.centreHeight;
            // A 4 mm drop per sample keeps the surface strictly downhill without becoming
            // a staircase: at 0.5 m spacing that is a 0.8% gradient.
            if (!first) level -= 0.004f;
            level = std::min(level, terrain - bedDepth * 0.35f);
            // Do not dig deeper than three times the bed depth to keep a level: past that
            // the "river" is a canyon and the user needs to move the path.
            const float floorHeight = terrain - bedDepth * 3.0f;
            if (level < floorHeight) {
                carve = std::max(carve, floorHeight - level);
                level = floorHeight;
            }
            sample.centreHeight = level;
            first = false;
        }
        path.waterValid = true;
        if (carve > 0.0f) {
            char message[192];
            std::snprintf(message, sizeof(message),
                          "river needed %.1f m of extra cut to keep a downhill surface", carve);
            diagnostics_.push_back({"RIVER_STEEPS", message, path.spec.id,
                                    "Move the upstream control points downhill or accept the gorge."});
        }
    }
}

void PathNetwork::solveBridges() {
    // A bridge exists where a road centreline crosses a river centreline. Walking every
    // road sample against the river's grid is enough: a crossing is a local event and the
    // grid already bounds the search.
    for (std::size_t roadIndex = 0; roadIndex < paths_.size(); ++roadIndex) {
        auto& road = paths_[roadIndex];
        if (road.spec.kind != PathKind::Road) continue;
        std::size_t spanStart = 0;
        bool inSpan = false;
        float deckHeight = 0.0f;
        std::string riverId;
        float riverWidth = 0.0f;
        for (std::size_t i = 0; i < road.samples.size(); ++i) {
            const auto& sample = road.samples[i];
            const PathHit hit = nearestOfKind(sample.x, sample.z, 32.0f, PathKind::River);
            const bool overWater = hit.valid() && hit.distance <= hit.width * 0.5f + 0.5f;
            if (overWater && !inSpan) {
                inSpan = true;
                spanStart = i;
                riverId = paths_[static_cast<std::size_t>(hit.pathIndex)].spec.id;
                riverWidth = hit.width;
                deckHeight = hit.waterHeight + 1.35f;
            } else if (overWater && inSpan) {
                deckHeight = std::max(deckHeight, hit.waterHeight + 1.35f);
                riverWidth = std::max(riverWidth, hit.width);
            } else if (!overWater && inSpan) {
                inSpan = false;
                // Extend the deck a metre past the water on each side so the abutment has
                // somewhere to land.
                const std::size_t first = spanStart > 0 ? spanStart - 1 : spanStart;
                const std::size_t last = std::min(road.samples.size() - 1, i);
                const float startDistance = road.samples[first].distance;
                const float endDistance = road.samples[last].distance;
                if (endDistance - startDistance > 1.5f) {
                    BridgeSpan span;
                    span.id = road.spec.id + "-bridge-" + std::to_string(bridges_.size());
                    span.roadId = road.spec.id;
                    span.riverId = riverId;
                    span.x = road.samples[(first + last) / 2].x;
                    span.z = road.samples[(first + last) / 2].z;
                    span.yaw = std::atan2(road.samples[last].x - road.samples[first].x,
                                          road.samples[last].z - road.samples[first].z);
                    span.lengthMetres = endDistance - startDistance;
                    span.widthMetres = road.spec.widthMetres + road.spec.shoulderMetres * 2.0f;
                    span.deckHeight = deckHeight;
                    span.riverWidthMetres = riverWidth;
                    span.waterHeight = deckHeight - 1.35f;
                    bridges_.push_back(span);
                    road.decks.push_back({first, last, deckHeight});
                }
            }
        }
        if (inSpan) {
            const std::size_t first = spanStart > 0 ? spanStart - 1 : spanStart;
            const std::size_t last = road.samples.size() - 1;
            BridgeSpan span;
            span.id = road.spec.id + "-bridge-" + std::to_string(bridges_.size());
            span.roadId = road.spec.id;
            span.riverId = riverId;
            span.x = road.samples[(first + last) / 2].x;
            span.z = road.samples[(first + last) / 2].z;
            span.yaw = std::atan2(road.samples[last].x - road.samples[first].x,
                                  road.samples[last].z - road.samples[first].z);
            span.lengthMetres = road.samples[last].distance - road.samples[first].distance;
            span.widthMetres = road.spec.widthMetres + road.spec.shoulderMetres * 2.0f;
            span.deckHeight = deckHeight;
            span.riverWidthMetres = riverWidth;
            span.waterHeight = deckHeight - 1.35f;
            bridges_.push_back(span);
            road.decks.push_back({first, last, deckHeight});
        }
    }
}

} // namespace world
} // namespace engine
