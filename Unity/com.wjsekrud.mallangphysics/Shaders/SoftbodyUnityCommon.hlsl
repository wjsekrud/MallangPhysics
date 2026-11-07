// UE `/Engine/Private/Common.ush` 대신 공유 셰이더 사본(Shared/*.hlsl)이 include하는 최소 헤더.
// Tools/sync_shared_shaders.py가 사본의 `#include "/Engine/Private/Common.ush"`를 `#include "../SoftbodyUnityCommon.hlsl"`로 바꾼다.
// 원본(.usf/.ush)이 실제로 쓰는 UE 매크로·상수만 둔다 — UE Common.ush 전체를 흉내 내지 않는다. 원본이 새 UE 매크로를 쓰면
// 컴파일러가 "undeclared identifier"로 알려 주므로 그때 UE 정의를 보고 여기에 더한다.
// #pragma once는 쓰지 않는다: Unity 전처리기는 그것이 파일 첫 줄일 때만 듣는다(fxc는 아예 모른다). include 가드로.
#ifndef SOFTBODY_UNITY_COMMON_HLSL
#define SOFTBODY_UNITY_COMMON_HLSL

// UE Engine/Shaders/Public/Platform.ush:781·785·788 (SM5·SM6에서 켜지는 컴파일러 특성). 원본은 UNROLL·LOOP·BRANCH만 쓴다.
// BRANCH = 조건문을 두 갈래 다 계산해 고르지 말고 실제로 갈라 돌게 하라는 힌트(2026-11-04 기준 원본이 30곳에서 쓴다).
#ifndef UNROLL
#define UNROLL [unroll]
#endif
#ifndef LOOP
#define LOOP [loop]
#endif
#ifndef BRANCH
#define BRANCH [branch]
#endif

// UE Engine/Shaders/Private/Common.ush:133은 `const static float PI = 3.1415926535897932f;`(상수)다. 여기서는 Unity Core RP
// ShaderLibrary/Macros.hlsl:23과 글자까지 같은 매크로로 둔다 — 머티리얼 쪽에서 Core RP를 같이 include해도 재정의 경고가 없고,
// 값(float로 반올림한 값)은 UE와 같다.
#ifndef PI
#define PI          3.14159265358979323846
#endif

#endif // SOFTBODY_UNITY_COMMON_HLSL
