#ifndef NATIVEGPU_MATERIAL_LAYER_SAMPLING
#define NATIVEGPU_MATERIAL_LAYER_SAMPLING
#include "material_layer_blend.hlsli"

// Intermediate bindless layout. Slot order matches the layered material contract:
// albedo, normal, ORM, roughness, metallic, occlusion, emissive, height, mask.
// UINT_MAX is an absent texture. All other values are absolute descriptor indices.
struct LayerTexture {
    float3 row0; uint texture;
    float3 row1; uint sampler;
    uint uvSet; uint channel; uint flags; uint padding;
};
// Material-level relief parameters. flags: mode bits 0-1 (0 off, 1 POM, 2 baked),
// bit 2 invert, bit 3 self shadow, bit 4 baked texture source.
struct SurfaceReliefMaterial {
    float3 row0; uint texture;
    float3 row1; uint sampler;
    uint uvSet; uint channel; uint flags; uint heightLayer;
    float amplitudeMetres, referenceHeight, minSteps, maxSteps;
    float refineSteps, fadeStartMetres, fadeEndMetres, grazingFade;
    float shadowSteps, heightOffsetMetres, padding0, padding1;
};
struct LayerMaterial {
    LayerTexture slots[9];
    float3 baseColor; float roughness;
    float3 emissiveFactor; float metallic;
    float normalStrength, occlusionStrength, emissiveStrength, heightScale;
    float heightOffset, weight, gain, bias;
    float width, heightBias; uint source, flags;
};
struct LayerSurface {
    float3 albedo; float roughness;
    float3 normal; float metallic;
    float3 emissive; float ao;
    float height;
};
struct LayerCoordinates {
    float2 uv[16];
    float2 dx[16];
    float2 dy[16];
    float3 normal, positionDx, positionDy;
    float4 vertexColor;
};
float2 LayerMap(float2 uv, LayerTexture slot) {
    float3 p=float3(uv,1);
    return float2(dot(slot.row0,p),dot(slot.row1,p));
}
float2 LayerMapDerivative(float2 derivative, LayerTexture slot) {
    return float2(dot(slot.row0.xy,derivative),dot(slot.row1.xy,derivative));
}
float4 LayerSample(LayerTexture slot, LayerCoordinates coords, float4 fallback) {
    if(slot.texture==0xffffffffu) return fallback;
    Texture2D texture=ResourceDescriptorHeap[NonUniformResourceIndex(slot.texture)];
    SamplerState sampler=SamplerDescriptorHeap[NonUniformResourceIndex(slot.sampler)];
    float2 uv=LayerMap(coords.uv[slot.uvSet],slot);
    return texture.SampleGrad(sampler,uv,LayerMapDerivative(coords.dx[slot.uvSet],slot),LayerMapDerivative(coords.dy[slot.uvSet],slot));
}
float3 LayerNormal(LayerTexture slot, LayerCoordinates coords, float strength) {
    float3 n=normalize(coords.normal);
    if(slot.texture==0xffffffffu) return n;
    float2 dx=LayerMapDerivative(coords.dx[slot.uvSet],slot),dy=LayerMapDerivative(coords.dy[slot.uvSet],slot);
    float determinant=dx.x*dy.y-dx.y*dy.x;
    if(abs(determinant)<1e-10) return n;
    float3 tangent=(coords.positionDx*dy.y-coords.positionDy*dx.y)/determinant;
    tangent-=n*dot(n,tangent);
    if(dot(tangent,tangent)<1e-10) return n;
    tangent=normalize(tangent);
    float3 rawBitangent=(-coords.positionDx*dy.x+coords.positionDy*dx.x)/determinant;
    float3 bitangent=normalize(cross(n,tangent))*(dot(cross(n,tangent),rawBitangent)<0?-1:1);
    float3 sampled=LayerSample(slot,coords,float4(.5,.5,1,1)).xyz*2-1;
    if((slot.flags&1u)!=0) sampled.z=sqrt(saturate(1-dot(sampled.xy,sampled.xy)));
    sampled.xy*=strength;
    float3 result=tangent*sampled.x+bitangent*sampled.y+n*sampled.z;
    return dot(result,result)<1e-16?n:normalize(result);
}
LayerSurface LayerSampleSurface(LayerMaterial layer, LayerCoordinates coords) {
    LayerSurface surface;
    float4 albedo=LayerSample(layer.slots[0],coords,1);
    float4 orm=LayerSample(layer.slots[2],coords,1);
    float4 roughness=LayerSample(layer.slots[3],coords,1);
    float4 metallic=LayerSample(layer.slots[4],coords,1);
    float4 occlusion=LayerSample(layer.slots[5],coords,1);
    float4 emissive=LayerSample(layer.slots[6],coords,0);
    float4 height=LayerSample(layer.slots[7],coords,.5);
    surface.albedo=albedo.rgb*layer.baseColor;
    surface.normal=LayerNormal(layer.slots[1],coords,layer.normalStrength);
    surface.roughness=layer.roughness*(layer.slots[3].texture==0xffffffffu?orm.g:roughness[layer.slots[3].channel]);
    surface.metallic=layer.metallic*(layer.slots[4].texture==0xffffffffu?orm.b:metallic[layer.slots[4].channel]);
    float ao=layer.slots[5].texture==0xffffffffu?orm.r:occlusion[layer.slots[5].channel];
    surface.ao=lerp(1,ao,layer.occlusionStrength);
    surface.emissive=emissive.rgb*layer.emissiveFactor*layer.emissiveStrength;
    surface.height=height[layer.slots[7].channel]*layer.heightScale+layer.heightOffset;
    return surface;
}
LayerSurface LayerComposite(LayerSurface bottom, LayerSurface top, float weight) {
    LayerSurface result;
    result.albedo=lerp(bottom.albedo,top.albedo,weight);
    result.normal=LayerBlendNormal(bottom.normal,top.normal,weight);
    result.roughness=lerp(bottom.roughness,top.roughness,weight);
    result.metallic=lerp(bottom.metallic,top.metallic,weight);
    result.ao=lerp(bottom.ao,top.ao,weight);
    result.emissive=lerp(bottom.emissive,top.emissive,weight);
    result.height=lerp(bottom.height,top.height,weight);
    return result;
}
// source: 0 constant, 1 texture mask, 2 vertex color; flags: bit 0 invert,
// bit 1 height blending, bit 2 disabled. Every field is checked by the loader.
LayerSurface LayerApply(LayerSurface bottom, LayerMaterial top, LayerCoordinates coords) {
    if((top.flags&4u)!=0 || top.weight==0) return bottom;
    float raw=top.source==1?LayerSample(top.slots[8],coords,0)[top.slots[8].channel]:top.source==2?coords.vertexColor[top.slots[8].channel]:1;
    float coverage=top.weight*saturate(((top.flags&1u)!=0?1-raw:raw)*top.gain+top.bias);
    if(coverage==0) return bottom;
    LayerSurface surface=LayerSampleSurface(top,coords);
    if((top.flags&2u)!=0) coverage=LayerHeightCoverage(coverage,bottom.height,surface.height,top.width,top.heightBias);
    return LayerComposite(bottom,surface,coverage);
}

