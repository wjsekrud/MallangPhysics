// Copyright (c) 2026 wjsekrud

#include "SoftbodyLumpSupport.h"

#include "Distance/DistPoint3Triangle3.h"
#include "MeshQueries.h"
#include "TriangleTypes.h"

THIRD_PARTY_INCLUDES_START
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
THIRD_PARTY_INCLUDES_END

// 두 파일(SoftbodyCarrierBake·SoftbodyLumpSupport)이 같은 static 분류를 쓴다 — 유니티 빌드에서 한 묶음에 들면 먼저 온 정의 하나만 남긴다.
#ifndef SOFTBODY_EDITOR_LOG_CARRIER_DEFINED
#define SOFTBODY_EDITOR_LOG_CARRIER_DEFINED
DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyCarrier, Log, All);
#endif

using namespace UE::Geometry;

namespace SoftbodyLumpSupport
{
namespace
{
	/** 덩어리 칸으로 치는 몫 — BuildLumpTexels와 같은 문턱. */
	constexpr double SeedWeight = 1e-3;
	/** 고리를 이만큼 넓혀도 깊이가 계속 늘면 둔덕으로 보지 않는다(안전 한도, 실측은 다섯 고리 안 — 일지 246). */
	constexpr int32 MaxRings = 512;
	/** 영역이 살갗 조각 넓이의 이 몫을 넘도록 깊이가 계속 늘면 둔덕으로 보지 않는다. */
	constexpr double MaxRegionShare = 0.25;
	/** 빼빼한 삼각형이 막을 흔들지 않게 각마다 코탄젠트를 이 안으로 자른다. */
	constexpr double MaxCot = 10.0;
	/** 같은 자리 정점을 하나로 보는 격자(cm). */
	constexpr double WeldCell = 1e-3;

	using FColMatrix = Eigen::SparseMatrix<double>;
	using FRowMatrix = Eigen::SparseMatrix<double, Eigen::RowMajor>;

	/** 영역 정점의 자리를 영역 밖 살갗을 위치와 기울기까지 이어 가장 매끄럽게 메운다(Q = L·A⁻¹·L, 영역 밖은 그대로). */
	bool FillPositions(const FRowMatrix& Q, const TArray<FVector3d>& P, const TArray<uint8>& InRegion, TArray<FVector3d>& Out)
	{
		const int32 N = P.Num();
		TArray<int32> Local;
		Local.Init(INDEX_NONE, N);
		TArray<int32> Ids;
		for (int32 v = 0; v < N; ++v)
		{
			if (InRegion[v])
			{
				Local[v] = Ids.Num();
				Ids.Add(v);
			}
		}
		Out = P;
		const int32 M = Ids.Num();
		if (M == 0)
		{
			return true;
		}
		TArray<Eigen::Triplet<double>> Trip;
		Eigen::MatrixXd Rhs = Eigen::MatrixXd::Zero(M, 3);
		for (int32 i = 0; i < M; ++i)
		{
			for (FRowMatrix::InnerIterator It(Q, Ids[i]); It; ++It)
			{
				const int32 j = (int32)It.col();
				const double q = It.value();
				if (Local[j] != INDEX_NONE)
				{
					Trip.Emplace(i, Local[j], q);
				}
				else
				{
					Rhs(i, 0) -= q * P[j].X;
					Rhs(i, 1) -= q * P[j].Y;
					Rhs(i, 2) -= q * P[j].Z;
				}
			}
		}
		FColMatrix A(M, M);
		A.setFromTriplets(Trip.GetData(), Trip.GetData() + Trip.Num());
		Eigen::SimplicialLDLT<FColMatrix> LDLT(A);
		if (LDLT.info() != Eigen::Success)
		{
			return false;
		}
		const Eigen::MatrixXd X = LDLT.solve(Rhs);
		if (LDLT.info() != Eigen::Success || !X.allFinite())
		{
			return false;
		}
		for (int32 i = 0; i < M; ++i)
		{
			Out[Ids[i]] = FVector3d(X(i, 0), X(i, 1), X(i, 2));
		}
		return true;
	}

