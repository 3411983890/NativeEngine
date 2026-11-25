#pragma once
#include <cstdint>
namespace nativegpu::detail {
constexpr unsigned MaxSmokeMedia=8;
// 16-byte rows, shared by SM 6.6 froxel and compatibility compositors.
struct SmokeVolume {
    float originCell[4];std::uint32_t dimensions[4],offsets[4];
    float albedoEmission[4],optics[4];
};
struct SmokeScene {std::uint32_t info[4]{};SmokeVolume volumes[MaxSmokeMedia]{};};
struct SmokeObservation {std::uint64_t serial=0,sourceTick=0,version=0;bool sharedProxy=false;std::uint32_t scalarElements=0;float cellMetres=0;};
static_assert(sizeof(SmokeVolume)==80&&sizeof(SmokeScene)==656,"Smoke optical ABI");
}
