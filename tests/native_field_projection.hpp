#pragma once
#include "../engine/native/fields/local.hpp"

namespace {
Channel visualDensity() {return {"density","fixed-density","smoke",Domain::VisualReference,Merge::Sum,0,1000,1000};}
LocalSpec localSpec(std::uint64_t source,Cell origin={0,0,0}) {
    LocalSpec spec;spec.owner="smoke";spec.source=source;spec.originFine=origin;spec.dimensions={2,2,2};
    spec.channels={{0,ProjectionReduction::VolumeAverage}};return spec;
}
LocalHandle createLocal(LocalFields& fields,LocalSpec spec) {
    const auto created=fields.create(std::move(spec));check(created.result.accepted(),"LEF creation failed");return created.handle;
}
void fillLocal(LocalFields& fields,LocalHandle handle,int value,Tick tick,ChannelId channel=0,std::size_t n=8) {
    check(fields.replace(handle,{{channel,std::vector<std::int32_t>(n,value)}},tick).accepted(),"LEF replacement failed");
}
Lease visualReader(World& world,Region region={{0,0,0},{1,1,1}},std::vector<ChannelId> channels={0}) {
    LeaseRequest r;r.owner="consumer";r.region=region;r.channels=std::move(channels);r.consistency=Consistency::Stale;r.maxPages=32;
    return acquire(world,std::move(r));
}
void atomicFieldBatches() {
    World world(8,2);world.registerChannel(visualDensity());
    world.registerChannel({"emission","fixed","smoke",Domain::VisualReference,Merge::Maximum,0,1000,1});
    auto r=request({{0,0,0},{16,1,1}});r.channels={0,1};auto writer=acquire(world,r);
    Publication a{writer,0,1,1,1,1,Domain::VisualReference,{{{0,0,0},100}}};
    Publication b{writer,1,1,1,1,1,Domain::VisualReference,{{{0,0,0},2000}}};
    check(world.publishAtomic({a,b})==PublishResult::Invalid,"invalid second channel partially accepted batch");
    check(world.stats().publications==0&&world.stats().sourceRecords==0,"failed batch leaked its first channel");
    check(world.commit(1)&&!world.sampleSource(writer,0,1,{0,0,0},1).valid(),"failed batch leaked into committed view");
    a.sourceTick=a.publishTick=2;b.sourceTick=b.publishTick=2;b.writes[0].value=200;
    check(world.publishAtomic({a,b})==PublishResult::Accepted,"valid multi-channel batch rejected");
    check(!world.sourceView(0,1).exists,"staged source metadata leaked before commit");
    check(world.commit(2),"multi-channel commit failed");
    check(world.sampleSource(writer,0,1,{0,0,0},2).value==100&&world.sampleSource(writer,1,1,{0,0,0},2).value==200,"channels not published together");
    check(world.publishAtomic({a,b})==PublishResult::Duplicate,"retransmitted multi-channel batch not idempotent");
    a.epoch=2;a.sourceTick=a.publishTick=3;a.writes[0].value=400;
    b.source=2;b.sourceTick=b.publishTick=3;
    check(world.publishAtomic({a,b})==PublishResult::CapacityExceeded,"batch ignored source-record budget");
    check(world.commit(3),"unchanged snapshot commit failed");
    check(world.sampleSource(writer,0,1,{0,0,0},3).value==100&&world.sourceView(0,1).epoch==1,"capacity failure changed earlier batch entry");
    a.sourceTick=2;a.publishTick=10;a.writes.clear();
    check(world.publishAt(a)==PublishResult::Accepted,"future withdrawal rejected");
    check(!world.commit(9),"empty withdrawal bypassed future publication barrier");
    check(world.sampleSource(writer,0,1,{0,0,0},3).value==100,"future withdrawal changed old snapshot");
    check(world.commit(10)&&!world.sampleSource(writer,0,1,{0,0,0},10).valid(),"withdrawal did not commit");
    check(world.sourceView(0,1).exists&&!world.sourceView(0,1).hasContribution,"withdrawal acknowledgement missing");
    check(world.publishAtomic({})==PublishResult::Invalid,"empty atomic operation accepted");
    ++groups;
}
void lateFieldPublication() {
    World world;world.registerChannel(density());auto writer=acquire(world);
    auto eventual=acquire(world,request({{0,0,0},{1,1,1}},false));
    auto strong=acquire(world,request({{0,0,0},{1,1,1}},false,Consistency::Strong));
    Publication p{writer,0,1,1,5,8,Domain::CpuAuthority,{{{0,0,0},80}}};
    check(world.publishAt(p)==PublishResult::Accepted&&!world.commit(7)&&world.commit(8),"source/publish clock barrier incorrect");
    const auto view=world.sourceView(0,1);
    check(view.sourceTick==5&&view.publishTick==8&&view.snapshotTick==8&&view.resident,"source and publication times conflated");
    const auto sample=world.sampleSource(eventual,0,1,{0,0,0},8);
    check(sample.valid()&&sample.ageTicks==3&&sample.sourceEpoch==1,"delayed source age/version incorrect");
    check(world.sampleSource(strong,0,1,{0,0,0},8).status==Status::Stale,"Strong disguised old source as fresh");
    check(world.sampleSource(eventual,0,1,{0,0,0},9).status==Status::Stale,"Eventual ignored delayed source age");
    p.epoch=2;p.sourceTick=4;p.publishTick=9;
    check(world.publishAt(p)==PublishResult::PastTick,"source time regressed with newer publication");
    p.sourceTick=10;
    check(world.publishAt(p)==PublishResult::Invalid,"source newer than publication accepted");
    ++groups;
}
void localFieldCreation() {
    World world;world.registerChannel(visualDensity());world.registerChannel({"hazard","fixed","smoke",Domain::CpuAuthority,Merge::Sum,0,1000,1});
    world.registerChannel({"maximum","fixed","smoke",Domain::VisualReference,Merge::Maximum,0,1000,1});
    LocalFields fields(world,{2,64});auto a=createLocal(fields,localSpec(1));
    check(world.stats().activePages==0&&fields.stats().localSamples==8,"unobserved LEF allocated WF pages");
    const auto zero=fields.sample(a,0,{0,0,0},0);
    check(zero.sample.valid()&&zero.sample.value==0&&zero.representation==Representation::LocalDetail,"private known zero not declared");
    check(!fields.create(localSpec(1)).result.accepted(),"source ID reused");
    auto bad=localSpec(2);bad.finePerWorld=3;check(!fields.create(bad).result.accepted(),"unsupported scale accepted");
    bad=localSpec(2);bad.channels[0].channel=1;check(!fields.create(bad).result.accepted(),"visual LEF acquired CPU authority");
    bad.channels[0].channel=2;check(!fields.create(bad).result.accepted(),"density averaging accepted a maximum channel");
    bad=localSpec(2);bad.channels[0].reduction=static_cast<ProjectionReduction>(99);check(!fields.create(bad).result.accepted(),"unknown reduction accepted");
    auto maximum=localSpec(2);maximum.channels={{2,ProjectionReduction::Maximum}};auto b=createLocal(fields,maximum);
    check(fields.create(localSpec(3)).result.code==LocalCode::Capacity,"instance budget ignored");
    check(fields.retire(a,0).accepted()&&fields.retire(b,0).accepted()&&fields.stats().localSamples==0,"local-only retirement leaked samples");
    check(fields.move(a,{1,0,0},1).code==LocalCode::InvalidHandle,"retired local handle modified replacement");
    ++groups;
}
void localReductionAndBarrier() {
    World world;world.registerChannel(visualDensity());world.registerChannel({"emission","fixed","smoke",Domain::VisualReference,Merge::Maximum,0,1000,1});
    LocalFields fields(world);auto spec=localSpec(1,{-2,0,0});spec.dimensions={4,2,2};spec.channels.push_back({1,ProjectionReduction::Maximum});
    auto h=createLocal(fields,spec);std::vector<std::int32_t> maxima;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<4;++x)maxima.push_back(x+10*y+100*z);
    check(fields.replace(h,{{1,maxima},{0,std::vector<std::int32_t>(16,125)}},5).accepted(),"unordered local channels not canonicalized");
    const auto version=fields.inspect(h).localVersion;
    check(!fields.replace(h,{{0,std::vector<std::int32_t>(16,2000)},{1,maxima}},6).accepted()&&fields.inspect(h).localVersion==version,"invalid local payload partially applied");
    check(fields.promote(h,8).accepted()&&fields.inspect(h).state==LocalState::PromotePending,"promotion did not enter pending state");
    check(fields.settle(h).code==LocalCode::Busy&&!world.sourceView(0,1).exists,"promotion acknowledged before publication barrier");
    check(fields.retire(h,8).code==LocalCode::Busy,"in-flight promotion retired without a barrier");
    check(!world.commit(7)&&world.commit(8)&&fields.settle(h).accepted(),"projection barrier failed");
    const auto info=fields.inspect(h);
    check(info.state==LocalState::Shared&&info.acknowledgedEpoch==1,"shared state/epoch missing");
    check(info.projection[0].coarseSamples==2&&info.projection[0].inputIntegral==2000&&info.projection[0].quantizationResidual==0,"density reduction lost integral");
    const auto d=fields.sample(h,0,{-1,0,0},8,Representation::SharedProxy);
    const auto m=fields.sample(h,1,{-1,0,0},8,Representation::SharedProxy);
    check(d.representation==Representation::SharedProxy&&d.sample.value==125&&d.sample.sourceEpoch==1,"negative coarse cell projection incorrect");
    check(m.sample.value==111&&m.sample.sourceEpoch==d.sample.sourceEpoch,"maximum reduction or channel version differs");
    check(fields.sample(h,0,{-1,0,0},9,Representation::SharedProxy).sample.status==Status::Stale,"stale projection silently successful");
    check(fields.retire(h,9).accepted()&&world.commit(9)&&fields.settle(h).accepted()&&!fields.valid(h),"shared retirement did not wait for withdrawal commit");
    world.collect(0);check(world.stats().committedBytes==0&&world.stats().sourceRecords==0,"two-channel retirement leaked WF state");
    ++groups;
}
void projectionQuantization() {
    World world;world.registerChannel(visualDensity());LocalFields fields(world);auto h=createLocal(fields,localSpec(1,{-1,-1,-1}));fillLocal(fields,h,1,1);
    check(fields.promote(h,1).accepted()&&world.commit(1)&&fields.settle(h).accepted(),"partial coarse coverage rejected");
    const auto metric=fields.inspect(h).projection.front();
    check(metric.coarseSamples==8&&metric.inputIntegral==8&&metric.reconstructedIntegral==0&&metric.quantizationResidual==8,"quantization residual hidden or wrong");
    check(fields.sample(h,0,{-1,-1,-1},1).sample.value==1&&fields.sample(h,0,{-1,-1,-1},1,Representation::SharedProxy).sample.value==0,"local detail or rounded coarse value incorrect");
    check(fields.retire(h,2).accepted()&&world.commit(2)&&fields.settle(h).accepted(),"quantized source cleanup failed");world.collect(0);
    ++groups;
}
void distantLocalMove() {
    World world(2);world.registerChannel(visualDensity());LocalFields fields(world);auto spec=localSpec(1);spec.maxWorldPages=1;
    auto h=createLocal(fields,spec);fillLocal(fields,h,100,1);
    check(fields.promote(h,1).accepted()&&world.commit(1)&&fields.settle(h).accepted(),"initial move source failed");
    auto reader=visualReader(world);
    check(fields.move(h,{32000,0,0},2).accepted(),"far movement required allocating intervening space");
    check(world.stats().activePages==2&&world.sourceView(0,1).epoch==1,"move changed committed epoch or allocated gap pages");
    check(world.sampleSource(reader,0,1,{0,0,0},1).value==100,"move withdrew old footprint before commit");
    const auto routed=fields.sample(h,0,{32000,0,0},2,Representation::SharedProxy);
    check(routed.representation==Representation::LocalDetail&&routed.sample.value==100,"pending move rendered stale WF instead of local source");
    check(world.commit(2)&&!world.sampleSource(reader,0,1,{0,0,0},2).valid(),"old footprint survived atomic move");
    check(fields.settle(h,23).accepted()&&world.sourceView(0,1).epoch==3,"move takeover epoch not acknowledged");
    check(fields.sample(h,0,{32000,0,0},2,Representation::SharedProxy).representation==Representation::SharedProxy,"new projected footprint unavailable");
    check(fields.move(h,{0,0,0},1).code==LocalCode::Invalid,"old transform tick accepted");
    check(fields.move(h,{std::numeric_limits<std::int32_t>::max(),0,0},3).code==LocalCode::Invalid,"overflowing local bounds accepted");
    check(world.release(reader,7),"old reader release failed");world.collect(22);
    check(world.stats().retiredPages==1,"move reclaimed old shared page before maximum fence");world.collect(23);
    check(world.stats().retiredPages==0&&world.stats().activePages==1,"move did not reclaim completed old page");
    check(fields.retire(h,3).accepted()&&world.commit(3)&&fields.settle(h).accepted(),"moved source retirement failed");world.collect(23);
    check(world.stats().committedBytes==0,"moved source leaked pages");++groups;
}
void failedLocalMove() {
    World world(1,1);world.registerChannel(visualDensity());LocalFields fields(world);auto h=createLocal(fields,localSpec(1));fillLocal(fields,h,100,1);
    check(fields.promote(h,1).accepted()&&world.commit(1)&&fields.settle(h).accepted(),"budget source setup failed");
    const auto before=fields.inspect(h);const auto publications=world.stats().publications;
    check(fields.move(h,{32,0,0},2).code==LocalCode::Capacity,"move ignored simultaneous old/new page residency");
    const auto after=fields.inspect(h);
    check(after.originFine==before.originFine&&after.localVersion==before.localVersion&&!after.pending&&world.stats().publications==publications,"failed move changed local or shared state");
    check(fields.sample(h,0,{0,0,0},2,Representation::SharedProxy).sample.value==100,"failed move destroyed old projection");
    fillLocal(fields,h,50,2);
    check(fields.project(h,2).accepted()&&world.commit(2)&&fields.settle(h).accepted(),"replacement incorrectly required an extra source tile");
    check(fields.sample(h,0,{0,0,0},2,Representation::SharedProxy).sample.value==50,"projection update accumulated instead of replacing");
    check(fields.project(h,2).code==LocalCode::PublicationRejected&&!fields.inspect(h).pending,"committed tick rewritten by projection");
    check(fields.retire(h,3).accepted()&&world.commit(3)&&fields.settle(h).accepted(),"budget source cleanup failed");world.collect(0);++groups;
}
void exclusiveSourceRouting() {
    World world;world.registerChannel(visualDensity());LocalFields fields(world);auto a=createLocal(fields,localSpec(1)),b=createLocal(fields,localSpec(2));
    fillLocal(fields,a,700,1);fillLocal(fields,b,700,1);
    check(fields.promote(a,1).accepted()&&fields.promote(b,1).accepted()&&world.commit(1)&&fields.settle(a).accepted()&&fields.settle(b).accepted(),"two source projection failed");
    auto reader=visualReader(world);const auto combined=world.sample(reader,0,{0,0,0},1);
    check(combined.value==1000&&combined.clamped,"shared source saturation incorrect");
    for(auto h:{a,b}) {
        const auto local=fields.sample(h,0,{0,0,0},1),shared=fields.sample(h,0,{0,0,0},1,Representation::SharedProxy);
        check(local.representation==Representation::LocalDetail&&local.sample.value==700,"local source sampled aggregate");
        check(shared.representation==Representation::SharedProxy&&shared.sample.value==700,"WF source sampled aggregate or counted local twice");
    }
    check(fields.retire(a,2).accepted()&&world.commit(2)&&fields.settle(a).accepted(),"first source withdrawal failed");
    check(world.sample(reader,0,{0,0,0},2).value==700,"source withdrawal subtracted from saturated aggregate");
    auto c=createLocal(fields,localSpec(3));check(!fields.valid(a)&&c.generation!=a.generation,"slot reuse accepted stale handle");
    check(fields.retire(c,2).accepted()&&fields.retire(b,3).accepted()&&world.commit(3)&&fields.settle(b).accepted(),"remaining sources cleanup failed");
    check(world.release(reader),"source reader cleanup failed");world.collect(0);
    check(world.stats().committedBytes==0&&world.stats().sourceRecords==0&&!fields.create(localSpec(1)).result.accepted(),"source lifetime leaked or ID reused");++groups;
}
void localObservationPolicy() {
    World world(16,16);world.registerChannel(visualDensity());LocalFields fields(world);std::vector<LocalHandle> handles;
    for(unsigned i=0;i<6;++i){auto h=createLocal(fields,localSpec(i+1,{static_cast<std::int32_t>(32*i),0,0}));fillLocal(fields,h,100,1);handles.push_back(h);}
    PromotionPolicy policy;policy.minimumLocalTicks=2;policy.minimumSharedTicks=3;policy.promotionsPerTick=2;
    std::vector<Observation> observations={{handles[5],false,0,0,700},{handles[1],false,0,10,700},{handles[0],false,0,10,700},{handles[2],false,0,5,700},{handles[3],false,1,0,0},{handles[4],true,0,0,0}};
    fields.observe(observations,1,policy);check(fields.stats().pending==0,"minimum local residency ignored");
    fields.observe(observations,2,policy);
    check(fields.inspect(handles[0]).pending&&fields.inspect(handles[1]).pending&&!fields.inspect(handles[2]).pending,"stable priority/source quota not enforced");
    const auto repeated=fields.observe({{handles[2],false,0,99,700}},2,policy);
    check(repeated.front().result.code==LocalCode::Capacity,"repeated subscription call bypassed per-tick quota");
    check(world.commit(2)&&fields.settle(handles[0]).accepted()&&fields.settle(handles[1]).accepted(),"observed promotion commit failed");
    fields.observe({{handles[0],false,0,0,500}},3,policy);check(!fields.inspect(handles[0]).pending,"hysteresis middle band oscillated");
    fields.observe({{handles[0],false,0,0,0}},4,policy);check(!fields.inspect(handles[0]).pending,"minimum shared residency ignored");
    fields.observe({{handles[0],false,0,0,0},{handles[1],false,1,0,0}},5,policy);
    check(fields.inspect(handles[0]).state==LocalState::DemotePending&&fields.inspect(handles[1]).state==LocalState::Shared,"subscriber failed to retain shared coverage");
    check(world.commit(5)&&fields.settle(handles[0]).accepted(),"observed demotion failed");
    fields.observe({{handles[0],false,0,0,700}},6,policy);check(!fields.inspect(handles[0]).pending,"demotion immediately promoted again");
    fillLocal(fields,handles[0],100,7);fields.observe({{handles[0],true,0,0,0}},7,policy);
    check(fields.inspect(handles[0]).pending&&world.commit(7)&&fields.settle(handles[0]).accepted(),"worldVisible did not promote after residency");
    const auto before=fields.stats().acceptedProjections;
    throws([&]{fields.observe({{handles[0]},{handles[0]}},8,policy);},"duplicate subscriptions accepted");
    check(fields.stats().acceptedProjections==before,"invalid observation batch mutated state");
    const auto invalid=fields.observe({{{999,999},false,0,0,700},{handles[0],true,0,0,0}},8,policy);
    check(invalid.back().result.code==LocalCode::InvalidHandle,"invalid observer handle not reported");
    for(auto h:handles)check(fields.retire(h,8).accepted(),"policy source retirement failed");
    check(world.commit(8),"policy withdrawal commit failed");for(auto h:handles)if(fields.valid(h))check(fields.settle(h).accepted(),"policy source settle failed");world.collect(0);
    check(world.stats().committedBytes==0&&fields.stats().localSamples==0,"policy lifecycle leaked resources");++groups;
}
void deterministicLocalPromotion() {
    std::uint64_t authority=0;
    for(unsigned run=0;run<100;++run) {
        World world;world.registerChannel({"hazard","fixed","authority",Domain::CpuAuthority,Merge::Sum,0,1000,1});const auto visual=world.registerChannel(visualDensity());
        auto r=request({{0,0,0},{1,1,1}});r.owner="authority";auto writer=acquire(world,r);
        check(world.publish(writer,0,99,1,2,Domain::CpuAuthority,{{{0,0,0},77}})==PublishResult::Accepted,"authority setup failed");
        LocalFields fields(world);std::vector<LocalHandle> handles;std::vector<Observation> observations;
        for(unsigned i=0;i<8;++i){auto spec=localSpec(i+1);spec.channels[0].channel=visual;auto h=createLocal(fields,spec);fillLocal(fields,h,100+i,2,visual);handles.push_back(h);observations.push_back({h,false,1,5,0});}
        std::rotate(observations.begin(),observations.begin()+run%8,observations.end());if(run%2)std::reverse(observations.begin(),observations.end());
        PromotionPolicy policy;policy.minimumLocalTicks=0;fields.observe(observations,2,policy);
        check(world.commit(2),"deterministic projection commit failed");
        for(unsigned i=0;i<8;++i){if(i<4)check(fields.settle(handles[i]).accepted(),"deterministic source settle failed");check(world.sourceView(visual,i+1).exists==(i<4),"subscriber order changed promoted source selection");}
        if(!run)authority=world.digest();check(world.digest()==authority,"visual subscription order changed authority digest");
        for(auto h:handles)check(fields.retire(h,3).accepted(),"deterministic source retirement failed");
        check(world.commit(3),"deterministic withdrawal commit failed");for(auto h:handles)if(fields.valid(h))check(fields.settle(h).accepted(),"deterministic source cleanup failed");
        world.release(writer);world.collect(0);check(world.stats().committedBytes==0,"deterministic run leaked pages");
    }
    ++groups;
}
void localProjectionCycles() {
    World world(1,1);world.registerChannel(visualDensity());LocalFields fields(world,{1,8});LocalHandle previous{};
    for(unsigned i=1;i<=1000;++i) {
        auto h=createLocal(fields,localSpec(i));check(!fields.valid(previous),"projection cycle reused stale generation");fillLocal(fields,h,i%1000,2*i-1);
        check(fields.promote(h,2*i-1).accepted()&&world.commit(2*i-1)&&fields.settle(h).accepted(),"projection cycle promotion failed");
        check(fields.retire(h,2*i).accepted()&&world.commit(2*i)&&fields.settle(h).accepted(),"projection cycle retirement failed");world.collect(0);
        check(world.stats().committedBytes==0&&world.stats().sourceRecords==0&&fields.stats().localSamples==0,"projection cycle leaked payload or ledger");previous=h;
    }
    ++groups;
}
void persistentSourceDoesNotPinRetiredEmitters() {
    World world(1,2);world.registerChannel(visualDensity());LocalFields fields(world,{2,16});
    const auto persistent=createLocal(fields,localSpec(1));fillLocal(fields,persistent,100,1);
    check(fields.promote(persistent,1).accepted()&&world.commit(1)&&fields.settle(persistent).accepted(),"persistent source setup failed");
    for(unsigned i=0;i<1000;++i) {
        auto h=createLocal(fields,localSpec(i+2));fillLocal(fields,h,100,2*i+2);
        check(fields.promote(h,2*i+2).accepted()&&world.commit(2*i+2)&&fields.settle(h).accepted(),"persistent writer pinned previous emitter tombstones");
        check(fields.retire(h,2*i+3).accepted()&&world.commit(2*i+3)&&fields.settle(h).accepted(),"short source withdrawal failed");
        world.collect(0);
        check(world.stats().sourceRecords==1&&world.stats().sourceTiles==1&&fields.stats().localSamples==8,"long-lived source retained unrelated dead source records");
    }
    check(fields.retire(persistent,2002).accepted()&&world.commit(2002)&&fields.settle(persistent).accepted(),"persistent source cleanup failed");world.collect(0);
    check(world.stats().committedBytes==0&&world.stats().sourceRecords==0,"persistent-source stress leaked payload");++groups;
}
void projectionTests() {
    atomicFieldBatches();lateFieldPublication();localFieldCreation();localReductionAndBarrier();projectionQuantization();distantLocalMove();failedLocalMove();exclusiveSourceRouting();localObservationPolicy();deterministicLocalPromotion();localProjectionCycles();persistentSourceDoesNotPinRetiredEmitters();
}
}
