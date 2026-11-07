// Copyright (c) 2026 wjsekrud

#include "SoftbodyAuthoringData.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Misc/EngineVersionComparison.h"
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
#include "Materials/MaterialExpressionUtils.h"  // 5.6·5.7은 엔진 Internal 폴더라 프로젝트 플러그인에서 못 읽는다(쓰는 곳도 5.8만)
#endif
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "SoftbodyBakeLibrary.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyColliderField.h"
#include "SoftbodyMeshData.h"
#include "Serialization/CustomVersion.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "SoftbodyAuthoring"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyAuthoring, Log, All);

namespace
{
	/**
	 * 실행 버튼이 오류로 끝났음을 에디터 오른쪽 아래에 알린다. Output Log의 오류 줄만으로는 진행 창이 닫히면 성공처럼 보인다.
	 * 실패한 실행은 저장하지 않는다 — 바뀐 것은 메모리에만 남는다.
	 */
	void NotifyRunFailed(const USoftbodyAuthoringData& Asset, const FText& RunName, bool bSaveSkipped)
	{
		const FText Text = bSaveSkipped
			? FText::Format(LOCTEXT("RunFailedNotSaved", "{0} failed on {1}, so nothing was saved. The errors are in the Output Log."),
				RunName, FText::FromString(Asset.GetName()))
			: FText::Format(LOCTEXT("RunFailed", "{0} failed on {1}. The errors are in the Output Log."), RunName, FText::FromString(Asset.GetName()));
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 10.f;
		Info.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}

	/**
	 * 칠 텍스처를 다시 가져오면 알림의 버튼으로 칠을 적용한다. 확인 없이 돌면 측정에 드는 칠이 바뀔 때 몇 분짜리 Run Bake와 저장까지 하게 된다.
	 * 측정이 읽은 칠이 그대로면 접촉 지도만 다시 구우면 되므로 Reapply Paint를 권한다.
	 */
	void NotifyPaintReimported(USoftbodyAuthoringData& Asset, const UTexture2D& Texture, bool bNeedsBake)
	{
		const FText AssetName = FText::FromString(Asset.GetName());
		const FText TextureName = FText::FromString(Texture.GetName());
		FNotificationInfo Info(bNeedsBake
			? FText::Format(LOCTEXT("PaintReimportedBake", "{0} changed. Run Bake on {1} applies the new paint (it takes a few minutes)."), TextureName, AssetName)
			: FText::Format(LOCTEXT("PaintReimportedReapply", "{0} changed. Reapply Paint on {1} applies the new paint."), TextureName, AssetName));
		Info.bFireAndForget = false;
		const TSharedRef<TWeakPtr<SNotificationItem>> Shown = MakeShared<TWeakPtr<SNotificationItem>>();
		const auto Close = [Shown]()
		{
			if (TSharedPtr<SNotificationItem> Item = Shown->Pin())
			{
				Item->SetCompletionState(SNotificationItem::CS_None);
				Item->ExpireAndFadeout();
			}
		};
		const TWeakObjectPtr<USoftbodyAuthoringData> WeakAsset(&Asset);
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			bNeedsBake ? LOCTEXT("PaintReimportedRunBake", "Run Bake") : LOCTEXT("PaintReimportedRunReapply", "Reapply Paint"), FText::GetEmpty(),
			FSimpleDelegate::CreateLambda([Close, WeakAsset, bNeedsBake]()
			{
				Close();
				if (USoftbodyAuthoringData* Target = WeakAsset.Get())
				{
					if (bNeedsBake)
					{
						Target->RunBake();
					}
					else
					{
						Target->RunPaintConvert();
					}
				}
			}),
			SNotificationItem::CS_Pending));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("PaintReimportedLater", "Later"), FText::GetEmpty(),
			FSimpleDelegate::CreateLambda(Close), SNotificationItem::CS_Pending));
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Pending);
			*Shown = Item;
		}
	}

	/** 슬롯 이름 → 현재 메시의 슬롯 인덱스 (리임포트 후에도 이름으로 재결속) */
	int32 FindSlotIndexByName(const UObject* Mesh, FName SlotName)
	{
		if (const USkeletalMesh* SK = Cast<USkeletalMesh>(Mesh))
		{
			const TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();
			for (int32 i = 0; i < Mats.Num(); ++i)
			{
				if (Mats[i].MaterialSlotName == SlotName)
				{
					return i;
				}
			}
		}
		else if (const UStaticMesh* SM = Cast<UStaticMesh>(Mesh))
		{
			const TArray<FStaticMaterial>& Mats = SM->GetStaticMaterials();
			for (int32 i = 0; i < Mats.Num(); ++i)
			{
				if (Mats[i].MaterialSlotName == SlotName)
				{
					return i;
				}
			}
		}
		return INDEX_NONE;
	}

	/** 대상 메시의 슬롯 머티리얼(비어 있는 슬롯은 건너뜀) */
	TArray<UMaterialInterface*> SlotMaterials(const UObject* Mesh)
	{
		TArray<UMaterialInterface*> Out;
		if (const USkeletalMesh* SK = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& M : SK->GetMaterials())
			{
				if (M.MaterialInterface)
				{
					Out.Add(M.MaterialInterface);
				}
			}
		}
		else if (const UStaticMesh* SM = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& M : SM->GetStaticMaterials())
			{
				if (M.MaterialInterface)
				{
					Out.Add(M.MaterialInterface);
				}
			}
		}
		return Out;
	}

	/** 슬롯 중 소프트바디 머티리얼(표면 캐리어를 읽는)이 붙은 것이 하나라도 있는가 */
	bool AnySlotReadsCarrier(const UObject* Mesh)
	{
		for (const UMaterialInterface* Material : SlotMaterials(Mesh))
		{
			if (SoftbodyBakeShared::ReadsCarrier(Material))
			{
				return true;
			}
		}
		return false;
	}
}

FString USoftbodyAuthoringData::ResolveOutputFolder() const
{
	return OutputFolder.IsEmpty() ? FPackageName::GetLongPackagePath(GetOutermost()->GetName()) : OutputFolder;
}

TArray<FString> USoftbodyAuthoringData::GetSkeletonBoneNames() const
{
	TArray<FString> Names;
	if (const USkeletalMesh* Skel = Cast<USkeletalMesh>(TargetMesh))
	{
		const FReferenceSkeleton& Ref = Skel->GetRefSkeleton();
		for (int32 i = 0; i < Ref.GetNum(); ++i)
		{
			Names.Add(Ref.GetBoneName(i).ToString());
		}
	}
	return Names;
}

TArray<FString> USoftbodyAuthoringData::GetMaterialSlotNameOptions() const
{
	TArray<FString> Names;
	if (const USkeletalMesh* SK = Cast<USkeletalMesh>(TargetMesh))
	{
		for (const FSkeletalMaterial& M : SK->GetMaterials())
		{
			Names.Add(M.MaterialSlotName.ToString());
		}
	}
	else if (const UStaticMesh* SM = Cast<UStaticMesh>(TargetMesh))
	{
		for (const FStaticMaterial& M : SM->GetStaticMaterials())
		{
			Names.Add(M.MaterialSlotName.ToString());
		}
	}
	return Names;
}

// --- 실행 ---

void USoftbodyAuthoringData::BeginRun()
{
	// 실행 버튼마다 BeginRun 바로 뒤에 ON_SCOPE_EXIT로 RunSerial을 올린다(일찍 끝나도) — Simulation 탭이 새 굽기로 다시 세운다(일지 234).
	// 굽기 전부터 저장하지 않은 패키지는 굽기가 저장하지 않는다(SaveOutputs) — 사용자가 하던 다른 작업을 말없이 저장하지 않게.
	DirtyBeforeRun.Reset();
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->IsDirty())
		{
			DirtyBeforeRun.Add(It->GetFName());
		}
	}
}

void USoftbodyAuthoringData::RunBake()
{
	BeginRun();
	ON_SCOPE_EXIT { ++RunSerial; };
	const int32 NumStages = 2 + (bCreateSoftbodyMaterial ? 1 : 0) + (bBakeSurfaceCarrier ? 1 : 0)
		+ (bBakeColliderField ? 1 : 0) + (bConfigureNaniteTessellation ? 1 : 0) + (bSaveAfterBake ? 1 : 0);
	FScopedSlowTask SlowTask((float)NumStages, LOCTEXT("RunBake", "Softbody: Run Bake"));
	SlowTask.MakeDialog();

	const double Start = FPlatformTime::Seconds();
	bool bOk = true;
	{
		// 메시·머티리얼·이 에셋을 바꾼 것을 Edit > Undo 한 번으로 되돌릴 수 있게 한 묶음으로. 저장은 묶음을 닫은 뒤에 한다.
		FScopedTransaction Transaction(LOCTEXT("RunBakeTransaction", "Softbody: Run Bake"));
		Modify();
		SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageMeasured", "Measuring the flesh (takes minutes)"));
		if (!BakeMeasuredStage())
		{
			NotifyRunFailed(*this, LOCTEXT("RunBakeName", "Run Bake"), bSaveAfterBake);
			return;
		}
		// 칠 단계는 Slot Masks가 비어도 돈다: Physics Driven Bones·Hard Surface 조각·출렁임 기준·(스태틱) 칠 텍스처가 지금 값이 되어야 같은
		// 버튼이 같은 결과를 낸다(전에는 비면 건너뛰어 옛 값이 남았다). 바로 뒤 머티리얼 단계가 입힐 슬롯도 출렁임 기준에 센다.
		SlowTask.EnterProgressFrame(1.f, LOCTEXT("StagePaint", "Converting the slot mask paint"));
		bOk &= PaintConvertStage(PendingSoftbodySlots());
		if (!bBakeSurfaceCarrier && BakedData && BakedData->HasSurfaceCarrier() && BakedData->bPaintVolume)
		{
			UE_LOG(LogSoftbodyAuthoring, Display,
				TEXT("%s: Bake Contact Map is off, so the contact map keeps the old Depth and Volume paint until you press Rebake Contact Map."),
				*GetName());
		}
		if (bCreateSoftbodyMaterial)
		{
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageMaterial", "Creating the Softbody material"));
			bOk &= CreateMaterialStage();
		}
		if (bBakeSurfaceCarrier)
		{
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageCarrier", "Baking the contact map"));
			bOk &= SurfaceCarrierStage();
		}
		if (bBakeColliderField)
		{
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageField", "Baking the shape field"));
			bOk &= ColliderFieldStage();
		}
		if (bConfigureNaniteTessellation)
		{
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageNanite", "Setting up Nanite tessellation"));
			bOk &= NaniteTessellationStage();
		}
	}
	// 단계 하나라도 실패하면 저장하지 않는다 — 메시·데이터·머티리얼이 서로 어긋난 채 디스크에 남지 않게.
	if (bOk && bSaveAfterBake)
	{
		SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageSave", "Saving"));
		SaveOutputs();
	}
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Run Bake %s (%.0f s) -> %s"), *GetName(),
		bOk ? TEXT("finished") : (bSaveAfterBake ? TEXT("failed, nothing was saved (see the errors above)") : TEXT("failed (see the errors above)")),
		FPlatformTime::Seconds() - Start, *GetPathNameSafe(BakedData));
	if (!bOk)
	{
		NotifyRunFailed(*this, LOCTEXT("RunBakeName", "Run Bake"), bSaveAfterBake);
	}
}

