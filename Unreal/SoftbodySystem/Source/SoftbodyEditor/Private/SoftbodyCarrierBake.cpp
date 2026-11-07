// Copyright (c) 2026 wjsekrud

// ADR-0003 표면 캐리어 베이크: 몸의 바깥 봉투 표면을 뽑아 한 장의 UV 아틀라스로 펼치고,
// 런타임 표면 장(SoftbodySurfaceField)이 쓰는 칸별 데이터와 렌더 정점별 캐리어 UV를 굽는다.
// ADR-0007: 차트는 메시 UV0 아일랜드(기본) 또는 봉투를 XAtlas로 자른 조각이다. UV0 차트면 캐리어 UV를 정점
// 인스턴스마다 메시 UV 채널에 쓰고 호스트 머티리얼에 잇는다. 고스트 칸은 옆 차트의 정확한 점을 가리킨다.
#include "SoftbodyBakeLibrary.h"

#include "Algo/BinarySearch.h"
#include "Algo/Reverse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "BoneWeights.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/ThreadSafeCounter.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "DynamicMeshEditor.h"
#include "Generators/MarchingCubes.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "MeshQueries.h"
#include "MeshSimplification.h"
#include "Misc/PackageName.h"
#include "Selections/MeshConnectedComponents.h"
#include "SkeletalMeshAttributes.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyContactResponse.h"
#include "SoftbodyComponent.h"
#include "SoftbodyColliderField.h"
#include "SoftbodyLumpSupport.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyPlateBake.h"
#include "Misc/Paths.h"
#include "Spatial/FastWinding.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "XAtlasWrapper.h"

#include <cmath>

using namespace UE::Geometry;

// 두 파일(SoftbodyCarrierBake·SoftbodyLumpSupport)이 같은 static 분류를 쓴다 — 유니티 빌드에서 한 묶음에 들면 먼저 온 정의 하나만 남긴다.
#ifndef SOFTBODY_EDITOR_LOG_CARRIER_DEFINED
#define SOFTBODY_EDITOR_LOG_CARRIER_DEFINED
DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyCarrier, Log, All);
#endif

namespace
{
	/** 캐리어 칸 종류 (CarrierPositionTex.w와 셰이더 규약이 같다). */
	constexpr uint8 KindEmpty = 0;
	constexpr uint8 KindSurface = 1;
	constexpr uint8 KindGhost = 2;
	/** 차트 밖으로 고스트 칸을 두는 폭(텍셀). 이완 패스는 이웃 1칸, bilinear는 1칸을 읽는다. */
	constexpr int32 GhostRings = 3;
	/**
	 * 아틀라스 가장자리에 비워 두는 띠(텍셀, 일지 300). 판(ADR-0038)은 발자국 칸 둘레 거친 칸 두 고리의 마디를 읽어서, 표면 칸이 지도 가장자리에서
	 * 1.5 거친 칸(CoarseBlock 4면 6칸) 안에 있으면 판이 될 수 없다(SoftbodyPlateBake::NodesInsideGrid — "지도 가장자리"로 빠짐). 차트 여백
	 * (GhostRings + 1)에 이 띠를 더해 표면 칸이 가장자리에서 7칸 넘게 떨어지게 한다. 합성 겹친 가슴골 조각·장식 옷 시험 캐릭터의 버클 섬이 가장자리 4칸에
	 * 놓여 버클이 판에서 빠졌다.
	 */
	constexpr int32 AtlasBorderBand = 3;
	constexpr int32 SmoothIterations = 4;
	/** UV0 아일랜드가 이보다 많으면 차트로 쓰지 않는다(배치가 느리고 아틀라스가 여백으로 찬다). */
	constexpr int32 MaxUV0Charts = 4096;
	/** UV0 차트를 포기하는 기준: UV 넓이가 0인 삼각형의 면적 비율, 같은 차트에서 접히거나 겹친 칸 비율. */
	constexpr double MaxUV0DegenerateArea = 0.05;
	constexpr double MaxUV0FoldTexels = 0.02;
	/**
	 * UV0 아일랜드를 감김 방향으로 가르는 문턱(거울 반쪽·앞뒷면): 적은 쪽 방향이 아일랜드 3-D 넓이의 이 몫 이상일 때만 가른다.
	 * 상용 캐릭터의 거울 아일랜드는 거의 반반이고, UV0의 작은 접힘(몇 삼각형)은 다시 펴기가 고친다.
	 */
	constexpr double MirrorMinShare = 0.1;
	/** 가른 뒤 같은 방향끼리 이어진 묶음이 이보다 작으면 둘레 방향으로 되돌린다(작은 접힘에 이음매를 만들지 않게). */
	constexpr int32 MirrorMinPartTris = 16;
	/** 아일랜드 다시 펴기(ADR-0007 §3.7)의 되풀이 횟수. 시험 캐릭터 칸 격자로 흉내 냈을 때 12번이면 더 변하지 않았다. */
	constexpr int32 FlattenIterations = 30;
	/** 다시 편 뒤에도 칸 밀도(아일랜드 평균 대비) 5~95%가 이 범위를 벗어나는 아일랜드는 로그에 적는다(이음매를 더 넣을 후보). */
	constexpr double FlattenEvenLow = 0.8;
	constexpr double FlattenEvenHigh = 1.25;
	/**
	 * 다시 펴다 새로 뒤집혀도 받아들이는 한도: 아일랜드 삼각형 수와 3D 넓이에 대한 비율. 아주 가는 삼각형 몇 개가 뒤집히는 것은
	 * 칸 한두 개도 차지하지 않는다(시험 캐릭터 앞몸 7398개 중 6개). 넘으면 그 아일랜드는 UV0 모양 그대로 쓴다.
	 */
	constexpr double FlattenFlipCount = 0.002;
	constexpr double FlattenFlipArea = 0.001;
	/** 각이 1° 안쪽으로 납작한 삼각형(코탄 57 초과)은 펴기 풀이에서 뺀다(이웃이 정점을 잡아 준다). */
	constexpr double FlattenMaxCot = 57.0;
	/**
	 * 편 모양이 제 몸과 겹치면, 겹친 삼각형에서 이 거리(메시 로컬, cm) 안의 경계 정점을 UV0 자리에 고정하고 다시 편다. 이것을
	 * FlattenPinRounds번까지 한다. 시험 캐릭터 사타구니·발가락 사이·손가락 사이가 한 번에 풀렸다.
	 */
	constexpr double FlattenPinRadius = 3.0;
	constexpr int32 FlattenPinRounds = 5;
	/**
	 * 이음매를 더 넣어 펴기(ADR-0007 §3.8). 다시 편 아일랜드에서 칸 크기가 가장 벗어난 방향의 배율(넓이 가중 95%)이
	 * FlattenCutTarget을 넘으면 아일랜드를 가른다. 틈 하나는 그 배율 로그의 넓이 가중 제곱평균제곱근(아일랜드 전체의 고르지
	 * 않은 정도)을 FlattenCutGain 이상 줄이고 95%를 늘리지 않아야 남긴다. 틈은 FlattenMaxCuts개까지, 틈을 시험하는 펴기는
	 * FlattenCutAttempts번까지, FlattenCutMinLength보다 짧은 틈(경계 바로 옆)은 내지 않는다. 넓이가 FlattenCutMinArea(메시
	 * 로컬², 시험 캐릭터 cm²)보다 작은 아일랜드는 가르지 않는다. 틈을 냈다가 되돌린 자리에서 FlattenCutRetryRadius 안은 다시
	 * 고르지 않는다. 가장 늘거나 준 자리는 정점 값을 이웃과 FlattenCutSmoothPasses번 평균해 고른다(삼각형 한두 개만 튀는 곳 말고
	 * 넓게 틀어진 곳).
	 */
	constexpr double FlattenCutTarget = 1.1;
	constexpr double FlattenCutGain = 0.05;
	constexpr int32 FlattenMaxCuts = 8;
	constexpr int32 FlattenCutAttempts = 16;
	constexpr double FlattenCutMinLength = 1.0;
	constexpr double FlattenCutMinArea = 20.0;
	constexpr double FlattenCutRetryRadius = 3.0;
	constexpr int32 FlattenCutSmoothPasses = 8;
	/** 전체 봉투 면적 대비 이보다 작은 연결 조각은 버린다(열린 셸이 등위면에 남기는 부스러기). */
	constexpr double MinComponentAreaFraction = 0.005;
	/**
	 * 미는 방향을 평균 내는 범위(가우스 반경) = 살 두께 × 이 값. 살은 두께 정도 범위가 한 덩어리로 움직이므로, 그보다 작은
	 * 모양(배꼽, 주름)은 눌려도 벌어지지 않고 통째로 가라앉는다. 시험 캐릭터 배(두께 약 1.9 cm)에서 약 1.4 cm — 배꼽 캡처로
	 * 재어 보니 0.75 cm로는 배꼽이 0.07 cm 벌어졌고 1.5 cm에서 벌어짐이 없었다.
	 */
	constexpr double PushDirectionSmoothing = 0.75;
	/**
	 * 제 살 깊이(ADR-0003 §16)를 재는 범위 = 살 두께 × 이 값. 런타임 접촉이 살 속에 묻힌 콜라이더를 찾는 깊이
	 * (SoftbodySurfaceField.usf의 1 + SB_CORE_FADE)와 같아야 한다.
	 */
	constexpr double ColumnSearch = 1.5;
	/**
	 * 미는 방향끼리의 내적 크기가 이보다 작은 칸만 "다른 쪽을 향한 면"으로 센다(약 45~135° 돌아선 면). 같은 면의 이웃, 미는 방향을
	 * 평균 내 한 방향이 된 작은 굴곡(배꼽·주름·돌기), 얇은 판의 뒷면(반대쪽)은 빠진다.
	 */
	constexpr double ColumnMaxFacing = 0.7;
	/** 다른 쪽을 향한 칸이 이만큼은 걸려야 제 살 깊이를 줄인다(칸 몇 개짜리 잡음에 흔들리지 않게 이 번째로 작은 값을 쓴다). */
	constexpr int32 ColumnMinPoints = 8;
	/**
	 * UV0 차트의 칸 점을 렌더 삼각형 위에서 세 정점 노멀 쪽으로 휘게 하는 정도(퐁 곡면, ADR-0007 §3.6). 0 = 평평한 삼각형,
	 * 1 = 세 정점의 접평면에 내린 점을 그대로 섞음. 칸 점으로 구한 노멀이 화면 노멀(정점 노멀 보간)과 맞을수록 눌린 자리
	 * 음영이 매끈하다. 시험 캐릭터 1024² 차트 안쪽 칸에서 칸 노멀(한 칸 차분)과 화면 노멀 차이(중앙 / 90%)가 0 → 3.3° / 7.6°,
	 * 0.25 → 2.2° / 5.5°, 0.5 → 1.7° / 4.5°, 0.75 → 2.3° / 5.5°로 0.5가 가장 작았다(봉투 점은 1.7° / 4.7°).
	 * 콜라이더 면에 붙은 칸은 이 칸 노멀 대신 구운 화면 노멀을 쓴다(CarrierRedirect w) — 이 값은 둘레·솟음 음영과 나나이트
	 * 테셀레이션이 쪼갠 점의 위치(칸 점 − 평평한 면만큼 콜라이더 안쪽)에 남는다.
	 */
	constexpr double DrawnSurfaceCurve = 0.5;

	/** 아틀라스 한 장: UV 삼각형, 차트, 칸별 삼각형·무게중심 좌표·종류·가져올 칸. */
	struct FCarrierAtlas
	{
		int32 Resolution = 0;
		TArray<FVector2d> UVs;
		TArray<int32> UVIndices;
		TArray<int32> UVToVertex;
		TArray<int32> TriChart;
		/** 칸이 가리키는 아틀라스 메시 위 점: 표면 칸은 칸 한가운데, 고스트 칸은 옆 차트에서 이어지는 점(BuildGhosts). */
		TArray<int32> TexelTri;
		TArray<FVector3d> TexelBary;
		TArray<uint8> Kind;
		/** 칸 좌표(정수 = 칸 한가운데). 고스트 칸은 옆 차트의 정확한 점(소수), 나머지는 자기 칸. */
		TArray<FVector2d> Redirect;
		/** 테두리 고스트 칸의 출처 표면 칸(BuildGhosts — 같은 차트에서 고리를 넓혀 온 표면 칸: 고리가 어느 차트의 것인지). 표면 칸은 제 칸, 빈 칸은 INDEX_NONE. */
		TArray<int32> GhostSource;
		/** 같은 차트에서 서로 이웃하지 않은 두 삼각형이 같은 칸을 차지한 수(UV가 접히거나 겹친 곳). */
		int32 FoldTexels = 0;
		/** 접힌 칸마다 (먼저 차지한 삼각형, 나중 삼각형). UV0 아일랜드를 못 쓸 때 어디서 왜 겹치는지 적는 데 쓴다(LogUV0Folds). */
		TArray<FIntPoint> FoldPairs;

		int32 Index(int32 X, int32 Y) const { return Y * Resolution + X; }

		FVector2d TriUVPixel(int32 Tri, int32 Corner) const
		{
			return UVs[UVIndices[Tri * 3 + Corner]] * (double)Resolution;
		}
	};

	/** 2D 점의 삼각형 무게중심 좌표(삼각형 밖이면 음수 성분, 연장에 그대로 쓴다). 퇴화면 false. */
	bool Barycentric2D(const FVector2d& P, const FVector2d& A, const FVector2d& B, const FVector2d& C, FVector3d& Out)
	{
		const FVector2d V0 = B - A;
		const FVector2d V1 = C - A;
		const FVector2d V2 = P - A;
		const double Den = V0.X * V1.Y - V1.X * V0.Y;
		if (FMath::Abs(Den) < 1e-12)
		{
			return false;
		}
		const double V = (V2.X * V1.Y - V1.X * V2.Y) / Den;
		const double W = (V0.X * V2.Y - V2.X * V0.Y) / Den;
		Out = FVector3d(1.0 - V - W, V, W);
		return true;
	}

	int32 FindRoot(TArray<int32>& Parent, int32 A)
	{
		while (Parent[A] != A)
		{
			Parent[A] = Parent[Parent[A]];
			A = Parent[A];
		}
		return A;
	}

	/**
	 * 소스 메시(MeshDescription)에서 winding number 0.5 등위면으로 바깥 봉투를 뽑는다.
	 * 열린 셸(옷 조각, 머리카락 카드)은 솔리드 안팎을 거의 바꾸지 않으므로 봉투에 들어오지 않는다.
	 */
	/**
	 * 정점·삼각형 번호를 위치 순서로 다시 매긴다. 병렬 등위면 추출은 스레드가 끝난 순서대로 번호를 매겨 같은 입력에서도
	 * 순서가 달라지고, 뒤의 단순화와 XAtlas 차트 나누기는 순서에 민감해 구울 때마다 차트 수가 달라졌다(시험 캐릭터 112 ↔ 128).
	 */
	void SortMeshByPosition(FDynamicMesh3& Mesh)
	{
		TArray<int32> Vertices;
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			Vertices.Add(Vertex);
		}
		Vertices.Sort([&Mesh](int32 A, int32 B)
		{
			const FVector3d PA = Mesh.GetVertex(A);
			const FVector3d PB = Mesh.GetVertex(B);
			if (PA.X != PB.X)
			{
				return PA.X < PB.X;
			}
			return PA.Y != PB.Y ? PA.Y < PB.Y : PA.Z < PB.Z;
		});
		TArray<int32> NewIndex;
		NewIndex.Init(INDEX_NONE, Mesh.MaxVertexID());
		FDynamicMesh3 Sorted;
		for (const int32 Vertex : Vertices)
		{
			NewIndex[Vertex] = Sorted.AppendVertex(Mesh.GetVertex(Vertex));
		}
		// 삼각형은 가장 작은 번호가 앞에 오게 돌린 뒤(감긴 방향 유지) 사전순으로.
		TArray<FIndex3i> Triangles;
		for (const int32 Tri : Mesh.TriangleIndicesItr())
		{
			const FIndex3i T = Mesh.GetTriangle(Tri);
			const FIndex3i N(NewIndex[T.A], NewIndex[T.B], NewIndex[T.C]);
			Triangles.Add(N.A < N.B && N.A < N.C ? N : (N.B < N.C ? FIndex3i(N.B, N.C, N.A) : FIndex3i(N.C, N.A, N.B)));
		}
		Triangles.Sort([](const FIndex3i& A, const FIndex3i& B)
		{
			if (A.A != B.A)
			{
				return A.A < B.A;
			}
			return A.B != B.B ? A.B < B.B : A.C < B.C;
		});
		int32 Failed = 0;
		for (const FIndex3i& T : Triangles)
		{
			Failed += Sorted.AppendTriangle(T) < 0 ? 1 : 0;
		}
		if (Failed > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Outer surface: %d triangles were left out while its triangles were sorted"), Failed);
		}
		Mesh = MoveTemp(Sorted);
	}

	bool ExtractEnvelope(const FDynamicMesh3& Source, double CellSize, int32 TargetTriangles, FDynamicMesh3& OutEnvelope,
		int32 SmoothIterationCount = SmoothIterations)
	{
		FDynamicMeshAABBTree3 SourceBVH(&Source, true);
		TFastWindingTree<FDynamicMesh3> Winding(&SourceBVH, true);

		FAxisAlignedBox3d Bounds = Source.GetBounds();
		Bounds.Expand(CellSize * 3.0);
		FMarchingCubes MarchingCubes;
		MarchingCubes.Bounds = Bounds;
		MarchingCubes.CubeSize = CellSize;
		MarchingCubes.IsoValue = 0.0;
		MarchingCubes.Implicit = [&Winding](FVector3d P) { return Winding.FastWindingNumber(P) - 0.5; };
		MarchingCubes.RootMode = ERootfindingModes::Bisection;
		MarchingCubes.RootModeSteps = 4;
		MarchingCubes.bParallelCompute = true;
		MarchingCubes.Generate();
		OutEnvelope = FDynamicMesh3(&MarchingCubes);
		SortMeshByPosition(OutEnvelope);
		if (OutEnvelope.TriangleCount() == 0)
		{
			UE_LOG(LogSoftbodyCarrier, Error,
				TEXT("Could not build the outer surface: it came out empty (cell %.2f cm). A mesh that is not closed (a single-sided sheet, an open tube) has no inside to build it from. Set Volume Mesh on the authoring asset to a closed, filled mesh placed in the same spot."),
				CellSize);
			return false;
		}

		// 삼각형이 바깥을 향하도록 방향을 맞춘다: 노멀 쪽으로 반 칸 나간 점의 winding이 0.5보다 작아야 한다.
		int32 Outward = 0;
		int32 Inward = 0;
		const int32 Step = FMath::Max(1, OutEnvelope.MaxTriangleID() / 256);
		for (int32 Tri = 0; Tri < OutEnvelope.MaxTriangleID(); Tri += Step)
		{
			if (!OutEnvelope.IsTriangle(Tri))
			{
				continue;
			}
			const FVector3d Probe = OutEnvelope.GetTriCentroid(Tri) + OutEnvelope.GetTriNormal(Tri) * (CellSize * 0.5);
			(Winding.FastWindingNumber(Probe) < 0.5 ? Outward : Inward)++;
		}
		if (Inward > Outward)
		{
			OutEnvelope.ReverseOrientation();
		}

		// 열린 셸(옷 가장자리, 레이스)은 winding이 0.5 근처인 얇은 영역을 만들어 등위면에 작은 조각을 남긴다.
		// 조각마다 차트가 생겨 아틀라스를 잘게 쪼개므로 버린다. 그 근처 정점은 다른 봉투 점을 읽는다.
		{
			FMeshConnectedComponents Components(&OutEnvelope);
			Components.FindConnectedTriangles();
			double TotalArea = 0.0;
			TArray<double> ComponentAreas;
			for (int32 Index = 0; Index < Components.Num(); ++Index)
			{
				double ComponentArea = 0.0;
				for (const int Tri : Components.GetComponent(Index).Indices)
				{
					ComponentArea += OutEnvelope.GetTriArea(Tri);
				}
				ComponentAreas.Add(ComponentArea);
				TotalArea += ComponentArea;
			}
			TArray<int> Removed;
			int32 RemovedComponents = 0;
			for (int32 Index = 0; Index < Components.Num(); ++Index)
			{
				if (ComponentAreas[Index] < MinComponentAreaFraction * TotalArea)
				{
					Removed.Append(Components.GetComponent(Index).Indices);
					++RemovedComponents;
				}
			}
			if (Removed.Num() > 0)
			{
				FDynamicMeshEditor Editor(&OutEnvelope);
				Editor.RemoveTriangles(Removed, true);
			}
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Outer surface pieces: %d found, %d small ones removed (%d triangles)"),
				Components.Num(), RemovedComponents, Removed.Num());
		}

		// 격자 계단을 지운다(바깥 노멀이 변위 방향이므로 계단이 남으면 함몰이 울퉁불퉁해진다).
		for (int32 Iteration = 0; Iteration < SmoothIterationCount; ++Iteration)
		{
			TArray<FVector3d> NewPositions;
			NewPositions.SetNum(OutEnvelope.MaxVertexID());
			ParallelFor(OutEnvelope.MaxVertexID(), [&OutEnvelope, &NewPositions](int32 Vertex)
			{
				if (!OutEnvelope.IsVertex(Vertex))
				{
					return;
				}
				FVector3d Sum = FVector3d::ZeroVector;
				int32 Count = 0;
				OutEnvelope.EnumerateVertexVertices(Vertex, [&OutEnvelope, &Sum, &Count](int32 Neighbor)
				{
					Sum += OutEnvelope.GetVertex(Neighbor);
					++Count;
				});
				const FVector3d P = OutEnvelope.GetVertex(Vertex);
				NewPositions[Vertex] = Count > 0 ? FMath::Lerp(P, Sum / (double)Count, 0.5) : P;
			});
			for (int32 Vertex = 0; Vertex < OutEnvelope.MaxVertexID(); ++Vertex)
			{
				if (OutEnvelope.IsVertex(Vertex))
				{
					OutEnvelope.SetVertex(Vertex, NewPositions[Vertex]);
				}
			}
		}

		if (TargetTriangles > 0 && OutEnvelope.TriangleCount() > TargetTriangles)
		{
			FQEMSimplification Simplifier(&OutEnvelope);
			Simplifier.SimplifyToTriangleCount(TargetTriangles);
		}
		OutEnvelope.CompactInPlace();
		return OutEnvelope.TriangleCount() > 0;
	}

	/**
	 * 면적 가중 정점 노멀 (바깥). 봉투 방향은 엔진 삼각형 노멀(GetTriNormal) 기준으로 맞췄으므로 반드시
	 * 같은 함수를 쓴다 — 언리얼은 왼손 좌표계라 GetTriNormal = (C−A)×(B−A)이고, 외적을 직접 (B−A)×(C−A)로
	 * 쓰면 노멀이 안쪽을 향해 함몰이 콜라이더 쪽으로 빨려 나간다.
	 */
	void ComputeVertexNormals(const FDynamicMesh3& Mesh, TArray<FVector3d>& OutNormals)
	{
		OutNormals.Init(FVector3d::ZeroVector, Mesh.MaxVertexID());
		for (const int32 Tri : Mesh.TriangleIndicesItr())
		{
			const FIndex3i T = Mesh.GetTriangle(Tri);
			const FVector3d Weighted = Mesh.GetTriNormal(Tri) * Mesh.GetTriArea(Tri);
			OutNormals[T.A] += Weighted;
			OutNormals[T.B] += Weighted;
			OutNormals[T.C] += Weighted;
		}
		for (FVector3d& N : OutNormals)
		{
			N = N.GetSafeNormal();
		}
	}

	/**
	 * XAtlas로 차트를 나누고 펼친 뒤, 배치는 직접 한다. 엔진 래퍼는 아틀라스가 여러 장이 되면 장 번호를
	 * 버리고, 넘치지 않아도 차트 일부를 겹쳐 놓는다(시험 캐릭터 실측: 밀도와 무관하게 표면 칸의 약 1%).
	 * 차트마다 실제 모양(여백 포함)을 칸 격자에 그려 빈자리에 끼워 넣고, 들어가는 가장 큰 공통 배율을 이분 탐색으로
	 * 찾으므로 구성상 겹침이 없다. XAtlas의 Resolution을 주면 장마다 정사각이라 차트 안의 텍셀 밀도는 등방이다.
	 */
	bool PackAtlas(const TArray<FVector2d>& SourceUVs, const TArray<int32>& VertexChart, int32 NumCharts, int32 Resolution,
		FCarrierAtlas& Out);

	bool BuildAtlas(const FDynamicMesh3& Envelope, int32 Resolution, FCarrierAtlas& Out)
	{
		TArray<int32> IndexBuffer;
		TArray<FVector3f> VertexBuffer;
		IndexBuffer.Reserve(Envelope.TriangleCount() * 3);
		VertexBuffer.Reserve(Envelope.VertexCount());
		for (int32 Vertex = 0; Vertex < Envelope.MaxVertexID(); ++Vertex)
		{
			VertexBuffer.Add(FVector3f(Envelope.GetVertex(Vertex)));
		}
		double Area = 0.0;
		for (int32 Tri = 0; Tri < Envelope.MaxTriangleID(); ++Tri)
		{
			const FIndex3i T = Envelope.GetTriangle(Tri);
			IndexBuffer.Add(T.A);
			IndexBuffer.Add(T.B);
			IndexBuffer.Add(T.C);
			Area += Envelope.GetTriArea(Tri);
		}

		// 차트를 크게 키운다: 이완 패스는 차트 경계를 고스트 칸으로 건너가지만 경계가 적을수록 매끈하고,
		// 차트마다 여백이 붙어 잘게 쪼개지면 아틀라스 활용률이 무너진다(시험 캐릭터 기본 설정: 차트 1171개, 11%).
		XAtlasWrapper::XAtlasChartOptions ChartOptions;
		ChartOptions.MaxCost = 12.0f;
		ChartOptions.NormalDeviationWeight = 1.0f;
		ChartOptions.MaxIterations = 4;
		XAtlasWrapper::XAtlasPackOptions PackOptions;
		PackOptions.Resolution = (uint32)Resolution;
		PackOptions.TexelsPerUnit = (float)((double)Resolution * FMath::Sqrt(0.5 / FMath::Max(Area, 1e-6)));
		PackOptions.Padding = 1;
		TArray<FVector2D> SourceUVs;
		if (!XAtlasWrapper::ComputeUVs(IndexBuffer, VertexBuffer, ChartOptions, PackOptions,
			SourceUVs, Out.UVIndices, Out.UVToVertex) || Out.UVIndices.Num() != IndexBuffer.Num())
		{
			UE_LOG(LogSoftbodyCarrier, Error, TEXT("Could not unwrap the outer surface into contact map islands (XAtlas failed)"));
			return false;
		}

		// 차트 = UV 정점을 공유하는 삼각형 묶음.
		const int32 NumTris = Out.UVIndices.Num() / 3;
		TArray<int32> Parent;
		Parent.SetNum(SourceUVs.Num());
		for (int32 i = 0; i < Parent.Num(); ++i)
		{
			Parent[i] = i;
		}
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const int32 A = FindRoot(Parent, Out.UVIndices[Tri * 3]);
			const int32 B = FindRoot(Parent, Out.UVIndices[Tri * 3 + 1]);
			Parent[B] = A;
			const int32 C = FindRoot(Parent, Out.UVIndices[Tri * 3 + 2]);
			Parent[C] = A;
		}
		TMap<int32, int32> RootToChart;
		TArray<int32> VertexChart;
		VertexChart.SetNum(SourceUVs.Num());
		for (int32 i = 0; i < SourceUVs.Num(); ++i)
		{
			const int32 Root = FindRoot(Parent, i);
			int32* Chart = RootToChart.Find(Root);
			if (!Chart)
			{
				Chart = &RootToChart.Add(Root, RootToChart.Num());
			}
			VertexChart[i] = *Chart;
		}
		const int32 NumCharts = RootToChart.Num();
		return PackAtlas(SourceUVs, VertexChart, NumCharts, Resolution, Out);
	}

	/**
	 * 차트 배치와 래스터화. Out.UVIndices(삼각형 코너 → UV 요소)·UVToVertex(UV 요소 → 아틀라스 메시 정점)는 채워져 있고,
	 * SourceUVs는 UV 요소마다 차트 좌표(칸 밀도가 모든 차트에서 같도록 정규화, 넓이 합 약 0.5), ElementChart는 요소의 차트.
	 */
	bool PackAtlas(const TArray<FVector2d>& SourceUVs, const TArray<int32>& VertexChart, int32 NumCharts, int32 Resolution,
		FCarrierAtlas& Out)
	{
		const int32 NumTris = Out.UVIndices.Num() / 3;
		Out.TriChart.SetNum(NumTris);
		TArray<int32> ChartTriangles;
		ChartTriangles.Init(0, NumCharts);
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			Out.TriChart[Tri] = VertexChart[Out.UVIndices[Tri * 3]];
			++ChartTriangles[Out.TriChart[Tri]];
		}
		{
			int32 Tiny = 0;
			int32 Largest = 0;
			for (const int32 Count : ChartTriangles)
			{
				Tiny += Count <= 4 ? 1 : 0;
				Largest = FMath::Max(Largest, Count);
			}
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Contact map islands: %d, with 4 or fewer triangles %d, largest %d triangles"), NumCharts, Tiny, Largest);
		}

		// 차트별 UV 사각 영역 (XAtlas 픽셀 단위)과 삼각형.
		TArray<FVector2d> ChartMin;
		TArray<FVector2d> ChartMax;
		ChartMin.Init(FVector2d(TNumericLimits<double>::Max()), NumCharts);
		ChartMax.Init(FVector2d(-TNumericLimits<double>::Max()), NumCharts);
		for (int32 i = 0; i < SourceUVs.Num(); ++i)
		{
			const FVector2d P(SourceUVs[i].X * Resolution, SourceUVs[i].Y * Resolution);
			ChartMin[VertexChart[i]] = FVector2d(FMath::Min(ChartMin[VertexChart[i]].X, P.X), FMath::Min(ChartMin[VertexChart[i]].Y, P.Y));
			ChartMax[VertexChart[i]] = FVector2d(FMath::Max(ChartMax[VertexChart[i]].X, P.X), FMath::Max(ChartMax[VertexChart[i]].Y, P.Y));
		}
		TArray<TArray<int32>> ChartTris;
		ChartTris.SetNum(NumCharts);
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			ChartTris[Out.TriChart[Tri]].Add(Tri);
		}
		// 실제 넓이가 큰 차트부터 놓는다(사각 영역 넓이·긴 변 순서보다 조금 더 들어간다).
		TArray<double> ChartArea;
		ChartArea.Init(0.0, NumCharts);
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const FVector2d A = SourceUVs[Out.UVIndices[Tri * 3]];
			const FVector2d B = SourceUVs[Out.UVIndices[Tri * 3 + 1]];
			const FVector2d C = SourceUVs[Out.UVIndices[Tri * 3 + 2]];
			ChartArea[Out.TriChart[Tri]] += 0.5 * FMath::Abs(FVector2d::CrossProduct(B - A, C - A));
		}
		TArray<int32> Order;
		for (int32 Chart = 0; Chart < NumCharts; ++Chart)
		{
			Order.Add(Chart);
		}
		Order.Sort([&ChartArea](int32 A, int32 B)
		{
			return ChartArea[A] != ChartArea[B] ? ChartArea[A] > ChartArea[B] : A < B;
		});

		// 모양대로 배치: 차트를 0·90·180·270° 돌리고 각각 좌우로 뒤집은 모양(8가지)을 칸 격자에 그리고 사방 Pad칸 넓혀
		// 점유 모양을 만든 뒤,
		// 이미 놓인 점유와 겹치지 않으면서 위쪽 끝이 가장 낮은 자리(같으면 왼쪽)에 끼운다. 휜 차트의 오목한 곳에도
		// 들어간다. 점유끼리 겹치지 않으므로 다른 차트의 표면 칸 사이에는 2 × Pad칸 이상 빈칸이 있다 — 고스트 칸
		// (GhostRings)이 서로 닿지 않고, 솟음 출처의 4×4 칸 블록에는 한 차트만 든다. 배율은 모든 차트가 같다(칸 밀도 등방).
		const int32 Pad = GhostRings + 1;
		// 방향 0~3 = 0·90·180·270° 돌림, 4~7 = 좌우로 뒤집은 뒤 같은 돌림. 뒤집힌 차트도 런타임은 같다(노멀 패스가
		// 방향을 맞추고, 기울기는 이웃 칸 위치 차로 구한다). UV0 아일랜드처럼 크고 휜 차트끼리 끼워 넣을 때 쓸모가 있다.
		constexpr int32 Orientations = 8;
		auto ToLocal = [&ChartMin, &ChartMax, Pad](const FVector2d& P, int32 Chart, int32 Rotation, double Scale)
		{
			FVector2d L = (P - ChartMin[Chart]) * Scale;
			const FVector2d E = (ChartMax[Chart] - ChartMin[Chart]) * Scale;
			if (Rotation >= 4)
			{
				L.X = E.X - L.X;
			}
			const int32 Turn = Rotation % 4;
			const FVector2d R = Turn == 1 ? FVector2d(E.Y - L.Y, L.X)
				: Turn == 2 ? FVector2d(E.X - L.X, E.Y - L.Y)
				: Turn == 3 ? FVector2d(L.Y, E.X - L.X)
				: L;
			return R + FVector2d(Pad, Pad);
		};
		struct FFootprint
		{
			int32 Width = 0;
			int32 Height = 0;
			TArray<int32> RowStart;  // 줄 Y의 구간은 Spans[RowStart[Y]] ~ Spans[RowStart[Y + 1] - 1]
			TArray<FIntPoint> Spans; // 점유 칸 [X, Y)
		};
		auto MakeFootprint = [&](int32 Chart, int32 Rotation, double Scale, FFootprint& F)
		{
			const FVector2d E = (ChartMax[Chart] - ChartMin[Chart]) * Scale;
			F.Width = (int32)FMath::CeilToDouble(Rotation % 2 ? E.Y : E.X) + 2 * Pad + 1;
			F.Height = (int32)FMath::CeilToDouble(Rotation % 2 ? E.X : E.Y) + 2 * Pad + 1;
			// 표면 칸. 최종 래스터화보다 조금 넉넉하게 잡고 꼭짓점 칸도 넣는다.
			TArray<uint8> Covered;
			Covered.Init(0, F.Width * F.Height);
			auto Mark = [&F, &Covered](int32 X, int32 Y)
			{
				Covered[FMath::Clamp(Y, 0, F.Height - 1) * F.Width + FMath::Clamp(X, 0, F.Width - 1)] = 1;
			};
			for (const int32 Tri : ChartTris[Chart])
			{
				FVector2d Q[3];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const FVector2D& UV = SourceUVs[Out.UVIndices[Tri * 3 + Corner]];
					Q[Corner] = ToLocal(FVector2d(UV.X * Resolution, UV.Y * Resolution), Chart, Rotation, Scale);
					Mark((int32)FMath::FloorToDouble(Q[Corner].X), (int32)FMath::FloorToDouble(Q[Corner].Y));
				}
				const int32 X0 = FMath::Max((int32)FMath::FloorToDouble(FMath::Min3(Q[0].X, Q[1].X, Q[2].X)), 0);
				const int32 X1 = FMath::Min((int32)FMath::CeilToDouble(FMath::Max3(Q[0].X, Q[1].X, Q[2].X)), F.Width - 1);
				const int32 Y0 = FMath::Max((int32)FMath::FloorToDouble(FMath::Min3(Q[0].Y, Q[1].Y, Q[2].Y)), 0);
				const int32 Y1 = FMath::Min((int32)FMath::CeilToDouble(FMath::Max3(Q[0].Y, Q[1].Y, Q[2].Y)), F.Height - 1);
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					for (int32 X = X0; X <= X1; ++X)
					{
						FVector3d Bary;
						if (Barycentric2D(FVector2d(X + 0.5, Y + 0.5), Q[0], Q[1], Q[2], Bary)
							&& Bary.X >= -1e-4 && Bary.Y >= -1e-4 && Bary.Z >= -1e-4)
						{
							Covered[Y * F.Width + X] = 1;
						}
					}
				}
			}
			// 사방 Pad칸 넓힌다(가로로 넓힌 뒤 세로로). 오목한 곳은 그대로 비어 다른 차트가 들어올 수 있다.
			TArray<uint8> Wide;
			Wide.Init(0, F.Width * F.Height);
			for (int32 Y = 0; Y < F.Height; ++Y)
			{
				int32 Last = -F.Width; // 가장 최근 표면 칸의 열
				for (int32 X = 0; X < F.Width + Pad; ++X)
				{
					if (X < F.Width && Covered[Y * F.Width + X])
					{
						Last = X;
						for (int32 Back = FMath::Max(X - Pad, 0); Back < X; ++Back)
						{
							Wide[Y * F.Width + Back] = 1;
						}
					}
					if (X < F.Width && X - Last <= Pad)
					{
						Wide[Y * F.Width + X] = 1;
					}
				}
			}
			F.RowStart.SetNum(F.Height + 1);
			F.Spans.Reset();
			for (int32 Y = 0; Y < F.Height; ++Y)
			{
				F.RowStart[Y] = F.Spans.Num();
				const int32 R0 = FMath::Max(Y - Pad, 0);
				const int32 R1 = FMath::Min(Y + Pad, F.Height - 1);
				int32 SpanStart = INDEX_NONE;
				for (int32 X = 0; X <= F.Width; ++X)
				{
					bool bFilled = false;
					for (int32 Row = R0; X < F.Width && Row <= R1 && !bFilled; ++Row)
					{
						bFilled = Wide[Row * F.Width + X] != 0;
					}
					if (bFilled && SpanStart == INDEX_NONE)
					{
						SpanStart = X;
					}
					else if (!bFilled && SpanStart != INDEX_NONE)
					{
						F.Spans.Add(FIntPoint(SpanStart, X));
						SpanStart = INDEX_NONE;
					}
				}
			}
			F.RowStart[F.Height] = F.Spans.Num();
		};

		// 점유 격자: 줄마다 64칸 비트 묶음.
		const int32 Words = (Resolution + 63) / 64;
		TArray<uint64> Occupied;
		// 줄 Y의 [X0, X1) 안에서 가장 오른쪽 점유 칸, 없으면 -1.
		auto LastOccupied = [&Occupied, Words](int32 Y, int32 X0, int32 X1) -> int32
		{
			const uint64* Row = Occupied.GetData() + (int64)Y * Words;
			for (int32 Word = (X1 - 1) >> 6; Word >= (X0 >> 6); --Word)
			{
				const int32 Base = Word << 6;
				uint64 Bits = Row[Word];
				if (X0 > Base)
				{
					Bits &= ~0ull << (X0 - Base);
				}
				if (X1 - Base < 64)
				{
					Bits &= (1ull << (X1 - Base)) - 1;
				}
				if (Bits)
				{
					return Base + 63 - (int32)FMath::CountLeadingZeros64(Bits);
				}
			}
			return -1;
		};
		// 위쪽 끝(Y + 높이)이 Limit보다 낮은 첫 자리를 아래 줄부터 찾는다. 부딪친 칸 너머로 건너뛴다.
		auto FindSpot = [&](const FFootprint& F, int32 Limit, FIntPoint& Spot) -> bool
		{
			int32 Hint = 0;
			for (int32 Y = 0; Y + F.Height <= Resolution && Y + F.Height < Limit; ++Y)
			{
				int32 X = 0;
				while (X + F.Width <= Resolution)
				{
					int32 Next = INDEX_NONE;
					for (int32 Step = 0; Step < F.Height; ++Step)
					{
						const int32 Row = (Hint + Step) % F.Height;
						for (int32 Index = F.RowStart[Row]; Index < F.RowStart[Row + 1] && Next == INDEX_NONE; ++Index)
						{
							const FIntPoint& Span = F.Spans[Index];
							const int32 Last = LastOccupied(Y + Row, X + Span.X, X + Span.Y);
							if (Last >= 0)
							{
								Next = Last - Span.X + 1;
								Hint = Row;
							}
						}
						if (Next != INDEX_NONE)
						{
							break;
						}
					}
					if (Next == INDEX_NONE)
					{
						Spot = FIntPoint(X, Y);
						return true;
					}
					X = Next;
				}
			}
			return false;
		};
		TArray<int32> ChartRotation;
		TArray<FIntPoint> ChartOffset;
		ChartRotation.Init(0, NumCharts);
		ChartOffset.Init(FIntPoint::ZeroValue, NumCharts);
		int32 Attempts = 0;
		auto TryPack = [&](double Scale, bool bWrite) -> bool
		{
			++Attempts;
			Occupied.Init(0, Words * Resolution);
			for (int32 Y = 0; Y < Resolution; ++Y)
			{
				uint64* Bits = Occupied.GetData() + (int64)Y * Words;
				const bool bEdgeRow = Y < AtlasBorderBand || Y >= Resolution - AtlasBorderBand;
				for (int32 X = 0; X < Resolution; ++X)
				{
					if (bEdgeRow || X < AtlasBorderBand || X >= Resolution - AtlasBorderBand)
					{
						Bits[X >> 6] |= 1ull << (X & 63);
					}
				}
			}
			for (const int32 Chart : Order)
			{
				FFootprint Best;
				int32 BestRotation = INDEX_NONE;
				FIntPoint BestSpot = FIntPoint::ZeroValue;
				int32 BestTop = MAX_int32;
				for (int32 Rotation = 0; Rotation < Orientations; ++Rotation)
				{
					FFootprint F;
					MakeFootprint(Chart, Rotation, Scale, F);
					FIntPoint Spot;
					if (F.Width <= Resolution && F.Height <= Resolution && FindSpot(F, BestTop, Spot))
					{
						BestTop = Spot.Y + F.Height;
						BestRotation = Rotation;
						BestSpot = Spot;
						Best = MoveTemp(F);
					}
				}
				if (BestRotation == INDEX_NONE)
				{
					return false;
				}
				for (int32 Row = 0; Row < Best.Height; ++Row)
				{
					uint64* Bits = Occupied.GetData() + (int64)(BestSpot.Y + Row) * Words;
					for (int32 Index = Best.RowStart[Row]; Index < Best.RowStart[Row + 1]; ++Index)
					{
						for (int32 X = BestSpot.X + Best.Spans[Index].X; X < BestSpot.X + Best.Spans[Index].Y; ++X)
						{
							Bits[X >> 6] |= 1ull << (X & 63);
						}
					}
				}
				if (bWrite)
				{
					ChartRotation[Chart] = BestRotation;
					ChartOffset[Chart] = BestSpot;
				}
			}
			return true;
		};
		const double PackStart = FPlatformTime::Seconds();
		double Low = 0.0;
		double High = 1.0;
		while (TryPack(High, false) && High < 1e4)
		{
			Low = High;
			High *= 2.0;
		}
		while (High - Low > 1e-3 * High)
		{
			const double Mid = 0.5 * (Low + High);
			(TryPack(Mid, false) ? Low : High) = Mid;
		}
		if (Low <= 0.0 || !TryPack(Low, true))
		{
			UE_LOG(LogSoftbodyCarrier, Error, TEXT("Could not fit %d contact map islands into one contact map at resolution %d. Raise Contact Map Resolution."), NumCharts, Resolution);
			return false;
		}
		{
			int32 Rotated[4] = {0, 0, 0, 0};
			int32 Mirrored = 0;
			for (const int32 Rotation : ChartRotation)
			{
				++Rotated[Rotation % 4];
				Mirrored += Rotation >= 4 ? 1 : 0;
			}
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Island packing: scale %.3f, rotated 0/90/180/270 deg = %d/%d/%d/%d (mirrored %d), %d attempts, %.1f s"),
				Low, Rotated[0], Rotated[1], Rotated[2], Rotated[3], Mirrored, Attempts, FPlatformTime::Seconds() - PackStart);
		}
		Out.UVs.SetNum(SourceUVs.Num());
		for (int32 i = 0; i < SourceUVs.Num(); ++i)
		{
			const int32 Chart = VertexChart[i];
			const FVector2d P(SourceUVs[i].X * Resolution, SourceUVs[i].Y * Resolution);
			Out.UVs[i] = (ToLocal(P, Chart, ChartRotation[Chart], Low) + FVector2d(ChartOffset[Chart])) / (double)Resolution;
		}

		// 래스터화: 텍셀 중심을 덮는 삼각형.
		Out.Resolution = Resolution;
		const int32 NumTexels = Resolution * Resolution;
		Out.TexelTri.Init(INDEX_NONE, NumTexels);
		Out.TexelBary.Init(FVector3d::ZeroVector, NumTexels);
		Out.FoldTexels = 0;
		Out.FoldPairs.Reset();
		int32 Overlaps = 0;
		int32 Claimed = 0;
		// 꼭짓점을 나눠 가진 삼각형은 변 위의 칸 한가운데를 함께 덮을 수 있다(접힘이 아니다).
		const auto Adjacent = [&Out](int32 A, int32 B)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				for (int32 j = 0; j < 3; ++j)
				{
					if (Out.UVToVertex[Out.UVIndices[A * 3 + i]] == Out.UVToVertex[Out.UVIndices[B * 3 + j]])
					{
						return true;
					}
				}
			}
			return false;
		};
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const FVector2d P0 = Out.TriUVPixel(Tri, 0);
			const FVector2d P1 = Out.TriUVPixel(Tri, 1);
			const FVector2d P2 = Out.TriUVPixel(Tri, 2);
			const int32 X0 = FMath::Clamp((int32)FMath::FloorToDouble(FMath::Min3(P0.X, P1.X, P2.X)), 0, Resolution - 1);
			const int32 X1 = FMath::Clamp((int32)FMath::CeilToDouble(FMath::Max3(P0.X, P1.X, P2.X)), 0, Resolution - 1);
			const int32 Y0 = FMath::Clamp((int32)FMath::FloorToDouble(FMath::Min3(P0.Y, P1.Y, P2.Y)), 0, Resolution - 1);
			const int32 Y1 = FMath::Clamp((int32)FMath::CeilToDouble(FMath::Max3(P0.Y, P1.Y, P2.Y)), 0, Resolution - 1);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					FVector3d Bary;
					if (!Barycentric2D(FVector2d(X + 0.5, Y + 0.5), P0, P1, P2, Bary)
						|| Bary.X < -1e-7 || Bary.Y < -1e-7 || Bary.Z < -1e-7)
					{
						continue;
					}
					const int32 Texel = Out.Index(X, Y);
					const int32 Existing = Out.TexelTri[Texel];
					if (Existing != INDEX_NONE && Out.TriChart[Existing] != Out.TriChart[Tri])
					{
						++Overlaps;
						continue;
					}
					if (Existing != INDEX_NONE && !Adjacent(Existing, Tri))
					{
						++Out.FoldTexels;
						Out.FoldPairs.Emplace(Existing, Tri);
					}
					Claimed += Existing == INDEX_NONE ? 1 : 0;
					Out.TexelTri[Texel] = Tri;
					Out.TexelBary[Texel] = Bary;
				}
			}
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Contact map: resolution %d, islands %d, triangles %d, packing scale %.3f, surface cells %d, cells where islands overlap %d, cells folded inside an island %d"),
			Resolution, NumCharts, NumTris, Low, Claimed, Overlaps, Out.FoldTexels);
		return true;
	}

	/** 삼각형 무게중심 좌표로 아틀라스 메시(XAtlas면 봉투, UV0 차트면 봉투 슬롯 렌더 삼각형) 위 3D 위치 (연장 좌표도 허용). */
	FVector3d AtlasPoint(const FDynamicMesh3& AtlasMesh, const FCarrierAtlas& Atlas, int32 Tri, const FVector3d& Bary)
	{
		return AtlasMesh.GetVertex(Atlas.UVToVertex[Atlas.UVIndices[Tri * 3]]) * Bary.X
			+ AtlasMesh.GetVertex(Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + 1]]) * Bary.Y
			+ AtlasMesh.GetVertex(Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + 2]]) * Bary.Z;
	}

	FVector2d AtlasUV(const FCarrierAtlas& Atlas, int32 Tri, const FVector3d& Bary)
	{
		return Atlas.UVs[Atlas.UVIndices[Tri * 3]] * Bary.X
			+ Atlas.UVs[Atlas.UVIndices[Tri * 3 + 1]] * Bary.Y
			+ Atlas.UVs[Atlas.UVIndices[Tri * 3 + 2]] * Bary.Z;
	}

	/** UV 점 근처(반경 2칸)에서 같은 차트의 표면 칸 중 가장 가까운 것. 없으면 INDEX_NONE. */
	int32 NearestSurfaceTexel(const FCarrierAtlas& Atlas, const FVector2d& PixelUV, int32 Chart)
	{
		const int32 CX = (int32)FMath::FloorToDouble(PixelUV.X);
		const int32 CY = (int32)FMath::FloorToDouble(PixelUV.Y);
		int32 Best = INDEX_NONE;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 Y = CY - 2; Y <= CY + 2; ++Y)
		{
			for (int32 X = CX - 2; X <= CX + 2; ++X)
			{
				if (X < 0 || Y < 0 || X >= Atlas.Resolution || Y >= Atlas.Resolution)
				{
					continue;
				}
				const int32 Texel = Atlas.Index(X, Y);
				if (Atlas.Kind[Texel] != KindSurface || (Chart != INDEX_NONE && Atlas.TriChart[Atlas.TexelTri[Texel]] != Chart))
				{
					continue;
				}
				const double Distance = FVector2d::DistSquared(FVector2d(X + 0.5, Y + 0.5), PixelUV);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					Best = Texel;
				}
			}
		}
		return Best;
	}

	/**
	 * 고스트 칸(ADR-0007 §3.2): 차트 밖 GhostRings 칸까지. 가장 가까운 표면 칸의 삼각형을 연장한 3D 위치에서 가장 가까운
	 * 아틀라스 메시 점(보통 옆 차트, 열린 가장자리면 제 차트)을 찾아 그 점의 칸 좌표(소수, 정수 = 칸 한가운데)를 가리키고,
	 * 그 점을 이 칸의 점으로 삼는다(TexelTri·TexelBary). 베이크는 위치·노멀·두께·스킨을 그 점에서 구하고, 런타임 이완은
	 * 함몰을 그 좌표에서 bilinear로 가져오며, 나머지 계산은 이 칸에서 한다. 가리킨 좌표를 둘러싼 네 칸이 모두 칸이어야
	 * 하고, 아니면 그 차트에서 가장 가까운 표면 칸을 가리킨다.
	 */
	void BuildGhosts(const FDynamicMesh3& AtlasMesh, const FDynamicMeshAABBTree3& AtlasBVH, FCarrierAtlas& Atlas)
	{
		const int32 R = Atlas.Resolution;
		const int32 NumTexels = R * R;
		Atlas.Kind.Init(KindEmpty, NumTexels);
		Atlas.Redirect.SetNum(NumTexels);
		TArray<int32> Source;
		Source.Init(INDEX_NONE, NumTexels);
		TArray<int32> Ring;
		Ring.Init(MAX_int32, NumTexels);
		TArray<int32> Frontier;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			Atlas.Redirect[Texel] = FVector2d(Texel % R, Texel / R);
			if (Atlas.TexelTri[Texel] != INDEX_NONE)
			{
				Atlas.Kind[Texel] = KindSurface;
				Source[Texel] = Texel;
				Ring[Texel] = 0;
				Frontier.Add(Texel);
			}
		}
		for (int32 Depth = 1; Depth <= GhostRings; ++Depth)
		{
			TArray<int32> Next;
			for (const int32 Texel : Frontier)
			{
				const int32 X = Texel % R;
				const int32 Y = Texel / R;
				const FIntPoint Offsets[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (const FIntPoint& Offset : Offsets)
				{
					const int32 NX = X + Offset.X;
					const int32 NY = Y + Offset.Y;
					if (NX < 0 || NY < 0 || NX >= R || NY >= R)
					{
						continue;
					}
					const int32 Neighbor = Atlas.Index(NX, NY);
					if (Ring[Neighbor] <= Depth)
					{
						continue;
					}
					Ring[Neighbor] = Depth;
					Source[Neighbor] = Source[Texel];
					Next.Add(Neighbor);
				}
			}
			Frontier = MoveTemp(Next);
		}

		// 고리 칸을 먼저 모두 고스트로 표시한다(가리킨 좌표를 둘러싼 칸 검사가 고스트 칸도 칸으로 센다).
		TArray<int32> Ghosts;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] != KindSurface && Source[Texel] != INDEX_NONE)
			{
				Atlas.Kind[Texel] = KindGhost;
				Ghosts.Add(Texel);
			}
		}
		const auto Carried = [&Atlas, R](int32 X, int32 Y)
		{
			return X >= 0 && Y >= 0 && X < R && Y < R && Atlas.Kind[Atlas.Index(X, Y)] != KindEmpty;
		};
		FThreadSafeCounter CrossChart;
		FThreadSafeCounter Snapped;
		FThreadSafeCounter Clamped;
		// 고스트 칸끼리는 서로의 결과를 읽지 않는다(표면 칸의 TexelTri와 모든 칸의 Kind만 읽는다).
		ParallelFor(Ghosts.Num(), [&](int32 Index)
		{
			const int32 Texel = Ghosts[Index];
			const int32 From = Source[Texel];
			const int32 FromTri = Atlas.TexelTri[From];
			const FVector2d Pixel((Texel % R) + 0.5, (Texel / R) + 0.5);
			// 찾지 못하면 출처 표면 칸을 그대로 따른다.
			int32 TargetTri = FromTri;
			FVector3d TargetBary = Atlas.TexelBary[From];
			FVector2d Target(From % R, From / R);
			bool bFound = false;
			FVector3d Bary;
			if (Barycentric2D(Pixel, Atlas.TriUVPixel(FromTri, 0), Atlas.TriUVPixel(FromTri, 1), Atlas.TriUVPixel(FromTri, 2), Bary))
			{
				const FVector3d Extended = AtlasPoint(AtlasMesh, Atlas, FromTri, Bary);
				double DistanceSq = 0.0;
				const int32 NearTri = AtlasBVH.FindNearestTriangle(Extended, DistanceSq);
				if (NearTri != IndexConstants::InvalidID)
				{
					const FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(AtlasMesh, NearTri, Extended);
					const FVector2d PixelUV = AtlasUV(Atlas, NearTri, Query.TriangleBaryCoords) * (double)R;
					const FVector2d Exact = PixelUV - FVector2d(0.5, 0.5);
					const int32 BX = (int32)FMath::FloorToDouble(Exact.X);
					const int32 BY = (int32)FMath::FloorToDouble(Exact.Y);
					if (Carried(BX, BY) && Carried(BX + 1, BY) && Carried(BX, BY + 1) && Carried(BX + 1, BY + 1))
					{
						TargetTri = NearTri;
						TargetBary = Query.TriangleBaryCoords;
						Target = Exact;
						bFound = true;
					}
					else
					{
						const int32 Found = NearestSurfaceTexel(Atlas, PixelUV, Atlas.TriChart[NearTri]);
						if (Found != INDEX_NONE)
						{
							TargetTri = Atlas.TexelTri[Found];
							TargetBary = Atlas.TexelBary[Found];
							Target = FVector2d(Found % R, Found / R);
							bFound = true;
							Snapped.Increment();
						}
					}
					if (bFound && Atlas.TriChart[TargetTri] != Atlas.TriChart[FromTri])
					{
						CrossChart.Increment();
					}
				}
			}
			if (!bFound)
			{
				Clamped.Increment();
			}
			Atlas.Redirect[Texel] = Target;
			Atlas.TexelTri[Texel] = TargetTri;
			Atlas.TexelBary[Texel] = TargetBary;
		});
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Island border cells %d: linked across a seam to the next island %d, nearest cell used instead of the exact point %d, left in place %d"),
			Ghosts.Num(), CrossChart.GetValue(), Snapped.GetValue(), Clamped.GetValue());
		Atlas.GhostSource = MoveTemp(Source);
	}

	/** 균일 격자 최근접 K 정점 (스킨 웨이트 전사용). */
	struct FPointGrid
	{
		double CellSize = 1.0;
		TMap<FIntVector, TArray<int32>> Cells;
		const TArray<FVector3d>* Points = nullptr;

		FIntVector Key(const FVector3d& P) const
		{
			return FIntVector((int32)FMath::FloorToDouble(P.X / CellSize),
				(int32)FMath::FloorToDouble(P.Y / CellSize), (int32)FMath::FloorToDouble(P.Z / CellSize));
		}

		void Build(const TArray<FVector3d>& InPoints, double InCellSize)
		{
			Points = &InPoints;
			CellSize = FMath::Max(InCellSize, 1e-3);
			for (int32 i = 0; i < InPoints.Num(); ++i)
			{
				Cells.FindOrAdd(Key(InPoints[i])).Add(i);
			}
		}

		void FindNearest(const FVector3d& P, int32 K, TArray<TPair<double, int32>>& Out) const
		{
			Out.Reset();
			const FIntVector C = Key(P);
			for (int32 Radius = 0; Radius < 64; ++Radius)
			{
				for (int32 Z = C.Z - Radius; Z <= C.Z + Radius; ++Z)
				{
					for (int32 Y = C.Y - Radius; Y <= C.Y + Radius; ++Y)
					{
						for (int32 X = C.X - Radius; X <= C.X + Radius; ++X)
						{
							if (FMath::Max3(FMath::Abs(X - C.X), FMath::Abs(Y - C.Y), FMath::Abs(Z - C.Z)) != Radius)
							{
								continue;
							}
							if (const TArray<int32>* Cell = Cells.Find(FIntVector(X, Y, Z)))
							{
								for (const int32 Index : *Cell)
								{
									Out.Emplace(FVector3d::DistSquared((*Points)[Index], P), Index);
								}
							}
						}
					}
				}
				// 반경 Radius 껍질까지 모았으면 그보다 가까운 점은 모두 들어왔다.
				if (Out.Num() >= K && Radius > 0)
				{
					const double Covered = (double)Radius * CellSize;
					Out.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
					if (Out[K - 1].Key <= Covered * Covered)
					{
						Out.SetNum(K);
						return;
					}
				}
			}
			Out.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
			if (Out.Num() > K)
			{
				Out.SetNum(K);
			}
		}

		/** 반경 안의 점마다 Visit(점 번호, 거리²). */
		template <typename FVisit>
		void ForEachInRadius(const FVector3d& P, double Radius, FVisit&& Visit) const
		{
			const FIntVector Lo = Key(P - FVector3d(Radius));
			const FIntVector Hi = Key(P + FVector3d(Radius));
			const double RadiusSq = Radius * Radius;
			for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
			{
				for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
				{
					for (int32 X = Lo.X; X <= Hi.X; ++X)
					{
						if (const TArray<int32>* Cell = Cells.Find(FIntVector(X, Y, Z)))
						{
							for (const int32 Index : *Cell)
							{
								const double DistSq = FVector3d::DistSquared((*Points)[Index], P);
								if (DistSq <= RadiusSq)
								{
									Visit(Index, DistSq);
								}
							}
						}
					}
				}
			}
		}
	};

	/**
	 * 봉투를 만들 슬롯 이름(편집용 이름과 임포트 이름 둘 다). CarrierEnvelopeSlots가 있으면 그것, 없으면 소프트바디
	 * 머티리얼이 붙은 슬롯. 비어 있으면 메시 전체를 쓴다는 뜻이다. OutLabel은 로그용.
	 */
	TSet<FName> ResolveEnvelopeSlots(const USoftbodyMeshData* Data, FString& OutLabel)
	{
		TArray<FName> Names;
		TArray<FName> ImportedNames;
		TArray<const UMaterialInterface*> Materials;
		if (Data->SourceSkeletalMesh)
		{
			for (const FSkeletalMaterial& M : Data->SourceSkeletalMesh->GetMaterials())
			{
				Names.Add(M.MaterialSlotName);
				ImportedNames.Add(M.ImportedMaterialSlotName);
				Materials.Add(M.MaterialInterface);
			}
		}
		else if (Data->SourceMesh)
		{
			for (const FStaticMaterial& M : Data->SourceMesh->GetStaticMaterials())
			{
				Names.Add(M.MaterialSlotName);
				ImportedNames.Add(M.ImportedMaterialSlotName);
				Materials.Add(M.MaterialInterface);
			}
		}
		const bool bExplicit = Data->CarrierEnvelopeSlots.Num() > 0;
		TSet<FName> Out;
		OutLabel.Reset();
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			const bool bPicked = bExplicit
				? Data->CarrierEnvelopeSlots.Contains(Names[i]) || Data->CarrierEnvelopeSlots.Contains(ImportedNames[i])
				: SoftbodyBakeShared::ReadsCarrier(Materials[i]);
			if (!bPicked)
			{
				continue;
			}
			for (const FName Name : {Names[i], ImportedNames[i]})
			{
				if (!Name.IsNone())
				{
					Out.Add(Name);
				}
			}
			OutLabel += (OutLabel.IsEmpty() ? TEXT("") : TEXT(", ")) + Names[i].ToString();
		}
		OutLabel = Out.Num() == 0 ? FString(TEXT("whole mesh"))
			: FString::Printf(TEXT("%s: %s"), bExplicit ? TEXT("Pressable Slots") : TEXT("slots with a softbody material"), *OutLabel);
		return Out;
	}

	/**
	 * 봉투를 뽑을 소스 메시: Slots가 비어 있으면 메시 전체, 아니면 그 슬롯 삼각형만. OutVertexInSlots는 메시 설명
	 * 정점 순서마다 그 슬롯 삼각형에 속하는가(캐리어 칸 스킨 웨이트를 가져올 정점 고르기).
	 * Skip(있으면): 메시 설명 삼각형 자리(FTriangleID 값)마다 1이면 뺀다 — 떠 있는 하드 서피스 조각(ADR-0037 §3.7). 봉투가 조각을 덮지 않고
	 * 그 밑 겉면을 따른다. 없으면 지금 그대로다.
	 */
	void BuildEnvelopeSource(const FMeshDescription& MD, const TSet<FName>& Slots, FDynamicMesh3& Out, TArray<bool>& OutVertexInSlots,
		const TArray<uint8>* Skip = nullptr)
	{
		if (Slots.Num() == 0 && !Skip)
		{
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(&MD, Out);
			OutVertexInSlots.Init(true, MD.Vertices().Num());
			return;
		}
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TVertexAttributesConstRef<FVector3f> Positions =
			MD.VertexAttributes().GetAttributesRef<FVector3f>(MeshAttribute::Vertex::Position);
		TMap<FVertexID, int32> Order;
		for (const FVertexID Vertex : MD.Vertices().GetElementIDs())
		{
			Order.Add(Vertex, Order.Num());
		}
		OutVertexInSlots.Init(false, Order.Num());
		TMap<FVertexID, int32> Mapped;
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if ((Slots.Num() > 0 && !Slots.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
				|| (Skip && Skip->IsValidIndex(Tri.GetValue()) && (*Skip)[Tri.GetValue()]))
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(Tri);
			int32 Ids[3];
			for (int32 c = 0; c < 3; ++c)
			{
				if (const int32* Found = Mapped.Find(Corners[c]))
				{
					Ids[c] = *Found;
				}
				else
				{
					Ids[c] = Out.AppendVertex(FVector3d(Positions[Corners[c]]));
					Mapped.Add(Corners[c], Ids[c]);
				}
				OutVertexInSlots[Order[Corners[c]]] = true;
			}
			if (Out.AppendTriangle(Ids[0], Ids[1], Ids[2]) < 0)
			{
				// 비다양체 변: 이 삼각형만 정점을 따로 둔다(감김 수에는 위치만 중요하다)
				for (int32 c = 0; c < 3; ++c)
				{
					Ids[c] = Out.AppendVertex(FVector3d(Positions[Corners[c]]));
				}
				Out.AppendTriangle(Ids[0], Ids[1], Ids[2]);
			}
		}
	}

	/**
	 * 테셀레이션 사이 값 무게(ADR-0006 §3.3)를 텍셀 표 A에 `2 + 무게`로 쓴다.
	 * 나나이트 테셀레이션은 삼각형 안의 점에서 세 코너의 캐리어 UV를 직선 보간해 표면 장을 읽는다. 세 코너가 다른 차트를
	 * 읽으면(또는 같은 차트 안에서 멀리 돌아가면) 보간한 UV가 엉뚱한 칸을 가리키므로, 그런 삼각형(걸친 삼각형)의 코너는
	 * 무게 0, 나머지는 1로 둔다. 같은 자리의 정점은 가장 작은 무게로 맞춘다(두 삼각형이 같은 변에서 같은 값을 내야 틈이
	 * 없다). A가 2 이상이면 이 무게가 있다는 뜻이고, 옛 베이크(A = 1)는 셰이더가 무게 0으로 읽어 지금처럼 그린다.
	 *
	 * 정점 밀도(ADR-0006 §3.3.1, 일지 74): 정점에 붙은 변의 평균 길이를 캐리어 칸 수로 재어 A에 `8 × round(칸 수 × 64)`를
	 * 더한다(1/64 칸 단위, 최대 약 64칸). 표면 장은 칸보다 가는 모양이 없으므로, 삼각형이 칸 한두 개 크기인 곳(손·얼굴)은
	 * 코너가 장을 다 표본한 상태라 쪼갠 점이 장을 다시 읽어도 bilinear 꺾임만 더해진다 — 셰이더가 이 칸 수로 사이 값을 퐁
	 * 곡면으로 바꾼다(문턱은 머티리얼 함수의 고정값 2칸. 칸 수는 이 베이크의 칸 크기로 재므로 캐리어 해상도가 달라져도 문턱은
	 * 같다). 같은 자리의 정점은 가장 짧은 값으로 맞춘다. 셰이더는 A − 2를 8로 나눈 몫을
	 * 칸 수, 나머지(0·1·4·5)를 전과 같이 무게·이음매로 읽는다. 몫이 0인 베이크(이 항목 전)는 밀도 무게 1이다.
	 */
	void WriteTessellationWeights(const FMeshDescription& MD, const TSet<FName>& Slots,
		const TArray<FVertexID>& VertexOrder, const TArray<FVector3d>& VertexPositions,
		const TArray<int32>& VertexChart, const TArray<FVector3d>& VertexCarrierPoint,
		const TArray<FVector2d>& VertexCarrierUV, int32 Resolution, double TexelSizeCm,
		TArray<FLinearColor>& TableTexels)
	{
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TMap<FVertexID, int32> Order;
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			Order.Add(VertexOrder[i], i);
		}
		TArray<float> Weight;
		Weight.Init(1.f, VertexOrder.Num());
		TArray<double> EdgeSum;
		TArray<int32> EdgeCount;
		EdgeSum.Init(0.0, VertexOrder.Num());
		EdgeCount.Init(0, VertexOrder.Num());
		int32 SoftTris = 0;
		int32 ChartStraddle = 0;
		int32 WrapStraddle = 0;
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if (Slots.Num() > 0 && !Slots.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
			{
				continue;
			}
			++SoftTris;
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(Tri);
			const int32 Ids[3] = {Order[Corners[0]], Order[Corners[1]], Order[Corners[2]]};
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 A = Ids[c];
				const int32 B = Ids[(c + 1) % 3];
				const double Len = FVector3d::Distance(VertexPositions[A], VertexPositions[B]);
				EdgeSum[A] += Len;
				EdgeSum[B] += Len;
				++EdgeCount[A];
				++EdgeCount[B];
			}
			bool bStraddle = false;
			for (int32 c = 0; c < 3; ++c)
			{
				bStraddle |= VertexChart[Ids[c]] == INDEX_NONE || VertexChart[Ids[c]] != VertexChart[Ids[0]];
			}
			ChartStraddle += bStraddle ? 1 : 0;
			if (!bStraddle)
			{
				// 같은 차트여도 차트가 휘어 돌아가면 UV 사이가 3D 거리보다 훨씬 길다(보간 UV가 차트 밖을 지난다).
				for (int32 c = 0; c < 3 && !bStraddle; ++c)
				{
					const int32 A = Ids[c];
					const int32 B = Ids[(c + 1) % 3];
					const double UVCm = FVector2d::Distance(VertexCarrierUV[A], VertexCarrierUV[B]) * Resolution * TexelSizeCm;
					const double Cm = FVector3d::Distance(VertexCarrierPoint[A], VertexCarrierPoint[B]);
					bStraddle = UVCm > 2.0 * Cm + 2.0 * TexelSizeCm;
				}
				WrapStraddle += bStraddle ? 1 : 0;
			}
			if (bStraddle)
			{
				for (const int32 Id : Ids)
				{
					Weight[Id] = 0.f;
				}
			}
		}
		// 변 길이(칸 수): 소프트바디 슬롯 삼각형이 하나도 안 붙은 정점은 정보 없음(0 → 셰이더가 밀도 무게 1로 읽는다)
		const double MaxEdgeTexels = 4095.0 / 64.0;
		TArray<double> EdgeTexels;
		EdgeTexels.Init(0.0, VertexOrder.Num());
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			if (EdgeCount[i] > 0)
			{
				EdgeTexels[i] = FMath::Clamp(EdgeSum[i] / EdgeCount[i] / FMath::Max(TexelSizeCm, 1e-6), 0.0, MaxEdgeTexels);
			}
		}
		// 같은 자리 정점(층·분리된 이음매)은 가장 작은 무게·가장 짧은 변으로
		TMap<FIntVector, float> ByPosition;
		TMap<FIntVector, double> EdgeByPosition;
		const auto Key = [](const FVector3d& P)
		{
			return FIntVector((int32)FMath::RoundToDouble(P.X * 1000.0), (int32)FMath::RoundToDouble(P.Y * 1000.0),
				(int32)FMath::RoundToDouble(P.Z * 1000.0));
		};
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			float& Min = ByPosition.FindOrAdd(Key(VertexPositions[i]), 1.f);
			Min = FMath::Min(Min, Weight[i]);
			if (EdgeCount[i] > 0)
			{
				double& MinEdge = EdgeByPosition.FindOrAdd(Key(VertexPositions[i]), MaxEdgeTexels);
				MinEdge = FMath::Min(MinEdge, EdgeTexels[i]);
			}
		}
		int32 Reduced = 0;
		int32 Valid = 0;
		TArray<double> EdgeStats;
		for (int32 i = 0; i < VertexOrder.Num() && i < TableTexels.Num(); ++i)
		{
			if (TableTexels[i].A < 0.5f)
			{
				continue;
			}
			const float W = ByPosition[Key(VertexPositions[i])];
			const double* Edge = EdgeByPosition.Find(Key(VertexPositions[i]));
			const int32 Q = Edge ? FMath::Clamp(FMath::RoundToInt(*Edge * 64.0), 0, 4095) : 0;
			TableTexels[i].A = 2.f + W + 8.f * (float)Q;
			++Valid;
			Reduced += W < 1.f ? 1 : 0;
			if (Edge)
			{
				EdgeStats.Add(*Edge);
			}
		}
		UE_LOG(LogSoftbodyCarrier, Display,
			TEXT("Tessellation weight: of %d softbody slot triangles, %d (%.1f%%) cross an island border and %d (%.1f%%) wrap around inside an island; of %d vertices, %d (%.1f%%) have weight 0"),
			SoftTris, ChartStraddle, 100.0 * ChartStraddle / FMath::Max(SoftTris, 1),
			WrapStraddle, 100.0 * WrapStraddle / FMath::Max(SoftTris, 1),
			Valid, Reduced, 100.0 * Reduced / FMath::Max(Valid, 1));
		if (EdgeStats.Num() > 0)
		{
			EdgeStats.Sort();
			const auto At = [&](double Fraction)
			{
				return EdgeStats[FMath::Clamp((int32)(Fraction * (EdgeStats.Num() - 1)), 0, EdgeStats.Num() - 1)];
			};
			int32 Below1 = 0;
			int32 Below2 = 0;
			int32 Below4 = 0;
			for (const double E : EdgeStats)
			{
				Below1 += E <= 1.0 ? 1 : 0;
				Below2 += E <= 2.0 ? 1 : 0;
				Below4 += E <= 4.0 ? 1 : 0;
			}
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Vertex density (mean length of attached edges, in contact map cells of %.3f cm): min %.2f, 5%% %.2f, median %.2f, 95%% %.2f, max %.2f cells; at most 1 cell %.1f%%, at most 2 cells %.1f%%, at most 4 cells %.1f%%"),
				TexelSizeCm, EdgeStats[0], At(0.05), At(0.5), At(0.95), EdgeStats.Last(),
				100.0 * Below1 / EdgeStats.Num(), 100.0 * Below2 / EdgeStats.Num(), 100.0 * Below4 / EdgeStats.Num());
		}
	}

	/**
	 * 칸마다 그 자리 렌더 삼각형 한 변의 길이(메시 로컬 단위)를 CarrierRedirect B에 쓴다(ADR-0010 §3.6). 솟는 띠가 이보다
	 * 좁으면 원래 정점만 움직이는 곳(차트 경계를 걸친 삼각형, 테셀레이션이 꺼지는 거리)에서 정점에 걸린 봉우리가 되므로 셰이더가
	 * 띠 거리의 하한으로 쓴다. 정점마다 붙은 변 길이 평균을 구하고, 칸마다 가까운 정점 8개를 9번째 정점까지 거리에서 0이 되는
	 * 무게로 섞는다 — 가까운 정점이 바뀌는 자리에서 들어오고 나가는 정점의 무게가 0이라 값이 끊기지 않는다. 고스트 칸도
	 * 자기가 가리키는 점(옆 차트)의 위치에서 구한다. 칠을 칸으로 옮길 때 다듬는 반경으로도 쓴다(PaintCellsFromVertices).
	 * 정점이 9개보다 적으면 모두 0이다.
	 */
	TArray<float> ComputeRenderEdgeLengths(const FMeshDescription& MD, const TSet<FName>& Slots, const TArray<FVertexID>& VertexOrder,
		const TArray<FVector3d>& VertexPositions, const FCarrierAtlas& Atlas, const TArray<FVector3d>& TexelPosition)
	{
		TArray<float> TexelEdge;
		TexelEdge.Init(0.f, Atlas.Kind.Num());
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TMap<FVertexID, int32> Order;
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			Order.Add(VertexOrder[i], i);
		}
		TArray<double> EdgeSum;
		TArray<int32> EdgeCount;
		EdgeSum.Init(0.0, VertexOrder.Num());
		EdgeCount.Init(0, VertexOrder.Num());
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if (Slots.Num() > 0 && !Slots.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(Tri);
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 A = Order[Corners[c]];
				const int32 B = Order[Corners[(c + 1) % 3]];
				const double Length = FVector3d::Distance(VertexPositions[A], VertexPositions[B]);
				EdgeSum[A] += Length;
				EdgeSum[B] += Length;
				++EdgeCount[A];
				++EdgeCount[B];
			}
		}
		TArray<FVector3d> Points;
		TArray<double> Lengths;
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			if (EdgeCount[i] > 0)
			{
				Points.Add(VertexPositions[i]);
				Lengths.Add(EdgeSum[i] / EdgeCount[i]);
			}
		}
		if (Points.Num() < 9)
		{
			return TexelEdge;
		}
		TArray<double> SortedLengths = Lengths;
		SortedLengths.Sort();
		FPointGrid Grid;
		Grid.Build(Points, FMath::Max(SortedLengths[SortedLengths.Num() / 2] * 2.0, 1e-2));
		const int32 NumTexels = TexelEdge.Num();
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty)
			{
				return;
			}
			TArray<TPair<double, int32>> Nearest;
			Grid.FindNearest(TexelPosition[Texel], 9, Nearest);
			if (Nearest.Num() < 9)
			{
				return;
			}
			const double Cutoff = FMath::Sqrt(Nearest[8].Key);
			double WeightSum = 0.0;
			double Sum = 0.0;
			for (int32 k = 0; k < 8; ++k)
			{
				const double Weight = FMath::Square(1.0 - FMath::Sqrt(Nearest[k].Key) / FMath::Max(Cutoff, 1e-6));
				WeightSum += Weight;
				Sum += Weight * Lengths[Nearest[k].Value];
			}
			TexelEdge[Texel] = WeightSum > 0.0 ? (float)(Sum / WeightSum) : (float)Lengths[Nearest[0].Value];
		});
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Render triangle edge length (per-vertex mean): 10%% %.2f / 50%% %.2f / 90%% %.2f"),
			SortedLengths[SortedLengths.Num() / 10], SortedLengths[SortedLengths.Num() / 2], SortedLengths[SortedLengths.Num() * 9 / 10]);
		return TexelEdge;
	}

	/**
	 * 정점마다 칠한 값(VertexValue, 렌더 정점 순서, 음수 = 읽지 못함)을 캐리어 칸으로 옮긴다(일지 209). 두 걸음이다.
	 * 1) 칸 자리 렌더 삼각형의 세 정점을 무게중심 좌표로 섞는다. 메시가 그리는 칠 그대로라 값이 끊기지 않는다. 전에 쓰던 "가까운 정점
	 *    4개의 역거리 평균"은 넷이 바뀌는 자리마다 값이 뛰어, 살 두께에 정점 간격의 마름모 계단을 남겼다(덩어리 값 일지 208과 같은 꼴).
	 * 2) 그 값을 이웃 칸에서 다시 섞는다: 반경 = 그 자리 렌더 변 길이(Edge, 최소 MinRadius), C2 웬들런드 무게, 같은 쪽을 보는 겉면
	 *    칸만(얇은 곳의 반대쪽 면을 섞지 않게). 무게중심 보간은 삼각형 변마다 기울기가 꺾이고, 살 두께는 누름이 멈추는 바닥(그리고
	 *    솟음 높이의 한도)이라 그 꺾임이 음영에 나온다. 정점 칠에는 변 길이보다 잔 결이 없으므로 그만큼 섞어도 잃는 그림이 없고, 결과가
	 *    삼각형을 어떻게 나눴는지에 덜 묶인다(같은 칠·같은 밀도의 다른 메시 둘에서 음영 차 rms 3.9 → 2.2°, 꺾임 99% 7 → 4°, 모의).
	 *    0인 칸은 0 그대로이고, 반경을 가장 가까운 0인 칸까지로 줄인다: 살이 없는 칸(눌리는 정도 0 — 솟음이 지나가지 못하는 벽,
	 *    ADR-0016 §5.2)의 집합이 1)과 같고, 그 가장자리에서 값이 0으로 이어진다.
	 * 칠이 있는 정점이 없는 칸은 Fallback. 결과 Out은 칸 수만큼. 모든 무게 안 값이 같으면 그 값 그대로(칠 1인 곳은 정확히 1).
	 */
	void PaintCellsFromVertices(const TArray<double>& VertexValue, double Fallback, const TArray<FIntVector>& Corners,
		const TArray<FVector3d>& Bary, const TArray<uint8>& Kind, const TArray<FVector3d>& Position, const TArray<FVector3d>& Normal,
		const TArray<float>& Edge, double MinRadius, TArray<double>& Out, double& OutMaxChange)
	{
		const int32 Num = Kind.Num();
		TArray<double> Raw;
		Raw.Init(Fallback, Num);
		ParallelFor(Num, [&](int32 T)
		{
			if (Kind[T] == KindEmpty || Corners[T].X == INDEX_NONE)
			{
				return;
			}
			double Sum = 0.0;
			double WeightSum = 0.0;
			int32 Present = INDEX_NONE;
			for (int32 c = 0; c < 3; ++c)
			{
				const double Value = VertexValue.IsValidIndex(Corners[T][c]) ? VertexValue[Corners[T][c]] : -1.0;
				if (Value < 0.0)
				{
					continue;
				}
				const double Weight = FMath::Max(Bary[T][c], 0.0);
				Sum += Weight * Value;
				WeightSum += Weight;
				Present = c;
			}
			if (WeightSum > 1e-12)
			{
				Raw[T] = Sum / WeightSum;
			}
			else if (Present != INDEX_NONE)
			{
				Raw[T] = VertexValue[Corners[T][Present]];
			}
		});

		TArray<FVector3d> Points;
		TArray<int32> PointTexel;
		TArray<float> Radii;
		for (int32 T = 0; T < Num; ++T)
		{
			if (Kind[T] == KindSurface && !Normal[T].IsZero())
			{
				Points.Add(Position[T]);
				PointTexel.Add(T);
				Radii.Add(FMath::Max(Edge[T], (float)MinRadius));
			}
		}
		Out = Raw;
		OutMaxChange = 0.0;
		if (Points.Num() == 0)
		{
			return;
		}
		Radii.Sort();
		FPointGrid Grid;
		Grid.Build(Points, FMath::Max((double)Radii[Radii.Num() / 2], MinRadius));
		TArray<double> Change;
		Change.Init(0.0, Num);
		ParallelFor(Num, [&](int32 T)
		{
			if (Kind[T] == KindEmpty || Normal[T].IsZero() || Raw[T] <= 0.0)
			{
				return;
			}
			const FVector3d& N = Normal[T];
			double Radius = FMath::Max((double)Edge[T], MinRadius);
			Grid.ForEachInRadius(Position[T], Radius, [&](int32 i, double DistSq)
			{
				const int32 S = PointTexel[i];
				if (Raw[S] <= 0.0 && Normal[S].Dot(N) > 0.0)
				{
					Radius = FMath::Min(Radius, FMath::Sqrt(DistSq));
				}
			});
			double Sum = 0.0;
			double WeightSum = 0.0;
			double Lo = Raw[T];
			double Hi = Raw[T];
			Grid.ForEachInRadius(Position[T], Radius, [&](int32 i, double DistSq)
			{
				const int32 S = PointTexel[i];
				if (Normal[S].Dot(N) <= 0.0)
				{
					return;
				}
				const double Q = FMath::Sqrt(DistSq) / Radius;
				if (Q >= 1.0)
				{
					return;
				}
				const double OneMinus = 1.0 - Q;
				const double Weight = OneMinus * OneMinus * OneMinus * OneMinus * (4.0 * Q + 1.0);
				Sum += Weight * Raw[S];
				WeightSum += Weight;
				Lo = FMath::Min(Lo, Raw[S]);
				Hi = FMath::Max(Hi, Raw[S]);
			});
			if (WeightSum > 0.0)
			{
				Out[T] = Lo == Hi ? Lo : Sum / WeightSum;
				Change[T] = FMath::Abs(Out[T] - Raw[T]);
			}
		});
		for (const double C : Change)
		{
			OutMaxChange = FMath::Max(OutMaxChange, C);
		}
	}

	/** 메시 UV0 아일랜드 차트 (ADR-0007 §3.1). */
	struct FMeshUV0Charts
	{
		/** 봉투 슬롯 렌더 삼각형(위치 = 메시 정점). 삼각형 번호 = 아틀라스 삼각형 번호. */
		FDynamicMesh3 Mesh;
		/** 아틀라스 삼각형 → 메시 삼각형. */
		TArray<FTriangleID> MeshTriangle;
		/**
		 * 메시 정점 번호 → 텍셀 표가 쓸 UV 요소. 이음매 정점은 차트 번호가 가장 작은 아일랜드의 요소라, 이음매 양쪽에서
		 * 나뉜 렌더 정점이 코너 WPO에서 같은 점을 읽는다.
		 */
		TMap<int32, int32> VertexElement;
		/** UV 요소((정점, UV0) 쌍)마다 차트와 메시 정점 번호. */
		TArray<int32> ElementChart;
		TArray<int32> ElementVertex;
		int32 NumCharts = 0;
		/** 봉투 슬롯 삼각형 면적 합(겹친 층 포함). */
		double Area = 0.0;
	};

	/**
	 * 하드 서피스 조각(ADR-0037 §3.7) 굽기 재료: 칠 단계(FindHardPieces)가 적은 메시 설명 삼각형마다 조각 번호와 조각마다 떠 있는가.
	 * 비어 있으면 조각이 없다 — 굽기의 하드 서피스 갈래를 모두 건너뛴다.
	 */
	struct FHardPieceBake
	{
		/** 메시 설명 삼각형 자리(FTriangleID 값) → 조각 번호(0 = 없음, 1부터). */
		TArray<int32> TrianglePiece;
		/** 조각 번호 − 1 → 떠 있는가(다른 겉면 위에 따로 놓인 장식). */
		TArray<uint8> Floating;
		/** 메시 설명 삼각형 자리 × 3 + 코너 → Softness(칠 단계 FindHardPieces). 비어 있으면 모든 판을 검정으로 굽는다(일지 298 전 칠 단계). */
		TArray<float> TriangleSoftness;
		/** 메시 설명 삼각형 자리 → 봉투에서 뺌(떠 있는 조각 — 봉투가 그 밑 겉면을 따른다). */
		TArray<uint8> SkipEnvelope;
		/** 떠 있는 조각마다(번호 − 1) 그 밑 겉면의 바깥 노멀(봉투에서 조각 가운데에 가장 가까운 점). 조각 칸을 밑 겉면에 붙이는 광선 방향. */
		TArray<FVector3d> Below;

		int32 Num() const { return Floating.Num(); }
		bool AnyFloating() const { return Floating.Contains(1); }
		bool IsFloating(int32 Piece) const { return Piece > 0 && Floating.IsValidIndex(Piece - 1) && Floating[Piece - 1] != 0; }
		int32 PieceOf(FTriangleID Tri) const { return TrianglePiece.IsValidIndex(Tri.GetValue()) ? TrianglePiece[Tri.GetValue()] : 0; }
		double CornerSoftness(FTriangleID Tri, int32 Corner) const
		{
			const int32 i = Tri.GetValue() * 3 + Corner;
			return TriangleSoftness.IsValidIndex(i) ? FMath::Clamp((double)TriangleSoftness[i], 0.0, 1.0) : 0.0;
		}
	};

	/** 하드 서피스 칸·조각 표(BuildHardPieceTexels의 결과). */
	struct FHardPieceTexels
	{
		/** 칸마다 조각 번호(1부터, 0 = 조각 아님). */
		TArray<uint16> Piece;
		/** 칸마다 접촉 시작점 올림 ÷ LiftScale × 65535(떠 있는 조각이 없으면 비어 있다). */
		TArray<uint16> Lift;
		float LiftScale = 0.f;
		TArray<FSoftbodyHardPiece> Pieces;
		TArray<uint32> Cells;
		TArray<uint32> Tiles;
		/** 칠한 조각마다(번호 − 1): 1부터 = 남은 판 번호, 음수 = 살로 남은 까닭(SoftbodyBakeShared::HardDropped*). 판이 하나도 없어도 채운다. */
		TArray<int32> Results;
		/** 칸마다 Softness × 65535(판 칸만 칠 값, 나머지 65535 — CarrierSoftnessTex). */
		TArray<uint16> Softness;
		/** 휘는 판 표(USoftbodyMeshData의 Hard* 같은 이름, ADR-0038 §4). */
		TArray<FVector4f> CellPlate;
		TArray<FUintVector2> CellGradRange;
		TArray<FVector4f> GradEntries;
		TArray<FVector4f> Nodes;
		TArray<uint32> NodeSrc;
		TArray<int32> NodeSlots;
		TArray<uint16> ModeShapes;
		TArray<FVector2f> Modes;
	};

	/**
	 * 하드 서피스 조각의 감김 판정(일지 286·288): 발자국 칸 미는 방향 평균의 길이(한쪽을 보는가)의 하한. 판은 조각 칸마다 그 칸의 미는 방향으로
	 * 같은 1차식만큼 움직이므로, 방향이 고르면 겉면이 조금 굽거나 층 사이에 걸쳐도 강체처럼 움직이고, 고리처럼 사방을 보면 앞을 누를 때 뒤가 들뜬다.
	 * 목·허리를 두른 고리 0.66~0.71, 몸을 감은 팔 띠 0.28~0.61, 판 0.8~1.0. 발자국이 평면에서 먼 정도로는 정하지 않는다 — 미는 방향이 겉면
	 * 노멀에서 최대 80°쯤 기울어 평평한 조각도 그 평면에서 멀어 보이고(한쪽을 보는 정도 0.96~1.00인 단추·옷깃·가슴 금속이 빠졌다), 모양에 맞춘
	 * 평면으로 재도 판의 움직임을 정하는 양이 아니다.
	 */
	constexpr double HardFlatFacing = 0.8;

	/**
	 * 조각 자기 칠(실행 중 강체 판 분류 FSoftbodyHardPiece::MaxSoftness, ADR-0038 §4.2): 칠 단계 조각(번호 − 1)마다 판 테두리가 아닌 정점 코너의 가장 큰
	 * Flex. 테두리 정점 = 같은 자리(0.001 메시 로컬 단위, FindHardPieces와 같은 용접)를 이 조각 밖 삼각형과 나누는 정점이다 — 칠 단계는 코너 칠을
	 * 텍스처에서 쌍선형으로 읽어(U × (W − 1)) 테두리 정점에서는 바깥 흰 칠과 섞인 회색이 되므로, 검정으로 칠한 조각도 그 코너 탓에 회색 조각이 되었다.
	 * 테두리가 아닌 정점이 없는 조각(모든 정점이 테두리인 작은 조각)은 모든 코너의 중앙값. 칠 단계가 코너 Flex를 적지 않았거나 삼각형이 없는 조각은 −1.
	 */
	TArray<double> PieceOwnFlex(const FMeshDescription& MD, const FHardPieceBake& Hard)
	{
		TArray<double> Own;
		Own.Init(-1.0, Hard.Num());
		if (Hard.Num() == 0 || Hard.TriangleSoftness.Num() != Hard.TrianglePiece.Num() * 3)
		{
			return Own;
		}
		const TVertexAttributesConstRef<FVector3f> Positions = MD.GetVertexPositions();
		const auto KeyOf = [&Positions](FVertexID Vertex)
		{
			const FVector3f P = Positions[Vertex] * 1000.f;
			return FIntVector(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z));
		};
		// 자리마다 그 자리를 쓰는 삼각형의 조각 번호(0 = 조각 아님). 다른 번호의 삼각형도 그 자리를 쓰면 −1(테두리).
		TMap<FIntVector, int32> Owner;
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			const int32 P = Hard.PieceOf(Tri);
			for (const FVertexID Corner : MD.GetTriangleVertices(Tri))
			{
				const FIntVector Key = KeyOf(Corner);
				if (int32* Found = Owner.Find(Key))
				{
					*Found = *Found == P ? P : -1;
				}
				else
				{
					Owner.Add(Key, P);
				}
			}
		}
		TArray<TArray<double>> All;
		All.SetNum(Hard.Num());
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			const int32 P = Hard.PieceOf(Tri);
			if (P <= 0 || P > Hard.Num())
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(Tri);
			for (int32 c = 0; c < 3; ++c)
			{
				const double S = Hard.CornerSoftness(Tri, c);
				All[P - 1].Add(S);
				if (Owner.FindChecked(KeyOf(Corners[c])) == P)
				{
					Own[P - 1] = FMath::Max(Own[P - 1], S);
				}
			}
		}
		for (int32 p = 0; p < Hard.Num(); ++p)
		{
			if (Own[p] < 0.0 && All[p].Num() > 0)
			{
				All[p].Sort();
				Own[p] = All[p][All[p].Num() / 2];
			}
		}
		return Own;
	}

	/**
	 * 하드 서피스 칸 번호·접촉 시작점 올림·조각 표(ADR-0037 §3.7). 조각 하나의 발자국 = 조각이 얹힌 겉면 칸:
	 *  - 떠 있는 조각(보석): 표면 칸에서 미는 방향 광선이 MaxGap 안에서 그 조각을 처음 맞으면(조각 밑면이 바로 위) 발자국. 올림 = 그 조각의
	 *    가장 먼 맞음(윗면, MaxLift 안)까지. 멀리서 조각을 맞는 칸(가슴 비탈에서 비스듬히)은 조각이 얹힌 칸이 아니다.
	 *  - 겉면에 칠한 조각(주사위 눈): 칸 자리 렌더 삼각형의 세 정점이 모두 그 조각인 표면 칸.
	 * 칠한 조각만 발자국을 한 칸 넓힌다(머티리얼이 조각 가장자리 정점에서 바깥 무른 칸을 쌍선형으로 섞지 않게 — 넓힌 칸의 올림은 0). 떠 있는 장식은 넓히지
	 * 않는다(넓힌 셔츠 칸이 판에 붙어 장식이 들리면 장식 둘레 셔츠가 같이 솟는다 — 장식 렌더 정점은 판 칸만 읽는다). 고스트는 가리키는
	 * 칸의 번호·올림을 쓴다. 조각 표는 발자국 칸 자리(정지, 메시 로컬)에서: 가운데, 노멀(칸 미는 방향 평균), 평면 두 축(주축), 반지름, 자유도
	 * (선 모양이면 짧은 축 기울기를 뺌), 발자국 거친 칸(묶음 Block)과 함몰 묶음(16칸). 한쪽을 보지 않는 발자국(감긴 띠·고리 —
	 * HardFlatFacing)은 짐작해 나누지 않고 뺀다(P05 — 칠에 틈을 두어 나누라고 조각마다 알린다).
	 * 휘는 판(ADR-0038 §4, 일지 298): 칸마다 Softness — 겉면에 칠한 칸은 TexelPaintSoftness(칠을 Depth와 같이 렌더 삼각형 보간으로 옮긴 값),
	 * 떠 있는 조각 칸은 광선이 맞은 조각 삼각형 자리의 칠, 넓힌 한 칸 고리는 안쪽 이웃 판 칸의 가장 작은 값(가장 단단한 값), 고스트는 가리키는
	 * 칸 값. 판마다 SoftbodyPlateBake가 국소 판·마디·판 모드를 굽는다(TexelSize = 캐리어 칸 한 변, HRef = 굽힘 길이 눈금). 발자국 거친 칸이
	 * SoftbodyPlateBake::MaxCells보다 많은 판과 접촉 지도 가장자리에서 판을 읽지 못하는 판은 뺀다. 남은 조각은 번호를 다시 1부터 매기고, 칠한
	 * 조각마다 결과를 Out.Results에 남긴다. 조각이 하나도 남지 않으면 false.
	 * 판을 그리는 칸(fb/orn 안 A, 판 5): OrnamentPiece = 칸마다 그 칸이 속한 떠 있는 조각(칠 단계 번호, 0 = 아님 — 겹친 층 단계가 정함, 비어 있으면
	 * 장식 칸이 없다), OrnamentOwnPoint 1 = 칸 위치가 제 장식 점인 칸(올림 0). 장식 칸은 가리킨 칸이 아니라 제 판 번호에 비트 15(DrawPlateBit)를 받는다.
	 * 장식 칸이 하나도 없는 떠 있는 판은 발자국 칸(과 그 번호를 받은 고스트)이 비트 15를 받는다 — 장식 정점이 그 칸을 읽는다.
	 */
	bool BuildHardPieceTexels(const FMeshDescription& MD, const FHardPieceBake& Hard, const TArray<FVertexID>& VertexOrder,
		const FCarrierAtlas& Atlas, const TArray<FIntVector>& TexelCorners, const TArray<FVector3d>& TexelPosition,
		const TArray<FVector3d>& TexelNormal, const TArray<double>& TexelPaintSoftness, const TArray<double>& TexelThickness, int32 Resolution,
		int32 Block, double TexelSize, double HRef, double MaxGap, double MaxLift, const TArray<int32>& OrnamentPiece,
		const TArray<uint8>& OrnamentOwnPoint, FHardPieceTexels& Out)
	{
		const int32 R = Resolution;
		const int32 NumTexels = R * R;
		const TVertexAttributesConstRef<FVector3f> Positions = MD.GetVertexPositions();
		TArray<int32> Piece;
		Piece.Init(0, NumTexels);
		TArray<double> Lift;
		Lift.Init(0.0, NumTexels);
		// 칸마다 Softness(판 칸만 뜻이 있다).
		TArray<double> Soft;
		Soft.Init(1.0, NumTexels);

		// 겉면에 칠한 조각: 정점마다 조각 번호(떠 있지 않은 조각의 삼각형이 닿는 정점).
		TArray<int32> VertexPiece;
		VertexPiece.Init(0, MD.Vertices().GetArraySize());
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			const int32 P = Hard.PieceOf(Tri);
			if (P > 0 && !Hard.IsFloating(P))
			{
				for (const FVertexID Corner : MD.GetTriangleVertices(Tri))
				{
					VertexPiece[Corner.GetValue()] = P;
				}
			}
		}
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			const FIntVector& C = TexelCorners[Texel];
			if (Atlas.Kind[Texel] != KindSurface || C.X == INDEX_NONE || C.Y == INDEX_NONE || C.Z == INDEX_NONE)
			{
				continue;
			}
			const int32 P0 = VertexPiece[VertexOrder[C.X].GetValue()];
			if (P0 > 0 && VertexPiece[VertexOrder[C.Y].GetValue()] == P0 && VertexPiece[VertexOrder[C.Z].GetValue()] == P0)
			{
				Piece[Texel] = P0;
				Soft[Texel] = TexelPaintSoftness.IsValidIndex(Texel) && TexelPaintSoftness[Texel] >= 0.0 ? FMath::Min(TexelPaintSoftness[Texel], 1.0) : 0.0;
			}
		}

		// 떠 있는 조각: 조각 삼각형만 담은 메시에 표면 칸에서 광선을 쏜다.
		if (Hard.AnyFloating())
		{
			FDynamicMesh3 PieceMesh;
			TArray<int32> PieceOfTri;
			TArray<FTriangleID> SourceOfTri;
			for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
			{
				const int32 P = Hard.PieceOf(Tri);
				if (!Hard.IsFloating(P))
				{
					continue;
				}
				const TArrayView<const FVertexID> C = MD.GetTriangleVertices(Tri);
				const int32 A = PieceMesh.AppendVertex(FVector3d(Positions[C[0]]));
				const int32 B = PieceMesh.AppendVertex(FVector3d(Positions[C[1]]));
				const int32 D = PieceMesh.AppendVertex(FVector3d(Positions[C[2]]));
				const int32 Added = PieceMesh.AppendTriangle(A, B, D);
				if (Added >= 0)
				{
					PieceOfTri.SetNum(FMath::Max(PieceOfTri.Num(), Added + 1));
					PieceOfTri[Added] = P;
					SourceOfTri.SetNum(FMath::Max(SourceOfTri.Num(), Added + 1));
					SourceOfTri[Added] = Tri;
				}
			}
			FDynamicMeshAABBTree3 PieceBVH(&PieceMesh, true);
			ParallelFor(NumTexels, [&](int32 Texel)
			{
				if (Atlas.Kind[Texel] != KindSurface || Piece[Texel] > 0 || TexelNormal[Texel].IsZero())
				{
					return;
				}
				TArray<MeshIntersection::FHitIntersectionResult> Hits;
				IMeshSpatial::FQueryOptions Options;
				Options.MaxDistance = MaxLift;
				PieceBVH.FindAllHitTriangles(FRay3d(TexelPosition[Texel], TexelNormal[Texel]), Hits, Options);
				int32 Nearest = 0;
				int32 NearestHit = INDEX_NONE;
				double NearestDistance = MaxLift;
				for (int32 h = 0; h < Hits.Num(); ++h)
				{
					const MeshIntersection::FHitIntersectionResult& Hit = Hits[h];
					if (Hit.Distance >= 0.0 && Hit.Distance < NearestDistance)
					{
						NearestDistance = Hit.Distance;
						Nearest = PieceOfTri[Hit.TriangleId];
						NearestHit = h;
					}
				}
				if (Nearest == 0 || NearestDistance > MaxGap)
				{
					return;
				}
				double Top = 0.0;
				for (const MeshIntersection::FHitIntersectionResult& Hit : Hits)
				{
					if (Hit.Distance >= 0.0 && Hit.Distance <= MaxLift && PieceOfTri[Hit.TriangleId] == Nearest)
					{
						Top = FMath::Max(Top, Hit.Distance);
					}
				}
				Piece[Texel] = Nearest;
				Lift[Texel] = Top;
				// 조각 밑면(처음 맞은 삼각형) 그 자리의 칠. 무게중심 좌표가 이상하면 세 코너의 평균.
				const MeshIntersection::FHitIntersectionResult& Hit = Hits[NearestHit];
				const FTriangleID Source = SourceOfTri[Hit.TriangleId];
				FVector3d Bary = Hit.BaryCoords;
				if (!(Bary.X >= -1e-3 && Bary.Y >= -1e-3 && Bary.Z >= -1e-3 && FMath::Abs(Bary.X + Bary.Y + Bary.Z - 1.0) < 1e-2))
				{
					Bary = FVector3d(1.0 / 3.0);
				}
				Soft[Texel] = FMath::Clamp(Bary.X * Hard.CornerSoftness(Source, 0) + Bary.Y * Hard.CornerSoftness(Source, 1)
					+ Bary.Z * Hard.CornerSoftness(Source, 2), 0.0, 1.0);
			});
		}

		// 한 칸 넓힘(표면 칸, 아직 조각 아닌 칸만, 올림 0) — 칠한 조각만(떠 있는 장식은 넓히지 않는다). Softness는 안쪽 이웃 판 칸의 가장 작은 값
		// (가장 단단한 값) — 판 가장자리가 무르게 읽히지 않게.
		{
			const TArray<int32> Before = Piece;
			for (int32 Y = 0; Y < R; ++Y)
			{
				for (int32 X = 0; X < R; ++X)
				{
					const int32 Texel = Y * R + X;
					if (Before[Texel] > 0 || Atlas.Kind[Texel] != KindSurface)
					{
						continue;
					}
					double RingSoft = 1.0;
					for (int32 k = 0; k < 9; ++k)
					{
						const int32 NX = X + k % 3 - 1;
						const int32 NY = Y + k / 3 - 1;
						const int32 Near = NY * R + NX;
						if (NX >= 0 && NY >= 0 && NX < R && NY < R && Before[Near] > 0 && !Hard.IsFloating(Before[Near])
							&& (Piece[Texel] == 0 || Before[Near] == Piece[Texel]))
						{
							Piece[Texel] = Before[Near];
							RingSoft = FMath::Min(RingSoft, Soft[Near]);
						}
					}
					if (Piece[Texel] > 0)
					{
						Soft[Texel] = RingSoft;
					}
				}
			}
		}

		// 조각 자기 칠(PieceOwnFlex, 안 B): 실행 중 강체 판 분류(Max Flex, 아래 판 표)와 붙은 조각 칸의 칠이 이 한 값을 본다. 칸 칠은 이 값을 넘지 않게
		// 자른다 — 테두리 칸은 바깥 흰 칠과 섞인 테두리 코너를 보간해 회색이라, 그대로면 강체 판 테두리의 붙듦(SB_HardSpringAt)이 무르고 판이 아닌 조각의
		// 테두리도 칠보다 무르게 굽는다. 떠 있는 장식 칸(광선이 맞은 장식 삼각형의 칠)은 그대로다. 칠 단계가 코너 Flex를 적지 않았으면(−1) 자르지 않는다.
		const TArray<double> OwnFlex = PieceOwnFlex(MD, Hard);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			const int32 P = Piece[Texel];
			if (P > 0 && !Hard.IsFloating(P) && OwnFlex.IsValidIndex(P - 1) && OwnFlex[P - 1] >= 0.0)
			{
				Soft[Texel] = FMath::Min(Soft[Texel], OwnFlex[P - 1]);
			}
		}

		// 조각 표: 발자국 칸(표면 칸)에서.
		const int32 NumIn = Hard.Num();
		TArray<TArray<int32>> Members;
		Members.SetNum(NumIn);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Piece[Texel] > 0 && Atlas.Kind[Texel] == KindSurface)
			{
				Members[Piece[Texel] - 1].Add(Texel);
			}
		}
		// 칠한 조각마다 슬롯·자리·크기(빠진 조각을 로그에 적는다): 조각 삼각형 정점의 평균과 상자, 첫 삼각형의 슬롯.
		TArray<FVector3d> PieceSum;
		PieceSum.Init(FVector3d::ZeroVector, NumIn);
		TArray<int32> PieceCount;
		PieceCount.Init(0, NumIn);
		TArray<FBox3d> PieceBox;
		PieceBox.Init(FBox3d(ForceInit), NumIn);
		TArray<FName> PieceSlot;
		PieceSlot.Init(NAME_None, NumIn);
		{
			const TPolygonGroupAttributesConstRef<FName> SlotNames = FStaticMeshConstAttributes(MD).GetPolygonGroupMaterialSlotNames();
			for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
			{
				const int32 P = Hard.PieceOf(Tri);
				if (P <= 0 || P > NumIn)
				{
					continue;
				}
				if (PieceCount[P - 1] == 0 && SlotNames.IsValid())
				{
					PieceSlot[P - 1] = SlotNames[MD.GetTrianglePolygonGroup(Tri)];
				}
				for (const FVertexID Corner : MD.GetTriangleVertices(Tri))
				{
					const FVector3d X(Positions[Corner]);
					PieceSum[P - 1] += X;
					PieceBox[P - 1] += X;
					PieceCount[P - 1] += 1;
				}
			}
		}
		struct FDroppedPiece
		{
			int32 Piece = 0;
			double Facing = 0.0;
		};
		TArray<FDroppedPiece> Wrapped;
		TArray<FDroppedPiece> Empty;
		TArray<FIntVector> TooLarge;   // (칠한 조각, 발자국 캐리어 칸 수, 발자국 거친 칸 수)
		TArray<int32> AtEdge;          // 칠한 조각(접촉 지도 가장자리에서 판을 읽지 못함 — 또는 판 굽기가 실패함)
		Out.Results.Init(0, NumIn);
		TArray<int32> Renumber;
		Renumber.Init(0, NumIn + 1);
		// 빠진 조각마다 슬롯·자리(메시 로컬)·크기 — 수만으로는 어느 조각인지 모른다(일지 286). 미리보기 Plates 보기도 같은 결과를 그린다.
		constexpr int32 MaxPieceLines = 40;
		const auto Where = [&](int32 p)
		{
			const FVector3d C = PieceCount[p] > 0 ? PieceSum[p] / (double)PieceCount[p] : FVector3d::ZeroVector;
			const double Across = PieceBox[p].IsValid ? (PieceBox[p].Max - PieceBox[p].Min).Size() : 0.0;
			return FString::Printf(TEXT("slot '%s' at (%.1f, %.1f, %.1f), %.1f across"), *PieceSlot[p].ToString(), C.X, C.Y, C.Z, Across);
		};
		// 살 없는 발자국 거친 칸 거르기: 실행 중 발자국 줄의 넓이는 그 거친 칸에 있는 이 조각의 살(두께 > 0) 표면 칸 몫이라(ContactPressurePiecesCS),
		// 그런 칸이 하나도 없으면 넓이 0·행렬 0인 줄이 된다. 판 모드·강체 맞춤은 그 줄도 판 위로 세어, 처질 수 없는 줄에 판을 맞추려다 발자국 하중이
		// 끝없이 커졌다(시험 캐릭터 캡처 ±1e5, 바닥으로도 안 풀림). 그런 거친 칸의 조각 칸은 조각에서 뺀다(판이 아닌 칸). 남는 칸이 없는 조각은 밑 겉면이
		// 없는 조각처럼 무른 채 남는다. 두께는 실행 중 읽는 값(접촉 지도 노멀 알파, 반정밀)으로 본다. 그 거친 칸에는 거리표 줄도 있다(줄 = 살 있는 칸).
		TSet<int32> Fleshless;   // 발자국 거친 칸이 모두 살 없는 칸이라 빠진 조각
		{
			int32 CellsAll = 0;
			int32 CellsDropped = 0;
			int32 PiecesDropped = 0;
			int32 Listed = 0;
			const auto CellKey = [&](int32 Texel) { return ((uint32)(Texel % R) / (uint32)Block) | (((uint32)(Texel / R) / (uint32)Block) << 16); };
			for (int32 p = 0; p < NumIn; ++p)
			{
				TArray<int32>& M = Members[p];
				if (M.Num() == 0)
				{
					continue;
				}
				TSet<uint32> All;
				TSet<uint32> Live;
				for (const int32 Texel : M)
				{
					All.Add(CellKey(Texel));
					if (TexelThickness.IsValidIndex(Texel) && FFloat16((float)TexelThickness[Texel]).GetFloat() > 0.f)
					{
						Live.Add(CellKey(Texel));
					}
				}
				const int32 Dropped = All.Num() - Live.Num();
				CellsAll += All.Num();
				CellsDropped += Dropped;
				if (Dropped > 0)
				{
					++PiecesDropped;
					TArray<int32> Kept;
					Kept.Reserve(M.Num());
					for (const int32 Texel : M)
					{
						if (Live.Contains(CellKey(Texel)))
						{
							Kept.Add(Texel);
						}
						else
						{
							Piece[Texel] = 0;
							Lift[Texel] = 0.0;
							Soft[Texel] = 1.0;
						}
					}
					M = MoveTemp(Kept);
					if (M.Num() == 0)
					{
						Fleshless.Add(p);
					}
				}
				const FString FootLine = FString::Printf(TEXT("Flex plates: %s: footprint %d coarse cells, %d without flesh (dropped)"),
					*Where(p), All.Num(), Dropped);
				if (Listed++ < MaxPieceLines)
				{
					UE_LOG(LogSoftbodyCarrier, Display, TEXT("%s"), *FootLine);
				}
				else
				{
					UE_LOG(LogSoftbodyCarrier, Verbose, TEXT("%s"), *FootLine);
				}
			}
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: footprint without flesh: %d of %d coarse cells dropped in %d pieces, %d pieces left with no footprint (they are not plates)"),
				CellsDropped, CellsAll, PiecesDropped, Fleshless.Num());
		}
		// 판 후보(감기지 않고 밑 겉면이 있는 조각): 굽기 재료를 모아 판 굽기를 나란히 돌린 뒤 차례대로 표에 넣는다.
		struct FPlateCandidate
		{
			int32 Piece = 0;
			FSoftbodyHardPiece Entry;
			TArray<uint32> Cells;
			TArray<uint32> Tiles;
			SoftbodyPlateBake::FPieceInput Input;
			SoftbodyPlateBake::FPieceOutput Output;
			bool bBaked = false;
		};
		TArray<FPlateCandidate> Candidates;
		const int32 CoarseResolution = R / FMath::Max(Block, 1);
		for (int32 p = 0; p < NumIn; ++p)
		{
			const TArray<int32>& M = Members[p];
			if (M.Num() == 0)
			{
				Out.Results[p] = SoftbodyBakeShared::HardDroppedNoSurface;
				Empty.Add({ p });
				continue;
			}
			FVector3d Center = FVector3d::ZeroVector;
			FVector3d Normal = FVector3d::ZeroVector;
			for (const int32 Texel : M)
			{
				Center += TexelPosition[Texel];
				Normal += TexelNormal[Texel];
			}
			Center /= (double)M.Num();
			const double Facing = Normal.Size() / (double)M.Num();   // 칸 미는 방향은 단위 — 모두 한쪽이면 1
			Normal = Normal.GetSafeNormal();
			if (Normal.IsZero())
			{
				Out.Results[p] = SoftbodyBakeShared::HardDroppedNoSurface;
				Empty.Add({ p });
				continue;
			}
			// 평면 두 축: 노멀에 수직인 아무 축에서 시작해 2×2 공분산의 주축으로.
			FVector3d E1 = FMath::Abs(Normal.X) < 0.9 ? FVector3d(1, 0, 0) : FVector3d(0, 1, 0);
			E1 = (E1 - Normal * E1.Dot(Normal)).GetSafeNormal();
			const FVector3d E2 = FVector3d::CrossProduct(Normal, E1);
			double Sxx = 0.0, Syy = 0.0, Sxy = 0.0, Radius = 0.0;
			for (const int32 Texel : M)
			{
				const FVector3d D = TexelPosition[Texel] - Center;
				const double U = D.Dot(E1);
				const double V = D.Dot(E2);
				Sxx += U * U;
				Syy += V * V;
				Sxy += U * V;
				Radius = FMath::Max(Radius, D.Size());
			}
			Sxx /= M.Num();
			Syy /= M.Num();
			Sxy /= M.Num();
			const double Angle = 0.5 * FMath::Atan2(2.0 * Sxy, Sxx - Syy);
			const FVector3d AxisU = E1 * FMath::Cos(Angle) + E2 * FMath::Sin(Angle);
			const FVector3d AxisV = FVector3d::CrossProduct(Normal, AxisU);
			const double Mean = 0.5 * (Sxx + Syy);
			const double Diff = FMath::Sqrt(FMath::Square(0.5 * (Sxx - Syy)) + Sxy * Sxy);
			const double Major = Mean + Diff;
			const double Minor = FMath::Max(Mean - Diff, 0.0);
			// 감긴 조각(허벅지 띠·목 고리처럼 몸을 둘러 감김): 판 하나로 움직이면 앞을 누를 때 뒤가 들뜬다. 짐작해 나누지 않고 뺀다(P05).
			if (Facing < HardFlatFacing)
			{
				Out.Results[p] = SoftbodyBakeShared::HardDroppedWrapped;
				Wrapped.Add({ p, Facing });
				continue;
			}
			FSoftbodyHardPiece Entry;
			Entry.Center = FVector3f(Center);
			Entry.AxisU = FVector3f(AxisU);
			Entry.AxisV = FVector3f(AxisV);
			Entry.Radius = (float)Radius;
			Entry.Freedom = M.Num() <= 1 ? 1 : (Minor < 0.01 * Major ? 2 : 3);
			Entry.bFloating = Hard.IsFloating(p + 1);
			TSet<uint32> Cells;
			TSet<uint32> Tiles;
			for (const int32 Texel : M)
			{
				const uint32 X = (uint32)(Texel % R);
				const uint32 Y = (uint32)(Texel / R);
				Cells.Add((X / Block) | ((Y / Block) << 16));
				Tiles.Add((X / 16) | ((Y / 16) << 16));
			}
			// 판 모드는 자르지 않는다 — 넘치는 판은 뺀다(SoftbodyPlateBake::MaxCells).
			if (Cells.Num() > SoftbodyPlateBake::MaxCells)
			{
				Out.Results[p] = SoftbodyBakeShared::HardDroppedTooLarge;
				TooLarge.Add(FIntVector(p, M.Num(), Cells.Num()));
				continue;
			}
			FPlateCandidate Candidate;
			Candidate.Piece = p;
			Candidate.Entry = Entry;
			Candidate.Cells = Cells.Array();
			Candidate.Tiles = Tiles.Array();
			SoftbodyPlateBake::FPieceInput& In = Candidate.Input;
			In.Block = Block;
			In.CoarseResolution = CoarseResolution;
			In.TexelSize = TexelSize;
			In.HRef = HRef;
			for (const int32 Texel : M)
			{
				const FVector3d D = TexelPosition[Texel] - Center;
				const int32 Tri = Atlas.TexelTri.IsValidIndex(Texel) ? Atlas.TexelTri[Texel] : INDEX_NONE;
				In.Texels.Add(FIntPoint(Texel % R, Texel / R));
				In.Plate.Add(FVector2d(D.Dot(AxisU), D.Dot(AxisV)));
				// 실행 중 읽는 값(G16)과 같게 반올림해 굽는다.
				In.Softness.Add(FMath::RoundToDouble(FMath::Clamp(Soft[Texel], 0.0, 1.0) * 65535.0) / 65535.0);
				In.Chart.Add(Tri != INDEX_NONE && Atlas.TriChart.IsValidIndex(Tri) ? Atlas.TriChart[Tri] : INDEX_NONE);
			}
			for (const uint32 C : Candidate.Cells)
			{
				In.Cells.Add(FIntPoint((int32)(C & 0xFFFFu), (int32)(C >> 16)));
			}
			// 조각 칸 둘레 두 칸 안의 고스트가 이 조각의 칸을 가리키면 굽힘 차분이 그 고스트를 건너편 칸으로 읽는다(차트 이음매, SoftbodyPlateBake).
			{
				TMap<int32, int32> MemberOf;
				for (int32 k = 0; k < M.Num(); ++k)
				{
					MemberOf.Add(M[k], k);
				}
				TSet<int32> Seen;
				for (const int32 Texel : M)
				{
					const int32 X = Texel % R;
					const int32 Y = Texel / R;
					for (int32 DY = -2; DY <= 2; ++DY)
					{
						for (int32 DX = -2; DX <= 2; ++DX)
						{
							const int32 GX = X + DX;
							const int32 GY = Y + DY;
							if (GX < 0 || GY < 0 || GX >= R || GY >= R)
							{
								continue;
							}
							const int32 G = GY * R + GX;
							if (Atlas.Kind[G] != KindGhost || Seen.Contains(G))
							{
								continue;
							}
							Seen.Add(G);
							const int32 TX = FMath::RoundToInt32(Atlas.Redirect[G].X);
							const int32 TY = FMath::RoundToInt32(Atlas.Redirect[G].Y);
							if (TX < 0 || TY < 0 || TX >= R || TY >= R)
							{
								continue;
							}
							if (const int32* K = MemberOf.Find(TY * R + TX))
							{
								In.GhostTexels.Add(FIntPoint(GX, GY));
								In.GhostMember.Add(*K);
							}
						}
					}
				}
			}
			if (!SoftbodyPlateBake::NodesInsideGrid(In.Texels, Block, CoarseResolution))
			{
				Out.Results[p] = SoftbodyBakeShared::HardDroppedAtlasEdge;
				AtEdge.Add(p);
				continue;
			}
			Candidates.Add(MoveTemp(Candidate));
		}
		ParallelFor(Candidates.Num(), [&](int32 i)
		{
			Candidates[i].bBaked = SoftbodyPlateBake::BakePiece(Candidates[i].Input, Candidates[i].Output);
		});
		// 확인 굽기(일지 298): 환경 변수 SOFTBODY_PLATE_DUMP_DIR가 있으면 발자국 64칸 이하 판 몇 개의 재료·결과를 CSV로 적는다(replica PlateBake와 견줌).
		const FString DumpDir = FPlatformMisc::GetEnvironmentVariable(TEXT("SOFTBODY_PLATE_DUMP_DIR"));
		int32 NumDumped = 0;
		// 굽기 확인 합계: ψ 정규 직교 오차(배정밀·반정밀), 강체 셋과의 직교, 가장 작은 β ÷ 가장 큰 β, 판 모드 수, 이음매로 갈린 빈 모드.
		double WorstOrtho = 0.0;
		double WorstHalfOrtho = 0.0;
		double WorstLeak = 0.0;
		double WorstBetaRatio = 1.0;
		int32 WorstBetaPiece = INDEX_NONE;
		double BetaLo = TNumericLimits<double>::Max();
		double BetaHi = 0.0;
		int32 NumModesTotal = 0;
		int32 NumExtraNull = 0;
		int32 NumSplit = 0;
		for (FPlateCandidate& Candidate : Candidates)
		{
			const int32 p = Candidate.Piece;
			const SoftbodyPlateBake::FPieceOutput& Plate = Candidate.Output;
			if (!Candidate.bBaked)
			{
				UE_LOG(LogSoftbodyCarrier, Error, TEXT("Flex plates: %s - %s, so it bends with its paint instead of moving as one plate."), *Where(p), *Plate.Error);
				Out.Results[p] = SoftbodyBakeShared::HardDroppedAtlasEdge;
				AtEdge.Add(p);
				continue;
			}
			FSoftbodyHardPiece& Entry = Candidate.Entry;
			Entry.FirstCell = Out.Cells.Num();
			Entry.NumCells = Candidate.Cells.Num();
			Out.Cells.Append(Candidate.Cells);
			Entry.FirstTile = Out.Tiles.Num();
			Entry.NumTiles = Candidate.Tiles.Num();
			Out.Tiles.Append(Candidate.Tiles);
			// 판의 대표 Flex 칠 = 판 칸 칠의 중앙값 — 옛 회색 판 모드 굳기용이다(실행 쪽은 6a단계에서 지움, 굽기는 6b까지 둠). 조각 자기 칠의
			// 가장 큰 값(PieceOwnFlex)과 조각 반지름으로 실행 중 강체 판을 가른다(셰이더 SB_HardKindOf, 크기 비). 칠 단계가 코너 Flex를 적지 않았으면 판 칸
			// 칠의 가장 큰 값.
			{
				TArray<double> PieceSoft = Candidate.Input.Softness;
				PieceSoft.Sort();
				Entry.Softness = PieceSoft.Num() > 0 ? (float)PieceSoft[PieceSoft.Num() / 2] : 0.f;
				Entry.MaxSoftness = OwnFlex.IsValidIndex(p) && OwnFlex[p] >= 0.0 ? (float)OwnFlex[p] : (PieceSoft.Num() > 0 ? (float)PieceSoft.Last() : 0.f);
			}
			Entry.FirstMode = Out.Modes.Num();
			Entry.NumModes = Plate.NumModes;
			for (int32 i = 0; i < Plate.NumModes; ++i)
			{
				Out.Modes.Add(FVector2f(Plate.Beta[i], Plate.WaveNumber[i]));
			}
			Entry.FirstShape = Out.ModeShapes.Num();
			Out.ModeShapes.Append(Plate.Shapes);
			const uint32 GradBase = (uint32)Out.GradEntries.Num();
			Out.CellPlate.Append(Plate.CellPlate);
			for (const FUintVector2& Range : Plate.GradRange)
			{
				Out.CellGradRange.Add(FUintVector2(Range.X + GradBase, Range.Y));
			}
			Out.GradEntries.Append(Plate.GradEntries);
			Entry.FirstNode = Out.Nodes.Num();
			Entry.NumNodes = Plate.Nodes.Num();
			const uint32 SrcBase = (uint32)Out.NodeSrc.Num();
			for (const FVector4f& Node : Plate.Nodes)
			{
				Out.Nodes.Add(FVector4f(Node.X, Node.Y, FMath::AsFloat(FMath::AsUInt(Node.Z) + SrcBase), Node.W));
			}
			Out.NodeSrc.Append(Plate.NodeSrc);
			Entry.NodeBoxMin = Plate.NodeBoxMin;
			Entry.NodeBoxSize = Plate.NodeBoxSize;
			Entry.FirstNodeSlot = Out.NodeSlots.Num();
			Out.NodeSlots.Append(Plate.NodeSlots);
			Out.Pieces.Add(Entry);
			Renumber[p + 1] = Out.Pieces.Num();
			Out.Results[p] = Out.Pieces.Num();

			WorstOrtho = FMath::Max(WorstOrtho, Plate.OrthoError);
			WorstHalfOrtho = FMath::Max(WorstHalfOrtho, Plate.HalfOrthoError);
			WorstLeak = FMath::Max(WorstLeak, Plate.RigidLeak);
			if (Plate.NumModes > 0 && Plate.MinBetaRatio < WorstBetaRatio)
			{
				WorstBetaRatio = Plate.MinBetaRatio;
				WorstBetaPiece = p;
			}
			NumModesTotal += Plate.NumModes;
			NumExtraNull += Plate.ExtraNull;
			NumSplit += Plate.ExtraNull > 0 ? 1 : 0;
			for (const float Beta : Plate.Beta)
			{
				BetaLo = FMath::Min(BetaLo, (double)Beta);
				BetaHi = FMath::Max(BetaHi, (double)Beta);
			}
			// 판마다 한 줄(앞 MaxPieceLines개는 Display, 나머지는 Verbose — 보석이 수백 개인 옷에서 로그를 덮지 않게).
			const FString PlateLine = FString::Printf(TEXT("Flex plates: plate %d (%s): %d coarse cells under it, %d plate modes (beta %.4g .. %.4g), %d modes with no bending (parts of the plate that bending does not join, as across a contact map seam)"),
				Out.Pieces.Num(), *Where(p), Entry.NumCells, Plate.NumModes, Plate.NumModes ? Plate.Beta[0] : 0.f,
				Plate.NumModes ? Plate.Beta.Last() : 0.f, Plate.ExtraNull);
			if (Out.Pieces.Num() <= MaxPieceLines)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("%s"), *PlateLine);
			}
			else
			{
				UE_LOG(LogSoftbodyCarrier, Verbose, TEXT("%s"), *PlateLine);
			}
			if (!DumpDir.IsEmpty() && NumDumped < 8 && Entry.NumCells <= 64)
			{
				SoftbodyPlateBake::WriteDebugCsv(Candidate.Input, Plate, FPaths::Combine(DumpDir, FString::Printf(TEXT("plate_%d.csv"), Out.Pieces.Num())));
				++NumDumped;
			}
		}
		if (Candidates.Num() > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: %d plate modes in all (beta %.4g .. %.4g mesh units, bending length scale %.3g), %d modes with no bending in %d plates (parts that bending does not join, as across a contact map seam)"),
				NumModesTotal, NumModesTotal ? BetaLo : 0.0, BetaHi, HRef, NumExtraNull, NumSplit);
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: bake check - mode shapes orthonormal to %.2g (%.2g in half precision), off the sink-and-tilt shapes by %.2g, smallest stiffness / largest %.3g"),
				WorstOrtho, WorstHalfOrtho, WorstLeak, WorstBetaRatio);
			if (WorstOrtho > 1e-6 || WorstLeak > 1e-6 || WorstBetaRatio < -1e-8)
			{
				UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: the plate mode bake check failed (orthonormal %.2g, off the sink-and-tilt shapes %.2g, smallest stiffness ratio %.3g at %s). Report this with the mesh."),
					WorstOrtho, WorstLeak, WorstBetaRatio, WorstBetaPiece != INDEX_NONE ? *Where(WorstBetaPiece) : TEXT("-"));
			}
		}
		if (Wrapped.Num() > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: %d painted pieces wrap around the body, so they bend with their paint instead of moving as plates. Leave a thin white gap in the Flex paint to split each into flat parts. The Plates preview shows them in red."),
				Wrapped.Num());
			for (int32 i = 0; i < FMath::Min(Wrapped.Num(), MaxPieceLines); ++i)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   %s - how much its surface faces one way %.2f (flat pieces %.1f or more)"),
					*Where(Wrapped[i].Piece), Wrapped[i].Facing, HardFlatFacing);
			}
			if (Wrapped.Num() > MaxPieceLines)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   and %d more"), Wrapped.Num() - MaxPieceLines);
			}
		}
		if (Empty.Num() > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: %d painted pieces have no contact map cells under them (outside the pressable slots, or no surface found within %.2f below a floating piece), so they are not plates. The Plates preview shows them in white."),
				Empty.Num(), MaxLift);
			for (int32 i = 0; i < FMath::Min(Empty.Num(), MaxPieceLines); ++i)
			{
				const int32 p = Empty[i].Piece;
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   %s - %s"), *Where(p), Fleshless.Contains(p)
					? TEXT("every coarse cell under it is without flesh (Depth 0)")
					: (Hard.IsFloating(p + 1)
					? *FString::Printf(TEXT("floating, no pressable surface within %.2f under it"), MaxGap)
					: TEXT("not on the pressable surface (Pressable Slots)")));
			}
			if (Empty.Num() > MaxPieceLines)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   and %d more"), Empty.Num() - MaxPieceLines);
			}
		}
		if (TooLarge.Num() > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: %d painted pieces cover more than %d coarse cells (%d x %d contact map cells each), so they bend with their paint instead of moving as plates. For rigid plates, paint each as smaller plates with thin white gaps between them. The Plates preview shows them in orange."),
				TooLarge.Num(), SoftbodyPlateBake::MaxCells, Block, Block);
			for (int32 i = 0; i < FMath::Min(TooLarge.Num(), MaxPieceLines); ++i)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   %s - %d contact map cells (%d coarse cells)"),
					*Where(TooLarge[i].X), TooLarge[i].Y, TooLarge[i].Z);
			}
			if (TooLarge.Num() > MaxPieceLines)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   and %d more"), TooLarge.Num() - MaxPieceLines);
			}
		}
		if (AtEdge.Num() > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: %d painted pieces sit at the very edge of the contact map, where a plate cannot be read, so they bend with their paint instead of moving as plates. Bake again with another Contact Map Resolution. The Plates preview shows them in purple."),
				AtEdge.Num());
			for (int32 i = 0; i < FMath::Min(AtEdge.Num(), MaxPieceLines); ++i)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   %s"), *Where(AtEdge[i]));
			}
			if (AtEdge.Num() > MaxPieceLines)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   and %d more"), AtEdge.Num() - MaxPieceLines);
			}
		}
		if (Out.Pieces.Num() == 0)
		{
			return false;
		}

		// 번호를 다시 매기고, 고스트는 가리키는 칸의 번호·올림·Softness를 쓴다.
		Out.Piece.Init(0, NumTexels);
		double MaxTop = 0.0;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			const int32 P = Renumber[Piece[Texel]];
			Piece[Texel] = P;
			Lift[Texel] = P > 0 ? Lift[Texel] : 0.0;
			MaxTop = FMath::Max(MaxTop, Lift[Texel]);
		}
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindGhost)
			{
				const int32 TX = FMath::Clamp((int32)FMath::RoundToDouble(Atlas.Redirect[Texel].X), 0, R - 1);
				const int32 TY = FMath::Clamp((int32)FMath::RoundToDouble(Atlas.Redirect[Texel].Y), 0, R - 1);
				const int32 Target = TY * R + TX;
				if (Atlas.Kind[Target] == KindSurface)
				{
					Out.Piece[Texel] = (uint16)Piece[Target];
					Lift[Texel] = Lift[Target];
					Soft[Texel] = Soft[Target];
				}
			}
			else
			{
				Out.Piece[Texel] = (uint16)Piece[Texel];
			}
		}
		// 판을 그리는 칸(fb/orn 안 A): 떠 있는 장식 렌더 정점이 읽는 칸에 비트 15를 둔다 — 합치기는 이 칸에만 판 자세를 그리고, 발자국(장식 밑 셔츠)
		// 칸은 살로 그린다(함몰 창이 판 한쪽 핀으로 푼 값). 장식 칸은 위 고스트 규칙(가리킨 칸의 번호)이 아니라 제 판 번호를 쓴다 — 장식 가장자리 칸이
		// 발자국 밖 셔츠를 가리켜도 판이다. Softness는 그 판의 대표 칠(판 규칙), 제 점에 둔 칸의 올림은 0(그 칸 위치가 이미 장식 점이다).
		// 판이 되지 못한 장식(감김·너무 큼·밑 겉면 없음)의 칸은 조각 번호를 받지 않는다(살처럼 — 다른 판의 발자국을 가리켜도 그 판을 그리지 않는다).
		// 장식 칸이 하나도 없는 떠 있는 판(자동 섬 지도는 장식 삼각형을 봉투에서 뺀다, 칸보다 가는 장식)은 지금처럼 발자국 칸과 그 번호를 받은 고스트가
		// 판을 그린다 — 장식 정점이 그 칸을 읽으므로 밑 옷도 장식과 같이 움직인다(로그로 알림).
		{
			TArray<int32> DrawCells;
			DrawCells.Init(0, Out.Pieces.Num());
			int32 NumDroppedOrnament = 0;
			if (OrnamentPiece.Num() == NumTexels)
			{
				for (int32 Texel = 0; Texel < NumTexels; ++Texel)
				{
					const int32 Raw = OrnamentPiece[Texel];
					if (Raw <= 0)
					{
						continue;
					}
					const int32 P = Renumber.IsValidIndex(Raw) ? Renumber[Raw] : 0;
					if (P > 0 && Out.Pieces[P - 1].bFloating)
					{
						Out.Piece[Texel] = (uint16)(P | SoftbodyHardSurface::DrawPlateBit);
						Soft[Texel] = FMath::Clamp((double)Out.Pieces[P - 1].Softness, 0.0, 1.0);
						if (OrnamentOwnPoint.IsValidIndex(Texel) && OrnamentOwnPoint[Texel] != 0)
						{
							Lift[Texel] = 0.0;
						}
						++DrawCells[P - 1];
					}
					else if (Out.Piece[Texel] != 0)
					{
						Out.Piece[Texel] = 0;
						++NumDroppedOrnament;
					}
				}
			}
			int32 NumOwn = 0;
			int32 NumOwnCells = 0;
			TArray<int32> Fallback;
			for (int32 p = 0; p < Out.Pieces.Num(); ++p)
			{
				if (!Out.Pieces[p].bFloating)
				{
					continue;
				}
				if (DrawCells[p] > 0)
				{
					++NumOwn;
					NumOwnCells += DrawCells[p];
				}
				else
				{
					Fallback.Add(p);
				}
			}
			if (Fallback.Num() > 0)
			{
				for (int32 Texel = 0; Texel < NumTexels; ++Texel)
				{
					const int32 P = Out.Piece[Texel];
					if (P > 0 && P <= Out.Pieces.Num() && Out.Pieces[P - 1].bFloating && DrawCells[P - 1] == 0)
					{
						Out.Piece[Texel] = (uint16)(P | SoftbodyHardSurface::DrawPlateBit);
					}
				}
			}
			if (NumOwn + Fallback.Num() > 0)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: %d floating plates draw on their own cells (%d cells), so the cloth under them keeps its own shape; %d floating plates have no cells of their own and draw on the cloth under them (it moves with them)%s"),
					NumOwn, NumOwnCells, Fallback.Num(), NumDroppedOrnament > 0 ? *FString::Printf(TEXT("; %d cells of ornaments that are not plates read no plate"), NumDroppedOrnament) : TEXT(""));
			}
			if (Fallback.Num() > 0 && OrnamentPiece.Num() == NumTexels)
			{
				for (int32 i = 0; i < FMath::Min(Fallback.Num(), MaxPieceLines); ++i)
				{
					const FVector3f& C = Out.Pieces[Fallback[i]].Center;
					UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates:   floating plate %d at (%.1f, %.1f, %.1f) has no contact map cells of its own (thinner than a cell?)"),
						Fallback[i] + 1, C.X, C.Y, C.Z);
				}
			}
		}
		// 거친 칸 하나를 두 조각이 나누면 실행 중 압력 칸이 한 조각만 받는다(ADR-0037 §3.7 데이터).
		{
			TMap<uint32, int32> Owner;
			int32 Shared = 0;
			for (int32 p = 0; p < Out.Pieces.Num(); ++p)
			{
				const FSoftbodyHardPiece& E = Out.Pieces[p];
				for (int32 c = E.FirstCell; c < E.FirstCell + E.NumCells; ++c)
				{
					if (int32* Found = Owner.Find(Out.Cells[c]))
					{
						Shared += *Found != p ? 1 : 0;
					}
					else
					{
						Owner.Add(Out.Cells[c], p);
					}
				}
			}
			if (Shared > 0)
			{
				UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: %d coarse cells (%d x %d contact map cells) are shared by two rigid pieces, so presses there move only one of them. Paint rigid pieces at least that far apart."),
					Shared, Block, Block);
			}
		}
		Out.Softness.Init(65535, NumTexels);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Out.Piece[Texel] > 0)
			{
				Out.Softness[Texel] = (uint16)FMath::Clamp(FMath::RoundToInt(Soft[Texel] * 65535.0), 0, 65535);
			}
		}
		if (MaxTop > 0.0)
		{
			Out.LiftScale = (float)MaxTop;
			Out.Lift.Init(0, NumTexels);
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				Out.Lift[Texel] = (uint16)FMath::Clamp(FMath::RoundToInt(Lift[Texel] / MaxTop * 65535.0), 0, 65535);
			}
		}
		int32 NumFloating = 0;
		int32 NumCellsTotal = 0;
		for (const FSoftbodyHardPiece& E : Out.Pieces)
		{
			NumFloating += E.bFloating ? 1 : 0;
			NumCellsTotal += E.NumCells;
		}
		TArray<double> Tops;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindSurface && Lift[Texel] > 0.0)
			{
				Tops.Add(Lift[Texel]);
			}
		}
		Tops.Sort();
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: %d plates (%d floating), %d coarse cells under them, plate top above the surface median %.2f / max %.2f (%d cells)"),
			Out.Pieces.Num(), NumFloating, NumCellsTotal, Tops.Num() ? Tops[Tops.Num() / 2] : 0.0, MaxTop, Tops.Num());
		return true;
	}

	/** 넓이 가중 백분위(Values는 (값, 넓이), 정렬해 둔다). */
	double WeightedPercentile(TArray<TPair<double, double>>& Values, double Q)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		double Total = 0.0;
		for (const TPair<double, double>& V : Values)
		{
			Total += V.Value;
		}
		double Sum = 0.0;
		for (const TPair<double, double>& V : Values)
		{
			Sum += V.Value;
			if (Sum >= Q * Total)
			{
				return V.Key;
			}
		}
		return Values.Last().Key;
	}

	/**
	 * 3D 삼각형을 UV 삼각형으로 옮길 때의 늘어남: OutScale = 길이 배율(UV 넓이 / 3D 넓이의 제곱근), OutAniso = 가장 늘어난
	 * 방향과 가장 줄어든 방향의 배율 비(1 = 모양 그대로). 퇴화면 false.
	 */
	bool TriangleStretch(const FVector3d& P0, const FVector3d& P1, const FVector3d& P2, const FVector2d& U0, const FVector2d& U1,
		const FVector2d& U2, double& OutScale, double& OutAniso)
	{
		const FVector3d E1 = P1 - P0;
		const FVector3d E2 = P2 - P0;
		const double L1 = E1.Length();
		const FVector3d Normal = E1.Cross(E2);
		const double Area2 = Normal.Length();
		if (L1 < 1e-9 || Area2 < 1e-12)
		{
			return false;
		}
		// 삼각형 평면의 2D 좌표(첫 변 = x축)로 쓴 두 변 (L1, 0)·(Q01, Q11)과 UV 두 변 W1·W2. 야코비안 J = W Q⁻¹.
		const FVector3d AxisX = E1 / L1;
		const FVector3d AxisY = (Normal / Area2).Cross(AxisX);
		const double Q01 = E2.Dot(AxisX);
		const double Q11 = E2.Dot(AxisY);
		const double DetQ = L1 * Q11;
		const FVector2d W1 = U1 - U0;
		const FVector2d W2 = U2 - U0;
		const double J00 = W1.X / L1;
		const double J01 = (W2.X * L1 - W1.X * Q01) / DetQ;
		const double J10 = W1.Y / L1;
		const double J11 = (W2.Y * L1 - W1.Y * Q01) / DetQ;
		const double DetJ = FMath::Abs(J00 * J11 - J01 * J10);
		if (DetJ < 1e-20)
		{
			return false;
		}
		const double Frob = J00 * J00 + J01 * J01 + J10 * J10 + J11 * J11;
		const double BigSq = 0.5 * (Frob + FMath::Sqrt(FMath::Max(Frob * Frob - 4.0 * DetJ * DetJ, 0.0)));
		OutScale = FMath::Sqrt(DetJ);
		OutAniso = BigSq / DetJ;
		return true;
	}

	/**
	 * 아일랜드 경계 고리: 삼각형 하나만 쓰는 변(같은 정점 셋의 삼각형 — 양면 메시의 앞뒤 — 은 하나로 친다)을 이은 정점 묶음.
	 * OutLoopOf[정점] = 고리 번호(경계가 아니면 INDEX_NONE), OutLinks[정점] = 경계 변으로 이웃한 정점. 고리 수를 돌려준다.
	 */
	int32 ChartBoundaryLoops(const TArray<FIndex3i>& Tris, int32 NumVerts, TArray<int32>& OutLoopOf, TArray<TArray<int32>>& OutLinks)
	{
		TMap<TPair<int32, int32>, int32> EdgeUse;
		TSet<FIntVector> SeenTris;
		for (const FIndex3i& Tri : Tris)
		{
			int32 V[3] = {Tri.A, Tri.B, Tri.C};
			Algo::Sort(V);
			bool bAlreadySet = false;
			SeenTris.Add(FIntVector(V[0], V[1], V[2]), &bAlreadySet);
			if (bAlreadySet || V[0] == V[1] || V[1] == V[2])
			{
				continue;
			}
			++EdgeUse.FindOrAdd(TPair<int32, int32>(V[0], V[1]));
			++EdgeUse.FindOrAdd(TPair<int32, int32>(V[1], V[2]));
			++EdgeUse.FindOrAdd(TPair<int32, int32>(V[0], V[2]));
		}
		OutLinks.Reset();
		OutLinks.SetNum(NumVerts);
		for (const TPair<TPair<int32, int32>, int32>& Edge : EdgeUse)
		{
			if (Edge.Value == 1)
			{
				OutLinks[Edge.Key.Key].Add(Edge.Key.Value);
				OutLinks[Edge.Key.Value].Add(Edge.Key.Key);
			}
		}
		OutLoopOf.Init(INDEX_NONE, NumVerts);
		int32 NumLoops = 0;
		for (int32 Seed = 0; Seed < NumVerts; ++Seed)
		{
			if (OutLinks[Seed].Num() == 0 || OutLoopOf[Seed] != INDEX_NONE)
			{
				continue;
			}
			const int32 Loop = NumLoops++;
			TArray<int32> Stack;
			Stack.Add(Seed);
			OutLoopOf[Seed] = Loop;
			while (Stack.Num() > 0)
			{
				const int32 V = Stack.Pop(EAllowShrinking::No);
				for (const int32 N : OutLinks[V])
				{
					if (OutLoopOf[N] == INDEX_NONE)
					{
						OutLoopOf[N] = Loop;
						Stack.Add(N);
					}
				}
			}
		}
		return NumLoops;
	}

	/** 경계 고리 중 UV 둘레(Values 좌표)가 가장 긴 것 = 바깥 경계. 고리가 없으면 INDEX_NONE. */
	int32 OuterBoundaryLoop(const TArray<int32>& LoopOf, const TArray<TArray<int32>>& Links, int32 NumLoops, const TArray<FVector2d>& Values)
	{
		TArray<double> LoopLength;
		LoopLength.Init(0.0, NumLoops);
		for (int32 V = 0; V < LoopOf.Num(); ++V)
		{
			for (const int32 N : Links[V])
			{
				LoopLength[LoopOf[V]] += N > V ? (Values[N] - Values[V]).Length() : 0.0;
			}
		}
		int32 Outer = INDEX_NONE;
		for (int32 Loop = 0; Loop < NumLoops; ++Loop)
		{
			Outer = (Outer == INDEX_NONE || LoopLength[Loop] > LoopLength[Outer]) ? Loop : Outer;
		}
		return Outer;
	}

	/**
	 * 아일랜드 UV(cm)를 한 변 Cell인 격자에 그려, 꼭짓점(메시 정점)을 나누지 않은 두 삼각형이 같은 칸 한가운데를 덮는 수를 센다
	 * — 아틀라스 배치의 FoldTexels와 같은 잣대를 배치 전에 아일랜드 하나에 쓴다. VertexIds = 아일랜드 안 번호 → 메시 정점.
	 * OutFoldTris가 있으면 겹친 두 삼각형을 넣는다.
	 */
	int32 CountChartFolds(const TArray<FIndex3i>& Tris, const TArray<int32>& VertexIds, const TArray<FVector2d>& UVs, double Cell,
		TArray<int32>* OutFoldTris = nullptr)
	{
		TMap<FIntPoint, int32> Owner;
		int32 Folds = 0;
		const auto Adjacent = [&](int32 A, int32 B)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				for (int32 j = 0; j < 3; ++j)
				{
					if (VertexIds[Tris[A][i]] == VertexIds[Tris[B][j]])
					{
						return true;
					}
				}
			}
			return false;
		};
		for (int32 t = 0; t < Tris.Num(); ++t)
		{
			const FVector2d P0 = UVs[Tris[t].A] / Cell;
			const FVector2d P1 = UVs[Tris[t].B] / Cell;
			const FVector2d P2 = UVs[Tris[t].C] / Cell;
			const int32 X0 = (int32)FMath::FloorToDouble(FMath::Min3(P0.X, P1.X, P2.X));
			const int32 X1 = (int32)FMath::CeilToDouble(FMath::Max3(P0.X, P1.X, P2.X));
			const int32 Y0 = (int32)FMath::FloorToDouble(FMath::Min3(P0.Y, P1.Y, P2.Y));
			const int32 Y1 = (int32)FMath::CeilToDouble(FMath::Max3(P0.Y, P1.Y, P2.Y));
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					FVector3d Bary;
					if (!Barycentric2D(FVector2d(X + 0.5, Y + 0.5), P0, P1, P2, Bary) || Bary.X < -1e-7 || Bary.Y < -1e-7 || Bary.Z < -1e-7)
					{
						continue;
					}
					const FIntPoint Key(X, Y);
					if (const int32* Existing = Owner.Find(Key))
					{
						if (!Adjacent(*Existing, t))
						{
							++Folds;
							if (OutFoldTris)
							{
								OutFoldTris->Add(*Existing);
								OutFoldTris->Add(t);
							}
						}
					}
					Owner.Add(Key, t);
				}
			}
		}
		return Folds;
	}

	/**
	 * 아일랜드 하나를 이음매(경계)는 그대로 두고 다시 편다(ADR-0007 §3.7). 삼각형마다 3D 모양을 회전만 해서 가장 가깝게 놓는
	 * 펴기(ARAP — Liu 외 2008, "A Local/Global Approach to Mesh Parameterization")라 각도와 넓이를 함께 맞춘다.
	 * 되풀이마다 (1) 삼각형마다 지금 UV에 가장 가까운 회전을 고르고 (2) 모든 삼각형이 그 회전을 가장 잘 따르는 UV를 푼다(코탄
	 * 무게 라플라시안, 공액 기울기). 풀이는 지난 UV 쪽으로 아주 약하게 당겨 아일랜드가 떠돌지 않게 한다.
	 * 경계는 대부분 자유롭게 움직여 아일랜드 모양도 3D에 맞게 바뀌지만, 제 몸과 겹치지는 않게 한다:
	 *  - 아일랜드 안 구멍(다른 아일랜드가 박힌 자리)의 테두리는 UV0 자리에 고정한다. 풀어 두면 둘레 살이 구멍 쪽으로 밀려 들어가
	 *    겹쳤다(시험 캐릭터 가슴).
	 *  - 편 모양이 겹치면(Cell 격자로 셈) 겹친 삼각형에서 FlattenPinRadius 안의 경계 정점을 UV0 자리에 더 고정하고 다시 편다
	 *    (FlattenPinRounds번까지 — 사타구니 쪽 허벅지 경계, 발가락 사이).
	 *  - 그래도 겹치면 UV0 쪽으로 섞어(0.75, 0.5, 0.25) 겹침이 UV0보다 늘지 않는 가장 큰 몫을 쓴다(OutBlend).
	 * Points·Tris·VertexIds(→ 메시 정점)는 아일랜드 안 번호, UVs는 시작값(UV0을 cm로 맞춘 것)을 받아 결과(cm)를 돌려준다.
	 * 삼각형마다 UV0에서의 방향을 그대로 지킨다 — 거울처럼 뒤집힌 아일랜드는 뒤집힌 채로, 양면 메시의 뒷면(같은 정점·UV를 거꾸로
	 * 감은 삼각형)은 앞면과 같은 자리에 편다. 새로 뒤집힌 삼각형이 한도(FlattenFlipCount·FlattenFlipArea)를 넘거나, 섞어도
	 * 겹치거나, 풀이가 안 되면 false(UVs는 시작값 그대로, OutFailure = 1 풀이 안 됨, 2 뒤집힘이 늘어남, 3 겹침이 늘어남).
	 * OutFlippedBefore·OutFlipped = UV0·결과에서 뒤집히거나 넓이가 0인 삼각형 수, OutPinned = UV0 자리에 고정한 경계 정점 수.
	 */
	bool FlattenChartARAP(const TArray<FVector3d>& Points, const TArray<FIndex3i>& Tris, const TArray<int32>& VertexIds, double Cell,
		TArray<FVector2d>& UVs, int32& OutFlippedBefore, int32& OutFlipped, int32& OutSkipped, int32& OutPinned, double& OutBlend,
		int32& OutFailure)
	{
		const int32 NumVerts = Points.Num();
		const int32 NumTris = Tris.Num();
		OutFlipped = 0;
		OutSkipped = 0;
		OutPinned = 0;
		OutBlend = 0.0;
		OutFailure = 0;
		const TArray<FVector2d> Start = UVs;
		// 삼각형마다 제 평면의 2D 좌표(첫 꼭짓점 = 원점, 첫 변 = x축).
		TArray<FVector2d> Local;
		Local.Init(FVector2d::ZeroVector, NumTris * 3);
		TArray<bool> Used;
		Used.Init(false, NumTris);
		TArray<bool> Counted;
		Counted.Init(false, NumTris);
		TArray<double> UVArea;
		UVArea.Init(0.0, NumTris);
		TArray<double> Area3D;
		Area3D.Init(0.0, NumTris);
		double TotalArea = 0.0;
		double SignedUV = 0.0;
		for (int32 t = 0; t < NumTris; ++t)
		{
			const FIndex3i& Tri = Tris[t];
			const FVector3d E1 = Points[Tri.B] - Points[Tri.A];
			const FVector3d E2 = Points[Tri.C] - Points[Tri.A];
			const double L1 = E1.Length();
			const FVector3d Normal = E1.Cross(E2);
			const double Area2 = Normal.Length();
			if (Tri.A == Tri.B || Tri.B == Tri.C || Tri.A == Tri.C || L1 < 1e-9 || Area2 < 1e-12)
			{
				continue;
			}
			const FVector3d AxisX = E1 / L1;
			const FVector3d AxisY = (Normal / Area2).Cross(AxisX);
			Local[t * 3 + 1] = FVector2d(L1, 0.0);
			Local[t * 3 + 2] = FVector2d(E2.Dot(AxisX), E2.Dot(AxisY));
			Used[t] = true;
			Counted[t] = true;
			Area3D[t] = 0.5 * Area2;
			TotalArea += Area3D[t];
			UVArea[t] = FVector2d::CrossProduct(Start[Tri.B] - Start[Tri.A], Start[Tri.C] - Start[Tri.A]);
			SignedUV += UVArea[t];
		}
		// 삼각형마다 지킬 방향: UV0에서 넓이가 있으면 그 부호, 넓이가 0이면 아일랜드 대부분의 부호.
		const double Mirror = SignedUV < 0.0 ? -1.0 : 1.0;
		TArray<double> Side;
		Side.Init(Mirror, NumTris);
		for (int32 t = 0; t < NumTris; ++t)
		{
			const FIndex3i& Tri = Tris[t];
			const double Scale = (Start[Tri.B] - Start[Tri.A]).SquaredLength() + (Start[Tri.C] - Start[Tri.A]).SquaredLength();
			if (Counted[t] && FMath::Abs(UVArea[t]) > 1e-6 * Scale)
			{
				Side[t] = UVArea[t] < 0.0 ? -1.0 : 1.0;
			}
		}
		const auto CountFlipped = [&](const TArray<FVector2d>& Values, double& OutArea)
		{
			int32 Count = 0;
			OutArea = 0.0;
			for (int32 t = 0; t < NumTris; ++t)
			{
				const FIndex3i& Tri = Tris[t];
				if (Counted[t] && !(Side[t] * FVector2d::CrossProduct(Values[Tri.B] - Values[Tri.A], Values[Tri.C] - Values[Tri.A]) > 0.0))
				{
					++Count;
					OutArea += Area3D[t];
				}
			}
			return Count;
		};
		double FlippedAreaBefore = 0.0;
		OutFlippedBefore = CountFlipped(Start, FlippedAreaBefore);

		// 코탄 무게(꼭짓점 k의 각 → 맞은편 변, 절반). 자르지 않는다 — 삼각형 하나의 코탄 무게 세 개는 그대로면 늘 풀리는 행렬을
		// 이루지만(둔각의 음수 무게 포함), 하나만 자르면 그 성질이 깨질 수 있고, 음수를 0으로 자르면 둔각 삼각형의 긴 변이 풀려
		// 비스듬히 찌그러졌다(가로세로 비 95% 1.14 → 1.33). 대신 각이 1° 안쪽으로 납작한 삼각형은 풀이에서 뺀다.
		TArray<FVector3d> Weight;
		Weight.Init(FVector3d::ZeroVector, NumTris);
		TArray<TArray<TPair<int32, double>>> Rows;
		Rows.SetNum(NumVerts);
		TArray<double> Diag;
		Diag.Init(0.0, NumVerts);
		const auto AddOff = [&Rows](int32 I, int32 J, double W)
		{
			for (TPair<int32, double>& Entry : Rows[I])
			{
				if (Entry.Key == J)
				{
					Entry.Value -= W;
					return;
				}
			}
			Rows[I].Emplace(J, -W);
		};
		for (int32 t = 0; t < NumTris; ++t)
		{
			if (!Used[t])
			{
				continue;
			}
			for (int32 c = 0; c < 3; ++c)
			{
				Local[t * 3 + c].Y *= Side[t];
			}
			FVector3d Cot;
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector2d A = Local[t * 3 + (k + 1) % 3] - Local[t * 3 + k];
				const FVector2d B = Local[t * 3 + (k + 2) % 3] - Local[t * 3 + k];
				Cot[k] = A.Dot(B) / FMath::Max(FMath::Abs(FVector2d::CrossProduct(A, B)), 1e-300);
			}
			if (Cot.GetAbsMax() > FlattenMaxCot)
			{
				Used[t] = false;
				++OutSkipped;
				continue;
			}
			for (int32 k = 0; k < 3; ++k)
			{
				const double W = 0.5 * Cot[k];
				Weight[t][k] = W;
				const int32 I = Tris[t][(k + 1) % 3];
				const int32 J = Tris[t][(k + 2) % 3];
				Diag[I] += W;
				Diag[J] += W;
				AddOff(I, J, W);
				AddOff(J, I, W);
			}
		}
		double MeanDiag = 0.0;
		for (const double D : Diag)
		{
			MeanDiag += FMath::Abs(D);
		}
		MeanDiag = FMath::Max(MeanDiag / FMath::Max(NumVerts, 1), 1e-12);

		// 경계 고리 중 UV0 둘레가 가장 긴 것이 바깥 경계, 나머지는 구멍이다.
		TArray<int32> LoopOf;
		TArray<TArray<int32>> BoundaryLinks;
		const int32 NumLoops = ChartBoundaryLoops(Tris, NumVerts, LoopOf, BoundaryLinks);
		const int32 Outer = OuterBoundaryLoop(LoopOf, BoundaryLinks, NumLoops, Start);
		TArray<bool> Pinned;
		Pinned.Init(false, NumVerts);
		for (int32 V = 0; V < NumVerts; ++V)
		{
			Pinned[V] = LoopOf[V] != INDEX_NONE && LoopOf[V] != Outer;
		}

		// 한 번 펴기: 고정한 정점은 UV0 자리로 아주 세게, 나머지는 지난 UV 쪽으로 아주 약하게 당긴다.
		TArray<double> Pull;
		TArray<double> InvDiag;
		Pull.SetNum(NumVerts);
		InvDiag.SetNum(NumVerts);
		TArray<double> R, Z, P, AP;
		R.SetNumUninitialized(NumVerts);
		Z.SetNumUninitialized(NumVerts);
		P.SetNumUninitialized(NumVerts);
		AP.SetNumUninitialized(NumVerts);
		const auto Apply = [&](const TArray<double>& X, TArray<double>& Out)
		{
			for (int32 i = 0; i < NumVerts; ++i)
			{
				double Sum = (Diag[i] + Pull[i]) * X[i];
				for (const TPair<int32, double>& Entry : Rows[i])
				{
					Sum += Entry.Value * X[Entry.Key];
				}
				Out[i] = Sum;
			}
		};
		// (라플라시안 + 당김) X = B를 공액 기울기(대각 전처리)로 푼다. X는 지난 값에서 시작한다.
		const auto Solve = [&](TArray<double>& X, const TArray<double>& B)
		{
			Apply(X, AP);
			double BNorm = 0.0;
			double RZ = 0.0;
			for (int32 i = 0; i < NumVerts; ++i)
			{
				R[i] = B[i] - AP[i];
				Z[i] = R[i] * InvDiag[i];
				P[i] = Z[i];
				RZ += R[i] * Z[i];
				BNorm += B[i] * B[i];
			}
			BNorm = FMath::Max(FMath::Sqrt(BNorm), 1e-30);
			double RNorm = 0.0;
			const int32 MaxSteps = FMath::Min(4 * NumVerts + 100, 5000);
			for (int32 Step = 0; Step < MaxSteps; ++Step)
			{
				RNorm = 0.0;
				for (int32 i = 0; i < NumVerts; ++i)
				{
					RNorm += R[i] * R[i];
				}
				RNorm = FMath::Sqrt(RNorm);
				if (RNorm <= 1e-9 * BNorm)
				{
					return true;
				}
				Apply(P, AP);
				double PAP = 0.0;
				for (int32 i = 0; i < NumVerts; ++i)
				{
					PAP += P[i] * AP[i];
				}
				if (!(PAP > 0.0))
				{
					return false;
				}
				const double Alpha = RZ / PAP;
				double NextRZ = 0.0;
				for (int32 i = 0; i < NumVerts; ++i)
				{
					X[i] += Alpha * P[i];
					R[i] -= Alpha * AP[i];
					Z[i] = R[i] * InvDiag[i];
					NextRZ += R[i] * Z[i];
				}
				const double Beta = NextRZ / FMath::Max(RZ, 1e-300);
				RZ = NextRZ;
				for (int32 i = 0; i < NumVerts; ++i)
				{
					P[i] = Z[i] + Beta * P[i];
				}
			}
			return RNorm <= 1e-5 * BNorm;
		};
		const auto Flatten = [&](TArray<FVector2d>& Out)
		{
			for (int32 i = 0; i < NumVerts; ++i)
			{
				Pull[i] = (Pinned[i] ? 1e6 : 1e-4) * FMath::Max(FMath::Abs(Diag[i]), MeanDiag);
				const double D = Diag[i] + Pull[i];
				InvDiag[i] = D > 1e-12 ? 1.0 / D : 1.0;
			}
			TArray<double> X, Y, BX, BY;
			X.SetNum(NumVerts);
			Y.SetNum(NumVerts);
			for (int32 i = 0; i < NumVerts; ++i)
			{
				X[i] = Start[i].X;
				Y[i] = Start[i].Y;
			}
			for (int32 Iteration = 0; Iteration < FlattenIterations; ++Iteration)
			{
				BX.Init(0.0, NumVerts);
				BY.Init(0.0, NumVerts);
				for (int32 t = 0; t < NumTris; ++t)
				{
					if (!Used[t])
					{
						continue;
					}
					// (1) 이 삼각형을 지금 UV에 가장 가깝게 놓는 회전: 변마다 (UV 변) ⊗ (3D 변)을 무게로 더한 행렬의 회전 몫.
					double S00 = 0.0;
					double S01 = 0.0;
					double S10 = 0.0;
					double S11 = 0.0;
					for (int32 k = 0; k < 3; ++k)
					{
						const int32 I = Tris[t][(k + 1) % 3];
						const int32 J = Tris[t][(k + 2) % 3];
						const FVector2d DU(X[I] - X[J], Y[I] - Y[J]);
						const FVector2d DX = Local[t * 3 + (k + 1) % 3] - Local[t * 3 + (k + 2) % 3];
						const double W = Weight[t][k];
						S00 += W * DU.X * DX.X;
						S01 += W * DU.X * DX.Y;
						S10 += W * DU.Y * DX.X;
						S11 += W * DU.Y * DX.Y;
					}
					const double Angle = FMath::Atan2(S10 - S01, S00 + S11);
					const double Cos = FMath::Cos(Angle);
					const double Sin = FMath::Sin(Angle);
					// (2)의 오른쪽: 회전한 3D 변을 두 끝점에 나눠 더한다.
					for (int32 k = 0; k < 3; ++k)
					{
						const int32 I = Tris[t][(k + 1) % 3];
						const int32 J = Tris[t][(k + 2) % 3];
						const FVector2d DX = Local[t * 3 + (k + 1) % 3] - Local[t * 3 + (k + 2) % 3];
						const double W = Weight[t][k];
						const double RX = W * (Cos * DX.X - Sin * DX.Y);
						const double RY = W * (Sin * DX.X + Cos * DX.Y);
						BX[I] += RX;
						BX[J] -= RX;
						BY[I] += RY;
						BY[J] -= RY;
					}
				}
				for (int32 i = 0; i < NumVerts; ++i)
				{
					BX[i] += Pull[i] * (Pinned[i] ? Start[i].X : X[i]);
					BY[i] += Pull[i] * (Pinned[i] ? Start[i].Y : Y[i]);
				}
				if (!Solve(X, BX) || !Solve(Y, BY))
				{
					return false;
				}
			}
			for (int32 i = 0; i < NumVerts; ++i)
			{
				if (!FMath::IsFinite(X[i]) || !FMath::IsFinite(Y[i]))
				{
					return false;
				}
				Out[i] = FVector2d(X[i], Y[i]);
			}
			return true;
		};

		// 펴고, 겹치면 겹친 곳 둘레 경계를 더 고정해 다시 편다.
		const int32 FoldsBefore = CountChartFolds(Tris, VertexIds, Start, Cell);
		TArray<FVector2d> Result;
		Result.SetNum(NumVerts);
		for (int32 Round = 0; Round <= FlattenPinRounds; ++Round)
		{
			if (!Flatten(Result))
			{
				OutFailure = 1;
				return false;
			}
			TArray<int32> FoldTris;
			if (Round == FlattenPinRounds || CountChartFolds(Tris, VertexIds, Result, Cell, &FoldTris) <= FoldsBefore)
			{
				break;
			}
			TSet<int32> Near;
			for (const int32 t : FoldTris)
			{
				Near.Add(Tris[t].A);
				Near.Add(Tris[t].B);
				Near.Add(Tris[t].C);
			}
			int32 Added = 0;
			for (int32 V = 0; V < NumVerts; ++V)
			{
				if (Pinned[V] || LoopOf[V] == INDEX_NONE)
				{
					continue;
				}
				for (const int32 N : Near)
				{
					if (FVector3d::DistSquared(Points[V], Points[N]) < FlattenPinRadius * FlattenPinRadius)
					{
						Pinned[V] = true;
						++Added;
						break;
					}
				}
			}
			if (Added == 0)
			{
				break;
			}
		}
		for (const bool bPinned : Pinned)
		{
			OutPinned += bPinned ? 1 : 0;
		}

		// 받아들이기: 새로 뒤집힌 삼각형이 한도 안이고 겹침이 UV0보다 늘지 않는 가장 큰 몫(1, 0.75, 0.5, 0.25).
		TArray<FVector2d> Blended;
		Blended.SetNum(NumVerts);
		for (const double Amount : {1.0, 0.75, 0.5, 0.25})
		{
			for (int32 i = 0; i < NumVerts; ++i)
			{
				Blended[i] = Start[i] + (Result[i] - Start[i]) * Amount;
			}
			double FlippedArea = 0.0;
			OutFlipped = CountFlipped(Blended, FlippedArea);
			if (OutFlipped - OutFlippedBefore > FMath::Max(3, (int32)(FlattenFlipCount * NumTris))
				|| FlippedArea - FlippedAreaBefore > FlattenFlipArea * TotalArea)
			{
				OutFailure = 2;
				continue;
			}
			if (CountChartFolds(Tris, VertexIds, Blended, Cell) > FoldsBefore)
			{
				OutFailure = 3;
				continue;
			}
			OutFailure = 0;
			OutBlend = Amount;
			UVs = MoveTemp(Blended);
			return true;
		}
		OutFlipped = OutFlippedBefore;
		return false;
	}

	/**
	 * 아일랜드 UV에서 칸 크기가 아일랜드 평균에서 가장 벗어난 방향의 배율 — 삼각형마다 max(가장 늘어난 방향 / 평균,
	 * 평균 / 가장 줄어든 방향) — 의 넓이 가중 Q 백분위. OutVertex가 있으면 정점마다 둘레 삼각형의 그 배율 로그를 넓이로 평균하고,
	 * OutRms가 있으면 그 배율 로그의 넓이 가중 제곱평균제곱근(아일랜드 전체가 고르지 않은 정도)을 넣는다.
	 */
	double ChartWorstStretch(const TArray<FVector3d>& Points, const TArray<FIndex3i>& Tris, const TArray<FVector2d>& UVs, double Q,
		TArray<double>* OutVertex = nullptr, double* OutRms = nullptr)
	{
		if (OutRms)
		{
			*OutRms = 0.0;
		}
		double Area3D = 0.0;
		double AreaUV = 0.0;
		for (const FIndex3i& Tri : Tris)
		{
			Area3D += 0.5 * FVector3d::CrossProduct(Points[Tri.B] - Points[Tri.A], Points[Tri.C] - Points[Tri.A]).Length();
			AreaUV += 0.5 * FMath::Abs(FVector2d::CrossProduct(UVs[Tri.B] - UVs[Tri.A], UVs[Tri.C] - UVs[Tri.A]));
		}
		TArray<double> VertexArea;
		if (OutVertex)
		{
			OutVertex->Init(0.0, Points.Num());
			VertexArea.Init(0.0, Points.Num());
		}
		if (Area3D <= 1e-12 || AreaUV <= 1e-20)
		{
			return 1.0;
		}
		const double Mean = FMath::Sqrt(AreaUV / Area3D);
		TArray<TPair<double, double>> Values;
		Values.Reserve(Tris.Num());
		double SumSquare = 0.0;
		double SumArea = 0.0;
		for (const FIndex3i& Tri : Tris)
		{
			double Scale = 0.0;
			double Aniso = 0.0;
			if (!TriangleStretch(Points[Tri.A], Points[Tri.B], Points[Tri.C], UVs[Tri.A], UVs[Tri.B], UVs[Tri.C], Scale, Aniso))
			{
				continue;
			}
			// 가장 늘어난 방향 = 넓이 배율 × √가로세로 비, 가장 줄어든 방향 = 넓이 배율 / √가로세로 비.
			const double Ratio = Scale / Mean;
			const double Worst = FMath::Sqrt(Aniso) * FMath::Max(Ratio, 1.0 / Ratio);
			const double Area = 0.5 * FVector3d::CrossProduct(Points[Tri.B] - Points[Tri.A], Points[Tri.C] - Points[Tri.A]).Length();
			Values.Emplace(Worst, Area);
			SumSquare += Area * FMath::Square(FMath::Loge(Worst));
			SumArea += Area;
			if (OutVertex)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					(*OutVertex)[Tri[c]] += Area * FMath::Loge(Worst);
					VertexArea[Tri[c]] += Area;
				}
			}
		}
		if (OutVertex)
		{
			for (int32 V = 0; V < Points.Num(); ++V)
			{
				(*OutVertex)[V] = VertexArea[V] > 0.0 ? (*OutVertex)[V] / VertexArea[V] : 0.0;
			}
		}
		if (OutRms && SumArea > 0.0)
		{
			*OutRms = FMath::Sqrt(SumSquare / SumArea);
		}
		Values.Sort([](const TPair<double, double>& A, const TPair<double, double>& B) { return A.Key < B.Key; });
		return Values.Num() > 0 ? WeightedPercentile(Values, Q) : 1.0;
	}

	/**
	 * 아일랜드 변을 따라 IsFrom 정점들에서 IsTo 정점까지 3D 길이가 가장 짧은 길(정점 차례, From 쪽 끝이 처음). 길 가운데
	 * 정점은 경계가 아니어야 한다(Interior). 못 찾으면 빈 배열.
	 */
	TArray<int32> ShortestChartPath(const TArray<FVector3d>& Points, const TArray<TArray<int32>>& Neighbors, const TArray<bool>& IsFrom,
		const TArray<bool>& IsTo, const TArray<bool>& Interior)
	{
		const int32 NumVerts = Points.Num();
		TArray<double> Distance;
		Distance.Init(TNumericLimits<double>::Max(), NumVerts);
		TArray<int32> Previous;
		Previous.Init(INDEX_NONE, NumVerts);
		const auto Nearer = [](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; };
		TArray<TPair<double, int32>> Heap;
		for (int32 V = 0; V < NumVerts; ++V)
		{
			if (IsFrom[V])
			{
				Distance[V] = 0.0;
				Heap.HeapPush(TPair<double, int32>(0.0, V), Nearer);
			}
		}
		while (Heap.Num() > 0)
		{
			TPair<double, int32> Top;
			Heap.HeapPop(Top, Nearer, EAllowShrinking::No);
			const int32 V = Top.Value;
			if (Top.Key > Distance[V])
			{
				continue;
			}
			if (IsTo[V] && !IsFrom[V])
			{
				TArray<int32> Path;
				for (int32 At = V; At != INDEX_NONE; At = Previous[At])
				{
					Path.Add(At);
				}
				Algo::Reverse(Path);
				return Path;
			}
			if (!IsFrom[V] && !Interior[V])
			{
				continue;
			}
			for (const int32 N : Neighbors[V])
			{
				const double Next = Distance[V] + FVector3d::Distance(Points[V], Points[N]);
				if (!IsFrom[N] && Next < Distance[N])
				{
					Distance[N] = Next;
					Previous[N] = V;
					Heap.HeapPush(TPair<double, int32>(Next, N), Nearer);
				}
			}
		}
		return TArray<int32>();
	}

	/** 아일랜드 정점마다 변으로 이웃한 정점. */
	void ChartNeighbors(const TArray<FIndex3i>& Tris, int32 NumVerts, TArray<TArray<int32>>& Out)
	{
		Out.Reset();
		Out.SetNum(NumVerts);
		for (const FIndex3i& Tri : Tris)
		{
			for (int32 c = 0; c < 3; ++c)
			{
				Out[Tri[c]].AddUnique(Tri[(c + 1) % 3]);
				Out[Tri[(c + 1) % 3]].AddUnique(Tri[c]);
			}
		}
	}

	/**
	 * 아일랜드를 정점 길 Path(이웃한 정점 차례)를 따라 가른다. 길 위 정점마다 둘레 삼각형을 "길이 아닌 변을 나눠 가진 삼각형끼리"
	 * 묶어, 첫 묶음 말고는 정점을 새로 만들어(같은 3D 점·메시 정점·시작 UV) 그 묶음의 삼각형이 새 정점을 쓰게 한다. 경계 안쪽에서
	 * 끝나는 길의 끝 정점은 둘레가 한 묶음이라 갈라지지 않는다(틈의 끝). 새 정점마다 Source에 원래 정점 번호를 더한다.
	 */
	void CutChartAlongPath(const TArray<int32>& Path, TArray<FIndex3i>& Tris, TArray<FVector3d>& Points, TArray<int32>& VertexIds,
		TArray<FVector2d>& Start, TArray<int32>& Source)
	{
		TSet<TPair<int32, int32>> CutEdges;
		for (int32 i = 0; i + 1 < Path.Num(); ++i)
		{
			CutEdges.Add(TPair<int32, int32>(FMath::Min(Path[i], Path[i + 1]), FMath::Max(Path[i], Path[i + 1])));
		}
		for (const int32 V : Path)
		{
			TArray<int32> Fan;
			for (int32 t = 0; t < Tris.Num(); ++t)
			{
				if (Tris[t].A == V || Tris[t].B == V || Tris[t].C == V)
				{
					Fan.Add(t);
				}
			}
			TArray<int32> Parent;
			Parent.SetNum(Fan.Num());
			for (int32 i = 0; i < Fan.Num(); ++i)
			{
				Parent[i] = i;
			}
			// 같은 변 (V, W)를 가진 삼각형끼리 잇는다. 길의 변은 잇지 않는다.
			TMap<int32, int32> FirstWithEdge;
			for (int32 i = 0; i < Fan.Num(); ++i)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					const int32 W = Tris[Fan[i]][c];
					if (W == V || CutEdges.Contains(TPair<int32, int32>(FMath::Min(V, W), FMath::Max(V, W))))
					{
						continue;
					}
					if (const int32* First = FirstWithEdge.Find(W))
					{
						Parent[FindRoot(Parent, i)] = FindRoot(Parent, *First);
					}
					else
					{
						FirstWithEdge.Add(W, i);
					}
				}
			}
			TMap<int32, int32> GroupVertex;
			for (int32 i = 0; i < Fan.Num(); ++i)
			{
				const int32 Root = FindRoot(Parent, i);
				int32 NewV = V;
				if (const int32* Found = GroupVertex.Find(Root))
				{
					NewV = *Found;
				}
				else
				{
					if (GroupVertex.Num() > 0)
					{
						// Add에 같은 배열의 원소를 넘기면 늘리다 옮겨질 수 있어 값으로 복사해 넘긴다.
						const FVector3d Point = Points[V];
						const int32 VertexId = VertexIds[V];
						const FVector2d StartUV = Start[V];
						const int32 SourceVertex = Source[V];
						NewV = Points.Add(Point);
						VertexIds.Add(VertexId);
						Start.Add(StartUV);
						Source.Add(SourceVertex);
					}
					GroupVertex.Add(Root, NewV);
				}
				FIndex3i& Tri = Tris[Fan[i]];
				for (int32 c = 0; c < 3; ++c)
				{
					Tri[c] = Tri[c] == V ? NewV : Tri[c];
				}
			}
		}
	}

	/** FlattenChartARAP의 결과 수. */
	struct FFlattenStats
	{
		int32 FlippedBefore = 0;
		int32 Flipped = 0;
		int32 Skipped = 0;
		int32 Pinned = 0;
		double Blend = 0.0;
		int32 Failure = 0;
	};

	/**
	 * 이음매를 더 넣어 펴기(ADR-0007 §3.8). 먼저 FlattenChartARAP로 편다(구멍 테두리는 UV0 자리에 고정). 칸 크기가 가장 벗어난
	 * 방향의 배율(넓이 가중 95%, ChartWorstStretch)이 FlattenCutTarget을 넘고 아일랜드가 FlattenCutMinArea 이상이면 틈을 내 본다.
	 * 틈마다 다시 펴서, 아일랜드 전체가 고르지 않은 정도(배율 로그의 제곱평균제곱근)를 FlattenCutGain 이상 줄이고 95%를 늘리지
	 * 않으면 남기고 아니면 되돌린다. 틈 후보:
	 *  1. 구멍(다른 아일랜드가 박힌 자리)마다 테두리에서 다른 경계까지 가장 짧은 길 — 구멍 테두리를 고정하지 않아도 되어 구멍이
	 *     바깥으로 벌어진다.
	 *  2. 가장 늘거나 준 경계 안쪽 자리에서 가장 가까운 경계까지(Gu 외 2002, "Geometry Images"의 가르기). 둥글게 솟은 곳(원뿔)은
	 *     틈으로 풀린다. 둥근 봉우리 둘레가 오목하게 이어진 곳(몸에 붙은 가슴)은 틈이 오목한 테를 지나며 양쪽이 겹치려 해서 대개
	 *     되돌려진다 — 그런 곳을 고르게 펴려면 봉우리를 따로 떼어야 한다(넣지 않음).
	 * 틈은 메시 캐리어 UV 채널의 이음매가 되어(렌더 정점이 틈을 따라 나뉜다) UV0 이음매와 똑같이 다룬다.
	 * 인자는 FlattenChartARAP와 같다. 가르면 Points·VertexIds·UVs 끝에 새 정점이 붙고 Tris가 바뀐다 — OutSource[i] = 새 정점
	 * (원래 정점 수 + i)의 원래 정점 번호. OutCuts·OutCutLength = 남긴 틈 수와 길이 합, OutWorstUncut·OutWorst = 가르지 않고 편
	 * 결과(펴지 못했으면 UV0 모양)와 최종 결과의 배율 95%.
	 */
	bool FlattenChartWithCuts(TArray<FVector3d>& Points, TArray<FIndex3i>& Tris, TArray<int32>& VertexIds, double Cell,
		TArray<FVector2d>& UVs, TArray<int32>& OutSource, int32& OutCuts, double& OutCutLength, double& OutWorstUncut, double& OutWorst,
		FFlattenStats& OutStats)
	{
		OutSource.Reset();
		OutCuts = 0;
		OutCutLength = 0.0;
		const int32 NumOriginal = Points.Num();
		const TArray<FVector2d> Start = UVs;
		const auto Run = [Cell](const TArray<FVector3d>& P, const TArray<FIndex3i>& T, const TArray<int32>& Ids, TArray<FVector2d>& Values,
			FFlattenStats& Stats)
		{
			return FlattenChartARAP(P, T, Ids, Cell, Values, Stats.FlippedBefore, Stats.Flipped, Stats.Skipped, Stats.Pinned, Stats.Blend,
				Stats.Failure);
		};
		const bool bUncut = Run(Points, Tris, VertexIds, UVs, OutStats);
		TArray<double> VertexLog;
		double Rms = 0.0;
		OutWorstUncut = ChartWorstStretch(Points, Tris, UVs, 0.95, &VertexLog, &Rms);
		OutWorst = OutWorstUncut;
		double Area = 0.0;
		for (const FIndex3i& Tri : Tris)
		{
			Area += 0.5 * FVector3d::CrossProduct(Points[Tri.B] - Points[Tri.A], Points[Tri.C] - Points[Tri.A]).Length();
		}
		if (OutWorstUncut <= FlattenCutTarget || Area < FlattenCutMinArea)
		{
			return bUncut;
		}

		// 지금까지 가장 좋은 결과(처음엔 가르지 않은 것)와 그 메시.
		TArray<FVector3d> BestPoints = Points;
		TArray<FIndex3i> BestTris = Tris;
		TArray<int32> BestIds = VertexIds;
		TArray<FVector2d> BestStart = Start;
		TArray<FVector2d> BestUVs = UVs;
		FFlattenStats BestStats = OutStats;
		double BestWorst = OutWorstUncut;
		double BestRms = Rms;
		TArray<int32> Source;
		Source.SetNum(NumOriginal);
		for (int32 V = 0; V < NumOriginal; ++V)
		{
			Source[V] = V;
		}
		int32 Cuts = 0;
		double CutLength = 0.0;

		// 틈 하나를 내고 펴 본다. 남기면 true.
		const auto TryCut = [&](const TArray<int32>& Path)
		{
			double Length = 0.0;
			for (int32 i = 0; i + 1 < Path.Num(); ++i)
			{
				Length += FVector3d::Distance(BestPoints[Path[i]], BestPoints[Path[i + 1]]);
			}
			if (Path.Num() < 2 || Length < FlattenCutMinLength)
			{
				return false;
			}
			TArray<FVector3d> TrialPoints = BestPoints;
			TArray<FIndex3i> TrialTris = BestTris;
			TArray<int32> TrialIds = BestIds;
			TArray<FVector2d> TrialStart = BestStart;
			TArray<int32> TrialSource = Source;
			CutChartAlongPath(Path, TrialTris, TrialPoints, TrialIds, TrialStart, TrialSource);
			TArray<FVector2d> TrialUVs = TrialStart;
			FFlattenStats TrialStats;
			if (!Run(TrialPoints, TrialTris, TrialIds, TrialUVs, TrialStats))
			{
				return false;
			}
			TArray<double> TrialLog;
			double TrialRms = 0.0;
			const double TrialWorst = ChartWorstStretch(TrialPoints, TrialTris, TrialUVs, 0.95, &TrialLog, &TrialRms);
			if (!(TrialRms < (1.0 - FlattenCutGain) * BestRms && TrialWorst <= BestWorst))
			{
				return false;
			}
			BestPoints = MoveTemp(TrialPoints);
			BestTris = MoveTemp(TrialTris);
			BestIds = MoveTemp(TrialIds);
			BestStart = MoveTemp(TrialStart);
			Source = MoveTemp(TrialSource);
			BestUVs = MoveTemp(TrialUVs);
			BestStats = TrialStats;
			BestWorst = TrialWorst;
			BestRms = TrialRms;
			VertexLog = MoveTemp(TrialLog);
			++Cuts;
			CutLength += Length;
			return true;
		};
		int32 Attempts = 0;

		// 1. 구멍을 다른 경계에 잇는다. 한 구멍씩, 고리 번호는 틈을 낼 때마다 바뀌므로 구멍은 정점 하나로 알아본다.
		TSet<int32> TriedHoles;
		while (Attempts < FlattenCutAttempts && Cuts < FlattenMaxCuts)
		{
			TArray<int32> LoopOf;
			TArray<TArray<int32>> Links;
			const int32 NumLoops = ChartBoundaryLoops(BestTris, BestPoints.Num(), LoopOf, Links);
			const int32 Outer = OuterBoundaryLoop(LoopOf, Links, NumLoops, BestStart);
			TSet<int32> TriedLoops;
			for (const int32 W : TriedHoles)
			{
				TriedLoops.Add(W < LoopOf.Num() ? LoopOf[W] : INDEX_NONE);
			}
			int32 Hole = INDEX_NONE;
			for (int32 Loop = 0; Loop < NumLoops && Hole == INDEX_NONE; ++Loop)
			{
				Hole = Loop != Outer && !TriedLoops.Contains(Loop) ? Loop : INDEX_NONE;
			}
			if (Hole == INDEX_NONE)
			{
				break;
			}
			TArray<bool> IsFrom;
			TArray<bool> IsTo;
			TArray<bool> Interior;
			IsFrom.SetNum(BestPoints.Num());
			IsTo.SetNum(BestPoints.Num());
			Interior.SetNum(BestPoints.Num());
			for (int32 V = 0; V < BestPoints.Num(); ++V)
			{
				IsFrom[V] = LoopOf[V] == Hole;
				IsTo[V] = LoopOf[V] != INDEX_NONE && LoopOf[V] != Hole;
				Interior[V] = LoopOf[V] == INDEX_NONE;
				if (IsFrom[V])
				{
					TriedHoles.Add(V);
				}
			}
			TArray<TArray<int32>> Neighbors;
			ChartNeighbors(BestTris, BestPoints.Num(), Neighbors);
			++Attempts;
			TryCut(ShortestChartPath(BestPoints, Neighbors, IsFrom, IsTo, Interior));
		}

		// 2. 가장 늘거나 준 곳(이웃과 평균한 값이 가장 큰 경계 안쪽 정점)에서 가장 가까운 경계까지.
		TArray<FVector3d> Rejected;
		while (Attempts < FlattenCutAttempts && Cuts < FlattenMaxCuts && BestWorst > FlattenCutTarget)
		{
			TArray<int32> LoopOf;
			TArray<TArray<int32>> Links;
			ChartBoundaryLoops(BestTris, BestPoints.Num(), LoopOf, Links);
			TArray<TArray<int32>> Neighbors;
			ChartNeighbors(BestTris, BestPoints.Num(), Neighbors);
			TArray<double> Smooth = VertexLog;
			for (int32 Pass = 0; Pass < FlattenCutSmoothPasses; ++Pass)
			{
				TArray<double> Next = Smooth;
				for (int32 V = 0; V < Smooth.Num(); ++V)
				{
					double Sum = 0.0;
					for (const int32 N : Neighbors[V])
					{
						Sum += Smooth[N];
					}
					Next[V] = Neighbors[V].Num() > 0 ? 0.5 * Smooth[V] + 0.5 * Sum / Neighbors[V].Num() : Smooth[V];
				}
				Smooth = MoveTemp(Next);
			}
			int32 Pick = INDEX_NONE;
			for (int32 V = 0; V < BestPoints.Num(); ++V)
			{
				if (LoopOf[V] != INDEX_NONE || (Pick != INDEX_NONE && Smooth[V] <= Smooth[Pick]))
				{
					continue;
				}
				bool bNearRejected = false;
				for (const FVector3d& R : Rejected)
				{
					bNearRejected |= FVector3d::DistSquared(R, BestPoints[V]) < FlattenCutRetryRadius * FlattenCutRetryRadius;
				}
				Pick = bNearRejected ? Pick : V;
			}
			if (Pick == INDEX_NONE)
			{
				break;
			}
			TArray<bool> IsFrom;
			TArray<bool> IsTo;
			TArray<bool> Interior;
			IsFrom.Init(false, BestPoints.Num());
			IsTo.SetNum(BestPoints.Num());
			Interior.SetNum(BestPoints.Num());
			IsFrom[Pick] = true;
			for (int32 V = 0; V < BestPoints.Num(); ++V)
			{
				IsTo[V] = LoopOf[V] != INDEX_NONE;
				Interior[V] = LoopOf[V] == INDEX_NONE;
			}
			const FVector3d PickPoint = BestPoints[Pick];
			++Attempts;
			if (!TryCut(ShortestChartPath(BestPoints, Neighbors, IsFrom, IsTo, Interior)))
			{
				Rejected.Add(PickPoint);
			}
		}

		if (Cuts == 0)
		{
			return bUncut;
		}
		Points = MoveTemp(BestPoints);
		Tris = MoveTemp(BestTris);
		VertexIds = MoveTemp(BestIds);
		UVs = MoveTemp(BestUVs);
		OutSource.Append(Source.GetData() + NumOriginal, Source.Num() - NumOriginal);
		OutCuts = Cuts;
		OutCutLength = CutLength;
		OutWorst = BestWorst;
		OutStats = BestStats;
		return true;
	}

	/**
	 * UV0 아일랜드를 못 쓸 때(펴 놓은 아일랜드 안에서 칸이 겹침) 어디서 왜 겹치는지 적는다. 겹친 칸마다 두 삼각형의
	 * 3-D 노멀과 거리로 셋을 가른다: 같은 자리(거리 < 0.05 cm, 같은 쪽 = 겹친 사본, 반대쪽 = 앞뒷면이 정점을 같이 씀), 등을 맞댐(반대쪽,
	 * 떨어짐 = 얇은 판의 앞뒷면이 한 아일랜드), 나머지(펴면서 아일랜드가 제 위로 접힘). 겹친 칸이 많은 아일랜드와 슬롯별 합을 적는다.
	 */
	void LogUV0Folds(const FMeshDescription& MD, const FMeshUV0Charts& Charts, const FCarrierAtlas& Atlas)
	{
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		const auto SlotOf = [&](int32 Tri) { return SlotNames[MD.GetTrianglePolygonGroup(Charts.MeshTriangle[Tri])]; };
		struct FChartFold
		{
			int32 Cells = 0;
			int32 Coincident = 0;
			int32 CoincidentFlipped = 0;
			int32 BackToBack = 0;
			int32 Other = 0;
			TArray<double> Distance;
		};
		TMap<int32, FChartFold> ByChart;
		TMap<FName, int32> BySlot;
		const int32 NumTris = Atlas.TriChart.Num();
		for (const FIntPoint& Pair : Atlas.FoldPairs)
		{
			if (!Charts.Mesh.IsTriangle(Pair.X) || !Charts.Mesh.IsTriangle(Pair.Y))
			{
				continue;
			}
			FChartFold& Fold = ByChart.FindOrAdd(Atlas.TriChart[Pair.Y]);
			++Fold.Cells;
			const FVector3d NA = Charts.Mesh.GetTriNormal(Pair.X);
			const FVector3d NB = Charts.Mesh.GetTriNormal(Pair.Y);
			const double Distance = FVector3d::Distance(Charts.Mesh.GetTriCentroid(Pair.X), Charts.Mesh.GetTriCentroid(Pair.Y));
			const double Facing = NA.Dot(NB);
			Fold.Distance.Add(Distance);
			if (Distance < 0.05)
			{
				++(Facing > 0.0 ? Fold.Coincident : Fold.CoincidentFlipped);
			}
			else if (Facing < -0.5)
			{
				++Fold.BackToBack;
			}
			else
			{
				++Fold.Other;
			}
			++BySlot.FindOrAdd(SlotOf(Pair.Y));
		}
		TArray<int32> ChartTris;
		ChartTris.Init(0, Charts.NumCharts);
		TArray<FVector3d> ChartCenter;
		ChartCenter.Init(FVector3d::ZeroVector, Charts.NumCharts);
		TArray<TMap<FName, int32>> ChartSlots;
		ChartSlots.SetNum(Charts.NumCharts);
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const int32 Chart = Atlas.TriChart[Tri];
			if (!ChartTris.IsValidIndex(Chart) || !Charts.Mesh.IsTriangle(Tri))
			{
				continue;
			}
			++ChartTris[Chart];
			ChartCenter[Chart] += Charts.Mesh.GetTriCentroid(Tri);
			++ChartSlots[Chart].FindOrAdd(SlotOf(Tri));
		}
		int32 Coincident = 0, Flipped = 0, Back = 0, Other = 0;
		for (const TPair<int32, FChartFold>& Pair : ByChart)
		{
			Coincident += Pair.Value.Coincident;
			Flipped += Pair.Value.CoincidentFlipped;
			Back += Pair.Value.BackToBack;
			Other += Pair.Value.Other;
		}
		UE_LOG(LogSoftbodyCarrier, Display,
			TEXT("UV0 fold check: %d folded cells in %d islands - same place facing the same way (a copy of the surface) %d, same place facing away (front and back share vertices) %d, back to back apart (a thin sheet's two sides in one island) %d, folded over itself %d"),
			Atlas.FoldPairs.Num(), ByChart.Num(), Coincident, Flipped, Back, Other);
		TArray<TPair<int32, FName>> Slots;
		for (const TPair<FName, int32>& Pair : BySlot)
		{
			Slots.Emplace(Pair.Value, Pair.Key);
		}
		Slots.Sort([](const TPair<int32, FName>& A, const TPair<int32, FName>& B) { return A.Key > B.Key; });
		FString SlotText;
		for (int32 Index = 0; Index < FMath::Min(Slots.Num(), 8); ++Index)
		{
			SlotText += FString::Printf(TEXT("%s%s %d"), Index > 0 ? TEXT(", ") : TEXT(""), *Slots[Index].Value.ToString(), Slots[Index].Key);
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("UV0 fold check by slot: %s"), *SlotText);
		TArray<TPair<int32, int32>> Ranked;
		for (const TPair<int32, FChartFold>& Pair : ByChart)
		{
			Ranked.Emplace(Pair.Value.Cells, Pair.Key);
		}
		Ranked.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Key > B.Key; });
		int32 TopCells = 0;
		for (int32 Index = 0; Index < FMath::Min(Ranked.Num(), 10); ++Index)
		{
			const int32 Chart = Ranked[Index].Value;
			FChartFold& Fold = ByChart[Chart];
			TopCells += Fold.Cells;
			Fold.Distance.Sort();
			FName Slot = NAME_None;
			int32 SlotTris = -1;
			for (const TPair<FName, int32>& Pair : ChartSlots[Chart])
			{
				if (Pair.Value > SlotTris)
				{
					Slot = Pair.Key;
					SlotTris = Pair.Value;
				}
			}
			const FVector3d Center = ChartCenter[Chart] / (double)FMath::Max(ChartTris[Chart], 1);
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("  Folded island %d: slot %s, %d triangles, %d folded cells (copy %d, front/back shared %d, back to back %d, folded over %d), distance between the overlapping triangles median %.2f cm, center (%.0f, %.0f, %.0f)"),
				Index + 1, *Slot.ToString(), ChartTris[Chart], Fold.Cells, Fold.Coincident, Fold.CoincidentFlipped, Fold.BackToBack, Fold.Other,
				Fold.Distance.Num() > 0 ? Fold.Distance[Fold.Distance.Num() / 2] : 0.0, Center.X, Center.Y, Center.Z);
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("UV0 fold check: the %d islands above hold %.0f%% of the folded cells"),
			FMath::Min(Ranked.Num(), 10), 100.0 * TopCells / FMath::Max(Atlas.FoldPairs.Num(), 1));
	}

	/**
	 * UV0 아일랜드를 차트로 삼아 아틀라스를 만든다(ADR-0007 §3.1). 봉투 슬롯 삼각형의 코너는 (정점, UV0)이 같으면 한
	 * 요소이고, 요소를 이어 가는 묶음이 아일랜드다. bFlatten이면 아일랜드 안을 이음매는 그대로 둔 채 칸 크기가 고르게 다시
	 * 펴고(§3.7), 아니면 차트 모양은 UV0 그대로다. 크기는 아일랜드마다 "UV 넓이 / 3D 넓이"로 나눠 칸 밀도를 맞춘다.
	 * UV 넓이가 0인 삼각형이 많거나 같은 차트 안에서 접힌 칸이 많으면 false와 이유를 돌려준다.
	 */
	bool ChartByMeshUV0(const FMeshDescription& MD, const TSet<FName>& Slots, int32 Resolution, bool bFlatten,
		FMeshUV0Charts& Out, FCarrierAtlas& Atlas, FString& OutReason)
	{
		FStaticMeshConstAttributes Attributes(MD);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector2f> UV0s = Attributes.GetVertexInstanceUVs();
		if (!UV0s.IsValid() || UV0s.GetNumChannels() < 1)
		{
			OutReason = TEXT("the mesh has no UV0 channel");
			return false;
		}
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);

		// --- 거울 UV 아일랜드 가르기 ---
		// 게임 모델은 좌우 반쪽(또는 얇은 판의 앞뒷면)이 같은 UV를 쓰고 가운데 선에서 이어져 있는 일이 많다. 그러면 (정점, UV0) 아일랜드
		// 하나에 두 반쪽이 들고, 다시 펼 때 테두리를 UV0 자리에 박으면 한쪽이 다른 쪽 위로 접힌다(UV0 fold check — 상용 캐릭터 6개 모두
		// 이것 때문에 UV0 아일랜드 전체를 버리고 자동 아일랜드로 넘어갔다). 거울 반쪽은 UV 감김 방향이 반대다. 그래서 두 방향이 다
		// 넉넉히 있는 아일랜드는 감김 방향이 바뀌는 곳에서 자른다 — 요소 열쇠에 방향을 더해, 바뀌는 선의 정점이 두 요소가 된다(§3.8의
		// 더 넣은 이음매와 같은 꼴). 조금만 뒤집힌 아일랜드(UV0의 작은 접힘)는 그대로 두고 다시 펴기가 고친다.
		struct FCorner
		{
			int32 Vertex = 0;
			uint64 UVBits = 0;
			FVector2f UV = FVector2f::ZeroVector;
		};
		TArray<FTriangleID> SlotTris;
		TArray<FCorner> Corners;
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if (Slots.Num() > 0 && !Slots.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
			{
				continue;
			}
			SlotTris.Add(Tri);
			const TArrayView<const FVertexInstanceID> Instances = MD.GetTriangleVertexInstances(Tri);
			for (int32 c = 0; c < 3; ++c)
			{
				FCorner& Corner = Corners.AddDefaulted_GetRef();
				Corner.Vertex = MD.GetVertexInstanceVertex(Instances[c]).GetValue();
				Corner.UV = UV0s.Get(Instances[c], 0);
				uint32 BitsX = 0;
				uint32 BitsY = 0;
				FMemory::Memcpy(&BitsX, &Corner.UV.X, sizeof(uint32));
				FMemory::Memcpy(&BitsY, &Corner.UV.Y, sizeof(uint32));
				Corner.UVBits = ((uint64)BitsX << 32) | (uint64)BitsY;
			}
		}
		const int32 NumSlotTris = SlotTris.Num();
		// 첫 판: 방향 없이 (정점, UV0) 아일랜드와 삼각형마다 감김 방향(UV 부호 넓이)·3-D 넓이
		TArray<int8> Side;
		Side.Init(0, NumSlotTris);
		int32 NumSplitIslands = 0;
		int32 NumSplitTris = 0;
		{
			TMap<TPair<int32, uint64>, int32> KeyOf;
			TArray<int32> KeyParent;
			TArray<int32> TriKeys;
			TriKeys.SetNum(NumSlotTris * 3);
			for (int32 i = 0; i < NumSlotTris * 3; ++i)
			{
				const TPair<int32, uint64> Key(Corners[i].Vertex, Corners[i].UVBits);
				if (const int32* Found = KeyOf.Find(Key))
				{
					TriKeys[i] = *Found;
				}
				else
				{
					TriKeys[i] = KeyParent.Num();
					KeyOf.Add(Key, TriKeys[i]);
					KeyParent.Add(TriKeys[i]);
				}
			}
			for (int32 t = 0; t < NumSlotTris; ++t)
			{
				const int32 A = FindRoot(KeyParent, TriKeys[t * 3]);
				KeyParent[FindRoot(KeyParent, TriKeys[t * 3 + 1])] = A;
				KeyParent[FindRoot(KeyParent, TriKeys[t * 3 + 2])] = A;
			}
			TArray<double> SignedUV;
			SignedUV.SetNum(NumSlotTris);
			TMap<int32, FVector2d> IslandSides;  // 뿌리 → (바른 방향 3-D 넓이, 뒤집힌 방향 3-D 넓이)
			TMap<int32, double> IslandUVSum;     // 뿌리 → |UV 부호 넓이| 합(작은 삼각형 문턱)
			TMap<int32, int32> IslandTris;
			for (int32 t = 0; t < NumSlotTris; ++t)
			{
				const FCorner* C = &Corners[t * 3];
				SignedUV[t] = 0.5 * FVector2d::CrossProduct(FVector2d(C[1].UV - C[0].UV), FVector2d(C[2].UV - C[0].UV));
				const FVector3d P0(Positions[FVertexID(C[0].Vertex)]);
				const FVector3d P1(Positions[FVertexID(C[1].Vertex)]);
				const FVector3d P2(Positions[FVertexID(C[2].Vertex)]);
				const double Area = 0.5 * FVector3d::CrossProduct(P1 - P0, P2 - P0).Length();
				const int32 Root = FindRoot(KeyParent, TriKeys[t * 3]);
				FVector2d& Sides = IslandSides.FindOrAdd(Root, FVector2d::ZeroVector);
				(SignedUV[t] >= 0.0 ? Sides.X : Sides.Y) += Area;
				IslandUVSum.FindOrAdd(Root) += FMath::Abs(SignedUV[t]);
				++IslandTris.FindOrAdd(Root);
			}
			// 두 방향이 다 넉넉한 아일랜드만: 적은 쪽이 넓이의 MirrorMinShare 이상
			TSet<int32> SplitRoots;
			for (const TPair<int32, FVector2d>& Pair : IslandSides)
			{
				const double Total = Pair.Value.X + Pair.Value.Y;
				if (Total > 0.0 && FMath::Min(Pair.Value.X, Pair.Value.Y) >= MirrorMinShare * Total)
				{
					SplitRoots.Add(Pair.Key);
				}
			}
			if (SplitRoots.Num() > 0)
			{
				// 방향: UV 넓이가 거의 0인 삼각형은 미정(-1)으로 두고 이웃(같은 열쇠를 쓰는 삼각형)의 다수를 따른다.
				TArray<TArray<int32>> KeyTris;
				KeyTris.SetNum(KeyParent.Num());
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					for (int32 c = 0; c < 3; ++c)
					{
						KeyTris[TriKeys[t * 3 + c]].AddUnique(t);
					}
				}
				TArray<int32> Pending;
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					const int32 Root = FindRoot(KeyParent, TriKeys[t * 3]);
					if (!SplitRoots.Contains(Root))
					{
						continue;
					}
					const double Small = 0.01 * IslandUVSum[Root] / (double)FMath::Max(IslandTris[Root], 1);
					Side[t] = (int8)(SignedUV[t] > Small ? 1 : SignedUV[t] < -Small ? 2 : -1);
					if (Side[t] < 0)
					{
						Pending.Add(t);
					}
				}
				const auto NeighborVote = [&](int32 t, int32& Votes1, int32& Votes2)
				{
					Votes1 = Votes2 = 0;
					for (int32 c = 0; c < 3; ++c)
					{
						for (const int32 Other : KeyTris[TriKeys[t * 3 + c]])
						{
							Votes1 += Side[Other] == 1 ? 1 : 0;
							Votes2 += Side[Other] == 2 ? 1 : 0;
						}
					}
				};
				for (int32 Pass = 0; Pass < 64 && Pending.Num() > 0; ++Pass)
				{
					TArray<int32> Still;
					for (const int32 t : Pending)
					{
						int32 Votes1 = 0;
						int32 Votes2 = 0;
						NeighborVote(t, Votes1, Votes2);
						if (Votes1 + Votes2 == 0)
						{
							Still.Add(t);
						}
						else
						{
							Side[t] = (int8)(Votes2 > Votes1 ? 2 : 1);
						}
					}
					if (Still.Num() == Pending.Num())
					{
						break;
					}
					Pending = MoveTemp(Still);
				}
				for (const int32 t : Pending)
				{
					Side[t] = 1;
				}
				// 작은 조각(같은 방향끼리 이어진 묶음이 MirrorMinPartTris보다 작음)은 둘레 방향으로 되돌린다 — UV0의 작은 접힘에 이음매를
				// 만들지 않게. 한 번만 본다.
				TArray<int32> PartParent;
				TArray<int32> TriPart;
				TriPart.Init(INDEX_NONE, NumSlotTris);
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					if (Side[t] <= 0)
					{
						continue;
					}
					TriPart[t] = PartParent.Add(PartParent.Num());
				}
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					if (TriPart[t] == INDEX_NONE)
					{
						continue;
					}
					for (int32 c = 0; c < 3; ++c)
					{
						for (const int32 Other : KeyTris[TriKeys[t * 3 + c]])
						{
							if (Other != t && TriPart[Other] != INDEX_NONE && Side[Other] == Side[t])
							{
								PartParent[FindRoot(PartParent, TriPart[Other])] = FindRoot(PartParent, TriPart[t]);
							}
						}
					}
				}
				TMap<int32, int32> PartSize;
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					if (TriPart[t] != INDEX_NONE)
					{
						++PartSize.FindOrAdd(FindRoot(PartParent, TriPart[t]));
					}
				}
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					if (TriPart[t] != INDEX_NONE && PartSize[FindRoot(PartParent, TriPart[t])] < MirrorMinPartTris)
					{
						Side[t] = (int8)(Side[t] == 1 ? 2 : 1);
					}
				}
				NumSplitIslands = SplitRoots.Num();
				for (int32 t = 0; t < NumSlotTris; ++t)
				{
					NumSplitTris += Side[t] > 0 ? 1 : 0;
				}
			}
		}

		// --- 요소와 아일랜드 ---
		// 요소 열쇠 = (정점 + 방향 × 2^32, UV0). 가르지 않는 아일랜드는 방향 0이라 전과 같다.
		TMap<TPair<int64, uint64>, int32> ElementOf;
		TArray<int32> ElementVertex;
		TArray<FVector2d> ElementUV0;
		TArray<int32> Parent;
		for (int32 t = 0; t < NumSlotTris; ++t)
		{
			int32 Ids[3];
			for (int32 c = 0; c < 3; ++c)
			{
				const FCorner& Corner = Corners[t * 3 + c];
				const TPair<int64, uint64> Key((int64)Corner.Vertex | ((int64)FMath::Max<int8>(Side[t], 0) << 32), Corner.UVBits);
				if (const int32* Found = ElementOf.Find(Key))
				{
					Ids[c] = *Found;
				}
				else
				{
					Ids[c] = ElementVertex.Num();
					ElementOf.Add(Key, Ids[c]);
					ElementVertex.Add(Corner.Vertex);
					ElementUV0.Add(FVector2d(Corner.UV));
					Parent.Add(Ids[c]);
				}
			}
			Out.MeshTriangle.Add(SlotTris[t]);
			Atlas.UVIndices.Append(Ids, 3);
			const int32 A = FindRoot(Parent, Ids[0]);
			Parent[FindRoot(Parent, Ids[1])] = A;
			Parent[FindRoot(Parent, Ids[2])] = A;
		}
		if (NumSplitIslands > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("UV0 islands with mirrored halves (or a sheet's two sides sharing UVs): %d, cut where the UV winding flips (%d triangles)"),
				NumSplitIslands, NumSplitTris);
		}
		const int32 NumTris = Out.MeshTriangle.Num();
		int32 NumElements = ElementVertex.Num();
		Out.ElementVertex = ElementVertex;
		if (NumTris == 0)
		{
			OutReason = TEXT("the pressable slots have no triangles");
			return false;
		}
		TMap<int32, int32> RootToChart;
		Out.ElementChart.SetNum(NumElements);
		for (int32 Element = 0; Element < NumElements; ++Element)
		{
			const int32 Root = FindRoot(Parent, Element);
			const int32* Chart = RootToChart.Find(Root);
			Out.ElementChart[Element] = Chart ? *Chart : RootToChart.Add(Root, RootToChart.Num());
		}
		Out.NumCharts = RootToChart.Num();
		if (Out.NumCharts > MaxUV0Charts)
		{
			OutReason = FString::Printf(TEXT("%d UV islands (limit %d)"), Out.NumCharts, MaxUV0Charts);
			return false;
		}

		// --- 아일랜드마다 칸 밀도 맞추기, 늘어남·UV 넓이 0 ---
		TArray<double> TriArea;
		TArray<double> TriAreaUV;
		TriArea.SetNum(NumTris);
		TriAreaUV.SetNum(NumTris);
		TArray<double> ChartArea;
		TArray<double> ChartAreaUV;
		ChartArea.Init(0.0, Out.NumCharts);
		ChartAreaUV.Init(0.0, Out.NumCharts);
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const int32* Ids = &Atlas.UVIndices[Tri * 3];
			const FVector3d P0(Positions[FVertexID(ElementVertex[Ids[0]])]);
			const FVector3d P1(Positions[FVertexID(ElementVertex[Ids[1]])]);
			const FVector3d P2(Positions[FVertexID(ElementVertex[Ids[2]])]);
			TriArea[Tri] = 0.5 * FVector3d::CrossProduct(P1 - P0, P2 - P0).Length();
			TriAreaUV[Tri] = 0.5 * FMath::Abs(FVector2d::CrossProduct(ElementUV0[Ids[1]] - ElementUV0[Ids[0]],
				ElementUV0[Ids[2]] - ElementUV0[Ids[0]]));
			const int32 Chart = Out.ElementChart[Ids[0]];
			ChartArea[Chart] += TriArea[Tri];
			ChartAreaUV[Chart] += TriAreaUV[Tri];
			Out.Area += TriArea[Tri];
		}
		// 아일랜드의 UV 밀도(UV 길이 / cm). UV 넓이가 0인 아일랜드는 다른 아일랜드의 중앙값을 쓴다.
		TArray<double> Density;
		Density.Init(0.0, Out.NumCharts);
		TArray<double> Valid;
		for (int32 Chart = 0; Chart < Out.NumCharts; ++Chart)
		{
			if (ChartArea[Chart] > 1e-12 && ChartAreaUV[Chart] > 1e-20)
			{
				Density[Chart] = FMath::Sqrt(ChartAreaUV[Chart] / ChartArea[Chart]);
				Valid.Add(Density[Chart]);
			}
		}
		if (Valid.Num() == 0)
		{
			OutReason = TEXT("every UV0 triangle has zero area");
			return false;
		}
		Valid.Sort();
		const double MedianDensity = Valid[Valid.Num() / 2];
		double Degenerate = 0.0;
		TArray<TPair<double, double>> Stretch;
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const double K = Density[Out.ElementChart[Atlas.UVIndices[Tri * 3]]];
			if (K <= 0.0 || TriAreaUV[Tri] < 0.01 * TriArea[Tri] * K * K)
			{
				Degenerate += TriArea[Tri];
			}
			else if (TriArea[Tri] > 0.0)
			{
				Stretch.Emplace(FMath::Sqrt(TriAreaUV[Tri] / (TriArea[Tri] * K * K)), TriArea[Tri]);
			}
		}
		Stretch.Sort([](const TPair<double, double>& A, const TPair<double, double>& B) { return A.Key < B.Key; });
		const double DegenerateRatio = Degenerate / FMath::Max(Out.Area, 1e-12);
		UE_LOG(LogSoftbodyCarrier, Display,
			TEXT("UV0 islands: %d, triangles %d, area %.0f, cell density relative to the island mean 5/50/95%% %.2f / %.2f / %.2f, area with zero UV area %.2f%%"),
			Out.NumCharts, NumTris, Out.Area, WeightedPercentile(Stretch, 0.05), WeightedPercentile(Stretch, 0.5),
			WeightedPercentile(Stretch, 0.95), 100.0 * DegenerateRatio);
		if (DegenerateRatio > MaxUV0DegenerateArea)
		{
			OutReason = FString::Printf(TEXT("triangles with zero UV area cover %.1f%% of the surface"), 100.0 * DegenerateRatio);
			return false;
		}

		// --- 아일랜드 안을 고르게 다시 펴기 (§3.7) ---
		// 요소마다 차트 좌표(cm). 시작 = UV0 / 아일랜드 밀도. 다시 펴지 않거나 못 편 아일랜드는 이 값 그대로다.
		TArray<FVector2d> FlatUVs;
		FlatUVs.SetNum(NumElements);
		for (int32 Element = 0; Element < NumElements; ++Element)
		{
			const double K = Density[Out.ElementChart[Element]];
			FlatUVs[Element] = ElementUV0[Element] / (K > 0.0 ? K : MedianDensity);
		}
		if (bFlatten)
		{
			const double StartSeconds = FPlatformTime::Seconds();
			TArray<TArray<int32>> ChartElements;
			ChartElements.SetNum(Out.NumCharts);
			TArray<int32> LocalIndex;
			LocalIndex.SetNum(NumElements);
			for (int32 Element = 0; Element < NumElements; ++Element)
			{
				LocalIndex[Element] = ChartElements[Out.ElementChart[Element]].Add(Element);
			}
			TArray<TArray<FIndex3i>> ChartTris;
			ChartTris.SetNum(Out.NumCharts);
			TArray<TArray<int32>> ChartTriIds;
			ChartTriIds.SetNum(Out.NumCharts);
			for (int32 Tri = 0; Tri < NumTris; ++Tri)
			{
				const int32* Ids = &Atlas.UVIndices[Tri * 3];
				ChartTris[Out.ElementChart[Ids[0]]].Add(FIndex3i(LocalIndex[Ids[0]], LocalIndex[Ids[1]], LocalIndex[Ids[2]]));
				ChartTriIds[Out.ElementChart[Ids[0]]].Add(Tri);
			}
			// 이음매를 더 넣어 편 아일랜드(§3.8): 아일랜드 안 번호의 결과 UV·새 정점의 원래 번호·틈 수와 길이·칸 크기 배율.
			TArray<TArray<FVector2d>> ChartUVs;
			ChartUVs.SetNum(Out.NumCharts);
			TArray<TArray<int32>> ChartSource;
			ChartSource.SetNum(Out.NumCharts);
			TArray<int32> CutCount;
			CutCount.Init(0, Out.NumCharts);
			TArray<double> CutLength;
			CutLength.Init(0.0, Out.NumCharts);
			TArray<double> WorstUncut;
			WorstUncut.Init(1.0, Out.NumCharts);
			TArray<double> WorstFinal;
			WorstFinal.Init(1.0, Out.NumCharts);
			TArray<uint8> Flattened;
			Flattened.Init(0, Out.NumCharts);
			TArray<int32> Flipped;
			Flipped.Init(0, Out.NumCharts);
			TArray<int32> FlippedBefore;
			FlippedBefore.Init(0, Out.NumCharts);
			TArray<int32> Failure;
			Failure.Init(0, Out.NumCharts);
			TArray<int32> Skipped;
			Skipped.Init(0, Out.NumCharts);
			TArray<double> Blend;
			Blend.Init(0.0, Out.NumCharts);
			TArray<int32> PinnedCount;
			PinnedCount.Init(0, Out.NumCharts);
			// 겹침을 셀 격자 = 대략 칸 크기(cm): 아틀라스의 약 1/3이 칸으로 찬다.
			const double FoldCell = FMath::Sqrt(Out.Area / (0.33 * (double)Resolution * (double)Resolution));
			ParallelFor(Out.NumCharts, [&](int32 Chart)
			{
				const TArray<int32>& Elements = ChartElements[Chart];
				if (ChartTris[Chart].Num() == 0)
				{
					return;
				}
				TArray<FVector3d> Points;
				TArray<FVector2d> UVs;
				TArray<int32> VertexIds;
				Points.Reserve(Elements.Num());
				UVs.Reserve(Elements.Num());
				VertexIds.Reserve(Elements.Num());
				for (const int32 Element : Elements)
				{
					Points.Add(FVector3d(Positions[FVertexID(ElementVertex[Element])]));
					UVs.Add(FlatUVs[Element]);
					VertexIds.Add(ElementVertex[Element]);
				}
				FFlattenStats Stats;
				const bool bFlattened = FlattenChartWithCuts(Points, ChartTris[Chart], VertexIds, FoldCell, UVs, ChartSource[Chart],
					CutCount[Chart], CutLength[Chart], WorstUncut[Chart], WorstFinal[Chart], Stats);
				FlippedBefore[Chart] = Stats.FlippedBefore;
				Flipped[Chart] = Stats.Flipped;
				Skipped[Chart] = Stats.Skipped;
				PinnedCount[Chart] = Stats.Pinned;
				Blend[Chart] = Stats.Blend;
				Failure[Chart] = Stats.Failure;
				if (bFlattened)
				{
					ChartUVs[Chart] = MoveTemp(UVs);
					Flattened[Chart] = 1;
				}
			});
			// 결과를 요소에 옮긴다. 틈으로 새로 생긴 정점은 새 요소(같은 메시 정점·UV0, 같은 차트)가 되고, 그 아일랜드 삼각형의
			// 코너가 가리키는 요소를 바꾼다 — 틈을 따라 캐리어 UV 채널 값이 둘로 나뉘어 렌더 정점이 나뉜다(UV0 이음매와 같다).
			int32 NumCutCharts = 0;
			int32 NumCuts = 0;
			double TotalCutLength = 0.0;
			const int32 ElementsBefore = NumElements;
			for (int32 Chart = 0; Chart < Out.NumCharts; ++Chart)
			{
				if (!Flattened[Chart])
				{
					continue;
				}
				TArray<int32>& Elements = ChartElements[Chart];
				const int32 NumOriginal = Elements.Num();
				for (int32 i = 0; i < NumOriginal; ++i)
				{
					FlatUVs[Elements[i]] = ChartUVs[Chart][i];
				}
				if (ChartSource[Chart].Num() == 0 && CutCount[Chart] == 0)
				{
					continue;
				}
				++NumCutCharts;
				NumCuts += CutCount[Chart];
				TotalCutLength += CutLength[Chart];
				for (int32 j = 0; j < ChartSource[Chart].Num(); ++j)
				{
					const int32 From = Elements[ChartSource[Chart][j]];
					const int32 FromVertex = ElementVertex[From];
					const FVector2d FromUV0 = ElementUV0[From];
					const int32 Element = ElementVertex.Add(FromVertex);
					ElementUV0.Add(FromUV0);
					Out.ElementChart.Add(Chart);
					FlatUVs.Add(ChartUVs[Chart][NumOriginal + j]);
					Elements.Add(Element);
				}
				for (int32 Local = 0; Local < ChartTriIds[Chart].Num(); ++Local)
				{
					const FIndex3i& Tri = ChartTris[Chart][Local];
					int32* Ids = &Atlas.UVIndices[ChartTriIds[Chart][Local] * 3];
					Ids[0] = Elements[Tri.A];
					Ids[1] = Elements[Tri.B];
					Ids[2] = Elements[Tri.C];
				}
			}
			NumElements = ElementVertex.Num();
			Out.ElementVertex = ElementVertex;
			if (NumCutCharts > 0)
			{
				UE_LOG(LogSoftbodyCarrier, Display,
					TEXT("Islands flattened with extra seams: %d, cuts %d (including cuts joining holes), total cut length %.0f, UV elements split along cuts %d"),
					NumCutCharts, NumCuts, TotalCutLength, NumElements - ElementsBefore);
			}

			// 펴기 전후 칸 밀도(아일랜드 평균 대비)와 가로세로 비. 다시 편 아일랜드의 넓이는 3D와 거의 같지만 정확히는 아니다.
			TArray<double> FlatAreaUV;
			FlatAreaUV.Init(0.0, Out.NumCharts);
			for (int32 Tri = 0; Tri < NumTris; ++Tri)
			{
				const int32* Ids = &Atlas.UVIndices[Tri * 3];
				FlatAreaUV[Out.ElementChart[Ids[0]]] +=
					0.5 * FMath::Abs(FVector2d::CrossProduct(FlatUVs[Ids[1]] - FlatUVs[Ids[0]], FlatUVs[Ids[2]] - FlatUVs[Ids[0]]));
			}
			TArray<TPair<double, double>> DensityBefore;
			TArray<TPair<double, double>> DensityAfter;
			TArray<TPair<double, double>> AnisoBefore;
			TArray<TPair<double, double>> AnisoAfter;
			TArray<TArray<TPair<double, double>>> ChartDensity;
			ChartDensity.SetNum(Out.NumCharts);
			for (int32 Tri = 0; Tri < NumTris; ++Tri)
			{
				const int32* Ids = &Atlas.UVIndices[Tri * 3];
				const int32 Chart = Out.ElementChart[Ids[0]];
				const FVector3d P0(Positions[FVertexID(ElementVertex[Ids[0]])]);
				const FVector3d P1(Positions[FVertexID(ElementVertex[Ids[1]])]);
				const FVector3d P2(Positions[FVertexID(ElementVertex[Ids[2]])]);
				double Scale = 0.0;
				double Aniso = 0.0;
				const double KBefore = Density[Chart] > 0.0 ? Density[Chart] : MedianDensity;
				if (TriangleStretch(P0, P1, P2, ElementUV0[Ids[0]], ElementUV0[Ids[1]], ElementUV0[Ids[2]], Scale, Aniso))
				{
					DensityBefore.Emplace(Scale / KBefore, TriArea[Tri]);
					AnisoBefore.Emplace(Aniso, TriArea[Tri]);
				}
				const double KAfter = FMath::Sqrt(FlatAreaUV[Chart] / FMath::Max(ChartArea[Chart], 1e-12));
				if (KAfter > 0.0 && TriangleStretch(P0, P1, P2, FlatUVs[Ids[0]], FlatUVs[Ids[1]], FlatUVs[Ids[2]], Scale, Aniso))
				{
					DensityAfter.Emplace(Scale / KAfter, TriArea[Tri]);
					AnisoAfter.Emplace(Aniso, TriArea[Tri]);
					ChartDensity[Chart].Emplace(Scale / KAfter, TriArea[Tri]);
				}
			}
			const auto ByKey = [](const TPair<double, double>& A, const TPair<double, double>& B) { return A.Key < B.Key; };
			DensityBefore.Sort(ByKey);
			DensityAfter.Sort(ByKey);
			AnisoBefore.Sort(ByKey);
			AnisoAfter.Sort(ByKey);
			int32 NumFlattened = 0;
			int32 NumFlipped = 0;
			int32 NumFlippedBefore = 0;
			int32 NumUnsolved = 0;
			int32 NumMoreFlips = 0;
			int32 NumMoreFolds = 0;
			int32 NumSkipped = 0;
			int32 NumBlended = 0;
			double LeastBlend = 1.0;
			int32 NumPinned = 0;
			for (int32 Chart = 0; Chart < Out.NumCharts; ++Chart)
			{
				NumSkipped += Skipped[Chart];
				NumPinned += Flattened[Chart] ? PinnedCount[Chart] : 0;
				NumMoreFolds += Failure[Chart] == 3 ? 1 : 0;
				if (Flattened[Chart] && Blend[Chart] < 1.0)
				{
					++NumBlended;
					LeastBlend = FMath::Min(LeastBlend, Blend[Chart]);
				}
				NumFlattened += Flattened[Chart];
				NumFlipped += Flattened[Chart] ? Flipped[Chart] : FlippedBefore[Chart];
				NumFlippedBefore += FlippedBefore[Chart];
				NumUnsolved += Failure[Chart] == 1 ? 1 : 0;
				NumMoreFlips += Failure[Chart] == 2 ? 1 : 0;
			}
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Island re-flattening: %d flattened (%.1f s), cell density 5/50/95/99%% %.2f / %.2f / %.2f / %.2f (UV0 %.2f / %.2f / %.2f / %.2f), aspect ratio 95%% %.2f (UV0 %.2f), flipped or zero-area triangles %d (UV0 %d), flat triangles left out of the solve %d, border vertices pinned to UV0 to avoid overlap %d, islands blended toward UV0 because of overlap %d (smallest flattened share %.2f), islands left in their UV0 shape %d (no solution %d, more flips %d, more overlap %d)"),
				NumFlattened, FPlatformTime::Seconds() - StartSeconds,
				WeightedPercentile(DensityAfter, 0.05), WeightedPercentile(DensityAfter, 0.5), WeightedPercentile(DensityAfter, 0.95),
				WeightedPercentile(DensityAfter, 0.99), WeightedPercentile(DensityBefore, 0.05), WeightedPercentile(DensityBefore, 0.5),
				WeightedPercentile(DensityBefore, 0.95), WeightedPercentile(DensityBefore, 0.99), WeightedPercentile(AnisoAfter, 0.95),
				WeightedPercentile(AnisoBefore, 0.95), NumFlipped, NumFlippedBefore, NumSkipped, NumPinned, NumBlended, LeastBlend,
				NumUnsolved + NumMoreFlips + NumMoreFolds, NumUnsolved, NumMoreFlips, NumMoreFolds);
			// 크게 굽은 아일랜드(공 하나를 통째로 편 것 등)는 이음매를 그대로 두고는 고르게 펴지지 않는다. 자리를 알 수 있게 적는다.
			TArray<TPair<double, int32>> Uneven;
			for (int32 Chart = 0; Chart < Out.NumCharts; ++Chart)
			{
				ChartDensity[Chart].Sort(ByKey);
				if (ChartDensity[Chart].Num() > 0 && (WeightedPercentile(ChartDensity[Chart], 0.05) < FlattenEvenLow
					|| WeightedPercentile(ChartDensity[Chart], 0.95) > FlattenEvenHigh))
				{
					Uneven.Emplace(ChartArea[Chart], Chart);
				}
			}
			Uneven.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key > B.Key; });
			for (int32 Index = 0; Index < FMath::Min(Uneven.Num(), 5); ++Index)
			{
				const int32 Chart = Uneven[Index].Value;
				FVector3d Center = FVector3d::ZeroVector;
				for (const int32 Element : ChartElements[Chart])
				{
					Center += FVector3d(Positions[FVertexID(ElementVertex[Element])]);
				}
				Center /= (double)FMath::Max(ChartElements[Chart].Num(), 1);
				UE_LOG(LogSoftbodyCarrier, Display,
					TEXT("  Unevenly flattened island %d/%d: area %.0f, triangles %d, center (%.0f, %.0f, %.0f), cell density 5/95%% %.2f / %.2f, worst direction 95%% %.2f -> %d cuts %.2f, flipped or zero-area triangles UV0 %d -> %d%s"),
					Index + 1, Uneven.Num(), ChartArea[Chart], ChartTris[Chart].Num(), Center.X, Center.Y, Center.Z,
					WeightedPercentile(ChartDensity[Chart], 0.05), WeightedPercentile(ChartDensity[Chart], 0.95), WorstUncut[Chart],
					CutCount[Chart], WorstFinal[Chart], FlippedBefore[Chart], Flipped[Chart],
					Flattened[Chart] ? TEXT("") : Failure[Chart] == 3 ? TEXT(" (overlapped, kept its UV0 shape)")
					: Failure[Chart] == 2 ? TEXT(" (more flipped triangles, kept its UV0 shape)") : TEXT(" (no solution, kept its UV0 shape)"));
			}
		}

		// --- 아틀라스 메시와 차트 좌표 ---
		// 차트 좌표 = 위 cm 좌표 / 아일랜드 밀도 × 정규화(넓이 합 약 0.5, XAtlas 출력과 같은 밀도). UV0 모양 그대로면 밀도가
		// 정확히 1이고, 다시 편 아일랜드는 1에 가깝다.
		const double Normalize = FMath::Sqrt(0.5 / FMath::Max(Out.Area, 1e-6));
		TArray<double> FlatDensity;
		FlatDensity.Init(0.0, Out.NumCharts);
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const int32* Ids = &Atlas.UVIndices[Tri * 3];
			FlatDensity[Out.ElementChart[Ids[0]]] +=
				0.5 * FMath::Abs(FVector2d::CrossProduct(FlatUVs[Ids[1]] - FlatUVs[Ids[0]], FlatUVs[Ids[2]] - FlatUVs[Ids[0]]));
		}
		for (int32 Chart = 0; Chart < Out.NumCharts; ++Chart)
		{
			FlatDensity[Chart] = FlatDensity[Chart] > 1e-20 && ChartArea[Chart] > 1e-12
				? FMath::Sqrt(FlatDensity[Chart] / ChartArea[Chart]) : 1.0;
		}
		TArray<FVector2d> SourceUVs;
		SourceUVs.SetNum(NumElements);
		TMap<int32, int32> MeshVertex;
		Atlas.UVToVertex.SetNum(NumElements);
		for (int32 Element = 0; Element < NumElements; ++Element)
		{
			SourceUVs[Element] = FlatUVs[Element] * (Normalize / FlatDensity[Out.ElementChart[Element]]);
			const int32 Vertex = ElementVertex[Element];
			const int32* Found = MeshVertex.Find(Vertex);
			Atlas.UVToVertex[Element] = Found ? *Found
				: MeshVertex.Add(Vertex, Out.Mesh.AppendVertex(FVector3d(Positions[FVertexID(Vertex)])));
			// 텍셀 표: 정점마다 차트 번호가 가장 작은 요소
			const int32* Current = Out.VertexElement.Find(Vertex);
			if (!Current || Out.ElementChart[Element] < Out.ElementChart[*Current])
			{
				Out.VertexElement.Add(Vertex, Element);
			}
		}
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			FIndex3i Ids(Atlas.UVToVertex[Atlas.UVIndices[Tri * 3]], Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + 1]],
				Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + 2]]);
			int32 Added = Out.Mesh.AppendTriangle(Ids);
			if (Added < 0)
			{
				// 비다양체 변·퇴화 삼각형: 이 삼각형만 정점을 따로 둔다(위치 질의에만 쓰므로 무해하다).
				Ids = FIndex3i(Out.Mesh.AppendVertex(Out.Mesh.GetVertex(Ids.A)), Out.Mesh.AppendVertex(Out.Mesh.GetVertex(Ids.B)),
					Out.Mesh.AppendVertex(Out.Mesh.GetVertex(Ids.C)));
				Added = Out.Mesh.AppendTriangle(Ids);
			}
			if (Added != Tri)
			{
				OutReason = FString::Printf(TEXT("could not add triangle %d to the contact map mesh"), Tri);
				return false;
			}
		}

		// --- 배치 ---
		if (!PackAtlas(SourceUVs, Out.ElementChart, Out.NumCharts, Resolution, Atlas))
		{
			OutReason = TEXT("the islands did not fit in the contact map");
			return false;
		}
		int32 Surface = 0;
		for (const int32 Tri : Atlas.TexelTri)
		{
			Surface += Tri != INDEX_NONE ? 1 : 0;
		}
		if (Atlas.FoldTexels > MaxUV0FoldTexels * Surface)
		{
			OutReason = FString::Printf(TEXT("%.1f%% of cells fold or overlap within the same island"), 100.0 * Atlas.FoldTexels / FMath::Max(Surface, 1));
			LogUV0Folds(MD, Out, Atlas);
			return false;
		}
		return true;
	}

	/**
	 * 그려지는 면(ADR-0007 §3.6): UV0 차트 칸의 점을 봉투 점 대신 렌더 삼각형 위에 둘 때 쓰는 정점 노멀.
	 * 봉투는 격자로 다시 떠서 줄인 각진 껍질이라, 그 위의 칸 점으로 구한 노멀은 화면 노멀과 몇 도씩 어긋난다. 가만히 있을 때는
	 * 노멀 패스가 같은 칸 점으로 전후를 비교해 어긋남이 지워지지만, 눌린 자리는 뒤 모양이 콜라이더 면이 되어 어긋남이 음영에
	 * 거꾸로 찍히고 정점도 콜라이더 면에서 (정점 − 봉투 점)만큼 떨어진다.
	 */
	struct FDrawnSurface
	{
		/** 아틀라스 메시 정점 → 렌더 노멀(같은 위치끼리 넓이 가중 평균 — UV0 이음매 양쪽 차트의 면이 이어진다). */
		TArray<FVector3d> VertexNormal;
		/** 아틀라스 삼각형 코너(삼각형 × 3 + 코너) → 그 코너의 렌더 노멀(칸의 화면 노멀을 보간할 때). */
		TArray<FVector3d> CornerNormal;
	};

	/** Normals: 정점 인스턴스마다 렌더 노멀(SoftbodyBakeShared::RenderedNormals). */
	void BuildDrawnSurface(const FMeshDescription& MD, const TArray<FVector3f>& Normals, const FMeshUV0Charts& UV0, const FCarrierAtlas& Atlas,
		FDrawnSurface& Out)
	{
		const FDynamicMesh3& Mesh = UV0.Mesh;
		const auto PositionKey = [](const FVector3d& P)
		{
			return FIntVector((int32)FMath::RoundToDouble(P.X * 1000.0), (int32)FMath::RoundToDouble(P.Y * 1000.0),
				(int32)FMath::RoundToDouble(P.Z * 1000.0));
		};
		const int32 NumTris = UV0.MeshTriangle.Num();
		Out.CornerNormal.Init(FVector3d::ZeroVector, NumTris * 3);
		TMap<FIntVector, FVector3d> SumAt;
		int32 Fallback = 0;
		for (int32 Tri = 0; Tri < NumTris; ++Tri)
		{
			const FVector3d Face = Mesh.GetTriNormal(Tri);
			const double Area = Mesh.GetTriArea(Tri);
			const TArrayView<const FVertexInstanceID> Instances = MD.GetTriangleVertexInstances(UV0.MeshTriangle[Tri]);
			for (int32 c = 0; c < 3; ++c)
			{
				FVector3d N = Normals.IsValidIndex(Instances[c].GetValue()) ? FVector3d(Normals[Instances[c].GetValue()]) : FVector3d::ZeroVector;
				if (N.SquaredLength() < 0.25)
				{
					// 노멀이 없는 메시(빌드에서 다시 계산)는 면 노멀로 채운다.
					N = Face;
					++Fallback;
				}
				N.Normalize();
				Out.CornerNormal[Tri * 3 + c] = N;
				SumAt.FindOrAdd(PositionKey(Mesh.GetVertex(Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + c]]))) += N * Area;
			}
		}
		Out.VertexNormal.Init(FVector3d::ZeroVector, Mesh.MaxVertexID());
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			Out.VertexNormal[Vertex] = SumAt.FindRef(PositionKey(Mesh.GetVertex(Vertex))).GetSafeNormal();
		}
		if (Fallback > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Rendered surface: %d triangle corners had no normal and use the face normal instead"), Fallback);
		}
	}

	/**
	 * 렌더 삼각형 위 점을 세 정점 노멀 쪽으로 휜 점(퐁 곡면, Boubekeur & Alexa 2008): 평평한 점을 세 정점의 접평면에 내려
	 * 무게중심 좌표로 섞고 Curve만큼 옮긴다. 정점에서는 정점 자체라 코너 WPO가 읽는 칸 점과 렌더 정점이 같다. 한 변 위의 점은
	 * 그 변의 두 정점만으로 정해져 이웃 삼각형과 이어진다. 연장 좌표(고스트 칸)에도 그대로 쓴다.
	 */
	FVector3d DrawnPoint(const FDynamicMesh3& Mesh, const FCarrierAtlas& Atlas, const FDrawnSurface& Drawn, int32 Tri,
		const FVector3d& Bary, double Curve)
	{
		FVector3d Corner[3];
		FVector3d Normal[3];
		for (int32 c = 0; c < 3; ++c)
		{
			const int32 Vertex = Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + c]];
			Corner[c] = Mesh.GetVertex(Vertex);
			Normal[c] = Drawn.VertexNormal[Vertex];
		}
		const FVector3d Flat = Corner[0] * Bary.X + Corner[1] * Bary.Y + Corner[2] * Bary.Z;
		FVector3d Curved = FVector3d::ZeroVector;
		for (int32 c = 0; c < 3; ++c)
		{
			Curved += (Flat - Normal[c] * (Flat - Corner[c]).Dot(Normal[c])) * Bary[c];
		}
		return Flat + (Curved - Flat) * Curve;
	}

	/**
	 * 화면 노멀(ADR-0007 §3.6)을 CarrierRedirect w 한 칸에 넣는다: 단위 벡터를 팔면체로 펼친 정사각형 좌표 두 값을 12비트씩
	 * 정수 하나로 묶고 1을 더한다(0 = 없음). 2²⁴ 이하 정수라 32비트 float에 그대로 담긴다. 셰이더 SB_ScreenNormal이 푼다.
	 */
	float EncodeScreenNormal(const FVector3d& N)
	{
		const double Sum = FMath::Abs(N.X) + FMath::Abs(N.Y) + FMath::Abs(N.Z);
		if (Sum < 1e-12)
		{
			return 0.f;
		}
		FVector2d E(N.X / Sum, N.Y / Sum);
		if (N.Z < 0.0)
		{
			E = FVector2d((1.0 - FMath::Abs(E.Y)) * (E.X >= 0.0 ? 1.0 : -1.0), (1.0 - FMath::Abs(E.X)) * (E.Y >= 0.0 ? 1.0 : -1.0));
		}
		const uint32 A = (uint32)FMath::Clamp(FMath::RoundToInt((E.X * 0.5 + 0.5) * 4095.0), 0, 4095);
		const uint32 B = (uint32)FMath::Clamp(FMath::RoundToInt((E.Y * 0.5 + 0.5) * 4095.0), 0, 4095);
		return (float)(1u + (A << 12) + B);
	}

	FVector3d DecodeScreenNormal(float Code)
	{
		if (Code < 0.5f)
		{
			return FVector3d::ZeroVector;
		}
		const uint32 Bits = (uint32)FMath::RoundToInt(Code) - 1u;
		const FVector2d E((Bits >> 12) * (2.0 / 4095.0) - 1.0, (Bits & 4095u) * (2.0 / 4095.0) - 1.0);
		FVector3d N(E.X, E.Y, 1.0 - FMath::Abs(E.X) - FMath::Abs(E.Y));
		const double Fold = FMath::Max(-N.Z, 0.0);
		N.X += N.X >= 0.0 ? -Fold : Fold;
		N.Y += N.Y >= 0.0 ? -Fold : Fold;
		return N.GetSafeNormal();
	}

	/** 봉투 슬롯 렌더 삼각형과 코너 노멀 — XAtlas 차트에서 칸의 화면 노멀·덩어리 값을 가장 가까운 렌더 삼각형으로 찾을 때. */
	struct FRenderNormals
	{
		FDynamicMesh3 Mesh;
		/** 삼각형 × 3 + 코너 → 그 코너의 렌더 노멀(노멀이 없으면 면 노멀). */
		TArray<FVector3d> CornerNormal;
		/** Mesh 정점 → 메시 디스크립션 정점 번호(FVertexID 값). */
		TArray<int32> SourceVertex;
	};

	/** Skip(있으면): 메시 설명 삼각형 자리마다 1이면 뺀다 — 떠 있는 하드 서피스 조각(ADR-0037 §3.7): 그 밑 칸이 밑 겉면의 값을 읽는다. */
	void BuildRenderNormals(const FMeshDescription& MD, const TArray<FVector3f>& Normals, const TSet<FName>& Slots, FRenderNormals& Out,
		const TArray<uint8>* Skip = nullptr)
	{
		FStaticMeshConstAttributes Attributes(MD);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TMap<FVertexID, int32> Mapped;
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if ((Slots.Num() > 0 && !Slots.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
				|| (Skip && Skip->IsValidIndex(Tri.GetValue()) && (*Skip)[Tri.GetValue()]))
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(Tri);
			const auto AddVertex = [&Out](const FVector3d& P, FVertexID Source)
			{
				const int32 Id = Out.Mesh.AppendVertex(P);
				Out.SourceVertex.SetNum(FMath::Max(Out.SourceVertex.Num(), Id + 1));
				Out.SourceVertex[Id] = Source.GetValue();
				return Id;
			};
			FIndex3i Ids;
			for (int32 c = 0; c < 3; ++c)
			{
				const int32* Found = Mapped.Find(Corners[c]);
				Ids[c] = Found ? *Found : Mapped.Add(Corners[c], AddVertex(FVector3d(Positions[Corners[c]]), Corners[c]));
			}
			int32 Added = Out.Mesh.AppendTriangle(Ids);
			if (Added < 0)
			{
				// 비다양체 변·겹친 삼각형: 이 삼각형만 정점을 따로 둔다.
				Ids = FIndex3i(AddVertex(Out.Mesh.GetVertex(Ids.A), Corners[0]), AddVertex(Out.Mesh.GetVertex(Ids.B), Corners[1]),
					AddVertex(Out.Mesh.GetVertex(Ids.C), Corners[2]));
				Added = Out.Mesh.AppendTriangle(Ids);
			}
			if (Added < 0)
			{
				continue;
			}
			const FVector3d Face = Out.Mesh.GetTriNormal(Added);
			const TArrayView<const FVertexInstanceID> Instances = MD.GetTriangleVertexInstances(Tri);
			Out.CornerNormal.SetNum(FMath::Max(Out.CornerNormal.Num(), (Added + 1) * 3));
			for (int32 c = 0; c < 3; ++c)
			{
				FVector3d N = Normals.IsValidIndex(Instances[c].GetValue()) ? FVector3d(Normals[Instances[c].GetValue()]) : FVector3d::ZeroVector;
				Out.CornerNormal[Added * 3 + c] = N.SquaredLength() < 0.25 ? Face : N.GetSafeNormal();
			}
		}
	}

	/**
	 * 메시 캐리어 UV 채널(ADR-0007 §3.3)을 정점 인스턴스마다 쓴다. UV0 차트면 봉투 슬롯 삼각형의 인스턴스에 자기 아일랜드
	 * 차트의 캐리어 UV를, 나머지에 (−1, −1)(텍셀 표를 읽는다)을 쓴다. 값이 바뀌는 곳은 UV0 이음매와 더 넣은 이음매(§3.8)다.
	 * UV0 이음매만이면 렌더 정점이 늘지 않고, 더 넣은 이음매는 그 길을 따라 렌더 정점이 나뉜다. 한 인스턴스를 쓰는 삼각형들이
	 * 이음매 양쪽에 있으면 인스턴스를 복사해 나눈다(노멀·탄젠트·색·UV를 복사). UV0 차트가 아니면 채널이 이미 있을 때만 모두
	 * (−1, −1)로 비운다. 쓴 채널 번호를 돌려준다(없으면 INDEX_NONE).
	 */
	int32 WriteCarrierUVChannel(FMeshDescription& MD, int32 ExistingChannel, int32 TexelChannel, int32 MaxChannels,
		const FMeshUV0Charts* UV0, const FCarrierAtlas& Atlas)
	{
		FStaticMeshAttributes Attributes(MD);
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		int32 Channel = ExistingChannel > 0 && ExistingChannel != TexelChannel && ExistingChannel < UVs.GetNumChannels()
			? ExistingChannel : INDEX_NONE;
		if (Channel == INDEX_NONE && UVs.GetNumChannels() > 1)
		{
			// 데이터가 채널 번호를 잃었어도(베이크 데이터를 되돌리거나 새로 만듦) 전에 구운 캐리어 채널이 메시에 남아 있으면 그
			// 자리에 다시 쓴다 — 채널이 쌓이지 않게. 캐리어 채널은 텍셀 표를 읽는 인스턴스가 정확히 (−1, −1)이고 나머지는
			// 0~1이라, 마지막 채널이 그 모양이면 캐리어 채널로 본다.
			const int32 Last = UVs.GetNumChannels() - 1;
			bool bCarrierLike = Last != TexelChannel;
			// 굽기는 텍셀 채널 바로 뒤에 캐리어 채널을 더한다. 그 자리가 마지막 채널이면 우리 것이다 — 모든 슬롯이 눌리는 메시는
			// (−1, −1)인 인스턴스가 없어 아래 모양 검사만으로는 알아보지 못해, 새 베이크 데이터로 구우면 채널이 하나 더 쌓였다.
			bool bAnyUnset = TexelChannel != INDEX_NONE && Last == TexelChannel + 1;
			for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
			{
				const FVector2f Value = UVs.Get(Instance, Last);
				const bool bUnset = Value.X == -1.f && Value.Y == -1.f;
				bAnyUnset |= bUnset;
				if (!bUnset && (Value.X < 0.f || Value.Y < 0.f || Value.X > 1.f || Value.Y > 1.f))
				{
					bCarrierLike = false;
					break;
				}
			}
			if (bCarrierLike && bAnyUnset)
			{
				Channel = Last;
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Reusing contact map UV channel %d from the previous bake"), Channel);
			}
		}
		if (!UV0 && Channel == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		if (Channel == INDEX_NONE)
		{
			Channel = UVs.GetNumChannels();
			if (Channel >= MaxChannels)
			{
				UE_LOG(LogSoftbodyCarrier, Warning,
					TEXT("The mesh already has %d UV channels, so the contact map UV channel could not be added (limit %d; Nanite and skeletal meshes read UV0 to UV3). Dents on triangles that cross a UV island border will be less detailed. Remove an unused UV channel and click Rebake Contact Map."),
					Channel, MaxChannels);
				return INDEX_NONE;
			}
			UVs.SetNumChannels(Channel + 1);
		}
		for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
		{
			UVs.Set(Instance, Channel, FVector2f(-1.f, -1.f));
		}
		if (UV0)
		{
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
			// 인스턴스 → 처음 쓴 캐리어 UV 요소, (인스턴스, 다른 요소) → 나눈 인스턴스.
			TMap<FVertexInstanceID, int32> Written;
			TMap<TPair<int32, int32>, FVertexInstanceID> Copies;
			int32 SplitInstances = 0;
			int32 Unsplit = 0;
			for (int32 Tri = 0; Tri < UV0->MeshTriangle.Num(); ++Tri)
			{
				const FTriangleID Triangle = UV0->MeshTriangle[Tri];
				const TArrayView<const FVertexInstanceID> View = MD.GetTriangleVertexInstances(Triangle);
				const FVertexInstanceID Corners[3] = {View[0], View[1], View[2]};
				for (int32 c = 0; c < 3; ++c)
				{
					const int32 Element = Atlas.UVIndices[Tri * 3 + c];
					const FVector2f Value(Atlas.UVs[Element]);
					const int32* First = Written.Find(Corners[c]);
					if (!First)
					{
						Written.Add(Corners[c], Element);
						UVs.Set(Corners[c], Channel, Value);
						continue;
					}
					if (FVector2f(Atlas.UVs[*First]) == Value)
					{
						continue;
					}
					const FPolygonID Polygon = MD.GetTrianglePolygon(Triangle);
					if (MD.GetPolygonTriangles(Polygon).Num() != 1)
					{
						++Unsplit;
						continue;
					}
					const TPair<int32, int32> Key(Corners[c].GetValue(), Element);
					FVertexInstanceID Copy;
					if (const FVertexInstanceID* Found = Copies.Find(Key))
					{
						Copy = *Found;
					}
					else
					{
						Copy = MD.CreateVertexInstance(MD.GetVertexInstanceVertex(Corners[c]));
						if (Normals.IsValid())
						{
							Normals.Set(Copy, Normals.Get(Corners[c]));
						}
						if (Tangents.IsValid())
						{
							Tangents.Set(Copy, Tangents.Get(Corners[c]));
						}
						if (BinormalSigns.IsValid())
						{
							BinormalSigns.Set(Copy, BinormalSigns.Get(Corners[c]));
						}
						if (Colors.IsValid())
						{
							Colors.Set(Copy, Colors.Get(Corners[c]));
						}
						for (int32 UVChannel = 0; UVChannel < UVs.GetNumChannels(); ++UVChannel)
						{
							UVs.Set(Copy, UVChannel, UVs.Get(Corners[c], UVChannel));
						}
						UVs.Set(Copy, Channel, Value);
						Copies.Add(Key, Copy);
						++SplitInstances;
					}
					MD.SetPolygonVertexInstance(Polygon, c, Copy);
				}
			}
			if (SplitInstances > 0 || Unsplit > 0)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Contact map UV channel: split %d vertex instances shared across a seam%s"), SplitInstances,
					Unsplit > 0 ? *FString::Printf(TEXT(", %d corners on multi-triangle polygons could not be split (those triangles use the UVs from one side of the seam)"), Unsplit)
						: TEXT(""));
			}
		}
		return Channel;
	}

	int32 RenderVertexCount(USkeletalMesh* Skeletal, UStaticMesh* Static)
	{
		if (Skeletal)
		{
			const FSkeletalMeshRenderData* RenderData = Skeletal->GetResourceForRendering();
			return RenderData && RenderData->LODRenderData.Num() > 0 ? (int32)RenderData->LODRenderData[0].GetNumVertices() : 0;
		}
		const FStaticMeshRenderData* RenderData = Static ? Static->GetRenderData() : nullptr;
		return RenderData && RenderData->LODResources.Num() > 0 ? RenderData->LODResources[0].GetNumVertices() : 0;
	}

	/**
	 * 메시 설명을 커밋하고 렌더 데이터를 다시 만든다. 더 넣은 이음매(ADR-0007 §3.8)를 따라 렌더 정점이 나뉘어 수가 바뀔 수 있다 —
	 * 정점별 베이크 데이터(텍셀 표·페인트·무름)는 메시 정점마다 하나이고 렌더 정점은 텍셀 UV 채널로 그 칸을 읽으므로(나뉜 정점도
	 * 같은 텍셀 UV) 어긋나지 않는다. 렌더 데이터를 못 만들면 false.
	 */
	bool CommitSourceMesh(USkeletalMesh* Skeletal, UStaticMesh* Static, int32 NumChannels, TConstArrayView<int32> OurChannels, int32 MaxChannels)
	{
		const int32 Before = RenderVertexCount(Skeletal, Static);
		if (Skeletal)
		{
			Skeletal->Modify();
			Skeletal->CommitMeshDescription(0);
			Skeletal->PostEditChange();
			FSkinnedAssetCompilingManager::Get().FinishAllCompilation();
			Skeletal->MarkPackageDirty();
		}
		else
		{
			Static->Modify();
			SoftbodyBakeShared::KeepLightmapUVsFree(Static, NumChannels, OurChannels, MaxChannels);
			Static->CommitMeshDescription(0);
			Static->Build(false);
			Static->PostEditChange();
			FStaticMeshCompilingManager::Get().FinishAllCompilation();
			Static->MarkPackageDirty();
		}
		const int32 After = RenderVertexCount(Skeletal, Static);
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Mesh rebuilt with the contact map UV channel: render vertices %d -> %d"), Before, After);
		return After > 0;
	}

	/**
	 * 같은 베이스 머티리얼을 쓰는 다른 소프트바디 메시 가운데 캐리어 UV 채널이 Channel과 다른 것의 이름들. TexCoord 번호는 머티리얼
	 * 그래프에 박히므로(인스턴스로 바꿀 수 없다) 두 메시의 채널이 다르면 한 베이스 머티리얼을 같이 쓸 수 없다.
	 */
	TArray<FString> OtherCarrierUsers(const UMaterial* Base, const USoftbodyMeshData* Self, int32 Channel)
	{
		TArray<FString> Out;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(USoftbodyMeshData::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Asset : Assets)
		{
			const USoftbodyMeshData* Other = Cast<USoftbodyMeshData>(Asset.GetAsset());
			if (!Other || Other == Self || Other->CarrierUVChannel == INDEX_NONE || Other->CarrierUVChannel == Channel)
			{
				continue;
			}
			TArray<UMaterialInterface*> Materials;
			if (Other->SourceSkeletalMesh)
			{
				for (const FSkeletalMaterial& Material : Other->SourceSkeletalMesh->GetMaterials())
				{
					Materials.Add(Material.MaterialInterface);
				}
			}
			else if (Other->SourceMesh)
			{
				for (const FStaticMaterial& Material : Other->SourceMesh->GetStaticMaterials())
				{
					Materials.Add(Material.MaterialInterface);
				}
			}
			if (Materials.ContainsByPredicate([Base](const UMaterialInterface* Material) { return Material && Material->GetMaterial() == Base; }))
			{
				Out.Add(FString::Printf(TEXT("%s (UV channel %d)"), *Other->GetName(), Other->CarrierUVChannel));
			}
		}
		return Out;
	}

	/** 함수 호출 입력에 물린 TexCoord의 채널(리루트는 따라간다). 물린 것이 없으면 INDEX_NONE, TexCoord가 아니면 −2. */
	int32 WiredCoordinateIndex(const FExpressionInput& Input)
	{
		const UMaterialExpression* Expression = Input.Expression;
		for (int32 Hop = 0; Hop < 16 && Expression; ++Hop)
		{
			if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(Expression))
			{
				Expression = Reroute->Input.Expression;
				continue;
			}
			const UMaterialExpressionTextureCoordinate* Coord = Cast<UMaterialExpressionTextureCoordinate>(Expression);
			return Coord ? Coord->CoordinateIndex : -2;
		}
		return INDEX_NONE;
	}

	/**
	 * 호스트 머티리얼(캐리어를 읽는 슬롯의 베이스 머티리얼)에서 MF_SoftbodyContactNormal·MF_SoftbodyDebugVis 호출의
	 * CarrierUV 입력에 TexCoord[Channel]을 잇는다(ADR-0007 §3.3). Channel이 INDEX_NONE이면 끊는다(텍셀 표로 돌아간다).
	 * 다른 소프트바디 메시가 같은 베이스 머티리얼을 다른 채널로 쓰고 있으면 그 머티리얼은 건드리지 않고 false — 전에는 말없이
	 * 다시 이어, 먼저 구운 메시의 눌림이 깨졌다.
	 */
	bool WireCarrierUV(const TArray<UMaterialInterface*>& Interfaces, int32 Channel, const USoftbodyMeshData* Self)
	{
		const FName FunctionNames[] = {TEXT("MF_SoftbodyContactNormal"), TEXT("MF_SoftbodyDebugVis")};
		const FName InputName(TEXT("CarrierUV"));
		TSet<UMaterial*> Done;
		bool bAllWired = true;
		for (UMaterialInterface* Interface : Interfaces)
		{
			UMaterial* Base = Interface && SoftbodyBakeShared::ReadsCarrier(Interface) ? Interface->GetMaterial() : nullptr;
			if (!Base || Done.Contains(Base))
			{
				continue;
			}
			Done.Add(Base);
			// 이미 다른 채널로 이어져 있으면 그 채널을 쓰는 다른 메시가 있는지 본다(이 메시의 옛 배선이면 그대로 다시 잇는다).
			bool bWiredElsewhere = false;
			for (UMaterialExpression* Expression : Base->GetExpressions())
			{
				const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
				if (!Call || !Call->MaterialFunction
					|| (Call->MaterialFunction->GetFName() != FunctionNames[0] && Call->MaterialFunction->GetFName() != FunctionNames[1]))
				{
					continue;
				}
				for (const FFunctionExpressionInput& In : Call->FunctionInputs)
				{
					if (In.ExpressionInput && In.ExpressionInput->InputName == InputName)
					{
						const int32 Wired = WiredCoordinateIndex(In.Input);
						bWiredElsewhere |= Wired != INDEX_NONE && Wired != Channel;
					}
				}
			}
			if (bWiredElsewhere)
			{
				const TArray<FString> Others = OtherCarrierUsers(Base, Self, Channel);
				if (Others.Num() > 0)
				{
					UE_LOG(LogSoftbodyCarrier, Error,
						TEXT("Material %s is shared with other Softbody meshes that read a different UV channel: %s. This mesh needs channel %d, "
							"and a UV channel number cannot differ between material instances. The material was left unchanged. Give this mesh "
							"its own material: duplicate the master material and use the copy on this mesh's slots (or turn on Create Softbody "
							"Material), then press Run Bake again."),
						*Base->GetName(), *FString::Join(Others, TEXT(", ")), Channel);
					bAllWired = false;
					continue;
				}
			}
			Base->Modify();
			UMaterialExpressionTextureCoordinate* TexCoord = nullptr;
			int32 Wired = 0;
			int32 Missing = 0;
			for (UMaterialExpression* Expression : Base->GetExpressions())
			{
				UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
				if (!Call || !Call->MaterialFunction
					|| (Call->MaterialFunction->GetFName() != FunctionNames[0] && Call->MaterialFunction->GetFName() != FunctionNames[1]))
				{
					continue;
				}
				FFunctionExpressionInput* Input = Call->FunctionInputs.FindByPredicate([&InputName](const FFunctionExpressionInput& In)
				{
					return In.ExpressionInput && In.ExpressionInput->InputName == InputName;
				});
				if (!Input)
				{
					++Missing;
					continue;
				}
				if (Channel == INDEX_NONE)
				{
					Input->Input.Expression = nullptr;
					continue;
				}
				if (!TexCoord)
				{
					for (UMaterialExpression* Other : Base->GetExpressions())
					{
						UMaterialExpressionTextureCoordinate* Candidate = Cast<UMaterialExpressionTextureCoordinate>(Other);
						if (Candidate && Candidate->CoordinateIndex == Channel && Candidate->UTiling == 1.f && Candidate->VTiling == 1.f
							&& !Candidate->UnMirrorU && !Candidate->UnMirrorV)
						{
							TexCoord = Candidate;
							break;
						}
					}
				}
				if (!TexCoord)
				{
					TexCoord = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Base,
						UMaterialExpressionTextureCoordinate::StaticClass(), Call->MaterialExpressionEditorX - 300,
						Call->MaterialExpressionEditorY + 120));
					if (TexCoord)
					{
						TexCoord->CoordinateIndex = Channel;
						TexCoord->Desc = TEXT("Contact map UV channel");
					}
				}
				if (TexCoord && UMaterialEditingLibrary::ConnectMaterialExpressions(TexCoord, FString(), Call, InputName.ToString()))
				{
					++Wired;
				}
			}
			if (Missing > 0)
			{
				UE_LOG(LogSoftbodyCarrier, Warning,
					TEXT("%s: %d softbody material function calls have no CarrierUV input, so the plugin content (MF_Softbody*) is older than the plugin code. Reinstall the plugin."),
					*Base->GetName(), Missing);
			}
			UMaterialEditingLibrary::RecompileMaterial(Base);
			Base->MarkPackageDirty();
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Material %s: CarrierUV %s"), *Base->GetName(),
				Channel == INDEX_NONE ? TEXT("disconnected") : *FString::Printf(TEXT("<- TexCoord[%d] (%d places)"), Channel, Wired));
		}
		return bAllWired;
	}
}

namespace
{
	/** 베이크 텍스처 원본(RGBA32F, 앞쪽 NumTexels칸)을 읽는다. */
	bool ReadTexture32F(UTexture2D* Texture, int32 NumTexels, TArray<FLinearColor>& Out)
	{
		TArray64<uint8> Raw;
		if (!Texture || !Texture->Source.IsValid() || !Texture->Source.GetMipData(Raw, 0)
			|| Raw.Num() < (int64)NumTexels * (int64)sizeof(FLinearColor))
		{
			return false;
		}
		Out.SetNumUninitialized(NumTexels);
		FMemory::Memcpy(Out.GetData(), Raw.GetData(), (int64)NumTexels * (int64)sizeof(FLinearColor));
		return true;
	}

	/** 베이크 텍스처 원본(RGBA16F, 앞쪽 NumTexels칸)을 읽는다. */
	bool ReadTexture16F(UTexture2D* Texture, int32 NumTexels, TArray<FFloat16Color>& Out)
	{
		TArray64<uint8> Raw;
		if (!Texture || !Texture->Source.IsValid() || !Texture->Source.GetMipData(Raw, 0)
			|| Raw.Num() < (int64)NumTexels * (int64)sizeof(FFloat16Color))
		{
			return false;
		}
		Out.SetNumUninitialized(NumTexels);
		FMemory::Memcpy(Out.GetData(), Raw.GetData(), (int64)NumTexels * (int64)sizeof(FFloat16Color));
		return true;
	}
}

// 다른 베이크 파일(솟음 랜드마크 거리표, SoftbodyBulgeLandmarkBake.cpp)이 같은 읽기를 쓴다.
bool SoftbodyBakeShared::ReadTextureSource32F(UTexture2D* Texture, int32 NumTexels, TArray<FLinearColor>& Out)
{
	return ReadTexture32F(Texture, NumTexels, Out);
}

bool SoftbodyBakeShared::ReadTextureSource16F(UTexture2D* Texture, int32 NumTexels, TArray<FFloat16Color>& Out)
{
	return ReadTexture16F(Texture, NumTexels, Out);
}

namespace
{

	/**
	 * 덩어리 옆 몫의 두 축(ADR-0012 §3.5): 칸의 미는 방향을 런타임이 읽는 16비트 값으로 반올림해 단위화하고, 거기 수직인 두 축을
	 * 셰이더 SB_LumpBasis와 같은 식(Duff 외 2017 — z의 부호 비트로만 갈린다)으로 만든다. 굽는 쪽과 읽는 쪽이 같은 비트에서 같은
	 * 축을 얻어 노멀 몫 + 옆 몫이 무름 전체로 되돌아간다.
	 */
	void LumpBasis(const FVector3d& Push, FVector3d& OutN, FVector3d& OutT1, FVector3d& OutT2)
	{
		const FFloat16Color Stored(FLinearColor((float)Push.X, (float)Push.Y, (float)Push.Z, 0.f));
		const FVector3f N = FVector3f(Stored.R.GetFloat(), Stored.G.GetFloat(), Stored.B.GetFloat()).GetSafeNormal();
		const float Sign = std::signbit(N.Z) ? -1.f : 1.f;
		const float A = -1.f / (Sign + N.Z);
		const float B = N.X * N.Y * A;
		OutN = FVector3d(N);
		OutT1 = FVector3d(1.f + Sign * N.X * N.X * A, Sign * B, -Sign * N.X);
		OutT2 = FVector3d(B, Sign + N.Y * N.Y * A, -N.Y);
	}

	/** 렌더 정점마다 덩어리 묶음(없으면 INDEX_NONE)과 그 몫 — 관성 그룹 채널 Attr2에서 BuildLumpTexels와 같은 규칙으로. 못 읽으면 false. */
	bool ReadVertexLumps(USoftbodyMeshData& Data, int32 NumVertices, TArray<int32>& OutLump, TArray<float>& OutWeight)
	{
		const int32 TableTexels = Data.TextureSize * Data.TextureSize;
		TArray<FFloat16Color> Groups;
		if (NumVertices > TableTexels || !ReadTexture16F(Data.AttributeTex2, TableTexels, Groups))
		{
			return false;
		}
		const int32 NumGroups = Data.InertiaGroupLump.Num();
		const auto IsLump = [&Data, NumGroups](int32 Group)
		{
			return Group >= 0 && Group < NumGroups && Data.InertiaGroupLump[Group];
		};
		OutLump.Init(INDEX_NONE, NumVertices);
		OutWeight.Init(0.f, NumVertices);
		for (int32 v = 0; v < NumVertices; ++v)
		{
			OutLump[v] = SoftbodyBakeShared::FVertexGroups::Decode(Groups[v].GetFloats()).PickLump(IsLump, OutWeight[v]);
		}
		return true;
	}

	/** 덩어리 칸 값(BuildLumpTexels)을 만든 정점·칸 값. 국소 모양(BuildLumpRegions)이 실행 중 덩어리와 같은 모양으로 마무리하게 넘긴다. */
	struct FLumpTexelSource
	{
		/** 렌더 정점마다 덩어리 묶음(없으면 INDEX_NONE)과 그 몫. */
		TArray<int32> VertexLump;
		TArray<float> VertexLumpWeight;
		/** 렌더 정점마다 칠 × 무름 세 방향 [3·v + k](가장 무른 칠한 살 = 1). */
		TArray<FVector3f> VertexShape;
		/** 칸마다 덩어리 묶음(덩어리가 아니면 INDEX_NONE). */
		TArray<int32> TexelLump;
	};

	/**
	 * 덩어리 값(ADR-0012). 렌더 정점마다 덩어리 묶음·무게(관성 그룹 채널 Attr2의 그룹 셋 중 덩어리 묶음 몫)와
	 * 칠(B) × 무름 세 방향을 정하고, 캐리어 칸마다 그 칸 자리 렌더 삼각형의 세 정점 값을 무게중심 좌표로 섞는다(TexelCorners·
	 * TexelBary — 렌더러가 정점 값을 삼각형 안에서 보간하는 것과 같다). 실행 중 덩어리 노멀(SB_LumpNormal)이 이 칸 값을 픽셀마다
	 * 미분하므로, 칸 값이 삼각형 안에서 매끄러워야 정점 위치가 그리는 모양과 음영이 맞는다. 전에 쓰던 "가까운 정점 4개의 역거리
	 * 가중"은 고른 넷이 바뀌는 선마다 값이 꺾여 정점 간격의 사선 줄무늬 음영을 그렸다(일지 208).
	 * 칸 값 = (노멀·칠×무름_x, _y, _z, 묶음 + 0.99 × 무게), 옆 몫 A·B = 미는 방향에 수직인 두 축으로 잰 칠×무름(§3.5,
	 * USoftbodyMeshData::CarrierLumpSideATex). 표면 칸마다 살 기둥(칸 넓이 × 기둥 깊이 × 무게)으로 덩어리의
	 * 부피 V와 평균 무름 C를 더한다 — 기둥 안쪽은 뼈대 쪽으로 0까지 줄어드는 모양으로 보고 겉면 값의 절반을 쓴다.
	 * 기둥 깊이(TexelColumnDepth)는 코어까지 거리다. 손이 덩어리를 밀 때 뿌리의 단단함은 V와 C의 곱이 정하고(ADR-0012 §3.2),
	 * 두 어림의 오차가 그 곱에서 지워진다. 코어까지 기둥은 척추 쪽에서 서로 겹쳐 V를 크게 세고, 절반 가정은 C를 작게 센다
	 * (갈비뼈 없는 관성 굽기에서는 처짐이 깊이로 거의 안 줄어 실제 몫이 0.7~0.9다). 같은 몸의 FEM과 견준 뿌리는 가슴 앞·위와
	 * 엉덩이에서 0.8~1.1배다(일지 248). 그래서 V와 C는 같이만 바꾼다 — 247은 V만 받침 위 둔덕으로 재서(시험 캐릭터 가슴 958 → 337)
	 * 뿌리가 FEM보다 2.5~3.9배 물렀고, 덩어리가 손을 훨씬 더 따라갔다.
	 * 눌리는 정도 칠을 곱하기 전 값이다: 덩어리의 살 부피는 흔들리는 살의 양이라, 눌림을 줄이려고 얇게 칠해도 덩어리의 무게는 그대로다.
	 * TexelMoundDepth(받침 위 높이)를 주면 둔덕 부피를 로그에 같이 적는다(값에는 쓰지 않는다).
	 * 스태틱 메시는 메시 전체가 칸 값의 묶음 0(무게 1)이지만 덩어리 값(OutLumps)은 만들지 않는다 — 누르면 통째로 비켜나는 살은 스켈레탈의
	 * Lump 본뿐이다(일지 296). 칸 값은 그대로 만든다: 접촉 패스가 출렁여 옮겨 간 자리에서 콜라이더를 재고 머티리얼이 그
	 * 음영을 맞춘다(텍스처째 없애면 출렁이는 동안 틈·겹침이 난다). 칸 값을 만들지 못했으면(스켈레탈은 덩어리가 없을 때도) false.
	 * OutSource: 주면 정점·칸 값을 넘긴다(국소 모양 BuildLumpRegions).
	 */
	bool BuildLumpTexels(USoftbodyMeshData& Data, int32 NumVertices, const TArray<uint8>& Kind,
		const TArray<FIntVector>& TexelCorners, const TArray<FVector3d>& TexelBary,
		const TArray<FVector3d>& TexelNormal, const TArray<double>& TexelColumnDepth, const TArray<double>* TexelMoundDepth,
		double TexelSize, TArray<FFloat16Color>& OutTexels, TArray<FFloat16Color>& OutSideA, TArray<FFloat16Color>& OutSideB,
		TArray<FSoftbodyInertiaLump>& OutLumps, FLumpTexelSource* OutSource = nullptr)
	{
		OutTexels.Reset();
		OutSideA.Reset();
		OutSideB.Reset();
		OutLumps.Reset();
		const int32 TableTexels = Data.TextureSize * Data.TextureSize;
		const bool bSkeletal = Data.SourceSkeletalMesh != nullptr;
		if (Data.NumInertiaResponses != 3 || Data.VertexInertiaResponseShapes.Num() != 3 * NumVertices
			|| NumVertices > TableTexels)
		{
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Lumps: skipped, the baked data has no flesh softness for inertia"));
			return false;
		}
		if (bSkeletal && !Data.InertiaGroupLump.Contains(true))
		{
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Lumps: skipped, no bone has the Lump role in Bone Overrides"));
			return false;
		}
		TArray<FFloat16Color> Paint;
		const bool bPaint = ReadTexture16F(Data.PaintTex, TableTexels, Paint);
		TArray<FFloat16Color> Groups;
		if (bSkeletal && !ReadTexture16F(Data.AttributeTex2, TableTexels, Groups))
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lumps: could not read the baked inertia groups, so lumps were not baked. Click Run Bake again."));
			return false;
		}

		// 정점마다 덩어리 묶음(없으면 INDEX_NONE)·무게, 칠 × 무름 세 방향.
		const int32 NumGroups = Data.InertiaGroupLump.Num();
		TArray<int32> VertexLump;
		VertexLump.Init(INDEX_NONE, NumVertices);
		TArray<float> VertexLumpWeight;
		VertexLumpWeight.Init(0.f, NumVertices);
		TArray<FVector3f> VertexShape;
		VertexShape.SetNumZeroed(3 * NumVertices);
		for (int32 v = 0; v < NumVertices; ++v)
		{
			const float PaintB = bPaint ? FMath::Max(Paint[v].B.GetFloat(), 0.f) : 1.f;
			for (int32 j = 0; j < 3; ++j)
			{
				VertexShape[3 * v + j] = Data.VertexInertiaResponseShapes[j * NumVertices + v] * PaintB;
			}
			if (!bSkeletal)
			{
				VertexLump[v] = 0;
				VertexLumpWeight[v] = 1.f;
				continue;
			}
			const auto IsLump = [&Data, NumGroups](int32 Group)
			{
				return Group >= 0 && Group < NumGroups && Data.InertiaGroupLump[Group];
			};
			VertexLump[v] = SoftbodyBakeShared::FVertexGroups::Decode(Groups[v].GetFloats()).PickLump(IsLump, VertexLumpWeight[v]);
		}

		const int32 NumTexels = Kind.Num();
		if (TexelCorners.Num() != NumTexels || TexelBary.Num() != NumTexels)
		{
			return false;
		}
		OutTexels.Init(FFloat16Color(FLinearColor(0.f, 0.f, 0.f, 0.f)), NumTexels);
		OutSideA.Init(FFloat16Color(FLinearColor(0.f, 0.f, 0.f, 0.f)), NumTexels);
		OutSideB.Init(FFloat16Color(FLinearColor(0.f, 0.f, 0.f, 0.f)), NumTexels);
		TArray<float> SideRatio; // 덩어리 칸의 무름 중 옆 방향 비율(로그), 덩어리 아니면 −1
		SideRatio.Init(-1.f, NumTexels);
		TArray<int32> TexelLump;
		TexelLump.Init(INDEX_NONE, NumTexels);
		TArray<float> TexelWeight;
		TexelWeight.Init(0.f, NumTexels);
		TArray<FVector3f> TexelShape;
		TexelShape.SetNumZeroed(3 * NumTexels);
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			const FIntVector& Corners = TexelCorners[Texel];
			if (Kind[Texel] == KindEmpty || Corners.X == INDEX_NONE)
			{
				return;
			}
			double WeightSum = 0.0;
			FVector3d Shape[3] = {FVector3d::ZeroVector, FVector3d::ZeroVector, FVector3d::ZeroVector};
			TArray<TPair<int32, double>, TInlineAllocator<3>> LumpWeights;
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 v = Corners[c];
				// 칸 점은 삼각형 안(변 위 포함)이지만 반올림으로 조금 밖일 수 있다 — 밖으로 늘이지 않는다.
				const double W = FMath::Max(TexelBary[Texel][c], 0.0);
				if (v < 0 || v >= NumVertices || W <= 0.0)
				{
					continue;
				}
				WeightSum += W;
				for (int32 j = 0; j < 3; ++j)
				{
					Shape[j] += W * FVector3d(VertexShape[3 * v + j]);
				}
				if (VertexLump[v] == INDEX_NONE)
				{
					continue;
				}
				TPair<int32, double>* Entry = LumpWeights.FindByPredicate(
					[Group = VertexLump[v]](const TPair<int32, double>& E) { return E.Key == Group; });
				if (Entry)
				{
					Entry->Value += W * VertexLumpWeight[v];
				}
				else
				{
					LumpWeights.Emplace(VertexLump[v], W * VertexLumpWeight[v]);
				}
			}
			if (WeightSum <= 0.0)
			{
				return;
			}
			int32 Best = INDEX_NONE;
			double BestWeight = 0.0;
			for (const TPair<int32, double>& Entry : LumpWeights)
			{
				if (Entry.Value > BestWeight)
				{
					Best = Entry.Key;
					BestWeight = Entry.Value;
				}
			}
			// 무름을 미는 방향(노멀 몫)과 거기 수직인 두 축(옆 몫)으로 나눈다. 셋을 합치면 무름 전체로 되돌아간다(§3.5).
			FVector3d N;
			FVector3d T1;
			FVector3d T2;
			LumpBasis(TexelNormal[Texel], N, T1, T2);
			FLinearColor Value(0.f, 0.f, 0.f, 0.f);
			FLinearColor SideA(0.f, 0.f, 0.f, 0.f);
			FLinearColor SideB(0.f, 0.f, 0.f, 0.f);
			double Total = 0.0;
			double Side = 0.0;
			for (int32 j = 0; j < 3; ++j)
			{
				Shape[j] /= WeightSum;
				TexelShape[3 * Texel + j] = FVector3f(Shape[j]);
				(&Value.R)[j] = (float)N.Dot(Shape[j]);
				(&SideA.R)[j] = (float)T1.Dot(Shape[j]);
				Total += Shape[j].SquaredLength();
				Side += FMath::Square(T1.Dot(Shape[j])) + FMath::Square(T2.Dot(Shape[j]));
			}
			SideA.A = (float)T2.Dot(Shape[0]);
			SideB.R = (float)T2.Dot(Shape[1]);
			SideB.G = (float)T2.Dot(Shape[2]);
			const float Weight = (float)FMath::Clamp(BestWeight / WeightSum, 0.0, 1.0);
			if (Best != INDEX_NONE && Weight > 1e-3f)
			{
				Value.A = (float)Best + 0.99f * Weight;
				TexelLump[Texel] = Best;
				TexelWeight[Texel] = Weight;
				if (Kind[Texel] == KindSurface && Total > 1e-12)
				{
					SideRatio[Texel] = (float)FMath::Sqrt(Side / Total);
				}
			}
			OutTexels[Texel] = FFloat16Color(Value);
			OutSideA[Texel] = FFloat16Color(SideA);
			OutSideB[Texel] = FFloat16Color(SideB);
		});

		{
			// 전에 파임 보정이 버리던 몫(§3.5): 덩어리 칸의 무름 중 옆 방향 비율.
			TArray<float> Ratios;
			for (const float Ratio : SideRatio)
			{
				if (Ratio >= 0.f)
				{
					Ratios.Add(Ratio);
				}
			}
			Ratios.Sort();
			if (Ratios.Num() > 0)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Lump sideways share: lump cells %d, sideways part of the softness median %.0f%% / 90%% %.0f%%"),
					Ratios.Num(), 100.f * Ratios[Ratios.Num() / 2], 100.f * Ratios[Ratios.Num() * 9 / 10]);
			}
		}

		// 덩어리마다 부피·평균 무름(표면 칸만 — 고스트 칸은 옆 차트 칸의 복사다).
		struct FLumpSum
		{
			double Volume = 0.0;
			double MoundVolume = 0.0;
			FVector3d Shape[3] = {FVector3d::ZeroVector, FVector3d::ZeroVector, FVector3d::ZeroVector};
		};
		TMap<int32, FLumpSum> Sums;
		const double TexelArea = TexelSize * TexelSize;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Kind[Texel] != KindSurface || TexelLump[Texel] == INDEX_NONE)
			{
				continue;
			}
			FLumpSum& Sum = Sums.FindOrAdd(TexelLump[Texel]);
			const double Column = TexelArea * FMath::Max(TexelColumnDepth[Texel], 0.0) * TexelWeight[Texel];
			Sum.Volume += Column;
			if (TexelMoundDepth)
			{
				Sum.MoundVolume += TexelArea * FMath::Max((*TexelMoundDepth)[Texel], 0.0) * TexelWeight[Texel];
			}
			for (int32 j = 0; j < 3; ++j)
			{
				Sum.Shape[j] += 0.5 * Column * FVector3d(TexelShape[3 * Texel + j]);
			}
		}
		TArray<int32> LumpGroups;
		Sums.GetKeys(LumpGroups);
		LumpGroups.Sort();
		bool bAnyColumn = false;
		for (const int32 Group : LumpGroups)
		{
			const FLumpSum& Sum = Sums[Group];
			if (Sum.Volume <= 0.0)
			{
				continue;
			}
			bAnyColumn = true;
			if (!bSkeletal)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Jiggle cells (whole mesh): volume %.0f. A static mesh has no lump, so a press never moves it aside as a whole"),
					Sum.Volume);
				continue;
			}
			FSoftbodyInertiaLump& Lump = OutLumps.AddDefaulted_GetRef();
			Lump.Group = Group;
			Lump.Volume = (float)Sum.Volume;
			Lump.MeanShapeX = FVector3f(Sum.Shape[0] / Sum.Volume);
			Lump.MeanShapeY = FVector3f(Sum.Shape[1] / Sum.Volume);
			Lump.MeanShapeZ = FVector3f(Sum.Shape[2] / Sum.Volume);
			const FName Bone = Data.InertiaGroupBones.IsValidIndex(Group) ? Data.InertiaGroupBones[Group] : NAME_None;
			const FString Measured = TexelMoundDepth ? FString::Printf(TEXT(" (mound above its support: %.0f)"), Sum.MoundVolume) : FString();
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Lump %d (%s): volume %.0f%s, mean softness X(%.3f %.3f %.3f) Y(%.3f %.3f %.3f) Z(%.3f %.3f %.3f)"),
				Group, *Bone.ToString(), Lump.Volume, *Measured,
				Lump.MeanShapeX.X, Lump.MeanShapeX.Y, Lump.MeanShapeX.Z, Lump.MeanShapeY.X, Lump.MeanShapeY.Y,
				Lump.MeanShapeY.Z, Lump.MeanShapeZ.X, Lump.MeanShapeZ.Y, Lump.MeanShapeZ.Z);
		}
		if (OutSource)
		{
			OutSource->VertexLump = MoveTemp(VertexLump);
			OutSource->VertexLumpWeight = MoveTemp(VertexLumpWeight);
			OutSource->VertexShape = MoveTemp(VertexShape);
			OutSource->TexelLump = MoveTemp(TexelLump);
		}
		return bSkeletal ? OutLumps.Num() > 0 : bAnyColumn;
	}

	/**
	 * 대칭 행렬 A(N×N, 행 우선)의 고유값과 고유벡터(OutVectors[i × N + m] = 고유벡터 m의 i 성분) — 순환 야코비 회전. A는 대각이 된다.
	 * 국소 모양의 16×16 유연도와 덩어리 평균 무름 3×3에 쓴다.
	 */
	void LumpRegionJacobi(TArray<double>& A, int32 N, TArray<double>& OutValues, TArray<double>& OutVectors)
	{
		OutVectors.Init(0.0, N * N);
		for (int32 i = 0; i < N; ++i)
		{
			OutVectors[i * N + i] = 1.0;
		}
		for (int32 Sweep = 0; Sweep < 64; ++Sweep)
		{
			double Off = 0.0;
			double Total = 0.0;
			for (int32 p = 0; p < N; ++p)
			{
				for (int32 q = 0; q < N; ++q)
				{
					const double Square = A[p * N + q] * A[p * N + q];
					Total += Square;
					Off += p != q ? Square : 0.0;
				}
			}
			if (Off <= 1e-28 * Total)
			{
				break;
			}
			for (int32 p = 0; p < N - 1; ++p)
			{
				for (int32 q = p + 1; q < N; ++q)
				{
					const double Apq = A[p * N + q];
					if (FMath::Abs(Apq) <= 1e-300)
					{
						continue;
					}
					// A ← Jᵀ·A·J, J = (p, q) 평면의 회전. A[p][q]가 0이 되는 각.
					const double Theta = (A[q * N + q] - A[p * N + p]) / (2.0 * Apq);
					const double T = (Theta >= 0.0 ? 1.0 : -1.0) / (FMath::Abs(Theta) + FMath::Sqrt(Theta * Theta + 1.0));
					const double C = 1.0 / FMath::Sqrt(T * T + 1.0);
					const double S = T * C;
					for (int32 k = 0; k < N; ++k)
					{
						const double Akp = A[k * N + p];
						const double Akq = A[k * N + q];
						A[k * N + p] = C * Akp - S * Akq;
						A[k * N + q] = S * Akp + C * Akq;
					}
					for (int32 k = 0; k < N; ++k)
					{
						const double Apk = A[p * N + k];
						const double Aqk = A[q * N + k];
						A[p * N + k] = C * Apk - S * Aqk;
						A[q * N + k] = S * Apk + C * Aqk;
					}
					for (int32 k = 0; k < N; ++k)
					{
						const double Vkp = OutVectors[k * N + p];
						const double Vkq = OutVectors[k * N + q];
						OutVectors[k * N + p] = C * Vkp - S * Vkq;
						OutVectors[k * N + q] = S * Vkp + C * Vkq;
					}
				}
			}
		}
		OutValues.SetNumUninitialized(N);
		for (int32 i = 0; i < N; ++i)
		{
			OutValues[i] = A[i * N + i];
		}
	}

	/**
	 * 덩어리 국소 모양(SoftbodyLumpRegion, 일지 251). 덩어리가 출렁임 모양 셋으로만 비켜나면, 그 모양에서 조금만 움직이는 밑동을 눌렀을 때
	 * 덩어리 전체가 지렛대처럼 흔들린다. 측정 굽기가 덩어리마다 푼 원판 누름 j의 FEM 응답 u_j(Data.LumpDiscBakes)에서 실행 중 덩어리
	 * 모양이 그 누름에서 이미 그리는 몫을 빼고, 남은 몫을 고유 모양으로 나눈다(정적 보정 모양 — Harmon & Zorin 2013의 잔여 유연도):
	 *   덩어리 모양 ĝ_k = 덩어리 몫 × 칠 × 무름 k(칸 값과 같은 정점 값), 그 단단함 K = (V/σ)·C(V = 덩어리 부피, C = 평균 무름,
	 *   σ = 가장 무른 칠한 살의 원시 무름 — 실행 중 ResolveLumpConstants와 같다)
	 *   Q_j = 누름 j의 힘이 ĝ에 하는 일, R_j = u_j − ĝ·K⁻¹·Q_j(덩어리 모양이 그리지 못한 몫), g_jl = 누름 j의 힘 · R_l(대칭으로 맞춘다)
	 *   g = Σ λ_m·v_m·v_mᵀ. 가장 큰 λ의 1/1000보다 큰 모양만 남긴다: Φ_m = Σ_j R_j·v_jm.
	 * 모양은 덩어리 몫 0.3 이상 정점에서 가장 큰 크기 c_m이 1이 되게 나누고, 단단함 = (σ/V)·λ_m / c_m²(Lumps의 RegionStiffness — 실행 중
	 * 덩어리 평형의 평균 무름 C와 같은 단위). 원판 누름마다 이 모델은 FEM 응답을 그대로 낸다(모양을 다 남기면).
	 * 모양은 칸마다 덩어리 칸 값과 같은 렌더 삼각형 보간으로 옮기고, 솟음 거친 칸(Block × Block 칸)마다 가장 많이 든 덩어리의 표면 칸으로
	 * 평균해 줄 하나로 적는다. 반환 = 줄 수(0 = 없음). OutIndex = 거친 칸마다 줄 번호 + 1(0 = 없음), OutRecords = 줄 텍셀(폭
	 * RecordTexels × RecordsPerRow, 줄 r의 텍셀 k = 13·r + k — USoftbodyMeshData::CarrierLumpRegionTex).
	 * 모양을 만들지 않는 덩어리: 원판 응답끼리 서로를 그려 내지 못하는 것(MaxBetweenSampleMiss), 국소 몫이 작은 것(MinLocalPart).
	 */
	int32 BuildLumpRegions(const USoftbodyMeshData& Data, const FLumpTexelSource& Source, const TArray<uint8>& Kind,
		const TArray<FIntVector>& TexelCorners, const TArray<FVector3d>& TexelBary,
		int32 Resolution, int32 Block, TArray<FSoftbodyInertiaLump>& Lumps, TArray<uint16>& OutIndex, TArray<FFloat16Color>& OutRecords)
	{
		OutIndex.Reset();
		OutRecords.Reset();
		for (FSoftbodyInertiaLump& Lump : Lumps)
		{
			Lump.RegionStiffness.Reset();
		}
		const int32 NumVertices = Source.VertexLump.Num();
		const int32 NumTexels = Kind.Num();
		if (Lumps.Num() == 0 || NumVertices == 0 || Source.VertexLumpWeight.Num() != NumVertices || Source.VertexShape.Num() != 3 * NumVertices
			|| Source.TexelLump.Num() != NumTexels || TexelCorners.Num() != NumTexels || TexelBary.Num() != NumTexels
			|| Resolution * Resolution != NumTexels || Block <= 0 || Resolution % Block != 0)
		{
			return 0;
		}
		if (Data.LumpDiscBakes.Num() == 0)
		{
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Lump local shapes: none, the measured flesh has no disc presses (baked before they were added). Press Run Bake: then a lump pressed near its base gives way around the press instead of swinging as a whole."));
			return 0;
		}
		double Sigma = 0.0;
		for (const float Scale : Data.InertiaResponseScales)
		{
			Sigma = FMath::Max(Sigma, (double)Scale);
		}
		if (Data.InertiaPaintScale > 0.f)
		{
			Sigma /= (double)Data.InertiaPaintScale;
		}
		if (!(Sigma > 0.0))
		{
			return 0;
		}

		constexpr int32 MaxModes = SoftbodyLumpRegion::MaxModes;
		// 모양의 크기 c를 재는 정점(측정 굽기가 원판 가운데를 고른 정점과 같은 몫)과 남길 모양의 고유값 비.
		constexpr float ScaleWeight = 0.3f;
		constexpr double KeepRatio = 1e-3;
		// 국소 모양을 만들 국소 몫의 하한(일지 254). 국소 몫 = 원판 누름의 응답 가운데 덩어리 모양이 그리지 못한 몫(유연도 비). 모든 원판에서
		// 이 아래면 덩어리 모양 하나가 누른 자리의 응답을 덩어리 식의 정밀도 안에서 이미 그린다 — 덩어리 뿌리 무름이 FEM과 맞는 폭(일지 248:
		// 0.8~1.1배)보다 작은 몫은 고쳐도 덩어리 식이 가리지 못한다. 그런 덩어리는 국소 모양 없이 실행 중 미지수 셋 풀이를 쓴다.
		constexpr double MinLocalPart = 0.2;
		// 표본 사이 누름에서 놓치는 몫의 상한(일지 263·265). 국소 모양은 원판 누름 표본의 응답을 섞어 그리므로, 표본 사이를 누르면 둘레 원판
		// 응답을 +·−로 섞는다. 원판 하나를 가리고 나머지 원판의 섞음으로 그 응답을 그려 보면 그 섞음이 얼마나 틀리는지 잴 수 있다(섞는 몫은
		// 실행 중 풀이와 같은 일 최소, 놓친 몫 = 넓이 가중 이동 오차 ÷ 응답 크기, 원판들의 중앙값). 놓친 몫이 덩어리 식의 정밀도(MinLocalPart와
		// 같은 20 %)보다 크면 국소 모양은 고치는 것보다 원판 크기의 들어감·솟음을 흩뜨리는 것이 더 크다. 구운 데이터로 잰 값: 화면에서 매끈한
		// 시험 캐릭터 가슴·엉덩이와 시연 몸 0.08~0.15, 우글거린 소시지 0.66과 큰 소품 0.61~0.98. 그런 덩어리는 모양 없이 실행 중 미지수 셋 풀이를 쓴다.
		constexpr double MaxBetweenSampleMiss = 0.2;
		struct FRegionShapes
		{
			int32 Group = INDEX_NONE;
			int32 NumModes = 0;
			/** 렌더 정점 → 원판 응답의 정점 번호(없으면 INDEX_NONE). */
			TArray<int32> LocalOf;
			/** [i × NumModes + m]: 정점 i에서 모양 m(가장 큰 크기 1, 메시 로컬 기본 자세 축). */
			TArray<FVector3f> Shapes;
		};
		TArray<FRegionShapes> Regions;
		TMap<int32, int32> RegionOfGroup;

		for (FSoftbodyInertiaLump& Lump : Lumps)
		{
			const int32 Group = Lump.Group;
			const FName Bone = Data.InertiaGroupBones.IsValidIndex(Group) ? Data.InertiaGroupBones[Group] : NAME_None;
			const FString LumpName = Bone.ToString();
			const FSoftbodyLumpDiscBake* Disc = Data.LumpDiscBakes.FindByPredicate([Group](const FSoftbodyLumpDiscBake& Bake) { return Bake.Group == Group; });
			if (!Disc)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Lump %d (%s) local shapes: none, the measure bake has no disc presses for this lump"), Group, *LumpName);
				continue;
			}
			const int32 NumDiscs = Disc->LoadOffsets.Num() - 1;
			const int32 N = Disc->Vertices.Num();
			bool bValid = NumDiscs >= 1 && NumDiscs <= MaxModes && N > 0 && Disc->Responses.Num() == NumDiscs * N
				&& Disc->LoadVertices.Num() == Disc->LoadForces.Num() && Disc->LoadOffsets[0] == 0 && Disc->LoadOffsets.Last() == Disc->LoadVertices.Num()
				&& Data.VertexAreas.Num() == NumVertices;
			for (int32 j = 0; bValid && j < NumDiscs; ++j)
			{
				bValid = Disc->LoadOffsets[j] <= Disc->LoadOffsets[j + 1];
			}
			for (int32 i = 0; bValid && i < N; ++i)
			{
				bValid = Disc->Vertices[i] >= 0 && Disc->Vertices[i] < NumVertices;
			}
			for (int32 l = 0; bValid && l < Disc->LoadVertices.Num(); ++l)
			{
				bValid = Disc->LoadVertices[l] >= 0 && Disc->LoadVertices[l] < NumVertices;
			}
			const double Volume = (double)Lump.Volume;
			if (!bValid || !(Volume > 0.0))
			{
				UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lump %d (%s) local shapes: none, its disc presses do not match this mesh. Press Run Bake."),
					Group, *LumpName);
				continue;
			}

			// 렌더 정점 → 원판 응답의 정점 번호(없으면 INDEX_NONE).
			TArray<int32> LocalOf;
			LocalOf.Init(INDEX_NONE, NumVertices);
			for (int32 i = 0; i < N; ++i)
			{
				LocalOf[Disc->Vertices[i]] = i;
			}

			// 표본 사이 누름에서 놓치는 몫(MaxBetweenSampleMiss). 일 W_kl = 누름 k의 힘 · u_l(누른 정점에서, 대칭으로 맞춤). 원판 j를 가리고
			// 나머지 원판 응답의 섞음 Σ w_k·u_k로 u_j를 그린다 — w는 일 최소(Σ_l W_kl·w_l = W_kj, 작은 고유값은 버림). 원판이 하나뿐이면
			// 다른 자리를 그릴 표본이 없다(놓친 몫 1).
			double BetweenSampleMiss = 1.0;
			if (NumDiscs >= 2)
			{
				TArray<double> Work;
				Work.Init(0.0, NumDiscs * NumDiscs);
				for (int32 k = 0; k < NumDiscs; ++k)
				{
					for (int32 l = Disc->LoadOffsets[k]; l < Disc->LoadOffsets[k + 1]; ++l)
					{
						const int32 Local = LocalOf[Disc->LoadVertices[l]];
						if (Local == INDEX_NONE)
						{
							continue;
						}
						const FVector3d F(Disc->LoadForces[l]);
						for (int32 m = 0; m < NumDiscs; ++m)
						{
							Work[k * NumDiscs + m] += F.Dot(FVector3d(Disc->Responses[m * N + Local]));
						}
					}
				}
				for (int32 k = 0; k < NumDiscs; ++k)
				{
					for (int32 m = k + 1; m < NumDiscs; ++m)
					{
						const double Average = 0.5 * (Work[k * NumDiscs + m] + Work[m * NumDiscs + k]);
						Work[k * NumDiscs + m] = Average;
						Work[m * NumDiscs + k] = Average;
					}
				}
				const int32 M = NumDiscs - 1;
				TArray<int32> Others;
				TArray<double> Sub;
				TArray<double> SubValues;
				TArray<double> SubVectors;
				TArray<double> Weights;
				TArray<double> Misses;
				for (int32 j = 0; j < NumDiscs; ++j)
				{
					Others.Reset();
					for (int32 k = 0; k < NumDiscs; ++k)
					{
						if (k != j)
						{
							Others.Add(k);
						}
					}
					Sub.SetNumUninitialized(M * M);
					for (int32 a = 0; a < M; ++a)
					{
						for (int32 b = 0; b < M; ++b)
						{
							Sub[a * M + b] = Work[Others[a] * NumDiscs + Others[b]];
						}
					}
					LumpRegionJacobi(Sub, M, SubValues, SubVectors);
					double ValueMax = 0.0;
					for (const double Value : SubValues)
					{
						ValueMax = FMath::Max(ValueMax, FMath::Abs(Value));
					}
					Weights.Init(0.0, M);
					for (int32 e = 0; e < M; ++e)
					{
						if (!(SubValues[e] > 1e-10 * ValueMax))
						{
							continue;
						}
						double Projection = 0.0;
						for (int32 a = 0; a < M; ++a)
						{
							Projection += SubVectors[a * M + e] * Work[Others[a] * NumDiscs + j];
						}
						for (int32 a = 0; a < M; ++a)
						{
							Weights[a] += SubVectors[a * M + e] * Projection / SubValues[e];
						}
					}
					double ErrorSq = 0.0;
					double SizeSq = 0.0;
					for (int32 i = 0; i < N; ++i)
					{
						FVector3d Drawn = FVector3d::ZeroVector;
						for (int32 a = 0; a < M; ++a)
						{
							Drawn += FVector3d(Disc->Responses[Others[a] * N + i]) * Weights[a];
						}
						const FVector3d Measured(Disc->Responses[j * N + i]);
						const double Area = FMath::Max((double)Data.VertexAreas[Disc->Vertices[i]], 0.0);
						ErrorSq += Area * (Drawn - Measured).SizeSquared();
						SizeSq += Area * Measured.SizeSquared();
					}
					Misses.Add(SizeSq > 0.0 ? FMath::Sqrt(ErrorSq / SizeSq) : 1.0);
				}
				Misses.Sort();
				const int32 Half = Misses.Num() / 2;
				BetweenSampleMiss = Misses.Num() % 2 == 1 ? Misses[Half] : 0.5 * (Misses[Half - 1] + Misses[Half]);
			}
			if (BetweenSampleMiss > MaxBetweenSampleMiss)
			{
				UE_LOG(LogSoftbodyCarrier, Display,
					TEXT("Lump %d (%s) local shapes: none, its disc presses cannot draw a press between them: hiding one disc, the other discs miss %.0f%% ")
					TEXT("of its response (median of %d discs, more than %.0f%%). Between samples they would draw scattered sample-sized dents. It gives way as a whole only."),
					Group, *LumpName, 100.0 * BetweenSampleMiss, NumDiscs, 100.0 * MaxBetweenSampleMiss);
				continue;
			}

			// 덩어리 모양의 단단함 K = (V/σ)·C. C는 실행 중과 같이 대칭으로 맞춘 평균 무름이고, 양의 정부호가 아니면 실행 중에도 이 덩어리는
			// 비켜나지 않는다. K⁻¹ = (σ/V)·Σ e·eᵀ / μ.
			const FVector3f Mean[3] = {Lump.MeanShapeX, Lump.MeanShapeY, Lump.MeanShapeZ};
			TArray<double> CMat;
			CMat.SetNumUninitialized(9);
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Col = 0; Col < 3; ++Col)
				{
					CMat[Row * 3 + Col] = 0.5 * ((double)Mean[Col][Row] + (double)Mean[Row][Col]);
				}
			}
			TArray<double> CValues;
			TArray<double> CVectors;
			LumpRegionJacobi(CMat, 3, CValues, CVectors);
			const double CMax = FMath::Max3(CValues[0], CValues[1], CValues[2]);
			const double CMin = FMath::Min3(CValues[0], CValues[1], CValues[2]);
			if (!(CMax > 0.0) || CMin <= 1e-9 * CMax)
			{
				UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lump %d (%s) local shapes: none, the lump's mean softness is not positive in every direction"),
					Group, *LumpName);
				continue;
			}
			double KInv[3][3];
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Col = 0; Col < 3; ++Col)
				{
					KInv[Row][Col] = 0.0;
					for (int32 e = 0; e < 3; ++e)
					{
						KInv[Row][Col] += (Sigma / Volume) * CVectors[Row * 3 + e] * CVectors[Col * 3 + e] / CValues[e];
					}
				}
			}

			// 덩어리 모양 ĝ_k(v) = 덩어리 몫 × 칠 × 무름 k(덩어리 칸 값과 같은 정점 값, 다른 덩어리의 정점은 0).
			const auto LumpShape = [&](int32 Vertex, int32 k)
			{
				return Source.VertexLump[Vertex] == Group
					? FVector3d(Source.VertexShape[3 * Vertex + k]) * (double)Source.VertexLumpWeight[Vertex]
					: FVector3d::ZeroVector;
			};
			FRegionShapes Region;
			Region.Group = Group;
			Region.LocalOf = MoveTemp(LocalOf);

			// 누름마다 덩어리 모양이 그리는 몫: X_j = K⁻¹·Q_j, 그 유연도 Q_j·X_j(로그용).
			TArray<FVector3d> X;
			X.SetNumUninitialized(NumDiscs);
			TArray<double> WholeCompliance;
			WholeCompliance.SetNumUninitialized(NumDiscs);
			TArray<double> PressedShare;
			PressedShare.SetNumUninitialized(NumDiscs);
			for (int32 j = 0; j < NumDiscs; ++j)
			{
				FVector3d Q = FVector3d::ZeroVector;
				double ForceSum = 0.0;
				double ShareSum = 0.0;
				for (int32 l = Disc->LoadOffsets[j]; l < Disc->LoadOffsets[j + 1]; ++l)
				{
					const int32 Vertex = Disc->LoadVertices[l];
					const FVector3d F(Disc->LoadForces[l]);
					for (int32 k = 0; k < 3; ++k)
					{
						Q[k] += F.Dot(LumpShape(Vertex, k));
					}
					ForceSum += F.Length();
					ShareSum += F.Length() * (Source.VertexLump[Vertex] == Group ? (double)Source.VertexLumpWeight[Vertex] : 0.0);
				}
				for (int32 Row = 0; Row < 3; ++Row)
				{
					X[j][Row] = KInv[Row][0] * Q.X + KInv[Row][1] * Q.Y + KInv[Row][2] * Q.Z;
				}
				WholeCompliance[j] = Q.Dot(X[j]);
				PressedShare[j] = ForceSum > 0.0 ? ShareSum / ForceSum : 0.0;
			}
			// 덩어리 모양이 그리지 못한 몫 R_j(v) = u_j(v) − Σ_k ĝ_k(v)·X_jk.
			TArray<FVector3d> R;
			R.SetNumUninitialized(NumDiscs * N);
			for (int32 j = 0; j < NumDiscs; ++j)
			{
				for (int32 i = 0; i < N; ++i)
				{
					const int32 Vertex = Disc->Vertices[i];
					R[j * N + i] = FVector3d(Disc->Responses[j * N + i]) - LumpShape(Vertex, 0) * X[j].X - LumpShape(Vertex, 1) * X[j].Y
						- LumpShape(Vertex, 2) * X[j].Z;
				}
			}
			// 남은 몫의 유연도 g_jl = 누름 j의 힘 · R_l(누른 정점에서).
			TArray<double> G;
			G.Init(0.0, NumDiscs * NumDiscs);
			int32 NumMissing = 0;
			for (int32 j = 0; j < NumDiscs; ++j)
			{
				for (int32 l = Disc->LoadOffsets[j]; l < Disc->LoadOffsets[j + 1]; ++l)
				{
					const int32 Local = Region.LocalOf[Disc->LoadVertices[l]];
					if (Local == INDEX_NONE)
					{
						++NumMissing;
						continue;
					}
					const FVector3d F(Disc->LoadForces[l]);
					for (int32 m = 0; m < NumDiscs; ++m)
					{
						G[j * NumDiscs + m] += F.Dot(R[m * N + Local]);
					}
				}
			}
			for (int32 j = 0; j < NumDiscs; ++j)
			{
				for (int32 m = j + 1; m < NumDiscs; ++m)
				{
					const double Average = 0.5 * (G[j * NumDiscs + m] + G[m * NumDiscs + j]);
					G[j * NumDiscs + m] = Average;
					G[m * NumDiscs + j] = Average;
				}
			}
			TArray<double> LocalCompliance;
			LocalCompliance.SetNumUninitialized(NumDiscs);
			for (int32 j = 0; j < NumDiscs; ++j)
			{
				LocalCompliance[j] = G[j * NumDiscs + j];
			}
			// 원판마다 국소 몫과 그 가장 큰 값(일지 254 — 국소 모양을 만들지 정한다).
			double LargestLocalPart = 0.0;
			for (int32 j = 0; j < NumDiscs; ++j)
			{
				const double Total = WholeCompliance[j] + LocalCompliance[j];
				LargestLocalPart = Total > 0.0 ? FMath::Max(LargestLocalPart, LocalCompliance[j] / Total) : LargestLocalPart;
			}
			const auto LogDiscs = [&]()
			{
				for (int32 j = 0; j < NumDiscs; ++j)
				{
					const FVector3f Center = Disc->Centers.IsValidIndex(j) ? Disc->Centers[j] : FVector3f::ZeroVector;
					const double Total = WholeCompliance[j] + LocalCompliance[j];
					UE_LOG(LogSoftbodyCarrier, Display,
						TEXT("  disc %2d at (%.1f %.1f %.1f), lump share %.2f: whole-lump compliance %.4f, local compliance %.4f (local part %.0f%%)"),
						j, Center.X, Center.Y, Center.Z, PressedShare[j], WholeCompliance[j], LocalCompliance[j],
						Total != 0.0 ? 100.0 * LocalCompliance[j] / Total : 0.0);
				}
			};
			if (LargestLocalPart < MinLocalPart)
			{
				UE_LOG(LogSoftbodyCarrier, Display,
					TEXT("Lump %d (%s) local shapes: none needed, its wobble shape already gives every disc press %.0f%% or more of the measured response ")
					TEXT("(largest local part %.0f%%, disc radius %.2f)"),
					Group, *LumpName, 100.0 * (1.0 - MinLocalPart), 100.0 * LargestLocalPart, Disc->Radius);
				LogDiscs();
				continue;
			}
			TArray<double> Lambda;
			TArray<double> Vectors;
			LumpRegionJacobi(G, NumDiscs, Lambda, Vectors);
			TArray<int32> Order;
			for (int32 m = 0; m < NumDiscs; ++m)
			{
				Order.Add(m);
			}
			Order.Sort([&Lambda](int32 A, int32 B) { return Lambda[A] > Lambda[B]; });
			const double LambdaMax = Lambda[Order[0]];

			// 모양 Φ_m = Σ_j R_j·v_jm, 크기 c_m = 덩어리 몫 ScaleWeight 이상 정점에서 가장 큰 크기.
			TArray<TArray<FVector3f>> ModeShapes;
			double StiffMin = TNumericLimits<double>::Max();
			double StiffMax = 0.0;
			for (const int32 m : Order)
			{
				if (!(LambdaMax > 0.0) || Lambda[m] <= KeepRatio * LambdaMax || ModeShapes.Num() >= MaxModes)
				{
					break;
				}
				TArray<FVector3d> Shape;
				Shape.Init(FVector3d::ZeroVector, N);
				for (int32 j = 0; j < NumDiscs; ++j)
				{
					const double Coef = Vectors[j * NumDiscs + m];
					for (int32 i = 0; i < N; ++i)
					{
						Shape[i] += R[j * N + i] * Coef;
					}
				}
				double Largest = 0.0;
				double LargestAll = 0.0;
				for (int32 i = 0; i < N; ++i)
				{
					const int32 Vertex = Disc->Vertices[i];
					const double Size = Shape[i].Length();
					LargestAll = FMath::Max(LargestAll, Size);
					if (Source.VertexLump[Vertex] == Group && Source.VertexLumpWeight[Vertex] >= ScaleWeight)
					{
						Largest = FMath::Max(Largest, Size);
					}
				}
				Largest = Largest > 0.0 ? Largest : LargestAll;
				if (!(Largest > 1e-20))
				{
					continue;
				}
				TArray<FVector3f>& Stored = ModeShapes.AddDefaulted_GetRef();
				Stored.SetNumUninitialized(N);
				for (int32 i = 0; i < N; ++i)
				{
					Stored[i] = FVector3f(Shape[i] / Largest);
				}
				const double Stiffness = (Sigma / Volume) * Lambda[m] / (Largest * Largest);
				Lump.RegionStiffness.Add((float)Stiffness);
				StiffMin = FMath::Min(StiffMin, Stiffness);
				StiffMax = FMath::Max(StiffMax, Stiffness);
			}

			const FString MissingNote = NumMissing > 0
				? FString::Printf(TEXT(", %d pressed vertices without a response skipped"), NumMissing) : FString();
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Lump %d (%s) local shapes: %d of %d disc presses kept (disc radius %.2f, between-sample miss %.0f%%, largest local part %.0f%%, local compliance eigenvalues ")
				TEXT("%.4g..%.4g), stiffness %.4g..%.4g (whole lump: mean softness eigenvalues %.3f %.3f %.3f, sigma %.4g, volume %.0f)%s"),
				Group, *LumpName, ModeShapes.Num(), NumDiscs, Disc->Radius, 100.0 * BetweenSampleMiss, 100.0 * LargestLocalPart, Lambda[Order.Last()], LambdaMax,
				ModeShapes.Num() > 0 ? StiffMin : 0.0, StiffMax, CValues[0], CValues[1], CValues[2], Sigma, Volume, *MissingNote);
			LogDiscs();
			if (ModeShapes.Num() == 0)
			{
				continue;
			}
			Region.NumModes = ModeShapes.Num();
			Region.Shapes.SetNumUninitialized(N * Region.NumModes);
			for (int32 i = 0; i < N; ++i)
			{
				for (int32 m = 0; m < Region.NumModes; ++m)
				{
					Region.Shapes[i * Region.NumModes + m] = ModeShapes[m][i];
				}
			}
			RegionOfGroup.Add(Group, Regions.Num());
			Regions.Add(MoveTemp(Region));
		}
		if (Regions.Num() == 0)
		{
			return 0;
		}

		// 거친 칸마다 줄 하나: 가장 많이 든 덩어리(표면 칸 수, 같으면 묶음 번호가 작은 쪽)의 표면 칸 모양 평균. 칸 모양 = 칸 자리 렌더 삼각형의
		// 세 정점 모양을 무게중심 좌표로 섞은 값(덩어리 칸 값과 같다 — 원판 응답이 없는 정점은 0).
		const int32 Coarse = Resolution / Block;
		constexpr int32 MaxRecords = 65534;
		OutIndex.Init(0, Coarse * Coarse);
		int32 NumRecords = 0;
		int32 NumSkippedCells = 0;
		TArray<int32, TInlineAllocator<16>> Counts;
		for (int32 CY = 0; CY < Coarse; ++CY)
		{
			for (int32 CX = 0; CX < Coarse; ++CX)
			{
				Counts.Init(0, Regions.Num());
				for (int32 Y = CY * Block; Y < (CY + 1) * Block; ++Y)
				{
					for (int32 X = CX * Block; X < (CX + 1) * Block; ++X)
					{
						const int32 Texel = Y * Resolution + X;
						const int32* RegionIndex = Kind[Texel] == KindSurface ? RegionOfGroup.Find(Source.TexelLump[Texel]) : nullptr;
						if (RegionIndex)
						{
							++Counts[*RegionIndex];
						}
					}
				}
				int32 Best = INDEX_NONE;
				for (int32 r = 0; r < Regions.Num(); ++r)
				{
					if (Counts[r] > 0 && (Best == INDEX_NONE || Counts[r] > Counts[Best]
						|| (Counts[r] == Counts[Best] && Regions[r].Group < Regions[Best].Group)))
					{
						Best = r;
					}
				}
				if (Best == INDEX_NONE)
				{
					continue;
				}
				const FRegionShapes& Region = Regions[Best];
				double Sum[MaxModes][3] = {};
				int32 Count = 0;
				for (int32 Y = CY * Block; Y < (CY + 1) * Block; ++Y)
				{
					for (int32 X = CX * Block; X < (CX + 1) * Block; ++X)
					{
						const int32 Texel = Y * Resolution + X;
						if (Kind[Texel] != KindSurface || Source.TexelLump[Texel] != Region.Group)
						{
							continue;
						}
						const FIntVector& Corners = TexelCorners[Texel];
						double WeightSum = 0.0;
						double Mix[MaxModes][3] = {};
						for (int32 c = 0; c < 3; ++c)
						{
							const int32 Vertex = Corners[c];
							const double W = FMath::Max(TexelBary[Texel][c], 0.0);
							if (Vertex < 0 || Vertex >= NumVertices || W <= 0.0)
							{
								continue;
							}
							WeightSum += W;
							const int32 Local = Region.LocalOf[Vertex];
							if (Local == INDEX_NONE)
							{
								continue;
							}
							for (int32 m = 0; m < Region.NumModes; ++m)
							{
								const FVector3f& Shape = Region.Shapes[Local * Region.NumModes + m];
								Mix[m][0] += W * Shape.X;
								Mix[m][1] += W * Shape.Y;
								Mix[m][2] += W * Shape.Z;
							}
						}
						if (WeightSum <= 0.0)
						{
							continue;
						}
						for (int32 m = 0; m < Region.NumModes; ++m)
						{
							for (int32 k = 0; k < 3; ++k)
							{
								Sum[m][k] += Mix[m][k] / WeightSum;
							}
						}
						++Count;
					}
				}
				if (Count == 0)
				{
					continue;
				}
				if (NumRecords >= MaxRecords)
				{
					++NumSkippedCells;
					continue;
				}
				OutIndex[CY * Coarse + CX] = (uint16)(NumRecords + 1);
				++NumRecords;
				// 텍셀 k(0~11) = 성분 k / 4의 모양 4(k % 4) ~ +3, 텍셀 12의 R = 덩어리 묶음.
				for (int32 k = 0; k < 12; ++k)
				{
					const int32 Component = k / 4;
					const int32 First = 4 * (k % 4);
					OutRecords.Add(FFloat16Color(FLinearColor((float)(Sum[First][Component] / Count), (float)(Sum[First + 1][Component] / Count),
						(float)(Sum[First + 2][Component] / Count), (float)(Sum[First + 3][Component] / Count))));
				}
				OutRecords.Add(FFloat16Color(FLinearColor((float)Region.Group, 0.f, 0.f, 0.f)));
			}
		}
		const int32 RecordWidth = SoftbodyLumpRegion::RecordTexels * SoftbodyLumpRegion::RecordsPerRow;
		while (OutRecords.Num() % RecordWidth != 0)
		{
			OutRecords.Add(FFloat16Color(FLinearColor(0.f, 0.f, 0.f, 0.f)));
		}
		if (NumSkippedCells > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lump local shapes: the table is full (%d entries); %d bulge cells were left without local shapes"),
				MaxRecords, NumSkippedCells);
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Lump local shapes: %d lumps, %d entries over %dx%d bulge cells (%dx%d contact map cells each), table %dx%d"),
			Regions.Num(), NumRecords, Coarse, Coarse, Block, Block, RecordWidth, OutRecords.Num() / RecordWidth);
		return NumRecords;
	}
}

bool USoftbodyBakeLibrary::BakeSurfaceCarrier(USoftbodyMeshData* Data, int32 Resolution,
	const TArray<FVector4>& CoreSpheres, float EnvelopeCellSize, int32 TargetTriangles, float CoreShrink, float MaxThickness)
{
	if (!Data || (!Data->SourceMesh && !Data->SourceSkeletalMesh))
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Contact map bake: the baked data has no source mesh. Click Run Bake first."));
		return false;
	}
	Resolution = FMath::Clamp((int32)FMath::RoundUpToPowerOfTwo((uint32)FMath::Max(Resolution, 64)), 64, 2048);
	USkeletalMesh* Skeletal = Data->SourceSkeletalMesh;
	FMeshDescription* MD = Skeletal ? Skeletal->GetMeshDescription(0) : Data->SourceMesh->GetMeshDescription(0);
	if (!MD)
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Contact map bake: the mesh has no LOD0 source geometry. Reimport the mesh with its source data."));
		return false;
	}
	if (MD->Vertices().Num() != Data->VertexPositions.Num() || Data->TextureSize <= 0)
	{
		UE_LOG(LogSoftbodyCarrier, Error,
			TEXT("Contact map bake: the mesh has %d vertices but the baked data has %d. The mesh changed after the last bake; click Run Bake."),
			MD->Vertices().Num(), Data->VertexPositions.Num());
		return false;
	}

	// --- 0. 하드 서피스 조각(ADR-0037 §3.7) ---
	// 칠 단계가 메시 설명 삼각형마다 적은 조각 번호(FindHardPieces). 메시가 바뀌어 크기가 맞지 않으면 조각 없이 굽는다. 조각이 없으면 아래
	// 모든 하드 서피스 갈래를 건너뛴다 — 칠하지 않은 자산의 굽기는 지금과 같다.
	FHardPieceBake Hard;
	if (Data->HardPiecesFloating.Num() > 0)
	{
		if (Data->HardTrianglePieces.Num() == MD->Triangles().GetArraySize())
		{
			Hard.TrianglePiece = Data->HardTrianglePieces;
			Hard.Floating = Data->HardPiecesFloating;
			if (Data->HardTriangleSoftness.Num() == Hard.TrianglePiece.Num() * 3)
			{
				Hard.TriangleSoftness = Data->HardTriangleSoftness;
			}
			else
			{
				UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: the paint step did not record the Flex values, so every plate is baked black (barely bending). Press Run Bake."));
			}
			Hard.SkipEnvelope.Init(0, Hard.TrianglePiece.Num());
			for (int32 Tri = 0; Tri < Hard.TrianglePiece.Num(); ++Tri)
			{
				Hard.SkipEnvelope[Tri] = Hard.IsFloating(Hard.TrianglePiece[Tri]) ? 1 : 0;
			}
		}
		else
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Flex plates: the mesh changed since the paint was converted, so the contact map is baked without rigid pieces. Press Run Bake."));
		}
	}

	// 연속 굽힘(Flex 칠): 칠 단계가 모든 삼각형 코너의 Flex를 적었으면(HardTriangleSoftnessLayout 1) 아래 6d가 모든 칸에 Flex를
	// 굽는다. 옛 칠 단계(판 코너만)는 지금처럼 판 칸만이다 — CarrierSoftnessCoverage 0, 검증이 Reapply Paint를 권한다.
	const bool bFlexAllCells = Data->HardTriangleSoftnessLayout == 1;
	TArray<float> TriangleFlex;
	if (bFlexAllCells && Data->HardTriangleSoftness.Num() == MD->Triangles().GetArraySize() * 3)
	{
		TriangleFlex = Data->HardTriangleSoftness;
	}

	// --- 1. 봉투 (표면 장 변위가 보이는 슬롯만) ---
	FString SlotLabel;
	TSet<FName> EnvelopeSlots = ResolveEnvelopeSlots(Data, SlotLabel);
	FDynamicMesh3 Source;
	TArray<bool> VertexInEnvelopeSlots;
	const TArray<uint8>* SkipEnvelope = Hard.AnyFloating() ? &Hard.SkipEnvelope : nullptr;
	BuildEnvelopeSource(*MD, EnvelopeSlots, Source, VertexInEnvelopeSlots, SkipEnvelope);
	if (EnvelopeSlots.Num() > 0 && Source.TriangleCount() == 0)
	{
		// 슬롯 이름이 메시 설명의 폴리곤 그룹 이름과 맞지 않는 메시(절차 생성·이름 없는 그룹): 메시 전체를 봉투로 쓴다
		UE_LOG(LogSoftbodyCarrier, Warning,
			TEXT("No triangles found for the slots (%s): the mesh's polygon group names differ from its material slot names. The whole mesh is used for the outer surface."), *SlotLabel);
		EnvelopeSlots.Reset();
		SlotLabel = TEXT("whole mesh (slot names did not match)");
		Source = FDynamicMesh3();
		BuildEnvelopeSource(*MD, EnvelopeSlots, Source, VertexInEnvelopeSlots, SkipEnvelope);
	}
	UE_LOG(LogSoftbodyCarrier, Display, TEXT("Outer surface source: %s (%d triangles)"), *SlotLabel, Source.TriangleCount());
	// 속 메시(ADR-0028): 봉투(미는 방향·살 두께·겹친 층 판정)는 이 메시의 속에서 뽑는다. 칸 위치·UV·스킨·칠은 위의 대상 메시 그대로다.
	// 트램펄린·얇은 통처럼 대상 메시가 닫혀 있지 않거나 속이 비어 있으면 제 메시로는 봉투가 안 나온다. 두께 있는 껍질의 안쪽 면은
	// 봉투 점이 바깥 면 위에 떨어져 겹친 층 규칙(ADR-0007 §3.4)으로 바깥 면의 고스트가 되고, 같은 함몰을 같은 방향으로 받는다.
	if (Data->VolumeMesh)
	{
		const FMeshDescription* VolumeMD = Data->VolumeMesh->GetMeshDescription(0);
		if (!VolumeMD || VolumeMD->Triangles().Num() == 0)
		{
			UE_LOG(LogSoftbodyCarrier, Error, TEXT("Contact map bake: volume mesh %s has no LOD0 source geometry. Reimport it with its source data, or clear Volume Mesh."), *Data->VolumeMesh->GetName());
			return false;
		}
		Source = FDynamicMesh3();
		FMeshDescriptionToDynamicMesh VolumeConverter;
		VolumeConverter.bDisableAttributes = true;
		VolumeConverter.Convert(VolumeMD, Source);
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Outer surface taken from volume mesh %s (%d triangles); cell positions, UVs, skin weights and paint still come from the target mesh"),
			*Data->VolumeMesh->GetName(), Source.TriangleCount());
	}
	const double Cell = EnvelopeCellSize > 0.f ? (double)EnvelopeCellSize : Source.GetBounds().MaxDim() / 256.0;
	FDynamicMesh3 Envelope;
	if (!ExtractEnvelope(Source, Cell, TargetTriangles, Envelope))
	{
		return false;
	}
	FDynamicMeshAABBTree3 EnvelopeBVH(&Envelope, true);
	TArray<FVector3d> EnvelopeNormals;
	ComputeVertexNormals(Envelope, EnvelopeNormals);
	double EnvelopeArea = 0.0;
	for (const int32 Tri : Envelope.TriangleIndicesItr())
	{
		EnvelopeArea += Envelope.GetTriArea(Tri);
	}
	UE_LOG(LogSoftbodyCarrier, Display, TEXT("Outer surface: cell %.2f cm, %d vertices, %d triangles, area %.0f"),
		Cell, Envelope.VertexCount(), Envelope.TriangleCount(), EnvelopeArea);

	// 하드 서피스: 떠 있는 조각마다 그 밑 겉면의 바깥 노멀 — 봉투는 조각을 빼고 뽑았으니 조각 가운데에서 가장 가까운 봉투 점이 조각 밑 겉면이다.
	if (Hard.AnyFloating())
	{
		const TVertexAttributesConstRef<FVector3f> MDPositions = MD->GetVertexPositions();
		TArray<FVector3d> CenterSum;
		TArray<double> AreaSum;
		CenterSum.Init(FVector3d::ZeroVector, Hard.Num());
		AreaSum.Init(0.0, Hard.Num());
		for (const FTriangleID Tri : MD->Triangles().GetElementIDs())
		{
			const int32 Piece = Hard.PieceOf(Tri);
			if (!Hard.IsFloating(Piece))
			{
				continue;
			}
			const TArrayView<const FVertexID> C = MD->GetTriangleVertices(Tri);
			const FVector3d A(MDPositions[C[0]]), B(MDPositions[C[1]]), D(MDPositions[C[2]]);
			const double W = 0.5 * FVector3d::CrossProduct(B - A, D - A).Size();
			CenterSum[Piece - 1] += W * (A + B + D) / 3.0;
			AreaSum[Piece - 1] += W;
		}
		Hard.Below.Init(FVector3d::ZeroVector, Hard.Num());
		for (int32 p = 0; p < Hard.Num(); ++p)
		{
			if (!Hard.Floating[p] || AreaSum[p] <= 0.0)
			{
				continue;
			}
			double DistanceSq = 0.0;
			const int32 Tri = EnvelopeBVH.FindNearestTriangle(CenterSum[p] / AreaSum[p], DistanceSq);
			if (Tri != IndexConstants::InvalidID)
			{
				const FIndex3i T = Envelope.GetTriangle(Tri);
				Hard.Below[p] = (EnvelopeNormals[T.A] + EnvelopeNormals[T.B] + EnvelopeNormals[T.C]).GetSafeNormal();
			}
		}
	}

	// --- 2. 차트·아틀라스 + 고스트 ---
	// 차트는 메시 UV0 아일랜드(ADR-0007) 또는 봉투를 XAtlas로 자른 조각이다. 아틀라스 메시 = 칸이 가리키는 점을 품은 메시
	// (UV0 차트면 봉투 슬롯 렌더 삼각형, XAtlas면 봉투 자체). 칸의 미는 방향·두께는 언제나 봉투에서 가져온다. 칸의 위치는
	// XAtlas면 봉투 점, UV0 차트면 그려지는 면(렌더 삼각형 위 퐁 곡면 점, ADR-0007 §3.6)이다.
	FCarrierAtlas Atlas;
	FMeshUV0Charts UV0;
	bool bUV0Charts = false;
	if (Data->CarrierCharts == ESoftbodyCarrierCharts::MeshUV0 || Data->CarrierCharts == ESoftbodyCarrierCharts::MeshUV0AsIs)
	{
		FString Reason;
		bUV0Charts = ChartByMeshUV0(*MD, EnvelopeSlots, Resolution, Data->CarrierCharts == ESoftbodyCarrierCharts::MeshUV0, UV0, Atlas,
			Reason);
		if (!bUV0Charts)
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Could not use the mesh's UV0 islands for the contact map (%s), so the outer surface is split into islands automatically instead. Fix the mesh's UV0, or set Contact Map Seams to Automatic (XAtlas)."), *Reason);
			Atlas = FCarrierAtlas();
			UV0 = FMeshUV0Charts();
		}
	}
	if (!bUV0Charts && !BuildAtlas(Envelope, Resolution, Atlas))
	{
		return false;
	}
	TUniquePtr<FDynamicMeshAABBTree3> UV0BVH;
	if (bUV0Charts)
	{
		UV0BVH = MakeUnique<FDynamicMeshAABBTree3>(&UV0.Mesh, true);
	}
	const FDynamicMesh3& AtlasMesh = bUV0Charts ? UV0.Mesh : Envelope;
	const FDynamicMeshAABBTree3& AtlasBVH = bUV0Charts ? *UV0BVH : EnvelopeBVH;
	BuildGhosts(AtlasMesh, AtlasBVH, Atlas);

	// --- 3. 두께 기준 코어 ---
	TArray<FVector4> Cores = CoreSpheres;
	if (Cores.Num() == 0 && Skeletal)
	{
		// 측정 베이크와 같은 뼈대: 본별 강체 설정(흔들림 본 제외 등)을 그대로 따르고, 뼈의 굵기도 같은 속(속 메시가 있으면 그 메시)에서 잰다.
		// 뼈로 치는 본도 측정 베이크가 적은 살 슬롯으로 고른다(일지 239 — 머리카락 가닥 속 구가 가슴 살 두께를 정하지 않게).
		const TSet<FName> FleshSlots(Data->RigidCoreSlots);
		SoftbodyBakeShared::CollectCoreSpheres(Skeletal, Cores, CoreShrink,
			Data->RigidShrinkPerBone.Num() > 0 ? &Data->RigidShrinkPerBone : nullptr,
			Data->VolumeMesh ? Data->VolumeMesh->GetMeshDescription(0) : nullptr, &FleshSlots);
	}

	/** 점 P(바깥 노멀 N)에서 가장 가까운 코어까지 거리 — 눌리는 정도 칠을 곱하기 전의 살 두께. */
	const auto ThicknessAt = [&](const FVector3d& P, const FVector3d& N)
	{
		double Thickness = TNumericLimits<double>::Max();
		for (const FVector4& Core : Cores)
		{
			Thickness = FMath::Min(Thickness, FVector3d::Distance(P, FVector3d(Core.X, Core.Y, Core.Z)) - Core.W);
		}
		if (Cores.Num() == 0)
		{
			// 코어가 없으면 안쪽으로 쏜 광선이 반대편 봉투에 닿는 거리의 절반(몸의 중심까지)을 쓴다.
			const FRay3d Ray(P - N * (Cell * 0.25), -N);
			double HitT = 0.0;
			int32 HitTri = IndexConstants::InvalidID;
			Thickness = EnvelopeBVH.FindNearestHitTriangle(Ray, HitT, HitTri) ? 0.5 * HitT : Cell * 8.0;
		}
		if (MaxThickness > 0.f)
		{
			Thickness = FMath::Min(Thickness, (double)MaxThickness);
		}
		return FMath::Max(Thickness, Cell);
	};

	// 렌더 정점(MD 정점 순서 = 텍셀 표와 페인트의 순서).
	FStaticMeshConstAttributes Attributes(*MD);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TArray<FVertexID> VertexOrder;
	TArray<FVector3d> VertexPositions;
	for (const FVertexID Vertex : MD->Vertices().GetElementIDs())
	{
		VertexOrder.Add(Vertex);
		VertexPositions.Add(FVector3d(Positions[Vertex]));
	}

	// --- 3.2 칠한 값: Depth(페인트 G, ADR-0016 §5·ADR-0038)와 Volume(페인트 A, ADR-0024) ---
	// 캐리어 칸의 살 두께 = 코어까지 거리 × 눌리는 정도 칠. 콜라이더는 살보다 한없이 단단하므로 "덜 눌린다"는 것은 그 자리 살이
	// 얇다는 뜻뿐이다. 두께에 곱해 두면 누른 깊이의 상한, 함몰 둘레 폭, 끌림이 퍼지는 폭, 솟음이 번지는 폭과 높이가 모두 같은 얇은
	// 살에서 나온다. 전에는 머티리얼이 다 풀린 변위에 칠을 곱해, 칠 0.5인 곳은 콜라이더가 절반만 그려진 자국을 뚫고 나왔고 솟음은
	// 칠과 상관없이 두꺼운 살의 폭으로 번졌다. 칠 0은 살이 없는 곳(두께 0)이다: 눌리지 않고, 솟는 살이 지나가지도 못한다.
	// 값은 정점마다 읽어 두고 칸으로는 칸 자리 렌더 삼각형의 무게중심 보간 + 렌더 변 길이만큼의 다듬기로 옮긴다(아래 4c,
	// PaintCellsFromVertices). 겹친 층은 캐리어 칸이 바깥 층 하나라 옷이 덮은 살의 칸은 옷 정점의 값을 받는다(페인트 변환이 옷 정점에
	// 살의 값을 넘겨 두므로 결국 살의 값이다). 칠을 읽지 못한 정점은 음수로 두고 보간에서 뺀다. 겉껍질 장력 칠(옛 페인트 R)은 일지 298에 없어졌다 — 컴포넌트 값 하나다.
	// 페인트 R = Jiggle(일지 298): 덩어리 손 밑 살층 강성 1/Jiggle(아래 6b) — 측정 FEM이 그 살을 같은 값으로 단단하게 푼다.
	TArray<double> VertexGain;
	TArray<double> VertexVolume;
	TArray<double> VertexJiggle;
	VertexGain.Init(-1.0, VertexOrder.Num());
	VertexVolume.Init(-1.0, VertexOrder.Num());
	VertexJiggle.Init(-1.0, VertexOrder.Num());
	double MinGain = 1.0;
	double MinVolume = 1.0;
	double MinJiggle = 1.0;
	if (Data->PaintTex && Data->TextureSize > 0)
	{
		TArray<FFloat16Color> PaintTexels;
		if (ReadTexture16F(Data->PaintTex, Data->TextureSize * Data->TextureSize, PaintTexels))
		{
			TVertexInstanceAttributesConstRef<FVector2f> InstanceUVs = Attributes.GetVertexInstanceUVs();
			for (int32 i = 0; i < VertexOrder.Num(); ++i)
			{
				if (!VertexInEnvelopeSlots.IsValidIndex(i) || !VertexInEnvelopeSlots[i])
				{
					continue;
				}
				// 고유 텍셀 UV는 위치당 하나라 어느 정점 인스턴스에서 읽어도 같다.
				TArrayView<const FVertexInstanceID> Instances = MD->GetVertexVertexInstanceIDs(VertexOrder[i]);
				if (Instances.Num() == 0 || Data->TexelUVChannel < 0 || Data->TexelUVChannel >= InstanceUVs.GetNumChannels())
				{
					continue;
				}
				const FVector2f TexelUV = InstanceUVs.Get(Instances[0], Data->TexelUVChannel);
				const int32 TX = FMath::Clamp((int32)(TexelUV.X * Data->TextureSize), 0, Data->TextureSize - 1);
				const int32 TY = FMath::Clamp((int32)(TexelUV.Y * Data->TextureSize), 0, Data->TextureSize - 1);
				const FFloat16Color& Paint = PaintTexels[TY * Data->TextureSize + TX];
				const double Gain = FMath::Clamp((double)Paint.G.GetFloat(), 0.0, 1.0);
				// 페인트 A는 Volume 소스를 매핑한 슬롯이 있을 때만 부피 보존이다(옛 페인트의 A는 다른 값일 수 있다).
				const double Volume = Data->bPaintVolume ? FMath::Clamp((double)Paint.A.GetFloat(), 0.0, 1.0) : 1.0;
				const double Jiggle = FMath::Clamp((double)Paint.R.GetFloat(), 0.0, 1.0);
				VertexGain[i] = Gain;
				VertexVolume[i] = Volume;
				VertexJiggle[i] = Jiggle;
				MinGain = FMath::Min(MinGain, Gain);
				MinVolume = FMath::Min(MinVolume, Volume);
				MinJiggle = FMath::Min(MinJiggle, Jiggle);
			}
		}
		else
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Could not read the slot paint, so Depth and Volume are baked as 1 everywhere: %s. Press Run Bake."),
				*Data->PaintTex->GetPathName());
		}
	}
	const bool bGain = MinGain < 0.999;
	const bool bJiggle = MinJiggle < 0.999;

	// --- 3.3 덩어리 받침 (ADR-0034, SoftbodyLumpSupport.h) ---
	// 덩어리 칸(Bone Overrides에서 Lump인 본의 살)의 살 두께를 뼈까지가 아니라 그 둔덕이 얹힌 면까지로 잰다. 뼈대에 갈비뼈가 없어 가슴 칸이
	// 척추까지 재져 둔덕 높이의 두 배가 넘었다(일지 242). 덩어리가 없는 자산·스태틱 메시·영역 밖 칸은 값이 그대로다.
	SoftbodyLumpSupport::FSupport LumpSupport;
	bool bLumpSupport = false;
	if (Skeletal && Cores.Num() > 0 && Data->InertiaGroupLump.Contains(true))
	{
		SoftbodyLumpSupport::FInput Input;
		if (ReadVertexLumps(*Data, VertexOrder.Num(), Input.Lump, Input.LumpWeight))
		{
			Input.Positions = VertexPositions;
			TMap<FVertexID, int32> IndexOf;
			IndexOf.Reserve(VertexOrder.Num());
			for (int32 i = 0; i < VertexOrder.Num(); ++i)
			{
				IndexOf.Add(VertexOrder[i], i);
			}
			Input.Triangles.Reserve(MD->Triangles().Num());
			for (const FTriangleID TriID : MD->Triangles().GetElementIDs())
			{
				const TArrayView<const FVertexID> TriVertices = MD->GetTriangleVertices(TriID);
				const int32* A = IndexOf.Find(TriVertices[0]);
				const int32* B = IndexOf.Find(TriVertices[1]);
				const int32* C = IndexOf.Find(TriVertices[2]);
				if (A && B && C)
				{
					Input.Triangles.Emplace(*A, *B, *C);
				}
			}
			Input.BoneDepth.SetNumUninitialized(VertexOrder.Num());
			Input.Gain.SetNumUninitialized(VertexOrder.Num());
			ParallelFor(VertexOrder.Num(), [&](int32 i)
			{
				// 코어가 있으면 ThicknessAt은 노멀을 읽지 않는다.
				Input.BoneDepth[i] = ThicknessAt(VertexPositions[i], FVector3d::UnitZ());
				Input.Gain[i] = VertexGain[i] >= 0.0 ? VertexGain[i] : 1.0;
			});
			Input.Cores = Cores;
			Input.GroupBones = Data->InertiaGroupBones;
			bLumpSupport = LumpSupport.Build(Input);
		}
		else
		{
			UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Lump support: could not read the baked inertia groups, so lump flesh thickness stays measured to the bones. Click Run Bake again."));
		}
	}
	// 저작 에셋 미리보기의 Lump Base 보기가 이 막을 그대로 그린다(에디터 전용 기록, 실행 중에는 읽지 않음). 덩어리가 없으면 빈 기록.
	if (bLumpSupport)
	{
		LumpSupport.ExportMembrane(Data->LumpBaseVertices, Data->LumpBaseTriangles, Data->LumpBaseHeights);
		Data->LumpBaseNotes = LumpSupport.GetNotes();
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Lump base recorded for the preview: %d vertices, %d triangles"),
			Data->LumpBaseVertices.Num(), Data->LumpBaseTriangles.Num() / 3);
	}
	else
	{
		// 덩어리가 있는데 받침을 하나도 못 찾았으면 그 까닭 줄만 남는다.
		Data->LumpBaseVertices.Reset();
		Data->LumpBaseTriangles.Reset();
		Data->LumpBaseHeights.Reset();
		Data->LumpBaseNotes = LumpSupport.GetNotes();
	}
	Data->bLumpBaseRecorded = true;

	// --- 3.5 미는 방향 ---
	// 런타임은 칸마다 이 방향 반대로 살을 밀고 같은 방향으로 콜라이더 광선을 쏜다. 봉투 노멀을 그대로 쓰면 배꼽처럼 두께보다
	// 작은 오목은 벽마다 제 노멀 쪽으로 밀려 서로 멀어지며 벌어진다(캡처: 노멀이 주변보다 15~30° 기운 칸이
	// 옆으로 0.5~0.7 cm). 살은 두께 정도 범위가 한 덩어리로 움직이므로, 반경 두께 × PushDirectionSmoothing 안의 봉투 노멀을
	// 가우스 무게(넓이 × 같은 쪽을 향한 정도)로 평균 낸다. 반대를 향한 면(허벅지 안쪽끼리, 가슴 사이)은 섞지 않는다.
	// 얇은 곳은 반경이 작아 제 노멀과 거의 같다. 고르게 굽은 면은 양쪽이 상쇄되어 방향이 그대로다. 반경은 칠을 곱하기 전 살 두께로
	// 잰다 — 코어까지 거리, 덩어리 영역은 받침까지(3.3): 얇게 칠한 살은 누른 깊이가 얕아 방향이 조금 달라도 보이지 않고, 칠 0에서
	// 반경이 0이 되는 것도 피한다.
	TArray<FVector3d> PushNormals = EnvelopeNormals;
	{
		TArray<int32> VertexIds;
		TArray<FVector3d> VertexPoints;
		for (const int32 Vertex : Envelope.VertexIndicesItr())
		{
			VertexIds.Add(Vertex);
			VertexPoints.Add(Envelope.GetVertex(Vertex));
		}
		TArray<double> VertexArea;
		VertexArea.Init(0.0, Envelope.MaxVertexID());
		for (const int32 Tri : Envelope.TriangleIndicesItr())
		{
			const FIndex3i T = Envelope.GetTriangle(Tri);
			const double Third = Envelope.GetTriArea(Tri) / 3.0;
			VertexArea[T.A] += Third;
			VertexArea[T.B] += Third;
			VertexArea[T.C] += Third;
		}
		FPointGrid Grid;
		Grid.Build(VertexPoints, Cell * 4.0);
		TArray<double> Deviation;
		Deviation.Init(0.0, VertexIds.Num());
		ParallelFor(VertexIds.Num(), [&](int32 i)
		{
			const FVector3d& N = EnvelopeNormals[VertexIds[i]];
			double Depth = ThicknessAt(VertexPoints[i], N);
			if (bLumpSupport)
			{
				double Share = 0.0;
				double Height = 0.0;
				double BasePainted = 0.0;
				double BaseUnpainted = 0.0;
				if (LumpSupport.Query(VertexPoints[i], VertexPoints[i], Depth, Share, Height, BasePainted, BaseUnpainted))
				{
					Depth = FMath::Max(Depth + Share * (FMath::Min(Depth, Height + BaseUnpainted) - Depth), Cell);
				}
			}
			const double Sigma = PushDirectionSmoothing * Depth;
			const double InvTwoSigmaSq = 0.5 / (Sigma * Sigma);
			FVector3d Sum = FVector3d::ZeroVector;
			Grid.ForEachInRadius(VertexPoints[i], 3.0 * Sigma, [&](int32 j, double DistSq)
			{
				const FVector3d& Other = EnvelopeNormals[VertexIds[j]];
				const double Facing = N.Dot(Other);
				if (Facing > 0.0)
				{
					Sum += Other * (VertexArea[VertexIds[j]] * Facing * FMath::Exp(-DistSq * InvTwoSigmaSq));
				}
			});
			const FVector3d Push = Sum.GetSafeNormal();
			if (!Push.IsZero())
			{
				PushNormals[VertexIds[i]] = Push;
				Deviation[i] = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Push.Dot(N), -1.0, 1.0)));
			}
		});
		Deviation.Sort();
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Push direction: tilt from the outer surface normal 50%% %.1f deg, 95%% %.1f deg, max %.1f deg (radius = thickness x %.2f)"),
			Deviation.Num() ? Deviation[Deviation.Num() / 2] : 0.0, Deviation.Num() ? Deviation[Deviation.Num() * 95 / 100] : 0.0,
			Deviation.Num() ? Deviation.Last() : 0.0, PushDirectionSmoothing);
	}

	// --- 4. 칸별 위치·미는 방향·두께 (표면 칸과 고스트 칸 모두 자기가 가리키는 점에서) ---
	// 화면 노멀은 렌더가 쓰는 것으로(빌드 설정이 다시 계산하면 저장된 노멀은 쓰지 않는다, 일지 345).
	const TArray<FVector3f> RenderedNormals = SoftbodyBakeShared::RenderedNormals(
		Skeletal ? static_cast<const UObject*>(Skeletal) : static_cast<const UObject*>(Data->SourceMesh.Get()), *MD);
	const int32 NumTexels = Resolution * Resolution;
	TArray<FVector3d> TexelPosition;
	TArray<FVector3d> TexelNormal;
	TArray<double> TexelCoreDepth;   // 코어까지 거리
	TArray<double> TexelThickness;   // 살 두께 = 코어까지 거리 × 눌리는 정도 칠 — 런타임이 읽는 값
	TArray<FVector3d> TexelSourcePoint; // 칸이 가리키는 아틀라스 메시 점(UV0 차트면 렌더 삼각형 위 — 스킨 웨이트를 가져올 자리)
	TexelPosition.Init(FVector3d::ZeroVector, NumTexels);
	TexelNormal.Init(FVector3d::ZeroVector, NumTexels);
	TexelCoreDepth.Init(0.0, NumTexels);
	TexelThickness.Init(0.0, NumTexels);
	TexelSourcePoint.Init(FVector3d::ZeroVector, NumTexels);
	// 그려지는 면(ADR-0007 §3.6): UV0 차트면 칸 위치를 겹친 층 판정 뒤에 봉투 점에서 이 점으로 바꾼다.
	FDrawnSurface Drawn;
	TArray<FVector3d> TexelDrawn;
	if (bUV0Charts)
	{
		BuildDrawnSurface(*MD, RenderedNormals, UV0, Atlas, Drawn);
		TexelDrawn.Init(FVector3d::ZeroVector, NumTexels);
	}
	int32 NumSurface = 0;
	ParallelFor(NumTexels, [&](int32 Texel)
	{
		if (Atlas.Kind[Texel] == KindEmpty)
		{
			return;
		}
		const int32 Tri = Atlas.TexelTri[Texel];
		const FVector3d& Bary = Atlas.TexelBary[Texel];
		const FVector3d Q = AtlasPoint(AtlasMesh, Atlas, Tri, Bary);
		// 칸의 봉투 점과 그 자리 봉투 삼각형의 세 정점·무게. XAtlas면 아틀라스 메시가 곧 봉투다. UV0 차트면 렌더 삼각형 위
		// 점에서 가장 가까운 봉투 점(누르는 면은 바깥 봉투다).
		FVector3d P = Q;
		FIndex3i Corners(Atlas.UVToVertex[Atlas.UVIndices[Tri * 3]], Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + 1]],
			Atlas.UVToVertex[Atlas.UVIndices[Tri * 3 + 2]]);
		FVector3d Weights = Bary;
		if (bUV0Charts)
		{
			double DistanceSq = 0.0;
			const int32 EnvelopeTri = EnvelopeBVH.FindNearestTriangle(Q, DistanceSq);
			if (EnvelopeTri == IndexConstants::InvalidID)
			{
				return;
			}
			const FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Envelope, EnvelopeTri, Q);
			P = Query.ClosestTrianglePoint;
			Corners = Envelope.GetTriangle(EnvelopeTri);
			Weights = Query.TriangleBaryCoords;
		}
		const FVector3d N = (EnvelopeNormals[Corners.A] * Weights.X + EnvelopeNormals[Corners.B] * Weights.Y
			+ EnvelopeNormals[Corners.C] * Weights.Z).GetSafeNormal();
		const FVector3d Push = (PushNormals[Corners.A] * Weights.X + PushNormals[Corners.B] * Weights.Y
			+ PushNormals[Corners.C] * Weights.Z).GetSafeNormal();
		TexelPosition[Texel] = P;
		TexelNormal[Texel] = Push.IsZero() ? N : Push;
		TexelCoreDepth[Texel] = ThicknessAt(P, N);
		TexelThickness[Texel] = TexelCoreDepth[Texel];   // 눌리는 정도 칠은 아래 4c에서 곱한다
		TexelSourcePoint[Texel] = Q;
		if (bUV0Charts)
		{
			TexelDrawn[Texel] = DrawnPoint(AtlasMesh, Atlas, Drawn, Tri, Bary, DrawnSurfaceCurve);
		}
	});
	for (int32 Texel = 0; Texel < NumTexels; ++Texel)
	{
		NumSurface += Atlas.Kind[Texel] == KindSurface ? 1 : 0;
	}

	// --- 4c. 칸 자리 렌더 삼각형과 칠한 값 ---
	// 칸마다 그 자리 렌더 삼각형의 세 정점(렌더 정점 순서 번호)과 무게중심 좌표. UV0 차트면 칸이 가리키는 렌더 삼각형, XAtlas면 칸 점에서
	// 가장 가까운 봉투 슬롯 렌더 삼각형. 정점마다 가진 값(칠, 스킨 웨이트, 덩어리 값, 화면 노멀)은 모두 이 삼각형에서 칸으로 옮긴다 —
	// 메시가 그 값을 그리는 방법이라 칸 값이 정점 배치의 계단을 갖지 않는다(일지 208·209).
	FRenderNormals Render;
	TUniquePtr<FDynamicMeshAABBTree3> RenderBVH;
	if (!bUV0Charts)
	{
		BuildRenderNormals(*MD, RenderedNormals, EnvelopeSlots, Render, SkipEnvelope);
		RenderBVH = MakeUnique<FDynamicMeshAABBTree3>(&Render.Mesh, true);
	}
	TArray<FIntVector> TexelCorners;
	TArray<FVector3d> TexelCornerBary;
	{
		TMap<int32, int32> OrderOf; // 메시 디스크립션 정점 번호 → 렌더 정점 순서
		OrderOf.Reserve(VertexOrder.Num());
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			OrderOf.Add(VertexOrder[i].GetValue(), i);
		}
		TexelCorners.Init(FIntVector(INDEX_NONE, INDEX_NONE, INDEX_NONE), NumTexels);
		TexelCornerBary.Init(FVector3d::ZeroVector, NumTexels);
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty)
			{
				return;
			}
			int32 Source[3];
			FVector3d Bary = Atlas.TexelBary[Texel];
			if (bUV0Charts)
			{
				const int32 Tri = Atlas.TexelTri[Texel];
				if (Tri < 0)
				{
					return;
				}
				for (int32 c = 0; c < 3; ++c)
				{
					Source[c] = UV0.ElementVertex[Atlas.UVIndices[Tri * 3 + c]];
				}
			}
			else
			{
				double DistanceSq = 0.0;
				const int32 Tri = RenderBVH->FindNearestTriangle(TexelPosition[Texel], DistanceSq);
				if (Tri == IndexConstants::InvalidID)
				{
					return;
				}
				Bary = TMeshQueries<FDynamicMesh3>::TriangleDistance(Render.Mesh, Tri, TexelPosition[Texel]).TriangleBaryCoords;
				const FIndex3i Ids = Render.Mesh.GetTriangle(Tri);
				for (int32 c = 0; c < 3; ++c)
				{
					Source[c] = Render.SourceVertex[Ids[c]];
				}
			}
			FIntVector Corners;
			for (int32 c = 0; c < 3; ++c)
			{
				const int32* Order = OrderOf.Find(Source[c]);
				if (!Order)
				{
					return;
				}
				Corners[c] = *Order;
			}
			TexelCorners[Texel] = Corners;
			TexelCornerBary[Texel] = Bary;
		});
	}
	const double AtlasArea = bUV0Charts ? UV0.Area : EnvelopeArea;
	const double TexelSizeCm = FMath::Sqrt(AtlasArea / FMath::Max(NumSurface, 1));
	// 다듬는 반경(봉투 점에서 잰 렌더 변 길이). CarrierRedirect B에 적는 값은 칸 위치가 그려지는 면으로 옮겨진 뒤 다시 잰다(아래 5).
	const TArray<float> PaintRadius = (bGain || bJiggle || MinVolume < 0.999 || Hard.Num() > 0 || TriangleFlex.Num() > 0)
		? ComputeRenderEdgeLengths(*MD, EnvelopeSlots, VertexOrder, VertexPositions, Atlas, TexelPosition) : TArray<float>();
	/** 정점 칠 → 칸(PaintCellsFromVertices): 렌더 삼각형 보간 뒤 렌더 변 길이만큼 다듬는다. 다듬는 반경은 적어도 칸 1.5개. */
	const auto PaintCells = [&](const TArray<double>& VertexValue, const TCHAR* What, TArray<double>& OutCells)
	{
		double MaxChange = 0.0;
		PaintCellsFromVertices(VertexValue, 1.0, TexelCorners, TexelCornerBary, Atlas.Kind, TexelPosition, TexelNormal, PaintRadius,
			1.5 * TexelSizeCm, OutCells, MaxChange);
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("%s paint to cells: render triangle interpolation, then smoothed over the render edge length (largest change %.3f)"),
			What, MaxChange);
	};
	TArray<double> GainCells;
	if (bGain)
	{
		PaintCells(VertexGain, TEXT("Depth"), GainCells);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] != KindEmpty)
			{
				TexelThickness[Texel] = TexelCoreDepth[Texel] * GainCells[Texel];
			}
		}
	}
	if (bGain)
	{
		int32 NumNoFlesh = 0;
		int32 NumThinned = 0;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindSurface)
			{
				NumNoFlesh += TexelThickness[Texel] <= 0.0 ? 1 : 0;
				NumThinned += TexelThickness[Texel] > 0.0 && TexelThickness[Texel] < 0.999 * TexelCoreDepth[Texel] ? 1 : 0;
			}
		}
		UE_LOG(LogSoftbodyCarrier, Display,
			TEXT("Depth paint scales flesh thickness: of %d surface cells, %d (%.1f%%) have no flesh (paint 0) and %d (%.1f%%) are thinner; lowest painted vertex %.3f"),
			NumSurface, NumNoFlesh, 100.0 * NumNoFlesh / FMath::Max(NumSurface, 1), NumThinned,
			100.0 * NumThinned / FMath::Max(NumSurface, 1), MinGain);
	}
	else
	{
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Depth paint is 1 everywhere: flesh thickness is the full distance to the rigid core"));
	}

	// 덩어리 받침(3.3): 영역 칸의 살 두께 = min(지금 두께, 막 위 높이 × 칠 + 밑 두께)를 영역 몫만큼(영역 가장자리 삼각형에서 0 → 1).
	// 영역 가장자리에서 막 위 높이는 0, 밑 두께는 둘레의 지금 두께라 새 경계가 없다. 둔덕 높이(칠 전, 코어까지 거리를 넘지 않음)는
	// 로그로만 — 덩어리 양 V는 평균 무름 C와 같이 코어까지 잰다(BuildLumpTexels, 6b: 둘의 오차가 곱에서 지워진다).
	TArray<double> TexelMoundDepth = TexelCoreDepth;
	if (bLumpSupport)
	{
		const TArray<double> BodyThickness = TexelThickness;
		TArray<int32> SupportGroup;
		SupportGroup.Init(INDEX_NONE - 1, NumTexels);
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty)
			{
				return;
			}
			const double Core = TexelCoreDepth[Texel];
			double Share = 0.0;
			double Height = 0.0;
			double BasePainted = 0.0;
			double BaseUnpainted = 0.0;
			int32 Group = INDEX_NONE;
			if (!LumpSupport.Query(TexelSourcePoint[Texel], TexelPosition[Texel], Core, Share, Height, BasePainted, BaseUnpainted, &Group))
			{
				return;
			}
			const double Gain = bGain ? GainCells[Texel] : 1.0;
			const double Body = BodyThickness[Texel];
			TexelThickness[Texel] = Body + Share * (FMath::Min(Body, Height * Gain + BasePainted) - Body);
			TexelMoundDepth[Texel] = Core + Share * (FMath::Min(Core, Height) - Core);
			SupportGroup[Texel] = Group;
		});
		// 덩어리마다 영역 안 칸(살이 있는 표면 칸)의 살 두께 전후.
		TMap<int32, TPair<TArray<double>, TArray<double>>> ByGroup;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (SupportGroup[Texel] != INDEX_NONE - 1 && Atlas.Kind[Texel] == KindSurface && BodyThickness[Texel] > 0.0)
			{
				TPair<TArray<double>, TArray<double>>& Entry = ByGroup.FindOrAdd(SupportGroup[Texel]);
				Entry.Key.Add(BodyThickness[Texel]);
				Entry.Value.Add(TexelThickness[Texel]);
			}
		}
		TArray<int32> Keys;
		ByGroup.GetKeys(Keys);
		Keys.Sort();
		for (const int32 Group : Keys)
		{
			TArray<double>& Before = ByGroup[Group].Key;
			TArray<double>& After = ByGroup[Group].Value;
			Before.Sort();
			After.Sort();
			const FName Bone = Data->InertiaGroupBones.IsValidIndex(Group) ? Data->InertiaGroupBones[Group] : NAME_None;
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Lump support for %s: %d surface cells with flesh on its mound; flesh thickness median %.2f -> %.2f cm, 90%% %.2f -> %.2f cm, thickest %.2f -> %.2f cm"),
				*Bone.ToString(), Before.Num(), Before[Before.Num() / 2], After[After.Num() / 2], Before[Before.Num() * 9 / 10],
				After[After.Num() * 9 / 10], Before.Last(), After.Last());
		}
	}

	// --- 4b. 제 살 깊이 (ADR-0003 §16) ---
	// 런타임 접촉은 칸마다 −미는 방향으로 광선을 쏘아, 표면을 뚫지 않고 살 속에 묻힌 콜라이더도 살 두께 × ColumnSearch 안이면 그
	// 칸을 누른 것으로 본다(깊이 찌른 손가락 위로 피부가 닫히지 않게). 상자처럼 면이 꺾인 물체에서는 그 광선이 다른 면의 살까지
	// 들어가, 윗면으로 들어온 공을 옆면 칸이 제 살 속 콜라이더로 보고 같이 파였다. 그래서 칸마다 "광선 위의 점이
	// 다른 쪽을 향한 면보다 이 칸에 더 가까운 깊이"를 구워 둔다. 이 칸에 접하고 안쪽에 놓인 반지름 t의 공이 다른 쪽을 향한 표면 점
	// q를 품기 시작하는 t가 그 깊이다: t = |q − P|² / (2·(P − q)·N). 런타임은 묻힌 콜라이더를 이 깊이까지만 찾는다.
	// 표면 점으로는 캐리어 칸 점(봉투 위에 고르게 놓였다)을, 면이 향한 쪽으로는 미는 방향을 쓴다. 둥근 몸은 살 두께 × ColumnSearch가
	// 몸의 굽은 반지름보다 짧아 거의 전부 1이다. 전부 1이면 텍스처를 만들지 않는다.
	TArray<uint8> ColumnTexels;
	bool bColumn = false;
	{
		TArray<FVector3d> ColumnPoints;
		TArray<int32> ColumnPointTexel;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] != KindEmpty && !TexelNormal[Texel].IsZero())
			{
				ColumnPoints.Add(TexelPosition[Texel]);
				ColumnPointTexel.Add(Texel);
			}
		}
		FPointGrid Grid;
		Grid.Build(ColumnPoints, FMath::Max(Cell * 4.0, 0.5));
		ColumnTexels.Init(255, NumTexels);
		TArray<double> ColumnDepth;   // 줄어든 칸의 제 살 깊이(cm), 안 줄어든 칸은 음수 — 로그용
		ColumnDepth.Init(-1.0, NumTexels);
		ParallelFor(ColumnPoints.Num(), [&](int32 Index)
		{
			const int32 Texel = ColumnPointTexel[Index];
			const FVector3d& P = ColumnPoints[Index];
			const FVector3d& N = TexelNormal[Texel];
			const double Search = ColumnSearch * TexelThickness[Texel];
			// 이 칸에 접하고 반지름이 Search인 공 안의 점만 깊이를 Search보다 줄일 수 있다. 작은 것부터 ColumnMinPoints개를 남긴다.
			double Smallest[ColumnMinPoints];
			int32 Count = 0;
			Grid.ForEachInRadius(P - N * Search, Search, [&](int32 Other, double)
			{
				const FVector3d Offset = ColumnPoints[Other] - P;
				const double Below = -Offset.Dot(N);
				if (Below <= 1e-6 || FMath::Abs(TexelNormal[ColumnPointTexel[Other]].Dot(N)) > ColumnMaxFacing)
				{
					return;
				}
				const double T = Offset.SizeSquared() / (2.0 * Below);
				if (Count == ColumnMinPoints && T >= Smallest[ColumnMinPoints - 1])
				{
					return;
				}
				int32 Slot = FMath::Min(Count, ColumnMinPoints - 1);
				while (Slot > 0 && Smallest[Slot - 1] > T)
				{
					Smallest[Slot] = Smallest[Slot - 1];
					--Slot;
				}
				Smallest[Slot] = T;
				Count = FMath::Min(Count + 1, ColumnMinPoints);
			});
			if (Count < ColumnMinPoints || Smallest[ColumnMinPoints - 1] >= Search)
			{
				return;
			}
			const double Ratio = Smallest[ColumnMinPoints - 1] / Search;
			ColumnTexels[Texel] = (uint8)FMath::Clamp(FMath::RoundToInt(Ratio * 255.0), 1, 255);
			ColumnDepth[Texel] = Smallest[ColumnMinPoints - 1];
		});
		TArray<double> Limited;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindSurface && ColumnTexels[Texel] < 255)
			{
				Limited.Add(ColumnDepth[Texel]);
			}
		}
		Limited.Sort();
		bColumn = Limited.Num() > 0;
		if (bColumn)
		{
			UE_LOG(LogSoftbodyCarrier, Display,
				TEXT("Own flesh depth (how deep a buried collider still presses a cell): of %d surface cells, %d (%.1f%%) are shallower than flesh thickness x %.1f; their depth 10%% %.2f, 50%% %.2f, 90%% %.2f cm"),
				NumSurface, Limited.Num(), 100.0 * Limited.Num() / FMath::Max(NumSurface, 1), ColumnSearch,
				Limited[Limited.Num() / 10], Limited[Limited.Num() / 2], Limited[Limited.Num() * 9 / 10]);
		}
		else
		{
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Own flesh depth: every cell is at least flesh thickness x %.1f deep; no texture needed"), ColumnSearch);
		}
	}

	// 겹친 층(UV0 차트, ADR-0007 §3.4): 속옷과 그 아래 피부처럼 여러 차트가 같은 자리를 덮으면 바깥 층 하나만 함몰을 풀고 나머지 칸은
	// 그 층의 점을 가리키는 고스트 칸이 된다(함몰을 가져오고 나머지는 제 칸에서 계산). 두 층이 같은 값을 써서 서로 뚫지 않고, 솟음 출처도
	// 한 번만 센다. 안쪽 층이 바깥 층을 따르고, 가깝다는 이유로 바깥 층이 안쪽 층을 따르지는 않는다(원칙 P09, 일지 300).
	//  - 붙은 바깥 층은 그려지는 면으로(일지 210·300): 칸의 그려지는 점에서 미는 방향으로 봉투 칸 둘까지 광선을 쏴, 이 칸을 덮은
	//    다른 면을 찾는다. 봉투는 봉투 칸 격자에서 다시 뜬 껍질이라 그보다 가까운 두 층의 순서를 가리지 못한다 — 전에는 봉투로만 갈라
	//    시험 캐릭터의 1 mm 위 속옷 칸 약 30%가 실제 순서와 상관없이 피부로 넘어갔다. 맡는 층이 바뀌는 곳은 두 층이 가로지르는 선(간격 0)이라 턱이 없다.
	//  - 봉투 안에 묻힌 칸(속 메시 껍질의 안쪽 벽처럼, ADR-0028)은 봉투로: 봉투 점에서 가장 가까운 아틀라스 메시 점의 고스트다.
	//  - 봉투 밖으로 나온 칸 뒤의 간격이 그 자리 살 두께보다 크면 몸에서 떨어진 자락이라 밑 몸을 따른다(아래 떨어진 자락).
	// 장식 칸(fb/orn 안 A): 칸마다 그 칸이 속한 떠 있는 조각(칠 단계 번호, 0 = 아님) — 떠 있는 조각 삼각형 위의 표면 칸과 그 차트의 고스트 고리.
	// BuildHardPieceTexels가 이 칸에 판을 그리는 칸 표지를 둔다. OrnamentOwnPoint 1 = 밑 겉면을 못 찾아 같은 조각 이웃 칸의 밑 점을 빌린 칸(칸 위치는 제
	// 장식 점 그대로). UV0 차트 캐리어만 — 자동 섬 지도는 장식 삼각형이 봉투에서 빠져 장식 칸이 없다.
	TArray<int32> OrnamentPiece;
	TArray<uint8> OrnamentOwnPoint;
	if (bUV0Charts)
	{
		const double Margin = 0.05 * Cell;
		const double NearLayer = 2.0 * Cell;
		// 이만큼 안으로 붙은 두 층은 같은 면으로 본다(어느 쪽이 맡아도 차이가 이 정도).
		const double LayerTie = 0.005 * Cell;
		// 다른 층으로 볼 면의 방향: 이 칸의 미는 방향과 45° 안.
		const double LayerFacing = 0.7;
		const int32 R = Resolution;
		TArray<FVector2d> AliasTarget;
		TArray<int32> AliasTri;
		TArray<FVector3d> AliasBary;
		TArray<uint8> LayerRule;   // 1 = 바깥 층에 덮여 고스트, 2 = 가까운 층의 바깥 층, 3 = 봉투 안에 묻혀 고스트, 4 = 테두리 고스트가 바깥 층을 따름,
		                           // 5 = 떠 있는 하드 서피스 조각 칸이 밑 겉면을 따름(ADR-0037 §3.7), 6 = 떨어진 자락이 밑 몸을 따름
		TArray<uint8> FlapCandidate;   // 봉투 밖으로 한 칸 넘게 나온 바깥 층 칸(떨어진 자락 판정 대상)
		// 바깥 층에 덮인 칸(규칙 1·4)의 그려지는 면 사이 간격(cm, 미는 방향). 굽기 로그에 분포만 남긴다(일지 332 — Layer Max Gap과 견줄 숫자).
		TArray<float> LayerGap;
		// 덮인 칸(규칙 1·4)이 바깥 표면(봉투) 안에 묻혀 있는지(1 = 아래 묻힌 칸 규칙 3의 대상), 묻힌 칸(규칙 3)의 봉투까지 거리(cm). 로그 전용(일지 332).
		TArray<uint8> LayerGapBuried;
		TArray<float> BuriedDepth;
		// 떨어진 자락 판정의 결과(일지 301): 0 대상 아님, 1 밑 몸을 따름, 2 밑 살 두께 안에 매인 바깥 층, 3 뒤를 보는 다른 면에서 막힘,
		// 4 닿는 데까지 뒤에 면이 없음, 5 찾은 밑 몸 칸을 따를 수 없음(빈 칸·같은 차트·둘레 빈 칸), 6 제 껍질의 안쪽 벽을 지나 밑 몸을 따름
		TArray<uint8> FlapOutcome;
		// 칸 뒤(-미는 방향)에 같은 쪽을 보는 다른 면이 막히지 않고 있다 = 이 칸은 무언가를 덮은 바깥 층이다. 아래 바깥 층 미는 방향(D)은 이 칸만 고친다.
		TArray<uint8> LayerBehind;
		// 봉투 점이 이 칸이 아니라 다른 면의 것인 칸(봉투 밖이라 고스트로 만들지 않은 칸). 아래 D가 봉투 방향이 이 칸 제 면의 몸 쪽을
		// 가리키는지 본다(일지 301 g2).
		TArray<uint8> EnvFromOther;
		// 하드 서피스: 아틀라스 삼각형 → 조각 번호(조각이 없으면 비어 있다). 떠 있는 조각은 겹친 층 규칙을 뒤집는다 — 조각 칸이 밑 겉면 점의
		// 고스트가 되고, 밑 겉면 칸은 조각을 바깥 층으로 보지 않고 제 칸으로 남아 함몰을 푼다(접촉은 구운 시작점 올림으로 조각 윗면에서 잰다).
		TArray<int32> AtlasPiece;
		if (Hard.AnyFloating())
		{
			AtlasPiece.Init(0, AtlasMesh.MaxTriangleID());
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				AtlasPiece[Tri] = UV0.MeshTriangle.IsValidIndex(Tri) ? Hard.PieceOf(UV0.MeshTriangle[Tri]) : 0;
			}
		}
		const auto FloatingTri = [&](int32 Tri) { return AtlasPiece.Num() > 0 && Hard.IsFloating(AtlasPiece[Tri]); };
		// 장식 칸 기록(위 OrnamentPiece) — 아래 겹친 층 판정이 칸 종류를 바꾸기 전에. 표면 칸은 제 삼각형, 테두리 고스트는 출처 표면 칸의 삼각형으로 가린다
		// (고스트 고리의 삼각형은 가리킨 점의 것이라 밑 옷일 수 있다).
		if (AtlasPiece.Num() > 0)
		{
			OrnamentPiece.Init(0, NumTexels);
			OrnamentOwnPoint.Init(0, NumTexels);
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				int32 Owner = INDEX_NONE;
				if (Atlas.Kind[Texel] == KindSurface)
				{
					Owner = Atlas.TexelTri[Texel];
				}
				else if (Atlas.Kind[Texel] == KindGhost && Atlas.GhostSource.IsValidIndex(Texel) && Atlas.GhostSource[Texel] != INDEX_NONE)
				{
					Owner = Atlas.TexelTri[Atlas.GhostSource[Texel]];
				}
				if (Owner != INDEX_NONE && AtlasPiece.IsValidIndex(Owner) && FloatingTri(Owner))
				{
					OrnamentPiece[Texel] = AtlasPiece[Owner];
				}
			}
		}
		// 조각 칸에서 밑 겉면을 찾는 광선의 길이(조각 윗면이 겉면에서 떠 있는 높이보다 넉넉히).
		const double HardDrop = 4.0 * NearLayer;
		int32 NumHardUnattached = 0;
		// 같은 면(일지 300): 칸의 삼각형과 맞은 삼각형이 같은 자리 정점으로 이은 이웃 두 고리 안이면 같은 면이고 다른 층이 아니다 — UV 이음매
		// 건너편의 같은 면(성긴 삼각형은 그려지는 곡면이 이음매 양쪽에서 LayerTie보다 어긋난다, 일지 210의 4 cm 블록 522칸). 전에는 메시 조각
		// 번호가 같으면 다른 층이 아니라고 했는데, 같은 자리 정점으로 용접된 겹친 층(가슴골을 건너는 판과 그 뒤 살갗, 피부에 붙은 띠)이 모두 한
		// 조각이 되어 층 관계를 잃었다(장식 옷 시험 캐릭터, 합성 겹친 가슴골 조각). 접힌 제 살(가슴 밑)은 아래 광선이 마주 본 면에서 멈춰 가른다.
		TArray<int32> WeldRoot;
		WeldRoot.Init(INDEX_NONE, AtlasMesh.MaxVertexID());
		{
			TMap<FIntVector, int32> VertexAt;
			for (const int32 Vertex : AtlasMesh.VertexIndicesItr())
			{
				const FVector3d P = AtlasMesh.GetVertex(Vertex);
				const FIntVector Key((int32)FMath::RoundToDouble(P.X * 1000.0), (int32)FMath::RoundToDouble(P.Y * 1000.0),
					(int32)FMath::RoundToDouble(P.Z * 1000.0));
				WeldRoot[Vertex] = VertexAt.FindOrAdd(Key, Vertex);
			}
		}
		TArray<TArray<int32>> LocalRoots;   // 삼각형 → 이웃 두 고리의 같은 자리 정점 대표(정렬)
		LocalRoots.SetNum(AtlasMesh.MaxTriangleID());
		{
			TArray<TArray<int32>> RootTris;
			RootTris.SetNum(AtlasMesh.MaxVertexID());
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				const FIndex3i T = AtlasMesh.GetTriangle(Tri);
				for (int32 c = 0; c < 3; ++c)
				{
					RootTris[WeldRoot[T[c]]].AddUnique(Tri);
				}
			}
			ParallelFor(AtlasMesh.MaxTriangleID(), [&](int32 Tri)
			{
				if (!AtlasMesh.IsTriangle(Tri))
				{
					return;
				}
				TArray<int32>& Local = LocalRoots[Tri];
				const FIndex3i T = AtlasMesh.GetTriangle(Tri);
				for (int32 c = 0; c < 3; ++c)
				{
					for (const int32 Near : RootTris[WeldRoot[T[c]]])
					{
						const FIndex3i U = AtlasMesh.GetTriangle(Near);
						for (int32 d = 0; d < 3; ++d)
						{
							Local.AddUnique(WeldRoot[U[d]]);
						}
					}
				}
				Local.Sort();
			});
		}
		const auto SameSheet = [&](int32 OwnTri, int32 HitTri)
		{
			const TArray<int32>& Local = LocalRoots[OwnTri];
			const FIndex3i T = AtlasMesh.GetTriangle(HitTri);
			for (int32 c = 0; c < 3; ++c)
			{
				if (Algo::BinarySearch(Local, WeldRoot[T[c]]) != INDEX_NONE)
				{
					return true;
				}
			}
			return false;
		};
		// 조각(같은 자리 정점으로 이은 삼각형 묶음) — 떨어진 자락 판정이 제 껍질의 안쪽 벽을 알아보는 데만 쓴다(층 판정은 위 SameSheet).
		TArray<int32> WeldPieceOfTri;
		WeldPieceOfTri.Init(INDEX_NONE, AtlasMesh.MaxTriangleID());
		{
			TArray<int32> Parent;
			Parent.SetNumUninitialized(AtlasMesh.MaxVertexID());
			for (int32 Vertex = 0; Vertex < Parent.Num(); ++Vertex)
			{
				Parent[Vertex] = Vertex;
			}
			const auto Find = [&Parent](int32 Vertex)
			{
				while (Parent[Vertex] != Vertex)
				{
					Parent[Vertex] = Parent[Parent[Vertex]];
					Vertex = Parent[Vertex];
				}
				return Vertex;
			};
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				const FIndex3i Corners = AtlasMesh.GetTriangle(Tri);
				const int32 A = Find(WeldRoot[Corners.A]);
				for (int32 c = 1; c < 3; ++c)
				{
					const int32 B = Find(WeldRoot[Corners[c]]);
					if (A != B)
					{
						Parent[B] = A;
					}
				}
			}
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				WeldPieceOfTri[Tri] = Find(WeldRoot[AtlasMesh.GetTriangle(Tri).A]);
			}
		}
		// 제 껍질의 안쪽 벽(일지 301 d): 칸 뒤에서 마주 보는 면이라도 같은 조각이고, 칸과 함께 봉투 밖에 떠 있거나(벽까지 거리 < 칸이 봉투 밖으로
		// 나온 거리) 칸 둘보다 얇으면(버클·닫힌 띠) 그 칸이 속한 닫힌 껍질의 뒷벽이라 막는 면이 아니다. 봉투 안 제 살끼리 마주 본 면(손가락 아래쪽,
		// 허벅지 안쪽)은 지나가지 않는다.
		const auto OwnShellWall = [&](int32 OwnTri, int32 HitTri, double Below, double Outside)
		{
			return WeldPieceOfTri[OwnTri] != INDEX_NONE && WeldPieceOfTri[OwnTri] == WeldPieceOfTri[HitTri]
				&& Below < FMath::Max(Outside, 2.0 * TexelSizeCm);
		};
		AliasTarget.Init(FVector2d(-1.0, -1.0), NumTexels);
		AliasTri.Init(INDEX_NONE, NumTexels);
		AliasBary.Init(FVector3d::ZeroVector, NumTexels);
		LayerRule.Init(0, NumTexels);
		LayerGap.Init(-1.f, NumTexels);
		LayerGapBuried.Init(0, NumTexels);
		BuriedDepth.Init(-1.f, NumTexels);
		FlapCandidate.Init(0, NumTexels);
		FlapOutcome.Init(0, NumTexels);
		LayerBehind.Init(0, NumTexels);
		EnvFromOther.Init(0, NumTexels);
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			// 차트 테두리 고스트(BuildGhosts)도 제 점(이음매 건너편 차트 위의 점)에서 가까운 층을 본다(일지 268). 그 점이 바깥 층에 덮였으면
			// 건너편 칸도 그 층을 따르는 고스트다. 예전에는 아래 253 규칙이 아틀라스에서 그 고스트를 따라가 이었는데, 펼침이 늘어난 자리(시험 캐릭터
			// 가슴 차트 안 작은 구멍)에서는 몇 mm 옆 점에 닿아, 그 칸을 읽는 피부가 손가락 벽에서 덜 눌려 속옷 밖으로 나왔다. 먼 층은 표면 칸만.
			const bool bBorderGhost = Atlas.Kind[Texel] == KindGhost;
			if (Atlas.Kind[Texel] != KindSurface && !bBorderGhost)
			{
				return;
			}
			// 제 점의 아틀라스 자리: 표면 칸은 칸 한가운데, 테두리 고스트는 가리키는 점.
			const FVector2d OwnTexel = bBorderGhost ? Atlas.Redirect[Texel] : FVector2d(Texel % R, Texel / R);
			const auto TexelOf = [&](int32 Tri, const FVector3d& Bary)
			{
				return AtlasUV(Atlas, Tri, Bary) * (double)R - FVector2d(0.5, 0.5);
			};
			// 가리킬 점 둘레 네 칸이 모두 있어야 고스트로 바꾼다(런타임이 그 네 칸을 쌍선형으로 읽는다).
			const auto Alias = [&](int32 Tri, const FVector3d& Bary, uint8 Rule)
			{
				const FVector2d Exact = TexelOf(Tri, Bary);
				const int32 BX = (int32)FMath::FloorToDouble(Exact.X);
				const int32 BY = (int32)FMath::FloorToDouble(Exact.Y);
				for (int32 Y = BY; Y <= BY + 1; ++Y)
				{
					for (int32 X = BX; X <= BX + 1; ++X)
					{
						if (X < 0 || Y < 0 || X >= R || Y >= R || Atlas.Kind[Atlas.Index(X, Y)] == KindEmpty)
						{
							return;
						}
					}
				}
				AliasTarget[Texel] = Exact;
				AliasTri[Texel] = Tri;
				AliasBary[Texel] = Bary;
				LayerRule[Texel] = Rule;
			};

			// 떠 있는 하드 서피스 조각의 칸: 밑 겉면(바깥 노멀 Below)의 점을 따른다 — 칸 그려지는 점에서 −Below 쪽 광선이 처음 맞는, 같은 쪽을
			// 보는 조각 아닌 면. 빗나가면(조각이 옷 끝 밖으로 걸침) 그 거리 안 가장 가까운 조각 아닌 점.
			if (Atlas.TexelTri[Texel] != INDEX_NONE && FloatingTri(Atlas.TexelTri[Texel]))
			{
				const FVector3d& Below = Hard.Below[AtlasPiece[Atlas.TexelTri[Texel]] - 1];
				if (Below.IsZero())
				{
					return;
				}
				IMeshSpatial::FQueryOptions Options;
				Options.MaxDistance = HardDrop;
				Options.TriangleFilterF = [&](int32 Tri) { return !FloatingTri(Tri); };
				TArray<MeshIntersection::FHitIntersectionResult> Hits;
				AtlasBVH.FindAllHitTriangles(FRay3d(TexelDrawn[Texel], -Below), Hits, Options);
				double Nearest = HardDrop;
				int32 BelowTri = INDEX_NONE;
				FVector3d BelowBary = FVector3d::ZeroVector;
				for (const MeshIntersection::FHitIntersectionResult& Hit : Hits)
				{
					if (Hit.Distance < Nearest && !FloatingTri(Hit.TriangleId) && AtlasMesh.GetTriNormal(Hit.TriangleId).Dot(Below) > 0.0)
					{
						Nearest = Hit.Distance;
						BelowTri = Hit.TriangleId;
						BelowBary = Hit.BaryCoords;
					}
				}
				if (BelowTri == INDEX_NONE)
				{
					double DistanceSq = 0.0;
					const int32 NearTri = AtlasBVH.FindNearestTriangle(TexelDrawn[Texel], DistanceSq, Options);
					if (NearTri != IndexConstants::InvalidID && FMath::Sqrt(DistanceSq) <= HardDrop)
					{
						BelowTri = NearTri;
						BelowBary = TMeshQueries<FDynamicMesh3>::TriangleDistance(AtlasMesh, NearTri, TexelDrawn[Texel]).TriangleBaryCoords;
					}
				}
				if (BelowTri != INDEX_NONE)
				{
					Alias(BelowTri, BelowBary, 5);
				}
				return;
			}

			// 바깥 층: 그려지는 면으로. 칸의 그려지는 점에서 미는 방향으로 봉투 칸 둘(NearLayer)까지 광선을 쏴 다른 면을 가까운 것부터 본다.
			//  - 뒤에서 맞은 면(면 노멀이 미는 방향과 45° 안) = 이 칸을 덮은 바깥 층 → 그 점을 따른다.
			//  - 앞에서 맞은 면(이 칸을 마주 봄)은 한 칸 안에서 뒤에서 맞는 면으로 빠져나가면 칸보다 얇은 장식(닫힌 띠·테)이라 그 바깥 면을 따르고,
			//    아니면 살끼리 마주 본 것(허벅지 안쪽, 손가락 사이, 가슴 밑 접힘)이라 거기서 멈춘다 — 그 너머의 면은 이 칸을 덮은 층이 아니다.
			//  - 같은 면(이웃 두 고리)·떠 있는 하드 조각·아틀라스에서 두 칸 안·LayerTie 안·비스듬한 면은 건너뛴다.
			// 전에는 ±봉투 칸 둘 안에서 같은 쪽을 보는 다른 메시 조각의 가장 바깥 면을 골랐다(일지 210). 마주 본 면에서 멈추지 않아 같은 조각을
			// 통째로 빼야 했고, 그래서 같은 자리 정점으로 용접된 겹친 층(가슴골을 건너는 판과 그 뒤 살갗)은 서로를 몰랐다.
			// 이 칸 밑 봉투 칸 둘 안에 같은 쪽을 보는 다른 면이 있으면 이 칸은 가까운 층의 바깥 층이다(아래 봉투 판정을 하지 않는다).
			// 따르는 칸은 바깥 층과 같은 값을 쓴다(고스트). 공기 간격이 있는 층에서는 바깥 층이 간격만큼 들어오기 전에도 같이 움직여, 덮인 칸과 안 덮인
			// 칸 사이가 찢어졌다(장식 옷 시험 캐릭터 가슴골 판 뒤 살갗, 간격 2~3 cm — 살 두께까지 넓혔던 첫 판). 그래서 따르기는 붙은 층(봉투 칸 둘 안)만이다.
			// 그보다 떨어진 매인 판(가슴골을 건너는 판)과 그 뒤 살은 각자 제 칸으로 눌린다 — 콜라이더가 판을 밀고 들어와 살에 닿으면 살도 눌린다.
			// 간격이 뒤 살 두께보다 크면 떨어진 자락이다(아래).
			const FVector3d& D = TexelDrawn[Texel];
			const FVector3d& N = TexelNormal[Texel];
			const int32 OwnTri = Atlas.TexelTri[Texel];
			bool bNearLayer = false;
			if (!N.IsZero() && OwnTri != INDEX_NONE)
			{
				const double Reach = NearLayer;
				TArray<MeshIntersection::FHitIntersectionResult> Hits;
				IMeshSpatial::FQueryOptions Options;
				Options.MaxDistance = NearLayer + Reach;
				AtlasBVH.FindAllHitTriangles(FRay3d(D - N * NearLayer, N), Hits, Options);
				struct FLayerHit
				{
					double Above;
					double Facing;
					int32 Tri;
					FVector3d Bary;
				};
				TArray<FLayerHit, TInlineAllocator<16>> Layers;
				for (const MeshIntersection::FHitIntersectionResult& Hit : Hits)
				{
					if (FloatingTri(Hit.TriangleId) || SameSheet(OwnTri, Hit.TriangleId)
						|| FVector2d::Distance(TexelOf(Hit.TriangleId, Hit.BaryCoords), OwnTexel) < 2.0)
					{
						continue;
					}
					const double Above = (DrawnPoint(AtlasMesh, Atlas, Drawn, Hit.TriangleId, Hit.BaryCoords, DrawnSurfaceCurve) - D).Dot(N);
					const double Facing = AtlasMesh.GetTriNormal(Hit.TriangleId).Dot(N);
					if (FMath::Abs(Above) <= LayerTie || Above > Reach)
					{
						continue;
					}
					if (Above < 0.0)
					{
						bNearLayer |= Above >= -NearLayer && Facing >= LayerFacing;
						continue;
					}
					Layers.Add(FLayerHit{Above, Facing, Hit.TriangleId, Hit.BaryCoords});
				}
				Layers.Sort([](const FLayerHit& A, const FLayerHit& B) { return A.Above < B.Above; });
				int32 Outer = INDEX_NONE;
				for (int32 h = 0; h < Layers.Num() && Outer == INDEX_NONE; ++h)
				{
					if (Layers[h].Facing >= LayerFacing)
					{
						Outer = h;
					}
					else if (Layers[h].Facing <= -LayerFacing)
					{
						for (int32 k = h + 1; k < Layers.Num() && Layers[k].Above <= Layers[h].Above + TexelSizeCm; ++k)
						{
							if (Layers[k].Facing >= LayerFacing)
							{
								Outer = k;
								break;
							}
						}
						break;
					}
				}
				if (Outer != INDEX_NONE)
				{
					Alias(Layers[Outer].Tri, Layers[Outer].Bary, bBorderGhost ? 4 : 1);
					LayerGap[Texel] = (float)Layers[Outer].Above;
					LayerGapBuried[Texel] = (TexelSourcePoint[Texel] - TexelPosition[Texel]).Dot(N) <= -Margin ? 1 : 0;
					return;
				}
				if (bNearLayer && !bBorderGhost)
				{
					LayerRule[Texel] = 2;
				}
			}
			if (bBorderGhost)
			{
				return;
			}

			// 봉투 안에 묻힌 칸(속 메시 껍질의 안쪽 벽처럼, ADR-0028): 봉투 점에서 가장 가까운 아틀라스 점의 고스트 — 가려진 안쪽 면이 바깥 면을 따른다.
			// 봉투 밖으로 나온 칸(봉투가 담지 못한 얇은 판·띠·자락)에는 하지 않는다. 전에는 방향을 보지 않아 이런 칸이 뒤 맨살의 고스트가 되었다
			// (합성 겹친 가슴골 조각의 띠 칸 94 %, 장식 옷 시험 캐릭터 브라 컵 정점 1/4이 0.3~2 cm 뒤 점을 따름). 한 칸 넘게 나온 칸은 떨어진 자락 판정으로 간다.
			const FVector3d& P = TexelPosition[Texel];
			const double Outside = (TexelSourcePoint[Texel] - P).Dot(N);
			const bool bBuried = Outside <= -Margin;
			if (!bBuried)
			{
				FlapCandidate[Texel] = Outside > TexelSizeCm && !N.IsZero() && OwnTri != INDEX_NONE ? 1 : 0;
			}
			const double Own = FVector3d::Distance(P, TexelSourcePoint[Texel]);
			double DistanceSq = 0.0;
			IMeshSpatial::FQueryOptions FarOptions;
			if (AtlasPiece.Num() > 0)
			{
				FarOptions.TriangleFilterF = [&](int32 Tri) { return !FloatingTri(Tri); };
			}
			const int32 NearTri = AtlasBVH.FindNearestTriangle(P, DistanceSq, FarOptions);
			if (NearTri == IndexConstants::InvalidID || FMath::Sqrt(DistanceSq) > Own - Margin)
			{
				return;
			}
			const FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(AtlasMesh, NearTri, P);
			if (FVector2d::Distance(TexelOf(NearTri, Query.TriangleBaryCoords), OwnTexel) < 2.0
				|| (bNearLayer && FVector3d::Distance(Query.ClosestTrianglePoint, D) <= NearLayer))
			{
				return;
			}
			// 봉투 밖(또는 봉투 위)인데 봉투 점이 다른 면의 것이면 고스트로 만들지 않고 표시만 한다(일지 301 g2). 얇은 판 봉투 껍질(몸에서 떨어진
			// 치마)의 안쪽 면이 가장 가까우면 미는 방향이 몸 쪽이다 — 장식 옷 시험 캐릭터 앞 허리 장식 47칸은 298에서 이 조건으로 0.4~0.5 cm 뒤 치마
			// 안벽의 고스트였고(그때도 방향은 −n), 위 봉투 안 조건(일지 300)으로 표면 칸이 되자 감김 판정에 들어가 판에서 빠졌다.
			if (!bBuried)
			{
				EnvFromOther[Texel] = 1;
				return;
			}
			Alias(NearTri, Query.TriangleBaryCoords, 3);
			BuriedDepth[Texel] = (float)Own;
		});
		// 떨어진 자락(일지 300, principles-reference "같은 자리·층"): 봉투 밖으로 한 칸 넘게 나와 바깥 층으로 남은 칸은 뒤(−미는 방향)를 본다. 처음
		// 맞은 마주 본 면(밑 몸의 겉면)까지 간격이 그 자리 살 두께(칠 전)보다 크면 몸에서 떨어진 자락이다 — 접촉은 천 시뮬레이션의 몫이고 밑 몸을
		// 따르는 고스트가 된다(전에 먼 층 규칙이 방향 없이 하던 일을 이 조건에서만). 간격이 살 두께 안이면 매인 바깥 층이라 제 칸으로 눌림을 받는다
		// (봉투 칸 둘 안이면 밑 살이 위 바깥 층 판정으로 이 칸을 따르고, 그보다 멀면 밑 살도 제 칸이다). 밑 몸 칸이 이미 이 칸의 차트를 따르면 따르지 않는다.
		// 경계를 간격에 따라 서서히 바꾸는 것은 아직 하지 않는다(칸마다 둘 중 하나).
		// 닫힌 옷 껍질(일지 301 d): 게임 옷은 1~1.5 cm 두께로 닫혀 있어, 뒤로 쏜 광선이 먼저 제 껍질의 안쪽 벽(마주 보는 면)에 닿아 막혔다 — 시험 캐릭터
		// 소매·치마 칸이 몸에서 5~33 cm 떨어진 채 제 칸으로 눌렸다(Cell points moved 95 % 0.33 -> 5.22 cm). 제 껍질 벽(OwnShellWall)은 지나간다.
		// 몸에 붙은 옷은 그대로 매인 바깥 층이다 — 간격이 밑 살 두께 안이면 자락이 아니고, 다른 조각의 마주 본 면과 봉투 안 제 살은 여전히 막는다.
		{
			const TArray<int32> AliasTri0 = AliasTri;
			ParallelFor(NumTexels, [&](int32 Texel)
			{
				if (!FlapCandidate[Texel] || AliasTri0[Texel] != INDEX_NONE)
				{
					return;
				}
				const FVector3d& D = TexelDrawn[Texel];
				const FVector3d& N = TexelNormal[Texel];
				const int32 OwnTri = Atlas.TexelTri[Texel];
				const FVector2d OwnTexel(Texel % R, Texel / R);
				const double Outside = (TexelSourcePoint[Texel] - TexelPosition[Texel]).Dot(N);
				const auto TexelOf = [&](int32 Tri, const FVector3d& Bary)
				{
					return AtlasUV(Atlas, Tri, Bary) * (double)R - FVector2d(0.5, 0.5);
				};
				TArray<MeshIntersection::FHitIntersectionResult> Hits;
				IMeshSpatial::FQueryOptions Options;
				Options.MaxDistance = FVector3d::Distance(TexelPosition[Texel], TexelSourcePoint[Texel]) + 2.0 * NearLayer;
				AtlasBVH.FindAllHitTriangles(FRay3d(D, -N), Hits, Options);
				double Gap = TNumericLimits<double>::Max();
				int32 BodyTri = INDEX_NONE;
				FVector3d BodyBary = FVector3d::ZeroVector;
				double BlockedAt = TNumericLimits<double>::Max();
				double PassedAt = TNumericLimits<double>::Max();
				for (const MeshIntersection::FHitIntersectionResult& Hit : Hits)
				{
					if (FloatingTri(Hit.TriangleId) || SameSheet(OwnTri, Hit.TriangleId)
						|| FVector2d::Distance(TexelOf(Hit.TriangleId, Hit.BaryCoords), OwnTexel) < 2.0)
					{
						continue;
					}
					const double Below = (D - DrawnPoint(AtlasMesh, Atlas, Drawn, Hit.TriangleId, Hit.BaryCoords, DrawnSurfaceCurve)).Dot(N);
					const double Facing = AtlasMesh.GetTriNormal(Hit.TriangleId).Dot(N);
					if (Below <= LayerTie)
					{
						continue;
					}
					if (Facing <= -LayerFacing)
					{
						if (OwnShellWall(OwnTri, Hit.TriangleId, Below, Outside))
						{
							PassedAt = FMath::Min(PassedAt, Below);
							continue;
						}
						BlockedAt = FMath::Min(BlockedAt, Below);
					}
					else if (Facing >= LayerFacing && Below < Gap)
					{
						Gap = Below;
						BodyTri = Hit.TriangleId;
						BodyBary = Hit.BaryCoords;
					}
				}
				if (BodyTri == INDEX_NONE || BlockedAt < Gap)
				{
					FlapOutcome[Texel] = (uint8)(BlockedAt < Gap ? 3 : 4);
					return;
				}
				LayerBehind[Texel] = 1;
				const FVector2d Exact = TexelOf(BodyTri, BodyBary);
				const int32 BX = (int32)FMath::FloorToDouble(Exact.X);
				const int32 BY = (int32)FMath::FloorToDouble(Exact.Y);
				const int32 HX = FMath::Clamp((int32)FMath::RoundToDouble(Exact.X), 0, R - 1);
				const int32 HY = FMath::Clamp((int32)FMath::RoundToDouble(Exact.Y), 0, R - 1);
				const int32 Body = Atlas.Index(HX, HY);
				if (Atlas.Kind[Body] == KindEmpty)
				{
					FlapOutcome[Texel] = 5;
					return;
				}
				if (Gap <= FMath::Max(TexelCoreDepth[Body], NearLayer))
				{
					FlapOutcome[Texel] = 2;
					return;
				}
				if (AliasTri0[Body] != INDEX_NONE && Atlas.TriChart[AliasTri0[Body]] == Atlas.TriChart[OwnTri])
				{
					FlapOutcome[Texel] = 5;
					return;
				}
				for (int32 Y = BY; Y <= BY + 1; ++Y)
				{
					for (int32 X = BX; X <= BX + 1; ++X)
					{
						if (X < 0 || Y < 0 || X >= R || Y >= R || Atlas.Kind[Atlas.Index(X, Y)] == KindEmpty)
						{
							FlapOutcome[Texel] = 5;
							return;
						}
					}
				}
				AliasTarget[Texel] = Exact;
				AliasTri[Texel] = BodyTri;
				AliasBary[Texel] = BodyBary;
				LayerRule[Texel] = 6;
				FlapOutcome[Texel] = (uint8)(PassedAt < Gap ? 6 : 1);
			});
			int32 NumOutcome[7] = {0, 0, 0, 0, 0, 0, 0};
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				++NumOutcome[FlapOutcome[Texel]];
			}
			const int32 NumFlapCandidates = NumOutcome[1] + NumOutcome[2] + NumOutcome[3] + NumOutcome[4] + NumOutcome[5] + NumOutcome[6];
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Detached flaps: of %d cells more than a cell outside the outer surface, %d follow the body under them (%d of them through their own closed cloth wall), %d are outer layers held within the flesh thickness under them, %d stop at another surface facing back, %d find no surface behind within reach, %d found a body cell they cannot copy"),
				NumFlapCandidates, NumOutcome[1] + NumOutcome[6], NumOutcome[6], NumOutcome[2], NumOutcome[3], NumOutcome[4], NumOutcome[5]);
		}
		// 밑 겉면을 못 찾은 장식 칸(장식 끝이 밑 옷에서 HardDrop 넘게 떨어짐, fb/orn 안 A): 같은 조각에서 밑 겉면을 찾은 가장 가까운 칸(그려지는 점 3-D
		// 거리)의 밑 점을 빌려 그 점의 고스트가 된다 — 함몰·끌림은 그 점에서 가져오고, 칸 위치는 제 장식 점 그대로라 판 자세를 제 자리의 판 좌표에서 읽는다
		// (장식 끝이 판과 한 몸으로 기운다). 살 칸으로 남기면 밑에 살이 없는 자리에 손 압력 칸이 생기고 장식 끝이 판과 따로 휜다.
		int32 NumHardBorrowed = 0;
		if (OrnamentPiece.Num() == NumTexels)
		{
			TMap<int32, TArray<int32>> AttachedOf;
			TArray<int32> Loose;
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				if (OrnamentPiece[Texel] <= 0)
				{
					continue;
				}
				if (LayerRule[Texel] == 5 && AliasTri[Texel] != INDEX_NONE)
				{
					AttachedOf.FindOrAdd(OrnamentPiece[Texel]).Add(Texel);
				}
				else if (Atlas.Kind[Texel] == KindSurface && AliasTri[Texel] == INDEX_NONE)
				{
					Loose.Add(Texel);
				}
			}
			TArray<int32> Borrow;
			Borrow.Init(INDEX_NONE, Loose.Num());
			ParallelFor(Loose.Num(), [&](int32 i)
			{
				const int32 Texel = Loose[i];
				const TArray<int32>* Attached = AttachedOf.Find(OrnamentPiece[Texel]);
				if (!Attached)
				{
					return;
				}
				double NearestSq = TNumericLimits<double>::Max();
				for (const int32 Other : *Attached)
				{
					const double D = FVector3d::DistSquared(TexelDrawn[Other], TexelDrawn[Texel]);
					if (D < NearestSq)
					{
						NearestSq = D;
						Borrow[i] = Other;
					}
				}
			});
			for (int32 i = 0; i < Loose.Num(); ++i)
			{
				const int32 From = Borrow[i];
				if (From == INDEX_NONE)
				{
					continue;
				}
				const int32 Texel = Loose[i];
				AliasTarget[Texel] = AliasTarget[From];
				AliasTri[Texel] = AliasTri[From];
				AliasBary[Texel] = AliasBary[From];
				LayerRule[Texel] = 5;
				OrnamentOwnPoint[Texel] = 1;
				++NumHardBorrowed;
			}
		}
		int32 Aliased = 0;
		int32 NumRule[7] = {0, 0, 0, 0, 0, 0, 0};
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			++NumRule[LayerRule[Texel]];
			if (AliasTri[Texel] != INDEX_NONE)
			{
				Atlas.Kind[Texel] = KindGhost;
				Atlas.Redirect[Texel] = AliasTarget[Texel];
				// 가리키는 층과 같은 점에서 계산해 두 층이 같은 함몰을 낸다(안쪽 층 정점은 제 간격만큼 아래에 남는다). 밑 점을 빌린 장식 칸은 제 점 그대로(위).
				if (OrnamentOwnPoint.Num() == 0 || OrnamentOwnPoint[Texel] == 0)
				{
					TexelDrawn[Texel] = DrawnPoint(AtlasMesh, Atlas, Drawn, AliasTri[Texel], AliasBary[Texel], DrawnSurfaceCurve);
				}
				Aliased += LayerRule[Texel] != 4 && LayerRule[Texel] != 5 ? 1 : 0;
			}
			else if (AtlasPiece.Num() > 0 && Atlas.Kind[Texel] == KindSurface && Atlas.TexelTri[Texel] != INDEX_NONE && FloatingTri(Atlas.TexelTri[Texel]))
			{
				++NumHardUnattached;
			}
		}
		if (AtlasPiece.Num() > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: %d cells of floating rigid pieces follow the surface below them (%d of them found no surface within %.2f cm and follow the nearest cell of the same piece that did, keeping their own place)%s"),
				NumRule[5], NumHardBorrowed, HardDrop,
				NumHardUnattached > 0 ? *FString::Printf(TEXT(", %d found nothing to follow and stay on the piece (they bend with it)"), NumHardUnattached) : TEXT(""));
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Layered clothing: of %d surface cells, %d (%.1f%%) follow another surface - %d under an outer layer within %.2f cm (two outer surface cells, by the drawn surfaces; %d cells are such an outer layer), %d buried inside the outer surface (by the outer surface), %d on detached flaps follow the body under them; island border cells under an outer layer: %d"),
			NumSurface, Aliased, 100.0 * Aliased / FMath::Max(NumSurface, 1), NumRule[1], NearLayer, NumRule[2], NumRule[3], NumRule[6], NumRule[4]);
		// 덮인 칸(규칙 1·4, 끝내 고스트가 된 칸)의 층 간격 분포(일지 332). 칸 판정은 바꾸지 않는다.
		{
			TArray<float> Gaps;
			TArray<float> BuriedGaps;   // 덮인 칸 중 봉투 안에 묻힌 칸의 간격
			TArray<float> Depths;       // 묻힌 칸(규칙 3)의 봉투까지 거리
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				if ((LayerRule[Texel] == 1 || LayerRule[Texel] == 4) && AliasTri[Texel] != INDEX_NONE && LayerGap[Texel] >= 0.f)
				{
					Gaps.Add(LayerGap[Texel]);
					if (LayerGapBuried[Texel])
					{
						BuriedGaps.Add(LayerGap[Texel]);
					}
				}
				if (LayerRule[Texel] == 3 && AliasTri[Texel] != INDEX_NONE && BuriedDepth[Texel] >= 0.f)
				{
					Depths.Add(BuriedDepth[Texel]);
				}
			}
			if (Depths.Num() > 0)
			{
				Depths.Sort();
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Layered clothing buried: %d cells inside the outer surface follow it, depth below it 50/90/99%%/max %.2f/%.2f/%.2f/%.2f cm"),
					Depths.Num(), Depths[Depths.Num() / 2], Depths[Depths.Num() * 9 / 10], Depths[FMath::Min(Depths.Num() * 99 / 100, Depths.Num() - 1)], Depths.Last());
			}
			if (Gaps.Num() > 0)
			{
				Gaps.Sort();
				const auto At = [&Gaps](double Q) { return Gaps[FMath::Clamp((int32)(Q * Gaps.Num()), 0, Gaps.Num() - 1)]; };
				const auto Over = [&Gaps](float Limit) { int32 N = 0; for (const float G : Gaps) { N += G > Limit ? 1 : 0; } return N; };
				const float MaxGap = Data ? Data->LayerMaxGap : 0.f;
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Layered clothing gap: %d covered cells, gap 50/90/99%%/max %.2f/%.2f/%.2f/%.2f cm; over 0.5/0.75/1.0/1.25/1.5 cm: %d/%d/%d/%d/%d; over this asset's Layer Max Gap %.2f cm: %d (%.1f%%)"),
					Gaps.Num(), At(0.5), At(0.9), At(0.99), Gaps.Last(), Over(0.5f), Over(0.75f), Over(1.0f), Over(1.25f), Over(1.5f),
					MaxGap, Over(MaxGap), 100.0 * Over(MaxGap) / Gaps.Num());
				int32 OverBuried = 0;
				for (const float G : BuriedGaps)
				{
					OverBuried += G > MaxGap ? 1 : 0;
				}
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Layered clothing gap: %d covered cells sit inside the outer surface; of the %d over Layer Max Gap, %d do (the buried rule would still make them follow)"),
					BuriedGaps.Num(), Over(MaxGap), OverBuried);
			}
		}

		// 고스트는 둘레에 표면 칸이 있는 자리를 가리켜야 한다(일지 253). 런타임은 고스트 칸의 값을 가리킨 점에서 가져오는데(함몰은 둘레 네 칸,
		// 국소 모양·끌림·거친 함몰은 둘레 거친 칸) 그 값은 표면 칸에서만 풀린다. 차트 테두리 고스트(BuildGhosts)는 위 판정 전에 옆 차트의 점을
		// 가리켰는데 그 칸이 이제 바깥 층을 따르는 고스트(속옷에 덮인 피부)일 수 있고, 덮인 피부 조각 둘이 서로를 가리키기도 한다. 그런 고스트는
		// 값이 0이거나 서로를 베껴, 그 자리 피부가 바깥 층을 따르지 않고 뚫고 나왔다(시험 캐릭터 젖꼭지 둘레 피부 섬의 테두리가 누르면 속옷 위로
		// 고리처럼 비쳤다). 가리킨 점 둘레 네 칸에 표면 칸이 없는 고스트는 차례로:
		//  1. 가장 가까운 칸(고스트)이 가리키는 점으로 따라간다(겹겹이면 몇 번, 둘레 네 칸이 모두 같은 곳을 가리키면 쌍선형으로 섞어). 제 자리 둘레로
		//     돌아오는 길은 표면 칸에 닿고 3-D로도 그 자리일 때만 따른다(얇은 껍질이 접힌 테두리, 일지 254).
		//  2. 2칸 안 가장 가까운 표면 칸 — 보통 이음매에서 가리킨 점이 옆 차트 표면 칸 바로 바깥(그 차트의 테두리 고스트 위)에 떨어진 경우.
		//  3. 둘레에 표면 칸이 있는 자리를 가리키는 옆 고스트(같은 차트 3칸 안, 가장 가까운) — 서로를 가리키는 고리.
		//  4. (되풀이 뒤에도 남으면) 3-D에서 가장 가까운 같은 쪽 표면 칸 — 사슬이 고스트끼리만 도는 고리(Peasant 옷 밑 피부, 일지 254). 먼 옷 밑이면 거리 제한 없이.
		// 몇 바퀴 되풀이한다(고리 한쪽이 3으로 풀리면 다른 쪽이 1로 풀린다). 위치(그려지는 점)도 따라간 칸 것을 쓴다. 렌더 삼각형·덩어리 값은 제 것
		// 그대로(겹친 층 고스트와 같다).
		{
			const auto Clamped = [R](int32 X, int32 Y) { return FIntPoint(FMath::Clamp(X, 0, R - 1), FMath::Clamp(Y, 0, R - 1)); };
			const auto Hosted = [&](const FVector2d& T)
			{
				const int32 BX = (int32)FMath::FloorToDouble(T.X);
				const int32 BY = (int32)FMath::FloorToDouble(T.Y);
				for (int32 k = 0; k < 4; ++k)
				{
					const FIntPoint C = Clamped(BX + (k & 1), BY + (k >> 1));
					if (Atlas.Kind[Atlas.Index(C.X, C.Y)] == KindSurface)
					{
						return true;
					}
				}
				return false;
			};
			int32 NumLost = 0;
			int32 NumHow[4] = {0, 0, 0, 0};
			for (int32 Round = 0; Round < 4; ++Round)
			{
				const TArray<FVector2d> Redirect0 = Atlas.Redirect;
				const TArray<FVector3d> Drawn0 = TexelDrawn;
				TArray<int32> Lost;
				for (int32 Texel = 0; Texel < NumTexels; ++Texel)
				{
					if (Atlas.Kind[Texel] == KindGhost && !Hosted(Redirect0[Texel]))
					{
						Lost.Add(Texel);
					}
				}
				if (Round == 0)
				{
					NumLost = Lost.Num();
				}
				if (Lost.Num() == 0)
				{
					break;
				}
				TArray<FVector2d> NewTarget;
				NewTarget.SetNum(Lost.Num());
				TArray<FVector3d> NewDrawn;
				NewDrawn.SetNum(Lost.Num());
				TArray<uint8> How;
				How.Init(0, Lost.Num());
				ParallelFor(Lost.Num(), [&](int32 i)
				{
					const int32 G = Lost[i];
					const FVector2d Own(G % R, G / R);
					// 1. 따라간다.
					FVector2d T = Redirect0[G];
					for (int32 Hop = 0; Hop < 8; ++Hop)
					{
						const FIntPoint NP = Clamped((int32)FMath::RoundToDouble(T.X), (int32)FMath::RoundToDouble(T.Y));
						const int32 N = Atlas.Index(NP.X, NP.Y);
						if (N == G || Atlas.Kind[N] != KindGhost)
						{
							break;
						}
						const FVector2d Next = Redirect0[N];
						const int32 BX = (int32)FMath::FloorToDouble(T.X);
						const int32 BY = (int32)FMath::FloorToDouble(T.Y);
						const double FX = FMath::Clamp(T.X - BX, 0.0, 1.0);
						const double FY = FMath::Clamp(T.Y - BY, 0.0, 1.0);
						FVector2d Mixed = FVector2d::ZeroVector;
						FVector3d MixedPoint = FVector3d::ZeroVector;
						bool bMix = true;
						for (int32 k = 0; k < 4 && bMix; ++k)
						{
							const FIntPoint C = Clamped(BX + (k & 1), BY + (k >> 1));
							const int32 CT = Atlas.Index(C.X, C.Y);
							bMix = CT != G && Atlas.Kind[CT] == KindGhost && FVector2d::Distance(Redirect0[CT], Next) < 3.0;
							const double W = ((k & 1) ? FX : 1.0 - FX) * ((k >> 1) ? FY : 1.0 - FY);
							Mixed += W * Redirect0[CT];
							MixedPoint += W * Drawn0[CT];
						}
						const FVector2d Candidate = bMix ? Mixed : Next;
						const FVector3d CandidatePoint = bMix ? MixedPoint : Drawn0[N];
						// 제 자리 둘레로 돌아오는 길: 표면 칸에 닿고 3-D로도 그 자리면 따른다(얇은 껍질 테두리가 접혀 뒷면이 앞면을 따르는
						// 곳 — Shell). 아니면 고리라 멈춘다.
						const bool bBack = FVector2d::Distance(Candidate, Own) < 4.0;
						if (Hosted(Candidate) && (!bBack || FVector3d::Distance(CandidatePoint, Drawn0[G]) <= 2.0 * Cell))
						{
							NewTarget[i] = Candidate;
							NewDrawn[i] = CandidatePoint;
							How[i] = 1;
							return;
						}
						if (bBack)
						{
							break;
						}
						T = Candidate;
					}
					// 2. 2칸 안 가장 가까운 표면 칸.
					const FVector2d First = Redirect0[G];
					double Best = 2.5 * 2.5;
					const int32 CX = (int32)FMath::RoundToDouble(First.X);
					const int32 CY = (int32)FMath::RoundToDouble(First.Y);
					for (int32 DY = -2; DY <= 2; ++DY)
					{
						for (int32 DX = -2; DX <= 2; ++DX)
						{
							const int32 X = CX + DX;
							const int32 Y = CY + DY;
							if (X < 0 || Y < 0 || X >= R || Y >= R || Atlas.Kind[Atlas.Index(X, Y)] != KindSurface)
							{
								continue;
							}
							const double D = FMath::Square(X - First.X) + FMath::Square(Y - First.Y);
							if (D < Best)
							{
								Best = D;
								NewTarget[i] = FVector2d(X, Y);
								NewDrawn[i] = Drawn0[Atlas.Index(X, Y)];
								How[i] = 2;
							}
						}
					}
					if (How[i] != 0)
					{
						return;
					}
					// 3. 둘레에 표면 칸이 있는 자리를 가리키는 옆 고스트(같은 자리 근처 — 아틀라스에서 옆에 놓였을 뿐인 다른 차트는 뺀다).
					Best = TNumericLimits<double>::Max();
					for (int32 DY = -3; DY <= 3; ++DY)
					{
						for (int32 DX = -3; DX <= 3; ++DX)
						{
							const int32 X = G % R + DX;
							const int32 Y = G / R + DY;
							if ((DX == 0 && DY == 0) || X < 0 || Y < 0 || X >= R || Y >= R)
							{
								continue;
							}
							const int32 M = Atlas.Index(X, Y);
							if (Atlas.Kind[M] != KindGhost || !Hosted(Redirect0[M]) || FVector3d::Distance(Drawn0[M], Drawn0[G]) > 2.0 * Cell)
							{
								continue;
							}
							const double D = DX * DX + DY * DY;
							if (D < Best)
							{
								Best = D;
								NewTarget[i] = Redirect0[M];
								NewDrawn[i] = Drawn0[M];
								How[i] = 3;
							}
						}
					}
				});
				int32 Changed = 0;
				for (int32 i = 0; i < Lost.Num(); ++i)
				{
					if (How[i] == 0)
					{
						continue;
					}
					Atlas.Redirect[Lost[i]] = NewTarget[i];
					TexelDrawn[Lost[i]] = NewDrawn[i];
					++NumHow[How[i]];
					++Changed;
				}
				if (Changed == 0)
				{
					break;
				}
			}
			// 4. 그래도 남은 고스트는 사슬이 고스트끼리만 돈다 — 덮인 조각이 서로를, 또는 제 테두리 고스트를 가리킨다(Peasant 옷 밑 피부).
			//    아틀라스로는 표면 칸에 닿을 길이 없으니 3-D에서 가장 가까운 표면 칸(그려지는 점 사이, 노멀이 같은 쪽)을 가리킨다 — 그 자리를
			//    덮은 바깥 층이다. 먼저 가까운 층 간격의 두 배 안에서 찾고, 없으면 거리 제한 없이.
			TArray<int32> Left;
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				if (Atlas.Kind[Texel] == KindGhost && !Hosted(Atlas.Redirect[Texel]))
				{
					Left.Add(Texel);
				}
			}
			if (Left.Num() > 0)
			{
				const double Reach = 2.0 * NearLayer;
				const auto KeyOf = [Reach](const FVector3d& P)
				{
					return FIntVector(FMath::FloorToInt32(P.X / Reach), FMath::FloorToInt32(P.Y / Reach), FMath::FloorToInt32(P.Z / Reach));
				};
				TMap<FIntVector, TArray<int32>> Buckets;
				for (int32 Texel = 0; Texel < NumTexels; ++Texel)
				{
					if (Atlas.Kind[Texel] == KindSurface)
					{
						Buckets.FindOrAdd(KeyOf(TexelDrawn[Texel])).Add(Texel);
					}
				}
				TArray<int32> Nearest;
				Nearest.Init(INDEX_NONE, Left.Num());
				ParallelFor(Left.Num(), [&](int32 i)
				{
					const int32 G = Left[i];
					const FVector3d& Point = TexelDrawn[G];
					const FVector3d& Facing = TexelNormal[G];
					const FIntVector Key = KeyOf(Point);
					double Best = Reach * Reach;
					for (int32 DZ = -1; DZ <= 1; ++DZ)
					{
						for (int32 DY = -1; DY <= 1; ++DY)
						{
							for (int32 DX = -1; DX <= 1; ++DX)
							{
								const TArray<int32>* Bucket = Buckets.Find(Key + FIntVector(DX, DY, DZ));
								if (!Bucket)
								{
									continue;
								}
								for (const int32 S : *Bucket)
								{
									const double D = FVector3d::DistSquared(TexelDrawn[S], Point);
									if (D < Best && (Facing.IsZero() || Facing.Dot(TexelNormal[S]) > 0.0))
									{
										Best = D;
										Nearest[i] = S;
									}
								}
							}
						}
					}
				});
				// 그 안에 없으면(먼 층 아래 — 몸에서 몇 cm 떨어진 치마 밑 다리처럼) 거리 제한 없이 가장 가까운 같은 쪽 표면 칸. 겹친 층 판정이 먼 층을
				// 거리 제한 없이 봉투로 고르는 것과 같은 뜻이다. 남은 고스트만 모든 표면 칸과 견준다(굽기에서 한 번).
				TArray<int32> Surfaces;
				for (int32 Texel = 0; Texel < NumTexels; ++Texel)
				{
					if (Atlas.Kind[Texel] == KindSurface)
					{
						Surfaces.Add(Texel);
					}
				}
				ParallelFor(Left.Num(), [&](int32 i)
				{
					if (Nearest[i] != INDEX_NONE)
					{
						return;
					}
					const FVector3d& Point = TexelDrawn[Left[i]];
					const FVector3d& Facing = TexelNormal[Left[i]];
					double Best = TNumericLimits<double>::Max();
					for (const int32 S : Surfaces)
					{
						const double D = FVector3d::DistSquared(TexelDrawn[S], Point);
						if (D < Best && (Facing.IsZero() || Facing.Dot(TexelNormal[S]) > 0.0))
						{
							Best = D;
							Nearest[i] = S;
						}
					}
				});
				for (int32 i = 0; i < Left.Num(); ++i)
				{
					if (Nearest[i] != INDEX_NONE)
					{
						Atlas.Redirect[Left[i]] = FVector2d(Nearest[i] % R, Nearest[i] / R);
						TexelDrawn[Left[i]] = TexelDrawn[Nearest[i]];
						++NumHow[0];
					}
				}
			}
			int32 Stuck = 0;
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				Stuck += Atlas.Kind[Texel] == KindGhost && !Hosted(Atlas.Redirect[Texel]) ? 1 : 0;
			}
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Cells that copy another cell: %d pointed where no surface cell is (at another such cell); now %d follow the chain to a surface cell, %d use the nearest surface cell, %d copy a neighbor, %d use the nearest surface cell in 3-D (their chain only loops through such cells); %d left"),
				NumLost, NumHow[1], NumHow[2], NumHow[3], NumHow[0], Stuck);
		}

		// 미는 방향은 면마다가 아니라 그 자리의 성질이다(일지 300, principles-reference "같은 자리·층" 미결 후보를 합성 겹친 가슴골 조각으로 정함).
		// 봉투는 봉투 칸 격자에서 다시 뜬 껍질이라 그보다 얇은 바깥 층(가슴골을 건너는 띠·판, 그 위 버클)을 담지 못한다. 그런 칸의 미는 방향은 가장
		// 가까운 봉투 점(가슴골 옆 벽 등)에서 와서 한 층 안에서도 칸마다 45°쯤 흩어졌고(합성 조각 띠: 층 평균에서 중앙 44°), 버클은 칸 미는 방향이
		// 제 면 노멀과 60° 어긋났다. 위 겹친 층 고침으로 띠·버클이 제 칸으로 직접 눌리자 흩어진 방향만큼 늘어나 찢겼다.
		//  1. 봉투 밖으로 한 칸 넘게 나온 표면 칸: 둘레 바깥 면(덮이지 않은 면)의 그려지는 노멀을 봉투 미는 방향과 같은 반경(두께 ×
		//     PushDirectionSmoothing)에서 가우스 무게(넓이 × 기준 방향과 같은 쪽 정도)로 평균한다. 기준을 그 평균으로 바꿔 한 번 더 — 버클 옆면처럼
		//     제 면이 옆을 봐도 둘레 바깥 면이 앞을 보면 앞이다. 봉투에서 한~두 칸 사이는 봉투 미는 방향과 섞어 잇는다.
		//  2. 다른 면을 따르는 칸(바깥 층에 덮인 칸, 봉투 안에 묻힌 칸, 떨어진 자락): 따르는 점의 미는 방향 — 같은 변위로 움직여 층 사이가 그대로다
		//     (버클·띠의 뒷면, 판 뒤 살갗).
		{
			TArray<FVector3d> FaceCenter;
			TArray<FVector3d> FaceNormal;
			TArray<double> FaceArea;
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				const FVector2d C = AtlasUV(Atlas, Tri, FVector3d(1.0 / 3.0)) * (double)R;
				const int32 X = FMath::Clamp((int32)FMath::FloorToDouble(C.X), 0, R - 1);
				const int32 Y = FMath::Clamp((int32)FMath::FloorToDouble(C.Y), 0, R - 1);
				if (Atlas.Kind[Atlas.Index(X, Y)] != KindSurface || FloatingTri(Tri))
				{
					continue;
				}
				FaceCenter.Add(AtlasMesh.GetTriCentroid(Tri));
				FaceNormal.Add(AtlasMesh.GetTriNormal(Tri));
				FaceArea.Add(AtlasMesh.GetTriArea(Tri));
			}
			FPointGrid FaceGrid;
			FaceGrid.Build(FaceCenter, 8.0 * TexelSizeCm);
			const TArray<FVector3d> EnvelopePush = TexelNormal;
			int32 NumOuter = 0;
			FThreadSafeCounter NumNoLayer;
			FThreadSafeCounter NumMarked;
			TArray<double> BorrowedTurn;   // 봉투 방향을 버린 칸: 고친 뒤 미는 방향과 제 바깥 면 노멀 사이 각
			// 칸 제 면으로 섞는 면의 쪽을 정하는 문턱: 제 면이 봉투 방향(또는 다른 면에서 멀어지는 방향)과 60° 안팎일 때만.
			const double SideFacing = 0.5;
			TArray<double> Turned;
			FCriticalSection TurnedLock;
			ParallelFor(NumTexels, [&](int32 Texel)
			{
				if (Atlas.Kind[Texel] != KindSurface || EnvelopePush[Texel].IsZero())
				{
					return;
				}
				const FVector3d& Env = EnvelopePush[Texel];
				const double Outside = (TexelSourcePoint[Texel] - TexelPosition[Texel]).Dot(Env);
				const double OutsideBlend = FMath::Clamp((Outside - TexelSizeCm) / TexelSizeCm, 0.0, 1.0);
				// 섞는 면의 쪽(일지 301 g2): 칸 제 그린 면의 바깥 쪽을 보는 면만 섞는다. 기준 방향과 같은 쪽만 보면 첫 걸음의 기준이 옆으로 기운
				// 칸에서 둘째 걸음이 얇은 판의 뒷장(반대를 보는 면)으로 넘어갈 수 있다. 제 면은 봉투 방향 쪽으로 뒤집어 쓴다 — 양면 장식의 뒷장 칸은
				// 제 면이 몸을 보지만 바깥 층이다(제 면 그대로 거르면 장식 옷 시험 캐릭터 허리 장식 뒷장 172칸이 다 몸 쪽으로 돈다, 오프라인 재현).
				// 제 면이 봉투 방향과 60° 넘게 어긋나면(버클 옆벽) 쪽을 정하지 않는다 — 둘레 앞면을 따른다(일지 300).
				// 봉투 점이 다른 면의 것이고 봉투 방향이 제 면의 몸 쪽이면(가장 가까운 코어 쪽), 봉투 방향은 이 칸 것이 아니다: 제 면을 코어에서
				// 멀어지는 쪽으로 뒤집어 거기서 시작하고 봉투 방향과 섞지 않는다. 몸 쪽은 다른 면이 아니라 코어로 정한다 — 칸 바로 앞에 붙은 띠의
				// 봉투 점을 받은 살갗 칸은 다른 면이 앞에 있어, 다른 면에서 멀어지는 쪽이면 몸 안을 민다. 코어가 없는 자산은 하지 않는다.
				const int32 OwnFaceTri = Atlas.TexelTri[Texel];
				const FVector3d OwnFace = OwnFaceTri != INDEX_NONE ? AtlasMesh.GetTriNormal(OwnFaceTri) : FVector3d::ZeroVector;
				FVector3d Side = FVector3d::ZeroVector;
				bool bBorrowed = false;
				if (EnvFromOther[Texel])
				{
					NumMarked.Increment();
				}
				if (EnvFromOther[Texel] && Cores.Num() > 0)
				{
					double NearestGap = TNumericLimits<double>::Max();
					FVector3d CoreCenter = TexelSourcePoint[Texel];
					for (const FVector4& Core : Cores)
					{
						const FVector3d Center(Core.X, Core.Y, Core.Z);
						const double Gap = FVector3d::Distance(TexelSourcePoint[Texel], Center) - Core.W;
						if (Gap < NearestGap)
						{
							NearestGap = Gap;
							CoreCenter = Center;
						}
					}
					const FVector3d Away = (TexelSourcePoint[Texel] - CoreCenter).GetSafeNormal();
					const double Along = OwnFace.Dot(Away);
					Side = FMath::Abs(Along) >= SideFacing ? OwnFace * FMath::Sign(Along) : Away;
					bBorrowed = !Side.IsZero() && Env.Dot(Side) <= 0.0;
				}
				if (!bBorrowed)
				{
					if (OutsideBlend <= 0.0)
					{
						return;
					}
					// 뒤에 층이 없는 칸(한 겹 판의 테두리, 성긴 봉투가 깎아 먹은 맨살의 볼록한 곳)은 무엇을 덮은 바깥 층이 아니라 봉투 미는 방향 그대로다
					// (일지 301 g). 봉투 칸 단위 문턱은 봉투가 거칠수록 걸리는 칸이 바뀌어 Sheet 테두리 고리를 다 빼지 못하고 깊이 깎인 맨살은 남긴다.
					if (!LayerBehind[Texel])
					{
						NumNoLayer.Increment();
						return;
					}
					const double Along = OwnFace.Dot(Env);
					Side = FMath::Abs(Along) >= SideFacing ? OwnFace * FMath::Sign(Along) : FVector3d::ZeroVector;
				}
				const double Blend = bBorrowed ? 1.0 : OutsideBlend;
				const double Sigma = FMath::Max(PushDirectionSmoothing * TexelCoreDepth[Texel], 2.0 * TexelSizeCm);
				const double InvTwoSigmaSq = 0.5 / (Sigma * Sigma);
				FVector3d Ref = bBorrowed ? Side : Env;
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					FVector3d Sum = FVector3d::ZeroVector;
					FaceGrid.ForEachInRadius(TexelDrawn[Texel], 3.0 * Sigma, [&](int32 j, double DistSq)
					{
						const double Facing = FaceNormal[j].Dot(Ref);
						if (Facing > 0.0 && (Side.IsZero() || FaceNormal[j].Dot(Side) > 0.0))
						{
							Sum += FaceNormal[j] * (FaceArea[j] * Facing * FMath::Exp(-DistSq * InvTwoSigmaSq));
						}
					});
					const FVector3d Next = Sum.GetSafeNormal();
					if (Next.IsZero())
					{
						break;
					}
					Ref = Next;
				}
				const FVector3d Push = (Env * (1.0 - Blend) + Ref * Blend).GetSafeNormal();
				if (Push.IsZero())
				{
					return;
				}
				TexelNormal[Texel] = Push;
				FScopeLock Scope(&TurnedLock);
				if (bBorrowed)
				{
					BorrowedTurn.Add(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Push.Dot(Side), -1.0, 1.0))));
					return;
				}
				++NumOuter;
				Turned.Add(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Push.Dot(Env), -1.0, 1.0))));
			});
			// 2. 다른 면을 따르는 칸(차트 테두리 고스트 포함, 떠 있는 하드 조각 칸 빼고 — 그 칸은 조각 노멀 하나)은 따르는 점(쌍선형 네 칸)의 미는
			//    방향 — 사슬이면 몇 번 되풀이해 끝의 값까지.
			int32 NumFollow = 0;
			for (int32 Round = 0; Round < 3; ++Round)
			{
				const TArray<FVector3d> Before = TexelNormal;
				NumFollow = 0;
				for (int32 Texel = 0; Texel < NumTexels; ++Texel)
				{
					const uint8 Rule = LayerRule[Texel];
					if (Atlas.Kind[Texel] != KindGhost || Rule == 5)
					{
						continue;
					}
					const FVector2d T = Atlas.Redirect[Texel];
					const int32 BX = (int32)FMath::FloorToDouble(T.X);
					const int32 BY = (int32)FMath::FloorToDouble(T.Y);
					const double FX = T.X - BX;
					const double FY = T.Y - BY;
					FVector3d Sum = FVector3d::ZeroVector;
					for (int32 k = 0; k < 4; ++k)
					{
						const int32 X = BX + (k & 1);
						const int32 Y = BY + (k >> 1);
						if (X < 0 || Y < 0 || X >= R || Y >= R || Atlas.Kind[Atlas.Index(X, Y)] == KindEmpty)
						{
							continue;
						}
						Sum += Before[Atlas.Index(X, Y)] * (((k & 1) ? FX : 1.0 - FX) * ((k >> 1) ? FY : 1.0 - FY));
					}
					const FVector3d Push = Sum.GetSafeNormal();
					if (!Push.IsZero())
					{
						TexelNormal[Texel] = Push;
						++NumFollow;
					}
				}
			}
			Turned.Sort();
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Push direction of outer layers the outer surface does not hold: %d cells now push along the outer drawn surface around them (turned from the outer surface direction median %.1f / 95%% %.1f deg); %d cells that follow another surface push like the point they follow; %d cells out of the outer surface with no layer behind keep the outer surface direction"),
				NumOuter, Turned.Num() ? Turned[Turned.Num() / 2] : 0.0, Turned.Num() ? Turned[Turned.Num() * 95 / 100] : 0.0, NumFollow, NumNoLayer.GetValue());
			BorrowedTurn.Sort();
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Outer surface point from another surface: %d surface cells out of or on the outer surface take their outer surface point from another surface (a thin outer surface shell around a layer off the body); %d of them pointed to the body side of their own face and now push along their own outer side (angle from their own outer face normal 95%% %.1f, max %.1f deg)"),
				NumMarked.GetValue(), BorrowedTurn.Num(), BorrowedTurn.Num() ? BorrowedTurn[BorrowedTurn.Num() * 95 / 100] : 0.0, BorrowedTurn.Num() ? BorrowedTurn.Last() : 0.0);
		}
	}

	// 칸 위치 = 그려지는 면(ADR-0007 §3.6). 봉투 점은 로그(옮긴 거리, 노멀 기준 차이)에만 남긴다.
	TArray<FVector3d> EnvelopePosition;
	if (bUV0Charts)
	{
		EnvelopePosition = TexelPosition;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] != KindEmpty && !TexelNormal[Texel].IsZero())
			{
				TexelPosition[Texel] = TexelDrawn[Texel];
			}
		}
	}

	// 화면 노멀(ADR-0007 §3.6): 칸 자리에서 렌더러가 쓰는 노멀(렌더 삼각형 코너 노멀을 무게중심 좌표로 보간). 노멀 패스가
	// 콜라이더 면에 붙은 칸에서 누르기 전 노멀로 써서, 눌린 자리 음영이 칸 점이 놓인 면의 굴곡과 상관없이 "화면 노멀 → 콜라이더
	// 면 노멀" 회전이 된다. UV0 차트면 칸이 가리키는 렌더 삼각형(겹친 층의 아래층 칸도 제 삼각형 — 그 칸을 읽는 픽셀의 노멀),
	// XAtlas면 칸 점(봉투 점)에서 가장 가까운 렌더 삼각형.
	TArray<float> TexelScreenCode;
	TexelScreenCode.Init(0.f, NumTexels);
	// XAtlas면 위 4c의 봉투 슬롯 렌더 삼각형(칠·스킨 웨이트·덩어리 값과 같은 삼각형).
	{
		const TArray<FVector3d>& CornerNormal = bUV0Charts ? Drawn.CornerNormal : Render.CornerNormal;
		TArray<double> CodeError;
		CodeError.Init(-1.0, NumTexels);
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty || TexelNormal[Texel].IsZero())
			{
				return;
			}
			int32 Tri = Atlas.TexelTri[Texel];
			FVector3d Bary = Atlas.TexelBary[Texel];
			if (!bUV0Charts)
			{
				double DistanceSq = 0.0;
				Tri = RenderBVH->FindNearestTriangle(TexelPosition[Texel], DistanceSq);
				if (Tri == IndexConstants::InvalidID)
				{
					return;
				}
				Bary = TMeshQueries<FDynamicMesh3>::TriangleDistance(Render.Mesh, Tri, TexelPosition[Texel]).TriangleBaryCoords;
			}
			if (Tri < 0 || Tri * 3 + 2 >= CornerNormal.Num())
			{
				return;
			}
			const FVector3d Screen = (CornerNormal[Tri * 3] * Bary.X + CornerNormal[Tri * 3 + 1] * Bary.Y
				+ CornerNormal[Tri * 3 + 2] * Bary.Z).GetSafeNormal();
			if (Screen.IsZero())
			{
				return;
			}
			TexelScreenCode[Texel] = EncodeScreenNormal(Screen);
			CodeError[Texel] = FMath::RadiansToDegrees(
				FMath::Acos(FMath::Clamp(DecodeScreenNormal(TexelScreenCode[Texel]).Dot(Screen), -1.0, 1.0)));
		});
		int32 Baked = 0;
		double MaxError = 0.0;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (CodeError[Texel] >= 0.0)
			{
				++Baked;
				MaxError = FMath::Max(MaxError, CodeError[Texel]);
			}
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Rendered normals: baked into %d cells (%s), max storage error %.3f deg"), Baked,
			bUV0Charts ? TEXT("from the render triangle each cell points at") : TEXT("from the nearest render triangle"), MaxError);
	}

	// 구운 노멀이 바깥을 향하는지 소스 솔리드로 확인한다: 노멀 쪽으로 한 칸 나간 점은 winding < 0.5여야 한다.
	// 뒤집히면 함몰이 콜라이더 쪽으로 빨려 나가므로 과반이 안쪽이면 베이크를 실패시킨다.
	{
		FDynamicMeshAABBTree3 SourceBVH(&Source, true);
		TFastWindingTree<FDynamicMesh3> Winding(&SourceBVH, true);
		int32 Outward = 0;
		int32 Checked = 0;
		const int32 Step = FMath::Max(1, NumTexels / 4096);
		for (int32 Texel = 0; Texel < NumTexels; Texel += Step)
		{
			if (Atlas.Kind[Texel] != KindSurface)
			{
				continue;
			}
			++Checked;
			Outward += Winding.FastWindingNumber(TexelPosition[Texel] + TexelNormal[Texel] * Cell) < 0.5 ? 1 : 0;
		}
		const double OutwardRatio = Checked > 0 ? (double)Outward / Checked : 0.0;
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Outward normal check: of %d cells, %.1f%% face outward"), Checked, 100.0 * OutwardRatio);
		if (OutwardRatio < 0.5)
		{
			UE_LOG(LogSoftbodyCarrier, Error, TEXT("Bake stopped: the contact map normals point inward. Check that the mesh is closed and its faces point outward, then click Rebake Contact Map."));
			return false;
		}
		// 속 메시(ADR-0028): 겉면이 속 메시 안에 놓여야 칸이 제 봉투 점(미는 방향·두께)을 읽는다. 노멀 반대쪽으로 한 칸 들어간 점은
		// 속 메시 안(winding > 0.5)이어야 한다. 밖에 있는 칸은 속 메시가 대상 메시보다 작거나 다른 자리에 있다는 뜻이다.
		if (Data->VolumeMesh)
		{
			int32 Inside = 0;
			for (int32 Texel = 0; Texel < NumTexels; Texel += Step)
			{
				if (Atlas.Kind[Texel] == KindSurface)
				{
					Inside += Winding.FastWindingNumber(TexelPosition[Texel] - TexelNormal[Texel] * Cell) > 0.5 ? 1 : 0;
				}
			}
			const double InsideRatio = Checked > 0 ? (double)Inside / Checked : 0.0;
			if (InsideRatio < 0.9)
			{
				UE_LOG(LogSoftbodyCarrier, Warning,
					TEXT("Volume mesh check: of %d surface cells, only %.1f%% are inside volume mesh %s. Its outside should sit where the target mesh's surface is; cells outside it get the wrong push direction and flesh thickness."),
					Checked, 100.0 * InsideRatio, *Data->VolumeMesh->GetName());
			}
			else
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("Volume mesh check: of %d surface cells, %.1f%% are inside the volume mesh"), Checked, 100.0 * InsideRatio);
			}
		}
	}

	TArray<FLinearColor> PositionTexels;
	TArray<FFloat16Color> NormalTexels;
	TArray<FLinearColor> RedirectTexels;
	PositionTexels.SetNum(NumTexels);
	NormalTexels.SetNum(NumTexels);
	RedirectTexels.SetNum(NumTexels);
	for (int32 Texel = 0; Texel < NumTexels; ++Texel)
	{
		const FVector3d& P = TexelPosition[Texel];
		const FVector3d& N = TexelNormal[Texel];
		PositionTexels[Texel] = FLinearColor((float)P.X, (float)P.Y, (float)P.Z, (float)Atlas.Kind[Texel]);
		NormalTexels[Texel] = FFloat16Color(FLinearColor((float)N.X, (float)N.Y, (float)N.Z, (float)TexelThickness[Texel]));
		RedirectTexels[Texel] = FLinearColor((float)Atlas.Redirect[Texel].X, (float)Atlas.Redirect[Texel].Y, 0.f,
			TexelScreenCode[Texel]);
	}

	// --- 5. 렌더 정점(텍셀 테이블 순서) → 캐리어 UV (위치당 하나 — 코너 WPO가 읽는다) ---
	const int32 TableSize = Data->TextureSize;
	TArray<FLinearColor> TableTexels;
	TableTexels.Init(FLinearColor(0.f, 0.f, 0.f, 0.f), TableSize * TableSize);
	TArray<double> AttachDistances;
	AttachDistances.SetNum(VertexOrder.Num());
	// 테셀레이션 사이 값 무게(ADR-0006 §3.3)를 정하려고 정점마다 읽는 차트·캐리어 점·UV를 남긴다.
	TArray<int32> VertexChart;
	TArray<FVector3d> VertexCarrierPoint;
	TArray<FVector2d> VertexCarrierUV;
	VertexChart.Init(INDEX_NONE, VertexOrder.Num());
	VertexCarrierPoint.Init(FVector3d::ZeroVector, VertexOrder.Num());
	VertexCarrierUV.Init(FVector2d::ZeroVector, VertexOrder.Num());
	ParallelFor(VertexOrder.Num(), [&](int32 Index)
	{
		const FVector3d& P = VertexPositions[Index];
		double DistanceSq = 0.0;
		const int32 Tri = EnvelopeBVH.FindNearestTriangle(P, DistanceSq);
		AttachDistances[Index] = TNumericLimits<double>::Max();
		if (Tri == IndexConstants::InvalidID || Index >= TableSize * TableSize)
		{
			return;
		}
		FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Envelope, Tri, P);
		const double Side = (P - Query.ClosestTrianglePoint).Dot(Envelope.GetTriNormal(Tri)) >= 0.0 ? 1.0 : -1.0;
		const double Signed = Side * FMath::Sqrt(Query.GetSquared());
		AttachDistances[Index] = FMath::Abs(Signed);
		// 이 정점이 읽을 캐리어 점: XAtlas면 봉투에서 가장 가까운 점. UV0 차트면 봉투 슬롯 정점은 제 아일랜드 차트의 자기 점
		// (이음매 정점은 차트 번호가 가장 작은 쪽), 그 밖의 정점은 봉투 슬롯 렌더 삼각형에서 가장 가까운 점.
		FVector2d UV = FVector2d::ZeroVector;
		int32 Chart = INDEX_NONE;
		FVector3d CarrierPoint = Query.ClosestTrianglePoint;
		if (!bUV0Charts)
		{
			UV = AtlasUV(Atlas, Tri, Query.TriangleBaryCoords);
			Chart = Atlas.TriChart[Tri];
		}
		else if (const int32* Element = UV0.VertexElement.Find(VertexOrder[Index].GetValue()))
		{
			UV = Atlas.UVs[*Element];
			Chart = UV0.ElementChart[*Element];
			CarrierPoint = P;
		}
		else
		{
			double AtlasDistanceSq = 0.0;
			const int32 AtlasTri = AtlasBVH.FindNearestTriangle(P, AtlasDistanceSq);
			if (AtlasTri == IndexConstants::InvalidID)
			{
				return;
			}
			const FDistPoint3Triangle3d AtlasQuery = TMeshQueries<FDynamicMesh3>::TriangleDistance(AtlasMesh, AtlasTri, P);
			UV = AtlasUV(Atlas, AtlasTri, AtlasQuery.TriangleBaryCoords);
			Chart = Atlas.TriChart[AtlasTri];
			CarrierPoint = AtlasQuery.ClosestTrianglePoint;
		}
		TableTexels[Index] = FLinearColor((float)UV.X, (float)UV.Y, (float)Signed, 1.f);
		VertexChart[Index] = Chart;
		VertexCarrierPoint[Index] = CarrierPoint;
		VertexCarrierUV[Index] = UV;
	});
	WriteTessellationWeights(*MD, EnvelopeSlots, VertexOrder, VertexPositions, VertexChart, VertexCarrierPoint,
		VertexCarrierUV, Resolution, TexelSizeCm, TableTexels);
	// 이음매 정점(UV0 차트, ADR-0007 §3.3): 같은 위치에 캐리어 UV가 다른 요소가 둘 이상이면(UV0 이음매, 더 넣은 이음매 §3.8)
	// 표 A에 4를 더한다.
	// 캐리어 UV 채널을 쓰는 머티리얼은 이 정점에서 사이 값 무게를 0으로 둔다 — 이음매 변 위에서는 양쪽 삼각형이 위치당
	// 하나인 값(코너 WPO와 표 UV로 읽은 코너 노멀의 퐁 곡면)만 써서 나나이트 테셀레이션이 틈을 내지 않는다.
	if (bUV0Charts)
	{
		const auto PositionKey = [](const FVector3d& P)
		{
			return FIntVector((int32)FMath::RoundToDouble(P.X * 1000.0), (int32)FMath::RoundToDouble(P.Y * 1000.0),
				(int32)FMath::RoundToDouble(P.Z * 1000.0));
		};
		TMap<FIntVector, int32> FirstElement;
		TSet<FIntVector> SeamKeys;
		for (int32 Element = 0; Element < UV0.ElementVertex.Num(); ++Element)
		{
			const FIntVector Key = PositionKey(FVector3d(Positions[FVertexID(UV0.ElementVertex[Element])]));
			if (const int32* First = FirstElement.Find(Key))
			{
				if (FVector2d::DistSquared(Atlas.UVs[*First], Atlas.UVs[Element]) > 1e-12)
				{
					SeamKeys.Add(Key);
				}
			}
			else
			{
				FirstElement.Add(Key, Element);
			}
		}
		int32 SeamVertices = 0;
		for (int32 i = 0; i < VertexOrder.Num() && i < TableTexels.Num(); ++i)
		{
			if (TableTexels[i].A > 0.5f && SeamKeys.Contains(PositionKey(VertexPositions[i])))
			{
				TableTexels[i].A += 4.f;
				++SeamVertices;
			}
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Seam vertices (UV0 seams + extra seams): %d at %d positions; materials using the contact map UV channel use only the smooth curved surface there"),
			SeamVertices, SeamKeys.Num());
	}
	// 같은 자리 정점은 한 점을 읽는다(원칙 P09, 일지 300). 위에서 텍셀 표는 정점 번호마다 제 UV0 요소를 읽었다. 그래서 번호가 다른 같은 자리 사본
	// (용접하지 않은 메시의 이음매, 장식 테 밑에 둔 피부 사본처럼 다른 층이 같은 자리에 둔 정점)이 서로 다른 칸을 읽어 눌리면 벌어졌다(장식 옷 시험 캐릭터
	// 같은 자리 무리 5,001개, 손 4 cm 누름에 1.16 cm). 칸보다 가는 UV 띠는 칸이 생기지 않아, 쌍선형으로 읽는 칸이 빈 칸인 정점은 움직이지 않았다.
	//  - 위치 키(이음매 표시·테셀레이션 무게와 같은 1/1000)로 묶은 무리마다 한 점: 읽는 칸이 모두 찬 점 → 차트 번호가 작은 쪽 → UV가 작은 쪽.
	//  - 고른 점이 빈 칸을 읽으면 3-D에서 가장 가까운 표면 칸 한가운데를 읽는다(래스터되지 않은 띠, principles-reference "같은 자리·층").
	// 테셀레이션 무게는 제 요소로 정한 값 그대로다 — 같은 자리 사본은 이음매 정점이라 무게가 이미 0이고, 무게는 같은 자리끼리 가장 작은 값을 쓴다.
	if (bUV0Charts)
	{
		const int32 R = Resolution;
		// 쌍선형 무게가 0이 아닌 칸이 모두 찼는가.
		const auto ReadsFilled = [&](const FVector2d& UV)
		{
			const FVector2d T = UV * (double)R - FVector2d(0.5, 0.5);
			const int32 BX = (int32)FMath::FloorToDouble(T.X);
			const int32 BY = (int32)FMath::FloorToDouble(T.Y);
			const double FX = T.X - BX;
			const double FY = T.Y - BY;
			for (int32 k = 0; k < 4; ++k)
			{
				const double W = ((k & 1) ? FX : 1.0 - FX) * ((k >> 1) ? FY : 1.0 - FY);
				if (W <= 1e-6)
				{
					continue;
				}
				const int32 X = BX + (k & 1);
				const int32 Y = BY + (k >> 1);
				if (X < 0 || Y < 0 || X >= R || Y >= R || Atlas.Kind[Atlas.Index(X, Y)] == KindEmpty)
				{
					return false;
				}
			}
			return true;
		};
		const auto PositionKey = [](const FVector3d& P)
		{
			return FIntVector((int32)FMath::RoundToDouble(P.X * 1000.0), (int32)FMath::RoundToDouble(P.Y * 1000.0),
				(int32)FMath::RoundToDouble(P.Z * 1000.0));
		};
		// 칸보다 가는 섬: 폭(2 × 넓이 ÷ 둘레)이 한 칸보다 좁은 UV 섬은 칸이 생겼다 말았다 해서(섬이 텍셀에 걸치는 자리에 따라) 그 섬의 정점이 읽는
		// 값이 굽기마다 달랐다(합성 겹친 가슴골 조각의 0.12 cm 가죽 테: 손 4 cm 누름 늘어남이 굽기마다 1.3~10.3배). 칸보다 얇은 장식은 붙은 면과 같은
		// 칸을 읽는다(원칙 P09) — 그런 섬의 정점은 3-D에서 가장 가까운, 가늘지 않은 섬의 표면 칸 한가운데를 읽는다.
		int32 NumChartsHere = 0;
		for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
		{
			NumChartsHere = FMath::Max(NumChartsHere, Atlas.TriChart.IsValidIndex(Tri) ? Atlas.TriChart[Tri] + 1 : 0);
		}
		TArray<double> ChartArea;
		TArray<double> ChartPerimeter;
		ChartArea.Init(0.0, NumChartsHere);
		ChartPerimeter.Init(0.0, NumChartsHere);
		{
			TMap<FIntPoint, int32> EdgeUses;
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				const int32 Chart = Atlas.TriChart.IsValidIndex(Tri) ? Atlas.TriChart[Tri] : INDEX_NONE;
				if (Chart < 0)
				{
					continue;
				}
				ChartArea[Chart] += AtlasMesh.GetTriArea(Tri);
				for (int32 c = 0; c < 3; ++c)
				{
					const int32 A = Atlas.UVIndices[Tri * 3 + c];
					const int32 B = Atlas.UVIndices[Tri * 3 + (c + 1) % 3];
					++EdgeUses.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
				}
			}
			for (const int32 Tri : AtlasMesh.TriangleIndicesItr())
			{
				const int32 Chart = Atlas.TriChart.IsValidIndex(Tri) ? Atlas.TriChart[Tri] : INDEX_NONE;
				if (Chart < 0)
				{
					continue;
				}
				for (int32 c = 0; c < 3; ++c)
				{
					const int32 A = Atlas.UVIndices[Tri * 3 + c];
					const int32 B = Atlas.UVIndices[Tri * 3 + (c + 1) % 3];
					if (EdgeUses[FIntPoint(FMath::Min(A, B), FMath::Max(A, B))] == 1)
					{
						ChartPerimeter[Chart] += FVector3d::Distance(AtlasMesh.GetVertex(Atlas.UVToVertex[A]), AtlasMesh.GetVertex(Atlas.UVToVertex[B]));
					}
				}
			}
		}
		const auto ThinChart = [&](int32 Chart)
		{
			return Chart >= 0 && Chart < NumChartsHere && 2.0 * ChartArea[Chart] < TexelSizeCm * ChartPerimeter[Chart];
		};
		int32 NumThinCharts = 0;
		for (int32 Chart = 0; Chart < NumChartsHere; ++Chart)
		{
			NumThinCharts += ThinChart(Chart) ? 1 : 0;
		}
		TMap<FIntVector, TArray<int32>> Groups;
		for (int32 i = 0; i < VertexOrder.Num() && i < TableTexels.Num(); ++i)
		{
			if (TableTexels[i].A > 0.5f)
			{
				Groups.FindOrAdd(PositionKey(VertexPositions[i])).Add(i);
			}
		}
		int32 NumShared = 0;
		int32 NumUnified = 0;
		int32 NumMoved = 0;
		TArray<int32> NeedNearest;
		TArray<uint8> NeedMustMove;   // 1 = 빈 칸을 읽어 반드시 옮김, 0 = 가는 섬이라 두 칸 안에 붙은 면이 있을 때만 옮김
		for (const TPair<FIntVector, TArray<int32>>& Pair : Groups)
		{
			const TArray<int32>& Members = Pair.Value;
			int32 Best = INDEX_NONE;
			bool bBestFilled = false;
			for (const int32 i : Members)
			{
				const FVector2d UV(TableTexels[i].R, TableTexels[i].G);
				const bool bFilled = ReadsFilled(UV) && !ThinChart(VertexChart[i]);
				if (Best == INDEX_NONE)
				{
					Best = i;
					bBestFilled = bFilled;
					continue;
				}
				const uint32 Chart = (uint32)VertexChart[i];
				const uint32 BestChart = (uint32)VertexChart[Best];
				const FVector2d BestUV(TableTexels[Best].R, TableTexels[Best].G);
				const bool bBetter = bFilled != bBestFilled ? bFilled
					: Chart != BestChart ? Chart < BestChart
					: UV.X != BestUV.X ? UV.X < BestUV.X : UV.Y < BestUV.Y;
				if (bBetter)
				{
					Best = i;
					bBestFilled = bFilled;
				}
			}
			if (Members.Num() > 1)
			{
				++NumShared;
				int32 Moved = 0;
				for (const int32 i : Members)
				{
					if (TableTexels[i].R != TableTexels[Best].R || TableTexels[i].G != TableTexels[Best].G)
					{
						TableTexels[i].R = TableTexels[Best].R;
						TableTexels[i].G = TableTexels[Best].G;
						++Moved;
					}
				}
				NumUnified += Moved > 0 ? 1 : 0;
				NumMoved += Moved;
			}
			if (!bBestFilled)
			{
				NeedNearest.Add(Best);
				NeedMustMove.Add(ReadsFilled(FVector2d(TableTexels[Best].R, TableTexels[Best].G)) ? 0 : 1);
			}
		}
		int32 NumNearest = 0;
		double FarthestNearest = 0.0;
		// 빈 칸을 읽던 정점은 기본 Surface Attach Range의 절반 안의 표면 칸만 읽게 옮긴다(일지 301 f). 그보다 멀면 빈 칸을 그대로 읽어 움직이지 않는다
		// — 거리 제한이 없을 때 옷 입은 시험 캐릭터 정점 125개가 18.6 cm 떨어진 칸을 읽었다(그 자리를 누르면 끌려간다). 붙음 무게가 그 절반부터 줄어든다.
		const double MoveCap = 0.5 * (double)GetDefault<USoftbodyComponent>()->SurfaceAttachRange;
		int32 NumLeftEmpty = 0;
		if (NeedNearest.Num() > 0)
		{
			TArray<FVector3d> SurfacePoints;
			TArray<int32> SurfaceTexels;
			for (int32 Texel = 0; Texel < NumTexels; ++Texel)
			{
				const int32 Tri = Atlas.TexelTri[Texel];
				if (Atlas.Kind[Texel] == KindSurface && !(Tri != INDEX_NONE && Atlas.TriChart.IsValidIndex(Tri) && ThinChart(Atlas.TriChart[Tri])))
				{
					SurfacePoints.Add(TexelPosition[Texel]);
					SurfaceTexels.Add(Texel);
				}
			}
			FPointGrid SurfaceGrid;
			SurfaceGrid.Build(SurfacePoints, 4.0 * TexelSizeCm);
			TArray<TPair<double, int32>> Found;
			// 가는 섬 정점은 붙은 면(두 칸 안)이 있을 때만 그 면을 읽는다 — 장식 섬 아래 면이 멀면(등 허리 장식 3.5 cm) 제 섬이 더 가깝다.
			const double ThinReach = 2.0 * TexelSizeCm;
			for (int32 n = 0; n < NeedNearest.Num(); ++n)
			{
				const int32 Lead = NeedNearest[n];
				SurfaceGrid.FindNearest(VertexPositions[Lead], 1, Found);
				if (Found.Num() == 0 || (!NeedMustMove[n] && Found[0].Key > ThinReach * ThinReach))
				{
					continue;
				}
				if (Found[0].Key > MoveCap * MoveCap)
				{
					NumLeftEmpty += Groups[PositionKey(VertexPositions[Lead])].Num();
					continue;
				}
				const int32 Texel = SurfaceTexels[Found[0].Value];
				const FVector2d UV(((Texel % R) + 0.5) / (double)R, ((Texel / R) + 0.5) / (double)R);
				for (const int32 i : Groups[PositionKey(VertexPositions[Lead])])
				{
					TableTexels[i].R = (float)UV.X;
					TableTexels[i].G = (float)UV.Y;
					++NumNearest;
				}
				FarthestNearest = FMath::Max(FarthestNearest, FMath::Sqrt(Found[0].Key));
			}
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Same-position vertices: %d positions hold more than one vertex, %d of them read different cells before and now read one point (%d vertices moved); %d vertices read an empty cell or an island narrower than a cell (%d such islands) and now read the nearest surface cell (up to %.3f cm away); %d vertices farther than %.1f cm from every surface cell keep reading an empty cell"),
			NumShared, NumUnified, NumMoved, NumNearest, NumThinCharts, FarthestNearest, NumLeftEmpty, MoveCap);
	}
	{
		const TArray<float> TexelEdge = ComputeRenderEdgeLengths(*MD, EnvelopeSlots, VertexOrder, VertexPositions, Atlas, TexelPosition);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			RedirectTexels[Texel].B = TexelEdge[Texel];
		}
	}
	if (bUV0Charts)
	{
		// 그려지는 면(ADR-0007 §3.6)이 칸 점을 봉투 점에서 얼마나 옮겼는지(= 전에는 눌린 정점이 콜라이더 면에서 떨어져 있던 거리).
		TArray<double> Moved;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindSurface && !TexelNormal[Texel].IsZero())
			{
				Moved.Add(FVector3d::Distance(EnvelopePosition[Texel], TexelPosition[Texel]));
			}
		}
		Moved.Sort();
		const auto MovedAt = [&Moved](double Q)
		{
			return Moved.Num() > 0 ? Moved[FMath::Clamp((int32)(Q * (Moved.Num() - 1)), 0, Moved.Num() - 1)] : 0.0;
		};
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Cell points moved onto the rendered surface (Phong %.2f): distance from the outer surface median %.3f / 95%% %.3f / max %.3f"),
			DrawnSurfaceCurve, MovedAt(0.5), MovedAt(0.95), MovedAt(1.0));
	}
	TArray<double> SortedAttach = AttachDistances;
	SortedAttach.Sort();
	const auto Percentile = [&SortedAttach](double Q)
	{
		return SortedAttach.Num() > 0 ? SortedAttach[FMath::Clamp((int32)(Q * (SortedAttach.Num() - 1)), 0, SortedAttach.Num() - 1)] : 0.0;
	};

	// --- 6. 스켈레탈: 캐리어 칸 스킨 웨이트 (칸 자리 렌더 삼각형 세 정점의 웨이트를 무게중심 좌표로 섞고, 상위 4본) ---
	// 메시가 그리는 점은 세 코너를 따로 스킨해 같은 무게로 섞은 것이라, 웨이트를 이렇게 섞은 칸은 자세가 바뀌어도 그려지는 면을
	// 따라간다(1차까지 같다). 전에는 가까운 정점 4개의 역거리 평균이라 넷이 바뀌는 자리마다 웨이트가 뛰고, 굽힌 자세에서 칸 위치가
	// 정점 간격의 계단으로 어긋날 수 있었다(일지 209).
	TArray<FFloat16Color> SkinIndexTexels;
	TArray<FFloat16Color> SkinWeightTexels;
	if (Skeletal)
	{
		FSkeletalMeshConstAttributes SkinAttributes(*MD);
		FSkinWeightsVertexAttributesConstRef SkinWeights = SkinAttributes.GetVertexSkinWeights();
		if (!SkinWeights.IsValid())
		{
			UE_LOG(LogSoftbodyCarrier, Error, TEXT("Contact map bake: the skeletal mesh has no skin weights. Reimport it with its skin weights."));
			return false;
		}
		// 렌더 삼각형이 없는 칸(드묾)만 가장 가까운 봉투 슬롯 정점 하나에서 — 머리카락이 덮은 피부 칸이 머리카락 본을 따라 움직이지 않게
		TArray<FVector3d> SkinSourcePositions;
		TArray<int32> SkinSourceIndex;
		for (int32 i = 0; i < VertexPositions.Num(); ++i)
		{
			if (VertexInEnvelopeSlots.IsValidIndex(i) && VertexInEnvelopeSlots[i])
			{
				SkinSourcePositions.Add(VertexPositions[i]);
				SkinSourceIndex.Add(i);
			}
		}
		FPointGrid Grid;
		Grid.Build(SkinSourcePositions, FMath::Max(Cell * 2.0, 1.0));
		SkinIndexTexels.Init(FFloat16Color(FLinearColor(0, 0, 0, 0)), NumTexels);
		SkinWeightTexels.Init(FFloat16Color(FLinearColor(1, 0, 0, 0)), NumTexels);
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty)
			{
				return;
			}
			TMap<int32, double> Accumulated;
			if (TexelCorners[Texel].X != INDEX_NONE)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					const double Weight = FMath::Max(TexelCornerBary[Texel][c], 0.0);
					if (Weight <= 0.0)
					{
						continue;
					}
					for (UE::AnimationCore::FBoneWeight BoneWeight : SkinWeights.Get(VertexOrder[TexelCorners[Texel][c]]))
					{
						Accumulated.FindOrAdd((int32)BoneWeight.GetBoneIndex()) += Weight * BoneWeight.GetWeight();
					}
				}
			}
			if (Accumulated.Num() == 0)
			{
				TArray<TPair<double, int32>> Nearest;
				Grid.FindNearest(bUV0Charts ? TexelSourcePoint[Texel] : TexelPosition[Texel], 1, Nearest);
				for (const TPair<double, int32>& Hit : Nearest)
				{
					for (UE::AnimationCore::FBoneWeight BoneWeight : SkinWeights.Get(VertexOrder[SkinSourceIndex[Hit.Value]]))
					{
						Accumulated.FindOrAdd((int32)BoneWeight.GetBoneIndex()) += BoneWeight.GetWeight();
					}
				}
			}
			TArray<TPair<int32, double>> Ranked;
			for (const TPair<int32, double>& Entry : Accumulated)
			{
				Ranked.Emplace(Entry.Key, Entry.Value);
			}
			Ranked.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });
			double Sum = 0.0;
			for (int32 k = 0; k < FMath::Min(4, Ranked.Num()); ++k)
			{
				Sum += Ranked[k].Value;
			}
			FLinearColor Indices(0, 0, 0, 0);
			FLinearColor Weights(0, 0, 0, 0);
			for (int32 k = 0; k < FMath::Min(4, Ranked.Num()); ++k)
			{
				(&Indices.R)[k] = (float)Ranked[k].Key;
				(&Weights.R)[k] = Sum > 0.0 ? (float)(Ranked[k].Value / Sum) : (k == 0 ? 1.f : 0.f);
			}
			SkinIndexTexels[Texel] = FFloat16Color(Indices);
			SkinWeightTexels[Texel] = FFloat16Color(Weights);
		});
	}

	// --- 6b. 덩어리 값 (ADR-0012): 위 4c의 칸 자리 렌더 삼각형에서 ---
	TArray<FFloat16Color> LumpTexels;
	TArray<FFloat16Color> LumpSideA;
	TArray<FFloat16Color> LumpSideB;
	TArray<FSoftbodyInertiaLump> Lumps;
	FLumpTexelSource LumpSource;
	const bool bLumps = BuildLumpTexels(*Data, VertexPositions.Num(), Atlas.Kind, TexelCorners, TexelCornerBary, TexelNormal,
		TexelCoreDepth, bLumpSupport ? &TexelMoundDepth : nullptr, TexelSizeCm, LumpTexels, LumpSideA, LumpSideB, Lumps, &LumpSource);
	// 덩어리 손 밑 살층 강성: 옆 몫 B = 1 − Jiggle. 실행 중 손이 미는 살층 강성에 1/Jiggle을 곱한다(셰이더 SB_LumpFleshStiffness) —
	// 측정 FEM이 그 칸 살을 1/Jiggle만큼 단단하게 풀었으므로 같은 살이다. 몸 전체를 고르게 단단히 칠해도 같은 손 누름의 덩어리 밀림이 그대로다(변위 제어).
	// 흰 살(칠 안 함)·옛 굽기는 0이라 배수 1이다.
	if (bLumps && bJiggle && LumpSideB.Num() == NumTexels)
	{
		TArray<double> JiggleCells;
		PaintCells(VertexJiggle, TEXT("Jiggle"), JiggleCells);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] != KindEmpty)
			{
				LumpSideB[Texel].B = FFloat16((float)FMath::Clamp(1.0 - JiggleCells[Texel], 0.0, 1.0));
			}
		}
	}

	// --- 6b2. 덩어리 국소 모양 (SoftbodyLumpRegion, 일지 251) ---
	// 측정 굽기의 원판 누름에서 위 덩어리 모양이 이미 그리는 몫을 뺀 고유 모양과 그 단단함(Lumps의 RegionStiffness). 칠과 덩어리 양이 들어가므로
	// 칠을 바꾸면(Reapply Paint) 여기서 다시 마무리한다. 줄은 솟음 거친 칸마다라, 굽기 끝의 솟음 거리표(BakeBulgeLandmarks)와 같은 묶음
	// (칸 크기가 같으면 같은 값)으로 모으고 거리표가 구워졌을 때만 저장한다.
	const int32 RegionBlock = SoftbodyBulgeTable::BlockFor((float)TexelSizeCm, Resolution);
	TArray<uint16> RegionIndexTexels;
	TArray<FFloat16Color> RegionRecordTexels;
	const int32 RegionRecords = bLumps && Lumps.Num() > 0 ? BuildLumpRegions(*Data, LumpSource, Atlas.Kind, TexelCorners, TexelCornerBary,
		Resolution, RegionBlock, Lumps, RegionIndexTexels, RegionRecordTexels) : 0;
	LumpSource = FLumpTexelSource();

	// --- 6c. 칸별 재료 칠: Volume(ADR-0024, 페인트 A) — 위 3.2의 값을 Depth와 같은 길(4c PaintCells)로 ---
	// 칠한 값이 전부 1이면 텍스처를 만들지 않는다(실행 중 전부 1로 읽는다). 칸에는 칠만 담고 저작 에셋 값은 실행 중에 곱한다.
	const auto MaterialPaintTexels = [&](const TArray<double>& VertexValue, double MinValue, bool bPainted, const TCHAR* What,
		TArray<uint8>& OutTexels) -> bool
	{
		if (MinValue >= 0.999)
		{
			if (bPainted)
			{
				UE_LOG(LogSoftbodyCarrier, Display, TEXT("%s paint is 1 everywhere; no texture needed"), What);
			}
			return false;
		}
		TArray<double> Cells;
		PaintCells(VertexValue, What, Cells);
		OutTexels.Init(255, NumTexels);
		FThreadSafeCounter NumPainted;
		ParallelFor(NumTexels, [&](int32 Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty)
			{
				return;
			}
			const double Value = Cells[Texel];
			OutTexels[Texel] = (uint8)FMath::Clamp(FMath::RoundToInt(Value * 255.0), 0, 255);
			if (Value < 0.999)
			{
				NumPainted.Increment();
			}
		});
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("%s paint: lowest painted vertex %.3f, cells below 1: %d / %d"),
			What, MinValue, NumPainted.GetValue(), NumTexels);
		return NumPainted.GetValue() > 0;
	};
	TArray<uint8> VolumeTexels;
	const bool bVolume = MaterialPaintTexels(VertexVolume, MinVolume, Data->bPaintVolume, TEXT("Volume"), VolumeTexels);

	// --- 6d. 하드 서피스 조각(ADR-0037 §3.7): 칸마다 조각 번호·접촉 시작점 올림과 조각 표. 조각이 없으면 건너뛴다(텍스처·표가 비어 저장되지 않는다). ---
	// 조각 밑면은 봉투 칸 둘 안(겹친 층의 가까운 층 판정과 같은 거리), 윗면을 찾는 광선 길이는 봉투 칸 여덟.
	// 살 있는 칸 두께의 중앙값·90 % — 살이 없는 칸(눌리는 정도 칠 0)이 넓은 메시에서 이 값이 0 쪽으로 끌려가면, 솟음이 번지는 폭의 상한과 이완의
	// 수렴 범위가 실제로 눌리는 살보다 좁게 잡힌다. 중앙값은 기록용이다 — 겉껍질 장력·판 굽힘 길이는 고정 길이 L0를 쓴다.
	double ThicknessMedian = 0.0;
	double ThicknessHigh = 0.0;
	{
		TArray<double> SurfaceThickness;
		SurfaceThickness.Reserve(NumSurface);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindSurface && TexelThickness[Texel] > 0.0)
			{
				SurfaceThickness.Add(TexelThickness[Texel]);
			}
		}
		SurfaceThickness.Sort();
		ThicknessHigh = SurfaceThickness.Num() > 0 ? SurfaceThickness[SurfaceThickness.Num() * 9 / 10] : 0.0;
		ThicknessMedian = SurfaceThickness.Num() > 0 ? SurfaceThickness[SurfaceThickness.Num() / 2] : 0.0;
	}
	// 판 칠 → 칸(ADR-0038 §3.5): Depth와 같은 렌더 삼각형 보간 + 렌더 변 길이 다듬기. 정점 값 = 그 정점이 닿은 (떠 있지 않은) 판 삼각형 코너
	// Softness의 가장 작은 값. 판에 닿지 않은 정점과 칸은 읽지 않음(−1)이라 다듬기가 판 안에서만 섞는다 — 둘레 흰 살이 판 가장자리를 무르게
	// 만들지 않는다. 검정(0)도 다듬지 않고 그대로다.
	TArray<double> SoftnessCells;
	if (Hard.Num() > 0)
	{
		TArray<int32> OrderOf;
		OrderOf.Init(INDEX_NONE, MD->Vertices().GetArraySize());
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			OrderOf[VertexOrder[i].GetValue()] = i;
		}
		TArray<double> VertexSoftness;
		VertexSoftness.Init(-1.0, VertexOrder.Num());
		for (const FTriangleID Tri : MD->Triangles().GetElementIDs())
		{
			const int32 PieceNumber = Hard.PieceOf(Tri);
			if (PieceNumber <= 0 || Hard.IsFloating(PieceNumber))
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD->GetTriangleVertices(Tri);
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 i = OrderOf.IsValidIndex(Corners[c].GetValue()) ? OrderOf[Corners[c].GetValue()] : INDEX_NONE;
				if (i != INDEX_NONE)
				{
					const double S = Hard.CornerSoftness(Tri, c);
					VertexSoftness[i] = VertexSoftness[i] < 0.0 ? S : FMath::Min(VertexSoftness[i], S);
				}
			}
		}
		double MaxChange = 0.0;
		PaintCellsFromVertices(VertexSoftness, -1.0, TexelCorners, TexelCornerBary, Atlas.Kind, TexelPosition, TexelNormal, PaintRadius,
			1.5 * TexelSizeCm, SoftnessCells, MaxChange);
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex paint to plate cells: render triangle interpolation, then smoothed inside each plate over the render edge length (largest change %.3f)"),
			MaxChange);
	}
	// 굽힘 길이 눈금 L0: 실행 중 셰이더(HardRefThickness)와 같은 고정 길이. 칠한 살 두께 중앙값을 쓰면 Depth 칠이 판 굳기를 바꾼다(원칙 P03).
	const double PlateLengthScale = (double)SoftbodyContactResponse::ReferenceLength;
	FHardPieceTexels HardTexels;
	const bool bHard = Hard.Num() > 0 && BuildHardPieceTexels(*MD, Hard, VertexOrder, Atlas, TexelCorners, TexelPosition, TexelNormal,
		SoftnessCells, TexelThickness, Resolution, RegionBlock, TexelSizeCm, PlateLengthScale, 2.0 * Cell, 8.0 * Cell, OrnamentPiece, OrnamentOwnPoint,
		HardTexels);
	// 조각 칸(발자국·고스트)의 미는 방향은 조각 노멀 하나다(일지 291). 칸마다 제 방향이면 같은 판 깊이에 칸이 다른 쪽으로 움직여 판이 찢어진다 —
	// 각진 주사위 눈의 73° 벽(미는 방향이 면 노멀에서 66°까지)이 1.5 cm 가라앉음에 옆으로 1.4 cm 밀려 윗면과 벌어졌다. 접촉 광선·변위·끌림이 모두
	// 이 방향을 쓰므로 판이 한 몸으로 움직인다. 감김 판정(BuildHardPieceTexels)은 원래 방향으로 이미 끝났다.
	if (bHard)
	{
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			const int32 P = HardTexels.Piece[Texel] & SoftbodyHardSurface::PieceMask;
			if (P > 0 && HardTexels.Pieces.IsValidIndex(P - 1))
			{
				const FSoftbodyHardPiece& Piece = HardTexels.Pieces[P - 1];
				const FVector3f N = FVector3f::CrossProduct(Piece.AxisU, Piece.AxisV).GetSafeNormal();
				if (!N.IsZero())
				{
					const FFloat16Color Old = NormalTexels[Texel];
					NormalTexels[Texel] = FFloat16Color(FLinearColor(N.X, N.Y, N.Z, Old.A.GetFloat()));
				}
			}
		}
	}
	// 단단한 조각의 렌더 정점은 제 판 칸만 읽는다(일지 300). 판은 칸으로 움직이는데, 조각 정점이 쌍선형으로 읽는 네 칸에 판 밖 칸이 섞이면
	// 장식이 판과 따로 휜다 — 장식 옷 시험 캐릭터 앞 버클은 코너의 65 %가 판 밖 칸(같은 자리 대표점으로 고른 띠 차트, 떠 있는 조각 칸이 가리키는 밑 겉면의
	// 테두리 둘레)을 읽어 손 4 cm 누름에 판 칸 잔차 0.17 cm, 장식 정점 1.6 cm. 판 발자국을 그 칸까지 넓히면(일지 300 C2) 띠 칸이 판 안에서 굽힘으로
	// 안 이어진 섬이 되고 미는 방향이 판 노멀로 돌아 찢김이 늘었다. 그래서 판은 그대로 두고, 네 칸이 모두 제 판인 점 가운데 3-D에서 가장 가까운
	// 점을 읽게 옮긴다. 같은 자리 정점(띠와 버클이 나눈 정점)은 같이 옮겨 같은 값을 읽는다 — 그 자리 띠는 버클에 매여 같이 움직인다.
	if (bHard)
	{
		TArray<int32> VertexPlate;   // 메시 정점 → 판 번호(1부터), 0 = 판 아님(살로 남은 조각 포함)
		VertexPlate.Init(0, MD->Vertices().GetArraySize());
		for (const FTriangleID Tri : MD->Triangles().GetElementIDs())
		{
			const int32 H = Hard.PieceOf(Tri);
			const int32 Plate = H > 0 && HardTexels.Results.IsValidIndex(H - 1) ? HardTexels.Results[H - 1] : 0;
			if (Plate > 0)
			{
				for (const FVertexID Corner : MD->GetTriangleVertices(Tri))
				{
					VertexPlate[Corner.GetValue()] = Plate;
				}
			}
		}
		const int32 R = Resolution;
		const auto PlateAt = [&](int32 X, int32 Y) -> int32
		{
			if (X < 0 || Y < 0 || X >= R || Y >= R)
			{
				return 0;
			}
			const int32 Texel = Y * R + X;
			// 떠 있는 판(fb/orn 안 A)의 제 판 칸 = 판을 그리는 칸(비트 15 — 장식 칸, 장식 칸이 없는 판은 발자국 칸). 붙은 판은 그 번호를 가진 표면 칸.
			const int32 Raw = (int32)HardTexels.Piece[Texel];
			const int32 P = Raw & SoftbodyHardSurface::PieceMask;
			if (P <= 0 || !HardTexels.Pieces.IsValidIndex(P - 1))
			{
				return 0;
			}
			if (HardTexels.Pieces[P - 1].bFloating)
			{
				return (Raw & SoftbodyHardSurface::DrawPlateBit) != 0 ? P : 0;
			}
			return Atlas.Kind[Texel] == KindSurface ? P : 0;
		};
		// 판마다 2×2 칸 묶음의 가운데 점(네 칸의 쌍선형 무게가 같은 자리). 네 칸이 모두 그 판인 묶음이 없는 작은 판은 그 판 칸이 가장 많은 묶음.
		struct FPlateBlock
		{
			FVector3d Position;
			FVector2d UV;
			int32 Count;
		};
		const int32 NumPlates = HardTexels.Pieces.Num();
		TArray<TArray<FPlateBlock>> Blocks;
		Blocks.SetNum(NumPlates + 1);
		TArray<int32> BestCount;
		BestCount.Init(0, NumPlates + 1);
		for (int32 Y = 0; Y + 1 < R; ++Y)
		{
			for (int32 X = 0; X + 1 < R; ++X)
			{
				for (int32 k = 0; k < 4; ++k)
				{
					// 네 칸 가운데 어느 판이든 그 판의 묶음으로 센다(한 묶음이 두 판에 걸치면 둘 다).
					const int32 Plate = PlateAt(X + (k & 1), Y + (k >> 1));
					if (Plate <= 0 || Plate > NumPlates)
					{
						continue;
					}
					bool bSeen = false;
					for (int32 j = 0; j < k; ++j)
					{
						bSeen |= PlateAt(X + (j & 1), Y + (j >> 1)) == Plate;
					}
					if (bSeen)
					{
						continue;
					}
					int32 Count = 0;
					FVector3d Sum = FVector3d::ZeroVector;
					for (int32 j = 0; j < 4; ++j)
					{
						if (PlateAt(X + (j & 1), Y + (j >> 1)) == Plate)
						{
							++Count;
							Sum += TexelPosition[(Y + (j >> 1)) * R + X + (j & 1)];
						}
					}
					Blocks[Plate].Add({Sum / Count, FVector2d((X + 1.0) / R, (Y + 1.0) / R), Count});
					BestCount[Plate] = FMath::Max(BestCount[Plate], Count);
				}
			}
		}
		const auto PositionKey = [](const FVector3d& P)
		{
			return FIntVector((int32)FMath::RoundToDouble(P.X * 1000.0), (int32)FMath::RoundToDouble(P.Y * 1000.0),
				(int32)FMath::RoundToDouble(P.Z * 1000.0));
		};
		TMap<FIntVector, TArray<int32>> Groups;
		for (int32 i = 0; i < VertexOrder.Num() && i < TableTexels.Num(); ++i)
		{
			if (TableTexels[i].A > 0.5f)
			{
				Groups.FindOrAdd(PositionKey(VertexPositions[i])).Add(i);
			}
		}
		int32 NumMovedGroups = 0;
		int32 NumMovedVertices = 0;
		double FarthestMove = 0.0;
		TSet<FIntVector> Done;
		for (int32 i = 0; i < VertexOrder.Num() && i < TableTexels.Num(); ++i)
		{
			const int32 Plate = TableTexels[i].A > 0.5f ? VertexPlate[VertexOrder[i].GetValue()] : 0;
			if (Plate <= 0 || Plate > NumPlates || Blocks[Plate].Num() == 0)
			{
				continue;
			}
			const FIntVector Key = PositionKey(VertexPositions[i]);
			if (Done.Contains(Key))
			{
				continue;
			}
			Done.Add(Key);
			// 지금 읽는 네 칸(무게 있는 칸)이 모두 제 판이면 그대로.
			const FVector2d T = FVector2d(TableTexels[i].R, TableTexels[i].G) * (double)R - FVector2d(0.5, 0.5);
			const int32 BX = (int32)FMath::FloorToDouble(T.X);
			const int32 BY = (int32)FMath::FloorToDouble(T.Y);
			bool bInside = true;
			for (int32 k = 0; k < 4 && bInside; ++k)
			{
				const double W = ((k & 1) ? T.X - BX : 1.0 - (T.X - BX)) * ((k >> 1) ? T.Y - BY : 1.0 - (T.Y - BY));
				bInside = W <= 1e-6 || PlateAt(BX + (k & 1), BY + (k >> 1)) == Plate;
			}
			if (bInside)
			{
				continue;
			}
			const FPlateBlock* Nearest = nullptr;
			double NearestSq = TNumericLimits<double>::Max();
			for (const FPlateBlock& Block : Blocks[Plate])
			{
				const double D = FVector3d::DistSquared(Block.Position, VertexPositions[i]);
				if (Block.Count == BestCount[Plate] && D < NearestSq)
				{
					NearestSq = D;
					Nearest = &Block;
				}
			}
			if (!Nearest)
			{
				continue;
			}
			for (const int32 j : Groups[Key])
			{
				TableTexels[j].R = (float)Nearest->UV.X;
				TableTexels[j].G = (float)Nearest->UV.Y;
				++NumMovedVertices;
			}
			++NumMovedGroups;
			FarthestMove = FMath::Max(FarthestMove, FMath::Sqrt(NearestSq));
		}
		int32 NumSmallPlates = 0;
		for (int32 Plate = 1; Plate <= NumPlates; ++Plate)
		{
			NumSmallPlates += BestCount[Plate] > 0 && BestCount[Plate] < 4 ? 1 : 0;
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: %d positions (%d vertices) of rigid pieces read cells outside their plate and now read the nearest point inside it (up to %.3f cm away; %d plates too small for a 2x2 block read their fullest block)"),
			NumMovedGroups, NumMovedVertices, FarthestMove, NumSmallPlates);
		// 확인(fb/orn 안 A): 떠 있는 판 정점 가운데 쌍선형으로 읽는 네 칸에 판을 그리지 않는 칸이 섞인 정점 수. 0이면 장식은 판 자세 그대로다 — 판을 그리는
		// 칸끼리는 한 평면이고 그 칸만 섞으면 평면이다.
		int32 NumFloatingVertices = 0;
		int32 NumFloatingOutside = 0;
		for (int32 i = 0; i < VertexOrder.Num() && i < TableTexels.Num(); ++i)
		{
			const int32 Plate = TableTexels[i].A > 0.5f ? VertexPlate[VertexOrder[i].GetValue()] : 0;
			if (Plate <= 0 || Plate > NumPlates || !HardTexels.Pieces[Plate - 1].bFloating)
			{
				continue;
			}
			++NumFloatingVertices;
			const FVector2d T = FVector2d(TableTexels[i].R, TableTexels[i].G) * (double)R - FVector2d(0.5, 0.5);
			const int32 BX = (int32)FMath::FloorToDouble(T.X);
			const int32 BY = (int32)FMath::FloorToDouble(T.Y);
			for (int32 k = 0; k < 4; ++k)
			{
				const double W = ((k & 1) ? T.X - BX : 1.0 - (T.X - BX)) * ((k >> 1) ? T.Y - BY : 1.0 - (T.Y - BY));
				if (W > 1e-6 && PlateAt(BX + (k & 1), BY + (k >> 1)) != Plate)
				{
					++NumFloatingOutside;
					break;
				}
			}
		}
		if (NumFloatingVertices > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex plates: %d vertices of floating plates, %d of them read a cell that does not draw the plate (0 means every ornament moves as one plate)"),
				NumFloatingVertices, NumFloatingOutside);
		}
	}

	// --- 7. 저장 ---
	const FString MeshName = Skeletal ? Skeletal->GetName() : Data->SourceMesh->GetName();
	const FString Folder = FPackageName::GetLongPackagePath(Data->GetPackage()->GetName());
	Data->Modify();
	Data->CarrierUVTex = SoftbodyBakeShared::CreateTexture32F(Folder, FString::Printf(TEXT("T_%s_CarrierUV"), *MeshName),
		TableSize, TableSize, TableTexels);
	Data->CarrierPositionTex = SoftbodyBakeShared::CreateTexture32F(Folder, FString::Printf(TEXT("T_%s_CarrierPos"), *MeshName),
		Resolution, Resolution, PositionTexels);
	Data->CarrierNormalTex = SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierNrm"), *MeshName),
		Resolution, Resolution, NormalTexels);
	Data->CarrierRedirectTex = SoftbodyBakeShared::CreateTexture32F(Folder, FString::Printf(TEXT("T_%s_CarrierRedirect"), *MeshName),
		Resolution, Resolution, RedirectTexels);
	Data->CarrierSkinIndexTex = Skeletal
		? SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierSkinIdx"), *MeshName),
			Resolution, Resolution, SkinIndexTexels)
		: nullptr;
	Data->CarrierSkinWeightTex = Skeletal
		? SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierSkinW"), *MeshName),
			Resolution, Resolution, SkinWeightTexels)
		: nullptr;
	Data->CarrierLumpTex = bLumps
		? SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierLump"), *MeshName),
			Resolution, Resolution, LumpTexels)
		: nullptr;
	Data->CarrierLumpSideATex = bLumps
		? SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierLumpSideA"), *MeshName),
			Resolution, Resolution, LumpSideA)
		: nullptr;
	Data->CarrierLumpSideBTex = bLumps
		? SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierLumpSideB"), *MeshName),
			Resolution, Resolution, LumpSideB)
		: nullptr;
	Data->CarrierVolumeTex = bVolume
		? SoftbodyBakeShared::CreateTextureG8(Folder, FString::Printf(TEXT("T_%s_CarrierVolume"), *MeshName),
			Resolution, Resolution, VolumeTexels)
		: nullptr;
	Data->CarrierColumnTex = bColumn
		? SoftbodyBakeShared::CreateTextureG8(Folder, FString::Printf(TEXT("T_%s_CarrierColumn"), *MeshName),
			Resolution, Resolution, ColumnTexels)
		: nullptr;
	Data->CarrierPieceTex = bHard
		? SoftbodyBakeShared::CreateTextureG16(Folder, FString::Printf(TEXT("T_%s_CarrierPiece"), *MeshName), Resolution, Resolution, HardTexels.Piece)
		: nullptr;
	Data->CarrierLiftTex = bHard && HardTexels.Lift.Num() > 0
		? SoftbodyBakeShared::CreateTextureG16(Folder, FString::Printf(TEXT("T_%s_CarrierLift"), *MeshName), Resolution, Resolution, HardTexels.Lift)
		: nullptr;
	Data->CarrierLiftScale = bHard ? HardTexels.LiftScale : 0.f;
	// 칸마다 Flex(연속 굽힘): 남은 판의 칸은 판 규칙 값(HardTexels.Softness), 나머지 칸은 정점 Flex를 Depth와 같은 길(PaintCells)로
	// 옮긴 값. 정점 값 = 그 정점이 닿은 삼각형 코너 Flex의 가장 작은 값이고, 남은 판의 정점은 바깥 칸에 흰색으로 든다 — 판 테두리가 둘레 맨살로
	// 번지지 않는다(판 둘레는 지금 그대로). 떠 있는 장식 삼각형은 읽지 않는다(그 밑 셔츠가 칸이다). 버린 조각(감김·너무 큼·가장자리)과 판이 되지
	// 않은 회색은 칠대로 굽는다. 흰색보다 어두운 칸도 판도 없으면 텍스처를 만들지 않는다.
	TArray<uint16> SoftnessTexels;
	double FlexMin = 1.0;
	if (TriangleFlex.Num() > 0)
	{
		TArray<int32> FlexOrderOf;
		FlexOrderOf.Init(INDEX_NONE, MD->Vertices().GetArraySize());
		for (int32 i = 0; i < VertexOrder.Num(); ++i)
		{
			FlexOrderOf[VertexOrder[i].GetValue()] = i;
		}
		const auto OrderIndex = [&FlexOrderOf](FVertexID Vertex)
		{
			return FlexOrderOf.IsValidIndex(Vertex.GetValue()) ? FlexOrderOf[Vertex.GetValue()] : INDEX_NONE;
		};
		// 남은 판(칠 단계 조각 번호 → 굽기 결과 > 0)의 삼각형과 그 정점
		const auto IsKeptPlate = [&Hard, &HardTexels](int32 Piece)
		{
			return Piece > 0 && !Hard.IsFloating(Piece) && HardTexels.Results.IsValidIndex(Piece - 1) && HardTexels.Results[Piece - 1] > 0;
		};
		TArray<uint8> PlateVertex;
		PlateVertex.Init(0, VertexOrder.Num());
		for (const FTriangleID Tri : MD->Triangles().GetElementIDs())
		{
			if (IsKeptPlate(Hard.PieceOf(Tri)))
			{
				for (const FVertexID Corner : MD->GetTriangleVertices(Tri))
				{
					const int32 i = OrderIndex(Corner);
					if (i != INDEX_NONE)
					{
						PlateVertex[i] = 1;
					}
				}
			}
		}
		TArray<double> VertexFlex;
		VertexFlex.Init(-1.0, VertexOrder.Num());
		for (const FTriangleID Tri : MD->Triangles().GetElementIDs())
		{
			const int32 Piece = Hard.PieceOf(Tri);
			if (Hard.IsFloating(Piece) || IsKeptPlate(Piece))
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD->GetTriangleVertices(Tri);
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 i = OrderIndex(Corners[c]);
				const int32 k = Tri.GetValue() * 3 + c;
				if (i == INDEX_NONE || !TriangleFlex.IsValidIndex(k))
				{
					continue;
				}
				const double S = PlateVertex[i] ? 1.0 : FMath::Clamp((double)TriangleFlex[k], 0.0, 1.0);
				VertexFlex[i] = VertexFlex[i] < 0.0 ? S : FMath::Min(VertexFlex[i], S);
			}
		}
		TArray<double> FlexCells;
		PaintCells(VertexFlex, TEXT("Flex"), FlexCells);
		SoftnessTexels.Init(65535, NumTexels);
		int32 NumBelowWhite = 0;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			if (Atlas.Kind[Texel] == KindEmpty)
			{
				continue;
			}
			// 판 칸은 판 규칙 값. 떠 있는 판에서 판을 그리지 않는 칸(장식 밑 셔츠 발자국과 그 번호를 받은 고스트 — fb/orn 안 A에서 살로 그린다)은 아래
			// 정점 칠 길로 제 칠(셔츠 칠)을 쓴다 — 둘레 셔츠와 같은 굳기라 발자국 경계에 굳기 단이 없다.
			const int32 RawPiece = bHard && HardTexels.Piece.IsValidIndex(Texel) ? (int32)HardTexels.Piece[Texel] : 0;
			const int32 PieceHere = RawPiece & SoftbodyHardSurface::PieceMask;
			const bool bClothUnderOrnament = PieceHere > 0 && HardTexels.Pieces.IsValidIndex(PieceHere - 1) && HardTexels.Pieces[PieceHere - 1].bFloating
				&& (RawPiece & SoftbodyHardSurface::DrawPlateBit) == 0;
			const int32 PlateNumber = bClothUnderOrnament ? 0 : PieceHere;
			if (PlateNumber > 0)
			{
				SoftnessTexels[Texel] = HardTexels.Softness.IsValidIndex(Texel) ? HardTexels.Softness[Texel] : 0;
				if (HardTexels.Pieces.IsValidIndex(PlateNumber - 1) && !HardTexels.Pieces[PlateNumber - 1].bFloating)
				{
					FlexMin = FMath::Min(FlexMin, SoftnessTexels[Texel] / 65535.0);
				}
				continue;
			}
			const double S = FMath::Clamp(FlexCells.IsValidIndex(Texel) ? FlexCells[Texel] : 1.0, 0.0, 1.0);
			SoftnessTexels[Texel] = (uint16)FMath::Clamp(FMath::RoundToInt(S * 65535.0), 0, 65535);
			FlexMin = FMath::Min(FlexMin, S);
			NumBelowWhite += SoftnessTexels[Texel] < 65535 ? 1 : 0;
		}
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Flex paint: %d cells outside plates bend stiffer than bare skin (darkest Flex %.3f)"), NumBelowWhite, FlexMin);
		if (NumBelowWhite == 0 && !bHard)
		{
			SoftnessTexels.Reset();
		}
	}
	else if (bHard)
	{
		SoftnessTexels = HardTexels.Softness;
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			const int32 RawPiece = HardTexels.Piece.IsValidIndex(Texel) ? (int32)HardTexels.Piece[Texel] : 0;
			const int32 PlateNumber = RawPiece & SoftbodyHardSurface::PieceMask;
			if (PlateNumber > 0 && HardTexels.Pieces.IsValidIndex(PlateNumber - 1) && !HardTexels.Pieces[PlateNumber - 1].bFloating)
			{
				FlexMin = FMath::Min(FlexMin, SoftnessTexels[Texel] / 65535.0);
			}
			// 떠 있는 판에서 판을 그리지 않는 칸(장식 밑 셔츠, fb/orn 안 A)은 살로 그린다 — 옛 칠 배치는 셔츠 칠을 모르므로 맨살(흰색)
			else if (PlateNumber > 0 && HardTexels.Pieces.IsValidIndex(PlateNumber - 1) && (RawPiece & SoftbodyHardSurface::DrawPlateBit) == 0)
			{
				SoftnessTexels[Texel] = 65535;
			}
		}
	}
	Data->CarrierSoftnessTex = SoftnessTexels.Num() > 0
		? SoftbodyBakeShared::CreateTextureG16(Folder, FString::Printf(TEXT("T_%s_CarrierSoftness"), *MeshName), Resolution, Resolution, SoftnessTexels)
		: nullptr;
	Data->CarrierSoftnessCoverage = bFlexAllCells ? 1 : 0;
	Data->CarrierFlexMin = (float)FlexMin;
	Data->HardPieces = bHard ? MoveTemp(HardTexels.Pieces) : TArray<FSoftbodyHardPiece>();
	Data->HardPieceCells = bHard ? MoveTemp(HardTexels.Cells) : TArray<uint32>();
	Data->HardPieceTiles = bHard ? MoveTemp(HardTexels.Tiles) : TArray<uint32>();
	Data->HardCellPlate = bHard ? MoveTemp(HardTexels.CellPlate) : TArray<FVector4f>();
	Data->HardCellGradRange = bHard ? MoveTemp(HardTexels.CellGradRange) : TArray<FUintVector2>();
	Data->HardGradEntries = bHard ? MoveTemp(HardTexels.GradEntries) : TArray<FVector4f>();
	Data->HardNodes = bHard ? MoveTemp(HardTexels.Nodes) : TArray<FVector4f>();
	Data->HardNodeSrc = bHard ? MoveTemp(HardTexels.NodeSrc) : TArray<uint32>();
	Data->HardNodeSlots = bHard ? MoveTemp(HardTexels.NodeSlots) : TArray<int32>();
	Data->HardModeShapes = bHard ? MoveTemp(HardTexels.ModeShapes) : TArray<uint16>();
	Data->HardModes = bHard ? MoveTemp(HardTexels.Modes) : TArray<FVector2f>();
	Data->HardPieceVersion = bHard ? SoftbodyHardSurface::Version : 0;
	// 미리보기 기록(Hard Surface Pieces 보기): 판이 하나도 남지 않았어도 남긴다 — 칠한 것이 왜 살로 남았는지 보이게.
	Data->HardPieceResults = Hard.Num() > 0 ? MoveTemp(HardTexels.Results) : TArray<int32>();
	Data->HardPieceResultsHash = Hard.Num() > 0 ? Data->HardMaskPaintHash : 0u;
	Data->InertiaLumps = MoveTemp(Lumps);
	Data->CarrierResolution = Resolution;
	Data->CarrierTexelSize = (float)TexelSizeCm;
	// 살 두께의 바닥값(ThicknessAt의 max(…, Cell)): 속이 살갗에 닿은 칸은 두께가 이 값으로 적힌다. 끝의 솟음 거리표(판 5, 일지 252)가 이 값
	// 이하인 칸을 붙은 살갗(뼈에 붙어 솟지 못하는 살갗)으로 보므로 같은 값을 남긴다.
	Data->CarrierThicknessFloor = (float)Cell;
	// 살 있는 칸 두께의 90 %와 중앙값(6d 앞에서 셈). 중앙값은 기록·캡처용이다. 겉껍질 장력과 판 굽힘 길이의
	// 눈금은 고정 길이 L0다(칠한 두께를 쓰면 한 부위의 Depth 칠이 다른 부위의 겉 재료를 바꾼다).
	Data->CarrierThicknessHigh = (float)ThicknessHigh;
	Data->CarrierThicknessMedian = (float)ThicknessMedian;
	// 2 = 고스트 칸이 옆 차트의 정확한 점을 갖는다(ADR-0007 §3.2)
	Data->CarrierVersion = 2;

	// --- 8. 메시 캐리어 UV 채널과 호스트 머티리얼 배선 (ADR-0007 §3.3) ---
	// 스켈레탈 렌더 정점과 나나이트는 UV0~UV3만 쓴다.
	UStaticMesh* StaticSource = Skeletal ? nullptr : Data->SourceMesh.Get();
	const int32 MaxChannels = (Skeletal || (StaticSource && StaticSource->IsNaniteEnabled())) ? 4 : 8;
	// 메시 설명을 고치기 전에 Undo 기록을 남긴다.
	if (Skeletal)
	{
		Skeletal->Modify();
	}
	else if (StaticSource)
	{
		StaticSource->Modify();
	}
	const int32 Channel = WriteCarrierUVChannel(*MD, Data->CarrierUVChannel, Data->TexelUVChannel, MaxChannels,
		bUV0Charts ? &UV0 : nullptr, Atlas);
	if (Channel != INDEX_NONE)
	{
		SoftbodyBakeShared::WriteLodCarrierChannels(Skeletal ? (UObject*)Skeletal : (UObject*)StaticSource, Channel, MaxChannels);
	}
	if (Channel != INDEX_NONE && !CommitSourceMesh(Skeletal, StaticSource, FStaticMeshAttributes(*MD).GetVertexInstanceUVs().GetNumChannels(),
		{Data->TexelUVChannel, Channel}, MaxChannels))
	{
		// 멈춘다: 이어 가면 채널 번호를 적고 머티리얼을 그 채널에 이어, 그리지 못하는 메시와 맞지 않는 데이터·머티리얼이 저장된다.
		// 굽기는 실패로 끝나 저장하지 않는다(USoftbodyAuthoringData::RunBake).
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Could not rebuild the mesh's render data after adding the contact map UV channel. Check the log above for mesh build errors."));
		return false;
	}
	Data->CarrierUVChannel = Channel;
	bool bMaterialsWired = true;
	{
		TArray<UMaterialInterface*> Materials;
		if (Skeletal)
		{
			for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
			{
				Materials.Add(Material.MaterialInterface);
			}
		}
		else
		{
			for (const FStaticMaterial& Material : StaticSource->GetStaticMaterials())
			{
				Materials.Add(Material.MaterialInterface);
			}
		}
		bMaterialsWired = WireCarrierUV(Materials, Channel, Data);
	}
	Data->MarkPackageDirty();

	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("Contact map baked: %s - islands from %s, resolution %d, surface cells %d (%.0f%%), cell size %.3f cm, flesh thickness 90%% %.2f cm, contact map UV channel %d, %d core spheres, vertex to outer surface distance median %.2f / 95%% %.2f / max %.2f"),
		*MeshName, bUV0Charts ? TEXT("UV0 islands") : TEXT("automatic (XAtlas)"), Resolution, NumSurface, 100.0 * NumSurface / NumTexels,
		Data->CarrierTexelSize, Data->CarrierThicknessHigh, Channel, Cores.Num(), Percentile(0.5), Percentile(0.95), Percentile(1.0));
	// 잠들기 상자(ADR-0020): 본마다 살이 콜라이더를 느낄 수 있는 자리. 모양 거리장이 이 뒤에 구워지면 그 단계가 한 번 더 부른다.
	BakeWakeBounds(Data);
	// 솟음 랜드마크 거리표(ADR-0031): 살 있는 거친 칸마다 랜드마크까지의 표면 거리. 없으면 실행 중 솟음이 꺼진다.
	// 표가 텍스처 한 장에 들지 않으면 굽기를 실패로 돌린다 — 솟음과 끌림이 조용히 꺼진 자산을 저장하지 않게.
	bool bBulgeTableTooLarge = false;
	BakeBulgeLandmarksSized(Data, SoftbodyBulgeTable::DefaultLandmarks, bBulgeTableTooLarge);
	// 덩어리 국소 모양(6b2): 줄이 솟음 거친 칸마다라 거리표가 같은 묶음으로 구워졌을 때만 쓴다(USoftbodyMeshData::HasLumpRegions).
	if (RegionRecords > 0 && Data->HasBulgeTable() && Data->BulgeCellBlock == RegionBlock)
	{
		const int32 Coarse = Resolution / RegionBlock;
		const int32 RecordWidth = SoftbodyLumpRegion::RecordTexels * SoftbodyLumpRegion::RecordsPerRow;
		Data->CarrierLumpRegionIndexTex = SoftbodyBakeShared::CreateTextureG16(Folder, FString::Printf(TEXT("T_%s_CarrierLumpRegionIdx"), *MeshName),
			Coarse, Coarse, RegionIndexTexels);
		Data->CarrierLumpRegionTex = SoftbodyBakeShared::CreateTexture16F(Folder, FString::Printf(TEXT("T_%s_CarrierLumpRegion"), *MeshName),
			RecordWidth, RegionRecordTexels.Num() / RecordWidth, RegionRecordTexels);
		Data->LumpRegionVersion = SoftbodyLumpRegion::Version;
		Data->LumpRegionRecords = RegionRecords;
	}
	else
	{
		if (RegionRecords > 0)
		{
			UE_LOG(LogSoftbodyCarrier, Warning,
				TEXT("Lump local shapes were not saved: they need the bulge distance table, which is missing or does not match them (see above). Lumps give way only as a whole. Click Rebake Contact Map."));
		}
		Data->CarrierLumpRegionIndexTex = nullptr;
		Data->CarrierLumpRegionTex = nullptr;
		Data->LumpRegionVersion = 0;
		Data->LumpRegionRecords = 0;
		for (FSoftbodyInertiaLump& Lump : Data->InertiaLumps)
		{
			Lump.RegionStiffness.Reset();
		}
	}
	Data->MarkPackageDirty();
	return bMaterialsWired && !bBulgeTableTooLarge;
}

bool SoftbodyBakeShared::ReadsCarrier(const UMaterialInterface* Material)
{
	if (!Material)
	{
		return false;
	}
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Material->GetAllTextureParameterInfo(Infos, Ids);
	return Infos.ContainsByPredicate([](const FMaterialParameterInfo& Info)
	{
		return Info.Name == FName(TEXT("CarrierUVTex"));
	});
}

int32 SoftbodyBakeShared::CheckMaterialChannels(const USoftbodyMeshData* Data, const UObject* Mesh, const TCHAR* Owner)
{
	if (!Data || !Mesh)
	{
		return 0;
	}
	TArray<UMaterialInterface*> Interfaces;
	if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
	{
		for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
		{
			Interfaces.Add(Material.MaterialInterface);
		}
	}
	else if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
	{
		for (const FStaticMaterial& Material : Static->GetStaticMaterials())
		{
			Interfaces.Add(Material.MaterialInterface);
		}
	}
	int32 Errors = 0;
	TSet<const UMaterial*> Done;
	for (const UMaterialInterface* Interface : Interfaces)
	{
		const UMaterial* Base = Interface && ReadsCarrier(Interface) ? Interface->GetMaterial() : nullptr;
		if (!Base || Done.Contains(Base))
		{
			continue;
		}
		Done.Add(Base);
		for (UMaterialExpression* Expression : Base->GetExpressions())
		{
			const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (!Call || !Call->MaterialFunction || !Call->MaterialFunction->GetName().StartsWith(TEXT("MF_Softbody")))
			{
				continue;
			}
			for (const FFunctionExpressionInput& In : Call->FunctionInputs)
			{
				if (!In.ExpressionInput)
				{
					continue;
				}
				const FName Name = In.ExpressionInput->InputName;
				const int32 Expected = Name == FName(TEXT("TexelUV")) ? Data->TexelUVChannel
					: Name == FName(TEXT("CarrierUV")) ? Data->CarrierUVChannel : -3;
				if (Expected == -3)
				{
					continue;
				}
				const int32 Wired = WiredCoordinateIndex(In.Input);
				// 이어지지 않았거나 TexCoord가 아닌 것(직접 만든 그래프)은 여기서 판단하지 않는다.
				if (Wired < 0 || Wired == Expected)
				{
					continue;
				}
				++Errors;
				UE_LOG(LogSoftbodyCarrier, Error,
					TEXT("%s: material %s feeds %s of %s from UV channel %d, but this mesh's bake uses channel %d. The mesh will read the wrong "
						"data. If the material is shared with another Softbody mesh, give this mesh its own copy of the material; then "
						"press Rebake Contact Map (it reconnects CarrierUV)%s."),
					Owner, *Base->GetName(), *Name.ToString(), *Call->MaterialFunction->GetName(), Wired, Expected,
					Name == FName(TEXT("TexelUV")) ? TEXT(" and set the TexelUV Texture Coordinate node to the right channel") : TEXT(""));
			}
		}
	}
	return Errors;
}

bool SoftbodyBakeShared::ExtractSolidEnvelope(const FDynamicMesh3& Source, double CellSize, int32 TargetTriangles,
	int32 SmoothIterationCount, FDynamicMesh3& OutEnvelope)
{
	return ExtractEnvelope(Source, CellSize, TargetTriangles, OutEnvelope, SmoothIterationCount);
}


bool USoftbodyBakeLibrary::MeasureCarrierAtlas(USoftbodyMeshData* Data, int32 Resolution, float PruneMargin,
	float EnvelopeCellSize, int32 TargetTriangles, bool bUsePaintMask)
{
	USkeletalMesh* Skeletal = Data ? Data->SourceSkeletalMesh.Get() : nullptr;
	FMeshDescription* MD = Skeletal ? Skeletal->GetMeshDescription(0) : nullptr;
	FImage PaintSource;
	if (!MD || !Data->PaintTex || !Data->PaintTex->Source.GetMipImage(PaintSource, 0))
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("MeasureCarrierAtlas: needs baked data for a skeletal mesh, with slot mask paint"));
		return false;
	}
	Resolution = FMath::Clamp((int32)FMath::RoundUpToPowerOfTwo((uint32)FMath::Max(Resolution, 64)), 64, 2048);
	FImage Paint;
	PaintSource.CopyTo(Paint, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	const FLinearColor* PaintPix = (const FLinearColor*)Paint.RawData.GetData();

	// 정점(베이크 순서 = 페인트 칸 순서)마다: 위치, 참여 여부(캐리어를 읽는 슬롯 && 페인트 G > 0), 주 본
	TArray<FVertexID> VertexOrder;
	TArray<FVector3d> Positions;
	TMap<FVertexID, int32> VertexIndex;
	FSkeletalMeshConstAttributes Attributes(*MD);
	TVertexAttributesConstRef<FVector3f> MDPositions = Attributes.GetVertexPositions();
	for (const FVertexID Vertex : MD->Vertices().GetElementIDs())
	{
		VertexIndex.Add(Vertex, VertexOrder.Num());
		VertexOrder.Add(Vertex);
		Positions.Add(FVector3d(MDPositions[Vertex]));
	}
	if (Positions.Num() > Paint.SizeX * Paint.SizeY)
	{
		return false;
	}
	TSet<FName> CarrierSlots;
	for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
	{
		if (SoftbodyBakeShared::ReadsCarrier(Material.MaterialInterface))
		{
			CarrierSlots.Add(Material.MaterialSlotName);
		}
	}
	TPolygonGroupAttributesConstRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	TArray<bool> InCarrierSlot;
	InCarrierSlot.Init(false, Positions.Num());
	for (const FTriangleID Tri : MD->Triangles().GetElementIDs())
	{
		if (!CarrierSlots.Contains(SlotNames[MD->GetTrianglePolygonGroup(Tri)]))
		{
			continue;
		}
		for (const FVertexID Vertex : MD->GetTriangleVertices(Tri))
		{
			InCarrierSlot[VertexIndex[Vertex]] = true;
		}
	}
	TArray<bool> Participates;
	TArray<int32> MainBone;
	Participates.SetNum(Positions.Num());
	MainBone.Init(INDEX_NONE, Positions.Num());
	FSkinWeightsVertexAttributesConstRef SkinWeights = Attributes.GetVertexSkinWeights();
	for (int32 i = 0; i < Positions.Num(); ++i)
	{
		Participates[i] = InCarrierSlot[i] && (!bUsePaintMask || PaintPix[i].G > 1e-3f);
		float Best = -1.f;
		if (SkinWeights.IsValid())
		{
			for (const UE::AnimationCore::FBoneWeight& Weight : SkinWeights.Get(VertexOrder[i]))
			{
				if (Weight.GetWeight() > Best)
				{
					Best = Weight.GetWeight();
					MainBone[i] = Weight.GetBoneIndex();
				}
			}
		}
	}

	// 봉투 (캐리어 베이크와 같은 소스)
	FString SlotLabel;
	FDynamicMesh3 Source;
	TArray<bool> VertexInEnvelopeSlots;
	BuildEnvelopeSource(*MD, ResolveEnvelopeSlots(Data, SlotLabel), Source, VertexInEnvelopeSlots);
	UE_LOG(LogSoftbodyCarrier, Display, TEXT("Contact map measure, outer surface source: %s"), *SlotLabel);
	const double Cell = EnvelopeCellSize > 0.f ? (double)EnvelopeCellSize : Source.GetBounds().MaxDim() / 256.0;
	FPointGrid Grid;
	Grid.Build(Positions, FMath::Max(Cell * 4.0, 1.0));
	FDynamicMesh3 Envelope;
	if (!ExtractEnvelope(Source, Cell, TargetTriangles, Envelope))
	{
		return false;
	}

	// 봉투 점에서 가까운 정점들로 참여 여부·주 본을 붙인다. Margin 안에 참여 정점이 하나라도 있으면 참여.
	auto Classify = [&](const FVector3d& P, double Margin, bool& bOutParticipates, int32& OutBone)
	{
		TArray<TPair<double, int32>> Nearest;
		Grid.FindNearest(P, 8, Nearest);
		bOutParticipates = false;
		OutBone = Nearest.Num() > 0 ? MainBone[Nearest[0].Value] : INDEX_NONE;
		for (const TPair<double, int32>& Hit : Nearest)
		{
			if (Participates[Hit.Value] && (Hit.Key <= Margin * Margin || Hit.Value == Nearest[0].Value))
			{
				bOutParticipates = true;
			}
		}
	};

	if (PruneMargin >= 0.f)
	{
		// 여유 거리 안에 참여 정점이 없는 봉투 삼각형을 잘라낸다. 가까운 8개만 보므로 여유는 정점 간격 몇 배까지만 유효하다.
		TArray<int32> Remove;
		for (const int32 Tri : Envelope.TriangleIndicesItr())
		{
			bool bKeep = false;
			int32 Bone = INDEX_NONE;
			Classify(Envelope.GetTriCentroid(Tri), PruneMargin, bKeep, Bone);
			if (!bKeep)
			{
				Remove.Add(Tri);
			}
		}
		for (const int32 Tri : Remove)
		{
			Envelope.RemoveTriangle(Tri, true, false);
		}
		// 잘라내고 남은 부스러기(2 cm² 미만 조각)는 버린다
		FMeshConnectedComponents Components(&Envelope);
		Components.FindConnectedTriangles();
		int32 Crumbs = 0;
		for (const FMeshConnectedComponents::FComponent& Component : Components.Components)
		{
			double Area = 0.0;
			for (const int32 Tri : Component.Indices)
			{
				Area += Envelope.GetTriArea(Tri);
			}
			if (Area < 2.0)
			{
				for (const int32 Tri : Component.Indices)
				{
					Envelope.RemoveTriangle(Tri, true, false);
				}
				++Crumbs;
			}
		}
		Envelope.CompactInPlace();
		UE_LOG(LogSoftbodyCarrier, Display, TEXT("Contact map measure: trimmed %d outer surface triangles that take no part (margin %.1f cm), dropped %d leftover crumbs"),
			Remove.Num(), PruneMargin, Crumbs);
	}

	FCarrierAtlas Atlas;
	if (!BuildAtlas(Envelope, Resolution, Atlas))
	{
		return false;
	}

	// 차트별: 봉투 면적, 표면 칸, 여백 포함 사각 점유(사각 영역 + 사방 GhostRings + 1칸 — 배치는 모양대로 끼워 이보다 적게 쓴다), 참여 여부, 주 본
	const int32 NumTris = Atlas.TriChart.Num();
	int32 NumCharts = 0;
	for (const int32 Chart : Atlas.TriChart)
	{
		NumCharts = FMath::Max(NumCharts, Chart + 1);
	}
	struct FChart
	{
		int32 Tris = 0;
		double Area = 0.0;
		double ParticipatingArea = 0.0;
		int32 Texels = 0;
		FVector2d Min = FVector2d(TNumericLimits<double>::Max());
		FVector2d Max = FVector2d(-TNumericLimits<double>::Max());
		TMap<int32, double> BoneArea;
	};
	TArray<FChart> Charts;
	Charts.SetNum(NumCharts);
	for (int32 Tri = 0; Tri < NumTris; ++Tri)
	{
		FChart& C = Charts[Atlas.TriChart[Tri]];
		const double Area = Envelope.GetTriArea(Tri);
		bool bIn = false;
		int32 Bone = INDEX_NONE;
		Classify(Envelope.GetTriCentroid(Tri), 0.0, bIn, Bone);
		++C.Tris;
		C.Area += Area;
		C.ParticipatingArea += bIn ? Area : 0.0;
		C.BoneArea.FindOrAdd(Bone) += Area;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector2d P = Atlas.TriUVPixel(Tri, Corner);
			C.Min = FVector2d(FMath::Min(C.Min.X, P.X), FMath::Min(C.Min.Y, P.Y));
			C.Max = FVector2d(FMath::Max(C.Max.X, P.X), FMath::Max(C.Max.Y, P.Y));
		}
	}
	for (int32 Texel = 0; Texel < Atlas.TexelTri.Num(); ++Texel)
	{
		if (Atlas.TexelTri[Texel] != INDEX_NONE)
		{
			++Charts[Atlas.TriChart[Atlas.TexelTri[Texel]]].Texels;
		}
	}

	struct FSum
	{
		int32 Charts = 0;
		int32 Tiny = 0;
		double Area = 0.0;
		int32 Texels = 0;
		double Footprint = 0.0;
	};
	FSum All, In, Out;
	TMap<int32, FSum> ByBone;
	const double Pad = 2.0 * (GhostRings + 1);
	for (const FChart& C : Charts)
	{
		const double Footprint = (FMath::CeilToDouble(C.Max.X - C.Min.X) + Pad) * (FMath::CeilToDouble(C.Max.Y - C.Min.Y) + Pad);
		const bool bIn = C.ParticipatingArea >= 0.5 * C.Area;
		int32 Bone = INDEX_NONE;
		double BestArea = -1.0;
		for (const TPair<int32, double>& Entry : C.BoneArea)
		{
			if (Entry.Value > BestArea)
			{
				BestArea = Entry.Value;
				Bone = Entry.Key;
			}
		}
		FSum* Targets[3] = {&All, bIn ? &In : &Out, &ByBone.FindOrAdd(Bone)};
		for (FSum* S : Targets)
		{
			++S->Charts;
			S->Tiny += C.Tris <= 4 ? 1 : 0;
			S->Area += C.Area;
			S->Texels += C.Texels;
			S->Footprint += Footprint;
		}
	}
	const double TotalTexels = (double)Resolution * Resolution;
	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("Contact map measure %s%s: resolution %d, outer surface triangles %d, area %.0f, islands %d (4 or fewer triangles %d), surface cells %d (%.1f%%), cell size %.3f cm, total footprint with padding %.0f cells (%.0f%%)"),
		PruneMargin >= 0.f ? TEXT("(after trimming)") : TEXT("(all)"), bUsePaintMask ? TEXT("") : TEXT("[slots only]"), Resolution, Envelope.TriangleCount(), All.Area, All.Charts, All.Tiny,
		All.Texels, 100.0 * All.Texels / TotalTexels, FMath::Sqrt(All.Area / FMath::Max(All.Texels, 1)), All.Footprint,
		100.0 * All.Footprint / TotalTexels);
	const TPair<const TCHAR*, const FSum*> Groups[2] = {{TEXT("taking part"), &In}, {TEXT("not taking part"), &Out}};
	for (const TPair<const TCHAR*, const FSum*>& Entry : Groups)
	{
		const FSum& S = *Entry.Value;
		UE_LOG(LogSoftbodyCarrier, Display,
			TEXT("  %s: islands %d (4 or fewer triangles %d), area %.0f%%, surface cells %.0f%%, footprint with padding %.0f%%"),
			Entry.Key, S.Charts, S.Tiny, 100.0 * S.Area / FMath::Max(All.Area, 1e-6),
			100.0 * S.Texels / FMath::Max(All.Texels, 1), 100.0 * S.Footprint / FMath::Max(All.Footprint, 1e-6));
	}
	ByBone.ValueSort([](const FSum& A, const FSum& B) { return A.Footprint > B.Footprint; });
	const FReferenceSkeleton& RefSkeleton = Skeletal->GetRefSkeleton();
	FString Line;
	int32 Listed = 0;
	for (const TPair<int32, FSum>& Entry : ByBone)
	{
		if (Listed++ >= 14)
		{
			break;
		}
		const FName BoneName = Entry.Key != INDEX_NONE ? RefSkeleton.GetBoneName(Entry.Key) : NAME_None;
		Line += FString::Printf(TEXT(" %s(islands %d, small %d, area %.0f%%, footprint %.0f%%)"), *BoneName.ToString(), Entry.Value.Charts,
			Entry.Value.Tiny, 100.0 * Entry.Value.Area / FMath::Max(All.Area, 1e-6),
			100.0 * Entry.Value.Footprint / FMath::Max(All.Footprint, 1e-6));
	}
	UE_LOG(LogSoftbodyCarrier, Display, TEXT("  Main bones with the largest footprint:%s"), *Line);
	return true;
}

bool USoftbodyBakeLibrary::DumpCarrierTileLinks(USoftbodyMeshData* Data, int32 TileSize, int32 MaxLinks)
{
	if (!Data || Data->CarrierResolution <= 0 || TileSize <= 0)
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("DumpCarrierTileLinks: the baked data has no contact map."));
		return false;
	}
	const int32 R = Data->CarrierResolution;
	const int32 NumTexels = R * R;
	TArray<FLinearColor> Position;
	TArray<FLinearColor> Redirect;
	if (!ReadTexture32F(Data->CarrierPositionTex.LoadSynchronous(), NumTexels, Position) || !ReadTexture32F(Data->CarrierRedirectTex.LoadSynchronous(), NumTexels, Redirect))
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("DumpCarrierTileLinks: could not read the contact map texture source."));
		return false;
	}
	const int32 Tiles = FMath::DivideAndRoundUp(R, TileSize);
	TArray<TSet<int32>> Links;
	Links.SetNum(Tiles * Tiles);
	TArray<int32> TileGhosts;
	TileGhosts.Init(0, Tiles * Tiles);
	int32 Ghosts = 0;
	// 표면·고스트 칸이 하나라도 든 묶음(접촉 패스 8×8 묶음이면 빈 묶음을 건너뛸 때 남는 몫)
	TArray<bool> Occupied;
	Occupied.Init(false, Tiles * Tiles);
	int32 Carried = 0;
	for (int32 Texel = 0; Texel < NumTexels; ++Texel)
	{
		const int32 Kind = FMath::RoundToInt(Position[Texel].A);
		if (Kind == KindSurface || Kind == KindGhost)
		{
			++Carried;
			Occupied[(Texel / R / TileSize) * Tiles + (Texel % R) / TileSize] = true;
		}
		if (Kind != KindGhost)
		{
			continue;
		}
		++Ghosts;
		const int32 X = Texel % R;
		const int32 Y = Texel / R;
		const int32 Self = (Y / TileSize) * Tiles + X / TileSize;
		++TileGhosts[Self];
		const int32 TX = FMath::Clamp(FMath::RoundToInt(Redirect[Texel].R), 0, R - 1);
		const int32 TY = FMath::Clamp(FMath::RoundToInt(Redirect[Texel].G), 0, R - 1);
		const int32 Target = (TY / TileSize) * Tiles + TX / TileSize;
		if (Target != Self)
		{
			Links[Self].Add(Target);
		}
	}
	// 분포: 0, 1, 2, 3, 4, 5~8, 9~16, 17~
	int32 Buckets[8] = {};
	int32 WithGhosts = 0;
	int32 MostLinks = 0;
	int64 Pairs = 0;
	TArray<bool> Over;
	Over.Init(false, Tiles * Tiles);
	int32 NumOver = 0;
	for (int32 Tile = 0; Tile < Tiles * Tiles; ++Tile)
	{
		const int32 Count = Links[Tile].Num();
		WithGhosts += TileGhosts[Tile] > 0 ? 1 : 0;
		MostLinks = FMath::Max(MostLinks, Count);
		Pairs += Count;
		const int32 Bucket = Count <= 4 ? Count : (Count <= 8 ? 5 : (Count <= 16 ? 6 : 7));
		++Buckets[Bucket];
		if (Count > MaxLinks)
		{
			Over[Tile] = true;
			++NumOver;
		}
	}
	// 넘친 묶음(늘 켬)과 둘레 한 겹 = 가만히 있어도 도는 묶음
	int32 AlwaysActive = 0;
	for (int32 TY = 0; TY < Tiles; ++TY)
	{
		for (int32 TX = 0; TX < Tiles; ++TX)
		{
			bool bOn = false;
			for (int32 DY = -1; DY <= 1 && !bOn; ++DY)
			{
				for (int32 DX = -1; DX <= 1 && !bOn; ++DX)
				{
					const int32 NX = TX + DX;
					const int32 NY = TY + DY;
					bOn = NX >= 0 && NY >= 0 && NX < Tiles && NY < Tiles && Over[NY * Tiles + NX];
				}
			}
			AlwaysActive += bOn ? 1 : 0;
		}
	}
	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("Relax tiles of %d x %d cells, %d x %d tiles: island border cells %d, tiles with border cells %d, link pairs %lld, most links in one tile %d"),
		TileSize, TileSize, Tiles, Tiles, Ghosts, WithGhosts, Pairs, MostLinks);
	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("  Links per tile: 0=%d 1=%d 2=%d 3=%d 4=%d 5-8=%d 9-16=%d 17+=%d"),
		Buckets[0], Buckets[1], Buckets[2], Buckets[3], Buckets[4], Buckets[5], Buckets[6], Buckets[7]);
	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("  Tiles with more than %d links: %d -> tiles always active, neighbours included: %d (%.1f%%)"),
		MaxLinks, NumOver, AlwaysActive, 100.0 * AlwaysActive / FMath::Max(Tiles * Tiles, 1));
	int32 NumOccupied = 0;
	for (const bool bOccupied : Occupied)
	{
		NumOccupied += bOccupied ? 1 : 0;
	}
	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("  Surface and island border cells %d (%.1f%%), tiles containing such cells %d (%.1f%%)"),
		Carried, 100.0 * Carried / FMath::Max(NumTexels, 1), NumOccupied, 100.0 * NumOccupied / FMath::Max(Tiles * Tiles, 1));
	return true;
}

// --- 잠들기 상자 (ADR-0020) ---

namespace
{
	/** 부위 격자의 부호 있는 거리(메시 로컬 단위). 셰이더 SB_FieldDistanceLocal과 같은 삼중선형 — 격자 밖은 가장 가까운 칸 값 + 격자까지 거리. */
	float SampleFieldDistance(const USoftbodyColliderField& Field, const FSoftbodyColliderFieldPart& Part, const FVector3f& Q)
	{
		const float Voxel = FMath::Max(Part.VoxelSize, 1e-6f);
		const FVector3f G = (Q - Part.Origin) / Voxel;
		const FVector3f C(FMath::Clamp(G.X, 0.f, (float)(Part.Dims.X - 1)), FMath::Clamp(G.Y, 0.f, (float)(Part.Dims.Y - 1)),
			FMath::Clamp(G.Z, 0.f, (float)(Part.Dims.Z - 1)));
		const float Outside = (G - C).Size() * Voxel;
		const int32 IX = FMath::Clamp((int32)C.X, 0, Part.Dims.X - 2);
		const int32 IY = FMath::Clamp((int32)C.Y, 0, Part.Dims.Y - 2);
		const int32 IZ = FMath::Clamp((int32)C.Z, 0, Part.Dims.Z - 2);
		const FVector3f T = C - FVector3f((float)IX, (float)IY, (float)IZ);
		const int32 SY = Part.Dims.X;
		const int32 SZ = Part.Dims.X * Part.Dims.Y;
		const int32 Base = Part.FirstVoxel + IX + SY * IY + SZ * IZ;
		if (!Field.Voxels.IsValidIndex(Base + SZ + SY + 1))
		{
			return TNumericLimits<float>::Max();
		}
		auto V = [&Field](int32 Index)
		{
			FFloat16 Half;
			Half.Encoded = Field.Voxels[Index];
			return Half.GetFloat();
		};
		const float V00 = FMath::Lerp(V(Base), V(Base + 1), T.X);
		const float V10 = FMath::Lerp(V(Base + SY), V(Base + SY + 1), T.X);
		const float V01 = FMath::Lerp(V(Base + SZ), V(Base + SZ + 1), T.X);
		const float V11 = FMath::Lerp(V(Base + SZ + SY), V(Base + SZ + SY + 1), T.X);
		return FMath::Lerp(FMath::Lerp(V00, V10, T.Y), FMath::Lerp(V01, V11, T.Y), T.Z) + Outside;
	}
}

bool USoftbodyBakeLibrary::BakeWakeBounds(USoftbodyMeshData* Data)
{
	if (!Data || !Data->HasSurfaceCarrier())
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Wake-up boxes for sleeping: %s has no contact map. Click Rebake Contact Map."), *GetNameSafe(Data));
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	const int32 Resolution = Data->CarrierResolution;
	const int32 NumTexels = Resolution * Resolution;
	TArray<FLinearColor> Positions;
	TArray<FFloat16Color> Normals;
	TArray<FFloat16Color> SkinIndex;
	TArray<FFloat16Color> SkinWeight;
	USkeletalMesh* Skeletal = Data->SourceSkeletalMesh.Get();
	if (!ReadTexture32F(Data->CarrierPositionTex.LoadSynchronous(), NumTexels, Positions) || !ReadTexture16F(Data->CarrierNormalTex.LoadSynchronous(), NumTexels, Normals)
		|| (Skeletal && (!ReadTexture16F(Data->CarrierSkinIndexTex.LoadSynchronous(), NumTexels, SkinIndex)
			|| !ReadTexture16F(Data->CarrierSkinWeightTex.LoadSynchronous(), NumTexels, SkinWeight))))
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Wake-up boxes for sleeping: could not read the contact map texture source of %s. Click Rebake Contact Map."), *Data->GetName());
		return false;
	}
	const int32 NumBones = Skeletal ? Skeletal->GetRefSkeleton().GetNum() : 1;

	// 캐리어 칸(표면·고스트 — 접촉 패스는 둘 다 계산한다)마다 정지 위치, 광선 끝(살 두께 × (1 + SB_CORE_FADE) 안쪽), 두께, 움직이는 본.
	struct FCell
	{
		FVector3f P;
		FVector3f End;
		float Thickness;
		int32 Bones[4];
	};
	constexpr float CoreFade = 0.5f;   // 셰이더 SB_CORE_FADE
	TArray<FCell> Cells;
	Cells.Reserve(NumTexels / 2);
	for (int32 Texel = 0; Texel < NumTexels; ++Texel)
	{
		const int32 Kind = FMath::RoundToInt(Positions[Texel].A);
		if (Kind != 1 && Kind != 2)
		{
			continue;
		}
		FCell& Cell = Cells.AddDefaulted_GetRef();
		Cell.P = FVector3f(Positions[Texel].R, Positions[Texel].G, Positions[Texel].B);
		const FVector3f N = FVector3f(Normals[Texel].R.GetFloat(), Normals[Texel].G.GetFloat(), Normals[Texel].B.GetFloat()).GetSafeNormal();
		Cell.Thickness = FMath::Max(Normals[Texel].A.GetFloat(), 0.f);
		Cell.End = Cell.P - N * (Cell.Thickness * (1.f + CoreFade));
		for (int32 k = 0; k < 4; ++k)
		{
			Cell.Bones[k] = INDEX_NONE;
			if (!Skeletal)
			{
				Cell.Bones[k] = k == 0 ? 0 : INDEX_NONE;
				continue;
			}
			// 셰이더와 같은 문턱(1e-4 아래 무게는 안 섞는다)
			if ((&SkinWeight[Texel].R)[k].GetFloat() <= 1e-4f)
			{
				continue;
			}
			const int32 Bone = FMath::RoundToInt((&SkinIndex[Texel].R)[k].GetFloat());
			Cell.Bones[k] = Bone >= 0 && Bone < NumBones ? Bone : INDEX_NONE;
		}
	}

	TArray<FBox3f> Bounds;
	Bounds.Init(FBox3f(ForceInit), NumBones);
	for (const FCell& Cell : Cells)
	{
		for (int32 k = 0; k < 4; ++k)
		{
			if (Cell.Bones[k] != INDEX_NONE)
			{
				Bounds[Cell.Bones[k]] += Cell.P;
				Bounds[Cell.Bones[k]] += Cell.End;
			}
		}
	}

	// 자기 접촉 부위(타깃 스켈레탈 메시의 모양 거리장에서 제 몸도 누르는 부위, ADR-0004 §11)마다: 기본 포즈에서 부위와 살 두께보다
	// 멀리 떨어진 칸(접촉 패스가 누르는 칸, SB_FieldDepth의 Apart > 0)만 그 부위의 본별 상자에 넣는다. 순서는 거리장 부위 순서다.
	TArray<FName> SelfParts;
	TArray<FBox3f> SelfBounds;
	const USoftbodyColliderField* SelfField = Skeletal ? USoftbodyColliderField::FindOnAsset(Skeletal) : nullptr;
	if (SelfField && SelfField->IsUsable())
	{
		TArray<int32> SelfPartIndices;
		for (int32 PartIndex = 0; PartIndex < SelfField->Parts.Num(); ++PartIndex)
		{
			if (SelfField->Parts[PartIndex].bPressesSelf)
			{
				SelfPartIndices.Add(PartIndex);
				SelfParts.Add(SelfField->Parts[PartIndex].Bone);
			}
		}
		const int32 NumParts = SelfPartIndices.Num();
		SelfBounds.Init(FBox3f(ForceInit), NumParts * NumBones);
		ParallelFor(NumParts, [&](int32 Slot)
		{
			const FSoftbodyColliderFieldPart& Part = SelfField->Parts[SelfPartIndices[Slot]];
			FBox3f* Row = &SelfBounds[Slot * NumBones];
			for (const FCell& Cell : Cells)
			{
				if (SampleFieldDistance(*SelfField, Part, Cell.P) <= Cell.Thickness)
				{
					continue;
				}
				for (int32 k = 0; k < 4; ++k)
				{
					if (Cell.Bones[k] != INDEX_NONE)
					{
						Row[Cell.Bones[k]] += Cell.P;
						Row[Cell.Bones[k]] += Cell.End;
					}
				}
			}
		});
	}

	int32 NumBoxed = 0;
	for (const FBox3f& Box : Bounds)
	{
		NumBoxed += Box.IsValid ? 1 : 0;
	}
	Data->Modify();
	Data->CarrierWakeBounds = MoveTemp(Bounds);
	Data->CarrierWakeSelfParts = MoveTemp(SelfParts);
	Data->CarrierWakeSelfBounds = MoveTemp(SelfBounds);
	Data->MarkPackageDirty();
	UE_LOG(LogSoftbodyCarrier, Display,
		TEXT("Wake-up boxes for sleeping: %s - %d cells, of %d bones %d have a box, %d self contact parts (%.1f s)"), *Data->GetName(), Cells.Num(), NumBones,
		NumBoxed, Data->CarrierWakeSelfParts.Num(), FPlatformTime::Seconds() - Start);
	return true;
}

namespace SoftbodyBakeShared
{
	FSeparatedSides ResolveSeparatedPairs(const USkeletalMesh* Mesh, TConstArrayView<FSoftbodySeparatedPair> Pairs)
	{
		FSeparatedSides Out;
		if (!Mesh)
		{
			return Out;
		}
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const int32 NumBones = Ref.GetNum();
		Out.BoneSide.Init(0, NumBones);

		// 적은 본끼리의 합집합 찾기: Link = 위 본(INDEX_NONE = 어느 쌍에도 적지 않은 본, 자기 자신 = 뿌리), Flip = 위 본과 다른 묶음인가.
		TArray<int32> Link;
		TArray<uint8> Flip;
		Link.Init(INDEX_NONE, NumBones);
		Flip.Init(0, NumBones);
		const auto Find = [&Link, &Flip](int32 Bone, uint8& OutFlip)
		{
			uint8 F = 0;
			while (Link[Bone] != Bone)
			{
				F ^= Flip[Bone];
				Bone = Link[Bone];
			}
			OutFlip = F;
			return Bone;
		};
		// A와 B를 Apart(1 = 다른 묶음, 0 = 같은 묶음)로 잇는다. 이미 반대로 이어져 있으면 거짓.
		const auto Join = [&Link, &Flip, &Find](int32 A, int32 B, uint8 Apart)
		{
			uint8 FA = 0;
			uint8 FB = 0;
			const int32 RA = Find(A, FA);
			const int32 RB = Find(B, FB);
			if (RA == RB)
			{
				return (uint8)(FA ^ FB) == Apart;
			}
			Link[RB] = RA;
			Flip[RB] = FA ^ FB ^ Apart;
			return true;
		};
		TArray<int32> UsedPairs;
		TArray<int32> UsedFirstBone;   // 쓴 쌍마다 Side A의 첫 본(무리를 찾고, 그 쪽을 묶음 1로 둘 때)
		for (int32 PairIndex = 0; PairIndex < Pairs.Num(); ++PairIndex)
		{
			const FSoftbodySeparatedPair& Pair = Pairs[PairIndex];
			TArray<int32> Sides[2];
			for (int32 S = 0; S < 2; ++S)
			{
				for (const FName& Name : S == 0 ? Pair.SideA : Pair.SideB)
				{
					if (Name.IsNone())
					{
						continue;
					}
					const int32 Bone = Ref.FindBoneIndex(Name);
					if (Bone == INDEX_NONE)
					{
						Out.Problems.Add(FString::Printf(TEXT("Pair %d: bone '%s' is not in %s."), PairIndex, *Name.ToString(), *Mesh->GetName()));
						continue;
					}
					Sides[S].AddUnique(Bone);
				}
			}
			if (Sides[0].Num() == 0 || Sides[1].Num() == 0)
			{
				Out.Problems.Add(FString::Printf(TEXT("Pair %d has an empty side, so it is not used."), PairIndex));
				continue;
			}
			bool bBoth = false;
			for (const int32 Bone : Sides[0])
			{
				bBoth |= Sides[1].Contains(Bone);
			}
			if (bBoth)
			{
				Out.Problems.Add(FString::Printf(TEXT("Pair %d lists the same bone on both sides, so it is not used."), PairIndex));
				continue;
			}
			// 이 쌍이 앞 쌍들과 모순되면(예: 왼쪽·오른쪽·가운데가 서로 떨어져야 함 — 묶음이 셋 필요) 이 쌍만 되돌린다.
			const TArray<int32> SavedLink = Link;
			const TArray<uint8> SavedFlip = Flip;
			for (int32 S = 0; S < 2; ++S)
			{
				for (const int32 Bone : Sides[S])
				{
					if (Link[Bone] == INDEX_NONE)
					{
						Link[Bone] = Bone;
					}
				}
			}
			bool bFits = true;
			for (int32 S = 0; S < 2; ++S)
			{
				for (int32 i = 1; i < Sides[S].Num(); ++i)
				{
					bFits &= Join(Sides[S][0], Sides[S][i], 0);
				}
			}
			bFits &= Join(Sides[0][0], Sides[1][0], 1);
			if (!bFits)
			{
				Link = SavedLink;
				Flip = SavedFlip;
				Out.Problems.Add(FString::Printf(TEXT("Pair %d conflicts with the pairs above it: the contact map keeps only two groups apart, and this would need ")
					TEXT("three. It is not used."), PairIndex));
				continue;
			}
			UsedPairs.Add(PairIndex);
			UsedFirstBone.Add(Sides[0][0]);
		}
		Out.NumPairsUsed = UsedPairs.Num();
		if (UsedPairs.Num() == 0)
		{
			return Out;
		}

		// 무리(이어진 쌍들) 번호와, 적은 본마다 그 무리 안에서의 묶음(0·1).
		TMap<int32, int32> GroupOfRoot;
		TArray<TArray<int32>> GroupPairs;   // 무리 → 쌍 번호들(문제 글에)
		TArray<uint8> GroupBase;            // 무리 첫 쌍의 Side A가 묶음 1이 되는 뒤집기
		for (int32 k = 0; k < UsedPairs.Num(); ++k)
		{
			uint8 F = 0;
			const int32 Root = Find(UsedFirstBone[k], F);
			int32* Group = GroupOfRoot.Find(Root);
			if (!Group)
			{
				Group = &GroupOfRoot.Add(Root, GroupPairs.Num());
				GroupPairs.AddDefaulted();
				GroupBase.Add(F);
			}
			GroupPairs[*Group].Add(UsedPairs[k]);
		}
		const int32 NumGroups = GroupPairs.Num();
		// 본마다 따르는 적은 본(자기 또는 가장 가까운 적은 조상). 부모가 늘 앞 번호라 한 번 훑는다.
		TArray<int32> Owner;
		Owner.Init(INDEX_NONE, NumBones);
		TArray<int32> OwnerGroup;
		TArray<uint8> OwnerFlip;
		OwnerGroup.Init(INDEX_NONE, NumBones);
		OwnerFlip.Init(0, NumBones);
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (Link[Bone] != INDEX_NONE)
			{
				Owner[Bone] = Bone;
				uint8 F = 0;
				OwnerGroup[Bone] = GroupOfRoot.FindChecked(Find(Bone, F));
				OwnerFlip[Bone] = F;
			}
			else
			{
				const int32 Parent = Ref.GetParentIndex(Bone);
				Owner[Bone] = Parent != INDEX_NONE ? Owner[Parent] : INDEX_NONE;
			}
		}

		// 무리 사이에서 어느 쪽을 같은 묶음에 둘지: 메시에서 맞닿은 쪽끼리 같은 묶음이 되게(쌍이 아닌 맞닿은 부위가 떨어지지 않게). 정점마다
		// 가장 많이 걸린 무리·묶음(웨이트 0.5 이상)을 정하고 삼각형 변이 두 무리를 이으면 센다: Same = 무리 안 묶음이 같은 변, Differ = 다른 변.
		TArray<uint8> GroupFlip;
		GroupFlip.Init(0, NumGroups);
		if (NumGroups > 1 && NumGroups <= 12)
		{
			TArray<int64> Same;
			TArray<int64> Differ;
			Same.Init(0, NumGroups * NumGroups);
			Differ.Init(0, NumGroups * NumGroups);
			if (const FMeshDescription* MD = Mesh->GetMeshDescription(0))
			{
				const FSkinWeightsVertexAttributesConstRef SkinWeights = FSkeletalMeshConstAttributes(*MD).GetVertexSkinWeights();
				TArray<int32> VertexMark;   // 무리 × 2 + 묶음, 없으면 INDEX_NONE
				VertexMark.Init(INDEX_NONE, MD->Vertices().GetArraySize());
				if (SkinWeights.IsValid())
				{
					for (const FVertexID VertID : MD->Vertices().GetElementIDs())
					{
						TMap<int32, float> Sum;
						for (UE::AnimationCore::FBoneWeight BW : SkinWeights.Get(VertID))
						{
							const int32 Bone = (int32)BW.GetBoneIndex();
							const int32 Listed = Bone >= 0 && Bone < NumBones ? Owner[Bone] : INDEX_NONE;
							if (Listed != INDEX_NONE)
							{
								const int32 Group = OwnerGroup[Listed];
								Sum.FindOrAdd(Group * 2 + (OwnerFlip[Listed] ^ GroupBase[Group])) += BW.GetWeight();
							}
						}
						for (const TPair<int32, float>& Pair : Sum)
						{
							if (Pair.Value >= 0.5f)
							{
								VertexMark[VertID.GetValue()] = Pair.Key;
							}
						}
					}
				}
				for (const FTriangleID TriID : MD->Triangles().GetElementIDs())
				{
					const TArrayView<const FVertexID> Corners = MD->GetTriangleVertices(TriID);
					for (int32 c = 0; c < 3; ++c)
					{
						const int32 MA = VertexMark[Corners[c].GetValue()];
						const int32 MB = VertexMark[Corners[(c + 1) % 3].GetValue()];
						if (MA == INDEX_NONE || MB == INDEX_NONE || MA / 2 == MB / 2)
						{
							continue;
						}
						const int32 Index = FMath::Min(MA / 2, MB / 2) * NumGroups + FMath::Max(MA / 2, MB / 2);
						((MA & 1) == (MB & 1) ? Same : Differ)[Index] += 1;
					}
				}
			}
			// 무리 0은 그대로 두고 나머지 뒤집기를 모두 해 본다(무리 12개까지 2048가지). 같으면 뒤집지 않은 쪽(앞 쪽).
			int64 BestCost = TNumericLimits<int64>::Max();
			int32 BestMask = 0;
			for (int32 Mask = 0; Mask < (1 << (NumGroups - 1)); ++Mask)
			{
				int64 Cost = 0;
				for (int32 i = 0; i < NumGroups; ++i)
				{
					for (int32 j = i + 1; j < NumGroups; ++j)
					{
						const uint8 Fi = i == 0 ? 0 : (uint8)((Mask >> (i - 1)) & 1);
						const uint8 Fj = (uint8)((Mask >> (j - 1)) & 1);
						// 뒤집기가 같으면 묶음이 다른 변이 떨어지고, 다르면 묶음이 같던 변이 떨어진다.
						Cost += (Fi ^ Fj) ? Same[i * NumGroups + j] : Differ[i * NumGroups + j];
					}
				}
				if (Cost < BestCost)
				{
					BestCost = Cost;
					BestMask = Mask;
				}
			}
			for (int32 i = 1; i < NumGroups; ++i)
			{
				GroupFlip[i] = (uint8)((BestMask >> (i - 1)) & 1);
			}
			for (int32 i = 0; i < NumGroups; ++i)
			{
				for (int32 j = i + 1; j < NumGroups; ++j)
				{
					const int64 Apart = (GroupFlip[i] ^ GroupFlip[j]) ? Same[i * NumGroups + j] : Differ[i * NumGroups + j];
					if (Apart > 0)
					{
						const auto List = [](const TArray<int32>& Indices)
						{
							return FString::JoinBy(Indices, TEXT(", "), [](int32 Index) { return FString::FromInt(Index); });
						};
						Out.Problems.Add(FString::Printf(TEXT("Pairs %s and pairs %s touch on the mesh (%lld edges), and some of the touching flesh is ")
							TEXT("kept apart although it is not paired. Separate them less, or pair them."), *List(GroupPairs[i]), *List(GroupPairs[j]), Apart));
					}
				}
			}
		}

		// 본마다 묶음 1·2: 무리 첫 쌍의 Side A 쪽이 묶음 1(GroupBase), 무리 뒤집기를 더한다.
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			const int32 Listed = Owner[Bone];
			if (Listed != INDEX_NONE)
			{
				const int32 Group = OwnerGroup[Listed];
				Out.BoneSide[Bone] = (uint8)(1 + (OwnerFlip[Listed] ^ GroupBase[Group] ^ GroupFlip[Group]));
			}
		}
		return Out;
	}
}

bool USoftbodyBakeLibrary::BakeSeparatedParts(USoftbodyMeshData* Data, const TArray<FSoftbodySeparatedPair>& Pairs)
{
	if (!Data)
	{
		return false;
	}
	// 몫 텍스처를 떼면 실행 중 부위 나누기가 꺼진다.
	const auto Remove = [Data]()
	{
		if (!Data->CarrierSeparationTex.IsNull())
		{
			Data->Modify();
			Data->CarrierSeparationTex = nullptr;
			Data->MarkPackageDirty();
		}
	};
	USkeletalMesh* Skeletal = Data->SourceSkeletalMesh.Get();
	if (Pairs.Num() == 0)
	{
		Remove();
		return true;
	}
	if (!Skeletal)
	{
		UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Separated Pairs on %s: only skeletal meshes have bones to separate, so nothing is separated."), *Data->GetName());
		Remove();
		return true;
	}
	if (!Data->HasSurfaceCarrier())
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Separated Pairs on %s: there is no contact map yet. Click Rebake Contact Map."), *Data->GetName());
		return false;
	}
	// 본마다 묶음(0 = 어느 쌍에도 없음, 1·2) — 저작 에셋 미리보기의 Separated Parts 보기와 같은 함수.
	const SoftbodyBakeShared::FSeparatedSides Sides = SoftbodyBakeShared::ResolveSeparatedPairs(Skeletal, Pairs);
	for (const FString& Problem : Sides.Problems)
	{
		UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Separated Pairs on %s: %s"), *Data->GetName(), *Problem);
	}
	if (Sides.NumPairsUsed == 0)
	{
		UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Separated Pairs on %s: no pair can be used, so nothing is separated."), *Data->GetName());
		Remove();
		return true;
	}
	const TArray<uint8>& Member = Sides.BoneSide;
	const int32 NumBones = Member.Num();
	const int32 Resolution = Data->CarrierResolution;
	const int32 NumTexels = Resolution * Resolution;
	TArray<FFloat16Color> SkinIndex;
	TArray<FFloat16Color> SkinWeight;
	if (!ReadTexture16F(Data->CarrierSkinIndexTex.LoadSynchronous(), NumTexels, SkinIndex)
		|| !ReadTexture16F(Data->CarrierSkinWeightTex.LoadSynchronous(), NumTexels, SkinWeight))
	{
		UE_LOG(LogSoftbodyCarrier, Error, TEXT("Separated Pairs on %s: could not read the contact map skin weights. Click Rebake Contact Map."), *Data->GetName());
		return false;
	}
	// 칸마다 두 묶음 몫 = 그 묶음 본의 웨이트 합(접촉 패스가 칸을 움직이는 상위 4본, 1e-4 아래 무게는 안 섞는다 — 셰이더와 같은 문턱). 옷이 두 묶음에
	// 섞여 걸린 칸은 섞인 대로 남는다.
	TArray<uint16> Texels;
	Texels.Init(0, NumTexels);
	int32 Counts[3] = {0, 0, 0};   // 묶음 1만, 묶음 2만, 둘 다
	for (int32 Texel = 0; Texel < NumTexels; ++Texel)
	{
		float Share[2] = {0.f, 0.f};
		for (int32 k = 0; k < 4; ++k)
		{
			const float Weight = (&SkinWeight[Texel].R)[k].GetFloat();
			const int32 Bone = FMath::RoundToInt((&SkinIndex[Texel].R)[k].GetFloat());
			if (Weight > 1e-4f && Bone >= 0 && Bone < NumBones && Member[Bone] != 0)
			{
				Share[Member[Bone] - 1] += Weight;
			}
		}
		const uint32 A = (uint32)FMath::Clamp(FMath::RoundToInt(Share[0] * 255.f), 0, 255);
		const uint32 B = (uint32)FMath::Clamp(FMath::RoundToInt(Share[1] * 255.f), 0, 255);
		Texels[Texel] = (uint16)(A | (B << 8));
		Counts[0] += A > 0 && B == 0 ? 1 : 0;
		Counts[1] += B > 0 && A == 0 ? 1 : 0;
		Counts[2] += A > 0 && B > 0 ? 1 : 0;
	}
	if (Counts[0] + Counts[2] == 0 || Counts[1] + Counts[2] == 0)
	{
		UE_LOG(LogSoftbodyCarrier, Warning, TEXT("Separated Pairs on %s: no contact map cell is skinned to one of the two sides, so nothing is separated. ")
			TEXT("The Separated Parts preview view shows where each side is."), *Data->GetName());
		Remove();
		return true;
	}
	const FString Folder = FPackageName::GetLongPackagePath(Data->GetPackage()->GetName());
	Data->Modify();
	Data->CarrierSeparationTex = SoftbodyBakeShared::CreateTextureG16(Folder, FString::Printf(TEXT("T_%s_CarrierSeparation"), *Skeletal->GetName()),
		Resolution, Resolution, Texels);
	Data->MarkPackageDirty();
	UE_LOG(LogSoftbodyCarrier, Display, TEXT("Separated Pairs baked: %s - %d pairs, cells only on side 1 %d, only on side 2 %d, on both %d"), *Data->GetName(),
		Sides.NumPairsUsed, Counts[0], Counts[1], Counts[2]);
	return true;
}
