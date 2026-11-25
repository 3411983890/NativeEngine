#include "../../host/pond_demo.hpp"
#include "../../host/wetness.hpp"
#include "../../engine/native/water/transfer.hpp"
#include "../../engine/native/simulation/clock.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
unsigned checks=0;
void require(bool pass,const char* label){++checks;if(!pass)throw std::runtime_error(label);}
std::shared_ptr<const engine::water::Asset> pool(){
    auto a=std::make_shared<engine::water::Asset>();a->id="water-interaction-test";a->backend=engine::water::Backend::Shallow;
    a->extentX=a->extentZ=8;a->cellMetres=.125f;a->columns=a->rows=64;a->stillHeightMetres=.4f;a->maxSubsteps=8;a->maxCells=4096;return a;
}
double momentum(const engine::water::Body& body){
    double result=0;for(unsigned z=0;z<64;++z)for(unsigned x=0;x<64;++x){auto q=body.query(-4+(x+.5f)*.125f,-4+(z+.5f)*.125f);result+=q.depthMetres*q.velocityZ*.125*.125*1000;}return result;
}
}
int main(){try{
    engine::water::Body body(pool());const auto initial=body.diagnostics();
    engine::water::Body raised(pool(),{2,3,4});
    require(std::fabs(raised.query(2,4).surfaceHeightMetres-3.4f)<.00001f,"translated water level uses origin Y");
    require(std::fabs(raised.bedAt(2,4)-3)<.00001f,"translated bottom uses origin Y");
    require(raised.interact(2,4,.65f,-.05f,0,1),"translated patch interaction accepted");
    require(body.interact(0,0,.65f,-.08f,0,4),"wet patch accepts disturbance");
    require(body.diagnostics().initialVolumeCubicMetres==initial.initialVolumeCubicMetres,"interaction must not rewrite conservation baseline");
    require(std::fabs(body.diagnostics().volumeCubicMetres-initial.volumeCubicMetres)<.0001f,"interaction conserves volume");
    require(std::fabs(momentum(body)-4)<.0001,"horizontal impulse is integrated in N s");
    require(body.interact(0,0,.65f,-.04f,0,6),"second interaction accepted");
    require(std::fabs(momentum(body)-10)<.0001,"second impulse preserves prior momentum");
    require(body.query(0,0).surfaceHeightMetres<.4f,"entry depresses the contact patch");
    require(body.interact(3.98f,0,.65f,-.06f,0,0),"edge patch accepted");
    require(std::fabs(body.diagnostics().volumeCubicMetres-initial.volumeCubicMetres)<.0002f,"truncated boundary kernel conserves volume");
    require(!body.interact(9,0,.65f,-.04f,0,0),"outside interaction rejected");
    bool rejected=false;try{body.interact(0,0,std::numeric_limits<float>::quiet_NaN(),0,0,0);}catch(const engine::water::Error&){rejected=true;}require(rejected,"nonfinite interaction rejected");
    // Contact speed is recorded, and it also shapes the footprint: a faster contact is
    // stretched along its impulse instead of stamping the same circle every step. Volume
    // must be unaffected, because the kernel stays zero-net-volume whatever its aspect.
    {
        engine::water::Body slow(pool()),fast(pool());
        require(slow.interact(0,0,.65f,-.08f,0,4,0.0f),"zero impact speed interaction accepted");
        require(fast.interact(0,0,.65f,-.08f,0,4,3.25f),"known impact speed interaction accepted");
        require(slow.events().back().impactSpeed==0.0f,"default interaction reports the speed as unknown");
        require(std::fabs(fast.events().back().impactSpeed-3.25f)<1e-6f,"interaction records the contact speed");
        require(std::fabs(slow.diagnostics().volumeCubicMetres-fast.diagnostics().volumeCubicMetres)<1e-4f,
                "contact speed changes the shape, not the volume");
        bool stretched=false;
        for(unsigned z=0;z<64&&!stretched;++z)for(unsigned x=0;x<64;++x){
            const float px=-4+(x+.5f)*.125f,pz=-4+(z+.5f)*.125f;
            if(std::fabs(slow.query(px,pz).surfaceHeightMetres-fast.query(px,pz).surfaceHeightMetres)>1e-5f){stretched=true;break;}
        }
        require(stretched,"contact speed stretches the footprint along travel");
        // A stomp has no horizontal impulse, so there is no direction to stretch along and
        // the two speeds must describe the same still contact.
        engine::water::Body stompSlow(pool()),stompFast(pool());
        require(stompSlow.interact(0,0,.65f,-.08f,0,0,0.0f)&&stompFast.interact(0,0,.65f,-.08f,0,0,9.0f),
                "directionless contacts accepted at both speeds");
        require(stompSlow.digest()==stompFast.digest(),"an undirected contact is not stretched by speed");
        bool speedRejected=false;
        try{fast.interact(0,0,.65f,-.04f,0,0,-1.0f);}catch(const engine::water::Error&){speedRejected=true;}
        require(speedRejected,"negative impact speed rejected");
        bool runawayRejected=false;
        try{fast.interact(0,0,.65f,-.04f,0,0,80.0f);}catch(const engine::water::Error&){runawayRejected=true;}
        require(runawayRejected,"unphysical impact speed rejected");
    }
    for(unsigned i=0;i<480;++i)body.step(1.f/120);
    require(body.diagnostics().nonFinite==0,"finite disturbed evolution");
    require(std::fabs(body.diagnostics().massErrorFraction)<.001f,"disturbed evolution conserves volume");
    require(!body.diagnostics().cflClamped,"fixture remains within CFL substep capacity");
    host::PondDemo a,b;
    for(unsigned tick=1;tick<=240;++tick){engine::Vec3 position{0,0,6-tick*.025f};a.update(tick,position);b.update(tick,position);}
    const auto surface=a.surface(),replay=b.surface();
    require(a.disturbances()>0,"fixed tick movement generates disturbances");
    require(a.disturbances()==b.disturbances(),"replay event count agrees");
    require(surface.columns==129&&surface.rows==129&&surface.vertices.size()==16641,"continuous surface dimensions");
    require(surface.vertices.size()==replay.vertices.size(),"replay mesh dimensions agree");
    for(std::size_t i=0;i<surface.vertices.size();++i){const auto& v=surface.vertices[i];const auto& r=replay.vertices[i];
        const float n=v.normal[0]*v.normal[0]+v.normal[1]*v.normal[1]+v.normal[2]*v.normal[2];
        require(std::isfinite(v.position[1])&&std::fabs(n-1)<.0001f,"finite normalized surface");
        require(v.position[1]==r.position[1],"replay heights agree");
    }
    // The residual feed must mirror the authoritative interactions one for one. A
    // duplicate would double a splash, a missing event would leave the CPU surface
    // rippling with no visible detail.
    require(!surface.events.empty(),"surface publishes residual events");
    require(surface.events.size()==std::min<std::size_t>(a.disturbances(),32),"residual events mirror interactions");
    require(surface.events.size()==replay.events.size(),"replay publishes the same residual events");
    require(surface.rippleExtent[0]==8&&surface.rippleExtent[1]==8,"residual footprint follows the water extent");
    // The residual shaders map texel (0,0) to the published origin, so the origin must
    // be the footprint corner, not the body centre. Publishing the centre shifted every
    // ripple half an extent away from the interaction that created it.
    require(surface.rippleOrigin[0]==-4&&surface.rippleOrigin[1]==-4,"residual footprint is published corner-first");
    require(surface.rippleOrigin[0]+surface.rippleExtent[0]*.5f==0&&surface.rippleOrigin[1]+surface.rippleExtent[1]*.5f==0,
            "residual footprint stays centred on the body origin");
    std::uint32_t previousTick=0;
    for(std::size_t i=0;i<surface.events.size();++i){const auto& e=surface.events[i];const auto& r=replay.events[i];
        require(e.tick>previousTick,"residual events carry increasing ticks");previousTick=e.tick;
        require(e.tick==r.tick&&e.x==r.x&&e.z==r.z&&e.radius==r.radius&&e.displacement==r.displacement,"replay events agree");
        const auto& authoritative=a.body().events()[i];
        require(e.directionX==authoritative.directionX&&e.directionZ==authoritative.directionZ&&e.stretch==authoritative.stretch,
                "render feed preserves the authoritative contact direction and aspect");
        require(e.directionX==r.directionX&&e.directionZ==r.directionZ&&e.stretch==r.stretch,"replay preserves wake direction");
        require(e.stretch>=.5f&&e.stretch<1,"moving contact retains a directional footprint");
        require(e.radius>.0f&&e.radius<=8&&std::fabs(e.displacement)<=.5f,"event radius and displacement stay bounded");
        require(e.x>=-4&&e.x<=4&&e.z>=-4&&e.z<=4,"event position lies inside the footprint");
        // Same mapping the vertex and compute stages apply: an event outside [0,1] would
        // be invisible detail that the CPU still paid for.
        const float u=(e.x-surface.rippleOrigin[0])/surface.rippleExtent[0];
        const float v=(e.z-surface.rippleOrigin[1])/surface.rippleExtent[1];
        require(u>=0&&u<=1&&v>=0&&v<=1,"event maps inside the residual UV range");
        require(e.tick<=240u,"event tick stays inside the simulated window");
    }
    // The footprint derives the contact speed from the same stride displacement it uses
    // for the impulse; a moving character therefore reports a bounded, nonzero speed and
    // the replay reproduces it bit for bit. Splash intensity consumes this and nothing
    // else, so a wrong value here is a wrong amount of water in the air.
    float peakImpact=0,minRadius=1e9f,maxRadius=0,minDepth=0,maxDepth=-1e9f;
    {
        std::uint32_t previousTick=0;
        const auto& walked=a.body().events();const auto& replayed=b.body().events();
        require(walked.size()==replayed.size(),"impact history replays with the same length");
        for(std::size_t i=0;i<walked.size();++i){
            const auto& e=walked[i];
            require(e.tick>previousTick,"impact events carry increasing ticks");previousTick=static_cast<std::uint32_t>(e.tick);
            require(e.impactSpeed==replayed[i].impactSpeed,"replay reproduces the contact speed");
            require(std::isfinite(e.impactSpeed)&&e.impactSpeed>=0&&e.impactSpeed<=20.0001f,"contact speed stays bounded");
            peakImpact=std::max(peakImpact,e.impactSpeed);
            require(e.radius==replayed[i].radius&&e.displacement==replayed[i].displacement,"replay reproduces the footprint");
            minRadius=std::min(minRadius,e.radius);maxRadius=std::max(maxRadius,e.radius);
            minDepth=std::min(minDepth,e.displacement);maxDepth=std::max(maxDepth,e.displacement);
        }
        require(peakImpact>0.1f,"a walking contact reports a usable impact speed");
        // Consecutive steps must not be the same wave: the radius and the displaced depth
        // are jittered per contact, and both stay inside the authored band.
        require(maxRadius-minRadius>0.05f,"footprint radius varies between contacts");
        require(maxDepth-minDepth>0.005f,"displaced depth varies between contacts");
        require(minRadius>=0.05f&&maxRadius<=1.2f,"jittered radius stays physical");
        require(maxDepth<0&&minDepth>-0.3f,"jittered depth stays physical");
    }
    // A requested tick larger than the substep budget can resolve must shrink the
    // advanced span, not stretch the substep past its CFL bound. The unresolved
    // time is carried and repaid by later ticks.
    float debtAfterStretch=0;
    {
        engine::water::Body stretched(pool());
        stretched.step(.25f);
        const auto first=stretched.diagnostics();
        require(first.cflClamped,"oversized fixed tick reports CFL clamping");
        require(first.substeps>=1&&first.substeps<=8,"substeps stay inside the asset budget");
        require(first.lastCflNumber<=1.0001f,"no substep exceeds its CFL bound");
        debtAfterStretch=first.timeDebtSeconds;
        require(debtAfterStretch>.0f&&debtAfterStretch<=.25f,"unresolved time is carried as bounded debt");
        for(unsigned i=0;i<240;++i)stretched.step(1.f/120);
        const auto recovered=stretched.diagnostics();
        require(recovered.timeDebtSeconds==0,"debt is repaid when capacity allows");
        require(!recovered.cflClamped,"recovered tick advances the whole step");
        require(recovered.nonFinite==0&&recovered.sanitizedCells==0,"debt repayment stays finite");
        require(std::fabs(recovered.massErrorFraction)<.001f,"debt repayment conserves volume");
    }
    // A violent impulse drives the local wave speed far past the budget. The
    // authority must stay finite and deterministic, never hide a fault, and
    // account for unresolved time instead of integrating past CFL.
    float worstCfl=0, stressDebt=0, stressDropped=0;std::uint64_t stressSanitized=0;
    {
        const auto makeViolent=[&]{engine::water::Body body(pool());body.interact(0,0,.3f,0,9e4f,0);return body;};
        auto violent=makeViolent(),replayViolent=makeViolent();
        bool clamped=false;
        for(unsigned i=0;i<600;++i){
            violent.step(1.f/120);replayViolent.step(1.f/120);
            const auto d=violent.diagnostics();worstCfl=std::max(worstCfl,d.lastCflNumber);
            clamped=clamped||d.cflClamped;
            require(d.nonFinite==0,"violent transient never leaves non-finite state");
        }
        const auto d=violent.diagnostics();
        require(clamped,"violent impulse exceeds the substep budget");
        require(worstCfl<=1.0001f,"violent transient never integrates past the CFL bound");
        require(d.timeDebtSeconds<=.25f,"violent transient debt stays capped");
        require(d.timeDebtSeconds==0||d.droppedSeconds>0,"unresolved violent time is repaid or explicitly dropped");
        require(d.massErrorFraction<.05f,"violent transient keeps its mass ledger");
        require(violent.digest()==replayViolent.digest(),"violent transient replays deterministically");
        stressDebt=d.timeDebtSeconds;stressDropped=d.droppedSeconds;stressSanitized=d.sanitizedCells;
    }
    // ---- M1: event identity, same-tick multiplicity, capacity and reset epoch ----
    std::uint64_t sameTickContacts=0;std::uint32_t firstSequence=0,lastSequence=0,epochBefore=0,epochAfter=0;
    {
        engine::water::Body water(pool());
        epochBefore=water.timelineEpoch();
        require(water.interact(0,0,.65f,-.05f,0,2,1.5f),"first same-tick contact accepted");
        require(water.interact(1,1,.5f,-.04f,1,0,1.0f),"second same-tick contact accepted");
        const auto& events=water.events();
        require(events.size()==2,"both same-tick contacts are recorded");
        require(events[0].tick==events[1].tick,"same-tick contacts share the occurrence tick");
        require(events[1].sequence==events[0].sequence+1,"sequence is the identity, not the tick");
        require(events[0].timelineEpoch==epochBefore&&events[1].timelineEpoch==epochBefore,"events carry the timeline epoch");
        sameTickContacts=events.size();firstSequence=events.front().sequence;lastSequence=events.back().sequence;
        water.reset();
        epochAfter=water.timelineEpoch();
        require(epochAfter==epochBefore+1,"reset advances the timeline epoch");
        require(water.events().empty(),"reset clears the contact history");
        require(water.interact(0,0,.65f,-.02f,0,0,0),"a contact after reset is accepted");
        require(water.events().back().sequence>0,"sequence restarts above the fresh cursor");
        engine::water::Body ring(pool());
        for(unsigned i=0;i<200;++i)ring.interact(0,0,.3f,0,0,0,0);
        require(ring.events().size()<=128,"contact history stays bounded");
        require(ring.droppedEvents()>0,"evicted contacts are counted instead of disappearing silently");
    }
    // ---- M1: per-foot contact state machine ----
    unsigned footEntries=0,footDrags=0;
    {
        engine::water::Body water(pool());
        engine::water::FootContactState left,right;
        using engine::water::ContactSource;
        using engine::water::ContactKind;
        // Observe soles just above the still surface.
        engine::water::updateFootContact(water,{-0.35f,.40f,.2f},1.f/120,1,left,{},1,ContactSource::LeftFoot);
        engine::water::updateFootContact(water,{0.35f,.40f,.2f},1.f/120,2,right,{},1,ContactSource::RightFoot);
        // Both feet cross the surface within the same fixed tick.
        const auto landLeft=engine::water::updateFootContact(water,{-0.35f,.385f,.2f},1.f/120,1,left,{},1,ContactSource::LeftFoot);
        const auto landRight=engine::water::updateFootContact(water,{0.35f,.10f,.2f},1.f/120,2,right,{},1,ContactSource::RightFoot);
        require(landLeft.interacted&&landRight.interacted,"both soles can contact water in one tick");
        require(water.events().size()==2,"the two feet do not overwrite each other");
        require(water.events()[0].tick==water.events()[1].tick,"same-tick foot contacts share a tick");
        require(water.events()[0].contactId==1&&water.events()[1].contactId==2,"each foot carries its own contact id");
        require(water.events()[0].source==std::uint8_t(ContactSource::LeftFoot)&&
                water.events()[1].source==std::uint8_t(ContactSource::RightFoot),"contact source distinguishes the feet");
        require(water.events()[0].kind==std::uint8_t(ContactKind::FootPlant),"a moderate descent plants a foot");
        require(water.events()[1].kind==std::uint8_t(ContactKind::Landing),"a fast descent is a landing");
        require(water.events()[1].normalSpeed>water.events()[0].normalSpeed,"the landing records a stronger normal speed");
        footEntries=2;
        // Standing still must not keep pulsing.
        const auto standing=water.events().size();
        for(unsigned i=0;i<240;++i)
            engine::water::updateFootContact(water,{-0.35f,.32f,.2f},1.f/120,1,left,{},1,ContactSource::LeftFoot);
        require(water.events().size()==standing,"a standing foot emits no repeated pulses");
        // Dragging integrates along distance.
        for(unsigned i=0;i<24;++i)
            engine::water::updateFootContact(water,{-0.35f+float(i)*.02f,.32f,.2f},1.f/120,1,left,{},1,ContactSource::LeftFoot);
        require(water.events().size()>standing,"dragging emits distance-gated tail contacts");
        footDrags=unsigned(water.events().size()-standing);
        // Leaving is weak.
        const float entryDisplacement=std::fabs(water.events()[0].displacement);
        const auto leave=engine::water::updateFootContact(water,{0.11f,.95f,.2f},1.f/120,1,left,{},1,ContactSource::LeftFoot);
        require(leave.interacted,"lifting a submerged foot emits a leaving contact");
        require(leave.event.kind==std::uint8_t(ContactKind::Leaving),"leaving is not an entry");
        require(std::fabs(leave.event.displacement)<entryDisplacement,"leaving is weaker than entry");
        // Teleport: a huge step is not a stroke.
        const auto beforeTeleport=water.events().size();
        engine::water::updateFootContact(water,{3.0f,.32f,.2f},1.f/120,1,left,{},1,ContactSource::LeftFoot);
        require(water.events().size()==beforeTeleport,"a teleport does not fabricate a wake");
        // Vertical arrival vs careful plant at the same spot: the jitter is identical, so
        // the only difference left is the normal speed.
        engine::water::Body shallowWater(pool()),deepWater(pool());
        engine::water::FootContactState slow,fast;
        engine::water::updateFootContact(shallowWater,{0,.40f,0},1.f/120,3,slow,{},1,ContactSource::LeftFoot);
        engine::water::updateFootContact(deepWater,{0,.40f,0},1.f/120,4,fast,{},1,ContactSource::LeftFoot);
        const auto planted=engine::water::updateFootContact(shallowWater,{0,.385f,0},.1f,3,slow,{},1,ContactSource::LeftFoot);
        const auto dropped=engine::water::updateFootContact(deepWater,{0,.15f,0},.1f,4,fast,{},1,ContactSource::LeftFoot);
        require(planted.interacted&&dropped.interacted,"both vertical contacts are accepted");
        require(dropped.normalSpeed>planted.normalSpeed,"a drop records a larger normal speed");
        require(std::fabs(dropped.displacementMetres)>std::fabs(planted.displacementMetres),
                "a drop disturbs more water than a careful plant");
    }
    // ---- M2: directional rotation and non-square footprint ----
    double rotationAgreement=0,nonSquareAspect=0;
    {
        const auto build=[&](float ix,float iz){
            engine::water::Body water(pool());water.interact(0,0,.8f,-.09f,ix,iz,3.f);return water;
        };
        auto moments=[&](const engine::water::Body& water,float ax,float az,double& along,double& across){
            along=across=0;
            for(unsigned z=0;z<64;++z)for(unsigned x=0;x<64;++x){
                const float px=-4+(x+.5f)*.125f,pz=-4+(z+.5f)*.125f;
                const auto q=water.query(px,pz);const double w=std::fabs(q.depthMetres-.4);
                const double a=double(px)*ax+double(pz)*az,c=-double(px)*az+double(pz)*ax;
                along+=w*a*a;across+=w*c*c;
            }
        };
        double along0=0,across0=0,along45=0,across45=0;
        moments(build(0,4),0,1,along0,across0);
        const float d=1.f/std::sqrt(2.f);
        moments(build(4*d,4*d),d,d,along45,across45);
        require(along0>across0*1.1f&&along45>across45*1.1f,"a directed contact stretches along travel at both angles");
        rotationAgreement=std::fabs(along0/across0-along45/across45);
        require(rotationAgreement<.25,"the stretched footprint rotates with the contact direction");
        auto rectangular=std::make_shared<engine::water::Asset>(*pool());
        rectangular->extentZ=4;rectangular->rows=32;
        engine::water::Body round(rectangular);round.interact(0,0,.7f,-.08f,0,0,0);
        double xx=0,zz=0;
        for(unsigned z=0;z<32;++z)for(unsigned x=0;x<64;++x){
            const float px=-4+(x+.5f)*.125f,pz=-2+(z+.5f)*.125f;
            const double w=std::fabs(round.query(px,pz).depthMetres-.4);
            xx+=w*px*px;zz+=w*pz*pz;
        }
        nonSquareAspect=xx/zz;
        require(std::fabs(nonSquareAspect-1)<.08,"a radial contact stays round on a non-square footprint");
    }
    // ---- M3: slope equilibrium, localised river and drain limits ----
    double riverVelocity=0,riverLedgerError=0;
    {
        auto slope=std::make_shared<engine::water::Asset>(*pool());
        slope->bathymetry.type=engine::water::BathymetryType::Slope;
        slope->bathymetry.heightMetres=-.2f;slope->bathymetry.slopeX=.05f;
        engine::water::Body calm(slope);
        const float west=calm.query(-3,0).surfaceHeightMetres,east=calm.query(3,0).surfaceHeightMetres;
        for(unsigned i=0;i<480;++i)calm.step(1.f/120);
        const auto calmDiagnostics=calm.diagnostics();
        require(calmDiagnostics.maxSpeedMetresPerSecond<.002f,"a sloped bed at rest does not accelerate");
        require(calmDiagnostics.massErrorFraction<1e-4f,"sloped still water conserves volume");
        require(std::fabs(calm.query(-3,0).surfaceHeightMetres-west)<.002f&&
                std::fabs(calm.query(3,0).surfaceHeightMetres-east)<.002f,"the still free surface stays level");

        auto river=std::make_shared<engine::water::Asset>(*pool());
        river->bathymetry.type=engine::water::BathymetryType::Channel;
        river->bathymetry.heightMetres=-.3f;river->bathymetry.channelWidthMetres=4;
        river->bathymetry.depthMetres=.2f;river->stillHeightMetres=.15f;
        engine::water::FlowOpening source;
        source.x=-3.5f;source.z=0;source.radiusMetres=.6f;source.flowCubicMetresPerSecond=.02f;
        source.speedMetresPerSecond=.8f;source.directionX=1;
        engine::water::FlowOpening drain;
        drain.x=0;drain.z=0;drain.radiusMetres=.6f;drain.flowCubicMetresPerSecond=.02f;
        river->sources.push_back(source);river->drains.push_back(drain);
        engine::water::Body flow(river);
        const double initial=flow.diagnostics().volumeCubicMetres;
        for(unsigned i=0;i<240;++i)flow.step(1.f/120);
        const auto d=flow.diagnostics();
        require(d.sourceCells>0&&d.drainCells>0,"localised openings cover cells");
        require(d.inflowRequestedCubicMetres>0&&
                std::fabs(d.inflowActualCubicMetres-d.inflowRequestedCubicMetres)<1e-6f,
                "the source delivers the requested volume");
        require(d.outflowActualCubicMetres>0&&
                d.outflowLimitedCubicMetres<d.outflowRequestedCubicMetres*.25f,
                "the drain removes water without starving");
        riverLedgerError=std::fabs((d.volumeCubicMetres-initial)-(d.addedCubicMetres-d.removedCubicMetres));
        require(riverLedgerError<1e-3,"the flow ledger closes");
        require(d.massErrorFraction<1e-3f,"the river keeps its mass ledger");
        const auto nearInlet=flow.query(-3.55f,0);
        riverVelocity=nearInlet.velocityX;
        require(nearInlet.velocityX>.002f,"the inflow drives a downstream current");
        require(std::fabs(nearInlet.velocityZ)<.2f,"the channel keeps the current downstream");

        const auto build=[](float cellMetres,unsigned columns,unsigned rows){
            auto asset=std::make_shared<engine::water::Asset>();
            asset->id="water-resolution-test";asset->backend=engine::water::Backend::Shallow;
            asset->extentX=columns*cellMetres;asset->extentZ=rows*cellMetres;asset->cellMetres=cellMetres;
            asset->columns=columns;asset->rows=rows;asset->stillHeightMetres=.4f;asset->maxSubsteps=8;
            asset->maxCells=4096;
            engine::water::FlowOpening inlet;
            inlet.x=0;inlet.z=0;inlet.radiusMetres=.5f;inlet.flowCubicMetresPerSecond=.02f;
            inlet.speedMetresPerSecond=.4f;inlet.directionX=1;
            asset->sources.push_back(inlet);
            return asset;
        };
        engine::water::Body fine(build(.125f,64,64)),coarse(build(.25f,32,32));
        for(unsigned i=0;i<120;++i){fine.step(1.f/120);coarse.step(1.f/120);}
        const auto fineDiagnostics=fine.diagnostics(),coarseDiagnostics=coarse.diagnostics();
        require(std::fabs(fineDiagnostics.inflowActualCubicMetres-coarseDiagnostics.inflowActualCubicMetres)<1e-4f,
                "a specified flow does not change with grid resolution");
        require(std::fabs(fineDiagnostics.maxDepthMetres-coarseDiagnostics.maxDepthMetres)<
                .12f*std::max(.01f,fineDiagnostics.maxDepthMetres),"the deposited depth stays resolution-independent");

        auto dry=std::make_shared<engine::water::Asset>(*pool());
        dry->stillHeightMetres=-1;
        engine::water::FlowOpening hungry;
        hungry.x=0;hungry.z=0;hungry.radiusMetres=.6f;hungry.flowCubicMetresPerSecond=.5f;
        dry->drains.push_back(hungry);
        engine::water::Body empty(dry);
        require(empty.diagnostics().volumeCubicMetres==0,"the dry fixture starts empty");
        for(unsigned i=0;i<10;++i)empty.step(1.f/120);
        const auto emptyDiagnostics=empty.diagnostics();
        require(emptyDiagnostics.outflowActualCubicMetres==0,"a dry drain withdraws nothing");
        require(emptyDiagnostics.outflowLimitedCubicMetres>0,"the dry drain reports its shortfall");
        require(emptyDiagnostics.massErrorFraction<1e-6f,"a dry drain cannot create negative water");
    }
    // ---- M4: submerged rock, emergent rock and wall ----
    double obstacleMassError=0;
    {
        auto emergent=std::make_shared<engine::water::Asset>(*pool());
        engine::water::Obstacle rock;rock.x=0;rock.z=0;rock.radiusMetres=.6f;rock.topHeightMetres=.8f;
        emergent->obstacles.push_back(rock);
        engine::water::Body blocked(emergent);
        require(blocked.solidAt(32,32),"an emergent obstacle marks a solid cell");
        require(blocked.query(0,0).depthMetres==0,"there is no water inside the solid footprint");
        require(!blocked.interact(0,0,.4f,-.05f,0,0,0),"the solver rejects a contact inside rock");
        const double initial=blocked.diagnostics().volumeCubicMetres;
        for(unsigned i=0;i<240;++i)blocked.step(1.f/120);
        const auto d=blocked.diagnostics();
        obstacleMassError=d.massErrorFraction;
        require(d.massErrorFraction<.001f,"the displaced volume is baked into the baseline");
        require(std::fabs(d.volumeCubicMetres-initial)<.01f,"the wall does not leak or pump water");
        require(d.solidCells>0&&blocked.depthAt(32,32)==0,"the solid cell stays dry");

        auto submerged=std::make_shared<engine::water::Asset>(*pool());
        engine::water::Obstacle low;low.x=0;low.z=0;low.radiusMetres=.8f;low.topHeightMetres=.2f;
        submerged->obstacles.push_back(low);
        engine::water::Body covered(submerged);
        require(!covered.solidAt(32,32),"a low rock is not solid");
        require(std::fabs(covered.bedAt(0,0)-.2f)<1e-6f,"a low rock raises the bed");
        require(covered.query(0,0).depthMetres>.15f,"water still covers a low rock");
        for(unsigned i=0;i<240;++i)covered.step(1.f/120);
        require(covered.diagnostics().massErrorFraction<.001f,"water flows over a low rock without leaking");

        auto wall=std::make_shared<engine::water::Asset>(*pool());
        engine::water::Obstacle pier;
        pier.x=0;pier.z=0;pier.radiusMetres=.8f;pier.topHeightMetres=1.2f;
        wall->obstacles.push_back(pier);
        engine::water::FlowOpening inlet;
        inlet.x=-2.5f;inlet.z=0;inlet.radiusMetres=.5f;inlet.flowCubicMetresPerSecond=.05f;
        inlet.speedMetresPerSecond=.6f;inlet.directionX=1;
        engine::water::FlowOpening outlet;
        outlet.x=2.5f;outlet.z=0;outlet.radiusMetres=.5f;outlet.flowCubicMetresPerSecond=.05f;
        wall->sources.push_back(inlet);wall->drains.push_back(outlet);
        engine::water::Body around(wall);
        for(unsigned i=0;i<480;++i)around.step(1.f/120);
        const auto aroundDiagnostics=around.diagnostics();
        require(aroundDiagnostics.massErrorFraction<.002f,"flow around a wall keeps its ledger");
        require(around.depthAt(32,32)==0,"the wall stays impermeable under flow");
        float bypass=0;
        for(unsigned z=0;z<64;++z)for(unsigned x=0;x<64;++x){
            const float px=-4+(x+.5f)*.125f,pz=-4+(z+.5f)*.125f;
            const auto q=around.query(px,pz);
            if(std::fabs(pz)>.9f)bypass=std::max(bypass,std::fabs(q.velocityX));
        }
        require(bypass>.002f,"water is diverted around the wall instead of stopping");
    }
    // ---- M5: reaction impulse and foam transport ----
    double reactionApplied=0,foamDownstream=0,foamFar=0;
    {
        engine::water::Body water(pool());
        water.interact(0,0,.6f,-.04f,0,10,1);
        const double before=momentum(water);
        reactionApplied=water.applyImpulse(0,0,.6f,0,-4);
        const auto d=water.diagnostics();
        require(reactionApplied>0&&std::fabs(d.reactionImpulseNs-4)<1e-3f,"the reaction impulse is recorded");
        require(std::fabs((momentum(water)-before)+4)<.01,"the reaction removes water momentum");
        require(std::fabs(d.volumeCubicMetres-d.initialVolumeCubicMetres)<.001f,"the reaction moves no water");

        // A uniform current with no surface slope isolates the foam scalar: the level does
        // not change, so the "disturbed surface" foam source cannot paint the whole grid.
        // 25.6 kN s over a 8x8 m, 0.4 m deep pool is 1 m/s.
        auto river=std::make_shared<engine::water::Asset>(*pool());
        river->material.foamThreshold=5.f;
        host::PondDemo demo(river,{},{.65f,.24f,.14f},{.035f,.12f,.095f},.09f);
        demo.update(1,{0,0,20});
        demo.bodyHandle()->applyImpulse(0,0,7.f,25600.f,0);
        demo.surface();demo.seedFoam(-2,0,.15f,1.f);
        for(unsigned tick=2;tick<=41;++tick){demo.update(tick,{0,0,20});demo.surface();}
        foamDownstream=demo.foamAt(-1.8f,0);
        foamFar=demo.foamAt(1.f,0);
        require(demo.query(-2,0).velocityX>.5f,"the foam fixture has a uniform current");
        require(foamDownstream>.1f,"foam is carried downstream by the flow");
        require(foamDownstream>foamFar+.05f,"foam does not stay pinned to its source");

        host::PondDemo still(pool(),{},{.65f,.24f,.14f},{.035f,.12f,.095f},.09f);
        still.update(1,{0,0,20});
        still.surface();still.seedFoam(-2,0,.15f,1.f);
        for(unsigned tick=2;tick<=41;++tick){still.update(tick,{0,0,20});still.surface();}
        require(still.foamAt(-2,0)>.1f,"without flow the foam stays at its source");
        require(still.foamAt(-1.8f,0)<.05f,"without flow the foam does not drift");
    }
    // ---- M6: waterfall transfer ledger ----
    double transferBefore=0,transferAfter=0,transferInTransit=0;
    {
        engine::water::Body upstream(pool()),downstream(pool());
        engine::water::TransferSettings settings;
        settings.sourceX=-2;settings.sourceZ=0;settings.sourceRadiusMetres=.8f;
        settings.landingX=2;settings.landingZ=0;settings.landingRadiusMetres=.8f;
        settings.requestedFlowCubicMetresPerSecond=.05f;
        settings.transitSeconds=.25f;settings.exitNormalSpeed=3;
        engine::water::TransferLink link(&upstream,&downstream,settings);
        transferBefore=link.totalVolumeCubicMetres();
        for(unsigned i=0;i<240;++i){
            link.step(1.f/120);upstream.step(1.f/120);downstream.step(1.f/120);
        }
        const auto d=link.diagnostics();
        transferInTransit=d.inTransitVolumeCubicMetres;
        transferAfter=link.totalVolumeCubicMetres();
        require(d.withdrawnVolumeCubicMetres>0&&d.deliveredVolumeCubicMetres>0&&d.arrivals>0,
                "the link withdraws, transports and delivers water");
        require(d.lostVolumeCubicMetres==0,"the configured landing region accepts every drop");
        require(d.landingEvents>0&&d.landingEvents<d.arrivals,
                "visual landing contacts are coalesced while every volume arrival is delivered");
        // The transfer itself is exact. The two bodies also carry their own bounded
        // floating-point drift, measured separately, so the combined tolerance states both.
        require(std::fabs(d.withdrawnVolumeCubicMetres-
                          (d.deliveredVolumeCubicMetres+d.inTransitVolumeCubicMetres+
                           d.lostVolumeCubicMetres))<1e-6,
                "withdrawn water is fully delivered, in transit or reported lost");
        require(upstream.diagnostics().massErrorFraction<1e-4f&&
                downstream.diagnostics().massErrorFraction<1e-4f,
                "each side of the transfer keeps its own mass ledger");
        require(std::fabs(transferAfter-transferBefore)<5e-3,
                "upstream + in transit + downstream is conserved for the session");
        require(!downstream.events().empty(),"the landing contact reaches the downstream history");
        const double withdrawn=d.withdrawnVolumeCubicMetres;
        link.setEnabled(false);
        for(unsigned i=0;i<60;++i){link.step(1.f/120);upstream.step(1.f/120);downstream.step(1.f/120);}
        require(std::fabs(link.diagnostics().withdrawnVolumeCubicMetres-withdrawn)<1e-9,
                "a disabled link stops withdrawing new water");
        engine::water::TransferLink restart(&upstream,&downstream,settings);
        restart.step(1.f/120);
        const double beforeReset=restart.totalVolumeCubicMetres();
        require(restart.diagnostics().inTransitVolumeCubicMetres>0,"the restarted link holds water in transit");
        restart.reset();
        require(restart.diagnostics().inTransitVolumeCubicMetres==0,"reset clears the in-transit ledger");
        require(std::fabs(restart.totalVolumeCubicMetres()-beforeReset)<1e-3,
                "reset returns in-transit water instead of duplicating it");
        // Local transfer regions must move with a translated water body.
        engine::water::Body translatedUp(pool(),{3,2.5f,-4}),translatedDown(pool(),{-5,1.2f,6});
        engine::water::TransferLink moved(&translatedUp,&translatedDown,settings);
        for(unsigned i=0;i<120;++i){
            moved.step(1.f/120);translatedUp.step(1.f/120);translatedDown.step(1.f/120);
        }
        require(!translatedDown.events().empty(),"a translated landing still produces a contact");
        const auto& landing=translatedDown.events().back();
        require(std::fabs(landing.x-(-5.f+settings.landingX))<.05f&&
                std::fabs(landing.z-(6.f+settings.landingZ))<.05f,
                "the landing contact is placed in world space, not at the local origin");
        require(translatedUp.diagnostics().volumeCubicMetres<
                translatedUp.diagnostics().initialVolumeCubicMetres,
                "the translated source region is actually drained");
    }
    // ---- M7: presentation LOD and timing are isolated from the authority ----
    {
        host::PondDemo demo;
        for(unsigned tick=1;tick<=30;++tick)demo.update(tick,{0,0,6-tick*.02f});
        const auto digestBefore=demo.body().digest();
        const float volumeBefore=demo.diagnostics().volumeCubicMetres;
        const auto validFrame=[](const nativegpu::WaterSurfaceFrame& frame){
            if(frame.columns<2||frame.rows<2||frame.vertices.size()!=std::size_t(frame.columns)*frame.rows)return false;
            for(const auto& vertex:frame.vertices)
                for(float value:vertex.position)if(!std::isfinite(value))return false;
            for(std::size_t i=0;i<frame.events.size();++i){
                if(frame.events[i].timelineEpoch!=frame.timelineEpoch)return false;
                if(i&&frame.events[i].sequence<=frame.events[i-1].sequence)return false;
            }
            return true;
        };
        const auto full=demo.surface(0,0),reduced=demo.surface(0,2);
        require(validFrame(full),"the full-detail frame is well formed");
        require(validFrame(reduced),"the reduced-detail frame is well formed");
        require(full.columns==129&&reduced.columns==33,"LOD changes only the presentation mesh");
        require(demo.body().digest()==digestBefore&&demo.diagnostics().volumeCubicMetres==volumeBefore,
                "surface LOD leaves the authoritative body untouched");
        require(demo.diagnostics().lastStepMilliseconds>=0.f,"the solver reports its step cost");
        // Presentation work must not feed back. One run builds surfaces and foam every
        // tick; the other never does. The authoritative digest has to stay identical.
        host::PondDemo rendered,headless;
        for(unsigned tick=1;tick<=120;++tick){
            const engine::Vec3 player{0,0,6-tick*.02f};
            rendered.update(tick,player);rendered.surface();
            headless.update(tick,player);
        }
        require(rendered.body().digest()==headless.body().digest(),
                "surface and foam work do not change the authoritative body");
        require(rendered.disturbances()==headless.disturbances(),
                "presentation does not change the contact history");
    }
    // ---- M7: render-frame grouping must not change the authoritative water digest ----
    // The same one second is delivered as 30, 60 and 144 frames; the logical clock
    // still produces 120 fixed ticks, and the authority must be bit-identical.
    std::uint64_t clockedDigest=0;
    {
        const auto runClocked=[&](unsigned fps){
            engine::water::Body clocked(pool());
            engine::simulation::Clock clock;
            std::uint64_t previous=0;
            for(unsigned frame=1;frame<=fps;++frame){
                const auto ns=frame*1000000000ull/fps;
                clock.addNanoseconds(ns-previous);previous=ns;
                while(clock.consumeTick()){
                    if(clock.tick()%40==0)
                        require(clocked.interact(0,0,.65f,-.05f,0,3.f,1.2f),
                                "clocked contact accepted");
                    clocked.step(1.f/120);
                }
            }
            require(clock.tick()==120,"render grouping changed the logical tick count");
            return clocked.digest();
        };
        clockedDigest=runClocked(60);
        require(runClocked(30)==clockedDigest,
                "30 fps grouping changed the authoritative water digest");
        require(runClocked(144)==clockedDigest,
                "144 fps grouping changed the authoritative water digest");
    }
    // ---- M7: surface wetness history ----
    double wetnessAfter6s=0,wetnessAfter30s=0;
    {
        std::array<host::WetnessSample,5> samples{};
        for(auto& sample:samples){sample.valid=true;sample.surfaceHeightMetres=.16f;}
        require(std::fabs(host::wetnessTarget(samples,-1.f,true)-1.f)<1e-6f,
                "a fully covered bed is wet");
        samples[4].valid=false;
        require(host::wetnessTarget(samples,-1.f,true)==0.f,
                "a partly covered courtyard is not painted wet as one sheet");
        require(host::wetnessTarget(samples,-1.f,false)==1.f,
                "a partially touched prop is wet");
        require(host::wetnessTarget(samples,.30f,false)==0.f,
                "a prop above the water stays dry");
        host::WetnessState state;
        host::updateWetness(state,1.f,.1f);
        require(state.value==1.f,"wetting is immediate");
        for(unsigned i=0;i<720;++i)host::updateWetness(state,0.f,1.f/120);
        wetnessAfter6s=state.value;
        require(wetnessAfter6s>.15f&&wetnessAfter6s<.5f,
                "wetness dries over seconds instead of one frame");
        for(unsigned i=0;i<3600;++i)host::updateWetness(state,0.f,1.f/120);
        wetnessAfter30s=state.value;
        require(wetnessAfter30s<.02f,"wetness fully dries");
    }
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"massError\":"<<body.diagnostics().massErrorFraction
        <<",\"disturbances\":"<<a.disturbances()<<",\"vertices\":"<<surface.vertices.size()
        <<",\"peakImpactSpeed\":"<<peakImpact
        <<",\"cfl\":{\"stretchDebtSeconds\":"<<debtAfterStretch<<",\"worstCflNumber\":"<<worstCfl
        <<",\"stressDebtSeconds\":"<<stressDebt<<",\"stressDroppedSeconds\":"<<stressDropped
        <<",\"stressSanitizedCells\":"<<stressSanitized<<"}"
        <<",\"contacts\":{\"sameTick\":"<<sameTickContacts<<",\"firstSequence\":"<<firstSequence
        <<",\"lastSequence\":"<<lastSequence<<",\"epochBefore\":"<<epochBefore<<",\"epochAfter\":"<<epochAfter
        <<",\"footEntries\":"<<footEntries<<",\"footDrags\":"<<footDrags<<"}"
        <<",\"propagation\":{\"rotationAgreement\":"<<rotationAgreement<<",\"nonSquareAspect\":"<<nonSquareAspect<<"}"
        <<",\"river\":{\"velocity\":"<<riverVelocity<<",\"ledgerError\":"<<riverLedgerError<<"}"
        <<",\"obstacleMassError\":"<<obstacleMassError
        <<",\"clock\":{\"fps\":[30,60,144],\"ticks\":120,\"digest\":"<<clockedDigest<<"}"
        <<",\"wetness\":{\"after6s\":"<<wetnessAfter6s<<",\"after30s\":"<<wetnessAfter30s<<"}"
        <<",\"reactionImpulse\":"<<reactionApplied<<",\"foam\":{\"downstream\":"<<foamDownstream<<",\"far\":"<<foamFar<<"}"
        <<",\"transfer\":{\"before\":"<<transferBefore<<",\"after\":"<<transferAfter
        <<",\"inTransit\":"<<transferInTransit<<"}}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
