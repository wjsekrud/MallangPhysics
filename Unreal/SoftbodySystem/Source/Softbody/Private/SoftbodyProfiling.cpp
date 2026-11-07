// Copyright (c) 2026 wjsekrud

#include "SoftbodyProfiling.h"

#include "Camera/CameraActor.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "SoftbodyComponent.h"
#include "SoftbodySurfaceField.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyProfile, Log, All);

const TCHAR* USoftbodyScenarioSubsystem::TagPrefix = TEXT("SoftbodyScenario.");

namespace
{
	/** 끊김 시험이 읽는 누계(SoftbodyProfiling::GetResourceCounters). */
	SoftbodyProfiling::FResourceCounters GResourceCounters;
	/** GPU 상태 읽기를 이 프레임까지 켠다(Softbody.Stats·Softbody.Profile). 게임 스레드만 만진다. */
	uint64 GForceStatsUntil = 0;
	/** 명령 뒤 GPU 상태 읽기를 켜 두는 프레임 수와, 표를 쓰기까지 기다리는 프레임 수(읽기 고리가 몇 프레임 늦다). */
	constexpr uint64 ForceStatsFrames = 30;
	constexpr uint64 DumpDelayFrames = 10;

	TAutoConsoleVariable<FString> CVarScenarioStart(
		TEXT("softbody.Scenario.Start"), TEXT(""),
		TEXT("Profiling: the scenario to switch to when play begins (a name the Softbody.Scenario command accepts, such as 4_3, All or None). ")
		TEXT("Empty (default) leaves every actor on."),
		ECVF_Default);

	bool IsAll(const FString& Name)
	{
		return Name.IsEmpty() || Name.Equals(TEXT("All"), ESearchCase::IgnoreCase);
	}

	bool IsNone(const FString& Name)
	{
		return Name.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	}

	/** 액터가 단 장면 이름들(태그 "SoftbodyScenario.<이름>"). */
	void ScenarioNames(const AActor* Actor, TArray<FString>& Out)
	{
		const int32 PrefixLength = FCString::Strlen(USoftbodyScenarioSubsystem::TagPrefix);
		for (const FName& Tag : Actor->Tags)
		{
			const FString Text = Tag.ToString();
			if (Text.StartsWith(USoftbodyScenarioSubsystem::TagPrefix))
			{
				Out.Add(Text.Mid(PrefixLength));
			}
		}
	}

	bool HasScenarioTag(const AActor* Actor)
	{
		for (const FName& Tag : Actor->Tags)
		{
			if (Tag.ToString().StartsWith(USoftbodyScenarioSubsystem::TagPrefix))
			{
				return true;
			}
		}
		return false;
	}

	bool InScenario(const AActor* Actor, const FString& Name)
	{
		TArray<FString> Names;
		ScenarioNames(Actor, Names);
		return Names.ContainsByPredicate([&Name](const FString& Each) { return Each.Equals(Name, ESearchCase::IgnoreCase); });
	}