void USoftbodyAuthoringData::RunPaintConvert()
{
	if (bLegacyPaintLayout)
	{
		ConvertLegacyPaint();
	}
	// 네 칠 모두 측정(출렁임)에 든다(ADR-0038 §3.6). 측정이 읽은 칠이 지금 칠과 다르면 캐리어만 다시 구우면 출렁임의 뼈와 접촉의 두께가
	// 어긋나므로(원칙 P04) Run Bake로 돈다.
	if (BakedData && TargetMesh && SoftbodyBakeShared::HashMeasuredPaint(TargetMesh, SlotMasks) != BakedData->MeasuredPaintHash)
	{
		UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: the paint changed since the flesh was last measured, so Reapply Paint runs Run Bake."), *GetName());
		RunBake();
		return;
	}
	BeginRun();
	ON_SCOPE_EXIT { ++RunSerial; };
	bool bOk = true;
	{
		FScopedTransaction Transaction(LOCTEXT("RunPaintConvertTransaction", "Softbody: Reapply Paint"));
		Modify();
		bOk = PaintConvertStage();
		// 눌리는 정도(살 두께에 곱한다, ADR-0016 §5)와 부피 보존(ADR-0024)은 표면 캐리어가 칸마다 구워 두는 값이라, 이미 구운 캐리어가
		// 있으면 같이 다시 굽는다. 안 그러면 마스크를 고쳐 다시 변환해도 눌리는 모양은 옛 칠 그대로다.
		if (bOk && BakedData && BakedData->CarrierUVTex)
		{
			bOk = SurfaceCarrierStage();
		}
	}
	if (!bOk)
	{
		NotifyRunFailed(*this, LOCTEXT("ReapplyPaintName", "Reapply Paint"), bSaveAfterBake);
		return;
	}
	if (bSaveAfterBake)
	{
		SaveOutputs();
	}
}

void USoftbodyAuthoringData::RunCreateMaterial()
{
	BeginRun();
	ON_SCOPE_EXIT { ++RunSerial; };
	FScopedSlowTask SlowTask(2.f, LOCTEXT("StageMaterial", "Creating the Softbody material"));
	SlowTask.MakeDialog();
	SlowTask.EnterProgressFrame(1.f);
	bool bOk = true;
	{
		FScopedTransaction Transaction(LOCTEXT("RunCreateMaterialTransaction", "Softbody: Recreate Material"));
		Modify();
		bOk = CreateMaterialStage();
		// 새 머티리얼에 CarrierUV를 잇고 봉투를 눌리는 슬롯으로 좁히려면 캐리어를 다시 굽는다
		if (bOk && BakedData && BakedData->CarrierUVTex)
		{
			SlowTask.EnterProgressFrame(1.f, LOCTEXT("StageCarrier", "Baking the contact map"));
			bOk = SurfaceCarrierStage();
		}
	}
	if (!bOk)
	{
		NotifyRunFailed(*this, LOCTEXT("RecreateMaterialName", "Recreate Material"), bSaveAfterBake);
		return;
	}
	if (bSaveAfterBake)
	{
		SaveOutputs();
	}
}

void USoftbodyAuthoringData::RunSurfaceCarrier()
{
	BeginRun();
	ON_SCOPE_EXIT { ++RunSerial; };
	FScopedSlowTask SlowTask(1.f, LOCTEXT("StageCarrier", "Baking the contact map"));
	SlowTask.MakeDialog();
	SlowTask.EnterProgressFrame(1.f);
	bool bOk = false;
	{
		FScopedTransaction Transaction(LOCTEXT("RunSurfaceCarrierTransaction", "Softbody: Rebake Contact Map"));
		Modify();
		bOk = SurfaceCarrierStage();
	}
	if (!bOk)
	{
		NotifyRunFailed(*this, LOCTEXT("RebakeContactMapName", "Rebake Contact Map"), bSaveAfterBake);
	}
	else if (bSaveAfterBake)
	{
		SaveOutputs();
	}
}

void USoftbodyAuthoringData::RunColliderField()
{
	BeginRun();
	ON_SCOPE_EXIT { ++RunSerial; };
	FScopedSlowTask SlowTask(1.f, LOCTEXT("StageField", "Baking the shape field"));
	SlowTask.MakeDialog();
	SlowTask.EnterProgressFrame(1.f);
	bool bOk = false;
	{
		FScopedTransaction Transaction(LOCTEXT("RunColliderFieldTransaction", "Softbody: Rebake Shape Field"));
		Modify();
		bOk = ColliderFieldStage();
	}
	if (!bOk)
	{
		NotifyRunFailed(*this, LOCTEXT("RebakeShapeFieldName", "Rebake Shape Field"), bSaveAfterBake);
	}
	else if (bSaveAfterBake)
	{
		SaveOutputs();
	}
}

void USoftbodyAuthoringData::RunNaniteTessellation()
{
	BeginRun();
	ON_SCOPE_EXIT { ++RunSerial; };
	FScopedSlowTask SlowTask(1.f, LOCTEXT("StageNanite", "Setting up Nanite tessellation"));
	SlowTask.MakeDialog();
	SlowTask.EnterProgressFrame(1.f);
	bool bOk = false;
	{
		FScopedTransaction Transaction(LOCTEXT("RunNaniteTessellationTransaction", "Softbody: Reapply Nanite Tessellation"));
		Modify();
		bOk = NaniteTessellationStage();
	}
	if (!bOk)
	{
		NotifyRunFailed(*this, LOCTEXT("ReapplyNaniteName", "Reapply Nanite Tessellation"), bSaveAfterBake);
	}
	else if (bSaveAfterBake)
	{
		SaveOutputs();
	}
}

namespace
{
	// 저작 에셋 저장 꼴의 판(일지 298): 1 = 슬롯 칠 고정 배치(ADR-0038). 그 전 판으로 저장된 에셋의 슬롯 칠은 옛 배치다(ConvertLegacyPaint가 옮긴다).
	// 2 = Flesh Reach 기본값 3(일지 316): 그 전 판 에셋에 적히지 않은 Flesh Reach는 옛 기본값 0이다(Serialize가 지킨다).
	const FGuid AuthoringVersionGuid(0x5B0D7A11, 0x29844C3E, 0x9F1A2D38, 0x0298A001);
	enum : int32
	{
		AuthoringVersionInitial = 0,
		AuthoringVersionFixedPaint = 1,
		AuthoringVersionFleshReachDefault = 2,
		AuthoringVersionLatest = AuthoringVersionFleshReachDefault,
	};
	FCustomVersionRegistration GRegisterAuthoringVersion(AuthoringVersionGuid, AuthoringVersionLatest, TEXT("SoftbodyAuthoring"));
}

void USoftbodyAuthoringData::Serialize(FArchive& Ar)
{
	// Flesh Reach 기본값 0 → 3(일지 316): 이 판 전에 저장된 에셋은 Flesh Reach를 적지 않았으면(그때 기본값 0이라 저장되지 않았다) 0을 지킨다
	// — 다시 구울 때 조용히 바뀌지 않게. 패키지에서 읽을 때만 본다(되돌리기는 건너뛰고, 링커가 없는 새 에셋·복제는 지금 판이라 걸리지 않는다).
	if (Ar.IsLoading() && !Ar.IsTransacting() && GetLinker() && GetLinkerCustomVersion(AuthoringVersionGuid) < AuthoringVersionFleshReachDefault)
	{
		FleshReach = 0.f;
	}
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(AuthoringVersionGuid);
}

bool USoftbodyAuthoringData::ConvertLegacyPaint()
{
	if (!bLegacyPaintLayout)
	{
		UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: the slot paint already uses the fixed layout (R Depth, G Jiggle, B Volume and A Flex)"), *GetName());
		return false;
	}
	Modify();
	TArray<FString> Notes;
	for (TPair<FName, FSoftbodyPaintSlotMask>& Pair : SlotMasks)
	{
		SoftbodyBakeShared::ConvertLegacySlotMask(Pair.Value, Pair.Key, Notes);
	}
	bLegacyPaintLayout = false;
	MarkPackageDirty();
	for (const FString& Line : Notes)
	{
		UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: %s"), *GetName(), *Line);
	}
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: slot paint moved to the fixed layout (%d slots: R Depth, G Jiggle, B Volume and A Flex). Run Bake applies it."),
		*GetName(), SlotMasks.Num());
	return true;
}

void USoftbodyAuthoringData::PostLoad()
{
	Super::PostLoad();
	bStaticTarget = Cast<UStaticMesh>(TargetMesh.Get()) != nullptr;
	if (GetLinkerCustomVersion(AuthoringVersionGuid) < AuthoringVersionFixedPaint && SlotMasks.Num() > 0)
	{
		bLegacyPaintLayout = true;
	}
	// 옛 "Exclude (Make Lump)" 체크 → Role = Lump. 굽기 값(BoneExcluded…)이 같아 다시 굽지 않아도 된다.
	for (FSoftbodyRigidBoneOverride& Override : RigidBoneOverrides)
	{
		if (Override.bExclude)
		{
			Override.Role = ESoftbodyBoneRole::Lump;
			Override.bExclude = false;
		}
	}
}

