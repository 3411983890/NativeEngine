#pragma once
#include "solver.hpp"
#include <cmath>
#include <stdexcept>
namespace engine::smoke {
inline Layout configurationLayout(const Spec& s){
    if((!std::isfinite(s.originMetres.x)||!std::isfinite(s.originMetres.y)||!std::isfinite(s.originMetres.z))||std::abs(s.originMetres.x)>1000000||std::abs(s.originMetres.y)>1000000||std::abs(s.originMetres.z)>1000000)throw std::invalid_argument("NG-E-SMOKE: invalid metre origin");
    if(!s.channels||(s.channels&~7u)||!std::isfinite(s.cellMetres)||s.cellMetres<.01f||s.cellMetres>10||!std::isfinite(s.ambientKelvin)||s.ambientKelvin<100||s.ambientKelvin>1000||!std::isfinite(s.maxSpeed)||s.maxSpeed<=0||s.maxSpeed>100||!std::isfinite(s.courant)||s.courant<=0||s.courant>.5f||!s.pressureIterations||s.pressureIterations>256||!s.maxSubsteps||s.maxSubsteps>64)throw std::invalid_argument("NG-E-SMOKE: invalid grid/schedule");
    std::uint64_t n=1;for(auto d:s.dimensions){if(d<2||d>128)throw std::invalid_argument("NG-E-SMOKE: invalid dimensions");n*=d;}
    const float nonnegative[]={s.densityDecay,s.cooling,s.fuelDecay,s.burnRate,s.heatPerFuel,s.sootPerFuel,s.buoyancy,s.smokeWeight,s.vorticity};for(float v:nonnegative)if(!std::isfinite(v)||v<0||v>10000)throw std::invalid_argument("NG-E-SMOKE: invalid model rate");
    if(!std::isfinite(s.maxDensity)||s.maxDensity<=0||s.maxDensity>10000||!std::isfinite(s.maxFuel)||s.maxFuel<=0||s.maxFuel>10000||!std::isfinite(s.maxTemperature)||s.maxTemperature<=s.ambientKelvin||s.maxTemperature>10000||!std::isfinite(s.ignitionKelvin)||s.ignitionKelvin<s.ambientKelvin||s.ignitionKelvin>s.maxTemperature||!std::isfinite(s.pressureRelaxation)||s.pressureRelaxation<=0||s.pressureRelaxation>1)throw std::invalid_argument("NG-E-SMOKE: invalid bounds/pressure");
    Layout l;l.cells=static_cast<unsigned>(n);for(unsigned a=0;a<3;++a)if(s.channels&(1u<<a)){l.offsets[a]=l.scalarCount;l.scalarCount+=l.cells;}return l;
}
}
