#pragma once
#include "../renderer/d3d12_renderer.h"
#include "../engine/native/fields/vector_source.hpp"
#include "../third_party/json.hpp"

namespace host {
// A single authored visual source and read lease shared by registered GPU
// consumers. Samples are explicitly taken at the current render-frame game
// tick, then held for that frame's visual catch-up substeps.
class SimulationFields {
public:
    explicit SimulationFields(const std::string& file);
    std::shared_ptr<engine::fields::World> world() const{return source_.world();}
    bool drive(nativegpu::GpuFieldFrame& frame,engine::fields::Tick tick,std::string& error,bool active=true);
    std::shared_ptr<engine::fields::GpuVectorView> binding() const{return binding_.lock();}
    nlohmann::json report() const;
private:
    engine::fields::VectorSource source_;engine::fields::GpuVectorSpec spec_;engine::fields::GpuLease lease_;
    std::weak_ptr<engine::fields::GpuVectorView> binding_;
    engine::fields::Tick lastTick_=0;std::uint64_t epoch_=0,epochs_=0,bindings_=0;bool started_=false,leased_=false;
};
}