// --- Surface relief (POM) -------------------------------------------------
// Orthonormal frame for one UV set derived from world position derivatives.
struct ReliefBasis {
    float3 tangent, bitangent, normal;
    float metresPerU, metresPerV;
    bool valid;
};
ReliefBasis ReliefBasisFor(uint uvSet, LayerCoordinates coords) {
    ReliefBasis basis;
    basis.tangent=float3(1,0,0);basis.bitangent=float3(0,1,0);basis.normal=normalize(coords.normal);
    basis.metresPerU=1;basis.metresPerV=1;basis.valid=false;
    float2 dx=coords.dx[uvSet],dy=coords.dy[uvSet];
    float determinant=dx.x*dy.y-dx.y*dy.x;
    if(abs(determinant)<1e-10) return basis;
    float3 dpdu=(coords.positionDx*dy.y-coords.positionDy*dx.y)/determinant;
    float3 dpdv=(-coords.positionDx*dx.y+coords.positionDy*dx.x)/determinant;
    float lenU=length(dpdu),lenV=length(dpdv);
    if(lenU<1e-8||lenV<1e-8) return basis;
    float3 normal=basis.normal;
    float3 tangent=dpdu-normal*dot(normal,dpdu);
    if(dot(tangent,tangent)<1e-16) return basis;
    tangent=normalize(tangent);
    float3 bitangent=cross(normal,tangent);
    if(dot(bitangent,dpdv)<0) bitangent=-bitangent;
    basis.tangent=tangent;basis.bitangent=normalize(bitangent);
    basis.metresPerU=lenU;basis.metresPerV=lenV;basis.valid=true;
    return basis;
}
float ReliefRawHeight(SurfaceReliefMaterial relief, LayerMaterial heightLayer, LayerCoordinates coords, float2 uv) {
    if((relief.flags&16u)!=0) {
        if(relief.texture==0xffffffffu) return 1;
        Texture2D texture=ResourceDescriptorHeap[NonUniformResourceIndex(relief.texture)];
        SamplerState sampler=SamplerDescriptorHeap[NonUniformResourceIndex(relief.sampler)];
        float3 mappingPosition=float3(uv,1);
        float2 mapped=float2(dot(relief.row0,mappingPosition),dot(relief.row1,mappingPosition));
        float2 dx=float2(dot(relief.row0.xy,coords.dx[relief.uvSet]),dot(relief.row1.xy,coords.dx[relief.uvSet]));
        float2 dy=float2(dot(relief.row0.xy,coords.dy[relief.uvSet]),dot(relief.row1.xy,coords.dy[relief.uvSet]));
        return texture.SampleGrad(sampler,mapped,dx,dy)[relief.channel];
    }
    LayerTexture slot=heightLayer.slots[7];
    if(slot.texture==0xffffffffu) return 1;
    Texture2D texture=ResourceDescriptorHeap[NonUniformResourceIndex(slot.texture)];
    SamplerState sampler=SamplerDescriptorHeap[NonUniformResourceIndex(slot.sampler)];
    return texture.SampleGrad(sampler,LayerMap(uv,slot),LayerMapDerivative(coords.dx[slot.uvSet],slot),LayerMapDerivative(coords.dy[slot.uvSet],slot))[slot.channel];
}
float ReliefSurfaceDepth(SurfaceReliefMaterial relief, float height01) {
    float h=(relief.flags&4u)!=0?1-height01:height01;
    return clamp((relief.referenceHeight-h)*relief.amplitudeMetres,0,relief.amplitudeMetres*relief.referenceHeight);
}
// Bounded linear search plus refinement. The hit location is propagated from
// the height UV set to every other active UV set through world space.
// reliefInfo = (hit depth 0..1, UV offset magnitude 0..1, used steps 0..1) for debug views.
void ReliefApply(inout LayerCoordinates coords, SurfaceReliefMaterial relief, LayerMaterial heightLayer, float3 view, float distanceMetres, out float3 reliefInfo) {
    reliefInfo=float3(0,0,0);
    if((relief.flags&3u)!=1u||relief.amplitudeMetres<=0) return;
    uint heightUvSet=(relief.flags&16u)!=0?relief.uvSet:heightLayer.slots[7].uvSet;
    ReliefBasis heightBasis=ReliefBasisFor(heightUvSet,coords);
    if(!heightBasis.valid) return;
    float vn=dot(view,heightBasis.normal);
    if(vn<1e-4) return;
    float maxDepth=relief.amplitudeMetres*relief.referenceHeight;
    if(maxDepth<=0) return;
    float fade=1;
    if(relief.fadeEndMetres>relief.fadeStartMetres) fade*=saturate((relief.fadeEndMetres-distanceMetres)/(relief.fadeEndMetres-relief.fadeStartMetres));
    if(relief.grazingFade>0) fade*=saturate(vn/max(relief.grazingFade,1e-6));
    if(fade<=0) return;
    float2 slope=float2(dot(view,heightBasis.tangent)/(vn*heightBasis.metresPerU),dot(view,heightBasis.bitangent)/(vn*heightBasis.metresPerV));
    int steps=(int)clamp(round(lerp(relief.maxSteps,relief.minSteps,saturate(vn))),1.0,max(relief.maxSteps,1.0));
    float2 uv0=coords.uv[heightUvSet];
    float previous=0,hit=0;
    bool found=false;
    [loop] for(int step=1;step<=steps;++step) {
        float t=maxDepth*(float)step/(float)steps;
        float surface=ReliefSurfaceDepth(relief,ReliefRawHeight(relief,heightLayer,coords,uv0+t*slope));
        if(t>=surface) {
            float low=previous,high=t;
            [unroll] for(uint refine=0;refine<8u;++refine) {
                if(refine>=(uint)relief.refineSteps) break;
                float mid=(low+high)*.5;
                if(mid>=ReliefSurfaceDepth(relief,ReliefRawHeight(relief,heightLayer,coords,uv0+mid*slope))) high=mid; else low=mid;
            }
            hit=high;found=true;break;
        }
        previous=t;
    }
    if(!found) hit=maxDepth;
    float2 heightOffset=hit*fade*slope;
    float3 worldOffset=heightBasis.tangent*(heightOffset.x*heightBasis.metresPerU)+heightBasis.bitangent*(heightOffset.y*heightBasis.metresPerV);
    float offsetScale=max(maxDepth*length(slope),1e-6);
    reliefInfo=float3(saturate(hit/maxDepth),saturate(length(heightOffset)/offsetScale),found?(float)steps/(float)max(steps,1):1);
    [unroll] for(uint set=0;set<8u;++set) {
        ReliefBasis basis=ReliefBasisFor(set,coords);
        if(!basis.valid) continue;
        coords.uv[set]+=float2(dot(worldOffset,basis.tangent)/basis.metresPerU,dot(worldOffset,basis.bitangent)/basis.metresPerV);
    }
}
// Optional directional self-shadowing of the relief height field. Only direct
// light is attenuated; ambient and the source AO stay untouched.
float ReliefSelfShadow(SurfaceReliefMaterial relief, LayerMaterial heightLayer, LayerCoordinates coords, float3 lightDirection) {
    if((relief.flags&8u)==0u||relief.amplitudeMetres<=0) return 1;
    uint heightUvSet=(relief.flags&16u)!=0?relief.uvSet:heightLayer.slots[7].uvSet;
    ReliefBasis basis=ReliefBasisFor(heightUvSet,coords);
    if(!basis.valid) return 1;
    float ln=dot(lightDirection,basis.normal);
    if(ln<=0.05) return 1;
    float maxDepth=relief.amplitudeMetres*relief.referenceHeight;
    if(maxDepth<=0) return 1;
    float2 slope=float2(dot(lightDirection,basis.tangent)/(ln*basis.metresPerU),dot(lightDirection,basis.bitangent)/(ln*basis.metresPerV));
    float2 uv0=coords.uv[heightUvSet];
    float base=ReliefSurfaceDepth(relief,ReliefRawHeight(relief,heightLayer,coords,uv0));
    float maximum=maxDepth/max(ln,.1);
    int steps=(int)clamp(round(relief.shadowSteps),1.0,max(relief.shadowSteps,1.0));
    [loop] for(int step=1;step<=steps;++step) {
        float t=maximum*(float)step/(float)steps;
        float surface=ReliefSurfaceDepth(relief,ReliefRawHeight(relief,heightLayer,coords,uv0+t*slope));
        if(surface<base-t*ln-.002) return 0;
    }
    return 1;
}
#endif
