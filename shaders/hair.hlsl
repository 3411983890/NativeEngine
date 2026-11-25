// Strand draw for the cooked hairstyle (nativegpu.hair-gpu-pack/1).
//
// One non-indexed draw of six vertices per segment; the vertex shader expands each
// segment into a camera-facing ribbon from the strand record and the point records the
// cook wrote. The pixel shader is the GPU implementation of engine/hair/shading.mjs:
//   * coverage-corrected alpha, so a sub-pixel strand is one partially transparent pixel
//     instead of a solid band,
//   * dual-lobe Kajiya-Kay plus a forward transmission lobe,
//   * deep-opacity self-shadow sampled from the light-space layer grid,
//   * debug channels that expose coverage, tangent, root-to-tip, id, depth and shadow.
//
// Not implemented here, and not claimed: order-independent transparency or a sorted
// transparency cache. Strands composite in upload order. `HairDrawStats` reports that.

cbuffer HairConstants : register(b0) {
    column_major float4x4 viewProjection;
    float4 cameraPosition;   // xyz
    float4 lightDirection;   // xyz toward the light, w intensity
    float4 lightColour;      // rgb
    float4 albedo;           // rgb, w calibrated longitudinal roughness
    float4 material;         // radial roughness, shift radians, root-to-tip scale, specular scale
    float4 material2;        // diffuse scale, transmission scale, ambient, shadow strength
    float4 shadowU;          // light-space u axis xyz, w grid size
    float4 shadowV;          // light-space v axis xyz, w layer count
    float4 shadowOrigin;     // uMin, vMin, texel size, sigma per metre
    float4 viewport;         // width, height, tanHalfFovY, near metres
    float4 options;          // min width px, max width px, alpha minimum, debug view
    uint4 counts;            // strands, points, segments per strand, flags
    float4 ambientColour;
    float4 rootTint;         // rgb tip tint
    column_major float4x4 headTransform;
    column_major float4x4 inverseHead;
    float4 shadowLight;      // world light rotated into the hair rest space
};

StructuredBuffer<uint4> pointRecords : register(t0);
StructuredBuffer<uint4> strandRecords : register(t1);
StructuredBuffer<float2> shadowLayers : register(t2);
// Draw order: back-to-front root depth. The geometry stays in upload order; only this
// index list changes, so sorting does not cost a geometry re-upload.
StructuredBuffer<uint> strandOrder : register(t3);

struct PixelInput {
    float4 position : SV_POSITION;
    float3 world : TEXCOORD0;
    float3 tangent : TEXCOORD1;
    float rootToTip : TEXCOORD2;
    float alpha : TEXCOORD3;
    nointerpolation uint stableId : TEXCOORD4;
    nointerpolation uint materialGroup : TEXCOORD5;
};

void loadPoint(uint index, out float3 position, out float3 tangent, out float width, out float rootToTip) {
    uint4 first = pointRecords[index * 2];
    uint4 second = pointRecords[index * 2 + 1];
    position = asfloat(first.xyz);
    tangent = asfloat(uint3(first.w, second.x, second.y));
    width = asfloat(second.z);
    rootToTip = asfloat(second.w);
}

float3 safeNormalize(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 1e-12 ? value * rsqrt(lengthSquared) : fallback;
}

float fiberLobe(float3 shiftedTangent, float3 halfVector, float exponent, float radialExponent) {
    float cosine = dot(shiftedTangent, halfVector);
    float sine = sqrt(max(0, 1 - cosine * cosine));
    return pow(sine, exponent) * pow(max(0, cosine), radialExponent * 0.15);
}

