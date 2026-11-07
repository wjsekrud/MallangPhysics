// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "UObject/WeakObjectPtr.h"

class UPrimitiveComponent;
struct FKAggregateGeom;

/**
 * 콜라이더 형상. 프리미티브의 BodySetup(AggGeom)에서 구·캡슐·박스·컨벡스를 월드 프레임으로 펼친 해석적 표현이다.
 * 표면 장이 이 형상을 GPU로 올려 광선으로 누른 깊이를 잰다(SoftbodySurfaceField). 스케일은 파라미터에 접었고
 * Frame은 회전+이동만 갖는다.
 */
enum class ESoftbodyColliderShapeType : uint8
{
	Sphere,
	Capsule,
	Box,
	Convex
};

struct SOFTBODY_API FSoftbodyColliderShape
{
	ESoftbodyColliderShapeType Type = ESoftbodyColliderShapeType::Sphere;
	/** 형상 로컬 → 월드 (스케일 없음). 캡슐 축은 로컬 Z. */
	FTransform Frame = FTransform::Identity;
	/** 구·캡슐 반경 (cm). */
	float Radius = 0.f;
	/** 캡슐 원통부 반길이 (cm). */
	float HalfLength = 0.f;
	/** 박스 반폭 (cm). 컨벡스는 로컬 정점 상자의 반폭 — 덩어리 힘이 누르는 것의 가로 크기로 쓴다(일지 248 ②). */
	FVector HalfExtent = FVector::ZeroVector;
	/** 컨벡스: 로컬 평면 집합. 안쪽은 모든 평면에서 dot(N, x) <= W. */
	TArray<FPlane> LocalPlanes;
	/** 컬링용 월드 AABB. */
	FBox WorldBounds = FBox(ForceInit);
	/** 소스 식별 (끌림이 지난 프레임 위치를 찾는 열쇠): 컴포넌트 + 바디/요소 인덱스. */
	TWeakObjectPtr<const UPrimitiveComponent> Component;
	int32 SourceIndex = 0;

	void UpdateBounds();
};

namespace SoftbodyColliderShapes
{
	/**
	 * 스케일 포함 바디 변환으로 AggGeom을 월드 형상으로 펼친다. 테이퍼드 캡슐은 큰 반경의 캡슐로
	 * 근사하고, 컨벡스는 IndexData 삼각형 평면(없으면 건너뜀)을 쓴다. 추가한 형상 수를 반환한다.
	 */
	SOFTBODY_API int32 AppendShapesFromAggregate(
		const FKAggregateGeom& AggGeom, const FTransform& BodyToWorld,
		const UPrimitiveComponent* Component, int32 SourceIndexBase,
		TArray<FSoftbodyColliderShape>& OutShapes);

	/**
	 * 프리미티브의 단순 콜리전을 형상으로 만든다. 스켈레탈은 피직스 에셋 바디별(본 변환),
	 * 그 밖은 GetBodySetup()의 AggGeom. 단순 형상이 없는 컴플렉스 전용 콜리전은 0을 반환한다.
	 */
	SOFTBODY_API int32 AppendShapesFromPrimitive(
		const UPrimitiveComponent* Component, TArray<FSoftbodyColliderShape>& OutShapes);

	/** 삼각형 인덱스에서 바깥 방향 평면 집합을 만든다(중복 제거). */
	SOFTBODY_API void BuildConvexPlanes(
		TConstArrayView<FVector> Vertices, TConstArrayView<int32> Indices,
		TArray<FPlane>& OutPlanes);
}
