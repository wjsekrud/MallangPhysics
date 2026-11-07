// Softbody/Lit (Tessellated)의 hull·domain 도우미(모든 위치 패스가 같이 쓴다). 말은 SoftbodyMaterial.hlsl 테셀레이션 절:
// 코너 = 원래 정점, 쪼갠 점 = 테셀레이터가 새로 낸 점, 쪼갬 수 = 변 하나를 나누는 조각 수, 제어점 = 정점 셰이더가 hull로 넘기는 코너 값 묶음.
//
// 단계:
//   정점 셰이더 — 코너를 Softbody/Lit과 같은 함수(SoftbodyDisplacedPositionOS)로 옮기고, 정점 표를 읽어 쪼갠 점이 쓸 코너 값을 만든다
//                (SoftbodyTessMakeControlPoint). 정점 표는 여기서만 읽는다.
//   hull         — 제어점을 그대로 넘기고(SoftbodyTessHull), 변마다 쪼갬 수를 그 변 두 끝 코너 값만으로 정한다(SoftbodyTessPatchConstant).
//                  안쪽 쪼갬 수 = 세 변의 최대.
//   domain       — 코너 값을 무게중심 좌표로 잇고 쪼갠 점 높이를 더해 최종 위치를 낸 뒤(SoftbodyTessDomainPoint) 패스의 URP 정점 함수를
//                  부른다(패스 파일). 보간 값과 표면 장 텍스처만 읽는다.
// 틈이 없게: 이웃 삼각형이 나눠 가진 변 위의 쪼갠 점은 두 삼각형에서 같은 두 코너 값·같은 무게(테셀레이터가 변 위 점을 두 쪽에서 같게 낸다)로
// 계산된다. 위치에 닿는 계산은 모두 precise라 더하는 차례·곱셈-덧셈 합치기가 삼각형마다 달라지지 않는다(셋째 무게 0은 정확히 0을 더한다).
// 패스마다 쪼갬 수·위치 식이 같은 함수라 깊이 우선(DepthOnly)·ForwardLit(ZTest Equal)·모션 벡터가 같은 깊이를 본다.
// 필요: 먼저 SoftbodyLitTessInput.hlsl(프로퍼티 셋과 SoftbodyLitInput.hlsl).
// 고를 수 있는 것(패스 파일이 include 전에 정의):
//   SOFTBODY_TESS_PIXEL_SHADING — 관성 기울기·디버그 색도 넘긴다(색·노멀을 쓰는 패스).
//   SOFTBODY_TESS_PREVIOUS      — 지난 프레임 위치도 쪼갠 점에서(모션 벡터 패스).
#ifndef SOFTBODY_LIT_TESS_COMMON_INCLUDED
#define SOFTBODY_LIT_TESS_COMMON_INCLUDED

// 정점 셰이더 출력 = hull 입출력 = domain 입력. 시스템 값이 아닌 의미 이름만 쓴다(정점 셰이더가 SV_Position을 내지 않는다).
struct SoftbodyTessControlPoint
{
    float3 positionOS : INTERNALTESSPOS;   // 커널로 옮긴 코너(오브젝트 공간)
    float3 normalOS : TEXCOORD0;           // 옮기기 전 노멀(노멀은 정점에서 바꾸지 않는다 — 픽셀이 돌린다)
    float4 tangentOS : TEXCOORD1;
    float4 texcoord : TEXCOORD2;           // xy = UV0, zw = 정적 라이트맵 UV
    float4 dynamicLightmapUV : TEXCOORD3;  // xy = 동적 라이트맵 UV(zw 0). float2로 두면 Vulkan(HLSLcc)이 DYNAMICLIGHTMAP_ON 정점 단계를 잘못 옮긴다(vec3 * vec4)
    float4 restPositionWS : TEXCOORD4;     // xyz = 옮기기 전 월드 위치(m), w = 쪼갤 값이 있는가(SoftbodyTessCornerActive) — 쪼갬 수 전용
    float4 carrier : TEXCOORD5;            // 픽셀과 같은 캐리어(메시 캐리어 UV) — UE 보간기 Carrier
    float4 shading : TEXCOORD6;            // 노멀 변화·어둡기 — UE 보간기 Shading
    float4 relPosCornerDisp : TEXCOORD7;   // xyz = 원점 기준 옮기기 전 위치(cm), w = 코너 함몰의 노멀 성분(cm) — UE RelPos·CornerDisp
    float phongDot : TEXCOORD8;            // 퐁 곡면 코너 몫 r·e — UE PhongDot
#if defined(SOFTBODY_TESS_PIXEL_SHADING)
    float3 inertiaTilt : TEXCOORD9;
    float3 visColor : TEXCOORD10;
#endif
#if defined(SOFTBODY_TESS_PREVIOUS)
    float4 prevPositionWS : TEXCOORD11;    // xyz = 지난 프레임 옮긴 코너(월드 m), w = 지난 코너 함몰의 노멀 성분(cm)
    float4 prevRelPosPhong : TEXCOORD12;   // xyz = 지난 원점 기준 옮기기 전 위치(cm), w = 지난 퐁 코너 몫
#endif
};