	FString ActorName(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** 표면 장마다 GPU 상태 한 줄(Softbody.Stats). 걸음이 긴 장부터. */
	void DumpStats(UWorld* World, const TCHAR* Why)
	{
		struct FRow
		{
			FString Name;
			uint32 Resolution = 0;
			uint32 DragResolution = 0;
			bool bStats = false;
			USoftbodySurfaceField::FGpuState State;
		};
		TArray<FRow> Rows;
		for (TObjectIterator<USoftbodyComponent> It; It; ++It)
		{
			USoftbodyComponent* Component = *It;
			if (!IsValid(Component) || Component->GetWorld() != World)
			{
				continue;
			}
			const USoftbodySurfaceField* Field = Component->GetSurfaceField();
			if (!Field)
			{
				continue;
			}
			FRow& Row = Rows.AddDefaulted_GetRef();
			Row.Name = Field->GetDebugName();
			if (Row.Name.IsEmpty() && Component->GetOwner())
			{
				Row.Name = ActorName(Component->GetOwner());
			}
			Row.Resolution = Field->GetLastResolution();
			Row.DragResolution = Field->GetLastDragResolution();
			Row.bStats = Field->GetGpuState(Row.State);
		}
		Rows.Sort([](const FRow& A, const FRow& B)
		{
			if (A.bStats != B.bStats)
			{
				return A.bStats;
			}
			const uint32 StepsA = FMath::Max(A.State.DragGather.MaxSteps, A.State.DragCorrect.MaxSteps);
			const uint32 StepsB = FMath::Max(B.State.DragGather.MaxSteps, B.State.DragCorrect.MaxSteps);
			return StepsA != StepsB ? StepsA > StepsB : A.Name < B.Name;
		});

		const USoftbodyScenarioSubsystem* Scenarios = World->GetSubsystem<USoftbodyScenarioSubsystem>();
		auto IntCVar = [](const TCHAR* Name) -> int32
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			return Variable ? Variable->GetInt() : -1;
		};
		double Ms[(int32)USoftbodySurfaceField::EGpuTime::Num];
		USoftbodySurfaceField::GetGpuTimes(Ms);
		using EGpuTime = USoftbodySurfaceField::EGpuTime;
		UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] %s, frame %llu, scenario '%s'%s, softbody.Drag.CorrectPasses %d, ")
			TEXT("softbody.Drag.Slices %d, softbody.Surface.UpdateBudget %d"), Why, (unsigned long long)GFrameCounter,
			Scenarios && !Scenarios->GetActive().IsEmpty() ? *Scenarios->GetActive() : TEXT("All"),
			Scenarios && Scenarios->IsPaused() ? TEXT(" (paused)") : TEXT(""),
			IntCVar(TEXT("softbody.Drag.CorrectPasses")), IntCVar(TEXT("softbody.Drag.Slices")), IntCVar(TEXT("softbody.Surface.UpdateBudget")));
		UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] GPU ms (all fields, latest frame): Contact %.3f Tiles %.3f Dent %.3f Drag %.3f Pressure %.3f ")
			TEXT("Compose %.3f Lump %.3f"), Ms[(int32)EGpuTime::Contact], Ms[(int32)EGpuTime::Tiles], Ms[(int32)EGpuTime::Dent], Ms[(int32)EGpuTime::Drag],
			Ms[(int32)EGpuTime::Pressure], Ms[(int32)EGpuTime::Compose], Ms[(int32)EGpuTime::Lump]);
		// 걸음은 8×8 묶음 전체(모든 조각)의 값이다 — 조각 하나는 그 1/softbody.Drag.Slices 안팎을 걷는다(일지 191).
		UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] field | res/drag | age | pressed rest | contact dent | sources reach clusters mixed | ")
			TEXT("correct: groups maxSteps sumSteps nearTiles maxNear maxFar | gather: groups maxSteps sumSteps nearTiles maxNear maxFar ")
			TEXT("(steps per 8x8 group; each slice walks about steps / slices)"));
		for (const FRow& Row : Rows)
		{
			if (!Row.bStats)
			{
				UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] %-36s | %4u/%3u | no GPU state yet"), *Row.Name, Row.Resolution, Row.DragResolution);
				continue;
			}
			const USoftbodySurfaceField::FGpuState& S = Row.State;
			UE_LOG(LogSoftbodyProfile, Display,
				TEXT("[Softbody.Stats] %-36s | %4u/%3u | %4u | %u %u | %5u %4u | %5u %4u %4u %3u | %4u %5u %8u %3u %5u %4u | %4u %5u %8u %3u %5u %4u"),
				*Row.Name, Row.Resolution, Row.DragResolution, S.AgeFrames, S.bPressed, S.bResting, S.ContactGroups, S.DentTiles, S.DragSources,
				S.DragGroups, S.DragClusters, S.DragMixedClusters,
				S.DragCorrect.Groups, S.DragCorrect.MaxSteps, S.DragCorrect.SumSteps, S.DragCorrect.MaxNearTiles, S.DragCorrect.MaxNear, S.DragCorrect.MaxFar,
				S.DragGather.Groups, S.DragGather.MaxSteps, S.DragGather.SumSteps, S.DragGather.MaxNearTiles, S.DragGather.MaxNear, S.DragGather.MaxFar);
		}
		// 함몰 목록 까닭별 수: 위 dent를 겹치지 않는 몫으로 나눈다. widened = 이음새·늘 켬 묶음을 넓힌 둘레, pieces = 하드 서피스 조각이 더한 묶음.
		// 줄머리가 'dent-why'라 위 표를 읽는 스크립트의 줄 정규식에 걸리지 않는다.
		UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] dent tiles by reason: field | dent = pressed ring linked always-on widened pieces | ")
			TEXT("Separated Parts (tiles solved with the part channels / dent, off = not separated)"));
		for (const FRow& Row : Rows)
		{
			if (!Row.bStats)
			{
				continue;
			}
			const USoftbodySurfaceField::FGpuState& S = Row.State;
			const uint32 Named = S.DentPressedTiles + S.DentRingTiles + S.DentLinkedTiles + S.DentAlwaysTiles;
			const uint32 Widened = S.DentCoreTiles > Named ? S.DentCoreTiles - Named : 0u;
			const uint32 Pieces = S.DentTiles > S.DentCoreTiles ? S.DentTiles - S.DentCoreTiles : 0u;
			const FString Parts = S.DentTiles == 0 ? FString(TEXT("-"))
				: (S.bDentParts ? FString::Printf(TEXT("%u / %u"), S.DentPartsTiles, S.DentTiles) : FString(TEXT("off")));
			UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] dent-why %-36s | %5u = %4u %4u %5u %4u %5u %4u | %s"), *Row.Name, S.DentTiles,
				S.DentPressedTiles, S.DentRingTiles, S.DentLinkedTiles, S.DentAlwaysTiles, Widened, Pieces, *Parts);
		}
		// 관통 계약 밖의 누름(원칙 P06, 일지 295): 살 두께보다 깊이 눌린 칸이 있는 장만. 줄머리가 'too-deep'이라 위 표의 줄 정규식에 걸리지 않는다.
		for (const FRow& Row : Rows)
		{
			if (Row.bStats && Row.State.DeepCells > 0)
			{
				UE_LOG(LogSoftbodyProfile, Warning, TEXT("[Softbody.Stats] too-deep %-36s | %u surface cells pressed deeper than the flesh, up to %.2f cm past it. ")
					TEXT("There the skin is pushed past the bone, down to Max Contact Depth: stop the presser at the flesh (Flesh Thickness view), lower Max Contact Depth, or paint Depth brighter there."),
					*Row.Name, Row.State.DeepCells, Row.State.DeepestBeyond);
			}
		}
		// 들어온 면 표지(일지 313): 한 칸의 기둥에 콜라이더가 셋 이상 든 칸(개발 통계 — 넘친 콜라이더는 그 갱신에 표지 없이 지금 규칙으로 잰다).
		for (const FRow& Row : Rows)
		{
			if (Row.bStats && Row.State.LabelOverflowCells > 0)
			{
				UE_LOG(LogSoftbodyProfile, Display, TEXT("[Softbody.Stats] label-overflow %-36s | %u surface cells have more than two colliders in their column."),
					*Row.Name, Row.State.LabelOverflowCells);
			}
		}
	}

	void DumpLater(UWorld* World, const FString& Why)
	{
		GForceStatsUntil = FMath::Max<uint64>(GForceStatsUntil, GFrameCounter + ForceStatsFrames);
		const uint64 Due = GFrameCounter + DumpDelayFrames;
		TWeakObjectPtr<UWorld> WeakWorld = World;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld, Due, Why](float) -> bool
		{
			if (GFrameCounter < Due)
			{
				return true;
			}
			if (UWorld* Alive = WeakWorld.Get())
			{
				DumpStats(Alive, *Why);
			}
			return false;
		}));
	}

	void StatsCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (!World)
		{
			return;
		}
		DumpLater(World, TEXT("Softbody.Stats"));
		Ar.Logf(TEXT("[Softbody.Stats] Reading the GPU state of every Softbody contact field; the table goes to the log in %d frames (LogSoftbodyProfile)."),
			(int32)DumpDelayFrames);
	}

	void ProfileCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (!World || !GEngine)
		{
			return;
		}
		GEngine->Exec(World, TEXT("ProfileGPU"), *GLog);
		DumpLater(World, TEXT("Softbody.Profile"));
		Ar.Logf(TEXT("[Softbody.Profile] ProfileGPU for this frame (each field's passes sit under 'Softbody <actor name>') and the Softbody.Stats ")
			TEXT("table in %d frames, both in the log."), (int32)DumpDelayFrames);
	}

	void ScenarioCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		USoftbodyScenarioSubsystem* Scenarios = World ? World->GetSubsystem<USoftbodyScenarioSubsystem>() : nullptr;
		if (!Scenarios)
		{
			Ar.Logf(TEXT("[Softbody.Scenario] Only works while playing (PIE or game)."));
			return;
		}
		FString Report;
		if (Args.Num() == 0)
		{
			Scenarios->Describe(Report);
		}
		else
		{
			Scenarios->Apply(Args[0], Report);
		}
		Ar.Logf(TEXT("%s"), *Report);
		UE_LOG(LogSoftbodyProfile, Display, TEXT("%s"), *Report);
	}

	void PauseCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		USoftbodyScenarioSubsystem* Scenarios = World ? World->GetSubsystem<USoftbodyScenarioSubsystem>() : nullptr;
		if (!Scenarios)
		{
			Ar.Logf(TEXT("[Softbody.Scenario.Pause] Only works while playing (PIE or game)."));
			return;
		}
		const bool bPause = Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : !Scenarios->IsPaused();
		FString Report;
		Scenarios->SetPaused(bPause, Report);
		Ar.Logf(TEXT("%s"), *Report);
		UE_LOG(LogSoftbodyProfile, Display, TEXT("%s"), *Report);
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyStatsCommand(
		TEXT("Softbody.Stats"),
		TEXT("Profiling: writes one line per Softbody contact field to the log (LogSoftbodyProfile) about 10 frames later: pressed or resting, contact ")
		TEXT("and dent groups, drag sources, and for the two drag passes the number of 8x8 groups that ran and the longest walk of one group. ")
		TEXT("Works without 'stat softbody'."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&StatsCommand));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyProfileCommand(
		TEXT("Softbody.Profile"),
		TEXT("Profiling: runs ProfileGPU for this frame (each contact field's passes are grouped under 'Softbody <actor name>') and writes the ")
		TEXT("Softbody.Stats table a few frames later. Both go to the log."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&ProfileCommand));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyScenarioCommand(
		TEXT("Softbody.Scenario"),
		TEXT("Profiling: with no argument, lists the scenarios in this map (actors tagged SoftbodyScenario.<name>). With a name, keeps only that ")
		TEXT("scenario's actors on; the others are hidden with collision, ticking and physics off, so their contact fields cost nothing. Views ")
		TEXT("through the scenario's camera if it has one. 'All' turns everything back on, 'None' turns every tagged actor off."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&ScenarioCommand));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyScenarioPauseCommand(
		TEXT("Softbody.Scenario.Pause"),
		TEXT("Profiling: 1 stops the motion of the scenario actors that are on (animation, movers, actor ticks) while their Softbody components keep ")
		TEXT("running, to measure a held press. 0 resumes. No argument toggles."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&PauseCommand));

	// 개발(일지 240): 창 없는 시험이 쓰는 누르는 손가락 — SA 창 Simulation 탭과 같은 질의 전용 캡슐(모든 채널을 막는다).
	TWeakObjectPtr<AActor> GProbeCapsule;

	void ProbeCapsuleCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		if (!World)
		{
			return;
		}
		if (Args.Num() < 8)
		{
			if (AActor* Actor = GProbeCapsule.Get())
			{
				Actor->Destroy();
			}
			GProbeCapsule.Reset();
			return;
		}
		double V[8];
		for (int32 i = 0; i < 8; ++i)
		{
			V[i] = FCString::Atod(*Args[i]);
		}
		AActor* Actor = GProbeCapsule.Get();
		if (!Actor || Actor->GetWorld() != World)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
			UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Actor, TEXT("ProbeCapsule"));
			Capsule->SetMobility(EComponentMobility::Movable);
			Capsule->SetCollisionObjectType(ECC_WorldDynamic);
			Capsule->SetCollisionResponseToAllChannels(ECR_Block);
			Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Capsule->SetGenerateOverlapEvents(false);
			Actor->SetRootComponent(Capsule);
			Capsule->RegisterComponent();
			GProbeCapsule = Actor;
		}
		UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Actor->GetRootComponent());
		const float Radius = FMath::Max((float)V[6], 0.05f);
		Capsule->SetCapsuleSize(Radius, FMath::Max((float)V[7], Radius));
		const FVector Axis = FVector(V[3], V[4], V[5]).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		Actor->SetActorLocationAndRotation(FVector(V[0], V[1], V[2]), FRotationMatrix::MakeFromZ(Axis).ToQuat());
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyProbeCapsuleCommand(
		TEXT("softbody.Debug.ProbeCapsule"),
		TEXT("Development: places a test capsule that presses Softbody meshes like the Simulation tab's finger. Arguments: center x y z, axis x y z, ")
		TEXT("radius, half height (cm, world). No arguments removes it."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&ProbeCapsuleCommand));
}

