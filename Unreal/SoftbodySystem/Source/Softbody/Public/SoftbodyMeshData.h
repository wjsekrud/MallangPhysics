// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SoftbodyMeshData.generated.h"

class USkeletalMesh;
class UStaticMesh;
class UTexture2D;

/**
 * 솟음 퍼뜨리기의 랜드마크 거리표(ADR-0031). 캐리어 굽기가 솟음 거친 격자(캐리어 칸 B×B가 한 칸)의 살 있는 칸마다, 살 표면 위에
 * 고르게 뽑은 랜드마크 L개까지의 표면 거리(이음매를 건너고 살 없는 칸은 돌아간다)를 적는다. 실행 중 두 칸의 표면 거리를
 * max_m |d_im − d_jm|(참값의 하한, 과대평가 없음)으로 어림해 눌린 칸들의 가우스 커널을 한 번에 더한다 — 반복 없이. 굽기(에디터)와
 * 셰이더(SB_BULGE_*)가 같은 값을 쓴다.
 */
namespace SoftbodyBulgeTable
{
	/**
	 * 표 규약 판(BulgeTableVersion). 1 = 줄마다 거리만(일지 167). 2 = 줄마다 거리 L개 뒤에 가까운 랜드마크 NearestLandmarks개의 번호가 거리
	 * 작은 순으로 붙는다(일지 169 — 실행 중에 128개를 훑어 고르던 비용을 굽기로). 3 = 거리를 재는 살 그래프에 같은 차트 안 대각선 이웃이 든다
	 * (ADR-0033 — 네 이웃만이면 대각선 방향 거리가 참값보다 최대 41% 길어, 끌림 커널이 하한을 직선과 견줄 때 평평한 쌍까지 하한으로 갔다;
	 * 솟음의 하한도 더 맞는다). 4 = 살 그래프에 나이트 이웃(일지 201 — (±1, ±2)·(±2, ±1) 여덟을 더해 평면 과대가 8.2%에서 2.75%로, 차트 사이 틈을
	 * 건너는 가짜 간선은 정지 위치 3D 거리 검사로 막는다; 끌림의 하한 문턱 SB_DRAG_BOUND_TOL을 1.15에서 1.03으로 내리기 위해). 5 = 줄 끝에
	 * 텍셀 하나를 더 붙여, 그 칸에서 가장 가까운 붙은 살갗까지 살을 따라 간 거리 b를 적는다(일지 252 — 같은 Unit, 0xFFFF = 그 살 조각에 붙은
	 * 살갗이 없다 = 멀다). 붙은 살갗은 살갗이 뼈(스태틱 메시는 판)에 붙어 솟지 못하는 표면 칸이다: 칠 0, 속이 살갗에 닿아 두께가 캐리어 굽기의
	 * 바닥값(USoftbodyMeshData::CarrierThicknessFloor)으로 적힌 칸, 칠이 페이드되어 그보다 얇아진 칸. 솟음이 그쪽에서 턱 없이 0으로 잦아들게
	 * 한다(전에는 칸마다 살 두께에서 잘라 칠 0 띠 둘레에 40~70° 턱이 섰다). 판 4 표는 그대로 읽는다(b가 없다 = 붙은 살갗이 멀다고 본다).
	 * 6 = 줄 끝에 텍셀 하나를 더 붙여 그 줄의 살 조각 번호를 적는다. 살 조각은 위 거리를 재는 살 그래프의 이어진 덩어리이고,
	 * 줄 수 큰 조각부터 0, 1, 2…다. 압력 행렬과 긴 몫 모으기가 "같은 살 조각인가"를 이 번호로 가른다 — 전에는 실행 중에 거리표 줄에서 처음 닿는
	 * 랜드마크 번호로 유도해, 랜드마크가 없는 작은 조각(거친 칸 4개 미만)이 모두 한 번호로 묶였다. 판 4·5 표는 그대로 읽는다(번호는 지금처럼 랜드마크에서).
	 * 그보다 옛 판의 표는 없는 것으로 보고 다시 구우라고 알린다.
	 */
	constexpr int32 Version = 6;
	/** 아직 읽는 가장 옛 판. 판 4는 줄 끝의 b만 없고 앞쪽 규약은 같다. */
	constexpr int32 MinVersion = 4;
	/** 줄 끝에 붙은 살갗까지 거리 b가 붙기 시작한 판. */
	constexpr int32 RoomVersion = 5;
	/** 줄 끝에 살 조각 번호가 붙기 시작한 판. */
	constexpr int32 PieceVersion = 6;
	/** 거리표 텍스처 한 줄의 텍셀 수(셰이더 SB_LANDMARK_TABLE_WIDTH). 줄 r의 텍셀 t는 텍셀 (r × RowTexels(L, 판) + t) % Width, / Width에 있다. */
	constexpr int32 Width = 2048;
	/** 거리 한 단위(cm). G16 텍스처의 uint16 값 × 이 값 = cm, 0xFFFF = 닿지 않음(셰이더 SB_LANDMARK_UNIT). */
	constexpr float Unit = 0.01f;
	/** 기본 랜드마크 수. 시험 캐릭터 캐리어 모의(일지 166)에서 128이면 표면 거리와 거의 같은 결과였다. 셰이더 공유 메모리 크기라 상한 128. */
	constexpr int32 DefaultLandmarks = 128;
	constexpr int32 MaxLandmarks = 128;
	constexpr int32 MinLandmarks = 16;
	/** 줄마다 거리 뒤에 적는 가까운 랜드마크 번호 수(셰이더 SB_LANDMARK_NEAREST). 원천이 거리 하한을 잴 때 이 랜드마크들만 쓴다(일지 167). */
	constexpr int32 NearestLandmarks = 16;

	/**
	 * 랜드마크 L개인 판 TableVersion 표의 줄 하나 텍셀 수: 거리 L개 + 가까운 랜드마크 번호 NearestLandmarks개 + (판 5부터) 붙은 살갗까지
	 * 거리 b 하나 + (판 6부터) 살 조각 번호 하나(셰이더 LandmarkRowTexels와 같은 규칙). 구워 둔 표를 읽을 때는 그 표의 판(BulgeTableVersion)을 넘긴다.
	 */
	constexpr int32 RowTexels(int32 Landmarks, int32 TableVersion)
	{
		return Landmarks + NearestLandmarks + (TableVersion >= RoomVersion ? 1 : 0) + (TableVersion >= PieceVersion ? 1 : 0);
	}
	/** 지금 판(Version)으로 굽는 표의 줄 하나 텍셀 수. */
	constexpr int32 RowTexels(int32 Landmarks)
	{
		return RowTexels(Landmarks, Version);
	}
	/** 줄 안에서 붙은 살갗까지 거리 b의 자리(판 5부터, 셰이더 NumLandmarks + SB_LANDMARK_NEAREST). */
	constexpr int32 RoomOffset(int32 Landmarks)
	{
		return Landmarks + NearestLandmarks;
	}
	/** 줄 안에서 살 조각 번호의 자리(판 6부터, b 다음 — 셰이더 SB_PressurePiece가 NumLandmarks + SB_LANDMARK_NEAREST + 1에서 읽는다). */
	constexpr int32 PieceOffset(int32 Landmarks)
	{
		return Landmarks + NearestLandmarks + 1;
	}
	/** 거친 칸이 이 크기(cm)를 넘지 않는 가장 큰 묶음을 쓴다(셰이더·런타임 솟음 거친 격자와 같은 규칙). */
	constexpr float CoarseTexelLimit = 1.f;
	/**
	 * 묶음 한 변의 최대 캐리어 칸 수. 배치가 차트 사이에 9칸 이상 빈칸을 두므로 4×4 묶음까지는 두 차트가 섞이지 않고 옆 묶음도 같은
	 * 차트다. 8×8이면 다른 차트의 묶음끼리 이웃할 수 있다. 셰이더 SB_COARSE_MAX_BLOCK(내리기가 펼쳐 읽는 살 칸 수의 한 변)과 같이 둔다.
	 */
	constexpr int32 MaxCoarseBlock = 4;

	/** 칸 크기 TexelSize(cm)인 캐리어(한 변 Resolution칸)의 솟음 거친 묶음 한 변 칸 수. 굽기와 런타임이 같은 값을 얻는다. */
	inline int32 BlockFor(float TexelSize, int32 Resolution)
	{
		int32 Block = 1;
		while (Block < MaxCoarseBlock && TexelSize * Block * 2 <= CoarseTexelLimit && Resolution % (Block * 2) == 0)
		{
			Block *= 2;
		}
		return Block;
	}
}

/**
 * 덩어리 국소 모양(일지 251, ③A). 덩어리가 구운 무름 세 모양(출렁임 모양)으로만 비켜나면, 그 모양에서 조금만 움직이는 밑동을 눌렀을 때
 * 그 자리를 비키려고 덩어리 전체가 크게 움직여 끝이 지렛대처럼 흔들렸다. 측정 굽기가 덩어리마다 원판 누름 MaxModes개의 FEM 응답을 풀고
 * (FSoftbodyLumpDiscBake), 캐리어 굽기가 지금 덩어리 모양이 그 누름에서 이미 그리는 몫을 빼 고유 모양으로 나눈다(정적 보정 모양 — 잔여
 * 유연도). 실행 중 덩어리 평형이 모양마다 크기 q를 좌표로 같이 푼다: 누른 자리 둘레만 꺼지고 끝은 거의 안 움직인다. 굽기와 셰이더
 * (SB_REGION_*)가 같은 값을 쓴다. 누르는 것(손·공·다른 물체)과 상관없이 받는 덩어리만의 데이터다. 출렁임 모양이 모든 원판 누름 응답의
 * 80 % 이상을 이미 그리는 덩어리는 모양을 만들지 않고, 그런 자산은 실행 중 미지수 셋 풀이를 쓴다(일지 254).
 */
