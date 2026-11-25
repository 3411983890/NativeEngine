#include "solver.hpp"
#include "configuration.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace engine::smoke {
namespace {
bool finite(float v){return std::isfinite(v);}
bool finite(Vec3 v){return finite(v.x)&&finite(v.y)&&finite(v.z);}
float component(Vec3 v,unsigned a){return a==0?v.x:a==1?v.y:v.z;}
void component(Vec3& v,unsigned a,float x){if(a==0)v.x=x;else if(a==1)v.y=x;else v.z=x;}
float dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
using Cell=std::array<int,3>;
struct Grid {
    const Spec& s;const std::vector<CellInput>& inputs;Layout l;
    bool inside(Cell c) const{return c[0]>=0&&c[1]>=0&&c[2]>=0&&c[0]<int(s.dimensions[0])&&c[1]<int(s.dimensions[1])&&c[2]<int(s.dimensions[2]);}
    unsigned index(Cell c) const{return (c[2]*s.dimensions[1]+c[1])*s.dimensions[0]+c[0];}
    Cell cell(unsigned i) const{return {int(i%s.dimensions[0]),int(i/s.dimensions[0]%s.dimensions[1]),int(i/(s.dimensions[0]*s.dimensions[1]))};}
    bool solid(Cell c) const{return !inside(c)||inputs[index(c)].solid;}
    float value(const State& state,Cell c,unsigned a) const{return l.offsets[a]==UINT32_MAX?(a==1?s.ambientKelvin:0):state.scalars[l.offsets[a]+index(c)];}
    float wall(Cell c,unsigned a) const{return inside(c)?component(inputs[index(c)].wallVelocity,a):0;}
    bool wallFace(Cell c,unsigned a,float& v) const{auto next=c;++next[a];const bool x=solid(c),y=solid(next);if(!x&&!y)return false;v=((x?wall(c,a):0)+(y?wall(next,a):0))/float(unsigned(x)+unsigned(y));return true;}
    float face(const std::vector<Vec3>& velocity,Cell c,unsigned a) const{if(!inside(c))return 0;float v;if(wallFace(c,a,v))return v;return component(velocity[index(c)],a);}
    Vec3 center(const std::vector<Vec3>& velocity,Cell c) const{if(solid(c))return {};Vec3 out;for(unsigned a=0;a<3;++a){auto previous=c;--previous[a];component(out,a,.5f*(face(velocity,c,a)+face(velocity,previous,a)));}return out;}
    float sampleFace(const std::vector<Vec3>& velocity,Vec3 p,unsigned a) const{
        p=p-Vec3{.5f,.5f,.5f};component(p,a,component(p,a)-.5f);const Cell low{int(std::floor(p.x)),int(std::floor(p.y)),int(std::floor(p.z))};const float f[]={p.x-low[0],p.y-low[1],p.z-low[2]};float out=0;
        for(unsigned j=0;j<8;++j){Cell c=low;float w=1;for(unsigned k=0;k<3;++k){const unsigned bit=(j>>k)&1;c[k]+=bit;w*=bit?f[k]:1-f[k];}out+=w*face(velocity,c,a);}return out;
    }
    Vec3 sampleVelocity(const std::vector<Vec3>& velocity,Vec3 p) const{return {sampleFace(velocity,p,0),sampleFace(velocity,p,1),sampleFace(velocity,p,2)};}
    Vec3 trace(Vec3 start,Vec3 target) const{
        const Vec3 d=target-start;Cell c{int(std::floor(start.x)),int(std::floor(start.y)),int(std::floor(start.z))};float next[3],stride[3];int direction[3];
        for(unsigned a=0;a<3;++a){const float v=component(d,a);direction[a]=v>0?1:-1;stride[a]=std::abs(v)>1e-12f?std::abs(1/v):1e20f;next[a]=std::abs(v)>1e-12f?(float(c[a]+(v>0))-component(start,a))/v:1e20f;}
        for(unsigned i=0;i<8;++i){unsigned a=next[0]<=next[1]&&next[0]<=next[2]?0:next[1]<=next[2]?1:2;const float t=next[a];if(t>=1)return target;c[a]+=direction[a];if(solid(c))return start+d*std::max(0.f,t-1e-4f);next[a]+=stride[a];}return start;
    }
    float sampleScalar(const State& state,Vec3 p,unsigned a,float* minimum=nullptr,float* maximum=nullptr) const{
        p=p-Vec3{.5f,.5f,.5f};const Cell low{int(std::floor(p.x)),int(std::floor(p.y)),int(std::floor(p.z))};const float f[]={p.x-low[0],p.y-low[1],p.z-low[2]};float out=0,weights=0,mn=std::numeric_limits<float>::max(),mx=-mn;
        for(unsigned j=0;j<8;++j){Cell c=low;float w=1;for(unsigned k=0;k<3;++k){const unsigned bit=(j>>k)&1;c[k]+=bit;w*=bit?f[k]:1-f[k];}if(solid(c))continue;const float v=value(state,c,a);out+=w*v;weights+=w;mn=std::min(mn,v);mx=std::max(mx,v);}
        if(weights<1e-8f){out=a==1?s.ambientKelvin:0;mn=mx=out;}else out/=weights;if(minimum)*minimum=mn;if(maximum)*maximum=mx;return out;
    }
    std::vector<float> divergence(const std::vector<Vec3>& v) const{std::vector<float> out(l.cells);for(unsigned i=0;i<l.cells;++i){const auto c=cell(i);if(solid(c))continue;for(unsigned a=0;a<3;++a){auto previous=c;--previous[a];out[i]+=(face(v,c,a)-face(v,previous,a))/s.cellMetres;}}return out;}
};
float bounded(float value,float low,float high,Diagnostics& d){const auto out=std::clamp(value,low,high);d.clampedValues+=out!=value;return out;}
}
Layout layout(const Spec& s){return configurationLayout(s);}

