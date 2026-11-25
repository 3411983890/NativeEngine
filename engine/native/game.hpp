#pragma once

#include <string>
#include <functional>
#include <vector>
#include <array>

#include <memory>

#include "determinism.hpp"
#include "animation.hpp"
#include "convex_mesh.hpp"
#include "fracture.hpp"
#include "vec3.hpp"
#include "physics.hpp"
#include "water/body.hpp"
#include "water/coupling.hpp"
#include "gameplay/runtime.hpp"
#include "material_sampling.hpp"
#include "material_ior.hpp"
#include "material_layer_asset.hpp"
#include "assets/environment.hpp"

namespace engine {

// Scene objects use center positions and full extents. A player object's
// position is the spawn location of its feet.
struct Material {
    float albedo[3]{0.65f, 0.65f, 0.65f};
    float roughness = 0.72f;
    float metalness = 0.0f;
    std::string pattern = "checker";
};

// A named, cooked material. Texture names are base names: "brick_red" resolves to
// brick_red.albedo.ngtex, brick_red.normal.ngtex and brick_red.orm.ngtex at load time.
struct MaterialDefinition {
    std::string name;
    std::string albedo;
    std::string normal;
    std::string orm;
    float uvScale = 1.0f;
    float roughness = 1.0f;
    float metalness = 0.0f;
    // Surface-history controls: 0 = dry/pristine, 1 = soaked/worn.
    float wetness = 0.0f;
    float wear = 0.0f;
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.1f;
    float ior = 1.5f;
    bool alphaMasked = false;
    bool alphaBlended = false;
    float opacity = 1.0f;
    float alphaCutoff = 0.5f;
    std::string emissive;
    float emissiveFactor[3]{1.0f,1.0f,1.0f};
    float emissiveStrength = 1.0f;
    std::string occlusion;
    std::string coat,coatRoughness,coatNormal;
    float occlusionStrength = 1.0f;
    bool unlit = false;
    TextureMapping textureMappings[8];
    bool hasTextureMappings = false;
    gameplay::Json layeredAuthoring;
    std::vector<material::layered::LayerAsset> layers;
    material::layered::SurfaceReliefAsset surfaceRelief;
};
inline nlohmann::json materialSamplingIdentity(const MaterialDefinition& material){
    auto value=nlohmann::json::array();const unsigned count=(material.textureMappings[5].sampler||material.textureMappings[6].sampler||material.textureMappings[7].sampler)?8:material.textureMappings[4].sampler?5:4;for(unsigned i=0;i<count;i++){const auto& mapping=material.textureMappings[i];value.push_back({{"row0",{mapping.row0[0],mapping.row0[1],mapping.row0[2]}},{"row1",{mapping.row1[0],mapping.row1[1],mapping.row1[2]}},{"sampler",mapping.sampler}});}return value;
}

struct Light {
    Vec3 position;
    Vec3 direction{0.0f, 0.0f, 1.0f};
    float color[3]{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    // 0 = point, 1 = spot. Spot uses direction + cosOuter/cosInner.
    float cosOuter = -1.0f;
    float cosInner = -1.0f;
    unsigned type = 0;
    bool castSurfaceShadow = true;
    bool castVolumetricShadow = true;
    float volumetricIntensity = 1.0f;
};

// Scene-owned night/atmosphere controls. Defaults reproduce the courtyard look so the
// legacy path is unaffected when a scene omits this block.
struct FogVolume {
    Vec3 center{},extent{4,4,4};
    float density=0.02f;
};
struct Lighting {
    std::shared_ptr<const assets::EnvironmentMap> panorama;
    std::string environmentFile,environmentHash;
    float environmentIntensity=1,environmentRotation=0;
    float ambient[3]{0.34f, 0.36f, 0.40f};
    float skyColor[3]{0.055f, 0.075f, 0.11f};
    float keyDirection[3]{-0.45f, 0.85f, -0.55f};
    float keyColor[3]{1.0f, 1.0f, 1.0f};
    float keyIntensity = 1.30f;
    float fogColor[3]{0.52f, 0.62f, 0.74f};
    float fogDensity = 0.0f;
    float fogHeightFalloff = 0.08f;
    float fogBaseHeight = 0.0f;
    bool toneMap = false;
    float exposure = 1.0f;
    // Night cloud layer controls for the screen-space volumetric sky.
    float cloudCoverage = 0.35f;
    float cloudHeight = 92.0f;
    float cloudThickness = 36.0f;
    float cloudSpeed = 0.02f;
    unsigned quality = 0;
    bool localShadows = true, ibl = true, ddgi = true, cloudShadows = true, temporal = true;
    unsigned maxShadowLights = 4;
    std::vector<FogVolume> fogVolumes;
};

struct CharacterReference {
    gameplay::Json authoring=gameplay::Json::object();
    std::string file,sha256,binarySha256,clip,rootMotion="asset";
    std::string anchor="center";
    gameplay::Json controller=gameplay::Json::object();
    gameplay::Json rig=gameplay::Json::object();
    Vec3 scale{1,1,1},offset{};
    float speed=1,timeOffset=0,yaw=0;
    std::vector<int> materials;
};
struct Entity {
    gameplay::Json components = gameplay::Json::object();
    std::string id;
    std::string name;
    std::string kind;
    Vec3 position;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    float color[3]{0.65f, 0.65f, 0.65f};
    Material material;
    bool hasMaterial = false;
    // Index into Scene::materials, or -1 for a legacy inline material.
    int materialIndex = -1;
    // Imported geometry uses the shared immutable vertex/index upload table.
    std::string meshFile, meshHash;
    int importedMesh = -1;
    // Optional baked terrain collision. Heights are world metres, row-major.
    bool hasHeightfield = false;
    bool heightfieldEnabled = true;
    float heightfieldOrigin[2]{0.0f, 0.0f};
    float heightfieldSize[2]{1.0f, 1.0f};
    unsigned heightfieldWidth = 0, heightfieldHeight = 0;
    std::vector<float> heightfieldHeights;
    float heightfieldMaxError = 0.0f;
    CharacterReference character;
    int characterAsset = -1;
    bool hasLight = false;
    float lightIntensity = 1.0f;
    float lightRange = 10.0f;
    float lightColor[3]{1.0f, 1.0f, 1.0f};
    bool castSurfaceShadow = true, castVolumetricShadow = true;
    float volumetricIntensity = 1.0f;
    bool collider = false;
    // Optional rigid-body authoring. A body is dynamic only when the scene says so;
    // every other collider participates as an immovable collision shape.
    bool hasPhysics = false;
    bool dynamicBody = false;
    float physicsMass = 14.0f;
    float friction = 0.62f;
    float restitution = 0.06f;
    // Optional destructible authoring. Strength is the impulse scale at which damage
    // becomes meaningful; the actual fracture threshold is a fraction of it.
    bool destructible = false;
    float strength = 45.0f;
    float debrisLifetime = 18.0f;
    int fragments = 10;
    // Bonded destruction keeps the entity as a cell+bond graph: an impact opens a
    // local hole instead of replacing the whole object. `entity` keeps the 0.11.0
    // behaviour (whole-object fracture into box shards).
    bool bonded = false;
    int bondedCells = 24;
    // Baked Voronoi shard set for the bonded graph. Empty keeps the uniform box grid,
    // which is what every scene written before the bake existed gets.
    std::string voronoi;
    bool structural = false;
    unsigned fractureSeed = 0;
    // Vehicle entities drive along Z and loop; 0 means a static prop-like entity.
    float vehicleSpeed = 0.0f;
    int vehicleDirection = 1;
};

// Authored joint. Bodies are referenced by entity id rather than body index because the
// rigid world is rebuilt from the scene on reset, and a body index would not survive it.
struct SceneJoint {
    std::string type = "point";  // "point" | "hinge" | "rope"
    std::string bodyA;           // entity id, required
    std::string bodyB;           // entity id; empty anchors A to the world
    Vec3 anchorA{0.0f, 0.0f, 0.0f};  // local to A, or the world point when bodyB is empty
    Vec3 anchorB{0.0f, 0.0f, 0.0f};  // local to B
    Vec3 axis{0.0f, 1.0f, 0.0f};     // hinge axis, local to A
    float hingeSpan = 0.25f;
    float restLength = 0.0f;
    float compliance = 0.0f;         // spring softness, m/N; 0 is rigid
    float lowerLimit = -3.14159265f;
    float upperLimit = 3.14159265f;
    float breakForce = 0.0f;         // N; 0 is unbreakable
    bool collideConnected = false;
    float motorSpeed = 0.0f;         // rad/s
    float motorMaxTorque = 0.0f;     // N*m; 0 disables the motor
    float friction = 0.0f;           // 0..1 damping per second about the hinge axis
};

struct VegetationRegionReference {
    std::string file;
    std::string cooked = "build/vegetation/cooked";
    std::string textures = "build/vegetation/textures";
};

struct Scene {
    std::string assetRoot = "assets";
    std::vector<VegetationRegionReference> vegetationRegions;
    std::vector<Entity> entities;
    std::vector<Light> lights;
    std::vector<MaterialDefinition> materials;
    // Baked Voronoi shard sets referenced by Entity::voronoi. Kept beside the entities
    // rather than inside them so a package shared by two walls is uploaded once.
    std::vector<VoronoiPackage> voronoiPackages;
    std::vector<ConvexMesh> importedMeshes;
    std::vector<std::shared_ptr<const animation::Asset>> characterAssets;
    std::vector<SceneJoint> joints;
    Lighting lighting;
    float walkSpeed = 3.0f;
    float runSpeed = 6.0f;
    float jumpSpeed = 7.0f;
    float gravity = 20.0f;
};

namespace gameplay { std::string sceneIdentity(const Scene& scene); }

struct Input {
    float forward = 0.0f;
    float right = 0.0f;
    bool run = false;
    bool jump = false;
    bool interact = false;
    // Physics demo actions: throw a heavy pellet, or detonate a radial blast ahead.
    bool fire = false;
    bool blast = false;
    // Mouse/orbit deltas are radians per update, not angular velocities.
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
};

struct Pose {
    // Entity UUID for authored and runtime entities; empty for synthetic poses (shards,
    // player-held items). The editor uses it to apply a draft overlay without
    // rebuilding the scene.
    std::string id;
    Vec3 position;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    float color[3]{0.65f, 0.65f, 0.65f};
    float roughness = 0.72f;
    float metalness = 0.0f;
    unsigned rayVisibilityMask = 1;
    unsigned texture = 0;
    // Cooked material index, or -1 for the legacy procedural path.
    int material = -1;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    // Index into Game::convexMeshes(), or -1 to draw the unit cube scaled by `scale`.
    // A convex pose ignores `scale`: the mesh is already in body-local space.
    int mesh = -1;
};
struct CharacterPose {
    std::string entityId,clip;
    const animation::Asset* asset=nullptr;
    Vec3 position,scale{1,1,1};
    float yaw=0,pitch=0,roll=0;
    float color[3]{1,1,1};
    std::vector<int> materials;
    animation::PoseBuffer pose;
    std::vector<animation::Matrix> palette;
    std::array<float,animation::MaxMorphTargets> morphWeights{};
    const std::vector<animation::VertexDelta>* deformation=nullptr;
};

class Game {
public:
    void initialize(Scene scene);
    void update(Input input, float dt);
    void reset();
    gameplay::Runtime& gameplayRuntime() { return gameplay_; }
    const gameplay::Runtime& gameplayRuntime() const { return gameplay_; }
    gameplay::Json snapshot() const;
    void restore(const gameplay::Json& snapshot);
    void save(const std::string& path) const;
    void load(const std::string& path);
    std::vector<Pose> renderPoses() const;
    std::vector<CharacterPose> renderCharacters() const;
    // Headlights/taillights of moving vehicles. Appended to the scene's static lights by
    // the host each frame; not part of the immutable scene document.
    std::vector<Light> dynamicLights() const;
    void setCharacter(std::shared_ptr<const animation::Asset> asset, bool features = true);
    const animation::Controller& animationController() const { return animation_; }
    std::vector<animation::Event> takeAnimationEvents();

