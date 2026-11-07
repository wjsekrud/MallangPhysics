// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SoftbodySurfaceField.h"
#include "SoftbodySurfaceBatch.generated.h"

class USoftbodyMeshData;
struct FStreamableHandle;

/**
 * 월드 묶음이 쥔 접촉 지도 하나(Mesh Data 하나의 캐리어·거리표 텍스처, 일지 222·235). 쓰는 표면 장이 있는 동안과, 마지막 장이 놓은 뒤
 * softbody.Surface.ReleaseDelay초 동안 쥔다 — 그 안에 다시 켜면 다시 읽지 않는다.
 */
USTRUCT()
struct FSoftbodyContactMapEntry
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<USoftbodyMeshData> Data;
	/** 다 올라온 텍스처(경로 순서와 상관없이 올라온 것부터). */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> Loaded;
	/** 올릴 경로들과 그 가운데 다 올라온(또는 실패한) 것의 표지. */
	TArray<FSoftObjectPath> Paths;
	TArray<bool> PathDone;
	/** 이 지도를 쓰는 표면 장 수와 마지막으로 놓은 때(초, FPlatformTime). */
	int32 Users = 0;
	double LastUsed = 0.0;
	bool bReady = false;
	bool bFailed = false;
};

/** 표면 장이 이번 프레임 갱신을 신청하며 같이 내는 정보 — 갱신률 LOD(ADR-0021)가 "누굴 돌릴지" 고르는 데 쓴다. */
struct FSoftbodySurfaceSchedule
{
	/** 메시가 화면에서 차지하는 크기(FSoftbodySurfaceFieldInput::ScreenSize). */
	float ScreenSize = 1.f;
	/** 닿을 수 있는 콜라이더·손 부위가 있다. 없으면 떼고 돌아가는 중이다. */
	bool bHasCandidates = false;
	/** 메시가 화면에 있다(FSoftbodySurfaceFieldInput::bInView). 화면에 없는 것은 맨 뒤 차례다. */
	bool bInView = true;
	/** 지난 틱엔 닿을 수 있는 것이 없었는데 이번 틱에 생겼다(또는 비운 뒤 첫 갱신). 닿는 순간은 미루지 않는다. */
	bool bNewlyTouched = false;
	/** 붙든 잡기가 있다(ADR-0041) — 갱신 예산에서 닿는 순간과 같은 앞줄에 선다(갱신 주기 상한은 누름과 같다). */
	bool bGrabbing = false;
	/** 마지막으로 GPU 갱신한 뒤 미룬 프레임 수. */
	int32 FramesSinceUpdate = 0;
	/** 마지막으로 GPU 갱신한 월드 시간(초, 음수 = 아직 없음). 갱신 주기 상한이 쓴다. */
	double LastUpdateTime = -1.0;
	/** 렌더 스레드 상태를 처음 만드는 갱신이다(켜거나 스폰한 뒤 처음, 잠든 지 오래된 뒤). 프레임마다 몇 개까지만 돈다(softbody.Surface.FirstUpdates). */
	bool bFirstUpdate = false;
	/** 갱신 주기 상한의 차례(0~1): 표면 장마다 다르게 두어 같은 프레임에 몰리지 않게 한다. */
	float RatePhase = 0.f;
};

// 월드의 표면 장 묶음(ADR-0021). 소프트바디 컴포넌트들이 틱에서 신청한 이번 프레임 갱신을 모았다가, 액터 틱이 다 끝난 뒤
// (장면을 그리기 전) 한 번에 고른다.
//   - 갱신률 LOD: 화면에서 작은 인스턴스는 몇 프레임에 한 번만 돌리고(softbody.Surface.FullRateScreenSize), 프레임마다 돌리는 수에
//     상한을 둔다(softbody.Surface.UpdateBudget, 기본 4). 미룬 프레임의 시간은 다음 갱신이 한 걸음에 푼다(USoftbodySurfaceField::Update).
//   - 갱신 주기 상한(일지 156): 켜면(프로젝트 설정 Limit Contact Update Rate) 인스턴스마다 초당 갱신 수가 그 값을 넘지 않는다. 시간을
//     초당 갱신 수 칸으로 나눠 칸이 바뀐 프레임에만 돌고, 칸의 시작은 인스턴스마다 다르게 둬 같은 프레임에 몰리지 않는다.
//   - 묶기: 고른 인스턴스들을 그래프 하나로 올린다(USoftbodySurfaceField::RenderBatch, 인스턴스마다 패스들을 차례로). 5.8 D3D12의
//     레거시 장벽에서는 섞어 넣어도 겹치지 않아(ADR-0021 §3.2) 비용은 갱신 수에 비례한다.
//   - 텍스처 풀(ADR-0022): 표면 장이 갱신을 넘어 쥐는 텍스처 한 벌(FSoftbodySurfaceTextures)을 처음 닿을 때 주고, 콜라이더 없이
//     복원이 끝나면 0으로 비운 것을 돌려받는다 — 메모리는 배치한 수가 아니라 닿은 수에 비례한다(softbody.Surface.TexturePool).
// 돌리든 미루든 신청한 표면 장마다 OnScheduled로 알려 준다(같은 프레임, 게임 스레드).
/**
 * Gathers the contact updates of the Softbody components in this world and runs them together each frame.
 * Softbody components use it on their own. You do not need to call it.
 */
