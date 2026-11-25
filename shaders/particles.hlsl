// FIELD-PLAN v0.2 + FIELD-REALISM-GOAL: particle simulation, deterministic trilinear field
// splat, volumetric resolve, and a physically grounded ray-marched composite.
//
// Passes in this file, compiled as separate blobs:
//   PASS_SIMULATE  : one thread per particle. Integrates the particle, perturbs it with
//                    curl-noise turbulence, and splats it into the density/heat volumes with
//                    a 2x2x2 trilinear tent kernel using *integer*
//                    atomics. The kernel weights depend only on the particle position, and
//                    integer addition is associative and commutative, so the accumulated
//                    field does not depend on thread order -- the determinism claim in
//                    docs/FIELD-PLAN.md section 7.2, made executable.
//   PASS_RESOLVE   : converts the integer volumes into a filterable RGBA16F volume.
//   PASS_VOLUME    : fullscreen ray march of that volume. Physically grounded shading:
//                    Planckian blackbody emission (independent of the sun), Henyey-Greenstein
//                    phase for the key light, a two-octave multi-scattering approximation,
//                    the fire as an internal point light, and a per-pixel jittered march
//                    origin that breaks up banding. Used directly at full resolution
//                    (--particle-temporal off) or into the half-res target.
//   PASS_ACCUMULATE: exponential temporal accumulation of the half-res march into history.
//   PASS_COMPOSITE : depth-aware bilateral upsample of the history over the scene colour.

struct Particle {
    float3 position;
    float  temperature;
    float3 velocity;
    float  age;
};

#ifdef PASS_SIMULATE
struct Collider {
    float4 center;
    float4 extent;
};

cbuffer SimConstants : register(b0) {
    float4 gParams;    // x dt, y elapsed, z count, w frame index
    float4 gEmitter;   // xyz centre, w radius
    float4 gWind;      // xyz wind, w gravity
    float4 gVolume;    // xyz min, w extent
    float4 gScales;    // x densityQuantum, y heatQuantum, z dim, w lifetime
    float4 gMisc;      // x burst, y buoyancy, z sink, w turbulence
    float4 gColliderInfo;  // x count
};

RWStructuredBuffer<Particle> gParticles : register(u0);
RWTexture3D<uint> gVolumeDensity : register(u1);
RWTexture3D<uint> gVolumeHeat : register(u2);
StructuredBuffer<Collider> gColliderBuffer : register(t0);

uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rand01(uint seed) {
    return (hashUint(seed) & 0x00FFFFFFu) / 16777215.0;
}

// Value noise over an integer lattice, built on hashUint so it costs a few ALU ops and
// needs no texture. The domain is offset far from zero to keep the lattice coordinates
// positive before the uint cast.
float vnoise(float3 p) {
    float3 shifted = p + 4096.0;
    float3 pf = floor(shifted);
    uint3 i = uint3((uint)pf.x, (uint)pf.y, (uint)pf.z);
    float3 f = frac(shifted);
    f = f * f * (3.0 - 2.0 * f);
    uint base = i.x * 0x8da6b343u + i.y * 0xd8163841u + i.z * 0xcb1ab31fu;
    float c000 = rand01(base);
    float c100 = rand01(base + 0x8da6b343u);
    float c010 = rand01(base + 0xd8163841u);
    float c110 = rand01(base + 0x8da6b343u + 0xd8163841u);
    float c001 = rand01(base + 0xcb1ab31fu);
    float c101 = rand01(base + 0xcb1ab31fu + 0x8da6b343u);
    float c011 = rand01(base + 0xcb1ab31fu + 0xd8163841u);
    float c111 = rand01(base + 0xcb1ab31fu + 0x8da6b343u + 0xd8163841u);
    float c00 = lerp(c000, c100, f.x);
    float c10 = lerp(c010, c110, f.x);
    float c01 = lerp(c001, c101, f.x);
    float c11 = lerp(c011, c111, f.x);
    return lerp(lerp(c00, c10, f.y), lerp(c01, c11, f.y), f.z);
}

