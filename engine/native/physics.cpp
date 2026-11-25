#include "physics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace engine {
namespace physics {
namespace {

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
float absf(float v) { return std::abs(v); }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(Vec3 v, Vec3 fallback = {0, 1, 0}) {
    const float l2 = dot(v, v);
    if (!std::isfinite(l2) || l2 < 1.0e-12f) return fallback;
    return v / std::sqrt(l2);
}

// Logarithmic map of the relative rotation to * from, as an axis-angle vector.
// Used to rebuild angular velocity after an XPBD position projection.
Vec3 rotationDelta(Quat from, Quat to) {
    const Quat inverse{from.w, -from.x, -from.y, -from.z};
    Quat delta = multiply(to, inverse);
    if (delta.w < 0.0f) delta = {-delta.w, -delta.x, -delta.y, -delta.z};
    const float sinHalf =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (sinHalf < 1.0e-6f) return {2.0f * delta.x, 2.0f * delta.y, 2.0f * delta.z};
    const float angle = 2.0f * std::atan2(sinHalf, delta.w);
    return {delta.x * (angle / sinHalf), delta.y * (angle / sinHalf),
            delta.z * (angle / sinHalf)};
}

struct Obb {
    Vec3 center;
    Vec3 axis[3];
    Vec3 half;
};

Obb makeObb(const Body& body) {
    Obb result;
    result.center = body.position;
    result.half = body.half;
    const Mat3 rotation = toMatrix(body.orientation);
    for (int i = 0; i < 3; ++i) {
        result.axis[i] = {rotation.m[i * 3], rotation.m[i * 3 + 1],
                          rotation.m[i * 3 + 2]};
    }
    return result;
}

Vec3 support(const Obb& box, Vec3 direction) {
    Vec3 result = box.center;
    for (int i = 0; i < 3; ++i) {
        const float sign = dot(direction, box.axis[i]) >= 0.0f ? 1.0f : -1.0f;
        const float extent = i == 0 ? box.half.x : (i == 1 ? box.half.y : box.half.z);
        result += box.axis[i] * (sign * extent);
    }
    return result;
}

float radiusOnAxis(const Obb& box, Vec3 axis) {
    return absf(dot(box.axis[0], axis)) * box.half.x +
           absf(dot(box.axis[1], axis)) * box.half.y +
           absf(dot(box.axis[2], axis)) * box.half.z;
}

struct AxisTest {
    bool valid = false;
    bool separated = false;
    float overlap = std::numeric_limits<float>::max();
    Vec3 axis{1, 0, 0};
};

AxisTest testAxis(const Obb& a, const Obb& b, Vec3 axis) {
    AxisTest result;
    const float lengthSquared = dot(axis, axis);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-10f) {
        return result;
    }
    result.valid = true;
    axis = axis / std::sqrt(lengthSquared);
    const float ra = radiusOnAxis(a, axis);
    const float rb = radiusOnAxis(b, axis);
    const float distance = dot(b.center - a.center, axis);
    const float overlap = ra + rb - absf(distance);
    if (overlap < -1.0e-4f) {
        result.separated = true;
        return result;
    }
    // Keep the normal pointing from A to B.
    result.axis = distance < 0.0f ? axis * -1.0f : axis;
    result.overlap = overlap;
    return result;
}

bool aabbOverlap(const Obb& a, const Obb& b) {
    const Vec3 delta = b.center - a.center;
    for (int worldAxis = 0; worldAxis < 3; ++worldAxis) {
        float ea = 0.0f, eb = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float av = i == 0 ? a.half.x : (i == 1 ? a.half.y : a.half.z);
            const float bv = i == 0 ? b.half.x : (i == 1 ? b.half.y : b.half.z);
            ea += absf(worldAxis == 0 ? a.axis[i].x
                                      : (worldAxis == 1 ? a.axis[i].y : a.axis[i].z)) * av;
            eb += absf(worldAxis == 0 ? b.axis[i].x
                                      : (worldAxis == 1 ? b.axis[i].y : b.axis[i].z)) * bv;
        }
        const float d = worldAxis == 0 ? delta.x : (worldAxis == 1 ? delta.y : delta.z);
        if (absf(d) > ea + eb + 0.002f) return false;
    }
    return true;
}

struct PolyVertex {
    Vec3 point;
};

std::vector<PolyVertex> clipPolygon(const std::vector<PolyVertex>& input,
                                    Vec3 planeNormal, float planeOffset) {
    std::vector<PolyVertex> output;
    if (input.empty()) return output;
    output.reserve(input.size() + 1);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const PolyVertex& a = input[i];
        const PolyVertex& b = input[(i + 1) % input.size()];
        const float da = dot(planeNormal, a.point) - planeOffset;
        const float db = dot(planeNormal, b.point) - planeOffset;
        if (da <= 0.0f) output.push_back(a);
        if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
            const float t = da / (da - db);
            output.push_back({a.point + (b.point - a.point) * t});
        }
    }
    return output;
}

std::vector<ContactPoint> faceContacts(const Obb& reference, const Obb& incident,
                                      int referenceAxis, Vec3 refOutward,
                                      Vec3 solverNormal) {
    std::vector<ContactPoint> result;
    const float signReference = dot(reference.axis[referenceAxis], refOutward) >= 0.0f
                                    ? 1.0f
                                    : -1.0f;
    const float halfReference = referenceAxis == 0
                                    ? reference.half.x
                                    : (referenceAxis == 1 ? reference.half.y
                                                          : reference.half.z);
    const Vec3 faceCenter =
        reference.center + reference.axis[referenceAxis] * (signReference * halfReference);

    int incidentAxis = 0;
    float bestAlignment = -1.0f;
    for (int i = 0; i < 3; ++i) {
        const float alignment = absf(dot(incident.axis[i], refOutward));
        if (alignment > bestAlignment) {
            bestAlignment = alignment;
            incidentAxis = i;
        }
    }
    const float signIncident = dot(incident.axis[incidentAxis], refOutward) > 0.0f
                                   ? -1.0f
                                   : 1.0f;
    const float halfIncident = incidentAxis == 0
                                   ? incident.half.x
                                   : (incidentAxis == 1 ? incident.half.y
                                                        : incident.half.z);
    const int u = (incidentAxis + 1) % 3;
    const int v = (incidentAxis + 2) % 3;
    const float hu = u == 0 ? incident.half.x : (u == 1 ? incident.half.y : incident.half.z);
    const float hv = v == 0 ? incident.half.x : (v == 1 ? incident.half.y : incident.half.z);
    const Vec3 faceCenterIncident =
        incident.center + incident.axis[incidentAxis] * (signIncident * halfIncident);

    std::vector<PolyVertex> polygon = {
        {faceCenterIncident + incident.axis[u] * hu + incident.axis[v] * hv},
        {faceCenterIncident - incident.axis[u] * hu + incident.axis[v] * hv},
        {faceCenterIncident - incident.axis[u] * hu - incident.axis[v] * hv},
        {faceCenterIncident + incident.axis[u] * hu - incident.axis[v] * hv},
    };
    for (int side = 0; side < 3; ++side) {
        if (side == referenceAxis) continue;
        const float halfSide = side == 0 ? reference.half.x
                                        : (side == 1 ? reference.half.y : reference.half.z);
        const Vec3 axis = reference.axis[side];
        polygon = clipPolygon(polygon, axis, dot(axis, faceCenter) + halfSide);
        if (polygon.empty()) break;
        polygon = clipPolygon(polygon, axis * -1.0f, -dot(axis, faceCenter) + halfSide);
        if (polygon.empty()) break;
    }
    if (polygon.empty()) {
        // Degenerate clipping (extremely thin boxes or numerical grazing). Fall back
        // to the midpoint between the two support points; the contact is still stable
        // enough for the solver and avoids dropping a real collision.
        const Vec3 pa = support(reference, solverNormal);
        const Vec3 pb = support(incident, solverNormal * -1.0f);
        result.push_back({(pa + pb) * 0.5f,
                          std::max(0.0f, dot(pb - pa, solverNormal))});
        return result;
    }
    for (const PolyVertex& vertex : polygon) {
        const float separation = dot(refOutward, vertex.point - faceCenter);
        if (separation > 0.012f) continue;
        result.push_back({vertex.point - refOutward * (separation * 0.5f),
                          std::max(0.0f, -separation)});
        if (result.size() >= 4) break;
    }
    if (result.empty()) {
        const Vec3 pa = support(reference, solverNormal);
        const Vec3 pb = support(incident, solverNormal * -1.0f);
        result.push_back({(pa + pb) * 0.5f,
                          std::max(0.0f, dot(pb - pa, solverNormal))});
    }
    return result;
}

