#pragma once
#include "matrix_math.h"
#include "../engine/native/effects/gpu_runtime.hpp"
#include <map>

namespace nativegpu {
struct GpuSpriteLimits {unsigned capacity=262144;std::uint64_t maxAllocationBytes=32ull*1024*1024;};
struct GpuSpriteStats {
    std::uint64_t allocationBytes=0,lastFence=0,frames=0,gatherIndirects=0,sortIndirects=0,drawIndirects=0;
};
struct GpuSpriteBuffers {ID3D12Resource* records=nullptr;ID3D12Resource* counters=nullptr;ID3D12Resource* draws=nullptr;unsigned capacity=0;};
// GPU gather, global view-depth sort and three blend-mode indirect draws. The
// caller binds its shader-visible texture heap, viewport and two RTVs plus DSV.
// Texture indices address that bound heap. Input pools must be prepareDraw-ready.
// No CPU particle readback/upload, waits or per-particle descriptor allocation.
class GpuEffectSprites {
public:
    GpuEffectSprites(ID3D12Device* device,DXGI_FORMAT color,DXGI_FORMAT keyLight,DXGI_FORMAT depth,GpuSpriteLimits limits={});
    ~GpuEffectSprites();
    GpuEffectSprites(const GpuEffectSprites&)=delete;
    GpuEffectSprites& operator=(const GpuEffectSprites&)=delete;
    bool prepare(ID3D12GraphicsCommandList* list,std::uint64_t fence,const Mat4& viewProjection,
                 const std::vector<engine::effects::GpuDrawBatch>& batches,
                 const std::map<std::string,unsigned>& textures,std::string& error);
    bool draw(ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error);
    GpuSpriteBuffers buffers() const;
    GpuSpriteStats stats() const;
    bool shutdown(std::uint64_t completedFence,std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
