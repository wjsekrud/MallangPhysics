// Copyright (c) 2026 wjsekrud

#include "SoftbodyAuthoringEditor.h"

#include "AdvancedPreviewScene.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "BoneWeights.h"
#include "CompGeom/ConvexHull3.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMeshEditor.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "Generators/SphereGenerator.h"
#include "IDetailCustomization.h"
#include "IDetailsView.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "SoftbodyAuthoringData.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyComponent.h"
#include "SoftbodyGrabberComponent.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyPlateBake.h"
#include "SoftbodySimulationPreview.h"
#include "SoftbodySurfaceBatch.h"
#include "SoftbodySurfaceField.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UnrealWidget.h"
#include "Slate/SceneViewport.h"
#include "Misc/EngineVersionComparison.h"
#include "ToolMenus.h"
#include "ViewportToolbar/UnrealEdViewportToolbar.h"
#include "ViewportToolbar/UnrealEdViewportToolbarContext.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SoftbodyAuthoringEditor"

using namespace UE::Geometry;

/** 정점마다 원소 하나, 원소 번호 = 정점 번호인 색 오버레이(값 0). 5.6에는 이 함수가 없고 5.7에는 번호를 맞추는 인자가 없다. */
static void CreatePerVertexColors(FDynamicMeshColorOverlay* Colors)
{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	const FDynamicMesh3* Mesh = Colors->GetParentMesh();
	Colors->ClearElements();
	const float Zero[4] = {0.f, 0.f, 0.f, 0.f};
	for (const int32 V : Mesh->VertexIndicesItr())
	{
		Colors->InsertElement(V, Zero);
	}
	for (const int32 T : Mesh->TriangleIndicesItr())
	{
		Colors->SetTriangle(T, Mesh->GetTriangle(T));
	}
#else
	Colors->CreatePerVertex(0.f, true);
#endif
}

namespace SoftbodyAuthoringEditorLocal
{
	const FName AppId(TEXT("SoftbodyAuthoringEditorApp"));
	const FName DetailsTabId(TEXT("SoftbodyAuthoringEditor_Details"));
	const FName PreviewTabId(TEXT("SoftbodyAuthoringEditor_CorePreview"));

	/** 설정을 바꾸고 이만큼 그대로면 다시 계산한다(값을 끄는 동안 매 프레임 계산하지 않게). */
	constexpr double SettleSeconds = 0.3;

	/**
	 * 디테일 패널의 카테고리를 이름 앞 번호(1. Target ~ 9. Output) 순서로 둔다. 엔진은 버튼(Run Bake·Reapply Paint 등)이 있는 카테고리만
	 * 순서 번호를 다시 매겨, 버튼 없는 2. Bake·3. Rigid Core가 번호가 겹쳐 4. Paint 뒤로 밀렸다(일지 374, 5.6~5.8 같음).
	 */
	class FAuthoringCategoryOrder : public IDetailCustomization
	{
	public:
		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			DetailBuilder.SortCategories([](const TMap<FName, IDetailCategoryBuilder*>& Categories)
			{
				for (const TPair<FName, IDetailCategoryBuilder*>& Category : Categories)
				{
					const FString Name = Category.Key.ToString();
					int32 Dot = INDEX_NONE;
					const bool bNumbered = Name.FindChar(TEXT('.'), Dot) && Dot > 0 && Name.Left(Dot).IsNumeric();
					// 번호 없는 카테고리는 번호 붙은 것 뒤에 원래 순서대로.
					Category.Value->SetSortOrder(bNumbered ? FCString::Atoi(*Name.Left(Dot)) : 1000 + Category.Value->GetSortOrder());
				}
			});
		}
	};

	/**
	 * 미리보기가 보여 주는 것(일지 233). 음수 = 굽기 전(지금 값으로, 고치는 대로 다시 그림): RigidCore = 굽기가 붙들 속, SeparatedParts = Separated
	 * Pairs의 두 묶음(굽기와 같은 ResolveSeparatedPairs), Paint* = 슬롯 마스크의 칠 한 종류(굽기와 같은 SamplePaintKind, Hard Surface는 칠 단계와
	 * 같은 FindHardPieces가 찾을 조각). 0~5 = 마지막 굽기 결과를
	 * 컴포넌트 Debug View와 같은 색으로 — 값이 곧 Debug View Mode다(MF_SoftbodyDebugVis의 VisMode).
	 * FleshThickness = 마지막 굽기의 살 두께(ADR-0037 §3.5), LumpBase = 마지막 접촉 지도 굽기의 덩어리 받침 막(ADR-0034), HardPieces = 마지막 접촉
	 * 지도 굽기가 하드 서피스 칠 조각마다 정한 결과(ADR-0037 §3.7). 이 셋은 Debug View Mode에는 없는 이 창 전용 보기로, 머티리얼이 아니라 CPU가
	 * 정점 색으로 칠한다. 굽기 전 색 보기도 같다.
	 */
	enum class EPreviewView : int32
	{
		PaintSoftness = -6,
		PaintVolume = -5,
		PaintJiggle = -4,
		PaintDepth = -3,
		SeparatedParts = -2,
		RigidCore = -1,
		FleshFit = 0,
		CoreConnection = 1,
		ContactMapSeams = 2,
		SurfaceDistance = 3,
		Softness = 4,
		DentsBetweenVertices = 5,
		FleshThickness = 6,
		LumpBase = 7,
		HardPieces = 8,
	};

	/** 회색으로 칠하는 칠 보기가 읽는 칠 종류. 칠 보기가 아니면(또는 판 조각으로 칠하는 Softness면) 비어 있다. */
	TOptional<SoftbodyBakeShared::EPaintKind> PaintKindOf(EPreviewView View)
	{
		using SoftbodyBakeShared::EPaintKind;
		switch (View)
		{
		case EPreviewView::PaintDepth: return EPaintKind::Depth;
		case EPreviewView::PaintJiggle: return EPaintKind::Jiggle;
		case EPreviewView::PaintVolume: return EPaintKind::Volume;
		default: return {};
		}
	}

	/** 하드 서피스 조각·판 번호(1부터)마다 다른 진한 색: 황금각으로 색상을 돌리고, 빨강(감긴 조각)과 헷갈리지 않게 빨강 둘레 60도는 비운다. */
	FLinearColor HardPieceColor(int32 Number)
	{
		const float Hue = 30.f + FMath::Fmod((float)(Number - 1) * 137.508f, 300.f);
		return FLinearColor(Hue, 0.85f, 1.f).HSVToLinearRGB();
	}

	struct FPreviewViewInfo
	{
		EPreviewView View;
		FText Label;
		FText Tooltip;
		FText Legend;
		/** 접촉 지도 표(CarrierUVTex)에서 읽는 모드. */
		bool bNeedsContactMap;
	};

	/** 굽기 결과 보기의 메뉴 순서 = SA 칸 순서(2. Bake → 3. Rigid Core → 4. Paint → 6. Contact Map). */
	const EPreviewView ResultViews[] = { EPreviewView::FleshFit, EPreviewView::CoreConnection, EPreviewView::Softness,
		EPreviewView::ContactMapSeams, EPreviewView::SurfaceDistance, EPreviewView::DentsBetweenVertices, EPreviewView::FleshThickness,
		EPreviewView::LumpBase, EPreviewView::HardPieces };

	/** 칠 보기의 메뉴 순서 = 슬롯 마스크 칸 순서. */
	const EPreviewView PaintViews[] = { EPreviewView::PaintDepth, EPreviewView::PaintJiggle, EPreviewView::PaintVolume, EPreviewView::PaintSoftness };

	const FPreviewViewInfo& ViewInfo(EPreviewView View)
	{
		// 색 설명은 컴포넌트 Debug View Mode 툴팁과 같은 말(SoftbodyVis.ush).
		static const FPreviewViewInfo Infos[] = {
			{ EPreviewView::RigidCore, LOCTEXT("ViewRigidCore", "Rigid Core"),
				LOCTEXT("ViewRigidCoreTip", "What Run Bake will hold still, with the current values. It updates as you edit."),
				FText::GetEmpty(), false },
			{ EPreviewView::FleshFit, LOCTEXT("ViewFleshFit", "Flesh Fit"),
				LOCTEXT("ViewFleshFitTip", "Last bake: how well each vertex sits in the measured flesh. Same colors as Debug View Mode 0."),
				LOCTEXT("LegendFleshFit", "Green: inside the measured flesh. Yellow: estimated. Red: failed."), false },
			{ EPreviewView::CoreConnection, LOCTEXT("ViewCoreConnection", "Core Connection"),
				LOCTEXT("ViewCoreConnectionTip", "Last bake: whether each vertex connects to the rigid core. Same colors as Debug View Mode 1."),
				LOCTEXT("LegendCoreConnection", "Cyan: connected to the rigid core. Magenta: not connected."), false },
			{ EPreviewView::ContactMapSeams, LOCTEXT("ViewSeams", "Contact Map Seams"),
				LOCTEXT("ViewSeamsTip", "Last bake: the contact map as a checker pattern. Same colors as Debug View Mode 2."),
				LOCTEXT("LegendSeams", "Parts with the same pattern as the skin under them dent together. Lines where the pattern breaks are seams. Gray: not pressable."), true },
			{ EPreviewView::SurfaceDistance, LOCTEXT("ViewDistance", "Distance to Outer Surface"),
				LOCTEXT("ViewDistanceTip", "Last bake: how far each vertex is from the outer surface. Same colors as Debug View Mode 3."),
				LOCTEXT("LegendDistance", "Green: on the outer surface. Red: far outside. Blue: deep inside. Full color at 10 cm."), true },
			{ EPreviewView::Softness, LOCTEXT("ViewSoftness", "Jiggle Size"),
				LOCTEXT("ViewSoftnessTip", "Last bake: how much each vertex jiggles. Same colors as Debug View Mode 4."),
				LOCTEXT("LegendSoftness", "Navy: does not move. Then green, and red is softest. Magenta: moves the opposite way."), false },
			{ EPreviewView::DentsBetweenVertices, LOCTEXT("ViewBetween", "Dents Between Vertices"),
				LOCTEXT("ViewBetweenTip", "Last bake: where dents show inside triangles. Same colors as Debug View Mode 5."),
				LOCTEXT("LegendBetween", "Green: dents show inside triangles. Red: only at the vertices along seams. Gray: not pressable."), true },
			// 색 안내는 메시마다 다른 cm 값이라 FSoftbodyCorePreviewClient가 칠할 때 만든다.
			{ EPreviewView::FleshThickness, LOCTEXT("ViewThickness", "Flesh Thickness"),
				LOCTEXT("ViewThicknessTip", "Last bake: how thick the flesh is under each vertex, down to the rigid core. The rigid core, Max Flesh Thickness and Depth paint set it."),
				FText::GetEmpty(), true },
			{ EPreviewView::LumpBase, LOCTEXT("ViewLumpBase", "Lump Base"),
				LOCTEXT("ViewLumpBaseTip", "Last contact map bake: the surface each lump sits on, as if the lump were not there. A lump's flesh is measured down to it."),
				FText::GetEmpty(), true },
			// 색 안내는 조각 수가 들어가 FSoftbodyCorePreviewClient가 칠할 때 만든다.
			{ EPreviewView::HardPieces, LOCTEXT("ViewHardPieces", "Plates"),
				LOCTEXT("ViewHardPiecesTip", "Last contact map bake: which parts painted in Flex became rigid plates, and which bend with their paint. Run Bake or Reapply Paint updates this view."),
				FText::GetEmpty(), true },
			{ EPreviewView::SeparatedParts, LOCTEXT("ViewSeparated", "Separated Parts"),
				LOCTEXT("ViewSeparatedTip", "The two sides of the Separated Pairs, with the current values. It updates as you edit."),
				LOCTEXT("LegendSeparated", "Orange and blue: the two sides kept apart. Each pair has one side in each color. Mixed colors: skinned to both sides. Gray: in no pair."),
				false },
			{ EPreviewView::PaintDepth, LOCTEXT("ViewPaintDepth", "Depth Paint"),
				LOCTEXT("ViewPaintDepthTip", "Depth from the current Slot Masks. Run Bake applies changes."), FText::GetEmpty(), false },
			{ EPreviewView::PaintJiggle, LOCTEXT("ViewPaintJiggle", "Jiggle Paint"),
				LOCTEXT("ViewPaintJiggleTip", "Jiggle from the current Slot Masks. Run Bake applies changes."), FText::GetEmpty(), false },
			{ EPreviewView::PaintVolume, LOCTEXT("ViewPaintVolume", "Volume Paint"),
				LOCTEXT("ViewPaintVolumeTip", "Volume from the current Slot Masks. Run Bake applies changes."), FText::GetEmpty(), false },
			// 색 안내는 조각 수가 들어가 FSoftbodyCorePreviewClient가 칠할 때 만든다.
			{ EPreviewView::PaintSoftness, LOCTEXT("ViewPaintSoft", "Flex Paint"),
				LOCTEXT("ViewPaintSoftTip", "Painted Flex pieces from the current Slot Masks, before baking. Run Bake applies changes, and Plates then shows which become rigid plates."),
				FText::GetEmpty(), false },
		};
		for (const FPreviewViewInfo& Info : Infos)
		{
			if (Info.View == View)
			{
				return Info;
			}
		}
		return Infos[0];
	}

	/** 메시 에셋 슬롯의 머티리얼(스태틱·스켈레탈). */
	TArray<UMaterialInterface*> AssetMaterials(const UObject* Mesh)
	{
		TArray<UMaterialInterface*> Out;
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& Material : Static->GetStaticMaterials())
			{
				Out.Add(Material.MaterialInterface);
			}
		}
		else if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
			{
				Out.Add(Material.MaterialInterface);
			}
		}
		return Out;
	}

	const FMeshDescription* LodZero(UObject* Mesh)
	{
		if (UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			return Static->GetMeshDescription(0);
		}
		if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			return Skeletal->GetMeshDescription(0);
		}
		return nullptr;
	}

	/**
	 * 메시 설명 정점마다 실행 중 눌리는지: 접촉 지도를 읽는 머티리얼이 걸린 슬롯의 삼각형에 속하면 참. 다른 슬롯 정점도 굽기가 가장 가까운
	 * 살 표면의 접촉 지도 주소를 받아 두므로(머리카락이 등 살 두께를 빌린다) 정점 색 보기는 이것으로 가린다.
	 * 슬롯 이름이 폴리곤 그룹 이름과 하나도 맞지 않으면 모두 참.
	 */
	TArray<bool> PressableVertices(const UObject* Mesh, const FMeshDescription& MD)
	{
		TSet<FName> Pressable;
		const auto AddSlot = [&Pressable](FName Name, FName ImportedName, const UMaterialInterface* Material)
		{
			if (SoftbodyBakeShared::ReadsCarrier(Material))
			{
				Pressable.Add(Name);
				Pressable.Add(ImportedName);
			}
		};
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& Slot : Static->GetStaticMaterials())
			{
				AddSlot(Slot.MaterialSlotName, Slot.ImportedMaterialSlotName, Slot.MaterialInterface);
			}
		}
		else if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& Slot : Skeletal->GetMaterials())
			{
				AddSlot(Slot.MaterialSlotName, Slot.ImportedMaterialSlotName, Slot.MaterialInterface);
			}
		}
		Pressable.Remove(NAME_None);
		TArray<bool> Out;
		Out.Init(false, MD.Vertices().GetArraySize());
		const TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		bool bAnyNameMatches = false;
		for (const FPolygonGroupID Group : MD.PolygonGroups().GetElementIDs())
		{
			bAnyNameMatches |= SlotNames.IsValid() && Pressable.Contains(SlotNames[Group]);
		}
		if (!bAnyNameMatches)
		{
			Out.Init(true, Out.Num());
			return Out;
		}
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if (Pressable.Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
			{
				for (const FVertexID Corner : MD.GetTriangleVertices(Tri))
				{
					Out[Corner.GetValue()] = true;
				}
			}
		}
		return Out;
	}

	/**
	 * 메시 설명(스켈레탈은 기준 자세)을 그릴 수 있는 동적 메시로. 노멀은 정점마다 다시 낸다.
	 * OutSourceVertices: 동적 메시 정점 → 메시 설명 정점(열린 모서리에서 나뉜 정점은 같은 정점을 가리킨다). OutSourceTriangles: 동적 메시
	 * 삼각형 → 메시 설명 삼각형.
	 */
	FDynamicMesh3 ToPreviewMesh(const FMeshDescription* MD, TArray<FVertexID>* OutSourceVertices = nullptr,
		TArray<FTriangleID>* OutSourceTriangles = nullptr)
	{
		FDynamicMesh3 Mesh;
		if (MD)
		{
			FMeshDescriptionToDynamicMesh Converter;
			Converter.bDisableAttributes = true;
			Converter.Convert(MD, Mesh);
			if (OutSourceVertices)
			{
				*OutSourceVertices = MoveTemp(Converter.VertIDMap);
			}
			if (OutSourceTriangles)
			{
				*OutSourceTriangles = MoveTemp(Converter.TriIDMap);
			}
		}
		Mesh.EnableAttributes();
		FMeshNormals::InitializeOverlayToPerVertexNormals(Mesh.Attributes()->PrimaryNormals(), false);
		return Mesh;
	}

	/**
	 * 캐리어 UV 자리의 살 두께(CarrierNormalTex w, 메시 로컬 단위): 텍셀 중심 규약의 쌍선형. 두께 0인 칸(빈 칸·Depth 칠 0)은 빼고 섞는다 —
	 * 차트 가장자리 정점이 빈 칸 쪽으로 얇게 칠해지지 않게. 둘레 칸이 모두 0이면 0.
	 */
	float CellThickness(const TArray<FFloat16Color>& Cells, int32 Resolution, const FVector2f& CarrierUV)
	{
		const float X = CarrierUV.X * Resolution - 0.5f;
		const float Y = CarrierUV.Y * Resolution - 0.5f;
		const int32 X0 = FMath::FloorToInt32(X);
		const int32 Y0 = FMath::FloorToInt32(Y);
		float Sum = 0.f;
		float Weight = 0.f;
		for (int32 DY = 0; DY < 2; ++DY)
		{
			for (int32 DX = 0; DX < 2; ++DX)
			{
				const int32 PX = FMath::Clamp(X0 + DX, 0, Resolution - 1);
				const int32 PY = FMath::Clamp(Y0 + DY, 0, Resolution - 1);
				const float Value = Cells[PY * Resolution + PX].A.GetFloat();
				const float W = (DX ? X - X0 : 1.f - (X - X0)) * (DY ? Y - Y0 : 1.f - (Y - Y0));
				if (Value > 0.f)
				{
					Sum += W * Value;
					Weight += W;
				}
			}
		}
		return Weight > 1e-6f ? Sum / Weight : 0.f;
	}

	/** 살 두께 색(0 = 얇음, 1 = 두꺼움): 보라 → 청록 → 노랑(viridis — 밝기가 고르게 올라 색이 헷갈려도 순서가 읽힌다). 선형 색. */
	FLinearColor ThicknessColor(float T)
	{
		static const FColor Stops[] = { FColor(68, 1, 84), FColor(59, 82, 139), FColor(33, 145, 140), FColor(94, 201, 98), FColor(253, 231, 37) };
		constexpr int32 Last = (int32)UE_ARRAY_COUNT(Stops) - 1;
		const float X = FMath::Clamp(T, 0.f, 1.f) * Last;
		const int32 I = FMath::Min(FMath::FloorToInt32(X), Last - 1);
		return FMath::Lerp(FLinearColor::FromSRGBColor(Stops[I]), FLinearColor::FromSRGBColor(Stops[I + 1]), X - I);
	}

	/**
	 * 메시보다 큰 구(Bottom, 판 밑에 까는 아주 큰 구)는 메시 상자와 겹친 부분만 닫힌 덩어리로 만든다 — 상자 밖은 살이 없어 붙들 것도 없다.
	 * 구와 상자가 겹친 곳은 볼록하므로, 상자 면의 격자 점 중 구 안의 것과 그 점 쪽 구 겉의 점 중 상자 안의 것을 모아 볼록 껍질을 씌운다.
	 */
	void AppendClippedSphere(FDynamicMeshEditor& Editor, const FVector3d& Center, double Radius, const FAxisAlignedBox3d& Box)
	{
		constexpr int32 N = 24;
		FAxisAlignedBox3d Grown = Box;
		Grown.Expand(1e-4 * Box.MaxDim() + 1e-6);
		TArray<FVector3d> Points;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const int32 U = (Axis + 1) % 3;
			const int32 V = (Axis + 2) % 3;
			for (int32 Side = 0; Side < 2; ++Side)
			{
				for (int32 I = 0; I <= N; ++I)
				{
					for (int32 J = 0; J <= N; ++J)
					{
						FVector3d Q;
						Q[Axis] = Side ? Box.Max[Axis] : Box.Min[Axis];
						Q[U] = FMath::Lerp(Box.Min[U], Box.Max[U], (double)I / N);
						Q[V] = FMath::Lerp(Box.Min[V], Box.Max[V], (double)J / N);
						const FVector3d D = Q - Center;
						const double Length = D.Length();
						if (Length <= Radius)
						{
							Points.Add(Q);
						}
						const FVector3d OnSphere = Length > UE_DOUBLE_SMALL_NUMBER ? Center + D * (Radius / Length) : Center;
						if (Length > UE_DOUBLE_SMALL_NUMBER && Grown.Contains(OnSphere))
						{
							Points.Add(OnSphere);
						}
					}
				}
			}
		}
		TConvexHull3<double> Hull;
		if (Points.Num() < 4 || !Hull.Solve(TArrayView<const FVector3d>(Points)))
		{
			return;
		}
		FVector3d Middle = FVector3d::Zero();
		for (const FVector3d& P : Points)
		{
			Middle += P;
		}
		Middle /= (double)Points.Num();

		FDynamicMesh3 Piece;
		TMap<int32, int32> PointToVertex;
		for (const FIndex3i& Tri : Hull.GetTriangles())
		{
			FIndex3i Out;
			for (int32 K = 0; K < 3; ++K)
			{
				const int32* Found = PointToVertex.Find(Tri[K]);
				Out[K] = Found ? *Found : PointToVertex.Add(Tri[K], Piece.AppendVertex(Points[Tri[K]]));
			}
			// 겉을 바깥으로 — 껍질의 삼각형 방향은 안팎을 정하지 않으므로 무게중심에서 보고 맞춘다. 엔진의 면 노멀은 (C−A)×(B−A)다(왼손 좌표).
			const FVector3d A = Piece.GetVertex(Out.A);
			if ((Piece.GetVertex(Out.C) - A).Cross(Piece.GetVertex(Out.B) - A).Dot(A - Middle) < 0.0)
			{
				Swap(Out.B, Out.C);
			}
			Piece.AppendTriangle(Out);
		}
		Piece.EnableAttributes();
		FMeshNormals::InitializeOverlayTopologyFromOpeningAngle(&Piece, Piece.Attributes()->PrimaryNormals(), 30.0);
		FMeshNormals::QuickRecomputeOverlayNormals(Piece);
		FMeshIndexMappings Mappings;
		Editor.AppendMesh(&Piece, Mappings);
	}

	/**
	 * 구들을 한 메시로. 구가 수천 개(사람 몸의 본 선)여도 그리기 한 번이다. Box가 있으면 상자를 넘는 구는 상자 안 부분만, 상자에 닿지 않는 구는
	 * 그리지 않는다.
	 */
	FDynamicMesh3 SpheresMesh(const TArray<FVector4>& Spheres, const FBox& Box)
	{
		// 구가 몇 개 안 되면(Center의 작은 소품, Custom) 매끈하게, 많으면 가볍게.
		const bool bFew = Spheres.Num() <= 64;
		FSphereGenerator Generator;
		Generator.Radius = 1.0;
		Generator.NumPhi = bFew ? 16 : 8;
		Generator.NumTheta = bFew ? 24 : 12;
		Generator.Generate();
		const FDynamicMesh3 Unit(&Generator);
		const FAxisAlignedBox3d Clip = Box.IsValid ? FAxisAlignedBox3d(Box.Min, Box.Max) : FAxisAlignedBox3d::Empty();
		FAxisAlignedBox3d Inside = Clip;
		if (Box.IsValid)
		{
			Inside.Expand(1e-3 * Clip.MaxDim());
		}

		FDynamicMesh3 Out;
		Out.EnableAttributes();
		FDynamicMeshEditor Editor(&Out);
		for (const FVector4& Sphere : Spheres)
		{
			const FVector3d Center(Sphere.X, Sphere.Y, Sphere.Z);
			const double Radius = Sphere.W;
			if (Box.IsValid)
			{
				if (Clip.DistanceSquared(Center) > Radius * Radius)
				{
					continue;
				}
				if (!Inside.Contains(FAxisAlignedBox3d(Center - FVector3d(Radius), Center + FVector3d(Radius))))
				{
					AppendClippedSphere(Editor, Center, Radius, Clip);
					continue;
				}
			}
			FMeshIndexMappings Mappings;
			Editor.AppendMesh(&Unit, Mappings,
				[Center, Radius](int32, const FVector3d& P) { return Center + Radius * P; },
				[](int32, const FVector3d& N) { return N; });
		}
		return Out;
	}

	UDynamicMeshComponent* NewPreviewComponent(FAdvancedPreviewScene& Scene, UMaterialInterface* Parent, const FLinearColor& Color, bool bShadows,
		float CenterOpacity = 0.f, float EdgeOpacity = 0.f)
	{
		UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialInstanceDynamic* Material = Parent ? UMaterialInstanceDynamic::Create(Parent, Component) : nullptr;
		if (Material)
		{
			Material->SetVectorParameterValue(TEXT("Color"), Color);
			if (EdgeOpacity > 0.f)
			{
				Material->SetScalarParameterValue(TEXT("CenterOpacity"), CenterOpacity);
				Material->SetScalarParameterValue(TEXT("EdgeOpacity"), EdgeOpacity);
			}
			Component->SetMaterial(0, Material);
		}
		Component->SetShadowsEnabled(bShadows);
		Scene.AddComponent(Component, FTransform::Identity);
		return Component;
	}
}