void buildTangentBasis(Vec3 normal, Vec3& t1, Vec3& t2) {
    const Vec3 reference = absf(normal.y) < 0.92f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    t1 = normalized(cross(normal, reference));
    t2 = normalized(cross(normal, t1));
}

} // namespace

// --------------------------------------------------------------------- quaternion

Quat normalize(Quat q) {
    const float lengthSquared = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f) return {};
    const float inv = 1.0f / std::sqrt(lengthSquared);
    return {q.w * inv, q.x * inv, q.y * inv, q.z * inv};
}

Quat multiply(Quat a, Quat b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Quat fromAxisAngle(Vec3 axis, float radians) {
    axis = normalized(axis);
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return {std::cos(half), axis.x * s, axis.y * s, axis.z * s};
}

Quat fromEuler(float yaw, float pitch, float roll) {
    const Quat qy = fromAxisAngle({0, 1, 0}, yaw);
    const Quat qx = fromAxisAngle({1, 0, 0}, pitch);
    const Quat qz = fromAxisAngle({0, 0, 1}, roll);
    return normalize(multiply(multiply(qy, qx), qz));
}

Vec3 rotate(Quat q, Vec3 v) {
    // v' = v + 2 * qv x (qv x v + w*v)
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t = cross(qv, v) + v * q.w;
    return v + cross(qv, t) * 2.0f;
}

Mat3 toMatrix(Quat q) {
    q = normalize(q);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat3 result;
    result.m[0] = 1.0f - 2.0f * (yy + zz);
    result.m[1] = 2.0f * (xy + wz);
    result.m[2] = 2.0f * (xz - wy);
    result.m[3] = 2.0f * (xy - wz);
    result.m[4] = 1.0f - 2.0f * (xx + zz);
    result.m[5] = 2.0f * (yz + wx);
    result.m[6] = 2.0f * (xz + wy);
    result.m[7] = 2.0f * (yz - wx);
    result.m[8] = 1.0f - 2.0f * (xx + yy);
    return result;
}

Mat3 transpose(Mat3 a) {
    Mat3 result;
    result.m[0] = a.m[0];
    result.m[1] = a.m[3];
    result.m[2] = a.m[6];
    result.m[3] = a.m[1];
    result.m[4] = a.m[4];
    result.m[5] = a.m[7];
    result.m[6] = a.m[2];
    result.m[7] = a.m[5];
    result.m[8] = a.m[8];
    return result;
}

Vec3 transform(Mat3 a, Vec3 v) {
    return {a.m[0] * v.x + a.m[3] * v.y + a.m[6] * v.z,
            a.m[1] * v.x + a.m[4] * v.y + a.m[7] * v.z,
            a.m[2] * v.x + a.m[5] * v.y + a.m[8] * v.z};
}

Mat3 multiply(Mat3 a, Mat3 b) {
    Mat3 result;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a.m[k * 3 + row] * b.m[column * 3 + k];
            }
            result.m[column * 3 + row] = sum;
        }
    }
    return result;
}

void toEuler(Quat q, float& yaw, float& pitch, float& roll) {
    const Mat3 r = toMatrix(q);
    // Column-major elements: r.m[col * 3 + row]. For R = Ry * Rx * Rz:
    // R[0][2] = sin(yaw)cos(pitch), R[1][2] = -sin(pitch),
    // R[1][0] = cos(pitch)sin(roll), R[1][1] = cos(pitch)cos(roll).
    const float r02 = r.m[6];
    const float r12 = r.m[7];
    const float r22 = r.m[8];
    const float r10 = r.m[1];
    const float r11 = r.m[4];
    const float r20 = r.m[2];
    const float r00 = r.m[0];
    const float sinPitch = clampf(-r12, -1.0f, 1.0f);
    pitch = std::asin(sinPitch);
    if (std::abs(r12) < 0.9995f) {
        yaw = std::atan2(r02, r22);
        roll = std::atan2(r10, r11);
    } else {
        // Gimbal lock: fold roll into yaw.
        yaw = std::atan2(-r20, r00);
        roll = 0.0f;
    }
}

// -------------------------------------------------------------------------- body

namespace {
Mat3 inertia(const Body& body) {
    const float x = body.half.x * 2.0f;
    const float y = body.half.y * 2.0f;
    const float z = body.half.z * 2.0f;
    const float m = body.mass;
    const float ix = m / 12.0f * (y * y + z * z);
    const float iy = m / 12.0f * (x * x + z * z);
    const float iz = m / 12.0f * (x * x + y * y);
    Mat3 result;
    result.m[0] = ix > 1.0e-9f ? 1.0f / ix : 0.0f;
    result.m[4] = iy > 1.0e-9f ? 1.0f / iy : 0.0f;
    result.m[8] = iz > 1.0e-9f ? 1.0f / iz : 0.0f;
    return result;
}

void refreshInertia(Body& body) {
    if (!body.dynamic) {
        body.invInertiaWorld = {};
        return;
    }
    const Mat3 rotation = toMatrix(body.orientation);
    body.invInertiaWorld =
        multiply(multiply(rotation, body.invInertiaLocal), transpose(rotation));
}
} // namespace

Vec3 worldAxis(const Body& body, int axis) {
    if (axis < 0 || axis > 2) return {1, 0, 0};
    const Mat3 matrix = toMatrix(body.orientation);
    return {matrix.m[axis * 3], matrix.m[axis * 3 + 1], matrix.m[axis * 3 + 2]};
}

float volume(const Body& body) {
    return 8.0f * body.half.x * body.half.y * body.half.z;
}

float kineticEnergy(const Body& body) {
    if (!body.dynamic) return 0.0f;
    return 0.5f * body.mass * dot(body.velocity, body.velocity);
}

bool intersects(const Body& a, const Body& b) {
    const Obb oa = makeObb(a);
    const Obb ob = makeObb(b);
    if (!aabbOverlap(oa, ob)) return false;
    for (int i = 0; i < 3; ++i) {
        if (testAxis(oa, ob, oa.axis[i]).separated) return false;
        if (testAxis(oa, ob, ob.axis[i]).separated) return false;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const AxisTest test = testAxis(oa, ob, cross(oa.axis[i], ob.axis[j]));
            if (test.valid && test.separated) return false;
        }
    }
    return true;
}

Aabb worldBounds(const Body& body) {
    const Obb box = makeObb(body);
    Aabb result;
    result.min = body.position;
    result.max = body.position;
    for (int i = 0; i < 3; ++i) {
        const Vec3 axis = box.axis[i];
        const float extent = i == 0 ? body.half.x : (i == 1 ? body.half.y : body.half.z);
        const Vec3 spread{absf(axis.x) * extent, absf(axis.y) * extent,
                          absf(axis.z) * extent};
        result.min -= spread;
        result.max += spread;
    }
    return result;
}

// ------------------------------------------------------------------------- world