namespace SoftbodyLumpRegion
{
	/** 줄 규약 판(USoftbodyMeshData::LumpRegionVersion). 다른 판이면 없는 것으로 본다 — 덩어리는 출렁임 모양만으로 비켜난다. */
	constexpr int32 Version = 1;
	/** 덩어리마다 원판 누름 수 = 고유 모양 수의 상한(셰이더 SB_REGION_MODES). */
	constexpr int32 MaxModes = 16;
	/** 거친 칸 하나의 줄 텍셀 수: 성분(x, y, z)마다 모양 넷씩 넷 = 12텍셀, 마지막 텍셀 R = 덩어리 묶음 번호(셰이더 SB_REGION_RECORD_TEXELS). */
	constexpr int32 RecordTexels = 13;
	/** 줄 텍스처 한 행의 줄 수(폭 = RecordTexels × RecordsPerRow, 셰이더 SB_REGION_RECORDS_PER_ROW). */
	constexpr int32 RecordsPerRow = 80;
}

/**
 * 판(ADR-0037 §3.7, 일지 285 · 휘는 판 ADR-0038 §4, 일지 298): 슬롯 칠 Flex(A, 코드 이름 Softness)를 흰색보다 어둡게 칠한 이어진 자리 하나가
 * 판 하나다(장식 금속·보석, 주사위 눈, 덧댄 가죽).
 * 판은 밑 살 위에서 휜다. 칠이 어두울수록 굽힘 길이 ℓ_b = 0.6·L0·√((1 − S)/S)(L0 = SoftbodyContactResponse::ReferenceLength)가 길어
 * 넓게 짐을 나르고, 검정은 거의 한 몸으로 가라앉고 기운다. L0 두께의 살에서 손 옆 판 띠가 판째 가라앉는 몫이 약 1 − S가 되게 맞춘 곡선이다.
 * 굽기는 판만의 값을 정한다(조각 소속·발자국·틀·판 모드 ψ와 굳기 β) — 살이 버티는 정도는 실행 중 압력 풀이가 지금 쌍 식으로 짓는다(컴포넌트
 * 크기·저작 에셋 재료값이 실행 중 바뀐다, P04). 굽기와 셰이더(SB_HARD_*)가 같은 값을 쓴다.
 */
namespace SoftbodyHardSurface
{
	/**
	 * 조각 표 규약 판(USoftbodyMeshData::HardPieceVersion). 다른 판이면 조각이 없는 것으로 본다 — 칠한 조각도 살처럼 눌린다.
	 * 2 = 휘는 판(일지 298): 판 모드·국소 판·마디 표와 칸마다 Softness. 1(단단한 판만)은 다시 구워야 판이 된다.
	 * 3 = 굽힘 길이 곡선 0.6·L0·√((1 − S)/S)와 고정 길이 L0, 조각마다 대표 칠(FSoftbodyHardPiece::Softness — Flex Offset이 읽는다).
	 * 4 = 조각마다 가장 큰 칠(FSoftbodyHardPiece::MaxSoftness — 실행 중 강체 판 분류), 떠 있는 장식은 셔츠 위 한 칸 넓힘 없음.
	 * 5 = 떠 있는 장식은 제 칸에 판을 그린다: 조각 번호 텍스처의 비트 15(DrawPlateBit)가 판을 그리는 칸 — 장식 칸이다. 장식 밑 셔츠(발자국)는
	 *     살로 그려 장식이 들려도 따라 오르지 않는다. 4로 구운 지도는 장식 정점이 셔츠 칸을 읽어, 그대로 쓰면 장식이 셔츠처럼 휘므로 다시 굽는다.
	 */
	constexpr int32 Version = 5;
	/** Flex 문턱: 세 코너가 모두 이 값보다 작은 삼각형이 판이다(흰색 맨살만 빼고 회색은 모두 휘는 판, ℓ_b(0.98) ≈ 0.09 L0). */
	constexpr float PieceThreshold = 0.98f;
	/** 옛 규칙(판 1, 일지 298 전): 세 코너가 모두 이 값 이하면 단단한 판. 굽기는 더 쓰지 않는다 — 실행 쪽이 판 2로 옮겨 가면 지운다. */
	constexpr float MaskThreshold = 0.5f;
	/** 한 자산의 조각 상한(조각 번호 텍스처 G16에 번호 + 1을 적는다). */
	constexpr int32 MaxPieces = 4096;
	/**
	 * 조각 번호 텍스처(G16)의 비트 15: 판을 그리는 칸. 떠 있는 장식은 장식 칸(장식 삼각형 위의 칸)이고, 장식 칸이 없는 판(자동 섬 지도, 칸보다 가는
	 * 장식)은 지금처럼 발자국 칸이다. 합치기는 이 칸에만 판 자세를 그린다. 아래 15비트(PieceMask)가 조각 번호다. 셰이더 SB_HARD_DRAW_PLATE와 짝.
	 */
	constexpr uint16 DrawPlateBit = 0x8000;
	constexpr uint16 PieceMask = 0x7FFF;
	static_assert(MaxPieces + 1 <= PieceMask, "plate numbers must stay below the draw-plate bit");
}

// 판 하나(SoftbodyHardSurface). 메시 로컬 단위, 기본 자세.
/** One area painted below white in Flex. Black ones move as rigid plates and gray ones bend with the skin. Filled by the contact map bake. */
USTRUCT(BlueprintType)
struct FSoftbodyHardPiece
{
	GENERATED_BODY()

