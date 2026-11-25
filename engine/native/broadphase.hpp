#pragma once

// Deterministic dynamic AABB tree (BVH) used as the rigid-body broadphase.
//
// Contract with the solver:
//   * The tree only *filters* candidate pairs. Narrowphase still runs the exact
//     SAT test, so a fat AABB can never create a contact, only extra candidates.
//   * Callers sort the emitted pairs by (bodyA, bodyB) and run narrowphase in that
//     order. That makes the contact stream identical to the O(n^2) reference path,
//     which is what keeps replays and the determinism digest byte-stable.
//   * No AVL rotations. Tree quality is maintained by a rebuild heuristic
//     (insertion count and height) that rebuilds from centroids with a total order
//     (centroid, then user id), so the structure never depends on sort stability.
//   * Fat AABBs: a leaf is only re-inserted when its tight box leaves the stored
//     box, so resting bodies cost one containment test per step.
//   * Single threaded and allocation-free during queries. Not safe to share
//     between threads.

#include "vec3.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine {
namespace physics {

struct Aabb {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extent() const { return max - min; }
    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

    float surfaceArea() const {
        const Vec3 d = extent();
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    bool contains(const Aabb& other) const {
        return min.x <= other.min.x && min.y <= other.min.y && min.z <= other.min.z &&
               max.x >= other.max.x && max.y >= other.max.y && max.z >= other.max.z;
    }

    // Touching counts as overlap: a conservative answer is always safe here.
    bool overlaps(const Aabb& other) const {
        return min.x <= other.max.x && max.x >= other.min.x &&
               min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }

    Aabb expanded(float margin) const {
        Aabb result = *this;
        if (margin <= 0.0f) return result;
        result.min = {min.x - margin, min.y - margin, min.z - margin};
        result.max = {max.x + margin, max.y + margin, max.z + margin};
        return result;
    }

    Aabb merged(const Aabb& other) const {
        Aabb result;
        result.min = {std::min(min.x, other.min.x), std::min(min.y, other.min.y),
                      std::min(min.z, other.min.z)};
        result.max = {std::max(max.x, other.max.x), std::max(max.y, other.max.y),
                      std::max(max.z, other.max.z)};
        return result;
    }
};

struct AabbPair {
    unsigned a = 0;
    unsigned b = 0;
};

class AabbTree {
public:
    static constexpr int kNull = -1;

    void clear() {
        nodes_.clear();
        proxies_.clear();
        freeProxies_.clear();
        stack_.clear();
        root_ = kNull;
        freeNode_ = kNull;
        proxyCount_ = 0;
        liveNodes_ = 0;
        insertionCount_ = 0;
        rebuilds_ = 0;
    }

    // Returns a stable handle. The handle is recycled only after destroyProxy.
    int createProxy(const Aabb& box, unsigned user, float margin) {
        const int leaf = allocateNode();
        nodes_[leaf].box = box.expanded(margin);
        nodes_[leaf].user = user;
        insertLeaf(leaf);

        int proxy = kNull;
        if (!freeProxies_.empty()) {
            proxy = freeProxies_.back();
            freeProxies_.pop_back();
            proxies_[proxy] = leaf;
        } else {
            proxies_.push_back(leaf);
            proxy = static_cast<int>(proxies_.size()) - 1;
        }
        ++proxyCount_;
        ++insertionCount_;
        maybeRebuild();
        return proxy;
    }

    void destroyProxy(int proxy) {
        if (proxy < 0 || proxy >= static_cast<int>(proxies_.size())) return;
        const int leaf = proxies_[proxy];
        if (leaf == kNull) return;
        removeLeaf(leaf);
        releaseNode(leaf);
        proxies_[proxy] = kNull;
        freeProxies_.push_back(proxy);
        if (proxyCount_ > 0) --proxyCount_;
    }

    // Re-inserts only when the tight box escapes the stored fat box.
    bool moveProxy(int proxy, const Aabb& box, float margin) {
        if (proxy < 0 || proxy >= static_cast<int>(proxies_.size())) return false;
        const int leaf = proxies_[proxy];
        if (leaf == kNull) return false;
        if (nodes_[leaf].box.contains(box)) return false;
        removeLeaf(leaf);
        nodes_[leaf].box = box.expanded(margin);
        nodes_[leaf].child1 = kNull;
        nodes_[leaf].child2 = kNull;
        nodes_[leaf].height = 0;
        insertLeaf(leaf);
        ++insertionCount_;
        maybeRebuild();
        return true;
    }