#if defined(PASS_VS)
PixelInput VSMain(uint id : SV_VertexID) {
    PixelInput output;
    output.position = float4(0, 0, 0, 1);
    output.world = 0; output.tangent = float3(0, 0, 1);
    output.rootToTip = 0; output.alpha = 0;
    output.stableId = 0; output.materialGroup = 0;

    uint segments = max(counts.z, 1u);
    uint segment = id / 6;
    uint corner = id % 6;
    uint strandSlot = segment / segments;
    uint local = segment % segments;
    if (strandSlot >= counts.x) return output;
    // The draw order is the order of this index list, not the geometry order.
    uint strandIndex = strandOrder[strandSlot];
    if (strandIndex >= counts.x) return output;

    uint4 strandFirst = strandRecords[strandIndex * 2];
    uint4 strandSecond = strandRecords[strandIndex * 2 + 1];
    uint firstPoint = strandFirst.x;
    if (local + 1 >= strandFirst.y) return output;

    float3 position0, tangent0, position1, tangent1;
    float width0, width1, root0, root1;
    loadPoint(firstPoint + local, position0, tangent0, width0, root0);
    loadPoint(firstPoint + local + 1, position1, tangent1, width1, root1);
    // Rigid attachment to the head bone. The solver stays in rest space; the world
    // transform happens here and applies to both the position and the tangent.
    position0 = mul(headTransform, float4(position0, 1)).xyz;
    position1 = mul(headTransform, float4(position1, 1)).xyz;
    tangent0 = normalize(mul((float3x3)headTransform, tangent0));
    tangent1 = normalize(mul((float3x3)headTransform, tangent1));

    float4 clip0 = mul(viewProjection, float4(position0, 1));
    float4 clip1 = mul(viewProjection, float4(position1, 1));
    if (clip0.w <= viewport.w || clip1.w <= viewport.w) return output;

    // The packed width is a diameter in metres. Converting it to pixels with the same
    // frustum the projection uses is what makes the width clamp meaningful; a constant
    // metres-to-pixels factor would scale with distance and never clamp at close range.
    float metresPerPixel0 = 2.0 * clip0.w * viewport.z / viewport.y;
    float metresPerPixel1 = 2.0 * clip1.w * viewport.z / viewport.y;
    float pixels0 = width0 / metresPerPixel0;
    float pixels1 = width1 / metresPerPixel1;
    float draw0 = clamp(pixels0, options.x, options.y);
    float draw1 = clamp(pixels1, options.x, options.y);
    float alpha0 = saturate(pixels0 / max(draw0, 1e-4));
    float alpha1 = saturate(pixels1 / max(draw1, 1e-4));

    float3 segmentDirection = safeNormalize(position1 - position0, float3(0, -1, 0));
    float3 midpoint = 0.5 * (position0 + position1);
    float3 toEye = safeNormalize(cameraPosition.xyz - midpoint, float3(0, 0, 1));
    float3 side = cross(segmentDirection, toEye);
    if (dot(side, side) < 1e-8) side = cross(segmentDirection, float3(0, 1, 0));
    side = safeNormalize(side, float3(1, 0, 0));

    float halfWorld0 = 0.5 * draw0 * metresPerPixel0;
    float halfWorld1 = 0.5 * draw1 * metresPerPixel1;
    float3 offset0 = side * halfWorld0;
    float3 offset1 = side * halfWorld1;
    float3 world;
    float tangentMix;
    if (corner == 0) { world = position0 + offset0; tangentMix = 0; }
    else if (corner == 1) { world = position1 + offset1; tangentMix = 1; }
    else if (corner == 2) { world = position1 - offset1; tangentMix = 1; }
    else if (corner == 3) { world = position0 + offset0; tangentMix = 0; }
    else if (corner == 4) { world = position1 - offset1; tangentMix = 1; }
    else { world = position0 - offset0; tangentMix = 0; }

    output.position = mul(viewProjection, float4(world, 1));
    output.world = world;
    output.tangent = normalize(lerp(tangent0, tangent1, tangentMix));
    output.rootToTip = lerp(root0, root1, tangentMix);
    output.alpha = lerp(alpha0, alpha1, tangentMix);
    output.stableId = strandFirst.w;
    output.materialGroup = strandFirst.z;
    return output;
}
#endif

#if defined(PASS_PS)
float shadowThicknessAt(float3 restWorld, float3 lightDirectionNormalized) {
    if (material2.w <= 0.0) return 0.0;
    uint size = (uint)shadowU.w;
    uint layers = (uint)shadowV.w;
    if (size == 0 || layers == 0) return 0.0;
    float2 lightUv = float2(dot(restWorld, shadowU.xyz), dot(restWorld, shadowV.xyz));
    float2 local = (lightUv - shadowOrigin.xy) / shadowOrigin.z;
    int2 cell = (int2)floor(local);
    if (cell.x < 0 || cell.y < 0 || cell.x >= (int)size || cell.y >= (int)size) return 0.0;
    uint base = (uint)(cell.y) * size + (uint)(cell.x);
    float depth = dot(restWorld, lightDirectionNormalized);
    float thickness = 0;
    uint baseLayer = base * layers;
    [loop] for (uint slot = 0; slot < layers; ++slot) {
        float2 layer = shadowLayers[baseLayer + slot];
        // A layer closer to the light is not between the light and this point. The point's
        // own layer is excluded so a strand never shadows itself.
        if (layer.x < depth - shadowOrigin.z * 0.75) thickness += layer.y;
    }
    return thickness;
}

