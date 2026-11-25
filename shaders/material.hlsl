// Cooked-material shader, compiled as SM 6.6 so textures come from
// ResourceDescriptorHeap instead of a fixed Texture2DArray.
//
// SceneConstants matches renderer/d3d12_renderer_internal.h exactly: a legacy 44-float
// prefix for the SM 5.1 fallback, then the 64-light buffer, key light and fog parameters
// used by the lighting system.
cbuffer DrawConstants : register(b0) {
    column_major float4x4 model;
    column_major float4x4 viewProjection;
    column_major float4x4 normalMatrix;
    float4 baseColor;
    float roughness;
    float metalness;
    uint albedoIndex;
    uint normalIndex;
    uint ormIndex;
    float uvScale;
};

struct GpuLight {
    float4 positionRange;
    float4 colorIntensity;
    float4 directionCos;
    float4 params; // x = type (0 point, 1 spot), y = cosInner
};

struct TextureMapping {
    float3 row0; uint samplerIndex;
    float3 row1; uint padding;
};
#if defined(PASS_PS) && !defined(PASS_INLINE)
#include "material_layer_sampling.hlsli"
#endif
struct GpuMaterial {
    uint albedo;
    uint normal;
    uint orm;
    uint flags;
    float uvScale;
    float roughness;
    float metalness;
    float wetness;
    float wear;
    float clearcoat;
    float clearcoatRoughness;
    float alphaCutoff;
    uint emissive;
    float3 emissiveFactor;
    float emissiveStrength;
    float opacity;
    float dielectricF0;
    uint coat;
    uint occlusion;
    float occlusionStrength;
    uint coatRoughness;
    uint coatNormal;
    TextureMapping textureMappings[8];
#if defined(PASS_PS) && !defined(PASS_INLINE)
    uint layerCount;
    uint layerOffset;
    uint layerDescriptor;
    SurfaceReliefMaterial surfaceRelief;
#endif
};
StructuredBuffer<GpuMaterial> materialRecords : register(t4);

cbuffer SceneConstants : register(b1) {
    float4 ambient;
    float4 legacyLightPositionRange[4];
    float4 legacyLightColorIntensity[4];
    uint legacyLightCount;
    float3 scenePad;
    float4 cameraPosition;
    GpuLight lights[64];
    float4 keyDirectionIntensity; // xyz direction to key, w intensity
    float4 keyColor;
    float4 fogParams;
    float4 fogColor;
    uint gpuLightCount;
    uint qualitySlot;
    uint transparencyBase;
    uint transparencyCapacity;
    uint layerDebugMode;
    uint layerDebugLayer;
    uint2 layerDebugPadding;
};
#include "lighting_quality_common.hlsli"
#include "transparency_common.hlsli"
cbuffer SkinPalette : register(b2) { column_major float4x4 joints[256]; float4 morphWeights[4]; uint vertexCount; uint morphCount; uint2 skinPadding; };
struct SkinDelta { float3 position; float3 normal; };
struct MorphDelta { float3 position; float3 normal; float3 tangent; };
StructuredBuffer<MorphDelta> morphDeltas : register(t20);
StructuredBuffer<SkinDelta> softDeltas : register(t21);
SamplerState materialSampler : register(s0);

struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
    float2 normalUv:TEXCOORD1;float2 ormUv:TEXCOORD2;float2 emissiveUv:TEXCOORD3;float2 occlusionUv:TEXCOORD4;float4 color:COLOR0;float4 tangent:TANGENT0;
    float2 coatUv:TEXCOORD5;float2 coatRoughnessUv:TEXCOORD6;float2 coatNormalUv:TEXCOORD7; };
struct PixelInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    // Object-space position scaled to metres, plus the untransformed normal. Planar UVs
    // are derived from these so a moving/rotating object keeps its texture attached.
    float3 materialPosition : TEXCOORD2;
    float3 materialNormal : TEXCOORD3;
    nointerpolation uint meshUv : TEXCOORD4;
    float2 normalUv:TEXCOORD5;float2 ormUv:TEXCOORD6;float2 emissiveUv:TEXCOORD7;float2 occlusionUv:TEXCOORD8;
    float4 color:COLOR0;
    float4 tangent:TEXCOORD9;
    float2 coatUv:TEXCOORD10;float2 coatRoughnessUv:TEXCOORD11;float2 coatNormalUv:TEXCOORD12;
};
struct PixelOutput {
    float4 surface : SV_TARGET0;
    float4 keyLight : SV_TARGET1;
};