    // Emits every overlapping leaf pair exactly once. Order is traversal order;
    // callers must sort. The tree owns the stack buffer, so `out` must not alias it.
    void queryPairs(std::vector<AabbPair>& out) const {
        out.clear();
        if (root_ == kNull || proxyCount_ < 2) return;
        stack_.clear();
        stack_.push_back(root_);
        stack_.push_back(root_);
        while (stack_.size() >= 2) {
            const int b = stack_.back();
            stack_.pop_back();
            const int a = stack_.back();
            stack_.pop_back();
            if (a == kNull || b == kNull) continue;
            if (!nodes_[a].box.overlaps(nodes_[b].box)) continue;
            const bool leafA = isLeaf(a);
            const bool leafB = isLeaf(b);
            if (a == b) {
                if (leafA) continue;
                const int c1 = nodes_[a].child1;
                const int c2 = nodes_[a].child2;
                stack_.push_back(c1);
                stack_.push_back(c1);
                stack_.push_back(c2);
                stack_.push_back(c2);
                stack_.push_back(c1);
                stack_.push_back(c2);
            } else if (leafA && leafB) {
                const unsigned ua = nodes_[a].user;
                const unsigned ub = nodes_[b].user;
                out.push_back({std::min(ua, ub), std::max(ua, ub)});
            } else if (leafA) {
                stack_.push_back(a);
                stack_.push_back(nodes_[b].child1);
                stack_.push_back(a);
                stack_.push_back(nodes_[b].child2);
            } else if (leafB) {
                stack_.push_back(nodes_[a].child1);
                stack_.push_back(b);
                stack_.push_back(nodes_[a].child2);
                stack_.push_back(b);
            } else if (nodes_[a].box.surfaceArea() >= nodes_[b].box.surfaceArea()) {
                // Descend the larger node to keep the stack shallow.
                stack_.push_back(nodes_[a].child1);
                stack_.push_back(b);
                stack_.push_back(nodes_[a].child2);
                stack_.push_back(b);
            } else {
                stack_.push_back(a);
                stack_.push_back(nodes_[b].child1);
                stack_.push_back(a);
                stack_.push_back(nodes_[b].child2);
            }
        }
    }

    unsigned proxyCount() const { return proxyCount_; }
    unsigned nodeCount() const { return liveNodes_; }
    int height() const { return root_ == kNull ? 0 : nodes_[root_].height; }
    unsigned rebuilds() const { return rebuilds_; }
    unsigned insertions() const { return insertionCount_; }
    bool empty() const { return root_ == kNull; }

private:
    struct Node {
        Aabb box;
        int parent = kNull;
        int child1 = kNull;
        int child2 = kNull;
        int next = kNull;  // free-list link
        int height = -1;   // -1 = free, 0 = leaf
        unsigned user = 0;
    };

    bool isLeaf(int index) const { return nodes_[index].child1 == kNull; }

    int allocateNode() {
        int index = kNull;
        if (freeNode_ != kNull) {
            index = freeNode_;
            freeNode_ = nodes_[index].next;
        } else {
            nodes_.push_back(Node{});
            index = static_cast<int>(nodes_.size()) - 1;
        }
        nodes_[index] = Node{};
        nodes_[index].height = 0;
        ++liveNodes_;
        return index;
    }

    void releaseNode(int index) {
        nodes_[index] = Node{};
        nodes_[index].next = freeNode_;
        nodes_[index].height = -1;
        freeNode_ = index;
        if (liveNodes_ > 0) --liveNodes_;
    }

    int refit(int index) {
        const int c1 = nodes_[index].child1;
        const int c2 = nodes_[index].child2;
        nodes_[index].height = 1 + std::max(nodes_[c1].height, nodes_[c2].height);
        nodes_[index].box = nodes_[c1].box.merged(nodes_[c2].box);
        return index;
    }

    float childCost(int child, const Aabb& box) const {
        const Aabb merged = nodes_[child].box.merged(box);
        if (isLeaf(child)) return merged.surfaceArea();
        return merged.surfaceArea() - nodes_[child].box.surfaceArea();
    }

    void insertLeaf(int leaf) {
        if (root_ == kNull) {
            root_ = leaf;
            nodes_[leaf].parent = kNull;
            return;
        }
        const Aabb box = nodes_[leaf].box;
        int index = root_;
        while (!isLeaf(index)) {
            const Aabb& nodeBox = nodes_[index].box;
            const Aabb combined = nodeBox.merged(box);
            const float area = nodeBox.surfaceArea() + 1.0e-6f;
            const float combinedArea = combined.surfaceArea() + 1.0e-6f;
            const float cost = 2.0f * combinedArea;
            const float inheritance = 2.0f * (combinedArea - area);
            const float cost1 = childCost(nodes_[index].child1, box) + inheritance;
            const float cost2 = childCost(nodes_[index].child2, box) + inheritance;
            if (cost < cost1 && cost < cost2) break;
            index = cost1 <= cost2 ? nodes_[index].child1 : nodes_[index].child2;
        }
        const int sibling = index;
        const int oldParent = nodes_[sibling].parent;
        const int newParent = allocateNode();
        nodes_[newParent].parent = oldParent;
        nodes_[newParent].box = box.merged(nodes_[sibling].box);
        nodes_[newParent].height = nodes_[sibling].height + 1;
        nodes_[newParent].child1 = sibling;
        nodes_[newParent].child2 = leaf;
        nodes_[sibling].parent = newParent;
        nodes_[leaf].parent = newParent;
        if (oldParent != kNull) {
            if (nodes_[oldParent].child1 == sibling) nodes_[oldParent].child1 = newParent;
            else nodes_[oldParent].child2 = newParent;
        } else {
            root_ = newParent;
        }
        int ancestor = nodes_[leaf].parent;
        while (ancestor != kNull) {
            refit(ancestor);
            ancestor = nodes_[ancestor].parent;
        }
    }

