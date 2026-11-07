// Copyright (c) 2026 wjsekrud

#include "SoftbodyMeasure.h"

#include "Async/ParallelFor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Generate/IsosurfaceStuffing.h"
#include "MeshDescription.h"
#include "MeshQueries.h"
#include "SoftbodyContactResponse.h"
#include "SoftbodyLayerLinks.h"
#include "Spatial/FastWinding.h"
#include "StaticMeshAttributes.h"

#include <atomic>

THIRD_PARTY_INCLUDES_START
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
THIRD_PARTY_INCLUDES_END

using namespace UE::Geometry;

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyMeasure, Log, All);

namespace
{
	/** 3x3 역행렬 (row-major 9). 특이하면 단위 행렬 */
	void Invert3x3(const double* M, double* Out)
	{
		const double A = M[4] * M[8] - M[5] * M[7];
		const double B = M[5] * M[6] - M[3] * M[8];
		const double C = M[3] * M[7] - M[4] * M[6];
		const double Det = M[0] * A + M[1] * B + M[2] * C;
		if (FMath::Abs(Det) < 1e-30)
		{
			Out[0] = 1; Out[1] = 0; Out[2] = 0;
			Out[3] = 0; Out[4] = 1; Out[5] = 0;
			Out[6] = 0; Out[7] = 0; Out[8] = 1;
			return;
		}
		const double Inv = 1.0 / Det;
		Out[0] = A * Inv;
		Out[1] = (M[2] * M[7] - M[1] * M[8]) * Inv;
		Out[2] = (M[1] * M[5] - M[2] * M[4]) * Inv;
		Out[3] = B * Inv;
		Out[4] = (M[0] * M[8] - M[2] * M[6]) * Inv;
		Out[5] = (M[2] * M[3] - M[0] * M[5]) * Inv;
		Out[6] = C * Inv;
		Out[7] = (M[1] * M[6] - M[0] * M[7]) * Inv;
		Out[8] = (M[0] * M[4] - M[1] * M[3]) * Inv;
	}

	/** 3x3 블록 CSR 강성 행렬 */
	struct FBlockSparse
	{
		int32 NumNodes = 0;
		TArray<int32> RowPtr;
		TArray<int32> ColIdx;
		TArray<double> Blk; // 블록당 9 (row-major)
	};

	/** 임베디드 표면점: tet 4노드 + barycentric 가중 */
	struct FEmbed
	{
		int32 Nodes[4] = { INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };
		double W[4] = { 0, 0, 0, 0 };
		/** 외삽 한도 안에 든 tet이 없어 둘레에서 가장 가까운 tet의 가장자리 값으로 대신했다. */
		bool bNearest = false;
		bool IsValid() const { return Nodes[0] != INDEX_NONE; }
		double MinWeight() const
		{
			return IsValid()
				? FMath::Min(FMath::Min(W[0], W[1]), FMath::Min(W[2], W[3]))
				: -1.0;
		}
	};

	/** tet bbox 버킷 그리드 — 표면점의 포함 tet 탐색 */
	struct FTetLookup
	{
		double Cell = 1.0;
		FVector3d Min = FVector3d::ZeroVector;
		FIntVector Dims = FIntVector(1, 1, 1);
		TArray<TArray<int32>> Buckets;

		void Build(const TArray<FVector3d>& Verts, const TArray<FIntVector4>& Tets,
			const FVector3d& BoundsMin, const FVector3d& BoundsMax, double InCell)
		{
			Cell = InCell;
			Min = BoundsMin - FVector3d(Cell);
			const FVector3d Size = BoundsMax + FVector3d(Cell) - Min;
			Dims.X = FMath::Max(1, FMath::CeilToInt32(Size.X / Cell));
			Dims.Y = FMath::Max(1, FMath::CeilToInt32(Size.Y / Cell));
			Dims.Z = FMath::Max(1, FMath::CeilToInt32(Size.Z / Cell));
			Buckets.SetNum(Dims.X * Dims.Y * Dims.Z);

			for (int32 t = 0; t < Tets.Num(); ++t)
			{
				FVector3d Lo(TNumericLimits<double>::Max());
				FVector3d Hi(TNumericLimits<double>::Lowest());
				for (int32 k = 0; k < 4; ++k)
				{
					const FVector3d& P = Verts[Tets[t][k]];
					Lo = FVector3d::Min(Lo, P);
					Hi = FVector3d::Max(Hi, P);
				}
				const FIntVector A = CellOf(Lo);
				const FIntVector Bc = CellOf(Hi);
				for (int32 X = A.X; X <= Bc.X; ++X)
					for (int32 Y = A.Y; Y <= Bc.Y; ++Y)
						for (int32 Z = A.Z; Z <= Bc.Z; ++Z)
						{
							Buckets[(Z * Dims.Y + Y) * Dims.X + X].Add(t);
						}
			}
		}

		FIntVector CellOf(const FVector3d& P) const
		{
			return FIntVector(
				FMath::Clamp((int32)((P.X - Min.X) / Cell), 0, Dims.X - 1),
				FMath::Clamp((int32)((P.Y - Min.Y) / Cell), 0, Dims.Y - 1),
				FMath::Clamp((int32)((P.Z - Min.Z) / Cell), 0, Dims.Z - 1));
		}

		const TArray<int32>& CandidatesAt(const FVector3d& P) const
		{
			const FIntVector C = CellOf(P);
			return Buckets[(C.Z * Dims.Y + C.Y) * Dims.X + C.X];
		}

		/** P가 든 칸과 그 둘레 26칸의 tet. 같은 tet이 여러 번 나올 수 있다. */
		template <typename FuncType>
		void ForEachCandidateAround(const FVector3d& P, FuncType&& Func) const
		{
			const FIntVector C = CellOf(P);
			for (int32 Z = FMath::Max(C.Z - 1, 0); Z <= FMath::Min(C.Z + 1, Dims.Z - 1); ++Z)
				for (int32 Y = FMath::Max(C.Y - 1, 0); Y <= FMath::Min(C.Y + 1, Dims.Y - 1); ++Y)
					for (int32 X = FMath::Max(C.X - 1, 0); X <= FMath::Min(C.X + 1, Dims.X - 1); ++X)
					{
						for (const int32 t : Buckets[(Z * Dims.Y + Y) * Dims.X + X])
						{
							Func(t);
						}
					}
		}
	};

	bool ComputeBary(const TArray<FVector3d>& Verts, const FIntVector4& Tet, const FVector3d& P, double* OutW)
	{
		const FVector3d& X0 = Verts[Tet.X];
		const FVector3d C0 = Verts[Tet.Y] - X0;
		const FVector3d C1 = Verts[Tet.Z] - X0;
		const FVector3d C2 = Verts[Tet.W] - X0;
		const double Det = C0 | (C1 ^ C2);
		if (FMath::Abs(Det) < 1e-12)
		{
			return false;
		}
		const double InvDet = 1.0 / Det;
		const FVector3d D = P - X0;
		// 역기저(reciprocal basis) 행 = Dm⁻¹ 행
		const double B1 = ((C1 ^ C2) | D) * InvDet;
		const double B2 = ((C2 ^ C0) | D) * InvDet;
		const double B3 = ((C0 ^ C1) | D) * InvDet;
		OutW[0] = 1.0 - B1 - B2 - B3;
		OutW[1] = B1;
		OutW[2] = B2;
		OutW[3] = B3;
		return true;
	}

	FEmbed EmbedPoint(const TArray<FVector3d>& Verts, const TArray<FIntVector4>& Tets,
		const FTetLookup& Lookup, const FVector3d& P)
	{
		FEmbed Best;
		double BestMinW = -0.5; // 표면점은 워프된 tet 경계에서 살짝 벗어날 수 있어 느슨한 외삽 허용
		for (const int32 t : Lookup.CandidatesAt(P))
		{
			double W[4];
			if (!ComputeBary(Verts, Tets[t], P, W))
			{
				continue;
			}
			const double MinW = FMath::Min(FMath::Min(W[0], W[1]), FMath::Min(W[2], W[3]));
			if (MinW > BestMinW)
			{
				BestMinW = MinW;
				Best.Nodes[0] = Tets[t].X;
				Best.Nodes[1] = Tets[t].Y;
				Best.Nodes[2] = Tets[t].Z;
				Best.Nodes[3] = Tets[t].W;
				Best.W[0] = W[0]; Best.W[1] = W[1]; Best.W[2] = W[2]; Best.W[3] = W[3];
				if (MinW >= -1e-4)
				{
					break; // 실질적으로 포함 — 더 볼 필요 없음
				}
			}
		}
		if (Best.IsValid())
		{
			return Best;
		}

		// 한도 안에 든 tet이 없다. 격자 tet은 표면에 꼭 맞지 않아(경계가 반 칸까지 어긋난다) 표면 정점이 작은 경계 tet에서
		// 멀리 벗어나는 자리가 생긴다. 그대로 두면 그 정점만 무름이 0이라 출렁일 때 혼자 제자리에 남는다.
		// 둘레 칸까지 넓혀 가장 덜 벗어난 tet을 고르고, 무게를 tet 안으로 눌러(음수는 0, 합 1) 그 tet 가장자리의 값을 쓴다.
		// 멀리 외삽하지 않으므로 작은 tet에서도 값이 튀지 않는다.
		BestMinW = TNumericLimits<double>::Lowest();
		Lookup.ForEachCandidateAround(P, [&](int32 t)
		{
			double W[4];
			if (!ComputeBary(Verts, Tets[t], P, W))
			{
				return;
			}
			const double MinW = FMath::Min(FMath::Min(W[0], W[1]), FMath::Min(W[2], W[3]));
			if (MinW > BestMinW)
			{
				BestMinW = MinW;
				Best.Nodes[0] = Tets[t].X;
				Best.Nodes[1] = Tets[t].Y;
				Best.Nodes[2] = Tets[t].Z;
				Best.Nodes[3] = Tets[t].W;
				Best.W[0] = W[0]; Best.W[1] = W[1]; Best.W[2] = W[2]; Best.W[3] = W[3];
			}
		});
		if (Best.IsValid())
		{
			double Sum = 0.0;
			for (double& W : Best.W)
			{
				W = FMath::Max(W, 0.0);
				Sum += W;
			}
			for (double& W : Best.W)
			{
				W /= Sum;
			}
			Best.bNearest = true;
		}
		return Best;
	}

