// Copyright (c) 2026 wjsekrud

#include "SoftbodySurfaceBatch.h"

#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Softbody.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyProfiling.h"
#include "SoftbodySettings.h"

DECLARE_FLOAT_COUNTER_STAT(TEXT("Surface Field Pool Idle (MB)"), STAT_SoftbodyFieldPoolMemory, STATGROUP_Softbody);

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodySurfaceBatch, Log, All);

// 갱신률 LOD(ADR-0021)의 품질 설정. 재료값이 아니라 프로젝트의 품질 설정이라 컴포넌트가 아니라 콘솔 변수다(스케일러빌리티 ini에서
// 단계별로 둘 수 있다). 상한 0·화면 크기 0이면 모든 표면 장이 매 프레임 돈다.
// 기본 4: 시연 레벨 26개에서 약 2 ms, 화면 차이 없음. 미룬 시간은 다음 갱신이 한 걸음에 푼다.
static TAutoConsoleVariable<int32> CVarSoftbodySurfaceUpdateBudget(
	TEXT("softbody.Surface.UpdateBudget"), 4,
	TEXT("Most Softbody objects whose contact is updated on the GPU in one frame, per world. 0 = no limit. Default 4: up to four touched ")
	TEXT("objects update every frame; beyond that, updates take turns across frames (lower GPU cost, usually no visible difference). ")
	TEXT("Priority: objects just being touched, then visible objects with a collider in reach or that waited softbody.Surface.MaxInterval frames, ")
	TEXT("then visible objects recovering after release, then off-screen objects (larger on screen and longer waiting first within each). ")
	TEXT("A skipped object catches up in one step at its next update."),
	ECVF_Scalability);

static TAutoConsoleVariable<float> CVarSoftbodySurfaceFullRateScreenSize(
	TEXT("softbody.Surface.FullRateScreenSize"), 0.f,
	TEXT("Objects that look smaller than this screen size (bounding sphere diameter relative to screen height, about 0 to 1) update their ")
	TEXT("contact only once every ceil(this value / screen size) frames, up to softbody.Surface.MaxInterval. Saves GPU time on distant objects. ")
	TEXT("Default 0 = every object updates every frame. The first moment of contact always updates that frame, whatever the size."),
	ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarSoftbodySurfaceMaxInterval(
	TEXT("softbody.Surface.MaxInterval"), 4,
	TEXT("Longest gap, in frames, between contact updates when updates are skipped for small or distant objects. Default 4: even the ")
	TEXT("smallest object updates at 15 Hz at 60 fps. Objects held back by softbody.Surface.UpdateBudget move to the front after waiting ")
	TEXT("this long (if more than the budget have waited this long, larger on screen and longer waiting go first)."),
	ECVF_Scalability);

// 갱신 주기 상한(일지 156): 켜고 끄기와 값은 프로젝트 설정(USoftbodySettings)이다. 콘솔 변수는 패키지한 빌드에서 견줘 보려고 둔다.
static TAutoConsoleVariable<float> CVarSoftbodySurfaceMaxUpdateRate(
	TEXT("softbody.Surface.MaxUpdateRate"), -1.f,
	TEXT("Most times a second a touched object recomputes its dents, whatever the frame rate. -1 (default) = use Project Settings > ")
	TEXT("Plugins > Softbody > Limit Contact Update Rate. 0 = no limit. For example 30 at 120 fps updates each object every fourth ")
	TEXT("frame. The first moment of contact always updates at once."),
	ECVF_Scalability);

// 텍스처 풀(ADR-0022). 품질·메모리 설정이라 콘솔 변수다.
static TAutoConsoleVariable<int32> CVarSoftbodySurfaceTexturePool(
	TEXT("softbody.Surface.TexturePool"), 2,
	TEXT("Spare sets of contact textures kept per world for each contact map kind (resolution, with or without lumps, and the coarse grid size). An ")
	TEXT("object takes a set when first touched and returns it, cleared, once it has fully recovered with no collider nearby, so memory grows with how ")
	TEXT("many objects are touched, not how many are placed. This many spare sets are prepared ahead (spread over frames, softbody.Surface.CreateBudget) ")
	TEXT("while an object of that kind is on, so the first touch does not wait. Sets beyond this number are freed after softbody.Surface.ReleaseDelay. ")
	TEXT("Default 2. One set is about 70 MB for a 1024x1024 character with lumps and about 4 MB for a 256x256 prop."),
	ECVF_Scalability);

// 끊김 없는 켜고 끄기(일지 235): 렌더 타깃 만들기·놓기와 접촉 지도 올리기를 프레임마다 나눈다. 품질·메모리 설정이라 콘솔 변수다.
static TAutoConsoleVariable<float> CVarSoftbodySurfaceCreateBudget(
	TEXT("softbody.Surface.CreateBudget"), 4.f,
	TEXT("Most GPU memory (MB) of contact textures created, and freed, per frame in a world. Creating textures costs render thread and GPU time, ")
	TEXT("so switching many Softbody objects on, touching many at once or freeing many is spread over frames instead of hitching: contact starts a ")
	TEXT("few frames later on objects still waiting for their textures (inertia runs at once). At least one texture per frame. 0 = no limit. Default 4."),
	ECVF_Scalability);

static TAutoConsoleVariable<float> CVarSoftbodySurfaceReleaseDelay(
	TEXT("softbody.Surface.ReleaseDelay"), 10.f,
	TEXT("Seconds that contact textures and contact maps stay in memory after the last Softbody object using them is switched off, finishes ")
	TEXT("recovering or is removed. Switching it on again within this time costs nothing; after it, the memory is freed a little per frame ")
	TEXT("(softbody.Surface.CreateBudget). GPU work stops at once either way. 0 = start freeing at once. Default 10."),
	ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarSoftbodySurfaceFirstUpdates(
	TEXT("softbody.Surface.FirstUpdates"), 2,
	TEXT("Most Softbody objects per frame, in a world, that run their first contact update after being switched on, spawned or streamed in ")
	TEXT("(that update builds the object's lookup tables on the render thread). Others start a frame or two later, so touching many new objects ")
	TEXT("at once does not hitch. Objects switched back on within softbody.Surface.ReleaseDelay keep their tables and do not count. 0 = no limit. Default 2."),
	ECVF_Scalability);

static TAutoConsoleVariable<float> CVarSoftbodySurfaceLoadBudget(
	TEXT("softbody.Surface.LoadBudget"), 2.f,
	TEXT("Most contact map data (MB) loading at once in a world, each texture counting as at least 0.5 MB. The engine creates each contact map ")
	TEXT("texture on the render thread in one piece when it finishes loading, so loading a few at a time keeps a frame from creating many at ")
	TEXT("once (switching many objects on after their contact maps were unloaded). At least one texture loads at a time. 0 = no limit. Default 2."),
	ECVF_Scalability);

namespace
{
	/** 접촉 지도 텍스처 한 장의 바이트 어림(굽기 형식: 위치·이음 RGBA32F, 노멀·스킨·덩어리 RGBA16F, 부피·겉껍질·제 살 깊이 G8, 거리표 G16). */
	int64 EstimateContactMapBytes(const USoftbodyMeshData* Data, const FSoftObjectPath& Path)
	{
		const int64 Texels = (int64)Data->CarrierResolution * (int64)Data->CarrierResolution;
		if (Path == Data->CarrierPositionTex.ToSoftObjectPath() || Path == Data->CarrierRedirectTex.ToSoftObjectPath())
		{
			return Texels * 16;
		}
		if (Path == Data->CarrierVolumeTex.ToSoftObjectPath()
			|| Path == Data->CarrierColumnTex.ToSoftObjectPath())
		{
			return Texels;
		}
		if (Path == Data->CarrierBulgeDistanceTex.ToSoftObjectPath())
		{
			return (int64)FMath::Max(Data->BulgeCellCount, 1)
				* (int64)SoftbodyBulgeTable::RowTexels(FMath::Max(Data->BulgeLandmarkCount, 1), Data->BulgeTableVersion) * 2;
		}
		return Texels * 8;
	}
}

void USoftbodySurfaceBatch::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &USoftbodySurfaceBatch::OnPostActorTick);
	// 접촉 패스의 파이프라인을 처음 쓰기 전에 백그라운드에서 만들어 둔다(일지 235 — 처음 닿는 프레임에 만들면 몇 초 멈춘다).
	USoftbodySurfaceField::PrecachePipelines();
}

