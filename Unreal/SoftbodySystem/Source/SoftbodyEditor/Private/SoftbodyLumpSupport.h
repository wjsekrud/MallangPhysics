// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"

/**
 * 덩어리 받침(ADR-0034): 덩어리 칸의 살 두께를 그 덩어리가 얹힌 면까지로 잰다. 뼈대에 갈비뼈가 없어 가슴 칸이 척추까지 재지던
 * 것(시험 캐릭터 가슴 끝 10.6 cm, 둔덕 높이는 약 5 cm)을 바로잡는다.
 *  - 영역: 덩어리 본의 스킨 웨이트가 걸린 살갗에서 시작해, 아래 막의 꼭대기 밑 깊이가 더 늘지 않을 때까지 한 고리씩 넓힌다.
 *    웨이트는 어느 덩어리인지와 그 가운데를 정하고, 둔덕이 어디서 끝나는지는 형상이 정한다 — 출렁임 모양을 위해 둔덕 위쪽만
 *    칠한 웨이트(시연 몸: 둔덕의 약 3분의 1, 최대 0.56)도 둔덕 전체로 넓어진다. 웨이트가 둔덕을 다 덮으면 넓히지 않는다.
 *  - 막: 영역 밖 살갗을 위치와 기울기까지 이어 영역 안을 가장 매끄럽게 메운 면(bilaplacian, 영역 밖 두 고리가 고정) = 그 덩어리가
 *    없을 때의 몸 겉면.
 *  - 밑 두께: 영역 바로 밖의 지금 살 두께(뼈까지 거리 × 눌리는 정도 칠)를 영역 안으로 고르게 이은 값(조화 보간).
 * 칸의 두께 = min(지금 두께, 막 위 높이 × 칠 + 밑 두께). 영역 가장자리에서 지금 두께와 같아 새 경계가 생기지 않는다. 덩어리는
 * 둘레 몸이 얇을 때만 얇아진다 — 둘레를 칠로 얇게 한 가슴은 둔덕 높이가 되고, 두꺼운 엉덩이·허벅지 위의 엉덩이는 거의 그대로다.
 * 뼈대에 없는 뼈(갈비뼈)는 지금처럼 칠의 몫이다(ADR-0026 §2.3).
 */
namespace SoftbodyLumpSupport
{
	/** 렌더 정점(텍셀 테이블 순서)마다 넣는 값. */
	struct FInput
	{
		TArray<FVector3d> Positions;
		/** 렌더 정점 번호 셋. 모든 슬롯(옷 조각은 받침을 찾는 살갗에서 빠진다 — 아래 Build). */
		TArray<FIntVector> Triangles;
		/** 덩어리 묶음(관성 그룹 번호), 없으면 INDEX_NONE. */
		TArray<int32> Lump;
		/** 그 묶음의 몫(0~1). */
		TArray<float> LumpWeight;
		/** 뼈(코어 구)까지 거리 — 눌리는 정도 칠을 곱하기 전의 살 두께. */
		TArray<double> BoneDepth;
		/** 눌리는 정도 칠(0~1). 못 읽은 정점은 1. */
		TArray<double> Gain;
		/** 코어 구(x, y, z, 반지름) — 꼭대기를 뼈에서 먼 쪽으로 찾을 때. */
		TArray<FVector4> Cores;
		/** 로그용 묶음 이름. */
		TArray<FName> GroupBones;
	};

	class FSupport
	{
	public:
		/** 받침을 찾은 덩어리가 하나라도 있으면 true. 못 찾은 덩어리는 경고를 남기고 지금 규칙(뼈까지)을 그대로 쓴다. */
		bool Build(const FInput& In);

		/**
		 * 점 하나의 받침. RegionPoint = 그려지는 면 위의 점(그 아래 살갗이 영역 안인지 본다 — 옷 칸은 그 밑 살갗), DepthPoint =
		 * 두께를 재는 점(캐리어 칸의 봉투 점). RegionPoint가 살갗에서 MaxReach보다 멀면 받침이 없다. 받침이 있으면 true:
		 * OutShare = 영역 몫(영역 안 1, 영역 가장자리 삼각형에서 0 → 1), OutHeight = 막까지 거리(막 위 높이),
		 * OutBasePainted / OutBaseUnpainted = 밑 두께(눌리는 정도 칠을 곱한 것 / 곱하기 전).
		 * 여러 스레드에서 불러도 된다.
		 */
		bool Query(const FVector3d& RegionPoint, const FVector3d& DepthPoint, double MaxReach,
			double& OutShare, double& OutHeight, double& OutBasePainted, double& OutBaseUnpainted, int32* OutGroup = nullptr) const;

		/**
		 * 저작 에셋 미리보기(Lump Base 보기)에 남길 막: 막 삼각형이 쓰는 정점만(메시 공간), 삼각형(정점 번호 셋씩), 정점마다 그 위 둔덕 높이
		 * (원래 살갗까지 거리 — 영역 밖 고리는 0). Build가 거짓이면 빈 배열.
		 */
		void ExportMembrane(TArray<FVector3f>& OutVertices, TArray<int32>& OutTriangles, TArray<float>& OutHeights) const;

		/** 덩어리마다 한 줄(미리보기 상태 줄, 영어): 꼭대기의 둔덕 높이와 막 밑 살 두께. */
		const TArray<FString>& GetNotes() const { return Notes; }

	private:
		TArray<FString> Notes;
		/** 받침을 찾은 살갗 조각(원래 자리). 정점 번호 = 아래 배열 번호. */
		UE::Geometry::FDynamicMesh3 Skin;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> SkinTree;
		/** 막: 영역과 그 둘레 한 고리를 덮는 삼각형, 영역 정점은 메운 자리. */
		UE::Geometry::FDynamicMesh3 Membrane;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> MembraneTree;
		TArray<double> SkinShare;
		/** 영역 정점이 속한 덩어리 묶음(넓힌 고리는 넓혀 준 이웃의 묶음), 영역 밖은 INDEX_NONE — 로그용. */
		TArray<int32> SkinGroup;
		TArray<double> SkinBasePainted;
		TArray<double> SkinBaseUnpainted;
	};
}