#if WITH_EDITOR
void USoftbodyAuthoringData::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	bStaticTarget = Cast<UStaticMesh>(TargetMesh.Get()) != nullptr;
	// 압축성 값(ADR-0037 §3.3)은 굽지 않고 구운 데이터로 바로 옮긴다 — 함몰·솟음이 그 자리에서 바뀐다(덩어리 모양은 Run Bake 뒤).
	// 겉껍질 장력은 컴포넌트 값이다(굽기가 쓰지 않는다).
	if (Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(USoftbodyAuthoringData, VolumePreservation))
	{
		SyncMaterialValues();
	}
}
#endif

void USoftbodyAuthoringData::SyncMaterialValues()
{
	const float Volume = FMath::Clamp(VolumePreservation, 0.f, 1.f);
	if (BakedData && BakedData->VolumePreservation != Volume)
	{
		BakedData->Modify();
		BakedData->VolumePreservation = Volume;
	}
}

bool USoftbodyAuthoringData::ResolveStaticCoreSpheres(UStaticMesh* Static, TArray<FVector4>& OutSpheres, bool bLogEmpty, bool bLogFailure) const
{
	if (StaticCore == ESoftbodyStaticCore::Custom)
	{
		for (const FSoftbodyStaticRigidSphere& Sphere : StaticRigidSpheres)
		{
			if (Sphere.Radius > UE_KINDA_SMALL_NUMBER)
			{
				OutSpheres.Add(FVector4(Sphere.Center, Sphere.Radius));
			}
		}
		if (OutSpheres.Num() == 0 && bLogEmpty)
		{
			UE_LOG(LogSoftbodyAuthoring, Error,
				TEXT("%s: Core Placement is Custom Spheres but Rigid Core Spheres has no sphere. Add one, or set Core Placement to Center or Bottom. The bake did not start."),
				*GetName());
		}
		return OutSpheres.Num() > 0;
	}
	// 살을 채우는 메시(속 메시가 있으면 그 메시)의 모양에서 — 측정 굽기와 같은 안팎·같은 칸
	UStaticMesh* Volume = VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr;
	UStaticMesh* Solid = Volume ? Volume : Static;
	const FMeshDescription* MD = Solid ? Solid->GetMeshDescription(0) : nullptr;
	const int32 Num = MD ? SoftbodyBakeShared::CollectStaticCoreSpheres(*MD, StaticCore == ESoftbodyStaticCore::Bottom, CellSizeFrac,
		DefaultRigidShrink, OutSpheres) : 0;
	if (Num == 0 && bLogFailure)
	{
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: Could not place the rigid core (Core Placement %s): %s has no closed inside. Use a closed mesh, set Volume Mesh, or use Custom Spheres."),
			*GetName(), StaticCore == ESoftbodyStaticCore::Bottom ? TEXT("Bottom") : TEXT("Center"), *GetNameSafe(Solid));
	}
	return Num > 0;
}

TMap<FName, float> USoftbodyAuthoringData::BoneOverrideMap() const
{
	const auto ValueOf = [](const FSoftbodyRigidBoneOverride& O)
	{
		switch (O.Role)
		{
		case ESoftbodyBoneRole::Lump:
			return O.bIncludeChildren ? SoftbodyBakeShared::BoneExcludedWithChildren : SoftbodyBakeShared::BoneExcludedAlone;
		case ESoftbodyBoneRole::Accessory:
			return O.bIncludeChildren ? SoftbodyBakeShared::BoneAccessoryWithChildren : SoftbodyBakeShared::BoneAccessoryAlone;
		default:
			return O.Shrink;
		}
	};
	TMap<FName, const FSoftbodyRigidBoneOverride*> Entries;
	for (const FSoftbodyRigidBoneOverride& O : RigidBoneOverrides)
	{
		if (!O.BoneName.IsNone())
		{
			Entries.Add(O.BoneName, &O);
		}
	}
	TMap<FName, float> PerBone;
	const USkeletalMesh* Skel = Cast<USkeletalMesh>(TargetMesh.Get());
	if (!Skel)
	{
		for (const TPair<FName, const FSoftbodyRigidBoneOverride*>& Pair : Entries)
		{
			PerBone.Add(Pair.Key, ValueOf(*Pair.Value));
		}
		return PerBone;
	}
	// Include Child Bones 굵기는 항목이 없는 아래 본에 펼쳐 적는다(가장 가까운 위 항목이 이긴다) — 굽기는 본마다 값 하나만 본다. 뺀 본은
	// 값(아래까지/그 본만)으로 굽기가 펼친다. 부모가 늘 앞 번호라 한 번 훑는다.
	const FReferenceSkeleton& Ref = Skel->GetRefSkeleton();
	TArray<float> Inherited;
	Inherited.Init(0.f, Ref.GetNum());
	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
	{
		const int32 Parent = Ref.GetParentIndex(Bone);
		Inherited[Bone] = Parent != INDEX_NONE ? Inherited[Parent] : 0.f;
		const FName Name = Ref.GetBoneName(Bone);
		if (const FSoftbodyRigidBoneOverride* const* Entry = Entries.Find(Name))
		{
			const FSoftbodyRigidBoneOverride& O = **Entry;
			PerBone.Add(Name, ValueOf(O));
			if (O.Role == ESoftbodyBoneRole::Bone && O.bIncludeChildren)
			{
				Inherited[Bone] = O.Shrink;
			}
		}
		else if (Inherited[Bone] > 0.f)
		{
			PerBone.Add(Name, Inherited[Bone]);
		}
	}
	return PerBone;
}

TArray<FVector4> USoftbodyAuthoringData::PreviewRigidCore(FString& OutStatus, bool& bOutProblem) const
{
	TArray<FVector4> Spheres;
	bOutProblem = false;
	UStaticMesh* Volume = VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr;
	const FMeshDescription* VolumeMD = Volume ? Volume->GetMeshDescription(0) : nullptr;
	// 살을 채우는 메시 = 측정 칸을 재는 메시(속 메시가 있으면 그것). 칸 = 가장 긴 변 × Measure Cell Size(SoftbodyMeasure와 같다).
	const FMeshDescription* SolidMD = VolumeMD;
	float MinRadiusCells = 0.f;
	if (USkeletalMesh* Skel = Cast<USkeletalMesh>(TargetMesh))
	{
		SolidMD = SolidMD ? SolidMD : Skel->GetMeshDescription(0);
		const TMap<FName, float> PerBone = BoneOverrideMap();
		const TSet<FName> CoreSlots = ResolveCoreSlots();
		const int32 NumLines = SoftbodyBakeShared::CollectCoreSpheres(Skel, Spheres, DefaultRigidShrink,
			PerBone.Num() > 0 ? &PerBone : nullptr, VolumeMD, &CoreSlots);
		if (Spheres.Num() == 0)
		{
			bOutProblem = true;
			OutStatus = TEXT("No bone of the skeleton lies inside the mesh, so Run Bake makes no jiggle. Check that the bones sit inside the mesh.");
		}
		else
		{
			MinRadiusCells = SoftbodyBakeShared::CoreMinRadiusCells;
			OutStatus = FString::Printf(TEXT("Bones: %d spheres along %d bone lines."), Spheres.Num(), NumLines);
			// 구운 데이터가 다른 설정으로 구워졌으면 알린다(일지 239).
			const FString Stale = DescribeStaleCore();
			if (!Stale.IsEmpty())
			{
				bOutProblem = true;
				OutStatus += TEXT(" ") + Stale;
			}
		}
	}
	else if (UStaticMesh* Static = Cast<UStaticMesh>(TargetMesh))
	{
		SolidMD = SolidMD ? SolidMD : Static->GetMeshDescription(0);
		if (!ResolveStaticCoreSpheres(Static, Spheres, false, false))
		{
			bOutProblem = true;
			OutStatus = StaticCore == ESoftbodyStaticCore::Custom
				? FString(TEXT("Rigid Core Spheres has no sphere, so Run Bake will stop. Add one, or set Core Placement to Center or Bottom."))
				: FString::Printf(TEXT("%s has no closed inside, so the core cannot be placed. Use a closed mesh, set Volume Mesh, or use Custom Spheres."),
					*GetNameSafe(Volume ? Volume : Static));
		}
		else if (StaticCore == ESoftbodyStaticCore::Center)
		{
			MinRadiusCells = SoftbodyBakeShared::CoreMinRadiusCells;
			OutStatus = FString::Printf(TEXT("Center: %d spheres through the middle of the shape."), Spheres.Num());
		}
		else if (StaticCore == ESoftbodyStaticCore::Bottom)
		{
			const double Cell = SolidMD ? FMath::Max(SolidMD->ComputeBoundingBox().GetSize().GetMax() * CellSizeFrac, 0.01) : 0.0;
			OutStatus = FString::Printf(TEXT("Bottom: the lowest %.2f cm is held."), 0.5 * Cell);
		}
		else
		{
			OutStatus = FString::Printf(TEXT("Custom Spheres: %d spheres."), Spheres.Num());
		}
	}
	else
	{
		bOutProblem = true;
		OutStatus = TEXT("Set Target Mesh to see its rigid core.");
	}
	// 굽기와 같은 반지름 하한: 가는 속이 격자 점 사이로 토막 나지 않게 측정 굽기가 키우는 만큼 키워 그린다.
	if (MinRadiusCells > 0.f && SolidMD)
	{
		const double MinRadius = MinRadiusCells * FMath::Max(SolidMD->ComputeBoundingBox().GetSize().GetMax() * CellSizeFrac, 0.01);
		for (FVector4& Sphere : Spheres)
		{
			Sphere.W = FMath::Max(Sphere.W, MinRadius);
		}
	}
	return Spheres;
}

// --- 단계 ---

