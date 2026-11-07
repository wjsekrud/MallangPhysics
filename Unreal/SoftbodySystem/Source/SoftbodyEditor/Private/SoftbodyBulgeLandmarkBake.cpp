// Copyright (c) 2026 wjsekrud

// ADR-0031 솟음 랜드마크 거리표 굽기: 솟음 거친 격자(캐리어 칸 B×B가 한 칸, SoftbodyBulgeTable::BlockFor)의 살 있는 칸마다, 살 표면에 고르게
// 뽑은 랜드마크 L개까지의 표면 거리를 적는다. 거리는 살 그래프(이웃한 거친 칸 — 옆 넷·같은 차트 안 대각선 넷(판 3)·3D 거리 검사를 지난 나이트 여덟(판 4) —
// + 이음매 건너편, 살 없는 칸은 지나지 못한다)에서 다익스트라로 잰다. 런타임(SoftbodySurfaceField.usf 솟음·끌림 부분)은 두 칸의 표면 거리를 max_m |d_im − d_jm|(참값의 하한)으로
// 어림해 눌린 칸들의 커널을 반복 없이 더한다(솟음 ADR-0031, 끌림 ADR-0033). 판 5부터 줄 끝에 그 칸에서 가장 가까운 붙은 살갗(뼈에 붙어
// 솟지 못하는 살갗)까지 살을 따라 간 거리도 적는다 — 솟음이 그쪽에서 턱 없이 0으로 잦아든다(일지 252). 캐리어 굽기(BakeSurfaceCarrier)가
// 끝에 부르고, Tools/bake_bulge_landmarks.py가 옛 데이터에 붙인다. 판 6부터 줄 끝에 그 줄의 살 조각 번호(살 그래프의 이어진 덩어리)도 적는다 —
// 압력 행렬과 긴 몫 모으기가 "같은 살 조각인가"를 이 번호로 가른다.
#include "SoftbodyBakeLibrary.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Misc/PackageName.h"
#include "RHIGlobals.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyMeshData.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyBulgeTable, Log, All);

namespace
{
	// 칸 종류(캐리어 굽기 SoftbodyCarrierBake.cpp의 KindSurface·KindGhost와 같은 값). 이름을 따로 둔다 — 에디터 모듈 유니티 빌드에서 두 파일이 한 묶음에
	// 들면 같은 이름의 익명 네임스페이스 상수가 부딪힌다.
	constexpr int32 LandmarkKindSurface = 1;
	constexpr int32 LandmarkKindGhost = 2;
	constexpr uint16 Unreachable = 0xffff;
	/** 이보다 작은 살 조각(거친 칸 수)에는 랜드마크를 두지 않는다: 그 칸들의 거리는 모두 "닿지 않음"이라 서로 3D 직선거리로만 묶인다. */
	constexpr int32 MinComponentCells = 4;
	/**
	 * 캐리어 굽기의 봉투 칸 자동 규칙(Envelope Cell Size 0): 바깥 면의 가장 긴 변 / 이 값(BakeSurfaceCarrier). 살 두께 바닥값을 적기 전에
	 * 구운 데이터에서 그 값을 어림할 때만 쓴다.
	 */
	constexpr float EnvelopeCellsPerSide = 256.f;

	/** 살 있는 거친 칸들의 그래프(CSR). 간선 무게 = 두 칸 정지 위치 평균 사이 거리(메시 로컬). */
	struct FFleshGraph
	{
		int32 Num = 0;
		TArray<int32> Offsets;
		TArray<int32> Neighbors;
		TArray<float> Weights;
	};

	struct FHeapLess
	{
		bool operator()(const TPair<float, int32>& A, const TPair<float, int32>& B) const { return A.Key < B.Key; }
	};

	/** Dist에 출발 거리를 넣어 둔 칸(유한한 값)들에서 한꺼번에 시작하는 다익스트라. 끝나면 Dist = 가장 가까운 출발 칸을 거친 가장 짧은 거리. */
	void DijkstraFrom(const FFleshGraph& Graph, TArray<float>& Dist)
	{
		TArray<TPair<float, int32>> Heap;
		for (int32 Node = 0; Node < Graph.Num; ++Node)
		{
			if (Dist[Node] < TNumericLimits<float>::Max())
			{
				Heap.HeapPush(TPair<float, int32>(Dist[Node], Node), FHeapLess());
			}
		}
		while (Heap.Num() > 0)
		{
			TPair<float, int32> Top;
			Heap.HeapPop(Top, FHeapLess(), EAllowShrinking::No);
			if (Top.Key > Dist[Top.Value])
			{
				continue;
			}
			for (int32 Edge = Graph.Offsets[Top.Value]; Edge < Graph.Offsets[Top.Value + 1]; ++Edge)
			{
				const int32 Next = Graph.Neighbors[Edge];
				const float D = Top.Key + Graph.Weights[Edge];
				if (D < Dist[Next])
				{
					Dist[Next] = D;
					Heap.HeapPush(TPair<float, int32>(D, Next), FHeapLess());
				}
			}
		}
	}

	void Dijkstra(const FFleshGraph& Graph, int32 Source, TArray<float>& Dist)
	{
		Dist.Init(TNumericLimits<float>::Max(), Graph.Num);
		Dist[Source] = 0.f;
		DijkstraFrom(Graph, Dist);
	}
}

bool USoftbodyBakeLibrary::BakeBulgeLandmarks(USoftbodyMeshData* Data, int32 LandmarkCount)
{
	bool bTooLarge = false;
	return BakeBulgeLandmarksSized(Data, LandmarkCount, bTooLarge);
}