namespace
{
	/** FPreviewWorldScope 깊이(게임 스레드). */
	int32 GSoftbodyPreviewWorldScopes = 0;
}

USoftbodySurfaceBatch::FPreviewWorldScope::FPreviewWorldScope()
{
	check(IsInGameThread());
	++GSoftbodyPreviewWorldScopes;
}

USoftbodySurfaceBatch::FPreviewWorldScope::~FPreviewWorldScope()
{
	--GSoftbodyPreviewWorldScopes;
}

bool USoftbodySurfaceBatch::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return Super::DoesSupportWorldType(WorldType) || (WorldType == EWorldType::EditorPreview && GSoftbodyPreviewWorldScopes > 0);
}

void USoftbodySurfaceBatch::Deinitialize()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
	Pending.Reset();
	// 쉬는·만드는 중인·놓을 벌은 월드와 함께 GC가 거둔다(월드를 내리는 것은 끊김을 따지지 않는 때다).
	Idle.Reset();
	IdleCoarse.Reset();
	Building.Reset();
	BuildingCoarse.Reset();
	Retiring.Reset();
	FineKinds.Reset();
	CoarseKinds.Reset();
	for (FContactMapRequest& Request : ContactMapRequests)
	{
		if (Request.Handle.IsValid())
		{
			Request.Handle->CancelHandle();
		}
	}
	ContactMapRequests.Reset();
	ContactMaps.Reset();
	Sleepers.Reset();
	Super::Deinitialize();
}

void USoftbodySurfaceBatch::Enqueue(USoftbodySurfaceField* Field, FSoftbodySurfaceFieldRender&& Render, const FSoftbodySurfaceSchedule& Schedule)
{
	// 지난 프레임에 올리지 못한 신청(틱 뒤 알림이 없던 프레임)은 그 렌더 타깃이 이미 사라졌을 수 있어 버린다.
	if (PendingFrame != GFrameCounter)
	{
		Pending.Reset();
		PendingFrame = GFrameCounter;
	}
	FPending& Entry = Pending.AddDefaulted_GetRef();
	Entry.Field = Field;
	Entry.Render = MoveTemp(Render);
	Entry.Schedule = Schedule;
}

