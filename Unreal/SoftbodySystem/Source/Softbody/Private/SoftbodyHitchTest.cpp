// Copyright (c) 2026 wjsekrud

#include "SoftbodyHitchTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderTimer.h"
#include "SoftbodyComponent.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyProfiling.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyHitch, Log, All);

namespace
{
	TAutoConsoleVariable<int32> CVarHitchTestStart(
		TEXT("softbody.HitchTest.Start"), 0,
		TEXT("Hitch test: 1 = run Softbody.HitchTest from the frame play begins (includes level start). For automated runs."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarHitchTestQuit(
		TEXT("softbody.HitchTest.Quit"), 0,
		TEXT("Hitch test: 1 = close the game when the test ends (standalone game only, never the editor)."),
		ECVF_Default);

	/** 단계 이름과 길이(초). 순서는 Tick·BeginPhase의 번호와 같다. */
	struct FPhaseDef
	{
		const TCHAR* Name;
		double Seconds;
	};
	const FPhaseDef GPhases[] = {
		{TEXT("start"), 10.0},        // 레벨 시작(또는 시험 시작): 무버가 누르며 첫 접촉이 일어난다
		{TEXT("off_all"), 4.0},       // 모든 소프트바디 Deactivate
		{TEXT("on_all_warm"), 8.0},   // 곧바로 모두 Activate(메모리 유예 안)
		{TEXT("toggle_one"), 5.0},    // 가장 큰 캐리어 하나를 0.5초마다 껐다 켬
		{TEXT("off_all_long"), 14.0}, // 모두 끄고 유예(10초)를 넘겨 기다림 — 메모리를 놓는 프레임
		{TEXT("gc"), 3.0},            // 전체 GC(쿡 빌드는 여기서 접촉 지도가 내려간다)
		{TEXT("on_all_cold"), 10.0},  // 유예·GC 뒤 모두 Activate
		{TEXT("spawn"), 6.0},         // 소프트바디 액터 복제 스폰
		{TEXT("destroy"), 4.0},       // 복제 지우기
		{TEXT("contact_off"), 4.0},   // 모두 Enable Contact 끔
		{TEXT("contact_on"), 8.0},    // 모두 Enable Contact 켬
	};
	constexpr int32 NumPhases = UE_ARRAY_COUNT(GPhases);
	constexpr double ToggleInterval = 0.5;
	constexpr int32 MaxToggles = 10;
	constexpr int32 MaxSpawnTemplates = 4;

	double CyclesToMs(uint32 Cycles)
	{
		return FPlatformTime::ToMilliseconds(Cycles);
	}

	void HitchTestCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		USoftbodyHitchTestSubsystem* Test = World ? World->GetSubsystem<USoftbodyHitchTestSubsystem>() : nullptr;
		if (!Test)
		{
			Ar.Logf(TEXT("[Softbody.HitchTest] Only works while playing (PIE or game)."));
			return;
		}
		if (Test->Start())
		{
			Ar.Logf(TEXT("[Softbody.HitchTest] Started (about 76 s). Results go to the log and Saved/Profiling/SoftbodyHitch."));
		}
		else
		{
			Ar.Logf(TEXT("[Softbody.HitchTest] Already running."));
		}
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyHitchTestCommand(
		TEXT("Softbody.HitchTest"),
		TEXT("Hitch test: turns every Softbody component off and on, toggles the largest one, waits past the memory grace, runs a full GC, spawns ")
		TEXT("and destroys copies and toggles Enable Contact, recording frame, game, render, RHI and GPU times each frame. Writes a summary to the ")
		TEXT("log and a CSV to Saved/Profiling/SoftbodyHitch. Changes the level's Softbody components - use it only for testing."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&HitchTestCommand));

	/** 중앙값(복사본을 정렬한다). */
	float Median(TArray<float> Values)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		return Values[Values.Num() / 2];
	}
}

bool USoftbodyHitchTestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// 시험 도구라 출시용(Shipping) 빌드의 월드에는 만들지 않는다.
	return !UE_BUILD_SHIPPING && (WorldType == EWorldType::Game || WorldType == EWorldType::PIE);
}

void USoftbodyHitchTestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void USoftbodyHitchTestSubsystem::Deinitialize()
{
	bRunning = false;
	Super::Deinitialize();
}

void USoftbodyHitchTestSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (CVarHitchTestStart.GetValueOnGameThread() != 0)
	{
		Start();
	}
}

TStatId USoftbodyHitchTestSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USoftbodyHitchTestSubsystem, STATGROUP_Tickables);
}

bool USoftbodyHitchTestSubsystem::Start()
{
	if (bRunning)
	{
		return false;
	}
	bRunning = true;
	Frames.Reset();
	Frames.Reserve(8192);
	Spawned.Reset();
	PhaseIndex = 0;
	PhaseStart = FPlatformTime::Seconds();
	LastTickTime = PhaseStart;
	PendingActionMs = 0.0;
	UE_LOG(LogSoftbodyHitch, Display, TEXT("Hitch test started: %d phases."), NumPhases);
	BeginPhase(0);
	return true;
}

void USoftbodyHitchTestSubsystem::GatherComponents(TArray<USoftbodyComponent*>& Out) const
{
	Out.Reset();
	const UWorld* World = GetWorld();
	for (TObjectIterator<USoftbodyComponent> It; It; ++It)
	{
		if (IsValid(*It) && It->GetWorld() == World && It->IsRegistered())
		{
			Out.Add(*It);
		}
	}
}

void USoftbodyHitchTestSubsystem::SetAllActive(bool bActive)
{
	TArray<USoftbodyComponent*> Components;
	GatherComponents(Components);
	const double Begin = FPlatformTime::Seconds();
	for (USoftbodyComponent* Component : Components)
	{
		Component->SetActive(bActive);
	}
	PendingActionMs += (FPlatformTime::Seconds() - Begin) * 1000.0;
	UE_LOG(LogSoftbodyHitch, Display, TEXT("  %s %d components (%.2f ms on the game thread)."), bActive ? TEXT("Activate") : TEXT("Deactivate"),
		Components.Num(), (FPlatformTime::Seconds() - Begin) * 1000.0);
}

void USoftbodyHitchTestSubsystem::SetAllContact(bool bContact)
{
	TArray<USoftbodyComponent*> Components;
	GatherComponents(Components);
	for (USoftbodyComponent* Component : Components)
	{
		Component->bEnableContact = bContact;
	}
	UE_LOG(LogSoftbodyHitch, Display, TEXT("  Enable Contact %s on %d components (applied on their next tick)."), bContact ? TEXT("on") : TEXT("off"),
		Components.Num());
}