	/** y = K x */
	void ApplyOperator(const FBlockSparse& K, const TArray<double>& X, TArray<double>& Y)
	{
		const int32 N = K.NumNodes;
		Y.SetNumUninitialized(N * 3, EAllowShrinking::No);
		for (int32 Row = 0; Row < N; ++Row)
		{
			double S0 = 0, S1 = 0, S2 = 0;
			for (int32 b = K.RowPtr[Row]; b < K.RowPtr[Row + 1]; ++b)
			{
				const int32 Col3 = K.ColIdx[b] * 3;
				const double* M = &K.Blk[b * 9];
				const double X0 = X[Col3], X1 = X[Col3 + 1], X2 = X[Col3 + 2];
				S0 += M[0] * X0 + M[1] * X1 + M[2] * X2;
				S1 += M[3] * X0 + M[4] * X1 + M[5] * X2;
				S2 += M[6] * X0 + M[7] * X1 + M[8] * X2;
			}
			Y[Row * 3] = S0;
			Y[Row * 3 + 1] = S1;
			Y[Row * 3 + 2] = S2;
		}
	}

	double Dot(const TArray<double>& A, const TArray<double>& B)
	{
		double S = 0;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			S += A[i] * B[i];
		}
		return S;
	}

	/** 야코비(3x3 블록) 전처리 CG. X는 0 초기화 상태로 전달 */
	int32 SolveCG(const FBlockSparse& K,
		const TArray<double>& InvDiag /*노드당 9, 대각 블록 역행렬*/,
		const TArray<double>& F, TArray<double>& X, int32 MaxIter, double Tol)
	{
		const int32 Dof = K.NumNodes * 3;
		TArray<double> R = F;
		TArray<double> Z, P, Q;
		Z.SetNumUninitialized(Dof);
		auto Precond = [&](const TArray<double>& In, TArray<double>& Out)
		{
			Out.SetNumUninitialized(Dof, EAllowShrinking::No);
			for (int32 n = 0; n < K.NumNodes; ++n)
			{
				const double* M = &InvDiag[n * 9];
				const double R0 = In[n * 3], R1 = In[n * 3 + 1], R2 = In[n * 3 + 2];
				Out[n * 3] = M[0] * R0 + M[1] * R1 + M[2] * R2;
				Out[n * 3 + 1] = M[3] * R0 + M[4] * R1 + M[5] * R2;
				Out[n * 3 + 2] = M[6] * R0 + M[7] * R1 + M[8] * R2;
			}
		};

		const double FNorm = FMath::Sqrt(Dot(F, F));
		if (FNorm < 1e-30)
		{
			return 0;
		}
		Precond(R, Z);
		P = Z;
		double RZ = Dot(R, Z);
		int32 Iter = 0;
		for (; Iter < MaxIter; ++Iter)
		{
			ApplyOperator(K, P, Q);
			const double PQ = Dot(P, Q);
			if (FMath::Abs(PQ) < 1e-30)
			{
				break;
			}
			const double Alpha = RZ / PQ;
			for (int32 i = 0; i < Dof; ++i)
			{
				X[i] += Alpha * P[i];
				R[i] -= Alpha * Q[i];
			}
			if (FMath::Sqrt(Dot(R, R)) < Tol * FNorm)
			{
				break;
			}
			Precond(R, Z);
			const double RZNew = Dot(R, Z);
			const double Beta = RZNew / RZ;
			RZ = RZNew;
			for (int32 i = 0; i < Dof; ++i)
			{
				P[i] = Z[i] + Beta * P[i];
			}
		}
		return Iter;
	}

	/**
	 * K의 직접 솔버: sparse Cholesky(LDLT) 한 번 + 우변별 후방 대입. 세 방향 무름을 같은 K로 푸므로 CG보다 싸다.
	 * LDLT::solve는 분해가 불변이면 스레드 안전 — 방향별 ParallelFor 안에서 호출 가능.
	 */
	struct FDirectSolver
	{
		Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> LDLT;
		int32 Dof = 0;
		bool bReady = false;

		bool Build(const FBlockSparse& K)
		{
			Dof = K.NumNodes * 3;
			TArray<Eigen::Triplet<double>> Trip;
			Trip.Reserve(K.RowPtr[K.NumNodes] * 9);
			for (int32 n = 0; n < K.NumNodes; ++n)
			{
				for (int32 b = K.RowPtr[n]; b < K.RowPtr[n + 1]; ++b)
				{
					const int32 c = K.ColIdx[b];
					const double* M = &K.Blk[b * 9];
					for (int32 r = 0; r < 3; ++r)
					{
						for (int32 cc = 0; cc < 3; ++cc)
						{
							Trip.Emplace(n * 3 + r, c * 3 + cc, M[r * 3 + cc]);
						}
					}
				}
			}
			Eigen::SparseMatrix<double> A(Dof, Dof);
			A.setFromTriplets(Trip.GetData(), Trip.GetData() + Trip.Num());
			LDLT.compute(A);
			bReady = LDLT.info() == Eigen::Success;
			return bReady;
		}

		void Solve(const double* B, double* OutX) const
		{
			Eigen::Map<const Eigen::VectorXd> b(B, Dof);
			Eigen::Map<Eigen::VectorXd> X(OutX, Dof);
			X = LDLT.solve(b);
		}
	};
}

/**
 * SoftbodyMeasure.h의 FSoftbodyMeasureSolver: 무름 풀이의 앵커 고정 강성·분해와 살갗 표본 커널. 덩어리 원판 누름이 같은 격자를 같은
 * 분해로 다시 푼다. 힘은 표본 커널의 전치로 격자 점에 나눠 걸어, 정점에서 정점으로 가는 응답이 대칭이다(상반 정리).
 */
struct FSoftbodyMeasureSolver
{
	FBlockSparse AnchoredK;
	/** 격자 점마다 3×3 대각 블록의 역행렬(CG 전처리). */
	TArray<double> AnchoredInvDiag;
	/** 자유도가 작으면 직접 분해(LDLT). 준비되지 않았으면 CG로 푼다. */
	FDirectSolver Direct;
	TArray<FVector3d> Nodes;
	/** 고정한 격자 점(단단한 속, 뼈로 칠한 살, 코어와 이어지지 않은 살): 힘을 걸지 않는다. */
	TArray<uint8> AnchorNodes;
	double Cell = 0.0;
	/** MD 정점 ID → 같은 자리 정점 묶음. */
	TArray<int32> VertToUnique;
	TArray<FVector3d> UPos;
	/** 묶음이 살 격자에서 값을 읽는가: 살 속에 들었고 코어에 이어졌으며 둘레에 살 격자 점이 있다. */
	TArray<uint8> UUsable;
	/** 묶음 u의 표본 커널 = KernelOffsets[u] ~ [u + 1] 구간의 (격자 점, 무게). 무름 표본과 같은 무게(둘레 질량 × 감쇠 × 흐려짐 ÷ 무게 합). */
	TArray<int32> KernelOffsets;
	TArray<int32> KernelNodes;
	TArray<double> KernelWeights;
};

