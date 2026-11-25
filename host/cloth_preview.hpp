#pragma once
#include "../renderer/d3d12_renderer.h"
#include "../third_party/json.hpp"
#include <filesystem>

namespace host {
struct ClothPreviewOptions {
    std::filesystem::path library="build/cloth/library.json";
    std::string asset;
    unsigned count=1,debugMode=0;
    engine::cloth::GpuExecution execution=engine::cloth::GpuExecution::Automatic;
    bool simulationWind=false;
    engine::Vec3 origin{0,1,0},wind{0,0,4},teleportOrigin{2,1,0};
    std::uint64_t pauseTick=0,resumeTick=0,teleportTick=0,resetTick=0;
};
class ClothPreview {
public:
    explicit ClothPreview(ClothPreviewOptions options);
    std::vector<nativegpu::GpuClothConfig> configuration() const;
    bool drive(nativegpu::GpuClothFrame& frame,std::uint64_t gameTick,std::string& error);
    nlohmann::json report() const;
private:
    ClothPreviewOptions options_;engine::cloth::Library library_;
    std::uint64_t startTick_=0,lastGameTick_=0,epoch_=0,epochs_=0,pausedTicks_=0,teleports_=0,resets_=0;
    bool started_=false,paused_=false;
    engine::Vec3 offset(unsigned i) const;
    bool pausedAt(std::uint64_t age) const;
};
}
