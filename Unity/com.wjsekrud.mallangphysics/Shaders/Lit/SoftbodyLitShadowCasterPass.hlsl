// Softbody/Lit ShadowCaster 패스. URP 17.3 ShadowCasterPass.hlsl을 고치지 않고 감싼다: 위치만 옮긴다.
// 그림자 바이어스는 옮긴 위치·원래 노멀로 걸린다(점·스포트 빛 방향도 옮긴 위치에서) — 노멀은 정점에서 바꾸지 않는 규칙(03 §10.3).
// 필요: 먼저 SoftbodyLitInput.hlsl.
#ifndef SOFTBODY_LIT_SHADOW_CASTER_PASS_INCLUDED
#define SOFTBODY_LIT_SHADOW_CASTER_PASS_INCLUDED

#include "Packages/com.unity.render-pipelines.universal/Shaders/ShadowCasterPass.hlsl"

struct SoftbodyShadowAttributes
{
    Attributes base;
    uint vertexID : SV_VertexID;
};

Varyings SoftbodyShadowPassVertex(SoftbodyShadowAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    attributes.positionOS.xyz = SoftbodyDisplacedPositionOS(input.vertexID, attributes.positionOS.xyz);
    return ShadowPassVertex(attributes);
}

#endif // SOFTBODY_LIT_SHADOW_CASTER_PASS_INCLUDED