bool RunSoftbodyMeasure(const FMeshDescription& MD, FDynamicMesh3& DynMesh,
	const FDynamicMesh3* Solid, const FSoftbodyMeasureParams& Params, FSoftbodyMeasureOut& Out)
{
	const double StartTime = FPlatformTime::Seconds();

	FStaticMeshConstAttributes Attrs(MD);
	TVertexAttributesConstRef<FVector3f> Positions = Attrs.GetVertexPositions();
	const int32 MaxVertID = MD.Vertices().GetArraySize();

	// --- 솔리드화: winding number (비워터타이트 다중 셸에 강건, 숨은 내벽 공동 자동) ---
	// 안팎은 속 메시(있으면, ADR-0028)에서, 살갗(칠·살갗까지 거리)은 대상 메시(BVH)에서 읽는다.
	// 떠 있는 하드 서피스 조각(ADR-0037 §3.7)은 둘 다에서 뺀다 — 세 코너가 모두 조각 정점 자리인 삼각형. 조각이 없으면 대상 메시 그대로다.
	FDynamicMesh3 SkinWithoutPieces;
	if (Params.SkinSkipPositions.Num() > 0)
	{
		SkinWithoutPieces.Copy(DynMesh);
		int32 Removed = 0;
		for (const int32 Tri : DynMesh.TriangleIndicesItr())
		{
			const FIndex3i Corners = DynMesh.GetTriangle(Tri);
			bool bPiece = true;
			for (int32 k = 0; k < 3 && bPiece; ++k)
			{
				bPiece = Params.SkinSkipPositions.Contains(FVector3f(DynMesh.GetVertex(Corners[k])));
			}
			if (bPiece && SkinWithoutPieces.RemoveTriangle(Tri, true, false) == EMeshResult::Ok)
			{
				++Removed;
			}
		}
		UE_LOG(LogSoftbodyMeasure, Display, TEXT("Flex: %d triangles of floating plates left out of the skin and the flesh fill"), Removed);
	}
	const FDynamicMesh3& Skin = Params.SkinSkipPositions.Num() > 0 ? SkinWithoutPieces : DynMesh;
	FDynamicMeshAABBTree3 BVH(&Skin, true);
	const FDynamicMesh3& SolidMesh = Solid ? *Solid : Skin;
	FDynamicMeshAABBTree3 SolidBVH(&SolidMesh, true);
	TFastWindingTree<FDynamicMesh3> Winding(&SolidBVH, true);

	const FAxisAlignedBox3d Bounds = SolidMesh.GetBounds();
	const double Cell = FMath::Max(Bounds.MaxDim() * Params.CellSizeFrac, 0.01);

	const double MinRigidRadius = (double)Params.RigidMinRadiusCells * Cell;
	auto InRigid = [&Params, MinRigidRadius](const FVector3d& P) -> bool
	{
		for (const FVector4& S : Params.RigidSpheres)
		{
			const double Radius = FMath::Max((double)S.W, MinRigidRadius);
			if (FVector3d::DistSquared(P, FVector3d(S.X, S.Y, S.Z)) < Radius * Radius)
			{
				return true;
			}
		}
		return false;
	};

	// 살갗에 칠한 값을 살 속에서 읽기: 점 P에서 가장 가까운 살갗 점의 값. Jiggle(FSoftbodyMeasureParams::VertexJiggle), Volume(VertexVolume),
	// Depth(VertexDepth — 뼈 깊이)가 같은 방법을 쓴다. 칠이 없으면 어디서나 1.
	// DynMesh의 정점 번호는 MD와 다를 수 있어(논매니폴드 분할) 자리로 잇는다. 같은 자리의 정점 사본은 평균.
	const auto BuildSkinPaint = [&](const TArray<float>& VertexValues, TMap<FVector3f, FVector2f>& OutByPosition)
	{
		if (VertexValues.Num() != MaxVertID)
		{
			return false;
		}
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			OutByPosition.FindOrAdd(Positions[VertID], FVector2f::ZeroVector)
				+= FVector2f(FMath::Clamp(VertexValues[VertID], 0.f, 1.f), 1.f);
		}
		return true;
	};
	std::atomic<int32> FleshMisses{0};
	const auto SkinPaintAt = [&](const TMap<FVector3f, FVector2f>& ByPosition, const FVector3d& P) -> double
	{
		double SkinSq = 0.0;
		const int32 Tri = BVH.FindNearestTriangle(P, SkinSq);
		if (Tri == IndexConstants::InvalidID)
		{
			return 1.0;
		}
		const FDistPoint3Triangle3d Nearest = TMeshQueries<FDynamicMesh3>::TriangleDistance(Skin, Tri, P);
		const FIndex3i Corners = Skin.GetTriangle(Tri);
		double Value = 0.0;
		for (int32 k = 0; k < 3; ++k)
		{
			const FVector2f* Painted = ByPosition.Find(FVector3f(Skin.GetVertex(Corners[k])));
			FleshMisses += Painted ? 0 : 1;
			Value += Nearest.TriangleBaryCoords[k] * (Painted && Painted->Y > 0.f ? (double)(Painted->X / Painted->Y) : 1.0);
		}
		return Value;
	};
	TMap<FVector3f, FVector2f> FleshByPosition;
	const bool bFleshPainted = Params.VertexJiggle.ContainsByPredicate([](float Value) { return Value < 0.999f; })
		&& BuildSkinPaint(Params.VertexJiggle, FleshByPosition);
	auto FleshAt = [&](const FVector3d& P) -> double
	{
		return SkinPaintAt(FleshByPosition, P);
	};
	// Depth(뼈 깊이, ADR-0038 §3.2): 1보다 작게 칠한 정점이 있을 때만 읽는다.
	TMap<FVector3f, FVector2f> DepthByPosition;
	const bool bDepthPainted = Params.VertexDepth.ContainsByPredicate([](float Value) { return Value < 0.999f; })
		&& BuildSkinPaint(Params.VertexDepth, DepthByPosition);
	// 부피 보존 v = 저작 에셋 값 × 칠(ADR-0037 §3.3). 칠은 1보다 작게 칠한 정점이 있을 때만 읽는다. 값과 칠이 모두 1이면 어디서나 살의 푸아송 비다.
	TMap<FVector3f, FVector2f> VolumeByPosition;
	const bool bVolumePaintVaries = Params.VertexVolume.ContainsByPredicate([](float Value) { return Value < 0.999f; })
		&& BuildSkinPaint(Params.VertexVolume, VolumeByPosition);
	const double VolumeValue = FMath::Clamp((double)Params.VolumeValue, 0.0, 1.0);
	const bool bVolumePainted = bVolumePaintVaries || VolumeValue < 0.999;
	const double NuFlesh = SoftbodyContactResponse::PoissonOfVolume(1.f);
	/** 살갗의 부피 보존 v인 살의 푸아송 비(표면 장의 응답 표와 같은 곡선). */
	const auto PoissonOf = [](double Volume)
	{
		return (double)SoftbodyContactResponse::PoissonOfVolume((float)Volume);
	};
	/** 점 P의 부피 보존 v = 값 × 가장 가까운 살갗 점의 칠. */
	const auto VolumeAt = [&](const FVector3d& P)
	{
		return VolumeValue * (bVolumePaintVaries ? SkinPaintAt(VolumeByPosition, P) : 1.0);
	};

	// --- 격자 tet 생성 (IsosurfaceStuffing, conforming 아님 — 표면은 임베드) ---
	TIsosurfaceStuffing<double> Stuffing;
	const FVector3d Expand(Cell * 2.0);
	Stuffing.Bounds = UE::Math::TBox<double>(Bounds.Min - Expand, Bounds.Max + Expand);
	Stuffing.CellSize = Cell;
	Stuffing.IsoValue = 0.0;
	Stuffing.Implicit = [&Winding](FVector3d P) { return Winding.FastWindingNumber(P) - 0.5; };
	Stuffing.Generate();

	const TArray<FVector3d>& Nodes = Stuffing.Vertices;
	const TArray<FIntVector4>& Tets = Stuffing.Tets;
	Out.NumNodes = Nodes.Num();
	Out.NumTets = Tets.Num();
	if (Tets.Num() == 0)
	{
		UE_LOG(LogSoftbodyMeasure, Error,
			TEXT("Could not measure the flesh: the mesh encloses no volume (cell %.2f). If the mesh is not closed (a single-layer sheet or an open tube), set Volume Mesh on the authoring asset to a filled mesh in the same place."),
			Cell);
		return false;
	}

	// --- 선형 FEM 조립 ---
	// 요소는 정변형 사면체(사면체 안에서 변형이 하나)다. 다만 부피 변형(눌려 부피가 줄거나 느는 몫)은 사면체마다가 아니라 격자 점
	// 둘레 사면체들의 부피 평균으로 잰다(Bonet & Burton 1998의 평균 절점 압력 요소). 실제 살처럼 거의 압축되지 않는 재료(푸아송 비
	// 0.49)를 사면체마다 부피를 지키게 풀면 격자가 굳어(잠김) 밀리는 양이 몇 배 줄어드는데, 둘레 평균으로 재면 그 구속이 느슨해져
	// 굳지 않는다(ADR-0005 §8: 바닥을 고정한 판 모형에서 0.499에서도 밀리는 양이 0.35의 절반 안, 옆으로 미는 응답은 그대로).
	//   에너지 = Σ_사면체 V·μ·(ε:ε − tr(ε)²/3)  +  Σ_격자점 ½·κ_n·V_n·θ_n²
	//   θ_n = Σ_{n이 든 사면체} (V/4)·tr(ε) ÷ V_n,   V_n = Σ (V/4),   κ_n = 그 사면체들의 부피 강성(λ + 2μ/3)의 부피 평균
	// 푸아송 비는 사면체마다 그 살의 부피 보존 칠에서 정한다(PoissonOf): 살로 칠한 곳은 거의 압축되지 않고, 폼으로 칠한 곳은 눌린
	// 자리에서 부피가 준다. 격자 점 둘레의 부피 강성 κ_n이 둘레 사면체의 평균이라, 칠이 바뀌는 경계에서도 요소는 그대로다.
	const double VolEps = Cell * Cell * Cell * 1e-6;
	TArray<TMap<int32, int32>> RowBlocks;
	RowBlocks.SetNum(Nodes.Num());
	TArray<double> BlockData;
	auto AddToBlock = [&RowBlocks, &BlockData](int32 RowNode, int32 ColNode, const double* M9)
	{
		int32* Idx = RowBlocks[RowNode].Find(ColNode);
		if (!Idx)
		{
			Idx = &RowBlocks[RowNode].Add(ColNode, BlockData.Num() / 9);
			BlockData.AddZeroed(9);
		}
		double* Dst = &BlockData[*Idx * 9];
		for (int32 k = 0; k < 9; ++k)
		{
			Dst[k] += M9[k];
		}
	};

	// 부피 보존 칠: 사면체는 가운데 점의 값으로 푸아송 비를, 격자 점은 제 자리의 값으로 뼈대에 매다는 스프링의 세기를 정한다.
	TArray<float> TetPoisson;
	TArray<float> NodePoisson;
	int32 NumFoamTets = 0;
	if (bVolumePainted)
	{
		TetPoisson.SetNumUninitialized(Tets.Num());
		ParallelFor(Tets.Num(), [&](int32 t)
		{
			const FIntVector4& Tet = Tets[t];
			TetPoisson[t] = (float)PoissonOf(VolumeAt((Nodes[Tet.X] + Nodes[Tet.Y] + Nodes[Tet.Z] + Nodes[Tet.W]) * 0.25));
		});
		NodePoisson.SetNumUninitialized(Nodes.Num());
		ParallelFor(Nodes.Num(), [&](int32 n)
		{
			NodePoisson[n] = (float)PoissonOf(VolumeAt(Nodes[n]));
		});
	}

	// Jiggle 칠: 사면체는 가운데 점의 값으로 강성을, 격자 점은 제 자리의 값으로 Flesh Reach 스프링의 세기를 정한다.
	TArray<float> TetFlesh;
	TArray<float> NodeFlesh;
	int32 NumFirmTets = 0;
	if (bFleshPainted)
	{
		TetFlesh.SetNumUninitialized(Tets.Num());
		ParallelFor(Tets.Num(), [&](int32 t)
		{
			const FIntVector4& Tet = Tets[t];
			TetFlesh[t] = (float)FleshAt((Nodes[Tet.X] + Nodes[Tet.Y] + Nodes[Tet.Z] + Nodes[Tet.W]) * 0.25);
		});
		NodeFlesh.SetNumUninitialized(Nodes.Num());
		ParallelFor(Nodes.Num(), [&](int32 n)
		{
			NodeFlesh[n] = (float)FleshAt(Nodes[n]);
		});
	}

	int32 NumRigidTets = 0;
	TArray<double> NodeMass;
	NodeMass.Init(0.0, Nodes.Num());
	// 격자 점마다: 둘레 부피 V_n, 둘레 부피 강성의 합 Σ (V/4)·κ, 둘레 평균 부피 변형이 이웃 점의 이동에 따라 변하는 정도 Σ (V/4)·∇N_a
	// (V_n으로 나누기 전).
	TArray<double> NodePatchVolume;
	TArray<double> NodePatchBulk;
	TArray<TMap<int32, FVector3d>> NodePatchDilation;
	NodePatchVolume.Init(0.0, Nodes.Num());
	NodePatchBulk.Init(0.0, Nodes.Num());
	NodePatchDilation.SetNum(Nodes.Num());
	for (int32 TetIndex = 0; TetIndex < Tets.Num(); ++TetIndex)
	{
		const FIntVector4& Tet = Tets[TetIndex];
		const FVector3d& X0 = Nodes[Tet.X];
		const FVector3d C0 = Nodes[Tet.Y] - X0;
		const FVector3d C1 = Nodes[Tet.Z] - X0;
		const FVector3d C2 = Nodes[Tet.W] - X0;
		const double Det = C0 | (C1 ^ C2);
		const double Vol = FMath::Abs(Det) / 6.0;
		if (Vol < VolEps)
		{
			continue; // 스터핑 슬리버 tet — 강성 기여 무시
		}
		const double InvDet = 1.0 / Det;
		FVector3d G[4];
		G[1] = (C1 ^ C2) * InvDet;
		G[2] = (C2 ^ C0) * InvDet;
		G[3] = (C0 ^ C1) * InvDet;
		G[0] = -(G[1] + G[2] + G[3]);

		const FVector3d Centroid = X0 + (C0 + C1 + C2) * 0.25;
		const bool bRigid = InRigid(Centroid);
		NumRigidTets += bRigid ? 1 : 0;
		// 살의 강성 = 1 ÷ 칠한 말랑함(뼈보다 단단해지지는 않는다). 칠이 없으면 1.
		const double Firmness = bFleshPainted
			? FMath::Min(1.0 / FMath::Max((double)TetFlesh[TetIndex], 1e-6), (double)Params.RigidStiffnessMul)
			: 1.0;
		NumFirmTets += !bRigid && Firmness > 1.0 + 1e-3 ? 1 : 0;
		const double E = bRigid ? (double)Params.RigidStiffnessMul : Firmness;
		const double Nu = bVolumePainted ? (double)TetPoisson[TetIndex] : NuFlesh;
		NumFoamTets += !bRigid && Nu < NuFlesh - 1e-3 ? 1 : 0;
		const double Lambda = E * Nu / ((1.0 + Nu) * (1.0 - 2.0 * Nu));
		const double Mu = E / (2.0 * (1.0 + Nu));
		const double Kappa = Lambda + 2.0 * Mu / 3.0;

		const int32 TetNodes[4] = { Tet.X, Tet.Y, Tet.Z, Tet.W };
		for (int32 a = 0; a < 4; ++a)
		{
			NodeMass[TetNodes[a]] += Vol * 0.25;
			NodePatchVolume[TetNodes[a]] += Vol * 0.25;
			NodePatchBulk[TetNodes[a]] += Vol * 0.25 * Kappa;
			TMap<int32, FVector3d>& Patch = NodePatchDilation[TetNodes[a]];
			for (int32 b = 0; b < 4; ++b)
			{
				Patch.FindOrAdd(TetNodes[b], FVector3d::ZeroVector) += G[b] * (Vol * 0.25);
			}
		}
		for (int32 a = 0; a < 4; ++a)
		{
			for (int32 b = 0; b < 4; ++b)
			{
				// 모양 변형(부피를 빼고 남은 것)만 사면체마다: K_ab = V·(μ ∇Nb∇Naᵀ + μ (∇Na·∇Nb) I − (2μ/3) ∇Na∇Nbᵀ).
				// 부피 변형은 아래에서 격자 점 둘레 평균으로 더한다.
				const double GaGb = G[a] | G[b];
				double M[9];
				for (int32 r = 0; r < 3; ++r)
				{
					for (int32 c = 0; c < 3; ++c)
					{
						M[r * 3 + c] = Vol * (Mu * G[b][r] * G[a][c] - (2.0 * Mu / 3.0) * G[a][r] * G[b][c]);
					}
					M[r * 3 + r] += Vol * Mu * GaGb;
				}
				AddToBlock(TetNodes[a], TetNodes[b], M);
			}
		}
	}
	// 부피 변형: 격자 점 n마다 ½·κ_n·V_n·θ_n², θ_n = Σ (V/4)·∇N_a·u_a ÷ V_n → K_ac += (κ_n/V_n)·(Σ(V/4)∇N_a)(Σ(V/4)∇N_c)ᵀ ÷ V_n.
	// 둘레의 점끼리 모두 이어지므로 행렬이 두 겹 이웃까지 넓어진다(사면체마다일 때의 4배쯤).
	for (int32 n = 0; n < Nodes.Num(); ++n)
	{
		if (NodePatchVolume[n] <= 0.0)
		{
			continue;
		}
		const double Weight = NodePatchBulk[n] / (NodePatchVolume[n] * NodePatchVolume[n]);
		for (const TPair<int32, FVector3d>& A : NodePatchDilation[n])
		{
			for (const TPair<int32, FVector3d>& C : NodePatchDilation[n])
			{
				double M[9];
				for (int32 r = 0; r < 3; ++r)
				{
					for (int32 c = 0; c < 3; ++c)
					{
						M[r * 3 + c] = Weight * A.Value[r] * C.Value[c];
					}
				}
				AddToBlock(A.Key, C.Key, M);
			}
		}
	}
	NodePatchDilation.Empty();

	// 앵커 스프링: 소프트 노드는 CG 가해성(SPD)을 위한 수치적 접지만 — 강하면 탄성 기반의
	// 감쇠 길이가 스커트 반경 수준으로 내려와 원거리 응답을 잘라먹고 내부 구조 차이를 가린다
	// (실측: 0.05·cell에서 솔리드/속빔 sN이 0.056/0.061로 수렴). 강체 모드는 솔브 후 명시적 제거.
	// 강체 프리미티브 내부 노드는 스켈레톤이 든다는 의미로 접지 수준.
	const double KaSoft = 0.001 * Cell;
	const double KaRigid = (double)Params.RigidStiffnessMul * Cell;
	// Flesh Reach가 0보다 크면 살을 뼈대에 스프링으로도 매단다(FSoftbodyMeasureParams::FleshReach). 살이 그보다 두꺼운 곳은 더
	// 두꺼워져도 더 밀리지 않아, 밀리는 양이 깊이가 아니라 칠한 말랑함을 따른다(얇은 살은 전처럼 두께만큼 덜 밀린다). 스프링도
	// 살의 강성과 같이 칠에 반비례해 단단해지므로, 두꺼운 살이 밀리는 양은 칠한 값에 비례한다.
	const double FleshReach = Params.FleshReach;
	const bool bFleshSpring = FleshReach > 0.0;
	for (int32 n = 0; n < Nodes.Num(); ++n)
	{
		double Ka = InRigid(Nodes[n]) ? KaRigid : KaSoft;
		if (bFleshSpring && Ka == KaSoft)
		{
			// 스프링의 세기 = 그 자리 살의 전단 강성 ÷ 두께²(전단 강성 = 살 단단함 ÷ (2 × (1 + 푸아송 비))).
			const double Nu = bVolumePainted ? (double)NodePoisson[n] : NuFlesh;
			const double FleshSpring = 1.0 / (2.0 * (1.0 + Nu)) / (FleshReach * FleshReach);
			const double Flesh = bFleshPainted ? (double)NodeFlesh[n] : 1.0;
			Ka += NodeMass[n] * FleshSpring * FMath::Min(1.0 / FMath::Max(Flesh, 1e-6), (double)Params.RigidStiffnessMul);
		}
		double M[9] = { Ka, 0, 0, 0, Ka, 0, 0, 0, Ka };
		AddToBlock(n, n, M);
	}

	// CSR 변환
	FBlockSparse K;
	K.NumNodes = Nodes.Num();
	K.RowPtr.SetNumUninitialized(K.NumNodes + 1);
	int32 TotalBlocks = 0;
	for (int32 n = 0; n < K.NumNodes; ++n)
	{
		K.RowPtr[n] = TotalBlocks;
		TotalBlocks += RowBlocks[n].Num();
	}
	K.RowPtr[K.NumNodes] = TotalBlocks;
	K.ColIdx.SetNumUninitialized(TotalBlocks);
	K.Blk.SetNumUninitialized(TotalBlocks * 9);
	{
		int32 Cursor = 0;
		for (int32 n = 0; n < K.NumNodes; ++n)
		{
			RowBlocks[n].KeySort(TLess<int32>());
			for (const TPair<int32, int32>& Pair : RowBlocks[n])
			{
				K.ColIdx[Cursor] = Pair.Key;
				FMemory::Memcpy(&K.Blk[Cursor * 9], &BlockData[Pair.Value * 9], 9 * sizeof(double));
				++Cursor;
			}
		}
	}
	RowBlocks.Empty();
	BlockData.Empty();

	// --- 표면 정점 용접(위치 정확 일치) + 임베딩 ---
	FTetLookup Lookup;
	Lookup.Build(Nodes, Tets, Bounds.Min - Expand, Bounds.Max + Expand, Cell);

	TMap<FVector3f, int32> PosToUnique;
	TArray<FVector3d> UPos;
	TArray<TArray<int32>> UMembers;
	TArray<int32> VertToUnique;
	VertToUnique.Init(INDEX_NONE, MaxVertID);
	for (const FVertexID VertID : MD.Vertices().GetElementIDs())
	{
		const FVector3f P = Positions[VertID];
		int32* Existing = PosToUnique.Find(P);
		if (!Existing)
		{
			Existing = &PosToUnique.Add(P, UPos.Num());
			UPos.Add(FVector3d(P));
			UMembers.AddDefaulted();
		}
		UMembers[*Existing].Add(VertID);
		VertToUnique[VertID] = *Existing;
	}
	const int32 NumUnique = UPos.Num();

	TArray<FEmbed> UEmbed;
	UEmbed.SetNum(NumUnique);
	int32 NumEmbedded = 0;
	int32 NumContained = 0;
	int32 NumExtrapolated = 0;
	int32 NumNearest = 0;
	for (int32 u = 0; u < NumUnique; ++u)
	{
		UEmbed[u] = EmbedPoint(Nodes, Tets, Lookup, UPos[u]);
		if (UEmbed[u].IsValid())
		{
			++NumEmbedded;
			if (UEmbed[u].bNearest)
			{
				++NumNearest;
			}
			else if (UEmbed[u].MinWeight() >= -1e-4)
			{
				++NumContained;
			}
			else
			{
				++NumExtrapolated;
			}
		}
	}
	TArray<uint8> UCoreConnection;
	UCoreConnection.Init(0, NumUnique);
	UE_LOG(LogSoftbodyMeasure, Display,
		TEXT("Measure setup: cell %.2f, %d tetrahedra (%d rigid), %d grid points, Poisson ratio %.2f for flesh (volume change averaged around grid points; lowered toward foam "
			"by Volume Preservation %.2f x paint in %d tetrahedra) | surface vertices: inside %d, extrapolated %d, nearest tetrahedron used %d, failed %d / unique vertices %d"),
		Cell, Out.NumTets, NumRigidTets, Out.NumNodes, NuFlesh, VolumeValue, NumFoamTets, NumContained, NumExtrapolated, NumNearest,
		NumUnique - NumEmbedded, NumUnique);

	// --- [ADR-0005] 살의 무름 3방향 ---
	// 강체 프리미티브 내부 노드를 고정(Dirichlet)하고, 나머지 노드에 +X/+Y/+Z 방향의 질량 비례
	// 체적력을 걸어 정적 변형을 푼다. 런타임은 이 세 모양에 "그 자리 살이 받는 가속·중력 변화"를
	// 곱해 흔들림을 만든다. 앵커가 없으면 중심/피벗을 추정하지 않고 생략한다.
	if (Params.bBakeInertiaResponses && Params.RigidSpheres.Num() > 0)
	{
		TArray<uint8> AnchorNodes;
		AnchorNodes.Init(0, Nodes.Num());
		double AnchorMass = 0.0;
		FVector3d AnchorOrigin = FVector3d::ZeroVector;
		int32 AnchorNodeCount = 0;
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			if (InRigid(Nodes[n]))
			{
				AnchorNodes[n] = 1;
				++AnchorNodeCount;
				AnchorMass += NodeMass[n];
				AnchorOrigin += NodeMass[n] * Nodes[n];
			}
		}
		if (AnchorNodeCount < 4 || AnchorMass <= UE_DOUBLE_SMALL_NUMBER)
		{
			UE_LOG(LogSoftbodyMeasure, Error,
				TEXT("Inertia bake failed: the rigid core holds too few grid points to anchor the flesh "
					"(%d points, mass %.6g). Make the rigid core larger (Bone Thickness, Rigid Core Spheres) or lower Measure Cell Size."),
				AnchorNodeCount, AnchorMass);
			return false;
		}
		AnchorOrigin /= AnchorMass;

		// 코어와 연결되지 않은 체적 성분은 드라이버 상대 관성이 정의되지 않는다.
		// 작은 접지 스프링으로 임의 운동을 만들지 않고 해당 성분 전체를 0 응답으로 둔다.
		// 의상/부속물 이름을 알 필요 없이 tet 연결성 하나로 결정한다.
		TArray<int32> ComponentParent;
		ComponentParent.SetNumUninitialized(Nodes.Num());
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			ComponentParent[n] = n;
		}
		TFunction<int32(int32)> FindComponent = [&ComponentParent, &FindComponent](int32 Node)
		{
			if (ComponentParent[Node] != Node)
			{
				ComponentParent[Node] = FindComponent(ComponentParent[Node]);
			}
			return ComponentParent[Node];
		};
		auto MergeComponent = [&ComponentParent, &FindComponent](int32 A, int32 B)
		{
			A = FindComponent(A);
			B = FindComponent(B);
			if (A != B)
			{
				ComponentParent[B] = A;
			}
		};
		for (const FIntVector4& Tet : Tets)
		{
			const FVector3d& X0 = Nodes[Tet.X];
			const double Vol = FMath::Abs(
				(Nodes[Tet.Y] - X0)
				| ((Nodes[Tet.Z] - X0) ^ (Nodes[Tet.W] - X0))) / 6.0;
			if (Vol < VolEps)
			{
				continue;
			}
			MergeComponent(Tet.X, Tet.Y);
			MergeComponent(Tet.X, Tet.Z);
			MergeComponent(Tet.X, Tet.W);
		}
		TArray<uint8> ComponentHasCore;
		ComponentHasCore.Init(0, Nodes.Num());
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			if (AnchorNodes[n])
			{
				ComponentHasCore[FindComponent(n)] = 1;
			}
		}
		int32 NumCoreDisconnectedSurface = 0;
		for (int32 u = 0; u < NumUnique; ++u)
		{
			if (!UEmbed[u].IsValid())
			{
				continue;
			}
			const bool bConnected =
				ComponentHasCore[FindComponent(UEmbed[u].Nodes[0])] != 0;
			UCoreConnection[u] = bConnected ? 1 : 2;
			NumCoreDisconnectedSurface += bConnected ? 0 : 1;
		}
		int32 NumInactiveNodes = 0;
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			if (!ComponentHasCore[FindComponent(n)])
			{
				AnchorNodes[n] = 1;
				++NumInactiveNodes;
			}
		}
		if (NumInactiveNodes > 0)
		{
			UE_LOG(LogSoftbodyMeasure, Display,
				TEXT("Inertia: %d grid points not connected to the rigid core will not jiggle "
					"(%d unique surface vertices affected)"),
				NumInactiveNodes, NumCoreDisconnectedSurface);
		}

		// Depth 칠(ADR-0038 §3.2): 살갗 점 S의 칠이 p, S에서 단단한 속 겉까지 거리가 D면, S가 가장 가까운 살갗인 격자 점 가운데 깊이 d ≥ p·D인 것은
		// 뼈다 — 단단한 속과 같이 고정한다. p = 1이면 속까지 살이라 고정이 없고, p ≤ 0.01이면 살갗 바로 밑부터 뼈(옛 Flesh Softness 0과 같다).
		// 접촉 지도도 같은 칠로 살 두께(속까지 거리 × p)를 잰다 — 출렁임과 눌림이 같은 뼈를 본다.
		if (bDepthPainted)
		{
			const auto CoreDistance = [&Params, MinRigidRadius](const FVector3d& P)
			{
				double Best = TNumericLimits<double>::Max();
				for (const FVector4& S : Params.RigidSpheres)
				{
					const double Radius = FMath::Max((double)S.W, MinRigidRadius);
					Best = FMath::Min(Best, FVector3d::Dist(P, FVector3d(S.X, S.Y, S.Z)) - Radius);
				}
				return FMath::Max(Best, 0.0);
			};
			TArray<uint8> DepthBone;
			DepthBone.Init(0, Nodes.Num());
			ParallelFor(Nodes.Num(), [&](int32 n)
			{
				if (AnchorNodes[n] || NodeMass[n] <= 0.0)
				{
					return;
				}
				double SkinSq = 0.0;
				const int32 Tri = BVH.FindNearestTriangle(Nodes[n], SkinSq);
				if (Tri == IndexConstants::InvalidID)
				{
					return;
				}
				const FDistPoint3Triangle3d Nearest = TMeshQueries<FDynamicMesh3>::TriangleDistance(Skin, Tri, Nodes[n]);
				const double Paint = SkinPaintAt(DepthByPosition, Nodes[n]);
				if (Paint >= 0.999)
				{
					return;
				}
				const double Depth = FMath::Sqrt(SkinSq);
				const double ToCore = Params.RigidSpheres.Num() > 0 ? CoreDistance(Nearest.ClosestTrianglePoint) : 0.0;
				DepthBone[n] = Depth >= Paint * ToCore || Paint <= 0.01 ? 1 : 0;
			});
			int32 NumBoneNodes = 0;
			for (int32 n = 0; n < Nodes.Num(); ++n)
			{
				if (DepthBone[n])
				{
					AnchorNodes[n] = 1;
					++NumBoneNodes;
				}
			}
			UE_LOG(LogSoftbodyMeasure, Display,
				TEXT("Depth paint: %d grid points fixed as bone (on top of %d inside the skeleton, %d total), skin vertices that could not read the paint %d (should be 0)"),
				NumBoneNodes, AnchorNodeCount, Nodes.Num(), FleshMisses.load());
		}
		if (bFleshPainted)
		{
			double FleshSum = 0.0;
			for (const float Value : NodeFlesh)
			{
				FleshSum += Value;
			}
			UE_LOG(LogSoftbodyMeasure, Display,
				TEXT("Jiggle paint: stiffened tetrahedra %d / %d, grid point mean %.3f, Flesh Reach %.1f cm, skin vertices that could not read the paint %d (should be 0)"),
				NumFirmTets, Tets.Num(), FleshSum / FMath::Max(Nodes.Num(), 1), FleshReach, FleshMisses.load());
		}

		// 덩어리 원판 누름(SolveSoftbodyLumpDiscs)이 같은 분해를 다시 쓰도록 앵커 고정 강성과 분해를 여기에 만들어 굽기 끝까지 쥔다.
		TSharedPtr<FSoftbodyMeasureSolver> Solver = MakeShared<FSoftbodyMeasureSolver>();

		// 기존 측정 연산자의 아주 작은 전역 대각항(KaSoft)은 유지한다.
		// 물리 코어가 보이는 앵커를 정하고, 이 에셋 독립적인 항은 얇거나 약하게 연결된
		// 체적 영역의 수치 조건만 안정화한다.
		FBlockSparse& AnchoredK = Solver->AnchoredK;
		// 옮긴다: K는 이 뒤에 쓰지 않는다. 복사하면 굽기 끝까지 같은 크기가 두 벌이 된다(노드당 ~4 KB).
		AnchoredK = MoveTemp(K);
		const double AnchorDiagonal = FMath::Max(Cell, 1e-6);
		for (int32 Row = 0; Row < AnchoredK.NumNodes; ++Row)
		{
			const bool bAnchorRow = AnchorNodes[Row] != 0;
			for (int32 b = AnchoredK.RowPtr[Row]; b < AnchoredK.RowPtr[Row + 1]; ++b)
			{
				const int32 Col = AnchoredK.ColIdx[b];
				double* M = &AnchoredK.Blk[b * 9];
				if (bAnchorRow)
				{
					FMemory::Memzero(M, 9 * sizeof(double));
					if (Col == Row)
					{
						M[0] = M[4] = M[8] = AnchorDiagonal;
					}
				}
				else if (AnchorNodes[Col])
				{
					FMemory::Memzero(M, 9 * sizeof(double));
				}
			}
		}

		TArray<double>& AnchoredInvDiag = Solver->AnchoredInvDiag;
		AnchoredInvDiag.SetNumUninitialized(AnchoredK.NumNodes * 9);
		for (int32 n = 0; n < AnchoredK.NumNodes; ++n)
		{
			bool bFoundDiagonal = false;
			for (int32 b = AnchoredK.RowPtr[n]; b < AnchoredK.RowPtr[n + 1]; ++b)
			{
				if (AnchoredK.ColIdx[b] == n)
				{
					Invert3x3(&AnchoredK.Blk[b * 9], &AnchoredInvDiag[n * 9]);
					bFoundDiagonal = true;
					break;
				}
			}
			if (!bFoundDiagonal)
			{
				UE_LOG(LogSoftbodyMeasure, Error,
					TEXT("Inertia bake failed: missing diagonal block in the anchored solve (grid point %d)"), n);
				return false;
			}
		}

		constexpr int32 NumResponses = 3;
		const int32 Dof = Nodes.Num() * 3;
		TArray<TArray<double>> Forces;
		TArray<TArray<double>> Solutions;
		Forces.SetNum(NumResponses);
		Solutions.SetNum(NumResponses);
		for (int32 Response = 0; Response < NumResponses; ++Response)
		{
			Forces[Response].SetNumZeroed(Dof);
			Solutions[Response].SetNumZeroed(Dof);
			for (int32 n = 0; n < Nodes.Num(); ++n)
			{
				if (AnchorNodes[n] || NodeMass[n] <= 0.0)
				{
					continue;
				}
				// 방향 Response로 당기는 체적력(중력처럼 질량에 비례). 가속 a는 체적력 −a와 같다.
				Forces[Response][n * 3 + Response] = NodeMass[n];
			}
		}

		FDirectSolver& AnchoredDirect = Solver->Direct;
		constexpr int32 MaxAnchoredDirectDof = 50000;
		if (Dof <= MaxAnchoredDirectDof && AnchoredDirect.Build(AnchoredK))
		{
			UE_LOG(LogSoftbodyMeasure, Display,
				TEXT("Inertia solve: direct factorization (LDLT), dof %d"), Dof);
		}
		TArray<int32> ResponseIterations;
		TArray<double> ResponseResiduals;
		ResponseIterations.SetNumZeroed(NumResponses);
		ResponseResiduals.SetNumZeroed(NumResponses);
		ParallelFor(NumResponses, [&](int32 Response)
		{
			if (AnchoredDirect.bReady)
			{
				AnchoredDirect.Solve(
					Forces[Response].GetData(), Solutions[Response].GetData());
				ResponseIterations[Response] = 0;
			}
			else
			{
				ResponseIterations[Response] = SolveCG(
					AnchoredK, AnchoredInvDiag, Forces[Response], Solutions[Response],
					8000, 1e-8);
			}
			TArray<double> AX;
			ApplyOperator(AnchoredK, Solutions[Response], AX);
			double ResidualSq = 0.0;
			double ForceSq = 0.0;
			for (int32 i = 0; i < Dof; ++i)
			{
				const double R = Forces[Response][i] - AX[i];
				ResidualSq += R * R;
				ForceSq += Forces[Response][i] * Forces[Response][i];
			}
			ResponseResiduals[Response] = FMath::Sqrt(ResidualSq / FMath::Max(ForceSq, 1e-30));
		});

		double MaxResidual = 0.0;
		double MaxAnchorError = 0.0;
		for (int32 Response = 0; Response < NumResponses; ++Response)
		{
			MaxResidual = FMath::Max(MaxResidual, ResponseResiduals[Response]);
			for (int32 n = 0; n < Nodes.Num(); ++n)
			{
				if (AnchorNodes[n])
				{
					const int32 N3 = n * 3;
					MaxAnchorError = FMath::Max(MaxAnchorError,
						FVector3d(Solutions[Response][N3], Solutions[Response][N3 + 1],
							Solutions[Response][N3 + 2]).Length());
				}
			}
		}
		if (!FMath::IsFinite(MaxResidual) || MaxResidual > 1e-4)
		{
			UE_LOG(LogSoftbodyMeasure, Error,
				TEXT("Inertia bake failed: the solve did not converge (relative residual %.3e > 1e-4)"), MaxResidual);
			return false;
		}

		Out.InertiaResponseVersion = 2;
		Out.NumInertiaResponses = NumResponses;
		Out.InertiaAnchorLocal = FVector3f(AnchorOrigin);
		Out.InertiaAnchorNodeCount = AnchorNodeCount;
		Out.InertiaMaxAnchorError = (float)MaxAnchorError;
		Out.InertiaMaxRelativeResidual = (float)MaxResidual;
		Out.InertiaResponseScales.Init(0.f, NumResponses);
		Out.VertexInertiaResponseShapes.Init(
			FVector3f::ZeroVector, NumResponses * MaxVertID);

		// 표면 정점의 무름 = 그 점 둘레(반지름 1.5셀) 살 노드 변형의 질량 가중 평균. 정점이 속한 사면체
		// 하나를 보간(밖이면 바깥으로 늘림)하면 사면체가 바뀌는 곳마다 값이 튀고, 뼈 가까이에서 늘리면
		// 부호까지 뒤집혀 흔들릴 때 반점이 된다. 둘레 평균은 정점이 움직이면 값도 연속으로 움직이고
		// 노드 값 범위를 넘지 않는다. 살에서 2셀 넘게 떨어진 정점(떠 있는 카드 등)은 0으로 사라진다.
		// 강체 운동은 빼지 않는다 — 앵커가 뼈를 따라가므로 이 변형은 이미 "뼈에 대해 살이 밀리는 양"이다.
		// 세 방향은 가장 크게 밀리는 값 하나로 함께 나눠 방향 사이 비율을 지킨다.
		const double KernelRadius = 1.5 * Cell;
		const double KernelRadiusSq = KernelRadius * KernelRadius;
		auto KernelCell = [KernelRadius](const FVector3d& P)
		{
			return FIntVector(FMath::FloorToInt32(P.X / KernelRadius), FMath::FloorToInt32(P.Y / KernelRadius),
				FMath::FloorToInt32(P.Z / KernelRadius));
		};
		TMultiMap<FIntVector, int32> NodeGrid;
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			if (NodeMass[n] > 0.0)
			{
				NodeGrid.Add(KernelCell(Nodes[n]), n);
			}
		}

		TArray<FVector3d> SurfaceFields;
		SurfaceFields.Init(FVector3d::ZeroVector, NumResponses * NumUnique);
		double SharedMax = 0.0;
		double Reach = 0.0;
		int32 NumFaded = 0;
		TArray<int32> Nearby;
		// 정점 묶음마다 표본 커널(격자 점, 무게 × 흐려짐 ÷ 무게 합)을 남긴다. 원판 누름이 같은 무게로 힘을 나눠 걸고 응답을 읽는다.
		TArray<int32>& KernelOffsets = Solver->KernelOffsets;
		TArray<int32>& KernelNodes = Solver->KernelNodes;
		TArray<double>& KernelWeights = Solver->KernelWeights;
		KernelOffsets.SetNumUninitialized(NumUnique + 1);
		TArray<TPair<int32, double>> KernelRow;
		for (int32 u = 0; u < NumUnique; ++u)
		{
			KernelOffsets[u] = KernelNodes.Num();
			KernelRow.Reset();
			const FVector3d P = UPos[u];
			const FIntVector C = KernelCell(P);
			double WeightSum = 0.0;
			double NearestSq = TNumericLimits<double>::Max();
			FVector3d Sum[NumResponses] = {};
			for (int32 dz = -1; dz <= 1; ++dz)
			for (int32 dy = -1; dy <= 1; ++dy)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				Nearby.Reset();
				NodeGrid.MultiFind(C + FIntVector(dx, dy, dz), Nearby);
				for (const int32 n : Nearby)
				{
					const double DistSq = FVector3d::DistSquared(P, Nodes[n]);
					NearestSq = FMath::Min(NearestSq, DistSq);
					if (DistSq >= KernelRadiusSq)
					{
						continue;
					}
					const double Falloff = 1.0 - DistSq / KernelRadiusSq;
					const double W = Falloff * Falloff * NodeMass[n];
					WeightSum += W;
					KernelRow.Emplace(n, W);
					const int32 N3 = n * 3;
					for (int32 Response = 0; Response < NumResponses; ++Response)
					{
						Sum[Response] += W * FVector3d(Solutions[Response][N3],
							Solutions[Response][N3 + 1], Solutions[Response][N3 + 2]);
					}
				}
			}
			if (WeightSum <= 0.0)
			{
				++NumFaded;
				continue;
			}
			const double Nearest = FMath::Sqrt(NearestSq);
			const double FadeT = FMath::Clamp((Nearest - Cell) / Cell, 0.0, 1.0);
			const double Fade = 1.0 - FadeT * FadeT * (3.0 - 2.0 * FadeT);
			NumFaded += Fade < 1.0 ? 1 : 0;
			if (Fade <= 0.0)
			{
				continue;
			}
			const double KernelScale = Fade / WeightSum;
			for (const TPair<int32, double>& Entry : KernelRow)
			{
				KernelNodes.Add(Entry.Key);
				KernelWeights.Add(Entry.Value * KernelScale);
			}
			Reach = FMath::Max(Reach, (P - AnchorOrigin).Length());
			for (int32 Response = 0; Response < NumResponses; ++Response)
			{
				const FVector3d Value = Sum[Response] * (Fade / WeightSum);
				SurfaceFields[Response * NumUnique + u] = Value;
				const double Length = Value.Length();
				Out.InertiaResponseScales[Response] = FMath::Max(
					Out.InertiaResponseScales[Response], (float)Length);
				SharedMax = FMath::Max(SharedMax, Length);
			}
		}
		KernelOffsets[NumUnique] = KernelNodes.Num();
		UE_LOG(LogSoftbodyMeasure, Display,
			TEXT("Inertia surface sampling: radius %.2f, vertices faded or turned off for lying away from the flesh %d / %d"),
			KernelRadius, NumFaded, NumUnique);
		if (!FMath::IsFinite(SharedMax) || SharedMax <= 1e-12)
		{
			UE_LOG(LogSoftbodyMeasure, Error,
				TEXT("Inertia bake failed: no surface vertex can jiggle (max offset %.6g). The rigid core or bone-painted flesh may cover the whole mesh; lower Bone Thickness or paint Depth brighter in Slot Masks."), SharedMax);
			return false;
		}
		Out.InertiaAnchorReach = (float)Reach;
		const double InvMax = 1.0 / SharedMax;
		for (int32 Response = 0; Response < NumResponses; ++Response)
		{
			for (int32 u = 0; u < NumUnique; ++u)
			{
				const FVector3f Normalized(SurfaceFields[Response * NumUnique + u] * InvMax);
				for (const int32 VertID : UMembers[u])
				{
					Out.VertexInertiaResponseShapes[
						Response * MaxVertID + VertID] = Normalized;
				}
			}
		}

		UE_LOG(LogSoftbodyMeasure, Display,
			TEXT("Inertia softness done: anchor points %d, center (%.2f %.2f %.2f), reach %.1f, "
				"CG iterations %d/%d/%d, max residual %.2e, per-direction max (before shared normalization) %.4g/%.4g/%.4g"),
			AnchorNodeCount, AnchorOrigin.X, AnchorOrigin.Y, AnchorOrigin.Z, Reach,
			ResponseIterations[0], ResponseIterations[1], ResponseIterations[2],
			MaxResidual, Out.InertiaResponseScales[0], Out.InertiaResponseScales[1],
			Out.InertiaResponseScales[2]);

		// 덩어리 원판 누름(SolveSoftbodyLumpDiscs)이 쓸 나머지: 격자 점, 고정한 점, 정점 묶음과 그 묶음이 격자에서 값을 읽는지.
		Solver->Nodes = Nodes;
		Solver->AnchorNodes = MoveTemp(AnchorNodes);
		Solver->Cell = Cell;
		Solver->VertToUnique = VertToUnique;
		Solver->UPos = UPos;
		Solver->UUsable.Init(0, NumUnique);
		for (int32 u = 0; u < NumUnique; ++u)
		{
			Solver->UUsable[u] = UCoreConnection[u] == 1 && KernelOffsets[u + 1] > KernelOffsets[u] ? 1 : 0;
		}
		Out.Solver = MoveTemp(Solver);
	}

	// --- 최종 기록 (MD 정점 인덱스로 전개) ---
	Out.BakeDiagnosticsVersion = 1;
	Out.VertexEmbeddingStatus.Init(0, MaxVertID);
	Out.VertexEmbeddingMinWeight.Init(-1.f, MaxVertID);
	Out.VertexCoreConnection.Init(0, MaxVertID);
	Out.VertexInertiaResponseRms.Init(0.f, MaxVertID);
	for (int32 u = 0; u < NumUnique; ++u)
	{
		const uint8 EmbeddingStatus = !UEmbed[u].IsValid()
			? 1
			: (UEmbed[u].MinWeight() >= -1e-4 && !UEmbed[u].bNearest ? 3 : 2);
		double ResponseEnergy = 0.0;
		if (Out.NumInertiaResponses > 0 && UMembers[u].Num() > 0)
		{
			const int32 Representative = UMembers[u][0];
			for (int32 Response = 0; Response < Out.NumInertiaResponses; ++Response)
			{
				ResponseEnergy += Out.VertexInertiaResponseShapes[
					Response * MaxVertID + Representative].SizeSquared();
			}
		}
		const float ResponseRms = Out.NumInertiaResponses > 0
			? (float)FMath::Sqrt(ResponseEnergy / Out.NumInertiaResponses)
			: 0.f;
		for (const int32 VertID : UMembers[u])
		{
			Out.VertexEmbeddingStatus[VertID] = EmbeddingStatus;
			Out.VertexEmbeddingMinWeight[VertID] = (float)UEmbed[u].MinWeight();
			Out.VertexCoreConnection[VertID] = UCoreConnection[u];
			Out.VertexInertiaResponseRms[VertID] = ResponseRms;
		}
	}

	UE_LOG(LogSoftbodyMeasure, Display,
		TEXT("Measure done: %d unique vertices, %.1fs"), NumUnique, FPlatformTime::Seconds() - StartTime);
	return true;
}

