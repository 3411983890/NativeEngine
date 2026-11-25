#include "field_deposits.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace engine::effects {
namespace f=fields;
namespace {
bool equal(Handle a,Handle b){return a.slot==b.slot&&a.generation==b.generation;}
bool same(const f::LocalSpec& a,const f::LocalSpec& b){return a.originFine==b.originFine&&a.dimensions==b.dimensions;}
int floorDivide(int value,unsigned scale){const auto s=static_cast<int>(scale);return value/s-(value%s<0);}
f::Region coverage(const f::LocalSpec& s){return {{floorDivide(s.originFine.x,s.finePerWorld),floorDivide(s.originFine.y,s.finePerWorld),floorDivide(s.originFine.z,s.finePerWorld)},{floorDivide(s.originFine.x+s.dimensions.x-1,s.finePerWorld)+1,floorDivide(s.originFine.y+s.dimensions.y-1,s.finePerWorld)+1,floorDivide(s.originFine.z+s.dimensions.z-1,s.finePerWorld)+1}};}
bool finite(Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}

}
struct GpuFieldDeposits::Impl:std::enable_shared_from_this<GpuFieldDeposits::Impl> {
    struct Generation {f::GpuLocalHandle local;f::GpuSharingHandle sharing;f::LocalSpec spec;f::GpuLocalFootprint cost;bool ready=false;std::uint64_t replacementFor=0;};
    struct Binding {Handle handle;std::string assetHash;GpuDepositObservation observation;std::unique_ptr<Generation> current;bool replaceShared=false;GpuDepositPublicationInfo publication;};
    struct Reservation {std::shared_ptr<const Asset> asset;Vec3 origin;f::LocalSpec spec;f::GpuLocalFootprint cost;bool projection=false;};
    struct Headroom {std::uint64_t bytes=0,samples=0;std::size_t slots=0,projections=0;};
    struct Permit:GpuDepositPermit {
        std::weak_ptr<Impl> owner;std::uint64_t serial;
        Permit(std::weak_ptr<Impl> o,std::uint64_t s):owner(std::move(o)),serial(s){}
        ~Permit() override{if(auto o=owner.lock())o->reservations.erase(serial);}
        bool move(Vec3 origin,std::string& error) override{if(auto o=owner.lock())return o->reserveMove(serial,origin,error);error="NG-E-EFFECT-DEPOSIT: reservation owner stopped";return false;}
    };
    struct Read {f::GpuLease lease;f::LeaseRequest request;};
    ID3D12Device* device;ID3D12Fence* timeline;std::vector<f::Channel> catalog;GpuDepositLimits limits;
    f::GpuLocalFields local;std::unique_ptr<f::GpuVisualWorld> ownedWorld;f::GpuVisualWorld& world;f::GpuFieldSharing sharing;
    std::set<f::ChannelId> writableChannels;
    struct Prepared {bool active=false,staged=false;GpuDepositFrame frame;std::set<std::uint64_t> written;std::vector<std::pair<std::uint64_t,bool>> selected;};
    Prepared prepared;
    std::map<std::uint64_t,Binding> bindings;std::vector<std::unique_ptr<Generation>> retired;std::vector<Read> readers;
    std::map<std::uint64_t,Reservation> reservations;
    GpuDepositStats counters;std::uint64_t lastSource=0;f::Tick seenTick=0,simulatedTick=0;bool hasSeen=false,hasSimulated=false;
    Impl(ID3D12Device* d,ID3D12Fence* t,std::vector<f::Channel> c,GpuDepositLimits l,f::GpuVisualWorld* external=nullptr,std::vector<f::ChannelId> writable={}):device(d),timeline(t),catalog(std::move(c)),limits(l),local(d,t,catalog,l.local,l.grid),ownedWorld(external?nullptr:std::make_unique<f::GpuVisualWorld>(d,t,catalog,l.world,l.grid)),world(external?*external:*ownedWorld),sharing(world,l.maxInstances*2){
        if(external)writableChannels.insert(writable.begin(),writable.end());else for(unsigned i=0;i<catalog.size();++i)writableChannels.insert(i);
        d->AddRef();t->AddRef();retired.reserve(l.maxInstances);readers.reserve(l.world.maxLeases);
    }
    ~Impl(){timeline->Release();device->Release();}
    Binding* find(Handle h){for(auto& pair:bindings)if(equal(pair.second.handle,h))return &pair.second;return nullptr;}
    const Binding* find(Handle h) const{return const_cast<Impl*>(this)->find(h);}
    const Generation* materialized(std::uint64_t serial) const{const auto found=bindings.find(serial);return found==bindings.end()?nullptr:found->second.current.get();}
    Headroom missing(const Reservation& r,const Generation* g) const{Headroom out;out.bytes=r.cost.mutableBytes-std::min(r.cost.mutableBytes,g?g->cost.mutableBytes:0);out.samples=r.cost.samples-std::min(r.cost.samples,g?g->cost.samples:0);out.slots=g?0:1;if(r.projection){out.bytes+=r.cost.projectionBytes;out.projections=1;}return out;}
    Headroom headroom(std::uint64_t serial=0,const Reservation* replacement=nullptr) const{Headroom total;for(const auto& pair:reservations){const auto h=missing(pair.first==serial&&replacement?*replacement:pair.second,materialized(pair.first));total.bytes+=h.bytes;total.samples+=h.samples;total.slots+=h.slots;total.projections+=h.projections;}if(replacement&&!reservations.count(serial)){const auto h=missing(*replacement,materialized(serial));total.bytes+=h.bytes;total.samples+=h.samples;total.slots+=h.slots;total.projections+=h.projections;}return total;}
    bool fits(std::uint64_t serial,const Reservation& requested,bool replacement,std::string& error) const{
        const auto before=local.stats();const auto h=headroom(serial,&requested);const auto own=missing(requested,materialized(serial));auto bytes=before.allocationBytes+h.bytes,samples=before.localSamples+h.samples;auto slots=before.instances+h.slots;
        if(replacement){bytes+=requested.cost.mutableBytes-(own.bytes-(requested.projection?requested.cost.projectionBytes:0));samples+=requested.cost.samples-own.samples;slots+=1-own.slots;}
        if(bytes>limits.local.maxAllocationBytes||samples>limits.local.maxLocalSamples||slots>limits.local.maxInstances||before.projectionViews+h.projections>limits.local.maxProjectionViews||(replacement&&before.retiredInstances>=limits.local.maxInstances)){error="NG-E-EFFECT-DEPOSIT: reserved aligned private-field capacity exceeded";return false;}return true;
    }
    bool reserveMove(std::uint64_t serial,Vec3 origin,std::string& error){
        auto found=reservations.find(serial);if(found==reservations.end()||!finite(origin)||counters.failed){error="NG-E-EFFECT-DEPOSIT: invalid move reservation";return false;}auto next=found->second;Generation previous;previous.spec=next.spec;if(!plan(*next.asset,origin,&previous,next.spec,error)||!local.estimate(next.spec,next.cost,error))return false;next.origin=origin;const auto* current=materialized(serial);const bool changed=current&&!same(current->spec,next.spec);const auto binding=bindings.find(serial);next.projection=(changed&&sharing.inspect(current->sharing).state==f::LocalState::Shared)||(binding!=bindings.end()&&binding->second.replaceShared);if(!fits(serial,next,changed,error))return false;found->second=std::move(next);return true;
    }
    bool canMaterialize(std::uint64_t serial,const f::GpuLocalFootprint& cost) const{const auto before=local.stats();auto h=headroom();const auto r=reservations.find(serial);if(r!=reservations.end()){const auto own=missing(r->second,materialized(serial));h.bytes-=own.bytes-(r->second.projection?r->second.cost.projectionBytes:0);h.samples-=own.samples;h.slots-=own.slots;}return before.allocationBytes+h.bytes+cost.mutableBytes<=limits.local.maxAllocationBytes&&before.localSamples+h.samples+cost.samples<=limits.local.maxLocalSamples&&before.instances+h.slots+1<=limits.local.maxInstances&&before.retiredInstances<limits.local.maxInstances;}
    bool canProject(std::uint64_t serial,const Generation& source) const{const auto before=local.stats();auto h=headroom();const auto r=reservations.find(serial);if(r!=reservations.end()&&r->second.projection){h.bytes-=r->second.cost.projectionBytes;--h.projections;}return source.cost.worldPages<=limits.maxPagesPerSource&&before.allocationBytes+h.bytes+source.cost.projectionBytes<=limits.local.maxAllocationBytes&&before.projectionViews+h.projections<limits.local.maxProjectionViews;}
    bool plan(const Asset& a,Vec3 origin,const Generation* previous,f::LocalSpec& spec,std::string& error) const{
        spec={};
        const auto invalid=[&](const char* why){error=std::string("NG-E-EFFECT-DEPOSIT: ")+why;return false;};
        const auto fieldError=fieldDepositError(a);if(!fieldError.empty()){error=fieldError;return false;}
        const double ratio=double(limits.grid.worldCellMetres)/a.fieldVoxelMetres;const auto rounded=std::llround(ratio);
        if(!std::isfinite(ratio)||rounded<1||rounded>16||(rounded&(rounded-1))||std::fabs(ratio-rounded)>1e-5*rounded)return invalid("fine/world grid ratio must be 1/2/4/8/16");
        spec.finePerWorld=static_cast<unsigned>(rounded);spec.maxWorldPages=limits.maxPagesPerSource;
        bool allLocal=true;std::set<f::ChannelId> channels;
        for(const auto& e:a.emitters){allLocal&=e.localSpace;for(const auto& m:e.modules)if(m.kind==ModuleKind::FieldDeposit){auto found=std::find_if(catalog.begin(),catalog.end(),[&](const auto& c){return c.name==m.fieldChannel;});if(found==catalog.end()||!writableChannels.count(static_cast<f::ChannelId>(found-catalog.begin())))return invalid("deposit channel not granted to this producer");if(spec.owner.empty())spec.owner=found->owner;else if(spec.owner!=found->owner)return invalid("one source cannot span different channel owners");const auto id=static_cast<f::ChannelId>(found-catalog.begin());if(channels.insert(id).second)spec.channels.push_back({id,found->merge==f::Merge::Sum?f::ProjectionReduction::VolumeAverage:f::ProjectionReduction::Maximum});}}
        if(spec.channels.empty())return invalid("source has no fieldDeposit modules");std::sort(spec.channels.begin(),spec.channels.end(),[](const auto& a,const auto& b){return a.channel<b.channel;});
        const double cell=double(limits.grid.worldCellMetres)/spec.finePerWorld;const float at[]={origin.x,origin.y,origin.z},minimum[]={a.boundsMin.x,a.boundsMin.y,a.boundsMin.z},grid[]={limits.grid.originMetres.x,limits.grid.originMetres.y,limits.grid.originMetres.z};
        int lo[3],hi[3];for(unsigned axis=0;axis<3;++axis){const double start=(double(at[axis])+minimum[axis]-grid[axis])/cell;const double lower=std::floor(start),upper=std::ceil(start+a.fieldDimensions[axis]);if(!std::isfinite(start)||lower<-1048576||upper>1048576)return invalid("deposit lattice outside supported coordinate range");lo[axis]=static_cast<int>(lower);hi[axis]=static_cast<int>(upper);}
        // Particle snapshots can be rebuilt: local-space follows the instance;
        // world/mixed-space retains the union of prior authored sweep bounds.
        if(previous&&!allLocal){const auto& p=previous->spec;const int oldLo[]={p.originFine.x,p.originFine.y,p.originFine.z},oldDim[]={p.dimensions.x,p.dimensions.y,p.dimensions.z};for(unsigned i=0;i<3;++i){lo[i]=std::min(lo[i],oldLo[i]);hi[i]=std::max(hi[i],oldLo[i]+oldDim[i]);}}
        for(unsigned i=0;i<3;++i)if(hi[i]<=lo[i]||hi[i]-lo[i]>128)return invalid("aligned/swept private field exceeds 128 cells per axis");
        const auto samples=std::uint64_t(hi[0]-lo[0])*(hi[1]-lo[1])*(hi[2]-lo[2])*spec.channels.size();if(samples>limits.local.maxLocalSamples)return invalid("aligned private field exceeds the sample budget");spec.originFine={lo[0],lo[1],lo[2]};spec.dimensions={hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]};return true;
    }
    bool retire(std::unique_ptr<Generation> source,f::Tick tick,std::string& error,std::uint64_t replacementFor=0){
        if(!source)return true;source->replacementFor=replacementFor;
        // A shared exit is staged together with the next publication batch.
        // Keep its generation until that batch commits; capacity rollback must
        // not leave a live WF source without its retirement metadata.
        if(sharing.inspect(source->sharing).state==f::LocalState::LocalOnly){const auto r=sharing.retire(source->sharing,tick);if(!r.accepted()){error=r.error;return false;}}
        if(sharing.valid(source->sharing)||local.stats().retiredInstances>=limits.local.maxInstances)retired.push_back(std::move(source));else if(!local.retire(source->local,error))return false;return true;
    }
    std::unique_ptr<Generation> create(f::LocalSpec spec,f::Tick tick,std::string& error){
        if(lastSource==UINT64_MAX){error="NG-E-EFFECT-DEPOSIT: source identity exhausted";return {};}
        spec.source=++lastSource;auto out=std::make_unique<Generation>();out->spec=std::move(spec);if(!local.estimate(out->spec,out->cost,error)||!local.create(out->spec,out->local,error))return {};
        auto registration=sharing.create({out->spec.owner,out->spec.source,limits.maxPagesPerSource},tick);if(!registration.result.accepted()){std::string ignored;local.retire(out->local,ignored);error=registration.result.error;return {};}out->sharing=registration.handle;return out;
    }
    unsigned subscribers(const Generation& source) const{std::vector<f::ChannelId> channels;for(const auto& channel:source.spec.channels)channels.push_back(channel.channel);return world.subscribers(coverage(source.spec),channels);}
    void defer(Binding& binding,bool promotion,const std::string& why){auto& p=binding.publication;const auto tick=prepared.frame.tick;if(p.pending&&p.lastAttemptTick==tick)return;if(!p.pending){p.pendingSince=tick;p.deferredTicks=0;}p.pending=true;p.lastAttemptTick=tick;++p.deferredTicks;p.reason=why;++counters.deferredCandidates;counters.lastCandidateDeferral=why;if(promotion)++counters.deferredPromotions;else ++counters.deferredUpdates;}
};
std::vector<f::Channel> GpuFieldDeposits::defaultCatalog(){return {{"density","density-q","effects",f::Domain::VisualReference,f::Merge::Sum,0,INT32_MAX,1000},{"moisture","moisture-q","effects",f::Domain::VisualReference,f::Merge::Maximum,0,65535,65535}};}
GpuFieldDeposits::GpuFieldDeposits(ID3D12Device* d,ID3D12Fence* timeline,std::vector<f::Channel> catalog,GpuDepositLimits limits){
    if(!d||!timeline||!limits.maxInstances||limits.maxInstances>128||limits.local.maxInstances<limits.maxInstances*2||!limits.maxPagesPerSource||limits.maxPagesPerSource>4096||limits.local.maxAllocationBytes>limits.maxAllocationBytes||limits.world.maxAllocationBytes>limits.maxAllocationBytes-limits.local.maxAllocationBytes||limits.promotion.demoteAt<0||limits.promotion.promoteAt<=limits.promotion.demoteAt||!limits.promotion.promotionsPerTick||!limits.publicationMaxDelayTicks||limits.publicationMaxDelayTicks>432000)throw Error("NG-E-EFFECT-DEPOSIT","Invalid deposit limits/partition/policy");
    std::set<std::string> names;for(const auto& c:catalog)if(c.name.empty()||!names.insert(c.name).second||c.owner.empty()||c.unit.empty()||c.domain!=f::Domain::VisualReference||c.minimum!=0||c.maximum<=0||!c.unitsPerInteger||(c.merge!=f::Merge::Sum&&c.merge!=f::Merge::Maximum))throw Error("NG-E-EFFECT-DEPOSIT","Deposit catalog must contain unique nonnegative visual scalar channels");
    impl_=std::make_shared<Impl>(d,timeline,std::move(catalog),limits);
}
GpuFieldDeposits::GpuFieldDeposits(f::GpuVisualWorld& world,std::vector<f::ChannelId> writable,GpuDepositLimits limits){
    auto context=world.context();limits.grid=context.grid;
    if(!context.device||!context.timeline||!limits.maxInstances||limits.maxInstances>128||limits.local.maxInstances<limits.maxInstances*2||!limits.maxPagesPerSource||limits.maxPagesPerSource>4096||limits.local.maxAllocationBytes>limits.maxAllocationBytes||limits.promotion.demoteAt<0||limits.promotion.promoteAt<=limits.promotion.demoteAt||!limits.promotion.promotionsPerTick||!limits.publicationMaxDelayTicks||limits.publicationMaxDelayTicks>432000||writable.empty())throw Error("NG-E-EFFECT-DEPOSIT","Invalid shared-world limits or channel grant");
    std::set<std::string> names;for(const auto& c:context.catalog)if(c.name.empty()||!names.insert(c.name).second)throw Error("NG-E-EFFECT-DEPOSIT","Shared catalog requires unique channel names");
    std::set<f::ChannelId> seen;for(auto id:writable){if(id>=context.catalog.size()||!seen.insert(id).second)throw Error("NG-E-EFFECT-DEPOSIT","Unknown or repeated channel grant");const auto& c=context.catalog[id];if(c.owner.empty()||c.unit.empty()||c.domain!=f::Domain::VisualReference||c.minimum!=0||c.maximum<=0||!c.unitsPerInteger||(c.merge!=f::Merge::Sum&&c.merge!=f::Merge::Maximum))throw Error("NG-E-EFFECT-DEPOSIT","Channel grant must be a nonnegative visual scalar");}
    impl_=std::make_shared<Impl>(context.device,context.timeline,std::move(context.catalog),limits,&world,std::move(writable));
}
GpuFieldDeposits::~GpuFieldDeposits()=default;
bool GpuFieldDeposits::supports(const Asset& a,std::string& error) const{error.clear();bool writes=false;for(const auto& e:a.emitters)for(const auto& m:e.modules)writes|=m.kind==ModuleKind::FieldDeposit;if(!writes)return true;if(!impl_){error="NG-E-EFFECT-DEPOSIT: stopped";return false;}f::LocalSpec spec;return impl_->plan(a,{},nullptr,spec,error);}
GpuDepositAdmissionResult GpuFieldDeposits::admit(const GpuDepositSpawnRequest& request){
    if(!impl_||impl_->counters.failed||!request.asset||!request.serial||!finite(request.origin))return {SpawnStatus::UnsupportedFeature,"NG-E-EFFECT-DEPOSIT: invalid admission request",{}};auto& s=*impl_;const auto reject=[&](SpawnStatus status,std::string why){++s.counters.admissionRejected;return GpuDepositAdmissionResult{status,std::move(why),{}};};
    try{if(s.reservations.count(request.serial)||s.bindings.count(request.serial))return reject(SpawnStatus::UnsupportedFeature,"NG-E-EFFECT-DEPOSIT: serial already reserved");std::set<std::uint64_t> live;for(const auto& p:s.bindings)live.insert(p.first);for(const auto& p:s.reservations)live.insert(p.first);if(live.size()>=s.limits.maxInstances)return reject(SpawnStatus::Capacity,"NG-E-EFFECT-DEPOSIT: reserved writer count exceeded");
        Impl::Reservation reserved;reserved.asset=request.asset;reserved.origin=request.origin;std::string error;if(!s.plan(*request.asset,request.origin,nullptr,reserved.spec,error)||!s.local.estimate(reserved.spec,reserved.cost,error))return reject(SpawnStatus::UnsupportedFeature,std::move(error));collect();if(!s.fits(request.serial,reserved,false,error))return reject(SpawnStatus::Capacity,std::move(error));auto permit=std::make_shared<Impl::Permit>(impl_,request.serial);s.reservations.emplace(request.serial,std::move(reserved));return {SpawnStatus::Accepted,{},std::move(permit)};
    }catch(const std::exception& e){return reject(SpawnStatus::Capacity,e.what());}
}
bool GpuFieldDeposits::prepare(const GpuDepositFrame& frame,std::string& error){
    error.clear();if(!impl_){error="NG-E-EFFECT-DEPOSIT: stopped";return false;}auto& s=*impl_;const auto fail=[&](const std::string& why){s.counters.failed=true;error=why;return false;};
    if(s.prepared.active){error="NG-E-EFFECT-DEPOSIT: finish the pending publication before preparing again";return false;}
    if(s.sharing.inGroupTransaction()){error="NG-E-EFFECT-DEPOSIT: local preparation must precede joint publication";return false;}
    if(s.counters.failed)return fail("NG-E-EFFECT-DEPOSIT: failed recorder requires reconstruction");
    if(!frame.commands||!frame.fence||frame.fence<s.counters.lastFence||frame.fence<=s.timeline->GetCompletedValue()||frame.tick==UINT64_MAX||frame.instances.size()>s.limits.maxInstances||(s.hasSeen&&frame.tick<s.seenTick)||(s.hasSimulated&&frame.tick<=s.simulatedTick))return fail("NG-E-EFFECT-DEPOSIT: invalid frame/fence/tick/capacity");
    ID3D12Device* device=nullptr;const auto result=frame.commands->GetDevice(IID_PPV_ARGS(&device));const bool validDevice=SUCCEEDED(result)&&device==s.device;if(device)device->Release();if(!validDevice||(frame.commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&frame.commands->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return fail("NG-E-EFFECT-DEPOSIT: command list/device mismatch");
    try{
        std::set<std::uint64_t> live,written;std::set<std::pair<unsigned,unsigned>> handles;std::map<std::uint64_t,f::LocalSpec> plans;
        for(const auto& i:frame.instances){if(!i.asset||!i.serial||!finite(i.origin)||i.emitters.size()!=i.asset->emitters.size()||!live.insert(i.serial).second||!handles.emplace(i.instance.slot,i.instance.generation).second||std::any_of(i.emitters.begin(),i.emitters.end(),[](const auto* p){return !p;}))return fail("NG-E-EFFECT-DEPOSIT: invalid/duplicate instance input");auto found=s.bindings.find(i.serial);if(found!=s.bindings.end()&&(!equal(found->second.handle,i.instance)||found->second.assetHash!=i.asset->hash))return fail("NG-E-EFFECT-DEPOSIT: serial/generation/asset changed");f::LocalSpec spec;const auto reserved=s.reservations.find(i.serial);if(reserved!=s.reservations.end()){const auto& r=reserved->second;if(r.asset->hash!=i.asset->hash||r.origin.x!=i.origin.x||r.origin.y!=i.origin.y||r.origin.z!=i.origin.z)return fail("NG-E-EFFECT-DEPOSIT: reservation identity/origin mismatch");spec=r.spec;}else if(!s.plan(*i.asset,i.origin,found==s.bindings.end()?nullptr:found->second.current.get(),spec,error))return fail(error);plans.emplace(i.serial,std::move(spec));}
        collect();s.hasSeen=true;s.seenTick=frame.tick;
        for(auto it=s.bindings.begin();it!=s.bindings.end();)if(!live.count(it->first)){if(!s.retire(std::move(it->second.current),frame.tick,error))return fail(error);it=s.bindings.erase(it);}else ++it;
        s.counters.lastFence=frame.fence;s.counters.lastTick=frame.tick;
        if(!frame.simulated){++s.counters.flushes;return true;}
        for(const auto& instance:frame.instances){auto& binding=s.bindings[instance.serial];binding.handle=instance.instance;binding.assetHash=instance.asset->hash;if(instance.paused)continue;
            std::size_t batches=0;for(const auto& e:instance.asset->emitters)for(const auto& m:e.modules)batches+=m.kind==ModuleKind::FieldDeposit;if(s.local.stats().heldInputReferences/4+batches>s.limits.local.maxInputBatches){++s.counters.deferredWrites;continue;}
            auto& plan=plans.at(instance.serial);if(!binding.current||!same(binding.current->spec,plan)){f::GpuLocalFootprint cost;if(!s.local.estimate(plan,cost,error))return fail(error);if(!s.canMaterialize(instance.serial,cost)){++s.counters.deferredWrites;continue;}auto replacement=s.create(std::move(plan),frame.tick,error);if(!replacement)return fail(error);if(binding.current){binding.replaceShared=binding.replaceShared||s.sharing.inspect(binding.current->sharing).state==f::LocalState::Shared;if(!s.retire(std::move(binding.current),frame.tick,error,instance.serial))return fail(error);++s.counters.gridRebuilds;}binding.current=std::move(replacement);}
            auto& source=*binding.current;if(!s.local.begin(source.local,frame.commands,frame.fence,frame.tick,error))return fail(error);
            for(std::size_t emitter=0;emitter<instance.emitters.size();++emitter)for(const auto& module:instance.asset->emitters[emitter].modules)if(module.kind==ModuleKind::FieldDeposit){const auto channel=std::find_if(s.catalog.begin(),s.catalog.end(),[&](const auto& c){return c.name==module.fieldChannel;});if(!s.local.splat(source.local,static_cast<f::ChannelId>(channel-s.catalog.begin()),*instance.emitters[emitter],module.fieldQuantum,frame.commands,frame.fence,error))return fail(error);}
            if(!s.local.resolve(source.local,frame.commands,frame.fence,error))return fail(error);source.ready=true;written.insert(instance.serial);++s.counters.rewrites;
        }
        s.prepared.active=true;s.prepared.frame=frame;for(auto& instance:s.prepared.frame.instances)instance.emitters.clear();s.prepared.written=std::move(written);s.hasSimulated=true;s.simulatedTick=frame.tick;++s.counters.frames;return true;
    }catch(const std::exception& e){return fail(std::string("NG-E-EFFECT-DEPOSIT: ")+e.what());}
}
GpuDepositPublicationStage GpuFieldDeposits::stagePublication(ID3D12GraphicsCommandList* commands,std::uint64_t fence,std::string& error){
    error.clear();if(!impl_||impl_->counters.failed||!impl_->prepared.active||impl_->prepared.staged||!impl_->sharing.inGroupTransaction()||commands!=impl_->prepared.frame.commands||fence!=impl_->prepared.frame.fence){error="NG-E-EFFECT-DEPOSIT: invalid publication scope/recorder/fence";return GpuDepositPublicationStage::Invalid;}auto& s=*impl_;const auto& frame=s.prepared.frame;const auto& written=s.prepared.written;auto& selected=s.prepared.selected;s.prepared.staged=true;
    const auto invalid=[&](const std::string& why){s.counters.failed=true;error=why;return GpuDepositPublicationStage::Invalid;};
    const auto defer=[&](Impl::Binding& binding,bool promotion,const std::string& why){s.defer(binding,promotion,why);};
    try{
        for(const auto& source:s.retired)if(s.sharing.valid(source->sharing)&&(!source->replacementFor||!s.bindings.count(source->replacementFor))){const auto r=s.sharing.retire(source->sharing,frame.tick);if(!r.accepted())return invalid(r.error);}
        s.sharing.observe({},frame.tick,s.limits.promotion);
        auto ordered=frame.instances;std::sort(ordered.begin(),ordered.end(),[&](const auto& a,const auto& b){const auto& x=s.bindings.at(a.serial);const auto& y=s.bindings.at(b.serial);if(x.observation.priority!=y.observation.priority)return x.observation.priority>y.observation.priority;const auto tx=x.publication.pending?x.publication.pendingSince:frame.tick,ty=y.publication.pending?y.publication.pendingSince:frame.tick;if(tx!=ty)return tx<ty;const auto sx=x.current?x.current->spec.source:UINT64_MAX,sy=y.current?y.current->spec.source:UINT64_MAX;return std::tie(sx,a.serial)<std::tie(sy,b.serial);});
        for(const auto& instance:ordered){auto& binding=s.bindings.at(instance.serial);if(!binding.current||!binding.current->ready)continue;auto& source=*binding.current;auto info=s.sharing.inspect(source.sharing);const auto& external=binding.observation;const bool visible=external.worldVisible||instance.asset->fieldLevel=="worldVisible";const unsigned subscribers=s.subscribers(source)+external.subscribers;f::GpuSharingObservation observation{source.sharing,visible,subscribers,external.priority,external.concentration,{}};
            bool promotion=info.state==f::LocalState::LocalOnly;
            if(!binding.replaceShared){
                if(info.state==f::LocalState::Shared){const auto decision=s.sharing.observe({observation},frame.tick,s.limits.promotion).front().result;if(!decision.accepted())return invalid(decision.error);if(s.sharing.inspect(source.sharing).state!=f::LocalState::Shared){selected.emplace_back(instance.serial,false);continue;}if(!written.count(instance.serial)&&!binding.publication.pending)continue;}
                else if(info.state==f::LocalState::LocalOnly){if(!(visible||subscribers||external.concentration>=s.limits.promotion.promoteAt)){binding.publication.pending=false;binding.publication.reason.clear();binding.publication.deferredTicks=0;continue;}if(frame.tick<info.stateSince||frame.tick-info.stateSince<s.limits.promotion.minimumLocalTicks)continue;if(s.sharing.stats().promotionsThisTick>=s.limits.promotion.promotionsPerTick){defer(binding,true,"promotion quota");continue;}}
                else continue;
            }
            if(!s.canProject(instance.serial,source)){defer(binding,promotion,"private projection capacity");continue;}
            f::GpuVisualProposal proposal;proposal.owner=source.spec.owner;proposal.source=source.spec.source;proposal.region=coverage(source.spec);proposal.maxPages=source.spec.maxWorldPages;for(const auto& channel:source.spec.channels)proposal.channels.push_back(channel.channel);if(binding.replaceShared)for(const auto& old:s.retired)if(old->replacementFor==instance.serial&&s.sharing.valid(old->sharing))proposal.retiringSources.push_back(old->spec.source);
            const auto budget=s.world.budget(&proposal);if(!budget.fits()){if(budget.limit==f::GpuBudgetLimit::Invalid)return invalid(budget.reason());defer(binding,promotion,budget.reason());continue;}
            auto projection=s.local.project(source.local,frame.tick,frame.commands,frame.fence,error);if(!projection)return invalid(error);
            if(binding.replaceShared)for(const auto& old:s.retired)if(old->replacementFor==instance.serial&&s.sharing.valid(old->sharing)){const auto r=s.sharing.retire(old->sharing,frame.tick);if(!r.accepted())return invalid(r.error);}
            f::LocalResult accepted;if(binding.replaceShared)accepted=s.sharing.promote(source.sharing,projection,frame.tick);else if(promotion){observation.projection=std::move(projection);accepted=s.sharing.observe({observation},frame.tick,s.limits.promotion).front().result;}else accepted=s.sharing.project(source.sharing,projection,frame.tick);
            if(!accepted.accepted()){if(accepted.code==f::LocalCode::Capacity||(accepted.code==f::LocalCode::PublicationRejected&&accepted.publication==f::PublishResult::CapacityExceeded)){if(binding.replaceShared){error=accepted.error;return GpuDepositPublicationStage::Deferred;}defer(binding,promotion,accepted.error);continue;}return invalid(accepted.error);}selected.emplace_back(instance.serial,promotion);
        }
        return GpuDepositPublicationStage::Ready;
    }catch(const std::exception& e){return invalid(std::string("NG-E-EFFECT-DEPOSIT: ")+e.what());}
}
bool GpuFieldDeposits::finishPublication(f::GpuCommitResult committed,std::string& error){
    if(!impl_||!impl_->prepared.active||impl_->sharing.inGroupTransaction()){error="NG-E-EFFECT-DEPOSIT: publication scope has not finished";return false;}auto& s=*impl_;const auto& frame=s.prepared.frame;const auto& selected=s.prepared.selected;
    const auto fail=[&](const std::string& why){s.counters.failed=true;error=why;s.prepared={};return false;};
    if(!committed.accepted()){if(!committed.deferred())return fail(error.empty()?"NG-E-EFFECT-DEPOSIT: joint publication failed":error);if(s.sharing.stats().pending)return fail("NG-E-EFFECT-DEPOSIT: rollback must precede deferred completion");++s.counters.deferredCommits;s.counters.lastCommitDeferral=error;for(const auto& entry:selected)s.defer(s.bindings.at(entry.first),entry.second,error);s.prepared={};error.clear();return true;}
    if(!s.prepared.staged)return fail("NG-E-EFFECT-DEPOSIT: publication was not staged");
    if(s.world.stats().leases&&s.world.sourceView("",0).snapshotTick!=frame.tick)return fail("NG-E-EFFECT-DEPOSIT: committed snapshot tick differs from prepared simulation tick");
    try{
        for(const auto& entry:selected){auto& publication=s.bindings.at(entry.first).publication;publication.pending=false;publication.overdue=false;publication.reason.clear();publication.deferredTicks=0;publication.lastAttemptTick=publication.lastPublishedTick=frame.tick;}
        for(auto& pair:s.bindings)if(pair.second.current){auto& binding=pair.second;const auto r=s.sharing.settle(binding.current->sharing);if(!r.accepted())return fail(r.error);if(binding.replaceShared&&s.sharing.inspect(binding.current->sharing).state==f::LocalState::Shared){binding.replaceShared=false;auto reserved=s.reservations.find(pair.first);if(reserved!=s.reservations.end())reserved->second.projection=false;}}
        for(auto it=s.retired.begin();it!=s.retired.end();){if(s.sharing.valid((*it)->sharing)){const auto r=s.sharing.settle((*it)->sharing);if(!r.accepted())return fail(r.error);if(s.sharing.valid((*it)->sharing)){++it;continue;}}if(s.local.stats().retiredInstances>=s.limits.local.maxInstances){++it;continue;}if(!s.local.retire((*it)->local,error))return fail(error);it=s.retired.erase(it);}
        s.prepared={};error.clear();return true;
    }catch(const std::exception& e){return fail(std::string("NG-E-EFFECT-DEPOSIT: ")+e.what());}
}
f::GpuFieldSharing* GpuFieldDeposits::publicationController(){return impl_?&impl_->sharing:nullptr;}
bool GpuFieldDeposits::blocksNewViews(const f::LeaseRequest& request) const{
    if(!impl_)return true;f::Region bounds;if(!request.storageRegion(bounds))return true;
    for(const auto& source:impl_->retired){if(source->replacementFor&&impl_->bindings.count(source->replacementFor))continue;const auto state=impl_->sharing.inspect(source->sharing).state;if(state!=f::LocalState::Shared&&state!=f::LocalState::DemotePending)continue;const auto region=coverage(source->spec);if(bounds.min.x>=region.max.x||region.min.x>=bounds.max.x||bounds.min.y>=region.max.y||region.min.y>=bounds.max.y||bounds.min.z>=region.max.z||region.min.z>=bounds.max.z)continue;if(std::any_of(source->spec.channels.begin(),source->spec.channels.end(),[&](const auto& channel){return std::find(request.channels.begin(),request.channels.end(),channel.channel)!=request.channels.end();}))return true;}return false;
}
bool GpuFieldDeposits::record(const GpuDepositFrame& frame,std::string& error){
    if(!impl_||!impl_->ownedWorld){error="NG-E-EFFECT-DEPOSIT: shared world requires phased publication";return false;}
    if(!prepare(frame,error))return false;if(!frame.simulated)return true;auto& s=*impl_;f::GpuFieldTransaction transaction(s.world,{&s.sharing});
    if(!transaction.begin(error))return finishPublication({},error);
    const auto staged=stagePublication(frame.commands,frame.fence,error);
    if(staged!=GpuDepositPublicationStage::Ready){transaction.rollback();return finishPublication({staged==GpuDepositPublicationStage::Deferred?f::GpuCommitCode::Deferred:f::GpuCommitCode::Invalid},error);}
    if(!s.world.stats().leases){transaction.rollback();return finishPublication({f::GpuCommitCode::Committed},error);}
    return finishPublication(transaction.commit(frame.commands,frame.fence,frame.tick,error),error);
}

f::GpuAcquisition GpuFieldDeposits::acquire(f::LeaseRequest request){if(!impl_||impl_->counters.failed||request.writable)return {false,{},"NG-E-EFFECT-DEPOSIT: only live read grants are exposed"};auto r=impl_->world.acquire(request);if(r.accepted)impl_->readers.push_back({r.lease,std::move(request)});return r;}
bool GpuFieldDeposits::release(f::GpuLease lease,std::string& error){if(!impl_){error="NG-E-EFFECT-DEPOSIT: stopped";return false;}auto& s=*impl_;auto found=std::find_if(s.readers.begin(),s.readers.end(),[&](const auto& r){return r.lease.slot==lease.slot&&r.lease.generation==lease.generation;});if(found==s.readers.end()){error="NG-E-EFFECT-DEPOSIT: invalid reader";return false;}if(!s.world.release(lease,error))return false;s.readers.erase(found);return true;}
std::shared_ptr<f::GpuVisualView> GpuFieldDeposits::view(f::GpuLease lease,std::string& error){if(!impl_){error="NG-E-EFFECT-DEPOSIT: stopped";return {};}return impl_->world.view(lease,error);}
bool GpuFieldDeposits::observe(Handle h,GpuDepositObservation observation,std::string& error){error.clear();auto* b=impl_?impl_->find(h):nullptr;if(!b||observation.concentration<0||observation.subscribers>4096){error="NG-E-EFFECT-DEPOSIT: invalid observer/handle";return false;}b->observation=observation;return true;}
GpuDepositSourceInfo GpuFieldDeposits::inspect(Handle h) const{const auto* b=impl_?impl_->find(h):nullptr;if(!b||!b->current)return {};const auto& source=*b->current;auto publication=b->publication;publication.overdue=publication.pending&&impl_->seenTick>=publication.pendingSince&&impl_->seenTick-publication.pendingSince>=impl_->limits.publicationMaxDelayTicks;return {true,source.ready,source.spec.source,source.spec,impl_->sharing.inspect(source.sharing),impl_->local.inspect(source.local).sourceTick,std::move(publication)};}
std::vector<GpuDepositPublicationEntry> GpuFieldDeposits::publicationBacklog() const{
    std::vector<GpuDepositPublicationEntry> out;if(!impl_)return out;for(const auto& entry:impl_->bindings){const auto& binding=entry.second;if(binding.publication.pending){const auto info=inspect(binding.handle);out.push_back({binding.handle,info.source,binding.observation.priority,info.publication});}}
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){if(a.priority!=b.priority)return a.priority>b.priority;return std::tie(a.publication.pendingSince,a.source)<std::tie(b.publication.pendingSince,b.source);});return out;
}
f::GpuLocalBuffers GpuFieldDeposits::localBuffers(Handle h) const{const auto* b=impl_?impl_->find(h):nullptr;return b&&b->current?impl_->local.buffers(b->current->local):f::GpuLocalBuffers{};}
bool GpuFieldDeposits::markLocalUsed(Handle h,std::uint64_t fence){auto* b=impl_?impl_->find(h):nullptr;return b&&b->current&&impl_->local.markUsed(b->current->local,fence);}
void GpuFieldDeposits::collect(){if(impl_){impl_->world.collect();impl_->local.collect();}}
GpuDepositStats GpuFieldDeposits::stats() const{if(!impl_)return {};auto out=impl_->counters;out.instances=impl_->bindings.size();out.pendingRetirements=impl_->retired.size();out.readLeases=impl_->readers.size();out.local=impl_->local.stats();out.world=impl_->world.stats();out.sharing=impl_->sharing.stats();out.ownsWorld=bool(impl_->ownedWorld);out.publicationPrepared=impl_->prepared.active;out.publicationStaged=impl_->prepared.staged;for(const auto& instance:impl_->prepared.frame.instances)out.retainedPoolPointers+=instance.emitters.size();out.allocationBytes=out.local.allocationBytes+(out.ownsWorld?out.world.accountedBytes:0);out.reservations=impl_->reservations.size();for(const auto& r:impl_->reservations){out.reservedMutableBytes+=r.second.cost.mutableBytes;out.reservedSamples+=r.second.cost.samples;}out.unmaterializedBytes=impl_->headroom().bytes;for(const auto& entry:impl_->bindings)if(entry.second.publication.pending){++out.pendingPublications;if(impl_->seenTick>=entry.second.publication.pendingSince&&impl_->seenTick-entry.second.publication.pendingSince>=impl_->limits.publicationMaxDelayTicks)++out.overduePublications;}return out;}
bool GpuFieldDeposits::shutdown(std::string& error){error.clear();if(!impl_)return true;collect();if(impl_->prepared.active||!impl_->bindings.empty()||!impl_->retired.empty()||!impl_->readers.empty()||!impl_->reservations.empty()||stats().allocationBytes){error="NG-E-EFFECT-DEPOSIT: runtime/consumers/resources still active";return false;}if((impl_->ownedWorld&&!impl_->world.shutdown(error))||!impl_->local.shutdown(error))return false;impl_.reset();return true;}
}
