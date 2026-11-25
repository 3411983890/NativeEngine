#pragma once

// Bonded (cell + bond) destruction for destructible box props.
//
// The entity is split into a grid of cells. Neighbouring cells share a bond whose
// area is the shared face, so a corner bond is weaker than a face bond. Impacts
// deposit energy into nearby bonds; a bond breaks when the energy per unit area
// exceeds the material toughness. Union-find over the surviving bonds yields
// islands: the largest island stays as the body, smaller islands detach and fall.
//
// This deliberately does not need Voronoi cells: a cell is a box, so the existing
// instanced-OBB renderer and the OBB narrowphase keep working. `Cell` only exposes
// centre/half/volume/mass, so the offline convex bake can replace the geometry
// later without touching the bond graph or the union-find.

#include "vec3.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace engine {
namespace fracture {

struct Cell {
    Vec3 center;  // local to the entity
    Vec3 half;
    float volume = 0.0f;
    float mass = 0.0f;
    int body = -1;          // rigid body once the cell is backed by a body
    bool detached = false;  // island no longer supported, simulated on its own
    float damage = 0.0f;    // accumulated 0..1; >= 1 pulverises the cell
    // Index into the owner's convex mesh table, or -1 when the cell renders as a box.
    // The mesh is in body-local space; `center`/`half` remain the physics proxy.
    int mesh = -1;
    // Cut-face mesh drawn as a second, differently tinted pose. -1 when the shard has no
    // interior faces (every face was on the original surface).
    int meshInterior = -1;
};

struct Bond {
    int a = -1;
    int b = -1;
    float area = 0.0f;    // shared face area, m^2
    float damage = 0.0f;  // accumulated 0..1
    bool broken = false;
};

struct Island {
    std::vector<int> cells;
    float volume = 0.0f;
    Vec3 center;  // volume-weighted centre, local
};

class Structure {
public:
    void clear() {
        cells_.clear();
        bonds_.clear();
    }

