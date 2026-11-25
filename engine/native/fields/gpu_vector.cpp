#include "gpu_vector.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace engine::fields {
namespace {
struct Constants {
    std::array<std::uint32_t,28> query;
    std::uint32_t table=0,wind[3]{};
    std::uint32_t water[3]{},filter=1;
    float origin[3]{},inverseCell=1;
    float windScale[3]{};std::uint32_t flags=0;
    float waterScale[3]{};std::uint32_t reserved=0;
};
static_assert(sizeof(Constants)==192,"Sparse vector constant ABI changed");
D3D12_RESOURCE_DESC description(){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=256;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;}
}
struct GpuVectorView::Impl {
    std::shared_ptr<GpuFieldStorage> storage;GpuView view;GpuBindings bindings;
    ID3D12Resource* constants=nullptr;Tick requested=0;std::uint64_t fence=0,bytes=0;float cell=1;unsigned flags=0;
    ~Impl(){storage->releaseView(view,fence);if(constants)constants->Release();}
};
GpuVectorView::GpuVectorView()=default;
GpuVectorView::~GpuVectorView()=default;
bool GpuVectorView::supports(const std::string& name) const{return impl_&&((name=="windVelocity"&&(impl_->flags&1))||(name=="waterVelocity"&&(impl_->flags&2)));}
Tick GpuVectorView::requestedTick() const{return impl_->requested;}
float GpuVectorView::cellMetres() const{return impl_->cell;}
ID3D12Device* GpuVectorView::device() const{return impl_->storage->device();}
ID3D12DescriptorHeap* GpuVectorView::descriptors() const{return impl_->bindings.descriptors;}
D3D12_GPU_VIRTUAL_ADDRESS GpuVectorView::constantsAddress() const{return impl_->constants->GetGPUVirtualAddress();}
bool GpuVectorView::canUse(std::uint64_t fence) const{return fence>impl_->storage->stats().completedFence&&fence>=impl_->fence;}
bool GpuVectorView::markUsed(std::uint64_t fence){if(!canUse(fence)||!impl_->storage->markUsed(impl_->view,fence))return false;impl_->fence=fence;return true;}
std::uint64_t GpuVectorView::lastFence() const{return impl_->fence;}
struct GpuVectorFields::Impl {
    std::shared_ptr<GpuFieldStorage> storage;GpuVectorLimits limits;std::vector<std::shared_ptr<GpuVectorView>> views;
    std::uint64_t completed=0,rejected=0;
};
GpuVectorFields::GpuVectorFields(std::shared_ptr<GpuFieldStorage> storage,GpuVectorLimits limits){
    if(!storage||!limits.maxBindings||limits.maxBindings>256||!limits.maxAllocationBytes)throw std::invalid_argument("NG-E-FIELD-VECTOR: invalid binding limits");
    impl_=std::make_unique<Impl>();impl_->storage=std::move(storage);impl_->limits=limits;
}
GpuVectorFields::~GpuVectorFields()=default;
GpuVectorResult GpuVectorFields::bind(GpuLease lease,const GpuVectorSpec& spec,Tick requested){
    auto& s=*impl_;const auto reject=[&](const std::string& error){++s.rejected;return GpuVectorResult{{},error};};collect(s.completed);
    if(spec.channels.empty()||spec.channels.size()>2||!std::isfinite(spec.cellMetres)||spec.cellMetres<.001f||spec.cellMetres>100||!std::isfinite(spec.originMetres.x)||!std::isfinite(spec.originMetres.y)||!std::isfinite(spec.originMetres.z))return reject("NG-E-FIELD-VECTOR: invalid metre grid or vector mapping");
    auto acquired=s.storage->view(lease);if(!acquired.accepted)return reject(acquired.error);
    auto state=std::make_unique<GpuVectorView::Impl>();state->storage=s.storage;state->view=acquired.view;state->requested=requested;state->cell=spec.cellMetres;
    if(!s.storage->bindings(state->view,state->bindings))return reject("NG-E-FIELD-VECTOR: view disappeared");
    const auto& b=state->bindings;
    if(requested<b.snapshotTick||(b.consistency==Consistency::Strong&&requested!=b.snapshotTick))return reject("NG-E-FIELD-VECTOR: requested tick is not available in this snapshot");
    Constants data{};data.query=b.constants(0,requested);data.table=b.pageTableDescriptor;data.origin[0]=spec.originMetres.x;data.origin[1]=spec.originMetres.y;data.origin[2]=spec.originMetres.z;data.inverseCell=1/spec.cellMetres;
    try{for(const auto& item:spec.channels){unsigned flag=item.first=="windVelocity"?1:item.first=="waterVelocity"?2:0;if(!flag)return reject("NG-E-FIELD-VECTOR: unsupported logical vector channel");
            auto* ids=flag==1?data.wind:data.water;auto* scales=flag==1?data.windScale:data.waterScale;Domain domain=Domain::VisualReference;
            for(unsigned axis=0;axis<3;++axis){auto id=item.second[axis];if(id>=b.channelCount||(b.channelMask[id/32]&(1u<<(id%32)))==0)return reject("NG-E-FIELD-VECTOR: component not granted by the view lease");
                const auto& channel=s.storage->world().channel(id);if(channel.unit!="m/s"||!channel.unitsPerInteger||(axis&&channel.domain!=domain))return reject("NG-E-FIELD-VECTOR: velocity requires m/s components from one domain");
                domain=channel.domain;ids[axis]=id;scales[axis]=1.f/channel.unitsPerInteger;}
            data.flags|=flag;
        }}catch(const std::exception& e){return reject(e.what());}
    state->flags=data.flags;auto desc=description();state->bytes=s.storage->device()->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
    const auto used=stats().allocationBytes;
    if(s.views.size()>=s.limits.maxBindings||state->bytes>s.limits.maxAllocationBytes||used>s.limits.maxAllocationBytes-state->bytes)return reject("NG-E-FIELD-VECTOR: outstanding metadata binding budget exhausted");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    if(FAILED(s.storage->device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&state->constants))))return reject("NG-E-FIELD-VECTOR: metadata allocation failed");
    void* mapped=nullptr;D3D12_RANGE none{0,0};if(FAILED(state->constants->Map(0,&none,&mapped)))return reject("NG-E-FIELD-VECTOR: metadata map failed");std::memset(mapped,0,256);std::memcpy(mapped,&data,sizeof(data));D3D12_RANGE written{0,256};state->constants->Unmap(0,&written);
    auto out=std::shared_ptr<GpuVectorView>(new GpuVectorView);out->impl_=std::move(state);s.views.push_back(out);return {out,{}};
}
void GpuVectorFields::collect(std::uint64_t completed){auto& s=*impl_;s.completed=std::max(s.completed,completed);s.views.erase(std::remove_if(s.views.begin(),s.views.end(),[&](const auto& view){return view.use_count()==1&&view->lastFence()<=s.completed;}),s.views.end());s.storage->collect(s.completed);}
GpuVectorStats GpuVectorFields::stats() const{const auto& s=*impl_;GpuVectorStats out;out.bindings=s.views.size();out.completedFence=s.completed;out.rejectedBindings=s.rejected;for(const auto& view:s.views)out.allocationBytes+=view->impl_->bytes;return out;}
bool GpuVectorFields::shutdown(std::uint64_t fence,std::string& error){collect(fence);error.clear();if(!impl_->views.empty()){error="NG-E-FIELD-VECTOR: active clients or incomplete metadata fences";return false;}return true;}
}
