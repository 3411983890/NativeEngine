#pragma once

namespace engine::gameplay {
struct NativeSnapshot;
}

#include "determinism.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::animation {
constexpr unsigned MaxBones = 256, MaxCurves = 32, SampleRate = 60,
                   TickRate = 120;
// The upper half of the shared curve buffer carries authored morph weights.
constexpr unsigned MorphCurveBase = 16, MaxMorphTargets = 16;
struct Error : std::runtime_error {
  std::string code;
  Error(const char *code, const std::string &message);
};
struct V3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
using Matrix = std::array<float, 16>;
struct Transform {
  V3 translation;
  Quat rotation;
  V3 scale{1, 1, 1};
};
Matrix matrix(const Transform &transform);
Matrix multiply(const Matrix &a, const Matrix &b);
V3 point(const Matrix &m, V3 p);
Quat normalized(Quat q);
Quat slerp(Quat a, Quat b, float weight);
struct BoneMask {
  std::array<std::uint64_t, 4> bits{};
  bool has(unsigned bone) const;
  void set(unsigned bone);
  static BoneMask all(unsigned count);
};
struct PoseBuffer {
  std::array<std::array<float, 3>, MaxBones> translation{};
  std::array<std::array<float, 4>, MaxBones> rotation{};
  std::array<std::array<float, 3>, MaxBones> scale{};
  std::array<float, MaxBones> layerWeight{};
  std::array<float, MaxCurves> curves{};
  PoseBuffer();
  Transform get(unsigned bone) const;
  void set(unsigned bone, const Transform &value);
};
struct Bone {
  std::string name;
  int parent = -1;
  Transform rest;
  Matrix inverseBind{};
};
struct Skeleton {
  std::vector<Bone> bones;
  void validate() const;
  PoseBuffer restPose() const;
  std::vector<Matrix> globals(const PoseBuffer &pose) const;
  std::vector<Matrix> palette(const PoseBuffer &pose) const;
};
struct Notify {
  unsigned frame = 0;
  std::string name;
};
struct Clip {
  std::string name;
  unsigned boneCount = 0, frameCount = 0;
  bool loop = true;
  std::vector<Transform> samples; // frame-major, including the endpoint
  std::vector<std::array<float, MaxMorphTargets>> morphSamples;
  std::uint16_t stepMorphMask = 0;
  std::array<std::uint8_t, MaxBones>
      stepChannels{}; // translation=1, rotation=2, scale=4
  std::vector<Notify> notifies;
  V3 motionVelocity; // Authored in-place velocity, or zero when root tracks
                     // supply it.
  float turnRate = 0;
  // Inferred support-phase foot speed in model units per second. Zero means
  // unknown; gameplay then matches playback to its configured walk/run speed.
  float locomotionSpeed = 0;
  bool matchable = false;
  unsigned tags = 1;
  std::array<std::array<float, 2>, 2> contacts{{{{0, 0}}, {{0, 0}}}};
  // Contacts are only used for support locking when the source is authored or
  // the inferred interval is reliable. Legacy clips without metadata stay
  // unknown instead of assuming an arbitrary half cycle.
  enum class ContactSource : std::uint8_t { Unknown, Authored, Inferred };
  ContactSource contactSource = ContactSource::Unknown;
  float contactConfidence = 0;
  bool contactKnown() const {
    return contactSource == ContactSource::Authored ||
           (contactSource == ContactSource::Inferred &&
            contactConfidence >= .5f);
  }
  double duration() const;
  // True when every sampled bone is identical to frame zero. Static idle clips
  // get a procedural breathing/weight-shift layer instead of freezing.
  bool staticPose() const;
  PoseBuffer sample(double seconds) const;
  V3 rootDelta(double from, double to) const;
};
PoseBuffer blend(const PoseBuffer &a, const PoseBuffer &b, unsigned count,
                 float weight, const BoneMask &mask);
struct BlendPoint {
  float x = 0, y = 0;
  unsigned clip = 0;
};
struct BlendWeight {
  unsigned clip = 0;
  float weight = 0;
};
std::vector<BlendWeight> blendSpace1D(std::vector<BlendPoint> points, float x);
// Explicit triangles avoid topology depending on floating point triangulation.
std::vector<BlendWeight>
blendSpace2D(const std::vector<BlendPoint> &points,
             const std::vector<std::array<unsigned, 3>> &triangles, float x,
             float y);