unsigned substeps(const Spec& s,float dt){layout(s);if(!finite(dt)||dt<=0||dt>.25f)throw std::invalid_argument("NG-E-SMOKE: invalid dt");const auto count=std::max(1u,unsigned(std::ceil(double(dt)*s.maxSpeed/(s.cellMetres*s.courant))));if(count>s.maxSubsteps)throw std::invalid_argument("NG-E-SMOKE: CFL substep budget exceeded");return count;}
State initialState(const Spec& s){const auto l=layout(s);State out;out.scalars.resize(l.scalarCount);out.velocity.resize(l.cells);if(l.offsets[1]!=UINT32_MAX)std::fill_n(out.scalars.begin()+l.offsets[1],l.cells,s.ambientKelvin);return out;}
void validateInputs(const Spec& s,const std::vector<CellInput>& in){const auto l=layout(s);if(in.size()!=l.cells)throw std::invalid_argument("NG-E-SMOKE: input extent");for(const auto& c:in){if((!(s.channels&Density)&&c.densityRate!=0)||(!(s.channels&Temperature)&&c.temperatureRate!=0)||(!(s.channels&Fuel)&&c.fuelRate!=0))throw std::invalid_argument("NG-E-SMOKE: source requires an undeclared channel");if(!finite(c.densityRate)||c.densityRate<0||c.densityRate>1e6f||!finite(c.temperatureRate)||std::abs(c.temperatureRate)>1e6f||!finite(c.fuelRate)||c.fuelRate<0||c.fuelRate>1e6f||!finite(c.acceleration)||!finite(c.wallVelocity))throw std::invalid_argument("NG-E-SMOKE: invalid source/boundary");for(unsigned a=0;a<3;++a)if(std::abs(component(c.acceleration,a))>10000||std::abs(component(c.wallVelocity,a))>s.maxSpeed)throw std::invalid_argument("NG-E-SMOKE: input velocity/force bound");}}
void validateState(const Spec& s,const State& state){const auto l=layout(s);if(state.scalars.size()!=l.scalarCount||state.velocity.size()!=l.cells)throw std::invalid_argument("NG-E-SMOKE: state extent");for(unsigned a=0;a<3;++a)if(l.offsets[a]!=UINT32_MAX)for(unsigned i=0;i<l.cells;++i){const float v=state.scalars[l.offsets[a]+i],lo=a==1?s.ambientKelvin:0,hi=a==0?s.maxDensity:a==1?s.maxTemperature:s.maxFuel;if(!finite(v)||v<lo||v>hi)throw std::invalid_argument("NG-E-SMOKE: initial scalar bounds");}for(auto v:state.velocity)for(unsigned a=0;a<3;++a)if(!finite(component(v,a))||std::abs(component(v,a))>s.maxSpeed)throw std::invalid_argument("NG-E-SMOKE: initial velocity bounds");}
Diagnostics step(const Spec& s,const std::vector<CellInput>& inputs,State& state,float dt){
    const auto steps=substeps(s,dt);validateInputs(s,inputs);validateState(s,state);const auto l=layout(s);Grid g{s,inputs,l};Diagnostics d;d.substeps=steps;const float h=dt/steps,dx=s.cellMetres;std::vector<Vec3> advected(l.cells),forced(l.cells),projected(l.cells),curl(l.cells),confinement(l.cells);std::vector<float> p(l.cells),next(l.cells);State forward=state,backward=state;
    for(unsigned sub=0;sub<steps;++sub){
        for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);const auto& source=inputs[i];float density=g.value(state,c,0),temperature=g.value(state,c,1),fuel=g.value(state,c,2);if(source.solid){density=fuel=0;temperature=s.ambientKelvin;}else{density+=source.densityRate*h;temperature=s.ambientKelvin+(temperature-s.ambientKelvin)*std::exp(-s.cooling*h)+source.temperatureRate*h;fuel=fuel*std::exp(-s.fuelDecay*h)+source.fuelRate*h;if((s.channels&(Temperature|Fuel))==(Temperature|Fuel)&&temperature>=s.ignitionKelvin){const float burned=fuel*(1-std::exp(-s.burnRate*h));fuel-=burned;temperature+=burned*s.heatPerFuel;density+=burned*s.sootPerFuel;}density*=std::exp(-s.densityDecay*h);}
            const float values[]={bounded(density,0,s.maxDensity,d),bounded(temperature,s.ambientKelvin,s.maxTemperature,d),bounded(fuel,0,s.maxFuel,d)};for(unsigned a=0;a<3;++a)if(l.offsets[a]!=UINT32_MAX)state.scalars[l.offsets[a]+i]=values[a];
        }
        for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);for(unsigned a=0;a<3;++a){float wall;if(g.wallFace(c,a,wall)){component(advected[i],a,wall);continue;}Vec3 at{c[0]+.5f,c[1]+.5f,c[2]+.5f};component(at,a,component(at,a)+.5f);const auto back=g.trace(at,at-g.sampleVelocity(state.velocity,at)*(h/dx));component(advected[i],a,g.sampleFace(state.velocity,back,a));}}
        if(s.vorticity>0){for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);Vec3 derivative[3];for(unsigned a=0;a<3;++a){auto lo=c,hi=c;--lo[a];++hi[a];derivative[a]=(g.center(advected,hi)-g.center(advected,lo))/(2*dx);}curl[i]={derivative[1].z-derivative[2].y,derivative[2].x-derivative[0].z,derivative[0].y-derivative[1].x};}
            for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);Vec3 grad;for(unsigned a=0;a<3;++a){auto lo=c,hi=c;--lo[a];++hi[a];const float x=g.solid(lo)?0:std::sqrt(dot(curl[g.index(lo)],curl[g.index(lo)])),y=g.solid(hi)?0:std::sqrt(dot(curl[g.index(hi)],curl[g.index(hi)]));component(grad,a,(y-x)/(2*dx));}const float length=std::sqrt(dot(grad,grad));confinement[i]=length>1e-6f?cross(grad/length,curl[i])*(s.vorticity*dx):Vec3{};}}
        for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);for(unsigned a=0;a<3;++a){float wall;if(g.wallFace(c,a,wall)){component(forced[i],a,wall);continue;}auto neighbor=c;++neighbor[a];const auto j=g.index(neighbor);float acceleration=.5f*(component(inputs[i].acceleration,a)+component(inputs[j].acceleration,a));if(a==1)acceleration+=s.buoyancy*(.5f*(g.value(state,c,1)+g.value(state,neighbor,1))-s.ambientKelvin)-s.smokeWeight*.5f*(g.value(state,c,0)+g.value(state,neighbor,0));if(s.vorticity>0)acceleration+=.5f*(component(confinement[i],a)+component(confinement[j],a));component(forced[i],a,bounded(component(advected[i],a)+h*acceleration,-s.maxSpeed,s.maxSpeed,d));}}
        const auto divergence=g.divergence(forced);std::fill(p.begin(),p.end(),0);
        for(unsigned iteration=0;iteration<s.pressureIterations;++iteration){for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);float sum=0;unsigned count=0;if(!g.solid(c))for(unsigned a=0;a<3;++a)for(int sign:{-1,1}){auto neighbor=c;neighbor[a]+=sign;if(!g.solid(neighbor)){sum+=p[g.index(neighbor)];++count;}}next[i]=count?p[i]+s.pressureRelaxation*((sum-dx*dx*divergence[i])/count-p[i]):0;}p.swap(next);}
        for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);for(unsigned a=0;a<3;++a){float wall;if(g.wallFace(c,a,wall)){component(projected[i],a,wall);continue;}auto neighbor=c;++neighbor[a];component(projected[i],a,bounded(component(forced[i],a)-(p[g.index(neighbor)]-p[i])/dx,-s.maxSpeed,s.maxSpeed,d));}}
        const auto after=g.divergence(projected);double beforeSum=0,afterSum=0,residualSum=0;unsigned fluid=0;for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);if(g.solid(c))continue;float laplacian=0;for(unsigned a=0;a<3;++a)for(int sign:{-1,1}){auto neighbor=c;neighbor[a]+=sign;if(!g.solid(neighbor))laplacian+=(p[g.index(neighbor)]-p[i])/(dx*dx);}const float residual=divergence[i]-laplacian;beforeSum+=double(divergence[i])*divergence[i];afterSum+=double(after[i])*after[i];residualSum+=double(residual)*residual;++fluid;}d.divergenceBeforeL2=std::sqrt(beforeSum/std::max(1u,fluid));d.divergenceAfterL2=std::sqrt(afterSum/std::max(1u,fluid));d.pressureResidualL2=std::sqrt(residualSum/std::max(1u,fluid));
        const auto advect=[&](const State& input,State& output,float time){for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);const Vec3 at{c[0]+.5f,c[1]+.5f,c[2]+.5f};const auto back=g.trace(at,at-g.center(projected,c)*(time/dx));for(unsigned a=0;a<3;++a)if(l.offsets[a]!=UINT32_MAX)output.scalars[l.offsets[a]+i]=g.solid(c)?(a==1?s.ambientKelvin:0):g.sampleScalar(input,back,a);}};
        advect(state,forward,h);if(s.macCormack){advect(forward,backward,-h);for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);if(g.solid(c))continue;const Vec3 at{c[0]+.5f,c[1]+.5f,c[2]+.5f};const auto back=g.trace(at,at-g.center(projected,c)*(h/dx));for(unsigned a=0;a<3;++a)if(l.offsets[a]!=UINT32_MAX){float mn,mx;g.sampleScalar(state,back,a,&mn,&mx);const auto j=l.offsets[a]+i;forward.scalars[j]=std::clamp(forward.scalars[j]+.5f*(state.scalars[j]-backward.scalars[j]),mn,mx);}}}state.scalars.swap(forward.scalars);state.velocity=projected;
    }
    const double voxel=double(dx)*dx*dx;for(unsigned i=0;i<l.cells;++i){const auto c=g.cell(i);if(!inputs[i].solid){d.densityIntegral+=g.value(state,c,0)*voxel;d.fuelIntegral+=g.value(state,c,2)*voxel;}for(unsigned a=0;a<3;++a)d.maximumSpeed=std::max(d.maximumSpeed,std::abs(component(state.velocity[i],a)));}return d;
}
}
