#include "animation.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace engine::animation {
namespace {
void require(bool ok, const char *code, const std::string &message) {
  if (!ok)
    throw Error(code, message);
}
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
float dot(Quat a, Quat b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
Quat product(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat inverse(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
V3 qlog(Quat q) {
  q = normalized(q);
  if (q.w < 0)
    q = {-q.x, -q.y, -q.z, -q.w};
  float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  float k = s > 1e-7f ? 2 * std::atan2(s, q.w) / s : 2;
  return {q.x * k, q.y * k, q.z * k};
}
Quat qexp(V3 v) {
  float a = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  float k = a > 1e-7f ? std::sin(a * 0.5f) / a : 0.5f;
  return normalized({v.x * k, v.y * k, v.z * k, std::cos(a * 0.5f)});
}
float length(V3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
bool finite(V3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
void hashBone(determinism::Hasher &h, const PoseBuffer &p, unsigned b) {
  for (float f : p.translation[b])
    h.quantized(f);
  for (float f : p.rotation[b])
    h.quantized(f);
  for (float f : p.scale[b])
    h.quantized(f);
  h.quantized(p.layerWeight[b]);
}
bool changed(const PoseBuffer &a, const PoseBuffer &b, unsigned bone) {
  return a.translation[bone] != b.translation[bone] ||
         a.rotation[bone] != b.rotation[bone] ||
         a.scale[bone] != b.scale[bone] ||
         a.layerWeight[bone] != b.layerWeight[bone];
}
void validPose(const PoseBuffer &p, unsigned count) {
  for (unsigned b = 0; b < count; ++b) {
    auto t = p.get(b);
    float n = dot(t.rotation, t.rotation);
    require(finite(t.translation) && finite(t.scale) && std::isfinite(n) &&
                std::abs(n - 1) < 0.002f && t.scale.x > 0 && t.scale.y > 0 &&
                t.scale.z > 0 && std::isfinite(p.layerWeight[b]) &&
                p.layerWeight[b] >= 0 && p.layerWeight[b] <= 1,
            "NG-E-ANIM-POSE", "Invalid pose at bone " + std::to_string(b));
  }
  for (float f : p.curves)
    require(std::isfinite(f), "NG-E-ANIM-POSE", "Non-finite curve");
}
std::array<float, 9> difference(const Transform &a, const Transform &b) {
  auto r = qlog(product(a.rotation, inverse(b.rotation)));
  return {{a.translation.x - b.translation.x, a.translation.y - b.translation.y,
           a.translation.z - b.translation.z, r.x, r.y, r.z,
           a.scale.x - b.scale.x, a.scale.y - b.scale.y,
           a.scale.z - b.scale.z}};
}
void notifyRange(const Clip &clip, unsigned clipId, std::uint64_t previous,
                 std::uint64_t current, std::uint64_t tick,
                 std::vector<Event> &events) {
  const std::uint64_t period = (clip.frameCount - 1) * 2ull;
  for (const auto &n : clip.notifies) {
    const std::uint64_t at = n.frame * 2ull;
    // Frame zero fires on the first tick, then once per loop. Endpoint events
    // belong to the previous loop; zero and endpoint must not be aliases.
    if (previous == 0 && at == 0 && current > 0)
      events.push_back({tick, n.name, clipId});
    if (clip.loop) {
      std::uint64_t cycle = previous / period;
      for (; cycle <= current / period; ++cycle) {
        auto occurrence = cycle * period + at;
        if (occurrence > previous && occurrence <= current)
          events.push_back({tick, n.name, clipId});
      }
    } else if (at > previous && at <= current)
      events.push_back({tick, n.name, clipId});
  }
}
} // namespace
Error::Error(const char *c, const std::string &message)
    : std::runtime_error(std::string(c) + ": " + message), code(c) {}
Quat normalized(Quat q) {
  float n = std::sqrt(dot(q, q));
  require(std::isfinite(n) && n > 1e-8f, "NG-E-ANIM-POSE",
          "Invalid quaternion");
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
Quat slerp(Quat a, Quat b, float t) {
  a = normalized(a);
  b = normalized(b);
  float d = dot(a, b);
  if (d < 0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    d = -d;
  }
  t = std::clamp(t, 0.0f, 1.0f);
  float u = 1 - t, v = t;
  if (d < 0.9995f) {
    float theta = std::acos(std::clamp(d, -1.f, 1.f));
    float s = std::sin(theta);
    u = std::sin((1 - t) * theta) / s;
    v = std::sin(t * theta) / s;
  }
  return normalized({a.x * u + b.x * v, a.y * u + b.y * v, a.z * u + b.z * v,
                     a.w * u + b.w * v});
}
Matrix matrix(const Transform &t) {
  auto q = normalized(t.rotation);
  float x = q.x, y = q.y, z = q.z, w = q.w;
  return {
      {(1 - 2 * y * y - 2 * z * z) * t.scale.x,
       (2 * x * y + 2 * z * w) * t.scale.x, (2 * x * z - 2 * y * w) * t.scale.x,
       0, (2 * x * y - 2 * z * w) * t.scale.y,
       (1 - 2 * x * x - 2 * z * z) * t.scale.y,
       (2 * y * z + 2 * x * w) * t.scale.y, 0,
       (2 * x * z + 2 * y * w) * t.scale.z, (2 * y * z - 2 * x * w) * t.scale.z,
       (1 - 2 * x * x - 2 * y * y) * t.scale.z, 0, t.translation.x,
       t.translation.y, t.translation.z, 1}};
}
Matrix multiply(const Matrix &a, const Matrix &b) {
  Matrix out{};
  for (unsigned c = 0; c < 4; ++c) {
    for (unsigned r = 0; r < 4; ++r) {
      for (unsigned k = 0; k < 4; ++k)
        out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    }
  }
  return out;
}
V3 point(const Matrix &m, V3 p) {
  return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
          m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
          m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
bool BoneMask::has(unsigned bone) const {
  return bone < MaxBones && (bits[bone / 64] & (1ull << (bone % 64)));
}
void BoneMask::set(unsigned bone) {
  require(bone < MaxBones, "NG-E-ANIM-MASK", "Bone mask index out of range");
  bits[bone / 64] |= 1ull << (bone % 64);
}
BoneMask BoneMask::all(unsigned count) {
  require(count <= MaxBones, "NG-E-ANIM-MASK", "Too many bones");
  BoneMask out;
  for (unsigned b = 0; b < count; ++b)
    out.set(b);
  return out;
}
PoseBuffer::PoseBuffer() {
  for (unsigned b = 0; b < MaxBones; ++b) {
    rotation[b][3] = 1;
    scale[b] = {{1, 1, 1}};
    layerWeight[b] = 1;
  }
}
Transform PoseBuffer::get(unsigned b) const {
  require(b < MaxBones, "NG-E-ANIM-MASK", "Pose index out of range");
  return {{translation[b][0], translation[b][1], translation[b][2]},
          {rotation[b][0], rotation[b][1], rotation[b][2], rotation[b][3]},
          {scale[b][0], scale[b][1], scale[b][2]}};
}
void PoseBuffer::set(unsigned b, const Transform &t) {
  require(b < MaxBones, "NG-E-ANIM-MASK", "Pose index out of range");
  translation[b] = {{t.translation.x, t.translation.y, t.translation.z}};
  rotation[b] = {{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}};
  scale[b] = {{t.scale.x, t.scale.y, t.scale.z}};
}
void Skeleton::validate() const {
  require(!bones.empty() && bones.size() <= MaxBones, "NG-E-ANIM-SKELETON",
          "Skeleton needs 1..256 bones");
  std::set<std::string> names;
  unsigned roots = 0;
  for (unsigned b = 0; b < bones.size(); ++b) {
    const auto &bone = bones[b];
    require(!bone.name.empty() && names.insert(bone.name).second,
            "NG-E-ANIM-SKELETON", "Duplicate or empty bone name");
    require(bone.parent >= -1 && bone.parent < static_cast<int>(b),
            "NG-E-ANIM-SKELETON", "Hierarchy must be parent-first and acyclic");
    if (bone.parent < 0)
      ++roots;
    for (float f : bone.inverseBind)
      require(std::isfinite(f), "NG-E-ANIM-SKELETON",
              "Non-finite inverse bind");
  }
  require(roots == 1, "NG-E-ANIM-SKELETON", "Exactly one root required");
  validPose(restPose(), static_cast<unsigned>(bones.size()));
}
PoseBuffer Skeleton::restPose() const {
  PoseBuffer p;
  for (unsigned b = 0; b < bones.size(); ++b)
    p.set(b, bones[b].rest);
  return p;
}
std::vector<Matrix> Skeleton::globals(const PoseBuffer &p) const {
  std::vector<Matrix> out;
  out.reserve(bones.size());
  for (unsigned b = 0; b < bones.size(); ++b) {
    auto m = matrix(p.get(b));
    if (bones[b].parent >= 0)
      m = multiply(out[bones[b].parent], m);
    out.push_back(m);
  }
  return out;
}
std::vector<Matrix> Skeleton::palette(const PoseBuffer &p) const {
  auto out = globals(p);
  for (unsigned b = 0; b < out.size(); ++b)
    out[b] = multiply(out[b], bones[b].inverseBind);
  return out;
}
double Clip::duration() const {
  return frameCount > 1 ? double(frameCount - 1) / SampleRate : 0;
}
bool Clip::staticPose() const {
  if (frameCount < 2 || samples.empty())
    return true;
  for (unsigned f = 1; f < frameCount; ++f)
    for (unsigned b = 0; b < boneCount; ++b) {
      const auto &a = samples[b], &c = samples[f * boneCount + b];
      if (std::abs(a.translation.x - c.translation.x) > 1e-5f ||
          std::abs(a.translation.y - c.translation.y) > 1e-5f ||
          std::abs(a.translation.z - c.translation.z) > 1e-5f ||
          std::abs(a.scale.x - c.scale.x) > 1e-5f ||
          std::abs(a.scale.y - c.scale.y) > 1e-5f ||
          std::abs(a.scale.z - c.scale.z) > 1e-5f ||
          std::abs(dot(a.rotation, c.rotation)) < 1.f - 1e-6f)
        return false;
    }
  return true;
}
PoseBuffer Clip::sample(double seconds) const {
  require(boneCount > 0 && boneCount <= MaxBones && frameCount >= 2 &&
              samples.size() == std::size_t(boneCount) * frameCount,
          "NG-E-ANIM-CLIP", "Malformed clip " + name);
  require(std::isfinite(seconds), "NG-E-ANIM-CLIP", "Non-finite sample time");
  double time = loop ? std::fmod(std::max(0.0, seconds), duration())
                     : std::clamp(seconds, 0.0, duration());
  double frame = time * SampleRate;
  unsigned a = std::min(static_cast<unsigned>(frame), frameCount - 1),
           b = std::min(a + 1, frameCount - 1);
  float w = static_cast<float>(frame - a);
  PoseBuffer out;
  for (unsigned bone = 0; bone < boneCount; ++bone) {
    auto x = samples[a * boneCount + bone], y = samples[b * boneCount + bone];
    out.set(bone,
            {add(x.translation, mul(sub(y.translation, x.translation),
                                    (stepChannels[bone] & 1) ? 0 : w)),
             slerp(x.rotation, y.rotation, (stepChannels[bone] & 2) ? 0 : w),
             add(x.scale, mul(sub(y.scale, x.scale),
                              (stepChannels[bone] & 4) ? 0 : w))});
  }
  out.curves[0] = static_cast<float>(time / duration());
  require(morphSamples.empty()||morphSamples.size()==frameCount,
          "NG-E-ANIM-CLIP","Malformed morph samples");
  if(!morphSamples.empty())for(unsigned m=0;m<MaxMorphTargets;++m) {
    const float x=morphSamples[a][m],y=morphSamples[b][m];
    out.curves[MorphCurveBase+m]=x+(y-x)*((stepMorphMask&(1u<<m))?0:w);
  }
  return out;
}
V3 Clip::rootDelta(double from, double to) const {
  require(std::isfinite(from) && std::isfinite(to) && from >= 0 && to >= from,
          "NG-E-ANIM-CLIP", "Invalid root motion interval");
  auto unwrapped = [&](double t) {
    auto p = sample(t).get(0).translation;
    if (loop) {
      auto d = sub(samples[(frameCount - 1) * boneCount].translation,
                   samples[0].translation);
      p = add(p, mul(d, static_cast<float>(std::floor(t / duration()))));
    }
    return p;
  };
  return sub(unwrapped(to), unwrapped(from));
}
PoseBuffer blend(const PoseBuffer &a, const PoseBuffer &b, unsigned count,
                 float w, const BoneMask &mask) {
  require(count <= MaxBones && std::isfinite(w), "NG-E-ANIM-BLEND",
          "Invalid blend arguments");
  w = std::clamp(w, 0.f, 1.f);
  auto out = a;
  for (unsigned bone = 0; bone < count; ++bone)
    if (mask.has(bone)) {
      auto x = a.get(bone), y = b.get(bone);
      float weight = w * std::clamp(b.layerWeight[bone], 0.f, 1.f);
      out.set(bone, {add(x.translation,
                         mul(sub(y.translation, x.translation), weight)),
                     slerp(x.rotation, y.rotation, weight),
                     add(x.scale, mul(sub(y.scale, x.scale), weight))});
    }
  for (unsigned c = 0; c < MaxCurves; ++c)
    out.curves[c] = a.curves[c] + (b.curves[c] - a.curves[c]) * w;
  return out;
}
std::vector<BlendWeight> blendSpace1D(std::vector<BlendPoint> p, float x) {
  require(!p.empty() && std::isfinite(x), "NG-E-ANIM-BLEND",
          "Empty or invalid 1D blend space");
  for (auto v : p)
    require(std::isfinite(v.x), "NG-E-ANIM-BLEND", "Invalid 1D point");
  std::sort(p.begin(), p.end(), [](auto a, auto b) {
    return std::tie(a.x, a.clip) < std::tie(b.x, b.clip);
  });
  for (unsigned i = 1; i < p.size(); ++i)
    require(p[i].x > p[i - 1].x, "NG-E-ANIM-BLEND", "Duplicate 1D point");
  if (x <= p.front().x)
    return {{p.front().clip, 1}};
  if (x >= p.back().x)
    return {{p.back().clip, 1}};
  for (unsigned i = 1; i < p.size(); ++i)
    if (x <= p[i].x) {
      float w = (x - p[i - 1].x) / (p[i].x - p[i - 1].x);
      return {{p[i - 1].clip, 1 - w}, {p[i].clip, w}};
    }
  return {};
}
std::vector<BlendWeight>
blendSpace2D(const std::vector<BlendPoint> &p,
             const std::vector<std::array<unsigned, 3>> &triangles, float x,
             float y) {
  require(!p.empty() && std::isfinite(x) && std::isfinite(y), "NG-E-ANIM-BLEND",
          "Invalid 2D space");
  for (auto v : p)
    require(std::isfinite(v.x) && std::isfinite(v.y), "NG-E-ANIM-BLEND",
            "Invalid 2D point");
  for (auto t : triangles) {
    for (auto i : t)
      require(i < p.size(), "NG-E-ANIM-BLEND", "Triangle index out of range");
    auto a = p[t[0]], b = p[t[1]], c = p[t[2]];
    float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    require(std::abs(d) > 1e-8f, "NG-E-ANIM-BLEND", "Degenerate triangle");
    float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d,
          v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d,
          w = 1 - u - v;
    if (u >= 0 && v >= 0 && w >= 0)
      return {{a.clip, u}, {b.clip, v}, {c.clip, w}};
  }
  // Outside the authored domain: project to the closest triangle edge.
  float best = std::numeric_limits<float>::infinity();
  std::vector<BlendWeight> result;
  for (auto t : triangles)
    for (unsigned e = 0; e < 3; ++e) {
      auto a = p[t[e]], b = p[t[(e + 1) % 3]];
      float dx = b.x - a.x, dy = b.y - a.y;
      float w = std::clamp(
          ((x - a.x) * dx + (y - a.y) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
      float d = (a.x + w * dx - x) * (a.x + w * dx - x) +
                (a.y + w * dy - y) * (a.y + w * dy - y);
      if (d < best) {
        best = d;
        result = {{a.clip, 1 - w}, {b.clip, w}};
      }
    }
  require(!result.empty(), "NG-E-ANIM-BLEND", "2D space needs triangles");
  return result;
}
void Asset::validate() const {
  skeleton.validate();
  require(!clips.empty(), "NG-E-ANIM-CLIP", "No clips");
  std::set<std::string> names;
  auto uniformScale = [](V3 scale) {
    return std::abs(scale.x - scale.y) < 1e-5f &&
           std::abs(scale.x - scale.z) < 1e-5f;
  };
  for (const auto &bone : skeleton.bones)
    require(uniformScale(bone.rest.scale), "NG-E-ANIM-SKIN",
            "Only uniform joint scale is supported");
  for (const auto &c : clips) {
    require(finite(c.motionVelocity) && std::isfinite(c.turnRate) &&
                std::abs(c.turnRate) <= 20 && c.tags > 0 && c.tags <= 65535 &&
                std::isfinite(c.locomotionSpeed) && c.locomotionSpeed >= 0 &&
                c.locomotionSpeed <= 50,
            "NG-E-ANIM-CLIP", "Invalid matching metadata");
    for (auto range : c.contacts)
      for (float phase : range)
        require(std::isfinite(phase) && phase >= 0 && phase <= 1,
                "NG-E-ANIM-CLIP", "Invalid contact phase");
    require(std::isfinite(c.contactConfidence) && c.contactConfidence >= 0 &&
                c.contactConfidence <= 1,
            "NG-E-ANIM-CLIP", "Invalid contact metadata");
    require(!c.name.empty() && names.insert(c.name).second &&
                c.boneCount == skeleton.bones.size() && c.frameCount >= 2 &&
                c.samples.size() == std::size_t(c.boneCount) * c.frameCount,
            "NG-E-ANIM-CLIP", "Invalid or duplicate clip " + c.name);
    require(c.morphSamples.empty()||c.morphSamples.size()==c.frameCount,
            "NG-E-ANIM-CLIP","Invalid morph frame count");
    for(const auto& frame:c.morphSamples)for(unsigned m=0;m<MaxMorphTargets;++m)
      require(std::isfinite(frame[m])&&std::abs(frame[m])<=8&&(m<morphs.size()||frame[m]==0),
              "NG-E-ANIM-CLIP","Invalid morph sample");
    PoseBuffer p;
    for (unsigned f = 0; f < c.frameCount; ++f) {
      for (unsigned b = 0; b < c.boneCount; ++b) {
        p.set(b, c.samples[f * c.boneCount + b]);
        require(uniformScale(c.samples[f * c.boneCount + b].scale),
                "NG-E-ANIM-SKIN", "Only uniform animated scale is supported");
      }
      validPose(p, c.boneCount);
    }
    unsigned last = 0;
    for (auto n : c.notifies) {
      require(!n.name.empty() && n.frame < c.frameCount && n.frame >= last,
              "NG-E-ANIM-CLIP", "Invalid notify");
      last = n.frame;
    }
  }
  require(rig.pelvis >= -1 && rig.pelvis < int(skeleton.bones.size()) &&
              rig.lookBone >= -1 && rig.lookBone < int(skeleton.bones.size()) &&
              rig.spine >= -1 && rig.spine < int(skeleton.bones.size()) &&
              rig.legs.size() <= 2 && std::isfinite(rig.confidence) &&
              rig.confidence >= 0 && rig.confidence <= 1 &&
              rig.warnings.size() <= 16,
          "NG-E-ANIM-RIG", "Invalid rig semantics");
  const auto &rs = rig.settings;
  require(std::isfinite(rs.height) && rs.height >= .1f && rs.height <= 100.f &&
              std::isfinite(rs.turnThreshold) && rs.turnThreshold >= .05f &&
              rs.turnThreshold <= 3.14f && std::isfinite(rs.turnStepHeight) &&
              rs.turnStepHeight >= 0 && rs.turnStepHeight <= 5 &&
              std::isfinite(rs.turnStepDuration) && rs.turnStepDuration >= .01f &&
              rs.turnStepDuration <= 10 && std::isfinite(rs.turnCooldown) &&
              rs.turnCooldown >= 0 && rs.turnCooldown <= 10 &&
              std::isfinite(rs.maxTwist) && rs.maxTwist >= 0 &&
              rs.maxTwist <= 6.28f && std::isfinite(rs.maxSlopeCosine) &&
              rs.maxSlopeCosine >= 0 && rs.maxSlopeCosine <= 1 &&
              std::isfinite(rs.swingPrediction) && rs.swingPrediction >= 0 &&
              rs.swingPrediction <= 10 && std::isfinite(rs.maxFootCorrection) &&
              rs.maxFootCorrection >= 0 && rs.maxFootCorrection <= 10 &&
              std::isfinite(rs.pelvisLower) && rs.pelvisLower >= -10 &&
              rs.pelvisLower <= 0 && std::isfinite(rs.pelvisUpper) &&
              rs.pelvisUpper >= 0 && rs.pelvisUpper <= 10 &&
              std::isfinite(rs.maxStepHeight) && rs.maxStepHeight >= 0 &&
              rs.maxStepHeight <= 10 && std::isfinite(rs.maxPelvisShift) &&
              rs.maxPelvisShift >= 0 && rs.maxPelvisShift <= 1 &&
              std::isfinite(rs.soleRoll) && rs.soleRoll >= 0 &&
              rs.soleRoll <= 1.5f,
          "NG-E-ANIM-RIG", "Invalid rig settings");
  for (const auto &c : rig.legs)
    require(c.root < skeleton.bones.size() && c.joint < skeleton.bones.size() &&
                c.tip < skeleton.bones.size() &&
                (c.toe < 0 || (c.toe < int(skeleton.bones.size()) &&
                               skeleton.bones[c.toe].parent == int(c.tip))) &&
                skeleton.bones[c.joint].parent == int(c.root) &&
                skeleton.bones[c.tip].parent == int(c.joint) &&
                finite(c.pole) && std::isfinite(c.minBend) &&
                std::isfinite(c.maxBend) && c.minBend >= 0 &&
                c.maxBend > c.minBend && c.maxBend < 3.14159265f &&
                std::isfinite(c.soleHeight) && c.soleHeight >= 0 &&
                c.soleHeight < .5f && std::isfinite(c.footLength) &&
                c.footLength > 0 && c.footLength < 2,
            "NG-E-ANIM-RIG", "Invalid rig chain");
  require(morphs.size() <= MaxMorphTargets && muscles.size() <= 64 &&
              softRegions.size() <= 16,
          "NG-E-ANIM-SKIN", "Feature capacity exceeded");
  std::set<std::string> morphNames;
  for (const auto &m : morphs) {
    require(!m.name.empty() && morphNames.insert(m.name).second,
            "NG-E-ANIM-SKIN", "Duplicate morph");
    unsigned previous = 0;
    bool first = true;
    for (const auto &d : m.deltas) {
      require(d.vertex < vertices.size() && (first || d.vertex > previous) &&
                  finite(d.position) && finite(d.normal) && finite(d.tangent) &&
                  length(d.position) <= 10 && length(d.normal) <= 10 && length(d.tangent)<=10 &&
                  (length(d.tangent)==0||std::abs(vertices[d.vertex].tangent[3])==1),
              "NG-E-ANIM-SKIN", "Invalid morph delta");
      first = false;
      previous = d.vertex;
    }
  }
  for (const auto &m : muscles)
    require(m.bone < skeleton.bones.size() && m.target < morphs.size() &&
                std::isfinite(m.angleStart) && std::isfinite(m.angleEnd) &&
                m.angleEnd > m.angleStart && std::isfinite(m.maxWeight) &&
                m.maxWeight >= 0 && m.maxWeight <= 1,
            "NG-E-ANIM-SKIN", "Invalid muscle driver");
  for (const auto &r : softRegions) {
    require(r.bone < skeleton.bones.size() && finite(r.center) &&
                std::isfinite(r.radius) && r.radius >= .02f && r.radius <= 1 &&
                std::isfinite(r.maxOffset) && r.maxOffset > 0 &&
                r.maxOffset <= .1f && !r.vertices.empty(),
            "NG-E-ANIM-SKIN", "Invalid soft region");
    std::set<unsigned> used;
    for (unsigned v : r.vertices)
      require(v < vertices.size() && used.insert(v).second, "NG-E-ANIM-SKIN",
              "Invalid soft embedding");
  }
  require(!vertices.empty() && !indices.empty() && indices.size() % 3 == 0,
          "NG-E-ANIM-SKIN", "Missing triangle mesh");
  for (auto i : indices)
    require(i < vertices.size(), "NG-E-ANIM-SKIN", "Index out of range");
  for (const auto &v : vertices) {
    float sum = 0;
    for (unsigned k = 0; k < 4; ++k) {
      require(v.joints[k] < skeleton.bones.size() &&
                  std::isfinite(v.weights[k]) && v.weights[k] >= 0,
              "NG-E-ANIM-SKIN", "Invalid joint or weight");
      sum += v.weights[k];
    }
    require(std::abs(sum - 1) < 1e-4f, "NG-E-ANIM-SKIN",
            "Skin weights must sum to one");
    for (float f : v.position)
      require(std::isfinite(f), "NG-E-ANIM-SKIN", "Invalid position");
    for (float f : v.normal)
      require(std::isfinite(f), "NG-E-ANIM-SKIN", "Invalid normal");
    for (float f : v.uv)
      require(std::isfinite(f), "NG-E-ANIM-SKIN", "Invalid UV");
    if(hasTextureUvs)for(const auto& uv:v.textureUvs)for(float f:uv)
      require(std::isfinite(f)&&std::abs(f)<=1000000,"NG-E-ANIM-SKIN","Invalid per-slot texture UV");
    if(hasLayerUvs)for(const auto& uv:v.layerUvs)for(float f:uv)require(std::isfinite(f)&&std::abs(f)<=1000000,"NG-E-ANIM-SKIN","Invalid layer UV");
    for(float f:v.color)require(std::isfinite(f)&&f>=0&&f<=1,"NG-E-ANIM-SKIN","Invalid vertex color");
    float tangentLength=0,tangentDot=0,normalLength=0;for(unsigned k=0;k<4;k++)require(std::isfinite(v.tangent[k]),"NG-E-ANIM-SKIN","Invalid tangent");for(unsigned k=0;k<3;k++){tangentLength+=v.tangent[k]*v.tangent[k];tangentDot+=v.tangent[k]*v.normal[k];normalLength+=v.normal[k]*v.normal[k];}
    require((v.tangent[3]==0&&tangentLength==0)||(std::abs(v.tangent[3])==1&&std::abs(tangentLength-1)<.02f&&std::abs(tangentDot)<.02f*std::sqrt(normalLength)),"NG-E-ANIM-SKIN","Invalid tangent frame");
  }
}
unsigned Asset::clipIndex(const std::string &name) const {
  for (unsigned c = 0; c < clips.size(); ++c)
    if (clips[c].name == name)
      return c;
  throw Error("NG-E-ANIM-CLIP", "Missing clip " + name);
}
void Graph::compile(std::vector<Trait> traits, unsigned count) {
  require(count > 0 && count <= MaxBones, "NG-E-ANIM-GRAPH",
          "Invalid bone count");
  std::sort(traits.begin(), traits.end(), [](const Trait &a, const Trait &b) {
    return std::tie(a.stage, a.priority, a.id) <
           std::tie(b.stage, b.priority, b.id);
  });
  std::set<std::uint32_t> seen;
  for (auto &t : traits) {
    require(static_cast<unsigned>(t.stage) < 5 && t.evaluate &&
                seen.insert(t.id).second,
            "NG-E-ANIM-GRAPH", "Invalid or duplicate trait");
    require(t.deterministic || t.stage == Stage::Deform ||
                t.stage == Stage::Emit,
            "NG-E-ANIM-GRAPH", "Non-deterministic Select/Blend/Correct trait");
    for (auto dep : t.after)
      require(dep != t.id && seen.count(dep), "NG-E-ANIM-GRAPH",
              "Dependency violates fixed stage/priority/id order");
    for (unsigned b = 0; b < MaxBones; ++b) {
      require(b < count || (!t.reads.has(b) && !t.writes.has(b)),
              "NG-E-ANIM-MASK", "Mask exceeds skeleton");
      require(!t.writes.has(b) ||
                  (t.stage != Stage::Select && t.stage != Stage::Emit),
              "NG-E-ANIM-MASK", "Select/Emit cannot write pose");
    }
    require(t.stage != Stage::Deform || !t.writes.has(0), "NG-E-ANIM-MASK",
            "Deform cannot write root");
  }
  traits_ = std::move(traits);
  boneCount_ = count;
}
void Graph::evaluate(const Intent &in, Selection &selection, PoseBuffer &pose,
                     float budget) {
  require(boneCount_ > 0 && std::isfinite(in.speed) &&
              std::isfinite(in.heading) && std::isfinite(in.verticalSpeed) &&
              std::isfinite(budget) && budget >= 0,
          "NG-E-ANIM-GRAPH", "Invalid evaluation input");
  determinism::Hasher h;
  h.text("nativegpu.animation/1");
  h.u64(in.tick);
  h.quantized(in.speed);
  h.quantized(in.verticalSpeed);
  h.quantized(in.heading);
  h.u8(in.grounded);
  for (auto v :
       {in.position, in.actualVelocity, in.desiredVelocity, in.lookTarget}) {
    h.quantized(v.x);
    h.quantized(v.y);
    h.quantized(v.z);
  }
  h.quantized(in.angularVelocity);
  h.quantized(in.cameraDistance);
  h.u32(in.tags);
  h.u8(in.hasLookTarget);
  deformScale_ = 1;
  for (unsigned stage = 0; stage < 5; ++stage) {
    auto start = std::chrono::steady_clock::now();
    auto stageStart = pose;
    BoneMask writes;
    std::uint32_t curves = 0;
    for (auto &t : traits_)
      if (static_cast<unsigned>(t.stage) == stage) {
        auto before = pose;
        auto decision = selection;
        t.evaluate({in, selection}, pose);
        require(stage == 0 || (selection.clip == decision.clip &&
                               selection.frame == decision.frame &&
                               selection.cost == decision.cost),
                "NG-E-ANIM-MASK", "Only Select may change decisions");
        for (unsigned b = 0; b < MaxBones; ++b)
          require(!changed(before, pose, b) || t.writes.has(b),
                  "NG-E-ANIM-MASK",
                  "Trait " + std::to_string(t.id) + " wrote undeclared bone " +
                      std::to_string(b));
        for (unsigned c = 0; c < MaxCurves; ++c)
          require(before.curves[c] == pose.curves[c] ||
                      (t.writeCurves & (1u << c)),
                  "NG-E-ANIM-MASK", "Trait wrote undeclared curve");
        validPose(pose, boneCount_);
        if (t.deterministic) {
          if (t.externalDigest)
            t.externalDigest(h);
          for (unsigned b = 0; b < boneCount_; ++b)
            if (t.writes.has(b))
              writes.set(b);
          curves |= t.writeCurves;
        }
      }
    if (stage == 3) {
      float norm = 0;
      for (unsigned b = 1; b < boneCount_; ++b) {
        auto a = stageStart.get(b), v = pose.get(b);
        norm += length(sub(v.translation, a.translation)) +
                0.1f * length(qlog(product(v.rotation, inverse(a.rotation)))) +
                length(sub(v.scale, a.scale));
      }
      deformScale_ = norm > budget ? budget / norm : 1;
      if (deformScale_ < 1)
        pose = blend(stageStart, pose, boneCount_, deformScale_,
                     BoneMask::all(boneCount_));
    }
    if (stage < 4) {
      h.u32(stage);
      if (stage == 0) {
        h.u32(selection.clip);
        h.u32(selection.frame);
        h.u64(selection.cost);
      }
      for (unsigned b = 0; b < boneCount_; ++b)
        if (writes.has(b)) {
          h.u32(b);
          hashBone(h, pose, b);
        }
      for (unsigned c = 0; c < MaxCurves; ++c)
        if (curves & (1u << c)) {
          h.u32(c);
          h.quantized(pose.curves[c]);
        }
    }
    stages_[stage] = {h.value(), std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - start)
                                     .count()};
  }
}
void Inertializer::begin(const PoseBuffer &from, const PoseBuffer &fromPrev,
                         const PoseBuffer &to, const PoseBuffer &toPrev,
                         unsigned count, float dt, float settleSeconds) {
  require(count <= MaxBones && std::isfinite(dt) && dt > 0 &&
              std::isfinite(settleSeconds) && settleSeconds > 0,
          "NG-E-ANIM-BLEND", "Invalid spring step");
  omega_ = 6 / settleSeconds;
  for (unsigned b = 0; b < count; ++b) {
    offset_[b] = difference(from.get(b), to.get(b));
    auto fv = difference(from.get(b), fromPrev.get(b)),
         tv = difference(to.get(b), toPrev.get(b));
    for (unsigned k = 0; k < 9; ++k)
      velocity_[b][k] = (fv[k] - tv[k]) / dt;
  }
}
PoseBuffer Inertializer::apply(const PoseBuffer &target, unsigned count,
                               double age) const {
  require(count <= MaxBones && std::isfinite(age) && age >= 0,
          "NG-E-ANIM-BLEND", "Invalid spring age");
  auto out = target;
  const float t = static_cast<float>(age), omega = omega_,
              decay = std::exp(-omega * t);
  for (unsigned b = 0; b < count; ++b) {
    std::array<float, 9> d;
    for (unsigned k = 0; k < 9; ++k)
      d[k] = (offset_[b][k] + (velocity_[b][k] + omega * offset_[b][k]) * t) *
             decay;
    auto v = target.get(b);
    v.translation = add(v.translation, {d[0], d[1], d[2]});
    v.rotation = product(qexp({d[3], d[4], d[5]}), v.rotation);
    v.scale = add(v.scale, {d[6], d[7], d[8]});
    out.set(b, v);
  }
  return out;
}
void Inertializer::digest(determinism::Hasher &h, unsigned count) const {
  h.quantized(omega_);
  for (unsigned b = 0; b < count; ++b)
    for (unsigned k = 0; k < 9; ++k) {
      h.quantized(offset_[b][k]);
      h.quantized(velocity_[b][k]);
    }
}
const char *stateName(State s) {
  switch (s) {
  case State::Idle:
    return "idle";
  case State::Walk:
    return "walk";
  case State::Run:
    return "run";
  case State::Jump:
    return "jump";
  case State::Fall:
    return "fall";
  }
  return "invalid";
}
void Controller::initialize(std::shared_ptr<const Asset> asset,
                            ControllerConfig config) {
  require(bool(asset), "NG-E-ANIM-SKELETON", "Missing asset");
  asset->validate();
  require(std::isfinite(config.walkSpeed) && std::isfinite(config.runSpeed) &&
              config.walkSpeed > 0 && config.runSpeed > config.walkSpeed &&
              std::isfinite(config.transitionSeconds) &&
              config.transitionSeconds > 0,
          "NG-E-ANIM-GRAPH", "Invalid controller configuration");
  std::array<unsigned, 5> indices;
  for (unsigned i = 0; i < 5; ++i)
    indices[i] = asset->clipIndex(config.clips[i]);
  asset_ = std::move(asset);
  config_ = std::move(config);
  if (!config_.rigOverride)
    config_.rig = asset_->rig.settings;
  const auto &r = config_.rig;
  require(std::isfinite(r.height) && r.height > 0 &&
              std::isfinite(r.turnStepDuration) && r.turnStepDuration > 0 &&
              std::isfinite(r.turnCooldown) && r.turnCooldown >= 0 &&
              std::isfinite(r.turnThreshold) && r.turnThreshold > 0 &&
              std::isfinite(r.turnStepHeight) && r.turnStepHeight >= 0 &&
              std::isfinite(r.maxSlopeCosine) && r.maxSlopeCosine >= 0 &&
              r.maxSlopeCosine <= 1 && std::isfinite(r.maxFootCorrection) &&
              r.maxFootCorrection >= 0 && std::isfinite(r.maxStepHeight) &&
              r.maxStepHeight >= 0 && std::isfinite(r.pelvisLower) &&
              std::isfinite(r.pelvisUpper) && r.pelvisLower <= r.pelvisUpper &&
              std::isfinite(r.maxPelvisShift) && r.maxPelvisShift >= 0 &&
              r.maxPelvisShift <= 1 && std::isfinite(r.soleRoll) &&
              r.soleRoll >= 0 && r.soleRoll <= 1.5f,
          "NG-E-ANIM-RIG", "Invalid rig configuration");
  clips_ = indices;
  // A static idle clip is a pose, not motion. The Correct stage adds a small
  // procedural breathing/weight shift so stopping does not freeze the rig.
  idleStatic_ = asset_->clips[clips_[0]].staticPose();
  database_.build(*asset_);
  initialized_ = true;
  prepareRig();
  makeGraph();
  reset();
}
void Controller::makeGraph() {
  const auto count = static_cast<unsigned>(asset_->skeleton.bones.size());
  Trait selectTrait;
  selectTrait.id = 1;
  selectTrait.stage = Stage::Select;
  selectTrait.evaluate = [this](const Context &c, PoseBuffer &) {
    select(c.intent);
  };
  Trait blendTrait;
  blendTrait.id = 2;
  blendTrait.stage = Stage::Blend;
  blendTrait.after = {1};
  blendTrait.writes = BoneMask::all(count);
  blendTrait.writeCurves = 0xffffffffu;
  blendTrait.evaluate = [this, count](const Context &, PoseBuffer &out) {
    sampleBase(out);
    if (transitioning_)
      out = inertializer_.apply(out, count,
                                double(tick_ - transitionTick_) / TickRate);
    // Gameplay owns translation; the extracted delta is available separately.
    if(!asset_->preserveRootTranslation)out.translation[0] = asset_->skeleton.restPose().translation[0];
    if (montageActive_) {
      auto upper =
          asset_->clips[montageClip_].sample(double(montageTicks_) / TickRate);
      double seconds = double(montageTicks_) / TickRate,
             duration = asset_->clips[montageClip_].duration();
      float envelope = static_cast<float>(
          std::clamp(std::min(seconds, duration - seconds) / 0.10, 0.0, 1.0));
      envelope = envelope * envelope * (3 - 2 * envelope);
      out = blend(out, upper, count, montageWeight_ * envelope, montageMask_);
    }
  };
  Trait correctTrait;
  correctTrait.id = 3;
  correctTrait.stage = Stage::Correct;
  correctTrait.after = {2};
  correctTrait.reads = correctTrait.writes = BoneMask::all(count);
  correctTrait.evaluate = [this](const Context &c, PoseBuffer &out) {
    correct(c.intent, out);
  };
  Trait deformTrait;
  deformTrait.id = 4;
  deformTrait.stage = Stage::Deform;
  deformTrait.after = {3};
  deformTrait.reads = BoneMask::all(count);
  deformTrait.readCurves = 0xffff0000u;
  deformTrait.evaluate = [this](const Context &c, PoseBuffer &out) {
    deform(c.intent, out);
  };
  deformTrait.externalDigest = [this](determinism::Hasher &h) {
    hashFeatures(h);
  };
  graph_.compile({std::move(selectTrait), std::move(blendTrait),
                  std::move(correctTrait), std::move(deformTrait)},
                 count);
}
void Controller::reset() {
  if (!initialized_)
    return;
  state_ = State::Idle;
  tick_ = stateTicks_ = transitionTick_ = montageTicks_ = 0;
  locomotionSeconds_ = 0;
  playbackRate_ = 1;
  stateTransitionTick_ = 0;
  stateTransitionActive_ = false;
  front_ = 0;
  transitioning_ = montageActive_ = false;
  speed_ = 0;
  events_.clear();
  resetFeatures();
  decisions_.clear();
  rootMotion_ = {};
  selection_ = {clips_[0], 0, 0};
  inertializer_ = Inertializer{};
  montageClip_ = 0;
  montageMask_ = {};
  montageWeight_ = 1;
  poses_[0] = poses_[1] = previousPrevious_ =
      asset_->clips[clips_[0]].sample(0);
  if(!asset_->preserveRootTranslation)poses_[0].translation[0] = poses_[1].translation[0] =
      previousPrevious_.translation[0] =
          asset_->skeleton.restPose().translation[0];
  // Initialize the stage chain at tick zero without advancing playback.
  Intent input;
  graph_.evaluate(input, selection_, poses_[0]);
  poses_[1] = previousPrevious_ = poses_[0];
}
double Controller::playbackSeconds() const {
  return (state_ == State::Walk || state_ == State::Run)
             ? locomotionSeconds_
             : double(stateTicks_) / TickRate;
}
void Controller::sampleBase(PoseBuffer &out) const {
  if (features_.matching) {
    out = asset_->clips[features_.matchedClip].sample(features_.matchedSeconds);
    return;
  }
  double seconds = playbackSeconds();
  if (state_ == State::Walk || state_ == State::Run) {
    float w = std::clamp((speed_ - config_.walkSpeed) /
                             (config_.runSpeed - config_.walkSpeed),
                         0.f, 1.f);
    auto a = asset_->clips[clips_[1]].sample(seconds),
         b = asset_->clips[clips_[2]].sample(seconds);
    out = blend(
        a, b, static_cast<unsigned>(asset_->skeleton.bones.size()), w,
        BoneMask::all(static_cast<unsigned>(asset_->skeleton.bones.size())));
  } else
    out = asset_->clips[selection_.clip].sample(seconds);
}
void Controller::select(const Intent &input) {
  speed_ = std::max(0.f, input.speed);
  // Hysteresis keeps the idle pose from flickering when the smoothed speed
  // hovers around the threshold.
  const bool wasIdle = state_ == State::Idle;
  State next =
      !input.grounded
          ? (input.verticalSpeed > 0 ? State::Jump : State::Fall)
          : (speed_ < (wasIdle ? 0.18f : 0.10f)
                 ? State::Idle
                 : (speed_ > (config_.walkSpeed + config_.runSpeed) * 0.5f
                        ? State::Run
                        : State::Walk));
  bool transition = next != state_;
  // Idle is a clip state, never a motion-matching state. Keeping the matcher
  // active at zero speed snaps playback back to the closest contact frame and
  // freezes the character in a walk pose.
  bool match = config_.motionMatching && input.grounded &&
               next != State::Idle && !database_.frames.empty();
  Selection chosen = selection_;
  bool switched = false;
  if (match) {
    if (features_.matching)
      features_.matchedSeconds += 1.0 / TickRate;
    const auto &previous = asset_->clips[selection_.clip];
    unsigned continuingFrame =
        features_.matching ? unsigned(std::fmod(features_.matchedSeconds,
                                                previous.duration()) *
                                      SampleRate)
                           : selection_.frame;
    auto query = database_.query(*asset_, poses_[front_], input);
    // Query history contains measured foot velocities and contact state.
    auto now = asset_->skeleton.globals(poses_[front_]);
    auto prior = asset_->skeleton.globals(previousPrevious_);
    for (unsigned side = 0; side < asset_->rig.legs.size(); ++side) {
      auto bone = asset_->rig.legs[side].tip;
      for (unsigned k = 0; k < 3; ++k) {
        float v = ((now[bone][12 + k] - now[0][12 + k]) -
                   (prior[bone][12 + k] - prior[0][12 + k])) *
                  TickRate;
        query[12 + side * 6 + k] = std::int16_t(
            std::clamp(std::lround(v * database_.scales[12 + side * 6 + k]),
                       -30000l, 30000l));
      }
      query[23 + side] = std::int16_t(
          features_.feet[side].locked ? database_.scales[23 + side] : 0);
    }
    chosen = database_.search(query, input.tags,
                              {selection_.clip, continuingFrame, 0},
                              features_.matching, &metrics_.secondCost);
    if (chosen.cost == std::numeric_limits<std::uint64_t>::max())
      match = false;
    else {
      switched = !features_.matching || chosen.clip != selection_.clip ||
                 std::abs(int(chosen.frame) - int(continuingFrame)) > 3;
      if (switched && features_.matching && tick_ - features_.lastSwitch < 12) {
        chosen = {selection_.clip, continuingFrame, selection_.cost};
        switched = false;
      }
      if (switched) {
        features_.matchedSeconds = double(chosen.frame) / SampleRate;
        features_.lastSwitch = tick_;
        ++metrics_.switches;
      }
      features_.matchedClip = chosen.clip;
    }
  }
  bool wasMatching = features_.matching;
  features_.matching = match;
  // Locomotion shares a clock so changing walk/run preserves phase.
  bool locomotion = (next == State::Walk || next == State::Run) &&
                    (state_ == State::Walk || state_ == State::Run);
  if (transition || switched || wasMatching != match) {
    state_ = next;
    if (transition) {
      stateTransitionTick_ = tick_;
      stateTransitionActive_ = true;
    }
    if (!locomotion) {
      stateTicks_ = 0;
      locomotionSeconds_ = 0;
    }
    selection_.clip =
        match ? chosen.clip : clips_[static_cast<unsigned>(state_)];
    PoseBuffer target;
    sampleBase(target);
    auto prior = asset_->clips[selection_.clip].sample(
        std::max(0.0, playbackSeconds() - 1.0 / TickRate));
    target.translation[0] = prior.translation[0] =
        asset_->skeleton.restPose().translation[0];
    inertializer_.begin(poses_[front_], previousPrevious_, target, prior,
                        static_cast<unsigned>(asset_->skeleton.bones.size()),
                        1.f / TickRate,
                        static_cast<float>(config_.transitionSeconds));
    transitionTick_ = tick_;
    transitioning_ = true;
  }
  if (match) {
    selection_ = chosen;
    selection_.frame =
        unsigned(std::fmod(features_.matchedSeconds,
                           asset_->clips[chosen.clip].duration()) *
                 SampleRate);
    return;
  }
  const auto &clip = asset_->clips[selection_.clip];
  double f = playbackSeconds() * SampleRate;
  selection_.frame =
      clip.loop ? static_cast<unsigned>(std::fmod(f, clip.frameCount - 1))
                : std::min(static_cast<unsigned>(f), clip.frameCount - 1);
}
void Controller::tick(const Intent &in) {
  require(initialized_, "NG-E-ANIM-GRAPH", "Controller is not initialized");
  require(in.tick == tick_ + 1, "NG-E-ANIM-TICK",
          "Animation requires consecutive fixed ticks");
  require(std::isfinite(in.speed) && in.speed >= 0 &&
              std::isfinite(in.verticalSpeed) && std::isfinite(in.heading) &&
              finite(in.position) && finite(in.actualVelocity) &&
              finite(in.desiredVelocity) && finite(in.lookTarget) &&
              std::isfinite(in.angularVelocity) &&
              std::isfinite(in.modelScale) && in.modelScale > 0 &&
              std::isfinite(in.cameraDistance) && in.cameraDistance >= 0,
          "NG-E-ANIM-POSE", "Non-finite animation input");
  // Re-time the locomotion clip to the measured support-phase foot speed. With
  // a 6.4 m/s run on a 3.1 m/s clip, playing at 1x leaves the support foot
  // dragging behind the body and permanently IK-clamped.
  playbackRate_ = 1;
  if (!features_.matching &&
      (state_ == State::Walk || state_ == State::Run)) {
    const auto &walkClip = asset_->clips[clips_[1]];
    const auto &runClip = asset_->clips[clips_[2]];
    const float w = std::clamp(
        (speed_ - config_.walkSpeed) /
            std::max(1e-4f, config_.runSpeed - config_.walkSpeed),
        0.f, 1.f);
    float reference =
        (walkClip.locomotionSpeed * (1 - w) + runClip.locomotionSpeed * w) *
        std::max(1e-4f, in.modelScale);
    if (!(reference > .05f))
      reference = config_.walkSpeed * (1 - w) + config_.runSpeed * w;
    playbackRate_ =
        std::clamp(in.speed / std::max(.05f, reference), .35f, 3.f);
    locomotionSeconds_ += double(playbackRate_) / TickRate;
  }
  tick_ = in.tick;
  events_.clear();
  ++stateTicks_;
  if (montageActive_)
    ++montageTicks_;
  auto priorTicks = stateTicks_ - 1;
  bool priorMatching = features_.matching;
  unsigned priorClip = selection_.clip;
  auto priorMatchTicks = static_cast<std::uint64_t>(
      std::llround(features_.matchedSeconds * TickRate));
  auto &back = poses_[1 - front_];
  back = asset_->skeleton.restPose();
  graph_.evaluate(in, selection_, back);
  if (decisions_.size() == 4096)
    decisions_.erase(decisions_.begin());
  decisions_.push_back({in.tick, selection_, metrics_.secondCost});
  const auto &c = asset_->clips[selection_.clip];
  if (stateTicks_ == 0)
    priorTicks = 0;
  if (features_.matching) {
    auto current = static_cast<std::uint64_t>(
        std::llround(features_.matchedSeconds * TickRate));
    bool continuous = priorMatching && priorClip == selection_.clip &&
                      current == priorMatchTicks + 1;
    if (continuous) {
      rootMotion_ = c.rootDelta(double(priorMatchTicks) / TickRate,
                                double(current) / TickRate);
      notifyRange(c, selection_.clip, priorMatchTicks, current, tick_, events_);
    } else
      rootMotion_ = {};
  } else {
    rootMotion_ = c.rootDelta(double(priorTicks) / TickRate,
                              double(stateTicks_) / TickRate);
    notifyRange(c, selection_.clip, priorTicks, stateTicks_, tick_, events_);
  }
  if (montageActive_) {
    const auto &m = asset_->clips[montageClip_];
    notifyRange(m, montageClip_, montageTicks_ - 1, montageTicks_, tick_,
                events_);
    if (double(montageTicks_) / TickRate >= m.duration())
      montageActive_ = false;
  }
  if(asset_->preserveRootTranslation)rootMotion_={};
  previousPrevious_ = poses_[front_];
  front_ = 1 - front_;
  auto deformStart = std::chrono::steady_clock::now();
  rebuildDeformation();
  graph_.addDeformationTime(std::chrono::duration<double, std::micro>(
                                std::chrono::steady_clock::now() - deformStart)
                                .count());
  // Let the analytic tail decay below the digest grid before disabling it.
  if (transitioning_ && double(tick_ - transitionTick_) / TickRate >=
                            std::max(1.0, config_.transitionSeconds * 4))
    transitioning_ = false;
}
void Controller::play(const std::string &name, const BoneMask &mask,
                      float weight) {
  require(initialized_ && std::isfinite(weight) && weight >= 0 && weight <= 1,
          "NG-E-ANIM-GRAPH", "Invalid montage");
  for (unsigned b = 0; b < MaxBones; ++b)
    require(!mask.has(b) || (b > 0 && b < asset_->skeleton.bones.size()),
            "NG-E-ANIM-MASK", "Montage cannot modify root or missing bones");
  unsigned c = asset_->clipIndex(name);
  require(!asset_->clips[c].loop, "NG-E-ANIM-CLIP",
          "Montage must be non-looping");
  montageClip_ = c;
  montageMask_ = mask;
  montageWeight_ = weight;
  montageTicks_ = 0;
  montageActive_ = true;
}
void Controller::digest(determinism::Hasher &h) const {
  h.text("nativegpu.animation-controller/1");
  h.u8(initialized_);
  if (!initialized_)
    return;
  h.text(asset_->contentHash);
  hashFeatures(h);
  h.u64(tick_);
  h.u64(stateTicks_);
  h.u64(transitionTick_);
  h.u8(static_cast<unsigned char>(state_));
  h.u8(transitioning_);
  h.u8(montageActive_);
  h.u32(montageClip_);
  h.u64(montageTicks_);
  h.quantized(montageWeight_);
  for (auto b : montageMask_.bits)
    h.u64(b);
  h.quantized(speed_);
  h.quantized(float(locomotionSeconds_));
  h.quantized(playbackRate_);
  h.u8(stateTransitionActive_);
  h.u64(stateTransitionTick_);
  h.quantized(config_.walkSpeed);
  h.quantized(config_.runSpeed);
  h.quantized(static_cast<float>(config_.transitionSeconds));
  h.u8(config_.rigOverride);
  const auto &rs = config_.rig;
  for (float value : {rs.height, rs.turnThreshold, rs.turnStepHeight,
                      rs.turnStepDuration, rs.turnCooldown, rs.maxTwist,
                      rs.maxSlopeCosine, rs.swingPrediction,
                      rs.maxFootCorrection, rs.pelvisLower, rs.pelvisUpper,
                      rs.maxStepHeight, rs.maxPelvisShift, rs.soleRoll})
    h.quantized(value);
  h.u8(rs.footLock);
  h.u8(rs.turnInPlace);
  for (auto clip : clips_)
    h.u32(clip);
  for (const auto &stage : graph_.stages())
    h.u64(stage.hash);
  if (transitioning_)
    inertializer_.digest(h,
                         static_cast<unsigned>(asset_->skeleton.bones.size()));
  for (unsigned b = 0; b < asset_->skeleton.bones.size(); ++b) {
    hashBone(h, pose(), b);
    hashBone(h, previousPrevious_, b);
  }
  for (const auto &e : events_) {
    h.u64(e.tick);
    h.text(e.name);
    h.u32(e.clip);
  }
}
} // namespace engine::animation
