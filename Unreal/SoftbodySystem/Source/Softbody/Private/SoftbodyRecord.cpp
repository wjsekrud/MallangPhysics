// Copyright (c) 2026 wjsekrud

// 디버그: 사용자가 켜고 끄는 구간 기록(콘솔 명령 Softbody.Record). 기록하는 동안에는 틱마다 움직인 액터의 자리, 보는 카메라, 장이 갱신됐는지만
// 적는다(무거운 일이 없어 끊김이 없다). 실제 장은 표시한 순간(mark)과 끝에만 캡처한다. 창 없는 실행(Tools/bench/invariance의 op replay)이 같은
// 움직임·조절값·콘솔 변수로 다시 돌려 아무 프레임이나 캡처하고, 표시한 순간의 실제 캡처와 견줘 다시 돌린 것이 같은지 확인한다.
#include "SoftbodyRecord.h"

#include "Animation/AnimInstance.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SoftbodyComponent.h"
#include "SoftbodyMeshData.h"
#include "SoftbodySurfaceField.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyRecord, Log, All);

TFunction<bool(UWorld*, FVector&, FRotator&, float&)> SoftbodyRecord::GetEditorView;

namespace
{
	/** 기록 프레임 상한(60 Hz로 10분). 넘으면 저장하고 멈춘다. */
	constexpr int32 MaxFrames = 36000;
	/** 루트 변환이 이만큼 바뀐 틱에만 그 액터 자리를 적는다(cm·쿼터니언 성분·배율). */
	constexpr float MoveTolerance = 1e-4f;

	TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const double Value : Values)
		{
			Out.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Out;
	}

	/** [x, y, z, qx, qy, qz, qw, sx, sy, sz] — 월드 변환. */
	TArray<TSharedPtr<FJsonValue>> TransformArray(const FTransform& T)
	{
		const FVector L = T.GetLocation();
		const FQuat Q = T.GetRotation();
		const FVector S = T.GetScale3D();
		return Numbers({L.X, L.Y, L.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z});
	}

	const TCHAR* WorldTypeName(const UWorld* World)
	{
		switch (World->WorldType)
		{
		case EWorldType::Editor: return TEXT("Editor");
		case EWorldType::PIE: return TEXT("PIE");
		case EWorldType::Game: return TEXT("Game");
		default: return TEXT("Other");
		}
	}

	/** 지금 이 월드를 보는 카메라: 에디터 월드는 레벨 에디터 뷰포트(에디터 모듈이 채운 함수), 그 밖은 플레이어 카메라. */
	bool ReadView(UWorld* World, FVector& Location, FRotator& Rotation, float& Fov)
	{
		if (SoftbodyRecord::GetEditorView && SoftbodyRecord::GetEditorView(World, Location, Rotation, Fov))
		{
			return true;
		}
		if (const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(World, 0))
		{
			Location = Camera->GetCameraLocation();
			Rotation = Camera->GetCameraRotation();
			Fov = Camera->GetFOVAngle();
			return true;
		}
		return false;
	}

	/** 기즈모 같은 에디터 전용·임시 액터는 다시 돌릴 월드에 없어 적지 않는다. */
	bool IsRecordedActor(const AActor* Actor)
	{
		return IsValid(Actor) && !Actor->IsEditorOnly() && !Actor->HasAnyFlags(RF_Transient) && Actor->GetRootComponent() != nullptr;
	}

	/** 소프트바디 주인의 스켈레탈 메시가 어떻게 자세를 잡는지와 뼈 월드 자리 — 다시 돌린 월드의 자세가 같은지 견주는 데 쓴다. */
	TSharedPtr<FJsonObject> PoseOf(const AActor* Owner)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Owner);
		for (const USkeletalMeshComponent* Mesh : Meshes)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			const EAnimationMode::Type Mode = Mesh->GetAnimationMode();
			Row->SetStringField(TEXT("mode"), Mode == EAnimationMode::AnimationBlueprint ? TEXT("AnimationBlueprint")
				: Mode == EAnimationMode::AnimationSingleNode ? TEXT("SingleNode") : TEXT("Custom"));
			Row->SetStringField(TEXT("anim_instance"), Mesh->GetAnimInstance() ? Mesh->GetAnimInstance()->GetClass()->GetPathName() : FString());
			Row->SetBoolField(TEXT("pause_anims"), Mesh->bPauseAnims);
			Row->SetNumberField(TEXT("rate_scale"), Mesh->GlobalAnimRateScale);
