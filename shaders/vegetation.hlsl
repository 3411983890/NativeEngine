// Vegetation: GPU culling, indirect instance draw, double-sided leaf shading.
//
// Two things in here are the point of the file, not decoration:
//   1. The instance list is compacted on the GPU. The CPU uploads the packed page once
//      and never reads a count back, so a 30M world index does not become a 30M CPU loop.
//   2. A leaf is a thin, translucent, double-sided surface. The back face is not "the
//      front face with a flipped normal": it transmits light through the leaf thickness,
//      which is what makes a canopy read as foliage instead of painted cardboard.
//   3. Leaf-on-leaf occlusion is resolved by a key-light shadow map that this module also
//      writes, using the same packed instance list and the same wind displacement. A
//      canopy lit by an unshadowed sun reads as a flat blob no matter how good its albedo.
//
// Vertex record: three float4 per vertex (48 bytes).
//   v0 = position.xyz, windWeight
//   v1 = normal.xyz, 0
//   v2 = uv.xy, packed RGBA8 colour (as uint bits), 0

struct VegetationConstants {
    float4x4 viewProjection;
    // Key-light matrix for the vegetation shadow map. It lives next to the camera matrix in
    // one constant block on purpose: the shadow pass and the main pass are recorded into
    // the same command list, so they must not race on a CPU-written constant buffer.
    float4x4 lightViewProjection;
    float4 cameraPosition;   // xyz = eye, w = seconds
    float4 windDirection;    // xyz = unit direction, w = metres/second
    float4 pageOrigin;       // x = minX, y = baseY, z = minZ, w = wind amplitude
    float4 pageScale;        // x = spanX, y = heightRange, z = spanZ, w = wind frequency
    float4 lightDirection;   // xyz = direction to the key light, w = intensity
    float4 lightColor;       // rgb = key colour, w = ambient
    // w is a bitfield: 1 bark baseColor, 2 leaf baseColor, 4 leaf opacity, 8 normal maps,
    // 16 bark ORM, 32 leaf ORM, 64 leaf thickness, 128 bark height.
    float4 shading;          // x = viewportHeight, y = tanHalfFov, z = alphaCutoff, w = texture bits
    float4 materialBark;     // x = roughness factor, y = thickness metres, z = absorption, w = AO strength
    float4 materialLeaf;     // x = roughness factor, y = thickness metres, z = absorption, w = AO strength
    float4 materialCommon;   // x = normalScale, y = height scale in UV, z = transmission gain, w = doubleSided
    float4 instanceBounds;   // x = height metres, y = radius metres, z = max distance, w = unused
    float4 shadow;           // x = map size, y = depth bias, z = strength, w = normal bias metres
    uint4 counts;            // x = instanceCount, y = visibleCapacity, z = speciesCount, w = texture base
    // Debug channel. x is the mode (0 = shaded); y is a size in cells for the UV checker. The
    // block is 88 DWORDs now; the root signature budget is unaffected because the constants
    // travel as one root CBV rather than root constants.
    float4 debug;            // x = mode, y = checker cells, z/w unused
    // Per-material surface flags: x = bark normal flip (1 = OpenGL-convention sheet), y = leaf.
    float4 materialSurface;  // z/w reserved
};

ConstantBuffer<VegetationConstants> constants : register(b0);

// Texture availability travels as a bitfield in shading.w, which keeps the constant block
// at 56 DWORDs and the root signature inside its 64-DWORD budget.
static bool hasTexture(uint bit) { return ((uint)constants.shading.w & bit) != 0; }

StructuredBuffer<uint> instanceRecords : register(t0); // 8 x uint32 per instance
StructuredBuffer<float4> vertices : register(t1);      // three float4 per vertex

// Material textures. One slot per quantity, in the order the descriptor table binds them.
//   t3 bark baseColor  t4 bark normal  t5 bark ORM  t6 bark height
//   t7 leaf baseColor  t8 leaf normal  t9 leaf ORM  t10 leaf thickness  t11 leaf opacity
// A slot that was not uploaded still has a descriptor (a 1x1 placeholder) and the bitfield in
// shading.w says whether it is real data.
Texture2D<float4> barkBaseColor : register(t3);
Texture2D<float4> barkNormal : register(t4);
Texture2D<float4> barkOrm : register(t5);
Texture2D<float4> barkHeight : register(t6);
Texture2D<float4> leafBaseColor : register(t7);
Texture2D<float4> leafNormal : register(t8);
Texture2D<float4> leafOrm : register(t9);
Texture2D<float4> leafThickness : register(t10);
Texture2D<float4> leafOpacity : register(t11);
// Key-light depth of the same vegetation, written by the shadow pass of this module.
Texture2D<float> shadowDepth : register(t12);
SamplerState materialSampler : register(s0);
// Comparison sampler: "is this fragment farther than the stored caster". Border is white,
// so a fragment projected outside the shadow map reads as fully lit instead of black.
SamplerComparisonState shadowComparison : register(s1);

