#pragma once
#include "asset.hpp"
#include "../fields/gpu_vector.hpp"
#include <optional>

namespace engine::cloth {
struct GpuVertex {std::array<float,4> position{},previous{},velocity{},normal{},tangent{};};
static_assert(sizeof(GpuVertex)==80,"GPU cloth vertex ABI changed");
struct GpuLimits {std::uint32_t maxVertices=16384,maxDispatchesPerStep=8192;std::uint64_t maxAllocationBytes=64*1024*1024;};
enum class GpuExecution {Automatic,Dispatch,Grouped};
struct GpuParameters {Vec3 origin{},wind{};std::shared_ptr<fields::GpuVectorView> windField;GpuExecution execution=GpuExecution::Automatic;bool reserveWindField=false;};
struct GpuBuffers {
    ID3D12Resource* vertices=nullptr;ID3D12Resource* wind=nullptr;ID3D12Resource* diagnostics=nullptr;ID3D12Resource* membranes=nullptr;
    std::uint32_t vertexCount=0,triangleCount=0;
    D3D12_RESOURCE_STATES vertexState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
struct GpuStats {std::uint64_t tick=0,lastFence=0,allocationBytes=0,dispatches=0,batchedTicks=0;std::uint32_t substeps=0,membraneColors=0,hingeColors=0,groupSharedBytes=0;bool initialized=false,readReady=false,paused=false,grouped=false;};
// Visual triangular XPBD. Immutable offline topology; no queue waits/readbacks.
// All commands and the field cache must use this actual fence/queue timeline.
// The caller retains read/draw buffers through the last registered use.
class GpuSolver {
public:
    GpuSolver(ID3D12Device* device,ID3D12Fence* timeline,std::shared_ptr<const Asset> asset,GpuLimits limits={},GpuParameters parameters={});
    ~GpuSolver();
    GpuSolver(const GpuSolver&)=delete;
    bool initialize(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    bool step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,float dt,std::string& error,std::optional<fields::Tick> fieldTick={});
    bool reset(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    bool teleport(ID3D12GraphicsCommandList* commands,std::uint64_t fence,Vec3 origin,std::string& error);
    // Updates the uniform input/fallback, clearing a previously installed spatial
    // sample before the next query. A configured field still queries each tick.
    bool setWind(Vec3 wind);
    bool bindWindField(std::shared_ptr<fields::GpuVectorView> wind,std::string& error);
    bool clearWindField(std::string& error);
    bool setSelfCollision(bool enabled,std::string& error);
    void setPaused(bool paused);
    bool prepareRead(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error);
    GpuBuffers buffers() const;
    GpuStats stats() const;
    ID3D12Device* device() const;
    ID3D12Fence* timeline() const;
    std::array<std::uint32_t,2> gridDimensions() const;
    bool shutdown(std::string& error);
private:
    friend class GpuBatch;
    struct BatchInfo {std::array<ID3D12Resource*,12> resources{};std::array<std::uint32_t,32> constants{};std::uint32_t substeps=0;bool paused=false,grouped=false,field=false;};
    bool batchInfo(ID3D12GraphicsCommandList*,std::uint64_t,float,std::optional<fields::Tick>,BatchInfo&,std::string&) const;
    void batchPrepare(ID3D12GraphicsCommandList*,const BatchInfo&);
    void batchFinish(std::uint64_t,const BatchInfo&,std::optional<fields::Tick>);
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