/**
 * 미리보기 뷰포트의 클라이언트. 에셋 값을 매 틱 훑어(값 몇 개라 싸다) 바뀌었으면, 마우스를 놓고 SettleSeconds 지난 뒤 다시 그린다.
 * 속성 패널·되돌리기·파이썬 어디서 바꿔도 같은 길로 들어온다.
 */
class FSoftbodyCorePreviewClient : public FEditorViewportClient
{
public:
	FSoftbodyCorePreviewClient(FAdvancedPreviewScene& InScene, const TSharedRef<SEditorViewport>& InWidget, USoftbodyAuthoringData* InAsset)
		: FEditorViewportClient(nullptr, &InScene, InWidget)
		, Scene(InScene)
		, Asset(InAsset)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		SetRealtime(true);
		EngineShowFlags.SetGrid(false);
		// 비스듬히 위에서(바닥에 놓인 판도 윗면이 보이게). 엔진 메시 창처럼 +Y를 보는 몸의 앞쪽이다.
		SetViewLocation(FVector(-75.0, 130.0, 70.0));
		SetViewRotation(FRotator(-25.0, -60.0, 0.0));

		// 겉 메시: 가운데는 비치고 윤곽으로 갈수록 진한 빛 없는 반투명, 밝기는 면 방향으로(에셋은 Tools/create_preview_shell_material.py가 만든다).
		// 엔진 디버그 재질의 고른 반투명은 한 겹이면 배경에 묻혔고, 가운데 0.08은 거의 안 보였고 0.35는 속 주황이 바랬다(일지 232 §5).
		UMaterialInterface* Shell = LoadObject<UMaterialInterface>(nullptr, TEXT("/SoftbodySystem/Editor/M_SoftbodyPreviewShell.M_SoftbodyPreviewShell"));
		ShellMaterial = Shell;
		UMaterialInterface* Lit = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/LevelColorationLitMaterial.LevelColorationLitMaterial"));
		CoreComponent = NewPreviewComponent(Scene, Lit, FLinearColor(1.f, 0.42f, 0.08f, 1.f), true);
		MeshComponent = NewPreviewComponent(Scene, Shell, FLinearColor(0.7f, 0.72f, 0.76f, 1.f), false, 0.22f, 0.8f);
		VolumeComponent = NewPreviewComponent(Scene, Shell, FLinearColor(0.3f, 0.55f, 1.f, 1.f), false, 0.08f, 0.35f);
		VolumeComponent->SetEnableWireframeRenderPass(true);
		VolumeComponent->WireframeColor = FLinearColor(0.3f, 0.55f, 1.f);
		// 살 두께 보기(ADR-0037 §3.5): CPU가 칠한 정점 색을 엔진의 빛 받는 정점 색 머티리얼(VertexColor → 확산색)로 그린다.
		UMaterialInterface* VertexColor = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
		ThicknessComponent = NewPreviewComponent(Scene, nullptr, FLinearColor::White, true);
		ThicknessComponent->SetMaterial(0, VertexColor);
		ThicknessComponent->SetVisibility(false);
		// 굽기 전 색 보기(Separated Parts, 칠)와 덩어리 받침 막(Lump Base)도 같은 정점 색 머티리얼로 그린다.
		BeforeColorComponent = NewPreviewComponent(Scene, nullptr, FLinearColor::White, true);
		BeforeColorComponent->SetMaterial(0, VertexColor);
		BeforeColorComponent->SetVisibility(false);
		LumpBaseComponent = NewPreviewComponent(Scene, nullptr, FLinearColor::White, true);
		LumpBaseComponent->SetMaterial(0, VertexColor);
		LumpBaseComponent->SetVisibility(false);
		HardPiecesComponent = NewPreviewComponent(Scene, nullptr, FLinearColor::White, true);
		HardPiecesComponent->SetMaterial(0, VertexColor);
		HardPiecesComponent->SetVisibility(false);
		// 메시를 다시 임포트하면 설정값은 그대로라 해시로는 모른다. 그린 메시가 바뀌면 다시 그린다.
		MeshChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FSoftbodyCorePreviewClient::OnObjectChanged);
	}

	virtual ~FSoftbodyCorePreviewClient() override
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(MeshChangedHandle);
	}

	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		USoftbodyAuthoringData* Data = Asset.Get();
		if (!Data)
		{
			return;
		}
		// 굽기 전 보기는 보기마다 읽는 값이 다르다: 속은 SettingsKey, 색 보기는 거기에 Separated Pairs·Slot Masks(ColorKey)와 보기 자체.
		// 굽기 결과 보기에서는 0을 더해, 결과 보기 사이를 오가도 속을 다시 계산하지 않는다.
		const uint32 ViewPart = IsResultView() ? 0u
			: HashCombine(GetTypeHash((int32)View), View == SoftbodyAuthoringEditorLocal::EPreviewView::RigidCore ? 0u : ColorKey(*Data));
		const uint32 Key = HashCombine(SettingsKey(*Data), ViewPart);
		const double Now = FPlatformTime::Seconds();
		if (Key != PendingKey)
		{
			PendingKey = Key;
			PendingSince = Now;
		}
		// 슬라이더를 끄는 동안(마우스를 잡은 동안)은 계산하지 않는다. 처음 한 번과 보기를 바꾼 직후는 바로.
		const bool bSettled = Now - PendingSince >= SoftbodyAuthoringEditorLocal::SettleSeconds && !FSlateApplication::Get().HasAnyMouseCaptor();
		if (Key != BuiltKey && (bSettled || !bBuiltOnce || bViewChanged))
		{
			bViewChanged = false;
			Rebuild(*Data);
		}
		// 굽기 결과는 굽기가 바꾼다(텍스처·메시 UV는 제자리에서 바뀌어 그대로 보인다). 다시 짤 것은 가리키는 것이 바뀔 때뿐.
		if (IsResultView())
		{
			const uint32 ResultNow = ResultSettingsKey(*Data);
			if (ResultNow != ResultKey)
			{
				RebuildResult(*Data, ResultNow);
			}
		}
	}

	void SetView(SoftbodyAuthoringEditorLocal::EPreviewView InView)
	{
		if (InView != View)
		{
			// 앞 보기의 색이 새 보기 이름 밑에 남지 않게 다시 칠할 때까지 숨긴다.
			bBeforeColorShown = false;
			bViewChanged = true;
		}
		View = InView;
		ResultKey = 0;
		ApplyVisibility();
		Invalidate();
	}
	SoftbodyAuthoringEditorLocal::EPreviewView GetView() const { return View; }
	bool IsResultView() const { return (int32)View >= 0; }
	/** 굽기 전 색 보기(Separated Parts, 칠): 정점 색 사본을 지금 값으로 칠한다. */
	bool IsBeforeColorView() const { return !IsResultView() && View != SoftbodyAuthoringEditorLocal::EPreviewView::RigidCore; }

	FText GetStatus() const { return IsResultView() ? ResultStatus : Status; }
	bool HasProblem() const { return IsResultView() ? bResultProblem : bProblem; }

	FText GetLegend() const
	{
		using namespace SoftbodyAuthoringEditorLocal;
		if (View == EPreviewView::FleshThickness)
		{
			return bThicknessShown ? ThicknessLegend : FText::GetEmpty();
		}
		if (View == EPreviewView::LumpBase)
		{
			return bLumpBaseShown ? LumpBaseLegend : FText::GetEmpty();
		}
		if (View == EPreviewView::HardPieces)
		{
			return bHardPiecesShown ? HardPiecesLegend : FText::GetEmpty();
		}
		if (View == EPreviewView::PaintSoftness)
		{
			return bBeforeColorShown ? HardPaintLegend : FText::GetEmpty();
		}
		if (PaintKindOf(View).IsSet())
		{
			return bBeforeColorShown
				? LOCTEXT("LegendPaint", "White: 1. Black: 0. Light blue: unpainted here, so it counts as 1.")
				: FText::GetEmpty();
		}
		if (IsResultView() || View == EPreviewView::SeparatedParts)
		{
			return ViewInfo(View).Legend;
		}
		return BuiltVolume.IsValid()
			? LOCTEXT("LegendVolume", "Orange: never moves. Gray: Target Mesh. Blue lines: Volume Mesh. Run Bake applies changes.")
			: LOCTEXT("Legend", "Orange: never moves. Gray: Target Mesh. Run Bake applies changes.");
	}