RWStructuredBuffer<uint> visibleInstances : register(u0);
RWStructuredBuffer<uint> drawArguments : register(u1);
RWStructuredBuffer<uint> visibleCount : register(u2);

static const float kPi = 3.14159265359;

// Decodes one packed 32-byte instance. Field order is the ABI from
// engine/vegetation/region.mjs; a mismatch here would silently rotate or resize a forest.
void decodeInstance(uint index, out float3 position, out float3 axisX, out float3 axisY, out float3 axisZ,
                    out float scale, out uint species, out float phase) {
    uint base = index * 8;
    uint packedA = instanceRecords[base + 0];
    uint packedB = instanceRecords[base + 1];
    uint packedC = instanceRecords[base + 2];
    uint packedD = instanceRecords[base + 3];

    uint qx = packedA & 0xFFFFu;
    uint qz = (packedA >> 16) & 0xFFFFu;
    uint qy = packedB & 0xFFFFu;
    uint yaw = (packedB >> 16) & 0xFFFFu;
    // HLSL has no 16-bit signed type; sign-extend explicitly.
    int tiltX = (int)(packedC & 0xFFFFu);
    if (tiltX >= 32768) tiltX -= 65536;
    int tiltZ = (int)((packedC >> 16) & 0xFFFFu);
    if (tiltZ >= 32768) tiltZ -= 65536;
    uint scaleByte = packedD & 0xFFu;
    species = (packedD >> 8) & 0xFFu;
    uint phaseRaw = (packedD >> 16) & 0xFFFFu;

    position = float3(
        constants.pageOrigin.x + (qx / 65535.0) * constants.pageScale.x,
        constants.pageOrigin.y + (qy / 65535.0) * constants.pageScale.y,
        constants.pageOrigin.z + (qz / 65535.0) * constants.pageScale.z);

    float yawAngle = (yaw / 65536.0) * 2.0 * kPi;
    float tiltXAngle = (tiltX / 32767.0) * (kPi * 0.5);
    float tiltZAngle = (tiltZ / 32767.0) * (kPi * 0.5);
    float cy = cos(yawAngle), sy = sin(yawAngle);
    float cx = cos(tiltXAngle), sx = sin(tiltXAngle);
    float cz = cos(tiltZAngle), sz = sin(tiltZAngle);
    axisY = float3(sx * cz, cx * cz, -sz);
    axisX = float3(cy, 0.0, -sy);
    axisZ = cross(axisX, axisY);
    scale = exp2((scaleByte / 255.0) * 8.0 - 4.0);
    phase = phaseRaw / 65536.0;
}

// Conservative clip-space box test: reject only when every corner is outside one plane.
bool boxOutside(float4 clip[8]) {
    [unroll] for (uint plane = 0; plane < 6; ++plane) {
        bool allOutside = true;
        [unroll] for (uint corner = 0; corner < 8; ++corner) {
            float4 c = clip[corner];
            float value = 0.0;
            if (plane == 0) value = c.x + c.w;
            else if (plane == 1) value = c.w - c.x;
            else if (plane == 2) value = c.y + c.w;
            else if (plane == 3) value = c.w - c.y;
            else if (plane == 4) value = c.z;
            else value = c.w - c.z;
            if (value >= 0.0) { allOutside = false; break; }
        }
        if (allOutside) return true;
    }
    return false;
}

