#pragma once
#include "water_draw.hpp"
#include "../engine/native/material_ior.hpp"
#include "../engine/native/material_layer_gpu.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

#include "matrix_math.h"
#include "gpu_cloth_draw.hpp"
#include "vegetation_draw.hpp"
#include "hair_draw.hpp"
#include "../engine/native/animation.hpp"
#include "../engine/native/convex_mesh.hpp"
#include "../engine/native/material_sampling.hpp"
#include "../engine/native/assets/environment.hpp"
#include "../engine/native/effects/gpu_runtime.hpp"
#include "../engine/native/fields/gpu_world.hpp"

namespace nativegpu {
// Shared visual field frame, independent of any particular consumer. Its
// callbacks record on the renderer's actual graphics timeline.
struct GpuFieldFrame {
    std::uint64_t epoch=0;
    std::uint64_t effectEpoch=0;
    std::function<engine::fields::GpuAcquisition(engine::fields::LeaseRequest)> acquire;
    std::function<bool(engine::fields::GpuLease)> release;
    std::function<engine::fields::GpuVectorResult(engine::fields::GpuLease,const engine::fields::GpuVectorSpec&,engine::fields::Tick)> publish;
    std::shared_ptr<engine::fields::GpuVectorView> wind;
};
using GpuFieldDriver=std::function<bool(GpuFieldFrame&,std::string&)>;
// Executed while the renderer records a frame on its own graphics timeline.
// The host controls logical instance/tick decisions without owning command lists.
struct GpuEffectFrame {
    engine::effects::GpuRuntime& runtime;
    std::function<bool(std::string&)> step;
    std::uint64_t epoch=0;
    // These callbacks are valid only during this frame. publishField records on
    // the renderer's queue and returns a read view, never a GPU authority value.
    std::function<engine::fields::GpuAcquisition(engine::fields::LeaseRequest)> acquireField;
    std::function<bool(engine::fields::GpuLease)> releaseField;
    std::function<engine::fields::GpuVectorResult(engine::fields::GpuLease,const engine::fields::GpuVectorSpec&,engine::fields::Tick)> publishField;
    std::function<bool(engine::fields::Tick,std::string&)> stepField;
    // Joint visual WF consumer router. Views are marked for this frame's real
    // queue fence; retained snapshots must call useVisualField on later use.
    std::function<engine::fields::GpuAcquisition(engine::fields::LeaseRequest)> acquireVisualField;
    std::function<bool(engine::fields::GpuLease,std::string&)> releaseVisualField;
    std::function<std::shared_ptr<engine::fields::GpuVisualView>(engine::fields::GpuLease,std::string&)> viewVisualField;
    std::function<bool(const std::shared_ptr<engine::fields::GpuVisualView>&)> useVisualField;
};
using GpuEffectDriver=std::function<bool(GpuEffectFrame&,std::string&)>;

struct GpuClothConfig {
    std::shared_ptr<const engine::cloth::Asset> asset;
    engine::Vec3 origin{},wind{};ClothSurface surface;
    engine::cloth::GpuExecution execution=engine::cloth::GpuExecution::Automatic;
    bool simulationWind=false;
};
struct GpuClothFrame {
    std::vector<engine::cloth::GpuSolver*> solvers;
    std::function<bool(unsigned,float,std::string&)> step;
    std::function<bool(unsigned,engine::Vec3,std::string&)> teleport;
    std::function<bool(unsigned,std::string&)> reset;
    std::uint64_t epoch=0;
    std::function<bool(float,std::string&)> stepAll;
};
using GpuClothDriver=std::function<bool(GpuClothFrame&,std::string&)>;

// Matrices are column-major, with column vectors: clip = viewProjection * model * position.
struct Vertex { float x, y, z, nx, ny, nz, u, v; float textureUvs[4][2]{}; float color[4]{1,1,1,1}; float tangent[4]{}; };
// Cooked texture payload. Mips are tightly packed in mip order. `format` is one of the
// NGTX format codes (0 RGBA8, 1 BC1, 2 BC3, 3 BC5); each texture becomes its own
// Texture2D so dimensions, mip counts and formats can differ.
struct TextureUpload {
    unsigned width = 0;
    unsigned height = 0;
    unsigned mips = 0;
    unsigned format = 0;
    bool srgb = false;
    std::vector<unsigned char> pixels;
};
struct EffectSprite {
    float position[3]{};
    float color[4]{1,1,1,1};
    float size[2]{1,1};
    float rotation = 0;
    unsigned texture = 0;
    unsigned blend = 0; // 0 additive, 1 premultiplied alpha, 2 cutout
    unsigned long long stableId = 0;
};
// Texture slots in the shader-visible heap plus the world-space size of one repeat.
struct MaterialUpload {
    std::string name;
    unsigned albedo = 0;
    unsigned normal = 0;
    unsigned orm = 0;
    float uvScale = 1.0f;
    float roughness = 1.0f;
    float metalness = 1.0f;
    // Surface-history controls, carried in the GpuMaterial record for the bindless path.
    float wetness = 0.0f;
    float wear = 0.0f;
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.1f;
    float ior = 1.5f;
    bool alphaMasked = false;
    bool alphaBlended = false;
    float opacity = 1.0f;
    float alphaCutoff = 0.5f;
    bool hasEmissive = false;
    unsigned emissive = 0;
    float emissiveFactor[3]{1.0f,1.0f,1.0f};
    float emissiveStrength = 1.0f;
    bool hasOcclusion = false;
    unsigned occlusion = 0;
    unsigned coat=0,coatRoughness=0,coatNormal=0;
    bool hasCoat=false,hasCoatRoughness=false,hasCoatNormal=false;
    float occlusionStrength = 1.0f;
    bool unlit = false;
    engine::TextureMapping textureMappings[8];
    bool hasTextureMappings = false;
    // Relative texture indices; converted to shader heap indices on upload.
    // Bit 0 requires the extra texture UV streams (sets 1-4), bit 1 the extra
    // layer UV streams (sets 5-7). Draws whose mesh lacks them are rejected.
    unsigned uvStreamMask = 0;
    unsigned authoredLayers = 0;
    std::vector<engine::material::layered::GpuLayer> layers;
    engine::material::layered::GpuSurfaceRelief surfaceRelief;
    bool hasSurfaceRelief = false;
};
struct DrawItem {
    Mat4 model;
    float color[4];
    // Legacy bounded texture slot. Kept for untextured scenes and fault injection.
    unsigned texture = 0;
    // Bounded material table generation. 0 skips the generation check (compatibility).
    unsigned textureGeneration = 0;
    // Index into the configured material table. -1 means "no cooked material".
    int material = -1;
    float roughness = 0.72f;
    float metalness = 0.0f;
    // 1 static occluder, 2 dynamic occluder, 4 emitter shell (visible to GI/key,
    // excluded from local-light visibility so a lamp does not shadow its own source).
    unsigned rayVisibilityMask = 1;
    // Non-owning views, valid through submit. Deforming meshes use the VS path.
    const engine::animation::Asset* skin = nullptr;
    const std::vector<engine::animation::Matrix>* palette = nullptr;
    const std::array<float, engine::animation::MaxMorphTargets>* morphWeights = nullptr;
    const std::vector<engine::animation::VertexDelta>* deformation = nullptr;
    // Index into the convex mesh table configured with configureConvexMeshes(). -1 draws
    // the unit cube. The mesh is in body-local space, so `model` must not apply a scale.
    int mesh = -1;
    // Index range inside a shared skin/mesh buffer; zero count draws the whole mesh.
    unsigned firstIndex = 0, indexCount = 0;
    // Runtime surface wetness override in [0,1]; negative keeps the material's authored
    // value. Only the bindless opaque path consumes it (it rides in the otherwise unused
    // ormIndex root constant, which the material record has replaced).
    float wetness = -1.0f;
};
// Screen-space rectangles are composited after lighting and included in captures.
// A layer batches all glyph runs of one colour into one command.
struct OverlayLayer {
    float color[4]{1,1,1,1};
    std::vector<RECT> rectangles;
};
struct LightDesc {
    float position[3];
    float color[3];
    float intensity = 1.0f;
    float range = 10.0f;
    // 0 = point, 1 = spot. Spot uses direction + cosOuter/cosInner; point ignores them.
    float direction[3] = {0.0f, 0.0f, 1.0f};
    float cosOuter = -1.0f;
    float cosInner = -1.0f;
    unsigned type = 0;
    unsigned stableId = 0;
    bool castSurfaceShadow = true;
    bool castVolumetricShadow = true;
    float volumetricIntensity = 1.0f;
    bool isStatic = false;
};
// Night/atmosphere controls live outside the per-frame draw list so the host can change
// them when a scene is loaded or hot-reloaded. Defaults reproduce the old courtyard look.
struct FogVolume {
    float center[3]{};
    float extent[3] = {4,4,4}; // full world-space dimensions
    float density = 0.02f;
};
struct Environment {
    std::shared_ptr<const engine::assets::EnvironmentMap> panorama;
    float environmentIntensity=1,environmentRotation=0;
    float ambient[3] = {0.34f, 0.36f, 0.40f};
    float skyColor[3] = {0.055f, 0.075f, 0.11f};
    float keyDirection[3] = {-0.45f, 0.85f, -0.55f}; // direction *to* the key light
    float keyColor[3] = {1.0f, 1.0f, 1.0f};
    float keyIntensity = 1.30f;
    float fogColor[3] = {0.52f, 0.62f, 0.74f};
    // density = 0 keeps the legacy/courtyard path's pixel output unchanged.
    float fogDensity = 0.0f;
    float fogHeightFalloff = 0.08f;
    float fogBaseHeight = 0.0f;
    float cloudCoverage = 0.35f;
    float cloudHeight = 92.0f;
    float cloudThickness = 36.0f;
    float cloudSpeed = 0.02f;
    bool toneMap = false;
    float exposure = 1.0f;
    // Enables the screen-space volumetric fog/cloud pass in composite. Off for legacy.
    bool volumetric = false;
    // 0 retains the compatibility path; 1/2 select balanced/high world-space lighting.
    unsigned quality = 0;
    bool localShadows = true;
    bool ibl = true;
    bool ddgi = true;
    bool cloudShadows = true;
    bool temporal = true;
    unsigned maxShadowLights = 4;
    std::vector<FogVolume> fogVolumes;
};
// Axis-aligned obstacle for the particle field. `center` and `extent` are full extents, so
// the box spans center +/- extent/2 on every axis. The scene has no per-entity rotation, so
// an AABB is exact rather than an approximation.
struct FieldCollider {
    float center[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float extent[4] = {1.0f, 1.0f, 1.0f, 0.0f};
};
struct FrameStats {
    // cpuMs is measured around the whole submit including Present; cpuSubmitMs stops
    // before Present so it is not dominated by the vsync wait.
    double cpuMs = 0.0;
    double cpuSubmitMs = 0.0;
    double gpuMs = 0.0;
    bool gpuValid = false;
    // Time spent inside the DXR shadow dispatch for the most recent completed frame.
    double gpuRtMs = 0.0;
    bool gpuRtValid = false;
};

// One resident vegetation page/species pair. The host owns World streaming and only
// hands decoded pages to the renderer; this keeps file I/O and residency off the GPU
// timeline while allowing the scene and vegetation to share one device, depth buffer
// and composite pass.
struct VegetationLayer {
    VegetationMeshData mesh;
    std::vector<engine::vegetation::PackedInstance> instances;
    engine::vegetation::PageRecord page;
    VegetationTextureSet textures;
    VegetationMaterial material;
    float bounds[3] = {1.0f, 1.0f, 0.0f};
};

// A cooked hairstyle resident for the scene pass. Direction, colour and intensity come
// from the scene Environment every frame; the material and the shadow controls are
// authored per hairstyle. `debugView` follows HairDebugView.
struct HairSetup {
    engine::hair::GpuPack pack;
    HairMaterial material;
    float shadowStrength = 0.85f;
    float shadowSigmaPerMetre = 2400.0f;
    float shadowDensityScale = 1.0f;
    float lightIntensityScale = 1.0f;
    int debugView = 0;
    HairSimulation simulation;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Enables the D3D12 validation layer for the next initialize(). Must be called first.
    void setValidationEnabled(bool enabled);