private:
	/** 굽기 결과 보기가 가리키는 것: 메시, 굽기 데이터와 그 텍스처, 메시 슬롯 머티리얼(Create Softbody Material이 바꾼다). */
	static uint32 ResultSettingsKey(const USoftbodyAuthoringData& Data)
	{
		const UObject* Target = Data.TargetMesh.Get();
		const USoftbodyMeshData* Baked = Data.BakedData.Get();
		uint32 Key = HashCombine(GetTypeHash(Target), GetTypeHash(Baked));
		if (Baked)
		{
			Key = HashCombine(Key, GetTypeHash(Baked->AttributeTexDebug.Get()));
			Key = HashCombine(Key, GetTypeHash(Baked->CarrierUVTex.Get()));
			Key = HashCombine(Key, GetTypeHash(Baked->SourceMesh.Get()));
			Key = HashCombine(Key, GetTypeHash(Baked->SourceSkeletalMesh.Get()));
		}
		// 실행 버튼(굽기·칠 다시 입히기·접촉 지도 다시 굽기)마다: 살 두께 보기는 텍스처를 CPU에서 읽어 칠한 사본이라 제자리에서 바뀐 텍스처를 스스로 보지 못한다.
		Key = HashCombine(Key, GetTypeHash(Data.GetRunSerial()));
		for (const UMaterialInterface* Material : SoftbodyAuthoringEditorLocal::AssetMaterials(Target))
		{
			Key = HashCombine(Key, GetTypeHash(Material));
		}
		return Key == 0 ? 1u : Key;
	}

	void ApplyVisibility()
	{
		using namespace SoftbodyAuthoringEditorLocal;
		const bool bResult = IsResultView();
		const bool bCore = View == EPreviewView::RigidCore;
		CoreComponent->SetVisibility(bCore);
		// 겉 메시(반투명 회색)는 속 보기와, 몸 속에 막을 그리는 Lump Base 보기에서.
		MeshComponent->SetVisibility(bCore || (View == EPreviewView::LumpBase && bLumpBaseShown));
		VolumeComponent->SetVisibility(bCore && BuiltVolume.IsValid());
		for (UMeshComponent* Component : { (UMeshComponent*)ResultStatic, (UMeshComponent*)ResultSkeletal })
		{
			if (Component)
			{
				Component->SetVisibility(bResult && Component == ResultShown);
			}
		}
		ThicknessComponent->SetVisibility(View == EPreviewView::FleshThickness && bThicknessShown);
		BeforeColorComponent->SetVisibility(IsBeforeColorView() && bBeforeColorShown);
		LumpBaseComponent->SetVisibility(View == EPreviewView::LumpBase && bLumpBaseShown);
		HardPiecesComponent->SetVisibility(View == EPreviewView::HardPieces && bHardPiecesShown);
	}

	/**
	 * 굽기 결과 보기: 메시를 제 머티리얼 그대로 그리고, MF_SoftbodyDebugVis가 있는 슬롯에 컴포넌트가 넣는 것과 같은 굽기 텍스처와
	 * VisWeight·VisMode를 넣는다(USoftbodyComponent::SetupRuntime·UpdateMaterialParams와 같은 이름). 컴포넌트 값은 쓰지 않는다 —
	 * 색 범위를 정하는 Follow Range(모드 3)와 정점 밀도 기준(모드 5)은 머티리얼 기본값이다.
	 */
	void RebuildResult(USoftbodyAuthoringData& Data, uint32 Key)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		ResultKey = Key;
		ResultShown = nullptr;
		bThicknessShown = false;
		bLumpBaseShown = false;
		bHardPiecesShown = false;
		bResultProblem = true;
		UObject* Target = Data.TargetMesh.Get();
		const USoftbodyMeshData* Baked = Data.BakedData.Get();
		const UObject* BakedFor = Baked
			? (Baked->SourceSkeletalMesh ? (const UObject*)Baked->SourceSkeletalMesh.Get() : (const UObject*)Baked->SourceMesh.Get())
			: nullptr;
		if (!Target)
		{
			ResultStatus = LOCTEXT("ResultNoTarget", "Pick a Target Mesh.");
		}
		else if (!Baked || !Baked->AttributeTexDebug)
		{
			ResultStatus = LOCTEXT("ResultNotBaked", "Not baked yet. Press Run Bake to see this view.");
		}
		else if (BakedFor && BakedFor != Target)
		{
			ResultStatus = FText::Format(LOCTEXT("ResultOtherMesh", "The last bake is for {0}. Press Run Bake for this Target Mesh."),
				FText::FromString(BakedFor->GetName()));
		}
		else if (ViewInfo(View).bNeedsContactMap && !Baked->CarrierUVTex)
		{
			ResultStatus = LOCTEXT("ResultNoContactMap", "The last bake has no contact map. Turn on Bake Contact Map and press Run Bake.");
		}
		else if (View == EPreviewView::FleshThickness)
		{
			// 다른 보기에 갔다 와도 같은 굽기면 다시 칠하지 않는다(큰 몸은 메시 사본을 만드는 데 시간이 든다).
			if (Key != ThicknessKey)
			{
				ThicknessKey = Key;
				bThicknessBuilt = BuildThickness(*Baked, Target);
			}
			bThicknessShown = bThicknessBuilt;
			bResultProblem = !bThicknessBuilt;
			ResultStatus = ThicknessStatus;
		}
		else if (View == EPreviewView::LumpBase)
		{
			if (Key != LumpBaseKey)
			{
				LumpBaseKey = Key;
				BuildLumpBase(*Baked);
			}
			bLumpBaseShown = bLumpBaseBuilt;
			bResultProblem = bLumpBaseProblem;
			ResultStatus = LumpBaseStatus;
		}
		else if (View == EPreviewView::HardPieces)
		{
			if (Key != HardPiecesKey)
			{
				HardPiecesKey = Key;
				BuildHardPieces(*Baked, Target);
			}
			bHardPiecesShown = bHardPiecesBuilt;
			bResultProblem = bHardPiecesProblem;
			ResultStatus = HardPiecesStatus;
		}
		else
		{
			ResultShown = ShowResultMesh(Target);
		}

		if (ResultShown)
		{
			static const FName VisWeightName(TEXT("VisWeight"));
			// 디버그 색이 없는 슬롯(머리카락 카드·눈처럼 다른 머티리얼)은 옅은 회색 겉으로 — 색 있는 슬롯이 묻히지 않게.
			UMaterialInstanceDynamic* Gray = ShellMaterial ? UMaterialInstanceDynamic::Create(ShellMaterial, ResultShown) : nullptr;
			if (Gray)
			{
				Gray->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.7f, 0.72f, 0.76f, 1.f));
				Gray->SetScalarParameterValue(TEXT("CenterOpacity"), 0.12f);
				Gray->SetScalarParameterValue(TEXT("EdgeOpacity"), 0.5f);
			}
			ResultShown->EmptyOverrideMaterials();
			const int32 NumSlots = ResultShown->GetNumMaterials();
			int32 Colored = 0;
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				UMaterialInterface* Material = ResultShown->GetMaterial(Slot);
				float Unused = 0.f;
				if (Material && Material->GetScalarParameterValue(FHashedMaterialParameterInfo(VisWeightName), Unused))
				{
					UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Material, ResultShown);
					MID->SetTextureParameterValue(TEXT("AttrDbgTex"), Baked->AttributeTexDebug);
					if (Baked->CarrierUVTex)
					{
						MID->SetTextureParameterValue(TEXT("CarrierUVTex"), Baked->CarrierUVTex);
					}
					MID->SetScalarParameterValue(VisWeightName, 1.f);
					MID->SetScalarParameterValue(TEXT("VisMode"), (float)(int32)View);
					ResultShown->SetMaterial(Slot, MID);
					++Colored;
				}
				else if (Gray)
				{
					ResultShown->SetMaterial(Slot, Gray);
				}
			}
			// 나나이트는 불투명·마스크만 그린다. 반투명 슬롯(회색 겉이나 메시 제 반투명)이 있으면 이 미리보기 메시만 나나이트를 끈다.
			// 스태틱은 엔진이 스스로 내리지만 나나이트 스켈레탈은 블렌드 모드를 거르지 않아 Lumen 카드 캡처가 멈췄다(일지 237).
			bool bTranslucentSlot = false;
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				const UMaterialInterface* Material = ResultShown->GetMaterial(Slot);
				bTranslucentSlot |= Material && !IsOpaqueOrMaskedBlendMode(*Material);
			}
			if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(ResultShown))
			{
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
				// 5.6·5.7의 스켈레탈 메시 컴포넌트에는 이 스위치가 없다.
				Skinned->SetForceDisableNanite(bTranslucentSlot);
#endif
			}
			else if (UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(ResultShown))
			{
				StaticComponent->SetForceDisableNanite(bTranslucentSlot);
			}
			if (Colored == 0)
			{
				ResultStatus = LOCTEXT("ResultNoVisSlot",
					"No material slot can show these colors. Its material needs MF_SoftbodyDebugVis, which Create Softbody Material adds.");
			}
			else if (Colored < NumSlots)
			{
				ResultStatus = FText::Format(LOCTEXT("ResultSomeSlots",
					"Last bake. {0} of {1} material slots show colors, and gray slots have no MF_SoftbodyDebugVis."), Colored, NumSlots);
				bResultProblem = false;
			}
			else
			{
				ResultStatus = LOCTEXT("ResultAllSlots", "Last bake. Run Bake updates this view.");
				bResultProblem = false;
			}
		}
		ApplyVisibility();
		Invalidate();
	}

	/** 굽기 결과를 그릴 메시 컴포넌트(처음 쓸 때 만든다). 스켈레탈은 기준 자세 — 애님도 후처리 애님 블루프린트도 돌리지 않는다. */
	UMeshComponent* ShowResultMesh(UObject* Target)
	{
		if (UStaticMesh* Static = Cast<UStaticMesh>(Target))
		{
			if (!ResultStatic)
			{
				ResultStatic = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				Scene.AddComponent(ResultStatic, FTransform::Identity);
			}
			ResultStatic->SetStaticMesh(Static);
			return ResultStatic;
		}
		if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Target))
		{
			if (!ResultSkeletal)
			{
				ResultSkeletal = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				ResultSkeletal->SetDisablePostProcessBlueprint(true);
				Scene.AddComponent(ResultSkeletal, FTransform::Identity);
			}
			ResultSkeletal->SetSkeletalMeshAsset(Skeletal);
			return ResultSkeletal;
		}
		return nullptr;
	}

	/**
	 * 살 두께 보기(ADR-0037 §3.5): 함몰과 그 둘레 솟음의 폭이 구운 살 두께에서 나오므로 그 두께를 보인다. 메시 설명(스켈레탈은 기준 자세)의
	 * 사본을 정점 색으로 칠한다 — 메시의 머티리얼·셰이더는 그대로다(정점 셰이더의 텍스처 읽기를 늘리지 않는다, P07).
	 * 정점마다 텍셀 표(CarrierUVTex RGBA32F, 텍셀 UV 채널로 주소, A < 0.5 = 칸 없음)에서 캐리어 UV(RG)를 읽고, 그 자리 칸들의 두께
	 * (CarrierNormalTex RGBA16F의 w, 메시 로컬 단위)를 섞는다 — 실행 중 SB_LayerAt이 컴포넌트 배율을 곱해 읽는 값이다.
	 * 색 범위는 굽기가 적은 두 값: 바닥값(CarrierThicknessFloor, 속이 살갗에 닿은 칸) ~ 살 있는 칸의 90 % 값(CarrierThicknessHigh).
	 * 실패하면 이유를 ThicknessStatus에 남기고 거짓.
	 */
	bool BuildThickness(const USoftbodyMeshData& Baked, UObject* Target)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		ThicknessLegend = FText::GetEmpty();
		const FMeshDescription* MD = LodZero(Target);
		const int32 TableSize = Baked.TextureSize;
		const int32 Resolution = Baked.CarrierResolution;
		if (Baked.CarrierThicknessHigh <= 0.f)
		{
			ThicknessStatus = LOCTEXT("ThicknessOldBake", "The last bake is older than this view. Press Run Bake.");
			return false;
		}
		// 바닥값은 일지 252 전 굽기면 0이다(그대로 0부터). 살이 거의 다 바닥값인 메시도 색이 갈리게 범위를 0.1 cm 이상으로 둔다.
		const float Low = FMath::Max(Baked.CarrierThicknessFloor, 0.f);
		const float High = FMath::Max(Baked.CarrierThicknessHigh, Low + 0.1f);
		TArray<FLinearColor> Table;
		TArray<FFloat16Color> Cells;
		if (!MD || TableSize <= 0 || Resolution <= 0
			|| !SoftbodyBakeShared::ReadTextureSource32F(Baked.CarrierUVTex, TableSize * TableSize, Table)
			|| !SoftbodyBakeShared::ReadTextureSource16F(Baked.CarrierNormalTex.LoadSynchronous(), Resolution * Resolution, Cells))
		{
			ThicknessStatus = LOCTEXT("ThicknessNoSource", "Could not read the contact map textures. Press Run Bake again.");
			return false;
		}
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = FStaticMeshConstAttributes(*MD).GetVertexInstanceUVs();
		if (Baked.TexelUVChannel < 0 || Baked.TexelUVChannel >= UVs.GetNumChannels())
		{
			ThicknessStatus = FText::Format(LOCTEXT("ThicknessNoChannel", "The mesh no longer has the baked UV channel {0}. Press Run Bake again."),
				Baked.TexelUVChannel);
			return false;
		}

		// 칸이 없거나(접촉 지도 밖) 살이 없거나(Depth 칠 0) 눌리지 않는 슬롯이면 0 — 다른 보기의 "눌리지 않음"과 같은 회색으로 칠한다.
		const TArray<bool> Pressable = PressableVertices(Target, *MD);
		const auto VertexThickness = [&](const FVertexID Source) -> float
		{
			const TArrayView<const FVertexInstanceID> Instances = MD->GetVertexVertexInstanceIDs(Source);
			if (Instances.Num() == 0 || !Pressable.IsValidIndex(Source.GetValue()) || !Pressable[Source.GetValue()])
			{
				return 0.f;
			}
			// 텍셀 UV는 위치당 하나라 어느 정점 인스턴스에서 읽어도 같다.
			const FVector2f TexelUV = UVs.Get(Instances[0], Baked.TexelUVChannel);
			const int32 TX = FMath::Clamp((int32)(TexelUV.X * TableSize), 0, TableSize - 1);
			const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TableSize), 0, TableSize - 1);
			const FLinearColor& Carrier = Table[TY * TableSize + TX];
			return Carrier.A >= 0.5f ? CellThickness(Cells, Resolution, FVector2f(Carrier.R, Carrier.G)) : 0.f;
		};

		TArray<FVertexID> SourceVertices;
		FDynamicMesh3 Mesh = ToPreviewMesh(MD, &SourceVertices);
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
		CreatePerVertexColors(Colors);
		const FLinearColor Gray(0.35f, 0.35f, 0.35f);
		for (const int32 V : Mesh.VertexIndicesItr())
		{
			const float Thickness = SourceVertices.IsValidIndex(V) ? VertexThickness(SourceVertices[V]) : 0.f;
			const FLinearColor Color = Thickness > 0.f ? ThicknessColor((Thickness - Low) / (High - Low)) : Gray;
			Colors->SetElement(V, FVector4f(Color.R, Color.G, Color.B, 1.f));
		}
		ThicknessComponent->SetMesh(MoveTemp(Mesh));

		FNumberFormattingOptions Cm;
		Cm.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(2);
		ThicknessLegend = FText::Format(LOCTEXT("LegendThickness",
			"Purple: {0} cm or less. Teal: {1} cm. Yellow: {2} cm or more. Gray: not pressable. Sizes are at scale 1.\n"
			"Thicker flesh spreads dents and the rise around them wider. Pressing deeper than this pushes the skin past the bone."),
			FText::AsNumber(Low, &Cm), FText::AsNumber(0.5f * (Low + High), &Cm), FText::AsNumber(High, &Cm));
		ThicknessStatus = LOCTEXT("ThicknessShown", "Last bake. Run Bake updates this view.");
		return true;
	}

	/**
	 * Lump Base 보기(ADR-0034): 접촉 지도 굽기가 덩어리 칸의 살 두께를 잴 때 쓴 받침 막(USoftbodyMeshData::LumpBase*, 에디터 전용 기록)을 그대로
	 * 그린다 — 덩어리가 없을 때의 몸 겉면이다. 색 = 그 위 둔덕 높이(가장자리 0 → 가장 높은 곳). 겉 메시는 반투명으로 같이 보인다.
	 * 결과를 bLumpBaseBuilt(그릴 막이 있음)·bLumpBaseProblem·LumpBaseStatus·LumpBaseLegend에 남긴다.
	 */
	void BuildLumpBase(const USoftbodyMeshData& Baked)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		bLumpBaseBuilt = false;
		bLumpBaseProblem = false;
		LumpBaseLegend = FText::GetEmpty();
		if (!Baked.bLumpBaseRecorded)
		{
			bLumpBaseProblem = true;
			LumpBaseStatus = LOCTEXT("LumpBaseOld", "The last contact map bake is older than this view. Press Rebake Contact Map.");
			return;
		}
		const int32 NumVertices = Baked.LumpBaseVertices.Num();
		if (NumVertices == 0 || Baked.LumpBaseHeights.Num() != NumVertices || Baked.LumpBaseTriangles.Num() < 3)
		{
			// 덩어리는 있는데 받침을 못 찾았으면 굽기가 남긴 까닭을 보인다.
			bLumpBaseProblem = Baked.LumpBaseNotes.Num() > 0;
			LumpBaseStatus = bLumpBaseProblem ? FText::FromString(FString::Join(Baked.LumpBaseNotes, TEXT("\n")))
				: LOCTEXT("LumpBaseNone",
					"No lump in the last bake. A bone with the Lump role in Bone Overrides makes one, and Rebake Contact Map finds what it sits on.");
			return;
		}
		float MaxHeight = 0.f;
		for (const float Height : Baked.LumpBaseHeights)
		{
			MaxHeight = FMath::Max(MaxHeight, Height);
		}
		MaxHeight = FMath::Max(MaxHeight, 0.1f);
		FDynamicMesh3 Mesh;
		for (const FVector3f& Vertex : Baked.LumpBaseVertices)
		{
			Mesh.AppendVertex(FVector3d(Vertex));
		}
		for (int32 i = 0; i + 2 < Baked.LumpBaseTriangles.Num(); i += 3)
		{
			const FIndex3i Tri(Baked.LumpBaseTriangles[i], Baked.LumpBaseTriangles[i + 1], Baked.LumpBaseTriangles[i + 2]);
			if (Mesh.IsVertex(Tri.A) && Mesh.IsVertex(Tri.B) && Mesh.IsVertex(Tri.C))
			{
				Mesh.AppendTriangle(Tri);
			}
		}
		Mesh.EnableAttributes();
		FMeshNormals::InitializeOverlayToPerVertexNormals(Mesh.Attributes()->PrimaryNormals(), false);
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
		CreatePerVertexColors(Colors);
		for (const int32 V : Mesh.VertexIndicesItr())
		{
			const FLinearColor Color = ThicknessColor(Baked.LumpBaseHeights[V] / MaxHeight);
			Colors->SetElement(V, FVector4f(Color.R, Color.G, Color.B, 1.f));
		}
		LumpBaseComponent->SetMesh(MoveTemp(Mesh));
		bLumpBaseBuilt = true;

		FNumberFormattingOptions Cm;
		Cm.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		LumpBaseLegend = FText::Format(LOCTEXT("LegendLumpBase",
			"Colored surface: what each lump sits on, the body as if the lump were not there. Its color is how far the skin is above it: "
			"purple at the rim, yellow at {0} cm. Gray: Target Mesh.\n"
			"The Lump role in Bone Overrides decides which lumps get one. The bake finds where each lump ends from the mesh shape."),
			FText::AsNumber(MaxHeight, &Cm));
		LumpBaseStatus = FText::FromString(TEXT("Last contact map bake. Rebake Contact Map updates this view.\n")
			+ FString::Join(Baked.LumpBaseNotes, TEXT("\n")));
	}

	/**
	 * Hard Surface Pieces 보기(ADR-0037 §3.7): 마지막 접촉 지도 굽기가 칠한 조각마다 남긴 결과(USoftbodyMeshData::HardPieceResults, 에디터 전용
	 * 기록)를 메시 설명 사본(스켈레탈은 기준 자세)의 삼각형마다 칠한다 — 남은 판은 조각마다 다른 진한 색, 몸을 둘러 감겨 살로 남은 조각은 빨강,
	 * 밑에 누를 겉면이 없어 살로 남은 조각은 흰색, 너무 커서(발자국 거친 칸 SoftbodyPlateBake::MaxCells 넘음) 살로 남은 조각은 주황, 접촉 지도
	 * 가장자리라 판을 읽지 못해 살로 남은 조각은 보라, 칠하지 않은 곳은 짙은 회색. 색은 삼각형 꼭짓점마다 따로 둔다(조각 경계에서 섞이지 않게).
	 * 결과를 bHardPiecesBuilt·bHardPiecesProblem·HardPiecesStatus·HardPiecesLegend에 남긴다.
	 */
	void BuildHardPieces(const USoftbodyMeshData& Baked, UObject* Target)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		bHardPiecesBuilt = false;
		bHardPiecesProblem = true;
		HardPiecesLegend = FText::GetEmpty();
		const FMeshDescription* MD = LodZero(Target);
		const int32 NumPainted = Baked.HardPiecesFloating.Num();
		if (!MD)
		{
			HardPiecesStatus = LOCTEXT("HardPiecesNoMesh", "Pick a Target Mesh.");
			return;
		}
		if (NumPainted == 0)
		{
			bHardPiecesProblem = false;
			HardPiecesStatus = LOCTEXT("HardPiecesNone",
				"No piece was painted in Flex in the last bake. Paint Flex in 4. Paint > Slot Masks, then press Run Bake.");
			return;
		}
		if (Baked.HardTrianglePieces.Num() != MD->Triangles().GetArraySize())
		{
			HardPiecesStatus = LOCTEXT("HardPiecesMeshChanged", "The mesh changed since the paint was applied. Press Run Bake.");
			return;
		}
		if (Baked.HardPieceResults.Num() != NumPainted || Baked.HardPieceResultsHash != Baked.HardMaskPaintHash)
		{
			HardPiecesStatus = LOCTEXT("HardPiecesStale",
				"The last contact map bake has no record of these plates. It is older than this view, or the Flex paint changed since. Press Run Bake.");
			return;
		}
		int32 NumRigid = 0;
		int32 NumBending = 0;   // 남은 판 가운데 Max Flex > 0(Flex Offset 0에서 판이 아니라 칠대로 굽는다 — 연속 굽힘)
		int32 NumWrapped = 0;
		int32 NumNoSurface = 0;
		int32 NumTooLarge = 0;
		int32 NumAtEdge = 0;
		for (const int32 Result : Baked.HardPieceResults)
		{
			const bool bBends = Result > 0 && Baked.HardPieces.IsValidIndex(Result - 1) && Baked.HardPieces[Result - 1].MaxSoftness > 0.f
				&& !Baked.HardPieces[Result - 1].bFloating;
			NumRigid += Result > 0 && !bBends ? 1 : 0;
			NumBending += bBends ? 1 : 0;
			NumWrapped += Result == SoftbodyBakeShared::HardDroppedWrapped ? 1 : 0;
			NumNoSurface += Result == SoftbodyBakeShared::HardDroppedNoSurface ? 1 : 0;
			NumTooLarge += Result == SoftbodyBakeShared::HardDroppedTooLarge ? 1 : 0;
			NumAtEdge += Result == SoftbodyBakeShared::HardDroppedAtlasEdge ? 1 : 0;
		}
		const auto ColorOf = [&Baked](int32 Result) -> FLinearColor
		{
			if (Result > 0 && Baked.HardPieces.IsValidIndex(Result - 1) && Baked.HardPieces[Result - 1].MaxSoftness > 0.f
				&& !Baked.HardPieces[Result - 1].bFloating)
			{
				return FLinearColor(0.35f, 0.62f, 0.95f);   // 회색 조각: 판이 아니라 칠대로 굽는다(연속 굽힘)
			}
			if (Result > 0)
			{
				return HardPieceColor(Result);   // 판마다 다른 진한 색
			}
			if (Result == SoftbodyBakeShared::HardDroppedWrapped)
			{
				return FLinearColor(0.7f, 0.01f, 0.01f);
			}
			if (Result == SoftbodyBakeShared::HardDroppedNoSurface)
			{
				return FLinearColor(1.f, 1.f, 1.f);
			}
			if (Result == SoftbodyBakeShared::HardDroppedTooLarge)
			{
				return FLinearColor(1.f, 0.3f, 0.f);
			}
			if (Result == SoftbodyBakeShared::HardDroppedAtlasEdge)
			{
				return FLinearColor(0.4f, 0.05f, 0.7f);
			}
			return FLinearColor(0.07f, 0.07f, 0.075f);   // 칠하지 않은 곳은 짙게 — 옅은 회색은 빛을 받아 하얗게 날아가 작은 조각이 묻혔다
		};
		TArray<FTriangleID> SourceTriangles;
		FDynamicMesh3 Mesh = ToPreviewMesh(MD, nullptr, &SourceTriangles);
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
		for (const int32 T : Mesh.TriangleIndicesItr())
		{
			const int32 Source = SourceTriangles.IsValidIndex(T) ? SourceTriangles[T].GetValue() : INDEX_NONE;
			const int32 Piece = Baked.HardTrianglePieces.IsValidIndex(Source) ? Baked.HardTrianglePieces[Source] : 0;
			const int32 Result = Piece > 0 && Baked.HardPieceResults.IsValidIndex(Piece - 1) ? Baked.HardPieceResults[Piece - 1] : 0;
			const FLinearColor C = ColorOf(Result);
			const FVector4f Value(C.R, C.G, C.B, 1.f);
			Colors->SetTriangle(T, FIndex3i(Colors->AppendElement(Value), Colors->AppendElement(Value), Colors->AppendElement(Value)));
		}
		HardPiecesComponent->SetMesh(MoveTemp(Mesh));
		bHardPiecesBuilt = true;
		bHardPiecesProblem = false;
		HardPiecesLegend = FText::Format(LOCTEXT("LegendHardPieces",
			"Colors: rigid plates ({0}), one color each. They sink and tilt as one piece.\n"
			"Light blue: gray pieces ({6}). They bend with their paint, stiffer the darker it is. A negative Flex Offset can make them rigid.\n"
			"Red: painted pieces that wrap around the body ({1}). They bend with their paint. Leave a thin white gap to split each into flat parts.\n"
			"White: painted pieces with no pressable surface under them ({2}). They are not plates.\n"
			"Orange: painted pieces too large to be one rigid plate, over {3} coarse cells ({4}). They bend with their paint. For rigid plates paint smaller ones with thin white gaps.\n"
			"Purple: painted pieces at the very edge of the contact map ({5}). They bend with their paint. Bake again with another Contact Map Resolution.\n"
			"Dark gray: not part of a painted piece. Gray paint here still bends, and white is bare skin."),
			NumRigid, NumWrapped, NumNoSurface, SoftbodyPlateBake::MaxCells, NumTooLarge, NumAtEdge, NumBending);
		HardPiecesStatus = LOCTEXT("HardPiecesShown", "Last contact map bake. Run Bake or Reapply Paint updates this view.");
	}

	/**
	 * Hard Surface Paint 보기(굽기 전): 지금 Slot Masks로 칠 단계와 같은 함수(FindHardPieces)가 찾을 단단한 조각을 겉 메시 사본의
	 * 삼각형마다 칠한다 — 조각마다 다른 진한 색(Hard Surface Pieces 보기와 같은 색 규칙), 칠하지 않은 곳은 짙은 회색. 조각이 몸을 감는지·밑에
	 * 누를 겉면이 있는지는 접촉 지도 굽기가 정하므로 여기서는 모른다(그 뒤 Hard Surface Pieces 보기가 보여 준다).
	 * 결과를 bBeforeColorShown·Status·bProblem·HardPaintLegend에 남긴다.
	 */
	void BuildHardPaint(const USoftbodyAuthoringData& Data, UObject* Target, const FMeshDescription& MD)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		TArray<int32> TrianglePieces;
		TArray<uint8> Floating;
		uint32 Hash = 0;
		const int32 NumPieces = SoftbodyBakeShared::FindHardPieces(MD, Data.SlotMasks, SoftbodyBakeShared::SlotAliasesOf(Target),
			TrianglePieces, Floating, Hash, false);
		int32 NumFloating = 0;
		for (const uint8 bFloating : Floating)
		{
			NumFloating += bFloating ? 1 : 0;
		}
		FDynamicMesh3 Mesh = ColorBaseMesh;
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
		for (const int32 T : Mesh.TriangleIndicesItr())
		{
			const int32 Source = ColorSourceTriangles.IsValidIndex(T) ? ColorSourceTriangles[T].GetValue() : INDEX_NONE;
			const int32 Piece = TrianglePieces.IsValidIndex(Source) ? TrianglePieces[Source] : 0;
			// 칠하지 않은 곳은 짙게 — Hard Surface Pieces 보기와 같다(옅은 회색은 빛을 받아 하얗게 날아가 작은 조각이 묻힌다).
			const FLinearColor C = Piece > 0 ? HardPieceColor(Piece) : FLinearColor(0.07f, 0.07f, 0.075f);
			const FVector4f Value(C.R, C.G, C.B, 1.f);
			Colors->SetTriangle(T, FIndex3i(Colors->AppendElement(Value), Colors->AppendElement(Value), Colors->AppendElement(Value)));
		}
		BeforeColorComponent->SetMesh(MoveTemp(Mesh));
		bBeforeColorShown = true;
		HardPaintLegend = FText::Format(LOCTEXT("LegendHardPaint",
			"Colors: painted pieces ({0}), one color each. Black ones become rigid plates and gray ones bend with their paint. {1} of them are "
			"separate parts lying on the surface, such as gems.\n"
			"Dark gray: not part of a painted piece. After the contact map bake, Plates shows which pieces became rigid plates."),
			NumPieces, NumFloating);
		Status = NumPieces > 0
			? ViewInfo(EPreviewView::PaintSoftness).Tooltip
			: LOCTEXT("HardPaintNone", "No painted pieces. Paint Flex darker than white in the Paint Texture's alpha, or set Flex to Black everywhere, in 4. Paint > Slot Masks.");
		AppendMaskProblems(Data, SoftbodyBakeShared::EPaintKind::Softness);
	}

	/** 칠 보기의 상태 줄에 이 칠이 고른 대로 읽히지 않는 슬롯(굽기와 같은 판정 DescribeSlotMaskProblems)을 덧붙이고 문제로 표시한다. */
	void AppendMaskProblems(const USoftbodyAuthoringData& Data, TOptional<SoftbodyBakeShared::EPaintKind> Kind)
	{
		TArray<FString> Lines;
		TArray<FString> Changed;
		SoftbodyBakeShared::DescribeSlotMaskProblems(Data.SlotMasks, Lines, Changed, Kind);
		Lines.Append(Changed);
		if (Lines.Num() > 0)
		{
			bProblem = true;
			Status = FText::FromString(Status.ToString() + TEXT("\n") + FString::Join(Lines, TEXT("\n")));
		}
	}

	/**
	 * 굽기 전 색 보기(Separated Parts, 칠 넷): 겉 메시 사본(ColorBaseMesh)을 지금 값으로 정점마다 칠한다. 굽기가 쓰는 함수와 같은 함수로 값을
	 * 낸다 — Separated Parts는 ResolveSeparatedPairs + 메시 스킨 웨이트, 칠은 SamplePaintKind(슬롯 마스크를 메시 UV0에서 읽음).
	 * 결과를 bBeforeColorShown·Status·bProblem에 남긴다.
	 */
	void BuildBeforeColors(const USoftbodyAuthoringData& Data, UObject* Target)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		bBeforeColorShown = false;
		bProblem = false;
		const FMeshDescription* MD = LodZero(Target);
		if (!Target || !MD || ColorBaseMesh.TriangleCount() == 0)
		{
			bProblem = true;
			Status = LOCTEXT("ColorNoTarget", "Pick a Target Mesh.");
			return;
		}
		if (View == EPreviewView::PaintSoftness)
		{
			BuildHardPaint(Data, Target, *MD);
			return;
		}
		TArray<FLinearColor> VertexColors;   // 메시 설명 정점 ID 자리
		VertexColors.Init(FLinearColor(0.35f, 0.35f, 0.35f), MD->Vertices().GetArraySize());
		if (View == EPreviewView::SeparatedParts)
		{
			const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Target);
			if (!Skeletal)
			{
				bProblem = true;
				Status = LOCTEXT("SeparatedStatic", "Separated Pairs need a skeletal mesh: the sides are read from bone weights.");
			}
			else
			{
				const SoftbodyBakeShared::FSeparatedSides Sides = SoftbodyBakeShared::ResolveSeparatedPairs(Skeletal, Data.SeparatedPairs);
				const FSkinWeightsVertexAttributesConstRef SkinWeights = FSkeletalMeshConstAttributes(*MD).GetVertexSkinWeights();
				const FLinearColor None(0.35f, 0.35f, 0.35f);
				const FLinearColor First = FLinearColor::FromSRGBColor(FColor(255, 120, 30));
				const FLinearColor Second = FLinearColor::FromSRGBColor(FColor(40, 120, 255));
				int32 NumOnSides = 0;
				if (SkinWeights.IsValid())
				{
					for (const FVertexID VertID : MD->Vertices().GetElementIDs())
					{
						float Share[2] = { 0.f, 0.f };
						for (UE::AnimationCore::FBoneWeight BW : SkinWeights.Get(VertID))
						{
							const int32 Bone = (int32)BW.GetBoneIndex();
							const uint8 Side = Sides.BoneSide.IsValidIndex(Bone) ? Sides.BoneSide[Bone] : 0;
							if (Side != 0)
							{
								Share[Side - 1] += BW.GetWeight();
							}
						}
						const float A = FMath::Clamp(Share[0], 0.f, 1.f);
						const float B = FMath::Clamp(Share[1], 0.f, 1.f - A);
						VertexColors[VertID.GetValue()] = None * (1.f - A - B) + First * A + Second * B;
						NumOnSides += A + B > 0.f ? 1 : 0;
					}
				}
				if (Data.SeparatedPairs.Num() == 0)
				{
					Status = LOCTEXT("SeparatedNone", "No Separated Pairs. Add a pair in 6. Contact Map to keep two parts apart.");
				}
				else if (Sides.Problems.Num() > 0)
				{
					bProblem = true;
					Status = FText::FromString(FString::Join(Sides.Problems, TEXT("\n")));
				}
				else if (NumOnSides == 0)
				{
					bProblem = true;
					Status = LOCTEXT("SeparatedEmpty", "No vertex is skinned to the bones of the Separated Pairs.");
				}
				else
				{
					Status = FText::Format(LOCTEXT("SeparatedOk", "{0} {0}|plural(one=pair,other=pairs), with the current values. Rebake Contact Map applies changes."),
						Sides.NumPairsUsed);
				}
			}
		}
		else if (const TOptional<SoftbodyBakeShared::EPaintKind> Kind = PaintKindOf(View); Kind.IsSet())
		{
			TArray<bool> Painted;
			const TArray<float> Values = SoftbodyBakeShared::SamplePaintKind(Target, *MD, Data.SlotMasks, Kind.GetValue(), Painted);
			const FLinearColor Unpainted = FLinearColor::FromSRGBColor(FColor(150, 185, 235));
			int32 NumPainted = 0;
			for (const FVertexID VertID : MD->Vertices().GetElementIDs())
			{
				const int32 V = VertID.GetValue();
				if (Painted.IsValidIndex(V) && Painted[V] && Values.IsValidIndex(V))
				{
					const uint8 Gray = (uint8)FMath::RoundToInt32(FMath::Clamp(Values[V], 0.f, 1.f) * 255.f);
					VertexColors[V] = FLinearColor::FromSRGBColor(FColor(Gray, Gray, Gray));
					++NumPainted;
				}
				else
				{
					VertexColors[V] = Unpainted;
				}
			}
			Status = NumPainted > 0
				? ViewInfo(View).Tooltip
				: LOCTEXT("PaintNone", "Nothing is painted. Set a Paint Texture, or White or Black everywhere, in 4. Paint > Slot Masks.");
			// 텍스처가 빈 텍스처 채널(칠하지 않은 슬롯처럼 든다)과 sRGB 마스크를 굽기와 같은 판정으로 알린다(미리보기 = 결과).
			AppendMaskProblems(Data, Kind);
		}
		FDynamicMesh3 Mesh = ColorBaseMesh;
		Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
		CreatePerVertexColors(Colors);
		for (const int32 V : Mesh.VertexIndicesItr())
		{
			const FLinearColor Color = ColorSourceVertices.IsValidIndex(V) && VertexColors.IsValidIndex(ColorSourceVertices[V].GetValue())
				? VertexColors[ColorSourceVertices[V].GetValue()] : FLinearColor(0.35f, 0.35f, 0.35f);
			Colors->SetElement(V, FVector4f(Color.R, Color.G, Color.B, 1.f));
		}
		BeforeColorComponent->SetMesh(MoveTemp(Mesh));
		bBeforeColorShown = true;
	}

	void OnObjectChanged(UObject* Object, FPropertyChangedEvent&)
	{
		if (!Object)
		{
			return;
		}
		if (Object == BuiltTarget.Get() || Object == BuiltVolume.Get())
		{
			bMeshesBuilt = false;
			BuiltKey = 0;
			PendingKey = 0;
			ResultKey = 0;
			ThicknessKey = 0;
			LumpBaseKey = 0;
			HardPiecesKey = 0;
			return;
		}
		// 칠 보기: 슬롯 마스크 텍스처를 다시 들여오면 설정값은 그대로라 해시로는 모른다.
		const USoftbodyAuthoringData* Data = Asset.Get();
		if (Data && IsBeforeColorView() && Cast<UTexture2D>(Object))
		{
			for (const TPair<FName, FSoftbodyPaintSlotMask>& Pair : Data->SlotMasks)
			{
				if (Pair.Value.Texture == Object)
				{
					BuiltKey = 0;
					PendingKey = 0;
				}
			}
		}
	}

	/** 굽기 전 색 보기가 읽는 값: Separated Pairs와 Slot Masks(텍스처와 채널마다의 Fill). */
	static uint32 ColorKey(const USoftbodyAuthoringData& Data)
	{
		uint32 Key = GetTypeHash(Data.SeparatedPairs.Num());
		for (const FSoftbodySeparatedPair& Pair : Data.SeparatedPairs)
		{
			Key = HashCombine(Key, GetTypeHash(Pair.SideA.Num()));
			for (const FName Bone : Pair.SideA)
			{
				Key = HashCombine(Key, GetTypeHash(Bone));
			}
			Key = HashCombine(Key, GetTypeHash(Pair.SideB.Num()));
			for (const FName Bone : Pair.SideB)
			{
				Key = HashCombine(Key, GetTypeHash(Bone));
			}
		}
		for (const TPair<FName, FSoftbodyPaintSlotMask>& Pair : Data.SlotMasks)
		{
			const FSoftbodyPaintSlotMask& Mask = Pair.Value;
			Key = HashCombine(Key, HashCombine(GetTypeHash(Pair.Key), GetTypeHash(Mask.Texture.Get())));
			for (const ESoftbodyPaintFill Fill : { Mask.DepthFill, Mask.JiggleFill, Mask.VolumeFill, Mask.SoftnessFill })
			{
				Key = HashCombine(Key, GetTypeHash((uint8)Fill));
			}
		}
		return Key;
	}

	static uint32 SettingsKey(const USoftbodyAuthoringData& Data)
	{
		uint32 Key = GetTypeHash(Data.TargetMesh.Get());
		Key = HashCombine(Key, GetTypeHash(Data.VolumeMesh.Get()));
		Key = HashCombine(Key, GetTypeHash((uint8)Data.StaticCore));
		Key = HashCombine(Key, GetTypeHash(Data.DefaultRigidShrink));
		Key = HashCombine(Key, GetTypeHash(Data.CellSizeFrac));
		for (const FSoftbodyStaticRigidSphere& Sphere : Data.StaticRigidSpheres)
		{
			Key = HashCombine(Key, HashCombine(GetTypeHash(Sphere.Center), GetTypeHash(Sphere.Radius)));
		}
		for (const FSoftbodyRigidBoneOverride& Override : Data.RigidBoneOverrides)
		{
			Key = HashCombine(Key, HashCombine(GetTypeHash(Override.BoneName), HashCombine(GetTypeHash((uint8)Override.Role), GetTypeHash(Override.Shrink))));
			Key = HashCombine(Key, GetTypeHash(Override.bIncludeChildren));
		}
		// 살 슬롯(일지 239)과 굽기: 뼈로 치는 본이 슬롯 머티리얼·Softbody Material Slots로 바뀌고, 상태 줄이 마지막 굽기와 견준다.
		Key = HashCombine(Key, GetTypeHash(Data.bCreateSoftbodyMaterial));
		for (const FName Slot : Data.SoftbodyMaterialSlots)
		{
			Key = HashCombine(Key, GetTypeHash(Slot));
		}
		for (const UMaterialInterface* Material : SoftbodyAuthoringEditorLocal::AssetMaterials(Data.TargetMesh.Get()))
		{
			Key = HashCombine(Key, GetTypeHash(Material));
		}
		Key = HashCombine(Key, HashCombine(GetTypeHash(Data.BakedData.Get()), GetTypeHash(Data.GetRunSerial())));
		return Key;
	}

	void Rebuild(USoftbodyAuthoringData& Data)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		BuiltKey = PendingKey;
		bBuiltOnce = true;

		UObject* Target = Data.TargetMesh.Get();
		UStaticMesh* Volume = Data.VolumeMesh.Get() != Target ? Data.VolumeMesh.Get() : nullptr;
		if (Target != BuiltTarget.Get() || Volume != BuiltVolume.Get() || !bMeshesBuilt)
		{
			// 굽기 전 색 보기가 칠할 사본도 같이(메시 설명 정점 ID 대응표와 함께) — 값을 고칠 때마다 메시를 다시 바꾸지 않게.
			ColorSourceVertices.Reset();
			ColorSourceTriangles.Reset();
			FDynamicMesh3 TargetMesh = ToPreviewMesh(LodZero(Target), &ColorSourceVertices, &ColorSourceTriangles);
			ColorBaseMesh = TargetMesh;
			FDynamicMesh3 VolumeMesh = ToPreviewMesh(Volume ? Volume->GetMeshDescription(0) : nullptr);
			MeshBox = FBox(ForceInit);
			for (const FDynamicMesh3* Mesh : { &TargetMesh, &VolumeMesh })
			{
				if (Mesh->TriangleCount() > 0)
				{
					const FAxisAlignedBox3d Bounds = Mesh->GetBounds();
					MeshBox += FBox(FVector(Bounds.Min), FVector(Bounds.Max));
				}
			}
			MeshComponent->SetMesh(MoveTemp(TargetMesh));
			VolumeComponent->SetMesh(MoveTemp(VolumeMesh));
			BuiltTarget = Target;
			BuiltVolume = Volume;
			ApplyVisibility();
			bMeshesBuilt = true;
			bFramed = false;
		}

		// 속은 속 보기에서만 계산한다(큰 몸은 시간이 든다). 다른 굽기 전 보기로 갔다 오면 키가 바뀌어 다시 계산한다.
		if (View == EPreviewView::RigidCore)
		{
			FString Text;
			const TArray<FVector4> Spheres = Data.PreviewRigidCore(Text, bProblem);
			CoreComponent->SetMesh(SpheresMesh(Spheres, MeshBox));
			Status = FText::FromString(Text);
		}
		else if (IsBeforeColorView())
		{
			BuildBeforeColors(Data, Target);
			ApplyVisibility();
		}

		// 처음(또는 메시를 바꾼 뒤)에만 화면에 맞추고 바닥을 메시 밑에 둔다. 그 뒤로는 사용자가 돌려 둔 시점을 지킨다.
		if (!bFramed && MeshBox.IsValid)
		{
			FocusViewportOnBox(MeshBox, true);
			Scene.SetFloorOffset((float)-MeshBox.Min.Z);
			bFramed = true;
		}
		Invalidate();
	}

	FAdvancedPreviewScene& Scene;
	TWeakObjectPtr<USoftbodyAuthoringData> Asset;
	// 미리보기 장면이 참조를 쥔다(FPreviewScene::AddReferencedObjects).
	UDynamicMeshComponent* MeshComponent = nullptr;
	UDynamicMeshComponent* VolumeComponent = nullptr;
	UDynamicMeshComponent* CoreComponent = nullptr;
	// 굽기 결과 보기(일지 233): 메시 종류마다 하나, ResultShown이 지금 보이는 것.
	UStaticMeshComponent* ResultStatic = nullptr;
	USkeletalMeshComponent* ResultSkeletal = nullptr;
	UMeshComponent* ResultShown = nullptr;
	// 살 두께 보기: 정점 색으로 칠한 메시 사본. ThicknessKey = 칠한 때의 ResultSettingsKey(같으면 다시 칠하지 않는다).
	UDynamicMeshComponent* ThicknessComponent = nullptr;
	uint32 ThicknessKey = 0;
	bool bThicknessBuilt = false;
	bool bThicknessShown = false;
	FText ThicknessStatus;
	FText ThicknessLegend;
	// 굽기 전 색 보기(Separated Parts, 칠): 겉 메시 사본(ColorBaseMesh, 대상이 바뀔 때 만든다)과 그 정점 → 메시 설명 정점 대응표.
	UDynamicMeshComponent* BeforeColorComponent = nullptr;
	FDynamicMesh3 ColorBaseMesh;
	TArray<FVertexID> ColorSourceVertices;
	// 같은 사본의 삼각형 → 메시 설명 삼각형(Hard Surface Paint 보기가 조각을 삼각형마다 칠한다)과 그 보기의 색 안내(조각 수가 들어간다).
	TArray<FTriangleID> ColorSourceTriangles;
	FText HardPaintLegend;
	bool bBeforeColorShown = false;
	bool bViewChanged = false;
	// Lump Base 보기: 굽기가 남긴 받침 막. LumpBaseKey = 그린 때의 ResultSettingsKey.
	UDynamicMeshComponent* LumpBaseComponent = nullptr;
	uint32 LumpBaseKey = 0;
	bool bLumpBaseBuilt = false;
	bool bLumpBaseShown = false;
	bool bLumpBaseProblem = false;
	FText LumpBaseStatus;
	FText LumpBaseLegend;
	// Hard Surface Pieces 보기: 삼각형마다 칠한 메시 사본. HardPiecesKey = 그린 때의 ResultSettingsKey.
	UDynamicMeshComponent* HardPiecesComponent = nullptr;
	uint32 HardPiecesKey = 0;
	bool bHardPiecesBuilt = false;
	bool bHardPiecesShown = false;
	bool bHardPiecesProblem = false;
	FText HardPiecesStatus;
	FText HardPiecesLegend;
	// 플러그인 에셋 — 겉 메시의 MID가 부모로 쥔다.
	UMaterialInterface* ShellMaterial = nullptr;

	SoftbodyAuthoringEditorLocal::EPreviewView View = SoftbodyAuthoringEditorLocal::EPreviewView::RigidCore;
	uint32 ResultKey = 0;
	FText ResultStatus;
	bool bResultProblem = false;

	FDelegateHandle MeshChangedHandle;
	TWeakObjectPtr<UObject> BuiltTarget;
	FBox MeshBox = FBox(ForceInit);
	TWeakObjectPtr<UStaticMesh> BuiltVolume;
	uint32 BuiltKey = 0;
	uint32 PendingKey = 0;
	double PendingSince = 0.0;
	bool bBuiltOnce = false;
	bool bMeshesBuilt = false;
	bool bFramed = false;
	FText Status;
	bool bProblem = false;
};

