// 소프트바디 머티리얼 함수(엔진 쪽 사용자 HLSL API). UE 머티리얼 함수 셋(MF_SoftbodyKernel·MF_SoftbodyContactNormal·MF_SoftbodyDebugVis)이
// 하던 일을 함수로 낸다 — 계산은 모두 공유 사본(Shaders/Shared, UE 원본 그대로)이 하고, 여기서는 Unity 쪽 입력만 만든다:
//   - 정점 표 주소: UE는 메시 UV 채널(TexelUV)을 읽었고, Unity는 SV_VertexID에서 만든다(D-17).
//   - 단위: 셰이더 안은 cm, Unity 위치는 m — 경계에서 ×100 / ×0.01(D-13).
//   - 원점: UE ActorPositionWS 자리에 _SoftbodyOriginWS(대상 렌더러 위치, D-19).
//   - 이동은 월드 공간에서 더한다(D-19) — 스킨 메시는 그리는 틀이 루트 본(크기 없음)이라 오브젝트 공간에 기대지 않는다.
//
// 지켜야 할 규칙(UE와 같음):
//   1. 정점 표(_SoftbodyAttr2Tex·_SoftbodyPaintTex·_SoftbodyInertiaResponseTex·_SoftbodyCarrierUVTex·_SoftbodyCarrierVertexUVTex·
//      _SoftbodyAttrDbgTex)는 정점 셰이더에서만 읽는다. 칸이 정점 번호 순서라 픽셀이 보간한 주소로 읽으면 무관한 칸을 가로지른다.
//   2. 위치만 옮기고 노멀은 바꿔치기하지 않는다 — 픽셀에서 회전으로 돌린다(노멀맵·저작 노멀이 남는다).
//   3. 위치를 내는 패스는 모두 같은 함수 하나로 같은 변형을 한다(그림자·깊이 우선·모션 벡터가 맞물리게). SoftbodyDisplacedPositionOS와 …WS는
//      같은 값을 다른 계산 순서로 내므로(OS 판은 월드 → 오브젝트로 돌린다) 깊이 비트가 다를 수 있다 — 셰이더 하나 안에서는 둘 중 하나만 쓴다
//      (Softbody/Lit·Shader Graph 함수는 OS 판).
//
// 필요한 것: URP Core.hlsl(또는 같은 Core RP 매크로·공간 변환)을 먼저 include하고, 프로퍼티 선언이 있어야 한다 —
// 손으로 짠 셰이더는 SoftbodyMaterialInput.hlsl, Shader Graph는 블랙보드(같은 이름, Exposed).
// 테셀레이션(맨 아래 절): 공유 사본의 테셀레이션 함수(SoftbodyPhongCornerVS·SoftbodyCornerNormalDispVS·SoftbodyTessellationDisplacement)를
// Softbody/Lit (Tessellated)가 부른다. 부르지 않는 셰이더(Softbody/Lit·Shader Graph)에서는 컴파일러가 버린다.
#ifndef SOFTBODY_MATERIAL_INCLUDED
#define SOFTBODY_MATERIAL_INCLUDED

#include "Packages/com.unity.render-pipelines.core/ShaderLibrary/GlobalSamplers.hlsl"
#include "Packages/com.wjsekrud.mallangphysics/Shaders/Shared/SoftbodySurfaceShading.hlsl"   // SoftbodyFieldCommon·SoftbodyKernel을 함께 넣는다
#include "Packages/com.wjsekrud.mallangphysics/Shaders/Shared/SoftbodyVis.hlsl"

// 정점 표는 칸을 섞으면 안 되므로 포인트, 표면 장 텍스처는 칸 사이를 이어 읽으므로 이선형. 둘 다 텍스처 임포트 설정에 기대지 않는 인라인 샘플러다.
#define SOFTBODY_TABLE_SAMPLER sampler_PointClamp
#define SOFTBODY_FIELD_SAMPLER sampler_LinearClamp

// Unity 1 단위 = 1 m, 셰이더 안 길이 = cm(SoftbodyUnits.CmPerUnit).
#define SOFTBODY_CM_PER_UNIT 100.0
#define SOFTBODY_UNIT_PER_CM 0.01