// Curl of three decorrelated noise potentials: a divergence-free turbulence vector, so it
// stirs the plume into tongues and eddies without pumping net volume into the field.
float3 curlNoise(float3 p) {
    const float e = 0.35;
    float3 ex = float3(e, 0.0, 0.0);
    float3 ey = float3(0.0, e, 0.0);
    float3 ez = float3(0.0, 0.0, e);
    float3 o1 = float3(17.13, 5.71, 9.37);
    float3 o2 = float3(3.97, 23.11, 13.53);
    float dpsi1dy = vnoise(p + ey) - vnoise(p - ey);
    float dpsi1dz = vnoise(p + ez) - vnoise(p - ez);
    float dpsi2dx = vnoise(p + o1 + ex) - vnoise(p + o1 - ex);
    float dpsi2dz = vnoise(p + o1 + ez) - vnoise(p + o1 - ez);
    float dpsi3dx = vnoise(p + o2 + ex) - vnoise(p + o2 - ex);
    float dpsi3dy = vnoise(p + o2 + ey) - vnoise(p + o2 - ey);
    return float3(dpsi3dy - dpsi2dz, dpsi1dz - dpsi3dx, dpsi2dx - dpsi1dy) / (2.0 * e);
}

void respawn(inout Particle p, uint index) {
    uint seed = index * 1973u + (uint)gParams.w * 9277u + 1u;
    float a = rand01(seed);
    float b = rand01(seed ^ 0x9E3779B9u);
    float c = rand01(seed ^ 0x85EBCA6Bu);
    float2 direction = float2(cos(a * 6.2831853), sin(a * 6.2831853));
    // gMisc.x is the explosion burst: it widens the source, throws particles outward and
    // makes them hotter for a few frames, then decays back to 0.
    float burst = gMisc.x;
    float radius = gEmitter.w * (1.0 + burst * 9.0) * sqrt(b);
    p.position = gEmitter.xyz + float3(direction.x * radius, c * (0.1 + burst * 0.4), direction.y * radius);
    float outward = 0.18 + burst * 7.0;
    p.velocity = float3(direction.x * outward, 0.55 + b * 0.6 + burst * 4.5, direction.y * outward);
    p.temperature = 1.0 + burst * 2.5;
    // Seed the age across the lifetime so the pool forms a continuous column instead of
    // one ball of particles that all rose together.
    p.age = rand01(seed ^ 0x27D4EB2Fu) * gScales.w * 0.85;
}

