// 소프트바디 머티리얼 프로퍼티 선언(손으로 짠 셰이더용). 이름은 런타임 SoftbodyShaderIds.Names와 글자까지 같다(DECISIONS D-30) —
// 컴포넌트가 렌더러마다 머티리얼 사본에 이 이름으로 값을 넣는다.
//
// 쓰는 법(손으로 짠 URP 셰이더):
//   1. ShaderLab Properties 블록에 같은 이름의 프로퍼티를 둔다(Include/README.md에 복사할 줄이 있다). 텍스처 넷
//      (_SoftbodyContactData·_SoftbodyInertiaResponseTex·_SoftbodySurfaceDispTex·_SoftbodyCarrierUVTex)이 Properties에 있어야
//      컴포넌트·굽기·저작이 "소프트바디 머티리얼"로 알아본다(Material.HasTexture).
//   2. 이 파일을 URP Core.hlsl 다음에 include하고, 셰이더의 CBUFFER_START(UnityPerMaterial) 안에 SOFTBODY_PER_MATERIAL_PROPERTIES를 펼친다.
//      모든 패스가 같은 상수 버퍼 배치여야 SRP Batcher가 받는다.
//   3. 그다음 SoftbodyMaterial.hlsl(함수)을 include한다.
// Shader Graph는 이 파일을 쓰지 않는다 — 블랙보드 프로퍼티가 같은 이름으로 선언된다(SoftbodyShaderGraph.hlsl).
//
// 값의 뜻(UE 머티리얼 파라미터와 같음, 길이는 cm):
//   텍스처 — 관성 브릿지 지금·지난(8×9 RGBA16F), 정점 표(굽기 텍스처, 칸 = 정점 번호), 표면 장 결과(변위·지난 변위·노멀 회전·덩어리 노멀).
//   _SoftbodyL2WX/Y/Z(.xyz) = 표면 장 프레임 → 월드 회전의 행 0·1·2, _SoftbodyPrevL2W* = 지난 프레임 것.
//   _SoftbodyOriginWS(.xyz, 월드 m) = 프레임 원점(대상 렌더러 위치), _SoftbodyPrevOriginWS = 지난 프레임 것.
//   _SoftbodyTextureSize = 정점 표 한 변(0 = 세팅 전), _SoftbodyInertiaResponseCount = 관성 표 줄 수(0 = 관성 끔),
//   _SoftbodySurfaceFieldOn = 0 접촉 없음 / 1 표면 장 / 2 표면 장 + 덩어리 노멀 — UE는 Contact Detail Full이면 0.25를 더한다(1.25·2.25, 일지 361:
//     공유 사본 SB_FullDetailOn = 소수 부분 > 0.125가 함몰 벽을 4x4 칸으로 세운다). Unity도 Full로 도는 틱에 컴포넌트가 0.25를 더한다(D-66) — 소수 부분 0 = Basic
//     규칙(SB_EdgeSameSideBlend). 그 갈래를 부르는 쪼갠 점 변위(SoftbodyTessellationDisplacement)는 Softbody/Lit (Tessellated)만 쓴다,
//     _SoftbodySurfaceAttachRange = 따라가는 거리(Follow Range),
//   _SoftbodyMaxDisplacement = 최종 이동 길이 상한, _SoftbodySkirtStrength = 접촉을 따르는 세기(머티리얼 값),
//   _SoftbodySurfaceBetweenWeight·_SoftbodySurfaceBetweenEdgeTexels = 삼각형 안 픽셀이 장을 직접 읽는 무게의 배율·문턱(머티리얼 값),
//   _SoftbodyCarrierVertexUVOn = 1이면 정점마다 캐리어 UV 표를 읽음, _SoftbodyVisWeight·_SoftbodyVisMode = 디버그 보기(Debug View).
// #pragma once는 쓰지 않는다(Unity 전처리기는 파일 첫 줄일 때만 듣는다, 규약 10 §6.1) — include 가드로.
#ifndef SOFTBODY_MATERIAL_INPUT_INCLUDED
#define SOFTBODY_MATERIAL_INPUT_INCLUDED

// 셰이더 하나가 프로퍼티를 더 둘 때(Softbody/Lit (Tessellated)의 테셀레이션 값 셋 — SoftbodyLitTessInput.hlsl) 이 파일보다 먼저 정의한다.
// 비어 있으면 아래 배치는 Softbody/Lit과 같다.
#ifndef SOFTBODY_PER_MATERIAL_EXTRA_PROPERTIES
#define SOFTBODY_PER_MATERIAL_EXTRA_PROPERTIES
#endif

// UnityPerMaterial 안에 펼친다. float4 여덟 개를 앞에 두어 16바이트 경계에 맞추고 스칼라 열하나(+ 위에서 더한 프로퍼티)가 뒤따른다.
#define SOFTBODY_PER_MATERIAL_PROPERTIES \
    float4 _SoftbodyL2WX; \
    float4 _SoftbodyL2WY; \
    float4 _SoftbodyL2WZ; \
    float4 _SoftbodyPrevL2WX; \
    float4 _SoftbodyPrevL2WY; \
    float4 _SoftbodyPrevL2WZ; \
    float4 _SoftbodyOriginWS; \
    float4 _SoftbodyPrevOriginWS; \
    float _SoftbodyTextureSize; \
    float _SoftbodyInertiaResponseCount; \
    float _SoftbodySurfaceFieldOn; \
    float _SoftbodySurfaceAttachRange; \
    float _SoftbodyMaxDisplacement; \
    float _SoftbodySkirtStrength; \
    float _SoftbodySurfaceBetweenWeight; \
    float _SoftbodySurfaceBetweenEdgeTexels; \
    float _SoftbodyCarrierVertexUVOn; \
    float _SoftbodyVisWeight; \
    float _SoftbodyVisMode; \
    SOFTBODY_PER_MATERIAL_EXTRA_PROPERTIES

// 텍스처는 상수 버퍼 밖(SRP Batcher와 무관). 샘플러는 따로 두지 않는다 — 정점 표는 sampler_PointClamp, 장은 sampler_LinearClamp(인라인)를
// 쓴다(D3D11 샘플러 16개 한도에 둘만 더한다).
TEXTURE2D(_SoftbodyContactData);
TEXTURE2D(_SoftbodyPrevContactData);
TEXTURE2D(_SoftbodyAttr2Tex);
TEXTURE2D(_SoftbodyAttrDbgTex);
TEXTURE2D(_SoftbodyPaintTex);
TEXTURE2D(_SoftbodyInertiaResponseTex);
TEXTURE2D(_SoftbodyCarrierUVTex);
TEXTURE2D(_SoftbodyCarrierVertexUVTex);
TEXTURE2D(_SoftbodySurfaceDispTex);
TEXTURE2D(_SoftbodyPrevSurfaceDispTex);
TEXTURE2D(_SoftbodySurfaceNormalTex);
TEXTURE2D(_SoftbodySurfaceLumpNormalTex);

#endif // SOFTBODY_MATERIAL_INPUT_INCLUDED
