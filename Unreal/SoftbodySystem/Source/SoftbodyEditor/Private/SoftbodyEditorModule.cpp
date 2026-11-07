// Copyright (c) 2026 wjsekrud

#include "ContentBrowserMenuContexts.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "FileHelpers.h"
#include "LevelEditorViewport.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "SoftbodyAuthoringData.h"
#include "SoftbodyBakeLibrary.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyColliderField.h"
#include "SoftbodyRecord.h"
#include "Subsystems/ImportSubsystem.h"
#include "ToolMenus.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "SoftbodyEditor"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyEditorMenu, Log, All);

/**
 * 에디터 통합:
 *  - 소스 마스크 리임포트 → 저작 에셋 자동 재변환 훅 (설계 §8.3 "저장→알트탭" 루프).
 *    ImportSubsystem은 GEditor 소속이라 PostEngineInit까지 미룬다 (커맨드릿에선 GEditor 없음 → 무등록).
 *  - 메시 에셋 우클릭 메뉴 "Softbody: 모양 거리장 굽기" — 손처럼 누르는 쪽 메시(소프트바디가 아니라 저작 에셋이 없는 메시)의
 *    실제 모양을 거리 격자로 구워 메시에 붙인다(ADR-0004). 소프트바디 몸의 자기 접촉은 저작 에셋 §7에서 굽는다.
 */
static FSimpleMulticastDelegate& OnPostEngineInit()
{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	return FCoreDelegates::OnPostEngineInit;
#else
	return FCoreDelegates::GetOnPostEngineInit();
#endif
}

class FSoftbodyEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PostEngineInitHandle = OnPostEngineInit().AddLambda([this]()
		{
			if (GEditor)
			{
				if (UImportSubsystem* Import = GEditor->GetEditorSubsystem<UImportSubsystem>())
				{
					ImportHandle = Import->OnAssetPostImport.AddStatic(&USoftbodyAuthoringData::HandleAssetPostImport);
				}
			}
		});
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSoftbodyEditorModule::RegisterMenus));
		// 구간 기록(Softbody.Record)이 에디터 월드에서 보는 카메라를 읽는다 — 런타임 모듈은 레벨 에디터 뷰포트를 모른다.
		SoftbodyRecord::GetEditorView = [](UWorld* World, FVector& OutLocation, FRotator& OutRotation, float& OutFov)
		{
			const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
			if (!Client || Client->GetWorld() != World || !Client->IsPerspective())
			{
				return false;
			}
			OutLocation = Client->GetViewLocation();
			OutRotation = Client->GetViewRotation();
			OutFov = Client->ViewFOV;
			return true;
		};
	}

	virtual void ShutdownModule() override
	{
		SoftbodyRecord::GetEditorView = nullptr;
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		OnPostEngineInit().Remove(PostEngineInitHandle);
		if (ImportHandle.IsValid() && GEditor)
		{
			if (UImportSubsystem* Import = GEditor->GetEditorSubsystem<UImportSubsystem>())
			{
				Import->OnAssetPostImport.Remove(ImportHandle);
			}
		}
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		for (const TCHAR* MenuName : { TEXT("ContentBrowser.AssetContextMenu.SkeletalMesh"), TEXT("ContentBrowser.AssetContextMenu.StaticMesh") })
		{
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(MenuName);
			FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
			Section.AddDynamicEntry("SoftbodyShapeField", FNewToolMenuSectionDelegate::CreateStatic(&FSoftbodyEditorModule::AddShapeFieldEntry));
		}
	}

	static void AddShapeFieldEntry(FToolMenuSection& Section)
	{
		UContentBrowserAssetContextMenuContext* Context = Section.FindContext<UContentBrowserAssetContextMenuContext>();
		if (!Context || Context->SelectedAssets.Num() == 0)
		{
			return;
		}
		Section.AddMenuEntry("SoftbodyBakeShapeField",
			LOCTEXT("BakeShapeField", "Softbody: Bake Shape Field"),
			LOCTEXT("BakeShapeFieldTip",
				"Lets this mesh press Softbody flesh with its real shape (every finger, the hollow of the palm) instead of its simple "
				"collision. Use it on hands and props that press. A Softbody character's own hands are set up in its Softbody Authoring "
				"Data instead. Takes a few seconds per mesh and saves the mesh."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Context]()
			{
				BakeShapeFields(Context->LoadSelectedObjects<UObject>());
			})));
	}

	static void BakeShapeFields(const TArray<UObject*>& Meshes)
	{
		FScopedSlowTask SlowTask((float)Meshes.Num(), LOCTEXT("BakingShapeField", "Softbody: Bake Shape Field"));
		SlowTask.MakeDialog();
		TArray<UPackage*> ToSave;
		{
			FScopedTransaction Transaction(LOCTEXT("BakeShapeFieldTransaction", "Softbody: Bake Shape Field"));
			for (UObject* Mesh : Meshes)
			{
				SlowTask.EnterProgressFrame(1.f, FText::Format(LOCTEXT("BakingShapeFieldOf", "Baking the shape field: {0}"),
					FText::FromString(GetNameSafe(Mesh))));
				if (!Mesh || (!Cast<UStaticMesh>(Mesh) && !Cast<USkeletalMesh>(Mesh)))
				{
					continue;
				}
				if (!SoftbodyBakeShared::IsEditableContent(Mesh))
				{
					UE_LOG(LogSoftbodyEditorMenu, Error,
						TEXT("%s is engine content and cannot be changed. Duplicate the mesh into your project's Content folder and bake the copy."),
						*Mesh->GetPathName());
					continue;
				}
				// 굽기 전부터 저장하지 않은 메시는 저장하지 않는다(사용자가 하던 작업이 섞였을 수 있다).
				const bool bWasDirty = Mesh->GetOutermost()->IsDirty();
				USoftbodyColliderField* Field = USoftbodyBakeLibrary::BakeColliderField(Mesh, 0.f, TArray<FName>());
				if (!Field)
				{
					UE_LOG(LogSoftbodyEditorMenu, Error, TEXT("%s: the shape field bake failed. See LogSoftbodyColliderField above."), *Mesh->GetName());
					continue;
				}
				Mesh->MarkPackageDirty();
				if (bWasDirty)
				{
					UE_LOG(LogSoftbodyEditorMenu, Warning,
						TEXT("%s already had unsaved changes before the bake, so it was not saved. Save it yourself when ready."), *Mesh->GetName());
				}
				else
				{
					ToSave.Add(Mesh->GetOutermost());
				}
				UE_LOG(LogSoftbodyEditorMenu, Display, TEXT("%s: shape field baked (%d parts)."), *Mesh->GetName(), Field->Parts.Num());
			}
		}
		if (ToSave.Num() > 0)
		{
			UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/true);
		}
	}

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle ImportHandle;
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSoftbodyEditorModule, SoftbodyEditor);