bool SoftbodyProfiling::IsForcingStats()
{
	return GFrameCounter <= GForceStatsUntil;
}

SoftbodyProfiling::FResourceCounters& SoftbodyProfiling::GetResourceCounters()
{
	return GResourceCounters;
}

bool USoftbodyScenarioSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// 프로파일링 도구라 출시용(Shipping) 빌드의 월드에는 만들지 않는다.
	return !UE_BUILD_SHIPPING && (WorldType == EWorldType::Game || WorldType == EWorldType::PIE);
}

void USoftbodyScenarioSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const FString Start = CVarScenarioStart.GetValueOnGameThread();
	if (!Start.IsEmpty())
	{
		FString Report;
		Apply(Start, Report);
		UE_LOG(LogSoftbodyProfile, Display, TEXT("%s"), *Report);
	}
}

bool USoftbodyScenarioSubsystem::Apply(const FString& Name, FString& OutReport)
{
	UWorld* World = GetWorld();
	const bool bAll = IsAll(Name);
	const bool bNone = IsNone(Name);
	if (!bAll && !bNone)
	{
		bool bFound = false;
		for (TActorIterator<AActor> It(World); It && !bFound; ++It)
		{
			bFound = InScenario(*It, Name);
		}
		if (!bFound)
		{
			FString List;
			Describe(List);
			OutReport = FString::Printf(TEXT("[Softbody.Scenario] No scenario named '%s'.\n%s"), *Name, *List);
			return false;
		}
	}
	const bool bWasPaused = bPaused;
	if (bWasPaused)
	{
		ResumeMotion();
	}
	int32 On = 0;
	int32 Off = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!HasScenarioTag(Actor))
		{
			continue;
		}
		const bool bOn = bAll || (!bNone && InScenario(Actor, Name));
		const TWeakObjectPtr<AActor> Key = Actor;
		if (bOn)
		{
			if (FFrozen* Saved = Frozen.Find(Key))
			{
				Thaw(Actor, *Saved);
				Frozen.Remove(Key);
			}
			++On;
		}
		else
		{
			if (!Frozen.Contains(Key))
			{
				Freeze(Actor);
			}
			++Off;
		}
	}
	Active = bAll ? FString() : Name;
	ViewScenario(Active);
	if (bWasPaused)
	{
		PauseMotion();
	}
	OutReport = FString::Printf(TEXT("[Softbody.Scenario] '%s': %d tagged actors on, %d off%s."), bAll ? TEXT("All") : *Name, On, Off,
		bPaused ? TEXT(" (motion paused)") : TEXT(""));
	return true;
}