UCLASS()
class SOFTBODY_API USoftbodySurfaceBatch : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	/** 게임·PIE·에디터 월드에 더해, FPreviewWorldScope 안에서 만든 에디터 미리보기 월드(SA 창 Simulation 탭, 일지 234)에 둔다. */
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	/**
	 * 이 범위 안에서 만드는 에디터 미리보기 월드에 묶음을 둔다(SA 창 Simulation 탭이 미리보기 장면을 만들 때). 모든 미리보기 월드(썸네일·다른 에셋 창)에
	 * 두면 엔진 종료 때 플러그인이 내려간 뒤에 정리되는 월드가 이 클래스를 불러 종료가 접근 위반으로 끝났다(일지 234).
	 */
	struct SOFTBODY_API FPreviewWorldScope
	{
		FPreviewWorldScope();
		~FPreviewWorldScope();
	};

	/** 이번 프레임 갱신을 신청한다(게임 스레드, 액터 틱 안). Render는 묶음이 가져간다. */
	void Enqueue(USoftbodySurfaceField* Field, FSoftbodySurfaceFieldRender&& Render, const FSoftbodySurfaceSchedule& Schedule);
	/** 아직 올리지 않은 신청을 뺀다(표면 장이 사라질 때). */
	void Cancel(const USoftbodySurfaceField* Field);

	/** 갱신률 LOD: 이 화면 크기면 몇 프레임에 한 번 도는가(1 = 매 프레임). */
	static int32 UpdateIntervalFor(float ScreenSize);
	/** 갱신 주기 상한(초당 갱신 수, 0 = 상한 없음): 콘솔 변수가 0 이상이면 그 값, 아니면 프로젝트 설정(USoftbodySettings). */
	static float MaxUpdateRate();

	/**
	 * 텍스처 풀(ADR-0022, 일지 235): 같은 종류의 쉬는 가는 벌(해상도·덩어리 유무)과 거친 벌(거친 격자 한 변)을 같이 준다(둘 다 있을 때만, 모두 0으로 비워져
	 * 있다). 없으면 false — 그 종류를 원한다고 적어 두면 풀이 프레임 예산(softbody.Surface.CreateBudget) 안에서 한 장씩 만든다. 그 자리에서 만들지 않는다:
	 * 한 프레임에 몰아 만들면 렌더 스레드와 GPU가 멈칫한다.
	 */
	bool TryAcquire(int32 Resolution, bool bLump, int32 CoarseResolution, FSoftbodySurfaceTextures& OutFine, FSoftbodySurfaceCoarseTextures& OutCoarse);
	/** 가는 벌을 0으로 비워 돌려받는다(쉬는 벌로 둔다 — 놓는 것은 Service가 ReleaseDelay 뒤에 조금씩). */
	void Release(FSoftbodySurfaceTextures&& Set);
	/** 거친 벌을 돌려받는다(다음에 잡는 장의 첫 갱신이 비우고 고정 표를 다시 만든다). */
	void ReleaseCoarse(FSoftbodySurfaceCoarseTextures&& Set);
	/**
	 * 이 종류(가는 벌·거친 벌)를 쓰는 켜진 장이 하나 늘었다: 쉬는 벌을 softbody.Surface.TexturePool 수까지 프레임 예산 안에서 미리 만들어 둔다(레벨 로드·
	 * 스폰·켤 때, 처음 닿는 순간 기다리지 않게).
	 */
	void Reserve(int32 Resolution, bool bLump, int32 CoarseResolution);
	/**
	 * 하드 서피스 조각이 있는 장이 켜졌다(게임 월드): 조각 순열의 파이프라인을 지금부터 데운다. 처음 누를 때 그 장의 접촉이 드라이버 컴파일(빈 캐시에서
	 * 10초 남짓)을 기다리지 않게 하려는 것이다. 조각이 없는 프로젝트는 부르지 않는다(원칙 E06).
	 */
	void WantHardPipelines() { bWantHardPipelines = true; }
	/**
	 * 이 종류를 쓰는 켜진 장이 하나 줄었다. 쉬는 벌은 바로 지우지 않는다 — softbody.Surface.ReleaseDelay초 동안 그 종류가 쓰이지 않으면 Service가 프레임마다
	 * 조금씩 놓는다(끄면 GPU 갱신은 바로 0, 메모리는 유예 뒤 0, 일지 222·235).
	 */
	void Unreserve(int32 Resolution, bool bLump, int32 CoarseResolution);
	/** 쉬는 벌들과 만드는 중인 벌들의 메모리(바이트, stat softbody). */
	int64 GetIdleTextureBytes() const;

	/**
	 * 접촉 지도(일지 222·235): 이 Mesh Data의 캐리어·거리표 텍스처를 쓰는 표면 장이 하나 늘었다. 올라와 있지 않으면 비동기로 나눠 올린다 — 한 번에 올리는
	 * 양을 softbody.Surface.LoadBudget MB로 묶어, 큰 지도를 한 프레임에 올리느라 렌더 스레드가 멈칫하지 않게 한다.
	 */
	void AcquireContactMap(USoftbodyMeshData* Data);
	/** 이 Mesh Data의 접촉 지도를 쓰는 장이 하나 줄었다(텍스처는 ReleaseDelay초 동안 더 쥔다). */
	void ReleaseContactMap(const USoftbodyMeshData* Data);
	/** 다 올라왔는가. 하나라도 못 올렸으면 OutFailed(한 번 경고하고 접촉을 끈 채 둔다). */
	bool IsContactMapReady(const USoftbodyMeshData* Data, bool* OutFailed = nullptr) const;
	/** 잠든 표면 장(일지 235): ReleaseDelay초가 지나면 렌더 스레드 상태를 놓는다(프레임마다 하나씩). */
	void AddSleeper(USoftbodySurfaceField* Field);

