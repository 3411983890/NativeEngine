cbuffer WaterConstants : register(b0) {
    column_major float4x4 viewProjection;
    column_major float4x4 inverseViewProjection;
    float4 cameraTime;
    float4 keyDirectionIntensity;
    float4 keyColorRoughness;
    float4 skyIor;
    float4 absorption;
    float4 scattering;
    uint4 grid;
    float4 viewport;
    // xy = residual footprint origin in world xz, zw = footprint extent in metres.
    float4 rippleOriginExtent;
    // xy = residual resolution, zw = residual texel size in metres.
    float4 rippleGrid;
    // x = displacement scale, y = normal-detail scale, z = 1 when the residual is bound.
    float4 rippleScale;
};
Texture2D<float4> opaqueBase : register(t0);
Texture2D<float4> opaqueKey : register(t1);
Texture2D<float> opaqueDepth : register(t2);
Texture2D<float> shadowMask : register(t3);
SamplerState linearClamp : register(s0);
struct WaterVertex {float3 position;float foam;float3 normal;float depth;float2 velocity;};
StructuredBuffer<WaterVertex> vertices : register(t4);
Texture2D<float> rippleHeight : register(t5);
struct Varyings {float4 clip : SV_Position;float3 world : WORLD;float3 normal : NORMAL;float2 foamDepth : TEXCOORD0;float2 residual : TEXCOORD1;float2 flow : TEXCOORD2;};
float2 ResidualUV(float3 position) {
    return (position.xz - rippleOriginExtent.xy) / max(rippleOriginExtent.zw, 1e-4);
}
// The residual grid is finer than the render mesh, so its geometry is sampled at the
// vertex while its slope carries the sub-cell detail into the shading normal.
float2 ResidualSlope(float2 uv) {
    if (rippleScale.z <= 0) return 0;
    float2 step = 1.0 / max(rippleGrid.xy, 2);
    float left = rippleHeight.SampleLevel(linearClamp, uv - float2(step.x, 0), 0);
    float right = rippleHeight.SampleLevel(linearClamp, uv + float2(step.x, 0), 0);
    float down = rippleHeight.SampleLevel(linearClamp, uv - float2(0, step.y), 0);
    float up = rippleHeight.SampleLevel(linearClamp, uv + float2(0, step.y), 0);
    return float2(right - left, up - down) / (2 * max(rippleGrid.zw, 1e-4)) * rippleScale.y;
}
Varyings VSMain(uint id : SV_VertexID) {
    uint cell=id/6,corner=id%6;
    uint x=cell%(grid.x-1),z=cell/(grid.x-1);
    const uint2 offsets[6]={uint2(0,0),uint2(0,1),uint2(1,0),uint2(1,0),uint2(0,1),uint2(1,1)};
    uint2 c=uint2(x,z)+offsets[corner];WaterVertex v=vertices[c.y*grid.x+c.x];
    float3 position=v.position;
    float2 uv=ResidualUV(position);
    float2 inside=saturate(min(uv,1-uv)*24);
    float mask=rippleScale.z*inside.x*inside.y;
    if(mask>0)position.y+=rippleHeight.SampleLevel(linearClamp,uv,0)*rippleScale.x*mask;
    Varyings o;o.world=position;o.normal=v.normal;o.foamDepth=float2(v.foam,v.depth);
    o.residual=ResidualSlope(uv)*mask;o.flow=v.velocity;
    o.clip=mul(viewProjection,float4(position,1));return o;
}
float3 WorldAt(float2 uv,float depth) {
    float4 p=mul(inverseViewProjection,float4(uv*float2(2,-2)+float2(-1,1),depth,1));return p.xyz/p.w;
}
float3 SceneAt(float2 uv) {
    return opaqueBase.SampleLevel(linearClamp,uv,0).rgb+opaqueKey.SampleLevel(linearClamp,uv,0).rgb*shadowMask.SampleLevel(linearClamp,uv,0);
}
float3 Environment(float3 r) {
    float3 horizon=skyIor.rgb*.9+float3(.08,.075,.06);
    return lerp(horizon,skyIor.rgb,pow(saturate(r.y),.4));
}
float3 Reflection(float3 p,float3 r,out float confidence) {
    float3 environment=Environment(r);confidence=0;
    const float2 texel=1.0/max(viewport.xy,1.0)*1.5;
    float travel=.05,previous=0;
    [loop] for(uint step=0;step<48;++step) {
        float3 rayPoint=p+r*travel;
        float4 clip=mul(viewProjection,float4(rayPoint,1));
        if(clip.w<=0)break;
        float2 uv=clip.xy/clip.w*float2(.5,-.5)+.5;
        if(any(uv<.005)||any(uv>.995))break;
        // Compare device depth, not the distance back to the eye: the old test accepted any
        // hit within a fixed tolerance, so a ray sliding along a leg stayed "behind" it for
        // dozens of steps and painted the vertical smear under the character.
        const float rayDepth=clip.z/clip.w;
        const float sceneDepth=opaqueDepth.SampleLevel(linearClamp,uv,0);
        if(sceneDepth<.999999) {
            const float gap=rayDepth-sceneDepth;
            if(gap>=0&&previous<0) {
                // A large depth step inside one texel means the ray grazed a silhouette; a
                // mirror would not show that surface, so the hit is discarded instead of
                // being smeared outward.
                const float spread=max(max(abs(opaqueDepth.SampleLevel(linearClamp,uv+float2(texel.x,0),0)-sceneDepth),
                                           abs(opaqueDepth.SampleLevel(linearClamp,uv-float2(texel.x,0),0)-sceneDepth)),
                                       max(abs(opaqueDepth.SampleLevel(linearClamp,uv+float2(0,texel.y),0)-sceneDepth),
                                           abs(opaqueDepth.SampleLevel(linearClamp,uv-float2(0,texel.y),0)-sceneDepth)));
                const float edge=1.0-smoothstep(.0004,.004,spread);
                confidence=saturate(min(min(uv.x,1-uv.x),min(uv.y,1-uv.y))*14*edge);
                return lerp(environment,SceneAt(uv),confidence);
            }
            previous=gap;
        } else previous=1.0;
        travel+=.05+.010*step;
    }
    return environment;
}
float3 FineNormal(float3 p,float3 geometric) {
    // Analytic slopes of sub-mesh wind ripples. Attenuate frequencies that the
    // current pixel footprint cannot resolve; no static blue normal wallpaper.
    float2 slope=0;float footprint=max(length(ddx(p.xz)),length(ddy(p.xz)));
    const float2 directions[3]={float2(.93,.37),float2(-.61,.79),float2(.3,-.95)};
    const float wavelengths[3]={.24,.13,.075};
    [unroll] for(uint i=0;i<3;++i) {
        float k=6.2831853/wavelengths[i];float fade=1-smoothstep(.15,.5,footprint/wavelengths[i]);
        float phase=dot(p.xz,directions[i])*k-cameraTime.w*(1.7+i*.71);
        slope+=directions[i]*cos(phase)*(.0022/(1+i*.6))*k*fade;
    }
    return normalize(geometric+float3(-slope.x,0,-slope.y));
}
struct Output {float4 surface : SV_Target0;float4 key : SV_Target1;};
float Hash21(float2 p){p=frac(p*float2(123.34,456.21));p+=dot(p,p+45.32);return frac(p.x*p.y);}
float ValueNoise(float2 p){
    float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);
    float a=Hash21(i),b=Hash21(i+float2(1,0)),c=Hash21(i+float2(0,1)),d=Hash21(i+float2(1,1));
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
// Foam is lace, not a solid sticker: two scrolling octaves of value noise, with the fine
// octave dropped once a pixel cannot resolve it, otherwise the bank line shimmers.
float FoamPattern(float2 p,float footprint,float2 flow){
    // White water drifts with the current instead of sliding along a world axis that
    // has nothing to do with the flow. The bounded flow term keeps a slow river readable
    // and a fast one from smearing the pattern into a single streak.
    float2 drift=flow*cameraTime.w*clamp(.6/sqrt(1+dot(flow,flow)),0.05,1.5);
    float2 scroll=float2(cameraTime.w*.32,-cameraTime.w*.21)-drift;
    float coarse=ValueNoise(p*6.5+scroll);
    float fine=ValueNoise(p*19.0-scroll*1.6);
    float fade=1-smoothstep(.006,.020,footprint);
    return saturate(lerp(coarse,coarse*.45+fine*.55,fade));
}
Output PSMain(Varyings input) {
    clip(input.foamDepth.y-.0005);
    float2 uv=input.clip.xy/viewport.xy;
    float3 v=normalize(cameraTime.xyz-input.world);
    float3 geometric=normalize(input.normal);
    // The vertex normal already carries the coarse height field. Rebuild the slope, add
    // the residual gradient, then hand the result to the analytic detail layer.
    float2 slope=-geometric.xz/max(geometric.y,1e-4)+input.residual;
    float3 n=FineNormal(input.world,normalize(float3(-slope.x,1,-slope.y)));
    bool underwater=dot(v,n)<0;if(underwater)n=-n;
    float ndv=saturate(dot(n,v));
    float f0=pow((skyIor.w-1)/(skyIor.w+1),2);
    float fresnel=f0+(1-f0)*pow(1-ndv,5);
    if(underwater&&skyIor.w*skyIor.w*(1-ndv*ndv)>1)fresnel=1;
    float backgroundDepth=opaqueDepth.SampleLevel(linearClamp,uv,0);
    float3 bottom=WorldAt(uv,min(backgroundDepth,.99999));
    float thickness=backgroundDepth<.99999?clamp(length(bottom-input.world),0,30):8;
    float2 distorted=uv+n.xz*min(thickness,1.8)*.018/max(1,length(input.world-cameraTime.xyz)*.12);
    distorted=clamp(distorted,.001,.999);
    float distortedDepth=opaqueDepth.SampleLevel(linearClamp,distorted,0);
    // Reject foreground silhouettes and above-water geometry in the refraction.
    float3 refractedPoint=WorldAt(distorted,min(distortedDepth,.99999));
    if(distortedDepth<=input.clip.z||(!underwater&&refractedPoint.y>input.world.y+.015))distorted=uv;
    else if(distortedDepth<.99999)thickness=clamp(length(refractedPoint-input.world),0,30);
    // The depth-buffer difference is a chord, not the path a refracted ray travels. Snell
    // at the interface gives a bounded correction: the in-water segment is longer by
    // 1/cosT. This is still an approximation (the bottom is a depth sample, not a traced
    // surface), but it no longer treats a grazing view and a top-down view as equal.
    const float cosI=max(saturate(dot(-v,n)),.02);
    const float eta=1.0/max(skyIor.w,1.001);
    const float cosT=sqrt(saturate(1.0-eta*eta*(1.0-cosI*cosI)));
    const float opticalPath=clamp(thickness/lerp(1.0,max(cosT,.35),.6),0,30);
    float3 transmittance=exp(-absorption.rgb*opticalPath);
    // Visual caustics: residual curvature focuses transmitted light on the bed. This is
    // a bounded presentation term, not a light-transport solve; it fades with thickness.
    float caustic=1.0;
    if(!underwater){
        // Caustics brighten where the wave surface actually bends, not on calm water.
        // The residual slope is the bounded focus proxy; the gain is deliberately small.
        const float focus=smoothstep(.03,.30,length(ResidualSlope(uv)));
        caustic=1.0+.16*focus*exp(-max(thickness,0.0)*.6);
    }
    float3 lighting=max(skyIor.rgb*.6+keyColorRoughness.rgb*keyDirectionIntensity.w*.25,.02);
    float3 transmission=(SceneAt(distorted)*transmittance+scattering.rgb*(1-transmittance)*lighting)*caustic;
    float confidence;float3 reflected=Reflection(input.world+n*.035,reflect(-v,n),confidence);
    float3 l=normalize(keyDirectionIntensity.xyz),h=normalize(l+v);
    float ndl=saturate(dot(n,l)),ndh=saturate(dot(n,h)),vdh=saturate(dot(v,h));
    float a=keyColorRoughness.w*keyColorRoughness.w,a2=a*a;
    float D=a2/max(3.14159265*pow(ndh*ndh*(a2-1)+1,2),1.e-7);
    float k=pow(keyColorRoughness.w+1,2)/8;
    float G=(ndv/max(ndv*(1-k)+k,.001))*(ndl/max(ndl*(1-k)+k,.001));
    float F=f0+(1-f0)*pow(1-vdh,5);
    float3 specular=keyColorRoughness.rgb*keyDirectionIntensity.w*D*G*F/max(4*ndv,.001);
    // Foam has two sources and neither of them is a second simulation:
    //  - contact with the bank. The depth term alone is only one cell wide on a flat bed,
    //    so it is combined with a band along the surface footprint, which is where the
    //    mesh actually ends; the depth term keeps the band from painting open water when
    //    the footprint is larger than the wet area;
    //  - agitation, from the CPU solver's own surface velocity, so a footstep and its wake
    //    raise white water without another solver.
    float2 localXZ=input.world.xz-rippleOriginExtent.xy;
    float2 toEdge=min(localXZ,rippleOriginExtent.zw-localXZ);
    float2 surfaceXZ=input.world.xz;
    float footprint=max(length(ddx(surfaceXZ)),length(ddy(surfaceXZ)));
    float lace=smoothstep(.42,.78,FoamPattern(surfaceXZ,footprint,input.flow));
    // Warp the distance to the edge so the band is not an airbrushed stripe: real froth
    // advances and retreats along the bank.
    float warp=(FoamPattern(surfaceXZ*.55+13.7,footprint,input.flow)-.5)*.09;
    float edgeDistance=max(min(toEdge.x,toEdge.y)+warp,0);
    // Two scales: a bright contact line plus a wide broken froth behind it. The band has to
    // be wider than the last cell of the surface, because that cell sags onto the bed and
    // is depth-clipped; a narrow band would only ever show as a one-pixel outline.
    float shoreLine=1.0-smoothstep(0.02,0.13,edgeDistance);
    float shoreFroth=(1.0-smoothstep(0.05,0.42,edgeDistance))*0.42;
    // Where the mesh actually sags onto the bed there is no column left at all; that last
    // centimetre is the true contact and is always wet foam.
    float shallow=saturate(1.0-input.foamDepth.y/0.05);
    float agitation=saturate(input.foamDepth.x);
    float pattern=lerp(0.55,1.0,lace);
    float foam=saturate(max(shoreLine*pattern,max(shoreFroth*lace,agitation*pattern)));
    foam=saturate(max(foam,shallow*pattern*0.85));
    // Foam scatters instead of reflecting: a neutral, slightly warm surface that keeps the
    // local lighting so it still reads as lit while sitting on the water.
    float3 foamColor=float3(.86,.89,.90)*lighting+skyIor.rgb*.08;
    float3 color=lerp(transmission*(1-fresnel)+reflected*fresnel+specular,foamColor,foam);
    if(grid.z==1)color=n*.5+.5;
    if(grid.z==2)color=saturate(thickness*.35).xxx;
    if(grid.z==3)color=reflected;
    if(grid.z==4)color=transmission;
    if(grid.z==5)color=float3(fresnel,confidence,foam);
    if(grid.z==6)color=rippleHeight.SampleLevel(linearClamp,ResidualUV(input.world),0)*6+float3(.5,.5,.5);
    if(grid.z==7)color=saturate((caustic-1.0)/.16).xxx;
    Output o;o.surface=float4(min(max(color,0),65504),1);o.key=float4(0,0,0,1);return o;
}
