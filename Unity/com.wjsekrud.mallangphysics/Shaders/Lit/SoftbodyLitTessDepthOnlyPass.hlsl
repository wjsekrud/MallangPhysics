// Softbody/Lit (Tessellated) DepthOnly 패스. domain이 URP DepthOnlyVertex를 부른다(픽셀 = URP DepthOnlyFragment).
// 깊이 우선에서 ForwardLit이 ZTest Equal로 이 깊이를 보므로 쪼갬 수·위치는 ForwardLit과 같은 함수다(SoftbodyLitTessCommon.hlsl).
// 필요: 먼저 SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_DEPTH_ONLY_PASS_INCLUDED
#define SOFTBODY_LIT_TESS_DEPTH_ONLY_PASS_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitDepthOnlyPass.hlsl"

// URP DepthOnly의 Attributes에는 노멀이 없다 — 높이 방향(보간 노멀)이 다른 패스와 같아야 하므로 노멀을 따로 받는다.
struct SoftbodyTessDepthOnlyAttributes
{
    Attributes base;
    float3 normalOS : NORMAL;
    uint vertexID : SV_VertexID;
};

SoftbodyTessControlPoint SoftbodyTessDepthOnlyVertex(SoftbodyTessDepthOnlyAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    return SoftbodyTessMakeControlPoint(input.vertexID, attributes.position.xyz, input.normalOS, float4(1.0, 0.0, 0.0, 1.0),
        float4(attributes.texcoord, 0.0, 0.0), float2(0.0, 0.0));
}

[domain("tri")]
Varyings SoftbodyTessDepthOnlyDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    SoftbodyTessPoint p = SOFTBODY_TESS_DOMAIN_POINT(patch, bary);
    Attributes attributes = (Attributes)0;
    attributes.position = float4(p.positionOS, 1.0);   // URP DepthOnly는 이름이 position
    attributes.texcoord = p.texcoord.xy;
    return DepthOnlyVertex(attributes);
}

#endif // SOFTBODY_LIT_TESS_DEPTH_ONLY_PASS_INCLUDED