#if WITH_EDITOR
			Row->SetBoolField(TEXT("update_in_editor"), Mesh->GetUpdateAnimationInEditor());
#endif
			TSharedPtr<FJsonObject> Bones = MakeShared<FJsonObject>();
			for (int32 Index = 0; Index < Mesh->GetNumBones(); ++Index)
			{
				const FName Bone = Mesh->GetBoneName(Index);
				const FVector P = Mesh->GetBoneLocation(Bone, EBoneSpaces::WorldSpace);
				Bones->SetArrayField(Bone.ToString(), Numbers({P.X, P.Y, P.Z}));
			}
			Row->SetObjectField(TEXT("bones"), Bones);
			Out->SetObjectField(Mesh->GetName(), Row);
		}
		return Out;
	}

	/** 컴포넌트 하나: 주인·구운 데이터와 저장 안 된 변경 여부, 패널에서 바꿀 수 있는 모든 값(글자로 — apply가 그대로 되돌려 넣는다), 자세. */
	TSharedPtr<FJsonObject> ComponentRow(const USoftbodyComponent* Component)
	{
		const AActor* Owner = Component->GetOwner();
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("owner"), Owner ? Owner->GetActorNameOrLabel() : FString());
		Row->SetStringField(TEXT("owner_name"), Owner ? Owner->GetName() : FString());
		Row->SetStringField(TEXT("owner_class"), Owner ? Owner->GetClass()->GetPathName() : FString());
		Row->SetBoolField(TEXT("owner_package_dirty"), Owner && Owner->GetPackage()->IsDirty());
		Row->SetStringField(TEXT("component"), Component->GetName());
		const UObject* Data = Component->MeshData;
		Row->SetStringField(TEXT("mesh_data"), Data ? Data->GetPathName() : FString());
		Row->SetBoolField(TEXT("mesh_data_dirty"), Data && Data->GetPackage()->IsDirty());
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(USoftbodyComponent::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			// 기준값(Delta)으로 자기 자신을 준다: 기준값 없이 부르면 0과 같은 값을 "기본값과 같다"며 빈 글자로 냈다(첫 시험에서 Skin Tension 0 등).
			FString Text;
			It->ExportText_InContainer(0, Text, Component, Component, const_cast<USoftbodyComponent*>(Component), PPF_None);
			Props->SetStringField(It->GetName(), Text);
		}
		Row->SetObjectField(TEXT("props"), Props);
		if (Owner)
		{
			Row->SetObjectField(TEXT("pose"), PoseOf(Owner));
		}
		return Row;
	}

	struct FRecording
	{
		TWeakObjectPtr<UWorld> World;
		FString Folder;
		/** 0이 아니면 이 프레임마다 실제 장도 캡처한다(그때마다 멈칫한다). */
		int32 Every = 0;
		TArray<TWeakObjectPtr<USoftbodyComponent>> Components;
		TArray<double> LastUpdate;
		TMap<TWeakObjectPtr<AActor>, FTransform> LastTransform;
		TSet<TWeakObjectPtr<AActor>> Owners;
		TSharedPtr<FJsonObject> Root;
		TSharedPtr<FJsonObject> ActorInfo;
		TArray<TSharedPtr<FJsonValue>> Frames;
		TArray<TSharedPtr<FJsonValue>> Marks;
		double StartTime = 0.0;
		uint64 StartFrame = 0;
		int32 NumMarks = 0;
		int64 NumMoves = 0;
		FDelegateHandle TickHandle;
	};
	TUniquePtr<FRecording> GRecording;

	void StopRecording(FOutputDevice* Ar, bool bCapture);

	/** 지금 장들을 캡처해 기록 폴더 marks/에 둔다. 사본은 이번 틱(또는 다음에 도는) 갱신 끝에 뜬다 — 그 틱이 기록의 프레임 i. */
	int32 Mark(const FString& Note, bool bAuto)
	{
		FRecording& R = *GRecording;
		TArray<USoftbodyComponent*> Components;
		for (const TWeakObjectPtr<USoftbodyComponent>& Weak : R.Components)
		{
			if (USoftbodyComponent* Component = Weak.Get())
			{
				Components.Add(Component);
			}
		}
		++R.NumMarks;
		const FString Name = FString::Printf(TEXT("%02d_%s_f%d"), R.NumMarks,
			*FPaths::MakeValidFileName(Note.IsEmpty() ? FString(TEXT("mark")) : Note, TEXT('_')), R.Frames.Num());
		const int32 Requested = SoftbodyCapture::CaptureInto(Components, false, R.Folder / TEXT("marks") / Name,
			FString::Printf(TEXT("; Softbody.Record mark %d (%s) at recorded frame %d"), R.NumMarks, *Note, R.Frames.Num()));
		const UWorld* World = R.World.Get();
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("i"), R.Frames.Num());
		Row->SetNumberField(TEXT("t"), World ? World->GetTimeSeconds() - R.StartTime : -1.0);
		Row->SetStringField(TEXT("note"), Note);
		Row->SetStringField(TEXT("folder"), TEXT("marks/") + Name);
		Row->SetNumberField(TEXT("requested"), Requested);
		Row->SetBoolField(TEXT("auto"), bAuto);
		R.Marks.Add(MakeShared<FJsonValueObject>(Row));
		return Requested;
	}

	/** 월드 틱 맨 앞(입력으로 옮긴 자리가 들어간 뒤, 액터·장이 돌기 전): 이 틱이 볼 자리·카메라를 적는다. */
	void OnPreActorTick(UWorld* TickWorld, ELevelTick, float DeltaSeconds)
	{
		if (!GRecording)
		{
			return;
		}
		FRecording& R = *GRecording;
		UWorld* World = R.World.Get();
		if (!World)
		{
			// PIE가 끝나는 등 기록하던 월드가 사라졌다: 있는 것만 저장한다.
			StopRecording(nullptr, false);
			return;
		}
		if (TickWorld != World)
		{
			return;
		}
		const bool bFirst = R.Frames.Num() == 0;
		TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetNumberField(TEXT("t"), World->GetTimeSeconds() - R.StartTime);
		Frame->SetNumberField(TEXT("dt"), DeltaSeconds);
		Frame->SetNumberField(TEXT("frame"), (double)(GFrameCounter - R.StartFrame));
		FVector Location;
		FRotator Rotation;
		float Fov = 90.f;
		if (ReadView(World, Location, Rotation, Fov))
		{
			Frame->SetArrayField(TEXT("cam"), Numbers({Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll, Fov}));
		}
		TSharedPtr<FJsonObject> Moved = MakeShared<FJsonObject>();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsRecordedActor(Actor))
			{
				continue;
			}
			const USceneComponent* Root = Actor->GetRootComponent();
			const FTransform T = Root->GetComponentTransform();
			bool bWrite = false;
			if (FTransform* Last = R.LastTransform.Find(Actor))
			{
				bWrite = !Last->Equals(T, MoveTolerance);
				*Last = T;
			}
			else
			{
				// 첫 프레임에는 움직일 수 있는 액터와 소프트바디 주인만 적는다(저장된 레벨과 다를 수 있는 자리 — 정적 액터는 실행 중에 못 옮긴다).
				// 기록 중 새로 생긴 액터는 적는다.
				R.LastTransform.Add(Actor, T);
				bWrite = !bFirst || Root->Mobility == EComponentMobility::Movable || R.Owners.Contains(Actor);
			}
			if (!bWrite)
			{
				continue;
			}
			const FString Label = Actor->GetActorNameOrLabel();
			Moved->SetArrayField(Label, TransformArray(T));
			if (!R.ActorInfo->HasField(Label))
			{
				TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
				Info->SetStringField(TEXT("name"), Actor->GetName());
				Info->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
				Info->SetStringField(TEXT("mobility"), Root->Mobility == EComponentMobility::Movable ? TEXT("Movable")
					: Root->Mobility == EComponentMobility::Stationary ? TEXT("Stationary") : TEXT("Static"));
				R.ActorInfo->SetObjectField(Label, Info);
			}
			++R.NumMoves;
		}
		if (Moved->Values.Num() > 0)
		{
			Frame->SetObjectField(TEXT("actors"), Moved);
		}
		// 지난 틱에 갱신이 돈 장(기록 첫 줄의 components 차례).
		TArray<TSharedPtr<FJsonValue>> Updated;
		for (int32 Index = 0; Index < R.Components.Num(); ++Index)
		{
			const USoftbodyComponent* Component = R.Components[Index].Get();
			const USoftbodySurfaceField* Field = Component ? Component->GetSurfaceField() : nullptr;
			if (Field && Field->GetLastUpdateTime() != R.LastUpdate[Index])
			{
				R.LastUpdate[Index] = Field->GetLastUpdateTime();
				Updated.Add(MakeShared<FJsonValueNumber>(Index));
			}
		}
		if (Updated.Num() > 0)
		{
			Frame->SetArrayField(TEXT("updated"), Updated);
		}
		R.Frames.Add(MakeShared<FJsonValueObject>(Frame));
		if (R.Every > 0 && R.Frames.Num() % R.Every == 0)
		{
			Mark(TEXT("auto"), true);
		}
		if (R.Frames.Num() >= MaxFrames)
		{
			UE_LOG(LogSoftbodyRecord, Warning, TEXT("[Softbody.Record] %d frames recorded - stopping"), MaxFrames);
			StopRecording(nullptr, true);
		}
	}

	void StopRecording(FOutputDevice* Ar, bool bCapture)
	{
		if (!GRecording)
		{
			return;
		}
		FRecording& R = *GRecording;
		FWorldDelegates::OnWorldPreActorTick.Remove(R.TickHandle);
		if (bCapture && R.World.IsValid())
		{
			Mark(TEXT("end"), false);
		}
		R.Root->SetArrayField(TEXT("frames"), R.Frames);
		R.Root->SetArrayField(TEXT("marks"), R.Marks);
		R.Root->SetObjectField(TEXT("actors"), R.ActorInfo);
		R.Root->SetStringField(TEXT("stopped"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(R.Root.ToSharedRef(), Writer);
		const FString Path = R.Folder / TEXT("record.json");
		const bool bSaved = FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString Line = FString::Printf(TEXT("[Softbody.Record] stopped: %d frames, %lld actor moves, %d marks -> %s%s"),
			R.Frames.Num(), R.NumMoves, R.Marks.Num(), *Path, bSaved ? TEXT("") : TEXT(" (could not write the file)"));
		UE_LOG(LogSoftbodyRecord, Log, TEXT("%s"), *Line);
		if (Ar)
		{
			Ar->Logf(TEXT("%s"), *Line);
		}
		GRecording.Reset();
	}

	void StartRecording(UWorld* CommandWorld, const FString& Filter, int32 Every, FOutputDevice& Ar)
	{
		// 고르는 월드는 Softbody.CaptureField와 같다: PIE(게임 월드)에 소프트바디가 있으면 거기, 없으면 명령을 받은 월드.
		TArray<USoftbodyComponent*> Components;
		UWorld* World = nullptr;
		for (int32 Pass = 0; Pass < 2 && Components.Num() == 0; ++Pass)
		{
			for (TObjectIterator<USoftbodyComponent> It; It; ++It)
			{
				USoftbodyComponent* Component = *It;
				UWorld* ComponentWorld = IsValid(Component) && !Component->IsTemplate() ? Component->GetWorld() : nullptr;
				if (!ComponentWorld || (Pass == 0 ? !ComponentWorld->IsGameWorld() : ComponentWorld != CommandWorld) || !Component->GetSurfaceField()
					|| (World && ComponentWorld != World))
				{
					continue;
				}
				const FString OwnerName = Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString();
				if (!Filter.IsEmpty() && !OwnerName.Contains(Filter) && !Component->GetName().Contains(Filter))
				{
					continue;
				}
				World = ComponentWorld;
				Components.Add(Component);
			}
		}
		if (!World)
		{
			Ar.Logf(TEXT("[Softbody.Record] No Softbody component with contact turned on was found (name filter: '%s')"), *Filter);
			return;
		}
		GRecording = MakeUnique<FRecording>();
		FRecording& R = *GRecording;
		R.World = World;
		R.Every = FMath::Max(Every, 0);
		R.StartTime = World->GetTimeSeconds();
		R.StartFrame = GFrameCounter;
		R.Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SoftbodyRecord") / FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
		IFileManager::Get().MakeDirectory(*R.Folder, true);
		R.Root = MakeShared<FJsonObject>();
		R.ActorInfo = MakeShared<FJsonObject>();
		R.Root->SetNumberField(TEXT("version"), 1);
		R.Root->SetStringField(TEXT("started"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		TSharedPtr<FJsonObject> WorldRow = MakeShared<FJsonObject>();
		WorldRow->SetStringField(TEXT("map"), UWorld::RemovePIEPrefix(World->GetPackage()->GetName()));
		WorldRow->SetStringField(TEXT("type"), WorldTypeName(World));
		WorldRow->SetBoolField(TEXT("level_dirty"), World->GetPackage()->IsDirty());
		R.Root->SetObjectField(TEXT("world"), WorldRow);
		R.Root->SetStringField(TEXT("filter"), Filter);
		R.Root->SetNumberField(TEXT("every"), R.Every);
		TArray<TSharedPtr<FJsonValue>> Rows;
		// 시작할 때 이미 무언가 닿아 있으면 그 전 이력(들어온 방향 표지·끌림·덩어리)은 기록에 없어, 다시 돌린 장이 그 이력만큼 다를 수 있다
		// (시험: 눌린 채 시작하면 눌린 동안 수백 칸이 몇 cm 달랐고, 닿지 않은 채 시작하면 0.02 cm 안).
		TArray<FString> Touching;
		for (USoftbodyComponent* Component : Components)
		{
			R.Components.Add(Component);
			const USoftbodySurfaceField* Field = Component->GetSurfaceField();
			if (Field && (Field->HasCandidates() || Field->HasTextures()))
			{
				Touching.Add(Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : Component->GetName());
			}
			R.LastUpdate.Add(Field ? Field->GetLastUpdateTime() : -1.0);
			R.Owners.Add(Component->GetOwner());
			Rows.Add(MakeShared<FJsonValueObject>(ComponentRow(Component)));
		}
		R.Root->SetArrayField(TEXT("components"), Rows);
		TArray<TSharedPtr<FJsonValue>> TouchingRows;
		for (const FString& Name : Touching)
		{
			TouchingRows.Add(MakeShared<FJsonValueString>(Name));
		}
		R.Root->SetArrayField(TEXT("touching_at_start"), TouchingRows);
		// 소프트바디 콘솔 변수(개발 스위치 포함) — 다시 돌릴 때 같은 값으로 맞춘다.
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
			[&Variables](const TCHAR* Name, IConsoleObject* Object)
			{
				if (IConsoleVariable* Variable = Object ? Object->AsVariable() : nullptr)
				{
					Variables->SetStringField(Name, Variable->GetString());
				}
			}), TEXT("softbody."));
		R.Root->SetObjectField(TEXT("cvars"), Variables);
		R.TickHandle = FWorldDelegates::OnWorldPreActorTick.AddStatic(&OnPreActorTick);
		FString Names;
		for (const USoftbodyComponent* Component : Components)
		{
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + (Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString(TEXT("NoOwner")))
				+ TEXT(".") + Component->GetName();
		}
		Ar.Logf(TEXT("[Softbody.Record] recording %s world %s (%s)%s -> %s. 'Softbody.Record mark [note]' captures the fields now; 'Softbody.Record' again stops."),
			WorldTypeName(World), *UWorld::RemovePIEPrefix(World->GetPackage()->GetName()), *Names,
			R.Every > 0 ? *FString::Printf(TEXT(", fields every %d frames"), R.Every) : TEXT(""), *R.Folder);
		if (Touching.Num() > 0)
		{
			Ar.Logf(ELogVerbosity::Warning, TEXT("[Softbody.Record] a collider is already near %s. That is fine if nothing presses yet; if something already presses, ")
				TEXT("the replay cannot know how it got there and pressed areas may differ - stop, move away until the dent is gone and start again."),
				*FString::Join(Touching, TEXT(", ")));
		}
	}

	/** 기록의 조절값·콘솔 변수를 이 월드에 맞추고, 게임 월드면 기록 카메라용 카메라 액터(SoftbodyRecordView)를 세워 그것으로 본다. */
	void ApplyRecording(const FString& PathArgument, UWorld* World, FOutputDevice& Ar)
	{
		FString Path = PathArgument.TrimQuotes();
		if (FPaths::DirectoryExists(Path))
		{
			Path = Path / TEXT("record.json");
		}
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!World || !FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			Ar.Logf(TEXT("[Softbody.Record] apply: could not read %s"), *Path);
			return;
		}
		TArray<FString> Lines;
		int32 NumValues = 0;
		int32 NumMissing = 0;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Root->TryGetArrayField(TEXT("components"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject> Row = Value->AsObject();
				const FString OwnerLabel = Row->GetStringField(TEXT("owner"));
				const FString ComponentName = Row->GetStringField(TEXT("component"));
				USoftbodyComponent* Found = nullptr;
				for (TActorIterator<AActor> It(World); It && !Found; ++It)
				{
					if (It->GetActorNameOrLabel() != OwnerLabel)
					{
						continue;
					}
					TInlineComponentArray<USoftbodyComponent*> Components(*It);
					for (USoftbodyComponent* Component : Components)
					{
						if (Component->GetName() == ComponentName)
						{
							Found = Component;
							break;
						}
					}
				}
				if (!Found)
				{
					++NumMissing;
					Lines.Add(FString::Printf(TEXT("  not found: %s.%s"), *OwnerLabel, *ComponentName));
					continue;
				}
				const TSharedPtr<FJsonObject>* Props = nullptr;
				if (!Row->TryGetObjectField(TEXT("props"), Props))
				{
					continue;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
				{
					FProperty* Property = FindFProperty<FProperty>(USoftbodyComponent::StaticClass(), *Pair.Key);
					FString Want;
					if (!Property || Property->HasAnyPropertyFlags(CPF_EditConst) || !Pair.Value->TryGetString(Want))
					{
						continue;
					}
					FString Now;
					Property->ExportText_InContainer(0, Now, Found, Found, Found, PPF_None);
					if (Now == Want || Want.IsEmpty())
					{
						continue;
					}
					Property->ImportText_InContainer(*Want, Found, Found, PPF_None);
					Lines.Add(FString::Printf(TEXT("  %s.%s %s: %s -> %s"), *OwnerLabel, *ComponentName, *Pair.Key, *Now, *Want));
					++NumValues;
				}
			}
		}
		int32 NumVariables = 0;
		const TSharedPtr<FJsonObject>* Variables = nullptr;
		if (Root->TryGetObjectField(TEXT("cvars"), Variables))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Variables)->Values)
			{
				IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key);
				FString Want;
				if (!Variable || Variable->TestFlags(ECVF_ReadOnly) || !Pair.Value->TryGetString(Want) || Variable->GetString() == Want)
				{
					continue;
				}
				Lines.Add(FString::Printf(TEXT("  %s: %s -> %s"), *Pair.Key, *Variable->GetString(), *Want));
				Variable->Set(*Want, ECVF_SetByConsole);
				++NumVariables;
			}
		}
		bool bView = false;
		if (World->IsGameWorld())
		{
			ACameraActor* View = nullptr;
			for (TActorIterator<ACameraActor> It(World); It; ++It)
			{
				if (It->GetName().StartsWith(TEXT("SoftbodyRecordView")))
				{
					View = *It;
				}
			}
			if (!View)
			{
				FActorSpawnParameters Params;
				Params.Name = TEXT("SoftbodyRecordView");
				View = World->SpawnActor<ACameraActor>(Params);
			}
			APlayerController* Controller = World->GetFirstPlayerController();
			if (View && Controller)
			{
				Controller->SetViewTargetWithBlend(View, 0.f);
				bView = true;
			}
		}
		const FString Line = FString::Printf(TEXT("[Softbody.Record] applied %s: %d component values, %d console variables, %d components not found%s\n%s"),
			*Path, NumValues, NumVariables, NumMissing, bView ? TEXT(", viewing through SoftbodyRecordView") : TEXT(""), *FString::Join(Lines, TEXT("\n")));
		UE_LOG(LogSoftbodyRecord, Log, TEXT("%s"), *Line);
		Ar.Logf(TEXT("%s"), *Line);
	}

	void RecordCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		const FString Verb = Args.Num() > 0 ? Args[0].ToLower() : FString();
		if (Verb == TEXT("apply"))
		{
			ApplyRecording(Args.Num() > 1 ? FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" ")) : FString(), World, Ar);
			return;
		}
		if (Verb == TEXT("mark"))
		{
			if (!GRecording)
			{
				Ar.Logf(TEXT("[Softbody.Record] not recording - 'Softbody.Record' starts"));
				return;
			}
			const FString Note = Args.Num() > 1 ? FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT("_")) : FString();
			const int32 Requested = Mark(Note, false);
			Ar.Logf(TEXT("[Softbody.Record] mark %d at frame %d: capturing %d components"), GRecording->NumMarks, GRecording->Frames.Num(), Requested);
			return;
		}
		if (Verb == TEXT("stop") || (Verb.IsEmpty() && GRecording))
		{
			if (!GRecording)
			{
				Ar.Logf(TEXT("[Softbody.Record] not recording"));
				return;
			}
			StopRecording(&Ar, true);
			return;
		}
		if (GRecording)
		{
			Ar.Logf(TEXT("[Softbody.Record] already recording -> %s ('Softbody.Record' stops)"), *GRecording->Folder);
			return;
		}
		FString Filter;
		int32 Every = 0;
		for (int32 Index = Verb == TEXT("start") ? 1 : 0; Index < Args.Num(); ++Index)
		{
			if (Args[Index].StartsWith(TEXT("every="), ESearchCase::IgnoreCase))
			{
				Every = FCString::Atoi(*Args[Index].RightChop(6));
			}
			else
			{
				Filter = Args[Index];
			}
		}
		StartRecording(World, Filter, Every, Ar);
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyRecordCommand(
		TEXT("Softbody.Record"),
		TEXT("Debug: records a stretch you choose so it can be replayed offline. 'Softbody.Record' starts (optional: an actor or component name, ")
		TEXT("every=N to also capture the fields every N frames - each capture hitches); run it again to stop. While recording it writes, each frame, ")
		TEXT("the actors that moved, the view camera and which fields updated - light, so no hitch. 'Softbody.Record mark [note]' captures the fields now ")
		TEXT("(like Softbody.CaptureField). Stopping captures once more and saves Saved/SoftbodyRecord/<time>/record.json. ")
		TEXT("'Softbody.Record apply <record>' sets the recorded component values and softbody console variables in this world (used by the replay runner)."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&RecordCommand));
}
