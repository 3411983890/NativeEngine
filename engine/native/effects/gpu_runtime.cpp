#include "gpu_runtime.hpp"
#include "../determinism.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace engine::effects {
namespace {
bool finite(Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
// Same integer rounding rule as the CPU reference: intensity multiplies counts, so it
// must land on whole particles without drifting over thousands of ticks.
std::uint64_t scaledCount(std::uint64_t count,float intensity){return static_cast<std::uint64_t>(std::llround(static_cast<double>(count)*static_cast<double>(intensity)));}
bool deposits(const Asset& a){for(const auto& e:a.emitters)for(const auto& m:e.modules)if(m.kind==ModuleKind::FieldDeposit)return true;return false;}
void check(HRESULT value,const char* message){if(FAILED(value))throw Error("NG-E-EFFECT-GPU",message);}
struct Resource {
    ID3D12Resource* p=nullptr;
    Resource()=default;Resource(const Resource&)=delete;Resource& operator=(const Resource&)=delete;
    ~Resource(){if(p)p->Release();}
};
D3D12_RESOURCE_DESC description(std::uint64_t bytes,bool writable=false){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=writable?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;return d;}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.StateBefore=before;b.Transition.StateAfter=after;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;list->ResourceBarrier(1,&b);}
std::string unsupported(const Asset& asset,const fields::GpuVectorView* field,bool deposits){
    if(asset.hasEvents||!asset.rules.empty())return "GPU visual event queue is not integrated";
    bool velocity=false,writing=false;std::set<std::string> written,read;
    for(const auto& e:asset.emitters)for(const auto& m:e.modules){if(m.kind==ModuleKind::VelocityField){velocity=true;read.insert(m.fieldChannel);}if(m.kind==ModuleKind::FieldDeposit){writing=true;written.insert(m.fieldChannel);}}
    if(asset.hasField||velocity||writing){
        if(!asset.hasField||(!velocity&&!writing)||asset.fieldChannels.empty())return "GPU effects require explicit field read/write modules and bindings";
        if(writing&&!deposits)return "GPU field deposits require an explicit writer driver";
        if(velocity){if(!field||asset.fieldLevel!="worldVisible")return "GPU velocity reads require an explicit world field binding";if(!std::isfinite(asset.fieldVoxelMetres)||std::fabs(asset.fieldVoxelMetres-field->cellMetres())>1e-6f*field->cellMetres())return "Effect field voxel size differs from the bound metre grid";}
        for(const auto& name:asset.fieldChannels)if(!written.count(name)&&(!read.count(name)||!field||!field->supports(name)))return "Effect declares field channels not granted by its read/write bindings";
    }
    for(const auto& e:asset.emitters){auto reason=GpuParticlePool::unsupported(e,field,deposits);if(!reason.empty())return reason;
        for(const auto& r:e.renderers)if(r.lit)return "GPU sprite lighting is not integrated";}
    return {};
}
void validate(const std::shared_ptr<const Asset>& a){
    if(!a||a->id.empty()||a->hash.empty()||a->layoutHash.empty()||!a->duration||a->duration>432000||!a->maxAlive||a->maxAlive>1000000||!a->maxSpawnPerTick||a->maxSpawnPerTick>1000000||a->emitters.empty()||a->emitters.size()>32)
        throw Error("NG-E-EFFECT-ASSET","Invalid GPU effect asset");
    const auto fieldError=fieldDepositError(*a);if(!fieldError.empty())throw Error("NG-E-EFFECT-ASSET",fieldError);
    if(a->smoke){const auto error=smoke::effectConfigurationError(*a->smoke,static_cast<unsigned>(a->emitters.size()),a->maxAlive);if(!error.empty())throw Error("NG-E-EFFECT-ASSET",error);}
    std::set<std::string> ids;
    for(const auto& e:a->emitters){if(e.id.empty()||!ids.insert(e.id).second||e.rateMilli>1000000000ull||e.bursts.size()>256||e.renderers.empty())throw Error("NG-E-EFFECT-ASSET","Invalid GPU emitter contract");
        std::uint64_t previous=std::numeric_limits<std::uint64_t>::max();
        for(const auto& b:e.bursts){if(b.tick>=a->duration||!b.count||b.count>1000000||(previous!=std::numeric_limits<std::uint64_t>::max()&&b.tick<=previous))throw Error("NG-E-EFFECT-ASSET","Invalid GPU emitter burst");previous=b.tick;}}
}
}
static_assert(sizeof(GpuBudgetCounters)==32,"GPU budget counter ABI changed");
struct GpuRuntime::Impl {
    struct EmitterState {std::unique_ptr<GpuParticlePool> pool;std::uint64_t phase=0,sequence=0;};
    struct Instance {
        std::shared_ptr<const Asset> asset;
        std::shared_ptr<GpuParticleBudget> budget;
        std::shared_ptr<fields::GpuVectorView> field;
        std::shared_ptr<GpuDepositPermit> depositPermit,smokePermit;
        std::vector<EmitterState> emitters;
        Vec3 origin{};std::uint64_t age=0,serial=0,seed=0,lastFence=0,lastBirthSerial=0;
        bool paused=false,emitting=true;float intensity=1.0f;
        std::uint64_t bytes() const{auto bytes=budget->allocationBytes();for(const auto& e:emitters)bytes+=e.pool->stats().allocationBytes;return bytes;}
    };
    struct Slot {std::uint32_t generation=1;std::unique_ptr<Instance> instance;};
    struct Retired {std::unique_ptr<Instance> instance;bool clearPending=false;};
    struct SampleTag {Handle handle;std::uint64_t birthSerial=0;};
    struct Sample {Resource readback;std::uint64_t fence=0,order=0;bool pending=false;std::vector<SampleTag> tags;};
    ID3D12Device* device=nullptr;GpuRuntimeLimits limits;
    std::shared_ptr<GpuParticleBudget> global;
    std::map<std::string,std::shared_ptr<const Asset>> assets;
    std::vector<Slot> slots;std::vector<Retired> retired;std::array<Sample,3> samples;
    GpuRuntimeStats counters;std::uint64_t serial=0,baseBytes=0,completed=0,readbackSerial=0;std::optional<fields::Tick> lastFieldTick;GpuFieldDepositDriver depositDriver;GpuFieldDepositAdmission admission;GpuSmokeHooks smoke;std::vector<GpuSmokeRetirement> smokeRetirements;
    Impl(ID3D12Device* d,GpuRuntimeLimits l,GpuFieldDepositDriver deposits,GpuFieldDepositAdmission admit,GpuSmokeHooks hooks):limits(l),depositDriver(std::move(deposits)),admission(std::move(admit)),smoke(std::move(hooks)){
        const auto rd=description((l.maxInstances+1ull)*32),gd=description(32,true);
        const auto readBytes=d->GetResourceAllocationInfo(0,1,&rd).SizeInBytes;
        const auto globalBytes=d->GetResourceAllocationInfo(0,1,&gd).SizeInBytes;
        if(globalBytes>l.maxAllocationBytes||readBytes>(l.maxAllocationBytes-globalBytes)/3)throw Error("NG-E-EFFECT-GPU-BUDGET","GPU runtime telemetry exceeds allocation budget");
        baseBytes=globalBytes+3*readBytes;
        global=std::make_shared<GpuParticleBudget>(d,l.maxParticles,l.maxSpawnPerTick);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        for(auto& sample:samples)check(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&sample.readback.p)),"Create GPU effect asynchronous readback");
        device=d;d->AddRef();
    }
    ~Impl(){if(device)device->Release();}
    Instance* find(Handle h) const{return h.slot<slots.size()&&slots[h.slot].generation==h.generation?slots[h.slot].instance.get():nullptr;}
    std::uint64_t bytes() const{auto value=baseBytes;for(const auto& s:slots)if(s.instance)value+=s.instance->bytes();for(const auto& r:retired)value+=r.instance->bytes();return value;}
    void retire(std::size_t slot,bool clear){auto& s=slots[slot];if(!s.instance)return;auto instance=std::move(s.instance);if(instance->asset->smoke)smokeRetirements.push_back({instance->serial,clear});instance->depositPermit.reset();instance->smokePermit.reset();s.generation=s.generation==UINT32_MAX?0:s.generation+1;
        // Unrecorded resources have no ledger contribution or GPU reference.
        if(instance->lastFence)retired.push_back({std::move(instance),clear});}
    bool initialize(Instance& instance,ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error){
        for(auto& e:instance.emitters)if(!e.pool->stats().initialized&&!e.pool->initialize(commands,fence,error))return false;
        instance.lastFence=fence;counters.lastFence=fence;return true;
    }
    bool clearRetired(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error){
        for(auto& r:retired)if(r.clearPending){for(auto& e:r.instance->emitters)if(e.pool->stats().initialized&&!e.pool->clear(commands,fence,error))return false;r.clearPending=false;r.instance->lastFence=fence;counters.lastFence=fence;}return true;
    }
    bool deposit(ID3D12GraphicsCommandList* commands,std::uint64_t fence,bool simulated,std::string& error){
        if(!depositDriver)return true;
        try{GpuDepositFrame frame;frame.commands=commands;frame.fence=fence;frame.tick=counters.tick;frame.simulated=simulated;
            for(std::size_t slot=0;slot<slots.size();++slot)if(const auto* i=slots[slot].instance.get()){
                bool writing=false;for(const auto& e:i->asset->emitters)for(const auto& m:e.modules)writing|=m.kind==ModuleKind::FieldDeposit;
                if(!writing)continue;GpuDepositInstance out;out.instance={static_cast<std::uint32_t>(slot),slots[slot].generation};out.serial=i->serial;out.asset=i->asset;out.origin=i->origin;out.paused=i->paused;for(const auto& e:i->emitters)out.emitters.push_back(e.pool.get());frame.instances.push_back(std::move(out));
            }
            if(!depositDriver(frame,error)){counters.fieldDepositFailed=true;if(error.empty())error="NG-E-EFFECT-GPU-FIELD: field deposit driver rejected recording";return false;}
            if(simulated)++counters.fieldDepositCalls;else ++counters.fieldDepositFlushCalls;return true;
        }catch(const std::exception& e){counters.fieldDepositFailed=true;error=std::string("NG-E-EFFECT-GPU-FIELD: deposit driver failed: ")+e.what();return false;}
    }
    bool recordSmoke(ID3D12GraphicsCommandList* commands,std::uint64_t fence,bool simulated,std::string& error){
        if(!smoke.record)return true;
        try{GpuSmokeFrame frame;frame.commands=commands;frame.fence=fence;frame.tick=counters.tick;frame.simulated=simulated;frame.retired=smokeRetirements;
            for(std::size_t slot=0;slot<slots.size();++slot)if(const auto* i=slots[slot].instance.get();i&&i->asset->smoke){GpuDepositInstance out;out.instance={static_cast<std::uint32_t>(slot),slots[slot].generation};out.serial=i->serial;out.asset=i->asset;out.origin=i->origin;out.paused=i->paused;for(const auto& e:i->emitters)out.emitters.push_back(e.pool.get());frame.instances.push_back(std::move(out));}
            if(!smoke.record(frame,error)){counters.smokeFailed=true;counters.smokeError=error.empty()?"GPU smoke driver rejected recording":error;error=counters.smokeError;return false;}
            smokeRetirements.clear();counters.lastFence=fence;if(simulated)++counters.smokeCalls;else ++counters.smokeFlushCalls;return true;
        }catch(const std::exception& e){counters.smokeFailed=true;counters.smokeError=e.what();error=e.what();return false;}
    }
    void readback(ID3D12GraphicsCommandList* commands,std::uint64_t fence){
        // No active/retired initialized pool means the global ledger is undefined.
        if(!global->lastFence())return;
        auto found=std::find_if(samples.begin(),samples.end(),[](const auto& sample){return !sample.pending;});
        if(found==samples.end()){++counters.skippedReadbacks;return;}
        auto& sample=*found;sample.tags.clear();
        const auto copy=[&](ID3D12Resource* source,std::uint64_t offset){transition(commands,source,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyBufferRegion(sample.readback.p,offset,source,0,32);transition(commands,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);};
        copy(global->buffer(),0);
        for(std::size_t i=0;i<slots.size();++i)if(auto* instance=slots[i].instance.get();instance&&instance->lastFence){sample.tags.push_back({{static_cast<std::uint32_t>(i),slots[i].generation},instance->lastBirthSerial});copy(instance->budget->buffer(),sample.tags.size()*32ull);instance->lastFence=fence;}
        sample.fence=fence;sample.order=++readbackSerial;sample.pending=true;counters.lastFence=fence;
    }
};
GpuRuntime::GpuRuntime(ID3D12Device* device,GpuRuntimeLimits limits,GpuFieldDepositDriver deposits,GpuFieldDepositAdmission admission,GpuSmokeHooks smoke){
    if(!device||!limits.maxParticles||limits.maxParticles>1000000||!limits.maxInstances||limits.maxInstances>4096||!limits.maxSpawnPerTick||limits.maxSpawnPerTick>1000000||!limits.maxAllocationBytes)throw Error("NG-E-EFFECT-GPU-BUDGET","Invalid GPU runtime limits");
    impl_=std::make_unique<Impl>(device,limits,std::move(deposits),std::move(admission),std::move(smoke));
}
GpuRuntime::~GpuRuntime()=default;
void GpuRuntime::install(std::shared_ptr<const Asset> asset){
    if(!impl_)throw Error("NG-E-EFFECT-GPU-STATE","GPU runtime is shut down");validate(asset);auto& s=*impl_;auto found=s.assets.find(asset->id);
    if(found!=s.assets.end()){if(found->second->hash==asset->hash)return;throw Error("NG-E-EFFECT-REVISION","GPU live reload requires a separate migration contract");}
    if(s.assets.size()>=4096)throw Error("NG-E-EFFECT-GPU-BUDGET","GPU asset registry full");s.assets.emplace(asset->id,std::make_shared<const Asset>(*asset));
}
SpawnResult GpuRuntime::spawn(const std::string& id,Vec3 origin,std::uint64_t seed,std::shared_ptr<fields::GpuVectorView> field,SpawnParams params){
    if(!impl_)return {SpawnStatus::Capacity,{},"GPU runtime is shut down"};auto& s=*impl_;auto asset=s.assets.find(id);
    if(asset==s.assets.end())return {SpawnStatus::UnknownAsset,{},"Unknown GPU effect asset"};
    if(!std::isfinite(params.intensity))return {SpawnStatus::InvalidParameter,{},"GPU effect intensity is not finite"};
    if(!finite(origin))return {SpawnStatus::InvalidPosition,{},"GPU effect position is not finite"};
    if(field&&field->device()!=s.device)return {SpawnStatus::UnsupportedFeature,{},"Field binding belongs to another device"};
    if(s.counters.smokeFailed)return {SpawnStatus::UnsupportedFeature,{},"GPU smoke recording failed; drain and recreate this runtime"};
    if(asset->second->smoke&&(!s.smoke.record||!s.smoke.admit))return {SpawnStatus::UnsupportedFeature,{},"Smoke effects require explicit driver and admission hooks"};
    if(asset->second->smoke){std::size_t count=s.smokeRetirements.size();for(const auto& x:s.slots)if(x.instance&&x.instance->asset->smoke)++count;if(count>=s.limits.maxInstances)return {SpawnStatus::Capacity,{},"Smoke lifecycle queue requires a flush"};}
    if(s.counters.fieldDepositFailed)return {SpawnStatus::UnsupportedFeature,{},"GPU field deposit recording failed; recreate this runtime"};
    auto reason=unsupported(*asset->second,field.get(),!!s.depositDriver);if(!reason.empty())return {SpawnStatus::UnsupportedFeature,{},reason};
    std::size_t slot=0;while(slot<s.slots.size()&&(s.slots[slot].instance||s.slots[slot].generation==0))++slot;
    if(slot>=s.limits.maxInstances||s.serial==UINT64_MAX)return {SpawnStatus::Capacity,{},"GPU effect instance capacity exhausted"};
    const auto used=s.bytes();auto budgetDesc=description(32,true);const auto budgetBytes=s.device->GetResourceAllocationInfo(0,1,&budgetDesc).SizeInBytes;
    if(used>s.limits.maxAllocationBytes||budgetBytes>s.limits.maxAllocationBytes-used)return {SpawnStatus::Capacity,{},"GPU effect allocation budget exhausted (including retired resources)"};
    auto instance=std::make_unique<Impl::Instance>();instance->asset=asset->second;instance->origin=origin;instance->seed=seed;instance->serial=s.serial+1;
    instance->intensity=std::clamp(params.intensity,0.0f,8.0f);
    if(s.admission&&deposits(*asset->second)){
        GpuDepositAdmissionResult admitted;try{admitted=s.admission({instance->serial,asset->second,origin});}catch(const std::exception& e){admitted={SpawnStatus::Capacity,e.what(),{}};}
        if(admitted.status!=SpawnStatus::Accepted||!admitted.permit){++s.counters.fieldAdmissionRejected;s.counters.fieldAdmissionError=admitted.reason.empty()?"GPU field admission returned no reservation":admitted.reason;return {admitted.status==SpawnStatus::Capacity?SpawnStatus::Capacity:SpawnStatus::UnsupportedFeature,{},s.counters.fieldAdmissionError};}instance->depositPermit=std::move(admitted.permit);
    }
    if(asset->second->smoke){GpuDepositAdmissionResult admitted;try{admitted=s.smoke.admit({instance->serial,asset->second,origin});}catch(const std::exception& e){admitted={SpawnStatus::Capacity,e.what(),{}};}
        if(admitted.status!=SpawnStatus::Accepted||!admitted.permit){++s.counters.smokeAdmissionRejected;s.counters.smokeError=admitted.reason.empty()?"Smoke admission returned no reservation":admitted.reason;return {admitted.status==SpawnStatus::Capacity?SpawnStatus::Capacity:SpawnStatus::UnsupportedFeature,{},s.counters.smokeError};}instance->smokePermit=std::move(admitted.permit);}
    for(const auto& e:asset->second->emitters)for(const auto& m:e.modules)if(m.kind==ModuleKind::VelocityField)instance->field=field;
    const auto cap=std::min(asset->second->maxAlive,s.limits.maxParticles);
    instance->budget=std::make_shared<GpuParticleBudget>(s.device,cap,asset->second->maxSpawnPerTick);
    std::uint64_t allocated=budgetBytes;
    try{for(const auto& emitter:asset->second->emitters){Impl::EmitterState e;e.pool=std::make_unique<GpuParticlePool>(s.device,emitter,cap,GpuPoolLimits{cap,s.limits.maxAllocationBytes-used-allocated},instance->budget,s.global,field,!!s.depositDriver);allocated+=e.pool->stats().allocationBytes;e.pool->move(origin);instance->emitters.push_back(std::move(e));}}
    catch(const Error& error){if(error.code=="NG-E-EFFECT-GPU-BUDGET")return {SpawnStatus::Capacity,{},error.what()};throw;}
    if(slot==s.slots.size())s.slots.emplace_back();s.slots[slot].instance=std::move(instance);++s.serial;
    return {SpawnStatus::Accepted,{static_cast<std::uint32_t>(slot),s.slots[slot].generation},{}};
}
bool GpuRuntime::valid(Handle h) const{return impl_&&impl_->find(h);}
bool GpuRuntime::bindField(Handle h,std::shared_ptr<fields::GpuVectorView> field,std::string& error){
    error.clear();auto* instance=impl_?impl_->find(h):nullptr;
    if(!instance||!instance->field||!field||field->device()!=impl_->device){error="NG-E-EFFECT-GPU-FIELD: invalid handle, field or device";return false;}
    error=unsupported(*instance->asset,field.get(),!!impl_->depositDriver);if(!error.empty())return false;
    for(auto& e:instance->emitters)if(!e.pool->bindField(field,error))return false;instance->field=std::move(field);return true;
}
bool GpuRuntime::pause(Handle h,bool paused){auto* i=impl_?impl_->find(h):nullptr;if(!i)return false;i->paused=paused;return true;}
bool GpuRuntime::move(Handle h,Vec3 origin){
    auto* i=impl_?impl_->find(h):nullptr;if(!i||!finite(origin))return false;
    if(i->smokePermit){std::string error;bool accepted=false;try{accepted=i->smokePermit->move(origin,error);}catch(const std::exception& e){error=e.what();}if(!accepted){++impl_->counters.smokeMoveRejected;impl_->counters.smokeError=error;return false;}}
    if(i->depositPermit){std::string error;bool accepted=false;try{accepted=i->depositPermit->move(origin,error);}catch(const std::exception& e){error=e.what();}
        if(!accepted){++impl_->counters.fieldMoveRejected;impl_->counters.fieldAdmissionError=error.empty()?"GPU field move admission rejected":std::move(error);return false;}}
    i->origin=origin;for(auto& e:i->emitters)e.pool->move(origin);return true;
}
bool GpuRuntime::stop(Handle h,bool immediate){auto* i=impl_?impl_->find(h):nullptr;if(!i)return false;i->emitting=false;if(immediate)impl_->retire(h.slot,true);return true;}
void GpuRuntime::clear(){if(impl_)for(std::size_t i=0;i<impl_->slots.size();++i)impl_->retire(i,true);}
void GpuRuntime::collect(std::uint64_t completedFence){
    if(!impl_)return;auto& s=*impl_;if(completedFence<s.completed)throw Error("NG-E-EFFECT-GPU-FENCE","Completed fence moved backwards");s.completed=completedFence;
    std::vector<Impl::Sample*> ready;for(auto& sample:s.samples)if(sample.pending&&sample.fence<=completedFence)ready.push_back(&sample);
    std::sort(ready.begin(),ready.end(),[](const auto* a,const auto* b){return a->order<b->order;});
    for(auto* sample:ready){void* mapped=nullptr;D3D12_RANGE range{0,(sample->tags.size()+1)*32};check(sample->readback.p->Map(0,&range,&mapped),"Map completed GPU effect telemetry");auto* bytes=static_cast<const unsigned char*>(mapped);
        std::memcpy(&s.counters.visual,bytes,32);s.counters.telemetryFence=sample->fence;s.counters.telemetryValid=true;
        for(std::size_t n=0;n<sample->tags.size();++n){const auto tag=sample->tags[n];auto* i=s.find(tag.handle);GpuBudgetCounters values;std::memcpy(&values,bytes+(n+1)*32,32);
            if(i&&!i->emitting&&values.alive==0&&tag.birthSerial==i->lastBirthSerial)s.retire(tag.handle.slot,false);}
        D3D12_RANGE none{0,0};sample->readback.p->Unmap(0,&none);sample->pending=false;sample->tags.clear();}
    s.retired.erase(std::remove_if(s.retired.begin(),s.retired.end(),[&](const auto& r){return !r.clearPending&&r.instance->lastFence<=completedFence;}),s.retired.end());
}
bool GpuRuntime::flush(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t completedFence,std::string& error){
    error.clear();if(!impl_||(impl_->counters.fieldDepositFailed||impl_->counters.smokeFailed)||!commands||!fence||fence<=completedFence||fence<impl_->counters.lastFence){error="NG-E-EFFECT-GPU-STATE: invalid flush or failed field writer";return false;}
    collect(completedFence);if(!impl_->clearRetired(commands,fence,error)||!impl_->deposit(commands,fence,false,error)||!impl_->recordSmoke(commands,fence,false,error))return false;impl_->readback(commands,fence);return true;
}
bool GpuRuntime::step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::uint64_t completedFence,std::string& error,std::optional<fields::Tick> fieldTick){
    error.clear();if(!impl_||(impl_->counters.fieldDepositFailed||impl_->counters.smokeFailed)||!commands||!fence||fence<=completedFence||fence<impl_->counters.lastFence||impl_->counters.tick==UINT64_MAX){error="NG-E-EFFECT-GPU-STATE: invalid runtime tick or failed field writer";return false;}
    collect(completedFence);auto& s=*impl_;
    bool hasField=false;
    for(const auto& slot:s.slots)if(const auto* i=slot.instance.get();i&&i->field&&!i->paused){hasField=true;
        if(!fieldTick||i->field->requestedTick()!=*fieldTick||!i->field->canUse(fence)||(s.lastFieldTick&&*fieldTick<*s.lastFieldTick)){error="NG-E-EFFECT-GPU-FIELD: field tick/view/fence mismatch";return false;}}
    struct Request {Impl::Instance* instance;std::size_t emitter;std::uint64_t phase;std::uint32_t count;};std::vector<Request> requests;
    // Validate all integer decisions before recording or mutating this tick.
    for(auto& slot:s.slots)if(auto* i=slot.instance.get();i&&!i->paused){if(i->age==UINT64_MAX){error="NG-E-EFFECT-TIME: GPU instance age overflow";return false;}
        for(std::size_t n=0;n<i->emitters.size();++n){auto& e=i->emitters[n];const auto& def=i->asset->emitters[n];std::uint64_t phase=e.phase,count=0;
            if(i->emitting&&(i->asset->loop||i->age<i->asset->duration)){phase+=scaledCount(def.rateMilli,i->intensity);count=phase/120000;phase%=120000;for(const auto& b:def.bursts)if(b.tick==i->age%i->asset->duration)count+=scaledCount(b.count,i->intensity);}
            if(count>UINT32_MAX||e.sequence>UINT64_MAX-count){error="NG-E-EFFECT-TIME: GPU spawn sequence overflow";return false;}requests.push_back({i,n,phase,static_cast<std::uint32_t>(count)});}}
    if(!s.clearRetired(commands,fence,error))return false;
    for(auto& slot:s.slots)if(slot.instance&&!s.initialize(*slot.instance,commands,fence,error))return false;
    bool globalReset=false;
    for(auto& slot:s.slots)if(auto* i=slot.instance.get();i&&!i->paused){if(!i->emitters.front().pool->prepareBudgets(commands,fence,true,!globalReset,error))return false;globalReset=true;
        for(auto& e:i->emitters)if(!e.pool->simulate(commands,fence,i->age,i->origin,error,fieldTick))return false;}
    for(const auto& r:requests){auto& i=*r.instance;auto& e=i.emitters[r.emitter];const auto seed=determinism::deriveSeed(i.seed^i.asset->seed^determinism::mix64(i.serial),i.asset->emitters[r.emitter].id);
        if(!e.pool->spawn(commands,fence,r.count,e.sequence,seed,error))return false;e.phase=r.phase;e.sequence+=r.count;if(r.count)i.lastBirthSerial=s.counters.tick+1;}
    if(!s.deposit(commands,fence,true,error)||!s.recordSmoke(commands,fence,true,error))return false;
    for(auto& slot:s.slots)if(auto* i=slot.instance.get();i&&!i->paused){++i->age;if(!i->asset->loop&&i->age>=i->asset->duration)i->emitting=false;}
    ++s.counters.tick;if(hasField)s.lastFieldTick=fieldTick;s.readback(commands,fence);return true;
}
bool GpuRuntime::prepareDraw(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error){
    error.clear();if(!impl_||(impl_->counters.fieldDepositFailed||impl_->counters.smokeFailed)||!commands||!fence||fence<=impl_->completed||fence<impl_->counters.lastFence){error="NG-E-EFFECT-GPU-STATE: invalid runtime draw or failed field writer";return false;}
    auto& s=*impl_;if(!s.clearRetired(commands,fence,error))return false;
    for(auto& slot:s.slots)if(slot.instance){if(!s.initialize(*slot.instance,commands,fence,error))return false;for(auto& e:slot.instance->emitters)if(!e.pool->prepareDraw(commands,fence,error))return false;}return true;
}
std::vector<GpuDrawBatch> GpuRuntime::drawBatches() const{
    std::vector<GpuDrawBatch> out;if(!impl_)return out;const auto& s=*impl_;
    for(std::size_t slot=0;slot<s.slots.size();++slot)if(const auto* i=s.slots[slot].instance.get())for(std::size_t n=0;n<i->emitters.size();++n)if(i->emitters[n].pool->stats().drawReady)out.push_back({{static_cast<std::uint32_t>(slot),s.slots[slot].generation},static_cast<std::uint32_t>(n),i->emitters[n].pool->buffers(),i->asset->emitters[n].renderers});return out;
}
GpuRuntimeStats GpuRuntime::stats() const{if(!impl_)return {};auto out=impl_->counters;out.allocationBytes=impl_->bytes();for(const auto& slot:impl_->slots)if(slot.instance)++out.instances;out.retiredInstances=impl_->retired.size();for(const auto& sample:impl_->samples)if(sample.pending)++out.pendingReadbacks;return out;}
bool GpuRuntime::shutdown(std::uint64_t completedFence,std::string& error){error.clear();if(!impl_)return true;if(completedFence<impl_->counters.lastFence){error="NG-E-EFFECT-GPU-FENCE: runtime is still in use";return false;}impl_.reset();return true;}
}