struct SoftbodyTessFactors
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

// 정점 셰이더: 이동 전 오브젝트 위치·노멀에서 제어점을 만든다.
SoftbodyTessControlPoint SoftbodyTessMakeControlPoint(uint vertexID, float3 positionOS, float3 normalOS, float4 tangentOS,
    float4 texcoord, float2 dynamicLightmapUV)
{
    SoftbodyTessControlPoint o = (SoftbodyTessControlPoint)0;
    float3 positionWS = TransformObjectToWorld(positionOS);
    float3 normalWS = TransformObjectToWorldNormal(normalOS, true);
    SoftbodyVertexShadingData data = SoftbodyVertexShading(vertexID, positionWS, normalWS);
    float4 carrierCorner = SoftbodyCornerCarrier(vertexID);
    float3 relPosCm = SoftbodyRelPosCm(positionWS, _SoftbodyOriginWS.xyz);

    o.positionOS = SoftbodyDisplacedPositionOS(vertexID, positionOS);
    o.normalOS = normalOS;
    o.tangentOS = tangentOS;
    o.texcoord = texcoord;
    o.dynamicLightmapUV = float4(dynamicLightmapUV, 0.0, 0.0);
    o.restPositionWS = float4(positionWS, SoftbodyTessCornerActive(carrierCorner));
    o.carrier = data.carrier;
    o.shading = data.shading;
    o.relPosCornerDisp = float4(relPosCm, SoftbodyTessCornerDispCm(carrierCorner, normalWS, false));
    o.phongDot = SoftbodyPhongCornerVS(data.shading, relPosCm);
#if defined(SOFTBODY_TESS_PIXEL_SHADING)
    o.inertiaTilt = data.inertiaTilt;
    o.visColor = data.visColor;
#endif
    return o;
}

#if defined(SOFTBODY_TESS_PREVIOUS)
// 모션 벡터: 지난 프레임 코너(지난 이동 전 월드 위치 + 지난 커널 이동)와 쪼갠 점이 쓸 지난 코너 값. 노멀 변화(shading)·덩어리 높이는 지난 값이
// 없어 지금 값을 쓴다(UE도 지난 노멀 텍스처가 없다). normalOS = 지금 노멀.
void SoftbodyTessSetPrevious(inout SoftbodyTessControlPoint o, uint vertexID, float3 previousPositionWS, float3 normalOS)
{
    float3 normalWS = TransformObjectToWorldNormal(normalOS, true);
    float4 carrierCorner = SoftbodyCornerCarrier(vertexID);
    float3 prevRelPosCm = SoftbodyRelPosCm(previousPositionWS, _SoftbodyPrevOriginWS.xyz);
    o.prevPositionWS = float4(previousPositionWS + SoftbodyPreviousDisplacementWS(vertexID, previousPositionWS),
        SoftbodyTessCornerDispCm(carrierCorner, normalWS, true));
    o.prevRelPosPhong = float4(prevRelPosCm, SoftbodyPhongCornerVS(o.shading, prevRelPosCm));
}
#endif

// hull: 쪼갬 수. edge[i]는 코너 i 맞은편 변(무게 i가 0인 변)이다.
SoftbodyTessFactors SoftbodyTessPatchConstant(InputPatch<SoftbodyTessControlPoint, 3> patch)
{
    SoftbodyTessFactors f;
    f.edge[0] = SoftbodyTessEdgeFactor(patch[1].restPositionWS, patch[2].restPositionWS, _SoftbodyTessEdgeLength, _SoftbodyTessMaxFactor);
    f.edge[1] = SoftbodyTessEdgeFactor(patch[2].restPositionWS, patch[0].restPositionWS, _SoftbodyTessEdgeLength, _SoftbodyTessMaxFactor);
    f.edge[2] = SoftbodyTessEdgeFactor(patch[0].restPositionWS, patch[1].restPositionWS, _SoftbodyTessEdgeLength, _SoftbodyTessMaxFactor);
    f.inside = max(f.edge[0], max(f.edge[1], f.edge[2]));
    return f;
}

