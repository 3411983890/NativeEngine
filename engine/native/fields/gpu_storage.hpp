#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include "world.hpp"

namespace engine::fields {
struct GpuLease { std::uint32_t slot=0,generation=0; };
struct GpuView { std::uint32_t slot=0,generation=0; };
struct GpuAcquisition { bool accepted=false; GpuLease lease; std::string error; };
struct GpuViewResult { bool accepted=false; GpuView view; std::string error; };
struct GpuLimits {
    std::size_t maxPages=64,maxSnapshots=3,maxLeases=256,maxViews=256;
    std::uint64_t maxAllocationBytes=32*1024*1024;
};
struct GpuBindings {
    ID3D12DescriptorHeap* descriptors=nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS pageTable=0;
    std::uint32_t pageCount=0,channelCount=0;
    std::uint32_t pageTableDescriptor=0;
    Region region;
    std::array<std::uint32_t,8> channelMask{};
    Tick snapshotTick=0,maxAgeTicks=0;
    std::uint64_t snapshotVersion=0,serial=0;
    Consistency consistency=Consistency::Eventual;
    std::uint32_t haloCells=0;
    std::array<std::uint32_t,28> constants(std::uint32_t queryCount,Tick requestedTick) const;
};
struct GpuFieldStats {
    std::size_t leases=0,views=0,snapshots=0,currentPages=0,residentPages=0,descriptorSlots=0;
    std::uint64_t defaultAllocationBytes=0,uploadAllocationBytes=0,completedFence=0;
    std::uint64_t publishedSnapshots=0,rejectedSnapshots=0;
};
// Real D3D12 sparse read cache of committed CPU data (including read-only
// authority mirrors). It never publishes GPU values back into World. Pages are
// immutable per snapshot and shared by overlapping consumers. A view is a read
// lease even after its spatial lease is released. The caller supplies one GPU
// fence timeline and must submit/signal every publish fence and mark later reads.
// No queue waits or readbacks are performed here. Read halos and fixed-point
// trilinear sampling use the same snapshot as the core region. GPU solver
// writes and the renderer adapter are separate work packages.
class GpuFieldStorage {
public:
    GpuFieldStorage(World& world,ID3D12Device* device,GpuLimits limits={});
    GpuFieldStorage(std::shared_ptr<World> world,ID3D12Device* device,GpuLimits limits={});
    ~GpuFieldStorage();
    GpuFieldStorage(const GpuFieldStorage&)=delete;
    GpuFieldStorage& operator=(const GpuFieldStorage&)=delete;
    GpuAcquisition acquire(LeaseRequest request);
    bool release(GpuLease lease,std::uint64_t lastUseFence=0);
    bool publish(ID3D12GraphicsCommandList* commands,std::uint64_t submissionFence,std::string& error);
    GpuViewResult view(GpuLease lease);
    bool bindings(GpuView view,GpuBindings& out) const;
    bool markUsed(GpuView view,std::uint64_t fence);
    bool releaseView(GpuView view,std::uint64_t lastUseFence=0);
    void collect(std::uint64_t completedFence);
    bool shutdown(std::uint64_t completedFence,std::string& error);
    GpuFieldStats stats() const;
    ID3D12Device* device() const;
    const World& world() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
constexpr std::size_t GpuFieldMaskWords=(BrickVoxels+31)/32;
constexpr std::size_t GpuFieldPageWords=3*BrickVoxels+2*GpuFieldMaskWords;
struct GpuFieldQuery {
    std::int32_t x,y,z;std::uint32_t channel;
    std::uint32_t fractionX=0,fractionY=0,fractionZ=0,filter=0; // 0=lattice, 1=Q8 trilinear
};
struct GpuFieldAnswer {
    std::int32_t value=0;std::uint32_t status=0;
    std::uint32_t sourceLow=0,sourceHigh=0,ageLow=0,ageHigh=0,versionLow=0,versionHigh=0;
    std::uint32_t clamped=0,padding[3]{};
};
static_assert(sizeof(GpuFieldQuery)==32&&sizeof(GpuFieldAnswer)==48,"GPU query ABI changed");
} // namespace engine::fields