	/**
	 * 영역 밖 값(두 벌)을 영역 안으로 고르게 잇는다: 양의 무게 라플라스라 영역 안 값은 둘레 값 사이에 머문다(밑 두께가 둘레보다
	 * 두껍거나 얇아지지 않는다). W = 변마다 양의 무게(대각 0).
	 */
	bool FillValues(const FRowMatrix& W, const TArray<uint8>& InRegion, TArray<double>& V0, TArray<double>& V1)
	{
		const int32 N = InRegion.Num();
		TArray<int32> Local;
		Local.Init(INDEX_NONE, N);
		TArray<int32> Ids;
		for (int32 v = 0; v < N; ++v)
		{
			if (InRegion[v])
			{
				Local[v] = Ids.Num();
				Ids.Add(v);
			}
		}
		const int32 M = Ids.Num();
		if (M == 0)
		{
			return true;
		}
		TArray<Eigen::Triplet<double>> Trip;
		Eigen::MatrixXd Rhs = Eigen::MatrixXd::Zero(M, 2);
		for (int32 i = 0; i < M; ++i)
		{
			double Diagonal = 0.0;
			for (FRowMatrix::InnerIterator It(W, Ids[i]); It; ++It)
			{
				const int32 j = (int32)It.col();
				const double w = It.value();
				Diagonal += w;
				if (Local[j] != INDEX_NONE)
				{
					Trip.Emplace(i, Local[j], -w);
				}
				else
				{
					Rhs(i, 0) += w * V0[j];
					Rhs(i, 1) += w * V1[j];
				}
			}
			Trip.Emplace(i, i, Diagonal);
		}
		FColMatrix A(M, M);
		A.setFromTriplets(Trip.GetData(), Trip.GetData() + Trip.Num());
		Eigen::SimplicialLDLT<FColMatrix> LDLT(A);
		if (LDLT.info() != Eigen::Success)
		{
			return false;
		}
		const Eigen::MatrixXd X = LDLT.solve(Rhs);
		if (LDLT.info() != Eigen::Success || !X.allFinite())
		{
			return false;
		}
		for (int32 i = 0; i < M; ++i)
		{
			V0[Ids[i]] = X(i, 0);
			V1[Ids[i]] = X(i, 1);
		}
		return true;
	}

	/** Point에서 메운 면(Tris의 삼각형, Filled 자리)까지 가장 가까운 거리. */
	double DepthUnder(const FVector3d& Point, const TArray<FVector3d>& Filled, const TArray<FIntVector>& Triangles, const TArray<int32>& Tris)
	{
		double Best = TNumericLimits<double>::Max();
		for (const int32 t : Tris)
		{
			const FIntVector& T = Triangles[t];
			FDistPoint3Triangle3d Distance(Point, FTriangle3d(Filled[T.X], Filled[T.Y], Filled[T.Z]));
			Best = FMath::Min(Best, Distance.GetSquared());
		}
		return Best < TNumericLimits<double>::Max() ? FMath::Sqrt(Best) : 0.0;
	}

	/** 한 덩어리 묶음(관성 그룹)의 씨앗과 꼭대기. */
	struct FLumpSeed
	{
		int32 Group = INDEX_NONE;
		int32 Piece = INDEX_NONE;     // 받침을 찾는 살갗 조각
		int32 Apex = INDEX_NONE;      // 살갗 정점(아래 지역 번호)
		double SeedArea = 0.0;
		int32 Cluster = INDEX_NONE;
	};

