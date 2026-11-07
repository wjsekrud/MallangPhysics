// Copyright (c) 2026 wjsekrud

// ADR-0006 정점 사이 함몰: 메시(스켈레탈·스태틱)를 나나이트 테셀레이션으로 그리는 세팅(메시·머티리얼)과 에셋 규약 검사.
#include "SoftbodyBakeLibrary.h"

#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyMeshData.h"
#include "StaticMeshCompiler.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyNanite, Log, All);

/**
 * 메시의 나나이트 설정 읽기·쓰기. 5.6에는 접근 함수가 없어 멤버를 직접 바꾸고, 5.8의 NotifyNaniteSettingsChanged와 같은 변경 알림을 보낸다.
 */
template <typename TMesh>
static FMeshNaniteSettings GetNaniteSettings(const TMesh* Mesh)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return Mesh->NaniteSettings;
#else
	return Mesh->GetNaniteSettings();
#endif
}

template <typename TMesh>
static void SetNaniteSettings(TMesh* Mesh, const FMeshNaniteSettings& Settings)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	Mesh->NaniteSettings = Settings;
	FPropertyChangedEvent Event(FindFProperty<FProperty>(TMesh::StaticClass(), GET_MEMBER_NAME_CHECKED(TMesh, NaniteSettings)));
	Mesh->PostEditChangeProperty(Event);
#else
	Mesh->SetNaniteSettings(Settings);
	Mesh->NotifyNaniteSettingsChanged();
#endif
}

namespace
{
	/** 나나이트 메시가 읽는 UV 채널 수 (NANITE_MAX_UVS). */
	constexpr int32 NaniteMaxUVs = 4;
	/**
	 * r.Nanite.DicingRate 하한. 삼각형 한 변이 화면에서 112 × DicingRate 픽셀을 넘으면 엔진이 패치를 두 번 나눠 쪼개는데,
	 * UE 5.8에서 그 경로가 스키닝 메시를 조각내고 구멍을 낸다(실측). 4면 448 픽셀까지 안전하다.
	 */
	constexpr float MinDicingRate = 4.f;
	/** 같은 정점의 두 노멀이 이보다 벌어지면 하드 노멀로 본다(cos 1°). */
	constexpr float HardNormalCos = 0.99985f;
	/**
	 * UV 보간을 끈 나나이트는 덩어리 단계를 고를 때 UV 오차를 세지 않아, 평평한 면은 가까이서도 크게 줄인 삼각형으로 그리고
	 * 그 꼭짓점 UV가 가장 가까운 원래 점 값이라 무늬 텍스처가 톱니처럼 어긋난다(일지 373). 줄인 삼각형의 변이 화면에서
	 * 이 픽셀 수 안이 되게 묶는다(엔진 Max Edge Length Factor, WPO로 변형되는 메시의 지나친 단순화를 막는 설정).
	 */
	constexpr float MaxEdgeLengthFactor = 4.f;
	const FName NormalFunctionName(TEXT("MF_SoftbodyContactNormal"));
	const FName Param_TessMagnitude(TEXT("SoftbodyTessMagnitude"));
	const FName Param_TessCenter(TEXT("SoftbodyTessCenter"));

	UMaterialExpressionMaterialFunctionCall* FindFunctionCall(UMaterial* Material, FName FunctionName)
	{
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (Call && Call->MaterialFunction && Call->MaterialFunction->GetFName() == FunctionName)
			{
				return Call;
			}
		}
		return nullptr;
	}

	/**
	 * 소프트바디 슬롯 삼각형에서 같은 정점의 노멀이 갈라진 정점 수. 나나이트는 이런 정점을 둘로 나눠 두고 테셀레이션이
	 * 각자 자기 노멀 방향으로 밀어 틈이 벌어진다(ADR-0006 §3.4).
	 */
	int32 CountHardNormalVertices(const FMeshDescription& MD, const TSet<FName>& SoftSlots)
	{
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TVertexInstanceAttributesConstRef<FVector3f> Normals =
			MD.VertexInstanceAttributes().GetAttributesRef<FVector3f>(MeshAttribute::VertexInstance::Normal);
		if (!Normals.IsValid())
		{
			return 0;
		}
		TMap<FVertexID, FVector3f> FirstNormal;
		TSet<FVertexID> Hard;
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if (!SoftSlots.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
			{
				continue;
			}
			for (const FVertexInstanceID Instance : MD.GetTriangleVertexInstances(Tri))
			{
				const FVertexID Vertex = MD.GetVertexInstanceVertex(Instance);
				const FVector3f Normal = Normals[Instance].GetSafeNormal();
				if (const FVector3f* First = FirstNormal.Find(Vertex))
				{
					if (FVector3f::DotProduct(*First, Normal) < HardNormalCos)
					{
						Hard.Add(Vertex);
					}
				}
				else
				{
					FirstNormal.Add(Vertex, Normal);
				}
			}
		}
		return Hard.Num();
	}
}

