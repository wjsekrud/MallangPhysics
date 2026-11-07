// Copyright (c) 2026 wjsekrud

#include "SoftbodyColliderField.h"

#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Math/Float16.h"

USoftbodyColliderField* USoftbodyColliderField::FindOnAsset(UObject* MeshAsset)
{
	IInterface_AssetUserData* Holder = Cast<IInterface_AssetUserData>(MeshAsset);
	USoftbodyColliderField* Field = Holder
		? Cast<USoftbodyColliderField>(Holder->GetAssetUserDataOfClass(USoftbodyColliderField::StaticClass()))
		: nullptr;
	return Field && Field->IsUsable() ? Field : nullptr;
}

USoftbodyColliderField* USoftbodyColliderField::FindOnPrimitive(const UPrimitiveComponent* Component)
{
	if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
	{
		return FindOnAsset(Static->GetStaticMesh());
	}
	if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
	{
		return FindOnAsset(Skinned->GetSkinnedAsset());
	}
	return nullptr;
}

FVector3f USoftbodyColliderField::GetPartFleshHalfExtent(int32 PartIndex) const
{
	if (!Parts.IsValidIndex(PartIndex))
	{
		return FVector3f::ZeroVector;
	}
	BuildPartCaches();
	return FleshHalfExtentCache[PartIndex];
}

void USoftbodyColliderField::BuildPartCaches() const
{
	if (FleshHalfExtentVoxels == Voxels.Num() && FleshHalfExtentCache.Num() == Parts.Num())
	{
		return;
	}
	FleshHalfExtentCache.SetNum(Parts.Num());
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const FSoftbodyColliderFieldPart& Part = Parts[Index];
		FIntVector Lo(TNumericLimits<int32>::Max());
		FIntVector Hi(TNumericLimits<int32>::Min());
		const int32 Cells = Part.Dims.X * Part.Dims.Y * Part.Dims.Z;
		for (int32 Cell = 0; Cell < Cells && Voxels.IsValidIndex(Part.FirstVoxel + Cell); ++Cell)
		{
			FFloat16 Distance;
			Distance.Encoded = Voxels[Part.FirstVoxel + Cell];
			const float D = Distance.GetFloat();
			if (D >= 0.f)
			{
				continue;
			}
			const FIntVector C(Cell % Part.Dims.X, (Cell / Part.Dims.X) % Part.Dims.Y, Cell / (Part.Dims.X * Part.Dims.Y));
			Lo = FIntVector(FMath::Min(Lo.X, C.X), FMath::Min(Lo.Y, C.Y), FMath::Min(Lo.Z, C.Z));
			Hi = FIntVector(FMath::Max(Hi.X, C.X), FMath::Max(Hi.Y, C.Y), FMath::Max(Hi.Z, C.Z));
		}
		const FIntVector Box = Lo.X <= Hi.X ? Hi - Lo + FIntVector(1) : Part.Dims;
		FleshHalfExtentCache[Index] = 0.5f * Part.VoxelSize * FVector3f((float)Box.X, (float)Box.Y, (float)Box.Z);
	}
	FleshHalfExtentVoxels = Voxels.Num();
}