struct SkinVertex {
  float position[3]{}, normal[3]{}, uv[2]{};
  std::uint32_t joints[4]{};
  float weights[4]{1, 0, 0, 0};
  float textureUvs[4][2]{};
  float color[4]{1,1,1,1};
  float tangent[4]{};
  float layerUvs[3][2]{};
};
struct MorphDelta {
  unsigned vertex = 0;
  V3 position, normal, tangent;
};
struct MorphTarget {
  std::string name;
  std::vector<MorphDelta> deltas;
};
struct RigChain {
  unsigned root = 0, joint = 0, tip = 0;
  int toe = -1;
  V3 pole{0, 0, -1};
  float minBend = 0.01f, maxBend = 2.9f, soleHeight = 0.06f;
  float footLength = 0.15f;
};
struct RigSettings {
  float height = 1.8f;
  float turnThreshold = .35f;
  float turnStepHeight = .09f;
  float turnStepDuration = .28f;
  float turnCooldown = .12f;
  float maxTwist = 1.f;
  float maxSlopeCosine = .7071068f;
  float swingPrediction = .45f;
  float maxFootCorrection = .35f;
  float pelvisLower = -.22f;
  float pelvisUpper = .08f;
  float maxStepHeight = .25f;
  // Horizontal pelvis travel the solver may use to keep a support foot
  // reachable before it gives up and releases the contact. Model units, scaled
  // by the instance scale like every other rig length.
  float maxPelvisShift = .07f;
  // Sole pitch limit for the toe-off roll, in radians. Zero keeps the sole
  // pinned flat, which is the previous behaviour.
  float soleRoll = .45f;
  bool footLock = true, turnInPlace = true;
};
struct RigDefinition {
  int pelvis = -1, lookBone = -1, spine = -1;
  std::vector<RigChain> legs;
  // Upper arm -> forearm -> hand, left first. Optional: a rig without arms keeps
  // its procedural feet and reports no hand contacts. The contact layer drives
  // these chains exactly like a leg (two-bone solve, surface-local anchor), so a
  // palm on a wall, a hand on a rail or a climbing hold needs no special case.
  std::vector<RigChain> arms;
  RigSettings settings;
  std::string source;
  float confidence = 0;
  std::vector<std::string> warnings;
};
struct MuscleDriver {
  unsigned bone = 0, target = 0;
  float angleStart = 0, angleEnd = 1.5f, maxWeight = 1;
};
struct SoftRegion {
  unsigned bone = 0;
  V3 center;
  float radius = 0.12f, maxOffset = 0.035f;
  std::vector<unsigned> vertices;
};
struct MeshPart {
  std::string name, material;
  unsigned firstIndex = 0, indexCount = 0;
};
struct Asset {
  Skeleton skeleton;
  std::vector<Clip> clips;
  std::vector<SkinVertex> vertices;
  bool hasTextureUvs = false;
  bool hasLayerUvs = false;
  bool hasVertexColors = false;
  bool hasTangents = false;
  std::vector<std::uint32_t> indices;
  std::vector<MeshPart> meshParts;
  RigDefinition rig;
  std::vector<MorphTarget> morphs;
  std::vector<MuscleDriver> muscles;
  std::vector<SoftRegion> softRegions;
  std::string contentHash;
  std::string geometryHash; // Immutable mesh/morph bytes; shared by clip variants.
  std::array<std::string,5> controllerClips{{"idle","walk","run","jump","fall"}};
  std::string chestBone="chest", rightHandBone="right_hand";
  bool preserveRootTranslation = false;
  void validate() const;
  unsigned clipIndex(const std::string &name) const;
};
// Loading is atomic; errors never leave a partially usable asset or a T-pose
// fallback.
std::shared_ptr<const Asset> loadAsset(const std::filesystem::path &header);
std::shared_ptr<const Asset> loadAssetData(const std::vector<std::uint8_t>& header,const std::vector<std::uint8_t>& binary);