void World::clear() {
    bodies_.clear();
    contacts_.clear();
    frameImpacts_.clear();
    broadphase_.clear();
    bodyProxy_.clear();
    candidates_.clear();
    joints_.clear();
    jointPrev_.clear();
    noCollidePairs_.clear();
    broadphasePairs_ = 0;
    broadphaseMode_ = 0;
    nextJointId_ = 1;
    nextId_ = 1;
    lastContactCount_ = 0;
    substepsLast_ = 0;
    totalSteps_ = 0;
    lastStepMs_ = totalStepMs_ = maxStepMs_ = 0.0;
    timedSteps_ = 0;
}

void World::reserve(std::size_t count, std::size_t contactCapacity) {
    bodies_.reserve(count);
    bodyProxy_.reserve(count);
    contacts_.reserve(std::max<std::size_t>(contactCapacity, 64));
    candidates_.reserve(std::max<std::size_t>(contactCapacity, 64));
    frameImpacts_.reserve(64);
}

unsigned World::addBody(const BodyDesc& desc) {
    Body body;
    body.id = nextId_++;
    body.entity = desc.entity;
    body.dynamic = desc.dynamic && desc.mass > 0.0f;
    body.debris = desc.debris;
    body.projectile = desc.projectile;
    body.visible = desc.visible;
    body.hullPoints = desc.hullPoints;
    body.hullPointCount = desc.hullPointCount;
    body.hullIndices = desc.hullIndices;
    body.hullIndexCount = desc.hullIndexCount;
    body.heightfield = desc.heightfield;
    body.half = {std::max(0.001f, absf(desc.halfExtents.x)),
                 std::max(0.001f, absf(desc.halfExtents.y)),
                 std::max(0.001f, absf(desc.halfExtents.z))};
    body.position = desc.position;
    body.orientation = normalize(desc.orientation);
    body.velocity = desc.velocity;
    body.angularVelocity = desc.angularVelocity;
    body.mass = std::max(0.001f, desc.mass);
    body.invMass = body.dynamic ? 1.0f / body.mass : 0.0f;
    body.friction = clampf(desc.friction, 0.0f, 2.0f);
    body.restitution = clampf(desc.restitution, 0.0f, 1.0f);
    body.linearDamping = clampf(desc.linearDamping, 0.0f, 4.0f);
    body.angularDamping = clampf(desc.angularDamping, 0.0f, 6.0f);
    body.lifetime = std::max(0.0f, desc.lifetime);
    for (int i = 0; i < 3; ++i) body.color[i] = desc.color[i];
    body.material = desc.material;
    body.texture = desc.texture;
    body.roughness = desc.roughness;
    body.metalness = desc.metalness;
    body.invInertiaLocal = inertia(body);
    refreshInertia(body);
    bodies_.push_back(body);
    bodyProxy_.push_back(-1);
    return static_cast<unsigned>(bodies_.size() - 1);
}

void World::deactivate(unsigned index) {
    if (index < bodies_.size()) bodies_[index].active = false;
}

void World::wake(unsigned index) {
    if (index >= bodies_.size()) return;
    Body& body = bodies_[index];
    if (!body.active || !body.dynamic) return;
    body.sleeping = false;
    body.sleepTimer = 0.0f;
}

void World::wakeAll() {
    for (Body& body : bodies_) {
        if (body.active && body.dynamic) {
            body.sleeping = false;
            body.sleepTimer = 0.0f;
        }
    }
}

Body* World::body(unsigned index) {
    return index < bodies_.size() ? &bodies_[index] : nullptr;
}

const Body* World::body(unsigned index) const {
    return index < bodies_.size() ? &bodies_[index] : nullptr;
}

std::vector<Impact> World::takeImpacts() {
    std::vector<Impact> result;
    result.swap(frameImpacts_);
    return result;
}

void World::applyImpulse(Body& body, Vec3 impulse, Vec3 r) {
    if (!body.dynamic) return;
    body.velocity += impulse * body.invMass;
    body.angularVelocity += transform(body.invInertiaWorld, cross(r, impulse));
}

void World::applyRadialImpulse(Vec3 center, float radius, float deltaSpeed,
                               bool includeSleeping) {
    if (radius <= 0.0f || deltaSpeed <= 0.0f) return;
    const float radiusSquared = radius * radius;
    for (std::size_t index = 0; index < bodies_.size(); ++index) {
        Body& body = bodies_[index];
        if (!body.active || !body.dynamic) continue;
        if (body.sleeping && !includeSleeping) continue;
        const Vec3 offset = body.position - center;
        const float distanceSquared = dot(offset, offset);
        if (distanceSquared > radiusSquared) continue;
        const float distance = std::sqrt(distanceSquared);
        const float falloff = 1.0f - distance / radius;
        const Vec3 direction = distance > 1.0e-4f ? offset / distance
                                                  : Vec3{0.0f, 1.0f, 0.0f};
        wake(static_cast<unsigned>(index));
        body.velocity += direction * (deltaSpeed * falloff * falloff);
        // A deterministic tumbling component seeded by body id. This keeps repeated
        // runs identical while making a blast read as more than uniform translation.
        unsigned hash = body.id * 1664525u + 1013904223u;
        const float ax = static_cast<float>((hash >> 8) & 255) / 255.0f - 0.5f;
        const float ay = static_cast<float>((hash >> 16) & 255) / 255.0f - 0.5f;
        const float az = static_cast<float>((hash >> 24) & 255) / 255.0f - 0.5f;
        body.angularVelocity += Vec3{ax, ay, az} * (falloff * 6.0f);
    }
}

void World::addForce(unsigned index, Vec3 force) {
    if (index >= bodies_.size()) return;
    Body& body = bodies_[index];
    if (!body.active || !body.dynamic) return;
    body.externalForce += force;
}

void World::addForceAtPoint(unsigned index, Vec3 force, Vec3 point) {
    if (index >= bodies_.size()) return;
    Body& body = bodies_[index];
    if (!body.active || !body.dynamic) return;
    body.externalForce += force;
    body.externalTorque += cross(point - body.position, force);
}

void World::addTorque(unsigned index, Vec3 torque) {
    if (index >= bodies_.size()) return;
    Body& body = bodies_[index];
    if (!body.active || !body.dynamic) return;
    body.externalTorque += torque;
}

void World::wakeIfNeeded(Body& a, Body& b) {
    if (!a.dynamic || !b.dynamic) return;
    if (a.sleeping && !b.sleeping) wake(static_cast<unsigned>(&a - bodies_.data()));
    if (b.sleeping && !a.sleeping) wake(static_cast<unsigned>(&b - bodies_.data()));
}

void World::generateContacts() {
    contacts_.clear();
    updateBroadphase();
    candidates_.clear();
    const std::size_t count = bodies_.size();
    if (broadphaseMode_ == 0) {
        // Reference path: every unordered pair of active bodies, in index order.
        for (std::size_t i = 0; i < count; ++i) {
            if (!bodies_[i].active) continue;
            for (std::size_t j = i + 1; j < count; ++j) {
                if (!bodies_[j].active) continue;
                candidates_.push_back({static_cast<unsigned>(i), static_cast<unsigned>(j)});
            }
        }
    } else {
        // The tree reports a superset of the exact-AABB pairs. Sorting by body
        // index makes the contact stream identical to the reference path, which
        // is what the determinism digest and replay contract depend on.
        broadphase_.queryPairs(candidates_);
        std::sort(candidates_.begin(), candidates_.end(),
                  [](const AabbPair& left, const AabbPair& right) {
                      if (left.a != right.a) return left.a < right.a;
                      return left.b < right.b;
                  });
        candidates_.erase(std::unique(candidates_.begin(), candidates_.end(),
                                      [](const AabbPair& left, const AabbPair& right) {
                                          return left.a == right.a && left.b == right.b;
                                      }),
                          candidates_.end());
    }
    broadphasePairs_ = static_cast<unsigned>(candidates_.size());
    for (const AabbPair& pair : candidates_) {
        collidePair(pair.a, pair.b);
    }
    lastContactCount_ = static_cast<unsigned>(contacts_.size());
}

