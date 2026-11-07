// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "SoftbodyMeshData.h"

struct FMeshDescription;
class UStaticMesh;
namespace UE::Geometry { class FDynamicMesh3; }

/**
 * 무름 풀이가 만든 살 격자와 그 분해를 굽기 함수가 끝날 때까지 쥔다(정의는 SoftbodyMeasure.cpp). 덩어리 원판 누름
 * (SolveSoftbodyLumpDiscs)이 분해를 다시 하지 않고 같은 격자로 더 푼다. 격자와 분해가 수백 MB일 수 있어 쓰고 나면 바로 놓는다.
 */
struct FSoftbodyMeasureSolver;

/** 측정 베이크 파라미터: 메시를 사면체 격자로 채워 선형 FEM으로 살의 무름(ADR-0005)을 구한다. */
struct FSoftbodyMeasureParams
{
	/** tet 격자 셀 크기 = 바운드 최장변 × 이 값. 작을수록 얇은 살이 격자에 잡히지만 베이크가 느려진다. */
	float CellSizeFrac = 0.025f;

	/** 강체 내부 프리미티브 (xyz=로컬 중심, w=반지름). 저작 형식이 아니라 함수 인자 수준 — 본 매핑은 범위 외 */
	TArray<FVector4> RigidSpheres;

	/**
	 * 속 메시(ADR-0028): 살을 채울 때 안팎을 읽는 메시. nullptr면 측정 대상 메시 자체. 닫히지 않은 얇은 막(트램펄린)은 제 메시로는
	 * 속이 없어 격자를 채울 수 없으므로, 같은 자리에 속을 채운 메시를 준다. 살갗(칠을 읽는 면, 살갗까지 거리)은 그대로 대상 메시다.
	 */
	UStaticMesh* VolumeMesh = nullptr;

	/**
	 * 재질 — 무름은 공통 최대로 정규화하므로 E의 전역 스케일은 상쇄되고 비율만 남는다.
	 * 푸아송 비 = 살이 눌릴 때 부피를 얼마나 지키는가(0.5 = 전혀 안 줄어든다). 조절값이 아니라 부피 보존 v = 이 값 × 칠(아래
	 * VertexVolume)에서 곡선 v → ν(SoftbodyContactResponse::PoissonOfVolume, ADR-0037 §3.3)로 정해진다 — 표면 장(함몰·솟음)이 같은 v를
	 * 같은 곡선으로 읽어(응답 표), 살이 부피를 지키는 정도를 한 군데서 정한다.
	 *  - v 1 = ν 0.49: 실제 살. 곧장 눌린 덩어리가 납작해지며 옆으로 퍼진다.
	 *  - v 0 = ν 0: 눌린 자리에서 부피가 줄어드는 폼·스펀지. 덩어리가 옆으로 퍼지지 않고 눌린 만큼 줄어든다.
	 *  - 사이 값은 3-D 살층에서 솟는 몫이 v가 되는 ν(v 0.5 ≈ 0.44). 0.35(옛 굽기 값)는 눌린 깊이에 견준 옆 퍼짐이
	 *    0.49의 절반쯤일 뿐, 옆으로 나오는 양 자체는 1~2할밖에 줄지 않는다(일지 144).
	 * 0.45를 넘는 값은 부피 변형을 격자 점 둘레로 평균하는 요소라야 굳지 않는다(ADR-0005 §8).
	 * VolumeValue = 저작 에셋의 Volume Preservation 값(메시 전체, 0~1). 칠이 없으면 이 값이 곧 v다.
	 */
	float VolumeValue = 1.f;
	float RigidStiffnessMul = 1000.f;

	/**
	 * 격자 한 칸보다 가는 단단한 속도 격자에 잡히게 하는 최소 반지름(격자 칸 수). 격자 점은 구 안에 들어야 고정되는데, 구가 칸보다
	 * 작으면 점이 걸리는 곳과 안 걸리는 곳이 생겨 가는 뼈(손가락, 팔뚝)가 토막 난다. 0.6칸이면 어디서든 점이 하나는 걸린다
	 * (격자는 체심 입방이라 가장 가까운 점까지 최대 0.56칸). 뼈대의 본 선으로 만든 속(스켈레탈)에 쓰고, 저작자가 직접 놓은 구
	 * (스태틱)는 0으로 그대로 둔다.
	 */
	float RigidMinRadiusCells = 0.f;

	/**
	 * 관성용 "살의 무름" 3방향을 굽는다(ADR-0005): 강체 프리미티브 내부 노드를 고정하고, 나머지 살에
	 * X·Y·Z 방향으로 같은 크기의 체적력(질량 비례)을 걸었을 때 표면이 밀리는 모양. 앵커가 없으면
	 * 만들지 않으며 중심/피벗을 추정하지 않는다.
	 */
	bool bBakeInertiaResponses = false;