void USoftbodyHitchTestSubsystem::SpawnCopies()
{
	UWorld* World = GetWorld();
	TArray<USoftbodyComponent*> Components;
	GatherComponents(Components);
	// 큰 캐리어부터, Mesh Data마다 하나씩.
	Components.Sort([](const USoftbodyComponent& A, const USoftbodyComponent& B)
	{
		const int32 RA = A.MeshData ? A.MeshData->CarrierResolution : 0;
		const int32 RB = B.MeshData ? B.MeshData->CarrierResolution : 0;
		return RA > RB;
	});
	TSet<const USoftbodyMeshData*> Seen;
	TArray<AActor*> Templates;
	for (USoftbodyComponent* Component : Components)
	{
		AActor* Owner = Component->GetOwner();
		if (!Owner || !Component->MeshData || Seen.Contains(Component->MeshData))
		{
			continue;
		}
		Seen.Add(Component->MeshData);
		Templates.Add(Owner);
		if (Templates.Num() >= MaxSpawnTemplates)
		{
			break;
		}
	}
	const double Begin = FPlatformTime::Seconds();
	for (AActor* Template : Templates)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const FTransform Transform(Template->GetActorRotation(), Template->GetActorLocation() + FVector(0.0, 0.0, 3000.0), Template->GetActorScale3D());
		AActor* Copy = World->SpawnActor(Template->GetClass(), &Transform, Params);
		if (!Copy)
		{
			continue;
		}
		// 블루프린트 컴포넌트의 인스턴스 값(메시·머티리얼·Mesh Data)은 템플릿 복제로 넘어오지 않는다 — 같은 이름의 컴포넌트에서 옮긴다. 소프트바디는 다음
		// 틱에 Mesh Data가 바뀐 것을 보고 다시 세팅한다(실행 중 Mesh Data 바꾸기 경로도 같이 잰다).
		for (UActorComponent* Component : Copy->GetComponents())
		{
			const UActorComponent* Source = nullptr;
			for (const UActorComponent* Other : Template->GetComponents())
			{
				if (Other && Other->GetFName() == Component->GetFName() && Other->GetClass() == Component->GetClass())
				{
					Source = Other;
					break;
				}
			}
			if (!Source)
			{
				continue;
			}
			auto CopyMaterials = [](const UMeshComponent* From, UMeshComponent* To)
			{
				for (int32 Slot = 0; Slot < From->GetNumMaterials(); ++Slot)
				{
					UMaterialInterface* Material = From->GetMaterial(Slot);
					if (const UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Material))
					{
						Material = MID->Parent;
					}
					To->SetMaterial(Slot, Material);
				}
			};
			if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
			{
				const UStaticMeshComponent* From = CastChecked<UStaticMeshComponent>(Source);
				Static->SetStaticMesh(From->GetStaticMesh());
				CopyMaterials(From, Static);
			}
			else if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
			{
				const USkeletalMeshComponent* From = CastChecked<USkeletalMeshComponent>(Source);
				Skeletal->SetSkeletalMesh(From->GetSkeletalMeshAsset());
				CopyMaterials(From, Skeletal);
			}
			else if (USoftbodyComponent* Softbody = Cast<USoftbodyComponent>(Component))
			{
				Softbody->MeshData = CastChecked<USoftbodyComponent>(Source)->MeshData;
			}
		}
		Spawned.Add(Copy);
	}
	PendingActionMs += (FPlatformTime::Seconds() - Begin) * 1000.0;
	UE_LOG(LogSoftbodyHitch, Display, TEXT("  Spawned %d copies (%.2f ms on the game thread)."), Spawned.Num(), (FPlatformTime::Seconds() - Begin) * 1000.0);
}

void USoftbodyHitchTestSubsystem::DestroyCopies()
{
	const double Begin = FPlatformTime::Seconds();
	int32 Destroyed = 0;
	for (const TWeakObjectPtr<AActor>& Copy : Spawned)
	{
		if (AActor* Actor = Copy.Get())
		{
			Destroyed += Actor->Destroy() ? 1 : 0;
		}
	}
	Spawned.Reset();
	PendingActionMs += (FPlatformTime::Seconds() - Begin) * 1000.0;
	UE_LOG(LogSoftbodyHitch, Display, TEXT("  Destroyed %d copies (%.2f ms on the game thread)."), Destroyed, (FPlatformTime::Seconds() - Begin) * 1000.0);
}

void USoftbodyHitchTestSubsystem::BeginPhase(int32 Phase)
{
	UE_LOG(LogSoftbodyHitch, Display, TEXT("Phase %d %s (%.0f s)."), Phase, GPhases[Phase].Name, GPhases[Phase].Seconds);
	switch (Phase)
	{
	case 1: // off_all
	case 4: // off_all_long
		SetAllActive(false);
		break;
	case 2: // on_all_warm
	case 6: // on_all_cold
		SetAllActive(true);
		break;
	case 3: // toggle_one
	{
		TArray<USoftbodyComponent*> Components;
		GatherComponents(Components);
		USoftbodyComponent* Largest = nullptr;
		for (USoftbodyComponent* Component : Components)
		{
			const int32 Resolution = Component->MeshData ? Component->MeshData->CarrierResolution : 0;
			if (!Largest || Resolution > (Largest->MeshData ? Largest->MeshData->CarrierResolution : 0))
			{
				Largest = Component;
			}
		}
		ToggleTarget = Largest;
		NextToggle = ToggleInterval;
		Toggles = 0;
		UE_LOG(LogSoftbodyHitch, Display, TEXT("  Toggling %s every %.1f s."), Largest ? *Largest->GetPathName() : TEXT("<none>"), ToggleInterval);
		break;
	}
	case 5: // gc
	{
		const double Begin = FPlatformTime::Seconds();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		const double Ms = (FPlatformTime::Seconds() - Begin) * 1000.0;
		PendingActionMs += Ms;
		UE_LOG(LogSoftbodyHitch, Display, TEXT("  Full GC %.2f ms."), Ms);
		break;
	}
	case 7: // spawn
		SpawnCopies();
		break;
	case 8: // destroy
		DestroyCopies();
		break;
	case 9: // contact_off
		SetAllContact(false);
		break;
	case 10: // contact_on
		SetAllContact(true);
		break;
	default:
		break;
	}
}

