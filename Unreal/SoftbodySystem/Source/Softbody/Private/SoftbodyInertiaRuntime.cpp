// Copyright (c) 2026 wjsekrud

#include "SoftbodyInertiaRuntime.h"

namespace SoftbodyInertiaRuntime
{
	void AdvanceDampedMode(double& X, double& XDot, double XEq, double Omega, double Decay, double DeltaTime)
	{
		if (!(DeltaTime > 0.0))
		{
			return;
		}
		const double T = DeltaTime;
		const double P = X - XEq;
		const double V = XDot;
		const double W2 = Omega * Omega;
		// b² = ω² − a²: 양수면 출렁이며(cos·sin), 음수면 출렁이지 않고(cosh·sinh) 가라앉는다. 0 근처는 급수로 잇는다.
		const double B2 = W2 - Decay * Decay;
		double C;  // e^{−aT}·cos(bT) (또는 cosh)
		double SB; // e^{−aT}·sin(bT)/b (또는 sinh(bT)/b)
		if (FMath::Abs(B2) * T * T < 1e-6)
		{
			const double E = FMath::Exp(-Decay * T);
			C = E * (1.0 - 0.5 * B2 * T * T);
			SB = E * T * (1.0 - B2 * T * T / 6.0);
		}
		else if (B2 > 0.0)
		{
			const double B = FMath::Sqrt(B2);
			const double E = FMath::Exp(-Decay * T);
			C = E * FMath::Cos(B * T);
			SB = E * FMath::Sin(B * T) / B;
		}
		else
		{
			// e^{−aT}cosh(bT) = (e^{−(a−b)T} + e^{−(a+b)T})/2 — a가 아주 커도 넘치지 않는다.
			const double B = FMath::Sqrt(-B2);
			const double Slow = FMath::Exp(-(Decay - B) * T);
			const double Fast = FMath::Exp(-(Decay + B) * T);
			C = 0.5 * (Slow + Fast);
			SB = 0.5 * (Slow - Fast) / B;
		}
		X = XEq + P * C + (V + Decay * P) * SB;
		XDot = V * C - (W2 * P + Decay * V) * SB;
	}