bool World::useBvh() const {
    if (settings_.broadphase == 1) return false;
    if (settings_.broadphase == 2) return true;
    unsigned active = 0;
    for (const Body& body : bodies_) {
        if (body.active) ++active;
    }
    const int threshold = std::max(8, settings_.broadphaseThreshold);
    return active >= static_cast<unsigned>(threshold);
}

void World::updateBroadphase() {
    const std::size_t count = bodies_.size();
    if (bodyProxy_.size() != count) bodyProxy_.resize(count, -1);
    broadphaseMode_ = useBvh() ? 1u : 0u;
    if (broadphaseMode_ == 0) {
        // Dropping proxies on a mode switch keeps the tree from holding stale
        // leaves when a caller flips settings at runtime.
        if (broadphase_.proxyCount() > 0) {
            for (int& proxy : bodyProxy_) {
                if (proxy < 0) continue;
                broadphase_.destroyProxy(proxy);
                proxy = -1;
            }
        }
        return;
    }
    const float margin = std::max(0.004f, settings_.broadphaseMargin);
    for (std::size_t i = 0; i < count; ++i) {
        const Body& body = bodies_[i];
        if (!body.active) {
            if (bodyProxy_[i] >= 0) {
                broadphase_.destroyProxy(bodyProxy_[i]);
                bodyProxy_[i] = -1;
            }
            continue;
        }
        const Aabb box = worldBounds(body);
        if (bodyProxy_[i] < 0) {
            bodyProxy_[i] =
                broadphase_.createProxy(box, static_cast<unsigned>(i), margin);
        } else {
            broadphase_.moveProxy(bodyProxy_[i], box, margin);
        }
    }
}

// Builds the narrowphase shape for a body. A body without a hull reports a box, so the
// convex path and the SAT path describe the same geometry for box bodies.
static bool makeConvexShape(const Body& body, convex::Shape& out) {
    const Mat3 basis = toMatrix(body.orientation);
    out.center = body.position;
    for (int axis = 0; axis < 3; ++axis) {
        out.axis[axis] = {basis.m[axis * 3 + 0], basis.m[axis * 3 + 1], basis.m[axis * 3 + 2]};
    }
    if (body.hullPoints != nullptr && body.hullPointCount > 0) {
        out.points = body.hullPoints;
        out.pointCount = body.hullPointCount;
        out.indices = body.hullIndices;
        out.indexCount = body.hullIndexCount;
        return true;
    }
    out.half = body.half;
    return true;
}

void World::collidePair(unsigned indexA, unsigned indexB) {
    if (indexA >= bodies_.size() || indexB >= bodies_.size()) return;
    Body& a = bodies_[indexA];
    Body& b = bodies_[indexB];
    if (!a.active || !b.active) return;
    if (!a.dynamic && !b.dynamic) return;
    if (a.dynamic && !b.dynamic && a.sleeping) return;
    if (b.dynamic && !a.dynamic && b.sleeping) return;
    if (a.sleeping && b.sleeping) return;
    if (!noCollidePairs_.empty() && skipConnectedPair(indexA, indexB)) return;
    const Obb oa = makeObb(a);
    const Obb ob = makeObb(b);
    if (!aabbOverlap(oa, ob)) return;

    // Heightfield contact: sample the terrain under the dynamic box footprint.
    // A box on a single-valued field is approximated by its four footprint
    // extremes plus the centre; the reported max error is the bake's error.
    if (a.heightfield != nullptr || b.heightfield != nullptr) {
        const bool terrainIsA = a.heightfield != nullptr;
        const Body& terrain = terrainIsA ? a : b;
        const Body& solid = terrainIsA ? b : a;
        if (!solid.dynamic || solid.sleeping) return;
        const Heightfield& field = *terrain.heightfield;
        if (!field.valid()) return;
        const Obb box = terrainIsA ? ob : oa;
        const float halfX = std::abs(box.axis[0].x) * box.half.x + std::abs(box.axis[1].x) * box.half.y + std::abs(box.axis[2].x) * box.half.z;
        const float halfZ = std::abs(box.axis[0].z) * box.half.x + std::abs(box.axis[1].z) * box.half.y + std::abs(box.axis[2].z) * box.half.z;
        const float halfY = std::abs(box.axis[0].y) * box.half.x + std::abs(box.axis[1].y) * box.half.y + std::abs(box.axis[2].y) * box.half.z;
        const float xs[5] = {solid.position.x, solid.position.x - halfX, solid.position.x + halfX, solid.position.x - halfX, solid.position.x + halfX};
        const float zs[5] = {solid.position.z, solid.position.z - halfZ, solid.position.z - halfZ, solid.position.z + halfZ, solid.position.z + halfZ};
        float top = -3.0e38f;
        for (int sample = 0; sample < 5; ++sample) top = std::max(top, field.sample(xs[sample], zs[sample]));
        const float bottom = solid.position.y - halfY;
        const float penetration = top - bottom;
        if (penetration <= 0.0f) return;
        wakeIfNeeded(a, b);
        std::vector<ContactPoint> points;
        points.push_back({Vec3{solid.position.x, top, solid.position.z}, penetration});
        emitContacts(terrainIsA ? indexA : indexB, terrainIsA ? indexB : indexA, points, Vec3{0.0f, 1.0f, 0.0f}, penetration);
        return;
    }

    // Convex hull path (F4c). A baked Voronoi shard carries its real hull, so it collides as
    // the shape you can see instead of the box around it. Everything else keeps the SAT
    // path, and a hull pair that the algorithm cannot resolve falls through to SAT rather
    // than dropping the contact: an invented or missing contact is worse than a coarse one.
    if (a.hullPoints != nullptr || b.hullPoints != nullptr) {
        convex::Shape shapeA, shapeB;
        if (makeConvexShape(a, shapeA) && makeConvexShape(b, shapeB)) {
            convex::Manifold manifold;
            const convex::Result result = convex::collide(shapeA, shapeB, manifold);
            if (result == convex::Result::Disjoint) return;
            if (result == convex::Result::Overlapping) {
                wakeIfNeeded(a, b);
                std::vector<ContactPoint> points;
                points.reserve(static_cast<std::size_t>(manifold.count));
                for (int index = 0; index < manifold.count; ++index) {
                    points.push_back({manifold.points[index], manifold.penetrations[index]});
                }
                emitContacts(indexA, indexB, points, manifold.normal, manifold.depth);
                return;
            }
        }
    }

    AxisTest best;
    int bestType = -1;  // 0 = A face, 1 = B face, 2 = edge
    Vec3 edgeAxis{0, 1, 0};
    bool separated = false;
    for (int axis = 0; axis < 3 && !separated; ++axis) {
        const AxisTest test = testAxis(oa, ob, oa.axis[axis]);
        if (test.separated) {
            separated = true;
            break;
        }
        if (test.valid && test.overlap < best.overlap) {
            best = test;
            bestType = 0;
        }
    }
    for (int axis = 0; axis < 3 && !separated; ++axis) {
        const AxisTest test = testAxis(oa, ob, ob.axis[axis]);
        if (test.separated) {
            separated = true;
            break;
        }
        if (test.valid && test.overlap < best.overlap) {
            best = test;
            bestType = 1;
        }
    }
    for (int ia = 0; ia < 3 && !separated; ++ia) {
        for (int ib = 0; ib < 3 && !separated; ++ib) {
            const AxisTest test = testAxis(oa, ob, cross(oa.axis[ia], ob.axis[ib]));
            if (!test.valid) continue;
            if (test.separated) {
                separated = true;
                break;
            }
            // A tiny preference for face contacts suppresses jitter from
            // near-parallel edge axes in stacks.
            if (test.overlap + 1.0e-4f < best.overlap || bestType < 0) {
                best = test;
                bestType = 2;
                edgeAxis = test.axis;
            }
        }
    }
    if (separated || bestType < 0) return;

    wakeIfNeeded(a, b);
    std::vector<ContactPoint> points;
    if (bestType == 0) {
        int axisIndex = 0;
        float bestProjection = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float projection = absf(dot(oa.axis[axis], best.axis));
            if (projection > bestProjection) {
                bestProjection = projection;
                axisIndex = axis;
            }
        }
        points = faceContacts(oa, ob, axisIndex, best.axis, best.axis);
    } else if (bestType == 1) {
        int axisIndex = 0;
        float bestProjection = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float projection = absf(dot(ob.axis[axis], best.axis));
            if (projection > bestProjection) {
                bestProjection = projection;
                axisIndex = axis;
            }
        }
        points = faceContacts(ob, oa, axisIndex, best.axis * -1.0f, best.axis);
    } else {
        const Vec3 pa = support(oa, edgeAxis);
        const Vec3 pb = support(ob, edgeAxis * -1.0f);
        points.push_back({(pa + pb) * 0.5f, std::max(0.0f, best.overlap)});
    }
    if (points.empty()) {
        const Vec3 pa = support(oa, best.axis);
        const Vec3 pb = support(ob, best.axis * -1.0f);
        points.push_back({(pa + pb) * 0.5f, std::max(0.0f, best.overlap)});
    }
    emitContacts(indexA, indexB, points, normalized(best.axis), best.overlap);
}