bool USoftbodyBakeLibrary::ConfigureNaniteTessellation(USoftbodyMeshData* Data, float Magnitude, float Center,
	float MaxWorldPositionOffset)
{
	USkeletalMesh* Skeletal = Data ? Data->SourceSkeletalMesh.Get() : nullptr;
	UStaticMesh* Static = Data && !Skeletal ? Data->SourceMesh.Get() : nullptr;
	FMeshDescription* MD = Skeletal ? Skeletal->GetMeshDescription(0) : Static ? Static->GetMeshDescription(0) : nullptr;
	if (!MD)
	{
		UE_LOG(LogSoftbodyNanite, Error, TEXT("ConfigureNaniteTessellation: the baked data has no mesh. Click Run Bake first."));
		return false;
	}
	const FString MeshName = Skeletal ? Skeletal->GetName() : Static->GetName();
	if (Data->TexelUVChannel >= NaniteMaxUVs)
	{
		UE_LOG(LogSoftbodyNanite, Error,
			TEXT("ConfigureNaniteTessellation: the texel UV channel is %d, but Nanite only reads UV0 to UV%d. Remove unused UV channels from the mesh and click Run Bake again."),
			Data->TexelUVChannel, NaniteMaxUVs - 1);
		return false;
	}
	Magnitude = FMath::Max(Magnitude, 0.01f);
	Center = FMath::Clamp(Center, 0.f, 1.f);
	bool bOk = true;

	// --- 머티리얼: 모든 슬롯 사용 플래그, 소프트바디 슬롯은 테셀레이션 ---
	TSet<UMaterial*> AllBases;
	TSet<UMaterial*> SoftBases;
	TSet<FName> SoftSlots;
	FString SoftLabel;
	struct FSlot
	{
		FName Name;
		FName ImportedName;
		UMaterialInterface* Material;
	};
	TArray<FSlot> Slots;
	if (Skeletal)
	{
		for (const FSkeletalMaterial& Slot : Skeletal->GetMaterials())
		{
			Slots.Add({ Slot.MaterialSlotName, Slot.ImportedMaterialSlotName, Slot.MaterialInterface });
		}
	}
	else
	{
		for (const FStaticMaterial& Slot : Static->GetStaticMaterials())
		{
			Slots.Add({ Slot.MaterialSlotName, Slot.ImportedMaterialSlotName, Slot.MaterialInterface });
		}
	}
	for (const FSlot& Slot : Slots)
	{
		UMaterialInterface* Interface = Slot.Material;
		UMaterial* Base = Interface ? Interface->GetMaterial() : nullptr;
		if (!Base)
		{
			UE_LOG(LogSoftbodyNanite, Warning, TEXT("Slot %s has no material; Nanite draws it with the default material"),
				*Slot.Name.ToString());
			continue;
		}
		if (IsTranslucentBlendMode(Base->GetBlendMode()))
		{
			UE_LOG(LogSoftbodyNanite, Error,
				TEXT("Slot %s uses translucent material %s, so the whole mesh cannot render as Nanite. Use an Opaque or Masked material on that slot."),
				*Slot.Name.ToString(), *Base->GetName());
			bOk = false;
		}
		AllBases.Add(Base);
		if (!SoftbodyBakeShared::ReadsCarrier(Interface))
		{
			continue;
		}
		SoftBases.Add(Base);
		SoftSlots.Add(Slot.Name);
		SoftSlots.Add(Slot.ImportedName);
		SoftLabel += (SoftLabel.IsEmpty() ? TEXT("") : TEXT(", ")) + Slot.Name.ToString();
		// 편집기 미리보기용 — 런타임에는 컴포넌트가 머티리얼 값과 컴포넌트 스케일로 다시 넣는다.
		if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Interface))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Param_TessMagnitude, Magnitude);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Param_TessCenter, Center);
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			Instance->MarkPackageDirty();
		}
	}
	if (SoftBases.Num() == 0)
	{
		UE_LOG(LogSoftbodyNanite, Error, TEXT("ConfigureNaniteTessellation: no material slot uses a softbody material (one that reads the contact map). Click Recreate Material, or assign the softbody material to the slots that should dent."));
		return false;
	}
	for (UMaterial* Base : AllBases)
	{
		const bool bSoft = SoftBases.Contains(Base);
		if (!SoftbodyBakeShared::IsEditableContent(Base))
		{
			// 엔진 머티리얼은 고쳐 저장할 수 없다. 슬롯에 프로젝트의 머티리얼(복제본)을 걸어야 나나이트로 그릴 수 있다.
			UE_LOG(LogSoftbodyNanite, Error,
				TEXT("%s is engine content and was left unchanged. Put a material from your project on that slot (duplicate it into your "
					"Content folder), then click Reapply Nanite Tessellation."), *Base->GetPathName());
			bOk = false;
			continue;
		}
		Base->Modify();
		SoftbodyBakeShared::SetMaterialUsage(Base, MATUSAGE_Nanite);
		if (Skeletal)
		{
			SoftbodyBakeShared::SetMaterialUsage(Base, MATUSAGE_SkeletalMesh);
		}
		if (bSoft)
		{
			UMaterialExpressionMaterialFunctionCall* Call = FindFunctionCall(Base, NormalFunctionName);
			if (!Call || !UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Displacement"), MP_Displacement))
			{
				UE_LOG(LogSoftbodyNanite, Error,
					TEXT("%s: could not connect the Displacement output of %s to the material's Displacement pin. Either the material does not call that function (click Recreate Material), or the plugin content (MF_Softbody*) is older than the plugin code (reinstall the plugin)."),
					*Base->GetName(), *NormalFunctionName.ToString());
				bOk = false;
				continue;
			}
			Base->bEnableTessellation = true;
			Base->DisplacementScaling.Magnitude = Magnitude;
			Base->DisplacementScaling.Center = Center;
			Base->MaxWorldPositionOffsetDisplacement = MaxWorldPositionOffset;
		}
		UMaterialEditingLibrary::RecompileMaterial(Base);
		Base->MarkPackageDirty();
		UE_LOG(LogSoftbodyNanite, Display, TEXT("Material %s: usage set for %s%s"), *Base->GetName(),
			Skeletal ? TEXT("Nanite and Skeletal Mesh") : TEXT("Nanite"),
			bSoft ? *FString::Printf(TEXT(", tessellation (displacement from %.2f cm inward to %.2f cm outward, max WPO %.0f cm)"),
				Center * Magnitude, (1.f - Center) * Magnitude, MaxWorldPositionOffset) : TEXT(""));
	}

	// --- 에셋 규약 검사 (저작자 몫) ---
	const int32 Morphs = Skeletal ? Skeletal->GetMorphTargets().Num() : 0;
	if (Morphs > 0)
	{
		UE_LOG(LogSoftbodyNanite, Warning,
			TEXT("The mesh has %d morph targets, and Nanite skinning ignores them. Put facial expressions on a separate head mesh and use helper bones for joint correction."), Morphs);
	}
	const int32 HardVertices = CountHardNormalVertices(*MD, SoftSlots);
	if (HardVertices > 0)
	{
		UE_LOG(LogSoftbodyNanite, Warning,
			TEXT("The softbody slots (%s) have %d vertices with hard normals; tessellation can open cracks there. Smooth those normals in your DCC tool."),
			*SoftLabel, HardVertices);
	}

	// 두 번 나누는 경로가 깨뜨리는 것은 스키닝 메시뿐이라 스태틱 메시에는 경고하지 않는다.
	if (const IConsoleVariable* DicingRate = Skeletal ? IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.DicingRate")) : nullptr)
	{
		if (DicingRate->GetFloat() < MinDicingRate)
		{
			UE_LOG(LogSoftbodyNanite, Warning,
				TEXT("r.Nanite.DicingRate is %.1f, so skinned meshes break into pieces up close. Set r.Nanite.DicingRate=%.0f or higher under [SystemSettings] in your project's DefaultEngine.ini."),
				DicingRate->GetFloat(), MinDicingRate);
		}
	}

	// --- 메시: 나나이트 켬, UV 보간 끔, 줄인 변 길이 묶음 ---
	bool bNanite = false;
	if (Skeletal)
	{
		FMeshNaniteSettings Settings = GetNaniteSettings(Skeletal);
		Settings.bEnabled = true;
		Settings.bLerpUVs = false;
		if (Settings.MaxEdgeLengthFactor <= 0.f)
		{
			Settings.MaxEdgeLengthFactor = MaxEdgeLengthFactor;   // 사용자가 둔 값은 그대로
		}
		Skeletal->Modify();
		SetNaniteSettings(Skeletal, Settings);
		FSkinnedAssetCompilingManager::Get().FinishAllCompilation();
		Skeletal->MarkPackageDirty();
		const FSkeletalMeshRenderData* RenderData = Skeletal->GetResourceForRendering();
		bNanite = RenderData && RenderData->HasValidNaniteData();
	}
	else
	{
		FMeshNaniteSettings Settings = GetNaniteSettings(Static);
		Settings.bEnabled = true;
		Settings.bLerpUVs = false;
		if (Settings.MaxEdgeLengthFactor <= 0.f)
		{
			Settings.MaxEdgeLengthFactor = MaxEdgeLengthFactor;   // 사용자가 둔 값은 그대로
		}
		Static->Modify();
		SetNaniteSettings(Static, Settings);
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
		Static->MarkPackageDirty();
		bNanite = Static->HasValidNaniteData();
	}
	if (!bNanite)
	{
		UE_LOG(LogSoftbodyNanite, Error, TEXT("%s: Nanite data was not built. Check the mesh build errors above in the log."), *MeshName);
		bOk = false;
	}
	UE_LOG(LogSoftbodyNanite, Display,
		TEXT("ConfigureNaniteTessellation %s: Nanite %s, softbody slots %s, materials %d (tessellated %d), morph targets %d, hard normal vertices %d, texel UV channel %d"),
		*MeshName, bNanite ? TEXT("on") : TEXT("failed"), *SoftLabel, AllBases.Num(), SoftBases.Num(), Morphs,
		HardVertices, Data->TexelUVChannel);
	return bOk;
}
