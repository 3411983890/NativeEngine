#ifndef LIGHTING_QUALITY_COMMON
#define LIGHTING_QUALITY_COMMON
#include "smoke_medium.hlsli"
static const float QPi = 3.14159265359;
struct QualityLight {
    float4 positionRange, colorIntensity, directionCos, params, shadow;
};
struct QualityInstance {
    column_major float4x4 inverseModel;
    float4 color;
    uint4 textures;
    float4 material;
};
struct QualityConstants {
    column_major float4x4 inverseViewProjection, viewProjection, previousViewProjection;
    float4 eyeFrame, forwardHistory, previousEye, previousForward;
    float4 keyDirectionIntensity, keyColor, skyExposure, ambient;
    float4 fog, cloud, wind, probeOriginSpacing, probeCountsHistory;
    uint4 grid, screen, slots[15];
    float4 sourceEnvironment;
    float4 cloudShadowOrigin, diffuseSH[9];
    uint4 volumeInfo;
    float4 volumeCenters[8], volumeExtents[8];
    QualityLight lights[64];
    uint4 smokeInfo;SmokeVolume smokeVolumes[8];
};
float QHash(float3 p) { return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453); }
float3 QRay(ConstantBuffer<QualityConstants> q, float2 uv) {
    float4 p = mul(q.inverseViewProjection, float4(uv.x * 2 - 1, 1 - uv.y * 2, 1, 1));
    return normalize(p.xyz / p.w - q.eyeFrame.xyz);
}
float QVisibilityMask(ConstantBuffer<QualityConstants> q, float3 p, float3 dir, float range,uint mask) {
    if (q.slots[10].x == 0 || range <= 0.005) return 1;
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[q.slots[0].x];
    RayDesc ray; ray.Origin = p; ray.Direction = dir; ray.TMin = 0.005; ray.TMax = range;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, 0, mask, ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1 : 0;
}
float QVisibility(ConstantBuffer<QualityConstants> q,float3 p,float3 dir,float range) {
    return QVisibilityMask(q,p,dir,range,7);
}
float3 QCubeDirection(uint face,float2 uv);
uint QCubeCoordinates(float3 d,out float2 uv) {
    float3 a=abs(d);float2 p;uint face;
    if(a.x>=a.y&&a.x>=a.z){face=d.x>0?0:1;p=float2(d.x>0?-d.z:d.z,-d.y)/a.x;}
    else if(a.y>=a.z){face=d.y>0?2:3;p=float2(d.x,d.y>0?d.z:-d.z)/a.y;}
    else {face=d.z>0?4:5;p=float2(d.z>0?d.x:-d.x,-d.y)/a.z;}
    uv=p*0.5+0.5;return face;
}
float QLocalVisibility(ConstantBuffer<QualityConstants> q,uint index,float3 p,float3 normal) {
    float3 delta=q.lights[index].positionRange.xyz-p;float distance=length(delta);
    if(distance<0.04)return 1;
    float3 dir=delta/distance;float range=max(distance-0.04,0.006);
    uint cache=q.lights[index].params.z>0?uint(q.lights[index].params.z)-1:0xFFFFFFFF;
    if(cache>=q.slots[12].z)return QVisibilityMask(q,p,dir,range,3);
    Texture2DArray<float> depth=ResourceDescriptorHeap[q.slots[12].x];
    float2 uv;uint face=QCubeCoordinates(-dir,uv);float visible=0;
    [unroll]for(uint sample=0;sample<4;sample++) {
        float2 offset=float2((sample&1)?1:-1,(sample&2)?1:-1)*(0.5/128);
        float2 sampleUv;uint sampleFace=QCubeCoordinates(QCubeDirection(face,uv+offset),sampleUv);
        uint2 texel=min(uint2(saturate(sampleUv)*128),127u);
        float3 sampleDirection=QCubeDirection(sampleFace,(float2(texel)+0.5)/128);
        float compare=distance;
        if(dot(normal,normal)>0.5) {
            float denominator=dot(sampleDirection,normal);
            if(abs(denominator)>0.001)compare=dot(p-q.lights[index].positionRange.xyz,normal)/denominator;
        }
        float z=depth.Load(int4(texel,cache*6+sampleFace,0));
        visible+=(compare-0.015<=z)?0.25:0;
    }
    return visible>0?visible*QVisibilityMask(q,p,dir,range,2):0;
}
float3 QEnvironmentDirection(ConstantBuffer<QualityConstants> q,float3 dir){return float3(q.sourceEnvironment.x*dir.x-q.sourceEnvironment.y*dir.z,dir.y,q.sourceEnvironment.y*dir.x+q.sourceEnvironment.x*dir.z);}
float3 QSky(ConstantBuffer<QualityConstants> q, float3 dir) {
    if(q.slots[14].y!=0){
        Texture2D<float4> panorama=ResourceDescriptorHeap[q.slots[14].x];uint width,height;panorama.GetDimensions(width,height);float3 d=QEnvironmentDirection(q,normalize(dir));float2 coord=float2(.5+atan2(d.z,d.x)/(2*QPi),acos(clamp(d.y,-1,1))/QPi)*float2(width,height)-.5;int2 at=int2(floor(coord));float2 f=frac(coord);float3 value=0;
        [unroll]for(uint i=0;i<4;i++){int2 o=int2(i&1,i>>1),p=at+o;p.x=(p.x+int(width))%int(width);p.y=clamp(p.y,0,int(height)-1);float2 w=lerp(1-f,f,float2(o));value+=panorama.Load(int3(p,0)).rgb*w.x*w.y;}return value*q.sourceEnvironment.z;
    }
    return q.skyExposure.rgb * (dir.y < 0 ? 0.12 : lerp(0.45, 1.8, sqrt(saturate(dir.y))));
}
float3 QDiffuseSky(ConstantBuffer<QualityConstants> q, float3 n) {
    if(q.slots[14].y!=0)n=QEnvironmentDirection(q,n);
    float basis[9] = {0.282095, 0.488603*n.y, 0.488603*n.z, 0.488603*n.x,
        1.092548*n.x*n.y, 1.092548*n.y*n.z, 0.315392*(3*n.z*n.z-1),
        1.092548*n.x*n.z, 0.546274*(n.x*n.x-n.y*n.y)};
    float3 e = 0; [unroll] for(uint i=0;i<9;i++) e += q.diffuseSH[i].rgb * basis[i];
    return max(e, 0)*(q.slots[14].y!=0?q.sourceEnvironment.z:1);
}
float2 QOct(float3 n) {
    n /= abs(n.x)+abs(n.y)+abs(n.z);
    float2 v = n.xy;
    if(n.z < 0) v = (1-abs(v.yx)) * float2(v.x>=0?1:-1, v.y>=0?1:-1);
    return v*0.5+0.5;
}
float3 QOctDirection(float2 uv) {
    float2 v=uv*2-1; float3 n=float3(v,1-abs(v.x)-abs(v.y));
    if(n.z<0) n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
// Each 8x8 probe tile has a 6x6 interior and an octahedrally folded border.
float2 QBorderUV(uint2 t) {
    int2 p=int2(t)-1;
    if(p.x<0){p.x=0;p.y=5-p.y;} else if(p.x>5){p.x=5;p.y=5-p.y;}
    if(p.y<0){p.y=0;p.x=5-p.x;} else if(p.y>5){p.y=5;p.x=5-p.x;}
    return (float2(p)+0.5)/6;
}
float4 QProbeTexel(Texture2D<float4> tex, uint index, float3 direction) {
    uint2 tile=uint2(index%16,index/16)*8;
    float2 s=QOct(direction)*6+0.5;
    uint2 b=uint2(floor(s)); float2 f=frac(s);
    return lerp(lerp(tex.Load(int3(tile+b,0)),tex.Load(int3(tile+b+uint2(1,0),0)),f.x),
                lerp(tex.Load(int3(tile+b+uint2(0,1),0)),tex.Load(int3(tile+b+1,0)),f.x),f.y);
}
// RGB is incident diffuse irradiance from surfaces. A is the visible sky fraction;
// sky radiance itself is supplied by SH, avoiding a second copy of the environment.
float4 QIrradiance(ConstantBuffer<QualityConstants> q, float3 p, float3 n) {
    if((q.slots[9].w & 2)==0) return float4(0,0,0,1);
    Texture2D<float4> irr=ResourceDescriptorHeap[q.slots[1].x];
    Texture2D<float4> moments=ResourceDescriptorHeap[q.slots[1].y];
    Texture3D<float4> data=ResourceDescriptorHeap[q.slots[1].z];
    float3 cell=(p-q.probeOriginSpacing.xyz)/q.probeOriginSpacing.w;
    int3 base=int3(floor(cell)); float3 fraction=frac(cell);
    float4 total=0; float weights=0;
    [unroll] for(uint i=0;i<8;i++) {
        int3 o=int3(i&1,(i>>1)&1,(i>>2)&1), c=base+o;
        if(any(c<0)||any(c>=int3(16,8,16)))continue;
        float4 state=data.Load(int4(c,0)); if(state.w<0.5)continue;
        float3 probe=q.probeOriginSpacing.xyz+float3(c)*q.probeOriginSpacing.w+state.xyz;
        float3 delta=p+n*0.06-probe; float distance=length(delta);
        float3 direction=delta/max(distance,0.001);
        uint id=c.x+16*(c.y+8*c.z);
        float2 depth=QProbeTexel(moments,id,direction).rg;
        float variance=max(depth.y-depth.x*depth.x,0.01);
        float d=max(distance-depth.x,0);
        float visible=pow(variance/(variance+d*d),3);
        float3 tri=lerp(1-fraction,fraction,float3(o));
        float normalWeight=pow(saturate(dot(n,-direction)*0.5+0.5),2)+0.05;
        float weight=tri.x*tri.y*tri.z*visible*normalWeight;
        total+=QProbeTexel(irr,id,n)*weight; weights+=weight;
    }
    return weights>1e-5 ? total/weights : float4(0,0,0,1);
}
float QCloudVisibility(ConstantBuffer<QualityConstants> q, float3 p, SamplerState samplerState) {
    if((q.slots[9].w&4)==0 || q.keyDirectionIntensity.y<0.02 || p.y>=q.cloud.y) return 1;
    float3 base=p+q.keyDirectionIntensity.xyz*((q.cloud.y-p.y)/q.keyDirectionIntensity.y);
    float2 uv=(base.xz-q.cloudShadowOrigin.xy)/q.cloudShadowOrigin.z+0.5;
    if(any(uv<0)||any(uv>1))return 1;
    Texture2D<float> shadow=ResourceDescriptorHeap[q.slots[5].x];
    return shadow.SampleLevel(samplerState,uv,0);
}
float3 QLocalRadiance(QualityLight light,float3 p,out float3 direction,out float distance) {
    float3 delta=light.positionRange.xyz-p; distance=length(delta);
    direction=delta/max(distance,0.001);
    float a=saturate(1-distance/max(light.positionRange.w,0.001)); a*=a;
    if(light.params.x>0.5) a*=saturate((dot(-direction,normalize(light.directionCos.xyz))-
        light.directionCos.w)/max(light.params.y-light.directionCos.w,0.001));
    return light.colorIntensity.rgb*light.colorIntensity.w*a;
}
float QPhase(float cosine,float g) {
    return (1-g*g)/(4*QPi*pow(max(1+g*g-2*g*cosine,0.001),1.5));
}
float3 QCubeDirection(uint face,float2 uv) {
    float2 p=uv*2-1;
    if(face==0)return normalize(float3(1,-p.y,-p.x));
    if(face==1)return normalize(float3(-1,-p.y,p.x));
    if(face==2)return normalize(float3(p.x,1,p.y));
    if(face==3)return normalize(float3(p.x,-1,-p.y));
    if(face==4)return normalize(float3(p.x,-p.y,1));
    return normalize(float3(-p.x,-p.y,-1));
}
float2 QHammersley(uint i,uint count) { return float2(float(i)/count,reversebits(i)*2.3283064365386963e-10); }
float3 QGGX(float2 xi,float rough,float3 n) {
    float a=max(rough*rough,0.001), phi=2*QPi*xi.x;
    float cosTheta=sqrt((1-xi.y)/(1+(a*a-1)*xi.y));
    float3 h=float3(cos(phi)*sqrt(1-cosTheta*cosTheta),sin(phi)*sqrt(1-cosTheta*cosTheta),cosTheta);
    float3 t=normalize(cross(abs(n.z)<0.999?float3(0,0,1):float3(1,0,0),n));
    return normalize(t*h.x+cross(n,t)*h.y+n*h.z);
}
float3 QEnvironmentSpecular(ConstantBuffer<QualityConstants> q,float3 n,float3 view,float rough,
                            float3 f0,SamplerState samplerState) {
    TextureCube<float4> cube=ResourceDescriptorHeap[q.slots[0].z];
    Texture2D<float2> lut=ResourceDescriptorHeap[q.slots[0].w];
    float2 brdf=lut.SampleLevel(samplerState,clamp(float2(saturate(dot(n,view)),rough),1.0/128,127.0/128),0);
    return cube.SampleLevel(samplerState,reflect(-view,n),rough*6).rgb*(f0*brdf.x+brdf.y);
}
#endif