void World::emitContacts(unsigned indexA, unsigned indexB,
                         const std::vector<ContactPoint>& points, Vec3 normal,
                         float fallbackDepth) {
    if (indexA >= bodies_.size() || indexB >= bodies_.size()) return;
    Body& a = bodies_[indexA];
    Body& b = bodies_[indexB];
    for (const ContactPoint& point : points) {
        Contact contact;
        contact.indexA = indexA;
        contact.indexB = indexB;
        contact.entityA = a.entity;
        contact.entityB = b.entity;
        contact.point = point.point;
        contact.normal = normal;
        contact.penetration = point.penetration > 0.0f
                                  ? point.penetration
                                  : std::max(0.0f, fallbackDepth);
        const Vec3 ra = point.point - a.position;
        const Vec3 rb = point.point - b.position;
        const Vec3 va = a.velocity + cross(a.angularVelocity, ra);
        const Vec3 vb = b.velocity + cross(b.angularVelocity, rb);
        contact.approachSpeed = dot(vb - va, contact.normal);
        const float restitution = std::max(a.restitution, b.restitution);
        contact.restitution = contact.approachSpeed < -settings_.restitutionThreshold
                                  ? restitution
                                  : 0.0f;
        const float invMassSum = a.invMass + b.invMass;
        if (invMassSum <= 0.0f) continue;
        const Vec3 angularA = transform(a.invInertiaWorld, cross(ra, contact.normal));
        const Vec3 angularB = transform(b.invInertiaWorld, cross(rb, contact.normal));
        const float normalMass = invMassSum + dot(contact.normal, cross(angularA, ra)) +
                                 dot(contact.normal, cross(angularB, rb));
        if (normalMass <= 1.0e-9f) continue;
        contact.normalMass = 1.0f / normalMass;
        buildTangentBasis(contact.normal, contact.tangent[0], contact.tangent[1]);
        for (int t = 0; t < 2; ++t) {
            const Vec3 tangent = contact.tangent[t];
            const Vec3 ta = transform(a.invInertiaWorld, cross(ra, tangent));
            const Vec3 tb = transform(b.invInertiaWorld, cross(rb, tangent));
            const float mass = invMassSum + dot(tangent, cross(ta, ra)) +
                               dot(tangent, cross(tb, rb));
            contact.tangentMass[t] = mass > 1.0e-9f ? 1.0f / mass : 0.0f;
        }
        contacts_.push_back(contact);
    }
}
void World::solveContacts(float dt) {
    const float inverseDt = dt > 1.0e-9f ? 1.0f / dt : 0.0f;
    for (int iteration = 0; iteration < settings_.velocityIterations; ++iteration) {
        for (Contact& contact : contacts_) {
            if (contact.indexA >= bodies_.size() || contact.indexB >= bodies_.size()) continue;
            Body* a = &bodies_[contact.indexA];
            Body* b = &bodies_[contact.indexB];
            if (!a->active || !b->active) continue;
            if (!a->dynamic && !b->dynamic) continue;
            if (a->sleeping && b->sleeping) continue;

            const Vec3 ra = contact.point - a->position;
            const Vec3 rb = contact.point - b->position;
            Vec3 relative = b->velocity + cross(b->angularVelocity, rb) -
                            a->velocity - cross(a->angularVelocity, ra);
            const float velocityAlongNormal = dot(relative, contact.normal);
            const float penetrationError =
                std::max(contact.penetration - settings_.positionSlop, 0.0f);
            float bias = settings_.baumgarte * penetrationError * inverseDt;
            bias = std::min(bias, settings_.maxBiasSpeed);
            const float targetVelocity =
                contact.restitution > 0.0f ? -contact.restitution * contact.approachSpeed
                                           : 0.0f;
            if (velocityAlongNormal > targetVelocity + bias && penetrationError <= 0.0f) {
                continue;
            }
            const float lambda =
                (targetVelocity + bias - velocityAlongNormal) * contact.normalMass;
            const float oldNormal = contact.normalImpulse;
            contact.normalImpulse = std::max(0.0f, oldNormal + lambda);
            const float appliedNormal = contact.normalImpulse - oldNormal;
            applyImpulse(*a, contact.normal * -appliedNormal, ra);
            applyImpulse(*b, contact.normal * appliedNormal, rb);

            if (a->sleeping && b->dynamic && !b->sleeping) wake(static_cast<unsigned>(a - bodies_.data()));
            if (b->sleeping && a->dynamic && !a->sleeping) wake(static_cast<unsigned>(b - bodies_.data()));

            const float friction = std::sqrt(a->friction * b->friction);
            const float maxFriction = friction * contact.normalImpulse;
            for (int t = 0; t < 2; ++t) {
                relative = b->velocity + cross(b->angularVelocity, rb) -
                           a->velocity - cross(a->angularVelocity, ra);
                const float vt = dot(relative, contact.tangent[t]);
                const float lambdaT = -vt * contact.tangentMass[t];
                Vec3 newImpulse = contact.tangentImpulse + contact.tangent[t] * lambdaT;
                const float tangentLength = length(newImpulse);
                if (tangentLength > maxFriction && tangentLength > 1.0e-8f) {
                    newImpulse = newImpulse * (maxFriction / tangentLength);
                }
                const Vec3 applied = newImpulse - contact.tangentImpulse;
                contact.tangentImpulse = newImpulse;
                applyImpulse(*a, applied * -1.0f, ra);
                applyImpulse(*b, applied, rb);
            }
        }
    }
}

void World::correctPositions() {
    for (int iteration = 0; iteration < settings_.positionIterations; ++iteration) {
        // A face manifold has up to four contact records for the same pair. Applying a
        // positional correction for each of them over-corrects by 4x and pumps energy
        // into stacks, so use one deepest point per pair.
        std::size_t index = 0;
        while (index < contacts_.size()) {
            const Contact& first = contacts_[index];
            float deepest = first.penetration;
            std::size_t end = index + 1;
            while (end < contacts_.size() &&
                   contacts_[end].indexA == first.indexA &&
                   contacts_[end].indexB == first.indexB) {
                deepest = std::max(deepest, contacts_[end].penetration);
                ++end;
            }
            if (first.indexA < bodies_.size() && first.indexB < bodies_.size()) {
                Body* a = &bodies_[first.indexA];
                Body* b = &bodies_[first.indexB];
                const float invMassSum = a->invMass + b->invMass;
                if (a->active && b->active && invMassSum > 0.0f) {
                    float correction = std::max(deepest - settings_.positionSlop, 0.0f);
                    if (correction > 0.0f) {
                        correction = std::min(correction * 0.45f, 0.06f);
                        if (a->dynamic) {
                            a->position -= first.normal * (correction * a->invMass / invMassSum);
                        }
                        if (b->dynamic) {
                            b->position += first.normal * (correction * b->invMass / invMassSum);
                        }
                    }
                }
            }
            index = end;
        }
    }
}