	/**
	 * Depth 칠(ADR-0038 §3.2 — 접촉의 살 두께와 같은 칠). MD 정점 ID 기준(GetArraySize 크기)이고, 비어 있으면 전부 1이다. 뼈가 살갗에서
	 * 얼마나 가까운가: 살 속 격자 점의 깊이(가장 가까운 살갗까지) d가 그 살갗 점의 값 p × 그 점에서 단단한 속(RigidSpheres) 겉까지 거리 D
	 * 이상이면 뼈로 고정한다. 1이면 속까지 살(고정 없음), 0이면 살갗 바로 밑부터 뼈. 뼈대에 본이 없는 넓은 뼈(갈비뼈, 골반, 머리뼈)를 칠로
	 * 알려 주는 길이다 — 접촉도 같은 칠로 그만큼 얇은 살을 본다(일지 298 전에는 Flesh Softness 0으로 따로 칠했다).
	 */
	TArray<float> VertexDepth;

	/**
	 * Jiggle 칠(살의 무름, ADR-0038 §3.3 — 옛 Flesh Softness의 강성 규칙). MD 정점 ID 기준이고, 비어 있으면 전부 1이다. 1이면 보통 살,
	 * 그 사이는 그만큼 단단한 살(강성 = 1 ÷ 값, 뼈 강성까지)이다. 0이어도 고정하지 않는다(뼈는 Depth). 살 속의 점은 가장 가까운 살갗 점의 값을 따른다.
	 */
	TArray<float> VertexJiggle;

	/**
	 * 부피 보존 칠(ADR-0024). MD 정점 ID 기준(GetArraySize 크기)이고, 비어 있으면 전부 1이다. 그 살갗 밑의 살이 눌릴 때 부피를
	 * 얼마나 지키는가: 1이면 살·젤, 0이면 폼(위 VolumeValue에 곱한다). 살 속의 점은 가장 가까운 살갗 점의 값을 따른다
	 * (Jiggle 칠과 같은 방법).
	 */
	TArray<float> VertexVolume;

	/**
	 * 떠 있는 하드 서피스 조각(ADR-0037 §3.7 — 옷 위 보석·금속 장식)의 정점 자리(메시 로컬). 세 코너가 모두 여기 든 삼각형은 살갗(칠·살갗까지
	 * 거리를 읽는 면)과 살을 채우는 안팎에서 뺀다 — 조각 밑 살은 그 밑 겉면의 칠을 읽는다(시험 캐릭터 가슴 보석이 Chest 본 웨이트로 뼈 칠을 받아
	 * 둘레 살을 뼈로 만든 일, 일지 285). 비면 지금 그대로. HardMaskHash = 그 칠의 지문(칠 단계와 견준다).
	 */
	TSet<FVector3f> SkinSkipPositions;
	uint32 HardMaskHash = 0;

	/**
	 * 살이 한 덩어리로 흔들리는 두께(메시 로컬 cm, ADR-0027). 0보다 크면 쓴다(일지 298 전에는 살의 말랑함 칠이 있을 때만). 살을 단단한 속에 고정하는 것에 더해
	 * 살 전체를 뼈대에 스프링으로 매단다(스프링의 세기 = 살의 전단 강성 ÷ 이 값²). 살이 이보다 두꺼운 곳은 더 두꺼워져도 더 밀리지
	 * 않아 밀리는 양이 칠한 말랑함을 따르고, 이보다 얇은 살은 전처럼 얇은 만큼 덜 밀린다. 0이면 매달지 않는다: 그때는 살이 깊은
	 * 곳일수록 크게 밀려, 속에 뼈가 없는 배가 같은 밝기의 가슴보다 몇 배 크게 흔들린다.
	 */
	float FleshReach = 0.f;

	/**
	 * 겹친 층(ADR-0014): 속옷처럼 살 위에 덮인 면이 바로 아래 살의 값을 따르게 찾는 거리(cm, 0 = 끔).
	 * LayerSlots: 따를 면이 될 수 있는 머티리얼 슬롯 이름(소프트바디 머티리얼 슬롯). 비우면 전부.
	 */
	float LayerMaxGap = 0.f;
	TSet<FName> LayerSlots;
};

