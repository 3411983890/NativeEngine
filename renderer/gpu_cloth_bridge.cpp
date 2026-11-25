#include "d3d12_renderer_internal.h"
#include <atomic>
#include <cmath>
#include <sstream>

namespace nativegpu {
bool Renderer::configureGpuCloth(std::vector<GpuClothConfig> cloths,GpuClothDriver driver,std::string& error){
    error.clear();auto& s=*impl_;
    if(s.initialized||cloths.empty()||cloths.size()>32||!driver){error="NG-E-CLOTH-CONFIG: stopped renderer, 1-32 cloths and frame driver required";return false;}
    try{for(auto& cloth:cloths){
        if(!cloth.asset||cloth.asset->skinned||!validateClothSurface(cloth.surface))throw engine::cloth::Error("NG-E-CLOTH-UNSUPPORTED","Preview requires /2 environment cloth and a valid surface");
        engine::cloth::validateTriangleTopology(*cloth.asset);
        for(float value:{cloth.origin.x,cloth.origin.y,cloth.origin.z,cloth.wind.x,cloth.wind.y,cloth.wind.z})if(!std::isfinite(value))throw engine::cloth::Error("NG-E-CLOTH-CONFIG","Non-finite origin or wind");
        cloth.asset=std::make_shared<const engine::cloth::Asset>(*cloth.asset);
    }}catch(const std::exception& e){error=e.what();return false;}
    s.configuredGpuCloth=std::move(cloths);s.gpuClothDriver=std::move(driver);return true;
}
bool Renderer::Impl::createGpuCloth(std::string& error){
    if(configuredGpuCloth.empty())return true;
    try{
        constexpr std::uint64_t totalBudget=64ull*1024*1024;std::uint64_t bytes=0;
        for(const auto& config:configuredGpuCloth){engine::cloth::GpuParameters parameters;parameters.origin=config.origin;parameters.wind=config.wind;parameters.execution=config.execution;parameters.reserveWindField=config.simulationWind;
            engine::cloth::GpuLimits limits;limits.maxAllocationBytes=totalBudget-bytes;
            auto solver=std::make_unique<engine::cloth::GpuSolver>(device,fence,config.asset,limits,parameters);bytes+=solver->stats().allocationBytes;gpuCloth.push_back(std::move(solver));}
        bool grouped=false;for(const auto& solver:gpuCloth)grouped|=solver->stats().grouped;
        if(grouped){engine::cloth::GpuBatchLimits limits;limits.maxInstances=static_cast<unsigned>(gpuCloth.size());limits.maxAllocationBytes=totalBudget-bytes;gpuClothBatch=std::make_unique<engine::cloth::GpuBatch>(device,fence,limits);}
        gpuClothDraw=std::make_unique<GpuClothDraw>(device,fence,detail::SceneColorFormat,detail::SceneColorFormat,detail::DepthDsvFormat);
        static std::atomic<std::uint64_t> epochs{0};gpuClothEpoch=++epochs;return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool Renderer::Impl::recordGpuCloth(ID3D12GraphicsCommandList* list,std::string& error){
    if(gpuCloth.empty())return true;
    try{
        GpuClothFrame frame;frame.epoch=gpuClothEpoch;
        std::optional<engine::fields::Tick> fieldTick;if(gpuSimulationWind)fieldTick=gpuSimulationWind->requestedTick();
        for(unsigned i=0;i<gpuCloth.size();++i)if(configuredGpuCloth[i].simulationWind){if(gpuSimulationWind){if(!gpuCloth[i]->bindWindField(gpuSimulationWind,error))return false;}else if(!gpuCloth[i]->clearWindField(error))return false;}
        for(auto& solver:gpuCloth){if(!solver->stats().initialized&&!solver->initialize(list,nextFence,error))return false;frame.solvers.push_back(solver.get());}
        frame.step=[&](unsigned i,float dt,std::string& message){if(i>=gpuCloth.size()){message="NG-E-CLOTH-STATE: invalid instance";return false;}return gpuCloth[i]->step(list,nextFence,dt,message,configuredGpuCloth[i].simulationWind?fieldTick:std::optional<engine::fields::Tick>{});};
        if(gpuClothBatch)frame.stepAll=[&](float dt,std::string& message){std::vector<engine::cloth::GpuBatchStep> requests;requests.reserve(gpuCloth.size());for(auto& solver:gpuCloth)requests.push_back({solver.get(),dt,fieldTick});return gpuClothBatch->step(list,nextFence,requests,message);};
        frame.teleport=[&](unsigned i,engine::Vec3 origin,std::string& message){if(i>=gpuCloth.size()){message="NG-E-CLOTH-STATE: invalid instance";return false;}return gpuCloth[i]->teleport(list,nextFence,origin,message);};
        frame.reset=[&](unsigned i,std::string& message){if(i>=gpuCloth.size()){message="NG-E-CLOTH-STATE: invalid instance";return false;}return gpuCloth[i]->reset(list,nextFence,message);};
        return gpuClothDriver(frame,error);
    }catch(const std::exception& e){error=e.what();return false;}
}
bool Renderer::Impl::drawGpuCloth(ID3D12GraphicsCommandList* list,const Mat4& vp,const float camera[3],std::string& error){
    if(!gpuClothDraw)return true;
    ClothLighting lighting;lighting.camera={camera[0],camera[1],camera[2]};lighting.keyDirection={environment.keyDirection[0],environment.keyDirection[1],environment.keyDirection[2]};lighting.keyIntensity=environment.keyIntensity;
    for(unsigned i=0;i<3;++i){lighting.ambient[i]=environment.ambient[i];lighting.keyColor[i]=environment.keyColor[i];}
    for(unsigned i=0;i<gpuCloth.size();++i)if(!gpuClothDraw->draw(list,nextFence,*gpuCloth[i],vp,configuredGpuCloth[i].surface,lighting,error))return false;
    ++gpuClothFrames;return true;
}
void Renderer::Impl::releaseGpuCloth(){gpuClothDraw.reset();gpuClothBatch.reset();gpuCloth.clear();}
std::string Renderer::Impl::gpuClothJson() const{
    std::uint64_t bytes=0,dispatches=0,tick=0;unsigned grouped=0;for(const auto& solver:gpuCloth){const auto stats=solver->stats();bytes+=stats.allocationBytes;dispatches+=stats.dispatches;tick=std::max(tick,stats.tick);grouped+=stats.grouped?1:0;}
    const auto draw=gpuClothDraw?gpuClothDraw->stats():ClothDrawStats{};
    const auto batch=gpuClothBatch?gpuClothBatch->stats():engine::cloth::GpuBatchStats{};dispatches+=batch.dispatches;
    std::ostringstream out;out<<"\"gpuCloth\":{\"available\":"<<(gpuClothDraw?"true":"false")<<",\"instances\":"<<gpuCloth.size()<<",\"epoch\":"<<gpuClothEpoch<<",\"tick\":"<<tick<<",\"frames\":"<<gpuClothFrames
        <<",\"sharedWindBound\":"<<(gpuSimulationWind?"true":"false")<<",\"sharedWindTick\":"<<(gpuSimulationWind?gpuSimulationWind->requestedTick():0)<<",\"simulationBytes\":"<<bytes<<",\"bufferBudgetBytes\":67108864,\"dispatches\":"<<dispatches<<",\"draws\":"<<draw.draws<<",\"triangles\":"<<draw.triangles
        <<",\"batchAvailable\":"<<(gpuClothBatch?"true":"false")<<",\"batchDispatches\":"<<batch.dispatches<<",\"batchedSteps\":"<<batch.batchedSteps<<",\"batchFallbackSteps\":"<<batch.fallbackSteps<<",\"batchAccountedBytes\":"<<batch.accountedBytes<<",\"totalAccountedBytes\":"<<(bytes+batch.accountedBytes)
        <<",\"batchMetadataAllocationBytes\":"<<batch.metadataAllocationBytes<<",\"batchDescriptorEstimateBytes\":"<<batch.descriptorEstimateBytes<<",\"batchDescriptorSlots\":"<<batch.descriptorSlots<<",\"batchActiveSlots\":"<<batch.activeSlots<<",\"batchHeldResourceReferences\":"<<batch.heldResourceReferences
        <<",\"groupedInstances\":"<<grouped<<",\"dispatchInstances\":"<<(gpuCloth.size()-grouped)<<",\"groupThreads\":"<<(grouped?256:0)<<",\"sharedBytesPerGroup\":"<<(grouped?30720:0)
        <<",\"timingSamples\":"<<gpuClothTimingSamples<<",\"simulationGpuMs\":"<<gpuClothSimulationMs<<",\"drawGpuMs\":"<<gpuClothDrawMs
        <<",\"includedInFrameGpuTime\":true,\"synchronousVertexReadback\":false,\"cpuVertexUpload\":false,\"twoSided\":true,\"gridUV\":true,\"lighting\":\"ambient-and-key-charlie-sheen\",\"localLights\":false,\"shadowCaster\":false,\"skinned\":false},";return out.str();
}
}