enum class Stage : unsigned { Select, Blend, Correct, Deform, Emit };
struct Intent {
  Intent() = default;
  Intent(std::uint64_t t, float s, float vertical, float h, bool g)
      : tick(t), speed(s), verticalSpeed(vertical), heading(h), grounded(g) {}
  std::uint64_t tick = 0;
  float speed = 0, verticalSpeed = 0, heading = 0;
  bool grounded = true;
  V3 position, actualVelocity, desiredVelocity;
  float angularVelocity = 0, cameraDistance = 2;
  // Uniform model-to-world scale used by the character reference. Rig sizes are
  // authored in model space; world ground targets must apply this scale.
  float modelScale = 1;
  unsigned tags = 1;
  bool hasLookTarget = false;
  V3 lookTarget;
  // Facing the body is being steered toward when it differs from `heading`.
  // Gameplay holds the body still while both feet are planted, so a turn has to
  // be decided from the desired facing; otherwise the model only starts turning
  // after the residual has already twisted the support foot.
  bool hasTurnTarget = false;
  float turnTarget = 0;
};
struct GroundHit {
  bool hit = false;
  unsigned surface = 0;
  V3 point, normal{0, 1, 0}, velocity;
  Matrix surfaceToWorld{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
};
using GroundQuery = std::function<GroundHit(V3 origin, float distance)>;
struct IKResult {
  float error = 0;
  bool clamped = false;
};
IKResult solveTwoBone(const Skeleton &, PoseBuffer &, const RigChain &,
                      V3 target, V3 pole);
void solveLookAt(const Skeleton &, PoseBuffer &, unsigned bone, V3 target,
                 float maxAngle = 1.1f);
V3 inversePoint(const Matrix &, V3);
struct Selection {
  unsigned clip = 0, frame = 0;
  std::uint64_t cost = 0;
};
struct Context {
  const Intent &intent;
  Selection &selection;
};
struct Trait {
  std::uint32_t id = 0;
  Stage stage = Stage::Blend;
  int priority = 0;
  BoneMask reads, writes;
  std::uint32_t readCurves = 0, writeCurves = 0;
  bool deterministic = true;
  std::vector<std::uint32_t> after;
  std::function<void(const Context &, PoseBuffer &)> evaluate;
  std::function<void(determinism::Hasher &)> externalDigest;
};
struct StageResult {
  std::uint64_t hash = 0;
  double microseconds = 0;
};
class Graph {
public:
  void compile(std::vector<Trait> traits, unsigned boneCount);
  void evaluate(const Intent &intent, Selection &selection, PoseBuffer &pose,
                float deformBudget = 0.02f);
  const std::array<StageResult, 5> &stages() const { return stages_; }
  float lastDeformScale() const { return deformScale_; }
  void addDeformationTime(double microseconds) {
    stages_[3].microseconds += microseconds;
  }

private:
  friend struct engine::gameplay::NativeSnapshot;
  unsigned boneCount_ = 0;
  std::vector<Trait> traits_;
  std::array<StageResult, 5> stages_{};
  float deformScale_ = 1;
};
// Quaternion offsets use log/exp, translations and scale use analytic
// critically damped springs. Age is absolute time, never integrated per frame.
class Inertializer {
public:
  void begin(const PoseBuffer &previous, const PoseBuffer &previousPrevious,
             const PoseBuffer &target, const PoseBuffer &targetPrevious,
             unsigned count, float dt, float settleSeconds = 0.25f);
  PoseBuffer apply(const PoseBuffer &target, unsigned count, double age) const;
  void digest(determinism::Hasher &h, unsigned count) const;

private:
  friend struct engine::gameplay::NativeSnapshot;
  std::array<std::array<float, 9>, MaxBones> offset_{}, velocity_{};
  float omega_ = 24;
};
enum class State { Idle, Walk, Run, Jump, Fall };
const char *stateName(State state);
struct Event {
  std::uint64_t tick = 0;
  std::string name;
  unsigned clip = 0;
};
struct ControllerConfig {
  std::array<std::string, 5> clips{{"idle", "walk", "run", "jump", "fall"}};
  float walkSpeed = 3, runSpeed = 6;
  double transitionSeconds = 0.25;
  bool motionMatching = true, proceduralRig = true, muscles = true,
       softTissue = true;
  // Scene instances may override the asset's diagnosed rig settings. Keeping
  // the whole effective block in the snapshot makes continuation deterministic.
  bool rigOverride = false;
  RigSettings rig;
};
constexpr unsigned MotionDimensions = 26;
struct MotionFrame {
  unsigned clip = 0, frame = 0, tags = 1;
  std::array<std::int16_t, MotionDimensions> feature{};
};
struct MotionDatabase {
  std::vector<MotionFrame> frames;
  std::array<float, MotionDimensions> scales{};
  void build(const Asset &);
  std::array<std::int16_t, MotionDimensions>
  query(const Asset &, const PoseBuffer &, const Intent &) const;
  Selection search(const std::array<std::int16_t, MotionDimensions> &,
                   unsigned tags, Selection continuing, bool bias,
                   std::uint64_t *secondCost = nullptr) const;
};
enum class FootPhase : std::uint8_t { Swing, Release, Landing, Support };
// Which part of the sole carries the contact. Free means the foot is not
// constrained by the ground; Flat pins the whole sole plane, Toe and Heel let
// the ankle pitch about the front or the back of the foot while that edge
// stays put. This is what keeps a toe-off from reading as a frozen shoe.
enum class ContactMode : std::uint8_t { Free, Flat, Heel, Toe };
// Why a support foot stopped being anchored. Diagnostics only: the solver state
// itself is `FootState::locked`.
enum class ContactRelease : std::uint8_t {
  None,
  Lift,
  Drift,
  Overreach,
  Turn,
  Surface,
  Unwalkable,
  GroundLost,
  Teleport,
  Blend,
  // The stride ran out of leg: the body has walked far enough past the anchor
  // that the next tick would over-extend. Planned, not a failure — the stance
  // ends one tick early so the sole is never dragged by the IK clamp.
  Stride,
  // A generic contact whose validity window ran out. The hand that let go of the
  // rail, not a solver failure.
  Expired
};
enum class RigFallback : std::uint8_t {
  None,
  Disabled,
  NotGrounded,
  NoGround,
  Unwalkable,
  NoLanding,
  Unreachable,
  Teleport
};
const char *footPhaseName(FootPhase phase);
const char *contactModeName(ContactMode mode);
const char *contactReleaseName(ContactRelease reason);
const char *rigFallbackName(RigFallback reason);
struct FootState {
  bool locked = false;
  unsigned surface = 0;
  V3 anchor, previous, normal{0, 1, 0};
  float weight = 0;
  FootPhase phase = FootPhase::Swing;
  ContactMode mode = ContactMode::Free;
  ContactRelease release = ContactRelease::None;
  V3 target, targetNormal{0, 1, 0};
  V3 swingStart;
  V3 swingEnd;
  float swingTime = 0, swingDuration = 0;
  bool turnStep = false;
  float cooldown = 0;
  bool contact = false;
  // Ankle position the clip draws for this tick, in world space. Its world
  // velocity decides whether the clip is actually planting the foot; a pin that
  // starts while the clip is still swinging is what drags a leg behind the body.
  V3 animated;
  // Gameplay heading when this anchor was planted. The turn residual is the
  // heading change since then; using the anchor vector itself would grow during
  // ordinary forward locomotion.
  float anchorHeading = 0;
  // Sole pitch about the active contact edge, in radians, positive lifting the
  // toe. Applied to the ankle target and to the foot bone, never to the pivot.
  float roll = 0;
  // Ankle target of the flat sole before the roll is applied, in world space.
  // The reach check needs it: a rolled target can sit further from the hip than
  // the leg can extend, and when that happens the pitch has to give way, not the
  // contact point.
  V3 flat;
  // Hip-to-anchor distance, in model units, when this contact was established.
  // The stance ends when the body has walked far enough past the foot to add to
  // it, which is why the release test uses growth and not an absolute length: a
  // rig whose rest pose stands on straight legs is already at full extension
  // before it moves at all.
  float reachAtPlant = 0;
  // World position of the active contact point on the surface. Slip is measured
  // here rather than at the ankle: rolling over the toe is supposed to move the
  // ankle, so an ankle-based metric reports a slide that is not happening.
  V3 pivot, previousPivot;
  bool pivotValid = false;
  // Roll that `previousPivot` was measured with. The active edge is chosen from
  // the sign of the roll, so a sign change moves the pivot to another point on
  // the shoe and the measurement has to restart.
  float pivotRoll = 0;
  // The foot is rotating on purpose this tick (turn pivot, sole roll), so the
  // contact motion it produces is classified as intentional, not as slip.
  bool pivoting = false;
  // `pivoting` on the previous tick. An intentional rotation moves the point the
  // next measurement is taken from, so the comparison has to start over instead
  // of reporting the turn's own displacement as a slide.
  bool pivoted = false;
};
// A contact that is not a foot: a palm on a wall, a hand on a rail, a climbing
// hold. The feet keep their dedicated solver — the phase machine, the sole roll
// and the step planner are not generic — but every other body part goes through
// this one contract, so a climbing rig or a quadruped reuses it instead of
// growing a third special case. The anchor lives in the contact surface's local
// space, exactly like a foot anchor, so a moving platform carries the hold.
// Surface id used when a hold is not on anything the ground query knows: a rail,
// a wall, a climbing hold. The anchor is then world-fixed and no per-tick surface
// refresh happens.
constexpr unsigned WorldContactSurface = 0xFFFFFFFFu;

struct ContactConstraint {
  bool active = false;
  // Bound on the first tick after being requested: the surface is looked up from
  // the point the caller gave, and from then on the anchor is surface-relative.
  bool bound = false;
  unsigned id = 0;
  int chain = -1;                 // index into Controller::contactChains()
  V3 anchor;                      // contact point in surface space
  unsigned surface = WorldContactSurface;
  V3 pose;                        // world position of the anchor this tick
  V3 normal{0, 1, 0};
  // How much of the solve drives this contact, 0..1. Ramped in and out so a hand
  // that grabs or lets go never snaps the arm.
  float weight = 0;
  // Share of the shared pelvis solve this contact asks for, 0..1. Higher wins
  // when two contacts pull the hips in opposite directions.
  float priority = .5f;
  // A contact that is allowed to slide along its surface: a palm pushing a wall,
  // a hand riding a handrail. It still drives the limb to the point, but it does
  // not vote for the pelvis and the anchor follows the solved bone, so the slide
  // is never reported as tracking error.
  bool slip = false;
  ContactRelease release = ContactRelease::None;
  // Tick after which the contact is dropped on its own; 0 means "until released".
  std::uint64_t validUntil = 0;
  // Last solved position of the contacting bone, for the slip measurement.
  V3 previous;
  bool measured = false;
  float error = 0;
};

struct SoftParticle {
  V3 position, previous, velocity;
};
struct SoftState {
  std::array<SoftParticle, 4> particles{};
  V3 previousCenter;
  bool initialized = false;
};
struct FeatureState {
  std::array<FootState, 2> feet{};
  // Non-foot contacts, in registration order. Part of the deterministic state:
  // a restored save has to keep the hand on the rail.
  std::vector<ContactConstraint> contacts;
  std::array<float, MaxMorphTargets> morphWeights{};
  std::vector<SoftState> soft;
  V3 previousPosition;
  float previousHeading = 0, pelvisOffset = 0, lodWeight = 1;
  // Horizontal pelvis compensation, world space (y unused). Solved together
  // with the vertical offset from every active contact instead of letting one
  // leg claim the pelvis, which is what biased double support to the left foot.
  V3 pelvisShift;
  int steppingFoot = -1;
  float turnResidual = 0;
  unsigned turnSteps = 0;
  RigFallback fallback = RigFallback::None;
  std::uint64_t lastSwitch = 0;
  unsigned matchedClip = 0;
  double matchedSeconds = 0;
  bool matching = false, hasPreviousPosition = false;
};
struct FeatureMetrics {
  unsigned databaseFrames = 0, lockedFeet = 0, switches = 0, softPoints = 0;
  unsigned groundQueries = 0, swingCount = 0, turnSteps = 0, ikClamped = 0;
  std::uint64_t secondCost = 0;
  float footSlip = 0, penetration = 0, ikError = 0, muscleWeight = 0;
  float softMaxOffset = 0, softVolumeError = 0, softPenetration = 0;
  float turnResidual = 0;
  // Contact quality, measured on the sole instead of on the ankle target.
  // `anchorError` is the solver residual of a locked foot, `contactSlip` the
  // speed of the contact point relative to its surface, `pivotSlip` the part of
  // that motion an intentional pivot asked for, `soleAngle` the angle between
  // the sole plane and the surface, and `switchJump` the ankle discontinuity of
  // a support hand-over.
  float anchorError = 0, contactSlip = 0, pivotSlip = 0, soleAngle = 0,
        switchJump = 0;
  float pelvisShiftUsed = 0;
  unsigned supportSwitches = 0, toeOffs = 0, heelStrikes = 0;
  // Ticks where the sole pitch was cut because the rolled ankle target no longer
  // fit inside the leg's reach. A non-zero count with a near-zero `contactSlip`
  // is the intended trade: less toe-off, no slide.
  unsigned rollLimited = 0;
  // Generic contacts: how many were driven this tick, and the worst residual
  // between the contacting bone and its anchor.
  unsigned activeContacts = 0;
  float contactError = 0;
  // Slide speed of a pinned hold, measured at the contacting bone. Contacts that
  // declared themselves free to slip are excluded, the same way `pivotSlip`
  // keeps intentional foot pivots out of `contactSlip`.
  float holdSlip = 0;
  // Releases by reason, in ContactRelease order. Excluded from the digest: a
  // metric describing the last tick is not simulation state.
  std::array<unsigned, 12> releaseCounts{};
};
struct VertexDelta {
  V3 position, normal;
}; // Model-space post-skin deformation.
struct DecisionLog {
  std::uint64_t tick = 0;
  Selection chosen;
  std::uint64_t secondCost = 0;
};
class Controller {
public:
  Controller() = default;
  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;
  Controller(Controller &&) = delete;
  Controller &operator=(Controller &&) = delete;
  void initialize(std::shared_ptr<const Asset> asset,
                  ControllerConfig config = {});
  void reset();
  void tick(const Intent &intent);
  void play(const std::string &clip, const BoneMask &mask, float weight = 1);
  const PoseBuffer &pose() const { return poses_[front_]; }
  const Asset *asset() const { return asset_.get(); }
  State state() const { return state_; }
  const Selection &selection() const { return selection_; }
  const std::array<StageResult, 5> &stages() const { return graph_.stages(); }
  const std::vector<Event> &events() const { return events_; }
  V3 rootMotion() const { return rootMotion_; }
  void setGroundQuery(GroundQuery query) { groundQuery_ = std::move(query); }
  const FeatureState &featureState() const { return features_; }
  const FeatureMetrics &featureMetrics() const { return metrics_; }
  // True while a foot is off the ground, either a natural swing or a turn step.
  // Gameplay may only advance the body facing then: rotating a planted foot is
  // a slide, which is the "the whole model spins on its heels" artifact.
  bool feetAirborne() const {
    if (features_.steppingFoot >= 0)
      return true;
    for (unsigned side = 0; side < features_.feet.size(); ++side) {
      const auto phase = features_.feet[side].phase;
      if (phase == FootPhase::Swing || phase == FootPhase::Release)
        return true;
    }
    return false;
  }
  const RigSettings &rigSettings() const { return config_.rig; }
  // Locomotion clips are re-timed to the measured support speed; exposed for
  // diagnostics and regression tests.
  float playbackRate() const { return playbackRate_; }
  const std::vector<VertexDelta> &deformation() const { return deformation_; }
  const MotionDatabase &motionDatabase() const { return database_; }
  const std::vector<DecisionLog> &decisions() const { return decisions_; }
  // --- Generic contacts -----------------------------------------------------
  // A two-bone chain the contact layer may drive (shoulder -> elbow -> wrist for
  // a hand, hip -> knee -> ankle for a foot). Registering a chain does nothing by
  // itself; a contact has to hold a point on one.
  unsigned addContactChain(const RigChain &chain);
  const std::vector<RigChain> &contactChains() const { return contactChains_; }
  // Index of a rig-provided arm chain in `contactChains()`: left = 0, right = 1.
  // -1 when the asset has no such arm, which is how a caller knows a hand hold
  // is unavailable instead of guessing bone indices.
  int armChain(unsigned side) const {
    return side < armChains_.size() ? armChains_[side] : -1;
  }
  std::uint64_t tickCount() const { return tick_; }
  // Hold `worldPoint` with the tip of `chain`. The surface is resolved from the
  // ground query on the first tick, then stored surface-local so a moving
  // platform carries the contact. Re-issuing the same id updates the hold, so a
  // caller can refresh it every frame; `expires` (0 = never) is an absolute tick.
  // Returns false when the chain does not exist or no slot is free.
  bool setContact(unsigned id, int chain, V3 worldPoint, V3 normal,
                  float priority = .5f, std::uint64_t expires = 0,
                  bool allowSlip = false);
  // Stop holding. The weight ramps out, the arm is handed back to the clip, and
  // the release reason is recorded.
  void releaseContact(unsigned id, ContactRelease reason = ContactRelease::Lift);
  const std::vector<ContactConstraint> &contacts() const {
    return features_.contacts;
  }
  // Restore derived render data without advancing simulation.
  void rebuildDeformation();
  void digest(determinism::Hasher &h) const;

private:
  friend struct engine::gameplay::NativeSnapshot;
  void makeGraph();
  void select(const Intent &intent);
  void sampleBase(PoseBuffer &out) const;
  void correct(const Intent &, PoseBuffer &);
  void deform(const Intent &, const PoseBuffer &);
  void applyIdleMotion(const Intent &, PoseBuffer &);
  // Advances every active generic contact: binds the surface, tests reach,
  // ramps the weight and reports the release reason. Runs before the pelvis
  // solve so a contact can vote for where the hips have to be.
  void updateContacts(const Intent &in, const PoseBuffer &pose,
                      V3 &pelvisNeed, float &pelvisWeight);
  // Solves the chains after the pelvis has moved.
  void solveContacts(const Intent &in, PoseBuffer &pose);
  void resetFeatures();
  void prepareRig();
  void resetFeet(bool teleport);
  void hashFeatures(determinism::Hasher &) const;
  double playbackSeconds() const;
  std::shared_ptr<const Asset> asset_;
  ControllerConfig config_;
  std::array<unsigned, 5> clips_{};
  std::array<PoseBuffer, 2> poses_;
  PoseBuffer previousPrevious_;
  unsigned front_ = 0;
  Graph graph_;
  Selection selection_;
  State state_ = State::Idle;
  std::uint64_t tick_ = 0, stateTicks_ = 0, transitionTick_ = 0,
                montageTicks_ = 0;
  std::uint64_t stateTransitionTick_ = 0;
  bool stateTransitionActive_ = false;
  double locomotionSeconds_ = 0;
  float playbackRate_ = 1;
  bool initialized_ = false, transitioning_ = false, montageActive_ = false;
  bool idleStatic_ = false;
  unsigned montageClip_ = 0;
  BoneMask montageMask_;
  float montageWeight_ = 1, speed_ = 0;
  Inertializer inertializer_;
  V3 rootMotion_;
  std::vector<Event> events_;
  GroundQuery groundQuery_;
  MotionDatabase database_;
  FeatureState features_;
  FeatureMetrics metrics_;
  std::array<V3, 2> stanceOffset_{};
  std::array<Quat, 2> restFootRotation_{};
  std::array<Quat, 2> restToeRotation_{};
  std::array<float, 2> legReach_{};
  // Chains the generic contact layer may drive, and their maximum reach in model
  // units (upper + lower segment). The rig arms are registered here at
  // initialization; an application may append more (a tail, a climbing tool).
  std::vector<RigChain> contactChains_;
  std::vector<float> contactReach_;
  // Indices into `contactChains_` for the rig arms, -1 when the asset has none.
  std::array<int, 2> armChains_{-1, -1};
  std::vector<VertexDelta> deformation_;
  std::vector<DecisionLog>
      decisions_; // Bounded diagnostics, excluded from simulation snapshots.
};
} // namespace engine::animation
