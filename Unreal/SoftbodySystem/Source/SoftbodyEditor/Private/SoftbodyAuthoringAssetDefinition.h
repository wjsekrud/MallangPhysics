// Copyright (c) 2026 wjsekrud

#pragma once

#include "AssetDefinitionDefault.h"
#include "SoftbodyAuthoringAssetDefinition.generated.h"

/**
 * Registers Softbody Authoring Data with the Content Browser.
 *
 * The factory alone is not sufficient in UE 5.8: AssetTools filters new-asset
 * factories whose supported class has no Asset Definition or Asset Type Actions.
 */
UCLASS()
class UAssetDefinition_SoftbodyAuthoringData final
	: public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	/** 새로 만들기 메뉴의 "Softbody" 칸에 둔다(전에는 카테고리가 없어 Miscellaneous에 있었다). */
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
	/** 속성 패널 옆에 단단한 속 미리보기가 붙은 창으로 연다(일지 232). */
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
