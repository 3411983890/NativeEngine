#pragma once
#include "../vec3.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace engine::smoke {
enum Channel:std::uint32_t {Density=1,Temperature=2,Fuel=4};
struct Spec {
    std::array<unsigned,3> dimensions{16,16,16};
    Vec3 originMetres{};
    std::uint32_t channels=Density|Temperature|Fuel;
    float cellMetres=.125f,ambientKelvin=293.15f,maxSpeed=8,courant=.5f;
    float densityDecay=.05f,cooling=.4f,fuelDecay=0,burnRate=2,ignitionKelvin=700;
    // Artist model: K per kg/m^3 fuel consumed, not combustion chemistry.
    float heatPerFuel=800,sootPerFuel=.3f,maxDensity=100,maxTemperature=4000,maxFuel=100;
    float buoyancy=.01f,smokeWeight=.1f,vorticity=.2f,pressureRelaxation=2.f/3;
    unsigned pressureIterations=48,maxSubsteps=16;
    bool macCormack=true;
};
struct CellInput {
    float densityRate=0,temperatureRate=0,fuelRate=0;
    Vec3 acceleration{},wallVelocity{};bool solid=false;
};
struct Layout {
    std::uint32_t cells=0,scalarCount=0;
    std::array<std::uint32_t,3> offsets{UINT32_MAX,UINT32_MAX,UINT32_MAX};
};
struct State {
    // Enabled channels only, SoA. Positive staggered face velocities in m/s;
    // negative outer faces are closed stationary walls, stored implicitly.
    std::vector<float> scalars;std::vector<Vec3> velocity;
};
struct Diagnostics {
    double divergenceBeforeL2=0,divergenceAfterL2=0,pressureResidualL2=0;
    double densityIntegral=0,fuelIntegral=0;
    float maximumSpeed=0;std::uint64_t clampedValues=0;unsigned substeps=0;
};
Layout layout(const Spec&);
unsigned substeps(const Spec&,float dt);
State initialState(const Spec&);
void validateInputs(const Spec&,const std::vector<CellInput>&);
void validateState(const Spec&,const State&);
// CPU visual reference. No CPU-authority World mutation. Validation and the
// bounded CFL schedule precede all mutation; inputs are rates per second.
Diagnostics step(const Spec&,const std::vector<CellInput>&,State&,float dt);
}
