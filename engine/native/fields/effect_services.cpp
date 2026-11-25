#include "effect_services.hpp"
#include <map>
#include <stdexcept>

namespace engine::fields {
struct GpuEffectFields::Impl {
    EffectFieldLimits limits;GpuVisualWorld world;smoke::GpuSmokeService solver;
    effects::GpuFieldDeposits deposits;smoke::GpuSmokeSharing sharing;
    std::map<std::pair<unsigned,unsigned>,LeaseRequest> readers;
    SmokeObserver observer;EffectFieldStats counters;
    struct Pending {bool active=false,simulated=false;ID3D12GraphicsCommandList* commands=nullptr;std::uint64_t fence=0;Tick tick=0;} pending;
    static effects::GpuDepositLimits depositConfig(const EffectFieldLimits& l){auto result=l.deposits;result.grid=l.grid;return result;}
    static smoke::SharingLimits sharingConfig(const EffectFieldLimits& l){auto result=l.sharing;result.grid=l.grid;return result;}
    Impl(ID3D12Device* d,ID3D12Fence* f,EffectFieldLimits l):limits(l),world(d,f,GpuEffectFields::catalog(),l.world,l.grid),solver(d,f,l.smoke),deposits(world,{ParticleDensity,ParticleMoisture},depositConfig(l)),sharing(world,solver,SmokeDensity,sharingConfig(l)){}
    bool fail(std::string why,std::string& error){counters.failed=true;error=std::move(why);if(deposits.stats().publicationPrepared){std::string ignored=error;deposits.finishPublication({},ignored);}if(sharing.stats().publicationPrepared){std::string ignored=error;sharing.finishPublication({},ignored);}pending={};return false;}
    bool prepare(const effects::GpuDepositFrame& frame,std::string& error){
        error.clear();if(counters.failed||pending.active)return fail("NG-E-EFFECT-FIELDS: failed or overlapping runtime preparation",error);
        if(!deposits.prepare(frame,error))return fail(error,error);pending={true,frame.simulated,frame.commands,frame.fence,frame.tick};return true;
    }
    bool record(const effects::GpuSmokeFrame& frame,std::string& error){
        error.clear();if(counters.failed||!pending.active||pending.simulated!=frame.simulated||pending.commands!=frame.commands||pending.fence!=frame.fence||pending.tick!=frame.tick)return fail("NG-E-EFFECT-FIELDS: runtime producer phases disagree",error);
        try{
            if(!solver.record(frame,error))return fail(error,error);
            std::vector<smoke::SharingObservation> observations;if(observer)for(const auto& output:solver.outputs()){auto observed=observer(output);observed.serial=output.serial;observations.push_back(observed);}
            if(!sharing.prepare(frame.commands,frame.fence,frame.tick,frame.simulated,observations,error))return fail(error,error);
            counters.lastFence=frame.fence;
            if(!frame.simulated){++counters.flushes;pending={};return true;}
            GpuFieldTransaction transaction(world,{deposits.publicationController(),sharing.publicationController()});if(!transaction.begin(error))return fail(error,error);
            const auto depositStage=deposits.stagePublication(frame.commands,frame.fence,error);GpuCommitResult committed;
            if(depositStage==effects::GpuDepositPublicationStage::Deferred){transaction.rollback();committed={GpuCommitCode::Deferred};}
            else if(depositStage!=effects::GpuDepositPublicationStage::Ready){const auto reason=error;transaction.rollback();return fail(reason,error);}
            else if(sharing.stagePublication(frame.commands,frame.fence,error)!=smoke::SharingPublicationStage::Ready){const auto reason=error;transaction.rollback();return fail(reason,error);}
            else if(!world.stats().leases){transaction.rollback();committed={GpuCommitCode::Committed};}
            else committed=transaction.commit(frame.commands,frame.fence,frame.tick,error);
            const auto reason=error;std::string depositError=reason,smokeError=reason;const bool a=deposits.finishPublication(committed,depositError),b=sharing.finishPublication(committed,smokeError);
            if(!a||!b)return fail(!a?depositError:smokeError,error);
            if(committed.deferred()){++counters.deferredCommits;counters.lastDeferral=reason;}
            else if(!committed.accepted())return fail(reason,error);
            ++counters.steps;counters.lastTick=frame.tick;pending={};error.clear();return true;
        }catch(const std::exception& e){return fail(e.what(),error);}
    }
    void collect(){world.collect();deposits.collect();sharing.collect();solver.collect();}
    EffectFieldStats stats(){collect();auto out=counters;out.world=world.stats();const auto a=deposits.stats();const auto b=sharing.stats();const auto c=solver.stats();out.fieldBytes=a.local.allocationBytes+b.local.allocationBytes+out.world.accountedBytes;out.allocationBytes=out.fieldBytes+c.allocationBytes;out.reservedSmokeBytes=c.reservedBytes;out.readers=readers.size();out.recording=pending.active;return out;}
};
std::vector<Channel> GpuEffectFields::catalog(){auto result=effects::GpuFieldDeposits::defaultCatalog();auto density=smoke::GpuSmokeSharing::catalog().front();density.name="smokeDensity";result.push_back(std::move(density));return result;}
GpuEffectFields::GpuEffectFields(ID3D12Device* device,ID3D12Fence* fence,EffectFieldLimits limits){
    const std::uint64_t partitions[]={limits.world.maxAllocationBytes,limits.deposits.local.maxAllocationBytes,limits.sharing.local.maxAllocationBytes,limits.smoke.maxAllocationBytes};std::uint64_t remaining=limits.maxAllocationBytes;for(auto bytes:partitions){if(bytes>remaining)throw std::invalid_argument("NG-E-EFFECT-FIELDS: partition budgets exceed total");remaining-=bytes;}impl_=std::make_shared<Impl>(device,fence,limits);
}
GpuEffectFields::~GpuEffectFields()=default;
EffectFieldHooks GpuEffectFields::hooks(){
    std::weak_ptr<Impl> weak=impl_;EffectFieldHooks hooks;
    hooks.deposits=[weak](const auto& frame,std::string& error){if(auto s=weak.lock())return s->prepare(frame,error);error="NG-E-EFFECT-FIELDS: coordinator stopped";return false;};
    hooks.admission=[weak](const auto& request){if(auto s=weak.lock();s&&!s->counters.failed&&!s->pending.active)return s->deposits.admit(request);return effects::GpuDepositAdmissionResult{effects::SpawnStatus::UnsupportedFeature,"NG-E-EFFECT-FIELDS: stopped/busy",{}};};
    hooks.smoke.admit=[weak](const auto& request){if(auto s=weak.lock();s&&!s->counters.failed&&!s->pending.active)return s->solver.admit(request);return effects::GpuDepositAdmissionResult{effects::SpawnStatus::UnsupportedFeature,"NG-E-EFFECT-FIELDS: stopped/busy",{}};};
    hooks.smoke.record=[weak](const auto& frame,std::string& error){if(auto s=weak.lock())return s->record(frame,error);error="NG-E-EFFECT-FIELDS: coordinator stopped";return false;};return hooks;
}
bool GpuEffectFields::setSmokeObserver(SmokeObserver observer,std::string& error){error.clear();if(!impl_||impl_->counters.failed||impl_->pending.active){error="NG-E-EFFECT-FIELDS: stopped/failed/recording";return false;}impl_->observer=std::move(observer);return true;}
GpuAcquisition GpuEffectFields::acquire(LeaseRequest request){if(!impl_||impl_->counters.failed||impl_->pending.active||request.writable)return {false,{},"NG-E-EFFECT-FIELDS: live read-only consumer required"};auto result=impl_->world.acquire(request);if(result.accepted)impl_->readers.emplace(std::make_pair(result.lease.slot,result.lease.generation),std::move(request));return result;}
bool GpuEffectFields::release(GpuLease lease,std::string& error){error.clear();if(!impl_||impl_->pending.active||!impl_->readers.count({lease.slot,lease.generation})){error="NG-E-EFFECT-FIELDS: invalid consumer or active recording";return false;}if(!impl_->world.release(lease,error))return false;impl_->readers.erase({lease.slot,lease.generation});return true;}
std::shared_ptr<GpuVisualView> GpuEffectFields::view(GpuLease lease,std::string& error){error.clear();if(!impl_||impl_->counters.failed||impl_->pending.active){error="NG-E-EFFECT-FIELDS: stopped/failed/recording";return {};}const auto reader=impl_->readers.find({lease.slot,lease.generation});if(reader==impl_->readers.end()||impl_->deposits.blocksNewViews(reader->second)||impl_->sharing.blocksNewViews(reader->second)){error="NG-E-EFFECT-FIELDS: unknown consumer or uncommitted source retirement";return {};}return impl_->world.view(lease,error);}
effects::GpuFieldDeposits& GpuEffectFields::deposits(){if(!impl_)throw std::logic_error("NG-E-EFFECT-FIELDS: stopped");return impl_->deposits;}
smoke::GpuSmokeService& GpuEffectFields::solver(){if(!impl_)throw std::logic_error("NG-E-EFFECT-FIELDS: stopped");return impl_->solver;}
smoke::GpuSmokeSharing& GpuEffectFields::sharing(){if(!impl_)throw std::logic_error("NG-E-EFFECT-FIELDS: stopped");return impl_->sharing;}
void GpuEffectFields::collect(){if(impl_)impl_->collect();}
EffectFieldStats GpuEffectFields::stats() const{return impl_?impl_->stats():EffectFieldStats{};}
bool GpuEffectFields::shutdown(std::string& error){
    error.clear();if(!impl_)return true;const auto all=impl_->stats();const auto d=impl_->deposits.stats();const auto m=impl_->sharing.stats();const auto s=impl_->solver.stats();
    if(impl_->pending.active||!impl_->readers.empty()||all.allocationBytes||all.world.leases||all.world.views||d.instances||d.pendingRetirements||d.reservations||m.instances||s.instances||s.retiring||s.reservations||impl_->world.context().timeline->GetCompletedValue()<impl_->counters.lastFence){error="NG-E-EFFECT-FIELDS: active recording, producers, consumers or GPU work";return false;}
    if(!impl_->sharing.shutdown(error)||!impl_->deposits.shutdown(error)||!impl_->solver.shutdown(error)||!impl_->world.shutdown(error))return false;impl_.reset();return true;
}
}
