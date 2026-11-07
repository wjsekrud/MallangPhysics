// Copyright (c) 2026 wjsekrud

#include "SoftbodyComponent.h"

#include "Softbody.h"
#include "SoftbodyContactResponse.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "ConvexVolume.h"
#include "Engine/OverlapResult.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RHI.h"
#include "SceneManagement.h"
#include "UObject/UnrealType.h"
#include "SoftbodyColliderField.h"
#include "SoftbodyGrabberComponent.h"
#include "SoftbodyInertiaRuntime.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyProfiling.h"
#include "SoftbodySurfaceField.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyRuntime, Log, All);

DECLARE_CYCLE_STAT(TEXT("Softbody Tick"), STAT_SoftbodyTick, STATGROUP_Softbody);
DECLARE_CYCLE_STAT(TEXT("Softbody Upload"), STAT_SoftbodyUpload, STATGROUP_Softbody);
DECLARE_CYCLE_STAT(TEXT("Softbody Surface Field"), STAT_SoftbodySurfaceField, STATGROUP_Softbody);
// 표면 장 GPU 시간(ms, 모든 표면 장 합) — stat gpu의 Softbody 줄과 같은 값을 모아 보인다.
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Total (ms)"), STAT_SoftbodyGpuTotal, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Contact (ms)"), STAT_SoftbodyGpuContact, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Tiles (ms)"), STAT_SoftbodyGpuTiles, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Dent (ms)"), STAT_SoftbodyGpuDent, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Drag (ms)"), STAT_SoftbodyGpuDrag, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Pressure (ms)"), STAT_SoftbodyGpuPressure, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Compose (ms)"), STAT_SoftbodyGpuCompose, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("GPU Lump (ms)"), STAT_SoftbodyGpuLump, STATGROUP_Softbody);
// 개수와 GPU 상태(GPU 상태는 몇 프레임 늦다)
DECLARE_DWORD_COUNTER_STAT(TEXT("Components"), STAT_SoftbodyComponents, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Running"), STAT_SoftbodyFieldsRunning, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Stopped"), STAT_SoftbodyFieldsStopped, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Small Or Off Screen"), STAT_SoftbodyFieldsSmall, STATGROUP_Softbody);
// 콜라이더 후보는 있었지만 닿을 수 있는 자리에 없어 하나도 올리지 않은 표면 장(ADR-0020 잠들기)
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Nothing In Reach"), STAT_SoftbodyFieldsNothingInReach, STATGROUP_Softbody);
// 갱신률 LOD(ADR-0021): 깨어 있는 것 가운데 이번 프레임 GPU 갱신을 돌린 수와 미룬 수.
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Field GPU Updates"), STAT_SoftbodyFieldUpdates, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Deferred (Rate LOD)"), STAT_SoftbodyFieldsDeferred, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Still (Pressed, No Change)"), STAT_SoftbodyFieldsStill, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Pressed (GPU)"), STAT_SoftbodyFieldsPressed, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Surface Fields Resting (GPU)"), STAT_SoftbodyFieldsResting, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Dent Tiles 16x16 (GPU)"), STAT_SoftbodyDentTiles, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drag Sources (GPU)"), STAT_SoftbodyDragSources, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drag Groups In Reach 8x8 (GPU)"), STAT_SoftbodyDragGroups, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Contact Groups 8x8 (GPU)"), STAT_SoftbodyContactGroups, STATGROUP_Softbody);
// 끌림 걸음 집계(일지 189): 묶음 원천 수, 그 가운데 뭉치지 못하는 섞인 묶음 수, 바로잡기·모으기 패스에서 8×8 묶음 하나가 차례로 도는 걸음의 최대(모든
// 표면 장 가운데 가장 큰 값 — 패스 시간은 가장 긴 묶음이 정한다). 장마다 값은 콘솔 Softbody.Stats.
DECLARE_DWORD_COUNTER_STAT(TEXT("Drag Clusters (GPU)"), STAT_SoftbodyDragClusters, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drag Mixed Clusters (GPU)"), STAT_SoftbodyDragMixedClusters, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drag Correct Steps Max (GPU)"), STAT_SoftbodyDragCorrectSteps, STATGROUP_Softbody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Drag Gather Steps Max (GPU)"), STAT_SoftbodyDragGatherSteps, STATGROUP_Softbody);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Surface Field Textures (MB)"), STAT_SoftbodyFieldMemory, STATGROUP_Softbody);

namespace
{
	// 개발(일지 240): 덩어리 평형 값을 로그로 — 새 GPU 풀이가 올 때마다 덩어리 묶음마다 한 줄.
	TAutoConsoleVariable<int32> CVarDebugLumps(
		TEXT("softbody.Debug.Lumps"), 0,
		TEXT("Development: 1 logs each lump's solved position every time a new GPU solution arrives (how far it moved off its inertia target, ")
		TEXT("the contact stiffness against the lump's own spring, and how many cm the softest point moved)."),
		ECVF_Default);

#if STATS
	/** stat softbody: 표면 장 GPU 시간을 프레임에 한 번 싣는다(여러 컴포넌트 중 처음 틱하는 것이). */
	void PublishSoftbodyGpuTimes()
	{
		static uint64 PublishedFrame = MAX_uint64;
		if (PublishedFrame == GFrameCounter)
		{
			return;
		}
		PublishedFrame = GFrameCounter;
		using EGpuTime = USoftbodySurfaceField::EGpuTime;
		double Ms[(int32)EGpuTime::Num];
		USoftbodySurfaceField::GetGpuTimes(Ms);
		double Total = 0.0;
		for (const double Value : Ms)
		{
			Total += Value;
		}
		SET_FLOAT_STAT(STAT_SoftbodyGpuTotal, Total);
		SET_FLOAT_STAT(STAT_SoftbodyGpuContact, Ms[(int32)EGpuTime::Contact]);
		SET_FLOAT_STAT(STAT_SoftbodyGpuTiles, Ms[(int32)EGpuTime::Tiles]);
		SET_FLOAT_STAT(STAT_SoftbodyGpuDent, Ms[(int32)EGpuTime::Dent]);
		SET_FLOAT_STAT(STAT_SoftbodyGpuDrag, Ms[(int32)EGpuTime::Drag]);
		SET_FLOAT_STAT(STAT_SoftbodyGpuPressure, Ms[(int32)EGpuTime::Pressure]);
		SET_FLOAT_STAT(STAT_SoftbodyGpuCompose, Ms[(int32)EGpuTime::Compose]);
		SET_FLOAT_STAT(STAT_SoftbodyGpuLump, Ms[(int32)EGpuTime::Lump]);
	}

	/** stat softbody: 끌림 걸음 최대를 이 프레임의 모든 표면 장 가운데 가장 큰 값으로 싣는다(카운터는 프레임마다 0으로 돌아가 SET으로 쓴다). */
	void PublishDragStepsMax(uint32 CorrectSteps, uint32 GatherSteps)
	{
		static uint64 Frame = MAX_uint64;
		static uint32 Correct = 0;
		static uint32 Gather = 0;
		if (Frame != GFrameCounter)
		{
			Frame = GFrameCounter;
			Correct = 0;
			Gather = 0;
		}
		Correct = FMath::Max(Correct, CorrectSteps);
		Gather = FMath::Max(Gather, GatherSteps);
		SET_DWORD_STAT(STAT_SoftbodyDragCorrectSteps, Correct);
		SET_DWORD_STAT(STAT_SoftbodyDragGatherSteps, Gather);
	}
#endif
}

namespace
{
	/** 끌림(ADR-0008)이 콜라이더의 지난 프레임 위치를 찾는 열쇠: (컴포넌트, 바디/요소 또는 부위 번호). */
	using FSoftbodyMotionKey = TPair<FObjectKey, int32>;
	using FSoftbodyMotionFrames = TMap<FSoftbodyMotionKey, FMatrix>;

	/**
	 * 끌림: 지금 프레임 점 → 같은 콜라이더에 붙은 점의 지난 프레임 위치 행렬(행 벡터 규약)의 열 0~2를 싣는다.
	 * FrameFromLocal은 콜라이더 로컬 → 표면 장 프레임. 지난 프레임에 없던 콜라이더는 제자리(단위 행렬)다.
	 */
	void AppendMotion(const FMatrix& FrameFromLocal, const FMatrix* PreviousFrameFromLocal, TArray<FVector4f>& Out)
	{
		const FMatrix Motion = PreviousFrameFromLocal ? FrameFromLocal.Inverse() * *PreviousFrameFromLocal : FMatrix::Identity;
		for (int32 Column = 0; Column < 3; ++Column)
		{
			Out.Add(FVector4f((float)Motion.M[0][Column], (float)Motion.M[1][Column], (float)Motion.M[2][Column],
				(float)Motion.M[3][Column]));
		}
	}

	/**
	 * 끌림에 쓸 지난 프레임 콜라이더 위치: Previous에서 찾아 싣고, 이번 위치를 Current에 적는다. 들어온 면 표지(일지 313)의 고정 번호도 같은
	 * 차례로 Ids에 싣는다 — 지난 갱신에 없던 부위는 NewCollider 비트(지난 자세를 모른다, 처음 겹침 규칙). bWholeCollider면 번호는 컴포넌트 하나에
	 * 하나다(바깥 콜라이더: 장갑 손의 거리장 부위들·피직스 바디들이 한 손 — 손가락 부위가 지난 점의 기둥에 더 깊이 든 손바닥도 같은 손으로 따라간다).
	 * 자기 몸 부위는 부위마다(몸 전체를 한 번호로 묶으면 가슴을 지난 제 손가락 표지로 기둥 깊이의 다른 부위까지 따라간다).
	 */
	struct FSoftbodyMotionTracker
	{
		const FSoftbodyMotionFrames& Previous;
		FSoftbodyMotionFrames& Current;
		TArray<FVector4f>& Out;
		TArray<uint32>& Ids;
		FSoftbodyColliderLabelIds& LabelIds;
		bool bWholeCollider;

		void Add(const FSoftbodyMotionKey& Key, const FMatrix& FrameFromLocal)
		{
			const FMatrix* PreviousFrame = Previous.Find(Key);
			AppendMotion(FrameFromLocal, PreviousFrame, Out);
			Current.Add(Key, FrameFromLocal);
			const FSoftbodyMotionKey LabelKey = bWholeCollider ? FSoftbodyMotionKey(Key.Key, INDEX_NONE) : Key;
			Ids.Add(LabelIds.Get(LabelKey) | (PreviousFrame ? 0u : FSoftbodyColliderLabelIds::NewCollider));
		}
	};


	/** 잠들기(ADR-0020): 콜라이더가 본 상자에서 이 거리(cm) 안이면 닿을 수 있다고 본다 — 본 상자 사이에 놓이는 스킨 웨이트 섞인 칸과 끌린 피부의 몫. */
	constexpr float WakeMargin = 2.f;

	/** 기본 포즈 메시 로컬 상자를 표면 장 프레임(컴포넌트 회전만 뺀 cm 공간 = 컴포넌트 공간 × 스케일)으로 옮기고 Margin만큼 키운다. */
	FBox WakeBoxToFrame(const FBox3f& Local, const FMatrix44f* Skinning, const FVector3f& Scale, float Margin)
	{
		FBox Out(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			FVector3f P((Corner & 1) ? Local.Max.X : Local.Min.X, (Corner & 2) ? Local.Max.Y : Local.Min.Y,
				(Corner & 4) ? Local.Max.Z : Local.Min.Z);
			if (Skinning)
			{
				const FVector4f Skinned = Skinning->TransformPosition(P);
				P = FVector3f(Skinned.X, Skinned.Y, Skinned.Z);
			}
			Out += FVector(P * Scale);
		}
		return Out.ExpandBy(Margin);
	}

	/** 콜라이더 형상의 월드 상자를 표면 장 프레임으로. 상자가 없으면(컨벡스를 채우기 전) 비어 있다. */
	FBox ShapeFrameBox(const FSoftbodyColliderShape& Shape, const FQuat& InverseRotation, const FVector& Origin)
	{
		FBox Out(ForceInit);
		if (!Shape.WorldBounds.IsValid)
		{
			return Out;
		}
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector P((Corner & 1) ? Shape.WorldBounds.Max.X : Shape.WorldBounds.Min.X,
				(Corner & 2) ? Shape.WorldBounds.Max.Y : Shape.WorldBounds.Min.Y,
				(Corner & 4) ? Shape.WorldBounds.Max.Z : Shape.WorldBounds.Min.Z);
			Out += InverseRotation.RotateVector(P - Origin);
		}
		return Out;
	}

	/** 거리장 부위의 격자 상자(한 칸 여유)를 표면 장 프레임으로. FrameFromMesh = 부위 메시 로컬 → 프레임(행 벡터 규약). */
	FBox PartFrameBox(const FSoftbodyColliderFieldPart& Part, const FMatrix& FrameFromMesh)
	{
		const FVector Lo = FVector(Part.Origin) - FVector((double)Part.VoxelSize);
		const FVector Hi = FVector(Part.Origin) + FVector((double)Part.Dims.X, (double)Part.Dims.Y, (double)Part.Dims.Z) * (double)Part.VoxelSize;
		FBox Out(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector P((Corner & 1) ? Hi.X : Lo.X, (Corner & 2) ? Hi.Y : Lo.Y, (Corner & 4) ? Hi.Z : Lo.Z);
			Out += FVector(FrameFromMesh.TransformPosition(P));
		}
		return Out;
	}
}

/**
 * 잠들기(ADR-0020): 이번 프레임 몸의 본마다 "그 본이 움직이는 살이 콜라이더를 느낄 수 있는 자리"(베이크한 상자를 지금 자세로 옮긴 것, 표면 장
 * 프레임 cm). 콜라이더·손 부위의 상자가 어느 본의 상자와도 겹치지 않으면 이번 프레임 누를 수 없으므로 GPU에 올리지 않는다. 깨우는 판단을
 * CPU가 틱마다 하므로 닿는 순간 같은 프레임에 깨고, 그 사이 GPU 계산은 "콜라이더 없음"과 같아 복원 뒤 멈춘다.
 */
struct FSoftbodyWakeBoxes
{
	/** false = 베이크에 상자가 없거나(옛 캐리어) 자세를 아직 모른다 — 거르지 않는다. */
	bool bEnabled = false;
	const USoftbodyMeshData* Data = nullptr;
	TConstArrayView<FMatrix44f> Skinning;
	FVector3f Scale = FVector3f::OneVector;
	float Margin = 0.f;
	/** 본마다 프레임 상자(칸이 없는 본은 비어 있다). 스태틱은 [0]. */
	TArray<FBox> Bones;
	FBox All = FBox(ForceInit);
	/** 자기 접촉 부위 상자를 쓸 수 있는가(베이크 때 부위와 지금 거리장 부위가 같다). */
	bool bSelfValid = false;

	void Build(const USoftbodyMeshData* InData, bool bSkeletal, TConstArrayView<FMatrix44f> InSkinning, const FVector3f& InScale,
		float InMargin, const USoftbodyColliderField* SelfField)
	{
		Data = InData;
		Skinning = InSkinning;
		Scale = InScale;
		Margin = InMargin;
		bEnabled = Data && Data->CarrierWakeBounds.Num() > 0
			&& (bSkeletal ? Skinning.Num() >= Data->CarrierWakeBounds.Num() : Data->CarrierWakeBounds.Num() == 1);
		if (!bEnabled)
		{
			return;
		}
		Bones.SetNum(Data->CarrierWakeBounds.Num());
		for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
		{
			const FBox3f& Local = Data->CarrierWakeBounds[Bone];
			Bones[Bone] = Local.IsValid ? WakeBoxToFrame(Local, bSkeletal ? &Skinning[Bone] : nullptr, Scale, Margin) : FBox(ForceInit);
			All += Bones[Bone];
		}
		// 베이크한 자기 접촉 부위 = 거리장에서 제 몸도 누르는 부위를 순서대로(ADR-0004 §11). 옛 거리장은 부위가 모두 그렇다.
		bSelfValid = SelfField != nullptr;
		int32 Slot = 0;
		for (int32 Part = 0; bSelfValid && Part < SelfField->Parts.Num(); ++Part)
		{
			if (SelfField->Parts[Part].bPressesSelf)
			{
				bSelfValid = Data->CarrierWakeSelfParts.IsValidIndex(Slot) && Data->CarrierWakeSelfParts[Slot] == SelfField->Parts[Part].Bone;
				++Slot;
			}
		}
		bSelfValid = bSelfValid && Data->CarrierWakeSelfParts.Num() == Slot && Data->CarrierWakeSelfBounds.Num() == Slot * Bones.Num();
	}