TSet<FName> USoftbodyAuthoringData::ResolveCoreSlots() const
{
	// 측정 굽기는 머티리얼 단계보다 먼저 돈다 — 첫 굽기에는 아직 소프트바디 머티리얼이 없으니 곧 받을 슬롯(Softbody Material Slots)도 센다.
	return SoftbodyBakeShared::CoreSlotNames(Cast<USkeletalMesh>(TargetMesh.Get()),
		bCreateSoftbodyMaterial ? TConstArrayView<FName>(SoftbodyMaterialSlots) : TConstArrayView<FName>());
}

TArray<FName> USoftbodyAuthoringData::PendingSoftbodySlots() const
{
	TArray<FName> Pending;
	if (!bCreateSoftbodyMaterial)
	{
		return Pending;
	}
	// 머티리얼 단계(CreateMaterialStage)와 같은 규칙: 메시에 있는 슬롯만, 이미 소프트바디 머티리얼인 슬롯은 그대로 두므로 지금 머티리얼을 따른다.
	for (const FName SlotName : SoftbodyMaterialSlots)
	{
		const int32 Index = FindSlotIndexByName(TargetMesh, SlotName);
		if (Index == INDEX_NONE)
		{
			continue;
		}
		const UMaterialInterface* Current = nullptr;
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(TargetMesh.Get()))
		{
			Current = Skeletal->GetMaterials()[Index].MaterialInterface;
		}
		else if (const UStaticMesh* Static = Cast<UStaticMesh>(TargetMesh.Get()))
		{
			Current = Static->GetStaticMaterials()[Index].MaterialInterface;
		}
		if (!SoftbodyBakeShared::ReadsCarrier(Current))
		{
			Pending.Add(SlotName);
		}
	}
	return Pending;
}

FString USoftbodyAuthoringData::DescribeStaleCore() const
{
	const USoftbodyMeshData* Baked = BakedData.Get();
	USkeletalMesh* Skel = Cast<USkeletalMesh>(TargetMesh.Get());
	if (!Baked || !Skel || Baked->SourceSkeletalMesh != Skel)
	{
		return FString();
	}
	TArray<FString> Changed;
	// Bone Overrides: 굽기가 적은 표와 지금 표(같은 펼침 — BoneOverrideMap)를 본마다 견준다.
	const TMap<FName, float> Now = BoneOverrideMap();
	TSet<FName> Bones;
	for (const TPair<FName, float>& Pair : Now)
	{
		Bones.Add(Pair.Key);
	}
	for (const TPair<FName, float>& Pair : Baked->RigidShrinkPerBone)
	{
		Bones.Add(Pair.Key);
	}
	TArray<FString> BoneNames;
	for (const FName Bone : Bones)
	{
		const float* A = Now.Find(Bone);
		const float* B = Baked->RigidShrinkPerBone.Find(Bone);
		if (!A || !B || !FMath::IsNearlyEqual(*A, *B, 1e-4f))
		{
			BoneNames.Add(Bone.ToString());
		}
	}
	if (BoneNames.Num() > 0)
	{
		BoneNames.Sort();
		const int32 Shown = FMath::Min(BoneNames.Num(), 4);
		Changed.Add(FString::Printf(TEXT("different Bone Overrides (%s%s)"), *FString::Join(TArrayView<const FString>(BoneNames.GetData(), Shown), TEXT(", ")),
			BoneNames.Num() > Shown ? TEXT(", ...") : TEXT("")));
	}
	if (Baked->RigidCoreShrink > 0.f && !FMath::IsNearlyEqual(Baked->RigidCoreShrink, DefaultRigidShrink, 1e-4f))
	{
		Changed.Add(TEXT("a different Bone Thickness"));
	}
	// 살 슬롯: 소프트바디 머티리얼이 하나도 없을 때 구운 데이터(내 마스터 머티리얼을 입히기 전의 첫 굽기, 이 규칙(일지 239) 전의 굽기)는
	// 비어 있고 메시 전체를 살로 쟀다 — 지금 소프트바디 슬롯이 있으면 다시 구울 것으로 본다(일지 378: 옛 문구 "older plugin version"은
	// 첫 굽기에서 원인을 알 수 없었다).
	TArray<FName> SlotsNow = ResolveCoreSlots().Array();
	TArray<FName> SlotsBaked = Baked->RigidCoreSlots;
	SlotsNow.Sort(FNameLexicalLess());
	SlotsBaked.Sort(FNameLexicalLess());
	if (SlotsNow != SlotsBaked)
	{
		Changed.Add(SlotsBaked.Num() == 0 ? FString(TEXT("the whole mesh as flesh (no slot had a Softbody material yet)"))
			: FString(TEXT("different slots with a Softbody material")));
	}
	return Changed.Num() > 0
		? FString::Printf(TEXT("The last bake used %s, so the baked jiggle and contact do not match this preview. Press Run Bake."),
			*FString::Join(Changed, TEXT(" and ")))
		: FString();
}

bool USoftbodyAuthoringData::BakeMeasuredStage()
{
	if (bLegacyPaintLayout)
	{
		ConvertLegacyPaint();
	}
	const FString Folder = ResolveOutputFolder();
	USoftbodyMeshData* Result = nullptr;

	// 속 메시(ADR-0028): 있으면 살을 채우는 안팎·봉투·거리장이 이 메시의 속을 쓴다. 대상 메시와 같으면 비운 것과 같다.
	UStaticMesh* Volume = VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr;
	if (Volume && (!Volume->GetMeshDescription(0) || Volume->GetMeshDescription(0)->Triangles().Num() == 0))
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Volume Mesh %s has no LOD0 mesh description. The bake did not start. Reimport that mesh or clear Volume Mesh."),
			*GetName(), *Volume->GetName());
		return false;
	}
	if (Volume)
	{
		UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Volume Mesh %s - the inside of the flesh, its outer surface and the shape field come from this mesh. UVs, skinning and paint come from Target Mesh."),
			*GetName(), *Volume->GetName());
	}
	// 같은 이름의 다른 메시: 굽기 산출물은 출력 폴더 + 메시 이름(SD_<메시>, T_<메시>_*)으로 찾으므로, 다른 폴더의 같은 이름 메시를
	// 같은 출력 폴더에 구우면 서로의 데이터와 텍스처를 덮어쓴다. 이 에셋이 쥔 데이터(대상 메시를 바꾼 경우)가 아니면 굽기 전에 멈춘다.
	if (TargetMesh)
	{
		const FString DataName = TEXT("SD_") + TargetMesh->GetName();
		const FString DataPath = Folder / DataName + TEXT(".") + DataName;
		const USoftbodyMeshData* Existing = FindObject<USoftbodyMeshData>(nullptr, *DataPath);
		if (!Existing && FPackageName::DoesPackageExist(Folder / DataName))
		{
			Existing = LoadObject<USoftbodyMeshData>(nullptr, *DataPath);
		}
		const UObject* Owner = !Existing ? nullptr
			: Existing->SourceSkeletalMesh ? static_cast<const UObject*>(Existing->SourceSkeletalMesh.Get())
			: static_cast<const UObject*>(Existing->SourceMesh.Get());
		if (Existing && Existing != BakedData.Get() && Owner && Owner != TargetMesh.Get())
		{
			UE_LOG(LogSoftbodyAuthoring, Error,
				TEXT("%s: %s holds the bake of %s, another mesh with the same name, and baking %s would overwrite it. Set Output Folder to another folder."),
				*GetName(), *Existing->GetPathName(), *Owner->GetPathName(), *TargetMesh->GetPathName());
			return false;
		}
	}

	if (USkeletalMesh* Skel = Cast<USkeletalMesh>(TargetMesh))
	{
		const TMap<FName, float> PerBone = BoneOverrideMap();
		const TSet<FName> CoreSlots = ResolveCoreSlots();
		// 겹친 층이 따를 면도 곧 소프트바디 머티리얼을 받을 슬롯까지 센다(PendingSoftbodySlots) — 첫 굽기가 두 번째와 같은 면을 고르게.
		Result = USoftbodyBakeLibrary::BakeSkeletalMeshMeasuredOverrides(Skel, Folder,
			CellSizeFrac, MaxInertiaGroups, DefaultRigidShrink, PerBone, LayerMaxGap, &SlotMasks, FleshReach, Volume, &CoreSlots, VolumePreservation,
			PendingSoftbodySlots());
	}
	else if (UStaticMesh* Static = Cast<UStaticMesh>(TargetMesh))
	{
		TArray<FVector4> RigidSpheres;
		if (!ResolveStaticCoreSpheres(Static, RigidSpheres, true))
		{
			return false;
		}
		// 모양에서 만든 속(Center)은 스켈레탈 본처럼 가늘어지는 곳이 있어 격자 점 사이로 토막 나지 않게 반지름 하한을 준다.
		Result = USoftbodyBakeLibrary::BakeStaticMeshMeasuredMasks(Static, Folder, RigidSpheres, CellSizeFrac, LayerMaxGap, SlotMasks, FleshReach,
			Volume, StaticCore == ESoftbodyStaticCore::Center ? SoftbodyBakeShared::CoreMinRadiusCells : 0.f, VolumePreservation,
			PendingSoftbodySlots());
	}
	else
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Target Mesh is not set to a skeletal or static mesh"), *GetName());
		return false;
	}

	if (!Result)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Measuring the flesh failed (see the errors above)"), *GetName());
		return false;
	}
	BakedData = Result;
	LastValidationErrors = -1;
	MarkPackageDirty();
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Measuring the flesh finished -> %s (texel UV channel %d; the Softbody material's TexelUV input reads a TexCoord node set to this channel)"),
		*GetName(), *Result->GetPathName(), Result->TexelUVChannel);
	return true;
}