/** 미리보기 뷰포트: 미리보기 장면을 갖고, 왼쪽 아래에 지금 속의 설명(또는 문제)과 색 안내를 띄운다. */
class SSoftbodyCorePreviewViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SSoftbodyCorePreviewViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args, USoftbodyAuthoringData* InAsset)
	{
		Asset = InAsset;
		PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
		SEditorViewport::Construct(SEditorViewport::FArguments());
		LivePreviews().RemoveAll([](const TWeakPtr<SSoftbodyCorePreviewViewport>& Weak) { return !Weak.IsValid(); });
		LivePreviews().Add(StaticCastSharedRef<SSoftbodyCorePreviewViewport>(AsShared()));
	}

	/** 열린 SA 창의 미리보기들(Softbody.PreviewView가 보기를 바꾼다). */
	static TArray<TWeakPtr<SSoftbodyCorePreviewViewport>>& LivePreviews()
	{
		static TArray<TWeakPtr<SSoftbodyCorePreviewViewport>> Previews;
		return Previews;
	}

	SoftbodyAuthoringEditorLocal::EPreviewView GetView() const
	{
		return Client.IsValid() ? Client->GetView() : SoftbodyAuthoringEditorLocal::EPreviewView::RigidCore;
	}

	void SetView(SoftbodyAuthoringEditorLocal::EPreviewView View)
	{
		if (Client.IsValid())
		{
			Client->SetView(View);
		}
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
	{
		Client = MakeShared<FSoftbodyCorePreviewClient>(*PreviewScene, SharedThis(this), Asset.Get());
		return Client.ToSharedRef();
	}

	virtual TSharedPtr<SWidget> BuildViewportToolbar() override
	{
		const FName ToolbarName(TEXT("SoftbodyAuthoringEditor.ViewportToolbar"));
		if (!UToolMenus::Get()->IsMenuRegistered(ToolbarName))
		{
			UToolMenu* Menu = UToolMenus::Get()->RegisterMenu(ToolbarName, NAME_None, EMultiBoxType::SlimHorizontalToolBar);
			Menu->StyleName = "ViewportToolbar";
			// 무엇을 볼지: 굽기 전 속 하나 + 마지막 굽기 결과 여섯(일지 233). 메뉴는 한 번 등록하므로 뷰포트는 문맥에서 찾는다.
			FToolMenuSection& Left = Menu->AddSection("Left");
			Left.AddDynamicEntry("SoftbodyPreviewView", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section)
			{
				UUnrealEdViewportToolbarContext* Context = Section.FindContext<UUnrealEdViewportToolbarContext>();
				if (!Context)
				{
					return;
				}
				const TWeakPtr<SSoftbodyCorePreviewViewport> Viewport = StaticCastWeakPtr<SSoftbodyCorePreviewViewport>(Context->Viewport);
				Section.AddEntry(FToolMenuEntry::InitSubMenu(
					"SoftbodyPreviewView",
					TAttribute<FText>::CreateLambda([Viewport]()
					{
						const TSharedPtr<SSoftbodyCorePreviewViewport> Pinned = Viewport.Pin();
						return SoftbodyAuthoringEditorLocal::ViewInfo(Pinned ? Pinned->GetView() : SoftbodyAuthoringEditorLocal::EPreviewView::RigidCore).Label;
					}),
					LOCTEXT("ViewMenuTip", "What the preview shows: the current values and paint before baking, or a view of the last bake."),
					FNewToolMenuDelegate::CreateLambda([Viewport](UToolMenu* Submenu) { FillViewMenu(Submenu, Viewport); }),
					false,
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Visible")));
			}));
			FToolMenuSection& Right = Menu->AddSection("Right");
			Right.Alignment = EToolMenuSectionAlign::Last;
			Right.AddEntry(UE::UnrealEd::CreateCameraSubmenu(UE::UnrealEd::FViewportCameraMenuOptions().ShowAll()));
			// 보기 모드 메뉴는 부모 메뉴를 먼저 등록해야 한다(엔진 뷰포트들과 같은 순서).
			const FName ViewParent(TEXT("UnrealEd.ViewportToolbar.View"));
			if (!UToolMenus::Get()->IsMenuRegistered(ViewParent))
			{
				UToolMenus::Get()->RegisterMenu(ViewParent);
			}
			UToolMenus::Get()->RegisterMenu("SoftbodyAuthoringEditor.ViewportToolbar.ViewModes", ViewParent);
			Right.AddEntry(UE::UnrealEd::CreateViewModesSubmenu());
			Right.AddEntry(UE::UnrealEd::CreateDefaultShowSubmenu());
		}
		FToolMenuContext Context;
		Context.AppendCommandList(GetCommandList());
		UUnrealEdViewportToolbarContext* ContextObject = NewObject<UUnrealEdViewportToolbarContext>();
		ContextObject->Viewport = SharedThis(this);
		ContextObject->bShowCoordinateSystemControls = false;
		ContextObject->bShowSurfaceSnap = false;
		Context.AddObject(ContextObject);
		return UToolMenus::Get()->GenerateWidget(ToolbarName, Context);
	}

	virtual void PopulateViewportOverlays(TSharedRef<SOverlay> Overlay) override
	{
		SEditorViewport::PopulateViewportOverlays(Overlay);
		Overlay->AddSlot()
			.VAlign(VAlign_Bottom)
			.HAlign(HAlign_Left)
			.Padding(8.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("FloatingBorder"))
				.Padding(8.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return Client.IsValid() ? Client->GetStatus() : FText::GetEmpty(); })
						.ColorAndOpacity_Lambda([this]()
						{
							return Client.IsValid() && Client->HasProblem() ? FSlateColor(FLinearColor(1.f, 0.35f, 0.3f)) : FSlateColor::UseForeground();
						})
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return Client.IsValid() ? Client->GetLegend() : FText::GetEmpty(); })
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			];
	}

private:
	static void FillViewMenu(UToolMenu* Menu, TWeakPtr<SSoftbodyCorePreviewViewport> Viewport)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		auto AddView = [&Viewport](FToolMenuSection& Section, EPreviewView View)
		{
			const FPreviewViewInfo& Info = ViewInfo(View);
			Section.AddMenuEntry(NAME_None, Info.Label, Info.Tooltip, FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Viewport, View]()
					{
						if (const TSharedPtr<SSoftbodyCorePreviewViewport> Pinned = Viewport.Pin())
						{
							Pinned->SetView(View);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Viewport, View]()
					{
						const TSharedPtr<SSoftbodyCorePreviewViewport> Pinned = Viewport.Pin();
						return Pinned && Pinned->GetView() == View;
					})),
				EUserInterfaceActionType::RadioButton);
		};
		FToolMenuSection& Before = Menu->FindOrAddSection("BeforeBake", LOCTEXT("ViewBeforeBake", "Before Bake"));
		AddView(Before, EPreviewView::RigidCore);
		AddView(Before, EPreviewView::SeparatedParts);
		FToolMenuSection& Paint = Menu->FindOrAddSection("Paint", LOCTEXT("ViewPaint", "Paint (Current Slot Masks)"));
		for (const EPreviewView View : PaintViews)
		{
			AddView(Paint, View);
		}
		FToolMenuSection& Last = Menu->FindOrAddSection("LastBake", LOCTEXT("ViewLastBake", "Last Bake"));
		for (const EPreviewView View : ResultViews)
		{
			AddView(Last, View);
		}
	}

	TWeakObjectPtr<USoftbodyAuthoringData> Asset;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FSoftbodyCorePreviewClient> Client;
};