	/** 막이 서로 닿는(두 고리 안) 덩어리 묶음들 — 함께 넓히고 함께 멈춘다. */
	struct FCluster
	{
		TArray<int32> Lumps;          // FLumpSeed 번호
		TArray<int32> Vertices;       // 지금 영역
		double Area = 0.0;
		double PieceArea = 0.0;
		double Depth = 0.0;           // 꼭대기 밑 깊이의 합
		int32 Rings = 0;
		bool bActive = true;
		bool bFailed = false;
		FString Why;
	};
}

bool FSupport::Build(const FInput& In)
{
	Notes.Reset();
	const int32 NumRender = In.Positions.Num();
	if (NumRender == 0 || In.Lump.Num() != NumRender || In.LumpWeight.Num() != NumRender || In.BoneDepth.Num() != NumRender
		|| In.Gain.Num() != NumRender)
	{
		return false;
	}

	// --- 같은 자리 정점을 하나로 (나눠진 조각을 꿰맨 메시가 막에 이음매를 만들지 않게) ---
	TArray<int32> Weld;
	Weld.SetNumUninitialized(NumRender);
	TArray<int32> FirstRender;   // 묶은 정점 → 처음 렌더 정점
	TMap<FIntVector, int32> CellOf;
	for (int32 v = 0; v < NumRender; ++v)
	{
		const FVector3d& P = In.Positions[v];
		const FIntVector Key((int32)FMath::RoundToInt(P.X / WeldCell), (int32)FMath::RoundToInt(P.Y / WeldCell),
			(int32)FMath::RoundToInt(P.Z / WeldCell));
		if (const int32* Found = CellOf.Find(Key))
		{
			Weld[v] = *Found;
		}
		else
		{
			Weld[v] = FirstRender.Num();
			CellOf.Add(Key, Weld[v]);
			FirstRender.Add(v);
		}
	}
	const int32 NumWeld = FirstRender.Num();
	TArray<int32> WeldLump;
	TArray<double> WeldWeight;
	WeldLump.Init(INDEX_NONE, NumWeld);
	WeldWeight.Init(0.0, NumWeld);
	for (int32 v = 0; v < NumRender; ++v)
	{
		const int32 w = Weld[v];
		if (In.Lump[v] != INDEX_NONE && In.LumpWeight[v] > WeldWeight[w])
		{
			WeldLump[w] = In.Lump[v];
			WeldWeight[w] = In.LumpWeight[v];
		}
	}
	TArray<FIntVector> WeldTris;
	WeldTris.Reserve(In.Triangles.Num());
	for (const FIntVector& T : In.Triangles)
	{
		if (T.X < 0 || T.Y < 0 || T.Z < 0 || T.X >= NumRender || T.Y >= NumRender || T.Z >= NumRender)
		{
			continue;
		}
		const FIntVector W(Weld[T.X], Weld[T.Y], Weld[T.Z]);
		if (W.X == W.Y || W.Y == W.Z || W.Z == W.X)
		{
			continue;
		}
		const FVector3d& A = In.Positions[FirstRender[W.X]];
		const FVector3d& B = In.Positions[FirstRender[W.Y]];
		const FVector3d& C = In.Positions[FirstRender[W.Z]];
		if ((B - A).Cross(C - A).SizeSquared() < 1e-20)
		{
			continue;
		}
		WeldTris.Add(W);
	}

	// --- 이어진 조각 ---
	TArray<int32> Parent;
	Parent.SetNumUninitialized(NumWeld);
	for (int32 v = 0; v < NumWeld; ++v)
	{
		Parent[v] = v;
	}
	const auto Root = [&Parent](int32 v)
	{
		while (Parent[v] != v)
		{
			Parent[v] = Parent[Parent[v]];
			v = Parent[v];
		}
		return v;
	};
	for (const FIntVector& T : WeldTris)
	{
		const int32 A = Root(T.X);
		const int32 B = Root(T.Y);
		const int32 C = Root(T.Z);
		Parent[B] = A;
		Parent[Root(C)] = A;
	}
	TMap<int32, double> PieceArea;
	for (const FIntVector& T : WeldTris)
	{
		const FVector3d& A = In.Positions[FirstRender[T.X]];
		PieceArea.FindOrAdd(Root(T.X)) += 0.5 * (In.Positions[FirstRender[T.Y]] - A).Cross(In.Positions[FirstRender[T.Z]] - A).Size();
	}

	// --- 덩어리 묶음마다 받침을 찾을 살갗 조각: 그 묶음의 웨이트가 걸린 조각 가운데 가장 넓은 것(옷 조각이 아니라 몸) ---
	TMap<int32, int32> PieceOfGroup;
	for (int32 w = 0; w < NumWeld; ++w)
	{
		if (WeldLump[w] == INDEX_NONE || WeldWeight[w] <= SeedWeight)
		{
			continue;
		}
		const int32 Piece = Root(w);
		int32& Best = PieceOfGroup.FindOrAdd(WeldLump[w], Piece);
		if (PieceArea.FindRef(Piece) > PieceArea.FindRef(Best))
		{
			Best = Piece;
		}
	}
	if (PieceOfGroup.Num() == 0)
	{
		return false;
	}
	TSet<int32> BasePieces;
	for (const TPair<int32, int32>& Pair : PieceOfGroup)
	{
		BasePieces.Add(Pair.Value);
	}

	// --- 받침을 찾는 살갗(위 조각들)의 지역 번호 ---
	TArray<int32> LocalOf;
	LocalOf.Init(INDEX_NONE, NumWeld);
	TArray<int32> WeldOf;
	for (int32 w = 0; w < NumWeld; ++w)
	{
		if (BasePieces.Contains(Root(w)))
		{
			LocalOf[w] = WeldOf.Num();
			WeldOf.Add(w);
		}
	}
	const int32 N = WeldOf.Num();
	TArray<FVector3d> P;
	P.SetNumUninitialized(N);
	TArray<int32> PieceOfVertex;
	PieceOfVertex.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		P[i] = In.Positions[FirstRender[WeldOf[i]]];
		PieceOfVertex[i] = Root(WeldOf[i]);
	}
	TArray<FIntVector> T;
	for (const FIntVector& W : WeldTris)
	{
		if (LocalOf[W.X] != INDEX_NONE)
		{
			T.Emplace(LocalOf[W.X], LocalOf[W.Y], LocalOf[W.Z]);
		}
	}