    // Physics interaction surface. The host calls these for F/G and cinematic demos.
    void fireProjectile();
    void fireProjectileAt(Vec3 target, float speed = 34.0f);
    void detonate(Vec3 center, float radius = 6.0f, float deltaSpeed = 9.0f);
    physics::Stats physicsStats() const;

    // Water coupling. The host keeps the authoritative body alive through a shared
    // handle; the Game advances it once per fixed tick, applies buoyancy/drag to
    // dynamic bodies and switches the character into a wading state. Attaching a body
    // resets it to its initial state.
    void setWater(std::shared_ptr<water::Body> body,
                  const water::CouplingSettings& coupling = {},
                  const water::WadingSettings& wading = {},
                  const water::FootprintSettings& footprint = {});
    void clearWater();
    bool waterEnabled() const { return water_ != nullptr; }
    const water::Body* waterBody() const { return water_.get(); }
    water::Query queryWater(float x, float z) const;
    water::Diagnostics waterDiagnostics() const;
    const std::vector<water::InteractionEvent>& waterEvents() const;
    unsigned waterDisturbances() const { return waterDisturbances_; }
    bool wading() const { return wading_.wading; }
    float waterImmersionMetres() const { return wading_.immersionMetres; }
    float waterImmersionFraction() const { return wading_.immersionFraction; }
    float waterDepthMetres() const { return wading_.depthMetres; }
    float waterMaxImmersionMetres() const { return waterMaxImmersion_; }
    bool entityColliderBounds(const std::string& id, physics::Aabb& bounds) const;
    unsigned fractureCount() const { return fractureCount_; }
    unsigned fragmentsSpawned() const { return fragmentsSpawned_; }
    unsigned structuralCollapses() const { return structuralCollapses_; }
    unsigned projectileCount() const { return projectileCount_; }
    unsigned explosionCount() const { return explosionCount_; }
    unsigned impactCount() const { return impactCount_; }
    unsigned destructibleCount() const { return destructibleCount_; }
    unsigned bondedHoles() const { return bondedHoles_; }
    unsigned bondedBrokenBonds() const { return bondedBrokenBonds_; }
    // Convex shard hulls referenced by Pose::mesh. Built once at initialize() from the
    // baked Voronoi packages; empty when no scene declares one.
    const std::vector<ConvexMesh>& convexMeshes() const { return convexMeshes_; }
    // Shards that came from a baked package rather than the uniform box grid.
    unsigned bakedShardCount() const;
    unsigned intactDestructibleCount() const;
    float peakImpactImpulse() const { return peakImpactImpulse_; }
    float totalImpactEnergy() const { return totalImpactEnergy_; }
    const std::vector<float>& entityDamage() const { return damage_; }