	// 발자국(조각이 얹힌 겉면 칸)의 넓이 중심. 조각의 1차식 φ = (1, (x − Center)·AxisU, (x − Center)·AxisV)가 이 점에서 잰다.
	/** Center of the surface the piece rests on (mesh local, reference pose). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FVector3f Center = FVector3f::ZeroVector;

	// 판 평면의 두 축(단위, 서로 수직). 판 노멀 = AxisU × AxisV(바깥쪽).
	/** First in-plane axis of the piece. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FVector3f AxisU = FVector3f::ZeroVector;

	/** Second in-plane axis of the piece. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FVector3f AxisV = FVector3f::ZeroVector;

	// 발자국 칸이 Center에서 떨어진 가장 먼 거리 — 실행 중 후보 조각을 고르는 상자.
	/** Distance from the center to the farthest cell under the piece (mesh local). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	float Radius = 0.f;

	// 1 = 가라앉기만, 2 = 가라앉음 + AxisU 기울기, 3 = 가라앉음 + 기울기 둘. 선 모양 조각(사슬·띠)은 짧은 축 기울기를 뺀다.
	/** How the piece can move: 1 = sinks only, 2 = sinks and tilts about one axis, 3 = sinks and tilts freely. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 Freedom = 3;

	// 다른 겉면 위에 따로 놓인 조각(보석·금속 장식). 발자국은 그 밑 겉면이고 손은 조각 윗면(접촉 시작점 올림)에서 잰다.
	/** True for a piece sitting on another surface (a gem on clothing); false for a piece that is part of the surface (a die pip). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	bool bFloating = false;

	// 개발: 조각의 대표 Flex 칠 = 판 칸 칠의 중앙값(굽기). 판 모드 굳기 β는 칸마다 칠로 구워져 있고, 실행 중 Flex Offset은 이 값을
	// 옮긴 칠과 이 값의 굽힘 길이 비의 세제곱을 그 조각 모드 전부에 곱한다(고르게 칠한 판은 정확, 한 판 안에 회색이 섞이면 어림).
	/** The plate's Flex paint (the median over its cells). Flex Offset on the Softbody component shifts it while playing. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody", meta = (DisplayName = "Flex"))
	float Softness = 0.f;

	// 개발: 조각 자기 칠의 가장 큰 값(굽기 PieceOwnFlex — 판 테두리가 아닌 정점 코너, 그런 정점이 없으면 모든 코너의 중앙값).
	// 테두리 코너는 칠 텍스처를 쌍선형으로 읽어 바깥 흰 칠과 섞이므로 뺀다. 이 값(+ Flex Offset)의 굽힘 길이가 조각 반지름의 RigidPieceRatio배 이상이면
	// (검정은 늘) 실행 중 압력 풀이가 판을 강체로 푼다(SoftbodyContactResponse::RigidFlexLimit). 판 칸 칠은 굽기에서 이 값을 넘지 않게 자른다.
	// 떠 있는 장식은 이 값과 상관없이 늘 강체다.
	/** The piece's largest Flex paint, read away from its rim. The piece moves as one rigid plate when this paint is stiff enough for the piece's size (always at black). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody", meta = (DisplayName = "Max Flex"))
	float MaxSoftness = 0.f;

	// HardPieceCells[FirstCell .. FirstCell + NumCells): 발자국 거친 칸(솟음 거친 격자 BulgeCellBlock, x | y << 16).
	/** Start of this plate's cells in the baked plate tables (count in Num Cells). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 FirstCell = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 NumCells = 0;

	// HardPieceTiles[FirstTile .. FirstTile + NumTiles): 발자국이 걸친 함몰 묶음(16 × 16 칸, x | y << 16).
	/** Start of this plate's contact map tiles in the baked plate tables (count in Num Tiles). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 FirstTile = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 NumTiles = 0;

	// 판 모드(ADR-0038 §4, 판 2): HardModes[FirstMode .. FirstMode + NumModes) = 모드마다 (β, k). 모드 수 = 발자국 칸 수 − 강체 셋 차수
	// (보통 n_F − 3, n_F < 3이면 0). 모양은 HardModeShapes[FirstShape + f·NumModes + i](f = 조각 안 발자국 번호, FirstCell 차례).
	/** Start of this plate's bending shapes in the baked plate tables (count in Num Modes). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 FirstMode = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 NumModes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 FirstShape = 0;

	// 판 마디(발자국 + 둘레 두 거친 칸): HardNodes[FirstNode .. FirstNode + NumNodes).
	/** Start of this plate's points in the baked plate tables (count in Num Nodes). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 FirstNode = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 NumNodes = 0;

	// 마디 상자(거친 칸): HardNodeSlots[FirstNodeSlot + (y − NodeBoxMin.Y)·NodeBoxSize.X + (x − NodeBoxMin.X)] = 조각 안 마디 번호, 없으면 −1.
	/** Corner of the box around this plate's points in the baked plate tables (size in Node Box Size). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FIntPoint NodeBoxMin = FIntPoint::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FIntPoint NodeBoxSize = FIntPoint::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 FirstNodeSlot = 0;
};

// 캐리어 차트(표면 장을 펼친 아틀라스 조각)를 나누는 방법 (ADR-0007).
/** How the contact map is cut into flat pieces. Dents are slightly less accurate along the seams between pieces. */
UENUM(BlueprintType)
enum class ESoftbodyCarrierCharts : uint8
{
	// 메시에 캐리어 UV 채널을 하나 더해 이음매를 걸친 삼각형이 없다. UV0이 알맞지 않으면(UV 넓이 0, 접힘) 자동으로 굽는다.
	// 아일랜드 안은 이음매를 그대로 둔 채 칸 크기가 고르게 다시 편다(ADR-0007 §3.7). 고르지 않은 아일랜드는 가장 늘거나 준 곳에서
	// 경계까지 캐리어 UV 채널에만 이음매를 더 넣는다(원뿔처럼 솟은 아일랜드, §3.8) — 렌더 정점이 그 길을 따라 나뉘고 이음매 선은 퐁 곡면으로 이어진다.
	/**
	 * Cuts the contact map along the mesh's UV0 islands, so seams fall on the UV0 seams. Each island is re-flattened with even cell sizes.
	 * Falls back to Automatic when UV0 cannot be used (zero area or folded UVs).
	 */
	MeshUV0 UMETA(DisplayName = "Mesh UV0 Islands"),
	// 봉투를 곡률 기준으로 나눈다. 이음매를 걸친 삼각형은 퐁 곡면으로 채운다.
	/** Cuts the surface automatically by curvature (XAtlas). You cannot choose where the seams go. */
	Automatic UMETA(DisplayName = "Automatic (XAtlas)"),
	/**
	 * Uses the UV0 islands exactly as laid out (only scaled per island), for comparison with Mesh UV0 Islands. Where UV0 is stretched
	 * or squeezed, cell size and dent rim width change with it.
	 */
	MeshUV0AsIs UMETA(DisplayName = "Mesh UV0 Islands (As Is)"),
};

// 덩어리 하나(ADR-0012): 뼈대(강체)에서 뺀 본에 매달린 살. 캐리어 굽기가 칸마다 잰 값의 합이다.
/**
 * One lump: flesh hanging from a Lump bone (not part of the rigid core), such as a breast.
 * When pressed, it moves aside as one piece along with the dent.
 */
USTRUCT(BlueprintType)
struct FSoftbodyInertiaLump
{
	GENERATED_BODY()

	/** Inertia group of this lump (the group of its Lump bone). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 Group = 0;

	// = 캐리어 칸 넓이 × 살 두께 × 덩어리 무게의 합.
	/** Flesh volume of the lump (mesh local units cubed). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	float Volume = 0.f;

	// 부피 평균 무름(칠 적용, 가장 무른 점 = 1, 기본 자세 축). 살 기둥 안쪽은 뼈대 쪽으로 0까지 줄어드는 모양으로 보고 겉면 값의 절반을 쓴다.
	/**
	 * Average direction and amount the lump's flesh moves when the flesh is pulled along +X (softest point = 1, reference pose
	 * axes). Y and Z below are the same for +Y and +Z.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FVector3f MeanShapeX = FVector3f::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FVector3f MeanShapeY = FVector3f::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	FVector3f MeanShapeZ = FVector3f::ZeroVector;

	// 국소 모양(SoftbodyLumpRegion)마다 단단함 Λ̂. 실행 중 덩어리 평형의 에너지 ½ Σ Λ̂_m q_m²로, 평균 무름 C와 같은 단위다(칸 이동 =
	// 흔들림 크기 A × 모양 × q). 모양 수 = 원소 수. 비면(옛 굽기) 덩어리는 출렁임 모양만으로 비켜난다.
	/** Stiffness of each local shape the lump can take around a press. Empty on lumps that need none. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TArray<float> RegionStiffness;
};

// 덩어리 국소 모양의 재료(SoftbodyLumpRegion): 측정 굽기가 덩어리마다 원판 누름을 FEM으로 푼 응답. 캐리어 굽기가 칠·덩어리 양(V·C)을
// 넣어 마무리한다 — 칠을 바꿔 Reapply Paint만 해도 FEM을 다시 풀지 않는다. 에디터 전용(USoftbodyMeshData::LumpDiscBakes).
/** Raw local press responses of one lump, solved by the measure bake. Only used while baking. */
USTRUCT()
struct FSoftbodyLumpDiscBake
{
	GENERATED_BODY()

	/** 덩어리 묶음(관성 그룹 번호, 스태틱은 0). */
	UPROPERTY()
	int32 Group = INDEX_NONE;

	/** 원판 반지름(메시 로컬). */
	UPROPERTY()
	float Radius = 0.f;

	/** 원판 가운데(메시 로컬) — 로그와 검사용. */
	UPROPERTY()
	TArray<FVector3f> Centers;

	/** 응답을 적은 정점(굽기 정점 순서 — VertexPositions·VertexInertiaResponseShapes와 같다): 이 덩어리 무게가 0보다 큰 정점. */
	UPROPERTY()
	TArray<int32> Vertices;

	/** [원판 j × Vertices.Num() + i]: 원판 j를 힘 합 1로 고르게 눌렀을 때 정점 Vertices[i]가 밀린 양(메시 로컬, 살 단단함 1 — 무름 굽기의 원시 단위). */
	UPROPERTY()
	TArray<FVector3f> Responses;

	/** 원판 j의 힘은 LoadOffsets[j] ~ LoadOffsets[j + 1] 구간의 (LoadVertices, LoadForces): 정점마다 안쪽 노멀 쪽 힘, 원판마다 크기 합 1. */
	UPROPERTY()
	TArray<int32> LoadOffsets;

	UPROPERTY()
	TArray<int32> LoadVertices;

	UPROPERTY()
	TArray<FVector3f> LoadForces;
};

// 겹친 층 연결 하나(ADR-0014): 이 정점(텍셀 번호)은 제 값 대신 Sources 정점들의 원래 값을 Weights로 섞은 값을 쓴다.
/**
 * One layered clothing link: a cloth vertex lying over skin (such as underwear) takes the blended values of the skin vertices below
 * it, so both layers dent and jiggle together.
 */
USTRUCT()
struct FSoftbodyLayerLink
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Vertex = INDEX_NONE;

	UPROPERTY()
	TArray<int32> Sources;

	UPROPERTY()
	TArray<float> Weights;
};

// 프리베이크 산출물. 정점당 속성 텍스처(고유 텍셀 UV로 인덱싱), 관성용 살의 무름 3방향(ADR-0005), 표면 장 캐리어(ADR-0003)를 담는다.
/**
 * Baked Softbody data for one mesh, filled by Run Bake on a Softbody Authoring Data asset: per-vertex values, how soft the flesh is
 * (for jiggle) and the contact map. Put it in the Softbody component's Mesh Data.
 */
UCLASS(BlueprintType)
class SOFTBODY_API USoftbodyMeshData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Median triangle edge length of the LOD0 mesh (mesh local units). The bake check tools use it as a distance reference. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Diagnostics")
	float MedianEdgeLength = 0.f;

	/** The static mesh this data was baked from (None for a skeletal mesh bake). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Softbody")
	TObjectPtr<UStaticMesh> SourceMesh;

	/** The skeletal mesh this data was baked from (None for a static mesh bake). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Softbody")
	TObjectPtr<USkeletalMesh> SourceSkeletalMesh;

#if WITH_EDITORONLY_DATA
	// 속 메시(ADR-0028): 표면 캐리어의 봉투(미는 방향·살 두께·겹친 층 판정), 살의 무름을 재는 살 채움, 모양 거리장이 이 메시의 속을
	// 쓰고, 칸 위치·UV·스킨·칠은 소스 메시에서 그대로 나온다. 에디터 전용 — 쿡에 들어가지 않는다.
	/**
	 * The closed, filled mesh whose inside the bake treats as flesh. Empty = the source mesh itself.
	 * For an open or hollow source mesh. Positions, UVs, skinning and paint still come from the source mesh.
	 */
	UPROPERTY(EditAnywhere, Category = "Softbody")
	TObjectPtr<UStaticMesh> VolumeMesh;
#endif