	// --- 코탄젠트 라플라스 L, 정점 넓이 A, 막의 행렬 Q = L·A⁻¹·L, 밑 두께를 잇는 양의 무게 W ---
	TArray<double> Area;
	Area.Init(0.0, N);
	TArray<FVector3d> Normal;
	Normal.Init(FVector3d::ZeroVector, N);
	TArray<TArray<int32>> Nbr;
	Nbr.SetNum(N);
	TArray<TArray<int32>> TrisOf;
	TrisOf.SetNum(N);
	TArray<Eigen::Triplet<double>> LTrip;
	LTrip.Reserve(T.Num() * 12);
	TMap<uint64, double> EdgeCot;
	EdgeCot.Reserve(T.Num() * 3 / 2);
	for (int32 t = 0; t < T.Num(); ++t)
	{
		const int32 Id[3] = {T[t].X, T[t].Y, T[t].Z};
		const FVector3d Cross = (P[Id[1]] - P[Id[0]]).Cross(P[Id[2]] - P[Id[0]]);
		const double TriArea = 0.5 * Cross.Size();
		for (int32 k = 0; k < 3; ++k)
		{
			const int32 i = Id[k];
			const int32 j = Id[(k + 1) % 3];
			const int32 o = Id[(k + 2) % 3];
			const FVector3d a = P[i] - P[o];
			const FVector3d b = P[j] - P[o];
			const double Cot = FMath::Clamp(a.Dot(b) / FMath::Max(a.Cross(b).Size(), 1e-12), -MaxCot, MaxCot);
			const uint64 Key = ((uint64)FMath::Min(i, j) << 32) | (uint32)FMath::Max(i, j);
			EdgeCot.FindOrAdd(Key) += 0.5 * Cot;
			Area[i] += TriArea / 3.0;
			Normal[i] += Cross;
			TrisOf[i].Add(t);
			Nbr[i].AddUnique(j);
			Nbr[j].AddUnique(i);
		}
	}
	TArray<double> Diagonal;
	Diagonal.Init(0.0, N);
	TArray<Eigen::Triplet<double>> WTrip;
	WTrip.Reserve(EdgeCot.Num() * 2);
	double MeanPositive = 0.0;
	for (const TPair<uint64, double>& Edge : EdgeCot)
	{
		MeanPositive += FMath::Max(Edge.Value, 0.0);
	}
	MeanPositive = EdgeCot.Num() > 0 ? MeanPositive / EdgeCot.Num() : 1.0;
	for (const TPair<uint64, double>& Edge : EdgeCot)
	{
		const int32 i = (int32)(Edge.Key >> 32);
		const int32 j = (int32)(Edge.Key & 0xffffffffu);
		LTrip.Emplace(i, j, Edge.Value);
		LTrip.Emplace(j, i, Edge.Value);
		Diagonal[i] -= Edge.Value;
		Diagonal[j] -= Edge.Value;
		// 둔각 변의 음수 무게는 0으로, 끊긴 변이 없게 작은 무게를 더한다.
		const double Positive = FMath::Max(Edge.Value, 0.0) + 1e-3 * MeanPositive;
		WTrip.Emplace(i, j, Positive);
		WTrip.Emplace(j, i, Positive);
	}
	Eigen::VectorXd InvArea(N);
	for (int32 i = 0; i < N; ++i)
	{
		LTrip.Emplace(i, i, Diagonal[i]);
		InvArea(i) = 1.0 / FMath::Max(Area[i], 1e-8);
	}
	FColMatrix L(N, N);
	L.setFromTriplets(LTrip.GetData(), LTrip.GetData() + LTrip.Num());
	const FColMatrix LA = L * InvArea.asDiagonal();
	const FColMatrix QCol = LA * L;
	FRowMatrix Q;
	Q = QCol;
	FRowMatrix W(N, N);
	W.setFromTriplets(WTrip.GetData(), WTrip.GetData() + WTrip.Num());