    // Cooked material path. Call before initialize() (and again after shutdown() when the
    // host rebuilds the renderer). Empty textures/materials keep the legacy procedural
    // path used by the courtyard scene. Every texture must share width/height/mip count.
    bool configureMaterials(std::vector<TextureUpload> textures,
                            std::vector<MaterialUpload> materials, std::string& error);
    void clearMaterials();
    bool materialsConfigured() const;
    unsigned materialCount() const;

    // Convex mesh table used by DrawItem::mesh. Meshes are concatenated into one vertex
    // and one index buffer, so the set is uploaded as a unit. Safe to call before
    // initialize() (deferred to init) or afterwards (uploads immediately and waits for
    // the GPU, so it must not be called mid-frame).
    bool configureConvexMeshes(std::vector<engine::ConvexMesh> meshes, std::string& error);
    unsigned convexMeshCount() const;
    // Bytes of vertex+index data resident for the convex pool. Reported for the budget gate.
    unsigned convexMeshBytes() const;

    // Scene-owned lighting/atmosphere. Call before or after initialize(); the renderer
    // reads it every frame. Kept separate from DrawItem so a hot reload can update it.
    void setEnvironment(const Environment& environment);
    void setOverlay(std::vector<OverlayLayer> layers);
    bool configureEffectTextures(std::vector<TextureUpload> textures, std::string& error);
    bool setEffectSprites(std::vector<EffectSprite> sprites, std::string& error);
    bool configureGpuEffects(std::vector<std::shared_ptr<const engine::effects::Asset>> assets,
                             std::map<std::string,unsigned> textureSlots,GpuEffectDriver driver,std::string& error);
    engine::effects::GpuRuntimeStats gpuEffectStats() const;
    bool configureGpuCloth(std::vector<GpuClothConfig> cloths,GpuClothDriver driver,std::string& error);
    bool configureSimulationFields(std::shared_ptr<engine::fields::World> world,engine::fields::GpuLimits storageLimits,
                                  engine::fields::GpuVectorLimits vectorLimits,GpuFieldDriver driver,std::string& error);
    bool configureGpuEffectFields(std::shared_ptr<engine::fields::World> world,engine::fields::GpuLimits storageLimits,
                                  engine::fields::GpuVectorLimits vectorLimits,std::string& error);
    Environment environment() const;

