// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"

/**
 * 휘는 판 굽기(ADR-0038 §4, 일지 298). 칠 Flex(코드 이름 Softness)가 흰색보다 어두운 이어진 자리(조각) 하나마다, 압력 풀이가 판이 나르는 짐을 풀 재료를
 * 한 번 구한다. 실행 중 살이 받는 힘은 발자국 거친 칸마다 "그 칸 손 힘 − 판이 나르는 몫"이고, 판이 나르는 몫은 여기서 구운 판 모드의 합이다.
 * 판 모드 = 발자국 위 판 굽힘의 고유 모양 ψ(강체 셋 — 가라앉음·두 기울기 — 에 직교)와 그 굳기 β. 검정은 아주 센 판, 흰색에 가까운 회색은
 * 거의 나르지 않는 판이라 칠 전체가 한 식이다.
 * 식은 Tools/contact_response/replica_plate.py의 PlateBake(ghost='plane')를 그대로 옮겼다. 모든 길이·자리는 메시 로컬 단위다.
 */
namespace SoftbodyPlateBake
{
	/** 검정(Flex 0)의 굽힘 길이 ÷ L0. 끝없이 단단한 판 대신 아주 센 유한 판으로 두어 풀이에 특이한 줄이 생기지 않는다(replica BLACK_ELL). */
	constexpr double BlackEll = 50.0;

	/**
	 * 한 조각의 발자국 거친 칸 상한. 넘으면 굽기가 그 조각을 빼고 알린다. 모드를 자르면 자른 모드를 판이 나르지 않아 회색 판이 몇 배
	 * 틀리므로(plate_report §5.5) 자르지 않고 뺀다.
	 */
	constexpr int32 MaxCells = 512;

	/**
	 * Flex 칠 S → 굽힘 길이(메시 로컬): 0.6·L0·√((1 − S)/S). 흰색 1은 0(맨살), 0.5는 0.6 L0, 검정 쪽은 BlackEll × L0에서 멈춘다. L0 두께
	 * 살에서 손 옆 판 띠가 판째 가라앉는 몫이 약 1 − S가 되게 맞춘 곡선이다(셰이더 SB_HardBendRatio, 재현 plate_ref.ell_b와 짝).
	 */
	double BendingLength(double Softness, double HRef);

	/** 조각 하나의 굽기 재료(메시 로컬). */
	struct FPieceInput
	{
		/** 거친 칸 한 변의 캐리어 칸 수(솟음 거친 격자 BulgeCellBlock). */
		int32 Block = 1;
		/** 거친 격자 한 변의 칸 수(캐리어 해상도 ÷ Block). */
		int32 CoarseResolution = 0;
		/** 캐리어 칸 한 변(CarrierTexelSize). */
		double TexelSize = 1.0;
		/** 굽힘 길이 눈금 L0(SoftbodyContactResponse::ReferenceLength). */
		double HRef = 1.0;
		/** 조각 가는 칸(아틀라스 칸 좌표). 발자국을 한 칸 넓힌 칸도 든다. */
		TArray<FIntPoint> Texels;
		/** 가는 칸마다 정지 자리의 판 좌표 ((x − Center)·AxisU, (x − Center)·AxisV). */
		TArray<FVector2d> Plate;
		/** 가는 칸마다 Softness. */
		TArray<double> Softness;
		/** 가는 칸마다 차트 번호. 굽힘 차분은 같은 차트 칸끼리만 센다(아틀라스에서 우연히 붙은 두 차트를 잇지 않게). */
		TArray<int32> Chart;
		/**
		 * 조각 칸 둘레 고스트 칸(가는 칸 자리)과 그 고스트가 값을 가져오는 조각 칸(Texels 안 번호). 굽힘 차분은 이 고스트를 그 조각 칸으로 읽어
		 * 차트 이음매를 건너간다 — 고스트는 이음매 건너편 칸의 값을 그대로 쓰므로 같은 자리 = 같은 값이다(일지 300). 전에는 이음매에서 차분이 끊겨
		 * 작은 UV 섬 여러 개로 된 장식(버클)이 섬마다 따로 가라앉고 기울었다(장식 옷 시험 캐릭터 앞 버클 판 모드 8개 중 7개가 굽힘 0).
		 */
		TArray<FIntPoint> GhostTexels;
		TArray<int32> GhostMember;
		/** 발자국 거친 칸(HardPieceCells와 같은 차례 — 조각 안 번호 f). */
		TArray<FIntPoint> Cells;
	};