	// --- 덩어리 묶음마다 씨앗(웨이트가 걸린 살갗)과 꼭대기(웨이트 절반 넘는 곳에서 뼈 반대쪽으로 가장 나온 정점) ---
	TArray<FLumpSeed> Seeds;
	TArray<uint8> Seed;
	Seed.Init(0, N);
	TArray<int32> SeedLump;
	SeedLump.Init(INDEX_NONE, N);
	TArray<int32> Groups;
	PieceOfGroup.GetKeys(Groups);
	Groups.Sort();
	for (const int32 Group : Groups)
	{
		const int32 Piece = PieceOfGroup[Group];
		double MaxWeight = 0.0;
		int32 NumInPiece = 0;
		int32 NumSeed = 0;
		for (int32 i = 0; i < N; ++i)
		{
			if (PieceOfVertex[i] != Piece)
			{
				continue;
			}
			++NumInPiece;
			if (WeldLump[WeldOf[i]] == Group && WeldWeight[WeldOf[i]] > SeedWeight)
			{
				++NumSeed;
				MaxWeight = FMath::Max(MaxWeight, WeldWeight[WeldOf[i]]);
			}
		}
		const FName Bone = In.GroupBones.IsValidIndex(Group) ? In.GroupBones[Group] : NAME_None;
		if (NumSeed == 0)
		{
			continue;
		}
		if (NumSeed == NumInPiece)
		{
			UE_LOG(LogSoftbodyCarrier, Warning,
				TEXT("Lump support for %s: its skin weights cover the whole mesh piece, so there is no surrounding skin to continue under it. Its flesh thickness stays measured to the bones."),
				*Bone.ToString());
			Notes.Add(FString::Printf(TEXT("%s: no base. Its skin weights cover its whole mesh piece, so its flesh is measured to the bones."), *Bone.ToString()));
			continue;
		}
		FLumpSeed& Entry = Seeds.AddDefaulted_GetRef();
		Entry.Group = Group;
		Entry.Piece = Piece;
		FVector3d Mean = FVector3d::ZeroVector;
		FVector3d Centroid = FVector3d::ZeroVector;
		double Mass = 0.0;
		for (int32 i = 0; i < N; ++i)
		{
			if (PieceOfVertex[i] != Piece || WeldLump[WeldOf[i]] != Group || WeldWeight[WeldOf[i]] <= SeedWeight)
			{
				continue;
			}
			Seed[i] = 1;
			SeedLump[i] = Seeds.Num() - 1;
			Entry.SeedArea += Area[i];
			const double Weight = WeldWeight[WeldOf[i]];
			if (Weight >= 0.5 * MaxWeight)
			{
				Mean += Normal[i].GetSafeNormal() * (Area[i] * Weight);
				Centroid += P[i] * (Area[i] * Weight);
				Mass += Area[i] * Weight;
			}
		}
		Centroid /= FMath::Max(Mass, 1e-12);
		// 노멀의 감긴 방향에 기대지 않게, 둔덕 가운데가 가장 가까운 뼈에서 멀어지는 쪽을 바깥으로 본다.
		FVector3d Away = FVector3d::ZeroVector;
		double Nearest = TNumericLimits<double>::Max();
		for (const FVector4& Core : In.Cores)
		{
			const FVector3d C(Core.X, Core.Y, Core.Z);
			const double Distance = FVector3d::Distance(Centroid, C) - Core.W;
			if (Distance < Nearest)
			{
				Nearest = Distance;
				Away = Centroid - C;
			}
		}
		Mean = Mean.GetSafeNormal();
		if (!Away.IsZero() && Mean.Dot(Away) < 0.0)
		{
			Mean = -Mean;
		}
		double Highest = -TNumericLimits<double>::Max();
		for (int32 i = 0; i < N; ++i)
		{
			if (SeedLump[i] == Seeds.Num() - 1 && WeldWeight[WeldOf[i]] >= 0.5 * MaxWeight && P[i].Dot(Mean) > Highest)
			{
				Highest = P[i].Dot(Mean);
				Entry.Apex = i;
			}
		}
	}
	if (Seeds.Num() == 0)
	{
		return false;
	}

	// --- 막이 서로 닿는 묶음끼리 무리(씨앗을 두 고리 넓혀 이어진 것 — 막을 메우는 식이 두 고리까지 읽는다) ---
	TArray<FCluster> Clusters;
	TArray<int32> ClusterOf;
	ClusterOf.Init(INDEX_NONE, N);
	{
		TArray<uint8> Near = Seed;
		for (int32 Ring = 0; Ring < 2; ++Ring)
		{
			TArray<uint8> Next = Near;
			for (int32 i = 0; i < N; ++i)
			{
				if (Near[i])
				{
					for (const int32 j : Nbr[i])
					{
						Next[j] = 1;
					}
				}
			}
			Near = MoveTemp(Next);
		}
		TArray<FCluster> Found;
		for (int32 i = 0; i < N; ++i)
		{
			if (!Near[i] || ClusterOf[i] != INDEX_NONE)
			{
				continue;
			}
			const int32 Id = Found.Num();
			Found.AddDefaulted();
			TArray<int32> Stack = {i};
			ClusterOf[i] = Id;
			while (Stack.Num() > 0)
			{
				const int32 v = Stack.Pop(EAllowShrinking::No);
				for (const int32 j : Nbr[v])
				{
					if (Near[j] && ClusterOf[j] == INDEX_NONE)
					{
						ClusterOf[j] = Id;
						Stack.Add(j);
					}
				}
			}
		}
		for (int32 s = 0; s < Seeds.Num(); ++s)
		{
			Seeds[s].Cluster = Seeds[s].Apex != INDEX_NONE ? ClusterOf[Seeds[s].Apex] : INDEX_NONE;
		}
		Clusters.Reset();
		for (int32 c = 0; c < Found.Num(); ++c)
		{
			FCluster& Cluster = Clusters.Add_GetRef(MoveTemp(Found[c]));
			for (int32 s = 0; s < Seeds.Num(); ++s)
			{
				if (Seeds[s].Cluster == c)
				{
					Cluster.Lumps.Add(s);
					Cluster.PieceArea = PieceArea.FindRef(Seeds[s].Piece);
				}
			}
		}
		for (int32 i = 0; i < N; ++i)
		{
			if (Seed[i])
			{
				FCluster& Cluster = Clusters[ClusterOf[i]];
				Cluster.Vertices.Add(i);
				Cluster.Area += Area[i];
			}
		}
	}

