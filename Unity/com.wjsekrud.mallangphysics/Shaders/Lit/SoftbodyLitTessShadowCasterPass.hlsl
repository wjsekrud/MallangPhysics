// Softbody/Lit (Tessellated) ShadowCaster 패스. domain이 URP ShadowPassVertex를 부른다(그림자 바이어스·점 빛 방향이 쪼갠 점 위치에서 걸린다).
// 쪼갬 수는 빛이 아니라 카메라 기준이다(SoftbodyTessEdgeFactor — _SoftbodyTessCamera) — 그림자가 화면에 그려진 모양과 같은 모양에서 진다.
// _CASTING_PUNCTUAL_LIGHT_SHADOW는 URP가 정점 단계 키워드로 두지만 여기서는 domain이 바이어스를 거므로 셰이더가 domain 단계 키워드로 둔다.
// 필요: 먼저 SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_SHADOW_CASTER_PASS_INCLUDED
#define SOFTBODY_LIT_TESS_SHADOW_CASTER_PASS_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitShadowCasterPass.hlsl"

SoftbodyTessControlPoint SoftbodyTessShadowVertex(SoftbodyShadowAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    return SoftbodyTessMakeControlPoint(input.vertexID, attributes.positionOS.xyz, attributes.normalOS, float4(1.0, 0.0, 0.0, 1.0),
        float4(attributes.texcoord, 0.0, 0.0), float2(0.0, 0.0));
}

[domain("tri")]
Varyings SoftbodyTessShadowDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    SoftbodyTessPoint p = SOFTBODY_TESS_DOMAIN_POINT(patch, bary);
    Attributes attributes = (Attributes)0;
    attributes.positionOS = float4(p.positionOS, 1.0);
    attributes.normalOS = p.normalOS;
    attributes.texcoord = p.texcoord.xy;
    return ShadowPassVertex(attributes);
}

#endif // SOFTBODY_LIT_TESS_SHADOW_CASTER_PASS_INCLUDED
