// Copyright (c) 2026 wjsekrud

#include "SoftbodyInertiaAnalysis.h"

#include "Engine/Texture2D.h"
#include "SoftbodyMeshData.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyInertiaAnalysis, Log, All);

namespace
{
	/**
	 * 무름이 표면에서 매끄러운지 잰다(반점 = 가까운 정점끼리 값이 튐, 역방향 = 당긴 쪽과 반대로 밀림).
	 * 이웃 = 에지 길이 중앙값의 2배 안의 정점. 결과는 가장 무른 점(1) 대비 비율이다.
	 */
	void LogSoftnessSmoothness(const USoftbodyMeshData* Data, int32 NumVerts)
	{
		const float Radius = FMath::Max(2.f * Data->MedianEdgeLength, 1e-3f);
		const TArray<FVector3f>& Shapes = Data->VertexInertiaResponseShapes;
		auto CellOf = [Radius](const FVector3f& P)
		{
			return FIntVector(FMath::FloorToInt(P.X / Radius), FMath::FloorToInt(P.Y / Radius),
				FMath::FloorToInt(P.Z / Radius));
		};
		TMultiMap<FIntVector, int32> Grid;
		for (int32 v = 0; v < NumVerts; ++v)
		{
			Grid.Add(CellOf(Data->VertexPositions[v]), v);
		}
		TArray<float> Jumps;
		Jumps.Reserve(NumVerts);
		TArray<int32> Neighbors;
		for (int32 v = 0; v < NumVerts; ++v)
		{
			const FVector3f P = Data->VertexPositions[v];
			const FIntVector C = CellOf(P);
			float Jump = 0.f;
			for (int32 dz = -1; dz <= 1; ++dz)
			for (int32 dy = -1; dy <= 1; ++dy)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				Neighbors.Reset();
				Grid.MultiFind(C + FIntVector(dx, dy, dz), Neighbors);
				for (const int32 w : Neighbors)
				{
					if (w == v || FVector3f::DistSquared(P, Data->VertexPositions[w]) > Radius * Radius)
					{
						continue;
					}
					float Diff = 0.f;
					for (int32 k = 0; k < 3; ++k)
					{
						Diff += (Shapes[k * NumVerts + v] - Shapes[k * NumVerts + w]).SizeSquared();
					}
					Jump = FMath::Max(Jump, FMath::Sqrt(Diff));
				}
			}
			Jumps.Add(Jump);
		}
		Jumps.Sort();
		auto Percentile = [&Jumps](float Q)
		{
			return Jumps.Num() > 0 ? Jumps[FMath::Clamp((int32)(Q * (Jumps.Num() - 1)), 0, Jumps.Num() - 1)] : 0.f;
		};

		// 역방향 면적 비율: 임베딩 상태별 (1 실패, 2 외삽, 3 포함)
		double Area[4] = {};
		double Against[4] = {};
		for (int32 v = 0; v < NumVerts; ++v)
		{
			const int32 Status = Data->VertexEmbeddingStatus.IsValidIndex(v)
				? FMath::Clamp((int32)Data->VertexEmbeddingStatus[v], 0, 3) : 0;
			const double A = FMath::Max((double)Data->VertexAreas[v], 0.0);
			for (int32 k = 0; k < 3; ++k)
			{
				Area[Status] += A;
				Against[Status] += Shapes[k * NumVerts + v][k] < -0.02f ? A : 0.0;
			}
		}
		auto Ratio = [&](int32 s) { return Area[s] > 0.0 ? 100.0 * Against[s] / Area[s] : 0.0; };
		UE_LOG(LogSoftbodyInertiaAnalysis, Display,
			TEXT("InertiaResponse %s smoothness: neighbor (within %.2f) difference median %.3f / 95%% %.3f / max %.3f, "
				"backward-moving area: inside %.1f%% / extrapolated %.1f%% / failed %.1f%%"),
			*Data->GetName(), Radius, Percentile(0.5f), Percentile(0.95f), Percentile(1.f),
			Ratio(3), Ratio(2), Ratio(1));
	}
}