void USoftbodyScenarioSubsystem::Freeze(AActor* Actor)
{
	FFrozen Saved;
	Saved.bHidden = Actor->IsHidden();
	Saved.bCollision = Actor->GetActorEnableCollision();
	Saved.bActorTick = Actor->IsActorTickEnabled();
	Actor->SetActorHiddenInGame(true);
	Actor->SetActorEnableCollision(false);
	Actor->SetActorTickEnabled(false);
	for (UActorComponent* Component : Actor->GetComponents())
	{
		if (!Component)
		{
			continue;
		}
		if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component); Primitive && Primitive->IsSimulatingPhysics())
		{
			Primitive->SetSimulatePhysics(false);
			Saved.Simulating.Add(Primitive);
		}
		if (Component->IsComponentTickEnabled())
		{
			Component->SetComponentTickEnabled(false);
			Saved.Ticking.Add(Component);
		}
	}
	Frozen.Add(Actor, MoveTemp(Saved));
}

void USoftbodyScenarioSubsystem::Thaw(AActor* Actor, FFrozen& Saved)
{
	Actor->SetActorHiddenInGame(Saved.bHidden);
	Actor->SetActorEnableCollision(Saved.bCollision);
	Actor->SetActorTickEnabled(Saved.bActorTick);
	for (const TWeakObjectPtr<UActorComponent>& Component : Saved.Ticking)
	{
		if (Component.IsValid())
		{
			Component->SetComponentTickEnabled(true);
		}
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& Primitive : Saved.Simulating)
	{
		if (Primitive.IsValid())
		{
			Primitive->SetSimulatePhysics(true);
		}
	}
}

