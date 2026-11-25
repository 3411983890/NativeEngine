#pragma once
#include "rate_view.hpp"
namespace engine::smoke {
template<class T>struct RateCom {T* p=nullptr;RateCom()=default;RateCom(const RateCom&)=delete;RateCom& operator=(const RateCom&)=delete;~RateCom(){reset();}T** out(){return &p;}void reset(){if(p)p->Release();p=nullptr;}};
struct GpuSmokeRateView::Impl {
    RateCom<ID3D12Device> device;RateCom<ID3D12Fence> timeline;
    RateCom<ID3D12Resource> accumulators,rates,diagnostics;
    struct Input {std::array<RateCom<ID3D12Resource>,4> resources;};
    std::vector<std::shared_ptr<Input>> inputs;
    GpuRateBindings binding;std::uint64_t bytes=0,accumulatorBytes=0,producerFence=0,lastFence=0;
    bool ready=false;
    void collect(){if(timeline.p->GetCompletedValue()>=producerFence){inputs.clear();if(accumulators.p){accumulators.reset();bytes-=accumulatorBytes;accumulatorBytes=0;}}}
};
}
