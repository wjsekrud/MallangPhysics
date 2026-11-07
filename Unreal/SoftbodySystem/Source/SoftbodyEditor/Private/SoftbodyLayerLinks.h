// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "SoftbodyMeshData.h"

struct FMeshDescription;

/**
 * 겹친 층(ADR-0014): 속옷처럼 살 위에 얇게 덮인 면의 정점이 바로 아래 살의 값(눌리는 정도·흔들림의 무름·본 그룹)을
 * 따르게 하는 연결표. 같은 자리의 두 층이 같은 변위를 받아, 누르거나 흔들려도 살이 옷을 뚫고 나오지 않는다.
 */
namespace SoftbodyLayers
{
	/**
	 * 정점마다 노멀 반대쪽(안쪽)으로 MaxGap 안을 본다.
	 *  - 같은 쪽을 보는 다른 조각의 면이 먼저 나오면 그 면이 덮인 살이다. 그 점의 값을 따른다(MaxGap 절반부터 서서히 덜
	 *    따르고 MaxGap에서 제 값). 같은 조각이면(제 살이 접힌 곳) 따르지 않는다.
	 *  - 반대쪽을 보는 면이 아주 가까이(MaxGap × 0.3 안) 먼저 나오면 제 판의 뒷면이다(두께 있는 천). 그 너머에 덮인 살이
	 *    있으면 그 살을 따른다. 없으면 뒷면에서 덮인 살을 따르는 꼭짓점만 따른다 — 천의 안쪽 면은 바깥 면을 거쳐 같은 살을
	 *    따르고, 손가락 사이처럼 살끼리 얇은 곳의 앞뒤는 제 값 그대로다. 손가락처럼 두꺼운 곳은 뒷면이 멀어 보지 않는다.
	 *  - 같은 자리에 겹친 정점(두께 없는 양면 천)은 그 자리에서 따르는 곳이 있는 정점을 함께 따른다.
	 * 따름은 사슬로 이어질 수 있어(겉옷 → 속옷 → 살) 끝까지 풀고, 원래 값들의 무게 합으로 남긴다.
	 * EligibleSlots: 따를 면이 될 수 있는 머티리얼 슬롯 이름(소프트바디 머티리얼 슬롯). 비우면 전부 — 눈꺼풀이 눈알을 따르지
	 * 않게, 흔들리지 않는 슬롯은 건너뛴다.
	 * 결과의 정점 번호 = 텍셀 번호(MeshDescription 정점 원소 순서).
	 */
	void BuildLayerLinks(const FMeshDescription& MD, float MaxGap, const TSet<FName>& EligibleSlots,
		TArray<FSoftbodyLayerLink>& OutLinks);

	/** 정점 값 배열(텍셀 번호)에 연결표를 적용한다: 연결된 정점 = 원래 값들의 무게 합. */
	template<typename ValueType>
	void Apply(const TArray<FSoftbodyLayerLink>& Links, TArray<ValueType>& Values, const ValueType& Zero)
	{
		if (Links.Num() == 0)
		{
			return;
		}
		const TArray<ValueType> Original = Values;
		for (const FSoftbodyLayerLink& Link : Links)
		{
			if (!Values.IsValidIndex(Link.Vertex))
			{
				continue;
			}
			ValueType Sum = Zero;
			for (int32 k = 0; k < Link.Sources.Num(); ++k)
			{
				if (Original.IsValidIndex(Link.Sources[k]))
				{
					Sum += Original[Link.Sources[k]] * Link.Weights[k];
				}
			}
			Values[Link.Vertex] = Sum;
		}
	}
}