    // Replaces the resident vegetation draw set atomically. Empty layers disable the
    // pass. Called by the streaming host only when page residency/species LOD changes.
    bool configureVegetation(std::vector<VegetationLayer> layers, std::string& error);
    unsigned vegetationLayerCount() const;

    // Replaces the resident hairstyle. An empty pack disables the pass. Configuration
    // happens after initialize() because the upload goes through the renderer's own
    // command list, exactly like vegetation.
    bool configureHair(HairSetup setup, std::string& error);
    void clearHair();
    bool hairConfigured() const;
    HairDrawStats hairStats() const;
    // Model-to-world transform of the hairstyle, normally the player's head skin matrix.
    // Identity keeps the strands at their authored rest position.
    void setHairHeadTransform(const Mat4& transform);

    bool initialize(HWND window, unsigned width, unsigned height, std::string& error);
    bool resize(unsigned width, unsigned height, std::string& error);
    bool setWaterSurface(WaterSurfaceFrame frame,std::string& error);
    // Second, independent water body (waterfall upstream). An empty frame removes it.
    bool setUpperWaterSurface(WaterSurfaceFrame frame,std::string& error);
    // Falling sheets between configured water bodies. Empty clears the list.
    void setWaterfalls(std::vector<WaterfallFrame> frames);
    // `cameraPosition` is the world-space eye used by the DXR shadow pass to orient
    // reconstructed normals; it has no effect on the raster path.
    bool render(const Mat4& viewProjection, const float cameraPosition[3],
                const std::vector<DrawItem>& items, const std::vector<LightDesc>& lights,
                std::string& error);
    void shutdown();
    // Complete queued validation/readback before reporting a finite native run.
    bool synchronize(std::string& error);