[numthreads(64, 1, 1)]
void CSSimulate(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= (uint)gParams.z) return;

    Particle p = gParticles[index];
    float dt = gParams.x;

    // Frame 0 always respawns so the buffer does not depend on its initial contents.
    // Death is driven by age only: tying it to temperature made every particle expire at
    // the same instant, which collapsed the plume into a single rising ball.
    if (gParams.w < 0.5 || p.age > gScales.w) {
        respawn(p, index);
    }

    p.age += dt;
    // Buoyancy decays with age instead of following temperature: hot vapour rises fast,
    // then the parcel becomes roughly neutral and drifts. Tying lift to temperature alone
    // made every particle shoot up for one second and fall straight back down.
    float buoyancy = gMisc.y * exp(-p.age * 0.5) - gMisc.z;
    p.velocity += float3(gWind.x, buoyancy, gWind.z) * dt;
    // Curl-noise turbulence: small divergence-free eddies that tear the flame front and
    // keep the smoke from looking laminated. Strength arrives in gMisc.w.
    if (gMisc.w > 0.001) {
        float3 noiseDomain = p.position * 0.9 + float3(0.0, gParams.y * 0.8, 0.0);
        p.velocity += curlNoise(noiseDomain) * (gMisc.w * dt);
    }
    p.position += p.velocity * dt;
    p.temperature = max(0.0, p.temperature - dt * 1.1);

    // Push the particle out of any obstacle it ended up inside, along the shallowest axis,
    // and damp the bounced component. The scene has no rotated entities, so the AABB test
    // is exact rather than conservative.
    uint colliderCount = (uint)gColliderInfo.x;
    for (uint colliderIndex = 0; colliderIndex < colliderCount; ++colliderIndex) {
        float3 center = gColliderBuffer[colliderIndex].center.xyz;
        float3 halfExtent = gColliderBuffer[colliderIndex].extent.xyz * 0.5;
        float3 relative = p.position - center;
        float3 inside = abs(relative) - halfExtent;
        if (all(inside < 0.0)) {
            float3 penetration = -inside;
            float3 direction = float3(relative.x >= 0.0 ? 1.0 : -1.0,
                                      relative.y >= 0.0 ? 1.0 : -1.0,
                                      relative.z >= 0.0 ? 1.0 : -1.0);
            if (penetration.x <= penetration.y && penetration.x <= penetration.z) {
                p.position.x = center.x + direction.x * (halfExtent.x + 0.01);
                p.velocity.x *= -0.2;
            } else if (penetration.y <= penetration.z) {
                p.position.y = center.y + direction.y * (halfExtent.y + 0.01);
                p.velocity.y *= -0.2;
            } else {
                p.position.z = center.z + direction.z * (halfExtent.z + 0.01);
                p.velocity.z *= -0.2;
            }
        }
    }

    float3 local = (p.position - gVolume.xyz) / gVolume.w;
    if (any(local < 0.0) || any(local > 1.0) || p.position.y < gVolume.y) {
        respawn(p, index);
        local = (p.position - gVolume.xyz) / gVolume.w;
    }
    gParticles[index] = p;

    // Trilinear tent splat: the particle's contribution is spread over the 2x2x2
    // neighbourhood with trilinear (tent kernel) weights. Every weight is a deterministic
    // function of the particle position, so the integer atomic accumulation stays order
    // independent -- the determinism contract is unchanged.
    uint dim = (uint)gScales.z;
    float3 cellCoord = local * (float)dim - 0.5;
    int3 baseCell = clamp((int3)floor(cellCoord), (int3)0, (int3)(dim - 2));
    float3 f = saturate(cellCoord - (float3)baseCell);
    float burstGain = 1.0 + gMisc.x * 2.5;
    float densityTotal = gScales.x * burstGain;
    float heatTotal = p.temperature * gScales.y * burstGain;
    [unroll] for (uint oz = 0; oz <= 1; ++oz) {
        [unroll] for (uint oy = 0; oy <= 1; ++oy) {
            [unroll] for (uint ox = 0; ox <= 1; ++ox) {
                float3 side = float3((float)ox, (float)oy, (float)oz);
                float weight = lerp(1.0 - f.x, f.x, side.x) *
                               lerp(1.0 - f.y, f.y, side.y) *
                               lerp(1.0 - f.z, f.z, side.z);
                uint dq = (uint)(densityTotal * weight + 0.5);
                uint hq = (uint)(heatTotal * weight + 0.5);
                if (dq == 0 && hq == 0) continue;
                int3 cell = baseCell + (int3)side;
                if (dq > 0) InterlockedAdd(gVolumeDensity[cell], dq);
                if (hq > 0) InterlockedAdd(gVolumeHeat[cell], hq);
            }
        }
    }
}
#endif

#ifdef PASS_RESOLVE
cbuffer ResolveConstants : register(b0) {
    float4 gResolve;  // x dim, y densityInv, z heatInv, w exposure
};

Texture3D<uint> gDensityIn : register(t0);
Texture3D<uint> gHeatIn : register(t1);
RWTexture3D<float4> gVolumeOut : register(u0);

[numthreads(8, 8, 8)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint dim = (uint)gResolve.x;
    if (id.x >= dim || id.y >= dim || id.z >= dim) return;
    int3 cell = (int3)id;
    float density = saturate((float)gDensityIn.Load(int4(cell, 0)) * gResolve.y);
    float heat = saturate((float)gHeatIn.Load(int4(cell, 0)) * gResolve.z);
    // Inject, do not overwrite: the solver keeps smoke alive across frames and the splat is
    // only the per-frame source term.
    gVolumeOut[cell] = gVolumeOut[cell] + float4(density, heat, 0.0, 0.0) * gResolve.w;
}
#endif

#if defined(PASS_VOLUME) || defined(PASS_COMPOSITE)
// Shared helpers for the ray marcher and the composite.

// Planck's law evaluated at three representative wavelengths and normalised against a
// 6500 K reference, so high temperatures converge to white. This replaces the hand-tuned
// three-stop colour ramp: fire colour is now a function of temperature alone.
float planckSample(float lambdaNm, float kelvin) {
    float x = 1.4387769e7 / (lambdaNm * kelvin);  // second radiation constant in nm*K
    return 1.0 / (exp(min(x, 50.0)) - 1.0);
}

