// Copyright (c) 2026 wjsekrud

#include "SoftbodyColliderShapes.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

void FSoftbodyColliderShape::UpdateBounds()
{
	WorldBounds = FBox(ForceInit);
	switch (Type)
	{
	case ESoftbodyColliderShapeType::Sphere:
		WorldBounds = FBox::BuildAABB(Frame.GetLocation(), FVector(Radius));
		break;
	case ESoftbodyColliderShapeType::Capsule:
	{
		const FVector Axis = Frame.GetUnitAxis(EAxis::Z) * HalfLength;
		WorldBounds += FBox::BuildAABB(Frame.GetLocation() + Axis, FVector(Radius));
		WorldBounds += FBox::BuildAABB(Frame.GetLocation() - Axis, FVector(Radius));
		break;
	}
	case ESoftbodyColliderShapeType::Box:
		WorldBounds = FBox::BuildAABB(FVector::ZeroVector, HalfExtent).TransformBy(Frame);
		break;
	case ESoftbodyColliderShapeType::Convex:
	default:
		// 평면 집합만으로는 정점을 모르므로 변환 전에 호출자가 채운다.
		break;
	}
}

namespace SoftbodyColliderShapes
{
	void BuildConvexPlanes(
		TConstArrayView<FVector> Vertices, TConstArrayView<int32> Indices,
		TArray<FPlane>& OutPlanes)
	{
		OutPlanes.Reset();
		if (Vertices.Num() < 4 || Indices.Num() < 3)
		{
			return;
		}
		FVector Centroid = FVector::ZeroVector;
		for (const FVector& V : Vertices)
		{
			Centroid += V;
		}
		Centroid /= (double)Vertices.Num();

		for (int32 Tri = 0; Tri + 2 < Indices.Num(); Tri += 3)
		{
			const int32 I0 = Indices[Tri];
			const int32 I1 = Indices[Tri + 1];
			const int32 I2 = Indices[Tri + 2];
			if (!Vertices.IsValidIndex(I0) || !Vertices.IsValidIndex(I1) || !Vertices.IsValidIndex(I2))
			{
				continue;
			}
			FVector Normal = FVector::CrossProduct(Vertices[I1] - Vertices[I0], Vertices[I2] - Vertices[I0]);
			if (!Normal.Normalize(UE_KINDA_SMALL_NUMBER))
			{
				continue;
			}
			double W = FVector::DotProduct(Normal, Vertices[I0]);
			// 바깥 방향 규약: 중심은 항상 안쪽(음수)이어야 한다.
			if (FVector::DotProduct(Normal, Centroid) - W > 0.0)
			{
				Normal = -Normal;
				W = -W;
			}
			bool bDuplicate = false;
			for (const FPlane& Existing : OutPlanes)
			{
				if (FVector::DotProduct(FVector(Existing), Normal) > 0.9999
					&& FMath::Abs(Existing.W - W) < 1e-3)
				{
					bDuplicate = true;
					break;
				}
			}
			if (!bDuplicate)
			{
				OutPlanes.Add(FPlane(Normal, W));
			}
		}
	}