	// --- 넓히기: 무리마다 한 고리씩 넓혀 꼭대기 밑 깊이가 늘면 받아들이고, 처음으로 늘지 않으면 그 전에서 멈춘다 ---
	TArray<uint8> Region = Seed;
	TArray<int32> Owner;
	Owner.Init(INDEX_NONE, N);
	for (int32 i = 0; i < N; ++i)
	{
		Owner[i] = SeedLump[i] != INDEX_NONE ? Seeds[SeedLump[i]].Group : INDEX_NONE;
	}
	TArray<FVector3d> Filled;
	const auto TrianglesTouching = [&](const TArray<uint8>& InRegion)
	{
		TArray<int32> Out;
		for (int32 t = 0; t < T.Num(); ++t)
		{
			if (InRegion[T[t].X] || InRegion[T[t].Y] || InRegion[T[t].Z])
			{
				Out.Add(t);
			}
		}
		return Out;
	};
	const auto ClusterDepth = [&](const FCluster& Cluster, const TArray<FVector3d>& Pos, const TArray<int32>& Tris)
	{
		double Sum = 0.0;
		for (const int32 s : Cluster.Lumps)
		{
			Sum += DepthUnder(P[Seeds[s].Apex], Pos, T, Tris);
		}
		return Sum;
	};
	if (!FillPositions(Q, P, Region, Filled))
	{
		UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lump support: could not solve the surface under the lumps, so their flesh thickness stays measured to the bones."));
		return false;
	}
	{
		const TArray<int32> Tris = TrianglesTouching(Region);
		for (FCluster& Cluster : Clusters)
		{
			Cluster.Depth = ClusterDepth(Cluster, Filled, Tris);
			Cluster.bActive = Cluster.Lumps.Num() > 0;
		}
	}
	for (int32 Iteration = 0; Iteration < MaxRings; ++Iteration)
	{
		TArray<uint8> Trial = Region;
		TArray<TArray<int32>> Ring;
		Ring.SetNum(Clusters.Num());
		TArray<int32> RingOwner;
		RingOwner.Init(INDEX_NONE, N);
		bool bAny = false;
		for (int32 c = 0; c < Clusters.Num(); ++c)
		{
			if (!Clusters[c].bActive)
			{
				continue;
			}
			for (const int32 v : Clusters[c].Vertices)
			{
				for (const int32 j : Nbr[v])
				{
					if (!Trial[j])
					{
						Trial[j] = 1;
						Ring[c].Add(j);
						RingOwner[j] = Owner[v];
					}
				}
			}
			bAny |= Ring[c].Num() > 0;
			if (Ring[c].Num() == 0)
			{
				Clusters[c].bActive = false;
			}
		}
		if (!bAny)
		{
			break;
		}
		TArray<FVector3d> TrialFilled;
		if (!FillPositions(Q, P, Trial, TrialFilled))
		{
			for (FCluster& Cluster : Clusters)
			{
				Cluster.bActive = false;
			}
			break;
		}
		const TArray<int32> Tris = TrianglesTouching(Trial);
		for (int32 c = 0; c < Clusters.Num(); ++c)
		{
			FCluster& Cluster = Clusters[c];
			if (!Cluster.bActive)
			{
				continue;
			}
			const double Depth = ClusterDepth(Cluster, TrialFilled, Tris);
			if (Depth <= Cluster.Depth * (1.0 + 1e-6))
			{
				Cluster.bActive = false;
				continue;
			}
			Cluster.Depth = Depth;
			++Cluster.Rings;
			for (const int32 v : Ring[c])
			{
				Region[v] = 1;
				Owner[v] = RingOwner[v];
				Cluster.Vertices.Add(v);
				Cluster.Area += Area[v];
			}
			if (Cluster.Area > MaxRegionShare * Cluster.PieceArea)
			{
				Cluster.bActive = false;
				Cluster.bFailed = true;
				Cluster.Why = FString::Printf(TEXT("the surface under it kept getting deeper past %.0f%% of its mesh piece (%d rings)"),
					100.0 * MaxRegionShare, Cluster.Rings);
			}
		}
	}
	for (FCluster& Cluster : Clusters)
	{
		if (Cluster.bActive && !Cluster.bFailed)
		{
			Cluster.bFailed = true;
			Cluster.Why = FString::Printf(TEXT("the surface under it kept getting deeper for %d rings"), Cluster.Rings);
		}
	}

