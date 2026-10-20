// DXR static shadow pass.
//
// The raster pass fills a depth buffer and separates its key-light contribution into
// its own render target. This library traces one shadow ray per screen pixel toward the
// fixed key light and writes a visibility mask. The composite pass (post.hlsl) is the
// only place the mask is applied, so the exact same raster image is produced when the
// mask is forced to 1.0 for the pure-raster fallback.
//
// Reconstructing world position from depth keeps the shadow pass independent of the
// geometry path: box instances, a future mesh-shader path and any imported mesh all use
// the same acceleration structure and the same screen-space shadow query.

struct ShadowConstants
{
    column_major float4x4 invViewProjection; // clip -> world
    float4 cameraPosition;                   // xyz = eye, w = unused
    float4 keyLightDirection;                // xyz = normalized direction toward light, w = origin bias
    float4 traceParams;                      // x, y = target pixel size, z = max distance, w = enabled
    float4 depthParams;                      // x = near, y = far, z = 1/width, w = 1/height
};

ConstantBuffer<ShadowConstants> gShadow : register(b0);
Texture2D<float> gDepth : register(t0);
RaytracingAccelerationStructure gScene : register(t1);
RWTexture2D<float> gVisibility : register(u0);

struct ShadowPayload
{
    float visible; // 1 = light reaches the surface, 0 = occluded
};

float3 ReconstructWorld(uint2 pixel, uint2 size)
{
    float2 uv = (float2(pixel) + 0.5) / float2(size);
    // The host uses a left-handed projection with a top-left origin NDC, matching D3D.
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float depth = gDepth.Load(int3(pixel, 0));
    float4 clip = float4(ndc, depth, 1.0);
    float4 world = mul(gShadow.invViewProjection, clip);
    return world.xyz / max(world.w, 1e-6);
}

[shader("raygeneration")]
void ShadowRayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;
    if (any(pixel >= size))
    {
        return;
    }
    if (gShadow.traceParams.w < 0.5)
    {
        gVisibility[pixel] = 1.0;
        return;
    }
    float depth = gDepth.Load(int3(pixel, 0));
    if (depth >= 0.999999)
    {
        gVisibility[pixel] = 1.0; // sky: nothing to shadow
        return;
    }

    float3 center = ReconstructWorld(pixel, size);
    // Geometric normal from two neighbouring pixels. Neighbour depth is clamped so a
    // silhouette does not produce NaN; the camera-facing flip then fixes the sign.
    uint2 rightPixel = min(pixel + uint2(1, 0), size - 1);
    uint2 downPixel = min(pixel + uint2(0, 1), size - 1);
    float3 right = ReconstructWorld(rightPixel, size);
    float3 down = ReconstructWorld(downPixel, size);
    float3 tangentX = right - center;
    float3 tangentY = down - center;
    float3 normal = float3(0.0, 1.0, 0.0);
    float3 crossProduct = cross(tangentX, tangentY);
    if (dot(crossProduct, crossProduct) > 1e-16)
    {
        normal = normalize(crossProduct);
    }
    if (dot(normal, center - gShadow.cameraPosition.xyz) > 0.0)
    {
        normal = -normal;
    }

    float3 toLight = normalize(gShadow.keyLightDirection.xyz);
    if (dot(normal, toLight) <= 0.0)
    {
        // Surface faces away from the key light: the raster key term is already zero and
        // tracing here would only produce self-intersection noise.
        gVisibility[pixel] = 0.0;
        return;
    }

    RayDesc ray;
    ray.Origin = center + normal * max(gShadow.keyLightDirection.w, 0.0);
    ray.Direction = toLight;
    ray.TMin = 0.0;
    ray.TMax = max(gShadow.traceParams.z, 1.0);

    ShadowPayload payload;
    payload.visible = 1.0;
    // Any hit ends the search; the closest-hit shader records occlusion in the payload.
    TraceRay(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 0, 0, ray, payload);
    gVisibility[pixel] = payload.visible;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.visible = 1.0;
}

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    payload.visible = 0.0;
}