    // Runtime toggle for the DXR static shadow pass. When disabled (or when the pass
    // cannot be built) rendering falls back to the raster path with a full visibility
    // mask, and every report field records that fallback.
    void setRtShadows(bool enabled);
    bool rtShadowsEnabled() const;
    bool rtShadowsImplemented() const;

    // Mesh shader sample path. Falls back to the vertex path when unavailable/disabled.
    void setMeshShaders(bool enabled);
    bool meshShadersEnabled() const;
    bool meshShadersImplemented() const;
    bool meshPathActive() const;

    // View-frustum culling for the raster path. Off restores the pre-culling submission
    // pattern, which is what the before/after evidence compares against.
    // The DXR TLAS and the shadow pass still consume every static item: an occluder
    // outside the camera frustum can still cast a shadow into it.
    void setFrustumCulling(bool enabled);
    bool frustumCullingEnabled() const;
    unsigned culledDrawsLastFrame() const;
    unsigned submittedDrawsLastFrame() const;

    // 0 final, 1 shadow mask, 2 key light, 3 ambient surface. Diagnostic evidence only.
    void setCompositeDebugView(unsigned view);
    // Preview-only layered material debug view: mode 0 off, 1 coverage, 2 height, 3 layer albedo.
    void setLayerDebugView(unsigned mode, unsigned layer);