void USoftbodySurfaceBatch::Cancel(const USoftbodySurfaceField* Field)
{
	Pending.RemoveAll([Field](const FPending& Entry) { return Entry.Field.Get() == Field; });
}

int32 USoftbodySurfaceBatch::UpdateIntervalFor(float ScreenSize)
{
	const float FullRate = CVarSoftbodySurfaceFullRateScreenSize.GetValueOnGameThread();
	if (FullRate <= 0.f || ScreenSize >= FullRate)
	{
		return 1;
	}
	const int32 MaxInterval = FMath::Max(CVarSoftbodySurfaceMaxInterval.GetValueOnGameThread(), 1);
	return FMath::Clamp(FMath::CeilToInt(FullRate / FMath::Max(ScreenSize, 1e-3f)), 1, MaxInterval);
}

float USoftbodySurfaceBatch::MaxUpdateRate()
{
	const float Override = CVarSoftbodySurfaceMaxUpdateRate.GetValueOnGameThread();
	if (Override >= 0.f)
	{
		return Override;
	}
	const USoftbodySettings* Settings = GetDefault<USoftbodySettings>();
	return Settings->bLimitContactUpdateRate ? FMath::Max(Settings->MaxContactUpdateRate, 0.f) : 0.f;
}

bool USoftbodySurfaceBatch::TryAcquire(int32 Resolution, bool bLump, int32 CoarseResolution, FSoftbodySurfaceTextures& OutFine,
	FSoftbodySurfaceCoarseTextures& OutCoarse)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyPoolAcquire);
	const int32 FineIndex = Idle.IndexOfByPredicate([Resolution, bLump](const FSoftbodySurfaceTextures& Set)
	{
		return Set.Resolution == Resolution && Set.bLump == bLump;
	});
	const int32 CoarseIndex = IdleCoarse.IndexOfByPredicate([CoarseResolution](const FSoftbodySurfaceCoarseTextures& Set)
	{
		return Set.Resolution == CoarseResolution;
	});
	if (FineIndex == INDEX_NONE || CoarseIndex == INDEX_NONE)
	{
		// 없는 쪽을 원한다고 적어 둔다 — Service가 프레임 예산 안에서 다른 여분보다 먼저 만든다. 있는 쪽은 그대로 둔다(둘 다 있을 때만 준다).
		if (FineIndex == INDEX_NONE)
		{
			++FineKinds.FindOrAdd(KindKey(Resolution, bLump)).Wanted;
		}
		if (CoarseIndex == INDEX_NONE)
		{
			++CoarseKinds.FindOrAdd(CoarseResolution).Wanted;
		}
		return false;
	}
	OutFine = MoveTemp(Idle[FineIndex]);
	Idle.RemoveAtSwap(FineIndex);
	OutCoarse = MoveTemp(IdleCoarse[CoarseIndex]);
	IdleCoarse.RemoveAtSwap(CoarseIndex);
	const double Now = FPlatformTime::Seconds();
	FineKinds.FindOrAdd(KindKey(Resolution, bLump)).LastUsed = Now;
	CoarseKinds.FindOrAdd(CoarseResolution).LastUsed = Now;
	return true;
}

void USoftbodySurfaceBatch::Release(FSoftbodySurfaceTextures&& Set)
{
	if (!Set.IsComplete())
	{
		for (int32 Index = 0; Index < FSoftbodySurfaceTextures::NumTargets(true); ++Index)
		{
			if (UTextureRenderTarget2D* Target = *Set.Slot(Index))
			{
				Retiring.Add(Target);
			}
		}
		return;
	}
	// 0으로 비워 쉬게 둔다 — 비우기는 Service가 이 프레임에 돌려받은 벌을 모아 그래프 하나로(벌마다 그래프를 올리면 많이 끌 때 렌더 스레드가 늘어난다),
	// 놓는 것은 ReleaseDelay 뒤에 프레임 예산만큼. 이 프레임에 다른 장이 잡아도 비우기는 그 장의 갱신보다 먼저 올라간다(Service가 Flush보다 먼저).
	for (int32 Index = 0; Index < FSoftbodySurfaceTextures::NumTargets(Set.bLump); ++Index)
	{
		if (UTextureRenderTarget2D* Target = *Set.Slot(Index))
		{
			PendingClears.Add(Target);
		}
	}
	Set.IdleSince = FPlatformTime::Seconds();
	FineKinds.FindOrAdd(KindKey(Set.Resolution, Set.bLump)).LastUsed = Set.IdleSince;
	Idle.Add(MoveTemp(Set));
}

void USoftbodySurfaceBatch::ReleaseCoarse(FSoftbodySurfaceCoarseTextures&& Set)
{
	if (!Set.IsComplete())
	{
		for (int32 Index = 0; Index < FSoftbodySurfaceCoarseTextures::NumTargets(); ++Index)
		{
			if (UTextureRenderTarget2D* Target = *Set.Slot(Index))
			{
				Retiring.Add(Target);
			}
		}
		return;
	}
	Set.IdleSince = FPlatformTime::Seconds();
	CoarseKinds.FindOrAdd(Set.Resolution).LastUsed = Set.IdleSince;
	IdleCoarse.Add(MoveTemp(Set));
}