	// RGBA16F 검수 채널 (DebugVis 0·1·4가 읽는다): R=예약(0), G=관성 무름(세 방향 평균, 가장 무른 점 ≈ 1, 음수 = 반대로 밀림),
	// B=임베딩 상태(0 실패, 0.5 외삽, 1 포함), A=물리 코어 연결(0/1).
	/**
	 * Per-vertex check values shown by the Debug View modes: how soft the flesh is, whether the vertex lies inside the measured
	 * flesh, and whether it connects to the rigid core.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TObjectPtr<UTexture2D> AttributeTexDebug;

	// RGBA16F (스켈레탈 측정 베이크 전용): (첫째 그룹, 셋째의 몫, 둘째 그룹 + 16 × 셋째 그룹, 첫째의 몫), 둘째의 몫 = 나머지.
	// 그룹을 둘만 적던 전 베이크(둘째 채널 0, 셋째 채널 = 둘째 그룹)도 같은 식으로 읽힌다. 살의 무름은 메시당 하나만 굽는다.
	/**
	 * Per-vertex inertia groups (skeletal meshes only): the up to three bone groups whose motion makes each vertex jiggle, and how
	 * much each one counts. Summed from the full skin weights.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TObjectPtr<UTexture2D> AttributeTex2;

	// 스킨 웨이트 본을 상위 병합. 런타임이 이 본들의 가속·회전 가속·중력 방향 변화를 그룹별 진동자에 공급한다.
	/**
	 * The bone that drives each inertia group (up to 16). Each group jiggles with its bone's acceleration, rotation and change in
	 * gravity direction.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TArray<FName> InertiaGroupBones;

	// ADR-0012: 뼈대에서 뺀 본(RigidShrinkPerBone 0 이하, 부모 쪽에 뺀 본이 없는 것)이 이끄는 묶음.
	/**
	 * Whether each inertia group is a lump: flesh led by a Lump bone (breasts, buttocks) that moves aside as one piece when pressed.
	 * Groups inside the rigid core, such as the torso, do not move aside.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TArray<bool> InertiaGroupLump;

	// 뒤에 도는 캐리어 베이크도 같은 뼈대로 살 두께를 재도록 남긴다.
	/**
	 * Per-bone rigid core settings used by the bake (bone name -> Bone Thickness).
	 * -1 = that bone and the bones below it are not rigid core, -2 = only that bone.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TMap<FName, float> RigidShrinkPerBone;

	// 개발(일지 239): 뼈로 치는 본 = 이 슬롯의 정점을 움직이는 본(SoftbodyBakeShared::CollectCoreSpheres). 측정 굽기가 적고 접촉 지도 굽기가 같은 뼈대로 살 두께를 잰다.
	/**
	 * Material slots the rigid core belongs to (slots with a Softbody material). Bones that move only other slots are not part of the core.
	 * Empty = every slot.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TArray<FName> RigidCoreSlots;

	/** Default Bone Thickness used by the last Run Bake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	float RigidCoreShrink = 0.f;

	// 표면 장(v5) 봉투를 만들 슬롯. 캐리어 베이크는 이 슬롯 삼각형만으로 봉투와 아틀라스를 만든다.
	/**
	 * Material slots that can be pressed. Only these get space in the contact map. Empty = the Softbody slots, or the whole mesh if none.
	 * Rebake Contact Map after changing it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact Map")
	TArray<FName> CarrierEnvelopeSlots;

	/**
	 * How the contact map is cut into pieces. Dents are slightly less accurate along the seams (the red bands in Debug View Mode 5).
	 * Mesh UV0 Islands reuses the UV0 seams the artist already hid. Rebake the contact map after changing it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact Map")
	ESoftbodyCarrierCharts CarrierCharts = ESoftbodyCarrierCharts::MeshUV0;

	/** Farthest distance from each group's bone (reference pose) to a vertex it moves (mesh local). Limits rotational jiggle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TArray<float> InertiaGroupReach;

	// 페인트 층(RGBA16F 텍셀, ADR-0038): R=안 씀(옛 겉껍질 장력 칠 — 일지 298에 없어짐, 1), G=Depth(살 두께 배율 — 캐리어 굽기가 살 두께에 곱한다,
	// ADR-0016 §5; 머티리얼은 0인 정점만 표면 장에서 뗀다), B=관성량(칠 아님 — 1에서 기준 맞춤·Physics Driven Bones·겹친 층), A=Volume(0 폼 … 1 살,
	// ADR-0024 — bPaintVolume일 때만 캐리어 굽기가 읽는다). 베이크가 초기값(전부 1)을 채우고, 슬롯 칠이 있으면 칠 단계가 대체한다.
	/**
	 * Per-vertex paint made by the bake from the slot paint: depth, jiggle amount after the bake's own scaling, and volume. When empty,
	 * everything counts as fully painted (white).
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TObjectPtr<UTexture2D> PaintTex;

	// 페인트 A가 부피 보존(ADR-0024)인가. 꺼져 있으면 캐리어 굽기가 A를 읽지 않고 전부 1(살)로 둔다.
	/** On when a slot mask paints Volume Preservation. When off, all flesh fully preserves its volume. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	bool bPaintVolume = false;

	// 개발(ADR-0037 §3.3): 저작 에셋의 Volume Preservation 값. 실행 중 v = 이 값 × 칸 칠(CarrierVolumeTex) + 컴포넌트 Volume Offset(0~1로
	// 자름) — 저작 에셋에서 바꾸면 굽지 않고 바로 여기로 와 함몰·솟음이 바뀐다(PIE 중에도).
	/**
	 * Volume Preservation of the authoring asset (0 foam ... 1 flesh), for the whole mesh. The painted Volume multiplies it and the
	 * component's Volume Offset adds to the result. Changing it on the authoring asset updates this at once.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	float VolumePreservation = 1.f;

	// 개발: 마지막 측정 굽기가 쓴 값(덩어리 모양의 푸아송 비, SoftbodyContactResponse::PoissonOfVolume). 위 값과 다르면 저작 에셋 Validate가 Run Bake를 권한다.
	/** Volume Preservation that the last Run Bake measured the flesh with. How a whole lump spreads follows this value until the next Run Bake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	float MeasuredVolumePreservation = 1.f;

	// 페인트 B를 그 비율만큼 끈다. 페인트를 다시 만들 때(GeneratePaintTexture*) 적용된다.
	/**
	 * Bones moved by simulated physics bodies. Flesh skinned to these bones and their children gets proportionally less inertia jiggle.
	 * Copied from Physics Driven Bones on the authoring asset when the paint is reapplied.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	TArray<FName> PhysicsDrivenBones;

	// 관성용 살의 무름 규약 (ADR-0005). 2 = 체적력 +X/+Y/+Z 3방향, 세 방향 공통 최대 진폭 1로 정규화(가장 무른 점 = 1).
	// 1(구 6축 가속 응답장)은 런타임이 받지 않는다.
	/** Format version of the baked flesh softness. It must be 2; older bakes do not jiggle until you press Run Bake again. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	int32 InertiaResponseVersion = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	int32 NumInertiaResponses = 0;

	// TexSize × (TexSize·줄 수) RGBA16F, 줄 수 = GetInertiaResponseRows(). 줄 k(0=X, 1=Y, 2=Z)의 RGB는 살을 +k로 당겼을 때 정점이
	// 밀리는 방향·양(메시 로컬, 가장 무른 점 = 1). 그 뒤의 줄은 InertiaResponseRows 참고.
	/**
	 * Baked flesh softness per vertex: which way and how far each vertex moves when the flesh is pulled along X, Y and Z (softest
	 * point = 1). This is what makes the flesh jiggle.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	TObjectPtr<UTexture2D> InertiaResponseTex;

	// InertiaResponseTex의 줄 수(줄 하나 = TextureSize² 정점 표). 3 = 무름 세 방향뿐, InertiaRowsWithShading(7) = 음영 기울기 줄이 붙는다:
	//   3줄: +X/+Y/+Z로 당겼을 때 노멀 방향으로 밀리는 양 × 칠(RGB), 팔면체로 접은 노멀 x(A)
	//   4줄, 5줄: 접선 1, 접선 2 방향 1 cm당 3줄 RGB 변화(4줄 A = 접은 노멀 y)
	//   6줄: 본 묶음 자리 0, 1 몫의 1 cm당 변화(접선 1 R·G, 접선 2 B·A)
	// 머티리얼이 정점마다 노멀을 돌린다(일지 133, 셰이더 SoftbodyInertiaTilt). 접선 둘은 굽기와 셰이더가 같은 규칙으로 노멀에서 만든다.
	// 페인트를 다시 만들 때마다(GeneratePaintTexture*) 같이 다시 만든다. 0 = 이 값을 적기 전의 굽기(3줄, BakeInertiaShading으로 올린다).
	/**
	 * Rows in the baked flesh softness texture. 7 = the shading tilts with the jiggle; 3 or 0 = the shading does not follow the jiggle.
	 * Press Reapply Paint or Run Bake to update.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	int32 InertiaResponseRows = 0;

	static constexpr int32 InertiaRowsWithShading = 7;

	/** InertiaResponseTex의 실제 줄 수(머티리얼 파라미터 InertiaResponseCount). */
	int32 GetInertiaResponseRows() const { return FMath::Max(InertiaResponseRows, NumInertiaResponses); }

