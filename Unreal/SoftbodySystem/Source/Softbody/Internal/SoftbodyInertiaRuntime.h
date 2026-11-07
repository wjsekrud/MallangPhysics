// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"

namespace SoftbodyInertiaRuntime
{
	/**
	 * q'' + 2 zeta omega q' + omega^2(q-qEq)=0의 한 프레임 해석해.
	 * 프레임 동안 qEq가 일정하다고 보고 적분하므로 큰 DeltaTime에서도 폭주하지 않는다.
	 */
	SOFTBODY_API void AdvanceResponse(
		float& Q, float& QDot, float QEq,
		float FrequencyHz, float DampingRatio, float DeltaTime);

	/**
	 * x'' = −ω²(x − xEq) − 2a·x'의 한 프레임 해석해(ADR-0012 §8). a = 감쇠 계수의 절반(1/s). AdvanceResponse와 달리 진동수를
	 * 그대로 받고, 감쇠가 진동수보다 커도(a ≥ ω, 출렁이지 않고 가라앉음) 된다.
	 */
	SOFTBODY_API void AdvanceDampedMode(
		double& X, double& XDot, double XEq, double Omega, double Decay, double DeltaTime);

	/** 대칭 3×3 행렬의 고윳값과 고유벡터(야코비 회전). OutVectors의 열 i가 고윳값 i의 단위 고유벡터다. */
	SOFTBODY_API void SymmetricEigen3(const double In[3][3], double OutValues[3], double OutVectors[3][3]);

	/** 이전→현재 월드 회전의 최단 호에서 월드 각속도(rad/s)를 구한다. */
	SOFTBODY_API FVector ComputeAngularVelocity(
		const FQuat& Previous, const FQuat& Current, float DeltaTime);

	/** Express a world-space vector in the mesh reference-pose component axes. */
	SOFTBODY_API FVector ToReferenceAxes(
		const FVector& WorldVector,
		const FQuat& CurrentWorldRotation,
		const FQuat& ReferenceComponentRotation);

	/** Rotation that transports a reference-pose component vector into world space. */
	SOFTBODY_API FQuat ReferenceToWorldRotation(
		const FQuat& CurrentWorldRotation,
		const FQuat& ReferenceComponentRotation);

	/**
	 * 소프트 상한. 상한 L의 SoftLimitStart 비율(K = SoftLimitStart·L)까지는 X를 그대로 돌려주고, [K, 2L−K]에서 기울기가
	 * 이어지는 곡선 K + (L−K)(2t − t²)로 L에 붙으며, 그 너머는 L이다(셰이더의 SB_SoftLimit과 같은 식). 0 이하는 0.
	 */
	SOFTBODY_API float SoftLimit(float X, float Limit, float SoftLimitStart);

	/**
	 * 상한(관성 흔들림·최종 변위)이 휘기 시작하는 비율. 조절값(Soft Limit Start)이었다가 아무도 바꾸지 않아 고정했다(일지 227).
	 * 머티리얼의 최종 변위 상한(SoftbodyKernel.ush SB_CAP_SOFT_START)과 같은 값이다. 접촉 깊이 상한은 이 비율을 쓰지 않는다 — 살 두께·
	 * Max Contact Depth까지 그대로 그리고 그 너머에서 휜다(셰이더 SB_PressLimit, 관통 계약, 일지 295).
	 */
	inline constexpr float LimitEaseStart = 0.7f;

	/** 살의 무름이 구워진 기준 중력 크기 (cm/s²). 흔들림 값의 단위 "1 g"이기도 하다. */
	constexpr float StandardGravity = 980.f;

	/**
	 * 출렁이는 빠르기(Hz)는 흔들림 크기에서 나온다(ADR-0015 §2.5). 1 g에 Amount cm 처지는 스프링의 고유 진동수
	 * ω = √(g / Amount) — 크고 무르게 움직이는 살은 느리게, 조금만 움직이는 살은 빠르게 출렁인다. 크기와 빠르기를 따로 두면
	 * 충격(급정지)에 대한 반응이 빠르기에 비례해 커져 물리와 반대로 가므로 둘을 한 손잡이로 묶었다. [0.1, 30] Hz로 자른다.
	 */
	SOFTBODY_API float FrequencyFromAmount(float Amount);

