#include "d3d12_renderer_internal.h"
#include "frustum.hpp"
#include <atomic>
#include <algorithm>
#include <sstream>

namespace nativegpu {
bool Renderer::configureGpuEffects(std::vector<std::shared_ptr<const engine::effects::Asset>> assets,
                                  std::map<std::string,unsigned> slots,GpuEffectDriver driver,std::string& error){
    error.clear();auto& s=*impl_;
    if(s.initialized){error="NG-E-EFFECT-STATE: GPU effect configuration requires a stopped renderer";return false;}
    if(assets.empty()||assets.size()>4096||!driver){error="NG-E-EFFECT-ASSET: missing GPU assets or frame driver";return false;}
    for(const auto& item:slots)if(item.second>=s.configuredEffectTextures.size()){error="NG-E-EFFECT-TEXTURE: GPU effect texture slot outside configured bank";return false;}
    for(auto& asset:assets){if(!asset){error="NG-E-EFFECT-ASSET: null GPU effect asset";return false;}asset=std::make_shared<const engine::effects::Asset>(*asset);}
    s.configuredGpuEffects=std::move(assets);s.configuredGpuEffectTextures=std::move(slots);s.gpuEffectDriver=std::move(driver);return true;
}
engine::effects::GpuRuntimeStats Renderer::gpuEffectStats() const{return impl_->gpuEffectRuntime?impl_->gpuEffectRuntime->stats():engine::effects::GpuRuntimeStats{};}
bool Renderer::configureGpuEffectFields(std::shared_ptr<engine::fields::World> world,engine::fields::GpuLimits storageLimits,
                                      engine::fields::GpuVectorLimits vectorLimits,std::string& error){
    error.clear();auto& s=*impl_;if(s.initialized){error="NG-E-FIELD-GPU-STATE: field configuration requires a stopped renderer";return false;}
    if(!world||!storageLimits.maxPages||storageLimits.maxPages>4096||!storageLimits.maxSnapshots||storageLimits.maxSnapshots>16||!storageLimits.maxAllocationBytes||
       !storageLimits.maxLeases||storageLimits.maxLeases>4096||!storageLimits.maxViews||storageLimits.maxViews>4096||!vectorLimits.maxBindings||vectorLimits.maxBindings>256||!vectorLimits.maxAllocationBytes){error="NG-E-FIELD-GPU-BUDGET: invalid field configuration";return false;}
    s.configuredGpuFieldWorld=std::move(world);s.configuredGpuFieldLimits=storageLimits;s.configuredGpuVectorLimits=vectorLimits;return true;
}
bool Renderer::Impl::createGpuEffects(std::string& error){
    if(configuredGpuEffects.empty())return true;
    try{gpuEffectFields=std::make_unique<engine::fields::GpuEffectFields>(device,fence);gpuEffectDeposits=&gpuEffectFields->deposits();gpuSmokeService=&gpuEffectFields->solver();gpuSmokeSharing=&gpuEffectFields->sharing();auto hooks=gpuEffectFields->hooks();
        gpuEffectRuntime=std::make_unique<engine::effects::GpuRuntime>(device,engine::effects::GpuRuntimeLimits{},std::move(hooks.deposits),std::move(hooks.admission),std::move(hooks.smoke));
        for(const auto& asset:configuredGpuEffects)gpuEffectRuntime->install(asset);
        gpuEffectSprites=std::make_unique<GpuEffectSprites>(device,detail::SceneColorFormat,detail::SceneColorFormat,detail::DepthDsvFormat);
        static std::atomic<std::uint64_t> epochs{0};gpuEffectEpoch=++epochs;return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool Renderer::Impl::recordGpuEffects(ID3D12GraphicsCommandList* list,const Mat4& vp,std::string& error){
    if(!gpuEffectRuntime)return true;
    try{const auto completed=fence->GetCompletedValue();gpuEffectRuntime->collect(completed);
        if(gpuVectorFields){gpuVectorFields->collect(completed);configuredGpuFieldWorld->collect(completed);}
        GpuEffectFrame frame{*gpuEffectRuntime,[&](std::string& message){return gpuEffectRuntime->step(list,nextFence,completed,message);},gpuEffectEpoch,{},{},{},{},{},{},{},{}};
        auto fields=fieldFrame(list);frame.acquireField=std::move(fields.acquire);frame.releaseField=std::move(fields.release);frame.publishField=std::move(fields.publish);
        frame.stepField=[&](engine::fields::Tick tick,std::string& message){return gpuEffectRuntime->step(list,nextFence,completed,message,tick);};
        frame.acquireVisualField=[this](engine::fields::LeaseRequest request){return gpuEffectFields->acquire(std::move(request));};frame.releaseVisualField=[this](engine::fields::GpuLease lease,std::string& message){return gpuEffectFields->release(lease,message);};
        frame.viewVisualField=[this](engine::fields::GpuLease lease,std::string& message){auto view=gpuEffectFields->view(lease,message);if(view&&!view->markUsed(nextFence)){message="NG-E-EFFECT-FIELDS: invalid frame read fence";return std::shared_ptr<engine::fields::GpuVisualView>{};}return view;};frame.useVisualField=[this](const std::shared_ptr<engine::fields::GpuVisualView>& view){return view&&view->markUsed(nextFence);};
        const auto frustum=frustumFromViewProjection(vp);if(!gpuEffectFields->setSmokeObserver([frustum](const engine::smoke::ServiceOutput& out){const auto& p=out.buffers.spec;const float minimum[]={p.originMetres.x,p.originMetres.y,p.originMetres.z},maximum[]={minimum[0]+p.dimensions[0]*p.cellMetres,minimum[1]+p.dimensions[1]*p.cellMetres,minimum[2]+p.dimensions[2]*p.cellMetres};return engine::smoke::SharingObservation{out.serial,!aabbOutsideFrustum(frustum,minimum,maximum)};},error))return false;
        if(!gpuEffectDriver(frame,error))return false;
        if(gpuSmokeService&&!gpuEffectRuntime->flush(list,nextFence,completed,error))return false;
        if(!gpuEffectRuntime->prepareDraw(list,nextFence,error))return false;
        std::map<std::string,unsigned> descriptors;for(const auto& item:configuredGpuEffectTextures)descriptors.emplace(item.first,detail::EffectTextureSrvBase+item.second);
        ID3D12DescriptorHeap* heaps[]={srvHeap};list->SetDescriptorHeaps(1,heaps);
        return gpuEffectSprites->prepare(list,nextFence,vp,gpuEffectRuntime->drawBatches(),descriptors,error);
    }catch(const std::exception& e){error=e.what();return false;}
}
bool Renderer::Impl::drawGpuEffects(ID3D12GraphicsCommandList* list,unsigned index,std::string& error){
    if(!gpuEffectSprites)return true;
    D3D12_CPU_DESCRIPTOR_HANDLE targets[2];targets[0]=rtvHeap->GetCPUDescriptorHandleForHeapStart();targets[0].ptr+=static_cast<SIZE_T>(detail::RtvSlotBase+index*detail::RtvSlotsPerFrame)*rtvStride;targets[1]=targets[0];targets[1].ptr+=rtvStride;
    auto z=dsvHeap->GetCPUDescriptorHandleForHeapStart();z.ptr+=static_cast<SIZE_T>(index)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);list->OMSetRenderTargets(2,targets,FALSE,&z);
    D3D12_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
    return gpuEffectSprites->draw(list,nextFence,error);
}
void Renderer::Impl::releaseGpuEffects(){
    // Renderer::shutdown has already waited for its graphics queue. These module
    // allocations are accounted separately from the renderer's tracked objects.
    gpuEffectSprites.reset();gpuEffectRuntime.reset();gpuEffectFields.reset();gpuSmokeSharing=nullptr;gpuSmokeService=nullptr;gpuEffectDeposits=nullptr;smokeScene={};smokePixelReads.clear();
}
std::string Renderer::Impl::gpuEffectsJson() const{
    const auto runtime=gpuEffectRuntime?gpuEffectRuntime->stats():engine::effects::GpuRuntimeStats{};const auto draw=gpuEffectSprites?gpuEffectSprites->stats():GpuSpriteStats{};
    const auto field=gpuFieldStorage?gpuFieldStorage->stats():engine::fields::GpuFieldStats{};const auto vectors=gpuVectorFields?gpuVectorFields->stats():engine::fields::GpuVectorStats{};
    const auto cpu=configuredGpuFieldWorld?configuredGpuFieldWorld->stats():engine::fields::Stats{};
    const auto deposits=gpuEffectDeposits?gpuEffectDeposits->stats():engine::effects::GpuDepositStats{};
    const auto smoke=gpuSmokeService?gpuSmokeService->stats():engine::smoke::ServiceStats{};
    const auto shared=gpuSmokeSharing?gpuSmokeSharing->stats():engine::smoke::SharingStats{};
    auto visualFields=gpuEffectFields?gpuEffectFields->stats():engine::fields::EffectFieldStats{};
    // These probes collect completed work independently. Sum the component
    // samples published below, so asynchronous reclamation cannot make the
    // reported total disagree with its reported parts.
    visualFields.fieldBytes=deposits.local.allocationBytes+shared.local.allocationBytes+visualFields.world.accountedBytes;
    visualFields.allocationBytes=visualFields.fieldBytes+smoke.allocationBytes;
    std::ostringstream capture;capture<<'[';for(std::size_t i=0;i<smokeCaptureSources.size();++i){if(i)capture<<',';const auto& o=smokeCaptureSources[i];capture<<"{\"serial\":"<<o.serial<<",\"sourceTick\":"<<o.sourceTick<<",\"version\":"<<o.version<<",\"representation\":\""<<(o.sharedProxy?"wf":"lef")<<"\",\"scalarElements\":"<<o.scalarElements<<",\"cellMetres\":"<<o.cellMetres<<'}';}capture<<']';
    std::ostringstream backlog;backlog<<'[';bool firstPublication=true;if(gpuEffectDeposits)for(const auto& entry:gpuEffectDeposits->publicationBacklog()){if(!firstPublication)backlog<<',';firstPublication=false;const auto& p=entry.publication;backlog<<"{\"slot\":"<<entry.instance.slot<<",\"generation\":"<<entry.instance.generation<<",\"source\":"<<entry.source<<",\"priority\":"<<entry.priority<<",\"pendingSince\":"<<p.pendingSince<<",\"lastAttemptTick\":"<<p.lastAttemptTick<<",\"lastPublishedTick\":"<<p.lastPublishedTick<<",\"deferredTicks\":"<<p.deferredTicks<<",\"overdue\":"<<(p.overdue?"true":"false")<<",\"reason\":"<<detail::jsonString(p.reason)<<'}';}backlog<<']';
    std::ostringstream out;out<<"\"gpuEffects\":{\"available\":"<<(gpuEffectRuntime?"true":"false")<<",\"simulationBackend\":\"gpu-visual\",\"tick\":"<<runtime.tick
       <<",\"instances\":"<<runtime.instances<<",\"retiredInstances\":"<<runtime.retiredInstances<<",\"telemetryValid\":"<<(runtime.telemetryValid?"true":"false")<<",\"telemetryFence\":"<<runtime.telemetryFence
       <<",\"alive\":"<<runtime.visual.alive<<",\"emitted\":"<<runtime.visual.emitted<<",\"expired\":"<<runtime.visual.expired<<",\"killed\":"<<runtime.visual.killed<<",\"dropped\":"<<runtime.visual.dropped<<",\"faults\":"<<runtime.visual.faults
       <<",\"runtimeBytes\":"<<runtime.allocationBytes<<",\"drawBytes\":"<<draw.allocationBytes<<",\"frames\":"<<draw.frames<<",\"gatherIndirects\":"<<draw.gatherIndirects<<",\"sortIndirects\":"<<draw.sortIndirects<<",\"drawIndirects\":"<<draw.drawIndirects
       <<",\"timingSamples\":"<<gpuEffectTimingSamples<<",\"simulateSortGpuMs\":"<<gpuEffectSimulationMs<<",\"drawGpuMs\":"<<gpuEffectDrawMs
       <<",\"includedInFrameGpuTime\":true,\"globalAlphaSorted\":true,\"synchronousParticleReadback\":false,\"lit\":false,\"softParticles\":false"
       <<",\"visualFields\":{\"available\":"<<(gpuEffectFields?"true":"false")<<",\"unified\":true,\"failed\":"<<(visualFields.failed?"true":"false")<<",\"steps\":"<<visualFields.steps<<",\"flushes\":"<<visualFields.flushes<<",\"lastTick\":"<<visualFields.lastTick<<",\"deferredCommits\":"<<visualFields.deferredCommits<<",\"lastDeferral\":"<<detail::jsonString(visualFields.lastDeferral)<<",\"allocationBytes\":"<<visualFields.allocationBytes<<",\"fieldBytes\":"<<visualFields.fieldBytes<<",\"worldBytes\":"<<visualFields.world.accountedBytes<<",\"worldSources\":"<<visualFields.world.sources<<",\"worldPages\":"<<visualFields.world.currentPages<<",\"worldSnapshots\":"<<visualFields.world.snapshots<<",\"worldCommits\":"<<visualFields.world.commits<<",\"readers\":"<<visualFields.readers<<",\"worldAccountedOnce\":true}"
       <<",\"smoke\":{\"available\":"<<(gpuSmokeService?"true":"false")<<",\"frames\":"<<smoke.frames<<",\"steps\":"<<smoke.solverSteps<<",\"tailSteps\":"<<smoke.tailSteps<<",\"skippedSteps\":"<<smoke.skippedSteps<<",\"instances\":"<<smoke.instances<<",\"tails\":"<<smoke.tails<<",\"retiring\":"<<smoke.retiring<<",\"reservations\":"<<smoke.reservations<<",\"reservedBytes\":"<<smoke.reservedBytes<<",\"allocationBytes\":"<<smoke.allocationBytes<<",\"failed\":"<<(smoke.failed?"true":"false")<<",\"mediaFrames\":"<<smokeMediaFrames<<",\"mediaInstances\":"<<smokeMediaInstances<<",\"mediaCount\":"<<smokeScene.info[0]<<",\"compositeMediaPipeline\":"<<(compositeMediaActive?"true":"false")<<",\"captureSources\":"<<capture.str()<<",\"sharedMediaCount\":"<<smokeScene.info[1]<<",\"sharedMediaFrames\":"<<smokeSharedFrames<<",\"sharedMediaInstances\":"<<smokeSharedInstances<<",\"sharing\":{\"available\":"<<(gpuSmokeSharing?"true":"false")<<",\"failed\":"<<(shared.failed?"true":"false")<<",\"instances\":"<<shared.instances<<",\"shared\":"<<shared.sharing.shared<<",\"pending\":"<<shared.pending<<",\"queriesBlocked\":"<<(shared.queriesBlocked?"true":"false")<<",\"imports\":"<<shared.imports<<",\"publications\":"<<shared.publications<<",\"deferredCandidates\":"<<shared.deferredCandidates<<",\"deferredCommits\":"<<shared.deferredCommits<<",\"allocationBytes\":"<<shared.allocationBytes<<",\"worldPages\":"<<shared.world.currentPages<<",\"worldSources\":"<<shared.world.sources<<",\"worldSnapshots\":"<<shared.world.snapshots<<",\"lastDeferral\":"<<detail::jsonString(shared.lastDeferral)<<"},\"sourceReadback\":false,\"lastTick\":"<<smoke.lastTick<<"}"
       <<",\"sharedFieldFrames\":"<<gpuFieldFrames<<",\"sharedFieldEpoch\":"<<gpuFieldEpoch<<",\"fields\":{\"available\":"<<(gpuFieldStorage?"true":"false")<<",\"worldTick\":"<<(configuredGpuFieldWorld?configuredGpuFieldWorld->committedTick():0)<<",\"worldVersion\":"<<(configuredGpuFieldWorld?configuredGpuFieldWorld->version():0)
       <<",\"leases\":"<<field.leases<<",\"snapshots\":"<<field.snapshots<<",\"currentPages\":"<<field.currentPages<<",\"residentPages\":"<<field.residentPages<<",\"descriptorSlots\":"<<field.descriptorSlots
       <<",\"defaultBytes\":"<<field.defaultAllocationBytes<<",\"uploadBytes\":"<<field.uploadAllocationBytes<<",\"metadataBytes\":"<<vectors.allocationBytes<<",\"metadataBindings\":"<<vectors.bindings<<",\"bindingsCreated\":"<<gpuFieldBindings
       <<",\"publishedSnapshots\":"<<field.publishedSnapshots<<",\"rejectedSnapshots\":"<<field.rejectedSnapshots<<",\"rejectedBindings\":"<<vectors.rejectedBindings
       <<",\"cpuPages\":"<<cpu.activePages<<",\"cpuRetiredPages\":"<<cpu.retiredPages<<",\"cpuBytes\":"<<cpu.committedBytes<<",\"cpuLeases\":"<<cpu.leases<<",\"cpuSources\":"<<cpu.sourceRecords<<",\"fieldReadback\":false}"
       <<",\"deposits\":{\"available\":"<<(gpuEffectDeposits?"true":"false")<<",\"failed\":"<<(deposits.failed?"true":"false")<<",\"frames\":"<<deposits.frames<<",\"flushes\":"<<deposits.flushes<<",\"instances\":"<<deposits.instances<<",\"pendingRetirements\":"<<deposits.pendingRetirements
       <<",\"reservations\":"<<deposits.reservations<<",\"reservedMutableBytes\":"<<deposits.reservedMutableBytes<<",\"reservedSamples\":"<<deposits.reservedSamples<<",\"unmaterializedBytes\":"<<deposits.unmaterializedBytes<<",\"admissionRejected\":"<<runtime.fieldAdmissionRejected<<",\"moveRejected\":"<<runtime.fieldMoveRejected<<",\"lastAdmissionError\":"<<detail::jsonString(runtime.fieldAdmissionError)<<",\"deferredWrites\":"<<deposits.deferredWrites<<",\"deferredUpdates\":"<<deposits.deferredUpdates<<",\"rewrites\":"<<deposits.rewrites<<",\"gridRebuilds\":"<<deposits.gridRebuilds<<",\"deferredPromotions\":"<<deposits.deferredPromotions<<",\"allocationBytes\":"<<deposits.allocationBytes<<",\"localBytes\":"<<deposits.local.allocationBytes<<",\"localInstances\":"<<deposits.local.instances<<",\"localRetiredInstances\":"<<deposits.local.retiredInstances<<",\"localProjections\":"<<deposits.local.projections<<",\"localDispatches\":"<<deposits.local.dispatches
       <<",\"publicationBacklog\":"<<backlog.str()<<",\"deferredCandidates\":"<<deposits.deferredCandidates<<",\"pendingPublications\":"<<deposits.pendingPublications<<",\"overduePublications\":"<<deposits.overduePublications<<",\"lastCandidateDeferral\":"<<detail::jsonString(deposits.lastCandidateDeferral)<<",\"deferredCommits\":"<<deposits.deferredCommits<<",\"lastCommitDeferral\":"<<detail::jsonString(deposits.lastCommitDeferral)<<",\"worldBytes\":"<<(deposits.ownsWorld?deposits.world.accountedBytes:0)<<",\"worldPages\":"<<deposits.world.currentPages<<",\"worldSnapshots\":"<<deposits.world.snapshots<<",\"worldSources\":"<<deposits.world.sources<<",\"worldLeases\":"<<deposits.world.leases<<",\"worldCommits\":"<<deposits.world.commits<<",\"worldDispatches\":"<<deposits.world.dispatches<<",\"shared\":"<<deposits.sharing.shared<<",\"runtimeCallbacks\":"<<runtime.fieldDepositCalls<<",\"fieldValueReadback\":false,\"includedInEffectsGpuTime\":true}},";return out.str();
}
}
