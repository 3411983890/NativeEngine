#include "sharing.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace engine::smoke {
namespace f=engine::fields;

struct GpuSmokeSharing::Impl {
    struct Entry {
        std::uint64_t serial=0,source=0,published=0;
        f::Tick firstSeen=0,pendingSince=0;bool materialized=false,retiring=false,pending=false;
        f::GpuLocalHandle local;f::GpuSharingHandle sharing;f::LocalSpec spec;
        SharingObservation observation;ServiceOutput output;std::shared_ptr<f::GpuLocalProjection> projection;std::string reason;
    };
    ID3D12Device* device;ID3D12Fence* timeline;GpuSmokeService& producer;SharingLimits limits;
    f::GpuLocalFields local;std::unique_ptr<f::GpuVisualWorld> ownedWorld;f::GpuVisualWorld& world;f::GpuFieldSharing sharing;f::ChannelId densityChannel=0;
    struct Prepared {bool active=false,staged=false,dirty=false;ID3D12GraphicsCommandList* commands=nullptr;std::uint64_t fence=0;f::Tick tick=0;std::vector<std::uint64_t> retire,demote,published;std::map<std::uint64_t,std::pair<f::Tick,std::uint64_t>> live;};
    Prepared prepared;
    std::map<std::uint64_t,Entry> entries;std::map<std::pair<unsigned,unsigned>,f::LeaseRequest> readers;
    std::uint64_t lastSource=0;bool hasTick=false,readerDirty=false;SharingStats counters;
    Impl(ID3D12Device* d,ID3D12Fence* fence,GpuSmokeService& p,SharingLimits l,f::GpuVisualWorld* external=nullptr,f::ChannelId density=0):device(d),timeline(fence),producer(p),limits(l),local(d,fence,external?external->context().catalog:GpuSmokeSharing::catalog(),l.local,l.grid),ownedWorld(external?nullptr:std::make_unique<f::GpuVisualWorld>(d,fence,GpuSmokeSharing::catalog(),l.world,l.grid)),world(external?*external:*ownedWorld),sharing(world,l.maxInstances),densityChannel(density){device->AddRef();timeline->AddRef();}
    ~Impl(){timeline->Release();device->Release();}
    bool valid(ID3D12GraphicsCommandList* list,std::uint64_t fence) const{
        if(!list||!fence||fence<counters.lastFence||fence<=timeline->GetCompletedValue()||(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return false;
        ID3D12Device* owner=nullptr;const auto result=list->GetDevice(IID_PPV_ARGS(&owner));const bool valid=SUCCEEDED(result)&&owner==device;if(owner)owner->Release();return valid;
    }
    bool plan(Entry& e,std::string& error) const{
        const auto& p=e.output.buffers.spec;unsigned ratio=0;
        for(unsigned candidate:{1u,2u,4u,8u,16u})if(std::abs(double(p.cellMetres)-double(limits.grid.worldCellMetres)/candidate)<=p.cellMetres*1e-6){ratio=candidate;break;}
        if(!(p.channels&Density)||!ratio||double(p.maxDensity)*1024>8388607){error="NG-E-SMOKE-SHARING: density/grid/quantization unsupported";return false;}
        e.spec={};e.spec.owner="smoke";e.spec.source=lastSource+1;e.spec.finePerWorld=ratio;e.spec.dimensions={int(p.dimensions[0]),int(p.dimensions[1]),int(p.dimensions[2])};e.spec.channels={{densityChannel,f::ProjectionReduction::VolumeAverage}};e.spec.maxWorldPages=limits.world.maxPages;e.spec.maxAgeTicks=limits.maxAgeTicks;
        const float origin[]={p.originMetres.x,p.originMetres.y,p.originMetres.z},grid[]={limits.grid.originMetres.x,limits.grid.originMetres.y,limits.grid.originMetres.z};int* lattice[]={&e.spec.originFine.x,&e.spec.originFine.y,&e.spec.originFine.z};
        for(unsigned a=0;a<3;++a){const double x=(double(origin[a])-grid[a])/p.cellMetres;if(!std::isfinite(x)||x<-1048575||x>1048575||std::abs(x-std::round(x))>1e-5){error="NG-E-SMOKE-SHARING: unaligned world grid";return false;}*lattice[a]=int(std::round(x));}return true;
    }
    void defer(Entry& e,f::Tick tick,const std::string& reason){if(!e.pending)e.pendingSince=tick;e.pending=true;e.reason=reason;counters.lastDeferral=reason;++counters.deferredCandidates;}
    bool demanded(const Entry& e) const{
        if(e.observation.worldVisible||e.observation.subscribers||e.observation.concentration>=limits.promotion.promoteAt)return true;
        auto copy=e;std::string error;f::GpuLocalFootprint cost;if((e.materialized||plan(copy,error))&&local.estimate(copy.spec,cost,error))return world.subscribers(cost.coarseRegion,{densityChannel})>0;return false;
    }
    bool blocked() const{for(const auto& pair:entries)if(pair.second.retiring&&pair.second.materialized&&sharing.inspect(pair.second.sharing).state==f::LocalState::Shared)return true;return false;}
    void collect(){local.collect();world.collect();}
    SharingStats stats(){collect();auto out=counters;out.local=local.stats();out.world=world.stats();out.sharing=sharing.stats();out.ownsWorld=bool(ownedWorld);out.publicationPrepared=prepared.active;out.publicationStaged=prepared.staged;for(const auto& pair:entries){const auto& b=pair.second.output.buffers;out.retainedSolverPointers+=!!b.scalars+!!b.velocity+!!b.pressure+!!b.diagnostics;}out.allocationBytes=out.local.allocationBytes+(out.ownsWorld?out.world.accountedBytes:0);out.instances=unsigned(entries.size());out.queriesBlocked=blocked();for(const auto& pair:entries){out.pending+=pair.second.pending;out.retiring+=pair.second.retiring;}return out;}
    bool drop(Entry& e,f::Tick tick,std::string& error){if(e.materialized){if(sharing.valid(e.sharing)){const auto r=sharing.retire(e.sharing,tick);if(!r.accepted()){error=r.error;return false;}}if(!local.retire(e.local,error))return false;}e.materialized=false;e.source=e.published=0;e.projection.reset();e.pending=false;e.reason.clear();e.firstSeen=tick;return true;}
};
std::vector<f::Channel> GpuSmokeSharing::catalog(){return {{"density","kg/m^3","smoke",f::Domain::VisualReference,f::Merge::Sum,0,1048576,1024}};}
GpuSmokeSharing::GpuSmokeSharing(ID3D12Device* d,ID3D12Fence* fence,GpuSmokeService& producer,SharingLimits l){
    if(!l.maxInstances||l.maxInstances>64||!l.maxPublicationsPerTick||l.maxPublicationsPerTick>64||l.local.maxInstances<l.maxInstances||!l.maxAgeTicks||l.promotion.demoteAt<0||l.promotion.promoteAt<=l.promotion.demoteAt||!l.promotion.promotionsPerTick||l.local.maxAllocationBytes>l.maxAllocationBytes||l.world.maxAllocationBytes>l.maxAllocationBytes-l.local.maxAllocationBytes)throw std::invalid_argument("NG-E-SMOKE-SHARING: invalid limits");impl_=std::make_unique<Impl>(d,fence,producer,l);
}
GpuSmokeSharing::GpuSmokeSharing(f::GpuVisualWorld& world,GpuSmokeService& producer,f::ChannelId density,SharingLimits l){
    auto context=world.context();l.grid=context.grid;
    if(!context.device||!context.timeline||density>=context.catalog.size()||!l.maxInstances||l.maxInstances>64||!l.maxPublicationsPerTick||l.maxPublicationsPerTick>64||l.local.maxInstances<l.maxInstances||!l.maxAgeTicks||l.promotion.demoteAt<0||l.promotion.promoteAt<=l.promotion.demoteAt||!l.promotion.promotionsPerTick||l.local.maxAllocationBytes>l.maxAllocationBytes)throw std::invalid_argument("NG-E-SMOKE-SHARING: invalid shared-world limits/channel");
    const auto canonical=catalog().front();const auto& channel=context.catalog[density];std::set<std::string> names;for(const auto& c:context.catalog)if(c.name.empty()||!names.insert(c.name).second)throw std::invalid_argument("NG-E-SMOKE-SHARING: unique global channel names required");
    if(channel.owner!=canonical.owner||channel.unit!=canonical.unit||channel.domain!=canonical.domain||channel.merge!=canonical.merge||channel.minimum!=canonical.minimum||channel.maximum!=canonical.maximum||channel.unitsPerInteger!=canonical.unitsPerInteger)throw std::invalid_argument("NG-E-SMOKE-SHARING: channel is not canonical physical smoke density");
    impl_=std::make_unique<Impl>(context.device,context.timeline,producer,l,&world,density);
}
GpuSmokeSharing::~GpuSmokeSharing()=default;
bool GpuSmokeSharing::prepare(ID3D12GraphicsCommandList* list,std::uint64_t fence,f::Tick tick,bool simulated,const std::vector<SharingObservation>& observations,std::string& error){
    error.clear();if(!impl_||impl_->counters.failed){error="NG-E-SMOKE-SHARING: stopped/failed";return false;}auto& s=*impl_;if(s.prepared.active||s.sharing.inGroupTransaction()){error="NG-E-SMOKE-SHARING: finish publication before preparing";return false;}
    if(!s.valid(list,fence)||tick==UINT64_MAX||(s.hasTick&&tick<s.counters.lastTick)){error="NG-E-SMOKE-SHARING: invalid recorder/time/fence";return false;}
    auto outputs=s.producer.outputs();std::map<std::uint64_t,ServiceOutput> live;for(auto& output:outputs){if(output.buffers.sourceTick>tick){error="NG-E-SMOKE-SHARING: future producer tick";return false;}live.emplace(output.serial,std::move(output));}
    std::map<std::uint64_t,SharingObservation> observed;for(const auto& o:observations)if(!live.count(o.serial)||o.concentration<0||!observed.emplace(o.serial,o).second){error="NG-E-SMOKE-SHARING: unknown/duplicate/invalid observation";return false;}
    s.collect();for(auto& pair:s.entries)pair.second.retiring=!live.count(pair.first);
    for(auto& pair:live){auto found=s.entries.find(pair.first);if(found==s.entries.end()){if(s.entries.size()>=s.limits.maxInstances){++s.counters.deferredCandidates;s.counters.lastDeferral="NG-E-SMOKE-SHARING: instance tracking budget";continue;}Impl::Entry e;e.serial=pair.first;e.firstSeen=tick;found=s.entries.emplace(pair.first,std::move(e)).first;}auto& e=found->second;e.output=std::move(pair.second);e.output.buffers.scalars=e.output.buffers.velocity=e.output.buffers.pressure=e.output.buffers.diagnostics=nullptr;e.observation=observed.count(e.serial)?observed.at(e.serial):SharingObservation{};}
    ++s.counters.syncs;if(!simulated||(s.hasTick&&tick==s.counters.lastTick))return true;
    s.hasTick=true;s.counters.lastTick=tick;s.counters.lastFence=fence;
    try{
        // Registrations and local allocation are outside the publication
        // checkpoint. Rollback must never invalidate an Entry's handles.
        for(auto& pair:s.entries){auto& e=pair.second;if(e.materialized||e.retiring||!s.demanded(e)||tick-e.firstSeen<s.limits.promotion.minimumLocalTicks)continue;
            if(!s.plan(e,error)){s.defer(e,tick,error);continue;}f::GpuLocalFootprint cost;if(!s.local.estimate(e.spec,cost,error)){s.defer(e,tick,error);continue;}
            f::GpuVisualProposal proposal;proposal.owner="smoke";proposal.source=e.spec.source;proposal.region=cost.coarseRegion;proposal.channels={s.densityChannel};proposal.maxPages=e.spec.maxWorldPages;const auto budget=s.world.budget(&proposal);if(!budget.fits()){s.defer(e,tick,budget.reason());continue;}
            if(s.lastSource==UINT64_MAX)throw std::runtime_error("NG-E-SMOKE-SHARING: source identity overflow");const auto made=s.sharing.create({"smoke",++s.lastSource,e.spec.maxWorldPages,s.limits.maxAgeTicks},e.firstSeen);if(!made.result.accepted()){s.defer(e,tick,made.result.error);continue;}if(!s.local.create(e.spec,e.local,error)){s.sharing.retire(made.handle,tick);s.defer(e,tick,error);continue;}e.sharing=made.handle;e.source=e.spec.source;e.materialized=true;
        }
        s.prepared.active=true;s.prepared.commands=list;s.prepared.fence=fence;s.prepared.tick=tick;for(const auto& pair:live)s.prepared.live.emplace(pair.first,std::make_pair(pair.second.buffers.sourceTick,pair.second.buffers.version));return true;
    }catch(const std::exception& e){s.counters.failed=true;error=e.what();return false;}
}
SharingPublicationStage GpuSmokeSharing::stagePublication(ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
    error.clear();if(!impl_||impl_->counters.failed||!impl_->prepared.active||impl_->prepared.staged||!impl_->sharing.inGroupTransaction()||list!=impl_->prepared.commands||fence!=impl_->prepared.fence){error="NG-E-SMOKE-SHARING: invalid publication scope/recorder/fence";return SharingPublicationStage::Invalid;}auto& s=*impl_;auto& batch=s.prepared;const auto tick=batch.tick;
    std::map<std::uint64_t,std::pair<f::Tick,std::uint64_t>> live;for(const auto& output:s.producer.outputs())live.emplace(output.serial,std::make_pair(output.buffers.sourceTick,output.buffers.version));if(live!=batch.live){error="NG-E-SMOKE-SHARING: producer changed after preparation";return SharingPublicationStage::Invalid;}
    batch.staged=true;auto& dirty=batch.dirty;dirty=s.readerDirty;auto& retire=batch.retire;auto& demote=batch.demote;auto& published=batch.published;
    try{
        for(auto& pair:s.entries){auto& e=pair.second;if(!e.materialized){if(e.retiring)retire.push_back(e.serial);continue;}const auto info=s.sharing.inspect(e.sharing);
            if(e.retiring){const auto r=s.sharing.retire(e.sharing,tick);if(!r.accepted())throw std::runtime_error(r.error);dirty|=info.state==f::LocalState::Shared;retire.push_back(e.serial);}
            else if(info.state==f::LocalState::Shared&&!s.demanded(e)&&e.observation.concentration<=s.limits.promotion.demoteAt&&tick-info.stateSince>=s.limits.promotion.minimumSharedTicks){const auto r=s.sharing.demote(e.sharing,tick);if(!r.accepted())throw std::runtime_error(r.error);dirty=true;demote.push_back(e.serial);}
        }
        std::vector<Impl::Entry*> candidates;for(auto& pair:s.entries){auto& e=pair.second;if(e.retiring||std::find(demote.begin(),demote.end(),e.serial)!=demote.end())continue;const auto info=e.materialized?s.sharing.inspect(e.sharing):f::GpuSharingInfo{};const bool active=e.materialized&&info.state==f::LocalState::Shared;
            if((active||s.demanded(e))&&(active||tick-e.firstSeen>=s.limits.promotion.minimumLocalTicks)&&(!active||e.published!=e.output.buffers.version)){if(!e.pending){e.pending=true;e.pendingSince=tick;}candidates.push_back(&e);}
            else if(!active){e.pending=false;e.reason.clear();}
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto* a,const auto* b){if(a->observation.priority!=b->observation.priority)return a->observation.priority>b->observation.priority;if(a->pendingSince!=b->pendingSince)return a->pendingSince<b->pendingSince;return a->serial<b->serial;});unsigned used=0,promoted=0;
        for(auto* pointer:candidates){auto& e=*pointer;const bool active=e.materialized&&s.sharing.inspect(e.sharing).state==f::LocalState::Shared;
            if(used>=s.limits.maxPublicationsPerTick||(!active&&promoted>=s.limits.promotion.promotionsPerTick)){s.defer(e,tick,"NG-E-SMOKE-SHARING: publication quota");continue;}
            if(!e.materialized)continue;
            f::GpuLocalFootprint cost;if(!s.local.estimate(e.spec,cost,error)){s.defer(e,tick,error);continue;}f::GpuVisualProposal proposal;proposal.owner="smoke";proposal.source=e.materialized?e.source:e.spec.source;proposal.region=cost.coarseRegion;proposal.channels={s.densityChannel};proposal.maxPages=e.spec.maxWorldPages;
            const auto budget=s.world.budget(&proposal);if(!budget.fits()){s.defer(e,tick,budget.reason());continue;}
            const auto local=s.local.inspect(e.local);if(local.inputVersion!=e.output.buffers.version){if(!s.local.replaceScalar(e.local,s.producer.densityInput(e.serial),list,fence,error)){s.defer(e,tick,error);continue;}++s.counters.imports;e.projection.reset();}
            if(!e.projection){e.projection=s.local.project(e.local,tick,list,fence,error);if(!e.projection){s.defer(e,tick,error);continue;}}
            const auto r=active?s.sharing.project(e.sharing,e.projection,tick):s.sharing.promote(e.sharing,e.projection,tick);if(!r.accepted()){s.defer(e,tick,r.error);continue;}dirty=true;++used;promoted+=!active;published.push_back(e.serial);
        }
        return SharingPublicationStage::Ready;
    }catch(const std::exception& e){s.counters.failed=true;error=e.what();return SharingPublicationStage::Invalid;}
}
bool GpuSmokeSharing::finishPublication(f::GpuCommitResult committed,std::string& error){
    if(!impl_||!impl_->prepared.active||impl_->sharing.inGroupTransaction()){error="NG-E-SMOKE-SHARING: publication scope has not finished";return false;}auto& s=*impl_;auto& batch=s.prepared;const auto tick=batch.tick;const auto& published=batch.published;const auto& retire=batch.retire;const auto& demote=batch.demote;
    const auto fail=[&](const std::string& why){s.counters.failed=true;error=why;s.prepared={};return false;};
    if(!committed.accepted()){if(!committed.deferred())return fail(error.empty()?"NG-E-SMOKE-SHARING: joint publication failed":error);if(s.sharing.stats().pending)return fail("NG-E-SMOKE-SHARING: rollback must precede completion");++s.counters.deferredCommits;s.counters.lastDeferral=error;for(auto serial:published)s.defer(s.entries.at(serial),tick,error);s.prepared={};error.clear();return true;}
    if(!batch.staged)return fail("NG-E-SMOKE-SHARING: publication not staged");if(batch.dirty&&s.world.sourceView("",0).snapshotTick!=tick)return fail("NG-E-SMOKE-SHARING: committed tick differs from prepared tick");
    try{
        for(auto& pair:s.entries)if(pair.second.materialized&&s.sharing.valid(pair.second.sharing)){const auto r=s.sharing.settle(pair.second.sharing);if(!r.accepted())throw std::runtime_error("NG-E-SMOKE-SHARING: commit acknowledgment mismatch");}
        for(auto serial:published){auto& e=s.entries.at(serial);e.published=e.output.buffers.version;e.pending=false;e.reason.clear();++s.counters.publications;}
        for(auto serial:demote)if(!s.drop(s.entries.at(serial),tick,error))throw std::runtime_error(error);
        for(auto serial:retire){auto& e=s.entries.at(serial);if(!s.drop(e,tick,error))throw std::runtime_error(error);s.entries.erase(serial);}
        s.readerDirty=false;s.prepared={};s.collect();error.clear();return true;
    }catch(const std::exception& e){return fail(e.what());}
}
f::GpuFieldSharing* GpuSmokeSharing::publicationController(){return impl_?&impl_->sharing:nullptr;}
bool GpuSmokeSharing::blocksNewViews(const f::LeaseRequest& request) const{
    if(!impl_)return true;if(std::find(request.channels.begin(),request.channels.end(),impl_->densityChannel)==request.channels.end())return false;f::Region bounds;if(!request.storageRegion(bounds))return true;
    for(const auto& pair:impl_->entries){const auto& entry=pair.second;if(!entry.retiring||!entry.materialized)continue;const auto state=impl_->sharing.inspect(entry.sharing).state;if(state!=f::LocalState::Shared&&state!=f::LocalState::DemotePending)continue;f::GpuLocalFootprint cost;std::string error;if(!impl_->local.estimate(entry.spec,cost,error))return true;const auto r=cost.coarseRegion;if(bounds.min.x<r.max.x&&r.min.x<bounds.max.x&&bounds.min.y<r.max.y&&r.min.y<bounds.max.y&&bounds.min.z<r.max.z&&r.min.z<bounds.max.z)return true;}return false;
}
bool GpuSmokeSharing::sync(ID3D12GraphicsCommandList* list,std::uint64_t fence,f::Tick tick,const std::vector<SharingObservation>& observations,std::string& error){
    if(!impl_||!impl_->ownedWorld){error="NG-E-SMOKE-SHARING: shared world requires phased publication";return false;}
    if(!prepare(list,fence,tick,true,observations,error))return false;auto& s=*impl_;if(!s.prepared.active)return true;f::GpuFieldTransaction transaction(s.world,{&s.sharing});if(!transaction.begin(error))return finishPublication({},error);
    if(stagePublication(list,fence,error)!=SharingPublicationStage::Ready){transaction.rollback();return finishPublication({},error);}
    if(!s.prepared.dirty){transaction.rollback();return finishPublication({f::GpuCommitCode::Committed},error);}
    return finishPublication(transaction.commit(list,fence,tick,error),error);
}

f::GpuAcquisition GpuSmokeSharing::acquire(f::LeaseRequest request){if(!impl_||impl_->counters.failed||request.writable)return {false,{},"NG-E-SMOKE-SHARING: read-only consumer required"};auto result=impl_->world.acquire(request);if(result.accepted){impl_->readers.emplace(std::make_pair(result.lease.slot,result.lease.generation),std::move(request));impl_->readerDirty=true;}return result;}
bool GpuSmokeSharing::release(f::GpuLease lease,std::string& error){error.clear();if(!impl_||!impl_->readers.count({lease.slot,lease.generation})){error="NG-E-SMOKE-SHARING: invalid consumer";return false;}if(!impl_->world.release(lease,error))return false;impl_->readers.erase({lease.slot,lease.generation});impl_->readerDirty=true;return true;}
std::shared_ptr<f::GpuVisualView> GpuSmokeSharing::view(f::GpuLease lease,std::string& error){error.clear();if(!impl_||impl_->counters.failed||impl_->blocked()||!impl_->readers.count({lease.slot,lease.generation})){error="NG-E-SMOKE-SHARING: unavailable/pending retirement";return {};}return impl_->world.view(lease,error);}
SharingSource GpuSmokeSharing::inspect(std::uint64_t serial) const{SharingSource out;if(!impl_)return out;const auto found=impl_->entries.find(serial);if(found==impl_->entries.end())return out;const auto& e=found->second;out.exists=true;out.serial=serial;out.materialized=e.materialized;out.retiring=e.retiring;out.pending=e.pending;out.source=e.source;out.inputVersion=e.output.buffers.version;out.publishedInputVersion=e.published;out.sourceTick=e.output.buffers.sourceTick;out.pendingSince=e.pendingSince;out.reason=e.reason;if(e.materialized)out.sharing=impl_->sharing.inspect(e.sharing);return out;}
void GpuSmokeSharing::collect(){if(impl_)impl_->collect();}
std::vector<SharedDensityMedium> GpuSmokeSharing::media() const{std::vector<SharedDensityMedium> out;if(!impl_||impl_->counters.failed)return out;for(const auto& pair:impl_->entries){const auto& e=pair.second;if(!e.materialized||e.retiring||!e.projection||e.published!=e.output.buffers.version||e.projection->bindings().inputVersion!=e.published||impl_->sharing.inspect(e.sharing).state!=f::LocalState::Shared)continue;out.push_back({e.serial,e.published,e.output.buffers.sourceTick,e.projection});}return out;}
SharingStats GpuSmokeSharing::stats() const{return impl_?impl_->stats():SharingStats{};}
bool GpuSmokeSharing::shutdown(std::string& error){error.clear();if(!impl_)return true;if(impl_->prepared.active||!impl_->entries.empty()||!impl_->readers.empty()||impl_->timeline->GetCompletedValue()<impl_->counters.lastFence){error="NG-E-SMOKE-SHARING: live producers/readers/retirement/fence";return false;}if((impl_->ownedWorld&&!impl_->world.shutdown(error))||!impl_->local.shutdown(error))return false;impl_.reset();return true;}
}
