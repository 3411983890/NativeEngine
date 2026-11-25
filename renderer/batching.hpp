#pragma once

// Draw-call batching: turns the per-item submission list into per-material instance
// batches.
//
// The renderer currently issues one DrawIndexedInstanced(..., 1, ...) per scene item, so
// a 608-entity town costs 608 draws and 608 root-constant writes. Measuring the town
// (tools/cull-audit.mjs) says frustum culling removes 2.3% of those items while grouping
// by material collapses 608 into 16 — the draw count itself is the bottleneck, not the
// visible set.
//
// This header is the planning half only: it decides which items share a draw and what
// per-instance record each one contributes. It performs no GPU work, so it can be unit
// tested without a device. The GPU half (instance SRV + shader variant + instanced draw)
// plugs in against this plan.
//
// Three invariants are easy to lose and are enforced here:
//   * Only opaque items are batched. Transparent items are submitted back-to-front by the
//     caller, and grouping them by material would reorder them. They stay on the per-item
//     path until there is a depth-sorted transparent pass.
//   * Instances of a batch are contiguous. A single DrawIndexedInstanced can only address
//     a contiguous instance range, so the plan packs each batch's records together instead
//     of leaving them strided through the buffer. Order within a batch is input order.
//   * Skinned items are never batched. Their per-draw state includes a palette slot and a
//     deformation table indexed by draw order; folding them into a shared batch would
//     silently reuse the wrong skin.

#include <cstdint>
#include <vector>

namespace nativegpu {

// 96 B, 16-byte aligned so the HLSL StructuredBuffer stride matches without padding
// surprises. model + color + the two scalar material overrides are everything the vertex
// shader needs per item; the material table indices stay in root constants because a
// batch shares one material by construction.
struct InstanceRecord {
    float model[16];
    float color[4];
    float roughness;
    float metalness;
    float padding[2];
};
static_assert(sizeof(InstanceRecord) == 96, "InstanceRecord stride is a shader contract");

// One input item, decoupled from DrawItem so this stays testable without D3D12 headers.
struct BatchInput {
    const float* model = nullptr;  // 16 floats, column-major
    const float* color = nullptr;  // 4 floats
    int material = -1;
    int mesh = -1;
    unsigned firstIndex = 0;
    unsigned indexCount = 0;
    float roughness = 0.72f;
    float metalness = 0.0f;
    bool transparent = false;
    bool skinned = false;
};

struct DrawBatch {
    int material = -1;
    int mesh = -1;
    unsigned firstIndex = 0;
    unsigned indexCount = 0;
    unsigned firstInstance = 0;
    unsigned instanceCount = 0;
    bool transparent = false;
};

struct BatchPlan {
    std::vector<InstanceRecord> instances;
    std::vector<DrawBatch> batches;
    unsigned skinnedItems = 0;
    unsigned transparentItems = 0;
    // Reused across frames. The planner runs on the submit path, so it must not allocate
    // once the plan has grown to the scene's size.
    std::vector<unsigned> batchOf;
    std::vector<unsigned> order;
    std::vector<unsigned> write;
};

// Groups items that differ only in their per-instance record. The key is everything the
// draw call itself carries (material, mesh, index range, pass); the record carries the
// rest. Items are appended in input order, so a caller that submits back-to-front keeps
// that order inside each batch.
inline void planBatches(const std::vector<BatchInput>& inputs, BatchPlan& plan) {
    plan.instances.clear();
    plan.batches.clear();
    plan.skinnedItems = 0;
    plan.transparentItems = 0;
    plan.instances.reserve(inputs.size());
    plan.batches.reserve(32);

    // Pass 1: assign every batchable item to a batch and remember the order.
    plan.batchOf.assign(inputs.size(), 0xFFFFFFFFu);
    plan.order.clear();
    plan.order.reserve(inputs.size());
    auto& batchOf = plan.batchOf;
    auto& order = plan.order;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const BatchInput& input = inputs[i];
        if (input.skinned) { ++plan.skinnedItems; continue; }
        if (input.transparent) { ++plan.transparentItems; continue; }
        if (!input.model || !input.color) continue;

        unsigned slot = 0xFFFFFFFFu;
        for (std::size_t b = 0; b < plan.batches.size(); ++b) {
            const DrawBatch& candidate = plan.batches[b];
            if (candidate.material == input.material && candidate.mesh == input.mesh &&
                candidate.firstIndex == input.firstIndex && candidate.indexCount == input.indexCount) {
                slot = static_cast<unsigned>(b);
                break;
            }
        }
        if (slot == 0xFFFFFFFFu) {
            DrawBatch created;
            created.material = input.material;
            created.mesh = input.mesh;
            created.firstIndex = input.firstIndex;
            created.indexCount = input.indexCount;
            plan.batches.push_back(created);
            slot = static_cast<unsigned>(plan.batches.size() - 1);
        }
        batchOf[i] = slot;
        order.push_back(static_cast<unsigned>(i));
    }

    // Pass 2: prefix sum gives each batch its contiguous instance range.
    for (DrawBatch& batch : plan.batches) batch.instanceCount = 0;
    for (unsigned index : order) ++plan.batches[batchOf[index]].instanceCount;
    unsigned cursor = 0;
    for (DrawBatch& batch : plan.batches) {
        batch.firstInstance = cursor;
        cursor += batch.instanceCount;
    }

    // Pass 3: fill the instance buffer at each batch's cursor, preserving input order.
    plan.instances.resize(order.size());
    plan.write.resize(plan.batches.size());
    auto& write = plan.write;
    for (std::size_t b = 0; b < plan.batches.size(); ++b) write[b] = plan.batches[b].firstInstance;
    for (unsigned index : order) {
        const BatchInput& input = inputs[index];
        InstanceRecord record{};
        for (int i = 0; i < 16; ++i) record.model[i] = input.model[i];
        for (int i = 0; i < 4; ++i) record.color[i] = input.color[i];
        record.roughness = input.roughness;
        record.metalness = input.metalness;
        plan.instances[write[batchOf[index]]++] = record;
    }
}

} // namespace nativegpu
