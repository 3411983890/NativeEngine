#include "cloth_preview.hpp"
#include <cmath>

namespace host {
ClothPreview::ClothPreview(ClothPreviewOptions options):options_(std::move(options)){
    if(options_.asset.empty()||!options_.count||options_.count>16||options_.debugMode>3||
       (options_.resumeTick&&(!options_.pauseTick||options_.resumeTick<=options_.pauseTick)))throw engine::cloth::Error("NG-E-CLOTH-CONFIG","Invalid cloth preview count, mode or pause/resume interval");
    for(auto v:{options_.origin,options_.teleportOrigin,options_.wind})for(float component:{v.x,v.y,v.z})if(!std::isfinite(component))throw engine::cloth::Error("NG-E-CLOTH-CONFIG","Cloth vectors must be finite");
    for(auto v:{options_.origin,options_.teleportOrigin})if(std::fabs(v.x)>1e6f||std::fabs(v.y)>1e6f||std::fabs(v.z)>1e6f)throw engine::cloth::Error("NG-E-CLOTH-CONFIG","Cloth origin exceeds the metre mapping bound");
    if(std::fabs(options_.wind.x)>10000||std::fabs(options_.wind.y)>10000||std::fabs(options_.wind.z)>10000)throw engine::cloth::Error("NG-E-CLOTH-CONFIG","Cloth wind exceeds the supported velocity bound");
    library_=engine::cloth::loadClothLibrary(options_.library.u8string(),"assets");
    const auto found=library_.cloths.find(options_.asset);
    if(found==library_.cloths.end())throw engine::cloth::Error("NG-E-CLOTH-ASSET","Requested cloth is not in the cooked library");
    if(!found->second->triangleModel||found->second->skinned)throw engine::cloth::Error("NG-E-CLOTH-UNSUPPORTED","GPU preview requires /2 environment cloth; bones are not integrated");
}
engine::Vec3 ClothPreview::offset(unsigned i) const{
    const auto& asset=*library_.cloths.at(options_.asset);const float width=asset.rest[asset.columns-1].x-asset.rest.front().x;
    return {static_cast<float>(i)*(width+.3f),0,0};
}
bool ClothPreview::pausedAt(std::uint64_t age) const{return options_.pauseTick&&age>=options_.pauseTick&&(!options_.resumeTick||age<options_.resumeTick);}
std::vector<nativegpu::GpuClothConfig> ClothPreview::configuration() const{
    std::vector<nativegpu::GpuClothConfig> result;
    for(unsigned i=0;i<options_.count;++i){nativegpu::GpuClothConfig c;c.asset=library_.cloths.at(options_.asset);c.origin=options_.origin+offset(i);c.wind=options_.wind;c.surface.debugMode=options_.debugMode;c.execution=options_.execution;c.simulationWind=options_.simulationWind;result.push_back(std::move(c));}return result;
}
bool ClothPreview::drive(nativegpu::GpuClothFrame& frame,std::uint64_t gameTick,std::string& error){
    error.clear();if(frame.solvers.size()!=options_.count||!frame.step||!frame.teleport||!frame.reset||!frame.epoch){error="NG-E-CLOTH-STATE: incomplete renderer frame";return false;}
    if(!started_){started_=true;startTick_=lastGameTick_=gameTick;}
    if(gameTick<lastGameTick_){error="NG-E-CLOTH-TIME: preview game clock moved backwards";return false;}
    if(epoch_!=frame.epoch){
        // Device recovery resets visual deformation, while events retain the
        // independent game age. No GPU snapshot enters a save/gameplay digest.
        epoch_=frame.epoch;++epochs_;lastGameTick_=gameTick;const auto age=gameTick-startTick_;paused_=pausedAt(age);
        for(unsigned i=0;i<frame.solvers.size();++i){frame.solvers[i]->setPaused(paused_);if(options_.teleportTick&&age>=options_.teleportTick&&!frame.teleport(i,options_.teleportOrigin+offset(i),error))return false;}
    }
    while(lastGameTick_<gameTick){
        ++lastGameTick_;const auto age=lastGameTick_-startTick_;paused_=pausedAt(age);
        for(unsigned i=0;i<frame.solvers.size();++i){
            frame.solvers[i]->setPaused(paused_);
            if(options_.teleportTick&&age==options_.teleportTick&&!frame.teleport(i,options_.teleportOrigin+offset(i),error))return false;
            if(options_.resetTick&&age==options_.resetTick&&!frame.reset(i,error))return false;
        }
        if(frame.stepAll){if(!frame.stepAll(1.f/120,error))return false;}
        else for(unsigned i=0;i<frame.solvers.size();++i)if(!frame.step(i,1.f/120,error))return false;
        if(paused_)++pausedTicks_;if(options_.teleportTick&&age==options_.teleportTick)++teleports_;if(options_.resetTick&&age==options_.resetTick)++resets_;
    }
    return true;
}
nlohmann::json ClothPreview::report() const{
    return {{"requested",true},{"asset",options_.asset},{"libraryHash",library_.hash},{"assetHash",library_.cloths.at(options_.asset)->hash},{"instances",options_.count},{"backend","gpu-visual"},
        {"startGameTick",startTick_},{"lastGameTick",lastGameTick_},{"gameAge",lastGameTick_-startTick_},{"epochs",epochs_},{"pausedGameTicks",pausedTicks_},{"paused",paused_},{"teleports",teleports_},{"resets",resets_},
        {"execution",options_.execution==engine::cloth::GpuExecution::Automatic?"auto":options_.execution==engine::cloth::GpuExecution::Dispatch?"dispatch":"grouped"},{"recovery","restart visual rest state at current origin; retain game age and pause interval"},{"gameAuthorityIntegrated",false},{"saveDeformation",false},{"sharedWindSource",options_.simulationWind}};
}
}