void USoftbodyHitchTestSubsystem::StepPhase(int32 Phase, double PhaseTime)
{
	if (Phase != 3)
	{
		return;
	}
	USoftbodyComponent* Target = ToggleTarget.Get();
	if (!Target || Toggles >= MaxToggles || PhaseTime < NextToggle)
	{
		return;
	}
	const bool bOn = !Target->IsActive();
	const double Begin = FPlatformTime::Seconds();
	Target->SetActive(bOn);
	PendingActionMs += (FPlatformTime::Seconds() - Begin) * 1000.0;
	++Toggles;
	NextToggle += ToggleInterval;
}

void USoftbodyHitchTestSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bRunning)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	SoftbodyProfiling::FResourceCounters& Counters = SoftbodyProfiling::GetResourceCounters();

	FFrame& Frame = Frames.AddDefaulted_GetRef();
	Frame.Time = Now;
	Frame.Phase = PhaseIndex;
	Frame.FrameMs = (float)((Now - LastTickTime) * 1000.0);
	Frame.GameMs = (float)CyclesToMs(GGameThreadTime);
	Frame.RenderMs = (float)CyclesToMs(GRenderThreadTime);
	Frame.RhiMs = (float)CyclesToMs(GRHIThreadTime);
	Frame.GpuMs = (float)CyclesToMs(RHIGetGPUFrameCycles(0));
	Frame.TargetsCreated = Counters.TargetsCreated;
	Frame.CreatedMB = (float)((double)Counters.BytesCreated / (1024.0 * 1024.0));
	Frame.TargetsReleased = Counters.TargetsReleased;
	Frame.ReleasedMB = (float)((double)Counters.BytesReleased / (1024.0 * 1024.0));
	Frame.ContactMapRequests = Counters.ContactMapRequests;
	Counters = SoftbodyProfiling::FResourceCounters();
	LastTickTime = Now;

	// 단계: 시간이 다 되면 다음 단계의 일을 이 프레임에 한다(그 시간은 다음 프레임 기록의 ActionMs로 간다).
	const double PhaseTime = Now - PhaseStart;
	StepPhase(PhaseIndex, PhaseTime);
	if (PhaseTime >= GPhases[PhaseIndex].Seconds)
	{
		if (PhaseIndex + 1 >= NumPhases)
		{
			Frame.ActionMs = (float)PendingActionMs;
			PendingActionMs = 0.0;
			Finish();
			return;
		}
		if (PhaseIndex == 3)
		{
			// 왕복이 꺼진 채 끝났으면 켜 둔다(다음 단계가 모두 끈다).
			if (USoftbodyComponent* Target = ToggleTarget.Get(); Target && !Target->IsActive())
			{
				Target->SetActive(true);
			}
		}
		++PhaseIndex;
		PhaseStart = Now;
		BeginPhase(PhaseIndex);
	}
	Frame.ActionMs = (float)PendingActionMs;
	PendingActionMs = 0.0;
}