	void SymmetricEigen3(const double In[3][3], double OutValues[3], double OutVectors[3][3])
	{
		double A[3][3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				A[Row][Col] = 0.5 * (In[Row][Col] + In[Col][Row]);
				OutVectors[Row][Col] = Row == Col ? 1.0 : 0.0;
			}
		}
		const int32 Pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
		for (int32 Sweep = 0; Sweep < 50; ++Sweep)
		{
			const double Off = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
			const double Diag = A[0][0] * A[0][0] + A[1][1] * A[1][1] + A[2][2] * A[2][2];
			if (Off <= 1e-30 * FMath::Max(Diag, 1e-300))
			{
				break;
			}
			for (const auto& Pair : Pairs)
			{
				const int32 P = Pair[0];
				const int32 Q = Pair[1];
				if (FMath::Abs(A[P][Q]) < 1e-300)
				{
					continue;
				}
				const double Theta = (A[Q][Q] - A[P][P]) / (2.0 * A[P][Q]);
				const double Tn = (Theta >= 0.0 ? 1.0 : -1.0) / (FMath::Abs(Theta) + FMath::Sqrt(Theta * Theta + 1.0));
				const double Cs = 1.0 / FMath::Sqrt(Tn * Tn + 1.0);
				const double Sn = Tn * Cs;
				for (int32 K = 0; K < 3; ++K)
				{
					const double Kp = A[K][P];
					const double Kq = A[K][Q];
					A[K][P] = Cs * Kp - Sn * Kq;
					A[K][Q] = Sn * Kp + Cs * Kq;
				}
				for (int32 K = 0; K < 3; ++K)
				{
					const double Pk = A[P][K];
					const double Qk = A[Q][K];
					A[P][K] = Cs * Pk - Sn * Qk;
					A[Q][K] = Sn * Pk + Cs * Qk;
				}
				for (int32 K = 0; K < 3; ++K)
				{
					const double Vp = OutVectors[K][P];
					const double Vq = OutVectors[K][Q];
					OutVectors[K][P] = Cs * Vp - Sn * Vq;
					OutVectors[K][Q] = Sn * Vp + Cs * Vq;
				}
			}
		}
		for (int32 K = 0; K < 3; ++K)
		{
			OutValues[K] = A[K][K];
		}
	}

	float SoftLimit(float X, float Limit, float SoftLimitStart)
	{
		if (!(Limit > 0.f) || !FMath::IsFinite(Limit))
		{
			return 0.f;
		}
		if (!(X > 0.f))
		{
			return 0.f;
		}
		if (!FMath::IsFinite(X))
		{
			return Limit;
		}
		const float SoftStart = FMath::Clamp(SoftLimitStart, 0.f, 1.f) * Limit;
		if (X <= SoftStart)
		{
			return X;
		}
		const float Span = Limit - SoftStart;
		if (Span <= UE_SMALL_NUMBER)
		{
			return Limit;
		}
		const float T = FMath::Min((X - SoftStart) / (2.f * Span), 1.f);
		return SoftStart + Span * (2.f * T - T * T);
	}

	float FrequencyFromAmount(float Amount)
	{
		if (!(Amount > 0.f) || !FMath::IsFinite(Amount))
		{
			return 30.f;
		}
		const float Omega = FMath::Sqrt(StandardGravity / Amount);
		return FMath::Clamp(Omega / (2.f * PI), 0.1f, 30.f);
	}

	float WobbleFrequency(float SagCm, float Speed)
	{
		if (!(SagCm > 0.f) || !FMath::IsFinite(SagCm))
		{
			return 30.f;
		}
		const float Omega = FMath::Sqrt(StandardGravity / SagCm) * FMath::Max(Speed, 0.f);
		return FMath::Clamp(Omega / (2.f * PI), 0.1f, 30.f);
	}

	void AdvanceResponse(
		float& Q, float& QDot, float QEq,
		float FrequencyHz, float DampingRatio, float DeltaTime)
	{
		if (DeltaTime <= UE_KINDA_SMALL_NUMBER)
		{
			return;
		}

		const float W0 = 2.f * PI * FMath::Max(FrequencyHz, 0.01f);
		const float Zeta = FMath::Clamp(DampingRatio, 0.f, 0.9999f);
		const float Wd = W0 * FMath::Sqrt(FMath::Max(1.f - Zeta * Zeta, 1e-6f));
		const float E = FMath::Exp(-Zeta * W0 * DeltaTime);
		const float C = FMath::Cos(Wd * DeltaTime);
		const float S = FMath::Sin(Wd * DeltaTime);
		const float P = Q - QEq;
		const float V = QDot;
		Q = QEq + E * (P * C + (V + Zeta * W0 * P) / Wd * S);
		QDot = E * (V * C - (W0 * W0 * P + Zeta * W0 * V) / Wd * S);

		if (FMath::Square(Q) < 1e-6f && FMath::Square(QDot) < 1e-6f
			&& FMath::IsNearlyZero(QEq, 1e-6f))
		{
			Q = 0.f;
			QDot = 0.f;
		}
	}

	FVector ComputeAngularVelocity(
		const FQuat& Previous, const FQuat& Current, float DeltaTime)
	{
		if (DeltaTime <= UE_KINDA_SMALL_NUMBER)
		{
			return FVector::ZeroVector;
		}

		FQuat Delta = Current.GetNormalized() * Previous.GetNormalized().Inverse();
		if (Delta.W < 0.f)
		{
			Delta.X = -Delta.X;
			Delta.Y = -Delta.Y;
			Delta.Z = -Delta.Z;
			Delta.W = -Delta.W;
		}
		Delta.Normalize();

		FVector Axis = FVector::ForwardVector;
		double Angle = 0.0;
		Delta.ToAxisAndAngle(Axis, Angle);
		if (!Axis.Normalize() || !FMath::IsFinite(Angle))
		{
			return FVector::ZeroVector;
		}
		return Axis * (Angle / DeltaTime);
	}

	FVector ToReferenceAxes(
		const FVector& WorldVector,
		const FQuat& CurrentWorldRotation,
		const FQuat& ReferenceComponentRotation)
	{
		return ReferenceComponentRotation.GetNormalized().RotateVector(
			CurrentWorldRotation.GetNormalized().UnrotateVector(WorldVector));
	}

	FQuat ReferenceToWorldRotation(
		const FQuat& CurrentWorldRotation,
		const FQuat& ReferenceComponentRotation)
	{
		return (CurrentWorldRotation.GetNormalized()
			* ReferenceComponentRotation.GetNormalized().Inverse()).GetNormalized();
	}

	void ComputeLoadEquilibrium(
		const FVector& Acceleration, const FVector& AngularAcceleration,
		const FVector& WorldGravity, float GravityScale, const FQuat& ReferenceToWorld,
		FVector& OutLinear, FVector& OutAngular)
	{
		const FQuat Rotation = ReferenceToWorld.GetNormalized();
		const FVector RestGravity(0.f, 0.f, -StandardGravity);
		const FVector Felt = Rotation.UnrotateVector(WorldGravity * GravityScale - Acceleration);
		// 서 있을 때의 1 g 처짐은 모델에 들어 있어 뺀다. 1 위의 몫은 서 있어도 (배율 − 1) g만큼 더 처지게 남기고, 1 아래는 기울기 몫만
		// 줄인다(0 = 중력 영향 없음, 살이 위로 뜨지 않는다 — 일지 343). 1에서 두 꼴이 이어진다.
		OutLinear = (Felt - RestGravity * FMath::Min(GravityScale, 1.f)) / StandardGravity;
		OutAngular = Rotation.UnrotateVector(-AngularAcceleration) / StandardGravity;
	}

	float InertiaLoadMagnitude(const float* Load, float Reach)
	{
		if (!Load)
		{
			return 0.f;
		}
		// 벡터 크기로 잰다. 절댓값 합은 최대 √3배 과대평가라 상한이 실제 정점 변위보다 먼저 걸렸다.
		const float Linear = FMath::Sqrt(Load[0] * Load[0] + Load[1] * Load[1] + Load[2] * Load[2]);
		const float Angular = FMath::Sqrt(Load[3] * Load[3] + Load[4] * Load[4] + Load[5] * Load[5]);
		return Linear + FMath::Max(Reach, 0.f) * Angular;
	}

	void LimitInertiaLoad(float* Load, float Reach, float Amount, float MaxOffset, float SoftLimitStart)
	{
		if (!Load)
		{
			return;
		}
		if (MaxOffset <= 0.f || Amount <= 0.f)
		{
			FMemory::Memzero(Load, 6 * sizeof(float));
			return;
		}

		const float Bound = Amount * InertiaLoadMagnitude(Load, Reach);
		const float Limited = SoftLimit(Bound, MaxOffset, SoftLimitStart);
		if (Bound > Limited && Bound > UE_SMALL_NUMBER)
		{
			const float Scale = Limited / Bound;
			for (int32 i = 0; i < 6; ++i)
			{
				Load[i] *= Scale;
			}
		}
	}

	void SaturateInertiaLoad(float* Load, float Reach, float Firmness)
	{
		if (!Load || !(Firmness > 0.f) || !FMath::IsFinite(Firmness))
		{
			return;
		}
		const float Magnitude = InertiaLoadMagnitude(Load, Reach);
		if (!(Magnitude > UE_SMALL_NUMBER) || !FMath::IsFinite(Magnitude))
		{
			return;
		}
		// m / (1 + m F): m ≪ 1/F면 m, m = 1/F면 절반, m → ∞면 1/F. 방향은 그대로다.
		const float Scale = 1.f / (1.f + Magnitude * Firmness);
		for (int32 i = 0; i < 6; ++i)
		{
			Load[i] *= Scale;
		}
	}
}