	/** 외부 콜라이더(형상·거리장 부위)가 어느 본의 살에든 닿을 수 있는가. */
	bool Reaches(const FBox& FrameBox) const
	{
		if (!bEnabled)
		{
			return true;
		}
		if (!All.IsValid || !All.Intersect(FrameBox))
		{
			return false;
		}
		for (const FBox& Bone : Bones)
		{
			if (Bone.IsValid && Bone.Intersect(FrameBox))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Slot번째 자기 접촉 부위(거리장에서 제 몸도 누르는 부위만 센 순서)가 누를 수 있는 살(기본 포즈에서 붙어 있던 살은 뺀)에 닿을 수
	 * 있는가. 부위 상자가 없으면 Reaches와 같다.
	 */
	bool SelfReaches(int32 Slot, const FBox& FrameBox) const
	{
		if (!bEnabled)
		{
			return true;
		}
		if (!bSelfValid)
		{
			return Reaches(FrameBox);
		}
		if (!All.IsValid || !All.Intersect(FrameBox))
		{
			return false;
		}
		const bool bSkeletal = Skinning.Num() > 0;
		for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
		{
			if (!Bones[Bone].IsValid || !Bones[Bone].Intersect(FrameBox))
			{
				continue;
			}
			const FBox3f& Local = Data->CarrierWakeSelfBounds[Slot * Bones.Num() + Bone];
			if (Local.IsValid && WakeBoxToFrame(Local, bSkeletal ? &Skinning[Bone] : nullptr, Scale, Margin).Intersect(FrameBox))
			{
				return true;
			}
		}
		return false;
	}
};

namespace
{
	/**
	 * 모양 거리장 부위 하나를 FieldInstances 규약(SoftbodySurfaceField.h)으로 싣는다. FrameFromMesh는 부위 메시 로컬 →
	 * 표면 장 프레임 행렬(행 벡터 규약), SelfPart는 자기 몸 부위 번호(외부 콜라이더는 -1 — 셰이더는 부호로 자기 몸인지 본다).
	 * FleshHalfExtent는 부위 살 상자의 반폭(USoftbodyColliderField::GetPartFleshHalfExtent). Key로 끌림의 지난 위치를 찾는다.
	 */
	void PackFieldInstance(const FSoftbodyColliderFieldPart& Part, const FVector3f& FleshHalfExtent, int32 VoxelBase,
		const FMatrix& FrameFromMesh, float SelfPart, TArray<FVector4f>& Out, FSoftbodyMotionTracker& Motion, const FSoftbodyMotionKey& Key)
	{
		Motion.Add(Key, FrameFromMesh);
		const FMatrix MeshFromFrame = FrameFromMesh.Inverse();
		for (int32 Column = 0; Column < 3; ++Column)
		{
			Out.Add(FVector4f((float)MeshFromFrame.M[0][Column], (float)MeshFromFrame.M[1][Column],
				(float)MeshFromFrame.M[2][Column], (float)MeshFromFrame.M[3][Column]));
		}
		Out.Add(FVector4f(Part.Origin, Part.VoxelSize));
		Out.Add(FVector4f((float)Part.Dims.X, (float)Part.Dims.Y, (float)Part.Dims.Z, (float)(VoxelBase + Part.FirstVoxel)));
		const double Scale = (FrameFromMesh.GetScaledAxis(EAxis::X).Size() + FrameFromMesh.GetScaledAxis(EAxis::Y).Size()
			+ FrameFromMesh.GetScaledAxis(EAxis::Z).Size()) / 3.0;
		Out.Add(FVector4f((float)Scale, SelfPart, 0.f, 0.f));
		Out.Add(FVector4f(FleshHalfExtent, 0.f));
	}

	/**
	 * 외부 콜라이더 하나의 거리장 부위를 싣는다. 스켈레탈은 부위마다 그 본의 기본 포즈 → 현재 행렬을 따른다. 잠들기 상자(Wake)에
	 * 닿을 수 없는 부위는 싣지 않고 OutGated에 센다. 실은 부위 수를 돌려주고, 거리장이 없거나 MaxInstances를 넘으면 -1(아무것도 싣지 않는다).
	 */
	int32 AppendColliderFieldInstances(USoftbodySurfaceField& Surface, const UPrimitiveComponent* Collider,
		const FMatrix& WorldToFrame, int32 MaxInstances, TArray<FVector4f>& Out, FSoftbodyMotionTracker& Motion,
		const FSoftbodyWakeBoxes& Wake, int32& OutGated)
	{
		const USoftbodyColliderField* Field = USoftbodyColliderField::FindOnPrimitive(Collider);
		if (!Field || Out.Num() / SoftbodySurfaceFieldRows::PerInstance + Field->Parts.Num() > MaxInstances)
		{
			return INDEX_NONE;
		}
		const int32 VoxelBase = Surface.RegisterColliderField(Field);
		const FMatrix WorldFromComponent = Collider->GetComponentTransform().ToMatrixWithScale();
		const FObjectKey ColliderKey(Collider);
		const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Collider);
		TArray<FMatrix44f> RefToLocal;
		if (Skinned)
		{
			Skinned->CacheRefToLocalMatrices(RefToLocal);
		}
		int32 Added = 0;
		for (int32 PartIndex = 0; PartIndex < Field->Parts.Num(); ++PartIndex)
		{
			const FSoftbodyColliderFieldPart& Part = Field->Parts[PartIndex];
			FMatrix FrameFromMesh = WorldFromComponent * WorldToFrame;
			if (Skinned)
			{
				const int32 Bone = Skinned->GetBoneIndex(Part.Bone);
				if (!RefToLocal.IsValidIndex(Bone))
				{
					continue;
				}
				FrameFromMesh = FMatrix(RefToLocal[Bone]) * FrameFromMesh;
			}
			if (!Wake.Reaches(PartFrameBox(Part, FrameFromMesh)))
			{
				++OutGated;
				continue;
			}
			PackFieldInstance(Part, Field->GetPartFleshHalfExtent(PartIndex), VoxelBase, FrameFromMesh, -1.f, Out, Motion,
				FSoftbodyMotionKey(ColliderKey, PartIndex));
			++Added;
		}
		return Added;
	}

	// MF_SoftbodyKernel / MF_SoftbodyDebugVis 파라미터 이름 규약 (Tools/softbody_material.py와 일치)
	const FName Param_ContactData(TEXT("ContactData"));
	// 지난 프레임 값(모션 벡터용): 머티리얼이 지난 프레임 변위를 구해 속도 버퍼에 쓴다.
	const FName Param_PrevContactData(TEXT("PrevContactData"));
	const FName Param_PrevSurfaceDispTex(TEXT("PrevSurfaceDispTex"));
	const FName Param_PrevL2W[3] = {FName(TEXT("PrevL2WX")), FName(TEXT("PrevL2WY")), FName(TEXT("PrevL2WZ"))};
	const FName Param_AttrDbgTex(TEXT("AttrDbgTex"));
	const FName Param_Attr2Tex(TEXT("Attr2Tex"));
	const FName Param_PaintTex(TEXT("PaintTex"));
	const FName Param_InertiaResponseTex(TEXT("InertiaResponseTex"));
	const FName Param_InertiaResponseCount(TEXT("InertiaResponseCount"));
	const FName Param_L2WX(TEXT("L2WX"));
	const FName Param_L2WY(TEXT("L2WY"));
	const FName Param_L2WZ(TEXT("L2WZ"));
	const FName Param_MaxDisplacement(TEXT("MaxDisplacement"));
	const FName Param_VisWeight(TEXT("VisWeight"));
	const FName Param_VisMode(TEXT("VisMode"));
	// ADR-0003 표면 장 (v5). SurfaceFieldOn = 1이면 머티리얼이 표면 장 변위·음영을 읽는다(캐리어가 없으면 0 = 접촉 꺼짐). Full이면 +0.25(PushMaterialParams).
	const FName Param_SurfaceFieldOn(TEXT("SurfaceFieldOn"));
	const FName Param_CarrierUVTex(TEXT("CarrierUVTex"));
	const FName Param_SurfaceDispTex(TEXT("SurfaceDispTex"));
	const FName Param_SurfaceNormalTex(TEXT("SurfaceNormalTex"));
	// 덩어리 이동이 기울인 노멀(ADR-0018). SurfaceFieldOn = 2일 때만 머티리얼이 읽는다.
	const FName Param_SurfaceLumpNormalTex(TEXT("SurfaceLumpNormalTex"));
	const FName Param_SurfaceAttachRange(TEXT("SurfaceAttachRange"));
	// ADR-0006 정점 사이 함몰: 나나이트 Displacement 값을 머티리얼의 변위 크기·중심에 맞추는 값
	const FName Param_TessMagnitude(TEXT("SoftbodyTessMagnitude"));
	const FName Param_TessCenter(TEXT("SoftbodyTessCenter"));

	const FName Tag_Ignore(TEXT("SoftbodyIgnore"));

	constexpr int32 NumDebugVisModes = 6;
	/** 표면 장이 한 프레임에 읽는 콜라이더 형상 상한. */
	constexpr int32 MaxSurfaceShapes = 32;
	/** 표면 장이 한 프레임에 읽는 컨벡스 평면 총수 상한. */
	constexpr int32 MaxSurfacePlanes = 2048;
	/** 표면 장이 한 프레임에 읽는 모양 거리장 부위(외부 콜라이더 + 자기 부위) 상한. */
	constexpr int32 MaxSurfaceFieldInstances = 512;

	/**
	 * 머티리얼의 텍스처 파라미터를 잇거나(Texture 있음) 푼다(없음). 엔진은 빈 텍스처를 받으면 아무 일도 하지 않고 지난 텍스처를
	 * 그대로 붙든다(UMaterialInstance::SetTextureParameterValueInternal). 그래서 풀 때는 부모 머티리얼의 값을 다시 넣는다.
	 */
	void SetOrClearTexture(UMaterialInstanceDynamic* MID, FName Name, UTexture* Texture)
	{
		if (!Texture && MID->Parent)
		{
			MID->Parent->GetTextureParameterValue(FHashedMaterialParameterInfo(Name), Texture);
		}
		MID->SetTextureParameterValue(Name, Texture);
	}

	void UploadFloat16Texture(UTexture2D* Texture, const TArray<FFloat16Color>& Data,
		int32 Width, int32 Height)
	{
		check(Data.Num() == Width * Height);
		const uint32 Pitch = Width * sizeof(FFloat16Color);
		uint8* Src = (uint8*)FMemory::Malloc(Data.Num() * sizeof(FFloat16Color));
		FMemory::Memcpy(Src, Data.GetData(), Data.Num() * sizeof(FFloat16Color));
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
		Texture->UpdateTextureRegions(0, 1, Region, Pitch, sizeof(FFloat16Color), Src,
			[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
			{
				FMemory::Free(SrcData);
				delete Regions;
			});
	}
}

USoftbodyComponent::USoftbodyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 켜기·끄기(ADR-0020 §3.6): 틱은 활성화(Activate)가 켠다. Auto Activate가 기본 켬이라 보통은 게임 시작부터 돌고, 끈 컴포넌트는 Activate 전까지
	// 아무것도 하지 않는다. 전에는 Auto Activate가 꺼진 채 틱만 돌아 엔진의 Deactivate가 아무 일도 하지 않았다(활성 상태일 때만 틱을 끈다).
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bAutoActivate = true;
	// 피직스로 시뮬레이트하는 본(가슴 등)은 물리 단계가 끝나야 최종 자세가 된다. 그 전에 돌면 애니메이션 자세의 피부에
	// 대고 함몰을 재서, 피직스가 밀어낸 살과 함몰이 어긋난다.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

UMeshComponent* USoftbodyComponent::ResolveTargetMeshComponent() const
{
	// 고른 메시가 지워졌으면(블루프린트 액터가 컴포넌트를 새로 만들면 옛 컴포넌트를 가리킨 채 남는다) 고르지 않은 것으로 본다.
	if (IsValid(TargetMeshComponent))
	{
		return TargetMeshComponent;
	}

	TInlineComponentArray<UMeshComponent*> Candidates(GetOwner());
	Candidates.RemoveAll([](const UMeshComponent* Candidate)
	{
		return !Candidate || Candidate->ComponentHasTag(Tag_Ignore);
	});

	if (MeshData)
	{
		for (UMeshComponent* Candidate : Candidates)
		{
			if (const UStaticMeshComponent* Static =
				Cast<UStaticMeshComponent>(Candidate))
			{
				if (MeshData->SourceMesh
					&& Static->GetStaticMesh() == MeshData->SourceMesh)
				{
					return Candidate;
				}
			}
			else if (const USkeletalMeshComponent* Skeletal =
				Cast<USkeletalMeshComponent>(Candidate))
			{
				if (MeshData->SourceSkeletalMesh
					&& Skeletal->GetSkeletalMeshAsset()
						== MeshData->SourceSkeletalMesh)
				{
					return Candidate;
				}
			}
		}
	}

	return Candidates.Num() > 0 ? Candidates[0] : nullptr;
}

FTransform USoftbodyComponent::GetTargetTransform() const
{
	if (const UMeshComponent* Target = SetupTarget.Get())
	{
		return Target->GetComponentTransform();
	}
	return GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity;
}

bool USoftbodyComponent::IsMaterialCompatible(
	const UMaterialInterface* Material) const
{
	if (!Material)
	{
		return false;
	}

	// MF_SoftbodyKernel의 고유 텍스처 파라미터 세 개가 모두 있어야 현재 커널이 연결된 것이다.
	// SurfaceDispTex가 없으면 표면 장(v5) 전의 호스트이므로 Tools/rebuild_softbody_functions.py가 필요하다.
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Material->GetAllTextureParameterInfo(Infos, Ids);
	bool bContactData = false;
	bool bInertiaResponse = false;
	bool bSurfaceDisp = false;
	for (const FMaterialParameterInfo& Info : Infos)
	{
		bContactData |= Info.Name == Param_ContactData;
		bInertiaResponse |= Info.Name == Param_InertiaResponseTex;
		bSurfaceDisp |= Info.Name == Param_SurfaceDispTex;
	}
	return bContactData && bInertiaResponse && bSurfaceDisp;
}

bool USoftbodyComponent::ValidateSetup() const
{
	return ValidateSetupInternal(true);
}

// Details 패널은 인자·반환값이 없는 CallInEditor 함수만 버튼으로 만든다(엔진 규칙) — bool을 돌려주는 ValidateSetup은 버튼이 되지 않는다.
void USoftbodyComponent::ValidateSetupButton() const
{
	ValidateSetupInternal(true);
}

bool USoftbodyComponent::ValidateSetupInternal(bool bAllUnusedValues) const
{
	bool bValid = true;
	UMeshComponent* Target = ResolveTargetMeshComponent();
	if (!Target)
	{
		UE_LOG(LogSoftbodyRuntime, Error,
			TEXT("%s: no target mesh component. Put the component on an actor with a skeletal or static mesh, or set Target Mesh Component."),
			*GetPathName());
		return false;
	}

	if (!MeshData)
	{
		UE_LOG(LogSoftbodyRuntime, Error,
			TEXT("%s: MeshData is not assigned. Set Mesh Data to the Baked Data of the mesh's Softbody Authoring Data."), *GetPathName());
		bValid = false;
	}
	else
	{
		bool bSourceMatches = false;
		if (const UStaticMeshComponent* Static =
			Cast<UStaticMeshComponent>(Target))
		{
			bSourceMatches = MeshData->SourceMesh
				&& Static->GetStaticMesh() == MeshData->SourceMesh;
		}
		else if (const USkeletalMeshComponent* Skeletal =
			Cast<USkeletalMeshComponent>(Target))
		{
			bSourceMatches = MeshData->SourceSkeletalMesh
				&& Skeletal->GetSkeletalMeshAsset()
					== MeshData->SourceSkeletalMesh;
		}
		if (!bSourceMatches)
		{
			UE_LOG(LogSoftbodyRuntime, Error,
				TEXT("%s: target mesh does not match MeshData source (%s). Set Mesh Data to this mesh's Baked Data, or bake this mesh."),
				*GetPathName(), *MeshData->GetPathName());
			bValid = false;
		}
	}

	int32 CompatibleSlots = 0;
	for (int32 Slot = 0; Slot < Target->GetNumMaterials(); ++Slot)
	{
		const UMaterialInterface* Material = Target->GetMaterial(Slot);
		if (!IsMaterialCompatible(Material))
		{
			continue;
		}
		++CompatibleSlots;
		// 테셀레이션 높이는 saturate(높이 / 크기 + 중심)이라 중심 0이면 정점 사이 함몰이 모두 잘린다. 저작 에셋의 Nanite 단계는 0.75를 넣지만
		// 머티리얼을 손으로 세팅하면 엔진 기본값 0이 남을 수 있다.
		if (Material->IsTessellationEnabled())
		{
			const FDisplacementScaling Scaling = Material->GetDisplacementScaling();
			if (Scaling.Magnitude > 1e-4f && Scaling.Center <= 0.f)
			{
				UE_LOG(LogSoftbodyRuntime, Warning,
					TEXT("%s: material %s has Nanite tessellation with Displacement Center 0, so the dents between vertices are cut off. "
						"Set Center (material Details > Nanite > Displacement) to about 0.75, or set Nanite Displacement Center in the "
						"Softbody Authoring Data and click Reapply Nanite Tessellation."),
					*GetPathName(), *Material->GetName());
			}
		}
	}
	if (CompatibleSlots == 0)
	{
		UE_LOG(LogSoftbodyRuntime, Error,
			TEXT("%s: target %s has no material slot containing the Softbody "
				"material function parameters (ContactData, InertiaResponseTex, SurfaceDispTex). "
				"Put MF_SoftbodyKernel into the mesh material (press Recreate Material in its Softbody Authoring Data, or wire it into your own master material)."),
			*GetPathName(), *Target->GetPathName());
		bValid = false;
	}

	// 같은 메시에 Softbody 컴포넌트가 둘이면 슬롯의 같은 머티리얼 인스턴스에 서로 값을 덮어써 어느 쪽도 맞게 보이지 않는다.
	{
		TInlineComponentArray<USoftbodyComponent*> Softbodies(GetOwner());
		for (const USoftbodyComponent* Other : Softbodies)
		{
			if (Other != this && Other->ResolveTargetMeshComponent() == Target)
			{
				UE_LOG(LogSoftbodyRuntime, Error,
					TEXT("%s: %s also deforms %s. Both write the same material parameters, so neither shows correctly. Keep one Softbody component per mesh."),
					*GetPathName(), *Other->GetName(), *Target->GetName());
				bValid = false;
				break;
			}
		}
	}

	if (bEnableInertia && !bContactOnlyValidation
		&& !HasInertiaResponseBake())
	{
		UE_LOG(LogSoftbodyRuntime, Error,
			TEXT("%s: inertia is enabled but MeshData has no inertia softness bake. "
				"Press Run Bake in its Softbody Authoring Data."),
			*GetPathName());
		bValid = false;
	}
	else if (bEnableInertia && !bContactOnlyValidation
		&& MeshData->GetInertiaResponseRows() < USoftbodyMeshData::InertiaRowsWithShading)
	{
		UE_LOG(LogSoftbodyRuntime, Warning,
			TEXT("%s: Mesh Data %s was baked by an older version, so the flesh jiggles but its shading does not "
				"follow the motion. Press Reapply Paint or Run Bake in its Softbody Authoring Data."),
			*GetPathName(), *MeshData->GetName());
	}
	else if (bEnableInertia && !bContactOnlyValidation && MeshData->InertiaGroupSag.Num() == 0)
	{
		// 옛 눈금(칠한 살 가운데 가장 무른 점 = Inertia Amount)은 그 점이 메시마다 다른 부위라 같은 값이 메시마다 다르게 흔들린다.
		UE_LOG(LogSoftbodyRuntime, Warning,
			TEXT("%s: MeshData %s uses the old jiggle scale, so the same Inertia Amount jiggles differently on other meshes. "
				"Press Reapply Paint on its authoring asset."),
			*GetPathName(), *MeshData->GetName());
	}

	if (bContactOnlyValidation)
	{
		UE_LOG(LogSoftbodyRuntime, Display,
			TEXT("%s: Contact Only (Debug) is on, so the jiggle is off."),
			*GetPathName());
	}

	if (MeshData)
	{
		LogUnusedValues(Target, bAllUnusedValues);
	}

	if (bValid)
	{
		// 접촉 상태: 표면 장은 게임이 시작할 때 만들어지므로(BeginPlay) 에디터에서도 맞게 말하도록 만들어질 조건으로 가른다.
		const TCHAR* ContactStatus = !bEnableContact ? TEXT("off (Enable Contact)")
			: (!MeshData->HasSurfaceCarrier() ? TEXT("off (Mesh Data has no contact map)")
				: (!USoftbodySurfaceField::IsSupported() ? TEXT("off (GPU cannot run the contact pass)") : TEXT("surface field")));
		UE_LOG(LogSoftbodyRuntime, Display,
			TEXT("%s: setup valid (target=%s, compatible material slots=%d, contact=%s, inertia=%s)."),
			*GetPathName(), *Target->GetPathName(), CompatibleSlots, ContactStatus,
			bEnableInertia ? TEXT("enabled") : TEXT("disabled"));
	}
	return bValid;
}

void USoftbodyComponent::LogUnusedValues(const UMeshComponent* Target, bool bAll) const
{
	// 이 메시에서 아무 일도 하지 않는 값. 같은 컴포넌트의 다른 값 때문이면 디테일 패널이 흐리므로(EditCondition) 여기서는 메시 종류·구운 데이터·
	// 모양 거리장처럼 다른 오브젝트에 달린 것만 본다. 게임 시작 때(bAll 거짓)는 기본값에서 바꾼 값만 적는다 — 손대지 않은 기본값까지 소품마다 적으면 로그가 묻힌다.
	const USoftbodyComponent* Defaults = GetDefault<USoftbodyComponent>();
	const auto Wanted = [bAll](bool bChanged) { return bAll || bChanged; };
	const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Target);
	// 흔들림이 도는가: 흐리는 조건과 같은 값에 관성 굽기(없으면 위에서 오류를 이미 적었다).
	const bool bInertia = bEnableInertia && !bContactOnlyValidation && InertiaAmount > 0.f && HasInertiaResponseBake();
	const bool bLumps = MeshData->InertiaLumps.Num() > 0;
	const bool bDrag = ContactDetail == ESoftbodyContactDetail::Full && SurfaceDragRatio > 0.f;
	TArray<FString> Unused;
	if (!Skinned)
	{
		// 스태틱 메시는 메시 전체가 구동점 하나다: 본·애니메이션에 달린 값은 듣지 않는다.
		TArray<FString> Names;
		if (bEnableContact && bSelfContact && Wanted(bSelfContact != Defaults->bSelfContact))
		{
			Names.Add(TEXT("Self Contact"));
		}
		if (bInertia && InertiaGroupOverrides.Num() > 0)
		{
			Names.Add(TEXT("Inertia Group Overrides"));
		}
		if (bInertia && Wanted(AnimationInertiaWeight != Defaults->AnimationInertiaWeight))
		{
			Names.Add(TEXT("Animation Motion Weight"));
		}
		if (Names.Num() > 0)
		{
			Unused.Add(FString::Printf(TEXT("On a static mesh these settings have no effect: %s."), *FString::Join(Names, TEXT(", "))));
		}
	}
	else
	{
		// 자기 접촉은 메시 에셋에 구운 모양 거리장의 제 몸을 누르는 부위(Self Contact Bones)로만 누른다(UpdateSurfaceField·AppendSelfFieldInstances).
		if (bEnableContact && bSelfContact && Wanted(bSelfContact != Defaults->bSelfContact))
		{
			const USoftbodyColliderField* Field = USoftbodyColliderField::FindOnPrimitive(Skinned);
			const bool bSelfParts = Field && Field->Parts.ContainsByPredicate([](const FSoftbodyColliderFieldPart& Part) { return Part.bPressesSelf; });
			if (!bSelfParts)
			{
				Unused.Add(FString::Printf(TEXT("Self Contact has no effect because %s has no shape field with Self Contact Bones. In its Softbody ")
					TEXT("Authoring Data tick Bake Shape Field, set Self Contact Bones (for example hand_l and hand_r) and press Rebake Shape Field."),
					*GetNameSafe(Skinned->GetSkinnedAsset())));
			}
		}
		// 덩어리 아닌 부위(뼈대)는 처지지 않는다 — 그룹 덮어쓰기의 Gravity Scale로만(ResolveInertiaGroupGravityScale, 일지 316). 덩어리 표시가 없는 옛
		// 베이크와 관성 그룹이 없는 베이크는 모든 부위를 덩어리로 본다.
		bool bLumpGroup = MeshData->InertiaGroupBones.Num() == 0;
		for (int32 Group = 0; Group < MeshData->InertiaGroupBones.Num(); ++Group)
		{
			bLumpGroup |= !MeshData->InertiaGroupLump.IsValidIndex(Group) || MeshData->InertiaGroupLump[Group];
		}
		if (bInertia && !bLumpGroup && InertiaGravityScale > 0.f && Wanted(InertiaGravityScale != Defaults->InertiaGravityScale))
		{
			Unused.Add(TEXT("Gravity Scale has no effect because this mesh has no lumps and parts on the skeleton do not sag. Set breast or ")
				TEXT("buttock bones to Lump in the authoring asset's Bone Overrides and press Run Bake, or give a part its own Gravity Scale in ")
				TEXT("Inertia Group Overrides."));
		}
	}
	// 덩어리 셋: 이 컴포넌트 조건은 맞아 흐리지 않았는데 구운 덩어리가 없다. Squeeze Stiffening은 끌림이 돌면 덩어리 없이도 듣는다.
	// 스태틱 메시에는 덩어리가 없다(일지 296).
	if (bEnableContact && bInertia && LumpYield > 0.f && !bLumps)
	{
		const TCHAR* LumpWhy = Skinned
			? TEXT("Mesh Data has no lumps. Set breast or buttock bones to Lump in the authoring asset's Bone Overrides and press Run Bake.")
			: TEXT("a static mesh has no lumps. Only bones set to Lump on a skeletal mesh move aside as a whole.");
		if (Wanted(LumpYield != Defaults->LumpYield || LumpGrip != Defaults->LumpGrip))
		{
			Unused.Add(FString::Printf(TEXT("Lump Yield and Lump Grip have no effect because %s"), LumpWhy));
		}
		if (!bDrag && Wanted(LumpStiffen != Defaults->LumpStiffen))
		{
			Unused.Add(TEXT("Squeeze Stiffening has no effect because Mesh Data has no lumps and skin drag is off (it needs Contact Detail ")
				TEXT("Full and Drag Friction above 0)."));
		}
	}
	// Flex(ADR-0038 §4): 칠한 칸의 굽힘과 강체 판은 Contact Detail Full의 압력 풀이에만 있다 — Basic이면 살처럼 눌린다.
	// Flex Offset은 칸 칠에 더해 0~1로 자른다(칠 안 한 살은 1) — 끝에 닿아 더 움직여도 같은 자리를 알린다.
	if (bEnableContact && MeshData->HasSurfaceCarrier())
	{
		const bool bPainted = MeshData->HasHardPieces() || !MeshData->CarrierSoftnessTex.IsNull();
		const float Darkest = bPainted ? FMath::Clamp(MeshData->CarrierFlexMin, 0.f, 1.f) : 1.f;
		// 칠한 Flex는 기본값(Basic)이어도 알린다 — 칠한 판·굽힘이 말없이 꺼지지 않게
		if (ContactDetail != ESoftbodyContactDetail::Full)
		{
			if (bPainted)
			{
				Unused.Add(TEXT("The Flex paint dents like bare skin because Contact Detail is Basic. Flex needs Contact Detail Full."));
			}
			else if (Wanted(FlexOffset != Defaults->FlexOffset))
			{
				Unused.Add(TEXT("Flex Offset has no effect because Contact Detail is Basic. Flex needs Contact Detail Full."));
			}
		}
		else if (FlexOffset > 0.f && !bPainted)
		{
			Unused.Add(TEXT("Flex Offset above 0 has no effect because nothing is painted in Flex. Unpainted skin is already white."));
		}
		else if (FlexOffset > 0.f && Darkest + FlexOffset >= 1.f)
		{
			Unused.Add(FString::Printf(TEXT("Flex Offset %+.2f lifts even the darkest Flex paint (%.2f) to white, so nothing bends stiffer or stays a plate."),
				FlexOffset, Darkest));
		}
		else if (FlexOffset < 0.f)
		{
			// 칠한 조각: 가장 큰 칠 + 값이 강체 문턱 칠(조각 크기 비, RigidFlexLimit) 밑이면 강체 판(셰이더 SB_HardKindOf와 같은 셈) — 이 값 때문에
			// 강체가 된 조각만 센다.
			int32 ToRigid = 0;
			for (const FSoftbodyHardPiece& Piece : MeshData->HardPieces)
			{
				const float Margin = Piece.MaxSoftness - SoftbodyContactResponse::RigidFlexLimit(Piece.Radius);
				ToRigid += !Piece.bFloating && Margin > 0.f && Margin + FlexOffset <= 0.f ? 1 : 0;
			}
			if (MeshData->HasHardPieces() && ToRigid > 0)
			{
				Unused.Add(FString::Printf(TEXT("Flex Offset %+.2f makes %d of %d painted pieces stiff enough for their size, so they move as rigid plates."),
					FlexOffset, ToRigid, MeshData->HardPieces.Num()));
			}
			if (FlexOffset <= -1.f)
			{
				Unused.Add(FString::Printf(TEXT("Flex Offset %+.2f takes unpainted skin to black, where bending is as stiff as it gets."), FlexOffset));
			}
			else if (bPainted && Darkest > 0.f && Darkest + FlexOffset <= 0.f)
			{
				Unused.Add(FString::Printf(TEXT("Flex Offset %+.2f takes the darkest Flex paint (%.2f) to black, where bending is as stiff as it gets."),
					FlexOffset, Darkest));
			}
		}
	}
	// Volume Offset: 칸의 v(저작 에셋 값 × 칠)에 더해 0~1로 자른다. 칠 안 한 자리(v = 저작 에셋 값)가 끝을 넘으면 거기서 멈춰 더 올리거나
	// 내려도 같다 — 조용히 멈추지 않게 적는다. Volume을 칠한 자리는 칠 값에 따라 더 늦게 닿는다.
	if (bEnableContact && MeshData->HasSurfaceCarrier() && VolumeOffset != 0.f)
	{
		const float White = FMath::Clamp(MeshData->VolumePreservation, 0.f, 1.f);
		if (White + VolumeOffset > 1.f)
		{
			// 칠 안 한 자리는 1에서 멈춘다. Volume 칠이 없는 메시면 값이 아무것도 바꾸지 않는다.
			Unused.Add(FString::Printf(TEXT("Volume Offset %+.2f: unpainted areas are at Volume %.2f and stop at 1 (flesh)%s"), VolumeOffset, White,
				MeshData->bPaintVolume ? TEXT(". Areas painted darker in Volume still change.") : TEXT(", so it has no effect on this mesh.")));
		}
		else if (White + VolumeOffset < 0.f)
		{
			Unused.Add(FString::Printf(TEXT("Volume Offset %+.2f: unpainted areas are at Volume %.2f, so they stop at 0 (foam)."),
				VolumeOffset, White));
		}
		else if (VolumeOffset < 0.f && MeshData->bPaintVolume)
		{
			Unused.Add(FString::Printf(TEXT("Volume Offset %+.2f: areas painted below %.2f in Volume stop at 0 (foam)."), VolumeOffset,
				-VolumeOffset / FMath::Max(White, 1e-3f)));
		}
	}
	for (const FString& Line : Unused)
	{
		UE_LOG(LogSoftbodyRuntime, Display, TEXT("%s: %s"), *GetPathName(), *Line);
	}
}

void USoftbodyComponent::BeginPlay()
{
	Super::BeginPlay();

	// 블루프린트 액터의 값을 실행 중에 디테일 패널에서 고치면 엔진이 그 액터의 컴포넌트를 전부 새로 만들고, 새 컴포넌트의 BeginPlay를
	// 인스턴스마다 준 값(MeshData, 메시 에셋, 머티리얼)을 돌려놓기 전에 부른다. 그때 세팅하면 빈 값으로 실패하므로 첫 틱으로 미룬다.
	if (GIsReconstructingBlueprintInstances)
	{
		bSetupPending = true;
		return;
	}
	SetupRuntime();
}

bool USoftbodyComponent::IsSetupStale() const
{
	if (bSetupPending || FObjectKey(MeshData) != SetupMeshData)
	{
		return true;
	}
	const UMeshComponent* Target = SetupTarget.Get();
	if (!Target)
	{
		// 세팅 때의 메시가 지워졌거나, 메시 없이 세팅했는데 그 뒤에 메시를 골라 줬다.
		return bSetupHadTarget || IsValid(TargetMeshComponent);
	}
	if (IsValid(TargetMeshComponent) && TargetMeshComponent != Target)
	{
		return true;
	}
	// 메시에 걸어 둔 머티리얼이 다른 것으로 바뀌었으면 값을 넣을 통로가 끊긴 것이다.
	for (int32 Index = 0; Index < MIDs.Num(); ++Index)
	{
		if (Target->GetMaterial(MIDSlots[Index]) != MIDs[Index])
		{
			return true;
		}
	}
	return false;
}

void USoftbodyComponent::SetupRuntime()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodySetupRuntime);
	bSetupPending = false;

	// 앞선 세팅이 남긴 것을 걷는다(처음이면 아무것도 없다).
	UMeshComponent* const PreviousTarget = SetupTarget.Get();
	if (UMeshComponent* Previous = PreviousTarget)
	{
		Previous->TransformUpdated.Remove(TargetTransformHandle);
		RemoveTickPrerequisiteComponent(Previous);
	}
	TargetTransformHandle.Reset();
	ReleaseSurfaceField();
	// 앞선 세팅이 끈 World Position Offset은 옛 메시에 되돌린다 — 이번 세팅이 끈 채로 끝나면(켜지지 않음) 새 메시에 다시 끈다.
	SetTargetWorldPositionOffset(true);

	// 액터 틱(데모 액터의 이동 등)이 끝난 뒤 갱신하도록 순서 고정
	if (AActor* Owner = GetOwner())
	{
		AddTickPrerequisiteActor(Owner);
	}

	ContactTexture = UTexture2D::CreateTransient(BridgeWidth, BridgeRows, PF_FloatRGBA);
	ContactTexture->SRGB = false;
	ContactTexture->Filter = TF_Nearest;
	ContactTexture->UpdateResource();
	PreviousContactTexture = UTexture2D::CreateTransient(BridgeWidth, BridgeRows, PF_FloatRGBA);
	PreviousContactTexture->SRGB = false;
	PreviousContactTexture->Filter = TF_Nearest;
	PreviousContactTexture->UpdateResource();
	LastContactData.Reset();
	bHasLastL2W = false;

	// 표면 장(v5): 접촉이 켜져 있고 캐리어가 있으면 GPU 상태를 만든다. 없으면 접촉이 꺼지고 관성만 돈다. GPU가 접촉을 돌릴 수 없어도
	// 꺼진다 — 그 까닭은 USoftbodySurfaceField::IsSupported가 한 번 남기므로 여기서는 캐리어가 없을 때만 알린다. 끈 컴포넌트는 만들지 않는다
	// (일지 222) — 켜는 순간 SyncSurfaceField가 만든다.
	bContactApplied = WantsSurfaceField();
	if (bContactApplied)
	{
		USoftbodySurfaceField* Field = NewObject<USoftbodySurfaceField>(this);
		if (Field->Initialize(MeshData, IsActive()))
		{
			SurfaceField = Field;
		}
		else if (USoftbodySurfaceField::IsSupported())
		{
			UE_LOG(LogSoftbodyRuntime, Warning,
				TEXT("%s: %s has no contact map (press Run Bake on its Softbody Authoring Data asset) — contact is off, inertia still runs."),
				*GetPathName(), MeshData ? *MeshData->GetPathName() : TEXT("<no MeshData>"));
		}
	}

	// 앞선 세팅의 머티리얼 인스턴스(표면 장 텍스처는 위 ReleaseSurfaceField가 이미 풀었다). 대상 메시가 바뀌면 아래에서 꺼진 값을 민다.
	const TArray<TObjectPtr<UMaterialInstanceDynamic>> PreviousMIDs = MoveTemp(MIDs);
	MIDs.Reset();
	MIDSlots.Reset();
	SkinnedTarget = nullptr;
	// 찾은 메시는 TargetMeshComponent(사용자가 고르는 값)에 적지 않고 따로 기억한다 — 적어 두면 저장되는 값이 되어, 블루프린트 액터가
	// 컴포넌트를 새로 만들 때 지워진 옛 메시를 가리킨 채 돌아온다.
	UMeshComponent* ResolvedTarget = ResolveTargetMeshComponent();
	SetupTarget = ResolvedTarget;
	bSetupHadTarget = ResolvedTarget != nullptr;
	// 대상 메시가 바뀌었다: 옛 메시의 머티리얼 인스턴스는 더 갱신되지 않으므로 정점 셰이더의 두 갈래(접촉·관성)를 끈다 — 안 그러면 옛 메시가
	// 마지막 관성·접촉 모양으로 굳는다. 같은 메시면 아래에서 같은 인스턴스를 다시 쓰므로 건드리지 않는다.
	if (PreviousTarget && PreviousTarget != ResolvedTarget)
	{
		for (UMaterialInstanceDynamic* MID : PreviousMIDs)
		{
			if (MID)
			{
				MID->SetScalarParameterValue(Param_SurfaceFieldOn, 0.f);
				MID->SetScalarParameterValue(Param_InertiaResponseCount, 0.f);
			}
		}
	}
	SetupMeshData = FObjectKey(MeshData);

	if (!ResolvedTarget)
	{
		UE_LOG(LogSoftbodyRuntime, Error,
			TEXT("Softbody %s: found no mesh component to deform. Put the component on an actor with a skeletal or static mesh, or set Target Mesh Component."), *GetName());
	}
	else
	{
		SkinnedTarget = Cast<USkinnedMeshComponent>(ResolvedTarget);
		// A skeletal mesh can finish parallel pose evaluation after this
		// component was otherwise eligible to tick. Waiting on the target's
		// completion handle keeps inertia and uploads on the finalized pose.
		AddTickPrerequisiteComponent(ResolvedTarget);
		// 순간이동(리스폰·컷 전환처럼 텔레포트로 표시한 이동)을 알아야 관성이 건너뛴 거리를 가속으로 읽지 않는다.
		TargetTransformHandle = ResolvedTarget->TransformUpdated.AddUObject(this, &USoftbodyComponent::OnTargetTransformUpdated);

		for (int32 SlotIdx = 0; SlotIdx < ResolvedTarget->GetNumMaterials(); ++SlotIdx)
		{
			if (!IsMaterialCompatible(ResolvedTarget->GetMaterial(SlotIdx)))
			{
				continue;
			}
			if (UMaterialInstanceDynamic* MID =
				ResolvedTarget->CreateAndSetMaterialInstanceDynamic(SlotIdx))
			{
				MID->SetTextureParameterValue(Param_ContactData, ContactTexture);
				MID->SetTextureParameterValue(Param_PrevContactData, PreviousContactTexture);
				// 베이크 속성은 메시별 데이터 — 공유 머티리얼에 MID로 주입. 데이터에 없는 텍스처는 머티리얼의 기본값으로 되돌린다
				// (페인트는 흰색 = 중립). 슬롯에 이미 있던 MID를 그대로 쓰므로, 앞선 세팅이 넣은 값이 남아 있을 수 있다.
				SetOrClearTexture(MID, Param_CarrierUVTex, MeshData ? MeshData->CarrierUVTex.Get() : nullptr);
				SetOrClearTexture(MID, Param_AttrDbgTex, MeshData ? MeshData->AttributeTexDebug.Get() : nullptr);
				SetOrClearTexture(MID, Param_Attr2Tex, MeshData ? MeshData->AttributeTex2.Get() : nullptr);
				SetOrClearTexture(MID, Param_PaintTex, MeshData ? MeshData->PaintTex.Get() : nullptr);
				SetOrClearTexture(MID, Param_InertiaResponseTex, MeshData ? MeshData->InertiaResponseTex.Get() : nullptr);
				// 좌표가 0인 동안에는 바인딩돼 있어도 정적 접촉에 관여하지 않는다.
				MID->SetScalarParameterValue(Param_InertiaResponseCount, 0.f);
				MIDs.Add(MID);
				MIDSlots.Add(SlotIdx);
			}
		}
	}
	// 이미 있던 MID가 앞선 표면 장의 렌더 타깃을 가리킬 수 있으므로 먼저 풀고, 이 표면 장이 텍스처를 잡고 있으면 잇는다.
	BindSurfaceFieldTextures(false);
	BindSurfaceFieldTextures(true);
	// 게임 시작 때는 아무 일도 하지 않는 값 가운데 기본값에서 바꾼 것만 알린다(Validate Setup 버튼은 전부).
	ValidateSetupInternal(false);

	ResetInertiaState();

	// 관성 그룹 해석: 베이크된 본 이름 → 스킨드 타겟 본 인덱스.
	// 살의 무름은 기본 포즈 컴포넌트 축으로 구워지므로 본의 기본 포즈 회전도 함께 저장한다.
	NumInertiaGroups = 0;
	if (MeshData && SkinnedTarget.IsValid() && MeshData->InertiaGroupBones.Num() > 0)
	{
		TArray<FTransform> RefComponentPose;
		const FReferenceSkeleton* RefSkeleton = nullptr;
		if (MeshData->SourceSkeletalMesh)
		{
			RefSkeleton = &MeshData->SourceSkeletalMesh->GetRefSkeleton();
			const TArray<FTransform>& RefLocalPose = RefSkeleton->GetRefBonePose();
			RefComponentPose.SetNum(RefSkeleton->GetNum());
			for (int32 Bone = 0; Bone < RefSkeleton->GetNum(); ++Bone)
			{
				const int32 Parent = RefSkeleton->GetParentIndex(Bone);
				RefComponentPose[Bone] = Parent >= 0
					? RefLocalPose[Bone] * RefComponentPose[Parent]
					: RefLocalPose[Bone];
			}
		}

		const int32 NumG = FMath::Min(MeshData->InertiaGroupBones.Num(), MaxInertiaGroups);
		int32 NumUnresolved = 0;
		for (int32 g = 0; g < NumG; ++g)
		{
			GroupBoneIndices[g] = SkinnedTarget->GetBoneIndex(MeshData->InertiaGroupBones[g]);
			NumUnresolved += GroupBoneIndices[g] == INDEX_NONE ? 1 : 0;
			GroupReferenceRotation[g] = FQuat::Identity;
			if (RefSkeleton)
			{
				const int32 RefBone =
					RefSkeleton->FindBoneIndex(MeshData->InertiaGroupBones[g]);
				if (RefComponentPose.IsValidIndex(RefBone))
				{
					GroupReferenceRotation[g] =
						RefComponentPose[RefBone].GetRotation().GetNormalized();
				}
			}
		}
		NumInertiaGroups = NumG;
		if (NumUnresolved > 0)
		{
			UE_LOG(LogSoftbodyRuntime, Warning,
				TEXT("Softbody: %d of %d inertia group bones were not found on the mesh's skeleton; those groups will not jiggle. Run Bake again if the skeleton changed."),
				NumUnresolved, NumG);
		}
	}
	ComplexOnlyLogged.Reset();

	// 켜지지 않은 채 세팅했으면(Auto Activate 끔) 틱이 없어 PushMaterialParams가 돌지 않는다 — 머티리얼이 꺼진 값을 보게 한 번 민다.
	if (!IsActive())
	{
		SuspendRuntime();
	}
}

