// Softbody/Lit (Tessellated)의 입력. Softbody/Lit의 입력(SoftbodyLitInput.hlsl — URP LitInput 사본)에 테셀레이션 프로퍼티 셋을 더한다.
// UnityPerMaterial 배치가 Softbody/Lit과 다르므로(프로퍼티 셋이 늘었다) 이 셰이더의 모든 패스가 이 파일을 include한다 — SRP Batcher 조건.
//   _SoftbodyTessEdgeLength  쪼갠 삼각형 변이 화면에서 노리는 길이(픽셀). 작을수록 촘촘하다.
//   _SoftbodyTessMaxFactor   변 하나를 최대 몇 조각으로(1~64).
//   _SoftbodyPhongStrength   UE SoftbodyPhongStrength — 장을 읽지 않는 쪼갠 점이 눌려 기운 코너 노멀을 따라 휘는 정도(0 평평 ~ 1).
// 그다음 hull·domain 도우미(SoftbodyLitTessCommon.hlsl)를 넣는다.
#ifndef SOFTBODY_LIT_TESS_INPUT_INCLUDED
#define SOFTBODY_LIT_TESS_INPUT_INCLUDED

#define SOFTBODY_PER_MATERIAL_EXTRA_PROPERTIES \
    float _SoftbodyTessEdgeLength; \
    float _SoftbodyTessMaxFactor; \
    float _SoftbodyPhongStrength;

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitTessCommon.hlsl"

#endif // SOFTBODY_LIT_TESS_INPUT_INCLUDED