float3 blackbody(float kelvin) {
    kelvin = clamp(kelvin, 500.0, 12000.0);
    float3 c = float3(planckSample(612.0, kelvin), planckSample(545.0, kelvin),
                      planckSample(465.0, kelvin));
    const float3 reference =
        float3(planckSample(612.0, 6500.0), planckSample(545.0, 6500.0), planckSample(465.0, 6500.0));
    return c / reference;
}

// Henyey-Greenstein phase. The 1/(4 pi) normalisation is folded into the intensity scale
// applied at the call site; g = 0 reproduces isotropic scattering.
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// Fire colour. Planck's law gives the *chromaticity* of a hot body correctly, but its
// absolute magnitude collapses by six orders of magnitude between 3000 K and 1500 K, so
// using the raw ratio (or a fractional power of it) makes the cool half of a flame read as
// black. Split the two concerns instead: the blackbody ratio is normalised to unit
// luminance and carries only the hue, while the radiance comes from the temperature curve
// at the call site. A 1400 K tip is then a dim deep orange rather than nothing, and a
// 3200 K core is a bright warm white -- which is what a camera records.
float3 emissionColor(float kelvin) {
    float3 c = blackbody(kelvin);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    return c / max(luma, 1e-9);
}

uint volumeHash(uint2 pixel, uint frame) {
    uint h = pixel.x * 0x8da6b343u ^ pixel.y * 0xd8163841u ^ frame * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
#endif

#ifdef PASS_VOLUME
cbuffer VolumeConstants : register(b0) {
    float4x4 gInvViewProjection;
    float4 gCamera;       // xyz eye, w frame index (drives the march jitter)
    float4 gVolumeBox;    // xyz min, w extent
    float4 gVolumeParams; // x density gain, y extinction, z ambient, w step count
    float4 gLight;        // xyz direction to key light, w intensity
    float4 gFire;         // xyz emitter world position, w emission gain
};

Texture3D<float4> gVolumeTex : register(t0);
Texture2D<float> gSceneDepth : register(t1);
SamplerState gLinearClamp : register(s0);

void VSFullscreen(uint id : SV_VertexID,
                  out float4 position : SV_Position,
                  out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSVolume(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint2 dims;
    gSceneDepth.GetDimensions(dims.x, dims.y);

    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearWorld = mul(gInvViewProjection, float4(ndc, 0.0, 1.0));
    float4 farWorld = mul(gInvViewProjection, float4(ndc, 1.0, 1.0));
    nearWorld /= nearWorld.w;
    farWorld /= farWorld.w;

    float3 rayOrigin = gCamera.xyz;
    float3 rayDirection = normalize(farWorld.xyz - nearWorld.xyz);

    float3 boxMin = gVolumeBox.xyz;
    float3 boxMax = boxMin + gVolumeBox.w;
    float3 inverseDirection = 1.0 / rayDirection;
    float3 t0 = (boxMin - rayOrigin) * inverseDirection;
    float3 t1 = (boxMax - rayOrigin) * inverseDirection;
    float3 tSmaller = min(t0, t1);
    float3 tLarger = max(t0, t1);
    float enter = max(max(tSmaller.x, tSmaller.y), max(tSmaller.z, 0.0));
    float exitT = min(min(tLarger.x, tLarger.y), tLarger.z);
    if (exitT <= enter) discard;

    // Clip the march at the opaque surface so geometry in front occludes the plume. The
    // depth texture is always full resolution; uv maps onto it directly whether this march
    // runs at full or half resolution, and the composite pass repairs edge error with
    // bilateral weights.
    int2 pixel = int2(uv * float2(dims));
    pixel = clamp(pixel, int2(0, 0), int2(dims) - 1);
    float sceneDepth = gSceneDepth.Load(int3(pixel, 0)).r;
    if (sceneDepth < 1.0) {
        float4 sceneWorld = mul(gInvViewProjection, float4(ndc, sceneDepth, 1.0));
        sceneWorld /= sceneWorld.w;
        exitT = min(exitT, dot(sceneWorld.xyz - rayOrigin, rayDirection));
    }
    if (exitT <= enter) discard;

    uint steps = (uint)max(gVolumeParams.w, 1.0);
    float stepLength = (exitT - enter) / (float)steps;

    // Per-pixel, per-frame jitter on the march origin: the fixed-step banding pattern is
    // broken into noise that the temporal accumulation (or the eye) averages away.
    float jitter = (float)(volumeHash((uint2)position.xy, (uint)gCamera.w) & 0x00FFFFFFu) /
                   16777215.0;

    float3 smokeColor = float3(0.50, 0.53, 0.58);
    float3 lightDirection = normalize(gLight.xyz);
    float cosKey = dot(rayDirection, lightDirection);
    // Key light is moderately forward-scattering; the fill octave is nearly isotropic.
    float phaseKey = phaseHG(cosKey, 0.45) * 0.42;
    float phaseFill = phaseHG(cosKey, 0.12) * 0.85;
    float transmittance = 1.0;
    float3 scattered = 0.0;

    [loop] for (uint step = 0; step < steps; ++step) {
        float t = enter + (step + 0.5 + jitter) * stepLength;
        if (t > exitT) break;
        float3 world = rayOrigin + rayDirection * t;
        float3 uvw = (world - boxMin) / gVolumeBox.w;
        float4 texel = gVolumeTex.SampleLevel(gLinearClamp, uvw, 0);
        // Fade the last slabs so the field box does not end in a hard wall.
        float3 edge = smoothstep(0.0, 0.10, uvw) * smoothstep(0.0, 0.10, 1.0 - uvw);
        float edgeFade = min(min(edge.x, edge.y), edge.z);
        float density = texel.r * gVolumeParams.x * edgeFade;
        float heat = texel.g;

        if (density > 0.001) {
            // Short march toward the key light: cheap self-shadowing. The second
            // accumulator halves the extinction, approximating one extra scattering octave:
            // thick smoke picks up a soft internal fill instead of going flat grey.
            float lightTransmittance = 1.0;
            float fillTransmittance = 1.0;
            [unroll] for (int s = 1; s <= 4; ++s) {
                float3 sampleWorld = world + lightDirection * (gVolumeBox.w * 0.03 * s);
                float3 sampleUvw = (sampleWorld - boxMin) / gVolumeBox.w;
                float sampleDensity = gVolumeTex.SampleLevel(gLinearClamp, sampleUvw, 0).r *
                                      gVolumeParams.x;
                lightTransmittance *= exp(-sampleDensity * gVolumeParams.y * gVolumeBox.w * 0.03);
                fillTransmittance *= exp(-sampleDensity * gVolumeParams.y * gVolumeBox.w * 0.015);
            }

            float sigma = density * gVolumeParams.y;
            float stepTransmittance = exp(-sigma * stepLength);
            // Ambient scales up slightly with density: optically thick regions trap
            // multiply scattered light rather than reading as thin grey veils.
            float3 ambient = smokeColor * gVolumeParams.z * (1.0 + density * 0.6);
            float3 direct = gLight.w * lightTransmittance * phaseKey * smokeColor;
            float3 fill = gLight.w * fillTransmittance * phaseFill * smokeColor * 0.35;
            // Blackbody emission: the chroma comes from Planck at the local temperature, the
            // radiance from the heat itself, so a flame has a white-hot core, an orange body
            // and deep red tips instead of one flat clipped colour. Deliberately independent
            // of the key light -- a fire does not dim at night.
            float heatUnit = saturate(heat);
            float kelvin = 900.0 + 2400.0 * heatUnit;
            float emissionGain = 0.12 + 2.4 * heatUnit * heatUnit;
            float3 emission = emissionColor(kelvin) * gFire.w * emissionGain * edgeFade;
            // The fire lights its own smoke from inside: a point light at the emitter,
            // attenuated by distance and by a Beer proxy toward it, forward-scattered.
            float3 toFire = gFire.xyz - world;
            float fireDistance = length(toFire);
            float3 fireDirection = toFire / max(fireDistance, 1e-3);
            float fireAttenuation = gFire.w * 0.55 / (1.0 + fireDistance * fireDistance * 5.0);
            float fireTransmittance = exp(-density * gVolumeParams.y * fireDistance * 0.6);
            float firePhase = phaseHG(dot(rayDirection, fireDirection), 0.25) * 0.5;
            // emissionColor() is unit-luminance chroma now, so the glow needs an explicit
            // small magnitude: it is a scattering term, not a light source.
            float3 fireGlow = emissionColor(1900.0) * fireAttenuation * fireTransmittance *
                              firePhase * 1.6 * edgeFade * 0.006;

            float3 radiance = ambient + direct + fill + emission + fireGlow;

            scattered += transmittance * (1.0 - stepTransmittance) * radiance;
            transmittance *= stepTransmittance;
            if (transmittance < 0.01) break;
        }
    }

    float alpha = 1.0 - transmittance;
    if (alpha <= 0.001) discard;
    // Premultiplied output: the direct path blends ONE / INV_SRC_ALPHA; the temporal path
    // overwrites the half-res target and the composite pass performs the blend.
    return float4(scattered, alpha);
}
#endif

#ifdef PASS_ACCUMULATE
cbuffer AccumulateConstants : register(b0) {
    float4 gAccum;  // x blend (1 = history reset), y half width, z half height, w unused
};

Texture2D<float4> gHalf : register(t0);
RWTexture2D<float4> gHistory : register(u0);

[numthreads(8, 8, 1)]
void CSAccumulate(uint3 id : SV_DispatchThreadID) {
    uint2 halfDims = uint2((uint)gAccum.y, (uint)gAccum.z);
    if (id.x >= halfDims.x || id.y >= halfDims.y) return;
    float4 current = gHalf.Load(int3((int2)id, 0));
    float4 history = gHistory[(int2)id];
    // Exponential blend toward the new frame. A blend of 1 hard-resets (camera moved,
    // first frame); lower values trade responsiveness for convergence.
    float blend = saturate(gAccum.x);
    gHistory[(int2)id] = lerp(history, current, blend);
}
#endif

#ifdef PASS_COMPOSITE
cbuffer CompositeConstants : register(b0) {
    float4 gComposite;  // x near, y far, z full width, w full height
};

Texture2D<float4> gHistory : register(t0);
Texture2D<float> gSceneDepth : register(t1);

void VSComposite(uint id : SV_VertexID,
                 out float4 position : SV_Position,
                 out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float linearDepth(float depth, float nearPlane, float farPlane) {
    return nearPlane * farPlane / max(farPlane - depth * (farPlane - nearPlane), 1e-4);
}

float4 PSComposite(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint2 halfDims;
    gHistory.GetDimensions(halfDims.x, halfDims.y);
    int2 fullPixel = (int2)position.xy;
    float centreDepth = gSceneDepth.Load(int3(fullPixel, 0)).r;
    float centreLinear = linearDepth(centreDepth, gComposite.x, gComposite.y);

    // Depth-aware bilateral upsample of the half-res history. Four taps, each weighted by
    // bilinear proximity and by depth agreement with this pixel, so smoke edges do not
    // bleed over foreground geometry.
    float2 halfCoord = uv * float2(halfDims) - 0.5;
    int2 h00 = clamp((int2)floor(halfCoord), int2(0, 0), int2((int)halfDims.x - 2, (int)halfDims.y - 2));
    float2 f = saturate(halfCoord - (float2)h00);
    float4 sum = 0.0;
    float weightSum = 0.0;
    [unroll] for (int oy = 0; oy <= 1; ++oy) {
        [unroll] for (int ox = 0; ox <= 1; ++ox) {
            int2 tap = h00 + int2(ox, oy);
            float bilinear = (ox == 0 ? 1.0 - f.x : f.x) * (oy == 0 ? 1.0 - f.y : f.y);
            // The matching full-res depth texel sits at 2x the half-res tap centre.
            int2 depthTexel = clamp(tap * 2 + 1, int2(0, 0),
                                    int2((int)gComposite.z, (int)gComposite.w) - 1);
            float tapDepth = gSceneDepth.Load(int3(depthTexel, 0)).r;
            float tapLinear = linearDepth(tapDepth, gComposite.x, gComposite.y);
            float similarity = exp(-abs(tapLinear - centreLinear) * 2.0);
            float weight = bilinear * similarity + 1e-4;
            sum += gHistory.Load(int3(tap, 0)) * weight;
            weightSum += weight;
        }
    }
    float4 colour = sum / max(weightSum, 1e-4);
    if (colour.a <= 0.001) discard;
    // Premultiplied over-blend onto the scene colour target: ONE / INV_SRC_ALPHA.
    return colour;
}
#endif