struct FSoftbodyMeasureOut
{
	/**
	 * 베이크 커버리지 진단 v1. 배열은 MD 정점 ID 기준(GetArraySize 크기).
	 * EmbeddingStatus: 0=미기록, 1=실패, 2=tet 외삽, 3=tet 내부 포함.
	 * CoreConnection: 0=미확인/임베딩 실패, 1=물리 코어 연결, 2=코어 미연결.
	 */
	int32 BakeDiagnosticsVersion = 0;
	TArray<uint8> VertexEmbeddingStatus;
	TArray<float> VertexEmbeddingMinWeight;
	TArray<uint8> VertexCoreConnection;
	/** 최종 저장되는 3방향 무름의 정점별 벡터 RMS. */
	TArray<float> VertexInertiaResponseRms;

	/** ADR-0005 살의 무름 3방향(체적력 +X/+Y/+Z에 대한 앵커 구속 정적 변형). */
	int32 InertiaResponseVersion = 0;
	int32 NumInertiaResponses = 0;
	FVector3f InertiaAnchorLocal = FVector3f::ZeroVector;
	int32 InertiaAnchorNodeCount = 0;
	float InertiaMaxAnchorError = 0.f;
	float InertiaMaxRelativeResidual = 0.f;
	/** 앵커 중심에서 가장 먼 표면 정점까지 거리(메시 로컬). 회전 흔들림 상한 계산용. */
	float InertiaAnchorReach = 0.f;
	/** 방향별 원시 최대 진폭(진단용). 세 방향은 이 중 최댓값 하나로 함께 정규화된다. */
	TArray<float> InertiaResponseScales;
	/** 응답장-메이저 [k·GetArraySize + VertID], 세 방향 공통 최대 진폭 1로 정규화. */
	TArray<FVector3f> VertexInertiaResponseShapes;

	int32 NumTets = 0;
	int32 NumNodes = 0;

	/** 무름을 구웠을 때만: 살 격자와 그 분해. 덩어리 원판 누름(SolveSoftbodyLumpDiscs)이 쓰고 굽기 함수가 끝나기 전에 놓는다. */
	TSharedPtr<FSoftbodyMeasureSolver> Solver;
};

/**
 * 측정 백엔드: winding number 솔리드화 → 격자 tet(IsosurfaceStuffing) → 선형 FEM → 강체 프리미티브 안 노드를 고정하고
 * +X/+Y/+Z 체적력에 대한 정적 변형(살의 무름 3방향, bBakeInertiaResponses일 때) + 표면 정점 임베딩 진단.
 * Solid: 살을 채울 때 안팎을 읽는 메시(속 메시, ADR-0028). nullptr면 DynMesh. 살갗 질의(칠, 살갗까지 거리)는 늘 DynMesh다.
 */
bool RunSoftbodyMeasure(const FMeshDescription& MD, UE::Geometry::FDynamicMesh3& DynMesh,
	const UE::Geometry::FDynamicMesh3* Solid, const FSoftbodyMeasureParams& Params, FSoftbodyMeasureOut& Out);

/**
 * 덩어리 국소 모양(SoftbodyLumpRegion)의 재료. 덩어리마다 살갗에 원판 누름 SoftbodyLumpRegion::MaxModes개를 고르게 놓고(가장 먼 점
 * 뽑기), 무름과 같은 격자·분해로 FEM 응답을 풀어 OutBakes에 적는다(덩어리 하나에 하나). 캐리어 굽기가 칠과 덩어리 양을 넣어 마무리한다.
 * 원판의 힘은 표본 커널의 전치로 격자 점에 나눠 걸고 응답은 같은 커널로 읽어, 정점에서 정점으로 가는 응답이 대칭이다. 응답은 정규화하지
 * 않은 원시 단위(살 단단함 1, 힘 합 1)다.
 * 배열은 굽기 정점 순서(MeshDescription 정점 원소 순서 = 텍셀 번호)다. VertTexel: MD 정점 ID → 굽기 정점 번호. VertexAreas: 정점 넓이.
 * VertexLump·VertexLumpWeight: 정점의 덩어리 묶음(없으면 INDEX_NONE)과 그 몫. LayerLinks: 겹친 층 — 따르는 정점은 원판에 들지 않고, 응답은
 * 무름처럼 아래 살의 값을 섞어 쓴다. 덩어리가 없거나 하나도 풀지 못하면 false.
 */
bool SolveSoftbodyLumpDiscs(const FSoftbodyMeasureSolver& Solver, const FMeshDescription& MD, TConstArrayView<int32> VertTexel,
	TConstArrayView<float> VertexAreas, TConstArrayView<int32> VertexLump, TConstArrayView<float> VertexLumpWeight,
	const TArray<FSoftbodyLayerLink>& LayerLinks, TArray<FSoftbodyLumpDiscBake>& OutBakes);