bool USoftbodyBakeLibrary::BakeBulgeLandmarksSized(USoftbodyMeshData* Data, int32 LandmarkCount, bool& bOutTooLarge)
{
	bOutTooLarge = false;
	if (!Data || !Data->HasSurfaceCarrier())
	{
		UE_LOG(LogSoftbodyBulgeTable, Error, TEXT("BakeBulgeLandmarks: %s has no contact map. Click Rebake Contact Map."), *GetNameSafe(Data));
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	const int32 Resolution = Data->CarrierResolution;
	const int32 NumTexels = Resolution * Resolution;
	TArray<FLinearColor> Positions;
	TArray<FFloat16Color> Normals;
	TArray<FLinearColor> Redirect;
	if (!SoftbodyBakeShared::ReadTextureSource32F(Data->CarrierPositionTex.LoadSynchronous(), NumTexels, Positions)
		|| !SoftbodyBakeShared::ReadTextureSource16F(Data->CarrierNormalTex.LoadSynchronous(), NumTexels, Normals)
		|| !SoftbodyBakeShared::ReadTextureSource32F(Data->CarrierRedirectTex.LoadSynchronous(), NumTexels, Redirect))
	{
		UE_LOG(LogSoftbodyBulgeTable, Error, TEXT("BakeBulgeLandmarks: could not read the contact map texture source of %s. Click Rebake Contact Map."),
			*Data->GetName());
		return false;
	}
	LandmarkCount = FMath::Clamp(LandmarkCount, SoftbodyBulgeTable::MinLandmarks, SoftbodyBulgeTable::MaxLandmarks);

	// --- 1. 거친 격자의 살 있는 칸(줄 번호 = 줄 우선 순서) ---
	// 런타임(CoarseInitCS·CoarseRowCS)과 같은 정의: 종류 1(표면)이고 두께 > 0인 칸이 하나라도 든 거친 칸.
	const int32 Block = SoftbodyBulgeTable::BlockFor(Data->CarrierTexelSize, Resolution);
	const int32 Coarse = Resolution / Block;
	const int32 NumCells = Coarse * Coarse;
	auto KindAt = [&](int32 X, int32 Y) { return FMath::RoundToInt(Positions[Y * Resolution + X].A); };
	auto FleshAt = [&](int32 X, int32 Y)
	{
		const int32 Texel = Y * Resolution + X;
		return KindAt(X, Y) == LandmarkKindSurface && Normals[Texel].A.GetFloat() > 0.f;
	};
	TArray<int32> RowOf;
	RowOf.Init(INDEX_NONE, NumCells);
	TArray<FVector3f> Rest;
	TArray<float> Thickness;
	TArray<int32> CellOfRow;
	for (int32 Cell = 0; Cell < NumCells; ++Cell)
	{
		const int32 CX = (Cell % Coarse) * Block;
		const int32 CY = (Cell / Coarse) * Block;
		FVector3f Sum = FVector3f::ZeroVector;
		float SumThickness = 0.f;
		int32 Count = 0;
		for (int32 Y = CY; Y < CY + Block; ++Y)
		{
			for (int32 X = CX; X < CX + Block; ++X)
			{
				if (FleshAt(X, Y))
				{
					const FLinearColor& P = Positions[Y * Resolution + X];
					Sum += FVector3f(P.R, P.G, P.B);
					SumThickness += Normals[Y * Resolution + X].A.GetFloat();
					++Count;
				}
			}
		}
		if (Count > 0)
		{
			RowOf[Cell] = Rest.Num();
			Rest.Add(Sum / (float)Count);
			Thickness.Add(SumThickness / (float)Count);
			CellOfRow.Add(Cell);
		}
	}
	const int32 NumRows = Rest.Num();
	if (NumRows == 0)
	{
		UE_LOG(LogSoftbodyBulgeTable, Warning, TEXT("BakeBulgeLandmarks: %s has no cells with flesh; the bulge stays off."), *Data->GetName());
		return false;
	}

	// --- 2. 살 그래프: 거친 격자 이웃(둘 다 살 — 옆 넷·대각선 넷·나이트 여덟) + 이음매(살 칸 옆 고스트 칸이 가리키는 점이 든 거친 칸) ---
	// 대각선(판 3, ADR-0033): 옆 넷만이면 대각선 방향 거리가 맨해튼 꼴로 참값보다 최대 41%(평균 27%) 길어, 끌림 커널이 하한을 3D 직선과 견줄 때 평평한
	// 쌍까지 하한으로 가고 가까운 칸의 커널이 그만큼 작았다(미끄러지는 원판 −14%, 일지 177). 배치가 차트 사이에 9칸 이상 빈칸을 두므로 거친 칸 대각선
	// 이웃(√2 × 4칸 이하)에 살이 있으면 같은 차트다.
	// 나이트 이웃(판 4, 일지 201): 여덟 이웃 그래프도 평면에서 방향에 따라 직선보다 최대 8.2%(옆과 대각선의 한가운데 22.5° 방향) 길다. 그래서 런타임이
	// 랜드마크 하한을 직선과 견주는 문턱(SoftbodySurfaceField.usf SB_DRAG_BOUND_TOL)을 1.15로 느슨하게 둬야 했고, 그만큼 접힌 쌍을 늦게 알아봤다.
	// (±1, ±2)·(±2, ±1) 여덟을 더한 열여섯 이웃 그래프는 과대가 최대 2.75%(이웃 방향 사이 각의 한가운데; 평면 격자 모의 — 4이웃 41.4%, 8이웃 8.24%,
	// 16이웃 2.75%)라 문턱을 1.03으로 내릴 수 있다. 다만 나이트 이웃은 두 칸을 담는 상자가 가는 칸 12 × 8(대각선 ≈ 15칸)이라 차트 사이 빈칸 9개를 건너
	// 다른 차트의 칸에 닿을 수 있다 — 대각선 이웃처럼 틈 보장만으로는 같은 차트라고 못 한다. 그래서 두 칸의 정지 위치 사이 3D 거리가 같은 차트에서
	// 있을 수 있는 값(√5 × 거친 칸 한 변, 살이 휘거나 늘어난 몫으로 1.25배)을 넘으면 잇지 않는다. 다른 차트의 칸은 대개 몸의 다른 곳이라 이 검사에
	// 걸린다(배치는 넓이 순이라 아틀라스 이웃은 몸의 이웃이 아니다). 남는 틈은 아틀라스에서 이웃하면서 3D로도 거친 칸 두세 개 안에 드는 다른 차트의 쌍인데,
	// 그 둘은 이음매 건너편이거나 그 정도로 가까운 접힘이라 간선 하나가 거리를 조금 줄일 뿐이다. 간선 무게는 다른 간선과 같이 정지 위치 사이 3D 거리.
	TSet<uint64> EdgeSet;
	TArray<TPair<int32, int32>> Edges;
	auto AddEdge = [&](int32 A, int32 B)
	{
		if (A == B)
		{
			return;
		}
		const uint64 Key = ((uint64)FMath::Min(A, B) << 32) | (uint64)FMath::Max(A, B);
		bool bAlready = false;
		EdgeSet.Add(Key, &bAlready);
		if (!bAlready)
		{
			Edges.Emplace(A, B);
		}
	};
	TArray<float> SideLengths;
	for (int32 Cell = 0; Cell < NumCells; ++Cell)
	{
		if (RowOf[Cell] == INDEX_NONE)
		{
			continue;
		}
		const int32 X = Cell % Coarse;
		const int32 Y = Cell / Coarse;
		if (X + 1 < Coarse && RowOf[Cell + 1] != INDEX_NONE)
		{
			AddEdge(RowOf[Cell], RowOf[Cell + 1]);
			SideLengths.Add(FVector3f::Distance(Rest[RowOf[Cell]], Rest[RowOf[Cell + 1]]));
		}
		if (Y + 1 < Coarse && RowOf[Cell + Coarse] != INDEX_NONE)
		{
			AddEdge(RowOf[Cell], RowOf[Cell + Coarse]);
			SideLengths.Add(FVector3f::Distance(Rest[RowOf[Cell]], Rest[RowOf[Cell + Coarse]]));
		}
		if (X + 1 < Coarse && Y + 1 < Coarse && RowOf[Cell + Coarse + 1] != INDEX_NONE)
		{
			AddEdge(RowOf[Cell], RowOf[Cell + Coarse + 1]);
		}
		if (X > 0 && Y + 1 < Coarse && RowOf[Cell + Coarse - 1] != INDEX_NONE)
		{
			AddEdge(RowOf[Cell], RowOf[Cell + Coarse - 1]);
		}
	}
	// 거친 칸 한 변(메시 로컬) = 캐리어 칸 크기 × 묶음 한 변. 칸 밀도는 모든 차트에서 같다(배치 배율이 하나). 칸 크기를 재기 전에 구운 캐리어
	// (CarrierTexelSize 0)면 옆 이웃 간선 길이의 중앙값으로 어림한다.
	float CoarseSide = (float)Block * Data->CarrierTexelSize;
	const bool bCoarseSideFromTexel = CoarseSide > 0.f;
	if (!bCoarseSideFromTexel && SideLengths.Num() > 0)
	{
		SideLengths.Sort();
		CoarseSide = SideLengths[SideLengths.Num() / 2];
	}
	const float KnightMaxChord = 1.25f * FMath::Sqrt(5.f) * CoarseSide;
	int32 NumKnightEdges = 0;
	int32 NumKnightSkipped = 0;
	{
		// 위쪽(Y 큰 쪽) 넷만 돌면 쌍마다 한 번씩 본다.
		const int32 Offsets[4][2] = {{1, 2}, {-1, 2}, {2, 1}, {-2, 1}};
		for (int32 Cell = 0; Cell < NumCells; ++Cell)
		{
			if (RowOf[Cell] == INDEX_NONE)
			{
				continue;
			}
			const int32 X = Cell % Coarse;
			const int32 Y = Cell / Coarse;
			for (const int32* Offset : Offsets)
			{
				const int32 NX = X + Offset[0];
				const int32 NY = Y + Offset[1];
				if (NX < 0 || NX >= Coarse || NY >= Coarse)
				{
					continue;
				}
				const int32 Other = RowOf[NY * Coarse + NX];
				if (Other == INDEX_NONE)
				{
					continue;
				}
				if (FVector3f::Distance(Rest[RowOf[Cell]], Rest[Other]) > KnightMaxChord)
				{
					++NumKnightSkipped;
					continue;
				}
				const int32 Before = Edges.Num();
				AddEdge(RowOf[Cell], Other);
				NumKnightEdges += Edges.Num() - Before;
			}
		}
	}
	UE_LOG(LogSoftbodyBulgeTable, Display,
		TEXT("Bulge distance table: %s - %d two-by-one diagonal links added, %d skipped by the 3D distance check (bulge cell side %.3f mesh units from %s)"),
		*Data->GetName(), NumKnightEdges, NumKnightSkipped, CoarseSide,
		bCoarseSideFromTexel ? TEXT("the contact map cell size") : TEXT("the median side link length"));
	int32 NumSeamEdges = 0;
	{
		const int32 Offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (int32 Y = 0; Y < Resolution; ++Y)
		{
			for (int32 X = 0; X < Resolution; ++X)
			{
				if (!FleshAt(X, Y))
				{
					continue;
				}
				const int32 Self = RowOf[(Y / Block) * Coarse + X / Block];
				for (const int32* Offset : Offsets)
				{
					const int32 NX = X + Offset[0];
					const int32 NY = Y + Offset[1];
					if (NX < 0 || NY < 0 || NX >= Resolution || NY >= Resolution || KindAt(NX, NY) != LandmarkKindGhost)
					{
						continue;
					}
					// 고스트 칸이 가리키는 점(규약 2는 소수 좌표)이 든 캐리어 칸 → 거친 칸.
					const FLinearColor& R = Redirect[NY * Resolution + NX];
					const int32 TX = FMath::Clamp(FMath::RoundToInt(R.R), 0, Resolution - 1);
					const int32 TY = FMath::Clamp(FMath::RoundToInt(R.G), 0, Resolution - 1);
					const int32 Other = RowOf[(TY / Block) * Coarse + TX / Block];
					if (Other != INDEX_NONE && Other != Self)
					{
						const int32 Before = Edges.Num();
						AddEdge(Self, Other);
						NumSeamEdges += Edges.Num() - Before;
					}
				}
			}
		}
	}
	FFleshGraph Graph;
	Graph.Num = NumRows;
	{
		TArray<int32> Degree;
		Degree.Init(0, NumRows);
		for (const TPair<int32, int32>& Edge : Edges)
		{
			++Degree[Edge.Key];
			++Degree[Edge.Value];
		}
		Graph.Offsets.SetNum(NumRows + 1);
		Graph.Offsets[0] = 0;
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			Graph.Offsets[Row + 1] = Graph.Offsets[Row] + Degree[Row];
		}
		Graph.Neighbors.SetNumUninitialized(Graph.Offsets[NumRows]);
		Graph.Weights.SetNumUninitialized(Graph.Offsets[NumRows]);
		TArray<int32> Fill = Graph.Offsets;
		for (const TPair<int32, int32>& Edge : Edges)
		{
			const float W = FMath::Max(FVector3f::Distance(Rest[Edge.Key], Rest[Edge.Value]), 1e-4f);
			Graph.Neighbors[Fill[Edge.Key]] = Edge.Value;
			Graph.Weights[Fill[Edge.Key]++] = W;
			Graph.Neighbors[Fill[Edge.Value]] = Edge.Key;
			Graph.Weights[Fill[Edge.Value]++] = W;
		}
	}

	// --- 3. 살 조각(연결 성분)과 랜드마크를 둘 칸 ---
	TArray<int32> Component;
	Component.Init(INDEX_NONE, NumRows);
	TArray<int32> ComponentSize;
	{
		TArray<int32> Stack;
		for (int32 Seed = 0; Seed < NumRows; ++Seed)
		{
			if (Component[Seed] != INDEX_NONE)
			{
				continue;
			}
			const int32 Id = ComponentSize.Add(0);
			Component[Seed] = Id;
			Stack.Add(Seed);
			while (Stack.Num() > 0)
			{
				const int32 Node = Stack.Pop(EAllowShrinking::No);
				++ComponentSize[Id];
				for (int32 Edge = Graph.Offsets[Node]; Edge < Graph.Offsets[Node + 1]; ++Edge)
				{
					const int32 Next = Graph.Neighbors[Edge];
					if (Component[Next] == INDEX_NONE)
					{
						Component[Next] = Id;
						Stack.Add(Next);
					}
				}
			}
		}
	}
	TArray<bool> Eligible;
	Eligible.SetNumUninitialized(NumRows);
	int32 NumEligibleComponents = 0;
	for (int32 Row = 0; Row < NumRows; ++Row)
	{
		Eligible[Row] = ComponentSize[Component[Row]] >= MinComponentCells;
	}
	for (int32 Size : ComponentSize)
	{
		NumEligibleComponents += Size >= MinComponentCells ? 1 : 0;
	}
	// 살 조각 번호(판 6, 줄 끝 — 5c): 줄 수 큰 조각부터 0, 1, 2…, 같은 크기면 먼저 나온 조각이 앞. 실행 중에는 같은지만 보지만, 캡처에서 0이 가장 큰
	// 조각(보통 몸)이 되어 읽기 쉽다. 0xFFFF는 "모름"으로 남긴다 — 조각이 그보다 많으면(있을 수 없는 크기) 넘친 조각은 마지막 번호를 같이 쓴다.
	TArray<int32> PieceOrder;
	PieceOrder.SetNumUninitialized(ComponentSize.Num());
	for (int32 Id = 0; Id < ComponentSize.Num(); ++Id)
	{
		PieceOrder[Id] = Id;
	}
	PieceOrder.Sort([&ComponentSize](int32 A, int32 B)
	{
		return ComponentSize[A] != ComponentSize[B] ? ComponentSize[A] > ComponentSize[B] : A < B;
	});
	TArray<uint16> PieceOfComponent;
	PieceOfComponent.SetNumUninitialized(ComponentSize.Num());
	for (int32 Rank = 0; Rank < PieceOrder.Num(); ++Rank)
	{
		PieceOfComponent[PieceOrder[Rank]] = (uint16)FMath::Min(Rank, (int32)Unreachable - 1);
	}

	// --- 4. 랜드마크: 가장 먼 점 뽑기. 아직 어느 랜드마크에도 닿지 않은 칸(다른 살 조각)이 먼저 뽑힌다. ---
	TArray<int32> Landmarks;
	{
		const float Infinite = TNumericLimits<float>::Max();
		TArray<float> MinDist;
		MinDist.SetNumUninitialized(NumRows);
		int32 First = INDEX_NONE;
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			MinDist[Row] = Eligible[Row] ? Infinite : -1.f;
			if (Eligible[Row] && (First == INDEX_NONE || Thickness[Row] > Thickness[First]))
			{
				First = Row;
			}
		}
		if (First == INDEX_NONE)
		{
			First = 0;
		}
		TArray<float> Dist;
		int32 Next = First;
		while (Landmarks.Num() < LandmarkCount)
		{
			Landmarks.Add(Next);
			Dijkstra(Graph, Next, Dist);
			for (int32 Row = 0; Row < NumRows; ++Row)
			{
				if (Eligible[Row] && Dist[Row] < MinDist[Row])
				{
					MinDist[Row] = Dist[Row];
				}
			}
			Next = INDEX_NONE;
			for (int32 Row = 0; Row < NumRows; ++Row)
			{
				if (Eligible[Row] && MinDist[Row] > 0.f && (Next == INDEX_NONE || MinDist[Row] > MinDist[Next]))
				{
					Next = Row;
				}
			}
			if (Next == INDEX_NONE)
			{
				break;
			}
		}
	}

	// --- 5. 거리표: 줄 = 살 칸, 랜드마크마다 uint16(단위 SoftbodyBulgeTable::Unit, 닿지 않으면 0xFFFF), 그 뒤에 가까운 랜드마크 번호,
	// 줄 끝에 붙은 살갗까지 거리(5b) ---
	const int32 L = Landmarks.Num();
	const int32 Width = SoftbodyBulgeTable::Width;
	const int32 RowTexels = SoftbodyBulgeTable::RowTexels(L);
	const int64 NumValues = (int64)NumRows * RowTexels;
	// 표는 Width칸 폭 텍스처 한 장이다. 높이가 GPU 텍스처 한도를 넘으면 올라가지 않으므로 굽지 않고 표를 비운다(캐리어 굽기가 실패로 돌린다).
	const int64 TableHeight = (NumValues + Width - 1) / Width;
	if (TableHeight > (int64)GetMax2DTextureDimension())
	{
		UE_LOG(LogSoftbodyBulgeTable, Error,
			TEXT("BakeBulgeLandmarks: %s has %d cells with flesh, so its bulge distance table would be %lld texels tall, more than the %u the GPU allows. "
				"Lower Contact Map Resolution in the Softbody Authoring Data and bake again."),
			*Data->GetName(), NumRows, TableHeight, GetMax2DTextureDimension());
		Data->Modify();
		Data->CarrierBulgeDistanceTex = nullptr;
		Data->BulgeTableVersion = 0;
		Data->BulgeCellCount = 0;
		bOutTooLarge = true;
		return false;
	}
	const int32 Height = (int32)TableHeight;
	TArray<uint16> Texels;
	Texels.Init(Unreachable, (int64)Width * Height);
	TArray<float> NearestLandmark;
	NearestLandmark.Init(TNumericLimits<float>::Max(), NumRows);
	float MaxFinite = 0.f;
	{
		TArray<float> Dist;
		for (int32 M = 0; M < L; ++M)
		{
			Dijkstra(Graph, Landmarks[M], Dist);
			for (int32 Row = 0; Row < NumRows; ++Row)
			{
				const float D = Dist[Row];
				uint16 Value = Unreachable;
				if (D < TNumericLimits<float>::Max())
				{
					const int32 Units = FMath::RoundToInt(D / SoftbodyBulgeTable::Unit);
					Value = (uint16)FMath::Min(Units, (int32)Unreachable - 1);
					NearestLandmark[Row] = FMath::Min(NearestLandmark[Row], D);
					MaxFinite = FMath::Max(MaxFinite, D);
				}
				Texels[(int64)Row * RowTexels + M] = Value;
			}
		}
	}
	// 줄마다 가까운 랜드마크 NearestLandmarks개의 번호, 거리 작은 순(같은 거리면 번호 작은 순 — 셰이더가 실행 중에 고르던 열쇠 거리 << 8 | 번호와
	// 같은 순서). 원천(SpreadRestrictCS)이 거리 하한을 잴 때 이 랜드마크들만 쓴다. 닿지 않는 랜드마크(0xFFFF)도 셈에 든다 — 다른 살 조각의 칸과는
	// 하한이 커져 더해지지 않는 것이 맞다.
	{
		TArray<uint32> Keys;
		Keys.SetNumUninitialized(L);
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			const int64 Base = (int64)Row * RowTexels;
			for (int32 M = 0; M < L; ++M)
			{
				Keys[M] = ((uint32)Texels[Base + M] << 8) | (uint32)M;
			}
			Keys.Sort();
			for (int32 K = 0; K < SoftbodyBulgeTable::NearestLandmarks; ++K)
			{
				Texels[Base + L + K] = (uint16)(Keys[FMath::Min(K, L - 1)] & 0xffu);
			}
		}
	}

	// --- 5b. 줄 끝: 붙은 살갗까지 거리(판 5, 일지 252) ---
	// 붙은 살갗 = 살갗이 뼈(스태틱 메시는 판)에 붙어 위로 움직이지 못하는 표면 칸. 칠한 살 두께가 캐리어 굽기의 바닥값(굽기가 잴 수 있는 가장
	// 얇은 살, CarrierThicknessFloor) 이하인 칸이다: 칠 0(두께 0), 속이 살갗에 닿아 두께가 바닥값으로 적힌 칸, 칠이 페이드되어 그보다 얇아진 칸.
	// 런타임 솟음이 이 거리로 그 칸이 솟을 수 있는 높이를 줄여, 붙은 살갗 쪽 솟음이 턱 없이 0으로 잦아든다(전에는 칸마다 살 두께에서 잘라 두께
	// 비탈이 그대로 40~70° 턱으로 섰다, 일지 250). 거리는 줄의 살 가운데(Rest)에서 살을 따라 간 길이다. 제 거친 칸이나 옆 여덟 거친 칸(캐리어
	// 격자)에 붙은 살갗 칸이 있는 줄이 씨앗이고(씨앗 거리 = 가장 가까운 그 칸까지 3D 거리), 위 살 그래프에서 여러 씨앗 다익스트라를 한 번 돈다.
	// 이음매 건너편의 붙은 살갗은 이음매 둘레 고스트 칸이 가리키는 칸으로 본다 — 이음매가 칠 0 띠를 따라 나도 띠 옆 줄이 씨앗이 된다.
	float Floor = Data->CarrierThicknessFloor;
	const bool bFloorEstimated = !(Floor > 0.f);
	if (bFloorEstimated)
	{
		// 바닥값을 적기 전에 구운 캐리어(Tools/bake_bulge_landmarks.py가 표만 다시 구울 때): 캐리어 굽기의 자동 규칙(Envelope Cell Size 0 = 바깥
		// 면의 가장 긴 변 / 256)을 칸 정지 위치의 범위로 어림한다. 칸 점은 면 끝에서 칸 하나 안쪽까지만 놓이므로 칸 둘을 더한다 — 모자라면 두께가
		// 바닥값에 딱 붙은 칸(속이 살갗에 닿은 칸)이 빠진다. 가장 얇은 살 칸의 값은 바닥값이 아니다: 칠이 페이드된 칸은 0에 가까운 값도 가진다
		// (시연 몸 6e-8). Envelope Cell Size를 정했거나 속 메시가 대상 메시보다 크면 어긋난다 — Rebake Contact Map이 정확한 값을 적는다.
		FBox3f Bounds(ForceInit);
		for (int32 Texel = 0; Texel < NumTexels; ++Texel)
		{
			const FLinearColor& P = Positions[Texel];
			if (FMath::RoundToInt(P.A) != 0)
			{
				Bounds += FVector3f(P.R, P.G, P.B);
			}
		}
		Floor = (Bounds.GetSize().GetMax() + 2.f * CoarseSide / (float)Block) / EnvelopeCellsPerSide;
	}
	// 노멀 텍스처의 두께는 하프(FP16)로 적혀 있다. 바닥값도 같게 반올림해 견주어야 바닥값에 딱 붙은 칸이 반올림으로 빠지지 않는다.
	const float FloorStored = FFloat16(Floor).GetFloat();
	auto PinnedAt = [&](int32 Texel)
	{
		return FMath::RoundToInt(Positions[Texel].A) == LandmarkKindSurface && Normals[Texel].A.GetFloat() <= FloorStored;
	};
	int32 NumPinnedBare = 0;   // 살이 없는 칸(칠 0)
	int32 NumPinnedThin = 0;   // 살이 바닥값 이하로 얇은 칸(속이 살갗에 닿음, 칠 페이드)
	for (int32 Texel = 0; Texel < NumTexels; ++Texel)
	{
		if (PinnedAt(Texel))
		{
			if (Normals[Texel].A.GetFloat() > 0.f)
			{
				++NumPinnedThin;
			}
			else
			{
				++NumPinnedBare;
			}
		}
	}
	const float Infinity = TNumericLimits<float>::Max();
	TArray<float> PinDistance;
	PinDistance.Init(Infinity, NumRows);
	int32 NumSeedRows = 0;
	for (int32 Row = 0; Row < NumRows; ++Row)
	{
		const int32 CX = CellOfRow[Row] % Coarse;
		const int32 CY = CellOfRow[Row] / Coarse;
		float Nearest = Infinity;
		for (int32 Y = FMath::Max(CY - 1, 0) * Block; Y < FMath::Min(CY + 2, Coarse) * Block; ++Y)
		{
			for (int32 X = FMath::Max(CX - 1, 0) * Block; X < FMath::Min(CX + 2, Coarse) * Block; ++X)
			{
				int32 Texel = Y * Resolution + X;
				if (KindAt(X, Y) == LandmarkKindGhost)
				{
					// 고스트 칸이 가리키는 점이 든 칸(위 2의 이음매 간선과 같은 규칙).
					const FLinearColor& R = Redirect[Texel];
					Texel = FMath::Clamp(FMath::RoundToInt(R.G), 0, Resolution - 1) * Resolution + FMath::Clamp(FMath::RoundToInt(R.R), 0, Resolution - 1);
				}
				if (PinnedAt(Texel))
				{
					const FLinearColor& P = Positions[Texel];
					Nearest = FMath::Min(Nearest, FVector3f::Distance(Rest[Row], FVector3f(P.R, P.G, P.B)));
				}
			}
		}
		PinDistance[Row] = Nearest;
		NumSeedRows += Nearest < Infinity ? 1 : 0;
	}
	DijkstraFrom(Graph, PinDistance);
	// 같은 단위로 줄 끝에 적는다. 붙은 살갗이 없는 살 조각의 줄은 0xFFFF(멀다 — 그 살은 두께로만 솟음을 막는다).
	const int32 RoomTexel = SoftbodyBulgeTable::RoomOffset(L);
	TArray<float> PinReached;
	TArray<bool> PieceReached;
	PieceReached.Init(false, ComponentSize.Num());
	for (int32 Row = 0; Row < NumRows; ++Row)
	{
		uint16 Value = Unreachable;
		if (PinDistance[Row] < Infinity)
		{
			Value = (uint16)FMath::Min(FMath::RoundToInt(PinDistance[Row] / SoftbodyBulgeTable::Unit), (int32)Unreachable - 1);
			PinReached.Add(PinDistance[Row]);
			PieceReached[Component[Row]] = true;
		}
		Texels[(int64)Row * RowTexels + RoomTexel] = Value;
	}
	// --- 5c. 줄 끝: 살 조각 번호(판 6) ---
	const int32 PieceTexel = SoftbodyBulgeTable::PieceOffset(L);
	for (int32 Row = 0; Row < NumRows; ++Row)
	{
		Texels[(int64)Row * RowTexels + PieceTexel] = PieceOfComponent[Component[Row]];
	}
	PinReached.Sort();
	int32 NumPiecesUnreached = 0;
	for (const bool bReached : PieceReached)
	{
		NumPiecesUnreached += bReached ? 0 : 1;
	}
	UE_LOG(LogSoftbodyBulgeTable, Display,
		TEXT("Bulge distance table: %s - skin that cannot rise (surface cells with flesh at or below %.3f mesh units, %s): %d cells with no flesh + %d with thinner flesh; "
			"%d of %d bulge cells start next to it, %d bulge cells in %d flesh pieces reach none (only flesh thickness limits their bulge); distance to that skin 10%% %.2f, median %.2f mesh units"),
		*Data->GetName(), FloorStored,
		bFloorEstimated ? TEXT("floor estimated from the contact map size because it was baked before the floor was recorded; Rebake Contact Map records it")
			: TEXT("the contact map bake's thinnest measurable flesh"),
		NumPinnedBare, NumPinnedThin, NumSeedRows, NumRows, NumRows - PinReached.Num(), NumPiecesUnreached,
		PinReached.Num() > 0 ? PinReached[PinReached.Num() / 10] : 0.f, PinReached.Num() > 0 ? PinReached[PinReached.Num() / 2] : 0.f);

	{
		// 살 조각 크기(줄 수, 번호 차례). 랜드마크가 없는 작은 조각도 제 번호를 가진다(판 6 전에는 실행 중 유도라 모두 한 번호였다).
		FString Sizes;
		int32 NumSmall = 0;
		int32 NumSmallRows = 0;
		for (int32 Rank = 0; Rank < PieceOrder.Num(); ++Rank)
		{
			const int32 Size = ComponentSize[PieceOrder[Rank]];
			NumSmall += Size < MinComponentCells ? 1 : 0;
			NumSmallRows += Size < MinComponentCells ? Size : 0;
			if (Rank < 12)
			{
				Sizes += FString::Printf(TEXT("%s%d"), Rank > 0 ? TEXT(" / ") : TEXT(""), Size);
			}
		}
		UE_LOG(LogSoftbodyBulgeTable, Display,
			TEXT("Bulge distance table: %s - flesh pieces (table layout version %d): %d pieces, bulge cells per piece %s%s; %d small pieces without reference points (%d bulge cells) keep their own piece number"),
			*Data->GetName(), SoftbodyBulgeTable::Version, PieceOrder.Num(), *Sizes, PieceOrder.Num() > 12 ? TEXT(" ...") : TEXT(""), NumSmall, NumSmallRows);
	}

	TArray<float> Spacing;
	for (int32 Row = 0; Row < NumRows; ++Row)
	{
		if (NearestLandmark[Row] < TNumericLimits<float>::Max())
		{
			Spacing.Add(NearestLandmark[Row]);
		}
	}
	Spacing.Sort();

	// --- 6. 저장 ---
	const FString MeshName = Data->SourceSkeletalMesh ? Data->SourceSkeletalMesh->GetName()
		: (Data->SourceMesh ? Data->SourceMesh->GetName() : Data->GetName());
	const FString Folder = FPackageName::GetLongPackagePath(Data->GetPackage()->GetName());
	Data->Modify();
	Data->CarrierBulgeDistanceTex = SoftbodyBakeShared::CreateTextureG16(Folder, FString::Printf(TEXT("T_%s_CarrierBulge"), *MeshName),
		Width, Height, Texels);
	Data->BulgeTableVersion = SoftbodyBulgeTable::Version;
	Data->BulgeLandmarkCount = L;
	Data->BulgeCellBlock = Block;
	Data->BulgeCellCount = NumRows;
	Data->MarkPackageDirty();
	UE_LOG(LogSoftbodyBulgeTable, Display,
		TEXT("Bulge distance table: %s - bulge cells %dx%d contact map cells, %d cells with flesh in %d flesh pieces (%d with reference points), %d seam links, "
			"%d reference points + %d nearest kept per bulge cell (nearest reference point median %.1f, 90%% %.1f mesh units; farthest distance %.0f), texture %dx%d G16 (%.1f MB, %s), table layout version %d, %.1f s"),
		*Data->GetName(), Block, Block, NumRows, ComponentSize.Num(), NumEligibleComponents, NumSeamEdges, L, SoftbodyBulgeTable::NearestLandmarks,
		Spacing.Num() > 0 ? Spacing[Spacing.Num() / 2] : 0.f, Spacing.Num() > 0 ? Spacing[Spacing.Num() * 9 / 10] : 0.f, MaxFinite,
		Width, Height, (double)Width * Height * 2.0 / (1024.0 * 1024.0),
		Data->CarrierBulgeDistanceTex ? GPixelFormats[Data->CarrierBulgeDistanceTex->GetPixelFormat()].Name : TEXT("?"),
		SoftbodyBulgeTable::Version, FPlatformTime::Seconds() - Start);
	if (MaxFinite >= (float)Unreachable * SoftbodyBulgeTable::Unit)
	{
		UE_LOG(LogSoftbodyBulgeTable, Warning,
			TEXT("Bulge distance table: %s is larger than %.0f mesh units across its surface; distances beyond that saturate (the bulge still spreads correctly near the press)."),
			*Data->GetName(), (float)Unreachable * SoftbodyBulgeTable::Unit);
	}
	return true;
}

