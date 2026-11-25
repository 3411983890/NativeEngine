#include "gpu_storage.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace engine::fields {
namespace {
template<class T> struct Com {
    T* p=nullptr;
    Com()=default;Com(const Com&)=delete;Com& operator=(const Com&)=delete;
    Com(Com&& other) noexcept:p(other.p){other.p=nullptr;}
    Com& operator=(Com&& other) noexcept{if(this!=&other){reset();p=other.p;other.p=nullptr;}return *this;}
    ~Com(){reset();}void reset(){if(p){p->Release();p=nullptr;}}
    T** out(){reset();return &p;}
};
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)){std::ostringstream message;message<<"NG-E-FIELD-GPU: "<<operation<<" HRESULT 0x"<<std::hex<<static_cast<unsigned>(hr);throw std::runtime_error(message.str());}
}
D3D12_RESOURCE_DESC bufferDescription(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
std::uint64_t allocation(ID3D12Device* device,std::uint64_t bytes) {
    auto d=bufferDescription(bytes);return device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;
}
Com<ID3D12Resource> buffer(ID3D12Device* device,std::uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
    Com<ID3D12Resource> result;D3D12_HEAP_PROPERTIES heap{};heap.Type=type;heap.CreationNodeMask=heap.VisibleNodeMask=1;auto d=bufferDescription(bytes);
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(result.out())),"Create page buffer");return result;
}
void upload(ID3D12Resource* resource,const void* bytes,std::size_t count) {
    void* mapped=nullptr;D3D12_RANGE read{0,0};check(resource->Map(0,&read,&mapped),"Map upload");std::memcpy(mapped,bytes,count);D3D12_RANGE written{0,count};resource->Unmap(0,&written);
}
std::int32_t brick(std::int32_t n){return n/BrickDim-(n%BrickDim<0?1:0);}
struct Key {
    ChannelId channel;Cell brick;
    bool operator<(const Key& b) const{return std::tie(channel,brick.x,brick.y,brick.z)<std::tie(b.channel,b.brick.x,b.brick.y,b.brick.z);}
};
struct PageRecord {std::int32_t x,y,z;std::uint32_t channel,resource,padding[3]{};};
static_assert(sizeof(PageRecord)==32&&BrickDim==16,"Sparse shader ABI changed");
std::uint32_t nextGeneration(std::uint32_t n){return n==std::numeric_limits<std::uint32_t>::max()?0:n+1;}
}
struct GpuFieldStorage::Impl {
    std::shared_ptr<World> worldOwner;
    struct LeaseSlot {bool active=false;std::uint32_t generation=1;Lease cpu;LeaseRequest request;};
    struct Snapshot {
        Com<ID3D12DescriptorHeap> descriptors;Com<ID3D12Resource> table;
        std::vector<Com<ID3D12Resource>> pages,uploads;
        std::uint64_t defaultBytes=0,uploadBytes=0,pageUploadBytes=0,uploadFence=0,lastFence=0,serial=0;
        Tick tick=0;std::uint64_t version=0;std::uint32_t channelCount=0;std::size_t viewRefs=0;
    };
    struct ViewSlot {bool active=false;std::uint32_t generation=1;std::shared_ptr<Snapshot> snapshot;LeaseRequest request;};
    World& world;ID3D12Device* device;GpuLimits limits;
    std::vector<LeaseSlot> leases;std::vector<ViewSlot> views;
    std::vector<std::shared_ptr<Snapshot>> snapshots;std::shared_ptr<Snapshot> current;
    std::uint64_t completed=0,serial=0,published=0,rejected=0;
    Impl(World& w,ID3D12Device* d,GpuLimits l):world(w),device(d),limits(l){device->AddRef();}
    ~Impl(){for(auto& s:leases)if(s.active)world.release(s.cpu);views.clear();current.reset();snapshots.clear();device->Release();}
    LeaseSlot* find(GpuLease h){if(h.slot>=leases.size())return nullptr;auto& s=leases[h.slot];return s.active&&s.generation==h.generation?&s:nullptr;}
    const ViewSlot* find(GpuView h) const{if(h.slot>=views.size())return nullptr;const auto& s=views[h.slot];return s.active&&s.generation==h.generation?&s:nullptr;}
    ViewSlot* find(GpuView h){return const_cast<ViewSlot*>(static_cast<const Impl*>(this)->find(h));}
    std::set<Key> keys(const LeaseRequest* extra=nullptr) const {
        std::set<Key> result;
        const auto append=[&](const LeaseRequest& r) {
            Region coverage;r.storageRegion(coverage);
            const Cell a{brick(coverage.min.x),brick(coverage.min.y),brick(coverage.min.z)},b{brick(coverage.max.x-1),brick(coverage.max.y-1),brick(coverage.max.z-1)};
            for(auto channel:r.channels)for(std::int64_t x=a.x;x<=b.x;++x)for(std::int64_t y=a.y;y<=b.y;++y)for(std::int64_t z=a.z;z<=b.z;++z)
                result.insert({channel,{static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),static_cast<std::int32_t>(z)}});
        };
        for(const auto& s:leases)if(s.active)append(s.request);if(extra)append(*extra);return result;
    }
    void collect(std::uint64_t fence) {
        completed=std::max(completed,fence);
        for(auto& s:snapshots)if(s->uploadFence<=completed&&!s->uploads.empty()){s->uploads.clear();s->uploadBytes-=s->pageUploadBytes;s->pageUploadBytes=0;}
        snapshots.erase(std::remove_if(snapshots.begin(),snapshots.end(),[&](const auto& s){return s!=current&&s->viewRefs==0&&s->lastFence<=completed;}),snapshots.end());
    }
};
std::array<std::uint32_t,28> GpuBindings::constants(std::uint32_t queries,Tick requested) const {
    std::array<std::uint32_t,28> out{};
    out[0]=region.min.x;out[1]=region.min.y;out[2]=region.min.z;out[3]=pageCount;
    out[4]=region.max.x;out[5]=region.max.y;out[6]=region.max.z;out[7]=queries;
    std::copy(channelMask.begin(),channelMask.end(),out.begin()+8);
    out[16]=static_cast<std::uint32_t>(snapshotTick);out[17]=static_cast<std::uint32_t>(snapshotTick>>32);
    out[18]=static_cast<std::uint32_t>(snapshotVersion);out[19]=static_cast<std::uint32_t>(snapshotVersion>>32);
    out[20]=static_cast<std::uint32_t>(requested);out[21]=static_cast<std::uint32_t>(requested>>32);
    out[22]=static_cast<std::uint32_t>(maxAgeTicks);out[23]=static_cast<std::uint32_t>(maxAgeTicks>>32);
    out[24]=static_cast<std::uint32_t>(consistency);out[25]=channelCount;out[26]=haloCells;return out;
}
GpuFieldStorage::GpuFieldStorage(World& world,ID3D12Device* device,GpuLimits limits) {
    if(!device||!limits.maxPages||limits.maxPages>4096||!limits.maxSnapshots||limits.maxSnapshots>16||!limits.maxLeases||limits.maxLeases>4096||!limits.maxViews||limits.maxViews>4096||!limits.maxAllocationBytes)
        throw std::invalid_argument("NG-E-FIELD-GPU-BUDGET");
    impl_=std::make_unique<Impl>(world,device,limits);
}
GpuFieldStorage::GpuFieldStorage(std::shared_ptr<World> world,ID3D12Device* device,GpuLimits limits)
    :GpuFieldStorage(world?*world:throw std::invalid_argument("NG-E-FIELD-GPU: null shared World"),device,limits){impl_->worldOwner=std::move(world);}
