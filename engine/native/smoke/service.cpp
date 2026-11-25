#include "service.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
namespace engine::smoke {
struct GpuSmokeService::Impl:std::enable_shared_from_this<Impl> {
    struct Entry {
        std::uint64_t serial=0,reserved=0,lastFence=0,tailUntil=0,lastStep=0,previousSerial=0,skippedSteps=0,lastAttemptTick=0;
        effects::Handle handle;std::shared_ptr<const effects::Asset> asset;
        std::unique_ptr<GpuSmokeFireSolver> solver;std::unique_ptr<GpuParticleSmokeSource> sources;
        bool owned=true,started=false,paused=false,tail=false,retiring=false;
        unsigned work=0;
    };
    struct Permit:effects::GpuDepositPermit {
        std::weak_ptr<Impl> owner;std::uint64_t serial=0;
        ~Permit(){if(auto s=owner.lock()){auto it=s->entries.find(serial);if(it!=s->entries.end()){it->second->owned=false;if(!it->second->started){it->second->retiring=true;if(s->lastSerial==serial)s->lastSerial=it->second->previousSerial;}}s->collect();}}
        bool move(Vec3 p,std::string& error) override {error.clear();auto s=owner.lock();if(!s||!s->entries.count(serial)||!s->entries.at(serial)->owned||s->entries.at(serial)->retiring||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||std::abs(p.x)>1000000||std::abs(p.y)>1000000||std::abs(p.z)>1000000){error="NG-E-SMOKE-SERVICE: invalid emitter move";return false;}return true;}
    };
    ID3D12Device* device=nullptr;ID3D12Fence* timeline=nullptr;ServiceLimits limits;ServiceStats counters;
    std::map<std::uint64_t,std::unique_ptr<Entry>> entries;std::uint64_t lastSerial=0,maxObservedSerial=0;bool hasTick=false;
    Impl(ID3D12Device* d,ID3D12Fence* f,ServiceLimits l):limits(l){
        if(!d||!f||!l.maxInstances||l.maxInstances>64||!l.maxDispatchesPerTick||l.maxDispatchesPerTick>1048576||!l.maxAllocationBytes||!l.solver.maxInputSnapshots||l.solver.maxInputSnapshots>64||!l.sources.maxFrames||l.sources.maxFrames>64||l.sources.maxFrames>l.solver.maxInputSnapshots||!l.sources.maxBatches||l.sources.maxBatches>64||!l.sources.maxParticlesPerBatch||l.sources.maxParticlesPerBatch>1000000)throw std::invalid_argument("NG-E-SMOKE-SERVICE: invalid limits/device");
        ID3D12Device* owner=nullptr;const auto code=f->GetDevice(IID_PPV_ARGS(&owner));const bool same=SUCCEEDED(code)&&owner==d;if(owner)owner->Release();if(!same)throw std::invalid_argument("NG-E-SMOKE-SERVICE: timeline device mismatch");device=d;d->AddRef();timeline=f;f->AddRef();
    }
    ~Impl(){if(timeline)timeline->Release();if(device)device->Release();}
    void collect(){const auto done=timeline->GetCompletedValue();for(auto it=entries.begin();it!=entries.end();){auto& e=*it->second;e.solver->collect();e.sources->collect();std::string error;if(e.retiring&&!e.owned&&e.lastFence<=done&&e.solver->shutdown(error)&&e.sources->shutdown(error))it=entries.erase(it);else ++it;}}
    ServiceStats stats() const{auto out=counters;for(const auto& pair:entries){const auto& e=*pair.second;out.reservedBytes+=e.reserved;out.allocationBytes+=e.solver->stats().allocationBytes+e.sources->stats().allocationBytes;out.reservations+=e.owned;out.retiring+=e.retiring;out.tails+=e.tail&&!e.retiring;out.instances+=!e.retiring;}return out;}
    bool valid(ID3D12GraphicsCommandList* list,std::uint64_t fence) const{if(!list||!fence||fence<counters.lastFence||fence<=timeline->GetCompletedValue()||(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return false;ID3D12Device* owner=nullptr;const auto hr=list->GetDevice(IID_PPV_ARGS(&owner));const bool same=SUCCEEDED(hr)&&owner==device;if(owner)owner->Release();return same;}
    effects::GpuDepositAdmissionResult admit(const effects::GpuDepositSpawnRequest& r){
        const auto reject=[&](effects::SpawnStatus status,const std::string& reason){++counters.admissionRejected;return effects::GpuDepositAdmissionResult{status,reason,{}};};
        if(counters.failed||!r.serial||r.serial<=lastSerial||!r.asset||!r.asset->smoke)return reject(effects::SpawnStatus::UnsupportedFeature,"NG-E-SMOKE-SERVICE: failed/invalid/reused smoke admission");
        const auto& c=*r.asset->smoke;const auto error=effectConfigurationError(c,static_cast<unsigned>(r.asset->emitters.size()),r.asset->maxAlive);if(!error.empty())return reject(effects::SpawnStatus::UnsupportedFeature,error);
        try{collect();if(entries.size()>=limits.maxInstances)return reject(effects::SpawnStatus::Capacity,"NG-E-SMOKE-SERVICE: instance budget including tails/retirements");if(c.sources.size()>limits.sources.maxBatches||r.asset->maxAlive>limits.sources.maxParticlesPerBatch)return reject(effects::SpawnStatus::Capacity,"NG-E-SMOKE-SERVICE: source batch/particle capacity");
            auto spec=c.solver;spec.originMetres=r.origin+c.originOffset;const auto cost=gpuFootprint(device,spec);const auto frame=particleSourceFrameBytes(device,spec);const auto sourcePeak=frame*limits.sources.maxFrames;const auto solverPeak=cost.fieldBytes+cost.inputSnapshotBytes;const auto reserve=solverPeak+sourcePeak;
            const auto steps=substeps(spec,1.f/120);const auto work=steps*(spec.pressureIterations+8+unsigned(spec.vorticity>0)+2*unsigned(spec.macCormack))+2;const auto fullWork=work+2+static_cast<unsigned>(c.sources.size());
            if(solverPeak>limits.solver.maxAllocationBytes||sourcePeak>limits.sources.maxAllocationBytes||reserve>limits.maxAllocationBytes||stats().reservedBytes>limits.maxAllocationBytes-reserve||work>limits.solver.maxDispatchesPerStep||fullWork>limits.maxDispatchesPerTick)return reject(effects::SpawnStatus::Capacity,"NG-E-SMOKE-SERVICE: reserved aligned byte/dispatch budget");
            auto e=std::make_unique<Entry>();e->serial=r.serial;e->previousSerial=lastSerial;e->asset=r.asset;e->reserved=reserve;e->work=fullWork;e->solver=std::make_unique<GpuSmokeFireSolver>(device,timeline,spec,limits.solver);e->sources=std::make_unique<GpuParticleSmokeSource>(device,timeline,spec,ParticleSourceQuantization{c.unitsPerSource},limits.sources);std::string why;if(!e->solver->setInputs(std::vector<CellInput>(layout(spec).cells),why))return reject(effects::SpawnStatus::Capacity,why);if(e->solver->stats().allocationBytes!=solverPeak)throw std::runtime_error("NG-E-SMOKE-SERVICE: solver allocation estimate mismatch");
            auto permit=std::make_shared<Permit>();permit->owner=shared_from_this();permit->serial=r.serial;entries.emplace(r.serial,std::move(e));lastSerial=r.serial;maxObservedSerial=std::max(maxObservedSerial,r.serial);return {effects::SpawnStatus::Accepted,{},std::move(permit)};
        }catch(const std::exception& e){return reject(effects::SpawnStatus::Capacity,e.what());}
    }
    bool record(const effects::GpuSmokeFrame& frame,std::string& error){
        error.clear();const auto reject=[&](const char* why){error=std::string("NG-E-SMOKE-SERVICE: ")+why;return false;};
        if(counters.failed||!valid(frame.commands,frame.fence)||frame.tick==UINT64_MAX||(hasTick&&(frame.tick<counters.lastTick||(frame.simulated&&frame.tick==counters.lastTick))))return reject("invalid recorder/list/fence/tick");
        collect();std::map<std::uint64_t,const effects::GpuDepositInstance*> live;std::set<std::uint64_t> retired;
        for(const auto& i:frame.instances){auto it=entries.find(i.serial);if(!live.emplace(i.serial,&i).second||it==entries.end()||!it->second->owned||it->second->retiring||!i.asset||i.asset.get()!=it->second->asset.get()||i.emitters.size()!=i.asset->emitters.size())return reject("live instance differs from admitted asset/lifecycle");for(auto* pool:i.emitters)if(!pool)return reject("null live particle pool");}
        for(const auto& r:frame.retired){if(!r.serial||r.serial>maxObservedSerial||!retired.insert(r.serial).second||live.count(r.serial))return reject("invalid retirement set");const auto it=entries.find(r.serial);if(it!=entries.end()&&it->second->owned)return reject("retirement still owns spawn reservation");if(it!=entries.end()&&!r.immediate&&frame.tick>UINT64_MAX-it->second->asset->smoke->tailTicks)return reject("tail tick overflow");}
        for(const auto& pair:entries)if(pair.second->owned&&!live.count(pair.first))return reject("incomplete live set");
        bool recording=false;
        try{
            for(const auto& r:frame.retired)if(auto it=entries.find(r.serial);it!=entries.end()){auto& e=*it->second;if(r.immediate||!e.started||!e.asset->smoke->tailTicks)e.retiring=true;else{e.tail=true;e.paused=false;e.tailUntil=frame.tick+e.asset->smoke->tailTicks;}}
            for(auto& pair:entries){auto& e=*pair.second;const auto found=live.find(pair.first);if(found!=live.end()){e.handle=found->second->instance;e.paused=found->second->paused;}else if(!e.owned&&!e.tail)e.retiring=true;if(e.tail&&frame.simulated&&frame.tick>=e.tailUntil)e.retiring=true;}
            if(frame.simulated){std::vector<Entry*> candidates;for(auto& pair:entries)if(!pair.second->retiring&&!pair.second->paused)candidates.push_back(pair.second.get());std::stable_sort(candidates.begin(),candidates.end(),[](const auto* a,const auto* b){return a->lastStep!=b->lastStep?a->lastStep<b->lastStep:a->serial<b->serial;});unsigned remaining=limits.maxDispatchesPerTick;
                for(auto* ptr:candidates){auto& e=*ptr;e.lastAttemptTick=frame.tick;const auto src=e.sources->stats();const auto gpu=e.solver->stats();if(e.work>remaining||(!e.tail&&(src.views>=limits.sources.maxFrames||gpu.particleRateSnapshots>=limits.solver.maxInputSnapshots))){++counters.skippedSteps;++e.skippedSteps;continue;}
                    std::vector<ParticleSourceBatch> batches;if(!e.tail){const auto& instance=*live.at(e.serial);for(const auto& source:e.asset->smoke->sources)batches.push_back({instance.emitters.at(source.emitter),source.quantum});}
                    recording=true;e.lastFence=frame.fence;counters.lastFence=frame.fence;std::shared_ptr<GpuSmokeRateView> rates;if(!e.tail){rates=e.sources->produce(batches,frame.commands,frame.fence,frame.tick,error);if(!rates)throw std::runtime_error(error);}if(!e.solver->step(frame.commands,frame.fence,frame.tick,1.f/120,error,std::move(rates)))throw std::runtime_error(error);e.started=true;e.lastStep=frame.tick+1;remaining-=e.work;++counters.solverSteps;counters.tailSteps+=e.tail;
                }++counters.frames;counters.lastTick=frame.tick;hasTick=true;
            }else ++counters.flushes;
            collect();return true;
        }catch(const std::exception& e){if(recording)counters.failed=true;error=e.what();return false;}
    }
};
GpuSmokeService::GpuSmokeService(ID3D12Device* d,ID3D12Fence* f,ServiceLimits l):impl_(std::make_shared<Impl>(d,f,l)){}
GpuSmokeService::~GpuSmokeService()=default;
effects::GpuSmokeHooks GpuSmokeService::hooks(){std::weak_ptr<Impl> weak=impl_;return {[weak](const effects::GpuSmokeFrame& frame,std::string& error){auto s=weak.lock();if(!s){error="NG-E-SMOKE-SERVICE: service is shut down";return false;}return s->record(frame,error);},[weak](const effects::GpuDepositSpawnRequest& r){auto s=weak.lock();return s?s->admit(r):effects::GpuDepositAdmissionResult{effects::SpawnStatus::UnsupportedFeature,"Smoke service is shut down",{}};}};}
effects::GpuDepositAdmissionResult GpuSmokeService::admit(const effects::GpuDepositSpawnRequest& r){return impl_?impl_->admit(r):effects::GpuDepositAdmissionResult{effects::SpawnStatus::UnsupportedFeature,"Smoke service is shut down",{}};}
bool GpuSmokeService::record(const effects::GpuSmokeFrame& frame,std::string& error){if(!impl_){error="NG-E-SMOKE-SERVICE: shut down";return false;}return impl_->record(frame,error);}
std::vector<ServiceOutput> GpuSmokeService::outputs() const{std::vector<ServiceOutput> out;if(impl_)for(const auto& pair:impl_->entries){const auto& e=*pair.second;if(e.started&&!e.retiring)out.push_back({e.handle,e.serial,e.asset->id,e.solver->buffers(),e.paused,e.tail,e.skippedSteps,e.lastAttemptTick,e.asset->smoke->optics});}return out;}
fields::GpuScalarInput GpuSmokeService::densityInput(std::uint64_t serial) const{
    fields::GpuScalarInput out;if(!impl_||impl_->counters.failed)return out;const auto found=impl_->entries.find(serial);
    if(found==impl_->entries.end()||!found->second->started||found->second->retiring)return out;
    const auto b=found->second->solver->buffers();if(!(b.spec.channels&Density))return out;
    out.values=b.scalars;out.timeline=impl_->timeline;out.offset=b.layout.offsets[0];out.dimensions={int(b.spec.dimensions[0]),int(b.spec.dimensions[1]),int(b.spec.dimensions[2])};out.originMetres=b.spec.originMetres;out.cellMetres=b.spec.cellMetres;out.maximum=b.spec.maxDensity;out.sourceTick=b.sourceTick;out.version=b.version;out.lastFence=found->second->lastFence;
    std::weak_ptr<Impl> weak=impl_;out.current=[weak,serial,resource=b.scalars,tick=b.sourceTick,version=b.version]{auto s=weak.lock();if(!s||s->counters.failed)return false;const auto i=s->entries.find(serial);if(i==s->entries.end()||!i->second->started||i->second->retiring)return false;const auto current=i->second->solver->buffers();return current.scalars==resource&&current.sourceTick==tick&&current.version==version;};
    out.prepareRead=[weak,serial,current=out.current](ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){auto s=weak.lock();if(!s||!current()||!s->valid(list,fence)){error="NG-E-SMOKE-SERVICE: stale density input";return false;}auto& e=*s->entries.at(serial);if(!e.solver->prepareRead(list,fence,error))return false;e.lastFence=fence;s->counters.lastFence=fence;return true;};return out;
}
bool GpuSmokeService::prepareRead(std::uint64_t serial,ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){error.clear();if(!impl_||impl_->counters.failed||!impl_->valid(list,fence)||!impl_->entries.count(serial)||impl_->entries.at(serial)->retiring){error="NG-E-SMOKE-SERVICE: invalid output consumer";return false;}auto& e=*impl_->entries.at(serial);if(!e.solver->prepareRead(list,fence,error))return false;e.lastFence=fence;impl_->counters.lastFence=fence;return true;}
void GpuSmokeService::collect(){if(impl_)impl_->collect();}
ServiceStats GpuSmokeService::stats() const{if(!impl_)return {};impl_->collect();return impl_->stats();}
bool GpuSmokeService::shutdown(std::string& error){error.clear();if(!impl_)return true;impl_->collect();if(!impl_->entries.empty()||impl_->timeline->GetCompletedValue()<impl_->counters.lastFence){error="NG-E-SMOKE-SERVICE: live reservations/tails/consumers or actual fence in flight";return false;}impl_.reset();return true;}
}
