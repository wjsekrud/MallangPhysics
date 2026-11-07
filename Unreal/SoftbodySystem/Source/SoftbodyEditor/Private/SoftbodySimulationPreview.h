// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/Object.h"
#include "SoftbodySimulationPreview.generated.h"

class UAnimationAsset;
class USkeleton;

// 개발: SA 창 Simulation 탭(일지 234)의 미리보기 설정: 스켈레탈 메시가 돌릴 애니메이션, Ctrl + 좌클릭이 누르는 손가락, Ctrl + 우클릭이 잡는 원. SA 창을 닫을 때까지 남고
// 저장하지 않는다. 컴포넌트 값은 따로(창이 쥔 미리보기 컴포넌트 값) — 이것들은 게임에 없는 미리보기 도구 값이다.
/**
 * Preview tools of the Simulation tab: the animation, the pressing finger and the grab circle. They last until the window closes
 * and are not saved.
 */
UCLASS(Transient)
class USoftbodySimulationPreviewSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Plays on the skeletal mesh in a loop, so you can see the jiggle it makes. Only animations for this mesh's skeleton are listed. */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (GetAssetFilter = "ShouldHideAnimation", EditCondition = "bSkeletal", EditConditionHides, HideEditConditionToggle))
	TObjectPtr<UAnimationAsset> Animation;

	/** The radius of the round fingertip that Ctrl + left drag presses with. */
	UPROPERTY(EditAnywhere, Category = "Preview", meta = (DisplayName = "Press Tip Radius", ClampMin = "0.2", UIMax = "10.0", Units = "cm"))
	float PressRadius = 1.f;

	/** How deep the tip pushes into the surface under the cursor. It stops before the far side, and the mouse wheel changes it while you press. */
	UPROPERTY(EditAnywhere, Category = "Preview", meta = (DisplayName = "Press Depth", ClampMin = "0.0", UIMax = "10.0", Units = "cm"))
	float PressDepth = 1.f;

	// 개발(ADR-0041 §3.10): 원은 월드 cm — 화면 픽셀이면 가까이·멀리서 잡는 넓이와 늘어나는 끝이 달라졌다. 게임 Grabber의 Grab Radius와 같은 값.
	/**
	 * The radius of the skin patch that Ctrl + right drag holds. Ctrl + mouse wheel over the mesh, or [ and ], changes it.
	 * The patch and any lump under it move at most 1.3 times the smaller of this radius and the flesh thickness, then stay there.
	 */
	UPROPERTY(EditAnywhere, Category = "Preview", meta = (DisplayName = "Grab Radius", ClampMin = "0.2", UIMax = "10.0", Units = "cm"))
	float GrabRadius = 2.f;

	// 개발: 지금 시뮬레이션하는 메시가 스켈레탈인가(Animation 칸을 보일지).
	/** True when the simulated mesh is skeletal, which shows the Animation row. */
	UPROPERTY(Transient)
	bool bSkeletal = false;

	/** 애니메이션 고르기가 보여 줄 스켈레톤. */
	TWeakObjectPtr<USkeleton> Skeleton;

	// 개발: Animation 칸의 에셋 고르기 거름: 이 메시의 스켈레톤과 맞지 않는 애니메이션은 숨긴다.
	/** Hides animations that do not match this mesh's skeleton from the Animation picker. */
	UFUNCTION()
	bool ShouldHideAnimation(const FAssetData& AssetData) const;
};