void USoftbodySurfaceBatch::Reserve(int32 Resolution, bool bLump, int32 CoarseResolution)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyPoolReserve);
	// 여분은 Service가 프레임 예산 안에서 만든다 — 레벨을 읽거나 스폰하거나 켜는 프레임에 몰아 만들지 않는다(일지 235).
	const double Now = FPlatformTime::Seconds();
	FKindUse& Fine = FineKinds.FindOrAdd(KindKey(Resolution, bLump));
	++Fine.Users;
	Fine.LastUsed = Now;
	if (CoarseResolution > 0)
	{
		FKindUse& Coarse = CoarseKinds.FindOrAdd(CoarseResolution);
		++Coarse.Users;
		Coarse.LastUsed = Now;
	}
}

void USoftbodySurfaceBatch::Unreserve(int32 Resolution, bool bLump, int32 CoarseResolution)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyPoolUnreserve);
	// 쉬는 벌은 바로 지우지 않는다(일지 235): 그 종류가 ReleaseDelay초 동안 쓰이지 않으면 Service가 조금씩 놓는다 — 껐다 곧 켜면 그대로 쓴다.
	const double Now = FPlatformTime::Seconds();
	if (FKindUse* Fine = FineKinds.Find(KindKey(Resolution, bLump)))
	{
		Fine->Users = FMath::Max(Fine->Users - 1, 0);
		Fine->LastUsed = Now;
	}
	if (FKindUse* Coarse = CoarseResolution > 0 ? CoarseKinds.Find(CoarseResolution) : nullptr)
	{
		Coarse->Users = FMath::Max(Coarse->Users - 1, 0);
		Coarse->LastUsed = Now;
	}
}

int64 USoftbodySurfaceBatch::GetIdleTextureBytes() const
{
	int64 Total = 0;
	for (const FSoftbodySurfaceTextures& Set : Idle)
	{
		Total += Set.Bytes();
	}
	for (const FSoftbodySurfaceCoarseTextures& Set : IdleCoarse)
	{
		Total += Set.Bytes();
	}
	for (const FSoftbodySurfaceTextures& Set : Building)
	{
		Total += Set.Bytes();
	}
	for (const FSoftbodySurfaceCoarseTextures& Set : BuildingCoarse)
	{
		Total += Set.Bytes();
	}
	return Total;
}

void USoftbodySurfaceBatch::Service()
{
	const double Now = FPlatformTime::Seconds();
	ServiceTextures(Now);
	ServiceContactMaps(Now);
	USoftbodySurfaceField::KeepPipelinesAlive();
	// 조각이 있는 장이 켜져 있으면 조각 순열 파이프라인을 끝까지 데운다(묻는 것이 데우기를 한 걸음씩 민다).
	if (bWantHardPipelines && USoftbodySurfaceField::AreHardPipelinesReady())
	{
		bWantHardPipelines = false;
	}
	// 잠든 표면 장: ReleaseDelay가 지나면 렌더 스레드 상태를 놓는다(한 프레임에 하나 — 놓기도 렌더 스레드 일이다).
	const double Delay = FMath::Max((double)CVarSoftbodySurfaceReleaseDelay.GetValueOnGameThread(), 0.0);
	bool bReleased = false;
	for (int32 Index = Sleepers.Num() - 1; Index >= 0; --Index)
	{
		USoftbodySurfaceField* Field = Sleepers[Index].Get();
		if (!Field || !Field->IsAsleep())
		{
			Sleepers.RemoveAtSwap(Index);
			continue;
		}
		if (!bReleased && Now - Field->GetSleepSince() >= Delay)
		{
			Field->ReleaseRenderState();
			Sleepers.RemoveAtSwap(Index);
			bReleased = true;
		}
	}
}

void USoftbodySurfaceBatch::AddSleeper(USoftbodySurfaceField* Field)
{
	Sleepers.AddUnique(Field);
}