// 개발: 창 없는 캡처·위키 그림용. 사용자는 미리보기 도구 막대의 보기 메뉴를 쓴다.
static FAutoConsoleCommand GSoftbodyPreviewViewCommand(
	TEXT("Softbody.PreviewView"),
	TEXT("Sets what the open Softbody Authoring Data previews show: -1 = Rigid Core, -2 = Separated Parts, -3 to -6 = Depth, Jiggle, ")
	TEXT("Volume and Flex paint, 0 to 5 = the last bake in the colors of that Debug View Mode, ")
	TEXT("6 = Flesh Thickness (the last bake's flesh thickness in cm), 7 = Lump Base, 8 = Plates."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		using namespace SoftbodyAuthoringEditorLocal;
		const int32 Mode = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : -1;
		const EPreviewView View = (Mode >= (int32)EPreviewView::PaintSoftness && Mode <= (int32)EPreviewView::HardPieces)
			? (EPreviewView)Mode : EPreviewView::RigidCore;
		// 캡처용: 창이 Simulation 탭을 앞에 두고 열렸어도 Preview 탭이 보이게 앞으로 가져온다.
		if (UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			for (UObject* Edited : Editors->GetAllEditedAssets())
			{
				IAssetEditorInstance* Instance = Cast<USoftbodyAuthoringData>(Edited) ? Editors->FindEditorForAsset(Edited, false) : nullptr;
				if (const TSharedPtr<FTabManager> Tabs = Instance ? Instance->GetAssociatedTabManager() : nullptr)
				{
					Tabs->TryInvokeTab(FTabId(PreviewTabId));
				}
			}
		}
		for (const TWeakPtr<SSoftbodyCorePreviewViewport>& Weak : SSoftbodyCorePreviewViewport::LivePreviews())
		{
			if (const TSharedPtr<SSoftbodyCorePreviewViewport> Viewport = Weak.Pin())
			{
				Viewport->SetView(View);
			}
		}
	}));

// ---- Simulation 탭(일지 234) ----

bool USoftbodySimulationPreviewSettings::ShouldHideAnimation(const FAssetData& AssetData) const
{
	const USkeleton* Target = Skeleton.Get();
	return !Target || !Target->IsCompatibleForEditor(AssetData);
}

namespace SoftbodyAuthoringEditorLocal
{
	const FName SimulationTabId(TEXT("SoftbodyAuthoringEditor_Simulation"));
	const FName SimulationDetailsTabId(TEXT("SoftbodyAuthoringEditor_SimulationDetails"));

	/** 미리보기 컴포넌트에서 사용자가 고치는 값: USoftbodyComponent가 선언한 편집 값 중 메시·Mesh Data(SA의 마지막 굽기로 정한다)를 뺀 것. */
	bool IsSimulationValue(const FProperty* Property)
	{
		static const FName TargetName = GET_MEMBER_NAME_CHECKED(USoftbodyComponent, TargetMeshComponent);
		static const FName DataName = GET_MEMBER_NAME_CHECKED(USoftbodyComponent, MeshData);
		return Property && Property->GetOwnerClass() == USoftbodyComponent::StaticClass()
			&& Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient)
			&& Property->GetFName() != TargetName && Property->GetFName() != DataName;
	}

	void CopySimulationValues(const USoftbodyComponent& From, USoftbodyComponent& To)
	{
		for (TFieldIterator<FProperty> It(USoftbodyComponent::StaticClass()); It; ++It)
		{
			if (IsSimulationValue(*It))
			{
				It->CopyCompleteValue_InContainer(&To, &From);
			}
		}
	}

	/** 컴포넌트가 값을 넣을 수 있는 머티리얼인가(USoftbodyComponent::IsMaterialCompatible과 같은 세 텍스처 파라미터). */
	bool HasSoftbodyMaterial(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return false;
		}
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Material->GetAllTextureParameterInfo(Infos, Ids);
		int32 Found = 0;
		for (const FMaterialParameterInfo& Info : Infos)
		{
			Found |= Info.Name == TEXT("ContactData") ? 1 : 0;
			Found |= Info.Name == TEXT("InertiaResponseTex") ? 2 : 0;
			Found |= Info.Name == TEXT("SurfaceDispTex") ? 4 : 0;
		}
		return Found == 7;
	}

	/**
	 * Ctrl + 좌클릭 누르기와 클릭 고르기가 광선을 쏘는 겉: 메시 설명의 삼각형(스켈레탈은 기준 자세와 스킨 웨이트, 지금 자세로 스킨해 쏜다).
	 * 콜리전(피직스 에셋 캡슐은 살 속에 있다)도, 눌린 모양도, 누르는 손가락도 아닌 눌리기 전 그려지는 겉이다 — 깊이·눌림이 닿는 점을 바꾸지 않는다.
	 */
	struct FPressMesh
	{
		TArray<FVector3f> Positions;
		TArray<FIntVector3> Triangles;
		/** 삼각형 T의 꼭짓점 C 노멀(메시 설명 정점 인스턴스, 기준 자세)은 CornerNormals[3T + C]. 각진 모서리는 면마다 따로다. */
		TArray<FVector3f> CornerNormals;
		/** 정점 V의 웨이트는 [WeightStart[V], WeightStart[V + 1]). 스태틱이면 비어 있다. */
		TArray<int32> WeightStart;
		TArray<int32> WeightBone;
		TArray<float> WeightValue;

		/** Pose가 채우는 지금 자세의 겉(월드). */
		TArray<FVector> World;
		TArray<FMatrix44f> RefToLocal;
		FTransform PosedTransform = FTransform::Identity;
		bool bPosed = false;

		/** 광선이 맞은 삼각형과 그 안의 자리(꼭짓점 1·2 쪽 무게 U·W). */
		struct FHit
		{
			int32 Triangle = INDEX_NONE;
			double T = 0.0;
			double U = 0.0;
			double W = 0.0;
		};

		/** Mesh: 노멀을 렌더와 같게 읽을 메시(빌드 설정이 다시 계산하면 다시 계산한 노멀, 일지 345). */
		void Build(const FMeshDescription* MD, const UObject* Mesh, bool bSkinned)
		{
			*this = FPressMesh();
			if (!MD)
			{
				return;
			}
			TArray<int32> Remap;
			Remap.Init(INDEX_NONE, MD->Vertices().GetArraySize());
			const TVertexAttributesRef<const FVector3f> Source = MD->GetVertexPositions();
			for (const FVertexID Vertex : MD->Vertices().GetElementIDs())
			{
				Remap[Vertex.GetValue()] = Positions.Add(Source[Vertex]);
			}
			const TArray<FVector3f> Normals = SoftbodyBakeShared::RenderedNormals(Mesh, *MD);
			for (const FTriangleID Triangle : MD->Triangles().GetElementIDs())
			{
				const TArrayView<const FVertexID> Corners = MD->GetTriangleVertices(Triangle);
				Triangles.Add(FIntVector3(Remap[Corners[0].GetValue()], Remap[Corners[1].GetValue()], Remap[Corners[2].GetValue()]));
				const TArrayView<const FVertexInstanceID> Instances = MD->GetTriangleVertexInstances(Triangle);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					CornerNormals.Add(Normals.IsValidIndex(Instances[Corner].GetValue()) ? Normals[Instances[Corner].GetValue()] : FVector3f::ZeroVector);
				}
			}
			if (bSkinned)
			{
				const FSkeletalMeshConstAttributes Attributes(*MD);
				const FSkinWeightsVertexAttributesConstRef Weights = Attributes.GetVertexSkinWeights();
				if (Weights.IsValid())
				{
					WeightStart.Reserve(Positions.Num() + 1);
					// Positions와 같은 순서(정점 ID 순).
					for (const FVertexID Vertex : MD->Vertices().GetElementIDs())
					{
						WeightStart.Add(WeightBone.Num());
						for (const UE::AnimationCore::FBoneWeight Weight : Weights.Get(Vertex))
						{
							WeightBone.Add(Weight.GetBoneIndex());
							WeightValue.Add(Weight.GetWeight());
						}
					}
					WeightStart.Add(WeightBone.Num());
				}
			}
		}

		/** 정점 V를 지금 자세로(웨이트를 섞은 뼈 행렬, 노멀이면 방향만). 컴포넌트 공간. */
		FVector3f Skin(int32 V, const FVector3f& Value, bool bDirection) const
		{
			if (RefToLocal.Num() == 0)
			{
				return Value;
			}
			FVector3f Sum = FVector3f::ZeroVector;
			float Total = 0.f;
			for (int32 K = WeightStart[V]; K < WeightStart[V + 1]; ++K)
			{
				if (RefToLocal.IsValidIndex(WeightBone[K]))
				{
					const FMatrix44f& Bone = RefToLocal[WeightBone[K]];
					Sum += FVector3f(bDirection ? Bone.TransformVector(Value) : Bone.TransformPosition(Value)) * WeightValue[K];
					Total += WeightValue[K];
				}
			}
			return Total > 0.f ? Sum / Total : Value;
		}

		/**
		 * 지금 자세의 겉을 월드로 편다. 자세가 그대로일 수 있으면(bMayChange 거짓) 컴포넌트가 움직이지 않은 동안 지난 것을 쓴다 —
		 * 스켈레탈은 애니메이션이 돌 때만 매번 스킨한다.
		 */
		void Pose(const UMeshComponent& Component, bool bMayChange)
		{
			const FTransform ToWorld = Component.GetComponentTransform();
			if (bPosed && !bMayChange && ToWorld.Equals(PosedTransform, 1e-4))
			{
				return;
			}
			RefToLocal.Reset();
			const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(&Component);
			if (Skinned && WeightStart.Num() == Positions.Num() + 1)
			{
				Skinned->CacheRefToLocalMatrices(RefToLocal);
			}
			World.SetNumUninitialized(Positions.Num());
			for (int32 V = 0; V < Positions.Num(); ++V)
			{
				World[V] = ToWorld.TransformPosition(FVector(Skin(V, Positions[V], false)));
			}
			PosedTransform = ToWorld;
			bPosed = true;
		}

		/**
		 * Pose로 편 겉에 광선(양면, MinT보다 먼 가장 가까운 것). Skip 삼각형은 건너뛰고, bExitOnly면 광선이 겉 밖으로 나가는 면
		 * (면 노멀이 광선 쪽)만 센다 — 겹친 옷 층처럼 들어가는 면은 지나친다.
		 */
		bool Trace(const FVector& Origin, const FVector& Direction, double MinT, int32 Skip, bool bExitOnly, FHit& Out) const
		{
			Out = FHit();
			double Best = TNumericLimits<double>::Max();
			for (int32 Index = 0; Index < Triangles.Num(); ++Index)
			{
				if (Index == Skip)
				{
					continue;
				}
				// Möller–Trumbore
				const FIntVector3& Triangle = Triangles[Index];
				const FVector& A = World[Triangle.X];
				const FVector E1 = World[Triangle.Y] - A;
				const FVector E2 = World[Triangle.Z] - A;
				const FVector P = Direction ^ E2;
				const double Det = E1 | P;
				if (FMath::Abs(Det) < 1e-12)
				{
					continue;
				}
				const double Inv = 1.0 / Det;
				const FVector S = Origin - A;
				const double U = (S | P) * Inv;
				if (U < 0.0 || U > 1.0)
				{
					continue;
				}
				const FVector Q = S ^ E1;
				const double W = (Direction | Q) * Inv;
				if (W < 0.0 || U + W > 1.0)
				{
					continue;
				}
				const double T = (E2 | Q) * Inv;
				// 엔진의 면 노멀은 (C − A) × (B − A)(FMeshDescription과 같다).
				if (T > MinT && T < Best && (!bExitOnly || ((E2 ^ E1) | Direction) > 0.0))
				{
					Best = T;
					Out.Triangle = Index;
					Out.T = T;
					Out.U = U;
					Out.W = W;
				}
			}
			return Out.Triangle != INDEX_NONE;
		}

		/** 광선 Direction이 이 면에서 겉 밖으로 나가는가(Trace의 bExitOnly와 같은 판정). */
		bool Exits(const FHit& Hit, const FVector& Direction) const
		{
			const FIntVector3& Triangle = Triangles[Hit.Triangle];
			const FVector& A = World[Triangle.X];
			return (((World[Triangle.Z] - A) ^ (World[Triangle.Y] - A)) | Direction) > 0.0;
		}

		FVector Point(const FHit& Hit) const
		{
			const FIntVector3& Triangle = Triangles[Hit.Triangle];
			return World[Triangle.X] * (1.0 - Hit.U - Hit.W) + World[Triangle.Y] * Hit.U + World[Triangle.Z] * Hit.W;
		}

		/**
		 * 맞은 자리의 바깥 노멀(월드): 꼭짓점 노멀을 지금 자세로 돌려 맞은 자리에서 섞는다 — 삼각형을 넘어가도 공 방향이 튀지 않는다.
		 * 메시 설명에 노멀이 없거나 면과 반대면 면 노멀.
		 */
		FVector Normal(const FHit& Hit) const
		{
			const FIntVector3& Triangle = Triangles[Hit.Triangle];
			const FVector Face = ((World[Triangle.Z] - World[Triangle.X]) ^ (World[Triangle.Y] - World[Triangle.X])).GetSafeNormal();
			const double Weights[3] = { 1.0 - Hit.U - Hit.W, Hit.U, Hit.W };
			const int32 Vertices[3] = { Triangle.X, Triangle.Y, Triangle.Z };
			FVector3f Local = FVector3f::ZeroVector;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Local += Skin(Vertices[Corner], CornerNormals[3 * Hit.Triangle + Corner], true) * (float)Weights[Corner];
			}
			// 늘인 컴포넌트에서 노멀은 축척의 역수로(역전치).
			const FVector Smooth = PosedTransform.GetRotation().RotateVector(FVector(Local) * FTransform::GetSafeScaleReciprocal(PosedTransform.GetScale3D())).GetSafeNormal();
			return (Smooth | Face) > 0.0 ? Smooth : Face;
		}

		/** 지금 자세의 겉에 광선을 쏴 가장 가까운 거리(OutT)를 낸다. 양면. */
		bool Raycast(const UMeshComponent& Component, const FVector& Origin, const FVector& Direction, double& OutT)
		{
			Pose(Component, true);
			FHit Hit;
			const bool bHit = Trace(Origin, Direction, 0.0, INDEX_NONE, false, Hit);
			OutT = Hit.T;
			return bHit;
		}
	};
}

/**
 * Simulation 탭의 뷰포트 클라이언트(일지 234). 미리보기 월드에 액터 하나(메시 + Softbody 컴포넌트)를 두고, 뷰포트가 보이는 동안만 월드를 틱한다
 * (엔진은 보이는 뷰포트만 틱한다 — 다른 탭 뒤에 숨으면 시뮬레이션도 GPU 일도 멈춘다). 게임과 같은 길: 액터의 BeginPlay로 컴포넌트가 시작하고,
 * 누르는 손가락은 채널 겹침으로 찾히는 진짜 캡슐 콜라이더다.
 * 마지막 굽기·대상 메시·메시 슬롯 머티리얼이 바뀌거나 실행 버튼이 끝나면(GetRunSerial) 액터를 새로 세운다 — 컴포넌트는 세팅할 때 굽기 데이터를 읽는다.
 */
class FSoftbodySimulationClient : public FEditorViewportClient
{
public:
	/** Press = Ctrl + 좌클릭 공 누르기, Skin = Ctrl + 우클릭 피부 잡기(ADR-0041 — 원 안을 Softbody Grabber로), Grab = Ctrl + Shift + 우클릭 메시 전체 끌기. */
	enum class EHand : uint8 { None, Press, Grab, Skin };

	FSoftbodySimulationClient(FAdvancedPreviewScene& InScene, const TSharedRef<SEditorViewport>& InWidget, USoftbodyAuthoringData* InAsset,
		USoftbodySimulationPreviewSettings* InSettings, USoftbodyComponent* InValues)
		: FEditorViewportClient(nullptr, &InScene, InWidget)
		, Scene(InScene)
		, Asset(InAsset)
		, Settings(InSettings)
		, Values(InValues)
	{
		SetRealtime(true);
		EngineShowFlags.SetGrid(false);
		EngineShowFlags.SetSelection(true);
		EngineShowFlags.SetSelectionOutline(true);
		SetViewLocation(FVector(-75.0, 130.0, 70.0));
		SetViewRotation(FRotator(-25.0, -60.0, 0.0));
		// 기즈모는 편집 모드의 선택(고른 액터)이 있을 때만 그려진다. 이 탭에는 고를 것이 메시 하나라 모드 도구 없이 GetWidgetMode·위치만으로 그린다
		// (FWidget은 모드 도구가 없으면 그 검사를 건너뛴다). 5.8의 새 기즈모(뷰포트 ITF)가 켜져 있으면 옛 기즈모를 그리지 않으므로 이 뷰포트는 옛
		// 조작(기즈모·카메라)을 쓴다 — InputWidgetDelta가 메시를 옮긴다.
		Widget->SetUsesEditorModeTools(nullptr);
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		ModeTools->SetSupportsViewportITF(false);
#endif

		// 누르는 손가락: 모든 채널을 막는 질의 전용 캡슐(Collider Channel을 바꿔도 찾힌다). 누르는 동안만 콜리전을 켠다. 모양은 Draw가 그린다.
		PressBall = NewObject<UCapsuleComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		PressBall->SetCapsuleSize(1.f, 1.f);
		PressBall->SetVisibility(false);
		PressBall->SetCollisionObjectType(ECC_WorldDynamic);
		PressBall->SetCollisionResponseToAllChannels(ECR_Block);
		PressBall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PressBall->SetGenerateOverlapEvents(false);
		Scene.AddComponent(PressBall, FTransform(FVector(0.0, 0.0, -1.0e5)));
		// 피부 잡기(ADR-0041): 게임과 같은 Softbody Grabber 하나를 장면에 두고 커서로 옮긴다(누르기가 진짜 캡슐 콜라이더를 쓰는 것과 같은 길).
		SkinGrabber = NewObject<USoftbodyGrabberComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Scene.AddComponent(SkinGrabber, FTransform(FVector(0.0, 0.0, -1.0e5)));
	}

	/** 탭을 닫을 때(뷰포트 위젯의 소멸자가 장면보다 먼저 부른다): 고친 값을 창에 돌려주고 액터를 지운다 — 컴포넌트가 표면 장을 놓는다. */
	void Teardown()
	{
		StoreValues();
		DestroyActor();
		if (OnSoftbodyChanged)
		{
			OnSoftbodyChanged(nullptr);
		}
	}

	USoftbodyComponent* GetSoftbody() const { return Softbody.Get(); }
	/** 액터를 새로 세우거나 지운 뒤(값 패널이 새 컴포넌트를, 없으면 보관용을 보게). */
	TFunction<void(USoftbodyComponent*)> OnSoftbodyChanged;

	void Restart() { bRestart = true; }

	FText GetStatus() const { return Status; }
	bool HasProblem() const { return bProblem; }

	FText GetHelp() const
	{
		if (Hand == EHand::Press && Settings.IsValid())
		{
			FNumberFormattingOptions Format = FNumberFormattingOptions::DefaultNoGrouping();
			Format.SetMaximumFractionalDigits(2);
			if (PressDepthNow() < Settings->PressDepth)
			{
				return FText::Format(LOCTEXT("SimHelpPressingFar", "Pressing {0} cm deep with a {1} cm tip. This is as deep as this spot goes before the far side."),
					FText::AsNumber(PressDepthNow(), &Format), FText::AsNumber(Settings->PressRadius, &Format));
			}
			return FText::Format(LOCTEXT("SimHelpPressing", "Pressing {0} cm deep with a {1} cm tip. The mouse wheel presses deeper or shallower."),
				FText::AsNumber(PressDepthNow(), &Format), FText::AsNumber(Settings->PressRadius, &Format));
		}
		if (Hand == EHand::Grab)
		{
			return LOCTEXT("SimHelpPulling", "Dragging the whole mesh. The mouse wheel moves it nearer or farther.");
		}
		FNumberFormattingOptions OneDigit = FNumberFormattingOptions::DefaultNoGrouping();
		OneDigit.SetMinimumFractionalDigits(1);
		OneDigit.SetMaximumFractionalDigits(1);
		// Basic은 옆 끌림이 없다(ADR-0041 §3.8) — 프리뷰 기본값이 Basic이라 첫 시험에서 옆으로 끈 몫이 안 보이는 것을 알린다.
		const USoftbodyComponent* Body = Softbody.Get();
		const FText BasicNote = Body && Body->ContactDetail == ESoftbodyContactDetail::Basic
			? LOCTEXT("SimHelpSkinBasic", " Basic pulls outward only. Full also stretches the skin sideways.") : FText::GetEmpty();
		if (Hand == EHand::Skin)
		{
			USoftbodyGrabberComponent::FGrabState State;
			if (SkinGrabber && SkinGrabber->IsGrabbing() && SkinGrabber->GetGrabState(State) && State.bHeld)
			{
				// 보이는 움직임만 쓴다(손이 끈 길이를 같이 쓰면 덩어리 몫·Basic 옆 몫만큼 달라 규칙을 알 수 없다) — 잡은 자리가 움직인 길이(덩어리
				// + 피부)와 그 끝, 그 가운데 덩어리 몫. 끝 = 1.3 × min(살 두께, 원 반지름) — 어느 쪽이 정했는지 같이 쓴다. 손이 더 가도 놓지 않고 끝에 머문다(덩어리도
				// 같은 끝 안 — 일지 350).
				const FText Why = State.Flesh < SkinRadius
					? FText::Format(LOCTEXT("SimHelpSkinWhyFlesh", "1.3 times the flesh thickness here, {0} cm"), FText::AsNumber(State.Flesh, &OneDigit))
					: FText::Format(LOCTEXT("SimHelpSkinWhyRadius", "1.3 times the grab radius, {0} cm"), FText::AsNumber(SkinRadius, &OneDigit));
				const FText Lump = State.LumpFollow >= 0.05f
					? FText::Format(LOCTEXT("SimHelpSkinLump", " The lump under it moved {0} cm of that."), FText::AsNumber(State.LumpFollow, &OneDigit))
					: FText::GetEmpty();
				if (State.Stretch >= 0.995f * State.MaxStretch)
				{
					return FText::Format(LOCTEXT("SimHelpSkinAtMax", "The held skin moved {0} cm, as far as it goes here ({1}).{2}{3}"),
						FText::AsNumber(State.MaxStretch, &OneDigit), Why, Lump, BasicNote);
				}
				return FText::Format(LOCTEXT("SimHelpSkinPulling", "The held skin moved {0} cm and stops at {1} cm ({2}).{3}{4}"),
					FText::AsNumber(State.Stretch, &OneDigit), FText::AsNumber(State.MaxStretch, &OneDigit), Why, Lump, BasicNote);
			}
			if (SkinGrabber && SkinGrabber->IsGrabbing())
			{
				return FText::Format(LOCTEXT("SimHelpSkinHolding", "Holding the skin in a {0} cm circle. The mouse wheel pulls it nearer or farther.{1}"),
					FText::AsNumber(SkinRadius, &OneDigit), BasicNote);
			}
		}
		if (!SkinFailure.IsEmpty() && (Hand == EHand::Skin || bHoverValid))
		{
			return SkinFailure;
		}
		if (bHoverValid)
		{
			return FText::Format(LOCTEXT("SimHelpSkinHover", "Right drag pulls the skin in the {0} cm circle (the mouse wheel or [ and ] resize it). Left drag presses with a {1} cm tip. Shift + right drag moves the whole mesh."),
				FText::AsNumber(GrabRadiusNow(), &OneDigit), FText::AsNumber(Settings.IsValid() ? Settings->PressRadius : 1.f, &OneDigit));
		}
		if (bSelected)
		{
			return LOCTEXT("SimHelpSelected", "W and E switch the gizmo between moving and rotating. Click empty space to put it away.");
		}
		return LOCTEXT("SimHelp", "Ctrl + left drag presses the mesh and Ctrl + right drag pulls its skin. Click the mesh for the move gizmo.");
	}