void USoftbodyScenarioSubsystem::SetPaused(bool bPause, FString& OutReport)
{
	if (bPause && !bPaused)
	{
		PauseMotion();
	}
	else if (!bPause && bPaused)
	{
		ResumeMotion();
	}
	OutReport = FString::Printf(TEXT("[Softbody.Scenario.Pause] %s: %d actors, %d components stopped (Softbody components keep running)."),
		bPaused ? TEXT("paused") : TEXT("running"), PausedActors.Num(), PausedComponents.Num());
}

void USoftbodyScenarioSubsystem::PauseMotion()
{
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (!HasScenarioTag(Actor) || Frozen.Contains(TWeakObjectPtr<AActor>(Actor)))
		{
			continue;
		}
		if (Actor->IsActorTickEnabled())
		{
			Actor->SetActorTickEnabled(false);
			PausedActors.Add(Actor);
		}
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (Component && !Component->IsA<USoftbodyComponent>() && Component->IsComponentTickEnabled())
			{
				Component->SetComponentTickEnabled(false);
				PausedComponents.Add(Component);
			}
		}
	}
	bPaused = true;
}

void USoftbodyScenarioSubsystem::ResumeMotion()
{
	for (const TWeakObjectPtr<AActor>& Actor : PausedActors)
	{
		if (Actor.IsValid())
		{
			Actor->SetActorTickEnabled(true);
		}
	}
	for (const TWeakObjectPtr<UActorComponent>& Component : PausedComponents)
	{
		if (Component.IsValid())
		{
			Component->SetComponentTickEnabled(true);
		}
	}
	PausedActors.Reset();
	PausedComponents.Reset();
	bPaused = false;
}