	/** Raw largest movement per direction before scaling (for diagnosis). All three directions are scaled by the largest of these. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	TArray<float> InertiaResponseScales;

	// 개발: RescaleInertiaPaint가 Paint.B에 곱한 배율 = 원시 최댓값 ÷ 기준 살의 원시 처짐. 덩어리 살 단단함(ADR-0012 §3.3)의
	// 기준 무름 = 위 최댓값 ÷ 이 값 = 기준 살의 원시 처짐이라 출렁임과 같은 살이다. 아래 묶음 처짐 몫이 비어 있으면 옛 굽기(칠한 살 가운데
	// 가장 무른 점 = 1, 일지 239)의 배율이다.
	/**
	 * How much the jiggle paint was scaled so that the reference flesh (thick flesh painted white in Jiggle) sags Inertia Amount at 1 g.
	 * Lumps use the same reference.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	float InertiaPaintScale = 0.f;

	// 개발: 묶음마다 살이 1 g에서 처지는 몫(기준 살 = 1) — 흔들리는 섹션 정점의 (무름 × Paint.B)를 묶음 몫으로 가중한 90 % 값.
	// 실행 중 그 묶음의 빠르기 = √(g ÷ (Inertia Amount × 이 값)) ÷ 2π × Jiggle Speed. 비어 있으면 옛 굽기라 모든 묶음이 1이다(전과 같은 동작).
	/** How far each inertia group's flesh sags under 1 g compared with the reference flesh (most of its vertices). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	TArray<float> InertiaGroupSag;

	// 개발: 묶음마다 정점 하나의 (묶음 몫 × 처짐) 가운데 가장 큰 값. 흔들림 상한(Max Inertia Offset)과 잠들기 상자가 쓴다.
	/** The most any vertex of each inertia group sags under 1 g compared with the reference flesh. Max Inertia Offset uses it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	TArray<float> InertiaGroupSagMax;

	// 베이크가 사용한 앵커 노드의 질량 가중 로컬 중심.
	/** Center of the rigid core used by the bake (mesh local). A static mesh swings around this point. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	FVector3f InertiaAnchorLocal = FVector3f::ZeroVector;

	/** Distance from that center to the farthest surface vertex (mesh local). Limits rotational jiggle on a static mesh. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	float InertiaAnchorReach = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	int32 InertiaAnchorNodeCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	float InertiaMaxAnchorError = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Inertia")
	float InertiaMaxRelativeResidual = 0.f;

#if WITH_EDITORONLY_DATA
	// 오프라인 검증용 응답장: 응답장-메이저 [k·NumVerts + v]. 굽기 검사 도구만 읽는다 — 쿡에 들어가지 않는다(일지 222).
	/** Baked flesh softness per vertex, kept for offline checks. */
	UPROPERTY()
	TArray<FVector3f> VertexInertiaResponseShapes;
#endif

