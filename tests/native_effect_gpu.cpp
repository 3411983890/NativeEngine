#include "../engine/native/effects/gpu_pool.hpp"
#include "../engine/native/determinism.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

using namespace engine::effects;
using engine::Vec3;
using gpu_test::Rig;
namespace {
unsigned checks=0,groups=0,particleComparisons=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void throws(F action,const char* why){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,why);}
struct Snapshot {
    std::array<std::uint32_t,GpuPoolCounterWords> counters{};std::array<std::uint32_t,4> draw{};
    std::array<std::uint32_t,3> dispatch{};std::vector<std::uint32_t> alive,dead;std::vector<unsigned char> data;GpuParticleLayout layout;
    std::uint32_t word(std::uint32_t offset,unsigned slot) const {std::uint32_t v;std::memcpy(&v,data.data()+offset+slot*4,4);return v;}
    float scalar(std::uint32_t offset) const {float v;std::memcpy(&v,data.data()+offset,4);return v;}
    Vec3 position(unsigned slot) const {auto offset=layout.position+slot*12;return {scalar(offset),scalar(offset+4),scalar(offset+8)};}
};
Snapshot snapshot(Rig& rig,GpuParticlePool& pool) {
    const auto b=pool.buffers();const auto stats=pool.stats();require(stats.initialized,"readback of uninitialized GPU pool");
    const std::size_t stateBytes=GpuPoolCounterWords*4,indicesBytes=b.capacity*4ull;
    const std::size_t offsets[]={0,stateBytes,stateBytes+16,stateBytes+28,stateBytes+28+indicesBytes,stateBytes+28+2*indicesBytes};
    const std::size_t total=offsets[5]+b.layout.bytes;
    gpu_test::Com<ID3D12Resource> readback;gpu_test::makeBuffer(rig.device.p,readback,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* resources[]={b.counters,b.drawArguments,b.dispatchArguments,b.alive,b.dead,b.attributes};
    const std::size_t sizes[]={stateBytes,16,12,indicesBytes,indicesBytes,b.layout.bytes};
    const D3D12_RESOURCE_STATES states[]={D3D12_RESOURCE_STATE_UNORDERED_ACCESS,stats.drawReady?D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        b.dispatchState,stats.drawReady?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,stats.drawReady?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    rig.begin();
    for(unsigned i=0;i<6;++i){gpu_test::transition(rig.commands.p,resources[i],states[i],D3D12_RESOURCE_STATE_COPY_SOURCE);rig.commands.p->CopyBufferRegion(readback.p,offsets[i],resources[i],0,sizes[i]);gpu_test::transition(rig.commands.p,resources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,states[i]);}
    rig.submit();Snapshot out;out.layout=b.layout;
    void* mapped=nullptr;D3D12_RANGE range{0,total};gpu_test::hr(readback.p->Map(0,&range,&mapped),"Map particle validation");auto* bytes=static_cast<const unsigned char*>(mapped);
    std::memcpy(out.counters.data(),bytes,stateBytes);std::memcpy(out.draw.data(),bytes+offsets[1],16);std::memcpy(out.dispatch.data(),bytes+offsets[2],12);
    out.alive.resize(b.capacity);out.dead.resize(b.capacity);out.data.resize(b.layout.bytes);
    std::memcpy(out.alive.data(),bytes+offsets[3],indicesBytes);std::memcpy(out.dead.data(),bytes+offsets[4],indicesBytes);std::memcpy(out.data.data(),bytes+offsets[5],out.data.size());D3D12_RANGE none{0,0};readback.p->Unmap(0,&none);return out;
}
Emitter emitter() {Emitter e;e.id="test";e.lifeMin=e.lifeMax=120;Module initial;initial.vector={0,4,0};initial.color={1,0.5f,0.25f,1};initial.size={0.1f,0.2f};e.modules={initial};e.renderers.push_back({"sprite","test.png","","",Blend::Additive,false});return e;}
void init(Rig& rig,GpuParticlePool& pool) {rig.begin();std::string error;require(pool.initialize(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();}
void step(Rig& rig,GpuParticlePool& pool,unsigned requested,std::uint64_t age=0,std::uint64_t sequence=0,std::uint64_t seed=7,Vec3 origin={}) {rig.begin();std::string error;require(pool.step(rig.commands.p,rig.ticket(),age,requested,sequence,seed,origin,error),error.c_str());rig.submit();}
void invariants(const Snapshot& s,unsigned capacity) {
    const auto alive=s.counters[0],free=s.counters[2];require(alive<=capacity&&free<=capacity&&alive+free==capacity,"alive/free counts do not conserve capacity");
    std::set<unsigned> slots;for(unsigned i=0;i<alive;++i)require(s.alive[i]<capacity&&slots.insert(s.alive[i]).second,"duplicate/out-of-range alive slot");
    for(unsigned i=0;i<free;++i)require(s.dead[i]<capacity&&slots.insert(s.dead[i]).second,"dead slot overlaps live or duplicate free slot");
    require(slots.size()==capacity&&s.counters[8]==0,"pool slot lost or shader fault reported");require(s.draw[0]==6&&s.draw[1]==alive&&s.draw[2]==0&&s.draw[3]==0,"GPU indirect draw count is not alive count");
}
void layoutAndValidation(Rig& rig) {
    auto l=GpuParticleLayout::make(3);require(l.position==0&&l.velocity==48&&l.age==96&&l.lifetime==112&&l.spawnId==128&&l.color==144&&l.size==192&&l.bytes==224,"GPU layout does not match cooked 16-byte aligned SoA");
    auto e=emitter();throws([&]{GpuParticlePool pool(rig.device.p,e,0);},"zero capacity accepted");
    throws([&]{GpuParticlePool pool(rig.device.p,e,8,{8,1});},"allocation budget ignored");
    e.shape=Shape::Mesh;require(!GpuParticlePool::unsupported(e).empty(),"GPU mesh emission silently changed shape");
    e=emitter();Module field;field.kind=ModuleKind::FieldCoupling;e.modules.push_back(field);throws([&]{GpuParticlePool pool(rig.device.p,e,8);},"unbound GPU field module silently enabled");
    e=emitter();e.modules.front().vector.x=std::numeric_limits<float>::quiet_NaN();throws([&]{GpuParticlePool pool(rig.device.p,e,8);},"non-finite GPU module accepted");++groups;
}
void lifecycleAndCompaction(Rig& rig) {
    auto e=emitter();e.lifeMin=e.lifeMax=3;GpuParticlePool pool(rig.device.p,e,257);std::string error;
    rig.begin();require(!pool.step(rig.commands.p,rig.ticket(),0,1,0,0,{},error),"GPU step before initialize succeeded");rig.submit();
    init(rig,pool);auto empty=snapshot(rig,pool);invariants(empty,257);require(empty.counters[0]==0,"initial live count not zero");
    step(rig,pool,300);auto full=snapshot(rig,pool);invariants(full,257);require(full.counters[0]==257&&full.counters[5]==257&&full.counters[7]==43,"spawn overflow did not reserve exact GPU capacity");
    require(full.dispatch[0]==5&&full.dispatch[1]==1&&full.dispatch[2]==1,"GPU spawn dispatch did not use accepted count");
    step(rig,pool,10,1,300);auto saturated=snapshot(rig,pool);invariants(saturated,257);require(saturated.counters[7]==53,"full pool did not count rejected spawns");
    step(rig,pool,0,2,310);step(rig,pool,13,3,310);auto recycled=snapshot(rig,pool);invariants(recycled,257);
    require(recycled.counters[0]==13&&recycled.counters[6]==257&&recycled.counters[5]==270,"expired slots were not available to same-tick spawn");
    for(unsigned i=0;i<recycled.counters[0];++i){require(recycled.word(recycled.layout.age,recycled.alive[i])==0,"recycled particle inherited old age");const auto id=recycled.word(recycled.layout.spawnId,recycled.alive[i]);require(id>=310&&id<323,"dropped requests shifted subsequent spawn ordinals");}
    rig.begin();require(pool.prepareDraw(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();invariants(snapshot(rig,pool),257);
    step(rig,pool,0,4);invariants(snapshot(rig,pool),257);require(!pool.stats().drawReady,"next GPU step did not restore compute resource states");
    rig.begin();require(pool.clear(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();auto cleared=snapshot(rig,pool);invariants(cleared,257);require(cleared.counters[0]==0&&cleared.counters[5]==0,"immediate clear left live or cumulative state");
    require(pool.shutdown(rig.submitted,error)&&pool.stats().allocationBytes==0,error.c_str());++groups;
}
void cpuReference(Rig& rig) {
    auto e=emitter();e.shape=Shape::Point;e.lifeMin=e.lifeMax=60;
    Module acceleration;acceleration.kind=ModuleKind::Acceleration;acceleration.vector={0,-9.81f,0};e.modules.push_back(acceleration);
    Module drag;drag.kind=ModuleKind::Drag;drag.coefficient=0.2f;e.modules.push_back(drag);
    Module noise;noise.kind=ModuleKind::CurlNoise;noise.coefficient=0.4f;noise.frequency=1.2f;e.modules.push_back(noise);
    Module color;color.kind=ModuleKind::ColorCurve;color.keys={ {0,{1,0.5f,0.25f,1}},{0.5f,{0.8f,0.3f,0.15f,0.7f}},{1,{0.2f,0.1f,0,0}} };e.modules.push_back(color);
    Module size;size.kind=ModuleKind::SizeCurve;size.keys={ {0,{0.1f,0.2f,0,0}},{1,{0.4f,0.5f,0,0}} };e.modules.push_back(size);
    auto asset=std::make_shared<Asset>();asset->id="gpu_reference";asset->hash="gpu_reference";asset->layoutHash="gpu_reference";asset->duration=1;asset->maxAlive=128;asset->maxSpawnPerTick=128;e.bursts={{0,97}};asset->emitters={e};
    Runtime cpu(128,1);cpu.install(asset);require(cpu.spawn(asset->id,{2,3,4},5).status==SpawnStatus::Accepted,"CPU reference spawn failed");
    GpuParticlePool pool(rig.device.p,e,128);init(rig,pool);
    for(unsigned age=0;age<61;++age) {
        cpu.step();step(rig,pool,age==0?97:0,age,age?97:0,7,{2,3,4});
        if(age==0||age==1||age==30||age==59||age==60){auto gpu=snapshot(rig,pool);invariants(gpu,128);const auto sprites=cpu.sprites();require(gpu.counters[0]==sprites.size(),"GPU/CPU lifetime count differs");
            for(unsigned i=0;i<gpu.counters[0];++i){auto slot=gpu.alive[i];const auto p=gpu.position(slot);require(std::fabs(p.x-sprites[0].position.x)<2e-5f&&std::fabs(p.y-sprites[0].position.y)<2e-5f&&std::fabs(p.z-sprites[0].position.z)<2e-5f,"GPU/CPU semi-implicit motion differs");
                for(unsigned c=0;c<4;++c)require(std::fabs(gpu.scalar(gpu.layout.color+slot*16+c*4)-sprites[0].color[c])<2e-5f,"GPU/CPU color curve differs");
                for(unsigned c=0;c<2;++c)require(std::fabs(gpu.scalar(gpu.layout.size+slot*8+c*4)-sprites[0].size[c])<2e-5f,"GPU/CPU size curve differs");++particleComparisons;}
        }
    }
    std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());++groups;
}
void extremeRequestsAndSpaces(Rig& rig) {
    auto e=emitter();e.localSpace=true;GpuParticlePool local(rig.device.p,e,8);init(rig,local);
    step(rig,local,std::numeric_limits<std::uint32_t>::max(),0,0,7,{2,3,4});auto before=snapshot(rig,local);invariants(before,8);
    require(before.counters[7]==std::numeric_limits<std::uint32_t>::max()-8,"large emission request overflowed drop accounting");
    step(rig,local,std::numeric_limits<std::uint32_t>::max(),1,std::numeric_limits<std::uint32_t>::max(),7,{2,3,4});
    auto after=snapshot(rig,local);invariants(after,8);require(after.counters[7]==std::numeric_limits<std::uint32_t>::max(),"cumulative GPU dropped counter wrapped instead of saturating");
    const auto p=after.position(after.alive[0]);require(std::fabs(p.x)<1e-6f&&p.y<1&&std::fabs(p.z)<1e-6f,"local GPU coordinates included world origin");
    require(local.move({12,3,4})&&local.buffers().origin.x==12&&local.buffers().localSpace,"local origin binding did not follow emitter move");
    auto moved=snapshot(rig,local);require(moved.position(moved.alive[0]).x==p.x,"moving a local emitter rewrote the particle simulation position");
    rig.begin();std::string error;require(!local.step(rig.commands.p,rig.ticket(),2,1,std::numeric_limits<std::uint64_t>::max(),7,{},error),"64-bit sequence overflow accepted");rig.submit();
    e.localSpace=false;GpuParticlePool world(rig.device.p,e,8);init(rig,world);step(rig,world,1,0,0,7,{2,3,4});
    require(world.move({12,3,4}),"world emitter move rejected");auto stationary=snapshot(rig,world);
    require(stationary.position(stationary.alive[0]).x==2&&!world.buffers().localSpace,"world-space particle moved with its emitter");
    require(local.shutdown(rig.submitted,error)&&world.shutdown(rig.submitted,error),error.c_str());++groups;
}
void lifetimeQuantization(Rig& rig) {
    auto e=emitter();e.lifeMin=1;e.lifeMax=432000;GpuParticlePool pool(rig.device.p,e,4097);init(rig,pool);
    const std::uint64_t first=(1ull<<40)+1000,base=0xfedcba9876543210ull;step(rig,pool,4097,0,first,base);auto output=snapshot(rig,pool);invariants(output,4097);
    for(unsigned i=0;i<output.counters[0];++i) {
        const auto slot=output.alive[i],ordinal=output.word(output.layout.spawnId,slot);
        const auto seed=base^engine::determinism::mix64((1ull<<40)|ordinal);
        const auto bits=engine::determinism::mix64(seed+3*0x9e3779b97f4a7c15ull)>>40;
        const auto expected=1+static_cast<unsigned>((static_cast<double>(bits)/16777216.0)*432000.0);
        require(output.word(output.layout.lifetime,slot)==expected,"GPU lifetime quantization differs from CPU double reference over full one-hour range");
    }
    std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());++groups;
}
void seededShapes(Rig& rig) {
    const auto base=0x12345678abcdef01ull,first=(1ull<<32)+19;
    for(auto shape:{Shape::Point,Shape::Sphere,Shape::Cone,Shape::Box,Shape::Line}) {
        auto e=emitter();e.shape=shape;e.radius=2;e.angle=0.4f;e.extent={2,4,6};e.lineEnd={2,3,4};e.lifeMin=40;e.lifeMax=100;
        GpuParticlePool a(rig.device.p,e,129),b(rig.device.p,e,129);init(rig,a);init(rig,b);step(rig,a,129,0,first,base,{2,3,4});step(rig,b,129,0,first,base,{2,3,4});
        auto sa=snapshot(rig,a),sb=snapshot(rig,b);invariants(sa,129);invariants(sb,129);
        const auto sorted=[&](const Snapshot& s){auto slots=s.alive;std::sort(slots.begin(),slots.end(),[&](auto x,auto y){return s.word(s.layout.spawnId,x)<s.word(s.layout.spawnId,y);});return slots;};
        auto slotsA=sorted(sa),slotsB=sorted(sb);
        for(unsigned i=0;i<129;++i) {
            const auto p=sa.position(slotsA[i]),q=sb.position(slotsB[i]);require(p.x==q.x&&p.y==q.y&&p.z==q.z,"same GPU seed changed per-ordinal shape despite compaction order");
            const auto ordinal=sa.word(sa.layout.spawnId,slotsA[i]);const auto seed=base^engine::determinism::mix64((1ull<<32)|ordinal);
            const auto life=40+static_cast<unsigned>(static_cast<float>(engine::determinism::mix64(seed+3*0x9e3779b97f4a7c15ull)>>40)*(1.f/16777216)*61);
            require(sa.word(sa.layout.lifetime,slotsA[i])==life,"GPU splitmix64/channel RNG differs from CPU integer reference");
            const Vec3 offset=p-Vec3{2,3,4};if(shape==Shape::Point)require(offset.x==0&&offset.y==0&&offset.z==0,"GPU point emission moved off source");
            if(shape==Shape::Sphere)require(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z<=4.00001f,"GPU sphere emission outside volume");
            if(shape==Shape::Cone)require(offset.x*offset.x+offset.z*offset.z<=4.00001f&&offset.y==0,"GPU cone base emission outside disc");
            if(shape==Shape::Box)require(std::fabs(offset.x)<=1&&std::fabs(offset.y)<=2&&std::fabs(offset.z)<=3,"GPU box emission outside volume");
            if(shape==Shape::Line)require(std::fabs(offset.y/3-offset.x/2)<1e-6f&&std::fabs(offset.z/4-offset.x/2)<1e-6f,"GPU line emission not collinear");
        }
        std::string error;require(a.shutdown(rig.submitted,error)&&b.shutdown(rig.submitted,error),error.c_str());
    }
    ++groups;
}
void longRecyclingAndFences(Rig& rig) {
    auto e=emitter();e.lifeMin=1;e.lifeMax=13;GpuParticlePool pool(rig.device.p,e,4097);init(rig,pool);
    for(unsigned tick=0;tick<128;++tick){step(rig,pool,173,tick,static_cast<std::uint64_t>(tick)*173);if(tick%16==0)invariants(snapshot(rig,pool),4097);}
    std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());
    GpuParticlePool pending(rig.device.p,e,8);gpu_test::Com<ID3D12Fence> gate;gpu_test::hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.out())),"Create fence gate");
    gpu_test::hr(rig.queue.p->Wait(gate.p,1),"Gate GPU queue");rig.begin();const auto ticket=rig.ticket();const auto initialized=pending.initialize(rig.commands.p,ticket,error);rig.submit(false);
    const bool earlyShutdown=pending.shutdown(rig.fence.p->GetCompletedValue(),error);const bool retained=pending.stats().allocationBytes>0;
    gpu_test::hr(gate.p->Signal(1),"Release GPU gate");rig.wait();
    require(initialized&&!earlyShutdown&&retained,"GPU pool freed before real queued work completed");require(pending.shutdown(rig.submitted,error)&&pending.stats().allocationBytes==0,"completed GPU pool resources did not release");++groups;
}
void sharedLedgerContract(Rig& rig) {
    auto e=emitter();e.lifeMin=e.lifeMax=1;
    auto instance=std::make_shared<GpuParticleBudget>(rig.device.p,5,3),global=std::make_shared<GpuParticleBudget>(rig.device.p,6,4);
    throws([&]{GpuParticlePool invalid(rig.device.p,e,5,{},instance,instance);},"same GPU ledger bound as instance and global");
    GpuParticlePool a(rig.device.p,e,5,{},instance,global),b(rig.device.p,e,5,{},instance,global);std::string error;
    rig.begin();require(a.initialize(rig.commands.p,rig.ticket(),error)&&b.initialize(rig.commands.p,rig.ticket(),error),error.c_str());
    require(a.prepareBudgets(rig.commands.p,rig.ticket(),true,true,error),error.c_str());
    require(a.simulate(rig.commands.p,rig.ticket(),0,{},error)&&b.simulate(rig.commands.p,rig.ticket(),0,{},error),error.c_str());
    require(!a.prepareDraw(rig.commands.p,rig.ticket(),error)&&!a.clear(rig.commands.p,rig.ticket(),error)&&!a.simulate(rig.commands.p,rig.ticket(),0,{},error),"half-completed GPU tick allowed draw/clear/another simulation");
    require(a.spawn(rig.commands.p,rig.ticket(),2,0,7,error)&&b.spawn(rig.commands.p,rig.ticket(),2,0,9,error),error.c_str());rig.submit();
    auto sa=snapshot(rig,a),sb=snapshot(rig,b);invariants(sa,5);invariants(sb,5);require(sa.counters[0]==2&&sb.counters[0]==1,"per-instance shared spawn budget not consumed by accepted count");
    rig.begin();require(a.prepareBudgets(rig.commands.p,rig.ticket(),true,true,error),error.c_str());require(a.simulate(rig.commands.p,rig.ticket(),1,{},error)&&b.simulate(rig.commands.p,rig.ticket(),1,{},error),error.c_str());require(a.spawn(rig.commands.p,rig.ticket(),3,2,7,error)&&b.spawn(rig.commands.p,rig.ticket(),3,2,9,error),error.c_str());rig.submit();
    sa=snapshot(rig,a);sb=snapshot(rig,b);invariants(sa,5);invariants(sb,5);require(sa.counters[0]==3&&sb.counters[0]==0,"expired particles were not removed from shared GPU ledger before admission");
    rig.begin();require(a.clear(rig.commands.p,rig.ticket(),error)&&b.clear(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();require(a.shutdown(rig.submitted,error)&&b.shutdown(rig.submitted,error),error.c_str());++groups;
}
}
int main(int argc,char** argv) {
    try {std::string adapter;if(argc==3&&std::string(argv[1])=="--adapter")adapter=argv[2];else if(argc!=1)throw std::runtime_error("Usage: native_effect_gpu_tests [--adapter substring]");Rig rig(adapter);
        layoutAndValidation(rig);lifecycleAndCompaction(rig);cpuReference(rig);seededShapes(rig);extremeRequestsAndSpaces(rig);lifetimeQuantization(rig);longRecyclingAndFences(rig);sharedLedgerContract(rig);auto errors=rig.debugErrors();require(errors==0,"D3D12 debug errors");
        std::cout<<nlohmann::json({{"schema","nativegpu.effect-gpu-pool-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"cpuParticleComparisons",particleComparisons},{"recyclingTicks",128},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"gpuAliveDeadLists",true},{"gpuIndirectCounts",true},{"runtimeReadback",false},{"rendererIntegrated",false},{"assetInstanceManagerIntegrated",false}}).dump()<<'\n';
    }catch(const std::exception& error){std::cerr<<"GPU effect pool failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
