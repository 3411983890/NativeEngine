// Bounded visual wave residual over the CPU water footprint.
//
// This is a linear wave equation integrated on a fixed grid, NOT a shallow-water
// solver: it transports height and a vertical velocity, not volume and momentum. It
// exists to add sub-centimetre surface detail that the 129x129 CPU surface cannot
// carry, and it never feeds gameplay back. Depth gates dry cells and sets the local wave
// speed; the horizontal velocity snapshot advects the field so detail travels with the
// river instead of staying pinned to world axes.
cbuffer RippleConstants : register(b0) {
    float4 originExtent;   // xy = world origin xz, zw = footprint extent in metres
    float4 timing;         // x = dt seconds, y = tick, z = seconds, w = cell size metres
    float4 params;         // x = gravity, y = damping per second, z = height limit, w = edge fade metres
    uint4 grid;            // xy = residual resolution, z = new event count, w = reset flag
    uint4 surfaceGrid;     // xy = CPU surface columns/rows, zw = reserved
};
struct RippleEvent {
    float2 position; float radius; float displacement;
    uint tick;
    float2 direction; float stretch;
    uint sequence; uint timelineEpoch; float normalSpeed;
};
// x = depth metres, yz = horizontal velocity m/s, w = foam coverage.
StructuredBuffer<float4> surfaceField : register(t2);
StructuredBuffer<RippleEvent> events : register(t3);
Texture2D<float> previousHeight : register(t0);
Texture2D<float> previousVelocity : register(t1);
RWTexture2D<float> nextHeight : register(u0);
RWTexture2D<float> nextVelocity : register(u1);

#define RIPPLE_DRY_DEPTH 0.02
#define RIPPLE_MIN_DEPTH 0.02