	/** 개발: 화면 가운데를 누른다(창 없는 확인용, Softbody.Simulation press). 단추 없이도 놓을 때까지 누른다. */
	void PressAtCenter(float Depth)
	{
		if (Settings.IsValid())
		{
			Settings->PressDepth = Depth;
		}
		if (Viewport)
		{
			ReleaseHand();
			const FIntPoint Size = Viewport->GetSizeXY();
			BeginHand(EHand::Press, Size.X / 2, Size.Y / 2);
		}
	}

	void Release() { ReleaseHand(); }

	/** 개발: Softbody.Simulation mouse가 흉내 낸 마우스 — 운영체제의 단추 상태는 보지 않는다. */
	void SetSyntheticInput(bool bSynthetic) { bSyntheticInput = bSynthetic; }

	/** 개발: 지금 상태를 로그에 한 줄(Softbody.Simulation state). */
	void LogState() const
	{
		const USoftbodyComponent* Body = Softbody.Get();
		const USoftbodySurfaceField* Field = Body ? Body->GetSurfaceField() : nullptr;
		USoftbodySurfaceField::FGpuState Gpu;
		const bool bGpu = Field && Field->GetGpuState(Gpu);
		UE_LOG(LogTemp, Display, TEXT("[Softbody.Simulation] ticks %d, draws %d, realtime %d, actor %d, active %d, field %d, gpu %d pressed %u resting %u dent tiles %u, hand %d button %s selected %d gizmo %d tracking %d, mesh %s, ball %s r %.2f collision %d"),
			TickCount, DrawCount, IsRealtime() ? 1 : 0, Actor.IsValid() ? 1 : 0, Body && Body->IsActive() ? 1 : 0, Field ? 1 : 0,
			bGpu ? 1 : 0, Gpu.bPressed, Gpu.bResting, Gpu.DentTiles, (int32)Hand, *TakenButton.ToString(), bSelected ? 1 : 0, (int32)GetWidgetMode(),
			bIsTracking ? 1 : 0, MeshComponent.IsValid() ? *MeshComponent->GetComponentLocation().ToString() : TEXT("-"),
			*PressBall->GetComponentLocation().ToString(), PressBall->GetUnscaledCapsuleRadius(), (int32)PressBall->GetCollisionEnabled());
		if (Hand == EHand::Press)
		{
			UE_LOG(LogTemp, Display, TEXT("[Softbody.Simulation] press triangle %d point %s normal %s room %.3f depth %.3f measure %.2f ms"),
				PressHit.Triangle, *PressPoint.ToString(), *PressNormal.ToString(), PressRoom, PressDepthNow(), PressMeasureMs);
		}
	}

	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		++TickCount;
		USoftbodyAuthoringData* Data = Asset.Get();
		const uint32 Key = Data ? SetupKey(*Data) : 0;
		if (Key != BuiltKey || bRestart)
		{
			Rebuild(Data, Key);
		}
		ApplyAnimation();
		if (TakenButton.IsValid())
		{
			// 뷰포트 밖에서 단추를 놓아 놓친 경우(창을 바꾸는 등) — 누르기·잡기도, 마우스를 엔진에 넘기지 않던 것도 푼다.
			const bool bHeld = Viewport && Viewport->KeyState(TakenButton)
				&& (bSyntheticInput || FSlateApplication::Get().GetPressedMouseButtons().Contains(TakenButton));
			if (!bHeld)
			{
				ReleaseHand();
			}
			else
			{
				MouseX = Viewport->GetMouseX();
				MouseY = Viewport->GetMouseY();
			}
		}
		if (Hand == EHand::Press)
		{
			UpdatePressBall();
		}
		else if (Hand == EHand::Grab)
		{
			UpdateGrab(DeltaSeconds);
		}
		else if (Hand == EHand::Skin)
		{
			UpdateSkinGrab();
		}
		UpdateHover();
		// 에디터가 뒤에 있으면 엔진이 실시간 그리기를 끈다(편집기 설정 Use Less CPU when in Background) — 보이지 않는 시뮬레이션도 같이 멈춘다.
		UWorld* World = Scene.GetWorld();
		if (World && IsRealtime())
		{
			World->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
	{
		FEditorViewportClient::Draw(View, PDI);
		++DrawCount;
		// 살 속에 들어간 부분도 보이게 맨 앞에.
		if (Hand == EHand::Press)
		{
			const FTransform& Tip = PressBall->GetComponentTransform();
			DrawWireCapsule(PDI, Tip.GetLocation(), Tip.GetUnitAxis(EAxis::X), Tip.GetUnitAxis(EAxis::Y), Tip.GetUnitAxis(EAxis::Z), FLinearColor(1.f, 0.45f, 0.1f),
				PressBall->GetUnscaledCapsuleRadius(), PressBall->GetUnscaledCapsuleHalfHeight(), 16, SDPG_Foreground);
		}
		else if (Hand == EHand::Grab && MeshComponent.IsValid())
		{
			const FVector Grabbed = MeshComponent->GetComponentTransform().TransformPosition(GrabLocal);
			const double Mark = 0.01 * FVector::Dist(Grabbed, View->ViewMatrices.GetViewOrigin());
			PDI->DrawLine(Grabbed, GrabTarget, FLinearColor(1.f, 0.45f, 0.1f), SDPG_Foreground);
			DrawWireSphere(PDI, Grabbed, FLinearColor(1.f, 0.45f, 0.1f), Mark, 12, SDPG_Foreground);
		}
		else if (Hand == EHand::Skin && SkinGrabber)
		{
			// 붙든 원(잡은 점의 지금 피부 자리, GPU가 아직 안 읽었으면 처음 맞은 자리)과 끄는 자리까지 줄. 못 잡았거나 놓쳤으면 회색.
			USoftbodyGrabberComponent::FGrabState State;
			const bool bState = SkinGrabber->IsGrabbing() && SkinGrabber->GetGrabState(State) && State.bHeld;
			const FVector Centre = bState ? State.GrabbedPoint : GrabAnchor;
			const FLinearColor Color = SkinGrabber->IsGrabbing() ? FLinearColor(1.f, 0.45f, 0.1f) : FLinearColor(0.45f, 0.45f, 0.45f);
			FVector AxisX;
			FVector AxisY;
			SkinNormal.FindBestAxisVectors(AxisX, AxisY);
			DrawCircle(PDI, Centre, AxisX, AxisY, Color, SkinRadius, 48, SDPG_Foreground);
			if (SkinGrabber->IsGrabbing())
			{
				const FVector Target = SkinGrabber->GetComponentLocation();
				PDI->DrawLine(Centre, Target, Color, SDPG_Foreground);
				DrawWireSphere(PDI, Target, Color, 0.25 * SkinRadius, 12, SDPG_Foreground);
			}
		}
		else if (Hand == EHand::None && bHoverValid)
		{
			// Ctrl을 누른 동안 커서 아래 겉에 잡을 원(Grab Radius, 월드 cm — 가까이서 보면 화면에서 크다). 앞 잡기를 못 했으면 회색.
			FVector AxisX;
			FVector AxisY;
			HoverNormal.FindBestAxisVectors(AxisX, AxisY);
			const FLinearColor Color = SkinFailure.IsEmpty() ? FLinearColor(1.f, 0.45f, 0.1f) : FLinearColor(0.45f, 0.45f, 0.45f);
			DrawCircle(PDI, HoverPoint, AxisX, AxisY, Color, GrabRadiusNow(), 48, SDPG_Foreground);
		}
	}

	/**
	 * 마우스(피직스 에셋 에디터와 같은 손): Ctrl + 좌클릭 끌기 = 공으로 누르기, Ctrl + 우클릭 끌기 = 원 안 살갗 잡기(ADR-0041), Ctrl + Shift + 우클릭 끌기 = 메시 전체 끌기, 누른 채 휠 = 깊이·거리.
	 * 그냥 클릭은 메시를 고르고(기즈모가 나온다) 빈 곳 클릭은 푼다(ProcessClick). 엔진의 "Ctrl + 끌기 = 기즈모 축 끌기"는 이 뷰포트에서 쓰지 않는다.
	 */
	virtual bool InputKey(const FInputKeyEventArgs& Args) override
	{
		const bool bWheel = Args.Key == EKeys::MouseScrollUp || Args.Key == EKeys::MouseScrollDown;
		if (TakenButton.IsValid())
		{
			// 잡은 동안: 그 단추를 놓으면 끝, 휠은 깊이·거리. 나머지(Ctrl을 먼저 떼는 것 등)는 엔진에 넘기지 않는다 — 넘기면 카메라나 기즈모 끌기가
			// 시작된다.
			if (Args.Key == TakenButton && Args.Event == IE_Released)
			{
				ReleaseHand();
			}
			else if (bWheel && Args.Event == IE_Pressed)
			{
				Wheel(Args.Key == EKeys::MouseScrollUp ? 1.f : -1.f);
			}
			return true;
		}
		// 잡을 원의 크기(Grab Radius, 월드 cm — ADR-0041 §3.10): [ 줄이고 ] 키운다 — 잡는 동안은 그대로. 이 뷰포트가 키보드 포커스를 가져야 오므로(디테일
		// 패널을 만진 뒤에는 안 온다) Ctrl을 누른 채 휠도 같은 일을 한다(아래).
		if ((Args.Key == EKeys::LeftBracket || Args.Key == EKeys::RightBracket) && (Args.Event == IE_Pressed || Args.Event == IE_Repeat))
		{
			StepGrabRadius(Args.Key == EKeys::RightBracket ? 1 : -1);
			return true;
		}
		// Ctrl을 누른 채 원이 메시 위에 보이면 휠 = 원 크기(위로 굴리면 크게). 휠은 키보드 포커스와 상관없이 커서 아래 뷰포트로 온다. 메시 밖이면 카메라 그대로.
		if (bWheel && Hand == EHand::None && bHoverValid && Args.Viewport && IsControlHeld(*Args.Viewport))
		{
			if (Args.Event == IE_Pressed)
			{
				StepGrabRadius(Args.Key == EKeys::MouseScrollUp ? 1 : -1);
			}
			return true;
		}
		const bool bMouseButton = Args.Key == EKeys::LeftMouseButton || Args.Key == EKeys::RightMouseButton;
		if (bMouseButton && (Args.Event == IE_Pressed || Args.Event == IE_DoubleClick) && Args.Viewport && IsControlHeld(*Args.Viewport))
		{
			// 메시를 비껴가도 단추는 이쪽이 쥔다(놓을 때까지 엔진에 넘기지 않음).
			if (bIsTracking)
			{
				AbortTracking();
			}
			ReleaseHand();
			TakenButton = Args.Key;
			MouseX = Args.Viewport->GetMouseX();
			MouseY = Args.Viewport->GetMouseY();
			// 우클릭: 그냥 Ctrl은 원 안 피부 잡기, Shift를 더하면 메시 전체 끌기(관성 출렁임 보기 — ADR-0041 §3.10).
			const EHand Wanted = Args.Key == EKeys::LeftMouseButton ? EHand::Press : (IsShiftHeld(*Args.Viewport) ? EHand::Grab : EHand::Skin);
			BeginHand(Wanted, MouseX, MouseY);
			return true;
		}
		// 포커스가 없던 뷰포트를 Ctrl + 클릭하면 엔진이 단추보다 Ctrl 키를 먼저 보낸다(FSceneViewport::ApplyModifierKeys). 단추가 눌린 채 오는 Ctrl을
		// 엔진에 넘기면 기즈모 축 끌기가 시작되므로 막는다.
		if ((Args.Key == EKeys::LeftControl || Args.Key == EKeys::RightControl) && Args.Viewport
			&& (Args.Viewport->KeyState(EKeys::LeftMouseButton) || Args.Viewport->KeyState(EKeys::RightMouseButton)))
		{
			return true;
		}
		if (Hand != EHand::None && bWheel)
		{
			if (Args.Event == IE_Pressed)
			{
				Wheel(Args.Key == EKeys::MouseScrollUp ? 1.f : -1.f);
			}
			return true;
		}
		return FEditorViewportClient::InputKey(Args);
	}

	virtual bool InputAxis(const FInputKeyEventArgs& Args) override
	{
		// 쥔 동안 마우스 움직임은 공·잡은 점이 따라간다 — 카메라가 돌지 않게 엔진 쪽에는 넘기지 않는다(커서도 숨지 않는다). 위치는 Tick이 읽는다.
		if (TakenButton.IsValid() && (Args.Key == EKeys::MouseX || Args.Key == EKeys::MouseY))
		{
			return true;
		}
		return FEditorViewportClient::InputAxis(Args);
	}

	/** 그냥 클릭: 메시 위면 고르고(기즈모), 빈 곳이면 푼다. 기즈모 손잡이를 끌지 않고 놓은 클릭은 그대로 둔다. */
	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override
	{
		if (Key == EKeys::LeftMouseButton)
		{
			if (!(HitProxy && HitProxy->IsA(HWidgetAxis::StaticGetType())))
			{
				UMeshComponent* Mesh = MeshComponent.Get();
				FVector Origin, Direction, ViewDirection;
				double T = 0.0;
				SetSelected(Mesh && CursorRay((int32)HitX, (int32)HitY, Origin, Direction, ViewDirection) && PressMesh.Raycast(*Mesh, Origin, Direction, T));
			}
			return;
		}
		FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
	}

	// 기즈모: 고른 메시를 옮기고 돌린다(관성 시험). 고르지 않았거나 누르고·잡는 동안은 없다.
	virtual UE::Widget::EWidgetMode GetWidgetMode() const override
	{
		return MeshComponent.IsValid() && bSelected && Hand == EHand::None ? WidgetMode : UE::Widget::WM_None;
	}
	virtual void SetWidgetMode(UE::Widget::EWidgetMode NewMode) override
	{
		if (CanSetWidgetMode(NewMode))
		{
			WidgetMode = NewMode;
			Invalidate();
		}
	}
	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override
	{
		return NewMode == UE::Widget::WM_Translate || NewMode == UE::Widget::WM_Rotate;
	}
	virtual FVector GetWidgetLocation() const override
	{
		return MeshComponent.IsValid() ? MeshComponent->GetComponentLocation() : FVector::ZeroVector;
	}
	virtual FMatrix GetWidgetCoordSystem() const override
	{
		return FMatrix::Identity;
	}
	virtual void TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override
	{
		bManipulating = bIsDraggingWidget;
	}
	virtual void TrackingStopped() override
	{
		bManipulating = false;
	}
	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale) override
	{
		USceneComponent* Root = MeshComponent.Get();
		if (!bManipulating || CurrentAxis == EAxisList::None || !Root)
		{
			return FEditorViewportClient::InputWidgetDelta(InViewport, CurrentAxis, Drag, Rot, Scale);
		}
		if (!Drag.IsZero())
		{
			Root->AddWorldOffset(Drag);
		}
		if (!Rot.IsZero())
		{
			Root->SetWorldRotation(Rot.Quaternion() * Root->GetComponentQuat());
		}
		return true;
	}

private:
	/** 시뮬레이션을 다시 세울 때: 대상 메시·마지막 굽기·실행 버튼·메시 슬롯 머티리얼(Create Softbody Material이 바꾼다). */
	static uint32 SetupKey(const USoftbodyAuthoringData& Data)
	{
		const UObject* Target = Data.TargetMesh.Get();
		uint32 Key = HashCombine(GetTypeHash(Target), GetTypeHash(Data.BakedData.Get()));
		Key = HashCombine(Key, GetTypeHash(Data.GetRunSerial()));
		for (const UMaterialInterface* Material : SoftbodyAuthoringEditorLocal::AssetMaterials(Target))
		{
			Key = HashCombine(Key, GetTypeHash(Material));
		}
		return Key == 0 ? 1u : Key;
	}

	void Rebuild(USoftbodyAuthoringData* Data, uint32 Key)
	{
		BuiltKey = Key;
		bRestart = false;
		ReleaseHand();
		StoreValues();
		DestroyActor();
		bProblem = true;
		UObject* Target = Data ? Data->TargetMesh.Get() : nullptr;
		USoftbodyMeshData* Baked = Data ? Data->BakedData.Get() : nullptr;
		const UObject* BakedFor = Baked
			? (Baked->SourceSkeletalMesh ? (const UObject*)Baked->SourceSkeletalMesh.Get() : (const UObject*)Baked->SourceMesh.Get())
			: nullptr;
		if (!Target)
		{
			Status = LOCTEXT("SimNoTarget", "Pick a Target Mesh.");
		}
		else if (!BakedFor)
		{
			Status = LOCTEXT("SimNotBaked", "Not baked yet. Press Run Bake to simulate this mesh.");
		}
		else if (BakedFor != Target)
		{
			Status = FText::Format(LOCTEXT("SimOtherMesh", "The last bake is for {0}. Press Run Bake for this Target Mesh."),
				FText::FromString(BakedFor->GetName()));
		}
		else
		{
			SpawnActor(Target, *Baked);
		}
		if (OnSoftbodyChanged)
		{
			OnSoftbodyChanged(Softbody.Get());
		}
		Invalidate();
	}

	void SpawnActor(UObject* Target, USoftbodyMeshData& Baked)
	{
		UWorld* World = Scene.GetWorld();
		if (!World)
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags = RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		if (!NewActor)
		{
			return;
		}
		UMeshComponent* Mesh = nullptr;
		USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Target);
		if (UStaticMesh* Static = Cast<UStaticMesh>(Target))
		{
			UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(NewActor, TEXT("Mesh"), RF_Transient);
			Component->SetStaticMesh(Static);
			Mesh = Component;
		}
		else if (Skeletal)
		{
			USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(NewActor, TEXT("Mesh"), RF_Transient);
			Component->SetSkeletalMeshAsset(Skeletal);
			Mesh = Component;
		}
		if (!Mesh)
		{
			NewActor->Destroy(false, false);
			return;
		}
		NewActor->SetRootComponent(Mesh);
		NewActor->AddInstanceComponent(Mesh);
		// 그냥 클릭으로 고르면 레벨처럼 윤곽을 그린다.
		const TSharedRef<bool> Outline = SelectedOutline;
		Mesh->SelectionOverrideDelegate = UPrimitiveComponent::FSelectionOverride::CreateLambda([Outline](const UPrimitiveComponent*) { return *Outline; });
		Mesh->RegisterComponent();

		USoftbodyComponent* Body = NewObject<USoftbodyComponent>(NewActor, TEXT("Softbody"), RF_Transient);
		if (const USoftbodyComponent* Kept = Values.Get())
		{
			SoftbodyAuthoringEditorLocal::CopySimulationValues(*Kept, *Body);
		}
		Body->MeshData = &Baked;
		Body->TargetMeshComponent = Mesh;
		NewActor->AddInstanceComponent(Body);
		Body->RegisterComponent();
		// 게임처럼 시작한다: 컴포넌트의 BeginPlay가 실행 상태를 만들고 틱을 켠다.
		NewActor->DispatchBeginPlay();

		Actor = NewActor;
		MeshComponent = Mesh;
		Softbody = Body;
		PressMesh.Build(SoftbodyAuthoringEditorLocal::LodZero(Target), Target, Skeletal != nullptr);
		AppliedAnimation = nullptr;
		bAnimationApplied = false;
		bPlaying = false;
		if (Settings.IsValid())
		{
			Settings->bSkeletal = Skeletal != nullptr;
			Settings->Skeleton = Skeletal ? Skeletal->GetSkeleton() : nullptr;
		}

		int32 Compatible = 0;
		for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
		{
			Compatible += SoftbodyAuthoringEditorLocal::HasSoftbodyMaterial(Mesh->GetMaterial(Slot)) ? 1 : 0;
		}
		if (Compatible == 0)
		{
			Status = LOCTEXT("SimNoMaterial", "No material slot has the Softbody material, so the mesh cannot move. See 5. Material on the left.");
		}
		else if (!Baked.HasSurfaceCarrier())
		{
			Status = LOCTEXT("SimNoContactMap", "The last bake has no contact map, so pressing does nothing. Turn on Bake Contact Map and press Run Bake.");
		}
		else
		{
			Status = LOCTEXT("SimRunning", "The last bake, running as in a game. Run Bake restarts it.");
			bProblem = false;
		}

