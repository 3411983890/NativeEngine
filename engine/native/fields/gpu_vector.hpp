#pragma once
#include "gpu_storage.hpp"
#include "../vec3.hpp"

namespace engine::fields {
struct GpuVectorSpec {
    // Logical names are mapped to three scalar m/s channels on one explicit grid.
    std::map<std::string,std::array<ChannelId,3>> channels;
    Vec3 originMetres{};
    float cellMetres=1;
};
struct GpuVectorLimits {std::size_t maxBindings=3;std::uint64_t maxAllocationBytes=3*65536ull;};
struct GpuVectorStats {std::size_t bindings=0;std::uint64_t allocationBytes=0,completedFence=0,rejectedBindings=0;};
// One immutable, read-only query binding. Its requested tick is explicit and
// separate from effect age. The owning GpuVectorFields retains it until the
// actual last-use fence completes, even after clients replace their binding.
class GpuVectorView {
public:
    ~GpuVectorView();
    GpuVectorView(const GpuVectorView&)=delete;
    bool supports(const std::string& logicalName) const;
    Tick requestedTick() const;
    float cellMetres() const;
    ID3D12Device* device() const;
    ID3D12DescriptorHeap* descriptors() const;
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress() const;
    bool canUse(std::uint64_t fence) const;
    bool markUsed(std::uint64_t fence);
    std::uint64_t lastFence() const;
private:
    GpuVectorView();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class GpuVectorFields;
};
struct GpuVectorResult {std::shared_ptr<GpuVectorView> view;std::string error;bool accepted() const{return !!view;}};
// Small bounded metadata cache over GpuFieldStorage. This class and World must
// remain alive through their outstanding fence use. No waits or readbacks.
class GpuVectorFields {
public:
    explicit GpuVectorFields(std::shared_ptr<GpuFieldStorage> storage,GpuVectorLimits limits={});
    ~GpuVectorFields();
    GpuVectorFields(const GpuVectorFields&)=delete;
    GpuVectorResult bind(GpuLease lease,const GpuVectorSpec& spec,Tick requestedTick);
    void collect(std::uint64_t completedFence);
    GpuVectorStats stats() const;
    bool shutdown(std::uint64_t completedFence,std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