	/** UV channel the bake added for per-vertex data. The Softbody material reads the values through it (the TexelUV input). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 TexelUVChannel = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	int32 TextureSize = 0;

	// ADR-0003 표면 캐리어(몸의 바깥 봉투 표면을 한 장으로 펼친 것) 규약 버전. 0 = 캐리어 없음, 2 = 고스트 칸이 옆 차트의 정확한 점을
	// 갖는다(ADR-0007 §3.2). 플러그인은 판 2부터만 받는다 — 판 2 미만(옛 판 1: 고스트 칸이 가까운 칸 하나를 통째로 따름)은 눌리지 않고
	// 로그에 경고 한 번이 남으니 다시 구워야 한다. BakeSurfaceCarrier가 채운다.
	/**
	 * Contact map format version. 0 = no contact map, and 1 = baked with an older plugin. Either way the mesh cannot be pressed until
	 * you press Rebake Contact Map.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 CarrierVersion = 0;

	/** Contact map size in texels per side. The textures used while playing have the same size. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 CarrierResolution = 0;

	// 함몰을 퍼뜨리는 격자 간격이다.
	/** Average surface length covered by one contact map cell (mesh local units). Smaller cells give finer dents. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	float CarrierTexelSize = 0.f;

	// 솟음(ADR-0011)의 퍼뜨리기 패스 수를 정한다. 0이면 이 값을 재기 전에 구운 캐리어다(에디터는 노멀 텍스처에서 잰다).
	/**
	 * Flesh thickness of the thicker contact map cells (90th percentile of the cells with flesh, mesh local units).
	 * The contact update sizes its widest dent and drag from it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	float CarrierThicknessHigh = 0.f;

	// 살 있는 칸 두께의 중앙값(기록·캡처용). 일지 279~298에는 겉껍질 장력 길이와 판 굽힘 길이의 눈금이었으나, 칠한 두께라 한 부위의 Depth 칠이 다른
	// 부위의 겉 재료를 바꿔(원칙 P03) 고정 길이 L0(SoftbodyContactResponse::ReferenceLength)를 쓴다. 0이면 이 값을 재기 전에 구웠다.
	/** Median flesh thickness of the contact map cells with flesh (mesh local units). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	float CarrierThicknessMedian = 0.f;

	// 캐리어 굽기가 살 두께를 잴 수 있는 가장 얇은 값(봉투를 뽑은 칸 크기, ThicknessAt의 max(…, 칸)). 속이 살갗에 닿은 칸은 두께가 이 값으로
	// 적힌다. 솟음 거리표(판 5, 일지 252)가 칠한 두께가 이 값 이하인 표면 칸을 붙은 살갗(뼈에 붙어 솟지 못하는 살갗)으로 보고 거리를 잰다.
	// 0이면 이 값을 적기 전에 구운 캐리어다(거리표 굽기가 캐리어 크기에서 어림한다).
	/**
	 * Thinnest flesh the contact map bake can measure (mesh local units). Cells where the rigid core touches the skin get this thickness.
	 * Cells at or below it count as skin held by the bone, so bulges fade out before reaching them.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	float CarrierThicknessFloor = 0.f;

	// 텍셀 테이블(TextureSize², 렌더 정점의 고유 텍셀 UV로 주소) RGBA32F: RG = 캐리어 UV, B = 봉투까지 부호 거리(바깥 +, 로컬 단위),
	// A = 2 + 사이 값 무게(ADR-0006 §3.3, 옛 베이크 1). 위치당 하나라 같은 자리의 층·이음매 복제 정점은 같은 캐리어 점을 가리킨다 — 코너 WPO가 읽는다.
	/** Per-vertex lookup into the contact map: where each vertex reads its dent, and its distance to the outer surface. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TObjectPtr<UTexture2D> CarrierUVTex;

	// ADR-0007 §3.3. 캐리어 베이크가 쓰고 호스트 머티리얼의 CarrierUV 입력에 잇는다. 봉투 슬롯이 아닌 삼각형의 값은 (−1, −1)(텍셀 표를 읽는다).
	/** Mesh UV channel holding the contact map UVs per vertex (-1 = none). The Softbody material's CarrierUV input reads this channel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 CarrierUVChannel = -1;

	// 접촉 지도 텍스처(아래 열한 장, 일지 222)는 약한 참조다: Mesh Data를 불러도 올라오지 않고, 접촉이 켜진 컴포넌트가 켜질 때 표면 장이 비동기로
	// 올려 쥔다(USoftbodySurfaceField::Initialize). 끈 컴포넌트·접촉을 끈 컴포넌트만 남으면 GC가 내린다. 텍셀 표(CarrierUVTex)는 머티리얼이 읽어
	// 그대로 강한 참조다.
	// CarrierResolution² RGBA32F: xyz = 캐리어 점의 정지 위치(메시 로컬), w = 칸 종류(0 빈칸, 1 표면, 2 고스트).
	/** Rest position of each contact map cell on the surface. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierPositionTex;

	// CarrierResolution² RGBA16F: xyz = 미는 방향(바깥 노멀을 살 두께 정도 범위로 평균 — 배꼽처럼 작은 모양이 눌려 벌어지지 않게),
	// w = 표면에서 코어까지 두께(로컬 단위).
	/** Per cell: the direction the flesh is pushed in, and the flesh thickness down to the rigid core. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierNormalTex;

	// CarrierResolution² RGBA32F: xy = 값을 가져올 칸 좌표(정수 = 칸 한가운데; 고스트 칸은 3D로 이어지는 옆 차트의 점, 규약 2부터 소수 좌표),
	// z = 칸 자리 렌더 삼각형 변 길이(메시 로컬), w = 칸 자리 화면 노멀(ADR-0007 §3.6: 팔면체 두 값을 12비트씩 묶은 정수 + 1, 0 = 없음)
	// — 노멀 패스가 콜라이더 면에 붙은 칸에서 누르기 전 노멀로 쓴다.
	/**
	 * Per cell: links that let dents cross the seams between contact map pieces, plus the local triangle size and surface normal.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierRedirectTex;

	// CarrierResolution² RGBA16F: 레퍼런스 스켈레톤 본 인덱스 4개.
	/** Skeletal meshes only: the four bones that move each contact map cell. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierSkinIndexTex;

	/** Skeletal meshes only: the weights of those four bones (they add up to 1). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierSkinWeightTex;

	// 덩어리 값(ADR-0012), CarrierResolution² RGBA16F. RGB = 바깥 노멀 쪽으로 밀리는 정도(칠 × 노멀·무름, +X/+Y/+Z, 기본 자세 축),
	// A = 덩어리 묶음 번호 + 0.99 × 덩어리 무게(스킨 웨이트 중 덩어리 몫, 0 = 덩어리 아님).
	// 스태틱 메시는 메시 전체가 묶음 0(무게 1)이지만 덩어리 힘이 없다(InertiaLumps 비움, 일지 296) — 출렁여 옮겨 간 자리에서 접촉을 재고 음영을
	// 맞추는 데만 쓴다.
	/**
	 * Per cell: which lump the cell belongs to and how much it moves with the lump. Without it, lumps do not move aside when pressed.
	 * Rebake Contact Map after changing the paint.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierLumpTex;

	// 덩어리 옆 몫(ADR-0012 §3.5) RGBA16F 두 장: 칠 × 무름 세 방향을 미는 방향에 수직인 두 축(셰이더 SB_LumpBasis)으로 잰 값.
	// A = (축1·무름_x, _y, _z, 축2·무름_x), B = (축2·무름_y, 축2·무름_z, 0, 0). CarrierLumpTex RGB(노멀 몫)와 합치면 무름 전체.
	// 접촉 패스가 그만큼 광선 출발점을 옮긴다. 없으면(옛 베이크) 노멀 몫만 옮긴다.
	/**
	 * Per cell: how much the cell moves sideways with its lump. Keeps a dent under the collider instead of letting it slide along
	 * with the lump.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierLumpSideATex;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierLumpSideBTex;

	// 부피 보존 칠(ADR-0024), CarrierResolution² G8. 페인트 A(슬롯 마스크의 Volume 소스)를 칸마다 가까운 렌더 정점에서 가져온 값.
	// 없으면(칠하지 않은 메시, 옛 베이크) 전부 1.
	/**
	 * Volume paint per contact map cell (0 to 1). 1 (flesh, gel): the pushed-in volume bulges up around a tight dent. 0 (foam): no bulge.
	 * Applied with Reapply Paint.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierVolumeTex;

	// 일지 298(ADR-0038 §3.5): 겉껍질 장력 칠(CarrierSkinTex)은 없어졌다 — 겉껍질 장력은 컴포넌트 값 하나다. 옛 굽기에 남은 텍스처는 읽지 않는다.

	// 부위 나누기(저작 에셋 Separated Pairs), CarrierResolution² G16: 칸마다 두 묶음 몫 — 낮은 8비트 s_a(묶음 1 본 웨이트 합 × 255), 높은 8비트
	// s_b(묶음 2). 칸 스킨 웨이트(CarrierSkinIndex·Weight, 상위 4본)에서 굽기가 만든다(BakeSeparatedParts). 셰이더 SB_SeparationAt가 같은 배치로 읽는다.
	// 없으면 부위 나누기가 꺼져 있다.
	/**
	 * Per cell: how much of it belongs to each side of the Separated Pairs (bone weights). Built from the authoring asset's Separated
	 * Pairs when the contact map is baked. Empty = no Separated Pairs.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierSeparationTex;

	// 하드 서피스(SoftbodyHardSurface), CarrierResolution² G16: 칸마다 조각 번호 + 1(0 = 조각 아님). 발자국 칸(조각이 얹힌 겉면 칸, 한 칸
	// 넓힘)과 조각 자체의 칸·고스트가 같은 번호를 갖는다. 비트 15(DrawPlateBit)는 판을 그리는 칸 — 떠 있는 장식 칸(판 5). 조각이 없는 자산은
	// 비어 있다 — 그 자산의 표면 장은 조각 코드가 없는 셰이더 순열을 쓴다.
	/**
	 * Per cell: which plate (Flex paint) the cell belongs to. Empty when no plate is painted.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierPieceTex;

	// 칸마다 Flex 칠(ADR-0038 §3.5), CarrierResolution² G16: S × 65535. 남은 판(CarrierPieceTex > 0)의 칸은 판 규칙
	// (렌더 삼각형 보간을 판 안에서만 다듬음, 발자국을 넓힌 한 칸 고리는 안쪽 이웃 판 칸의 가장 작은 값), 나머지 칸은 정점 Flex를 Depth와 같은 길로
	// 옮긴 값이다(남은 판 테두리 정점은 바깥 칸에 흰색으로 든다). 고스트는 가리키는 칸 값. 실행 중 칸 굽힘 길이 ℓ_b를 여기서 낸다(SB_BendLengthAt).
	// 흰색보다 어두운 칸도 판도 없으면 비어 있다. CarrierSoftnessCoverage 0인 옛 굽기는 판 칸만 칠 값이다.
	/** Per cell: its Flex paint (how stiff the skin bends there). Empty when nothing is painted below white. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierSoftnessTex;

	/**
	 * 1 = Carrier Softness Tex holds the Flex paint of every cell.
	 * 0 = it holds it only under plates, so gray paint elsewhere is bare skin until Reapply Paint.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	uint8 CarrierSoftnessCoverage = 0;

	// 칸 Flex의 가장 작은 값(떠 있는 장식 밑 칸은 빼고) — 플레이 시작에 Flex Offset이 칠을 검정 끝까지 내리는지 알린다.
	/** Darkest Flex paint over the cells (gems lying on the surface left out), for the play-start notes on Flex Offset. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	float CarrierFlexMin = 1.f;

	// 하드 서피스 접촉 시작점 올림, CarrierResolution² G16: 떠 있는 조각(보석)의 발자국 칸에서 노멀 쪽으로 조각 윗면까지 거리 ÷ CarrierLiftScale
	// × 65535. 접촉 패스가 그만큼 광선 출발점을 올려 손이 조각 윗면에 닿는 깊이를 잰다(칸 자리는 밑 겉면 그대로). 떠 있는 조각이 없으면 비어 있다.
	/** Per cell under a floating rigid piece: how far the piece's top is above the cell. Empty when no piece floats. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierLiftTex;

	/** Height (mesh local) that the largest value of Carrier Lift Tex stands for. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	float CarrierLiftScale = 0.f;

	/** Layout version of the plate table. With 0 or an older version, painted plates press like flesh until you press Run Bake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 HardPieceVersion = 0;

	/** Plates painted dark in Flex. Filled by the contact map bake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TArray<FSoftbodyHardPiece> HardPieces;

	// 조각마다 발자국 거친 칸(x | y << 16, 솟음 거친 격자)과 함몰 묶음(x | y << 16, 16 × 16 칸) 목록 — FSoftbodyHardPiece의 First·Num 구간.
	UPROPERTY()
	TArray<uint32> HardPieceCells;

	UPROPERTY()
	TArray<uint32> HardPieceTiles;

	// 휘는 판(ADR-0038 §4, 판 2) — 모두 메시 로컬 단위, 판 좌표 = ((x − Center)·AxisU, (x − Center)·AxisV). 식은 Tools/contact_response/
	// replica_plate.py PlateBake(ghost='plane').
	// 발자국 칸마다(HardPieceCells와 같은 차례, 전역 번호 = FirstCell + f): (x_F, y_F, A_F, 0) — 그 거친 칸 판 가는 칸 정지 자리의 판 좌표 평균과
	// 넓이(가는 칸 수 × CarrierTexelSize²). 압력 풀이의 발자국 줄이 이 자리에서 판을 잰다.
	UPROPERTY()
	TArray<FVector4f> HardCellPlate;

	// 발자국 칸마다 국소 판 기울기 항 구간 (첫 자리, 수) → HardGradEntries. 항 = (asfloat(이웃 f — 조각 안 번호), Gx 무게, Gy 무게, 0), 제 칸 몫도
	// 한 항이다. 그 칸 판 기울기(로컬 길이당) = Σ 항 (Gx, Gy) × w_g. 3 × 3 이웃 발자국 칸의 넓이 무게 최소 제곱(모자라면 5 × 5, 7 × 7).
	UPROPERTY()
	TArray<FUintVector2> HardCellGradRange;

	UPROPERTY()
	TArray<FVector4f> HardGradEntries;

	// 판 마디(조각마다 FirstNode부터 NumNodes개): (X, Y, asfloat(HardNodeSrc 첫 자리), asfloat(원천 수)). X·Y = 마디(거친 칸 가운데)의 판 좌표.
	// 마디 값 = 원천 발자국 칸 국소 판들(w_f + (X − x_f)gx_f + (Y − y_f)gy_f)의 평균. 가는 칸 판 깊이는 둘레 4 × 4 마디에서 Catmull-Rom으로 읽는다
	// (거친 연속 좌표 q = (가는 칸 + 0.5)/BulgeCellBlock − 0.5, 마디는 거친 칸 정수 자리 — 판 가는 칸마다 4 × 4가 다 있게 굽는다).
	UPROPERTY()
	TArray<FVector4f> HardNodes;

	// 마디 원천 = 조각 안 발자국 번호 f: 마디가 발자국 칸이면 그 칸 하나, 아니면 3 × 3(없으면 5 × 5) 안 발자국 칸들.
	UPROPERTY()
	TArray<uint32> HardNodeSrc;

	// 조각마다 마디 상자(FSoftbodyHardPiece::NodeBoxMin·NodeBoxSize) 칸마다 조각 안 마디 번호, 없으면 −1.
	UPROPERTY()
	TArray<int32> HardNodeSlots;

	// 판 모드 모양 ψ(조각마다 n_F × NumModes, F 먼저), 반정밀 비트(FFloat16과 같은 바이트 배치). 모드마다 Σ_F ψ² = 1, 강체 셋(1, x_F, y_F)과 직교.
	UPROPERTY()
	TArray<uint16> HardModeShapes;

	// 판 모드마다 (β, k). β = 그 모양의 판 굽힘 굳기 ψᵀB_cψ(B_c = 발자국 칸 판 굽힘, D/μ = ℓ_b³ 포함, 로컬 단위 — 실행이 길이 눈금 하나를
	// 곱한다), 오름차순. k = 모양의 파수(ψᵀLψ의 제곱근, L = 발자국 네 이웃 그래프 라플라시안 ÷ 거친 칸 한 변²) — 모드 줄 앞처리 어림.
	UPROPERTY()
	TArray<FVector2f> HardModes;

	/** 하드 서피스 조각이 구워져 있는가 — 있으면 표면 장이 조각 순열(SB_HARD_SURFACE)을 쓴다. */
	bool HasHardPieces() const
	{
		return HardPieceVersion == SoftbodyHardSurface::Version && HardPieces.Num() > 0 && !CarrierPieceTex.IsNull();
	}

#if WITH_EDITORONLY_DATA
	// 하드 서피스 칠 → 접촉 지도 굽기 다리(SoftbodyHardSurface). 칠 단계(Reapply Paint·Run Bake)가 슬롯 마스크에서 조각을 찾아 적고 접촉 지도
	// 굽기가 읽는다 — 접촉 지도 굽기는 슬롯 마스크를 모른다. 메시 설명 삼각형 자리(FTriangleID 값)마다 조각 번호(0 = 단단하지 않음, 1부터),
	// 조각마다 떠 있는가(단단하지 않은 삼각형과 용접한 정점을 나누지 않는 조각 — 다른 겉면 위에 놓인 장식). 쿡에서 빠진다.
	UPROPERTY()
	TArray<int32> HardTrianglePieces;

