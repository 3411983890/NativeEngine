// Field solver: buoyancy, semi-Lagrangian advection, divergence and pressure projection.
//
// This turns the particle splat from "density painted where particles are" into an Eulerian
// smoke state: a velocity field carries the smoke and the projection step makes that
// velocity divergence free. It is the F3 remainder described in docs/FIELD-PLAN.md.
//
// All field textures stay in UNORDERED_ACCESS and are read with Load plus manual trilinear
// interpolation, so no pass needs an SRV transition and passes are separated by UAV barriers
// only. One root signature serves every pass: four heap indices plus four scalars.

cbuffer PassConstants : register(b0) {
    uint4 gIndices;   // x srv0 (read A), y srv1 (read B), z uav0 (write), w dim
    float4 gParams;   // x dt, y dx, z buoyancy, w dissipation
    float4 gBurst;    // x strength, yzw blast centre in normalised volume space
};

float4 trilinear(RWTexture3D<float4> tex, float3 uvw, uint dim) {
    float3 p = saturate(uvw) * (float)(dim - 1);
    int3 i0 = (int3)floor(p);
    int3 i1 = min(i0 + 1, (int3)(dim - 1));
    float3 f = p - (float3)i0;
    float4 c000 = tex.Load(i0);
    float4 c100 = tex.Load(int3(i1.x, i0.y, i0.z));
    float4 c010 = tex.Load(int3(i0.x, i1.y, i0.z));
    float4 c110 = tex.Load(int3(i1.x, i1.y, i0.z));
    float4 c001 = tex.Load(int3(i0.x, i0.y, i1.z));
    float4 c101 = tex.Load(int3(i1.x, i0.y, i1.z));
    float4 c011 = tex.Load(int3(i0.x, i1.y, i1.z));
    float4 c111 = tex.Load(int3(i1.x, i1.y, i1.z));
    float4 c00 = lerp(c000, c100, f.x);
    float4 c10 = lerp(c010, c110, f.x);
    float4 c01 = lerp(c001, c101, f.x);
    float4 c11 = lerp(c011, c111, f.x);
    return lerp(lerp(c00, c10, f.y), lerp(c01, c11, f.y), f.z);
}

// Pushes hot smoke upward and lets momentum decay.
[numthreads(8, 8, 8)]
void CSBuoyancy(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> state = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> velocity = ResourceDescriptorHeap[gIndices.z];
    float heat = state.Load((int3)id).g;
    float3 v = velocity.Load((int3)id).xyz;
    v.y += heat * gParams.z * gParams.x;
    // Explosion blast: push the whole field away from the fire centre. This moves the smoke
    // that already exists, which a per-particle radius change cannot do.
    if (gBurst.x > 0.001) {
        float3 uvw = (id + 0.5) / (float)dim;
        float3 delta = uvw - gBurst.yzw;
        float distance = length(delta) + 1e-3;
        float falloff = exp(-distance * 5.0);
        v += (delta / distance) * (gBurst.x * falloff);
        v.y += gBurst.x * 0.35 * falloff;
    }
    v *= saturate(1.0 - gParams.w * gParams.x);
    velocity[id] = float4(v, 0.0);
}

// Semi-Lagrangian backtrace of the velocity field.
[numthreads(8, 8, 8)]
void CSAdvectVelocity(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> source = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    float3 uvw = (id + 0.5) / (float)dim;
    float3 velocity = source.Load((int3)id).xyz;
    float3 back = uvw - velocity * gParams.x / (gParams.y * dim);
    float3 sampled = trilinear(source, back, dim).xyz;
    target[id] = float4(sampled * saturate(1.0 - gParams.w * gParams.x), 0.0);
}

// Semi-Lagrangian backtrace of density and heat. The same pass runs forward (positive dt,
// applies dissipation and radiative heat decay) and backward for MacCormack (negative dt,
// no decay: the correction step restores the energy the backward sample would lose).
[numthreads(8, 8, 8)]
void CSAdvectState(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> state = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> velocity = ResourceDescriptorHeap[gIndices.y];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    float3 uvw = (id + 0.5) / (float)dim;
    float3 v = velocity.Load((int3)id).xyz;
    float3 back = uvw - v * gParams.x / (gParams.y * dim);
    float4 sampled = trilinear(state, back, dim);
    float forward = gParams.x >= 0.0 ? 1.0 : 0.0;
    float densityDecay = lerp(1.0, saturate(1.0 - gParams.w * abs(gParams.x)), forward);
    float heatDecay = lerp(1.0, 0.988, forward);
    target[id] = float4(sampled.r * densityDecay, sampled.g * heatDecay, 0.0, 1.0);
}