int32 USoftbodyComponent::GatherPressureShapes(TArray<FSoftbodyColliderShape>& OutShapes,
	TArray<const UPrimitiveComponent*>* OutFieldColliders)
{
	OutShapes.Reset();
	if (OutFieldColliders)
	{
		OutFieldColliders->Reset();
	}
	UWorld* World = GetWorld();
	UMeshComponent* Target = SetupTarget.Get();
	AActor* Owner = GetOwner();
	if (!World || !Target || !Owner)
	{
		return 0;
	}

	// 타깃 바운드를 MaxContactDepth만큼 키운 박스로 채널 오버랩.
	const FBoxSphereBounds Bounds = Target->Bounds;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SoftbodyColliderOverlap), false, Owner);
	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(Overlaps, Bounds.Origin, FQuat::Identity, PressureChannel,
		FCollisionShape::MakeBox(Bounds.BoxExtent + FVector(FMath::Max(MaxContactDepth, 1.f))), QueryParams);

	TSet<const UPrimitiveComponent*> Seen;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		const UPrimitiveComponent* Prim = Overlap.GetComponent();
		if (!Prim || Prim->GetOwner() == Owner || Seen.Contains(Prim))
		{
			continue;
		}
		Seen.Add(Prim);
		if (Prim->ComponentHasTag(Tag_Ignore)
			|| (Prim->GetOwner() && Prim->GetOwner()->ActorHasTag(Tag_Ignore))
			|| !Prim->IsQueryCollisionEnabled())
		{
			continue;
		}
		if (OutFieldColliders && USoftbodyColliderField::FindOnPrimitive(Prim))
		{
			OutFieldColliders->Add(Prim);
			continue;
		}
		const int32 Added = SoftbodyColliderShapes::AppendShapesFromPrimitive(Prim, OutShapes);
		if (Added == 0 && !ComplexOnlyLogged.Contains(Prim))
		{
			ComplexOnlyLogged.Add(Prim);
			UE_LOG(LogSoftbodyRuntime, Warning,
				TEXT("%s: %s has no simple collision shapes and no baked shape field, so it cannot press. Add simple collision or a physics asset, or right-click the mesh and pick Softbody: Bake Shape Field."),
				*GetPathName(), *Prim->GetPathName());
		}
	}
	return Overlaps.Num();
}