void USoftbodySurfaceBatch::ServiceTextures(double Now)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyPoolService);
	const int32 Keep = FMath::Max(CVarSoftbodySurfaceTexturePool.GetValueOnGameThread(), 0);
	const float BudgetMB = CVarSoftbodySurfaceCreateBudget.GetValueOnGameThread();
	const int64 Budget = BudgetMB > 0.f ? (int64)((double)BudgetMB * 1024.0 * 1024.0) : MAX_int64;
	const double Delay = FMath::Max((double)CVarSoftbodySurfaceReleaseDelay.GetValueOnGameThread(), 0.0);
	// 텍스처 한 장을 만들거나 놓는 데는 크기와 상관없는 몫도 든다(렌더 스레드에서 장마다 약 0.1 ms, 일지 235 측정) — 작은 거친 텍스처도 한 장에 0.5 MB로 센다.
	constexpr int64 MinTargetCost = 512 * 1024;

	// 0) 이 프레임에 돌려받은 가는 벌을 0으로(그래프 하나).
	if (PendingClears.Num() > 0)
	{
		FSoftbodySurfaceTextures::ClearTargets(PendingClears);
		PendingClears.Reset();
	}

	// 1) 종류마다 있어야 할 벌 수 = (켜진 장이 있으면 여분 Keep) + 이번 프레임에 받지 못한 장 수. 모자라면 만들기 시작한다 — 받지 못한 장의 몫은 줄 앞에,
	// 여분은 뒤에. 그 사이 꺼져 필요 없어진 만드는 중인 벌은 멈추고 만든 장을 놓는다.
	auto Plan = [&](TMap<int32, FKindUse>& Kinds, auto& IdleSets, auto& Builds, auto&& MakeBuild, auto&& SameKind)
	{
		for (TPair<int32, FKindUse>& Pair : Kinds)
		{
			FKindUse& Use = Pair.Value;
			const int32 Want = (Use.Users > 0 ? Keep : 0) + Use.Wanted;
			int32 Have = 0;
			for (const auto& Set : IdleSets)
			{
				Have += SameKind(Pair.Key, Set) ? 1 : 0;
			}
			int32 Making = 0;
			for (const auto& Set : Builds)
			{
				Making += SameKind(Pair.Key, Set) ? 1 : 0;
			}
			for (int32 Missing = Want - Have - Making, Index = 0; Index < Missing; ++Index)
			{
				if (Use.Wanted > 0)
				{
					Builds.Insert(MakeBuild(Pair.Key), 0);
				}
				else
				{
					Builds.Add(MakeBuild(Pair.Key));
				}
			}
			for (int32 Extra = Have + Making - FMath::Max(Want, Have), Index = Builds.Num() - 1; Extra > 0 && Index >= 0; --Index)
			{
				if (SameKind(Pair.Key, Builds[Index]))
				{
					for (int32 Target = 0; Target < 9; ++Target)
					{
						if (auto* Slot = Builds[Index].Slot(Target); Slot && *Slot)
						{
							Retiring.Add(*Slot);
						}
					}
					Builds.RemoveAt(Index);
					--Extra;
				}
			}
			Use.Wanted = 0;
		}
	};
	Plan(FineKinds, Idle, Building,
		[](int32 Key)
		{
			FSoftbodySurfaceTextures Set;
			Set.Resolution = Key / 2;
			Set.bLump = (Key & 1) != 0;
			return Set;
		},
		[](int32 Key, const FSoftbodySurfaceTextures& Set) { return KindKey(Set.Resolution, Set.bLump) == Key; });
	Plan(CoarseKinds, IdleCoarse, BuildingCoarse,
		[](int32 Key)
		{
			FSoftbodySurfaceCoarseTextures Set;
			Set.Resolution = Key;
			return Set;
		},
		[](int32 Key, const FSoftbodySurfaceCoarseTextures& Set) { return Set.Resolution == Key; });

	// 2) 만든다: 프레임 예산이 다 될 때까지 줄 앞부터 한 장씩(한 프레임에 적어도 한 장). 거친 벌(작다)을 먼저 — 첫 접촉은 두 벌이 다 있어야 돈다.
	int64 Spent = 0;
	bool bCreated = false;
	auto Advance = [&](auto& Builds, auto& IdleSets)
	{
		while (Builds.Num() > 0 && (Spent < Budget || !bCreated))
		{
			auto& Set = Builds[0];
			if (Set.Resolution <= 0)
			{
				Builds.RemoveAt(0);
				continue;
			}
			Spent += FMath::Max<int64>(Set.CreateNextTarget(this), MinTargetCost);
			bCreated = true;
			if (Set.IsComplete())
			{
				Set.IdleSince = Now;
				IdleSets.Add(MoveTemp(Set));
				Builds.RemoveAt(0);
			}
		}
	};
	Advance(BuildingCoarse, IdleCoarse);
	Advance(Building, Idle);

	// 3) 놓는다: 그 종류에 필요한 수(켜진 장이 있으면 여분 Keep, 없으면 0)를 넘는 쉬는 벌 가운데, 그 벌이 쉬기 시작한 때와 그 종류를 마지막으로 쓴 때(켜기·
	// 끄기·잡기) 둘 다에서 Delay초가 지난 것을 오래된 것부터 한 벌씩 — 미리 만든 여분도 그 종류를 끈 뒤 Delay초는 남는다.
	auto Trim = [&](TMap<int32, FKindUse>& Kinds, auto& IdleSets, auto&& KeyOf)
	{
		int32 Oldest = INDEX_NONE;
		for (int32 Index = 0; Index < IdleSets.Num(); ++Index)
		{
			const auto& Set = IdleSets[Index];
			const int32 Key = KeyOf(Set);
			const FKindUse* Use = Kinds.Find(Key);
			const double Since = FMath::Max(Set.IdleSince, Use ? Use->LastUsed : 0.0);
			if (Now - Since < Delay || (Oldest != INDEX_NONE && IdleSets[Oldest].IdleSince <= Set.IdleSince))
			{
				continue;
			}
			const int32 Need = Use && Use->Users > 0 ? Keep : 0;
			int32 Same = 0;
			for (const auto& Other : IdleSets)
			{
				Same += KeyOf(Other) == Key ? 1 : 0;
			}
			if (Same > Need)
			{
				Oldest = Index;
			}
		}
		if (Oldest != INDEX_NONE)
		{
			auto& Set = IdleSets[Oldest];
			for (int32 Target = 0; Target < 9; ++Target)
			{
				if (auto* Slot = Set.Slot(Target); Slot && *Slot)
				{
					Retiring.Add(*Slot);
				}
			}
			IdleSets.RemoveAtSwap(Oldest);
		}
	};
	if (Retiring.Num() == 0)
	{
		Trim(FineKinds, Idle, [](const FSoftbodySurfaceTextures& Set) { return KindKey(Set.Resolution, Set.bLump); });
		Trim(CoarseKinds, IdleCoarse, [](const FSoftbodySurfaceCoarseTextures& Set) { return Set.Resolution; });
	}

	// 4) 놓기로 한 렌더 타깃의 GPU 메모리를 프레임 예산만큼 돌려준다(한 프레임에 적어도 한 장).
	int64 Freed = 0;
	while (Retiring.Num() > 0 && (Freed < Budget || Freed == 0))
	{
		UTextureRenderTarget2D* Target = Retiring.Pop(EAllowShrinking::No);
		Freed += FMath::Max<int64>(FSoftbodySurfaceTextures::TargetBytes(Target), MinTargetCost);
		FSoftbodySurfaceTextures::ReleaseTarget(Target);
	}

	// 5) 아무도 쓰지 않고 쉬는·만드는 벌도 없는 종류는 잊는다.
	auto Forget = [&](TMap<int32, FKindUse>& Kinds, const auto& IdleSets, const auto& Builds, auto&& KeyOf)
	{
		for (auto It = Kinds.CreateIterator(); It; ++It)
		{
			if (It->Value.Users > 0 || Now - It->Value.LastUsed < Delay)
			{
				continue;
			}
			bool bInUse = false;
			for (const auto& Set : IdleSets)
			{
				bInUse |= KeyOf(Set) == It->Key;
			}
			for (const auto& Set : Builds)
			{
				bInUse |= KeyOf(Set) == It->Key;
			}
			if (!bInUse)
			{
				It.RemoveCurrent();
			}
		}
	};
	Forget(FineKinds, Idle, Building, [](const FSoftbodySurfaceTextures& Set) { return KindKey(Set.Resolution, Set.bLump); });
	Forget(CoarseKinds, IdleCoarse, BuildingCoarse, [](const FSoftbodySurfaceCoarseTextures& Set) { return Set.Resolution; });
}