// MacCormack correction: phi1 (first-order advected) + 0.5 * (phi0 - phi2), where phi2 is
// phi1 advected backward. The result is clamped to the 6-neighbourhood min/max of phi0 so
// the second-order term sharpens detail without ringing. Reads phi1 from and writes the
// corrected state to the same texture; each thread only touches its own cell of the
// target, so there is no cross-thread race.
[numthreads(8, 8, 8)]
void CSMacCormackState(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> original = ResourceDescriptorHeap[gIndices.x];   // phi0
    RWTexture3D<float4> backward = ResourceDescriptorHeap[gIndices.y];   // phi2
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];     // phi1 -> corrected
    int3 c = (int3)id;
    int3 hi = (int3)dim - 1;
    float4 phi0 = original.Load(c);
    float4 phi1 = target.Load(c);
    float4 phi2 = backward.Load(c);
    float4 corrected = phi1 + 0.5 * (phi0 - phi2);
    float4 mn = phi0;
    float4 mx = phi0;
    int3 offsets[6] = {int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0),
                       int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)};
    [unroll] for (int i = 0; i < 6; ++i) {
        int3 n = clamp(c + offsets[i], (int3)0, hi);
        float4 neighbour = original.Load(n);
        mn = min(mn, neighbour);
        mx = max(mx, neighbour);
    }
    target[id] = clamp(corrected, mn, mx);
}

// Central-difference divergence of the velocity field.
[numthreads(8, 8, 8)]
void CSDivergence(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> velocity = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> divergence = ResourceDescriptorHeap[gIndices.z];
    int3 c = (int3)id;
    int3 hi = (int3)dim - 1;
    float3 xp = velocity.Load(int3(min(c.x + 1, hi.x), c.y, c.z)).xyz;
    float3 xm = velocity.Load(int3(max(c.x - 1, 0), c.y, c.z)).xyz;
    float3 yp = velocity.Load(int3(c.x, min(c.y + 1, hi.y), c.z)).xyz;
    float3 ym = velocity.Load(int3(c.x, max(c.y - 1, 0), c.z)).xyz;
    float3 zp = velocity.Load(int3(c.x, c.y, min(c.z + 1, hi.z))).xyz;
    float3 zm = velocity.Load(int3(c.x, c.y, max(c.z - 1, 0))).xyz;
    float d = (xp.x - xm.x + yp.y - ym.y + zp.z - zm.z) / (2.0 * gParams.y);
    divergence[id] = float4(d, 0.0, 0.0, 0.0);
}

// One Jacobi iteration of the pressure Poisson solve.
[numthreads(8, 8, 8)]
void CSJacobi(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> pressure = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> divergence = ResourceDescriptorHeap[gIndices.y];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    int3 c = (int3)id;
    int3 hi = (int3)dim - 1;
    float sum = 0.0;
    sum += pressure.Load(int3(min(c.x + 1, hi.x), c.y, c.z)).x;
    sum += pressure.Load(int3(max(c.x - 1, 0), c.y, c.z)).x;
    sum += pressure.Load(int3(c.x, min(c.y + 1, hi.y), c.z)).x;
    sum += pressure.Load(int3(c.x, max(c.y - 1, 0), c.z)).x;
    sum += pressure.Load(int3(c.x, c.y, min(c.z + 1, hi.z))).x;
    sum += pressure.Load(int3(c.x, c.y, max(c.z - 1, 0))).x;
    float d = divergence.Load(c).x;
    float p = (sum - gParams.y * gParams.y * d) / 6.0;
    target[id] = float4(p, 0.0, 0.0, 1.0);
}

