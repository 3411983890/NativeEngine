#include "../engine/native/cloth/solver_gpu.hpp"
#include "../engine/native/cloth/solver_cpu.hpp"
#include "../engine/native/cloth/triangle_math.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <algorithm>
#include <limits>

namespace c=engine::cloth;namespace f=engine::fields;using engine::Vec3;using gpu_test::Rig;
namespace {
unsigned checks=0,groups=0,comparisons=0;float maxPositionError=0,maxVelocityError=0;
nlohmann::json qualityEvidence=nlohmann::json::array(),contactEvidence;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F fn,const char* why){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}require(failed,why);}
float length(Vec3 v){return c::detail::length3(v);}Vec3 vector(const std::array<float,4>& v){return {v[0],v[1],v[2]};}
struct Snapshot {std::vector<c::GpuVertex> vertices;std::vector<std::array<float,4>> wind;std::array<unsigned,16> diagnostics{};};
Snapshot read(Rig& rig,c::GpuSolver& solver){
    auto b=solver.buffers();const std::size_t vertexBytes=b.vertexCount*sizeof(c::GpuVertex),windBytes=b.vertexCount*16ull,total=vertexBytes+windBytes+64;gpu_test::Com<ID3D12Resource> staging;gpu_test::makeBuffer(rig.device.p,staging,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    rig.begin();std::string error;require(solver.prepareRead(rig.commands.p,rig.ticket(),error),error.c_str());b=solver.buffers();ID3D12Resource* resources[]={b.vertices,b.wind,b.diagnostics};const std::size_t sizes[]={vertexBytes,windBytes,64},offsets[]={0,vertexBytes,vertexBytes+windBytes};const D3D12_RESOURCE_STATES states[]={b.vertexState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    for(unsigned i=0;i<3;++i){gpu_test::transition(rig.commands.p,resources[i],states[i],D3D12_RESOURCE_STATE_COPY_SOURCE);rig.commands.p->CopyBufferRegion(staging.p,offsets[i],resources[i],0,sizes[i]);gpu_test::transition(rig.commands.p,resources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,states[i]);}rig.submit();
    Snapshot s;s.vertices.resize(b.vertexCount);s.wind.resize(b.vertexCount);void* data=nullptr;D3D12_RANGE range{0,total};gpu_test::hr(staging.p->Map(0,&range,&data),"Map cloth validation");auto bytes=static_cast<const char*>(data);std::memcpy(s.vertices.data(),bytes,vertexBytes);std::memcpy(s.wind.data(),bytes+vertexBytes,windBytes);std::memcpy(s.diagnostics.data(),bytes+vertexBytes+windBytes,64);D3D12_RANGE none{0,0};staging.p->Unmap(0,&none);return s;
}
void initialize(Rig& rig,c::GpuSolver& solver){rig.begin();std::string error;require(solver.initialize(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();}
void step(Rig& rig,c::GpuSolver& solver,std::optional<f::Tick> tick={}){rig.begin();std::string error;require(solver.step(rig.commands.p,rig.ticket(),1.f/120,error,tick),error.c_str());rig.submit();}
void compare(const Snapshot& s,const c::Solver& cpu,float posTolerance,float velocityTolerance){
    require(s.vertices.size()==cpu.vertexCount()&&s.diagnostics[0]==0,"GPU cloth dimensions/non-finite counter invalid");
    for(unsigned i=0;i<s.vertices.size();++i){float p=length(vector(s.vertices[i].position)-cpu.position(i)),v=length(vector(s.vertices[i].velocity)-cpu.velocity(i));maxPositionError=std::max(maxPositionError,p);maxVelocityError=std::max(maxVelocityError,v);require(p<posTolerance,"GPU cloth position differs from triangular CPU reference");require(v<velocityTolerance,"GPU cloth velocity differs from triangular CPU reference");++comparisons;}
}
void basis(const Snapshot& s){for(const auto& v:s.vertices){const auto n=vector(v.normal),t=vector(v.tangent);require(std::fabs(length(n)-1)<3e-4f&&std::fabs(length(t)-1)<3e-4f&&std::fabs(c::detail::dot3(n,t))<3e-4f,"GPU cloth output basis is not orthonormal");require(v.tangent[3]==1||v.tangent[3]==-1,"GPU cloth tangent handedness missing");}}
void analytical(const c::Asset& asset){
    const auto& t=asset.triangles.front();const auto& m=asset.membranes.front();std::array<Vec3,3> p{asset.rest[t[0]],asset.rest[t[1]],asset.rest[t[2]]};p[1].x+=.02f;p[2].z+=.01f;
    for(unsigned kind=0;kind<3;++kind){float value;std::array<Vec3,3> g;require(c::detail::membrane(p,m,kind,value,g),"analytic membrane unavailable");for(unsigned i=0;i<3;++i)for(unsigned axis=0;axis<3;++axis){auto a=p,b=p;float* ap[]={&a[i].x,&a[i].y,&a[i].z};float* bp[]={&b[i].x,&b[i].y,&b[i].z};*ap[axis]+=1e-5f;*bp[axis]-=1e-5f;float ca,cb;std::array<Vec3,3> unused;c::detail::membrane(a,m,kind,ca,unused);c::detail::membrane(b,m,kind,cb,unused);const float gradient[]={g[i].x,g[i].y,g[i].z};require(std::fabs((ca-cb)/2e-5f-gradient[axis])<.025f,"triangle strain gradient differs from finite differences");}}
    std::array<Vec3,4> fold{{{-.4f,.1f,-.1f},{.7f,.2f,.1f},{0,.8f,.3f},{.2f,-.6f,.4f}}};float angle;std::array<Vec3,4> g;require(c::detail::dihedral(fold,angle,g),"analytic hinge unavailable");Vec3 sum{},torque{};for(unsigned i=0;i<4;++i){sum+=g[i];torque+=c::detail::cross3(fold[i],g[i]);for(unsigned axis=0;axis<3;++axis){auto a=fold,b=fold;float* ap[]={&a[i].x,&a[i].y,&a[i].z};float* bp[]={&b[i].x,&b[i].y,&b[i].z};*ap[axis]+=.0005f;*bp[axis]-=.0005f;float ca,cb;std::array<Vec3,4> unused;c::detail::dihedral(a,ca,unused);c::detail::dihedral(b,cb,unused);const float gradient[]={g[i].x,g[i].y,g[i].z};require(std::fabs(c::detail::angleDifference(ca,cb)/.001f-gradient[axis])<.002f,"signed dihedral gradient differs from finite differences");}}
    require(length(sum)<1e-5f&&length(torque)<1e-5f,"hinge force violates rigid translation/rotation invariance");
    for(unsigned mode=0;mode<5;++mode){auto bad=asset;switch(mode){case 0:bad.membraneColors[0].push_back(bad.membraneColors[1].front());break;case 1:bad.hinges[0].vertices[3]=bad.hinges[0].vertices[2];break;case 2:bad.membranes[0].inverse[0]*=2;break;case 3:bad.adjacency[0].clear();break;case 4:bad.hingeColors[0].clear();break;}rejects([&]{c::validateTriangleTopology(bad);},"invalid/racing cloth topology accepted");}
    ++groups;
}
void reference(Rig& rig,std::shared_ptr<const c::Asset> asset){
    c::GpuSolver gpu(rig.device.p,rig.fence.p,asset);c::Solver cpu(asset);initialize(rig,gpu);auto initial=read(rig,gpu);compare(initial,cpu,1e-6f,1e-6f);basis(initial);require(gpu.setWind({0,0,4}),"set GPU wind failed");cpu.setWind({0,0,4});
    for(unsigned tick=1;tick<=120;++tick){step(rig,gpu);cpu.step(1.f/120);if(tick==1||tick==5||tick==20||tick==60||tick==120){auto state=read(rig,gpu);compare(state,cpu,.003f,.5f);basis(state);}}
    std::string error;require(gpu.shutdown(error),error.c_str());require(gpu.stats().allocationBytes==0,"cloth buffers retained after shutdown");++groups;
}
void quality(Rig& rig,std::shared_ptr<const c::Asset> asset){
    c::GpuSolver gpu(rig.device.p,rig.fence.p,asset);initialize(rig,gpu);require(gpu.setWind({0,0,4}),"cloth wind failed");
    for(unsigned tick=0;tick<600;++tick)step(rig,gpu);
    auto state=read(rig,gpu);require(state.diagnostics[0]==0,"long GPU cloth solve produced non-finite vertices");
    std::vector<float> errors;float maxSpeed=0,pinDrift=0;
    for(const auto& edge:asset->stretch)errors.push_back(std::fabs(length(vector(state.vertices[edge.b].position)-vector(state.vertices[edge.a].position))-edge.rest)/edge.rest);
    std::sort(errors.begin(),errors.end());const float strainP95=errors[static_cast<std::size_t>(std::ceil(errors.size()*.95))-1];
    require(strainP95<.05f,"GPU cloth edge strain P95 exceeded 5%");
    for(const auto& v:state.vertices)maxSpeed=std::max(maxSpeed,length(vector(v.velocity)));
    require(maxSpeed<25,"GPU cloth gained unbounded speed");
    for(const auto& pin:asset->pins){const float drift=length(vector(state.vertices[pin.vertex].position)-pin.anchor);pinDrift=std::max(pinDrift,drift);require(drift<1e-6f,"GPU cloth pin drifted");}
    basis(state);const auto stats=gpu.stats();
    qualityEvidence.push_back({{"asset",asset->id},{"ticks",600},{"durationSeconds",5},{"widthMetres",asset->rest[asset->columns-1].x-asset->rest[0].x},{"heightMetres",asset->rest.back().y-asset->rest[0].y},{"vertices",asset->rest.size()},{"edgeErrorP95",strainP95},{"maxSpeedMetresPerSecond",maxSpeed},{"maxPinDriftMetres",pinDrift},{"allocationBytes",stats.allocationBytes},{"membraneColors",stats.membraneColors},{"hingeColors",stats.hingeColors},{"dispatches",stats.dispatches}});
    std::string error;require(gpu.shutdown(error),error.c_str());++groups;
}
void contacts(Rig& rig,std::shared_ptr<const c::Asset> source){
    auto asset=std::make_shared<c::Asset>(*source);asset->collision.plane={true,{0,1,0},.05f};asset->collision.spheres.push_back({{0,.45f,0},.26f});asset->collision.capsules.push_back({{.5f,.2f,-.05f},{.5f,.7f,.05f},.08f});c::GpuSolver gpu(rig.device.p,rig.fence.p,asset);initialize(rig,gpu);gpu.setWind({0,0,3});
    for(unsigned tick=1;tick<=240;++tick){step(rig,gpu);if(tick!=1&&tick!=2&&tick!=30&&tick!=120&&tick!=240)continue;auto state=read(rig,gpu);require(state.diagnostics[0]==0,"cloth contacts became non-finite");require(state.diagnostics[11]>0,"cloth contact fixture did not exercise projection");
        std::vector<float> penetration;for(const auto& v:state.vertices){auto p=vector(v.position);float value=std::max(0.f,asset->collision.plane.offsetMetres-p.y);for(const auto& s:asset->collision.spheres)value=std::max(value,s.radius-length(p-s.centre));for(const auto& capsule:asset->collision.capsules){const auto axis=capsule.b-capsule.a;const float t=std::clamp(c::detail::dot3(p-capsule.a,axis)/c::detail::dot3(axis,axis),0.f,1.f);value=std::max(value,capsule.radius-length(p-(capsule.a+axis*t)));}penetration.push_back(std::max(0.f,value));}std::sort(penetration.begin(),penetration.end());const float p99=penetration[static_cast<std::size_t>(std::ceil(penetration.size()*.99))-1];require(penetration.back()<.01f&&p99<.002f,"GPU cloth contact penetration exceeded 10mm max/2mm P99");
        if(contactEvidence.is_null())contactEvidence={{"ticks",240},{"durationSeconds",2},{"sampledTicks",{1,2,30,120,240}},{"sampledMaxPenetrationMetres",0.f},{"sampledMaxP99PenetrationMetres",0.f}};
        contactEvidence["sampledMaxPenetrationMetres"]=std::max(contactEvidence["sampledMaxPenetrationMetres"].get<float>(),penetration.back());contactEvidence["sampledMaxP99PenetrationMetres"]=std::max(contactEvidence["sampledMaxP99PenetrationMetres"].get<float>(),p99);
        if(tick==240){require(state.diagnostics[12]>0&&state.diagnostics[13]>0&&state.diagnostics[14]>0,"plane/sphere/capsule projections were not each exercised");contactEvidence["planeProjections"]=state.diagnostics[12];contactEvidence["sphereProjections"]=state.diagnostics[13];contactEvidence["capsuleProjections"]=state.diagnostics[14];}
    }
    std::string error;require(gpu.shutdown(error),error.c_str());++groups;
}
void lifecycle(Rig& rig,std::shared_ptr<const c::Asset> asset){
    rejects([&]{c::GpuSolver bad(rig.device.p,rig.fence.p,asset,{16384,8192,1});},"aligned allocation budget ignored");auto skinned=std::make_shared<c::Asset>(*asset);skinned->skinned=true;rejects([&]{c::GpuSolver bad(rig.device.p,rig.fence.p,skinned);},"bone attachment silently treated as static pins");
    c::GpuSolver gpu(rig.device.p,rig.fence.p,asset);require(gpu.buffers().vertices==nullptr,"uninitialized GPU cloth exposed vertex data");initialize(rig,gpu);step(rig,gpu);auto before=read(rig,gpu);const auto tick=gpu.stats().tick;std::string error;rig.begin();require(!gpu.step(rig.commands.p,rig.ticket(),0,error)&&!gpu.step(rig.commands.p,rig.ticket(),std::numeric_limits<float>::quiet_NaN(),error)&&!gpu.step(rig.commands.p,rig.submitted,1.f/120,error),"invalid tick or completed fence accepted");gpu.setPaused(true);require(gpu.step(rig.commands.p,rig.ticket(),1.f/120,error)&&gpu.stats().tick==tick,"paused cloth advanced");rig.submit();gpu.setPaused(false);
    rig.begin();require(gpu.teleport(rig.commands.p,rig.ticket(),{10,2,-3},error),error.c_str());rig.submit();auto moved=read(rig,gpu);for(unsigned i=0;i<moved.vertices.size();++i){require(length(vector(moved.vertices[i].position)-vector(before.vertices[i].position)-Vec3{10,2,-3})<2e-6f,"teleport did not preserve deformation");require(length(vector(moved.vertices[i].velocity))==0,"teleport carried velocity");}require(!gpu.setSelfCollision(true,error)&&gpu.setSelfCollision(false,error),"GPU self-collision declaration ignored");rig.begin();require(gpu.reset(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();auto reset=read(rig,gpu);require(gpu.stats().tick==0,"reset did not reset the visual tick");for(unsigned i=0;i<reset.vertices.size();++i)require(length(vector(reset.vertices[i].position)-asset->rest[i]-Vec3{10,2,-3})<2e-6f,"reset lost the current origin");require(gpu.shutdown(error),error.c_str());
    c::GpuSolver bounded(rig.device.p,rig.fence.p,asset,{16384,1,64*1024*1024});initialize(rig,bounded);rig.begin();require(!bounded.step(rig.commands.p,rig.ticket(),1.f/120,error)&&bounded.stats().tick==0,"dispatch budget did not reject before mutating state");rig.submit();require(bounded.shutdown(error),error.c_str());
    c::GpuSolver held(rig.device.p,rig.fence.p,asset);gpu_test::Com<ID3D12Fence> gate;gpu_test::hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.out())),"Create cloth queue gate");gpu_test::hr(rig.queue.p->Wait(gate.p,1),"Gate cloth queue");rig.begin();bool recorded=held.initialize(rig.commands.p,rig.ticket(),error)&&held.step(rig.commands.p,rig.ticket(),1.f/120,error);rig.submit(false);bool released=held.shutdown(error);gpu_test::hr(gate.p->Signal(1),"Release cloth gate");rig.wait();require(recorded&&!released,"cloth released before the actual GPU fence");require(held.shutdown(error)&&held.stats().allocationBytes==0,"cloth resources did not retire after fence");++groups;
}
struct WindScene{
    std::shared_ptr<f::World> world=std::make_shared<f::World>(32);std::shared_ptr<f::GpuFieldStorage> storage;std::unique_ptr<f::GpuVectorFields> vectors;f::Lease writer;f::GpuLease lease;f::GpuVectorSpec spec;std::array<f::ChannelId,3> ids{};
    WindScene(Rig& rig){for(unsigned i=0;i<3;++i){f::Channel channel{"wind"+std::to_string(i),"m/s","wind",f::Domain::CpuAuthority,f::Merge::Sum};channel.minimum=-100000;channel.maximum=100000;channel.unitsPerInteger=1000;ids[i]=world->registerChannel(channel);}f::LeaseRequest r;r.region={{0,0,0},{5,5,5}};r.channels={ids.begin(),ids.end()};r.owner="wind";r.writable=true;r.maxPages=32;auto a=world->acquire(r);require(a.accepted,a.error.c_str());writer=a.lease;publish(1,false);storage=std::make_shared<f::GpuFieldStorage>(world,rig.device.p);r.region={{1,1,1},{4,4,4}};r.haloCells=1;r.writable=false;r.owner="cloth";r.maxAgeTicks=1;auto acquired=storage->acquire(r);require(acquired.accepted,acquired.error.c_str());lease=acquired.lease;spec.originMetres={-2,-2,-2};spec.channels["windVelocity"]=ids;vectors=std::make_unique<f::GpuVectorFields>(storage,f::GpuVectorLimits{8,8*65536ull});}
    void publish(f::Tick tick,bool missing){std::vector<f::Publication> batch;for(unsigned i=0;i<3;++i){std::vector<f::Write> writes;for(int z=0;z<5;++z)for(int y=0;y<5;++y)for(int x=0;x<5;++x){if(missing&&i==2&&x==2&&y==2&&z==2)continue;writes.push_back({{x,y,z},!missing&&i==2?5000:0});}batch.push_back({writer,ids[i],1,tick,tick,tick,f::Domain::CpuAuthority,std::move(writes)});}require(world->publishAtomic(batch)==f::PublishResult::Accepted&&world->commit(tick),"cloth wind fixture publication failed");}
    std::shared_ptr<f::GpuVectorView> bind(Rig& rig,f::Tick tick){rig.begin();std::string error;require(storage->publish(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();vectors->collect(rig.submitted);auto result=vectors->bind(lease,spec,tick);require(result.accepted(),result.error.c_str());return std::move(result.view);}
    void close(Rig& rig){std::string error;require(vectors->shutdown(rig.submitted,error),error.c_str());require(storage->release(lease,rig.submitted),"cloth field lease release failed");storage->collect(rig.submitted);require(storage->shutdown(rig.submitted,error),error.c_str());require(world->release(writer),"cloth wind writer release failed");world->collect(rig.submitted);}
};
void fieldWind(Rig& rig,std::shared_ptr<const c::Asset> asset){
    WindScene field(rig);auto first=field.bind(rig,1);c::GpuParameters parameters;parameters.windField=first;c::GpuSolver gpu(rig.device.p,rig.fence.p,asset,{},parameters);parameters.windField.reset();c::Solver cpu(asset);cpu.setWindSamples(std::vector<Vec3>(asset->rest.size(),{0,0,5}));initialize(rig,gpu);const auto digest=field.world->digest();for(unsigned i=0;i<5;++i){step(rig,gpu,1);cpu.step(1.f/120);}require(field.world->digest()==digest,"GPU cloth wrote into CPU authority wind");auto known=read(rig,gpu);compare(known,cpu,.001f,.1f);require(known.diagnostics[5]==5&&known.diagnostics[9]==1,"GPU cloth did not apply complete wind samples");
    field.publish(2,true);auto second=field.bind(rig,2);std::string error;require(gpu.bindWindField(second,error),error.c_str());step(rig,gpu,2);cpu.step(1.f/120);auto partial=read(rig,gpu);require(partial.diagnostics[4]==1&&partial.diagnostics[3]>0&&partial.diagnostics[3]<asset->rest.size(),"partial spatial sample did not reject the whole cloth wind batch");for(const auto& w:partial.wind)require(w[2]==5&&w[0]==0&&w[1]==0,"failed wind batch replaced only some vertices");compare(partial,cpu,.001f,.1f);
    auto stale=field.bind(rig,4);require(gpu.bindWindField(stale,error),error.c_str());step(rig,gpu,4);cpu.step(1.f/120);auto old=read(rig,gpu);require(old.diagnostics[4]==2&&old.diagnostics[6]>0,"stale cloth wind batch was silently accepted");compare(old,cpu,.001f,.1f);rig.begin();require(!gpu.step(rig.commands.p,rig.ticket(),1.f/120,error,3),"cloth ignored mismatched field tick");require(gpu.bindWindField(first,error),error.c_str());require(!gpu.step(rig.commands.p,rig.ticket(),1.f/120,error,1),"cloth field clock moved backwards");rig.submit();require(gpu.bindWindField(stale,error),error.c_str());rig.begin();require(gpu.teleport(rig.commands.p,rig.ticket(),{10,0,0},error),error.c_str());rig.submit();cpu.setOrigin({10,0,0});step(rig,gpu,4);cpu.step(1.f/120);auto moved=read(rig,gpu);require(moved.diagnostics[9]==0,"teleport retained wind from the old world location");compare(moved,cpu,.001f,.1f);require(gpu.shutdown(error),error.c_str());first.reset();second.reset();stale.reset();field.close(rig);++groups;
}
}
int main(int argc,char** argv){
    try{
        std::string adapter,libraryPath="build/cloth-gpu/cooked/library.json",fixturePath="build/cloth-gpu/fixtures/library.json";
        for(int i=1;i<argc;i+=2){
            if(i+1>=argc)throw std::runtime_error("Missing GPU cloth test option value");
            const std::string option=argv[i];
            if(option=="--adapter")adapter=argv[i+1];
            else if(option=="--library")libraryPath=argv[i+1];
            else if(option=="--fixtures")fixturePath=argv[i+1];
            else throw std::runtime_error("Usage: native_cloth_gpu_tests [--adapter substring] [--library path] [--fixtures path]");
        }
        auto library=c::loadClothLibrary(libraryPath,"assets");
        auto patch=c::loadClothLibrary(fixturePath,"assets").cloths.at("solver_patch");
        analytical(*patch);Rig rig(adapter);reference(rig,patch);quality(rig,library.cloths.at("flag_banner"));quality(rig,patch);contacts(rig,patch);lifecycle(rig,patch);fieldWind(rig,patch);
        auto errors=rig.debugErrors();require(errors==0,"GPU cloth D3D12 debug errors");
        std::cout<<nlohmann::json{{"schema","nativegpu.cloth-gpu-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"cpuVertexComparisons",comparisons},{"maxPositionDifferenceMetres",maxPositionError},{"maxVelocityDifferenceMetresPerSecond",maxVelocityError},{"quality",qualityEvidence},{"contacts",contactEvidence},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"physicalModel","triangle-strain+signed-dihedral-xpbd"},{"runtimeReadback",false},{"validationReadback","test-only"},{"hostRendering",false}}.dump()<<'\n';
    }catch(const std::exception& e){std::cerr<<"GPU cloth failed after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