bool USoftbodyAuthoringData::PaintConvertStage(TConstArrayView<FName> PendingSlots)
{
	if (!BakedData)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Baked Data is empty. Press Run Bake first, or set Baked Data to an existing bake."), *GetName());
		return false;
	}
	// 이름 → 인덱스는 변환 시점에 결속 — 백엔드(렌더 섹션)는 인덱스, 저작(이 에셋)은 이름.
	// 섹션 인덱스의 기준은 베이크가 읽은 소스 메시다 (TargetMesh는 대개 같지만 링크만 바꿨을 수 있음).
	const UObject* SectionMesh = BakedData->SourceSkeletalMesh
		? (const UObject*)BakedData->SourceSkeletalMesh : (const UObject*)BakedData->SourceMesh;
	TMap<int32, FSoftbodyPaintSlotMask> Resolved;
	for (const TPair<FName, FSoftbodyPaintSlotMask>& P : SlotMasks)
	{
		const int32 Idx = FindSlotIndexByName(SectionMesh, P.Key);
		if (Idx == INDEX_NONE)
		{
			UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: The mesh has no material slot named '%s'; skipped. Check the slot names in Slot Masks."),
				*GetName(), *P.Key.ToString());
			continue;
		}
		Resolved.Add(Idx, P.Value);
	}
	BakedData->Modify();
	BakedData->PhysicsDrivenBones = PhysicsDrivenBones;
	SyncMaterialValues();
	// 하드 서피스 조각(ADR-0037 §3.7): 접촉 지도 굽기는 슬롯 마스크를 모르므로 칠 단계가 찾아 구운 데이터에 넘긴다. 칠하지 않았으면 비운다.
	if (const FMeshDescription* SectionMD = BakedData->SourceSkeletalMesh ? BakedData->SourceSkeletalMesh->GetMeshDescription(0)
		: (BakedData->SourceMesh ? BakedData->SourceMesh->GetMeshDescription(0) : nullptr))
	{
		SoftbodyBakeShared::FindHardPieces(*SectionMD, SlotMasks, SoftbodyBakeShared::SlotAliasesOf(SectionMesh),
			BakedData->HardTrianglePieces, BakedData->HardPiecesFloating, BakedData->HardMaskPaintHash, true, &BakedData->HardTriangleSoftness);
		BakedData->HardTriangleSoftnessLayout = 1;   // 모든 삼각형 코너 Flex(연속 굽힘) — 접촉 지도 굽기가 모든 칸에 굽는다
	}
	// 읽으면 값이 바뀌는 마스크(sRGB 켜짐)를 알린다. 읽는 방식은 그대로다(바꾸면 그 마스크로 맞춰 둔 회색이 바뀐다). 텍스처가 비어 빠지는
	// 칠은 칠 변환·측정·조각 찾기가 그 자리에서 알린다.
	TArray<FString> Unread;
	TArray<FString> Changed;
	SoftbodyBakeShared::DescribeSlotMaskProblems(SlotMasks, Unread, Changed);
	for (const FString& Line : Changed)
	{
		UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: %s"), *GetName(), *Line);
	}
	UTexture2D* Paint = SoftbodyBakeShared::GeneratePaintForSlots(BakedData, Resolved, PendingSlots);
	if (!Paint)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Converting the slot mask paint failed (see the errors above)"), *GetName());
		return false;
	}
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Slot mask paint converted (%d slots) -> %s"),
		*GetName(), Resolved.Num(), *Paint->GetPathName());
	return true;
}

namespace
{
	const TCHAR* HostTemplatePath = TEXT("/SoftbodySystem/M_SoftbodyHost.M_SoftbodyHost");

	/** 예제 머티리얼의 TexelUV TexCoord 노드: Desc가 "TexelUV"인 것, 없으면 MF 호출의 TexelUV 입력에 물린 것 */
	UMaterialExpressionTextureCoordinate* FindTexelUVCoord(UMaterial* Material)
	{
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionTextureCoordinate* Coord = Cast<UMaterialExpressionTextureCoordinate>(Expression);
			if (Coord && Coord->Desc == TEXT("TexelUV"))
			{
				return Coord;
			}
		}
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (!Call)
			{
				continue;
			}
			for (const FFunctionExpressionInput& In : Call->FunctionInputs)
			{
				if (In.ExpressionInput && In.ExpressionInput->InputName == FName(TEXT("TexelUV")))
				{
					if (UMaterialExpressionTextureCoordinate* Coord = Cast<UMaterialExpressionTextureCoordinate>(In.Input.Expression))
					{
						return Coord;
					}
				}
			}
		}
		return nullptr;
	}

	/** 원래 슬롯 머티리얼이 쓰는 텍스처. 엔진 5.7부터 품질·기능 수준을 고르는 옛 함수는 빈 함수라(아무것도 돌려주지 않는다) 새 것을 쓴다. */
	TArray<UTexture*> UsedTextures(UMaterialInterface* Material)
	{
		TArray<UTexture*> Textures;
		if (Material)
		{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
			Material->GetUsedTextures(Textures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true);
#else
			Material->GetUsedTextures(Textures);
#endif
		}
		return Textures;
	}

	/**
	 * 원래 슬롯 머티리얼에서 베이스 컬러로 보이는 텍스처: 이름에 BaseColor/Albedo/Diffuse/_D/_Col가 든 것, 없으면 노멀 맵이 아닌 첫 sRGB 텍스처.
	 */
	UTexture* GuessBaseColorTexture(UMaterialInterface* Material)
	{
		UTexture* Fallback = nullptr;
		for (UTexture* Texture : UsedTextures(Material))
		{
			if (!Texture || Texture->IsNormalMap())
			{
				continue;
			}
			const FString Name = Texture->GetName();
			if (Name.Contains(TEXT("BaseColor")) || Name.Contains(TEXT("Albedo")) || Name.Contains(TEXT("Diffuse"))
				|| Name.EndsWith(TEXT("_D")) || Name.Contains(TEXT("_Col")) || Name.Contains(TEXT("_BC")))
			{
				return Texture;
			}
			if (!Fallback && Texture->SRGB)
			{
				Fallback = Texture;
			}
		}
		return Fallback;
	}

	/** 원래 슬롯 머티리얼의 노멀 맵(압축 설정이 노멀 맵인 첫 텍스처). */
	UTexture* GuessNormalTexture(UMaterialInterface* Material)
	{
		for (UTexture* Texture : UsedTextures(Material))
		{
			if (Texture && Texture->IsNormalMap())
			{
				return Texture;
			}
		}
		return nullptr;
	}

	/**
	 * 원래 슬롯 머티리얼의 거칠기 텍스처와 그 채널: 이름에 Roughness·Rough가 들거나 _R로 끝나면 R, 묶음(ORM·ARM·OcclusionRoughnessMetallic)이면
	 * G. sRGB 텍스처는 거칠기 데이터가 아니라고 보고 빼다.
	 */
	UTexture* GuessRoughnessTexture(UMaterialInterface* Material, FLinearColor& OutChannel)
	{
		for (UTexture* Texture : UsedTextures(Material))
		{
			if (!Texture || Texture->IsNormalMap() || Texture->SRGB)
			{
				continue;
			}
			const FString Name = Texture->GetName();
			if (Name.Contains(TEXT("ORM")) || Name.Contains(TEXT("ARM")) || Name.Contains(TEXT("OcclusionRoughnessMetallic")))
			{
				OutChannel = FLinearColor(0.f, 1.f, 0.f, 0.f);
				return Texture;
			}
			if (Name.Contains(TEXT("Rough")) || Name.EndsWith(TEXT("_R")))
			{
				OutChannel = FLinearColor(1.f, 0.f, 0.f, 0.f);
				return Texture;
			}
		}
		return nullptr;
	}

	/** 머티리얼 그래프의 텍스처 파라미터 노드(이름으로). */
	UMaterialExpressionTextureSampleParameter* FindTextureParameter(UMaterial* Material, FName Name)
	{
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression);
			if (Parameter && Parameter->ParameterName == Name)
			{
				return Parameter;
			}
		}
		return nullptr;
	}
}

