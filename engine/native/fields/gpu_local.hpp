#pragma once
#include "local.hpp"
#include "../effects/gpu_pool.hpp"
#include <functional>

namespace engine::fields {
struct GpuLocalGrid {Vec3 originMetres{};float worldCellMetres=1;};
struct GpuLocalLimits {
    std::uint32_t maxInstances=32,maxProjectionViews=16,maxInputBatches=128,maxParticlesPerSplat=1000000;
    std::uint64_t maxLocalSamples=1048576,maxAllocationBytes=64*1024*1024;
};
struct GpuLocalHandle {std::uint32_t slot=0,generation=0;};
struct GpuLocalInfo {
    std::uint64_t source=0,version=0,lastFence=0,inputVersion=0;Tick sourceTick=0;
    bool accumulating=false,resolved=false;
};
struct GpuLocalFootprint {
    std::uint64_t samples=0,mutableBytes=0,projectionBytes=0,worldPages=0;
    Region coarseRegion;
};
// Saturating counts: 0 accepted, 1 outside, 2 invalid, 3 clamped cells,
// 4 malformed counter, 5 saturation flag; 8..15 clamp loss, 16..23 outside
// loss, 24..31 invalid loss, in per-channel integer field quanta.
constexpr std::uint32_t GpuLocalDiagnosticWords=32;
struct GpuLocalBuffers {ID3D12Resource* values=nullptr;ID3D12Resource* diagnostics=nullptr;std::uint32_t voxels=0,channels=0;};
// Borrowed same-queue scalar state. prepareRead is called synchronously only
// after admission, and must protect the producer through the consumer fence
// and leave values in NON_PIXEL_SHADER_RESOURCE. No CPU values/readback.
struct GpuScalarInput {
    ID3D12Resource* values=nullptr;ID3D12Fence* timeline=nullptr;
    std::uint32_t offset=0;Cell dimensions;Vec3 originMetres{};
    float cellMetres=0,maximum=0;
    Tick sourceTick=0;std::uint64_t version=0,lastFence=0;
    // Optional pure producer validation, evaluated before dereferencing values.
    // This permits rejection after producer shutdown without retaining its
    // allocations outside the normal admitted input/fence ledger.
    std::function<bool()> current;
    std::function<bool(ID3D12GraphicsCommandList*,std::uint64_t,std::string&)> prepareRead;
};
struct GpuProjectionPlane {ChannelId channel=0;ProjectionReduction reduction=ProjectionReduction::VolumeAverage;ID3D12Resource* values=nullptr;ID3D12Resource* metrics=nullptr;Channel definition;};
struct GpuProjectionBindings {
    std::string owner;std::uint64_t source=0,epoch=0,localVersion=0,inputVersion=0;Tick sourceTick=0,publishTick=0;
    Region region;Cell originFine{},dimensionsFine{};GpuLocalGrid grid;std::uint32_t finePerWorld=1;std::vector<GpuProjectionPlane> planes;
    ID3D12Resource* diagnostics=nullptr;
};
// Immutable GPU projection, with actual-fence read lifetime. Values are dense
// int32 coarse cells (x fastest); metrics contain input/reconstructed uint64
// integrals and signed int64 residual, all as pairs of uint32. Visual only.
class GpuLocalProjection {
public:
    ~GpuLocalProjection();GpuLocalProjection(const GpuLocalProjection&)=delete;
    GpuProjectionBindings bindings() const;
    bool markUsed(std::uint64_t fence);std::uint64_t lastFence() const;
    ID3D12Device* device() const;ID3D12Fence* timeline() const;
private:
    GpuLocalProjection();struct Impl;std::unique_ptr<Impl> impl_;friend class GpuLocalFields;
};
struct GpuLocalStats {
    std::uint64_t allocationBytes=0,localSamples=0,dispatches=0,splatBatches=0,projections=0,lastFence=0;
    std::uint32_t instances=0,retiredInstances=0,projectionViews=0,heldInputReferences=0;
};
// GPU-only quantized visual LEF and coarse projections. The catalog is copied;
// no World publication or CPU authority mutation is performed. WF aggregation,
// solver ownership migration and fluid transport are separate services.
class GpuLocalFields {
public:
    GpuLocalFields(ID3D12Device*,ID3D12Fence*,std::vector<Channel> catalog,GpuLocalLimits limits={},GpuLocalGrid grid={});
    ~GpuLocalFields();GpuLocalFields(const GpuLocalFields&)=delete;
    bool create(LocalSpec spec,GpuLocalHandle& out,std::string& error);
    // Pure aligned resource estimate; source identity is assigned at create.
    bool estimate(const LocalSpec&,GpuLocalFootprint&,std::string& error) const;
    bool begin(GpuLocalHandle,ID3D12GraphicsCommandList*,std::uint64_t fence,Tick sourceTick,std::string& error);
    // A contribution is a declared integer field quantum per accepted particle,
    // deposited into its containing fine cell. Capacity*quantum is conservatively
    // admitted before GPU work; all batches per channel must fit uint32 exactly.
    bool splat(GpuLocalHandle,ChannelId,effects::GpuParticlePool&,std::uint32_t quantum,ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    bool resolve(GpuLocalHandle,ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    // Replace one-channel LEF from aligned nonnegative float concentrations.
    // Quantize once at fine-cell resolution, then reuse immutable projection
    // and WF source replacement. Rejects mixed channels and partial grids.
    bool replaceScalar(GpuLocalHandle,const GpuScalarInput&,ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    std::shared_ptr<GpuLocalProjection> project(GpuLocalHandle,Tick publishTick,ID3D12GraphicsCommandList*,std::uint64_t fence,std::string& error);
    bool retire(GpuLocalHandle,std::string& error);
    bool markUsed(GpuLocalHandle,std::uint64_t fence);
    GpuLocalInfo inspect(GpuLocalHandle) const;GpuLocalBuffers buffers(GpuLocalHandle) const;
    void collect();GpuLocalStats stats() const;bool shutdown(std::string& error);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