    // Screenshots are captured in two steps so the copy happens inside the frame
    // submission: request before render(), capture after render().
    bool requestScreenshot(const std::string& path, std::string& error);
    bool captureScreenshot(const std::string& path, std::string& error);

    // Compute probe (B1/B3): a dedicated compute queue plus an SM 6.6 heap-indexing check
    // against the descriptor-table path. Optional; reports through probes.compute.
    void setComputeEnabled(bool enabled);
    void setComputeCorrupt(bool corrupt);
    bool computeProbeVerified() const;

    // Particle field (docs/FIELD-PLAN.md F1-F3). Enable before initialize(); a failed build
    // leaves the renderer usable and reports through probes.particles.error.
    void setParticlesEnabled(bool enabled);
    bool particlesEnabled() const;
    bool particlesImplemented() const;
    void setParticleEmitter(const float position[3]);
    // Campfire preset: tighter, hotter and more opaque than the default plume.
    void setFirePreset(bool enabled);
    // Field bounds in world space. Resolution follows the effect: a small local box gives
    // centimetre voxels at the same voxel count as a large coarse one.
    void setFieldBounds(const float minimum[3], float extent);
    // Frame at which a one-shot explosion burst fires; 0 disables it.
    void setExplosionFrame(unsigned frame);
    // Last computed explosion burst in [0,1]; the host polls it so the blast can leave a
    // visible mark on the scene.
    float particleBurst() const;
    // Obstacles the particle field collides against. Replaces the previous set; the
    // renderer clamps to its fixed capacity and reports how many it kept.
    void setFieldColliders(const std::vector<FieldCollider>& colliders);
    // Half-res march + temporal accumulation + depth-aware upsample
    // (docs/FIELD-REALISM-GOAL.md). On by default; disabling falls back to the direct
    // full-resolution composite with the same shading model.
    void setParticleTemporal(bool enabled);
    bool particleTemporalEnabled() const;

    // Reliability surface used by the G1 evidence suite.
    void injectDeviceRemovalAtFrame(unsigned frame);
    unsigned injectedRemovalFrame() const;
    static long liveObjectCount();
    static unsigned long long objectsCreatedCount();
    unsigned long long videoMemoryBytes();
    // Compiles a deliberately broken shader to prove the invalid-pipeline diagnostic and
    // that the failure is reported instead of crashing.
    bool selfTestInvalidPipeline(std::string& code, std::string& text);

    std::string capabilitiesJson() const;
    std::string probesJson() const;
    FrameStats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace nativegpu