	// --- 못 찾은 무리는 빼고 마지막 막과 밑 두께 ---
	Region.Init(0, N);
	int32 NumFound = 0;
	for (const FCluster& Cluster : Clusters)
	{
		for (const int32 s : Cluster.Lumps)
		{
			const FName Bone = In.GroupBones.IsValidIndex(Seeds[s].Group) ? In.GroupBones[Seeds[s].Group] : NAME_None;
			if (Cluster.bFailed)
			{
				UE_LOG(LogSoftbodyCarrier, Warning,
					TEXT("Lump support for %s: could not find where the mound ends (%s), so its flesh thickness stays measured to the bones."),
					*Bone.ToString(), *Cluster.Why);
				Notes.Add(FString::Printf(TEXT("%s: no base. The bake could not find where the mound ends (%s), so its flesh is measured to the bones."),
					*Bone.ToString(), *Cluster.Why));
			}
		}
		if (Cluster.bFailed)
		{
			continue;
		}
		++NumFound;
		for (const int32 v : Cluster.Vertices)
		{
			Region[v] = 1;
		}
	}
	if (NumFound == 0 || !FillPositions(Q, P, Region, Filled))
	{
		return false;
	}
	TArray<double> BasePainted;
	TArray<double> BaseUnpainted;
	BasePainted.SetNumUninitialized(N);
	BaseUnpainted.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		const int32 r = FirstRender[WeldOf[i]];
		BaseUnpainted[i] = In.BoneDepth[r];
		BasePainted[i] = In.BoneDepth[r] * FMath::Clamp(In.Gain[r], 0.0, 1.0);
	}
	if (!FillValues(W, Region, BasePainted, BaseUnpainted))
	{
		UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lump support: could not carry the surrounding flesh thickness under the lumps, so their flesh thickness stays measured to the bones."));
		return false;
	}
	const TArray<int32> FinalTris = TrianglesTouching(Region);
	for (const FCluster& Cluster : Clusters)
	{
		if (Cluster.bFailed)
		{
			continue;
		}
		for (const int32 s : Cluster.Lumps)
		{
			const FLumpSeed& Entry = Seeds[s];
			const FName Bone = In.GroupBones.IsValidIndex(Entry.Group) ? In.GroupBones[Entry.Group] : NAME_None;
			const int32 r = FirstRender[WeldOf[Entry.Apex]];
			const double TipHeight = DepthUnder(P[Entry.Apex], Filled, T, FinalTris);
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Lump support for %s: weighted skin %.0f cm2, widened by %d rings to %.0f cm2 (with the lumps it touches); at the tip the mound is %.2f cm above the surface it sits on, flesh under that %.2f cm, bone distance %.2f cm"),
				*Bone.ToString(), Entry.SeedArea, Cluster.Rings, Cluster.Area, TipHeight, BasePainted[Entry.Apex], In.BoneDepth[r]);
			Notes.Add(FString::Printf(TEXT("%s: the tip is %.1f cm above its base, with %.1f cm of flesh under the base (%.0f cm2 of skin)."),
				*Bone.ToString(), TipHeight, BasePainted[Entry.Apex], Cluster.Area));
		}
	}

	// --- 질의용 메시: 살갗(원래 자리)과 막(영역과 그 둘레 한 고리 삼각형, 메운 자리) ---
	TArray<uint8> Near = Region;
	for (int32 i = 0; i < N; ++i)
	{
		if (Region[i])
		{
			for (const int32 j : Nbr[i])
			{
				Near[j] = 1;
			}
		}
	}
	Skin = FDynamicMesh3();
	Membrane = FDynamicMesh3();
	for (int32 i = 0; i < N; ++i)
	{
		Skin.AppendVertex(P[i]);
		Membrane.AppendVertex(Filled[i]);
	}
	for (const FIntVector& Tri : T)
	{
		Skin.AppendTriangle(FIndex3i(Tri.X, Tri.Y, Tri.Z));
		if (Near[Tri.X] || Near[Tri.Y] || Near[Tri.Z])
		{
			Membrane.AppendTriangle(FIndex3i(Tri.X, Tri.Y, Tri.Z));
		}
	}
	SkinTree = MakeUnique<FDynamicMeshAABBTree3>(&Skin, true);
	MembraneTree = MakeUnique<FDynamicMeshAABBTree3>(&Membrane, true);
	SkinShare.SetNumUninitialized(N);
	SkinGroup.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		SkinShare[i] = Region[i] ? 1.0 : 0.0;
		SkinGroup[i] = Region[i] ? Owner[i] : INDEX_NONE;
	}
	SkinBasePainted = MoveTemp(BasePainted);
	SkinBaseUnpainted = MoveTemp(BaseUnpainted);
	return true;
}

