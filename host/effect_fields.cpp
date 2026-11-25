#include "effect_fields.hpp"
#include <limits>
#include <stdexcept>

namespace host {
EffectFields::EffectFields(const std::string& file):source_(std::make_shared<engine::fields::World>(256),engine::fields::loadVectorSource(file)){
    if(source_.asset().domain!=engine::fields::Domain::VisualReference)throw std::invalid_argument("NG-E-FIELD-SOURCE: preview field tracks must be visual; Game owns CPU authority updates");
    spec_.channels=source_.channels();spec_.originMetres=source_.asset().originMetres;spec_.cellMetres=source_.asset().cellMetres;
}
bool EffectFields::publish(nativegpu::GpuEffectFrame& frame,engine::fields::Tick tick,std::string& error){
    if(!source_.stage(tick,error))return false;auto world=source_.world();if((!world->version()||world->committedTick()!=tick)&&!world->commit(tick)){error="NG-E-FIELD-SOURCE: commit failed";return false;}
    auto next=frame.publishField(lease_,spec_,tick);if(!next.accepted()){error=next.error;return false;}binding_=std::move(next.view);return true;
}
bool EffectFields::activate(nativegpu::GpuEffectFrame& frame,std::string& error){
    error.clear();if(!frame.acquireField||!frame.releaseField||!frame.publishField||!frame.stepField){error="NG-E-FIELD-GPU: frame does not support fields";return false;}
    if(epoch_!=frame.epoch){binding_.reset();leased_=false;epoch_=frame.epoch;++epochs_;}
    if(!leased_){auto acquired=frame.acquireField(source_.reader());if(!acquired.accepted){error=acquired.error;return false;}lease_=acquired.lease;leased_=true;}
    auto current=binding_.lock();if(current&&current->requestedTick()==tick_)return true;
    auto requested=tick_;if(!source_.active()&&source_.world()->version()&&requested<=source_.world()->committedTick()){
        if(source_.world()->committedTick()==std::numeric_limits<engine::fields::Tick>::max()){error="NG-E-FIELD-SOURCE: activation clock overflow";return false;}
        // A released contribution cannot amend an already committed tick. This
        // visual reactivation is an explicit new field tick, including on seek.
        requested=source_.world()->committedTick()+1;
    }
    if(!publish(frame,requested,error))return false;tick_=requested;return true;
}
bool EffectFields::step(nativegpu::GpuEffectFrame& frame,const std::vector<engine::effects::Handle>& handles,std::string& error){
    bool active=false;for(auto h:handles)active|=frame.runtime.valid(h);
    if(active&&!activate(frame,error))return false;
    if(tick_==std::numeric_limits<engine::fields::Tick>::max()){error="NG-E-FIELD-SOURCE: field time overflow";return false;}const auto next=tick_+1;
    if(active){if(!publish(frame,next,error))return false;for(auto h:handles)if(frame.runtime.valid(h)&&!frame.runtime.bindField(h,binding(),error))return false;if(!frame.stepField(next,error))return false;}
    else{if(!release(frame,error)||!frame.step(error))return false;}
    tick_=next;return true;
}
bool EffectFields::release(nativegpu::GpuEffectFrame& frame,std::string& error){
    error.clear();if(leased_&&!frame.releaseField(lease_)){error="NG-E-FIELD-GPU: spatial lease release failed";return false;}binding_.reset();leased_=false;if(!source_.release()){error="NG-E-FIELD-SOURCE: writer release failed";return false;}return true;
}
}