	/** 조각 하나의 굽기 결과. 구간 첫 자리(GradRange.X, Nodes의 첫 원천)는 이 조각 배열 안 자리다 — 합칠 때 앞 조각 몫을 더한다. */
	struct FPieceOutput
	{
		/** 발자국 칸마다 (x_F, y_F, A_F, 0): 그 칸 조각 가는 칸 판 좌표 평균과 넓이. */
		TArray<FVector4f> CellPlate;
		/** 발자국 칸마다 국소 판 기울기 항 (첫 자리, 수). */
		TArray<FUintVector2> GradRange;
		/** 기울기 항 (asfloat(이웃 f), Gx 무게, Gy 무게, 0). 제 칸 몫도 한 항이다. */
		TArray<FVector4f> GradEntries;
		/** 마디 상자(거친 칸): 발자국 + 둘레 두 칸. */
		FIntPoint NodeBoxMin = FIntPoint::ZeroValue;
		FIntPoint NodeBoxSize = FIntPoint::ZeroValue;
		/** 상자 칸마다 마디 번호, 없으면 −1. */
		TArray<int32> NodeSlots;
		/** 마디마다 (X, Y, asfloat(첫 원천), asfloat(원천 수)). */
		TArray<FVector4f> Nodes;
		/** 마디 원천(조각 안 발자국 번호 f). */
		TArray<uint32> NodeSrc;
		/** 판 모드 수(n_F − 강체 셋 차수). */
		int32 NumModes = 0;
		/** 모드마다 굳기 β(오름차순)와 파수 k. */
		TArray<float> Beta;
		TArray<float> WaveNumber;
		/** 모드 모양 ψ 반정밀 비트, n_F × NumModes(F 먼저). */
		TArray<uint16> Shapes;

		// 굽기 확인(로그용)
		/** 강체 셋 차수(보통 3, 한 줄 모양이면 2). */
		int32 RigidRank = 0;
		/** 굳기가 가장 큰 모드의 1e-9 배보다 무른 모드 수 — 차트 이음매로 갈린 발자국은 판끼리 굽힘이 이어지지 않아 빈 모드가 된다. */
		int32 ExtraNull = 0;
		/** max |ψᵀψ − I| (배정밀, 반정밀). */
		double OrthoError = 0.0;
		double HalfOrthoError = 0.0;
		/** max |Q3ᵀψ| — 모드가 강체 셋과 직교하는가. */
		double RigidLeak = 0.0;
		/** 가장 작은 β ÷ 가장 큰 β(반올림으로 생긴 음수를 0으로 두기 전). */
		double MinBetaRatio = 0.0;
		/** 실패 까닭(사용자에게 보이는 영어). */
		FString Error;
	};

	/**
	 * 가는 칸마다 Catmull-Rom 판 읽기가 쓰는 둘레 4 × 4 마디가 거친 격자 안에 다 있는가. 아틀라스 가장자리 두 거친 칸 안의 조각은 판을 읽지
	 * 못해 굽기가 뺀다.
	 */
	bool NodesInsideGrid(TConstArrayView<FIntPoint> Texels, int32 Block, int32 CoarseResolution);

	/** 조각 하나를 굽는다. 실패하면 Out.Error를 채우고 false. */
	bool BakePiece(const FPieceInput& In, FPieceOutput& Out);

	/** 굽기 확인용: 조각 하나의 재료와 결과를 CSV로 적는다(Tools/contact_response의 PlateBake와 견줄 때). */
	void WriteDebugCsv(const FPieceInput& In, const FPieceOutput& Out, const FString& Path);
}
