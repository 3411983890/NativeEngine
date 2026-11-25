#include "../engine/native/effects/gpu_runtime.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <cmath>
#include <limits>

using namespace engine;
using namespace engine::effects;
namespace f=engine::fields;
using gpu_test::Rig;
namespace {
unsigned checks=0,groups=0,comparisons=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void throws(F action,const char* why){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,why);}
constexpr f::Tick Base=(1ull<<40)+17;
struct Scene {
    f::World world{128};std::shared_ptr<f::GpuFieldStorage> storage;std::unique_ptr<f::GpuVectorFields> vectors;
    f::Lease writer,cpuReader;f::GpuLease lease;f::Region region;f::GpuVectorSpec spec;std::array<f::ChannelId,3> wind{},water{};bool withWater=false,leaseActive=true;
    Scene(Rig& rig,f::Consistency consistency=f::Consistency::Eventual,f::Tick maxAge=200,bool omitZ=false,bool clamp=false,bool gradient=false,bool flow=false,f::GpuVectorLimits metadata={}):withWater(flow){
        region=gradient?f::Region{{14,-18,-1},{18,-14,3}}:f::Region{{-2,-2,-2},{3,3,3}};
        spec.cellMetres=gradient?.5f:1.f;if(gradient)spec.originMetres={1,-2,3};
        std::vector<f::ChannelId> ids;
        const unsigned scales[]={100,10,1000};
        for(unsigned axis=0;axis<3;++axis){f::Channel c{"wind"+std::to_string(axis),"m/s","source",f::Domain::CpuAuthority,f::Merge::Sum};c.minimum=clamp&&axis==0?-100:-100000;c.maximum=clamp&&axis==0?100:100000;c.unitsPerInteger=scales[axis];wind[axis]=world.registerChannel(c);ids.push_back(wind[axis]);}
        if(flow)for(unsigned axis=0;axis<3;++axis){f::Channel c{"water"+std::to_string(axis),"m/s","source",f::Domain::VisualReference,f::Merge::Sum};c.minimum=-10000;c.maximum=10000;c.unitsPerInteger=100;water[axis]=world.registerChannel(c);ids.push_back(water[axis]);}
        spec.channels["windVelocity"]=wind;if(flow)spec.channels["waterVelocity"]=water;
        f::Region expanded{{region.min.x-1,region.min.y-1,region.min.z-1},{region.max.x+1,region.max.y+1,region.max.z+1}};
        f::LeaseRequest request;request.region=expanded;request.channels=ids;request.owner="source";request.writable=true;request.maxPages=64;
        auto w=world.acquire(request);require(w.accepted,w.error.c_str());writer=w.lease;
        std::vector<f::Publication> publications;
        for(unsigned index=0;index<ids.size();++index){std::vector<f::Write> writes;
            if(!(omitZ&&index==2))for(int x=expanded.min.x;x<expanded.max.x;++x)for(int y=expanded.min.y;y<expanded.max.y;++y)for(int z=expanded.min.z;z<expanded.max.z;++z){
                const int constant[]={clamp?100:400,10,-2000,200,-100,0};const int varied[]={x*5+y*2-z,y,z*20};
                writes.push_back({{x,y,z},gradient?varied[index%3]:constant[index]});}
            publications.push_back({writer,ids[index],71,1,Base,Base,index<3?f::Domain::CpuAuthority:f::Domain::VisualReference,std::move(writes)});
        }
        if(clamp){auto second=publications.front();second.source=72;publications.push_back(std::move(second));}
        require(world.publishAtomic(publications)==f::PublishResult::Accepted,"fixture velocity publication failed");require(world.commit(Base),"fixture velocity commit failed");
        request.region=region;request.owner="consumer";request.writable=false;request.consistency=consistency;request.maxAgeTicks=maxAge;request.haloCells=1;
        auto r=world.acquire(request);require(r.accepted,r.error.c_str());cpuReader=r.lease;
        storage=std::make_shared<f::GpuFieldStorage>(world,rig.device.p);auto acquired=storage->acquire(request);require(acquired.accepted,acquired.error.c_str());lease=acquired.lease;
        rig.begin();std::string error;require(storage->publish(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();storage->collect(rig.submitted);
        require(storage->stats().descriptorSlots==storage->stats().currentPages+1,"page table descriptor not included in GPU field accounting");
        vectors=std::make_unique<f::GpuVectorFields>(storage,metadata);
    }
    std::shared_ptr<f::GpuVectorView> bind(f::Tick tick=Base){auto result=vectors->bind(lease,spec,tick);require(result.accepted(),result.error.c_str());return std::move(result.view);}
    void close(Rig& rig){std::string error;require(vectors->shutdown(rig.submitted,error),error.c_str());if(leaseActive)require(storage->release(lease,rig.submitted),"release GPU velocity lease failed");storage->collect(rig.submitted);require(storage->shutdown(rig.submitted,error),error.c_str());require(storage->stats().defaultAllocationBytes==0&&storage->stats().uploadAllocationBytes==0,"field pages retained after all views/fences retired");require(world.release(cpuReader)&&world.release(writer),"fixture CPU lease release failed");world.collect(rig.submitted);}
};
struct Snapshot {
    std::array<unsigned,GpuPoolCounterWords> counters{};std::vector<unsigned> alive;std::vector<unsigned char> data;GpuParticleLayout layout;
    Vec3 vector(unsigned offset,unsigned slot) const{Vec3 v;std::memcpy(&v,data.data()+offset+slot*12,12);return v;}
};
Snapshot snapshot(Rig& rig,const GpuPoolBuffers& b,bool drawReady){
    const std::size_t offsets[]={0,64,64+b.capacity*4ull},sizes[]={64,b.capacity*4ull,b.layout.bytes};const auto total=offsets[2]+sizes[2];gpu_test::Com<ID3D12Resource> read;gpu_test::makeBuffer(rig.device.p,read,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* resources[]={b.counters,b.alive,b.attributes};const auto dataState=drawReady?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;const D3D12_RESOURCE_STATES states[]={D3D12_RESOURCE_STATE_UNORDERED_ACCESS,dataState,dataState};
    rig.begin();for(unsigned n=0;n<3;++n){gpu_test::transition(rig.commands.p,resources[n],states[n],D3D12_RESOURCE_STATE_COPY_SOURCE);rig.commands.p->CopyBufferRegion(read.p,offsets[n],resources[n],0,sizes[n]);gpu_test::transition(rig.commands.p,resources[n],D3D12_RESOURCE_STATE_COPY_SOURCE,states[n]);}rig.submit();void* p=nullptr;D3D12_RANGE range{0,total};gpu_test::hr(read.p->Map(0,&range,&p),"Map GPU field effect validation");auto* bytes=static_cast<const unsigned char*>(p);Snapshot out;out.layout=b.layout;std::memcpy(out.counters.data(),bytes,64);out.alive.resize(out.counters[0]);std::memcpy(out.alive.data(),bytes+offsets[1],out.alive.size()*4);out.data.resize(b.layout.bytes);std::memcpy(out.data.data(),bytes+offsets[2],b.layout.bytes);D3D12_RANGE none{0,0};read.p->Unmap(0,&none);return out;
}
Emitter emitter(bool flow=false){Emitter e;e.id="sample";e.lifeMin=e.lifeMax=240;Module initial;initial.vector={1,-2,0};e.modules={initial};Module wind;wind.kind=ModuleKind::VelocityField;wind.fieldChannel=flow?"waterVelocity":"windVelocity";wind.coefficient=3;e.modules.push_back(wind);e.renderers.push_back({"sprite","test.png","","",Blend::Premultiplied,false});return e;}
void init(Rig& rig,GpuParticlePool& pool){rig.begin();std::string error;require(pool.initialize(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();}
void step(Rig& rig,GpuParticlePool& pool,unsigned requested,f::Tick tick,unsigned age=0,Vec3 origin={}){rig.begin();std::string error;require(pool.step(rig.commands.p,rig.ticket(),age,requested,age?65:0,23,origin,error,tick),error.c_str());rig.submit();}
void vectorNear(Vec3 a,Vec3 b,const char* why){require(std::fabs(a.x-b.x)<3e-5f&&std::fabs(a.y-b.y)<3e-5f&&std::fabs(a.z-b.z)<3e-5f,why);++comparisons;}
void constantAndAuthority(Rig& rig){Scene scene(rig);auto view=scene.bind();auto e=emitter();GpuParticlePool pool(rig.device.p,e,65,{}, {},{},view);init(rig,pool);const auto digest=scene.world.digest();step(rig,pool,65,Base);Vec3 velocity{1,-2,0},position{};const float response=1-std::exp(-3.f/120);
    for(unsigned age=1;age<=60;++age){auto next=scene.bind(Base+age);std::string error;require(pool.bindField(next,error),error.c_str());view=std::move(next);step(rig,pool,0,Base+age,age);scene.vectors->collect(rig.submitted);velocity+=(Vec3{4,1,-2}-velocity)*response;position+=velocity*(1.f/120);
        if(age==1||age==7||age==30||age==60){auto gpu=snapshot(rig,pool.buffers(),false);require(gpu.counters[0]==65&&gpu.counters[9]==65*age&&gpu.counters[10]==0&&gpu.counters[8]==0,"GPU constant velocity query count/fault mismatch");for(auto slot:gpu.alive){vectorNear(gpu.vector(gpu.layout.velocity,slot),velocity,"GPU wind response differs from CPU analytic reference");vectorNear(gpu.vector(gpu.layout.position,slot),position,"GPU field-integrated position differs from CPU reference");}}}
    require(scene.world.digest()==digest,"visual GPU wind changed CPU authority state");std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());view.reset();scene.close(rig);++groups;
}
f::SamplePosition locate(Vec3 world,const f::GpuVectorSpec& grid){const Vec3 q=(world-grid.originMetres)*(1/grid.cellMetres);const float v[]={q.x,q.y,q.z};f::SamplePosition out;int* cells[]={&out.cell.x,&out.cell.y,&out.cell.z};for(unsigned i=0;i<3;++i){*cells[i]=static_cast<int>(std::floor(v[i]));out.fraction[i]=static_cast<unsigned>(std::floor((v[i]-*cells[i])*256));}return out;}
void gradientAndLocal(Rig& rig){Scene scene(rig,f::Consistency::Eventual,20,false,false,true);auto view=scene.bind();auto e=emitter();e.shape=Shape::Box;e.extent={1.5f,1.5f,1.5f};e.localSpace=true;e.modules[0].vector={};e.modules[1].coefficient=120;GpuParticlePool pool(rig.device.p,e,129,{}, {},{},view);init(rig,pool);const Vec3 origin=scene.spec.originMetres+Vec3{15.75f,-16.25f,.75f}*scene.spec.cellMetres;step(rig,pool,129,Base,0,origin);auto before=snapshot(rig,pool.buffers(),false);std::map<unsigned,Vec3> velocities,positions;const float weight=1-std::exp(-1.f);
    for(auto slot:before.alive){auto p=before.vector(before.layout.position,slot);auto q=locate(p+origin,scene.spec);Vec3 v;float* components[]={&v.x,&v.y,&v.z};for(unsigned axis=0;axis<3;++axis){auto sample=scene.world.sampleLinear(scene.cpuReader,scene.wind[axis],q,Base);require(sample.valid(),"CPU gradient fixture did not cover required corners");*components[axis]=static_cast<float>(sample.value)/scene.world.channel(scene.wind[axis]).unitsPerInteger*weight;}velocities[slot]=v;positions[slot]=p+v*(1.f/120);}
    step(rig,pool,0,Base,1,origin);auto after=snapshot(rig,pool.buffers(),false);require(after.counters[9]==129&&after.counters[10]==0,"GPU cross-brick field was reported unavailable");for(auto slot:after.alive){vectorNear(after.vector(after.layout.velocity,slot),velocities.at(slot),"GPU Q8 velocity across negative/cross-brick coordinates differs from CPU");vectorNear(after.vector(after.layout.position,slot),positions.at(slot),"GPU local-space field integration differs from CPU");}
    std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());view.reset();scene.close(rig);++groups;
}
void unknownStaleAndBounds(Rig& rig){
    for(unsigned mode=0;mode<6;++mode){Scene scene(rig,mode==2?f::Consistency::Stale:f::Consistency::Eventual,2,mode==0,mode==5);const auto tick=mode==1||mode==2?Base+3:Base;auto view=scene.bind(tick);auto e=emitter();if(mode==3)e.position={10,0,0};if(mode==4)e.position={9000,0,0};GpuParticlePool pool(rig.device.p,e,7,{}, {},{},view);init(rig,pool);step(rig,pool,7,tick);step(rig,pool,0,tick,1);auto gpu=snapshot(rig,pool.buffers(),false);const bool accepted=mode==2||mode==5;require(gpu.counters[9]==(accepted?7u:0u)&&gpu.counters[10]==(accepted?0u:7u),"field fallback did not preserve complete-vector availability");if(mode==1||mode==2)require(gpu.counters[11]==7,"stale field use/rejection was not reported");if(mode==3)require(gpu.counters[12]==7,"outside leased query centre not reported");if(mode==4)require(gpu.counters[14]==7,"out-of-range float field mapping not reported");if(mode==5)require(gpu.counters[13]==7,"clamped source flag did not reach particle diagnostics");
        Vec3 expected=e.modules[0].vector;if(accepted)expected+=(Vec3{mode==5?1.f:4.f,1,-2}-expected)*(1-std::exp(-3.f/120));for(auto slot:gpu.alive)vectorNear(gpu.vector(gpu.layout.velocity,slot),expected,"missing or stale vector silently acted as zero velocity");std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());view.reset();scene.close(rig);}
    ++groups;
}
void twoVectorModulesAndUnits(Rig& rig){Scene scene(rig,f::Consistency::Eventual,10,false,false,false,true);auto view=scene.bind();auto e=emitter();Module water=e.modules[1];water.fieldChannel="waterVelocity";water.coefficient=6;e.modules.push_back(water);GpuParticlePool pool(rig.device.p,e,3,{}, {},{},view);init(rig,pool);step(rig,pool,3,Base);step(rig,pool,0,Base,1);auto gpu=snapshot(rig,pool.buffers(),false);Vec3 expected{1,-2,0};expected+=(Vec3{4,1,-2}-expected)*(1-std::exp(-3.f/120));expected+=(Vec3{2,-1,0}-expected)*(1-std::exp(-6.f/120));for(auto slot:gpu.alive)vectorNear(gpu.vector(gpu.layout.velocity,slot),expected,"wind/water module order or component unit conversion changed");require(gpu.counters[9]==6,"both velocity modules did not sample one shared immutable snapshot");
    auto invalid=scene.spec;invalid.channels["windVelocity"]={scene.wind[0],scene.wind[1],255};require(!scene.vectors->bind(scene.lease,invalid,Base).accepted(),"unleased scalar component accepted");invalid=scene.spec;invalid.channels["density"]=scene.wind;require(!scene.vectors->bind(scene.lease,invalid,Base).accepted(),"non-velocity logical channel reinterpreted as m/s");
    std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());view.reset();scene.close(rig);++groups;
}
void metadataFencesAndLeases(Rig& rig){Scene scene(rig,f::Consistency::Eventual,10,false,false,false,false,{1,65536});auto view=scene.bind();GpuParticlePool pool(rig.device.p,emitter(),1,{}, {},{},view);require(!scene.vectors->bind(scene.lease,scene.spec,Base+1).accepted(),"metadata binding capacity ignored while a client owns the old view");
    require(scene.storage->release(scene.lease),"spatial lease release failed");scene.leaseActive=false;
    gpu_test::Com<ID3D12Fence> gate;gpu_test::hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.out())),"Create field retention gate");gpu_test::hr(rig.queue.p->Wait(gate.p,1),"Gate field particle commands");rig.begin();std::string error;const auto ticket=rig.ticket();const bool recorded=pool.initialize(rig.commands.p,ticket,error)&&pool.step(rig.commands.p,ticket,0,1,0,7,{},error,Base)&&pool.step(rig.commands.p,ticket,1,0,1,7,{},error,Base);rig.submit(false);const auto completed=rig.fence.p->GetCompletedValue();const bool freed=pool.shutdown(completed,error);const bool vectorFreed=scene.vectors->shutdown(completed,error);const auto bytes=scene.storage->stats().defaultAllocationBytes;
    gpu_test::hr(gate.p->Signal(1),"Release field particle gate");rig.wait();require(recorded&&!freed&&!vectorFreed&&bytes>0,"read view or metadata freed before its actual GPU fence");auto gpu=snapshot(rig,pool.buffers(),false);require(gpu.counters[9]==1,"released spatial lease invalidated an already acquired read snapshot");require(pool.shutdown(rig.submitted,error),error.c_str());view.reset();scene.close(rig);++groups;
}
void immutableSnapshotSwitch(Rig& rig){
    Scene scene(rig);auto old=scene.bind();GpuParticlePool first(rig.device.p,emitter(),1,{}, {},{},old);init(rig,first);step(rig,first,1,Base);
    std::vector<f::Publication> updates;
    const int values[]={-200,30,5000};
    for(unsigned axis=0;axis<3;++axis){std::vector<f::Write> writes;for(int x=-3;x<4;++x)for(int y=-3;y<4;++y)for(int z=-3;z<4;++z)writes.push_back({{x,y,z},values[axis]});updates.push_back({scene.writer,scene.wind[axis],71,2,Base+1,Base+1,f::Domain::CpuAuthority,std::move(writes)});}
    require(scene.world.publishAtomic(updates)==f::PublishResult::Accepted&&scene.world.commit(Base+1),"replacement authority snapshot rejected");
    rig.begin();std::string error;require(scene.storage->publish(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();
    auto next=scene.bind(Base+1);require(scene.storage->stats().snapshots==2,"old consumer snapshot was not retained after publication");
    GpuParticlePool second(rig.device.p,emitter(),1,{}, {},{},next);init(rig,second);step(rig,second,1,Base+1);step(rig,first,0,Base,1);step(rig,second,0,Base+1,1);
    auto a=snapshot(rig,first.buffers(),false),b=snapshot(rig,second.buffers(),false);const float weight=1-std::exp(-3.f/120);Vec3 initial{1,-2,0};
    vectorNear(a.vector(a.layout.velocity,a.alive.at(0)),initial+(Vec3{4,1,-2}-initial)*weight,"old field view sampled newly published values");
    vectorNear(b.vector(b.layout.velocity,b.alive.at(0)),initial+(Vec3{-2,3,5}-initial)*weight,"replacement field view sampled old values");
    require(first.bindField(next,error),error.c_str());step(rig,first,0,Base+1,2);auto switched=snapshot(rig,first.buffers(),false);auto expected=initial+(Vec3{4,1,-2}-initial)*weight;expected+=(Vec3{-2,3,5}-expected)*weight;
    vectorNear(switched.vector(switched.layout.velocity,switched.alive.at(0)),expected,"binding replacement reset particles or failed to switch snapshot");
    old.reset();scene.vectors->collect(rig.submitted);require(scene.storage->stats().snapshots==1,"replaced immutable snapshot did not retire after its fence");
    require(first.shutdown(rig.submitted,error)&&second.shutdown(rig.submitted,error),error.c_str());next.reset();scene.close(rig);++groups;
}
void bindingContracts(Rig& rig){
    Scene strong(rig,f::Consistency::Strong);require(!strong.vectors->bind(strong.lease,strong.spec,Base+1).accepted(),"Strong binding accepted a different snapshot tick");require(!strong.vectors->bind(strong.lease,strong.spec,Base-1).accepted(),"binding accepted a tick before its immutable snapshot");
    auto spec=strong.spec;spec.cellMetres=0;require(!strong.vectors->bind(strong.lease,spec,Base).accepted(),"zero metre cell accepted");spec=strong.spec;spec.originMetres.x=std::numeric_limits<float>::infinity();require(!strong.vectors->bind(strong.lease,spec,Base).accepted(),"nonfinite field origin accepted");
    f::LeaseRequest request;request.region=strong.region;request.channels={strong.wind[0],strong.wind[1]};request.owner="partial-vector";request.maxPages=64;auto partial=strong.storage->acquire(request);require(partial.accepted,partial.error.c_str());require(!strong.vectors->bind(partial.lease,strong.spec,Base).accepted(),"vector read bypassed a missing component lease");require(strong.storage->release(partial.lease),"partial lease release failed");
    f::GpuVectorFields tiny(strong.storage,{1,65535});require(!tiny.bind(strong.lease,strong.spec,Base).accepted(),"metadata ignored actual committed allocation size");std::string error;require(tiny.shutdown(rig.submitted,error),error.c_str());strong.close(rig);++groups;
}
void cookedRuntimeAndTickMapping(Rig& rig){Scene scene(rig);auto view=scene.bind();auto library=loadLibrary("build/effects-fields/cooked/library.json","assets");auto a=library.assets.at("wind_drift");require(a->fieldLevel=="worldVisible"&&a->fieldChannels==std::vector<std::string>{"windVelocity"}&&a->fieldVoxelMetres==1&&a->emitters[0].modules[1].kind==ModuleKind::VelocityField,"native cook lost vector channel or metre grid contract");GpuRuntime runtime(rig.device.p,{2048,2,64,16*1024*1024});runtime.install(a);require(runtime.spawn(a->id).status==SpawnStatus::UnsupportedFeature,"unbound authored velocity effect was silently accepted");auto spawned=runtime.spawn(a->id,{},19,view);require(spawned.status==SpawnStatus::Accepted,spawned.reason.c_str());
    rig.begin();std::string error;require(!runtime.step(rig.commands.p,rig.ticket(),rig.submitted,error)&&runtime.stats().tick==0,"missing field time mapping mutated the GPU instance tick");require(runtime.step(rig.commands.p,rig.ticket(),rig.submitted,error,Base),error.c_str());rig.submit();
    rig.begin();require(!runtime.step(rig.commands.p,rig.ticket(),rig.submitted,error,Base+1)&&runtime.stats().tick==1,"old query metadata bypassed explicit field tick validation");rig.submit();
    auto next=scene.bind(Base+1);require(runtime.bindField(spawned.handle,next,error),error.c_str());view=std::move(next);rig.begin();require(runtime.step(rig.commands.p,rig.ticket(),rig.submitted,error,Base+1)&&runtime.prepareDraw(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();auto batches=runtime.drawBatches();require(batches.size()==1,"authored velocity effect did not produce a GPU draw binding");auto gpu=snapshot(rig,batches[0].buffers,true);require(gpu.counters[9]>0&&gpu.counters[10]==0,"cooked runtime velocity module did not execute");
    auto stale=scene.bind(Base);require(runtime.bindField(spawned.handle,stale,error),error.c_str());rig.begin();require(!runtime.step(rig.commands.p,rig.ticket(),rig.submitted,error,Base),"runtime field clock moved backwards");rig.submit();stale.reset();require(runtime.shutdown(rig.submitted,error),error.c_str());view.reset();scene.close(rig);++groups;
}
void sharedWorldLifetime(Rig& rig){
    std::shared_ptr<f::GpuVectorView> view;std::weak_ptr<f::World> lifetime;
    {
        auto world=std::make_shared<f::World>(32);lifetime=world;std::array<f::ChannelId,3> ids{};
        for(unsigned i=0;i<3;++i){f::Channel c{"owner"+std::to_string(i),"m/s","owner",f::Domain::CpuAuthority,f::Merge::Sum};c.minimum=-100;c.maximum=100;ids[i]=world->registerChannel(c);}
        f::LeaseRequest request;request.region={{0,0,0},{2,2,2}};request.channels={ids.begin(),ids.end()};request.owner="owner";request.writable=true;request.maxPages=32;auto writer=world->acquire(request);require(writer.accepted,writer.error.c_str());
        for(auto id:ids){std::vector<f::Write> values;for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)values.push_back({{x,y,z},1});require(world->publish(writer.lease,id,1,1,1,f::Domain::CpuAuthority,values)==f::PublishResult::Accepted,"shared World fixture publication failed");}require(world->commit(1),"shared World commit failed");
        auto storage=std::make_shared<f::GpuFieldStorage>(world,rig.device.p);request.writable=false;request.owner="view";auto lease=storage->acquire(request);require(lease.accepted,lease.error.c_str());rig.begin();std::string error;require(storage->publish(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();storage->collect(rig.submitted);
        f::GpuVectorFields vectors(storage);f::GpuVectorSpec spec;spec.channels["windVelocity"]=ids;auto bound=vectors.bind(lease.lease,spec,1);require(bound.accepted(),bound.error.c_str());view=std::move(bound.view);require(storage->release(lease.lease)&&world->release(writer.lease),"shared World lease release failed");
    }
    require(!lifetime.expired(),"read view lost its shared World after renderer-side references were dropped");auto e=emitter();e.modules[0].vector={};GpuParticlePool pool(rig.device.p,e,1,{}, {},{},view);init(rig,pool);step(rig,pool,1,1);step(rig,pool,0,1,1);auto gpu=snapshot(rig,pool.buffers(),false);const float weight=1-std::exp(-3.f/120);vectorNear(gpu.vector(gpu.layout.velocity,gpu.alive.at(0)),{weight,weight,weight},"retained view stopped sampling after shared World owner replacement");std::string error;require(pool.shutdown(rig.submitted,error),error.c_str());view.reset();require(lifetime.expired(),"shared World leaked after the last completed read view");++groups;
}
}
int main(int argc,char** argv){try{std::string adapter;if(argc==3&&std::string(argv[1])=="--adapter")adapter=argv[2];else if(argc!=1)throw std::runtime_error("Usage: native_effect_field_gpu_tests [--adapter substring]");Rig rig(adapter);constantAndAuthority(rig);gradientAndLocal(rig);unknownStaleAndBounds(rig);twoVectorModulesAndUnits(rig);metadataFencesAndLeases(rig);immutableSnapshotSwitch(rig);bindingContracts(rig);cookedRuntimeAndTickMapping(rig);sharedWorldLifetime(rig);const auto errors=rig.debugErrors();require(errors==0,"D3D12 effect field debug errors");std::cout<<nlohmann::json({{"schema","nativegpu.effect-field-gpu-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"cpuVectorComparisons",comparisons},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"cpuAuthorityUnchanged",true},{"fieldReadback",false},{"runtimeCounterReadback","bounded-asynchronous"},{"validationReadback","synchronous-test-only"},{"readOnlyFieldConsumer",true}}).dump()<<'\n';}catch(const std::exception& error){std::cerr<<"GPU effect field failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
