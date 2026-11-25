#pragma once
#include "matrix_math.h"
#include "../engine/native/cloth/solver_gpu.hpp"

namespace nativegpu {
struct ClothSurface {
    std::array<float,3> albedo{.55f,.08f,.035f};
    float roughnessWarp=.55f,roughnessWeft=.75f,sheen=.2f,weaveStrength=.06f;
    std::array<float,2> weaveRepeat{48,48};
    // 0 cloth lighting, 1 oriented world normal, 2 grid UV, 3 tangent.
    unsigned debugMode=0;
};
struct ClothLighting {
    engine::Vec3 camera{0,0,5},keyDirection{0,0,1};
    std::array<float,3> ambient{.25f,.25f,.25f},keyColor{1,1,1};
    float keyIntensity=1;
};
struct ClothDrawStats {std::uint64_t draws=0,triangles=0,lastFence=0;};
bool validateClothSurface(const ClothSurface& surface);
// Opaque, two-sided MRT rendering directly from the simulation buffers. No CPU
// vertex copy, transient upload, descriptor heap, queue wait or state readback.
// Caller binds two RTVs, a depth DSV, viewport/scissor on this actual timeline.
class GpuClothDraw {
public:
    GpuClothDraw(ID3D12Device* device,ID3D12Fence* timeline,DXGI_FORMAT surface,DXGI_FORMAT key,DXGI_FORMAT depth);
    ~GpuClothDraw();
    GpuClothDraw(const GpuClothDraw&)=delete;
    bool draw(ID3D12GraphicsCommandList* list,std::uint64_t fence,engine::cloth::GpuSolver& solver,
              const Mat4& viewProjection,const ClothSurface& surface,const ClothLighting& lighting,std::string& error);
    ClothDrawStats stats() const;
    bool shutdown(std::string& error);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
