// Copyright (c) 2026 wjsekrud

#include "Softbody.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/**
 * 커널 HLSL(Shaders/*.ush)을 머티리얼 Custom 노드가 "/Plugin/SoftbodySystem/..."
 * 가상 경로로 include 할 수 있도록 매핑한다 (설계 §8.4). 머티리얼 컴파일 전에 등록돼야
 * 하므로 모듈 로딩 페이즈는 PostConfigInit.
 */
class FSoftbodyModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SoftbodySystem"));
		if (Plugin.IsValid())
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/SoftbodySystem"),
				FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
		else
		{
			// 플러그인 디스크립터 이름(SoftbodySystem.uplugin)을 바꾸면 여기서 못 찾는다 — 셰이더를 못 읽어 머티리얼과 접촉이 모두 깨진다.
			UE_LOG(LogInit, Error, TEXT("Softbody: the plugin SoftbodySystem was not found, so its shaders are not mapped. Keep the plugin file named SoftbodySystem.uplugin."));
		}
	}
};

IMPLEMENT_MODULE(FSoftbodyModule, Softbody);
