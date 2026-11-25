#include "../engine/native/cloth/solver_gpu.hpp"
#include "../engine/native/cloth/triangle_math.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace c=engine::cloth;using gpu_test::Rig;using engine::Vec3;
namespace {
unsigned checks=0,groups=0,comparisons=0;float maxPositionError=0,maxVelocityError=0;
nlohmann::json comparisonEvidence=nlohmann::json::array(),timingEvidence=nlohmann::json::array();
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
void equivalence(Rig& rig,std::shared_ptr<const c::Asset> asset,unsigned ticks){
    c::GpuParameters fast,reference;fast.execution=c::GpuExecution::Grouped;reference.execution=c::GpuExecution::Dispatch;fast.wind=reference.wind={0,0,4};c::GpuSolver grouped(rig.device.p,rig.fence.p,asset,{},fast),dispatch(rig.device.p,rig.fence.p,asset,{},reference);initialize(rig,grouped);initialize(rig,dispatch);std::string error;
    for(unsigned tick=1;tick<=ticks;++tick){rig.begin();require(grouped.step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());require(dispatch.step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());rig.submit();if(tick==1||tick==2||tick==10||tick==30||tick==60||tick==120||tick==ticks){auto a=snapshot(rig,grouped),b=snapshot(rig,dispatch);
#if defined(CLOTH_GROUP_TRACE)
        float p=0,v=0;for(unsigned i=0;i<a.vertices.size();++i){p=std::max(p,length(vector(a.vertices[i].position)-vector(b.vertices[i].position)));v=std::max(v,length(vector(a.vertices[i].velocity)-vector(b.vertices[i].velocity)));}std::cerr<<asset->id<<" tick="<<tick<<" p="<<p<<" v="<<v<<'\n';
#endif
        compare(a,b,.003f,.5f);}}
    comparisonEvidence.push_back({{"asset",asset->id},{"vertices",asset->rest.size()},{"ticks",ticks},{"groupDispatches",grouped.stats().dispatches},{"referenceDispatches",dispatch.stats().dispatches},{"groupSharedBytes",grouped.stats().groupSharedBytes}});
    require(grouped.stats().grouped&&!dispatch.stats().grouped&&grouped.stats().dispatches==ticks*3ull+2,"Execution selection/dispatch accounting invalid");require(grouped.stats().allocationBytes==dispatch.stats().allocationBytes,"Grouped path hid buffer allocations");require(grouped.shutdown(error)&&dispatch.shutdown(error),error.c_str());++groups;
}
void boundaries(Rig& rig,std::shared_ptr<const c::Asset> limit,std::shared_ptr<const c::Asset> fallback){
    std::string error;c::GpuParameters forced;forced.execution=c::GpuExecution::Grouped;
    c::GpuSolver accepted(rig.device.p,rig.fence.p,limit);require(accepted.stats().grouped&&accepted.stats().groupSharedBytes==30720,"512 vertex group limit not selected");initialize(rig,accepted);
    c::GpuSolver large(rig.device.p,rig.fence.p,fallback);require(!large.stats().grouped&&large.stats().groupSharedBytes==0,"Large cloth silently truncated to group capacity");initialize(rig,large);
    rejects([&]{c::GpuSolver bad(rig.device.p,rig.fence.p,fallback,{},forced);},"Forced grouped execution accepted oversized topology");forced.execution=static_cast<c::GpuExecution>(99);rejects([&]{c::GpuSolver bad(rig.device.p,rig.fence.p,limit,{},forced);},"Unknown execution mode accepted");
    c::GpuLimits budget;budget.maxDispatchesPerStep=3;c::GpuSolver cheap(rig.device.p,rig.fence.p,limit,budget);initialize(rig,cheap);c::GpuParameters old;old.execution=c::GpuExecution::Dispatch;c::GpuSolver expensive(rig.device.p,rig.fence.p,limit,budget,old);initialize(rig,expensive);rig.begin();require(cheap.step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());const auto before=expensive.stats();require(!expensive.step(rig.commands.p,rig.ticket(),1.f/120,error)&&expensive.stats().dispatches==before.dispatches&&expensive.stats().tick==0,"Reference backend exceeded dispatch budget or mutated on rejection");require(large.step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());rig.submit();
    require(accepted.shutdown(error)&&large.shutdown(error)&&cheap.shutdown(error)&&expensive.shutdown(error),error.c_str());++groups;
}
double percentile(std::vector<double> values,double q){std::sort(values.begin(),values.end());return values[static_cast<std::size_t>(std::ceil(values.size()*q))-1];}
struct Timer {
    gpu_test::Com<ID3D12QueryHeap> queries;gpu_test::Com<ID3D12Resource> read;std::uint64_t frequency=0;
    explicit Timer(Rig& rig){D3D12_QUERY_HEAP_DESC d{};d.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;d.Count=2;gpu_test::hr(rig.device.p->CreateQueryHeap(&d,IID_PPV_ARGS(queries.out())),"Create cloth benchmark timestamps");gpu_test::makeBuffer(rig.device.p,read,16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);gpu_test::hr(rig.queue.p->GetTimestampFrequency(&frequency),"Get cloth benchmark frequency");require(frequency>0,"No GPU timestamp frequency");}
    std::pair<double,double> measure(Rig& rig,std::vector<std::unique_ptr<c::GpuSolver>>& solvers){
        const auto begin=std::chrono::steady_clock::now();rig.begin();rig.commands.p->EndQuery(queries.p,D3D12_QUERY_TYPE_TIMESTAMP,0);std::string error;
        for(unsigned tick=0;tick<2;++tick)for(auto& solver:solvers)require(solver->step(rig.commands.p,rig.ticket(),1.f/120,error),error.c_str());
        rig.commands.p->EndQuery(queries.p,D3D12_QUERY_TYPE_TIMESTAMP,1);rig.commands.p->ResolveQueryData(queries.p,D3D12_QUERY_TYPE_TIMESTAMP,0,2,read.p,0);const auto end=std::chrono::steady_clock::now();rig.submit();
        void* data=nullptr;D3D12_RANGE range{0,16};gpu_test::hr(read.p->Map(0,&range,&data),"Map completed cloth timestamps");std::uint64_t ticks[2];std::memcpy(ticks,data,16);D3D12_RANGE none{0,0};read.p->Unmap(0,&none);require(ticks[1]>ticks[0],"GPU benchmark timestamps are empty or out of order");return {double(ticks[1]-ticks[0])*1000/double(frequency),std::chrono::duration<double,std::milli>(end-begin).count()};
    }
};
void benchmark(Rig& rig,std::shared_ptr<const c::Asset> asset,unsigned instances){
    std::vector<std::unique_ptr<c::GpuSolver>> grouped,reference;for(unsigned i=0;i<instances;++i){c::GpuParameters p;p.wind={0,0,4};p.origin={float(i)*2,0,0};p.execution=c::GpuExecution::Grouped;grouped.push_back(std::make_unique<c::GpuSolver>(rig.device.p,rig.fence.p,asset,c::GpuLimits{},p));p.execution=c::GpuExecution::Dispatch;reference.push_back(std::make_unique<c::GpuSolver>(rig.device.p,rig.fence.p,asset,c::GpuLimits{},p));initialize(rig,*grouped.back());initialize(rig,*reference.back());}
    Timer timer(rig);std::vector<double> fastGpu,slowGpu,fastCpu,slowCpu;
    for(unsigned sample=0;sample<68;++sample){std::pair<double,double> fast,slow;if(sample%2){fast=timer.measure(rig,grouped);slow=timer.measure(rig,reference);}else{slow=timer.measure(rig,reference);fast=timer.measure(rig,grouped);}if(sample<8)continue;fastGpu.push_back(fast.first);slowGpu.push_back(slow.first);fastCpu.push_back(fast.second);slowCpu.push_back(slow.second);}
    const auto fast95=percentile(fastGpu,.95),slow95=percentile(slowGpu,.95);
    timingEvidence.push_back({{"asset",asset->id},{"instances",instances},{"vertices",asset->rest.size()*instances},{"fixedTicksPerSample",2},{"warmupSamples",8},{"measuredSamples",60},{"alternatingOrder",true},{"groupGpuP50Ms",percentile(fastGpu,.5)},{"groupGpuP95Ms",fast95},{"dispatchGpuP50Ms",percentile(slowGpu,.5)},{"dispatchGpuP95Ms",slow95},{"gpuP95Speedup",slow95/fast95},{"groupCpuRecordP95Ms",percentile(fastCpu,.95)},{"dispatchCpuRecordP95Ms",percentile(slowCpu,.95)}});
    timingEvidence.back()["timestampFrequencyHz"]=timer.frequency;
    timingEvidence.back()["samplesMs"]={{"groupGpu",fastGpu},{"dispatchGpu",slowGpu},{"groupCpuRecord",fastCpu},{"dispatchCpuRecord",slowCpu}};
#if defined(CLOTH_GROUP_TRACE)
    std::cerr<<"Cloth benchmark: "<<timingEvidence.back().dump()<<'\n';
#endif
    require(fast95<slow95*.75,"Grouped GPU P95 did not improve at least 25% on this fixture");
    std::string error;for(auto& solver:grouped)require(solver->shutdown(error),error.c_str());for(auto& solver:reference)require(solver->shutdown(error),error.c_str());++groups;
}
}
int main(int argc,char** argv){try{std::string adapter;if(argc==3&&std::string(argv[1])=="--adapter")adapter=argv[2];else if(argc!=1)throw std::runtime_error("Usage: native_cloth_grouped_tests [--adapter substring]");
    auto library=c::loadClothLibrary("build/cloth-grouped/cooked/library.json","assets"),fixtures=c::loadClothLibrary("build/cloth-grouped/fixtures/library.json","assets");Rig rig(adapter);equivalence(rig,library.cloths.at("flag_banner"),600);equivalence(rig,fixtures.cloths.at("group_limit"),600);boundaries(rig,fixtures.cloths.at("group_limit"),fixtures.cloths.at("dispatch_fallback"));benchmark(rig,library.cloths.at("flag_banner"),1);benchmark(rig,fixtures.cloths.at("group_limit"),4);const auto errors=rig.debugErrors();require(errors==0,"Grouped cloth D3D12 debug errors");
    std::cout<<nlohmann::json{{"schema","nativegpu.cloth-grouped-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"vertexComparisons",comparisons},{"maxPositionDifferenceMetres",maxPositionError},{"maxVelocityDifferenceMetresPerSecond",maxVelocityError},{"equivalence",comparisonEvidence},{"timings",timingEvidence},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"runtimeReadback",false},{"validationReadback","completed state and timestamp samples only"},{"scope","paired cloth-only workloads; not the combined A-G frame budget"}}.dump()<<'\n';
}catch(const std::exception& e){std::cerr<<"Grouped cloth failed after "<<checks<<" checks: "<<e.what()<<"; max position="<<maxPositionError<<" max velocity="<<maxVelocityError<<'\n';return 1;}}
