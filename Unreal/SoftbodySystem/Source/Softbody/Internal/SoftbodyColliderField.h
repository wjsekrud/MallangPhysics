// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "SoftbodyColliderField.generated.h"

class UPrimitiveComponent;

// 모양 거리장의 부위 하나 (ADR-0004). 기본 포즈 메시 공간의 3D 격자에 "그 부위 살 표면까지의 거리"를 담는다.
/**
 * One part of a shape field. On a skeletal mesh there is one part per bone, and it moves rigidly with that bone while playing.
 * On a static mesh the whole mesh is one part.
 */
USTRUCT()
struct SOFTBODY_API FSoftbodyColliderFieldPart
{
	GENERATED_BODY()

	/** The bone that moves this part. None on a static mesh. */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	FName Bone;

	/** Center of the first grid cell (mesh local). */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	FVector3f Origin = FVector3f::ZeroVector;

	/** Size of one grid cell (mesh local units). */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	float VoxelSize = 1.f;

	/** Number of grid cells along each axis (at least 2). */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	FIntVector Dims = FIntVector(2, 2, 2);

	// USoftbodyColliderField::Voxels에서 이 부위가 시작하는 칸.
	/** Index of this part's first cell in the shape field's cell list. */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	int32 FirstVoxel = 0;

	// 개발: 자기 접촉에 싣는 부위인가(저작 데이터 Self Contact Bones 아래 본, ADR-0004 §11). 다른 소프트바디는 모든 부위로 눌린다.
	// 이 값이 생기기 전의 거리장은 누르는 본만 부위로 구웠으므로 기본값 true가 옛 동작과 같다.
	/** Whether this part also presses the character's own flesh (Self Contact Bones). Every part presses other Softbody meshes. */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	bool bPressesSelf = true;
};

// 누르는 쪽의 실제 모양 (ADR-0004). USoftbodyBakeLibrary::BakeColliderField가 굽는다. 컴플렉스 콜리전으로 누른다.
/**
 * Shape field: a baked copy of a presser's real shape, stored on the mesh asset (Bake Shape Field).
 * A mesh with one presses Softbody surfaces with its real shape, and on a Softbody character it also gives self contact.
 */
UCLASS()
class SOFTBODY_API USoftbodyColliderField : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** 현재 데이터 형식. 굽는 규칙이 바뀌면 올리고, 옛 데이터는 쓰지 않는다. */
	static constexpr int32 CurrentVersion = 1;

	/** The baked parts: one per bone on a skeletal mesh, one for the whole mesh on a static mesh. */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	TArray<FSoftbodyColliderFieldPart> Parts;

	// 부위들의 칸 값: 부호 있는 거리(메시 로컬 단위, 살 안쪽이 음수)의 반정밀 비트. 칸 순서는 x가 가장 빠르다.
	/** Baked distance to the surface for every cell of every part (negative inside). */
	UPROPERTY()
	TArray<uint16> Voxels;

	/** Format of the bake. A shape field from an older format is not used: bake it again. */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	int32 Version = 0;

	/** Cell size used for the bake (mesh local units, for display). */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	float BakedVoxelSize = 0.f;

	/** The material slots the shape was baked from (Shape Field Slots). Empty = the whole mesh. */
	UPROPERTY(VisibleAnywhere, Category = "Softbody")
	TArray<FName> MaterialSlots;

	bool IsUsable() const { return Version == CurrentVersion && Parts.Num() > 0 && Voxels.Num() > 0; }

	/** Returns the usable shape field stored on a static or skeletal mesh asset, or None if it has none or it is out of date. */
	UFUNCTION(BlueprintPure, Category = "Softbody|Shape Field")
	static USoftbodyColliderField* FindOnAsset(UObject* MeshAsset);

	/** 프리미티브가 그리는 메시 에셋의 거리장. */
	static USoftbodyColliderField* FindOnPrimitive(const UPrimitiveComponent* Component);

	/**
	 * 부위의 살이 든 칸(거리 < 0)을 감싸는 상자의 반폭(메시 로컬 단위). 격자 상자는 바깥 여백(MarginVoxels + 1칸 + 겹침)이 붙어 손가락보다
	 * 훨씬 넓다. 덩어리 힘이 누르는 것의 가로 크기로 쓴다(일지 248 ②). 칸 값에서 처음 한 번 구해 둔다(저장하지 않는다 — 다시 굽지 않아도 된다).
	 * 살 칸이 없는 부위는 격자 상자의 반폭.
	 */
	FVector3f GetPartFleshHalfExtent(int32 PartIndex) const;


private:
	/** 부위마다의 살 상자 반폭을 칸 값에서 구한다(칸 수가 바뀌면 — 다시 구우면 — 다시). */
	void BuildPartCaches() const;

	mutable TArray<FVector3f> FleshHalfExtentCache;
	mutable int32 FleshHalfExtentVoxels = INDEX_NONE;
};
