// URP 셰이더에 소프트바디를 끼우는 도우미. SoftbodyMaterial.hlsl(엔진 쪽 함수)을 URP 패스 꼴(VertexPositionInputs, Varyings 보간기)에 맞춘다.
// Softbody/Lit의 패스 파일(Shaders/Lit)이 이것을 쓰고, 손으로 짠 URP 셰이더도 같은 꼴로 쓸 수 있다(Include/README.md).
// 필요: URP Core.hlsl, 프로퍼티 선언(SoftbodyMaterialInput.hlsl과 UnityPerMaterial 안의 SOFTBODY_PER_MATERIAL_PROPERTIES).
#ifndef SOFTBODY_URP_INCLUDED
#define SOFTBODY_URP_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Include/SoftbodyMaterial.hlsl"

// URP GetVertexPositionInputs와 같되 위치를 소프트바디 이동만큼 옮긴 값. 안에서 SoftbodyDisplacedPositionOS를 부르므로 다른 패스가
// SoftbodyDisplacedPositionOS를 바로 써도 같은 깊이 비트가 나온다(…WS 판과는 섞지 않는다 — SoftbodyMaterial.hlsl 규칙 3).
VertexPositionInputs SoftbodyGetVertexPositionInputs(float3 positionOS, uint vertexID)
{
    return GetVertexPositionInputs(SoftbodyDisplacedPositionOS(vertexID, positionOS));
}

// Varyings에 넣는 보간기 넷(색을 쓰는 패스) / 셋(노멀만 쓰는 패스). URP Lit 패스는 TEXCOORD0~10을 쓰므로 Softbody/Lit은 11부터 둔다.
#define SOFTBODY_VARYINGS(i0, i1, i2, i3) \
    float4 softbodyCarrier : TEXCOORD##i0; \
    float4 softbodyShading : TEXCOORD##i1; \
    float3 softbodyInertiaTilt : TEXCOORD##i2; \
    float3 softbodyVisColor : TEXCOORD##i3;

#define SOFTBODY_VARYINGS_NORMAL(i0, i1, i2) \
    float4 softbodyCarrier : TEXCOORD##i0; \
    float4 softbodyShading : TEXCOORD##i1; \
    float3 softbodyInertiaTilt : TEXCOORD##i2;

// 정점: SoftbodyVertexShading 결과를 보간기에 싣는다.
#define SOFTBODY_TRANSFER_VARYINGS(data, output) \
    output.softbodyCarrier = (data).carrier; \
    output.softbodyShading = (data).shading; \
    output.softbodyInertiaTilt = (data).inertiaTilt; \
    output.softbodyVisColor = (data).visColor;

#define SOFTBODY_TRANSFER_VARYINGS_NORMAL(data, output) \
    output.softbodyCarrier = (data).carrier; \
    output.softbodyShading = (data).shading; \
    output.softbodyInertiaTilt = (data).inertiaTilt;

// 픽셀: 보간기에서 SoftbodyVertexShadingData를 다시 만든다(노멀만 쓰는 패스는 색 0).
SoftbodyVertexShadingData SoftbodyMakeShadingData(float4 carrier, float4 shading, float3 inertiaTilt, float3 visColor)
{
    SoftbodyVertexShadingData data;
    data.carrier = carrier;
    data.shading = shading;
    data.inertiaTilt = inertiaTilt;
    data.visColor = visColor;
    return data;
}

#define SOFTBODY_LOAD_VARYINGS(input) \
    SoftbodyMakeShadingData((input).softbodyCarrier, (input).softbodyShading, (input).softbodyInertiaTilt, (input).softbodyVisColor)

#define SOFTBODY_LOAD_VARYINGS_NORMAL(input) \
    SoftbodyMakeShadingData((input).softbodyCarrier, (input).softbodyShading, (input).softbodyInertiaTilt, float3(0.0, 0.0, 0.0))

// 정점 쪽 한 번에: 이동 전 월드 위치·노멀로 음영 값을 구한다(SMR 노멀은 단위 길이가 아니어도 된다 — 정규화해 쓴다, D-19b).
SoftbodyVertexShadingData SoftbodyVertexShadingOS(uint vertexID, float3 positionOS, float3 normalOS)
{
    return SoftbodyVertexShading(vertexID, TransformObjectToWorld(positionOS), TransformObjectToWorldNormal(normalOS, true));
}

#endif // SOFTBODY_URP_INCLUDED