void World::integrateVelocities(float dt) {
    for (Body& body : bodies_) {
        if (!body.active || !body.dynamic || body.sleeping) continue;
        const Vec3 acceleration = gravity_ + body.externalForce * body.invMass;
        body.velocity += acceleration * dt;
        body.angularVelocity += transform(body.invInertiaWorld, body.externalTorque) * dt;
        const float linear = std::max(0.0f, 1.0f - body.linearDamping * dt);
        const float angular = std::max(0.0f, 1.0f - body.angularDamping * dt);
        body.velocity *= linear;
        body.angularVelocity *= angular;
        const float speed = length(body.velocity);
        if (speed > 90.0f) body.velocity = body.velocity * (90.0f / speed);
        const float spin = length(body.angularVelocity);
        if (spin > 40.0f) body.angularVelocity = body.angularVelocity * (40.0f / spin);
    }
}

void World::integratePositions(float dt) {
    for (Body& body : bodies_) {
        if (!body.active || !body.dynamic || body.sleeping) continue;
        body.position += body.velocity * dt;
        const Quat omega{0.0f, body.angularVelocity.x, body.angularVelocity.y,
                         body.angularVelocity.z};
        const Quat derivative = multiply(omega, body.orientation);
        body.orientation.w += derivative.w * (0.5f * dt);
        body.orientation.x += derivative.x * (0.5f * dt);
        body.orientation.y += derivative.y * (0.5f * dt);
        body.orientation.z += derivative.z * (0.5f * dt);
        body.orientation = normalize(body.orientation);
        refreshInertia(body);
        ++body.ageTicks;
        if (body.lifetime > 0.0f) {
            body.lifetime -= dt;
            if (body.lifetime <= 0.0f) body.active = false;
        }
    }
}

void World::updateSleep(float dt) {
    for (Body& body : bodies_) {
        if (!body.active || !body.dynamic || body.sleeping) continue;
        if (length(body.velocity) < settings_.sleepLinear &&
            length(body.angularVelocity) < settings_.sleepAngular) {
            body.sleepTimer += dt;
            if (body.sleepTimer >= settings_.sleepDelay) {
                body.sleeping = true;
                body.sleepTimer = settings_.sleepDelay;
                body.velocity = {};
                body.angularVelocity = {};
            }
        } else {
            body.sleepTimer = 0.0f;
        }
    }
}

void World::collectImpacts() {
    for (const Contact& contact : contacts_) {
        if (contact.normalImpulse < 0.02f) continue;
        if (contact.approachSpeed > -0.55f) continue;
        Impact impact;
        impact.indexA = contact.indexA;
        impact.indexB = contact.indexB;
        impact.entityA = contact.entityA;
        impact.entityB = contact.entityB;
        impact.point = contact.point;
        impact.normal = contact.normal;
        impact.normalImpulse = contact.normalImpulse;
        impact.approachSpeed = -contact.approachSpeed;
        bool merged = false;
        for (Impact& existing : frameImpacts_) {
            const bool samePair =
                (existing.indexA == impact.indexA && existing.indexB == impact.indexB) ||
                (existing.indexA == impact.indexB && existing.indexB == impact.indexA);
            if (!samePair) continue;
            merged = true;
            if (impact.normalImpulse > existing.normalImpulse) {
                existing.point = impact.point;
                existing.normal = impact.normal;
                existing.normalImpulse = impact.normalImpulse;
                existing.approachSpeed = impact.approachSpeed;
            }
            break;
        }
        if (!merged) frameImpacts_.push_back(impact);
    }
}

// ------------------------------------------------------------------------ joints

unsigned World::addJoint(const JointDesc& desc) {
    if (desc.bodyA >= bodies_.size()) return 0;
    if (desc.bodyB != 0xFFFFFFFFu && desc.bodyB >= bodies_.size()) return 0;
    Joint joint;
    joint.id = nextJointId_++;
    joint.type = desc.type;
    joint.bodyA = desc.bodyA;
    joint.bodyB = desc.bodyB;
    joint.localAnchorA = desc.anchorA;
    joint.localAnchorB = desc.anchorB;
    joint.localAxis = normalized(desc.axis);
    const Vec3 helper = absf(joint.localAxis.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f}
                                                       : Vec3{1.0f, 0.0f, 0.0f};
    joint.localReference = normalized(cross(joint.localAxis, helper));
    joint.hingeSpan = std::max(0.01f, desc.hingeSpan);
    joint.restLength = std::max(0.0f, desc.restLength);
    joint.compliance = std::max(0.0f, desc.compliance);
    joint.lowerLimit = std::min(desc.lowerLimit, desc.upperLimit);
    joint.upperLimit = std::max(desc.lowerLimit, desc.upperLimit);
    joint.breakForce = std::max(0.0f, desc.breakForce);
    joint.collideConnected = desc.collideConnected;
    joint.motorSpeed = desc.motorSpeed;
    joint.motorMaxTorque = std::max(0.0f, desc.motorMaxTorque);
    joint.friction = std::clamp(desc.friction, 0.0f, 1.0f);
    joints_.push_back(joint);
    rebuildNoCollidePairs();
    return joint.id;
}

unsigned World::brokenJointCount() const {
    unsigned count = 0;
    for (const Joint& joint : joints_) {
        if (joint.broken) ++count;
    }
    return count;
}

void World::removeJoint(unsigned id) {
    for (std::size_t i = 0; i < joints_.size(); ++i) {
        if (joints_[i].id != id) continue;
        joints_.erase(joints_.begin() + static_cast<std::ptrdiff_t>(i));
        rebuildNoCollidePairs();
        return;
    }
}

void World::rebuildNoCollidePairs() {
    noCollidePairs_.clear();
    for (const Joint& joint : joints_) {
        if (joint.collideConnected) continue;
        if (joint.bodyB >= bodies_.size()) continue;
        noCollidePairs_.push_back({joint.bodyA, joint.bodyB});
    }
}

bool World::skipConnectedPair(unsigned indexA, unsigned indexB) const {
    for (const std::pair<unsigned, unsigned>& pair : noCollidePairs_) {
        if ((pair.first == indexA && pair.second == indexB) ||
            (pair.first == indexB && pair.second == indexA)) {
            return true;
        }
    }
    return false;
}

Joint* World::joint(unsigned id) {
    for (Joint& joint : joints_) {
        if (joint.id == id) return &joint;
    }
    return nullptr;
}

const Joint* World::joint(unsigned id) const {
    for (const Joint& joint : joints_) {
        if (joint.id == id) return &joint;
    }
    return nullptr;
}

// Snapshot the state of every body touched by a live joint before integration.
// XPBD derives velocity from the corrected displacement, so this is the "previous
// position" of the prediction step.
void World::captureJointBodies() {
    jointPrev_.clear();
    for (const Joint& joint : joints_) {
        if (joint.broken) continue;
        for (unsigned index : {joint.bodyA, joint.bodyB}) {
            if (index >= bodies_.size()) continue;
            const Body& body = bodies_[index];
            if (!body.dynamic || !body.active) continue;
            bool seen = false;
            for (const JointPrev& prev : jointPrev_) {
                if (prev.body == index) {
                    seen = true;
                    break;
                }
            }
            if (seen) continue;
            jointPrev_.push_back({index, body.position, body.orientation});
        }
    }
}