FSoftbodyContactMapEntry* USoftbodySurfaceBatch::FindContactMap(const USoftbodyMeshData* Data)
{
	return ContactMaps.FindByPredicate([Data](const FSoftbodyContactMapEntry& Entry) { return Entry.Data == Data; });
}

const FSoftbodyContactMapEntry* USoftbodySurfaceBatch::FindContactMap(const USoftbodyMeshData* Data) const
{
	return ContactMaps.FindByPredicate([Data](const FSoftbodyContactMapEntry& Entry) { return Entry.Data == Data; });
}

void USoftbodySurfaceBatch::AcquireContactMap(USoftbodyMeshData* Data)
{
	if (!Data)
	{
		return;
	}
	FSoftbodyContactMapEntry* Entry = FindContactMap(Data);
	if (!Entry)
	{
		Entry = &ContactMaps.AddDefaulted_GetRef();
		Entry->Data = Data;
		Data->GetContactMapPaths(Entry->Paths);
		Entry->PathDone.Init(false, Entry->Paths.Num());
	}
	++Entry->Users;
	Entry->LastUsed = FPlatformTime::Seconds();
	if (!Entry->bReady && !Entry->bFailed)
	{
		// 이미 메모리에 있는 텍스처는 바로 쥔다(에디터·다른 장이 쓰는 중·끈 지 얼마 안 됨) — 그러면 같은 프레임에 준비된다. 없는 것은 Service가 나눠 올린다.
		ServiceContactMaps(Entry->LastUsed);
	}
}

void USoftbodySurfaceBatch::ReleaseContactMap(const USoftbodyMeshData* Data)
{
	if (FSoftbodyContactMapEntry* Entry = FindContactMap(Data))
	{
		Entry->Users = FMath::Max(Entry->Users - 1, 0);
		Entry->LastUsed = FPlatformTime::Seconds();
	}
}

bool USoftbodySurfaceBatch::IsContactMapReady(const USoftbodyMeshData* Data, bool* OutFailed) const
{
	const FSoftbodyContactMapEntry* Entry = FindContactMap(Data);
	if (OutFailed)
	{
		*OutFailed = Entry && Entry->bFailed;
	}
	return Entry && Entry->bReady;
}

