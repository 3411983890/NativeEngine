#pragma once
#include "../renderer/d3d12_renderer.h"
#include "../engine/native/fields/vector_source.hpp"

namespace host {
// Optional authored visual field driver. The monotonically increasing field
// clock is explicit, survives renderer recreation, and never reads GPU values.
class EffectFields {
public:
    explicit EffectFields(const std::string& file);
    std::shared_ptr<engine::fields::World> world() const{return source_.world();}
    bool activate(nativegpu::GpuEffectFrame& frame,std::string& error);
    bool step(nativegpu::GpuEffectFrame& frame,const std::vector<engine::effects::Handle>& handles,std::string& error);
    bool release(nativegpu::GpuEffectFrame& frame,std::string& error);
    std::shared_ptr<engine::fields::GpuVectorView> binding() const{return binding_.lock();}
    const engine::fields::VectorSource& source() const{return source_;}
    engine::fields::Tick tick() const{return tick_;}
    bool leased() const{return leased_;}
    std::uint64_t epochs() const{return epochs_;}
private:
    bool publish(nativegpu::GpuEffectFrame& frame,engine::fields::Tick tick,std::string& error);
    engine::fields::VectorSource source_;
    engine::fields::GpuVectorSpec spec_;engine::fields::GpuLease lease_;
    std::weak_ptr<engine::fields::GpuVectorView> binding_;
    std::uint64_t epoch_=0,epochs_=0;engine::fields::Tick tick_=0;bool leased_=false;
};
}