int32 VerifySoftbodyInertiaResponseBake(const USoftbodyMeshData* Data)
{
	if (!Data)
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse: no data asset"));
		return -1;
	}

	int32 Failed = 0;
	constexpr int32 ExpectedResponses = 3;
	const int32 NumVerts = Data->VertexPositions.Num();
	if (Data->InertiaResponseVersion != 2
		|| Data->NumInertiaResponses != ExpectedResponses
		|| !Data->InertiaResponseTex
		|| Data->InertiaResponseScales.Num() != ExpectedResponses
		|| NumVerts <= 0
		|| Data->VertexAreas.Num() != NumVerts
		|| Data->VertexInertiaResponseShapes.Num() != ExpectedResponses * NumVerts)
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: version=%d responses=%d tex=%s "
				"scales=%d verts=%d areas=%d shapes=%d/%d"),
			*Data->GetName(), Data->InertiaResponseVersion,
			Data->NumInertiaResponses,
			Data->InertiaResponseTex ? TEXT("yes") : TEXT("no"),
			Data->InertiaResponseScales.Num(),
			NumVerts, Data->VertexAreas.Num(),
			Data->VertexInertiaResponseShapes.Num(),
			ExpectedResponses * NumVerts);
		return Failed + 1;
	}

	// 커맨드릿에서 막 생성한 텍스처는 플랫폼 리소스가 아직 없어 GetSizeX/Y가 0이다.
	// 베이크 산출물 계약은 직렬화 원본인 TextureSource 크기로 검사한다.
	if (Data->InertiaResponseTex->Source.GetSizeX() != Data->TextureSize
		|| Data->InertiaResponseTex->Source.GetSizeY()
			!= Data->TextureSize * Data->GetInertiaResponseRows())
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: texture size %dx%d, expected %dx%d"),
			*Data->GetName(),
			Data->InertiaResponseTex->Source.GetSizeX(),
			Data->InertiaResponseTex->Source.GetSizeY(),
			Data->TextureSize, Data->TextureSize * Data->GetInertiaResponseRows());
		++Failed;
	}
	if (Data->InertiaAnchorNodeCount < 4)
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: anchor nodes %d < 4"),
			*Data->GetName(), Data->InertiaAnchorNodeCount);
		++Failed;
	}
	if (!FMath::IsFinite(Data->InertiaMaxAnchorError)
		|| Data->InertiaMaxAnchorError > 1e-6f)
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: anchor error %.3e"),
			*Data->GetName(), Data->InertiaMaxAnchorError);
		++Failed;
	}
	if (!FMath::IsFinite(Data->InertiaMaxRelativeResidual)
		|| Data->InertiaMaxRelativeResidual > 1e-4f)
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: relative residual %.3e"),
			*Data->GetName(), Data->InertiaMaxRelativeResidual);
		++Failed;
	}
	if (!FMath::IsFinite(Data->InertiaAnchorReach) || Data->InertiaAnchorReach <= 0.f)
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: anchor reach %g"),
			*Data->GetName(), Data->InertiaAnchorReach);
		++Failed;
	}
	if (Data->InertiaGroupBones.Num() > 0
		&& Data->InertiaGroupReach.Num() != Data->InertiaGroupBones.Num())
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: %d group reach values for %d groups"),
			*Data->GetName(), Data->InertiaGroupReach.Num(), Data->InertiaGroupBones.Num());
		++Failed;
	}

	// 세 방향은 하나의 값으로 함께 정규화된다 → 전체 최대 길이 = 1.
	// 살은 당기는 방향으로 밀려야 한다 → 면적 가중 평균의 그 방향 성분이 양수.
	float SharedMax = 0.f;
	for (int32 Response = 0; Response < ExpectedResponses; ++Response)
	{
		const float Scale = Data->InertiaResponseScales[Response];
		bool bFinite = FMath::IsFinite(Scale);
		double Along = 0.0;
		double Area = 0.0;
		float MaxAmplitude = 0.f;
		for (int32 v = 0; v < NumVerts; ++v)
		{
			const FVector3f& Value =
				Data->VertexInertiaResponseShapes[Response * NumVerts + v];
			bFinite &= FMath::IsFinite(Value.X)
				&& FMath::IsFinite(Value.Y)
				&& FMath::IsFinite(Value.Z);
			MaxAmplitude = FMath::Max(MaxAmplitude, Value.Size());
			const double VertexArea = FMath::Max((double)Data->VertexAreas[v], 0.0);
			Along += VertexArea * Value[Response];
			Area += VertexArea;
		}
		SharedMax = FMath::Max(SharedMax, MaxAmplitude);
		const double MeanAlong = Area > 0.0 ? Along / Area : 0.0;
		if (!bFinite || !(Scale > 0.f) || !(MeanAlong > 0.0))
		{
			UE_LOG(LogSoftbodyInertiaAnalysis, Error,
				TEXT("VerifyInertiaResponse failed %s direction %d: scale=%g finite=%d mean along pull=%.4f"),
				*Data->GetName(), Response, Scale, bFinite ? 1 : 0, MeanAlong);
			++Failed;
			continue;
		}
		UE_LOG(LogSoftbodyInertiaAnalysis, Display,
			TEXT("InertiaResponse %s direction %d: raw max=%g normalized max=%.4f mean along pull=%.4f"),
			*Data->GetName(), Response, Scale, MaxAmplitude, MeanAlong);
	}
	if (Data->VertexPositions.Num() == NumVerts && Data->VertexAreas.Num() == NumVerts)
	{
		LogSoftnessSmoothness(Data, NumVerts);
	}
	if (!FMath::IsNearlyEqual(SharedMax, 1.f, 1e-3f))
	{
		UE_LOG(LogSoftbodyInertiaAnalysis, Error,
			TEXT("VerifyInertiaResponse failed %s: shared max over three directions %.5f != 1"),
			*Data->GetName(), SharedMax);
		++Failed;
	}

	UE_LOG(LogSoftbodyInertiaAnalysis, Display,
		TEXT("VerifyInertiaResponse %s: %s (anchorNodes=%d residual=%.2e anchorError=%.2e reach=%.1f)"),
		*Data->GetName(), Failed == 0 ? TEXT("passed") : TEXT("failed"),
		Data->InertiaAnchorNodeCount,
		Data->InertiaMaxRelativeResidual,
		Data->InertiaMaxAnchorError,
		Data->InertiaAnchorReach);
	return Failed;
}