// 정점이 구해 픽셀로 넘기는 값(UE 보간기 Carrier·Shading·InertiaTilt·DebugVis와 같다).
struct SoftbodyVertexShadingData
{
    float4 carrier;     // 픽셀이 표면 장을 읽을 캐리어 UV(xy), 따라가는 세기(z), 삼각형 안에서 장을 직접 읽는 무게 묶음(w)
    float4 shading;     // 정점의 노멀 변화(월드 xyz = 눌려 기운 노멀 − 원래 노멀)와 함몰 어둡기(w)
    float3 inertiaTilt; // 관성 흔들림이 면을 기울인 회전(축 × sin, 월드)
    float3 visColor;    // 디버그 보기 색(Debug View가 꺼져 있으면 0)
};

// ------------------------------------------------------------------------------------------------------------------------------
// 정점 표 주소(D-17): 칸 = Unity 정점 번호, S = _SoftbodyTextureSize, TexUV = ((id % S) + 0.5, (id / S) + 0.5) / S.
// S = 0(세팅 전)이면 1로 읽는다 — 그때는 관성 줄 수·표면 장 스위치도 0이라 아무것도 움직이지 않는다.
float2 SoftbodyTexelUV(uint vertexID)
{
    uint size = (uint)max(_SoftbodyTextureSize + 0.5, 1.0);
    return (float2(vertexID % size, vertexID / size) + 0.5) / (float)size;
}

// 커널이 쓰는 원점 상대 위치(cm). positionWS·originWS는 월드 m.
float3 SoftbodyRelPosCm(float3 positionWS, float3 originWS)
{
    return (positionWS - originWS) * SOFTBODY_CM_PER_UNIT;
}

// 메시 캐리어 UV(UE 메시 CarrierUV 채널 자리, D-18): 정점마다 자기 UV0 섬 차트의 캐리어 UV. 표가 없거나 봉투 밖이면 (−1, −1) = 표 UV를 쓴다.
float2 SoftbodyMeshCarrierUV(float2 texUV)
{
    float2 carrierUV = float2(-1.0, -1.0);
    UNITY_BRANCH if (_SoftbodyCarrierVertexUVOn > 0.5)
    {
        carrierUV = SAMPLE_TEXTURE2D_LOD(_SoftbodyCarrierVertexUVTex, SOFTBODY_TABLE_SAMPLER, texUV, 0).xy;
    }
    return carrierUV;
}

// UE SoftbodyEvalKernel 호출(MF_SoftbodyKernel의 Custom 노드). 결과 = 월드 cm 이동(접촉 + 관성, 길이 상한 뒤).
// 지금 프레임과 지난 프레임은 브릿지·변위 텍스처·프레임 회전만 바꿔 같은 식으로 부른다(UE PreviousFrameSwitch의 두 노드).
float3 SoftbodyKernelCm(float2 texUV, float3 relPosCm, Texture2D contactData, Texture2D surfaceDispTex,
    float3 l2wX, float3 l2wY, float3 l2wZ)
{
    return SoftbodyEvalKernel(texUV, relPosCm,
        _SoftbodyAttr2Tex, SOFTBODY_TABLE_SAMPLER, contactData, SOFTBODY_TABLE_SAMPLER,
        _SoftbodyPaintTex, SOFTBODY_TABLE_SAMPLER, _SoftbodyInertiaResponseTex, SOFTBODY_TABLE_SAMPLER,
        _SoftbodyCarrierUVTex, SOFTBODY_TABLE_SAMPLER, surfaceDispTex, SOFTBODY_FIELD_SAMPLER,
        _SoftbodySurfaceAttachRange, _SoftbodyMaxDisplacement, _SoftbodySkirtStrength,
        _SoftbodyInertiaResponseCount, l2wX, l2wY, l2wZ,
        _SoftbodySurfaceFieldOn);
}

// 이번 프레임 소프트바디 이동(월드 m). positionWS = 이동 전(스키닝 뒤) 월드 위치.
float3 SoftbodyDisplacementWS(uint vertexID, float3 positionWS)
{
    float2 texUV = SoftbodyTexelUV(vertexID);
    return SOFTBODY_UNIT_PER_CM * SoftbodyKernelCm(texUV, SoftbodyRelPosCm(positionWS, _SoftbodyOriginWS.xyz),
        _SoftbodyContactData, _SoftbodySurfaceDispTex, _SoftbodyL2WX.xyz, _SoftbodyL2WY.xyz, _SoftbodyL2WZ.xyz);
}

