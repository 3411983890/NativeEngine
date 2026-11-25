#include "../engine/native/cloth/batch_gpu.hpp"
#include "../engine/native/cloth/triangle_math.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace c=engine::cloth;namespace f=engine::fields;using gpu_test::Rig;using engine::Vec3;
namespace {
unsigned checks=0,groups=0,comparisons=0;float maxPositionError=0,maxVelocityError=0;
nlohmann::json timingEvidence;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F action,const char* why){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,why);}
Vec3 vector(const std::array<float,4>& v){return {v[0],v[1],v[2]};}float length(Vec3 v){return c::detail::length3(v);}
struct State {std::vector<c::GpuVertex> vertices;std::array<unsigned,16> diagnostics{};};
State snapshot(Rig& rig,c::GpuSolver& solver){
    const auto size=solver.buffers().vertexCount*sizeof(c::GpuVertex);gpu_test::Com<ID3D12Resource> read;gpu_test::makeBuffer(rig.device.p,read,size+64,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    rig.begin();std::string error;require(solver.prepareRead(rig.commands.p,rig.ticket(),error),error.c_str());auto b=solver.buffers();gpu_test::transition(rig.commands.p,b.vertices,b.vertexState,D3D12_RESOURCE_STATE_COPY_SOURCE);gpu_test::transition(rig.commands.p,b.diagnostics,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);rig.commands.p->CopyBufferRegion(read.p,0,b.vertices,0,size);rig.commands.p->CopyBufferRegion(read.p,size,b.diagnostics,0,64);gpu_test::transition(rig.commands.p,b.vertices,D3D12_RESOURCE_STATE_COPY_SOURCE,b.vertexState);gpu_test::transition(rig.commands.p,b.diagnostics,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);rig.submit();
    void* data=nullptr;D3D12_RANGE range{0,size+64};gpu_test::hr(read.p->Map(0,&range,&data),"Map grouped/reference state");State result;result.vertices.resize(b.vertexCount);std::memcpy(result.vertices.data(),data,size);std::memcpy(result.diagnostics.data(),static_cast<const char*>(data)+size,64);D3D12_RANGE none{0,0};read.p->Unmap(0,&none);return result;
}
void initialize(Rig& rig,c::GpuSolver& solver){rig.begin();std::string error;require(solver.initialize(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();}
void compare(const State& a,const State& b,float positionTolerance,float velocityTolerance){
    require(a.vertices.size()==b.vertices.size()&&a.diagnostics[0]==0&&b.diagnostics[0]==0,"Grouped/reference state invalid");
    for(unsigned i=0;i<a.vertices.size();++i){const float position=length(vector(a.vertices[i].position)-vector(b.vertices[i].position)),velocity=length(vector(a.vertices[i].velocity)-vector(b.vertices[i].velocity));maxPositionError=std::max(maxPositionError,position);maxVelocityError=std::max(maxVelocityError,velocity);require(position<positionTolerance,"Grouped/reference position difference exceeded tolerance");require(velocity<velocityTolerance,"Grouped/reference velocity difference exceeded tolerance");++comparisons;}
}

void exact(Rig& rig,c::GpuSolver& a,c::GpuSolver& b){auto x=snapshot(rig,a),y=snapshot(rig,b);compare(x,y,1e-8f,1e-8f);require(x.diagnostics==y.diagnostics,"Batch diagnostics differ");for(unsigned i=0;i<x.vertices.size();++i)require(std::memcmp(&x.vertices[i],&y.vertices[i],sizeof(c::GpuVertex))==0,"Batch position/previous/velocity/normal/tangent not bit exact");}
struct WindScene{
    std::shared_ptr<f::World> world=std::make_shared<f::World>(32);std::shared_ptr<f::GpuFieldStorage> storage;std::unique_ptr<f::GpuVectorFields> vectors;f::Lease writer;f::GpuLease lease;f::GpuVectorSpec spec;std::array<f::ChannelId,3> ids{};
    WindScene(Rig& rig){for(unsigned i=0;i<3;++i){f::Channel channel{"wind"+std::to_string(i),"m/s","wind",f::Domain::CpuAuthority,f::Merge::Sum};channel.minimum=-100000;channel.maximum=100000;channel.unitsPerInteger=1000;ids[i]=world->registerChannel(channel);}f::LeaseRequest r;r.region={{0,0,0},{5,5,5}};r.channels={ids.begin(),ids.end()};r.owner="wind";r.writable=true;r.maxPages=32;auto a=world->acquire(r);require(a.accepted,a.error.c_str());writer=a.lease;publish(1,false);storage=std::make_shared<f::GpuFieldStorage>(world,rig.device.p);r.region={{1,1,1},{4,4,4}};r.haloCells=1;r.writable=false;r.owner="cloth";r.maxAgeTicks=1;auto acquired=storage->acquire(r);require(acquired.accepted,acquired.error.c_str());lease=acquired.lease;spec.originMetres={-2,-2,-2};spec.channels["windVelocity"]=ids;vectors=std::make_unique<f::GpuVectorFields>(storage,f::GpuVectorLimits{8,8*65536ull});}
    void publish(f::Tick tick,bool missing){std::vector<f::Publication> batch;for(unsigned i=0;i<3;++i){std::vector<f::Write> writes;for(int z=0;z<5;++z)for(int y=0;y<5;++y)for(int x=0;x<5;++x){if(missing&&i==2&&x==2&&y==2&&z==2)continue;writes.push_back({{x,y,z},!missing&&i==2?5000:0});}batch.push_back({writer,ids[i],1,tick,tick,tick,f::Domain::CpuAuthority,std::move(writes)});}require(world->publishAtomic(batch)==f::PublishResult::Accepted&&world->commit(tick),"cloth wind fixture publication failed");}
    std::shared_ptr<f::GpuVectorView> bind(Rig& rig,f::Tick tick){rig.begin();std::string error;require(storage->publish(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();vectors->collect(rig.submitted);auto result=vectors->bind(lease,spec,tick);require(result.accepted(),result.error.c_str());return std::move(result.view);}
    void close(Rig& rig){std::string error;require(vectors->shutdown(rig.submitted,error),error.c_str());require(storage->release(lease,rig.submitted),"cloth field lease release failed");storage->collect(rig.submitted);require(storage->shutdown(rig.submitted,error),error.c_str());require(world->release(writer),"cloth wind writer release failed");world->collect(rig.submitted);}
};
void equivalence(Rig& rig,std::shared_ptr<const c::Asset> small,std::shared_ptr<const c::Asset> limit,std::shared_ptr<const c::Asset> fallback){
    c::GpuBatch batch(rig.device.p,rig.fence.p);std::vector<std::unique_ptr<c::GpuSolver>> batched,solo;
    for(auto asset:{small,limit,fallback}){c::GpuParameters p;p.wind={1,0,4};p.origin={float(batched.size())*3,0,0};batched.push_back(std::make_unique<c::GpuSolver>(rig.device.p,rig.fence.p,asset,c::GpuLimits{},p));solo.push_back(std::make_unique<c::GpuSolver>(rig.device.p,rig.fence.p,asset,c::GpuLimits{},p));initialize(rig,*batched.back());initialize(rig,*solo.back());}
    std::string error;for(unsigned tick=1;tick<=600;++tick){rig.begin();std::vector<c::GpuBatchStep> requests;
        for(unsigned i=0;i<batched.size();++i){if(i==2&&tick>10)continue;const bool paused=i==1&&tick>=40&&tick<60;batched[i]->setPaused(paused);solo[i]->setPaused(paused);
            if(tick==60){require(batched[i]->teleport(rig.commands.p,rig.ticket(),{float(i)*3+1,0,0},error),error.c_str());require(solo[i]->teleport(rig.commands.p,rig.ticket(),{float(i)*3+1,0,0},error),error.c_str());}
            if(tick==120){require(batched[i]->reset(rig.commands.p,rig.ticket(),error),error.c_str());require(solo[i]->reset(rig.commands.p,rig.ticket(),error),error.c_str());}
            if(tick==200){require(batched[i]->setWind({0,0,-3})&&solo[i]->setWind({0,0,-3}),"Wind update rejected");}
            const float dt=i==1?1.f/100:1.f/120;requests.push_back({batched[i].get(),dt,{}});require(solo[i]->step(rig.commands.p,rig.ticket(),dt,error),error.c_str());}
        require(batch.step(rig.commands.p,rig.ticket(),requests,error),error.c_str());rig.submit();
        if(tick==1||tick==10||tick==60||tick==120||tick==600)for(unsigned i=0;i<batched.size();++i)exact(rig,*batched[i],*solo[i]);
    }
    require(batch.stats().dispatches==600&&batch.stats().batchedSteps==1180&&batch.stats().fallbackSteps==10&&batch.stats().pausedSteps==20,"Batch accounting incorrect");require(batch.stats().activeSlots==0,"Completed slots retained");for(unsigned i=0;i<2;++i)require(batched[i]->stats().batchedTicks==(i?580:600),"Per-solver batch count incorrect");require(batch.shutdown(error),error.c_str());++groups;
}
void rejectionAndLifetime(Rig& rig,std::shared_ptr<const c::Asset> asset){
    c::GpuBatchLimits limits;limits.maxInstances=2;limits.maxInFlight=2;c::GpuBatch batch(rig.device.p,rig.fence.p,limits);c::GpuSolver a(rig.device.p,rig.fence.p,asset),b(rig.device.p,rig.fence.p,asset);initialize(rig,a);initialize(rig,b);std::string error;
    rejects([&]{auto bad=limits;bad.maxAllocationBytes=1;c::GpuBatch denied(rig.device.p,rig.fence.p,bad);},"Batch budget ignored");rejects([&]{auto bad=limits;bad.maxInFlight=0;c::GpuBatch denied(rig.device.p,rig.fence.p,bad);},"Zero ring admitted");
    gpu_test::Com<ID3D12Fence> other;gpu_test::hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(other.out())),"Create alternate timeline");c::GpuSolver wrong(rig.device.p,other.p,asset);
    rig.begin();require(!batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&wrong,1.f/120,{}}},error),"Foreign timeline accepted");require(!batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&a,1.f/120,{}}},error),"Duplicate solver accepted");require(!batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{nullptr,1.f/120,{}}},error),"Null solver accepted");require(!batch.step(rig.commands.p,rig.ticket(),{},error),"Empty batch accepted");require(!batch.step(rig.commands.p,rig.submitted,{{&a,1.f/120,{}}},error),"Completed fence accepted");require(!batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&b,std::numeric_limits<float>::quiet_NaN(),{}}},error),"Invalid late dt accepted");require(a.stats().tick==0&&b.stats().tick==0&&batch.stats().calls==0&&batch.stats().activeSlots==0,"Rejected batch partially advanced");
    require(batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&b,1.f/120,{}}},error),error.c_str());require(batch.step(rig.commands.p,rig.ticket(),{{&b,1.f/120,{}},{&a,1.f/120,{}}},error),error.c_str());require(!batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&b,1.f/120,{}}},error)&&a.stats().tick==2&&b.stats().tick==2,"Ring overwrite or partial third tick");
    b.setPaused(true);require(batch.step(rig.commands.p,rig.ticket(),{{&b,1.f/120,{}}},error)&&b.stats().tick==2,"Paused-only batch incorrectly requires a free slot");b.setPaused(false);
    require(!batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&b,1.f/120,{}},{&wrong,1.f/120,{}}},error),"Instance limit ignored");
    gpu_test::Com<ID3D12Fence> gate;gpu_test::hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.out())),"Create queue gate");gpu_test::hr(rig.queue.p->Wait(gate.p,1),"Queue gate wait");rig.submit(false);
    const auto held=batch.stats();const bool protectedState=held.activeSlots==2&&held.heldResourceReferences==48&&!batch.shutdown(error)&&!a.shutdown(error);
    gpu_test::hr(gate.p->Signal(1),"Release queue gate");rig.wait();require(protectedState,"Batch lifetime not protected by actual fence");batch.collect();require(batch.stats().activeSlots==0&&batch.stats().heldResourceReferences==0,"Completed references retained");
    rig.begin();require(batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,{}},{&b,1.f/120,{}}},error),error.c_str());rig.submit();require(batch.stats().dispatches==3,"Reclaimed ring not reusable");require(batch.shutdown(error)&&a.shutdown(error)&&b.shutdown(error),error.c_str());++groups;
}
void fields(Rig& rig,std::shared_ptr<const c::Asset> asset){
    WindScene wind(rig);auto view=wind.bind(rig,1);c::GpuParameters p;p.windField=view;c::GpuSolver a(rig.device.p,rig.fence.p,asset,{},p),b(rig.device.p,rig.fence.p,asset,{},p);p.windField.reset();initialize(rig,a);initialize(rig,b);c::GpuBatch batch(rig.device.p,rig.fence.p);std::string error;const auto digest=wind.world->digest();
    auto step=[&](f::Tick tick){rig.begin();require(batch.step(rig.commands.p,rig.ticket(),{{&a,1.f/120,tick}},error),error.c_str());require(b.step(rig.commands.p,rig.ticket(),1.f/120,error,tick),error.c_str());rig.submit();exact(rig,a,b);};for(unsigned i=0;i<5;++i)step(1);require(wind.world->digest()==digest,"Batch wrote CPU authority field");
    wind.publish(2,true);auto missing=wind.bind(rig,2);require(a.bindWindField(missing,error)&&b.bindWindField(missing,error),error.c_str());step(2);auto stale=wind.bind(rig,4);require(a.bindWindField(stale,error)&&b.bindWindField(stale,error),error.c_str());step(4);
    const auto before=a.stats().tick;rig.begin();require(!batch.step(rig.commands.p,rig.ticket(),{{&b,1.f/120,4},{&a,1.f/120,3}},error)&&a.stats().tick==before&&b.stats().tick==before,"Late field-time rejection mutated batch");rig.submit();require(batch.stats().dispatches==7&&a.stats().dispatches==23&&a.stats().batchedTicks==7,"Field dispatch accounting incorrect");require(batch.shutdown(error)&&a.shutdown(error)&&b.shutdown(error),error.c_str());view.reset();missing.reset();stale.reset();wind.close(rig);++groups;
}
double percentile(std::vector<double> values,double q){std::sort(values.begin(),values.end());return values[static_cast<std::size_t>(std::ceil(values.size()*q))-1];}
struct Timer {
    gpu_test::Com<ID3D12QueryHeap> queries;gpu_test::Com<ID3D12Resource> read;std::uint64_t frequency=0;
    explicit Timer(Rig& rig){D3D12_QUERY_HEAP_DESC d{};d.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;d.Count=2;gpu_test::hr(rig.device.p->CreateQueryHeap(&d,IID_PPV_ARGS(queries.out())),"Create cloth benchmark timestamps");gpu_test::makeBuffer(rig.device.p,read,16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);gpu_test::hr(rig.queue.p->GetTimestampFrequency(&frequency),"Get cloth benchmark frequency");require(frequency>0,"No GPU timestamp frequency");}
    std::pair<double,double> measure(Rig& rig,std::vector<std::unique_ptr<c::GpuSolver>>& solvers,c::GpuBatch* batch=nullptr){
        const auto begin=std::chrono::steady_clock::now();rig.begin();rig.commands.p->EndQuery(queries.p,D3D12_QUERY_TYPE_TIMESTAMP,0);std::string error;
        for(unsigned tick=0;tick<2;++tick){if(batch){std::vector<c::GpuBatchStep> requests;for(auto& solver:solvers)requests.push_back({solver.get(),1.f/120,{}});require(batch->step(rig.commands.p,rig.ticket(),requests,error),error.c_str());}else for(auto& solver:solvers)require(solver->step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());}
        rig.commands.p->EndQuery(queries.p,D3D12_QUERY_TYPE_TIMESTAMP,1);rig.commands.p->ResolveQueryData(queries.p,D3D12_QUERY_TYPE_TIMESTAMP,0,2,read.p,0);const auto end=std::chrono::steady_clock::now();rig.submit();
        void* data=nullptr;D3D12_RANGE range{0,16};gpu_test::hr(read.p->Map(0,&range,&data),"Map completed cloth timestamps");std::uint64_t ticks[2];std::memcpy(ticks,data,16);D3D12_RANGE none{0,0};read.p->Unmap(0,&none);require(ticks[1]>ticks[0],"GPU benchmark timestamps are empty or out of order");return {double(ticks[1]-ticks[0])*1000/double(frequency),std::chrono::duration<double,std::milli>(end-begin).count()};
    }
};