// Bilinear sample in pixel coordinates (texel centre at integer + 0.5). Used by the
// semi-Lagrangian backtrace; clamps at the footprint edge.
float SampleBilinear(Texture2D<float> tex, float2 pixel) {
    const float2 p = pixel - 0.5;
    const float2 base = floor(p);
    const float2 f = p - base;
    const int2 hi = int2(grid.xy) - 1;
    const int2 c0 = clamp(int2(base), int2(0, 0), hi);
    const int2 c1 = clamp(int2(base) + 1, int2(0, 0), hi);
    const float a = tex.Load(int3(c0.x, c0.y, 0));
    const float b = tex.Load(int3(c1.x, c0.y, 0));
    const float c = tex.Load(int3(c0.x, c1.y, 0));
    const float d = tex.Load(int3(c1.x, c1.y, 0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Semi-Lagrangian neighbour lookup. Out-of-range mirrors the centre sample (reflecting
// shoreline) instead of inventing an inflow.
float Advected(Texture2D<float> tex, float2 back, int2 offset, float centre) {
    float2 p = back + float2(offset);
    if (any(p < 0) || any(p >= float2(grid.xy))) return centre;
    return SampleBilinear(tex, p);
}

float ContactKernel(float r2) {
    if (r2 > 6.25) return 0;
    const float u = .5 * r2;
    // Zero integrated volume within the elliptical support; negative displacement
    // depresses the centre, matching the authoritative contact.
    return (1 - u) * exp(-u) - exp(-3.125);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= grid.x || id.y >= grid.y) return;
    const int2 cell = int2(id.xy);
    const float2 spacing = originExtent.zw / float2(grid.xy);
    const float dx = min(spacing.x, spacing.y);

    // Nearest CPU cell supplies the depth gate, the local wave speed and the flow.
    const uint2 probe = min(uint2(float2(id.xy) / float2(grid.xy) * float2(surfaceGrid.xy)),
                            surfaceGrid.xy - 1u);
    const float4 field = surfaceField[probe.y * surfaceGrid.x + probe.x];
    const float depth = field.x;
    const float2 flow = field.yz;
    // A fresh field texture has undefined contents, and the highest spatial mode decays
    // so slowly that garbage would survive for seconds as grid-scale spikes. The first
    // dispatch reads still water, then applies any contacts delivered with it.
    if (!(depth > RIPPLE_DRY_DEPTH)) { nextHeight[cell] = 0; nextVelocity[cell] = 0; return; }

    float h = 0, v = 0;
    if (grid.w == 0) {
        // Semi-Lagrangian backtrace: the state that arrives here came from upstream by
        // flow * dt. With zero flow this reduces to the cell itself.
        const float2 back = float2(id.xy) + .5 - flow * timing.x / spacing;
        h = SampleBilinear(previousHeight, back);
        v = SampleBilinear(previousVelocity, back);
        // Reflecting shoreline: an out-of-range neighbour mirrors the centre sample,
        // which removes the normal gradient instead of inventing an inflow.
        const float hL = Advected(previousHeight, back, int2(-1, 0), h);
        const float hR = Advected(previousHeight, back, int2(1, 0), h);
        const float hD = Advected(previousHeight, back, int2(0, -1), h);
        const float hU = Advected(previousHeight, back, int2(0, 1), h);
        const float diagonal = Advected(previousHeight, back, int2(-1, -1), h) + Advected(previousHeight, back, int2(1, -1), h)
                             + Advected(previousHeight, back, int2(-1, 1), h) + Advected(previousHeight, back, int2(1, 1), h);
        const float2 invSquare = 1 / (spacing * spacing);
        // Nine-point stencil: on a square grid axial weights are 4/6 and diagonal
        // weights 1/6. Its leading spatial error is rotationally symmetric.
        const float mixed = (diagonal - 2 * (hL + hR + hD + hU) + 4 * h) * invSquare.x * invSquare.y;
        const float laplacian = (hL + hR - 2 * h) * invSquare.x + (hD + hU - 2 * h) * invSquare.y
                              + min(dot(spacing, spacing) / 12, .49 / max(invSquare.x, invSquare.y)) * mixed;

        // c^2 = g h for gravity waves. The explicit update only stays clean well below
        // the five-point limit of 1/sqrt(2), so the speed is clamped rather than pushed
        // to the edge where grid-scale checkerboard noise survives the damping.
        float c2 = params.x * max(depth, RIPPLE_MIN_DEPTH);
        const float cMax = 0.35 * dx / max(timing.x, 1e-5);
        c2 = min(c2, cMax * cMax);
        v += timing.x * (c2 * laplacian - params.y * v);
        h += timing.x * v;
    }

    // Absorbing band: a hard wall would trap energy and slowly flood the pond with a
    // standing wave. The taper is exponential in seconds so the decay does not depend
    // on the render frame rate.
    const float2 local = (float2(id.xy) + .5) * spacing;
    const float2 edge = min(local, originExtent.zw - local);
    const float fade = saturate(min(edge.x, edge.y) / max(params.w, 1e-4));
    const float absorb = exp(-timing.x * lerp(8.0, 0.0, fade));
    h *= absorb;
    v *= absorb;

    // One-shot injection. The CPU filters by (timelineEpoch, sequence), so every event
    // in this buffer is new for this dispatch.
    [loop] for (uint i = 0; i < grid.z; ++i) {
        const RippleEvent e = events[i];
        if (e.radius <= 0) continue;
        const float2 world = originExtent.xy + local;
        float2 delta = world - e.position;
        if (dot(e.direction, e.direction) > .5) {
            delta = float2(dot(delta, e.direction) * e.stretch,
                           delta.x * e.direction.y - delta.y * e.direction.x);
        }
        const float r2 = dot(delta, delta) / (e.radius * e.radius);
        if (r2 > 6.25) continue;
        // Two volume-neutral bands vary their width and balance per contact. The
        // sequence is stable during replay and does not flicker between frames. A
        // vertical arrival widens the kernel: a landing is not a step.
        uint seed = e.sequence * 747796405u + asuint(e.radius) * 2891336453u + e.tick;
        seed = (seed ^ (seed >> 16u)) * 2246822519u;
        const float variation = float(seed & 65535u) / 65536.0;
        const float width = lerp(.5, .8, variation) + min(e.normalSpeed * .02, .12);
        const float balance = lerp(.15, .4, variation);
        h += e.displacement * lerp(ContactKernel(r2), ContactKernel(r2 / (width * width)), balance);
    }

    const float limit = params.z;
    h = clamp(isfinite(h) ? h : 0.0, -limit, limit);
    v = isfinite(v) ? clamp(v, -limit * 40.0, limit * 40.0) : 0.0;
    nextHeight[cell] = h;
    nextVelocity[cell] = v;
}
