#include "local.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace engine::fields {
namespace {
LocalResult result(LocalCode code,const char* error="",PublishResult publication=PublishResult::Invalid) {
    return {code,publication,error};
}
bool same(Cell a,Cell b){return a==b;}
bool validBounds(Cell origin,Cell dims) {
    for(auto pair:{std::make_pair(origin.x,dims.x),std::make_pair(origin.y,dims.y),std::make_pair(origin.z,dims.z)})
        if(pair.second<=0||static_cast<std::int64_t>(pair.first)+pair.second>std::numeric_limits<std::int32_t>::max())return false;
    return true;
}
std::size_t count(Cell dims,std::size_t limit) {
    std::size_t n=1;
    for(auto d:{dims.x,dims.y,dims.z}){if(d<=0||static_cast<std::uint64_t>(d)>limit/n)return 0;n*=static_cast<std::size_t>(d);}
    return n;
}
std::int32_t floorDivide(std::int32_t value,unsigned scale) {
    const auto d=static_cast<std::int32_t>(scale);
    return value/d-(value%d<0?1:0);
}
Cell coarse(Cell p,unsigned scale){return {floorDivide(p.x,scale),floorDivide(p.y,scale),floorDivide(p.z,scale)};}
Region region(Cell origin,Cell dims,unsigned scale) {
    const auto last=coarse({origin.x+dims.x-1,origin.y+dims.y-1,origin.z+dims.z-1},scale);
    return {coarse(origin,scale),{last.x+1,last.y+1,last.z+1}};
}
std::size_t localIndex(Cell position,Cell origin,Cell dims) {
    return static_cast<std::size_t>(position.x-origin.x)+static_cast<std::size_t>(dims.x)*
        (static_cast<std::size_t>(position.y-origin.y)+static_cast<std::size_t>(dims.y)*(position.z-origin.z));
}
bool contains(Cell origin,Cell dims,Cell p) {
    return p.x>=origin.x&&static_cast<std::int64_t>(p.x)<static_cast<std::int64_t>(origin.x)+dims.x&&
        p.y>=origin.y&&static_cast<std::int64_t>(p.y)<static_cast<std::int64_t>(origin.y)+dims.y&&
        p.z>=origin.z&&static_cast<std::int64_t>(p.z)<static_cast<std::int64_t>(origin.z)+dims.z;
}
}
LocalFields::LocalFields(World& world,LocalLimits limits):world_(world),limits_(limits) {
    if(!limits.maxInstances||limits.maxInstances>4096||!limits.maxLocalSamples||limits.maxLocalSamples>16777216)
        throw std::invalid_argument("NG-E-LEF-BUDGET: invalid local field limits");
}
LocalFields::~LocalFields() {
    for(auto& slot:slots_)if(slot.active){if(slot.hasWriter)world_.release(slot.writer);if(slot.hasPendingWriter)world_.release(slot.pendingWriter);}
}
const LocalFields::Slot* LocalFields::find(LocalHandle h) const {
    if(h.slot>=slots_.size())return nullptr;
    const auto& slot=slots_[h.slot];return slot.active&&slot.generation==h.generation?&slot:nullptr;
}
LocalFields::Slot* LocalFields::find(LocalHandle h){return const_cast<Slot*>(static_cast<const LocalFields*>(this)->find(h));}
bool LocalFields::valid(LocalHandle h) const{return find(h)!=nullptr;}
LocalInfo LocalFields::inspect(LocalHandle h) const {const auto* slot=find(h);return slot?slot->info:LocalInfo{};}
LocalCreation LocalFields::create(LocalSpec spec) {
    const auto reject=[](LocalCode code,const char* error){return LocalCreation{result(code,error),{}};};
    if(spec.owner.empty()||spec.owner.size()>128||spec.source<=lastSource_||spec.channels.empty()||spec.channels.size()>8||
        !validBounds(spec.originFine,spec.dimensions)||!spec.finePerWorld||spec.finePerWorld>16||
        (spec.finePerWorld&(spec.finePerWorld-1))||!spec.maxWorldPages)return reject(LocalCode::Invalid,"NG-E-LEF-SPEC");
    const auto n=count(spec.dimensions,limits_.maxLocalSamples);
    if(!n||n>limits_.maxLocalSamples/spec.channels.size()||n*spec.channels.size()>limits_.maxLocalSamples-localSamples_)
        return reject(LocalCode::Capacity,"NG-E-LEF-BUDGET");
    std::sort(spec.channels.begin(),spec.channels.end(),[](const auto& a,const auto& b){return a.channel<b.channel;});
    for(std::size_t i=0;i<spec.channels.size();++i) {
        const auto& c=spec.channels[i];
        if(c.reduction!=ProjectionReduction::VolumeAverage&&c.reduction!=ProjectionReduction::Maximum)return reject(LocalCode::Invalid,"NG-E-LEF-REDUCTION");
        if(i&&spec.channels[i-1].channel==c.channel)return reject(LocalCode::Invalid,"NG-E-LEF-DUPLICATE-CHANNEL");
        try {
            const auto& descriptor=world_.channel(c.channel);
            if(descriptor.domain!=Domain::VisualReference||descriptor.owner!=spec.owner||descriptor.minimum>0||descriptor.maximum<0||
                world_.sourcePresent(c.channel,spec.source))return reject(LocalCode::Invalid,"NG-E-LEF-OWNER-OR-DOMAIN");
            if((c.reduction==ProjectionReduction::VolumeAverage&&(descriptor.merge!=Merge::Sum||descriptor.minimum!=0))||
                (c.reduction==ProjectionReduction::Maximum&&descriptor.merge!=Merge::Maximum))return reject(LocalCode::Invalid,"NG-E-LEF-REDUCTION");
        }catch(const std::out_of_range&){return reject(LocalCode::Invalid,"NG-E-LEF-CHANNEL");}
    }
    std::size_t index=0;while(index<slots_.size()&&(slots_[index].active||slots_[index].generation==0))++index;
    if(index>=limits_.maxInstances)return reject(LocalCode::Capacity,"NG-E-LEF-INSTANCE-BUDGET");
    Slot next;next.active=true;next.spec=std::move(spec);
    next.info.state=LocalState::LocalOnly;next.info.source=next.spec.source;next.info.localVersion=1;
    next.info.originFine=next.spec.originFine;next.info.dimensions=next.spec.dimensions;
    for(const auto& c:next.spec.channels)next.values.push_back({c.channel,std::vector<std::int32_t>(n,0)});
    if(index==slots_.size())slots_.emplace_back();
    next.generation=slots_[index].generation;slots_[index]=std::move(next);
    localSamples_+=n*slots_[index].spec.channels.size();lastSource_=slots_[index].spec.source;
    return {result(LocalCode::Accepted),{static_cast<std::uint32_t>(index),slots_[index].generation}};
}
LocalResult LocalFields::replace(LocalHandle h,const std::vector<LocalValues>& values,Tick sourceTick) {
    auto* slot=find(h);if(!slot)return result(LocalCode::InvalidHandle);
    if(slot->info.state==LocalState::DemotePending)return result(LocalCode::Busy);
    if(sourceTick<slot->info.sourceTick||slot->info.localVersion==std::numeric_limits<std::uint64_t>::max()||values.size()!=slot->values.size())return result(LocalCode::Invalid,"NG-E-LEF-VERSION");
    auto next=values;std::sort(next.begin(),next.end(),[](const auto& a,const auto& b){return a.channel<b.channel;});
    for(std::size_t i=0;i<next.size();++i){
        if(next[i].channel!=slot->values[i].channel||next[i].values.size()!=slot->values[i].values.size())return result(LocalCode::Invalid,"NG-E-LEF-PAYLOAD");
        const auto& descriptor=world_.channel(next[i].channel);
        for(auto value:next[i].values)if(value<descriptor.minimum||value>descriptor.maximum)return result(LocalCode::Invalid,"NG-E-LEF-VALUE");
    }
    slot->values.swap(next);slot->info.sourceTick=sourceTick;++slot->info.localVersion;return result(LocalCode::Accepted);
}
LocalResult LocalFields::publish(Slot& slot,Cell origin,Tick publishTick,bool moving) {
    if(slot.info.pending)return result(LocalCode::Busy);
    if(slot.info.sourceTick>publishTick||slot.info.transformTick>publishTick||!validBounds(origin,slot.spec.dimensions)||slot.info.acknowledgedEpoch>std::numeric_limits<std::uint64_t>::max()-2)return result(LocalCode::Invalid,"NG-E-LEF-TIME-OR-BOUNDS");
    Lease writer=slot.writer;bool acquired=false;
    if(!slot.hasWriter||moving) {
        LeaseRequest request;request.owner=slot.spec.owner;request.writable=true;request.region=region(origin,slot.spec.dimensions,slot.spec.finePerWorld);
        request.maxPages=slot.spec.maxWorldPages;request.maxAgeTicks=slot.spec.maxAgeTicks;
        for(const auto& c:slot.spec.channels)request.channels.push_back(c.channel);
        const auto allocation=world_.acquire(std::move(request));
        if(!allocation.accepted){++rejections_;return result(LocalCode::Capacity,allocation.error.c_str());}
        writer=allocation.lease;acquired=true;
    }
    const auto epoch=slot.info.acknowledgedEpoch+(moving?2:1);
    std::vector<Publication> publications;std::vector<ProjectionMetric> metrics;
    const auto scale=slot.spec.finePerWorld;const std::int64_t volume=scale*scale*scale;
    for(std::size_t channelIndex=0;channelIndex<slot.values.size();++channelIndex) {
        const auto id=slot.values[channelIndex].channel;
        if(moving)publications.push_back({slot.writer,id,slot.spec.source,epoch-1,slot.info.sourceTick,publishTick,Domain::VisualReference,{}});
        std::map<Cell,std::int64_t> reduced;ProjectionMetric metric;metric.channel=id;metric.reduction=slot.spec.channels[channelIndex].reduction;
        std::size_t index=0;
        for(std::int32_t z=0;z<slot.spec.dimensions.z;++z)for(std::int32_t y=0;y<slot.spec.dimensions.y;++y)for(std::int32_t x=0;x<slot.spec.dimensions.x;++x) {
            const auto value=slot.values[channelIndex].values[index++];const auto p=coarse({origin.x+x,origin.y+y,origin.z+z},scale);
            if(metric.reduction==ProjectionReduction::VolumeAverage){reduced[p]+=value;metric.inputIntegral+=value;}
            else {const auto found=reduced.find(p);if(found==reduced.end())reduced.emplace(p,value);else found->second=std::max(found->second,static_cast<std::int64_t>(value));}
        }
        Publication publication{writer,id,slot.spec.source,epoch,slot.info.sourceTick,publishTick,Domain::VisualReference,{}};
        for(const auto& cell:reduced) {
            const auto value=metric.reduction==ProjectionReduction::VolumeAverage?(cell.second+volume/2)/volume:cell.second;
            publication.writes.push_back({cell.first,static_cast<std::int32_t>(value)});
            if(metric.reduction==ProjectionReduction::VolumeAverage)metric.reconstructedIntegral+=value*volume;
        }
        metric.coarseSamples=reduced.size();metric.quantizationResidual=metric.inputIntegral-metric.reconstructedIntegral;
        metrics.push_back(metric);publications.push_back(std::move(publication));
    }
    const auto published=world_.publishAtomic(publications);
    if(published!=PublishResult::Accepted&&published!=PublishResult::Duplicate) {
        if(acquired){world_.release(writer);world_.collect(0);}++rejections_;
        return result(LocalCode::PublicationRejected,"NG-E-LEF-PUBLISH",published);
    }
    slot.info.pending=true;slot.info.pendingEpoch=epoch;slot.info.pendingPublishTick=publishTick;slot.info.projection=std::move(metrics);
    if(acquired){slot.pendingWriter=writer;slot.hasPendingWriter=true;}
    slot.info.originFine=origin;slot.spec.originFine=origin;
    if(slot.info.state==LocalState::LocalOnly)slot.info.state=LocalState::PromotePending;
    if(moving)++slot.info.localVersion;
    ++projections_;return result(LocalCode::Accepted,"",published);
}
LocalResult LocalFields::promote(LocalHandle h,Tick tick) {
    auto* slot=find(h);if(!slot)return result(LocalCode::InvalidHandle);
    if(slot->info.state!=LocalState::LocalOnly)return result(slot->info.pending?LocalCode::Busy:LocalCode::NoChange);
    return publish(*slot,slot->info.originFine,tick,false);
}
LocalResult LocalFields::project(LocalHandle h,Tick tick) {
    auto* slot=find(h);if(!slot)return result(LocalCode::InvalidHandle);
    if(slot->info.state!=LocalState::Shared)return result(slot->info.pending?LocalCode::Busy:LocalCode::NotShared);
    return publish(*slot,slot->info.originFine,tick,false);
}
LocalResult LocalFields::move(LocalHandle h,Cell origin,Tick tick) {
    auto* slot=find(h);if(!slot)return result(LocalCode::InvalidHandle);
    if(slot->info.pending)return result(LocalCode::Busy);
    if(!validBounds(origin,slot->spec.dimensions)||tick<slot->info.sourceTick||tick<slot->info.transformTick||slot->info.localVersion==std::numeric_limits<std::uint64_t>::max())return result(LocalCode::Invalid,"NG-E-LEF-BOUNDS-OR-TIME");
    if(same(origin,slot->info.originFine))return result(LocalCode::NoChange);
    if(slot->info.state==LocalState::LocalOnly){slot->info.originFine=origin;slot->spec.originFine=origin;slot->info.transformTick=tick;++slot->info.localVersion;return result(LocalCode::Accepted);}
    const auto response=publish(*slot,origin,tick,true);if(response.accepted())slot->info.transformTick=tick;return response;
}
LocalResult LocalFields::withdraw(Slot& slot,Tick tick,bool retiring) {
    if(slot.info.pending)return result(LocalCode::Busy);
    if(slot.info.state!=LocalState::Shared)return result(LocalCode::NotShared);
    if(slot.info.acknowledgedEpoch==std::numeric_limits<std::uint64_t>::max()||tick<slot.info.sourceTick||tick<slot.info.transformTick)return result(LocalCode::Invalid,"NG-E-LEF-TIME");
    std::vector<Publication> publications;
    const auto epoch=slot.info.acknowledgedEpoch+1;
    for(const auto& c:slot.spec.channels)publications.push_back({slot.writer,c.channel,slot.spec.source,epoch,slot.info.sourceTick,tick,Domain::VisualReference,{}});
    const auto published=world_.publishAtomic(publications);
    if(published!=PublishResult::Accepted){++rejections_;return result(LocalCode::PublicationRejected,"NG-E-LEF-WITHDRAW",published);}
    slot.info.pending=true;slot.info.pendingEpoch=epoch;slot.info.pendingPublishTick=tick;slot.info.retireAfterCommit=retiring;
    slot.info.state=LocalState::DemotePending;return result(LocalCode::Accepted,"",published);
}
LocalResult LocalFields::demote(LocalHandle h,Tick tick){auto* slot=find(h);return slot?withdraw(*slot,tick,false):result(LocalCode::InvalidHandle);}
void LocalFields::dispose(Slot& slot) {
    localSamples_-=slot.values.front().values.size()*slot.values.size();
    if(slot.hasWriter)world_.release(slot.writer);if(slot.hasPendingWriter)world_.release(slot.pendingWriter);
    const auto generation=slot.generation==std::numeric_limits<std::uint32_t>::max()?0:slot.generation+1;
    slot=Slot{};slot.generation=generation;
}
LocalResult LocalFields::retire(LocalHandle h,Tick tick) {
    auto* slot=find(h);if(!slot)return result(LocalCode::InvalidHandle);
    if(slot->info.pending)return result(LocalCode::Busy);
    if(slot->info.state==LocalState::LocalOnly){dispose(*slot);return result(LocalCode::Accepted);}
    return withdraw(*slot,tick,true);
}
LocalResult LocalFields::settle(LocalHandle h,std::uint64_t fence) {
    auto* slot=find(h);if(!slot)return result(LocalCode::InvalidHandle);
    if(!slot->info.pending)return result(LocalCode::NoChange);
    for(const auto& c:slot->spec.channels) {
        const auto view=world_.sourceView(c.channel,slot->spec.source);
        if(!view.exists||!view.resident||view.epoch!=slot->info.pendingEpoch||view.publishTick!=slot->info.pendingPublishTick)return result(LocalCode::Busy);
    }
    const bool demoting=slot->info.state==LocalState::DemotePending;
    if(slot->hasPendingWriter){if(slot->hasWriter)world_.release(slot->writer,fence);slot->writer=slot->pendingWriter;slot->hasWriter=true;slot->hasPendingWriter=false;}
    slot->info.acknowledgedEpoch=slot->info.pendingEpoch;slot->info.pendingEpoch=0;slot->info.pending=false;
    if(demoting){world_.release(slot->writer,fence);slot->hasWriter=false;}
    if(slot->info.retireAfterCommit){dispose(*slot);return result(LocalCode::Accepted);}
    const auto next=demoting?LocalState::LocalOnly:LocalState::Shared;
    if(slot->info.state!=next)slot->info.stateSince=world_.committedTick();
    slot->info.state=next;return result(LocalCode::Accepted);
}
RoutedSample LocalFields::sample(LocalHandle h,ChannelId id,Cell p,Tick tick,Representation preferred) const {
    const auto* slot=find(h);RoutedSample out;
    if(!slot){out.sample.status=Status::InvalidLease;return out;}
    out.source=slot->spec.source;
    const auto value=std::find_if(slot->values.begin(),slot->values.end(),[&](const auto& v){return v.channel==id;});
    if(value==slot->values.end()){out.sample.status=Status::AccessDenied;return out;}
    const auto local=[&] {
        Sample sample;sample.sourceTick=slot->info.sourceTick;sample.snapshotTick=tick;sample.version=slot->info.localVersion;
        if(tick<slot->info.sourceTick){sample.status=Status::TickMismatch;return sample;}
        if(!contains(slot->info.originFine,slot->spec.dimensions,p)){sample.status=Status::OutsideRegion;return sample;}
        sample.value=value->values[localIndex(p,slot->info.originFine,slot->spec.dimensions)];sample.ageTicks=tick-slot->info.sourceTick;
        sample.status=sample.ageTicks>slot->spec.maxAgeTicks?Status::Stale:Status::Ok;return sample;
    };
    if(preferred==Representation::SharedProxy&&slot->hasWriter) {
        auto shared=world_.sampleSource(slot->writer,id,slot->spec.source,coarse(p,slot->spec.finePerWorld),tick);
        // A moved LEF cannot use its old projected footprint while waiting for
        // the new epoch. Always choose exactly one representation per source.
        if(shared.valid()&&(!slot->info.pending||!slot->hasPendingWriter)) {out.representation=Representation::SharedProxy;out.sample=shared;return out;}
    }
    out.sample=local();if(out.sample.valid())out.representation=Representation::LocalDetail;return out;
}
std::vector<ObservationResult> LocalFields::observe(const std::vector<Observation>& observations,Tick tick,const PromotionPolicy& policy) {
    if(policy.demoteAt<0||policy.promoteAt<=policy.demoteAt||!policy.promotionsPerTick||observations.size()>limits_.maxInstances||
        (hasObservationTick_&&tick<observationTick_))throw std::invalid_argument("NG-E-LEF-OBSERVATION");
    std::set<std::pair<std::uint32_t,std::uint32_t>> seen;
    for(const auto& o:observations)if(o.concentration<0||!seen.emplace(o.handle.slot,o.handle.generation).second)throw std::invalid_argument("NG-E-LEF-OBSERVATION");
    if(!hasObservationTick_||tick!=observationTick_){hasObservationTick_=true;observationTick_=tick;promotionsThisTick_=0;}
    auto ordered=observations;std::stable_sort(ordered.begin(),ordered.end(),[&](const auto& a,const auto& b){
        if(a.priority!=b.priority)return a.priority>b.priority;
        const auto* sa=find(a.handle);const auto* sb=find(b.handle);
        if(static_cast<bool>(sa)!=static_cast<bool>(sb))return sa!=nullptr;
        if(sa&&sb)return sa->spec.source<sb->spec.source;
        return std::tie(a.handle.slot,a.handle.generation)<std::tie(b.handle.slot,b.handle.generation);
    });
    std::vector<ObservationResult> output;
    for(const auto& o:ordered) {
        auto* slot=find(o.handle);LocalResult response=result(LocalCode::NoChange);
        if(!slot)response=result(LocalCode::InvalidHandle);
        else if(slot->info.pending)response=result(LocalCode::Busy);
        else if(tick<slot->info.stateSince)response=result(LocalCode::Invalid,"NG-E-LEF-TIME");
        else if(slot->info.state==LocalState::LocalOnly&&(o.worldVisible||o.subscribers||o.concentration>=policy.promoteAt)&&tick-slot->info.stateSince>=policy.minimumLocalTicks) {
            if(promotionsThisTick_>=policy.promotionsPerTick)response=result(LocalCode::Capacity,"NG-E-LEF-PROMOTION-QUOTA");
            else {response=promote(o.handle,tick);if(response.code==LocalCode::Accepted)++promotionsThisTick_;}
        } else if(slot->info.state==LocalState::Shared&&!o.worldVisible&&!o.subscribers&&o.concentration<=policy.demoteAt&&tick-slot->info.stateSince>=policy.minimumSharedTicks)response=demote(o.handle,tick);
        output.push_back({o.handle,std::move(response)});
    }
    return output;
}
LocalStats LocalFields::stats() const {
    LocalStats out;out.localSamples=localSamples_;out.acceptedProjections=projections_;out.rejectedProjections=rejections_;
    for(const auto& slot:slots_)if(slot.active){++out.instances;if(slot.info.state==LocalState::Shared)++out.shared;if(slot.info.pending)++out.pending;}
    return out;
}
} // namespace engine::fields