// Subtract the pressure gradient, leaving a divergence-free velocity field.
[numthreads(8, 8, 8)]
void CSGradientSubtract(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> velocity = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> pressure = ResourceDescriptorHeap[gIndices.y];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    int3 c = (int3)id;
    int3 hi = (int3)dim - 1;
    float px = pressure.Load(int3(min(c.x + 1, hi.x), c.y, c.z)).x -
               pressure.Load(int3(max(c.x - 1, 0), c.y, c.z)).x;
    float py = pressure.Load(int3(c.x, min(c.y + 1, hi.y), c.z)).x -
               pressure.Load(int3(c.x, max(c.y - 1, 0), c.z)).x;
    float pz = pressure.Load(int3(c.x, c.y, min(c.z + 1, hi.z))).x -
               pressure.Load(int3(c.x, c.y, max(c.z - 1, 0))).x;
    float3 v = velocity.Load(c).xyz;
    v -= float3(px, py, pz) / (2.0 * gParams.y);
    if (c.y == 0 || c.y == hi.y) v.y = 0.0;
    target[id] = float4(v, 0.0);
}

// Vorticity: curl of the velocity field, used by the confinement pass.
[numthreads(8, 8, 8)]
void CSCurl(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> velocity = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    int3 c = (int3)id;
    int3 hi = (int3)dim - 1;
    float3 xp = velocity.Load(int3(min(c.x + 1, hi.x), c.y, c.z)).xyz;
    float3 xm = velocity.Load(int3(max(c.x - 1, 0), c.y, c.z)).xyz;
    float3 yp = velocity.Load(int3(c.x, min(c.y + 1, hi.y), c.z)).xyz;
    float3 ym = velocity.Load(int3(c.x, max(c.y - 1, 0), c.z)).xyz;
    float3 zp = velocity.Load(int3(c.x, c.y, min(c.z + 1, hi.z))).xyz;
    float3 zm = velocity.Load(int3(c.x, c.y, max(c.z - 1, 0))).xyz;
    float inverse = 1.0 / (2.0 * gParams.y);
    float3 curl = float3(
        ((yp.z - ym.z) - (zp.y - zm.y)) * inverse,
        ((zp.x - zm.x) - (xp.z - xm.z)) * inverse,
        ((xp.y - xm.y) - (yp.x - ym.x)) * inverse);
    target[id] = float4(curl, 0.0);
}

// Vorticity confinement: push the velocity along N x curl, where N is the gradient of the
// curl magnitude. This restores the small eddies that semi-Lagrangian advection damps out.
[numthreads(8, 8, 8)]
void CSVorticity(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> curlTexture = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> velocityTexture = ResourceDescriptorHeap[gIndices.y];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    int3 c = (int3)id;
    int3 hi = (int3)dim - 1;
    float3 curl = curlTexture.Load(c).xyz;
    float3 gradient = float3(
        length(curlTexture.Load(int3(min(c.x + 1, hi.x), c.y, c.z)).xyz) -
            length(curlTexture.Load(int3(max(c.x - 1, 0), c.y, c.z)).xyz),
        length(curlTexture.Load(int3(c.x, min(c.y + 1, hi.y), c.z)).xyz) -
            length(curlTexture.Load(int3(c.x, max(c.y - 1, 0), c.z)).xyz),
        length(curlTexture.Load(int3(c.x, c.y, min(c.z + 1, hi.z))).xyz) -
            length(curlTexture.Load(int3(c.x, c.y, max(c.z - 1, 0))).xyz));
    float gradientLength = length(gradient);
    float3 v = velocityTexture.Load(c).xyz;
    if (gradientLength > 1e-5) {
        float3 normal = gradient / gradientLength;
        float3 force = cross(normal, curl) * (gParams.z * gParams.y);
        v += force * gParams.x;
    }
    target[id] = float4(v, 0.0);
}

// Copies the advected state back into the canonical buffer the renderer reads.
[numthreads(8, 8, 8)]
void CSCopyState(uint3 id : SV_DispatchThreadID) {
    uint dim = gIndices.w;
    if (any(id >= (uint3)dim)) return;
    RWTexture3D<float4> source = ResourceDescriptorHeap[gIndices.x];
    RWTexture3D<float4> target = ResourceDescriptorHeap[gIndices.z];
    target[id] = source.Load((int3)id);
}