// 지난 프레임 소프트바디 이동(월드 m) — 모션 벡터 전용. previousPositionWS = 지난 프레임 이동 전 월드 위치.
// UE와 같이 브릿지·변위·회전만 지난 값이고 정점 표는 같다. 원점도 지난 프레임 것(지난 위치와 짝 — 지난 위치가 없는 Shader Graph 함수는
// 이것 대신 지금 위치·지금 원점으로 SoftbodyKernelCm을 부른다).
float3 SoftbodyPreviousDisplacementWS(uint vertexID, float3 previousPositionWS)
{
    float2 texUV = SoftbodyTexelUV(vertexID);
    return SOFTBODY_UNIT_PER_CM * SoftbodyKernelCm(texUV, SoftbodyRelPosCm(previousPositionWS, _SoftbodyPrevOriginWS.xyz),
        _SoftbodyPrevContactData, _SoftbodyPrevSurfaceDispTex, _SoftbodyPrevL2WX.xyz, _SoftbodyPrevL2WY.xyz, _SoftbodyPrevL2WZ.xyz);
}

// 옮긴 월드 위치. precise: 패스마다 둘레 코드가 달라도 같은 비트가 나오게(깊이 우선 패스의 ZTest Equal·모션 벡터 패스가 같은 깊이를 본다).
float3 SoftbodyDisplacedPositionWS(uint vertexID, float3 positionWS)
{
    precise float3 displaced = positionWS + SoftbodyDisplacementWS(vertexID, positionWS);
    return displaced;
}

// 옮긴 오브젝트 공간 위치 — URP 패스(GetVertexPositionInputs 등)에 그대로 넘긴다. 이동은 월드에서 구하고, 그 벡터만 오브젝트 공간으로 돌린다
// (어떤 그리기 틀이든 월드에서 더한 것과 같다 — 스킨 메시의 틀이 루트 본이어도, 크기가 비균일이어도).
float3 SoftbodyDisplacedPositionOS(uint vertexID, float3 positionOS)
{
    float3 positionWS = TransformObjectToWorld(positionOS);
    precise float3 displaced = positionOS + TransformWorldToObjectDir(SoftbodyDisplacementWS(vertexID, positionWS), false);
    return displaced;
}

// ------------------------------------------------------------------------------------------------------------------------------
// 정점 쪽 음영 값(UE MF_SoftbodyContactNormal·MF_SoftbodyDebugVis의 정점 노드). positionWS·normalWS = 이동 전(노멀은 정규화된 월드 노멀).
SoftbodyVertexShadingData SoftbodyVertexShading(uint vertexID, float3 positionWS, float3 normalWS)
{
    SoftbodyVertexShadingData o;
    float2 texUV = SoftbodyTexelUV(vertexID);
    float2 meshCarrierUV = SoftbodyMeshCarrierUV(texUV);
    // 코너 노멀·코너 함몰·관성 음영은 표 UV로 읽는다(이음매에서 나뉜 두 정점이 같은 값), 픽셀 음영은 메시 캐리어 UV로(UE 배선 그대로).
    // 두 부름은 같은 표를 읽으므로 공유 함수의 메모가 표 읽기를 한 번으로 줄인다.
    float4 carrierCorner = SoftbodySurfaceCarrierVS(texUV, float2(-1.0, -1.0),
        _SoftbodyCarrierUVTex, SOFTBODY_TABLE_SAMPLER, _SoftbodyPaintTex, SOFTBODY_TABLE_SAMPLER,
        _SoftbodySurfaceAttachRange, _SoftbodySkirtStrength, _SoftbodySurfaceFieldOn,
        _SoftbodySurfaceBetweenWeight, _SoftbodySurfaceBetweenEdgeTexels);
    o.carrier = SoftbodySurfaceCarrierVS(texUV, meshCarrierUV,
        _SoftbodyCarrierUVTex, SOFTBODY_TABLE_SAMPLER, _SoftbodyPaintTex, SOFTBODY_TABLE_SAMPLER,
        _SoftbodySurfaceAttachRange, _SoftbodySkirtStrength, _SoftbodySurfaceFieldOn,
        _SoftbodySurfaceBetweenWeight, _SoftbodySurfaceBetweenEdgeTexels);
    o.shading = SoftbodySurfaceShadingVS(carrierCorner, normalWS,
        _SoftbodySurfaceNormalTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceLumpNormalTex, SOFTBODY_FIELD_SAMPLER,
        _SoftbodySurfaceFieldOn, _SoftbodyL2WX.xyz, _SoftbodyL2WY.xyz, _SoftbodyL2WZ.xyz);
    o.inertiaTilt = SoftbodyInertiaTiltVS(texUV, SoftbodyRelPosCm(positionWS, _SoftbodyOriginWS.xyz),
        _SoftbodyAttr2Tex, SOFTBODY_TABLE_SAMPLER, _SoftbodyContactData, SOFTBODY_TABLE_SAMPLER,
        _SoftbodyInertiaResponseTex, SOFTBODY_TABLE_SAMPLER,
        _SoftbodyInertiaResponseCount, carrierCorner, _SoftbodySurfaceFieldOn);
    o.visColor = float3(0.0, 0.0, 0.0);
    // UE는 늘 구하지만 섞는 무게(VisWeight)가 0이면 결과가 같으므로 디버그 보기가 꺼져 있으면 건너뛴다.
    UNITY_BRANCH if (_SoftbodyVisWeight > 0.0)
    {
        o.visColor = SoftbodyBakeVis(texUV, meshCarrierUV,
            _SoftbodyAttrDbgTex, SOFTBODY_TABLE_SAMPLER, _SoftbodyCarrierUVTex, SOFTBODY_TABLE_SAMPLER,
            _SoftbodyVisMode, _SoftbodySurfaceAttachRange, _SoftbodySurfaceBetweenEdgeTexels);
    }
    return o;
}

