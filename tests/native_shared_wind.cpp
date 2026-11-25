#include "../host/simulation_fields.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"
#include <algorithm>

namespace f=engine::fields;
namespace {
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F action,const char* why){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,why);}
struct Backend {
    gpu_test::Rig& rig;std::shared_ptr<f::GpuFieldStorage> storage;std::unique_ptr<f::GpuVectorFields> vectors;
    std::vector<f::GpuLease> leases;unsigned acquisitions=0,publications=0;bool failPublish=false;
    Backend(gpu_test::Rig& r,std::shared_ptr<f::World> world):rig(r),storage(std::make_shared<f::GpuFieldStorage>(world,r.device.p)),vectors(std::make_unique<f::GpuVectorFields>(storage,f::GpuVectorLimits{8,8*65536ull})){}
    nativegpu::GpuFieldFrame frame(std::uint64_t epoch){
        nativegpu::GpuFieldFrame out;out.epoch=epoch;
        out.acquire=[this](f::LeaseRequest request){++acquisitions;auto result=storage->acquire(std::move(request));if(result.accepted)leases.push_back(result.lease);return result;};
        out.release=[this](f::GpuLease lease){if(!storage->release(lease,rig.ticket()))return false;leases.erase(std::remove_if(leases.begin(),leases.end(),[&](auto old){return old.slot==lease.slot&&old.generation==lease.generation;}),leases.end());return true;};
        out.publish=[this](f::GpuLease lease,const f::GpuVectorSpec& spec,f::Tick tick){++publications;if(failPublish){failPublish=false;return f::GpuVectorResult{{},"test publish admission failure"};}std::string error;if(!storage->publish(rig.commands.p,rig.ticket(),error))return f::GpuVectorResult{{},error};return vectors->bind(lease,spec,tick);};return out;
    }
    void close(){std::string error;require(vectors->shutdown(rig.submitted,error),error.c_str());for(auto lease:leases)require(storage->release(lease,rig.submitted),"Shared reader release failed");storage->collect(rig.submitted);require(storage->shutdown(rig.submitted,error),error.c_str());}
};
}
int main(int argc,char** argv){try{
    std::string adapter;if(argc==3&&std::string(argv[1])=="--adapter")adapter=argv[2];else if(argc!=1)throw std::runtime_error("Usage: native_shared_wind_tests [--adapter substring]");
    gpu_test::Rig rig(adapter);host::SimulationFields source("build/simulation-fields/fixed.field.json");const auto world=source.world();require(world->stats().activePages==0&&world->stats().leases==0,"Source allocated pages before a subscriber");
    rejects([]{host::SimulationFields bad("build/simulation-fields/authority.field.json");},"Visual bridge accepted CPU authority track");rejects([]{host::SimulationFields bad("build/simulation-fields/flow.field.json");},"Wind consumer accepted flow-only track");
    std::string error;Backend first(rig,world);auto a=first.frame(1);rig.begin();require(source.drive(a,0,error)&&a.wind&&a.wind->requestedTick()==0,error.c_str());require(a.wind->markUsed(rig.ticket()),"Cannot register first use");rig.submit();const auto version=world->version();
    auto b=first.frame(1);rig.begin();require(source.drive(b,0,error)&&b.wind==a.wind,error.c_str());require(b.wind->markUsed(rig.ticket()),"Cannot register shared consumer use");rig.submit();require(first.acquisitions==1&&first.publications==1&&world->version()==version,"Same-tick consumers duplicated lease/publication");
    auto incomplete=first.frame(1);incomplete.publish={};rig.begin();require(!source.drive(incomplete,1,error)&&world->version()==version,"Invalid callback frame changed source");rig.submit();
    auto next=first.frame(1);rig.begin();require(source.drive(next,10,error)&&next.wind->requestedTick()==10,error.c_str());require(next.wind->markUsed(rig.ticket()),"Cannot register next field use");rig.submit();
    const auto nextVersion=world->version();auto backwards=first.frame(2);rig.begin();require(!source.drive(backwards,9,error)&&first.acquisitions==1&&first.publications==2&&world->version()==nextVersion&&source.report().at("epochs")==1,"Backwards time partially changed epoch/source");rig.submit();
    a.wind.reset();b.wind.reset();next.wind.reset();first.close();require(world->stats().leases==1,"GPU reader survived shutdown or CPU producer was removed");
    Backend recovered(rig,world);auto fresh=recovered.frame(2);rig.begin();require(source.drive(fresh,10,error)&&fresh.wind&&fresh.wind->requestedTick()==10,error.c_str());require(fresh.wind->markUsed(rig.ticket()),"Cannot register recovered field use");rig.submit();
    auto state=source.report();require(state.at("epochs")==2&&state.at("sourcePublications")==2&&state.at("bindings")==3&&world->version()==nextVersion,"Recovery recommitted source or lost explicit field clock");
    recovered.failPublish=true;auto failed=recovered.frame(2);rig.begin();require(!source.drive(failed,11,error)&&source.report().at("tick")==10&&world->committedTick()==11,"Failed GPU admission not reported explicitly");rig.submit();const auto failedVersion=world->version();
    auto retried=recovered.frame(2);rig.begin();require(source.drive(retried,11,error)&&retried.wind->requestedTick()==11,error.c_str());require(retried.wind->markUsed(rig.ticket()),"Cannot register retried use");rig.submit();require(world->version()==failedVersion&&source.report().at("sourcePublications")==3&&recovered.acquisitions==1,"Retry duplicated source contribution or read lease");
    fresh.wind.reset();retried.wind.reset();auto released=recovered.frame(2);rig.begin();require(source.drive(released,11,error,false)&&!released.wind&&!source.binding(),error.c_str());rig.submit();require(world->stats().leases==0&&world->stats().sourceRecords==0&&!source.report().at("leaseActive").get<bool>(),"Last consumer left source/reader allocations active");
    const auto acquisitionsBeforeRetry=recovered.acquisitions;auto sameTick=recovered.frame(2);rig.begin();require(!source.drive(sameTick,11,error)&&recovered.acquisitions==acquisitionsBeforeRetry&&world->stats().leases==0,"Reactivation amended a committed tick or partially acquired a lease");rig.submit();
    const auto publicationsBeforeIdle=recovered.publications;auto idle=recovered.frame(2);rig.begin();require(source.drive(idle,13,error,false),error.c_str());rig.submit();require(recovered.publications==publicationsBeforeIdle,"Unsubscribed field still publishes GPU snapshots");
    auto resumed=recovered.frame(2);rig.begin();require(source.drive(resumed,14,error)&&resumed.wind,error.c_str());require(resumed.wind->markUsed(rig.ticket()),"Cannot register resumed subscriber");rig.submit();require(world->stats().leases==2&&world->stats().sourceRecords==3,"Reactivation failed to restore exactly one producer/reader");
    const auto report=source.report();resumed.wind.reset();recovered.close();const auto errors=rig.debugErrors();require(errors==0,"Shared field D3D12 debug errors");
    std::cout<<nlohmann::json{{"schema","nativegpu.shared-wind-source/1"},{"ok",true},{"checks",checks},{"source",report},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"runtimeReadback",false}}.dump()<<'\n';
}catch(const std::exception& e){std::cerr<<"Shared wind source failed after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