    // Deterministic digest over every value the simulation reads next tick. Rendering
    // state, camera smoothing and wall-clock time are deliberately excluded so the
    // digest cannot change because a frame was drawn differently.
    void digest(determinism::Hasher& hasher) const;
    unsigned long long ticks() const { return ticks_; }
    // Read-only fixed-tick presentation observation. Called after each completed
    // step, including every substep in a render update; excluded from the digest.
    void observeFixedTicks(std::function<void(std::uint64_t,Vec3)> observer){tickObserver_=std::move(observer);}

    Vec3 playerPosition() const { return player_; }
    float playerRadius() const;
    float playerHeight() const;
    float playerStepHeight() const;
    float playerModelScale() const;
    Vec3 orbitCameraEye() const;
    Vec3 orbitCameraTarget() const;
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    bool hasKey() const { return hasKey_; }
    bool doorOpen() const { return doorOpen_; }
    bool complete() const { return complete_; }
    bool grounded() const { return grounded_; }
    std::string status() const;
    bool firstPerson() const;
    float cameraFov() const;
    Vec3 cameraEye() const;
    Vec3 cameraTarget() const;
    std::string interactionTarget() const;
    std::string interactionPrompt() const;

private:
    friend struct gameplay::NativeSnapshot;
    bool solid(const Entity& entity) const;
    struct CharacterBox {
        Vec3 center;
        Vec3 half;
        int bodyIndex = -1;
        bool dynamic = false;
    };
    void collectCharacterBoxes(std::vector<CharacterBox>& boxes) const;
    void tryStepUp(float dx, float dz);
    void resolveCharacterPenetration();
    float terrainHeightAt(float x, float z) const;
    void pushDynamicBody(int bodyIndex, Vec3 direction, float speed);
    void step(const Input& input, float dt);
    std::function<void(std::uint64_t,Vec3)> tickObserver_;
    void updateVehicles(float dt);
    int materialIndex(const std::string& name) const;
    void addVehiclePoses(const Entity& entity, std::vector<Pose>& poses) const;
    void moveHorizontal(float dx, float dz);
    void moveVertical(float dy, float groundSnap = 0.0f);
    void interact();
    void updateWadingState();
    void applyWaterCoupling(float dtSeconds);
    Vec3 location(const std::string& kind, Vec3 fallback) const;