void World::applyPositionalImpulse(Body& body, Vec3 impulse, Vec3 r) {
    if (!body.dynamic) return;
    body.position += impulse * body.invMass;
    const Vec3 dTheta = transform(body.invInertiaWorld, cross(r, impulse));
    const Quat omega{0.0f, dTheta.x, dTheta.y, dTheta.z};
    const Quat derivative = multiply(omega, body.orientation);
    body.orientation = normalize({body.orientation.w + derivative.w * 0.5f,
                                  body.orientation.x + derivative.x * 0.5f,
                                  body.orientation.y + derivative.y * 0.5f,
                                  body.orientation.z + derivative.z * 0.5f});
    refreshInertia(body);
}

void World::applyAngularCorrection(Body& body, Vec3 angularImpulse) {
    if (!body.dynamic) return;
    const Vec3 dTheta = transform(body.invInertiaWorld, angularImpulse);
    const Quat omega{0.0f, dTheta.x, dTheta.y, dTheta.z};
    const Quat derivative = multiply(omega, body.orientation);
    body.orientation = normalize({body.orientation.w + derivative.w * 0.5f,
                                  body.orientation.x + derivative.x * 0.5f,
                                  body.orientation.y + derivative.y * 0.5f,
                                  body.orientation.z + derivative.z * 0.5f});
    refreshInertia(body);
}

// One XPBD point constraint. `inequality` makes it pull-only (rope): it stays
// inactive while the distance is below restLength.
float World::projectPoint(unsigned indexA, unsigned indexB, Vec3 anchorA, Vec3 anchorB,
                          float restLength, bool inequality, float compliance, float h,
                          float& lambda) {
    if (indexA >= bodies_.size()) return 0.0f;
    Body& a = bodies_[indexA];
    Body* b = indexB < bodies_.size() ? &bodies_[indexB] : nullptr;
    const Vec3 delta = anchorB - anchorA;
    const float lengthSquared = dot(delta, delta);
    if (!std::isfinite(lengthSquared)) return 0.0f;
    const float distance = std::sqrt(lengthSquared);
    if (distance < 1.0e-6f) return 0.0f;
    const Vec3 n = delta / distance;
    const float C = distance - restLength;
    if (inequality && C <= 0.0f) {
        lambda = 0.0f;
        return 0.0f;
    }
    const float invMassA = a.dynamic ? a.invMass : 0.0f;
    const float invMassB = (b && b->dynamic) ? b->invMass : 0.0f;
    if (invMassA + invMassB <= 0.0f) return 0.0f;
    const Vec3 ra = anchorA - a.position;
    const Vec3 rb = b ? (anchorB - b->position) : Vec3{};
    const Vec3 angularA = a.dynamic ? transform(a.invInertiaWorld, cross(ra, n)) : Vec3{};
    const Vec3 angularB =
        (b && b->dynamic) ? transform(b->invInertiaWorld, cross(rb, n)) : Vec3{};
    const float w = invMassA + invMassB + dot(n, cross(angularA, ra)) +
                    dot(n, cross(angularB, rb));
    if (w <= 1.0e-9f) return 0.0f;
    const float alpha = compliance / (h * h);
    float dLambda = (-C - alpha * lambda) / (w + alpha);
    if (inequality && lambda + dLambda > 0.0f) dLambda = -lambda;
    lambda += dLambda;
    const Vec3 p = n * dLambda;
    applyPositionalImpulse(a, p * -1.0f, ra);
    if (b) applyPositionalImpulse(*b, p, rb);
    return dLambda;
}

// Angular limit for hinges: rotate both bodies about the hinge axis by the
// inverse-inertia weighted share of the violation.
void World::solveHingeLimit(Joint& joint) {
    if (joint.bodyA >= bodies_.size()) return;
    Body& a = bodies_[joint.bodyA];
    Body* b = joint.bodyB < bodies_.size() ? &bodies_[joint.bodyB] : nullptr;
    const Vec3 axis = normalized(rotate(a.orientation, joint.localAxis));
    const Vec3 uA = normalized(rotate(a.orientation, joint.localReference));
    const Vec3 uB = b ? normalized(rotate(b->orientation, joint.localReference)) : uA;
    const float angle = std::atan2(dot(cross(uA, uB), axis), dot(uA, uB));
    joint.angle = angle;
    float error = 0.0f;
    if (angle < joint.lowerLimit) error = angle - joint.lowerLimit;
    else if (angle > joint.upperLimit) error = angle - joint.upperLimit;
    else return;
    joint.limitError = error;
    float w = 0.0f;
    if (a.dynamic) w += dot(axis, transform(a.invInertiaWorld, axis));
    if (b && b->dynamic) w += dot(axis, transform(b->invInertiaWorld, axis));
    if (w <= 1.0e-9f) return;
    // Bound the angular step the limit may take in one iteration. The correction is turned
    // into velocity by the XPBD writeback, so an unbounded value here injects energy
    // instead of removing it: a hinge driven hard into its stop would launch itself.
    constexpr float maxLimitStep = 0.02f; // radians per iteration
    error = std::clamp(error, -maxLimitStep, maxLimitStep);
    const float correction = error / w;
    const Vec3 torque = axis * correction;
    applyAngularCorrection(a, torque);
    if (b) applyAngularCorrection(*b, torque * -1.0f);
}

// Hinge motor and friction. These act on angular velocity *after* the XPBD writeback, not
// on positions: a position-level drive would be converted into velocity by the writeback
// and then integrated again the next step, which compounds into an oscillation that only
// the torque clamp keeps bounded. A velocity-level drive is also what a real motor is.
void World::solveHingeDrive(Joint& joint, float dt) {
    if (joint.bodyA >= bodies_.size()) return;
    Body& a = bodies_[joint.bodyA];
    Body* b = joint.bodyB < bodies_.size() ? &bodies_[joint.bodyB] : nullptr;
    if (!a.dynamic && (!b || !b->dynamic)) return;
    const Vec3 axis = normalized(rotate(a.orientation, joint.localAxis));
    float w = 0.0f;
    if (a.dynamic && !a.sleeping) w += dot(axis, transform(a.invInertiaWorld, axis));
    if (b && b->dynamic && !b->sleeping) w += dot(axis, transform(b->invInertiaWorld, axis));
    if (w <= 1.0e-9f) return;

    const float relative = dot((b ? b->angularVelocity : Vec3{}) - a.angularVelocity, axis);

    // Velocity-level limit. The position limit alone can only absorb motion by correcting
    // every step, and that correction returns as velocity, so a drive pressed against a
    // limit pumps itself apart. Sign convention: the joint angle advances with the
    // relative rate, so a negative rate drives the lower limit.
    // One target for the relative rate, then a single correction towards it. Computing the
    // limit and the motor separately and adding both would apply the same velocity twice
    // and overshoot in the opposite direction. The blocked direction comes from the
    // position solve: a positive violation means the upper limit was crossed.
    // The stop is applied slightly before the limit. Waiting for the position solve to
    // report a violation means the body has already crossed the plane, and undoing that
    // overshoot is exactly the energy injection this clamp exists to avoid.
    constexpr float limitSlop = 0.05f;  // radians, about 3 degrees
    const bool nearLower = joint.angle <= joint.lowerLimit + limitSlop || joint.limitError < 0.0f;
    const bool nearUpper = joint.angle >= joint.upperLimit - limitSlop || joint.limitError > 0.0f;
    float target = joint.motorMaxTorque > 0.0f ? joint.motorSpeed : relative;
    if (nearLower && target < 0.0f) target = 0.0f;
    if (nearUpper && target > 0.0f) target = 0.0f;
    float delta = target - relative;
    if (joint.friction > 0.0f) {
        // Bleed the relative swing; the clamp keeps a fast spin from stopping dead.
        delta -= std::clamp(relative * std::clamp(joint.friction, 0.0f, 1.0f) * dt,
                            -std::abs(relative) * 0.5f, std::abs(relative) * 0.5f);
    }
    if (std::abs(delta) < 1.0e-6f) return;

    // Angular impulse that changes the relative rate by `delta`, then the torque budget
    // (impulse <= torque * dt) as the final word.
    float impulse = delta / w;
    if (joint.motorMaxTorque > 0.0f) {
        const float maxImpulse = joint.motorMaxTorque * dt;
        impulse = std::clamp(impulse, -maxImpulse, maxImpulse);
    }
    const Vec3 angularImpulse = axis * impulse;
    if (a.dynamic && !a.sleeping) a.angularVelocity = a.angularVelocity - transform(a.invInertiaWorld, angularImpulse);
    if (b && b->dynamic && !b->sleeping) b->angularVelocity = b->angularVelocity + transform(b->invInertiaWorld, angularImpulse);
    if (a.dynamic && a.sleeping && std::abs(impulse) > 1.0e-6f) wake(joint.bodyA);
    if (b && b->dynamic && b->sleeping && std::abs(impulse) > 1.0e-6f) wake(joint.bodyB);
}

