#include "../../shaders/physical_layer_blend.hlsli"
RWStructuredBuffer<float4> output : register(u0);
[numthreads(1,1,1)]
void CSMain(uint3 id:SV_DispatchThreadID) {
    PhysicalCoatResult result=PhysicalCoatClosure(.04,.05,.9,.8,.9,.95,.5,.8);
    output[0]=float4(result.reflectedShare,result.transmittedShare,result.coatSpecular,result.baseSpecular);
}