bool FSupport::Query(const FVector3d& RegionPoint, const FVector3d& DepthPoint, double MaxReach,
	double& OutShare, double& OutHeight, double& OutBasePainted, double& OutBaseUnpainted, int32* OutGroup) const
{
	OutShare = 0.0;
	if (!SkinTree.IsValid() || !MembraneTree.IsValid())
	{
		return false;
	}
	double DistanceSq = 0.0;
	const int32 Tri = SkinTree->FindNearestTriangle(RegionPoint, DistanceSq);
	if (Tri == IndexConstants::InvalidID || DistanceSq > MaxReach * MaxReach)
	{
		return false;
	}
	const FDistPoint3Triangle3d Closest = TMeshQueries<FDynamicMesh3>::TriangleDistance(Skin, Tri, RegionPoint);
	const FIndex3i Corners = Skin.GetTriangle(Tri);
	const FVector3d& Bary = Closest.TriangleBaryCoords;
	OutShare = Bary.X * SkinShare[Corners.A] + Bary.Y * SkinShare[Corners.B] + Bary.Z * SkinShare[Corners.C];
	if (OutShare <= 0.0)
	{
		OutShare = 0.0;
		return false;
	}
	OutShare = FMath::Min(OutShare, 1.0);
	if (OutGroup)
	{
		const int32 Best = Bary.X >= Bary.Y && Bary.X >= Bary.Z ? Corners.A : (Bary.Y >= Bary.Z ? Corners.B : Corners.C);
		*OutGroup = SkinGroup[Best] != INDEX_NONE ? SkinGroup[Best]
			: SkinGroup[Corners.A] != INDEX_NONE ? SkinGroup[Corners.A]
			: SkinGroup[Corners.B] != INDEX_NONE ? SkinGroup[Corners.B] : SkinGroup[Corners.C];
	}
	OutBasePainted = Bary.X * SkinBasePainted[Corners.A] + Bary.Y * SkinBasePainted[Corners.B] + Bary.Z * SkinBasePainted[Corners.C];
	OutBaseUnpainted = Bary.X * SkinBaseUnpainted[Corners.A] + Bary.Y * SkinBaseUnpainted[Corners.B]
		+ Bary.Z * SkinBaseUnpainted[Corners.C];
	double MembraneSq = 0.0;
	const int32 MembraneTri = MembraneTree->FindNearestTriangle(DepthPoint, MembraneSq);
	OutHeight = MembraneTri != IndexConstants::InvalidID ? FMath::Sqrt(MembraneSq) : 0.0;
	return true;
}

void FSupport::ExportMembrane(TArray<FVector3f>& OutVertices, TArray<int32>& OutTriangles, TArray<float>& OutHeights) const
{
	OutVertices.Reset();
	OutTriangles.Reset();
	OutHeights.Reset();
	if (!MembraneTree.IsValid())
	{
		return;
	}
	// 막은 살갗 정점을 모두 갖고 영역 둘레 삼각형만 쓴다 — 쓰는 정점만 앞에서부터 다시 센다.
	TMap<int32, int32> Compact;
	for (const int32 Tri : Membrane.TriangleIndicesItr())
	{
		const FIndex3i Corners = Membrane.GetTriangle(Tri);
		for (int32 k = 0; k < 3; ++k)
		{
			const int32 V = Corners[k];
			const int32* Found = Compact.Find(V);
			if (!Found)
			{
				Found = &Compact.Add(V, OutVertices.Num());
				OutVertices.Add(FVector3f(Membrane.GetVertex(V)));
				OutHeights.Add((float)FVector3d::Distance(Membrane.GetVertex(V), Skin.GetVertex(V)));
			}
			OutTriangles.Add(*Found);
		}
	}
}
}