void lateFields(Rig& rig,std::shared_ptr<const c::Asset> asset){
    WindScene wind(rig);auto view=wind.bind(rig,1);c::GpuParameters p;p.reserveWindField=true;c::GpuSolver late(rig.device.p,rig.fence.p,asset,{},p),uniform(rig.device.p,rig.fence.p,asset);p.windField=view;c::GpuSolver reference(rig.device.p,rig.fence.p,asset,{},p);
    initialize(rig,late);initialize(rig,uniform);initialize(rig,reference);c::GpuBatch batch(rig.device.p,rig.fence.p);std::string error;
    rig.begin();require(batch.step(rig.commands.p,rig.ticket(),{{&late,1.f/120,{}}},error)&&uniform.step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());rig.submit();exact(rig,late,uniform);
    require(!uniform.bindWindField(view,error),"Unreserved solver accepted late field binding");require(!late.bindWindField({},error),"Null field accepted");
    rig.begin();require(late.reset(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();require(late.bindWindField(view,error),error.c_str());
    for(unsigned i=0;i<6;++i){rig.begin();require(batch.step(rig.commands.p,rig.ticket(),{{&late,1.f/120,1}},error)&&reference.step(rig.commands.p,rig.ticket(),1.f/120,error,1),error.c_str());rig.submit();}exact(rig,late,reference);
    require(late.clearWindField(error)&&reference.clearWindField(error)&&late.setWind({0,0,2})&&reference.setWind({0,0,2}),"Field removal failed");
    rig.begin();require(batch.step(rig.commands.p,rig.ticket(),{{&late,1.f/120,{}}},error)&&reference.step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());rig.submit();exact(rig,late,reference);require(snapshot(rig,late).diagnostics[9]==0,"Removed field retained installed spatial wind");
    require(batch.shutdown(error)&&late.shutdown(error)&&reference.shutdown(error)&&uniform.shutdown(error),error.c_str());view.reset();p.windField.reset();wind.close(rig);++groups;
}
void benchmark(Rig& rig,std::shared_ptr<const c::Asset> asset){
    std::vector<std::unique_ptr<c::GpuSolver>> batched,solo;for(unsigned i=0;i<4;++i){c::GpuParameters p;p.wind={0,0,4};p.origin={float(i)*2,0,0};for(auto* list:{&batched,&solo}){list->push_back(std::make_unique<c::GpuSolver>(rig.device.p,rig.fence.p,asset,c::GpuLimits{},p));initialize(rig,*list->back());}}
    c::GpuBatch batch(rig.device.p,rig.fence.p);Timer timer(rig);std::vector<double> fastGpu,slowGpu,fastCpu,slowCpu;
    for(unsigned sample=0;sample<68;++sample){std::pair<double,double> fast,slow;if(sample%2){fast=timer.measure(rig,batched,&batch);slow=timer.measure(rig,solo);}else{slow=timer.measure(rig,solo);fast=timer.measure(rig,batched,&batch);}if(sample<8)continue;fastGpu.push_back(fast.first);slowGpu.push_back(slow.first);fastCpu.push_back(fast.second);slowCpu.push_back(slow.second);}
    timingEvidence={{"instances",4},{"vertices",2048},{"fixedTicksPerSample",2},{"warmupSamples",8},{"measuredSamples",60},{"alternatingOrder",true},{"timestampFrequencyHz",timer.frequency},{"batchGpuP50Ms",percentile(fastGpu,.5)},{"batchGpuP95Ms",percentile(fastGpu,.95)},{"soloGroupGpuP50Ms",percentile(slowGpu,.5)},{"soloGroupGpuP95Ms",percentile(slowGpu,.95)},{"batchCpuRecordP95Ms",percentile(fastCpu,.95)},{"soloGroupCpuRecordP95Ms",percentile(slowCpu,.95)},{"samplesMs",{{"batchGpu",fastGpu},{"soloGroupGpu",slowGpu},{"batchCpuRecord",fastCpu},{"soloGroupCpuRecord",slowCpu}}},{"serviceAccountedBytes",batch.stats().accountedBytes},{"metadataAllocationBytes",batch.stats().metadataAllocationBytes},{"descriptorEstimateBytes",batch.stats().descriptorEstimateBytes}};
    std::string error;require(batch.shutdown(error),error.c_str());++groups;
}
}
int main(int argc,char** argv){try{std::string adapter;if(argc==3&&std::string(argv[1])=="--adapter")adapter=argv[2];else if(argc!=1)throw std::runtime_error("Usage: native_cloth_batch_tests [--adapter substring]");
    auto library=c::loadClothLibrary("build/cloth-grouped/cooked/library.json","assets"),fixtures=c::loadClothLibrary("build/cloth-grouped/fixtures/library.json","assets"),patch=c::loadClothLibrary("build/cloth-draw/fixtures/library.json","assets");Rig rig(adapter);equivalence(rig,library.cloths.at("flag_banner"),fixtures.cloths.at("group_limit"),fixtures.cloths.at("dispatch_fallback"));rejectionAndLifetime(rig,library.cloths.at("flag_banner"));fields(rig,patch.cloths.at("solver_patch"));lateFields(rig,patch.cloths.at("solver_patch"));benchmark(rig,fixtures.cloths.at("group_limit"));const auto errors=rig.debugErrors();require(errors==0,"Batch D3D12 debug errors");
    std::cout<<nlohmann::json{{"schema","nativegpu.cloth-batch-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"vertexComparisons",comparisons},{"maxPositionDifferenceMetres",maxPositionError},{"maxVelocityDifferenceMetresPerSecond",maxVelocityError},{"timings",timingEvidence},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"runtimeReadback",false},{"scope","incremental cloth batch equivalence, lifetime and paired 2048-vertex benchmark; not combined A-G acceptance"}}.dump()<<'\n';
}catch(const std::exception& e){std::cerr<<"Cloth batch failed after "<<checks<<" checks: "<<e.what()<<"; max position="<<maxPositionError<<" max velocity="<<maxVelocityError<<'\n';return 1;}}