    void initializeGameplay();
    void syncGameplay();
    void publishGameplayPhysics();
    std::string playerEntityId() const;
    float playerParameter(const char* key,float fallback) const;
    float movementEdge() const;
    Vec3 characterBodyOffset(int entity,physics::Quat orientation={}) const;
    float worldEdge() const;
    void initializePhysics();
    void createJoints();
    void processImpacts();
    // Bonded destruction: deposits energy into the entity's bond graph and, once a
    // bond breaks, replaces the single body with per-cell bodies.
    void applyBondedImpact(int entity, Vec3 point, float energy);
    void activateBonded(int entity, Vec3 point, float energy);
    const VoronoiPackage* findVoronoiPackage(const std::string& path) const;
    bool adoptVoronoi(std::size_t entity, const VoronoiPackage& package, float density);
    void damageEntity(int entity, float impulse, Vec3 point);
    void fractureEntity(int entity, bool structuralCollapse, Vec3 impulsePoint,
                        float impulseMagnitude);
    void collapseUnsupported();
    bool supportedByScene(int entity, const std::vector<bool>& supported) const;
    void appendRuntimeBodyPoses(std::vector<Pose>& poses) const;
    Pose poseForEntityBody(int entity, const physics::Body& body) const;
    int bodyIndexForEntity(int entity) const;

