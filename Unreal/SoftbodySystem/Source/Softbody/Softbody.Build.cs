// Copyright (c) 2026 wjsekrud

using UnrealBuildTool;

public class Softbody : ModuleRules
{
	public Softbody(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 헤더 폴더: Public = 게임 코드가 쓰는 API, Internal = 같은 플러그인의 SoftbodyEditor만 쓰는 헤더, Private = 이 모듈만.
		// UBT가 세 폴더를 알아서 include 경로에 넣는다.

		// DeveloperSettings: 프로젝트 설정 > Plugins > Softbody(USoftbodySettings)
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });

		// Projects/RenderCore: 플러그인 셰이더 디렉터리 매핑 (IPluginManager, AddShaderSourceDirectoryMapping)
		// RHI/RenderCore: 표면 장 글로벌 컴퓨트 셰이더와 RDG 패스(ADR-0003)
		// ImageCore: 표면 장 캡처 명령(Softbody.CaptureField)이 RT를 이미지로 읽어 저장
		// Json: 구간 기록(Softbody.Record)이 기록을 쓰고 다시 읽는다
		PrivateDependencyModuleNames.AddRange(new string[] { "Projects", "RenderCore", "RHI", "ImageCore", "Json" });
	}
}
