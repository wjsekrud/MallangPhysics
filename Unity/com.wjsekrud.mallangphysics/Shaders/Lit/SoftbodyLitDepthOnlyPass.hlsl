// Softbody/Lit DepthOnly 패스. URP 17.3 DepthOnlyPass.hlsl을 고치지 않고 감싼다: 위치만 옮긴다.
// 깊이 우선(Depth Priming)에서 ForwardLit이 ZTest Equal로 이 깊이를 보므로 ForwardLit과 같은 함수(SoftbodyDisplacedPositionOS)로 옮긴다.
// 필요: 먼저 SoftbodyLitInput.hlsl.
#ifndef SOFTBODY_LIT_DEPTH_ONLY_PASS_INCLUDED
#define SOFTBODY_LIT_DEPTH_ONLY_PASS_INCLUDED

#include "Packages/com.unity.render-pipelines.universal/Shaders/DepthOnlyPass.hlsl"

struct SoftbodyDepthOnlyAttributes
{
    Attributes base;
    uint vertexID : SV_VertexID;
};

Varyings SoftbodyDepthOnlyVertex(SoftbodyDepthOnlyAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    attributes.position.xyz = SoftbodyDisplacedPositionOS(input.vertexID, attributes.position.xyz);   // URP DepthOnly는 이름이 position
    return DepthOnlyVertex(attributes);
}

#endif // SOFTBODY_LIT_DEPTH_ONLY_PASS_INCLUDED
