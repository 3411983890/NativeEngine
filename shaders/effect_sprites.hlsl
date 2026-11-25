cbuffer SpriteConstants : register(b0) {
    column_major float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    uint spriteOffset;
    uint textureIndex;
    uint blendKind;
    uint padding;
};
struct Sprite { float4 positionWidth; float4 color; float4 shape; };
StructuredBuffer<Sprite> sprites : register(t0);
SamplerState spriteSampler : register(s0);
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
#if defined(PASS_VS)
PixelInput VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    const Sprite sprite = sprites[spriteOffset+instance];
    const float2 uv = corners[vertex];
    float2 offset = (uv-float2(0.5,0.5))*float2(sprite.positionWidth.w,sprite.shape.x);
    float sine, cosine; sincos(sprite.shape.y,sine,cosine);
    offset = float2(offset.x*cosine-offset.y*sine,offset.x*sine+offset.y*cosine);
    const float3 world = sprite.positionWidth.xyz+cameraRight.xyz*offset.x-cameraUp.xyz*offset.y;
    PixelInput output; output.position=mul(viewProjection,float4(world,1)); output.uv=uv; output.color=sprite.color;
    return output;
}
#endif
#if defined(PASS_PS)
struct PixelOutput { float4 surface : SV_TARGET0; float4 keyLight : SV_TARGET1; };
PixelOutput PSMain(PixelInput input) {
    Texture2D spriteTexture = ResourceDescriptorHeap[textureIndex];
    float4 texel = spriteTexture.Sample(spriteSampler,input.uv);
    float alpha = saturate(texel.a*input.color.a);
    clip(alpha-(blendKind==2?0.5:0.0001));
    if (blendKind==2) alpha=1;
    PixelOutput output;
    output.surface=float4(texel.rgb*input.color.rgb*alpha,alpha);
    output.keyLight=float4(0,0,0,alpha);
    return output;
}
#endif
