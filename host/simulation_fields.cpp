#include "simulation_fields.hpp"

namespace host {
SimulationFields::SimulationFields(const std::string& file):source_(std::make_shared<engine::fields::World>(256),engine::fields::loadVectorSource(file)){
    if(source_.asset().domain!=engine::fields::Domain::VisualReference||!source_.channels().count("windVelocity"))throw std::invalid_argument("NG-E-FIELD-SOURCE: shared preview requires visual windVelocity; Game owns CPU authority updates");
    spec_.channels=source_.channels();spec_.originMetres=source_.asset().originMetres;spec_.cellMetres=source_.asset().cellMetres;
}
bool SimulationFields::drive(nativegpu::GpuFieldFrame& frame,engine::fields::Tick tick,std::string& error,bool active){
    error.clear();if(!frame.epoch||!frame.acquire||!frame.release||!frame.publish){error="NG-E-FIELD-GPU: incomplete shared field frame";return false;}
    if(started_&&tick<lastTick_){error="NG-E-FIELD-TIME: shared preview game clock moved backwards";return false;}
    if(epoch_!=frame.epoch){binding_.reset();leased_=false;epoch_=frame.epoch;++epochs_;}
    if(!active){
        if(leased_&&!frame.release(lease_)){error="NG-E-FIELD-GPU: shared reader release failed";return false;}
        leased_=false;binding_.reset();frame.wind.reset();
        if(!source_.release()){error="NG-E-FIELD-SOURCE: shared producer release failed";return false;}
        lastTick_=tick;started_=true;return true;
    }
    if(!source_.active()&&source_.world()->version()&&tick<=source_.world()->committedTick()){error="NG-E-FIELD-TIME: reactivation requires a newer game tick";return false;}
    if(!leased_){auto reader=source_.reader();reader.owner="shared-visual-consumers";auto acquired=frame.acquire(std::move(reader));if(!acquired.accepted){error=acquired.error;return false;}lease_=acquired.lease;leased_=true;}
    auto current=binding_.lock();if(current&&current->requestedTick()==tick){frame.wind=std::move(current);return true;}
    if(!source_.stage(tick,error))return false;
    auto world=source_.world();if((!world->version()||world->committedTick()!=tick)&&!world->commit(tick)){error="NG-E-FIELD-SOURCE: shared commit failed";return false;}
    auto next=frame.publish(lease_,spec_,tick);if(!next.accepted()){error=next.error;return false;}
    frame.wind=std::move(next.view);binding_=frame.wind;lastTick_=tick;started_=true;++bindings_;return true;
}
nlohmann::json SimulationFields::report() const{
    const auto state=source_.world()->stats();return {{"requested",true},{"assetHash",source_.asset().hash},{"domain","visual"},{"tick",lastTick_},{"epochs",epochs_},{"leaseActive",leased_},{"bindings",bindings_},{"sourcePublications",source_.publications()},
        {"worldTick",source_.world()->committedTick()},{"worldVersion",source_.world()->version()},{"worldDigest",std::to_string(source_.world()->digest())},{"cpuLeases",state.leases},{"cpuSources",state.sourceRecords},
        {"sampling","latest-render-frame-game-tick-held-for-visual-substeps"},{"readback",false},{"gameAuthorityIntegrated",false}};
}
}
