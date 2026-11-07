// Copyright (c) 2026 wjsekrud

#if WITH_DEV_AUTOMATION_TESTS

#include "SoftbodyAuthoringData.h"

#include "AssetDefinitionRegistry.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoftbodyAuthoringFactoryMenuTest,
	"Softbody.Setup.AuthoringFactoryMenu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSoftbodyAuthoringFactoryMenuTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	const UAssetDefinitionRegistry* Registry =
		UAssetDefinitionRegistry::Get();
	TestNotNull(TEXT("Asset Definition registry is available"), Registry);
	if (!Registry)
	{
		return false;
	}

	TestNotNull(TEXT("Softbody Authoring Data has an Asset Definition"),
		Registry->GetAssetDefinitionForClass(
			USoftbodyAuthoringData::StaticClass()));

	const TArray<UFactory*> NewAssetFactories =
		IAssetTools::Get().GetNewAssetFactories();
	const bool bFactoryIsRegistered =
		NewAssetFactories.ContainsByPredicate([](const UFactory* Factory)
		{
			return Factory
				&& Factory->IsA<USoftbodyAuthoringDataFactory>();
		});

	TestTrue(TEXT("Softbody Authoring Data is exposed in the New Asset menu"),
		bFactoryIsRegistered);
	return !HasAnyErrors();
}

#endif