bool USoftbodyAuthoringData::CreateMaterialStage()
{
	if (!BakedData)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Baked Data is empty. The material needs the texel UV channel from the bake; press Run Bake first."), *GetName());
		return false;
	}
	USkeletalMesh* Skeletal = Cast<USkeletalMesh>(TargetMesh);
	UStaticMesh* Static = Cast<UStaticMesh>(TargetMesh);
	if (!Skeletal && !Static)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Target Mesh is not set"), *GetName());
		return false;
	}
	if (SoftbodyMaterialSlots.Num() == 0)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Softbody Material Slots is empty. Add the slots that should get the Softbody material (skin and tight clothing)."), *GetName());
		return false;
	}
	UMaterial* Template = LoadObject<UMaterial>(nullptr, HostTemplatePath);
	if (!Template)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Could not find the plugin's example material %s. The plugin's Content folder is missing; reinstall the plugin."), *GetName(), HostTemplatePath);
		return false;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	const FString Folder = ResolveOutputFolder();
	const FString MeshName = TargetMesh->GetName();
	const FString BaseName = FString::Printf(TEXT("M_%s_Softbody"), *MeshName);
	UMaterial* Base = LoadObject<UMaterial>(nullptr, *(Folder / BaseName + TEXT(".") + BaseName));
	if (!Base)
	{
		Base = Cast<UMaterial>(AssetTools.DuplicateAsset(BaseName, Folder, Template));
		if (!Base)
		{
			UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Could not copy the example material to %s/%s"), *GetName(), *Folder, *BaseName);
			return false;
		}
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		// 예제 머티리얼은 옛 엔진(5.6)에서 저장해 두어, 5.8이 읽을 때 스켈레탈 사용을 따라 메시 디포머 사용까지 켠다(엔진 버전 올림 규칙).
		// 소프트바디는 메시 디포머를 쓰지 않으니 그 셰이더를 컴파일하지 않게 복사본에서 끈다(일지 367).
		Base->SetUsageByFlag(MATUSAGE_MeshDeformer, false);
#endif
	}
	Base->Modify();
	if (Skeletal)
	{
		SoftbodyBakeShared::SetMaterialUsage(Base, MATUSAGE_SkeletalMesh);
	}
	if (UMaterialExpressionTextureCoordinate* TexelCoord = FindTexelUVCoord(Base))
	{
		TexelCoord->CoordinateIndex = BakedData->TexelUVChannel;
	}
	else
	{
		UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: %s has no TexCoord node feeding TexelUV. Connect a TexCoord node set to channel %d to the TexelUV inputs of its Softbody material functions by hand."), *GetName(), *Base->GetName(),
			BakedData->TexelUVChannel);
	}
	UMaterialEditingLibrary::RecompileMaterial(Base);
	Base->MarkPackageDirty();

	bool bRoughnessSamplerSet = false;
	TArray<FSkeletalMaterial> SkelMats = Skeletal ? Skeletal->GetMaterials() : TArray<FSkeletalMaterial>();
	TArray<FStaticMaterial> StaticMats = Static ? Static->GetStaticMaterials() : TArray<FStaticMaterial>();
	int32 Assigned = 0;
	for (const FName SlotName : SoftbodyMaterialSlots)
	{
		const int32 Idx = FindSlotIndexByName(TargetMesh, SlotName);
		if (Idx == INDEX_NONE)
		{
			UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: The mesh has no material slot named '%s'; skipped. Check Softbody Material Slots."), *GetName(), *SlotName.ToString());
			continue;
		}
		UMaterialInterface* Current = Skeletal ? SkelMats[Idx].MaterialInterface.Get() : StaticMats[Idx].MaterialInterface.Get();
		if (SoftbodyBakeShared::ReadsCarrier(Current))
		{
			UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Slot '%s' already uses a Softbody material (%s); left as is"), *GetName(), *SlotName.ToString(),
				*Current->GetName());
			continue;
		}
		const FString InstanceName = FString::Printf(TEXT("MI_%s_%s"), *MeshName, *SlotName.ToString());
		UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(Folder / InstanceName + TEXT(".") + InstanceName));
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = Base;
			Instance = Cast<UMaterialInstanceConstant>(AssetTools.CreateAsset(InstanceName, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
		}
		if (!Instance)
		{
			UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Could not create the material instance %s/%s"), *GetName(), *Folder, *InstanceName);
			return false;
		}
		Instance->Modify();
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Base);
		// 에디터 프리뷰용 베이크 텍스처(PIE는 컴포넌트가 이름 규약으로 다시 넣는다)
		const TPair<FName, UTexture2D*> BakeTextures[] = {
			{ FName(TEXT("AttrDbgTex")), BakedData->AttributeTexDebug.Get() },
			{ FName(TEXT("Attr2Tex")), BakedData->AttributeTex2.Get() },
			{ FName(TEXT("InertiaResponseTex")), BakedData->InertiaResponseTex.Get() },
			{ FName(TEXT("PaintTex")), BakedData->PaintTex.Get() },
		};
		for (const TPair<FName, UTexture2D*>& Pair : BakeTextures)
		{
			if (Pair.Value)
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Pair.Key, Pair.Value);
			}
		}
		UTexture* BaseColor = GuessBaseColorTexture(Current);
		if (BaseColor)
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(TEXT("BaseColorTex")), BaseColor);
		}
		// 노멀 맵과 거칠기도 옮긴다(전에는 베이스 컬러만 옮겨, 바꿔 입힌 슬롯이 노멀 맵과 거칠기를 잃었다).
		UTexture* NormalMap = GuessNormalTexture(Current);
		if (NormalMap)
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(TEXT("NormalTex")), NormalMap);
		}
		FLinearColor RoughnessChannel(1.f, 0.f, 0.f, 0.f);
		UTexture* Roughness = GuessRoughnessTexture(Current, RoughnessChannel);
		if (Roughness)
		{
			// 텍스처 파라미터의 샘플러 종류는 그래프 노드에 있어 인스턴스마다 바꿀 수 없다: 이 메시만 쓰는 베이스 머티리얼에서 거칠기
			// 텍스처의 압축 설정에 맞춘다(슬롯마다 종류가 다르면 처음 것을 쓰고 나머지는 옮기지 않는다).
			UMaterialExpressionTextureSampleParameter* Parameter = FindTextureParameter(Base, FName(TEXT("RoughnessTex")));
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			const EMaterialSamplerType Wanted = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Roughness);
#else
			const EMaterialSamplerType Wanted = MaterialExpressionUtils::GetSamplerTypeForTexture(Roughness);
#endif
			if (Parameter && (!bRoughnessSamplerSet || Parameter->SamplerType == Wanted))
			{
				if (!bRoughnessSamplerSet && (Parameter->SamplerType != Wanted || Parameter->Texture != Roughness))
				{
					Base->Modify();
					Parameter->SamplerType = Wanted;
					Parameter->Texture = Roughness;
					UMaterialEditingLibrary::RecompileMaterial(Base);
				}
				bRoughnessSamplerSet = true;
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(TEXT("RoughnessTex")), Roughness);
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, FName(TEXT("RoughnessChannel")), RoughnessChannel);
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, FName(TEXT("Roughness")), 1.f);
			}
			else
			{
				Roughness = nullptr;
			}
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
		if (Skeletal)
		{
			SkelMats[Idx].MaterialInterface = Instance;
		}
		else
		{
			StaticMats[Idx].MaterialInterface = Instance;
		}
		++Assigned;
		UE_LOG(LogSoftbodyAuthoring, Display,
			TEXT("%s: slot '%s' now uses %s (was %s). Copied: base color %s, normal map %s, roughness %s. Set any missing texture on the "
				"material instance by hand (BaseColorTex, NormalTex, RoughnessTex)."),
			*GetName(), *SlotName.ToString(), *Instance->GetName(), *GetNameSafe(Current),
			BaseColor ? *BaseColor->GetName() : TEXT("(not found)"), NormalMap ? *NormalMap->GetName() : TEXT("(not found)"),
			Roughness ? *Roughness->GetName() : TEXT("(not found, 0.5)"));
	}
	if (Assigned > 0)
	{
		TargetMesh->Modify();
		if (Skeletal)
		{
			Skeletal->SetMaterials(SkelMats);
		}
		else
		{
			Static->SetStaticMaterials(StaticMats);
		}
		TargetMesh->MarkPackageDirty();
	}
	MarkPackageDirty();
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Softbody material ready - %s (texel UV channel %d), applied to %d slots"), *GetName(), *Base->GetName(),
		BakedData->TexelUVChannel, Assigned);
	return true;
}

bool USoftbodyAuthoringData::SurfaceCarrierStage()
{
	if (!BakedData)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Baked Data is empty. The contact map is baked on top of the measure bake; press Run Bake first."), *GetName());
		return false;
	}
	const bool bSkeletal = BakedData->SourceSkeletalMesh != nullptr;

	// 두께 기준 코어: (스태틱) 관성 코어 그대로 > (스켈레탈) 비움 = 뼈대의 본 선(굵기 = 살갗까지 거리 × DefaultRigidShrink).
	// 따로 적는 구 목록(Thickness Core Spheres)은 쓰는 에셋이 없어 지웠다(일지 316).
	TArray<FVector4> Cores;
	if (!bSkeletal)
	{
		// 측정 굽기와 같은 속(Core Placement). 구운 메시에서 만든다 — 그 뒤 Target Mesh를 바꿨어도 살의 무름과 같은 속이다.
		ResolveStaticCoreSpheres(BakedData->SourceMesh, Cores, false);
	}

	const UObject* SourceMesh = bSkeletal ? (const UObject*)BakedData->SourceSkeletalMesh : (const UObject*)BakedData->SourceMesh;
	const bool bMaterialWired = AnySlotReadsCarrier(SourceMesh);
	if (!bMaterialWired && bSkeletal)
	{
		// 스켈레탈은 코어와 관성 묶음도 살 슬롯으로 고른다(측정 굽기) — 접촉 지도만 다시 구우면 그대로 메시 전체라 Run Bake를 알린다(일지 378).
		UE_LOG(LogSoftbodyAuthoring, Warning,
			TEXT("%s: No slot uses a Softbody material yet, so this bake counts the whole mesh as flesh. After you assign the Softbody material, "
				"press Run Bake once more. Bones that move only other slots (hair, accessories) then leave the rigid core and the jiggle groups, "
				"and the material gets linked to the contact map."),
			*GetName());
	}
	else if (!bMaterialWired)
	{
		UE_LOG(LogSoftbodyAuthoring, Warning,
			TEXT("%s: No slot uses a Softbody material yet. The contact map is baked for %s now; after you assign the Softbody material, "
				"press Rebake Contact Map once more so the material is linked to the contact map and only the pressable slots are included."),
			*GetName(), CarrierEnvelopeSlots.Num() > 0 ? TEXT("the Pressable Slots") : TEXT("the whole mesh"));
	}

	BakedData->Modify();
	BakedData->CarrierEnvelopeSlots = CarrierEnvelopeSlots;
	BakedData->CarrierCharts = CarrierCharts;
	// 속 메시(ADR-0028): 캐리어의 봉투도 측정 베이크와 같은 속에서 뽑는다. 측정 베이크 뒤에 바꿨으면 살의 무름은 옛 속으로 구운 값이다.
	// 접촉 지도 굽기는 BakedData->VolumeMesh에서 속을 읽으므로 굽는 동안만 지금 값을 넣고, 끝나면 측정 베이크가 쓴 값으로 되돌린다 — 새 값을
	// 그대로 남기면 Rebake Contact Map만 누를 때 Validate가 "Run Bake가 필요하다"를 더 알리지 않는다(일지 363).
	UStaticMesh* Volume = VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr;
	UStaticMesh* const MeasuredVolume = BakedData->VolumeMesh;
	if (MeasuredVolume != Volume)
	{
		UE_LOG(LogSoftbodyAuthoring, Warning,
			TEXT("%s: Volume Mesh changed since the measure bake (was %s, now %s). The contact map uses the new volume mesh, but the baked flesh softness still comes from the old one. Press Run Bake to bake everything again."),
			*GetName(), *GetNameSafe(MeasuredVolume), *GetNameSafe(Volume));
	}
	BakedData->VolumeMesh = Volume;
	ON_SCOPE_EXIT { BakedData->VolumeMesh = MeasuredVolume; };
	if (!USoftbodyBakeLibrary::BakeSurfaceCarrier(BakedData, CarrierResolution, Cores, CarrierEnvelopeCellSize,
		CarrierTargetTriangles, DefaultRigidShrink, CarrierMaxThickness))
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Contact map bake failed, so contact (dents) stays off. See the LogSoftbodyCarrier lines above."), *GetName());
		return false;
	}
	// 부위 나누기: 방금 구운 칸 스킨 웨이트에서 칸마다 두 부위 몫(비어 있으면 몫 텍스처를 지운다).
	USoftbodyBakeLibrary::BakeSeparatedParts(BakedData, SeparatedPairs);
	LastValidationErrors = -1;
	MarkPackageDirty();
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Contact map finished - resolution %d, cell %.3f cm%s"), *GetName(),
		BakedData->CarrierResolution, BakedData->CarrierTexelSize,
		bMaterialWired ? TEXT("") : TEXT(" (press Rebake Contact Map again after assigning the Softbody material)"));
	return true;
}