    void removeLeaf(int leaf) {
        if (leaf == root_) {
            root_ = kNull;
            nodes_[leaf].parent = kNull;
            return;
        }
        const int parent = nodes_[leaf].parent;
        if (parent == kNull) return;
        const int grand = nodes_[parent].parent;
        const int sibling = nodes_[parent].child1 == leaf ? nodes_[parent].child2
                                                         : nodes_[parent].child1;
        if (grand != kNull) {
            if (nodes_[grand].child1 == parent) nodes_[grand].child1 = sibling;
            else nodes_[grand].child2 = sibling;
            nodes_[sibling].parent = grand;
            releaseNode(parent);
            int ancestor = grand;
            while (ancestor != kNull) {
                refit(ancestor);
                ancestor = nodes_[ancestor].parent;
            }
        } else {
            root_ = sibling;
            nodes_[sibling].parent = kNull;
            releaseNode(parent);
        }
        nodes_[leaf].parent = kNull;
    }

    void maybeRebuild() {
        const unsigned limit = std::max<unsigned>(64u, 4u * proxyCount_);
        if (insertionCount_ > limit || height() > 48) rebuild();
    }

    void rebuild() {
        std::vector<int> proxyIds;
        std::vector<Aabb> boxes;
        std::vector<unsigned> users;
        proxyIds.reserve(proxyCount_);
        boxes.reserve(proxyCount_);
        users.reserve(proxyCount_);
        for (int proxy = 0; proxy < static_cast<int>(proxies_.size()); ++proxy) {
            const int leaf = proxies_[proxy];
            if (leaf == kNull) continue;
            proxyIds.push_back(proxy);
            boxes.push_back(nodes_[leaf].box);
            users.push_back(nodes_[leaf].user);
        }
        nodes_.clear();
        root_ = kNull;
        freeNode_ = kNull;
        liveNodes_ = 0;
        std::vector<int> leaves(boxes.size(), kNull);
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const int leaf = allocateNode();
            nodes_[leaf].box = boxes[i];
            nodes_[leaf].user = users[i];
            leaves[i] = leaf;
            proxies_[proxyIds[i]] = leaf;
        }
        root_ = buildRange(leaves, 0, static_cast<int>(leaves.size()));
        insertionCount_ = 0;
        ++rebuilds_;
    }

    // Median split on the widest centroid axis. The comparator is a total order
    // (centroid, then user id) so the result never depends on sort stability.
    int buildRange(std::vector<int>& leaves, int begin, int end) {
        const int count = end - begin;
        if (count <= 0) return kNull;
        if (count == 1) {
            nodes_[leaves[begin]].parent = kNull;
            return leaves[begin];
        }
        Vec3 lo = nodes_[leaves[begin]].box.center();
        Vec3 hi = lo;
        for (int i = begin + 1; i < end; ++i) {
            const Vec3 c = nodes_[leaves[i]].box.center();
            lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
        }
        const Vec3 span = hi - lo;
        int axis = 0;
        if (span.y > span.x) axis = 1;
        if ((axis == 0 ? span.x : span.y) < span.z) axis = 2;
        const int sortAxis = axis;
        std::stable_sort(leaves.begin() + begin, leaves.begin() + end,
                         [this, sortAxis](int left, int right) {
                             const Vec3 a = nodes_[left].box.center();
                             const Vec3 b = nodes_[right].box.center();
                             const float av = sortAxis == 0 ? a.x : (sortAxis == 1 ? a.y : a.z);
                             const float bv = sortAxis == 0 ? b.x : (sortAxis == 1 ? b.y : b.z);
                             if (av != bv) return av < bv;
                             return nodes_[left].user < nodes_[right].user;
                         });
        const int mid = begin + count / 2;
        const int left = buildRange(leaves, begin, mid);
        const int right = buildRange(leaves, mid, end);
        const int parent = allocateNode();
        nodes_[parent].child1 = left;
        nodes_[parent].child2 = right;
        nodes_[parent].parent = kNull;
        nodes_[parent].height = 1 + std::max(nodes_[left].height, nodes_[right].height);
        nodes_[parent].box = nodes_[left].box.merged(nodes_[right].box);
        nodes_[left].parent = parent;
        nodes_[right].parent = parent;
        return parent;
    }

    std::vector<Node> nodes_;
    std::vector<int> proxies_;      // proxy handle -> leaf node
    std::vector<int> freeProxies_;
    mutable std::vector<int> stack_;  // node-pair stack for queryPairs
    int root_ = kNull;
    int freeNode_ = kNull;
    unsigned proxyCount_ = 0;
    unsigned liveNodes_ = 0;
    unsigned insertionCount_ = 0;
    unsigned rebuilds_ = 0;
};

} // namespace physics
} // namespace engine