bool USoftbodyComponent::IsSurfaceFieldMode() const
{
	return SurfaceField && SurfaceField->IsReady();
}

void USoftbodyComponent::BindSurfaceFieldTextures(bool bBind)
{
	// 표면 장의 렌더 타깃을 머티리얼에 잇거나(텍스처 한 벌을 잡았을 때) 푼다(접촉 끔·풀에 돌려줄 때). 풀면 머티리얼이 부모의 기본
	// 텍스처를 읽는다 — 돌려준 벌은 풀이 다른 인스턴스에 주거나 버리므로 이 머티리얼이 계속 가리키면 안 된다.
	// 텍스처를 아직 안 잡았으면(닿기 전) 잇지 않는다 — 잡는 순간 표면 장이 다시 부른다.
	UTextureRenderTarget2D* Displacement = bBind && SurfaceField ? SurfaceField->GetDisplacementTexture() : nullptr;
	UTextureRenderTarget2D* PreviousDisplacement = bBind && SurfaceField ? SurfaceField->GetPreviousDisplacementTexture() : nullptr;
	UTextureRenderTarget2D* Normal = bBind && SurfaceField ? SurfaceField->GetNormalTexture() : nullptr;
	UTextureRenderTarget2D* LumpNormal = bBind && SurfaceField ? SurfaceField->GetLumpNormalTexture() : nullptr;
	if (bBind && !Displacement)
	{
		return;
	}
	bPrevDispBoundToCurrent = false;
	for (UMaterialInstanceDynamic* MID : MIDs)
	{
		SetOrClearTexture(MID, Param_SurfaceDispTex, Displacement);
		SetOrClearTexture(MID, Param_PrevSurfaceDispTex, PreviousDisplacement);
		SetOrClearTexture(MID, Param_SurfaceNormalTex, Normal);
		if (!bBind || LumpNormal)
		{
			SetOrClearTexture(MID, Param_SurfaceLumpNormalTex, LumpNormal);
		}
	}
}

void USoftbodyComponent::OnUnregister()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyOnUnregister);
	// 표면 장이 쥔 텍스처를 월드 풀에 돌려준다(ADR-0022). 안 돌려주면 GC까지 붙들고 있다가 풀을 거치지 않고 사라진다.
	// 다시 등록되면(실행 중 메시를 바꿀 때 등) 켜져 있는 한 다음 틱의 SyncSurfaceField가 새로 만든다.
	ReleaseSurfaceField();
	bContactApplied = false;
	// 끈 채 지워지면(레벨 내리기·액터 지우기) 메시의 World Position Offset을 되돌려 둔다.
	SetTargetWorldPositionOffset(true);
	Super::OnUnregister();
}

#if WITH_EDITOR
bool USoftbodyComponent::KeepsRuntimeOnEdit(const FProperty* Property) const
{
	const UClass* Owner = Property ? Property->GetOwnerClass() : nullptr;
	const UWorld* World = GetWorld();
	return Owner && Owner->IsChildOf(USoftbodyComponent::StaticClass()) && World && World->IsGameWorld() && IsRegistered() && IsValid(this);
}

void USoftbodyComponent::PreEditChange(FEditPropertyChain& PropertyAboutToChange)
{
	// 구조체 안쪽 값(Inertia Direction Scale의 X, Inertia Group Overrides 항목의 값)을 고치면 FProperty 판에는 안쪽 값만 온다 — 이 컴포넌트의 값인지는
	// 멤버로 가려 넘긴다. UActorComponent는 사슬 판을 덮어쓰지 않으므로(FProperty 판만 덮어써 이름이 가려진다) UObject 판을 바로 부른다. 그 판이 안쪽 값으로
	// 아래 FProperty 판을 부른다.
	const auto* Member = PropertyAboutToChange.GetActiveMemberNode();
	TGuardValue<bool> OwnValue(bEditingOwnValue, Member && KeepsRuntimeOnEdit(Member->GetValue()));
	UObject::PreEditChange(PropertyAboutToChange);
}

void USoftbodyComponent::PreEditChange(FProperty* PropertyAboutToChange)
{
	// 실행 중(게임 월드) 이 컴포넌트의 값을 고치면 엔진의 다시 등록을 건너뛰고 고친다는 기록(UObject 판 = Modify)만 남긴다. 엔진 길(UActorComponent 판)은
	// 컴포넌트를 내렸다 올리고(OnUnregister가 표면 장을 놓고, OnRegister의 Activate(true)가 흔들림을 비운다), 블루프린트 액터는 PostEditChange에서 구성
	// 스크립트를 다시 돌려 컴포넌트를 새로 만든다 — 남은 자국·끌림·쥔 덩어리·흔들림이 값을 고칠 때마다 처음부터 시작했고, 그 프레임에 표면 장을 새로 만들었다.
	// 이 컴포넌트는 값을 틱마다 읽고, 다시 세팅이 필요한 값(Mesh Data·Target Mesh Component)은 IsSetupStale이, Enable Contact는 SyncSurfaceField가 맞추므로
	// 다시 등록이 할 일이 없다. 다시 등록하지 않으면 PostEditChange도 구성 스크립트를 다시 돌리지 않는다(ConsolidatedPostEditChange는 다시 등록한 것만 돌린다).
	if (bEditingOwnValue || KeepsRuntimeOnEdit(PropertyAboutToChange))
	{
		UObject::PreEditChange(PropertyAboutToChange);
		return;
	}
	Super::PreEditChange(PropertyAboutToChange);
}
#endif

void USoftbodyComponent::Activate(bool bReset)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyActivate);
	const bool bWasActive = IsActive();
	Super::Activate(bReset);
	if (!IsActive() || (bWasActive && !bReset))
	{
		return;
	}
	// 켜기(ADR-0020 §3.6): 처음처럼 시작한다 — 속도 기록을 비워 첫 틱은 자리만 적고(끈 동안 옮긴 거리를 가속으로 읽지 않는다), 끌림의 지난 위치를
	// 잊고(끈 동안의 콜라이더 이동이 한 번에 끌리지 않는다), 월드 풀에 이 캐리어 종류의 벌을 미리 만들어 둔다(처음 닿는 순간 멈칫하지 않게 — 끈 채
	// 세팅한 컴포넌트는 예약하지 않았다). 표면 장은 끌 때 재워 두어(SuspendRuntime) 닿는 순간 비운 뒤 첫 갱신으로 그 프레임에 돈다.
	ResetInertiaState();
	bTargetTeleported = false;
	PreviousShapeFrames.Reset();
	PreviousFieldFrames.Reset();
	PendingShapeFrames.Reset();
	PendingFieldFrames.Reset();
	PreviousSurfaceBones.Reset();
	PendingSurfaceBones.Reset();
	// 끌 때 끈 World Position Offset을 되돌리고, 표면 장을 다시 만든다(일지 222) — 접촉 지도는 비동기로 올라오고(이미 메모리에 있으면 바로), 그동안은
	// 접촉 없이 관성만 돈다. 세팅 전(게임 시작 때 Activate가 BeginPlay보다 먼저 불린다)이면 세팅이 만든다.
	SetTargetWorldPositionOffset(true);
	if (HasBegunPlay() && !bSetupPending)
	{
		SyncSurfaceField();
	}
}

void USoftbodyComponent::Deactivate()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyDeactivate);
	const bool bWasActive = IsActive();
	Super::Deactivate();
	if (bWasActive && !IsActive())
	{
		SuspendRuntime();
	}
}

void USoftbodyComponent::SuspendRuntime()
{
	// 끄기(ADR-0020 §3.6): 틱은 엔진이 멈췄다(콜라이더 질의·본 행렬·관성·업로드·파라미터 밀기 없음). 표면 장은 재운다 — 벌을 0으로 비워 풀에 돌려주고
	// (머티리얼 바인딩이 풀리고 이번 프레임 묶음 신청도 빠진다, GPU 갱신 없음) 다음에 닿는 순간은 비운 뒤 첫 갱신이 된다. 콜라이더의 지난 위치와 관성
	// 상태를 비우고, 머티리얼에 꺼진 값을 한 번 민다: 브릿지 0(지난 값도 0 — 속도 0), SurfaceFieldOn 0·InertiaResponseCount 0(정점 셰이더가 두 갈래를
	// 건너뛴다), 지난 L2W = 지금(모션 벡터 0). 메시는 모델링한 모양으로 돌아온다. 세팅 전이면(MID 없음) 밀 곳이 없다.
	// 끄면 0(일지 222·235): GPU 갱신은 바로 0. 표면 장은 재운다 — 텍스처 두 벌은 풀에, 접촉 지도는 월드 묶음에 돌려주고, 렌더 스레드 표만 쥔다. 풀·접촉 지도·
	// 표는 softbody.Surface.ReleaseDelay초 동안 쓰이지 않으면 월드 묶음이 프레임마다 조금씩 놓는다 — 그 안에 다시 켜면 다시 만들거나 읽지 않는다.
	SleepSurfaceField();
	bContactApplied = false;
	PendingShapeFrames.Reset();
	PendingFieldFrames.Reset();
	PendingSurfaceBones.Reset();
	bSurfaceFieldDispatched = false;
	bSurfaceFieldRan = false;
	SurfaceUpdateInterval = 0.f;
	SurfaceSinceUpdate = 0.f;
	ResetInertiaState();
	if (MIDs.Num() > 0)
	{
		LastContactData.Reset();
		bHasLastL2W = false;
		UploadContactTexture();
		PushMaterialParams();
	}
	// 머티리얼 정점 함수를 아예 돌리지 않는다. 같은 머티리얼의 다른 World Position Offset(바람 등)도 끈 동안 멈춘다.
	SetTargetWorldPositionOffset(false);
}

bool USoftbodyComponent::WantsSurfaceField() const
{
	return bEnableContact && IsActive();
}

void USoftbodyComponent::SetTargetWorldPositionOffset(bool bRestore)
{
	if (bRestore)
	{
		if (!bWorldPositionOffsetSuspended)
		{
			return;
		}
		bWorldPositionOffsetSuspended = false;
		UMeshComponent* Target = WorldPositionOffsetTarget.Get();
		WorldPositionOffsetTarget.Reset();
		if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Target))
		{
			Static->SetEvaluateWorldPositionOffset(bSavedEvaluateWorldPositionOffset);
		}
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		else if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Target))
		{
			Skinned->SetEvaluateWorldPositionOffset(bSavedEvaluateWorldPositionOffset);
		}
#endif
		return;
	}
	UMeshComponent* Target = SetupTarget.Get();
	if (bWorldPositionOffsetSuspended || !Target)
	{
		return;
	}
	if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Target))
	{
		bSavedEvaluateWorldPositionOffset = Static->bEvaluateWorldPositionOffset;
		Static->SetEvaluateWorldPositionOffset(false);
	}
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	// 5.6·5.7의 스켈레탈 메시 컴포넌트에는 WPO 스위치가 없다: 끈 동안에도 머티리얼 정점 함수가 돈다(값은 0이라 모양은 같고 비용만 남는다).
	else if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Target))
	{
		bSavedEvaluateWorldPositionOffset = Skinned->EvaluateWorldPositionOffset();
		Skinned->SetEvaluateWorldPositionOffset(false);
	}
#endif
	else
	{
		return;
	}
	WorldPositionOffsetTarget = Target;
	bWorldPositionOffsetSuspended = true;
}

void USoftbodyComponent::SyncSurfaceField()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodySyncSurfaceField);
	// 접촉 스위치(bEnableContact)나 켜기·끄기가 바뀌면 표면 장을 만들거나 지운다. 지울 때는 콜라이더의 지난 위치(끌림)도 잊는다.
	const bool bWant = WantsSurfaceField();
	if (bWant == bContactApplied)
	{
		return;
	}
	bContactApplied = bWant;
	if (bWant)
	{
		// 덩어리 해 읽기 번호는 표면 장(그 읽기 고리)마다 1부터 센다 — 새로 만들거나 깨운 장의 값을 바로 읽도록 되돌린다(전에는 껐다 켜면 끄기 전 번호에
		// 막혀 한동안 덩어리 해를 읽지 못했다, 일지 235).
		LumpSerial = 0;
		if (!SurfaceField)
		{
			USoftbodySurfaceField* Field = NewObject<USoftbodySurfaceField>(this);
			if (Field->Initialize(MeshData, IsActive()))
			{
				SurfaceField = Field;
				BindSurfaceFieldTextures(true);
			}
		}
		else if (SurfaceField->IsAsleep())
		{
			// 끈 동안 재워 둔 장을 깨운다 — 렌더 스레드 표(ReleaseDelay 안이면)와 풀의 텍스처·접촉 지도를 다시 쓴다(일지 235).
			SurfaceField->Wake(IsActive());
		}
		return;
	}
	SleepSurfaceField();
}

uint32 FSoftbodyColliderLabelIds::Get(const TPair<FObjectKey, int32>& Key)
{
	if (FEntry* Found = Entries.Find(Key))
	{
		Found->LastTick = Tick;
		return Found->Id;
	}
	if (Used.Num() != (int32)NumIds)
	{
		Used.Init(false, NumIds);
		Owners.SetNum(NumIds);
	}
	int32 Chosen = INDEX_NONE;
	for (uint32 Step = 0; Step < NumIds && Chosen == INDEX_NONE; ++Step)
	{
		const uint32 Id = (Next + Step) % NumIds;
		Chosen = Used[Id] ? INDEX_NONE : (int32)Id;
	}
	if (Chosen == INDEX_NONE)
	{
		uint64 Oldest = Tick;
		for (uint32 Id = 0; Id < NumIds; ++Id)
		{
			const FEntry* Owner = Entries.Find(Owners[Id]);
			if (Owner && Owner->LastTick < Oldest)
			{
				Oldest = Owner->LastTick;
				Chosen = (int32)Id;
			}
		}
		if (Chosen == INDEX_NONE)
		{
			return NoId;
		}
		Entries.Remove(Owners[Chosen]);
	}
	Used[Chosen] = true;
	Owners[Chosen] = Key;
	Entries.Add(Key, FEntry{(uint32)Chosen, Tick});
	Next = ((uint32)Chosen + 1) % NumIds;
	return (uint32)Chosen;
}

void USoftbodyComponent::SleepSurfaceField()
{
	// 끄기·접촉 끄기(일지 235): 표면 장을 지우지 않고 재운다 — GPU 갱신은 바로 0이고, 텍스처·접촉 지도는 풀이, 렌더 스레드 표는 장이 ReleaseDelay초 동안
	// 쥐어 그 안에 다시 켜면 아무것도 다시 만들지 않는다. 콜라이더의 지난 위치(끌림)는 잊는다.
	// 잡기(ADR-0041): 끄기·접촉 끄기는 대상 사라짐 — 재운 표면 장은 갱신하지 않으므로 GPU 줄은 다음에 깨어 처음 잡을 때 비운다.
	DropGrabs();
	if (SurfaceField)
	{
		BindSurfaceFieldTextures(false);
		SurfaceField->Sleep();
	}
	PreviousShapeFrames.Reset();
	PreviousFieldFrames.Reset();
	PendingShapeFrames.Reset();
	PendingFieldFrames.Reset();
	PreviousSurfaceBones.Reset();
	PendingSurfaceBones.Reset();
	bMotionFromLastUpdate = false;
	bSurfaceFieldDispatched = false;
	bSurfaceFieldRan = false;
	bPrevDispBoundToCurrent = false;
	SurfaceSinceUpdate = 0.f;
}

void USoftbodyComponent::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(InThis, Collector);
	Collector.AddReferencedObject(CastChecked<USoftbodyComponent>(InThis)->SurfaceField, InThis);
}

void USoftbodyComponent::ReleaseSurfaceField()
{
	// 잡기(ADR-0041): 표면 장을 지우면(Mesh Data를 바꿈·다시 세팅·등록 해제) 붙든 잡기는 대상 사라짐.
	DropGrabs();
	if (SurfaceField)
	{
		BindSurfaceFieldTextures(false);
		SurfaceField->Shutdown();
		SurfaceField->MarkAsGarbage();
		SurfaceField = nullptr;
	}
	PreviousShapeFrames.Reset();
	PreviousFieldFrames.Reset();
	PendingShapeFrames.Reset();
	PendingFieldFrames.Reset();
	PreviousSurfaceBones.Reset();
	PendingSurfaceBones.Reset();
	bMotionFromLastUpdate = false;
	bSurfaceFieldDispatched = false;
	bSurfaceFieldRan = false;
	bPrevDispBoundToCurrent = false;
	SurfaceUpdateInterval = 0.f;
	SurfaceSinceUpdate = 0.f;
}

void USoftbodyComponent::BindPreviousDisplacement(bool bToCurrent)
{
	// 갱신률 LOD(ADR-0021)가 이번 프레임을 미루면 화면의 변위는 지난 프레임과 같다. 머티리얼의 지난 프레임 계산(속도 버퍼)이 지금
	// 변위를 읽게 해 함몰의 속도를 0으로 둔다 — 지난 갱신의 움직임이 미룬 프레임마다 되풀이되면 TSR·모션 블러가 번진다.
	if (bPrevDispBoundToCurrent == bToCurrent || !SurfaceField)
	{
		return;
	}
	bPrevDispBoundToCurrent = bToCurrent;
	UTextureRenderTarget2D* Texture = bToCurrent ? SurfaceField->GetDisplacementTexture() : SurfaceField->GetPreviousDisplacementTexture();
	for (UMaterialInstanceDynamic* MID : MIDs)
	{
		MID->SetTextureParameterValue(Param_PrevSurfaceDispTex, Texture);
	}
}

void USoftbodyComponent::OnSurfaceFieldScheduled(bool bRan)
{
	bSurfaceFieldRan = bRan;
	if (bRan)
	{
		// 끌림의 "지난 위치"는 지난 틱이 아니라 지난 갱신이다 — 미룬 프레임은 건너뛰어 다음 갱신이 그동안의 움직임을 한 번에 본다.
		PreviousShapeFrames = MoveTemp(PendingShapeFrames);
		PreviousFieldFrames = MoveTemp(PendingFieldFrames);
		PreviousSurfaceBones = MoveTemp(PendingSurfaceBones);
		bMotionFromLastUpdate = true;
		// 쥔 덩어리 모드의 갱신 간격(일지 337): 이번 갱신까지 흐른 시간(미룬 틱 포함).
		SurfaceUpdateInterval = SurfaceSinceUpdate;
		SurfaceSinceUpdate = 0.f;
	}
	BindPreviousDisplacement(!bRan);
#if STATS
	if (bRan)
	{
		INC_DWORD_STAT(STAT_SoftbodyFieldUpdates);
	}
	else
	{
		INC_DWORD_STAT(STAT_SoftbodyFieldsDeferred);
	}
#endif
}

void USoftbodyComponent::OnSurfaceFieldStill()
{
	// 멈춘 틱(일지 203): 미룬 틱과 같다 — 화면의 변위가 지난 프레임과 같으니 속도 0, 끌림의 지난 위치는 마지막 갱신 그대로(깨는 갱신이 그동안의 움직임을 한 번에 본다).
	bSurfaceFieldRan = false;
	BindPreviousDisplacement(true);
#if STATS
	INC_DWORD_STAT(STAT_SoftbodyFieldsStill);
#endif
}