	// 삼각형 코너마다 Flex 칠(메시 설명 삼각형 자리 × 3 + 코너). HardTriangleSoftnessLayout 1 = 모든 삼각형(연속 굽힘 — Flex를 읽지 않는 슬롯은 1),
	// 0 = 옛 칠 단계(판 삼각형만, 나머지 1). 접촉 지도 굽기가 칸마다 Flex(CarrierSoftnessTex)와 판 굽힘을 여기서 낸다. 쿡에서 빠진다.
	UPROPERTY()
	TArray<float> HardTriangleSoftness;

	UPROPERTY()
	uint8 HardTriangleSoftnessLayout = 0;

	UPROPERTY()
	TArray<uint8> HardPiecesFloating;

	// 측정 굽기가 뺀 떠 있는 조각의 칠 지문(칠 단계와 다르면 Run Bake를 알린다 — Reapply Paint는 측정을 다시 하지 않는다).
	UPROPERTY()
	uint32 HardMaskMeasuredHash = 0;

	// 측정 굽기가 읽은 칠 전부(Depth·Jiggle·Volume·떠 있는 판)의 지문(SoftbodyBakeShared::MeasurePaintHashOf, ADR-0038 §3.6). 저작 에셋의 지금
	// 칠과 다르면 Reapply Paint가 Run Bake로 돈다 — 캐리어만 다시 구우면 출렁임의 뼈와 접촉의 두께가 어긋난다(원칙 P04).
	UPROPERTY()
	uint32 MeasuredPaintHash = 0;

	UPROPERTY()
	uint32 HardMaskPaintHash = 0;

	// 저작 에셋 미리보기 Hard Surface Pieces 보기 전용 기록: 칠 단계 조각(HardTrianglePieces 번호 − 1 자리)마다 접촉 지도 굽기의 결과 —
	// 1부터 = 남은 판 번호(HardPieces 자리 + 1), 음수 = 살로 남은 까닭(SoftbodyBakeShared::HardDropped*). HardPieceResultsHash = 그 굽기가 읽은
	// 칠 지문(HardMaskPaintHash와 다르면 그 뒤에 칠이 바뀌었다). 실행 중에는 읽지 않는다.
	UPROPERTY()
	TArray<int32> HardPieceResults;

	UPROPERTY()
	uint32 HardPieceResultsHash = 0;
#endif

#if WITH_EDITORONLY_DATA
	// 덩어리 받침 막(ADR-0034, SoftbodyLumpSupport) — 저작 에셋 미리보기의 Lump Base 보기 전용 기록이다. 실행 중에는 읽지 않고 쿡에서 빠진다.
	// 접촉 지도 굽기가 덩어리 칸의 살 두께를 잴 때 쓴 막을 그대로 남긴다: 메시 공간 정점, 삼각형(정점 번호 셋씩), 정점마다 그 위 둔덕 높이
	// (살갗까지 거리, 영역 밖 0), 덩어리마다 한 줄. bLumpBaseRecorded = 이 기록을 남기는 판의 굽기인가(옛 굽기와 가른다).
	UPROPERTY()
	TArray<FVector3f> LumpBaseVertices;
	UPROPERTY()
	TArray<int32> LumpBaseTriangles;
	UPROPERTY()
	TArray<float> LumpBaseHeights;
	UPROPERTY()
	TArray<FString> LumpBaseNotes;
	UPROPERTY()
	bool bLumpBaseRecorded = false;
#endif

	// 제 살 깊이(ADR-0003 §16), CarrierResolution² G8: 칸 밑의 살 가운데 다른 쪽을 향한 면보다 이 칸에 더 가까운 깊이를, 묻힌 콜라이더를
	// 찾는 깊이(살 두께 × 1.5)에 대한 비율로 적은 값. 캐리어 굽기가 메시 모양에서 구한다. 없으면(옛 베이크) 전부 1.
	/**
	 * How deep each cell's own flesh goes (0 to 1). A collider buried in the flesh only presses cells within this depth.
	 * Computed from the mesh shape; there is no setting for it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierColumnTex;

	// 솟음 랜드마크 거리표(ADR-0031, SoftbodyBulgeTable): G16, 한 줄 Width 텍셀. 살 있는 거친 칸(줄 번호 = 거친 격자 줄 우선 순서로
	// 센 번호)마다 랜드마크 BulgeLandmarkCount개까지의 표면 거리(uint16, 0.01 cm, 0xFFFF = 닿지 않음)와 그 뒤에 가까운 랜드마크
	// NearestLandmarks개의 번호(거리 작은 순, 판 2), 줄 끝에 붙은 살갗까지 거리 b(판 5, 같은 단위). 캐리어 굽기(BakeBulgeLandmarks)가 만든다.
	// 없거나 판 4보다 옛 판이면(옛 베이크) 솟음과 끌림(ADR-0033)이 꺼진다. 판 4는 돌지만 b가 없다.
	/**
	 * Surface distances that spread a dent's pushed-aside flesh and dragged skin along the surface around seams and bone bands.
	 * Missing = the bulge and the drag stay off until Rebake Contact Map.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierBulgeDistanceTex;

	/**
	 * Layout version of the bulge distance table. Below 4 = no usable table, so the bulge and the drag stay off until Rebake Contact Map.
	 * 4 and 5 still work, but Rebake Contact Map to get the current version, 6.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 BulgeTableVersion = 0;

	/** Number of surface landmarks the bulge distances are measured to (0 = older bake without the table). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 BulgeLandmarkCount = 0;

	/** Contact map cells per side of one bulge cell (the bulge spreads on this coarser grid). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 BulgeCellBlock = 0;

	/** Number of bulge cells with flesh (rows of the distance table). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 BulgeCellCount = 0;

	/** 솟음·끌림이 쓸 수 있는 거리표가 있는가(판 4~6). 줄 길이는 SoftbodyBulgeTable::RowTexels(BulgeLandmarkCount, BulgeTableVersion). */
	bool HasBulgeTable() const
	{
		return !CarrierBulgeDistanceTex.IsNull() && BulgeTableVersion >= SoftbodyBulgeTable::MinVersion
			&& BulgeTableVersion <= SoftbodyBulgeTable::Version && BulgeLandmarkCount > 0 && BulgeCellBlock > 0 && BulgeCellCount > 0;
	}

	/** 거리표 줄 끝에 붙은 살갗까지 거리 b가 있는가(판 5, 일지 252). 없으면(판 4) 붙은 살갗이 멀다고 본다. */
	bool HasBulgeRoom() const
	{
		return HasBulgeTable() && BulgeTableVersion >= SoftbodyBulgeTable::RoomVersion;
	}