    std::string contentId_;
    int playerSceneIndex_=-1;
    Scene initialScene_;
    Scene scene_;
    gameplay::Runtime gameplay_;
    unsigned appliedAnimationRequest_=0;
    std::map<std::string,Vec3> publishedVelocity_;
    std::map<std::string,int> runtimeBodies_;
    gameplay::Json publishedTransforms_=gameplay::Json::object();
    std::vector<Vec3> vehicleSpawns_;
    animation::Controller animation_;
    std::vector<animation::Event> animationEvents_;
    Vec3 spawn_{0.0f, 0.0f, 5.0f};
    Vec3 player_{0.0f, 0.0f, 5.0f};
    float verticalVelocity_ = 0.0f;
    float yaw_ = 0.0f;
    // Body facing. The camera yaw steers; the body follows at a bounded rate so
    // a mouse flick animates a turn instead of spinning the pelvis under planted
    // feet (which crosses the legs through each other).
    float bodyYaw_ = 0.0f;
    // Smoothed model height. A step-up moves the collision body instantly; the
    // model and the foot solve follow over ~0.1 s so the feet stay on the tread.
    float visualY_ = 0.0f;
    // Remaining time during which a step-up counts as supported while the body
    // slides forward onto the tread it was lifted onto.
    float stepGrace_ = 0.0f;
    float pitch_ = 0.35f;
    float animationPhase_ = 0.0f;
    float animationSpeed_ = 0.0f;
    double accumulator_ = 0.0;
    bool grounded_ = true;
    bool hasKey_ = false;
    bool doorOpen_ = false;
    bool complete_ = false;
    bool jumpWasDown_ = false;
    bool interactWasDown_ = false;
    bool fireWasDown_ = false;
    bool blastWasDown_ = false;
    bool jumpPending_ = false;
    bool interactPending_ = false;
    bool firePending_ = false;
    bool blastPending_ = false;
    float feedbackTime_ = 0.0f;
    std::string feedback_;
    unsigned long long ticks_ = 0;

    physics::World physics_;
    std::shared_ptr<water::Body> water_;
    water::CouplingSettings waterCoupling_{};
    water::WadingSettings wadingSettings_{};
    water::FootprintSettings footprintSettings_{};
    water::WadingState wading_{};
    water::FootprintState footprintState_{};
    // Per-foot contact state used when the rig exposes sole pivots. The root footprint
    // remains the documented fallback when it does not.
    std::array<water::FootContactState, 2> footWaterState_{};
    water::FootContactSettings footWaterSettings_{};
    std::vector<float> bodyWaterVolume_;
    unsigned waterDisturbances_ = 0;
    float waterMaxImmersion_ = 0.0f;
    std::vector<fracture::Structure> bonded_;
    std::vector<bool> bondedActive_;
    unsigned bondedHoles_ = 0;
    unsigned bondedBrokenBonds_ = 0;
    std::vector<ConvexMesh> convexMeshes_;
    // Parallel to convexMeshes_: the same hull as plain Vec3 points and triangle indices,
    // which is the layout the narrowphase consumes. Kept separate because ConvexMesh is a
    // 32-byte render vertex and the collision shapes must outlive the bodies that point at
    // them, so neither can borrow from the other.
    std::vector<std::vector<Vec3>> hullPoints_;
    std::vector<std::vector<std::uint32_t>> hullIndices_;
    std::vector<physics::Heightfield> heightfields_;
    std::vector<unsigned> jointIds_;
    std::vector<int> entityBody_;
    std::vector<bool> fractured_;
    std::vector<float> damage_;
    unsigned fractureCount_ = 0;
    unsigned fragmentsSpawned_ = 0;
    unsigned structuralCollapses_ = 0;
    unsigned projectileCount_ = 0;
    unsigned explosionCount_ = 0;
    unsigned impactCount_ = 0;
    unsigned destructibleCount_ = 0;
    float peakImpactImpulse_ = 0.0f;
    float totalImpactEnergy_ = 0.0f;
};

} // namespace engine