#if defined(PASS_VS)
PixelInput VSMain(VertexInput input) {
    PixelInput output;
    float4 world = mul(model, float4(input.position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)normalMatrix, input.normal);
    output.uv = input.uv;
    output.worldPosition = world.xyz;
    // model columns carry scale; using it keeps texel density independent of object size
    // while the UV itself stays in object space (fixed to the object).
    float3 objectScale = float3(
        length(float3(model[0][0], model[1][0], model[2][0])),
        length(float3(model[0][1], model[1][1], model[2][1])),
        length(float3(model[0][2], model[1][2], model[2][2])));
    output.materialPosition = input.position * objectScale;
    // Planar UV projection needs the world-space direction. A local normal under
    // non-uniform scale (16 x 0.36 x 16 ground) would be misread as a side face.
    output.materialNormal = normalize(mul((float3x3)normalMatrix, input.normal));
    output.meshUv = ((normalIndex & 0x40000000u) != 0 ? 1u : 0u)|((normalIndex & 0x20000000u) != 0 ? 2u : 0u)|((normalIndex & 0x10000000u) != 0 ? 4u : 0u);
    output.normalUv=input.normalUv;output.ormUv=input.ormUv;output.emissiveUv=input.emissiveUv;output.occlusionUv=input.occlusionUv;
    output.color=input.color;
    output.coatUv=input.coatUv;output.coatRoughnessUv=input.coatRoughnessUv;output.coatNormalUv=input.coatNormalUv;
    output.tangent=float4(mul((float3x3)model,input.tangent.xyz),input.tangent.w*(determinant((float3x3)model)<0?-1:1));
    return output;
}
struct SkinInput {
    float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
    uint4 indices : BLENDINDICES0; float4 weights : BLENDWEIGHT0;
    float2 normalUv:TEXCOORD1;float2 ormUv:TEXCOORD2;float2 emissiveUv:TEXCOORD3;float2 occlusionUv:TEXCOORD4;
    float4 color:COLOR0;
    float4 tangent:TANGENT0;
    float2 coatUv:TEXCOORD5;float2 coatRoughnessUv:TEXCOORD6;float2 coatNormalUv:TEXCOORD7;
};
PixelInput VSSkin(SkinInput input, uint vertexId : SV_VertexID) {
    [loop] for(uint m=0;m<morphCount;m++) {
        float w=morphWeights[m/4][m%4];
        if(w!=0) { MorphDelta d=morphDeltas[m*vertexCount+vertexId];input.position+=d.position*w;input.normal+=d.normal*w;input.tangent.xyz+=d.tangent*w; }
    }
    float4 p=0; float3 n=0;float3 tangent=0;
    [unroll] for(uint i=0;i<4;i++) {
        p += mul(joints[input.indices[i]],float4(input.position,1))*input.weights[i];
        n += mul((float3x3)joints[input.indices[i]],input.normal)*input.weights[i];
        tangent += mul((float3x3)joints[input.indices[i]],input.tangent.xyz)*input.weights[i];
    }
    SkinDelta soft=softDeltas[vertexId];
    VertexInput vertex; vertex.position=p.xyz+soft.position; vertex.normal=normalize(normalize(n)+soft.normal); vertex.uv=input.uv;
    vertex.normalUv=input.normalUv;vertex.ormUv=input.ormUv;vertex.emissiveUv=input.emissiveUv;vertex.occlusionUv=input.occlusionUv;
    vertex.color=input.color;
    vertex.coatUv=input.coatUv;vertex.coatRoughnessUv=input.coatRoughnessUv;vertex.coatNormalUv=input.coatNormalUv;
    vertex.tangent=float4(tangent,input.tangent.w);
    PixelInput output = VSMain(vertex);
    output.meshUv |= 1u;
    return output;
}
#endif

