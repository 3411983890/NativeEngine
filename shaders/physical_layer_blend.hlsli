#ifndef NATIVEGPU_PHYSICAL_LAYER_BLEND
#define NATIVEGPU_PHYSICAL_LAYER_BLEND
// Reference closure for nativegpu.physical-layers/1. Mirrors
// engine/material/physical-layers.mjs. Not yet consumed by the production pixel
// shader; the runtime keeps the phase-one parameter blend.
float PhysicalFresnel(float f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}
float PhysicalSpecularLobe(float cosHalf, float roughness) {
    float a = max(roughness * roughness, 0.0025), a2 = a * a;
    float nh = saturate(cosHalf), denominator = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265359 * denominator * denominator, 1e-8);
}
struct PhysicalCoatResult { float reflectedShare, transmittedShare, coatSpecular, baseSpecular; };
// coverage scales the coat; transmission scales the base independently. For a
// non-absorbing layer reflected + transmitted <= 1 by construction.
PhysicalCoatResult PhysicalCoatClosure(float f0, float coatRoughness, float baseRoughness,
                                       float cosCoatView, float cosCoatHalf, float cosBaseHalf,
                                       float coverage, float transmission) {
    PhysicalCoatResult result;
    float F = PhysicalFresnel(f0, abs(cosCoatView)) * saturate(coverage);
    result.reflectedShare = F;
    result.transmittedShare = (1.0 - F) * saturate(transmission);
    result.coatSpecular = PhysicalSpecularLobe(abs(cosCoatHalf), coatRoughness) * F;
    result.baseSpecular = PhysicalSpecularLobe(abs(cosBaseHalf), baseRoughness) * result.transmittedShare;
    return result;
}
#endif