void USoftbodyScenarioSubsystem::ViewScenario(const FString& Name)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Controller)
	{
		return;
	}
	ACameraActor* Camera = nullptr;
	if (!Name.IsEmpty() && !IsNone(Name))
	{
		for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
		{
			if (InScenario(*It, Name))
			{
				Camera = *It;
				break;
			}
		}
	}
	if (Camera)
	{
		Controller->SetViewTargetWithBlend(Camera, 0.f);
	}
	else if (const AActor* Viewing = Controller->GetViewTarget(); Viewing && Viewing->IsA<ACameraActor>() && HasScenarioTag(Viewing))
	{
		// 장면 카메라로 보던 중이면 플레이어 시점으로 돌아간다.
		AActor* Back = Controller->GetPawn() ? static_cast<AActor*>(Controller->GetPawn()) : static_cast<AActor*>(Controller);
		Controller->SetViewTargetWithBlend(Back, 0.f);
	}
}

void USoftbodyScenarioSubsystem::Describe(FString& OutReport) const
{
	struct FInfo
	{
		int32 Actors = 0;
		int32 Softbodies = 0;
		FString Camera;
	};
	TMap<FString, FInfo> Infos;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TArray<FString> Names;
		ScenarioNames(*It, Names);
		for (const FString& Name : Names)
		{
			FInfo& Info = Infos.FindOrAdd(Name);
			++Info.Actors;
			Info.Softbodies += It->FindComponentByClass<USoftbodyComponent>() ? 1 : 0;
			if (It->IsA<ACameraActor>() && Info.Camera.IsEmpty())
			{
				Info.Camera = ActorName(*It);
			}
		}
	}
	Infos.KeySort([](const FString& A, const FString& B) { return A < B; });
	OutReport = FString::Printf(TEXT("[Softbody.Scenario] %d scenarios in this map, active '%s'%s:"), Infos.Num(),
		Active.IsEmpty() ? TEXT("All") : *Active, bPaused ? TEXT(" (motion paused)") : TEXT(""));
	for (const TPair<FString, FInfo>& Pair : Infos)
	{
		OutReport += FString::Printf(TEXT("\n  %-14s %3d actors, %2d softbody%s%s"), *Pair.Key, Pair.Value.Actors, Pair.Value.Softbodies,
			Pair.Value.Camera.IsEmpty() ? TEXT("") : TEXT(", camera "), *Pair.Value.Camera);
	}
}
