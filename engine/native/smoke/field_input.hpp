#pragma once
#include "gpu.hpp"
#include "../fields/gpu_local.hpp"

namespace engine::smoke {
// Synchronous borrowed adapter; the solver must outlive replaceScalar().
// Density is concentration (kg/m^3), not particle source rate. Temperature
// needs a heat-capacity merge contract and is deliberately not a Sum channel.
inline fields::GpuScalarInput densityFieldInput(GpuSmokeFireSolver& solver){
    const auto b=solver.buffers();const auto stats=solver.stats();fields::GpuScalarInput in;
    if(!stats.initialized||stats.failed||!(b.spec.channels&Density))return in;
    in.values=b.scalars;in.timeline=solver.timeline();in.offset=b.layout.offsets[0];
    in.dimensions={static_cast<int>(b.spec.dimensions[0]),static_cast<int>(b.spec.dimensions[1]),static_cast<int>(b.spec.dimensions[2])};
    in.originMetres=b.spec.originMetres;in.cellMetres=b.spec.cellMetres;in.maximum=b.spec.maxDensity;
    in.sourceTick=b.sourceTick;in.version=b.version;in.lastFence=stats.lastFence;
    // No persistent resource retention before admission: stale/closed producer
    // descriptors are rejected by CPU metadata before dereferencing the buffer.
    in.current=[&solver,resource=b.scalars,tick=b.sourceTick,version=b.version]{
        const auto current=solver.buffers();
        return current.scalars==resource&&current.sourceTick==tick&&current.version==version;
    };
    in.prepareRead=[&solver,current=in.current](ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
        if(!current()){error="NG-E-SMOKE-FIELD: borrowed state changed before import";return false;}
        return solver.prepareRead(list,fence,error);
    };
    return in;
}
}