    // Uniform grid split. Divisions are clamped to [1, 64] and the total cell
    // count to kMaxCells; a larger request is rejected (returns false) instead of
    // silently degrading, so callers can fall back to whole-entity fracture.
    bool buildBox(Vec3 half, int nx, int ny, int nz, float density = 1.0f) {
        clear();
        nx = std::clamp(nx, 1, 64);
        ny = std::clamp(ny, 1, 64);
        nz = std::clamp(nz, 1, 64);
        if (nx * ny * nz > static_cast<int>(kMaxCells)) return false;
        const Vec3 size{half.x * 2.0f / static_cast<float>(nx),
                        half.y * 2.0f / static_cast<float>(ny),
                        half.z * 2.0f / static_cast<float>(nz)};
        const float cellVolume = size.x * size.y * size.z;
        cells_.reserve(static_cast<std::size_t>(nx * ny * nz));
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    Cell cell;
                    cell.center = {-half.x + size.x * (static_cast<float>(i) + 0.5f),
                                   -half.y + size.y * (static_cast<float>(j) + 0.5f),
                                   -half.z + size.z * (static_cast<float>(k) + 0.5f)};
                    cell.half = size * 0.5f;
                    cell.volume = cellVolume;
                    cell.mass = cellVolume * density;
                    cells_.push_back(cell);
                }
            }
        }
        auto index = [nx, ny](int i, int j, int k) { return (k * ny + j) * nx + i; };
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    if (i + 1 < nx) {
                        addBond(index(i, j, k), index(i + 1, j, k), size.y * size.z);
                    }
                    if (j + 1 < ny) {
                        addBond(index(i, j, k), index(i, j + 1, k), size.x * size.z);
                    }
                    if (k + 1 < nz) {
                        addBond(index(i, j, k), index(i, j, k + 1), size.x * size.y);
                    }
                }
            }
        }
        return true;
    }

    // Adopts an externally built cell/bond graph (the offline Voronoi bake). Cells keep
    // their mesh index so the renderer can draw the real hull while `center`/`half`
    // stay the OBB physics proxy. The graph is validated rather than trusted: a bond
    // must reference two live cells, and every cell needs positive volume.
    bool adopt(std::vector<Cell> cells, std::vector<Bond> bonds) {
        clear();
        if (cells.empty() || cells.size() > kMaxCells) return false;
        for (const Cell& cell : cells) {
            if (!(cell.volume > 0.0f) || !(cell.mass > 0.0f)) return false;
            if (!(cell.half.x > 0.0f) || !(cell.half.y > 0.0f) || !(cell.half.z > 0.0f)) return false;
        }
        for (const Bond& bond : bonds) {
            if (bond.a < 0 || bond.b < 0) return false;
            if (static_cast<std::size_t>(bond.a) >= cells.size()) return false;
            if (static_cast<std::size_t>(bond.b) >= cells.size()) return false;
            if (bond.a == bond.b) return false;
        }
        cells_ = std::move(cells);
        bonds_ = std::move(bonds);
        return true;
    }

    // Deposits `energy` (J) around `localPoint`. Energy is shared between bonds
    // whose midpoint is within `radius`, weighted by 1/(1+d^2) so the closest bonds
    // take most of it. A bond breaks when accumulated energy per unit area exceeds
    // `toughness` (J/m^2). Returns the number of bonds broken by this call.
    // Deposits `energy` (J) around `localPoint`. The nearest cells take most of it:
    // a cell whose damage passes 1.0 pulverises, which breaks every bond around it
    // and leaves a hole. Neighbouring bonds also accumulate damage, so repeated
    // hits widen the hole instead of stopping at one cell.
    int applyEnergy(Vec3 localPoint, float energy, float radius, float toughness) {
        if (cells_.empty() || bonds_.empty() || energy <= 0.0f) return 0;
        if (toughness <= 1.0e-6f) toughness = 1.0e-6f;
        radius = std::max(radius, 1.0e-3f);
        const float radiusSquared = radius * radius;

        weights_.assign(cells_.size(), 0.0f);
        float totalWeight = 0.0f;
        for (std::size_t index = 0; index < cells_.size(); ++index) {
            const Vec3 offset = cells_[index].center - localPoint;
            const float distanceSquared =
                offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
            if (distanceSquared > radiusSquared) continue;
            weights_[index] = 1.0f / (1.0f + distanceSquared);
            totalWeight += weights_[index];
        }
        if (totalWeight <= 0.0f) {
            // Nothing in range: dump the energy into the nearest cell so a hit on a
            // thin shell still does something instead of silently vanishing.
            std::size_t nearest = 0;
            float best = 1.0e30f;
            for (std::size_t index = 0; index < cells_.size(); ++index) {
                const Vec3 offset = cells_[index].center - localPoint;
                const float distanceSquared =
                    offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
                if (distanceSquared < best) {
                    best = distanceSquared;
                    nearest = index;
                }
            }
            weights_[nearest] = 1.0f;
            totalWeight = 1.0f;
        }

        // Pass 1: per-cell damage. A pulverised cell breaks all of its bonds.
        int broken = 0;
        for (std::size_t index = 0; index < cells_.size(); ++index) {
            if (weights_[index] <= 0.0f) continue;
            Cell& cell = cells_[index];
            const float area = std::max(4.0f * cell.half.x * cell.half.y, 1.0e-4f);
            const float share = energy * (weights_[index] / totalWeight);
            cell.damage += share / (toughness * area);
            if (cell.damage < 1.0f) continue;
            for (Bond& bond : bonds_) {
                if (bond.broken) continue;
                if (bond.a != static_cast<int>(index) && bond.b != static_cast<int>(index)) {
                    continue;
                }
                bond.broken = true;
                bond.damage = 1.0f;
                ++broken;
            }
        }

        // Pass 2: bonds in range absorb energy even when their cells survive, so the
        // ring around a hole is weakened rather than pristine.
        for (std::size_t index = 0; index < bonds_.size(); ++index) {
            Bond& bond = bonds_[index];
            if (bond.broken) continue;
            const Vec3 offset = bondMidpoint(bond) - localPoint;
            const float distanceSquared =
                offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
            if (distanceSquared > radiusSquared) continue;
            const float weight = 1.0f / (1.0f + distanceSquared);
            const float share = energy * (weight / totalWeight);
            bond.damage += share / (toughness * std::max(bond.area, 1.0e-6f));
            if (bond.damage >= 1.0f) {
                bond.broken = true;
                ++broken;
            }
        }
        return broken;
    }

    // Union-find over unbroken bonds, in bond order. Islands are returned sorted by
    // their smallest cell index, and cells inside an island are ascending.
    std::vector<Island> islands() const {
        std::vector<int> parent(cells_.size());
        for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
        auto find = [&parent](int node) {
            while (parent[static_cast<std::size_t>(node)] != node) {
                parent[static_cast<std::size_t>(node)] =
                    parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(node)])];
                node = parent[static_cast<std::size_t>(node)];
            }
            return node;
        };
        for (const Bond& bond : bonds_) {
            if (bond.broken) continue;
            if (bond.a < 0 || bond.b < 0) continue;
            const int rootA = find(bond.a);
            const int rootB = find(bond.b);
            if (rootA == rootB) continue;
            const int low = std::min(rootA, rootB);
            const int high = std::max(rootA, rootB);
            parent[static_cast<std::size_t>(high)] = low;
        }
        std::vector<Island> result;
        std::vector<int> lookup(cells_.size(), -1);
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            const int root = find(static_cast<int>(i));
            int slot = lookup[static_cast<std::size_t>(root)];
            if (slot < 0) {
                slot = static_cast<int>(result.size());
                lookup[static_cast<std::size_t>(root)] = slot;
                result.push_back({});
            }
            Island& island = result[static_cast<std::size_t>(slot)];
            island.cells.push_back(static_cast<int>(i));
            island.volume += cells_[i].volume;
            island.center += cells_[i].center * cells_[i].volume;
        }
        for (Island& island : result) {
            if (island.volume > 0.0f) island.center = island.center / island.volume;
            std::sort(island.cells.begin(), island.cells.end());
        }
        std::sort(result.begin(), result.end(), [](const Island& left, const Island& right) {
            if (left.cells.empty() || right.cells.empty()) return left.cells.size() < right.cells.size();
            return left.cells.front() < right.cells.front();
        });
        return result;
    }

    int largestIsland() const {
        const std::vector<Island> parts = islands();
        int best = -1;
        float bestVolume = -1.0f;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].volume > bestVolume) {
                bestVolume = parts[i].volume;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    float totalVolume() const {
        float volume = 0.0f;
        for (const Cell& cell : cells_) volume += cell.volume;
        return volume;
    }

    float totalMass() const {
        float mass = 0.0f;
        for (const Cell& cell : cells_) mass += cell.mass;
        return mass;
    }

    int brokenBonds() const {
        int count = 0;
        for (const Bond& bond : bonds_) {
            if (bond.broken) ++count;
        }
        return count;
    }

    float damageSum() const {
        float sum = 0.0f;
        for (const Bond& bond : bonds_) sum += bond.damage;
        for (const Cell& cell : cells_) sum += cell.damage;
        return sum;
    }

    std::size_t cellCount() const { return cells_.size(); }
    std::size_t bondCount() const { return bonds_.size(); }
    const std::vector<Cell>& cells() const { return cells_; }
    std::vector<Cell>& cells() { return cells_; }
    const std::vector<Bond>& bonds() const { return bonds_; }
    std::vector<Bond>& bonds() { return bonds_; }

    static constexpr std::size_t kMaxCells = 256;

private:
    Vec3 bondMidpoint(const Bond& bond) const {
        return (cells_[static_cast<std::size_t>(bond.a)].center +
                cells_[static_cast<std::size_t>(bond.b)].center) * 0.5f;
    }

    void addBond(int a, int b, float area) {
        Bond bond;
        bond.a = a;
        bond.b = b;
        bond.area = std::max(area, 1.0e-6f);
        bonds_.push_back(bond);
    }

    std::vector<Cell> cells_;
    std::vector<Bond> bonds_;
    mutable std::vector<float> weights_;  // scratch for applyEnergy
};

