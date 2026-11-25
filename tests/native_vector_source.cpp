#include "../engine/native/fields/vector_source.hpp"
#include "../engine/native/simulation/clock.hpp"
#include "../third_party/json.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace f=engine::fields;
using Json=nlohmann::json;
namespace {
unsigned checks=0,groups=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F fn,const char* message){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}require(rejected,message);}
Json fixture(){return {{"schema","nativegpu.vector-field/1"},{"id","track"},{"domain","cpu-authority"},{"originMetres",{1,-2,3}},{"cellMetres",.5},{"region",{{"min",{-1,-1,-1}},{"max",{2,2,2}}}},{"haloCells",1},{"maxAgeTicks",4},{"loopTicks",240},
    {"tracks",{{"windVelocity",Json::array({Json{{"tick",0},{"metresPerSecond",{4,1,-2}}},Json{{"tick",120},{"metresPerSecond",{-3,2,2}}},Json{{"tick",240},{"metresPerSecond",{4,1,-2}}}})}}}};}
f::VectorSourceAsset asset(){return f::parseVectorSource(fixture().dump());}
void contract(){auto a=asset();require(a.hash.size()==64&&a.domain==f::Domain::CpuAuthority&&a.tracks.size()==1&&a.originMetres.x==1&&a.cellMetres==.5,"asset contract not retained");
    for(unsigned mode=0;mode<12;++mode){auto j=fixture();switch(mode){case 0:j["unknown"]=1;break;case 1:j["schema"]="bad";break;case 2:j["region"]["min"][0]=.5;break;case 3:j["cellMetres"]=0;break;case 4:j["region"]["max"]={100,100,100};break;case 5:j["tracks"]["windVelocity"][1]["tick"]=0;break;case 6:j["tracks"]["windVelocity"][2]["metresPerSecond"]={1,1,1};break;case 7:j["tracks"]["density"]=j["tracks"]["windVelocity"];break;case 8:j["maxAgeTicks"]=-1;break;case 9:j["domain"]="gpu-authority";break;case 10:j["tracks"]["windVelocity"][1]["metresPerSecond"][0]=10001;break;case 11:j["tracks"]["windVelocity"][0]["tick"]=1;break;}rejects([&]{f::parseVectorSource(j.dump());},"invalid vector source accepted");}
    rejects([&]{f::parseVectorSource(std::string(2*1024*1024+1,' '));},"oversized source accepted");++groups;
}
void interpolation(){auto world=std::make_shared<f::World>(128);f::VectorSource source(world,asset());require(source.evaluate("windVelocity",0)==std::array<std::int32_t,3>{4000,1000,-2000},"first key changed");require(source.evaluate("windVelocity",60)==std::array<std::int32_t,3>{500,1500,0},"rational midpoint differs");require(source.evaluate("windVelocity",120)==std::array<std::int32_t,3>{-3000,2000,2000},"exact key not selected");require(source.evaluate("windVelocity",(1ull<<40)*240+60)==source.evaluate("windVelocity",60),"64-bit loop mapping lost precision");
    for(unsigned tick=0;tick<480;++tick){const unsigned t=tick%240;const double fraction=t<=120?t/120.:2-t/120.;auto value=source.evaluate("windVelocity",tick);const int start[]={4000,1000,-2000},delta[]={-7000,1000,4000};for(unsigned axis=0;axis<3;++axis)require(value[axis]==std::llround(start[axis]+delta[axis]*fraction),"integer interpolation differs from independent keyframe reference");}
    auto a=asset();a.loopTicks=0;a.tracks[0].keys={f::VectorKey{0,{0,0,0}},f::VectorKey{2,{1,-1,-3}}};f::VectorSource half(std::make_shared<f::World>(128),a);require(half.evaluate("windVelocity",1)==std::array<std::int32_t,3>{1,-1,-2},"signed half rounding changed");require(half.evaluate("windVelocity",UINT64_MAX)==std::array<std::int32_t,3>{1,-1,-3},"non-looping track did not hold its final key");++groups;
}
void publicationAndLifetime(){auto world=std::make_shared<f::World>(128);{
    auto a=asset();a.tracks.push_back({"waterVelocity",{{0,{2000,0,-1000}}}});f::VectorSource source(world,a);auto acquired=world->acquire(source.reader());require(acquired.accepted,acquired.error.c_str());const auto channels=source.channels();std::string error;
    require(source.stage(0,error),error.c_str());require(!world->sample(acquired.lease,channels.at("windVelocity")[0],{0,0,0},0).valid(),"staging leaked before commit");require(world->commit(0),"first source commit failed");
    for(const auto& item:channels)for(unsigned axis=0;axis<3;++axis){const auto& channel=world->channel(item.second[axis]);require(channel.unit=="m/s"&&channel.unitsPerInteger==1000&&channel.domain==f::Domain::CpuAuthority,"scalar registration lost units or domain");}
    require(source.stage(60,error)&&world->commit(60),"source keyframe update failed");require(source.publications()==2,"source epoch count changed");auto v=world->sample(acquired.lease,channels.at("windVelocity")[0],{0,0,0},60);require(v.valid()&&v.value==500&&v.sourceTick==60,"new source value/tick not committed");auto q=f::SamplePosition{{1,1,1},{255,255,255}};auto water=world->sampleLinear(acquired.lease,channels.at("waterVelocity")[2],q,60);require(water.valid()&&water.value==-1000,"source omitted a halo component or corner");
    const auto digest=world->digest();require(source.stage(60,error)&&source.publications()==2&&world->digest()==digest,"same source tick was republished");require(!source.stage(59,error)&&world->digest()==digest,"source clock moved backwards");require(world->sample(acquired.lease,channels.at("windVelocity")[0],{0,0,0},65).status==f::Status::Stale,"producer hid source age");require(world->release(acquired.lease),"reader release failed");}
    world->collect(0);require(world->stats().activePages==0&&world->stats().retiredPages==0&&world->stats().leases==0&&world->stats().sourceRecords==0,"producer teardown retained leases/pages/source records");++groups;
}
std::uint64_t run(unsigned fps){auto world=std::make_shared<f::World>(128);f::VectorSource source(world,asset());engine::simulation::Clock clock;std::string error;std::uint64_t previous=0;for(unsigned frame=1;frame<=fps;++frame){auto ns=frame*1000000000ull/fps;clock.addNanoseconds(ns-previous);previous=ns;while(clock.consumeTick()){require(source.stage(clock.tick(),error)&&world->commit(clock.tick()),"clocked source commit failed");}}require(clock.tick()==120&&source.publications()==120,"render grouping changed logical field ticks");return world->digest();}
void replay(){const auto reference=run(60);require(run(30)==reference&&run(144)==reference,"render frame rate changed CPU field digest");for(unsigned i=0;i<8;++i)require(run(60)==reference,"source replay diverged");++groups;}
void authored(){auto a=f::loadVectorSource("assets/fields/gust_wind.field.json");f::VectorSource source(std::make_shared<f::World>(256),a);require(a.domain==f::Domain::VisualReference&&a.region.min.x==1&&a.haloCells==1,"authored host wind contract changed");require(source.evaluate("windVelocity",60)==std::array<std::int32_t,3>{500,1500,0},"authored gust midpoint changed");++groups;}
void demandAndBudget(){
    auto world=std::make_shared<f::World>(128);f::VectorSource source(world,asset());require(world->stats().committedBytes==0&&world->stats().leases==0,"dormant source allocated field pages");std::string error;require(source.stage(1,error)&&world->commit(1),"source activation failed");require(world->stats().committedBytes>0,"active source omitted its CPU backing pages");require(source.release(),"source release failed");world->collect(0);require(world->stats().committedBytes==0&&world->stats().sourceRecords==0&&world->stats().leases==0,"source retained field pages without consumers");require(source.stage(2,error)&&world->commit(2),"source did not reacquire its region after reactivation");require(source.publications()==2,"source epoch restarted after deactivation");
    auto constrained=std::make_shared<f::World>(128,1);f::VectorSource limited(constrained,asset());require(!limited.stage(1,error)&&limited.publications()==0&&constrained->stats().sourceRecords==0&&constrained->stats().sourceTiles==0&&constrained->stats().committedBytes==0&&constrained->stats().leases==0,"failed multi-brick publication left partial data or a writer lease");++groups;
}
}
int main(){try{contract();interpolation();publicationAndLifetime();replay();authored();demandAndBudget();std::cout<<Json{{"schema","nativegpu.vector-source-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"replayRepeats",8},{"renderRates",{30,60,144}},{"fieldReadback",false}}.dump()<<'\n';}catch(const std::exception& e){std::cerr<<"Vector source failed after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