private:
	struct FPending
	{
		TWeakObjectPtr<USoftbodySurfaceField> Field;
		FSoftbodySurfaceFieldRender Render;
		FSoftbodySurfaceSchedule Schedule;
	};

	void OnPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	/** 갱신률 LOD로 돌릴 것을 고르고, 고른 것들을 그래프 하나로 올리고, 모두에게 알린다. */
	void Flush();
	/**
	 * 프레임마다(액터 틱 뒤): 원하는·모자란 벌을 프레임 예산만큼 한 장씩 만들고, 오래 쓰이지 않은 쉬는 벌을 프레임 예산만큼 놓고, 접촉 지도를 나눠 올리고
	 * 오래 쓰이지 않은 지도를 놓는다(일지 235).
	 */
	void Service();
	void ServiceTextures(double Now);
	void ServiceContactMaps(double Now);

	TArray<FPending> Pending;
	/** 텍스처 풀의 쉬는 벌들(모두 0으로 비워져 있다)과 만드는 중인 벌들. */
	UPROPERTY()
	TArray<FSoftbodySurfaceTextures> Idle;
	UPROPERTY()
	TArray<FSoftbodySurfaceCoarseTextures> IdleCoarse;
	UPROPERTY()
	TArray<FSoftbodySurfaceTextures> Building;
	UPROPERTY()
	TArray<FSoftbodySurfaceCoarseTextures> BuildingCoarse;
	/** 놓기로 한 렌더 타깃(프레임 예산만큼 GPU 메모리를 돌려준다). */
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> Retiring;
	/** 이 프레임에 돌려받아 0으로 비울 렌더 타깃(Service가 그래프 하나로 비운다). */
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> PendingClears;
	/** 종류마다: 그 종류를 쓰는 켜진 장 수(Reserve − Unreserve), 마지막으로 쓰인 때, 이번 프레임에 받지 못한 장 수. */
	struct FKindUse
	{
		int32 Users = 0;
		double LastUsed = 0.0;
		int32 Wanted = 0;
	};
	/** 가는 벌 종류(해상도 × 2 + 덩어리 유무)와 거친 벌 종류(거친 격자 한 변). */
	TMap<int32, FKindUse> FineKinds;
	TMap<int32, FKindUse> CoarseKinds;
	static int32 KindKey(int32 Resolution, bool bLump) { return Resolution * 2 + (bLump ? 1 : 0); }

	/** 쥔 접촉 지도들과 지금 올리는 중인 요청(경로마다 하나, 바이트 어림과 함께). */
	UPROPERTY()
	TArray<FSoftbodyContactMapEntry> ContactMaps;
	struct FContactMapRequest
	{
		FSoftObjectPath Path;
		int64 Bytes = 0;
		TSharedPtr<FStreamableHandle> Handle;
	};
	TArray<FContactMapRequest> ContactMapRequests;
	/** 잠든 표면 장들(렌더 스레드 상태를 아직 쥔). */
	TArray<TWeakObjectPtr<USoftbodySurfaceField>> Sleepers;
	/** 조각이 있는 장이 켜져 조각 순열 파이프라인을 데우는 중(WantHardPipelines). 준비되면 내린다. */
	bool bWantHardPipelines = false;
	FSoftbodyContactMapEntry* FindContactMap(const USoftbodyMeshData* Data);
	const FSoftbodyContactMapEntry* FindContactMap(const USoftbodyMeshData* Data) const;
	/** Pending을 모은 프레임(GFrameCounter). 올리지 못한 채 프레임이 넘어간 신청은 버린다. */
	uint64 PendingFrame = 0;
	FDelegateHandle PostActorTickHandle;
};
