// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SoftbodySettings.generated.h"

// 개발: 프로젝트 설정 > Plugins > Softbody. 프로젝트 전체의 품질·성능 설정(DefaultGame.ini에 저장)이다. 재료값은 컴포넌트에 있다.
/**
 * Project-wide performance settings for Softbody, saved in DefaultGame.ini. How the flesh looks and moves is set on each Softbody
 * component.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Softbody"))
class SOFTBODY_API USoftbodySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Recomputes each touched object's dents at most Contact Updates Per Second times a second, whatever the frame rate.
	 * Between updates the dents keep their last shape, so a fast hand can leave its print behind. The first touch always updates at once.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Performance", meta = (DisplayName = "Limit Contact Update Rate"))
	bool bLimitContactUpdateRate = false;

	/** How many times a second a touched object recomputes its dents when Limit Contact Update Rate is on. */
	UPROPERTY(config, EditAnywhere, Category = "Performance",
		meta = (DisplayName = "Contact Updates Per Second", EditCondition = "bLimitContactUpdateRate", ClampMin = "5", ClampMax = "240",
			UIMin = "15", UIMax = "120"))
	float MaxContactUpdateRate = 30.f;
};