#if defined(PASS_PS)
float2 MappedUv(float2 uv,TextureMapping mapping){if(mapping.samplerIndex==0)return uv;float3 value=float3(uv,1);return float2(dot(mapping.row0,value),dot(mapping.row1,value));}
float4 MappedSample(Texture2D texture,float2 uv,TextureMapping mapping){
    if(mapping.samplerIndex==0)return texture.Sample(materialSampler,uv);
    SamplerState sampler=SamplerDescriptorHeap[mapping.samplerIndex];return texture.Sample(sampler,uv);
}
float3 ApplyNormal(float3 n,float4 frame,float2 uv,TextureMapping mapping,float3 sampled,float3 dpdx,float3 dpdy){
    float3 projected=frame.xyz-n*dot(n,frame.xyz);
    if(abs(frame.w)>.5&&dot(projected,projected)>1e-10){
        float3 t=normalize(projected),b=cross(n,t)*sign(frame.w);float2 r0=float2(1,0),r1=float2(0,1);if(mapping.samplerIndex!=0){r0=mapping.row0.xy;r1=mapping.row1.xy;}float d=r0.x*r1.y-r0.y*r1.x;
        if(abs(d)>1e-10){float3 mt=(r1.y*t-r1.x*b)/d,mb=(-r0.y*t+r0.x*b)/d;t=normalize(mt);b=normalize(cross(n,t))*(dot(cross(n,t),mb)<0?-1:1);return normalize(t*sampled.x+b*sampled.y+n*sampled.z);}
    }else{float2 dx=ddx(uv),dy=ddy(uv);float d=dx.x*dy.y-dx.y*dy.x;if(abs(d)>1e-10){float3 t=(dpdx*dy.y-dpdy*dx.y)/d;t=normalize(t-n*dot(n,t));float3 br=(-dpdx*dy.x+dpdy*dx.x)/d,b=normalize(cross(n,t))*(dot(cross(n,t),br)<0?-1:1);return normalize(t*sampled.x+b*sampled.y+n*sampled.z);}}
    return n;
}
// Shared GGX/Smith/Schlick direct-light model. Roughness follows the IBL convention.
float3 DirectBrdf(float3 n, float3 v, float3 l, float3 albedo, float metal,
                  float rough, float coat, float coatRough, float dielectricF0,float3 coatN) {
    float nl = saturate(dot(n,l)), nv = saturate(dot(n,v));
    float3 h = normalize(l + v + 1e-7);
    float nh = saturate(dot(n,h)), vh = saturate(dot(v,h));
    float a = max(rough * rough, 0.0025), a2 = a*a;
    float d = a2 / max(QPi * pow(nh*nh*(a2-1)+1,2), 1e-8);
    float visibility = 0.5 / max(nl*sqrt(nv*nv*(1-a2)+a2) + nv*sqrt(nl*nl*(1-a2)+a2), 1e-6);
    float3 f0 = lerp(dielectricF0.xxx,albedo,metal);
    float3 f = f0 + (1-f0)*pow(1-vh,5);
    float3 base = (1-f)*(1-metal)*albedo/QPi + f*d*visibility;
    float cnl=saturate(dot(coatN,l)),cnv=saturate(dot(coatN,v)),cnh=saturate(dot(coatN,h));
    float ca=max(coatRough*coatRough,0.0025), ca2=ca*ca;
    float cd=ca2/max(QPi*pow(cnh*cnh*(ca2-1)+1,2),1e-8);
    float cv=0.5/max(cnl*sqrt(cnv*cnv*(1-ca2)+ca2)+cnv*sqrt(cnl*cnl*(1-ca2)+ca2),1e-6);
    float cf=0.04+0.96*pow(1-vh,5);
    return base*(1-coat*cf)*nl+coat*cf*cd*cv*cnl;
}