void World::solveJoints(float dt) {
    if (joints_.empty() || dt <= 1.0e-9f) return;
    // lambda is the multiplier for this step only. Keeping it across steps would
    // turn the break metric into an integral instead of a force.
    for (Joint& joint : joints_) { joint.lambda = 0.0f; joint.limitError = 0.0f; }
    const int iterations = std::max(1, settings_.jointIterations);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (Joint& joint : joints_) {
            if (joint.broken) continue;
            if (joint.bodyA >= bodies_.size()) {
                joint.broken = true;
                continue;
            }
            Body& a = bodies_[joint.bodyA];
            Body* b = joint.bodyB < bodies_.size() ? &bodies_[joint.bodyB] : nullptr;
            if (!a.active || (b && !b->active)) continue;
            const bool aMovable = a.dynamic && !a.sleeping;
            const bool bMovable = b && b->dynamic && !b->sleeping;
            if (!aMovable && !bMovable) continue;
            if (a.dynamic && a.sleeping && bMovable) wake(joint.bodyA);
            if (b && b->dynamic && b->sleeping && aMovable) wake(joint.bodyB);

            const Vec3 anchorA = a.position + rotate(a.orientation, joint.localAnchorA);
            const Vec3 anchorB = b ? b->position + rotate(b->orientation, joint.localAnchorB)
                                   : joint.localAnchorB;

            if (joint.type == JointType::Hinge) {
                // Two points separated along the hinge axis, each expressed in its
                // own body's frame. Rotation about the axis keeps both offsets
                // aligned; every other axis makes them disagree, which is the
                // constraint. The limit runs first so the point solve has the
                // last word and the hinge anchor cannot drift.
                const Vec3 axisA = rotate(a.orientation, joint.localAxis);
                const Vec3 axisB = b ? rotate(b->orientation, joint.localAxis) : axisA;
                const Vec3 offsetA = axisA * joint.hingeSpan;
                const Vec3 offsetB = axisB * joint.hingeSpan;
                solveHingeLimit(joint);
                projectPoint(joint.bodyA, joint.bodyB, anchorA + offsetA, anchorB + offsetB,
                             0.0f, false, joint.compliance, dt, joint.lambda);
                float secondary = 0.0f;
                projectPoint(joint.bodyA, joint.bodyB, anchorA - offsetA, anchorB - offsetB,
                             0.0f, false, joint.compliance, dt, secondary);
            } else if (joint.type == JointType::Rope) {
                projectPoint(joint.bodyA, joint.bodyB, anchorA, anchorB, joint.restLength,
                             true, joint.compliance, dt, joint.lambda);
            } else {
                projectPoint(joint.bodyA, joint.bodyB, anchorA, anchorB, joint.restLength,
                             false, joint.compliance, dt, joint.lambda);
            }
            // lambda carries kg*m (the XPBD multiplier), so force = lambda / h^2.
            if (joint.breakForce > 0.0f &&
                std::abs(joint.lambda) / (dt * dt) > joint.breakForce) {
                joint.broken = true;
            }
        }
    }
    // XPBD velocity writeback: v = (x_corrected - x_previous) / h.
    for (const JointPrev& prev : jointPrev_) {
        if (prev.body >= bodies_.size()) continue;
        Body& body = bodies_[prev.body];
        if (!body.dynamic || !body.active) continue;
        body.velocity = (body.position - prev.position) / dt;
        body.angularVelocity = rotationDelta(prev.orientation, body.orientation) / dt;
    }
    // Motors and hinge friction run last, so their velocity change is not overwritten by
    // the writeback above.
    for (Joint& joint : joints_) {
        if (joint.broken || joint.type != JointType::Hinge) continue;
        if (joint.bodyA >= bodies_.size()) continue;
        const Body& a = bodies_[joint.bodyA];
        const Body* b = joint.bodyB < bodies_.size() ? &bodies_[joint.bodyB] : nullptr;
        if (!a.active || (b && !b->active)) continue;
        solveHingeDrive(joint, dt);
    }
}

void World::step(float dt) {
    const auto start = std::chrono::steady_clock::now();
    dt = clampf(dt, 0.0f, 0.1f);
    if (dt <= 0.0f) return;
    frameImpacts_.clear();

    // Adaptive substeps: bound the distance any body can travel between collision
    // queries. This is a practical tunnelling guard for thrown objects and blasts.
    float maxSpeed = 0.0f;
    for (const Body& body : bodies_) {
        if (!body.active || !body.dynamic || body.sleeping) continue;
        const float radius = body.half.x + body.half.y + body.half.z;
        maxSpeed = std::max(maxSpeed, length(body.velocity) +
                                         length(body.angularVelocity) * radius);
    }
    int substeps = 1;
    if (maxSpeed * dt > settings_.maxTravelPerSubstep) {
        substeps = static_cast<int>(std::ceil(maxSpeed * dt /
                                             settings_.maxTravelPerSubstep));
    }
    substeps = std::max(1, std::min(settings_.maxSubsteps, substeps));
    substepsLast_ = static_cast<unsigned>(substeps);
    const float h = dt / static_cast<float>(substeps);
    for (int substep = 0; substep < substeps; ++substep) {
        integrateVelocities(h);
        generateContacts();
        solveContacts(h);
        correctPositions();
        // Snapshot before integration so XPBD can rebuild velocity from the
        // corrected displacement instead of double-counting the correction.
        captureJointBodies();
        integratePositions(h);
        solveJoints(h);
        updateSleep(h);
        collectImpacts();
    }
    // External inputs are consumed by this step. A caller that wants a continuous
    // force re-applies it every tick, which keeps the input explicit and replayable.
    for (Body& body : bodies_) {
        body.externalForce = {};
        body.externalTorque = {};
    }
    ++totalSteps_;
    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    lastStepMs_ = elapsed;
    totalStepMs_ += elapsed;
    ++timedSteps_;
    maxStepMs_ = std::max(maxStepMs_, elapsed);
}

Stats World::stats() const {
    Stats result;
    result.bodies = static_cast<unsigned>(bodies_.size());
    result.contacts = lastContactCount_;
    result.substeps = substepsLast_;
    result.totalSteps = totalSteps_;
    result.lastStepMs = lastStepMs_;
    result.maxStepMs = maxStepMs_;
    result.averageStepMs = timedSteps_ ? totalStepMs_ / static_cast<double>(timedSteps_) : 0.0;
    result.broadphaseMode = broadphaseMode_;
    result.broadphasePairs = broadphasePairs_;
    result.broadphaseNodes = broadphase_.nodeCount();
    result.broadphaseRebuilds = broadphase_.rebuilds();
    result.joints = static_cast<unsigned>(joints_.size());
    for (const Joint& joint : joints_) {
        if (joint.broken) ++result.brokenJoints;
    }
    for (const Body& body : bodies_) {
        if (!body.active) continue;
        ++result.activeBodies;
        if (body.dynamic) ++result.dynamicBodies;
        if (body.dynamic && body.sleeping) ++result.sleepingBodies;
        if (body.debris) ++result.debris;
        if (body.projectile) ++result.projectiles;
    }
    return result;
}

} // namespace physics
} // namespace engine
