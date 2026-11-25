#ifndef NATIVEGPU_MATERIAL_LAYER_BLEND
#define NATIVEGPU_MATERIAL_LAYER_BLEND

// Inputs are validated by the material compiler/loader. This function mirrors
// engine/native/material_layered.hpp and engine/material/layered.mjs.
float LayerHeightCoverage(float coverage,float bottomHeight,float topHeight,float width,float bias) {
    if(coverage <= 0.0) return 0.0;
    if(coverage >= 1.0) return 1.0;
    return saturate(coverage+(topHeight-bottomHeight+bias)/max(width,0.0001)*coverage*(1.0-coverage));
}
float3 LayerBlendNormal(float3 bottom,float3 top,float coverage) {
    float3 mixed=lerp(bottom,top,coverage);
    if(dot(mixed,mixed)<1e-16) mixed=coverage<.5?bottom:top;
    return normalize(mixed);
}
#endif