void USoftbodySurfaceBatch::ServiceContactMaps(double Now)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyContactMapService);
	const float BudgetMB = CVarSoftbodySurfaceLoadBudget.GetValueOnGameThread();
	const int64 Budget = BudgetMB > 0.f ? (int64)((double)BudgetMB * 1024.0 * 1024.0) : MAX_int64;
	const double Delay = FMath::Max((double)CVarSoftbodySurfaceReleaseDelay.GetValueOnGameThread(), 0.0);

	// 1) 끝난 요청을 거둔다.
	int64 InFlight = 0;
	for (int32 Index = ContactMapRequests.Num() - 1; Index >= 0; --Index)
	{
		FContactMapRequest& Request = ContactMapRequests[Index];
		if (Request.Handle.IsValid() && !Request.Handle->HasLoadCompleted() && !Request.Handle->WasCanceled())
		{
			InFlight += Request.Bytes;
			continue;
		}
		UObject* Object = Request.Path.ResolveObject();
		for (FSoftbodyContactMapEntry& Entry : ContactMaps)
		{
			const int32 PathIndex = Entry.Paths.IndexOfByKey(Request.Path);
			if (PathIndex != INDEX_NONE && !Entry.PathDone[PathIndex])
			{
				Entry.PathDone[PathIndex] = true;
				if (Object)
				{
					Entry.Loaded.Add(Object);
				}
				else if (!Entry.bFailed)
				{
					Entry.bFailed = true;
					UE_LOG(LogSoftbodySurfaceBatch, Warning, TEXT("Contact is off for %s: its contact map texture %s could not be loaded. Press Rebake Contact Map."),
						*GetPathNameSafe(Entry.Data), *Request.Path.ToString());
				}
			}
		}
		ContactMapRequests.RemoveAtSwap(Index);
	}

	// 2) 쓰는 장이 있는 지도마다: 메모리에 있는 텍스처는 바로 쥐고, 없는 것은 올리는 양이 예산 안이면 요청한다(한 번에 적어도 한 장).
	for (FSoftbodyContactMapEntry& Entry : ContactMaps)
	{
		if (Entry.Users <= 0 || Entry.bReady || Entry.bFailed || !Entry.Data)
		{
			continue;
		}
		bool bAllDone = true;
		for (int32 PathIndex = 0; PathIndex < Entry.Paths.Num(); ++PathIndex)
		{
			if (Entry.PathDone[PathIndex])
			{
				continue;
			}
			const FSoftObjectPath& Path = Entry.Paths[PathIndex];
			if (UObject* Object = Path.ResolveObject())
			{
				Entry.PathDone[PathIndex] = true;
				Entry.Loaded.Add(Object);
				continue;
			}
			bAllDone = false;
			if (ContactMapRequests.ContainsByPredicate([&Path](const FContactMapRequest& Request) { return Request.Path == Path; }))
			{
				continue;
			}
			// 작은 텍스처도 한 장에 0.5 MB로 센다 — 텍스처를 만드는 데는 크기와 상관없는 몫도 든다(렌더 타깃과 같은 규칙).
			const int64 Bytes = FMath::Max<int64>(EstimateContactMapBytes(Entry.Data, Path), 512 * 1024);
			if (InFlight > 0 && InFlight + Bytes > Budget)
			{
				continue;
			}
			FContactMapRequest& Request = ContactMapRequests.AddDefaulted_GetRef();
			Request.Path = Path;
			Request.Bytes = Bytes;
			++SoftbodyProfiling::GetResourceCounters().ContactMapRequests;
			if (UAssetManager::IsInitialized())
			{
				Request.Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Path, FStreamableDelegate(), FStreamableManager::AsyncLoadHighPriority);
			}
			else
			{
				Path.TryLoad();
			}
			InFlight += Bytes;
		}
		if (bAllDone)
		{
			Entry.bReady = true;
		}
	}

	// 3) 쓰는 장이 없는 채 Delay초가 지난 지도는 놓는다(쥔 것이 없는 텍스처는 다음 GC에 내려간다).
	for (int32 Index = ContactMaps.Num() - 1; Index >= 0; --Index)
	{
		const FSoftbodyContactMapEntry& Entry = ContactMaps[Index];
		if (Entry.Users > 0 || Now - Entry.LastUsed < Delay)
		{
			continue;
		}
		for (int32 Request = ContactMapRequests.Num() - 1; Request >= 0; --Request)
		{
			if (Entry.Paths.Contains(ContactMapRequests[Request].Path))
			{
				if (ContactMapRequests[Request].Handle.IsValid())
				{
					ContactMapRequests[Request].Handle->CancelHandle();
				}
				ContactMapRequests.RemoveAtSwap(Request);
			}
		}
		ContactMaps.RemoveAtSwap(Index);
	}
}

void USoftbodySurfaceBatch::OnPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World == GetWorld())
	{
		// 액터 틱(표면 장의 신청·풀 요청)이 다 끝난 뒤: 풀과 접촉 지도를 프레임 예산만큼 손보고, 이번 프레임 갱신을 올린다.
		Service();
#if STATS
		SET_FLOAT_STAT(STAT_SoftbodyFieldPoolMemory, (double)GetIdleTextureBytes() / (1024.0 * 1024.0));
#endif
		Flush();
	}
}

