// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SoftbodyHitchTest.generated.h"

class AActor;
class USoftbodyComponent;

// 끊김 시험: 소프트바디를 켜고 끄고 스폰하는 순서를 자동으로 밟으며 프레임마다 프레임 시간, 게임·렌더·RHI 스레드·GPU 시간, 이 플러그인이
// 만들고 놓은 렌더 타깃을 적는다. 상용 게임은 플레이 중에 스폰·레벨 스트리밍·거리별 켜고 끄기를 하므로 어느 경로에서도 프레임이 끊기면 안 된다 —
// 고치기 전후를 같은 순서로 잰다.
//  - 콘솔 Softbody.HitchTest: 지금 시작한다. softbody.HitchTest.Start 1이면 플레이를 시작한 프레임부터 잰다(레벨 시작 포함).
//  - 끝나면 단계마다 요약을 로그(LogSoftbodyHitch)에, 프레임마다 값을 Saved/Profiling/SoftbodyHitch/에 CSV로 쓴다.
//  - softbody.HitchTest.Quit 1이면 끝난 뒤 게임을 닫는다(창 없는 자동 실행).
// 레벨의 소프트바디를 실제로 끄고 켜고 복제하니 시험할 때만 쓴다. 게임·PIE 월드에만 있다.
/**
 * Testing tool: Softbody.HitchTest switches the level's Softbody components on and off and spawns copies while recording frame times.
 * Results go to the log and Saved/Profiling/SoftbodyHitch. Use it only for testing. Exists only in game and PIE worlds.
 */
UCLASS()
class SOFTBODY_API USoftbodyHitchTestSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** 시험을 처음부터 시작한다. 이미 도는 중이면 false. */
	bool Start();
	bool IsRunning() const { return bRunning; }

private:
	/** 프레임 하나의 기록. 스레드 시간은 엔진이 stat unit에 쓰는 값(지난 프레임 몫, GPU는 몇 프레임 늦다). */
	struct FFrame
	{
		double Time = 0.0;
		int32 Phase = 0;
		float FrameMs = 0.f;
		float GameMs = 0.f;
		float RenderMs = 0.f;
		float RhiMs = 0.f;
		float GpuMs = 0.f;
		/** 이 프레임에 시험이 직접 부른 켜기·끄기·스폰 같은 일의 게임 스레드 시간. */
		float ActionMs = 0.f;
		int32 TargetsCreated = 0;
		float CreatedMB = 0.f;
		int32 TargetsReleased = 0;
		float ReleasedMB = 0.f;
		int32 ContactMapRequests = 0;
	};

	/** 단계 시작에 한 번 하는 일. */
	void BeginPhase(int32 Phase);
	/** 단계 안에서 시간에 맞춰 하는 일(왕복). */
	void StepPhase(int32 Phase, double PhaseTime);
	void Finish();

	void GatherComponents(TArray<USoftbodyComponent*>& Out) const;
	void SetAllActive(bool bActive);
	void SetAllContact(bool bContact);
	void SpawnCopies();
	void DestroyCopies();

	bool bRunning = false;
	bool bStartOnBeginPlay = false;
	int32 PhaseIndex = 0;
	double PhaseStart = 0.0;
	double LastTickTime = 0.0;
	/** 왕복 단계: 다음 뒤집기까지 / 뒤집은 수. */
	double NextToggle = 0.0;
	int32 Toggles = 0;
	TWeakObjectPtr<USoftbodyComponent> ToggleTarget;
	TArray<TWeakObjectPtr<AActor>> Spawned;
	/** 이번 프레임에 시험이 직접 부른 일의 게임 스레드 시간(ms). */
	double PendingActionMs = 0.0;
	TArray<FFrame> Frames;
};
