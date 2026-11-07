// Copyright (c) 2026 wjsekrud

#if WITH_DEV_AUTOMATION_TESTS

#include "SoftbodyInertiaRuntime.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoftbodyInertiaRuntimeContractTest,
	"Softbody.Inertia.RuntimeLoadContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoftbodyInertiaRuntimeContractTest::RunTest(const FString& Parameters)
{
	using namespace SoftbodyInertiaRuntime;

	float Q = 0.f;
	float V = 0.f;
	for (int32 i = 0; i < 120; ++i)
	{
		AdvanceResponse(Q, V, 0.f, 2.5f, 0.25f, 1.f / 60.f);
	}
	TestEqual(TEXT("Zero input cannot create inertia from rest"), Q, 0.f);
	TestEqual(TEXT("Zero input cannot create inertia velocity from rest"), V, 0.f);

	Q = 10.f;
	V = 0.f;
	const float InitialEnergyProxy = Q * Q;
	for (int32 i = 0; i < 360; ++i)
	{
		AdvanceResponse(Q, V, 0.f, 2.5f, 0.25f, 1.f / 60.f);
	}
	TestTrue(TEXT("Response returns to the zero equilibrium"),
		Q * Q + V * V < InitialEnergyProxy * 1e-4f);

	float Q30 = 0.f;
	float V30 = 0.f;
	float Q120 = 0.f;
	float V120 = 0.f;
	for (int32 i = 0; i < 30; ++i)
	{
		AdvanceResponse(Q30, V30, 4.f, 2.5f, 0.25f, 1.f / 30.f);
	}
	for (int32 i = 0; i < 120; ++i)
	{
		AdvanceResponse(Q120, V120, 4.f, 2.5f, 0.25f, 1.f / 120.f);
	}
	TestTrue(TEXT("Analytic response is frame-rate invariant"),
		FMath::IsNearlyEqual(Q30, Q120, 1e-4f)
		&& FMath::IsNearlyEqual(V30, V120, 1e-4f));

	const FQuat R0 = FQuat::Identity;
	const FQuat R1(FVector::UpVector, PI * 0.5f);
	const FVector AngularVelocity = ComputeAngularVelocity(R0, R1, 0.5f);
	TestTrue(TEXT("Angular velocity uses the shortest world-space rotation"),
		AngularVelocity.Equals(FVector(0.f, 0.f, PI), 1e-4f));

	const FQuat ReferenceRotation(FVector::RightVector, PI * 0.35f);
	const FQuat CurrentRotation =
		FQuat(FVector::UpVector, -PI * 0.2f) * ReferenceRotation;
	const FVector WorldVector(13.f, -7.f, 4.f);
	const FVector ReferenceVector = ToReferenceAxes(
		WorldVector, CurrentRotation, ReferenceRotation);
	const FVector RoundTrip = ReferenceToWorldRotation(
		CurrentRotation, ReferenceRotation).RotateVector(ReferenceVector);
	TestTrue(TEXT("Reference-pose response transport preserves direction"),
		RoundTrip.Equals(WorldVector, 1e-4f));

	// 흔들림 평형 (ADR-0005): 서 있고 가만하면 0, 가속하면 반대로 1 g당 1, 옆으로 누우면 그쪽으로 처짐.
	const FVector Gravity(0.f, 0.f, -StandardGravity);
	FVector Linear;
	FVector Angular;
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector::ZeroVector, Gravity, 1.f,
		FQuat::Identity, Linear, Angular);
	TestTrue(TEXT("Upright rest pose has no load"),
		Linear.IsNearlyZero(1e-5f) && Angular.IsNearlyZero(1e-8f));
	ComputeLoadEquilibrium(FVector(StandardGravity, 0.f, 0.f), FVector::ZeroVector, Gravity, 1.f,
		FQuat::Identity, Linear, Angular);
	TestTrue(TEXT("Flesh lags opposite to a 1 g acceleration"),
		Linear.Equals(FVector(-1.f, 0.f, 0.f), 1e-5f));
	// 기준 +Y가 월드 +Z로 가도록 X축으로 90° 눕힘: 월드 아래(−Z)는 기준 −Y.
	const FQuat LyingDown(FVector::ForwardVector, PI * 0.5f);
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector::ZeroVector, Gravity, 1.f,
		LyingDown, Linear, Angular);
	TestTrue(TEXT("Lying down sags toward the new down and releases the old sag"),
		Linear.Equals(FVector(0.f, -1.f, 1.f), 1e-4f));
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector::ZeroVector, Gravity, 0.f,
		LyingDown, Linear, Angular);
	TestTrue(TEXT("Gravity scale 0 ignores orientation"), Linear.IsNearlyZero(1e-5f));
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector::ZeroVector, Gravity, 2.f,
		FQuat::Identity, Linear, Angular);
	TestTrue(TEXT("Gravity scale 2 sags one more g while upright"), Linear.Equals(FVector(0.f, 0.f, -1.f), 1e-5f));
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector::ZeroVector, Gravity, 2.f,
		LyingDown, Linear, Angular);
	TestTrue(TEXT("Gravity scale 2 doubles the lying-down sag"), Linear.Equals(FVector(0.f, -2.f, 1.f), 1e-4f));
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector::ZeroVector, Gravity, 0.5f,
		LyingDown, Linear, Angular);
	TestTrue(TEXT("Gravity scale below 1 only scales the tilt"), Linear.Equals(FVector(0.f, -0.5f, 0.5f), 1e-4f));
	ComputeLoadEquilibrium(FVector::ZeroVector, FVector(0.f, 0.f, 98.f), Gravity, 0.f,
		FQuat::Identity, Linear, Angular);
	TestTrue(TEXT("Angular acceleration yields a per-distance load"),
		Angular.Equals(FVector(0.f, 0.f, -0.1f), 1e-6f));

	float Load[6] = { 4.f, -3.f, 2.f, 0.1f, 0.f, 0.f };
	LimitInertiaLoad(Load, 20.f, 2.f, 5.f);
	const float Bound = 2.f * InertiaLoadMagnitude(Load, 20.f);
	TestTrue(TEXT("Load bound limits the lag at every vertex"), Bound <= 5.0001f);
	float Zeroed[6] = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };
	LimitInertiaLoad(Zeroed, 20.f, 2.f, 0.f);
	TestTrue(TEXT("A zero limit silences the output"), Zeroed[0] == 0.f && Zeroed[2] == 0.f);

	// 하중 크기는 벡터 크기(절댓값 합이 아님): (3, 4, 0) g + 도달 20 cm × 0.05/cm = 5 + 1.
	const float Mixed[6] = { 3.f, 4.f, 0.f, 0.f, 0.03f, 0.04f };
	TestTrue(TEXT("Load magnitude is the vector norm plus reach times the angular norm"),
		FMath::IsNearlyEqual(InertiaLoadMagnitude(Mixed, 20.f), 6.f, 1e-5f));

	// 큰 움직임 조이기(ADR-0015): 단단함 F에서 하중 = 1/F면 절반, 아주 크면 1/F에 붙고, 0이면 그대로(일지 297).
	float Sat[6] = { 2.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	SaturateInertiaLoad(Sat, 0.f, 0.5f);
	TestTrue(TEXT("A load of one over the firmness is halved"), FMath::IsNearlyEqual(Sat[0], 1.f, 1e-5f));
	float Big[6] = { 0.f, 0.f, 200.f, 0.f, 0.f, 0.f };
	SaturateInertiaLoad(Big, 0.f, 0.5f);
	TestTrue(TEXT("A huge load approaches one over the firmness without crossing it"),
		Big[2] < 2.f && Big[2] > 1.9f);
	float Off[6] = { 0.5f, -0.25f, 0.f, 0.01f, 0.f, 0.f };
	SaturateInertiaLoad(Off, 20.f, 0.f);
	TestTrue(TEXT("Firmness 0 leaves the load untouched"), Off[0] == 0.5f && Off[1] == -0.25f && Off[3] == 0.01f);
	float Small[6] = { 0.1f, 0.f, 0.f, 0.f, 0.f, 0.f };
	SaturateInertiaLoad(Small, 0.f, 0.5f);
	TestTrue(TEXT("Small loads are nearly untouched"), Small[0] > 0.095f && Small[0] < 0.1f);
	// 옛 포화 꼴은 0 → 0.01에서 98 % 꺼졌다. 단단함은 0 근처에서 이어지고 올릴수록 세게 조인다.
	float Tiny[6] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	SaturateInertiaLoad(Tiny, 0.f, 0.01f);
	TestTrue(TEXT("A tiny firmness barely changes the load"), Tiny[0] > 0.98f);
	float Soft[6] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	float Firm[6] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	SaturateInertiaLoad(Soft, 0.f, 1.f);
	SaturateInertiaLoad(Firm, 0.f, 5.f);
	TestTrue(TEXT("Higher firmness holds the wobble in more"), Firm[0] < Soft[0]);

	// 빠르기는 크기에서(ADR-0015 §2.5): ω = √(g/Amount). 2.5 cm → 3.15 Hz, 10 cm → 1.58 Hz, 네 배 크면 반으로 느리다.
	TestTrue(TEXT("2.5 cm per g swings at about 3.15 Hz"),
		FMath::IsNearlyEqual(FrequencyFromAmount(2.5f), 3.151f, 0.01f));
	TestTrue(TEXT("Four times the amount halves the frequency"),
		FMath::IsNearlyEqual(FrequencyFromAmount(10.f), 0.5f * FrequencyFromAmount(2.5f), 1e-4f));
	TestTrue(TEXT("Zero amount clamps to the fastest rate instead of dividing by zero"),
		FrequencyFromAmount(0.f) == 30.f && FrequencyFromAmount(1e6f) == 0.1f);

	// 묶음 빠르기 = √(g ÷ (Amount × 처짐 몫)) × Jiggle Speed. 몫 1·빠르기 1이면 옛 식과 같다.
	TestTrue(TEXT("Sag 1 and speed 1 match the amount-only frequency"),
		FMath::IsNearlyEqual(WobbleFrequency(10.f, 1.f), FrequencyFromAmount(10.f), 1e-5f));
	TestTrue(TEXT("A part that sags a quarter as far wobbles twice as fast"),
		FMath::IsNearlyEqual(WobbleFrequency(2.5f, 1.f), 2.f * WobbleFrequency(10.f, 1.f), 1e-4f));
	TestTrue(TEXT("Jiggle Speed scales the frequency"),
		FMath::IsNearlyEqual(WobbleFrequency(10.f, 2.f), 2.f * WobbleFrequency(10.f, 1.f), 1e-4f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoftbodySoftLimitContractTest,
	"Softbody.Limits.SoftLimitContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoftbodySoftLimitContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const float Limit = 10.f;
	const float Ratio = 0.7f;
	const float SoftStart = Ratio * Limit;

	TestEqual(TEXT("Below the knee the function is the identity"),
		SoftbodyInertiaRuntime::SoftLimit(3.f, Limit, Ratio), 3.f);
	TestEqual(TEXT("At the knee the function is the identity"),
		SoftbodyInertiaRuntime::SoftLimit(SoftStart, Limit, Ratio), SoftStart);
	TestTrue(TEXT("The finite input 2L-K reaches the limit exactly"),
		FMath::IsNearlyEqual(SoftbodyInertiaRuntime::SoftLimit(2.f * Limit - SoftStart, Limit, Ratio), Limit, 1e-5f));
	TestEqual(TEXT("Beyond 2L-K the function stays at the limit"),
		SoftbodyInertiaRuntime::SoftLimit(100.f, Limit, Ratio), Limit);
	TestEqual(TEXT("Non-positive limits collapse to zero"),
		SoftbodyInertiaRuntime::SoftLimit(5.f, 0.f, Ratio), 0.f);
	TestEqual(TEXT("A knee ratio of one is a hard clamp"),
		SoftbodyInertiaRuntime::SoftLimit(12.f, Limit, 1.f), Limit);
	TestEqual(TEXT("A knee ratio of one keeps values under the limit"),
		SoftbodyInertiaRuntime::SoftLimit(4.f, Limit, 1.f), 4.f);

	bool bMonotonic = true;
	bool bBounded = true;
	float Previous = SoftbodyInertiaRuntime::SoftLimit(0.f, Limit, Ratio);
	for (int32 Step = 1; Step <= 400; ++Step)
	{
		const float Value = SoftbodyInertiaRuntime::SoftLimit(0.05f * (float)Step, Limit, Ratio);
		bMonotonic &= Value >= Previous - 1e-6f;
		bBounded &= Value <= Limit + 1e-6f && Value <= 0.05f * (float)Step + 1e-6f;
		Previous = Value;
	}
	TestTrue(TEXT("The knee is monotonic"), bMonotonic);
	TestTrue(TEXT("The knee never exceeds the limit or the input"), bBounded);

	const float Epsilon = 1e-3f;
	const float SlopeBelow =
		(SoftbodyInertiaRuntime::SoftLimit(SoftStart, Limit, Ratio) - SoftbodyInertiaRuntime::SoftLimit(SoftStart - Epsilon, Limit, Ratio)) / Epsilon;
	const float SlopeAbove =
		(SoftbodyInertiaRuntime::SoftLimit(SoftStart + Epsilon, Limit, Ratio) - SoftbodyInertiaRuntime::SoftLimit(SoftStart, Limit, Ratio)) / Epsilon;
	TestTrue(TEXT("The knee is C1 at its start (slope 1 on both sides)"),
		FMath::IsNearlyEqual(SlopeBelow, 1.f, 1e-2f)
		&& FMath::IsNearlyEqual(SlopeAbove, 1.f, 1e-2f));
	const float End = 2.f * Limit - SoftStart;
	const float SlopeEnd =
		(SoftbodyInertiaRuntime::SoftLimit(End, Limit, Ratio) - SoftbodyInertiaRuntime::SoftLimit(End - Epsilon, Limit, Ratio)) / Epsilon;
	TestTrue(TEXT("The knee is C1 at the limit (slope 0)"),
		FMath::Abs(SlopeEnd) < 1e-2f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoftbodyLimitsResponseSoftLimitTest,
	"Softbody.Limits.InertiaLoadKnee",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoftbodyLimitsResponseSoftLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const float MaxOffset = 10.f;
	const float Ratio = 0.7f;
	// 흔들림 양 2 cm/g, 회전 몫은 도달 거리 20 cm를 곱해 같은 합에 들어간다.
	const float Amount = 2.f;
	const float Reach = 20.f;
	auto Bound = [Amount, Reach](const float* L)
	{
		return Amount * SoftbodyInertiaRuntime::InertiaLoadMagnitude(L, Reach);
	};

	float Small[6] = {1.5f, -1.f, 0.f, 0.025f, 0.f, 0.f};
	SoftbodyInertiaRuntime::LimitInertiaLoad(Small, Reach, Amount, MaxOffset, Ratio);
	TestTrue(TEXT("Bounds below the knee are untouched"),
		Small[0] == 1.5f && Small[1] == -1.f && Small[3] == 0.025f);

	// 2L − K = 13 → 정확히 상한: |(6, −2.5, 0)| = 6.5 g × 2 = 13.
	float Edge[6] = {6.f, -2.5f, 0.f, 0.f, 0.f, 0.f};
	SoftbodyInertiaRuntime::LimitInertiaLoad(Edge, Reach, Amount, MaxOffset, Ratio);
	TestTrue(TEXT("A bound of 2L-K lands exactly on the limit"),
		FMath::IsNearlyEqual(Bound(Edge), MaxOffset, 1e-4f));
	TestTrue(TEXT("Direction is preserved"),
		FMath::IsNearlyEqual(Edge[1] / Edge[0], -2.5f / 6.f, 1e-5f) && Edge[1] < 0.f);

	float Huge[6] = {100.f, 50.f, 25.f, 1.f, 0.f, 0.f};
	SoftbodyInertiaRuntime::LimitInertiaLoad(Huge, Reach, Amount, MaxOffset, Ratio);
	TestTrue(TEXT("Large bounds saturate at the limit"),
		FMath::IsNearlyEqual(Bound(Huge), MaxOffset, 1e-4f));

	// 쥔 덩어리의 모드 적분(ADR-0012 §8): 출렁이는 쪽은 AdvanceResponse와 같고, 감쇠가 진동수를 넘어도 넘치지 않고 이어진다.
	{
		const double W0 = 2.0 * PI * 2.5;
		float Qf = 3.f;
		float Vf = -1.f;
		double Qd = 3.0;
		double Vd = -1.0;
		for (int32 i = 0; i < 30; ++i)
		{
			SoftbodyInertiaRuntime::AdvanceResponse(Qf, Vf, 0.5f, 2.5f, 0.25f, 1.f / 60.f);
			SoftbodyInertiaRuntime::AdvanceDampedMode(Qd, Vd, 0.5, W0, 0.25 * W0, 1.0 / 60.0);
		}
		TestTrue(TEXT("Damped mode matches the underdamped response"),
			FMath::IsNearlyEqual((double)Qf, Qd, 1e-3) && FMath::IsNearlyEqual((double)Vf, Vd, 1e-2));
		double Below = 1.0;
		double BelowV = 0.0;
		double Above = 1.0;
		double AboveV = 0.0;
		SoftbodyInertiaRuntime::AdvanceDampedMode(Below, BelowV, 0.0, W0, W0 * (1.0 - 1e-7), 0.05);
		SoftbodyInertiaRuntime::AdvanceDampedMode(Above, AboveV, 0.0, W0, W0 * (1.0 + 1e-7), 0.05);
		TestTrue(TEXT("Damped mode is continuous across critical damping"),
			FMath::IsNearlyEqual(Below, Above, 1e-6) && FMath::IsNearlyEqual(BelowV, AboveV, 1e-4));
		double Heavy = 1.0;
		double HeavyV = 0.0;
		bool bMonotone = true;
		for (int32 i = 0; i < 120; ++i)
		{
			const double Before = Heavy;
			SoftbodyInertiaRuntime::AdvanceDampedMode(Heavy, HeavyV, 0.0, W0, 50.0 * W0, 1.0 / 60.0);
			bMonotone &= Heavy <= Before && Heavy >= 0.0 && FMath::IsFinite(Heavy);
		}
		TestTrue(TEXT("Heavily damped mode settles without overshoot"), bMonotone && Heavy < 1.0);

		const double M[3][3] = {{4.0, 1.0, 0.5}, {1.0, 3.0, 0.2}, {0.5, 0.2, 2.0}};
		double Values[3];
		double Vectors[3][3];
		SoftbodyInertiaRuntime::SymmetricEigen3(M, Values, Vectors);
		double Worst = 0.0;
		for (int32 i = 0; i < 3; ++i)
		{
			for (int32 Row = 0; Row < 3; ++Row)
			{
				double MV = 0.0;
				for (int32 Col = 0; Col < 3; ++Col)
				{
					MV += M[Row][Col] * Vectors[Col][i];
				}
				Worst = FMath::Max(Worst, FMath::Abs(MV - Values[i] * Vectors[Row][i]));
			}
		}
		TestTrue(TEXT("Symmetric eigen decomposition satisfies M v = λ v"), Worst < 1e-9);
	}

	// 4 g × 2 = 8: 무릎(7)과 상한(10) 사이 → 7보다 크고 8보다 작게 눌린다.
	float Mid[6] = {4.f, 0.f, 0.f, 0.f, 0.f, 0.f};
	SoftbodyInertiaRuntime::LimitInertiaLoad(Mid, Reach, Amount, MaxOffset, Ratio);
	TestTrue(TEXT("Bounds inside the knee are compressed smoothly below the input"),
		Bound(Mid) < 8.f && Bound(Mid) > 7.f);
	return true;
}

#endif
