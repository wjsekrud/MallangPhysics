// Copyright (c) 2026 wjsekrud

using UnrealBuildTool;

public class SoftbodyEditor : ModuleRules
{
	public SoftbodyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Softbody",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RHI",
			"TargetPlatform",
			"RenderCore",
			"AdvancedPreviewScene",
			"AnimationCore",
			"AnimGraph",
			"BlueprintGraph",
			"ContentBrowser",
			"ToolMenus",
			"Slate",
			"SlateCore",
			"AssetDefinition",
			"AssetRegistry",
			"AssetTools",
			"DynamicMesh",
			"Eigen",
			"GeometryAlgorithms",
			"GeometryCore",
			"GeometryFramework",
			"ImageCore",
			"InputCore",
			"MaterialEditor",
			"MeshConversion",
			"MeshDescription",
			"PhysicsUtilities",
			"PropertyEditor",
			"SkeletalMeshDescription",
			"TextureUtilitiesCommon",
			"StaticMeshDescription",
			"TetMeshing",
			"UnrealEd",
		});
	}
}