[numthreads(64, 1, 1)]
void CSCull(uint3 dispatchId : SV_DispatchThreadID) {
    uint index = dispatchId.x;
    if (index >= constants.counts.x) return;

    float3 position, axisX, axisY, axisZ;
    float scale, phase;
    uint species;
    decodeInstance(index, position, axisX, axisY, axisZ, scale, species, phase);
    if (species >= constants.counts.z) return;

    // Bounds include the wind excursion, because the culler must not reject a plant that
    // the vertex shader will then displace back into view.
    float height = constants.instanceBounds.x * scale;
    float radius = constants.instanceBounds.y * scale;
    float sway = constants.pageOrigin.w * scale;
    float3 minimum = position + float3(-radius - sway, 0.0, -radius - sway);
    float3 maximum = position + float3(radius + sway, height + sway, radius + sway);

    float4 clip[8];
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        float3 p = float3((corner & 1) ? maximum.x : minimum.x,
                          (corner & 2) ? maximum.y : minimum.y,
                          (corner & 4) ? maximum.z : minimum.z);
        clip[corner] = mul(constants.viewProjection, float4(p, 1.0));
    }
    if (boxOutside(clip)) return;

    float3 toCamera = constants.cameraPosition.xyz - position;
    float viewDistance = length(toCamera);
    if (viewDistance > constants.instanceBounds.z) return;

    uint slot;
    InterlockedAdd(visibleCount[0], 1u, slot);
    if (slot < constants.counts.y) visibleInstances[slot] = index;
}

// Runs after CSCull: turns the append count into the indirect draw arguments and resets
// the counter for the next frame. One thread, one dispatch, no CPU readback.
// IndexCountPerInstance (argument 0) is written once when the species is uploaded and is
// never touched here, so this pass only owns the instance count.
[numthreads(1, 1, 1)]
void CSArgs(uint3 dispatchId : SV_DispatchThreadID) {
    drawArguments[1] = min(visibleCount[0], constants.counts.y);
    visibleCount[0] = 0;
}

[numthreads(1, 1, 1)]
void CSReset(uint3 dispatchId : SV_DispatchThreadID) {
    visibleCount[0] = 0;
}

struct VsOutput {
    float4 position : SV_Position;
    float3 worldNormal : NORMAL0;
    float3 worldPosition : POSITION0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 tangent : TANGENT0;
    float3 localPosition : TEXCOORD1;
    float material : TEXCOORD2;
};

VsOutput buildOutput(uint vertexId, uint instanceId, float4x4 transform) {
    VsOutput output;
    // The visible list is a UAV the cull pass fills. Clamping the index means a stale or
    // partially written list produces a wrong instance instead of an out-of-bounds read
    // that takes the whole device down.
    uint packed = visibleInstances[min(instanceId, max(1u, constants.counts.x) - 1u)];
    float3 position, axisX, axisY, axisZ;
    float scale, phase;
    uint species;
    decodeInstance(packed, position, axisX, axisY, axisZ, scale, species, phase);

    float4 v0 = vertices[vertexId * 3 + 0];
    float4 v1 = vertices[vertexId * 3 + 1];
    float4 v2 = vertices[vertexId * 3 + 2];
    float3 local = v0.xyz;
    float3 localNormal = v1.xyz;
    float windWeight = v0.w;

    float3 world = position + (axisX * local.x + axisY * local.y + axisZ * local.z) * scale;
    float3 worldNormal = normalize(axisX * localNormal.x + axisY * localNormal.y + axisZ * localNormal.z);

    // Bounded procedural sway. The displacement is a function of the vertex's stored wind
    // weight, so the trunk stays planted and only the crown moves; the amplitude is part of
    // the culling bounds above.
    float time = constants.cameraPosition.w;
    float wave = sin(time * constants.pageScale.w + phase * 6.2831 + local.y * 0.35);
    float gust = 0.6 + 0.4 * sin(time * constants.pageScale.w * 0.31 + phase * 3.1);
    world += constants.windDirection.xyz * (wave * gust * windWeight * constants.pageOrigin.w * scale);

    uint packedColor = asuint(v2.z);
    output.color = float4(
        (packedColor & 0xFFu) / 255.0,
        ((packedColor >> 8) & 0xFFu) / 255.0,
        ((packedColor >> 16) & 0xFFu) / 255.0,
        ((packedColor >> 24) & 0xFFu) / 255.0);
    output.worldNormal = worldNormal;
    output.worldPosition = world;
    output.uv = v2.xy;
    output.tangent = normalize(axisX);
    output.localPosition = local;
    // Material index travels per vertex: bark and leaf share one draw, and the pixel
    // shader picks the texture set from this value instead of a second draw call.
    output.material = v1.w;
    output.position = mul(transform, float4(world, 1.0));
    return output;
}

VsOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    return buildOutput(vertexId, instanceId, constants.viewProjection);
}

