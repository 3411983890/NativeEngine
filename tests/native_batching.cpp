// Draw batching contract tests.
//
// The plan is the only thing standing between "608 draws" and "16 draws", and every way
// it can be wrong is silent: merging two materials shows the wrong texture, sorting
// batches breaks transparent ordering, and folding a skinned item into a batch reuses
// another character's palette. Each case below pins one of those.

#include "../renderer/batching.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace nativegpu;

namespace {

std::array<float, 16> translation(float x, float y, float z) {
    std::array<float, 16> m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = x; m[13] = y; m[14] = z;
    return m;
}

std::array<float, 4> rgba(float r, float g, float b, float a) { return {r, g, b, a}; }

// 608 town-like items spread over 16 materials, the shape measured from
// .screen/town.scene.json.
void testTownShapeCollapses() {
    std::vector<std::array<float, 16>> models;
    std::vector<std::array<float, 4>> colors;
    models.reserve(608);
    colors.reserve(608);
    for (int i = 0; i < 608; ++i) {
        models.push_back(translation(float(i % 32) * 2.0f, 0.0f, float(i / 32) * 2.0f));
        colors.push_back(rgba(1, 1, 1, 1));
    }

    std::vector<BatchInput> inputs;
    inputs.reserve(608);
    for (int i = 0; i < 608; ++i) {
        BatchInput input;
        input.model = models[std::size_t(i)].data();
        input.color = colors[std::size_t(i)].data();
        input.material = i % 16;
        inputs.push_back(input);
    }

    BatchPlan plan;
    planBatches(inputs, plan);
    assert(plan.batches.size() == 16);
    assert(plan.instances.size() == 608);
    assert(plan.skinnedItems == 0);

    unsigned covered = 0;
    for (const DrawBatch& batch : plan.batches) {
        assert(batch.firstInstance == covered);
        covered += batch.instanceCount;
    }
    assert(covered == 608);
    std::cout << "  608 items over 16 materials -> " << plan.batches.size() << " batches\n";
}

void testBatchOrderFollowsFirstOccurrence() {
    const auto a = translation(0, 0, 0), b = translation(1, 0, 0), c = translation(2, 0, 0);
    const auto white = rgba(1, 1, 1, 1);
    BatchInput items[3] = {};
    items[0].model = a.data(); items[0].color = white.data(); items[0].material = 5;
    items[1].model = b.data(); items[1].color = white.data(); items[1].material = 2;
    items[2].model = c.data(); items[2].color = white.data(); items[2].material = 5;

    BatchPlan plan;
    planBatches({items, items + 3}, plan);
    assert(plan.batches.size() == 2);
    // Material 5 appeared first, so its batch is first. Batches keep first-occurrence
    // order even though their instance ranges are packed contiguously.
    assert(plan.batches[0].material == 5);
    assert(plan.batches[0].instanceCount == 2);
    assert(plan.batches[0].firstInstance == 0);
    assert(plan.batches[1].material == 2);
    assert(plan.batches[1].firstInstance == 2);
}

void testTransparentDoesNotMergeWithOpaque() {
    const auto m = translation(0, 0, 0);
    const auto white = rgba(1, 1, 1, 1);
    BatchInput items[2] = {};
    items[0].model = m.data(); items[0].color = white.data(); items[0].material = 3;
    items[1].model = m.data(); items[1].color = white.data(); items[1].material = 3;
    items[1].transparent = true;

    BatchPlan plan;
    planBatches({items, items + 2}, plan);
    // Transparent items keep their submission order, so they are counted and left on the
    // per-item path instead of being folded into a material batch.
    assert(plan.batches.size() == 1);
    assert(!plan.batches[0].transparent);
    assert(plan.transparentItems == 1);
}

void testSkinnedItemsStayOnThePerItemPath() {
    const auto m = translation(0, 0, 0);
    const auto white = rgba(1, 1, 1, 1);
    BatchInput items[3] = {};
    items[0].model = m.data(); items[0].color = white.data(); items[0].material = 1;
    items[1].model = m.data(); items[1].color = white.data(); items[1].material = 1; items[1].skinned = true;
    items[2].model = m.data(); items[2].color = white.data(); items[2].material = 1;

    BatchPlan plan;
    planBatches({items, items + 3}, plan);
    assert(plan.skinnedItems == 1);
    assert(plan.instances.size() == 2);
    assert(plan.batches.size() == 1);
    assert(plan.batches[0].instanceCount == 2);
}

void testDistinctIndexRangesStaySeparate() {
    const auto m = translation(0, 0, 0);
    const auto white = rgba(1, 1, 1, 1);
    BatchInput items[2] = {};
    items[0].model = m.data(); items[0].color = white.data(); items[0].material = 0;
    items[1].model = m.data(); items[1].color = white.data(); items[1].material = 0;
    items[1].mesh = 7; items[1].firstIndex = 36; items[1].indexCount = 72;

    BatchPlan plan;
    planBatches({items, items + 2}, plan);
    assert(plan.batches.size() == 2);
    assert(plan.batches[1].mesh == 7);
    assert(plan.batches[1].indexCount == 72);
}

void testRecordCarriesPerInstanceState() {
    const auto m = translation(3, 4, 5);
    const auto tint = rgba(0.25f, 0.5f, 0.75f, 1.0f);
    BatchInput input;
    input.model = m.data(); input.color = tint.data();
    input.material = 9; input.roughness = 0.33f; input.metalness = 0.66f;

    BatchPlan plan;
    planBatches({&input, &input + 1}, plan);
    assert(plan.instances.size() == 1);
    const InstanceRecord& record = plan.instances[0];
    assert(std::fabs(record.model[12] - 3.0f) < 1e-6f);
    assert(std::fabs(record.model[13] - 4.0f) < 1e-6f);
    assert(std::fabs(record.model[14] - 5.0f) < 1e-6f);
    assert(std::fabs(record.color[2] - 0.75f) < 1e-6f);
    assert(std::fabs(record.roughness - 0.33f) < 1e-6f);
    assert(std::fabs(record.metalness - 0.66f) < 1e-6f);
}

void testReuseClearsPreviousPlan() {
    const auto m = translation(0, 0, 0);
    const auto white = rgba(1, 1, 1, 1);
    BatchInput input;
    input.model = m.data(); input.color = white.data(); input.material = 4;

    BatchPlan plan;
    planBatches({&input, &input + 1}, plan);
    assert(plan.instances.size() == 1);
    planBatches({}, plan);
    assert(plan.instances.empty());
    assert(plan.batches.empty());
}

} // namespace

int main() {
    testTownShapeCollapses();
    testBatchOrderFollowsFirstOccurrence();
    testTransparentDoesNotMergeWithOpaque();
    testSkinnedItemsStayOnThePerItemPath();
    testDistinctIndexRangesStaySeparate();
    testRecordCarriesPerInstanceState();
    testReuseClearsPreviousPlan();
    std::cout << "Native batching: material grouping, first-occurrence order, pass split, "
                 "skinned exclusion, index-range separation and record fidelity passed.\n";
    return 0;
}
