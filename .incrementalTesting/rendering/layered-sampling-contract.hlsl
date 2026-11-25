#include "../../shaders/material_layer_sampling.hlsli"
StructuredBuffer<LayerMaterial> layers : register(t0);
struct Input { float4 position:SV_POSITION; float2 uv:TEXCOORD0; float3 world:TEXCOORD1; float3 normal:NORMAL; float4 color:COLOR0; };
float4 PSMain(Input input) : SV_TARGET {
    LayerCoordinates coords=(LayerCoordinates)0;
    [unroll] for(uint i=0;i<16;i++){coords.uv[i]=input.uv;coords.dx[i]=ddx(input.uv);coords.dy[i]=ddy(input.uv);}
    coords.normal=input.normal;coords.positionDx=ddx(input.world);coords.positionDy=ddy(input.world);coords.vertexColor=input.color;
    LayerSurface surface=LayerSampleSurface(layers[0],coords);
    surface=LayerApply(surface,layers[1],coords);
    return float4(surface.albedo+surface.emissive,surface.ao);
}