void USoftbodyHitchTestSubsystem::Finish()
{
	bRunning = false;
	DestroyCopies();

	// 단계마다 요약: 중앙값과 최댓값(스레드마다), 만들고 놓은 렌더 타깃.
	FString Report = FString::Printf(TEXT("Softbody hitch test, %d frames (ms; median / max, spike = max - median):"), Frames.Num());
	for (int32 Phase = 0; Phase < NumPhases; ++Phase)
	{
		TArray<float> FrameMs, GameMs, RenderMs, RhiMs, GpuMs;
		float MaxFrame = 0.f, MaxGame = 0.f, MaxRender = 0.f, MaxRhi = 0.f, MaxGpu = 0.f, MaxAction = 0.f;
		int32 Created = 0, Released = 0, Requests = 0;
		float CreatedMB = 0.f, ReleasedMB = 0.f;
		double MaxFrameAt = 0.0;
		double Begin = -1.0;
		for (const FFrame& Frame : Frames)
		{
			if (Frame.Phase != Phase)
			{
				continue;
			}
			if (Begin < 0.0)
			{
				Begin = Frame.Time;
			}
			FrameMs.Add(Frame.FrameMs);
			GameMs.Add(Frame.GameMs);
			RenderMs.Add(Frame.RenderMs);
			RhiMs.Add(Frame.RhiMs);
			GpuMs.Add(Frame.GpuMs);
			if (Frame.FrameMs > MaxFrame)
			{
				MaxFrame = Frame.FrameMs;
				MaxFrameAt = Frame.Time - Begin;
			}
			MaxGame = FMath::Max(MaxGame, Frame.GameMs);
			MaxRender = FMath::Max(MaxRender, Frame.RenderMs);
			MaxRhi = FMath::Max(MaxRhi, Frame.RhiMs);
			MaxGpu = FMath::Max(MaxGpu, Frame.GpuMs);
			MaxAction = FMath::Max(MaxAction, Frame.ActionMs);
			Created += Frame.TargetsCreated;
			CreatedMB += Frame.CreatedMB;
			Released += Frame.TargetsReleased;
			ReleasedMB += Frame.ReleasedMB;
			Requests += Frame.ContactMapRequests;
		}
		const float MedFrame = Median(FrameMs), MedGame = Median(GameMs), MedRender = Median(RenderMs), MedRhi = Median(RhiMs), MedGpu = Median(GpuMs);
		Report += FString::Printf(
			TEXT("\n  %-13s %5d fr | frame %5.1f / %6.1f (at %4.1f s) | game %5.1f / %6.1f | render %5.1f / %6.1f | rhi %5.1f / %6.1f | gpu %5.1f / %6.1f")
			TEXT(" | action max %6.2f | targets +%d (%.1f MB) -%d (%.1f MB) | contact map loads %d"),
			GPhases[Phase].Name, FrameMs.Num(), MedFrame, MaxFrame, MaxFrameAt, MedGame, MaxGame, MedRender, MaxRender, MedRhi, MaxRhi, MedGpu, MaxGpu,
			MaxAction, Created, CreatedMB, Released, ReleasedMB, Requests);
	}
	UE_LOG(LogSoftbodyHitch, Display, TEXT("%s"), *Report);

	// 프레임마다 CSV.
	FString Csv = TEXT("frame,time,phase,frame_ms,game_ms,render_ms,rhi_ms,gpu_ms,action_ms,targets_created,created_mb,targets_released,released_mb,contact_map_loads\n");
	const double Origin = Frames.Num() > 0 ? Frames[0].Time : 0.0;
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		const FFrame& Frame = Frames[Index];
		Csv += FString::Printf(TEXT("%d,%.4f,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%d,%.3f,%d\n"), Index, Frame.Time - Origin, GPhases[Frame.Phase].Name,
			Frame.FrameMs, Frame.GameMs, Frame.RenderMs, Frame.RhiMs, Frame.GpuMs, Frame.ActionMs, Frame.TargetsCreated, Frame.CreatedMB,
			Frame.TargetsReleased, Frame.ReleasedMB, Frame.ContactMapRequests);
	}
	const FString Path = FPaths::Combine(FPaths::ProfilingDir(), TEXT("SoftbodyHitch"),
		FString::Printf(TEXT("hitch_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
	if (FFileHelper::SaveStringToFile(Csv, *Path))
	{
		UE_LOG(LogSoftbodyHitch, Display, TEXT("Per-frame values: %s"), *FPaths::ConvertRelativePathToFull(Path));
	}
	Frames.Reset();

	if (CVarHitchTestQuit.GetValueOnGameThread() != 0 && !GIsEditor)
	{
		UE_LOG(LogSoftbodyHitch, Display, TEXT("Hitch test done, closing the game (softbody.HitchTest.Quit)."));
		FPlatformMisc::RequestExit(false, TEXT("SoftbodyHitchTest"));
	}
}