// fractional_odd: 카메라가 다가가며 쪼갬 수가 늘 때 점이 튀지 않고 미끄러져 나온다(최대 63 — 64를 넣어도 63으로 쪼갠다).
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[patchconstantfunc("SoftbodyTessPatchConstant")]
[outputcontrolpoints(3)]
SoftbodyTessControlPoint SoftbodyTessHull(InputPatch<SoftbodyTessControlPoint, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

// domain에서 낸 쪼갠 점.
struct SoftbodyTessPoint
{
    float3 positionOS;          // 최종 위치(오브젝트) — 패스의 URP 정점 함수에 넘긴다
    float3 normalOS;            // 보간 노멀(정규화 안 함 — URP가 한다)
    float4 tangentOS;
    float4 texcoord;
    float2 dynamicLightmapUV;
    float4 carrier;
    float4 shading;
    float3 inertiaTilt;
    float3 visColor;
#if defined(SOFTBODY_TESS_PREVIOUS)
    float3 previousPositionWS;  // 지난 프레임 위치(월드 m)
#endif
};

// 셋째 항을 마지막에 더하는 차례를 모든 값에서 같게 둔다(변 위 점은 한 무게가 0이라 두 항의 합이 되고, 덧셈은 바꿔 더해도 같다).
#define SOFTBODY_TESS_LERP(p0, p1, p2, bary, member) (((p0).member * (bary).x + (p1).member * (bary).y) + (p2).member * (bary).z)

// domain: 쪼갠 점의 위치와 보간 값. 위치 = 옮긴 코너를 이은 점 + 보간 노멀 방향 높이(SoftbodyTessSplitHeightCm).
SoftbodyTessPoint SoftbodyTessDomainPoint(SoftbodyTessControlPoint p0, SoftbodyTessControlPoint p1, SoftbodyTessControlPoint p2, float3 bary)
{
    SoftbodyTessPoint o = (SoftbodyTessPoint)0;
    precise float3 cornerOS = SOFTBODY_TESS_LERP(p0, p1, p2, bary, positionOS);
    precise float3 normalOS = SOFTBODY_TESS_LERP(p0, p1, p2, bary, normalOS);
    precise float4 carrier = SOFTBODY_TESS_LERP(p0, p1, p2, bary, carrier);
    precise float4 shading = SOFTBODY_TESS_LERP(p0, p1, p2, bary, shading);
    precise float4 relPosCornerDisp = SOFTBODY_TESS_LERP(p0, p1, p2, bary, relPosCornerDisp);
    precise float phongDot = SOFTBODY_TESS_LERP(p0, p1, p2, bary, phongDot);
    precise float3 normalWS = SafeNormalize(TransformObjectToWorldNormal(normalOS, false));
    precise float heightCm = SoftbodyTessSplitHeightCm(carrier, relPosCornerDisp.w, normalWS, shading, phongDot, relPosCornerDisp.xyz,
        _SoftbodyPhongStrength, false);
    precise float3 positionOS = cornerOS + TransformWorldToObjectDir(normalWS * (heightCm * SOFTBODY_UNIT_PER_CM), false);

    o.positionOS = positionOS;
    o.normalOS = normalOS;
    o.tangentOS = SOFTBODY_TESS_LERP(p0, p1, p2, bary, tangentOS);
    o.texcoord = SOFTBODY_TESS_LERP(p0, p1, p2, bary, texcoord);
    o.dynamicLightmapUV = SOFTBODY_TESS_LERP(p0, p1, p2, bary, dynamicLightmapUV).xy;
    o.carrier = carrier;
    o.shading = shading;
#if defined(SOFTBODY_TESS_PIXEL_SHADING)
    o.inertiaTilt = SOFTBODY_TESS_LERP(p0, p1, p2, bary, inertiaTilt);
    o.visColor = SOFTBODY_TESS_LERP(p0, p1, p2, bary, visColor);
#endif
#if defined(SOFTBODY_TESS_PREVIOUS)
    precise float4 prevPosition = SOFTBODY_TESS_LERP(p0, p1, p2, bary, prevPositionWS);
    precise float4 prevRelPosPhong = SOFTBODY_TESS_LERP(p0, p1, p2, bary, prevRelPosPhong);
    precise float prevHeightCm = SoftbodyTessSplitHeightCm(carrier, prevPosition.w, normalWS, shading, prevRelPosPhong.w, prevRelPosPhong.xyz,
        _SoftbodyPhongStrength, true);
    o.previousPositionWS = prevPosition.xyz + normalWS * (prevHeightCm * SOFTBODY_UNIT_PER_CM);
#endif
    return o;
}

#define SOFTBODY_TESS_DOMAIN_POINT(patch, bary) SoftbodyTessDomainPoint((patch)[0], (patch)[1], (patch)[2], (bary))

// 픽셀 쪽 보간기 묶음(SOFTBODY_TRANSFER_VARYINGS가 받는 꼴).
SoftbodyVertexShadingData SoftbodyTessShadingData(SoftbodyTessPoint p)
{
    return SoftbodyMakeShadingData(p.carrier, p.shading, p.inertiaTilt, p.visColor);
}

#endif // SOFTBODY_LIT_TESS_COMMON_INCLUDED
