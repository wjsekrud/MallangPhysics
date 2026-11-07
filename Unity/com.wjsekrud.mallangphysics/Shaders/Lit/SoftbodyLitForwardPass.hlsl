// Softbody/Lit ForwardLit 패스(Forward·Forward+). URP 17.3 LitForwardPass.hlsl을 고치지 않고 감싼다:
//   정점 — 위치를 소프트바디 이동만큼 옮긴 뒤 URP 정점 함수를 부르고, 픽셀이 쓸 소프트바디 보간기 넷을 덧붙인다.
//   픽셀 — 보간된 정점 노멀·탄젠트를 소프트바디 회전(관성 기울기 → 표면 장 회전)만큼 돌리고, 기본 색 고리(함몰 어둡기·디버그 색)를 채운 뒤
//          URP 픽셀 함수를 부른다. 노멀과 탄젠트를 같이 돌리므로 노멀맵을 얹은 노멀이 UE(노멀맵 노멀을 돌림)와 같이 돈다.
// 필요: 먼저 SoftbodyLitInput.hlsl.
#ifndef SOFTBODY_LIT_FORWARD_PASS_INCLUDED
#define SOFTBODY_LIT_FORWARD_PASS_INCLUDED

#include "Packages/com.unity.render-pipelines.universal/Shaders/LitForwardPass.hlsl"

struct SoftbodyLitAttributes
{
    Attributes base;
    uint vertexID : SV_VertexID;   // 정점 표 칸(D-17)
};

struct SoftbodyLitVaryings
{
    Varyings base;
    SOFTBODY_VARYINGS(11, 12, 13, 14)   // URP Forward는 TEXCOORD0~10을 쓴다
};

SoftbodyLitVaryings SoftbodyLitPassVertex(SoftbodyLitAttributes input)
{
    SoftbodyLitVaryings output = (SoftbodyLitVaryings)0;
    Attributes attributes = input.base;
    // 인스턴싱 변종에서 아래 공간 변환이 이 인스턴스의 행렬을 쓰게 먼저 세팅한다(URP 정점 함수가 한 번 더 해도 같다).
    UNITY_SETUP_INSTANCE_ID(attributes);

    SoftbodyVertexShadingData softbody = SoftbodyVertexShadingOS(input.vertexID, attributes.positionOS.xyz, attributes.normalOS);
    attributes.positionOS.xyz = SoftbodyDisplacedPositionOS(input.vertexID, attributes.positionOS.xyz);

    output.base = LitPassVertex(attributes);
    SOFTBODY_TRANSFER_VARYINGS(softbody, output)
    return output;
}

void SoftbodyLitPassFragment(
    SoftbodyLitVaryings input
    , out half4 outColor : SV_Target0
#ifdef _WRITE_RENDERING_LAYERS
    , out uint outRenderingLayers : SV_Target1
#endif
)
{
    SoftbodyVertexShadingData softbody = SOFTBODY_LOAD_VARYINGS(input);
    float4 surfacePixel = SoftbodySurfacePixel(softbody, input.base.normalWS);
    input.base.normalWS = SoftbodyRotateByShading(input.base.normalWS, surfacePixel, softbody.inertiaTilt);
#if defined(REQUIRES_WORLD_SPACE_TANGENT_INTERPOLATOR)
    input.base.tangentWS.xyz = SoftbodyRotateByShading(input.base.tangentWS.xyz, surfacePixel, softbody.inertiaTilt);
#endif
    SoftbodyLitSetBaseColorInputs(1.0 - surfacePixel.w, softbody.visColor);

    LitPassFragment(input.base, outColor
#ifdef _WRITE_RENDERING_LAYERS
        , outRenderingLayers
#endif
    );
}

#endif // SOFTBODY_LIT_FORWARD_PASS_INCLUDED