FString USoftbodyBakeLibrary::DescribeBulgeTable(USoftbodyMeshData* Data)
{
	if (!Data || !Data->HasBulgeTable())
	{
		UE_LOG(LogSoftbodyBulgeTable, Warning, TEXT("DescribeBulgeTable: %s has no bulge distance table."), *GetNameSafe(Data));
		return FString();
	}
	UTexture2D* Tex = Data->CarrierBulgeDistanceTex.LoadSynchronous();
	// 에디터는 텍스처 플랫폼 데이터를 뒤에서 만든다 — 끝날 때까지 기다려야 실제 형식이 보인다(그 전에는 32×32 자리 텍스처).
	Tex->BlockOnAnyAsyncBuild();
	// 원본(G16)에서 첫 줄과 마지막 줄의 값 몇 개: 랜드마크 0~3까지의 거리(메시 로컬), 그 줄의 가장 가까운 랜드마크 거리, 줄 끝의 가까운
	// 랜드마크 번호 목록이 실제로 거리 작은 순인지(판 2 확인). 판 5면 붙은 살갗까지 거리와 모든 줄의 그 분포도. 줄 길이는 표의 판을 따른다.
	FString Sample;
	TArray64<uint8> Raw;
	const bool bRoom = Data->HasBulgeRoom();
	const bool bPiece = Data->BulgeTableVersion >= SoftbodyBulgeTable::PieceVersion;
	if (Tex->Source.IsValid() && Tex->Source.GetFormat() == TSF_G16 && Tex->Source.GetMipData(Raw, 0))
	{
		const uint16* Values = (const uint16*)Raw.GetData();
		const int32 L = Data->BulgeLandmarkCount;
		const int32 RowTexels = SoftbodyBulgeTable::RowTexels(L, Data->BulgeTableVersion);
		const int32 RoomTexel = SoftbodyBulgeTable::RoomOffset(L);
		const int32 PieceTexel = SoftbodyBulgeTable::PieceOffset(L);
		const int64 NumTexels = (int64)Tex->Source.GetSizeX() * Tex->Source.GetSizeY();
		auto RoomText = [](uint16 Value)
		{
			return Value == Unreachable ? FString(TEXT("none")) : FString::Printf(TEXT("%.2f"), Value * SoftbodyBulgeTable::Unit);
		};
		auto Describe = [&](int32 Row)
		{
			const uint16* R = Values + (int64)Row * RowTexels;
			uint16 Nearest = 0xffff;
			for (int32 M = 0; M < L; ++M)
			{
				Nearest = FMath::Min(Nearest, R[M]);
			}
			bool bSorted = true;
			FString List;
			for (int32 K = 0; K < SoftbodyBulgeTable::NearestLandmarks; ++K)
			{
				const int32 M = R[L + K];
				bSorted &= M < L && (K == 0 || R[R[L + K - 1]] <= R[M]);
				if (K < 4)
				{
					List += FString::Printf(TEXT("%s%d(%.2f)"), K > 0 ? TEXT(" ") : TEXT(""), M, M < L ? R[M] * SoftbodyBulgeTable::Unit : -1.f);
				}
			}
			FString Text = FString::Printf(TEXT("row %d: to landmarks 0..3 = %.2f %.2f %.2f %.2f, nearest %.2f (mesh units), nearest list %s... %s"),
				Row, R[0] * SoftbodyBulgeTable::Unit, R[1] * SoftbodyBulgeTable::Unit, R[2] * SoftbodyBulgeTable::Unit, R[3] * SoftbodyBulgeTable::Unit,
				Nearest * SoftbodyBulgeTable::Unit, *List, bSorted ? TEXT("(sorted by distance)") : TEXT("(NOT sorted - bake problem)"));
			if (bRoom)
			{
				Text += FString::Printf(TEXT(", to pinned skin %s"), *RoomText(R[RoomTexel]));
			}
			if (bPiece)
			{
				Text += FString::Printf(TEXT(", flesh piece %d"), (int32)R[PieceTexel]);
			}
			return Text;
		};
		Sample = Describe(0);
		const bool bAllRows = (int64)Data->BulgeCellCount * RowTexels <= NumTexels;
		if (bAllRows && Data->BulgeCellCount > 1)
		{
			Sample += TEXT("; ") + Describe(Data->BulgeCellCount - 1);
		}
		if (bRoom && bAllRows)
		{
			// 모든 줄의 붙은 살갗까지 거리. 닿지 않는 줄(0xFFFF)은 정렬하면 끝에 모인다.
			TArray<uint16> Room;
			Room.SetNumUninitialized(Data->BulgeCellCount);
			for (int32 Row = 0; Row < Data->BulgeCellCount; ++Row)
			{
				Room[Row] = Values[(int64)Row * RowTexels + RoomTexel];
			}
			Room.Sort();
			int32 NumReached = 0;
			while (NumReached < Room.Num() && Room[NumReached] != Unreachable)
			{
				++NumReached;
			}
			const auto At = [&](int32 Percent) { return NumReached > 0 ? Room[NumReached * Percent / 100] : Unreachable; };
			Sample += FString::Printf(TEXT("; to pinned skin over all %d rows (mesh units): 10%% %s, median %s, 90%% %s, %d rows reach none"),
				Room.Num(), *RoomText(At(10)), *RoomText(At(50)), *RoomText(At(90)), Room.Num() - NumReached);
		}
		if (bPiece && bAllRows)
		{
			// 살 조각마다 줄 수(번호 차례) — 굽기 로그 "flesh piece ids per row"와 같아야 한다.
			TMap<int32, int32> Pieces;
			for (int32 Row = 0; Row < Data->BulgeCellCount; ++Row)
			{
				++Pieces.FindOrAdd((int32)Values[(int64)Row * RowTexels + PieceTexel]);
			}
			Pieces.KeySort(TLess<int32>());
			FString List;
			for (const TPair<int32, int32>& Piece : Pieces)
			{
				if (List.Len() < 200)
				{
					List += FString::Printf(TEXT("%s%d:%d"), List.IsEmpty() ? TEXT("") : TEXT(" "), Piece.Key, Piece.Value);
				}
			}
			Sample += FString::Printf(TEXT("; %d flesh pieces (id:rows %s)"), Pieces.Num(), *List);
		}
	}
	const FString Summary = FString::Printf(TEXT("Bulge distance table of %s: texture %s %dx%d, pixel format %s (source %s), layout version %d, %d landmarks + %d nearest%s per row, %d cells, block %d; %s"),
		*Data->GetName(), *Tex->GetName(), Tex->GetSizeX(), Tex->GetSizeY(), GPixelFormats[Tex->GetPixelFormat()].Name,
		Tex->Source.IsValid() ? (Tex->Source.GetFormat() == TSF_G16 ? TEXT("G16") : TEXT("other")) : TEXT("none"),
		Data->BulgeTableVersion, Data->BulgeLandmarkCount, SoftbodyBulgeTable::NearestLandmarks,
		bPiece ? TEXT(" + distance to pinned skin + flesh piece id") : (bRoom ? TEXT(" + distance to pinned skin") : TEXT("")), Data->BulgeCellCount,
		Data->BulgeCellBlock, *Sample);
	UE_LOG(LogSoftbodyBulgeTable, Display, TEXT("%s"), *Summary);
	return Summary;
}