bool USoftbodyAuthoringData::ColliderFieldStage()
{
	UStaticMesh* Static = Cast<UStaticMesh>(TargetMesh);
	USkeletalMesh* Skeletal = Cast<USkeletalMesh>(TargetMesh);
	if (!Static && !Skeletal)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: The shape field is baked onto Target Mesh (skeletal or static), but Target Mesh is not set"), *GetName());
		return false;
	}
	if (!SoftbodyBakeShared::IsEditableContent(TargetMesh))
	{
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: %s is engine content and cannot be changed. Duplicate the mesh into your project's Content folder and bake the copy."),
			*GetName(), *TargetMesh->GetPathName());
		return false;
	}
	if (Static && PresserBones.Num() > 0)
	{
		UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: On a static mesh the whole mesh presses, so Self Contact Bones is ignored"), *GetName());
	}
	const TArray<FName> Bones = Skeletal ? PresserBones : TArray<FName>();
	USoftbodyColliderField* Field = USoftbodyBakeLibrary::BakeColliderField(TargetMesh, ColliderFieldVoxelSize, Bones, ColliderFieldPartOverlap,
		VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr, ShapeFieldSlots);
	if (!Field)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Shape field bake failed. See the LogSoftbodyColliderField lines above."), *GetName());
		return false;
	}
	TargetMesh->MarkPackageDirty();
	// 잠들기 상자(ADR-0020): 캐리어 굽기가 적은 자기 접촉 상자는 그때의 거리장 기준이라, 새 부위로 다시 적는다.
	if (Skeletal && BakedData && BakedData->HasSurfaceCarrier() && BakedData->SourceSkeletalMesh == Skeletal)
	{
		USoftbodyBakeLibrary::BakeWakeBounds(BakedData);
	}
	LastValidationErrors = -1;
	MarkPackageDirty();
	int32 NumSelfParts = 0;
	for (const FSoftbodyColliderFieldPart& Part : Field->Parts)
	{
		NumSelfParts += Part.bPressesSelf ? 1 : 0;
	}
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Shape field finished - %d parts press other Softbody meshes, %d of them also press the mesh itself, stored on %s%s"),
		*GetName(), Field->Parts.Num(), Skeletal ? NumSelfParts : 0, *TargetMesh->GetName(),
		Skeletal && Bones.Num() == 0 ? TEXT(" (Self Contact Bones is empty, so every part presses the mesh itself; for a body, list only the hands, such as Hand_L and Hand_R)") : TEXT(""));
	return true;
}

bool USoftbodyAuthoringData::NaniteTessellationStage()
{
	if (!BakedData)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Baked Data is empty. Nanite tessellation is set up on the baked mesh; press Run Bake first."), *GetName());
		return false;
	}
	const UObject* SourceMesh = BakedData->SourceSkeletalMesh
		? (const UObject*)BakedData->SourceSkeletalMesh : (const UObject*)BakedData->SourceMesh;
	if (!SourceMesh)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Baked Data has no source mesh. Press Run Bake again."), *GetName());
		return false;
	}
	if (!AnySlotReadsCarrier(SourceMesh))
	{
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: No slot uses a Softbody material. Assign the Softbody material first, then press Reapply Nanite Tessellation (it turns tessellation on in that material)."),
			*GetName());
		return false;
	}
	if (!USoftbodyBakeLibrary::ConfigureNaniteTessellation(BakedData, NaniteDisplacementMagnitude, NaniteDisplacementCenter,
		NaniteMaxWorldPositionOffset))
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Nanite tessellation setup failed. See the LogSoftbodyNanite lines above."), *GetName());
		return false;
	}
	MarkPackageDirty();
	UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Nanite tessellation set up (displacement %.1f cm, center %.2f, max world position offset %.0f cm)"),
		*GetName(), NaniteDisplacementMagnitude, NaniteDisplacementCenter, NaniteMaxWorldPositionOffset);
	return true;
}

void USoftbodyAuthoringData::SaveOutputs()
{
	// 저장하는 것: 이 굽기가 더럽힌 패키지만 — 굽기 전에 이미 저장하지 않은 상태였던 패키지는 사용자가 하던 작업이 섞였을 수 있어
	// 건드리지 않고 이름만 알린다(전에는 산출물 폴더의 더러운 패키지를 모두 저장해, 그 폴더에서 하던 다른 작업까지 말없이 저장했다).
	TSet<UPackage*> Packages;
	TArray<FString> LeftUnsaved;
	auto AddPackage = [this, &Packages, &LeftUnsaved](UPackage* Package)
	{
		if (!Package || !Package->IsDirty() || Package == GetTransientPackage() || Packages.Contains(Package))
		{
			return;
		}
		// 이 에셋은 전부터 더러웠어도 저장한다 — 그 값이 이번 굽기의 입력이다(일지 239: 구운 데이터만 저장되고 SA 변경이 크래시로 사라졌다).
		// 구운 데이터도 — 이 에셋의 재료값(Volume Preservation)을 바꾸면 굽지 않고 바로 옮겨 둬 더러워지고, 실행이 어차피 다시 쓴다(ADR-0037 §3.3).
		const bool bOwnOutput = Package == GetOutermost() || (BakedData && Package == BakedData->GetOutermost());
		if (DirtyBeforeRun.Contains(Package->GetFName()) && !bOwnOutput)
		{
			LeftUnsaved.AddUnique(Package->GetName());
			return;
		}
		Packages.Add(Package);
	};
	AddPackage(GetOutermost());
	AddPackage(BakedData ? BakedData->GetOutermost() : nullptr);
	AddPackage(TargetMesh ? TargetMesh->GetOutermost() : nullptr);
	for (const UMaterialInterface* Material : SlotMaterials(TargetMesh))
	{
		AddPackage(Material->GetOutermost());
		AddPackage(Material->GetMaterial() ? Material->GetMaterial()->GetOutermost() : nullptr);
	}
	// 산출물 폴더에 굽기가 만들거나 고친 텍스처·머티리얼
	const FString FolderPrefix = ResolveOutputFolder() / TEXT("");
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->IsDirty() && It->GetName().StartsWith(FolderPrefix))
		{
			AddPackage(*It);
		}
	}
	if (LeftUnsaved.Num() > 0)
	{
		UE_LOG(LogSoftbodyAuthoring, Warning,
			TEXT("%s: these packages already had unsaved changes before the bake, so the bake did not save them (save them yourself "
				"when ready): %s"), *GetName(), *FString::Join(LeftUnsaved, TEXT(", ")));
	}
	if (Packages.Num() == 0)
	{
		return;
	}
	TArray<UPackage*> ToSave = Packages.Array();
	if (UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/true))
	{
		UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Saved %d packages"), *GetName(), ToSave.Num());
	}
	else
	{
		UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: Some packages could not be saved. Save them yourself (File > Save All)."), *GetName());
	}
}

// --- 검증 ---

