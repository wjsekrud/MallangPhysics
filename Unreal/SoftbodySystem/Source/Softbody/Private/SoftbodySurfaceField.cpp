// Copyright (c) 2026 wjsekrud

#include "SoftbodySurfaceField.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "ImageCore.h"
#include "Misc/App.h"
#include "Misc/EngineVersionComparison.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"
#include "SoftbodyColliderField.h"
#include "SoftbodyColliderShapes.h"
#include "SoftbodyContactResponse.h"
#include "SoftbodyComponent.h"
#include "SoftbodyInertiaRuntime.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyProfiling.h"
#include "SoftbodySurfaceBatch.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "TextureResource.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodySurfaceField, Log, All);

// 받아들인 깊이(자국, ADR-0003 §14) 가운데 압력 없이 모양만 바뀐 몫(소성, ADR-0037 §3.6) — 접촉 압력 목표에서만 뺀다(오래 쥔 자국 둘레의 솟음과
// 칸보다 짧은 압축 몫이 그만큼 가라앉고 함몰 모양은 그대로). 조절값(Bulge Settle)이었다가 기본값 0.5로 고정했다(일지 227).
static constexpr float PlasticShare = 0.5f;

// GPU 시간(`stat gpu`): 표면 장 갱신을 단계별로 잰다. 같은 값을 stat softbody에도 보이려고, GPU 프로파일러가 끝난 프레임마다
// 알려 주는 시간을 받아 둔다(GetGpuTimes).
namespace SoftbodyGpuTimes
{
	std::atomic<double> Ms[(int32)USoftbodySurfaceField::EGpuTime::Num];
	/** 받은 때(FPlatformTime::Seconds). */
	std::atomic<double> Stamp[(int32)USoftbodySurfaceField::EGpuTime::Num];
	/**
	 * 받은 때의 렌더 프레임 번호(일지 222). 한 GPU 프레임의 단계 시간은 한 번에 알려 오므로 같은 번호를 받는다 — 가장 늦은 번호보다 앞선 단계는 그
	 * 프레임에 돌지 않은 것(접촉 단계 Basic·끌림 마찰 0의 끌림처럼)이라 0으로 보인다. 벽시계 0.5초만 보면 프레임이 빠른 실행에서 지난 값이 남았다.
	 */
	std::atomic<uint64> Frame[(int32)USoftbodySurfaceField::EGpuTime::Num];
}

// 5.6·5.7의 GPU 프로파일러에는 시간을 알려 주는 콜백(OnTimingResults)이 없어 stat gpu에만 보이고 stat softbody의 GPU 시간은 0이다.
#if HAS_GPU_STATS && !UE_VERSION_OLDER_THAN(5, 8, 0)
template <typename TNameProvider, int32 Slot>
struct TSoftbodyGpuStat : public UE::RHI::GPUProfiler::TGPUStat<TNameProvider>
{
	using EAction = UE::RHI::GPUProfiler::FGPUStat::EOnTimingResultsAction;

	virtual EAction OnTimingResults(UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double IdleMs, double WaitMs) override
	{
		if (Queue.Type == UE::RHI::GPUProfiler::FQueue::EType::Graphics && Queue.GPU == 0)
		{
			SoftbodyGpuTimes::Ms[Slot].store(BusyMs, std::memory_order_relaxed);
			SoftbodyGpuTimes::Stamp[Slot].store(FPlatformTime::Seconds(), std::memory_order_relaxed);
			SoftbodyGpuTimes::Frame[Slot].store(GFrameCounterRenderThread, std::memory_order_relaxed);
		}
		return EAction::Keep;
	}
};
#define SOFTBODY_GPU_STAT(StatName, NameString, Slot) \
	DECLARE_GPU_STAT_NAME_TYPE(StatName, NameString); \
	static TSoftbodyGpuStat<TRHIGPUStatNameProvider_##StatName, (int32)USoftbodySurfaceField::EGpuTime::Slot> GPUStat_##StatName
#else
#define SOFTBODY_GPU_STAT(StatName, NameString, Slot) DECLARE_GPU_STAT_NAMED(StatName, NameString)
#endif

SOFTBODY_GPU_STAT(SoftbodySurfaceContact, TEXT("Softbody Contact"), Contact);
// 함몰(ADR-0032) 두 패스 + 옮기기와 끌림(ADR-0033) 두 패스를 따로 잰다.
SOFTBODY_GPU_STAT(SoftbodySurfaceDent, TEXT("Softbody Dent"), Dent);
SOFTBODY_GPU_STAT(SoftbodySurfaceDrag, TEXT("Softbody Drag"), Drag);
// 이완 묶음 목록 만들기(ADR-0013 2단계): 이완 반복과 따로 보이게 한다.
SOFTBODY_GPU_STAT(SoftbodySurfaceTiles, TEXT("Softbody Tiles"), Tiles);
SOFTBODY_GPU_STAT(SoftbodySurfacePressure, TEXT("Softbody Pressure"), Pressure);
SOFTBODY_GPU_STAT(SoftbodySurfaceCompose, TEXT("Softbody Compose"), Compose);
SOFTBODY_GPU_STAT(SoftbodySurfaceLump, TEXT("Softbody Lump"), Lump);

// 프로파일링 스위치(일지 189): 끌림 바로잡기 비용을 같은 장면에서 견줘 보려고 둔다. 끌림 모양이 바뀌므로 비용을 잴 때만 쓴다.
static TAutoConsoleVariable<int32> CVarSoftbodyDragCorrectPasses(
	TEXT("softbody.Drag.CorrectPasses"), 1,
	TEXT("Profiling switch. Drag correction passes per contact update, 0 to 4 (default 1). 0 skips the pass to measure its GPU cost; ")
	TEXT("skin held under a still press then creeps slowly instead of staying put."),
	ECVF_RenderThreadSafe);

// 끌림 걸음 나누기(일지 191): 바로잡기·모으기의 거친 8×8 묶음 하나를 조각 넷(스레드 64 × 4)이 원천을 나눠 돈다. 1은 일지 190까지의 셈과 비트까지 같아
// 같은 장면에서 비용을 견줄 때 쓴다.
static TAutoConsoleVariable<int32> CVarSoftbodyDragSlices(
	TEXT("softbody.Drag.Slices"), 4,
	TEXT("Drag walk split. Each 8x8 group of coarse cells in the drag correct and gather passes runs as this many slices of 64 threads that ")
	TEXT("share the walk over the sources: 4 (default), 2, or 1 (a single slice, the same computation as before the split, bit for bit). ")
	TEXT("More slices shorten the longest serial walk; the drag result differs only by floating-point rounding."),
	ECVF_RenderThreadSafe);

// 멈춤(일지 203, ADR-0021 §8): 눌린 채 입력이 멈춘 장은 평형에 닿은 뒤 갱신을 쉰다. 두 문턱 모두 화면에 보이지 않는 크기(한 칸·반정밀도 아래).
static TAutoConsoleVariable<float> CVarSoftbodySurfaceStillDistance(
	TEXT("softbody.Surface.StillDistance"), 0.1f,
	TEXT("An object that is pressed but whose colliders, bones and settings have not moved more than this distance (cm) since its last ")
	TEXT("contact update, and whose last update changed no displacement by more than softbody.Surface.StillChange, skips contact updates ")
	TEXT("until something moves (GPU cost 0 while still; it wakes the same frame anything moves). Rotations count as the distance they ")
	TEXT("move a point at the object's bounding radius. 0 = never skip. Default 0.1."),
	ECVF_Scalability);

static TAutoConsoleVariable<float> CVarSoftbodySurfaceStillChange(
	TEXT("softbody.Surface.StillChange"), 0.001f,
	TEXT("Displacement change (cm) below which a contact update counts as having changed nothing, for softbody.Surface.StillDistance. ")
	TEXT("Default 0.001 (0.01 mm)."),
	ECVF_Scalability);

// 덩어리 평형의 갱신당 뉴턴 걸음 수(일지 254): 품질 단계(Low/High/Full)를 정하려고 걸음을 바꿔 결과·비용을 잰다. 기본 2(NumLumpSteps).
static TAutoConsoleVariable<int32> CVarSoftbodyLumpSteps(
	TEXT("softbody.Lump.Steps"), USoftbodySurfaceField::NumLumpSteps,
	TEXT("Development: solver steps per contact update for how a pressed lump gives way (default 2, 1 to 8). Fewer steps cost less and ")
	TEXT("reach the balance over a few updates instead of one. For measuring quality settings."),
	ECVF_Default);

/** 덩어리 평형 반복 상태의 묶음마다 줄 수(셰이더 SB_LUMP_STATE_ROWS, LumpSolveCS 주석). */
static constexpr uint32 LumpStateRows = 30;

/** 모양 거리장 칸 값의 GPU 버퍼. 렌더 스레드만 만진다. */
struct FSoftbodyFieldVoxelBuffer
{
	TRefCountPtr<FRDGPooledBuffer> Buffer;
};

/**
 * 쉬기(ADR-0013). 렌더 스레드만 만진다.
 * Buffer = 셰이더 RestFrames — [0] 마지막으로 눌린 뒤 지난 프레임 수, [1] 1 = 비워서 쉬는 중.
 * 묶음 목록(2단계): TileResidual = 묶음마다 갱신 끝에 남은 것(붙든 자리·함몰 비트), TileAnyResidual = 어느 묶음이든 남았는지(1칸),
 * TileLinkPairs = 고스트 칸이 값을 가져오는 (묶음, 다른 묶음) 쌍과 그 수(캐리어 고스트 표가 바뀌면 다시 만든다).
 * 접촉 묶음(3단계): ContactGroups = 표면·고스트 칸이 든 8×8 묶음(+ 덩어리 노멀 표지), ContactGroupCount = 그 수, ContactArgs =
 * 접촉 패스 간접 디스패치 인자, LumpList = 덩어리 칸이 든 8×8 묶음과 그 수(캐리어 위치·덩어리 텍스처가 바뀌면 다시 만든다).
 * 솟음(ADR-0031)·끌림(ADR-0033): CoarseRow = 거친 칸마다 거리표 줄 번호(CoarseRowCS, 캐리어로만 정해진다). SpreadStatic* = 거친 칸 표
 * (CoarseInitCS·CoarseRowCS)를 만든 캐리어와 텍스처(비교만 한다).
 * stat softbody: GPU 상태 읽기 고리(렌더 스레드만 만진다). 거둔 값은 원자 변수로 게임 스레드가 읽는다(GetGpuState).
 */
struct FSoftbodySurfaceRestState
{
	TRefCountPtr<FRDGPooledBuffer> Buffer;
	/** 발자국 칸 판 자리 표(HardCellPlate)를 한 번 올린 버퍼와 그 표의 일련번호(다시 구우면 표가 새로 만들어져 다시 올린다). */
	TRefCountPtr<FRDGPooledBuffer> HardCellPlate;
	uint32 HardPlateSerial = 0;
	TRefCountPtr<FRDGPooledBuffer> TileResidual;
	TRefCountPtr<FRDGPooledBuffer> TileAnyResidual;
	TRefCountPtr<FRDGPooledBuffer> TileLinkPairs;
	TRefCountPtr<FRDGPooledBuffer> TileLinkPairCount;
	/**
	 * 부위 나누기: 묶음마다 1비트 — 부위 몫이 함몰 채널에 닿을 수 있는 묶음(TileLinksCS가 이음새 쌍과 같이 만든다). 부위 나누기가 없는
	 * 캐리어는 모두 0이다.
	 */
	TRefCountPtr<FRDGPooledBuffer> TileParts;
	/** TileLinkPairs를 만든 고스트 표와 TileParts를 만든 부위 몫 텍스처(비교만 한다). */
	const FRHITexture* TileLinksRedirect = nullptr;
	const FRHITexture* TileLinksSeparation = nullptr;
	uint32 TilesPerSide = 0;
	/**
	 * 끌림 거친 격자 이웃 표(DragLinksCS, 일지 162): 캐리어로만 정해지는 표라 칸 종류 표처럼 표면 장이 사는 동안 여기 둔다. 만든 캐리어와
	 * 거친 격자 해상도, 0으로 비운 끌림 텍스처는 비교만 한다.
	 */
	TRefCountPtr<IPooledRenderTarget> DragLinks;
	const FRHITexture* DragLinksCarrier = nullptr;
	uint32 DragLinksResolution = 0;
	const FRHITexture* DragClearedTarget = nullptr;
	/** 지난 갱신에 끌림 패스를 돌렸는가 — 끄는 순간(마찰 0·접촉 단계 Basic) 끌림 텍스처를 0으로 비운다(일지 222). */
	bool bDragLast = false;
	TRefCountPtr<FRDGPooledBuffer> ContactGroups;
	TRefCountPtr<FRDGPooledBuffer> ContactGroupCount;
	TRefCountPtr<FRDGPooledBuffer> ContactArgs;
	TRefCountPtr<FRDGPooledBuffer> LumpList;
	TRefCountPtr<FRDGPooledBuffer> LumpListCount;
	/** ContactGroups를 만든 캐리어 위치·덩어리 텍스처와 해상도(비교만 한다). */
	const FRHITexture* ContactGroupsCarrier = nullptr;
	const FRHITexture* ContactGroupsLump = nullptr;
	uint32 ContactGroupsResolution = 0;
	TRefCountPtr<IPooledRenderTarget> CoarseRow;
	/** 거친 칸마다 흐린 재료(부피 보존 v, 겉껍질 길이 — CoarseMaterialCS, ADR-0037 §3.2; 캐리어와 칠로만 정해진다). */
	TRefCountPtr<IPooledRenderTarget> CoarseMaterial;
	/** 거친 칸마다 흐린 겉 굽힘 길이 ℓ_b(cm, 연속 굽힘 — CoarseMaterialCS, R16F; Flex 칠과 Flex Offset으로 정해진다, 흰 자산은 모두 0). */
	TRefCountPtr<IPooledRenderTarget> CoarseBend;
	/** 그 표의 거친 칸 흐린 굽힘 길이 최대(cm × 1000, 1 낱말 — 모으기가 셈할 칸 상자를 넓힌다). */
	TRefCountPtr<FRDGPooledBuffer> CoarseBendMax;
	const FRHITexture* SpreadStaticCarrier = nullptr;
	const FRHITexture* SpreadStaticTarget = nullptr;
	/** 거친 칸 표를 만든 거리표(비교만 한다 — 표만 다시 구우면 줄 번호를 다시 적는다). */
	const FRHITexture* SpreadStaticLandmarks = nullptr;
	// 거친 칸 표가 읽은 두께·부피 텍스처(CoarseLayer x·z — 함몰 거친 해의 폭, 일지 199): 다시 구우면 다시 만든다.
	const FRHITexture* SpreadStaticNormal = nullptr;
	const FRHITexture* SpreadStaticVolume = nullptr;
	/** 거친 칸 표를 만든 저작 에셋 압축성 값(FSoftbodySurfaceFieldRender::VolumeValue) — 바꾸면(에디터에서) 표를 다시 만든다. */
	float SpreadStaticVolumeValue = -1.f;
	/** 그 표를 만든 컴포넌트 Volume Offset(FSoftbodySurfaceFieldRender::VolumeOffset). −2 = 아직 안 만듦(값은 −1~1). */
	float SpreadStaticVolumeOffset = -2.f;
	/** 거친 칸 표가 읽은 겉껍질 칠과 칠 1의 겉껍질 길이(FSoftbodySurfaceFieldRender::SkinLength) — 다시 굽거나 저작 에셋 값을 바꾸면 표를 다시 만든다. */
	float SpreadStaticSkinLength = -1.f;
	/** 거친 칸 표(굽힘 길이)를 만든 Flex 칠 텍스처와 Flex Offset(−2 = 아직 안 만듦) — 바뀌면 표를 다시 만든다(연속 굽힘). */
	const FRHITexture* SpreadStaticSoftness = nullptr;
	float SpreadStaticFlexOffset = -2.f;
	/**
	 * 함몰 거친 해(일지 199): 0으로 비운 거친 해 텍스처(비교만 한다), 지난 갱신에 썼는가(거친 격자 한 변이 이웃 표 한도를 넘나들어 꺼졌다 켜질 때 남은 값을 비운다), 거친 묶음마다 남은 표지
	 * (셰이더 SB_COARSE_BUSY_* — 다음 갱신 거친 목록의 씨앗, 갱신을 넘어 남는다)와 그 거친 묶음 한 변 수.
	 */
	const FRHITexture* DentCoarseClearedTarget = nullptr;
	bool bDentCoarseLast = false;
	TRefCountPtr<FRDGPooledBuffer> DentCoarseBusy;
	uint32 DentCoarseTilesPerSide = 0;
	/** 함몰·옮기기가 읽는 칸 종류 표(R8, 일지 156 — 비트 2는 고스트 띠 바깥 고리, ADR-0032). 만든 캐리어 위치는 비교만 한다. */
	TRefCountPtr<IPooledRenderTarget> CarrierKind;
	const FRHITexture* CarrierKindSource = nullptr;
	/** 덩어리 평형(ADR-0017)의 반복 상태: 묶음마다 셰이더 SB_LUMP_STATE_ROWS줄(LumpSolveCS 주석). 프레임을 넘어 이어 푼다. */
	TRefCountPtr<FRDGPooledBuffer> LumpState;
	/**
	 * 부위 나누기의 거친 칸 표(RG16F, 살 칸의 두 부위 몫 평균 — 끌림이 읽는다): 다른 거친 칸 표(CoarseRow·CoarseMaterial)와 같이 CoarseInitCS가 만든다.
	 * 캐리어에 부위 나누기가 구워져 있을 때만 쓴다.
	 */
	TRefCountPtr<IPooledRenderTarget> CoarseSeparation;
	const FRHITexture* SpreadStaticSeparation = nullptr;

	/**
	 * 셰이더 StatOut: [0] 쉰다, [1] 함몰 묶음 수, [2] 접촉 묶음 수, [3] 눌린 칸이 있다, [4] 끌림 원천 수(끄는 힘이 1 µm 이상인 원천이 하나도 없으면 0 — 그때는
	 * 모으기가 원천을 올리지 않는다, 일지 179), [5] 끌림이 닿은 거친 묶음 수, [6] 끌림 묶음 원천 수, [7] 섞인 묶음 원천 수, [8..13] 바로잡기·[14..19] 모으기
	 * 걸음 집계(묶음 수, 걸음 최대, 합, 가까운 묶음 올림 최대, 가까운·먼 걸음 최대 — 일지 189), [20] 부위 묶음 수, [21..25] 함몰 목록 까닭별 수,
	 * [26] 1 = 부위 나누기 장(셰이더 SB_TILE_COUNT_PARTS·SB_TILE_COUNT_WHY 주석), [27] 살 두께보다 깊이 눌린 표면 칸 수,
	 * [28] 가장 깊이 넘은 양(cm, float 비트 — 셰이더 SB_ACTIVITY_DEEP, 일지 295), [29] 들어온 면 표지 자리가 모자란 표면 칸 수(일지 313).
	 */
	static constexpr int32 NumStatSlots = 3;
	static constexpr uint32 NumStatWords = 30;
	TUniquePtr<FRHIGPUBufferReadback> StatReadbacks[NumStatSlots];
	/** 칸마다 번호(0 = 빈 칸). 늦게 끝난 옛 값이 새 값을 덮지 않게 한다. */
	uint64 StatSerial[NumStatSlots] = {};
	/** 칸마다 그 읽기를 넣은 렌더 프레임(GFrameCounterRenderThread). */
	uint64 StatFrame[NumStatSlots] = {};
	uint64 NextStatSerial = 1;
	uint64 LatestStatSerial = 0;
	std::atomic<uint32> StatWords[NumStatWords] = {};
	std::atomic<uint64> LatestStatFrame{0};
	std::atomic<bool> bHasStats{false};

	/** 끝난 읽기를 거둔다(렌더 스레드). */
	void PollStats()
	{
		for (int32 Slot = 0; Slot < NumStatSlots; ++Slot)
		{
			if (StatSerial[Slot] == 0 || !StatReadbacks[Slot] || !StatReadbacks[Slot]->IsReady())
			{
				continue;
			}
			const uint32* Words = static_cast<const uint32*>(StatReadbacks[Slot]->Lock(NumStatWords * sizeof(uint32)));
			if (Words && StatSerial[Slot] > LatestStatSerial)
			{
				for (uint32 Index = 0; Index < NumStatWords; ++Index)
				{
					StatWords[Index].store(Words[Index], std::memory_order_relaxed);
				}
				LatestStatSerial = StatSerial[Slot];
				LatestStatFrame.store(StatFrame[Slot], std::memory_order_relaxed);
				bHasStats.store(true, std::memory_order_release);
			}
			StatReadbacks[Slot]->Unlock();
			StatSerial[Slot] = 0;
		}
	}

	/** 빈 읽기 칸(렌더 스레드). 모두 쓰는 중이면 nullptr — 이번 프레임은 읽지 않는다. */
	FRHIGPUBufferReadback* AcquireStat()
	{
		for (int32 Slot = 0; Slot < NumStatSlots; ++Slot)
		{
			if (StatSerial[Slot] == 0)
			{
				if (!StatReadbacks[Slot])
				{
					StatReadbacks[Slot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("SoftbodySurfaceStats"));
				}
				StatSerial[Slot] = NextStatSerial++;
				StatFrame[Slot] = GFrameCounterRenderThread;
				return StatReadbacks[Slot].Get();
			}
		}
		return nullptr;
	}

	/**
	 * 멈춤 표지 읽기 고리(일지 203, ADR-0021 §8): 갱신마다 TileCount 버퍼의 앞 StillWordIndex + 1낱말을 복사해 몇 프레임 뒤에 거둔다(렌더 스레드). 가장 최근
	 * 갱신의 (신청 번호, 표지)를 한 값으로 묶어 게임 스레드가 읽는다(LatestStill: 위 32비트 번호, 아래 32비트 표지 — 셰이더 SB_STILL_PRESSED·SB_STILL_CHANGED).
	 */
	static constexpr uint32 StillWordIndex = 16;   // 셰이더 SB_TILE_COUNT_STILL
	static constexpr int32 NumStillSlots = 3;
	TUniquePtr<FRHIGPUBufferReadback> StillReadbacks[NumStillSlots];
	uint64 StillSerial[NumStillSlots] = {};
	uint64 LatestStillSerial = 0;
	std::atomic<uint64> LatestStill{0};

	void PollStill()
	{
		for (int32 Slot = 0; Slot < NumStillSlots; ++Slot)
		{
			if (StillSerial[Slot] == 0 || !StillReadbacks[Slot] || !StillReadbacks[Slot]->IsReady())
			{
				continue;
			}
			const uint32* Words = static_cast<const uint32*>(StillReadbacks[Slot]->Lock((StillWordIndex + 1) * sizeof(uint32)));
			if (Words && StillSerial[Slot] > LatestStillSerial)
			{
				LatestStillSerial = StillSerial[Slot];
				LatestStill.store((StillSerial[Slot] << 32) | (uint64)Words[StillWordIndex], std::memory_order_release);
			}
			StillReadbacks[Slot]->Unlock();
			StillSerial[Slot] = 0;
		}
	}

	/** 빈 읽기 칸(렌더 스레드). 모두 쓰는 중이면 nullptr — 이번 갱신은 읽지 않는다(다음 갱신이 다시 적는다). */
	FRHIGPUBufferReadback* AcquireStill(uint64 Serial)
	{
		for (int32 Slot = 0; Slot < NumStillSlots; ++Slot)
		{
			if (StillSerial[Slot] == 0)
			{
				if (!StillReadbacks[Slot])
				{
					StillReadbacks[Slot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("SoftbodySurfaceStill"));
				}
				StillSerial[Slot] = Serial;
				return StillReadbacks[Slot].Get();
			}
		}
		return nullptr;
	}

	/**
	 * 잡기(ADR-0041): 잡기 줄(셰이더 GrabState — 갱신을 넘어 남는다, 표면 장이 처음 잡기를 받을 때 0으로 만든다)과 그 읽기 고리. 잡기가 있는 갱신마다 잡기 풀기
	 * 패스 뒤에 1 KB를 복사해 몇 프레임 뒤에 거둔다(렌더 스레드). 가장 최근 값은 잠금으로 게임 스레드와 나눈다(GrabLatestSerial이 그 번호).
	 */
	TRefCountPtr<FRDGPooledBuffer> GrabState;
	static constexpr int32 NumGrabSlots = 3;
	static constexpr uint32 NumGrabFloats = (uint32)(SoftbodyGrab::MaxRows * SoftbodyGrab::StateRows * 4);
	TUniquePtr<FRHIGPUBufferReadback> GrabReadbacks[NumGrabSlots];
	uint64 GrabReadSerial[NumGrabSlots] = {};
	uint64 NextGrabSerial = 1;
	uint64 LatestGrabSerialRT = 0;
	mutable FCriticalSection GrabLock;
	TArray<FVector4f> GrabLatest;
	std::atomic<uint64> GrabLatestSerial{0};

	void PollGrab()
	{
		for (int32 Slot = 0; Slot < NumGrabSlots; ++Slot)
		{
			if (GrabReadSerial[Slot] == 0 || !GrabReadbacks[Slot] || !GrabReadbacks[Slot]->IsReady())
			{
				continue;
			}
			const FVector4f* Rows = static_cast<const FVector4f*>(GrabReadbacks[Slot]->Lock(NumGrabFloats * sizeof(float)));
			if (Rows && GrabReadSerial[Slot] > LatestGrabSerialRT)
			{
				LatestGrabSerialRT = GrabReadSerial[Slot];
				FScopeLock Lock(&GrabLock);
				GrabLatest.SetNumUninitialized(SoftbodyGrab::MaxRows * SoftbodyGrab::StateRows);
				FMemory::Memcpy(GrabLatest.GetData(), Rows, NumGrabFloats * sizeof(float));
				GrabLatestSerial.store(LatestGrabSerialRT, std::memory_order_release);
			}
			GrabReadbacks[Slot]->Unlock();
			GrabReadSerial[Slot] = 0;
		}
	}

	/** 빈 읽기 칸(렌더 스레드). 모두 쓰는 중이면 nullptr — 이번 갱신은 읽지 않는다. */
	FRHIGPUBufferReadback* AcquireGrab()
	{
		for (int32 Slot = 0; Slot < NumGrabSlots; ++Slot)
		{
			if (GrabReadSerial[Slot] == 0)
			{
				if (!GrabReadbacks[Slot])
				{
					GrabReadbacks[Slot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("SoftbodySurfaceGrab"));
				}
				GrabReadSerial[Slot] = NextGrabSerial++;
				return GrabReadbacks[Slot].Get();
			}
		}
		return nullptr;
	}
};

/**
 * 덩어리 평형 결과(ADR-0012, ADR-0017)를 GPU에서 읽는 고리. 읽기 칸들은 렌더 스레드만 만지고, 가장 최근 값(Latest)은 Mutex로
 * 게임 스레드와 나눈다. 칸마다 번호(Serial, 0 = 빈 칸)를 붙여 늦게 끝난 옛 값이 새 값을 덮지 않게 한다.
 */
struct FSoftbodyLumpReadback
{
	static constexpr int32 NumSlots = 4;
	/** 묶음 × 결과 값(float). */
	static constexpr int32 NumWords = USoftbodySurfaceField::NumLumpGroups * USoftbodySurfaceField::NumLumpOutputs;

	TUniquePtr<FRHIGPUBufferReadback> Readbacks[NumSlots];
	uint64 Serial[NumSlots] = {};
	uint64 NextSerial = 1;

	mutable FCriticalSection Mutex;
	TArray<double> Latest;
	uint64 LatestSerial = 0;

	/** 끝난 읽기를 모두 거둔다(렌더 스레드). */
	void Poll()
	{
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			if (Serial[Slot] == 0 || !Readbacks[Slot] || !Readbacks[Slot]->IsReady())
			{
				continue;
			}
			const float* Words = static_cast<const float*>(Readbacks[Slot]->Lock(NumWords * sizeof(float)));
			TArray<double> Values;
			if (Words)
			{
				Values.SetNumUninitialized(NumWords);
				for (int32 Index = 0; Index < NumWords; ++Index)
				{
					Values[Index] = (double)Words[Index];
				}
			}
			Readbacks[Slot]->Unlock();
			if (Values.Num() > 0)
			{
				FScopeLock Lock(&Mutex);
				if (Serial[Slot] > LatestSerial)
				{
					Latest = MoveTemp(Values);
					LatestSerial = Serial[Slot];
				}
			}
			Serial[Slot] = 0;
		}
	}

	/** 빈 읽기 칸(렌더 스레드). 모두 쓰는 중이면 nullptr — 이번 프레임은 읽지 않는다. */
	FRHIGPUBufferReadback* Acquire()
	{
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			if (Serial[Slot] == 0)
			{
				if (!Readbacks[Slot])
				{
					Readbacks[Slot] = MakeUnique<FRHIGPUBufferReadback>(TEXT("SoftbodyLumpSolution"));
				}
				Serial[Slot] = NextSerial++;
				return Readbacks[Slot].Get();
			}
		}
		return nullptr;
	}
};

/**
 * 디버그 캡처(Softbody.CaptureField)의 접촉 압력 풀이 읽기 칸 — 캡처를 요청한 동안만 있다. 렌더 스레드만 만진다: 사본을 뜨는 갱신이 풀이 버퍼를 읽기
 * 칸으로 복사하고(Queue), 저장할 때 렌더 명령이 GPU를 기다려 CPU 배열로 옮긴다(Resolve — 디버그 저장이라 기다림을 받아들인다).
 */
struct FSoftbodyPressureCapture
{
	enum EBuffer : int32 { ReadHeader, ReadCells, ReadForce, ReadStats, ReadHardWork, ReadHardHand, ReadFRow, ReadPieceMotion, NumReads };

	TUniquePtr<FRHIGPUBufferReadback> Readbacks[NumReads];
	/** 버퍼마다 복사한 바이트 수(0 = 그 갱신에 없던 버퍼). */
	uint32 Bytes[NumReads] = {};
	/** 그 갱신의 조각 표와 Flex Offset(FSoftbodyPressureCaptureData로 넘긴다). */
	TSharedPtr<const FSoftbodyHardPieceTable, ESPMode::ThreadSafe> HardTable;
	float FlexOffset = 0.f;

	/** 버퍼 앞 NumBytes를 읽기 칸으로 복사하는 패스를 넣는다(렌더 스레드, 사본을 뜨는 갱신만). 읽기 칸은 처음 쓸 때 만든다. */
	void Queue(FRDGBuilder& GraphBuilder, EBuffer Index, FRDGBufferRef Buffer, uint32 NumBytes)
	{
		if (!Buffer || NumBytes == 0)
		{
			return;
		}
		if (!Readbacks[Index])
		{
			Readbacks[Index] = MakeUnique<FRHIGPUBufferReadback>(TEXT("SoftbodyPressureCapture"));
		}
		AddEnqueueCopyPass(GraphBuilder, Readbacks[Index].Get(), Buffer, NumBytes);
		Bytes[Index] = NumBytes;
	}

	/** 복사가 끝나기를 기다려 CPU 배열로 옮긴다(렌더 스레드). 복사하지 않은 버퍼는 빈 배열로 둔다. */
	void Resolve(FRHICommandListImmediate& RHICmdList, FSoftbodyPressureCaptureData& Out)
	{
		bool bAny = false;
		for (int32 Index = 0; Index < NumReads; ++Index)
		{
			bAny |= Readbacks[Index].IsValid() && Bytes[Index] > 0;
		}
		if (!bAny)
		{
			return;
		}
		// 앞서 넣은 복사를 GPU에 내고 다 끝나기를 기다린다
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		Read(RHICmdList, ReadHeader, Out.Header);
		Read(RHICmdList, ReadCells, Out.Cells);
		Read(RHICmdList, ReadForce, Out.Force);
		Read(RHICmdList, ReadStats, Out.Stats);
		Read(RHICmdList, ReadHardWork, Out.HardWork);
		Read(RHICmdList, ReadHardHand, Out.HardHand);
		Read(RHICmdList, ReadFRow, Out.FRow);
		Read(RHICmdList, ReadPieceMotion, Out.PieceMotion);
		Out.HardTable = HardTable;
		Out.FlexOffset = FlexOffset;
	}

	template <typename T>
	void Read(FRHICommandListImmediate& RHICmdList, EBuffer Index, TArray<T>& Out)
	{
		FRHIGPUBufferReadback* Readback = Readbacks[Index].Get();
		const uint32 NumBytes = Bytes[Index];
		if (!Readback || NumBytes < sizeof(T))
		{
			return;
		}
		Readback->Wait(RHICmdList, Readback->GetLastCopyGPUMask());
		if (const void* Data = Readback->Lock(NumBytes))
		{
			Out.SetNumUninitialized((int32)(NumBytes / sizeof(T)));
			FMemory::Memcpy(Out.GetData(), Data, Out.Num() * sizeof(T));
			Readback->Unlock();
		}
	}
};

namespace
{
	constexpr int32 ThreadGroupSize = 8;
	/** 복원이 이 비율 아래로 내려가면 갱신을 멈춘다. */
	constexpr double SettleRatio = 1e-3;
	/** 거의 줄지 않는 값(틱마다 e^{-1e-4}배보다 느림)은 기다릴 것이 없다: 이 틱 수가 지나면 복원이 끝난 것으로 본다. */
	constexpr double SettleTicksWithoutRecovery = 600.0;
	/** GPU 쉬기(셰이더 ActivityArgsCS)가 복원이 끝난 정도를 세는 정수 눈금: SB_SETTLED = 다 끝남, SB_RESTED = 세기를 멈추는 값. */
	constexpr uint32 SettledProgress = 0x1000000u;
	constexpr uint32 RestedProgress = 0x7fffffffu;
	/**
	 * 간접 디스패치 인자(ActivityArgsCS, 셰이더 SB_ARGS_*): 0 = 접촉 묶음 목록 패스(복사·합치기·노멀), 1 = 덩어리 힘 묶음 목록,
	 * 2 = 비우기(전체 해상도), 3 = 묶음 목록 만들기(스레드 묶음 하나, 어디든 눌렸거나 상태가 남았을 때만). 함몰·끌림·솟음 내리기의
	 * 인자는 따로 둔다(TileListCS가 목록 길이로 채운다).
	 */
	constexpr uint32 ArgsListed = 0;
	constexpr uint32 ArgsLump = 1;
	constexpr uint32 ArgsClear = 2;
	constexpr uint32 ArgsTiles = 3;
	constexpr uint32 NumArgs = 4;
	/**
	 * 목록 패스(TileListCS)가 목록 길이로 채우는 인자(셰이더 SB_LIST_ARGS_*): 0 = 함몰(함몰 묶음마다 하나), 1 = 옮기기(함몰·비울 묶음마다 하나),
	 * 2 = 함몰 거친 해(거친 묶음마다 하나, 일지 199). 접촉 압력 칸 패스(ADR-0037)는 함몰과 같은 0번을 쓴다. 3·4 = 부위 나누기 장의 함몰을 두 순열로
	 * 나눈 몫(목록 앞쪽 부위 묶음은 채널 순열, 나머지는 채널 없는 순열).
	 */
	constexpr uint32 ListArgsFine = 0;
	constexpr uint32 ListArgsCopy = 1;
	constexpr uint32 ListArgsCoarse = 2;
	constexpr uint32 ListArgsParts = 3;
	constexpr uint32 ListArgsSingle = 4;
	constexpr uint32 NumListArgs = 5;
	/** 끌림 원천 머리의 낱말 수(셰이더 SB_SOURCE_HEADER_WORDS). */
	constexpr uint32 SourceHeaderWords = 12;
	/**
	 * 접촉 패스 상태 버퍼(Activity) 앞머리 낱말 수(셰이더 SB_PRESSER_TABLE_BASE): [0] 눌린 칸이 있다, [1] 살 두께보다 깊이 눌린 표면 칸 수,
	 * [2] 가장 깊이 넘은 양(셰이더 SB_ACTIVITY_DEEP, 일지 295), [3] 들어온 면 표지 자리가 모자란 칸 수(셰이더 SB_ACTIVITY_LABEL_OVERFLOW). 그 뒤가 누른 것 표다.
	 */
	constexpr uint32 SoftbodySurfaceActivityHeaderWords = 4;
	/** 누른 것 표의 누른 것 × 덩어리 묶음마다 낱말 수(셰이더 SB_PRESSER_WORDS): 칸 수, 가로 크기, 미는 방향 합 셋(일지 338). */
	constexpr uint32 PresserWords = 5;
	// 접촉 압력(ADR-0037, 셰이더 SB_PRESSURE_*)
	/** 한 단계의 압력 칸 상한(셰이더 SB_PRESSURE_CAP). 손 330칸, 몸 크기 누름 720~830칸(거친 칸 0.64 cm) — 넘으면 2×2·4×4로 묶은 단계를 쓴다. */
	constexpr uint32 PressureCapacity = 768;
	/** 압력 칸 하나의 uint4 줄 수(셰이더 SB_PRESSURE_ROWS). */
	constexpr uint32 PressureRows = 13;    // 셰이더 SB_PRESSURE_ROWS: 일곱 줄 + 칸 재료의 성분 양 다섯(반정밀) + 칸 안 대각 하나(일지 289)
	/** 묶음 단계 수(셰이더 SB_PRESSURE_LEVELS): 거친 칸, 2×2, 4×4. */
	constexpr uint32 PressureLevels = 3;
	/** 압력 머리의 낱말 수(셰이더 SB_PRESSURE_HEADER_WORDS). */
	constexpr uint32 PressureHeaderWords = 16;
	// 하드 서피스 압력 단계(ADR-0037 §3.7, 셰이더 SB_HARD_*) — 조각이 구워진 장만 쓴다.
	/** 후보 조각 상한(셰이더 SB_HARD_CANDIDATES): 닿은 조각 + 압력 칸 상자 가까이, 가까운 순. */
	constexpr uint32 HardCandidates = 32;
	/** 압력 칸 패스가 내보내는 조각 위 손 칸 줄 상한(셰이더 SB_HARD_HAND_EMIT) — 풀이는 이 가운데 깊은 256줄(SB_HARD_HAND_CAP)만 받는다. */
	constexpr uint32 HardHandCapacity = 512;
	/** 조각 위 손 칸 한 줄의 float4 수(셰이더 SB_HARD_HAND_ROWS): 머리 + 가는 칸 표본 16. */
	constexpr uint32 HardHandRows = 17;
	/** 일 버퍼 낱말 수(셰이더 SB_HARD_WORK_WORDS). 발자국 줄은 조각 밖 압력 칸과 함께 PressureCapacity 안에 든다. */
	constexpr uint32 HardWorkWords = 232;
	/** 활성 집합 바퀴 예산과 바퀴마다 CG 걸음(일지 275 — 4 × 4면 3-D 잣대 35경우 모두 통과, 2 × 8부터 깨짐). */
	constexpr uint32 PressureRounds = 6;
	constexpr uint32 PressureSteps = 4;
	/** 긴 몫 모으기 반경 ÷ 쌍 두께(일지 275 — 5면 얇은 살 센 겉껍질 꼭대기 −20 %). */
	constexpr float PressureReachLayers = 6.f;
	/** 정규화·모으기 패스의 거친 칸 묶음 한 변(셰이더 SB_COARSE_GROUP, 끌림 모으기도 같다). */
	constexpr uint32 CoarseGroup = 8;
	// 끌림 원천 커널 합(ADR-0033, 셰이더 SB_DRAG_*)
	/**
	 * 한 갱신의 끌림 원천 목록 칸 수. 원천은 끄는 힘이 남은 거친 칸 하나씩: 손 하나 10~300개, 몸을 눕힌 넓은 누름이 수천 개(시험 캐릭터 모의 2천 개 안팎,
	 * 일지 177; 시연 앉는 방석 3천 개, 일지 186). 목록은 거친 8×8 묶음째 이어서 놓이고 넘치면 그 묶음의 원천이 모두 빠진다 — 그만큼 덜 끌린다.
	 */
	constexpr uint32 DragSourceCapacity = 16384;
	/** 원천 하나의 uint4 줄 수(셰이더 SB_DRAG_SOURCE_ROWS). */
	constexpr uint32 DragSourceRows = 7;
	/**
	 * 묶음 원천(원천이 있는 거친 8×8 묶음마다 원천들을 뭉친 것, 일지 187) 하나의 uint4 줄 수(셰이더 SB_DRAG_CLUSTER_ROWS). 끄는 힘 몫은 따로 float4 두 줄
	 * (DragClusterQ — 원천 패스와 바로잡기가 번갈아 쓴다). 칸 수 = 거친 격자의 8×8 묶음 수라 넘치지 않는다(시험 캐릭터 256² 1,024개 — 147 KB + 32 KB × 2).
	 */
	constexpr uint32 DragClusterRows = 9;
	constexpr uint32 DragClusterQRows = 2;
	/**
	 * 끌림 바로잡기 패스 수(일지 183, 기본 1 — 프로파일링 스위치 softbody.Drag.CorrectPasses): 붙은 칸의 커널 합을 붙든 자리에 맞추는 걸음. 끄는 힘은 지난
	 * 갱신에서 이어 오므로 걸음이 갱신마다 쌓인다 — 0이면 초당 20갱신에서 멈춘 누름이 1초 뒤에도 0.8 mm 어긋났고, 1이면 초당 60·20갱신 모두 0.001 mm 안
	 * (시험 캐릭터 가슴 모의). 2는 움직이는 동안만 조금 낫다.
	 */
	constexpr int32 DragCorrectPassesMax = 4;
	/**
	 * 끌림 원천 머리의 낱말 수. 배치는 솟음 머리와 같다(셰이더 SB_BULGE_HEADER_*): [0] 원천 수, [1~3] 상자 최소, [4~6] 상자 최대, [7] 자르는 거리 최대
	 * (SB_DRAG_HEADER_REACH), [8] 넘침, [9] 끄는 힘이 1 µm 이상인 원천 수(SB_DRAG_HEADER_LIVE, 일지 179 — 0이면 모으기가 원천을 올리지 않는다), [10] 묶음
	 * 원천 수(SB_DRAG_HEADER_CLUSTERS, 일지 187), [11] 빈 자리.
	 */
	constexpr uint32 DragHeaderWords = 12;
	/**
	 * 묶음 수 버퍼의 낱말 수(셰이더 SB_TILE_COUNT_WORDS): [0] 함몰 묶음 수, [1] 비울 묶음 수, [2] 끌림이 닿은 거친 묶음 수(stat), [3..15] 끌림 걸음 집계
	 * (셰이더 SB_DRAG_STAT_*, 일지 189 — 섞인 묶음 원천 수, 바로잡기·모으기마다 묶음 수·걸음 최대·합·가까운 묶음 올림 최대·가까운·먼 걸음 최대),
	 * [16] 멈춤 표지(일지 203 — 눌림·변위 바뀜 비트, 갱신마다 읽기 칸에 복사한다), [17] 함몰 목록 앞쪽 부위 묶음 수, [18..23] Softbody.Stats 목록
	 * 까닭별 수(셰이더 SB_TILE_COUNT_PARTS·SB_TILE_COUNT_WHY 주석). 멈춤 읽기는 [16]까지만 복사한다.
	 */
	constexpr uint32 TileCountWords = 24;
	static_assert(TileCountWords > FSoftbodySurfaceRestState::StillWordIndex, "the still readback copies the tile-count words up to the still word");
	/** 멈춤 표지 비트(셰이더 SB_STILL_PRESSED·SB_STILL_CHANGED). */
	constexpr uint32 StillPressed = 1u;
	constexpr uint32 StillChanged = 2u;
	/** 함몰 묶음 한 변의 칸 수(셰이더 SB_TILE). */
	constexpr uint32 TileSize = 16;
	/**
	 * 함몰 묶음 목록을 붙든 묶음 둘레 몇 겹까지 넓힐지(ADR-0032). 정적 해는 붙든 칸에서 함몰 폭 ℓ로 잦아드니 둘레 한 겹(16칸) 밖은 1 µm 아래거나
	 * 잘라도 기울기 꺾임이 0.04~0.43°다(시험 캐릭터, 2겹과 차 0.023 mm 아래, 일지 172). 반복 수로 넓히던 2겹(일지 172 이전)과 결과는 같고 묶음은
	 * 1/3이다.
	 */
	constexpr int32 DentTileReach = 1;
	/**
	 * 함몰 거친 해(일지 199)의 거친 패스 수 = 두꺼운 칸의 함몰 폭(거친 칸 수) ÷ 이 값, 1~DentCoarsePassesMax. 모의(판 9.2·좁은 함몰): 폭 1.7칸은 1번,
	 * 16칸은 2번이면 누르고 떼는 동안 노멀 차 0.9° 아래, 32칸은 4번이 필요했다(2번이면 5.6°).
	 */
	constexpr double DentCoarseCellsPerPass = 8.0;
	constexpr int32 DentCoarsePassesMax = 4;
	/** 함몰 패스 하나의 체비쇼프 반복 수(셰이더 SB_DENT_ITERATIONS). */
	constexpr int32 DentIterations = 32;
	/**
	 * 묶음마다 고스트 칸이 가리키는 묶음을 적는 칸 수(셰이더 SB_TILE_LINKS). 넘치는 묶음은 목록 패스가 돌 때마다(어디든 눌렸거나 상태가 남았을 때)
	 * 그 둘레까지 늘 켠다. 측정 최대(1024², DumpCarrierTileLinks): 시험 캐릭터 12, 다른 시험 캐릭터 여섯 17~31,
	 * 옷 입은 시험 캐릭터 21. 512² 캐리어는 시험 캐릭터 20이었다. 16일 때는 다른 시험 캐릭터의 넘친 묶음
	 * (한 캐릭터 194개)과 그 둘레가 누를 때마다 목록에 들어(691~1,053개) 작은 누름에서는 그것이 목록의 대부분이었다.
	 * 32면 잰 11개 자산 모두 넘치지 않는다. 쌍 버퍼는 장마다 묶음 수 × 32 × 4 B(1024²에서 512 KB)이고 캐리어가 바뀔 때만 만든다.
	 */
	constexpr uint32 TileLinksPerTile = 32;

	uint32 ArgsOffset(uint32 Slot)
	{
		return Slot * sizeof(FRHIDispatchIndirectParameters);
	}

	/**
	 * 걸음 하나가 e^{-StepDecay}배로 줄이는 값이 SettleRatio까지 가는 길의 얼마를 가는가(1 = 다 감). 로그로 세므로 걸음 크기가
	 * 섞여도 더하면 된다. TickDecay = 틱 하나의 같은 값.
	 */
	double SettleProgress(double TickDecay, double StepDecay, double StepTicks)
	{
		return TickDecay < 1e-4 ? StepTicks / SettleTicksWithoutRecovery : StepDecay / -FMath::Loge(SettleRatio);
	}

	/**
	 * 걸음 하나(StepTime 초)가 콜라이더가 떠난 뒤의 복원을 얼마나 끝내는가(더해서 1이면 남은 함몰이 SettleRatio 아래다). 눌린 자리가
	 * 다 돌아왔는지를 틱 수나 갱신 수가 아니라 이 값으로 센다: 미룬 갱신(ADR-0021)은 걸음이 커서 한 번에 더 많이 줄이고, 미룬
	 * 틱은 아무것도 줄이지 않는다.
	 *  - 함몰: 이완의 암시적 한 걸음마다 e^{−복원율 × 걸음 시간}배가 된다(가장 느린 모양인 고른 함몰, ADR-0019).
	 *  - 받아들인 깊이(ADR-0003 §14): 틱마다 AcceptReturn배.
	 * 둘 다 줄어야 끝이므로 느린 쪽을 센다.
	 */
	double SettleOfStep(const FSoftbodySurfaceFieldInput& Input, double TickTime, double StepTime)
	{
		const double StepTicks = TickTime > 1e-6 ? StepTime / TickTime : 1.0;
		const double Rate = FMath::Max((double)Input.RecoveryRate, 0.0);
		double Progress = SettleProgress(Rate * TickTime, Rate * StepTime, StepTicks);
		if (Input.AcceptBlend > 0.f)
		{
			const double AcceptTick = -FMath::Loge((double)FMath::Clamp(Input.AcceptReturn, 1e-6f, 1.f));
			Progress = FMath::Min(Progress, SettleProgress(AcceptTick, AcceptTick * StepTicks, StepTicks));
		}
		return FMath::Clamp(Progress, 0.0, 1.0);
	}

	// 끌림 거친 격자(일지 161, ADR-0033) = 솟음 거친 격자(굽기가 거리표와 같이 정한 SoftbodyBulgeTable::BlockFor).
	/** 끌림 입력 버퍼의 거친 칸마다 줄 수(셰이더 SB_DRAG_INPUT_ROWS). */
	constexpr uint32 DragInputRows = 3;
	/** 끌림 거친 격자 한 변의 최대 칸 수: 이웃 표(DragLinks)의 칸 번호가 20비트다. 거친 칸이 0.5 cm를 넘으므로 2048² 캐리어도 10 m가 넘어야 넘는다. */
	constexpr uint32 MaxDragResolution = 1024;

	/**
	 * 함몰 정적 해의 체비쇼프(Saad 12.1) 고윳값 범위 아래끝: 두꺼운 살(두께 상위 10%)의 폭 Reach(cm)가 칸 크기 Cell(cm)에서 만드는
	 * 1/(1 + 4·Reach²/칸²)와 반복 Iterations번으로 줄일 수 있는 값(8/N²) 가운데 큰 쪽(ADR-0019·ADR-0032). Reach를 모르면(0) 8/N².
	 */
	double StaticChebyshevLowFor(double Reach, double Cell, int32 Iterations)
	{
		const double ThickLow = Reach > 0.0 ? 1.0 / (1.0 + Reach * Reach * 4.0 / FMath::Square(Cell)) : 0.0;
		return FMath::Clamp(FMath::Max(ThickLow, 8.0 / FMath::Square((double)Iterations)), 1e-6, 0.5);
	}

	/**
	 * 캐리어 표면 칸 살 두께의 두꺼운 쪽(90번째 백분위, 메시 로컬 단위). 구울 때 잰 값이 없으면(그 전에 구운 캐리어) 에디터에서는
	 * 노멀 텍스처 원본의 w(두께)에서 잰다. 둘 다 없으면 0.
	 */
	float ThicknessHighOf(const USoftbodyMeshData* Data)
	{
		if (Data->CarrierThicknessHigh > 0.f)
		{
			return Data->CarrierThicknessHigh;
		}
#if WITH_EDITORONLY_DATA
		FImage Image;
		UTexture2D* NormalTex = Data->CarrierNormalTex.Get();
		if (NormalTex && NormalTex->Source.IsValid() && NormalTex->Source.GetMipImage(Image, 0))
		{
			Image.ChangeFormat(ERawImageFormat::RGBA32F, EGammaSpace::Linear);
			TArray<float> Thickness;
			for (const FLinearColor& Texel : Image.AsRGBA32F())
			{
				if (Texel.A > 0.f)
				{
					Thickness.Add(Texel.A);
				}
			}
			if (Thickness.Num() > 0)
			{
				Thickness.Sort();
				return Thickness[Thickness.Num() * 9 / 10];
			}
		}
#endif
		return 0.f;
	}

	/**
	 * 하드 서피스(ADR-0037 §3.7, 셰이더 SB_HARD_SURFACE): 1 = 이 장의 자산에 단단한 조각이 구워져 있다. 조각 코드는 이 순열에만 있다 — 조각이 없는
	 * 자산은 0(지금과 같은 셰이더)을 돌아 레지스터·읽기·디스패치 어느 것도 늘지 않는다(비용 분리, 원칙 E06). 순열 1의 파이프라인은 시작 때 전역으로
	 * 데우지 않고 조각이 있는 자산의 장이 켜질 때(ReservePool) 데운다(USoftbodySurfaceField::AreHardPipelinesReady).
	 */
	class FSoftbodyHardSurfaceDim : SHADER_PERMUTATION_BOOL("SB_HARD_SURFACE");

	class FSoftbodySurfaceContactCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierSkinIndex)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierSkinWeight)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Shapes)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Bones)
			// 손 닻(일지 336): 지난 갱신 자세 — 피부가 본을 따라 움직인 거리를 뺀다(모르면 Bones와 같은 값).
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PreviousBones)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Planes)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FieldVoxels)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FieldInstances)
			SHADER_PARAMETER(uint32, NumShapes)
			SHADER_PARAMETER(uint32, NumFieldInstances)
			SHADER_PARAMETER(uint32, bSkinned)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(FVector3f, ComponentScale)
			SHADER_PARAMETER(float, MaxContactDepth)
			SHADER_PARAMETER(float, EdgeTrimWidth)
			SHADER_PARAMETER(uint32, bContactLongRoom)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierColumn)
			SHADER_PARAMETER(uint32, bCarrierColumn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ContactOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, EdgeOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, PositionOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ShapeMotion)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FieldMotion)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, StatePrevious)
			SHADER_PARAMETER(float, DragRatio)
			SHADER_PARAMETER(float, LumpStiffen)
			SHADER_PARAMETER(uint32, bMotionHistory)
			// 손 닻(일지 201): 읽기만(쓰는 것은 덩어리 힘 패스 첫 걸음) — 이 패스의 UAV는 8개로 D3D11 한도에 차 있다
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, LumpAnchor)
			// 끌림 거친 격자(일지 161)
			SHADER_PARAMETER(uint32, DragBlock)
			SHADER_PARAMETER(uint32, DragResolution)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint4>, DragLinks)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DragState)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, DragInputOut)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierVolume)
			SHADER_PARAMETER(uint32, bCarrierVolume)
			SHADER_PARAMETER(float, VolumeValue)
			SHADER_PARAMETER(float, VolumeOffset)
			SHADER_PARAMETER(float, SkinLength)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLumpSideA)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLumpSideB)
			SHADER_PARAMETER(uint32, bLumpSide)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InertiaBridge)
			SHADER_PARAMETER(uint32, LumpMode)
			SHADER_PARAMETER(FVector4f, ComponentRotation)
			SHADER_PARAMETER(FVector3f, FrameToActor)
			// 덩어리 국소 모양(일지 251): 국소 모양 패스가 거친 칸마다 쓴 옮긴 양을 광선 출발점에 넣는다(읽기만).
			SHADER_PARAMETER(uint32, bLumpRegion)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, LumpRegionOffset)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER(float, ThicknessScale)
			// 덩어리 칸 접촉 값(셰이더 SB_PackLumpDepth — 반정밀 여섯 값 + 미는 방향, 일지 338).
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint4>, LumpDepthOut)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, LumpNormalOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ActivityOut)
			// 이완 묶음 눌림 표지는 ActivityOut의 TilePressedBase부터(쓰기 슬롯 8개 가운데 하나를 들어온 면 표지에 내주려고 합쳤다 — 일지 313).
			SHADER_PARAMETER(uint32, TilePressedBase)
			SHADER_PARAMETER(uint32, TilesPerSide)
			// 들어온 면 표지(일지 313): 갱신을 넘어 남는 표지(칸마다 제자리에서 읽고 쓴다)·콜라이더 고정 번호.
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ContactLabels)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ColliderIds)
			SHADER_PARAMETER(uint32, bLabels)
			SHADER_PARAMETER(uint32, bLabelHistory)
			SHADER_PARAMETER(uint32, bSweep)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
			// 하드 서피스(순열 SB_HARD_SURFACE만 읽는다): 떠 있는 조각 밑 칸의 접촉 시작점 올림(G16 × HardLiftScale, 메시 로컬).
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierLift)
			SHADER_PARAMETER(float, HardLiftScale)
			SHADER_PARAMETER(uint32, bCarrierLift)
			// 잡기(ADR-0041): 이번 갱신 입력과 지난 갱신까지의 줄(읽기만 — 쓰기는 잡기 풀기 패스). 고르기는 ActivityOut의 GrabPickBase부터(쓰기 자원 8개 그대로).
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabInput)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabState)
			SHADER_PARAMETER(uint32, GrabCount)
			SHADER_PARAMETER(uint32, GrabPickBase)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			// 끌림은 지난 프레임 피부 자리를 PositionOut에 새 값을 쓰기 전에 그 텍스처에서 읽는다. 쓰기로 묶은 네 채널 텍스처를 읽는 것은
			// D3D11에서 GPU가 따로 지원해야 하는 기능이라 엔진이 기본으로 막는다. 지원하지 않는 GPU에서는 표면 장을 만들지 않는다
			// (USoftbodySurfaceField::IsSupported).
			OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		}
	};

	/**
	 * 덩어리 평형의 크기(일지 254, 셰이더 SB_LUMP_REGION): 1 = 국소 모양 16개를 같이 푸는 판(미지수 19), 0 = 출렁임 모양만 푸는 판(미지수 3 —
	 * 칸 합 값 210 → 10, 풀이 19×19 → 3×3). 국소 모양이 구워져 켜진 장(bLumpRegion)만 큰 판을 쓴다 — 국소 모양이 없는 자산은 그 비용을 내지
	 * 않는다. 두 판 모두 미리 컴파일 목록에 든다(순열마다 — 켬·끔이나 품질 단계로 바뀌어도 끊기지 않게).
	 */
	class FSoftbodyLumpRegionDim : SHADER_PERMUTATION_BOOL("SB_LUMP_REGION");

	/**
	 * 부위 나누기(Separated Parts, 셰이더 "부위 나누기"): 캐리어에 부위 몫이 구워진 장만 함몰이 채널 셋을 같은 반복에서 같이 풀고(공유 메모리 32 KB 한
	 * 덩이), 압력 칸·긴 몫 모으기가 채널 몫을 모은다. 공유 메모리·장벽 꼴이 바뀌는 패스만 순열로 가른다 — 부위 나누기가 없는 장은 전과 같은 셰이더를 돈다.
	 * 두 판 모두 미리 컴파일 목록에 든다(켜고 끌 때 끊기지 않게).
	 */
	class FSoftbodySeparationDim : SHADER_PERMUTATION_BOOL("SB_SEPARATION");

	/** 덩어리 평형(ADR-0012, ADR-0017): 지금 반복 값에서 칸마다 에너지 기울기·헤세·에너지 몫을 덩어리 묶음별로 더한다. */
	class FSoftbodySurfaceLumpForceCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceLumpForceCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceLumpForceCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyLumpRegionDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLumpSideA)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLumpSideB)
			SHADER_PARAMETER(uint32, bLumpSide)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint4>, LumpDepthIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PositionIn)
			// 누른 것 표(일지 248 ②): 접촉 패스의 쉬기 버퍼 뒤쪽. 칸 넓이로 닿은 크기를 재려고 칸 크기, 누른 것 번호를 풀려고 형상 수.
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, PresserTable)
			SHADER_PARAMETER(uint32, NumShapes)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(float, DragRatio)
			SHADER_PARAMETER(float, LumpGrip)
			SHADER_PARAMETER(float, LumpStiffen)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpIterate)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpSumsOut)
			// 손 닻(일지 201): 첫 걸음은 LumpAnchorOut에 쓰고(In은 한 칸짜리), 둘째 걸음부터 LumpAnchorIn에서 읽는다(Out은 한 칸짜리). UAV 2개.
			SHADER_PARAMETER(uint32, bFirstStep)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, LumpAnchorIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, LumpAnchorOut)
			// 덩어리 힘 묶음(일지 251): 이번 프레임 손 밑 누름·쥐기 칸이 있는 묶음만(LumpActiveCS가 프레임에 한 번 고른다).
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LumpActiveTiles)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LumpActiveCount)
			// 덩어리 국소 모양(일지 251): 칸마다 모양 16개를 거친 칸 줄에서 쌍선형으로 읽는다.
			SHADER_PARAMETER(uint32, bLumpRegion)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LumpRegionIndex)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, LumpRegionTable)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 덩어리 힘 묶음 고르기(일지 251): 덩어리 칸이 든 묶음 가운데 이번 프레임 손 밑 누름·쥐기 칸이 있는 묶음만 — 덩어리 힘 패스가 걸음마다
	 * 이것만 돈다. 누름·쥐기는 접촉 패스가 정해 걸음 사이에 바뀌지 않는다.
	 */
	class FSoftbodySurfaceLumpActiveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceLumpActiveCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceLumpActiveCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER(uint32, bLumpSide)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint4>, LumpDepthIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PositionIn)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, DragRatio)
			SHADER_PARAMETER(float, LumpGrip)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LumpList)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LumpListCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpActiveTilesOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpActiveCountOut)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 덩어리 평형(ADR-0017): 프레임마다 반복 시작점 — 지난 프레임 결과에서 관성 목표가 움직인 만큼 옮겨 이어 푼다. 덩어리 힘 패스의 간접 인자도 쓴다. */
	class FSoftbodySurfaceLumpStartCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceLumpStartCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceLumpStartCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyLumpRegionDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpGroups)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, LumpState)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpSumsOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LumpActiveCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpForceArgsOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 덩어리 국소 모양(일지 251): 거친 칸마다 국소 모양이 지금 옮긴 양(접촉 패스의 광선 출발점·합치기의 변위가 읽는다). */
	class FSoftbodySurfaceLumpRegionCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceLumpRegionCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceLumpRegionCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LumpRegionIndex)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, LumpRegionTable)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpIterate)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InertiaBridge)
			SHADER_PARAMETER(FVector4f, ComponentRotation)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, LumpRegionOffsetOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 덩어리 평형(ADR-0017): 칸 합으로 뉴턴 한 걸음(에너지가 늘면 반 되돌림). 마지막 걸음은 결과를 적는다. 덩어리 묶음마다 스레드 묶음 하나. */
	class FSoftbodySurfaceLumpSolveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceLumpSolveCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceLumpSolveCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyLumpRegionDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, LumpGroups)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, LumpState)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpSumsOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, LumpOut)
			SHADER_PARAMETER(uint32, bLastStep)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 옮기기(ADR-0032): 함몰 둘째 패스 결과를 상태 A로(고스트 칸은 가리키는 점의 값), 목록에서 빠지는 묶음은 0으로.
	 */
	class FSoftbodySurfaceDentCopyCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDentCopyCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDentCopyCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CarrierKind)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, StateIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, StateOut)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			// 함몰 거친 해(일지 199): 목록 밖 칸을 가리키는 고스트 칸은 거친 해를 옮긴다.
			SHADER_PARAMETER(uint32, DentCoarseOn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarse)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTiles)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileBits)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileResidualOut)
			SHADER_PARAMETER(uint32, TilesPerSide)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 함몰(ADR-0032): 누른 자리 기억에 붙든 살층의 정적 해를 함몰 묶음마다 둘레 8칸 창에서 푼다. 갱신마다 두 번(DentPass 1·2). 부위 나누기(순열)는
	 * 같은 반복에서 채널 셋을 같이 풀어 다른 부위에서 온 꼬리를 뺀 함몰을 상태로 쓴다.
	 */
	class FSoftbodySurfaceDentCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDentCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDentCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodySeparationDim, FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CarrierKind)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierVolume)
			SHADER_PARAMETER(uint32, bCarrierVolume)
			SHADER_PARAMETER(float, VolumeValue)
			SHADER_PARAMETER(float, VolumeOffset)
			SHADER_PARAMETER(float, SkinLength)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PositionIn)
			// 함몰 거친 해(일지 199): 목록 밖은 거친 해로 읽는다(목록 묶음의 출발 값은 DentCoarseAddCS가 상태 A에 넣어 둔다 — 일지 200).
			SHADER_PARAMETER(uint32, DentCoarseOn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarse)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, StateIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, StateOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, HoldState)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, DentPins)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DentPinsIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, AcceptedOut)
			SHADER_PARAMETER(float, AcceptBlend)
			SHADER_PARAMETER(float, AcceptReturn)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			// 접촉 압력(ADR-0037): 붙든 칸 목표 P + L − γ·h·A₁·p의 긴 몫·거친 압력(거친 칸 RG)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, ContactLong)
			SHADER_PARAMETER(uint32, bContactLong)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, ContactBend)   // 연속 굽힘: 굽힘 거친 해 W_c(Flex 순열만 읽는다)
			SHADER_PARAMETER(uint32, DentPass)
			SHADER_PARAMETER(float, HoldDecay)
			SHADER_PARAMETER(float, DentTheta)
			SHADER_PARAMETER(float, DentDelta)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTiles)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileBits)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileResidualOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileAnyResidualOut)
			SHADER_PARAMETER(uint32, TilesPerSide)
			// 부위 나누기: 칸 몫, 긴 몫 읽기의 솟음 채널, 첫 패스가 쓰고 둘째 패스가 읽는 함몰 채널(순열)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSeparation)
			SHADER_PARAMETER(uint32, bSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactLongSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SeparationStateIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SeparationStateOut)
			// 부위 나누기 장: 목록의 어느 몫을 도는가(0 전체, 1 앞쪽 부위 묶음 — 채널 순열, 2 나머지 — 채널 없는 순열)
			SHADER_PARAMETER(uint32, DentListPart)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
			// 하드 서피스(순열 SB_HARD_SURFACE만 읽는다, ADR-0037 §3.7): 칸 조각 번호, 조각 틀·움직임(켜짐).
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierPiece)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceMotion)
			SHADER_PARAMETER(uint32, HardPieceCount)
			// 가는 창 판 용수철(SB_HardSpringAt)·연속 굽힘(SB_BendSpringAt): 칸 Flex 칠(묶였는가), 굽힘 길이 눈금 L0, Flex Offset
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSoftness)
			SHADER_PARAMETER(uint32, bCarrierSoftness)
			SHADER_PARAMETER(float, HardRefThickness)
			SHADER_PARAMETER(float, FlexOffset)
			// 잡기(ADR-0041): 원 안 칸 용수철 줄·원 둘레 창 바닥 없음(SB_GrabGrip·SB_GrabNear — 원은 정지 자리 × 스케일로 잰다).
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabState)
			SHADER_PARAMETER(uint32, GrabCount)
			SHADER_PARAMETER(FVector3f, ComponentScale)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 함몰 거친 해(일지 199·200): 첫 패스 앞, 함몰을 푸는 가는 묶음의 상태 A를 출발 값으로(지난 갱신에 푼 묶음 A + 쌍선형 고침, 새로 든 묶음 거친 해).
	 */
	class FSoftbodySurfaceDentCoarseAddCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDentCoarseAddCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDentCoarseAddCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarse)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarseFix)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DentStateInOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTiles)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileResidualOut)
			SHADER_PARAMETER(uint32, TilesPerSide)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 함몰 거친 해(일지 199): 거친 칸마다 출발 값(지난 가는 해 평균 또는 지난 거친 해)과 아래 한계(붙든 칸이 있으면). 거친 묶음 목록만 돈다.
	 */
	class FSoftbodySurfaceDentCoarseRestrictCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDentCoarseRestrictCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDentCoarseRestrictCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CarrierKind)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, HoldState)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, AcceptedOut)
			SHADER_PARAMETER(float, HoldDecay)
			// 접촉 압력(ADR-0037): 바닥은 가는 창과 같은 목표(SB_DentTarget)로 — 긴 몫·거친 압력·칸 재료를 읽는다.
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierVolume)
			SHADER_PARAMETER(uint32, bCarrierVolume)
			SHADER_PARAMETER(float, VolumeValue)
			SHADER_PARAMETER(float, VolumeOffset)
			SHADER_PARAMETER(float, SkinLength)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, ContactLong)
			SHADER_PARAMETER(uint32, bContactLong)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, StateIn)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileBits)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileResidualOut)
			SHADER_PARAMETER(uint32, TilesPerSide)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarse)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentCoarseList)
			SHADER_PARAMETER(uint32, DentCoarseListBase)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DentCoarseStartOut)
			// 부위 나누기: 목표의 긴 몫 읽기
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSeparation)
			SHADER_PARAMETER(uint32, bSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactLongSeparation)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
			// 하드 서피스(순열 SB_HARD_SURFACE만 읽는다, ADR-0037 §3.7): 칸 조각 번호, 조각 틀·움직임(켜짐).
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierPiece)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceMotion)
			SHADER_PARAMETER(uint32, HardPieceCount)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			// 가는 창 판 용수철(SB_HardSpringAt)·연속 굽힘(SB_BendSpringAt): 칸 Flex 칠(묶였는가), 굽힘 길이 눈금 L0, Flex Offset
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSoftness)
			SHADER_PARAMETER(uint32, bCarrierSoftness)
			SHADER_PARAMETER(float, HardRefThickness)
			SHADER_PARAMETER(float, FlexOffset)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 함몰 거친 해(일지 199) 거친 패스 한 번: 거친 묶음마다 32×32 거친 칸 창에서 같은 함몰 식을 체비쇼프로(이음매는 거친 칸 링크, 열린 끝은 자유단).
	 * 마지막 패스가 거친 해·고침·남은 표지를 쓴다.
	 */
	class FSoftbodySurfaceDentCoarseCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDentCoarseCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDentCoarseCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(float, CoarseTexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(uint32, DragResolution)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint4>, DragLinks)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragInput)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseLayer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentCoarseList)
			SHADER_PARAMETER(uint32, DentCoarseListBase)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DentCoarseStart)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarseIn)
			// 연속 굽힘(Flex 순열만 읽는다): 거친 칸 흐린 굽힘 길이, 긴 몫 L, 굽힘 거친 해 W_c — 굽는 거친 칸 용수철의 목표 W_c + L
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoarseBend)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, ContactLong)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, ContactBend)
			SHADER_PARAMETER(uint32, bContactLong)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DentCoarseOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DentCoarseFixOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DentCoarseBusyOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileAnyResidualOut)
			SHADER_PARAMETER(uint32, DentCoarseFirst)
			SHADER_PARAMETER(uint32, DentCoarseLast)
			SHADER_PARAMETER(float, DentCoarseTheta)
			SHADER_PARAMETER(float, DentCoarseHalf)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 칸 종류 표(R8, 아래 2비트 칸 종류, 비트 2 고스트 띠 바깥 고리 — 셰이더 CarrierKind 주석): 캐리어가 바뀔 때만 돈다. */
	class FSoftbodySurfaceCarrierKindCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceCarrierKindCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceCarrierKindCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CarrierKindOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 솟음(ADR-0011·ADR-0031): 처음 한 번 거친 칸마다 살 칸 정지 위치 평균·넓이·두께·삼각형 변(캐리어로만 정해지는 값)을 만든다. */
	class FSoftbodySurfaceCoarseInitCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceCoarseInitCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceCoarseInitCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			// 부피 보존 칠(ADR-0024): 함몰 거친 해의 칸마다 함몰 폭(CoarseLayer.z, 일지 199).
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierVolume)
			SHADER_PARAMETER(uint32, bCarrierVolume)
			SHADER_PARAMETER(float, VolumeValue)
			SHADER_PARAMETER(float, VolumeOffset)
			SHADER_PARAMETER(float, SkinLength)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, CoarseInfoOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, CoarseLayerOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CoarseSkinOut)
			// 부위 나누기: 거친 칸마다 두 부위 몫 평균(캐리어에 구워져 있을 때만 — 없으면 한 칸짜리 자리 UAV)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSeparation)
			SHADER_PARAMETER(uint32, bCarrierSeparation)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, CoarseSeparationOut)
			// 연속 굽힘(Flex 칠): 거친 칸마다 겉 굽힘 길이 평균(흐리기 전). 칸 칠 + Flex Offset, 강체 조각 칸은 0(조각 틀 셋째 줄 w로 종류를 본다).
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSoftness)
			SHADER_PARAMETER(uint32, bCarrierSoftness)
			SHADER_PARAMETER(float, FlexOffset)
			SHADER_PARAMETER(float, HardRefThickness)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierPiece)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER(uint32, HardPieceCount)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CoarseBendRawOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		// 칸 굽힘 길이(SB_BendLengthAt)가 조각 읽기(하드 서피스 정의 안)를 쓴다 — 순열 차원이 아니라 이 패스만 늘 켠다(처음 한 번 도는 패스라 값싸다,
		// 전역 파이프라인 미리 만들기에 그대로 든다).
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("SB_HARD_SURFACE"), 1);
		}
	};

	/** 접촉 압력(ADR-0037)·끌림(ADR-0033): 처음 한 번 살 있는 거친 칸에 거리표 줄 번호를 매긴다(스레드 묶음 하나, 굽기와 같은 줄 우선 순서). */
	class FSoftbodySurfaceCoarseRowCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceCoarseRowCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceCoarseRowCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CoarseRowOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 접촉 압력(ADR-0037 §3.2): 처음 한 번 거친 칸 재료를 흐린다(부피 보존은 0.5h 폭, 겉껍질 길이는 제 칸 가는 창 폭의 0.5배, 일지 281). */
	class FSoftbodySurfaceCoarseMaterialCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceCoarseMaterialCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceCoarseMaterialCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseLayer)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoarseSkin)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, CoarseMaterialOut)
			// 연속 굽힘: 거친 칸 겉 굽힘 길이(흐리기 전 → 부피 보존과 같은 0.5h 폭으로 흐린 값)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoarseBendRaw)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CoarseBendOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CoarseBendMaxOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 끌림 거친 격자(일지 161): 처음 한 번 거친 칸마다 이웃 표를 만든다(이음매는 건너편 두 칸 사이 보간, 일지 162). */
	class FSoftbodySurfaceDragLinksCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDragLinksCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDragLinksCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			// 이음매 링크의 줄 거르기(일지 217): 가리키는 점의 정지 위치가 거친 칸 4개(메시 로컬)보다 멀면 버린다.
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint4>, DragLinksOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 접촉 압력(ADR-0037 §3.1 ①): 함몰 묶음 목록의 가는 묶음마다 거친 칸의 붙든 칸을 모아 압력 칸 목록에 넣는다(단계 0·1·2 — 거친 칸, 2×2, 4×4).
	 */
	class FSoftbodySurfaceContactPressureCellsCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactPressureCellsCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactPressureCellsCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodySeparationDim, FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, HoldState)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, AcceptedOut)
			SHADER_PARAMETER(float, HoldDecay)
			SHADER_PARAMETER(float, PlasticShare)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CoarseRow)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CoarseMaterial)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LandmarkDistance)
			SHADER_PARAMETER(uint32, NumLandmarks)
			SHADER_PARAMETER(uint32, LandmarkRowTexels)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTiles)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureHeaderOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, PressureCellsOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, PressureSlotOut)
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseAmounts)   // 압력 칸마다 재료의 성분 양(일지 289)
			// 연속 굽힘(Flex 순열만 읽는다): 굽힘 성분 양 표와 거친 칸 흐린 겉 굽힘 길이
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseBendAmounts)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoarseBend)
			// 부위 나누기(순열): 압력 칸마다 붙든 칸의 두 부위 몫 평균
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSeparation)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector2f>, PressureSeparationOut)
			// 하드 서피스(순열): 조각 위 붙든 칸은 압력 칸 대신 조각 위 손 칸 줄로
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierPiece)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER(uint32, HardPieceCount)
			SHADER_PARAMETER(float, FlexOffset)   // 조각 종류(SB_HardKindOf): 회색 조각 칸은 판 줄이 아니라 보통 압력 칸(연속 굽힘)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, HardWorkOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, HardHandOut)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
			// 잡기(ADR-0041): 원 안 칸은 잡기 줄(부호 자유, 대각에 손끝 무름)로 모은다.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabState)
			SHADER_PARAMETER(uint32, GrabCount)
			SHADER_PARAMETER(FVector3f, ComponentScale)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 접촉 압력: 칸 수가 상한 안인 가장 가는 단계를 고르고 행렬·풀이 간접 인자를 쓴다(스레드 하나). */
	class FSoftbodySurfaceContactPressureArgsCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactPressureArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactPressureArgsCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureHeaderOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureArgsOut)
			// 하드 서피스(순열): 후보 조각 고르기, 발자국 줄·모드 줄 자리, 펼치기 인자
			SHADER_PARAMETER(float, PressureReachLayers)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER(float, FlexOffset)   // 조각 종류(SB_HardKindOf): 강체(크기 비로 단단한 붙은 조각·떠 있는 장식)만 후보다
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector2>, HardPieceCellRange)
			SHADER_PARAMETER(uint32, HardPieceCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, HardWorkOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, HardArgsOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 접촉 압력 ②: 압력 칸 쌍마다 처짐/힘 K(i, j)(32 × 32 쌍 묶음). */
	class FSoftbodySurfaceContactPressureMatrixCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactPressureMatrixCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactPressureMatrixCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, PressureHeader)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, PressureCells)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureMatrixOut)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseLong)
			SHADER_PARAMETER_SAMPLER(SamplerState, ContactResponseSampler)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 하드 서피스 펼치기(ADR-0037 §3.7·ADR-0038 §4, 새 디스패치 — 조각이 구워진 장만, 셰이더 SB_HARD_SURFACE 1): 후보 조각마다 묶음 하나가 발자국 거친 칸을
	 * 압력 칸 목록의 조각 밖 칸 뒤에 보통 줄로 쓰고, 풀이가 읽는 판 자리를 발자국 줄마다 쓴다.
	 */
	class FSoftbodySurfaceContactPressurePiecesCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactPressurePiecesCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactPressurePiecesCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CoarseRow)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CoarseMaterial)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LandmarkDistance)
			SHADER_PARAMETER(uint32, NumLandmarks)
			SHADER_PARAMETER(uint32, LandmarkRowTexels)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierPiece)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER(uint32, HardPieceCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HardWork)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector2>, HardPieceCellRange)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HardPieceCellList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureHeaderOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, PressureCellsOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardCellPlate)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, FRowOut)
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseAmounts)   // 발자국 줄마다 재료의 성분 양(일지 289)
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseBendAmounts)   // 강체 판 밑 살은 굽힘 0이라 읽지 않는다(같은 함수라 묶어 둔다)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("SB_HARD_SURFACE"), 1);
		}
	};

	/** 접촉 압력 ③: 한 스레드 묶음이 활성 집합 켤레 기울기로 접촉 힘을 푼다(0에서 — 상태 없음). */
	class FSoftbodySurfaceContactPressureSolveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactPressureSolveCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactPressureSolveCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, PressureHeader)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, PressureCells)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, PressureMatrix)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PressureForceOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureStatsOut)
			SHADER_PARAMETER(uint32, PressureRounds)
			SHADER_PARAMETER(uint32, PressureSteps)
			// 하드 서피스(순열, ADR-0038 §4): 조각 위 손 칸 줄·발자국 줄, 꼬리(조각 켜짐·발자국 압력)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HardWork)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardHand)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector2>, HardPieceCellRange)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HardPieceCellList)
			// 조각 종류(SB_HardKindOf): 조각 틀 셋째 줄 w와 Flex Offset
			SHADER_PARAMETER(float, FlexOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FRow)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, HardPieceMotionOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, TiedPressureOut)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 접촉 압력 ④: 거친 칸마다 긴 몫 L과 거친 압력 p를 ContactLong(RG)에 쓴다(모든 칸). */
	class FSoftbodySurfaceContactLongGatherCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactLongGatherCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactLongGatherCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodySeparationDim, FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, PressureHeader)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, PressureCells)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, PressureForce)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, PressureSlot)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseLayer)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CoarseRow)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CoarseMaterial)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LandmarkDistance)
			SHADER_PARAMETER(uint32, NumLandmarks)
			SHADER_PARAMETER(uint32, LandmarkRowTexels)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileBits)
			SHADER_PARAMETER(uint32, TilesPerSide)
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseLong)
			SHADER_PARAMETER_SAMPLER(SamplerState, ContactResponseSampler)
			SHADER_PARAMETER(float, PressureReachLayers)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, ContactLongOut)
			// 부위 나누기(순열): 출처 무게별 솟음 채널 셋
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector2f>, PressureSeparation)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ContactLongSeparationOut)
			// 하드 서피스(순열): 발자국 줄은 압력 칸 배열의 조각 밖 칸 다음 자리(HardWork가 그 경계), 발자국 거친 압력
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HardWork)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, TiedPressure)
			// 연속 굽힘(Flex 순열): 굽힘 전체 커널 표, 거친 칸 흐린 굽힘 길이와 그 최대, 굽힘 거친 해 W_c 출력
			SHADER_PARAMETER_TEXTURE(Texture3D<float>, ContactResponseBendFull)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoarseBend)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CoarseBendMax)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ContactBendOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 끌림(ADR-0033): 거친 칸마다 붙든 자리·끄는 힘을 갱신하고 원천 목록을 거친 8×8 묶음째 이어서 만들며, 묶음마다 원천들을 뭉친 묶음 원천을 쓴다(일지 187). */
	class FSoftbodySurfaceDragSourceCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDragSourceCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDragSourceCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(float, LumpStiffen)
			SHADER_PARAMETER(float, HoldDecay)
			SHADER_PARAMETER(uint32, DragResolution)
			SHADER_PARAMETER(float, DragTexelSize)
			SHADER_PARAMETER(float, DragReachMax)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint4>, DragLinks)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DragState)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DragHold)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragInput)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CoarseRow)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LandmarkDistance)
			SHADER_PARAMETER(uint32, NumLandmarks)
			SHADER_PARAMETER(uint32, LandmarkRowTexels)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DragHoldNextOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, DragSourcesOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, DragSourceQOut)
			SHADER_PARAMETER(uint32, DragSourceCapacity)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DragHeaderOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, DragClustersOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, DragClusterQOut)
			SHADER_PARAMETER(uint32, DragClusterCapacity)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			// 부위 나누기: 원천마다 거친 칸의 두 부위 몫
			SHADER_PARAMETER(uint32, bSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CoarseSeparation)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector2f>, DragSourceSeparationOut)
			// 잡기(ADR-0041): 원 안 거친 칸은 붙든 칸(붙든 자리 = 당김의 접선 몫, 힘 한도 없음).
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabState)
			SHADER_PARAMETER(uint32, GrabCount)
			SHADER_PARAMETER(FVector3f, ComponentScale)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 끌림 걸음 나누기(일지 191, 셰이더 SB_DRAG_SLICES): 바로잡기·모으기 묶음 하나의 조각 수(스레드 64 × 조각, 디스패치 묶음 수는 그대로). 1이면 일지
	 * 190까지와 비트까지 같다. softbody.Drag.Slices가 고른다.
	 */
	class FSoftbodyDragSlicesDim : SHADER_PERMUTATION_SPARSE_INT("SB_DRAG_SLICES", 1, 2, 4);

	/**
	 * 끌림 바로잡기(ADR-0033, 일지 183): 같은 커널 합으로 끄는 힘을 붙든 자리 쪽으로 한 걸음 더 고친다(거친 격자 전체, 지난 차가 남은 붙은 칸만 셈 — 일지 184).
	 * 먼 묶음은 뭉친 원천으로 세고, 끝에 묶음 원천의 끄는 힘 몫을 고친 Q로 다시 모은다(일지 187).
	 */
	class FSoftbodySurfaceDragCorrectCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDragCorrectCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDragCorrectCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyDragSlicesDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(uint32, DragResolution)
			SHADER_PARAMETER(float, DragTexelSize)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragInput)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CoarseRow)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LandmarkDistance)
			SHADER_PARAMETER(uint32, NumLandmarks)
			SHADER_PARAMETER(uint32, LandmarkRowTexels)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, DragSources)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragSourceQ)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, DragSourceQOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, DragClusters)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragClusterQ)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, DragClusterQOut)
			SHADER_PARAMETER(uint32, DragClusterCapacity)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DragHeader)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DragHoldNext)
			// 부위 나누기: 원천·받는 칸의 두 부위 몫
			SHADER_PARAMETER(uint32, bSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CoarseSeparation)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector2f>, DragSourceSeparation)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/**
	 * 끌림(ADR-0033): 거친 8×8 묶음마다 원천 커널 합으로 끌림·덮인 몫을 모으고 붙든 자리·끄는 힘을 DragHold 한 장에 묶어 옮긴다(거친 격자 전체). 먼 묶음은
	 * 뭉친 원천으로 센다(일지 187).
	 */
	class FSoftbodySurfaceDragGatherCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceDragGatherCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceDragGatherCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyDragSlicesDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(uint32, DragResolution)
			SHADER_PARAMETER(float, DragTexelSize)
			SHADER_PARAMETER(float, DragReachMax)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragInput)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CoarseRow)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, LandmarkDistance)
			SHADER_PARAMETER(uint32, NumLandmarks)
			SHADER_PARAMETER(uint32, LandmarkRowTexels)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, DragSources)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragSourceQ)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, DragClusters)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DragClusterQ)
			SHADER_PARAMETER(uint32, DragClusterCapacity)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DragHeader)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DragHoldNext)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DragHoldOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DragStateOut)
			// 부위 나누기: 원천·받는 칸의 두 부위 몫
			SHADER_PARAMETER(uint32, bSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, CoarseSeparation)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector2f>, DragSourceSeparation)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FSoftbodySurfaceComposeCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceComposeCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceComposeCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierVolume)
			SHADER_PARAMETER(uint32, bCarrierVolume)
			SHADER_PARAMETER(float, VolumeValue)
			SHADER_PARAMETER(float, VolumeOffset)
			SHADER_PARAMETER(float, SkinLength)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, EdgeIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, StateIn)
			// 덩어리 국소 모양(일지 251): 접촉 패스가 광선 출발점에 넣은 것과 같은 옮긴 양을 변위로 그린다.
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER(uint32, bLumpRegion)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, LumpRegionOffset)
			// 함몰 거친 해(일지 199): 목록 밖 칸은 거친 해를 3차 B-스플라인으로.
			SHADER_PARAMETER(uint32, DentCoarseOn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarse)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileBits)
			SHADER_PARAMETER(uint32, TilesPerSide)
			// 끌림 거친 격자(일지 161)
			SHADER_PARAMETER(uint32, DragBlock)
			SHADER_PARAMETER(uint32, DragResolution)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint4>, DragLinks)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DragState)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			// 접촉 압력(ADR-0037): 긴 몫 L(거친 칸 RG — 값을 지나는 3차로 읽는다)과 붙든 칸 붙들 깊이(함몰 첫 패스 — 붙든 칸은 손 깊이에 선다).
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, ContactLong)
			SHADER_PARAMETER(uint32, bContactLong)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DentPinsIn)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DisplacementOut)
			// 멈춤 표지(일지 203): 지난 갱신 변위와 견줘 바뀐 묶음이 있으면 TileCount[16]에 적는다.
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PrevDisplacementIn)
			SHADER_PARAMETER(float, StillChange)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			// 부위 나누기: 긴 몫 읽기의 솟음 채널(함몰은 상태 A가 이미 나눈 값이다)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSeparation)
			SHADER_PARAMETER(uint32, bSeparation)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactLongSeparation)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
			// 하드 서피스(순열 SB_HARD_SURFACE만 읽는다, ADR-0037 §3.7): 칸 조각 번호, 조각 틀·움직임(켜짐).
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierPiece)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceMotion)
			SHADER_PARAMETER(uint32, HardPieceCount)
			// 가는 창 판 용수철(SB_HardSpringAt)·연속 굽힘(SB_BendSpringAt): 칸 Flex 칠(묶였는가), 굽힘 길이 눈금 L0, Flex Offset
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSoftness)
			SHADER_PARAMETER(uint32, bCarrierSoftness)
			SHADER_PARAMETER(float, HardRefThickness)
			SHADER_PARAMETER(float, FlexOffset)
			// 잡기(ADR-0041): 원 둘레는 솟음 한도 없이 U − L(SB_GrabNear).
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabState)
			SHADER_PARAMETER(uint32, GrabCount)
			SHADER_PARAMETER(FVector3f, ComponentScale)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FSoftbodySurfaceNormalCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceNormalCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceNormalCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PositionIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DisplacementIn)
			// 솟음을 얹기 전 함몰: 어둡기를 잴 칸을 고른다(일지 99).
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, StateIn)
			// 함몰 거친 해(일지 199): 어둡기를 재는 칸 = 합치기와 같은 함몰(목록 밖은 거친 해).
			SHADER_PARAMETER(uint32, DentCoarseOn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DentCoarse)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileBits)
			SHADER_PARAMETER(uint32, TilesPerSide)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(float, TexelSize)
			SHADER_PARAMETER(float, OcclusionStrength)
			// 화면 노멀을 접촉 패스와 같은 스킨으로 옮긴다(ADR-0007 §3.6, 콜라이더 면에 붙은 칸에서만).
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierSkinIndex)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierSkinWeight)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Bones)
			SHADER_PARAMETER(uint32, bSkinned)
			SHADER_PARAMETER(FVector3f, ComponentScale)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, NormalOut)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, LumpNormalIn)
			SHADER_PARAMETER(uint32, LumpMode)
			// 콜라이더 면에 선 칸(일지 299): 합치기가 붙들 깊이에 세운 칸을 같은 기록으로 읽는다 — 함몰 첫 패스의 붙들 깊이, 목록이 섰는지, 국소 모양 몫.
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DentPinsIn)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentTileCount)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER(uint32, bLumpRegion)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, LumpRegionOffset)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 쉬기(ADR-0013): 눌린 칸이 있었는지로 뒤 패스들의 간접 디스패치 인자를 쓴다(스레드 하나). */
	/**
	 * 잡기 줄 풀기(ADR-0041, 접촉 패스 뒤 스레드 묶음 하나): 새 잡기의 잡은 칸·시작 간격, 붙든 잡기의 당긴 벡터·한도, 놓는 중 줄의 감쇠를 GrabState에.
	 * 잡기가 있는 갱신만 돈다.
	 */
	class FSoftbodySurfaceGrabResolveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceGrabResolveCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceGrabResolveCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GrabInput)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, GrabStateOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, GrabPicks)
			SHADER_PARAMETER(uint32, GrabCount)
			SHADER_PARAMETER(uint32, GrabPickBase)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierNormal)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierSkinIndex)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierSkinWeight)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Bones)
			SHADER_PARAMETER(uint32, bSkinned)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PositionIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactIn)
			SHADER_PARAMETER(FVector3f, ComponentScale)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(float, ThicknessScale)
			SHADER_PARAMETER(float, HoldDecay)
			SHADER_PARAMETER(uint32, bContactLong)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierVolume)
			SHADER_PARAMETER(uint32, bCarrierVolume)
			SHADER_PARAMETER(float, VolumeValue)
			SHADER_PARAMETER(float, VolumeOffset)
			SHADER_PARAMETER(float, SkinLength)
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, GrabFlexPaint)
			SHADER_PARAMETER(uint32, bGrabFlexPaint)
			SHADER_PARAMETER(float, GrabFlexOffset)
			// 덩어리 국소 모양이 잡은 칸을 옮긴 양(피부 자리에 더한다 — 합치기가 그리는 몫)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER(uint32, bLumpRegion)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, LumpRegionOffset)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CoarseInfo)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER(float, TexelSize)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FSoftbodySurfaceActivityArgsCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceActivityArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceActivityArgsCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ActivityIn)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileAnyResidualOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RestFrames)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ArgsOut)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LumpListCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, PressureHeaderOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DragHeaderOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileListArgsOut)
			SHADER_PARAMETER(uint32, FullGroups)
			SHADER_PARAMETER(uint32, SettleStep)
			SHADER_PARAMETER(uint32, bResetRest)
			SHADER_PARAMETER(uint32, ArgsStride)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 지난 변위 보관(접촉 묶음 목록만, 쉬는 동안은 건너뛴다). */
	/** 고스트 칸 베끼기 모으기(일지 329): 고스트 칸마다 가리키는 점의 최종 변위를 머티리얼 벽 규칙으로 읽어 임시 텍스처에. */
	class FSoftbodySurfaceGhostGatherCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceGhostGatherCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceGhostGatherCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CarrierKind)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DisplacementIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, GhostDisplacementOut)
			SHADER_PARAMETER(uint32, bWallGhost)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 고스트 칸 베끼기 덮어쓰기(일지 329): 고스트 칸만 모은 값으로, 멈춤 표지도 이 값으로. */
	class FSoftbodySurfaceGhostApplyCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceGhostApplyCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceGhostApplyCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, CarrierKind)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, GhostDisplacementIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DisplacementOut)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PrevDisplacementIn)
			SHADER_PARAMETER(float, StillChange)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FSoftbodySurfaceCopyCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceCopyCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceCopyCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, CopyIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, CopyOut)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroups)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ContactGroupCount)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 쉬기 시작 프레임에 이완 상태·받아들인 깊이·변위·노멀을 0으로. */
	class FSoftbodySurfaceClearCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceClearCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceClearCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ClearStateA)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ClearDisplacement)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ClearPreviousDisplacement)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ClearNormal)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ClearAccepted)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ClearHold)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ClearDrag)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ClearDragHold)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(uint32, DragResolution)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 이완 묶음(ADR-0013 2단계): 처음 한 번, 묶음마다 고스트 칸이 값을 가져오는 다른 묶음을 이음새 쌍으로 적는다. */
	class FSoftbodySurfaceTileLinksCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceTileLinksCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceTileLinksCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierRedirect)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER(uint32, TilesPerSide)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileLinkPairsOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileLinkPairCountOut)
			// 부위 나누기: 묶음마다 부위 몫이 함몰 채널에 닿는지 1비트.
			SHADER_PARAMETER_TEXTURE(Texture2D<float>, CarrierSeparation)
			SHADER_PARAMETER(uint32, bCarrierSeparation)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TilePartsOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 이완 묶음 목록과 솟음 영역(ADR-0013 §8): 스레드 묶음 하나가 묶음 비트를 넓혀 두 목록을 만든다. */
	class FSoftbodySurfaceTileListCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceTileListCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceTileListCS, FGlobalShader);
		using FPermutationDomain = TShaderPermutationDomain<FSoftbodyHardSurfaceDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TilePressed)
			SHADER_PARAMETER(uint32, TilePressedBase)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileResidual)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileLinkPairs)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileLinkPairCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileCountOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileListOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileBitsOut)
			SHADER_PARAMETER(uint32, TilesPerSide)
			SHADER_PARAMETER(int32, TileReach)
			SHADER_PARAMETER(uint32, ArgsStride)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileListArgsOut)
			// 부위 나누기 장인가(함몰 순열), Softbody.Stats가 읽는 갱신인가(목록 까닭별 수를 TileCountOut에 센다).
			SHADER_PARAMETER(uint32, bDentParts)
			SHADER_PARAMETER(uint32, bTileStats)
			// 부위 나누기: 묶음마다 부위 몫이 닿는지(TileLinksCS) — 함몰 목록을 채널 순열 몫과 나머지로 가른다.
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileParts)
			// 함몰 거친 해(일지 199): 거친 묶음 목록을 같이 만든다 — TileListOut 뒤쪽 DentCoarseListBase부터(쓰기 버퍼를 늘리지 않는다, ADR-0025).
			SHADER_PARAMETER(uint32, DentCoarseOn)
			SHADER_PARAMETER(uint32, CoarseResolution)
			SHADER_PARAMETER(uint32, CoarseBlock)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DentCoarseBusy)
			SHADER_PARAMETER(uint32, DentCoarseListBase)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
			// 하드 서피스(순열 SB_HARD_SURFACE만 읽는다, ADR-0037 §3.7): 조각마다 함몰 묶음 목록.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector2>, HardPieceTileRange)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HardPieceTileList)
			SHADER_PARAMETER(uint32, HardPieceCount)
			// 조각 종류(SB_HardKindOf — 회색 조각은 판이 아니라 묶음을 통째로 넣지 않는다, 연속 굽힘)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, HardPieceFrames)
			SHADER_PARAMETER(float, FlexOffset)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** 접촉 묶음(ADR-0013 3단계): 처음 한 번, 표면·고스트 칸이 든 8×8 묶음 목록과 접촉 패스 간접 디스패치 인자를 만든다. */
	class FSoftbodySurfaceContactGroupsCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceContactGroupsCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceContactGroupsCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierPosition)
			SHADER_PARAMETER_TEXTURE(Texture2D<float4>, CarrierLump)
			SHADER_PARAMETER(uint32, Resolution)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ContactGroupsOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ContactGroupCountOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ContactArgsOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpListOut)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, LumpListCountOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	/** stat softbody: 표면 장 GPU 상태를 한 버퍼에 모은다(스레드 하나, 통계를 보는 중일 때만). */
	class FSoftbodySurfaceStatsCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSoftbodySurfaceStatsCS);
		SHADER_USE_PARAMETER_STRUCT(FSoftbodySurfaceStatsCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StatActivity)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, StatRest)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StatTileCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StatContactGroupCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StatDragHeader)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, StatOut)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceLumpActiveCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "LumpActiveCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceLumpForceCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "LumpForceCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceLumpStartCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "LumpStartCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceLumpSolveCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "LumpSolveCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceLumpRegionCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "LumpRegionCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDentCopyCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DentCopyCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDentCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DentCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDentCoarseRestrictCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DentCoarseRestrictCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDentCoarseCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DentCoarseCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDentCoarseAddCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DentCoarseAddCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceCarrierKindCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "CarrierKindCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceCoarseInitCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "CoarseInitCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceCoarseRowCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "CoarseRowCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceCoarseMaterialCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "CoarseMaterialCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDragLinksCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DragLinksCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactPressureCellsCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactPressureCellsCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactPressureArgsCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactPressureArgsCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactPressureMatrixCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactPressureMatrixCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactPressurePiecesCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactPressurePiecesCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactPressureSolveCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactPressureSolveCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactLongGatherCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactLongGatherCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDragSourceCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DragSourceCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDragCorrectCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DragCorrectCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceDragGatherCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "DragGatherCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceComposeCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ComposeCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceGhostGatherCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "GhostGatherCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceGhostApplyCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "GhostApplyCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceNormalCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "NormalCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceActivityArgsCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ActivityArgsCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceGrabResolveCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "GrabResolveCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceCopyCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "CopyCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceClearCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ClearCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceTileLinksCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "TileLinksCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceTileListCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "TileListCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceContactGroupsCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "ContactGroupsCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FSoftbodySurfaceStatsCS, "/Plugin/SoftbodySystem/SoftbodySurfaceField.usf", "StatsCS", SF_Compute);

	/** 렌더 타깃 한 장의 GPU 바이트(끊김 시험의 누계). */
	int64 FieldTargetBytes(const UTextureRenderTarget2D* Target)
	{
		return Target ? (int64)Target->SizeX * (int64)Target->SizeY * (int64)GPixelFormats[Target->GetFormat()].BlockBytes : 0;
	}

	UTextureRenderTarget2D* CreateFieldTarget(UObject* Outer, int32 Resolution, const TCHAR* Name,
		ETextureRenderTargetFormat Format = RTF_RGBA16f)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyCreateFieldTarget);
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Outer,
			MakeUniqueObjectName(Outer, UTextureRenderTarget2D::StaticClass(), Name), RF_Transient);
		Target->RenderTargetFormat = Format;
		Target->bCanCreateUAV = true;
		Target->ClearColor = FLinearColor::Transparent;
		Target->Filter = TF_Bilinear;
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;
		Target->bAutoGenerateMips = false;
		Target->InitAutoFormat(Resolution, Resolution);
		Target->UpdateResourceImmediate(true);
		SoftbodyProfiling::FResourceCounters& Counters = SoftbodyProfiling::GetResourceCounters();
		++Counters.TargetsCreated;
		Counters.BytesCreated += FieldTargetBytes(Target);
		return Target;
	}

	/** 렌더 타깃의 GPU 메모리를 GC를 기다리지 않고 놓는다(오브젝트는 GC가 거둔다). */
	void ReleaseFieldTarget(UTextureRenderTarget2D* Target)
	{
		if (!Target)
		{
			return;
		}
		SoftbodyProfiling::FResourceCounters& Counters = SoftbodyProfiling::GetResourceCounters();
		++Counters.TargetsReleased;
		Counters.BytesReleased += FieldTargetBytes(Target);
		Target->ReleaseResource();
	}

	/** 렌더 스레드에서 쓸 베이크 텍스처 리소스 (게임 스레드에서 수집). */
	FTextureResource* ResourceOf(UTexture2D* Texture)
	{
		return Texture ? Texture->GetResource() : nullptr;
	}

	/** 접촉 지도 텍스처(Mesh Data의 약한 참조, 일지 222): 올라와 있을 때만 자원이 있다 — 표면 장은 다 올라온 뒤에만 돈다(IsReady). */
	FTextureResource* ResourceOf(const TSoftObjectPtr<UTexture2D>& Texture)
	{
		return ResourceOf(Texture.Get());
	}

	FRHITexture* RHIOf(FTextureResource* Resource)
	{
		return Resource && Resource->TextureRHI.IsValid() ? Resource->TextureRHI.GetReference() : GBlackTexture->TextureRHI.GetReference();
	}

	/**
	 * 없어도 되는 베이크 텍스처(부피 보존 칠, 제 살 깊이): 없으면 nullptr. 패스들이 이 값이 비었는지로 "칠 없음 = 전부 1"을 고른다.
	 * RHIOf를 쓰면 없는 텍스처가 검은 텍스처로 바뀌어, 칠하지 않은 캐리어가 전부 0으로 읽힌다(눌려도 둘레가 솟지 않고 함몰 둘레가
	 * 폼처럼 넓어지며, 살 속에 묻힌 콜라이더를 찾지 못한다 — 일지 138).
	 */
	FRHITexture* OptionalRHIOf(FTextureResource* Resource)
	{
		return Resource && Resource->TextureRHI.IsValid() ? Resource->TextureRHI.GetReference() : nullptr;
	}
}

bool USoftbodySurfaceField::IsSupported()
{
	static const bool bSupported = []()
	{
		// 접촉 패스의 셰이더는 SM5 이상에서만 컴파일된다. 모바일(ES3.1)·Quest처럼 그 아래에서 돌면 컴파일되지 않은 셰이더로 패스를
		// 올리게 되므로 접촉을 끈다.
		if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
		{
			UE_LOG(LogSoftbodySurfaceField, Warning,
				TEXT("Contact is OFF for every Softbody component: it needs Shader Model 5 (desktop D3D11, D3D12, Vulkan or Metal), "
					"and this renderer runs at a lower feature level (mobile, ES3.1)."));
			return false;
		}
		// 엔진이 GPU에 물어 값을 채워 두는 D3D11·D3D12에서만 본다. 벌컨은 엔진이 이 값을 채우지 않아 늘 "안 됨"으로 읽히고,
		// 렌더링 없는 실행(Null)은 볼 것이 없다.
		const ERHIInterfaceType Type = GDynamicRHI ? RHIGetInterfaceType() : ERHIInterfaceType::Null;
		if (Type != ERHIInterfaceType::D3D11 && Type != ERHIInterfaceType::D3D12)
		{
			return true;
		}
		// 접촉 패스가 읽는 것은 Position 텍스처(RGBA32f)다(FSoftbodySurfaceTextures::Create와 같은 포맷이어야 한다).
		const bool bCanRead = RHIIsTypedUAVLoadSupported(PF_A32B32G32R32F);
		if (bCanRead)
		{
			UE_LOG(LogSoftbodySurfaceField, Log,
				TEXT("Contact is available on this GPU (%s): it can read a four-channel float texture that is bound for writing."),
				GDynamicRHI->GetName());
		}
		else
		{
			UE_LOG(LogSoftbodySurfaceField, Warning,
				TEXT("Contact is OFF for every Softbody component: this GPU or driver (%s) cannot read a four-channel float texture "
					"that is bound for writing (typed UAV load of R32G32B32A32_FLOAT). Inertia still runs. A newer GPU driver may enable it."),
				GDynamicRHI->GetName());
		}
		return bCanRead;
	}();
	return bSupported;
}

namespace
{
	/**
	 * 접촉 파이프라인 준비(일지 235). 드라이버 캐시가 빈 첫 실행에서 접촉 셰이더 하나가 몇 초씩 걸리고, 캐시가 따뜻해도 런타임 PSO는 처음 쓰는 디스패치가
	 * 만들며 RHI 스레드를 기다리게 한다(쿡 빌드 첫 접촉 RHI 29 ms). 그래서 (1) 백그라운드에서 미리 만들고(드라이버 컴파일), (2) 끝나면 런타임 캐시에 프레임마다
	 * 몇 개씩 넣고, (3) 쓰지 않아 캐시에서 빠지지 않게(r.pso.evictiontime) 가끔 다시 찾는다. (2)까지 끝나야 월드 묶음이 접촉 갱신을 올린다.
	 */
	std::atomic<bool> GPipelinesRequested{false};
	std::atomic<int32> GPipelineCount{0};
	std::atomic<int32> GPipelinesWarmed{0};
	std::atomic<bool> GPipelineWarmInFlight{false};
	std::atomic<bool> GPipelinesReady{false};
	FCriticalSection GPipelineEventsLock;
	TArray<FGraphEventRef> GPipelineEvents;
	// 하드 서피스 순열(SB_HARD_SURFACE ≠ 0)의 파이프라인: 같은 세 걸음을 조각이 있는 장이 처음 물을 때 따로 한다(AreHardPipelinesReady, 원칙 E06).
	std::atomic<bool> GHardPipelinesStarted{false};
	std::atomic<bool> GHardPipelinesRequested{false};
	std::atomic<int32> GHardPipelineCount{0};
	std::atomic<int32> GHardPipelinesWarmed{0};
	std::atomic<bool> GHardPipelineWarmInFlight{false};
	std::atomic<bool> GHardPipelinesReady{false};
	TArray<FGraphEventRef> GHardPipelineEvents;   // GPipelineEventsLock으로 지킨다
	double GHardPipelinesFirstAsked = 0.0;
	/** 마지막으로 런타임 캐시를 다시 찾은 때(게임 스레드). */
	double GPipelineKeepAliveTime = 0.0;
	/** 런타임 캐시에 프레임마다 넣는 수 / 다시 찾는 간격(초 — 엔진은 r.pso.evictiontime(기본 60초) 두 번 동안 안 쓴 PSO를 뺀다). */
	constexpr int32 PipelinesWarmedPerFrame = 2;
	constexpr double PipelineKeepAliveSeconds = 20.0;

	/**
	 * 하드 서피스 순열(SB_HARD_SURFACE ≠ 0)인가. 그 순열의 파이프라인은 전역 미리 만들기에서 빼고 조각이 있는 장이 처음 물을 때 따로 데운다(비용 분리 —
	 * 원칙 E06: 조각을 칠하지 않은 프로젝트의 접촉 시작이 늦지 않게). 순열 차원에 하드 서피스가 든 셰이더 형식은 여기에 하나씩 적는다.
	 */
	bool IsHardSurfacePermutation(const FShaderType* Type, int32 Permutation)
	{
		if (Type == &FSoftbodySurfaceContactCS::GetStaticType())
		{
			return FSoftbodySurfaceContactCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceDentCS::GetStaticType())
		{
			return FSoftbodySurfaceDentCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceDentCoarseRestrictCS::GetStaticType())
		{
			return FSoftbodySurfaceDentCoarseRestrictCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceDentCoarseCS::GetStaticType())
		{
			return FSoftbodySurfaceDentCoarseCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceComposeCS::GetStaticType())
		{
			return FSoftbodySurfaceComposeCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceTileListCS::GetStaticType())
		{
			return FSoftbodySurfaceTileListCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceContactPressureCellsCS::GetStaticType())
		{
			return FSoftbodySurfaceContactPressureCellsCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceContactPressureArgsCS::GetStaticType())
		{
			return FSoftbodySurfaceContactPressureArgsCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceContactPressureMatrixCS::GetStaticType())
		{
			return FSoftbodySurfaceContactPressureMatrixCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceContactPressureSolveCS::GetStaticType())
		{
			return FSoftbodySurfaceContactPressureSolveCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		if (Type == &FSoftbodySurfaceContactLongGatherCS::GetStaticType())
		{
			return FSoftbodySurfaceContactLongGatherCS::FPermutationDomain(Permutation).Get<FSoftbodyHardSurfaceDim>();
		}
		// 하드 서피스에만 쓰는 패스(펼치기)는 통째로
		return Type == &FSoftbodySurfaceContactPressurePiecesCS::GetStaticType();
	}

	/**
	 * 파이프라인 하나를 가리키는 값과 그것으로 미리 만들기·런타임 캐시에 넣기. 5.8은 셰이더 + 특수화 상수, 5.6·5.7은 셰이더 하나로 파이프라인이 정해진다.
	 */
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	using FSoftbodyComputePipeline = FRHIComputeShader*;
	FSoftbodyComputePipeline MakeSoftbodyPipeline(const TShaderRef<FShader>&, FRHIComputeShader* RHIShader)
	{
		return RHIShader;
	}
	FPSOPrecacheRequestResult PrecacheSoftbodyPipeline(FSoftbodyComputePipeline Pipeline)
	{
		return PipelineStateCache::PrecacheComputePipelineState(Pipeline, nullptr, /*bForcePrecache*/ true);
	}
	void CreateSoftbodyPipeline(FRHIComputeCommandList& RHICmdList, FSoftbodyComputePipeline Pipeline)
	{
		PipelineStateCache::GetAndOrCreateComputePipelineState(RHICmdList, Pipeline, /*bFromFileCache*/ false);
	}
#else
	using FSoftbodyComputePipeline = FComputePipelineStateInitializer;
	FSoftbodyComputePipeline MakeSoftbodyPipeline(const TShaderRef<FShader>& Shader, FRHIComputeShader* RHIShader)
	{
		return FComputePipelineStateInitializer(RHIShader, Shader.GetSpecializationConstants());
	}
	FPSOPrecacheRequestResult PrecacheSoftbodyPipeline(const FSoftbodyComputePipeline& Pipeline)
	{
		FPSOPrecacheRequestParams Params;
		Params.PrecachePriority = EPSOPrecachePriority::High;
		Params.bForcePrecache = true;
		return PipelineStateCache::PrecacheComputePipelineState(Pipeline, Params);
	}
	void CreateSoftbodyPipeline(FRHIComputeCommandList& RHICmdList, const FSoftbodyComputePipeline& Pipeline)
	{
		PipelineStateCache::GetAndOrCreateComputePipelineState(RHICmdList, Pipeline);
	}
#endif

	/**
	 * 이 플러그인의 컴퓨트 셰이더(순열마다)를 차례로(렌더 스레드). 셰이더가 다시 컴파일될 수 있어(에디터) 매번 셰이더 맵에서 다시 찾는다.
	 * bHard = 하드 서피스 순열만(아니면 그 순열을 뺀 나머지만).
	 */
	template <typename FFunc>
	int32 ForEachSoftbodyComputePipeline(FFunc&& Func, bool bHard = false)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		if (!ShaderMap)
		{
			return 0;
		}
		int32 Index = 0;
		for (TLinkedList<FShaderType*>::TIterator It(FShaderType::GetTypeList()); It; It.Next())
		{
			FGlobalShaderType* Type = It->GetGlobalShaderType();
			if (!Type || Type->GetFrequency() != SF_Compute || !FCString::Strstr(Type->GetShaderFilename(), TEXT("/Plugin/SoftbodySystem/")))
			{
				continue;
			}
			for (int32 Permutation = 0; Permutation < Type->GetPermutationCount(); ++Permutation)
			{
				if (IsHardSurfacePermutation(Type, Permutation) != bHard)
				{
					continue;
				}
				TShaderRef<FShader> Shader = ShaderMap->GetShader(Type, Permutation);
				if (!Shader.IsValid())
				{
					continue;
				}
				FRHIComputeShader* RHIShader = static_cast<FRHIComputeShader*>(Shader.GetRHIShaderBase(SF_Compute, false));
				if (!RHIShader)
				{
					continue;
				}
				Func(Index++, MakeSoftbodyPipeline(Shader, RHIShader));
			}
		}
		return Index;
	}
}

void USoftbodySurfaceField::PrecachePipelines()
{
	static bool bStarted = false;
	if (bStarted || !FApp::CanEverRender() || !IsSupported())
	{
		return;
	}
	bStarted = true;
	ENQUEUE_RENDER_COMMAND(SoftbodyPrecachePipelines)([](FRHICommandListImmediate&)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyPrecachePipelines);
		TArray<FGraphEventRef> Events;
		const int32 Count = ForEachSoftbodyComputePipeline([&Events](int32, const FSoftbodyComputePipeline& Pipeline)
		{
			const FPSOPrecacheRequestResult Result = PrecacheSoftbodyPipeline(Pipeline);
			if (Result.AsyncCompileEvent.IsValid())
			{
				Events.Add(Result.AsyncCompileEvent);
			}
		});
		UE_LOG(LogSoftbodySurfaceField, Log, TEXT("Precaching %d contact compute pipelines (%d compiling in the background)."), Count, Events.Num());
		{
			FScopeLock Lock(&GPipelineEventsLock);
			GPipelineEvents = MoveTemp(Events);
		}
		GPipelineCount.store(Count, std::memory_order_release);
		GPipelinesRequested.store(true, std::memory_order_release);
	});
}

bool USoftbodySurfaceField::ArePipelinesReady()
{
	if (GPipelinesReady.load(std::memory_order_acquire))
	{
		return true;
	}
	if (!GPipelinesRequested.load(std::memory_order_acquire))
	{
		PrecachePipelines();
		return false;
	}
	{
		// 안전장치: 컴파일 이벤트가 어떤 까닭으로 끝나지 않아도 접촉이 영영 꺼진 채 남지 않게, 60초가 지나면 기다리지 않는다(드라이버 캐시가 빈 느린 PC도
		// 보통 그 안에 끝난다 — 넘으면 남은 것은 처음 쓰는 디스패치가 기다린다).
		static const double FirstAsked = FPlatformTime::Seconds();
		const bool bGiveUp = FPlatformTime::Seconds() - FirstAsked > 60.0;
		FScopeLock Lock(&GPipelineEventsLock);
		for (const FGraphEventRef& Event : GPipelineEvents)
		{
			if (Event.IsValid() && !Event->IsComplete() && !bGiveUp)
			{
				return false;
			}
		}
		if (bGiveUp && GPipelineEvents.Num() > 0)
		{
			UE_LOG(LogSoftbodySurfaceField, Warning, TEXT("Contact compute pipelines are still compiling after 60 s - contact starts anyway and may wait for them once."));
		}
		GPipelineEvents.Reset();
	}
	// 드라이버 컴파일이 끝났다: 런타임 캐시에 프레임마다 몇 개씩 넣는다(캐시가 따뜻해 하나에 몇 ms 안쪽, RHI 스레드가 그만큼 기다린다).
	const int32 Count = GPipelineCount.load(std::memory_order_acquire);
	const int32 Warmed = GPipelinesWarmed.load(std::memory_order_acquire);
	if (Warmed >= Count)
	{
		GPipelineKeepAliveTime = FPlatformTime::Seconds();
		GPipelinesReady.store(true, std::memory_order_release);
		UE_LOG(LogSoftbodySurfaceField, Log, TEXT("Contact compute pipelines ready (%d)."), Count);
		return true;
	}
	if (!GPipelineWarmInFlight.exchange(true))
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyWarmPipelines)([Warmed](FRHICommandListImmediate& RHICmdList)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyWarmPipelines);
			ForEachSoftbodyComputePipeline([&RHICmdList, Warmed](int32 Index, const FSoftbodyComputePipeline& Pipeline)
			{
				if (Index >= Warmed && Index < Warmed + PipelinesWarmedPerFrame)
				{
					CreateSoftbodyPipeline(RHICmdList, Pipeline);
				}
			});
			GPipelinesWarmed.store(Warmed + PipelinesWarmedPerFrame, std::memory_order_release);
			GPipelineWarmInFlight.store(false, std::memory_order_release);
		});
	}
	return false;
}

void USoftbodySurfaceField::KeepPipelinesAlive()
{
	if (!GPipelinesReady.load(std::memory_order_acquire))
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now - GPipelineKeepAliveTime < PipelineKeepAliveSeconds)
	{
		return;
	}
	GPipelineKeepAliveTime = Now;
	// 런타임 캐시에서 다시 찾는다 — 오래 쓰지 않은 PSO를 엔진이 빼면 다음 접촉이 다시 만들며 RHI 스레드를 기다린다. 빠졌으면 다시 넣는다.
	// 하드 서피스 순열은 데운 뒤에만(조각이 있는 장이 있었다).
	const bool bHard = GHardPipelinesReady.load(std::memory_order_acquire);
	ENQUEUE_RENDER_COMMAND(SoftbodyKeepPipelines)([bHard](FRHICommandListImmediate& RHICmdList)
	{
		ForEachSoftbodyComputePipeline([&RHICmdList](int32, const FSoftbodyComputePipeline& Pipeline)
		{
			CreateSoftbodyPipeline(RHICmdList, Pipeline);
		});
		if (bHard)
		{
			ForEachSoftbodyComputePipeline([&RHICmdList](int32, const FSoftbodyComputePipeline& Pipeline)
			{
				CreateSoftbodyPipeline(RHICmdList, Pipeline);
			}, true);
		}
	});
}

bool USoftbodySurfaceField::AreHardPipelinesReady()
{
	if (GHardPipelinesReady.load(std::memory_order_acquire))
	{
		return true;
	}
	// 기본 순열이 먼저 준비된다(같은 길, 같은 렌더 명령 차례).
	if (!ArePipelinesReady())
	{
		return false;
	}
	if (!GHardPipelinesStarted.exchange(true))
	{
		GHardPipelinesFirstAsked = FPlatformTime::Seconds();
		ENQUEUE_RENDER_COMMAND(SoftbodyPrecacheHardPipelines)([](FRHICommandListImmediate&)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyPrecacheHardPipelines);
			TArray<FGraphEventRef> Events;
			const int32 Count = ForEachSoftbodyComputePipeline([&Events](int32, const FSoftbodyComputePipeline& Pipeline)
			{
				const FPSOPrecacheRequestResult Result = PrecacheSoftbodyPipeline(Pipeline);
				if (Result.AsyncCompileEvent.IsValid())
				{
					Events.Add(Result.AsyncCompileEvent);
				}
			}, true);
			UE_LOG(LogSoftbodySurfaceField, Log, TEXT("Precaching %d hard surface compute pipelines (%d compiling in the background)."), Count, Events.Num());
			{
				FScopeLock Lock(&GPipelineEventsLock);
				GHardPipelineEvents = MoveTemp(Events);
			}
			GHardPipelineCount.store(Count, std::memory_order_release);
			GHardPipelinesRequested.store(true, std::memory_order_release);
		});
		return false;
	}
	if (!GHardPipelinesRequested.load(std::memory_order_acquire))
	{
		return false;
	}
	{
		// 기본 순열과 같은 안전장치(60초).
		const bool bGiveUp = FPlatformTime::Seconds() - GHardPipelinesFirstAsked > 60.0;
		FScopeLock Lock(&GPipelineEventsLock);
		for (const FGraphEventRef& Event : GHardPipelineEvents)
		{
			if (Event.IsValid() && !Event->IsComplete() && !bGiveUp)
			{
				return false;
			}
		}
		GHardPipelineEvents.Reset();
	}
	const int32 Count = GHardPipelineCount.load(std::memory_order_acquire);
	const int32 Warmed = GHardPipelinesWarmed.load(std::memory_order_acquire);
	if (Warmed >= Count)
	{
		GHardPipelinesReady.store(true, std::memory_order_release);
		UE_LOG(LogSoftbodySurfaceField, Log, TEXT("Hard surface compute pipelines ready (%d)."), Count);
		return true;
	}
	if (!GHardPipelineWarmInFlight.exchange(true))
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyWarmHardPipelines)([Warmed](FRHICommandListImmediate& RHICmdList)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyWarmHardPipelines);
			ForEachSoftbodyComputePipeline([&RHICmdList, Warmed](int32 Index, const FSoftbodyComputePipeline& Pipeline)
			{
				if (Index >= Warmed && Index < Warmed + PipelinesWarmedPerFrame)
				{
					CreateSoftbodyPipeline(RHICmdList, Pipeline);
				}
			}, true);
			GHardPipelinesWarmed.store(Warmed + PipelinesWarmedPerFrame, std::memory_order_release);
			GHardPipelineWarmInFlight.store(false, std::memory_order_release);
		});
	}
	return false;
}

bool USoftbodySurfaceField::UsesHardSurface() const
{
	return Data && Data->HasHardPieces();
}

bool USoftbodySurfaceField::Initialize(USoftbodyMeshData* InData, bool bReservePool)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyFieldInitialize);
	// 옛 플러그인이 구운 캐리어(판 2 미만: 고스트 칸이 가까운 칸 하나를 통째로 따르던 규약)는 받지 않는다 — 접촉을 끄고 한 번만 알린다.
	// HasSurfaceCarrier가 판 2부터만 참이라 아래에서 캐리어 없음으로 돌아간다.
	if (InData && InData->CarrierVersion > 0 && InData->CarrierVersion < 2 && !bCarrierVersionWarned)
	{
		bCarrierVersionWarned = true;
		UE_LOG(LogSoftbodySurfaceField, Warning,
			TEXT("Contact is off for %s: its contact map was baked with an older plugin (format %d, expected 2). Press Rebake Contact Map."),
			*GetPathNameSafe(InData), InData->CarrierVersion);
	}
	if (!InData || !InData->HasSurfaceCarrier() || !IsSupported())
	{
		return false;
	}
	if (Data == InData && (!HasTextures() || StateA->SizeX == InData->CarrierResolution))
	{
		return true;
	}
	// 데이터가 바뀌었으면 쥐고 있던 텍스처는 돌려준다. 텍스처 두 벌은 처음 닿을 때 잡는다(ADR-0022).
	ReleaseTextures();
	UWorld* World = GetWorld();
	USoftbodySurfaceBatch* Batch = World ? World->GetSubsystem<USoftbodySurfaceBatch>() : nullptr;
	if (bContactMapAcquired && Data && Pool.IsValid())
	{
		Pool->ReleaseContactMap(Data);
	}
	bContactMapAcquired = false;
	Data = InData;
	HardTable.Reset();
	Pool = Batch;
	if (!LumpReadback.IsValid())
	{
		LumpReadback = MakeShared<FSoftbodyLumpReadback, ESPMode::ThreadSafe>();
	}
	IdleSettle = 0.0;
	bCleared = true;
	// 접촉 지도(일지 222·235): 캐리어·거리표 텍스처를 올린다. 이미 메모리에 있으면(같은 Mesh Data의 다른 장, 끈 지 얼마 안 된 장, 에디터) 바로 준비되고,
	// 아니면 월드 묶음이 몇 장씩 나눠 비동기로 올린다 — 그동안 IsReady가 거짓이라 접촉 없이 관성만 돈다. 큰 지도를 한 프레임에 올리면 렌더 스레드가 텍스처마다
	// 기다렸다 통째로 복사해 끊긴다(쿡 빌드, 일지 235).
	ContactMapRefs.Reset();
	bContactMapLoaded = false;
	if (ContactMapHandle.IsValid())
	{
		ContactMapHandle->CancelHandle();
		ContactMapHandle.Reset();
	}
	if (Batch)
	{
		Batch->AcquireContactMap(InData);
		bContactMapAcquired = true;
		PollContactMap();
	}
	else
	{
		LoadContactMapDirect();
	}
	// 월드 풀: 게임 월드면 이 캐리어 종류를 쓰는 장으로 적는다 — 풀이 여분을 프레임 예산 안에서 미리 만들어 처음 닿는 순간 기다리지 않게 한다
	// (softbody.Surface.TexturePool). 끈 채 세팅한 컴포넌트는 켤 때 부른다(ReservePool).
	if (bReservePool)
	{
		ReservePool();
	}
	return true;
}

void USoftbodySurfaceField::LoadContactMapDirect()
{
	// 월드 밖(묶음이 없다): 직접 비동기로 올린다.
	TArray<FSoftObjectPath> Paths;
	Data->GetContactMapPaths(Paths);
	if (UAssetManager::IsInitialized())
	{
		ContactMapHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths,
			FStreamableDelegate::CreateWeakLambda(this, [this]() { OnContactMapLoaded(); }), FStreamableManager::AsyncLoadHighPriority);
	}
	else
	{
		for (const FSoftObjectPath& Path : Paths)
		{
			Path.TryLoad();
		}
	}
	if (!bContactMapLoaded && (!ContactMapHandle.IsValid() || ContactMapHandle->HasLoadCompleted()))
	{
		OnContactMapLoaded();
	}
}

void USoftbodySurfaceField::Sleep()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyFieldSleep);
	if (bAsleep)
	{
		return;
	}
	if (USoftbodySurfaceBatch* Batch = PendingBatch.Get())
	{
		Batch->Cancel(this);
		PendingBatch.Reset();
		bCaptureSent = false;
	}
	ReleaseTextures();
	// 다시 깨면 처음처럼 "모든 장이 0"에서 시작한다(미룬 시간·닿는 순간 셈도 처음부터).
	bCleared = true;
	IdleSettle = 0.0;
	PendingDeltaTime = 0.f;
	FramesSinceUpdate = 0;
	LastUpdateTime = -1.0;
	bHadCandidates = false;
	bLumpNormals = false;
	// 덩어리 읽기 고리는 새로 만든다 — 잠들기 전 갱신의 늦은 결과가 깬 뒤에 읽히지 않게(소유 컴포넌트는 깰 때 읽은 번호를 0으로 되돌린다).
	if (LumpReadback.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyLumpReadbackRelease)(
			[State = MoveTemp(LumpReadback)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	USoftbodySurfaceBatch* Batch = Pool.Get();
	if (bPoolReserved)
	{
		if (Batch)
		{
			Batch->Unreserve(ReservedResolution, bReservedLump, ReservedCoarseResolution);
		}
		bPoolReserved = false;
	}
	if (bContactMapAcquired)
	{
		if (Batch)
		{
			Batch->ReleaseContactMap(Data);
		}
		bContactMapAcquired = false;
	}
	if (ContactMapHandle.IsValid())
	{
		ContactMapHandle->CancelHandle();
		ContactMapHandle.Reset();
	}
	ContactMapRefs.Reset();
	bContactMapLoaded = false;
	bAsleep = true;
	SleepSince = FPlatformTime::Seconds();
	if (Batch)
	{
		Batch->AddSleeper(this);
	}
	else
	{
		ReleaseRenderState();
	}
}

void USoftbodySurfaceField::Wake(bool bReservePool)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyFieldWake);
	if (!bAsleep || !Data)
	{
		return;
	}
	bAsleep = false;
	if (!LumpReadback.IsValid())
	{
		LumpReadback = MakeShared<FSoftbodyLumpReadback, ESPMode::ThreadSafe>();
	}
	if (USoftbodySurfaceBatch* Batch = Pool.Get())
	{
		Batch->AcquireContactMap(Data);
		bContactMapAcquired = true;
		PollContactMap();
	}
	else
	{
		LoadContactMapDirect();
	}
	if (bReservePool)
	{
		ReservePool();
	}
}

void USoftbodySurfaceField::ReleaseRenderState()
{
	// 렌더 스레드 상태는 렌더 스레드에서 놓는다 — 렌더 명령은 차례로 돌아, 앞서 올린 갱신이 다 쓴 뒤에 놓인다. 다음 갱신이 필요하면 다시 만든다.
	if (RestState.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyRestStateRelease)(
			[State = MoveTemp(RestState)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	if (FieldVoxelBuffer.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyFieldVoxelRelease)(
			[State = MoveTemp(FieldVoxelBuffer)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	RegisteredFields.Reset();
	RegisteredFieldBases.Reset();
	FieldVoxelWords.Empty();
	FieldVoxelVersion = 0;
	FieldVoxelSentVersion = 0;
	FieldVoxelUploadedVersion = 0;
	bRenderStateBuilt = false;
}

int32 USoftbodySurfaceField::CoarseResolutionFor(float ThicknessScale) const
{
	if (!Data || Data->CarrierResolution <= 0)
	{
		return 0;
	}
	// Update의 거친 격자와 같은 규칙: 굽기가 거리표와 같이 정한 묶음 크기, 없으면(옛 베이크) 칸 크기에서.
	const float TexelSize = FMath::Max(Data->CarrierTexelSize * ThicknessScale, 1e-3f);
	const int32 Block = Data->HasBulgeTable() ? Data->BulgeCellBlock : SoftbodyBulgeTable::BlockFor(TexelSize, Data->CarrierResolution);
	return Data->CarrierResolution / FMath::Max(Block, 1);
}

bool USoftbodySurfaceField::IsReady() const
{
	if (!Data || bAsleep)
	{
		return false;
	}
	if (bContactMapLoaded)
	{
		return true;
	}
	const USoftbodySurfaceBatch* Batch = Pool.Get();
	return bContactMapAcquired && Batch && Batch->IsContactMapReady(Data);
}

bool USoftbodySurfaceField::PollContactMap()
{
	if (bContactMapLoaded)
	{
		return true;
	}
	USoftbodySurfaceBatch* Batch = Pool.Get();
	if (!Data || !bContactMapAcquired || !Batch || !Batch->IsContactMapReady(Data))
	{
		return false;
	}
	bContactMapLoaded = true;
	SpreadThicknessHigh = ThicknessHighOf(Data);
	return true;
}

void USoftbodySurfaceField::ReservePool()
{
	USoftbodySurfaceBatch* Batch = Pool.Get();
	const UWorld* World = GetWorld();
	if (!bPoolReserved && Data && Batch && World && World->IsGameWorld())
	{
		ReservedResolution = Data->CarrierResolution;
		bReservedLump = !Data->CarrierLumpTex.IsNull();
		// 거친 격자는 굽기가 정한 묶음 크기로(거리표 없는 옛 베이크만 메시 스케일에 따라 달라진다 — 그때는 처음 닿을 때 맞는 종류를 원한다고 적는다).
		ReservedCoarseResolution = CoarseResolutionFor(1.f);
		Batch->Reserve(ReservedResolution, bReservedLump, ReservedCoarseResolution);
		if (UsesHardSurface())
		{
			Batch->WantHardPipelines();
		}
		bPoolReserved = true;
	}
}

void USoftbodySurfaceField::OnContactMapLoaded()
{
	if (!Data || bContactMapLoaded)
	{
		return;
	}
	TArray<FSoftObjectPath> Paths;
	Data->GetContactMapPaths(Paths);
	ContactMapRefs.Reset(Paths.Num());
	int32 Missing = 0;
	for (const FSoftObjectPath& Path : Paths)
	{
		if (UObject* Object = Path.ResolveObject())
		{
			ContactMapRefs.Add(Object);
		}
		else
		{
			++Missing;
		}
	}
	ContactMapHandle.Reset();
	if (Missing > 0)
	{
		UE_LOG(LogSoftbodySurfaceField, Warning, TEXT("Contact is off for %s: %d of its contact map textures could not be loaded. Press Rebake Contact Map."),
			*GetPathNameSafe(Data), Missing);
		ContactMapRefs.Reset();
		return;
	}
	bContactMapLoaded = true;
	SpreadThicknessHigh = ThicknessHighOf(Data);
}

void USoftbodySurfaceField::Shutdown()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyFieldShutdown);
	// 끄면 0(일지 222·235): 두 벌(가는·거친)을 풀에 돌려주고(ReleaseTextures: 머티리얼 바인딩이 풀리고 이번 프레임 묶음 신청도 빠진다), 풀 예약과 접촉 지도를
	// 놓는다 — GPU 갱신은 바로 0이고, 풀과 접촉 지도는 softbody.Surface.ReleaseDelay초 동안 쓰이지 않으면 월드 묶음이 프레임마다 조금씩 놓는다(그 안에 다시
	// 켜면 다시 만들거나 읽지 않는다). 렌더 스레드 상태(칸 종류 표·이웃 표·읽기 칸)는 지금 놓는다 — 렌더 명령은 차례로 돌아, 앞서 올린 갱신이 다 쓴 뒤에 놓인다.
	if (USoftbodySurfaceBatch* Batch = PendingBatch.Get())
	{
		Batch->Cancel(this);
		PendingBatch.Reset();
		bCaptureSent = false;
	}
	ReleaseTextures();
	if (LumpReadback.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyLumpReadbackRelease)(
			[State = MoveTemp(LumpReadback)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	if (RestState.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyRestStateRelease)(
			[State = MoveTemp(RestState)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	// 모양 거리장 칸 버퍼(풀 버퍼 참조)도 렌더 스레드에서 놓는다 — 게임 스레드에서 마지막 참조가 풀리면 풀 버퍼를 렌더 스레드 밖에서 놓았다.
	if (FieldVoxelBuffer.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyFieldVoxelRelease)(
			[State = MoveTemp(FieldVoxelBuffer)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	if (bPoolReserved)
	{
		if (USoftbodySurfaceBatch* Batch = Pool.Get())
		{
			Batch->Unreserve(ReservedResolution, bReservedLump, ReservedCoarseResolution);
		}
		bPoolReserved = false;
	}
	if (bContactMapAcquired)
	{
		if (USoftbodySurfaceBatch* Batch = Pool.Get())
		{
			Batch->ReleaseContactMap(Data);
		}
		bContactMapAcquired = false;
	}
	if (ContactMapHandle.IsValid())
	{
		ContactMapHandle->CancelHandle();
		ContactMapHandle.Reset();
	}
	ContactMapRefs.Reset();
	bContactMapLoaded = false;
	bRenderStateBuilt = false;
}

bool USoftbodySurfaceField::GetGrabState(TArray<FVector4f>& OutRows, uint64& InOutSerial) const
{
	if (!RestState.IsValid())
	{
		return false;
	}
	const uint64 Latest = RestState->GrabLatestSerial.load(std::memory_order_acquire);
	if (Latest == 0 || Latest <= InOutSerial)
	{
		return false;
	}
	FScopeLock Lock(&RestState->GrabLock);
	OutRows = RestState->GrabLatest;
	InOutSerial = Latest;
	return OutRows.Num() == SoftbodyGrab::MaxRows * SoftbodyGrab::StateRows;
}

bool USoftbodySurfaceField::GetLumpSolution(TArray<double>& OutSolution, uint64& InOutSerial) const
{
	if (!LumpReadback.IsValid())
	{
		return false;
	}
	FScopeLock Lock(&LumpReadback->Mutex);
	if (LumpReadback->LatestSerial <= InOutSerial || LumpReadback->Latest.Num() != NumLumpGroups * NumLumpOutputs)
	{
		return false;
	}
	OutSolution = LumpReadback->Latest;
	InOutSerial = LumpReadback->LatestSerial;
	return true;
}

void USoftbodySurfaceField::GetGpuTimes(double (&OutMs)[(int32)EGpuTime::Num])
{
	const double Now = FPlatformTime::Seconds();
	uint64 LatestFrame = 0;
	for (int32 Index = 0; Index < (int32)EGpuTime::Num; ++Index)
	{
		LatestFrame = FMath::Max(LatestFrame, SoftbodyGpuTimes::Frame[Index].load(std::memory_order_relaxed));
	}
	for (int32 Index = 0; Index < (int32)EGpuTime::Num; ++Index)
	{
		const bool bFresh = Now - SoftbodyGpuTimes::Stamp[Index].load(std::memory_order_relaxed) < 0.5
			&& SoftbodyGpuTimes::Frame[Index].load(std::memory_order_relaxed) == LatestFrame;
		OutMs[Index] = bFresh ? SoftbodyGpuTimes::Ms[Index].load(std::memory_order_relaxed) : 0.0;
	}
}

bool USoftbodySurfaceField::GetGpuState(FGpuState& Out) const
{
	if (!RestState.IsValid() || !RestState->bHasStats.load(std::memory_order_acquire))
	{
		return false;
	}
	Out.bResting = RestState->StatWords[0].load(std::memory_order_relaxed);
	Out.DentTiles = RestState->StatWords[1].load(std::memory_order_relaxed);
	Out.ContactGroups = RestState->StatWords[2].load(std::memory_order_relaxed);
	Out.bPressed = RestState->StatWords[3].load(std::memory_order_relaxed) != 0 ? 1u : 0u;
	Out.DragSources = RestState->StatWords[4].load(std::memory_order_relaxed);
	Out.DragGroups = RestState->StatWords[5].load(std::memory_order_relaxed);
	Out.DragClusters = RestState->StatWords[6].load(std::memory_order_relaxed);
	Out.DragMixedClusters = RestState->StatWords[7].load(std::memory_order_relaxed);
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const int32 Base = 8 + Pass * 6;
		FGpuState::FDragWalk& Walk = Pass == 0 ? Out.DragCorrect : Out.DragGather;
		Walk.Groups = RestState->StatWords[Base].load(std::memory_order_relaxed);
		Walk.MaxSteps = RestState->StatWords[Base + 1].load(std::memory_order_relaxed);
		Walk.SumSteps = RestState->StatWords[Base + 2].load(std::memory_order_relaxed);
		Walk.MaxNearTiles = RestState->StatWords[Base + 3].load(std::memory_order_relaxed);
		Walk.MaxNear = RestState->StatWords[Base + 4].load(std::memory_order_relaxed);
		Walk.MaxFar = RestState->StatWords[Base + 5].load(std::memory_order_relaxed);
	}
	// [20] 부위 묶음 수, [21..25] 함몰 목록 까닭별 수, [26] 부위 나누기 장(셰이더 StatsCS)
	Out.DentPartsTiles = RestState->StatWords[20].load(std::memory_order_relaxed);
	Out.DentPressedTiles = RestState->StatWords[21].load(std::memory_order_relaxed);
	Out.DentRingTiles = RestState->StatWords[22].load(std::memory_order_relaxed);
	Out.DentLinkedTiles = RestState->StatWords[23].load(std::memory_order_relaxed);
	Out.DentAlwaysTiles = RestState->StatWords[24].load(std::memory_order_relaxed);
	Out.DentCoreTiles = RestState->StatWords[25].load(std::memory_order_relaxed);
	Out.bDentParts = RestState->StatWords[26].load(std::memory_order_relaxed) != 0 ? 1u : 0u;
	// 일지 295: [27] 살 두께보다 깊이 눌린 표면 칸 수, [28] 가장 깊이 넘은 양(float 비트)
	Out.DeepCells = RestState->StatWords[27].load(std::memory_order_relaxed);
	const uint32 DeepestBits = RestState->StatWords[28].load(std::memory_order_relaxed);
	FMemory::Memcpy(&Out.DeepestBeyond, &DeepestBits, sizeof(float));
	Out.LabelOverflowCells = RestState->StatWords[29].load(std::memory_order_relaxed);
	const uint64 StatFrame = RestState->LatestStatFrame.load(std::memory_order_relaxed);
	Out.AgeFrames = GFrameCounter > StatFrame ? (uint32)FMath::Min<uint64>(GFrameCounter - StatFrame, MAX_uint32) : 0u;
	return true;
}

int64 USoftbodySurfaceField::GetTextureMemoryBytes() const
{
	int64 Bytes = 0;
	for (const UTextureRenderTarget2D* Target : {StateA.Get(), Hold.Get(), Position.Get(), Displacement.Get(), PreviousDisplacement.Get(), Normal.Get(),
		Accepted.Get(), Labels.Get(), LumpNormal.Get(), LumpAnchor.Get(), ContactLong.Get(), CoarseInfo.Get(), CoarseLayer.Get(), DragState.Get(), DragHold.Get(),
		DentCoarse.Get(), ContactBend.Get()})
	{
		if (Target)
		{
			Bytes += (int64)Target->SizeX * (int64)Target->SizeY * (int64)GPixelFormats[Target->GetFormat()].BlockBytes;
		}
	}
	// 디버그 캡처 사본(요청한 동안만 있다).
	for (const UTextureRenderTarget2D* Target : CaptureCopies)
	{
		Bytes += FieldTargetBytes(Target);
	}
	return Bytes;
}

void USoftbodySurfaceField::BeginDestroy()
{
	// 묶음(ADR-0021)에 이번 프레임 신청이 남아 있으면 뺀다 — 렌더 타깃이 사라진 뒤 그 포인터로 돌면 안 된다.
	if (USoftbodySurfaceBatch* Batch = PendingBatch.Get())
	{
		Batch->Cancel(this);
	}
	// Shutdown 없이 지워지면(월드를 내릴 때) 풀 예약과 접촉 지도를 여기서 놓는다.
	if (bPoolReserved)
	{
		if (USoftbodySurfaceBatch* Batch = Pool.Get())
		{
			Batch->Unreserve(ReservedResolution, bReservedLump, ReservedCoarseResolution);
		}
		bPoolReserved = false;
	}
	if (bContactMapAcquired)
	{
		if (USoftbodySurfaceBatch* Batch = Pool.Get())
		{
			Batch->ReleaseContactMap(Data);
		}
		bContactMapAcquired = false;
	}
	// 읽기 칸의 RHI 자원은 렌더 스레드에서 놓는다(쉬기 상태에도 통계 읽기 칸이 있다).
	if (LumpReadback.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyLumpReadbackRelease)(
			[State = MoveTemp(LumpReadback)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	if (RestState.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyRestStateRelease)(
			[State = MoveTemp(RestState)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	// 모양 거리장 칸 버퍼(풀 버퍼 참조)도 렌더 스레드에서 놓는다 — 게임 스레드에서 마지막 참조가 풀리면 풀 버퍼를 렌더 스레드 밖에서 놓았다.
	if (FieldVoxelBuffer.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyFieldVoxelRelease)(
			[State = MoveTemp(FieldVoxelBuffer)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	Super::BeginDestroy();
}

int32 USoftbodySurfaceField::PackShapes(TConstArrayView<FSoftbodyColliderShape> WorldShapes,
	const FTransform& ComponentTransform, int32 MaxShapes, int32 MaxPlanes,
	TArray<FVector4f>& OutShapes, TArray<FVector4f>& OutPlanes, TArray<int32>* OutSourceIndices)
{
	OutShapes.Reset();
	OutPlanes.Reset();
	if (OutSourceIndices)
	{
		OutSourceIndices->Reset();
	}
	const FQuat InverseRotation = ComponentTransform.GetRotation().Inverse();
	const FVector Origin = ComponentTransform.GetLocation();
	int32 Skipped = 0;
	int32 Packed = 0;
	for (int32 SourceIndex = 0; SourceIndex < WorldShapes.Num(); ++SourceIndex)
	{
		const FSoftbodyColliderShape& Shape = WorldShapes[SourceIndex];
		float Type = 0.f;
		FVector4f Size(0.f, 0.f, 0.f, 0.f);
		switch (Shape.Type)
		{
		case ESoftbodyColliderShapeType::Sphere:
			Type = 0.f;
			Size = FVector4f(Shape.Radius, 0.f, 0.f, 0.f);
			break;
		case ESoftbodyColliderShapeType::Capsule:
			Type = 1.f;
			Size = FVector4f(Shape.Radius, Shape.HalfLength, 0.f, 0.f);
			break;
		case ESoftbodyColliderShapeType::Box:
			Type = 2.f;
			Size = FVector4f((float)Shape.HalfExtent.X, (float)Shape.HalfExtent.Y, (float)Shape.HalfExtent.Z, 0.f);
			break;
		case ESoftbodyColliderShapeType::Convex:
			// 평면 뒤에 정점 상자 반폭 한 줄을 더 싣는다(아래).
			if (Shape.LocalPlanes.Num() == 0 || OutPlanes.Num() + Shape.LocalPlanes.Num() + 1 > MaxPlanes)
			{
				++Skipped;
				continue;
			}
			Type = 3.f;
			Size = FVector4f((float)OutPlanes.Num(), (float)Shape.LocalPlanes.Num(), 0.f, 0.f);
			break;
		default:
			++Skipped;
			continue;
		}
		if (Packed >= MaxShapes)
		{
			++Skipped;
			continue;
		}
		if (Shape.Type == ESoftbodyColliderShapeType::Convex)
		{
			for (const FPlane& Plane : Shape.LocalPlanes)
			{
				OutPlanes.Add(FVector4f((float)Plane.X, (float)Plane.Y, (float)Plane.Z, (float)Plane.W));
			}
			// 덩어리 힘이 누르는 것의 가로 크기로 쓰는 로컬 정점 상자 반폭(셰이더 SB_PresserBreadth). 평면 개수 밖이라 평면 읽기는 보지 않는다.
			OutPlanes.Add(FVector4f(FVector3f(Shape.HalfExtent), 0.f));
		}
		const FVector Center = InverseRotation.RotateVector(Shape.Frame.GetLocation() - Origin);
		const FQuat Rotation = (InverseRotation * Shape.Frame.GetRotation()).GetNormalized();
		OutShapes.Add(FVector4f((float)Center.X, (float)Center.Y, (float)Center.Z, Type));
		OutShapes.Add(FVector4f((float)Rotation.X, (float)Rotation.Y, (float)Rotation.Z, (float)Rotation.W));
		OutShapes.Add(Size);
		if (OutSourceIndices)
		{
			OutSourceIndices->Add(SourceIndex);
		}
		++Packed;
	}
	return Skipped;
}

int32 USoftbodySurfaceField::RegisterColliderField(const USoftbodyColliderField* Field)
{
	if (!Field)
	{
		return 0;
	}
	for (int32 Index = 0; Index < RegisteredFields.Num(); ++Index)
	{
		if (RegisteredFields[Index].Get() == Field)
		{
			return RegisteredFieldBases[Index];
		}
	}
	// 첫 칸 번호가 늘 짝수가 되도록 거리장마다 반정밀 두 개씩 묶어 붙인다(홀수 개면 마지막 짝은 0).
	const int32 Base = FieldVoxelWords.Num() * 2;
	const int32 NumWords = (Field->Voxels.Num() + 1) / 2;
	FieldVoxelWords.Reserve(FieldVoxelWords.Num() + NumWords);
	for (int32 Word = 0; Word < NumWords; ++Word)
	{
		const uint32 Low = Field->Voxels[Word * 2];
		const uint32 High = Word * 2 + 1 < Field->Voxels.Num() ? Field->Voxels[Word * 2 + 1] : 0u;
		FieldVoxelWords.Add(Low | (High << 16));
	}
	RegisteredFields.Add(Field);
	RegisteredFieldBases.Add(Base);
	++FieldVoxelVersion;
	return Base;
}

bool USoftbodySurfaceField::Update(const FSoftbodySurfaceFieldInput& Input)
{
	// 멈춘 틱(일지 203)은 지난 갱신의 덩어리 노멀이 텍스처에 그대로라 값을 잇는다(미룬 틱과 같다).
	const bool bHadLumpNormals = bLumpNormals;
	bLumpNormals = false;
	// 그리지 않는 실행(전용 서버, 창 없는 커맨드릿)에는 렌더 타깃의 GPU 자원이 없다. 표면 장은 화면에만 쓰이므로 건너뛴다.
	if (!FApp::CanEverRender())
	{
		return false;
	}
	// 접촉 지도가 다 올라올 때까지는 돌지 않는다(월드 묶음이 나눠 올린다, 일지 235).
	if (!Data || !PollContactMap())
	{
		return false;
	}

	// 비워진 뒤(또는 처음) 첫 프레임은 위치 RT에 이어지는 지난 위치가 없다.
	const bool bMotionHistory = !bCleared;
	// 잡기(ADR-0041)도 닿을 것이다 — 잡는 동안은 닿을 콜라이더가 없어 쉬는 규칙만 멈춘다(화면 밖·작게 보여 쉬는 것은 컴포넌트가 잡기 줄을 싣지 않는다).
	const bool bHasShapes = Input.Shapes.Num() > 0 || Input.FieldInstances.Num() > 0 || Input.Grabs.Num() > 0;
	// 갱신률 LOD(ADR-0021): 닿는 순간(지난 틱엔 닿을 수 있는 것이 없었다)과 비운 뒤 첫 갱신은 미루지 않고 이번 프레임에 돈다.
	const bool bNewlyTouched = bHasShapes && (!bHadCandidates || bCleared);
	bHadCandidates = bHasShapes;
	bInView = Input.bInView;
	LastDeltaTime = FMath::Max(Input.DeltaTime, 0.f);
	if (bHasShapes)
	{
		IdleSettle = 0.0;
		// "장이 모두 0"(bCleared)은 묶음이 실제로 돌린 뒤 OnScheduled가 푼다 — 미룬 갱신은 장에 아무것도 쓰지 않았다(ADR-0021 §3.3의 4).
		// 처음 닿는 순간(또는 비운 뒤 다시 닿는 순간) 갱신을 넘어 남는 텍스처 두 벌(가는·거친)을 풀에서 잡는다(ADR-0022). 풀에 없으면 이번 틱은 돌지 않는다 —
		// 풀이 프레임 예산 안에서 만들어 두면 몇 프레임 뒤 눌리기 시작한다(일지 235: 그 자리에서 만들면 렌더 스레드·GPU가 멈칫한다).
		if (!HasTextures())
		{
			const FVector3f TouchScale = Input.ComponentScale.GetAbs();
			if (!AcquireTextures(CoarseResolutionFor((TouchScale.X + TouchScale.Y + TouchScale.Z) / 3.f)))
			{
				return false;
			}
		}
	}
	else
	{
		if (bCleared)
		{
			// 닿자마자 떠나 한 번도 못 돌렸으면 잡아 둔 벌(아직 0)을 돌려준다. 잡은 것이 없으면 아무것도 하지 않는다.
			ReleaseTextures();
			return false;
		}
		// 복원이 끝났는지는 실제로 돌린 갱신이 줄인 몫으로 센다(OnScheduled). 틱 수로 세면, 갱신 상한에 밀려 한 번도 돌지 못한 장이
		// 눌린 모양 그대로 멈춰 있다가 "복원 시간"이 지난 틱에 한꺼번에 지워졌다(일지 136). 화면에 없는 장만은 돌지 못한 틱도 끝난
		// 것으로 센다: 지워져도 보이지 않고, 안 그러면 보이지 않는 장이 다 돌 때까지 텍스처를 쥐고 차례를 다툰다.
		if (!bInView)
		{
			IdleSettle += SettleOfStep(Input, LastDeltaTime, LastDeltaTime);
		}
		if (IdleSettle >= 1.0)
		{
			// 콜라이더 없이 복원이 끝났다: 갱신을 넘어 남는 텍스처를 0으로 비워 월드 풀에 돌려준다(ADR-0022). 머티리얼은 그동안 표면 장을
			// 읽지 않는다(SurfaceFieldOn 0, 바인딩도 푼다). 다시 닿으면 그 프레임에 한 벌을 다시 잡는다.
			bCleared = true;
			PendingDeltaTime = 0.f;
			FramesSinceUpdate = 0;
			ReleaseTextures();
			return false;
		}
	}

	// 멈춤(일지 203, ADR-0021 §8): 눌린 채 입력(콜라이더·본·거리장 부위·덩어리 목표·조절값)이 마지막으로 돌린 갱신과 같고(허용 오차 softbody.Surface.StillDistance),
	// 그 뒤 돌린 갱신의 GPU 표지가 "눌림·변위 안 바뀜"이면 신청하지 않는다 — 평형이라 다시 돌려도 같은 값을 쓴다. 깨우기는 CPU가 같은 프레임에(입력이 움직이면
	// 바로 신청), 재우기만 몇 프레임 늦은 GPU 표지로(늦는 쪽이 더 돌리는 쪽). 멈춘 동안 시간은 모으지 않는다 — 평형에는 흐를 시간이 없고, 모으면 깨는 걸음이
	// 멈춘 시간 전체가 되어 그때 뗀 칸의 기억·자국·끄는 힘이 한 번에 사라진다. 뗀 뒤 복원 중인 장은 눌림 비트가 없어 멈추지 않는다(복원 끝은 지금 셈대로).
	bStill = false;
	const float StillDistance = CVarSoftbodySurfaceStillDistance.GetValueOnGameThread();
	const bool bInputStill = StillDistance > 0.f && bHasLastRunInput && InputStill(Input, LastRunInput, StillDistance);
	if (!bInputStill)
	{
		StillSinceSerial = 0;
	}
	else if (StillSinceSerial == 0)
	{
		StillSinceSerial = UpdateSerial + 1;   // 아래에서 신청하면 받을 번호
	}
	// 디버그 캡처 요청은 멈춘 장도 한 번 돌린다 — 접촉 텍스처는 갱신 안에서만 살아서, 쉬면 사본이 없다(일지 210). 평형이라 돌려도 같은 값이다.
	if (bInputStill && !bCapturePending && RestState.IsValid())
	{
		const uint64 Latest = RestState->LatestStill.load(std::memory_order_acquire);
		const uint32 LatestSerial = (uint32)(Latest >> 32);
		const uint32 Word = (uint32)Latest;
		if (LatestSerial >= (uint32)StillSinceSerial && (Word & StillPressed) != 0 && (Word & StillChanged) == 0)
		{
			bLumpNormals = bHadLumpNormals;
			OnStill();
			return true;
		}
	}

	FSoftbodySurfaceFieldRender RI;
	RI.StateA = StateA->GameThread_GetRenderTargetResource();
	RI.Hold = Hold->GameThread_GetRenderTargetResource();
	RI.Position = Position->GameThread_GetRenderTargetResource();
	RI.Displacement = Displacement->GameThread_GetRenderTargetResource();
	RI.PreviousDisplacement = PreviousDisplacement->GameThread_GetRenderTargetResource();
	RI.Normal = Normal->GameThread_GetRenderTargetResource();
	RI.Accepted = Accepted->GameThread_GetRenderTargetResource();
	RI.Labels = Labels ? Labels->GameThread_GetRenderTargetResource() : nullptr;
	RI.CarrierPosition = ResourceOf(Data->CarrierPositionTex);
	RI.CarrierNormal = ResourceOf(Data->CarrierNormalTex);
	RI.Redirect = ResourceOf(Data->CarrierRedirectTex);
	RI.SkinIndex = ResourceOf(Data->CarrierSkinIndexTex);
	RI.SkinWeight = ResourceOf(Data->CarrierSkinWeightTex);
	// 부위 나누기(Separated Parts): 굽기가 담은 칸 몫이 있을 때만.
	RI.CarrierSeparation = ResourceOf(Data->CarrierSeparationTex);
	RI.bSeparation = RI.CarrierSeparation ? 1u : 0u;
	// 하드 서피스(ADR-0037 §3.7): 조각이 구워진 자산만 조각 순열을 쓴다. 실행 중 바뀌지 않는다(구운 데이터로만 정해진다).
	RI.CarrierPiece = Data->HasHardPieces() ? ResourceOf(Data->CarrierPieceTex) : nullptr;
	RI.CarrierLift = RI.CarrierPiece ? ResourceOf(Data->CarrierLiftTex) : nullptr;
	// Flex 칠: 조각이 없어도 칠이 구워져 있으면 싣는다 — 연속 굽힘은 판 밖 칸도 칠을 읽는다(거친 칸 굽힘 길이 CoarseBend).
	RI.CarrierSoftness = ResourceOf(Data->CarrierSoftnessTex);
	// 판 굽힘 길이 눈금 L0: 굽기(SoftbodyPlateBake)와 같은 고정 길이 — 칠한 살 두께 중앙값을 쓰면 Depth 칠이 판 굳기를 바꾼다(원칙 P03).
	RI.HardRefThickness = SoftbodyContactResponse::ReferenceLength;
	RI.CarrierLiftScale = Data->CarrierLiftScale;
	if (RI.CarrierPiece && !HardTable.IsValid())
	{
		// 셰이더 꼴 조각 표: 한 번 만들어 갱신마다 공유한다(구운 표는 데이터가 바뀔 때만 바뀐다).
		TSharedRef<FSoftbodyHardPieceTable, ESPMode::ThreadSafe> Table = MakeShared<FSoftbodyHardPieceTable, ESPMode::ThreadSafe>();
		for (const FSoftbodyHardPiece& Piece : Data->HardPieces)
		{
			Table->Frames.Add(FVector4f(Piece.Center, Piece.Softness));   // w = 조각의 대표 Flex 칠(Flex Offset 비)
			Table->Frames.Add(FVector4f(Piece.AxisU, Piece.Radius));
			// w = 조각 종류(셰이더 SB_HardKindOf): 떠 있는 장식은 FloatingPieceKind, 붙은 판은 가장 큰 Flex 칠 − 강체 문턱 칠(조각 크기 비 —
			// SoftbodyContactResponse::RigidFlexLimit). w + Flex Offset ≤ 0이면 강체 판, 아니면 칠대로 굽는 겉이다.
			Table->Frames.Add(FVector4f(Piece.AxisV, Piece.bFloating ? SoftbodyContactResponse::FloatingPieceKind
				: Piece.MaxSoftness - SoftbodyContactResponse::RigidFlexLimit(Piece.Radius)));
			Table->TileRange.Add(FUintVector2((uint32)Piece.FirstTile, (uint32)Piece.NumTiles));
			Table->CellRange.Add(FUintVector2((uint32)Piece.FirstCell, (uint32)Piece.NumCells));
		}
		static int32 NextHardPlateSerial = 0;
		Table->Serial = (uint32)FPlatformAtomics::InterlockedIncrement(&NextHardPlateSerial);
		Table->TileList = Data->HardPieceTiles;
		Table->CellList = Data->HardPieceCells;
		Table->CellPlate = Data->HardCellPlate;
		HardTable = Table;
	}
	RI.HardTable = RI.CarrierPiece ? HardTable : nullptr;
	RI.bHardSurface = RI.CarrierPiece && RI.HardTable.IsValid() && RI.HardTable->TileRange.Num() > 0 && RI.HardTable->CellPlate.Num() > 0 ? 1u : 0u;
	// 연속 굽힘(Flex 칠): 조각이 없어도 칠이 구워져 있거나 Flex Offset이 음수면 칸이 굽을 수 있다 — 같은 순열을 빈 조각 표로 쓴다. 파이프라인이 아직이면
	// 이번 갱신은 굽힘 없이 돈다(묻는 것이 준비를 한 걸음 민다 — 실행 중 Offset을 내려도 끊기지 않고 굽힘이 몇 프레임 늦게 켜진다).
	RI.bFlexOnly = !RI.bHardSurface && (RI.CarrierSoftness || FMath::Clamp(Input.FlexOffset, -1.f, 1.f) < 0.f) && AreHardPipelinesReady() ? 1u : 0u;
	RI.Shapes = Input.Shapes;
	RI.Bones = Input.Bones;
	// 잡기(ADR-0041): 줄 배치가 맞을 때만(아니면 잡기 없음).
	if (Input.Grabs.Num() == SoftbodyGrab::MaxRows * SoftbodyGrab::InputRows)
	{
		RI.GrabInput = Input.Grabs;
	}
	// 손 닻(일지 336): 지난 갱신 자세를 모르면 지금 자세 — 피부가 본을 따라 움직인 거리 0(전 규칙).
	RI.PreviousBones = Input.PreviousBones.Num() == Input.Bones.Num() ? Input.PreviousBones : Input.Bones;
	RI.Planes = Input.Planes;
	RI.FieldInstances = Input.FieldInstances;
	// 끌림: 형상·부위마다 3행. 개수가 맞지 않으면(지난 위치를 모름) 모두 제자리로 본다.
	auto MotionOrIdentity = [](const TArray<FVector4f>& Motion, int32 NumRows, TArray<FVector4f>& Out)
	{
		if (Motion.Num() == NumRows)
		{
			Out = Motion;
			return;
		}
		Out.Reset(NumRows);
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			const int32 Column = Row % 3;
			Out.Add(FVector4f(Column == 0 ? 1.f : 0.f, Column == 1 ? 1.f : 0.f, Column == 2 ? 1.f : 0.f, 0.f));
		}
	};
	MotionOrIdentity(Input.ShapeMotion, Input.Shapes.Num(), RI.ShapeMotion);
	MotionOrIdentity(Input.FieldMotion, Input.FieldInstances.Num() / SoftbodySurfaceFieldRows::PerInstance * 3, RI.FieldMotion);
	RI.DragRatio = FMath::Max(Input.DragRatio, 0.f);
	// 살의 힘 법칙의 가파름: 누르는 힘(덩어리)과 피부를 옆으로 미는 힘(끌림)이 같이 쓴다(ADR-0008 §12).
	RI.LumpStiffen = FMath::Max(Input.LumpStiffen, 0.f);
	// 부피 보존 칠(ADR-0024): 캐리어에 칸별 부피 보존이 구워져 있으면 이완(함몰 둘레 폭)과 솟음(솟을 부피)이 읽는다. 없으면 전부 1(살).
	RI.CarrierVolume = ResourceOf(Data->CarrierVolumeTex);
	// 압축성(ADR-0037 §3.3): 칸의 v = 저작 에셋 값 × 칸 칠, 컴포넌트 Volume Offset은 셰이더가 그 v에 더하고 0~1로 자른다(SB_VolumeShifted —
	// v는 기준 누름에서 살 대비 솟는 부피 몫이라 같은 폭이면 어느 칠에서나 같은 만큼 달라 보인다). 둘 다 굽지 않고 여기로 바로 온다(덩어리 모양은
	// 구운 저작 에셋 값).
	RI.VolumeValue = FMath::Clamp(Data->VolumePreservation, 0.f, 1.f);
	RI.VolumeOffset = FMath::Clamp(Input.VolumeOffset, -1.f, 1.f);
	// 판(ADR-0038 §4): 컴포넌트 Flex Offset을 판 칠에 더한다 — 가는 창 붙듦은 칸 칠에, 판 모드 굳기는 조각의 대표 칠을 옮긴 굽힘 길이 비로.
	RI.FlexOffset = FMath::Clamp(Input.FlexOffset, -1.f, 1.f);
	// 겉껍질 장력(ADR-0037 §3.4): 길이 L_T = 4 s² × L0(고정 길이, 메시 로컬). s는 컴포넌트 값이라 실행 중에 바꾸면 바로 온다. 칸별 칠은
	// 없다(일지 298, ADR-0038 §3.5 — 부분 겉은 Flex 칠의 판). 전에는 L0 대신 칠한 살 두께 중앙값이라 다른 부위의 Depth 칠이 겉껍질을 바꿨다(원칙 P03).
	{
		const float SkinValue = FMath::Clamp(Input.SkinTension, 0.f, 2.f);   // 상한 2(SoftbodyComponent.h Skin Tension)
		RI.SkinLength = 4.f * SkinValue * SkinValue * SoftbodyContactResponse::ReferenceLength;
	}
	// 제 살 깊이(ADR-0003 §16): 접촉이 살 속에 묻힌 콜라이더를 찾는 깊이의 칸별 비율. 없으면 전부 1(살 두께 그대로).
	RI.CarrierColumn = ResourceOf(Data->CarrierColumnTex);
	RI.bMotionHistory = bMotionHistory ? 1u : 0u;
	// 들어온 면 표지(일지 313): 콜라이더 고정 번호를 올리는 차례(형상 → 거리장 부위)로 잇는다. 개수가 맞지 않으면 번호 없음(127 — 지금 규칙).
	// 길 검사는 콜라이더의 지난 자세가 지난 갱신 것일 때만(쉬다 깨어난 첫 갱신은 PositionOut의 지난 자리와 시점이 다르다).
	{
		const int32 NumShapes = Input.Shapes.Num() / 3;
		const int32 NumParts = Input.FieldInstances.Num() / SoftbodySurfaceFieldRows::PerInstance;
		RI.ColliderIds.Reset(FMath::Max(NumShapes + NumParts, 1));
		const bool bShapeIds = Input.ShapeIds.Num() == NumShapes;
		const bool bFieldIds = Input.FieldIds.Num() == NumParts;
		for (int32 Index = 0; Index < NumShapes; ++Index)
		{
			RI.ColliderIds.Add(bShapeIds ? Input.ShapeIds[Index] : 127u);
		}
		for (int32 Index = 0; Index < NumParts; ++Index)
		{
			RI.ColliderIds.Add(bFieldIds ? Input.FieldIds[Index] : 127u);
		}
		if (RI.ColliderIds.Num() == 0)
		{
			RI.ColliderIds.Add(127u);
		}
		RI.bSweep = Input.bSweep && bMotionHistory ? 1u : 0u;
	}
	// 덩어리(ADR-0012): 브릿지가 머티리얼과 같은 배치(8 × 9)일 때만 옮긴 자리에서 잰다.
	RI.CarrierLump = ResourceOf(Data->CarrierLumpTex);
	RI.CarrierLumpSideA = ResourceOf(Data->CarrierLumpSideATex);
	RI.CarrierLumpSideB = ResourceOf(Data->CarrierLumpSideBTex);
	if (RI.CarrierLump && Input.InertiaBridge.Num() == 72)
	{
		RI.InertiaBridge = Input.InertiaBridge;
		RI.ComponentRotation = FVector4f(Input.ComponentRotation.X, Input.ComponentRotation.Y, Input.ComponentRotation.Z,
			Input.ComponentRotation.W);
		RI.FrameToActor = Input.FrameToActor;
		RI.LumpMode = Input.bLumpForce ? 2u : 1u;
		RI.LumpNormal = LumpNormal ? LumpNormal->GameThread_GetRenderTargetResource() : nullptr;
		RI.LumpAnchor = LumpAnchor ? LumpAnchor->GameThread_GetRenderTargetResource() : nullptr;
		RI.LumpReadback = LumpReadback;
		RI.LumpGrip = FMath::Max(Input.LumpGrip, 0.f);
		if (RI.LumpMode == 2 && Input.LumpGroups.Num() == NumLumpGroups * NumLumpGroupRows)
		{
			RI.LumpGroups = Input.LumpGroups;
		}
	}
	if (!FieldVoxelBuffer.IsValid())
	{
		FieldVoxelBuffer = MakeShared<FSoftbodyFieldVoxelBuffer, ESPMode::ThreadSafe>();
	}
	// 새 거리장이 들어왔으면 칸 값을 싣는다. 미룬 갱신은 실은 것째로 버려지므로 "올라갔다"는 실제로 돌린 뒤(OnScheduled)에만 적는다.
	// 전엔 여기서 적어, 처음 닿는 갱신이 상한에 밀려 미뤄지면 손 거리장이 영영 빈 버퍼(값 0 = 어디나 표면)로 남았다(일지 106).
	if (FieldVoxelUploadedVersion != FieldVoxelVersion)
	{
		RI.FieldVoxelUpload = FieldVoxelWords;
		FieldVoxelSentVersion = FieldVoxelVersion;
	}
	RI.FieldVoxelState = FieldVoxelBuffer;
	if (!RestState.IsValid())
	{
		RestState = MakeShared<FSoftbodySurfaceRestState, ESPMode::ThreadSafe>();
	}
	RI.RestState = RestState;
	RI.bReadStats = Input.bReadStats;
	// 멈춤(일지 203): 변위 변화 문턱(cm). 멈춤을 껐으면(-1) 합치기가 견주지 않고 읽기도 하지 않는다.
	RI.StillChange = StillDistance > 0.f ? FMath::Max(CVarSoftbodySurfaceStillChange.GetValueOnGameThread(), 0.f) : -1.f;
	if (DebugName.IsEmpty())
	{
		// 프로파일 이름(일지 189): 액터 이름(에디터·PIE는 아웃라이너 이름). 한 액터에 같은 컴포넌트가 여럿이면 컴포넌트 이름을 붙인다.
		const UActorComponent* Component = Cast<UActorComponent>(GetOuter());
		const AActor* Owner = Component ? Component->GetOwner() : nullptr;
		if (Owner)
		{
#if WITH_EDITOR
			DebugName = Owner->GetActorLabel();
#else
			DebugName = Owner->GetName();
#endif
			TInlineComponentArray<UActorComponent*> Same;
			Owner->GetComponents(Component->GetClass(), Same);
			if (Same.Num() > 1)
			{
				DebugName += TEXT(".") + Component->GetName();
			}
		}
		else
		{
			DebugName = GetOuter() ? GetOuter()->GetName() : GetName();
		}
	}
	RI.DebugName = DebugName;
	RI.ComponentScale = Input.ComponentScale;
	RI.Resolution = (uint32)Data->CarrierResolution;
	// 칸 크기와 두께는 메시 로컬 단위다. 비균일 스케일은 평균으로 근사한다.
	const FVector3f AbsScale = Input.ComponentScale.GetAbs();
	RI.ThicknessScale = (AbsScale.X + AbsScale.Y + AbsScale.Z) / 3.f;
	RI.TexelSize = FMath::Max(Data->CarrierTexelSize * RI.ThicknessScale, 1e-3f);
	RI.EdgeTrimWidth = FMath::Max(Input.EdgeTrim, 0.f) * RI.TexelSize;
	RI.MaxContactDepth = Input.MaxContactDepth;
	RI.OcclusionStrength = FMath::Clamp(Input.OcclusionStrength, 0.f, 1.f);
	// 갱신률 LOD(ADR-0021): 미룬 프레임의 시간을 모아 한 걸음으로 푼다. 틱마다 곱하는 값(복원·자국 길들기·자국 사라짐)은 모은
	// 시간만큼 거듭제곱해 "몇 틱을 한 번에" 푼 것과 같게 한다. 암시적 한 걸음이라 걸음이 커져도 안정하다.
	const float StepTime = PendingDeltaTime + LastDeltaTime;
	const float StepScale = LastDeltaTime > 1e-6f ? StepTime / LastDeltaTime : 1.f;
	RI.AcceptBlend = 1.f - FMath::Pow(1.f - FMath::Clamp(Input.AcceptBlend, 0.f, 1.f), StepScale);
	RI.AcceptReturn = FMath::Pow(FMath::Clamp(Input.AcceptReturn, 0.f, 1.f), StepScale);
	// 쉬기(ADR-0013): 이번 걸음이 복원을 끝내는 몫. CPU(콜라이더가 떠난 뒤, OnScheduled)와 GPU(눌린 칸이 없어진 뒤, ActivityArgsCS)가
	// 같은 값을 더해 센다.
	StepSettle = SettleOfStep(Input, LastDeltaTime, StepTime);
	RI.SettleStep = (uint32)FMath::Clamp<int64>(FMath::RoundToInt64(StepSettle * (double)SettledProgress), 1, (int64)SettledProgress);
	// 살층 한 식(ADR-0019)의 시간: 복원율 R 하나. 걸음 h = e^{RΔt} − 1(갱신률 LOD가 미룬 시간 포함): 고른 함몰이 한 걸음에 1/(1 + h) = e^{−RΔt}배가
	// 되어 프레임 시간과 상관없이 복원 빠르기가 정확하다. h = RΔt(앞 오일러)면 같은 복원율이 30 fps에서 느리고 144 fps에서 빨랐다(1%까지 0.22초 /
	// 0.17초, 정확한 값 0.15초). 함몰(ADR-0032)의 누른 자리 기억과 끌림(ADR-0033)의 끄는 힘이 같은 배수로 줄어든다. 폭 ℓ은 두께에서.
	// 함몰 정적 해의 체비쇼프 범위(D⁻¹A, 위끝 2는 대각 우세라 늘 맞다)의 아래끝은 두꺼운 살(두께 상위 10%)의 폭이 만드는 값과 N번으로 줄일 수 있는
	// 값(8/N²) 가운데 큰 쪽이다 — 그보다 넓은 모양은 이 갱신에 덜 다가갈 뿐 커지지 않는다.
	// 함몰 폭(ADR-0037): 응답 표의 가는 창 폭 ℓ₁/h 가운데 가장 넓은 것(부피 보존 전부, 겉껍질은 두꺼운 살의 τ = L_T/h까지) × 두꺼운 살.
	// 같은 L_T에서 얇은 살은 τ가 커도 폭 ℓ₁ = (ℓ₁/h)·h는 두꺼운 살보다 좁다.
	const float TauHigh = SpreadThicknessHigh > 0.f ? RI.SkinLength / SpreadThicknessHigh : 0.f;
	const double WindowRatio = (double)SoftbodyContactResponse::WindowRatioMax(TauHigh);
	const double Reach = WindowRatio * (double)SpreadThicknessHigh * (double)RI.ThicknessScale;
	{
		const float RelaxStep = FMath::Exp(FMath::Min(FMath::Max(Input.RecoveryRate, 0.f) * StepTime, 30.f)) - 1.f;
		RI.HoldDecay = 1.f / (1.f + RelaxStep);
		RI.DentChebyshevLow = StaticChebyshevLowFor(Reach, RI.TexelSize, DentIterations);
	}
	if (!RI.CarrierPosition || !RI.CarrierNormal || !RI.Redirect)
	{
		return false;
	}

	// 솟음(ADR-0011·ADR-0031)과 끌림(ADR-0033)의 거친 격자: 묶음은 굽기가 거리표와 같이 정한 값(없으면 같은 규칙으로 여기서). 거친 격자 텍스처는
	// 크기가 바뀔 때만 새로 만든다. 거리표가 없는 옛 베이크는 솟음과 끌림을 끈다(경고 한 번) — 함몰은 그대로 돈다.
	// 거리표는 16비트 한 채널(G16)이어야 셰이더가 읽는 값이 굽기가 적은 정수다. 에디터에서 막 구워 아직 만드는 중(PF_Unknown)이면 이번 갱신만
	// 솟음 없이 돈다(경고 없음). 다른 형식이면 굽기가 잘못된 것 — 경고하고 끈다.
	const bool bBulgeTable = Data->HasBulgeTable();
	const UTexture2D* BulgeTable = Data->CarrierBulgeDistanceTex.Get();
	EPixelFormat LandmarkFormat = bBulgeTable && BulgeTable ? BulgeTable->GetPixelFormat() : PF_Unknown;
#if WITH_EDITOR
	// 에디터는 텍스처를 뒤에서 만드는 동안 32×32 자리 텍스처를 보인다 — 그동안은 "아직"이지 잘못된 형식이 아니다.
	if (BulgeTable && (BulgeTable->IsCompiling() || BulgeTable->IsDefaultTexture()))
	{
		LandmarkFormat = PF_Unknown;
	}
#endif
	const bool bBulgeTableReady = bBulgeTable && LandmarkFormat == PF_G16;
	const int32 Block = bBulgeTable ? Data->BulgeCellBlock : SoftbodyBulgeTable::BlockFor(RI.TexelSize, Data->CarrierResolution);
	const int32 CoarseResolution = Data->CarrierResolution / Block;
	// 거친 격자 텍스처는 처음 닿을 때 가는 벌과 같이 풀에서 잡았다(일지 235). 크기가 다르면(거리표 없는 옛 베이크를 실행 중에 크기 바꿈) 두 벌을 돌려주고
	// 다음 틱에 맞는 종류를 잡는다 — 그 자리에서 만들지 않는다.
	if (!ContactLong || !CoarseInfo || !CoarseLayer || !DragState || !DragHold || !DentCoarse || !ContactBend || ContactLong->SizeX != CoarseResolution)
	{
		bCleared = true;
		PendingDeltaTime = 0.f;
		FramesSinceUpdate = 0;
		ReleaseTextures();
		return false;
	}
	RI.LandmarkDistance = bBulgeTableReady ? ResourceOf(Data->CarrierBulgeDistanceTex) : nullptr;
	RI.NumLandmarks = bBulgeTableReady ? (uint32)Data->BulgeLandmarkCount : 0u;
	// 거리표 줄 길이는 판마다 다르다(판 5는 줄 끝에 붙은 살갗까지 거리 — 일지 252, 판 6은 그 뒤에 살 조각 번호 — 셰이더가 줄 길이로 판을 가려
	// SB_PressurePiece에서 읽는다). 판 4 표도 그대로 읽고 솟을 여유는 살 두께로 둔다.
	RI.LandmarkRowTexels = bBulgeTableReady ? (uint32)SoftbodyBulgeTable::RowTexels(Data->BulgeLandmarkCount, Data->BulgeTableVersion) : 0u;
	// 끌림(ADR-0033)은 솟음 거친 격자의 거리표·정지 위치·넓이를 같이 쓴다 — 거리표가 없으면 끌림도 끈다. 이웃 표의 칸 번호가 20비트라 거친 격자 한 변이
	// MaxDragResolution을 넘는 캐리어(10 m가 넘는 메시)도 끈다.
	const bool bDragReady = bBulgeTableReady && CoarseResolution <= (int32)MaxDragResolution;
	if (!bBulgeTableReady && (Input.bContactLong || RI.DragRatio > 0.f))
	{
		if (!bBulgeTableWarned && !bBulgeTable)
		{
			bBulgeTableWarned = true;
			UE_LOG(LogSoftbodySurfaceField, Warning,
				TEXT("The rise around presses and drag are off for %s: its contact map has no bulge distance table of a supported layout (baked before the table existed, or with an older plugin - table layout version %d, supported %d to %d). Press Rebake Contact Map to turn them back on."),
				*GetPathNameSafe(Data), Data->BulgeTableVersion, SoftbodyBulgeTable::MinVersion, SoftbodyBulgeTable::Version);
		}
		else if (!bBulgeTableWarned && bBulgeTable && LandmarkFormat != PF_Unknown)
		{
			bBulgeTableWarned = true;
			UE_LOG(LogSoftbodySurfaceField, Warning,
				TEXT("The rise around presses and drag are off for %s: its bulge distance table has pixel format %s instead of G16 (16-bit grayscale). Press Rebake Contact Map."),
				*GetPathNameSafe(Data), GPixelFormats[LandmarkFormat].Name);
		}
	}
	else if (!bDragReady && RI.DragRatio > 0.f && !bBulgeTableWarned)
	{
		bBulgeTableWarned = true;
		UE_LOG(LogSoftbodySurfaceField, Warning, TEXT("Drag is off for %s: its bulge grid is %d cells per side, more than the %d the drag grid supports."),
			*GetPathNameSafe(Data), CoarseResolution, MaxDragResolution);
	}
	// 접촉 압력·긴 몫(ADR-0037): Contact Detail Full이고 거리표가 있을 때만. 아니면 함몰만(Basic — 붙든 칸 목표 = 붙들 깊이).
	// 거리표에 랜드마크가 없으면 압력 패스가 돌지 않으므로 함몰·합치기도 긴 몫을 읽지 않는다(지난 값이 남은 거친 표를 읽지 않게).
	RI.bContactLong = Input.bContactLong && bBulgeTableReady && RI.NumLandmarks > 0 ? 1u : 0u;
	RI.bWallGhost = Input.bWallGhost ? 1u : 0u;
	if (!bDragReady)
	{
		RI.DragRatio = 0.f;
	}
	RI.ContactLong = ContactLong->GameThread_GetRenderTargetResource();
	RI.ContactBend = ContactBend->GameThread_GetRenderTargetResource();
	RI.CoarseInfo = CoarseInfo->GameThread_GetRenderTargetResource();
	RI.CoarseLayer = CoarseLayer->GameThread_GetRenderTargetResource();
	RI.CoarseBlock = (uint32)Block;
	RI.CoarseResolution = (uint32)CoarseResolution;
	RI.CoarseTexelSize = RI.TexelSize * Block;
	// 덩어리 국소 모양(일지 251): 덩어리 힘을 잴 때, 굽기가 이 거친 격자에 줄을 적어 두었을 때만. 줄 번호는 G16이어야 셰이더가 읽는 값이 굽기가
	// 적은 정수다. 에디터에서 막 구워 아직 만드는 중이면 이번 갱신만 덩어리가 출렁임 모양으로 비켜난다.
	RI.LumpRegionIndex = nullptr;
	RI.LumpRegionTable = nullptr;
	RI.bLumpRegion = 0u;
	if (RI.LumpMode == 2 && RI.LumpGroups.Num() > 0 && Data->HasLumpRegions()
		&& Block == Data->BulgeCellBlock)
	{
		const UTexture2D* RegionIndexTex = Data->CarrierLumpRegionIndexTex.Get();
		const UTexture2D* RegionTableTex = Data->CarrierLumpRegionTex.Get();
		bool bRegionReady = RegionIndexTex && RegionTableTex && RegionIndexTex->GetPixelFormat() == PF_G16
			&& RegionIndexTex->GetSizeX() == CoarseResolution && RegionIndexTex->GetSizeY() == CoarseResolution;
#if WITH_EDITOR
		bRegionReady = bRegionReady && !RegionIndexTex->IsCompiling() && !RegionIndexTex->IsDefaultTexture() && !RegionTableTex->IsCompiling()
			&& !RegionTableTex->IsDefaultTexture();
#endif
		if (bRegionReady)
		{
			RI.LumpRegionIndex = ResourceOf(Data->CarrierLumpRegionIndexTex);
			RI.LumpRegionTable = ResourceOf(Data->CarrierLumpRegionTex);
			RI.bLumpRegion = RI.LumpRegionIndex && RI.LumpRegionTable ? 1u : 0u;
		}
	}
	SpreadBlock = Block;
	SpreadTexelSize = RI.CoarseTexelSize;
	SpreadLandmarks = RI.bContactLong ? (int32)RI.NumLandmarks : 0;
	// 끌림 거친 격자 = 솟음 거친 격자(ADR-0033). 끌림 텍스처 둘(끌림 G, 붙든 자리와 끄는 힘 — 일지 184에 하나로 묶음)은 거친 벌에 있다 — 새로 잡은 첫 갱신에
	// 렌더 스레드가 0으로 비우고, 이웃 표는 렌더 스레드가 캐리어에서 만든다.
	RI.DragState = DragState->GameThread_GetRenderTargetResource();
	RI.DragHold = DragHold->GameThread_GetRenderTargetResource();
	RI.DragBlock = (uint32)Block;
	RI.DragResolution = (uint32)CoarseResolution;
	RI.DragTexelSize = RI.CoarseTexelSize;
	// 끌림 폭 ℓ_t = √(h²/3 + L_T·h)(ADR-0037 §3.4) — 두꺼운 살, 겉껍질은 칠 1의 길이(가장 넓은 쪽).
	{
		const double ThickCm = (double)SpreadThicknessHigh * (double)RI.ThicknessScale;
		const double SkinCm = (double)RI.SkinLength * (double)RI.ThicknessScale;
		RI.DragReachMax = (float)FMath::Sqrt(ThickCm * ThickCm / 3.0 + SkinCm * ThickCm);
	}
	// 끌림 마찰 0(또는 접촉 단계 Basic)이면 끌림 패스를 돌리지 않는다 — 솟음 부피 0·덩어리 밀림 0과 같은 규칙(일지 222). 끌림 텍스처는 렌더 스레드가
	// 끄는 순간 0으로 비운다.
	RI.bDragTable = bDragReady && RI.DragRatio > 0.f ? 1u : 0u;
	// 잡기(ADR-0041): Full이면 Drag Friction이 0이어도 잡기 줄이 있는 동안 끌림 패스가 돈다 — 옆으로 끈 피부가 늘어난다(Drag Friction은 잡기에 듣지 않는다).
	// 놓는 중 줄이 1 µm 아래로 비워질 때까지 돌아, 끌림이 꺼지며 지워지는 값은 화면에 보이지 않는다.
	if (bDragReady && RI.bContactLong != 0u && RI.GrabInput.Num() > 0)
	{
		RI.bDragTable = 1u;
	}
	// 함몰 거친 해(일지 199): 거친 칸 표·이웃 표를 끌림과 같이 쓴다(거리표는 쓰지 않는다). 이웃 표의 칸 번호가 20비트라 거친 격자 한 변이
	// MaxDragResolution을 넘는 캐리어는 끈다(목록 밖은 0 — 일지 197까지의 꼴). 텍스처는 거친 벌에 있다.
	RI.DentCoarse = DentCoarse->GameThread_GetRenderTargetResource();
	RI.bDentCoarse = CoarseResolution <= (int32)MaxDragResolution ? 1u : 0u;
	{
		// 거친 패스 수와 체비쇼프 범위: 두꺼운 칸(두께 상위 10%)의 가장 넓은 함몰 폭이 거친 칸 몇 개인가 — 둘레 폭 넓힘의 상한인 끌림 폭(폼의 폭보다
		// 넓다).
		const double CoarseReach = Reach;
		const double CoarseCells = CoarseReach / FMath::Max((double)RI.CoarseTexelSize, 1e-3);
		RI.DentCoarsePasses = (uint32)FMath::Clamp((int32)FMath::CeilToDouble(CoarseCells / DentCoarseCellsPerPass), 1, DentCoarsePassesMax);
		RI.DentCoarseChebyshevLow = StaticChebyshevLowFor(CoarseReach, RI.CoarseTexelSize, DentIterations);
	}
	DragBlock = Block;
	LastResolution = RI.Resolution;
	LastDragResolution = RI.DragResolution;

	// 접촉 패스가 이번 프레임 덩어리 노멀을 쓴다(머티리얼이 SurfaceFieldOn = 2로 읽는다). 미룬 프레임에도 지난 갱신의 덩어리 노멀이
	// 텍스처에 그대로 있다.
	bLumpNormals = RI.LumpMode != 0 && RI.LumpNormal != nullptr;

	// 디버그 캡처(Softbody.CaptureField): 요청이 있으면 이번 갱신 끝에 저장할 장들의 사본을 뜬다. 실제로 돌았는지는 OnScheduled가 안다. 신청에 싣기
	// 바로 앞에서 정한다 — 위에서 일찍 돌아간 틱은 신청이 없는데 "실었다"가 남아, 캡처가 상한 없이 그 장을 기다릴 수 있었다.
	if (bCapturePending)
	{
		PrepareCaptureCopies(RI);
	}
	bCaptureSent = RI.CaptureCopies.Num() > 0;

	// 묶음(ADR-0021): 이 월드의 표면 장들을 한 그래프로 올리는 묶음에 신청한다. 액터 틱이 다 끝난 뒤 묶음이 갱신률 LOD로 "누굴
	// 돌릴지" 고르고 OnScheduled로 알려 준다. 월드 밖에서 만든 표면 장(묶음이 없다)은 혼자 바로 올린다.
	FSoftbodySurfaceSchedule Schedule;
	Schedule.ScreenSize = Input.ScreenSize;
	Schedule.bHasCandidates = bHasShapes;
	Schedule.bInView = bInView;
	Schedule.bNewlyTouched = bNewlyTouched;
	Schedule.bGrabbing = Input.bGrabbing;
	Schedule.FramesSinceUpdate = FramesSinceUpdate;
	Schedule.LastUpdateTime = LastUpdateTime;
	Schedule.RatePhase = RatePhase;
	Schedule.bFirstUpdate = !bRenderStateBuilt;
	// 멈춤(일지 203): 이 신청의 번호와 입력을 적어 둔다 — 실제로 돌면(OnScheduled) 멈춤 비교의 기준이 된다.
	PendingSerial = ++UpdateSerial;
	RI.Serial = PendingSerial;
	PendingInput = Input;
	UWorld* World = GetWorld();
	if (USoftbodySurfaceBatch* Batch = World ? World->GetSubsystem<USoftbodySurfaceBatch>() : nullptr)
	{
		PendingBatch = Batch;
		Batch->Enqueue(this, MoveTemp(RI), Schedule);
		return true;
	}
	ENQUEUE_RENDER_COMMAND(SoftbodySurfaceFieldUpdate)(
		[RI = MoveTemp(RI)](FRHICommandListImmediate& RHICmdList) mutable
		{
			TArray<FSoftbodySurfaceFieldRender> One;
			One.Add(MoveTemp(RI));
			USoftbodySurfaceField::RenderBatch(RHICmdList, One);
		});
	OnScheduled(true);
	return true;
}

float USoftbodySurfaceField::NextRatePhase()
{
	// 황금비 수열: 만든 차례대로 0~1을 고르게 채운다.
	static std::atomic<uint32> Serial{0};
	return (float)FMath::Frac((double)Serial.fetch_add(1, std::memory_order_relaxed) * 0.6180339887498949);
}

void USoftbodySurfaceField::OnScheduled(bool bRan)
{
	// 처음 갱신이 렌더 스레드 상태(표들)를 만들었다 — 다음부터는 처음 갱신 상한(softbody.Surface.FirstUpdates)에 들지 않는다.
	bRenderStateBuilt |= bRan;
	PendingBatch.Reset();
	if (bRan)
	{
		PendingDeltaTime = 0.f;
		FramesSinceUpdate = 0;
		// 멈춤(일지 203): 이 갱신의 입력이 다음 비교의 기준.
		LastRunSerial = PendingSerial;
		LastRunInput = MoveTemp(PendingInput);
		bHasLastRunInput = true;
		if (const UWorld* World = GetWorld())
		{
			LastUpdateTime = World->GetTimeSeconds();
		}
		// 실제로 돌렸다: 이제 장에 값이 있고(다음 갱신은 지난 위치를 쓰고 쉬기 상태를 이어 센다) 실어 보낸 거리장 칸 값이 GPU에 있다.
		bCleared = false;
		// 콜라이더가 떠난 뒤의 갱신이면 이 걸음이 줄인 몫을 센다(화면에 없는 장은 Update가 틱마다 센다).
		if (!bHadCandidates && bInView)
		{
			IdleSettle += StepSettle;
		}
		FieldVoxelUploadedVersion = FieldVoxelSentVersion;
	}
	else
	{
		// 미룬 프레임: 그 시간은 다음 갱신이 한 걸음에 푼다(Update의 StepTime). 장은 그대로라 bCleared·거리장 칸 값도 그대로다.
		PendingDeltaTime += LastDeltaTime;
		++FramesSinceUpdate;
	}
	if (bCaptureSent)
	{
		// 디버그 캡처: 돌린 프레임에만 사본이 복사됐다. 미뤘으면 다음 신청에 다시 싣는다. 사본을 뜬 갱신의 번호·프레임과 그 갱신의 거친 격자 값을 적어 둔다
		// — 저장은 보통 다음 틱이라, 그때의 거친 격자 값은 다음 신청의 것이다.
		if (bRan)
		{
			bCaptureCopied = true;
			bCapturePending = false;
			CaptureSerial = LastRunSerial;
			CaptureFrame = (uint64)GFrameCounter;
			CaptureDragBlock = DragBlock;
			CaptureSpreadBlock = SpreadBlock;
			CaptureSpreadTexelSize = SpreadTexelSize;
			CaptureSpreadLandmarks = SpreadLandmarks;
			CaptureSettings = CaptureSettingsLine(GetOuter());
			CaptureShapes = LastRunInput.Shapes;
			CapturePlanes = LastRunInput.Planes;
			CaptureFieldInstances = LastRunInput.FieldInstances;
			CaptureVoxelWords = FieldVoxelWords.Num();
		}
		bCaptureSent = false;
	}
	if (USoftbodyComponent* Owner = Cast<USoftbodyComponent>(GetOuter()))
	{
		Owner->OnSurfaceFieldScheduled(bRan);
	}
}

void USoftbodySurfaceField::OnStill()
{
	// 멈춘 틱(일지 203): 미룬 틱과 같이 다룬다 — 장은 그대로, 시간은 모으지 않는다(평형). 소유 컴포넌트는 지난 변위를 지금 변위로 묶고(속도 0) 끌림의 지난
	// 위치를 마지막 갱신 그대로 둔다(깨는 갱신이 그동안의 움직임을 한 번에 본다).
	bStill = true;
	PendingBatch.Reset();
	FramesSinceUpdate = FMath::Min(FramesSinceUpdate + 1, 1 << 20);
	if (USoftbodyComponent* Owner = Cast<USoftbodyComponent>(GetOuter()))
	{
		Owner->OnSurfaceFieldStill();
	}
}

bool USoftbodySurfaceField::InputStill(const FSoftbodySurfaceFieldInput& A, const FSoftbodySurfaceFieldInput& B, float Distance)
{
	// 멈춤(일지 203): 결과를 바꿀 수 있는 입력이 모두 허용 오차 안인가. 거리(cm)는 Distance, 방향·회전 성분은 바운드 반지름 R 안의 어느 점도 Distance보다
	// 더 움직이지 않는 값(행렬 열·노멀은 Distance / R, 쿼터니언 성분은 그 반 — 성분 차 ≈ 각도의 절반). 조절값·구성(개수·종류·상수)은 꼭 같아야 한다.
	const float R = FMath::Max(A.BoundsRadius, 1.f);
	const float Dir = Distance / R;
	const float Quat = 0.5f * Dir;
	if (A.Shapes.Num() != B.Shapes.Num() || A.Planes.Num() != B.Planes.Num() || A.Bones.Num() != B.Bones.Num()
		|| A.FieldInstances.Num() != B.FieldInstances.Num() || A.LumpGroups.Num() != B.LumpGroups.Num()
		|| A.InertiaBridge.Num() != B.InertiaBridge.Num() || A.Grabs.Num() != B.Grabs.Num())
	{
		return false;
	}
	if (A.ComponentScale != B.ComponentScale || A.MaxContactDepth != B.MaxContactDepth
		|| A.EdgeTrim != B.EdgeTrim || A.bContactLong != B.bContactLong || A.bWallGhost != B.bWallGhost || A.OcclusionStrength != B.OcclusionStrength
		|| A.RecoveryRate != B.RecoveryRate || A.MemoryTime != B.MemoryTime || A.MemoryFadeTime != B.MemoryFadeTime
		|| A.DragRatio != B.DragRatio
		|| A.bLumpForce != B.bLumpForce || A.LumpGrip != B.LumpGrip || A.LumpStiffen != B.LumpStiffen
		|| A.VolumeOffset != B.VolumeOffset || A.FlexOffset != B.FlexOffset || A.SkinTension != B.SkinTension || A.BakedVolumePreservation != B.BakedVolumePreservation)
	{
		return false;
	}
	// 프레임의 회전·원점(중력 방향과 구동점 자리가 바뀐다).
	if (FMath::Abs(A.ComponentRotation.X - B.ComponentRotation.X) > Quat || FMath::Abs(A.ComponentRotation.Y - B.ComponentRotation.Y) > Quat
		|| FMath::Abs(A.ComponentRotation.Z - B.ComponentRotation.Z) > Quat || FMath::Abs(A.ComponentRotation.W - B.ComponentRotation.W) > Quat
		|| (A.FrameToActor - B.FrameToActor).GetAbsMax() > Distance)
	{
		return false;
	}
	// 줄마다 성분별 허용 오차(0 = 꼭 같아야 한다). Period줄이 한 단위.
	const auto RowsStill = [](const TArray<FVector4f>& X, const TArray<FVector4f>& Y, int32 Period, const float* Tolerance)
	{
		for (int32 Row = 0; Row < X.Num(); ++Row)
		{
			const float* Tol = Tolerance + (Row % Period) * 4;
			const FVector4f& P = X[Row];
			const FVector4f& Q = Y[Row];
			if (FMath::Abs(P.X - Q.X) > Tol[0] || FMath::Abs(P.Y - Q.Y) > Tol[1] || FMath::Abs(P.Z - Q.Z) > Tol[2] || FMath::Abs(P.W - Q.W) > Tol[3])
			{
				return false;
			}
		}
		return true;
	};
	const float D = Distance;
	// 형상: (중심, 종류), 회전 쿼터니언, (크기 — 컨벡스는 평면 시작·개수).
	const float ShapeTol[12] = {D, D, D, 0.f, Quat, Quat, Quat, Quat, D, D, D, 0.f};
	// 컨벡스 평면: 노멀, 거리.
	const float PlaneTol[4] = {Dir, Dir, Dir, D};
	// 본 스키닝 행렬 열 셋: 선형부, 이동.
	const float ColumnTol[12] = {Dir, Dir, Dir, D, Dir, Dir, Dir, D, Dir, Dir, Dir, D};
	// 모양 거리장 부위: 행렬 열 셋, 그 뒤 칸 표(꼭 같아야 한다).
	const float FieldTol[24] = {Dir, Dir, Dir, D, Dir, Dir, Dir, D, Dir, Dir, Dir, D,
		0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
	// 덩어리 평형: 상수 두 줄은 꼭 같아야, 관성 목표(g)는 흔들림 25 cm/g에서 0.025 cm가 되는 값.
	constexpr float LumpTargetTolerance = 1e-3f;
	const float LumpTol[12] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, LumpTargetTolerance, LumpTargetTolerance, LumpTargetTolerance, 0.f};
	// 관성 브릿지(흔들림 cm·구동점 위치·회전): 거리로 본다.
	const float BridgeTol[4] = {D, D, D, D};
	// 잡기 줄(ADR-0041): 끄는 자리는 거리로, 반지름·잡는 점·번호·표지는 꼭 같아야 한다(번호·표지는 float 비트라 0 허용 오차로 견준다).
	const float GrabTol[12] = {D, D, D, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
	return RowsStill(A.Shapes, B.Shapes, 3, ShapeTol) && RowsStill(A.Planes, B.Planes, 1, PlaneTol) && RowsStill(A.Bones, B.Bones, 3, ColumnTol)
		&& RowsStill(A.FieldInstances, B.FieldInstances, SoftbodySurfaceFieldRows::PerInstance, FieldTol)
		&& RowsStill(A.LumpGroups, B.LumpGroups, 3, LumpTol) && RowsStill(A.InertiaBridge, B.InertiaBridge, 1, BridgeTol)
		&& RowsStill(A.Grabs, B.Grabs, SoftbodyGrab::InputRows, GrabTol);
}

TObjectPtr<UTextureRenderTarget2D>* FSoftbodySurfaceTextures::Slot(int32 Index)
{
	TObjectPtr<UTextureRenderTarget2D>* Slots[] = {&StateA, &Hold, &Position, &Displacement, &PreviousDisplacement, &Normal, &Accepted, &Labels, &LumpNormal,
		&LumpAnchor};
	return Index >= 0 && Index < UE_ARRAY_COUNT(Slots) ? Slots[Index] : nullptr;
}

static void FineTargetSpec(int32 Index, const TCHAR*& OutName, ETextureRenderTargetFormat& OutFormat)
{
	// 함몰 상태는 32비트 한 채널(끌림은 거친 격자 DragState, 일지 161). 누른 자리 기억(ADR-0032)도 32비트 한 채널 — 하프면 느린 복원율에서 줄임 한 번이
	// 반올림에 묻혀 빠르기가 자리값마다 달라진다. 이완 패스가 제자리에서 읽고 쓰는 받아들인 깊이도 UAV 읽기가 모든 GPU에서 되는 32비트 한 채널.
	// 덩어리 노멀(ADR-0018)과 손 닻(일지 201, 손 움직임을 프레임마다 더해 쌓는 값이라 32비트 두 채널)은 덩어리 값이 있는 캐리어만.
	struct FSpec
	{
		const TCHAR* Name;
		ETextureRenderTargetFormat Format;
	};
	static const FSpec Specs[] = {
		{TEXT("SoftbodySurfaceStateA"), RTF_R32f},
		{TEXT("SoftbodySurfaceHold"), RTF_R32f},
		{TEXT("SoftbodySurfacePosition"), RTF_RGBA32f},
		{TEXT("SoftbodySurfaceDisplacement"), RTF_RGBA16f},
		{TEXT("SoftbodySurfacePreviousDisplacement"), RTF_RGBA16f},
		{TEXT("SoftbodySurfaceNormal"), RTF_RGBA16f},
		{TEXT("SoftbodySurfaceAccepted"), RTF_R32f},
		// 들어온 면 표지(일지 313): 접촉 패스가 칸마다 제자리에서 읽고 쓴다 — 받아들인 깊이처럼 UAV 읽기가 모든 GPU에서 되는 32비트 한 채널.
		{TEXT("SoftbodySurfaceLabels"), RTF_R32f},
		{TEXT("SoftbodySurfaceLumpNormal"), RTF_RGBA16f},
		{TEXT("SoftbodySurfaceLumpAnchor"), RTF_RG32f},
	};
	check(Index >= 0 && Index < UE_ARRAY_COUNT(Specs));
	OutName = Specs[Index].Name;
	OutFormat = Specs[Index].Format;
}

int64 FSoftbodySurfaceTextures::CreateNextTarget(UObject* Outer)
{
	for (int32 Index = 0; Index < NumTargets(bLump); ++Index)
	{
		TObjectPtr<UTextureRenderTarget2D>* Target = Slot(Index);
		if (!*Target)
		{
			const TCHAR* Name;
			ETextureRenderTargetFormat Format;
			FineTargetSpec(Index, Name, Format);
			// 만들 때 0으로 비운다(CreateFieldTarget) — 표면 장은 "모든 장이 0"에서 시작한다(bMotionHistory 0).
			*Target = CreateFieldTarget(Outer, Resolution, Name, Format);
			return FieldTargetBytes(*Target);
		}
	}
	return 0;
}

bool FSoftbodySurfaceTextures::IsComplete() const
{
	if (Resolution <= 0)
	{
		return false;
	}
	for (int32 Index = 0; Index < NumTargets(bLump); ++Index)
	{
		if (!*Slot(Index))
		{
			return false;
		}
	}
	return true;
}

FSoftbodySurfaceTextures FSoftbodySurfaceTextures::Create(UObject* Outer, int32 Resolution, bool bLump)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyCreateTextureSet);
	FSoftbodySurfaceTextures Set;
	Set.Resolution = Resolution;
	Set.bLump = bLump;
	while (Set.CreateNextTarget(Outer) > 0)
	{
	}
	return Set;
}

int64 FSoftbodySurfaceTextures::TargetBytes(const UTextureRenderTarget2D* Target)
{
	return FieldTargetBytes(Target);
}

void FSoftbodySurfaceTextures::ReleaseTarget(UTextureRenderTarget2D* Target)
{
	ReleaseFieldTarget(Target);
}

void FSoftbodySurfaceTextures::Clear() const
{
	TArray<TObjectPtr<UTextureRenderTarget2D>, TInlineAllocator<9>> Targets;
	for (int32 Index = 0; Index < NumTargets(true); ++Index)
	{
		if (UTextureRenderTarget2D* Target = *Slot(Index))
		{
			Targets.Add(Target);
		}
	}
	ClearTargets(Targets);
}

void FSoftbodySurfaceTextures::ClearTargets(TConstArrayView<TObjectPtr<UTextureRenderTarget2D>> InTargets)
{
	// 엔진이 끝나는 중이면(PIE 종료·에디터 종료의 등록 해제) 렌더 자원이 먼저 사라질 수 있다 — 비울 필요도 없다.
	if (IsEngineExitRequested())
	{
		return;
	}
	TArray<FTextureRenderTargetResource*> Targets;
	Targets.Reserve(InTargets.Num());
	for (UTextureRenderTarget2D* Target : InTargets)
	{
		if (FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr)
		{
			Targets.Add(Resource);
		}
	}
	if (Targets.Num() == 0)
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(SoftbodySurfaceFieldClear)(
		[Targets = MoveTemp(Targets)](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
			for (FTextureRenderTargetResource* Target : Targets)
			{
				FRHITexture* Texture = Target->GetRenderTargetTexture();
				if (!Texture)
				{
					continue;
				}
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RegisterExternalTexture(GraphBuilder, Texture, TEXT("SoftbodySurfaceClear"))), Zero);
			}
			GraphBuilder.Execute();
		});
}

void FSoftbodySurfaceTextures::Destroy()
{
	for (int32 Index = 0; Index < NumTargets(true); ++Index)
	{
		TObjectPtr<UTextureRenderTarget2D>* Target = Slot(Index);
		if (*Target)
		{
			// GC를 기다리지 않고 GPU 메모리를 바로 돌려준다. 오브젝트는 참조만 놓고 GC에 맡긴다 — 버렸다고 표시하면(MarkAsGarbage)
			// 아직 가리키는 쪽이 있어도 GC가 지워 버려, 머티리얼의 렌더 스레드 사본에 죽은 포인터가 남는다(크래시).
			ReleaseFieldTarget(*Target);
			*Target = nullptr;
		}
	}
}

int64 FSoftbodySurfaceTextures::Bytes() const
{
	int64 Total = 0;
	for (int32 Index = 0; Index < NumTargets(true); ++Index)
	{
		Total += FieldTargetBytes(*Slot(Index));
	}
	return Total;
}

TObjectPtr<UTextureRenderTarget2D>* FSoftbodySurfaceCoarseTextures::Slot(int32 Index)
{
	TObjectPtr<UTextureRenderTarget2D>* Slots[] = {&ContactLong, &CoarseInfo, &CoarseLayer, &DragState, &DragHold, &DentCoarse, &ContactBend};
	return Index >= 0 && Index < UE_ARRAY_COUNT(Slots) ? Slots[Index] : nullptr;
}

static void CoarseTargetSpec(int32 Index, const TCHAR*& OutName, ETextureRenderTargetFormat& OutFormat)
{
	struct FSpec
	{
		const TCHAR* Name;
		ETextureRenderTargetFormat Format;
	};
	static const FSpec Specs[] = {
		{TEXT("SoftbodySurfaceContactLong"), RTF_RG32f},
		{TEXT("SoftbodySurfaceCoarseInfo"), RTF_RGBA32f},
		{TEXT("SoftbodySurfaceCoarseLayer"), RTF_RGBA32f},
		{TEXT("SoftbodySurfaceDragState"), RTF_RGBA32f},
		{TEXT("SoftbodySurfaceDragHold"), RTF_RGBA32f},
		{TEXT("SoftbodySurfaceDentCoarse"), RTF_R32f},
		{TEXT("SoftbodySurfaceContactBend"), RTF_R32f},
	};
	check(Index >= 0 && Index < UE_ARRAY_COUNT(Specs));
	OutName = Specs[Index].Name;
	OutFormat = Specs[Index].Format;
}

int64 FSoftbodySurfaceCoarseTextures::CreateNextTarget(UObject* Outer)
{
	for (int32 Index = 0; Index < NumTargets(); ++Index)
	{
		TObjectPtr<UTextureRenderTarget2D>* Target = Slot(Index);
		if (!*Target)
		{
			const TCHAR* Name;
			ETextureRenderTargetFormat Format;
			CoarseTargetSpec(Index, Name, Format);
			*Target = CreateFieldTarget(Outer, Resolution, Name, Format);
			return FieldTargetBytes(*Target);
		}
	}
	return 0;
}

bool FSoftbodySurfaceCoarseTextures::IsComplete() const
{
	if (Resolution <= 0)
	{
		return false;
	}
	for (int32 Index = 0; Index < NumTargets(); ++Index)
	{
		if (!*Slot(Index))
		{
			return false;
		}
	}
	return true;
}

FSoftbodySurfaceCoarseTextures FSoftbodySurfaceCoarseTextures::Create(UObject* Outer, int32 Resolution)
{
	FSoftbodySurfaceCoarseTextures Set;
	Set.Resolution = Resolution;
	while (Set.CreateNextTarget(Outer) > 0)
	{
	}
	return Set;
}

void FSoftbodySurfaceCoarseTextures::Destroy()
{
	for (int32 Index = 0; Index < NumTargets(); ++Index)
	{
		TObjectPtr<UTextureRenderTarget2D>* Target = Slot(Index);
		if (*Target)
		{
			ReleaseFieldTarget(*Target);
			*Target = nullptr;
		}
	}
}

int64 FSoftbodySurfaceCoarseTextures::Bytes() const
{
	int64 Total = 0;
	for (int32 Index = 0; Index < NumTargets(); ++Index)
	{
		Total += FieldTargetBytes(*Slot(Index));
	}
	return Total;
}

bool USoftbodySurfaceField::AcquireTextures(int32 CoarseResolution)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SoftbodyFieldAcquireTextures);
	if (HasTextures())
	{
		return true;
	}
	if (!Data || CoarseResolution <= 0)
	{
		return false;
	}
	const int32 Resolution = Data->CarrierResolution;
	const bool bLump = !Data->CarrierLumpTex.IsNull();
	FSoftbodySurfaceTextures Set;
	FSoftbodySurfaceCoarseTextures Coarse;
	if (USoftbodySurfaceBatch* Batch = Pool.Get())
	{
		// 풀에 쉬는 벌이 없으면 이번 프레임은 돌지 않는다 — 풀이 프레임 예산 안에서 만들어 두면 다음 틱에 잡는다(일지 235). 그 자리에서 만들지 않는다.
		if (!Batch->TryAcquire(Resolution, bLump, CoarseResolution, Set, Coarse))
		{
			return false;
		}
	}
	else
	{
		// 월드 밖(묶음이 없다): 제 텍스처를 바로 만든다.
		Set = FSoftbodySurfaceTextures::Create(this, Resolution, bLump);
		Coarse = FSoftbodySurfaceCoarseTextures::Create(this, CoarseResolution);
	}
	StateA = Set.StateA;
	Hold = Set.Hold;
	Position = Set.Position;
	Displacement = Set.Displacement;
	PreviousDisplacement = Set.PreviousDisplacement;
	Normal = Set.Normal;
	Accepted = Set.Accepted;
	Labels = Set.Labels;
	LumpNormal = Set.LumpNormal;
	LumpAnchor = Set.LumpAnchor;
	ContactLong = Coarse.ContactLong;
	CoarseInfo = Coarse.CoarseInfo;
	CoarseLayer = Coarse.CoarseLayer;
	DragState = Coarse.DragState;
	DragHold = Coarse.DragHold;
	DentCoarse = Coarse.DentCoarse;
	ContactBend = Coarse.ContactBend;
	// 머티리얼이 이 벌을 읽게 잇는다(지난 변위 파라미터 포함).
	if (USoftbodyComponent* Owner = Cast<USoftbodyComponent>(GetOuter()))
	{
		Owner->BindSurfaceFieldTextures(true);
	}
	return true;
}

void USoftbodySurfaceField::ReleaseTextures()
{
	// 멈춤(일지 203): 벌을 돌려주면 지난 갱신의 입력도 뜻이 없다 — 다음 닿는 순간은 돌아야 한다.
	bHasLastRunInput = false;
	StillSinceSerial = 0;
	bStill = false;
	if (!HasTextures())
	{
		return;
	}
	// 이번 프레임 묶음에 넣은 신청이 있으면 먼저 뺀다. 신청은 이 벌의 렌더 자원을 그대로 들고 있어서, 남겨 두면 틱 뒤 묶음이
	// 풀에서 지운 타깃으로 돌거나, 0으로 비워 돌려준 벌에 이 인스턴스의 함몰을 다시 써서 다음에 그 벌을 받은
	// 인스턴스에 남의 자국이 보인다. 컴포넌트가 이번 프레임 틱을 마친 뒤 같은 프레임에 지워지거나 접촉을 끌 때 생긴다.
	if (USoftbodySurfaceBatch* Batch = PendingBatch.Get())
	{
		Batch->Cancel(this);
		PendingBatch.Reset();
		bCaptureSent = false;
	}
	// 머티리얼이 타깃을 붙들지 않게 먼저 푼다 — 풀이 다른 인스턴스에 줄 수 있고, 지울 때 메모리가 돌아와야 한다.
	if (USoftbodyComponent* Owner = Cast<USoftbodyComponent>(GetOuter()))
	{
		Owner->BindSurfaceFieldTextures(false);
	}
	FSoftbodySurfaceTextures Set;
	Set.Resolution = StateA->SizeX;
	Set.bLump = LumpNormal != nullptr;
	Set.StateA = StateA;
	Set.Hold = Hold;
	Set.Position = Position;
	Set.Displacement = Displacement;
	Set.PreviousDisplacement = PreviousDisplacement;
	Set.Normal = Normal;
	Set.Accepted = Accepted;
	Set.Labels = Labels;
	Set.LumpNormal = LumpNormal;
	Set.LumpAnchor = LumpAnchor;
	FSoftbodySurfaceCoarseTextures Coarse;
	Coarse.Resolution = ContactLong ? ContactLong->SizeX : 0;
	Coarse.ContactLong = ContactLong;
	Coarse.CoarseInfo = CoarseInfo;
	Coarse.CoarseLayer = CoarseLayer;
	Coarse.DragState = DragState;
	Coarse.DragHold = DragHold;
	Coarse.DentCoarse = DentCoarse;
	Coarse.ContactBend = ContactBend;
	for (TObjectPtr<UTextureRenderTarget2D>* Target : {&StateA, &Hold, &Position, &Displacement, &PreviousDisplacement, &Normal, &Accepted, &Labels,
		&LumpNormal, &LumpAnchor, &ContactLong, &CoarseInfo, &CoarseLayer, &DragState, &DragHold, &DentCoarse, &ContactBend})
	{
		*Target = nullptr;
	}
	if (USoftbodySurfaceBatch* Batch = Pool.Get())
	{
		// 가는 벌은 풀이 0으로 비워 둔다: 다음에 쓰는 표면 장(이 인스턴스가 다시 닿을 때 포함)이 "모든 장이 0"에서 시작한다. 거친 벌은 다음에 잡는 장의 첫
		// 갱신이 비우고 고정 표를 다시 만든다(일지 235).
		Batch->Release(MoveTemp(Set));
		if (Coarse.IsComplete())
		{
			Batch->ReleaseCoarse(MoveTemp(Coarse));
		}
		else
		{
			Coarse.Destroy();
		}
		return;
	}
	// 풀이 없으면(월드 밖) GPU 메모리를 바로 놓는다.
	Set.Destroy();
	Coarse.Destroy();
}

void USoftbodySurfaceField::RequestCapture()
{
	if (!Data)
	{
		return;
	}
	// 사본 타깃은 신청에 실을 때(PrepareCaptureCopies) 그때 장과 같은 형식·크기로 만든다 — 지금은 장이 아직 텍스처를 잡지 않았을 수 있다.
	bCapturePending = true;
	bCaptureCopied = false;
	CaptureSerial = 0;
	CaptureFrame = 0;
	CaptureSettings.Reset();
}

void USoftbodySurfaceField::PrepareCaptureCopies(FSoftbodySurfaceFieldRender& RI)
{
	// 사본은 원본과 형식·크기가 같아야 통째로 복사된다. 장이 텍스처를 새로 잡았거나 거친 격자 크기가 바뀌었으면 다시 만든다 — 요청한 동안에만 만드는
	// 디버그 타깃이라 그 틱의 멈칫은 받아들인다(요청이 없으면 아무것도 하지 않는다). 접촉은 갱신 안 임시 텍스처(가는 격자, 반정밀 RGBA — RenderBatch의
	// 임시 텍스처 형식)라 원본 타깃이 없다.
	const UTextureRenderTarget2D* Sources[] = {StateA.Get(), nullptr, Position.Get(), Displacement.Get(), Normal.Get(), Accepted.Get(), Hold.Get(),
		Labels.Get(), DragState.Get(), ContactLong.Get(), ContactBend.Get()};
	static_assert(UE_ARRAY_COUNT(Sources) == (SIZE_T)SoftbodyCapture::NumCopies, "capture copies follow SoftbodyCapture::ECopy");
	CaptureCopies.SetNum(SoftbodyCapture::NumCopies);
	RI.CaptureCopies.SetNumZeroed(SoftbodyCapture::NumCopies);
	for (int32 Index = 0; Index < SoftbodyCapture::NumCopies; ++Index)
	{
		const UTextureRenderTarget2D* Source = Sources[Index];
		const int32 Size = Index == SoftbodyCapture::Contact ? (int32)RI.Resolution : (Source ? Source->SizeX : 0);
		const ETextureRenderTargetFormat Format = Source ? (ETextureRenderTargetFormat)Source->RenderTargetFormat : RTF_RGBA16f;
		if (Size <= 0)
		{
			continue;
		}
		TObjectPtr<UTextureRenderTarget2D>& Copy = CaptureCopies[Index];
		if (!Copy || Copy->SizeX != Size || (ETextureRenderTargetFormat)Copy->RenderTargetFormat != Format)
		{
			if (Copy)
			{
				Copy->ReleaseResource();
				Copy->MarkAsGarbage();
			}
			Copy = CreateFieldTarget(this, Size, TEXT("SoftbodySurfaceCapture"), Format);
		}
		RI.CaptureCopies[Index] = Copy->GameThread_GetRenderTargetResource();
	}
	// 접촉 압력 풀이(풀이 줄·판 상태·수렴 기록)도 같은 갱신에서 읽는다. 읽기 칸의 GPU 자원은 그 갱신의 렌더 스레드가 처음 쓸 때 만든다(요청한 동안만 있다).
	if (!PressureCapture.IsValid())
	{
		PressureCapture = MakeShared<FSoftbodyPressureCapture, ESPMode::ThreadSafe>();
	}
	RI.PressureCapture = PressureCapture;
}

void USoftbodySurfaceField::ReleaseCapture()
{
	for (TObjectPtr<UTextureRenderTarget2D>& Copy : CaptureCopies)
	{
		if (Copy)
		{
			Copy->ReleaseResource();
			Copy->MarkAsGarbage();
		}
	}
	CaptureCopies.Empty();
	CaptureSettings.Reset();
	// 압력 읽기 칸은 렌더 스레드에서 놓는다(렌더 명령은 차례로 돌아, 앞서 올린 갱신과 읽기가 다 쓴 뒤에 놓인다).
	if (PressureCapture.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftbodyPressureCaptureRelease)(
			[Capture = MoveTemp(PressureCapture)](FRHICommandListImmediate&) mutable
			{
				Capture.Reset();
			});
	}
	bCapturePending = false;
	bCaptureSent = false;
	bCaptureCopied = false;
}

bool USoftbodySurfaceField::ReadPressureCapture(FSoftbodyPressureCaptureData& Out) const
{
	Out = FSoftbodyPressureCaptureData();
	if (!bCaptureCopied || !PressureCapture.IsValid())
	{
		return false;
	}
	// 사본을 뜬 갱신은 이 명령보다 먼저 렌더 스레드에 올라갔다(렌더 명령은 차례로 돈다). 기다림은 디버그 저장에서만 생긴다.
	ENQUEUE_RENDER_COMMAND(SoftbodyPressureCaptureRead)(
		[Capture = PressureCapture, Target = &Out](FRHICommandListImmediate& RHICmdList)
		{
			Capture->Resolve(RHICmdList, *Target);
		});
	FlushRenderingCommands();
	return Out.Header.Num() > 0;
}

void USoftbodySurfaceField::ReadPipelineStateForCapture(bool& bOutContactReady, bool& bOutHardReady)
{
	// 읽기만 한다 — 캡처가 미리 만들기를 시작하거나 데우기를 밀면 재는 것이 재이는 것을 바꾼다.
	bOutContactReady = GPipelinesReady.load(std::memory_order_acquire);
	bOutHardReady = GHardPipelinesReady.load(std::memory_order_acquire);
}

namespace
{
	/**
	 * 표면 장 하나의 이번 프레임 RDG 손잡이(ADR-0021). 단계 함수들이 앞 단계가 만든 손잡이를 여기서 찾는다. 묶음은 인스턴스마다
	 * 패스들을 차례로 넣는다(ADR-0022: 갱신 안에서만 쓰는 임시 텍스처를 인스턴스끼리 돌려 쓰려고). 인스턴스끼리 공유하는 버퍼가
	 * 없어서 어떤 순서로 넣든 결과는 같다.
	 */
	struct FSurfaceFrame
	{
		const FSoftbodySurfaceFieldRender* RI = nullptr;
		FGlobalShaderMap* ShaderMap = nullptr;
		// 함몰(ADR-0032): A = 갱신을 넘어 남는 결과, B = 첫 패스 결과, C = 붙들 깊이, D = 둘째 패스 결과(옮기기가 A로), 기억 F.
		FRDGTextureRef TargetA = nullptr;
		FRDGTextureRef TargetB = nullptr;
		FRDGTextureRef TargetC = nullptr;
		FRDGTextureRef TargetD = nullptr;
		FRDGTextureRef HoldTarget = nullptr;
		/** 끌림 반복이 쓰지 않는 함몰 출력 자리(한 칸). */
		FRDGTextureRef DentUnused = nullptr;
		/** 함몰 둘째 패스가 쓰지 않는 붙든 칸 (목표, 깊이) 자리(한 칸, RG32F). */
		FRDGTextureRef PinsUnused = nullptr;
		FRDGTextureRef ContactTarget = nullptr;
		FRDGTextureRef EdgeTarget = nullptr;
		FRDGTextureRef PositionTarget = nullptr;
		FRDGTextureRef DisplacementTarget = nullptr;
		FRDGTextureRef NormalTarget = nullptr;
		FRDGTextureRef AcceptedTarget = nullptr;
		/** 들어온 면 표지(일지 313): 갱신을 넘어 남는 표지와 콜라이더 고정 번호. */
		FRDGTextureRef LabelsTarget = nullptr;
		FRDGBufferRef ColliderIdBuffer = nullptr;
		FRDGTextureRef LumpDepthTarget = nullptr;
		FRDGTextureRef LumpNormalTarget = nullptr;
		/** 손 닻(일지 201, 갱신을 넘어 남는다)과 그 자리에 묶는 한 칸짜리 빈 텍스처(덩어리 힘 패스가 걸음마다 읽기·쓰기를 바꿔 묶는다). */
		FRDGTextureRef LumpAnchorTarget = nullptr;
		FRDGTextureRef LumpAnchorUnused = nullptr;
		/**
		 * 읽기만 하고 값은 쓰지 않는 자리에 묶는 엔진의 검은 텍스처(일지 234). 아무도 쓰지 않은 그래프 텍스처를 읽기로 묶으면 렌더 그래프 검증이
		 * 걸려(개발 빌드에서 처음 누를 때 ensure와 약 1.5초 멈춤) 쓰기 전에 읽는 자리는 이것으로 바꾼다. 패스는 늘지 않는다.
		 */
		FRDGTextureRef ReadDummy = nullptr;
		FRDGTextureRef PreviousDisplacementTarget = nullptr;
		// 접촉 압력(ADR-0037): 거친 칸마다 (긴 몫 L, 거친 압력 p) — 갱신을 넘어 남는 거친 벌 텍스처(Full 갱신마다 모든 칸에 새로 쓴다).
		FRDGTextureRef ContactLongTarget = nullptr;
		FRDGTextureRef CoarseInfo = nullptr;
		FRDGTextureRef CoarseLayer = nullptr;
		FRDGTextureRef CoarseRow = nullptr;
		FRDGTextureRef CoarseMaterial = nullptr;
		/** 거친 칸 흐린 겉 굽힘 길이(cm, R16F — 연속 굽힘, FSoftbodySurfaceRestState::CoarseBend). */
		FRDGTextureRef CoarseBend = nullptr;
		/** 연속 굽힘: 굽힘 거친 해 W_c(거친 벌 텍스처)와 자산 거친 칸 굽힘 길이 최대(1 낱말, FSoftbodySurfaceRestState::CoarseBendMax). */
		FRDGTextureRef ContactBendTarget = nullptr;
		FRDGBufferRef CoarseBendMax = nullptr;
		FRHITexture* LandmarkDistanceRHI = nullptr;
		FRDGBufferRef ShapeBuffer = nullptr;
		FRDGBufferRef BoneBuffer = nullptr;
		FRDGBufferRef PreviousBoneBuffer = nullptr;
		FRDGBufferRef PlaneBuffer = nullptr;
		FRDGBufferRef FieldInstanceBuffer = nullptr;
		FRDGBufferRef ShapeMotionBuffer = nullptr;
		FRDGBufferRef FieldMotionBuffer = nullptr;
		FRDGBufferRef VoxelBuffer = nullptr;
		FRHITexture* CarrierPositionRHI = nullptr;
		FRHITexture* CarrierNormalRHI = nullptr;
		FRHITexture* RedirectRHI = nullptr;
		FRHITexture* CarrierLumpRHI = nullptr;
		FRHITexture* CarrierVolumeRHI = nullptr;
		FRHITexture* CarrierColumnRHI = nullptr;
		/** 하드 서피스 순열을 쓰는가(ADR-0037 §3.7)와 떠 있는 조각 밑 칸의 접촉 시작점 올림(없으면 null). */
		bool bHardSurface = false;
		/** 조각 없이 연속 굽힘만으로 하드 서피스(Flex) 순열을 쓰는 갱신 — 빈 조각 표(조각 수 0)를 묶는다. */
		bool bFlexOnly = false;
		FRHITexture* CarrierLiftRHI = nullptr;
		/** 하드 서피스: 칸 조각 번호, 조각 틀·움직임·함몰 묶음 목록(순열 1에서만 만든다 — 조각이 없는 자산은 null). */
		FRHITexture* CarrierPieceRHI = nullptr;
		/** 칸마다 Softness 칠(가는 창 판 용수철 — 없으면 null, 셰이더는 순열 1에서만 읽는다). */
		FRHITexture* CarrierSoftnessRHI = nullptr;
		FRDGBufferSRVRef HardPieceFramesSRV = nullptr;
		FRDGBufferSRVRef HardPieceMotionSRV = nullptr;
		FRDGBufferSRVRef HardTileRangeSRV = nullptr;
		FRDGBufferSRVRef HardTileListSRV = nullptr;
		FRDGBufferSRVRef HardCellRangeSRV = nullptr;
		FRDGBufferSRVRef HardCellListSRV = nullptr;
		/** 조각 움직임(압력 풀이 꼬리가 쓴다 — 풀이가 돌지 않은 갱신은 0, 모든 조각이 꺼져 살처럼 눌린다). */
		FRDGBufferRef HardPieceMotion = nullptr;
		/** 발자국 칸 판 자리 표(갱신을 넘어 남는 버퍼, FSoftbodySurfaceRestState::HardCellPlate). */
		FRDGBufferSRVRef HardCellPlateSRV = nullptr;
		/** 1 = Full(압력 풀이가 판을 풀고 함몰이 판에 붙는다), 0 = Basic(판 항 없음 — 조각이 살처럼 눌린다, ADR-0038 §4). */
		uint32 HardMode = 0;
		uint32 HardPieceCount = 0;
		FIntVector Groups = FIntVector::ZeroValue;
		uint32 TilesPerSide = 0;
		uint32 NumTiles = 0;
		int32 TileReach = 0;
		uint32 ArgsStride = 0;
		FRDGBufferRef Activity = nullptr;
		FRDGBufferUAVRef ActivityUAV = nullptr;
		/** 이완 묶음 눌림 표지가 Activity에서 시작하는 자리(누른 것 표 뒤). */
		uint32 TilePressedBase = 0;
		FRDGBufferRef ContactGroups = nullptr;
		FRDGBufferRef ContactGroupCount = nullptr;
		FRDGBufferRef ContactArgs = nullptr;
		FRDGBufferRef LumpList = nullptr;
		FRDGBufferRef LumpListCount = nullptr;
		FRDGBufferSRVRef ContactGroupsSRV = nullptr;
		FRDGBufferSRVRef ContactGroupCountSRV = nullptr;
		FRDGBufferSRVRef LumpListCountSRV = nullptr;
		FRDGBufferRef TileCount = nullptr;
		FRDGBufferRef TileList = nullptr;
		FRDGBufferRef TileResidual = nullptr;
		FRDGBufferRef TileAnyResidual = nullptr;
		FRDGBufferRef TileLinkPairs = nullptr;
		FRDGBufferRef TileLinkPairCount = nullptr;
		/** 부위 나누기: 묶음마다 부위 몫이 닿는지 1비트(Rest.TileParts). */
		FRDGBufferRef TileParts = nullptr;
		/** 이완 목록 비트(ADR-0022): 목록 패스가 쓰고 이완이 목록 밖 묶음의 임시 상태 텍스처를 0으로 읽는 데 쓴다. */
		FRDGBufferRef TileBits = nullptr;
		FRDGBufferRef TileListArgs = nullptr;
		// 접촉 압력(ADR-0037): 머리, 압력 칸 목록, 거친 칸 → 칸 번호, 행렬, 힘, 간접 인자, 풀이 집계(모두 갱신 안 임시).
		FRDGBufferRef PressureHeader = nullptr;
		FRDGBufferUAVRef PressureHeaderUAV = nullptr;
		FRDGBufferRef PressureCells = nullptr;
		FRDGTextureRef PressureSlot = nullptr;
		FRDGBufferRef PressureMatrix = nullptr;
		FRDGBufferRef PressureForce = nullptr;
		FRDGBufferRef PressureArgs = nullptr;
		FRDGBufferRef PressureStats = nullptr;
		FRDGBufferRef Args = nullptr;
		FRDGBufferRef RestBuffer = nullptr;
		// 덩어리 평형
		FRHIGPUBufferReadback* LumpSlot = nullptr;
		FRDGBufferRef LumpState = nullptr;
		// 관성 브릿지(국소 모양 패스와 접촉 패스가 같이 읽는다)와 국소 모양이 거친 칸마다 옮긴 양(일지 251 — 안 쓰는 장은 한 칸짜리)
		FRDGBufferSRVRef InertiaBridgeSRV = nullptr;
		FRDGTextureRef LumpRegionOffset = nullptr;
		FRDGBufferRef LumpSums = nullptr;
		FRDGBufferUAVRef LumpSumsUAV = nullptr;
		FRDGBufferRef LumpOut = nullptr;
		FRDGBufferSRVRef LumpGroupsSRV = nullptr;
		FRDGBufferSRVRef LumpListSRV = nullptr;
		// 덩어리 힘 묶음(일지 251): 이번 프레임 손 밑 누름·쥐기 칸이 있는 묶음과 그 수, 덩어리 힘 패스의 간접 인자(갱신 안에서만 쓴다).
		FRDGBufferSRVRef LumpActiveTilesSRV = nullptr;
		FRDGBufferSRVRef LumpActiveCountSRV = nullptr;
		FRDGBufferRef LumpForceArgs = nullptr;
		FRDGBufferSRVRef TileListSRV = nullptr;
		FRDGBufferSRVRef TileCountSRV = nullptr;
		FRDGBufferSRVRef TileBitsSRV = nullptr;
		FRDGTextureRef CarrierKind = nullptr;
		FRDGBufferUAVRef TileResidualUAV = nullptr;
		FRDGBufferUAVRef TileAnyResidualUAV = nullptr;
		// 끌림 거친 격자(일지 161, ADR-0033): 끌림 A와 붙든 자리(갱신을 넘어 남는다), 원천 패스가 쓰는 이번 갱신 붙든 자리(임시), 이웃 표, 접촉 패스가
		// 쓰는 입력 버퍼, 원천 목록과 머리(갱신 안 임시).
		FRDGTextureRef DragTargetA = nullptr;
		FRDGTextureRef DragHoldTarget = nullptr;
		FRDGTextureRef DragHoldNext = nullptr;
		FRDGTextureRef DragLinks = nullptr;
		FRDGBufferRef DragInput = nullptr;
		FRDGBufferSRVRef DragInputSRV = nullptr;
		FRDGBufferRef DragSources = nullptr;
		FRDGBufferRef DragSourceQ[2] = {nullptr, nullptr};
		FRDGBufferRef DragClusters = nullptr;
		FRDGBufferRef DragClusterQ[2] = {nullptr, nullptr};
		uint32 DragClusterCapacity = 0;
		FRDGBufferRef DragHeader = nullptr;
		FRDGBufferUAVRef DragHeaderUAV = nullptr;
		// 함몰 거친 해(일지 199): 거친 해(갱신을 넘어 남는다), 거친 묶음마다 남은 표지(갱신을 넘어 남는다)와 거친 묶음 한 변 수. 거친 묶음 목록은
		// TileList 뒤쪽(NumTiles부터 — [0] 수, [1..] 묶음, 그 뒤 목록 비트)에 있다.
		FRDGTextureRef DentCoarseTarget = nullptr;
		FRDGBufferRef DentCoarseBusy = nullptr;
		uint32 CoarseTilesPerSide = 0;
		// 부위 나누기(Separated Parts): 구운 칸 몫(없으면 null — 묶는 자리는 CarrierSeparationBind), 함몰 첫 패스가 쓰고 둘째 패스가 읽는 채널(임시)과
		// 둘째 패스의 쓰기 자리(한 칸짜리 — 쓰지 않는다), 솟음 채널(거친 칸)·압력 칸 몫·끌림 원천 몫(임시), 거친 칸 표(PrepareCoarseTables). 끈 장은 검은
		// 텍스처·한 칸짜리를 묶는다.
		bool bSeparation = false;
		/** 이번 갱신에 긴 몫 모으기가 솟음 채널을 쓰는가(부위 나누기가 켜져 있고 긴 몫이 돈다). 긴 몫 읽기의 bSeparation이다. */
		bool bLongSeparation = false;
		FRHITexture* CarrierSeparationRHI = nullptr;
		FRHITexture* CarrierSeparationBind = nullptr;
		FRDGTextureRef SeparationPass = nullptr;
		FRDGTextureRef SeparationUnused = nullptr;
		FRDGTextureRef ContactLongSeparation = nullptr;
		FRDGBufferRef PressureSeparation = nullptr;
		FRDGBufferRef DragSourceSeparation = nullptr;
		FRDGTextureRef CoarseSeparation = nullptr;
		// 잡기(ADR-0041): 이번 갱신 입력, 갱신을 넘어 남는 줄(잡기가 없는 갱신은 한 칸짜리 빈 버퍼), 줄 수(0 = 잡기 없음), Activity에서 고르기 낱말 자리.
		FRDGBufferRef GrabInput = nullptr;
		FRDGBufferRef GrabState = nullptr;
		FRDGBufferSRVRef GrabInputSRV = nullptr;
		FRDGBufferSRVRef GrabStateSRV = nullptr;
		uint32 GrabCount = 0;
		uint32 GrabPickBase = 0;
	};

	/**
	 * 연속 굽힘만 있는 장(조각 없음)이 Flex 순열을 돌 때 묶는 빈 조각 표: 배열마다 0 한 줄(빈 버퍼는 만들 수 없다). 셰이더는 조각 수 0이라 읽지 않는다.
	 */
	const FSoftbodyHardPieceTable& SoftbodyEmptyHardTable()
	{
		static const FSoftbodyHardPieceTable Table = []()
		{
			FSoftbodyHardPieceTable T;
			const FVector4f Zero4(0.f, 0.f, 0.f, 0.f);
			T.Frames = {Zero4, Zero4, Zero4};
			T.TileRange = {FUintVector2(0u, 0u)};
			T.TileList = {0u};
			T.CellRange = {FUintVector2(0u, 0u)};
			T.CellList = {0u};
			T.CellPlate = {Zero4};
			T.Serial = 0xffffffffu;
			return T;
		}();
		return Table;
	}

	/**
	 * 휘는 판 표 한 장(ADR-0038 §4)을 갱신을 넘어 남는 버퍼로: 표가 바뀐 갱신(처음·다시 구움)에만 올리고 다음부터는 다시 묶는다. 빈 표는 Empty 한 칸.
	 */
	template <typename T>
	FRDGBufferSRVRef SoftbodyHardPlateSRV(FRDGBuilder& GraphBuilder, TRefCountPtr<FRDGPooledBuffer>& Pooled, bool bFresh, const TCHAR* Name,
		const TArray<T>& Data, const T& Empty)
	{
		if (bFresh || !Pooled.IsValid())
		{
			FRDGBufferRef Buffer = CreateStructuredBuffer(GraphBuilder, Name, Data.Num() > 0 ? Data : TArray<T>{Empty});
			Pooled = GraphBuilder.ConvertToExternalBuffer(Buffer);
			return GraphBuilder.CreateSRV(Buffer);
		}
		return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Pooled));
	}

	/** 단계 0: 텍스처·버퍼 등록, 올리기, 프레임 임시 버퍼, 처음 한 번 만드는 표(접촉 묶음 목록·이음새 표). */
	void SetupSurfaceFrame(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FGlobalShaderMap* ShaderMap = F.ShaderMap;
		F.TargetA = RegisterExternalTexture(GraphBuilder, RI.StateA->GetRenderTargetTexture(), TEXT("SoftbodySurfaceStateA"));
		// 갱신 안에서만 쓰는 텍스처(ADR-0022)는 그래프 안 임시 텍스처로: 인스턴스마다 패스를 차례로 넣으므로 RDG 임시 할당기가 앞
		// 인스턴스가 다 쓴 메모리를 다음 인스턴스에 다시 준다 — 월드에 몇 개가 깨어 있든 한 벌 값이다. 내용은 정해지지 않은 채 오므로
		// 읽는 쪽은 모두 "이번 갱신에 쓴 칸"만 읽는다(접촉: 칸 종류, 이완: 목록 비트).
		const FRDGTextureDesc ScratchDesc = FRDGTextureDesc::Create2D(FIntPoint((int32)RI.Resolution, (int32)RI.Resolution), PF_FloatRGBA,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV);
		// 함몰 패스 결과·붙들 깊이는 32비트 한 채널(상태 A와 같다, ADR-0032).
		const FRDGTextureDesc StateDesc = FRDGTextureDesc::Create2D(FIntPoint((int32)RI.Resolution, (int32)RI.Resolution), PF_R32_FLOAT,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV);
		F.TargetB = GraphBuilder.CreateTexture(StateDesc, TEXT("SoftbodySurfaceDentFirst"));
		// 붙든 칸 (목표 T, 붙들 깊이 P) — ADR-0037(합치기가 P를 읽는다).
		F.TargetC = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.Resolution, (int32)RI.Resolution), PF_G32R32F,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceDentPins"));
		F.TargetD = GraphBuilder.CreateTexture(StateDesc, TEXT("SoftbodySurfaceDentSecond"));
		F.DentUnused = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceDentUnused"));
		F.PinsUnused = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_G32R32F, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceDentPinsUnused"));
		F.HoldTarget = RegisterExternalTexture(GraphBuilder, RI.Hold->GetRenderTargetTexture(), TEXT("SoftbodySurfaceHold"));
		// 끌림 거친 격자(일지 161, ADR-0033): 끌림·붙든 자리는 32비트 — 쌓이는 끌림이 갱신 사이에서 반올림되지 않게(하프 정밀도면 천천히 미끄러지는
		// 손이 한 프레임에 더하는 몇 µm가 반올림에 사라져 끌림이 멈췄다). 이번 갱신 붙든 자리(DragHoldNext)는 원천 패스가 쓰고 모으기 패스가 DragHold로
		// 옮기는 임시 텍스처 — 한 패스에서 같은 텍스처를 읽고 쓰지 않는다(D3D11은 R32 한 채널만 UAV로 읽는다).
		F.DragTargetA = RegisterExternalTexture(GraphBuilder, RI.DragState->GetRenderTargetTexture(), TEXT("SoftbodySurfaceDragState"));
		F.DragHoldTarget = RegisterExternalTexture(GraphBuilder, RI.DragHold->GetRenderTargetTexture(), TEXT("SoftbodySurfaceDragHold"));
		const FRDGTextureDesc DragDesc = FRDGTextureDesc::Create2D(FIntPoint((int32)RI.DragResolution, (int32)RI.DragResolution),
			PF_A32B32G32R32F, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV);
		F.DragHoldNext = GraphBuilder.CreateTexture(DragDesc, TEXT("SoftbodySurfaceDragHoldNext"));
		F.DragInput = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f),
			RI.DragResolution * RI.DragResolution * DragInputRows), TEXT("SoftbodySurfaceDragInput"));
		F.DragInputSRV = GraphBuilder.CreateSRV(F.DragInput);
		// 끌림 원천 목록·머리(ADR-0033): 갱신 안에서만 산다. 머리는 ActivityArgsCS가 비운다.
		F.DragSources = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), DragSourceCapacity * DragSourceRows),
			TEXT("SoftbodySurfaceDragSources"));
		// 원천마다 끄는 힘(일지 183): 원천 패스가 쓰고 바로잡기가 번갈아 고친다(원천 목록과 같은 칸 수, 각 256 KB).
		F.DragSourceQ[0] = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), DragSourceCapacity), TEXT("SoftbodySurfaceDragSourceQA"));
		F.DragSourceQ[1] = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), DragSourceCapacity), TEXT("SoftbodySurfaceDragSourceQB"));
		// 묶음 원천(일지 187): 거친 8×8 묶음마다 하나까지. 끄는 힘 몫은 원천 패스가 첫 버퍼에, 바로잡기가 고친 Q로 둘째 버퍼에 쓴다.
		const uint32 DragGroupsPerSide = FMath::DivideAndRoundUp(FMath::Max(RI.DragResolution, 1u), CoarseGroup);
		F.DragClusterCapacity = DragGroupsPerSide * DragGroupsPerSide;
		F.DragClusters = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), F.DragClusterCapacity * DragClusterRows),
			TEXT("SoftbodySurfaceDragClusters"));
		F.DragClusterQ[0] = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), F.DragClusterCapacity * DragClusterQRows),
			TEXT("SoftbodySurfaceDragClusterQA"));
		F.DragClusterQ[1] = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), F.DragClusterCapacity * DragClusterQRows),
			TEXT("SoftbodySurfaceDragClusterQB"));
		F.DragHeader = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DragHeaderWords), TEXT("SoftbodySurfaceDragHeader"));
		F.DragHeaderUAV = GraphBuilder.CreateUAV(F.DragHeader, PF_R32_UINT);
		F.ContactTarget = GraphBuilder.CreateTexture(ScratchDesc, TEXT("SoftbodySurfaceContact"));
		// 가장자리 부호 거리(ADR-0029): 접촉이 재고 합치기가 변위 A로 옮긴다. 칸 수라 반정밀 한 채널이면 된다.
		F.EdgeTarget = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.Resolution, (int32)RI.Resolution), PF_R16F,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceEdge"));
		F.PositionTarget = RegisterExternalTexture(GraphBuilder, RI.Position->GetRenderTargetTexture(), TEXT("SoftbodySurfacePosition"));
		F.DisplacementTarget = RegisterExternalTexture(GraphBuilder, RI.Displacement->GetRenderTargetTexture(), TEXT("SoftbodySurfaceDisplacement"));
		F.NormalTarget = RegisterExternalTexture(GraphBuilder, RI.Normal->GetRenderTargetTexture(), TEXT("SoftbodySurfaceNormal"));
		F.AcceptedTarget = RegisterExternalTexture(GraphBuilder, RI.Accepted->GetRenderTargetTexture(), TEXT("SoftbodySurfaceAccepted"));
		F.LabelsTarget = RI.Labels ? RegisterExternalTexture(GraphBuilder, RI.Labels->GetRenderTargetTexture(), TEXT("SoftbodySurfaceLabels")) : nullptr;
		// 덩어리 칸 접촉 값(셰이더 SB_PackLumpDepth): 반정밀 여섯 값 + 미는 방향을 32비트 넷에 묶는다(일지 338 — 전에는 반정밀 RGBA 한 장).
		F.LumpDepthTarget = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(RI.LumpMode == 2 ? FIntPoint((int32)RI.Resolution, (int32)RI.Resolution)
			: FIntPoint(1, 1), PF_R32G32B32A32_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
			RI.LumpMode == 2 ? TEXT("SoftbodySurfaceLumpDepth") : TEXT("SoftbodySurfaceLumpDepthUnused"));
		F.LumpNormalTarget = RI.LumpNormal
			? RegisterExternalTexture(GraphBuilder, RI.LumpNormal->GetRenderTargetTexture(), TEXT("SoftbodySurfaceLumpNormal"))
			: GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceLumpNormalUnused"));
		// 손 닻(일지 201): 덩어리 값이 있는 캐리어만 있다. 없으면 한 칸짜리 빈 텍스처(덩어리 없는 캐리어는 쥐는 칸이 없어 읽지 않는다). 빈 자리용 텍스처는
		// 따로 하나 더 둔다 — 덩어리 힘 패스가 닻과 빈 자리를 읽기·쓰기로 같이 묶으므로 둘이 같은 텍스처면 안 된다.
		const FRDGTextureDesc AnchorUnusedDesc = FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_G32R32F, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);
		F.LumpAnchorUnused = GraphBuilder.CreateTexture(AnchorUnusedDesc, TEXT("SoftbodySurfaceLumpAnchorUnused"));
		F.ReadDummy = RegisterExternalTexture(GraphBuilder, GBlackTexture->TextureRHI.GetReference(), TEXT("SoftbodySurfaceReadDummy"));
		F.LumpAnchorTarget = RI.LumpAnchor
			? RegisterExternalTexture(GraphBuilder, RI.LumpAnchor->GetRenderTargetTexture(), TEXT("SoftbodySurfaceLumpAnchor"))
			: GraphBuilder.CreateTexture(AnchorUnusedDesc, TEXT("SoftbodySurfaceLumpAnchorNone"));
		F.PreviousDisplacementTarget = RegisterExternalTexture(GraphBuilder,
			RI.PreviousDisplacement->GetRenderTargetTexture(), TEXT("SoftbodySurfacePreviousDisplacement"));
		F.ContactLongTarget = RegisterExternalTexture(GraphBuilder, RI.ContactLong->GetRenderTargetTexture(), TEXT("SoftbodySurfaceContactLong"));
		F.ContactBendTarget = RegisterExternalTexture(GraphBuilder, RI.ContactBend->GetRenderTargetTexture(), TEXT("SoftbodySurfaceContactBend"));
		// 긴 몫이 꺼져 있으면(Basic) 함몰·합치기가 읽지 않지만 묶어는 둔다.
		F.CoarseInfo = RegisterExternalTexture(GraphBuilder, RI.CoarseInfo->GetRenderTargetTexture(), TEXT("SoftbodySurfaceCoarseInfo"));
		F.CoarseLayer = RegisterExternalTexture(GraphBuilder, RI.CoarseLayer->GetRenderTargetTexture(), TEXT("SoftbodySurfaceCoarseLayer"));
		F.LandmarkDistanceRHI = RHIOf(RI.LandmarkDistance);
		F.ShapeBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceShapes"), RI.Shapes);
		F.BoneBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceBones"), RI.Bones);
		F.PreviousBoneBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfacePreviousBones"), RI.PreviousBones);
		F.PlaneBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfacePlanes"), RI.Planes);
		F.FieldInstanceBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceFieldInstances"), RI.FieldInstances);
		F.ShapeMotionBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceShapeMotion"), RI.ShapeMotion);
		F.FieldMotionBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceFieldMotion"), RI.FieldMotion);
		F.ColliderIdBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceColliderIds"), RI.ColliderIds);
		if (RI.FieldVoxelUpload.Num() > 0)
		{
			F.VoxelBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceFieldVoxels"), RI.FieldVoxelUpload);
			RI.FieldVoxelState->Buffer = GraphBuilder.ConvertToExternalBuffer(F.VoxelBuffer);
		}
		else if (RI.FieldVoxelState->Buffer.IsValid())
		{
			F.VoxelBuffer = GraphBuilder.RegisterExternalBuffer(RI.FieldVoxelState->Buffer);
		}
		else
		{
			F.VoxelBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceFieldVoxels"), TArray<uint32>());
		}
		F.CarrierPositionRHI = RHIOf(RI.CarrierPosition);
		F.CarrierNormalRHI = RHIOf(RI.CarrierNormal);
		F.RedirectRHI = RHIOf(RI.Redirect);
		F.CarrierLumpRHI = RHIOf(RI.CarrierLump);
		F.CarrierVolumeRHI = OptionalRHIOf(RI.CarrierVolume);
		F.CarrierColumnRHI = OptionalRHIOf(RI.CarrierColumn);
		F.bHardSurface = RI.bHardSurface != 0u && OptionalRHIOf(RI.CarrierPiece) != nullptr;
		F.CarrierLiftRHI = F.bHardSurface ? OptionalRHIOf(RI.CarrierLift) : nullptr;
		F.CarrierPieceRHI = F.bHardSurface ? OptionalRHIOf(RI.CarrierPiece) : nullptr;
		// 연속 굽힘만(조각 없음, Full): 같은 순열을 빈 조각 표로 — 조각 번호 자리에는 아무 텍스처(조각 수 0이라 읽지 않는다).
		F.bFlexOnly = !F.bHardSurface && RI.bFlexOnly != 0u && RI.bContactLong != 0u;
		if (F.bFlexOnly)
		{
			F.bHardSurface = true;
			F.CarrierPieceRHI = F.CarrierNormalRHI;
		}
		// Flex 칠은 조각 순열이 아니어도 거친 칸 굽힘 길이(CoarseInitCS)가 읽는다.
		F.CarrierSoftnessRHI = OptionalRHIOf(RI.CarrierSoftness);
		F.Groups = FComputeShaderUtils::GetGroupCount(
			FIntPoint((int32)RI.Resolution, (int32)RI.Resolution), FIntPoint(ThreadGroupSize, ThreadGroupSize));

		// 쉬기(ADR-0013): 접촉 패스가 눌린 칸이 있으면 [0]에 1을 적는다. [1..2]는 살 두께보다 깊은 누름(관통 계약 밖, 일지 295 — Softbody.Stats가 읽는다).
		// 덩어리가 있으면 그 뒤에 누른 것(형상 + 거리장 부위) × 덩어리 묶음마다 (눌린 덩어리 칸 수, 가로 크기)를 쌓는다 — 덩어리 힘이 닿은 크기로
		// 칸 강성을 정한다(셰이더 SB_PRESSER_TABLE_BASE, 일지 248 ②).
		// 이완 묶음(2단계): 접촉 패스가 눌린 칸이 있는 묶음을 누른 것 표 뒤(TilePressedBase부터)에 적는다 — 전에는 따로 버퍼였으나 접촉 패스의
		// 쓰기 슬롯 8개(D3D11 한도) 가운데 하나를 들어온 면 표지에 내주려고 합쳤다(일지 313).
		const int32 NumPressers = RI.Shapes.Num() / 3 + RI.FieldInstances.Num() / SoftbodySurfaceFieldRows::PerInstance;
		const int32 PresserValues = RI.LumpMode == 2 ? (int32)PresserWords * USoftbodySurfaceField::NumLumpGroups * NumPressers : 0;
		F.TilesPerSide = FMath::DivideAndRoundUp(RI.Resolution, TileSize);
		F.NumTiles = F.TilesPerSide * F.TilesPerSide;
		F.TilePressedBase = SoftbodySurfaceActivityHeaderWords + (uint32)PresserValues;
		// 잡기 고르기(ADR-0041): 묶음 눌림 표지 뒤에 줄마다 한 낱말(0으로 비운다 — 0 = 찾는 거리 안 피부 없음). 접촉 패스의 쓰기 자원을 늘리지 않는다.
		F.GrabPickBase = F.TilePressedBase + F.NumTiles;
		F.Activity = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), F.GrabPickBase + (uint32)SoftbodyGrab::MaxRows),
			TEXT("SoftbodySurfaceActivity"));
		F.ActivityUAV = GraphBuilder.CreateUAV(F.Activity, PF_R32_UINT);
		AddClearUAVPass(GraphBuilder, F.ActivityUAV, 0u);
		// 잡기 줄(ADR-0041): 잡기가 있는 갱신만 갱신을 넘어 남는 줄을 묶는다(처음이면 0으로 만든다). 없으면 한 칸짜리 빈 버퍼 — 셰이더는 GrabCount 0이라 읽지 않는다.
		F.GrabCount = RI.GrabInput.Num() == SoftbodyGrab::MaxRows * SoftbodyGrab::InputRows ? (uint32)SoftbodyGrab::MaxRows : 0u;
		if (F.GrabCount > 0)
		{
			F.GrabInput = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceGrabInput"), RI.GrabInput);
			FSoftbodySurfaceRestState& GrabRest = *RI.RestState;
			if (GrabRest.GrabState.IsValid())
			{
				F.GrabState = GraphBuilder.RegisterExternalBuffer(GrabRest.GrabState);
			}
			else
			{
				FRDGBufferDesc GrabDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), (uint32)(SoftbodyGrab::MaxRows * SoftbodyGrab::StateRows));
				GrabDesc.Usage |= EBufferUsageFlags::SourceCopy;
				F.GrabState = GraphBuilder.CreateBuffer(GrabDesc, TEXT("SoftbodySurfaceGrabState"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.GrabState), 0u);
				GrabRest.GrabState = GraphBuilder.ConvertToExternalBuffer(F.GrabState);
			}
		}
		else
		{
			F.GrabInput = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceGrabInputNone"), TArray<FVector4f>{FVector4f(0.f, 0.f, 0.f, 0.f)});
			F.GrabState = F.GrabInput;
		}
		F.GrabInputSRV = GraphBuilder.CreateSRV(F.GrabInput);
		F.GrabStateSRV = GraphBuilder.CreateSRV(F.GrabState);

		// 접촉 묶음(ADR-0013 3단계): 처음 한 번(캐리어가 바뀌면 다시) 표면·고스트 칸이 든 8×8 묶음 목록을 만든다. 접촉 패스는
		// 이 묶음만 돈다(나나이트 캐릭터 1024²에서 43%).
		// 같은 패스가 덩어리 칸이 든 묶음 목록(덩어리 힘)과 덩어리 노멀을 재는 묶음 표지도 만든다(ADR-0013 §8).
		{
			FSoftbodySurfaceRestState& Rest = *RI.RestState;
			if (!Rest.ContactArgs.IsValid() || Rest.ContactGroupsCarrier != F.CarrierPositionRHI || Rest.ContactGroupsLump != F.CarrierLumpRHI
				|| Rest.ContactGroupsResolution != RI.Resolution)
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceContact, "SoftbodySurfaceContactGroups");
				F.ContactGroups = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), (uint32)(F.Groups.X * F.Groups.Y)),
					TEXT("SoftbodySurfaceContactGroups"));
				F.ContactGroupCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("SoftbodySurfaceContactGroupCount"));
				F.ContactArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
					TEXT("SoftbodySurfaceContactArgs"));
				F.LumpList = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), (uint32)(F.Groups.X * F.Groups.Y)),
					TEXT("SoftbodySurfaceLumpList"));
				F.LumpListCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("SoftbodySurfaceLumpListCount"));
				FRDGBufferUAVRef CountUAV = GraphBuilder.CreateUAV(F.ContactGroupCount, PF_R32_UINT);
				FRDGBufferUAVRef ArgsUAV = GraphBuilder.CreateUAV(F.ContactArgs, PF_R32_UINT);
				FRDGBufferUAVRef LumpCountUAV = GraphBuilder.CreateUAV(F.LumpListCount, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, CountUAV, 0u);
				AddClearUAVPass(GraphBuilder, ArgsUAV, 0u);
				AddClearUAVPass(GraphBuilder, LumpCountUAV, 0u);
				FSoftbodySurfaceContactGroupsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactGroupsCS::FParameters>();
				Parameters->CarrierPosition = F.CarrierPositionRHI;
				Parameters->CarrierLump = F.CarrierLumpRHI;
				Parameters->Resolution = RI.Resolution;
				Parameters->ContactGroupsOut = GraphBuilder.CreateUAV(F.ContactGroups, PF_R32_UINT);
				Parameters->ContactGroupCountOut = CountUAV;
				Parameters->ContactArgsOut = ArgsUAV;
				Parameters->LumpListOut = GraphBuilder.CreateUAV(F.LumpList, PF_R32_UINT);
				Parameters->LumpListCountOut = LumpCountUAV;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceContactGroups"),
					TShaderMapRef<FSoftbodySurfaceContactGroupsCS>(ShaderMap), Parameters, F.Groups);
				// 접촉 패스는 목록 밖 묶음과 빈칸을 쓰지 않고, 덩어리 노멀은 표지가 붙은 묶음에만 쓴다: 캐리어가 바뀌었을 때 남은 값이
				// 없도록 출력을 한 번 비운다.
				// (갱신을 넘어 남는 Position·LumpNormal·LumpAnchor만 — Contact·Drag·LumpDepth는 임시 텍스처라 빈칸을 아무도 읽지 않는다, ADR-0022.)
				const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
				for (FRDGTextureRef Output : {F.PositionTarget, F.LumpNormalTarget, F.LumpAnchorTarget, F.LabelsTarget})
				{
					if (Output)
					{
						AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Output), Zero);
					}
				}
				Rest.ContactGroups = GraphBuilder.ConvertToExternalBuffer(F.ContactGroups);
				Rest.ContactGroupCount = GraphBuilder.ConvertToExternalBuffer(F.ContactGroupCount);
				Rest.ContactArgs = GraphBuilder.ConvertToExternalBuffer(F.ContactArgs);
				Rest.LumpList = GraphBuilder.ConvertToExternalBuffer(F.LumpList);
				Rest.LumpListCount = GraphBuilder.ConvertToExternalBuffer(F.LumpListCount);
				Rest.ContactGroupsCarrier = F.CarrierPositionRHI;
				Rest.ContactGroupsLump = F.CarrierLumpRHI;
				Rest.ContactGroupsResolution = RI.Resolution;
			}
			else
			{
				F.ContactGroups = GraphBuilder.RegisterExternalBuffer(Rest.ContactGroups);
				F.ContactGroupCount = GraphBuilder.RegisterExternalBuffer(Rest.ContactGroupCount);
				F.ContactArgs = GraphBuilder.RegisterExternalBuffer(Rest.ContactArgs);
				F.LumpList = GraphBuilder.RegisterExternalBuffer(Rest.LumpList);
				F.LumpListCount = GraphBuilder.RegisterExternalBuffer(Rest.LumpListCount);
			}
		}
		F.ContactGroupsSRV = GraphBuilder.CreateSRV(F.ContactGroups, PF_R32_UINT);
		F.ContactGroupCountSRV = GraphBuilder.CreateSRV(F.ContactGroupCount, PF_R32_UINT);
		F.LumpListCountSRV = GraphBuilder.CreateSRV(F.LumpListCount, PF_R32_UINT);

		// 함몰 묶음 목록(ADR-0013 2단계, ADR-0032): 눌렸거나 붙든 자리가 남은 묶음, 그 둘레 1겹과 이음새 건너편만 함몰을 푼다.
		F.TileReach = DentTileReach;
		F.ArgsStride = sizeof(FRHIDispatchIndirectParameters) / sizeof(uint32);
		// 목록은 푸는 묶음 뒤에 비울 묶음이 붙는다(둘은 겹치지 않아 합이 묶음 수를 넘지 않는다).
		// [0] 함몰 묶음 수, [1] 비울 묶음 수, [2] 끌림이 닿은 거친 묶음 수(stat, DragGatherCS), [3..] 끌림 걸음 집계(TileCountWords).
		FRDGBufferDesc TileCountDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TileCountWords);
		TileCountDesc.Usage |= EBufferUsageFlags::SourceCopy;   // 멈춤 표지 읽기(일지 203)
		F.TileCount = GraphBuilder.CreateBuffer(TileCountDesc, TEXT("SoftbodySurfaceTileCount"));
		// 뒤쪽 NumTiles부터 함몰 거친 해의 거친 묶음 목록([0] 수, [1..] 묶음, 그 뒤 목록 비트, 일지 199) — 목록 패스의 쓰기 버퍼를 늘리지 않는다(ADR-0025).
		F.CoarseTilesPerSide = FMath::DivideAndRoundUp(RI.CoarseResolution, (uint32)TileSize);
		const uint32 CoarseListTiles = F.CoarseTilesPerSide * F.CoarseTilesPerSide;
		F.TileList = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
			F.NumTiles + 1u + CoarseListTiles + FMath::DivideAndRoundUp(CoarseListTiles, 32u)), TEXT("SoftbodySurfaceTileList"));
		// 뒤쪽 절반 = 부위 나누기 장에서 채널 순열로 푸는 묶음 비트(그 장의 목록 패스만 쓴다).
		F.TileBits = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2u * FMath::DivideAndRoundUp(F.NumTiles, 32u)),
			TEXT("SoftbodySurfaceTileBits"));
		{
			FSoftbodySurfaceRestState& Rest = *RI.RestState;
			const bool bNewTiles = Rest.TilesPerSide != F.TilesPerSide || !Rest.TileResidual.IsValid()
				|| !Rest.TileAnyResidual.IsValid() || !Rest.TileLinkPairs.IsValid();
			if (bNewTiles)
			{
				F.TileResidual = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), F.NumTiles),
					TEXT("SoftbodySurfaceTileResidual"));
				Rest.TileResidual = GraphBuilder.ConvertToExternalBuffer(F.TileResidual);
				F.TileAnyResidual = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("SoftbodySurfaceTileAnyResidual"));
				Rest.TileAnyResidual = GraphBuilder.ConvertToExternalBuffer(F.TileAnyResidual);
			}
			else
			{
				F.TileResidual = GraphBuilder.RegisterExternalBuffer(Rest.TileResidual);
				F.TileAnyResidual = GraphBuilder.RegisterExternalBuffer(Rest.TileAnyResidual);
			}
			// 새로 만들었거나 CPU가 장들을 비운 뒤(모든 상태 0)면 남은 묶음도 없다.
			if (bNewTiles || RI.bMotionHistory == 0)
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.TileResidual, PF_R32_UINT), 0u);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.TileAnyResidual, PF_R32_UINT), 0u);
			}
			// 부위 몫 텍스처가 바뀌어도(다시 굽기·스트리밍) 부위 묶음 비트를 다시 만든다.
			FRHITexture* SeparationRHI = OptionalRHIOf(RI.CarrierSeparation);
			if (bNewTiles || Rest.TileLinksRedirect != F.RedirectRHI || Rest.TileLinksSeparation != SeparationRHI || !Rest.TileParts.IsValid())
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceTiles, "SoftbodySurfaceTileLinks");
				F.TileLinkPairs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), F.NumTiles * TileLinksPerTile),
					TEXT("SoftbodySurfaceTileLinkPairs"));
				F.TileLinkPairCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("SoftbodySurfaceTileLinkPairCount"));
				FRDGBufferUAVRef PairCountUAV = GraphBuilder.CreateUAV(F.TileLinkPairCount, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, PairCountUAV, 0u);
				F.TileParts = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::DivideAndRoundUp(F.NumTiles, 32u)),
					TEXT("SoftbodySurfaceTileParts"));
				FRDGBufferUAVRef PartsUAV = GraphBuilder.CreateUAV(F.TileParts, PF_R32_UINT);
				AddClearUAVPass(GraphBuilder, PartsUAV, 0u);
				FSoftbodySurfaceTileLinksCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceTileLinksCS::FParameters>();
				Parameters->CarrierPosition = F.CarrierPositionRHI;
				Parameters->CarrierRedirect = F.RedirectRHI;
				Parameters->Resolution = RI.Resolution;
				Parameters->TilesPerSide = F.TilesPerSide;
				Parameters->TileLinkPairsOut = GraphBuilder.CreateUAV(F.TileLinkPairs, PF_R32_UINT);
				Parameters->TileLinkPairCountOut = PairCountUAV;
				Parameters->CarrierSeparation = SeparationRHI ? SeparationRHI : F.CarrierNormalRHI;
				Parameters->bCarrierSeparation = SeparationRHI ? 1u : 0u;
				Parameters->TilePartsOut = PartsUAV;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceTileLinks"),
					TShaderMapRef<FSoftbodySurfaceTileLinksCS>(ShaderMap), Parameters, FIntVector((int32)F.TilesPerSide, (int32)F.TilesPerSide, 1));
				Rest.TileLinkPairs = GraphBuilder.ConvertToExternalBuffer(F.TileLinkPairs);
				Rest.TileLinkPairCount = GraphBuilder.ConvertToExternalBuffer(F.TileLinkPairCount);
				Rest.TileParts = GraphBuilder.ConvertToExternalBuffer(F.TileParts);
				Rest.TileLinksRedirect = F.RedirectRHI;
				Rest.TileLinksSeparation = SeparationRHI;
			}
			else
			{
				F.TileLinkPairs = GraphBuilder.RegisterExternalBuffer(Rest.TileLinkPairs);
				F.TileLinkPairCount = GraphBuilder.RegisterExternalBuffer(Rest.TileLinkPairCount);
				F.TileParts = GraphBuilder.RegisterExternalBuffer(Rest.TileParts);
			}
			Rest.TilesPerSide = F.TilesPerSide;
		}

		// 거친 벌을 풀에서 새로 잡은 첫 갱신(장이 모두 0)이면 거친 칸 고정 표(Info·Layer·줄 번호)를 다시 만든다 — 그 벌은 다른 표면 장이 쓰던 것일 수 있다
		// (일지 235). 끌림·거친 해·원뿔은 같은 때 아래에서 0으로 비운다.
		if (RI.bMotionHistory == 0)
		{
			RI.RestState->SpreadStaticTarget = nullptr;
		}
		// 끌림 거친 격자(일지 161): 이웃 표는 캐리어나 거친 격자 해상도가 바뀔 때만 만든다(표면 장이 사는 동안 남는 표, 칸마다 16 B). 끌림·붙든 자리는
		// 새 텍스처이거나 CPU가 장들을 비운 뒤(모든 장 0에서 시작)면 0으로 비운다.
		{
			FSoftbodySurfaceRestState& Rest = *RI.RestState;
			if (!Rest.DragLinks.IsValid() || Rest.DragLinksCarrier != F.CarrierPositionRHI || Rest.DragLinksResolution != RI.DragResolution)
			{
				F.DragLinks = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.DragResolution, (int32)RI.DragResolution),
					PF_R32G32B32A32_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceDragLinks"));
				FSoftbodySurfaceDragLinksCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDragLinksCS::FParameters>();
				Parameters->CarrierPosition = F.CarrierPositionRHI;
				Parameters->CarrierRedirect = F.RedirectRHI;
				Parameters->Resolution = RI.Resolution;
				Parameters->CoarseResolution = RI.DragResolution;
				Parameters->CoarseBlock = RI.DragBlock;
				Parameters->TexelSize = RI.TexelSize;
				Parameters->ThicknessScale = RI.ThicknessScale;
				Parameters->DragLinksOut = GraphBuilder.CreateUAV(F.DragLinks);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDragLinks"),
					TShaderMapRef<FSoftbodySurfaceDragLinksCS>(ShaderMap), Parameters, FComputeShaderUtils::GetGroupCount(
						FIntPoint((int32)RI.DragResolution, (int32)RI.DragResolution), FIntPoint(ThreadGroupSize, ThreadGroupSize)));
				Rest.DragLinks = GraphBuilder.ConvertToExternalTexture(F.DragLinks);
				Rest.DragLinksCarrier = F.CarrierPositionRHI;
				Rest.DragLinksResolution = RI.DragResolution;
			}
			else
			{
				F.DragLinks = GraphBuilder.RegisterExternalTexture(Rest.DragLinks);
			}
			FRHITexture* DragStateRHI = RI.DragState->GetRenderTargetTexture();
			// 끌림을 끄는 순간(마찰 0·접촉 단계 Basic, 일지 222)에도 비운다 — 끌림 패스가 돌지 않으면 합치기가 읽는 끌림이 그대로 남는다.
			if (Rest.DragClearedTarget != DragStateRHI || RI.bMotionHistory == 0 || (RI.bDragTable == 0 && Rest.bDragLast))
			{
				const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.DragTargetA), Zero);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.DragHoldTarget), Zero);
				Rest.DragClearedTarget = DragStateRHI;
			}
			Rest.bDragLast = RI.bDragTable != 0;
		}

		// 함몰 거친 해(일지 199): 거친 해와 거친 묶음 남은 표지는 새로 만들었거나 CPU가 장들을 비운 뒤, 껐다 켤 때 0으로 비운다.
		{
			FSoftbodySurfaceRestState& Rest = *RI.RestState;
			F.DentCoarseTarget = RegisterExternalTexture(GraphBuilder, RI.DentCoarse->GetRenderTargetTexture(), TEXT("SoftbodySurfaceDentCoarse"));
			FRHITexture* DentCoarseRHI = RI.DentCoarse->GetRenderTargetTexture();
			const uint32 NumCoarseTiles = F.CoarseTilesPerSide * F.CoarseTilesPerSide;
			const bool bNewBusy = !Rest.DentCoarseBusy.IsValid() || Rest.DentCoarseTilesPerSide != F.CoarseTilesPerSide;
			if (bNewBusy)
			{
				F.DentCoarseBusy = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(NumCoarseTiles, 1u)),
					TEXT("SoftbodySurfaceDentCoarseBusy"));
				Rest.DentCoarseBusy = GraphBuilder.ConvertToExternalBuffer(F.DentCoarseBusy);
				Rest.DentCoarseTilesPerSide = F.CoarseTilesPerSide;
			}
			else
			{
				F.DentCoarseBusy = GraphBuilder.RegisterExternalBuffer(Rest.DentCoarseBusy);
			}
			if (bNewBusy || Rest.DentCoarseClearedTarget != DentCoarseRHI || RI.bMotionHistory == 0 || (RI.bDentCoarse != 0) != Rest.bDentCoarseLast)
			{
				const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.DentCoarseTarget), Zero);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.DentCoarseBusy, PF_R32_UINT), 0u);
				Rest.DentCoarseClearedTarget = DentCoarseRHI;
			}
			Rest.bDentCoarseLast = RI.bDentCoarse != 0;
		}

		// 이완 인자: ActivityArgsCS가 0으로 비우고 TileListCS가 목록 길이로 채운다(목록 패스를 부르는 Args와 따로 둔다).
		F.TileListArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(NumListArgs),
			TEXT("SoftbodySurfaceTileListArgs"));
		// 접촉 압력(ADR-0037): 모두 갱신 안에서만 산다(크기가 늘 같아 RDG 임시 할당기가 인스턴스끼리 돌려 쓴다 — 새 할당 없음). 머리는 ActivityArgsCS가
		// 비운다. 행렬은 상한² 칸(768² × 4 B = 2.4 MB).
		F.PressureHeader = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), PressureHeaderWords), TEXT("SoftbodySurfacePressureHeader"));
		F.PressureHeaderUAV = GraphBuilder.CreateUAV(F.PressureHeader, PF_R32_UINT);
		F.PressureCells = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), PressureLevels * PressureCapacity * PressureRows),
			TEXT("SoftbodySurfacePressureCells"));
		F.PressureSlot = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), PF_R32_UINT,
			FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfacePressureSlot"));
		// 행렬은 반정밀 둘을 낱말 하나에(일지 283 — 풀이가 곱셈마다 행렬을 다 읽어 읽는 양이 비용을 정한다). 배치는 셰이더 PressureMatrixOut 주석:
		// 낱말 [i × ⌈N/2⌉ + j/2] — 칸 수 N이 상한일 때 상한 × ⌈상한/2⌉ 낱말.
		F.PressureMatrix = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), PressureCapacity * ((PressureCapacity + 1) / 2)),
			TEXT("SoftbodySurfacePressureMatrix"));
		F.PressureForce = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), PressureCapacity), TEXT("SoftbodySurfacePressureForce"));
		F.PressureArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(2), TEXT("SoftbodySurfacePressureArgs"));
		F.PressureStats = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("SoftbodySurfacePressureStats"));

		// 쉬기(ADR-0013): 눌린 칸이 없고 복원이 끝났으면 아래 패스들이 0개 묶음으로 돈다. 판단은 GPU가 이번 프레임에 한다.
		F.Args = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(NumArgs),
			TEXT("SoftbodySurfaceArgs"));
		if (RI.RestState->Buffer.IsValid())
		{
			F.RestBuffer = GraphBuilder.RegisterExternalBuffer(RI.RestState->Buffer);
		}
		else
		{
			const uint32 Initial[2] = {RestedProgress, 1u};
			F.RestBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceRest"), sizeof(uint32), 2, Initial, sizeof(Initial));
			RI.RestState->Buffer = GraphBuilder.ConvertToExternalBuffer(F.RestBuffer);
		}

		// 덩어리 평형: 읽기 칸이 모두 차 있으면 이번 프레임은 건너뛴다(반복 상태는 GPU에 남아 다음에 이어 푼다).
		F.LumpSlot = RI.LumpMode == 2 && RI.LumpGroups.Num() > 0 && RI.LumpReadback.IsValid() ? RI.LumpReadback->Acquire() : nullptr;
		// 반복 상태(묶음마다 LumpStateRows줄, 프레임을 넘어 남는다)와 이번 프레임 상수: 국소 모양 패스(일지 251)가 접촉 패스 앞에서 지난 풀이를 읽으므로
		// 덩어리 평형을 건너뛰는 프레임에도 잡는다.
		if (RI.LumpMode == 2 && RI.LumpGroups.Num() > 0)
		{
			FSoftbodySurfaceRestState& Rest = *RI.RestState;
			const uint32 NumStateRows = USoftbodySurfaceField::NumLumpGroups * LumpStateRows;
			if (Rest.LumpState.IsValid() && Rest.LumpState->Desc.NumElements == NumStateRows)
			{
				F.LumpState = GraphBuilder.RegisterExternalBuffer(Rest.LumpState);
			}
			else
			{
				F.LumpState = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumStateRows), TEXT("SoftbodyLumpState"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.LumpState), 0u);
				Rest.LumpState = GraphBuilder.ConvertToExternalBuffer(F.LumpState);
			}
			F.LumpGroupsSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodyLumpGroups"), RI.LumpGroups));
		}
		F.InertiaBridgeSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceInertiaBridge"),
			RI.InertiaBridge.Num() > 0 ? RI.InertiaBridge : TArray<FVector4f>{FVector4f(0.f, 0.f, 0.f, 0.f)}));
		// 하드 서피스(ADR-0037 §3.7): 조각이 구워진 장만 — 조각이 없는 자산은 아무 버퍼도 만들지 않는다(원칙 E06). 조각 움직임은 압력 풀이 꼬리가 쓴다
		// — 갱신마다 0으로 비워, 풀이가 돌지 않으면(Basic·닿은 것 없음) 모든 조각이 꺼져 살처럼 눌린다.
		if (F.bHardSurface)
		{
			// 연속 굽힘만인 장은 빈 조각 표(배열마다 0 한 줄, 조각 수 0)
			const FSoftbodyHardPieceTable& Table = RI.HardTable.IsValid() ? *RI.HardTable : SoftbodyEmptyHardTable();
			F.HardPieceCount = RI.HardTable.IsValid() ? (uint32)Table.TileRange.Num() : 0u;
			F.HardPieceFramesSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodyHardPieceFrames"), Table.Frames));
			F.HardTileRangeSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodyHardPieceTileRange"), Table.TileRange));
			F.HardTileListSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodyHardPieceTileList"),
				Table.TileList.Num() > 0 ? Table.TileList : TArray<uint32>{0u}));
			F.HardCellRangeSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodyHardPieceCellRange"),
				Table.CellRange.Num() > 0 ? Table.CellRange : TArray<FUintVector2>{FUintVector2(0u, 0u)}));
			F.HardCellListSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodyHardPieceCellList"),
				Table.CellList.Num() > 0 ? Table.CellList : TArray<uint32>{0u}));
			F.HardPieceMotion = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), FMath::Max(F.HardPieceCount, 1u)),
				TEXT("SoftbodyHardPieceMotion"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.HardPieceMotion), 0u);
			F.HardPieceMotionSRV = GraphBuilder.CreateSRV(F.HardPieceMotion);
			// Full은 압력 풀이가 강체 판을 푼다. Basic은 거친 풀이가 없어 판 항이 없다 — 조각이 살처럼 눌린다(ADR-0038 §4, Basic (가)는 지웠다).
			F.HardMode = RI.bContactLong != 0u ? 1u : 0u;
			// 발자국 칸 판 자리 표: 갱신을 넘어 남는 버퍼에 표가 바뀔 때만 올린다
			FSoftbodySurfaceRestState& PlateRest = *RI.RestState;
			const bool bFresh = PlateRest.HardPlateSerial != Table.Serial;
			PlateRest.HardPlateSerial = Table.Serial;
			F.HardCellPlateSRV = SoftbodyHardPlateSRV(GraphBuilder, PlateRest.HardCellPlate, bFresh, TEXT("SoftbodyHardCellPlate"), Table.CellPlate, FVector4f(0.f, 0.f, 0.f, 0.f));
		}
		// 국소 모양이 거친 칸마다 옮긴 양(일지 251): 국소 모양 패스가 쓰고 접촉·합치기가 읽는다. 안 쓰는 장은 0으로 비운 한 칸짜리를 묶는다.
		const bool bRegion = RI.bLumpRegion != 0u && F.LumpState;
		F.LumpRegionOffset = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(
			bRegion ? FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution) : FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceLumpRegionOffset"));
		if (!bRegion)
		{
			const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.LumpRegionOffset), Zero);
		}
	}

	/**
	 * 하드 서피스 조각 자원(ADR-0037 §3.7)과 휘는 판 읽기(ADR-0038 §4 — 마디 표·이번 갱신 마디 값, 고스트 칸 자리)를 묶는다. 순열 0은 읽지 않아 null 그대로
	 * 둔다(조각이 없는 자산은 버퍼가 없다).
	 */
	template <typename TParameters>
	void SetHardPieceParameters(TParameters* Parameters, const FSurfaceFrame& F)
	{
		Parameters->CarrierPiece = F.CarrierPieceRHI ? F.CarrierPieceRHI : F.CarrierNormalRHI;
		Parameters->HardPieceFrames = F.HardPieceFramesSRV;
		Parameters->HardPieceMotion = F.HardPieceMotionSRV;
		Parameters->HardPieceCount = F.HardPieceCount;
		Parameters->CarrierRedirect = F.RedirectRHI;
		// 칠이 없는 옛 굽기(판 1)는 조각이 없어 여기 오지 않는다. 텍스처가 빠졌으면 아무 텍스처나 묶어 둔다 — 순열 0은 읽지 않는다.
		Parameters->CarrierSoftness = F.CarrierSoftnessRHI ? F.CarrierSoftnessRHI : F.CarrierNormalRHI;
		Parameters->bCarrierSoftness = F.CarrierSoftnessRHI ? 1u : 0u;
		Parameters->HardRefThickness = F.RI->HardRefThickness;
		Parameters->FlexOffset = F.RI->FlexOffset;
	}

	/** 접촉 패스: 캐리어 칸마다 콜라이더 관통 깊이·노멀·위치·끌림을 적고, 눌린 칸이 있으면 쉬기 표지를 켠다. */
	void AddSurfaceContactPass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceContactCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactCS::FParameters>();
		Parameters->CarrierPosition = F.CarrierPositionRHI;
		Parameters->CarrierNormal = F.CarrierNormalRHI;
		Parameters->CarrierSkinIndex = RHIOf(RI.SkinIndex);
		Parameters->CarrierSkinWeight = RHIOf(RI.SkinWeight);
		Parameters->Shapes = GraphBuilder.CreateSRV(F.ShapeBuffer);
		Parameters->Bones = GraphBuilder.CreateSRV(F.BoneBuffer);
		Parameters->PreviousBones = GraphBuilder.CreateSRV(F.PreviousBoneBuffer);
		Parameters->Planes = GraphBuilder.CreateSRV(F.PlaneBuffer);
		Parameters->NumShapes = (uint32)(RI.Shapes.Num() / 3);
		Parameters->FieldVoxels = GraphBuilder.CreateSRV(F.VoxelBuffer);
		Parameters->FieldInstances = GraphBuilder.CreateSRV(F.FieldInstanceBuffer);
		Parameters->NumFieldInstances = (uint32)(RI.FieldInstances.Num() / SoftbodySurfaceFieldRows::PerInstance);
		Parameters->bSkinned = RI.Bones.Num() > 0 && RI.SkinIndex && RI.SkinWeight ? 1u : 0u;
		Parameters->Resolution = RI.Resolution;
		Parameters->ComponentScale = RI.ComponentScale;
		Parameters->MaxContactDepth = RI.MaxContactDepth;
		Parameters->EdgeTrimWidth = RI.EdgeTrimWidth;
		// 솟음이 꺼지면(Basic·거리표 없음) 솟는 살의 한도를 읽는 패스가 없어 거리장 부위의 위로 빈 높이 걸음을 건너뛴다(일지 229).
		Parameters->bContactLongRoom = RI.bContactLong;
		// 제 살 깊이가 없는 캐리어는 아무 텍스처나 묶어 두고 셰이더가 읽지 않는다(전부 1).
		Parameters->CarrierColumn = F.CarrierColumnRHI ? F.CarrierColumnRHI : F.CarrierNormalRHI;
		Parameters->bCarrierColumn = F.CarrierColumnRHI ? 1u : 0u;
		Parameters->ContactOut = GraphBuilder.CreateUAV(F.ContactTarget);
		Parameters->EdgeOut = GraphBuilder.CreateUAV(F.EdgeTarget);
		Parameters->PositionOut = GraphBuilder.CreateUAV(F.PositionTarget);
		Parameters->ShapeMotion = GraphBuilder.CreateSRV(F.ShapeMotionBuffer);
		Parameters->FieldMotion = GraphBuilder.CreateSRV(F.FieldMotionBuffer);
		Parameters->StatePrevious = F.TargetA;
		Parameters->DragRatio = RI.DragRatio;
		Parameters->LumpStiffen = RI.LumpStiffen;
		Parameters->bMotionHistory = RI.bMotionHistory;
		// 끌림 거친 격자(일지 161): 지난 갱신의 끌림을 읽고, 거친 칸마다 끌림 원천 패스가 읽을 입력을 쓴다(ADR-0033).
		Parameters->DragBlock = RI.DragBlock;
		Parameters->DragResolution = RI.DragResolution;
		Parameters->DragLinks = F.DragLinks;
		Parameters->DragState = F.DragTargetA;
		Parameters->DragInputOut = GraphBuilder.CreateUAV(F.DragInput);
		Parameters->CarrierVolume = F.CarrierVolumeRHI ? F.CarrierVolumeRHI : F.CarrierNormalRHI;
		Parameters->bCarrierVolume = F.CarrierVolumeRHI ? 1u : 0u;
		Parameters->VolumeValue = RI.VolumeValue;
		Parameters->VolumeOffset = RI.VolumeOffset;
		Parameters->SkinLength = RI.SkinLength;
		// 덩어리(ADR-0012): 브릿지가 없으면 한 칸짜리 0 버퍼, 덩어리 깊이 RT가 없으면 한 칸짜리 빈 텍스처를 묶는다(셰이더는
		// LumpMode로 읽고 쓰기를 건너뛴다).
		Parameters->CarrierLump = F.CarrierLumpRHI;
		// 옆 몫(§3.5)이 없는 옛 베이크는 노멀 몫만 옮긴다.
		Parameters->CarrierLumpSideA = RHIOf(RI.CarrierLumpSideA);
		Parameters->CarrierLumpSideB = RHIOf(RI.CarrierLumpSideB);
		Parameters->bLumpSide = RI.CarrierLumpSideA && RI.CarrierLumpSideB ? 1u : 0u;
		Parameters->InertiaBridge = F.InertiaBridgeSRV;
		Parameters->LumpMode = RI.LumpMode;
		Parameters->ComponentRotation = RI.ComponentRotation;
		Parameters->FrameToActor = RI.FrameToActor;
		// 덩어리 국소 모양(일지 251): 국소 모양 패스가 이 갱신 앞에 쓴 값. 꺼진 장은 0으로 비운 한 칸짜리를 묶고 셰이더가 읽지 않는다.
		Parameters->bLumpRegion = RI.bLumpRegion;
		Parameters->LumpRegionOffset = F.LumpRegionOffset;
		Parameters->CoarseInfo = F.CoarseInfo;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->ThicknessScale = RI.ThicknessScale;
		Parameters->LumpDepthOut = GraphBuilder.CreateUAV(F.LumpDepthTarget);
		// 덩어리 노멀(ADR-0018): 이웃 거리 = 렌더 삼각형 변 길이(리다이렉트 z), 칸 크기로 칸 수를 정한다.
		Parameters->CarrierRedirect = F.RedirectRHI;
		Parameters->TexelSize = RI.TexelSize;
		Parameters->LumpNormalOut = GraphBuilder.CreateUAV(F.LumpNormalTarget);
		// 손 닻(일지 201): 읽기만. 닻 텍스처가 없는 캐리어(덩어리 없음)는 검은 텍스처 — 어차피 쥐는 칸이 없다(이번 갱신의 빈 닻은 아직 아무도
		// 쓰지 않았다, 일지 234).
		Parameters->LumpAnchor = RI.LumpAnchor ? F.LumpAnchorTarget : F.ReadDummy;
		Parameters->ActivityOut = F.ActivityUAV;
		Parameters->TilePressedBase = F.TilePressedBase;
		Parameters->TilesPerSide = F.TilesPerSide;
		// 들어온 면 표지(일지 313): 칸 스레드가 제 칸만 제자리에서 읽고 쓴다(이웃을 읽지 않아 갱신 시작 사본이 없다). 표지 텍스처가 없는 벌(만들기 전
		// 옛 벌)은 한 칸짜리 빈 텍스처를 묶고 셰이더가 지금 규칙만 쓴다.
		Parameters->ColliderIds = GraphBuilder.CreateSRV(F.ColliderIdBuffer);
		if (F.LabelsTarget)
		{
			Parameters->ContactLabels = GraphBuilder.CreateUAV(F.LabelsTarget);
			Parameters->bLabels = 1u;
		}
		else
		{
			const FRDGTextureDesc NoneDesc = FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV);
			Parameters->ContactLabels = GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(NoneDesc, TEXT("SoftbodySurfaceLabelsNone")));
			Parameters->bLabels = 0u;
		}
		Parameters->bLabelHistory = RI.bMotionHistory;
		Parameters->bSweep = RI.bSweep;
		Parameters->ContactGroups = F.ContactGroupsSRV;
		Parameters->ContactGroupCount = F.ContactGroupCountSRV;
		Parameters->IndirectArgs = F.ContactArgs;
		// 하드 서피스(순열 1만 읽는다): 떠 있는 조각이 없으면 아무 텍스처나 묶고 읽지 않는다.
		Parameters->CarrierLift = F.CarrierLiftRHI ? F.CarrierLiftRHI : F.CarrierNormalRHI;
		Parameters->HardLiftScale = RI.CarrierLiftScale;
		Parameters->bCarrierLift = F.CarrierLiftRHI ? 1u : 0u;
		// 잡기(ADR-0041): 줄 수 0이면 셰이더가 잡기 코드를 건너뛴다.
		Parameters->GrabInput = F.GrabInputSRV;
		Parameters->GrabState = F.GrabStateSRV;
		Parameters->GrabCount = F.GrabCount;
		Parameters->GrabPickBase = F.GrabPickBase;
		FSoftbodySurfaceContactCS::FPermutationDomain Permutation;
		Permutation.Set<FSoftbodyHardSurfaceDim>(F.bHardSurface);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceContact"),
			TShaderMapRef<FSoftbodySurfaceContactCS>(F.ShaderMap, Permutation), Parameters, F.ContactArgs, 0);
	}

	/**
	 * 잡기 줄 풀기(ADR-0041): 접촉 패스 뒤 스레드 묶음 하나가 줄마다 고르기·당긴 벡터·한도·놓는 중 감쇠를 쓰고, 줄(1 KB)을 읽기 칸으로 복사한다(이벤트·프리뷰
	 * 도움말이 1~3프레임 뒤에 읽는다). 잡기가 없는 갱신에는 패스도 복사도 없다. 읽기 칸이 다 차 있으면 이번 갱신은 복사하지 않는다(다음 갱신이 다시 적는다).
	 */
	void AddSurfaceGrabResolvePass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		if (F.GrabCount == 0)
		{
			return;
		}
		FSoftbodySurfaceGrabResolveCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceGrabResolveCS::FParameters>();
		Parameters->GrabInput = F.GrabInputSRV;
		Parameters->GrabStateOut = GraphBuilder.CreateUAV(F.GrabState);
		Parameters->GrabPicks = GraphBuilder.CreateSRV(F.Activity, PF_R32_UINT);
		Parameters->GrabCount = F.GrabCount;
		Parameters->GrabPickBase = F.GrabPickBase;
		Parameters->CarrierPosition = F.CarrierPositionRHI;
		Parameters->CarrierNormal = F.CarrierNormalRHI;
		Parameters->CarrierSkinIndex = RHIOf(RI.SkinIndex);
		Parameters->CarrierSkinWeight = RHIOf(RI.SkinWeight);
		Parameters->Bones = GraphBuilder.CreateSRV(F.BoneBuffer);
		Parameters->bSkinned = RI.Bones.Num() > 0 && RI.SkinIndex && RI.SkinWeight ? 1u : 0u;
		Parameters->PositionIn = F.PositionTarget;
		Parameters->ContactIn = F.ContactTarget;
		Parameters->ComponentScale = RI.ComponentScale;
		Parameters->Resolution = RI.Resolution;
		Parameters->ThicknessScale = RI.ThicknessScale;
		Parameters->HoldDecay = RI.HoldDecay;
		Parameters->bContactLong = RI.bContactLong;
		Parameters->CarrierVolume = F.CarrierVolumeRHI ? F.CarrierVolumeRHI : F.CarrierNormalRHI;
		Parameters->bCarrierVolume = F.CarrierVolumeRHI ? 1u : 0u;
		Parameters->VolumeValue = RI.VolumeValue;
		Parameters->VolumeOffset = RI.VolumeOffset;
		Parameters->SkinLength = RI.SkinLength;
		Parameters->GrabFlexPaint = F.CarrierSoftnessRHI ? F.CarrierSoftnessRHI : F.CarrierNormalRHI;
		Parameters->bGrabFlexPaint = F.CarrierSoftnessRHI ? 1u : 0u;
		Parameters->GrabFlexOffset = RI.FlexOffset;
		Parameters->CarrierLump = F.CarrierLumpRHI;
		Parameters->CarrierRedirect = F.RedirectRHI;
		Parameters->bLumpRegion = RI.bLumpRegion;
		Parameters->LumpRegionOffset = F.LumpRegionOffset;
		Parameters->CoarseInfo = F.CoarseInfo;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->TexelSize = RI.TexelSize;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceGrabResolve"),
			TShaderMapRef<FSoftbodySurfaceGrabResolveCS>(F.ShaderMap), Parameters, FIntVector(1, 1, 1));
		if (FRHIGPUBufferReadback* Slot = RI.RestState->AcquireGrab())
		{
			AddEnqueueCopyPass(GraphBuilder, Slot, F.GrabState, FSoftbodySurfaceRestState::NumGrabFloats * sizeof(float));
		}
	}

	/** 쉬기 판단(인자 채우기)과 지난 변위 보관. */
	void AddSurfaceActivityPasses(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceActivityArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceActivityArgsCS::FParameters>();
		Parameters->ActivityIn = GraphBuilder.CreateSRV(F.Activity, PF_R32_UINT);
		// 지난 프레임 이완이 남긴 "어디든 남았다"를 읽고, 이번 프레임에 다시 쓰도록 목록 길이·이완·솟음 인자와 함께 비운다.
		Parameters->TileAnyResidualOut = GraphBuilder.CreateUAV(F.TileAnyResidual, PF_R32_UINT);
		Parameters->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
		Parameters->RestFrames = GraphBuilder.CreateUAV(F.RestBuffer);
		Parameters->ArgsOut = GraphBuilder.CreateUAV(F.Args, PF_R32_UINT);
		Parameters->ContactGroupCount = F.ContactGroupCountSRV;
		Parameters->LumpListCount = F.LumpListCountSRV;
		Parameters->PressureHeaderOut = F.PressureHeaderUAV;
		Parameters->DragHeaderOut = F.DragHeaderUAV;
		Parameters->TileListArgsOut = GraphBuilder.CreateUAV(F.TileListArgs, PF_R32_UINT);
		Parameters->FullGroups = (uint32)F.Groups.X;
		Parameters->SettleStep = RI.SettleStep;
		// 표면 장을 막 만들었거나 CPU가 비운 첫 프레임: 모든 장이 0이라 비울 필요 없이 쉬는 상태에서 시작한다.
		Parameters->bResetRest = RI.bMotionHistory != 0 ? 0u : 1u;
		Parameters->ArgsStride = F.ArgsStride;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceActivityArgs"),
			TShaderMapRef<FSoftbodySurfaceActivityArgsCS>(F.ShaderMap), Parameters, FIntVector(1, 1, 1));

		// 지난 변위: 이번 프레임 변위를 쓰기 전에 보관한다(머티리얼의 지난 프레임 계산 = 속도 버퍼가 읽는다).
		FSoftbodySurfaceCopyCS::FParameters* CopyParameters = GraphBuilder.AllocParameters<FSoftbodySurfaceCopyCS::FParameters>();
		CopyParameters->CopyIn = F.DisplacementTarget;
		CopyParameters->CopyOut = GraphBuilder.CreateUAV(F.PreviousDisplacementTarget);
		CopyParameters->Resolution = RI.Resolution;
		CopyParameters->ContactGroups = F.ContactGroupsSRV;
		CopyParameters->ContactGroupCount = F.ContactGroupCountSRV;
		CopyParameters->IndirectArgs = F.Args;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceCopyPrevious"),
			TShaderMapRef<FSoftbodySurfaceCopyCS>(F.ShaderMap), CopyParameters, F.Args, ArgsOffset(ArgsListed));
	}

	/** 함몰·비울 묶음 목록 만들기(스레드 묶음 하나): 어디든 눌렸거나 지난 갱신 끝에 상태가 남았을 때만 돈다(인자 3). */
	void AddSurfaceTileListPass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceTileListCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceTileListCS::FParameters>();
		Parameters->TilePressed = GraphBuilder.CreateSRV(F.Activity, PF_R32_UINT);
		Parameters->TilePressedBase = F.TilePressedBase;
		Parameters->TileResidual = GraphBuilder.CreateSRV(F.TileResidual, PF_R32_UINT);
		Parameters->TileLinkPairs = GraphBuilder.CreateSRV(F.TileLinkPairs, PF_R32_UINT);
		Parameters->TileLinkPairCount = GraphBuilder.CreateSRV(F.TileLinkPairCount, PF_R32_UINT);
		Parameters->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
		Parameters->TileListOut = GraphBuilder.CreateUAV(F.TileList, PF_R32_UINT);
		Parameters->TileBitsOut = GraphBuilder.CreateUAV(F.TileBits, PF_R32_UINT);
		Parameters->TilesPerSide = F.TilesPerSide;
		Parameters->TileReach = F.TileReach;
		Parameters->ArgsStride = F.ArgsStride;
		Parameters->TileListArgsOut = GraphBuilder.CreateUAV(F.TileListArgs, PF_R32_UINT);
		Parameters->bDentParts = F.bSeparation ? 1u : 0u;
		// 목록 까닭별 수는 Softbody.Stats·stat softbody가 읽는 갱신에만 센다(그 밖에는 셰이더가 건너뛴다).
		Parameters->bTileStats = RI.bReadStats ? 1u : 0u;
		Parameters->TileParts = GraphBuilder.CreateSRV(F.TileParts, PF_R32_UINT);
		Parameters->DentCoarseOn = RI.bDentCoarse;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->DentCoarseBusy = GraphBuilder.CreateSRV(F.DentCoarseBusy, PF_R32_UINT);
		Parameters->DentCoarseListBase = F.NumTiles;
		Parameters->IndirectArgs = F.Args;
		Parameters->HardPieceTileRange = F.HardTileRangeSRV;
		Parameters->HardPieceTileList = F.HardTileListSRV;
		Parameters->HardPieceCount = F.HardPieceCount;
		Parameters->HardPieceFrames = F.HardPieceFramesSRV;
		Parameters->FlexOffset = RI.FlexOffset;
		FSoftbodySurfaceTileListCS::FPermutationDomain Permutation;
		Permutation.Set<FSoftbodyHardSurfaceDim>(F.bHardSurface);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceTileList"),
			TShaderMapRef<FSoftbodySurfaceTileListCS>(F.ShaderMap, Permutation), Parameters, F.Args, ArgsOffset(ArgsTiles));
	}

	/**
	 * stat softbody(통계를 보는 중일 때만): 쉬는지·함몰 묶음 수·접촉 묶음 수·눌렸는지·끌림 원천 수(끄는 힘이 남은 원천이 없으면 0)·끌림이 닿은 묶음 수를 모아 몇 프레임
	 * 뒤에 읽는다(끌림 패스 뒤에 부른다).
	 */
	void AddSurfaceStatsPass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FRHIGPUBufferReadback* StatSlot = RI.bReadStats ? RI.RestState->AcquireStat() : nullptr;
		if (!StatSlot)
		{
			return;
		}
		constexpr uint32 NumStatWords = FSoftbodySurfaceRestState::NumStatWords;
		FRDGBufferDesc StatDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumStatWords);
		StatDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef StatBuffer = GraphBuilder.CreateBuffer(StatDesc, TEXT("SoftbodySurfaceStats"));
		FSoftbodySurfaceStatsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceStatsCS::FParameters>();
		Parameters->StatActivity = GraphBuilder.CreateSRV(F.Activity, PF_R32_UINT);
		Parameters->StatRest = GraphBuilder.CreateSRV(F.RestBuffer);
		Parameters->StatTileCount = GraphBuilder.CreateSRV(F.TileCount, PF_R32_UINT);
		Parameters->StatContactGroupCount = F.ContactGroupCountSRV;
		Parameters->StatDragHeader = GraphBuilder.CreateSRV(F.DragHeader, PF_R32_UINT);
		Parameters->StatOut = GraphBuilder.CreateUAV(StatBuffer, PF_R32_UINT);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceStats"),
			TShaderMapRef<FSoftbodySurfaceStatsCS>(F.ShaderMap), Parameters, FIntVector(1, 1, 1));
		AddEnqueueCopyPass(GraphBuilder, StatSlot, StatBuffer, NumStatWords * sizeof(uint32));
	}

	/**
	 * 멈춤 표지(일지 203): 이번 갱신의 눌림·바뀜 비트(TileCount 낱말 SB_TILE_COUNT_STILL)를 신청 번호와 함께 읽기 칸에 복사한다 — 합치기 뒤. 칸이 다 찼으면
	 * 이번 갱신은 읽지 않는다(다음 갱신이 다시 적는다). 멈춤이 꺼져 있으면(StillChange < 0) 읽지 않는다.
	 */
	void AddSurfaceStillReadback(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		if (RI.StillChange < 0.f)
		{
			return;
		}
		if (FRHIGPUBufferReadback* Slot = RI.RestState->AcquireStill(RI.Serial))
		{
			AddEnqueueCopyPass(GraphBuilder, Slot, F.TileCount, (FSoftbodySurfaceRestState::StillWordIndex + 1) * sizeof(uint32));
		}
	}

	// 덩어리 평형(ADR-0012, ADR-0017): 덩어리 뿌리 스프링과 손 사이 살이 같이 버티는 자리를 뉴턴으로 푼다 — 시작(지난 프레임
	// 결과에서 이어감) 한 번, 그 뒤 (칸 합 → 한 걸음)을 NumLumpSteps번. 칸 합은 이번 프레임 손 밑 누름·쥐기 칸이 있는 덩어리 묶음만 돌고(일지 251), 합 버퍼는 시작과
	// 걸음 풀이가 비운다(ADR-0013 §8). 마지막 걸음의 결과를 비동기로 읽는다.
	/** 이 장의 덩어리 평형 판(일지 254): 국소 모양이 구워져 켜졌을 때만 큰 판(미지수 19). 시작·힘·풀이가 같은 판을 쓴다. */
	TShaderPermutationDomain<FSoftbodyLumpRegionDim> LumpPermutation(const FSoftbodySurfaceFieldRender& RI)
	{
		TShaderPermutationDomain<FSoftbodyLumpRegionDim> Permutation;
		Permutation.Set<FSoftbodyLumpRegionDim>(RI.bLumpRegion != 0u);
		return Permutation;
	}

	void AddSurfaceLumpStart(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		if (!F.LumpSlot)
		{
			return;
		}
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		// 반복 상태와 이번 프레임 상수는 SetupSurfaceFrame이 잡아 두었다(국소 모양 패스가 먼저 읽는다).
		check(F.LumpState && F.LumpGroupsSRV);
		F.LumpSums = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
			USoftbodySurfaceField::NumLumpGroups * USoftbodySurfaceField::NumLumpValues * 2), TEXT("SoftbodyLumpSums"));
		F.LumpSumsUAV = GraphBuilder.CreateUAV(F.LumpSums, PF_R32_UINT);
		F.LumpListSRV = GraphBuilder.CreateSRV(F.LumpList, PF_R32_UINT);
		// 덩어리 힘 묶음 고르기(일지 251): 덩어리 칸 묶음 가운데 이번 프레임 손 밑 누름·쥐기 칸이 있는 묶음만 — 걸음마다 덩어리 힘 패스가 이것만
		// 돈다. 덩어리 힘 패스의 간접 인자는 시작 패스가 고른 수로 쓴다(고른 묶음마다 스레드 묶음 넷이 칸 합 값을 나눠 맡는다 — z).
		FRDGBufferRef ActiveTiles = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), F.LumpList->Desc.NumElements),
			TEXT("SoftbodyLumpActiveTiles"));
		FRDGBufferRef ActiveCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("SoftbodyLumpActiveCount"));
		FRDGBufferUAVRef ActiveCountUAV = GraphBuilder.CreateUAV(ActiveCount, PF_R32_UINT);
		AddClearUAVPass(GraphBuilder, ActiveCountUAV, 0u);
		{
			FSoftbodySurfaceLumpActiveCS::FParameters* ActiveParameters = GraphBuilder.AllocParameters<FSoftbodySurfaceLumpActiveCS::FParameters>();
			ActiveParameters->CarrierPosition = F.CarrierPositionRHI;
			ActiveParameters->CarrierLump = F.CarrierLumpRHI;
			ActiveParameters->bLumpSide = RI.CarrierLumpSideA && RI.CarrierLumpSideB ? 1u : 0u;
			ActiveParameters->LumpDepthIn = F.LumpDepthTarget;
			ActiveParameters->PositionIn = F.PositionTarget;
			ActiveParameters->Resolution = RI.Resolution;
			ActiveParameters->DragRatio = RI.DragRatio;
			ActiveParameters->LumpGrip = RI.LumpGrip;
			ActiveParameters->LumpGroups = F.LumpGroupsSRV;
			ActiveParameters->LumpList = F.LumpListSRV;
			ActiveParameters->LumpListCount = F.LumpListCountSRV;
			ActiveParameters->LumpActiveTilesOut = GraphBuilder.CreateUAV(ActiveTiles, PF_R32_UINT);
			ActiveParameters->LumpActiveCountOut = ActiveCountUAV;
			ActiveParameters->IndirectArgs = F.Args;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceLumpActive"),
				TShaderMapRef<FSoftbodySurfaceLumpActiveCS>(F.ShaderMap), ActiveParameters, F.Args, ArgsOffset(ArgsLump));
		}
		F.LumpActiveTilesSRV = GraphBuilder.CreateSRV(ActiveTiles, PF_R32_UINT);
		F.LumpActiveCountSRV = GraphBuilder.CreateSRV(ActiveCount, PF_R32_UINT);
		F.LumpForceArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("SoftbodyLumpForceArgs"));
		constexpr uint32 NumLumpWords = FSoftbodyLumpReadback::NumWords;
		FRDGBufferDesc OutDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), NumLumpWords);
		OutDesc.Usage |= EBufferUsageFlags::SourceCopy;
		F.LumpOut = GraphBuilder.CreateBuffer(OutDesc, TEXT("SoftbodyLumpOut"));
		FSoftbodySurfaceLumpStartCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceLumpStartCS::FParameters>();
		Parameters->LumpGroups = F.LumpGroupsSRV;
		Parameters->LumpState = GraphBuilder.CreateUAV(F.LumpState);
		Parameters->LumpSumsOut = F.LumpSumsUAV;
		Parameters->LumpActiveCount = F.LumpActiveCountSRV;
		Parameters->LumpForceArgsOut = GraphBuilder.CreateUAV(F.LumpForceArgs, PF_R32_UINT);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceLumpStart"),
			TShaderMapRef<FSoftbodySurfaceLumpStartCS>(F.ShaderMap, LumpPermutation(RI)), Parameters, FIntVector(1, 1, 1));
	}

	void AddSurfaceLumpStep(FRDGBuilder& GraphBuilder, FSurfaceFrame& F, int32 Step, int32 NumSteps)
	{
		if (!F.LumpSlot)
		{
			return;
		}
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceLumpForceCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceLumpForceCS::FParameters>();
		Parameters->CarrierPosition = F.CarrierPositionRHI;
		Parameters->CarrierNormal = F.CarrierNormalRHI;
		Parameters->CarrierLump = F.CarrierLumpRHI;
		Parameters->CarrierLumpSideA = RHIOf(RI.CarrierLumpSideA);
		Parameters->CarrierLumpSideB = RHIOf(RI.CarrierLumpSideB);
		Parameters->bLumpSide = RI.CarrierLumpSideA && RI.CarrierLumpSideB ? 1u : 0u;
		Parameters->LumpDepthIn = F.LumpDepthTarget;
		Parameters->PositionIn = F.PositionTarget;
		Parameters->PresserTable = GraphBuilder.CreateSRV(F.Activity, PF_R32_UINT);
		Parameters->NumShapes = (uint32)(RI.Shapes.Num() / 3);
		Parameters->TexelSize = RI.TexelSize;
		Parameters->DragRatio = RI.DragRatio;
		Parameters->LumpGrip = RI.LumpGrip;
		Parameters->LumpStiffen = RI.LumpStiffen;
		Parameters->LumpGroups = F.LumpGroupsSRV;
		Parameters->LumpIterate = GraphBuilder.CreateSRV(F.LumpState);
		Parameters->Resolution = RI.Resolution;
		Parameters->ThicknessScale = RI.ThicknessScale;
		Parameters->LumpSumsOut = F.LumpSumsUAV;
		// 손 닻(일지 201): 첫 걸음이 이번 출발점에서 한도 안으로 잘라 쓰고(닻 텍스처 = UAV, 읽기 자리는 검은 텍스처 — 첫 걸음은 읽지 않는다),
		// 둘째 걸음부터 그 값을 읽는다(닻 텍스처 = SRV, 쓰기 자리는 한 칸짜리). 같은 텍스처를 한 패스에 읽기·쓰기로 같이 묶을 수 없어 자리를 바꾼다.
		const bool bFirst = Step == 0;
		Parameters->bFirstStep = bFirst ? 1u : 0u;
		Parameters->LumpAnchorIn = bFirst ? F.ReadDummy : F.LumpAnchorTarget;
		Parameters->LumpAnchorOut = GraphBuilder.CreateUAV(bFirst ? F.LumpAnchorTarget : F.LumpAnchorUnused);
		Parameters->LumpActiveTiles = F.LumpActiveTilesSRV;
		Parameters->LumpActiveCount = F.LumpActiveCountSRV;
		// 덩어리 국소 모양(일지 251): 칸마다 모양 16개(거친 칸 줄의 쌍선형). 꺼진 장은 자리만 채우고 셰이더가 읽지 않는다.
		Parameters->bLumpRegion = RI.bLumpRegion;
		Parameters->LumpRegionIndex = RHIOf(RI.LumpRegionIndex);
		Parameters->LumpRegionTable = RHIOf(RI.LumpRegionTable);
		Parameters->CoarseInfo = F.CoarseInfo;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->IndirectArgs = F.LumpForceArgs;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceLumpForce"),
			TShaderMapRef<FSoftbodySurfaceLumpForceCS>(F.ShaderMap, LumpPermutation(RI)), Parameters, F.LumpForceArgs, 0);
		FSoftbodySurfaceLumpSolveCS::FParameters* SolveParameters = GraphBuilder.AllocParameters<FSoftbodySurfaceLumpSolveCS::FParameters>();
		SolveParameters->LumpGroups = F.LumpGroupsSRV;
		SolveParameters->LumpState = GraphBuilder.CreateUAV(F.LumpState);
		SolveParameters->LumpSumsOut = F.LumpSumsUAV;
		SolveParameters->LumpOut = GraphBuilder.CreateUAV(F.LumpOut, PF_R32_FLOAT);
		SolveParameters->bLastStep = Step == NumSteps - 1 ? 1u : 0u;
		// 덩어리 묶음마다 스레드 묶음 하나(웨이브 하나가 미지수 19개 — 국소 모양이 없는 판은 3개 — 의 촐레스키를 나눠 푼다).
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceLumpSolve"),
			TShaderMapRef<FSoftbodySurfaceLumpSolveCS>(F.ShaderMap, LumpPermutation(RI)), SolveParameters,
			FIntVector(USoftbodySurfaceField::NumLumpGroups, 1, 1));
	}

	void AddSurfaceLumpReadback(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		if (!F.LumpSlot)
		{
			return;
		}
		AddEnqueueCopyPass(GraphBuilder, F.LumpSlot, F.LumpOut, FSoftbodyLumpReadback::NumWords * sizeof(float));
	}

	/**
	 * 함몰·끌림 패스가 같이 쓰는 목록 SRV와 칸 종류 표(R8, 셰이더 CarrierKind 주석). 칸 종류 표는 캐리어가 바뀔 때만 다시 만든다(표면 장이 사는
	 * 동안 남는다, 1024²에서 1 MB). 갱신마다 함몰 패스 앞에서 한 번 부른다.
	 */
	void PrepareRelaxLists(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		if (F.TileListSRV)
		{
			return;   // 갱신에 한 번(접촉 압력 단계가 함몰보다 먼저 부른다)
		}
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		F.TileListSRV = GraphBuilder.CreateSRV(F.TileList, PF_R32_UINT);
		F.TileCountSRV = GraphBuilder.CreateSRV(F.TileCount, PF_R32_UINT);
		F.TileBitsSRV = GraphBuilder.CreateSRV(F.TileBits, PF_R32_UINT);
		F.TileResidualUAV = GraphBuilder.CreateUAV(F.TileResidual, PF_R32_UINT);
		F.TileAnyResidualUAV = GraphBuilder.CreateUAV(F.TileAnyResidual, PF_R32_UINT);
		FSoftbodySurfaceRestState& Rest = *RI.RestState;
		if (!Rest.CarrierKind.IsValid() || Rest.CarrierKindSource != F.CarrierPositionRHI
			|| Rest.CarrierKind->GetDesc().Extent.X != (int32)RI.Resolution)
		{
			F.CarrierKind = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.Resolution, (int32)RI.Resolution),
				PF_R8_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCarrierKind"));
			FSoftbodySurfaceCarrierKindCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceCarrierKindCS::FParameters>();
			Parameters->CarrierPosition = F.CarrierPositionRHI;
			Parameters->Resolution = RI.Resolution;
			Parameters->CarrierKindOut = GraphBuilder.CreateUAV(F.CarrierKind);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceCarrierKind"),
				TShaderMapRef<FSoftbodySurfaceCarrierKindCS>(F.ShaderMap), Parameters, FComputeShaderUtils::GetGroupCount(
					FIntPoint((int32)RI.Resolution, (int32)RI.Resolution), FIntPoint(ThreadGroupSize, ThreadGroupSize)));
			Rest.CarrierKind = GraphBuilder.ConvertToExternalTexture(F.CarrierKind);
			Rest.CarrierKindSource = F.CarrierPositionRHI;
		}
		else
		{
			F.CarrierKind = GraphBuilder.RegisterExternalTexture(Rest.CarrierKind);
		}
	}

	/** 솟음·끌림·함몰 거친 해가 같이 쓰는 거친 칸 표(아래 정의). */
	void PrepareCoarseTables(FRDGBuilder& GraphBuilder, FSurfaceFrame& F);

	/**
	 * 부위 나누기(Separated Parts): 이번 갱신의 임시 자원을 잡는다 — 함몰 첫 패스가 쓰고 둘째 패스가 읽는 채널 텍스처(32비트), 솟음 채널(거친 칸)·압력 칸
	 * 몫(긴 몫이 돌 때만), 끌림 원천 몫. 갱신을 넘어 남는 것은 거친 칸 표(PrepareCoarseTables)뿐이다 — 함몰 채널은 갱신마다 상태 A의 함몰에서 다시 푼다.
	 * 끈 장은 읽는 자리에 검은 텍스처, 쓰는 자리에 한 칸짜리를 묶는다(순열 0 셰이더는 읽지도 쓰지도 않는다).
	 */
	void PrepareSeparation(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		F.bSeparation = RI.bSeparation != 0u;
		F.CarrierSeparationRHI = OptionalRHIOf(RI.CarrierSeparation);
		F.CarrierSeparationBind = F.CarrierSeparationRHI ? F.CarrierSeparationRHI : F.CarrierNormalRHI;
		F.CoarseSeparation = F.ReadDummy;   // 거친 칸 표는 PrepareCoarseTables가 잡는다
		const FIntPoint Extent((int32)RI.Resolution, (int32)RI.Resolution);
		const FIntPoint CoarseExtent((int32)RI.CoarseResolution, (int32)RI.CoarseResolution);
		auto Tiny = [&GraphBuilder](EPixelFormat Format, const TCHAR* Name)
		{
			return GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), Format, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV), Name);
		};
		if (F.bSeparation)
		{
			F.SeparationPass = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_A32B32G32R32F, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceSeparationPass"));
			F.SeparationUnused = Tiny(PF_A32B32G32R32F, TEXT("SoftbodySurfaceSeparationUnused"));
			F.DragSourceSeparation = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), DragSourceCapacity),
				TEXT("SoftbodySurfaceDragSourceSeparation"));
		}
		else
		{
			F.SeparationPass = F.ReadDummy;
			F.SeparationUnused = nullptr;
			F.DragSourceSeparation = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfaceDragSourceSeparationUnused"),
				TArray<FVector2f>({FVector2f(0.f, 0.f)}));
		}
		F.bLongSeparation = F.bSeparation && RI.bContactLong != 0u && RI.NumLandmarks > 0;
		if (F.bLongSeparation)
		{
			F.ContactLongSeparation = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(CoarseExtent, PF_A32B32G32R32F, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceContactLongSeparation"));
			F.PressureSeparation = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), PressureLevels * PressureCapacity),
				TEXT("SoftbodySurfacePressureSeparation"));
		}
		else
		{
			F.ContactLongSeparation = Tiny(PF_A32B32G32R32F, TEXT("SoftbodySurfaceContactLongSeparationUnused"));
			F.PressureSeparation = CreateStructuredBuffer(GraphBuilder, TEXT("SoftbodySurfacePressureSeparationUnused"),
				TArray<FVector2f>({FVector2f(0.f, 0.f)}));
		}
	}

	/** 긴 몫 읽기(함몰 목표·합치기)가 묶는 솟음 채널: 이번 갱신에 모으기가 쓴 것, 아니면 검은 텍스처(읽지 않는다). */
	FRDGTextureRef LongSeparationRead(const FSurfaceFrame& F)
	{
		return F.bLongSeparation ? F.ContactLongSeparation : F.ReadDummy;
	}

	/**
	 * 옮기기(ADR-0032): 함몰 둘째 패스 결과(D)를 상태 A로(고스트 칸은 가리키는 점의 값), 목록에서 빠지는 묶음은 0으로. AddSurfaceDentPasses 끝에 부른다.
	 */
	void AddSurfaceDentCopyPass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceDentCopyCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDentCopyCS::FParameters>();
		Parameters->CarrierPosition = F.CarrierPositionRHI;
		Parameters->TexelSize = RI.TexelSize;
		Parameters->ThicknessScale = RI.ThicknessScale;
		Parameters->DentCoarseOn = RI.bDentCoarse;
		Parameters->DentCoarse = F.DentCoarseTarget;
		Parameters->CoarseInfo = F.CoarseInfo;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->CarrierKind = F.CarrierKind;
		Parameters->CarrierRedirect = F.RedirectRHI;
		Parameters->StateIn = F.TargetD;
		Parameters->StateOut = GraphBuilder.CreateUAV(F.TargetA);
		Parameters->Resolution = RI.Resolution;
		Parameters->DentTiles = F.TileListSRV;
		Parameters->DentTileCount = F.TileCountSRV;
		Parameters->DentTileBits = F.TileBitsSRV;
		Parameters->TileResidualOut = F.TileResidualUAV;
		Parameters->TilesPerSide = F.TilesPerSide;
		Parameters->IndirectArgs = F.TileListArgs;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDentCopy"),
			TShaderMapRef<FSoftbodySurfaceDentCopyCS>(F.ShaderMap), Parameters, F.TileListArgs, ArgsOffset(ListArgsCopy));
	}

	/**
	 * 함몰 거친 해(일지 199): 거친 묶음 목록(TileListCS)만 돈다. 출발 값·아래 한계(DentCoarseRestrictCS) → 거친 패스 RI.DentCoarsePasses번(DentCoarseCS —
	 * 마지막 패스가 거친 해 U, 고침 U − Ū, 남은 표지를 쓴다). 목록 묶음의 상태 A에 더할 고침 텍스처(DentCoarseAddCS가 읽는다)를 돌려준다.
	 */
	FRDGTextureRef AddSurfaceDentCoarsePasses(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		const FIntPoint CoarseExtent((int32)RI.CoarseResolution, (int32)RI.CoarseResolution);
		auto CoarseTexture = [&GraphBuilder, &CoarseExtent](EPixelFormat Format, const TCHAR* Name)
		{
			return GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(CoarseExtent, Format, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV), Name);
		};
		FRDGTextureRef Start = CoarseTexture(PF_A32B32G32R32F, TEXT("SoftbodySurfaceDentCoarseStart"));
		FRDGTextureRef Fix = CoarseTexture(PF_R32_FLOAT, TEXT("SoftbodySurfaceDentCoarseFix"));
		FRDGTextureRef Between[2] = {nullptr, nullptr};
		// 거친 목록 밖 칸의 고침은 0이어야 한다(가는 첫 패스가 쌍선형으로 읽는다). 거친 격자 크기라 작다.
		{
			const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Fix), Zero);
		}
		FRDGBufferSRVRef ListSRV = GraphBuilder.CreateSRV(F.TileList, PF_R32_UINT);   // 뒤쪽 NumTiles부터 거친 묶음 목록
		{
			FSoftbodySurfaceDentCoarseRestrictCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDentCoarseRestrictCS::FParameters>();
			Parameters->CarrierKind = F.CarrierKind;
			Parameters->Resolution = RI.Resolution;
			Parameters->ContactIn = F.ContactTarget;
			Parameters->HoldState = GraphBuilder.CreateUAV(F.HoldTarget);
			Parameters->AcceptedOut = GraphBuilder.CreateUAV(F.AcceptedTarget);
			Parameters->HoldDecay = RI.HoldDecay;
			// 바닥 = 가는 창과 같은 목표(ADR-0037): 긴 몫·거친 압력·칸 재료
			Parameters->CarrierPosition = F.CarrierPositionRHI;
			Parameters->CarrierNormal = F.CarrierNormalRHI;
			Parameters->CarrierVolume = F.CarrierVolumeRHI ? F.CarrierVolumeRHI : F.CarrierNormalRHI;
			Parameters->bCarrierVolume = F.CarrierVolumeRHI ? 1u : 0u;
			Parameters->VolumeValue = RI.VolumeValue;
			Parameters->VolumeOffset = RI.VolumeOffset;
			Parameters->SkinLength = RI.SkinLength;
			Parameters->TexelSize = RI.TexelSize;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->ContactLong = F.ContactLongTarget;
			Parameters->bContactLong = RI.bContactLong;
			Parameters->StateIn = F.TargetA;
			Parameters->DentTileBits = F.TileBitsSRV;
			Parameters->TileResidualOut = F.TileResidualUAV;
			Parameters->TilesPerSide = F.TilesPerSide;
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseBlock = RI.CoarseBlock;
			Parameters->DentCoarse = F.DentCoarseTarget;
			Parameters->DentCoarseList = ListSRV;
			Parameters->DentCoarseListBase = F.NumTiles;
			Parameters->DentCoarseStartOut = GraphBuilder.CreateUAV(Start);
			Parameters->CarrierSeparation = F.CarrierSeparationBind;
			Parameters->bSeparation = F.bLongSeparation ? 1u : 0u;
			Parameters->ContactLongSeparation = LongSeparationRead(F);
			Parameters->IndirectArgs = F.TileListArgs;
			SetHardPieceParameters(Parameters, F);
			FSoftbodySurfaceDentCoarseRestrictCS::FPermutationDomain Permutation;
			// 조각 갈래는 Full만: Basic은 판 항이 없다(그 갈래가 없는 순열 0).
			Permutation.Set<FSoftbodyHardSurfaceDim>(F.HardMode == 1u);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDentCoarseRestrict"),
				TShaderMapRef<FSoftbodySurfaceDentCoarseRestrictCS>(F.ShaderMap, Permutation), Parameters, F.TileListArgs, ArgsOffset(ListArgsCoarse));
		}
		// 체비쇼프 범위(가는 창과 같은 식, 거친 칸 크기와 함몰 폭에서).
		const double Theta = 0.5 * (2.0 + RI.DentCoarseChebyshevLow);
		const double Half = 0.5 * (2.0 - RI.DentCoarseChebyshevLow);
		FRDGTextureUAVRef FixUAV = GraphBuilder.CreateUAV(Fix);
		FRDGBufferUAVRef BusyUAV = GraphBuilder.CreateUAV(F.DentCoarseBusy, PF_R32_UINT);
		FRDGTextureRef Input = nullptr;
		for (uint32 Pass = 1; Pass <= RI.DentCoarsePasses; ++Pass)
		{
			const bool bLast = Pass == RI.DentCoarsePasses;
			FRDGTextureRef& Slot = Between[(Pass - 1) % 2];
			if (!bLast && !Slot)
			{
				Slot = CoarseTexture(PF_R32_FLOAT, TEXT("SoftbodySurfaceDentCoarseBetween"));
			}
			FRDGTextureRef Output = bLast ? F.DentCoarseTarget : Slot;
			FSoftbodySurfaceDentCoarseCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDentCoarseCS::FParameters>();
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseTexelSize = RI.CoarseTexelSize;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->DragResolution = RI.DragResolution;
			Parameters->DragLinks = F.DragLinks;
			Parameters->DragInput = F.DragInputSRV;
			Parameters->CoarseLayer = F.CoarseLayer;
			Parameters->DentTileCount = F.TileCountSRV;
			Parameters->DentCoarseList = ListSRV;
			Parameters->DentCoarseListBase = F.NumTiles;
			Parameters->DentCoarseStart = Start;
			// 첫 패스는 출발 값(Start.x)을 읽는다 — 입력 자리에는 같은 텍스처를 묶어 둔다(읽지 않는다).
			Parameters->DentCoarseIn = Input ? Input : Start;
			Parameters->CoarseBend = F.CoarseBend;
			Parameters->ContactLong = F.ContactLongTarget;
			Parameters->ContactBend = F.ContactBendTarget;
			Parameters->bContactLong = RI.bContactLong;
			Parameters->DentCoarseOut = GraphBuilder.CreateUAV(Output);
			Parameters->DentCoarseFixOut = FixUAV;
			Parameters->DentCoarseBusyOut = BusyUAV;
			Parameters->TileAnyResidualOut = F.TileAnyResidualUAV;
			Parameters->DentCoarseFirst = Pass == 1 ? 1u : 0u;
			Parameters->DentCoarseLast = bLast ? 1u : 0u;
			Parameters->DentCoarseTheta = (float)Theta;
			Parameters->DentCoarseHalf = (float)Half;
			Parameters->IndirectArgs = F.TileListArgs;
			FSoftbodySurfaceDentCoarseCS::FPermutationDomain Permutation;
			// 위 거친 함몰 시작과 같다: 통째로 판인 거친 칸은 Full에서만 생긴다.
			Permutation.Set<FSoftbodyHardSurfaceDim>(F.HardMode == 1u);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDentCoarse"),
				TShaderMapRef<FSoftbodySurfaceDentCoarseCS>(F.ShaderMap, Permutation), Parameters, F.TileListArgs, ArgsOffset(ListArgsCoarse));
			Input = Output;
		}
		return Fix;
	}

	/**
	 * 함몰(ADR-0032) 두 패스 + 옮기기, 함몰 묶음 목록만 돈다. 먼저 거친 해(일지 199)를 풀고 목록 묶음의 A에 고침을 더한다(일지 200). 첫 패스: A에서 출발해 B에 풀고
	 * 붙들 깊이를 C에 쓴다. 둘째 패스: B에서 출발해 D에 풀고 기억·받아들인 깊이·남은 것을 제자리에서 고친다. 옮기기: D를 A로(고스트 칸은 가리키는 점의 값,
	 * 비울 묶음은 0). 두 패스 모두 목록 밖은 거친 해를 읽는다.
	 */
	void AddSurfaceDentPasses(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		PrepareRelaxLists(GraphBuilder, F);
		// 거친 칸 표(정지 위치·넓이·함몰 폭)는 거친 해와 가는 창의 거친 읽기가 쓴다. 끈 장도 함몰 패스가 묶을 자리로 표를 만든다(캐리어마다 한 번).
		PrepareCoarseTables(GraphBuilder, F);
		if (RI.bDentCoarse)
		{
			FRDGTextureRef CoarseFix = AddSurfaceDentCoarsePasses(GraphBuilder, F);
			// 목록 묶음의 상태 A를 이번 출발 값으로(칸마다 한 번 — 일지 200). 옮기기가 갱신 끝에 같은 칸을 다시 쓴다.
			FSoftbodySurfaceDentCoarseAddCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDentCoarseAddCS::FParameters>();
			Parameters->CarrierPosition = F.CarrierPositionRHI;
			Parameters->Resolution = RI.Resolution;
			Parameters->TexelSize = RI.TexelSize;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseBlock = RI.CoarseBlock;
			Parameters->DentCoarse = F.DentCoarseTarget;
			Parameters->DentCoarseFix = CoarseFix;
			Parameters->DentStateInOut = GraphBuilder.CreateUAV(F.TargetA);
			Parameters->DentTiles = F.TileListSRV;
			Parameters->DentTileCount = F.TileCountSRV;
			Parameters->TileResidualOut = F.TileResidualUAV;
			Parameters->TilesPerSide = F.TilesPerSide;
			Parameters->IndirectArgs = F.TileListArgs;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDentCoarseAdd"),
				TShaderMapRef<FSoftbodySurfaceDentCoarseAddCS>(F.ShaderMap), Parameters, F.TileListArgs, ArgsOffset(ListArgsFine));
		}
		// 체비쇼프 고윳값 범위 [Low, 2]의 가운데와 반폭(셰이더가 반복마다 운동량·걸음을 잇는다 — Saad 12.1: 첫 반복 α 0·β 1/θ, 그 뒤 ρ_{k+1} = 1/(2σ − ρ_k), α = ρ_{k+1}ρ_k, β = 2ρ_{k+1}/δ).
		const double Theta = 0.5 * (2.0 + RI.DentChebyshevLow);
		const double Delta = 0.5 * (2.0 - RI.DentChebyshevLow);
		for (uint32 Pass = 1; Pass <= 2; ++Pass)
		{
			FSoftbodySurfaceDentCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDentCS::FParameters>();
			Parameters->CarrierKind = F.CarrierKind;
			Parameters->CarrierPosition = F.CarrierPositionRHI;
			Parameters->CarrierNormal = F.CarrierNormalRHI;
			Parameters->CarrierRedirect = F.RedirectRHI;
			// 부피 보존 칠(ADR-0024): 캐리어에 칠이 있을 때만 읽는다. 없으면 자리만 채우고 셰이더는 전부 1로 본다.
			Parameters->CarrierVolume = F.CarrierVolumeRHI ? F.CarrierVolumeRHI : F.CarrierNormalRHI;
			Parameters->bCarrierVolume = F.CarrierVolumeRHI ? 1u : 0u;
			Parameters->VolumeValue = RI.VolumeValue;
			Parameters->VolumeOffset = RI.VolumeOffset;
			Parameters->SkinLength = RI.SkinLength;
			Parameters->ContactIn = F.ContactTarget;
			Parameters->PositionIn = F.PositionTarget;
			Parameters->DentCoarseOn = RI.bDentCoarse;
			Parameters->DentCoarse = F.DentCoarseTarget;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseBlock = RI.CoarseBlock;
			Parameters->StateIn = Pass == 1 ? F.TargetA : F.TargetB;
			Parameters->StateOut = GraphBuilder.CreateUAV(Pass == 1 ? F.TargetB : F.TargetD);
			// 기억·받아들인 깊이는 첫 패스가 창에서 읽기만 하고 둘째 패스가 제 칸만 고친다. 붙들 깊이는 첫 패스가 쓰고 둘째 패스가 읽는다.
			Parameters->HoldState = GraphBuilder.CreateUAV(F.HoldTarget);
			// 붙든 칸 (목표, 붙들 깊이): 첫 패스가 C에 쓰고 둘째 패스가 C를 읽는다(D3D11은 두 채널 UAV를 읽지 못해 둘째 패스는 SRV로 — 쓰기 자리는 빈 텍스처).
			Parameters->DentPins = GraphBuilder.CreateUAV(Pass == 1 ? F.TargetC : F.PinsUnused);
			Parameters->DentPinsIn = Pass == 1 ? F.ReadDummy : F.TargetC;
			Parameters->AcceptedOut = GraphBuilder.CreateUAV(F.AcceptedTarget);
			Parameters->AcceptBlend = RI.AcceptBlend;
			Parameters->AcceptReturn = RI.AcceptReturn;
			Parameters->Resolution = RI.Resolution;
			Parameters->TexelSize = RI.TexelSize;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->ContactLong = F.ContactLongTarget;
			Parameters->bContactLong = RI.bContactLong;
			Parameters->ContactBend = F.ContactBendTarget;
			Parameters->DentPass = Pass;
			Parameters->HoldDecay = RI.HoldDecay;
			Parameters->DentTheta = (float)Theta;
			Parameters->DentDelta = (float)Delta;
			Parameters->DentTiles = F.TileListSRV;
			Parameters->DentTileCount = F.TileCountSRV;
			Parameters->DentTileBits = F.TileBitsSRV;
			Parameters->TileResidualOut = F.TileResidualUAV;
			Parameters->TileAnyResidualOut = F.TileAnyResidualUAV;
			Parameters->TilesPerSide = F.TilesPerSide;
			// 부위 나누기: 첫 패스는 상태 A의 함몰에서 출발해 채널을 임시 텍스처에 쓰고, 둘째 패스가 그것을 읽는다(둘째 패스의 쓰기 자리는 한 칸짜리).
			Parameters->CarrierSeparation = F.CarrierSeparationBind;
			Parameters->bSeparation = F.bLongSeparation ? 1u : 0u;
			Parameters->ContactLongSeparation = LongSeparationRead(F);
			if (F.bSeparation)
			{
				Parameters->SeparationStateIn = Pass == 1 ? F.ReadDummy : F.SeparationPass;
				Parameters->SeparationStateOut = GraphBuilder.CreateUAV(Pass == 1 ? F.SeparationPass : F.SeparationUnused);
			}
			Parameters->IndirectArgs = F.TileListArgs;
			SetHardPieceParameters(Parameters, F);
			// 잡기(ADR-0041): 원 안 칸 용수철 줄과 원 둘레 창의 바닥 없음. 줄 수 0이면 셰이더가 건너뛴다.
			Parameters->GrabState = F.GrabStateSRV;
			Parameters->GrabCount = F.GrabCount;
			Parameters->ComponentScale = RI.ComponentScale;
			FSoftbodySurfaceDentCS::FPermutationDomain Permutation;
			Permutation.Set<FSoftbodyHardSurfaceDim>(F.HardMode == 1u);
			if (!F.bSeparation)
			{
				Parameters->DentListPart = 0u;
				Permutation.Set<FSoftbodySeparationDim>(false);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDent"),
					TShaderMapRef<FSoftbodySurfaceDentCS>(F.ShaderMap, Permutation), Parameters, F.TileListArgs, ArgsOffset(ListArgsFine));
				continue;
			}
			// 부위 나누기 장: 부위 몫이 함몰 창에 닿는 묶음(목록 앞쪽)만 채널 순열로 풀고 나머지는 채널 없는 순열로 푼다 — 그 묶음들은 채널이
			// 늘 0이라 결과가 같다(셰이더 TileListCS의 부위 나누기 주석). 같은 패스의 두 몫은 서로 다른 칸을 쓰고 서로 쓴 값을 읽지 않는다.
			FSoftbodySurfaceDentCS::FParameters* SingleParameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDentCS::FParameters>();
			*SingleParameters = *Parameters;
			SingleParameters->DentListPart = 2u;
			SingleParameters->SeparationStateIn = nullptr;
			SingleParameters->SeparationStateOut = nullptr;
			Parameters->DentListPart = 1u;
			Permutation.Set<FSoftbodySeparationDim>(true);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDentParts"),
				TShaderMapRef<FSoftbodySurfaceDentCS>(F.ShaderMap, Permutation), Parameters, F.TileListArgs, ArgsOffset(ListArgsParts));
			Permutation.Set<FSoftbodySeparationDim>(false);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDent"),
				TShaderMapRef<FSoftbodySurfaceDentCS>(F.ShaderMap, Permutation), SingleParameters, F.TileListArgs, ArgsOffset(ListArgsSingle));
		}
		AddSurfaceDentCopyPass(GraphBuilder, F);
	}

	/**
	 * 솟음·끌림이 같이 쓰는 거친 칸 표(정지 위치·넓이·두께·거리표 줄 번호): 캐리어가 바뀔 때만 다시 만든다(표면 장이 사는 동안 남는다). 갱신에 한 번만
	 * 부른다(F.CoarseRow가 잡혀 있으면 그대로).
	 */
	void PrepareCoarseTables(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		if (F.CoarseRow)
		{
			return;
		}
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceRestState& Rest = *RI.RestState;
		FRHITexture* CoarseInfoRHI = RI.CoarseInfo->GetRenderTargetTexture();
		if (!Rest.CoarseRow.IsValid() || !Rest.CoarseMaterial.IsValid() || Rest.SpreadStaticCarrier != F.CarrierPositionRHI
			|| Rest.SpreadStaticTarget != CoarseInfoRHI || Rest.SpreadStaticNormal != F.CarrierNormalRHI || Rest.SpreadStaticVolume != F.CarrierVolumeRHI
			|| Rest.SpreadStaticLandmarks != F.LandmarkDistanceRHI || Rest.SpreadStaticVolumeValue != RI.VolumeValue
			|| Rest.SpreadStaticVolumeOffset != RI.VolumeOffset
			|| Rest.SpreadStaticSkinLength != RI.SkinLength
			|| Rest.SpreadStaticSeparation != F.CarrierSeparationRHI
			|| !Rest.CoarseBend.IsValid() || !Rest.CoarseBendMax.IsValid() || Rest.SpreadStaticSoftness != F.CarrierSoftnessRHI
			|| Rest.SpreadStaticFlexOffset != RI.FlexOffset)
		{
			// 크기가 같으면 지난 표 텍스처에 다시 쓴다 — 깨어날 때(거친 벌을 풀에서 새로 잡은 첫 갱신)마다 새로 만들지 않는다(끊김 없는 켜기, 일지 277).
			const FIntPoint CoarseExtent((int32)RI.CoarseResolution, (int32)RI.CoarseResolution);
			const bool bReuseTables = Rest.CoarseRow.IsValid() && Rest.CoarseMaterial.IsValid() && Rest.CoarseRow->GetDesc().Extent == CoarseExtent
				&& Rest.CoarseMaterial->GetDesc().Extent == CoarseExtent;
			// 굽힘 길이 표는 따로 본다(이 패치 앞에 만든 표 벌에는 없다 — 그때만 새로 만든다). Flex Offset이 바뀌면 같은 텍스처에 다시 쓴다(새 할당 없음).
			const bool bReuseBend = Rest.CoarseBend.IsValid() && Rest.CoarseBend->GetDesc().Extent == CoarseExtent;
			// 부위 나누기의 거친 칸 표: 캐리어에 부위 몫이 구워져 있을 때만(켜고 끄기와 상관없이 — 콘솔로 켤 때 표를 새로 만들지 않는다).
			const bool bSeparationTable = F.CarrierSeparationRHI != nullptr;
			const bool bReuseSeparation = Rest.CoarseSeparation.IsValid() && Rest.CoarseSeparation->GetDesc().Extent == CoarseExtent;
			FRDGTextureRef CoarseSeparationOut = !bSeparationTable
				? GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_G16R16F, FClearValueBinding::None,
					TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseSeparationUnused"))
				: bReuseSeparation ? GraphBuilder.RegisterExternalTexture(Rest.CoarseSeparation)
				: GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(CoarseExtent, PF_G16R16F, FClearValueBinding::None,
					TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseSeparation"));
			F.CoarseRow = bReuseTables ? GraphBuilder.RegisterExternalTexture(Rest.CoarseRow) : GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(
				CoarseExtent, PF_R32_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseRow"));
			F.CoarseMaterial = bReuseTables ? GraphBuilder.RegisterExternalTexture(Rest.CoarseMaterial) : GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(
				CoarseExtent, PF_G32R32F, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseMaterial"));
			// 거친 칸마다 살 칸의 겉껍질 장력 길이 평균(흐리기 전) — 흐린 재료를 만들 때만 쓰는 임시 텍스처(RDG 풀).
			FRDGTextureRef CoarseSkin = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(
				CoarseExtent, PF_R32_FLOAT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseSkin"));
			// 연속 굽힘: 거친 칸 겉 굽힘 길이(흐리기 전 임시 R32F → 흐린 값 R16F, 갱신을 넘어 남는다)
			FRDGTextureRef CoarseBendRaw = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(
				CoarseExtent, PF_R32_FLOAT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseBendRaw"));
			F.CoarseBend = bReuseBend ? GraphBuilder.RegisterExternalTexture(Rest.CoarseBend) : GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(
				CoarseExtent, PF_R16F, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceCoarseBend"));
			// 굽힘 길이 최대(1 낱말, 표를 만들 때마다 0에서 원자 최대) — 갱신을 넘어 남는다
			F.CoarseBendMax = Rest.CoarseBendMax.IsValid() ? GraphBuilder.RegisterExternalBuffer(Rest.CoarseBendMax)
				: GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("SoftbodySurfaceCoarseBendMax"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(F.CoarseBendMax, PF_R32_UINT), 0u);
			// 조각 틀(강체 조각 칸은 굽힘 0) — 조각 순열이 아닌 장은 한 줄짜리 빈 버퍼(HardPieceCount 0이라 읽지 않는다)
			FRDGBufferSRVRef BendFrames = F.HardPieceFramesSRV ? F.HardPieceFramesSRV : GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder,
				TEXT("SoftbodyHardPieceFramesUnused"), TArray<FVector4f>{FVector4f(0.f, 0.f, 0.f, 0.f)}));
			{
				FSoftbodySurfaceCoarseInitCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceCoarseInitCS::FParameters>();
				Parameters->CarrierPosition = F.CarrierPositionRHI;
				Parameters->CarrierNormal = F.CarrierNormalRHI;
				Parameters->CarrierRedirect = F.RedirectRHI;
				Parameters->CarrierVolume = F.CarrierVolumeRHI ? F.CarrierVolumeRHI : F.CarrierNormalRHI;
				Parameters->bCarrierVolume = F.CarrierVolumeRHI ? 1u : 0u;
				Parameters->VolumeValue = RI.VolumeValue;
				Parameters->VolumeOffset = RI.VolumeOffset;
				Parameters->SkinLength = RI.SkinLength;
				Parameters->Resolution = RI.Resolution;
				Parameters->TexelSize = RI.TexelSize;
				Parameters->ThicknessScale = RI.ThicknessScale;
				Parameters->CoarseResolution = RI.CoarseResolution;
				Parameters->CoarseBlock = RI.CoarseBlock;
				Parameters->CoarseInfoOut = GraphBuilder.CreateUAV(F.CoarseInfo);
				Parameters->CoarseLayerOut = GraphBuilder.CreateUAV(F.CoarseLayer);
				Parameters->CoarseSkinOut = GraphBuilder.CreateUAV(CoarseSkin);
				Parameters->CarrierSeparation = F.CarrierSeparationBind;
				Parameters->bCarrierSeparation = bSeparationTable ? 1u : 0u;
				Parameters->CoarseSeparationOut = GraphBuilder.CreateUAV(CoarseSeparationOut);
				Parameters->CarrierSoftness = F.CarrierSoftnessRHI ? F.CarrierSoftnessRHI : F.CarrierNormalRHI;
				Parameters->bCarrierSoftness = F.CarrierSoftnessRHI ? 1u : 0u;
				Parameters->FlexOffset = RI.FlexOffset;
				Parameters->HardRefThickness = RI.HardRefThickness;
				Parameters->CarrierPiece = F.CarrierPieceRHI ? F.CarrierPieceRHI : F.CarrierNormalRHI;
				Parameters->HardPieceFrames = BendFrames;
				Parameters->HardPieceCount = F.HardPieceFramesSRV ? F.HardPieceCount : 0u;
				Parameters->CoarseBendRawOut = GraphBuilder.CreateUAV(CoarseBendRaw);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceCoarseInit"),
					TShaderMapRef<FSoftbodySurfaceCoarseInitCS>(F.ShaderMap), Parameters, FComputeShaderUtils::GetGroupCount(
						FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), FIntPoint(ThreadGroupSize, ThreadGroupSize)));
			}
			{
				FSoftbodySurfaceCoarseRowCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceCoarseRowCS::FParameters>();
				Parameters->CoarseResolution = RI.CoarseResolution;
				Parameters->CoarseInfo = F.CoarseInfo;
				Parameters->CoarseRowOut = GraphBuilder.CreateUAV(F.CoarseRow);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceCoarseRow"),
					TShaderMapRef<FSoftbodySurfaceCoarseRowCS>(F.ShaderMap), Parameters, FIntVector(1, 1, 1));
			}
			{
				// 흐린 거친 칸 재료(ADR-0037 §3.2): 접촉 압력 행렬·긴 몫 모으기의 쌍 재료.
				FSoftbodySurfaceCoarseMaterialCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceCoarseMaterialCS::FParameters>();
				Parameters->CoarseResolution = RI.CoarseResolution;
				Parameters->CoarseBlock = RI.CoarseBlock;
				Parameters->TexelSize = RI.TexelSize;
				Parameters->ThicknessScale = RI.ThicknessScale;
				Parameters->CoarseInfo = F.CoarseInfo;
				Parameters->CoarseLayer = F.CoarseLayer;
				Parameters->CoarseSkin = CoarseSkin;
				Parameters->CoarseMaterialOut = GraphBuilder.CreateUAV(F.CoarseMaterial);
				Parameters->CoarseBendRaw = CoarseBendRaw;
				Parameters->CoarseBendOut = GraphBuilder.CreateUAV(F.CoarseBend);
				Parameters->CoarseBendMaxOut = GraphBuilder.CreateUAV(F.CoarseBendMax, PF_R32_UINT);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceCoarseMaterial"),
					TShaderMapRef<FSoftbodySurfaceCoarseMaterialCS>(F.ShaderMap), Parameters, FComputeShaderUtils::GetGroupCount(
						FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), FIntPoint(ThreadGroupSize, ThreadGroupSize)));
			}
			if (!bReuseTables)
			{
				Rest.CoarseRow = GraphBuilder.ConvertToExternalTexture(F.CoarseRow);
				Rest.CoarseMaterial = GraphBuilder.ConvertToExternalTexture(F.CoarseMaterial);
			}
			if (!bReuseBend)
			{
				Rest.CoarseBend = GraphBuilder.ConvertToExternalTexture(F.CoarseBend);
			}
			if (!Rest.CoarseBendMax.IsValid())
			{
				Rest.CoarseBendMax = GraphBuilder.ConvertToExternalBuffer(F.CoarseBendMax);
			}
			Rest.SpreadStaticSoftness = F.CarrierSoftnessRHI;
			Rest.SpreadStaticFlexOffset = RI.FlexOffset;
			if (bSeparationTable)
			{
				if (!bReuseSeparation)
				{
					Rest.CoarseSeparation = GraphBuilder.ConvertToExternalTexture(CoarseSeparationOut);
				}
				F.CoarseSeparation = CoarseSeparationOut;
			}
			else
			{
				Rest.CoarseSeparation.SafeRelease();
			}
			Rest.SpreadStaticSeparation = F.CarrierSeparationRHI;
			Rest.SpreadStaticCarrier = F.CarrierPositionRHI;
			Rest.SpreadStaticTarget = CoarseInfoRHI;
			Rest.SpreadStaticNormal = F.CarrierNormalRHI;
			Rest.SpreadStaticVolume = F.CarrierVolumeRHI;
			Rest.SpreadStaticVolumeValue = RI.VolumeValue;
			Rest.SpreadStaticVolumeOffset = RI.VolumeOffset;
			Rest.SpreadStaticSkinLength = RI.SkinLength;
			Rest.SpreadStaticLandmarks = F.LandmarkDistanceRHI;
		}
		else
		{
			F.CoarseRow = GraphBuilder.RegisterExternalTexture(Rest.CoarseRow);
			F.CoarseMaterial = GraphBuilder.RegisterExternalTexture(Rest.CoarseMaterial);
			F.CoarseBend = GraphBuilder.RegisterExternalTexture(Rest.CoarseBend);
			F.CoarseBendMax = GraphBuilder.RegisterExternalBuffer(Rest.CoarseBendMax);
			if (Rest.CoarseSeparation.IsValid())
			{
				F.CoarseSeparation = GraphBuilder.RegisterExternalTexture(Rest.CoarseSeparation);
			}
		}
	}

	/**
	 * 덩어리 국소 모양(일지 251): 거친 칸마다 국소 모양이 지금 옮긴 양을 적는다 — 접촉 패스 앞에서(광선 출발점). 지난 덩어리 평형의 모양 크기와
	 * 지금 그리는 출렁임을 읽는다. 국소 모양이 없거나 꺼진 장은 돌지 않는다(접촉·합치기가 읽지 않는다). 거친 칸 표(정지 위치·살 넓이)가 탭 무게에
	 * 들어서, 솟음·끌림이 꺼진 장(Basic)에서도 그 표를 만든다.
	 */
	void AddSurfaceLumpRegionPass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		if (RI.bLumpRegion == 0u || !F.LumpState)
		{
			return;
		}
		PrepareCoarseTables(GraphBuilder, F);
		FSoftbodySurfaceLumpRegionCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceLumpRegionCS::FParameters>();
		Parameters->LumpRegionIndex = RHIOf(RI.LumpRegionIndex);
		Parameters->LumpRegionTable = RHIOf(RI.LumpRegionTable);
		Parameters->CoarseInfo = F.CoarseInfo;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->LumpIterate = GraphBuilder.CreateSRV(F.LumpState);
		Parameters->LumpGroups = F.LumpGroupsSRV;
		Parameters->InertiaBridge = F.InertiaBridgeSRV;
		Parameters->ComponentRotation = RI.ComponentRotation;
		Parameters->LumpRegionOffsetOut = GraphBuilder.CreateUAV(F.LumpRegionOffset);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceLumpRegion"),
			TShaderMapRef<FSoftbodySurfaceLumpRegionCS>(F.ShaderMap), Parameters, FComputeShaderUtils::GetGroupCount(
				FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), FIntPoint(ThreadGroupSize, ThreadGroupSize)));
	}

	/**
	 * 끌림(ADR-0033) 패스, 거친 격자 전체: 원천 모으기(거친 칸마다 붙든 자리·끄는 힘을 이어 고치고 원천 목록을 묶음째 만든다 — 묶음마다 뭉친 묶음 원천도,
	 * 일지 187), 바로잡기 DragCorrectPasses번(붙은 칸의 커널 합을 붙든 자리에 맞추고 묶음 원천의 끄는 힘 몫을 다시 모은다, 일지 183), 모으기(거친 8×8 묶음마다
	 * 원천 커널 합을 DragState에, 붙든 자리·끄는 힘을 DragHold 한 장으로). 바로잡기·모으기는 먼 묶음을 뭉친 원천 하나로 센다.
	 * 거리표 없는 베이크는 끌림이 꺼져(Update, bDragTable 0) 여기 오지 않는다.
	 */
	void AddSurfaceDragPasses(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		if (RI.bDragTable == 0)
		{
			return;
		}
		PrepareCoarseTables(GraphBuilder, F);
		const FIntVector CoarseGroups = FComputeShaderUtils::GetGroupCount(
			FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), FIntPoint((int32)CoarseGroup, (int32)CoarseGroup));
		// 프로파일링 스위치(일지 189).
		const uint32 DragCorrectPasses = (uint32)FMath::Clamp(CVarSoftbodyDragCorrectPasses.GetValueOnRenderThread(), 0, DragCorrectPassesMax);
		// 걸음 나누기 조각 수(일지 191): 1·2·4 셰이더 순열 가운데 하나.
		const int32 DragSlicesSetting = CVarSoftbodyDragSlices.GetValueOnRenderThread();
		TShaderPermutationDomain<FSoftbodyDragSlicesDim> DragPermutation;
		DragPermutation.Set<FSoftbodyDragSlicesDim>(DragSlicesSetting >= 4 ? 4 : (DragSlicesSetting >= 2 ? 2 : 1));
		{
			FSoftbodySurfaceDragSourceCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDragSourceCS::FParameters>();
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->LumpStiffen = RI.LumpStiffen;
			Parameters->HoldDecay = RI.HoldDecay;
			Parameters->DragResolution = RI.DragResolution;
			Parameters->DragTexelSize = RI.DragTexelSize;
			Parameters->DragReachMax = RI.DragReachMax;
			Parameters->DragLinks = F.DragLinks;
			Parameters->DragState = F.DragTargetA;
			Parameters->DragHold = F.DragHoldTarget;
			Parameters->DragInput = F.DragInputSRV;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseRow = F.CoarseRow;
			Parameters->LandmarkDistance = F.LandmarkDistanceRHI;
			Parameters->NumLandmarks = RI.NumLandmarks;
			Parameters->LandmarkRowTexels = RI.LandmarkRowTexels;
			Parameters->DragHoldNextOut = GraphBuilder.CreateUAV(F.DragHoldNext);
			Parameters->DragSourcesOut = GraphBuilder.CreateUAV(F.DragSources);
			Parameters->DragSourceQOut = GraphBuilder.CreateUAV(F.DragSourceQ[0]);
			Parameters->DragSourceCapacity = DragSourceCapacity;
			Parameters->DragHeaderOut = F.DragHeaderUAV;
			Parameters->DragClustersOut = GraphBuilder.CreateUAV(F.DragClusters);
			Parameters->DragClusterQOut = GraphBuilder.CreateUAV(F.DragClusterQ[0]);
			Parameters->DragClusterCapacity = F.DragClusterCapacity;
			Parameters->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
			Parameters->bSeparation = F.bSeparation ? 1u : 0u;
			Parameters->CoarseSeparation = F.CoarseSeparation;
			Parameters->DragSourceSeparationOut = GraphBuilder.CreateUAV(F.DragSourceSeparation);
			Parameters->GrabState = F.GrabStateSRV;
			Parameters->GrabCount = F.GrabCount;
			Parameters->ComponentScale = RI.ComponentScale;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDragSource"),
				TShaderMapRef<FSoftbodySurfaceDragSourceCS>(F.ShaderMap), Parameters, CoarseGroups);
		}
		uint32 PullIndex = 0;
		for (uint32 Pass = 0; Pass < DragCorrectPasses; ++Pass)
		{
			FSoftbodySurfaceDragCorrectCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDragCorrectCS::FParameters>();
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->DragResolution = RI.DragResolution;
			Parameters->DragTexelSize = RI.DragTexelSize;
			Parameters->DragInput = F.DragInputSRV;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseRow = F.CoarseRow;
			Parameters->LandmarkDistance = F.LandmarkDistanceRHI;
			Parameters->NumLandmarks = RI.NumLandmarks;
			Parameters->LandmarkRowTexels = RI.LandmarkRowTexels;
			Parameters->DragSources = GraphBuilder.CreateSRV(F.DragSources);
			Parameters->DragSourceQ = GraphBuilder.CreateSRV(F.DragSourceQ[PullIndex]);
			Parameters->DragSourceQOut = GraphBuilder.CreateUAV(F.DragSourceQ[1 - PullIndex]);
			Parameters->DragClusters = GraphBuilder.CreateSRV(F.DragClusters);
			Parameters->DragClusterQ = GraphBuilder.CreateSRV(F.DragClusterQ[PullIndex]);
			Parameters->DragClusterQOut = GraphBuilder.CreateUAV(F.DragClusterQ[1 - PullIndex]);
			Parameters->DragClusterCapacity = F.DragClusterCapacity;
			Parameters->DragHeader = GraphBuilder.CreateSRV(F.DragHeader, PF_R32_UINT);
			Parameters->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
			Parameters->DragHoldNext = F.DragHoldNext;
			Parameters->bSeparation = F.bSeparation ? 1u : 0u;
			Parameters->CoarseSeparation = F.CoarseSeparation;
			Parameters->DragSourceSeparation = GraphBuilder.CreateSRV(F.DragSourceSeparation);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDragCorrect"),
				TShaderMapRef<FSoftbodySurfaceDragCorrectCS>(F.ShaderMap, DragPermutation), Parameters, CoarseGroups);
			PullIndex = 1 - PullIndex;
		}
		{
			FSoftbodySurfaceDragGatherCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceDragGatherCS::FParameters>();
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->DragResolution = RI.DragResolution;
			Parameters->DragTexelSize = RI.DragTexelSize;
			Parameters->DragReachMax = RI.DragReachMax;
			Parameters->DragInput = F.DragInputSRV;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseRow = F.CoarseRow;
			Parameters->LandmarkDistance = F.LandmarkDistanceRHI;
			Parameters->NumLandmarks = RI.NumLandmarks;
			Parameters->LandmarkRowTexels = RI.LandmarkRowTexels;
			Parameters->DragSources = GraphBuilder.CreateSRV(F.DragSources);
			Parameters->DragSourceQ = GraphBuilder.CreateSRV(F.DragSourceQ[PullIndex]);
			Parameters->DragClusters = GraphBuilder.CreateSRV(F.DragClusters);
			Parameters->DragClusterQ = GraphBuilder.CreateSRV(F.DragClusterQ[PullIndex]);
			Parameters->DragClusterCapacity = F.DragClusterCapacity;
			Parameters->DragHeader = GraphBuilder.CreateSRV(F.DragHeader, PF_R32_UINT);
			// 끌림이 닿은 거친 묶음 수(stat)는 목록 수 버퍼의 셋째 낱말에 센다(원천 머리는 이 패스가 SRV로 읽는다 — 셰이더 주석).
			Parameters->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
			Parameters->DragHoldNext = F.DragHoldNext;
			Parameters->DragHoldOut = GraphBuilder.CreateUAV(F.DragHoldTarget);
			Parameters->DragStateOut = GraphBuilder.CreateUAV(F.DragTargetA);
			Parameters->bSeparation = F.bSeparation ? 1u : 0u;
			Parameters->CoarseSeparation = F.CoarseSeparation;
			Parameters->DragSourceSeparation = GraphBuilder.CreateSRV(F.DragSourceSeparation);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceDragGather"),
				TShaderMapRef<FSoftbodySurfaceDragGatherCS>(F.ShaderMap, DragPermutation), Parameters, CoarseGroups);
		}
	}

	/**
	 * 접촉 압력(ADR-0037 §3.1 ①~④), 함몰 앞: 압력 칸 목록(함몰 묶음 목록의 붙든 칸), 단계 고르기, 행렬, 풀이(한 스레드 묶음), 긴 몫 모으기(거친 격자 전체).
	 * Contact Detail Full이고 거리표가 있을 때만(RI.bContactLong) — 아니면 함몰 목표가 붙들 깊이(Basic)라 긴 몫을 읽지 않는다. 패스 다섯(목록·인자·행렬·풀이·
	 * 모으기), 반복은 풀이 패스 안에만 있다. 행렬·풀이는 칸 수가 0이면 0개 묶음으로 돈다(간접 인자).
	 */
	void AddSurfacePressurePasses(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		if (RI.bContactLong == 0u || RI.NumLandmarks == 0)
		{
			return;
		}
		PrepareRelaxLists(GraphBuilder, F);
		PrepareCoarseTables(GraphBuilder, F);
		FRDGBufferSRVRef HeaderSRV = GraphBuilder.CreateSRV(F.PressureHeader, PF_R32_UINT);
		FRDGBufferSRVRef CellsSRV = GraphBuilder.CreateSRV(F.PressureCells);
		// 하드 서피스(ADR-0037 §3.7·ADR-0038 §4, 순열 1): 조각 위 손 칸 줄·발자국 줄 판 자리·발자국 압력. 모두 갱신 안에서만 사는 RDG 임시 — 조각이 구워진
		// 장만 만든다(원칙 E06). 발자국 줄 자체는 압력 칸 배열(PressureCells)의 조각 밖 칸 뒤에 든다.
		const bool bHard = F.bHardSurface;
		FRDGBufferRef HardWork = nullptr;
		FRDGBufferRef HardHand = nullptr;
		FRDGBufferRef FRow = nullptr;
		FRDGBufferRef HardArgs = nullptr;
		FRDGTextureRef TiedPressure = nullptr;
		if (bHard)
		{
			HardWork = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), HardWorkWords), TEXT("SoftbodyHardWork"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HardWork, PF_R32_UINT), 0u);
			HardHand = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), HardHandCapacity * HardHandRows), TEXT("SoftbodyHardHand"));
			FRow = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), PressureCapacity), TEXT("SoftbodyHardFRow"));
			HardArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(2), TEXT("SoftbodyHardArgs"));
			TiedPressure = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), PF_G32R32F,
				FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodyHardTiedPressure"));
			const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TiedPressure), Zero);
		}
		{
			FSoftbodySurfaceContactPressureCellsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactPressureCellsCS::FParameters>();
			Parameters->CarrierPosition = F.CarrierPositionRHI;
			Parameters->CarrierNormal = F.CarrierNormalRHI;
			Parameters->Resolution = RI.Resolution;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->ContactIn = F.ContactTarget;
			Parameters->HoldState = GraphBuilder.CreateUAV(F.HoldTarget);
			Parameters->AcceptedOut = GraphBuilder.CreateUAV(F.AcceptedTarget);
			Parameters->HoldDecay = RI.HoldDecay;
			Parameters->PlasticShare = PlasticShare;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseRow = F.CoarseRow;
			Parameters->CoarseMaterial = F.CoarseMaterial;
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseBlock = RI.CoarseBlock;
			Parameters->LandmarkDistance = F.LandmarkDistanceRHI;
			Parameters->NumLandmarks = RI.NumLandmarks;
			Parameters->LandmarkRowTexels = RI.LandmarkRowTexels;
			Parameters->DentTiles = F.TileListSRV;
			Parameters->DentTileCount = F.TileCountSRV;
			Parameters->PressureHeaderOut = F.PressureHeaderUAV;
			Parameters->PressureCellsOut = GraphBuilder.CreateUAV(F.PressureCells);
			Parameters->ContactResponseAmounts = SoftbodyContactResponse::GetAmountTexture();
			Parameters->ContactResponseBendAmounts = SoftbodyContactResponse::GetBendAmountTexture();
			Parameters->CoarseBend = F.CoarseBend;
			Parameters->PressureSlotOut = GraphBuilder.CreateUAV(F.PressureSlot);
			Parameters->CarrierSeparation = F.CarrierSeparationBind;
			if (F.bLongSeparation)
			{
				Parameters->PressureSeparationOut = GraphBuilder.CreateUAV(F.PressureSeparation);
			}
			if (bHard)
			{
				Parameters->CarrierPiece = F.CarrierPieceRHI;
				Parameters->HardPieceFrames = F.HardPieceFramesSRV;
				Parameters->HardPieceCount = F.HardPieceCount;
				Parameters->FlexOffset = RI.FlexOffset;
				Parameters->HardWorkOut = GraphBuilder.CreateUAV(HardWork, PF_R32_UINT);
				Parameters->HardHandOut = GraphBuilder.CreateUAV(HardHand);
			}
			Parameters->IndirectArgs = F.TileListArgs;
			Parameters->GrabState = F.GrabStateSRV;
			Parameters->GrabCount = F.GrabCount;
			Parameters->ComponentScale = RI.ComponentScale;
			FSoftbodySurfaceContactPressureCellsCS::FPermutationDomain Permutation;
			Permutation.Set<FSoftbodySeparationDim>(F.bLongSeparation);
			Permutation.Set<FSoftbodyHardSurfaceDim>(bHard);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfacePressureCells"),
				TShaderMapRef<FSoftbodySurfaceContactPressureCellsCS>(F.ShaderMap, Permutation), Parameters, F.TileListArgs, ArgsOffset(ListArgsFine));
		}
		{
			FSoftbodySurfaceContactPressureArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactPressureArgsCS::FParameters>();
			Parameters->PressureHeaderOut = F.PressureHeaderUAV;
			Parameters->PressureArgsOut = GraphBuilder.CreateUAV(F.PressureArgs, PF_R32_UINT);
			if (bHard)
			{
				Parameters->PressureReachLayers = PressureReachLayers;
				Parameters->ThicknessScale = RI.ThicknessScale;
				Parameters->HardPieceFrames = F.HardPieceFramesSRV;
				Parameters->FlexOffset = RI.FlexOffset;
				Parameters->HardPieceCellRange = F.HardCellRangeSRV;
				Parameters->HardPieceCount = F.HardPieceCount;
				Parameters->HardWorkOut = GraphBuilder.CreateUAV(HardWork, PF_R32_UINT);
				Parameters->HardArgsOut = GraphBuilder.CreateUAV(HardArgs, PF_R32_UINT);
			}
			FSoftbodySurfaceContactPressureArgsCS::FPermutationDomain Permutation;
			Permutation.Set<FSoftbodyHardSurfaceDim>(bHard);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfacePressureArgs"),
				TShaderMapRef<FSoftbodySurfaceContactPressureArgsCS>(F.ShaderMap, Permutation), Parameters, FIntVector(1, 1, 1));
		}
		FRDGBufferSRVRef HardWorkSRV = bHard ? GraphBuilder.CreateSRV(HardWork, PF_R32_UINT) : nullptr;
		if (bHard)
		{
			// 펼치기(새 디스패치): 후보 조각마다 묶음 하나 — 후보가 없으면 0 묶음
			FSoftbodySurfaceContactPressurePiecesCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactPressurePiecesCS::FParameters>();
			Parameters->CarrierPosition = F.CarrierPositionRHI;
			Parameters->CarrierNormal = F.CarrierNormalRHI;
			Parameters->Resolution = RI.Resolution;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseRow = F.CoarseRow;
			Parameters->CoarseMaterial = F.CoarseMaterial;
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseBlock = RI.CoarseBlock;
			Parameters->LandmarkDistance = F.LandmarkDistanceRHI;
			Parameters->NumLandmarks = RI.NumLandmarks;
			Parameters->LandmarkRowTexels = RI.LandmarkRowTexels;
			Parameters->CarrierPiece = F.CarrierPieceRHI;
			Parameters->HardPieceFrames = F.HardPieceFramesSRV;
			Parameters->HardPieceCount = F.HardPieceCount;
			Parameters->HardWork = HardWorkSRV;
			Parameters->HardPieceCellRange = F.HardCellRangeSRV;
			Parameters->HardPieceCellList = F.HardCellListSRV;
			Parameters->PressureHeaderOut = F.PressureHeaderUAV;
			Parameters->PressureCellsOut = GraphBuilder.CreateUAV(F.PressureCells);
			Parameters->ContactResponseAmounts = SoftbodyContactResponse::GetAmountTexture();
			Parameters->ContactResponseBendAmounts = SoftbodyContactResponse::GetBendAmountTexture();
			Parameters->HardCellPlate = F.HardCellPlateSRV;
			Parameters->FRowOut = GraphBuilder.CreateUAV(FRow);
			Parameters->IndirectArgs = HardArgs;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfacePressurePieces"),
				TShaderMapRef<FSoftbodySurfaceContactPressurePiecesCS>(F.ShaderMap), Parameters, HardArgs, 0);
		}
		{
			FSoftbodySurfaceContactPressureMatrixCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactPressureMatrixCS::FParameters>();
			Parameters->PressureHeader = HeaderSRV;
			Parameters->PressureCells = CellsSRV;
			Parameters->PressureMatrixOut = GraphBuilder.CreateUAV(F.PressureMatrix, PF_R32_UINT);
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->ContactResponseLong = SoftbodyContactResponse::GetLongTexture();
			Parameters->ContactResponseSampler = SoftbodyContactResponse::GetLongSampler();
			Parameters->IndirectArgs = F.PressureArgs;
			FSoftbodySurfaceContactPressureMatrixCS::FPermutationDomain Permutation;
			Permutation.Set<FSoftbodyHardSurfaceDim>(bHard);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfacePressureMatrix"),
				TShaderMapRef<FSoftbodySurfaceContactPressureMatrixCS>(F.ShaderMap, Permutation), Parameters, F.PressureArgs, 0);
		}
		{
			FSoftbodySurfaceContactPressureSolveCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactPressureSolveCS::FParameters>();
			Parameters->PressureHeader = HeaderSRV;
			Parameters->PressureCells = CellsSRV;
			Parameters->PressureMatrix = GraphBuilder.CreateSRV(F.PressureMatrix, PF_R32_UINT);
			Parameters->PressureForceOut = GraphBuilder.CreateUAV(F.PressureForce, PF_R32_FLOAT);
			Parameters->PressureStatsOut = GraphBuilder.CreateUAV(F.PressureStats, PF_R32_UINT);
			Parameters->PressureRounds = PressureRounds;
			Parameters->PressureSteps = PressureSteps;
			if (bHard)
			{
				Parameters->ThicknessScale = RI.ThicknessScale;
				Parameters->HardWork = HardWorkSRV;
				Parameters->HardHand = GraphBuilder.CreateSRV(HardHand);
				Parameters->HardPieceCellRange = F.HardCellRangeSRV;
				Parameters->HardPieceCellList = F.HardCellListSRV;
				Parameters->FlexOffset = RI.FlexOffset;
				Parameters->HardPieceFrames = F.HardPieceFramesSRV;
				Parameters->FRow = GraphBuilder.CreateSRV(FRow);
				Parameters->HardPieceMotionOut = GraphBuilder.CreateUAV(F.HardPieceMotion);
				Parameters->TiedPressureOut = GraphBuilder.CreateUAV(TiedPressure);
			}
			Parameters->IndirectArgs = F.PressureArgs;
			FSoftbodySurfaceContactPressureSolveCS::FPermutationDomain Permutation;
			Permutation.Set<FSoftbodyHardSurfaceDim>(bHard);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfacePressureSolve"),
				TShaderMapRef<FSoftbodySurfaceContactPressureSolveCS>(F.ShaderMap, Permutation), Parameters, F.PressureArgs, sizeof(FRHIDispatchIndirectParameters));
		}
		{
			FSoftbodySurfaceContactLongGatherCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceContactLongGatherCS::FParameters>();
			Parameters->PressureHeader = HeaderSRV;
			Parameters->PressureCells = CellsSRV;
			Parameters->PressureForce = GraphBuilder.CreateSRV(F.PressureForce, PF_R32_FLOAT);
			Parameters->PressureSlot = F.PressureSlot;
			Parameters->CoarseInfo = F.CoarseInfo;
			Parameters->CoarseLayer = F.CoarseLayer;
			Parameters->CoarseRow = F.CoarseRow;
			Parameters->CoarseMaterial = F.CoarseMaterial;
			Parameters->CoarseResolution = RI.CoarseResolution;
			Parameters->CoarseBlock = RI.CoarseBlock;
			Parameters->ThicknessScale = RI.ThicknessScale;
			Parameters->LandmarkDistance = F.LandmarkDistanceRHI;
			Parameters->NumLandmarks = RI.NumLandmarks;
			Parameters->LandmarkRowTexels = RI.LandmarkRowTexels;
			Parameters->DentTileBits = F.TileBitsSRV;
			Parameters->TilesPerSide = F.TilesPerSide;
			Parameters->ContactResponseLong = SoftbodyContactResponse::GetLongTexture();
			Parameters->ContactResponseSampler = SoftbodyContactResponse::GetLongSampler();
			Parameters->PressureReachLayers = PressureReachLayers;
			Parameters->TexelSize = RI.TexelSize;
			Parameters->ContactLongOut = GraphBuilder.CreateUAV(F.ContactLongTarget);
			if (F.bLongSeparation)
			{
				Parameters->PressureSeparation = GraphBuilder.CreateSRV(F.PressureSeparation);
				Parameters->ContactLongSeparationOut = GraphBuilder.CreateUAV(F.ContactLongSeparation);
			}
			if (bHard)
			{
				Parameters->HardWork = HardWorkSRV;
				Parameters->TiedPressure = TiedPressure;
				Parameters->ContactResponseBendFull = SoftbodyContactResponse::GetBendFullTexture();
				Parameters->CoarseBend = F.CoarseBend;
				Parameters->CoarseBendMax = GraphBuilder.CreateSRV(F.CoarseBendMax, PF_R32_UINT);
				Parameters->ContactBendOut = GraphBuilder.CreateUAV(F.ContactBendTarget);
			}
			FSoftbodySurfaceContactLongGatherCS::FPermutationDomain Permutation;
			Permutation.Set<FSoftbodySeparationDim>(F.bLongSeparation);
			Permutation.Set<FSoftbodyHardSurfaceDim>(bHard);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceContactLong"),
				TShaderMapRef<FSoftbodySurfaceContactLongGatherCS>(F.ShaderMap, Permutation), Parameters, FComputeShaderUtils::GetGroupCount(
					FIntPoint((int32)RI.CoarseResolution, (int32)RI.CoarseResolution), FIntPoint((int32)CoarseGroup, (int32)CoarseGroup)));
		}
		// 디버그 캡처(Softbody.CaptureField): 사본을 뜨는 갱신에만 풀이 줄·판 상태·수렴 기록을 CPU 읽기 칸으로 복사한다(요청이 없으면 패스도 할당도 없다).
		if (RI.PressureCapture.IsValid())
		{
			FSoftbodyPressureCapture& Capture = *RI.PressureCapture;
			Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadHeader, F.PressureHeader, PressureHeaderWords * (uint32)sizeof(uint32));
			Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadCells, F.PressureCells,
				PressureLevels * PressureCapacity * PressureRows * (uint32)sizeof(FUintVector4));
			Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadForce, F.PressureForce, PressureCapacity * (uint32)sizeof(float));
			Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadStats, F.PressureStats, 2u * (uint32)sizeof(uint32));
			if (bHard)
			{
				Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadHardWork, HardWork, HardWorkWords * (uint32)sizeof(uint32));
				Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadHardHand, HardHand, HardHandCapacity * HardHandRows * (uint32)sizeof(FVector4f));
				Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadFRow, FRow, PressureCapacity * (uint32)sizeof(FVector4f));
				Capture.Queue(GraphBuilder, FSoftbodyPressureCapture::ReadPieceMotion, F.HardPieceMotion, F.HardPieceCount * (uint32)sizeof(FVector4f));
			}
			Capture.HardTable = RI.HardTable;
			Capture.FlexOffset = RI.FlexOffset;
		}
	}

	/** 합치기: 함몰 − 긴 몫(둘레 솟음) + 끌림을 머티리얼이 읽는 변위로. */
	void AddSurfaceComposePass(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceComposeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceComposeCS::FParameters>();
		Parameters->CarrierPosition = F.CarrierPositionRHI;
		Parameters->CarrierRedirect = F.RedirectRHI;
		// 부피 보존 칠(ADR-0024): 캐리어에 칠이 있을 때만 읽는다. 없으면 자리만 채우고 셰이더는 전부 1로 본다.
		Parameters->CarrierVolume = F.CarrierVolumeRHI ? F.CarrierVolumeRHI : F.CarrierNormalRHI;
		Parameters->bCarrierVolume = F.CarrierVolumeRHI ? 1u : 0u;
		Parameters->VolumeValue = RI.VolumeValue;
		Parameters->VolumeOffset = RI.VolumeOffset;
		Parameters->SkinLength = RI.SkinLength;
		Parameters->ContactIn = F.ContactTarget;
		Parameters->EdgeIn = F.EdgeTarget;
		Parameters->StateIn = F.TargetA;
		// 덩어리 국소 모양(일지 251): 접촉 패스가 광선 출발점에 넣은 것과 같은 옮긴 양을 변위로 더한다.
		Parameters->CarrierLump = F.CarrierLumpRHI;
		Parameters->bLumpRegion = RI.bLumpRegion;
		Parameters->LumpRegionOffset = F.LumpRegionOffset;
		Parameters->DentCoarseOn = RI.bDentCoarse;
		Parameters->DentCoarse = F.DentCoarseTarget;
		Parameters->DentTileBits = F.TileBitsSRV;
		Parameters->TilesPerSide = F.TilesPerSide;
		Parameters->DragBlock = RI.DragBlock;
		Parameters->DragResolution = RI.DragResolution;
		Parameters->DragLinks = F.DragLinks;
		Parameters->DragState = F.DragTargetA;
		Parameters->CoarseInfo = F.CoarseInfo;
		// 접촉 압력(ADR-0037): 긴 몫(Full만 — Basic은 셰이더가 읽지 않는다)과 함몰 첫 패스의 붙들 깊이.
		Parameters->ContactLong = F.ContactLongTarget;
		Parameters->bContactLong = RI.bContactLong;
		Parameters->DentPinsIn = F.TargetC;
		Parameters->CarrierNormal = F.CarrierNormalRHI;
		Parameters->Resolution = RI.Resolution;
		Parameters->TexelSize = RI.TexelSize;
		Parameters->ThicknessScale = RI.ThicknessScale;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->DisplacementOut = GraphBuilder.CreateUAV(F.DisplacementTarget);
		Parameters->PrevDisplacementIn = F.PreviousDisplacementTarget;
		Parameters->StillChange = RI.StillChange;
		Parameters->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
		Parameters->ContactGroups = F.ContactGroupsSRV;
		Parameters->ContactGroupCount = F.ContactGroupCountSRV;
		Parameters->CarrierSeparation = F.CarrierSeparationBind;
		Parameters->bSeparation = F.bLongSeparation ? 1u : 0u;
		Parameters->ContactLongSeparation = LongSeparationRead(F);
		Parameters->IndirectArgs = F.Args;
		SetHardPieceParameters(Parameters, F);
		// 잡기(ADR-0041): 원 둘레는 솟음 한도 없이.
		Parameters->GrabState = F.GrabStateSRV;
		Parameters->GrabCount = F.GrabCount;
		Parameters->ComponentScale = RI.ComponentScale;
		FSoftbodySurfaceComposeCS::FPermutationDomain Permutation;
		Permutation.Set<FSoftbodyHardSurfaceDim>(F.bHardSurface);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceCompose"),
			TShaderMapRef<FSoftbodySurfaceComposeCS>(F.ShaderMap, Permutation), Parameters, F.Args, ArgsOffset(ArgsListed));

		// 고스트 칸 베끼기(일지 329): 고스트 칸이 가리키는 점의 최종 변위를 쓴다. 합치기 안에서는
		// 다른 칸의 결과를 읽을 수 없고 변위 텍스처를 읽으며 쓸 수 없어 모으기(그래프 안 임시 텍스처 — 두 패스 사이에만 살아 RDG가 다른 임시 메모리와 겹쳐 쓴다)
		// → 덮어쓰기 두 패스. 칸 종류는 함몰 패스가 준비한 1바이트 표로 거른다.
		{
			PrepareRelaxLists(GraphBuilder, F);   // 칸 종류 표(함몰 패스가 이미 불렀으면 그대로)
			FRDGTextureRef GhostTarget = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint((int32)RI.Resolution, (int32)RI.Resolution),
				PF_FloatRGBA, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), TEXT("SoftbodySurfaceGhostDisplacement"));
			FSoftbodySurfaceGhostGatherCS::FParameters* Gather = GraphBuilder.AllocParameters<FSoftbodySurfaceGhostGatherCS::FParameters>();
			Gather->CarrierKind = F.CarrierKind;
			Gather->CarrierRedirect = F.RedirectRHI;
			Gather->DisplacementIn = F.DisplacementTarget;
			Gather->GhostDisplacementOut = GraphBuilder.CreateUAV(GhostTarget);
			Gather->bWallGhost = RI.bWallGhost;
			Gather->Resolution = RI.Resolution;
			Gather->ContactGroups = F.ContactGroupsSRV;
			Gather->ContactGroupCount = F.ContactGroupCountSRV;
			Gather->IndirectArgs = F.Args;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceGhostGather"),
				TShaderMapRef<FSoftbodySurfaceGhostGatherCS>(F.ShaderMap), Gather, F.Args, ArgsOffset(ArgsListed));

			FSoftbodySurfaceGhostApplyCS::FParameters* Apply = GraphBuilder.AllocParameters<FSoftbodySurfaceGhostApplyCS::FParameters>();
			Apply->CarrierKind = F.CarrierKind;
			Apply->GhostDisplacementIn = GhostTarget;
			Apply->DisplacementOut = GraphBuilder.CreateUAV(F.DisplacementTarget);
			Apply->PrevDisplacementIn = F.PreviousDisplacementTarget;
			Apply->StillChange = RI.StillChange;
			Apply->TileCountOut = GraphBuilder.CreateUAV(F.TileCount, PF_R32_UINT);
			Apply->Resolution = RI.Resolution;
			Apply->ContactGroups = F.ContactGroupsSRV;
			Apply->ContactGroupCount = F.ContactGroupCountSRV;
			Apply->IndirectArgs = F.Args;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceGhostApply"),
				TShaderMapRef<FSoftbodySurfaceGhostApplyCS>(F.ShaderMap), Apply, F.Args, ArgsOffset(ArgsListed));
		}
	}

	/** 노멀 기울기·어둡기, 그리고 쉬기 시작 프레임에만 도는 비우기(복원이 끝나 남은 아주 작은 값을 0으로). */
	void AddSurfaceNormalPasses(FRDGBuilder& GraphBuilder, FSurfaceFrame& F)
	{
		const FSoftbodySurfaceFieldRender& RI = *F.RI;
		FSoftbodySurfaceNormalCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSoftbodySurfaceNormalCS::FParameters>();
		Parameters->CarrierPosition = F.CarrierPositionRHI;
		Parameters->CarrierNormal = F.CarrierNormalRHI;
		Parameters->CarrierRedirect = F.RedirectRHI;
		Parameters->ContactIn = F.ContactTarget;
		Parameters->PositionIn = F.PositionTarget;
		Parameters->DisplacementIn = F.DisplacementTarget;
		Parameters->Resolution = RI.Resolution;
		Parameters->ThicknessScale = RI.ThicknessScale;
		Parameters->TexelSize = RI.TexelSize;
		Parameters->OcclusionStrength = RI.OcclusionStrength;
		Parameters->StateIn = F.TargetA;
		Parameters->DentCoarseOn = RI.bDentCoarse;
		Parameters->DentCoarse = F.DentCoarseTarget;
		Parameters->DentTileBits = F.TileBitsSRV;
		Parameters->TilesPerSide = F.TilesPerSide;
		Parameters->CoarseInfo = F.CoarseInfo;
		Parameters->CoarseResolution = RI.CoarseResolution;
		Parameters->CoarseBlock = RI.CoarseBlock;
		Parameters->CarrierSkinIndex = RHIOf(RI.SkinIndex);
		Parameters->CarrierSkinWeight = RHIOf(RI.SkinWeight);
		Parameters->Bones = GraphBuilder.CreateSRV(F.BoneBuffer);
		Parameters->bSkinned = RI.Bones.Num() > 0 && RI.SkinIndex && RI.SkinWeight ? 1u : 0u;
		Parameters->ComponentScale = RI.ComponentScale;
		Parameters->NormalOut = GraphBuilder.CreateUAV(F.NormalTarget);
		Parameters->LumpNormalIn = F.LumpNormalTarget;
		Parameters->LumpMode = RI.LumpNormal ? RI.LumpMode : 0u;
		Parameters->DentPinsIn = F.TargetC;
		Parameters->DentTileCount = GraphBuilder.CreateSRV(F.TileCount, PF_R32_UINT);
		Parameters->CarrierLump = F.CarrierLumpRHI;
		Parameters->bLumpRegion = RI.bLumpRegion;
		Parameters->LumpRegionOffset = F.LumpRegionOffset;
		Parameters->ContactGroups = F.ContactGroupsSRV;
		Parameters->ContactGroupCount = F.ContactGroupCountSRV;
		Parameters->IndirectArgs = F.Args;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceNormal"),
			TShaderMapRef<FSoftbodySurfaceNormalCS>(F.ShaderMap), Parameters, F.Args, ArgsOffset(ArgsListed));

		FSoftbodySurfaceClearCS::FParameters* ClearParameters = GraphBuilder.AllocParameters<FSoftbodySurfaceClearCS::FParameters>();
		ClearParameters->ClearStateA = GraphBuilder.CreateUAV(F.TargetA);
		ClearParameters->ClearDisplacement = GraphBuilder.CreateUAV(F.DisplacementTarget);
		ClearParameters->ClearPreviousDisplacement = GraphBuilder.CreateUAV(F.PreviousDisplacementTarget);
		ClearParameters->ClearNormal = GraphBuilder.CreateUAV(F.NormalTarget);
		ClearParameters->ClearAccepted = GraphBuilder.CreateUAV(F.AcceptedTarget);
		ClearParameters->ClearHold = GraphBuilder.CreateUAV(F.HoldTarget);
		ClearParameters->ClearDrag = GraphBuilder.CreateUAV(F.DragTargetA);
		ClearParameters->ClearDragHold = GraphBuilder.CreateUAV(F.DragHoldTarget);
		ClearParameters->Resolution = RI.Resolution;
		ClearParameters->DragResolution = RI.DragResolution;
		ClearParameters->IndirectArgs = F.Args;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SoftbodySurfaceClear"),
			TShaderMapRef<FSoftbodySurfaceClearCS>(F.ShaderMap), ClearParameters, F.Args, ArgsOffset(ArgsClear));

		// 디버그 캡처(Softbody.CaptureField): 이 갱신이 다 쓴 장들을 여기서 한꺼번에 뜬다(차례 SoftbodyCapture::ECopy). 접촉은 임시라(ADR-0022) 지금밖에
		// 없고, 갱신을 넘어 남는 장도 저장할 때 읽으면 그 사이 갱신의 값이라 한 폴더에 두 순간이 섞였다. 요청이 없으면 사본이 없어 패스도 없다.
		if (RI.CaptureCopies.Num() == SoftbodyCapture::NumCopies)
		{
			const FRDGTextureRef Sources[] = {F.TargetA, F.ContactTarget, F.PositionTarget, F.DisplacementTarget, F.NormalTarget, F.AcceptedTarget,
				F.HoldTarget, F.LabelsTarget, F.DragTargetA, F.ContactLongTarget, F.ContactBendTarget};
			static_assert(UE_ARRAY_COUNT(Sources) == (SIZE_T)SoftbodyCapture::NumCopies, "capture copies follow SoftbodyCapture::ECopy");
			for (int32 Index = 0; Index < SoftbodyCapture::NumCopies; ++Index)
			{
				FTextureRenderTargetResource* Copy = RI.CaptureCopies[Index];
				if (Copy && Sources[Index])
				{
					AddCopyTexturePass(GraphBuilder, Sources[Index],
						RegisterExternalTexture(GraphBuilder, Copy->GetRenderTargetTexture(), TEXT("SoftbodySurfaceCapture")), FRHICopyTextureInfo());
				}
			}
		}
	}
}

void USoftbodySurfaceField::RenderBatch(FRHICommandListImmediate& RHICmdList, TArray<FSoftbodySurfaceFieldRender>& Inputs)
{
	if (Inputs.Num() == 0)
	{
		return;
	}
	for (const FSoftbodySurfaceFieldRender& RI : Inputs)
	{
		if (RI.LumpReadback.IsValid())
		{
			RI.LumpReadback->Poll();
		}
		RI.RestState->PollStats();
		RI.RestState->PollStill();
		RI.RestState->PollGrab();
	}
	// 묶음(ADR-0021)에 인스턴스마다 패스들을 차례로 넣는다(ADR-0022). 단계마다 섞어 넣어도 5.8 D3D12의 레거시 장벽에서는 겹치지
	// 않았고(ADR-0021 §3.2), 차례로 넣으면 갱신 안에서만 쓰는 임시 텍스처(접촉·끌림 입력·덩어리 깊이·함몰 B·C·D)의 수명이 인스턴스끼리
	// 겹치지 않아 RDG 임시 할당기가 같은 메모리를 돌려 준다 — 깨어 있는 인스턴스가 몇이든 한 벌 값이다.
	// GPU 시간 범위(RDG_EVENT_SCOPE_STAT)는 블록 안에서 닫는다 — Execute 때 열린 범위가 있으면 엔진이 멈춘다. 같은 이름의 범위는
	// 통계에서 합쳐진다(통계를 볼 때만 인스턴스마다 타임스탬프가 는다).
	FRDGBuilder GraphBuilder(RHICmdList);
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	for (FSoftbodySurfaceFieldRender& RI : Inputs)
	{
		// 프로파일 범위(일지 189): ProfileGPU·Nsight에서 이 장의 패스를 액터 이름 아래 묶는다(이벤트를 켰을 때만 이름을 만든다).
		RDG_EVENT_SCOPE(GraphBuilder, "Softbody %s", RI.DebugName);
		FSurfaceFrame F;
		F.RI = &RI;
		F.ShaderMap = ShaderMap;
		SetupSurfaceFrame(GraphBuilder, F);
		PrepareSeparation(GraphBuilder, F);
		{
			// 덩어리 국소 모양(일지 251): 접촉 패스가 광선 출발점에 넣을 옮긴 양을 먼저 적는다.
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceLump, "SoftbodySurfaceLumpRegion");
			AddSurfaceLumpRegionPass(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceContact, "SoftbodySurfaceContact");
			AddSurfaceContactPass(GraphBuilder, F);
			// 잡기(ADR-0041): 접촉 패스가 고른 칸과 지금 피부 자리로 줄을 푼다 — 뒤 패스(덩어리·압력·함몰·끌림·합치기)가 이번 갱신의 당김을 읽는다.
			AddSurfaceGrabResolvePass(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceContact, "SoftbodySurfaceActivity");
			AddSurfaceActivityPasses(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceTiles, "SoftbodySurfaceTiles");
			AddSurfaceTileListPass(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceLump, "SoftbodySurfaceLump");
			AddSurfaceLumpStart(GraphBuilder, F);
			const int32 LumpSteps = FMath::Clamp(CVarSoftbodyLumpSteps.GetValueOnRenderThread(), 1, 8);
			for (int32 Step = 0; Step < LumpSteps; ++Step)
			{
				AddSurfaceLumpStep(GraphBuilder, F, Step, LumpSteps);
			}
			AddSurfaceLumpReadback(GraphBuilder, F);
		}
		{
			// 접촉 압력과 긴 몫(ADR-0037): 함몰 목표(P + L − γ·h·A₁·p)가 읽으므로 함몰 앞.
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfacePressure, "SoftbodySurfacePressure");
			AddSurfacePressurePasses(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceDent, "SoftbodySurfaceDent");
			AddSurfaceDentPasses(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceDrag, "SoftbodySurfaceDrag");
			AddSurfaceDragPasses(GraphBuilder, F);
		}
		// 통계는 끌림 원천 머리까지 다 쓴 뒤에 모은다.
		AddSurfaceStatsPass(GraphBuilder, F);
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceCompose, "SoftbodySurfaceCompose");
			AddSurfaceComposePass(GraphBuilder, F);
		}
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, SoftbodySurfaceCompose, "SoftbodySurfaceNormal");
			AddSurfaceNormalPasses(GraphBuilder, F);
		}
		// 멈춤 표지(일지 203): 합치기가 적은 눌림·바뀜 비트를 읽기 칸으로.
		AddSurfaceStillReadback(GraphBuilder, F);
	}
	GraphBuilder.Execute();
}
