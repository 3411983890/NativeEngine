#ifndef SMOKE_MEDIUM_INCLUDED
#define SMOKE_MEDIUM_INCLUDED
struct SmokeVolume {
    float4 originCell;uint4 dimensions,offsets;
    float4 albedoEmission,optics;
};
// Closed grid bounds, cell-centred trilinear interpolation, clamped edge cells.
float2 SmokeValues(ByteAddressBuffer field,SmokeVolume v,float3 p){
    float3 grid=(p-v.originCell.xyz)/v.originCell.w;
    if(any(grid<0)||any(grid>=float3(v.dimensions.xyz)))return float2(0,v.optics.z);
    grid-=.5;int3 low=int3(floor(grid));float3 f=frac(grid);float2 value=0;
    [loop]for(uint j=0;j<8;j++){
        uint3 bit=uint3(j&1,(j>>1)&1,(j>>2)&1);uint3 at=uint3(clamp(low+int3(bit),0,int3(v.dimensions.xyz)-1));
        uint i=(at.z*v.dimensions.y+at.y)*v.dimensions.x+at.x;float3 w=lerp(1-f,f,float3(bit));
        uint rawDensity=v.offsets.x==0xffffffff?0:field.Load((v.offsets.x+i)*4);
        float density=v.dimensions.w!=0?(float)rawDensity*v.optics.w:asfloat(rawDensity);
        float temperature=v.offsets.y==0xffffffff?v.optics.z:asfloat(field.Load((v.offsets.y+i)*4));
        value+=float2(density,temperature)*w.x*w.y*w.z;
    }return value;
}
// Kelvin-driven artist emission, normalized at 1000K. This color ramp is not a
// spectral Planck integrator. Source units are linear RGB radiance per metre.
float3 SmokeEmission(float density,float temperature,SmokeVolume v){
    if(v.offsets.y==0xffffffff||temperature<v.optics.y||v.albedoEmission.w==0)return 0;
    float t=saturate((temperature-700)/2300);float3 color=lerp(float3(1,.03,.002),float3(1,.35,.04),saturate(t*2));
    color=lerp(color,float3(1,.9,.65),saturate(t*2-1));float ratio=temperature/1000;
    return min(color*(density*v.albedoEmission.w)*(ratio*ratio*ratio*ratio),1000000);
}
void SmokeAccumulate(ByteAddressBuffer field,SmokeVolume v,float3 p,inout float sigma,inout float3 scattering,inout float3 emission){
    float2 values=SmokeValues(field,v,p);float extinction=max(values.x,0)*v.optics.x;
    sigma+=extinction;scattering+=extinction*v.albedoEmission.rgb;emission+=SmokeEmission(max(values.x,0),values.y,v);
}
bool SmokeInterval(SmokeVolume v,float3 eye,float3 direction,float limit,out float a,out float b){
    a=0;b=limit;float3 hi=v.originCell.xyz+float3(v.dimensions.xyz)*v.originCell.w;
    [unroll]for(uint axis=0;axis<3;axis++){
        if(abs(direction[axis])<1e-8){if(eye[axis]<v.originCell[axis]||eye[axis]>hi[axis])return false;}
        else{float x=(v.originCell[axis]-eye[axis])/direction[axis],y=(hi[axis]-eye[axis])/direction[axis];a=max(a,min(x,y));b=min(b,max(x,y));}
    }return b>a;
}
#endif