void USoftbodySurfaceBatch::Flush()
{
	if (Pending.Num() == 0)
	{
		return;
	}
	// 접촉 패스의 파이프라인이 아직 만들어지는 중이면(드라이버 캐시가 빈 첫 실행) 이번 프레임 갱신을 모두 미룬다 — 그 자리에서 만들면 게임이 몇 초 멈춘다
	// (일지 235). 미룬 갱신과 같이 다루므로 접촉이 조금 늦게 시작할 뿐이다.
	if (!USoftbodySurfaceField::ArePipelinesReady())
	{
		for (FPending& Entry : Pending)
		{
			if (USoftbodySurfaceField* Field = Entry.Field.Get())
			{
				Field->OnScheduled(false);
			}
		}
		Pending.Reset();
		return;
	}
	// 1) 이번 프레임에 돌 차례인 것: 닿는 순간이거나, 제 간격(화면 크기로 정한다)만큼 미뤘고 주기 상한의 칸이 바뀌었다.
	// 주기 상한(일지 156): 시간을 초당 갱신 수 칸으로 나눠 마지막 갱신과 다른 칸에 들어선 프레임에만 돈다 — 초당 횟수가 상한을 넘지
	// 않고, 프레임 시간이 고르지 않아도 밀리거나 몰아 돌지 않는다. 칸 시작을 표면 장마다 엇갈려(RatePhase) 같은 프레임에 몰리지 않는다.
	const float Rate = MaxUpdateRate();
	const double Now = GetWorld()->GetTimeSeconds();
	TArray<int32, TInlineAllocator<32>> Due;
	// 하드 서피스 순열(ADR-0037 §3.7)의 파이프라인은 조각이 있는 장이 켜질 때(ReservePool) 데우기 시작하고, 준비될 때까지 그 장만 미룬다(원칙 E06 —
	// 조각이 없는 프로젝트는 묻지도 않는다).
	int32 HardReady = -1;
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		const FPending& Entry = Pending[Index];
		if (!Entry.Field.IsValid())
		{
			continue;
		}
		if (Entry.Field->UsesHardSurface())
		{
			if (HardReady < 0)
			{
				HardReady = USoftbodySurfaceField::AreHardPipelinesReady() ? 1 : 0;
			}
			if (HardReady == 0)
			{
				continue;
			}
		}
		const FSoftbodySurfaceSchedule& S = Entry.Schedule;
		const bool bRateDue = Rate <= 0.f || S.LastUpdateTime < 0.0
			|| FMath::FloorToDouble(Now * Rate + S.RatePhase) > FMath::FloorToDouble(S.LastUpdateTime * Rate + S.RatePhase);
		const int32 Interval = UpdateIntervalFor(S.ScreenSize);
		if (S.bNewlyTouched || (bRateDue && S.FramesSinceUpdate + 1 >= Interval))
		{
			Due.Add(Index);
		}
	}
	// 2) 상한이 있으면 줄을 세운다. 앞줄부터: 닿는 순간 > 화면에 있고 (닿을 수 있는 것이 있거나 최대 간격만큼 미뤘다) > 화면에 있고
	// 떼고 돌아가는 중 > 화면에 없음. 같은 줄 안에서는 화면에서 크고 오래 미룬 것 먼저.
	// 떼고 돌아가는 장도 최대 간격(softbody.Surface.MaxInterval)을 기다리면 닿을 수 있는 것과 같은 줄에 선다. 전에는 늘 뒷줄이라,
	// 닿을 수 있는 것이 상한을 채우는 동안 한 번도 돌지 못하고 눌린 모양 그대로 멈춰 있었다(일지 136).
	const int32 Budget = CVarSoftbodySurfaceUpdateBudget.GetValueOnGameThread();
	const int32 MaxFirst = CVarSoftbodySurfaceFirstUpdates.GetValueOnGameThread();
	int32 NumFirst = 0;
	for (int32 Index : Due)
	{
		NumFirst += Pending[Index].Schedule.bFirstUpdate ? 1 : 0;
	}
	if ((Budget > 0 && Due.Num() > Budget) || (MaxFirst > 0 && NumFirst > MaxFirst))
	{
		const int32 MaxInterval = FMath::Max(CVarSoftbodySurfaceMaxInterval.GetValueOnGameThread(), 1);
		const auto RowOf = [MaxInterval](const FSoftbodySurfaceSchedule& S)
		{
			if (S.bNewlyTouched || S.bGrabbing)
			{
				return 0;
			}
			if (!S.bInView)
			{
				return 3;
			}
			return S.bHasCandidates || S.FramesSinceUpdate + 1 >= MaxInterval ? 1 : 2;
		};
		Due.Sort([this, &RowOf](int32 A, int32 B)
		{
			const FSoftbodySurfaceSchedule& SA = Pending[A].Schedule;
			const FSoftbodySurfaceSchedule& SB = Pending[B].Schedule;
			const int32 RowA = RowOf(SA);
			const int32 RowB = RowOf(SB);
			if (RowA != RowB)
			{
				return RowA < RowB;
			}
			return SA.ScreenSize * (float)(SA.FramesSinceUpdate + 1) > SB.ScreenSize * (float)(SB.FramesSinceUpdate + 1);
		});
		// 처음 갱신(렌더 스레드 표를 만드는 갱신, 일지 235)은 앞줄부터 MaxFirst개만 — 나머지는 다음 프레임에(닿는 순간 표시가 남아 다시 앞줄에 선다).
		if (MaxFirst > 0 && NumFirst > MaxFirst)
		{
			int32 First = 0;
			Due.RemoveAll([this, MaxFirst, &First](int32 Index)
			{
				return Pending[Index].Schedule.bFirstUpdate && ++First > MaxFirst;
			});
		}
		if (Budget > 0 && Due.Num() > Budget)
		{
			Due.SetNum(Budget);
		}
	}
	TBitArray<> Run(false, Pending.Num());
	for (int32 Index : Due)
	{
		Run[Index] = true;
	}
	// 3) 고른 것들을 그래프 하나로 올리고, 신청한 모두에게 알린다(미룬 것은 시간을 모아 두고 머티리얼의 지난 변위를 지금 것으로).
	TArray<FSoftbodySurfaceFieldRender> Renders;
	Renders.Reserve(Due.Num());
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		FPending& Entry = Pending[Index];
		USoftbodySurfaceField* Field = Entry.Field.Get();
		if (!Field)
		{
			continue;
		}
		if (Run[Index])
		{
			Renders.Add(MoveTemp(Entry.Render));
		}
		Field->OnScheduled(Run[Index]);
	}
	Pending.Reset();
	if (Renders.Num() > 0)
	{
		ENQUEUE_RENDER_COMMAND(SoftbodySurfaceBatch)(
			[Renders = MoveTemp(Renders)](FRHICommandListImmediate& RHICmdList) mutable
			{
				USoftbodySurfaceField::RenderBatch(RHICmdList, Renders);
			});
	}
}
