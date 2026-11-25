#include "d3d12_renderer_internal.h"
#include <atomic>

namespace nativegpu {
bool Renderer::configureSimulationFields(std::shared_ptr<engine::fields::World> world,engine::fields::GpuLimits storageLimits,
                                        engine::fields::GpuVectorLimits vectorLimits,GpuFieldDriver driver,std::string& error){
    if(!driver){error="NG-E-FIELD-GPU-STATE: missing simulation field driver";return false;}
    if(!configureGpuEffectFields(std::move(world),storageLimits,vectorLimits,error))return false;
    impl_->gpuFieldDriver=std::move(driver);return true;
}
bool Renderer::Impl::createGpuFields(std::string& error){
    if(!configuredGpuFieldWorld)return true;
    try{gpuFieldStorage=std::make_shared<engine::fields::GpuFieldStorage>(configuredGpuFieldWorld,device,configuredGpuFieldLimits);
        gpuVectorFields=std::make_unique<engine::fields::GpuVectorFields>(gpuFieldStorage,configuredGpuVectorLimits);
        static std::atomic<std::uint64_t> epochs{0};gpuFieldEpoch=++epochs;return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
GpuFieldFrame Renderer::Impl::fieldFrame(ID3D12GraphicsCommandList* list){
    GpuFieldFrame frame;frame.epoch=gpuFieldEpoch;frame.effectEpoch=gpuEffectEpoch;
    frame.acquire=[this](engine::fields::LeaseRequest request){return gpuFieldStorage?gpuFieldStorage->acquire(std::move(request)):engine::fields::GpuAcquisition{false,{},"NG-E-FIELD-GPU: World not configured"};};
    frame.release=[this](engine::fields::GpuLease lease){return gpuFieldStorage&&gpuFieldStorage->release(lease,nextFence);};
    frame.publish=[this,list](engine::fields::GpuLease lease,const engine::fields::GpuVectorSpec& spec,engine::fields::Tick tick){
        if(!gpuVectorFields||tick!=configuredGpuFieldWorld->committedTick())return engine::fields::GpuVectorResult{{},"NG-E-FIELD-GPU: committed field tick mismatch"};
        std::string message;if(!gpuFieldStorage->publish(list,nextFence,message))return engine::fields::GpuVectorResult{{},message};
        auto result=gpuVectorFields->bind(lease,spec,tick);if(result.accepted())++gpuFieldBindings;return result;
    };return frame;
}
bool Renderer::Impl::recordGpuFields(ID3D12GraphicsCommandList* list,std::string& error){
    if(!gpuFieldStorage)return true;
    try{const auto completed=fence->GetCompletedValue();gpuVectorFields->collect(completed);configuredGpuFieldWorld->collect(completed);
        if(gpuFieldDriver){auto frame=fieldFrame(list);if(!gpuFieldDriver(frame,error))return false;
            if(frame.wind&&(frame.wind->device()!=device||!frame.wind->supports("windVelocity")||!frame.wind->canUse(nextFence))){error="NG-E-FIELD-GPU: invalid shared wind binding";return false;}
            gpuSimulationWind=std::move(frame.wind);++gpuFieldFrames;
        }return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
void Renderer::Impl::releaseGpuFields(){
    gpuSimulationWind.reset();gpuVectorFields.reset();gpuFieldStorage.reset();
    if(configuredGpuFieldWorld&&fence)configuredGpuFieldWorld->collect(fence->GetCompletedValue());
}
}