	int32 AppendShapesFromAggregate(
		const FKAggregateGeom& AggGeom, const FTransform& BodyToWorld,
		const UPrimitiveComponent* Component, int32 SourceIndexBase,
		TArray<FSoftbodyColliderShape>& OutShapes)
	{
		const FVector Scale3D = BodyToWorld.GetScale3D();
		FTransform NoScale = BodyToWorld;
		NoScale.RemoveScaling();
		int32 Added = 0;
		int32 Source = SourceIndexBase;

		for (const FKSphereElem& Elem : AggGeom.SphereElems)
		{
			const FKSphereElem Scaled = Elem.GetFinalScaled(Scale3D, FTransform::Identity);
			FSoftbodyColliderShape& Shape = OutShapes.AddDefaulted_GetRef();
			Shape.Type = ESoftbodyColliderShapeType::Sphere;
			Shape.Frame = FTransform(FQuat::Identity, Scaled.Center) * NoScale;
			Shape.Radius = Scaled.Radius;
			Shape.Component = Component;
			Shape.SourceIndex = Source++;
			Shape.UpdateBounds();
			++Added;
		}
		for (const FKSphylElem& Elem : AggGeom.SphylElems)
		{
			const FKSphylElem Scaled = Elem.GetFinalScaled(Scale3D, FTransform::Identity);
			FSoftbodyColliderShape& Shape = OutShapes.AddDefaulted_GetRef();
			Shape.Type = ESoftbodyColliderShapeType::Capsule;
			Shape.Frame = FTransform(Scaled.Rotation, Scaled.Center) * NoScale;
			Shape.Radius = Scaled.Radius;
			Shape.HalfLength = 0.5f * Scaled.Length;
			Shape.Component = Component;
			Shape.SourceIndex = Source++;
			Shape.UpdateBounds();
			++Added;
		}
		for (const FKTaperedCapsuleElem& Elem : AggGeom.TaperedCapsuleElems)
		{
			// 캡슐 근사: 큰 반경.
			const FKTaperedCapsuleElem Scaled = Elem.GetFinalScaled(Scale3D, FTransform::Identity);
			FSoftbodyColliderShape& Shape = OutShapes.AddDefaulted_GetRef();
			Shape.Type = ESoftbodyColliderShapeType::Capsule;
			Shape.Frame = FTransform(Scaled.Rotation, Scaled.Center) * NoScale;
			Shape.Radius = FMath::Max(Scaled.Radius0, Scaled.Radius1);
			Shape.HalfLength = 0.5f * Scaled.Length;
			Shape.Component = Component;
			Shape.SourceIndex = Source++;
			Shape.UpdateBounds();
			++Added;
		}
		for (const FKBoxElem& Elem : AggGeom.BoxElems)
		{
			const FKBoxElem Scaled = Elem.GetFinalScaled(Scale3D, FTransform::Identity);
			FSoftbodyColliderShape& Shape = OutShapes.AddDefaulted_GetRef();
			Shape.Type = ESoftbodyColliderShapeType::Box;
			Shape.Frame = FTransform(Scaled.Rotation, Scaled.Center) * NoScale;
			Shape.HalfExtent = FVector(0.5f * Scaled.X, 0.5f * Scaled.Y, 0.5f * Scaled.Z);
			Shape.Component = Component;
			Shape.SourceIndex = Source++;
			Shape.UpdateBounds();
			++Added;
		}
		for (const FKConvexElem& Elem : AggGeom.ConvexElems)
		{
			++Source;
			if (Elem.VertexData.Num() < 4 || Elem.IndexData.Num() < 12)
			{
				continue;
			}
			// 요소 로컬 정점 → 스케일된 바디 공간. 비균일 스케일에서도 평면은 스케일 후 정점에서 만든다.
			const FTransform ElemToBody = Elem.GetTransform();
			TArray<FVector> ScaledVertices;
			ScaledVertices.Reserve(Elem.VertexData.Num());
			FBox LocalBox(ForceInit);
			for (const FVector& V : Elem.VertexData)
			{
				const FVector Scaled = ElemToBody.TransformPosition(V) * Scale3D;
				ScaledVertices.Add(Scaled);
				LocalBox += Scaled;
			}
			TArray<FPlane> Planes;
			BuildConvexPlanes(ScaledVertices, Elem.IndexData, Planes);
			if (Planes.Num() < 4)
			{
				continue;
			}
			FSoftbodyColliderShape& Shape = OutShapes.AddDefaulted_GetRef();
			Shape.Type = ESoftbodyColliderShapeType::Convex;
			Shape.Frame = NoScale;
			Shape.LocalPlanes = MoveTemp(Planes);
			Shape.HalfExtent = LocalBox.GetExtent();
			Shape.Component = Component;
			Shape.SourceIndex = Source - 1;
			Shape.WorldBounds = LocalBox.TransformBy(NoScale);
			++Added;
		}
		return Added;
	}

	int32 AppendShapesFromPrimitive(
		const UPrimitiveComponent* Component, TArray<FSoftbodyColliderShape>& OutShapes)
	{
		if (!Component)
		{
			return 0;
		}
		if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			const UPhysicsAsset* PhysicsAsset = Skinned->GetPhysicsAsset();
			if (!PhysicsAsset)
			{
				return 0;
			}
			int32 Added = 0;
			for (int32 BodyIndex = 0; BodyIndex < PhysicsAsset->SkeletalBodySetups.Num(); ++BodyIndex)
			{
				const USkeletalBodySetup* Body = PhysicsAsset->SkeletalBodySetups[BodyIndex];
				if (!Body)
				{
					continue;
				}
				const int32 BoneIndex = Skinned->GetBoneIndex(Body->BoneName);
				if (BoneIndex == INDEX_NONE)
				{
					continue;
				}
				const FTransform BoneToWorld = Skinned->GetBoneTransform(BoneIndex);
				Added += AppendShapesFromAggregate(
					Body->AggGeom, BoneToWorld, Component, BodyIndex * 64, OutShapes);
			}
			return Added;
		}

		UBodySetup* BodySetup = const_cast<UPrimitiveComponent*>(Component)->GetBodySetup();
		if (!BodySetup)
		{
			return 0;
		}
		return AppendShapesFromAggregate(
			BodySetup->AggGeom, Component->GetComponentTransform(), Component, 0, OutShapes);
	}
}
