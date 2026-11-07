// Copyright (c) 2026 wjsekrud

#include "SoftbodyLayerLinks.h"

#include "Async/ParallelFor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyLayers, Log, All);

using namespace UE::Geometry;

namespace SoftbodyLayers
{
namespace
{
	enum class EFollowKind : uint8
	{
		None,
		Cover,             // 바로 뒤 = 덮인 살
		CoverThroughSheet, // 제 판의 뒷면 너머 = 덮인 살
		Sheet,             // 제 판의 뒷면
		SamePoint,         // 같은 자리 정점
	};

	/** 따를 곳: 받침 삼각형 꼭짓점(같은 자리 정점이면 하나)과 무게, 따르는 세기(1 = 완전히, 나머지는 제 값). */
	struct FFollow
	{
		int32 Corner[3] = {INDEX_NONE, INDEX_NONE, INDEX_NONE};
		float Bary[3] = {0.f, 0.f, 0.f};
		float Strength = 0.f;
	};

	/** 원래 값 번호와 무게의 섞음. 작은 무게는 버리고 합을 1로 맞춘다. */
	using FMix = TArray<TPair<int32, float>, TInlineAllocator<8>>;
	constexpr int32 MaxMixSources = 8;
	constexpr float MinMixWeight = 1e-3f;

	void AddTo(FMix& Mix, int32 Source, float Weight)
	{
		for (TPair<int32, float>& Entry : Mix)
		{
			if (Entry.Key == Source)
			{
				Entry.Value += Weight;
				return;
			}
		}
		Mix.Emplace(Source, Weight);
	}

	void Normalize(FMix& Mix)
	{
		Mix.Sort([](const TPair<int32, float>& A, const TPair<int32, float>& B)
		{
			return A.Value != B.Value ? A.Value > B.Value : A.Key < B.Key;
		});
		while (Mix.Num() > MaxMixSources || (Mix.Num() > 1 && Mix.Last().Value < MinMixWeight))
		{
			Mix.Pop(EAllowShrinking::No);
		}
		float Sum = 0.f;
		for (const TPair<int32, float>& Entry : Mix)
		{
			Sum += Entry.Value;
		}
		for (TPair<int32, float>& Entry : Mix)
		{
			Entry.Value /= FMath::Max(Sum, 1e-12f);
		}
	}