float USoftbodyComponent::WakeLumpLagBound() const
{
	// 덩어리 칸(스태틱은 메시 전체)은 흔들려 비켜난 자리에서 콜라이더를 잰다(접촉 패스의 LumpOffset). 지금 올리는 흔들림의 상한(무름 1인 점의
	// 흔들림 거리, 조이기·상한 적용 뒤)만큼 잠들기 상자를 더 키운다. 덩어리 칸 값이 없으면 0 — 스태틱은 덩어리 힘이 없어도 칸 값이 있다(일지 296).
	if (!(HasInertiaResponseBake() && bEnableInertia && !bContactOnlyValidation) || !MeshData || MeshData->CarrierLumpTex.IsNull())
	{
		return 0.f;
	}
	const bool bSkeletalGroups = NumInertiaGroups > 0 && SkinnedTarget.IsValid();
	const int32 NumDrivers = bSkeletalGroups ? NumInertiaGroups : 1;
	float Bound = 0.f;
	for (int32 g = 0; g < NumDrivers; ++g)
	{
		if (bSkeletalGroups && !(MeshData->InertiaGroupLump.IsValidIndex(g) && MeshData->InertiaGroupLump[g]))
		{
			continue;
		}
		const FSoftbodyInertiaDriver& Driver = InertiaDrivers[g];
		float L[6];
		FMemory::Memcpy(L, Driver.Load, sizeof(L));
		SoftbodyInertiaRuntime::SaturateInertiaLoad(L, Driver.Reach, InertiaSaturation);
		SoftbodyInertiaRuntime::LimitInertiaLoad(L, Driver.Reach, Driver.Amount * Driver.SagMax, InertiaMaxOffset, SoftbodyInertiaRuntime::LimitEaseStart);
		Bound = FMath::Max(Bound, SoftbodyInertiaRuntime::InertiaLoadMagnitude(L, Driver.Reach) * Driver.Amount * Driver.SagMax);
	}
	return Bound;
}

bool USoftbodyComponent::UpdateSurfaceFieldNear()
{
	// 지난 프레임에 이 월드를 그린 뷰(게임·PIE·화면 분할·편집기 뷰포트)마다 엔진이 LOD를 고르는 식으로 화면 크기를 재서 가장
	// 큰 값을 쓰고, 바운드 구가 어느 뷰의 시야에도 들지 않으면(시야 밖) 작게 보이는 것과 같이 본다(ADR-0020). 스켈레탈의
	// MaxDistanceFactor는 쓰지 않는다 — 나나이트 스켈레탈은 LOD가 하나면 갱신되지 않아 늘 0이다.
	const UMeshComponent* Target = SetupTarget.Get();
	const UWorld* World = GetWorld();
	if (!Target || !World || World->CachedViewInfoRenderedLastFrame.Num() == 0)
	{
		bSurfaceFieldNear = true;
		bSurfaceFieldInView = true;
		SurfaceScreenSize = 1.f;
		return true;
	}
	const FVector Origin = Target->Bounds.Origin;
	const float Radius = (float)Target->Bounds.SphereRadius;
	float ScreenSize = 0.f;
	bool bInView = false;
	for (const FWorldCachedViewInfo& View : World->CachedViewInfoRenderedLastFrame)
	{
		ScreenSize = FMath::Max(ScreenSize, ComputeBoundsScreenSize(FVector4(Origin, 1.0), Radius,
			FVector4(View.ViewToWorld.GetOrigin(), 1.0), View.ProjectionMatrix));
		if (!bInView)
		{
			FConvexVolume Frustum;
			GetViewFrustumBounds(Frustum, View.ViewProjectionMatrix, false);
			// 경계에서 켜졌다 꺼졌다 하지 않게, 켜져 있을 때는 바운드 구를 20% 키워 본다.
			bInView = Frustum.IntersectSphere(Origin, Radius * (bSurfaceFieldNear ? 1.2f : 1.f));
		}
	}
	SurfaceScreenSize = ScreenSize;
	bSurfaceFieldInView = bInView;
	const float Threshold = bSurfaceFieldNear ? SurfaceMinScreenSize : SurfaceMinScreenSize * 1.2f;
	bSurfaceFieldNear = bInView && (SurfaceMinScreenSize <= 0.f || ScreenSize >= Threshold);
	return bSurfaceFieldNear;
}

void USoftbodyComponent::UpdateSurfaceField(float Dt)
{
	SurfaceSinceUpdate += FMath::Max(Dt, 0.f);
	// 화면에 작게 보이거나 시야 밖이면 누르는 것을 모으지 않는다: 콜라이더가 떠난 것처럼 파임이 돌아온 뒤 GPU 갱신이 멈춘다(ADR-0013).
	const bool bNear = UpdateSurfaceFieldNear();
	ColliderLabelIds.BeginTick();
	TArray<FSoftbodyColliderShape> WorldShapes;
	TArray<const UPrimitiveComponent*> FieldColliders;
	if (bAutoDiscoverPressure && bNear)
	{
		GatherPressureShapes(WorldShapes, bUseColliderFields ? &FieldColliders : nullptr);
	}

	FSoftbodySurfaceFieldInput Input;
	const FTransform TargetTransform = GetTargetTransform();

	// 스켈레탈: 캐리어 점을 현재 포즈로 옮길 스키닝 행렬 (레퍼런스 포즈 → 현재, 컴포넌트 공간).
	// 행렬 열 0~2를 올리면 셰이더가 dot(float4(p, 1), 열)로 행 벡터 규약 v·M을 계산한다.
	TArray<FMatrix44f> TargetSkinning;
	if (USkinnedMeshComponent* Skinned = SkinnedTarget.Get())
	{
		const USkinnedMeshComponent* PoseSource = Skinned->LeaderPoseComponent.IsValid()
			? Skinned->LeaderPoseComponent.Get() : Skinned;
		const TArray<FTransform>& ComponentSpace = PoseSource->GetComponentSpaceTransforms();
		const TArray<int32>& LeaderMap = Skinned->GetLeaderBoneMap();
		const USkinnedAsset* Asset = Skinned->GetSkinnedAsset();
		if (Asset && ComponentSpace.Num() > 0)
		{
			const TArray<FMatrix44f>& RefBasesInverse = Asset->GetRefBasesInvMatrix();
			TargetSkinning.Reserve(RefBasesInverse.Num());
			Input.Bones.Reserve(RefBasesInverse.Num() * 3);
			for (int32 Bone = 0; Bone < RefBasesInverse.Num(); ++Bone)
			{
				const int32 Source = PoseSource != Skinned
					? (LeaderMap.IsValidIndex(Bone) ? LeaderMap[Bone] : INDEX_NONE)
					: Bone;
				const FMatrix44f Skinning = ComponentSpace.IsValidIndex(Source)
					? RefBasesInverse[Bone] * FMatrix44f(ComponentSpace[Source].ToMatrixWithScale())
					: FMatrix44f::Identity;
				TargetSkinning.Add(Skinning);
				for (int32 Column = 0; Column < 3; ++Column)
				{
					Input.Bones.Add(FVector4f(Skinning.M[0][Column], Skinning.M[1][Column],
						Skinning.M[2][Column], Skinning.M[3][Column]));
				}
			}
		}
	}
	// 손 닻(일지 336): 지난 갱신 자세를 같이 올려 셰이더가 피부가 본을 따라 움직인 거리를 뺀다. 이번 자세는 기다리게 두고 콜라이더의 지난 위치와
	// 같은 때에 바꾼다(OnSurfaceFieldScheduled, 장이 멈춘 틱은 아래 끝에서).
	Input.PreviousBones = PreviousSurfaceBones;
	PendingSurfaceBones = Input.Bones;

	// 잠들기(ADR-0020): 본마다 살이 콜라이더를 느낄 수 있는 자리를 지금 자세로 옮겨 두고, 닿을 수 없는 콜라이더·손 부위는 올리지 않는다.
	// 하나도 남지 않으면 콜라이더가 없는 것과 같아 표면 장이 복원 뒤 멈춘다. 이 검사는 틱마다 CPU가 하므로 닿는 순간 같은 프레임에 깬다.
	const USoftbodyColliderField* SelfField = bSelfContact && bNear && SkinnedTarget.IsValid()
		? USoftbodyColliderField::FindOnPrimitive(SkinnedTarget.Get()) : nullptr;
	FSoftbodyWakeBoxes Wake;
	if (WorldShapes.Num() > 0 || FieldColliders.Num() > 0 || SelfField)
	{
		Wake.Build(MeshData, SkinnedTarget.IsValid(), TargetSkinning, FVector3f(TargetTransform.GetScale3D()),
			WakeMargin + WakeLumpLagBound(), SelfField);
	}
	if (!Wake.bEnabled && MeshData && MeshData->CarrierWakeBounds.Num() == 0 && !bLoggedWakeMissing)
	{
		bLoggedWakeMissing = true;
		UE_LOG(LogSoftbodyRuntime, Log,
			TEXT("%s: %s was baked by an older version, so contact keeps running whenever a collider is near the mesh bounds. Press Rebake Contact Map to let it rest."),
			*GetPathName(), *MeshData->GetPathName());
	}
	if (SelfField && Wake.bEnabled && !Wake.bSelfValid && !bLoggedWakeSelfMismatch)
	{
		bLoggedWakeSelfMismatch = true;
		UE_LOG(LogSoftbodyRuntime, Warning,
			TEXT("%s: self contact parts differ from the wake boxes baked into %s — press Rebake Contact Map (or Rebake Shape Field) so contact can rest while the hands are away from the body."),
			*GetPathName(), *GetNameSafe(MeshData));
	}
	int32 NumGated = 0;
	if (Wake.bEnabled && WorldShapes.Num() > 0)
	{
		const FQuat InverseRotation = TargetTransform.GetRotation().Inverse();
		const FVector Origin = TargetTransform.GetLocation();
		NumGated += WorldShapes.RemoveAll([&](const FSoftbodyColliderShape& Shape)
		{
			const FBox Box = ShapeFrameBox(Shape, InverseRotation, Origin);
			return Box.IsValid && !Wake.Reaches(Box);
		});
	}

	TArray<int32> PackedSources;
	const int32 Skipped = USoftbodySurfaceField::PackShapes(WorldShapes, TargetTransform,
		MaxSurfaceShapes, MaxSurfacePlanes, Input.Shapes, Input.Planes, &PackedSources);
	// 끌림(ADR-0008): 형상마다 지난 프레임 위치(프레임 공간 회전·중심)를 찾아 "지금 점 → 지난 위치" 행렬을 싣는다.
	FSoftbodyMotionFrames ShapeFrames;
	FSoftbodyMotionTracker ShapeMotion{PreviousShapeFrames, ShapeFrames, Input.ShapeMotion, Input.ShapeIds, ColliderLabelIds, true};
	for (int32 Packed = 0; Packed < PackedSources.Num(); ++Packed)
	{
		const FSoftbodyColliderShape& Shape = WorldShapes[PackedSources[Packed]];
		const FVector4f& Center = Input.Shapes[Packed * 3 + 0];
		const FVector4f& Rotation = Input.Shapes[Packed * 3 + 1];
		const FTransform FrameFromShape(FQuat(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W), FVector(Center.X, Center.Y, Center.Z));
		ShapeMotion.Add(FSoftbodyMotionKey(FObjectKey(Shape.Component.Get()), Shape.SourceIndex), FrameFromShape.ToMatrixNoScale());
	}
	PendingShapeFrames = MoveTemp(ShapeFrames);
	if (Skipped > 0 && !bLoggedSurfaceSkippedShapes)
	{
		bLoggedSurfaceSkippedShapes = true;
		UE_LOG(LogSoftbodyRuntime, Warning,
			TEXT("%s: Contact skipped %d collision shape(s) over the per-frame budget (%d shapes, %d convex planes) or without planes."),
			*GetPathName(), Skipped, MaxSurfaceShapes, MaxSurfacePlanes);
	}

	// 모양 거리장(ADR-0004): 외부 콜라이더의 실제 모양 + 자기 몸의 다른 부위. 프레임 = 타깃 회전·위치(스케일 없음).
	const FMatrix WorldToFrame = FTransform(TargetTransform.GetRotation(), TargetTransform.GetLocation()).Inverse().ToMatrixWithScale();
	FSoftbodyMotionFrames FieldFrames;
	FSoftbodyMotionTracker FieldMotion{PreviousFieldFrames, FieldFrames, Input.FieldMotion, Input.FieldIds, ColliderLabelIds, true};
	for (const UPrimitiveComponent* Collider : FieldColliders)
	{
		if (AppendColliderFieldInstances(*SurfaceField, Collider, WorldToFrame, MaxSurfaceFieldInstances, Input.FieldInstances,
				FieldMotion, Wake, NumGated) < 0
			&& !bLoggedFieldBudget)
		{
			bLoggedFieldBudget = true;
			UE_LOG(LogSoftbodyRuntime, Warning, TEXT("%s: shape field of %s skipped (over %d parts per frame or bones not found)."),
				*GetPathName(), *Collider->GetPathName(), MaxSurfaceFieldInstances);
		}
	}
	if (SelfField)
	{
		AppendSelfFieldInstances(TargetSkinning, WorldToFrame, Input, FieldFrames, Wake, NumGated);
	}
	PendingFieldFrames = MoveTemp(FieldFrames);
#if STATS
	if (NumGated > 0 && Input.Shapes.Num() == 0 && Input.FieldInstances.Num() == 0)
	{
		INC_DWORD_STAT(STAT_SoftbodyFieldsNothingInReach);
	}
#endif

	Input.ComponentScale = FVector3f(TargetTransform.GetScale3D());
	Input.MaxContactDepth = MaxContactDepth;
	Input.OcclusionStrength = SurfaceOcclusionStrength;
	// 점탄성 막 한 식(ADR-0016 §9): 이완이 프레임 시간만큼 푼다.
	Input.RecoveryRate = FMath::Max(SurfaceRecoveryRate, 0.f);
	// 멈춤 비교(일지 203): 회전 허용 오차를 거리로 바꾸는 반지름과, 틱 시간에 안 매인 기억 원값.
	Input.BoundsRadius = SetupTarget.IsValid() ? (float)SetupTarget->Bounds.SphereRadius : 100.f;
	Input.MemoryTime = SurfaceMemoryTime;
	Input.MemoryFadeTime = SurfaceMemoryFadeTime;
	Input.DeltaTime = FMath::Max(Dt, 0.f);
	Input.ScreenSize = SurfaceScreenSize;
	Input.bInView = bSurfaceFieldInView;
	// 살이 눌린 모양에 길든다(ADR-0003 §14): 절반이 되는 시간 → 프레임마다 옮기는 비율·곱할 값.
	Input.AcceptBlend = SurfaceMemoryTime > 0.f ? 1.f - FMath::Pow(0.5f, FMath::Max(Dt, 0.f) / SurfaceMemoryTime) : 0.f;
	Input.AcceptReturn = FMath::Pow(0.5f, FMath::Max(Dt, 0.f) / FMath::Max(SurfaceMemoryFadeTime, 0.05f));
	// 접촉 단계(일지 222): Basic이면 끌림·솟음·둘레 넓힘 패스가 돌지 않게 재료값 대신 0을 보낸다(디테일 패널 값은 그대로 둔다). 끌림이 없으면 덩어리
	// 쥐기도 쥘 피부가 없어 꺼진다. 가장자리 다듬기도 Full에서만(일지 224) — 끄면 다듬기가 걸리는 칸의 가장 깊은 곳 다시 찾기도 돌지 않는다.
	const bool bFullDetail = ContactDetail == ESoftbodyContactDetail::Full;
	Input.EdgeTrim = bFullDetail ? SurfaceEdgeTrim : 0.f;
	Input.DragRatio = bFullDetail ? SurfaceDragRatio : 0.f;
	// 접촉 압력·긴 몫(ADR-0037)은 Full에서. 폭은 살 두께와 겉껍질에서, 솟는 몫은 저작 에셋의 Volume Preservation에서 온다(§3.5 — 폭만
	// 넓히던 Spread Scale은 지웠다).
	Input.bContactLong = bFullDetail;
	// 함몰 벽을 둘레 4x4 칸으로 이어 읽기(일지 359)도 Full에서만(일지 361) — 머티리얼은 SurfaceFieldOn으로 같은 단계를 받는다(PushMaterialParams).
	Input.bWallGhost = bFullDetail;
	// 덩어리(ADR-0012): 머티리얼과 같은 관성 브릿지로 덩어리가 비켜난 자리에서 파임을 잰다.
	BuildInertiaBridge(Input.InertiaBridge);
	Input.ComponentRotation = FQuat4f(TargetTransform.GetRotation());
	Input.FrameToActor = FVector3f(TargetTransform.GetLocation() - (GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector));
	Input.bLumpForce = LumpYield > 0.f && MeshData && MeshData->InertiaLumps.Num() > 0
		&& bEnableInertia && !bContactOnlyValidation && HasInertiaResponseBake();
	Input.LumpGrip = LumpGrip;
	Input.LumpStiffen = LumpStiffen;
	Input.VolumeOffset = VolumeOffset;
	Input.FlexOffset = FlexOffset;
	Input.SkinTension = SkinTension;
	// 멈춤 비교용: 저작 에셋의 Volume Preservation은 굽지 않고 구운 데이터로 바로 옮겨 와 실행 중에도 바뀐다.
	Input.BakedVolumePreservation = MeshData ? MeshData->VolumePreservation : 1.f;
	// 덩어리 평형(ADR-0017): 덩어리마다 뿌리 스프링·흔들림 크기·살 단단함과 이번 프레임 관성 목표를 올린다(GPU가 뉴턴으로 푼다).
	Input.LumpGroups.Reset();
	if (Input.bLumpForce)
	{
		Input.LumpGroups.Init(FVector4f(0.f, 0.f, 0.f, 0.f), USoftbodySurfaceField::NumLumpGroups * USoftbodySurfaceField::NumLumpGroupRows);
		for (const FSoftbodyInertiaLump& Lump : MeshData->InertiaLumps)
		{
			double Stiffness;
			double Amount;
			double C[3][3];
			if (!ResolveLumpConstants(Lump, Stiffness, Amount, C))
			{
				continue;
			}
			const FVector& Target = LumpInertialTarget[Lump.Group];
			FVector4f* Rows = &Input.LumpGroups[Lump.Group * USoftbodySurfaceField::NumLumpGroupRows];
			Rows[0] = FVector4f((float)C[0][0], (float)C[0][1], (float)C[0][2], (float)Stiffness);
			Rows[1] = FVector4f((float)C[1][1], (float)C[1][2], (float)C[2][2], (float)Amount);
			Rows[2] = FVector4f((float)Target.X, (float)Target.Y, (float)Target.Z, 1.f);
			// 국소 모양(일지 251)의 단단함 Λ: 굽기가 적은 값(C와 같은 단위라 메시 크기에 따라 바꾸지 않는다). 모양이 없는 자리는 1 — 그 모양은
			// 칸마다 0이라 크기가 0에 머문다.
			for (int32 Mode = 0; Mode < USoftbodySurfaceField::NumLumpRegionModes; ++Mode)
			{
				const bool bBaked = Lump.RegionStiffness.IsValidIndex(Mode) && Lump.RegionStiffness[Mode] > 0.f;
				Rows[3 + Mode / 4][Mode % 4] = bBaked ? Lump.RegionStiffness[Mode] : 1.f;
			}
		}
	}
#if STATS
	// 통계를 보는 중(stat softbody·stat gpu 등)일 때만 GPU 상태를 읽어 온다(스레드 하나짜리 패스 + 80바이트 읽기).
	Input.bReadStats = FThreadStats::IsCollectingData();
#endif
	// 프로파일 명령(Softbody.Stats·Softbody.Profile, 일지 189)이 잠시 켠다.
	Input.bReadStats |= SoftbodyProfiling::IsForcingStats();
	// 들어온 면 표지의 길 검사(일지 313): 콜라이더의 지난 자세가 표면 장의 지난 갱신 것일 때만(PositionOut의 지난 피부 자리와 같은 순간).
	Input.bSweep = bMotionFromLastUpdate;
	// 잡기(ADR-0041): 화면에 작게 보이거나 시야 밖이면 싣지 않는다 — 콜라이더처럼 쉬고(보이지 않는 비용 0), 잡기 칸은 남아 다시 보이면 이어진다.
	if (bNear)
	{
		BuildGrabRows(TargetTransform, Input);
	}
	bSurfaceFieldDispatched = SurfaceField->Update(Input);
	if (!bSurfaceFieldDispatched)
	{
		// 멈춘 표면 장: 콜라이더의 지난 위치는 틱마다 따라간다(깨는 첫 갱신이 지난 틱 위치에서 끌림을 본다). 신청했으면
		// 묶음이 돌린 프레임에만 OnSurfaceFieldScheduled가 옮긴다. 그 사이 몸이 움직였을 수 있어 깨는 첫 갱신은 길 검사를 하지 않는다.
		PreviousShapeFrames = MoveTemp(PendingShapeFrames);
		PreviousFieldFrames = MoveTemp(PendingFieldFrames);
		PreviousSurfaceBones = MoveTemp(PendingSurfaceBones);
		bMotionFromLastUpdate = false;
		// 쉬는 동안은 갱신 간격을 재지 않는다(깨는 첫 갱신이 쉰 시간을 간격으로 보지 않게).
		SurfaceSinceUpdate = 0.f;
	}
}