// Chooses a grid close to cubic for a box, clamped to kMaxCells. A wall gets a
// coarse grid along its thickness so it does not turn into hundreds of paper cells.
inline void chooseDivisions(Vec3 half, int targetCells, int& nx, int& ny, int& nz) {
    targetCells = std::clamp(targetCells, 1, static_cast<int>(Structure::kMaxCells));
    const float sx = std::max(half.x, 1.0e-3f);
    const float sy = std::max(half.y, 1.0e-3f);
    const float sz = std::max(half.z, 1.0e-3f);
    const float volume = sx * sy * sz;
    const float cellSize = std::cbrt(volume / static_cast<float>(targetCells));
    nx = std::clamp(static_cast<int>(std::round(sx / cellSize)), 1, 64);
    ny = std::clamp(static_cast<int>(std::round(sy / cellSize)), 1, 64);
    nz = std::clamp(static_cast<int>(std::round(sz / cellSize)), 1, 64);
    // Thin axes must stay at one cell: splitting a 0.3 m wall through its thickness
    // produces shards that are thinner than the contact slop.
    if (sx < 0.22f) nx = 1;
    if (sy < 0.22f) ny = 1;
    if (sz < 0.22f) nz = 1;
    // Shrink the widest axis until the grid fits the requested cell budget.
    while (nx * ny * nz > targetCells) {
        if (nx >= ny && nx >= nz && nx > 1) --nx;
        else if (ny >= nz && ny > 1) --ny;
        else if (nz > 1) --nz;
        else break;
    }
}

} // namespace fracture
} // namespace engine