// ------------------------------------------------------------------------------------------------------------------------------
// 픽셀 쪽. vertexNormalWS = 보간한 정점 노멀(노멀맵 전, 정규화 안 해도 된다).

// 이 픽셀의 표면 장 회전(축 × sin, 월드 xyz)과 함몰 어둡기(w, 0 = 그대로) — UE SoftbodySurfaceShadingPixel.
float4 SoftbodySurfacePixel(SoftbodyVertexShadingData data, float3 vertexNormalWS)
{
    return SoftbodySurfaceShadingPixel(data.shading, data.carrier, vertexNormalWS,
        _SoftbodySurfaceNormalTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceLumpNormalTex, SOFTBODY_FIELD_SAMPLER,
        _SoftbodySurfaceFieldOn, _SoftbodyL2WX.xyz, _SoftbodyL2WY.xyz, _SoftbodyL2WZ.xyz);
}

// 방향 하나(노멀·탄젠트 등)를 관성 회전 다음 표면 장 회전만큼 돌린다 — UE SoftbodySurfaceShadingPS와 같은 두 회전(정규화는 하지 않는다).
// 회전은 선형이라 노멀과 탄젠트를 같이 돌리면 노멀맵을 얹은 노멀을 돌린 것과 같다.
float3 SoftbodyRotateByShading(float3 direction, float4 surfacePixel, float3 inertiaTilt)
{
    return SB_RotateByTilt(surfacePixel.xyz, SB_RotateByTilt(inertiaTilt, direction));
}

// 픽셀 노멀(월드, 정규화) — UE MF_SoftbodyContactNormal의 Normal 출력. baseNormalWS = 노멀맵을 얹은 노멀. occlusion = 1 − 함몰 어둡기(1 = 그대로).
float3 SoftbodyPixelNormalWS(SoftbodyVertexShadingData data, float3 vertexNormalWS, float3 baseNormalWS, out float occlusion)
{
    float4 pix = SoftbodySurfacePixel(data, vertexNormalWS);
    occlusion = 1.0 - pix.w;
    return SoftbodySurfaceShadingPS(pix, data.inertiaTilt, baseNormalWS);
}

// 기본 색: 함몰 어둡기를 곱한 뒤 디버그 색과 섞는다(UE 호스트: BaseColorTex × Occlusion → MF_SoftbodyDebugVis의 lerp).
// 어둡기는 AO가 아니라 기본 색에 곱한다 — AO는 간접광에만 들어 눌린 자리가 직접광 아래에서 어두워지지 않는다.
float3 SoftbodyBaseColor(float3 baseColor, float occlusion, float3 visColor)
{
    return lerp(baseColor * occlusion, visColor, _SoftbodyVisWeight);
}