	/**
	 * 묶음 하나가 출렁이는 빠르기(Hz, ADR-0015 §2.5). 1 g에 SagCm만큼 처지는 스프링의 고유 진동수에 Speed(Jiggle Speed)를
	 * 곱한다. SagCm = Inertia Amount × 그 묶음의 처짐 몫(USoftbodyMeshData::InertiaGroupSag)이라, 같은 Amount에서 단단한 부위가 더 빨리
	 * 출렁인다. [0.1, 30] Hz로 자른다. Speed 1이면 FrequencyFromAmount(SagCm)와 같다.
	 */
	SOFTBODY_API float WobbleFrequency(float SagCm, float Speed);

	/**
	 * 구동점(본 또는 메시) 하나의 흔들림 평형 (ADR-0005).
	 * 살이 받는 힘 = 중력 − 가속. 기본 자세에서 받던 중력(기준 축 −Z, 1 g)은 이미 모델링된
	 * 모양이므로 뺀다(GravityScale이 1보다 크면 (GravityScale − 1) g는 남아 서 있어도 더 처진다, 1 아래는 기울기 몫만 줄어든다).
	 * 결과는 기본 자세 축의 g 단위다.
	 * @param Acceleration         구동점의 월드 가속 (cm/s²)
	 * @param AngularAcceleration  구동점의 월드 회전 가속 (rad/s²)
	 * @param WorldGravity         월드 중력 (cm/s²)
	 * @param ReferenceToWorld     기본 자세 축 → 월드 회전
	 * @param OutLinear            정점과 무관한 몫 (g)
	 * @param OutAngular           회전 몫 (1/cm). 정점에서 OutLinear + OutAngular × (정점 − 구동점).
	 */
	SOFTBODY_API void ComputeLoadEquilibrium(
		const FVector& Acceleration, const FVector& AngularAcceleration,
		const FVector& WorldGravity, float GravityScale, const FQuat& ReferenceToWorld,
		FVector& OutLinear, FVector& OutAngular);

	/**
	 * 구동점 하중의 크기(g): |Load[0..2]| + Reach × |Load[3..5]| (회전 몫은 가장 먼 정점 거리 Reach를 곱해 g가 된다).
	 * 무름은 최대 1로 정규화되므로, 이 값 × 흔들림 크기(cm/g)는 어느 정점에서나 흔들림 거리(cm)의 상한이다.
	 */
	SOFTBODY_API float InertiaLoadMagnitude(const float* Load, float Reach);

	/**
	 * 흔들림 상한(안전장치). InertiaLoadMagnitude × Amount(cm)에 소프트 상한(SoftLimit)을 적용하고 여섯 값을 같은 비율로
	 * 줄인다. 진동자 상태가 아니라 올리는 값에만 쓴다(ADR-0015 §2.4) — 상태를 깎으면 매 틱 겹쳐 적용돼 벽에 부딪힌 듯
	 * 멈췄다 튀고 프레임 레이트에 따라 달라졌다. 상태는 선형이라 입력 × 공진 이득 이상 커지지 않는다.
	 * MaxOffset 또는 Amount가 0 이하면 전부 0.
	 */
	SOFTBODY_API void LimitInertiaLoad(
		float* Load, float Reach, float Amount, float MaxOffset, float SoftLimitStart = 1.f);

	/**
	 * 큰 움직임에서 조여지기(ADR-0015). 단단함 F(1/g)에서 하중 크기 m(g)이 1/F에 이르면 절반으로, 아무리 커도 1/F를
	 * 넘지 않게 여섯 값을 같은 비율 1 / (1 + m F)로 줄인다. 작은 움직임은 거의 그대로다. F가 클수록 세게 조이고 0 근처에서
	 * 끊기지 않는다(옛 포화 S = 1/F는 0 → 0.01에서 98 % 꺼졌다, 일지 297).
	 * 진동자 상태가 아니라 올리는 값에만 적용하므로 흔들리는 리듬은 바뀌지 않는다. F 0 이하면 아무것도 하지 않는다.
	 */
	SOFTBODY_API void SaturateInertiaLoad(float* Load, float Reach, float Firmness);
}
