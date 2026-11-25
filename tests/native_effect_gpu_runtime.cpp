#include "../engine/native/effects/gpu_runtime.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

using namespace engine::effects;
using engine::Vec3;
using gpu_test::Rig;
namespace {
unsigned checks=0,groups=0,comparisons=0,stressSpawns=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void throws(F action,const char* why){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,why);}
Emitter emitter(std::string id="a") {Emitter e;e.id=id;e.lifeMin=e.lifeMax=3;Module m;m.vector={0,1,0};m.size={0.1f,0.2f};e.modules={m};e.renderers.push_back({"sprite","test.png","","",Blend::Additive,false});return e;}
std::shared_ptr<Asset> asset(std::string id="test") {auto a=std::make_shared<Asset>();a->id=id;a->hash=id;a->layoutHash=id;a->duration=4;a->maxAlive=5;a->maxSpawnPerTick=3;a->loop=true;a->emitters={emitter()};a->emitters[0].rateMilli=480000;return a;}
Handle spawn(GpuRuntime& runtime,const std::string& id="test",Vec3 origin={},std::uint64_t seed=0){auto result=runtime.spawn(id,origin,seed);require(result.status==SpawnStatus::Accepted,result.reason.c_str());return result.handle;}
void step(Rig& rig,GpuRuntime& runtime){rig.begin();std::string error;require(runtime.step(rig.commands.p,rig.ticket(),rig.fence.p->GetCompletedValue(),error),error.c_str());rig.submit();runtime.collect(rig.submitted);}
std::vector<GpuDrawBatch> draw(Rig& rig,GpuRuntime& runtime){rig.begin();std::string error;require(runtime.prepareDraw(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();return runtime.drawBatches();}
struct Particle {Vec3 position;std::uint32_t id=0,age=0,life=0;};
std::vector<Particle> particles(Rig& rig,const GpuDrawBatch& batch){
    const auto& b=batch.buffers;const std::size_t dataOffset=16+b.capacity*4ull,total=dataOffset+b.layout.bytes;
    gpu_test::Com<ID3D12Resource> readback;gpu_test::makeBuffer(rig.device.p,readback,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* resources[]={b.drawArguments,b.alive,b.attributes};const std::size_t offsets[]={0,16,dataOffset},sizes[]={16,b.capacity*4ull,b.layout.bytes};
    const D3D12_RESOURCE_STATES states[]={D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    rig.begin();for(unsigned i=0;i<3;++i){gpu_test::transition(rig.commands.p,resources[i],states[i],D3D12_RESOURCE_STATE_COPY_SOURCE);rig.commands.p->CopyBufferRegion(readback.p,offsets[i],resources[i],0,sizes[i]);gpu_test::transition(rig.commands.p,resources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,states[i]);}rig.submit();
    void* mapped=nullptr;D3D12_RANGE range{0,total};gpu_test::hr(readback.p->Map(0,&range,&mapped),"Map runtime validation");auto* bytes=static_cast<const unsigned char*>(mapped);
    const auto word=[&](std::size_t offset){std::uint32_t value;std::memcpy(&value,bytes+offset,4);return value;};
    const auto count=word(4);require(count<=b.capacity,"runtime draw count exceeds pool capacity");std::vector<Particle> out;
    for(unsigned n=0;n<count;++n){auto slot=word(16+n*4);require(slot<b.capacity,"runtime alive slot outside allocation");Particle p;std::memcpy(&p.position,bytes+dataOffset+b.layout.position+slot*12,12);if(b.localSpace)p.position+=b.origin;p.id=word(dataOffset+b.layout.spawnId+slot*4);p.age=word(dataOffset+b.layout.age+slot*4);p.life=word(dataOffset+b.layout.lifetime+slot*4);out.push_back(p);}
    D3D12_RANGE none{0,0};readback.p->Unmap(0,&none);return out;
}
void shutdown(Rig& rig,GpuRuntime& runtime){std::string error;require(runtime.shutdown(rig.submitted,error),error.c_str());require(runtime.stats().allocationBytes==0,"runtime shutdown retained buffer allocations");}
void assetContracts(Rig& rig){
    GpuRuntime runtime(rig.device.p,{8,2,8,8*1024*1024});auto a=asset();runtime.install(a);a->emitters.clear();spawn(runtime);step(rig,runtime);require(runtime.stats().visual.alive==3,"registry did not retain an immutable asset copy");
    require(runtime.spawn("unknown").status==SpawnStatus::UnknownAsset,"unknown GPU asset accepted");
    require(runtime.spawn("test",{std::numeric_limits<float>::quiet_NaN(),0,0}).status==SpawnStatus::InvalidPosition,"nonfinite GPU origin accepted");
    auto field=asset("field");field->hasField=true;runtime.install(field);require(runtime.spawn("field").status==SpawnStatus::UnsupportedFeature,"unbound GPU field asset accepted");
    auto events=asset("events");events->hasEvents=true;runtime.install(events);require(runtime.spawn("events").status==SpawnStatus::UnsupportedFeature,"unimplemented GPU events silently dropped");
    auto malformed=asset("bad");malformed->emitters[0].bursts={{1,1},{1,1}};throws([&]{runtime.install(malformed);},"duplicate GPU burst ticks accepted");
    auto replacement=asset();replacement->hash="replacement";throws([&]{runtime.install(replacement);},"GPU module hot replacement silently ignored");
    rig.begin();std::string error;require(!runtime.step(rig.commands.p,rig.submitted,rig.submitted,error)&&!runtime.flush(rig.commands.p,rig.submitted,rig.submitted,error),"runtime recorded new work under an already completed fence");rig.submit();
    shutdown(rig,runtime);++groups;
}
void sharedBudgets(Rig& rig){
    GpuRuntime runtime(rig.device.p,{7,2,4,16*1024*1024});auto a=asset();a->emitters.push_back(emitter("b"));a->emitters[1].rateMilli=480000;runtime.install(a);spawn(runtime);spawn(runtime);
    step(rig,runtime);auto stats=runtime.stats();require(stats.visual.alive==4&&stats.visual.emitted==4&&stats.visual.dropped==12,"global GPU per-tick spawn cap was ignored");
    auto batches=draw(rig,runtime);require(batches.size()==4,"multi-instance emitter batches missing");
    const unsigned expected[]={3,0,1,0};for(unsigned n=0;n<4;++n)require(particles(rig,batches[n]).size()==expected[n],"accepted GPU spawns did not consume shared budgets in authored order");
    step(rig,runtime);require(runtime.stats().visual.alive==7&&runtime.stats().visual.emitted==7,"shared instance/global maxAlive cap was ignored");
    step(rig,runtime);require(runtime.stats().visual.alive==7&&runtime.stats().visual.emitted==7,"full shared budget admitted new particles");
    step(rig,runtime);stats=runtime.stats();require(stats.visual.alive==7&&stats.visual.expired==4&&stats.visual.emitted==11&&stats.visual.dropped==53&&stats.visual.faults==0,"shared ledgers did not recycle exact same-tick expirations");
    batches=draw(rig,runtime);const auto first=particles(rig,batches[0]);unsigned newIds=0;for(const auto& p:first)if(p.id>=12&&p.id<15)++newIds;
    require(newIds==3,"shared-budget drops shifted future emitter request ordinals");
    shutdown(rig,runtime);++groups;
}
void expireBeforeAdmission(Rig& rig){
    GpuRuntime runtime(rig.device.p,{2,2,2,8*1024*1024});const auto base=runtime.stats().allocationBytes;auto early=asset("early"),late=asset("late");
    for(auto a:{early,late}){a->maxAlive=2;a->maxSpawnPerTick=2;a->duration=2;a->loop=false;a->emitters[0].rateMilli=0;a->emitters[0].lifeMin=a->emitters[0].lifeMax=1;}
    early->emitters[0].bursts={{1,2}};late->emitters[0].bursts={{0,2}};runtime.install(early);runtime.install(late);auto h0=spawn(runtime,"early"),h1=spawn(runtime,"late");
    step(rig,runtime);require(runtime.stats().visual.alive==2,"late instance failed initial admission");step(rig,runtime);
    require(runtime.valid(h0)&&!runtime.valid(h1),"natural drain did not retire the correct generation");auto batches=draw(rig,runtime);require(batches.size()==1&&particles(rig,batches[0]).size()==2,"early instance could not use capacity released by a later instance in the same tick");
    step(rig,runtime);require(!runtime.valid(h0)&&runtime.stats().visual.alive==0&&runtime.stats().instances==0&&runtime.stats().retiredInstances==0,"GPU instances did not naturally drain");require(runtime.stats().allocationBytes==base,"natural drain did not release instance buffers");shutdown(rig,runtime);++groups;
}
void cpuLifecycleReference(Rig& rig){
    GpuRuntime gpu(rig.device.p,{256,1,100,16*1024*1024});Runtime cpu(256,1);auto a=asset();a->maxAlive=256;a->maxSpawnPerTick=100;a->duration=5;a->seed=19;
    auto& e=a->emitters[0];e.rateMilli=123450;e.bursts={{0,7},{3,9}};e.localSpace=true;e.shape=Shape::Box;e.extent={2,3,4};e.lifeMin=21;e.lifeMax=31;
    gpu.install(a);cpu.install(a);auto gh=spawn(gpu,"test",{1,2,3},97);const auto cr=cpu.spawn("test",{1,2,3},97);require(cr.status==SpawnStatus::Accepted,"CPU lifecycle fixture rejected");
    for(unsigned tick=0;tick<65;++tick){
        if(tick==7||tick==10){const bool paused=tick==7;require(gpu.pause(gh,paused)&&cpu.pause(cr.handle,paused),"GPU pause/resume handle rejected");}
        if(tick==15)require(gpu.move(gh,{11,-2,3})&&cpu.move(cr.handle,{11,-2,3}),"GPU local move rejected");
        if(tick==25)require(gpu.stop(gh)&&cpu.stop(cr.handle),"GPU graceful stop rejected");
        cpu.step();step(rig,gpu);const auto gs=gpu.stats();const auto cs=cpu.stats();require(gs.visual.alive==cs.alive&&gs.visual.emitted==cs.emitted&&gs.visual.expired==cs.expired&&gs.visual.dropped==cs.dropped,"GPU rate/burst/loop/pause lifecycle counts differ from CPU");
        if(tick==0||tick==7||tick==15||tick==25||tick==40){auto batches=draw(rig,gpu);require(batches.size()==1,"GPU reference batch missing");const auto ps=particles(rig,batches[0]);std::map<std::uint32_t,Sprite> reference;for(const auto& s:cpu.sprites())reference.emplace(static_cast<std::uint32_t>(s.stableId-1),s);
            for(const auto& p:ps){auto found=reference.find(p.id);require(found!=reference.end(),"GPU request ordinal differs from CPU accepted ordinal");auto delta=p.position-found->second.position;require(std::fabs(delta.x)<2e-5f&&std::fabs(delta.y)<2e-5f&&std::fabs(delta.z)<2e-5f,"GPU seeded position/lifecycle move differs from CPU");++comparisons;}}
    }
    require(!gpu.valid(gh)&&!cpu.valid(cr.handle),"stopped GPU instance remained after natural expiration");shutdown(rig,gpu);++groups;
}
void sameFenceSnapshots(Rig& rig){
    GpuRuntime runtime(rig.device.p,{4,1,4,8*1024*1024});auto a=asset();a->duration=1;a->loop=false;a->emitters[0].rateMilli=0;a->emitters[0].bursts={{0,1}};a->emitters[0].lifeMin=a->emitters[0].lifeMax=3;runtime.install(a);auto h=spawn(runtime);
    rig.begin();std::string error;const auto fence=rig.ticket();require(runtime.prepareDraw(rig.commands.p,fence,error),error.c_str());require(runtime.flush(rig.commands.p,fence,rig.submitted,error),error.c_str());require(runtime.step(rig.commands.p,fence,rig.submitted,error),error.c_str());rig.submit();runtime.collect(rig.submitted);
    require(runtime.valid(h)&&runtime.stats().visual.alive==1,"old empty telemetry at the same fence retired particles born after it");
    step(rig,runtime);step(rig,runtime);step(rig,runtime);require(!runtime.valid(h),"same-fence instance failed to retire after real drain");shutdown(rig,runtime);++groups;
}
void fencesMemoryAndReadbackRing(Rig& rig){
    GpuRuntime runtime(rig.device.p,{4,2,4,1024*1024});const auto base=runtime.stats().allocationBytes;auto a=asset();a->duration=1;a->loop=false;a->emitters[0].rateMilli=0;a->emitters[0].bursts={{0,1}};a->emitters[0].lifeMin=a->emitters[0].lifeMax=6;runtime.install(a);auto h=spawn(runtime);
    gpu_test::Com<ID3D12Fence> gate;gpu_test::hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.out())),"Create runtime fence gate");gpu_test::hr(rig.queue.p->Wait(gate.p,1),"Gate runtime queue");rig.begin();std::string error;
    bool recorded=true;for(unsigned i=0;i<4;++i)recorded=runtime.step(rig.commands.p,rig.ticket(),rig.submitted,error)&&recorded;rig.submit(false);
    runtime.collect(rig.fence.p->GetCompletedValue());const auto blocked=runtime.stats();const bool earlyReleased=runtime.shutdown(rig.fence.p->GetCompletedValue(),error);const bool stopped=runtime.stop(h,true);const auto rejected=runtime.spawn("test");
    // Release the gate before assertions so a failed check cannot strand work.
    gpu_test::hr(gate.p->Signal(1),"Release runtime gate");rig.wait();
    require(recorded&&!earlyReleased&&blocked.pendingReadbacks==3&&blocked.skippedReadbacks==1&&!blocked.telemetryValid,"runtime waited, mapped unfinished telemetry, or exceeded its bounded readback ring");
    require(stopped&&!runtime.valid(h)&&rejected.status==SpawnStatus::Capacity,"immediate stop reused memory still referenced by queued GPU work");
    runtime.collect(rig.submitted);require(runtime.stats().retiredInstances==1&&runtime.stats().allocationBytes>base,"pending GPU kill lost its resource ownership");
    rig.begin();require(runtime.flush(rig.commands.p,rig.ticket(),rig.submitted,error),error.c_str());rig.submit();runtime.collect(rig.submitted);const auto cleared=runtime.stats();require(cleared.visual.alive==0&&cleared.visual.killed==1&&cleared.visual.faults==0&&cleared.retiredInstances==0&&cleared.allocationBytes==base,"immediate kill failed to clear ledger and release fenced resources");
    const auto replacement=spawn(runtime);require(replacement.slot==h.slot&&replacement.generation!=h.generation&&!runtime.pause(h,true),"stale GPU handle aliased a replacement instance");shutdown(rig,runtime);++groups;
}
void mixedInstanceStress(Rig& rig){
    GpuRuntime runtime(rig.device.p,{64,128,20,128*1024*1024});std::vector<std::string> ids;
    for(unsigned n=0;n<5;++n){auto a=asset("mixed"+std::to_string(n));a->maxAlive=2;a->maxSpawnPerTick=2;a->emitters[0].rateMilli=240000;a->emitters[0].shape=static_cast<Shape>(n);a->emitters[0].lifeMin=1;a->emitters[0].lifeMax=5;a->emitters[0].localSpace=n%2;runtime.install(a);ids.push_back(a->id);}
    std::vector<Handle> handles;for(unsigned n=0;n<128;++n){handles.push_back(spawn(runtime,ids[n%5],{static_cast<float>(n),0,0},n));++stressSpawns;}
    require(runtime.spawn(ids[0]).status==SpawnStatus::Capacity,"128-instance GPU cap was not enforced");
    for(unsigned tick=0;tick<18;++tick){if(tick%3==2){for(unsigned n=tick;n<handles.size();n+=16)require(runtime.stop(handles[n],true),"stress immediate stop rejected");}
        step(rig,runtime);auto s=runtime.stats();require(s.visual.alive<=64&&s.visual.faults==0&&s.allocationBytes<=128*1024*1024,"mixed GPU instances exceeded live/allocation budget or corrupted shared ledger");
        if(tick%3==2)for(unsigned n=tick;n<handles.size();n+=16){handles[n]=spawn(runtime,ids[(n+tick)%5],{},n+tick);++stressSpawns;}}
    runtime.clear();rig.begin();std::string error;require(runtime.flush(rig.commands.p,rig.ticket(),rig.submitted,error),error.c_str());rig.submit();runtime.collect(rig.submitted);require(runtime.stats().instances==0&&runtime.stats().retiredInstances==0&&runtime.stats().visual.alive==0&&runtime.stats().visual.faults==0,"128-instance repeated spawn/stop did not clear GPU resources");shutdown(rig,runtime);++groups;
}
}
int main(int argc,char** argv){
    try{std::string adapter;if(argc==3&&std::string(argv[1])=="--adapter")adapter=argv[2];else if(argc!=1)throw std::runtime_error("Usage: native_effect_gpu_runtime_tests [--adapter substring]");Rig rig(adapter);
        assetContracts(rig);sharedBudgets(rig);expireBeforeAdmission(rig);cpuLifecycleReference(rig);sameFenceSnapshots(rig);fencesMemoryAndReadbackRing(rig);mixedInstanceStress(rig);const auto errors=rig.debugErrors();require(errors==0,"D3D12 runtime debug errors");
        std::cout<<nlohmann::json({{"schema","nativegpu.effect-gpu-runtime-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"cpuParticleComparisons",comparisons},{"stressInstances",128},{"stressSpawns",stressSpawns},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"gpuSharedBudgets",true},{"boundedAsyncReadback",true},{"synchronousRuntimeReadback",false},{"rendererIntegrated",false}}).dump()<<'\n';
    }catch(const std::exception& error){std::cerr<<"GPU effect runtime failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