// ------------------------------------------------------------------------------------------------------------------------------
// 테셀레이션(UE 나나이트 테셀레이션 자리, U0 §6·51-r5-tess). 화면 효과: 손끝처럼 정점 사이에 드는 좁은 눌림이 음영만이 아니라 모양으로 보인다.
// 말:
//   코너   = 원래 정점. 정점 셰이더가 지금처럼 커널로 옮긴다(SoftbodyDisplacedPositionOS).
//   쪼갠 점 = 테셀레이터가 삼각형 변 위와 안에 새로 낸 점. 위치 = 세 코너의 옮긴 위치를 무게중심 좌표로 이은 점
//            + 보간 노멀 방향으로 SoftbodyTessellationDisplacement의 높이(UE TESS_CALL과 같은 입력).
//   쪼갬 수 = 변 하나를 몇 조각으로 나누는가(테셀레이션 인자).
// 코너 쪽 값(정점 셰이더, 정점 표를 읽는다)과 쪼갠 점 쪽 값(domain 셰이더, 보간 값과 표면 장 텍스처만 읽는다)을 나눈다 — 위 규칙 1.
// 이 절의 함수는 프로퍼티(테셀레이션 값 셋)를 인자로 받는다 — 이 파일은 그 프로퍼티가 없는 셰이더(Softbody/Lit·Shader Graph)에도 들어간다.

// 공유 함수가 돌려주는 0~1 값의 중심. 크기는 SoftbodyTessMagnitudeCm. 둘은 공유 사본을 고치지 않고 cm를 되살리려는 값이다:
// 값 = saturate(높이 / 크기 + 중심) → 높이 = (값 − 중심) × 크기. 크기 = 2 × Max Displacement라 |높이| ≤ Max Displacement가 잘리지 않고,
// 되살린 높이의 오차는 값의 반올림(0.5~1 사이 float 간격 6e-8) × 크기 — Max Displacement 50 cm에서 6e-6 cm다.
#define SOFTBODY_TESS_CENTER 0.5

float SoftbodyTessMagnitudeCm()
{
    return 2.0 * max(_SoftbodyMaxDisplacement, 1.0);
}

// 이 정점의 표 UV 캐리어(UE carrierCorner). SoftbodyVertexShading이 같은 인자로 이미 부른 뒤라 공유 함수의 메모가 표를 다시 읽지 않는다.
float4 SoftbodyCornerCarrier(uint vertexID)
{
    return SoftbodySurfaceCarrierVS(SoftbodyTexelUV(vertexID), float2(-1.0, -1.0),
        _SoftbodyCarrierUVTex, SOFTBODY_TABLE_SAMPLER, _SoftbodyPaintTex, SOFTBODY_TABLE_SAMPLER,
        _SoftbodySurfaceAttachRange, _SoftbodySkirtStrength, _SoftbodySurfaceFieldOn,
        _SoftbodySurfaceBetweenWeight, _SoftbodySurfaceBetweenEdgeTexels);
}

// 코너에 쪼갤 값이 있는가(1/0): 접촉 게인이 있거나 덩어리 노멀을 읽는다 — SB_SurfaceShadingCorner가 0을 돌려주지 않는 조건과 같다.
// 둘 다 아닌 코너만으로 된 변은 쪼개지 않는다(쪼갠 점 높이가 0이라 모양이 같다). 표면 장이 꺼져 있으면 캐리어가 0이라 늘 0.
float SoftbodyTessCornerActive(float4 carrierCorner)
{
    bool lump = SB_LumpNormalsOn(_SoftbodySurfaceFieldOn) && SB_CornerMapped(carrierCorner);
    return (carrierCorner.z > 0.0 || lump) ? 1.0 : 0.0;
}

// 코너 함몰의 노멀 성분(cm, UE CORNER_DISP_VS_CALL). previous = 지난 프레임 변위 텍스처·회전(모션 벡터의 지난 위치).
float SoftbodyTessCornerDispCm(float4 carrierCorner, float3 normalWS, bool previous)
{
    if (previous)
    {
        return SoftbodyCornerNormalDispVS(carrierCorner, normalWS, _SoftbodyPrevSurfaceDispTex, SOFTBODY_FIELD_SAMPLER,
            _SoftbodySurfaceLumpNormalTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceFieldOn,
            _SoftbodyPrevL2WX.xyz, _SoftbodyPrevL2WY.xyz, _SoftbodyPrevL2WZ.xyz);
    }
    return SoftbodyCornerNormalDispVS(carrierCorner, normalWS, _SoftbodySurfaceDispTex, SOFTBODY_FIELD_SAMPLER,
        _SoftbodySurfaceLumpNormalTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceFieldOn,
        _SoftbodyL2WX.xyz, _SoftbodyL2WY.xyz, _SoftbodyL2WZ.xyz);
}

