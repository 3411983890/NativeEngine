// Bonded fracture tests: grid construction, bond topology, energy thresholds,
// island extraction (union-find), mass conservation and determinism.

#include "../engine/native/fracture.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace engine;
using namespace engine::fracture;

namespace {

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

void testGridTopology() {
    Structure structure;
    assert(structure.buildBox({1.5f, 1.0f, 0.15f}, 6, 4, 1, 2.0f));
    assert(structure.cellCount() == 24);
    // 6x4 single layer: 5*4 vertical seams + 6*3 horizontal seams.
    assert(structure.bondCount() == 20 + 18);
    assert(near(structure.totalVolume(), 1.8f, 1.0e-4f));
    assert(near(structure.totalMass(), 3.6f, 1.0e-4f));
    const std::vector<Island> parts = structure.islands();
    assert(parts.size() == 1);
    assert(parts[0].cells.size() == 24);
    assert(parts[0].volume > 1.79f);
    // Bond areas: interior seams are 0.5 * 0.3 = 0.15 m^2.
    float minArea = 1.0e9f;
    float maxArea = 0.0f;
    for (const Bond& bond : structure.bonds()) {
        minArea = std::min(minArea, bond.area);
        maxArea = std::max(maxArea, bond.area);
    }
    assert(near(minArea, 0.15f, 1.0e-4f));
    assert(near(maxArea, 0.15f, 1.0e-4f));
}

void testEnergyThreshold() {
    Structure structure;
    assert(structure.buildBox({1.5f, 1.0f, 0.15f}, 6, 4, 1, 2.0f));
    // Below the per-bond threshold (toughness * area = 6 J), nothing breaks.
    assert(structure.applyEnergy({-0.25f, -0.25f, 0.0f}, 2.0f, 0.45f, 40.0f) == 0);
    assert(structure.brokenBonds() == 0);
    // Concentrated hit on an interior cell: its four bonds break, the cell detaches.
    const int broken = structure.applyEnergy({-0.25f, -0.25f, 0.0f}, 30.0f, 0.45f, 40.0f);
    assert(broken == 4);
    assert(structure.brokenBonds() == 4);
    const std::vector<Island> parts = structure.islands();
    assert(parts.size() == 2);
    int detached = -1;
    for (const Island& island : parts) {
        if (island.cells.size() == 1) detached = island.cells[0];
    }
    assert(detached >= 0);
    // Volume is conserved: the hole is a cell, not a deletion.
    assert(near(structure.totalVolume(), 1.8f, 1.0e-4f));
    float islandVolume = 0.0f;
    for (const Island& island : parts) islandVolume += island.volume;
    assert(near(islandVolume, structure.totalVolume(), 1.0e-4f));
    assert(structure.largestIsland() == 0);
}

void testSeamSplit() {
    Structure structure;
    assert(structure.buildBox({1.5f, 1.0f, 0.15f}, 6, 4, 1, 2.0f));
    // Break every bond across the middle seam (i = 2 -> i = 3).
    int seams = 0;
    for (Bond& bond : structure.bonds()) {
        const Cell& a = structure.cells()[static_cast<std::size_t>(bond.a)];
        const Cell& b = structure.cells()[static_cast<std::size_t>(bond.b)];
        if (std::abs(b.center.x - a.center.x) < 1.0e-4f) continue;
        const float mid = (a.center.x + b.center.x) * 0.5f;
        if (std::abs(mid) < 1.0e-3f) {
            bond.broken = true;
            ++seams;
        }
    }
    assert(seams == 4);
    const std::vector<Island> parts = structure.islands();
    assert(parts.size() == 2);
    assert(parts[0].cells.size() == 12);
    assert(parts[1].cells.size() == 12);
    assert(near(parts[0].volume + parts[1].volume, structure.totalVolume(), 1.0e-4f));
}

void testRepeatedHitsWidenHole() {
    Structure structure;
    assert(structure.buildBox({1.5f, 1.0f, 0.15f}, 6, 4, 1, 2.0f));
    int totalBroken = 0;
    for (int hit = 0; hit < 4; ++hit) {
        totalBroken += structure.applyEnergy({-0.25f, -0.25f, 0.0f}, 18.0f, 0.45f, 40.0f);
    }
    assert(totalBroken >= 4);
    const std::vector<Island> parts = structure.islands();
    assert(parts.size() >= 2);
    // The main body must still hold more than half of the wall.
    const Island& main = parts[static_cast<std::size_t>(structure.largestIsland())];
    assert(main.volume > structure.totalVolume() * 0.5f);
}

void testDeterminism() {
    Structure a;
    Structure b;
    assert(a.buildBox({1.5f, 1.0f, 0.15f}, 6, 4, 1, 2.0f));
    assert(b.buildBox({1.5f, 1.0f, 0.15f}, 6, 4, 1, 2.0f));
    for (Structure* structure : {&a, &b}) {
        structure->applyEnergy({-0.25f, -0.25f, 0.0f}, 30.0f, 0.45f, 40.0f);
        structure->applyEnergy({0.75f, 0.25f, 0.0f}, 12.0f, 0.35f, 40.0f);
    }
    assert(a.brokenBonds() == b.brokenBonds());
    assert(a.damageSum() == b.damageSum());
    const std::vector<Island> left = a.islands();
    const std::vector<Island> right = b.islands();
    assert(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        assert(left[i].cells == right[i].cells);
    }
}

void testDivisionHeuristic() {
    int nx = 0, ny = 0, nz = 0;
    chooseDivisions({1.5f, 1.0f, 0.15f}, 24, nx, ny, nz);
    assert(nz == 1);  // a 0.3 m wall must not be split through its thickness
    assert(nx * ny * nz <= 24);
    assert(nx >= 2 && ny >= 2);
    chooseDivisions({0.25f, 0.25f, 0.25f}, 512, nx, ny, nz);
    assert(nx * ny * nz <= static_cast<int>(Structure::kMaxCells));
    Structure tooBig;
    assert(!tooBig.buildBox({1.0f, 1.0f, 1.0f}, 64, 64, 64, 1.0f));
}

// A baked Voronoi package replaces the uniform grid: the cell/bond graph comes from the
// bake, and each cell keeps a mesh index so the renderer can draw the hull. The graph
// must still behave like a structure: energy opens a hole, the largest island survives,
// and a malformed graph is rejected instead of producing a half-built structure.
void testAdoptBakedShards() {
    Structure structure;
    // Three shards in a row, each 1x1x0.3, sharing two bonds. Volumes are the shard
    // volumes the bake would report.
    std::vector<Cell> cells(3);
    for (int index = 0; index < 3; ++index) {
        cells[static_cast<std::size_t>(index)].center = {-1.0f + static_cast<float>(index), 0.0f, 0.0f};
        cells[static_cast<std::size_t>(index)].half = {0.5f, 0.5f, 0.15f};
        cells[static_cast<std::size_t>(index)].volume = 0.3f;
        cells[static_cast<std::size_t>(index)].mass = 0.6f;
        cells[static_cast<std::size_t>(index)].mesh = index;
    }
    std::vector<Bond> bonds(2);
    bonds[0] = {0, 1, 0.3f, 0.0f, false};
    bonds[1] = {1, 2, 0.3f, 0.0f, false};
    assert(structure.adopt(cells, bonds));
    assert(structure.cellCount() == 3);
    assert(structure.bondCount() == 2);
    assert(near(structure.totalVolume(), 0.9f, 1.0e-5f));
    assert(structure.cells()[2].mesh == 2);
    assert(structure.islands().size() == 1);

    // A malformed graph must be refused: an out-of-range bond, a zero-volume cell and a
    // self-bond are all authoring errors, not something to clamp silently.
    Structure broken;
    std::vector<Cell> badCells = cells;
    badCells[1].volume = 0.0f;
    assert(!broken.adopt(badCells, bonds));
    std::vector<Bond> badBonds = bonds;
    badBonds[0].b = 7;
    assert(!broken.adopt(cells, badBonds));
    badBonds[0].b = 0;
    assert(!broken.adopt(cells, badBonds));
    assert(broken.cellCount() == 0);

    // Adopting resets any previous graph rather than appending to it.
    Structure reused;
    assert(reused.buildBox({1.0f, 1.0f, 0.15f}, 3, 3, 1, 1.0f));
    assert(reused.cellCount() == 9);
    assert(reused.adopt(cells, bonds));
    assert(reused.cellCount() == 3);
}

} // namespace

int main() {
    testGridTopology();
    testEnergyThreshold();
    testSeamSplit();
    testRepeatedHitsWidenHole();
    testDeterminism();
    testDivisionHeuristic();
    testAdoptBakedShards();
    std::cout << "Native fracture: grid topology, bond areas, energy threshold, island "
                 "union-find, repeated-hit widening, determinism, division limits and "
                 "baked Voronoi adoption passed.\n";
    return 0;
}