		// 처음 한 번만 화면에 맞추고 바닥을 메시 밑에 둔다.
		if (!bFramed)
		{
			const FBox Box = Mesh->Bounds.GetBox();
			if (Box.IsValid)
			{
				FocusViewportOnBox(Box, true);
				Scene.SetFloorOffset((float)-Box.Min.Z);
				bFramed = true;
			}
		}
	}

	void DestroyActor()
	{
		if (AActor* Existing = Actor.Get())
		{
			Existing->Destroy(false, false);
		}
		Actor = nullptr;
		MeshComponent = nullptr;
		Softbody = nullptr;
		PressMesh = SoftbodyAuthoringEditorLocal::FPressMesh();
	}

	void StoreValues()
	{
		if (USoftbodyComponent* Kept = Values.Get())
		{
			if (const USoftbodyComponent* Live = Softbody.Get())
			{
				SoftbodyAuthoringEditorLocal::CopySimulationValues(*Live, *Kept);
			}
		}
	}

	void ApplyAnimation()
	{
		USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(MeshComponent.Get());
		UAnimationAsset* Wanted = Settings.IsValid() ? Settings->Animation.Get() : nullptr;
		if (!Skeletal || (bAnimationApplied && Wanted == AppliedAnimation.Get()))
		{
			return;
		}
		AppliedAnimation = Wanted;
		bAnimationApplied = true;
		const USkeleton* Skeleton = Skeletal->GetSkeletalMeshAsset() ? Skeletal->GetSkeletalMeshAsset()->GetSkeleton() : nullptr;
		const bool bFits = Wanted && Skeleton && Skeleton->IsCompatibleForEditor(Wanted->GetSkeleton());
		bPlaying = bFits;
		// 없으면 기준 자세. 처음(아무것도 고르지 않음)에는 메시 그대로 둔다.
		if (bFits)
		{
			Skeletal->PlayAnimation(Wanted, true);
		}
		else if (Skeletal->GetAnimationMode() == EAnimationMode::AnimationSingleNode)
		{
			Skeletal->SetAnimation(nullptr);
		}
	}

	bool CursorRay(int32 X, int32 Y, FVector& OutOrigin, FVector& OutDirection, FVector& OutViewDirection)
	{
		if (!Viewport)
		{
			return false;
		}
		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(Viewport, GetScene(), EngineShowFlags));
		FSceneView* View = CalcSceneView(&ViewFamily);
		if (!View)
		{
			return false;
		}
		const FViewportCursorLocation Cursor(View, this, X, Y);
		OutOrigin = Cursor.GetOrigin();
		OutDirection = Cursor.GetDirection();
		OutViewDirection = View->GetViewDirection();
		return true;
	}

	static bool IsControlHeld(const FViewport& InViewport)
	{
		// 뷰포트 키 상태는 마우스 이벤트의 보조 키로 채워진다(포커스가 없던 첫 클릭도). 운영체제 상태는 덤.
		return InViewport.KeyState(EKeys::LeftControl) || InViewport.KeyState(EKeys::RightControl)
			|| FSlateApplication::Get().GetModifierKeys().IsControlDown();
	}

	static bool IsShiftHeld(const FViewport& InViewport)
	{
		return InViewport.KeyState(EKeys::LeftShift) || InViewport.KeyState(EKeys::RightShift)
			|| FSlateApplication::Get().GetModifierKeys().IsShiftDown();
	}

	/** 잡을 원 반지름(월드 cm — 미리보기 설정 Grab Radius, 게임 Grabber의 Grab Radius와 같은 값). */
	float GrabRadiusNow() const
	{
		return Settings.IsValid() ? FMath::Max(Settings->GrabRadius, 0.2f) : 2.f;
	}

	/** 잡을 원 반지름을 한 칸 키우거나 줄인다(고른 값 사다리 — 도움말의 cm가 깔끔하게). 디테일 패널의 Grab Radius가 그 값을 보인다. */
	void StepGrabRadius(int32 Direction)
	{
		if (!Settings.IsValid())
		{
			return;
		}
		static const float Steps[] = { 0.2f, 0.3f, 0.5f, 0.7f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 4.f, 5.f, 6.f, 8.f, 10.f, 12.f, 15.f, 20.f };
		const float Now = Settings->GrabRadius;
		float Next = Now;
		if (Direction > 0)
		{
			for (const float Step : Steps)
			{
				if (Step > Now * 1.001f)
				{
					Next = Step;
					break;
				}
			}
		}
		else
		{
			for (int32 Index = UE_ARRAY_COUNT(Steps) - 1; Index >= 0; --Index)
			{
				if (Steps[Index] < Now * 0.999f)
				{
					Next = Steps[Index];
					break;
				}
			}
		}
		Settings->GrabRadius = Next;
		Invalidate();
	}

	/**
	 * Ctrl을 누른 동안(손이 없을 때) 커서 아래 겉의 잡을 원(ADR-0041 §3.10): 커서가 움직이거나 애니메이션이 돌 때만 다시 쏜다. 원 반지름 = Grab Radius(월드
	 * cm — 가까이·멀리서 같은 넓이). Ctrl을 떼면 지운다.
	 */
	void UpdateHover()
	{
		const bool bWanted = Hand == EHand::None && Viewport && IsControlHeld(*Viewport) && MeshComponent.IsValid();
		if (!bWanted)
		{
			if (bHoverValid)
			{
				bHoverValid = false;
				Invalidate();
			}
			HoverMouseX = -1;
			return;
		}
		const int32 X = Viewport->GetMouseX();
		const int32 Y = Viewport->GetMouseY();
		if (X == HoverMouseX && Y == HoverMouseY && !bPlaying)
		{
			return;
		}
		if (!SkinFailure.IsEmpty() && FMath::Square(X - SkinMouseX) + FMath::Square(Y - SkinMouseY) > 64)
		{
			SkinFailure = FText::GetEmpty();   // 못 잡은 자리에서 8픽셀 넘게 비키면 앞 실패는 지운다
		}
		HoverMouseX = X;
		HoverMouseY = Y;
		FVector Origin, Direction, ViewDirection;
		SoftbodyAuthoringEditorLocal::FPressMesh::FHit Surface;
		bHoverValid = false;
		if (CursorRay(X, Y, Origin, Direction, ViewDirection))
		{
			PressMesh.Pose(*MeshComponent.Get(), bPlaying);
			if (PressMesh.Trace(Origin, Direction, 0.0, INDEX_NONE, false, Surface))
			{
				HoverPoint = PressMesh.Point(Surface);
				HoverNormal = PressMesh.Normal(Surface);
				if ((HoverNormal | (HoverPoint - GetViewLocation())) > 0.0)
				{
					HoverNormal = -HoverNormal;
				}
				bHoverValid = true;
			}
		}
		Invalidate();
	}

	/**
	 * 피부 잡기(ADR-0041): 끄는 자리 = 잡은 점(애니메이션이 돌면 그 자리를 따라간다 — 맞은 삼각형을 다시 놓는다) + 커서가 처음 자리에서 옮긴 만큼(잡은 점을
	 * 지나고 시선에 수직인 평면에서) + 휠로 민 거리(시선 앞뒤). Grabber를 그 자리에 둔다 — Softbody 컴포넌트가 틱마다 읽는다. 잡기가 끝났으면(못 잡음·대상 사라짐)
	 * 까닭을 도움말에.
	 */
	void UpdateSkinGrab()
	{
		UMeshComponent* Mesh = MeshComponent.Get();
		if (!SkinGrabber || !Mesh)
		{
			return;
		}
		if (!SkinGrabber->IsGrabbing())
		{
			if (SkinFailure.IsEmpty() && !SkinGrabber->GetLastFailure().IsEmpty())
			{
				SkinFailure = FText::FromString(SkinGrabber->GetLastFailure());
			}
			return;
		}
		if (bPlaying)
		{
			PressMesh.Pose(*Mesh, true);
			GrabAnchor = PressMesh.Point(SkinHit);
		}
		FVector Origin, Direction, ViewDirection;
		FVector StartOrigin, StartDirection, StartViewDirection;
		if (!CursorRay(MouseX, MouseY, Origin, Direction, ViewDirection) || !CursorRay(SkinMouseX, SkinMouseY, StartOrigin, StartDirection, StartViewDirection))
		{
			return;
		}
		const auto OnPlane = [this, &ViewDirection](const FVector& RayOrigin, const FVector& RayDirection)
		{
			const double Facing = RayDirection | ViewDirection;
			return FMath::Abs(Facing) > 1e-6 ? RayOrigin + RayDirection * (((GrabAnchor - RayOrigin) | ViewDirection) / Facing) : GrabAnchor;
		};
		const FVector Target = GrabAnchor + (OnPlane(Origin, Direction) - OnPlane(StartOrigin, StartDirection)) + ViewDirection * SkinPush;
		SkinGrabber->SetWorldLocation(Target);
	}

	/** 처음 닿는 점은 메시 설명 삼각형에 광선(스켈레탈은 지금 자세). 비껴가면 아무것도 하지 않는다. */
	void BeginHand(EHand NewHand, int32 X, int32 Y)
	{
		UMeshComponent* Mesh = MeshComponent.Get();
		FVector Origin, Direction, ViewDirection;
		SoftbodyAuthoringEditorLocal::FPressMesh::FHit Surface;
		if (!Mesh || !CursorRay(X, Y, Origin, Direction, ViewDirection))
		{
			return;
		}
		PressMesh.Pose(*Mesh, true);
		if (!PressMesh.Trace(Origin, Direction, 0.0, INDEX_NONE, false, Surface))
		{
			return;
		}
		const FVector Hit = PressMesh.Point(Surface);
		MouseX = X;
		MouseY = Y;
		Hand = NewHand;
		if (Hand == EHand::Press)
		{
			PressHit = Surface;
			PressMouseX = X;
			PressMouseY = Y;
			MeasurePress();
			UpdatePressBall();
			PressBall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		}
		else if (Hand == EHand::Skin)
		{
			// 원 반지름 = Grab Radius(월드 cm). Grabber를 맞은 자리에 두고 이 메시를 잡는다 — 잡은 칸은 GPU가 다음 갱신에 고른다(못 찾으면 회색 + 까닭).
			SkinHit = Surface;
			SkinMouseX = X;
			SkinMouseY = Y;
			SkinPush = 0.0;
			GrabAnchor = Hit;
			SkinNormal = PressMesh.Normal(Surface);
			if ((SkinNormal | (Hit - GetViewLocation())) > 0.0)
			{
				SkinNormal = -SkinNormal;
			}
			SkinRadius = GrabRadiusNow();
			SkinFailure = FText::GetEmpty();
			if (SkinGrabber)
			{
				SkinGrabber->GrabRadius = SkinRadius;
				SkinGrabber->SetWorldLocation(Hit);
				// 렌더 메시 위를 맞힌 점이라 찾는 거리는 1 cm(눈·머리카락을 눌러 옆 피부를 잡지 않는다 — 게임의 Grab At Location은 5 cm).
				if (!SkinGrabber->GrabAtLocationWithin(Hit, Softbody.Get(), 1.f))
				{
					SkinFailure = FText::FromString(SkinGrabber->GetLastFailure());
				}
			}
		}
		else
		{
			GrabAnchor = Hit;
			GrabTarget = Hit;
			GrabLocal = Mesh->GetComponentTransform().InverseTransformPosition(Hit);
			GrabPush = 0.0;
			GrabVelocity = FVector::ZeroVector;
		}
		Invalidate();
	}

	/** 단추를 놓았을 때: 누르던 손가락을 치우고, 끌던 메시는 그 자리에 둔다(속도는 버린다 — 멈춘 순간의 출렁임을 본다). */
	void ReleaseHand()
	{
		TakenButton = FKey();
		if (Hand == EHand::Press)
		{
			PressBall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		if (Hand == EHand::Skin && SkinGrabber && SkinGrabber->IsGrabbing())
		{
			SkinGrabber->Release();
		}
		Hand = EHand::None;
		HoverMouseX = -1;
		PressHit = SoftbodyAuthoringEditorLocal::FPressMesh::FHit();
		Invalidate();
	}

	/**
	 * 누른 채 휠: 누르기는 깊이 ±0.25 cm, 잡기는 시선을 따라 멀리·가까이(잡은 점까지 거리의 5%). 위로 굴리면 화면 안쪽.
	 * 깊이는 지금 들어간 깊이에서 센다 — 반대편 앞에서 멈춘 자리에서 더 굴려도 쌓이지 않고, 얕게 굴리면 바로 얕아진다.
	 */
	void Wheel(float Sign)
	{
		if (Hand == EHand::Press && Settings.IsValid())
		{
			Settings->PressDepth = FMath::Clamp(PressDepthNow() + 0.25f * Sign, 0.f, 30.f);
		}
		else if (Hand == EHand::Grab)
		{
			const double Distance = FVector::Dist(GrabAnchor + GetViewRotation().Vector() * GrabPush, GetViewLocation());
			GrabPush += Sign * FMath::Max(0.5, 0.05 * Distance);
		}
		else if (Hand == EHand::Skin)
		{
			// 피부 잡기는 누르기 깊이와 같은 간격(0.25 cm) — 메시 끌기의 "거리의 5 %"는 30 cm에서 1.5 cm라 한 칸에 볼 한도를 넘는다.
			SkinPush += 0.25 * Sign;
		}
	}

	void SetSelected(bool bNewSelected)
	{
		if (bSelected == bNewSelected)
		{
			return;
		}
		bSelected = bNewSelected;
		*SelectedOutline = bSelected;
		if (UMeshComponent* Mesh = MeshComponent.Get())
		{
			Mesh->PushSelectionToProxy();
		}
		Invalidate();
	}

	/**
	 * 잡아 끌기: 잡은 점(메시에 붙은 점)이 커서를 따라간다 — 처음 잡은 점을 지나고 시선에 수직인 평면에서(휠로 앞뒤). 피직스 에셋 에디터의 잡기처럼
	 * 용수철로 끈다: 임계 감쇠, 약 0.2초에 따라붙는다. 마우스가 멈추면 메시도 멈추고 그때 출렁인다. 메시는 돌지 않는다.
	 */
	void UpdateGrab(float DeltaSeconds)
	{
		USceneComponent* Root = MeshComponent.Get();
		FVector Origin, Direction, ViewDirection;
		if (!Root || !CursorRay(MouseX, MouseY, Origin, Direction, ViewDirection))
		{
			return;
		}
		const double Facing = Direction | ViewDirection;
		if (FMath::Abs(Facing) > 1e-6)
		{
			const FVector PlanePoint = GrabAnchor + ViewDirection * GrabPush;
			GrabTarget = Origin + Direction * (((PlanePoint - Origin) | ViewDirection) / Facing);
		}
		// 임계 감쇠 용수철의 닫힌 해(한 프레임 안에서 목표는 그대로): 프레임이 길어도 넘치지 않는다.
		constexpr double Rate = 25.0;
		const double Dt = FMath::Min((double)DeltaSeconds, 0.1);
		const FVector Grabbed = Root->GetComponentTransform().TransformPosition(GrabLocal);
		const FVector E0 = Grabbed - GrabTarget;
		const FVector C = GrabVelocity + Rate * E0;
		const double Decay = FMath::Exp(-Rate * Dt);
		const FVector E1 = (E0 + C * Dt) * Decay;
		GrabVelocity = (GrabVelocity - Rate * C * Dt) * Decay;
		Root->AddWorldOffset(E1 - E0);
	}

	/**
	 * 손가락 자리: 둥근 끝이 커서 아래 겉(눌리기 전, 지금 자세)에 붙고, 끝 앞쪽이 그 자리의 노멀을 따라 Press Depth만큼 들어간다.
	 * 커서가 움직일 때만 다시 쏜다 — 멈춘 커서 아래에서 애니메이션이 돌면 공은 맞은 살 자리를 따라간다. 커서가 메시를 벗어나면 마지막 자리에 남는다.
	 */
	void UpdatePressBall()
	{
		UMeshComponent* Mesh = MeshComponent.Get();
		if (!Mesh || PressHit.Triangle == INDEX_NONE)
		{
			return;
		}
		const bool bMoved = MouseX != PressMouseX || MouseY != PressMouseY;
		if (bMoved || bPlaying)
		{
			const double Start = FPlatformTime::Seconds();
			PressMesh.Pose(*Mesh, bPlaying);
			FVector Origin, Direction, ViewDirection;
			SoftbodyAuthoringEditorLocal::FPressMesh::FHit Surface;
			if (bMoved && CursorRay(MouseX, MouseY, Origin, Direction, ViewDirection) && PressMesh.Trace(Origin, Direction, 0.0, INDEX_NONE, false, Surface))
			{
				PressHit = Surface;
			}
			PressMouseX = MouseX;
			PressMouseY = MouseY;
			MeasurePress();
			PressMeasureMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		}
		// 손가락: 둥근 끝(Press Ball Radius)이 노멀을 따라 깊이만큼 들어가고, 몸통은 겉 밖(끝 반지름 두 배 높이)까지 이어진다. 공 하나면
		// 지름보다 깊이 눌렀을 때 통째로 살 속에 묻혀 겉과 닿지 않는다 — 겉은 돌아오고 반대편 살 가까운 곳만 들어갔다(일지 238).
		const float Radius = Settings.IsValid() ? FMath::Max(Settings->PressRadius, 0.2f) : 1.f;
		const FVector TipCenter = PressPoint - PressNormal * (PressDepthNow() - Radius);
		const FVector TopCenter = PressPoint + PressNormal * Radius;
		const float HalfHeight = (float)(0.5 * FVector::Dist(TipCenter, TopCenter)) + Radius;
		if (!FMath::IsNearlyEqual(PressBall->GetUnscaledCapsuleRadius(), Radius) || !FMath::IsNearlyEqual(PressBall->GetUnscaledCapsuleHalfHeight(), HalfHeight))
		{
			PressBall->SetCapsuleSize(Radius, HalfHeight);
		}
		PressBall->SetWorldLocationAndRotation(0.5 * (TipCenter + TopCenter), FRotationMatrix::MakeFromZ(PressNormal).ToQuat());
	}

	/**
	 * 맞은 자리의 점·바깥 노멀과 반대편까지 남은 깊이. 노멀은 보는 쪽으로(양면 광선이 뒷면을 맞혀도 화면 쪽에서 누른다).
	 * 반대편 = 그 점에서 노멀 반대로 쏜 광선이 처음 겉 밖으로 나가는 면 — 겹친 옷 층처럼 들어가는 면은 지나친다. 끝 앞쪽은 그 두께의 90%까지만
	 * 들어가 반대편 살에 닿거나 뚫고 나가지 않는다. 열린 메시(반대편이 없음)는 Press Depth 그대로.
	 * 나간 면 바로 뒤(LayerGap 안)에 들어가는 면이 있으면 그 층은 밑 살에 얹힌 껍질이라(두께 있는 장식·하드 서피스 판) 다음 나가는 면까지
	 * 잰다 — 0.2 cm 껍질 뒷면을 반대편으로 보면 손가락이 거의 들어가지 않았다(일지 345).
	 */
	void MeasurePress()
	{
		PressPoint = PressMesh.Point(PressHit);
		PressNormal = PressMesh.Normal(PressHit);
		if ((PressNormal | (PressPoint - GetViewLocation())) > 0.0)
		{
			PressNormal = -PressNormal;
		}
		using FHit = SoftbodyAuthoringEditorLocal::FPressMesh::FHit;
		constexpr double LayerGap = 1.0;   // cm: 얹힌 껍질과 밑 살 사이로 볼 틈
		const FVector Down = -PressNormal;
		constexpr double Deepest = 30.0;   // cm: Press Depth의 상한(휠도 여기까지) — 그만큼 들어갈 수 있으면 더 찾지 않는다
		FVector From = PressPoint;
		int32 Skip = PressHit.Triangle;
		double Before = 0.0;   // From까지 온 깊이
		PressRoom = TNumericLimits<double>::Max();
		for (int32 Layer = 0; Layer < 8; ++Layer)
		{
			FHit Far;
			if (!PressMesh.Trace(From, Down, 1e-3, Skip, true, Far))
			{
				break;
			}
			const double Exit = Before + Far.T;
			PressRoom = 0.9 * Exit;
			if (PressRoom >= Deepest)
			{
				break;
			}
			const FVector ExitPoint = From + Down * Far.T;
			FHit Next;
			if (!PressMesh.Trace(ExitPoint, Down, 1e-3, Far.Triangle, false, Next) || Next.T > LayerGap || PressMesh.Exits(Next, Down))
			{
				break;
			}
			Before = Exit + Next.T;
			From = ExitPoint + Down * Next.T;
			Skip = Next.Triangle;
		}
	}

	/** 지금 들어간 깊이: Press Depth, 반대편이 가까우면 그 앞까지. */
	float PressDepthNow() const
	{
		const float Wanted = Settings.IsValid() ? Settings->PressDepth : 1.f;
		return (float)FMath::Min((double)Wanted, PressRoom);
	}

	FAdvancedPreviewScene& Scene;
	TWeakObjectPtr<USoftbodyAuthoringData> Asset;
	TWeakObjectPtr<USoftbodySimulationPreviewSettings> Settings;
	/** 창이 쥔 값 보관(탭을 닫았다 열어도 남는다). */
	TWeakObjectPtr<USoftbodyComponent> Values;

	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UMeshComponent> MeshComponent;
	TWeakObjectPtr<USoftbodyComponent> Softbody;
	// 미리보기 장면이 참조를 쥔다.
	UCapsuleComponent* PressBall = nullptr;
	SoftbodyAuthoringEditorLocal::FPressMesh PressMesh;

	uint32 BuiltKey = 0;
	bool bRestart = false;
	bool bFramed = false;
	FText Status;
	bool bProblem = true;

	TWeakObjectPtr<UAnimationAsset> AppliedAnimation;
	bool bAnimationApplied = false;

	UE::Widget::EWidgetMode WidgetMode = UE::Widget::WM_Translate;
	bool bManipulating = false;
	/** 그냥 클릭으로 고른 메시(기즈모·선택 윤곽). 윤곽 델리게이트가 이 값을 본다. */
	bool bSelected = false;
	TSharedRef<bool> SelectedOutline = MakeShared<bool>(false);

	/** Ctrl + 클릭으로 쥔 손: 누르기(좌) 또는 잡기(우). TakenButton은 메시를 비껴가도 놓을 때까지 쥔다. */
	EHand Hand = EHand::None;
	FKey TakenButton;
	bool bSyntheticInput = false;
	/** 누르는 손가락이 붙은 겉 자리와 그것을 정한 커서(움직이면 다시 쏜다). */
	SoftbodyAuthoringEditorLocal::FPressMesh::FHit PressHit;
	int32 PressMouseX = -1;
	int32 PressMouseY = -1;
	FVector PressPoint = FVector::ZeroVector;
	FVector PressNormal = FVector::UpVector;
	/** 반대편 앞까지 들어갈 수 있는 깊이(cm, 없으면 끝없음). */
	double PressRoom = TNumericLimits<double>::Max();
	/** 개발: 마지막으로 다시 잰 데 든 시간(Softbody.Simulation state). */
	double PressMeasureMs = 0.0;
	/** 스켈레탈이 애니메이션을 돌리는 중(자세가 매 틱 바뀐다). */
	bool bPlaying = false;
	FVector GrabAnchor = FVector::ZeroVector;
	FVector GrabTarget = FVector::ZeroVector;
	FVector GrabLocal = FVector::ZeroVector;
	FVector GrabVelocity = FVector::ZeroVector;
	double GrabPush = 0.0;
	// 피부 잡기(ADR-0041): 장면의 Grabber(미리보기 장면이 참조를 쥔다), 맞은 겉 자리·노멀, 잡기를 시작한 커서, 휠로 민 거리(cm, 시선 앞이 +), 이 잡기의 원
	// 반지름(cm — 잡을 때의 Grab Radius), 커서 아래 원(Ctrl을 누른 동안), 마지막 실패 까닭(회색 원·도움말).
	USoftbodyGrabberComponent* SkinGrabber = nullptr;
	SoftbodyAuthoringEditorLocal::FPressMesh::FHit SkinHit;
	FVector SkinNormal = FVector::UpVector;
	int32 SkinMouseX = 0;
	int32 SkinMouseY = 0;
	double SkinPush = 0.0;
	float SkinRadius = 2.f;
	bool bHoverValid = false;
	FVector HoverPoint = FVector::ZeroVector;
	FVector HoverNormal = FVector::UpVector;
	int32 HoverMouseX = -1;
	int32 HoverMouseY = -1;
	FText SkinFailure;
	int32 MouseX = 0;
	int32 MouseY = 0;
	// 개발: Softbody.Simulation state.
	int32 TickCount = 0;
	int32 DrawCount = 0;
};

