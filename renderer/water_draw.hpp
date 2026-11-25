#pragma once
#include "matrix_math.h"
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nativegpu {
// Shared with the residual solver so the draw pass can derive texel size without a
// second query into the compute pass.
constexpr unsigned WaterRippleResolution = 256;
struct WaterVertex {
    float position[3]{};
    float foam=0;
    float normal[3]{0,1,0};
    float depth=0;
    // Authoritative horizontal velocity snapshot in m/s. The visual residual advects
    // its wave field by this flow; it never integrates momentum of its own.
    float velocity[2]{};
};
static_assert(sizeof(WaterVertex)==40,"Water vertex ABI");
// One authoritative CPU interaction, mirrored to the visual residual solver. `sequence`
// (paired with `timelineEpoch`) is the delivery identity. `tick` may repeat: several
// contacts can happen in one fixed tick, and none of them may be skipped or injected
// twice because of that.
struct WaterRippleEvent {
    float x=0,z=0,radius=0,displacement=0;
    std::uint32_t tick=0;
    float directionX=0,directionZ=0,stretch=1;
    std::uint32_t sequence=0;
    std::uint32_t timelineEpoch=0;
    // Downward relative speed at the contact. The residual uses it to weight the
    // injection shape, not as a second authority.
    float normalSpeed=0;
};
static_assert(sizeof(WaterRippleEvent)==44,"Water ripple event ABI");
struct WaterSurfaceFrame {
    std::vector<WaterVertex> vertices;
    // Ordered by sequence. The residual solver only adds visual detail; it never
    // feeds gameplay, so a dropped event cannot change the CPU water ledger.
    std::vector<WaterRippleEvent> events;
    std::array<float,2> rippleOrigin{0,0};
    std::array<float,2> rippleExtent{1,1};
    unsigned columns=0,rows=0;
    std::uint64_t tick=0;
    // Changes on reset/teleport. The residual drops its old field when this changes,
    // so a rewound timeline cannot mask new contacts as already delivered.
    std::uint32_t timelineEpoch=0;
    float time=0,roughness=.09f,ior=1.333f;
    std::array<float,3> absorption{.65f,.24f,.14f};
    std::array<float,3> scattering{.035f,.12f,.095f};
    unsigned debugMode=0;
};
struct WaterLighting {
    float camera[3]{},keyDirection[3]{0,1,0},keyColor[3]{1,1,1},sky[3]{.3f,.4f,.5f};
    float keyIntensity=1;
};
struct WaterDrawStats {std::uint64_t frames=0,triangles=0,uploadBytes=0;};
// One falling sheet. Positions are world metres; `flow` is the actual transfer rate from
// the ledger and only drives opacity/scroll, never the volume.
struct WaterfallFrame {
    float source[3]{}, landing[3]{};
    float widthMetres = 1.5f, flowCubicMetresPerSecond = 0, time = 0;
};
struct WaterfallDrawStats { std::uint64_t frames = 0, vertices = 0; };
class WaterfallDraw {
public:
    explicit WaterfallDraw(ID3D12Device* device);
    ~WaterfallDraw();
    bool record(ID3D12GraphicsCommandList* list, unsigned slot, const std::vector<WaterfallFrame>& frames,
                const Mat4& viewProjection, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                std::string& error);
    WaterfallDrawStats stats() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
bool validateWaterFrame(const WaterSurfaceFrame& frame,std::string& error);
// Bounded visual residual: a linear wave field over the same footprint as the CPU
// surface. It consumes the CPU event stream and never writes back to it.
class WaterRipple {
public:
    explicit WaterRipple(ID3D12Device* device);
    ~WaterRipple();
    // Leaves `result()` readable by both shader stages. `injected` reports how many
    // events this dispatch added; the caller keeps the authoritative count.
    bool update(ID3D12GraphicsCommandList* list,unsigned slot,const WaterSurfaceFrame& frame,unsigned& injected,std::string& error);
    ID3D12Resource* result() const;
    void reset();
    unsigned resolution() const;
    std::uint64_t dispatches() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
// Inputs are immutable HDR opaque-scene snapshots. The renderer owns barriers
// for the four source resources and binds the destination MRT/depth beforehand.
class WaterDraw {
public:
    WaterDraw(ID3D12Device* device);
    ~WaterDraw();
    bool record(ID3D12GraphicsCommandList* list,unsigned slot,unsigned width,unsigned height,
                ID3D12Resource* base,ID3D12Resource* key,ID3D12Resource* depth,ID3D12Resource* shadow,
                ID3D12Resource* ripple,const WaterSurfaceFrame& frame,const Mat4& vp,const WaterLighting& lighting,std::string& error);
    WaterDrawStats stats() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
