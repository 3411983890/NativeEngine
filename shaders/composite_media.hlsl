#include "smoke_medium.hlsli"

struct CompositeVertex {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Texture2D<float4> baseColor : register(t0);
Texture2D<float4> keyLight : register(t1);
Texture2D<float> shadowVisibility : register(t2);
Texture2D<float> sceneDepth : register(t3);
ByteAddressBuffer smokeFields[8]:register(t6);
struct TransparencyNode {float4 color;float depth;uint next;uint draw;uint primitive;};
Texture2D<uint> transparencyHeads : register(t4);
StructuredBuffer<TransparencyNode> transparencyNodes : register(t5);
bool TransparencyBefore(TransparencyNode a,TransparencyNode b){return a.depth<b.depth||(a.depth==b.depth&&(a.draw<b.draw||(a.draw==b.draw&&a.primitive<b.primitive)));}
bool TransparencyNext(uint head,float opaqueDepth,bool hasPrevious,TransparencyNode previous,out TransparencyNode result){
    bool found=false;result=(TransparencyNode)0;
    [loop]for(uint i=0;i<64&&head!=0xffffffffu;i++){TransparencyNode n=transparencyNodes[head];head=n.next;if(n.depth<=opaqueDepth&&(!hasPrevious||TransparencyBefore(previous,n))&&(!found||TransparencyBefore(n,result))){result=n;found=true;}}
    return found;
}
cbuffer CompositeConstants : register(b0) {
    uint debugView;
    uint toneMap;
    float exposure;
    uint volumetric;
};
struct CompositeLight {
    float4 positionRange;
    float4 colorIntensity;
    float4 directionCos;
    float4 params; // x = type (0 point, 1 spot), y = cosInner
};
cbuffer CompositeScene : register(b1) {
    column_major float4x4 invViewProjection;
    float4 cameraPosition;
    float4 fogParams;      // x density, y heightFalloff, z baseHeight, w maxDistance
    float4 fogColor;       // rgb color, w ambient scale
    float4 moonDirection;  // xyz direction to moon, w intensity
    float4 moonColor;
    float4 cloudParams;    // x time, y coverage, z height, w thickness
    CompositeLight fogLights[8];
    uint fogLightCount;
    float3 fogLightPadding;
    uint4 smokeInfo;SmokeVolume smokeVolumes[8];
};

float hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float noise3(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + float3(0, 0, 0));
    float n100 = hash13(i + float3(1, 0, 0));
    float n010 = hash13(i + float3(0, 1, 0));
    float n110 = hash13(i + float3(1, 1, 0));
    float n001 = hash13(i + float3(0, 0, 1));
    float n101 = hash13(i + float3(1, 0, 1));
    float n011 = hash13(i + float3(0, 1, 1));
    float n111 = hash13(i + float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}
float fbm3(float3 p) {
    return 0.55 * noise3(p) + 0.30 * noise3(p * 2.03) + 0.15 * noise3(p * 4.01);
}

float3 SkyColor(float3 direction) {
    float up = saturate(direction.y);
    float3 color = lerp(float3(0.004, 0.007, 0.016), float3(0.018, 0.030, 0.060), up);
    // Stars: a sparse hash on the direction, faded near the horizon.
    float star = step(0.9975, hash13(floor(direction * 900.0)));
    color += star * smoothstep(0.08, 0.35, direction.y) * float3(0.65, 0.72, 0.9);
    // Moon disk.
    float moonDot = dot(direction, normalize(moonDirection.xyz));
    float moon = smoothstep(0.9994, 0.99985, moonDot);
    color += moonColor.rgb * moon * 6.0;
    color += moonColor.rgb * smoothstep(0.995, 0.9994, moonDot) * 0.05;

    if (direction.y > 0.02) {
        float t0 = (cloudParams.z - cameraPosition.y) / direction.y;
        float t1 = (cloudParams.z + cloudParams.w - cameraPosition.y) / direction.y;
        float tMax = min(t1, 450.0);
        if (tMax > t0) {
            const int steps = 14;
            float dt = (tMax - t0) / float(steps);
            float transmittance = 1.0;
            float3 scattered = 0.0;
            for (int i = 0; i < steps; ++i) {
                float3 p = cameraPosition.xyz + direction * (t0 + (float(i) + 0.5) * dt);
                float base = fbm3(p * 0.012 + float3(cloudParams.x * 0.01, 0, 0));
                float density = saturate((base - (1.0 - cloudParams.y)) * 3.0);
                float layer = saturate((p.y - cloudParams.z) / max(cloudParams.w, 1.0));
                density *= smoothstep(0.0, 0.2, layer) * smoothstep(1.0, 0.75, layer);
                float alpha = 1.0 - exp(-density * dt * 0.05);
                float3 lit = moonColor.rgb * 1.8 + float3(0.22, 0.24, 0.30);
                scattered += transmittance * alpha * lit;
                transmittance *= (1.0 - alpha);
            }
            color = color * transmittance + scattered;
        }
    }
    return color;
}

float3 ApplyLegacyFog(float3 color, float3 world, float3 direction) {
    float distance = length(world - cameraPosition.xyz);
    if (fogParams.w > 0.0) distance = min(distance, fogParams.w);
    const int steps = 12;
    float dt = distance / float(steps);
    float transmittance = 1.0;
    float3 scattered = 0.0;
    float3 moonDir = normalize(moonDirection.xyz);
    float phase = 0.5 + 0.5 * dot(direction, -moonDir);
    for (int i = 0; i < steps; ++i) {
        float3 p = cameraPosition.xyz + direction * ((float(i) + 0.5) * dt);
        float density = fogParams.x * exp(-max(0.0, p.y - fogParams.z) * fogParams.y);
        float stepTransmittance = exp(-density * dt);
        float3 radiance = fogColor.rgb * fogColor.w +
                          moonColor.rgb * moonDirection.w * phase * 0.35;
        // Headlights scatter in the fog: this is what turns a lit car into visible beams.
        uint lightCount = min(fogLightCount, 8u);
        for (uint l = 0; l < lightCount; ++l) {
            CompositeLight light = fogLights[l];
            float3 toLight = light.positionRange.xyz - p;
            float d = length(toLight);
            float range = min(max(light.positionRange.w, 0.001), 18.0);
            if (d >= range) continue;
            float attenuation = saturate(1.0 - d / range);
            attenuation *= attenuation;
            float3 lightToSurface = -toLight / max(d, 0.001);
            float cone = 1.0;
            if (light.params.x > 0.5) {
                float cosAngle = dot(lightToSurface, normalize(light.directionCos.xyz));
                float outer = light.directionCos.w;
                if (cosAngle < outer) continue;
                cone = saturate((cosAngle - outer) /
                                max(light.params.y - outer, 0.001));
            }
            float forward = pow(saturate(dot(lightToSurface,
                                              normalize(light.directionCos.xyz))), 6.0);
            float lightPhase = 0.15 + 0.85 * forward;
            radiance += light.colorIntensity.rgb * light.colorIntensity.w *
                        attenuation * cone * lightPhase * 0.04;
        }
        scattered += transmittance * (1.0 - stepTransmittance) * radiance;
        transmittance *= stepTransmittance;
    }
    return color * transmittance + scattered;
}

float3 MediumLight(float3 p,float3 direction){
    float3 moonDir=normalize(moonDirection.xyz);float phase=.5+.5*dot(direction,-moonDir);
    float3 light=fogColor.rgb*fogColor.w+moonColor.rgb*moonDirection.w*phase*.35;
    [loop]for(uint i=0;i<min(fogLightCount,8u);i++){CompositeLight l=fogLights[i];float3 delta=l.positionRange.xyz-p;float distance=length(delta),range=min(max(l.positionRange.w,.001),18.0);if(distance>=range)continue;float attenuation=saturate(1-distance/range);float3 toward=-delta/max(distance,.001);float cone=l.params.x>.5?saturate((dot(toward,normalize(l.directionCos.xyz))-l.directionCos.w)/max(l.params.y-l.directionCos.w,.001)):1;float phase=.15+.85*pow(saturate(dot(toward,normalize(l.directionCos.xyz))),6);light+=l.colorIntensity.rgb*l.colorIntensity.w*attenuation*attenuation*cone*phase*.04;}
    return light;
}
float3 ApplyFog(float3 color,float3 world,float3 direction){
    if(smokeInfo.x==0)return (volumetric&1u)?ApplyLegacyFog(color,world,direction):color;
    float limit=length(world-cameraPosition.xyz);float edges[20];uint count=2;edges[0]=0;edges[1]=limit;
    if((volumetric&1u)&&fogParams.w>0&&fogParams.w<limit)edges[count++]=fogParams.w;
    [loop]for(uint i=0;i<smokeInfo.x;i++){float a,b;if(SmokeInterval(smokeVolumes[i],cameraPosition.xyz,direction,limit,a,b)){edges[count++]=a;edges[count++]=b;}}
    [loop]for(uint edge=1;edge<count;edge++){float value=edges[edge];uint j=edge;[loop]while(j>0&&edges[j-1]>value){edges[j]=edges[j-1];j--;}edges[j]=value;}
    float3 radiance=0;float transmittance=1;
    [loop]for(uint segment=1;segment<count;segment++){
        float a=edges[segment-1],b=edges[segment],length=b-a;if(length<1e-6)continue;float cell=1e9;float3 mid=cameraPosition.xyz+direction*((a+b)*.5);
        [loop]for(uint i=0;i<smokeInfo.x;i++){float3 grid=(mid-smokeVolumes[i].originCell.xyz)/smokeVolumes[i].originCell.w;if(all(grid>=0)&&all(grid<float3(smokeVolumes[i].dimensions.xyz)))cell=min(cell,smokeVolumes[i].originCell.w);}
        uint steps=cell<1e8?min(64u,max(1u,uint(ceil(length/(cell*.5))))):max(1u,uint(ceil(12*length/max(fogParams.w,limit))));float ds=length/steps;
        [loop]for(uint sample=0;sample<steps;sample++){
            float distance=a+(sample+.5)*ds;float3 p=cameraPosition.xyz+direction*distance;
            float sigma=((volumetric&1u)&&(fogParams.w<=0||distance<fogParams.w))?fogParams.x*exp(-max(0,p.y-fogParams.z)*fogParams.y):0;
            float3 scattering=sigma,emission=0;[loop]for(uint i=0;i<smokeInfo.x;i++)SmokeAccumulate(smokeFields[i],smokeVolumes[i],p,sigma,scattering,emission);
            float stepT=exp(-sigma*ds);float integral=sigma>1e-6?(1-stepT)/sigma:ds;
            radiance+=transmittance*(scattering*MediumLight(p,direction)+emission)*integral;transmittance*=stepT;
        }
    }return color*transmittance+radiance;
}

CompositeVertex VSMain(uint vertexId : SV_VertexID) {
    CompositeVertex output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
float4 PSMain(CompositeVertex input) : SV_Target {
    uint2 pixel = uint2(input.position.xy);
    float4 base = baseColor.Load(int3(pixel, 0));
    float4 key = keyLight.Load(int3(pixel, 0));
    float visibility = shadowVisibility.Load(int3(pixel, 0));
    // Diagnostic views used by the DXR shadow evidence: the mask itself, the isolated
    // key-light term, and the ambient surface term.
    if (debugView == 1) { return float4(visibility.xxx, 1.0); }
    if (debugView == 2) { return float4(key.rgb, 1.0); }
    if (debugView == 3) { return float4(base.rgb, 1.0); }
    if (debugView == 4) { return float4(1.0, 0.0, 1.0, 1.0); }
    float3 color = base.rgb + key.rgb * saturate(visibility);
    if ((volumetric&1u) != 0 && (volumetric&4u)==0) {
        float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
        float4 nearWorld = mul(invViewProjection, float4(ndc, 0.0, 1.0));
        float4 farWorld = mul(invViewProjection, float4(ndc, 1.0, 1.0));
        nearWorld /= nearWorld.w;
        farWorld /= farWorld.w;
        float3 direction = normalize(farWorld.xyz - nearWorld.xyz);
        float depth = sceneDepth.Load(int3(pixel, 0));
        if (depth >= 1.0) {
            color = SkyColor(direction);
        } else {
            float4 world = mul(invViewProjection, float4(ndc, depth, 1.0));
            world /= world.w;
            color = ApplyFog(color, world.xyz, direction);
        }
    }
    if((volumetric&4u)!=0){
        float2 ndc=input.uv*float2(2,-2)+float2(-1,1);float4 far=mul(invViewProjection,float4(ndc,1,1));float3 direction=normalize(far.xyz/far.w-cameraPosition.xyz);float depth=sceneDepth.Load(int3(pixel,0));float3 world;
        if(depth<1){float4 hitWorld=mul(invViewProjection,float4(ndc,depth,1));world=hitWorld.xyz/hitWorld.w;}else{float limit=max(fogParams.w,96.0);[loop]for(uint i=0;i<smokeInfo.x;i++){float a,b;if(SmokeInterval(smokeVolumes[i],cameraPosition.xyz,direction,4096,a,b))limit=max(limit,b);}world=cameraPosition.xyz+direction*limit;}
        // The legacy branch above already applied atmospheric fog; rebuild the
        // unattenuated surface/sky before the single combined medium integral.
        color=(depth>=1&&(volumetric&1u))?SkyColor(direction):base.rgb+key.rgb*saturate(visibility);color=ApplyFog(color,world,direction);
    }
    if((volumetric&2u)!=0){
        if(toneMap==0&&(volumetric&5u)==0)color=base.rgb+key.rgb*saturate(visibility);
        uint head=transparencyHeads.Load(int3(pixel,0));float opaqueDepth=sceneDepth.Load(int3(pixel,0));
        float3 accumulated=0;float transmittance=1;bool hasPrevious=false;TransparencyNode previous=(TransparencyNode)0;
        [loop]for(uint layer=0;layer<64;layer++){
            TransparencyNode node;if(!TransparencyNext(head,opaqueDepth,hasPrevious,previous,node))break;previous=node;hasPrevious=true;
            float3 surface=node.color.rgb;
            if((volumetric&5u)!=0){float2 ndc=input.uv*float2(2,-2)+float2(-1,1);float4 world=mul(invViewProjection,float4(ndc,node.depth,1));world/=world.w;surface=ApplyFog(surface,world.xyz,normalize(world.xyz-cameraPosition.xyz));}
            accumulated+=surface*node.color.a*transmittance;transmittance*=1-node.color.a;if(transmittance==0)break;
        }
        color=accumulated+color*transmittance;
    }
    if (toneMap != 0) {
        // Cooked albedo is sRGB-decoded to linear; encode back to sRGB for display. A
        // plain gamma keeps saturation and shadow contrast, which ACES washed out.
        color = pow(saturate(color * exposure), 1.0 / 2.2);
    }
    // Forcing visibility to 1.0 reproduces the 0.3.0 raster image exactly.
    return float4(toneMap != 0 ? color : ((volumetric&7u)!=0?saturate(color):saturate(base.rgb + key.rgb * saturate(visibility))), base.a);
}
