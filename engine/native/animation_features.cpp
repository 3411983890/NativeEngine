#include "animation.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::animation {
namespace {
constexpr float Pi = 3.14159265358979323846f, Dt = 1.f / TickRate;
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(V3 a);
V3 unit(V3 a, V3 fallback = {0, 1, 0});
V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 rotateY(V3 v, float heading) {
  const float c = std::cos(heading), s = std::sin(heading);
  return {c * v.x - s * v.z, v.y, s * v.x + c * v.z};
}
struct Landing {
  bool valid = false;
  V3 target, normal{0, 1, 0};
  unsigned surface = 0;
  Matrix surfaceToWorld{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
  float spread = 0;
};
Landing sampleLanding(const GroundQuery &query, V3 desired, V3 forward,
                      float radius, float up, float down, float maxSlopeCosine,
                      float maxSpread, float soleHeight, unsigned &queries) {
  Landing result;
  if (!query)
    return result;
  forward.y = 0;
  forward = unit(forward, {0, 0, -1});
  const V3 right{forward.z, 0, -forward.x};
  const std::array<V3, 5> offsets{{V3{},
                                   mul(forward, radius),
                                   mul(forward, -radius),
                                   mul(right, radius * .7f),
                                   mul(right, -radius * .7f)}};
  GroundHit chosen;
  std::array<GroundHit, 5> hits{};
  unsigned valid = 0;
  float lowest = std::numeric_limits<float>::max(),
        highest = -std::numeric_limits<float>::max();
  V3 normal{};
  for (unsigned i = 0; i < offsets.size(); ++i) {
    ++queries;
    hits[i] = query(add(add(desired, offsets[i]), {0, up, 0}), up + down);
    if (!hits[i].hit || hits[i].normal.y < maxSlopeCosine)
      continue;
    ++valid;
    lowest = std::min(lowest, hits[i].point.y);
    highest = std::max(highest, hits[i].point.y);
    normal = add(normal, unit(hits[i].normal));
    if (!chosen.hit || hits[i].point.y > chosen.point.y)
      chosen = hits[i];
  }
  if (valid < 2 || !chosen.hit)
    return result;
  result.spread = highest - lowest;
  if (result.spread > maxSpread)
    return result;
  result.valid = true;
  result.surface = chosen.surface;
  result.surfaceToWorld = chosen.surfaceToWorld;
  result.normal = unit(normal, unit(chosen.normal));
  result.target = add(chosen.point, mul(result.normal, soleHeight));
  return result;
}

float len(V3 a) { return std::sqrt(dot(a, a)); }
V3 unit(V3 a, V3 fallback) {
  float l = len(a);
  return l > 1e-7f ? mul(a, 1 / l) : fallback;
}
bool finite(V3 a) {
  return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
void require(bool ok, const char *message) {
  if (!ok)
    throw Error("NG-E-ANIM-RIG", message);
}
Quat product(Quat a, Quat b) {
  return normalized({a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
}
Quat inv(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
Quat rotationBetween(V3 a, V3 b) {
  a = unit(a);
  b = unit(b);
  float d = std::clamp(dot(a, b), -1.f, 1.f);
  if (d < -.9999f) {
    auto axis = unit(cross(a, std::abs(a.x) < .8f ? V3{1, 0, 0} : V3{0, 1, 0}));
    return {axis.x, axis.y, axis.z, 0};
  }
  auto c = cross(a, b);
  return normalized({c.x, c.y, c.z, 1 + d});
}
Quat globalRotation(const Skeleton &s, const PoseBuffer &p, unsigned b) {
  Quat q = p.get(b).rotation;
  for (int i = s.bones[b].parent; i >= 0; i = s.bones[i].parent)
    q = product(p.get(i).rotation, q);
  return q;
}
void rotateGlobal(const Skeleton &s, PoseBuffer &p, unsigned b, Quat delta) {
  auto q = product(delta, globalRotation(s, p, b));
  int parent = s.bones[b].parent;
  if (parent >= 0)
    q = product(inv(globalRotation(s, p, unsigned(parent))), q);
  auto t = p.get(b);
  t.rotation = q;
  p.set(b, t);
}
V3 rotateVector(Quat q, V3 v) {
  // Standard unit-quaternion rotation, written out instead of going through
  // product(): a zero-length offset would otherwise form a zero quaternion and
  // trip the pose validation.
  const V3 u{q.x, q.y, q.z};
  const V3 t = mul(cross(u, v), 2.f);
  return add(v, add(mul(t, q.w), cross(u, t)));
}
V3 origin(const Matrix &m) { return {m[12], m[13], m[14]}; }
V3 direction(const Matrix &m, V3 v) { return sub(point(m, v), origin(m)); }
Matrix actorMatrix(V3 p, float heading, float scale = 1) {
  Transform t;
  t.translation = p;
  t.scale = {scale, scale, scale};
  t.rotation = {0, -std::sin(heading / 2), 0, std::cos(heading / 2)};
  return matrix(t);
}
std::array<float, MotionDimensions>
rawFeature(const Asset &a, const PoseBuffer &p, V3 velocity, float turn,
           const std::array<V3, 3> &trajectory) {
  std::array<float, MotionDimensions> f{};
  unsigned at = 0;
  for (auto v : trajectory) {
    f[at++] = v.x;
    f[at++] = v.z;
  }
  for (float t : {.2f, .4f, .6f})
    f[at++] = turn * t;
  auto g = a.skeleton.globals(p);
  for (unsigned side = 0; side < 2; ++side) {
    V3 foot{};
    if (side < a.rig.legs.size())
      foot = sub(origin(g[a.rig.legs[side].tip]), origin(g[0]));
    f[at++] = foot.x;
    f[at++] = foot.y;
    f[at++] = foot.z;
    f[at++] = 0;
    f[at++] = 0;
    f[at++] = 0;
  }
  f[21] = velocity.x;
  f[22] = velocity.z;
  // Contact state is more reliable than an arbitrary normalized-time phase
  // across clips.
  f[23] = 0;
  f[24] = 0;
  f[25] = a.rig.pelvis >= 0
              ? origin(g[unsigned(a.rig.pelvis)]).y - origin(g[0]).y
              : 0;
  return f;
}
void contacts(const Clip &c, float phase,
              std::array<float, MotionDimensions> &f) {
  for (unsigned s = 0; s < 2; ++s) {
    if (!c.contactKnown()) {
      f[23 + s] = 0;
      continue;
    }
    auto r = c.contacts[s];
    bool on = r[0] <= r[1] ? phase >= r[0] && phase < r[1]
                           : phase >= r[0] || phase < r[1];
    f[23 + s] = on ? 1.f : 0.f;
  }
}
V3 clipVelocity(const Clip &c, double seconds) {
  auto d = c.rootDelta(seconds, seconds + 1.0 / 60);
  return len(d) > 1e-6f ? mul(d, 60) : c.motionVelocity;
}
std::array<std::int16_t, MotionDimensions>
quantize(const std::array<float, MotionDimensions> &f,
         const std::array<float, MotionDimensions> &scales) {
  std::array<std::int16_t, MotionDimensions> q{};
  for (unsigned i = 0; i < q.size(); ++i)
    q[i] = static_cast<std::int16_t>(
        std::clamp(std::lround(f[i] * scales[i]), -30000l, 30000l));
  return q;
}
std::array<V3, 4> tetra(float r) {
  return {{{r, r, r}, {r, -r, -r}, {-r, r, -r}, {-r, -r, r}}};
}
float volume(const std::array<V3, 4> &p) {
  return dot(sub(p[1], p[0]), cross(sub(p[2], p[0]), sub(p[3], p[0]))) / 6;
}
V3 closest(V3 p, V3 a, V3 b) {
  auto d = sub(b, a);
  return add(a,
             mul(d, std::clamp(dot(sub(p, a), d) / std::max(dot(d, d), 1e-8f),
                               0.f, 1.f)));
}
// Rodrigues rotation. Sole roll and the turn pivot are rigid rotations about a
// contact edge, so the edge itself is the fixed point of the solve.
V3 rotated(V3 v, V3 axis, float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  return add(add(mul(v, c), mul(cross(axis, v), s)),
             mul(axis, dot(axis, v) * (1 - c)));
}
Quat axisAngle(V3 axis, float angle) {
  axis = unit(axis, {1, 0, 0});
  const float s = std::sin(angle * .5f);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * .5f)};
}
// Ground-plane forward of the foot. The model faces -Z; projecting onto the
// contact plane keeps the roll axis on the surface instead of pushing it out of
// the ground on a slope.
V3 contactForward(float heading, V3 normal) {
  const V3 forward = rotateY({0, 0, -1}, heading);
  return unit(sub(forward, mul(normal, dot(forward, normal))), forward);
}
// Position inside the authored contact window: 0 at the first contact frame, 1
// at the last. Negative means the phase is outside the window, so the geometry
// decides the plant and the sole stays flat.
float windowProgress(const Clip &clip, unsigned side, float phase) {
  if (!clip.contactKnown() || side >= clip.contacts.size())
    return -1;
  const auto range = clip.contacts[side];
  const float length = range[1] >= range[0] ? range[1] - range[0]
                                            : 1.f - range[0] + range[1];
  if (length <= 1e-4f)
    return -1;
  float t = phase - range[0];
  if (t < 0)
    t += 1;
  if (t > length)
    return -1;
  return t / length;
}
// Contact edge offset for a rolled sole, in model units. The pivot is the same
// point on the shoe for the whole roll, so the edge is fixed by the sign of the
// roll rather than scaled with it.
float soleEdge(float roll, float footLength) {
  return (roll > 0 ? .55f : (roll < 0 ? -.35f : 0.f)) * footLength;
}
float smoothstep(float edge0, float edge1, float x) {
  const float t =
      std::clamp((x - edge0) / std::max(1e-6f, edge1 - edge0), 0.f, 1.f);
  return t * t * (3 - 2 * t);
}
// Ankle position for a sole rolled about its active contact edge. The pivot is
// the same point on the shoe for the whole roll, so changing the angle moves the
// ankle along an arc while the contact point stays put. That invariant is what
// lets the reach budget below cut the pitch instead of dragging the shoe.
V3 rolledAnkle(V3 flat, V3 normal, float heading, float roll, float soleHeight,
               float footLength, float scale) {
  if (roll == 0)
    return flat;
  const V3 forward = contactForward(heading, normal);
  const V3 surface = sub(flat, mul(normal, soleHeight * scale));
  const V3 pivot =
      add(surface, mul(forward, soleEdge(roll, footLength) * scale));
  const V3 axis = unit(cross(normal, forward), {1, 0, 0});
  return add(pivot, rotated(sub(flat, pivot), axis, roll));
}
// Pelvis translation is authored in model space. The offset is given in model
// space here and converted into the pelvis parent's frame, so a rig whose hips
// hang under a rotated root still moves where the solver asked.
void translatePelvis(const Skeleton &skeleton, PoseBuffer &pose, int pelvis,
                     V3 modelOffset) {
  if (pelvis < 0 || pelvis >= int(skeleton.bones.size()))
    return;
  // A zero offset is the common case (both feet already reach their targets).
  // Rotating it through the parent frame would build a zero-length quaternion,
  // which the pose validation correctly rejects.
  if (std::abs(modelOffset.x) + std::abs(modelOffset.y) +
          std::abs(modelOffset.z) <
      1e-9f)
    return;
  const int parent = skeleton.bones[unsigned(pelvis)].parent;
  const V3 local =
      parent >= 0
          ? rotateVector(inv(globalRotation(skeleton, pose, unsigned(parent))),
                         modelOffset)
          : modelOffset;
  pose.translation[unsigned(pelvis)][0] += local.x;
  pose.translation[unsigned(pelvis)][1] += local.y;
  pose.translation[unsigned(pelvis)][2] += local.z;
}
void hashV(determinism::Hasher &h, V3 v) {
  h.quantized(v.x);
  h.quantized(v.y);
  h.quantized(v.z);
}
} // namespace

const char *footPhaseName(FootPhase phase) {
  switch (phase) {
  case FootPhase::Swing:
    return "swing";
  case FootPhase::Release:
    return "release";
  case FootPhase::Landing:
    return "landing";
  case FootPhase::Support:
    return "support";
  }
  return "invalid";
}
const char *contactModeName(ContactMode mode) {
  switch (mode) {
  case ContactMode::Free:
    return "free";
  case ContactMode::Flat:
    return "flat";
  case ContactMode::Heel:
    return "heel";
  case ContactMode::Toe:
    return "toe";
  }
  return "invalid";
}
const char *contactReleaseName(ContactRelease reason) {
  switch (reason) {
  case ContactRelease::None:
    return "none";
  case ContactRelease::Lift:
    return "lift";
  case ContactRelease::Drift:
    return "drift";
  case ContactRelease::Overreach:
    return "overreach";
  case ContactRelease::Turn:
    return "turn";
  case ContactRelease::Surface:
    return "surface";
  case ContactRelease::Unwalkable:
    return "unwalkable";
  case ContactRelease::GroundLost:
    return "ground-lost";
  case ContactRelease::Teleport:
    return "teleport";
  case ContactRelease::Blend:
    return "blend";
  case ContactRelease::Stride:
    return "stride";
  case ContactRelease::Expired:
    return "expired";
  }
  return "invalid";
}
const char *rigFallbackName(RigFallback reason) {
  switch (reason) {
  case RigFallback::None:
    return "none";
  case RigFallback::Disabled:
    return "disabled";
  case RigFallback::NotGrounded:
    return "not-grounded";
  case RigFallback::NoGround:
    return "no-ground";
  case RigFallback::Unwalkable:
    return "unwalkable";
  case RigFallback::NoLanding:
    return "no-landing";
  case RigFallback::Unreachable:
    return "unreachable";
  case RigFallback::Teleport:
    return "teleport";
  }
  return "invalid";
}

V3 inversePoint(const Matrix &m, V3 p) {
  V3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
  float determinant = dot(a, cross(b, c));
  require(std::isfinite(determinant) && std::abs(determinant) > 1e-8f,
          "Singular control space");
  p = sub(p, origin(m));
  return {dot(p, cross(b, c)) / determinant, dot(p, cross(c, a)) / determinant,
          dot(p, cross(a, b)) / determinant};
}
IKResult solveTwoBone(const Skeleton &s, PoseBuffer &p, const RigChain &c,
                      V3 target, V3 pole) {
  require(c.root < s.bones.size() && c.joint < s.bones.size() &&
              c.tip < s.bones.size() &&
              s.bones[c.joint].parent == int(c.root) &&
              s.bones[c.tip].parent == int(c.joint),
          "Invalid two-bone chain");
  require(finite(target) && finite(pole) && c.minBend >= 0 &&
              c.maxBend > c.minBend && c.maxBend < Pi,
          "Invalid IK target or limits");
  auto g = s.globals(p);
  auto a = origin(g[c.root]), b = origin(g[c.joint]), end = origin(g[c.tip]);
  float l1 = len(sub(b, a)), l2 = len(sub(end, b));
  require(l1 > 1e-5f && l2 > 1e-5f, "Zero length IK bone");
  auto axis = unit(sub(target, a), {0, -1, 0});
  float wanted = len(sub(target, a));
  auto reach = [&](float bend) {
    return std::sqrt(
        std::max(0.f, l1 * l1 + l2 * l2 + 2 * l1 * l2 * std::cos(bend)));
  };
  float d =
      std::clamp(wanted, std::max(1e-5f, reach(c.maxBend)), reach(c.minBend));
  auto lateral = sub(sub(pole, a), mul(axis, dot(sub(pole, a), axis)));
  lateral = unit(
      lateral,
      unit(cross(axis, std::abs(axis.x) < .8f ? V3{1, 0, 0} : V3{0, 1, 0})));
  float x = (l1 * l1 - l2 * l2 + d * d) / (2 * d),
        y = std::sqrt(std::max(0.f, l1 * l1 - x * x));
  auto knee = add(add(a, mul(axis, x)), mul(lateral, y));
  rotateGlobal(s, p, c.root, rotationBetween(sub(b, a), sub(knee, a)));
  g = s.globals(p);
  b = origin(g[c.joint]);
  end = origin(g[c.tip]);
  rotateGlobal(s, p, c.joint,
               rotationBetween(sub(end, b), sub(add(a, mul(axis, d)), b)));
  g = s.globals(p);
  return {len(sub(origin(g[c.tip]), target)), std::abs(wanted - d) > 1e-5f};
}
void solveLookAt(const Skeleton &s, PoseBuffer &p, unsigned b, V3 target,
                 float maxAngle) {
  require(b < s.bones.size() && finite(target) && std::isfinite(maxAngle) &&
              maxAngle >= 0,
          "Invalid look control");
  auto g = s.globals(p);
  auto desired = sub(target, origin(g[b]));
  if (len(desired) < 1e-6f)
    return;
  auto delta = rotationBetween(direction(g[b], {0, 0, -1}), desired);
  float angle = 2 * std::acos(std::clamp(delta.w, -1.f, 1.f));
  if (angle > maxAngle && angle > 0)
    delta = slerp({}, delta, maxAngle / angle);
  rotateGlobal(s, p, b, delta);
}

void MotionDatabase::build(const Asset &a) {
  frames.clear();
  std::vector<std::array<float, MotionDimensions>> raw;
  std::array<float, MotionDimensions> maximum{};
  for (unsigned ci = 0; ci < a.clips.size(); ++ci) {
    const auto &c = a.clips[ci];
    if (!c.matchable)
      continue;
    for (unsigned frame = 0; frame + 1 < c.frameCount; ++frame) {
      double t = double(frame) / SampleRate;
      auto p = c.sample(t);
      auto velocity = clipVelocity(c, t);
      std::array<V3, 3> trajectory{};
      for (unsigned k = 0; k < 3; ++k) {
        float future = .2f * (k + 1);
        auto delta = c.rootDelta(t, t + future);
        trajectory[k] =
            len(delta) > 1e-6f ? delta : mul(c.motionVelocity, future);
      }
      auto f = rawFeature(a, p, velocity, c.turnRate, trajectory);
      contacts(c, float(t / c.duration()), f);
      auto next = a.skeleton.globals(c.sample(t + 1.0 / SampleRate)),
           now = a.skeleton.globals(p);
      for (unsigned side = 0; side < a.rig.legs.size(); ++side) {
        auto tip = a.rig.legs[side].tip;
        auto v = mul(sub(sub(origin(next[tip]), origin(next[0])),
                         sub(origin(now[tip]), origin(now[0]))),
                     float(SampleRate));
        f[12 + side * 6] = v.x;
        f[13 + side * 6] = v.y;
        f[14 + side * 6] = v.z;
      }
      for (unsigned i = 0; i < f.size(); ++i)
        maximum[i] = std::max(maximum[i], std::abs(f[i]));
      raw.push_back(f);
      frames.push_back({ci, frame, c.tags, {}});
    }
  }
  for (unsigned i = 0; i < scales.size(); ++i)
    scales[i] = 1000 / std::max(maximum[i], .1f);
  for (unsigned i = 0; i < frames.size(); ++i)
    frames[i].feature = quantize(raw[i], scales);
}
std::array<std::int16_t, MotionDimensions>
MotionDatabase::query(const Asset &a, const PoseBuffer &p,
                      const Intent &in) const {
  auto local = inversePoint(actorMatrix({}, in.heading, in.modelScale), in.actualVelocity),
       desired = inversePoint(actorMatrix({}, in.heading, in.modelScale), in.desiredVelocity);
  std::array<V3, 3> trajectory{};
  for (unsigned k = 0; k < 3; ++k) {
    float t = .2f * (k + 1);
    trajectory[k] = add(mul(local, t * .25f), mul(desired, t * .75f));
  }
  auto f = rawFeature(a, p, local, in.angularVelocity, trajectory);
  return quantize(f, scales);
}
Selection
MotionDatabase::search(const std::array<std::int16_t, MotionDimensions> &q,
                       unsigned tags, Selection continuing, bool bias,
                       std::uint64_t *second) const {
  Selection best = continuing;
  best.cost = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t runner = best.cost;
  for (const auto &f : frames) {
    if ((f.tags & tags) == 0)
      continue;
    std::uint64_t cost = 0;
    for (unsigned i = 0; i < q.size(); ++i) {
      std::int64_t d = std::int64_t(f.feature[i]) - q[i];
      unsigned weight = i < 9 ? 5 : (i >= 21 && i <= 22 ? 6 : 1);
      cost += std::uint64_t(d * d) * weight;
    }
    if (bias && (f.clip != continuing.clip ||
                 std::abs(int(f.frame) - int(continuing.frame)) > 3))
      cost += 180000;
    if (cost < best.cost) {
      runner = best.cost;
      best = {f.clip, f.frame, cost};
    } else
      runner = std::min(runner, cost);
  }
  if (second)
    *second = runner;
  return best;
}

void Controller::resetFeatures() {
  features_ = {};
  features_.lodWeight = config_.softTissue ? 1.f : 0.f;
  features_.soft.resize(asset_->softRegions.size());
  metrics_ = {};
  metrics_.databaseFrames = static_cast<unsigned>(database_.frames.size());
  deformation_.assign(asset_->vertices.size(), {});
}

void Controller::prepareRig() {
  stanceOffset_ = {};
  restFootRotation_ = {};
  restToeRotation_ = {};
  legReach_ = {};
  // The rig arms become contact chains before anything else can register
  // one, so their indices are stable: left = 0, right = 1. A rig whose legs
  // were rejected still gets hand contacts, which is why this runs before
  // the early return below.
  armChains_ = {-1, -1};
  contactChains_.clear();
  contactReach_.clear();
  for (unsigned side = 0; side < asset_->rig.arms.size() && side < 2; ++side)
    armChains_[side] = int(addContactChain(asset_->rig.arms[side]));
  if (asset_->rig.legs.empty())
    return;
  const auto restPose = asset_->skeleton.restPose();
  auto rest = asset_->skeleton.globals(restPose);
  const unsigned pelvis = asset_->rig.pelvis >= 0
                              ? unsigned(asset_->rig.pelvis)
                              : 0u;
  const V3 root = origin(rest[pelvis]);
  for (unsigned side = 0;
       side < asset_->rig.legs.size() && side < stanceOffset_.size(); ++side) {
    const auto &chain = asset_->rig.legs[side];
    const V3 hip = origin(rest[chain.root]),
             knee = origin(rest[chain.joint]),
             ankle = origin(rest[chain.tip]);
    stanceOffset_[side] = {ankle.x - root.x, 0, ankle.z - root.z};
    legReach_[side] = len(sub(knee, hip)) + len(sub(ankle, knee));
    // The rest sole is flat by construction. Aligning the foot back to this
    // global rotation (instead of assuming the bone's local +Y is up) keeps
    // imported rigs whose foot bone points along the toes grounded.
    restFootRotation_[side] =
        globalRotation(asset_->skeleton, restPose, chain.tip);
    if (chain.toe >= 0)
      restToeRotation_[side] =
          globalRotation(asset_->skeleton, restPose, unsigned(chain.toe));
  }
}
void Controller::resetFeet(bool teleport) {
  if (!features_.contacts.empty()) {
    if (teleport)
      for (const auto &c : features_.contacts)
        if (c.active)
          ++metrics_.releaseCounts[std::size_t(ContactRelease::Teleport)];
    // A hold belongs to a body that was standing somewhere. After a reset or a
    // teleport the old anchor is meaningless, and keeping it would drag the arm
    // across the world to a rail the character no longer touches.
    features_.contacts.clear();
  }
  if (teleport) {
    features_.feet = {};
    features_.steppingFoot = -1;
    features_.turnResidual = 0;
    features_.pelvisShift = {};
  } else {
    for (auto &foot : features_.feet) {
      if (foot.locked)
        foot.release = ContactRelease::Blend;
      foot.locked = false;
      foot.phase = FootPhase::Swing;
      foot.turnStep = false;
      foot.weight = 0;
      foot.mode = ContactMode::Free;
      foot.roll = 0;
      foot.pivotValid = false;
      foot.pivoting = false;
    }
    features_.steppingFoot = -1;
  }
}
void Controller::applyIdleMotion(const Intent &in, PoseBuffer &pose) {
  if (!config_.proceduralRig || !idleStatic_ || state_ != State::Idle ||
      features_.matching || !in.grounded)
    return;
  const auto &rig = asset_->rig;
  if (rig.pelvis < 0 && rig.spine < 0 && rig.lookBone < 0)
    return;
  // Ease in after stopping so the walk-to-idle blend stays clean.
  float ramp = std::clamp(float(stateTicks_) / (0.35f * TickRate), 0.f, 1.f);
  ramp = ramp * ramp * (3 - 2 * ramp);
  if (ramp <= 1e-4f)
    return;
  const float seconds = float(double(tick_) / TickRate);
  const float sway = std::sin(seconds * 2 * Pi / 3.4f);
  const float breath = std::sin(seconds * 2 * Pi / 2.7f + 1.1f);
  const float scan = std::sin(seconds * 2 * Pi / 7.3f + .4f);
  const float height = std::max(.1f, config_.rig.height);
  if (rig.pelvis >= 0) {
    const unsigned pelvis = unsigned(rig.pelvis);
    // Move the hips in model space; the foot solve keeps both soles planted.
    const int parent = asset_->skeleton.bones[pelvis].parent;
    const Quat parentRotation =
        parent >= 0 ? globalRotation(asset_->skeleton, pose, unsigned(parent))
                    : Quat{};
    const V3 lateral = rotateVector(inv(parentRotation), {1, 0, 0});
    const float amount = sway * height * .012f * ramp;
    pose.translation[pelvis][0] += lateral.x * amount;
    pose.translation[pelvis][1] += lateral.y * amount;
    pose.translation[pelvis][2] += lateral.z * amount;
    const float yaw = sway * .018f * ramp;
    rotateGlobal(asset_->skeleton, pose, pelvis,
                 {0, std::sin(yaw * .5f), 0, std::cos(yaw * .5f)});
  }
  if (rig.spine >= 0) {
    const float pitch = breath * .02f * ramp;
    rotateGlobal(asset_->skeleton, pose, unsigned(rig.spine),
                 {std::sin(pitch * .5f), 0, 0, std::cos(pitch * .5f)});
  }
  if (rig.lookBone >= 0 && !in.hasLookTarget) {
    const float yaw = scan * .028f * ramp;
    rotateGlobal(asset_->skeleton, pose, unsigned(rig.lookBone),
                 {0, std::sin(yaw * .5f), 0, std::cos(yaw * .5f)});
  }
}
unsigned Controller::addContactChain(const RigChain &chain) {
  require(chain.root < asset_->skeleton.bones.size() &&
              chain.joint < asset_->skeleton.bones.size() &&
              chain.tip < asset_->skeleton.bones.size(),
          "Contact chain bone out of range");
  const auto rest = asset_->skeleton.globals(asset_->skeleton.restPose());
  const float reach = len(sub(origin(rest[chain.joint]), origin(rest[chain.root]))) +
                      len(sub(origin(rest[chain.tip]), origin(rest[chain.joint])));
  require(reach > 1e-4f, "Contact chain has no length");
  contactChains_.push_back(chain);
  contactReach_.push_back(reach);
  return static_cast<unsigned>(contactChains_.size()) - 1;
}
bool Controller::setContact(unsigned id, int chain, V3 worldPoint, V3 normal,
                            float priority, std::uint64_t expires,
                            bool allowSlip) {
  if (!initialized_ || chain < 0 || chain >= int(contactChains_.size()))
    return false;
  require(finite(worldPoint) && finite(normal) && std::isfinite(priority) &&
              priority >= 0 && priority <= 1,
          "Invalid contact request");
  auto it = features_.contacts.begin();
  for (; it != features_.contacts.end(); ++it)
    if (it->id == id)
      break;
  if (it == features_.contacts.end()) {
    ContactConstraint fresh;
    fresh.id = id;
    features_.contacts.push_back(fresh);
    it = features_.contacts.end() - 1;
  }
  it->active = true;
  it->chain = chain;
  it->priority = priority;
  it->validUntil = expires;
  it->normal = unit(normal);
  // Changing the slip rule changes what the anchor means, so the hold has to
  // bind again instead of inheriting a pin (or a drag) it no longer wants.
  const bool slipChanged = it->slip != allowSlip;
  it->slip = allowSlip;
  // A hold is bound once, then follows its surface. Re-issuing the same id is the
  // normal way to keep it alive, so only a request that actually moved the point
  // re-binds; otherwise a caller refreshing every frame would drag the anchor.
  const bool moved =
      slipChanged || !it->bound || len(sub(it->pose, worldPoint)) > .01f;
  if (moved) {
    it->pose = worldPoint;
    it->bound = false;
    it->measured = false;
  }
  return true;
}
void Controller::releaseContact(unsigned id, ContactRelease reason) {
  for (auto &c : features_.contacts)
    if (c.id == id && c.active) {
      c.active = false;
      c.release = reason;
      ++metrics_.releaseCounts[std::size_t(reason)];
    }
}
void Controller::updateContacts(const Intent &in, const PoseBuffer &pose,
                                V3 &pelvisNeed, float &pelvisWeight) {
  metrics_.activeContacts = 0;
  metrics_.contactError = 0;
  metrics_.holdSlip = 0;
  if (features_.contacts.empty())
    return;
  const float scale = std::max(1e-4f, in.modelScale);
  const auto actor = actorMatrix(in.position, in.heading, scale);
  const auto globals = asset_->skeleton.globals(pose);
  // Expired holds and finished fades leave the list; ids are the caller's, so a
  // later re-issue creates a fresh entry rather than reviving a stale anchor.
  for (std::size_t i = 0; i < features_.contacts.size();) {
    auto &c = features_.contacts[i];
    if (c.active && c.validUntil && tick_ > c.validUntil) {
      c.active = false;
      c.release = ContactRelease::Expired;
      ++metrics_.releaseCounts[std::size_t(ContactRelease::Expired)];
    }
    if (!c.active && c.weight <= 0) {
      features_.contacts.erase(features_.contacts.begin() +
                               static_cast<std::ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
  for (auto &c : features_.contacts) {
    if (!c.active) {
      c.weight = std::max(0.f, c.weight - Dt * 6.f);
      continue;
    }
    const auto &chain = contactChains_[std::size_t(c.chain)];
    const float reach = contactReach_[std::size_t(c.chain)];
    if (!c.bound) {
      // Only bind to the ground when the requested point actually sits on it. A
      // rail at hip height or a hold on a wall is not "the surface under the
      // foot"; snapping those to the ground would drop the hand to the floor.
      c.anchor = c.pose;
      c.surface = WorldContactSurface;
      // A contact that is allowed to slip is not anchored to anything: its
      // point travels with the body, so binding it to the ground would turn
      // the intended drag into a pin.
      if (groundQuery_ && !c.slip) {
        const float probe = std::max(.2f, config_.rig.height * .5f) * scale;
        const GroundHit hit =
            groundQuery_(add(c.pose, mul(c.normal, probe)), probe * 2.f);
        ++metrics_.groundQueries;
        const float gap = len(sub(hit.point, c.pose));
        if (hit.hit && hit.normal.y >= config_.rig.maxSlopeCosine &&
            gap <= .05f * scale) {
          c.surface = hit.surface;
          c.anchor = inversePoint(hit.surfaceToWorld, c.pose);
          c.pose = point(hit.surfaceToWorld, c.anchor);
        }
      }
      c.bound = true;
      c.previous = point(actor, origin(globals[chain.tip]));
      c.measured = false;
      c.weight = 0;
    } else if (groundQuery_ && c.surface != WorldContactSurface) {
      // Follow the surface: a platform that moves carries the hand with it.
      const float probe = std::max(.2f, config_.rig.height * .5f) * scale;
      const GroundHit hit =
          groundQuery_(add(c.pose, mul(c.normal, probe)), probe * 2.f);
      ++metrics_.groundQueries;
      if (hit.hit && hit.surface == c.surface)
        c.pose = point(hit.surfaceToWorld, c.anchor);
    }
    const V3 tip = point(actor, origin(globals[chain.tip]));
    const float required =
        len(sub(inversePoint(actor, c.pose), origin(globals[chain.root])));
    // A contact that is allowed to slip is not constrained to the point, so it
    // can never overreach: the limb stays with the clip and the contact just
    // reports where it touches.
    if (!c.slip && required > reach * 1.02f) {
      c.active = false;
      c.release = ContactRelease::Overreach;
      ++metrics_.releaseCounts[std::size_t(ContactRelease::Overreach)];
      if (features_.fallback == RigFallback::None)
        features_.fallback = RigFallback::Unreachable;
      continue;
    }
    c.weight = std::min(1.f, c.weight + Dt * 8.f);
    if (c.weight > .98f)
      c.weight = 1.f;
    c.release = ContactRelease::None;
    ++metrics_.activeContacts;
    // A contact pulls the hips only once the limb is close to straight, and only
    // as far as its priority says. That is how a hand on a rail and a planted
    // foot share one pelvis without one of them silently winning.
    const float stretch = smoothstep(.9f, 1.f, required / std::max(1e-4f, reach));
    // A slipping contact never pulls the hips: the hand is supposed to move
    // along the surface, and letting it vote would drag the body with it.
    const float pull = c.slip ? 0.f : c.priority * stretch * c.weight;
    if (pull > 1e-4f) {
      const V3 need = sub(c.pose, tip);
      pelvisNeed = add(pelvisNeed, {need.x * pull, 0, need.z * pull});
      pelvisWeight += pull;
    }
  }
  for (auto &c : features_.contacts)
    if (!c.active && c.weight > 0)
      c.weight = std::max(0.f, c.weight - Dt * 6.f);
}
void Controller::solveContacts(const Intent &in, PoseBuffer &pose) {
  if (features_.contacts.empty())
    return;
  const float scale = std::max(1e-4f, in.modelScale);
  const auto actor = actorMatrix(in.position, in.heading, scale);
  for (auto &c : features_.contacts) {
    if (c.weight <= .001f)
      continue;
    const auto &chain = contactChains_[std::size_t(c.chain)];
    if (c.slip) {
      // No position constraint. The limb keeps following the clip and the
      // contact only records where it currently touches, which is what a palm
      // dragging along a wall does. Solving it would either pin the hand or
      // chase a target that runs away from the body.
      c.pose = point(actor, origin(asset_->skeleton.globals(pose)[chain.tip]));
      c.previous = c.pose;
      c.measured = true;
      c.error = 0;
      continue;
    }
    const auto before = pose;
    auto globals = asset_->skeleton.globals(pose);
    const V3 hip = origin(globals[chain.root]);
    const V3 target = inversePoint(actor, c.pose);
    const auto result = solveTwoBone(asset_->skeleton, pose, chain, target,
                                     add(hip, chain.pole));
    metrics_.ikError = std::max(metrics_.ikError, result.error);
    BoneMask mask;
    mask.set(chain.root);
    mask.set(chain.joint);
    mask.set(chain.tip);
    pose = blend(before, pose, unsigned(asset_->skeleton.bones.size()),
                 c.weight, mask);
    globals = asset_->skeleton.globals(pose);
    const V3 tip = point(actor, origin(globals[chain.tip]));
    c.error = len(sub(tip, c.pose));
    // Steady holds only. While the weight ramps the bone is still partly where
    // the clip put it, which is a hand-over cost, not a tracking error.
    if (c.weight > .98f) {
      metrics_.contactError = std::max(metrics_.contactError, c.error);
      // A pinned hold that moves is slipping. Measured at the contacting
      // bone, and only across two solved ticks, so a re-bind cannot report
      // the jump from the clip pose as a slide.
      if (!c.slip && c.measured)
        metrics_.holdSlip =
            std::max(metrics_.holdSlip, len(sub(tip, c.previous)) / Dt);
    }
    c.previous = tip;
    c.measured = true;
  }
}
void Controller::correct(const Intent &in, PoseBuffer &pose) {
  metrics_.lockedFeet = 0;
  metrics_.footSlip = metrics_.penetration = metrics_.ikError = 0;
  metrics_.groundQueries = metrics_.swingCount = metrics_.turnSteps =
      metrics_.ikClamped = 0;
  metrics_.turnResidual = 0;
  metrics_.anchorError = metrics_.contactSlip = metrics_.pivotSlip = 0;
  metrics_.soleAngle = metrics_.switchJump = metrics_.pelvisShiftUsed = 0;
  metrics_.supportSwitches = metrics_.toeOffs = metrics_.heelStrikes = 0;
  metrics_.rollLimited = 0;
  metrics_.releaseCounts = {};
  features_.fallback = RigFallback::None;
  const std::uint64_t stateTransitionTicks =
      std::uint64_t(config_.transitionSeconds * TickRate);
  if (stateTransitionActive_ &&
      tick_ - stateTransitionTick_ >= stateTransitionTicks)
    stateTransitionActive_ = false;
  // Runs before the foot solve so the planted feet absorb the idle sway.
  applyIdleMotion(in, pose);
  if (!config_.proceduralRig || asset_->rig.legs.empty()) {
    features_.fallback = RigFallback::Disabled;
    return;
  }
  const auto &rig = asset_->rig;
  const auto &settings = config_.rig;
  const float scale = std::max(1e-4f, in.modelScale);
  const auto actor = actorMatrix(in.position, in.heading, scale);
  auto globals = asset_->skeleton.globals(pose);
  const auto &clip = asset_->clips[selection_.clip];
  float phase =
      std::fmod(float(features_.matching ? features_.matchedSeconds
                                         : playbackSeconds()),
                float(clip.duration())) /
      float(clip.duration());
  // Keep the phase in [0,1): at the exact loop endpoint a strict range test can
  // otherwise report both feet unsupported, which spikes the pelvis and IK.
  phase -= std::floor(phase);
  const float headingDelta =
      features_.hasPreviousPosition
          ? std::remainder(in.heading - features_.previousHeading, 2 * Pi)
          : 0;
  const bool teleport =
      features_.hasPreviousPosition &&
      (len(sub(in.position, features_.previousPosition)) > 1.f ||
       std::abs(headingDelta) > 1.8f);
  if (teleport) {
    for (const auto &foot : features_.feet)
      if (foot.locked)
        ++metrics_.releaseCounts[std::size_t(ContactRelease::Teleport)];
    resetFeet(true);
    features_.pelvisOffset = 0;
    features_.pelvisShift = {};
    features_.fallback = RigFallback::Teleport;
  }
  if (!in.grounded) {
    for (const auto &foot : features_.feet)
      if (foot.locked)
        ++metrics_.releaseCounts[std::size_t(ContactRelease::GroundLost)];
    resetFeet(false);
    for (auto &foot : features_.feet)
      foot.release = ContactRelease::GroundLost;
    features_.pelvisOffset += (0 - features_.pelvisOffset) * .25f;
    features_.pelvisShift = mul(features_.pelvisShift, .75f);
    if (rig.pelvis >= 0)
      pose.translation[unsigned(rig.pelvis)][1] += features_.pelvisOffset / scale;
    features_.fallback = RigFallback::NotGrounded;
    if (in.hasLookTarget && rig.lookBone >= 0)
      solveLookAt(asset_->skeleton, pose, unsigned(rig.lookBone),
                  inversePoint(actor, in.lookTarget));
    return;
  }

  const float probeUp = std::max(.12f, settings.height * .25f) * scale;
  const float probeDown = std::max(.35f, settings.height * .7f) * scale;
  const unsigned count = static_cast<unsigned>(rig.legs.size());
  // During a state blend the pose is somewhere between the previous and the
  // next gait; anchoring it produces a visible drag. Let the animation carry
  // both feet until the transition settles. Motion-matching clip switches do
  // not count as state transitions.
  const std::uint64_t transitionTicks =
      std::uint64_t(config_.transitionSeconds * TickRate);
  const bool blending =
      stateTransitionActive_ && tick_ - stateTransitionTick_ < transitionTicks;
  std::array<V3, 2> world{};
  std::array<GroundHit, 2> hits{};
  std::array<bool, 2> walkable{};
  std::array<bool, 2> contact{};
  // Ankle drawn below the surface it is stepping onto (a riser taller than the
  // clip's foot lift); carried to the landing solve below.
  std::array<bool, 2> under{};
  // Yaw the foot target still owes the body after the pivot limit, per side.
  std::array<float, 2> yawExcess{};
  // Flat sole target of every active contact. The pelvis solve reads these, not
  // the rolled ankle target: rolling over the toe raises the ankle by design and
  // must not be mistaken for the body needing to rise.
  std::array<V3, 2> groundTargets{};
  // Why a foot that was locked stops being locked, decided where the test that
  // failed is still in scope instead of being guessed later.
  std::array<ContactRelease, 2> pendingRelease{};
  // A contact only votes on the pelvis once this tick produced a sole target for
  // it. A turn step that lands mid-tick has an anchor but no solved target yet.
  std::array<bool, 2> targetValid{};
  // Contact mode at the start of the tick. Counters and the slip measurement
  // compare against this rather than against the pre-budget mode of the target
  // loop, which no longer decides the mode.
  std::array<ContactMode, 2> modeBefore{};
  // Authored contact windows are per foot and do not have to tile the cycle. A
  // walk whose windows leave a gap (the bundled robot covers 64%) would lose the
  // foot solve entirely for the uncovered third of every stride, which reads as
  // the legs dangling. When no foot is inside any window the geometry decides
  // instead; a real flight phase still fails the plant test.
  std::array<bool, 2> windowOpen{};
  bool anyWindow = false;
  if (clip.contactKnown())
    for (unsigned side = 0; side < count; ++side) {
      const auto range = clip.contacts[side];
      const bool open = range[0] <= range[1]
                            ? phase >= range[0] && phase < range[1]
                            : phase >= range[0] || phase < range[1];
      windowOpen[side] = open;
      anyWindow = anyWindow || open;
    }
  for (unsigned side = 0; side < count; ++side) {
    const auto &chain = rig.legs[side];
    auto &foot = features_.feet[side];
    foot.cooldown = std::max(0.f, foot.cooldown - Dt);
    foot.pivoting = false;
    modeBefore[side] = foot.mode;
    world[side] = point(actor, origin(globals[chain.tip]));
    if (groundQuery_) {
      hits[side] =
          groundQuery_(add(world[side], {0, probeUp, 0}), probeUp + probeDown);
      ++metrics_.groundQueries;
    }
    walkable[side] =
        hits[side].hit && hits[side].normal.y >= settings.maxSlopeCosine;
    bool wanted = in.speed < .12f;
    if (clip.contactKnown())
      wanted = !anyWindow || windowOpen[side];
    wanted = wanted && settings.footLock && !blending;
    if (!walkable[side]) {
      if (foot.locked)
        ++metrics_.releaseCounts[std::size_t(hits[side].hit
                                                 ? ContactRelease::Unwalkable
                                                 : ContactRelease::GroundLost)];
      contact[side] = false;
      foot.contact = false;
      foot.normal = unit(hits[side].normal);
      foot.animated = world[side];
      continue;
    }
    const V3 normal = unit(hits[side].normal);
    // Height of the ankle above its own sole plane, measured in model space.
    // Contact is a property of the pose, not of where the body happens to be: a
    // step-up raises the whole skeleton, and reading the world height there
    // released the support foot on every riser and left both feet swinging.
    const float above = (origin(globals[chain.tip]).y - chain.soleHeight) * scale;
    const float sole = chain.soleHeight * scale;
    // Contact is decided from what the clip actually draws: the ankle has to be
    // on the surface *and* standing still in the world. Planting on the authored
    // phase window alone pins the foot while the clip is still swinging, and the
    // pin then drags the leg behind the body for the rest of the window.
    const V3 travel = features_.hasPreviousPosition
                          ? sub(world[side], foot.animated)
                          : V3{};
    const float ankleSpeed =
        std::sqrt(travel.x * travel.x + travel.z * travel.z) / Dt;
    const float plantTolerance = std::max(.02f, sole * .3f);
    const bool nearGround = above <= plantTolerance;
    // A riser taller than the clip's foot lift draws the ankle *below* the tread
    // the character is stepping onto. Treat that as contact: the solve then lifts
    // the shoe onto the tread in one stride instead of clipping through the edge
    // (and the ankle stops sliding through the step face).
    const bool underSurface =
        dot(sub(world[side], hits[side].point), normal) < sole;
    under[side] = underSurface;
    // A retimed clip keeps its support foot still in the world, so a slide of a
    // metre per second or less is the plant. Content without an inferred
    // locomotion speed is played at its authored rate and slides by construction
    // there; holding it to the same bar would never plant, so the bar follows the
    // retime error instead of a constant.
    const float plantSpeedLimit =
        1.f + std::abs(in.speed - playbackRate_ * clip.locomotionSpeed * scale);
    const bool planted = ankleSpeed <= plantSpeedLimit;
    // A planted foot stays planted while the clip keeps it on the surface and
    // the body does not carry the leg away from its anchor. Both failures are
    // what make a support leg trail behind the body, stretch and float.
    bool keep = false;
    // Horizontal distance between the anchor and the foot the clip draws. It is
    // the drag the pin would impose, so it bounds every way of staying locked —
    // including a foot drawn inside a tread, which must not keep its pin while
    // the body walks away from it.
    float drift = 0;
    const float maxDrift =
        std::max(.06f, std::min(settings.maxFootCorrection, .3f)) * scale;
    // How far the pinned ankle is from its hip, in model units. This decides
    // when the stance ends: the body walking past a planted foot is what pulls
    // the leg straight, and once it cannot reach any more the IK clamp holds the
    // pin while the ankle rides up the leg — the backwards slide. Releasing a
    // tick early costs nothing visible and keeps the sole where it was planted.
    float anchorReach = 0;
    if (foot.locked) {
      const V3 pinned = point(hits[side].surfaceToWorld, foot.anchor);
      const V3 gap = sub(pinned, world[side]);
      drift = std::sqrt(gap.x * gap.x + gap.z * gap.z);
      anchorReach =
          len(sub(inversePoint(actor, pinned), origin(globals[chain.root])));
    }
    if (foot.locked && !blending && foot.surface == hits[side].surface) {
      // Height and drift are the release criteria. Speed is deliberately not
      // used here: a stance foot accelerates hard through toe-off while still on
      // the ground, and releasing on speed alone chatters the IK every step.
      const float approach = len(in.actualVelocity) * Dt / scale;
      // Only a stride walks the body past a planted foot. Standing (and any
      // content that fakes a moving surface under an idle pose) keeps its pin
      // until the existing height and drift tests end it.
      const bool gait = state_ == State::Walk || state_ == State::Run;
      const bool stretching =
          gait && anchorReach > foot.reachAtPlant + .02f &&
          anchorReach + approach > legReach_[side] * .99f;
      keep = above <= plantTolerance * 2.5f && drift <= maxDrift && !stretching;
      if (!keep)
        pendingRelease[side] = above > plantTolerance * 2.5f
                                  ? ContactRelease::Lift
                                  : (stretching ? ContactRelease::Stride
                                                : ContactRelease::Drift);
    }
    const bool intended = wanted;
    // Acquisition and maintenance are separate: a foot that is already locked is
    // governed by the hysteresis above, so a clip that keeps drawing it at sole
    // height while sweeping it forward cannot re-plant it every tick and drag
    // the leg along. A released foot re-pins at its new spot on the next tick.
    wanted = keep || (!foot.locked && intended && nearGround && planted) ||
             (underSurface && settings.footLock && !blending &&
              (!foot.locked || drift <= maxDrift));
    contact[side] = wanted;
    // Reported contact is the clip's intent; `locked` is what the solver actually
    // anchors. Content whose authored foot arc never reaches the surface reports
    // contact but never gets pinned, which is what keeps the leg from stretching.
    foot.contact = intended || wanted;
    foot.normal = normal;
    foot.animated = world[side];
  }

  // Turn-in-place residual: how far the body has rotated since each support
  // foot was planted. The anchor vector itself swings behind the body during
  // ordinary forward locomotion and must not be used here.
  float maxError = 0;
  float maxDemand = 0;
  int candidate = -1;
  float candidateError = 0;
  std::array<float, 2> sideError{};
  // Gameplay holds the body still while both feet are planted, so the step has
  // to be decided from the facing the body is being steered toward. Without it
  // the model starts rotating first and the support foot is twisted (slides)
  // before a foot ever leaves the ground.
  const float facing = in.hasTurnTarget ? in.turnTarget : in.heading;
  const float pivotLimit = std::min(settings.turnThreshold, settings.maxTwist);
  // A foot that is stepping cannot be released: the step is already carrying it
  // and the other foot has to keep supporting. When the body outruns the step
  // the stepping foot is landed early instead (see below).
  const bool stepInFlight = features_.steppingFoot >= 0;
  for (unsigned side = 0; side < count; ++side) {
    auto &foot = features_.feet[side];
    if (!foot.locked || !walkable[side] || foot.turnStep)
      continue;
    const float residual =
        std::remainder(in.heading - foot.anchorHeading, 2 * Pi);
    const float error = std::abs(residual);
    const float demand =
        std::abs(std::remainder(facing - foot.anchorHeading, 2 * Pi));
    sideError[side] = error;
    maxError = std::max(maxError, error);
    maxDemand = std::max(maxDemand, demand);
    // A planted foot must never stay twisted past the hard limit: the two-bone
    // solve would rotate the thigh through the other leg. Release the anchor and
    // let the clip carry the leg; the next contact re-plants it under the new
    // heading. This is what keeps a fast mouse flick from crossing the legs.
    if (error > settings.maxTwist) {
      if (!stepInFlight) {
        if (foot.locked)
          ++metrics_.releaseCounts[std::size_t(ContactRelease::Turn)];
        contact[side] = false;
        foot.contact = false;
        foot.locked = false;
        foot.phase = FootPhase::Swing;
        foot.release = ContactRelease::Turn;
      }
      if (features_.fallback == RigFallback::None)
        features_.fallback = RigFallback::Unreachable;
      continue;
    }
    // Below the hard limit the planted foot pivots with the body: the ankle
    // target is yawed by whatever exceeds the pivot limit, so the visible twist
    // never passes the point where the knee would swing across.
    yawExcess[side] = residual - std::clamp(residual, -pivotLimit, pivotLimit);
    if (foot.cooldown > 0)
      continue;
    if (demand > candidateError) {
      candidate = int(side);
      candidateError = demand;
    }
  }
  features_.turnResidual = metrics_.turnResidual = maxError;
  // A held body does not rotate at all until a foot lifts, so the demand has to
  // count as "turning" as well or the step would never start.
  const bool turning = std::abs(in.angularVelocity) > .8f ||
                       std::abs(headingDelta) > .012f ||
                       maxDemand > settings.turnThreshold;
  const float walkingTurnLimit = std::max(.25f, config_.walkSpeed * .5f);
  // While moving, in-place stepping is not appropriate. Release a support foot
  // that has been twisted past the configured limit so the animation carries the
  // leg instead of the IK crossing it through the body. The next contact window
  // re-plants it under the new heading.
  if (turning && in.speed >= walkingTurnLimit) {
    for (unsigned side = 0; side < count; ++side)
      if (features_.feet[side].locked &&
          sideError[side] > settings.turnThreshold) {
        contact[side] = false;
        features_.feet[side].contact = false;
        features_.feet[side].locked = false;
        features_.feet[side].release = ContactRelease::Turn;
        ++metrics_.releaseCounts[std::size_t(ContactRelease::Turn)];
      }
  }
  if (settings.turnInPlace && turning && in.speed < walkingTurnLimit &&
      candidate >= 0 && maxDemand > settings.turnThreshold &&
      features_.steppingFoot < 0) {
    const int other = 1 - candidate;
    const auto &support = features_.feet[other];
    if (support.locked && support.phase == FootPhase::Support &&
        !support.turnStep) {
      const auto &chain = rig.legs[unsigned(candidate)];
      // Place the foot for the facing the body will have when it lands, not for
      // the one it is leaving.
      V3 desired = add(in.position,
                       rotateY(mul(stanceOffset_[unsigned(candidate)], scale),
                               facing));
      desired.y = in.position.y;
      const Landing landing = sampleLanding(
          groundQuery_, desired, rotateY({0, 0, -1}, facing),
          chain.footLength * scale, probeUp * 1.6f, probeDown * 1.4f,
          settings.maxSlopeCosine, settings.maxStepHeight * scale,
          chain.soleHeight * scale, metrics_.groundQueries);
      if (landing.valid) {
        auto &foot = features_.feet[unsigned(candidate)];
        foot.locked = false;
        foot.surface = 0;
        foot.phase = FootPhase::Swing;
        foot.turnStep = true;
        foot.swingStart = world[unsigned(candidate)];
        foot.swingEnd = landing.target;
        foot.target = foot.swingStart;
        foot.targetNormal = landing.normal;
        foot.swingTime = 0;
        foot.swingDuration = settings.turnStepDuration;
        foot.cooldown = settings.turnCooldown;
        foot.weight = 1;
        foot.contact = false;
        // Heading at lift-off. The swing is cut short when the body rotates
        // further than one step can absorb, so a fast turn never leaves the
        // support leg twisting while the swinging leg is still in the air.
        foot.anchorHeading = in.heading;
        features_.steppingFoot = candidate;
        ++metrics_.turnSteps;
        ++features_.turnSteps;
        contact[unsigned(candidate)] = false;
        contact[unsigned(other)] = true;
      } else {
        features_.fallback = RigFallback::NoLanding;
        features_.feet[unsigned(candidate)].cooldown = settings.turnCooldown;
      }
    } else if (features_.fallback == RigFallback::None)
      features_.fallback = RigFallback::Unreachable;
  }

  bool hasSupport = false;
  for (unsigned side = 0; side < count; ++side) {
    auto &foot = features_.feet[side];
    const auto &chain = rig.legs[side];
    if (foot.turnStep && features_.steppingFoot == int(side)) {
      foot.swingTime += Dt;
      const float t = std::clamp(
          foot.swingTime / std::max(.01f, foot.swingDuration), 0.f, 1.f);
      const float smooth = t * t * (3 - 2 * t);
      V3 target = add(foot.swingStart,
                      mul(sub(foot.swingEnd, foot.swingStart), smooth));
      target.y += std::sin(Pi * t) * settings.turnStepHeight * scale;
      foot.target = target;
      foot.normal = foot.targetNormal;
      foot.phase = FootPhase::Swing;
      foot.locked = false;
      // Land the step as soon as the body has turned further than the support
      // foot can hold. Without this a fast flick keeps one leg in the air while
      // the other is twisted past the limit, which is the crossed-leg pose.
      const float swung =
          std::abs(std::remainder(in.heading - foot.anchorHeading, 2 * Pi));
      const bool hurry =
          swung > std::max(.35f, settings.maxTwist - settings.turnThreshold);
      if (t >= 1.f || hurry) {
        GroundHit end;
        if (groundQuery_) {
          end = groundQuery_(add(foot.swingEnd, {0, probeUp, 0}),
                             probeUp + probeDown);
          ++metrics_.groundQueries;
        }
        if (end.hit && end.normal.y >= settings.maxSlopeCosine) {
          const V3 targetPoint =
              add(end.point,
                  mul(unit(end.normal), rig.legs[side].soleHeight * scale));
          foot.anchor = inversePoint(end.surfaceToWorld, targetPoint);
          foot.surface = end.surface;
          foot.locked = true;
          foot.previous = targetPoint;
          foot.anchorHeading = in.heading;
          foot.target = targetPoint;
          foot.flat = targetPoint;
          foot.reachAtPlant = len(sub(inversePoint(actor, targetPoint),
                                      origin(globals[chain.root])));
          foot.normal = unit(end.normal);
          foot.phase = FootPhase::Support;
          foot.turnStep = false;
          foot.mode = ContactMode::Flat;
          foot.roll = 0;
          foot.pivotValid = false;
          foot.release = ContactRelease::None;
          foot.weight = 1;
          foot.cooldown = settings.turnCooldown;
          hasSupport = true;
          contact[side] = true;
          groundTargets[side] = targetPoint;
          targetValid[side] = true;
          ++metrics_.supportSwitches;
        } else {
          features_.fallback = RigFallback::NoLanding;
          foot.turnStep = false;
          foot.locked = false;
          foot.phase = FootPhase::Swing;
          foot.mode = ContactMode::Free;
          foot.roll = 0;
          foot.release = ContactRelease::Unwalkable;
          foot.weight = 0;
          foot.cooldown = settings.turnCooldown;
        }
        if (features_.steppingFoot == int(side))
          features_.steppingFoot = -1;
      }
      continue;
    }
    if (!walkable[side]) {
      if (foot.locked)
        ++metrics_.releaseCounts[std::size_t(
            hits[side].hit ? ContactRelease::Unwalkable
                           : ContactRelease::GroundLost)];
      foot.release = hits[side].hit ? ContactRelease::Unwalkable
                                    : ContactRelease::GroundLost;
      foot.turnStep = false;
      foot.locked = false;
      foot.phase = FootPhase::Swing;
      foot.mode = ContactMode::Free;
      foot.roll = 0;
      foot.pivotValid = false;
      foot.target = world[side];
      foot.normal = {0, 1, 0};
      foot.weight = std::max(0.f, foot.weight - Dt * 18);
      if (features_.steppingFoot == int(side))
        features_.steppingFoot = -1;
      if (features_.fallback == RigFallback::None)
        features_.fallback = hits[side].hit ? RigFallback::Unwalkable
                                            : RigFallback::NoGround;
      continue;
    }
    const V3 groundTarget =
        add(hits[side].point,
            mul(unit(hits[side].normal),
                rig.legs[side].soleHeight * scale));
    // Predict reachability before pinning. A pin the leg cannot hold either
    // drags the leg straight or is released a few ticks later, and both read as
    // a slide. The test is made against the unassisted leg length: pelvis
    // compensation is a stabiliser for contacts that already exist, not a way to
    // force a pin on content whose clip puts the foot out of reach.
    if (!foot.locked) {
      const V3 hip = origin(globals[chain.root]);
      const V3 wantedLocal = inversePoint(actor, groundTarget);
      const float reachNeed = len(sub(wantedLocal, hip));
      // A small margin covers a stance leg that is straight by construction; the
      // settled-contact release above still leaves room for pelvis help. The test
      // has to stay loose: the clip touches down before the leg is loaded, and a
      // pin refused here is a foot that never plants at all.
      if (reachNeed > legReach_[side] * 1.01f + .01f * scale) {
        contact[side] = false;
        foot.contact = false;
        foot.phase = FootPhase::Swing;
        foot.mode = ContactMode::Free;
        foot.roll += (0 - foot.roll) * .5f;
        foot.pivotValid = false;
        // The pin was refused, so the clip carries this foot. Drop the IK weight
        // with it, or the stale target keeps dragging the shoe through the air
        // towards a point the solver no longer believes in.
        foot.weight = std::max(0.f, foot.weight - Dt * 18);
        foot.normal = unit(hits[side].normal);
        foot.animated = world[side];
        foot.target = world[side];
        foot.flat = world[side];
        if (features_.fallback == RigFallback::None)
          features_.fallback = RigFallback::Unreachable;
        continue;
      }
    }
    if (contact[side]) {
      const bool wasLocked = foot.locked;
      // Reset evaluates the graph before gameplay supplies the model scale.
      // Re-anchor on the first tick when the locked target no longer matches
      // the measured sole height, otherwise a scaled character keeps its feet
      // buried. Later ticks keep the anchor so step edges stay stable.
      const float heightError =
          foot.locked && tick_ <= 1
              ? std::abs(dot(sub(foot.target, groundTarget),
                             unit(hits[side].normal)))
              : 0.f;
      if (!wasLocked) {
        // Hand-over continuity: the pin has to engage where the clip already
        // draws the foot. Anything larger is the pop a viewer sees the moment
        // the foot is taken over, so it is measured in world units.
        if (features_.hasPreviousPosition)
          metrics_.switchJump = std::max(
              metrics_.switchJump, len(sub(groundTarget, world[side])));
        ++metrics_.supportSwitches;
      }
      if (!wasLocked || foot.surface != hits[side].surface ||
          heightError > .02f * scale) {
        foot.anchor = inversePoint(hits[side].surfaceToWorld, groundTarget);
        foot.surface = hits[side].surface;
        foot.previous = groundTarget;
        foot.locked = true;
        foot.anchorHeading = in.heading;
        // Reference length for the stance budget: how far the hip already was
        // from the fresh anchor. A stance ends when the body adds to it.
        foot.reachAtPlant = len(sub(inversePoint(actor, groundTarget),
                                    origin(globals[chain.root])));
        // A contact that is established or moved this tick has no previous sole
        // position to compare against. Keeping the old pivot reported the
        // re-plant — a turn step placing the foot somewhere else on purpose — as
        // a ten-metre-per-second slide.
        foot.pivotValid = false;
      }
      foot.target = point(hits[side].surfaceToWorld, foot.anchor);
      // Pivot the planted foot with the body beyond the twist limit so the leg
      // never has to rotate across the midline inside the pelvis. The contact
      // point rotates on purpose, so the motion it produces is classified as an
      // intentional pivot instead of being counted as slip.
      if (yawExcess[side] != 0.f) {
        const V3 relative = sub(foot.target, in.position);
        foot.target = add(in.position, rotateY(relative, yawExcess[side]));
        foot.pivoting = true;
      }
      // Standing still: work off a leftover twist by pivoting the foot on the
      // spot a few degrees at a time. Otherwise the leg keeps whatever twist the
      // last turn left behind for as long as the character stands there.
      // Recomputed from the (possibly just refreshed) anchor so a foot that
      // re-planted this tick never gets an extra twist applied to it.
      const float leftover = std::remainder(in.heading - foot.anchorHeading, 2 * Pi);
      if (std::abs(leftover) > .01f && std::abs(in.angularVelocity) < .35f &&
          std::abs(leftover) < settings.maxTwist) {
        const float step = std::clamp(leftover, -3.f * Dt, 3.f * Dt);
        const V3 pivot =
            add(in.position, rotateY(sub(foot.target, in.position), step));
        foot.anchor = inversePoint(hits[side].surfaceToWorld, pivot);
        foot.anchorHeading = std::remainder(foot.anchorHeading + step, 2 * Pi);
        foot.target = pivot;
        foot.pivoting = true;
      }
      foot.normal = unit(hits[side].normal);
      // Contact mode follows the authored contact window: heel first, flat
      // through stance, toe last. The ankle target is rotated about the active
      // edge, so the edge stays on the ground while the shoe pitches over it —
      // the difference between a toe-off and a sole welded to the floor.
      float wantRoll = 0;
      const float progress = windowProgress(clip, side, phase);
      // Rolling the sole is a gait event, not a standing pose: an idle clip that
      // declares contact for its whole loop must not send the character up onto
      // its toes every cycle.
      const bool gait = state_ == State::Walk || state_ == State::Run;
      if (settings.soleRoll > 1e-4f && progress >= 0 && !blending && gait) {
        if (progress > .72f)
          wantRoll = settings.soleRoll * smoothstep(.72f, 1.f, progress);
        else if (progress < .18f)
          wantRoll = -settings.soleRoll * .35f * smoothstep(.18f, 0.f, progress);
      }
      foot.roll += (wantRoll - foot.roll) * .35f;
      if (std::abs(foot.roll) < .008f)
        foot.roll = 0;
      // Flat target the roll is derived from, kept so the reach budget can
      // re-roll this tick's target without redoing the anchor and pivot work.
      foot.flat = foot.target;
      foot.target = rolledAnkle(foot.flat, foot.normal, in.heading, foot.roll,
                                chain.soleHeight, chain.footLength, scale);
      // The mode is classified after the reach budget below has had its say.
      // Deciding it here made the mode oscillate on a trimmed toe-off — the
      // target loop asked for Toe, the budget gave the pitch back — and every
      // oscillation restarted the slip measurement, which is how a real slide
      // stayed invisible.
      // The first tick after a reset has no pose history: snap the sole to the
      // ground instead of blending out of the unaligned bind pose.
      foot.weight = features_.hasPreviousPosition
                        ? std::min(1.f, foot.weight + Dt * 18)
                        : 1.f;
      // Snap the tail of the ramp. A foot at 0.99 is still blended one percent
      // toward the clip, and a clip played at three times speed moves the ankle
      // by centimetres per tick, so that percent is a visible slide on a foot
      // that is supposed to be nailed to its anchor.
      if (foot.weight > .98f)
        foot.weight = 1.f;
      foot.phase = foot.weight >= .999f ? FootPhase::Support
                                        : FootPhase::Landing;
      // A foot that the clip draws inside the tread it is stepping onto gets
      // lifted out immediately: a partial blend would leave the shoe buried in
      // the riser for the whole landing ramp.
      if (under[side]) foot.weight = std::max(foot.weight, .85f);
      foot.release = ContactRelease::None;
      ++metrics_.lockedFeet;
      hasSupport = true;
      groundTargets[side] = groundTarget;
      targetValid[side] = true;
    } else {
      if (foot.locked || foot.phase == FootPhase::Support) {
        const ContactRelease why =
            pendingRelease[side] != ContactRelease::None
                ? pendingRelease[side]
                : (blending ? ContactRelease::Blend : ContactRelease::Lift);
        if (foot.locked)
          ++metrics_.releaseCounts[std::size_t(why)];
        foot.release = why;
        foot.swingStart = foot.previous;
        foot.swingEnd = world[side];
        foot.swingTime = 0;
        foot.swingDuration = std::max(.08f, .18f);
        foot.turnStep = false;
        foot.phase = FootPhase::Release;
        foot.locked = false;
        ++metrics_.swingCount;
      } else if (foot.phase == FootPhase::Release)
        foot.phase = FootPhase::Swing;
      // The animation is the primary swing source. Earlier velocity-based
      // prediction dragged swing feet ahead of the body at run speed and made
      // the legs trail the character. The re-timed playback now keeps the
      // landing point aligned by construction.
      foot.target = world[side];
      foot.normal = unit(hits[side].normal);
      foot.weight = std::max(0.f, foot.weight - Dt * 18);
      foot.phase = FootPhase::Swing;
      foot.mode = ContactMode::Free;
      foot.roll += (0 - foot.roll) * .5f;
      if (std::abs(foot.roll) < .008f)
        foot.roll = 0;
      foot.pivotValid = false;
      foot.pivoting = false;
    }
  }

  // Shared pelvis solve. Every active contact votes on where the hips have to
  // be: vertical takes the deepest requirement (a foot that needs the hips down
  // wins), horizontal the average pull of the anchors. Solving both together is
  // what stops one leg from claiming the pelvis and biasing double support
  // toward that side.
  float verticalNeed = 0;
  V3 horizontalNeed{};
  float overrun = 0, worstReach = 0, voteWeight = 0;
  unsigned votes = 0;
  if (hasSupport)
    for (unsigned side = 0; side < count; ++side) {
      if (!contact[side] || !targetValid[side])
        continue;
      // The vote is measured against the pinned contact target, not against the
      // ground point under the animated ankle. That point sits directly below the
      // clip's foot by construction, so its horizontal difference was always
      // zero and the horizontal compensation never engaged — the leg then
      // over-extended and the sole slid instead of the pelvis following it.
      const V3 need = sub(features_.feet[side].flat, world[side]);
      verticalNeed = votes == 0 ? need.y : std::min(verticalNeed, need.y);
      ++votes;
      if (legReach_[side] <= 1e-5f)
        continue;
      const V3 hip = origin(globals[rig.legs[side].root]);
      const float required =
          len(sub(inversePoint(actor, features_.feet[side].target), hip));
      overrun = std::max(overrun, required - legReach_[side]);
      worstReach = std::max(worstReach, required / legReach_[side]);
      // Only a leg that is close to full extension has a say. A plain average
      // over both feet cancels itself out in double support — the trailing foot
      // wants the hips back, the leading foot wants them forward — and the hips
      // then stay put while the trailing leg over-extends and the sole slides.
      // Standing is excluded: idle sway is body motion the feet are supposed to
      // absorb, and a rig that stands on straight legs would otherwise read as
      // permanently over-extended and have its sway cancelled by the hips.
      const bool gait = state_ == State::Walk || state_ == State::Run;
      const float stretch =
          gait ? smoothstep(.93f, 1.f, required / legReach_[side]) : 0.f;
      horizontalNeed =
          add(horizontalNeed, {need.x * stretch, 0, need.z * stretch});
      voteWeight += stretch;
    }
  // Generic contacts vote in the same solve: a hand on a rail pulls the hips the
  // same way a stretched leg does, scaled by its priority.
  updateContacts(in, pose, horizontalNeed, voteWeight);
  if (votes || voteWeight > 1e-4f) {
    horizontalNeed = voteWeight > 1e-4f
                         ? mul(horizontalNeed, 1.f / voteWeight)
                         : V3{};
    // The hips only travel horizontally to keep a leg reachable. Doing it all
    // the time would make the body lag its collision box on every stride.
    horizontalNeed = mul(horizontalNeed, smoothstep(.93f, 1.f, worstReach));
    const float limit = std::max(0.f, settings.maxPelvisShift) * scale;
    const float magnitude = len(horizontalNeed);
    if (magnitude > limit && magnitude > 1e-6f)
      horizontalNeed = mul(horizontalNeed, limit / magnitude);
    // Lowering the hips is the last thing tried before a contact is given up.
    verticalNeed = std::clamp(verticalNeed - std::max(0.f, overrun),
                              settings.pelvisLower * scale,
                              settings.pelvisUpper * scale);
  } else {
    verticalNeed = 0;
  }
  features_.pelvisOffset += (verticalNeed - features_.pelvisOffset) * .25f;
  if (hasSupport) {
    features_.pelvisShift.x += (horizontalNeed.x - features_.pelvisShift.x) * .2f;
    features_.pelvisShift.z += (horizontalNeed.z - features_.pelvisShift.z) * .2f;
  } else {
    features_.pelvisShift = mul(features_.pelvisShift, .8f);
    if (len(features_.pelvisShift) < 1e-4f)
      features_.pelvisShift = {};
  }
  features_.pelvisShift.y = 0;
  metrics_.pelvisShiftUsed =
      std::sqrt(features_.pelvisShift.x * features_.pelvisShift.x +
                features_.pelvisShift.z * features_.pelvisShift.z);
  if (rig.pelvis >= 0) {
    // Pelvis translation is authored in model space; the offsets were solved in
    // world space, so the instance scale is divided out before they are applied.
    const V3 lateral =
        rotateY(mul(features_.pelvisShift, 1.f / scale), -in.heading);
    translatePelvis(asset_->skeleton, pose, rig.pelvis,
                    {lateral.x, features_.pelvisOffset / scale, lateral.z});
  }
  globals = asset_->skeleton.globals(pose);
  // After the hips have moved, so the arm reaches the hold from where the body
  // actually is. Handed back to the clip by its weight when released.
  solveContacts(in, pose);

  for (unsigned side = 0; side < count; ++side) {
    auto &foot = features_.feet[side];
    if (foot.weight <= .001f || !finite(foot.target))
      continue;
    const auto &chain = rig.legs[side];
    auto before = pose;
    auto g = asset_->skeleton.globals(pose);
    const V3 hip = origin(g[chain.root]);
    // Roll budget. The toe-off pitch swings the ankle up and back around the
    // contact edge; on a leg that is already straight that puts the target past
    // full extension, and the clamp that follows drags the sole forward for the
    // rest of the stance — the single worst slide the walk reports. Give up pitch
    // before contact: the edge does not move when the angle shrinks, so the shoe
    // keeps its grip and only the lift-off reads shallower.
    if (foot.locked && foot.roll != 0) {
      const float limit = legReach_[side] * .995f;
      auto reachOf = [&](float roll) {
        const V3 test =
            rolledAnkle(foot.flat, foot.normal, in.heading, roll,
                        chain.soleHeight, chain.footLength, scale);
        return len(sub(inversePoint(actor, test), hip));
      };
      if (reachOf(foot.roll) > limit) {
        // Five fixed halvings keep the choice deterministic and cheap; the last
        // candidate is a flat sole, which is always inside the reach the stance
        // already had.
        static const float factors[] = {1.f, .5f, .25f, .125f, 0.f};
        float roll = 0;
        for (const float factor : factors) {
          roll = foot.roll * factor;
          if (reachOf(roll) <= limit)
            break;
        }
        if (std::abs(roll) < .008f)
          roll = 0;
        ++metrics_.rollLimited;
        foot.roll = roll;
        foot.target = rolledAnkle(foot.flat, foot.normal, in.heading, foot.roll,
                                  chain.soleHeight, chain.footLength, scale);
      }
    }
    foot.mode = foot.roll > .02f ? ContactMode::Toe
                                 : (foot.roll < -.02f ? ContactMode::Heel
                                                      : ContactMode::Flat);
    if (foot.mode == ContactMode::Toe && modeBefore[side] != ContactMode::Toe)
      ++metrics_.toeOffs;
    if (foot.mode == ContactMode::Heel && modeBefore[side] != ContactMode::Heel)
      ++metrics_.heelStrikes;
    // The active edge moves when the mode changes, so the pivot is not the same
    // point across that tick. Restart the slip measurement instead of reporting
    // the edge offset as a metre-per-second slide.
    if (foot.mode != modeBefore[side])
      foot.pivotValid = false;
    V3 targetLocal = inversePoint(actor, foot.target);
    const V3 delta = sub(targetLocal, hip);
    // The stance leg is allowed to straighten completely: a slightly short
    // target is a contact that never quite lands, and that leftover is what the
    // sole-slide metric reports while the foot settles. Only a genuinely
    // over-long target is pulled back, and the release test below decides
    // whether the pin survives it.
    const float reach = std::max(1e-5f, legReach_[side]);
    const float required = len(delta);
    const bool clamped = required > reach;
    // Only a target that the leg genuinely cannot reach invalidates the pin. A
    // stance leg is straight by construction, so treating the last half percent
    // of reach as a failure released every support foot on every toe-off.
    // Keeping an impossible pin instead would drag the leg straight and away
    // from the pose the clip draws, which is the worse artifact: the release is
    // explicit, counted by reason, and reported through the fallback.
    const bool unreachable = required > legReach_[side] + .04f * scale;
    const bool overreach = unreachable;
    if (clamped) {
      targetLocal = add(hip, mul(unit(delta), reach));
    }
    if (overreach) {
      // Releasing hands the leg back to the animation for the few ticks it takes
      // to re-plant, which is the difference between a foot that follows the run
      // and a leg dragged straight and floating.
      if (foot.locked)
        ++metrics_.releaseCounts[std::size_t(ContactRelease::Overreach)];
      foot.release = ContactRelease::Overreach;
      foot.locked = false;
      foot.contact = false;
      foot.phase = FootPhase::Swing;
      foot.mode = ContactMode::Free;
      foot.pivotValid = false;
      foot.weight = std::max(0.f, foot.weight - Dt * 24);
      if (features_.steppingFoot == int(side))
        features_.steppingFoot = -1;
    }
    const auto result = solveTwoBone(asset_->skeleton, pose, chain, targetLocal,
                                     add(hip, chain.pole));
    metrics_.ikError = std::max(metrics_.ikError, result.error);
    // A stance leg is straight by construction, so the last fraction of a percent
    // of reach is not a failure. Only a target the leg genuinely cannot reach is
    // reported, which keeps the diagnostic meaningful.
    if (unreachable) {
      ++metrics_.ikClamped;
      if (features_.fallback == RigFallback::None)
        features_.fallback = RigFallback::Unreachable;
    }
    // Align the sole to the surface using the rest orientation as the
    // reference. Foot bones in imported rigs often point along the toes, so
    // assuming the bone's local +Y is up would flip the foot over.
    const auto localNormal =
        direction(actorMatrix({}, -in.heading, 1.f), foot.normal);
    const Quat groundRotation = rotationBetween({0, 1, 0}, localNormal);
    const Quat wantedRotation =
        product(groundRotation, restFootRotation_[side]);
    rotateGlobal(asset_->skeleton, pose, chain.tip,
                 product(wantedRotation,
                         inv(globalRotation(asset_->skeleton, pose,
                                            chain.tip))));
    // Pitch the shoe about the active contact edge. The ankle target was rolled
    // already, so the bone is rotated by the same angle to keep the sole tangent
    // to the roll arc instead of staying flat while the ankle climbs.
    const V3 forward = contactForward(in.heading, foot.normal);
    if (foot.roll != 0) {
      const V3 axis = unit(cross(foot.normal, forward), {1, 0, 0});
      rotateGlobal(asset_->skeleton, pose, chain.tip,
                   axisAngle(axis, foot.roll));
    }
    // Align the toe to its rest orientation too; otherwise the authored toe
    // pitch can push the toe mesh through the floor while the sole is flat. A
    // toe bone that keeps its rest plane is exactly what makes a rolled sole
    // bend at the ball instead of lifting the whole foot off the ground.
    if (chain.toe >= 0) {
      const unsigned toe = unsigned(chain.toe);
      const Quat toeWanted = product(groundRotation, restToeRotation_[side]);
      rotateGlobal(asset_->skeleton, pose, toe,
                   product(toeWanted,
                           inv(globalRotation(asset_->skeleton, pose, toe))));
    }
    BoneMask mask;
    mask.set(chain.root);
    mask.set(chain.joint);
    mask.set(chain.tip);
    if (chain.toe >= 0)
      mask.set(unsigned(chain.toe));
    pose = blend(before, pose, unsigned(asset_->skeleton.bones.size()),
                 foot.weight, mask);
    g = asset_->skeleton.globals(pose);
    const V3 final = point(actor, origin(g[chain.tip]));
    // Steady contact only: while the landing ramp is blending the leg in, the
    // ankle is deliberately travelling from the clip pose to the anchor, which is
    // a hand-over cost reported by `switchJump`, not a slide.
    if (foot.locked && foot.weight > .98f) {
      // Measure the sole, not the ankle. A toe-off moves the ankle on purpose;
      // what has to stay put is the edge the foot is rolling over. The lever is
      // rotated with the same angle the ankle target used, so the pivot is the
      // fixed point of the roll instead of its projection on the ground.
      // The lever must be the exact one the ankle target used. Between two
      // measurements the active edge can change (flat to toe, or toe to heel),
      // and the pivot then refers to a different point on the shoe: restart the
      // measurement instead of reporting the edge offset as a slide.
      if ((foot.roll > 0) != (foot.pivotRoll > 0))
        foot.pivotValid = false;
      foot.pivotRoll = foot.roll;
      const float edge = soleEdge(foot.roll, chain.footLength) * scale;
      const V3 lever =
          sub(mul(foot.normal, chain.soleHeight * scale), mul(forward, edge));
      const V3 pivotPoint =
          foot.roll != 0
              ? sub(final, rotated(lever, unit(cross(foot.normal, forward),
                                               {1, 0, 0}),
                                   foot.roll))
              : sub(final, lever);
      const V3 expected = mul(hits[side].velocity, Dt);
      if (foot.pivotValid) {
        const V3 slid = sub(sub(pivotPoint, foot.previousPivot), expected);
        const float speed =
            std::sqrt(slid.x * slid.x + slid.z * slid.z) / Dt;
        if (foot.pivoting)
          metrics_.pivotSlip = std::max(metrics_.pivotSlip, speed);
        else if (!foot.pivoted) {
          // A foot that was turned on purpose last tick has a new reference
          // point, so the comparison across that boundary is meaningless; the
          // turn's own displacement belongs to `pivotSlip`, not to this metric.
          metrics_.contactSlip = std::max(metrics_.contactSlip, speed);
        }
      }
      foot.pivoted = foot.pivoting;
      foot.previousPivot = pivotPoint;
      foot.pivotValid = true;
      const V3 residual = sub(final, foot.target);
      metrics_.anchorError = std::max(
          metrics_.anchorError,
          std::sqrt(residual.x * residual.x + residual.z * residual.z));
      const Quat soleDelta =
          product(globalRotation(asset_->skeleton, pose, chain.tip),
                  inv(restFootRotation_[side]));
      const V3 soleNormal = unit(rotateVector(soleDelta, foot.normal), {0, 1, 0});
      metrics_.soleAngle =
          std::max(metrics_.soleAngle,
                   std::acos(std::clamp(dot(soleNormal, foot.normal), -1.f, 1.f)));
    } else {
      foot.pivotValid = false;
      foot.pivoted = false;
    }
    if (foot.locked && foot.weight > .999f && foot.roll == 0) {
      // Ankle displacement, kept for continuity with earlier budgets. A rolled
      // sole moves the ankle on purpose, so those ticks are excluded; the sole
      // motion is measured through `contactSlip` instead.
      const V3 moved = sub(final, foot.previous);
      const V3 expected = mul(hits[side].velocity, Dt);
      metrics_.footSlip =
          std::max(metrics_.footSlip, len(sub(moved, expected)) / Dt);
    }
    foot.previous = final;
    if (walkable[side])
      metrics_.penetration = std::max(
          metrics_.penetration,
          std::max(0.f, chain.soleHeight * scale -
                            dot(sub(final, hits[side].point), foot.normal)));
  }
  if (in.hasLookTarget && rig.lookBone >= 0)
    solveLookAt(asset_->skeleton, pose, unsigned(rig.lookBone),
                inversePoint(actor, in.lookTarget));
}

void Controller::deform(const Intent &in, const PoseBuffer &pose) {
  metrics_.muscleWeight = metrics_.softMaxOffset = metrics_.softVolumeError =
      metrics_.softPenetration = 0;
  metrics_.softPoints = 0;
  for (unsigned i = 0; i < features_.morphWeights.size(); ++i)
    features_.morphWeights[i] = pose.curves[MorphCurveBase+i];
  if (config_.muscles)
    for (const auto &m : asset_->muscles) {
      auto q = product(pose.get(m.bone).rotation,
                       inv(asset_->skeleton.bones[m.bone].rest.rotation));
      float angle = 2 * std::acos(std::clamp(std::abs(q.w), 0.f, 1.f));
      float w = std::clamp((angle - m.angleStart) / (m.angleEnd - m.angleStart),
                           0.f, 1.f);
      w = w * w * (3 - 2 * w) * m.maxWeight;
      features_.morphWeights[m.target] =
          std::max(features_.morphWeights[m.target], w);
      metrics_.muscleWeight = std::max(metrics_.muscleWeight, w);
    }
  float lod = config_.softTissue && in.cameraDistance < 18 ? 1.f : 0.f;
  if (!config_.softTissue)
    features_.lodWeight = 0;
  else
    features_.lodWeight +=
        std::clamp(lod - features_.lodWeight, -Dt * 4, Dt * 4);
  auto globals = asset_->skeleton.globals(pose);
  auto actor = actorMatrix(in.position, in.heading);
  auto previousActor =
      actorMatrix(features_.previousPosition, features_.previousHeading);
  bool teleport = features_.hasPreviousPosition &&
                  len(sub(in.position, features_.previousPosition)) > 1;
  for (unsigned region = 0; region < asset_->softRegions.size(); ++region) {
    const auto &spec = asset_->softRegions[region];
    auto &state = features_.soft[region];
    auto offsets = tetra(spec.radius * .45f);
    std::array<V3, 4> targets{};
    for (unsigned i = 0; i < 4; ++i)
      targets[i] = point(globals[spec.bone], add(spec.center, offsets[i]));
    auto center = point(globals[spec.bone], spec.center);
    if (!state.initialized || teleport || features_.lodWeight == 0) {
      for (unsigned i = 0; i < 4; ++i)
        state.particles[i] = {targets[i], targets[i], {}};
      state.initialized = true;
    }
    if (features_.lodWeight > 0) {
      for (unsigned i = 0; i < 4; ++i) {
        auto &p = state.particles[i];
        if (features_.hasPreviousPosition && !teleport) {
          p.position = inversePoint(actor, point(previousActor, p.position));
          p.velocity =
              direction(actorMatrix({}, features_.previousHeading - in.heading),
                        p.velocity);
        }
        p.previous = p.position;
        p.velocity = mul(add(p.velocity, {0, -Dt * .8f, 0}), .96f);
        p.position = add(p.position, mul(p.velocity, Dt));
      }
      std::array<float, 4> attachLambda{};
      std::array<float, 6> edgeLambda{};
      float volumeLambda = 0;
      for (unsigned iteration = 0; iteration < 8; ++iteration) {
        for (unsigned i = 0; i < 4; ++i) {
          auto &p = state.particles[i];
          auto d = sub(p.position, targets[i]);
          float l = len(d), alpha = .00002f / (Dt * Dt);
          if (l > 1e-7f) {
            float dl = (-l - alpha * attachLambda[i]) / (1 + alpha);
            attachLambda[i] += dl;
            p.position = add(p.position, mul(d, dl / l));
          }
        }
        unsigned edge = 0;
        for (unsigned i = 0; i < 4; ++i)
          for (unsigned j = i + 1; j < 4; ++j) {
            auto &a = state.particles[i].position;
            auto &b = state.particles[j].position;
            auto d = sub(b, a);
            float l = len(d), rest = len(sub(targets[j], targets[i])),
                  alpha = .0000002f / (Dt * Dt);
            if (l > 1e-7f) {
              float dl = (-(l - rest) - alpha * edgeLambda[edge]) / (2 + alpha);
              edgeLambda[edge] += dl;
              auto delta = mul(d, dl / l);
              a = sub(a, delta);
              b = add(b, delta);
            }
            ++edge;
          }
        std::array<V3, 4> p{};
        for (unsigned i = 0; i < 4; ++i)
          p[i] = state.particles[i].position;
        std::array<V3, 4> grad{};
        grad[1] = mul(cross(sub(p[2], p[0]), sub(p[3], p[0])), 1.f / 6);
        grad[2] = mul(cross(sub(p[3], p[0]), sub(p[1], p[0])), 1.f / 6);
        grad[3] = mul(cross(sub(p[1], p[0]), sub(p[2], p[0])), 1.f / 6);
        grad[0] = mul(add(add(grad[1], grad[2]), grad[3]), -1);
        float alpha = 1e-10f / (Dt * Dt), den = alpha;
        for (auto v : grad)
          den += dot(v, v);
        float dl =
            (-(volume(p) - volume(targets)) - alpha * volumeLambda) / den;
        volumeLambda += dl;
        for (unsigned i = 0; i < 4; ++i)
          state.particles[i].position =
              add(state.particles[i].position, mul(grad[i], dl));
        V3 base = origin(globals[spec.bone]),
           end = point(globals[spec.bone], {0, -.3f, 0});
        for (unsigned i = 0; i < 4; ++i) {
          auto &p = state.particles[i];
          auto d = sub(p.position, targets[i]);
          float l = len(d);
          if (l > spec.maxOffset)
            p.position = add(targets[i], mul(d, spec.maxOffset / l));
          auto capsule = closest(p.position, base, end);
          auto radial = sub(p.position, capsule);
          float distance = len(radial);
          if (distance < .035f)
            p.position = add(capsule, mul(unit(radial, {1, 0, 0}), .035f));
        }
      }
      std::array<V3, 4> final{};
      for (unsigned i = 0; i < 4; ++i) {
        auto &p = state.particles[i];
        p.velocity = mul(sub(p.position, p.previous), 1 / Dt);
        if (len(p.velocity) > 3)
          p.velocity = mul(unit(p.velocity), 3);
        require(finite(p.position) && finite(p.velocity),
                "Non-finite soft tissue");
        final[i] = p.position;
        metrics_.softMaxOffset =
            std::max(metrics_.softMaxOffset, len(sub(p.position, targets[i])));
        auto axis = closest(p.position, origin(globals[spec.bone]),
                            point(globals[spec.bone], {0, -.3f, 0}));
        metrics_.softPenetration =
            std::max(metrics_.softPenetration,
                     std::max(0.f, .035f - len(sub(p.position, axis))));
      }
      metrics_.softVolumeError =
          std::max(metrics_.softVolumeError,
                   std::abs(volume(final) / volume(targets) - 1));
      metrics_.softPoints += 4;
    }
    state.previousCenter = center;
  }
  features_.previousPosition = in.position;
  features_.previousHeading = in.heading;
  features_.hasPreviousPosition = true;
}
void Controller::rebuildDeformation() {
  deformation_.assign(asset_->vertices.size(), {});
  if (asset_->softRegions.empty())
    return;
  // During evaluation the back buffer holds the corrected pose; callers
  // restoring a snapshot use front.
  const auto &pose = poses_[front_];
  auto globals = asset_->skeleton.globals(pose);
  for (unsigned r = 0; r < asset_->softRegions.size(); ++r) {
    const auto &region = asset_->softRegions[r];
    if (r >= features_.soft.size() || !features_.soft[r].initialized)
      continue;
    auto rest = tetra(region.radius * .45f);
    auto bindGlobals = asset_->skeleton.globals(asset_->skeleton.restPose());
    for (unsigned index : region.vertices) {
      const auto &v = asset_->vertices[index];
      auto local =
          sub(inversePoint(bindGlobals[region.bone],
                           {v.position[0], v.position[1], v.position[2]}),
              region.center);
      // Tetrahedral affine embedding; weights may be negative outside the
      // proxy.
      Matrix basis{{rest[1].x - rest[0].x, rest[1].y - rest[0].y,
                    rest[1].z - rest[0].z, 0, rest[2].x - rest[0].x,
                    rest[2].y - rest[0].y, rest[2].z - rest[0].z, 0,
                    rest[3].x - rest[0].x, rest[3].y - rest[0].y,
                    rest[3].z - rest[0].z, 0, rest[0].x, rest[0].y, rest[0].z,
                    1}};
      auto b = inversePoint(basis, local);
      std::array<float, 4> weights{{1 - b.x - b.y - b.z, b.x, b.y, b.z}};
      V3 delta{};
      for (unsigned i = 0; i < 4; ++i) {
        auto target = point(globals[region.bone], add(region.center, rest[i]));
        delta =
            add(delta, mul(sub(features_.soft[r].particles[i].position, target),
                           weights[i]));
      }
      float l = len(delta);
      if (l > region.maxOffset)
        delta = mul(delta, region.maxOffset / l);
      deformation_[index].position =
          add(deformation_[index].position, mul(delta, features_.lodWeight));
    }
  }
  // Recompute post-skin normal corrections from the final triangles; zero
  // deformation is exact.
  auto palette = asset_->skeleton.palette(pose);
  std::vector<V3> p(asset_->vertices.size()), n(p.size()), sum(p.size());
  for (unsigned i = 0; i < p.size(); ++i) {
    const auto &v = asset_->vertices[i];
    V3 pos{v.position[0], v.position[1], v.position[2]},
        normal{v.normal[0], v.normal[1], v.normal[2]};
    for (unsigned m = 0; m < asset_->morphs.size(); ++m)
      if (features_.morphWeights[m] != 0)
        for (const auto &d : asset_->morphs[m].deltas)
          if (d.vertex == i) {
            pos = add(pos, mul(d.position, features_.morphWeights[m]));
            normal = add(normal, mul(d.normal, features_.morphWeights[m]));
          }
    for (unsigned j = 0; j < 4; ++j) {
      p[i] = add(p[i], mul(point(palette[v.joints[j]], pos), v.weights[j]));
      n[i] =
          add(n[i], mul(direction(palette[v.joints[j]], normal), v.weights[j]));
    }
    p[i] = add(p[i], deformation_[i].position);
  }
  for (unsigned i = 0; i < asset_->indices.size(); i += 3) {
    auto a = asset_->indices[i], b = asset_->indices[i + 1],
         c = asset_->indices[i + 2];
    auto face = cross(sub(p[b], p[a]), sub(p[c], p[a]));
    if (dot(face, n[a]) < 0)
      face = mul(face, -1);
    sum[a] = add(sum[a], face);
    sum[b] = add(sum[b], face);
    sum[c] = add(sum[c], face);
  }
  for (unsigned i = 0; i < p.size(); ++i)
    if (len(deformation_[i].position) > 1e-8f)
      deformation_[i].normal = sub(unit(sum[i], unit(n[i])), unit(n[i]));
}
void Controller::hashFeatures(determinism::Hasher &h) const {
  h.u8(config_.motionMatching);
  h.u8(config_.proceduralRig);
  h.u8(config_.muscles);
  h.u8(config_.softTissue);
  h.u8(features_.matching);
  h.u32(features_.matchedClip);
  h.quantized(float(features_.matchedSeconds));
  h.u64(features_.lastSwitch);
  h.u8(static_cast<std::uint8_t>(features_.fallback));
  h.u8(static_cast<std::uint8_t>(features_.steppingFoot + 1));
  h.quantized(features_.turnResidual);
  h.u32(features_.turnSteps);
  hashV(h, features_.previousPosition);
  h.quantized(features_.previousHeading);
  h.u8(features_.hasPreviousPosition);
  h.quantized(features_.pelvisOffset);
  hashV(h, features_.pelvisShift);
  h.quantized(features_.lodWeight);
  for (const auto &f : features_.feet) {
    h.u8(f.locked);
    h.u32(f.surface);
    hashV(h, f.anchor);
    hashV(h, f.previous);
    hashV(h, f.normal);
    h.quantized(f.weight);
    h.u8(static_cast<std::uint8_t>(f.phase));
    h.u8(static_cast<std::uint8_t>(f.mode));
    h.u8(static_cast<std::uint8_t>(f.release));
    h.quantized(f.roll);
    hashV(h, f.pivot);
    hashV(h, f.previousPivot);
    h.u8(f.pivotValid);
    h.quantized(f.pivotRoll);
    h.u8(f.pivoting);
    hashV(h, f.target);
    hashV(h, f.targetNormal);
    hashV(h, f.swingStart);
    hashV(h, f.swingEnd);
    h.quantized(f.swingTime);
    h.quantized(f.swingDuration);
    h.u8(f.turnStep);
    h.quantized(f.cooldown);
    h.quantized(f.anchorHeading);
    h.u8(f.contact);
    h.u8(f.pivoted);
    h.quantized(f.reachAtPlant);
    hashV(h, f.animated);
  }
  for (float w : features_.morphWeights)
    h.quantized(w);
  for (const auto &c : features_.contacts) {
    h.u32(c.id);
    h.u8(c.active);
    h.u8(c.bound);
    h.u32(static_cast<unsigned>(c.chain));
    hashV(h, c.anchor);
    h.u32(c.surface);
    hashV(h, c.pose);
    h.quantized(c.weight);
    h.quantized(c.priority);
    h.u8(static_cast<std::uint8_t>(c.release));
    h.u64(c.validUntil);
  }
  for (const auto &s : features_.soft) {
    h.u8(s.initialized);
    hashV(h, s.previousCenter);
    for (const auto &p : s.particles) {
      hashV(h, p.position);
      hashV(h, p.previous);
      hashV(h, p.velocity);
    }
  }
}
} // namespace engine::animation
