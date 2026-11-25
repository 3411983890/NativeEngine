#pragma once

// Convex-convex narrowphase: GJK for the overlap test, EPA for the penetration
// depth, then face clipping for the contact manifold.
//
// Why this exists: a baked Voronoi shard is an irregular polyhedron, but until now its
// collision proxy was the axis-aligned box around it, which is on average twice the
// shard's volume (median 1.97x on the shipped wall assets, worst case 4.21x). The visible
// consequence is that a shard balances on a single corner and never tips, because the box
// it is simulated as has a wide flat base.
//
// Design notes:
//   * Shapes are described by a centre, an orthonormal basis and either half extents (box)
//     or a local point cloud plus triangle indices (hull). No dependency on the physics
//     world, so this header is testable on its own.
//   * Every loop runs in a fixed order with a fixed iteration cap and breaks ties by
//     index, so the result is bit-identical across runs. The physics layer depends on
//     that for replay.
//   * `collide` reports failure rather than approximating. The caller falls back to the
//     box path, which keeps a regression from turning into a physics explosion.

#include "vec3.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace engine::convex {

inline float dotv(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 crossv(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthv(Vec3 a) { return std::sqrt(dotv(a, a)); }
inline Vec3 normalizedv(Vec3 a) {
    const float l = lengthv(a);
    return l > 1.0e-12f ? a / l : Vec3{0.0f, 1.0f, 0.0f};
}
inline float compv(Vec3 v, int index) {
    return index == 0 ? v.x : (index == 1 ? v.y : v.z);
}
inline bool finitev(Vec3 a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

// A convex shape in world space.
struct Shape {
    Vec3 center;
    Vec3 axis[3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    Vec3 half{0.0f, 0.0f, 0.0f};
    // Hull data, local to `center` and expressed in the `axis` basis. When points is
    // non-null the box half extents are ignored.
    const Vec3* points = nullptr;
    int pointCount = 0;
    // Triangle list into `points`, used only to recover face polygons for clipping.
    const std::uint32_t* indices = nullptr;
    int indexCount = 0;

    bool hull() const { return points != nullptr && pointCount > 0; }
    Vec3 toWorld(Vec3 local) const {
        return center + axis[0] * local.x + axis[1] * local.y + axis[2] * local.z;
    }
};

inline Vec3 support(const Shape& shape, Vec3 direction) {
    if (shape.hull()) {
        // Project the world direction into the shape basis once, then scan the cloud.
        const Vec3 local{dotv(direction, shape.axis[0]), dotv(direction, shape.axis[1]),
                         dotv(direction, shape.axis[2])};
        int best = 0;
        float bestProjection = dotv(shape.points[0], local);
        for (int index = 1; index < shape.pointCount; ++index) {
            const float projection = dotv(shape.points[index], local);
            if (projection > bestProjection) {
                bestProjection = projection;
                best = index;
            }
        }
        return shape.toWorld(shape.points[best]);
    }
    Vec3 result = shape.center;
    for (int axis = 0; axis < 3; ++axis) {
        const float sign = dotv(direction, shape.axis[axis]) >= 0.0f ? 1.0f : -1.0f;
        result += shape.axis[axis] * (compv(shape.half, axis) * sign);
    }
    return result;
}

// Three outcomes so the caller can tell "these shapes are apart" from "the algorithm gave
// up". Only the latter may fall back to another narrowphase: treating a failed test as a
// separation would silently drop collisions, and treating it as contact would invent them.
enum class Result {
    Disjoint,
    Overlapping,
    Failed,
};

struct Manifold {
    Vec3 normal{0.0f, 1.0f, 0.0f};  // from A toward B
    float depth = 0.0f;
    int count = 0;
    Vec3 points[4];
    float penetrations[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

namespace detail {

// A simplex vertex carries the Minkowski point plus the witness point on each shape, so
// the manifold can be placed in world space without a second pass.
struct Support {
    Vec3 minkowski;
    Vec3 onA;
    Vec3 onB;
};

inline Support minkowskiSupport(const Shape& a, const Shape& b, Vec3 direction) {
    Support result;
    result.onA = support(a, direction);
    result.onB = support(b, direction * -1.0f);
    result.minkowski = result.onA - result.onB;
    return result;
}

constexpr int MaxGjkIterations = 32;
constexpr int MaxEpaIterations = 48;
constexpr int MaxEpaFaces = 256;

struct Simplex {
    Support vertex[4];
    int count = 0;
};

// Closest point to the origin on a triangle, returning barycentric weights. Used by the
// tetrahedron reduction; returns false when the triangle is degenerate.
inline bool closestOnTriangle(const Vec3& a, const Vec3& b, const Vec3& c, Vec3& out,
                              float& u, float& v, float& w) {
    const Vec3 ab = b - a, ac = c - a, ap = a * -1.0f;
    const float d1 = dotv(ab, ap), d2 = dotv(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        out = a; u = 1.0f; v = 0.0f; w = 0.0f;
        return true;
    }
    const Vec3 bp = b * -1.0f;
    const float d3 = dotv(ab, bp), d4 = dotv(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        out = b; u = 0.0f; v = 1.0f; w = 0.0f;
        return true;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float t = d1 / (d1 - d3);
        out = a + ab * t; u = 1.0f - t; v = t; w = 0.0f;
        return true;
    }
    const Vec3 cp = c * -1.0f;
    const float d5 = dotv(ab, cp), d6 = dotv(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        out = c; u = 0.0f; v = 0.0f; w = 1.0f;
        return true;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float t = d2 / (d2 - d6);
        out = a + ac * t; u = 1.0f - t; v = 0.0f; w = t;
        return true;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        out = b + (c - b) * t; u = 0.0f; v = 1.0f - t; w = t;
        return true;
    }
    const float denominator = va + vb + vc;
    if (std::abs(denominator) < 1.0e-12f) return false;
    const float inverse = 1.0f / denominator;
    v = vb * inverse;
    w = vc * inverse;
    u = 1.0f - v - w;
    out = a * u + b * v + c * w;
    return true;
}

// Reduces the simplex to the smallest subset whose hull contains the closest point to the
// origin, and returns that point. This is the "sub-distance" step of GJK.
inline Vec3 reduceSimplex(Simplex& simplex) {
    if (simplex.count == 1) return simplex.vertex[0].minkowski;
    if (simplex.count == 2) {
        const Vec3 a = simplex.vertex[0].minkowski;
        const Vec3 b = simplex.vertex[1].minkowski;
        const Vec3 ab = b - a;
        const float t = dotv(ab, a * -1.0f) / std::max(dotv(ab, ab), 1.0e-12f);
        if (t <= 0.0f) {
            simplex.count = 1;
            return a;
        }
        if (t >= 1.0f) {
            simplex.vertex[0] = simplex.vertex[1];
            simplex.count = 1;
            return b;
        }
        return a + ab * t;
    }
    if (simplex.count == 3) {
        Vec3 closest;
        float u = 0.0f, v = 0.0f, w = 0.0f;
        if (!closestOnTriangle(simplex.vertex[0].minkowski, simplex.vertex[1].minkowski,
                               simplex.vertex[2].minkowski, closest, u, v, w)) {
            simplex.count = 2;
            return reduceSimplex(simplex);
        }
        const Support p0 = simplex.vertex[0], p1 = simplex.vertex[1], p2 = simplex.vertex[2];
        int kept = 0;
        if (u > 0.0f) simplex.vertex[kept++] = p0;
        if (v > 0.0f) simplex.vertex[kept++] = p1;
        if (w > 0.0f) simplex.vertex[kept++] = p2;
        simplex.count = kept > 0 ? kept : 1;
        if (simplex.count == 1) simplex.vertex[0] = p0;
        return closest;
    }
    // Tetrahedron: the origin is inside when no face has it on the outward side. Otherwise
    // the closest feature lies on the face that the origin is outside of, so reduce to that
    // face and recurse. Picking "the face with the smallest plane distance" instead would
    // stall: the simplex would oscillate between three and four points forever.
    static const int faceVertex[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    static const int opposite[4] = {3, 2, 1, 0};
    for (int face = 0; face < 4; ++face) {
        const Vec3 p0 = simplex.vertex[faceVertex[face][0]].minkowski;
        const Vec3 p1 = simplex.vertex[faceVertex[face][1]].minkowski;
        const Vec3 p2 = simplex.vertex[faceVertex[face][2]].minkowski;
        const Vec3 apex = simplex.vertex[opposite[face]].minkowski;
        Vec3 normal = crossv(p1 - p0, p2 - p0);
        if (lengthv(normal) < 1.0e-12f) continue;
        // Orient the normal away from the fourth vertex.
        if (dotv(normal, apex - p0) > 0.0f) normal = normal * -1.0f;
        if (dotv(normal, p0 * -1.0f) <= 0.0f) continue;  // origin on the inner side
        const Support kept[3] = {simplex.vertex[faceVertex[face][0]],
                                 simplex.vertex[faceVertex[face][1]],
                                 simplex.vertex[faceVertex[face][2]]};
        simplex.vertex[0] = kept[0];
        simplex.vertex[1] = kept[1];
        simplex.vertex[2] = kept[2];
        simplex.count = 3;
        return reduceSimplex(simplex);
    }
    // Origin enclosed by all four faces.
    return Vec3{0.0f, 0.0f, 0.0f};
}

inline bool samePoint(Vec3 a, Vec3 b) { return dotv(a - b, a - b) < 1.0e-14f; }

// Grows a lower-dimensional simplex into a tetrahedron so EPA has something to expand.
inline bool expandToTetrahedron(const Shape& a, const Shape& b, Simplex& simplex) {
    if (simplex.count >= 4) return true;
    // Candidate directions: the basis axes plus the simplex edges' perpendiculars. The
    // first one that yields a point not already in the simplex is added.
    Vec3 directions[6]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                       {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
    for (int pass = 0; pass < 4 && simplex.count < 4; ++pass) {
        bool added = false;
        for (int d = 0; d < 6 && !added; ++d) {
            Vec3 direction = directions[d];
            if (simplex.count >= 2) {
                // Bias away from the existing edge so the new point is not collinear.
                const Vec3 edge = simplex.vertex[1].minkowski - simplex.vertex[0].minkowski;
                direction = direction + normalizedv(edge) * 0.5f;
            }
            const Support candidate = minkowskiSupport(a, b, direction);
            bool duplicate = false;
            for (int index = 0; index < simplex.count; ++index) {
                if (samePoint(candidate.minkowski, simplex.vertex[index].minkowski)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            simplex.vertex[simplex.count++] = candidate;
            added = true;
        }
        if (!added) break;
    }
    return simplex.count == 4;
}

struct EpaFace {
    int index[3];
    Vec3 normal;
    float distance;
};

inline bool epa(const Shape& a, const Shape& b, const Simplex& start, Vec3& normal,
                float& depth, Vec3& pointA, Vec3& pointB) {
    Support vertices[MaxEpaFaces];
    int vertexCount = 0;
    for (int index = 0; index < start.count && index < 4; ++index) {
        vertices[vertexCount++] = start.vertex[index];
    }
    if (vertexCount < 4) return false;

    EpaFace faces[MaxEpaFaces];
    int faceCount = 0;
    auto addFace = [&](int i0, int i1, int i2) -> bool {
        if (faceCount >= MaxEpaFaces) return false;
        EpaFace face;
        face.index[0] = i0;
        face.index[1] = i1;
        face.index[2] = i2;
        Vec3 faceNormal = crossv(vertices[i1].minkowski - vertices[i0].minkowski,
                                 vertices[i2].minkowski - vertices[i0].minkowski);
        const float faceLength = lengthv(faceNormal);
        if (faceLength < 1.0e-9f) return true;  // degenerate, skip
        faceNormal = faceNormal / faceLength;
        float distance = dotv(faceNormal, vertices[i0].minkowski);
        // Face normals must point away from the origin.
        if (distance < 0.0f) {
            faceNormal = faceNormal * -1.0f;
            distance = -distance;
            std::swap(face.index[1], face.index[2]);
        }
        face.normal = faceNormal;
        face.distance = distance;
        faces[faceCount++] = face;
        return true;
    };
    addFace(0, 1, 2);
    addFace(0, 3, 1);
    addFace(0, 2, 3);
    addFace(1, 3, 2);
    if (faceCount < 4) return false;

    for (int iteration = 0; iteration < MaxEpaIterations; ++iteration) {
        // Closest face; ties go to the lowest index so the walk is reproducible.
        int best = 0;
        for (int index = 1; index < faceCount; ++index) {
            if (faces[index].distance < faces[best].distance) best = index;
        }
        const EpaFace closestFace = faces[best];
        const Support candidate = minkowskiSupport(a, b, closestFace.normal);
        const float candidateDistance = dotv(closestFace.normal, candidate.minkowski);
        if (candidateDistance - closestFace.distance < 1.0e-4f ||
            vertexCount >= MaxEpaFaces) {
            // Converged: project the closest face onto each shape.
            depth = closestFace.distance;
            normal = closestFace.normal;
            Vec3 aSum{0.0f, 0.0f, 0.0f}, bSum{0.0f, 0.0f, 0.0f};
            for (int corner = 0; corner < 3; ++corner) {
                aSum += vertices[closestFace.index[corner]].onA;
                bSum += vertices[closestFace.index[corner]].onB;
            }
            pointA = aSum / 3.0f;
            pointB = bSum / 3.0f;
            return finitev(normal) && std::isfinite(depth);
        }

        // Add the new vertex and rebuild the visible face fan.
        const int newIndex = vertexCount;
        vertices[vertexCount++] = candidate;
        EpaFace kept[MaxEpaFaces];
        int keptCount = 0;
        int horizon[MaxEpaFaces][2];
        int horizonCount = 0;
        for (int index = 0; index < faceCount; ++index) {
            const EpaFace& face = faces[index];
            const float side = dotv(face.normal, candidate.minkowski) - face.distance;
            if (side <= 1.0e-6f) {
                if (keptCount < MaxEpaFaces) kept[keptCount++] = face;
                continue;
            }
            // Visible: its edges may belong to the horizon.
            for (int edge = 0; edge < 3; ++edge) {
                const int v0 = face.index[edge];
                const int v1 = face.index[(edge + 1) % 3];
                bool shared = false;
                for (int other = 0; other < faceCount && !shared; ++other) {
                    if (other == index) continue;
                    const EpaFace& peer = faces[other];
                    const float peerSide = dotv(peer.normal, candidate.minkowski) - peer.distance;
                    if (peerSide <= 1.0e-6f) continue;
                    for (int peerEdge = 0; peerEdge < 3; ++peerEdge) {
                        if (peer.index[peerEdge] == v1 &&
                            peer.index[(peerEdge + 1) % 3] == v0) {
                            shared = true;
                            break;
                        }
                    }
                }
                if (!shared && horizonCount < MaxEpaFaces) {
                    horizon[horizonCount][0] = v0;
                    horizon[horizonCount][1] = v1;
                    ++horizonCount;
                }
            }
        }
        faceCount = keptCount;
        for (int index = 0; index < keptCount; ++index) faces[index] = kept[index];
        for (int index = 0; index < horizonCount; ++index) {
            addFace(horizon[index][0], horizon[index][1], newIndex);
        }
        if (faceCount == 0) return false;
    }
    return false;
}

// Most anti-parallel face of `shape` along `direction`, as a world-space polygon.
struct Face {
    Vec3 normal;
    Vec3 point[8];
    int count = 0;
};

inline bool findFace(const Shape& shape, Vec3 direction, Face& out) {
    if (!shape.hull() || shape.indices == nullptr || shape.indexCount < 3) {
        // Box: pick the axis with the largest projection and emit its quad.
        int bestAxis = 0;
        for (int axis = 1; axis < 3; ++axis) {
            if (std::abs(dotv(direction, shape.axis[axis])) >
                std::abs(dotv(direction, shape.axis[bestAxis]))) {
                bestAxis = axis;
            }
        }
        const float sign = dotv(direction, shape.axis[bestAxis]) >= 0.0f ? 1.0f : -1.0f;
        const int u = (bestAxis + 1) % 3;
        const int v = (bestAxis + 2) % 3;
        out.normal = shape.axis[bestAxis] * sign;
        out.count = 4;
        for (int corner = 0; corner < 4; ++corner) {
            const float su = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
            const float sv = (corner == 0 || corner == 1) ? -1.0f : 1.0f;
            out.point[corner] = shape.center + shape.axis[bestAxis] * (compv(shape.half, bestAxis) * sign) +
                                shape.axis[u] * (compv(shape.half, u) * su) +
                                shape.axis[v] * (compv(shape.half, v) * sv);
        }
        // Order the quad so its winding matches the normal.
        const Vec3 e0 = out.point[1] - out.point[0];
        const Vec3 e1 = out.point[2] - out.point[0];
        if (dotv(crossv(e0, e1), out.normal) < 0.0f) std::swap(out.point[1], out.point[3]);
        return true;
    }
    float bestAlignment = -2.0f;
    int bestTriangle = -1;
    for (int triangle = 0; triangle + 2 < shape.indexCount; triangle += 3) {
        const Vec3 p0 = shape.points[shape.indices[triangle]];
        const Vec3 p1 = shape.points[shape.indices[triangle + 1]];
        const Vec3 p2 = shape.points[shape.indices[triangle + 2]];
        Vec3 normal = crossv(p1 - p0, p2 - p0);
        const float normalLength = lengthv(normal);
        if (normalLength < 1.0e-9f) continue;
        normal = normal / normalLength;
        const float alignment = dotv(normal, direction);
        if (alignment > bestAlignment) {
            bestAlignment = alignment;
            bestTriangle = triangle;
        }
    }
    if (bestTriangle < 0) return false;
    out.normal = normalizedv(crossv(shape.points[shape.indices[bestTriangle + 1]] -
                                        shape.points[shape.indices[bestTriangle]],
                                    shape.points[shape.indices[bestTriangle + 2]] -
                                        shape.points[shape.indices[bestTriangle]]));
    out.count = 3;
    for (int corner = 0; corner < 3; ++corner) {
        out.point[corner] = shape.toWorld(shape.points[shape.indices[bestTriangle + corner]]);
    }
    return true;
}

// Sutherland-Hodgman clip of `polygon` against the half-space dot(normal, p) <= offset.
inline int clipAgainstPlane(const Vec3* polygon, int count, Vec3 normal, float offset,
                            Vec3* out) {
    int outCount = 0;
    for (int index = 0; index < count; ++index) {
        const Vec3 current = polygon[index];
        const Vec3 next = polygon[(index + 1) % count];
        const float currentDistance = dotv(normal, current) - offset;
        const float nextDistance = dotv(normal, next) - offset;
        if (currentDistance <= 0.0f) {
            if (outCount < 8) out[outCount++] = current;
        }
        if ((currentDistance < 0.0f && nextDistance > 0.0f) ||
            (currentDistance > 0.0f && nextDistance < 0.0f)) {
            const float t = currentDistance / (currentDistance - nextDistance);
            if (outCount < 8) out[outCount++] = current + (next - current) * t;
        }
    }
    return outCount;
}

// Builds the contact manifold. The normal comes from EPA; the reference face is the one
// more perpendicular to it, and the incident face is clipped against the reference side
// planes. Points that end up in front of the reference plane are dropped.
inline bool buildManifold(const Shape& a, const Shape& b, Vec3 normal, float depth,
                          const Vec3& contactA, const Vec3& contactB, Manifold& out) {
    Face faceA, faceB;
    const bool haveA = findFace(a, normal, faceA);
    const bool haveB = findFace(b, normal * -1.0f, faceB);
    if (!haveA || !haveB) {
        // Fall back to the EPA witness points as a single-point contact.
        out.normal = normal;
        out.depth = depth;
        out.count = 1;
        out.points[0] = (contactA + contactB) * 0.5f;
        out.penetrations[0] = depth;
        return true;
    }
    const bool useA = std::abs(dotv(faceA.normal, normal)) >= std::abs(dotv(faceB.normal, normal * -1.0f));
    const Face& reference = useA ? faceA : faceB;
    const Face& incident = useA ? faceB : faceA;
    const Vec3 referenceNormal = useA ? normal : normal * -1.0f;

    // Clip the incident polygon against the reference face's side planes.
    Vec3 polygon[8];
    int count = incident.count;
    for (int index = 0; index < count; ++index) polygon[index] = incident.point[index];
    for (int edge = 0; edge < reference.count && count > 0; ++edge) {
        const Vec3 p0 = reference.point[edge];
        const Vec3 p1 = reference.point[(edge + 1) % reference.count];
        Vec3 sideNormal = crossv(referenceNormal, p1 - p0);
        const float sideLength = lengthv(sideNormal);
        if (sideLength < 1.0e-9f) continue;
        sideNormal = sideNormal / sideLength;
        // Orient the side plane inward.
        const Vec3 toCentre = reference.point[0] - p0;
        if (dotv(sideNormal, toCentre) < 0.0f) sideNormal = sideNormal * -1.0f;
        Vec3 clipped[8];
        count = clipAgainstPlane(polygon, count, sideNormal * -1.0f,
                                 -dotv(sideNormal, p0), clipped);
        for (int index = 0; index < count; ++index) polygon[index] = clipped[index];
    }
    if (count == 0) {
        out.normal = normal;
        out.depth = depth;
        out.count = 1;
        out.points[0] = (contactA + contactB) * 0.5f;
        out.penetrations[0] = depth;
        return true;
    }

    // Keep the deepest points, then the most spread out, up to four.
    struct Candidate { Vec3 point; float penetration; };
    Candidate candidates[8];
    int candidateCount = 0;
    for (int index = 0; index < count && candidateCount < 8; ++index) {
        const float separation = dotv(referenceNormal, polygon[index] - reference.point[0]);
        if (separation > 1.0e-3f) continue;
        // Project onto the reference plane so the contact point sits on the surface of the
        // reference body rather than floating on the incident face.
        candidates[candidateCount].point = polygon[index] - referenceNormal * separation;
        candidates[candidateCount].penetration = -separation;
        ++candidateCount;
    }
    if (candidateCount == 0) {
        out.normal = normal;
        out.depth = depth;
        out.count = 1;
        out.points[0] = (contactA + contactB) * 0.5f;
        out.penetrations[0] = depth;
        return true;
    }

    int order[8];
    for (int index = 0; index < candidateCount; ++index) order[index] = index;
    for (int index = 1; index < candidateCount; ++index) {
        const int key = order[index];
        int scan = index - 1;
        while (scan >= 0 && candidates[order[scan]].penetration < candidates[key].penetration) {
            order[scan + 1] = order[scan];
            --scan;
        }
        order[scan + 1] = key;
    }
    int chosen[4];
    int chosenCount = 0;
    chosen[chosenCount++] = order[0];
    if (candidateCount > 1) {
        // Farthest from the first point.
        int farthest = -1;
        float bestDistance = -1.0f;
        for (int index = 0; index < candidateCount; ++index) {
            const float distance = lengthv(candidates[index].point - candidates[chosen[0]].point);
            if (distance > bestDistance) {
                bestDistance = distance;
                farthest = index;
            }
        }
        if (farthest >= 0 && bestDistance > 1.0e-4f) chosen[chosenCount++] = farthest;
    }
    if (candidateCount > 2 && chosenCount == 2) {
        // Largest triangle area with the first two.
        int bestThird = -1;
        float bestArea = -1.0f;
        for (int index = 0; index < candidateCount; ++index) {
            const float area = lengthv(crossv(candidates[chosen[0]].point - candidates[index].point,
                                              candidates[chosen[1]].point - candidates[index].point));
            if (area > bestArea) {
                bestArea = area;
                bestThird = index;
            }
        }
        if (bestThird >= 0 && bestArea > 1.0e-5f) chosen[chosenCount++] = bestThird;
    }
    if (candidateCount > 3 && chosenCount == 3) {
        int bestFourth = -1;
        float bestDistance = -1.0f;
        for (int index = 0; index < candidateCount; ++index) {
            float distance = 1.0e30f;
            for (int corner = 0; corner < chosenCount; ++corner) {
                distance = std::min(distance, lengthv(candidates[index].point -
                                                      candidates[chosen[corner]].point));
            }
            if (distance > bestDistance) {
                bestDistance = distance;
                bestFourth = index;
            }
        }
        if (bestFourth >= 0 && bestDistance > 1.0e-4f) chosen[chosenCount++] = bestFourth;
    }

    out.normal = normal;
    out.depth = depth;
    out.count = chosenCount;
    for (int index = 0; index < chosenCount; ++index) {
        out.points[index] = candidates[chosen[index]].point;
        out.penetrations[index] = candidates[chosen[index]].penetration;
    }
    return true;
}

} // namespace detail

// Full convex-convex test. Returns false when the shapes are disjoint or when the
// algorithm fails to converge, which the caller treats as "fall back to the box path".
inline Result collide(const Shape& a, const Shape& b, Manifold& out) {
    if (!finitev(a.center) || !finitev(b.center)) return Result::Failed;
    detail::Simplex simplex;
    simplex.vertex[0] = detail::minkowskiSupport(a, b, Vec3{1.0f, 0.0f, 0.0f});
    simplex.count = 1;

    Vec3 direction = simplex.vertex[0].minkowski * -1.0f;
    if (lengthv(direction) < 1.0e-9f) direction = Vec3{0.0f, 1.0f, 0.0f};

    bool overlapping = false;
    for (int iteration = 0; iteration < detail::MaxGjkIterations; ++iteration) {
        const detail::Support candidate = detail::minkowskiSupport(a, b, direction);
        if (dotv(candidate.minkowski, direction) < 0.0f) return Result::Disjoint;
        bool duplicate = false;
        for (int index = 0; index < simplex.count; ++index) {
            if (detail::samePoint(candidate.minkowski, simplex.vertex[index].minkowski)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            overlapping = true;
            break;
        }
        if (simplex.count >= 4) break;
        simplex.vertex[simplex.count++] = candidate;
        const Vec3 closest = detail::reduceSimplex(simplex);
        const float distanceSquared = dotv(closest, closest);
        if (distanceSquared < 1.0e-12f) {
            overlapping = true;
            break;
        }
        direction = closest * -1.0f;
        if (!finitev(direction)) return Result::Failed;
    }
    if (!overlapping && simplex.count < 4) return Result::Disjoint;
    if (!detail::expandToTetrahedron(a, b, simplex)) return Result::Failed;

    Vec3 normal;
    float depth = 0.0f;
    Vec3 contactA, contactB;
    if (!detail::epa(a, b, simplex, normal, depth, contactA, contactB)) return Result::Failed;
    if (!finitev(normal) || !std::isfinite(depth) || depth <= 0.0f) return Result::Failed;
    // EPA's normal already points from A toward B: the Minkowski difference A-B is
    // shifted towards -B, so its closest boundary to the origin lies on the B side.
    normal = normalizedv(normal);
    if (!detail::buildManifold(a, b, normal, depth, contactA, contactB, out)) {
        return Result::Failed;
    }
    return Result::Overlapping;
}

} // namespace engine::convex