	/** Volume and average softness of each lump (bones set to Lump on a skeletal mesh). Filled by the contact map bake. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TArray<FSoftbodyInertiaLump> InertiaLumps;

	// 덩어리 국소 모양(SoftbodyLumpRegion)의 줄 번호, G16, 솟음 거친 격자(BulgeCellBlock) 한 변 칸 수². 값 × 65535 = 줄 번호 + 1(0 = 없음).
	/**
	 * Per bulge cell of a lump: which entry of the lump shape table it uses. With it, a lump pressed near its base gives way locally
	 * around the press instead of swinging as a whole. Built by the contact map bake from the measure bake's presses.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierLumpRegionIndexTex;

	// 줄 텍스처 RGBA16F, 폭 RecordTexels × RecordsPerRow. 줄 r은 행 r / RecordsPerRow, 열 (r % RecordsPerRow) × RecordTexels부터. 텍셀 k(0~11) =
	// 성분 k / 4(0 x, 1 y, 2 z — 메시 로컬 기본 자세 축)의 모양 4(k % 4) ~ 4(k % 4) + 3(RGBA). 텍셀 12의 R = 덩어리 묶음 번호. 모양은 덩어리마다
	// 가장 큰 크기가 1인 단위(덩어리의 RegionStiffness와 짝).
	/** The lump shape table: per bulge cell, how far each local shape moves that spot (see Carrier Lump Region Index Tex). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	TSoftObjectPtr<UTexture2D> CarrierLumpRegionTex;

	/** Layout version of the lump shape table (0 = none; lumps give way only as a whole until Run Bake). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 LumpRegionVersion = 0;

	/** Number of entries in the lump shape table. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Contact Map")
	int32 LumpRegionRecords = 0;

	bool HasLumpRegions() const
	{
		return LumpRegionVersion == SoftbodyLumpRegion::Version && LumpRegionRecords > 0 && !CarrierLumpRegionIndexTex.IsNull()
			&& !CarrierLumpRegionTex.IsNull() && HasBulgeTable();
	}

	// 일지 296 전의 스태틱 굽기는 메시 전체를 덩어리 하나로 구웠다. 스태틱 메시는 누르면 통째로 비켜나지 않으므로
	// 읽을 때 그 덩어리 값을 버린다 — 다시 굽지 않아도 덩어리 힘이 꺼진다. 칸 값(CarrierLumpTex)은 출렁임을 따라 접촉을 재고 음영을 맞추는 데
	// 그대로 쓴다.
	virtual void PostLoad() override
	{
		Super::PostLoad();
		if (SourceMesh && !SourceSkeletalMesh)
		{
			InertiaLumps.Reset();
#if WITH_EDITORONLY_DATA
			LumpDiscBakes.Reset();
#endif
		}
	}

	// 잠들기(ADR-0020): 칸의 정지 위치와, 접촉이 콜라이더를 찾으러 안쪽으로 쏘는 광선의 끝(살 두께 × 1.5)을 다 담는다. 번호 = 레퍼런스
	// 스켈레톤 본 번호(칸을 움직이는 본 넷에 모두 넣는다), 스태틱 메시는 [0] 하나. 비면 옛 베이크 — 늘 올린다.
	// 캐리어 굽기(BakeSurfaceCarrier → BakeWakeBounds)가 채운다.
	/**
	 * Sleeping: per bone, the box (reference pose, mesh local) where that bone's flesh can be pressed. When no collider or hand can
	 * reach any box, contact sleeps and costs no GPU time.
	 */
	UPROPERTY()
	TArray<FBox3f> CarrierWakeBounds;

	// 실행 중 거리장의 부위와 다르면(거리장을 캐리어 뒤에 다시 구움) 자기 부위는 거르지 않고 늘 올린다 — 캐리어(또는 저작 에셋의 모양
	// 거리장 단계)를 다시 구우면 맞춰진다.
	/**
	 * Sleeping: bone names of the self contact parts (the shape field parts that press the mesh itself), in shape field order. If they
	 * no longer match the shape field, self contact never sleeps until the contact map or the shape field is baked again.
	 */
	UPROPERTY()
	TArray<FName> CarrierWakeSelfParts;

	// [부위 × 본 수 + 본]. 기본 포즈에서 부위와 살 두께보다 멀리 떨어진 칸만이다(접촉 패스가 누르는 규칙과 같다, ADR-0004 §9).
	/**
	 * Sleeping: for each self contact part and bone, the box of cells that part can press. Flesh the part always touches (the forearm
	 * next to the wrist) is left out, so a resting hand does not keep the whole body awake.
	 */
	UPROPERTY()
	TArray<FBox3f> CarrierWakeSelfBounds;

	/** 접촉 지도가 구워져 있는가(텍스처가 올라와 있는지는 보지 않는다 — 접촉 지도 텍스처는 약한 참조, 일지 222). */
	bool HasSurfaceCarrier() const
	{
		return CarrierVersion >= 2 && CarrierResolution > 0 && CarrierUVTex && !CarrierPositionTex.IsNull()
			&& !CarrierNormalTex.IsNull() && !CarrierRedirectTex.IsNull()
			&& (!SourceSkeletalMesh || (!CarrierSkinIndexTex.IsNull() && !CarrierSkinWeightTex.IsNull()));
	}

	/** 구워진 접촉 지도 텍스처(약한 참조)의 경로들 — 표면 장이 이것을 비동기로 올린다(일지 222). */
	void GetContactMapPaths(TArray<FSoftObjectPath>& OutPaths) const
	{
		OutPaths.Reset();
		for (const TSoftObjectPtr<UTexture2D>* Texture : {&CarrierPositionTex, &CarrierNormalTex, &CarrierRedirectTex, &CarrierSkinIndexTex,
			&CarrierSkinWeightTex, &CarrierLumpTex, &CarrierLumpSideATex, &CarrierLumpSideBTex, &CarrierVolumeTex, &CarrierSeparationTex,
			&CarrierColumnTex, &CarrierBulgeDistanceTex, &CarrierLumpRegionIndexTex, &CarrierLumpRegionTex, &CarrierPieceTex, &CarrierLiftTex,
			&CarrierSoftnessTex})
		{
			if (!Texture->IsNull())
			{
				OutPaths.Add(Texture->ToSoftObjectPath());
			}
		}
	}

	/**
	 * Loads the contact map textures now (they load on their own when a Softbody component that uses them is switched on). For editor
	 * scripts that read them.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody")
	void LoadContactMap()
	{
		TArray<FSoftObjectPath> Paths;
		GetContactMapPaths(Paths);
		for (const FSoftObjectPath& Path : Paths)
		{
			Path.TryLoad();
		}
	}

#if WITH_EDITORONLY_DATA
	// 아래 정점 배열들은 굽기와 굽기 검사 도구만 읽는다 — 쿡에 들어가지 않는다(일지 222).
	// 겹친 층(ADR-0014): 측정 베이크가 만들고, 관성 무름·본 그룹·페인트(눌리는 정도·출렁이는 정도)에 똑같이 적용한다.
	/**
	 * Layered clothing: for each cloth vertex lying over skin (such as underwear), the skin vertices it follows and their weights, so
	 * cloth and skin move together and the skin does not poke through.
	 */
	UPROPERTY()
	TArray<FSoftbodyLayerLink> LayerLinks;

	/** Distance used to find layered clothing (cm, 0 = not searched). Comes from Layer Max Gap on the authoring asset. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody")
	float LayerMaxGap = 0.f;

	// 덩어리마다 원판 누름의 FEM 응답(SoftbodyLumpRegion). 측정 굽기가 채우고 캐리어 굽기가 국소 모양으로 마무리한다.
	/** Raw local press responses per lump, solved by the measure bake and finished by the contact map bake. */
	UPROPERTY()
	TArray<FSoftbodyLumpDiscBake> LumpDiscBakes;

	// MeshDescription 정점 순서.
	/** Welded vertex positions of the baked mesh (mesh local), used by the bake check tools. */
	UPROPERTY()
	TArray<FVector3f> VertexPositions;

	/** Surface area per vertex (a third of the adjacent triangle areas), in the same order as VertexPositions. */
	UPROPERTY()
	TArray<float> VertexAreas;

	// 오프라인 베이크 커버리지 진단 스키마. 0은 진단 도입 전 데이터, 1은 임베딩/코어 연결/최종 관성 응답 RMS를 정점별로 기록한다.
	/** Format version of the per-vertex bake diagnostics. 0 = none recorded. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Softbody|Diagnostics")
	int32 BakeDiagnosticsVersion = 0;

	// 0=미기록, 1=임베딩 실패, 2=tet 외삽, 3=tet 내부 포함.
	/** Per vertex: 0 = not recorded, 1 = outside the measured flesh (failed), 2 = just outside (extrapolated), 3 = inside. */
	UPROPERTY()
	TArray<uint8> VertexEmbeddingStatus;

	// 선택된 tet의 최소 barycentric weight. 임베딩 실패는 -1.
	/** Per vertex: how far inside the measured flesh the vertex sits (-1 = failed). */
	UPROPERTY()
	TArray<float> VertexEmbeddingMinWeight;

	/** Per vertex: 0 = unknown or failed, 1 = connected to the rigid core, 2 = not connected. */
	UPROPERTY()
	TArray<uint8> VertexCoreConnection;

	// 최종 저장된 3방향 무름 벡터의 정점별 RMS.
	/** Per vertex: average size of the baked softness over the three directions. */
	UPROPERTY()
	TArray<float> VertexInertiaResponseRms;
#endif
};