// Shadow-caster vertex shader. It shares the instance decode, the wind displacement and the
// material attribute with VSMain — a caster that moves differently from the visible mesh is
// what produces crawling shadow edges under wind.
VsOutput VSShadow(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    return buildOutput(vertexId, instanceId, constants.lightViewProjection);
}

// Depth-only, alpha-tested. A leaf card must cast a leaf shadow, not the shadow of the
// rectangle it is stored as, so the same mask cutout runs here.
void PSShadow(VsOutput input) {
    // The caster uses the same cutout as the visible pass: a leaf shadow has to have the
    // leaf's silhouette, not the rectangle's.
    if ((uint)(input.material + 0.5) == 1u && ((uint)constants.shading.w & 4u) != 0u) {
        if (leafOpacity.Sample(materialSampler, input.uv).r < constants.shading.z) discard;
    }
}

// Schlick fresnel, used for the leaf sheen and the bark edge response.
float3 fresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Key-light visibility from the vegetation shadow map: 1 = lit, 0 = fully occluded.
// The lookup is offset along the geometric normal before projecting, because a depth bias
// alone cannot separate a caster from the surface it shades when the light is nearly
// parallel to that surface.
float shadowVisibility(float3 worldPosition, float3 normal) {
    if (constants.shadow.z <= 0.0) return 1.0;
    float3 offset = worldPosition + normal * constants.shadow.w;
    float4 clip = mul(constants.lightViewProjection, float4(offset, 1.0));
    float3 projected = clip.xyz / max(clip.w, 1e-5);
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    // Outside the light frustum is "not covered by this map", not "in shadow": a partial
    // map must never darken what it did not render.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) return 1.0;
    float reference = projected.z - constants.shadow.y;
    float texel = 1.0 / max(constants.shadow.x, 1.0);
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            sum += shadowDepth.SampleCmpLevelZero(shadowComparison, uv + float2(x, y) * texel, reference);
        }
    }
    return sum * (1.0 / 9.0);
}

