// Softbody/Lit GBuffer 패스(Deferred·Deferred+). URP 17.3 LitGBufferPass.hlsl을 고치지 않고 감싼다 — ForwardLit 감싸개와 같은 일
// (정점 위치 이동 + 보간기, 픽셀 노멀·탄젠트 회전 + 기본 색 고리).
// 필요: 먼저 SoftbodyLitInput.hlsl.
#ifndef SOFTBODY_LIT_GBUFFER_PASS_INCLUDED
#define SOFTBODY_LIT_GBUFFER_PASS_INCLUDED

#include "Packages/com.unity.render-pipelines.universal/Shaders/LitGBufferPass.hlsl"

struct SoftbodyLitGBufferAttributes
{
    Attributes base;
    uint vertexID : SV_VertexID;
};

struct SoftbodyLitGBufferVaryings
{
    Varyings base;
    SOFTBODY_VARYINGS(11, 12, 13, 14)   // URP GBuffer는 TEXCOORD0~9를 쓴다
};

SoftbodyLitGBufferVaryings SoftbodyLitGBufferPassVertex(SoftbodyLitGBufferAttributes input)
{
    SoftbodyLitGBufferVaryings output = (SoftbodyLitGBufferVaryings)0;
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);

    SoftbodyVertexShadingData softbody = SoftbodyVertexShadingOS(input.vertexID, attributes.positionOS.xyz, attributes.normalOS);
    attributes.positionOS.xyz = SoftbodyDisplacedPositionOS(input.vertexID, attributes.positionOS.xyz);

    output.base = LitGBufferPassVertex(attributes);
    SOFTBODY_TRANSFER_VARYINGS(softbody, output)
    return output;
}

GBufferFragOutput SoftbodyLitGBufferPassFragment(SoftbodyLitGBufferVaryings input)
{
    SoftbodyVertexShadingData softbody = SOFTBODY_LOAD_VARYINGS(input);
    float4 surfacePixel = SoftbodySurfacePixel(softbody, input.base.normalWS);
    input.base.normalWS = SoftbodyRotateByShading(input.base.normalWS, surfacePixel, softbody.inertiaTilt);
#if defined(REQUIRES_WORLD_SPACE_TANGENT_INTERPOLATOR)
    input.base.tangentWS.xyz = SoftbodyRotateByShading(input.base.tangentWS.xyz, surfacePixel, softbody.inertiaTilt);
#endif
    SoftbodyLitSetBaseColorInputs(1.0 - surfacePixel.w, softbody.visColor);
    return LitGBufferPassFragment(input.base);
}

#endif // SOFTBODY_LIT_GBUFFER_PASS_INCLUDED