PixelOutput PSMain(PixelInput input) {
    float3 n = normalize(input.normal);
    float3 baseNormal = normalize(input.materialNormal);
    float3 an = abs(baseNormal);
    float2 planar;
    float3 tangent;
    float3 bitangent;
    if (an.x >= an.y && an.x >= an.z) {
        planar = input.materialPosition.zy;
        tangent = float3(0, 0, 1);
        bitangent = float3(0, 1, 0);
    } else if (an.y >= an.z) {
        planar = input.materialPosition.xz;
        tangent = float3(1, 0, 0);
        bitangent = float3(0, 0, 1);
    } else {
        planar = input.materialPosition.xy;
        tangent = float3(1, 0, 0);
        bitangent = float3(0, 1, 0);
    }
#if defined(PASS_INLINE)
    GpuMaterial material=(GpuMaterial)0;material.uvScale=1;material.opacity=1;material.roughness=roughness;material.metalness=metalness;material.dielectricF0=.04;
#else
    GpuMaterial material = materialRecords[albedoIndex];
#endif
    float2 uv = planar / max(material.uvScale, 0.001);
    if (input.meshUv != 0) uv = input.uv / max(material.uvScale, 0.001);

#if defined(PASS_INLINE)
    ConstantBuffer<QualityConstants> inlineQ=ResourceDescriptorHeap[qualitySlot];Texture2DArray inlineTextures=ResourceDescriptorHeap[inlineQ.slots[14].z];float4 albedoSample=inlineTextures.Sample(materialSampler,float3(uv,albedoIndex));
#else
    Texture2D albedoTexture = ResourceDescriptorHeap[material.albedo];
    float4 albedoSample = MappedSample(albedoTexture,MappedUv(uv,material.textureMappings[0]),material.textureMappings[0]);
#endif
    albedoSample *= input.color;
    albedoSample.a *= material.opacity * baseColor.a;
    if ((material.flags & 1u) != 0) clip(albedoSample.a - material.alphaCutoff);
    float3 albedo = albedoSample.rgb * baseColor.rgb;
    float alpha=(material.flags&16u)!=0?saturate(albedoSample.a):1;
    if((material.flags&8u)!=0){PixelOutput unlit;unlit.surface=float4(albedo,alpha);unlit.keyLight=float4(0,0,0,alpha);return unlit;}
#if defined(PASS_INLINE)
    float wet = material.wetness;
#else
    // The bindless opaque path reuses ormIndex as a per-entity wetness override. The
    // sentinel keeps the authored material value for everything else.
    float wet = ormIndex == 0xFFFFFFFFu ? material.wetness : saturate(asfloat(ormIndex));
#endif
    float wear = material.wear;

    uint normalSlot = material.normal & 0x7FFFFFFFu;
    Texture2D normalTexture = ResourceDescriptorHeap[normalSlot];
    float2 normalUv=MappedUv((input.meshUv&2u)?input.normalUv/max(material.uvScale,.001):uv,material.textureMappings[1]);
#if defined(PASS_INLINE)
    float3 sampledNormal=float3(0,0,1);
#else
    float3 sampledNormal = MappedSample(normalTexture,normalUv,material.textureMappings[1]).xyz * 2.0 - 1.0;
#endif
    sampledNormal.z = sqrt(saturate(1.0 - dot(sampledNormal.xy, sampledNormal.xy)));
    n=ApplyNormal(n,input.tangent,normalUv,material.textureMappings[1],sampledNormal,ddx(input.worldPosition),ddy(input.worldPosition));

#if defined(PASS_INLINE)
    float4 orm=float4(1,1,1,0);
#else
    Texture2D ormTexture = ResourceDescriptorHeap[material.orm];
    float4 orm = MappedSample(ormTexture,MappedUv((input.meshUv&2u)?input.ormUv/max(material.uvScale,.001):uv,material.textureMappings[2]),material.textureMappings[2]);
#endif
    float ao = saturate(orm.r);
    if((material.flags & 4u)!=0){Texture2D occlusionTexture=ResourceDescriptorHeap[material.occlusion];float value=MappedSample(occlusionTexture,MappedUv((input.meshUv&2u)?input.occlusionUv/max(material.uvScale,.001):uv,material.textureMappings[4]),material.textureMappings[4]).r;ao=lerp(1,saturate(value),material.occlusionStrength);}
    float rough = saturate(orm.g * material.roughness);
    float metal = saturate(orm.b * material.metalness);
    float emissive = orm.a;
    float3 emission;
    if((material.flags & 2u) != 0){
        Texture2D emissiveTexture = ResourceDescriptorHeap[material.emissive];
        emission = MappedSample(emissiveTexture,MappedUv((input.meshUv&2u)?input.emissiveUv/max(material.uvScale,.001):uv,material.textureMappings[3]),material.textureMappings[3]).rgb * material.emissiveFactor * material.emissiveStrength;
    }

    float3 view = normalize(cameraPosition.xyz - input.worldPosition);
    float reliefShadow = 1.0;
#if !defined(PASS_INLINE)
    if(material.layerCount>0){
        LayerCoordinates coords=(LayerCoordinates)0;
        // Frozen first-version UV set map: set 0 is UV0 (or the built-in planar
        // projection), sets 1-4 are the four extra texture UV streams and sets
        // 5-7 the three extra layer UV streams. The loader bounds the index and
        // the draw path rejects streams the mesh does not actually carry.
        coords.uv[0]=input.meshUv!=0?input.uv:planar;
        coords.uv[1]=input.normalUv;coords.uv[2]=input.ormUv;coords.uv[3]=input.emissiveUv;coords.uv[4]=input.occlusionUv;
        coords.uv[5]=input.coatUv;coords.uv[6]=input.coatRoughnessUv;coords.uv[7]=input.coatNormalUv;
        [unroll] for(uint set=0;set<8;++set){coords.dx[set]=ddx(coords.uv[set]);coords.dy[set]=ddy(coords.uv[set]);}
        coords.normal=normalize(input.normal);coords.positionDx=ddx(input.worldPosition);coords.positionDy=ddy(input.worldPosition);coords.vertexColor=input.color;
        StructuredBuffer<LayerMaterial> layers=ResourceDescriptorHeap[material.layerDescriptor];
        SurfaceReliefMaterial relief=material.surfaceRelief;
        float3 reliefInfo=float3(0,0,0);
        if((relief.flags&3u)==1u&&relief.amplitudeMetres>0&&relief.heightLayer<material.layerCount){
            LayerMaterial heightLayer=layers[material.layerOffset+relief.heightLayer];
            ReliefApply(coords,relief,heightLayer,view,length(cameraPosition.xyz-input.worldPosition),reliefInfo);
            reliefShadow=ReliefSelfShadow(relief,heightLayer,coords,normalize(keyDirectionIntensity.xyz));
        }
        LayerSurface layered=LayerSampleSurface(layers[material.layerOffset],coords);
        // Preview-only debug views never persist in the scene.
        uint debugMode=layerDebugMode,debugLayer=layerDebugLayer;
        LayerSurface selected=layered;float selectedCoverage=1.0;
        [loop] for(uint i=1;i<min(material.layerCount,4u);++i){
            LayerMaterial lm=layers[material.layerOffset+i];
            float raw=lm.source==1u?LayerSample(lm.slots[8],coords,0)[lm.slots[8].channel]:lm.source==2u?coords.vertexColor[lm.slots[8].channel]:1.0;
            float coverage=((lm.flags&4u)!=0||lm.weight==0)?0.0:lm.weight*saturate(((lm.flags&1u)!=0?1.0-raw:raw)*lm.gain+lm.bias);
            if(coverage>0.0){
                LayerSurface top=LayerSampleSurface(lm,coords);
                if((lm.flags&2u)!=0)coverage=LayerHeightCoverage(coverage,layered.height,top.height,lm.width,lm.heightBias);
                if(coverage>0.0)layered=LayerComposite(layered,top,coverage);
                if(i==debugLayer){selected=top;selectedCoverage=coverage;}
            }else if(debugMode!=0u&&i==debugLayer){
                selected=LayerSampleSurface(lm,coords);selectedCoverage=0.0;
            }
        }
        if(debugMode!=0u){
            float3 value=debugMode==6u?reliefInfo.z.xxx:debugMode==5u?reliefInfo.y.xxx:debugMode==4u?reliefInfo.x.xxx:debugMode==2u?saturate(layered.height*0.5+0.5).xxx:debugMode==1u?selectedCoverage.xxx:selected.albedo;
            PixelOutput debug;debug.surface=float4(value,alpha);debug.keyLight=float4(0,0,0,alpha);return debug;
        }
        albedo=layered.albedo*baseColor.rgb; n=layered.normal; rough=layered.roughness; metal=layered.metallic; ao=layered.ao; emission=layered.emissive;
    }
#endif

    // Surface history: wetness darkens/glosses, wear exposes metal and roughens.
    albedo *= lerp(1.0, 0.62, wet);
    rough = lerp(rough, 0.12, wet * 0.85);
    metal = saturate(metal + wear * 0.40);
    rough = lerp(rough, 0.30, wear * 0.55);
    albedo = lerp(albedo, albedo * 0.72 + float3(0.20, 0.14, 0.09), wear * 0.30);

    float3 specColor = lerp(material.dielectricF0.xxx, albedo, metal);
    float clearcoat = saturate(material.clearcoat);
    float clearcoatRough = saturate(material.clearcoatRoughness);
    float layerScale=max(material.uvScale,.001);
    if((material.flags&32u)!=0){Texture2D tex=ResourceDescriptorHeap[material.coat];clearcoat*=MappedSample(tex,MappedUv((input.meshUv&4u)?input.coatUv/layerScale:uv,material.textureMappings[5]),material.textureMappings[5]).r;}
    if((material.flags&64u)!=0){Texture2D tex=ResourceDescriptorHeap[material.coatRoughness];clearcoatRough*=MappedSample(tex,MappedUv((input.meshUv&4u)?input.coatRoughnessUv/layerScale:uv,material.textureMappings[6]),material.textureMappings[6]).g;}
    float3 coatN=normalize(input.normal);
    if((material.flags&128u)!=0){Texture2D tex=ResourceDescriptorHeap[material.coatNormal&0x7FFFFFFFu];float2 cuv=MappedUv((input.meshUv&4u)?input.coatNormalUv/layerScale:uv,material.textureMappings[7]);float3 cn=MappedSample(tex,cuv,material.textureMappings[7]).xyz*2-1;cn.z=sqrt(saturate(1-dot(cn.xy,cn.xy)));coatN=ApplyNormal(coatN,input.tangent,cuv,material.textureMappings[7],cn,ddx(input.worldPosition),ddy(input.worldPosition));}
    float clearcoatShininess = lerp(160.0, 8.0, clearcoatRough);
    float coatFresnel=clearcoat*(.04+.96*pow(1-saturate(abs(dot(coatN,view))),5));
    float shininess = lerp(160.0, 6.0, rough);

    // Night ambient is deliberately tiny; the 64-light buffer supplies the street.
    float diffuseWeight = (1.0-material.dielectricF0)*(1.0-metal);
    float3 surface = albedo * ambient.rgb * 0.75 * ao * diffuseWeight;
    surface += albedo * float3(0.012, 0.016, 0.026) * ao * diffuseWeight;
    if((material.flags & 2u) == 0
#if !defined(PASS_INLINE)
       && material.layerCount==0
#endif
       ) emission = albedo * emissive * 2.0;
    surface += emission;
    surface*=1-coatFresnel;
    if (qualitySlot != 0) {
        ConstantBuffer<QualityConstants> q = ResourceDescriptorHeap[qualitySlot];
        float4 gi = QIrradiance(q, input.worldPosition, n);
        if ((q.slots[9].w & 1) != 0) {
            surface = albedo * (QDiffuseSky(q,n) * gi.a + gi.rgb) * diffuseWeight * ao / QPi;
            surface=(surface+emission+QEnvironmentSpecular(q,n,view,max(rough,0.04),specColor,materialSampler)*ao*gi.a)*(1-coatFresnel);
            if (clearcoat > 0.0) {
                surface += QEnvironmentSpecular(q,coatN,view,max(clearcoatRough,0.02),float3(0.04,0.04,0.04),materialSampler)
                           * ao * gi.a * clearcoat;
            }
        } else {
            surface += albedo * gi.rgb * diffuseWeight * ao / QPi*(1-coatFresnel);
        }
    }

    uint lightCount = min(gpuLightCount, 64u);
    for (uint i = 0; i < lightCount; ++i) {
        GpuLight light = lights[i];
        float3 toLight = light.positionRange.xyz - input.worldPosition;
        float distanceToLight = length(toLight);
        float range = max(light.positionRange.w, 0.001);
        float attenuation = saturate(1.0 - distanceToLight / range);
        attenuation *= attenuation;
        float3 lightDirection = toLight / max(distanceToLight, 0.001);
        float cone = 1.0;
        if (light.params.x > 0.5) {
            float cosAngle = dot(-lightDirection, normalize(light.directionCos.xyz));
            float outer = light.directionCos.w;
            if (cosAngle < outer) continue;
            cone = saturate((cosAngle - outer) / max(light.params.y - outer, 0.001));
        }
        float ndotl = saturate(dot(n, lightDirection));
        float specular = pow(saturate(dot(n, normalize(lightDirection + view))), shininess);
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation * cone;
        radiance *= reliefShadow;
        if (qualitySlot != 0 && any(radiance > 0.0001)) {
            ConstantBuffer<QualityConstants> q = ResourceDescriptorHeap[qualitySlot];
            float shadowWeight = q.lights[i].shadow.x;
            if (shadowWeight > 0) radiance *= lerp(1.0, QLocalVisibility(q,i,
                input.worldPosition+n*0.02,normalize(input.normal)),shadowWeight);
        }
        surface += DirectBrdf(n,view,lightDirection,albedo,metal,max(rough,0.04),clearcoat,max(clearcoatRough,0.02),material.dielectricF0,coatN)*radiance;
    }

    float3 keyDirection = normalize(keyDirectionIntensity.xyz);
    float keyIntensity = max(keyDirectionIntensity.w, 0.0);
    float keyDiffuse = saturate(dot(n, keyDirection));
    float keySpec = pow(saturate(dot(n, normalize(keyDirection + view))), shininess);
    float keyCoat = pow(saturate(dot(n, normalize(keyDirection + view))), clearcoatShininess);
    float3 keyRadiance = keyColor.rgb * keyIntensity * reliefShadow;
    if (qualitySlot != 0) {
        ConstantBuffer<QualityConstants> q = ResourceDescriptorHeap[qualitySlot];
        keyRadiance *= QCloudVisibility(q,input.worldPosition,materialSampler);
    }

    PixelOutput output;
    // The accumulation target is FP16. Keep very strong authored emission finite
    // before the composite's exposure/tone mapping, avoiding inf/inf in Reinhard.
    surface=min(surface,65504.0);
    output.surface = float4(surface, alpha);
    output.keyLight = float4(min(DirectBrdf(n,view,keyDirection,albedo,metal,max(rough,0.04),clearcoat,max(clearcoatRough,0.02),material.dielectricF0,coatN)*keyRadiance,65504.0),alpha);
    return output;
}
[earlydepthstencil]
void PSCapture(PixelInput input,uint primitive:SV_PrimitiveID){
    PixelOutput shaded=PSMain(input);float alpha=shaded.surface.a;if(alpha<=0)return;
    uint2 pixel=uint2(input.position.xy);
    RWTexture2D<uint> coverage=ResourceDescriptorHeap[transparencyBase+4];
    RWByteAddressBuffer counter=ResourceDescriptorHeap[transparencyBase+2];
    uint layer;InterlockedAdd(coverage[pixel],1,layer);
    if(layer>=TransparencyMaxLayers){uint old;counter.InterlockedOr(4,2,old);return;}
    uint index;counter.InterlockedAdd(0,1,index);
    if(index>=(transparencyCapacity&0x7fffffffu)){uint old;counter.InterlockedOr(4,1,old);return;}
    float visibility=1;
    if((transparencyCapacity&0x80000000u)!=0&&any(shaded.keyLight.rgb>0)){
        RaytracingAccelerationStructure scene=ResourceDescriptorHeap[transparencyBase+3];
        RayDesc ray;ray.Origin=input.worldPosition+normalize(input.normal)*.02;ray.Direction=normalize(keyDirectionIntensity.xyz);ray.TMin=.005;ray.TMax=200;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(scene,0,7,ray);while(query.Proceed()){}
        visibility=query.CommittedStatus()==COMMITTED_NOTHING?1:0;
    }
    RWTexture2D<uint> heads=ResourceDescriptorHeap[transparencyBase];
    RWStructuredBuffer<TransparencyNode> nodes=ResourceDescriptorHeap[transparencyBase+1];
    uint previous;InterlockedExchange(heads[pixel],index,previous);
    TransparencyNode node;node.color=float4(shaded.surface.rgb+shaded.keyLight.rgb*visibility,alpha);node.depth=input.position.z;node.next=previous;node.draw=ormIndex;node.primitive=primitive;nodes[index]=node;
}
#endif