void USoftbodyComponent::AppendSelfFieldInstances(TConstArrayView<FMatrix44f> Skinning, const FMatrix& WorldToFrame,
	FSoftbodySurfaceFieldInput& Input, TMap<TPair<FObjectKey, int32>, FMatrix>& CurrentFieldFrames, const FSoftbodyWakeBoxes& Wake,
	int32& OutGated)
{
	USkinnedMeshComponent* Skinned = SkinnedTarget.Get();
	const USoftbodyColliderField* Field = Skinned ? USoftbodyColliderField::FindOnPrimitive(Skinned) : nullptr;
	if (!Field || Field->Parts.Num() == 0 || Skinning.Num() == 0)
	{
		return;
	}
	// 자기 접촉은 제 몸도 누르는 부위(Self Contact Bones)만 싣는다. 나머지 부위는 다른 소프트바디를 누를 때만 쓴다(ADR-0004 §11).
	int32 NumSelfParts = 0;
	for (const FSoftbodyColliderFieldPart& Part : Field->Parts)
	{
		NumSelfParts += Part.bPressesSelf ? 1 : 0;
	}
	if (NumSelfParts == 0)
	{
		return;
	}
	if (Input.FieldInstances.Num() / SoftbodySurfaceFieldRows::PerInstance + NumSelfParts > MaxSurfaceFieldInstances)
	{
		if (!bLoggedFieldBudget)
		{
			bLoggedFieldBudget = true;
			UE_LOG(LogSoftbodyRuntime, Warning, TEXT("%s: self contact skipped this frame (over %d shape field parts)."),
				*GetPathName(), MaxSurfaceFieldInstances);
		}
		return;
	}
	const int32 VoxelBase = SurfaceField->RegisterColliderField(Field);
	const FMatrix WorldFromComponent = Skinned->GetComponentTransform().ToMatrixWithScale();
	FSoftbodyMotionTracker Motion{PreviousFieldFrames, CurrentFieldFrames, Input.FieldMotion, Input.FieldIds, ColliderLabelIds, false};
	const FObjectKey SelfKey(Skinned);
	int32 Slot = -1;
	for (int32 Part = 0; Part < Field->Parts.Num(); ++Part)
	{
		if (!Field->Parts[Part].bPressesSelf)
		{
			continue;
		}
		++Slot;
		// 본을 못 찾은 부위(메시를 바꾼 뒤 다시 굽지 않음)는 싣지 않는다.
		const int32 Bone = Skinned->GetBoneIndex(Field->Parts[Part].Bone);
		if (!Skinning.IsValidIndex(Bone))
		{
			if (!bLoggedSelfFieldMismatch)
			{
				bLoggedSelfFieldMismatch = true;
				UE_LOG(LogSoftbodyRuntime, Warning, TEXT("%s: self contact part %s skipped — bone not found on %s. Press Rebake Shape Field."),
					*GetPathName(), *Field->Parts[Part].Bone.ToString(), *GetNameSafe(Skinned->GetSkinnedAsset()));
			}
			continue;
		}
		const FMatrix FrameFromMesh = FMatrix(Skinning[Bone]) * WorldFromComponent * WorldToFrame;
		// 잠들기(ADR-0020): 이 부위가 누를 수 있는 살(기본 포즈에서 붙어 있던 살은 뺀)에 닿을 수 없으면 싣지 않는다.
		if (!Wake.SelfReaches(Slot, PartFrameBox(Field->Parts[Part], FrameFromMesh)))
		{
			++OutGated;
			continue;
		}
		PackFieldInstance(Field->Parts[Part], Field->GetPartFleshHalfExtent(Part), VoxelBase, FrameFromMesh, (float)Part,
			Input.FieldInstances, Motion, FSoftbodyMotionKey(SelfKey, Part));
	}
}

namespace
{
	uint32 GrabBits(float Value)
	{
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	float GrabFloat(uint32 Bits)
	{
		float Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}
}

int32 USoftbodyComponent::BeginGrab(USoftbodyGrabberComponent* Grabber, const FVector& Location, const FVector& TraceDirection, float Radius,
	uint32& OutSerial, FString* OutReason, float SearchDistance)
{
	static_assert(MaxGrabRows == SoftbodyGrab::MaxRows, "the component's grab slots match the GPU grab rows");
	const auto Fail = [OutReason](const TCHAR* Why)
	{
		if (OutReason)
		{
			*OutReason = Why;
		}
		return INDEX_NONE;
	};
	if (!IsActive() || !bEnableContact)
	{
		return Fail(TEXT("its Softbody component is off or Enable Contact is off"));
	}
	if (!IsSurfaceFieldMode() || !SurfaceField || !SurfaceField->IsReady())
	{
		return Fail(TEXT("its contact is not ready yet (the contact map is still loading, or Mesh Data has no contact map)"));
	}
	int32 Holding = 0;
	for (const FGrabSlot& Slot : GrabSlots)
	{
		Holding += Slot.State == FGrabSlot::EState::Holding ? 1 : 0;
	}
	if (Holding >= SoftbodyGrab::MaxHolding)
	{
		return Fail(TEXT("the mesh already holds 4 grabs"));
	}
	// 빈 칸이 먼저, 없으면 놓는 중 줄 가운데 남은 당김이 가장 작은 줄(그 당김은 새 잡기가 덮는다 — 1 µm까지 기다리면 Recovery Rate가 낮은 살에서 몇 번
	// 잡은 뒤 잡기가 실패한다).
	int32 Chosen = INDEX_NONE;
	float SmallestLeft = TNumericLimits<float>::Max();
	for (int32 Row = 0; Row < MaxGrabRows; ++Row)
	{
		const FGrabSlot& Slot = GrabSlots[Row];
		if (Slot.State == FGrabSlot::EState::Free)
		{
			Chosen = Row;
			break;
		}
		if (Slot.State == FGrabSlot::EState::Releasing && Slot.Remaining < SmallestLeft)
		{
			SmallestLeft = Slot.Remaining;
			Chosen = Row;
		}
	}
	if (Chosen == INDEX_NONE)
	{
		return Fail(TEXT("no grab row is free"));
	}
	FGrabSlot& Slot = GrabSlots[Chosen];
	Slot = FGrabSlot();
	Slot.State = FGrabSlot::EState::Holding;
	Slot.Serial = NextGrabSerial++;
	if (NextGrabSerial == 0)
	{
		NextGrabSerial = 1;
	}
	Slot.Grabber = Grabber;
	Slot.Location = Location;
	Slot.TraceDirection = TraceDirection.GetSafeNormal();
	Slot.Radius = FMath::Max(Radius, 0.1f);
	Slot.SearchDistance = FMath::Max(SearchDistance, 0.f);
	OutSerial = Slot.Serial;
	return Chosen;
}

void USoftbodyComponent::EndGrab(int32 Slot, uint32 Serial)
{
	if (Slot < 0 || Slot >= MaxGrabRows || GrabSlots[Slot].Serial != Serial || GrabSlots[Slot].State != FGrabSlot::EState::Holding)
	{
		return;
	}
	// 놓는 중: GPU가 남은 당김을 Recovery Rate로 줄이고 1 µm 아래에서 줄을 비운다(PollGrabs가 그때 칸을 비운다).
	GrabSlots[Slot].State = FGrabSlot::EState::Releasing;
	GrabSlots[Slot].Grabber.Reset();
}

bool USoftbodyComponent::GetGrabInfo(int32 Slot, uint32 Serial, FGrabInfo& Out) const
{
	if (Slot < 0 || Slot >= MaxGrabRows || GrabSlots[Slot].Serial != Serial || !GrabSlots[Slot].bSeen)
	{
		return false;
	}
	const FGrabSlot& Grab = GrabSlots[Slot];
	const FTransform TargetTransform = GetTargetTransform();
	Out.bHeld = Grab.bGpuHeld;
	Out.Stretch = Grab.Stretch;
	Out.Limit = Grab.Limit;
	Out.Flesh = Grab.Flesh;
	Out.LumpFollow = Grab.LumpFollow;
	Out.GrabbedPoint = TargetTransform.GetLocation() + TargetTransform.GetRotation().RotateVector(FVector(Grab.GrabbedFrame));
	return true;
}

void USoftbodyComponent::PollGrabs()
{
	if (!SurfaceField)
	{
		return;
	}
	TArray<FVector4f> Rows;
	if (!SurfaceField->GetGrabState(Rows, GrabReadSerial))
	{
		return;
	}
	for (int32 Row = 0; Row < MaxGrabRows; ++Row)
	{
		FGrabSlot& Slot = GrabSlots[Row];
		const FVector4f* Gpu = &Rows[Row * SoftbodyGrab::StateRows];
		const uint32 Word = GrabBits(Gpu[0].X);
		const SoftbodyGrab::EStatus Status = (SoftbodyGrab::EStatus)(Word & 0xffu);
		const uint32 Serial = GrabBits(Gpu[0].Z);
		// GPU 확인 없이 비운 칸: 줄이 비었으면 비우기 표지를 거둔다.
		if (Slot.State == FGrabSlot::EState::Free)
		{
			Slot.bClear = Slot.bClear && Status != SoftbodyGrab::EStatus::Empty;
			continue;
		}
		// GPU가 아직 이 잡기를 보지 않았다(줄에는 앞 잡기가 남아 있다). 붙들기 전에 놓은 잡기(GPU가 한 번도 읽지 않은 번호)는 GPU가 다시 볼 일이 없다 — 칸을
		// 비우고 줄을 비우기 표지로 지운다(안 그러면 놓는 중으로 남아 표면 장이 쉬지 못한다).
		if (Serial != Slot.Serial)
		{
			if (Slot.State == FGrabSlot::EState::Releasing && !Slot.bSeen)
			{
				Slot = FGrabSlot();
				Slot.bClear = true;
			}
			continue;
		}
		Slot.bSeen = true;
		Slot.bGpuHeld = Status == SoftbodyGrab::EStatus::Held;
		Slot.Stretch = Gpu[4].W;
		Slot.Limit = Gpu[2].W;
		Slot.Flesh = Gpu[1].W;
		Slot.LumpFollow = Status == SoftbodyGrab::EStatus::Held ? Gpu[8].W : 0.f;
		Slot.Remaining = Status == SoftbodyGrab::EStatus::Releasing ? Gpu[7].W : 0.f;
		Slot.GrabbedFrame = FVector3f(Gpu[3].X, Gpu[3].Y, Gpu[3].Z);
		if (Slot.State == FGrabSlot::EState::Holding)
		{
			USoftbodyGrabberComponent* Grabber = Slot.Grabber.Get();
			if (Status == SoftbodyGrab::EStatus::Missed || Status == SoftbodyGrab::EStatus::Empty)
			{
				// 못 잡음: 칸을 비운다 — 다음 갱신에 붙드는 표지가 꺼져 GPU가 줄을 비운다.
				const uint32 Ended = Slot.Serial;
				Slot = FGrabSlot();
				if (Grabber)
				{
					Grabber->NotifyGrabEnded(ESoftbodyGrabEndReason::Missed, Ended);
				}
			}
		}
		else if (Slot.State == FGrabSlot::EState::Releasing
			&& (Status == SoftbodyGrab::EStatus::Empty || Status == SoftbodyGrab::EStatus::Missed))
		{
			Slot = FGrabSlot();
		}
	}
}

void USoftbodyComponent::BuildGrabRows(const FTransform& TargetTransform, FSoftbodySurfaceFieldInput& Input)
{
	bool bAny = false;
	for (FGrabSlot& Slot : GrabSlots)
	{
		// 손(Grabber)이 사라진 붙든 잡기는 놓는다(Grabber의 OnUnregister가 먼저 Release를 부르지만, 바로 GC로 사라진 경우).
		if (Slot.State == FGrabSlot::EState::Holding && !Slot.Grabber.IsValid())
		{
			Slot.State = FGrabSlot::EState::Releasing;
		}
		bAny |= Slot.State != FGrabSlot::EState::Free || Slot.bClear;
	}
	if (!bAny)
	{
		return;
	}
	const FQuat ToFrame = TargetTransform.GetRotation().Inverse();
	const FVector Origin = TargetTransform.GetLocation();
	const auto Frame = [&ToFrame, &Origin](const FVector& World)
	{
		return FVector3f(ToFrame.RotateVector(World - Origin));
	};
	Input.Grabs.SetNumZeroed(SoftbodyGrab::MaxRows * SoftbodyGrab::InputRows);
	bool bHolding = false;
	for (int32 Row = 0; Row < MaxGrabRows; ++Row)
	{
		const FGrabSlot& Slot = GrabSlots[Row];
		FVector4f* Out = &Input.Grabs[Row * SoftbodyGrab::InputRows];
		if (Slot.State == FGrabSlot::EState::Free)
		{
			Out[2].W = GrabFloat(Slot.bClear ? SoftbodyGrab::FlagClear : 0u);
			continue;
		}
		const bool bHold = Slot.State == FGrabSlot::EState::Holding;
		const USoftbodyGrabberComponent* Grabber = Slot.Grabber.Get();
		const FVector Target = bHold && Grabber ? Grabber->GetComponentLocation() : Slot.Location;
		uint32 Flags = bHold ? SoftbodyGrab::FlagHold : 0u;
		Flags |= bHold && !Slot.TraceDirection.IsNearlyZero() ? SoftbodyGrab::FlagFacing : 0u;
		// 찾는 거리(셰이더 표지 비트 8~15, 0.1 cm 단위 — 0이면 기본 5 cm)
		if (Slot.SearchDistance > 0.f)
		{
			Flags |= (uint32)FMath::Clamp(FMath::RoundToInt(Slot.SearchDistance * 10.f), 1, 255) << 8u;
		}
		Out[0] = FVector4f(Frame(Target), Slot.Radius);
		Out[1] = FVector4f(Frame(Slot.Location), GrabFloat(Slot.Serial));
		Out[2] = FVector4f(FVector3f(ToFrame.RotateVector(Slot.TraceDirection)), GrabFloat(Flags));
		bHolding |= bHold;
	}
	Input.bGrabbing = bHolding;
}

void USoftbodyComponent::DropGrabs()
{
	for (FGrabSlot& Slot : GrabSlots)
	{
		if (Slot.State == FGrabSlot::EState::Free)
		{
			continue;
		}
		USoftbodyGrabberComponent* Grabber = Slot.State == FGrabSlot::EState::Holding ? Slot.Grabber.Get() : nullptr;
		const uint32 Ended = Slot.Serial;
		Slot = FGrabSlot();
		Slot.bClear = true;
		if (Grabber)
		{
			Grabber->NotifyGrabEnded(ESoftbodyGrabEndReason::TargetLost, Ended);
		}
	}
	GrabReadSerial = 0;
}

void USoftbodyComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	SCOPE_CYCLE_COUNTER(STAT_SoftbodyTick);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
#if STATS
	PublishSoftbodyGpuTimes();
	INC_DWORD_STAT(STAT_SoftbodyComponents);
#endif

	// 세팅한 대상(MeshData, 변형할 메시, 그 메시에 건 머티리얼)이 실행 중에 바뀌었으면 다시 세팅한다. 블루프린트 액터의 값을 디테일
	// 패널에서 고쳐 엔진이 컴포넌트를 새로 만든 경우도 여기서 잡힌다(새 메시에 옛 머티리얼이 되돌아와 우리가 건 것을 덮는다).
	if (IsSetupStale())
	{
		SetupRuntime();
	}
	SyncSurfaceField();

	// 관성을 먼저 움직인다: 표면 장이 이번 프레임 흔들림(머티리얼이 그릴 값)으로 덩어리 칸의 파임을 잰다(ADR-0012).
	UpdateLumpContact();
	StepInertia(DeltaTime);

	if (IsSurfaceFieldMode())
	{
		SCOPE_CYCLE_COUNTER(STAT_SoftbodySurfaceField);
		PollGrabs();
		UpdateSurfaceField(DeltaTime);
#if STATS
		// 멈춘 장(일지 203)은 OnSurfaceFieldStill이 "Still"로 센다 — 깨어 있지만 신청하지 않았다.
		if (bSurfaceFieldDispatched)
		{
			if (!SurfaceField || !SurfaceField->IsStill())
			{
				INC_DWORD_STAT(STAT_SoftbodyFieldsRunning);
			}
		}
		else
		{
			INC_DWORD_STAT(STAT_SoftbodyFieldsStopped);
		}
		if (!bSurfaceFieldNear)
		{
			INC_DWORD_STAT(STAT_SoftbodyFieldsSmall);
		}
		if (SurfaceField)
		{
			INC_FLOAT_STAT_BY(STAT_SoftbodyFieldMemory, (double)SurfaceField->GetTextureMemoryBytes() / (1024.0 * 1024.0));
			USoftbodySurfaceField::FGpuState GpuState;
			if (bSurfaceFieldDispatched && SurfaceField->GetGpuState(GpuState))
			{
				INC_DWORD_STAT_BY(STAT_SoftbodyFieldsPressed, GpuState.bPressed);
				INC_DWORD_STAT_BY(STAT_SoftbodyFieldsResting, GpuState.bResting);
				INC_DWORD_STAT_BY(STAT_SoftbodyDentTiles, GpuState.DentTiles);
				INC_DWORD_STAT_BY(STAT_SoftbodyDragSources, GpuState.DragSources);
				INC_DWORD_STAT_BY(STAT_SoftbodyDragGroups, GpuState.DragGroups);
				INC_DWORD_STAT_BY(STAT_SoftbodyContactGroups, GpuState.ContactGroups);
				INC_DWORD_STAT_BY(STAT_SoftbodyDragClusters, GpuState.DragClusters);
				INC_DWORD_STAT_BY(STAT_SoftbodyDragMixedClusters, GpuState.DragMixedClusters);
				PublishDragStepsMax(GpuState.DragCorrect.MaxSteps, GpuState.DragGather.MaxSteps);
			}
		}
#endif
	}
	else
	{
		bSurfaceFieldDispatched = false;
	}

	if (!bLoggedInertiaActivity
		&& UE_LOG_ACTIVE(LogSoftbodyRuntime, Verbose))
	{
		float MaxLoad = 0.f;
		float MaxLag = 0.f;
		for (const FSoftbodyInertiaDriver& Driver : InertiaDrivers)
		{
			const float Load = SoftbodyInertiaRuntime::InertiaLoadMagnitude(Driver.Load, Driver.Reach);
			MaxLoad = FMath::Max(MaxLoad, Load);
			MaxLag = FMath::Max(MaxLag, Load * Driver.Amount * Driver.SagMax);
		}
		if (MaxLag > 0.1f)
		{
			UE_LOG(LogSoftbodyRuntime, Verbose,
				TEXT("%s: inertia active (max load %.3f g, max lag %.2f cm)."),
				*GetPathName(), MaxLoad, MaxLag);
			bLoggedInertiaActivity = true;
		}
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_SoftbodyUpload);
		UploadContactTexture();
		PushMaterialParams();
	}
	OnSoftbodyUpdated.Broadcast();
}

bool USoftbodyComponent::HasInertiaResponseBake() const
{
	return MeshData
		&& MeshData->InertiaResponseVersion == 2
		&& MeshData->NumInertiaResponses == NumInertiaResponses
		&& MeshData->InertiaResponseTex;
}

void USoftbodyComponent::ResetInertiaState()
{
	for (FSoftbodyInertiaDriver& Driver : InertiaDrivers)
	{
		Driver = FSoftbodyInertiaDriver();
	}
	for (int32 Group = 0; Group < MaxInertiaGroups; ++Group)
	{
		LumpModes[Group] = FLumpModes();
		LumpSolutions[Group] = FLumpSolution();
		LumpInertialTarget[Group] = FVector::ZeroVector;
	}
	LumpSerial = 0;
	bMotionInit = false;
}

namespace
{
	/** 한 틱에 이만큼(도) 넘게 더 돌면 순간이동으로 본다(지금 회전 빠르기로 설명되는 몫을 뺀 나머지). */
	constexpr double TeleportTurnDegrees = 45.0;

	/**
	 * 애니메이션이 블렌드 없이 바뀌면 본이 한 프레임에 몇 cm 건너뛴다(일지 342: 걷기·뛰기 전환 5.8~17.8 cm, 120 fps에서 170~520 g). 컴포넌트 공간
	 * 자세의 가속이 이보다 크면 그 틱은 움직임 가속을 버리고 다음 틱에 속도를 다시 잰다 — 살은 하던 대로 출렁인다. 보통 움직임은 가장 센 뛰기도
	 * 15 g 아래(30 fps), 회전은 몸통이 휙 도는 것도 수십 rad/s².
	 */
	constexpr double PoseSnapAcceleration = 25.0 * 981.0;
	constexpr double PoseSnapAngularAcceleration = 250.0;