	float MixDifference(const FMix& A, const FMix& B)
	{
		float Difference = 0.f;
		for (const TPair<int32, float>& Entry : A)
		{
			const TPair<int32, float>* Other = B.FindByPredicate([&Entry](const TPair<int32, float>& E) { return E.Key == Entry.Key; });
			Difference += FMath::Abs(Entry.Value - (Other ? Other->Value : 0.f));
		}
		for (const TPair<int32, float>& Entry : B)
		{
			if (!A.ContainsByPredicate([&Entry](const TPair<int32, float>& E) { return E.Key == Entry.Key; }))
			{
				Difference += Entry.Value;
			}
		}
		return Difference;
	}
}

void BuildLayerLinks(const FMeshDescription& MD, float MaxGap, const TSet<FName>& EligibleSlots,
	TArray<FSoftbodyLayerLink>& OutLinks)
{
	OutLinks.Reset();
	if (MaxGap <= 0.f)
	{
		return;
	}
	FStaticMeshConstAttributes Attributes(MD);
	const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const TPolygonGroupAttributesConstRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	// 정점: 텍셀 번호 = MeshDescription 정점 원소 순서
	TArray<int32> Compact;
	Compact.Init(INDEX_NONE, MD.Vertices().GetArraySize());
	TArray<FVector3d> P;
	P.Reserve(MD.Vertices().Num());
	for (const FVertexID Vertex : MD.Vertices().GetElementIDs())
	{
		Compact[Vertex] = P.Num();
		P.Add(FVector3d(Positions[Vertex]));
	}
	const int32 N = P.Num();

	// 삼각형: 꼭짓점, 넓이 크기 노멀, 따를 면이 될 수 있는가(슬롯)
	TArray<FIndex3i> Tris;
	TArray<FVector3d> TriNormal;
	TArray<uint8> TriEligible;
	TArray<FName> VertexSlot;
	VertexSlot.Init(NAME_None, N);
	int32 NumEligible = 0;
	for (const FTriangleID Triangle : MD.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(Triangle);
		const FIndex3i I(Compact[Corners[0]], Compact[Corners[1]], Compact[Corners[2]]);
		Tris.Add(I);
		TriNormal.Add((P[I.B] - P[I.A]).Cross(P[I.C] - P[I.A]));
		const FName Slot = SlotNames.IsValid() ? SlotNames[MD.GetTrianglePolygonGroup(Triangle)] : NAME_None;
		const bool bEligible = EligibleSlots.Num() == 0 || EligibleSlots.Contains(Slot);
		TriEligible.Add(bEligible ? 1 : 0);
		NumEligible += bEligible ? 1 : 0;
		for (int32 k = 0; k < 3; ++k)
		{
			VertexSlot[I[k]] = Slot;
		}
	}
	if (NumEligible == 0)
	{
		UE_LOG(LogSoftbodyLayers, Warning, TEXT("Layered clothing: no Softbody Material Slots name matches a mesh material slot, so clothing may follow any slot. Check the slot names in Softbody Material Slots."));
		for (uint8& Eligible : TriEligible)
		{
			Eligible = 1;
		}
	}

	// 바깥쪽: 메시 전체의 부호 있는 부피가 양이 되도록 노멀 방향을 맞춘다(닫힌 몸이 대부분을 차지한다).
	double SignedVolume = 0.0;
	for (int32 t = 0; t < Tris.Num(); ++t)
	{
		SignedVolume += P[Tris[t].A].Dot(TriNormal[t]);
	}
	if (SignedVolume < 0.0)
	{
		for (FVector3d& Normal : TriNormal)
		{
			Normal = -Normal;
		}
	}

	// 정점 노멀 = 둘레 삼각형 노멀의 넓이 무게 합. 앞뒤 면이 한 정점을 나눠 서로 지우면 방향이 없다.
	TArray<FVector3d> VertexNormal;
	VertexNormal.Init(FVector3d::ZeroVector, N);
	TArray<double> VertexArea;
	VertexArea.Init(0.0, N);
	for (int32 t = 0; t < Tris.Num(); ++t)
	{
		for (int32 k = 0; k < 3; ++k)
		{
			VertexNormal[Tris[t][k]] += TriNormal[t];
			VertexArea[Tris[t][k]] += TriNormal[t].Length();
		}
	}
	TBitArray<> HasNormal(false, N);
	for (int32 v = 0; v < N; ++v)
	{
		const double Length = VertexNormal[v].Length();
		if (Length > 0.2 * VertexArea[v] && Length > 1e-12)
		{
			VertexNormal[v] /= Length;
			HasNormal[v] = true;
		}
	}

	// 같은 자리(0.01 mm) 정점 묶음과 조각(삼각형으로 이어진 부분, 같은 자리는 하나로 본다)
	TArray<int32> Weld;
	Weld.SetNumUninitialized(N);
	{
		TMap<FIntVector, int32> Cells;
		Cells.Reserve(N);
		constexpr double Quantum = 1e-3;
		for (int32 v = 0; v < N; ++v)
		{
			const FIntVector Key((int32)FMath::RoundToDouble(P[v].X / Quantum), (int32)FMath::RoundToDouble(P[v].Y / Quantum),
				(int32)FMath::RoundToDouble(P[v].Z / Quantum));
			Weld[v] = Cells.FindOrAdd(Key, v);
		}
	}
	TArray<int32> Piece = Weld;
	auto FindPiece = [&Piece](int32 v)
	{
		while (Piece[v] != v)
		{
			Piece[v] = Piece[Piece[v]];
			v = Piece[v];
		}
		return v;
	};
	for (const FIndex3i& I : Tris)
	{
		const int32 A = FindPiece(I.A);
		for (int32 k = 1; k < 3; ++k)
		{
			const int32 B = FindPiece(I[k]);
			if (A != B)
			{
				Piece[B] = A;
			}
		}
	}
	for (int32 v = 0; v < N; ++v)
	{
		Piece[v] = FindPiece(v);
	}

	// 광선용 삼각형 모음(꼭짓점을 나누지 않아 비다양체도 그대로 들어간다; 삼각형 번호 = Tris 번호)
	FDynamicMesh3 Soup;
	for (int32 t = 0; t < Tris.Num(); ++t)
	{
		const int32 A = Soup.AppendVertex(P[Tris[t].A]);
		const int32 B = Soup.AppendVertex(P[Tris[t].B]);
		const int32 C = Soup.AppendVertex(P[Tris[t].C]);
		if (Soup.AppendTriangle(A, B, C) != t)
		{
			UE_LOG(LogSoftbodyLayers, Error, TEXT("Layered clothing: could not add triangle %d to the ray search, so layered clothing is skipped for this mesh."), t);
			return;
		}
	}
	const FDynamicMeshAABBTree3 Tree(&Soup, true);

	// 정점마다 안쪽으로 광선
	const double Thin = 0.3 * MaxGap;
	constexpr double Lift = 1e-3;
	TArray<FFollow> Follow;
	Follow.SetNum(N);
	TArray<EFollowKind> Kind;
	Kind.Init(EFollowKind::None, N);
	ParallelFor(N, [&](int32 v)
	{
		if (!HasNormal[v])
		{
			return;
		}
		const FVector3d Normal = VertexNormal[v];
		const FRay3d Ray(P[v] + Normal * Lift, -Normal);
		const int32 Self = Weld[v];
		// 이 정점(과 같은 자리 정점)을 꼭짓점으로 가진 삼각형은 제 면이다.
		const IMeshSpatial::FQueryOptions Options(MaxGap + Lift, [&Tris, &TriEligible, &Weld, Self](int32 t)
		{
			const FIndex3i& I = Tris[t];
			return TriEligible[t] != 0 && Weld[I.A] != Self && Weld[I.B] != Self && Weld[I.C] != Self;
		});
		TArray<MeshIntersection::FHitIntersectionResult> Hits;
		if (!Tree.FindAllHitTriangles(Ray, Hits, Options))
		{
			return;
		}
		auto Facing = [&](int32 h) { return TriNormal[Hits[h].TriangleId].Dot(Normal); };
		auto OtherPiece = [&](int32 h) { return Piece[Tris[Hits[h].TriangleId].A] != Piece[v]; };
		int32 Pick = INDEX_NONE;
		EFollowKind PickKind = EFollowKind::None;
		if (Facing(0) > 0.0)
		{
			if (OtherPiece(0))
			{
				Pick = 0;
				PickKind = EFollowKind::Cover;
			}
		}
		else if (Hits[0].Distance - Lift <= Thin)
		{
			// 뒷면을 지난 다음 면(뒷면이 이웃 삼각형 경계에서 두 번 잡힌 것은 건너뛴다)
			int32 Next = 1;
			while (Next < Hits.Num() && Facing(Next) <= 0.0 && Hits[Next].Distance - Hits[0].Distance < 1e-3)
			{
				++Next;
			}
			if (Next < Hits.Num() && Facing(Next) > 0.0 && OtherPiece(Next))
			{
				Pick = Next;
				PickKind = EFollowKind::CoverThroughSheet;
			}
			else
			{
				Pick = 0;
				PickKind = EFollowKind::Sheet;
			}
		}
		if (Pick == INDEX_NONE)
		{
			return;
		}
		const MeshIntersection::FHitIntersectionResult& Hit = Hits[Pick];
		const FIndex3i& I = Tris[Hit.TriangleId];
		FVector3d Bary(FMath::Max(Hit.BaryCoords.X, 0.0), FMath::Max(Hit.BaryCoords.Y, 0.0), FMath::Max(Hit.BaryCoords.Z, 0.0));
		Bary /= FMath::Max(Bary.X + Bary.Y + Bary.Z, 1e-12);
		FFollow& F = Follow[v];
		for (int32 k = 0; k < 3; ++k)
		{
			F.Corner[k] = I[k];
			F.Bary[k] = (float)Bary[k];
		}
		// 천의 앞뒤는 늘 함께, 덮인 살은 MaxGap 절반부터 서서히 덜 따른다(살에서 떨어진 천은 제 값).
		const double Gap = FMath::Max(Hit.Distance - Lift, 0.0);
		F.Strength = PickKind == EFollowKind::Sheet
			? 1.f
			: 1.f - (float)FMath::SmoothStep(0.5, 1.0, Gap / MaxGap);
		Kind[v] = F.Strength > 0.f ? PickKind : EFollowKind::None;
	});

	// 판 뒷면 따름은 뒷면(천의 바깥 면)에서 덮인 살을 따르는 꼭짓점만 쓴다. 천의 안쪽 면이 바깥 면을 거쳐 살을 따르게 하려는
	// 것이고, 손가락 사이처럼 살끼리 얇은 곳의 앞뒤는 같은 살이라 따를 필요가 없다. 마주 본 앞뒤 정점이 서로를 거의 통째로
	// 따르는 고리(풀리지 않는다)도 이것으로 생기지 않는다.
	auto FollowsCovered = [&Kind](int32 v)
	{
		return v != INDEX_NONE && (Kind[v] == EFollowKind::Cover || Kind[v] == EFollowKind::CoverThroughSheet);
	};
	for (int32 v = 0; v < N; ++v)
	{
		if (Kind[v] != EFollowKind::Sheet)
		{
			continue;
		}
		FFollow& F = Follow[v];
		float Sum = 0.f;
		for (int32 k = 0; k < 3; ++k)
		{
			F.Bary[k] = FollowsCovered(F.Corner[k]) ? F.Bary[k] : 0.f;
			Sum += F.Bary[k];
		}
		if (Sum < 1e-3f)
		{
			Kind[v] = EFollowKind::None;
			continue;
		}
		for (float& Bary : F.Bary)
		{
			Bary /= Sum;
		}
	}

	// 같은 자리 정점: 따르는 곳이 없는 정점은 같은 자리에서 따르는 곳이 있는 정점을 따른다(덮인 살을 찾은 쪽 우선).
	TMap<int32, int32> FollowerAtPoint;
	for (int32 v = 0; v < N; ++v)
	{
		if (Kind[v] == EFollowKind::None)
		{
			continue;
		}
		int32& Best = FollowerAtPoint.FindOrAdd(Weld[v], v);
		if (Kind[Best] == EFollowKind::Sheet && Kind[v] != EFollowKind::Sheet)
		{
			Best = v;
		}
	}
	for (int32 v = 0; v < N; ++v)
	{
		const int32* Best = Kind[v] == EFollowKind::None ? FollowerAtPoint.Find(Weld[v]) : nullptr;
		if (Best && *Best != v)
		{
			Follow[v] = FFollow();
			Follow[v].Corner[0] = *Best;
			Follow[v].Bary[0] = 1.f;
			Follow[v].Strength = 1.f;
			Kind[v] = EFollowKind::SamePoint;
		}
	}

	// 사슬 풀기: 따르는 정점의 값 = 세기 × 따를 곳 꼭짓점 값의 무게 합 + (1 − 세기) × 제 값. 꼭짓점도 따르는 정점이면
	// 그 섞음을 쓴다. 차례로 고쳐 쓰며, 덮이지 않은 살까지 절반 넘게 이어진 섞음이 바뀌지 않을 때까지 되풀이한다. 서로를
	// 따르는 고리(손가락 사이 얇은 살 앞뒤)는 살에 닿지 않아 천천히 한 값으로 모일 뿐이라 기다리지 않는다.
	TArray<int32> Linked;
	TArray<int32> MixIndex;
	MixIndex.Init(INDEX_NONE, N);
	for (int32 v = 0; v < N; ++v)
	{
		if (Kind[v] != EFollowKind::None)
		{
			MixIndex[v] = Linked.Num();
			Linked.Add(v);
		}
	}
	TArray<FMix> Mixes;
	Mixes.SetNum(Linked.Num());
	for (int32 i = 0; i < Linked.Num(); ++i)
	{
		Mixes[i].Emplace(Linked[i], 1.f);
	}
	int32 Sweeps = 0;
	float Change = 0.f;
	float LoopChange = 0.f;
	for (; Sweeps < 64; ++Sweeps)
	{
		Change = 0.f;
		LoopChange = 0.f;
		for (int32 i = 0; i < Linked.Num(); ++i)
		{
			const int32 v = Linked[i];
			const FFollow& F = Follow[v];
			FMix Next;
			for (int32 k = 0; k < 3; ++k)
			{
				const int32 Corner = F.Corner[k];
				if (Corner == INDEX_NONE || F.Bary[k] <= 0.f)
				{
					continue;
				}
				const float Weight = F.Strength * F.Bary[k];
				if (MixIndex[Corner] == INDEX_NONE)
				{
					AddTo(Next, Corner, Weight);
				}
				else
				{
					for (const TPair<int32, float>& Entry : Mixes[MixIndex[Corner]])
					{
						AddTo(Next, Entry.Key, Weight * Entry.Value);
					}
				}
			}
			if (F.Strength < 1.f)
			{
				AddTo(Next, v, 1.f - F.Strength);
			}
			Normalize(Next);
			float RootShare = 0.f;
			for (const TPair<int32, float>& Entry : Next)
			{
				RootShare += MixIndex[Entry.Key] == INDEX_NONE ? Entry.Value : 0.f;
			}
			float& Tracked = RootShare >= 0.5f ? Change : LoopChange;
			Tracked = FMath::Max(Tracked, MixDifference(Mixes[i], Next));
			Mixes[i] = MoveTemp(Next);
		}
		if (Change < 1e-4f && Sweeps > 0)
		{
			++Sweeps;
			break;
		}
	}

	// 결과와 로그: 종류별 수, 슬롯별 따르는 정점 비율, 따르지 않는 정점(덮이지 않은 살)까지 이어진 몫
	int32 KindCount[5] = {};
	TMap<FName, FIntPoint> PerSlot; // X 정점, Y 따르는 정점
	double RootShare = 0.0;
	for (int32 v = 0; v < N; ++v)
	{
		++KindCount[(int32)Kind[v]];
		FIntPoint& Slot = PerSlot.FindOrAdd(VertexSlot[v]);
		++Slot.X;
	}
	for (int32 i = 0; i < Linked.Num(); ++i)
	{
		const int32 v = Linked[i];
		const FMix& Mix = Mixes[i];
		if (Mix.Num() == 1 && Mix[0].Key == v)
		{
			continue;
		}
		++PerSlot.FindOrAdd(VertexSlot[v]).Y;
		FSoftbodyLayerLink& Link = OutLinks.AddDefaulted_GetRef();
		Link.Vertex = v;
		for (const TPair<int32, float>& Entry : Mix)
		{
			Link.Sources.Add(Entry.Key);
			Link.Weights.Add(Entry.Value);
			RootShare += MixIndex[Entry.Key] == INDEX_NONE ? Entry.Value : 0.0;
		}
	}
	FString SlotLog;
	for (const TPair<FName, FIntPoint>& Slot : PerSlot)
	{
		if (Slot.Value.Y > 0)
		{
			SlotLog += FString::Printf(TEXT("%s%s %d/%d"), SlotLog.IsEmpty() ? TEXT("") : TEXT(", "),
				*Slot.Key.ToString(), Slot.Value.Y, Slot.Value.X);
		}
	}
	UE_LOG(LogSoftbodyLayers, Display,
		TEXT("Layered clothing (max gap %.2f): %d vertices - over flesh %d, over flesh behind a sheet %d, sheet back %d, same point %d -> %d following vertices "
			"(mean share reaching uncovered flesh %.2f, %d passes - remaining change %.1e, loops not reaching flesh %.1e). Following/total per slot: %s"),
		MaxGap, N, KindCount[(int32)EFollowKind::Cover], KindCount[(int32)EFollowKind::CoverThroughSheet],
		KindCount[(int32)EFollowKind::Sheet], KindCount[(int32)EFollowKind::SamePoint], OutLinks.Num(),
		OutLinks.Num() > 0 ? RootShare / OutLinks.Num() : 0.0, Sweeps, Change, LoopChange, *SlotLog);
}
}
