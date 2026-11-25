#include "lighting_quality_common.hlsli"
#include "transparency_common.hlsli"
ConstantBuffer<QualityConstants> quality : register(b0);
cbuffer DispatchConstants : register(b1) { uint dispatchIndex; uint3 dispatchPadding; };
SamplerState linearClamp : register(s0);
SamplerState linearWrap : register(s1);
#define Q quality

float PeriodicHash(float3 p,float period) { return QHash(p-floor(p/period)*period); }
float Noise(float3 p,float period) {
    float3 i=floor(p),f=frac(p); f=f*f*(3-2*f);
    return lerp(lerp(lerp(PeriodicHash(i,period),PeriodicHash(i+float3(1,0,0),period),f.x),
        lerp(PeriodicHash(i+float3(0,1,0),period),PeriodicHash(i+float3(1,1,0),period),f.x),f.y),
        lerp(lerp(PeriodicHash(i+float3(0,0,1),period),PeriodicHash(i+float3(1,0,1),period),f.x),
        lerp(PeriodicHash(i+float3(0,1,1),period),PeriodicHash(i+1,period),f.x),f.y),f.z);
}
[numthreads(4,4,4)] void CSNoise(uint3 id:SV_DispatchThreadID) {
    if(any(id>=64))return;
    RWTexture3D<float4> noise=ResourceDescriptorHeap[Q.slots[4].y];
    float3 p=(float3(id)+0.5)/8;
    float base=Noise(p,8)*0.57+Noise(p*2,16)*0.28+Noise(p*4,32)*0.15;
    noise[id]=float4(base,Noise(p*3,24),Noise(p*7,56),1);
    if(id.z==0) {
        RWTexture2D<float> weather=ResourceDescriptorHeap[Q.slots[4].w];
        weather[id.xy]=Noise(float3((float2(id.xy)+0.5)/16,3.5),4);
    }
}
[numthreads(8,8,1)] void CSIbl(uint3 id:SV_DispatchThreadID) {
    uint size=max(64u>>dispatchIndex,1u); if(any(id.xy>=size)||id.z>=6)return;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[Q.slots[9].x+dispatchIndex];
    float3 n=QCubeDirection(id.z,(float2(id.xy)+0.5)/size),sum=0;float weight=0;
    float rough=float(dispatchIndex)/6;
    [loop]for(uint i=0;i<64;i++) {
        float3 h=QGGX(QHammersley(i,64),rough,n),l=normalize(2*dot(n,h)*h-n);
        float w=saturate(dot(n,l));sum+=QSky(Q,l)*w;weight+=w;
    }
    output[id]=float4(sum/max(weight,0.001),1);
}
[numthreads(8,8,1)] void CSBrdf(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=64))return;
    RWTexture2D<float2> output=ResourceDescriptorHeap[Q.slots[9].y];
    float nv=(id.x+0.5)/64,rough=(id.y+0.5)/64;
    float3 v=float3(sqrt(1-nv*nv),0,nv); float2 result=0;
    [loop]for(uint i=0;i<64;i++) {
        float3 h=QGGX(QHammersley(i,64),rough,float3(0,0,1)),l=normalize(2*dot(v,h)*h-v);
        float nl=saturate(l.z),nh=saturate(h.z),vh=saturate(dot(v,h));
        if(nl>0) {
            float k=rough*rough*0.5;
            float g=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);
            float visible=g*vh/max(nh*nv,0.001),f=pow(1-vh,5);
            result+=float2((1-f)*visible,f*visible);
        }
    }
    output[id.xy]=result/64;
}
[numthreads(8,8,1)] void CSReset(uint3 id:SV_DispatchThreadID) {
    if(id.x>=128||id.y>=1024)return;
    [unroll]for(uint i=0;i<2;i++) {
        RWTexture2D<float4> irr=ResourceDescriptorHeap[384+16+i*6+3];
        RWTexture2D<float4> distance=ResourceDescriptorHeap[384+16+i*6+4];
        irr[id.xy]=float4(0,0,0,1);distance[id.xy]=float4(64,4096,0,0);
        if(id.x<16&&id.y<128) {
            RWTexture3D<float4> data=ResourceDescriptorHeap[384+16+i*6+5];
            data[uint3(id.x,id.y%8,id.y/8)]=0;
        }
    }
}
groupshared float3 probeRadiance[64];
groupshared float3 probeDirections[64];
groupshared float probeDistances[64];
groupshared float probeSky[64];
groupshared float4 probeState;
float3 ProbeHitRadiance(uint instanceId,float3 p,float3 rayDirection,out float3 n) {
    StructuredBuffer<QualityInstance> instances=ResourceDescriptorHeap[Q.slots[0].y];
    QualityInstance instance=instances[instanceId];
    float3 local=mul(instance.inverseModel,float4(p,1)).xyz;
    float3 a=abs(local),ln=0;
    if(a.x>=a.y&&a.x>=a.z)ln.x=local.x>=0?1:-1;
    else if(a.y>=a.z)ln.y=local.y>=0?1:-1;
    else ln.z=local.z>=0?1:-1;
    n=normalize(mul(ln,(float3x3)instance.inverseModel));
    if(dot(n,rayDirection)>0)return 0; // Inside probes relocate before doing hit lighting.
    float3 albedo=instance.color.rgb;float emissive=0,metal=instance.material.y;
    if(instance.textures.w!=0) {
        Texture2D<float4> base=ResourceDescriptorHeap[instance.textures.x];
        Texture2D<float4> orm=ResourceDescriptorHeap[instance.textures.y];
        float3 an=abs(n);float2 planar=an.x>=an.y&&an.x>=an.z?p.zy:an.y>=an.z?p.xz:p.xy;
        float2 uv=planar/max(instance.material.z,0.001);
        albedo*=base.SampleLevel(linearWrap,uv,2).rgb;
        float4 material=orm.SampleLevel(linearWrap,uv,2);emissive=material.a*2;metal*=material.b;
        float wet=instance.material.w,wear=float(instance.textures.w>>8)/255;
        albedo*=lerp(1,0.62,wet);metal=saturate(metal+wear*0.4);
        albedo=lerp(albedo,albedo*0.72+float3(0.20,0.14,0.09),wear*0.30);
    }
    float3 origin=p+n*0.025;
    float3 lighting=0;
    if(any(Q.skyExposure.rgb>0.0001)) {
        // One cosine-weighted sky visibility sample at the secondary surface prevents
        // unoccluded sky SH from becoming light leaking through an indoor bounce.
        float2 xi=float2(QHash(p+float(Q.grid.w)*0.013),QHash(p.zyx+17.3));
        float3 skyDirection=QGGX(xi,1,n);
        lighting=QSky(Q,skyDirection)*QPi*QVisibility(Q,origin,skyDirection,64);
    }
    float sun=QVisibility(Q,origin,Q.keyDirectionIntensity.xyz,200)*QCloudVisibility(Q,p,linearClamp);
    lighting+=Q.keyColor.rgb*Q.keyDirectionIntensity.w*saturate(dot(n,Q.keyDirectionIntensity.xyz))*sun;
    [loop]for(uint i=0;i<Q.slots[10].z;i++) {
        float3 direction;float distance;
        float3 radiance=QLocalRadiance(Q.lights[i],p,direction,distance);
        float ndotl=saturate(dot(n,direction));
        if(ndotl>0&&any(radiance>0.0001))
            lighting+=radiance*ndotl*QLocalVisibility(Q,i,origin,n);
    }
    return albedo*(lighting*(1-metal)*0.318309886+emissive);
}
[numthreads(64,1,1)] void CSProbe(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint index=Q.slots[7].w+group.x; if(index>=2048)return;
    uint3 cell=uint3(index%16,(index/16)%8,index/128);
    Texture3D<float4> oldData=ResourceDescriptorHeap[Q.slots[2].y];
    if(lane==0)probeState=oldData.Load(int4(cell,0));
    GroupMemoryBarrierWithGroupSync();
    float3 origin=Q.probeOriginSpacing.xyz+float3(cell)*Q.probeOriginSpacing.w+probeState.xyz;
    float3 direction;
    if(lane<6) {direction=0;direction[lane/2]=(lane&1)?-1:1;}
    else {
        float z=1-2*(float(lane-6)+0.5)/58;
        float phi=(float(lane-6)*2.39996323)+float(Q.grid.w%64)*0.618;
        direction=float3(cos(phi)*sqrt(1-z*z),z,sin(phi)*sqrt(1-z*z));
    }
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[Q.slots[0].x];
    RayDesc ray;ray.Origin=origin;ray.Direction=direction;ray.TMin=0.01;ray.TMax=64;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;query.TraceRayInline(scene,0,0xFF,ray);
    while(query.Proceed()){}
    float3 radiance=0;float distance=64,sky=1;
    if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        distance=query.CommittedRayT();float3 normal;
        radiance=ProbeHitRadiance(query.CommittedInstanceID(),origin+direction*distance,direction,normal);
        if(dot(normal,direction)>0)distance=-distance;
        sky=0;
    }
    probeDirections[lane]=direction;probeRadiance[lane]=min(radiance,32);
    probeDistances[lane]=distance;probeSky[lane]=sky;
    GroupMemoryBarrierWithGroupSync();
    if(lane==0) {
        uint inside=0;float nearest=1e6;uint nearIndex=0;
        [unroll]for(uint i=0;i<6;i++) {
            if(probeDistances[i]<0) {
                inside++;if(-probeDistances[i]<nearest){nearest=-probeDistances[i];nearIndex=i;}
            }
        }
        if(inside>=3) {
            probeState.xyz=clamp(probeState.xyz+probeDirections[nearIndex]*(nearest+0.12),
                -Q.probeOriginSpacing.w*0.45,Q.probeOriginSpacing.w*0.45);
            probeState.w=0;
        }else probeState.w=1;
        RWTexture3D<float4> data=ResourceDescriptorHeap[Q.slots[7].z];data[cell]=probeState;
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 texel=uint2(lane%8,lane/8),pixel=uint2(index%16,index/16)*8+texel;
    float3 n=QOctDirection(QBorderUV(texel));
    float3 energy=0;float skyWeight=0,cosWeight=0,momentWeight=0;float2 moments=0;
    [loop]for(uint i=0;i<64;i++) {
        float cosine=saturate(dot(n,probeDirections[i]));
        energy+=probeRadiance[i]*cosine;skyWeight+=probeSky[i]*cosine;cosWeight+=cosine;
        float w=pow(cosine,16);float d=abs(probeDistances[i]);
        moments+=float2(d,d*d)*w;momentWeight+=w;
    }
    float4 value=float4(energy*(4*QPi/64),skyWeight/max(cosWeight,0.001));
    Texture2D<float4> previous=ResourceDescriptorHeap[Q.slots[1].w];
    Texture2D<float4> previousDistance=ResourceDescriptorHeap[Q.slots[2].x];
    float4 history=previous.Load(int3(pixel,0));
    float hysteresis=probeState.w>0.5&&Q.slots[10].w==0?Q.probeCountsHistory.w:0;
    float change=length(value.rgb-history.rgb)/max(length(history.rgb),0.05);
    if(change>0.3)hysteresis=min(hysteresis,0.5);
    RWTexture2D<float4> output=ResourceDescriptorHeap[Q.slots[7].x];
    RWTexture2D<float4> depth=ResourceDescriptorHeap[Q.slots[7].y];
    output[pixel]=lerp(value,history,hysteresis);
    depth[pixel]=float4(lerp(moments/max(momentWeight,0.001),previousDistance.Load(int3(pixel,0)).rg,hysteresis),0,0);
}
float CloudDensity(float3 p) {
    if(Q.cloud.x<=0)return 0;
    float h=(p.y-Q.cloud.y)/Q.cloud.z;if(h<=0||h>=1)return 0;
    Texture3D<float4> noise=ResourceDescriptorHeap[Q.slots[4].x];
    Texture2D<float> weather=ResourceDescriptorHeap[Q.slots[4].z];
    float3 advected=p-Q.wind.xyz*Q.cloud.w;
    float coverage=saturate(Q.cloud.x+(weather.SampleLevel(linearWrap,advected.xz*0.001,0)-0.5)*0.4);
    float3 shape=noise.SampleLevel(linearWrap,advected*0.008,0).rgb;
    float detail=noise.SampleLevel(linearWrap,advected*0.035,0).g;
    float envelope=smoothstep(0,0.15,h)*(1-smoothstep(0.65,1,h));
    return saturate((shape.r-(1-coverage))*3.5-(1-detail)*0.15)*envelope;
}
float CloudLight(float3 p,uint steps) {
    if((Q.slots[9].w&4)==0||Q.cloud.x<=0)return 1;
    float3 l=Q.keyDirectionIntensity.xyz;
    if(l.y<0.02)return 0;
    float length=(Q.cloud.y+Q.cloud.z-p.y)/l.y,dt=min(length,1000)/steps,tau=0;
    [loop]for(uint i=0;i<steps;i++)tau+=CloudDensity(p+l*((i+0.5)*dt))*dt*0.08;
    return exp(-tau);
}
[numthreads(8,8,1)] void CSCloudShadow(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=128))return;
    float2 xz=Q.cloudShadowOrigin.xy+((float2(id.xy)+0.5)/128-0.5)*Q.cloudShadowOrigin.z;
    RWTexture2D<float> shadow=ResourceDescriptorHeap[Q.slots[5].y];
    shadow[id.xy]=CloudLight(float3(xz.x,Q.cloud.y+0.01,xz.y),32);
}
[numthreads(8,8,1)] void CSLocalShadowCache(uint3 id:SV_DispatchThreadID) {
    uint cache=id.z/6,face=id.z%6;
    if(any(id.xy>=128)||cache>=Q.slots[12].z||(Q.slots[12].w&(1u<<cache))==0)return;
    QualityLight light=Q.lights[Q.slots[13][cache]];
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[Q.slots[0].x];
    RayDesc ray;ray.Origin=light.positionRange.xyz;
    ray.Direction=QCubeDirection(face,(float2(id.xy)+0.5)/128);ray.TMin=0.005;ray.TMax=max(light.positionRange.w,0.006);
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;query.TraceRayInline(scene,0,1,ray);while(query.Proceed()){}
    RWTexture2DArray<float> output=ResourceDescriptorHeap[Q.slots[12].y];
    output[id]=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT?query.CommittedRayT():light.positionRange.w;
}
float FogZ(float slice) { return 0.25*pow(Q.fog.w/0.25,slice/Q.grid.z); }
float3 FogPosition(uint3 id,float jitter) {
    float3 direction=QRay(Q,(float2(id.xy)+0.5)/float2(Q.grid.xy));
    return Q.eyeFrame.xyz+direction*(FogZ(id.z+jitter)/max(dot(direction,Q.forwardHistory.xyz),0.01));
}
float FogDensityAt(float3 p){
    float sigma=Q.fog.x*exp(-max(0,p.y-Q.fog.z)*Q.fog.y);
    [loop]for(uint volume=0;volume<Q.volumeInfo.x;volume++){
        float3 normalized=abs(p-Q.volumeCenters[volume].xyz)/max(Q.volumeExtents[volume].xyz*.5,.001);
        sigma+=Q.volumeCenters[volume].w*saturate((1-max(normalized.x,max(normalized.y,normalized.z)))*5);
    }return sigma;
}
[numthreads(4,4,4)] void CSFogInject(uint3 id:SV_DispatchThreadID) {
    if(any(id>=Q.grid.xyz))return;
    float jitter=0.25+QHash(float3(id.xy,Q.grid.w%16))*0.5;
    float3 p=FogPosition(id,jitter),view=normalize(p-Q.eyeFrame.xyz);
    float sigma=FogDensityAt(p);
    float3 scattering=sigma*.95,emission=0;
    float rayScale=1/max(dot(view,Q.forwardHistory.xyz),.01);
    float sliceStart=id.z==0?0:FogZ(id.z)*rayScale,sliceEnd=FogZ(id.z+1)*rayScale;
    [loop]for(uint source=0;source<Q.smokeInfo.x;source++){
        SmokeVolume v=Q.smokeVolumes[source];float a,b;if(!SmokeInterval(v,Q.eyeFrame.xyz,view,sliceEnd,a,b))continue;a=max(a,sliceStart);if(b<=a)continue;
        ByteAddressBuffer field=ResourceDescriptorHeap[v.offsets.w];uint samples=min(8u,max(1u,uint(ceil((b-a)/(v.originCell.w*.5)))));
        float weight=(b-a)/((sliceEnd-sliceStart)*samples);
        [loop]for(uint sample=0;sample<samples;sample++){
            float localSigma=0;float3 localScattering=0,localEmission=0;
            SmokeAccumulate(field,v,Q.eyeFrame.xyz+view*(a+(sample+.5)*(b-a)/samples),localSigma,localScattering,localEmission);
            sigma+=localSigma*weight;scattering+=localScattering*weight;emission+=localEmission*weight;
        }
    }
    // Cache incident light around source boundaries too. Final pixels sample
    // this smooth light field but integrate their own exact medium intervals;
    // low-resolution froxels must not spatially average away optical depth.
    bool cacheLight=false;
    float padding=sliceEnd-sliceStart+4*length(p-Q.eyeFrame.xyz)*max(1.f/Q.grid.x,1.f/Q.grid.y);
    [loop]for(uint source=0;source<Q.smokeInfo.x;source++){
        SmokeVolume v=Q.smokeVolumes[source];float3 nearest=clamp(p,v.originCell.xyz,v.originCell.xyz+float3(v.dimensions.xyz)*v.originCell.w);
        cacheLight=cacheLight||length(p-nearest)<=padding;
    }
    if(Q.smokeInfo.x>0){RWTexture3D<float4> incident=ResourceDescriptorHeap[Q.slots[14].w+1];incident[id]=0;}
    if(sigma<1e-7&&all(emission<1e-7)&&!cacheLight) {
        RWTexture3D<float4> history=ResourceDescriptorHeap[Q.slots[2].w];history[id]=0;
        RWTexture3D<float4> output=ResourceDescriptorHeap[Q.slots[3].x];output[id]=0;return;
    }
    float3 sky=QDiffuseSky(Q,float3(0,1,0))/QPi;
    float4 gi=QIrradiance(Q,p,float3(0,1,0));
    float visibility=1;if(Q.keyDirectionIntensity.w>0)visibility=QVisibility(Q,p,Q.keyDirectionIntensity.xyz,200)*QCloudVisibility(Q,p,linearClamp);
    float3 staticLight=(sky*gi.a+gi.rgb/QPi)*0.25+Q.keyColor.rgb*Q.keyDirectionIntensity.w*
        QPhase(dot(view,Q.keyDirectionIntensity.xyz),0.6)*visibility;
    if(Q.forwardHistory.w>0.5&&Q.smokeInfo.x==0) {
        float4 oldClip=mul(Q.previousViewProjection,float4(p,1));
        float2 uv=oldClip.xy/oldClip.w*float2(0.5,-0.5)+0.5;
        float z=dot(p-Q.previousEye.xyz,Q.previousForward.xyz);
        float slice=log(max(z,0.25)/0.25)/log(Q.fog.w/0.25);
        if(oldClip.w>0&&all(uv>0)&&all(uv<1)&&slice>0&&slice<1) {
            Texture3D<float4> history=ResourceDescriptorHeap[Q.slots[2].z];
            float4 old=history.SampleLevel(linearClamp,float3(uv,slice),0);
            float valid=abs(old.a-sigma)<max(0.002,sigma*0.2)?0.85:0;
            staticLight=lerp(staticLight,clamp(old.rgb,staticLight*0.5,staticLight*1.5+0.01),valid);
        }
    }
    RWTexture3D<float4> historyOut=ResourceDescriptorHeap[Q.slots[2].w];
    historyOut[id]=float4(staticLight,sigma);
    // Moving local lights never enter static history, so headlights leave no light trails.
    float3 dynamicLight=0;
    float scores[4]={0,0,0,0};uint indices[4]={0,0,0,0};
    [loop]for(uint i=0;i<Q.slots[10].z;i++) {
        QualityLight light=Q.lights[i];if(light.shadow.z<=0)continue;
        float3 direction;float distance;float3 radiance=QLocalRadiance(light,p,direction,distance);
        if(!any(radiance>0.0001))continue;
        float score=dot(radiance,float3(0.2126,0.7152,0.0722))*light.shadow.z*QPhase(dot(view,direction),0.3);
        [unroll]for(uint k=0;k<4;k++) {
            if(score>scores[k]) {
                [unroll]for(int j=3;j>int(k);j--){scores[j]=scores[j-1];indices[j]=indices[j-1];}
                scores[k]=score;indices[k]=i;break;
            }
        }
    }
    [unroll]for(uint k=0;k<4;k++) {
        if(scores[k]<=0)continue;
        QualityLight light=Q.lights[indices[k]];float3 direction;float distance;
        float3 radiance=QLocalRadiance(light,p,direction,distance);
        float visible=light.shadow.y>0&&k<2?QLocalVisibility(Q,indices[k],p,float3(0,0,0)):1;
        dynamicLight+=radiance*QPhase(dot(view,direction),0.3)*visible*light.shadow.z;
    }
    RWTexture3D<float4> output=ResourceDescriptorHeap[Q.slots[3].x];
    output[id]=float4((staticLight+dynamicLight)*scattering+emission,sigma);
    if(Q.smokeInfo.x>0){RWTexture3D<float4> incident=ResourceDescriptorHeap[Q.slots[14].w+1];incident[id]=float4(staticLight+dynamicLight,1);}
}
[numthreads(8,8,1)] void CSFogIntegrate(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=Q.grid.xy))return;
    Texture3D<float4> input=ResourceDescriptorHeap[Q.slots[3].y];
    RWTexture3D<float4> output=ResourceDescriptorHeap[Q.slots[3].z];
    float3 l=0;float t=1;
    float rayScale=1/max(dot(QRay(Q,(float2(id.xy)+0.5)/float2(Q.grid.xy)),Q.forwardHistory.xyz),0.01);
    [loop]for(uint z=0;z<Q.grid.z;z++) {
        float4 sample=input.Load(int4(id.xy,z,0));float dt=(FogZ(z+1)-(z==0?0:FogZ(z)))*rayScale;
        float stepT=exp(-sample.a*dt);
        l+=t*sample.rgb*(sample.a>1e-6?(1-stepT)/sample.a:dt);t*=stepT;
        output[uint3(id.xy,z)]=float4(l,t);
    }
}
[numthreads(8,8,1)] void CSCloud(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=Q.screen.zw))return;
    float2 uv=(float2(id.xy)+0.5)/float2(Q.screen.zw);
    float3 direction=QRay(Q,uv);float4 result=float4(0,0,0,1);float first=0;
    float dy=direction.y;
    if(Q.cloud.x>0&&abs(dy)>0.0001) {
        float a=(Q.cloud.y-Q.eyeFrame.y)/dy,b=(Q.cloud.y+Q.cloud.z-Q.eyeFrame.y)/dy;
        float start=max(min(a,b),0),end=min(max(a,b),1000);
        Texture2D<float> opaqueDepth=ResourceDescriptorHeap[Q.slots[8].w];
        // Stop at the nearest opaque sample covered by this half-resolution pixel.
        [unroll]for(uint sample=0;sample<4;sample++) {
            uint2 pixel=min(id.xy*2+uint2(sample&1,sample>>1),Q.screen.xy-1);
            float depth=opaqueDepth.Load(int3(pixel,0));
            if(depth<0.999999) {
                float2 ndc=(float2(pixel)+0.5)/float2(Q.screen.xy)*float2(2,-2)+float2(-1,1);
                float4 world=mul(Q.inverseViewProjection,float4(ndc,depth,1));
                end=min(end,length(world.xyz/world.w-Q.eyeFrame.xyz));
            }
        }
        if(end>start) {
            uint steps=Q.slots[11].z;float dt=(end-start)/steps;
            float jitter=QHash(float3(id.xy,Q.grid.w%16));
            [loop]for(uint i=0;i<steps;i++) {
                float distance=start+(i+jitter)*dt;
                float3 p=Q.eyeFrame.xyz+direction*distance;float density=CloudDensity(p);
                if(density<0.001)continue;
                if(first==0)first=distance;
                float lightT=CloudLight(p,Q.slots[11].w);
                // Three attenuation octaves approximate energy from multiple scattering.
                float phase=QPhase(dot(direction,Q.keyDirectionIntensity.xyz),0.65);
                float3 incoming=Q.keyColor.rgb*Q.keyDirectionIntensity.w*
                    (lightT*phase+0.5*sqrt(lightT)/(4*QPi)+0.25*pow(lightT,0.25)/(4*QPi));
                incoming+=QSky(Q,float3(0,1,0))*lerp(0.3,0.8,saturate((p.y-Q.cloud.y)/Q.cloud.z));
                float stepT=exp(-density*dt*0.08);
                result.rgb+=result.a*(1-stepT)*incoming;result.a*=stepT;
                if(result.a<0.01)break;
            }
        }
    }
    if(first>0&&Q.forwardHistory.w>0.5) {
        float3 oldPosition=Q.eyeFrame.xyz+direction*first-Q.wind.xyz*Q.wind.w;
        float4 clip=mul(Q.previousViewProjection,float4(oldPosition,1));
        float2 oldUv=clip.xy/clip.w*float2(0.5,-0.5)+0.5;
        if(clip.w>0&&all(oldUv>0)&&all(oldUv<1)) {
            Texture2D<float> oldDepth=ResourceDescriptorHeap[Q.slots[6].w];
            Texture2D<float4> oldColor=ResourceDescriptorHeap[Q.slots[6].x];
            float distance=oldDepth.SampleLevel(linearClamp,oldUv,0);
            if(abs(distance-length(oldPosition-Q.previousEye.xyz))<max(2,first*0.02)) {
                float4 history=oldColor.SampleLevel(linearClamp,oldUv,0);
                history.rgb=clamp(history.rgb,result.rgb*0.6,result.rgb*1.4+0.005);
                history.a=clamp(history.a,result.a-0.1,result.a+0.1);
                result=lerp(result,history,0.8);
            }
        }
    }
    RWTexture2D<float4> color=ResourceDescriptorHeap[Q.slots[5].z];
    RWTexture2D<float> depth=ResourceDescriptorHeap[Q.slots[6].y];
    color[id.xy]=result;depth[id.xy]=first;
}
struct FullscreenVertex {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
FullscreenVertex VSQuality(uint id:SV_VertexID) {
    FullscreenVertex o;o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
float4 SmokeQualityPrefix(float2 uv,float limit,float3 direction){
    float edges[20];uint count=2;edges[0]=0;edges[1]=limit;
    float fogEnd=Q.fog.w/max(dot(direction,Q.forwardHistory.xyz),.01);
    if(fogEnd>0&&fogEnd<limit)edges[count++]=fogEnd;
    [loop]for(uint i=0;i<Q.smokeInfo.x;i++){float a,b;if(SmokeInterval(Q.smokeVolumes[i],Q.eyeFrame.xyz,direction,limit,a,b)){edges[count++]=a;edges[count++]=b;}}
    [loop]for(uint edge=1;edge<count;edge++){float value=edges[edge];uint j=edge;[loop]while(j>0&&edges[j-1]>value){edges[j]=edges[j-1];j--;}edges[j]=value;}
    float3 radiance=0;float transmittance=1;
    Texture3D<float4> incident=ResourceDescriptorHeap[Q.slots[14].w];
    [loop]for(uint segment=1;segment<count;segment++){
        float a=edges[segment-1],b=edges[segment],span=b-a;if(span<1e-6)continue;
        float cell=1e9;float3 mid=Q.eyeFrame.xyz+direction*((a+b)*.5);
        [loop]for(uint i=0;i<Q.smokeInfo.x;i++){SmokeVolume v=Q.smokeVolumes[i];float3 grid=(mid-v.originCell.xyz)/v.originCell.w;if(all(grid>=0)&&all(grid<float3(v.dimensions.xyz)))cell=min(cell,v.originCell.w);}
        if(cell>1e8&&((Q.fog.x<=0&&Q.volumeInfo.x==0)||a>=fogEnd))continue;
        uint samples=cell<1e8?min(64u,max(1u,uint(ceil(span/(cell*.5))))):max(1u,uint(ceil(12*span/fogEnd)));float ds=span/samples;
        [loop]for(uint sample=0;sample<samples;sample++){
            float distance=a+(sample+.5)*ds;float3 p=Q.eyeFrame.xyz+direction*distance;
            float sigma=distance<fogEnd?FogDensityAt(p):0;float3 scattering=sigma*.95,emission=0;
            [loop]for(uint source=0;source<Q.smokeInfo.x;source++){SmokeVolume v=Q.smokeVolumes[source];ByteAddressBuffer field=ResourceDescriptorHeap[v.offsets.w];SmokeAccumulate(field,v,p,sigma,scattering,emission);}
            float viewZ=distance*dot(direction,Q.forwardHistory.xyz),z=log(max(viewZ,.25)/.25)/log(Q.fog.w/.25);
            float3 light=incident.SampleLevel(linearClamp,float3(uv,saturate(z)),0).rgb;
            float stepT=exp(-sigma*ds),integral=sigma>1e-6?(1-stepT)/sigma:ds;
            radiance+=transmittance*(light*scattering+emission)*integral;transmittance*=stepT;
        }
    }return float4(radiance,transmittance);
}
float3 TransparencyQualityFog(float3 color,float2 uv,float distance,float3 direction){
    Texture2D<float4> cloud=ResourceDescriptorHeap[Q.slots[5].w];Texture2D<float> cloudDepth=ResourceDescriptorHeap[Q.slots[6].z];
    float2 coord=uv*Q.screen.zw-.5;int2 at=int2(floor(coord));float2 fraction=frac(coord);float4 c=0;float weights=0;
    [unroll]for(uint i=0;i<4;i++){int2 o=int2(i&1,i>>1),sample=clamp(at+o,0,int2(Q.screen.zw)-1);float cd=cloudDepth.Load(int3(sample,0));float2 w=lerp(1-fraction,fraction,float2(o));float weight=w.x*w.y;float4 value=cloud.Load(int3(sample,0));if(cd>0&&cd>distance)value=float4(0,0,0,1);c+=value*weight;weights+=weight;}
    c/=max(weights,.001);color=color*c.a+c.rgb;
    if(Q.smokeInfo.x>0){float4 f=SmokeQualityPrefix(uv,distance,direction);color=color*f.a+f.rgb;}
    else if(Q.fog.x>0||Q.volumeInfo.x>0){float viewZ=min(distance*dot(direction,Q.forwardHistory.xyz),Q.fog.w);float z=log(max(viewZ,.25)/.25)/log(Q.fog.w/.25);Texture3D<float4> fog=ResourceDescriptorHeap[Q.slots[3].w];float4 f=fog.SampleLevel(linearClamp,float3(uv,saturate(z)-.5/Q.grid.z),0);color=color*f.a+f.rgb;}
    return color;
}
float4 PSQuality(FullscreenVertex input):SV_TARGET {
    uint2 pixel=uint2(input.position.xy);
    Texture2D<float4> base=ResourceDescriptorHeap[Q.slots[8].x];
    Texture2D<float4> key=ResourceDescriptorHeap[Q.slots[8].y];
    Texture2D<float> shadow=ResourceDescriptorHeap[Q.slots[8].z];
    Texture2D<float> depth=ResourceDescriptorHeap[Q.slots[8].w];
    float visibility=shadow.Load(int3(pixel,0));float3 b=base.Load(int3(pixel,0)).rgb;
    float3 k=key.Load(int3(pixel,0)).rgb;
    if(Q.slots[9].z==1)return float4(visibility.xxx,1);
    if(Q.slots[9].z==2)return float4(k,1);
    if(Q.slots[9].z==3)return float4(b,1);
    float3 direction=QRay(Q,input.uv);float d=depth.Load(int3(pixel,0));
    float3 color=b+k*visibility;float distance=1000;
    if(d>=0.999999) {
        color=QSky(Q,direction);
        if(Q.slots[14].y==0){
        float moon=smoothstep(0.9994,0.99985,dot(direction,Q.keyDirectionIntensity.xyz));
        color+=Q.keyColor.rgb*Q.keyDirectionIntensity.w*moon*6;
        float night=1-saturate(dot(Q.skyExposure.rgb,float3(0.2126,0.7152,0.0722))*20);
        color+=step(0.998,QHash(floor(direction*900)))*smoothstep(0.08,0.35,direction.y)*0.5*night;
        }
    } else {
        float4 world=mul(Q.inverseViewProjection,float4(input.uv*float2(2,-2)+float2(-1,1),d,1));
        distance=length(world.xyz/world.w-Q.eyeFrame.xyz);
    }
    Texture2D<float4> cloud=ResourceDescriptorHeap[Q.slots[5].w];
    Texture2D<float> cloudDepth=ResourceDescriptorHeap[Q.slots[6].z];
    // Depth-aware 2x2 reconstruction keeps foreground silhouettes out of cloud history.
    float2 coord=input.uv*Q.screen.zw-0.5;int2 at=int2(floor(coord));float2 fraction=frac(coord);
    float4 c=0;float weights=0;
    [unroll]for(uint i=0;i<4;i++) {
        int2 o=int2(i&1,i>>1),sample=clamp(at+o,0,int2(Q.screen.zw)-1);
        float cd=cloudDepth.Load(int3(sample,0));float2 w=lerp(1-fraction,fraction,float2(o));
        float weight=w.x*w.y;
        float4 value=cloud.Load(int3(sample,0));
        if(cd>0&&cd>distance)value=float4(0,0,0,1);
        c+=value*weight;weights+=weight;
    }
    c/=max(weights,0.001);color=color*c.a+c.rgb;
    if(Q.fog.x>0||Q.volumeInfo.x>0||Q.smokeInfo.x>0) {
        float viewZ=min(distance*dot(direction,Q.forwardHistory.xyz),Q.fog.w);
        float z=log(max(viewZ,0.25)/0.25)/log(Q.fog.w/0.25);
        Texture3D<float4> fog=ResourceDescriptorHeap[Q.slots[3].w];
        float4 f=Q.smokeInfo.x>0?SmokeQualityPrefix(input.uv,distance,direction):fog.SampleLevel(linearClamp,float3(input.uv,saturate(z)-0.5/Q.grid.z),0);
        color=color*f.a+f.rgb;
        if(Q.slots[9].z==5)return float4(f.a.xxx,1);
        if(Q.slots[9].z==9)return float4(f.rgb/(1+f.rgb),1);
    }
    if(Q.slots[9].z==6)return float4(c.a.xxx,1);
    if(Q.slots[9].z==7)return float4(QCloudVisibility(Q,Q.eyeFrame.xyz+direction*min(distance,100),linearClamp).xxx,1);
    if(dispatchPadding.z!=0){
        Texture2D<uint> heads=ResourceDescriptorHeap[dispatchPadding.x];StructuredBuffer<TransparencyNode> nodes=ResourceDescriptorHeap[dispatchPadding.y];uint head=heads.Load(int3(pixel,0));
        float3 accumulated=0;float transmittance=1;bool hasPrevious=false;TransparencyNode previous=(TransparencyNode)0;
        [loop]for(uint layer=0;layer<TransparencyMaxLayers;layer++){TransparencyNode node;if(!TransparencyNext(nodes,head,d,hasPrevious,previous,node))break;previous=node;hasPrevious=true;
            float4 world=mul(Q.inverseViewProjection,float4(input.uv*float2(2,-2)+float2(-1,1),node.depth,1));float distance=length(world.xyz/world.w-Q.eyeFrame.xyz);
            float3 surface=TransparencyQualityFog(node.color.rgb,input.uv,distance,direction);accumulated+=surface*node.color.a*transmittance;transmittance*=1-node.color.a;if(transmittance==0)break;
        }color=accumulated+color*transmittance;
    }
    if((Q.slots[9].w&8)==0)return float4(saturate(color),1);
    color*=Q.skyExposure.w;
    // Reinhard shoulder retains HDR energy instead of clipping it before gamma encoding.
    color=color/(1+color);return float4(pow(saturate(color),1/2.2),1);
}