GpuFieldStorage::~GpuFieldStorage()=default;
GpuAcquisition GpuFieldStorage::acquire(LeaseRequest request) {
    auto& s=*impl_;const auto reject=[](const std::string& error){return GpuAcquisition{false,{},error};};
    if(request.writable)return reject("NG-E-FIELD-GPU: cache leases are read-only");
    Region coverage;if(!request.storageRegion(coverage))return reject("NG-E-FIELD-GPU: invalid halo or region");
    try {for(auto id:request.channels){const auto domain=s.world.channel(id).domain;if(domain!=Domain::VisualReference&&domain!=Domain::CpuAuthority)return reject("NG-E-FIELD-GPU: unknown channel domain");}}
    catch(const std::exception& e){return reject(e.what());}
    std::size_t index=0;while(index<s.leases.size()&&(s.leases[index].active||!s.leases[index].generation))++index;
    if(index>=s.limits.maxLeases)return reject("NG-E-FIELD-GPU: lease capacity");
    request.maxPages=std::min(request.maxPages,s.limits.maxPages);
    auto cpuRequest=request;cpuRequest.consistency=Consistency::Stale;cpuRequest.region=coverage;cpuRequest.haloCells=0;
    const auto cpu=s.world.acquire(std::move(cpuRequest));if(!cpu.accepted)return reject(cpu.error);
    if(s.keys(&request).size()>s.limits.maxPages){s.world.release(cpu.lease);s.world.collect(0);return reject("NG-E-FIELD-GPU: shared page budget");}
    if(index==s.leases.size())s.leases.emplace_back();auto& slot=s.leases[index];slot.cpu=cpu.lease;slot.request=std::move(request);slot.active=true;
    s.current.reset();s.collect(s.completed);return {true,{static_cast<std::uint32_t>(index),slot.generation},{}};
}
bool GpuFieldStorage::release(GpuLease h,std::uint64_t fence) {
    auto& s=*impl_;auto* slot=s.find(h);if(!slot)return false;
    s.world.release(slot->cpu,fence);slot->active=false;slot->generation=nextGeneration(slot->generation);slot->request={};
    if(s.current)s.current->lastFence=std::max(s.current->lastFence,fence);
    s.current.reset();s.collect(s.completed);return true;
}
bool GpuFieldStorage::publish(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error) {
    auto& s=*impl_;error.clear();s.collect(s.completed);
    const auto reject=[&](const char* message){++s.rejected;error=message;return false;};
    if(!commands||fence<=s.completed||!s.world.version())return reject("NG-E-FIELD-GPU: uncommitted world or invalid submission fence");
    if(s.current&&s.current->version==s.world.version())return true;
    const auto keys=s.keys();if(keys.empty())return reject("NG-E-FIELD-GPU: no spatial leases");
    if(s.snapshots.size()>=s.limits.maxSnapshots||s.serial==std::numeric_limits<std::uint64_t>::max())return reject("NG-E-FIELD-GPU: in-flight snapshot budget");
    try {
        constexpr auto pageBytes=GpuFieldPageWords*sizeof(std::uint32_t);
        const auto pageAllocation=allocation(s.device,pageBytes),tableBytes=keys.size()*sizeof(PageRecord),tableAllocation=allocation(s.device,tableBytes);
        const auto currentStats=stats();const auto used=currentStats.defaultAllocationBytes+currentStats.uploadAllocationBytes;
        const auto needed=keys.size()*2*pageAllocation+tableAllocation;
        if(needed>s.limits.maxAllocationBytes||used>s.limits.maxAllocationBytes-needed)return reject("NG-E-FIELD-GPU: committed allocation budget");
        auto next=std::make_shared<Impl::Snapshot>();next->tick=s.world.committedTick();next->version=s.world.version();next->channelCount=static_cast<std::uint32_t>(s.world.channelCount());
        next->uploadFence=next->lastFence=fence;next->serial=s.serial+1;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=static_cast<UINT>(keys.size()+1);heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(s.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(next->descriptors.out())),"Create sparse descriptors");
        auto descriptor=next->descriptors.p->GetCPUDescriptorHandleForHeapStart();const auto stride=s.device->GetDescriptorHandleIncrementSize(heap.Type);
        std::vector<PageRecord> table;table.reserve(keys.size());
        for(const auto& key:keys) {
            std::array<std::uint32_t,GpuFieldPageWords> data{};
            for(std::size_t i=0;i<BrickVoxels;++i) {
                const Cell cell{key.brick.x*16+static_cast<std::int32_t>(i%16),key.brick.y*16+static_cast<std::int32_t>((i/16)%16),key.brick.z*16+static_cast<std::int32_t>(i/256)};
                for(const auto& lease:s.leases) {
                    Region coverage;if(!lease.active||!lease.request.storageRegion(coverage)||!coverage.contains(cell)||std::find(lease.request.channels.begin(),lease.request.channels.end(),key.channel)==lease.request.channels.end())continue;
                    const auto value=s.world.sample(lease.cpu,key.channel,cell,next->tick);
                    if(value.status==Status::Ok||value.status==Status::Stale) {
                        data[i]=static_cast<std::uint32_t>(value.value);data[BrickVoxels+2*i]=static_cast<std::uint32_t>(value.sourceTick);data[BrickVoxels+2*i+1]=static_cast<std::uint32_t>(value.sourceTick>>32);
                        data[3*BrickVoxels+i/32]|=1u<<(i%32);if(value.clamped)data[3*BrickVoxels+GpuFieldMaskWords+i/32]|=1u<<(i%32);
                    }
                    break;
                }
            }
            next->pages.push_back(buffer(s.device,pageBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON));
            next->uploads.push_back(buffer(s.device,pageBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ));upload(next->uploads.back().p,data.data(),pageBytes);
            next->defaultBytes+=pageAllocation;next->uploadBytes+=pageAllocation;next->pageUploadBytes+=pageAllocation;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=static_cast<UINT>(GpuFieldPageWords);srv.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
            s.device->CreateShaderResourceView(next->pages.back().p,&srv,descriptor);descriptor.ptr+=stride;
            table.push_back({key.brick.x,key.brick.y,key.brick.z,key.channel,static_cast<std::uint32_t>(table.size()),{0,0,0}});
        }
        next->table=buffer(s.device,tableBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);upload(next->table.p,table.data(),tableBytes);next->uploadBytes+=tableAllocation;
        D3D12_SHADER_RESOURCE_VIEW_DESC tableView{};tableView.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;tableView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;tableView.Buffer.NumElements=static_cast<UINT>(table.size());tableView.Buffer.StructureByteStride=sizeof(PageRecord);
        s.device->CreateShaderResourceView(next->table.p,&tableView,descriptor);
        s.snapshots.reserve(s.snapshots.size()+1);
        // All allocations and CPU packing succeed before modifying the command list.
        for(std::size_t i=0;i<next->pages.size();++i) {
            D3D12_RESOURCE_BARRIER uploadBarrier{};uploadBarrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;uploadBarrier.Transition.pResource=next->pages[i].p;uploadBarrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;uploadBarrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;uploadBarrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;commands->ResourceBarrier(1,&uploadBarrier);
            commands->CopyBufferRegion(next->pages[i].p,0,next->uploads[i].p,0,pageBytes);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=next->pages[i].p;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;commands->ResourceBarrier(1,&barrier);
        }
        s.snapshots.push_back(next);s.current=next;++s.serial;++s.published;s.collect(s.completed);return true;
    } catch(const std::exception& e){++s.rejected;error=e.what();return false;}
}
GpuViewResult GpuFieldStorage::view(GpuLease h) {
    auto& s=*impl_;auto* lease=s.find(h);if(!lease)return {false,{},"NG-E-FIELD-GPU: invalid lease"};
    if(!s.current)return {false,{},"NG-E-FIELD-GPU: snapshot not published for this coverage"};
    std::size_t index=0;while(index<s.views.size()&&(s.views[index].active||!s.views[index].generation))++index;
    if(index>=s.limits.maxViews)return {false,{},"NG-E-FIELD-GPU: view capacity"};
    if(index==s.views.size())s.views.emplace_back();auto& slot=s.views[index];slot.request=lease->request;slot.snapshot=s.current;slot.active=true;++slot.snapshot->viewRefs;
    return {true,{static_cast<std::uint32_t>(index),slot.generation},{}};
}
bool GpuFieldStorage::bindings(GpuView h,GpuBindings& out) const {
    const auto* view=impl_->find(h);if(!view)return false;const auto& snapshot=*view->snapshot;
    out={};out.descriptors=snapshot.descriptors.p;out.pageTable=snapshot.table.p->GetGPUVirtualAddress();out.pageCount=static_cast<std::uint32_t>(snapshot.pages.size());out.channelCount=snapshot.channelCount;
    out.pageTableDescriptor=out.pageCount;
    out.snapshotTick=snapshot.tick;out.snapshotVersion=snapshot.version;out.serial=snapshot.serial;out.region=view->request.region;out.consistency=view->request.consistency;out.maxAgeTicks=view->request.consistency==Consistency::Strong?0:view->request.maxAgeTicks;out.haloCells=view->request.haloCells;
    for(auto id:view->request.channels)out.channelMask[id/32]|=1u<<(id%32);return true;
}
bool GpuFieldStorage::markUsed(GpuView h,std::uint64_t fence) {auto* view=impl_->find(h);if(!view||fence<=impl_->completed)return false;view->snapshot->lastFence=std::max(view->snapshot->lastFence,fence);return true;}
bool GpuFieldStorage::releaseView(GpuView h,std::uint64_t fence) {
    auto& s=*impl_;auto* view=s.find(h);if(!view)return false;view->snapshot->lastFence=std::max(view->snapshot->lastFence,fence);--view->snapshot->viewRefs;view->snapshot.reset();view->request={};view->active=false;view->generation=nextGeneration(view->generation);s.collect(s.completed);return true;
}
void GpuFieldStorage::collect(std::uint64_t completed){impl_->collect(completed);}
bool GpuFieldStorage::shutdown(std::uint64_t fence,std::string& error) {
    collect(fence);const auto state=stats();error.clear();if(state.leases||state.views||state.snapshots){error="NG-E-FIELD-GPU: active leases/views or incomplete fences";return false;}return true;
}
GpuFieldStats GpuFieldStorage::stats() const {
    const auto& s=*impl_;GpuFieldStats out;out.completedFence=s.completed;out.publishedSnapshots=s.published;out.rejectedSnapshots=s.rejected;
    for(const auto& lease:s.leases)if(lease.active)++out.leases;for(const auto& view:s.views)if(view.active)++out.views;
    out.snapshots=s.snapshots.size();out.currentPages=s.current?s.current->pages.size():0;
    for(const auto& snapshot:s.snapshots){out.residentPages+=snapshot->pages.size();out.descriptorSlots+=snapshot->pages.size()+1;out.defaultAllocationBytes+=snapshot->defaultBytes;out.uploadAllocationBytes+=snapshot->uploadBytes;}return out;
}
ID3D12Device* GpuFieldStorage::device() const{return impl_->device;}
const World& GpuFieldStorage::world() const{return impl_->world;}
} // namespace engine::fields