float4 PSMain(PixelInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET {
    float alpha = input.alpha;
    if (alpha < options.z) discard;
    uint debugView = (uint)options.w;

    if (debugView == 1) return float4(alpha, alpha, alpha, alpha);

    float3 tangent = safeNormalize(input.tangent, float3(0, -1, 0));
    float3 viewDirection = safeNormalize(cameraPosition.xyz - input.world, float3(0, 0, 1));
    float3 normal = safeNormalize(viewDirection - tangent * dot(viewDirection, tangent), float3(0, 1, 0));
    if (!frontFace) normal = -normal;
    float3 lightDirectionNormalized = safeNormalize(lightDirection.xyz, float3(0, 1, 0));
    float3 restWorld = mul(inverseHead, float4(input.world, 1)).xyz;
    float3 restLight = safeNormalize(shadowLight.xyz, lightDirectionNormalized);
    float thickness = shadowThicknessAt(restWorld, restLight);

    if (debugView == 2) return float4(tangent * 0.5 + 0.5, 1);
    if (debugView == 3) return float4(input.rootToTip, input.rootToTip * 0.5, 0, 1);
    if (debugView == 4) {
        uint hash = input.stableId * 2654435761u;
        return float4((hash & 255) / 255.0, ((hash >> 8) & 255) / 255.0, ((hash >> 16) & 255) / 255.0, 1);
    }
    if (debugView == 5) {
        float group = frac(input.materialGroup * 0.6180339);
        return float4(group, 1 - group, 0.25, 1);
    }
    if (debugView == 6) {
        float distanceToEye = length(cameraPosition.xyz - input.world);
        float value = saturate(1 - distanceToEye / 0.6);
        return float4(value, value, value, 1);
    }
    if (debugView == 7) {
        float value = saturate(thickness * 24);
        return float4(value, value, value, 1);
    }

    float3 transmittance = exp(-shadowOrigin.w * thickness * float3(1, 1, 1));
    transmittance = lerp(float3(1, 1, 1), transmittance, saturate(material2.w));
    float3 direct = lightColour.rgb * lightDirection.w * transmittance;

    // Dual-lobe Kajiya-Kay with a Marschner-style azimuthal shift. Both lobes use the same
    // half vector; the shift is what separates the R and TRT highlights on a cylinder.
    float3 halfVector = safeNormalize(viewDirection + lightDirectionNormalized, viewDirection);
    float longitudinal = max(1e-3, albedo.w);
    float radial = max(1e-3, material.x);
    float exponent = max(1.0, 2.0 / (longitudinal * longitudinal) - 2.0);
    float radialExponent = max(1.0, 2.0 / (radial * radial) - 2.0);
    float primary = fiberLobe(safeNormalize(tangent + normal * material.y, tangent), halfVector,
        exponent, radialExponent);
    float secondary = fiberLobe(safeNormalize(tangent - normal * material.y, tangent), halfVector,
        exponent, radialExponent);
    // Kajiya-Kay diffuse: a fibre is a cylinder, so the diffuse term follows sin(T, L).
    float fibreDiffuse = sqrt(max(0, 1 - pow(dot(tangent, lightDirectionNormalized), 2)));
    // Forward transmission is strongest when the light is behind the fibre relative to
    // the viewer; a lamp in front produces almost none, which is what makes backlight read.
    float backFacing = saturate(-dot(viewDirection, lightDirectionNormalized) * 0.5 + 0.5);
    float transmission = pow(backFacing, 2.4) * (1 - pow(abs(dot(tangent, viewDirection)), 2) * 0.6);
    float3 tint = 1 - (1 - rootTint.rgb) * input.rootToTip * material.z;

    float3 diffuse = albedo.rgb * fibreDiffuse * material2.x * direct;
    float3 specular = (primary + secondary * 0.35) * material.w * direct * (0.55 + 0.45 * albedo.rgb);
    float3 forward = albedo.rgb * transmission * material2.y * direct;
    float3 radiance = (diffuse + specular + forward) * tint + albedo.rgb * ambientColour.rgb * material2.z;
    return float4(radiance * alpha, alpha);
}
#endif
