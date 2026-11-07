// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SoftbodyGrabberComponent.generated.h"

class USoftbodyComponent;
struct FHitResult;

// 개발: ADR-0041 — 이벤트 까닭은 셰이더 SB_GRAB_REASON_*과 같은 값(1 놓음, 3 못 잡음). TargetLost는 CPU 몫.
/** Why a Softbody Grabber stopped holding the skin. */
UENUM(BlueprintType)
enum class ESoftbodyGrabEndReason : uint8
{
	None = 0 UMETA(Hidden),
	/** Release was called, or the Grabber was destroyed. */
	Released = 1,
	/** No skin to hold near the grab point: nothing within 5 cm, flesh painted black in Depth, or a rigid plate. */
	Missed = 3,
	/** The grabbed Softbody was turned off, lost its contact, or was destroyed. */
	TargetLost = 4,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSoftbodyGrabEndedDelegate, ESoftbodyGrabEndReason, Reason);

// 개발(ADR-0041): 잡기 하나 = 이 컴포넌트 하나. 잡는 동안 이 컴포넌트 자리가 끄는 자리다 — Softbody 컴포넌트가 틱마다 읽는다(이 컴포넌트는 틱이 없다).
// 잡은 칸은 GPU가 다음 갱신에 고른다(찾는 거리 5 cm). bool은 CPU가 그 프레임에 아는 실패만 — 나머지는 1~3프레임 뒤 On Grab Ended(Missed).
/**
 * Holds a patch of skin on a Softbody mesh and pulls it while this component moves, like pinching a cheek. One Grabber holds one patch.
 * The patch follows up to its limit (Get Stretch) and a lump under it moves by Lump Grip. Released, the skin returns at Recovery Rate.
 */
