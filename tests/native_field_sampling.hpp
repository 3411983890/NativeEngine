// Included by native_fields.cpp; uses its assertion and lease helpers.
namespace {
void samplingHalosAndPolynomial() {
    World world;
    world.registerChannel({"linear","fixed","smoke",Domain::CpuAuthority,Merge::Sum,
        std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),1});
    auto producer=acquire(world,request({{-2,14,-18},{1,17,-15}}));
    std::vector<Write> writes;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        writes.push_back({{-1+x,15+y,-17+z},-500+256*x+512*y+1024*z});
    check(world.publish(producer,0,1,1,10,Domain::CpuAuthority,writes)==PublishResult::Accepted,"polynomial publish failed");
    world.commit(10);
    auto r=request({{-1,15,-17},{0,16,-16}},false);r.haloCells=1;
    const auto halo=world.acquire(r);check(halo.accepted&&halo.coveredPages==8,"three-axis halo page accounting failed");
    const auto linear=world.sampleLinear(halo.lease,0,{{-1,15,-17},{64,128,192}},10);
    check(linear.valid()&&linear.value==588&&linear.sourceTick==10,"cross-brick polynomial interpolation failed");
    const auto bySource=world.sampleSourceLinear(halo.lease,0,1,{{-1,15,-17},{64,128,192}},10);
    check(bySource.valid()&&bySource.value==588&&bySource.sourceEpoch==1,"source interpolation lost epoch or value");
    check(world.sample(halo.lease,0,{0,15,-17},10).status==Status::OutsideRegion,"halo granted query-centre access");
    check(world.sampleLinear(halo.lease,0,{{0,15,-17},{0,0,0}},10).status==Status::OutsideRegion,"linear centre escaped lease");
    auto noHalo=r;noHalo.haloCells=0;auto narrow=acquire(world,noHalo);
    check(world.sampleLinear(narrow,0,{{-1,15,-17},{1,0,0}},10).status==Status::OutsideRegion,"neighbouring lease leaked stencil permission");
    check(world.sampleLinear(narrow,0,{{-1,15,-17},{0,0,0}},10).value==-500,"zero-weight corners required halo");
    const auto before=world.stats().leases;
    auto tooSmall=r;tooSmall.maxPages=7;check(!world.acquire(tooSmall).accepted,"halo bypassed page cap");
    auto invalid=r;invalid.haloCells=2;check(!world.acquire(invalid).accepted,"unsupported halo accepted");
    invalid=r;invalid.writable=true;invalid.owner="smoke";check(!world.acquire(invalid).accepted,"writer halo granted");
    invalid=r;invalid.region.min.x=std::numeric_limits<int>::min();invalid.region.max.x=invalid.region.min.x+1;
    check(!world.acquire(invalid).accepted,"negative halo coordinate overflow accepted");
    invalid=r;invalid.region.min.x=std::numeric_limits<int>::max()-1;invalid.region.max.x=std::numeric_limits<int>::max();
    check(!world.acquire(invalid).accepted,"positive halo coordinate overflow accepted");
    check(world.stats().leases==before,"rejected halo leaked lease");
    check(world.sampleLinear(halo.lease,0,{{-1,15,-17},{256,0,0}},10).status==Status::InvalidPosition,"invalid Q8 fraction accepted");
    check(world.sampleSourceLinear(halo.lease,0,99,{{-1,15,-17},{1,1,1}},10).status==Status::Unavailable,"unknown source became zero");
    world.release(halo.lease);check(world.sampleLinear(halo.lease,0,{{-1,15,-17},{}},10).status==Status::InvalidLease,"retired interpolation lease remained valid");
    ++groups;
}
void samplingRoundingAndExtremes() {
    World world;
    world.registerChannel({"signed","fixed","smoke",Domain::CpuAuthority,Merge::Sum,
        std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),1});
    auto producer=acquire(world,request({{0,0,0},{16,1,1}}));
    std::vector<Write> writes={{{0,0,0},0},{{1,0,0},1},{{2,0,0},0},{{3,0,0},-1},
        {{4,0,0},std::numeric_limits<int>::min()},{{5,0,0},std::numeric_limits<int>::max()},
        {{6,0,0},std::numeric_limits<int>::min()},{{7,0,0},std::numeric_limits<int>::min()},
        {{8,0,0},std::numeric_limits<int>::max()},{{9,0,0},std::numeric_limits<int>::max()}};
    check(world.publish(producer,0,1,1,1,Domain::CpuAuthority,writes)==PublishResult::Accepted,"extreme publish failed");world.commit(1);
    check(world.sampleLinear(producer,0,{{0,0,0},{128,0,0}},1).value==1,"positive tie did not round away from zero");
    check(world.sampleLinear(producer,0,{{2,0,0},{128,0,0}},1).value==-1,"negative tie did not round away from zero");
    check(world.sampleLinear(producer,0,{{4,0,0},{128,0,0}},1).value==-1,"full int32 cancellation overflowed");
    check(world.sampleLinear(producer,0,{{6,0,0},{255,0,0}},1).value==std::numeric_limits<int>::min(),"INT_MIN interpolation overflowed");
    check(world.sampleLinear(producer,0,{{8,0,0},{255,0,0}},1).value==std::numeric_limits<int>::max(),"INT_MAX interpolation overflowed");
    ++groups;
}
void samplingUnknownAndFreshness() {
    World world;world.registerChannel(density());auto producer=acquire(world,request({{0,0,0},{2,2,2}}));
    std::vector<Write> values;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        if(x||y||z)values.push_back({{x,y,z},100});
    world.publish(producer,0,1,1,5,Domain::CpuAuthority,values);
    world.publishAt({producer,0,2,1,1,5,Domain::CpuAuthority,{{{0,0,0},100}}});world.commit(5);
    auto eventual=acquire(world,request({{0,0,0},{2,2,2}},false));
    auto strong=acquire(world,request({{0,0,0},{2,2,2}},false,Consistency::Strong));
    auto stale=acquire(world,request({{0,0,0},{2,2,2}},false,Consistency::Stale));
    auto p=SamplePosition{{0,0,0},{128,128,128}};
    auto a=world.sampleLinear(eventual,0,p,5);check(a.status==Status::Stale&&a.value==100&&a.sourceTick==1&&a.ageTicks==4,"oldest weighted corner freshness lost");
    check(world.sampleLinear(strong,0,p,5).status==Status::Stale,"Strong interpolation accepted stale corner");
    check(world.sampleLinear(strong,0,p,6).status==Status::TickMismatch,"Strong interpolation accepted another snapshot");
    check(world.sampleLinear(stale,0,p,5).valid(),"Stale query incorrectly enforced maxAge");
    check(world.sampleLinear(eventual,0,{{1,1,1},{}},5).sourceTick==5,"zero-weight old corner changed age");
    world.publish(producer,0,2,2,6,Domain::CpuAuthority,{});world.commit(6);
    check(world.sampleLinear(stale,0,p,6).status==Status::Unavailable,"missing weighted corner became zero");
    check(world.sampleLinear(stale,0,{{1,1,1},{}},6).valid(),"zero-weight missing corner made sample unknown");
    world.publish(producer,0,3,1,7,Domain::CpuAuthority,{{{0,0,0},1000},{{1,0,0},1000}});
    world.publish(producer,0,4,1,7,Domain::CpuAuthority,{{{0,0,0},1000}});world.commit(7);
    auto saturated=world.sampleLinear(stale,0,{{0,0,0},{128,0,0}},7);
    check(saturated.valid()&&saturated.value==1000&&saturated.clamped,"weighted corner clamp flag lost");
    ++groups;
}
void samplingTests() {samplingHalosAndPolynomial();samplingRoundingAndExtremes();samplingUnknownAndFreshness();}
}
