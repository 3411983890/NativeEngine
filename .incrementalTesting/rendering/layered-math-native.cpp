#include "../../engine/native/material_layered.hpp"
#include "../../engine/native/material_layer_gpu.hpp"
#include <iomanip>
#include <iostream>

int main() {
    using namespace engine::material::layered;
    std::cout<<std::setprecision(9);
    for(float mask:{0.f,.01f,.25f,.5f,.75f,.99f,1.f})
      for(float bottom:{-.5f,0.f,.5f,1.f})
        for(float top:{-.5f,0.f,.5f,1.f})
          for(float width:{.001f,.1f,1.f}) {
            const auto out=heightCoverage(mask,bottom,top,width,.125f);
            std::cout<<mask<<' '<<bottom<<' '<<top<<' '<<width<<' '<<.125f<<' '<<out<<'\n';
          }
    Surface a,b;a.normal={0,0,1};b.normal={0,0,-1};a.roughness=.1f;b.roughness=.9f;
    const auto midpoint=blend(a,b,.5f);
    if(midpoint.normal[2]!=-1||std::abs(midpoint.roughness-.5f)>1e-6f)return 1;
    bool rejected=false;try{(void)heightCoverage(.5f,0,0,0);}catch(const std::invalid_argument&){rejected=true;}
    return rejected?0:1;
}