UCLASS(ClassGroup = (Softbody), meta = (BlueprintSpawnableComponent))
class SOFTBODY_API USoftbodyGrabberComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USoftbodyGrabberComponent();

	// 개발: 원은 정지 자리(메시 로컬 × 스케일)로 잰다. 원 안 칸은 둥근 손끝처럼 붙든다(가운데 단단, 가장자리 느슨 — 셰이더 SB_GrabGrip).
	/**
	 * Radius of the held patch, in world cm. A wider patch makes a wider, rounder pull.
	 * The patch moves at most 1.3 times the smaller of this radius and the flesh thickness, and so does a lump under it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (ClampMin = "0.1", UIMin = "0.3", UIMax = "8.0", Units = "cm"))
	float GrabRadius = 1.f;

	/**
	 * Called when this Grabber stops holding: Released, Missed (no skin to hold near the grab point) or TargetLost (the Softbody was
	 * turned off or destroyed). Missed arrives 1 to 3 frames after the grab.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Softbody")
	FSoftbodyGrabEndedDelegate OnGrabEnded;

	/**
	 * Grabs the skin nearest to Location (world) until Release, on Target or else the nearest Softbody. It releases an earlier grab first.
	 * False if no Softbody is near, contact is off or not ready, or 4 grabs are held. On Grab Ended reports Missed if no skin is within 5 cm.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody", meta = (AdvancedDisplay = "Target"))
	bool GrabAtLocation(FVector Location, USoftbodyComponent* Target = nullptr);

	/** 개발(프리뷰): Grab At Location을 찾는 거리 SearchDistance(cm)로 — 렌더 메시 위를 맞힌 점은 1 cm면 눈·머리카락을 눌러 옆 피부를 잡지 않는다. */
	bool GrabAtLocationWithin(const FVector& Location, USoftbodyComponent* Target, float SearchDistance);

	/**
	 * Grabs the skin where a trace hit (Impact Point), on the side the trace came from. Uses the Softbody of the hit mesh, or the
	 * nearest one. Same results as Grab At Location.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody")
	bool GrabAtHit(const FHitResult& Hit);

	/** Lets go. The pulled skin returns at the mesh's Recovery Rate and a lump wobbles back. On Grab Ended reports Released. */
	UFUNCTION(BlueprintCallable, Category = "Softbody")
	void Release();

	/** True from a successful grab until it ends (Release, Missed or TargetLost). */
	UFUNCTION(BlueprintPure, Category = "Softbody")
	bool IsGrabbing() const;

	/** The Softbody being held, or none. */
	UFUNCTION(BlueprintPure, Category = "Softbody")
	USoftbodyComponent* GetGrabbedSoftbody() const;

	// 개발: 늘어난 길이 = 보이는 길이(잡은 자리가 잡은 뒤 움직인 길이 = 덩어리 + 표면 장, 한도 뒤 — Basic은 표면 장의 바깥 몫만), 끝 = 한도 min(h, R) × 1.3.
	// 피부와 덩어리가 함께 이 끝에 머문다(일지 350).
	/**
	 * How far the held patch has moved since the grab and the most it moves at this spot, in cm (a lump under it counts in both).
	 * The patch stays at the most, so a hand that goes farther leaves a gap. False until the skin is found; values are 1 to 3 frames late.
	 */
	UFUNCTION(BlueprintPure, Category = "Softbody")
	bool GetStretch(float& Stretch, float& MaxStretch) const;

	/** 개발: 지금 잡기의 GPU 상태(1~3프레임 늦다 — 프리뷰 도움말). 잡지 않았거나 GPU가 아직 읽지 않았으면 false. */
	struct FGrabState
	{
		/** 보이는 길이(cm) — 잡은 자리가 잡은 뒤 움직인 길이(덩어리 + 표면 장, 한도 뒤 — Basic은 표면 장의 바깥 몫만). */
		float Stretch = 0.f;
		/** 끝(cm) = 한도 min(h, R) × 1.3 — 덩어리와 피부를 합친 길이. 손이 더 가도 여기 머문다. */
		float MaxStretch = 0.f;
		/** 잡은 점의 살 두께 h(cm) — 끝을 원 반지름과 살 두께 가운데 어느 쪽이 정했는지 도움말이 쓴다. */
		float Flesh = 0.f;
		/** 잡은 뒤 덩어리(가슴 등)가 따라 나온 길이(cm) — Stretch 가운데 덩어리 몫. */
		float LumpFollow = 0.f;
		/** 잡은 점의 지금 피부 자리(월드). */
		FVector GrabbedPoint = FVector::ZeroVector;
		/** GPU가 잡은 칸을 골랐다(못 잡았으면 이벤트가 먼저 온다). */
		bool bHeld = false;
	};
	bool GetGrabState(FGrabState& Out) const;

	/** 개발: Softbody 컴포넌트가 부른다 — 그쪽에서 끝난 잡기(못 잡음·대상 사라짐). 상태를 비우고 이벤트를 낸다. */
	void NotifyGrabEnded(ESoftbodyGrabEndReason Reason, uint32 Serial);

	/** 개발: 잡은 Softbody의 잡기 칸·번호(없으면 INDEX_NONE). */
	int32 GetGrabSlot() const { return GrabSlot; }
	uint32 GetGrabSerial() const { return GrabSerial; }
	/** 개발: 마지막으로 잡지 못하거나 놓친 까닭(영어 한 문장, 프리뷰 도움말 — 잡는 데 성공하면 비운다). */
	const FString& GetLastFailure() const { return LastFailure; }

	virtual void OnUnregister() override;

private:
	/** 대상에 잡기를 신청한다(앞 잡기는 먼저 놓는다). */
	bool BeginOn(USoftbodyComponent* Target, const FVector& Location, const FVector& TraceDirection, float SearchDistance = 0.f);
	/** 잡는 점에서 가장 가까운 Softbody(대상 메시 바운드로 — 여럿이 점을 품으면 바운드가 작은 쪽, 없으면 찾는 거리 안 가장 가까운 것). */
	static USoftbodyComponent* FindSoftbodyNear(const UWorld* World, const FVector& Location, const UPrimitiveComponent* Preferred);

	TWeakObjectPtr<USoftbodyComponent> Grabbed;
	int32 GrabSlot = INDEX_NONE;
	uint32 GrabSerial = 0;
	FString LastFailure;
};
