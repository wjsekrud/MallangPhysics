// Copyright (c) 2026 wjsekrud

#include "SoftbodyAuthoringAssetDefinition.h"

#include "SoftbodyAuthoringData.h"
#include "SoftbodyAuthoringEditor.h"

#define LOCTEXT_NAMESPACE "SoftbodyAuthoringAssetDefinition"

FText UAssetDefinition_SoftbodyAuthoringData::GetAssetDisplayName() const
{
	return LOCTEXT("SoftbodyAuthoringDataDisplayName",
		"Softbody Authoring Data");
}

FLinearColor UAssetDefinition_SoftbodyAuthoringData::GetAssetColor() const
{
	return FLinearColor(FColor(67, 166, 255));
}

TSoftClassPtr<UObject>
UAssetDefinition_SoftbodyAuthoringData::GetAssetClass() const
{
	return USoftbodyAuthoringData::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_SoftbodyAuthoringData::GetAssetCategories() const
{
	static const FAssetCategoryPath Categories[] = { FAssetCategoryPath(LOCTEXT("SoftbodyCategory", "Softbody")) };
	return Categories;
}

EAssetCommandResult UAssetDefinition_SoftbodyAuthoringData::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (USoftbodyAuthoringData* Asset : OpenArgs.LoadObjects<USoftbodyAuthoringData>())
	{
		const TSharedRef<FSoftbodyAuthoringEditor> Editor = MakeShared<FSoftbodyAuthoringEditor>();
		Editor->InitEditor(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, Asset);
	}
	return EAssetCommandResult::Handled;
}

#undef LOCTEXT_NAMESPACE