// 쪼갬 수를 정하는 카메라(전역, Softbody 런타임 SoftbodyTessellationCamera가 카메라마다 그리기 전에 넣는다):
// xyz = 카메라 월드 위치(m), w = 거리 1 m에서 1 m가 화면에서 몇 픽셀인가(원근, = 0.5 × 화면 높이 × 투영 [1][1]),
// 정사영이면 −(1 m의 픽셀 수, 거리와 무관). 그림자 패스도 같은 값을 보게 셰이더 상수(빛 시점 투영·_ScreenParams)를 쓰지 않는다.
// 0(넣는 쪽이 없음 — 시험 그리기 등)이면 지금 그리는 시점의 카메라 상수로 어림한다.
float4 _SoftbodyTessCamera;

// 변 하나의 쪼갬 수. a·b = 두 끝 코너(xyz 이동 전 월드 위치 m, w = SoftbodyTessCornerActive). 두 끝 값만 쓰고 두 끝을 바꿔 불러도 같은
// 비트가 나와(합·차의 제곱만 쓴다) 변을 나눠 가진 이웃 삼각형이 같은 수로 쪼갠다 — 틈이 없다. 위치는 옮기기 전 것이다: UV 이음매에서
// 나뉜 두 정점 사본은 옮기기 전 위치 비트가 같다.
// 화면 길이(픽셀) ÷ edgeLengthPixels를 1~maxFactor(≤ 64)로 자른다. 접촉이 꺼졌거나 두 끝 다 쪼갤 값이 없으면 1.
float SoftbodyTessEdgeFactor(float4 a, float4 b, float edgeLengthPixels, float maxFactor)
{
    precise float factor = 1.0;
    if (_SoftbodySurfaceFieldOn >= 0.5 && max(a.w, b.w) > 0.5)
    {
        float3 cameraWS = _SoftbodyTessCamera.xyz;
        float pixelsPerMeter = _SoftbodyTessCamera.w;
        if (pixelsPerMeter == 0.0)
        {
            cameraWS = _WorldSpaceCameraPos.xyz;
            pixelsPerMeter = 0.5 * _ScreenParams.y * abs(UNITY_MATRIX_P[1][1]) * (unity_OrthoParams.w > 0.5 ? -1.0 : 1.0);
        }
        precise float3 middle = (a.xyz + b.xyz) * 0.5;
        precise float3 edge = a.xyz - b.xyz;
        precise float lengthM = sqrt(dot(edge, edge));
        precise float pixels = pixelsPerMeter < 0.0
            ? lengthM * -pixelsPerMeter
            : lengthM * pixelsPerMeter / max(distance(middle, cameraWS), 1e-4);
        factor = clamp(pixels / max(edgeLengthPixels, 1.0), 1.0, clamp(maxFactor, 1.0, 64.0));
    }
    return factor;
}

// 쪼갠 점의 노멀 방향 높이(cm, + = 바깥). 인자는 UE TESS_CALL 그대로(보간 값): carrier = 메시 캐리어(픽셀과 같은 값), cornerDisp·phongDot = 코너 값의 보간,
// normalWS = 보간한 이동 전 노멀(월드), relPosCm = 쪼갠 점의 이동 전 위치(원점 기준 cm). previous = 지난 프레임 변위 텍스처·회전.
float SoftbodyTessSplitHeightCm(float4 carrier, float cornerDisp, float3 normalWS, float4 shading, float phongDot, float3 relPosCm,
    float phongStrength, bool previous)
{
    float magnitude = SoftbodyTessMagnitudeCm();
    float value;
    if (previous)
    {
        value = SoftbodyTessellationDisplacement(carrier, cornerDisp, normalWS, shading, phongDot, relPosCm, phongStrength,
            _SoftbodyPrevSurfaceDispTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceLumpNormalTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceFieldOn,
            _SoftbodyPrevL2WX.xyz, _SoftbodyPrevL2WY.xyz, _SoftbodyPrevL2WZ.xyz, magnitude, SOFTBODY_TESS_CENTER);
    }
    else
    {
        value = SoftbodyTessellationDisplacement(carrier, cornerDisp, normalWS, shading, phongDot, relPosCm, phongStrength,
            _SoftbodySurfaceDispTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceLumpNormalTex, SOFTBODY_FIELD_SAMPLER, _SoftbodySurfaceFieldOn,
            _SoftbodyL2WX.xyz, _SoftbodyL2WY.xyz, _SoftbodyL2WZ.xyz, magnitude, SOFTBODY_TESS_CENTER);
    }
    return (value - SOFTBODY_TESS_CENTER) * magnitude;
}

#endif // SOFTBODY_MATERIAL_INCLUDED
