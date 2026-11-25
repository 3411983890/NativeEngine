#pragma once
#include "configuration.hpp"
#include <algorithm>
#include <string>
namespace engine::smoke {
struct EffectSource {std::uint32_t emitter=0;std::array<std::uint32_t,3> quantum{};};
struct Optics {
    float extinctionPerDensity=1,emissionPerDensity=.05f,emissionStartKelvin=700;
    Vec3 scatteringAlbedo{.65f,.65f,.65f};
};
struct EffectConfig {
    Spec solver;Vec3 originOffset{};
    std::array<std::uint32_t,3> unitsPerSource{1000000,1000,1000000};
    unsigned tailTicks=240;std::vector<EffectSource> sources;
    Optics optics;
};
// Pure validation shared by authenticated parsing and native programmatic assets.
inline std::string effectConfigurationError(const EffectConfig& c,unsigned emitters,unsigned capacity){
    try{configurationLayout(c.solver);}catch(const std::exception& e){return e.what();}
    if(c.solver.originMetres.x!=0||c.solver.originMetres.y!=0||c.solver.originMetres.z!=0)return "Smoke asset uses originOffset; solver origin must be zero";
    const auto& o=c.optics;if(!std::isfinite(o.extinctionPerDensity)||o.extinctionPerDensity<0||o.extinctionPerDensity>1000||!std::isfinite(o.emissionPerDensity)||o.emissionPerDensity<0||o.emissionPerDensity>1000||!std::isfinite(o.emissionStartKelvin)||o.emissionStartKelvin<100||o.emissionStartKelvin>10000)return "Invalid smoke optical coefficients";
    for(float a:{o.scatteringAlbedo.x,o.scatteringAlbedo.y,o.scatteringAlbedo.z})if(!std::isfinite(a)||a<0||a>1)return "Invalid smoke scattering albedo";
    if(!capacity||capacity>1000000||c.tailTicks>432000||c.sources.empty()||c.sources.size()>32)return "Invalid smoke source/tail capacity";
    if(!std::isfinite(c.originOffset.x)||!std::isfinite(c.originOffset.y)||!std::isfinite(c.originOffset.z)||std::abs(c.originOffset.x)>100000||std::abs(c.originOffset.y)>100000||std::abs(c.originOffset.z)>100000)return "Invalid smoke grid offset";
    std::array<std::uint64_t,3> totals{};std::vector<unsigned> seen;
    for(auto units:c.unitsPerSource)if(!units||units>1000000000)return "Invalid smoke source quantization";
    for(const auto& source:c.sources){if(source.emitter>=emitters||std::find(seen.begin(),seen.end(),source.emitter)!=seen.end())return "Unknown or duplicate smoke emitter";seen.push_back(source.emitter);bool nonzero=false;for(unsigned a=0;a<3;++a){const auto q=source.quantum[a];nonzero|=q!=0;if(q&&!(c.solver.channels&(1u<<a)))return "Smoke source requires an enabled channel";const auto amount=std::uint64_t(q)*capacity;if(amount>UINT32_MAX||totals[a]>UINT32_MAX-amount)return "Smoke source uint32 bound";totals[a]+=amount;}if(!nonzero)return "Smoke source quantizes to zero";}
    const double volume=std::pow(double(c.solver.cellMetres),3);for(unsigned a=0;a<3;++a)if(totals[a]/(c.unitsPerSource[a]*volume)>1000000.0)return "Smoke source per-cell rate bound";
    const double count=std::ceil(double(1.f/120)*c.solver.maxSpeed/(c.solver.cellMetres*c.solver.courant));if(count>c.solver.maxSubsteps)return "Smoke fixed tick exceeds CFL substep budget";
    return {};
}
}