bool SolveSoftbodyLumpDiscs(const FSoftbodyMeasureSolver& Solver, const FMeshDescription& MD, TConstArrayView<int32> VertTexel,
	TConstArrayView<float> VertexAreas, TConstArrayView<int32> VertexLump, TConstArrayView<float> VertexLumpWeight,
	const TArray<FSoftbodyLayerLink>& LayerLinks, TArray<FSoftbodyLumpDiscBake>& OutBakes)
{
	OutBakes.Reset();
	const double StartTime = FPlatformTime::Seconds();
	const int32 NumVertices = VertexAreas.Num();
	const int32 NumUnique = Solver.UPos.Num();
	const int32 NumNodes = Solver.Nodes.Num();
	const int32 Dof = NumNodes * 3;
	if (NumVertices == 0 || NumUnique == 0 || NumNodes == 0 || VertexLump.Num() != NumVertices || VertexLumpWeight.Num() != NumVertices
		|| VertTexel.Num() != Solver.VertToUnique.Num() || Solver.KernelOffsets.Num() != NumUnique + 1
		|| Solver.UUsable.Num() != NumUnique || Solver.AnchorNodes.Num() != NumNodes)
	{
		UE_LOG(LogSoftbodyMeasure, Warning, TEXT("Lump presses: skipped, the per-vertex arrays do not match the measured mesh"));
		return false;
	}
	// 원판 가운데를 고를 살: 덩어리 몫이 이보다 큰 정점. 원판에는 가운데 정점과 노멀이 이보다 비슷하게 향한 정점만 든다(가슴 사이처럼
	// 반지름 안에 마주 보는 살이 들어와도 그쪽은 누르지 않는다).
	constexpr float CandidateWeight = 0.3f;
	constexpr double DiscFacing = 0.3;
	constexpr int32 MinCandidates = 4;
	const int32 MaxDiscs = SoftbodyLumpRegion::MaxModes;

	// 굽기 정점 → 같은 자리 정점 묶음(측정이 살갗 값을 재는 단위). 묶음마다 넓이(정점 넓이의 합)와 겹친 층의 따르는 정점인가.
	TArray<int32> UniqueOf;
	UniqueOf.Init(INDEX_NONE, NumVertices);
	for (int32 VertID = 0; VertID < VertTexel.Num(); ++VertID)
	{
		const int32 Vertex = VertTexel[VertID];
		if (Vertex >= 0 && Vertex < NumVertices)
		{
			UniqueOf[Vertex] = Solver.VertToUnique[VertID];
		}
	}
	TArray<double> UArea;
	UArea.Init(0.0, NumUnique);
	for (int32 v = 0; v < NumVertices; ++v)
	{
		if (UniqueOf[v] != INDEX_NONE)
		{
			UArea[UniqueOf[v]] += FMath::Max((double)VertexAreas[v], 0.0);
		}
	}
	TArray<bool> UFollower;
	UFollower.Init(false, NumUnique);
	for (const FSoftbodyLayerLink& Link : LayerLinks)
	{
		if (Link.Vertex >= 0 && Link.Vertex < NumVertices && UniqueOf[Link.Vertex] != INDEX_NONE)
		{
			UFollower[UniqueOf[Link.Vertex]] = true;
		}
	}

	// 묶음마다 바깥 노멀: 둘레 삼각형 노멀의 넓이 가중 합. 언리얼은 왼손 좌표계라 삼각형 (P0, P1, P2)의 바깥 노멀은 (P2 − P0) × (P1 − P0)다
	// (GeometryCore VectorUtil::Normal, 측정이 살을 채울 때 쓰는 winding number와 같은 규칙). 아래에서 살 격자 쪽과 견줘 한 번 더 확인한다.
	TArray<FVector3d> UNormal;
	UNormal.Init(FVector3d::ZeroVector, NumUnique);
	{
		const TVertexAttributesConstRef<FVector3f> Positions = MD.GetVertexPositions();
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> Tri = MD.GetTriangleVertices(TriID);
			const FVector3d P0(Positions[Tri[0]]);
			const FVector3d P1(Positions[Tri[1]]);
			const FVector3d P2(Positions[Tri[2]]);
			const FVector3d Normal = (P2 - P0) ^ (P1 - P0);
			for (int32 k = 0; k < 3; ++k)
			{
				const int32 VertID = Tri[k].GetValue();
				const int32 Unique = Solver.VertToUnique.IsValidIndex(VertID) ? Solver.VertToUnique[VertID] : INDEX_NONE;
				if (Unique != INDEX_NONE)
				{
					UNormal[Unique] += Normal;
				}
			}
		}
		for (FVector3d& Normal : UNormal)
		{
			Normal = Normal.GetSafeNormal();
		}
	}

	// 덩어리 묶음과 원판 가운데가 될 정점(살 격자에서 값을 읽고, 따르는 정점이 아니고, 몫이 CandidateWeight를 넘는 묶음).
	TArray<int32> Groups;
	for (int32 v = 0; v < NumVertices; ++v)
	{
		if (VertexLump[v] != INDEX_NONE && VertexLumpWeight[v] > 0.f)
		{
			Groups.AddUnique(VertexLump[v]);
		}
	}
	Groups.Sort();
	struct FLumpDiscPlan
	{
		int32 Group = INDEX_NONE;
		/** 묶음마다 원판 힘을 적을 굽기 정점(같은 자리 정점 중 이 덩어리 몫이 가장 큰 것)과 그 몫. */
		TArray<int32> URep;
		TArray<float> UWeight;
		TArray<int32> Candidates;
		double Area = 0.0;
		double Radius = 0.0;
		/** 원판마다 가운데 묶음, 힘을 받는 묶음과 그 힘. */
		TArray<int32> Centers;
		TArray<TArray<int32>> DiscUnique;
		TArray<TArray<FVector3d>> DiscForce;
		int32 FirstLoad = 0;
	};
	TArray<FLumpDiscPlan> Plans;
	for (const int32 Group : Groups)
	{
		FLumpDiscPlan Plan;
		Plan.Group = Group;
		Plan.URep.Init(INDEX_NONE, NumUnique);
		Plan.UWeight.Init(0.f, NumUnique);
		for (int32 v = 0; v < NumVertices; ++v)
		{
			const int32 Unique = UniqueOf[v];
			if (Unique != INDEX_NONE && VertexLump[v] == Group && VertexLumpWeight[v] > Plan.UWeight[Unique])
			{
				Plan.UWeight[Unique] = VertexLumpWeight[v];
				Plan.URep[Unique] = v;
			}
		}
		for (int32 Unique = 0; Unique < NumUnique; ++Unique)
		{
			if (Plan.UWeight[Unique] > CandidateWeight && Solver.UUsable[Unique] && !UFollower[Unique] && !UNormal[Unique].IsZero()
				&& UArea[Unique] > 0.0)
			{
				Plan.Candidates.Add(Unique);
				Plan.Area += UArea[Unique];
			}
		}
		if (Plan.Candidates.Num() < MinCandidates)
		{
			UE_LOG(LogSoftbodyMeasure, Display, TEXT("Lump presses for group %d: skipped, only %d vertices carry more than %.0f%% of the lump"),
				Group, Plan.Candidates.Num(), 100.f * CandidateWeight);
			continue;
		}
		Plans.Add(MoveTemp(Plan));
	}
	if (Plans.Num() == 0)
	{
		return false;
	}

	// 노멀이 바깥을 향하는지: 정점의 표본 커널이 읽는 격자 점(살 속)의 무게 중심에서 정점 쪽이 바깥이다. 과반이 안쪽이면 모두 뒤집는다.
	int32 NumOutward = 0;
	int32 NumChecked = 0;
	for (const FLumpDiscPlan& Plan : Plans)
	{
		for (const int32 Unique : Plan.Candidates)
		{
			FVector3d Inside = FVector3d::ZeroVector;
			double WeightSum = 0.0;
			for (int32 k = Solver.KernelOffsets[Unique]; k < Solver.KernelOffsets[Unique + 1]; ++k)
			{
				Inside += Solver.Nodes[Solver.KernelNodes[k]] * Solver.KernelWeights[k];
				WeightSum += Solver.KernelWeights[k];
			}
			if (WeightSum <= 0.0)
			{
				continue;
			}
			++NumChecked;
			NumOutward += (Solver.UPos[Unique] - Inside / WeightSum).Dot(UNormal[Unique]) > 0.0 ? 1 : 0;
		}
	}
	if (NumChecked > 0 && 2 * NumOutward < NumChecked)
	{
		UE_LOG(LogSoftbodyMeasure, Warning, TEXT("Lump presses: only %d of %d vertex normals point away from the flesh, so all were flipped"),
			NumOutward, NumChecked);
		for (FVector3d& Normal : UNormal)
		{
			Normal = -Normal;
		}
		NumOutward = NumChecked - NumOutward;
	}

	// 원판: 가장 먼 점 뽑기로 MaxDiscs개(후보 평균에서 가장 먼 후보부터, 이미 뽑은 가운데들에서 가장 먼 후보를 차례로). 반지름 = 원판들이
	// 덩어리 겉넓이를 대충 나눠 덮는 크기. 원판 힘 = 넓이 몫 × 안쪽 노멀(크기 합 1).
	int32 NumLoads = 0;
	TArray<TPair<int32, int32>> LoadOf; // (계획, 원판)
	for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
	{
		FLumpDiscPlan& Plan = Plans[PlanIndex];
		const TArray<int32>& Candidates = Plan.Candidates;
		const int32 NumCandidates = Candidates.Num();
		Plan.Radius = FMath::Max(0.75 * FMath::Sqrt(Plan.Area / MaxDiscs), 1.0);
		FVector3d Mean = FVector3d::ZeroVector;
		for (const int32 Unique : Candidates)
		{
			Mean += Solver.UPos[Unique];
		}
		Mean /= (double)NumCandidates;
		int32 Next = 0;
		double Farthest = -1.0;
		for (int32 i = 0; i < NumCandidates; ++i)
		{
			const double Distance = FVector3d::Distance(Solver.UPos[Candidates[i]], Mean);
			if (Distance > Farthest)
			{
				Farthest = Distance;
				Next = i;
			}
		}
		TArray<int32> Picked;
		TArray<double> MinDistance;
		MinDistance.Init(TNumericLimits<double>::Max(), NumCandidates);
		while (Picked.Num() < FMath::Min(MaxDiscs, NumCandidates))
		{
			Picked.Add(Candidates[Next]);
			const FVector3d Center = Solver.UPos[Candidates[Next]];
			Farthest = -1.0;
			for (int32 i = 0; i < NumCandidates; ++i)
			{
				MinDistance[i] = FMath::Min(MinDistance[i], FVector3d::Distance(Solver.UPos[Candidates[i]], Center));
				if (MinDistance[i] > Farthest)
				{
					Farthest = MinDistance[i];
					Next = i;
				}
			}
		}
		const double RadiusSq = Plan.Radius * Plan.Radius;
		Plan.FirstLoad = NumLoads;
		for (const int32 Center : Picked)
		{
			TArray<int32> Members;
			double DiscArea = 0.0;
			for (const int32 Unique : Candidates)
			{
				if (FVector3d::DistSquared(Solver.UPos[Unique], Solver.UPos[Center]) <= RadiusSq
					&& UNormal[Unique].Dot(UNormal[Center]) > DiscFacing)
				{
					Members.Add(Unique);
					DiscArea += UArea[Unique];
				}
			}
			if (DiscArea <= 0.0)
			{
				continue;
			}
			TArray<FVector3d> Forces;
			Forces.Reserve(Members.Num());
			for (const int32 Unique : Members)
			{
				Forces.Add(-UNormal[Unique] * (UArea[Unique] / DiscArea));
			}
			Plan.Centers.Add(Center);
			Plan.DiscUnique.Add(MoveTemp(Members));
			Plan.DiscForce.Add(MoveTemp(Forces));
			LoadOf.Emplace(PlanIndex, Plan.Centers.Num() - 1);
			++NumLoads;
		}
	}

	// 원판마다 풀기: 힘을 표본 커널의 전치로 격자 점에 나눠 걸고(고정한 점은 0 — 체적력과 같다) 무름과 같은 분해로 푼다.
	const double SolveStart = FPlatformTime::Seconds();
	TArray<TArray<double>> Solutions;
	Solutions.SetNum(NumLoads);
	TArray<double> Residuals;
	Residuals.Init(0.0, NumLoads);
	TArray<int32> Iterations;
	Iterations.Init(0, NumLoads);
	ParallelFor(NumLoads, [&](int32 Load)
	{
		const FLumpDiscPlan& Plan = Plans[LoadOf[Load].Key];
		const int32 Disc = LoadOf[Load].Value;
		TArray<double> Force;
		Force.SetNumZeroed(Dof);
		for (int32 i = 0; i < Plan.DiscUnique[Disc].Num(); ++i)
		{
			const int32 Unique = Plan.DiscUnique[Disc][i];
			const FVector3d& F = Plan.DiscForce[Disc][i];
			for (int32 k = Solver.KernelOffsets[Unique]; k < Solver.KernelOffsets[Unique + 1]; ++k)
			{
				const int32 Node = Solver.KernelNodes[k];
				if (Solver.AnchorNodes[Node])
				{
					continue;
				}
				const double W = Solver.KernelWeights[k];
				Force[Node * 3] += W * F.X;
				Force[Node * 3 + 1] += W * F.Y;
				Force[Node * 3 + 2] += W * F.Z;
			}
		}
		TArray<double>& X = Solutions[Load];
		X.SetNumZeroed(Dof);
		if (Solver.Direct.bReady)
		{
			Solver.Direct.Solve(Force.GetData(), X.GetData());
		}
		else
		{
			Iterations[Load] = SolveCG(Solver.AnchoredK, Solver.AnchoredInvDiag, Force, X, 8000, 1e-8);
		}
		TArray<double> AX;
		ApplyOperator(Solver.AnchoredK, X, AX);
		double ResidualSq = 0.0;
		double ForceSq = 0.0;
		for (int32 i = 0; i < Dof; ++i)
		{
			const double R = Force[i] - AX[i];
			ResidualSq += R * R;
			ForceSq += Force[i] * Force[i];
		}
		Residuals[Load] = FMath::Sqrt(ResidualSq / FMath::Max(ForceSq, 1e-30));
	});
	const double SolveSeconds = FPlatformTime::Seconds() - SolveStart;

	// 덩어리마다 응답을 읽는다: 무름과 같은 표본 커널로 정점 묶음마다 재고, 겹친 층은 무름처럼 아래 살의 값을 섞는다.
	TArray<bool> InLump;
	for (const FLumpDiscPlan& Plan : Plans)
	{
		const int32 NumDiscs = Plan.Centers.Num();
		double MaxResidual = 0.0;
		int32 MaxIterations = 0;
		for (int32 Disc = 0; Disc < NumDiscs; ++Disc)
		{
			const double Residual = Residuals[Plan.FirstLoad + Disc];
			MaxResidual = FMath::IsFinite(Residual) ? FMath::Max(MaxResidual, Residual) : TNumericLimits<double>::Max();
			MaxIterations = FMath::Max(MaxIterations, Iterations[Plan.FirstLoad + Disc]);
		}
		if (NumDiscs == 0)
		{
			UE_LOG(LogSoftbodyMeasure, Display, TEXT("Lump presses for group %d: skipped, no disc covers any surface area"), Plan.Group);
			continue;
		}
		if (MaxResidual > 1e-4)
		{
			UE_LOG(LogSoftbodyMeasure, Warning, TEXT("Lump presses for group %d: skipped, the solve did not converge (relative residual %.3e > 1e-4)"),
				Plan.Group, MaxResidual);
			continue;
		}

		FSoftbodyLumpDiscBake Bake;
		Bake.Group = Plan.Group;
		Bake.Radius = (float)Plan.Radius;
		InLump.Init(false, NumVertices);
		for (int32 v = 0; v < NumVertices; ++v)
		{
			if (VertexLump[v] == Plan.Group && VertexLumpWeight[v] > 0.f)
			{
				Bake.Vertices.Add(v);
				InLump[v] = true;
			}
		}
		// 읽어야 할 묶음: 이 덩어리 정점과, 그중 겹친 층의 따르는 정점이 섞는 살 정점.
		TArray<bool> Needed;
		Needed.Init(false, NumUnique);
		for (const int32 v : Bake.Vertices)
		{
			if (UniqueOf[v] != INDEX_NONE)
			{
				Needed[UniqueOf[v]] = true;
			}
		}
		for (const FSoftbodyLayerLink& Link : LayerLinks)
		{
			if (!InLump.IsValidIndex(Link.Vertex) || !InLump[Link.Vertex])
			{
				continue;
			}
			for (const int32 Source : Link.Sources)
			{
				if (Source >= 0 && Source < NumVertices && UniqueOf[Source] != INDEX_NONE)
				{
					Needed[UniqueOf[Source]] = true;
				}
			}
		}
		const int32 NumLumpVertices = Bake.Vertices.Num();
		Bake.Responses.SetNumZeroed(NumDiscs * NumLumpVertices);
		ParallelFor(NumDiscs, [&](int32 Disc)
		{
			const TArray<double>& X = Solutions[Plan.FirstLoad + Disc];
			TArray<FVector3f> UValue;
			UValue.Init(FVector3f::ZeroVector, NumUnique);
			for (int32 Unique = 0; Unique < NumUnique; ++Unique)
			{
				if (!Needed[Unique])
				{
					continue;
				}
				FVector3d Sum = FVector3d::ZeroVector;
				for (int32 k = Solver.KernelOffsets[Unique]; k < Solver.KernelOffsets[Unique + 1]; ++k)
				{
					const int32 N3 = Solver.KernelNodes[k] * 3;
					Sum += Solver.KernelWeights[k] * FVector3d(X[N3], X[N3 + 1], X[N3 + 2]);
				}
				UValue[Unique] = FVector3f(Sum);
			}
			TArray<FVector3f> Values;
			Values.Init(FVector3f::ZeroVector, NumVertices);
			for (int32 v = 0; v < NumVertices; ++v)
			{
				if (UniqueOf[v] != INDEX_NONE)
				{
					Values[v] = UValue[UniqueOf[v]];
				}
			}
			SoftbodyLayers::Apply(LayerLinks, Values, FVector3f::ZeroVector);
			for (int32 i = 0; i < NumLumpVertices; ++i)
			{
				Bake.Responses[Disc * NumLumpVertices + i] = Values[Bake.Vertices[i]];
			}
		});

		int32 NumLoadEntries = 0;
		Bake.LoadOffsets.Reserve(NumDiscs + 1);
		for (int32 Disc = 0; Disc < NumDiscs; ++Disc)
		{
			Bake.Centers.Add(FVector3f(Solver.UPos[Plan.Centers[Disc]]));
			Bake.LoadOffsets.Add(Bake.LoadVertices.Num());
			for (int32 i = 0; i < Plan.DiscUnique[Disc].Num(); ++i)
			{
				Bake.LoadVertices.Add(Plan.URep[Plan.DiscUnique[Disc][i]]);
				Bake.LoadForces.Add(FVector3f(Plan.DiscForce[Disc][i]));
			}
			NumLoadEntries += Plan.DiscUnique[Disc].Num();
		}
		Bake.LoadOffsets.Add(Bake.LoadVertices.Num());

		const FString Method = Solver.Direct.bReady ? FString(TEXT("direct factorization"))
			: FString::Printf(TEXT("CG, up to %d iterations"), MaxIterations);
		UE_LOG(LogSoftbodyMeasure, Display,
			TEXT("Lump presses for group %d: %d discs of radius %.2f over %d vertices (area %.0f, %.1f pressed vertices per disc), responses at %d vertices (%.1f MB), ")
			TEXT("solve %.2f s for all lumps (%s, max residual %.2e)"),
			Plan.Group, NumDiscs, Plan.Radius, Plan.Candidates.Num(), Plan.Area, (double)NumLoadEntries / NumDiscs, NumLumpVertices,
			(double)Bake.Responses.Num() * sizeof(FVector3f) / (1024.0 * 1024.0), SolveSeconds, *Method, MaxResidual);
		OutBakes.Add(MoveTemp(Bake));
		for (int32 Disc = 0; Disc < NumDiscs; ++Disc)
		{
			Solutions[Plan.FirstLoad + Disc].Empty();
		}
	}
	UE_LOG(LogSoftbodyMeasure, Display, TEXT("Lump presses: %d lumps, %d solves, vertex normals pointing away from the flesh %d / %d, %.1f s"),
		OutBakes.Num(), NumLoads, NumOutward, NumChecked, FPlatformTime::Seconds() - StartTime);
	return OutBakes.Num() > 0;
}