void USoftbodyAuthoringData::RunValidation()
{
	LastValidationErrors = -1;
	if (!BakedData)
	{
		UE_LOG(LogSoftbodyAuthoring, Error, TEXT("%s: Nothing to validate - Baked Data is empty. Press Run Bake first."), *GetName());
		MarkPackageDirty();
		return;
	}

	int32 Errors = USoftbodyBakeLibrary::VerifyBakeIntegrity(BakedData);
	if (Errors < 0)
	{
		MarkPackageDirty();
		return;
	}
	USoftbodyBakeLibrary::DumpBakeCoverageStats(BakedData);

	const bool bSourceMatches =
		(BakedData->SourceSkeletalMesh
			&& BakedData->SourceSkeletalMesh.Get() == TargetMesh.Get())
		|| (BakedData->SourceMesh
			&& BakedData->SourceMesh.Get() == TargetMesh.Get());
	if (TargetMesh && !bSourceMatches)
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: Baked Data was baked from a different mesh than Target Mesh. Press Run Bake."), *GetName());
	}
	// 압축성 값(ADR-0037 §3.3): 함몰·솟음은 구운 데이터의 값을 바로 읽지만, 덩어리 모양은 마지막 측정 굽기의 값으로 구워져 있다.
	SyncMaterialValues();
	if (!FMath::IsNearlyEqual(BakedData->MeasuredVolumePreservation, FMath::Clamp(VolumePreservation, 0.f, 1.f), 1e-3f))
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: Volume Preservation is %.2f but the flesh was measured with %.2f. Dents already use the new value; press Run Bake so a whole lump spreads the same way."),
			*GetName(), VolumePreservation, BakedData->MeasuredVolumePreservation);
	}
	// 겉껍질 장력(ADR-0037 §3.4): 길이 눈금은 고정 길이 L0라 옛 접촉 지도도 겉껍질이 돈다.
	// 판(ADR-0038 §4): 칠한 판이 있는데 접촉 지도의 판 표가 옛 판(1 = 단단한 판만)이면 실행이 판을 없는 것으로 보아 살처럼 눌린다.
	// 칠 단계가 코너마다 Softness를 다시 적어야 하므로 Run Bake(또는 Reapply Paint)를 권한다.
	if (bBakeSurfaceCarrier && BakedData->HardPiecesFloating.Num() > 0 && BakedData->HardPieces.Num() > 0
		&& BakedData->HardPieceVersion != SoftbodyHardSurface::Version)
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: the contact map holds an older plate layout, so the Flex plates dent like bare skin. Press Run Bake."),
			*GetName());
	}
	// 연속 굽힘: Flex를 텍스처로 칠했는데 접촉 지도가 판 칸에만 Flex를 담은 옛 굽기면, 판 밖 회색은 맨살로 남는다. 판만 칠한
	// 자산(검정·흰색)은 그대로라 오류가 아니라 경고다.
	if (bBakeSurfaceCarrier && BakedData->HasSurfaceCarrier() && BakedData->CarrierSoftnessCoverage == 0)
	{
		bool bFlexTexture = false;
		for (const TPair<FName, FSoftbodyPaintSlotMask>& Pair : SlotMasks)
		{
			bFlexTexture |= SoftbodyBakeShared::IsTextureMaskSource(SoftbodyBakeShared::PaintSource(Pair.Value, SoftbodyBakeShared::EPaintKind::Softness));
		}
		if (bFlexTexture)
		{
			UE_LOG(LogSoftbodyAuthoring, Warning,
				TEXT("%s: the contact map was baked by an older version that read gray Flex paint only on plates, so gray Flex elsewhere still acts like bare skin. Press Reapply Paint."),
				*GetName());
		}
	}
	// 옛 배치 슬롯 칠(일지 298 전): 굽기는 먼저 옮긴다(ConvertLegacyPaint). 옮기기 전에는 미리보기와 이 검사가 옛 텍스처를 새 배치로 읽는다.
	if (bLegacyPaintLayout)
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: the slot paint was made before the fixed layout (R Depth, G Jiggle, B Volume and A Flex). Press Run Bake: it moves the paint first."),
			*GetName());
	}
	// 측정이 읽은 칠(ADR-0038 §3.6 — Depth·Jiggle·Volume·떠 있는 판)이 지금 칠과 다르면 출렁임과 접촉이 다른 칠을 본다.
	else if (TargetMesh && SoftbodyBakeShared::HashMeasuredPaint(TargetMesh, SlotMasks) != BakedData->MeasuredPaintHash)
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: the slot paint changed since the flesh was measured, so the jiggle and the contact read different paint. Press Run Bake."),
			*GetName());
	}
	// 슬롯 마스크가 고른 대로 읽히지 않는 곳: 텍스처가 빈 텍스처 채널(그 칠만 칠하지 않은 슬롯처럼 든다)과 sRGB 마스크(회색이 어둡게 든다).
	// 굽기는 그대로 끝나므로 오류가 아니라 경고다.
	{
		TArray<FString> Unread;
		TArray<FString> Changed;
		SoftbodyBakeShared::DescribeSlotMaskProblems(SlotMasks, Unread, Changed);
		Unread.Append(Changed);
		for (const FString& Line : Unread)
		{
			UE_LOG(LogSoftbodyAuthoring, Warning, TEXT("%s: %s"), *GetName(), *Line);
		}
	}
	if (BakedData->VolumeMesh != (VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr))
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: Volume Mesh differs from the one Baked Data was baked with (baked with %s, now %s). The inside of the flesh changed; press Run Bake to bake everything again."),
			*GetName(), *GetNameSafe(BakedData->VolumeMesh), *GetNameSafe(VolumeMesh.Get() != TargetMesh.Get() ? VolumeMesh.Get() : nullptr));
	}

	if (USkeletalMesh* Skel = Cast<USkeletalMesh>(TargetMesh))
	{
		const int32 MappingErrors =
			USoftbodyBakeLibrary::VerifySkeletalBake(Skel, BakedData);
		Errors += MappingErrors >= 0 ? MappingErrors : 1;
	}

	if (BakedData->InertiaResponseVersion != 2
		|| BakedData->NumInertiaResponses != 3
		|| !BakedData->InertiaResponseTex)
	{
		++Errors;
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: The flesh softness bake is missing or out of date, so nothing jiggles. A skeletal mesh needs skin-weighted bones inside the mesh; a static mesh needs Core Placement Center or Bottom, or a Rigid Core Sphere inside the mesh. Then press Run Bake."),
			*GetName());
	}
	else
	{
		const int32 InertiaErrors =
			USoftbodyBakeLibrary::VerifyInertiaResponseBake(BakedData);
		Errors += InertiaErrors >= 0 ? InertiaErrors : 1;
	}

	// 표면 캐리어: 켜 두었는데 없으면 접촉이 조용히 꺼진다
	if (bBakeSurfaceCarrier)
	{
		if (!BakedData->CarrierUVTex || BakedData->CarrierResolution <= 0)
		{
			++Errors;
			UE_LOG(LogSoftbodyAuthoring, Error,
				TEXT("%s: There is no contact map, so nothing dents. Press Rebake Contact Map (or Run Bake)."), *GetName());
		}
		else if (TargetMesh && !AnySlotReadsCarrier(TargetMesh))
		{
			UE_LOG(LogSoftbodyAuthoring, Warning,
				TEXT("%s: No slot uses a Softbody material, so the mesh will not deform. Assign the Softbody material, then press Rebake Contact Map."),
				*GetName());
		}
	}

	// 머티리얼이 이 베이크의 UV 채널을 읽는가(같은 마스터 머티리얼을 채널이 다른 메시가 같이 쓰면 여기서 잡힌다)
	if (TargetMesh)
	{
		Errors += SoftbodyBakeShared::CheckMaterialChannels(BakedData, TargetMesh, *GetName());
	}

	// 모양 거리장: 켜 두었는데 메시에 없으면 자기 접촉·실제 모양 누르기가 빠진다
	if (bBakeColliderField && TargetMesh)
	{
		const USoftbodyColliderField* Field = USoftbodyColliderField::FindOnAsset(TargetMesh);
		const TSet<FName> BakedSlots(Field ? Field->MaterialSlots : TArray<FName>());
		const TSet<FName> WantedSlots(ShapeFieldSlots);
		auto SlotLabel = [](const TSet<FName>& Slots)
		{
			return Slots.Num() == 0 ? FString(TEXT("the whole mesh"))
				: FString::JoinBy(Slots, TEXT(", "), [](const FName Name) { return Name.ToString(); });
		};
		if (!Field)
		{
			++Errors;
			UE_LOG(LogSoftbodyAuthoring, Error,
				TEXT("%s: %s has no shape field. Press Rebake Shape Field (or Run Bake)."), *GetName(), *TargetMesh->GetName());
		}
		else if (BakedSlots.Num() != WantedSlots.Num() || !BakedSlots.Includes(WantedSlots))
		{
			++Errors;
			UE_LOG(LogSoftbodyAuthoring, Error,
				TEXT("%s: Shape Field Slots changed since the shape field bake (baked from %s, now %s). Press Rebake Shape Field."),
				*GetName(), *SlotLabel(BakedSlots), *SlotLabel(WantedSlots));
		}
	}

	LastValidationErrors = Errors;
	MarkPackageDirty();
	if (Errors == 0)
	{
		UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Validation passed"), *GetName());
	}
	else
	{
		UE_LOG(LogSoftbodyAuthoring, Error,
			TEXT("%s: Validation found %d problems (see the errors above)"), *GetName(), Errors);
	}
}

void USoftbodyAuthoringData::HandleAssetPostImport(UFactory* Factory, UObject* CreatedObject)
{
	UTexture2D* Tex = Cast<UTexture2D>(CreatedObject);
	if (!Tex)
	{
		return;
	}

	// 이 텍스처를 참조하는 저작 에셋을 레지스트리에서 역추적 — SlotMasks에 물려 있으면 알림의 버튼 하나로 다시 적용한다.
	// "저장 → 알트탭" 루프 (설계 §8.3). 자동으로 굽지는 않는다(NotifyPaintReimported).
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FName> Referencers;
	Registry.GetReferencers(Tex->GetOutermost()->GetFName(), Referencers);

	for (const FName& PackageName : Referencers)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets);
		for (const FAssetData& AD : Assets)
		{
			if (AD.AssetClassPath != USoftbodyAuthoringData::StaticClass()->GetClassPathName())
			{
				continue;
			}
			USoftbodyAuthoringData* Authoring = Cast<USoftbodyAuthoringData>(AD.GetAsset());
			if (!Authoring || !Authoring->BakedData)
			{
				continue;
			}
			bool bUsesTex = false;
			for (const TPair<FName, FSoftbodyPaintSlotMask>& P : Authoring->SlotMasks)
			{
				bUsesTex |= P.Value.Texture == Tex;
			}
			if (bUsesTex)
			{
				// 네 칠이 모두 측정에 들므로 측정이 읽은 칠이 바뀌었으면 Run Bake다(ADR-0038 §3.6, Reapply Paint도 그때는 Run Bake로 돈다).
				const bool bNeedsBake = Authoring->bLegacyPaintLayout || !Authoring->TargetMesh
					|| SoftbodyBakeShared::HashMeasuredPaint(Authoring->TargetMesh, Authoring->SlotMasks) != Authoring->BakedData->MeasuredPaintHash;
				UE_LOG(LogSoftbodyAuthoring, Display, TEXT("%s: Paint texture %s was reimported. Press %s to apply it."),
					*Authoring->GetName(), *Tex->GetName(), bNeedsBake ? TEXT("Run Bake") : TEXT("Reapply Paint"));
				NotifyPaintReimported(*Authoring, *Tex, bNeedsBake);
			}
		}
	}
}

// --- 팩토리 ---

USoftbodyAuthoringDataFactory::USoftbodyAuthoringDataFactory()
{
	SupportedClass = USoftbodyAuthoringData::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* USoftbodyAuthoringDataFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName,
	EObjectFlags Flags, UObject* /*Context*/, FFeedbackContext* /*Warn*/)
{
	USoftbodyAuthoringData* Data = NewObject<USoftbodyAuthoringData>(InParent, InClass, InName, Flags);
	// 새 에셋의 스태틱 메시 속은 모양의 한가운데(일지 231). C++ 기본값(Custom)은 이 값이 생기기 전 에셋이 적어 둔 구를 지키려고 둔 것이다.
	Data->StaticCore = ESoftbodyStaticCore::Center;
	return Data;
}

#undef LOCTEXT_NAMESPACE