	/**
	 * 이전 기록으로 한 점의 가속·회전 가속을 추정하고 기록을 갱신한다.
	 * bHasVelocity가 거짓이면(처음·히치·순간이동 뒤) 이번 틱은 속도만 재고 가속은 0이다.
	 * 텔레포트 표시 없이 옮긴 순간이동(SetActorLocation 등)도 여기서 걸러 낸다: 지금 속도로 갈 수 있는 거리보다 JumpDistance(cm) 넘게
	 * 더 옮겼거나 TeleportTurnDegrees 넘게 더 돌았으면 그 틱은 가속 없이 자리만 옮기고 다음 틱에 속도를 다시 잰다. 착지처럼 갑자기
	 * 멈추는 것은 자리가 이어지므로 걸리지 않는다. 돌려준 값: 순간이동으로 보았는가.
	 */
	bool MeasureMotion(const FVector& Location, const FQuat& Rotation, float Dt, float JumpDistance, bool& bHasVelocity,
		FVector& PrevLoc, FVector& PrevVel, FQuat& PrevRotation, FVector& PrevAngularVelocity,
		FVector& OutAcceleration, FVector& OutAngularAcceleration)
	{
		OutAcceleration = FVector::ZeroVector;
		OutAngularAcceleration = FVector::ZeroVector;
		if (bHasVelocity)
		{
			const double Moved = FVector::Distance(Location, PrevLoc);
			const double Turned = FMath::RadiansToDegrees(Rotation.AngularDistance(PrevRotation));
			const bool bJumped = JumpDistance > 0.f && Moved > PrevVel.Size() * Dt + JumpDistance;
			const bool bSnapped = Turned > FMath::RadiansToDegrees(PrevAngularVelocity.Size() * Dt) + TeleportTurnDegrees;
			if (bJumped || bSnapped)
			{
				PrevLoc = Location;
				PrevRotation = Rotation;
				bHasVelocity = false;
				return true;
			}
		}
		const FVector Velocity = (Location - PrevLoc) / Dt;
		const FVector AngularVelocity =
			SoftbodyInertiaRuntime::ComputeAngularVelocity(PrevRotation, Rotation, Dt);
		if (bHasVelocity)
		{
			OutAcceleration = ((Velocity - PrevVel) / Dt).GetClampedToMaxSize(50000.f);
			OutAngularAcceleration =
				((AngularVelocity - PrevAngularVelocity) / Dt).GetClampedToMaxSize(500.f);
		}
		PrevLoc = Location;
		PrevVel = Velocity;
		PrevRotation = Rotation;
		PrevAngularVelocity = AngularVelocity;
		bHasVelocity = true;
		return false;
	}
}

void USoftbodyComponent::OnTargetTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport)
{
	// 피직스가 바디를 따라 옮기는 것과 보통의 이동은 None으로 온다. 텔레포트로 표시한 이동만 순간이동이다.
	bTargetTeleported |= Teleport != ETeleportType::None;
}

void USoftbodyComponent::StepInertia(float Dt)
{
	// 관성은 살의 무름 베이크가 있을 때만 존재한다. 없는 데이터는 ValidateSetup이
	// 이미 오류로 보고했으므로 여기서는 조용히 0을 유지한다.
	if (bContactOnlyValidation || !bEnableInertia || !HasInertiaResponseBake())
	{
		ResetInertiaState();
		return;
	}
	if (Dt <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 구동점: 스켈레탈은 관성 그룹 본마다, 스태틱은 메시 전체의 앵커 중심 하나.
	// 살 한 점의 흔들림 = 그 점의 무름 × (구동점의 가속·중력 변화 + 회전 가속 × 그 점까지 거리).
	const bool bSkeletal = NumInertiaGroups > 0 && SkinnedTarget.IsValid();
	const int32 NumDrivers = bSkeletal ? NumInertiaGroups : 1;
	const FTransform TargetTransform = GetTargetTransform();
	const FQuat TargetRotation = TargetTransform.GetRotation().GetNormalized();
	const float WorldScale = (float)TargetTransform.GetMaximumAxisScale();
	const UWorld* World = GetWorld();
	const FVector WorldGravity(0.f, 0.f,
		World ? World->GetGravityZ() : -SoftbodyInertiaRuntime::StandardGravity);
	// 히치 가드: 긴 틱은 가속 추정을 건너뛰고 기록만 현재로 맞춘다(흔들림 상태는 이어 간다).
	const bool bRestart = !bMotionInit || Dt > FMath::Max(AccelerationDtGuard, 0.01f);
	// 순간이동: 지난 틱 뒤에 타겟이 텔레포트로 옮겨졌다. 건너뛴 거리는 가속이 아니므로 이번 틱은 자리 기록만 새 자리로 옮긴다.
	// 흔들림 상태는 그대로 두고(움직이던 살은 하던 대로 출렁인다) 속도는 다음 틱에 다시 잰다 — 표시 없는 순간이동(MeasureMotion)과 같다.
	// 옛 속도를 두면 달리다 멈춘 자리로 리스폰할 때 다음 틱이 그 차이를 수십 g 가속으로 읽는다.
	const bool bTeleported = bTargetTeleported;
	bTargetTeleported = false;

	for (int32 d = 0; d < NumDrivers; ++d)
	{
		FSoftbodyInertiaDriver& Driver = InertiaDrivers[d];
		FVector Pivot;
		FQuat Rotation;
		FVector PoseLoc = FVector::ZeroVector;
		FQuat PoseRotation = FQuat::Identity;
		float Reach = 0.f;
		if (bSkeletal)
		{
			if (GroupBoneIndices[d] == INDEX_NONE)
			{
				Driver.ClearLoad();
				continue;
			}
			const FTransform BoneWorld = SkinnedTarget->GetBoneTransform(GroupBoneIndices[d]);
			// Identity를 LocalToWorld로 넘기면 follower/leader-pose 매핑까지
			// SkinnedMeshComponent가 처리한 컴포넌트 공간 본 변환을 얻는다.
			const FTransform BonePose =
				SkinnedTarget->GetBoneTransform(GroupBoneIndices[d], FTransform::Identity);
			Pivot = BoneWorld.GetLocation();
			Rotation = BoneWorld.GetRotation().GetNormalized();
			PoseLoc = BonePose.GetLocation();
			PoseRotation = BonePose.GetRotation().GetNormalized();
			Driver.ReferenceToWorld = SoftbodyInertiaRuntime::ReferenceToWorldRotation(
				Rotation, GroupReferenceRotation[d]);
			Reach = MeshData->InertiaGroupReach.IsValidIndex(d) ? MeshData->InertiaGroupReach[d] : 0.f;
		}
		else
		{
			Pivot = TargetTransform.TransformPosition(FVector(MeshData->InertiaAnchorLocal));
			Rotation = TargetRotation;
			Driver.ReferenceToWorld = TargetRotation;
			Reach = MeshData->InertiaAnchorReach;
		}
		Driver.Pivot = Pivot;

		FVector Acceleration = FVector::ZeroVector;
		FVector AngularAcceleration = FVector::ZeroVector;
		if (bRestart)
		{
			// 자리만 기록한다. 속도는 다음 틱에 재고, 그 틱의 가속은 0이다(MeasureMotion).
			Driver.PrevLoc = Pivot;
			Driver.PrevVel = FVector::ZeroVector;
			Driver.PrevRotation = Rotation;
			Driver.PrevAngularVelocity = FVector::ZeroVector;
			Driver.PrevPoseLoc = PoseLoc;
			Driver.PrevPoseVel = FVector::ZeroVector;
			Driver.PrevPoseRotation = PoseRotation;
			Driver.PrevPoseAngularVelocity = FVector::ZeroVector;
			Driver.bHasVelocity = false;
			Driver.bHasPoseVelocity = false;
		}
		else
		{
			// 월드 운동 = 액터 이동과 애니메이션을 합친 이 구동점의 실제 가속.
			FVector WorldAcceleration = FVector::ZeroVector;
			FVector WorldAngularAcceleration = FVector::ZeroVector;
			if (bTeleported)
			{
				Driver.PrevLoc = Pivot;
				Driver.PrevRotation = Rotation;
				Driver.bHasVelocity = false;
			}
			else
			{
				MeasureMotion(Pivot, Rotation, Dt, TeleportDistance, Driver.bHasVelocity, Driver.PrevLoc, Driver.PrevVel,
					Driver.PrevRotation, Driver.PrevAngularVelocity,
					WorldAcceleration, WorldAngularAcceleration);
			}
			Acceleration = ComponentInertiaWeight * WorldAcceleration;
			AngularAcceleration = ComponentInertiaWeight * WorldAngularAcceleration;
			if (bSkeletal)
			{
				// 애니메이션 몫 = 컴포넌트 공간 포즈의 가속. 액터 몫 = 나머지(월드 − 애니메이션).
				// 두 배율이 1이면 실제 가속 그대로다.
				FVector PoseAcceleration;
				FVector PoseAngularAcceleration;
				MeasureMotion(PoseLoc, PoseRotation, Dt, TeleportDistance, Driver.bHasPoseVelocity, Driver.PrevPoseLoc, Driver.PrevPoseVel,
					Driver.PrevPoseRotation, Driver.PrevPoseAngularVelocity,
					PoseAcceleration, PoseAngularAcceleration);
				if (PoseAcceleration.Size() > PoseSnapAcceleration || PoseAngularAcceleration.Size() > PoseSnapAngularAcceleration)
				{
					// 자세가 건너뛰었다(PoseSnapAcceleration): 월드 가속에도 같은 건너뜀이 들어 있으므로 둘 다 버린다. 이번 틱에 잰 속도는 건너뛴 속도라
					// 다음 틱에 다시 잰다(안 그러면 다음 틱에 반대 방향 가속이 한 번 더 든다).
					Acceleration = FVector::ZeroVector;
					AngularAcceleration = FVector::ZeroVector;
					Driver.bHasVelocity = false;
					Driver.bHasPoseVelocity = false;
				}
				else
				{
					const float AnimationShare = AnimationInertiaWeight - ComponentInertiaWeight;
					Acceleration += AnimationShare * TargetRotation.RotateVector(PoseAcceleration);
					AngularAcceleration += AnimationShare * TargetRotation.RotateVector(PoseAngularAcceleration);
				}
			}
		}

		// 부위별 조절(ADR-0015): 중력 처짐은 덩어리만(뼈대 부위는 그룹 덮어쓰기로만), 방향 배율은 가속·중력 몫에만,
		// 빠르기·멈춤은 그룹 덮어쓰기가 있으면 그 값.
		const FSoftbodyInertiaGroupOverride* Override = FindInertiaGroupOverride(bSkeletal ? d : 0);
		const float Damping = Override && Override->bOverrideDamping ? Override->Damping : InertiaDamping;
		Driver.Amount = ResolveInertiaGroupAmount(bSkeletal ? d : 0);
		Driver.Reach = Reach * WorldScale;
		ResolveInertiaGroupSag(bSkeletal ? d : 0, Driver.Sag, Driver.SagMax);
		// 빠르기는 그 묶음이 처지는 거리에서 나온다(§2.5): 1 g에 Amount × 처짐 몫 cm 처지는 스프링의 고유 진동수 × Jiggle Speed.
		// 배율 0인 부위는 올리는 값이 0이라 상관없다.
		const float Speed = FMath::Max(JiggleSpeed, 0.1f);
		const float Frequency = SoftbodyInertiaRuntime::WobbleFrequency(Driver.Amount * Driver.Sag, Speed);
		// Jiggle Speed는 리듬만 바꾼다: 움직임 하중(가속·회전 가속)을 같은 비로 나눠 급정지·착지 뒤 첫 튐 높이(≈ ω × 처짐 × 속도 변화 ÷ g)를
		// 그대로 둔다. 기울 때의 중력 몫은 나누지 않는다.
		const float MotionShare = 1.f / Speed;

		FVector LinearLoad;
		FVector AngularLoad;
		SoftbodyInertiaRuntime::ComputeLoadEquilibrium(Acceleration * MotionShare, AngularAcceleration * MotionShare,
			WorldGravity, ResolveInertiaGroupGravityScale(bSkeletal ? d : 0), Driver.ReferenceToWorld,
			LinearLoad, AngularLoad);
		LinearLoad *= InertiaDirectionScale.GetAbs();
		LumpInertialTarget[d] = LinearLoad;
		LinearLoad = ResolveLumpTarget(d, LinearLoad);
		if (!bMotionInit)
		{
			// 첫 프레임은 지금 자세의 중력 처짐에서 가만히 시작한다(누운 채 시작해도 떨어지는 출렁임이 없다).
			Driver.ClearLoad();
			for (int32 k = 0; k < 3; ++k)
			{
				Driver.Load[k] = (float)LinearLoad[k];
			}
		}
		else
		{
			// 손에 쥔 덩어리(ADR-0012 §8)는 접촉 조직의 강성을 넣어 선형 세 축을 함께 적분한다. 놓으면 자유 진동자 그대로.
			const bool bHeld = LumpModes[d].bValid;
			if (bHeld)
			{
				AdvanceLumpModes(Driver, LinearLoad, d, Frequency, Damping, Dt, SurfaceUpdateInterval);
			}
			for (int32 k = 0; k < 3; ++k)
			{
				if (!bHeld)
				{
					SoftbodyInertiaRuntime::AdvanceResponse(Driver.Load[k], Driver.LoadVel[k],
						(float)LinearLoad[k], Frequency, Damping, Dt);
				}
				SoftbodyInertiaRuntime::AdvanceResponse(Driver.Load[3 + k], Driver.LoadVel[3 + k],
					(float)AngularLoad[k], Frequency, Damping, Dt);
			}
		}
		// 상한(InertiaMaxOffset)은 상태가 아니라 브릿지에 올리는 값에 건다(BuildInertiaBridge, ADR-0015 §2.4).
	}
	for (int32 d = NumDrivers; d < MaxInertiaGroups; ++d)
	{
		InertiaDrivers[d].ClearLoad();
	}
	bMotionInit = true;
}

const FSoftbodyInertiaGroupOverride* USoftbodyComponent::FindInertiaGroupOverride(int32 Group) const
{
	if (InertiaGroupOverrides.Num() == 0 || !MeshData || !MeshData->InertiaGroupBones.IsValidIndex(Group))
	{
		return nullptr;
	}
	return InertiaGroupOverrides.Find(MeshData->InertiaGroupBones[Group]);
}

float USoftbodyComponent::ResolveInertiaGroupAmount(int32 Group) const
{
	const FSoftbodyInertiaGroupOverride* Override = FindInertiaGroupOverride(Group);
	return FMath::Max(InertiaAmount, 0.f) * (Override ? FMath::Max(Override->AmountScale, 0.f) : 1.f);
}

void USoftbodyComponent::ResolveInertiaGroupSag(int32 Group, float& OutSag, float& OutSagMax) const
{
	// 옛 굽기(처짐 몫을 적기 전)는 1, 1 — 칠한 살 가운데 가장 무른 점에 맞춘 눈금 그대로다(Validate Setup이 Reapply Paint를 권한다).
	OutSag = 1.f;
	OutSagMax = 1.f;
	if (MeshData && MeshData->InertiaGroupSag.IsValidIndex(Group) && MeshData->InertiaGroupSagMax.IsValidIndex(Group))
	{
		OutSag = FMath::Max(MeshData->InertiaGroupSag[Group], 1e-3f);
		OutSagMax = FMath::Max(MeshData->InertiaGroupSagMax[Group], 0.f);
	}
}

float USoftbodyComponent::ResolveInertiaGroupGravityScale(int32 Group) const
{
	const FSoftbodyInertiaGroupOverride* Override = FindInertiaGroupOverride(Group);
	if (Override && Override->bOverrideGravityScale)
	{
		return FMath::Max(Override->GravityScale, 0.f);
	}
	// 뼈대 부위는 처지지 않는다(ADR-0015 §2.2 — 그 배수였던 Skeleton Gravity Scale은 일지 316에서 지움). 덩어리 표시가 없는 베이크(ADR-0012
	// 전)와 스태틱 메시는 모든 부위를 덩어리로 본다(옛 동작).
	const bool bSkeletal = NumInertiaGroups > 0 && SkinnedTarget.IsValid();
	const bool bLump = !bSkeletal || !MeshData || !MeshData->InertiaGroupLump.IsValidIndex(Group)
		|| MeshData->InertiaGroupLump[Group];
	return bLump ? FMath::Max(InertiaGravityScale, 0.f) : 0.f;
}

TArray<FString> USoftbodyComponent::GetInertiaGroupBoneNames() const
{
	TArray<FString> Names;
	if (MeshData)
	{
		for (const FName& Bone : MeshData->InertiaGroupBones)
		{
			Names.Add(Bone.ToString());
		}
	}
	return Names;
}

void USoftbodyComponent::AdvanceLumpModes(FSoftbodyInertiaDriver& Driver, const FVector& Target, int32 Group,
	float FrequencyHz, float DampingRatio, float Dt, float UpdateInterval) const
{
	// C·L''/ω0² = −(C + sQ)(L − E) − 감쇠·L'. E = 덩어리 평형(ResolveLumpTarget, 이미 Target에 들어 있음), sQ = 그 자리의 살 강성.
	// L − E = V·y로 두면(VᵀCV = I, VᵀsQV = Λ) 모드마다 y'' = −ω²·y − 2ζω·y', ω = ω0·√(1 + Λ).
	// 쥔 조직은 뿌리와 같은 살이라 손실(감쇠비 ζ)이 같다(§8.1): 쥐면 진동수가 올라 남는 떨림이 작고 짧아지고, 끈적하게 늘어지지 않는다.
	// 접촉이 없으면(Λ = 0) 자유 진동자와 같다.
	// 한 주기가 표면 장 갱신 네 번보다 짧은 모드(ω × 갱신 간격 > π/2 — 한 갱신에 90° 넘게 돈다)는 진동자로 적분하지 않고 벗어난 몫이 한 갱신 시간에
	// 걸쳐 줄어든다(속도 0, 일지 337·ADR-0040 §4.4). 평형은 갱신마다 한 번, 몇 갱신 늦게 오는데 그런 떨림은 다음 평형이 오기 전에 방향이 바뀌어 늦게 온
	// 평형이 떨림을 키웠다(모의: 지금 적분은 15~46 Hz 쥔 모드가 늦은 고리 이득 0.6부터 커지고, 이 규칙이면 ±0.9까지 가라앉는다 — Inertia Amount 0.5의
	// 손바닥 쥠이 16~22 Hz). 그 떨림은 갱신 빈도로는 그릴 수도 없다. 문턱을 넘나들어도 자리가 튀지 않는다.
	const double Interval = FMath::Max((double)UpdateInterval, (double)Dt);
	const FLumpModes& Modes = LumpModes[Group];
	const double W0 = 2.0 * PI * FMath::Max((double)FrequencyHz, 0.01);
	const double Zeta = FMath::Clamp((double)DampingRatio, 0.0, 0.9999);
	double Offset[3];
	double Velocity[3];
	for (int32 k = 0; k < 3; ++k)
	{
		Offset[k] = (double)Driver.Load[k] - Target[k];
		Velocity[k] = (double)Driver.LoadVel[k];
	}
	double COffset[3] = {};
	double CVelocity[3] = {};
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			COffset[Row] += Modes.Compliance[Row][Col] * Offset[Col];
			CVelocity[Row] += Modes.Compliance[Row][Col] * Velocity[Col];
		}
	}
	double Y[3] = {};
	double YDot[3] = {};
	for (int32 i = 0; i < 3; ++i)
	{
		for (int32 k = 0; k < 3; ++k)
		{
			Y[i] += Modes.Basis[k][i] * COffset[k];
			YDot[i] += Modes.Basis[k][i] * CVelocity[k];
		}
		const double Lambda = FMath::Max(Modes.Stiffness[i], 0.0);
		const double Omega = W0 * FMath::Sqrt(1.0 + Lambda);
		if (Omega * Interval > 0.5 * PI)
		{
			Y[i] *= FMath::Exp(-(double)Dt / Interval);
			YDot[i] = 0.0;
		}
		else
		{
			SoftbodyInertiaRuntime::AdvanceDampedMode(Y[i], YDot[i], 0.0, Omega, Zeta * Omega, (double)Dt);
		}
	}
	for (int32 k = 0; k < 3; ++k)
	{
		double Back = 0.0;
		double BackVel = 0.0;
		for (int32 i = 0; i < 3; ++i)
		{
			Back += Modes.Basis[k][i] * Y[i];
			BackVel += Modes.Basis[k][i] * YDot[i];
		}
		Driver.Load[k] = (float)(Target[k] + Back);
		Driver.LoadVel[k] = (float)BackVel;
	}
}