/** Simulation 탭의 뷰포트: 미리보기 장면을 갖고, 왼쪽 아래에 상태(또는 문제)와 조작 안내를 띄운다. */
class SSoftbodySimulationViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SSoftbodySimulationViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args, USoftbodyAuthoringData* InAsset, USoftbodySimulationPreviewSettings* InSettings, USoftbodyComponent* InValues)
	{
		Asset = InAsset;
		Settings = InSettings;
		Values = InValues;
		{
			// 이 월드에만 표면 장 묶음을 둔다(다른 미리보기 월드에는 두지 않는다). 미리보기 월드의 객체가 되돌리기 기록에 들어가지 않게.
			USoftbodySurfaceBatch::FPreviewWorldScope AllowSurfaceBatch;
			PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues().SetTransactional(false));
		}
		SEditorViewport::Construct(SEditorViewport::FArguments());
		LiveSimulations().RemoveAll([](const TWeakPtr<SSoftbodySimulationViewport>& Weak) { return !Weak.IsValid(); });
		LiveSimulations().Add(StaticCastSharedRef<SSoftbodySimulationViewport>(AsShared()));
	}

	virtual ~SSoftbodySimulationViewport() override
	{
		// 장면(멤버)이 사라지기 전에 액터를 지운다 — 컴포넌트가 끝내기(EndPlay·등록 해제)로 표면 장을 놓는다.
		if (Client.IsValid())
		{
			Client->Teardown();
		}
		// 지운 월드의 객체(컴포넌트·표면 장·돌려준 텍스처 벌)를 다음 프레임에 거둔다 — 닫으면 메모리도 바로 돌아온다. GC를 기다리게 두면 탭을 닫고
		// 곧 에디터를 끌 때 그 객체들이 종료 막바지까지 남아 종료가 접근 위반으로 끝났다(일지 234, 누른 뒤 닫았을 때만).
		if (GEngine)
		{
			GEngine->ForceGarbageCollection(true);
		}
	}

	/** 열린 Simulation 탭들(Softbody.Simulation이 다룬다). */
	static TArray<TWeakPtr<SSoftbodySimulationViewport>>& LiveSimulations()
	{
		static TArray<TWeakPtr<SSoftbodySimulationViewport>> Simulations;
		return Simulations;
	}

	TSharedPtr<FSoftbodySimulationClient> GetClient() const { return Client; }
	USoftbodySimulationPreviewSettings* GetSettings() const { return Settings.Get(); }

	/**
	 * 개발(Softbody.Simulation mouse): 마우스 단추·움직임을 실제 마우스와 같은 길(FSceneViewport → 클라이언트 InputKey)로 흉내 낸다.
	 * Fraction = 뷰포트 안 자리(0~1). 운영체제 커서는 움직이지 않는다.
	 */
	void InjectMouse(const FKey& Button, const FString& Phase, const FVector2D& Fraction, bool bCtrl)
	{
		if (!SceneViewport.IsValid() || !ViewportWidget.IsValid() || !Client.IsValid())
		{
			return;
		}
		Client->SetSyntheticInput(true);
		const FGeometry Geometry = ViewportWidget->GetTickSpaceGeometry();
		const FVector2D Size = Geometry.GetLocalSize();
		const FVector2D Position = Geometry.LocalToAbsolute(Fraction * Size);
		const FVector2D Last = bInjected ? LastInjected : Position;
		bInjected = true;
		LastInjected = Position;
		TSet<FKey> Pressed;
		if (Phase != TEXT("up"))
		{
			Pressed.Add(Button);
		}
		const FModifierKeysState Modifiers(false, false, bCtrl, false, false, false, false, false, false);
		const FPointerEvent Event(FSlateApplicationBase::CursorPointerIndex, Position, Last, Pressed, Phase == TEXT("move") ? FKey() : Button, 0.f, Modifiers);
		if (Phase == TEXT("down"))
		{
			SceneViewport->OnMouseButtonDown(Geometry, Event);
		}
		else if (Phase == TEXT("move"))
		{
			SceneViewport->OnMouseMove(Geometry, Event);
		}
		else if (Phase == TEXT("up"))
		{
			SceneViewport->OnMouseButtonUp(Geometry, Event);
		}
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
	{
		Client = MakeShared<FSoftbodySimulationClient>(*PreviewScene, SharedThis(this), Asset.Get(), Settings.Get(), Values.Get());
		return Client.ToSharedRef();
	}

	virtual TSharedPtr<SWidget> BuildViewportToolbar() override
	{
		const FName ToolbarName(TEXT("SoftbodyAuthoringEditor.SimulationToolbar"));
		if (!UToolMenus::Get()->IsMenuRegistered(ToolbarName))
		{
			UToolMenu* Menu = UToolMenus::Get()->RegisterMenu(ToolbarName, NAME_None, EMultiBoxType::SlimHorizontalToolBar);
			Menu->StyleName = "ViewportToolbar";
			FToolMenuSection& Left = Menu->AddSection("Left");
			Left.AddDynamicEntry("SoftbodySimulationRestart", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section)
			{
				UUnrealEdViewportToolbarContext* Context = Section.FindContext<UUnrealEdViewportToolbarContext>();
				if (!Context)
				{
					return;
				}
				const TWeakPtr<SSoftbodySimulationViewport> Viewport = StaticCastWeakPtr<SSoftbodySimulationViewport>(Context->Viewport);
				Section.AddEntry(FToolMenuEntry::InitToolBarButton(
					"SoftbodySimulationRestart",
					FUIAction(FExecuteAction::CreateLambda([Viewport]()
					{
						if (const TSharedPtr<SSoftbodySimulationViewport> Pinned = Viewport.Pin())
						{
							if (const TSharedPtr<FSoftbodySimulationClient> PinnedClient = Pinned->GetClient())
							{
								PinnedClient->Restart();
							}
						}
					})),
					LOCTEXT("SimRestart", "Restart"),
					LOCTEXT("SimRestartTip", "Starts the simulation over from rest and puts the mesh back where it started."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh")));
			}));
			FToolMenuSection& Right = Menu->AddSection("Right");
			Right.Alignment = EToolMenuSectionAlign::Last;
			Right.AddEntry(UE::UnrealEd::CreateCameraSubmenu(UE::UnrealEd::FViewportCameraMenuOptions().ShowAll()));
			const FName ViewParent(TEXT("UnrealEd.ViewportToolbar.View"));
			if (!UToolMenus::Get()->IsMenuRegistered(ViewParent))
			{
				UToolMenus::Get()->RegisterMenu(ViewParent);
			}
			UToolMenus::Get()->RegisterMenu("SoftbodyAuthoringEditor.SimulationToolbar.ViewModes", ViewParent);
			Right.AddEntry(UE::UnrealEd::CreateViewModesSubmenu());
			Right.AddEntry(UE::UnrealEd::CreateDefaultShowSubmenu());
		}
		FToolMenuContext Context;
		Context.AppendCommandList(GetCommandList());
		UUnrealEdViewportToolbarContext* ContextObject = NewObject<UUnrealEdViewportToolbarContext>();
		ContextObject->Viewport = SharedThis(this);
		ContextObject->bShowCoordinateSystemControls = false;
		ContextObject->bShowSurfaceSnap = false;
		Context.AddObject(ContextObject);
		return UToolMenus::Get()->GenerateWidget(ToolbarName, Context);
	}

	virtual void PopulateViewportOverlays(TSharedRef<SOverlay> Overlay) override
	{
		SEditorViewport::PopulateViewportOverlays(Overlay);
		Overlay->AddSlot()
			.VAlign(VAlign_Bottom)
			.HAlign(HAlign_Left)
			.Padding(8.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("FloatingBorder"))
				.Padding(8.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return Client.IsValid() ? Client->GetStatus() : FText::GetEmpty(); })
						.ColorAndOpacity_Lambda([this]()
						{
							return Client.IsValid() && Client->HasProblem() ? FSlateColor(FLinearColor(1.f, 0.35f, 0.3f)) : FSlateColor::UseForeground();
						})
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return Client.IsValid() ? Client->GetHelp() : FText::GetEmpty(); })
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]
			];
	}

private:
	TWeakObjectPtr<USoftbodyAuthoringData> Asset;
	TWeakObjectPtr<USoftbodySimulationPreviewSettings> Settings;
	TWeakObjectPtr<USoftbodyComponent> Values;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FSoftbodySimulationClient> Client;
	FVector2D LastInjected = FVector2D::ZeroVector;
	bool bInjected = false;
};

/**
 * Simulation 탭 내용. 뷰포트(미리보기 월드)는 탭이 처음 보일 때 만든다 — 창을 열면 이 탭은 Preview 뒤에 있고, 뒤에 있는 탭의 내용은 그려지지도
 * 틱되지도 않아서 Simulation을 보지 않으면 월드도 없다. 탭을 닫으면 위젯과 함께 월드가 사라진다.
 */
class SSoftbodySimulationHost : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSoftbodySimulationHost) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args, USoftbodyAuthoringData* InAsset, USoftbodySimulationPreviewSettings* InSettings, USoftbodyComponent* InValues,
		TFunction<void(USoftbodyComponent*)> InOnSoftbodyChanged)
	{
		Asset = InAsset;
		Settings = InSettings;
		Values = InValues;
		OnSoftbodyChanged = MoveTemp(InOnSoftbodyChanged);
		ChildSlot[SNullWidget::NullWidget];
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		if (!Viewport.IsValid())
		{
			Viewport = SNew(SSoftbodySimulationViewport, Asset.Get(), Settings.Get(), Values.Get());
			if (const TSharedPtr<FSoftbodySimulationClient> Client = Viewport->GetClient())
			{
				Client->OnSoftbodyChanged = OnSoftbodyChanged;
			}
			ChildSlot[Viewport.ToSharedRef()];
		}
	}

private:
	TWeakObjectPtr<USoftbodyAuthoringData> Asset;
	TWeakObjectPtr<USoftbodySimulationPreviewSettings> Settings;
	TWeakObjectPtr<USoftbodyComponent> Values;
	TFunction<void(USoftbodyComponent*)> OnSoftbodyChanged;
	TSharedPtr<SSoftbodySimulationViewport> Viewport;
};

// 개발: 창 없는 확인·위키 그림용. 사용자는 SA 창 도구 막대의 Simulate와 뷰포트의 Ctrl + 우클릭을 쓴다.
static FAutoConsoleCommand GSoftbodySimulationCommand(
	TEXT("Softbody.Simulation"),
	TEXT("Development: drives the Simulation tab of open Softbody Authoring Data windows. open = open the tab, press <depth cm> [radius cm] = press ")
	TEXT("the middle of the view until release, release, anim <asset path> = play an animation (skeletal meshes), state = one log line, ")
	TEXT("mouse <l|r> <down|move|up> <x 0-1> <y 0-1> [ctrl] = a mouse event through the viewport as a real mouse sends it."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const FString Verb = Args.Num() > 0 ? Args[0].ToLower() : FString();
		if (Verb == TEXT("open"))
		{
			UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			if (!Editors)
			{
				return;
			}
			for (UObject* Edited : Editors->GetAllEditedAssets())
			{
				IAssetEditorInstance* Instance = Editors->FindEditorForAsset(Edited, false);
				if (Instance && Instance->GetEditorName() == FName(TEXT("SoftbodyAuthoringEditor")))
				{
					static_cast<FSoftbodyAuthoringEditor*>(Instance)->OpenSimulation();
				}
			}
			return;
		}
		UAnimationAsset* Animation = Verb == TEXT("anim") && Args.Num() > 1 ? LoadObject<UAnimationAsset>(nullptr, *Args[1]) : nullptr;
		for (const TWeakPtr<SSoftbodySimulationViewport>& Weak : SSoftbodySimulationViewport::LiveSimulations())
		{
			const TSharedPtr<SSoftbodySimulationViewport> Viewport = Weak.Pin();
			const TSharedPtr<FSoftbodySimulationClient> Client = Viewport ? Viewport->GetClient() : nullptr;
			if (!Client)
			{
				continue;
			}
			if (Verb == TEXT("press"))
			{
				if (Args.Num() > 2 && Viewport->GetSettings())
				{
					Viewport->GetSettings()->PressRadius = FCString::Atof(*Args[2]);
				}
				Client->PressAtCenter(Args.Num() > 1 ? FCString::Atof(*Args[1]) : 1.f);
			}
			else if (Verb == TEXT("release"))
			{
				Client->Release();
			}
			else if (Verb == TEXT("state"))
			{
				Client->LogState();
			}
			else if (Verb == TEXT("anim") && Viewport->GetSettings())
			{
				Viewport->GetSettings()->Animation = Animation;
			}
			else if (Verb == TEXT("mouse") && Args.Num() >= 5)
			{
				const FKey Button = Args[1].ToLower() == TEXT("r") ? EKeys::RightMouseButton : EKeys::LeftMouseButton;
				const FVector2D Fraction(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]));
				Viewport->InjectMouse(Button, Args[2].ToLower(), Fraction, Args.Num() > 5 && Args[5].ToLower() == TEXT("ctrl"));
			}
		}
	}));

void FSoftbodyAuthoringEditor::InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host, USoftbodyAuthoringData* InAsset)
{
	using namespace SoftbodyAuthoringEditorLocal;
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsArgs.bHideSelectionTip = true;
	DetailsView = PropertyEditor.CreateDetailView(DetailsArgs);
	DetailsView->RegisterInstancedCustomPropertyLayout(USoftbodyAuthoringData::StaticClass(),
		FOnGetDetailCustomizationInstance::CreateLambda([]() -> TSharedRef<IDetailCustomization> { return MakeShared<FAuthoringCategoryOrder>(); }));
	DetailsView->SetObject(InAsset);

	Asset = InAsset;
	Preview = SNew(SSoftbodyCorePreviewViewport, InAsset);
	SimulationSettings.Reset(NewObject<USoftbodySimulationPreviewSettings>(GetTransientPackage(), NAME_None, RF_Transient));
	// 값 보관용(등록하지 않는다): 컴포넌트 기본값에서 시작한다.
	SimulationValues.Reset(NewObject<USoftbodyComponent>(GetTransientPackage(), NAME_None, RF_Transient));

	// Simulation Details: 위는 미리보기 설정(애니메이션·누르는 손가락), 아래는 이 미리보기에서만 쓰는 컴포넌트 값.
	TSharedRef<IDetailsView> SettingsDetails = PropertyEditor.CreateDetailView(DetailsArgs);
	SettingsDetails->SetObject(SimulationSettings.Get());
	SimulationValueDetails = PropertyEditor.CreateDetailView(DetailsArgs);
	// 레벨의 컴포넌트와 같은 줄만: 이 클래스가 선언한 값(메시·Mesh Data는 SA의 마지막 굽기로 정해 뺀다). 엔진 공통 칸(Activation·Tags 등)은 숨긴다.
	SimulationValueDetails->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& PropertyAndParent)
	{
		const FProperty* Root = PropertyAndParent.ParentProperties.Num() > 0 ? PropertyAndParent.ParentProperties.Last() : &PropertyAndParent.Property;
		return IsSimulationValue(Root);
	}));
	// 시뮬레이션이 돌기 전·탭을 닫은 뒤에는 보관용을 고친다 — 다음에 세울 때 그 값으로 시작한다.
	SimulationValueDetails->SetObject(SimulationValues.Get());
	SimulationDetailsPanel = SNew(SSplitter)
		.Orientation(Orient_Vertical)
		+ SSplitter::Slot().Value(0.25f)
		[
			SettingsDetails
		]
		+ SSplitter::Slot().Value(0.75f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 6.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SimValuesHint",
					"Component values for the Simulation tab only. They reset when this window closes. Shift + right-click a value or a category name to copy it."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SimulationValueDetails.ToSharedRef()
			]
		];

	// v3: 왼쪽은 Details 뒤에 Simulation Details, 오른쪽은 Preview 뒤에 Simulation — 둘 다 연 채, 앞은 Details·Preview(일지 234 §5).
	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("Standalone_SoftbodyAuthoringEditor_v3")
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split(FTabManager::NewStack()->SetSizeCoefficient(0.4f)
				->AddTab(DetailsTabId, ETabState::OpenedTab)
				->AddTab(SimulationDetailsTabId, ETabState::OpenedTab)
				->SetForegroundTab(DetailsTabId))
			->Split(FTabManager::NewStack()->SetSizeCoefficient(0.6f)
				->AddTab(PreviewTabId, ETabState::OpenedTab)
				->AddTab(SimulationTabId, ETabState::OpenedTab)
				->SetForegroundTab(PreviewTabId))
		);
	InitAssetEditor(Mode, Host, AppId, Layout, /*bCreateDefaultStandaloneMenu=*/true, /*bCreateDefaultToolbar=*/true, InAsset);

	TSharedPtr<FExtender> SimulateExtender = MakeShared<FExtender>();
	SimulateExtender->AddToolBarExtension("Asset", EExtensionHook::After, GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateSP(this, &FSoftbodyAuthoringEditor::FillToolbar));
	AddToolbarExtender(SimulateExtender);
	RegenerateMenusAndToolbars();
}

void FSoftbodyAuthoringEditor::FillToolbar(FToolBarBuilder& Builder)
{
	Builder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FSoftbodyAuthoringEditor::OpenSimulation)),
		NAME_None,
		LOCTEXT("Simulate", "Simulate"),
		LOCTEXT("SimulateTip", "Shows the Simulation tab and its component values. It runs the last bake as in a game, without placing it in a level."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "PlayWorld.Simulate"));
}

void FSoftbodyAuthoringEditor::OpenSimulation()
{
	if (const TSharedPtr<FTabManager> Tabs = GetTabManager())
	{
		Tabs->TryInvokeTab(SoftbodyAuthoringEditorLocal::SimulationDetailsTabId);
		Tabs->TryInvokeTab(SoftbodyAuthoringEditorLocal::SimulationTabId);
	}
}

void FSoftbodyAuthoringEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	using namespace SoftbodyAuthoringEditorLocal;
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "Softbody Authoring Data"));
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);
	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FSoftbodyAuthoringEditor::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
	InTabManager->RegisterTabSpawner(PreviewTabId, FOnSpawnTab::CreateSP(this, &FSoftbodyAuthoringEditor::SpawnPreviewTab))
		.SetDisplayName(LOCTEXT("PreviewTab", "Preview"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));
	InTabManager->RegisterTabSpawner(SimulationTabId, FOnSpawnTab::CreateSP(this, &FSoftbodyAuthoringEditor::SpawnSimulationTab))
		.SetDisplayName(LOCTEXT("SimulationTab", "Simulation"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "PlayWorld.Simulate"));
	InTabManager->RegisterTabSpawner(SimulationDetailsTabId, FOnSpawnTab::CreateSP(this, &FSoftbodyAuthoringEditor::SpawnSimulationDetailsTab))
		.SetDisplayName(LOCTEXT("SimulationDetailsTab", "Simulation Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FSoftbodyAuthoringEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	using namespace SoftbodyAuthoringEditorLocal;
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(PreviewTabId);
	InTabManager->UnregisterTabSpawner(SimulationTabId);
	InTabManager->UnregisterTabSpawner(SimulationDetailsTabId);
}

TSharedRef<SDockTab> FSoftbodyAuthoringEditor::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab).Label(LOCTEXT("DetailsTitle", "Details"))[DetailsView.ToSharedRef()];
}

TSharedRef<SDockTab> FSoftbodyAuthoringEditor::SpawnPreviewTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab).Label(LOCTEXT("PreviewTitle", "Preview"))[Preview.ToSharedRef()];
}

TSharedRef<SDockTab> FSoftbodyAuthoringEditor::SpawnSimulationTab(const FSpawnTabArgs& Args)
{
	// 열 때마다 새로 만든다 — 탭을 닫으면 위젯과 함께 미리보기 월드가 사라져 비용이 0이 된다. 값은 창이 쥔 보관용에 남는다.
	// 값 패널은 시뮬레이션이 돌면 그 컴포넌트를(고치면 바로 듣는다), 없으면 보관용을 보인다.
	TWeakPtr<IDetailsView> WeakDetails = SimulationValueDetails;
	TWeakObjectPtr<USoftbodyComponent> WeakValues = SimulationValues.Get();
	auto OnSoftbodyChanged = [WeakDetails, WeakValues](USoftbodyComponent* Live)
	{
		if (const TSharedPtr<IDetailsView> Details = WeakDetails.Pin())
		{
			Details->SetObject(Live ? Live : WeakValues.Get());
		}
	};
	return SNew(SDockTab).Label(LOCTEXT("SimulationTitle", "Simulation"))
	[
		SNew(SSoftbodySimulationHost, Asset.Get(), SimulationSettings.Get(), SimulationValues.Get(), OnSoftbodyChanged)
	];
}

TSharedRef<SDockTab> FSoftbodyAuthoringEditor::SpawnSimulationDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab).Label(LOCTEXT("SimulationDetailsTitle", "Simulation Details"))[SimulationDetailsPanel.ToSharedRef()];
}

FName FSoftbodyAuthoringEditor::GetToolkitFName() const
{
	return FName(TEXT("SoftbodyAuthoringEditor"));
}

FText FSoftbodyAuthoringEditor::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Softbody Authoring Data");
}

FString FSoftbodyAuthoringEditor::GetWorldCentricTabPrefix() const
{
	return TEXT("Softbody Authoring Data ");
}

FLinearColor FSoftbodyAuthoringEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.26f, 0.65f, 1.f, 0.5f);
}

#undef LOCTEXT_NAMESPACE
