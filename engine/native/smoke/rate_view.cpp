#include "rate_view_internal.hpp"
namespace engine::smoke {
GpuSmokeRateView::GpuSmokeRateView()=default;
GpuSmokeRateView::~GpuSmokeRateView()=default;
GpuRateBindings GpuSmokeRateView::bindings() const{return impl_&&impl_->ready?impl_->binding:GpuRateBindings{};}
ID3D12Device* GpuSmokeRateView::device() const{return impl_?impl_->device.p:nullptr;}
ID3D12Fence* GpuSmokeRateView::timeline() const{return impl_?impl_->timeline.p:nullptr;}
bool GpuSmokeRateView::markUsed(std::uint64_t fence){if(!impl_||!impl_->ready||!fence||fence<impl_->lastFence||fence<impl_->producerFence||fence<=impl_->timeline.p->GetCompletedValue())return false;impl_->lastFence=fence;return true;}
std::uint64_t GpuSmokeRateView::lastFence() const{return impl_?impl_->lastFence:0;}
}