bool USoftbodyComponent::ResolveLumpConstants(const FSoftbodyInertiaLump& Lump, double& OutStiffness, double& OutAmount,
	double OutCompliance[3][3]) const
{
	// 덩어리(ADR-0012 §3.3): 살 단단함을 흔들림 크기(1 g에서 가장 무른 점이 밀리는 cm)에 맞추므로 파임과 이동의 비율은 따로 정하는
	// 값 없이 살의 모양에서 나온다. σ = 그 "가장 무른 점"의 원시 무름(살 단단함 1), V = 덩어리 살 부피, m = LumpYield.
	// 기준은 출렁임과 같은 살이다 — 굽기는 Paint.B에 (원시 최댓값 ÷ 기준 살의 원시 처짐)을 곱하므로 σ = 메시 전체 최댓값 ÷
	// InertiaPaintScale = 기준 살(Jiggle 흰색, 3 cm보다 두꺼운 살)의 원시 처짐이다. 옛 굽기는 칠한 살 가운데 가장 무른 점(일지 239), 이 값을
	// 적기 전 굽기(0)는 메시 전체 최댓값이다.
	if (!MeshData)
	{
		return false;
	}
	const bool bSkeletal = NumInertiaGroups > 0 && SkinnedTarget.IsValid();
	const int32 Group = Lump.Group;
	if (Group < 0 || Group >= USoftbodySurfaceField::NumLumpGroups || (bSkeletal ? Group >= NumInertiaGroups : Group != 0))
	{
		return false;
	}
	const FVector AbsScale = GetTargetTransform().GetScale3D().GetAbs();
	const double Scale = (AbsScale.X + AbsScale.Y + AbsScale.Z) / 3.0;
	const double Volume = (double)Lump.Volume * Scale * Scale * Scale;
	double Sigma = 0.0;
	for (const float ResponseScale : MeshData->InertiaResponseScales)
	{
		Sigma = FMath::Max(Sigma, (double)ResponseScale);
	}
	if (MeshData->InertiaPaintScale > 0.f)
	{
		Sigma /= (double)MeshData->InertiaPaintScale;
	}
	Sigma *= Scale * Scale;
	// 이 덩어리의 흔들림 크기(그룹 배율 포함). 0이면 덩어리가 비켜나지 않는다.
	OutAmount = (double)ResolveInertiaGroupAmount(bSkeletal ? Group : 0);
	if (Volume <= 0.0 || Sigma <= 0.0 || OutAmount < 1e-2)
	{
		return false;
	}
	OutStiffness = (double)LumpYield * Sigma / Volume * FMath::Square((double)MeshData->CarrierTexelSize * Scale);
	const FVector3f Mean[3] = {Lump.MeanShapeX, Lump.MeanShapeY, Lump.MeanShapeZ};
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			// C[k][j] = 살을 j로 당겼을 때 평균 이동의 k 성분. 상반 정리로 대칭이라 굽기 어림의 비대칭은 평균으로 지운다.
			OutCompliance[Row][Col] = 0.5 * ((double)Mean[Col][Row] + (double)Mean[Row][Col]);
		}
	}
	return true;
}

FVector USoftbodyComponent::ResolveLumpTarget(int32 Group, const FVector& InertialTarget) const
{
	const FLumpSolution& Solution = LumpSolutions[Group];
	if (!Solution.bValid)
	{
		return InertialTarget;
	}
	// GPU는 몇 프레임 전 관성 목표 T_n으로 풀었다. 그 뒤 바뀐 몫 ΔT는 쥔 살의 강성 H만큼 덜 따라간다(선형 응답):
	// 목표 = E + (C + H)⁻¹·C·ΔT = E + V(I + Λ)⁻¹VᵀC·ΔT (VᵀCV = I, VᵀHV = Λ). 강성이 없으면(미끄러지는 손만) ΔT 그대로.
	const FVector Change = InertialTarget - Solution.TargetUsed;
	FVector Follow = Change;
	const FLumpModes& Modes = LumpModes[Group];
	if (Modes.bValid)
	{
		double CChange[3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			CChange[Row] = Modes.Compliance[Row][0] * Change.X + Modes.Compliance[Row][1] * Change.Y + Modes.Compliance[Row][2] * Change.Z;
		}
		double Y[3];
		for (int32 i = 0; i < 3; ++i)
		{
			Y[i] = (Modes.Basis[0][i] * CChange[0] + Modes.Basis[1][i] * CChange[1] + Modes.Basis[2][i] * CChange[2]) / (1.0 + Modes.Stiffness[i]);
		}
		for (int32 k = 0; k < 3; ++k)
		{
			Follow[k] = Modes.Basis[k][0] * Y[0] + Modes.Basis[k][1] * Y[1] + Modes.Basis[k][2] * Y[2];
		}
	}
	// 수치 안전장(g). 보이는 이동은 InertiaMaxOffset이 부드럽게 막는다.
	return InertialTarget + (Solution.Equilibrium + Follow - InertialTarget).GetClampedToMaxSize(20.0);
}

void USoftbodyComponent::UpdateLumpContact()
{
	// 덩어리(ADR-0012, ADR-0017). GPU가 덩어리마다 에너지
	//   Ψ(E) = ½(E − T)ᵀC(E − T) + s/A² · Σ_칸 [W(d) + 붙은 피부 몫],   d = d0 + A·g·E
	// 를 뉴턴으로 풀어(LumpForceCS·LumpSolveCS) 평형 E와 그 자리의 살 강성 H = s·J를 몇 프레임 늦게 넘긴다. C = 덩어리 살의 부피 평균
	// 무름, T = 관성 목표, g = 칸이 바깥 노멀 쪽으로 밀리는 정도 × 덩어리 무게, W = 살의 힘 법칙(살 두께까지 조일수록 단단해지고
	// 그 너머는 코어 기울기로 곧게 — 손이 덩어리 속으로 들어갈수록 덩어리가 비켜난다). 표면 장이 쉬었으면(콜라이더 없음) 손도 없다.
	const bool bActive = LumpYield > 0.f && bSurfaceFieldDispatched && SurfaceField && MeshData
		&& MeshData->InertiaLumps.Num() > 0 && bEnableInertia && !bContactOnlyValidation && HasInertiaResponseBake();
	if (!bActive)
	{
		for (int32 Group = 0; Group < MaxInertiaGroups; ++Group)
		{
			LumpSolutions[Group] = FLumpSolution();
			LumpModes[Group] = FLumpModes();
		}
		return;
	}
	TArray<double> Values;
	if (!SurfaceField->GetLumpSolution(Values, LumpSerial))
	{
		return; // 새 값이 올 때까지 지난 값을 쓴다
	}
	for (int32 Group = 0; Group < MaxInertiaGroups; ++Group)
	{
		LumpSolutions[Group] = FLumpSolution();
		LumpModes[Group] = FLumpModes();
	}
	for (const FSoftbodyInertiaLump& Lump : MeshData->InertiaLumps)
	{
		double Stiffness;
		double Amount;
		double CMat[3][3];
		if (!ResolveLumpConstants(Lump, Stiffness, Amount, CMat))
		{
			continue;
		}
		const int32 Driver = Lump.Group;
		const double* Out = &Values[Driver * USoftbodySurfaceField::NumLumpOutputs];
		const FVector Equilibrium(Out[0], Out[1], Out[2]);
		const FVector TargetUsed(Out[9], Out[10], Out[11]);
		if (Out[12] < 0.5 || Equilibrium.ContainsNaN() || TargetUsed.ContainsNaN())
		{
			continue;
		}
		LumpSolutions[Driver].bValid = true;
		LumpSolutions[Driver].Equilibrium = Equilibrium;
		LumpSolutions[Driver].TargetUsed = TargetUsed;
		if (CVarDebugLumps.GetValueOnGameThread() > 0)
		{
			const FVector Off = Equilibrium - TargetUsed;
			const double TraceSJ = Out[3] + Out[6] + Out[8];
			const double TraceC = CMat[0][0] + CMat[1][1] + CMat[2][2];
			const FSoftbodyInertiaDriver& Drive = InertiaDrivers[Driver];
			const FVector Shown(Drive.Load[0] * Drive.Amount, Drive.Load[1] * Drive.Amount, Drive.Load[2] * Drive.Amount);
			UE_LOG(LogSoftbodyRuntime, Display,
				TEXT("[lump] %s group %d (%s): E-T (%.3f %.3f %.3f) g |%.3f|, softest point moves %.2f cm, contact/root stiffness %.3f, ")
				TEXT("s %.5f, A %.2f, shown lag (%.2f %.2f %.2f) cm, frame %llu"),
				*GetOwner()->GetName(), Driver, MeshData->InertiaGroupBones.IsValidIndex(Driver) ? *MeshData->InertiaGroupBones[Driver].ToString() : TEXT("?"),
				Off.X, Off.Y, Off.Z, Off.Size(), Amount * Off.Size(), TraceC > 0.0 ? TraceSJ / TraceC : 0.0, Stiffness, Amount,
				Shown.X, Shown.Y, Shown.Z, (unsigned long long)GFrameCounter);
		}

		// 쥔 살의 강성 H(GPU가 받아들인 마지막 점에서, xx xy xz yy yz zz). 미끄러지는 손만 있으면 0이라 모드 없이 자유 진동자.
		const int32 Upper[3][3] = {{3, 4, 5}, {4, 6, 7}, {5, 7, 8}};
		double KMat[3][3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				KMat[Row][Col] = Out[Upper[Row][Col]];
			}
		}
		if (KMat[0][0] + KMat[1][1] + KMat[2][2] <= 0.0)
		{
			continue;
		}
		// 쥔 조직의 모드(§8): sQ·v = Λ·C·v. C^{−1/2}로 대칭으로 바꿔 푼다(V = C^{−1/2}·U, VᵀCV = I).
		double CValues[3];
		double CVectors[3][3];
		SoftbodyInertiaRuntime::SymmetricEigen3(CMat, CValues, CVectors);
		const double CMax = FMath::Max3(CValues[0], CValues[1], CValues[2]);
		if (!(CMax > 0.0) || FMath::Min3(CValues[0], CValues[1], CValues[2]) <= 1e-9 * CMax)
		{
			continue;
		}
		double InvHalf[3][3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				InvHalf[Row][Col] = 0.0;
				for (int32 k = 0; k < 3; ++k)
				{
					InvHalf[Row][Col] += CVectors[Row][k] * CVectors[Col][k] / FMath::Sqrt(CValues[k]);
				}
			}
		}
		double Sym[3][3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				double Sum = 0.0;
				for (int32 a = 0; a < 3; ++a)
				{
					for (int32 b = 0; b < 3; ++b)
					{
						Sum += InvHalf[Row][a] * KMat[a][b] * InvHalf[b][Col];
					}
				}
				Sym[Row][Col] = Sum;
			}
		}
		double Lambda[3];
		double U[3][3];
		SoftbodyInertiaRuntime::SymmetricEigen3(Sym, Lambda, U);
		FLumpModes& Modes = LumpModes[Driver];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				Modes.Compliance[Row][Col] = CMat[Row][Col];
				double V = 0.0;
				for (int32 k = 0; k < 3; ++k)
				{
					V += InvHalf[Row][k] * U[k][Col];
				}
				Modes.Basis[Row][Col] = V;
			}
			Modes.Stiffness[Row] = FMath::Max(Lambda[Row], 0.0);
		}
		Modes.bValid = true;
	}
}

void USoftbodyComponent::BuildInertiaBridge(TArray<FVector4f>& Out) const
{
	Out.Init(FVector4f(0.f, 0.f, 0.f, 0.f), BridgeWidth * BridgeRows);
	// 행0 텍셀0의 A 채널은 스켈레탈 그룹 레이아웃 표지(관성 그룹 수)다. Attr2가 없는
	// 스태틱 머티리얼은 텍스처 값만으로 상위-2 그룹 레이아웃과 안전하게 구분할 수 없다.
	Out[0].W = (float)NumInertiaGroups;
	// 액터 상대 월드 벡터 — LWC 회피 + 액터/본 이동 추종
	const FVector ActorLoc = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
	// 관성 구동점 4텍셀: 흔들림 값(cm, InertiaAmount 적용) 가속·중력 xyz + 회전 x, 회전 yz,
	// 기본 자세→월드 회전, 액터 상대 구동점 위치. 스태틱은 행0 텍셀4부터, 스켈레탈 그룹은 행1부터.
	// 텍셀 1의 z = 1이면 덩어리 묶음(ADR-0018): 머티리얼 관성 음영이 표면 장 덩어리 노멀을 쓸 때 그 몫을 뺀다.
	// 텍셀 3의 w = 메시 스케일(메시 로컬 1 cm의 월드 길이): 관성 음영이 구운 접선(로컬)과 흔들림(월드 cm)을 같은 단위로 맞춘다.
	const float MeshScale = FMath::Max((float)GetTargetTransform().GetMaximumAxisScale(), 1e-4f);
	auto WriteDriver = [this, &Out, &ActorLoc, MeshScale](const FSoftbodyInertiaDriver& Driver, int32 FirstTexel, bool bLump)
	{
		// 큰 움직임 조이기와 상한(ADR-0015 §2.4)은 올리는 값에만 — 진동자 상태는 선형 그대로라 리듬이 바뀌지 않고,
		// 상한에 닿아도 꼭대기만 눌릴 뿐 멈췄다 튀지 않는다.
		float L[6];
		FMemory::Memcpy(L, Driver.Load, sizeof(L));
		SoftbodyInertiaRuntime::SaturateInertiaLoad(L, Driver.Reach, InertiaSaturation);
		SoftbodyInertiaRuntime::LimitInertiaLoad(L, Driver.Reach, Driver.Amount * Driver.SagMax, InertiaMaxOffset, SoftbodyInertiaRuntime::LimitEaseStart);
		const float A = Driver.Amount;
		const FQuat& R = Driver.ReferenceToWorld;
		const FVector Pivot = Driver.Pivot - ActorLoc;
		Out[FirstTexel + 0] = FVector4f(L[0] * A, L[1] * A, L[2] * A, L[3] * A);
		Out[FirstTexel + 1] = FVector4f(L[4] * A, L[5] * A, bLump ? 1.f : 0.f, 0.f);
		Out[FirstTexel + 2] = FVector4f((float)R.X, (float)R.Y, (float)R.Z, (float)R.W);
		Out[FirstTexel + 3] = FVector4f((float)Pivot.X, (float)Pivot.Y, (float)Pivot.Z, MeshScale);
	};
	// 꺼진 컴포넌트는 브릿지가 0이다(SuspendRuntime이 한 번 올린다).
	const bool bInertiaActive = IsActive() && HasInertiaResponseBake() && bEnableInertia && !bContactOnlyValidation;
	if (bInertiaActive && NumInertiaGroups > 0 && SkinnedTarget.IsValid())
	{
		for (int32 g = 0; g < NumInertiaGroups; ++g)
		{
			const bool bLump = MeshData && MeshData->InertiaGroupLump.IsValidIndex(g) && MeshData->InertiaGroupLump[g];
			WriteDriver(InertiaDrivers[g], BridgeWidth + g * 4, bLump);
		}
	}
	else if (bInertiaActive)
	{
		// 스태틱은 덩어리 칸 값이 있으면 메시 전체가 덩어리 칸이다(덩어리 힘은 없다, 일지 296). 표면 장의 덩어리 노멀과 같은 조건이다.
		WriteDriver(InertiaDrivers[0], 4, MeshData && !MeshData->CarrierLumpTex.IsNull());
	}
}

void USoftbodyComponent::UploadContactTexture()
{
	if (!ContactTexture)
	{
		return;
	}
	TArray<FVector4f> Bridge;
	BuildInertiaBridge(Bridge);
	TArray<FFloat16Color> Data;
	Data.Reserve(Bridge.Num());
	for (const FVector4f& Texel : Bridge)
	{
		Data.Add(FFloat16Color(FLinearColor(Texel.X, Texel.Y, Texel.Z, Texel.W)));
	}

	// 지난 프레임 값을 먼저 올린다(처음에는 이번 값 = 속도 0).
	if (PreviousContactTexture)
	{
		UploadFloat16Texture(PreviousContactTexture, LastContactData.Num() == Data.Num() ? LastContactData : Data,
			BridgeWidth, BridgeRows);
	}
	UploadFloat16Texture(ContactTexture, Data, BridgeWidth, BridgeRows);
	LastContactData = MoveTemp(Data);
}

void USoftbodyComponent::PushMaterialParams()
{
	const FTransform TargetTransform = GetTargetTransform();
	// 2 = 이번 프레임 표면 장이 덩어리 노멀도 썼다(ADR-0018): 머티리얼이 덩어리 음영을 거기서 읽고 관성 음영에서 뺀다.
	// 표면 장이 돌지만 덩어리 노멀이 없으면 1 — 관성 음영이 덩어리까지 그린다. 0 = 접촉 없음(캐리어 없음·접촉 끔·콜라이더 없이 복원이
	// 끝나 비운 채 쉼): 머티리얼이 표면 장을 읽지 않는다(값이 어차피 0이다, ADR-0020).
	// 꺼진 컴포넌트(SuspendRuntime)는 접촉·관성 둘 다 0 — 정점 셰이더가 두 갈래를 건너뛴다.
	// Contact Detail Full이면 0.25를 더한다(1.25·2.25, 일지 361): 머티리얼은 Full에서만 함몰 벽을 둘레 4x4 칸으로 세운다(셰이더 SB_FullDetailOn).
	// 머티리얼의 다른 비교(> 0.5·> 1.5)는 정수 부분만 본다 — 머티리얼 함수 입력을 늘리지 않으려고 한 값에 묶었다.
	const bool bFieldLive = IsActive() && IsSurfaceFieldMode() && !SurfaceField->IsCleared();
	const float SurfaceFieldOn = bFieldLive
		? (bSurfaceFieldDispatched && SurfaceField->HasLumpNormals() ? 2.f : 1.f) + (ContactDetail == ESoftbodyContactDetail::Full ? 0.25f : 0.f)
		: 0.f;

	// 정규화 응답장 q는 월드 cm 단위다. 액터 스케일을 다시 곱하면 InertiaMaxOffset의
	// 월드 상한이 깨지므로 로컬 응답 방향에는 회전만 적용한다.
	const FMatrix L2W = TargetTransform.ToMatrixNoScale();
	// 나나이트 테셀레이션은 Displacement를 메시 로컬 노멀로 밀어 컴포넌트 스케일이 곱해진다 — 월드 cm로 맞춘다.
	const float TessScale = FMath::Max((float)TargetTransform.GetMaximumAxisScale(), 1e-4f);
	const FLinearColor ColX((float)L2W.M[0][0], (float)L2W.M[1][0], (float)L2W.M[2][0], 0.f);
	const FLinearColor ColY((float)L2W.M[0][1], (float)L2W.M[1][1], (float)L2W.M[2][1], 0.f);
	const FLinearColor ColZ((float)L2W.M[0][2], (float)L2W.M[1][2], (float)L2W.M[2][2], 0.f);
	const FLinearColor Columns[3] = {ColX, ColY, ColZ};
	const bool bResponseActive =
		IsActive() && !bContactOnlyValidation && bEnableInertia && HasInertiaResponseBake();

	for (UMaterialInstanceDynamic* MID : MIDs)
	{
		MID->SetScalarParameterValue(
			Param_InertiaResponseCount, bResponseActive ? (float)MeshData->GetInertiaResponseRows() : 0.f);
		MID->SetScalarParameterValue(Param_MaxDisplacement, MaxDisplacement);
		MID->SetScalarParameterValue(Param_SurfaceFieldOn, SurfaceFieldOn);
		// 검수 시각화 토글 — MF_SoftbodyDebugVis의 lerp(BaseColor, 검수색, VisWeight).
		// 매 틱 밀어 PIE 디테일 패널에서 실시간 토글이 된다.
		MID->SetScalarParameterValue(Param_VisWeight, bDebugVis ? 1.f : 0.f);
		MID->SetScalarParameterValue(Param_VisMode,
			(float)FMath::Clamp(DebugVisMode, 0, NumDebugVisModes - 1));
		MID->SetVectorParameterValue(Param_L2WX, ColX);
		MID->SetVectorParameterValue(Param_L2WY, ColY);
		MID->SetVectorParameterValue(Param_L2WZ, ColZ);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			MID->SetVectorParameterValue(Param_PrevL2W[Axis], bHasLastL2W ? LastL2W[Axis] : Columns[Axis]);
		}
		MID->SetScalarParameterValue(Param_SurfaceAttachRange, SurfaceAttachRange);
		// 머티리얼(인스턴스)의 변위 크기·중심 그대로 — 저작자는 머티리얼 한 곳만 고친다(ADR-0006 §3.6).
		const FDisplacementScaling Scaling = MID->GetDisplacementScaling();
		MID->SetScalarParameterValue(Param_TessMagnitude, Scaling.Magnitude * TessScale);
		MID->SetScalarParameterValue(Param_TessCenter, Scaling.Center);
	}
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		LastL2W[Axis] = Columns[Axis];
	}
	bHasLastL2W = true;
}