float4 PSMain(VsOutput input, bool frontFace : SV_IsFrontFace) : SV_Target {
    const uint material = (uint)(input.material + 0.5);
    const bool isLeaf = material == 1;
    const uint bits = (uint)constants.shading.w;
    const float4 materialFactors = isLeaf ? constants.materialLeaf : constants.materialBark;
    float2 uv = input.uv;

    // Debug view mode. 11 and 12 are passes, not shading: they remove one material class from
    // the frame so "is the fork connected" and "is the leaf on its petiole" can be judged without
    // foliage in the way. Everything else is evaluated after the surface terms are known.
    const uint debugMode = (uint)(constants.debug.x + 0.5);
    if (debugMode == 11u && isLeaf) discard;
    if (debugMode == 12u && !isLeaf) discard;

    float3 geometric = normalize(input.worldNormal);
    if (!frontFace) geometric = -geometric;
    float3 toEye = normalize(constants.cameraPosition.xyz - input.worldPosition);

    // Bark parallax. Height is a relative surface offset, not a displacement of the trunk
    // silhouette, so it may only move the *texture lookup* — never the geometry, never the
    // depth used by the shadow pass. The offset is in UV units, scaled by the tile size so
    // 1.6 m of trunk always shows the same relief.
    if (!isLeaf && (bits & 128u) != 0u) {
        float3 tangent0 = normalize(input.tangent - geometric * dot(geometric, input.tangent));
        float3 bitangent0 = cross(geometric, tangent0);
        float2 viewTangent = float2(dot(toEye, tangent0), dot(toEye, bitangent0));
        float height = barkHeight.SampleLevel(materialSampler, uv, 0).r - 0.5;
        uv += viewTangent * height * constants.materialCommon.y;
    }

    // Cutout. Foliage coverage comes from the opacity sheet; bark is opaque and has no
    // alpha test at all, so a stray alpha value cannot punch holes in a trunk.
    float coverage = 1.0;
    if (isLeaf) {
        coverage = (bits & 4u) != 0u
            ? leafOpacity.Sample(materialSampler, uv).r
            : input.color.a;
    }
    if (isLeaf && coverage < constants.shading.z) discard;

    // Base colour. The baked vertex colour carries the per-leaf tint and the interior/exterior
    // variation; the map carries the detail. Multiplying them keeps both without a second map.
    //
    // A species with no colour map (grass, and the conifer's untextured needles) falls back to
    // the vertex colour alone. Those bytes are authored display-referred, so they are converted
    // to linear here; without the conversion an untextured clump renders roughly one stop
    // brighter than a textured one and reads as white plastic rather than foliage.
    float3 albedo = input.color.rgb;
    if (isLeaf) {
        if ((bits & 2u) != 0u) albedo = leafBaseColor.Sample(materialSampler, uv).rgb * (input.color.rgb * 2.2);
        else albedo = pow(saturate(albedo), 2.2);
    } else if ((bits & 1u) != 0u) {
        albedo = barkBaseColor.Sample(materialSampler, uv).rgb;
    } else {
        albedo = pow(saturate(albedo), 2.2);
    }

    // ORM: R = ambient occlusion, G = roughness, B = metallic. Vegetation metallic is 0 by
    // contract, so it is read and then used only to prove it is 0 (see the f0 blend below).
    float4 orm = float4(1.0, materialFactors.x, 0.0, 1.0);
    if (isLeaf && (bits & 32u) != 0u) orm = leafOrm.Sample(materialSampler, uv);
    else if (!isLeaf && (bits & 16u) != 0u) orm = barkOrm.Sample(materialSampler, uv);
    const float occlusion = lerp(1.0, orm.r, materialFactors.w);
    const float metallic = saturate(orm.b);
    // Roughness is the authored factor modulated by the map, not a second unexplained curve.
    const float roughness = clamp(materialFactors.x * orm.g, 0.04, 1.0);

    // Normal map, scaled once by the material's normalScale. The green channel carries the
    // author's convention: a `directX-` sheet stores green pointing down the surface and an
    // `openGL+` sheet stores it pointing up, so the flip happens exactly once here and is driven
    // by the material, not by a global guess about the image.
    float3 normalSample = float3(0.0, 0.0, 1.0);
    if ((bits & 8u) != 0u) {
        const float3 raw = isLeaf
            ? leafNormal.Sample(materialSampler, uv).xyz
            : barkNormal.Sample(materialSampler, uv).xyz;
        const float flipGreen = isLeaf ? constants.materialSurface.y : constants.materialSurface.x;
        const float2 tangentXY = float2(raw.x, flipGreen > 0.5 ? 1.0 - raw.y : raw.y);
        normalSample = float3((tangentXY * 2.0 - 1.0) * constants.materialCommon.x, raw.z * 2.0 - 1.0);
        normalSample = normalize(normalSample);
    }

    // Tangent frame. `input.tangent` is the instance X axis, orthogonalised against the
    // geometric normal; it is stable per instance, which is what a tangent-space normal map
    // needs to keep its orientation across a crown.
    float3 tangent = input.tangent - geometric * dot(geometric, input.tangent);
    tangent = length(tangent) > 1e-4 ? normalize(tangent) : normalize(cross(geometric, float3(0.0, 1.0, 0.0)));
    float3 bitangent = cross(geometric, tangent);
    float3 normal = normalize(tangent * normalSample.x + bitangent * normalSample.y + geometric * normalSample.z);
    if (dot(normal, geometric) < 0.0) normal = -normal;

    float3 light = normalize(constants.lightDirection.xyz);
    float3 halfway = normalize(light + toEye);

    // Sky/ground ambient. AO scales the indirect term only: multiplying direct light by AO is
    // how a canopy turns into a grey blob instead of a shadowed one.
    float up = saturate(normal.y * 0.5 + 0.5);
    float3 skyAmbient = constants.lightColor.rgb * 0.55 + float3(0.10, 0.16, 0.24);
    float3 groundAmbient = float3(0.11, 0.09, 0.06);
    float3 ambient = lerp(groundAmbient, skyAmbient, up) * constants.lightColor.w * occlusion;

    // Wrapped diffuse: foliage scatters, so an edge-on leaf still receives some key light.
    float wrap = isLeaf ? 0.45 : 0.12;
    float diffuseTerm = saturate((dot(normal, light) + wrap) / (1.0 + wrap));
    // Interior darkening: the inside of a crown is darker than its silhouette. This stands in
    // for the leaf-on-leaf contact that a shadow map texel is too coarse to resolve.
    float depthTint = lerp(0.72, 1.0, saturate(input.localPosition.y * 0.9 + 0.25));

    float visibility = 1.0;
    if (constants.shadow.z > 0.0) {
        visibility = lerp(1.0, shadowVisibility(input.worldPosition, geometric), saturate(constants.shadow.z));
    }
    float3 diffuse = albedo * constants.lightColor.rgb * (diffuseTerm * constants.lightDirection.w) * depthTint * visibility;

    // Thickness transmission. The optical depth is absorption (1/m) x thickness (m) divided by
    // the cosine of the light, with the grazing denominator clamped: a leaf seen edge-on does
    // not become infinitely bright. A thicker or more absorptive leaf transmits less — the
    // opposite of the old "thickness x 260" term, which brightened as the map got whiter.
    float3 transmitted = float3(0.0, 0.0, 0.0);
    // Effective thickness in metres: the material's scale times the relative thickness map.
    // Hoisted out of the transmission branch so the debug view can show the same number the
    // shading uses instead of a second, independently computed one.
    float thicknessMetres = isLeaf ? materialFactors.y : materialFactors.y;
    if (isLeaf && (bits & 64u) != 0u) thicknessMetres *= leafThickness.Sample(materialSampler, uv).r;
    if (isLeaf) {
        const float back = saturate(dot(-normal, light));
        const float opticalDepth = materialFactors.z * thicknessMetres / max(0.15, back);
        const float transmittance = exp(-opticalDepth);
        const float transmittedVisibility = lerp(1.0, visibility, 0.35);
        transmitted = albedo * constants.lightColor.rgb
            * (back * transmittance * constants.lightDirection.w * 1.4 * constants.materialCommon.z)
            * transmittedVisibility;
    }

    // Specular. f0 comes from metallic (which is 0 for vegetation) so the term stays dielectric.
    const float alpha = max(0.02, roughness * roughness);
    const float ndoth = saturate(dot(normal, halfway));
    const float distribution = alpha / (3.14159265 * pow(ndoth * ndoth * (alpha - 1.0) + 1.0, 2.0));
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float3 fresnel = fresnelSchlick(saturate(dot(halfway, toEye)), f0);
    const float specularScale = (isLeaf ? 0.35 : 0.12) * (1.0 - roughness * 0.5);
    const float3 specularColor = fresnel * constants.lightColor.rgb
        * (distribution * specularScale * constants.lightDirection.w) * visibility;

    float3 color = ambient * albedo + diffuse + transmitted + specularColor;

    // Debug views read the same values the shaded path uses, in the same order, so "the debug
    // view says AO is 1" and "the shaded frame has no occlusion" cannot both be true.
    // 11 and 12 are view *filters*: they removed one material class above and then shade what is
    // left normally. Falling into this block would return black and make both views look empty.
    const bool isFilterView = debugMode == 11u || debugMode == 12u;
    if (debugMode != 0u && !isFilterView) {
        float3 view = float3(0.0, 0.0, 0.0);
        if (debugMode == 1u) {
            view = albedo;
        } else if (debugMode == 2u) {
            // Checker in UV space: the number of cells is a shader constant, so a cell can be
            // made large enough to read the sheet orientation (and a mirrored V is obvious).
            const float cells = max(1.0, constants.debug.y);
            const float2 at = floor(uv * cells);
            const float odd = frac((at.x + at.y) * 0.5) > 0.25 ? 1.0 : 0.0;
            view = float3(odd, odd, odd);
        } else if (debugMode == 3u) {
            view = geometric * 0.5 + 0.5;
        } else if (debugMode == 4u) {
            view = normal * 0.5 + 0.5;
        } else if (debugMode == 5u) {
            view = occlusion.xxx;
        } else if (debugMode == 6u) {
            view = roughness.xxx;
        } else if (debugMode == 7u) {
            view = metallic.xxx;
        } else if (debugMode == 8u) {
            view = coverage.xxx;
        } else if (debugMode == 9u) {
            // Grey level is 0..1 mm of effective thickness; bark saturates, a leaf does not.
            view = saturate(thicknessMetres / 0.001).xxx;
        } else if (debugMode == 10u) {
            const float height = (bits & 128u) != 0u ? barkHeight.Sample(materialSampler, uv).r : 0.5;
            view = height.xxx;
        } else if (debugMode == 13u) {
            const float depth = distance(constants.cameraPosition.xyz, input.worldPosition);
            view = saturate(depth / max(1.0, constants.instanceBounds.z)).xxx;
        } else if (debugMode == 14u) {
            view = visibility.xxx;
        }
        return float4(view, 1.0);
    }
    return float4(color, 1.0);
}
