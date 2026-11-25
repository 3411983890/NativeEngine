#include "gpu_sharing.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace engine::fields {
namespace {
LocalResult result(LocalCode code,const char* error="",PublishResult publication=PublishResult::Invalid){return {code,publication,error};}
bool same(Region a,Region b){return a.min==b.min&&a.max==b.max;}
struct GroupWriterScope {
    bool& allowed;bool previous;
    explicit GroupWriterScope(bool& flag):allowed(flag),previous(flag){allowed=true;}
    ~GroupWriterScope(){allowed=previous;}
};
}
GpuFieldSharing::GpuFieldSharing(GpuVisualWorld& world,std::size_t maxInstances):world_(world),maxInstances_(maxInstances){
    if(!maxInstances||maxInstances>4096)throw std::invalid_argument("NG-E-FIELD-SHARING: invalid instance budget");slots_.reserve(maxInstances);
}
GpuFieldSharing::~GpuFieldSharing(){rollbackTransaction();if(world_.transactionGroup_)world_.transactionGroup_->rollback();for(auto& slot:slots_)if(slot.active&&slot.hasWriter){std::string ignored;world_.release(slot.writer,ignored);}}
bool GpuFieldSharing::beginTransaction(std::string& error){
    error.clear();if(checkpoint_){error="NG-E-FIELD-SHARING: nested transaction";return false;}
    try{auto saved=std::make_unique<Checkpoint>(Checkpoint{slots_,counters_,observationTick_,hasObservationTick_});if(!world_.beginTransaction(error))return false;checkpoint_=std::move(saved);return true;}catch(const std::exception& e){error=e.what();return false;}
}
void GpuFieldSharing::rollbackTransaction() noexcept{
    if(group_){group_->rollback();return;}
    if(!checkpoint_)return;if(world_.rollbackTransaction()){slots_=std::move(checkpoint_->slots);counters_=checkpoint_->counters;observationTick_=checkpoint_->observationTick;hasObservationTick_=checkpoint_->hasObservationTick;}checkpoint_.reset();
}
GpuCommitResult GpuFieldSharing::commitTransaction(ID3D12GraphicsCommandList* list,std::uint64_t fence,Tick tick,std::string& error){
    if(group_){error="NG-E-FIELD-SHARING: joint publication requires group commit";return {};}
    if(!checkpoint_){error="NG-E-FIELD-SHARING: no transaction";return {};}
    const auto result=world_.tryCommit(list,fence,tick,error);if(result.accepted()){world_.finishTransaction();checkpoint_.reset();}else rollbackTransaction();return result;
}
const GpuFieldSharing::Slot* GpuFieldSharing::find(GpuSharingHandle h) const{if(h.slot>=slots_.size())return nullptr;const auto& s=slots_[h.slot];return s.active&&s.generation==h.generation?&s:nullptr;}
GpuFieldSharing::Slot* GpuFieldSharing::find(GpuSharingHandle h){return const_cast<Slot*>(static_cast<const GpuFieldSharing*>(this)->find(h));}
bool GpuFieldSharing::valid(GpuSharingHandle h) const{return find(h)!=nullptr;}
GpuSharingInfo GpuFieldSharing::inspect(GpuSharingHandle h) const{const auto* s=find(h);return s?s->info:GpuSharingInfo{};}
GpuSharingCreation GpuFieldSharing::create(GpuSharingSpec spec,Tick createdAt){
    if(world_.transactionGroup_)return {result(LocalCode::Busy,"NG-E-FIELD-SHARING: register before joint publication"),{}};
    if(spec.owner.empty()||spec.owner.size()>128||spec.source<=lastSource_||!spec.maxWorldPages||spec.maxWorldPages>4096||createdAt==UINT64_MAX)return {result(LocalCode::Invalid,"NG-E-FIELD-SHARING: invalid source registration"),{}};
    for(const auto& s:slots_)if(s.active&&s.spec.owner==spec.owner&&s.spec.source==spec.source)return {result(LocalCode::Invalid,"NG-E-FIELD-SHARING: source already registered"),{}};
    std::size_t index=0;while(index<slots_.size()&&(slots_[index].active||!slots_[index].generation))++index;if(index>=maxInstances_)return {result(LocalCode::Capacity,"NG-E-FIELD-SHARING: instance budget"),{}};
    if(index==slots_.size())slots_.emplace_back();auto& s=slots_[index];s.spec=std::move(spec);s.info.state=LocalState::LocalOnly;s.info.source=s.spec.source;s.info.stateSince=s.info.lastActionTick=createdAt;s.active=true;lastSource_=s.spec.source;
    return {result(LocalCode::Accepted),{static_cast<std::uint32_t>(index),s.generation}};
}
LocalResult GpuFieldSharing::publish(Slot& s,std::shared_ptr<GpuLocalProjection> projection,Tick tick){
    if(!canMutate())return result(LocalCode::Busy,"NG-E-FIELD-SHARING: not a joint publication participant");
    if(s.info.pending)return result(LocalCode::Busy);
    if(!projection||tick==UINT64_MAX||tick<s.info.lastActionTick||s.info.acknowledgedEpoch==UINT64_MAX)return result(LocalCode::Invalid,"NG-E-FIELD-SHARING: projection/time/epoch");
    auto b=projection->bindings();if(b.owner!=s.spec.owner||b.source!=s.spec.source||b.publishTick>tick||b.sourceTick>tick||b.sourceTick<s.info.sourceTick||b.planes.empty())return result(LocalCode::Invalid,"NG-E-FIELD-SHARING: source identity/time");
    std::vector<ChannelId> channels;channels.reserve(b.planes.size());for(const auto& p:b.planes)channels.push_back(p.channel);
    if(s.hasWriter&&(!same(b.region,s.region)||channels!=s.channels))return result(LocalCode::Invalid,"NG-E-FIELD-SHARING: moving coverage/channels require a separate transition");
    GroupWriterScope access(world_.groupMutationAllowed_);GpuLease writer=s.writer;bool acquired=false;
    if(!s.hasWriter){LeaseRequest r;r.owner=s.spec.owner;r.writable=true;r.region=b.region;r.channels=channels;r.maxPages=s.spec.maxWorldPages;r.maxAgeTicks=s.spec.maxAgeTicks;auto a=world_.acquire(std::move(r));if(!a.accepted){++counters_.rejections;return result(LocalCode::Capacity,a.error.c_str());}writer=a.lease;acquired=true;}
    const auto epoch=s.info.acknowledgedEpoch+1;std::string error;const auto accepted=world_.publish(writer,std::move(projection),epoch,tick,error);
    if(accepted!=PublishResult::Accepted&&accepted!=PublishResult::Duplicate){if(acquired){std::string ignored;world_.release(writer,ignored);}++counters_.rejections;return result(LocalCode::PublicationRejected,error.c_str(),accepted);}
    s.writer=writer;s.hasWriter=true;s.region=b.region;s.channels=std::move(channels);s.info.pending=true;s.info.pendingEpoch=epoch;s.info.pendingPublishTick=tick;s.info.lastActionTick=tick;s.info.sourceTick=b.sourceTick;s.info.localVersion=b.localVersion;
    if(s.info.state==LocalState::LocalOnly){s.info.state=LocalState::PromotePending;++counters_.promotions;}else ++counters_.updates;
    return result(LocalCode::Accepted,"",accepted);
}
LocalResult GpuFieldSharing::promote(GpuSharingHandle h,std::shared_ptr<GpuLocalProjection> p,Tick tick){auto* s=find(h);if(!s)return result(LocalCode::InvalidHandle);if(s->info.pending)return result(LocalCode::Busy);if(s->info.state!=LocalState::LocalOnly)return result(LocalCode::NoChange);return publish(*s,std::move(p),tick);}
LocalResult GpuFieldSharing::project(GpuSharingHandle h,std::shared_ptr<GpuLocalProjection> p,Tick tick){auto* s=find(h);if(!s)return result(LocalCode::InvalidHandle);if(s->info.pending)return result(LocalCode::Busy);if(s->info.state!=LocalState::Shared)return result(LocalCode::NotShared);return publish(*s,std::move(p),tick);}
LocalResult GpuFieldSharing::withdraw(Slot& s,Tick tick,bool retiring){
    if(!canMutate())return result(LocalCode::Busy,"NG-E-FIELD-SHARING: not a joint publication participant");
    if(s.info.pending)return result(LocalCode::Busy);if(s.info.state!=LocalState::Shared)return result(LocalCode::NotShared);
    if(tick==UINT64_MAX||tick<s.info.lastActionTick||tick<s.info.sourceTick||s.info.acknowledgedEpoch==UINT64_MAX)return result(LocalCode::Invalid,"NG-E-FIELD-SHARING: withdrawal time/epoch");
    GroupWriterScope access(world_.groupMutationAllowed_);const auto epoch=s.info.acknowledgedEpoch+1;std::string error;const auto accepted=world_.withdraw(s.writer,s.spec.source,epoch,tick,error);
    if(accepted!=PublishResult::Accepted){++counters_.rejections;return result(LocalCode::PublicationRejected,error.c_str(),accepted);}
    world_.retireCoverage(s.writer);
    s.info.pending=true;s.info.pendingEpoch=epoch;s.info.pendingPublishTick=s.info.lastActionTick=tick;s.info.retireAfterCommit=retiring;s.info.state=LocalState::DemotePending;++counters_.withdrawals;return result(LocalCode::Accepted,"",accepted);
}
LocalResult GpuFieldSharing::demote(GpuSharingHandle h,Tick tick){auto* s=find(h);return s?withdraw(*s,tick,false):result(LocalCode::InvalidHandle);}
void GpuFieldSharing::dispose(Slot& s){GroupWriterScope access(world_.groupMutationAllowed_);if(s.hasWriter){std::string ignored;world_.release(s.writer,ignored);}const auto generation=s.generation==UINT32_MAX?0:s.generation+1;s=Slot{};s.generation=generation;}
LocalResult GpuFieldSharing::retire(GpuSharingHandle h,Tick tick){if(!canMutate())return result(LocalCode::Busy,"NG-E-FIELD-SHARING: not a joint publication participant");auto* s=find(h);if(!s)return result(LocalCode::InvalidHandle);if(s->info.pending)return result(LocalCode::Busy);if(tick==UINT64_MAX||tick<s->info.lastActionTick||tick<s->info.stateSince)return result(LocalCode::Invalid,"NG-E-FIELD-SHARING: retirement time");if(s->info.state==LocalState::LocalOnly){dispose(*s);return result(LocalCode::Accepted);}return withdraw(*s,tick,true);}
LocalResult GpuFieldSharing::settle(GpuSharingHandle h){
    if(world_.transactionGroup_)return result(LocalCode::Busy,"NG-E-FIELD-SHARING: settle after joint commit");
    auto* s=find(h);if(!s)return result(LocalCode::InvalidHandle);if(!s->info.pending)return result(LocalCode::NoChange);
    const auto v=world_.sourceView(s->spec.owner,s->spec.source);const bool demoting=s->info.state==LocalState::DemotePending;
    if(!v.exists||!v.resident||v.epoch!=s->info.pendingEpoch||v.publishTick!=s->info.pendingPublishTick||v.hasContribution==demoting||v.sourceTick!=s->info.sourceTick)return result(LocalCode::Busy);
    if(demoting){std::string error;if(!world_.release(s->writer,error))return result(LocalCode::PublicationRejected,error.c_str());s->hasWriter=false;}
    s->info.acknowledgedEpoch=s->info.pendingEpoch;s->info.pendingEpoch=0;s->info.pending=false;
    if(s->info.retireAfterCommit){dispose(*s);return result(LocalCode::Accepted);}
    const auto next=demoting?LocalState::LocalOnly:LocalState::Shared;if(next!=s->info.state)s->info.stateSince=v.snapshotTick;s->info.state=next;return result(LocalCode::Accepted);
}
std::vector<GpuSharingObservationResult> GpuFieldSharing::observe(const std::vector<GpuSharingObservation>& input,Tick tick,const PromotionPolicy& policy){
    if(!canMutate())throw std::invalid_argument("NG-E-FIELD-SHARING: not a joint publication participant");
    if(tick==UINT64_MAX||policy.demoteAt<0||policy.promoteAt<=policy.demoteAt||!policy.promotionsPerTick||input.size()>maxInstances_||(hasObservationTick_&&tick<observationTick_))throw std::invalid_argument("NG-E-FIELD-SHARING: observation batch/policy/time");
    std::set<std::pair<std::uint32_t,std::uint32_t>> seen;for(const auto& o:input)if(o.concentration<0||!seen.emplace(o.handle.slot,o.handle.generation).second)throw std::invalid_argument("NG-E-FIELD-SHARING: duplicate/invalid observation");
    auto ordered=input;std::sort(ordered.begin(),ordered.end(),[&](const auto& a,const auto& b){if(a.priority!=b.priority)return a.priority>b.priority;const auto* x=find(a.handle);const auto* y=find(b.handle);if(!!x!=!!y)return !!x;if(x&&y)return std::tie(x->spec.source,x->spec.owner)<std::tie(y->spec.source,y->spec.owner);return std::tie(a.handle.slot,a.handle.generation)<std::tie(b.handle.slot,b.handle.generation);});
    std::vector<GpuSharingObservationResult> output;output.reserve(ordered.size());if(!hasObservationTick_||tick!=observationTick_){hasObservationTick_=true;observationTick_=tick;counters_.promotionsThisTick=0;}
    for(const auto& o:ordered){auto* s=find(o.handle);auto r=result(LocalCode::NoChange);
        if(!s)r=result(LocalCode::InvalidHandle);else if(s->info.pending)r=result(LocalCode::Busy);else if(tick<s->info.stateSince||tick<s->info.lastActionTick)r=result(LocalCode::Invalid,"NG-E-FIELD-SHARING: observation precedes source state");
        else if(s->info.state==LocalState::LocalOnly&&(o.worldVisible||o.subscribers||o.concentration>=policy.promoteAt)&&tick-s->info.stateSince>=policy.minimumLocalTicks){if(counters_.promotionsThisTick>=policy.promotionsPerTick)r=result(LocalCode::Capacity,"NG-E-FIELD-SHARING: promotion quota");else{r=promote(o.handle,o.projection,tick);if(r.code==LocalCode::Accepted)++counters_.promotionsThisTick;}}
        else if(s->info.state==LocalState::Shared&&!o.worldVisible&&!o.subscribers&&o.concentration<=policy.demoteAt&&tick-s->info.stateSince>=policy.minimumSharedTicks)r=demote(o.handle,tick);
        output.push_back({o.handle,std::move(r)});
    }return output;
}
GpuSharingStats GpuFieldSharing::stats() const{auto out=counters_;for(const auto& s:slots_)if(s.active){++out.instances;if(s.info.state==LocalState::Shared)++out.shared;if(s.info.pending)++out.pending;}return out;}
GpuFieldTransaction::GpuFieldTransaction(GpuVisualWorld& world,std::vector<GpuFieldSharing*> participants):world_(world),participants_(std::move(participants)){}
GpuFieldTransaction::~GpuFieldTransaction(){rollback();}
bool GpuFieldTransaction::begin(std::string& error){
    error.clear();if(active_||participants_.empty()){error="NG-E-FIELD-GROUP: active/empty publication group";return false;}
    std::set<GpuFieldSharing*> seen;for(auto* p:participants_)if(!p||&p->world_!=&world_||p->checkpoint_||p->stats().pending||!seen.insert(p).second){error="NG-E-FIELD-GROUP: invalid/duplicate/foreign/busy participant";return false;}
    if(world_.hasStagedPublications()){error="NG-E-FIELD-GROUP: publish all sources inside the joint boundary";return false;}
    try{std::vector<std::unique_ptr<GpuFieldSharing::Checkpoint>> saved;saved.reserve(participants_.size());for(auto* p:participants_)saved.push_back(std::make_unique<GpuFieldSharing::Checkpoint>(GpuFieldSharing::Checkpoint{p->slots_,p->counters_,p->observationTick_,p->hasObservationTick_}));
        if(!world_.beginTransaction(error))return false;
        for(std::size_t i=0;i<participants_.size();++i){participants_[i]->checkpoint_=std::move(saved[i]);participants_[i]->group_=this;}world_.transactionGroup_=this;active_=true;return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
void GpuFieldTransaction::rollback() noexcept{
    if(!active_)return;const bool restored=world_.rollbackTransaction();
    for(auto* p:participants_){if(restored&&p->checkpoint_){auto& c=*p->checkpoint_;p->slots_=std::move(c.slots);p->counters_=c.counters;p->observationTick_=c.observationTick;p->hasObservationTick_=c.hasObservationTick;}p->checkpoint_.reset();p->group_=nullptr;}
    world_.transactionGroup_=nullptr;world_.groupCommitInProgress_=world_.groupMutationAllowed_=false;active_=false;
}
GpuCommitResult GpuFieldTransaction::commit(ID3D12GraphicsCommandList* list,std::uint64_t fence,Tick tick,std::string& error){
    error.clear();if(!active_){error="NG-E-FIELD-GROUP: no active publication group";return {};}
    world_.groupCommitInProgress_=true;GpuCommitResult committed;
    try{committed=world_.tryCommit(list,fence,tick,error);}catch(const std::exception& e){error=e.what();committed={GpuCommitCode::ResourceFailure};}
    world_.groupCommitInProgress_=false;if(!committed.accepted()){rollback();return committed;}
    world_.finishTransaction();for(auto* p:participants_){p->checkpoint_.reset();p->group_=nullptr;}world_.transactionGroup_=nullptr;world_.groupMutationAllowed_=false;active_=false;return committed;
}
}
