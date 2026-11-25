#pragma once
#include <cmath>
#include <limits>
namespace engine {
inline bool validMaterialIor(double value){return std::isfinite(value)&&(value==0||(value>=1&&value<=std::numeric_limits<float>::max()));}
inline float materialDielectricF0(float ior){const double ratio=(static_cast<double>(ior)-1)/(static_cast<double>(ior)+1);return ior==0?1.f:static_cast<float>(ratio*ratio);}
}
