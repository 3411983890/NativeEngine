#pragma once
#include "asset.hpp"
#include <cmath>

namespace engine::cloth::detail {
inline float dot3(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 cross3(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline float length3(Vec3 a){return std::sqrt(dot3(a,a));}
inline bool membrane(const std::array<Vec3,3>& p,const Membrane& m,unsigned kind,float& value,std::array<Vec3,3>& gradient){
    const auto e0=p[1]-p[0],e1=p[2]-p[0];const auto& a=m.inverse;
    const Vec3 u=e0*a[0]+e1*a[1],v=e0*a[2]+e1*a[3];const float cu[]={-a[0]-a[1],a[0],a[1]},cv[]={-a[2]-a[3],a[2],a[3]};
    if(kind<2){const auto f=kind==0?u:v;const float l=length3(f);if(l<1e-9f)return false;value=l-1;for(unsigned i=0;i<3;++i)gradient[i]=f*((kind==0?cu[i]:cv[i])/l);}
    else{value=dot3(u,v);for(unsigned i=0;i<3;++i)gradient[i]=v*cu[i]+u*cv[i];}return true;
}
inline bool dihedral(const std::array<Vec3,4>& p,float& angle,std::array<Vec3,4>& gradient){
    const Vec3 e=p[1]-p[0],n0=cross3(e,p[2]-p[0]),n1=cross3(p[3]-p[0],e);const float e2=dot3(e,e),a=dot3(n0,n0),b=dot3(n1,n1);
    if(e2<1e-16f||a<1e-20f||b<1e-20f)return false;const float l=std::sqrt(e2),norm=std::sqrt(a*b);
    angle=std::atan2(dot3(cross3(n0,n1),e)/(norm*l),dot3(n0,n1)/norm);
    gradient[2]=n0*(-l/a);gradient[3]=n1*(-l/b);
    gradient[0]=gradient[2]*(dot3(p[2]-p[1],e)/e2)+gradient[3]*(dot3(p[3]-p[1],e)/e2);
    gradient[1]=gradient[2]*(-dot3(p[2]-p[0],e)/e2)+gradient[3]*(-dot3(p[3]-p[0],e)/e2);return true;
}
inline float angleDifference(float a,float b){return std::atan2(std::sin(a-b),std::cos(a-b));}
}
