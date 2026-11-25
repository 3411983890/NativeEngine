#include "../engine/native/effects/runtime.hpp"
#include "../engine/native/simulation/clock.hpp"
#include "../third_party/json.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <functional>
#include <limits>

using namespace engine::effects;
using Json=nlohmann::json;
namespace {
unsigned checks=0,groups=0;
void check(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void throws(F action,const char* message) { bool threw=false; try { action(); } catch (const std::exception&) { threw=true; } check(threw,message); }
std::shared_ptr<const Asset> fixture;
std::shared_ptr<const Asset> changed(std::function<void(Asset&)> mutate) {
    auto asset=std::make_shared<Asset>(*fixture); mutate(*asset); return asset;
}
void hashesAndAssets(const Library& library,const std::filesystem::path& directory) {
    check(sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA empty reference failed");
    check(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA abc reference failed");
    check(library.assets.size()>=2,"library assets missing");
    check(fixture->emitters.size()==1&&fixture->emitters[0].modules.size()==3,"source module contract not loaded");
    std::ifstream stream(directory/(fixture->hash+".ngeffect.json")); Json document; stream>>document;
    document["payloadJson"]=document["payloadJson"].get<std::string>()+" ";
    throws([&] {parseCooked(document.dump());},"payload tampering not rejected");
    document["schema"]="nativegpu.effect-cook/1";
    throws([&] {parseCooked(document.dump());},"obsolete cook silently accepted");
    Runtime runtime; runtime.install(library.assets.at("smoke_reference"));
    auto unsupported=runtime.spawn("smoke_reference");
    check(unsupported.status==SpawnStatus::UnsupportedFeature&&!unsupported.reason.empty(),"unsupported field effect silently activated");
    ++groups;
}
void emissionAndMovement() {
    Runtime runtime; runtime.install(changed([](Asset& a) {
        a.emitters[0].shape=Shape::Point; a.emitters[0].lifeMin=a.emitters[0].lifeMax=120;
    }));
    auto spawned=runtime.spawn(fixture->id,{2,3,4},17);
    check(spawned.status==SpawnStatus::Accepted,"spawn failed");
    check(runtime.stats().alive==0,"spawn emitted outside simulation tick");
    runtime.step();
    check(runtime.stats().alive==96&&runtime.stats().emitted==96,"burst zero did not emit exactly once");
    const auto first=runtime.sprites().front();
    check(first.position.x==2&&first.position.y==3&&first.position.z==4,"world origin not applied");
    runtime.step();
    const auto moved=runtime.sprites().front();
    const float expected=3+(4-9.81f/120)/120;
    check(std::abs(moved.position.y-expected)<1e-6f,"semi-implicit integration differs from analytic first step");
    check(moved.color[3]<1&&moved.color[3]>0,"lifetime color curve not evaluated");
    check(runtime.stats().emitted==96,"one-shot burst repeated");
    for (int i=0;i<120;++i) runtime.step();
    check(runtime.stats().alive==0&&runtime.stats().expired==96&&!runtime.valid(spawned.handle),"finite effect did not drain/retire");
    ++groups;
}
void rateAndFps() {
    auto rate=changed([](Asset& a) {
        a.duration=120; a.emitters[0].bursts.clear(); a.emitters[0].rateMilli=120500;
        a.emitters[0].lifeMin=a.emitters[0].lifeMax=240;
    });
    std::uint64_t reference=0;
    for (unsigned fps:{30,60,144,240}) {
        Runtime runtime; runtime.install(rate); runtime.spawn(rate->id,{},77);
        engine::simulation::Clock clock; std::uint64_t last=0;
        for (unsigned frame=1;frame<=fps;++frame) {
            const auto end=1000000000ull*frame/fps; clock.addNanoseconds(end-last); last=end;
            while (clock.consumeTick()) runtime.step();
        }
        check(runtime.stats().emitted==120&&runtime.stats().tick==120,"rational rate lost particles or ticks");
        if (!reference) reference=runtime.digest();
        check(runtime.digest()==reference,"render fps affected native particle state");
    }
    ++groups;
}
void pauseMoveStopAndGenerations() {
    auto local=changed([](Asset& a) {a.emitters[0].shape=Shape::Point;a.emitters[0].localSpace=true;});
    Runtime runtime(1000,1); runtime.install(local);
    auto a=runtime.spawn(local->id,{1,0,0}); runtime.step();
    check(runtime.pause(a.handle,true),"pause failed"); const auto y=runtime.sprites().front().position.y;
    for (int i=0;i<100;++i) runtime.step();
    check(runtime.stats().alive==96&&runtime.sprites().front().position.y==y,"paused particles advanced");
    check(runtime.move(a.handle,{4,0,0}),"move failed");
    check(runtime.sprites().front().position.x==4,"local-space particles did not follow origin");
    check(runtime.spawn(local->id).status==SpawnStatus::Capacity,"instance limit ignored");
    check(runtime.stop(a.handle,true)&&runtime.stats().alive==0&&runtime.stats().killed==96,"immediate stop leaked particles");
    auto b=runtime.spawn(local->id); check(b.status==SpawnStatus::Accepted,"retired instance slot unavailable");
    check(!runtime.stop(a.handle,true)&&!runtime.pause(a.handle,false)&&!runtime.move(a.handle,{}),"stale handle modified replacement");
    check(!runtime.move(b.handle,{std::numeric_limits<float>::quiet_NaN(),0,0}),"NaN origin accepted");
    runtime.step(); runtime.stop(b.handle); for(int i=0;i<100;++i)runtime.step();
    check(runtime.stats().alive==0&&!runtime.valid(b.handle),"drain stop never retired");
    ++groups;
}
void budgetsAndStress() {
    Runtime limited(20,2); limited.install(fixture); limited.spawn(fixture->id); limited.step();
    check(limited.stats().alive==20&&limited.stats().dropped==76,"global particle budget not enforced/reported");
    Runtime emitLimited; emitLimited.install(changed([](Asset& a) {a.maxSpawnPerTick=7;}));
    emitLimited.spawn(fixture->id); emitLimited.step();
    check(emitLimited.stats().alive==7&&emitLimited.stats().dropped==89,"spawn-per-tick budget not enforced");
    Runtime many(20000,128); many.install(fixture);
    std::vector<Handle> handles;
    for(int i=0;i<128;++i) {auto spawn=many.spawn(fixture->id,{static_cast<float>(i),0,0});check(spawn.status==SpawnStatus::Accepted,"128 instance spawn rejected");handles.push_back(spawn.handle);}
    many.step(); check(many.stats().alive==128*96&&many.stats().instances==128,"mixed instance pools lost particles");
    for(auto h:handles)check(many.stop(h,true),"stress stop failed");
    check(many.stats().alive==0&&many.stats().instances==0,"stress stop leaked instances");
    ++groups;
}
void reloadAndSnapshotIsolation() {
    Runtime runtime; runtime.install(fixture); auto handle=runtime.spawn(fixture->id).handle; runtime.step();
    const auto before=runtime.sprites();
    auto compatible=changed([](Asset& a) {a.revision=1;a.hash="updated-parameters";a.emitters[0].modules[0].vector.y=9;});
    check(runtime.reload(compatible)==ReloadResult::Preserved,"compatible reload rejected");
    check(runtime.valid(handle)&&runtime.stats().alive==96,"compatible reload reset active particles");
    auto incompatible=std::make_shared<Asset>(*compatible); incompatible->revision=2; incompatible->hash="updated-layout"; incompatible->layoutHash="different"; incompatible->maxAlive=128;
    check(runtime.reload(incompatible)==ReloadResult::ResetRequired&&runtime.stats().alive==96,"incompatible reload mutated live state before reset consent");
    check(runtime.reload(incompatible,true)==ReloadResult::Reset&&runtime.stats().alive==0&&runtime.valid(handle),"explicit reload reset failed");
    throws([&] {runtime.reload(fixture);},"old asset revision replaced new state");
    Runtime isolated; auto mutableAsset=std::make_shared<Asset>(*fixture); isolated.install(mutableAsset);
    mutableAsset->emitters[0].bursts[0].count=1; isolated.spawn(fixture->id); isolated.step();
    check(isolated.stats().alive==96,"external typed asset mutation changed installed snapshot");
    ++groups;
}
void productionLibrary(const Library& library) {
    const char* required[]={"spark_reference","sparks_burst","explosion_core","muzzle_flash","campfire_loop",
                            "steam_vent","dust_puff","blood_spray","rain_loop","snow_loop","leaves_drift"};
    for (const auto* id:required) check(library.assets.count(id)==1,"production effect asset missing");
    check(library.assets.size()>=11,"production effect library shrank");
    // Every authored sprite effect must actually be spawnable; a library of assets the
    // runtime refuses is not a deliverable.
    Runtime catalogue(200000,128);
    for (const auto& entry:library.assets) catalogue.install(entry.second);
    std::vector<Handle> handles;
    for (const auto* id:required) {
        const auto spawned=catalogue.spawn(id,{0,0,0});
        check(spawned.status==SpawnStatus::Accepted,(std::string("production asset rejected: ")+id).c_str());
        handles.push_back(spawned.handle);
    }
    for (int step=0;step<60;++step) catalogue.step();
    bool finite=true;
    for (const auto& sprite:catalogue.sprites())
        finite=finite&&std::isfinite(sprite.position.x)&&std::isfinite(sprite.position.y)&&std::isfinite(sprite.position.z);
    check(catalogue.stats().alive>0&&finite,"production library produced no live finite particles");
    // Short effects retire on their own before the loop ends; only live handles must stop.
    for (auto handle:handles) if (catalogue.valid(handle)) check(catalogue.stop(handle,true),"production effect did not stop");
    // Children dispatched by events outlive their parent by design; clear() is the
    // teardown path that must leave nothing behind.
    catalogue.clear();
    check(catalogue.stats().alive==0&&catalogue.stats().instances==0,"production library clear leaked particles");

    // The authored explosion must compose its sparks through the event graph.
    Runtime composed(200000,64);
    composed.install(library.assets.at("explosion_core"));
    composed.install(library.assets.at("sparks_burst"));
    check(composed.spawn("explosion_core").status==SpawnStatus::Accepted,"explosion spawn rejected");
    for (int step=0;step<240;++step) composed.step();
    check(composed.stats().subeffects>0,"explosion never dispatched its sparks");
    bool composedFinite=true;
    for (const auto& sprite:composed.sprites())
        composedFinite=composedFinite&&std::isfinite(sprite.position.x)&&std::isfinite(sprite.position.y)&&std::isfinite(sprite.position.z);
    check(composed.stats().events>0&&composedFinite,"explosion produced no finite events");
    const auto muzzle=library.assets.at("muzzle_flash");
    Runtime flash(20000,8);
    flash.install(muzzle); flash.install(library.assets.at("sparks_burst"));
    check(flash.spawn("muzzle_flash").status==SpawnStatus::Accepted,"muzzle flash spawn rejected");
    for (int step=0;step<4;++step) flash.step();
    check(flash.stats().subeffects==1,"muzzle flash did not emit exactly one spark burst");
    ++groups;
}
void eventsAndCollisions() {
    // Parent emits a child on its own spawn. Subeffects are dispatched at the next
    // tick boundary so the ordering never depends on how far the parent got this tick.
    auto parent=std::make_shared<Asset>(*fixture);
    parent->id="event_parent"; parent->hash="event-parent"; parent->maxDepth=2; parent->maxEventsPerTick=4;
    parent->rules={{EventKind::Spawn,"event_child",1,0}};
    auto child=std::make_shared<Asset>(*fixture);
    child->id="event_child"; child->hash="event-child"; child->maxDepth=1; child->maxEventsPerTick=4;
    child->rules={{EventKind::Spawn,"event_grandchild",1,0}};
    auto grandchild=std::make_shared<Asset>(*fixture);
    grandchild->id="event_grandchild"; grandchild->hash="event-grandchild"; grandchild->maxDepth=1; grandchild->maxEventsPerTick=4;
    Runtime runtime(200000,64);
    runtime.install(parent); runtime.install(child); runtime.install(grandchild);
    check(runtime.spawn("event_parent",{1,2,3}).status==SpawnStatus::Accepted,"event parent spawn rejected");
    check(runtime.stats().instances==1&&runtime.events().size()==1,"spawn event not recorded");
    check(runtime.events().front().kind==EventKind::Spawn&&runtime.events().front().depth==0,"spawn event identity");
    runtime.step();
    check(runtime.stats().instances==2&&runtime.stats().subeffects==1,"spawn rule did not dispatch a child");
    check(runtime.events().size()==2&&runtime.events().back().kind==EventKind::Spawn,"child spawn event missing");
    runtime.step();
    // The child tries to spawn a grandchild, but the child's own maxDepth is 1.
    check(runtime.stats().instances==2&&runtime.stats().eventsDropped>0,"depth budget did not stop recursion");
    runtime.clearEvents();
    check(runtime.events().empty(),"clearEvents left records behind");

    // A death storm is capped by maxEventsPerTick and reports every refused spawn.
    auto storm=std::make_shared<Asset>(*fixture);
    storm->id="event_storm"; storm->hash="event-storm"; storm->maxDepth=2; storm->maxEventsPerTick=2;
    storm->emitters[0].lifeMin=storm->emitters[0].lifeMax=1;
    storm->rules={{EventKind::Death,"event_child",5,0}};
    Runtime limited(200000,64);
    limited.install(storm); limited.install(child);
    limited.spawn("event_storm");
    for (int i=0;i<3;++i) limited.step();
    check(limited.stats().subeffects==2,"death storm exceeded maxEventsPerTick");
    check(limited.stats().eventsDropped>=90,"death storm drops were not reported");

    // Collision: a downward stream must bounce off a sphere and emit a normal.
    auto falling=std::make_shared<Asset>(*fixture);
    falling->id="event_falling"; falling->hash="event-falling";
    falling->emitters[0].shape=Shape::Point; falling->emitters[0].lifeMin=falling->emitters[0].lifeMax=240;
    falling->emitters[0].modules[0].vector={0,-4,0};
    Runtime physics(10000,8);
    physics.install(falling);
    physics.setColliders({{ColliderKind::Sphere,{0,0,0},{1,1,1},0.5f}});
    auto ball=physics.spawn("event_falling",{0,2,0});
    check(ball.status==SpawnStatus::Accepted,"falling spawn rejected");
    bool bounced=false;
    for (int step=0;step<180;++step) {
        physics.step();
        for (const auto& sprite:physics.sprites()) if (sprite.position.y>0.5f) bounced=true;
    }
    check(physics.stats().collisions>0,"particles passed through the collider");
    check(bounced,"collision did not reflect any particle");
    const auto& records=physics.events();
    const auto collision=std::find_if(records.begin(),records.end(),[](const Event& e){return e.kind==EventKind::Collision;});
    check(collision!=records.end(),"collision event not emitted");
    check(std::fabs(collision->position.y)<0.51f&&std::fabs(collision->position.x)<0.51f,"collision event outside the collider");
    check(std::fabs(collision->normal.y)>0.5f,"collision normal does not face the surface");
    throws([&] {physics.setColliders({{ColliderKind::Sphere,{0,0,0},{1,1,1},0}});},"zero-radius collider accepted");
    throws([&] {physics.setColliders({{ColliderKind::Sphere,{std::numeric_limits<float>::quiet_NaN(),0,0},{1,1,1},1}});},"NaN collider accepted");

    // Timeline markers fire once at the authored tick.
    auto timeline=std::make_shared<Asset>(*fixture);
    timeline->id="event_timeline"; timeline->hash="event-timeline"; timeline->maxDepth=2; timeline->maxEventsPerTick=2;
    timeline->rules={{EventKind::Timeline,"event_child",1,30}};
    Runtime clock(20000,8);
    clock.install(timeline); clock.install(child);
    clock.spawn("event_timeline");
    for (int step=0;step<31;++step) clock.step();
    check(clock.stats().subeffects==0,"timeline fired early");
    clock.step();
    check(clock.stats().subeffects==1,"timeline marker did not fire at its tick");
    for (int step=0;step<20;++step) clock.step();
    check(clock.stats().subeffects==1,"timeline marker repeated");

    // Threshold rules need a comparison value the schema does not carry; refuse them.
    auto threshold=std::make_shared<Asset>(*fixture);
    threshold->id="event_threshold"; threshold->hash="event-threshold"; threshold->maxDepth=1; threshold->maxEventsPerTick=1;
    threshold->rules={{EventKind::Threshold,"event_child",1,0}};
    Runtime refused; refused.install(threshold); refused.install(child);
    check(refused.spawn("event_threshold").status==SpawnStatus::UnsupportedFeature,"threshold rule silently accepted");

    std::uint64_t reference=0;
    for (unsigned run=0;run<100;++run) {
        Runtime repeat(200000,64);
        repeat.install(parent); repeat.install(child); repeat.install(grandchild);
        repeat.setColliders({{ColliderKind::Box,{0,0,0},{2,0.5f,2}}});
        repeat.spawn("event_parent",{0,1,0},5);
        for (int step=0;step<60;++step) repeat.step();
        if (!reference) reference=repeat.digest();
        check(repeat.digest()==reference,"100-run event graph diverged");
    }
    ++groups;
}
void determinism() {
    std::uint64_t reference=0;
    for (unsigned run=0;run<100;++run) {
        Runtime runtime; runtime.install(fixture); runtime.spawn(fixture->id,{2,1,-3},13); runtime.spawn(fixture->id,{-2,1,3},29);
        for(int i=0;i<50;++i)runtime.step();
        if(!reference)reference=runtime.digest();
        check(runtime.digest()==reference,"100-run CPU effect reference diverged");
        for(const auto& p:runtime.sprites())check(std::isfinite(p.position.x)&&std::isfinite(p.position.y)&&std::isfinite(p.position.z),"particle state not finite");
    }
    ++groups;
}
// Intensity is the only strength parameter a visual consumer may pass: it multiplies
// emission counts and must not move, resize or extend anything else.
void emissionIntensity() {
    auto burstWith=[](float intensity,std::uint64_t& emitted,float& firstY) {
        Runtime runtime; runtime.install(changed([](Asset& a) {
            a.emitters[0].shape=Shape::Point; a.emitters[0].lifeMin=a.emitters[0].lifeMax=120;
        }));
        SpawnParams params; params.intensity=intensity;
        const auto spawned=runtime.spawn(fixture->id,{2,3,4},17,params);
        check(spawned.status==SpawnStatus::Accepted,"intensity spawn rejected");
        runtime.step(); emitted=runtime.stats().emitted;
        firstY=runtime.sprites().empty()?0.0f:runtime.sprites().front().position.y;
    };
    std::uint64_t full=0,half=0,none=0;float fullY=0,halfY=0,noneY=0;
    burstWith(1.0f,full,fullY); burstWith(.5f,half,halfY); burstWith(0.0f,none,noneY);
    check(full==96,"full intensity keeps the authored burst");
    check(half==48,"half intensity halves the burst");
    check(none==0,"zero intensity emits nothing");
    check(fullY==3&&halfY==3,"intensity does not move the particles");
    // The clamp bounds one caller's claim on a shared pool. The dropped count is what
    // proves it: an unclamped 100x request would report 9472 rejects instead of 640.
    Runtime clamped; clamped.install(fixture); SpawnParams overshoot; overshoot.intensity=100.0f;
    check(clamped.spawn(fixture->id,{},1,overshoot).status==SpawnStatus::Accepted,"overshoot intensity rejected instead of clamped");
    clamped.step();
    const std::uint64_t requested=96ull*8ull;
    const std::uint64_t accepted=std::min<std::uint64_t>({requested,fixture->maxSpawnPerTick,fixture->maxAlive});
    check(clamped.stats().emitted==accepted&&clamped.stats().dropped==requested-accepted,"intensity is clamped before the pool budget");
    Runtime rejecting; rejecting.install(fixture); SpawnParams invalid; invalid.intensity=std::numeric_limits<float>::quiet_NaN();
    check(rejecting.spawn(fixture->id,{},1,invalid).status==SpawnStatus::InvalidParameter,"non-finite intensity accepted");
    // Rate emitters scale the phase increment, so a fractional intensity must not drift
    // over thousands of ticks the way a per-tick truncation would.
    auto rate=changed([](Asset& a){a.duration=120;a.emitters[0].bursts.clear();a.emitters[0].rateMilli=120000;a.emitters[0].lifeMin=a.emitters[0].lifeMax=240;});
    Runtime half_rate; half_rate.install(rate); SpawnParams halfParams; halfParams.intensity=.5f;
    check(half_rate.spawn(rate->id,{},3,halfParams).status==SpawnStatus::Accepted,"fractional rate spawn rejected");
    for(int i=0;i<120;++i)half_rate.step();
    check(half_rate.stats().emitted==60,"fractional intensity drifted the emission rate");
    ++groups;
}
}
int main(int argc,char** argv) {
    try {
        std::filesystem::path path="build/effects/library.json";
        if(argc==3&&std::string(argv[1])=="--library")path=argv[2];
        else if(argc!=1)throw std::runtime_error("Usage: native_effects_tests [--library path]");
        const auto library=loadLibrary(path.string(),"assets"); fixture=library.assets.at("spark_reference");
        hashesAndAssets(library,path.parent_path()); emissionAndMovement(); emissionIntensity(); rateAndFps(); pauseMoveStopAndGenerations(); budgetsAndStress(); reloadAndSnapshotIsolation(); productionLibrary(library); eventsAndCollisions(); determinism();
        std::cout<<"{\"schema\":\"nativegpu.effects-runtime-evidence/1\",\"ok\":true,\"groups\":"<<groups<<",\"checks\":"<<checks
                 <<",\"determinismRuns\":100,\"maxTestInstances\":128,\"simulationBackend\":\"cpu-reference\"}"<<std::endl;
    } catch(const std::exception& error) {std::cerr<<"Effects runtime failed after "<<checks<<" checks: "<<error.what()<<std::endl;return 1;}
}
