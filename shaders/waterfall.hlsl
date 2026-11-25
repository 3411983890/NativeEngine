// Falling water sheet between two water bodies.
//
// The sheet is presentation only: volume and momentum live in the transfer ledger on the
// CPU. Vertex positions come from the configured source/landing regions, and the shader
// adds the streaky, broken edge that a flat height field cannot express.
cbuffer WaterfallConstants : register(b0) {
    column_major float4x4 viewProjection;
    float4 sourceTime;    // xyz source world, w = seconds
    float4 landingWidth;  // xyz landing world, w = width metres
    float4 params;        // x = actual flow m3/s, y = opacity, z = height, w = tick
};
struct WaterfallVertex { float3 position; float2 uv; };
StructuredBuffer<WaterfallVertex> sheetVertices : register(t0);
struct VSOut {
    float4 clip : SV_Position;
    float2 uv : TEXCOORD0;
    float3 world : TEXCOORD1;
};

VSOut VSMain(uint id : SV_VertexID) {
    const WaterfallVertex input = sheetVertices[id];
    VSOut output;
    output.clip = mul(viewProjection, float4(input.position, 1));
    output.uv = input.uv;
    output.world = input.position;
    return output;
}

float Hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float ValueNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    float a = Hash21(i), b = Hash21(i + float2(1, 0));
    float c = Hash21(i + float2(0, 1)), d = Hash21(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

struct Output { float4 surface : SV_Target0; float4 key : SV_Target1; };

Output PSMain(VSOut input) {
    const float t = sourceTime.w;
    // Downward streaks: two scrolling octaves with different speeds. The fine octave
    // breaks the silhouette so the sheet does not read as a solid blue quad.
    float n = ValueNoise(float2(input.uv.x * 9.0, input.uv.y * 7.0 - t * 2.1));
    n = n * 0.65 + ValueNoise(float2(input.uv.x * 23.0 + 7.0, input.uv.y * 18.0 - t * 3.7)) * 0.35;
    float edge = smoothstep(0.02, 0.16, input.uv.x) * smoothstep(0.98, 0.84, input.uv.x);
    float top = smoothstep(0.0, 0.06, input.uv.y);
    float bottom = smoothstep(1.0, 0.92, input.uv.y);
    // Thin at the source, spreading towards the landing.
    float thickness = lerp(0.55, 1.0, saturate(input.uv.y * 1.2));
    float lace = saturate(0.35 + n * 0.9);
    float alpha = edge * top * bottom * thickness * (0.45 + lace * 0.55) * params.y;
    alpha *= smoothstep(0.0, 0.35, params.x * 4.0);
    float3 color = lerp(float3(0.46, 0.62, 0.72), float3(0.78, 0.88, 0.94), n);
    Output output;
    output.surface = float4(color * alpha, alpha);
    output.key = float4(0, 0, 0, 1);
    return output;
}
