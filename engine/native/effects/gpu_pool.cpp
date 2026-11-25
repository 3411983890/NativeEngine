#include "gpu_pool.hpp"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace engine::effects {
namespace {
template<class T>struct Com {
    T* p=nullptr;Com()=default;Com(const Com&)=delete;Com& operator=(const Com&)=delete;
    Com(Com&& other) noexcept:p(other.p){other.p=nullptr;}
    Com& operator=(Com&& other) noexcept {if(this!=&other){reset();p=other.p;other.p=nullptr;}return *this;}
    ~Com(){reset();}void reset(){if(p){p->Release();p=nullptr;}}T** out(){reset();return &p;}
};
void check(HRESULT hr,const char* why) {if(FAILED(hr)){std::ostringstream out;out<<why<<" HRESULT=0x"<<std::hex<<static_cast<unsigned>(hr);throw Error("NG-E-EFFECT-GPU",out.str());}}
bool finite(Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
struct GpuModule {std::uint32_t meta[4]{};float vector[4]{},color[4]{},sizeCoeff[4]{};};
struct GpuKey {float time[4]{},value[4]{};};
static_assert(sizeof(GpuModule)==64&&sizeof(GpuKey)==32,"GPU module ABI changed");
D3D12_RESOURCE_DESC desc(std::uint64_t bytes,bool writable) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=writable?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;return d;
}
Com<ID3D12Resource> buffer(ID3D12Device* device,std::uint64_t bytes,bool writable) {
    Com<ID3D12Resource> out;D3D12_HEAP_PROPERTIES heap{};heap.Type=writable?D3D12_HEAP_TYPE_DEFAULT:D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;const auto d=desc(bytes,writable);
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,writable?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(out.out())),"Create GPU particle buffer");return out;
}
void upload(ID3D12Resource* resource,const void* source,std::size_t bytes) {void* p=nullptr;D3D12_RANGE noRead{0,0};check(resource->Map(0,&noRead,&p),"Map particle definition");std::memcpy(p,source,bytes);D3D12_RANGE written{0,bytes};resource->Unmap(0,&written);}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.StateBefore=before;b.Transition.StateAfter=after;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;list->ResourceBarrier(1,&b);}
void barrier(ID3D12GraphicsCommandList* list) {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&b);}
std::uint32_t bits(float value){std::uint32_t out;std::memcpy(&out,&value,4);return out;}
}
GpuParticleLayout GpuParticleLayout::make(std::uint32_t capacity) {
    if(!capacity||capacity>1000000)throw Error("NG-E-EFFECT-GPU-BUDGET","Invalid particle capacity");
    GpuParticleLayout l;std::uint32_t next=0;
    const auto attribute=[&](unsigned bytes){auto offset=next;next+=(capacity*bytes+15)&~15u;return offset;};
    l.position=attribute(12);l.velocity=attribute(12);l.age=attribute(4);l.lifetime=attribute(4);l.spawnId=attribute(4);l.color=attribute(16);l.size=attribute(8);l.bytes=next;return l;
}
struct GpuParticleBudget::Impl {
    Com<ID3D12Resource> buffer;
    ID3D12Device* device=nullptr; // resource retains its device
    std::uint32_t capacity=0,maxSpawn=0;
    std::uint64_t bytes=0,fence=0;
    bool initialized=false;
};
GpuParticleBudget::GpuParticleBudget(ID3D12Device* device,std::uint32_t capacity,std::uint32_t maxSpawn) {
    if(!device||!capacity||capacity>1000000||!maxSpawn)throw Error("NG-E-EFFECT-GPU-BUDGET","Invalid shared GPU particle budget");
    auto s=std::make_unique<Impl>();s->device=device;s->capacity=capacity;s->maxSpawn=maxSpawn;
    auto description=desc(32,true);s->bytes=device->GetResourceAllocationInfo(0,1,&description).SizeInBytes;
    s->buffer=::engine::effects::buffer(device,32,true);impl_=std::move(s);
}
GpuParticleBudget::~GpuParticleBudget()=default;
ID3D12Resource* GpuParticleBudget::buffer() const{return impl_->buffer.p;}
std::uint64_t GpuParticleBudget::allocationBytes() const{return impl_->bytes;}
std::uint64_t GpuParticleBudget::lastFence() const{return impl_->fence;}
std::uint32_t GpuParticleBudget::capacity() const{return impl_->capacity;}
std::string GpuParticlePool::unsupported(const Emitter& e,const fields::GpuVectorView* field,bool externalFieldDeposits) {
    if(e.shape==Shape::Mesh)return "Mesh emission needs the cooked surface sampler";
    if(static_cast<unsigned>(e.shape)>static_cast<unsigned>(Shape::Line)||e.modules.empty()||e.modules.size()>32||e.modules.front().kind!=ModuleKind::Initialize)return "Invalid GPU emitter module/shape contract";
    if(e.lifeMin==0||e.lifeMax<e.lifeMin||e.lifeMax>432000||!finite(e.position)||!finite(e.extent)||!finite(e.lineEnd)||!std::isfinite(e.radius)||!std::isfinite(e.angle)||e.radius<0||e.angle<0||e.angle>3.141592654f)return "Invalid GPU emitter values";
    unsigned initializers=0;
    for(const auto& module:e.modules) {
        if(module.kind==ModuleKind::FieldCoupling)return "GPU field modules need explicit sparse view bindings";
        if(module.kind==ModuleKind::VelocityField&&(!field||!field->supports(module.fieldChannel)))return "Velocity field module needs a matching read-only vector binding";
        if(module.kind==ModuleKind::FieldDeposit&&(!externalFieldDeposits||!module.fieldQuantum||(module.fieldChannel!="density"&&module.fieldChannel!="moisture")))return "Field deposit module needs an explicit external writer and valid scalar quantum";
        if(static_cast<unsigned>(module.kind)>static_cast<unsigned>(ModuleKind::FieldDeposit)||!finite(module.vector)||!std::isfinite(module.coefficient)||!std::isfinite(module.frequency)||module.coefficient<0||module.coefficient>10000||module.frequency<=0)return "Invalid GPU module values";
        if(module.kind==ModuleKind::Initialize)++initializers;
        for(auto c:module.color)if(!std::isfinite(c)||c<0||c>64)return "Invalid GPU color";
        for(auto s:module.size)if(!std::isfinite(s)||s<0||s>10000)return "Invalid GPU size";
        if(module.color[3]>1)return "Invalid GPU alpha";
        if(module.kind==ModuleKind::ColorCurve||module.kind==ModuleKind::SizeCurve) {
            if(module.keys.size()<2||module.keys.size()>64||module.keys.front().t!=0||module.keys.back().t!=1)return "Invalid GPU curve range";
            float previous=-1;
            for(const auto& key:module.keys) {if(!std::isfinite(key.t)||key.t<=previous||key.t>1)return "Invalid GPU curve ordering";previous=key.t;
                for(unsigned i=0;i<(module.kind==ModuleKind::ColorCurve?4u:2u);++i)if(!std::isfinite(key.value[i])||key.value[i]<0||key.value[i]>(module.kind==ModuleKind::ColorCurve?(i==3?1.f:64.f):10000.f))return "Invalid GPU curve value";}
        }
    }
    if(initializers!=1)return "GPU emitter requires exactly one initialize module";
    for(const auto& renderer:e.renderers)if(renderer.type!="sprite")return "GPU pool layout currently covers sprite attributes only";
    return {};
}
struct GpuParticlePool::Impl {
    Com<ID3D12Device> device;Com<ID3D12RootSignature> root;
    std::array<Com<ID3D12PipelineState>,7> pipelines;
    Com<ID3D12CommandSignature> indirect;
    Com<ID3D12Resource> data,alive[2],dead,counters,args,draw,modules,keys;
    std::shared_ptr<GpuParticleBudget> instanceBudget,globalBudget;
    std::shared_ptr<fields::GpuVectorView> field;
    Emitter emitter;GpuParticleLayout layout;GpuPoolStats stats;std::uint32_t capacity=0,current=0;Vec3 origin{};bool argsIndirect=false,pending=false,externalDeposits=false;
    Impl(ID3D12Device* d,Emitter e,std::uint32_t cap,GpuPoolLimits limits,std::shared_ptr<GpuParticleBudget> ib,std::shared_ptr<GpuParticleBudget> gb,std::shared_ptr<fields::GpuVectorView> f):instanceBudget(std::move(ib)),globalBudget(std::move(gb)),field(std::move(f)),emitter(std::move(e)),layout(GpuParticleLayout::make(cap)),capacity(cap) {
        device.p=d;d->AddRef();
        if(std::none_of(emitter.modules.begin(),emitter.modules.end(),[](const auto& m){return m.kind==ModuleKind::VelocityField;}))field.reset();
        std::vector<GpuModule> packed;std::vector<GpuKey> curveKeys;
        for(const auto& module:emitter.modules) {
            GpuModule m;m.meta[0]=static_cast<std::uint32_t>(module.kind);m.meta[1]=static_cast<std::uint32_t>(curveKeys.size());m.meta[2]=static_cast<std::uint32_t>(module.keys.size());
            if(module.kind==ModuleKind::VelocityField)m.meta[3]=module.fieldChannel=="windVelocity"?1:2;
            m.vector[0]=module.vector.x;m.vector[1]=module.vector.y;m.vector[2]=module.vector.z;
            std::copy(module.color.begin(),module.color.end(),m.color);m.sizeCoeff[0]=module.size[0];m.sizeCoeff[1]=module.size[1];m.sizeCoeff[2]=module.coefficient;m.sizeCoeff[3]=module.frequency;
            for(const auto& key:module.keys){GpuKey k;k.time[0]=key.t;std::copy(key.value.begin(),key.value.end(),k.value);curveKeys.push_back(k);}packed.push_back(m);
        }
        if(curveKeys.empty())curveKeys.emplace_back();
        const std::uint64_t sizes[]={layout.bytes,cap*4ull,cap*4ull,cap*4ull,GpuPoolCounterWords*4,12,16,packed.size()*sizeof(GpuModule),curveKeys.size()*sizeof(GpuKey)};
        for(unsigned i=0;i<9;++i){auto description=desc(sizes[i],i<7);auto bytes=d->GetResourceAllocationInfo(0,1,&description).SizeInBytes;if(bytes>limits.maxAllocationBytes||stats.allocationBytes>limits.maxAllocationBytes-bytes)throw Error("NG-E-EFFECT-GPU-BUDGET","Particle allocations exceed budget");stats.allocationBytes+=bytes;}
        data=buffer(d,sizes[0],true);alive[0]=buffer(d,sizes[1],true);alive[1]=buffer(d,sizes[2],true);dead=buffer(d,sizes[3],true);counters=buffer(d,sizes[4],true);args=buffer(d,sizes[5],true);draw=buffer(d,sizes[6],true);
        modules=buffer(d,sizes[7],false);keys=buffer(d,sizes[8],false);upload(modules.p,packed.data(),sizes[7]);upload(keys.p,curveKeys.data(),sizes[8]);
        D3D12_ROOT_PARAMETER parameters[13]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=40;
        for(unsigned i=0;i<2;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i+1].Descriptor.ShaderRegister=i;}
        for(unsigned i=0;i<9;++i){parameters[i+3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[i+3].Descriptor.ShaderRegister=i;}
        parameters[12].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[12].Descriptor.ShaderRegister=1;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=field?13:12;signature.pParameters=parameters;signature.Flags=field?D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED:D3D12_ROOT_SIGNATURE_FLAG_NONE;
        Com<ID3DBlob> bytes,diagnostics;const auto serialized=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),diagnostics.out());
        if(FAILED(serialized))throw Error("NG-E-EFFECT-GPU",diagnostics.p?std::string(static_cast<const char*>(diagnostics.p->GetBufferPointer()),diagnostics.p->GetBufferSize()):"Serialize particle root failed");
        check(d->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create particle root");
        const nativegpu::shaders::Blob* blobs[]={&nativegpu::shaders::kEffectPoolInit,&nativegpu::shaders::kEffectPoolPrepareSim,&nativegpu::shaders::kEffectPoolSimulate,&nativegpu::shaders::kEffectPoolPrepareSpawn,&nativegpu::shaders::kEffectPoolSpawn,&nativegpu::shaders::kEffectPoolFinish,&nativegpu::shaders::kEffectPoolBudget};
        if(field)blobs[2]=&nativegpu::shaders::kEffectPoolSimulateField;
        for(unsigned i=0;i<7;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.p;pipeline.CS={blobs[i]->data,blobs[i]->size};check(d->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(pipelines[i].out())),"Create GPU particle pipeline");}
        D3D12_INDIRECT_ARGUMENT_DESC dispatch{};dispatch.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12_COMMAND_SIGNATURE_DESC command{};command.ByteStride=12;command.NumArgumentDescs=1;command.pArgumentDescs=&dispatch;
        check(d->CreateCommandSignature(&command,nullptr,IID_PPV_ARGS(indirect.out())),"Create particle dispatch signature");
    }
    std::array<std::uint32_t,40> constants(std::uint32_t requested=0,std::uint64_t age=0,std::uint64_t sequence=0,std::uint64_t seed=0) const {
        std::array<std::uint32_t,40> c{};c[0]=capacity;c[1]=requested;c[2]=static_cast<std::uint32_t>(age);c[3]=static_cast<std::uint32_t>(age>>32);
        c[4]=layout.position;c[5]=layout.velocity;c[6]=layout.age;c[7]=layout.lifetime;c[8]=layout.spawnId;c[9]=layout.color;c[10]=layout.size;c[11]=static_cast<std::uint32_t>(emitter.modules.size());
        c[12]=static_cast<std::uint32_t>(sequence);c[13]=static_cast<std::uint32_t>(sequence>>32);c[14]=static_cast<std::uint32_t>(seed);c[15]=static_cast<std::uint32_t>(seed>>32);
        c[16]=bits(origin.x);c[17]=bits(origin.y);c[18]=bits(origin.z);c[19]=emitter.localSpace;
        c[20]=bits(emitter.position.x);c[21]=bits(emitter.position.y);c[22]=bits(emitter.position.z);c[23]=static_cast<std::uint32_t>(emitter.shape);
        c[24]=bits(emitter.extent.x);c[25]=bits(emitter.extent.y);c[26]=bits(emitter.extent.z);c[27]=bits(emitter.radius);
        c[28]=bits(emitter.lineEnd.x);c[29]=bits(emitter.lineEnd.y);c[30]=bits(emitter.lineEnd.z);c[31]=bits(emitter.angle);c[32]=emitter.lifeMin;c[33]=emitter.lifeMax;c[34]=static_cast<std::uint32_t>(age%1000000);
        if(instanceBudget){c[35]|=1;c[36]=instanceBudget->impl_->capacity;c[37]=instanceBudget->impl_->maxSpawn;}
        if(globalBudget){c[35]|=2;c[38]=globalBudget->impl_->capacity;c[39]=globalBudget->impl_->maxSpawn;}return c;
    }
    void restore(ID3D12GraphicsCommandList* commands) {
        if(stats.drawReady){transition(commands,data.p,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(commands,alive[current].p,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(commands,draw.p,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);stats.drawReady=false;}
        if(argsIndirect){transition(commands,args.p,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);argsIndirect=false;}
    }
    bool fenceValid(std::uint64_t fence) const {return fence&&fence>=stats.lastFence&&(!instanceBudget||fence>=instanceBudget->impl_->fence)&&(!globalBudget||fence>=globalBudget->impl_->fence)&&(!field||field->canUse(fence));}
    void touch(std::uint64_t fence){stats.lastFence=fence;if(instanceBudget)instanceBudget->impl_->fence=fence;if(globalBudget)globalBudget->impl_->fence=fence;if(field)field->markUsed(fence);}
    void bind(ID3D12GraphicsCommandList* commands,const std::array<std::uint32_t,40>& c) {
        if(field){ID3D12DescriptorHeap* heaps[]={field->descriptors()};commands->SetDescriptorHeaps(1,heaps);}
        commands->SetComputeRootSignature(root.p);commands->SetComputeRoot32BitConstants(0,40,c.data(),0);commands->SetComputeRootShaderResourceView(1,modules.p->GetGPUVirtualAddress());commands->SetComputeRootShaderResourceView(2,keys.p->GetGPUVirtualAddress());
        if(field)commands->SetComputeRootConstantBufferView(12,field->constantsAddress());
        ID3D12Resource* buffers[]={data.p,alive[current].p,alive[1-current].p,dead.p,counters.p,args.p,draw.p,instanceBudget?instanceBudget->buffer():counters.p,globalBudget?globalBudget->buffer():counters.p};for(unsigned i=0;i<9;++i)commands->SetComputeRootUnorderedAccessView(i+3,buffers[i]->GetGPUVirtualAddress());
    }
    void dispatchIndirect(ID3D12GraphicsCommandList* commands,unsigned pipeline) {barrier(commands);transition(commands,args.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);argsIndirect=true;commands->SetPipelineState(pipelines[pipeline].p);commands->ExecuteIndirect(indirect.p,1,args.p,0,nullptr,0);barrier(commands);}
};
GpuParticlePool::GpuParticlePool(ID3D12Device* device,Emitter emitter,std::uint32_t capacity,GpuPoolLimits limits,std::shared_ptr<GpuParticleBudget> ib,std::shared_ptr<GpuParticleBudget> gb,std::shared_ptr<fields::GpuVectorView> field,bool externalFieldDeposits) {
    if(!device||!capacity||capacity>limits.maxParticles||limits.maxParticles>1000000||!limits.maxAllocationBytes)throw Error("NG-E-EFFECT-GPU-BUDGET","Invalid GPU pool/device limits");
    if((ib&&ib->impl_->device!=device)||(gb&&gb->impl_->device!=device)||(ib&&ib==gb))throw Error("NG-E-EFFECT-GPU-BUDGET","Shared ledgers require distinct resources on the pool device");
    if(field&&field->device()!=device)throw Error("NG-E-EFFECT-GPU-FIELD","Field view belongs to a different device");
    const auto reason=unsupported(emitter,field.get(),externalFieldDeposits);if(!reason.empty())throw Error("NG-E-EFFECT-GPU-UNSUPPORTED",reason);
    if(!nativegpu::shaders::kDxcAvailable)throw Error("NG-E-EFFECT-GPU-SHADER","GPU particle shaders unavailable");
    impl_=std::make_unique<Impl>(device,std::move(emitter),capacity,limits,std::move(ib),std::move(gb),std::move(field));
    impl_->externalDeposits=externalFieldDeposits;
}
GpuParticlePool::~GpuParticlePool()=default;
bool GpuParticlePool::initialize(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error) {
    error.clear();if(!impl_||!commands||!impl_->fenceValid(fence)||impl_->stats.initialized){error="NG-E-EFFECT-GPU-STATE: invalid initialization";return false;}
    if(!prepareBudgets(commands,fence,false,false,error))return false;
    auto& s=*impl_;s.bind(commands,s.constants());commands->SetPipelineState(s.pipelines[0].p);commands->Dispatch((s.capacity+63)/64,1,1);barrier(commands);s.stats.initialized=true;s.touch(fence);return true;
}
bool GpuParticlePool::prepareBudgets(ID3D12GraphicsCommandList* commands,std::uint64_t fence,bool resetInstance,bool resetGlobal,std::string& error) {
    error.clear();if(!impl_||!commands||!impl_->fenceValid(fence)||impl_->pending){error="NG-E-EFFECT-GPU-STATE: invalid budget preparation";return false;}
    auto& s=*impl_;auto c=s.constants();
    if(s.instanceBudget){if(!s.instanceBudget->impl_->initialized)c[35]|=16;else if(resetInstance)c[35]|=4;}
    if(s.globalBudget){if(!s.globalBudget->impl_->initialized)c[35]|=32;else if(resetGlobal)c[35]|=8;}
    if(c[35]&60){s.bind(commands,c);commands->SetPipelineState(s.pipelines[6].p);commands->Dispatch(1,1,1);barrier(commands);
        if(s.instanceBudget)s.instanceBudget->impl_->initialized=true;if(s.globalBudget)s.globalBudget->impl_->initialized=true;s.touch(fence);}
    return true;
}
bool GpuParticlePool::simulate(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t age,Vec3 origin,std::string& error,std::optional<fields::Tick> fieldTick) {
    error.clear();if(!impl_||!commands||!impl_->stats.initialized||!impl_->fenceValid(fence)||!finite(origin)||impl_->pending){error="NG-E-EFFECT-GPU-STATE: invalid simulation phase";return false;}
    if(impl_->field&&(!fieldTick||impl_->field->requestedTick()!=*fieldTick)){error="NG-E-EFFECT-GPU-FIELD: explicit field tick does not match the immutable query binding";return false;}
    auto& s=*impl_;s.restore(commands);s.origin=origin;s.bind(commands,s.constants(0,age));
    commands->SetPipelineState(s.pipelines[1].p);commands->Dispatch(1,1,1);s.dispatchIndirect(commands,2);s.pending=true;s.touch(fence);++s.stats.simulationIndirects;return true;
}
bool GpuParticlePool::spawn(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint32_t requested,std::uint64_t sequence,std::uint64_t seed,std::string& error) {
    error.clear();if(!impl_||!commands||!impl_->pending||!impl_->fenceValid(fence)||sequence>std::numeric_limits<std::uint64_t>::max()-requested){error="NG-E-EFFECT-GPU-STATE: invalid spawn phase";return false;}
    auto& s=*impl_;transition(commands,s.args.p,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);s.argsIndirect=false;s.bind(commands,s.constants(requested,0,sequence,seed));
    commands->SetPipelineState(s.pipelines[3].p);commands->Dispatch(1,1,1);s.dispatchIndirect(commands,4);
    commands->SetPipelineState(s.pipelines[5].p);commands->Dispatch(1,1,1);barrier(commands);s.current=1-s.current;s.pending=false;
    ++s.stats.recordedTicks;++s.stats.spawnIndirects;s.touch(fence);return true;
}
bool GpuParticlePool::step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t age,std::uint32_t requested,std::uint64_t sequence,std::uint64_t seed,Vec3 origin,std::string& error,std::optional<fields::Tick> fieldTick) {
    error.clear();if(sequence>std::numeric_limits<std::uint64_t>::max()-requested){error="NG-E-EFFECT-GPU-STATE: spawn sequence overflow";return false;}
    return simulate(commands,fence,age,origin,error,fieldTick)&&spawn(commands,fence,requested,sequence,seed,error);
}
bool GpuParticlePool::bindField(std::shared_ptr<fields::GpuVectorView> field,std::string& error){
    error.clear();if(!impl_||impl_->pending){error="NG-E-EFFECT-GPU-FIELD: invalid binding phase";return false;}
    if(!impl_->field)return true;
    if(!field||field->device()!=impl_->device.p){error="NG-E-EFFECT-GPU-FIELD: missing view or mismatched device";return false;}
    error=unsupported(impl_->emitter,field.get(),impl_->externalDeposits);if(!error.empty())return false;impl_->field=std::move(field);return true;
}
bool GpuParticlePool::move(Vec3 origin){if(!impl_||!finite(origin))return false;impl_->origin=origin;return true;}
bool GpuParticlePool::clear(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error) {
    error.clear();if(!impl_||!impl_->stats.initialized||!commands||!impl_->fenceValid(fence)||impl_->pending){error="NG-E-EFFECT-GPU-STATE: invalid clear";return false;}
    auto& s=*impl_;s.restore(commands);auto c=s.constants();c[35]|=64;s.bind(commands,c);commands->SetPipelineState(s.pipelines[0].p);commands->Dispatch((s.capacity+63)/64,1,1);barrier(commands);s.touch(fence);return true;
}
bool GpuParticlePool::prepareDraw(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error) {
    error.clear();if(!impl_||!impl_->stats.initialized||!commands||!impl_->fenceValid(fence)||impl_->pending){error="NG-E-EFFECT-GPU-STATE: invalid draw binding";return false;}
    auto& s=*impl_;if(!s.stats.drawReady){transition(commands,s.data.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(commands,s.alive[s.current].p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(commands,s.draw.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);s.stats.drawReady=true;}s.stats.lastFence=fence;return true;
}
GpuPoolBuffers GpuParticlePool::buffers() const {if(!impl_)return {};const auto& s=*impl_;return {s.data.p,s.alive[s.current].p,s.dead.p,s.counters.p,s.args.p,s.draw.p,s.layout,s.capacity,s.origin,s.emitter.localSpace,s.argsIndirect?D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT:D3D12_RESOURCE_STATE_UNORDERED_ACCESS};}
GpuPoolStats GpuParticlePool::stats() const {return impl_?impl_->stats:GpuPoolStats{};}
bool GpuParticlePool::shutdown(std::uint64_t completedFence,std::string& error) {error.clear();if(!impl_)return true;if(completedFence<impl_->stats.lastFence){error="NG-E-EFFECT-GPU-FENCE: pool is still in use";return false;}impl_.reset();return true;}
}
