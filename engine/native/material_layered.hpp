#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

// CPU reference for native material-layer evaluation. Keep heightCoverage in
// lockstep with engine/material/layered.mjs and shaders/material_layer_blend.hlsli.
namespace engine::material::layered {
inline float heightCoverage(float coverage,float bottomHeight,float topHeight,float width,float bias=0) {
    if(!std::isfinite(coverage)||coverage<0||coverage>1||!std::isfinite(bottomHeight)||std::abs(bottomHeight)>1000000||
       !std::isfinite(topHeight)||std::abs(topHeight)>1000000||!std::isfinite(width)||width<.0001f||width>1000||
       !std::isfinite(bias)||std::abs(bias)>1000)throw std::invalid_argument("invalid layered height input");
    if(coverage==0||coverage==1)return coverage;
    return std::clamp(coverage+(topHeight-bottomHeight+bias)/width*coverage*(1-coverage),0.f,1.f);
}
struct Surface {
    std::array<float,3> albedo{1,1,1},normal{0,0,1},emissive{0,0,0};
    float roughness=1,metallic=0,ao=1,height=.5f;
};
inline Surface blend(const Surface& a,const Surface& b,float weight) {
    if(!std::isfinite(weight)||weight<0||weight>1)throw std::invalid_argument("invalid layer coverage");
    auto length=[](const std::array<float,3>& v){return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);};
    const auto al=length(a.normal),bl=length(b.normal);
    if(!std::isfinite(al)||!std::isfinite(bl)||al<1e-8f||bl<1e-8f)throw std::invalid_argument("invalid layer normal");
    const auto mix=[weight](float x,float y){return x+(y-x)*weight;};Surface out;
    for(unsigned i=0;i<3;++i){out.albedo[i]=mix(a.albedo[i],b.albedo[i]);out.normal[i]=mix(a.normal[i],b.normal[i]);out.emissive[i]=mix(a.emissive[i],b.emissive[i]);}
    auto magnitude=length(out.normal);if(magnitude<1e-8f){out.normal=weight<.5f?a.normal:b.normal;magnitude=length(out.normal);}
    for(auto& component:out.normal)component/=magnitude;
    out.roughness=mix(a.roughness,b.roughness);out.metallic=mix(a.metallic,b.metallic);out.ao=mix(a.ao,b.ao);out.height=mix(a.height,b.height);
    return out;
}
} // namespace engine::material::layered
