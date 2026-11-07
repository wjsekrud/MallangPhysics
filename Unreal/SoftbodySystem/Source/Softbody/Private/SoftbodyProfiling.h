// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SoftbodyProfiling.generated.h"

class UActorComponent;
class UPrimitiveComponent;

/**
 * 프로파일링 도구(일지 189). 디버그 도구라 출시에도 둔다.
 *  - 콘솔 Softbody.Stats: 표면 장마다 GPU 상태(눌림, 접촉·함몰 묶음, 끌림 원천·묶음 원천, 끌림 패스의 걸음 집계)를 로그에 한 줄씩 쓴다.
 *  - 콘솔 Softbody.Profile: ProfileGPU(이번 프레임 GPU 패스 시간, 표면 장마다 액터 이름 범위 아래)와 Softbody.Stats를 같이 남긴다.
 *  - 장면 스위치(USoftbodyScenarioSubsystem): 액터 태그 "SoftbodyScenario.<이름>"으로 묶은 장면 하나만 켜고 나머지는 숨기고 멈춘다.
 */
namespace SoftbodyProfiling
{
	/** 프로파일 명령이 GPU 상태 읽기를 잠시 켠 동안 true — stat softbody를 켜지 않아도 Softbody.Stats가 읽을 값이 생긴다. */
	SOFTBODY_API bool IsForcingStats();

	/**
	 * 끊김 시험(Softbody.HitchTest)이 프레임마다 읽고 비우는 누계(게임 스레드만 만진다): 이 플러그인이 만든·놓은 렌더 타깃과 그 바이트, 새로 올리기
	 * 시작한 접촉 지도 텍스처 수.
	 */
	struct FResourceCounters
	{
		int32 TargetsCreated = 0;
		int64 BytesCreated = 0;
		int32 TargetsReleased = 0;
		int64 BytesReleased = 0;
		int32 ContactMapRequests = 0;
	};
	SOFTBODY_API FResourceCounters& GetResourceCounters();
}

// 장면 스위치(일지 189): 프로파일링 맵에서 장면 하나씩 따로 재려고 둔다. 액터 태그 "SoftbodyScenario.<이름>"이 그 액터를 장면 <이름>에 넣는다(한 액터가
// 여러 장면에 들 수 있다 — 칸, 칸 안 물체, 주제). 장면을 고르면(콘솔 Softbody.Scenario <이름>) 다른 장면의 액터는 숨기고 충돌·틱·물리를 끈다 — 끈 소프트바디는
// 표면 장 갱신을 신청하지 않아 GPU 일이 없다. 같은 태그를 단 카메라 액터가 있으면 그 카메라로 본다(화면 크기로 정하는 갱신률이 매번 같게).
// Softbody.Scenario.Pause는 켠 장면의 움직임(애니메이션·무버·액터 틱)만 멈추고 소프트바디는 계속 돌린다(멈춘 누름의 비용).
// 태그가 없는 액터는 건드리지 않는다. 게임·PIE 월드에만 있다.
/**
 * Profiling tool: Softbody.Scenario <name> keeps only the actors tagged SoftbodyScenario.<name> on, to measure one scene at a time.
 * Actors without such a tag are left alone. Exists only in game and PIE worlds.
 */
UCLASS()
class SOFTBODY_API USoftbodyScenarioSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** 장면 태그 머리. */
	static const TCHAR* TagPrefix;

	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	/** Name = 장면 이름, "All"(모두 켬), "None"(모두 끔). 없는 이름이면 false(OutReport에 있는 장면). */
	bool Apply(const FString& Name, FString& OutReport);
	/** 켠 장면의 움직임을 멈추거나 다시 움직인다(소프트바디 컴포넌트는 계속 돈다). */
	void SetPaused(bool bPause, FString& OutReport);
	/** 맵에 있는 장면들(액터·소프트바디 수, 카메라)과 지금 고른 장면. */
	void Describe(FString& OutReport) const;

	const FString& GetActive() const { return Active; }
	bool IsPaused() const { return bPaused; }

private:
	/** 꺼 둔 액터의 원래 상태. */
	struct FFrozen
	{
		bool bHidden = false;
		bool bCollision = true;
		bool bActorTick = false;
		TArray<TWeakObjectPtr<UActorComponent>> Ticking;
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Simulating;
	};

	void Freeze(AActor* Actor);
	void Thaw(AActor* Actor, FFrozen& Saved);
	void PauseMotion();
	void ResumeMotion();
	void ViewScenario(const FString& Name);

	TMap<TWeakObjectPtr<AActor>, FFrozen> Frozen;
	/** 멈춘 움직임(켠 장면의 액터 틱과 소프트바디가 아닌 컴포넌트 틱). */
	TArray<TWeakObjectPtr<AActor>> PausedActors;
	TArray<TWeakObjectPtr<UActorComponent>> PausedComponents;
	/** 고른 장면("" = 고르지 않음 — 모두 켜짐). */
	FString Active;
	bool bPaused = false;
};
