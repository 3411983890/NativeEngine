#include "../engine/native/simulation/field_adapters.hpp"
#include "../engine/native/cloth/solver_cpu.hpp"
#include "../engine/native/water/body.hpp"
#include "../third_party/json.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace engine;
using namespace engine::fields;
using namespace engine::simulation;
namespace {
unsigned checks=0,groups=0;
std::string clothLibrary="build/cloth/library.json";
void check(bool condition,const char* why){++checks;if(!condition)throw std::runtime_error(why);}
template<class F>void throws(F action,const char* why){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,why);}
bool near(float a,float b){return std::fabs(a-b)<1e-5f;}
Lease acquire(World& world,Region region,std::vector<ChannelId> channels,bool writable=false,Consistency consistency=Consistency::Eventual) {
    LeaseRequest r;r.region=region;r.channels=std::move(channels);r.owner=writable?"simulation":"consumer";r.writable=writable;r.consistency=consistency;r.maxPages=128;
    auto lease=world.acquire(r);check(lease.accepted,"adapter lease acquisition failed");return lease.lease;
}
void windChannels(World& world,Domain domain) {
    for(auto name:{"windX","windY","windZ"})world.registerChannel({name,"m/s","simulation",domain,Merge::Sum,-32000,32000,1000});
}
void publishWind(World& world,Lease lease,Tick tick,Domain domain,bool gradient=false) {
    std::vector<Publication> batch;
    for(unsigned c=0;c<3;++c) {
        Publication p{lease,c,1,tick,tick,tick,domain,{}};
        for(int z=-2;z<=2;++z)for(int y=-1;y<=3;++y)for(int x=-2;x<=2;++x)
            p.writes.push_back({{x,y,z},c==2?6000+(gradient?1000*x:0):0});
        batch.push_back(std::move(p));
    }
    check(world.publishAtomic(batch)==PublishResult::Accepted&&world.commit(tick),"wind publication failed");
}
void gridMapping() {
    FieldGrid grid{{10,-20,30},0.5};SamplePosition p;
    check(grid.locate({9.875f,-19.75f,30},p)&&p.cell==Cell{-1,0,0}&&p.fraction==std::array<std::uint32_t,3>{192,128,0},"negative metres mapping used truncation or wrong axis");
    Vec3 metres;check(grid.point({-2,3,4},metres)&&near(metres.x,9)&&near(metres.y,-18.5f)&&near(metres.z,32),"lattice point mapping failed");
    const auto unchanged=p;
    check(!grid.locate({std::numeric_limits<float>::quiet_NaN(),0,0},p)&&p.cell==unchanged.cell,"invalid mapping mutated output");
    check(!grid.locate({std::numeric_limits<float>::max(),0,0},p),"mapping accepted int32 coordinate overflow");
    grid.cellMetres=0;check(!grid.valid()&&!grid.locate({},p),"zero grid spacing accepted");
    grid.cellMetres=std::numeric_limits<double>::infinity();check(!grid.valid(),"non-finite grid accepted");
    grid.cellMetres=1e-300;check(!grid.locate({1,0,0},p),"tiny grid accepted quotient overflow");
    grid.cellMetres=1e300;check(!grid.point({2,0,0},metres),"lattice conversion overflowed float output");++groups;
}
void windContracts() {
    World world;windChannels(world,Domain::CpuAuthority);
    const Region region{{-2,-1,-2},{3,4,3}};auto writer=acquire(world,region,{0,1,2},true);
    auto reader=acquire(world,region,{0,1,2},false,Consistency::Stale);
    publishWind(world,writer,1,Domain::CpuAuthority,true);
    WindField wind(world,reader,{0,1,2},{},Domain::CpuAuthority);
    auto sample=wind.sample({-0.5f,0,0},1);
    check(sample.valid()&&near(sample.metresPerSecond.z,5.5f)&&sample.sourceTick==1&&sample.version==world.version(),"wind interpolation, units or version failed");
    check(sample.positionQuantizationMetres==1.0/256,"position quantization not reported");
    check(wind.sample({},2).status==Status::TickMismatch,"authority wind used an older committed snapshot");
    world.commit(2);check(wind.sample({},2).status==Status::Stale,"Stale lease bypassed authority freshness");
    check(wind.sample({std::numeric_limits<float>::quiet_NaN(),0,0},2).status==Status::InvalidPosition,"NaN wind position accepted");
    check(wind.sample({9,0,0},2).status==Status::OutsideRegion,"wind returned implicit zero outside coverage");
    throws([&]{WindField invalid(world,reader,{0,0,2},{},Domain::CpuAuthority);},"wind accepted repeated scalar component");
    throws([&]{WindField invalid(world,reader,{0,1,2},{},Domain::VisualReference);},"wind silently crossed authority domain");
    world.registerChannel({"temperature","K","simulation",Domain::CpuAuthority,Merge::Maximum,0,1000,1});
    throws([&]{WindField invalid(world,reader,{0,1,3},{},Domain::CpuAuthority);},"wind accepted temperature as velocity");
    world.release(reader);check(wind.sample({},2).status==Status::InvalidLease,"wind retained a released capability");++groups;
}
void clothCoupling() {
    World world;windChannels(world,Domain::VisualReference);
    const Region region{{-2,-1,-2},{3,4,3}};auto writer=acquire(world,region,{0,1,2},true);
    auto reader=acquire(world,region,{0,1,2});publishWind(world,writer,1,Domain::VisualReference);
    WindField wind(world,reader,{0,1,2},{},Domain::VisualReference);
    const auto library=cloth::loadClothLibrary(clothLibrary,".");
    cloth::Solver spatial(library.cloths.at("flag_banner")),constant(library.cloths.at("flag_banner"));
    constant.setWind({0,0,6});
    for(unsigned i=0;i<120;++i) {
        auto applied=applyWindField(spatial,wind,1);check(applied.applied&&applied.sampledVertices==spatial.vertexCount()&&applied.unavailableVertices==0,"cloth spatial wind application failed");
        spatial.step(1.0f/120);constant.step(1.0f/120);
    }
    for(std::size_t i=0;i<spatial.vertexCount();++i) {
        check(near(spatial.position(i).x,constant.position(i).x)&&near(spatial.position(i).y,constant.position(i).y)&&near(spatial.position(i).z,constant.position(i).z),"uniform field and constant wind produced different cloth motion");
    }
    const auto before=spatial.digest();auto failed=applyWindField(spatial,wind,5);
    check(!failed.applied&&failed.unavailableVertices==spatial.vertexCount()&&failed.firstFailure==Status::Stale&&spatial.digest()==before,"stale cloth wind partially replaced prior input");
    throws([&]{spatial.setWindSamples({{0,0,0}});},"cloth accepted incomplete spatial wind");
    std::vector<Vec3> invalid(spatial.vertexCount());invalid.back().z=std::numeric_limits<float>::quiet_NaN();
    throws([&]{spatial.setWindSamples(invalid);},"cloth accepted non-finite spatial wind");check(spatial.digest()==before,"rejected wind input changed cloth state");
    // A partially covered cloth must also keep its entire previous input.
    auto narrow=acquire(world,{{-1,0,0},{0,2,1}},{0,1,2});WindField incomplete(world,narrow,{0,1,2},{},Domain::VisualReference);
    failed=applyWindField(spatial,incomplete,1);check(!failed.applied&&failed.unavailableVertices>0&&spatial.digest()==before,"partial coverage damaged existing cloth input");
    auto resetCopy=spatial;resetCopy.setWind({});resetCopy.setOrigin({0,0,0});spatial.setOrigin({0,0,0});
    check(spatial.digest()==resetCopy.digest(),"teleport retained old spatial wind samples");++groups;
}
std::shared_ptr<water::Asset> pool() {
    auto a=std::make_shared<water::Asset>();a->id="coupling_pool";a->hash="coupling_pool";a->backend=water::Backend::Shallow;
    a->columns=a->rows=8;a->extentX=a->extentZ=4;a->cellMetres=0.5f;a->stillHeightMetres=1;
    a->bathymetry={water::BathymetryType::Flat,0,0,0,0};a->maxCells=64;a->maxSubsteps=8;return a;
}
void waterChannels(World& world,bool lastVisual=false) {
    for(unsigned i=0;i<4;++i)world.registerChannel({"water"+std::to_string(i),i<2?"m":"m/s","simulation",lastVisual&&i==3?Domain::VisualReference:Domain::CpuAuthority,Merge::Sum,i==1?0:-100000,100000,1000});
}
void waterProjection() {
    World world;waterChannels(world);water::Body body(pool());
    const Region region{{-3,0,-3},{4,1,4}};auto writer=acquire(world,region,{0,1,2,3},true);
    auto reader=acquire(world,region,{0,1,2,3},false,Consistency::Strong);
    WaterProjection spec{writer,{0,1,2,3},region,{},42,1,1,1,65536};
    const auto sourceDigest=body.digest();auto result=publishWater(world,body,spec);
    check(result.accepted()&&result.knownSamples>0&&result.unknownSamples>0&&result.bodyTick==0,"water did not distinguish known and unknown coverage");
    check(result.valueQuantization[0]==0.0005&&body.digest()==sourceDigest,"water projection changed solver or quantization bound");
    check(world.version()==0&&world.sample(reader,0,{0,0,0},1).status==Status::TickMismatch,"water adapter implicitly committed World");
    check(world.commit(1),"water projection commit failed");
    check(world.sampleSource(reader,0,42,{0,0,0},1).value==1000&&world.sampleSource(reader,1,42,{0,0,0},1).value==1000,"water height/depth projection incorrect");
    check(world.sampleSource(reader,2,42,{0,0,0},1).value==0&&world.sampleSource(reader,3,42,{0,0,0},1).valid(),"zero water velocity became unknown");
    check(world.sampleSourceLinear(reader,0,42,{{0,0,0},{128,0,128}},1).value==1000,"water layer bilinear sampling failed");
    check(world.sampleSource(reader,0,42,{3,0,3},1).status==Status::Unavailable,"outside-body projection invented water");
    // A second body's identity is selected through its source, never interpolated
    // as a scalar waterBodyId and never mixed with the first body's proxy.
    water::Body raised(pool());raised.setLevel(2);auto other=spec;other.source=43;other.sourceTick=other.publishTick=2;
    check(publishWater(world,raised,other).accepted()&&world.commit(2),"overlapping water source failed");
    check(world.sampleSource(reader,0,42,{0,0,0},2).value==1000&&world.sampleSource(reader,0,43,{0,0,0},2).value==2000,"overlapping water bodies lost source identity");
    body.setLevel(1.5f);spec.epoch=2;spec.sourceTick=spec.publishTick=3;
    auto rejected=spec;rejected.writer=acquire(world,region,{0,1,2},true);
    const auto before=world.stats().publications;
    check(!publishWater(world,body,rejected).accepted()&&world.stats().publications==before,"failed fourth channel partially staged water");
    check(world.commit(3)&&world.sampleSource(reader,0,42,{0,0,0},3).value==1000,"rejected water batch changed prior source");
    spec.sourceTick=spec.publishTick=4;check(publishWater(world,body,spec).accepted()&&world.commit(4),"water projection did not recover after rejection");
    check(world.sampleSource(reader,0,42,{0,0,0},4).value==1500,"water source epoch replacement doubled old height");
    auto small=spec;small.epoch=3;small.sourceTick=small.publishTick=5;small.maxSamples=1;
    check(!publishWater(world,body,small).accepted(),"water projection exceeded sample budget");
    auto layered=small;layered.region.max.y=2;check(!publishWater(world,body,layered).accepted(),"water projection accepted ambiguous vertical volume");
    // Out-of-range values are rejected before any batch is submitted.
    body.setLevel(200);small.maxSamples=65536;
    const auto previous=world.stats().publications;
    check(!publishWater(world,body,small).accepted()&&world.stats().publications==previous,"water projection silently saturated authority height");
    World invalid;waterChannels(invalid,true);auto denied=spec;denied.writer={};
    check(!publishWater(invalid,body,denied).accepted()&&invalid.stats().publications==0,"visual channel accepted authority water publication");++groups;
}
void coupledReplay() {
    std::uint64_t reference=0;
    for(unsigned run=0;run<32;++run) {
        World world;waterChannels(world);auto a=pool();water::Body body(a);
        const Region region{{-2,0,-2},{3,1,3}};auto writer=acquire(world,region,{0,1,2,3},true);
        WaterProjection spec{writer,{0,1,2,3},region,{},1,0,0,0,65536};
        for(unsigned tick=1;tick<=12;++tick) {
            if(tick==3)body.setDepth(4,4,2);body.step(1.0f/120);
            spec.epoch=spec.sourceTick=spec.publishTick=tick;
            check(publishWater(world,body,spec).accepted()&&world.commit(tick),"coupled authority replay publication failed");
        }
        if(run==0)reference=world.digest();check(world.digest()==reference,"water field authority replay changed digest");
    }
    ++groups;
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--cloth-library")clothLibrary=argv[2];
        else if(argc!=1)throw std::runtime_error("Usage: native_field_adapters_tests [--cloth-library path]");
        gridMapping();windContracts();clothCoupling();waterProjection();coupledReplay();
        std::cout<<nlohmann::json({{"schema","nativegpu.field-adapters-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"waterReplayRuns",32},{"clothSpatialWind",true},{"waterAuthorityProjection",true},{"gpuReadback",false},{"hostIntegrated",false}}).dump()<<'\n';
    }catch(const std::exception& error){std::cerr<<"Field adapters failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
