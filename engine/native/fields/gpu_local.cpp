#include "gpu_local.hpp"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace engine::fields {
namespace {
template<class T>struct Com{
    T* p=nullptr;Com()=default;Com(const Com&)=delete;Com& operator=(const Com&)=delete;
    Com(Com&& other) noexcept:p(other.p){other.p=nullptr;}
    Com& operator=(Com&& other) noexcept{if(this!=&other){if(p)p->Release();p=other.p;other.p=nullptr;}return *this;}
    ~Com(){if(p)p->Release();}T** out(){return &p;}
};
void check(HRESULT code,const char* why){if(FAILED(code))throw std::runtime_error(std::string("NG-E-FIELD-LOCAL-GPU: ")+why);}
std::uint32_t bits(float v){std::uint32_t out;std::memcpy(&out,&v,4);return out;}
int floorDiv(int value,int divisor){return value/divisor-(value%divisor<0);}
std::uint32_t nextGeneration(std::uint32_t value){return value==UINT32_MAX?0:value+1;}
D3D12_RESOURCE_DESC desc(std::uint64_t bytes){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;return d;}
std::uint64_t allocation(ID3D12Device* device,std::uint64_t bytes){const auto d=desc(bytes);return device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;}
void makeBuffer(ID3D12Device* device,Com<ID3D12Resource>& resource,std::uint64_t bytes){D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_DEFAULT;p.CreationNodeMask=p.VisibleNodeMask=1;const auto d=desc(bytes);check(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(resource.out())),"Create local field buffer");}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.StateBefore=from;b.Transition.StateAfter=to;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;list->ResourceBarrier(1,&b);}
void barrier(ID3D12GraphicsCommandList* list){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&b);}
Region footprint(const LocalSpec& s){const auto r=static_cast<int>(s.finePerWorld);return {{floorDiv(s.originFine.x,r),floorDiv(s.originFine.y,r),floorDiv(s.originFine.z,r)},{floorDiv(s.originFine.x+s.dimensions.x-1,r)+1,floorDiv(s.originFine.y+s.dimensions.y-1,r)+1,floorDiv(s.originFine.z+s.dimensions.z-1,r)+1}};}
std::uint64_t volume(Cell d){return std::uint64_t(d.x)*d.y*d.z;}
Cell size(Region r){return {r.max.x-r.min.x,r.max.y-r.min.y,r.max.z-r.min.z};}
enum Pipeline {Clear,Splat,Resolve,Project,Finalize,ImportScalar,PipelineCount};
}
struct GpuLocalProjection::Impl {
    struct Plane{Com<ID3D12Resource> values,groups,metrics;};
    Com<ID3D12Fence> timeline;Com<ID3D12Device> device;Com<ID3D12Resource> diagnostics;GpuProjectionBindings binding;std::vector<Plane> planes;
    std::uint64_t bytes=0,fence=0;
};
GpuLocalProjection::GpuLocalProjection()=default;GpuLocalProjection::~GpuLocalProjection()=default;
GpuProjectionBindings GpuLocalProjection::bindings() const{return impl_?impl_->binding:GpuProjectionBindings{};}
bool GpuLocalProjection::markUsed(std::uint64_t fence){if(!impl_||!fence||fence<impl_->fence||fence<=impl_->timeline.p->GetCompletedValue())return false;impl_->fence=fence;return true;}
std::uint64_t GpuLocalProjection::lastFence() const{return impl_?impl_->fence:0;}
ID3D12Device* GpuLocalProjection::device() const{return impl_?impl_->device.p:nullptr;}
ID3D12Fence* GpuLocalProjection::timeline() const{return impl_?impl_->timeline.p:nullptr;}
struct GpuLocalFields::Impl {
    struct Field{
        LocalSpec spec;GpuLocalInfo info;GpuLocalFootprint cost;std::array<Com<ID3D12Resource>,3> buffers;
        std::vector<std::uint64_t> admitted;std::uint64_t bytes=0;std::uint32_t voxels=0;
        bool initialized=false,hasTick=false;std::uint64_t epoch=0,lastProjectedVersion=0;Tick lastPublish=0;
        std::weak_ptr<GpuLocalProjection> lastView;
    };
    struct Slot{std::uint32_t generation=1;std::shared_ptr<Field> field;};
    struct Input{std::uint64_t fence=0;std::array<Com<ID3D12Resource>,4> resources;};
    Com<ID3D12Device> device;Com<ID3D12Fence> timeline;Com<ID3D12RootSignature> root;
    std::array<Com<ID3D12PipelineState>,PipelineCount> pipelines;
    std::vector<Channel> catalog;GpuLocalLimits limits;GpuLocalGrid grid;
    std::vector<Slot> slots;std::vector<std::shared_ptr<Field>> retired;
    std::vector<std::shared_ptr<GpuLocalProjection>> views;std::vector<Input> inputs;
    std::uint64_t lastSource=0;GpuLocalStats counters;
    Impl(ID3D12Device* d,ID3D12Fence* f,std::vector<Channel> c,GpuLocalLimits l,GpuLocalGrid g):catalog(std::move(c)),limits(l),grid(g){
        device.p=d;d->AddRef();timeline.p=f;f->AddRef();slots.reserve(l.maxInstances);retired.reserve(l.maxInstances);views.reserve(l.maxProjectionViews);inputs.reserve(l.maxInputBatches);
        D3D12_ROOT_PARAMETER p[10]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=32;
        for(unsigned i=0;i<3;++i){p[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[i+1].Descriptor.ShaderRegister=i;}
        for(unsigned i=0;i<6;++i){p[i+4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[i+4].Descriptor.ShaderRegister=i;}
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=10;rd.pParameters=p;Com<ID3DBlob> bytes,errors;
        check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),errors.out()),"Serialize local field root");check(d->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create local field root");
        const nativegpu::shaders::Blob* blobs[]={&nativegpu::shaders::kLocalFieldClear,&nativegpu::shaders::kLocalFieldSplat,&nativegpu::shaders::kLocalFieldResolve,&nativegpu::shaders::kLocalFieldProject,&nativegpu::shaders::kLocalFieldFinalize,&nativegpu::shaders::kLocalFieldImportScalar};
        for(unsigned i=0;i<PipelineCount;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.p;pd.CS={blobs[i]->data,blobs[i]->size};check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(pipelines[i].out())),"Create local field pipeline");}
    }
    Field* find(GpuLocalHandle h) const{if(h.slot>=slots.size()||slots[h.slot].generation!=h.generation)return nullptr;return slots[h.slot].field.get();}
    bool valid(ID3D12GraphicsCommandList* list,std::uint64_t fence) const{
        if(!list||!fence||fence<counters.lastFence||fence<=timeline.p->GetCompletedValue()||(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return false;
        Com<ID3D12Device> owner;return SUCCEEDED(list->GetDevice(IID_PPV_ARGS(owner.out())))&&owner.p==device.p;
    }
    void collect(){const auto done=timeline.p->GetCompletedValue();inputs.erase(std::remove_if(inputs.begin(),inputs.end(),[&](const auto& in){return in.fence<=done;}),inputs.end());retired.erase(std::remove_if(retired.begin(),retired.end(),[&](const auto& f){return f->info.lastFence<=done;}),retired.end());views.erase(std::remove_if(views.begin(),views.end(),[&](const auto& v){return v.use_count()==1&&v->lastFence()<=done;}),views.end());}
    GpuLocalStats stats() const{auto out=counters;for(const auto& slot:slots)if(slot.field){++out.instances;out.allocationBytes+=slot.field->bytes;out.localSamples+=std::uint64_t(slot.field->voxels)*slot.field->spec.channels.size();}for(const auto& f:retired){++out.retiredInstances;out.allocationBytes+=f->bytes;out.localSamples+=std::uint64_t(f->voxels)*f->spec.channels.size();}for(const auto& v:views){++out.projectionViews;out.allocationBytes+=v->impl_->bytes;}for(const auto& input:inputs)for(const auto& resource:input.resources)if(resource.p)++out.heldInputReferences;return out;}
    std::array<std::uint32_t,32> constants(const Field& f) const{
        std::array<std::uint32_t,32> c{};c[0]=f.spec.dimensions.x;c[1]=f.spec.dimensions.y;c[2]=f.spec.dimensions.z;c[3]=f.voxels;c[4]=f.spec.originFine.x;c[5]=f.spec.originFine.y;c[6]=f.spec.originFine.z;c[7]=f.spec.channels.size();c[8]=f.spec.finePerWorld;
        c[12]=bits(grid.originMetres.x);c[13]=bits(grid.originMetres.y);c[14]=bits(grid.originMetres.z);c[15]=bits(f.spec.finePerWorld/grid.worldCellMetres);return c;
    }
    void bind(ID3D12GraphicsCommandList* list,Field& f,const std::array<std::uint32_t,32>& c){list->SetComputeRootSignature(root.p);list->SetComputeRoot32BitConstants(0,32,c.data(),0);for(unsigned i=0;i<3;++i)list->SetComputeRootUnorderedAccessView(i+4,f.buffers[i].p->GetGPUVirtualAddress());}
    void dispatch(ID3D12GraphicsCommandList* list,Pipeline p,unsigned count){const auto groups=(count+63)/64;list->SetPipelineState(pipelines[p].p);list->Dispatch(std::min(groups,65535u),(groups+65534)/65535,1);barrier(list);++counters.dispatches;}
    void touch(Field& f,std::uint64_t fence){f.info.lastFence=fence;counters.lastFence=fence;}
};
GpuLocalFields::GpuLocalFields(ID3D12Device* device,ID3D12Fence* timeline,std::vector<Channel> catalog,GpuLocalLimits limits,GpuLocalGrid grid){
    if(!device||!timeline||catalog.empty()||catalog.size()>256||!limits.maxInstances||limits.maxInstances>256||!limits.maxProjectionViews||limits.maxProjectionViews>256||!limits.maxInputBatches||limits.maxInputBatches>4096||!limits.maxParticlesPerSplat||limits.maxParticlesPerSplat>1000000||!limits.maxLocalSamples||limits.maxLocalSamples>16777216||!limits.maxAllocationBytes||!std::isfinite(grid.worldCellMetres)||grid.worldCellMetres<.001f||grid.worldCellMetres>100||!std::isfinite(grid.originMetres.x)||!std::isfinite(grid.originMetres.y)||!std::isfinite(grid.originMetres.z))throw std::invalid_argument("NG-E-FIELD-LOCAL-GPU: invalid device, catalog, grid or limits");
    Com<ID3D12Device> owner;check(timeline->GetDevice(IID_PPV_ARGS(owner.out())),"Query local field timeline");if(owner.p!=device)throw std::invalid_argument("NG-E-FIELD-LOCAL-GPU: fence device mismatch");if(!nativegpu::shaders::kDxcAvailable)throw std::invalid_argument("NG-E-FIELD-LOCAL-GPU: shaders unavailable");impl_=std::make_unique<Impl>(device,timeline,std::move(catalog),limits,grid);
}
GpuLocalFields::~GpuLocalFields()=default;
bool GpuLocalFields::estimate(const LocalSpec& spec,GpuLocalFootprint& out,std::string& error) const{
    error.clear();out={};if(!impl_){error="NG-E-FIELD-LOCAL-GPU: stopped";return false;}const auto& s=*impl_;
    const auto invalid=[&]{error="NG-E-FIELD-LOCAL-GPU: invalid estimate spec";return false;};
    if(spec.owner.empty()||spec.channels.empty()||spec.channels.size()>8||!spec.maxWorldPages||spec.maxWorldPages>4096||(spec.finePerWorld!=1&&spec.finePerWorld!=2&&spec.finePerWorld!=4&&spec.finePerWorld!=8&&spec.finePerWorld!=16))return invalid();
    const int origins[]={spec.originFine.x,spec.originFine.y,spec.originFine.z},dims[]={spec.dimensions.x,spec.dimensions.y,spec.dimensions.z};for(unsigned i=0;i<3;++i)if(dims[i]<1||dims[i]>128||origins[i]<-1048575||std::int64_t(origins[i])+dims[i]>1048576)return invalid();
    std::set<ChannelId> seen;for(const auto& c:spec.channels){if(c.channel>=s.catalog.size()||!seen.insert(c.channel).second)return invalid();const auto& d=s.catalog[c.channel];if(d.domain!=Domain::VisualReference||d.owner!=spec.owner||d.minimum!=0||d.maximum<=0||!d.unitsPerInteger||(c.reduction==ProjectionReduction::VolumeAverage&&d.merge!=Merge::Sum)||(c.reduction==ProjectionReduction::Maximum&&d.merge!=Merge::Maximum)||(c.reduction!=ProjectionReduction::VolumeAverage&&c.reduction!=ProjectionReduction::Maximum))return invalid();}
    out.samples=volume(spec.dimensions)*spec.channels.size();out.mutableBytes=allocation(s.device.p,out.samples*4)*2+allocation(s.device.p,GpuLocalDiagnosticWords*4);out.coarseRegion=footprint(spec);const auto r=out.coarseRegion;const auto count=volume(size(r)),groups=(count+63)/64;
    out.projectionBytes=(allocation(s.device.p,count*4)+allocation(s.device.p,groups*16)+allocation(s.device.p,32))*spec.channels.size()+allocation(s.device.p,GpuLocalDiagnosticWords*4);out.worldPages=std::uint64_t(floorDiv(r.max.x-1,16)-floorDiv(r.min.x,16)+1)*(floorDiv(r.max.y-1,16)-floorDiv(r.min.y,16)+1)*(floorDiv(r.max.z-1,16)-floorDiv(r.min.z,16)+1)*spec.channels.size();return true;
}
bool GpuLocalFields::create(LocalSpec spec,GpuLocalHandle& out,std::string& error){
    error.clear();if(!impl_){error="NG-E-FIELD-LOCAL-GPU: stopped";return false;}auto& s=*impl_;s.collect();
    const auto bad=[&](const char* why){error=std::string("NG-E-FIELD-LOCAL-GPU: ")+why;return false;};
    if(spec.owner.empty()||!spec.source||spec.source<=s.lastSource||spec.channels.empty()||spec.channels.size()>8||!spec.maxWorldPages||spec.maxWorldPages>4096||(spec.finePerWorld!=1&&spec.finePerWorld!=2&&spec.finePerWorld!=4&&spec.finePerWorld!=8&&spec.finePerWorld!=16))return bad("invalid source, channels or projection limits");
    const int origins[]={spec.originFine.x,spec.originFine.y,spec.originFine.z},dims[]={spec.dimensions.x,spec.dimensions.y,spec.dimensions.z};for(unsigned i=0;i<3;++i)if(dims[i]<1||dims[i]>128||origins[i]<-1048575||std::int64_t(origins[i])+dims[i]>1048576)return bad("invalid local region");
    std::set<ChannelId> seen;for(const auto& c:spec.channels){if(c.channel>=s.catalog.size()||!seen.insert(c.channel).second)return bad("unknown or repeated channel");const auto& d=s.catalog[c.channel];if(d.domain!=Domain::VisualReference||d.owner!=spec.owner||d.minimum!=0||d.maximum<=0||!d.unitsPerInteger||(c.reduction==ProjectionReduction::VolumeAverage&&d.merge!=Merge::Sum)||(c.reduction==ProjectionReduction::Maximum&&d.merge!=Merge::Maximum)||(c.reduction!=ProjectionReduction::VolumeAverage&&c.reduction!=ProjectionReduction::Maximum))return bad("unsupported domain, owner or reduction");}
    const auto voxels=volume(spec.dimensions),samples=voxels*spec.channels.size();const auto before=s.stats();if(before.instances>=s.limits.maxInstances||samples>s.limits.maxLocalSamples||before.localSamples>s.limits.maxLocalSamples-samples)return bad("local sample/instance budget");
    GpuLocalFootprint cost;if(!estimate(spec,cost,error))return false;const auto bytes=cost.mutableBytes;if(bytes>s.limits.maxAllocationBytes||before.allocationBytes>s.limits.maxAllocationBytes-bytes)return bad("aligned allocation budget");
    try{auto f=std::make_shared<Impl::Field>();f->spec=std::move(spec);f->cost=cost;f->info.source=f->spec.source;f->voxels=static_cast<unsigned>(voxels);f->bytes=bytes;f->admitted.resize(f->spec.channels.size());makeBuffer(s.device.p,f->buffers[0],samples*4);makeBuffer(s.device.p,f->buffers[1],samples*4);makeBuffer(s.device.p,f->buffers[2],GpuLocalDiagnosticWords*4);
        unsigned slot=0;while(slot<s.slots.size()&&(s.slots[slot].field||!s.slots[slot].generation))++slot;if(slot==s.slots.size())s.slots.emplace_back();s.lastSource=f->spec.source;s.slots[slot].field=std::move(f);out={slot,s.slots[slot].generation};return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool GpuLocalFields::begin(GpuLocalHandle h,ID3D12GraphicsCommandList* list,std::uint64_t fence,Tick tick,std::string& error){
    error.clear();auto* f=impl_?impl_->find(h):nullptr;if(!f||!impl_->valid(list,fence)||f->info.accumulating||(f->hasTick&&tick<=f->info.sourceTick)||tick==UINT64_MAX){error="NG-E-FIELD-LOCAL-GPU: invalid begin phase, time or fence";return false;}auto& s=*impl_;
    if(!f->initialized){for(auto& b:f->buffers)transition(list,b.p,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);f->initialized=true;}
    s.bind(list,*f,s.constants(*f));s.dispatch(list,Clear,f->voxels*f->spec.channels.size());std::fill(f->admitted.begin(),f->admitted.end(),0);f->info.sourceTick=tick;f->info.inputVersion=0;f->info.accumulating=true;f->info.resolved=false;f->hasTick=true;s.touch(*f,fence);return true;
}
bool GpuLocalFields::splat(GpuLocalHandle h,ChannelId channel,effects::GpuParticlePool& pool,std::uint32_t quantum,ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
    error.clear();auto* f=impl_?impl_->find(h):nullptr;if(!f||!impl_->valid(list,fence)||!f->info.accumulating||!quantum){error="NG-E-FIELD-LOCAL-GPU: invalid splat phase or quantum";return false;}auto& s=*impl_;const auto b=pool.buffers();const auto state=pool.stats();
    const auto found=std::find_if(f->spec.channels.begin(),f->spec.channels.end(),[&](const auto& c){return c.channel==channel;});if(found==f->spec.channels.end()||!state.initialized||state.lastFence>fence||!b.attributes||!b.alive||!b.counters||!b.drawArguments||!b.capacity||b.capacity>s.limits.maxParticlesPerSplat){error="NG-E-FIELD-LOCAL-GPU: invalid source pool or channel";return false;}
    const unsigned slot=static_cast<unsigned>(found-f->spec.channels.begin());const auto promised=std::uint64_t(b.capacity)*quantum;
    if(promised>UINT32_MAX||f->admitted[slot]>UINT32_MAX-promised){error="NG-E-FIELD-LOCAL-GPU: uint32 contribution bound exceeded";return false;}
    Com<ID3D12Device> owner;if(FAILED(b.attributes->GetDevice(IID_PPV_ARGS(owner.out())))||owner.p!=s.device.p){error="NG-E-FIELD-LOCAL-GPU: input device mismatch";return false;}
    s.collect();if(s.inputs.size()>=s.limits.maxInputBatches){error="NG-E-FIELD-LOCAL-GPU: in-flight input batch budget";return false;}
    if(!pool.prepareDraw(list,fence,error))return false;
    auto c=s.constants(*f);c[9]=slot;c[10]=quantum;c[11]=b.localSpace;c[16]=bits(b.origin.x);c[17]=bits(b.origin.y);c[18]=bits(b.origin.z);c[19]=b.capacity;c[20]=b.layout.position;
    Impl::Input input;input.fence=fence;ID3D12Resource* resources[]={b.attributes,b.alive,b.counters,b.drawArguments};for(unsigned i=0;i<4;++i){input.resources[i].p=resources[i];resources[i]->AddRef();}s.inputs.push_back(std::move(input));
    transition(list,b.counters,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);s.bind(list,*f,c);for(unsigned i=0;i<3;++i)list->SetComputeRootShaderResourceView(i+1,resources[i]->GetGPUVirtualAddress());s.dispatch(list,Splat,b.capacity);transition(list,b.counters,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f->admitted[slot]+=promised;++s.counters.splatBatches;s.touch(*f,fence);return true;
}
bool GpuLocalFields::resolve(GpuLocalHandle h,ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
    error.clear();auto* f=impl_?impl_->find(h):nullptr;if(!f||!impl_->valid(list,fence)||!f->info.accumulating||f->info.version==UINT64_MAX){error="NG-E-FIELD-LOCAL-GPU: invalid resolve";return false;}auto& s=*impl_;
    for(unsigned i=0;i<f->spec.channels.size();++i){auto c=s.constants(*f);c[9]=i;c[21]=s.catalog[f->spec.channels[i].channel].maximum;s.bind(list,*f,c);s.dispatch(list,Resolve,f->voxels);}f->info.accumulating=false;f->info.resolved=true;++f->info.version;s.touch(*f,fence);return true;
}
bool GpuLocalFields::replaceScalar(GpuLocalHandle h,const GpuScalarInput& input,ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
    error.clear();auto* f=impl_?impl_->find(h):nullptr;
    const auto reject=[&](const char* why){error=std::string("NG-E-FIELD-SCALAR-GPU: ")+why;return false;};
    if(!f||!impl_->valid(list,fence)||f->info.accumulating||f->spec.channels.size()!=1||f->info.version==UINT64_MAX||input.sourceTick==UINT64_MAX||(f->hasTick&&input.sourceTick<=f->info.sourceTick))return reject("invalid phase, channels, source tick or fence");
    auto& s=*impl_;const auto& definition=s.catalog[f->spec.channels[0].channel];
    if(!input.values||input.timeline!=s.timeline.p||!input.prepareRead||!input.version||!input.lastFence||input.lastFence>fence||!(input.dimensions==f->spec.dimensions))return reject("invalid producer resource, timeline, version or dimensions");
    if(!std::isfinite(input.maximum)||input.maximum<0||definition.unitsPerInteger>8388607||definition.maximum>8388607||double(input.maximum)*definition.unitsPerInteger>8388607)return reject("float quantization range exceeds exact integer bound");
    const double cell=double(s.grid.worldCellMetres)/f->spec.finePerWorld;
    if(!std::isfinite(input.cellMetres)||std::abs(double(input.cellMetres)-cell)>cell*1e-6)return reject("cell scale mismatch");
    const float origins[]={input.originMetres.x,input.originMetres.y,input.originMetres.z},grid[]={s.grid.originMetres.x,s.grid.originMetres.y,s.grid.originMetres.z};
    const int lattice[]={f->spec.originFine.x,f->spec.originFine.y,f->spec.originFine.z};
    for(unsigned a=0;a<3;++a){const double expected=double(grid[a])+lattice[a]*cell;if(!std::isfinite(origins[a])||std::abs(double(origins[a])-expected)>cell*1e-5)return reject("origin is not aligned to the fine grid");}
    if(input.current&&!input.current())return reject("producer state changed or closed");
    const auto description=input.values->GetDesc();Com<ID3D12Device> owner;
    if(description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||(description.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||(std::uint64_t(input.offset)+f->voxels)*4>0x100000000ull||description.Width<(std::uint64_t(input.offset)+f->voxels)*4||FAILED(input.values->GetDevice(IID_PPV_ARGS(owner.out())))||owner.p!=s.device.p)return reject("invalid scalar buffer extent or device");
    s.collect();if(s.inputs.size()>=s.limits.maxInputBatches)return reject("in-flight input batch budget");
    // Admission is complete before calling the producer or recording commands.
    // AddRef protects the source buffer as well as its producer fence contract.
    Impl::Input retained;retained.fence=fence;retained.resources[0].p=input.values;input.values->AddRef();
    if(!input.prepareRead(list,fence,error))return false;
    if(!begin(h,list,fence,input.sourceTick,error))return false;
    s.inputs.push_back(std::move(retained));auto c=s.constants(*f);c[10]=definition.unitsPerInteger;c[20]=input.offset;c[21]=bits(input.maximum);
    s.bind(list,*f,c);list->SetComputeRootShaderResourceView(1,input.values->GetGPUVirtualAddress());s.dispatch(list,ImportScalar,f->voxels);
    if(!resolve(h,list,fence,error))return false;f->info.inputVersion=input.version;return true;
}
std::shared_ptr<GpuLocalProjection> GpuLocalFields::project(GpuLocalHandle h,Tick tick,ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
    error.clear();auto* f=impl_?impl_->find(h):nullptr;if(!f||!impl_->valid(list,fence)||!f->info.resolved||f->info.accumulating||tick<f->info.sourceTick||(f->epoch&&tick<f->lastPublish)||tick==UINT64_MAX){error="NG-E-FIELD-LOCAL-GPU: invalid projection phase, time or fence";return {};}
    auto& s=*impl_;if(f->lastProjectedVersion==f->info.version&&f->lastPublish==tick)if(auto current=f->lastView.lock()){if(!current->markUsed(fence)){error="NG-E-FIELD-LOCAL-GPU: cached projection fence";return {};}s.touch(*f,fence);return current;}
    s.collect();const auto r=f->cost.coarseRegion;const auto d=size(r);const auto count=volume(d),groups=(count+63)/64;
    const auto plannedPages=f->cost.worldPages,bytes=f->cost.projectionBytes;const auto before=s.stats();
    if(plannedPages>f->spec.maxWorldPages||s.views.size()>=s.limits.maxProjectionViews||bytes>s.limits.maxAllocationBytes||before.allocationBytes>s.limits.maxAllocationBytes-bytes){error="NG-E-FIELD-LOCAL-GPU: projection page, view or allocation budget";return {};}
    const bool newEpoch=f->lastProjectedVersion!=f->info.version||f->lastPublish!=tick;if(newEpoch&&f->epoch==UINT64_MAX){error="NG-E-FIELD-LOCAL-GPU: projection epoch overflow";return {};}
    try{auto view=std::shared_ptr<GpuLocalProjection>(new GpuLocalProjection);view->impl_=std::make_unique<GpuLocalProjection::Impl>();auto& v=*view->impl_;v.timeline.p=s.timeline.p;v.timeline.p->AddRef();v.device.p=s.device.p;v.device.p->AddRef();v.bytes=bytes;v.fence=fence;v.binding.owner=f->spec.owner;v.binding.source=f->spec.source;v.binding.epoch=f->epoch+(newEpoch?1:0);v.binding.localVersion=f->info.version;v.binding.inputVersion=f->info.inputVersion;v.binding.sourceTick=f->info.sourceTick;v.binding.publishTick=tick;v.binding.region=r;v.binding.originFine=f->spec.originFine;v.binding.dimensionsFine=f->spec.dimensions;v.binding.grid=s.grid;v.binding.finePerWorld=f->spec.finePerWorld;v.planes.resize(f->spec.channels.size());v.binding.planes.reserve(f->spec.channels.size());
        makeBuffer(s.device.p,v.diagnostics,GpuLocalDiagnosticWords*4);v.binding.diagnostics=v.diagnostics.p;
        for(unsigned i=0;i<v.planes.size();++i){auto& p=v.planes[i];makeBuffer(s.device.p,p.values,count*4);makeBuffer(s.device.p,p.groups,groups*16);makeBuffer(s.device.p,p.metrics,32);v.binding.planes.push_back({f->spec.channels[i].channel,f->spec.channels[i].reduction,p.values.p,p.metrics.p,s.catalog[f->spec.channels[i].channel]});}
        // All allocations/admission complete before the first projection command.
        transition(list,v.diagnostics.p,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);transition(list,f->buffers[2].p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(v.diagnostics.p,0,f->buffers[2].p,0,GpuLocalDiagnosticWords*4);transition(list,f->buffers[2].p,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(list,v.diagnostics.p,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=0;i<v.planes.size();++i){auto& p=v.planes[i];for(auto* b:{p.values.p,p.groups.p,p.metrics.p})transition(list,b,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);auto c=s.constants(*f);c[9]=i;c[24]=r.min.x;c[25]=r.min.y;c[26]=r.min.z;c[27]=count;c[28]=d.x;c[29]=d.y;c[30]=d.z;c[31]=f->spec.channels[i].reduction==ProjectionReduction::Maximum?1:0;s.bind(list,*f,c);list->SetComputeRootUnorderedAccessView(7,p.values.p->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(8,p.groups.p->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(9,p.metrics.p->GetGPUVirtualAddress());s.dispatch(list,Project,count);s.dispatch(list,Finalize,1);transition(list,p.values.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(list,p.metrics.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        s.views.push_back(view);f->epoch=v.binding.epoch;f->lastProjectedVersion=f->info.version;f->lastPublish=tick;f->lastView=view;++s.counters.projections;s.touch(*f,fence);return view;
    }catch(const std::exception& e){error=e.what();return {};}
}
bool GpuLocalFields::retire(GpuLocalHandle h,std::string& error){error.clear();if(!impl_||!impl_->find(h)){error="NG-E-FIELD-LOCAL-GPU: invalid retirement handle";return false;}auto& s=*impl_;s.collect();if(s.retired.size()>=s.limits.maxInstances){error="NG-E-FIELD-LOCAL-GPU: in-flight retirement budget";return false;}auto& slot=s.slots[h.slot];s.retired.push_back(slot.field);slot.field.reset();slot.generation=nextGeneration(slot.generation);s.collect();return true;}
GpuLocalInfo GpuLocalFields::inspect(GpuLocalHandle h) const{auto* f=impl_?impl_->find(h):nullptr;return f?f->info:GpuLocalInfo{};}
bool GpuLocalFields::markUsed(GpuLocalHandle h,std::uint64_t fence){auto* f=impl_?impl_->find(h):nullptr;if(!f||!f->info.resolved||!fence||fence<impl_->counters.lastFence||fence<=impl_->timeline.p->GetCompletedValue())return false;impl_->touch(*f,fence);return true;}
GpuLocalBuffers GpuLocalFields::buffers(GpuLocalHandle h) const{auto* f=impl_?impl_->find(h):nullptr;return f&&f->info.resolved?GpuLocalBuffers{f->buffers[1].p,f->buffers[2].p,f->voxels,static_cast<unsigned>(f->spec.channels.size())}:GpuLocalBuffers{};}
void GpuLocalFields::collect(){if(impl_)impl_->collect();}
GpuLocalStats GpuLocalFields::stats() const{if(!impl_)return {};impl_->collect();return impl_->stats();}
bool GpuLocalFields::shutdown(std::string& error){error.clear();if(impl_){impl_->collect();if(impl_->timeline.p->GetCompletedValue()<impl_->counters.lastFence){error="NG-E-FIELD-LOCAL-GPU: field work still in flight";return false;}for(const auto& view:impl_->views)if(view.use_count()>1||view->lastFence()>impl_->timeline.p->GetCompletedValue()){error="NG-E-FIELD-LOCAL-GPU: outstanding projection readers";return false;}}impl_.reset();return true;}
}
