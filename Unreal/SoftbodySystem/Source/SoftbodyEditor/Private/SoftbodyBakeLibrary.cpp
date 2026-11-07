// Copyright (c) 2026 wjsekrud

#include "SoftbodyBakeLibrary.h"
#include "ScopedTransaction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/ParallelFor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/MeshTransforms.h"
#include "DynamicMeshEditor.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Generators/BoxSphereGenerator.h"
#include "Generators/SphereGenerator.h"
#include "Generators/SweepGenerator.h"
#include "ImageCore.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "BoneWeights.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "SkeletalMeshAttributes.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkinnedAssetCompiler.h"
#include "Spatial/FastWinding.h"
#include "Spatial/PointHashGrid3.h"
#include "SoftbodyBakeShared.h"
#include "EditorFramework/AssetImportData.h"
#include "TextureImportSettings.h"
#include "SoftbodyMeasure.h"
#include "SoftbodyContactResponse.h"
#include "SoftbodyInertiaRuntime.h"
#include "SoftbodyInertiaAnalysis.h"
#include "SoftbodyLayerLinks.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyComponent.h"
#include "SoftbodySurfaceField.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "MeshQueries.h"
#include "Misc/FileHelper.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/** 렌더 정점 번호 First부터 Num개 — 스켈레탈 섹션은 제 정점 구간(BaseVertexIndex, NumVertices)을 통째로 가진다. */
	TArray<int32> SectionVertexRange(int32 First, int32 Num)
	{
		TArray<int32> Out;
		Out.Reserve(FMath::Max(Num, 0));
		for (int32 v = First; v < First + Num; ++v)
		{
			Out.Add(v);
		}
		return Out;
	}

	/**
	 * 스태틱 메시 섹션이 실제로 그리는 렌더 정점(인덱스 버퍼에서 모은다). 섹션의 MinVertexIndex..MaxVertexIndex는 정점 번호의
	 * 구간이라, 슬롯 경계의 정점을 두 섹션이 나눠 쓰면 그 사이에 있는 다른 슬롯의 정점까지 전부 품는다 — 두 슬롯짜리 판에서
	 * 한 슬롯의 마스크가 메시 전체에 칠해진다. 삼각형이 가리키는 정점만 모아야 슬롯별 칠이 그 슬롯 안에 머문다.
	 */
	TArray<int32> StaticSectionVertices(const FStaticMeshLODResources& LOD, const FStaticMeshSection& Sec)
	{
		TSet<int32> Unique;
		const uint32 NumIndices = (uint32)LOD.IndexBuffer.GetNumIndices();
		const uint32 End = FMath::Min(Sec.FirstIndex + Sec.NumTriangles * 3u, NumIndices);
		for (uint32 Index = Sec.FirstIndex; Index < End; ++Index)
		{
			Unique.Add((int32)LOD.IndexBuffer.GetIndex(Index));
		}
		TArray<int32> Out = Unique.Array();
		Out.Sort();
		return Out;
	}
}

using namespace UE::Geometry;

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyBake, Log, All);

namespace
{
	/**
	 * 굽기 텍스처를 쓸 패키지. 디스크에 같은 이름의 텍스처가 있는데 메모리에 없으면 먼저 올린다(일지 239). 구운 데이터가 접촉 지도 텍스처를
	 * 늦게 읽는 참조로 쥔 뒤(일지 222) 굽기 때 그 텍스처가 메모리에 없을 수 있는데, 올리지 않은 채 같은 이름으로 새로 만들면 저장이 "유효하지
	 * 않은 페이로드"로 실패해 구운 데이터와 텍스처가 어긋났다(새로 연 에디터에서 그 메시를 쓰는 것 없이 Run Bake, 커맨드릿 다시 굽기).
	 */
	UPackage* BakeTexturePackage(const FString& PackageName, const FString& Name)
	{
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		if (!FindObject<UTexture2D>(nullptr, *ObjectPath) && FPackageName::DoesPackageExist(PackageName))
		{
			LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return CreatePackage(*PackageName);
	}

	UTexture2D* CreateAttributeTextureRect(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<FFloat16Color>& Texels)
	{
		const FString PackageName = OutputFolder / Name;
		UPackage* Pkg = BakeTexturePackage(PackageName, Name);
		UTexture2D* Tex = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		Tex->Source.Init(Width, Height, 1, 1, TSF_RGBA16F, (const uint8*)Texels.GetData());
		Tex->CompressionSettings = TC_HDR;
		Tex->SRGB = false;
		Tex->Filter = TF_Nearest;
		Tex->MipGenSettings = TMGS_NoMipmaps;
		Tex->NeverStream = true;
		Tex->UpdateResource();
		Tex->PostEditChange();
		FAssetRegistryModule::AssetCreated(Tex);
		Pkg->MarkPackageDirty();
		return Tex;
	}

	UTexture2D* CreateAttributeTexture(const FString& OutputFolder, const FString& Name, int32 Size,
		const TArray<FFloat16Color>& Texels)
	{
		return CreateAttributeTextureRect(OutputFolder, Name, Size, Size, Texels);
	}

	UStaticMesh* SaveDynamicMeshAsStaticMesh(const FString& AssetPath, FDynamicMesh3& Mesh)
	{
		FMeshDescription MD;
		FStaticMeshAttributes MDAttrs(MD);
		MDAttrs.Register();
		FDynamicMeshToMeshDescription MeshConverter;
		MeshConverter.Convert(&Mesh, MD);

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UPackage* Pkg = CreatePackage(*AssetPath);
		UStaticMesh* SM = NewObject<UStaticMesh>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
		SM->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface)));

		FStaticMeshSourceModel& SrcModel = SM->AddSourceModel();
		SrcModel.BuildSettings.bRecomputeNormals = false;
		SrcModel.BuildSettings.bRecomputeTangents = true;
		SrcModel.BuildSettings.bGenerateLightmapUVs = false;

		SM->CreateMeshDescription(0, MoveTemp(MD));
		SM->CommitMeshDescription(0);
		SM->Build(false);
		SM->PostEditChange();
		FAssetRegistryModule::AssetCreated(SM);
		Pkg->MarkPackageDirty();

		UE_LOG(LogSoftbodyBake, Display, TEXT("Created test mesh: %s (%d vertices)"), *AssetPath, Mesh.VertexCount());
		return SM;
	}

	bool BuildInertiaShading(USoftbodyMeshData* Data, const TArray<FFloat16Color>* PaintTexels);

	/** 메시 종류 무관 베이크 산출물 */
	struct FBakeCoreOut
	{
		int32 TexelChannel = INDEX_NONE;
		int32 TexSize = 0;
		int32 NumVertices = 0;
		int32 NumInstances = 0;
		TArray<FFloat16Color> TexDbg; // 관성 무름/임베딩/코어 연결 검수 (AttributeTexDebug)
		int32 InertiaResponseVersion = 0;
		int32 NumInertiaResponses = 0;
		TArray<FFloat16Color> TexInertiaResponses; // TexSize × TexSize·3
		TArray<float> InertiaResponseScales;
		FVector3f InertiaAnchorLocal = FVector3f::ZeroVector;
		float InertiaAnchorReach = 0.f;
		int32 InertiaAnchorNodeCount = 0;
		float InertiaMaxAnchorError = 0.f;
		float InertiaMaxRelativeResidual = 0.f;
		TArray<FVector3f> VertexInertiaResponseShapes;
		TArray<int32> VertTexel; // MD 정점 ID → 텍셀 인덱스 (후속 채널 텍스처 생성용)
		TArray<FVector3f> VertexPositions;
		TArray<float> VertexAreas;
		int32 BakeDiagnosticsVersion = 0;
		TArray<uint8> VertexEmbeddingStatus;
		TArray<float> VertexEmbeddingMinWeight;
		TArray<uint8> VertexCoreConnection;
		TArray<float> VertexInertiaResponseRms;
		float MedianEdgeLength = 0.f;
		/** 겹친 층(ADR-0014) 연결표(텍셀 번호)와 찾은 거리. */
		TArray<FSoftbodyLayerLink> LayerLinks;
		float LayerMaxGap = 0.f;
		/** 무름 풀이의 살 격자와 분해(무름을 구웠을 때만). 덩어리 원판 누름(SolveSoftbodyLumpDiscs)이 쓰고, 쓰고 나면 바로 놓는다. */
		TSharedPtr<FSoftbodyMeasureSolver> MeasureSolver;
	};

	/**
	 * 전에 구운 텍셀 UV 채널이 메시에 남아 있으면 그 번호(없으면 INDEX_NONE). 베이크 데이터를 지웠거나 다른 폴더에 다시 구우면
	 * 데이터가 채널 번호를 모르므로, 메시에 채널이 하나씩 쌓였다. 텍셀 채널은 모양으로 알아본다: 같은 정점의 인스턴스는 같은 값이고,
	 * 값은 TexSize² 격자의 칸 가운데이며, 정점마다 칸이 다르다. 0번(아티스트의 UV0)은 보지 않는다.
	 */
	int32 FindPreviousTexelChannel(const FMeshDescription& MD, int32 TexSize)
	{
		FStaticMeshConstAttributes Attrs(MD);
		TVertexInstanceAttributesConstRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
		if (TexSize <= 0)
		{
			return INDEX_NONE;
		}
		for (int32 Channel = UVs.GetNumChannels() - 1; Channel >= 1; --Channel)
		{
			TMap<FVertexID, int32> CellOf;
			TSet<int32> Used;
			bool bTexelLike = MD.VertexInstances().Num() > 0;
			for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
			{
				const FVector2f Value = UVs.Get(Instance, Channel);
				const float X = Value.X * TexSize - 0.5f;
				const float Y = Value.Y * TexSize - 0.5f;
				const int32 CX = FMath::RoundToInt32(X);
				const int32 CY = FMath::RoundToInt32(Y);
				if (FMath::Abs(X - CX) > 1e-3f || FMath::Abs(Y - CY) > 1e-3f || CX < 0 || CY < 0 || CX >= TexSize || CY >= TexSize)
				{
					bTexelLike = false;
					break;
				}
				const int32 Cell = CY * TexSize + CX;
				const FVertexID Vertex = MD.GetVertexInstanceVertex(Instance);
				if (const int32* Known = CellOf.Find(Vertex))
				{
					if (*Known != Cell)
					{
						bTexelLike = false;
						break;
					}
					continue;
				}
				bool bAlreadyUsed = false;
				Used.Add(Cell, &bAlreadyUsed);
				if (bAlreadyUsed)
				{
					bTexelLike = false;
					break;
				}
				CellOf.Add(Vertex, Cell);
			}
			if (bTexelLike)
			{
				return Channel;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * 측정에 넘길 칠에 겹친 층(ADR-0014)을 적용한다: 옷 층 정점의 Depth·Jiggle·Volume = 밑 살 값들의 무게 합(ADR-0038 §2 — Softness만 제 슬롯).
	 * 측정은 살 속 격자 점마다 가장 가까운 겉면의 칠을 읽는데, 그 겉면이 옷이면 옷의 제 칠(옷 본 웨이트에서 나온 Depth 0 = 뼈, Jiggle 0 = 뼈 강성)이
	 * 살로 들어가 옷 밑 살이 뼈처럼 굳는다(런타임 칠·접촉 지도는 이미 연결된 값을 써서 출렁임과 눌림이 다른 살을 봤다, 원칙 P04).
	 * 연결표는 텍셀 번호(정점 원소 순서), 측정 칠은 정점 ID 순서라 옮겨 적용한다. 바꿀 것이 있으면 Measure의 사본을 OutLayered에 만들고 참을 돌려준다.
	 */
	bool ApplyLayerLinksToMeasurePaint(const FMeshDescription& MD, const TArray<FSoftbodyLayerLink>& Links,
		const FSoftbodyMeasureParams& Measure, FSoftbodyMeasureParams& OutLayered)
	{
		const int32 MaxVertID = MD.Vertices().GetArraySize();
		const auto IsPainted = [MaxVertID](const TArray<float>& Values) { return Values.Num() == MaxVertID; };
		if (Links.Num() == 0 || !(IsPainted(Measure.VertexDepth) || IsPainted(Measure.VertexJiggle) || IsPainted(Measure.VertexVolume)))
		{
			return false;
		}
		TArray<FVertexID> TexelVertex;
		TexelVertex.Reserve(MD.Vertices().Num());
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			TexelVertex.Add(VertID);
		}
		OutLayered = Measure;
		int32 NumChanged = 0;
		const auto Remap = [&](TArray<float>& Values)
		{
			if (!IsPainted(Values))
			{
				return;
			}
			TArray<float> ByTexel;
			ByTexel.SetNumUninitialized(TexelVertex.Num());
			for (int32 t = 0; t < TexelVertex.Num(); ++t)
			{
				ByTexel[t] = Values[TexelVertex[t].GetValue()];
			}
			SoftbodyLayers::Apply(Links, ByTexel, 0.f);
			for (const FSoftbodyLayerLink& Link : Links)
			{
				if (ByTexel.IsValidIndex(Link.Vertex))
				{
					float& Value = Values[TexelVertex[Link.Vertex].GetValue()];
					NumChanged += FMath::Abs(Value - ByTexel[Link.Vertex]) > 0.01f ? 1 : 0;
					Value = ByTexel[Link.Vertex];
				}
			}
		};
		Remap(OutLayered.VertexDepth);
		Remap(OutLayered.VertexJiggle);
		Remap(OutLayered.VertexVolume);
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("Measure: %d layered clothing vertices read the Depth, Jiggle and Volume of the flesh underneath (%d painted values changed)"),
			Links.Num(), NumChanged);
		return true;
	}

	/**
	 * 베이크 코어: 텍셀 UV 채널 기록 + 측정(FEM) 백엔드 실행 + 속성 텍셀 계산 (설계 문서 §5).
	 * MD를 직접 수정하므로 커밋/빌드는 호출측(스태틱/스켈레탈) 책임.
	 */
	bool RunBakeCore(FMeshDescription& MD, int32 ReuseChannel, int32 MaxUVChannels,
		FBakeCoreOut& Out, const FSoftbodyMeasureParams& Measure)
	{
		// 스켈레탈 MD도 기본 정점 속성(위치/노멀/UV)은 스태틱과 이름을 공유한다
		FStaticMeshAttributes Attrs(MD);
		TVertexAttributesRef<FVector3f> Positions = Attrs.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();

		Out.NumVertices = MD.Vertices().Num();
		Out.NumInstances = MD.VertexInstances().Num();

		// 단위 고정 임계값 대신 메시 해상도에서 자동 최소값을 유도한다. 공유 에지는 두 번
		// 들어갈 수 있지만 중앙값에는 영향이 없고, 비정상적으로 긴 소수 에지에도 강건하다.
		TArray<float> EdgeLengths;
		EdgeLengths.Reserve(MD.Triangles().Num() * 3);
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> Tri = MD.GetTriangleVertices(TriID);
			for (int32 e = 0; e < 3; ++e)
			{
				const float L = FVector3f::Distance(Positions[Tri[e]], Positions[Tri[(e + 1) % 3]]);
				if (L > UE_KINDA_SMALL_NUMBER && FMath::IsFinite(L))
				{
					EdgeLengths.Add(L);
				}
			}
		}
		if (EdgeLengths.Num() > 0)
		{
			EdgeLengths.Sort();
			const int32 Hi = EdgeLengths.Num() / 2;
			Out.MedianEdgeLength = (EdgeLengths.Num() & 1)
				? EdgeLengths[Hi]
				: 0.5f * (EdgeLengths[Hi - 1] + EdgeLengths[Hi]);
		}
		// 텍셀은 용접 정점 단위 — 속성(무름·진단·페인트)이 전부 정점 단위라
		// 인스턴스별 고유 텍셀은 정보 이득이 없고, 인스턴스마다 다른 UV는 렌더 빌드의
		// 정점 병합을 전부 깨뜨린다 (시험 캐릭터: 렌더 정점 4.4만 → 20만, VS 부하 4.5배).
		Out.TexSize = FMath::CeilToInt(FMath::Sqrt((float)Out.NumVertices));

		// --- 고유 텍셀 UV 채널 확보 (재베이크 시 기존 채널 재사용) ---
		const int32 PreviousChannel = ReuseChannel != INDEX_NONE && ReuseChannel < UVs.GetNumChannels()
			? ReuseChannel : FindPreviousTexelChannel(MD, Out.TexSize);
		if (PreviousChannel != INDEX_NONE)
		{
			Out.TexelChannel = PreviousChannel;
			if (PreviousChannel != ReuseChannel)
			{
				UE_LOG(LogSoftbodyBake, Display, TEXT("Reusing UV channel %d from an earlier Softbody bake of this mesh."), PreviousChannel);
			}
		}
		else
		{
			Out.TexelChannel = UVs.GetNumChannels();
			if (Out.TexelChannel >= MaxUVChannels)
			{
				UE_LOG(LogSoftbodyBake, Error, TEXT("The mesh already uses all %d UV channels, and the bake needs one more. Remove an unused UV channel from the mesh and bake again."), MaxUVChannels);
				return false;
			}
			UVs.SetNumChannels(Out.TexelChannel + 1);
		}

		// --- 측정 지오메트리 준비 ---
		FDynamicMesh3 DynMesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(&MD, DynMesh);

		// DynMesh는 바운드·BVH 레이캐스트·winding 질의 등 위치 기반으로만 쓰인다 —
		// MD 정점 인덱스와의 대응은 어디서도 요구하지 않으므로, 논매니폴드 에지 때문에
		// 변환기가 정점을 분할해도(시험 캐릭터: +478) 기하는 동일해 무해하다.
		// 삼각형 소실만이 실제 위험(레이캐스트 구멍) — 그것만 검사한다.
		if (DynMesh.TriangleCount() != MD.Triangles().Num())
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("Bake failed: triangles were lost while converting the mesh (mesh description %d, converted %d). Check the mesh for broken or degenerate triangles."), MD.Triangles().Num(), DynMesh.TriangleCount());
			return false;
		}
		if (DynMesh.VertexCount() != Out.NumVertices)
		{
			UE_LOG(LogSoftbodyBake, Display, TEXT("Non-manifold edges split vertices: %d -> %d (harmless, used only for position queries)"),
				Out.NumVertices, DynMesh.VertexCount());
		}

		// 속 메시(ADR-0028): 살을 채울 안팎은 이 메시에서 읽는다. 닫히지 않은 얇은 막은 제 메시로는 속이 없다.
		FDynamicMesh3 VolumeDyn;
		const FDynamicMesh3* Solid = nullptr;
		if (Measure.VolumeMesh)
		{
			const FMeshDescription* VolumeMD = Measure.VolumeMesh->GetMeshDescription(0);
			if (!VolumeMD || VolumeMD->Triangles().Num() == 0)
			{
				UE_LOG(LogSoftbodyBake, Error, TEXT("Volume Mesh %s has no LOD0 source geometry. Pick a static mesh with source data for Volume Mesh."), *Measure.VolumeMesh->GetName());
				return false;
			}
			FMeshDescriptionToDynamicMesh VolumeConverter;
			VolumeConverter.bDisableAttributes = true;
			VolumeConverter.Convert(VolumeMD, VolumeDyn);
			Solid = &VolumeDyn;
			UE_LOG(LogSoftbodyBake, Display, TEXT("Volume Mesh: %s (%d triangles) - the flesh volume comes from this mesh, the skin and paint from the target mesh"),
				*Measure.VolumeMesh->GetName(), VolumeDyn.TriangleCount());
		}

		TArray<float> VertexAreaById;
		VertexAreaById.Init(0.f, MD.Vertices().GetArraySize());
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> Tri = MD.GetTriangleVertices(TriID);
			const float AreaThird = FVector3f::CrossProduct(
				Positions[Tri[1]] - Positions[Tri[0]],
				Positions[Tri[2]] - Positions[Tri[0]]).Size() / 6.f;
			for (int32 k = 0; k < 3; ++k)
			{
				VertexAreaById[Tri[k]] += AreaThird;
			}
		}

		const int32 MaxVertID = MD.Vertices().GetArraySize();

		// --- 정점당 속성: 측정(FEM) 백엔드 ---
		TArray<uint8> VertexEmbeddingStatus;
		TArray<float> VertexEmbeddingMinWeight;
		TArray<uint8> VertexCoreConnection;
		TArray<float> VertexInertiaResponseRms;
		TArray<FVector3f> MDInertiaResponseShapes; // 응답장-메이저 [k·MaxVertID + VertID]
		// --- 겹친 층(ADR-0014)은 측정 전에 찾는다: 측정도 옷 층의 Depth·Jiggle·Volume을 밑 살 값으로 읽는다(ADR-0038 §2) ---
		Out.LayerMaxGap = Measure.LayerMaxGap;
		SoftbodyLayers::BuildLayerLinks(MD, Measure.LayerMaxGap, Measure.LayerSlots, Out.LayerLinks);
		FSoftbodyMeasureParams LayeredMeasure;
		const bool bLayeredPaint = ApplyLayerLinksToMeasurePaint(MD, Out.LayerLinks, Measure, LayeredMeasure);
		{
			FSoftbodyMeasureOut MeasureOut;
			if (!RunSoftbodyMeasure(MD, DynMesh, Solid, bLayeredPaint ? LayeredMeasure : Measure, MeasureOut))
			{
				return false;
			}
			Out.BakeDiagnosticsVersion = MeasureOut.BakeDiagnosticsVersion;
			VertexEmbeddingStatus = MoveTemp(MeasureOut.VertexEmbeddingStatus);
			VertexEmbeddingMinWeight =
				MoveTemp(MeasureOut.VertexEmbeddingMinWeight);
			VertexCoreConnection = MoveTemp(MeasureOut.VertexCoreConnection);
			VertexInertiaResponseRms =
				MoveTemp(MeasureOut.VertexInertiaResponseRms);
			Out.InertiaResponseVersion = MeasureOut.InertiaResponseVersion;
			Out.NumInertiaResponses = MeasureOut.NumInertiaResponses;
			Out.InertiaResponseScales = MoveTemp(MeasureOut.InertiaResponseScales);
			Out.InertiaAnchorLocal = MeasureOut.InertiaAnchorLocal;
			Out.InertiaAnchorReach = MeasureOut.InertiaAnchorReach;
			Out.InertiaAnchorNodeCount = MeasureOut.InertiaAnchorNodeCount;
			Out.InertiaMaxAnchorError = MeasureOut.InertiaMaxAnchorError;
			Out.InertiaMaxRelativeResidual = MeasureOut.InertiaMaxRelativeResidual;
			MDInertiaResponseShapes = MoveTemp(MeasureOut.VertexInertiaResponseShapes);
			Out.MeasureSolver = MoveTemp(MeasureOut.Solver);
		}

		// --- 겹친 층(ADR-0014): 속옷처럼 덮인 면의 정점이 바로 아래 살의 무름을 따른다(연결표는 측정 전에 찾았다) ---
		if (Out.LayerLinks.Num() > 0 && Out.NumInertiaResponses > 0)
		{
			TArray<FVertexID> TexelVertex;
			TexelVertex.Reserve(Out.NumVertices);
			for (const FVertexID VertID : MD.Vertices().GetElementIDs())
			{
				TexelVertex.Add(VertID);
			}
			float SharedMax = 0.f;
			TArray<FVector3f> Shapes;
			for (int32 r = 0; r < Out.NumInertiaResponses; ++r)
			{
				Shapes.SetNumUninitialized(TexelVertex.Num());
				for (int32 t = 0; t < TexelVertex.Num(); ++t)
				{
					Shapes[t] = MDInertiaResponseShapes[r * MaxVertID + TexelVertex[t]];
				}
				SoftbodyLayers::Apply(Out.LayerLinks, Shapes, FVector3f::ZeroVector);
				for (int32 t = 0; t < TexelVertex.Num(); ++t)
				{
					MDInertiaResponseShapes[r * MaxVertID + TexelVertex[t]] = Shapes[t];
					SharedMax = FMath::Max(SharedMax, Shapes[t].Size());
				}
			}
			// 가장 무른 점 = 1을 지킨다(그 점이 덮은 면이었으면 살 값으로 조금 줄었다). 원시 크기도 같이 맞춘다.
			if (SharedMax > 1e-6f && SharedMax < 1.f - 1e-5f)
			{
				for (FVector3f& Shape : MDInertiaResponseShapes)
				{
					Shape /= SharedMax;
				}
				for (float& Scale : Out.InertiaResponseScales)
				{
					Scale *= SharedMax;
				}
			}
		}

		// --- 인스턴스별 텍셀 UV 기록 + 속성 텍셀 채우기 ---
		const int32 TexSize = Out.TexSize;
		Out.TexDbg.Init(FFloat16Color(FLinearColor::Black), TexSize * TexSize);
		if (Out.NumInertiaResponses > 0)
		{
			Out.TexInertiaResponses.Init(
				FFloat16Color(FLinearColor::Black),
				TexSize * TexSize * Out.NumInertiaResponses);
		}

		TArray<int32>& VertTexel = Out.VertTexel;
		VertTexel.Init(INDEX_NONE, MaxVertID);
		int32 TexelIdx = 0;
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			const int32 TX = TexelIdx % TexSize;
			const int32 TY = TexelIdx / TexSize;
			VertTexel[VertID] = TexelIdx;
			{
				const uint8 EmbedStatus = VertexEmbeddingStatus[VertID];
				const float EmbedVisual = EmbedStatus == 3
					? 1.f : (EmbedStatus == 2 ? 0.5f : 0.f);
				const float CoreVisual =
					VertexCoreConnection[VertID] == 1 ? 1.f : 0.f;
				// G = 관성 무름: 살을 당긴 방향으로 밀리는 정도의 세 방향 평균(가장 무른 점 ≈ 1, 음수 = 반대로 밀림).
				float InertiaAlong = 0.f;
				if (Out.NumInertiaResponses == 3)
				{
					for (int32 r = 0; r < 3; ++r)
					{
						InertiaAlong += MDInertiaResponseShapes[r * MaxVertID + VertID][r] / 3.f;
					}
				}
				Out.TexDbg[TY * TexSize + TX] = FFloat16Color(FLinearColor(
					0.f, InertiaAlong, EmbedVisual, CoreVisual));
			}
			for (int32 r = 0; r < Out.NumInertiaResponses; ++r)
			{
				const FVector3f Response =
					MDInertiaResponseShapes[r * MaxVertID + VertID];
				Out.TexInertiaResponses[(r * TexSize + TY) * TexSize + TX] =
					FFloat16Color(FLinearColor(Response.X, Response.Y, Response.Z, 0.f));
			}
			++TexelIdx;
		}
		// 같은 정점의 인스턴스는 같은 텍셀 UV를 공유 → 렌더 정점 병합 보존
		for (const FVertexInstanceID InstID : MD.VertexInstances().GetElementIDs())
		{
			const int32 t = VertTexel[MD.GetVertexInstanceVertex(InstID)];
			UVs.Set(InstID, Out.TexelChannel,
				FVector2f((t % TexSize + 0.5f) / TexSize, (t / TexSize + 0.5f) / TexSize));
		}

		// --- CPU측 검수용 정점 배열 ---
		Out.VertexPositions.Reset(Out.NumVertices);
		Out.VertexAreas.Reset(Out.NumVertices);
		Out.VertexEmbeddingStatus.Reset(Out.NumVertices);
		Out.VertexEmbeddingMinWeight.Reset(Out.NumVertices);
		Out.VertexCoreConnection.Reset(Out.NumVertices);
		Out.VertexInertiaResponseRms.Reset(Out.NumVertices);
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			Out.VertexPositions.Add(Positions[VertID]);
			Out.VertexAreas.Add(VertexAreaById[VertID]);
			if (Out.BakeDiagnosticsVersion > 0)
			{
				Out.VertexEmbeddingStatus.Add(VertexEmbeddingStatus[VertID]);
				Out.VertexEmbeddingMinWeight.Add(
					VertexEmbeddingMinWeight[VertID]);
				Out.VertexCoreConnection.Add(VertexCoreConnection[VertID]);
				Out.VertexInertiaResponseRms.Add(
					VertexInertiaResponseRms[VertID]);
			}
		}
		if (Out.NumInertiaResponses > 0)
		{
			Out.VertexInertiaResponseShapes.Reset(
				Out.NumInertiaResponses * Out.NumVertices);
			for (int32 r = 0; r < Out.NumInertiaResponses; ++r)
			{
				for (const FVertexID VertID : MD.Vertices().GetElementIDs())
				{
					Out.VertexInertiaResponseShapes.Add(
						MDInertiaResponseShapes[r * MaxVertID + VertID]);
				}
			}
		}
		return true;
	}

	/** SD_ 데이터 에셋 재사용/생성 + 코어 산출물 반영 (텍스처 생성 포함) */
	USoftbodyMeshData* CommitBakeOutputs(const FString& OutputFolder, const FString& MeshName,
		USoftbodyMeshData* Existing, FBakeCoreOut& Core)
	{
		UTexture2D* AttribTexDbg = Core.TexDbg.Num() > 0
			? CreateAttributeTexture(OutputFolder, FString::Printf(TEXT("T_%s_Dbg"), *MeshName), Core.TexSize, Core.TexDbg)
			: nullptr;
		UTexture2D* InertiaResponseTex = Core.NumInertiaResponses > 0
			? CreateAttributeTextureRect(OutputFolder,
				FString::Printf(TEXT("T_%s_InertiaResponse"), *MeshName),
				Core.TexSize, Core.TexSize * Core.NumInertiaResponses,
				Core.TexInertiaResponses)
			: nullptr;

		USoftbodyMeshData* Data = Existing;
		if (!Data)
		{
			const FString DataAssetName = FString::Printf(TEXT("SD_%s"), *MeshName);
			const FString PackageName = OutputFolder / DataAssetName;
			UPackage* Pkg = CreatePackage(*PackageName);
			Data = NewObject<USoftbodyMeshData>(Pkg, FName(*DataAssetName), RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Data);
		}
		// 메시와 함께 되돌려지게(저작 에셋의 실행 버튼이 연 Undo 묶음): 데이터만 새 값으로 남으면 메시의 UV 채널과 어긋난다.
		Data->SetFlags(RF_Transactional);
		Data->Modify();
		Data->AttributeTexDebug = AttribTexDbg;
		Data->MedianEdgeLength = Core.MedianEdgeLength;
		Data->InertiaResponseVersion = Core.InertiaResponseVersion;
		Data->NumInertiaResponses = Core.NumInertiaResponses;
		Data->InertiaResponseTex = InertiaResponseTex;
		// 음영 줄(BuildInertiaShading)은 칠이 정해진 뒤에 붙는다.
		Data->InertiaResponseRows = Core.NumInertiaResponses;
		Data->InertiaResponseScales = MoveTemp(Core.InertiaResponseScales);
		Data->InertiaAnchorLocal = Core.InertiaAnchorLocal;
		Data->InertiaAnchorReach = Core.InertiaAnchorReach;
		Data->InertiaAnchorNodeCount = Core.InertiaAnchorNodeCount;
		Data->InertiaMaxAnchorError = Core.InertiaMaxAnchorError;
		Data->InertiaMaxRelativeResidual = Core.InertiaMaxRelativeResidual;
		Data->VertexInertiaResponseShapes =
			MoveTemp(Core.VertexInertiaResponseShapes);
		Data->TexelUVChannel = Core.TexelChannel;
		Data->TextureSize = Core.TexSize;
		Data->VertexPositions = MoveTemp(Core.VertexPositions);
		Data->VertexAreas = MoveTemp(Core.VertexAreas);
		Data->BakeDiagnosticsVersion = Core.BakeDiagnosticsVersion;
		Data->VertexEmbeddingStatus =
			MoveTemp(Core.VertexEmbeddingStatus);
		Data->VertexEmbeddingMinWeight =
			MoveTemp(Core.VertexEmbeddingMinWeight);
		Data->VertexCoreConnection =
			MoveTemp(Core.VertexCoreConnection);
		Data->VertexInertiaResponseRms =
			MoveTemp(Core.VertexInertiaResponseRms);
		Data->LayerLinks = MoveTemp(Core.LayerLinks);
		Data->LayerMaxGap = Core.LayerMaxGap;
		Data->MarkPackageDirty();
		return Data;
	}

	USoftbodyMeshData* FindExistingData(const FString& OutputFolder, const FString& MeshName)
	{
		const FString DataAssetName = FString::Printf(TEXT("SD_%s"), *MeshName);
		const FString DataAssetPath = OutputFolder / DataAssetName + TEXT(".") + DataAssetName;
		return LoadObject<USoftbodyMeshData>(nullptr, *DataAssetPath);
	}

	/** 스태틱 측정 베이크 경로 */
	USoftbodyMeshData* BakeStaticInternal(UStaticMesh* Mesh, const FString& OutputFolder,
		const FSoftbodyMeasureParams& Measure)
	{
		if (!Mesh)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("No mesh given. Set Target Mesh and bake again."));
			return nullptr;
		}
		if (!SoftbodyBakeShared::IsEditableContent(Mesh))
		{
			UE_LOG(LogSoftbodyBake, Error,
				TEXT("%s is engine content, and baking writes into the mesh (it adds UV channels). Duplicate the mesh into your project's "
					"Content folder and bake the copy."), *Mesh->GetPathName());
			return nullptr;
		}

		FMeshDescription* MD = Mesh->GetMeshDescription(0);
		if (!MD)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("%s has no LOD0 source geometry, so it cannot be baked. Reimport the mesh with its source data."), *Mesh->GetPathName());
			return nullptr;
		}

		const FString MeshName = Mesh->GetName();
		USoftbodyMeshData* Existing = FindExistingData(OutputFolder, MeshName);
		const int32 ReuseChannel = (Existing && Existing->SourceMesh == Mesh) ? Existing->TexelUVChannel : INDEX_NONE;

		// 메시 설명을 고치기 전에 Undo 기록을 남긴다.
		Mesh->Modify();
		FBakeCoreOut Core;
		if (!RunBakeCore(*MD, ReuseChannel, 8 /*MAX_STATIC_TEXCOORDS*/, Core, Measure))
		{
			return nullptr;
		}

		// 스태틱 메시에는 덩어리가 없다 — 누르면 통째로 비켜나는 살은 스켈레탈의 Lump 본뿐이다(일지 296).
		// 그래서 덩어리 국소 모양의 원판 누름도 풀지 않는다(스태틱 굽기 시간의 39~50 %였다).
		Core.MeasureSolver.Reset();

		// --- 메시 커밋 (라이트맵 UV 자동 생성이 텍셀 채널을 덮으면 생성 채널을 뒤로 옮긴다) ---
		SoftbodyBakeShared::KeepLightmapUVsFree(Mesh, FStaticMeshAttributes(*MD).GetVertexInstanceUVs().GetNumChannels(),
			{Core.TexelChannel}, 8 /*MAX_STATIC_TEXCOORDS*/);
		SoftbodyBakeShared::WriteLodTexelChannels(Mesh, *MD, Core.VertTexel, Core.TexSize, Core.TexelChannel, 8);
		Mesh->CommitMeshDescription(0);
		Mesh->Build(false);
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();

		USoftbodyMeshData* Data = CommitBakeOutputs(OutputFolder, MeshName, Existing, Core);
		Data->SourceMesh = Mesh;
		Data->SourceSkeletalMesh = nullptr;
		Data->VolumeMesh = Measure.VolumeMesh;
		Data->LumpDiscBakes.Reset();
		// 페인트 생성(GeneratePaintTexture*)을 거치지 않는 스태틱 베이크도 음영 줄을 갖게 한다(칠 = 1). 페인트를 만들면 다시 굽는다.
		BuildInertiaShading(Data, nullptr);

		UE_LOG(LogSoftbodyBake, Display, TEXT("Bake done: %s - %d vertices, %d vertex instances, texture %d², UV channel %d"),
			*MeshName, Core.NumVertices, Core.NumInstances, Core.TexSize, Core.TexelChannel);
		return Data;
	}

	/** 레퍼런스 포즈 컴포넌트 공간의 본 트랜스폼 (로컬 포즈를 부모 체인으로 합성) */
	void BuildRefPoseComponentSpace(const FReferenceSkeleton& RefSkel, TArray<FTransform>& OutPose)
	{
		const TArray<FTransform>& LocalPose = RefSkel.GetRefBonePose();
		OutPose.SetNum(RefSkel.GetNum());
		for (int32 b = 0; b < RefSkel.GetNum(); ++b)
		{
			const int32 Parent = RefSkel.GetParentIndex(b);
			OutPose[b] = Parent >= 0 ? LocalPose[b] * OutPose[Parent] : LocalPose[b];
		}
	}

	/**
	 * 살 슬롯(일지 239)의 정점 표(정점 ID 자리). 코어의 본 선과 관성 묶음이 같은 정점을 센다 — 다른 슬롯만 움직이는 본(머리카락, 치마)은
	 * 뼈도 묶음도 아니다. FleshSlots가 비었거나 어느 삼각형의 슬롯 이름과도 맞지 않으면 전부 참(메시 전체)이고, 맞지 않은 경우만
	 * bOutUnmatched가 참이다.
	 */
	TArray<bool> FleshVertexMask(const FMeshDescription& MD, const TSet<FName>* FleshSlots, bool& bOutUnmatched)
	{
		TArray<bool> InFlesh;
		InFlesh.Init(true, MD.Vertices().GetArraySize());
		bOutUnmatched = false;
		if (!FleshSlots || FleshSlots->Num() == 0)
		{
			return InFlesh;
		}
		TArray<bool> Marked;
		Marked.Init(false, InFlesh.Num());
		bool bAny = false;
		const TPolygonGroupAttributesConstRef<FName> SlotNames =
			MD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		for (const FTriangleID Tri : MD.Triangles().GetElementIDs())
		{
			if (FleshSlots->Contains(SlotNames[MD.GetTrianglePolygonGroup(Tri)]))
			{
				for (const FVertexID Corner : MD.GetTriangleVertices(Tri))
				{
					Marked[Corner.GetValue()] = true;
				}
				bAny = true;
			}
		}
		bOutUnmatched = !bAny;
		return bAny ? Marked : InFlesh;
	}

	/**
	 * 뼈대의 본 선 → 레퍼런스 포즈 공간 구 사슬(측정 베이크의 RigidSpheres, 표면 캐리어의 두께 기준). 살 속의 단단한 속이다.
	 *
	 * 뼈가 지나가는 자리는 본에서 읽는다. 스킨 웨이트가 걸린 본의 조인트에서 스킨 웨이트가 걸린 자식 본의 조인트까지가 그 본의
	 * 뼈다. 선이 조인트에서 조인트로 가므로 관절에서 끊기지 않는다. 그런 자식이 없는 끝 본(손끝·발끝·머리)은 조인트에서 제 살
	 * (그 본이 가장 큰 웨이트인 정점들)의 끝까지 잇는다 — 뼈대에는 끝 본이 어디서 끝나는지가 적혀 있지 않다.
	 * 스킨 웨이트가 없는 본(IK 본, 장식 사슬을 묶는 루트 본)은 뼈로 치지 않는다. 치면 루트에서 골반으로, 골반 한가운데서 치마 본으로
	 * 가는 선이 살을 붙잡는다.
	 *
	 * 굵기는 메시에서 잰다: 선 위의 점마다 살갗까지 거리 × RadiusScale. 살 한가운데서 굵고 살갗에 다가갈수록 가늘어져 겉으로
	 * 나오지 않는다. 메시 밖을 지나는 구간에는 뼈가 없다.
	 *
	 * 갈비뼈·골반·머리뼈처럼 실제로는 있지만 뼈대에 본이 없는 넓은 뼈는 여기서 만들지 않는다. 그 자리가 덜 출렁이고 덜 눌려야
	 * 하면 칠(슬롯 마스크)로 정한다.
	 * PerBoneScale: 본 이름 → 굵기 비율. 0 이하면 그 본(과 아래 본)을 뼈대에서 뺀다(ADR-0012 덩어리, 장식 본도). 미기재 본은 RadiusScale.
	 * VolumeMD: 속 메시(ADR-0028). 있으면 살갗까지 거리(굵기)와 안팎을 이 메시에서 잰다. 본 선과 웨이트는 Mesh에서 읽는다.
	 * FleshSlots: 살 슬롯(일지 239). 비어 있지 않으면 웨이트·끝 본의 살을 이 슬롯 정점에서만 센다 — 다른 슬롯만 움직이는 본(머리카락 가닥,
	 *   따로 붙은 장식)은 뼈가 아니다. 가닥 속의 구가 가닥이 얹힌 살(가슴 윗부분)의 살 두께와 출렁임 고정점이 되었다(시험 캐릭터: 가슴 정점의
	 *   97%가 머리카락 구를 가장 가까운 뼈로 재 살 두께 중앙값 3.1 cm, 빼면 10.4 cm).
	 * @return 뼈로 친 선의 수
	 */
	int32 CollectSkeletonCoreSpheres(USkeletalMesh* Mesh, TArray<FVector4>& OutSpheres, float RadiusScale,
		const TMap<FName, float>* PerBoneScale = nullptr, const FMeshDescription* VolumeMD = nullptr, const TSet<FName>* FleshSlots = nullptr)
	{
		const FMeshDescription* MD = Mesh->GetMeshDescription(0);
		if (!MD)
		{
			return 0;
		}
		FSkeletalMeshConstAttributes MeshAttrs(*MD);
		FSkinWeightsVertexAttributesConstRef SkinWeights = MeshAttrs.GetVertexSkinWeights();
		if (!SkinWeights.IsValid())
		{
			UE_LOG(LogSoftbodyBake, Warning, TEXT("Rigid core: the mesh has no skin weights, so no rigid core can be built from its bones."));
			return 0;
		}
		const TVertexAttributesConstRef<FVector3f> Positions = MeshAttrs.GetVertexPositions();
		const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
		const int32 NumBones = RefSkel.GetNum();
		TArray<FTransform> CompPose;
		BuildRefPoseComponentSpace(RefSkel, CompPose);

		// 살 슬롯의 정점(일지 239). 슬롯 이름이 폴리곤 그룹 이름과 하나도 맞지 않으면 메시 전체로 센다(전과 같음).
		bool bUnmatched = false;
		const TArray<bool> InFlesh = FleshVertexMask(*MD, FleshSlots, bUnmatched);
		if (bUnmatched)
		{
			UE_LOG(LogSoftbodyBake, Warning,
				TEXT("Rigid core: no triangle belongs to the slots with a Softbody material (the mesh's polygon group names differ from its slot names), so every slot counts as flesh."));
		}

		// 본마다 걸린 스킨 웨이트의 합(정점 하나치 = 1)과, 그 본이 가장 큰 웨이트인 정점들(끝 본의 살) — 살 슬롯의 정점만.
		// AllWeightSum은 모든 슬롯(살 슬롯이 아니라 빠진 본을 알리는 데만).
		TArray<float> WeightSum;
		WeightSum.Init(0.f, NumBones);
		TArray<float> AllWeightSum;
		AllWeightSum.Init(0.f, NumBones);
		TArray<TArray<FVector3d>> Owned;
		Owned.SetNum(NumBones);
		for (const FVertexID VertID : MD->Vertices().GetElementIDs())
		{
			const bool bFlesh = InFlesh[VertID.GetValue()];
			int32 Best = INDEX_NONE;
			float BestW = 0.f;
			for (UE::AnimationCore::FBoneWeight BW : SkinWeights.Get(VertID))
			{
				const int32 Bone = (int32)BW.GetBoneIndex();
				if (WeightSum.IsValidIndex(Bone))
				{
					AllWeightSum[Bone] += BW.GetWeight();
					if (!bFlesh)
					{
						continue;
					}
					WeightSum[Bone] += BW.GetWeight();
					if (BW.GetWeight() > BestW)
					{
						BestW = BW.GetWeight();
						Best = Bone;
					}
				}
			}
			if (Best != INDEX_NONE)
			{
				Owned[Best].Add(FVector3d(Positions[VertID]));
			}
		}

		// 뼈대에서 뺀 본(아래 본까지 또는 그 본만)과 본별 굵기 비율. 부모가 늘 앞 번호라 한 번 훑으면 된다.
		TArray<bool> bRemoved;
		bRemoved.Init(false, NumBones);
		TArray<bool> bRemovedBelow;
		bRemovedBelow.Init(false, NumBones);
		TArray<double> Scale;
		Scale.Init((double)RadiusScale, NumBones);
		for (int32 Bone = 1; Bone < NumBones; ++Bone)
		{
			bRemovedBelow[Bone] = bRemovedBelow[RefSkel.GetParentIndex(Bone)];
			bRemoved[Bone] = bRemovedBelow[Bone];
			if (const float* Override = PerBoneScale ? PerBoneScale->Find(RefSkel.GetBoneName(Bone)) : nullptr)
			{
				bRemoved[Bone] |= SoftbodyBakeShared::IsBoneRemoved(*Override);
				bRemovedBelow[Bone] |= SoftbodyBakeShared::IsBoneRemovedBelow(*Override);
				Scale[Bone] = (double)*Override;
			}
		}
		const auto IsBone = [&WeightSum, &bRemoved](int32 Bone)
		{
			return WeightSum[Bone] >= 1.f && !bRemoved[Bone];
		};

		// 살갗까지 거리를 잴 메시. 측정 베이크가 살로 채우는 것과 같은 안팎 기준(winding number 0.5)을 쓴다 — 속 메시가 있으면 그 메시.
		FDynamicMesh3 Solid;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.bDisableAttributes = true;
		Converter.Convert(VolumeMD ? VolumeMD : MD, Solid);
		FDynamicMeshAABBTree3 BVH(&Solid, true);
		TFastWindingTree<FDynamicMesh3> Winding(&BVH, true);
		const double Size = Solid.GetBounds().MaxDim();
		// 선을 따라 구를 놓는 간격: 반지름의 절반(이웃 구 사이가 3% 넘게 패이지 않는다), 키 170 cm 몸에서 0.25~1 cm 사이.
		const double MinStep = Size * 0.0015;
		const double MaxStep = Size * 0.006;

		int32 NumSamples = 0;
		const int32 FirstSphere = OutSpheres.Num();
		const auto AddLine = [&](const FVector3d& From, const FVector3d& To, double LineScale)
		{
			const double Length = FVector3d::Distance(From, To);
			for (double T = 0.0;;)
			{
				const FVector3d P = Length > UE_DOUBLE_SMALL_NUMBER ? FMath::Lerp(From, To, T / Length) : From;
				double Radius = 0.0;
				if (Winding.FastWindingNumber(P) > 0.5)
				{
					double DistanceSq = 0.0;
					BVH.FindNearestTriangle(P, DistanceSq);
					Radius = LineScale * FMath::Sqrt(DistanceSq);
				}
				++NumSamples;
				if (Radius > 0.0)
				{
					OutSpheres.Add(FVector4(P, Radius));
				}
				if (T >= Length)
				{
					break;
				}
				T = FMath::Min(T + FMath::Clamp(0.5 * Radius, MinStep, MaxStep), Length);
			}
		};

		int32 NumLines = 0;
		int32 NumEnds = 0;
		TArray<bool> bHasLine;
		bHasLine.Init(false, NumBones);
		FString Unjoined;
		int32 NumUnjoined = 0;
		for (int32 Child = 1; Child < NumBones; ++Child)
		{
			const int32 Parent = RefSkel.GetParentIndex(Child);
			if (!IsBone(Child))
			{
				continue;
			}
			if (!IsBone(Parent))
			{
				// 맨 위 본(골반)이 아닌데 부모와 이어지지 않은 본: 장식 사슬이면 맞고, 몸의 본이면 뼈가 여기서 끊긴다.
				if (!bRemoved[Parent] && WeightSum[Parent] > 0.f)
				{
					Unjoined += FString::Printf(TEXT("%s%s"), NumUnjoined > 0 ? TEXT(", ") : TEXT(""), *RefSkel.GetBoneName(Child).ToString());
					++NumUnjoined;
				}
				continue;
			}
			AddLine(CompPose[Parent].GetLocation(), CompPose[Child].GetLocation(), Scale[Parent]);
			bHasLine[Parent] = true;
			++NumLines;
		}
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (!IsBone(Bone) || bHasLine[Bone] || Owned[Bone].Num() == 0)
			{
				continue;
			}
			const FVector3d Joint = CompPose[Bone].GetLocation();
			FVector3d Center = FVector3d::ZeroVector;
			for (const FVector3d& P : Owned[Bone])
			{
				Center += P;
			}
			FVector3d Direction = Center / (double)Owned[Bone].Num() - Joint;
			if (!Direction.Normalize())
			{
				continue;
			}
			double Extent = 0.0;
			for (const FVector3d& P : Owned[Bone])
			{
				Extent = FMath::Max(Extent, (P - Joint).Dot(Direction));
			}
			AddLine(Joint, Joint + Direction * Extent, Scale[Bone]);
			++NumLines;
			++NumEnds;
		}

		UE_LOG(LogSoftbodyBake, Display, TEXT("Rigid core: %d bone lines (%d end bones) -> %d spheres, samples dropped for lying outside the mesh %d / %d"),
			NumLines, NumEnds, OutSpheres.Num() - FirstSphere, NumSamples - (OutSpheres.Num() - FirstSphere), NumSamples);
		if (NumUnjoined > 0)
		{
			UE_LOG(LogSoftbodyBake, Display,
				TEXT("Rigid core: %d bones hang from a parent with almost no skin weight, so their bone line is not joined to the parent - %s"), NumUnjoined, *Unjoined);
		}
		// 살 슬롯이 아니라 빠진 본(일지 239): 맨 위 본만 이름을 적는다(머리카락 사슬 수십 개를 다 적지 않게).
		int32 NumOther = 0;
		FString OtherRoots;
		int32 NumOtherRoots = 0;
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (bRemoved[Bone] || WeightSum[Bone] >= 1.f || AllWeightSum[Bone] < 1.f)
			{
				continue;
			}
			++NumOther;
			const int32 Parent = RefSkel.GetParentIndex(Bone);
			const bool bParentOther = Parent != INDEX_NONE && !bRemoved[Parent] && WeightSum[Parent] < 1.f && AllWeightSum[Parent] >= 1.f;
			if (!bParentOther && NumOtherRoots++ < 8)
			{
				OtherRoots += (OtherRoots.IsEmpty() ? TEXT("") : TEXT(", ")) + RefSkel.GetBoneName(Bone).ToString();
			}
		}
		if (NumOther > 0)
		{
			UE_LOG(LogSoftbodyBake, Display,
				TEXT("Rigid core: %d bones move only slots without a Softbody material (hair, accessories), so they are not bone inside the flesh - from %s%s"),
				NumOther, *OtherRoots, NumOtherRoots > 8 ? TEXT(", ...") : TEXT(""));
		}
		return NumLines;
	}

	/** 관성 그룹 번호 → 스킨 웨이트 합. 겹친 층 연결표(ADR-0014)로 섞을 수 있게 합·배율을 둔다. */
	struct FGroupWeights
	{
		TArray<TPair<int32, float>, TInlineAllocator<4>> Weights;

		void Add(int32 Group, float Weight)
		{
			for (TPair<int32, float>& Pair : Weights)
			{
				if (Pair.Key == Group)
				{
					Pair.Value += Weight;
					return;
				}
			}
			Weights.Emplace(Group, Weight);
		}

		FGroupWeights operator*(float Scale) const
		{
			FGroupWeights Scaled = *this;
			for (TPair<int32, float>& Pair : Scaled.Weights)
			{
				Pair.Value *= Scale;
			}
			return Scaled;
		}

		FGroupWeights& operator+=(const FGroupWeights& Other)
		{
			for (const TPair<int32, float>& Pair : Other.Weights)
			{
				Add(Pair.Key, Pair.Value);
			}
			return *this;
		}
	};

	/**
	 * 관성 그룹 빌드: 소유 정점 수 상위 MaxGroups 본을 드라이버로 고르고 나머지는 부모
	 * 체인으로 병합한다. 정점의 모든 스킨 웨이트를 그룹별로 합산해 큰 것 셋을 저장하므로
	 * 관절에서 지배 본 하나를 고를 때 생기는 경계를 완화한다.
	 * OutTex2 = 정점마다 그룹 셋과 몫(SoftbodyBakeShared::FVertexGroups의 자리).
	 * OutGroupReach = 그룹 본(기본 포즈)에서 그 그룹이 움직이는 정점까지의 최대 거리 — 회전 흔들림 상한용.
	 * 덩어리(ADR-0012): RigidShrinkPerBone에서 뼈대로부터 뺀 본(0 이하) 중 부모 쪽에 뺀 본이 없는 본은 루트 다음으로 먼저 그룹이
	 * 되고(OutGroupLump 참), 그 아래 본은 따로 그룹이 되지 않고 덩어리에 합쳐진다. 그 본만 뺀 본(BoneExcludedAlone)은 제 살만 덩어리이고
	 * 아래 본은 뼈로 남아 덩어리에 합쳐지지 않는다(일지 236).
	 * FleshSlots: 코어와 같은 살 슬롯(FleshVertexMask). 소유 정점 수와 닿는 거리를 이 슬롯 정점으로만 센다 — 다른 슬롯만 움직이는
	 * 머리카락·치마 본이 묶음 칸을 차지해 팔·척추가 빠지거나, 흔들리지 않는 머리카락 끝까지 잰 거리가 그 묶음 살을 조이지 않게(일지 378).
	 */
	bool BuildInertiaGroups(USkeletalMesh* Mesh, const FMeshDescription& MD, const FBakeCoreOut& Core,
		int32 MaxGroups, const TMap<FName, float>* RigidShrinkPerBone, TArray<FName>& OutGroupBones,
		TArray<bool>& OutGroupLump, TArray<FFloat16Color>& OutTex2, TArray<float>& OutGroupReach, const TSet<FName>* FleshSlots)
	{
		FSkeletalMeshConstAttributes MeshAttrs(MD);
		FSkinWeightsVertexAttributesConstRef SkinWeights = MeshAttrs.GetVertexSkinWeights();
		if (!SkinWeights.IsValid())
		{
			UE_LOG(LogSoftbodyBake, Warning, TEXT("Inertia groups: the mesh has no skin weights, so no inertia groups are made."));
			return false;
		}
		const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
		bool bUnmatched = false;
		const TArray<bool> InFlesh = FleshVertexMask(MD, FleshSlots, bUnmatched);

		// 정점별 지배 본 + 본별 소유 정점 수(살 슬롯 정점만)
		const int32 MaxVertID = MD.Vertices().GetArraySize();
		TArray<int32> DomBone;
		DomBone.Init(INDEX_NONE, MaxVertID);
		TMap<int32, int32> BoneVertCount;
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			if (!InFlesh[VertID.GetValue()])
			{
				continue;
			}
			int32 Best = INDEX_NONE;
			float BestW = 0.f;
			for (UE::AnimationCore::FBoneWeight BW : SkinWeights.Get(VertID))
			{
				if (BW.GetWeight() > BestW)
				{
					BestW = BW.GetWeight();
					Best = (int32)BW.GetBoneIndex();
				}
			}
			DomBone[VertID] = Best;
			if (Best != INDEX_NONE)
			{
				++BoneVertCount.FindOrAdd(Best);
			}
		}
		if (BoneVertCount.Num() == 0)
		{
			UE_LOG(LogSoftbodyBake, Warning, TEXT("Inertia groups: no vertex has a skin weight on a usable bone, so no inertia groups are made and the mesh does not jiggle."));
			return false;
		}

		// 소유 정점 수 상위 MaxGroups 본 = 그룹 (동수 시 본 인덱스 순 — 결정적)
		TArray<TPair<int32, int32>> Ranked;
		for (const TPair<int32, int32>& P : BoneVertCount)
		{
			Ranked.Emplace(P.Key, P.Value);
		}
		Ranked.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B)
		{
			return A.Value != B.Value ? A.Value > B.Value : A.Key < B.Key;
		});
		TMap<int32, int32> BoneToGroup;
		OutGroupBones.Reset();
		OutGroupLump.Reset();
		// 그룹 예산 밖의 본도 임의의 사지 그룹이 아니라 스켈레톤 기준으로 합쳐지게
		// 루트를 항상 0번 그룹으로 보존한다.
		if (RefSkel.GetNum() > 0 && MaxGroups > 0)
		{
			BoneToGroup.Add(0, 0);
			OutGroupBones.Add(RefSkel.GetBoneName(0));
			OutGroupLump.Add(false);
		}

		// 덩어리 본: 뼈대에서 뺀 본의 사슬 맨 위. 정점 수와 상관없이 먼저 그룹이 되어야 배·골반 그룹에 섞이지 않는다
		// (섞이면 엉덩이를 누른 힘이 골반 그룹 전체 — 아랫배까지 — 를 움직인다).
		// 그 본만 뺀 본(Include Child Bones 끔)은 제 살만 덩어리이고 아래 본은 덩어리가 아니다 — 뼈대(CollectSkeletonCoreSpheres)와 같은 규칙.
		// 장식 본(Accessory)은 덩어리가 아니고 제 묶음도 갖지 않는다 — 그 정점은 부모 쪽 묶음을 따른다(ResolveGroup이 위로 올라간다).
		TSet<int32> Excluded;
		TSet<int32> ExcludedBelow;
		TSet<int32> Accessory;
		TSet<int32> AccessoryBelow;
		if (RigidShrinkPerBone)
		{
			for (const TPair<FName, float>& Pair : *RigidShrinkPerBone)
			{
				const int32 Bone = RefSkel.FindBoneIndex(Pair.Key);
				if (Bone <= 0 || !SoftbodyBakeShared::IsBoneRemoved(Pair.Value))
				{
					continue;
				}
				const bool bBelow = SoftbodyBakeShared::IsBoneRemovedBelow(Pair.Value);
				(SoftbodyBakeShared::IsBoneAccessory(Pair.Value) ? Accessory : Excluded).Add(Bone);
				if (bBelow)
				{
					(SoftbodyBakeShared::IsBoneAccessory(Pair.Value) ? AccessoryBelow : ExcludedBelow).Add(Bone);
				}
			}
		}
		auto IsAccessoryBone = [&Accessory, &AccessoryBelow, &RefSkel](int32 Bone)
		{
			for (int32 Up = Bone; Up != INDEX_NONE; Up = RefSkel.GetParentIndex(Up))
			{
				if (Up == Bone ? Accessory.Contains(Up) : AccessoryBelow.Contains(Up))
				{
					return true;
				}
			}
			return false;
		};
		auto LumpRootOf = [&Excluded, &ExcludedBelow, &RefSkel](int32 Bone) -> int32
		{
			int32 Root = INDEX_NONE;
			for (int32 Up = Bone; Up != INDEX_NONE; Up = RefSkel.GetParentIndex(Up))
			{
				Root = (Up == Bone ? Excluded.Contains(Up) : ExcludedBelow.Contains(Up)) ? Up : Root;
			}
			return Root;
		};
		TMap<int32, int32> LumpVertCount;
		for (const TPair<int32, int32>& P : BoneVertCount)
		{
			const int32 Root = LumpRootOf(P.Key);
			if (Root != INDEX_NONE)
			{
				LumpVertCount.FindOrAdd(Root) += P.Value;
			}
		}
		TArray<int32> LumpRoots;
		LumpVertCount.GetKeys(LumpRoots);
		LumpRoots.Sort();
		for (const int32 Bone : LumpRoots)
		{
			if (OutGroupBones.Num() >= MaxGroups)
			{
				UE_LOG(LogSoftbodyBake, Warning, TEXT("Inertia groups: all %d groups are taken, so lump bone %s gets no group of its own. Raise Max Inertia Groups."),
					MaxGroups, *RefSkel.GetBoneName(Bone).ToString());
				continue;
			}
			BoneToGroup.Add(Bone, OutGroupBones.Num());
			OutGroupBones.Add(RefSkel.GetBoneName(Bone));
			OutGroupLump.Add(true);
		}
		for (int32 i = 0; i < Ranked.Num() && OutGroupBones.Num() < MaxGroups; ++i)
		{
			if (BoneToGroup.Contains(Ranked[i].Key) || LumpRootOf(Ranked[i].Key) != INDEX_NONE || IsAccessoryBone(Ranked[i].Key))
			{
				continue;
			}
			BoneToGroup.Add(Ranked[i].Key, OutGroupBones.Num());
			OutGroupBones.Add(RefSkel.GetBoneName(Ranked[i].Key));
			OutGroupLump.Add(false);
		}

		// 스킨 본 → 그룹 (자신 또는 부모 체인의 첫 그룹 본; 없으면 최대 그룹 0). 그 본만 뺀 덩어리는 아래 본(뼈로 남은 본)의 살을 받지 않는다.
		auto ResolveGroup = [&BoneToGroup, &RefSkel, &Excluded, &ExcludedBelow](int32 Bone) -> int32
		{
			for (int32 Up = Bone; Up != INDEX_NONE; Up = RefSkel.GetParentIndex(Up))
			{
				const bool bLumpAbove = Up != Bone && Excluded.Contains(Up) && !ExcludedBelow.Contains(Up);
				const int32* G = BoneToGroup.Find(Up);
				if (G && !bLumpAbove)
				{
					return *G;
				}
			}
			return 0;
		};

		// 그룹 본의 기본 포즈 컴포넌트 공간 위치 (MD 정점과 같은 공간)
		TArray<FVector3f> GroupPivots;
		{
			const TArray<FTransform>& RefLocalPose = RefSkel.GetRefBonePose();
			TArray<FTransform> RefComponentPose;
			RefComponentPose.SetNum(RefSkel.GetNum());
			for (int32 Bone = 0; Bone < RefSkel.GetNum(); ++Bone)
			{
				const int32 Parent = RefSkel.GetParentIndex(Bone);
				RefComponentPose[Bone] = Parent >= 0
					? RefLocalPose[Bone] * RefComponentPose[Parent]
					: RefLocalPose[Bone];
			}
			for (const FName& Bone : OutGroupBones)
			{
				const int32 BoneIndex = RefSkel.FindBoneIndex(Bone);
				GroupPivots.Add(BoneIndex != INDEX_NONE
					? FVector3f(RefComponentPose[BoneIndex].GetLocation())
					: FVector3f::ZeroVector);
			}
		}
		OutGroupReach.Init(0.f, OutGroupBones.Num());
		const TVertexAttributesConstRef<FVector3f> Positions = MeshAttrs.GetVertexPositions();

		OutTex2.Init(FFloat16Color(FLinearColor::Black), Core.TexSize * Core.TexSize);
		// 정점(텍셀)마다 그룹 무게. 겹친 층(ADR-0014)은 따르는 살 정점들의 그룹 무게를 섞어 쓴다 — 속옷과 그 아래 살이 같은
		// 본 그룹을 같은 비율로 따라 흔들린다.
		TArray<FGroupWeights> TexelGroups;
		TexelGroups.SetNum(Core.TexSize * Core.TexSize);
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			const int32 Texel = Core.VertTexel[VertID];
			if (TexelGroups.IsValidIndex(Texel))
			{
				for (UE::AnimationCore::FBoneWeight BW : SkinWeights.Get(VertID))
				{
					TexelGroups[Texel].Add(ResolveGroup((int32)BW.GetBoneIndex()), BW.GetWeight());
				}
			}
		}
		SoftbodyLayers::Apply(Core.LayerLinks, TexelGroups, FGroupWeights());
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			const int32 Texel = Core.VertTexel[VertID];
			TArray<TPair<int32, float>, TInlineAllocator<8>> RankedGroups;
			if (TexelGroups.IsValidIndex(Texel))
			{
				for (const TPair<int32, float>& Pair : TexelGroups[Texel].Weights)
				{
					RankedGroups.Emplace(Pair.Key, Pair.Value);
				}
			}
			RankedGroups.Sort([](const TPair<int32, float>& A, const TPair<int32, float>& B)
			{
				return !FMath::IsNearlyEqual(A.Value, B.Value)
					? A.Value > B.Value
					: A.Key < B.Key;
			});
			// 큰 것 셋을 남기고 셋의 합으로 나눈다(FVertexGroups). 묶음이 모자란 자리는 첫째 묶음에 몫 0으로 채운다.
			SoftbodyBakeShared::FVertexGroups Kept;
			const int32 NumKept = FMath::Min(RankedGroups.Num(), (int32)SoftbodyBakeShared::FVertexGroups::Num);
			float KeptWeight = 0.f;
			for (int32 k = 0; k < NumKept; ++k)
			{
				KeptWeight += RankedGroups[k].Value;
			}
			const bool bWeighted = KeptWeight > UE_KINDA_SMALL_NUMBER;
			Kept.Group[0] = NumKept > 0 ? RankedGroups[0].Key : 0;
			for (int32 k = 0; k < SoftbodyBakeShared::FVertexGroups::Num; ++k)
			{
				const bool bUsed = bWeighted && k < NumKept;
				Kept.Group[k] = bUsed ? RankedGroups[k].Key : Kept.Group[0];
				Kept.Weight[k] = bUsed ? RankedGroups[k].Value / KeptWeight : (k == 0 ? 1.f : 0.f);
			}
			if (Texel != INDEX_NONE)
			{
				OutTex2[Texel] = FFloat16Color(Kept.Encode());
			}
			// 닿는 거리는 첫째·둘째로 걸린 정점까지만 잰다. 셋째는 몫이 가장 작은 묶음이라(대개 몇 %), 그런 정점까지 재면 멀리서
			// 살짝 걸린 정점 하나가 그 부위의 조임(SaturateInertiaLoad)을 키운다. 살 슬롯 밖 정점(흔들리지 않는다)은 재지 않는다.
			for (int32 k = 0; k < 2 && InFlesh[VertID.GetValue()]; ++k)
			{
				OutGroupReach[Kept.Group[k]] = FMath::Max(OutGroupReach[Kept.Group[k]],
					FVector3f::Distance(Positions[VertID], GroupPivots[Kept.Group[k]]));
			}
		}

		FString GroupLog;
		for (int32 g = 0; g < OutGroupBones.Num(); ++g)
		{
			GroupLog += FString::Printf(TEXT("%s%d:%s(%.0f)%s"), g > 0 ? TEXT(", ") : TEXT(""), g,
				*OutGroupBones[g].ToString(), OutGroupReach[g], OutGroupLump[g] ? TEXT(" lump") : TEXT(""));
		}
		UE_LOG(LogSoftbodyBake, Display, TEXT("%d inertia groups (in parentheses: distance from the bone to its farthest vertex): %s"),
			OutGroupBones.Num(), *GroupLog);
		return true;
	}

	/** 스켈레탈 측정 베이크 경로 (스태틱과 대칭) */
	USoftbodyMeshData* BakeSkeletalInternal(USkeletalMesh* Mesh, const FString& OutputFolder,
		const FSoftbodyMeasureParams& Measure, int32 MaxInertiaGroups, const TMap<FName, float>* RigidShrinkPerBone,
		const TSet<FName>* FleshSlots)
	{
		if (!Mesh)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("No mesh given. Set Target Mesh and bake again."));
			return nullptr;
		}
		if (!SoftbodyBakeShared::IsEditableContent(Mesh))
		{
			UE_LOG(LogSoftbodyBake, Error,
				TEXT("%s is engine content, and baking writes into the mesh (it adds UV channels). Duplicate the mesh into your project's "
					"Content folder and bake the copy."), *Mesh->GetPathName());
			return nullptr;
		}

		FMeshDescription* MD = Mesh->GetMeshDescription(0);
		if (!MD)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("%s has no LOD0 source geometry, so it cannot be baked. Reimport the mesh with its source data."), *Mesh->GetPathName());
			return nullptr;
		}

		const FString MeshName = Mesh->GetName();
		USoftbodyMeshData* Existing = FindExistingData(OutputFolder, MeshName);
		const int32 ReuseChannel = (Existing && Existing->SourceSkeletalMesh == Mesh) ? Existing->TexelUVChannel : INDEX_NONE;

		Mesh->Modify();

		FBakeCoreOut Core;
		// 스켈레탈 렌더 정점 포맷은 MAX_TEXCOORDS(4)까지만 UV를 지원한다
		if (!RunBakeCore(*MD, ReuseChannel, 4, Core, Measure))
		{
			return nullptr;
		}

		// 관성 그룹 (커밋 전 — MD 스킨 웨이트 + 코어 텍셀 매핑 사용)
		TArray<FName> GroupBones;
		TArray<bool> GroupLump;
		TArray<FFloat16Color> Tex2;
		TArray<float> GroupReach;
		if (MaxInertiaGroups > 0)
		{
			BuildInertiaGroups(Mesh, *MD, Core, MaxInertiaGroups, RigidShrinkPerBone, GroupBones, GroupLump, Tex2, GroupReach, FleshSlots);
		}

		// 덩어리 국소 모양의 재료(SoftbodyLumpRegion): 덩어리마다 원판 누름을 무름과 같은 분해로 풀고 분해를 놓는다(캐리어 굽기가 칠·덩어리
		// 양을 넣어 마무리한다). 정점의 덩어리 묶음과 몫은 위 그룹 표에서 캐리어 굽기(ReadVertexLumps)와 같은 규칙으로 읽는다.
		TArray<FSoftbodyLumpDiscBake> LumpDiscs;
		if (Core.MeasureSolver.IsValid() && Tex2.Num() >= Core.NumVertices && GroupLump.Contains(true))
		{
			const auto IsLump = [&GroupLump](int32 Group) { return GroupLump.IsValidIndex(Group) && GroupLump[Group]; };
			TArray<int32> VertexLump;
			TArray<float> VertexLumpWeight;
			VertexLump.Init(INDEX_NONE, Core.NumVertices);
			VertexLumpWeight.Init(0.f, Core.NumVertices);
			for (int32 v = 0; v < Core.NumVertices; ++v)
			{
				VertexLump[v] = SoftbodyBakeShared::FVertexGroups::Decode(Tex2[v].GetFloats()).PickLump(IsLump, VertexLumpWeight[v]);
			}
			SolveSoftbodyLumpDiscs(*Core.MeasureSolver, *MD, Core.VertTexel, Core.VertexAreas, VertexLump, VertexLumpWeight, Core.LayerLinks,
				LumpDiscs);
		}
		Core.MeasureSolver.Reset();

		// 하프 정밀도 UV의 텍셀 중심 양자화 오차 예방 (베이크 시간 선택이므로 항상 켠다)
		if (FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(0))
		{
			LODInfo->BuildSettings.bUseFullPrecisionUVs = true;
		}

		SoftbodyBakeShared::WriteLodTexelChannels(Mesh, *MD, Core.VertTexel, Core.TexSize, Core.TexelChannel, 4);
		Mesh->CommitMeshDescription(0);
		Mesh->PostEditChange(); // 렌더 데이터 재빌드
		Mesh->MarkPackageDirty();

		USoftbodyMeshData* Data = CommitBakeOutputs(OutputFolder, MeshName, Existing, Core);
		Data->SourceMesh = nullptr;
		Data->SourceSkeletalMesh = Mesh;
		Data->VolumeMesh = Measure.VolumeMesh;
		Data->AttributeTex2 = Tex2.Num() > 0
			? CreateAttributeTexture(OutputFolder, FString::Printf(TEXT("T_%s_Attr2"), *MeshName), Core.TexSize, Tex2)
			: nullptr;
		Data->InertiaGroupBones = MoveTemp(GroupBones);
		Data->InertiaGroupLump = MoveTemp(GroupLump);
		Data->InertiaGroupReach = MoveTemp(GroupReach);
		// 덩어리 값은 캐리어 굽기가 다시 만든다(그룹 번호가 바뀌었을 수 있다). 국소 모양도 새 원판 누름으로 다시 마무리한다.
		Data->CarrierLumpTex = nullptr;
		Data->CarrierLumpSideATex = nullptr;
		Data->CarrierLumpSideBTex = nullptr;
		Data->InertiaLumps.Reset();
		Data->LumpDiscBakes = MoveTemp(LumpDiscs);
		Data->CarrierLumpRegionIndexTex = nullptr;
		Data->CarrierLumpRegionTex = nullptr;
		Data->LumpRegionVersion = 0;
		Data->LumpRegionRecords = 0;

		// 페인트 층 초기 채움 (M6 §8.2) — 저작 마스크는 GeneratePaintTexture 재호출로 적용
		USoftbodyBakeLibrary::GeneratePaintTexture(Data, nullptr);

		UE_LOG(LogSoftbodyBake, Display,
			TEXT("Skeletal bake done: %s - %d vertices, %d vertex instances, texture %d², UV channel %d, %d inertia groups"),
			*MeshName, Core.NumVertices, Core.NumInstances, Core.TexSize, Core.TexelChannel,
			Data->InertiaGroupBones.Num());
		return Data;
	}
}

namespace SoftbodyBakeShared
{
	UTexture2D* CreateTexture16F(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<FFloat16Color>& Texels)
	{
		return CreateAttributeTextureRect(OutputFolder, Name, Width, Height, Texels);
	}

	UTexture2D* CreateTexture32F(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<FLinearColor>& Texels)
	{
		const FString PackageName = OutputFolder / Name;
		UPackage* Pkg = BakeTexturePackage(PackageName, Name);
		UTexture2D* Tex = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		Tex->Source.Init(Width, Height, 1, 1, TSF_RGBA32F, (const uint8*)Texels.GetData());
		Tex->CompressionSettings = TC_HDR_F32;
		Tex->SRGB = false;
		Tex->Filter = TF_Nearest;
		Tex->MipGenSettings = TMGS_NoMipmaps;
		Tex->NeverStream = true;
		Tex->UpdateResource();
		Tex->PostEditChange();
		FAssetRegistryModule::AssetCreated(Tex);
		Pkg->MarkPackageDirty();
		return Tex;
	}

	UTexture2D* CreateTextureG8(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<uint8>& Texels)
	{
		const FString PackageName = OutputFolder / Name;
		UPackage* Pkg = BakeTexturePackage(PackageName, Name);
		UTexture2D* Tex = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		Tex->Source.Init(Width, Height, 1, 1, TSF_G8, Texels.GetData());
		Tex->CompressionSettings = TC_Grayscale;
		Tex->SRGB = false;
		Tex->Filter = TF_Nearest;
		Tex->MipGenSettings = TMGS_NoMipmaps;
		Tex->NeverStream = true;
		Tex->UpdateResource();
		Tex->PostEditChange();
		FAssetRegistryModule::AssetCreated(Tex);
		Pkg->MarkPackageDirty();
		return Tex;
	}

	UTexture2D* CreateTextureG16(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<uint16>& Texels)
	{
		const FString PackageName = OutputFolder / Name;
		UPackage* Pkg = BakeTexturePackage(PackageName, Name);
		UTexture2D* Tex = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
		Tex->Source.Init(Width, Height, 1, 1, TSF_G16, (const uint8*)Texels.GetData());
		// 16비트 회색조 원본은 TC_Grayscale에서 G16 그대로 올라간다(8비트로 줄지 않는다). 셰이더는 unorm으로 읽어 65535를 곱한다.
		Tex->CompressionSettings = TC_Grayscale;
		Tex->CompressionNone = true;
		Tex->SRGB = false;
		Tex->Filter = TF_Nearest;
		Tex->MipGenSettings = TMGS_NoMipmaps;
		Tex->NeverStream = true;
		Tex->UpdateResource();
		Tex->PostEditChange();
		FAssetRegistryModule::AssetCreated(Tex);
		Pkg->MarkPackageDirty();
		return Tex;
	}

	int32 CollectCoreSpheres(USkeletalMesh* Mesh, TArray<FVector4>& OutSpheres, float RadiusScale,
		const TMap<FName, float>* PerBoneScale, const FMeshDescription* VolumeMD, const TSet<FName>* FleshSlots)
	{
		return Mesh ? CollectSkeletonCoreSpheres(Mesh, OutSpheres, RadiusScale, PerBoneScale, VolumeMD, FleshSlots) : 0;
	}

	int32 CollectStaticCoreSpheres(const FMeshDescription& MD, bool bFloor, float CellSizeFrac, float RadiusScale,
		TArray<FVector4>& OutSpheres)
	{
		// 측정 베이크가 살로 채우는 것과 같은 안팎(winding number 0.5)과 같은 칸(바운드 가장 긴 변 × 비율)
		FDynamicMesh3 Solid;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.bDisableAttributes = true;
		Converter.Convert(&MD, Solid);
		if (Solid.TriangleCount() == 0)
		{
			return 0;
		}
		const FAxisAlignedBox3d Bounds = Solid.GetBounds();
		const double Size = Bounds.MaxDim();
		const double Cell = FMath::Max(Size * (double)CellSizeFrac, 0.01);
		const int32 First = OutSpheres.Num();

		if (bFloor)
		{
			// 바닥에 놓인 물체: 가장 낮은 곳에서 칸 절반 위까지 붙든다 — 바닥면의 격자 점 한 겹이 속 안에 들고, 속의 윗선이 보이는 옆면을
			// 가로지르지 않는다(일지 230). 평평한 판은 아주 큰 구의 윗부분이다: 크기의 1000배면 가장 먼 모서리에서 물러나는 양이 크기의
			// 1/2000 안이라 칸 절반보다 훨씬 작다.
			const double Radius = 1000.0 * Size;
			const FVector3d Center = Bounds.Center();
			OutSpheres.Add(FVector4(Center.X, Center.Y, Bounds.Min.Z + 0.5 * Cell - Radius, Radius));
			UE_LOG(LogSoftbodyBake, Display, TEXT("Rigid core (Bottom): the bottom %.2f cm (half a measure cell) is held"), 0.5 * Cell);
			return 1;
		}

		// Center: 반 칸 간격의 점마다 가장 가까운 겉의 점과 그쪽에서 오는 방향을 잰다. 이웃 점과 방향이 반대 쪽(100도 넘게)으로 바뀌면 두 점
		// 사이로 모양의 한가운데가 지나간다. 직각으로 만나는 두 면(상자 모서리 쪽 갈래)은 90도라 넣지 않는다 — 판의 속은 가운데 면 하나다.
		FDynamicMeshAABBTree3 BVH(&Solid, true);
		TFastWindingTree<FDynamicMesh3> Winding(&BVH, true);
		const double Step = 0.5 * Cell;
		const FVector3d Origin = Bounds.Min + FVector3d(0.5 * Step);
		const int32 NX = FMath::Max(1, FMath::FloorToInt32(Bounds.Width() / Step));
		const int32 NY = FMath::Max(1, FMath::FloorToInt32(Bounds.Height() / Step));
		const int32 NZ = FMath::Max(1, FMath::FloorToInt32(Bounds.Depth() / Step));
		const int64 Count = (int64)NX * NY * NZ;
		if (Count > 16 * 1024 * 1024)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("Rigid core (Center): %lld sample points are too many. Raise Measure Cell Size."), Count);
			return 0;
		}
		const auto PointAt = [&](int32 X, int32 Y, int32 Z) { return Origin + FVector3d(X, Y, Z) * Step; };
		TArray<FVector3f> Away;
		Away.SetNumZeroed((int32)Count);
		TArray<float> Depth;
		Depth.Init(-1.f, (int32)Count);
		ParallelFor(NZ, [&](int32 Z)
		{
			for (int32 Y = 0; Y < NY; ++Y)
			{
				for (int32 X = 0; X < NX; ++X)
				{
					const FVector3d P = PointAt(X, Y, Z);
					if (Winding.FastWindingNumber(P) <= 0.5)
					{
						continue;
					}
					const FVector3d Offset = P - BVH.FindNearestPoint(P);
					const double Distance = Offset.Length();
					if (Distance > UE_DOUBLE_SMALL_NUMBER)
					{
						const int32 Index = (Z * NY + Y) * NX + X;
						Away[Index] = FVector3f(Offset / Distance);
						Depth[Index] = (float)Distance;
					}
				}
			}
		});

		struct FMiddle
		{
			FVector3d P;
			double Depth;
		};
		TArray<FMiddle> Middle;
		int32 Deepest = INDEX_NONE;
		for (int32 Z = 0; Z < NZ; ++Z)
		{
			for (int32 Y = 0; Y < NY; ++Y)
			{
				for (int32 X = 0; X < NX; ++X)
				{
					const int32 Index = (Z * NY + Y) * NX + X;
					if (Depth[Index] <= 0.f)
					{
						continue;
					}
					if (Deepest == INDEX_NONE || Depth[Index] > Depth[Deepest])
					{
						Deepest = Index;
					}
					// 겉에 가까운 점(반 칸 안)은 가장 가까운 점이 흔들려 넣지 않는다.
					if (Depth[Index] < Step)
					{
						continue;
					}
					const int32 Neighbors[6][3] = { { X - 1, Y, Z }, { X + 1, Y, Z }, { X, Y - 1, Z }, { X, Y + 1, Z }, { X, Y, Z - 1 }, { X, Y, Z + 1 } };
					for (const int32* Q : Neighbors)
					{
						if (Q[0] < 0 || Q[1] < 0 || Q[2] < 0 || Q[0] >= NX || Q[1] >= NY || Q[2] >= NZ)
						{
							continue;
						}
						const int32 Other = (Q[2] * NY + Q[1]) * NX + Q[0];
						if (Depth[Other] > 0.f && FVector3f::DotProduct(Away[Index], Away[Other]) < -0.17f)
						{
							Middle.Add({ PointAt(X, Y, Z), (double)Depth[Index] });
							break;
						}
					}
				}
			}
		}
		if (Deepest == INDEX_NONE)
		{
			return 0;
		}
		if (Middle.Num() == 0)
		{
			// 칸보다 얇은 메시: 가장 깊은 점 하나
			const int32 X = Deepest % NX, Y = (Deepest / NX) % NY, Z = Deepest / (NX * NY);
			Middle.Add({ PointAt(X, Y, Z), (double)Depth[Deepest] });
		}

		// 깊은 점부터 구를 놓고, 이미 놓은 구의 중심에서 그 반지름의 절반 안에 드는 점은 건너뛴다 — 스켈레탈 본 선의 간격(반지름의 절반)과
		// 같아 이웃 구 사이가 몇 % 넘게 패이지 않는다.
		Middle.Sort([](const FMiddle& A, const FMiddle& B) { return A.Depth > B.Depth; });
		const double MaxRadius = RadiusScale * Middle[0].Depth;
		const double Bucket = FMath::Max(0.5 * MaxRadius, Step);
		TMap<FIntVector, TArray<int32>> Kept;
		const auto BucketOf = [&](const FVector3d& P)
		{
			return FIntVector(FMath::FloorToInt32(P.X / Bucket), FMath::FloorToInt32(P.Y / Bucket), FMath::FloorToInt32(P.Z / Bucket));
		};
		for (const FMiddle& M : Middle)
		{
			const double Radius = RadiusScale * M.Depth;
			if (Radius <= UE_DOUBLE_SMALL_NUMBER)
			{
				continue;
			}
			const FIntVector B = BucketOf(M.P);
			bool bCovered = false;
			for (int32 DZ = -1; DZ <= 1 && !bCovered; ++DZ)
			{
				for (int32 DY = -1; DY <= 1 && !bCovered; ++DY)
				{
					for (int32 DX = -1; DX <= 1 && !bCovered; ++DX)
					{
						if (const TArray<int32>* Near = Kept.Find(B + FIntVector(DX, DY, DZ)))
						{
							for (const int32 S : *Near)
							{
								const FVector4& Sphere = OutSpheres[S];
								if (FVector3d::DistSquared(M.P, FVector3d(Sphere.X, Sphere.Y, Sphere.Z)) <= FMath::Square(0.5 * Sphere.W))
								{
									bCovered = true;
									break;
								}
							}
						}
					}
				}
			}
			if (!bCovered)
			{
				Kept.FindOrAdd(B).Add(OutSpheres.Add(FVector4(M.P, Radius)));
			}
		}
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("Rigid core (Center): %d spheres through the middle of the shape (%d middle points, deepest %.2f cm, core radius up to %.2f cm)"),
			OutSpheres.Num() - First, Middle.Num(), Middle[0].Depth, MaxRadius);
		return OutSpheres.Num() - First;
	}

	bool IsEditableContent(const UObject* Asset)
	{
		if (!Asset)
		{
			return false;
		}
		const FString PackageName = Asset->GetOutermost()->GetName();
		if (PackageName.StartsWith(TEXT("/Engine/")))
		{
			return false;
		}
		// 엔진 플러그인의 콘텐츠도 엔진 폴더 아래에 있다.
		FString FileName;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, FileName))
		{
			const FString Full = FPaths::ConvertRelativePathToFull(FileName);
			if (FPaths::IsUnderDirectory(Full, FPaths::ConvertRelativePathToFull(FPaths::EngineDir())))
			{
				return false;
			}
		}
		return true;
	}

	FMeshDescription* OwnLodDescription(UObject* Mesh, int32 Lod)
	{
		if (UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			return Lod < Static->GetNumSourceModels() && !Static->IsReductionActive(Lod) && Static->IsMeshDescriptionValid(Lod)
				? Static->GetMeshDescription(Lod) : nullptr;
		}
		if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			return Lod < Skeletal->GetLODNum() && !Skeletal->IsReductionActive(Lod) && Skeletal->HasMeshDescription(Lod)
				? Skeletal->GetMeshDescription(Lod) : nullptr;
		}
		return nullptr;
	}

	int32 NumLods(const UObject* Mesh)
	{
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			return Static->GetNumSourceModels();
		}
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			return Skeletal->GetLODNum();
		}
		return 0;
	}

	namespace
	{
		/** Channel의 값이 모두 Size² 격자의 칸 가운데인가(텍셀 채널을 전에 쓴 LOD). */
		bool ChannelOnTexelCenters(const FMeshDescription& MD, int32 Channel, int32 Size)
		{
			FStaticMeshConstAttributes Attrs(MD);
			TVertexInstanceAttributesConstRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
			for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
			{
				const FVector2f Value = UVs.Get(Instance, Channel) * (float)Size - FVector2f(0.5f);
				if (FMath::Abs(Value.X - FMath::RoundToFloat(Value.X)) > 1e-3f || FMath::Abs(Value.Y - FMath::RoundToFloat(Value.Y)) > 1e-3f
					|| Value.X < -0.5f || Value.Y < -0.5f || Value.X > Size - 0.5f || Value.Y > Size - 0.5f)
				{
					return false;
				}
			}
			return true;
		}

		/** Channel의 값이 모두 (−1, −1)인가(캐리어 채널을 전에 쓴 LOD). */
		bool ChannelUnset(const FMeshDescription& MD, int32 Channel)
		{
			FStaticMeshConstAttributes Attrs(MD);
			TVertexInstanceAttributesConstRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
			for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
			{
				if (UVs.Get(Instance, Channel) != FVector2f(-1.f, -1.f))
				{
					return false;
				}
			}
			return true;
		}

		void CommitLod(UObject* Mesh, int32 Lod)
		{
			if (UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
			{
				Static->CommitMeshDescription(Lod);
			}
			else if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
			{
				if (FSkeletalMeshLODInfo* Info = Skeletal->GetLODInfo(Lod))
				{
					Info->BuildSettings.bUseFullPrecisionUVs = true;
				}
				Skeletal->CommitMeshDescription(Lod);
			}
		}
	}

	int32 WriteLodTexelChannels(UObject* Mesh, const FMeshDescription& BaseMD, TConstArrayView<int32> BaseVertTexel, int32 TexSize,
		int32 TexelChannel, int32 MaxChannels)
	{
		const int32 Lods = NumLods(Mesh);
		if (Lods <= 1 || TexSize <= 0)
		{
			return 0;
		}
		// LOD0 정점 자리 → 텍셀. 다른 LOD의 정점은 가장 가까운 LOD0 정점의 칸을 읽는다(그 정점의 무름·본 묶음·캐리어 자리).
		FStaticMeshConstAttributes BaseAttrs(BaseMD);
		TVertexAttributesConstRef<FVector3f> BasePositions = BaseAttrs.GetVertexPositions();
		FBox3d Bounds(ForceInit);
		for (const FVertexID Vertex : BaseMD.Vertices().GetElementIDs())
		{
			Bounds += FVector3d(BasePositions[Vertex]);
		}
		const double Cell = FMath::Max(Bounds.GetExtent().GetMax() / 64.0, 0.1);
		UE::Geometry::TPointHashGrid3d<int32> Grid(Cell, INDEX_NONE);
		for (const FVertexID Vertex : BaseMD.Vertices().GetElementIDs())
		{
			if (BaseVertTexel.IsValidIndex(Vertex.GetValue()) && BaseVertTexel[Vertex.GetValue()] != INDEX_NONE)
			{
				Grid.InsertPointUnsafe(Vertex.GetValue(), FVector3d(BasePositions[Vertex]));
			}
		}
		int32 Written = 0;
		for (int32 Lod = 1; Lod < Lods; ++Lod)
		{
			FMeshDescription* MD = OwnLodDescription(Mesh, Lod);
			if (!MD)
			{
				continue;
			}
			FStaticMeshAttributes Attrs(*MD);
			TVertexInstanceAttributesRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
			TVertexAttributesConstRef<FVector3f> Positions = Attrs.GetVertexPositions();
			if (UVs.GetNumChannels() > TexelChannel && !ChannelOnTexelCenters(*MD, TexelChannel, TexSize))
			{
				UE_LOG(LogSoftbodyBake, Warning,
					TEXT("%s LOD %d already uses UV channel %d for its own data, so Softbody left this LOD unchanged and it will not deform "
						"correctly. Give the LOD the same UV channels as LOD 0, or let LOD 0 generate it (LOD reduction)."),
					*Mesh->GetName(), Lod, TexelChannel);
				continue;
			}
			if (TexelChannel >= MaxChannels)
			{
				UE_LOG(LogSoftbodyBake, Warning,
					TEXT("%s LOD %d cannot take UV channel %d (limit %d), so Softbody left this LOD unchanged and it will not deform correctly."),
					*Mesh->GetName(), Lod, TexelChannel, MaxChannels);
				continue;
			}
			if (UVs.GetNumChannels() <= TexelChannel)
			{
				UVs.SetNumChannels(TexelChannel + 1);
			}
			TMap<FVertexID, FVector2f> Nearest;
			int32 NotFound = 0;
			for (const FVertexID Vertex : MD->Vertices().GetElementIDs())
			{
				const FVector3d Query(Positions[Vertex]);
				TPair<int32, double> Found(INDEX_NONE, 0.0);
				for (double Radius = Cell; Found.Key == INDEX_NONE && Radius <= Cell * 256.0; Radius *= 2.0)
				{
					Found = Grid.FindNearestInRadius(Query, Radius,
						[&BasePositions, &Query](const int32& Id) { return FVector3d::DistSquared(FVector3d(BasePositions[FVertexID(Id)]), Query); });
				}
				NotFound += Found.Key == INDEX_NONE ? 1 : 0;
				const int32 Texel = Found.Key != INDEX_NONE ? BaseVertTexel[Found.Key] : 0;
				Nearest.Add(Vertex, FVector2f((Texel % TexSize + 0.5f) / TexSize, (Texel / TexSize + 0.5f) / TexSize));
			}
			for (const FVertexInstanceID Instance : MD->VertexInstances().GetElementIDs())
			{
				UVs.Set(Instance, TexelChannel, Nearest[MD->GetVertexInstanceVertex(Instance)]);
			}
			if (NotFound > 0)
			{
				UE_LOG(LogSoftbodyBake, Warning,
					TEXT("%s LOD %d: %d vertices are far from every LOD 0 vertex, so they read the first cell and may move wrongly. Check that the LOD has the same shape as LOD 0."),
					*Mesh->GetName(), Lod, NotFound);
			}
			CommitLod(Mesh, Lod);
			++Written;
		}
		if (Written > 0)
		{
			UE_LOG(LogSoftbodyBake, Display, TEXT("%s: %d imported LOD(s) read the Softbody data of the nearest LOD 0 vertex."), *Mesh->GetName(),
				Written);
		}
		return Written;
	}

	void WriteLodCarrierChannels(UObject* Mesh, int32 CarrierChannel, int32 MaxChannels)
	{
		if (CarrierChannel == INDEX_NONE || CarrierChannel >= MaxChannels)
		{
			return;
		}
		for (int32 Lod = 1; Lod < NumLods(Mesh); ++Lod)
		{
			FMeshDescription* MD = OwnLodDescription(Mesh, Lod);
			if (!MD)
			{
				continue;
			}
			FStaticMeshAttributes Attrs(*MD);
			TVertexInstanceAttributesRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
			if (UVs.GetNumChannels() > CarrierChannel)
			{
				if (!ChannelUnset(*MD, CarrierChannel))
				{
					UE_LOG(LogSoftbodyBake, Warning,
						TEXT("%s LOD %d already uses UV channel %d for its own data, so dents on this LOD read the contact map at the wrong places. Free that UV channel on the LOD, then press Rebake Contact Map."),
						*Mesh->GetName(), Lod, CarrierChannel);
				}
				continue;
			}
			// (−1, −1) = 이 칸은 정점 표를 읽는다. 다른 LOD는 LOD0의 아일랜드 이음매와 정점이 맞지 않아 차트 UV를 쓰지 않는다.
			UVs.SetNumChannels(CarrierChannel + 1);
			for (const FVertexInstanceID Instance : MD->VertexInstances().GetElementIDs())
			{
				UVs.Set(Instance, CarrierChannel, FVector2f(-1.f, -1.f));
			}
			CommitLod(Mesh, Lod);
		}
	}

	void KeepLightmapUVsFree(UStaticMesh* Mesh, int32 NumChannels, TConstArrayView<int32> OurChannels, int32 MaxChannels)
	{
		if (!Mesh)
		{
			return;
		}
		for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels(); ++Lod)
		{
			FMeshBuildSettings& Build = Mesh->GetSourceModel(Lod).BuildSettings;
			if (!Build.bGenerateLightmapUVs || !OurChannels.Contains(Build.DstLightmapIndex))
			{
				continue;
			}
			const int32 Old = Build.DstLightmapIndex;
			if (NumChannels < MaxChannels)
			{
				// 생성되는 라이트맵 UV를 소프트바디 채널 뒤로 옮긴다. 라이트맵이 읽는 채널도 같이 옮긴다.
				Build.DstLightmapIndex = NumChannels;
				if (Mesh->GetLightMapCoordinateIndex() == Old)
				{
					Mesh->SetLightMapCoordinateIndex(NumChannels);
				}
				UE_LOG(LogSoftbodyBake, Display, TEXT("%s LOD %d: generated lightmap UVs moved from channel %d to %d (channel %d now holds Softbody data)."),
					*Mesh->GetName(), Lod, Old, NumChannels, Old);
				continue;
			}
			Build.bGenerateLightmapUVs = false;
			UE_LOG(LogSoftbodyBake, Warning,
				TEXT("%s LOD %d: lightmap UV generation was turned off: it wrote to UV channel %d, which now holds Softbody data, and there is no "
					"free channel left (limit %d). Static lighting on this mesh needs a lightmap UV of its own in a lower channel."),
				*Mesh->GetName(), Lod, Old, MaxChannels);
		}
	}
}

namespace
{
	/** 선형화된 소스 마스크 픽셀 + 바이리니어 샘플 */
	/** 머티리얼에 소프트바디 커널의 관성 입력(InertiaResponseTex)이 연결돼 있는가 — 실제로 흔들리는 섹션. */
	bool UsesSoftbodyInertia(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return false;
		}
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Material->GetAllTextureParameterInfo(Infos, Ids);
		return Infos.ContainsByPredicate([](const FMaterialParameterInfo& Info)
		{
			return Info.Name == FName(TEXT("InertiaResponseTex"));
		});
	}

	/**
	 * 겹친 층(ADR-0014)의 따를 면이 될 수 있는 슬롯: 소프트바디 머티리얼이 붙은 슬롯(눈꺼풀이 눈알을 따르지 않게) + Pending(같은 Run Bake의
	 * 머티리얼 단계가 곧 소프트바디 머티리얼을 입힐 슬롯 — 첫 굽기가 두 번째 굽기와 같은 면을 고르게). 그런 슬롯이 하나도 없으면
	 * (머티리얼을 입히지 않는 첫 베이크) 빈 집합 = 전부.
	 */
	TSet<FName> SoftbodySlotNames(const USkeletalMesh* Skeletal, const UStaticMesh* Static, TConstArrayView<FName> Pending = {})
	{
		TSet<FName> Names;
		const auto IsSoft = [Pending](const UMaterialInterface* Material, FName SlotName, FName ImportedName)
		{
			return UsesSoftbodyInertia(Material) || Pending.Contains(SlotName) || Pending.Contains(ImportedName);
		};
		if (Skeletal)
		{
			for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
			{
				if (IsSoft(Material.MaterialInterface, Material.MaterialSlotName, Material.ImportedMaterialSlotName))
				{
					Names.Add(Material.ImportedMaterialSlotName);
					Names.Add(Material.MaterialSlotName);
				}
			}
		}
		if (Static)
		{
			for (const FStaticMaterial& Material : Static->GetStaticMaterials())
			{
				if (IsSoft(Material.MaterialInterface, Material.MaterialSlotName, Material.ImportedMaterialSlotName))
				{
					Names.Add(Material.ImportedMaterialSlotName);
					Names.Add(Material.MaterialSlotName);
				}
			}
		}
		Names.Remove(NAME_None);
		return Names;
	}
}

namespace SoftbodyBakeShared
{
	TSet<FName> CoreSlotNames(const USkeletalMesh* Mesh, TConstArrayView<FName> Extra)
	{
		TSet<FName> Names;
		if (!Mesh)
		{
			return Names;
		}
		for (const FSkeletalMaterial& Material : Mesh->GetMaterials())
		{
			const bool bSoft = UsesSoftbodyInertia(Material.MaterialInterface) || ReadsCarrier(Material.MaterialInterface)
				|| Extra.Contains(Material.MaterialSlotName) || Extra.Contains(Material.ImportedMaterialSlotName);
			if (bSoft)
			{
				Names.Add(Material.ImportedMaterialSlotName);
				Names.Add(Material.MaterialSlotName);
			}
		}
		Names.Remove(NAME_None);
		return Names;
	}
}

namespace
{
	// 출렁임 눈금의 기준 살: Jiggle 흰색·Volume 1이고 이 두께(메시 로컬 cm)보다 두꺼운 살. Inertia Amount = 이 살이 1 g에서
	// 처지는 거리. 사람 크기 몸의 Flesh Reach 권장값과 같다 — Flesh Reach가 이 값이면 두꺼운 흰 살이 곧 기준 살이다.
	constexpr float ReferenceFleshReach = 3.f;

	/**
	 * 관성 눈금(ADR-0005 §3.1의 "흔들리는 곳 안에서 다시 맞춘다"를 바꿈). Inertia Amount = 기준 살(ReferenceFleshReach)이 1 g에서
	 * 처지는 거리라, Paint.B 전체에 (원시 최댓값 ÷ 기준 살의 원시 처짐)을 곱한다. 메시 안의 한 점에 맞추지 않으므로 칠 한 곳이 몸 전체
	 * 크기를 바꾸지 않고, 같은 칠·같은 두께의 살이면 어느 메시든 같은 Amount에서 같이 흔들린다. 소프트 슬롯에서 가장 무른 정점
	 * 하나를 1로 맞추면, 그 점이 캐릭터마다 다른 부위(엉덩이·머리·소매)에 걸려 같은 Amount에서 가슴이 0.013~0.99배가 된다.
	 * OutTexelSoftness = 칸마다 세 방향 가운데 가장 큰 무름(메시 최댓값 = 1), OutSoftTexel·OutNumSoftSections = 흔들리는 섹션의 칸 —
	 * 묶음 처짐 몫(ComputeInertiaGroupSag)이 같은 칸을 센다. 소프트바디 머티리얼이 아직 없으면 모든 칸을 센다. PendingSlots(같은 Run Bake의
	 * 머티리얼 단계가 곧 소프트바디 머티리얼을 입힐 슬롯)도 흔들리는 섹션으로 센다.
	 * 곱한 배율을 돌려준다(못 맞추면 1) — 덩어리 살 단단함의 기준 무름 = 원시 최댓값 ÷ 이 값 = 기준 살의 원시 처짐(USoftbodyMeshData::InertiaPaintScale).
	 */
	float RescaleInertiaPaint(const USoftbodyMeshData* Data, TArray<FFloat16Color>& Paint, TConstArrayView<FName> PendingSlots,
		TArray<float>& OutTexelSoftness, TBitArray<>& OutSoftTexel, int32& OutNumSoftSections)
	{
		const int32 TexSize = Data->TextureSize;
		const int32 NumTexels = TexSize * TexSize;
		const int32 NumResponses = Data->NumInertiaResponses;
		TArray64<uint8> Raw;
		if (!Data->InertiaResponseTex || NumResponses <= 0 || Paint.Num() != NumTexels
			|| !Data->InertiaResponseTex->Source.GetMipData(Raw, 0)
			|| Raw.Num() < (int64)NumTexels * NumResponses * (int64)sizeof(FFloat16Color))
		{
			return 1.f;
		}
		const FFloat16Color* Softness = (const FFloat16Color*)Raw.GetData();

		// 흔들리는 섹션의 칸 표시(묶음 처짐 몫도 같은 칸을 쓴다)
		TBitArray<>& SoftTexel = OutSoftTexel;
		SoftTexel.Init(false, NumTexels);
		int32& NumSoftSections = OutNumSoftSections;
		NumSoftSections = 0;
		const auto IsSoft = [PendingSlots](const UMaterialInterface* Material, FName SlotName, FName ImportedName)
		{
			return UsesSoftbodyInertia(Material) || PendingSlots.Contains(SlotName) || PendingSlots.Contains(ImportedName);
		};
		auto MarkRange = [&](const FStaticMeshVertexBuffer& VB, const TArray<int32>& Verts)
		{
			for (int32 v : Verts)
			{
				const FVector2f TexelUV = VB.GetVertexUV(v, Data->TexelUVChannel);
				const int32 TX = FMath::Clamp((int32)(TexelUV.X * TexSize), 0, TexSize - 1);
				const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TexSize), 0, TexSize - 1);
				SoftTexel[TY * TexSize + TX] = true;
			}
		};
		if (const USkeletalMesh* SK = Data->SourceSkeletalMesh)
		{
			const FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
			const TArray<FSkeletalMaterial>& Materials = SK->GetMaterials();
			if (RD && RD->LODRenderData.Num() > 0
				&& Data->TexelUVChannel < (int32)RD->LODRenderData[0].StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords())
			{
				const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
				for (const FSkelMeshRenderSection& Sec : LOD.RenderSections)
				{
					if (Materials.IsValidIndex(Sec.MaterialIndex)
						&& IsSoft(Materials[Sec.MaterialIndex].MaterialInterface, Materials[Sec.MaterialIndex].MaterialSlotName,
							Materials[Sec.MaterialIndex].ImportedMaterialSlotName))
					{
						MarkRange(LOD.StaticVertexBuffers.StaticMeshVertexBuffer,
							SectionVertexRange((int32)Sec.BaseVertexIndex, (int32)Sec.NumVertices));
						++NumSoftSections;
					}
				}
			}
		}
		else if (const UStaticMesh* SM = Data->SourceMesh)
		{
			const TArray<FStaticMaterial>& Materials = SM->GetStaticMaterials();
			if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
				if (Data->TexelUVChannel < (int32)LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords())
				{
					for (const FStaticMeshSection& Sec : LOD.Sections)
					{
						if (Materials.IsValidIndex(Sec.MaterialIndex)
							&& IsSoft(Materials[Sec.MaterialIndex].MaterialInterface, Materials[Sec.MaterialIndex].MaterialSlotName,
								Materials[Sec.MaterialIndex].ImportedMaterialSlotName))
						{
							MarkRange(LOD.VertexBuffers.StaticMeshVertexBuffer, StaticSectionVertices(LOD, Sec));
							++NumSoftSections;
						}
					}
				}
			}
		}

		OutTexelSoftness.SetNumZeroed(NumTexels);
		float ActiveMax = 0.f;
		float OverallMax = 0.f;
		for (int32 i = 0; i < NumTexels; ++i)
		{
			float Length = 0.f;
			for (int32 k = 0; k < NumResponses; ++k)
			{
				const FLinearColor C = Softness[k * NumTexels + i].GetFloats();
				Length = FMath::Max(Length, FVector3f(C.R, C.G, C.B).Size());
			}
			OutTexelSoftness[i] = Length;
			OverallMax = FMath::Max(OverallMax, Length);
			if (NumSoftSections == 0 || SoftTexel[i])
			{
				ActiveMax = FMath::Max(ActiveMax, Length * FMath::Max(Paint[i].B.GetFloat(), 0.f));
			}
		}
		// 기준 살의 원시 처짐: 측정의 살 스프링(Flesh Reach)에서 두께가 R보다 두꺼운 흰 살은 2(1 + ν)R²에서 더 처지지 않는다.
		// 측정 결과는 메시 최댓값으로 나눠 있으니 Paint.B에 (원시 최댓값 ÷ 기준 처짐)을 곱하면 정점 값 = 기준 살에 견준 처짐이다.
		double RawMax = 0.0;
		for (const float RawScale : Data->InertiaResponseScales)
		{
			RawMax = FMath::Max(RawMax, (double)RawScale);
		}
		if (!(RawMax > 0.0))
		{
			UE_LOG(LogSoftbodyBake, Warning, TEXT("Paint: the flesh was measured before its raw softness was recorded, so the jiggle scale is left as baked. Press Run Bake."));
			// 묶음 처짐 몫도 비운 채 둔다 — 실행 중 옛 눈금 그대로 돌고 Validate Setup이 알린다.
			OutTexelSoftness.Reset();
			return 1.f;
		}
		const double Nu = (double)SoftbodyContactResponse::PoissonOfVolume(1.f);
		const double ReferenceSag = 2.0 * (1.0 + Nu) * (double)ReferenceFleshReach * (double)ReferenceFleshReach;
		const float Scale = (float)(RawMax / ReferenceSag);
		// 칠한 살 가운데 가장 많이 처지는 점(기준 살 = 1). 거의 0이면 칠·뼈가 살을 다 덮었고, 몇 배면 Flesh Reach 0이라 깊은 살이 더 처진다.
		const float PaintedMax = ActiveMax * Scale;
		if (PaintedMax < 0.02f)
		{
			UE_LOG(LogSoftbodyBake, Warning,
				TEXT("Paint: the flesh on the Softbody material slots barely moves (%.3f of the reference flesh). Check that Depth and Jiggle are not black over the soft flesh."), PaintedMax);
		}
		else if (PaintedMax > 3.f)
		{
			UE_LOG(LogSoftbodyBake, Warning,
				TEXT("Paint: the deepest flesh moves %.1fx as far as the reference flesh, so Inertia Amount shakes it that much more. For a human-sized body set Flesh Reach to about 3 and press Run Bake."), PaintedMax);
		}
		for (FFloat16Color& P : Paint)
		{
			P.B = FFloat16(P.B.GetFloat() * Scale);
		}
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("Paint: jiggle scale - the reference flesh (white Jiggle, thicker than %.0f cm) sags Inertia Amount at 1 g: raw %.2f, mesh raw max %.2f -> jiggle paint x %.3f. %d jiggling sections, painted flesh moves up to %.2fx (whole mesh %.2fx)"),
			ReferenceFleshReach, ReferenceSag, RawMax, Scale, NumSoftSections, PaintedMax, OverallMax * Scale);
		return Scale;
	}

	/**
	 * 묶음마다 살이 1 g에서 처지는 몫(기준 살 = 1)을 굽는다(ADR-0015 §2.5를 바꿈). Paint.B가 다 정해진 뒤(기준 맞춤·Physics
	 * Driven Bones·겹친 층) 부른다. 몫 = 흔들리는 섹션 정점의 처짐(가장 큰 방향 무름 × Paint.B)을 (묶음 몫 × 처짐)으로 가중한 가운데 값 —
	 * 그 묶음에서 실제로 출렁이는 살의 처짐이다(일지 342: 몫으로만 가중한 90 % 값은 갈비·등 칸이 대부분인 척추 묶음을 0 → 30 Hz로 만들어, 거기
	 * 실린 가슴 밑동 살이 30 Hz로 떨었다). 실행 중
	 * 그 묶음은 √(g ÷ (Inertia Amount × 몫))로 출렁인다 — 같은 Amount에서 단단한 부위가 더 빨리 출렁인다. 정점 하나의 (묶음 몫 × 처짐)
	 * 가운데 가장 큰 값은 흔들림 상한(Max Inertia Offset)이 쓴다. 묶음마다 숫자 둘이라 정점 상태는 늘지 않는다.
	 */
	void ComputeInertiaGroupSag(USoftbodyMeshData* Data, const TArray<FFloat16Color>& Paint, const TArray<float>& TexelSoftness,
		const TBitArray<>& SoftTexel, int32 NumSoftSections)
	{
		Data->InertiaGroupSag.Reset();
		Data->InertiaGroupSagMax.Reset();
		// 칸 v = 굽기 정점 v(칠·무름·묶음 텍스처가 같은 순서다).
		const int32 NumVertices = FMath::Min3(Data->VertexPositions.Num(), Paint.Num(), TexelSoftness.Num());
		if (NumVertices <= 0 || (NumSoftSections > 0 && SoftTexel.Num() < NumVertices))
		{
			return;
		}
		const bool bSkeletal = Data->SourceSkeletalMesh != nullptr && Data->InertiaGroupBones.Num() > 0;
		const int32 NumGroups = bSkeletal ? Data->InertiaGroupBones.Num() : 1;
		TArray64<uint8> RawGroups;
		const FFloat16Color* GroupTexels = nullptr;
		if (bSkeletal)
		{
			if (!Data->AttributeTex2 || !Data->AttributeTex2->Source.GetMipData(RawGroups, 0)
				|| RawGroups.Num() < (int64)NumVertices * (int64)sizeof(FFloat16Color))
			{
				UE_LOG(LogSoftbodyBake, Warning, TEXT("Paint: the inertia groups could not be read, so every body part wobbles at the speed of the reference flesh. Press Run Bake."));
				return;
			}
			GroupTexels = reinterpret_cast<const FFloat16Color*>(RawGroups.GetData());
		}
		// 묶음마다 (처짐, 몫) 쌍
		TArray<TArray<TPair<float, float>>> PerGroup;
		PerGroup.SetNum(NumGroups);
		for (int32 v = 0; v < NumVertices; ++v)
		{
			if (NumSoftSections > 0 && !SoftTexel[v])
			{
				continue;
			}
			const float VertexSag = TexelSoftness[v] * FMath::Max(Paint[v].B.GetFloat(), 0.f);
			if (!GroupTexels)
			{
				PerGroup[0].Emplace(VertexSag, 1.f);
				continue;
			}
			const SoftbodyBakeShared::FVertexGroups Groups = SoftbodyBakeShared::FVertexGroups::Decode(GroupTexels[v].GetFloats());
			for (int32 j = 0; j < SoftbodyBakeShared::FVertexGroups::Num; ++j)
			{
				if (Groups.Weight[j] > 0.f && PerGroup.IsValidIndex(Groups.Group[j]))
				{
					PerGroup[Groups.Group[j]].Emplace(VertexSag, Groups.Weight[j]);
				}
			}
		}
		Data->InertiaGroupSag.Init(0.f, NumGroups);
		Data->InertiaGroupSagMax.Init(0.f, NumGroups);
		FString Summary;
		for (int32 g = 0; g < NumGroups; ++g)
		{
			TArray<TPair<float, float>>& Pairs = PerGroup[g];
			double Total = 0.0;
			float Largest = 0.f;
			for (const TPair<float, float>& Pair : Pairs)
			{
				Total += Pair.Value * Pair.Key;
				Largest = FMath::Max(Largest, Pair.Key * Pair.Value);
			}
			float Typical = 0.f;
			if (Total > 0.0)
			{
				Pairs.Sort([](const TPair<float, float>& Lhs, const TPair<float, float>& Rhs) { return Lhs.Key < Rhs.Key; });
				double Walked = 0.0;
				for (const TPair<float, float>& Pair : Pairs)
				{
					Walked += Pair.Value * Pair.Key;
					Typical = Pair.Key;
					if (Walked >= 0.5 * Total)
					{
						break;
					}
				}
			}
			// 출렁이는 살이 없는 묶음은 0 — 빠르기가 끝없이 커지므로 바닥을 둔다(움직이는 정점이 없어 보이지 않는다).
			Data->InertiaGroupSag[g] = FMath::Max(Typical, 1e-3f);
			Data->InertiaGroupSagMax[g] = Largest;
			const FString Name = bSkeletal ? Data->InertiaGroupBones[g].ToString() : FString(TEXT("whole mesh"));
			Summary += FString::Printf(TEXT("%s%s %.2f (%.1f Hz)"), g > 0 ? TEXT(", ") : TEXT(""), *Name, Data->InertiaGroupSag[g],
				SoftbodyInertiaRuntime::WobbleFrequency(10.f * Data->InertiaGroupSag[g], 1.f));
		}
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("Paint: jiggle per body part - sag under 1 g against the reference flesh, typical of its jiggling flesh (wobble at Inertia Amount 10): %s"), *Summary);
	}

	/**
	 * 피직스가 흔드는 본(Data->PhysicsDrivenBones와 그 아래 본)에 걸린 스킨 웨이트 비율만큼 관성량(Paint.B)을 줄인다.
	 * 관성 기준 다시 맞추기(RescaleInertiaPaint) 뒤에 부른다 — 먼저 줄이면 남은 곳이 새 기준이 되어 몸 전체가 더 흔들린다.
	 * 텍셀 값 = 그 텍셀을 쓰는 렌더 정점들의 비율 평균. 스켈레탈 LOD0 스킨 웨이트를 CPU에서 읽을 수 있어야 한다.
	 */
	void ApplyPhysicsDrivenBones(const USoftbodyMeshData* Data, TArray<FFloat16Color>& Paint)
	{
		const USkeletalMesh* SK = Data->SourceSkeletalMesh;
		if (Data->PhysicsDrivenBones.Num() == 0 || !SK)
		{
			return;
		}
		const FReferenceSkeleton& RefSkeleton = SK->GetRefSkeleton();
		TBitArray<> Driven(false, RefSkeleton.GetNum());
		int32 NumRoots = 0;
		for (const FName& Name : Data->PhysicsDrivenBones)
		{
			const int32 Bone = RefSkeleton.FindBoneIndex(Name);
			if (Bone == INDEX_NONE)
			{
				UE_LOG(LogSoftbodyBake, Warning, TEXT("Paint: bone '%s' in Physics Driven Bones is not in the skeleton. Fix the name in Physics Driven Bones."), *Name.ToString());
				continue;
			}
			Driven[Bone] = true;
			++NumRoots;
		}
		// 부모가 먼저 오므로 한 번 훑으면 아래 본까지 퍼진다.
		for (int32 Bone = 0; Bone < RefSkeleton.GetNum(); ++Bone)
		{
			const int32 Parent = RefSkeleton.GetParentIndex(Bone);
			Driven[Bone] = Driven[Bone] || (Parent != INDEX_NONE && Driven[Parent]);
		}
		const FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
		if (NumRoots == 0 || !RD || RD->LODRenderData.Num() == 0)
		{
			return;
		}
		const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
		const FStaticMeshVertexBuffer& VB = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		if (!LOD.SkinWeightVertexBuffer.GetNeedsCPUAccess() || Data->TexelUVChannel >= (int32)VB.GetNumTexCoords())
		{
			UE_LOG(LogSoftbodyBake, Warning,
				TEXT("Paint: Physics Driven Bones were skipped because the LOD0 skin weights cannot be read on the CPU or the mesh lacks the baked UV channel. Turn on Allow CPU Access for LOD0 of the skeletal mesh (or press Run Bake again), then Reapply Paint."));
			return;
		}
		const int32 TexSize = Data->TextureSize;
		TArray<float> ShareSum;
		TArray<int32> ShareCount;
		ShareSum.Init(0.f, Paint.Num());
		ShareCount.Init(0, Paint.Num());
		TArray<FSkinWeightInfo> Weights;
		LOD.SkinWeightVertexBuffer.GetSkinWeights(Weights);
		for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
		{
			for (uint32 Local = 0; Local < Section.NumVertices; ++Local)
			{
				const int32 Vertex = (int32)(Section.BaseVertexIndex + Local);
				const FSkinWeightInfo& Info = Weights[Vertex];
				float Total = 0.f;
				float OnDriven = 0.f;
				for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
				{
					const float W = (float)Info.InfluenceWeights[Influence];
					if (W <= 0.f)
					{
						continue;
					}
					const int32 Bone = Section.BoneMap[Info.InfluenceBones[Influence]];
					Total += W;
					OnDriven += Driven[Bone] ? W : 0.f;
				}
				const FVector2f TexelUV = VB.GetVertexUV(Vertex, Data->TexelUVChannel);
				const int32 TX = FMath::Clamp((int32)(TexelUV.X * TexSize), 0, TexSize - 1);
				const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TexSize), 0, TexSize - 1);
				const int32 Texel = TY * TexSize + TX;
				if (Total > 0.f && Paint.IsValidIndex(Texel))
				{
					ShareSum[Texel] += OnDriven / Total;
					++ShareCount[Texel];
				}
			}
		}
		int32 NumTouched = 0;
		for (int32 Texel = 0; Texel < Paint.Num(); ++Texel)
		{
			if (ShareCount[Texel] == 0 || ShareSum[Texel] <= 0.f)
			{
				continue;
			}
			const float Share = FMath::Clamp(ShareSum[Texel] / ShareCount[Texel], 0.f, 1.f);
			Paint[Texel].B = FFloat16(Paint[Texel].B.GetFloat() * (1.f - Share));
			++NumTouched;
		}
		UE_LOG(LogSoftbodyBake, Display, TEXT("Paint: %d Physics Driven Bones (and their child bones) - jiggle reduced in %d texels"),
			NumRoots, NumTouched);
	}

	struct FMaskImage
	{
		FImage Linear;
		const FLinearColor* Pix = nullptr;
		int32 W = 0;
		int32 H = 0;

		bool Load(UTexture2D* Tex)
		{
			FImage Src;
			if (!Tex || !Tex->Source.GetMipImage(Src, 0))
			{
				return false;
			}
			Src.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
			Pix = (const FLinearColor*)Linear.RawData.GetData();
			W = Linear.SizeX;
			H = Linear.SizeY;
			return W > 0 && H > 0;
		}

		FLinearColor Sample(float U, float V) const
		{
			const float X = FMath::Clamp(U, 0.f, 1.f) * (W - 1);
			const float Y = FMath::Clamp(V, 0.f, 1.f) * (H - 1);
			const int32 X0 = (int32)X;
			const int32 Y0 = (int32)Y;
			const int32 X1 = FMath::Min(X0 + 1, W - 1);
			const int32 Y1 = FMath::Min(Y0 + 1, H - 1);
			const float FX = X - X0;
			const float FY = Y - Y0;
			return FMath::Lerp(
				FMath::Lerp(Pix[Y0 * W + X0], Pix[Y0 * W + X1], FX),
				FMath::Lerp(Pix[Y1 * W + X0], Pix[Y1 * W + X1], FX), FY);
		}
	};

	/** 매핑된 소스 채널 값 (Keep은 호출 전에 걸러진다) */
	float SampleMaskChannel(const FLinearColor& C, ESoftbodyMaskSource Src)
	{
		switch (Src)
		{
		case ESoftbodyMaskSource::R: return C.R;
		case ESoftbodyMaskSource::G: return C.G;
		case ESoftbodyMaskSource::B: return C.B;
		case ESoftbodyMaskSource::A: return C.A;
		case ESoftbodyMaskSource::One: return 1.f;
		default: return 0.f; // Zero
		}
	}

	/** 칠 변환이 읽는 둘(Depth → 페인트 G, Volume → 페인트 A) 가운데 텍스처를 읽는 것이 있는가(전부 상수면 텍스처 불필요). */
	bool NeedsTexture(const FSoftbodyPaintSlotMask& Map)
	{
		using SoftbodyBakeShared::EPaintKind;
		return SoftbodyBakeShared::IsTextureMaskSource(SoftbodyBakeShared::PaintSource(Map, EPaintKind::Depth))
			|| SoftbodyBakeShared::IsTextureMaskSource(SoftbodyBakeShared::PaintSource(Map, EPaintKind::Volume));
	}

	/** 소스의 디테일 패널 이름(로그·경고 문장). */
	const TCHAR* MaskSourceName(ESoftbodyMaskSource Source)
	{
		switch (Source)
		{
		case ESoftbodyMaskSource::R: return TEXT("Paint Texture R");
		case ESoftbodyMaskSource::G: return TEXT("Paint Texture G");
		case ESoftbodyMaskSource::B: return TEXT("Paint Texture B");
		case ESoftbodyMaskSource::A: return TEXT("Paint Texture A");
		case ESoftbodyMaskSource::One: return TEXT("White everywhere");
		case ESoftbodyMaskSource::Zero: return TEXT("Black everywhere");
		default: return TEXT("unpainted");
		}
	}

	/**
	 * 슬롯 칠의 한 종류(Kind — 측정은 Depth·Jiggle·Volume, ADR-0038)를 메시 정점마다 읽는다. 측정 베이크가 살 속을 채우기 전에 필요한 값이라
	 * 페인트 층(그 뒤에 만든다)이 아니라 칠 텍스처에서 바로 읽는다.
	 * MD 정점 ID 기준 배열(칠이 없는 정점은 1). 정점이 여러 UV 사본을 가지면 평균. 읽을 슬롯이 없으면 빈 배열(= 전부 1).
	 * Masks의 키는 슬롯 이름이고, MD의 폴리곤 그룹 이름(들여올 때의 슬롯 이름)과는 SlotAliases(MD 이름 → 슬롯 이름)로 잇는다.
	 * Label = 로그에 적을 이름, ZeroLabel·OneLabel = 값 0과 1의 뜻(로그). bLog = false면 로그를 남기지 않는다(저작 에셋 미리보기).
	 * OutPainted(있으면): MD 정점 ID마다 이 칠을 매핑한 슬롯에서 읽었는가.
	 */
	TArray<float> SampleSlotMaskChannel(const FMeshDescription& MD, const TMap<FName, FSoftbodyPaintSlotMask>& Masks,
		const TMap<FName, FName>& SlotAliases, SoftbodyBakeShared::EPaintKind Kind, const TCHAR* Label,
		const TCHAR* ZeroLabel, const TCHAR* OneLabel, bool bLog = true, TArray<bool>* OutPainted = nullptr)
	{
		if (OutPainted)
		{
			OutPainted->Init(false, MD.Vertices().GetArraySize());
		}
		TArray<float> Out;
		TMap<FName, FMaskImage> Images;
		bool bAny = false;
		for (const TPair<FName, FSoftbodyPaintSlotMask>& Pair : Masks)
		{
			const ESoftbodyMaskSource Src = SoftbodyBakeShared::PaintSource(Pair.Value, Kind);
			if (Src == ESoftbodyMaskSource::Keep)
			{
				continue;
			}
			bAny = true;
			if (SoftbodyBakeShared::IsTextureMaskSource(Src) && !Images.Add(Pair.Key).Load(Pair.Value.Texture))
			{
				if (bLog)
				{
					UE_LOG(LogSoftbodyBake, Warning, TEXT("%s: slot '%s' reads %s, but its Paint Texture has no source pixels, so this paint stays unpainted (white) on that slot. Reimport the texture, or set this paint to White or Black everywhere."),
						Label, *Pair.Key.ToString(), MaskSourceName(Src));
				}
				Images.Remove(Pair.Key);
			}
		}
		if (!bAny)
		{
			return Out;
		}
		FStaticMeshConstAttributes Attrs(MD);
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
		const TPolygonGroupAttributesConstRef<FName> SlotNames = Attrs.GetPolygonGroupMaterialSlotNames();
		const int32 MaxVertID = MD.Vertices().GetArraySize();
		TArray<float> Sum;
		TArray<int32> Count;
		Sum.Init(0.f, MaxVertID);
		Count.Init(0, MaxVertID);
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			FName Slot = SlotNames.IsValid() ? SlotNames[MD.GetTrianglePolygonGroup(TriID)] : NAME_None;
			if (const FName* Alias = SlotAliases.Find(Slot))
			{
				Slot = *Alias;
			}
			const FSoftbodyPaintSlotMask* Map = Masks.Find(Slot);
			const ESoftbodyMaskSource Src = Map ? SoftbodyBakeShared::PaintSource(*Map, Kind) : ESoftbodyMaskSource::Keep;
			if (Src == ESoftbodyMaskSource::Keep)
			{
				continue;
			}
			const FMaskImage* Image = Images.Find(Slot);
			// 칠 변환·Hard Surface 조각과 같은 판정: 텍스처 채널인데 읽은 텍스처가 없으면 칠하지 않은 슬롯과 같다(1).
			if (SoftbodyBakeShared::EffectiveMaskSource(Src, Image != nullptr) == ESoftbodyMaskSource::Keep)
			{
				continue;
			}
			for (const FVertexInstanceID InstanceID : MD.GetTriangleVertexInstances(TriID))
			{
				FLinearColor C(0.f, 0.f, 0.f, 0.f);
				if (Image && UVs.IsValid() && UVs.GetNumChannels() > 0)
				{
					const FVector2f UV0 = UVs.Get(InstanceID, 0);
					C = Image->Sample(UV0.X, UV0.Y);
				}
				const int32 VertID = MD.GetVertexInstanceVertex(InstanceID).GetValue();
				Sum[VertID] += FMath::Clamp(SampleMaskChannel(C, Src), 0.f, 1.f);
				++Count[VertID];
			}
		}
		Out.Init(1.f, MaxVertID);
		int32 NumRead = 0;
		int32 NumZero = 0;
		int32 NumBetween = 0;
		double Total = 0.0;
		for (int32 v = 0; v < MaxVertID; ++v)
		{
			if (Count[v] > 0)
			{
				Out[v] = Sum[v] / Count[v];
				++NumRead;
				Total += Out[v];
				NumZero += Out[v] <= 0.01f ? 1 : 0;
				NumBetween += Out[v] > 0.01f && Out[v] < 0.99f ? 1 : 0;
				if (OutPainted)
				{
					(*OutPainted)[v] = true;
				}
			}
		}
		if (bLog)
		{
			UE_LOG(LogSoftbodyBake, Display, TEXT("%s: %d / %d vertices read from the mask - %s (0) %d, in between %d, %s (1) %d, mean %.3f"),
				Label, NumRead, MD.Vertices().Num(), ZeroLabel, NumZero, NumBetween, OneLabel, NumRead - NumZero - NumBetween,
				NumRead > 0 ? Total / NumRead : 1.0);
		}
		return Out;
	}

	/**
	 * 측정 베이크가 살 속을 채울 때 읽는 칠을 슬롯 칠에서 읽어 넣는다(ADR-0038): Depth(뼈 깊이 — 살 두께와 같은 칠), Jiggle(살의 강성),
	 * Volume(푸아송 비), Softness(떠 있는 판 조각은 살갗이 아니다). bLog = false면 로그 없이(칠 단계가 측정과 같은 칠인지 볼 때).
	 */
	void SampleMeasurePaint(const FMeshDescription& MD, const TMap<FName, FSoftbodyPaintSlotMask>& Masks,
		const TMap<FName, FName>& SlotAliases, FSoftbodyMeasureParams& Params, bool bLog = true)
	{
		using SoftbodyBakeShared::EPaintKind;
		Params.VertexDepth = SampleSlotMaskChannel(MD, Masks, SlotAliases, EPaintKind::Depth,
			TEXT("Depth"), TEXT("bone at the skin"), TEXT("flesh down to the core"), bLog);
		Params.VertexJiggle = SampleSlotMaskChannel(MD, Masks, SlotAliases, EPaintKind::Jiggle,
			TEXT("Jiggle"), TEXT("firm as bone"), TEXT("soft flesh"), bLog);
		Params.VertexVolume = SampleSlotMaskChannel(MD, Masks, SlotAliases, EPaintKind::Volume,
			TEXT("Volume"), TEXT("foam"), TEXT("flesh"), bLog);
		// 떠 있는 판 조각은 살갗으로 읽지 않는다(ADR-0037 §3.7). 조각이 없으면 비운 채라 측정은 지금 그대로다.
		TArray<int32> TrianglePieces;
		TArray<uint8> Floating;
		if (SoftbodyBakeShared::FindHardPieces(MD, Masks, SlotAliases, TrianglePieces, Floating, Params.HardMaskHash, false) > 0)
		{
			const TVertexAttributesConstRef<FVector3f> Positions = MD.GetVertexPositions();
			for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
			{
				const int32 Piece = TrianglePieces[TriID.GetValue()];
				if (Piece > 0 && Floating[Piece - 1])
				{
					for (const FVertexID Corner : MD.GetTriangleVertices(TriID))
					{
						Params.SkinSkipPositions.Add(Positions[Corner]);
					}
				}
			}
			if (bLog && Params.SkinSkipPositions.Num() > 0)
			{
				UE_LOG(LogSoftbodyBake, Display, TEXT("Flex: %d vertices of floating plates are not read as skin (the flesh under them reads the surface below)"),
					Params.SkinSkipPositions.Num());
			}
		}
	}

	/**
	 * 겹친 층(ADR-0014): 속옷처럼 덮인 면의 칸이 따르는 살 칸의 겉껍질 장력(R)·눌리는 정도(G)·출렁이는 정도(B)·부피 보존(A)을 쓴다 — 두 층이
	 * 같은 양·같은 모양으로 눌리고 흔들린다. 피직스 본 몫을 뺀 뒤에 부른다(살 쪽 본 웨이트로 줄인 값까지 같게).
	 */
	void ApplyLayerLinksToPaint(const USoftbodyMeshData* Data, TArray<FFloat16Color>& Paint)
	{
		if (Data->LayerLinks.Num() == 0)
		{
			return;
		}
		TArray<FVector4f> Values;
		Values.SetNumUninitialized(Paint.Num());
		for (int32 i = 0; i < Paint.Num(); ++i)
		{
			Values[i] = FVector4f(Paint[i].R.GetFloat(), Paint[i].G.GetFloat(), Paint[i].B.GetFloat(), Paint[i].A.GetFloat());
		}
		SoftbodyLayers::Apply(Data->LayerLinks, Values, FVector4f(0.f, 0.f, 0.f, 0.f));
		for (const FSoftbodyLayerLink& Link : Data->LayerLinks)
		{
			if (Paint.IsValidIndex(Link.Vertex))
			{
				Paint[Link.Vertex].R = FFloat16(Values[Link.Vertex].X);
				Paint[Link.Vertex].G = FFloat16(Values[Link.Vertex].Y);
				Paint[Link.Vertex].B = FFloat16(Values[Link.Vertex].Z);
				Paint[Link.Vertex].A = FFloat16(Values[Link.Vertex].W);
			}
		}
		UE_LOG(LogSoftbodyBake, Display, TEXT("Paint: %d layered clothing vertices take the Depth, Jiggle and Volume of the flesh underneath"), Data->LayerLinks.Num());
	}

	/**
	 * 메시의 삼각형을 구운 정점 번호(= 정점 텍셀 번호) 셋으로 돌려준다.
	 * 원본 메시(메시 디스크립션)의 삼각형을 쓴다: 구운 정점 번호는 그 정점 순서 그대로다. 굽고 나서 메시가 바뀌어 자리가 맞지 않으면
	 * 렌더 삼각형(LOD0)을 텍셀 UV로 되짚는다(이음매에서 갈라진 렌더 정점은 같은 번호로 합쳐진다). 나나이트 메시의 LOD0은 간략화된
	 * 대체 메시라 원본과 삼각형이 달라서 렌더 삼각형을 먼저 쓰지 않는다.
	 */
	void ForEachBakedTriangle(const USoftbodyMeshData* Data, TFunctionRef<void(int32, int32, int32)> Visit)
	{
		const int32 NumVertices = Data->VertexPositions.Num();
		const FMeshDescription* MD = Data->SourceSkeletalMesh ? Data->SourceSkeletalMesh->GetMeshDescription(0)
			: (Data->SourceMesh ? Data->SourceMesh->GetMeshDescription(0) : nullptr);
		if (MD && MD->Vertices().Num() == NumVertices)
		{
			const TVertexAttributesConstRef<FVector3f> Positions = MD->GetVertexPositions();
			TMap<FVertexID, int32> IndexOf;
			IndexOf.Reserve(NumVertices);
			bool bSameOrder = true;
			for (const FVertexID VertID : MD->Vertices().GetElementIDs())
			{
				bSameOrder = bSameOrder && Positions[VertID].Equals(Data->VertexPositions[IndexOf.Num()], 0.01f);
				IndexOf.Add(VertID, IndexOf.Num());
			}
			if (bSameOrder)
			{
				for (const FTriangleID TriID : MD->Triangles().GetElementIDs())
				{
					const TArrayView<const FVertexID> Corners = MD->GetTriangleVertices(TriID);
					Visit(IndexOf[Corners[0]], IndexOf[Corners[1]], IndexOf[Corners[2]]);
				}
				return;
			}
		}

		const int32 TexSize = Data->TextureSize;
		auto Run = [&](const FStaticMeshVertexBuffer& VB, TFunctionRef<uint32(int32)> IndexAt, int32 NumIndices)
		{
			if (Data->TexelUVChannel >= (int32)VB.GetNumTexCoords())
			{
				return;
			}
			auto VertexOf = [&](uint32 RenderVertex)
			{
				const FVector2f TexelUV = VB.GetVertexUV(RenderVertex, Data->TexelUVChannel);
				const int32 TX = FMath::Clamp((int32)(TexelUV.X * TexSize), 0, TexSize - 1);
				const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TexSize), 0, TexSize - 1);
				return TY * TexSize + TX;
			};
			for (int32 i = 0; i + 2 < NumIndices; i += 3)
			{
				Visit(VertexOf(IndexAt(i)), VertexOf(IndexAt(i + 1)), VertexOf(IndexAt(i + 2)));
			}
		};
		if (const USkeletalMesh* SK = Data->SourceSkeletalMesh)
		{
			const FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
			if (RD && RD->LODRenderData.Num() > 0)
			{
				const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
				TArray<uint32> Indices;
				LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
				Run(LOD.StaticVertexBuffers.StaticMeshVertexBuffer, [&Indices](int32 i) { return Indices[i]; }, Indices.Num());
			}
		}
		else if (const UStaticMesh* SM = Data->SourceMesh)
		{
			if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
				Run(LOD.VertexBuffers.StaticMeshVertexBuffer,
					[&LOD](int32 i) { return LOD.IndexBuffer.GetIndex((uint32)i); }, LOD.IndexBuffer.GetNumIndices());
			}
		}
	}

	/**
	 * 노멀을 팔면체에 접은 두 값(관성 표 3·4줄의 A)을 다시 편다. 셰이더 SB_InertiaRestNormal과 같은 계산을 같은 정밀도(float)로 한다 —
	 * 접선을 만들 때 어느 축과 곱할지(bAlongZ)를 펴기 전의 값으로 고르는데, 그 값은 더하기·빼기·비교뿐이라 셰이더와 한 자리도
	 * 다르지 않다.
	 */
	FVector3f UnfoldRestNormal(const FVector2f& Folded, bool& bAlongZ, float& Margin)
	{
		FVector3f V(Folded.X, Folded.Y, 1.f - FMath::Abs(Folded.X) - FMath::Abs(Folded.Y));
		const float Under = FMath::Max(-V.Z, 0.f);
		V.X += V.X >= 0.f ? -Under : Under;
		V.Y += V.Y >= 0.f ? -Under : Under;
		bAlongZ = FMath::Abs(V.Z) < 0.5f;
		Margin = FMath::Abs(FMath::Abs(V.Z) - 0.5f);
		return V.GetSafeNormal();
	}

	/**
	 * 살이 흔들릴 때 음영이 따라 기울게 하는 값을 구워 InertiaResponseTex 뒤에 붙인다(USoftbodyMeshData::InertiaResponseRows, 일지 133).
	 *
	 * 머티리얼은 정점 위치만 옮기고 노멀은 그대로 둔다. 흔들린 면이 흔들린 대로 칠해지려면 노멀을 면이 기운 만큼 돌려야 한다. 면이
	 * 기우는 것은 흔들림의 노멀 성분(면 밖으로 밀리는 양)이 면 위에서 자리마다 다르기 때문인데, 정점 셰이더는 제 표만 읽으므로
	 * 이웃과 얼마나 다른지를 모른다. 그래서 "이웃으로 1 cm 가면 값이 얼마나 바뀌는가"를 정점마다 여기서 재어 둔다.
	 * 흔들림의 노멀 성분 = 본 묶음의 몫 × (칠 × 무름 세 방향의 노멀 성분) · 하중이므로 적는 값은
	 *   - 칠 × 무름 세 방향의 노멀 성분(3줄), 그것이 면 위의 접선 1·2 방향으로 1 cm에 바뀌는 양(4·5줄),
	 *   - 그 정점이 가진 본 묶음 자리 0·1의 몫이 접선 1·2 방향으로 1 cm에 바뀌는 양(6줄; 자리 2 = 나머지),
	 *   - 노멀(3·4줄의 A에 팔면체로 접어서). 접선 둘은 노멀에서 정해진 규칙으로 만든다(셰이더가 같은 규칙으로 다시 만든다).
	 * 하중이 바뀌는 양은 셰이더가 안다. 정점 셰이더가 읽는 표는 이 넷과 본 묶음 표 하나로, 화면 미분으로 구하던 때와 같은 다섯 번이다.
	 * 흔들림이 면을 따라 살을 늘이거나 미는 몫(접선 성분의 기울기)은 적지 않는다: 그것까지 넣으면 표를 정점마다 여섯 번 더 읽는다.
	 *
	 * 재는 법: 정점에 붙은 삼각형마다 그 안의 기울기(삼각형 안에서 값은 직선으로 이어진다)를 구해 꼭지각으로 평균한다 — 매끄러운
	 * 정점 노멀을 만드는 것과 같은 평균이다. 같은 자리에 겹친 정점(메시 조각의 이음매)은 한 점으로 보고 양쪽 삼각형을 다 센다(따로
	 * 세면 흔들릴 때 이음매가 선으로 보인다). 꺾인 모서리의 정점은 양쪽 면을 평균한 한 값이 된다(면마다 따로 적을 자리가 없다).
	 * 본 묶음은 정점마다 셋까지만 적혀 있어 이웃이 가진 넷째 묶음은 보이지 않는다. 그 몫은 이 정점의 묶음들이 제 몫대로 나눠 받은
	 * 것으로 친다(보이지 않는 묶음도 이 정점처럼 움직인다고 본다) — 몫의 기울기를 다 더하면 0이 되게.
	 *
	 * PaintTexels: 정점 텍셀 순서의 페인트(B = 관성량). nullptr이면 Data의 PaintTex에서 읽고, 그것도 없으면 1.
	 */
	bool BuildInertiaShading(USoftbodyMeshData* Data, const TArray<FFloat16Color>* PaintTexels)
	{
		using SoftbodyBakeShared::FVertexGroups;
		const int32 NumVertices = Data ? Data->VertexPositions.Num() : 0;
		const int32 TexSize = Data ? Data->TextureSize : 0;
		const int32 NumTexels = TexSize * TexSize;
		if (NumVertices <= 0 || NumTexels < NumVertices || Data->NumInertiaResponses != 3
			|| Data->VertexInertiaResponseShapes.Num() != 3 * NumVertices)
		{
			return false;
		}

		TArray64<uint8> RawPaint;
		const FFloat16Color* Paint = PaintTexels && PaintTexels->Num() >= NumVertices ? PaintTexels->GetData() : nullptr;
		if (!Paint && Data->PaintTex && Data->PaintTex->Source.GetMipData(RawPaint, 0)
			&& RawPaint.Num() >= (int64)NumTexels * (int64)sizeof(FFloat16Color))
		{
			Paint = reinterpret_cast<const FFloat16Color*>(RawPaint.GetData());
		}
		TArray64<uint8> RawGroups;
		TArray<FVertexGroups> Groups;
		if (Data->AttributeTex2 && Data->AttributeTex2->Source.GetMipData(RawGroups, 0)
			&& RawGroups.Num() >= (int64)NumTexels * (int64)sizeof(FFloat16Color))
		{
			const FFloat16Color* Texels = reinterpret_cast<const FFloat16Color*>(RawGroups.GetData());
			Groups.SetNum(NumVertices);
			for (int32 v = 0; v < NumVertices; ++v)
			{
				Groups[v] = FVertexGroups::Decode(Texels[v].GetFloats());
			}
		}
		// 정점 x에서 묶음 Group의 몫(같은 묶음이 여러 자리에 있으면 합친다).
		auto ShareOf = [&Groups](int32 Vertex, int32 Group)
		{
			float Share = 0.f;
			for (int32 j = 0; j < FVertexGroups::Num; ++j)
			{
				Share += Groups[Vertex].Group[j] == Group ? Groups[Vertex].Weight[j] : 0.f;
			}
			return (double)Share;
		};

		// 칠 × 무름: 정점마다 세 방향 [3·v + k]
		TArray<FVector3d> Soft;
		Soft.SetNumUninitialized(3 * NumVertices);
		for (int32 v = 0; v < NumVertices; ++v)
		{
			const double PaintB = Paint ? (double)Paint[v].B.GetFloat() : 1.0;
			for (int32 k = 0; k < 3; ++k)
			{
				Soft[3 * v + k] = FVector3d(Data->VertexInertiaResponseShapes[k * NumVertices + v]) * PaintB;
			}
		}

		// 같은 자리의 정점은 한 점(Same[v] = 그 자리의 첫 정점)
		TArray<int32> Same;
		Same.SetNumUninitialized(NumVertices);
		int32 NumMerged = 0;
		{
			TMap<FVector3f, int32> AtPosition;
			AtPosition.Reserve(NumVertices);
			for (int32 v = 0; v < NumVertices; ++v)
			{
				int32& First = AtPosition.FindOrAdd(Data->VertexPositions[v], v);
				Same[v] = First;
				NumMerged += First != v ? 1 : 0;
			}
		}

		// 삼각형의 꼭지 하나: 그 삼각형 안에서 값 phi의 기울기 = CoefA × (phi_A − phi_Center) + CoefB × (phi_B − phi_Center), 꼭지각을 곱해 둠.
		struct FCorner
		{
			int32 Center = 0;
			int32 A = 0;
			int32 B = 0;
			FVector3d CoefA = FVector3d::ZeroVector;
			FVector3d CoefB = FVector3d::ZeroVector;
		};
		TArray<TArray<FCorner, TInlineAllocator<8>>> Corners;
		Corners.SetNum(NumVertices);
		TArray<FVector3d> NormalSum;
		NormalSum.Init(FVector3d::ZeroVector, NumVertices);
		TArray<double> AngleSum;
		AngleSum.Init(0.0, NumVertices);
		ForEachBakedTriangle(Data, [&](int32 V0, int32 V1, int32 V2)
		{
			const int32 Triangle[3] = {V0, V1, V2};
			if (V0 >= NumVertices || V1 >= NumVertices || V2 >= NumVertices
				|| Same[V0] == Same[V1] || Same[V1] == Same[V2] || Same[V2] == Same[V0])
			{
				return;
			}
			for (int32 c = 0; c < 3; ++c)
			{
				FCorner Corner;
				Corner.Center = Triangle[c];
				Corner.A = Triangle[(c + 1) % 3];
				Corner.B = Triangle[(c + 2) % 3];
				const FVector3d E1 = FVector3d(Data->VertexPositions[Corner.A]) - FVector3d(Data->VertexPositions[Corner.Center]);
				const FVector3d E2 = FVector3d(Data->VertexPositions[Corner.B]) - FVector3d(Data->VertexPositions[Corner.Center]);
				const FVector3d Normal = E1 ^ E2;
				const double NormalSq = Normal.SizeSquared();
				const double LengthSq = E1.SizeSquared() * E2.SizeSquared();
				if (NormalSq <= 1e-20 || LengthSq <= 1e-20)
				{
					return;
				}
				const double Angle = FMath::Acos(FMath::Clamp((E1 | E2) / FMath::Sqrt(LengthSq), -1.0, 1.0));
				Corner.CoefA = (E2 ^ Normal) * (Angle / NormalSq);
				Corner.CoefB = (Normal ^ E1) * (Angle / NormalSq);
				const int32 At = Same[Corner.Center];
				Corners[At].Add(Corner);
				NormalSum[At] += Normal * (Angle / FMath::Sqrt(NormalSq));
				AngleSum[At] += Angle;
			}
		});

		const int32 Rows = USoftbodyMeshData::InertiaRowsWithShading;
		TArray<FFloat16Color> Texels;
		Texels.Init(FFloat16Color(FLinearColor(0.f, 0.f, 0.f, 0.f)), NumTexels * Rows);
		// 반정밀 범위를 넘거나 수가 아닌 값은 0으로(그 정점은 음영을 돌리지 않는다).
		auto Stored = [](double Value) { return FMath::IsFinite(Value) && FMath::Abs(Value) < 6.e4 ? (float)Value : 0.f; };
		int32 NumLoose = 0;
		double SteepestSlope = 0.0;
		for (int32 v = 0; v < NumVertices; ++v)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& Shape = Data->VertexInertiaResponseShapes[k * NumVertices + v];
				Texels[k * NumTexels + v] = FFloat16Color(FLinearColor(Shape.X, Shape.Y, Shape.Z, 0.f));
			}
			const int32 At = Same[v];
			FVector3d SmoothNormal = NormalSum[At];
			if (AngleSum[At] <= 0.0 || !SmoothNormal.Normalize(1e-12))
			{
				// 삼각형에 붙지 않은 정점: 기울기 0(돌리지 않는다). 접은 노멀 (0, 0) = +Z.
				++NumLoose;
				continue;
			}

			// 노멀을 팔면체에 접어 반정밀로 적고, 셰이더가 읽을 그 값에서 노멀과 접선을 다시 만든다(굽기와 셰이더가 같은 접선을 쓴다).
			// 접선의 축을 고르는 문턱 바로 위에 걸리면 한 칸 옮겨 적는다(셰이더와 고르는 축이 갈릴 틈을 없앤다. 노멀은 0.2도쯤 달라진다).
			const FVector3f Unit(SmoothNormal);
			FVector2f Folded = FVector2f(Unit.X, Unit.Y) / (FMath::Abs(Unit.X) + FMath::Abs(Unit.Y) + FMath::Abs(Unit.Z));
			if (Unit.Z < 0.f)
			{
				Folded = FVector2f((1.f - FMath::Abs(Folded.Y)) * (Folded.X >= 0.f ? 1.f : -1.f), (1.f - FMath::Abs(Folded.X)) * (Folded.Y >= 0.f ? 1.f : -1.f));
			}
			bool bAlongZ = true;
			float Margin = 0.f;
			FVector3f RestNormal = FVector3f::ZAxisVector;
			for (int32 Try = 0; Try < 4; ++Try)
			{
				Folded = FVector2f(FFloat16(Folded.X).GetFloat(), FFloat16(Folded.Y).GetFloat());
				RestNormal = UnfoldRestNormal(Folded, bAlongZ, Margin);
				if (Margin > 1.e-3f)
				{
					break;
				}
				Folded.X += Folded.X >= 0.f ? -0.004f : 0.004f;
			}
			const FVector3d Normal(RestNormal);
			FVector3d Tangent[2];
			Tangent[0] = (Normal ^ (bAlongZ ? FVector3d::UnitZ() : FVector3d::UnitX())).GetSafeNormal();
			Tangent[1] = Normal ^ Tangent[0];

			// 이 정점의 묶음 자리 가운데 묶음마다 앞자리 하나만 센다(셰이더가 같은 묶음을 앞자리에 합친다).
			bool bFirstSlot[FVertexGroups::Num] = {};
			if (Groups.Num() > 0)
			{
				for (int32 s = 0; s < FVertexGroups::Num; ++s)
				{
					bFirstSlot[s] = true;
					for (int32 j = 0; j < s; ++j)
					{
						bFirstSlot[s] = bFirstSlot[s] && Groups[v].Group[j] != Groups[v].Group[s];
					}
				}
			}

			// Outward[k] = 칠 × 무름 k의 노멀 성분(이 정점의 노멀로 잰다 — 이웃 정점의 값도 이 노멀로 재서 견준다).
			FVector3d Outward;
			FVector3d OutwardSlope[2] = {FVector3d::ZeroVector, FVector3d::ZeroVector};
			double ShareSlope[2][FVertexGroups::Num] = {};
			for (int32 k = 0; k < 3; ++k)
			{
				Outward[k] = Soft[3 * v + k] | Normal;
			}
			for (const FCorner& Corner : Corners[At])
			{
				for (int32 i = 0; i < 2; ++i)
				{
					const double WeightA = (Corner.CoefA | Tangent[i]) / AngleSum[At];
					const double WeightB = (Corner.CoefB | Tangent[i]) / AngleSum[At];
					for (int32 k = 0; k < 3; ++k)
					{
						OutwardSlope[i][k] += ((Soft[3 * Corner.A + k] - Soft[3 * Corner.Center + k]) | Normal) * WeightA
							+ ((Soft[3 * Corner.B + k] - Soft[3 * Corner.Center + k]) | Normal) * WeightB;
					}
					for (int32 s = 0; s < FVertexGroups::Num; ++s)
					{
						if (bFirstSlot[s])
						{
							const int32 Group = Groups[v].Group[s];
							const double Here = ShareOf(Corner.Center, Group);
							ShareSlope[i][s] += (ShareOf(Corner.A, Group) - Here) * WeightA + (ShareOf(Corner.B, Group) - Here) * WeightB;
						}
					}
				}
			}
			for (int32 i = 0; i < 2; ++i)
			{
				// 이 정점에 적히지 않은 묶음(이웃의 넷째 묶음)으로 간 몫은 제 몫대로 나눠 받는다 — 기울기의 합이 0이라, 자리 2의 것은
				// 적지 않아도 −(자리 0 + 자리 1)이다.
				double Unseen = 0.0;
				for (int32 s = 0; s < FVertexGroups::Num; ++s)
				{
					Unseen -= ShareSlope[i][s];
				}
				for (int32 s = 0; s < FVertexGroups::Num; ++s)
				{
					if (bFirstSlot[s])
					{
						ShareSlope[i][s] += ShareOf(v, Groups[v].Group[s]) * Unseen;
					}
				}
				SteepestSlope = FMath::Max(SteepestSlope, OutwardSlope[i].Size());
			}
			Texels[3 * NumTexels + v] = FFloat16Color(FLinearColor(Stored(Outward.X), Stored(Outward.Y), Stored(Outward.Z), Folded.X));
			Texels[4 * NumTexels + v] = FFloat16Color(FLinearColor(
				Stored(OutwardSlope[0].X), Stored(OutwardSlope[0].Y), Stored(OutwardSlope[0].Z), Folded.Y));
			Texels[5 * NumTexels + v] = FFloat16Color(FLinearColor(
				Stored(OutwardSlope[1].X), Stored(OutwardSlope[1].Y), Stored(OutwardSlope[1].Z), 0.f));
			Texels[6 * NumTexels + v] = FFloat16Color(FLinearColor(
				Stored(ShareSlope[0][0]), Stored(ShareSlope[0][1]), Stored(ShareSlope[1][0]), Stored(ShareSlope[1][1])));
		}

		const FString MeshName = Data->SourceSkeletalMesh ? Data->SourceSkeletalMesh->GetName()
			: (Data->SourceMesh ? Data->SourceMesh->GetName() : Data->GetName());
		const FString Folder = FPackageName::GetLongPackagePath(Data->GetPackage()->GetName());
		Data->InertiaResponseTex = CreateAttributeTextureRect(Folder,
			FString::Printf(TEXT("T_%s_InertiaResponse"), *MeshName), TexSize, TexSize * Rows, Texels);
		Data->InertiaResponseRows = Rows;
		Data->MarkPackageDirty();
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("Jiggle shading %s: wrote normals and slopes for %d vertices (%d inertia table rows) - %d vertices merged as one point, %d vertices on no triangle, ")
			TEXT("steepest change of the outward push %.3f per 1 cm"),
			*Data->GetName(), NumVertices, Rows, NumMerged, NumLoose, SteepestSlope);
		return true;
	}

	/**
	 * 출렁임 칠(Paint.B)이 좁은 폭에서 급하게 바뀌는 곳을 찾아 알린다(일지 123). 흔들림은 정점마다 칠 × 구운 무름 × 하중으로 따로
	 * 정해지고 이웃 정점끼리 묶지 않는다. 그래서 칠이 변 한두 개 사이에서 크게 바뀌면 그 변의 두 끝만 서로 멀어져, 흔들릴 때 그
	 * 자리가 찢긴 듯 접혀 보인다.
	 *   - 재는 값: 가장 많이 흔들리는 점이 몇 cm 밀릴 때 어느 변의 두 끝이 그 변의 길이보다 더 어긋나기 시작하는가(x·y·z로
	 *     당겨 본 것 가운데 가장 이른 쪽). 런타임에서 그 크기는 InertiaAmount × 하중이고 InertiaMaxOffset을 넘지 않는다.
	 *   - 견주는 값: 변마다 칠을 두 끝의 평균으로 고르게 폈을 때의 같은 크기. 칠의 기울기를 뺀, 살 자체의 무름이 정하는 값이다.
	 * 칠 때문에 그 절반도 못 가서 접히면 경고한다 — 칠의 기울기가 살 자체보다 이웃을 더 크게 어긋나게 한다는 뜻이다.
	 */
	void WarnSteepInertiaPaint(const USoftbodyMeshData* Data, const TArray<FFloat16Color>& Paint)
	{
		const int32 NumVertices = Data->VertexPositions.Num();
		if (NumVertices <= 0 || Data->NumInertiaResponses != 3 || Data->VertexInertiaResponseShapes.Num() != 3 * NumVertices
			|| Paint.Num() < NumVertices)
		{
			return;
		}
		const int32 TexSize = Data->TextureSize;

		// 렌더 삼각형의 변을 구운 정점 번호(= 정점 텍셀 번호)로 모은다.
		TSet<uint64> Edges;
		ForEachBakedTriangle(Data, [&](int32 V0, int32 V1, int32 V2)
		{
			const int32 Corner[3] = {V0, V1, V2};
			for (int32 c = 0; c < 3; ++c)
			{
				const int32 A = FMath::Min(Corner[c], Corner[(c + 1) % 3]);
				const int32 B = FMath::Max(Corner[c], Corner[(c + 1) % 3]);
				if (A != B && B < NumVertices)
				{
					Edges.Add(((uint64)A << 32) | (uint64)B);
				}
			}
		});
		if (Edges.Num() == 0)
		{
			return;
		}

		struct FEdge
		{
			int32 A = 0;
			int32 B = 0;
			float Length = 0.f;
			/** 가장 많이 흔들리는 점이 1 cm 밀릴 때 두 끝이 어긋나는 거리(cm): 지금 칠로, 칠을 고르게 폈을 때. */
			float Slip = 0.f;
			float SlipFlat = 0.f;
		};
		// 거의 겹친 두 정점은 뺀다(반정밀 칠의 끝자리 차이가 변 길이보다 커 보인다).
		const float MeshSize = FBox3f(Data->VertexPositions).GetSize().Size();
		const float MinLength = 1.e-4f * MeshSize;
		TArray<FEdge> Measured;
		Measured.Reserve(Edges.Num());
		float Worst = 0.f;
		float WorstFlat = 0.f;
		int32 WorstEdge = INDEX_NONE;
		for (const uint64 Key : Edges)
		{
			FEdge Edge;
			Edge.A = (int32)(Key >> 32);
			Edge.B = (int32)(Key & 0xffffffffu);
			Edge.Length = FVector3f::Distance(Data->VertexPositions[Edge.A], Data->VertexPositions[Edge.B]);
			if (Edge.Length <= MinLength)
			{
				continue;
			}
			const float PaintA = FMath::Max(Paint[Edge.A].B.GetFloat(), 0.f);
			const float PaintB = FMath::Max(Paint[Edge.B].B.GetFloat(), 0.f);
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& ShapeA = Data->VertexInertiaResponseShapes[k * NumVertices + Edge.A];
				const FVector3f& ShapeB = Data->VertexInertiaResponseShapes[k * NumVertices + Edge.B];
				Edge.Slip = FMath::Max(Edge.Slip, (ShapeA * PaintA - ShapeB * PaintB).Size());
				Edge.SlipFlat = FMath::Max(Edge.SlipFlat, (ShapeA - ShapeB).Size() * 0.5f * (PaintA + PaintB));
			}
			Edge.Slip /= Edge.Length;
			Edge.SlipFlat /= Edge.Length;
			if (Edge.Slip > Worst)
			{
				Worst = Edge.Slip;
				WorstEdge = Measured.Num();
			}
			WorstFlat = FMath::Max(WorstFlat, Edge.SlipFlat);
			Measured.Add(Edge);
		}
		// 메시 크기보다 더 밀려야 접히는 칠은 문제 삼지 않는다(거의 안 흔들리는 단단한 물체).
		if (WorstEdge == INDEX_NONE || Worst <= 2.f * WorstFlat || Worst * MeshSize <= 1.f)
		{
			return;
		}
		int32 NumSteep = 0;
		for (const FEdge& Edge : Measured)
		{
			NumSteep += Edge.Slip > 2.f * WorstFlat ? 1 : 0;
		}

		// 가장 급한 변이 어디인지: 그 정점이 가장 큰 웨이트로 붙은 본(스켈레탈)과 메시 로컬 자리.
		const FEdge& Steepest = Measured[WorstEdge];
		FString Where;
		if (const USkeletalMesh* SK = Data->SourceSkeletalMesh)
		{
			const FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
			if (RD && RD->LODRenderData.Num() > 0 && RD->LODRenderData[0].SkinWeightVertexBuffer.GetNeedsCPUAccess())
			{
				const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
				const FStaticMeshVertexBuffer& VB = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
				TArray<FSkinWeightInfo> Weights;
				LOD.SkinWeightVertexBuffer.GetSkinWeights(Weights);
				for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
				{
					for (uint32 Local = 0; Local < Section.NumVertices && Where.IsEmpty(); ++Local)
					{
						const int32 Vertex = (int32)(Section.BaseVertexIndex + Local);
						const FVector2f TexelUV = VB.GetVertexUV(Vertex, Data->TexelUVChannel);
						const int32 TX = FMath::Clamp((int32)(TexelUV.X * TexSize), 0, TexSize - 1);
						const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TexSize), 0, TexSize - 1);
						if (TY * TexSize + TX != Steepest.A)
						{
							continue;
						}
						int32 Best = 0;
						for (int32 Influence = 1; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
						{
							Best = Weights[Vertex].InfluenceWeights[Influence] > Weights[Vertex].InfluenceWeights[Best] ? Influence : Best;
						}
						Where = SK->GetRefSkeleton().GetBoneName(Section.BoneMap[Weights[Vertex].InfluenceBones[Best]]).ToString() + TEXT(" bone, ");
					}
				}
			}
		}
		const FVector3f Middle = (Data->VertexPositions[Steepest.A] + Data->VertexPositions[Steepest.B]) * 0.5f;
		UE_LOG(LogSoftbodyBake, Warning,
			TEXT("Paint: the jiggle amount (after Physics Driven Bones) changes sharply over a narrow strip, so that spot may look torn or folded while it jiggles - once the most jiggling point moves ")
			TEXT("%.1f cm, two neighboring vertices slip apart by more than the distance between them (with evenly spread paint, only from %.1f cm). Steepest spot: %smesh position ")
			TEXT("(%.0f, %.0f, %.0f), paint %.2f <-> %.2f, %d such edges. Give the physics-driven bones' skin weights a wide, gradual falloff."),
			1.f / Worst, FMath::Min(1.f / FMath::Max(WorstFlat, UE_SMALL_NUMBER), MeshSize), *Where, Middle.X, Middle.Y, Middle.Z,
			Paint[Steepest.A].B.GetFloat(), Paint[Steepest.B].B.GetFloat(), NumSteep);
	}

	/**
	 * 페인트 층 생성 공용 경로. UV0는 머티리얼 섹션 안에서만 유일하고 슬롯끼리
	 * 0~1 공간을 겹쳐 쓰므로(상용 캐릭터 표준), 소스 마스크 조회는 반드시 렌더 섹션
	 * (정점 범위 + 머티리얼 인덱스) 단위로 갈라야 한다 — MaskForSlot이 슬롯별 마스크+
	 * 채널 매핑을 주고, 없는 슬롯·Keep 채널은 베이크 초기값 유지 (채널별 대체 —
	 * 그레이스케일 게인 마스크가 관성량까지 0으로 덮어쓰는 것을 막는다). 텍스처 채널을 고른 칠은 그 슬롯에 읽을 텍스처가 없으면 그 칠만
	 * Keep으로 읽는다(EffectiveMaskSource) — 같은 슬롯의 상수 칠은 그대로 든다.
	 * PendingSlots: 곧 소프트바디 머티리얼을 받을 슬롯(출렁임 기준에 센다, RescaleInertiaPaint).
	 * 심 위 정점은 렌더 사본마다 UV0가 다르다 — 같은 텍셀에 떨어지는 사본을 채널별 평균.
	 */
	UTexture2D* GeneratePaintInternal(USoftbodyMeshData* Data,
		TFunctionRef<const FSoftbodyPaintSlotMask*(int32 /*MaterialIndex*/)> MaskForSlot, bool bAnyMask,
		TConstArrayView<FName> PendingSlots = {})
	{
		if (!Data || Data->TextureSize <= 0)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("Paint: there is no baked data yet. Press Run Bake first."));
			return nullptr;
		}
		const int32 TexSize = Data->TextureSize;
		const int32 NumTexels = TexSize * TexSize;

		// 초기 채움 = 중립(모두 1). 살이 두꺼울수록 더 흔들리는 효과는 베이크한 무름에 이미 들어 있으므로 관성량을 두께로 다시 곱하지 않는다(ADR-0005).
		// 페인트 층 배치(ADR-0038): R = Jiggle(살의 무름 — 캐리어 굽기가 덩어리 손 밑 살층 강성 1/Jiggle로; 옷 층은 밑 살 값),
		// G = Depth(살 두께 배율 — 캐리어 굽기가 살 두께에 곱한다),
		// B = 관성량(칠 아님 — 1에서 기준 맞춤·Physics Driven Bones·겹친 층), A = Volume(Volume을 칠한 슬롯이 있을 때만 캐리어가 읽는다, bPaintVolume).
		TArray<FFloat16Color> Paint;
		Paint.Init(FFloat16Color(FLinearColor(1.f, 1.f, 1.f, 1.f)), NumTexels);
		bool bAnyVolume = false;

		int32 NumSections = 0;
		int32 NumMasked = 0;
		if (bAnyMask)
		{
			// 같은 마스크가 여러 슬롯에 물릴 수 있으므로 텍스처당 1회만 로드
			TMap<UTexture2D*, TUniquePtr<FMaskImage>> Loaded;
			auto GetMask = [&Loaded](UTexture2D* Tex) -> const FMaskImage*
			{
				if (!Tex)
				{
					return nullptr;
				}
				if (const TUniquePtr<FMaskImage>* Found = Loaded.Find(Tex))
				{
					return Found->Get();
				}
				TUniquePtr<FMaskImage> Img = MakeUnique<FMaskImage>();
				if (!Img->Load(Tex))
				{
					UE_LOG(LogSoftbodyBake, Warning, TEXT("Paint: could not read the pixels of mask texture %s, so the paint that reads its channels stays unpainted (white) on the slots that use it. Use an imported texture that still has its source data."), *Tex->GetName());
					Img.Reset();
				}
				return Loaded.Add(Tex, MoveTemp(Img)).Get();
			};

			// 채널별 누적 — 슬롯마다 대체 채널이 다를 수 있고(채널 매핑), 머티리얼 경계 위
			// 용접 정점은 두 섹션의 사본이 같은 텍셀에 떨어진다
			TArray<FLinearColor> Accum;
			Accum.Init(FLinearColor(0.f, 0.f, 0.f, 0.f), NumTexels);
			TArray<FIntVector4> Count;
			Count.Init(FIntVector4(0, 0, 0, 0), NumTexels);

			auto AccumRange = [&](const FStaticMeshVertexBuffer& VB, const TArray<int32>& Verts,
				const FMaskImage* Mask, const FSoftbodyPaintSlotMask& Map)
			{
				// 측정 굽기·미리보기와 같은 판정(EffectiveMaskSource): 읽은 텍스처가 없는 텍스처 채널은 그 칠만 Keep(흰색)이다.
				// 페인트 층 채널 순서(R Jiggle, G Depth, B 관성량 — 칠 아님, A Volume).
				using SoftbodyBakeShared::EPaintKind;
				const bool bTextureRead = Mask && Mask->Pix;
				ESoftbodyMaskSource Src[4] = { SoftbodyBakeShared::PaintSource(Map, EPaintKind::Jiggle), SoftbodyBakeShared::PaintSource(Map, EPaintKind::Depth),
					ESoftbodyMaskSource::Keep, SoftbodyBakeShared::PaintSource(Map, EPaintKind::Volume) };
				for (ESoftbodyMaskSource& S : Src)
				{
					S = SoftbodyBakeShared::EffectiveMaskSource(S, bTextureRead);
				}
				// Volume을 실제로 칠한 슬롯이 있어야 캐리어 굽기가 그 채널을 읽는다(bPaintVolume).
				bAnyVolume |= Src[3] != ESoftbodyMaskSource::Keep;
				for (int32 v : Verts)
				{
					const FVector2f TexelUV = VB.GetVertexUV(v, Data->TexelUVChannel);
					const int32 TX = FMath::Clamp((int32)(TexelUV.X * TexSize), 0, TexSize - 1);
					const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TexSize), 0, TexSize - 1);
					const int32 Texel = TY * TexSize + TX;
					FLinearColor C(0.f, 0.f, 0.f, 0.f);
					if (bTextureRead)
					{
						const FVector2f UV0 = VB.GetVertexUV(v, 0);
						C = Mask->Sample(UV0.X, UV0.Y);
					}
					for (int32 ch = 0; ch < 4; ++ch)
					{
						if (Src[ch] == ESoftbodyMaskSource::Keep)
						{
							continue;
						}
						(&Accum[Texel].R)[ch] += SampleMaskChannel(C, Src[ch]);
						++Count[Texel][ch];
					}
				}
			};

			// 텍스처가 없는 슬롯은 칠하지 않은 슬롯이다(흰색 = 칠 안 함, ADR-0038 §3.1) — 알리지 않는다. 텍스처는 있는데 못 읽으면 GetMask가 알린다.
			if (USkeletalMesh* SK = Data->SourceSkeletalMesh)
			{
				FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
				if (!RD || RD->LODRenderData.Num() == 0)
				{
					UE_LOG(LogSoftbodyBake, Error, TEXT("Paint: the mesh has no render data yet. Let the mesh finish building, then press Reapply Paint."));
					return nullptr;
				}
				const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
				const FStaticMeshVertexBuffer& VB = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
				if (Data->TexelUVChannel >= (int32)VB.GetNumTexCoords())
				{
					UE_LOG(LogSoftbodyBake, Error, TEXT("Paint: the mesh no longer has the baked UV channel %d. Press Run Bake again."), Data->TexelUVChannel);
					return nullptr;
				}
				for (const FSkelMeshRenderSection& Sec : LOD.RenderSections)
				{
					++NumSections;
					if (const FSoftbodyPaintSlotMask* Map = MaskForSlot((int32)Sec.MaterialIndex))
					{
						// 텍스처가 없어도 슬롯을 건너뛰지 않는다: 텍스처 채널을 고른 칠만 빠지고 상수 칠은 든다.
						const FMaskImage* Mask = NeedsTexture(*Map) ? GetMask(Map->Texture) : nullptr;
						AccumRange(VB, SectionVertexRange((int32)Sec.BaseVertexIndex, (int32)Sec.NumVertices), Mask, *Map);
						++NumMasked;
					}
				}
			}
			else if (UStaticMesh* SM = Data->SourceMesh)
			{
				if (!SM->GetRenderData() || SM->GetRenderData()->LODResources.Num() == 0)
				{
					UE_LOG(LogSoftbodyBake, Error, TEXT("Paint: the mesh has no render data yet. Let the mesh finish building, then press Reapply Paint."));
					return nullptr;
				}
				const FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
				const FStaticMeshVertexBuffer& VB = LOD.VertexBuffers.StaticMeshVertexBuffer;
				if (Data->TexelUVChannel >= (int32)VB.GetNumTexCoords())
				{
					UE_LOG(LogSoftbodyBake, Error, TEXT("Paint: the mesh no longer has the baked UV channel %d. Press Run Bake again."), Data->TexelUVChannel);
					return nullptr;
				}
				for (const FStaticMeshSection& Sec : LOD.Sections)
				{
					++NumSections;
					if (const FSoftbodyPaintSlotMask* Map = MaskForSlot(Sec.MaterialIndex))
					{
						const FMaskImage* Mask = NeedsTexture(*Map) ? GetMask(Map->Texture) : nullptr;
						AccumRange(VB, StaticSectionVertices(LOD, Sec), Mask, *Map);
						++NumMasked;
					}
				}
			}
			else
			{
				UE_LOG(LogSoftbodyBake, Error, TEXT("Paint: the baked data has no source mesh. Press Run Bake again."));
				return nullptr;
			}

			int32 Covered = 0;
			for (int32 i = 0; i < NumTexels; ++i)
			{
				FLinearColor P = Paint[i].GetFloats();
				bool bAnyCh = false;
				for (int32 ch = 0; ch < 4; ++ch)
				{
					if (Count[i][ch] > 0)
					{
						(&P.R)[ch] = (&Accum[i].R)[ch] / (float)Count[i][ch];
						bAnyCh = true;
					}
				}
				if (bAnyCh)
				{
					Paint[i] = FFloat16Color(P);
					++Covered;
				}
			}
			UE_LOG(LogSoftbodyBake, Display, TEXT("Paint: masks applied to %d/%d sections, %d/%d texels replaced"),
				NumMasked, NumSections, Covered, NumTexels);
		}

		TArray<float> TexelSoftness;
		TBitArray<> SoftTexel;
		int32 NumSoftSections = 0;
		Data->InertiaPaintScale = RescaleInertiaPaint(Data, Paint, PendingSlots, TexelSoftness, SoftTexel, NumSoftSections);
		ApplyPhysicsDrivenBones(Data, Paint);
		ApplyLayerLinksToPaint(Data, Paint);
		// 묶음 빠르기는 칠이 다 정해진 뒤(Physics Driven Bones·겹친 층까지)의 처짐으로 굽는다.
		ComputeInertiaGroupSag(Data, Paint, TexelSoftness, SoftTexel, NumSoftSections);
		WarnSteepInertiaPaint(Data, Paint);

		const FString MeshName = Data->SourceSkeletalMesh ? Data->SourceSkeletalMesh->GetName()
			: (Data->SourceMesh ? Data->SourceMesh->GetName() : Data->GetName());
		const FString Folder = FPackageName::GetLongPackagePath(Data->GetPackage()->GetName());
		Data->PaintTex = CreateAttributeTexture(Folder, FString::Printf(TEXT("T_%s_Paint"), *MeshName), TexSize, Paint);
		// 칠이 정해졌으니, 흔들릴 때 음영을 기울이는 값(칠 × 무름의 기울기)을 다시 굽는다.
		BuildInertiaShading(Data, &Paint);
		// Volume(ADR-0024)은 캐리어 굽기가 페인트 A에서 칸마다 가져간다. 페인트만 다시 바꿨으면 캐리어의 값은 옛 것이다.
		// 다시 구우라는 안내는 여기서 하지 않는다 — Run Bake·Reapply Paint는 곧 접촉 지도를 굽는다. 저작 에셋이 안 구울 때만 알린다.
		Data->bPaintVolume = bAnyVolume;
		Data->MarkPackageDirty();
		UE_LOG(LogSoftbodyBake, Display, TEXT("Paint texture: %s (%d², slot paint %s%s)"),
			*Data->PaintTex->GetPathName(), TexSize, bAnyMask ? TEXT("applied") : TEXT("none - baked defaults"),
			bAnyVolume ? TEXT(", Volume paint is read when the contact map is baked") : TEXT(""));
		return Data->PaintTex;
	}
}

namespace SoftbodyBakeShared
{
	bool IsTextureMaskSource(ESoftbodyMaskSource Source)
	{
		return Source == ESoftbodyMaskSource::R || Source == ESoftbodyMaskSource::G || Source == ESoftbodyMaskSource::B
			|| Source == ESoftbodyMaskSource::A;
	}

	ESoftbodyMaskSource EffectiveMaskSource(ESoftbodyMaskSource Source, bool bTextureRead)
	{
		return IsTextureMaskSource(Source) && !bTextureRead ? ESoftbodyMaskSource::Keep : Source;
	}

	bool CanReadMaskTexture(const UTexture2D* Texture)
	{
		return Texture && Texture->Source.IsValid();
	}

	const TCHAR* PaintKindName(EPaintKind Kind)
	{
		switch (Kind)
		{
		case EPaintKind::Depth: return TEXT("Depth");
		case EPaintKind::Jiggle: return TEXT("Jiggle");
		case EPaintKind::Volume: return TEXT("Volume");
		default: return TEXT("Flex");
		}
	}

	ESoftbodyMaskSource PaintSource(const FSoftbodyPaintSlotMask& Mask, EPaintKind Kind)
	{
		const ESoftbodyPaintFill Fills[NumPaintKinds] = { Mask.DepthFill, Mask.JiggleFill, Mask.VolumeFill, Mask.SoftnessFill };
		switch (Fills[(int32)Kind])
		{
		case ESoftbodyPaintFill::White: return ESoftbodyMaskSource::One;
		case ESoftbodyPaintFill::Black: return ESoftbodyMaskSource::Zero;
		default: break;
		}
		if (!Mask.Texture)
		{
			return ESoftbodyMaskSource::Keep;   // 텍스처 없는 슬롯 = 칠 안 함(흰색)
		}
		static const ESoftbodyMaskSource Channels[NumPaintKinds] = { ESoftbodyMaskSource::R, ESoftbodyMaskSource::G, ESoftbodyMaskSource::B, ESoftbodyMaskSource::A };
		return Channels[(int32)Kind];
	}

	void DescribeSlotMaskProblems(const TMap<FName, FSoftbodyPaintSlotMask>& Masks, TArray<FString>& OutUnread, TArray<FString>& OutChanged,
		TOptional<EPaintKind> Kind)
	{
		TSet<const UTexture2D*> Checked;
		TSet<const UTexture2D*> CheckedAlpha;
		for (const TPair<FName, FSoftbodyPaintSlotMask>& Pair : Masks)
		{
			const UTexture2D* Texture = Pair.Value.Texture.Get();
			TArray<EPaintKind, TInlineAllocator<NumPaintKinds>> Reads;
			for (int32 k = 0; k < NumPaintKinds; ++k)
			{
				const EPaintKind K = (EPaintKind)k;
				if ((!Kind.IsSet() || Kind.GetValue() == K) && IsTextureMaskSource(PaintSource(Pair.Value, K)))
				{
					Reads.Add(K);
				}
			}
			if (Reads.Num() == 0)
			{
				continue;
			}
			if (!CanReadMaskTexture(Texture))
			{
				OutUnread.Add(FString::Printf(TEXT("Slot '%s': its Paint Texture has no source pixels, so the paint it should give stays unpainted (white) on this slot. Reimport the texture."),
					*Pair.Key.ToString()));
				continue;
			}
			// 굽기는 원본 픽셀을 텍스처의 sRGB 설정대로 선형으로 풀어 읽는다(FMaskImage). 칠은 숫자라 sRGB가 켜져 있으면 값이 바뀐다.
			// 고정 배치에서는 쓰지 않는 채널이 0인 텍스처(옛 마스크)가 그대로 뼈·폼·판이 된다 — 텍스처 전체에서 0인 채널을 알린다.
			if (!Checked.Contains(Texture))
			{
				Checked.Add(Texture);
				if (Texture->Source.GetGammaSpace(0) != EGammaSpace::Linear)
				{
					OutChanged.Add(FString::Printf(TEXT("Paint texture %s has sRGB on, so the bake reads it as a color and its grays come out darker (0.5 reads as 0.21). Turn off sRGB in the texture's settings."),
						*Texture->GetName()));
				}
			}
			// 알파(Softness)를 칠한 PNG: 엔진의 PNG 들이기는 프로젝트 설정 PNG Infill에 따라 알파가 0인 칸의 RGB를 이웃 색으로 바꾼다(EditorFactories) —
			// 판 칸의 Depth·Jiggle·Volume이 바뀐다. TGA는 그대로 읽는다.
			if (Reads.Contains(EPaintKind::Softness) && !CheckedAlpha.Contains(Texture))
			{
				CheckedAlpha.Add(Texture);
				const FString Source = Texture->AssetImportData ? Texture->AssetImportData->GetFirstFilename() : FString();
				if (Source.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)
					&& GetDefault<UTextureImportSettings>()->GetPNGInfillMapDefault() != ETextureImportPNGInfill::Never)
				{
					OutChanged.Add(FString::Printf(TEXT("Paint texture %s is a PNG with Flex in its alpha. The PNG import can replace the color of fully transparent pixels with a neighbor's, which changes Depth, Jiggle and Volume on plates. Export it as TGA, or set Project Settings > Texture Import > PNG Infill to Never and reimport."),
						*Texture->GetName()));
				}
			}
			FMaskImage Image;
			if (Image.Load(const_cast<UTexture2D*>(Texture)))
			{
				float MaxOf[NumPaintKinds] = { 0.f, 0.f, 0.f, 0.f };
				const int32 Step = FMath::Max(1, FMath::Min(Image.W, Image.H) / 256);
				for (int32 y = 0; y < Image.H; y += Step)
				{
					for (int32 x = 0; x < Image.W; x += Step)
					{
						const FLinearColor& C = Image.Pix[y * Image.W + x];
						MaxOf[0] = FMath::Max(MaxOf[0], C.R);
						MaxOf[1] = FMath::Max(MaxOf[1], C.G);
						MaxOf[2] = FMath::Max(MaxOf[2], C.B);
						MaxOf[3] = FMath::Max(MaxOf[3], C.A);
					}
				}
				for (const EPaintKind K : Reads)
				{
					if (MaxOf[(int32)K] <= 0.01f)
					{
						OutChanged.Add(FString::Printf(TEXT("Slot '%s': %s reads channel %s of %s, which is black everywhere, so the whole slot becomes %s. If that channel is unused, set %s to White everywhere."),
							*Pair.Key.ToString(), PaintKindName(K), MaskSourceName(PaintSource(Pair.Value, K)), *Texture->GetName(),
							K == EPaintKind::Depth ? TEXT("bone") : K == EPaintKind::Jiggle ? TEXT("flesh as firm as bone")
								: K == EPaintKind::Volume ? TEXT("foam") : TEXT("one rigid plate"),
							PaintKindName(K)));
					}
				}
			}
		}
	}

	uint32 MeasurePaintHashOf(const ::FSoftbodyMeasureParams& Params)
	{
		// 칠이 하나도 없으면 0 — 일지 298 전에 구운 칠 없는 자산(지문 0)이 다시 굽지 않아도 맞는다.
		if (Params.VertexDepth.Num() == 0 && Params.VertexJiggle.Num() == 0 && Params.VertexVolume.Num() == 0 && Params.HardMaskHash == 0)
		{
			return 0;
		}
		uint32 Hash = FCrc::MemCrc32(Params.VertexDepth.GetData(), Params.VertexDepth.Num() * sizeof(float));
		Hash = FCrc::MemCrc32(Params.VertexJiggle.GetData(), Params.VertexJiggle.Num() * sizeof(float), Hash);
		Hash = FCrc::MemCrc32(Params.VertexVolume.GetData(), Params.VertexVolume.Num() * sizeof(float), Hash);
		return HashCombine(Hash, Params.HardMaskHash);
	}

	uint32 HashMeasuredPaint(const UObject* Mesh, const TMap<FName, FSoftbodyPaintSlotMask>& Masks)
	{
		FSoftbodyMeasureParams Params;
		const FMeshDescription* MD = nullptr;
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			MD = Skeletal->GetMeshDescription(0);
		}
		else if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			MD = Static->GetMeshDescription(0);
		}
		// 측정 굽기와 같은 조건: 칠이 있을 때만 읽는다.
		if (MD && Masks.Num() > 0)
		{
			SampleMeasurePaint(*MD, Masks, SlotAliasesOf(Mesh), Params, false);
		}
		return MeasurePaintHashOf(Params);
	}

	UTexture2D* GeneratePaintForSlots(USoftbodyMeshData* Data, const TMap<int32, FSoftbodyPaintSlotMask>& SlotMasks,
		TConstArrayView<FName> PendingSlots)
	{
		return GeneratePaintInternal(Data, [&SlotMasks](int32 Slot) { return SlotMasks.Find(Slot); }, SlotMasks.Num() > 0, PendingSlots);
	}

	TArray<float> SamplePaintKind(const UObject* Mesh, const FMeshDescription& MD, const TMap<FName, FSoftbodyPaintSlotMask>& Masks,
		EPaintKind Kind, TArray<bool>& OutPainted)
	{
		// 슬롯 마스크의 키는 슬롯 이름, 메시 설명의 폴리곤 그룹은 들여올 때의 이름 — 측정 굽기와 같은 대응표.
		TMap<FName, FName> Aliases;
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
			{
				Aliases.Add(Material.ImportedMaterialSlotName, Material.MaterialSlotName);
			}
		}
		else if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& Material : Static->GetStaticMaterials())
			{
				Aliases.Add(Material.ImportedMaterialSlotName, Material.MaterialSlotName);
			}
		}
		TArray<float> Out = SampleSlotMaskChannel(MD, Masks, Aliases, Kind, TEXT("Preview"), TEXT(""), TEXT(""), false, &OutPainted);
		if (Out.Num() == 0)
		{
			Out.Init(1.f, MD.Vertices().GetArraySize());
		}
		return Out;
	}

	TArray<FVector3f> RenderedNormals(const UObject* Mesh, const FMeshDescription& MD)
	{
		bool bRecompute = false;
		bool bWeighted = false;
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			if (Static->GetNumSourceModels() > 0)
			{
				const FMeshBuildSettings& Build = Static->GetSourceModel(0).BuildSettings;
				bRecompute = Build.bRecomputeNormals;
				bWeighted = Build.bComputeWeightedNormals;
			}
		}
		else if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			if (const FSkeletalMeshLODInfo* Info = Skeletal->GetLODInfo(0))
			{
				bRecompute = Info->BuildSettings.bRecomputeNormals;
				bWeighted = Info->BuildSettings.bComputeWeightedNormals;
			}
		}
		TArray<FVector3f> Out;
		Out.Init(FVector3f::ZeroVector, MD.VertexInstances().GetArraySize());
		FStaticMeshConstAttributes Stored(MD);
		const TVertexInstanceAttributesConstRef<FVector3f> StoredNormals = Stored.GetVertexInstanceNormals();
		if (!bRecompute)
		{
			if (StoredNormals.IsValid())
			{
				for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
				{
					Out[Instance.GetValue()] = StoredNormals[Instance];
				}
			}
			return Out;
		}
		// 사본에서 다시 계산한다. 엔진 함수가 빈 번호가 있는 메시 설명을 압축하므로 먼저 압축하고 번호 대응을 남긴다.
		FMeshDescription Copy = MD;
		FElementIDRemappings Remappings;
		const bool bCompacted = Copy.NeedsCompact();
		if (bCompacted)
		{
			Copy.Compact(Remappings);
		}
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Copy, 0.f);
		EComputeNTBsFlags Flags = EComputeNTBsFlags::Normals | EComputeNTBsFlags::IgnoreDegenerateTriangles;
		if (bWeighted)
		{
			Flags |= EComputeNTBsFlags::WeightedNTBs;
		}
		FStaticMeshOperations::ComputeTangentsAndNormals(Copy, Flags);
		const TVertexInstanceAttributesConstRef<FVector3f> Computed = FStaticMeshConstAttributes(Copy).GetVertexInstanceNormals();
		for (const FVertexInstanceID Instance : MD.VertexInstances().GetElementIDs())
		{
			const FVertexInstanceID InCopy = bCompacted ? Remappings.GetRemappedVertexInstanceID(Instance) : Instance;
			Out[Instance.GetValue()] = Computed[InCopy];
		}
		return Out;
	}

	TMap<FName, FName> SlotAliasesOf(const UObject* Mesh)
	{
		TMap<FName, FName> Aliases;
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			for (const FSkeletalMaterial& Material : Skeletal->GetMaterials())
			{
				Aliases.Add(Material.ImportedMaterialSlotName, Material.MaterialSlotName);
			}
		}
		else if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			for (const FStaticMaterial& Material : Static->GetStaticMaterials())
			{
				Aliases.Add(Material.ImportedMaterialSlotName, Material.MaterialSlotName);
			}
		}
		return Aliases;
	}

	int32 FindHardPieces(const FMeshDescription& MD, const TMap<FName, FSoftbodyPaintSlotMask>& Masks, const TMap<FName, FName>& SlotAliases,
		TArray<int32>& OutTrianglePieces, TArray<uint8>& OutFloating, uint32& OutHash, bool bLog, TArray<float>* OutTriangleSoftness)
	{
		OutTrianglePieces.Reset();
		OutFloating.Reset();
		OutHash = 0;
		if (OutTriangleSoftness)
		{
			OutTriangleSoftness->Reset();
		}
		// Softness(ADR-0038 §3.5): 흰색(칠 안 함·White everywhere)은 맨살, Black everywhere면 슬롯 전체가 판, 텍스처면 A 채널에서 읽는다.
		TMap<FName, FMaskImage> Images;
		bool bAny = false;
		for (const TPair<FName, FSoftbodyPaintSlotMask>& Pair : Masks)
		{
			const ESoftbodyMaskSource Src = PaintSource(Pair.Value, EPaintKind::Softness);
			if (Src == ESoftbodyMaskSource::One || Src == ESoftbodyMaskSource::Keep)
			{
				continue;
			}
			bAny = true;
			if (IsTextureMaskSource(Src) && !Images.Add(Pair.Key).Load(Pair.Value.Texture))
			{
				if (bLog)
				{
					UE_LOG(LogSoftbodyBake, Warning, TEXT("Flex: slot '%s' reads %s, but its Paint Texture has no source pixels, so that slot has no plates. Reimport the texture, or set Flex to Black everywhere."),
						*Pair.Key.ToString(), MaskSourceName(Src));
				}
				Images.Remove(Pair.Key);
			}
		}
		if (!bAny)
		{
			return 0;
		}
		FStaticMeshConstAttributes Attrs(MD);
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
		const TPolygonGroupAttributesConstRef<FName> SlotNames = Attrs.GetPolygonGroupMaterialSlotNames();
		const TVertexAttributesConstRef<FVector3f> Positions = Attrs.GetVertexPositions();
		const int32 MaxTri = MD.Triangles().GetArraySize();
		TArray<uint8> Hard;
		Hard.Init(0, MaxTri);
		TArray<float> CornerSoftness;
		CornerSoftness.Init(1.f, MaxTri * 3);
		int32 NumHard = 0;
		bool bBelowWhite = false;   // 코너 하나라도 흰색보다 어둡다(판이 없어도 그 칠은 굽는다)
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			FName Slot = SlotNames.IsValid() ? SlotNames[MD.GetTrianglePolygonGroup(TriID)] : NAME_None;
			if (const FName* Alias = SlotAliases.Find(Slot))
			{
				Slot = *Alias;
			}
			const FSoftbodyPaintSlotMask* Map = Masks.Find(Slot);
			// 칠 변환·측정과 같은 판정(EffectiveMaskSource): 읽은 텍스처가 없는 텍스처 채널은 Keep = 흰색 = 맨살.
			const FMaskImage* Image = Images.Find(Slot);
			const ESoftbodyMaskSource Src = EffectiveMaskSource(Map ? PaintSource(*Map, EPaintKind::Softness) : ESoftbodyMaskSource::Keep, Image != nullptr);
			if (Src == ESoftbodyMaskSource::One || Src == ESoftbodyMaskSource::Keep)
			{
				continue;
			}
			bool bHard = true;
			float Corner[3] = { 0.f, 0.f, 0.f };   // Black everywhere = 검정 판
			if (Src != ESoftbodyMaskSource::Zero)
			{
				// 세 코너가 모두 흰색보다 어두운 삼각형만 — 정점 평균은 슬롯 경계의 용접 정점에서 섞인다. 회색은 휘는 판이다(ADR-0038 §4).
				if (!UVs.IsValid() || UVs.GetNumChannels() == 0)
				{
					continue;
				}
				int32 c = 0;
				for (const FVertexInstanceID InstanceID : MD.GetTriangleVertexInstances(TriID))
				{
					const FVector2f UV0 = UVs.Get(InstanceID, 0);
					const float S = SampleMaskChannel(Image->Sample(UV0.X, UV0.Y), Src);
					bHard &= S < SoftbodyHardSurface::PieceThreshold;
					Corner[FMath::Min(c++, 2)] = FMath::Clamp(S, 0.f, 1.f);
				}
			}
			// 연속 굽힘(Flex 칠): 판이 되지 않는 삼각형도 코너 Flex를 남긴다 — 접촉 지도 굽기가 이것으로 모든 칸의 Flex를 굽는다.
			for (int32 c = 0; c < 3; ++c)
			{
				CornerSoftness[TriID.GetValue() * 3 + c] = Corner[c];
				bBelowWhite |= Corner[c] < 1.f;
			}
			if (bHard)
			{
				Hard[TriID.GetValue()] = 1;
				++NumHard;
			}
		}
		if (NumHard == 0)
		{
			// 판은 없어도 흰색보다 어두운 칠은 굽는다 — 코너 Flex는 넘긴다(지문은 판만 보므로 0 그대로).
			if (OutTriangleSoftness && bBelowWhite)
			{
				*OutTriangleSoftness = MoveTemp(CornerSoftness);
			}
			if (bLog)
			{
				UE_LOG(LogSoftbodyBake, Display, TEXT("Flex: read, 0 plates%s"),
					bBelowWhite ? TEXT(" (the paint below white still makes that skin bend stiffer)") : TEXT(" (nothing painted below white)"));
			}
			return 0;
		}

		// 같은 자리 정점 = 같은 점(0.001 메시 로컬 단위로 맞춘 자리). 단단한 삼각형끼리 자리로 이어 무리를 짓는다.
		TMap<FIntVector, int32> NodeOf;
		TArray<int32> Parent;
		const auto NodeAt = [&](const FVertexID Vertex) -> int32
		{
			const FVector3f P = Positions[Vertex] * 1000.f;
			const FIntVector Key(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z));
			if (const int32* Found = NodeOf.Find(Key))
			{
				return *Found;
			}
			const int32 Node = Parent.Add(Parent.Num());
			NodeOf.Add(Key, Node);
			return Node;
		};
		const auto Root = [&Parent](int32 Node)
		{
			while (Parent[Node] != Node)
			{
				Parent[Node] = Parent[Parent[Node]];
				Node = Parent[Node];
			}
			return Node;
		};
		TArray<int32> TriNode;
		TriNode.Init(INDEX_NONE, MaxTri);
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			if (!Hard[TriID.GetValue()])
			{
				continue;
			}
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(TriID);
			const int32 A = NodeAt(Corners[0]);
			for (int32 c = 1; c < 3; ++c)
			{
				const int32 RA = Root(A);
				const int32 RB = Root(NodeAt(Corners[c]));
				if (RA != RB)
				{
					Parent[RB] = RA;
				}
			}
			TriNode[TriID.GetValue()] = A;
		}
		// 단단하지 않은 삼각형이 닿는 점 — 그 점을 가진 조각은 겉면과 이어져 있다(떠 있지 않다).
		TArray<uint8> SoftNode;
		SoftNode.Init(0, Parent.Num());
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			if (Hard[TriID.GetValue()])
			{
				continue;
			}
			for (const FVertexID Corner : MD.GetTriangleVertices(TriID))
			{
				const FVector3f P = Positions[Corner] * 1000.f;
				if (const int32* Node = NodeOf.Find(FIntVector(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z))))
				{
					SoftNode[*Node] = 1;
				}
			}
		}
		TMap<int32, int32> PieceOfRoot;
		OutTrianglePieces.Init(0, MaxTri);
		int32 Dropped = 0;
		for (int32 Tri = 0; Tri < MaxTri; ++Tri)
		{
			if (TriNode[Tri] == INDEX_NONE)
			{
				continue;
			}
			const int32 R = Root(TriNode[Tri]);
			int32* Piece = PieceOfRoot.Find(R);
			if (!Piece)
			{
				if (PieceOfRoot.Num() >= SoftbodyHardSurface::MaxPieces)
				{
					++Dropped;
					continue;
				}
				Piece = &PieceOfRoot.Add(R, PieceOfRoot.Num() + 1);
				OutFloating.Add(1);
			}
			OutTrianglePieces[Tri] = *Piece;
		}
		for (int32 Node = 0; Node < Parent.Num(); ++Node)
		{
			if (SoftNode[Node])
			{
				if (const int32* Piece = PieceOfRoot.Find(Root(Node)))
				{
					OutFloating[*Piece - 1] = 0;
				}
			}
		}
		// 지문 = 조각 소속 + 판 코너 Softness(G16로 반올림). 회색만 바꿔도 판의 굳기가 바뀌므로 측정 칠 지문(MeasurePaintHashOf)이 달라져
		// Reapply Paint가 Run Bake로 돈다(P04 — 측정과 접촉이 같은 칠을 본다). 판 밖 코너 Flex는 넣지 않는다 — 측정은 그 칠을 읽지 않고, 접촉 지도는
		// Reapply Paint가 다시 굽는다.
		TArray<uint16> SoftnessBits;
		SoftnessBits.Reserve(NumHard * 3);
		for (int32 Tri = 0; Tri < MaxTri; ++Tri)
		{
			if (OutTrianglePieces[Tri] == 0)
			{
				continue;   // 판 밖 코너 Flex는 그대로 넘긴다(연속 굽힘)
			}
			for (int32 c = 0; c < 3; ++c)
			{
				SoftnessBits.Add((uint16)FMath::Clamp(FMath::RoundToInt(CornerSoftness[Tri * 3 + c] * 65535.f), 0, 65535));
			}
		}
		OutHash = FCrc::MemCrc32(OutTrianglePieces.GetData(), OutTrianglePieces.Num() * sizeof(int32));
		OutHash = FCrc::MemCrc32(SoftnessBits.GetData(), SoftnessBits.Num() * sizeof(uint16), OutHash);
		if (OutTriangleSoftness)
		{
			*OutTriangleSoftness = MoveTemp(CornerSoftness);
		}
		if (bLog)
		{
			int32 NumFloating = 0;
			for (const uint8 F : OutFloating)
			{
				NumFloating += F;
			}
			UE_LOG(LogSoftbodyBake, Display, TEXT("Flex: %d plates (%d floating on another surface, %d part of the surface), %d triangles"),
				OutFloating.Num(), NumFloating, OutFloating.Num() - NumFloating, NumHard);
			if (Dropped > 0)
			{
				UE_LOG(LogSoftbodyBake, Warning, TEXT("Flex: more than %d plates; %d triangles beyond that bend with their paint instead of moving as plates. Paint fewer separate plates."),
					SoftbodyHardSurface::MaxPieces, Dropped);
			}
		}
		return OutFloating.Num();
	}

	void ConvertLegacySlotMask(FSoftbodyPaintSlotMask& Mask, FName Slot, TArray<FString>& OutNotes)
	{
		const FString SlotName = Slot.ToString();
		const ESoftbodyMaskSource Keep = ESoftbodyMaskSource::Keep;
		const ESoftbodyMaskSource OldJiggle = Mask.FleshSoftness != Keep ? Mask.FleshSoftness : Mask.Inertia;
		if (Mask.FleshSoftness == Keep && Mask.Inertia != Keep)
		{
			OutNotes.Add(FString::Printf(TEXT("Slot '%s': the old Jiggle Amount paint becomes Jiggle (how soft the flesh is). The two are close but not the same, so check the jiggle."), *SlotName));
		}
		else if (Mask.FleshSoftness != Keep && Mask.Inertia != Keep)
		{
			OutNotes.Add(FString::Printf(TEXT("Slot '%s': the old Jiggle Amount paint is dropped. Jiggle comes from the old Flesh Softness paint, and bone now comes from Depth."), *SlotName));
		}
		if (Mask.SkinTension != Keep)
		{
			OutNotes.Add(FString::Printf(TEXT("Slot '%s': the old Skin Tension paint is dropped. Skin Tension is one value on the Softbody component; paint Flex for a stiff patch."), *SlotName));
		}
		// 새 채널(R G B A)의 옛 소스. 옛 Keep 뜻: 칠 셋은 흰색 1, Hard Surface는 0(조각 없음). Softness = 1 − Hard Surface.
		const ESoftbodyMaskSource Src[NumPaintKinds] = { Mask.NormalGain, OldJiggle, Mask.Volume, Mask.HardSurface };
		ESoftbodyPaintFill* Fills[NumPaintKinds] = { &Mask.DepthFill, &Mask.JiggleFill, &Mask.VolumeFill, &Mask.SoftnessFill };
		bool bNeedMain = false;
		bool bNeedHard = false;
		for (int32 k = 0; k < NumPaintKinds; ++k)
		{
			const bool bHardChannel = k == (int32)EPaintKind::Softness;
			const ESoftbodyMaskSource S = Src[k];
			if (IsTextureMaskSource(S))
			{
				*Fills[k] = ESoftbodyPaintFill::Texture;
				(bHardChannel ? bNeedHard : bNeedMain) = true;
				continue;
			}
			// 상수: 새 값이 1이면 White(칠 안 함), 0이면 Black.
			const bool bOne = bHardChannel ? (S != ESoftbodyMaskSource::One) : (S != ESoftbodyMaskSource::Zero);
			*Fills[k] = bOne ? ESoftbodyPaintFill::White : ESoftbodyPaintFill::Black;
		}
		FMaskImage Main;
		FMaskImage Hard;
		const bool bMain = bNeedMain && Main.Load(Mask.Texture);
		const bool bHard = bNeedHard && Hard.Load(Mask.HardSurfaceTexture);
		if (bNeedMain && !bMain)
		{
			OutNotes.Add(FString::Printf(TEXT("Slot '%s': could not read the old Mask Texture, so its texture paint becomes unpainted (white)."), *SlotName));
		}
		if (bNeedHard && !bHard)
		{
			OutNotes.Add(FString::Printf(TEXT("Slot '%s': could not read the old Hard Surface Texture, so Flex becomes unpainted (bare skin)."), *SlotName));
		}
		for (int32 k = 0; k < NumPaintKinds; ++k)
		{
			const bool bHardChannel = k == (int32)EPaintKind::Softness;
			if (*Fills[k] == ESoftbodyPaintFill::Texture && !(bHardChannel ? bHard : bMain))
			{
				*Fills[k] = ESoftbodyPaintFill::White;
			}
		}
		UTexture2D* NewTexture = nullptr;
		if (bMain || bHard)
		{
			const FMaskImage& Base = bMain ? Main : Hard;
			UTexture2D* BaseTexture = bMain ? Mask.Texture.Get() : Mask.HardSurfaceTexture.Get();
			const int32 W = Base.W;
			const int32 H = Base.H;
			TArray<FColor> Pixels;
			Pixels.SetNumUninitialized(W * H);
			const auto ToByte = [](float Value) { return (uint8)FMath::Clamp(FMath::RoundToInt32(Value * 255.f), 0, 255); };
			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					// FMaskImage::Sample과 같은 꼭짓점 맞춤(U × (W − 1)) — 같은 크기면 픽셀을 그대로 옮긴다.
					const float U = W > 1 ? (float)x / (float)(W - 1) : 0.f;
					const float V = H > 1 ? (float)y / (float)(H - 1) : 0.f;
					const FLinearColor CM = bMain ? Main.Sample(U, V) : FLinearColor::White;
					const FLinearColor CH = bHard ? Hard.Sample(U, V) : FLinearColor::Black;
					float Value[NumPaintKinds];
					for (int32 k = 0; k < NumPaintKinds; ++k)
					{
						if (*Fills[k] != ESoftbodyPaintFill::Texture)
						{
							Value[k] = 1.f;   // 이 채널은 Fill이 정한다 — 텍스처 채널은 흰색
						}
						else if (k == (int32)EPaintKind::Softness)
						{
							Value[k] = 1.f - FMath::Clamp(SampleMaskChannel(CH, Src[k]), 0.f, 1.f);
						}
						else
						{
							Value[k] = FMath::Clamp(SampleMaskChannel(CM, Src[k]), 0.f, 1.f);
						}
					}
					Pixels[y * W + x] = FColor(ToByte(Value[0]), ToByte(Value[1]), ToByte(Value[2]), ToByte(Value[3]));
				}
			}
			const FString Folder = FPackageName::GetLongPackagePath(BaseTexture->GetPackage()->GetName());
			const FString Name = BaseTexture->GetName() + TEXT("_Paint");
			UPackage* Pkg = BakeTexturePackage(Folder / Name, Name);
			NewTexture = NewObject<UTexture2D>(Pkg, FName(*Name), RF_Public | RF_Standalone);
			NewTexture->Source.Init(W, H, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
			NewTexture->SRGB = false;
			NewTexture->CompressionSettings = TC_Masks;
			NewTexture->UpdateResource();
			NewTexture->PostEditChange();
			FAssetRegistryModule::AssetCreated(NewTexture);
			Pkg->MarkPackageDirty();
			OutNotes.Add(FString::Printf(TEXT("Slot '%s': new Paint Texture %s (%dx%d)."), *SlotName, *NewTexture->GetPathName(), W, H));
		}
		Mask.Texture = NewTexture;
		Mask.NormalGain = ESoftbodyMaskSource::G;
		Mask.Inertia = Keep;
		Mask.Volume = Keep;
		Mask.SkinTension = Keep;
		Mask.FleshSoftness = Keep;
		Mask.HardSurface = ESoftbodyMaskSource::Zero;
		Mask.HardSurfaceTexture = nullptr;
	}
}

bool USoftbodyBakeLibrary::DumpPaintSectionStats(USoftbodyMeshData* Data)
{
	if (!Data || Data->TextureSize <= 0)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("PaintStats: no baked data. Press Run Bake first."));
		return false;
	}
	const int32 TexSize = Data->TextureSize;
	const int32 NumTexels = TexSize * TexSize;

	auto LoadTexels = [NumTexels](UTexture2D* Tex, TArray64<uint8>& Raw) -> const FFloat16Color*
	{
		if (!Tex || !Tex->Source.GetMipData(Raw, 0) || Raw.Num() < NumTexels * (int64)sizeof(FFloat16Color))
		{
			return nullptr;
		}
		return (const FFloat16Color*)Raw.GetData();
	};
	TArray64<uint8> RawPaint;
	const FFloat16Color* Paint = LoadTexels(Data->PaintTex, RawPaint);
	if (!Paint)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("PaintStats: could not read the paint texture. Press Reapply Paint."));
		return false;
	}

	// 섹션 → 소속 텍셀 집합 (심 사본 중복 제거) → 채널 평균 + 최종 Paint.B 활성률.
	auto DumpSection = [&](int32 SectionIdx, int32 MaterialIndex, const FStaticMeshVertexBuffer& VB, const TArray<int32>& Verts)
	{
		TSet<int32> Texels;
		for (int32 v : Verts)
		{
			const FVector2f TexelUV = VB.GetVertexUV(v, Data->TexelUVChannel);
			const int32 TX = FMath::Clamp((int32)(TexelUV.X * TexSize), 0, TexSize - 1);
			const int32 TY = FMath::Clamp((int32)(TexelUV.Y * TexSize), 0, TexSize - 1);
			Texels.Add(TY * TexSize + TX);
		}
		FLinearColor Avg(0, 0, 0, 0);
		int32 InertiaLive = 0;
		for (int32 t : Texels)
		{
			const FLinearColor P = Paint[t].GetFloats();
			Avg += P;
			InertiaLive += P.B > 1e-3f ? 1 : 0;
		}
		const float N = (float)FMath::Max(Texels.Num(), 1);
		Avg /= N;
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("PaintStats: section %d (slot %d, %d vertices, %d texels) mean Jiggle %.3f, Depth %.3f, jiggle scale %.3f, Volume %.3f | jiggles %.0f%%"),
			SectionIdx, MaterialIndex, Verts.Num(), Texels.Num(), Avg.R, Avg.G, Avg.B, Avg.A,
			100.f * InertiaLive / N);
	};

	if (USkeletalMesh* SK = Data->SourceSkeletalMesh)
	{
		FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
		if (!RD || RD->LODRenderData.Num() == 0)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("PaintStats: the mesh has no render data yet."));
			return false;
		}
		const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
		const FStaticMeshVertexBuffer& VB = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		for (int32 s = 0; s < LOD.RenderSections.Num(); ++s)
		{
			const FSkelMeshRenderSection& Sec = LOD.RenderSections[s];
			DumpSection(s, (int32)Sec.MaterialIndex, VB, SectionVertexRange((int32)Sec.BaseVertexIndex, (int32)Sec.NumVertices));
		}
	}
	else if (UStaticMesh* SM = Data->SourceMesh)
	{
		if (!SM->GetRenderData() || SM->GetRenderData()->LODResources.Num() == 0)
		{
			UE_LOG(LogSoftbodyBake, Error, TEXT("PaintStats: the mesh has no render data yet."));
			return false;
		}
		const FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
		const FStaticMeshVertexBuffer& VB = LOD.VertexBuffers.StaticMeshVertexBuffer;
		for (int32 s = 0; s < LOD.Sections.Num(); ++s)
		{
			const FStaticMeshSection& Sec = LOD.Sections[s];
			DumpSection(s, Sec.MaterialIndex, VB, StaticSectionVertices(LOD, Sec));
		}
	}
	else
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("PaintStats: the baked data has no source mesh. Press Run Bake again."));
		return false;
	}
	return true;
}

bool USoftbodyBakeLibrary::DumpBakeCoverageStats(USoftbodyMeshData* Data)
{
	if (!Data || Data->BakeDiagnosticsVersion < 1
		|| Data->TextureSize <= 0)
	{
		UE_LOG(LogSoftbodyBake, Warning,
			TEXT("CoverageStats: no diagnostic data. Press Run Bake again with this plugin version."));
		return false;
	}

	const int32 NumVertices = Data->VertexPositions.Num();
	const bool bArraysValid =
		Data->VertexEmbeddingStatus.Num() == NumVertices
		&& Data->VertexEmbeddingMinWeight.Num() == NumVertices
		&& Data->VertexCoreConnection.Num() == NumVertices
		&& Data->VertexInertiaResponseRms.Num() == NumVertices;
	if (NumVertices <= 0 || !bArraysValid)
	{
		UE_LOG(LogSoftbodyBake, Error,
			TEXT("CoverageStats: per-vertex diagnostic arrays do not match the vertex count. Press Run Bake again."));
		return false;
	}

	TArray64<uint8> RawPaint;
	const FFloat16Color* Paint = nullptr;
	const int32 NumTexels = Data->TextureSize * Data->TextureSize;
	if (Data->PaintTex
		&& Data->PaintTex->Source.GetMipData(RawPaint, 0)
		&& RawPaint.Num() >=
			NumTexels * (int64)sizeof(FFloat16Color))
	{
		Paint = reinterpret_cast<const FFloat16Color*>(
			RawPaint.GetData());
	}

	struct FCoverageStats
	{
		int32 Total = 0;
		int32 Failed = 0;
		int32 Extrapolated = 0;
		int32 Contained = 0;
		int32 CoreDisconnected = 0;
		int32 LowResponse = 0;
		int32 BaryCount = 0;
		int32 PaintCount = 0;
		double MinBary = TNumericLimits<double>::Max();
		double SumBary = 0.0;
		double SumResponse = 0.0;
		double SumPaintG = 0.0;
		double SumPaintB = 0.0;
	};

	constexpr float LowResponseThreshold = 1e-3f;
	auto AddVertex = [&](FCoverageStats& Stats, int32 Vertex)
	{
		if (Vertex < 0 || Vertex >= NumVertices)
		{
			return;
		}
		++Stats.Total;
		const uint8 Status = Data->VertexEmbeddingStatus[Vertex];
		Stats.Failed += Status == 1 ? 1 : 0;
		Stats.Extrapolated += Status == 2 ? 1 : 0;
		Stats.Contained += Status == 3 ? 1 : 0;
		Stats.CoreDisconnected +=
			Data->VertexCoreConnection[Vertex] == 2 ? 1 : 0;
		const float Rms = Data->VertexInertiaResponseRms[Vertex];
		Stats.LowResponse += Rms <= LowResponseThreshold ? 1 : 0;
		Stats.SumResponse += Rms;
		if (Status != 1)
		{
			const float MinWeight =
				Data->VertexEmbeddingMinWeight[Vertex];
			Stats.MinBary = FMath::Min(
				Stats.MinBary, (double)MinWeight);
			Stats.SumBary += MinWeight;
			++Stats.BaryCount;
		}
		if (Paint && Vertex < NumTexels)
		{
			const FLinearColor P = Paint[Vertex].GetFloats();
			Stats.SumPaintG += P.G;
			Stats.SumPaintB += P.B;
			++Stats.PaintCount;
		}
	};

	auto LogCoverageStats = [&](const FString& Label,
		const FCoverageStats& Stats)
	{
		const double N = FMath::Max(Stats.Total, 1);
		const double BaryN = FMath::Max(Stats.BaryCount, 1);
		const double PaintN = FMath::Max(Stats.PaintCount, 1);
		const double MinBary = Stats.BaryCount > 0
			? Stats.MinBary : -1.0;
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("CoverageStats %s: %d vertices | inside %.1f%%, extrapolated %.1f%%, "
				"not embedded %.1f%%, not connected to rigid core %.1f%%, "
				"barely moved by inertia (<= %.0e) %.1f%% | embedding weight min/avg %.4f/%.4f, "
				"mean inertia response %.4f, mean Depth paint/jiggle scale %.3f/%.3f"),
			*Label, Stats.Total,
			100.0 * Stats.Contained / N,
			100.0 * Stats.Extrapolated / N,
			100.0 * Stats.Failed / N,
			100.0 * Stats.CoreDisconnected / N,
			(double)LowResponseThreshold,
			100.0 * Stats.LowResponse / N,
			MinBary, Stats.SumBary / BaryN,
			Stats.SumResponse / N,
			Stats.SumPaintG / PaintN,
			Stats.SumPaintB / PaintN);
	};

	FCoverageStats Global;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		AddVertex(Global, Vertex);
	}
	LogCoverageStats(TEXT("all"), Global);

	auto DumpSection = [&](const FString& Label,
		const FStaticMeshVertexBuffer& VertexBuffer,
		const TSet<int32>& RenderVertices)
	{
		TSet<int32> SourceTexels;
		for (const int32 RenderVertex : RenderVertices)
		{
			const FVector2f UV = VertexBuffer.GetVertexUV(
				RenderVertex, Data->TexelUVChannel);
			const int32 X = FMath::Clamp(
				(int32)(UV.X * Data->TextureSize),
				0, Data->TextureSize - 1);
			const int32 Y = FMath::Clamp(
				(int32)(UV.Y * Data->TextureSize),
				0, Data->TextureSize - 1);
			const int32 Texel = Y * Data->TextureSize + X;
			if (Texel < NumVertices)
			{
				SourceTexels.Add(Texel);
			}
		}
		FCoverageStats SectionStats;
		for (const int32 Texel : SourceTexels)
		{
			AddVertex(SectionStats, Texel);
		}
		LogCoverageStats(Label, SectionStats);
	};

	if (USkeletalMesh* SkeletalMesh = Data->SourceSkeletalMesh)
	{
		FSkeletalMeshRenderData* RenderData =
			SkeletalMesh->GetResourceForRendering();
		if (RenderData && RenderData->LODRenderData.Num() > 0)
		{
			const FSkeletalMeshLODRenderData& LOD =
				RenderData->LODRenderData[0];
			const FStaticMeshVertexBuffer& VertexBuffer =
				LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
			for (int32 SectionIndex = 0;
				SectionIndex < LOD.RenderSections.Num();
				++SectionIndex)
			{
				const FSkelMeshRenderSection& Section =
					LOD.RenderSections[SectionIndex];
				TSet<int32> RenderVertices;
				for (int32 Vertex = (int32)Section.BaseVertexIndex;
					Vertex < (int32)(Section.BaseVertexIndex
						+ Section.NumVertices);
					++Vertex)
				{
					RenderVertices.Add(Vertex);
				}
				const FName SlotName =
					SkeletalMesh->GetMaterials().IsValidIndex(
						Section.MaterialIndex)
					? SkeletalMesh->GetMaterials()[
						Section.MaterialIndex].MaterialSlotName
					: NAME_None;
				DumpSection(FString::Printf(
					TEXT("render section %d (slot %d:%s)"),
					SectionIndex, Section.MaterialIndex,
					*SlotName.ToString()),
					VertexBuffer, RenderVertices);
			}
		}
	}
	else if (UStaticMesh* StaticMesh = Data->SourceMesh)
	{
		if (StaticMesh->GetRenderData()
			&& StaticMesh->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LOD =
				StaticMesh->GetRenderData()->LODResources[0];
			const FStaticMeshVertexBuffer& VertexBuffer =
				LOD.VertexBuffers.StaticMeshVertexBuffer;
			for (int32 SectionIndex = 0;
				SectionIndex < LOD.Sections.Num();
				++SectionIndex)
			{
				const FStaticMeshSection& Section =
					LOD.Sections[SectionIndex];
				TSet<int32> RenderVertices;
				for (uint32 Index = Section.FirstIndex;
					Index < Section.FirstIndex
						+ Section.NumTriangles * 3;
					++Index)
				{
					RenderVertices.Add(
						(int32)LOD.IndexBuffer.GetIndex(Index));
				}
				const FName SlotName =
					StaticMesh->GetStaticMaterials().IsValidIndex(
						Section.MaterialIndex)
					? StaticMesh->GetStaticMaterials()[
						Section.MaterialIndex].MaterialSlotName
					: NAME_None;
				DumpSection(FString::Printf(
					TEXT("render section %d (slot %d:%s)"),
					SectionIndex, Section.MaterialIndex,
					*SlotName.ToString()),
					VertexBuffer, RenderVertices);
			}
		}
	}
	return true;
}

bool USoftbodyBakeLibrary::DumpInertiaSoftness(USoftbodyMeshData* Data, const FString& CsvPath)
{
	const int32 NumVertices = Data ? Data->VertexPositions.Num() : 0;
	if (NumVertices <= 0 || Data->NumInertiaResponses != 3 || Data->VertexInertiaResponseShapes.Num() != 3 * NumVertices)
	{
		UE_LOG(LogSoftbodyBake, Warning, TEXT("InertiaSoftness: this data has no baked flesh softness for inertia. Press Run Bake first."));
		return false;
	}

	TArray64<uint8> RawPaint;
	const FFloat16Color* Paint = nullptr;
	if (Data->PaintTex && Data->PaintTex->Source.GetMipData(RawPaint, 0)
		&& RawPaint.Num() >= (int64)Data->TextureSize * Data->TextureSize * (int64)sizeof(FFloat16Color))
	{
		Paint = reinterpret_cast<const FFloat16Color*>(RawPaint.GetData());
	}

	// 정점마다 흔들림을 끄는 본 묶음 셋과 몫(머티리얼이 읽는 표 그대로).
	TArray64<uint8> RawGroups;
	const FFloat16Color* Groups = nullptr;
	if (Data->AttributeTex2 && Data->AttributeTex2->Source.GetMipData(RawGroups, 0)
		&& RawGroups.Num() >= (int64)Data->TextureSize * Data->TextureSize * (int64)sizeof(FFloat16Color))
	{
		Groups = reinterpret_cast<const FFloat16Color*>(RawGroups.GetData());
	}

	// 정점이 가장 큰 웨이트로 붙은 본. 구운 정점 번호는 메시 디스크립션의 정점 순서 그대로다(자리가 다르면 본 없이 낸다).
	TArray<FName> BoneOf;
	BoneOf.Init(NAME_None, NumVertices);
	FString Triangles;
	/** 메시 디스크립션의 삼각형을 구운 정점 번호 셋으로 적는다. */
	const auto AppendTriangles = [&Triangles, NumVertices](const FMeshDescription& MD)
	{
		TMap<FVertexID, int32> IndexOf;
		IndexOf.Reserve(NumVertices);
		int32 Next = 0;
		for (const FVertexID VertID : MD.Vertices().GetElementIDs())
		{
			IndexOf.Add(VertID, Next++);
		}
		for (const FTriangleID TriID : MD.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> Corners = MD.GetTriangleVertices(TriID);
			Triangles += FString::Printf(TEXT("%d,%d,%d\n"), IndexOf[Corners[0]], IndexOf[Corners[1]], IndexOf[Corners[2]]);
		}
	};
	if (USkeletalMesh* Mesh = Data->SourceSkeletalMesh)
	{
		const FMeshDescription* MD = Mesh->GetMeshDescription(0);
		FSkinWeightsVertexAttributesConstRef SkinWeights;
		if (MD)
		{
			SkinWeights = FSkeletalMeshConstAttributes(*MD).GetVertexSkinWeights();
		}
		if (MD && SkinWeights.IsValid() && MD->Vertices().Num() == NumVertices)
		{
			const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
			const TVertexAttributesConstRef<FVector3f> Positions = MD->GetVertexPositions();
			int32 Index = 0;
			bool bSameOrder = true;
			for (const FVertexID VertID : MD->Vertices().GetElementIDs())
			{
				bSameOrder &= Positions[VertID].Equals(Data->VertexPositions[Index], 0.01f);
				int32 Best = INDEX_NONE;
				float BestWeight = 0.f;
				for (UE::AnimationCore::FBoneWeight Weight : SkinWeights.Get(VertID))
				{
					if (Weight.GetWeight() > BestWeight)
					{
						BestWeight = Weight.GetWeight();
						Best = (int32)Weight.GetBoneIndex();
					}
				}
				BoneOf[Index++] = RefSkeleton.IsValidIndex(Best) ? RefSkeleton.GetBoneName(Best) : NAME_None;
			}
			if (!bSameOrder)
			{
				UE_LOG(LogSoftbodyBake, Warning, TEXT("InertiaSoftness: the mesh changed after the bake, so values cannot be split by bone - only the overall value is listed."));
				BoneOf.Init(NAME_None, NumVertices);
			}
			else if (!CsvPath.IsEmpty())
			{
				AppendTriangles(*MD);
			}
		}
	}
	else if (UStaticMesh* StaticMesh = Data->SourceMesh)
	{
		// 스태틱 메시도 삼각형을 같이 낸다(구운 값을 겉면 위에서 볼 때 쓴다). 정점 순서가 굽던 때와 같을 때만.
		const FMeshDescription* MD = CsvPath.IsEmpty() ? nullptr : StaticMesh->GetMeshDescription(0);
		if (MD && MD->Vertices().Num() == NumVertices)
		{
			const TVertexAttributesConstRef<FVector3f> Positions = MD->GetVertexPositions();
			int32 Index = 0;
			bool bSameOrder = true;
			for (const FVertexID VertID : MD->Vertices().GetElementIDs())
			{
				bSameOrder &= Positions[VertID].Equals(Data->VertexPositions[Index++], 0.01f);
			}
			if (bSameOrder)
			{
				AppendTriangles(*MD);
			}
		}
	}

	struct FBoneSoftness
	{
		int32 Count = 0;
		double Sum = 0.0;
		float Max = 0.f;
	};
	TMap<FName, FBoneSoftness> ByBone;
	FString Csv = TEXT("x,y,z,softness,paint,bone,xx,xy,xz,yx,yy,yz,zx,zy,zz,group0,group1,weight0,group2,weight2\n");
	for (int32 v = 0; v < NumVertices; ++v)
	{
		// 당긴 방향으로 밀리는 정도의 세 방향 평균(디버그 보기 4와 같은 값).
		float Softness = 0.f;
		for (int32 k = 0; k < 3; ++k)
		{
			Softness += Data->VertexInertiaResponseShapes[k * NumVertices + v][k] / 3.f;
		}
		const float PaintB = Paint ? Paint[v].GetFloats().B : 1.f;
		FBoneSoftness& Entry = ByBone.FindOrAdd(BoneOf[v]);
		++Entry.Count;
		Entry.Sum += Softness * PaintB;
		Entry.Max = FMath::Max(Entry.Max, Softness * PaintB);
		if (!CsvPath.IsEmpty())
		{
			const FVector3f& P = Data->VertexPositions[v];
			Csv += FString::Printf(TEXT("%.3f,%.3f,%.3f,%.5f,%.4f,%s"), P.X, P.Y, P.Z, Softness, PaintB, *BoneOf[v].ToString());
			// 당긴 방향(x, y, z)마다 밀리는 벡터, 그리고 본 묶음 셋과 첫째·셋째 묶음의 몫(둘째의 몫 = 나머지).
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& Shape = Data->VertexInertiaResponseShapes[k * NumVertices + v];
				Csv += FString::Printf(TEXT(",%.5f,%.5f,%.5f"), Shape.X, Shape.Y, Shape.Z);
			}
			if (Groups)
			{
				const SoftbodyBakeShared::FVertexGroups Group = SoftbodyBakeShared::FVertexGroups::Decode(Groups[v].GetFloats());
				Csv += FString::Printf(TEXT(",%d,%d,%.4f,%d,%.4f\n"), Group.Group[0], Group.Group[1], Group.Weight[0], Group.Group[2], Group.Weight[2]);
			}
			else
			{
				Csv += TEXT(",-1,-1,1.0000,-1,0.0000\n");
			}
		}
	}
	ByBone.ValueSort([](const FBoneSoftness& A, const FBoneSoftness& B) { return A.Sum / A.Count > B.Sum / B.Count; });
	FString Table;
	for (const TPair<FName, FBoneSoftness>& Pair : ByBone)
	{
		Table += FString::Printf(TEXT("%s%s %.3f/%.3f(%d)"), Table.IsEmpty() ? TEXT("") : TEXT(", "),
			Pair.Key.IsNone() ? TEXT("all") : *Pair.Key.ToString(), Pair.Value.Sum / Pair.Value.Count, Pair.Value.Max, Pair.Value.Count);
	}
	UE_LOG(LogSoftbodyBake, Display, TEXT("InertiaSoftness %s: per bone, how far the flesh moves under 1 g, mean/max (vertex count); softest point = 1 x paint - %s"),
		*Data->GetName(), *Table);
	if (!CsvPath.IsEmpty() && !FFileHelper::SaveStringToFile(Csv, *CsvPath))
	{
		UE_LOG(LogSoftbodyBake, Warning, TEXT("InertiaSoftness: could not write %s"), *CsvPath);
		return false;
	}
	if (!Triangles.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Triangles, *(FPaths::GetBaseFilename(CsvPath, false) + TEXT("_triangles.csv")));
	}
	// 관성 음영 줄(관성 표 3~6줄: 노멀 성분과 그 기울기, 접은 노멀, 묶음 몫의 기울기)이 있으면 머티리얼이 읽는 값 그대로 _shading 파일로 낸다.
	const int32 ShadingRows = USoftbodyMeshData::InertiaRowsWithShading;
	const int64 NumTexels = (int64)Data->TextureSize * Data->TextureSize;
	TArray64<uint8> RawRows;
	if (!CsvPath.IsEmpty() && Data->GetInertiaResponseRows() >= ShadingRows && Data->InertiaResponseTex
		&& Data->InertiaResponseTex->Source.GetMipData(RawRows, 0)
		&& RawRows.Num() >= NumTexels * ShadingRows * (int64)sizeof(FFloat16Color))
	{
		const FFloat16Color* Rows = reinterpret_cast<const FFloat16Color*>(RawRows.GetData());
		FString Shading;
		for (int32 Row = 3; Row < ShadingRows; ++Row)
		{
			Shading += FString::Printf(TEXT("%srow%dx,row%dy,row%dz,row%da"), Row > 3 ? TEXT(",") : TEXT(""), Row, Row, Row, Row);
		}
		Shading += LINE_TERMINATOR;
		for (int32 v = 0; v < NumVertices; ++v)
		{
			for (int32 Row = 3; Row < ShadingRows; ++Row)
			{
				const FLinearColor Value = Rows[Row * NumTexels + v].GetFloats();
				Shading += FString::Printf(TEXT("%s%.6g,%.6g,%.6g,%.6g"), Row > 3 ? TEXT(",") : TEXT(""), Value.R, Value.G, Value.B, Value.A);
			}
			Shading += LINE_TERMINATOR;
		}
		FFileHelper::SaveStringToFile(Shading, *(FPaths::GetBaseFilename(CsvPath, false) + TEXT("_shading.csv")));
	}
	return true;
}

bool USoftbodyBakeLibrary::BakeInertiaShading(USoftbodyMeshData* Data)
{
	if (!BuildInertiaShading(Data, nullptr))
	{
		UE_LOG(LogSoftbodyBake, Warning, TEXT("BakeInertiaShading: %s has no baked flesh softness for inertia. Press Run Bake first."), *GetNameSafe(Data));
		return false;
	}
	return true;
}

UTexture2D* USoftbodyBakeLibrary::GeneratePaintTexture(USoftbodyMeshData* Data, UTexture2D* SourceMask)
{
	if (SourceMask && Data)
	{
		// 전 슬롯 공통 마스크 — 슬롯별 UV0가 겹치는 다중 슬롯 메시는 Slots 버전을 쓸 것
		int32 NumSlots = 0;
		if (Data->SourceSkeletalMesh)
		{
			NumSlots = Data->SourceSkeletalMesh->GetMaterials().Num();
		}
		else if (Data->SourceMesh)
		{
			NumSlots = Data->SourceMesh->GetStaticMaterials().Num();
		}
		if (NumSlots > 1)
		{
			UE_LOG(LogSoftbodyBake, Warning,
				TEXT("Paint: one mask is shared by all %d material slots. If the slots' UV0 layouts overlap, use one mask per slot (Slot Masks, or GeneratePaintTextureSlots)."), NumSlots);
		}
	}
	// 단일 텍스처 = 전 슬롯에 같은 칠(고정 배치 — R Depth, B Volume을 페인트 층에 넣는다. Jiggle·Softness는 측정·조각 찾기가 읽는 칠이라 여기서는 안 듦)
	FSoftbodyPaintSlotMask FullMap;
	FullMap.Texture = SourceMask;
	return GeneratePaintInternal(Data,
		[SourceMask, &FullMap](int32) { return SourceMask ? &FullMap : nullptr; }, SourceMask != nullptr);
}

UTexture2D* USoftbodyBakeLibrary::GeneratePaintTextureSlots(USoftbodyMeshData* Data,
	const TMap<int32, FSoftbodyPaintSlotMask>& SlotMasks)
{
	return GeneratePaintInternal(Data,
		[&SlotMasks](int32 Slot) { return SlotMasks.Find(Slot); }, SlotMasks.Num() > 0);
}

USoftbodyMeshData* USoftbodyBakeLibrary::BakeStaticMeshMeasured(UStaticMesh* Mesh, const FString& OutputFolder,
	const TArray<FVector4>& RigidSpheres, float CellSizeFrac, float LayerMaxGap)
{
	return BakeStaticMeshMeasuredMasks(Mesh, OutputFolder, RigidSpheres, CellSizeFrac, LayerMaxGap, TMap<FName, FSoftbodyPaintSlotMask>(), 0.f);
}

TArray<FVector4> USoftbodyBakeLibrary::PreviewStaticCore(UStaticMesh* Mesh, bool bFloor, float CellSizeFrac, float Thickness)
{
	TArray<FVector4> Spheres;
	if (const FMeshDescription* MD = Mesh ? Mesh->GetMeshDescription(0) : nullptr)
	{
		SoftbodyBakeShared::CollectStaticCoreSpheres(*MD, bFloor, CellSizeFrac, Thickness, Spheres);
	}
	return Spheres;
}

USoftbodyMeshData* USoftbodyBakeLibrary::BakeStaticMeshMeasuredMasks(UStaticMesh* Mesh, const FString& OutputFolder,
	const TArray<FVector4>& RigidSpheres, float CellSizeFrac, float LayerMaxGap, const TMap<FName, FSoftbodyPaintSlotMask>& SlotMasks,
	float FleshReach, UStaticMesh* VolumeMesh, float RigidMinRadiusCells, float VolumePreservation, TConstArrayView<FName> PendingSoftbodySlots)
{
	FSoftbodyMeasureParams Params;
	Params.VolumeValue = FMath::Clamp(VolumePreservation, 0.f, 1.f);
	Params.CellSizeFrac = CellSizeFrac;
	Params.RigidSpheres = RigidSpheres;
	Params.RigidMinRadiusCells = FMath::Max(RigidMinRadiusCells, 0.f);
	Params.bBakeInertiaResponses = true;
	Params.LayerMaxGap = LayerMaxGap;
	Params.LayerSlots = SoftbodySlotNames(nullptr, Mesh, PendingSoftbodySlots);
	Params.VolumeMesh = VolumeMesh;
	if (const FMeshDescription* MD = Mesh && SlotMasks.Num() > 0 ? Mesh->GetMeshDescription(0) : nullptr)
	{
		TMap<FName, FName> Aliases;
		for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
		{
			Aliases.Add(Material.ImportedMaterialSlotName, Material.MaterialSlotName);
		}
		SampleMeasurePaint(*MD, SlotMasks, Aliases, Params);
	}
	// Flesh Reach 스프링은 값만으로 켜진다(ADR-0038 §3.3 — 고정 배치에서 "칠을 매핑했는가"는 숨은 스위치가 된다).
	Params.FleshReach = FMath::Max(FleshReach, 0.f);
	USoftbodyMeshData* Data = BakeStaticInternal(Mesh, OutputFolder, Params);
	if (Data)
	{
		Data->VolumePreservation = Params.VolumeValue;
		Data->MeasuredVolumePreservation = Params.VolumeValue;
		Data->HardMaskMeasuredHash = Params.HardMaskHash;
		Data->MeasuredPaintHash = SoftbodyBakeShared::MeasurePaintHashOf(Params);
	}
	return Data;
}

USoftbodyMeshData* USoftbodyBakeLibrary::BakeSkeletalMeshMeasured(USkeletalMesh* Mesh, const FString& OutputFolder,
	float CellSizeFrac, int32 MaxInertiaGroups, float RigidShrink, float LayerMaxGap)
{
	return BakeSkeletalMeshMeasuredOverrides(Mesh, OutputFolder, CellSizeFrac, MaxInertiaGroups, RigidShrink,
		TMap<FName, float>(), LayerMaxGap);
}

USoftbodyMeshData* USoftbodyBakeLibrary::BakeSkeletalMeshMeasuredOverrides(USkeletalMesh* Mesh, const FString& OutputFolder,
	float CellSizeFrac, int32 MaxInertiaGroups, float RigidShrink, const TMap<FName, float>& RigidShrinkPerBone,
	float LayerMaxGap, const TMap<FName, FSoftbodyPaintSlotMask>* SlotMasks, float FleshReach, UStaticMesh* VolumeMesh,
	const TSet<FName>* CoreSlots, float VolumePreservation, TConstArrayView<FName> PendingSoftbodySlots)
{
	if (!Mesh)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("No mesh given. Set Target Mesh and bake again."));
		return nullptr;
	}

	FSoftbodyMeasureParams Params;
	Params.CellSizeFrac = CellSizeFrac;
	Params.LayerMaxGap = LayerMaxGap;
	Params.LayerSlots = SoftbodySlotNames(Mesh, nullptr, PendingSoftbodySlots);
	Params.VolumeMesh = VolumeMesh;
	Params.VolumeValue = FMath::Clamp(VolumePreservation, 0.f, 1.f);

	// 살 속의 단단한 속 = 뼈대의 본 선(굵기는 살갗까지 거리 × RigidShrink). 없으면 살을 붙잡을 뼈가 없어 관성 무름을 만들지 않는다.
	// 속 메시(ADR-0028)가 있으면 굵기(살갗까지 거리)도 그 메시에서 잰다.
	// 살 슬롯(일지 239): 그 슬롯의 정점을 움직이는 본만 뼈다. 저작 에셋은 곧 소프트바디 머티리얼을 받을 슬롯까지 넘기고, 넘기지 않으면 지금
	// 소프트바디 머티리얼이 붙은 슬롯(없으면 메시 전체). 데이터에 남겨 접촉 지도 굽기가 같은 뼈대로 살 두께를 잰다.
	const TSet<FName> FleshSlots = CoreSlots ? *CoreSlots : SoftbodyBakeShared::CoreSlotNames(Mesh);
	const int32 NumLines = CollectSkeletonCoreSpheres(Mesh, Params.RigidSpheres, RigidShrink,
		RigidShrinkPerBone.Num() > 0 ? &RigidShrinkPerBone : nullptr, VolumeMesh ? VolumeMesh->GetMeshDescription(0) : nullptr, &FleshSlots);
	if (Params.RigidSpheres.Num() == 0)
	{
		UE_LOG(LogSoftbodyBake, Warning,
			TEXT("No rigid core (%d bone lines): none of the skin-weighted bones lie inside the mesh, so the bake continues without inertia (no jiggle). Check that the skeleton's bones sit inside the mesh."), NumLines);
	}
	else
	{
		// 메시당 살의 무름 3방향 하나만 만든다. 정점별 스킨 그룹 셋이 런타임에서
		// 어느 본의 상태를 사용할지 정하므로 본 수만큼 응답 텍스처를 복제하지 않는다.
		Params.bBakeInertiaResponses = true;
		// 손가락·팔뚝의 뼈는 격자 한 칸보다 가늘다. 그대로 두면 격자 점이 걸리는 곳과 안 걸리는 곳이 생겨 뼈가 토막 난다.
		Params.RigidMinRadiusCells = SoftbodyBakeShared::CoreMinRadiusCells;
	}
	if (const FMeshDescription* MD = SlotMasks && SlotMasks->Num() > 0 ? Mesh->GetMeshDescription(0) : nullptr)
	{
		TMap<FName, FName> Aliases;
		for (const FSkeletalMaterial& Material : Mesh->GetMaterials())
		{
			Aliases.Add(Material.ImportedMaterialSlotName, Material.MaterialSlotName);
		}
		SampleMeasurePaint(*MD, *SlotMasks, Aliases, Params);
	}
	// Flesh Reach 스프링은 값만으로 켜진다(ADR-0038 §3.3).
	Params.FleshReach = FMath::Max(FleshReach, 0.f);

	USoftbodyMeshData* Data = BakeSkeletalInternal(Mesh, OutputFolder, Params, MaxInertiaGroups,
		RigidShrinkPerBone.Num() > 0 ? &RigidShrinkPerBone : nullptr, &FleshSlots);
	if (Data)
	{
		Data->RigidShrinkPerBone = RigidShrinkPerBone;
		Data->RigidCoreSlots = FleshSlots.Array();
		Data->RigidCoreSlots.Sort(FNameLexicalLess());
		Data->RigidCoreShrink = RigidShrink;
		Data->VolumePreservation = Params.VolumeValue;
		Data->MeasuredVolumePreservation = Params.VolumeValue;
		Data->HardMaskMeasuredHash = Params.HardMaskHash;
		Data->MeasuredPaintHash = SoftbodyBakeShared::MeasurePaintHashOf(Params);
	}
	return Data;
}

int32 USoftbodyBakeLibrary::VerifyBakeIntegrity(USoftbodyMeshData* Data)
{
	if (!Data)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("VerifyIntegrity: no data asset"));
		return -1;
	}

	int32 Errors = 0;
	auto Fail = [&Errors, Data](const TCHAR* Message)
	{
		++Errors;
		UE_LOG(LogSoftbodyBake, Error, TEXT("VerifyIntegrity %s: %s. Press Run Bake."),
			*Data->GetName(), Message);
	};

	const bool bStatic = Data->SourceMesh != nullptr;
	const bool bSkeletal = Data->SourceSkeletalMesh != nullptr;
	if (bStatic == bSkeletal)
	{
		Fail(TEXT("needs exactly one source mesh (static or skeletal)"));
	}
	if (Data->TextureSize <= 0)
	{
		Fail(TEXT("TextureSize is 0 or less"));
	}
	if (Data->TexelUVChannel < 0)
	{
		Fail(TEXT("TexelUVChannel is negative"));
	}

	auto CheckTexture = [&Fail, Data](const UTexture2D* Texture,
		const TCHAR* Label, bool bRequired, int32 ExpectedHeight)
	{
		if (!Texture)
		{
			if (bRequired)
			{
				Fail(*FString::Printf(TEXT("%s is missing"), Label));
			}
			return;
		}
		if (Texture->Source.GetSizeX() != Data->TextureSize
			|| Texture->Source.GetSizeY() != ExpectedHeight)
		{
			Fail(*FString::Printf(TEXT("%s size %dx%d, expected %dx%d"),
				Label, Texture->Source.GetSizeX(), Texture->Source.GetSizeY(),
				Data->TextureSize, ExpectedHeight));
		}
	};

	const int32 SquareHeight = FMath::Max(Data->TextureSize, 0);
	CheckTexture(Data->AttributeTexDebug, TEXT("AttributeTexDebug"), false, SquareHeight);
	CheckTexture(Data->AttributeTex2, TEXT("AttributeTex2"), false, SquareHeight);
	CheckTexture(Data->PaintTex, TEXT("PaintTex"), false, SquareHeight);
	if (Data->InertiaResponseTex || Data->NumInertiaResponses > 0)
	{
		CheckTexture(Data->InertiaResponseTex, TEXT("InertiaResponseTex"), true,
			Data->TextureSize * Data->GetInertiaResponseRows());
	}

	const int32 NumVertices = Data->VertexPositions.Num();
	if (NumVertices <= 0)
	{
		Fail(TEXT("no baked CPU vertices"));
	}
	auto CheckVertexArray = [&Fail, NumVertices](int32 Num, const TCHAR* Label)
	{
		if (Num != NumVertices)
		{
			Fail(*FString::Printf(TEXT("%s count %d, expected %d"),
				Label, Num, NumVertices));
		}
	};
	CheckVertexArray(Data->VertexAreas.Num(), TEXT("VertexAreas"));
	if (Data->BakeDiagnosticsVersion >= 1)
	{
		CheckTexture(Data->AttributeTexDebug, TEXT("AttributeTexDebug"), true,
			SquareHeight);
		CheckVertexArray(Data->VertexEmbeddingStatus.Num(),
			TEXT("VertexEmbeddingStatus"));
		CheckVertexArray(Data->VertexEmbeddingMinWeight.Num(),
			TEXT("VertexEmbeddingMinWeight"));
		CheckVertexArray(Data->VertexCoreConnection.Num(),
			TEXT("VertexCoreConnection"));
		CheckVertexArray(Data->VertexInertiaResponseRms.Num(),
			TEXT("VertexInertiaResponseRms"));
	}

	int32 InvalidPositions = 0;
	int32 InvalidAreas = 0;
	int32 InvalidDiagnostics = 0;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		const FVector3f Position = Data->VertexPositions[Vertex];
		const bool bPositionFinite = FMath::IsFinite(Position.X)
			&& FMath::IsFinite(Position.Y) && FMath::IsFinite(Position.Z);
		const bool bAreaValid = Data->VertexAreas.IsValidIndex(Vertex)
			&& FMath::IsFinite(Data->VertexAreas[Vertex])
			&& Data->VertexAreas[Vertex] >= 0.f;
		InvalidPositions += bPositionFinite ? 0 : 1;
		InvalidAreas += bAreaValid ? 0 : 1;
		if (Data->BakeDiagnosticsVersion >= 1)
		{
			const bool bStatusValid =
				Data->VertexEmbeddingStatus.IsValidIndex(Vertex)
				&& Data->VertexEmbeddingStatus[Vertex] >= 1
				&& Data->VertexEmbeddingStatus[Vertex] <= 3;
			const bool bMinWeightValid =
				Data->VertexEmbeddingMinWeight.IsValidIndex(Vertex)
				&& FMath::IsFinite(
					Data->VertexEmbeddingMinWeight[Vertex]);
			const bool bCoreValid =
				Data->VertexCoreConnection.IsValidIndex(Vertex)
				&& Data->VertexCoreConnection[Vertex] <= 2;
			const bool bRmsValid =
				Data->VertexInertiaResponseRms.IsValidIndex(Vertex)
				&& FMath::IsFinite(
					Data->VertexInertiaResponseRms[Vertex])
				&& Data->VertexInertiaResponseRms[Vertex] >= 0.f;
			InvalidDiagnostics +=
				!(bStatusValid && bMinWeightValid && bCoreValid && bRmsValid);
		}
	}
	if (InvalidPositions > 0)
	{
		Fail(*FString::Printf(TEXT("%d invalid CPU vertex positions"),
			InvalidPositions));
	}
	if (InvalidAreas > 0)
	{
		Fail(*FString::Printf(TEXT("%d invalid CPU vertex areas"),
			InvalidAreas));
	}
	if (InvalidDiagnostics > 0)
	{
		Fail(*FString::Printf(TEXT("%d invalid CPU bake diagnostic values"),
			InvalidDiagnostics));
	}

	int32 InvalidUVs = 0;
	if (bStatic && Data->SourceMesh->GetRenderData()
		&& Data->SourceMesh->GetRenderData()->LODResources.Num() > 0)
	{
		const FStaticMeshLODResources& LOD =
			Data->SourceMesh->GetRenderData()->LODResources[0];
		const int32 NumUVChannels =
			LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
		if (Data->TexelUVChannel >= NumUVChannels)
		{
			Fail(*FString::Printf(TEXT("static render data has no UV channel %d (has %d)"),
				Data->TexelUVChannel, NumUVChannels));
		}
		else
		{
			const int32 NumRenderVertices =
				LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
			for (int32 Vertex = 0; Vertex < NumRenderVertices; ++Vertex)
			{
				const FVector2f UV =
					LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(
						Vertex, Data->TexelUVChannel);
				InvalidUVs += !FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y)
					|| UV.X < 0.f || UV.X > 1.f || UV.Y < 0.f || UV.Y > 1.f;
			}
		}
	}
	else if (bSkeletal)
	{
		FSkeletalMeshRenderData* RenderData =
			Data->SourceSkeletalMesh->GetResourceForRendering();
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			Fail(TEXT("no skeletal render data"));
		}
		else
		{
			const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
			const int32 NumUVChannels =
				LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
			if (Data->TexelUVChannel >= NumUVChannels)
			{
				Fail(*FString::Printf(TEXT("skeletal render data has no UV channel %d (has %d)"),
					Data->TexelUVChannel, NumUVChannels));
			}
			else
			{
				const int32 NumRenderVertices =
					LOD.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
				for (int32 Vertex = 0; Vertex < NumRenderVertices; ++Vertex)
				{
					const FVector2f UV =
						LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(
							Vertex, Data->TexelUVChannel);
					InvalidUVs += !FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y)
						|| UV.X < 0.f || UV.X > 1.f || UV.Y < 0.f || UV.Y > 1.f;
				}
			}
		}
	}
	else if (bStatic)
	{
		Fail(TEXT("no static render data"));
	}
	if (InvalidUVs > 0)
	{
		Fail(*FString::Printf(TEXT("%d texel UVs are outside [0,1] or invalid"), InvalidUVs));
	}

	UE_LOG(LogSoftbodyBake, Display,
		TEXT("VerifyIntegrity %s: %s (vertices=%d, texture=%dx%d)"),
		*Data->GetName(), Errors == 0 ? TEXT("passed") : TEXT("failed"),
		NumVertices, Data->TextureSize, Data->TextureSize);
	return Errors;
}

int32 USoftbodyBakeLibrary::VerifySkeletalBake(USkeletalMesh* Mesh, USoftbodyMeshData* Data)
{
	if (!Mesh || !Data || !Data->AttributeTex2 || Data->TextureSize <= 0)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("VerifySkeletal: missing input (needs the mesh, the baked data and its inertia group texture). Press Run Bake."));
		return -1;
	}

	TArray64<uint8> Raw;
	const int32 TexSize = Data->TextureSize;
	if (!Data->AttributeTex2->Source.GetMipData(Raw, 0) || Raw.Num() < (int64)TexSize * TexSize * (int64)sizeof(FFloat16Color))
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("VerifySkeletal: could not read the texture source"));
		return -1;
	}
	const FFloat16Color* Texels = (const FFloat16Color*)Raw.GetData();

	FSkeletalMeshRenderData* RD = Mesh->GetResourceForRendering();
	if (!RD || RD->LODRenderData.Num() == 0)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("VerifySkeletal: no render data"));
		return -1;
	}
	const FSkeletalMeshLODRenderData& LOD = RD->LODRenderData[0];
	if (Data->TexelUVChannel >= (int32)LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords())
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("VerifySkeletal: render data has no UV channel %d"), Data->TexelUVChannel);
		return -1;
	}

	// 렌더 정점마다: 텍셀 UV가 베이크 정점 텍셀을 가리키고, 그 텍셀의 관성 그룹 셋과 몫이 유효해야 한다(몫은 큰 순서).
	int32 BadTexel = 0;
	int32 BadGroups = 0;
	const int32 NumBakeVertices = Data->VertexPositions.Num();
	const int32 NumGroups = Data->InertiaGroupBones.Num();
	const int32 NumRenderVerts = LOD.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	for (int32 v = 0; v < NumRenderVerts; ++v)
	{
		const FVector2f UV = LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(v, Data->TexelUVChannel);
		const int32 TX = FMath::Clamp((int32)(UV.X * TexSize), 0, TexSize - 1);
		const int32 TY = FMath::Clamp((int32)(UV.Y * TexSize), 0, TexSize - 1);
		const int32 Texel = TY * TexSize + TX;
		if (Texel >= NumBakeVertices)
		{
			++BadTexel;
			continue;
		}
		const FLinearColor Stored = Texels[Texel].GetFloats();
		const SoftbodyBakeShared::FVertexGroups Groups = SoftbodyBakeShared::FVertexGroups::Decode(Stored);
		bool bValid = FMath::IsFinite(Stored.A) && FMath::IsFinite(Stored.G) && Stored.A <= 1.f + 1e-3f && Stored.G >= -1e-3f
			&& Groups.Weight[0] >= Groups.Weight[1] - 2e-3f && Groups.Weight[1] >= Groups.Weight[2] - 2e-3f;
		for (const int32 Group : Groups.Group)
		{
			bValid = bValid && Group >= 0 && Group < NumGroups;
		}
		BadGroups += bValid ? 0 : 1;
	}

	UE_LOG(LogSoftbodyBake, Display,
		TEXT("VerifySkeletal: %d render vertices - texels outside the baked vertices %d, invalid inertia groups (top three and weights) %d"),
		NumRenderVerts, BadTexel, BadGroups);
	return BadTexel + BadGroups;
}

UStaticMesh* USoftbodyBakeLibrary::CreateHollowSphereTestMesh(const FString& AssetPath,
	float OuterRadius, float InnerRadius, int32 Resolution)
{
	FSphereGenerator OuterGen;
	OuterGen.Radius = OuterRadius;
	OuterGen.NumPhi = Resolution;
	OuterGen.NumTheta = Resolution;
	OuterGen.Generate();
	FDynamicMesh3 Combined(&OuterGen);

	if (InnerRadius > 0.f)
	{
		FSphereGenerator InnerGen;
		InnerGen.Radius = InnerRadius;
		InnerGen.NumPhi = 32;
		InnerGen.NumTheta = 32;
		InnerGen.Generate();
		FDynamicMesh3 Inner(&InnerGen);
		Inner.ReverseOrientation(true); // 노멀이 안쪽 → winding number가 내부를 공동(0)으로 판정

		FDynamicMeshEditor Editor(&Combined);
		FMeshIndexMappings Mappings;
		Editor.AppendMesh(&Inner, Mappings);
	}

	return SaveDynamicMeshAsStaticMesh(AssetPath, Combined);
}

UStaticMesh* USoftbodyBakeLibrary::CreateEllipsoidTestMesh(const FString& AssetPath,
	FVector Radii, int32 Resolution)
{
	Resolution = FMath::Max(Resolution, 8);
	Radii.X = FMath::Max(FMath::Abs(Radii.X), 1.f);
	Radii.Y = FMath::Max(FMath::Abs(Radii.Y), 1.f);
	Radii.Z = FMath::Max(FMath::Abs(Radii.Z), 1.f);

	FSphereGenerator Generator;
	Generator.Radius = 1.f;
	Generator.NumPhi = Resolution;
	Generator.NumTheta = Resolution;
	Generator.Generate();
	FDynamicMesh3 Mesh(&Generator);
	MeshTransforms::Scale(
		Mesh, FVector3d(Radii), FVector3d::Zero(), false);
	FMeshNormals::QuickComputeVertexNormals(Mesh);
	if (Mesh.HasAttributes())
	{
		FMeshNormals::InitializeOverlayToPerVertexNormals(
			Mesh.Attributes()->PrimaryNormals(), true);
	}
	return SaveDynamicMeshAsStaticMesh(AssetPath, Mesh);
}

UStaticMesh* USoftbodyBakeLibrary::CreateUniformSphereTestMesh(const FString& AssetPath, float Radius, int32 EdgeVertices)
{
	EdgeVertices = FMath::Clamp(EdgeVertices, 4, 256);
	FBoxSphereGenerator Generator;
	Generator.Box = FOrientedBox3d(FVector3d::Zero(), FVector3d::One());
	Generator.EdgeVertices = FIndex3i(EdgeVertices, EdgeVertices, EdgeVertices);
	Generator.Radius = FMath::Max(FMath::Abs(Radius), 1.f);
	Generator.Generate();
	FDynamicMesh3 Mesh(&Generator);
	FMeshNormals::QuickComputeVertexNormals(Mesh);
	if (Mesh.HasAttributes())
	{
		FMeshNormals::InitializeOverlayToPerVertexNormals(
			Mesh.Attributes()->PrimaryNormals(), true);
	}
	return SaveDynamicMeshAsStaticMesh(AssetPath, Mesh);
}

UStaticMesh* USoftbodyBakeLibrary::CreateDumbbellTestMesh(const FString& AssetPath,
	float HalfLength, float LobeRadius, float NeckRadius, int32 Resolution)
{
	Resolution = FMath::Max(Resolution, 12);
	HalfLength = FMath::Max(FMath::Abs(HalfLength), 10.f);
	LobeRadius = FMath::Max(FMath::Abs(LobeRadius), 2.f);
	NeckRadius = FMath::Clamp(FMath::Abs(NeckRadius), 1.f, LobeRadius);

	FGeneralizedCylinderGenerator Generator;
	Generator.CrossSection = FPolygon2d::MakeCircle(1.0, Resolution);
	Generator.InitialFrame = FFrame3d(
		FVector3d(-HalfLength, 0.0, 0.0),
		FVector3d::UnitX());
	Generator.bCapped = true;
	Generator.CapType = ECapType::FlatMidpointFan;

	const int32 NumLengthSamples = Resolution + 1;
	Generator.Path.Reserve(NumLengthSamples);
	Generator.PathScales.Reserve(NumLengthSamples);
	const float EndRadius = FMath::Max(0.15f * LobeRadius, 2.f);
	for (int32 Sample = 0; Sample < NumLengthSamples; ++Sample)
	{
		const float SignedT = -1.f + 2.f * (float)Sample / (float)(NumLengthSamples - 1);
		const float AbsT = FMath::Abs(SignedT);
		float Radius;
		if (AbsT <= 0.5f)
		{
			const float Alpha = FMath::SmoothStep(0.f, 1.f, AbsT * 2.f);
			Radius = FMath::Lerp(NeckRadius, LobeRadius, Alpha);
		}
		else
		{
			const float Alpha = FMath::SmoothStep(0.f, 1.f, (AbsT - 0.5f) * 2.f);
			Radius = FMath::Lerp(LobeRadius, EndRadius, Alpha);
		}
		Generator.Path.Add(FVector3d(SignedT * HalfLength, 0.0, 0.0));
		Generator.PathScales.Add(FVector2d(Radius, Radius));
	}

	Generator.Generate();
	FDynamicMesh3 Mesh(&Generator);
	return SaveDynamicMeshAsStaticMesh(AssetPath, Mesh);
}

int32 USoftbodyBakeLibrary::VerifyInertiaResponseBake(USoftbodyMeshData* Data)
{
	return VerifySoftbodyInertiaResponseBake(Data);
}

namespace
{
	/** 컴파일 동안 LogMaterial/LogShaderCompilers의 경고·오류 줄을 모은다(워커 스레드 로그 포함). */
	class FSoftbodyCompileLogCapture : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName MaterialCategory(TEXT("LogMaterial"));
			static const FName CompilerCategory(TEXT("LogShaderCompilers"));
			if ((Category == MaterialCategory || Category == CompilerCategory)
				&& Verbosity <= ELogVerbosity::Warning)
			{
				FScopeLock Lock(&Mutex);
				Lines.Add(FString::Printf(TEXT("[%s] %s"), *Category.ToString(), Text));
			}
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		TArray<FString> TakeLines()
		{
			FScopeLock Lock(&Mutex);
			return MoveTemp(Lines);
		}
	private:
		FCriticalSection Mutex;
		TArray<FString> Lines;
	};
}

TArray<FString> USoftbodyBakeLibrary::CompileMaterialShadersSync(UMaterialInterface* MaterialInterface)
{
	TArray<FString> Errors;
	UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
	if (!Material)
	{
		Errors.Add(TEXT("no base material"));
		return Errors;
	}
	ITargetPlatform* TargetPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
	if (!TargetPlatform)
	{
		Errors.Add(TEXT("no running target platform"));
		return Errors;
	}

	// 편집기의 렌더링용 셰이더 맵은 온디맨드(그리기 시점) 컴파일이라 헤드리스에서는 아무것도
	// 컴파일되지 않고, 함수 안쪽 Custom 노드 include(.ush) 변경은 DDC 키에도 반영되지 않는다
	// (실측: 미정의 식별자를 넣어도 OK). 쿡 경로(RecompileShaders Material과 같은
	// BeginCacheForCookedPlatformData)는 전체 셰이더 맵을 동기로 컴파일하므로 이것만 커널 HLSL을
	// 실제로 검증한다. StateId를 새로 만들어 DDC 미스를 강제하며 에셋은 저장하지 않는다.
	FSoftbodyCompileLogCapture Capture;
	GLog->AddOutputDevice(&Capture);
	const double StartTime = FPlatformTime::Seconds();
	Material->StateId = FGuid::NewGuid();
	Material->BeginCacheForCookedPlatformData(TargetPlatform);
	while (!Material->IsCachedCookedPlatformDataLoaded(TargetPlatform))
	{
		FPlatformProcess::Sleep(0.05f);
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->ProcessAsyncResults(false, false);
		}
	}
	Material->ClearCachedCookedPlatformData(TargetPlatform);
	GLog->Flush();
	GLog->RemoveOutputDevice(&Capture);
	const double Elapsed = FPlatformTime::Seconds() - StartTime;

	const TArray<FString> Captured = Capture.TakeLines();
	for (const FString& Line : Captured)
	{
		const bool bFailure = Line.Contains(TEXT("Failed to compile"))
			|| Line.Contains(TEXT("error"), ESearchCase::IgnoreCase);
		if (bFailure)
		{
			Errors.Add(Line);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("CompileMaterialShadersSync %s: cooked-platform compile %.1fs, %d compiler lines, %d errors"),
		*Material->GetPathName(), Elapsed, Captured.Num(), Errors.Num());
	return Errors;
}

namespace
{
	/** 대칭 3×3 행렬의 가장 큰 고유벡터 (거듭제곱 반복). */
	FVector3f LargestEigenvector(const FMatrix44f& C)
	{
		FVector3f V(1.f, 0.7f, 0.3f);
		for (int32 Iteration = 0; Iteration < 64; ++Iteration)
		{
			const FVector3f Next(
				C.M[0][0] * V.X + C.M[0][1] * V.Y + C.M[0][2] * V.Z,
				C.M[1][0] * V.X + C.M[1][1] * V.Y + C.M[1][2] * V.Z,
				C.M[2][0] * V.X + C.M[2][1] * V.Y + C.M[2][2] * V.Z);
			const float Length = Next.Size();
			if (Length < 1e-12f)
			{
				break;
			}
			V = Next / Length;
		}
		return V.GetSafeNormal();
	}

	float Percentile(TArray<float> Values, float Fraction)
	{
		Values.Sort();
		return Values[FMath::Clamp(FMath::RoundToInt(Fraction * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}

	FMatrix44f PointCovariance(const TArray<FVector3f>& Points, const FVector3f& Mean, const FVector3f* RemoveAxis)
	{
		FMatrix44f C(ForceInitToZero);
		for (const FVector3f& P : Points)
		{
			FVector3f D = P - Mean;
			if (RemoveAxis)
			{
				D -= *RemoveAxis * FVector3f::DotProduct(D, *RemoveAxis);
			}
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Column = 0; Column < 3; ++Column)
				{
					C.M[Row][Column] += D[Row] * D[Column];
				}
			}
		}
		return C;
	}
}

UPhysicsAsset* USoftbodyBakeLibrary::CreateBonePhysicsAsset(USkeletalMesh* Mesh, const FString& AssetPath, int32 MinVertices)
{
	if (!Mesh)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("CreateBonePhysicsAsset: no mesh given"));
		return nullptr;
	}
	// 본별 정점은 CPU 접근이 켜진 렌더 버퍼에서 읽는다. 꺼져 있으면 켜고 다시 빌드한 뒤 비동기 빌드를 기다린다.
	if (FSkeletalMeshLODInfo* LodInfo = Mesh->GetLODInfo(0); LodInfo && !LodInfo->bAllowCPUAccess)
	{
		LodInfo->bAllowCPUAccess = true;
		Mesh->PostEditChange();
	}
	USkinnedAsset* const Compiling[] = { Mesh };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0
		|| !RenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetAllowCPUAccess()
		|| !RenderData->LODRenderData[0].SkinWeightVertexBuffer.GetNeedsCPUAccess())
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("CreateBonePhysicsAsset: LOD0 render data cannot be read on the CPU. Turn on Allow CPU Access for LOD0 of the skeletal mesh and try again."));
		return nullptr;
	}

	// 정점마다 가장 큰 웨이트의 본 → 그 본 공간 위치로 모은다.
	const FSkeletalMeshLODRenderData& Lod = RenderData->LODRenderData[0];
	Mesh->CalculateInvRefMatrices();
	const TArray<FMatrix44f>& InvRef = Mesh->GetRefBasesInvMatrix();
	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	TArray<TArray<FVector3f>> BonePoints;
	BonePoints.SetNum(RefSkeleton.GetRawBoneNum());
	TArray<FSkinWeightInfo> Weights;
	Lod.SkinWeightVertexBuffer.GetSkinWeights(Weights);
	for (const FSkelMeshRenderSection& Section : Lod.RenderSections)
	{
		for (uint32 Local = 0; Local < Section.NumVertices; ++Local)
		{
			const int32 Vertex = Section.BaseVertexIndex + Local;
			const FSkinWeightInfo& Info = Weights[Vertex];
			int32 Best = 0;
			for (int32 Influence = 1; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
			{
				if (Info.InfluenceWeights[Influence] > Info.InfluenceWeights[Best])
				{
					Best = Influence;
				}
			}
			const int32 Bone = Section.BoneMap[Info.InfluenceBones[Best]];
			BonePoints[Bone].Add(InvRef[Bone].TransformPosition(Lod.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex)));
		}
	}

	const FString AssetName = FPackageName::GetShortName(AssetPath);
	UPackage* Pkg = CreatePackage(*AssetPath);
	UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	PhysicsAsset->PreviewSkeletalMesh = Mesh;
	const FPhysAssetCreateParams Params;

	// 본 공간에서 주축(PCA)을 잡는다. 부모가 있는 본(손가락 마디)은 주축을 따라 캡슐: 길이 = 투영 범위, 반지름 = 축까지
	// 거리의 80번째 백분위. 루트 본(손바닥처럼 넓적한 부위)은 주축 좌표계의 상자로 감싼다.
	for (int32 Bone = 0; Bone < BonePoints.Num(); ++Bone)
	{
		const TArray<FVector3f>& Points = BonePoints[Bone];
		if (Points.Num() < FMath::Max(MinVertices, 4))
		{
			continue;
		}
		FVector3f Mean = FVector3f::ZeroVector;
		for (const FVector3f& P : Points)
		{
			Mean += P;
		}
		Mean /= (float)Points.Num();
		const FVector3f Axis = LargestEigenvector(PointCovariance(Points, Mean, nullptr));
		const FName BoneName = RefSkeleton.GetBoneName(Bone);
		const int32 BodyIndex = FPhysicsAssetUtils::CreateNewBody(PhysicsAsset, BoneName, Params);
		FKAggregateGeom& Geom = PhysicsAsset->SkeletalBodySetups[BodyIndex]->AggGeom;
		Geom.EmptyElements();

		if (RefSkeleton.GetParentIndex(Bone) != INDEX_NONE)
		{
			float MinT = TNumericLimits<float>::Max();
			float MaxT = TNumericLimits<float>::Lowest();
			TArray<float> Radial;
			Radial.Reserve(Points.Num());
			for (const FVector3f& P : Points)
			{
				const float T = FVector3f::DotProduct(P - Mean, Axis);
				MinT = FMath::Min(MinT, T);
				MaxT = FMath::Max(MaxT, T);
				Radial.Add(((P - Mean) - Axis * T).Size());
			}
			const float Radius = Percentile(Radial, 0.8f);
			FKSphylElem Capsule;
			Capsule.Center = FVector(Mean + Axis * (0.5f * (MinT + MaxT)));
			Capsule.Rotation = FRotator(FQuat::FindBetweenNormals(FVector::UpVector, FVector(Axis)));
			Capsule.Radius = Radius;
			Capsule.Length = FMath::Max(0.f, (MaxT - MinT) - 2.f * Radius);
			Geom.SphylElems.Add(Capsule);
			UE_LOG(LogSoftbodyBake, Display, TEXT("  %s: capsule radius %.2f length %.2f (%d vertices)"),
				*BoneName.ToString(), Capsule.Radius, Capsule.Length, Points.Num());
		}
		else
		{
			// 두 번째 축: 주축 성분을 뺀 공분산에서 다시 가장 큰 방향.
			FVector3f Second = LargestEigenvector(PointCovariance(Points, Mean, &Axis));
			Second = (Second - Axis * FVector3f::DotProduct(Second, Axis)).GetSafeNormal();
			const FVector3f Third = FVector3f::CrossProduct(Axis, Second);
			FVector3f Lo(TNumericLimits<float>::Max());
			FVector3f Hi(TNumericLimits<float>::Lowest());
			for (const FVector3f& P : Points)
			{
				const FVector3f D = P - Mean;
				const FVector3f C(FVector3f::DotProduct(D, Axis), FVector3f::DotProduct(D, Second), FVector3f::DotProduct(D, Third));
				Lo = Lo.ComponentMin(C);
				Hi = Hi.ComponentMax(C);
			}
			const FVector3f MidLocal = 0.5f * (Lo + Hi);
			FKBoxElem Box;
			Box.Center = FVector(Mean + Axis * MidLocal.X + Second * MidLocal.Y + Third * MidLocal.Z);
			Box.Rotation = FRotator(FQuat(FMatrix(FVector(Axis), FVector(Second), FVector(Third), FVector::ZeroVector)));
			Box.X = Hi.X - Lo.X;
			Box.Y = Hi.Y - Lo.Y;
			Box.Z = Hi.Z - Lo.Z;
			Geom.BoxElems.Add(Box);
			UE_LOG(LogSoftbodyBake, Display, TEXT("  %s: box %.2f x %.2f x %.2f (%d vertices)"),
				*BoneName.ToString(), Box.X, Box.Y, Box.Z, Points.Num());
		}
	}
	if (PhysicsAsset->SkeletalBodySetups.Num() == 0)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("CreateBonePhysicsAsset %s: no bone has enough skinned vertices to get a body. Lower MinVertices."), *AssetPath);
		return nullptr;
	}
	PhysicsAsset->UpdateBodySetupIndexMap();
	PhysicsAsset->UpdateBoundsBodiesArray();
	{
		// 메시에 다른 피직스 에셋이 걸려 있었으면 알리고, 되돌릴 수 있게 한다.
		FScopedTransaction Transaction(NSLOCTEXT("SoftbodyBake", "AssignPhysicsAsset", "Softbody: Assign Physics Asset"));
		if (UPhysicsAsset* Previous = Mesh->GetPhysicsAsset(); Previous && Previous != PhysicsAsset)
		{
			UE_LOG(LogSoftbodyBake, Warning, TEXT("%s: physics asset %s replaced by %s (Edit > Undo restores it)."),
				*Mesh->GetName(), *Previous->GetPathName(), *PhysicsAsset->GetPathName());
		}
		Mesh->Modify();
		Mesh->SetPhysicsAsset(PhysicsAsset);
	}
	FAssetRegistryModule::AssetCreated(PhysicsAsset);
	Pkg->MarkPackageDirty();
	Mesh->MarkPackageDirty();
	UE_LOG(LogSoftbodyBake, Display, TEXT("Created physics asset: %s (%d bodies)"), *AssetPath, PhysicsAsset->SkeletalBodySetups.Num());
	return PhysicsAsset;
}

namespace
{
	bool ReadSourceLinear(UTexture2D* Tex, TArray<FLinearColor>& Out, int32& W, int32& H)
	{
		FImage Src;
		if (!Tex || !Tex->Source.GetMipImage(Src, 0))
		{
			return false;
		}
		FImage Linear;
		Src.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		W = Linear.SizeX;
		H = Linear.SizeY;
		Out.SetNumUninitialized(W * H);
		FMemory::Memcpy(Out.GetData(), Linear.RawData.GetData(), sizeof(FLinearColor) * W * H);
		return true;
	}

	/** 텍셀 중심 규약의 쌍선형 샘플(클램프). Kind가 있으면 표면 칸이 아닌 칸(고스트·빈 칸)에 걸린 가중치를 돌려준다. */
	FLinearColor SampleBilinear(const TArray<FLinearColor>& Img, int32 W, int32 H, const FVector2f& UV,
		const TArray<FLinearColor>* Kind, float& OutOffSurfaceWeight)
	{
		const float X = UV.X * W - 0.5f;
		const float Y = UV.Y * H - 0.5f;
		const int32 X0 = FMath::FloorToInt(X);
		const int32 Y0 = FMath::FloorToInt(Y);
		const float FX = X - X0;
		const float FY = Y - Y0;
		FLinearColor Acc(0.f, 0.f, 0.f, 0.f);
		OutOffSurfaceWeight = 0.f;
		for (int32 DY = 0; DY < 2; ++DY)
		{
			for (int32 DX = 0; DX < 2; ++DX)
			{
				const int32 PX = FMath::Clamp(X0 + DX, 0, W - 1);
				const int32 PY = FMath::Clamp(Y0 + DY, 0, H - 1);
				const float Wt = (DX ? FX : 1.f - FX) * (DY ? FY : 1.f - FY);
				Acc += Img[PY * W + PX] * Wt;
				if (Kind && FMath::RoundToInt((*Kind)[PY * W + PX].A) != 1)
				{
					OutOffSurfaceWeight += Wt;
				}
			}
		}
		return Acc;
	}

	float CompactC2(float NormalizedDistance)
	{
		const float Q = FMath::Clamp(NormalizedDistance, 0.f, 1.f);
		const float W = FMath::Square(FMath::Square(1.f - Q));
		return NormalizedDistance < 1.f ? W * (1.f + 4.f * Q) : 0.f;
	}

	float GapPercentile(TArray<float> Values, float Q)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		return Values[FMath::Clamp((int32)(Q * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}
}

bool USoftbodyBakeLibrary::DumpLayerGap(USoftbodyComponent* Softbody, FName InnerSlot, FName OuterSlot,
	FVector CenterWorld, float Radius, float MaxGap, const FString& CsvPath)
{
	USoftbodyMeshData* Data = Softbody ? Softbody->MeshData.Get() : nullptr;
	USkeletalMeshComponent* Body = Softbody ? Cast<USkeletalMeshComponent>(Softbody->GetTargetMeshComponent()) : nullptr;
	USoftbodySurfaceField* Field = Softbody ? Softbody->GetSurfaceField() : nullptr;
	UTextureRenderTarget2D* DispRT = Field ? Field->GetDisplacementTexture() : nullptr;
	USkeletalMesh* Mesh = Data ? Data->SourceSkeletalMesh.Get() : nullptr;
	if (!Data || !Body || !DispRT || !Mesh || !Data->CarrierUVTex || !Data->CarrierPositionTex.LoadSynchronous() || !Data->PaintTex)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("LayerGap: needs a skeletal Softbody component with contact running (one of these is missing: baked data, mesh, contact, contact map, paint)"));
		return false;
	}

	int32 TableW = 0, TableH = 0, PosW = 0, PosH = 0, PaintW = 0, PaintH = 0;
	TArray<FLinearColor> CarrierUV, CarrierPos, Paint, Disp;
	if (!ReadSourceLinear(Data->CarrierUVTex, CarrierUV, TableW, TableH)
		|| !ReadSourceLinear(Data->CarrierPositionTex.LoadSynchronous(), CarrierPos, PosW, PosH)
		|| !ReadSourceLinear(Data->PaintTex, Paint, PaintW, PaintH) || PaintW != TableW)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("LayerGap: could not read the source data of the baked textures"));
		return false;
	}
	FTextureRenderTargetResource* DispRes = DispRT->GameThread_GetRenderTargetResource();
	if (!DispRes || !DispRes->ReadLinearColorPixels(Disp, FReadSurfaceDataFlags(RCM_MinMax)) || Disp.Num() != PosW * PosH)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("LayerGap: could not read the displacement texture"));
		return false;
	}
	const int32 R = PosW;
	const float TexelSize = FMath::Max(Data->CarrierTexelSize, 1e-3f);

	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return false;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	const FStaticMeshVertexBuffer& VB = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	const FPositionVertexBuffer& PB = LOD.StaticVertexBuffers.PositionVertexBuffer;
	TArray<uint32> Indices;
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	auto SlotOf = [&Materials](int32 MaterialIndex)
	{
		return Materials.IsValidIndex(MaterialIndex) ? Materials[MaterialIndex].MaterialSlotName : NAME_None;
	};

	// 렌더 정점마다: 캐리어 UV·봉투 거리, 커널과 같은 식의 변위(메시 로컬 = 표면 장 프레임), 표면 밖 칸 가중치
	const int32 NumVerts = (int32)PB.GetNumVertices();
	TArray<FVector3f> VertDisp;
	TArray<FVector2f> VertCarrierUV;
	TArray<float> VertOffSurface;
	VertDisp.SetNumZeroed(NumVerts);
	VertCarrierUV.SetNumZeroed(NumVerts);
	VertOffSurface.SetNumZeroed(NumVerts);
	for (int32 V = 0; V < NumVerts; ++V)
	{
		const FVector2f T = VB.GetVertexUV(V, Data->TexelUVChannel);
		const int32 TX = FMath::Clamp((int32)(T.X * TableW), 0, TableW - 1);
		const int32 TY = FMath::Clamp((int32)(T.Y * TableH), 0, TableH - 1);
		const FLinearColor C = CarrierUV[TY * TableW + TX];
		if (C.A <= 0.5f)
		{
			continue;
		}
		VertCarrierUV[V] = FVector2f(C.R, C.G);
		const FLinearColor F = SampleBilinear(Disp, R, R, VertCarrierUV[V], &CarrierPos, VertOffSurface[V]);
		const float Attach = CompactC2(FMath::Abs(C.B) / FMath::Max(Softbody->SurfaceAttachRange, 1e-3f) * 2.f - 1.f);
		// 커널과 같다(SB_SurfaceFleshGate): 눌리는 정도 칠은 살 두께에 구워져 있고, 칠이 0인 정점만 표면 장을 따르지 않는다.
		const float Gain = Paint[TY * TableW + TX].G > 0.f ? Attach : 0.f;
		VertDisp[V] = FVector3f(F.R, F.G, F.B) * Gain;
	}

	const FVector3f Center = (FVector3f)Body->GetComponentTransform().InverseTransformPosition(CenterWorld);
	const float Reach = Radius + 2.f * MaxGap + 2.f;

	// 바깥 층 삼각형(범위 안) → 거리 질의용 메시
	FDynamicMesh3 Outer;
	TArray<int32> OuterToRender;
	TMap<int32, int32> RenderToOuter;
	auto OuterVertex = [&](int32 V)
	{
		if (const int32* Found = RenderToOuter.Find(V))
		{
			return *Found;
		}
		const int32 Id = Outer.AppendVertex(FVector3d(PB.VertexPosition(V)));
		RenderToOuter.Add(V, Id);
		OuterToRender.Add(V);
		return Id;
	};
	TArray<TPair<int32, int32>> InnerRanges;  // (첫 삼각형 인덱스 위치, 삼각형 수)
	for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
	{
		const FName Slot = SlotOf(Section.MaterialIndex);
		if (Slot == InnerSlot)
		{
			InnerRanges.Add({(int32)Section.BaseIndex, (int32)Section.NumTriangles});
		}
		if (Slot != OuterSlot)
		{
			continue;
		}
		for (uint32 Tri = 0; Tri < Section.NumTriangles; ++Tri)
		{
			const int32 I0 = (int32)Indices[Section.BaseIndex + Tri * 3 + 0];
			const int32 I1 = (int32)Indices[Section.BaseIndex + Tri * 3 + 1];
			const int32 I2 = (int32)Indices[Section.BaseIndex + Tri * 3 + 2];
			if (FVector3f::Dist(PB.VertexPosition(I0), Center) > Reach)
			{
				continue;
			}
			Outer.AppendTriangle(OuterVertex(I0), OuterVertex(I1), OuterVertex(I2));
		}
	}
	if (Outer.TriangleCount() == 0 || InnerRanges.Num() == 0)
	{
		UE_LOG(LogSoftbodyBake, Error, TEXT("LayerGap: no outer layer (%s) triangles within the radius, or no inner layer (%s) slot"),
			*OuterSlot.ToString(), *InnerSlot.ToString());
		return false;
	}
	FDynamicMeshAABBTree3 OuterTree(&Outer);

	// 두 정점이 같은 캐리어 차트를 읽는가: 캐리어 UV 거리(칸)가 실제 거리(칸)보다 훨씬 크면 차트 경계 양쪽이다
	auto CrossChart = [&](int32 A, int32 B)
	{
		const float UVTexels = FVector2f::Distance(VertCarrierUV[A], VertCarrierUV[B]) * R;
		const float PosTexels = FVector3f::Distance(PB.VertexPosition(A), PB.VertexPosition(B)) / TexelSize;
		return UVTexels > PosTexels * 1.5f + 2.f;
	};

	// 안쪽 정점 → 바깥 층 짝
	TSet<int32> InnerVerts;
	for (const TPair<int32, int32>& Range : InnerRanges)
	{
		for (int32 K = 0; K < Range.Value * 3; ++K)
		{
			const int32 V = (int32)Indices[Range.Key + K];
			if (FVector3f::Dist(PB.VertexPosition(V), Center) <= Radius)
			{
				InnerVerts.Add(V);
			}
		}
	}
	struct FGroup
	{
		int32 Pairs = 0;
		int32 Pokes = 0;
		TArray<float> AbsDelta;
	};
	FGroup Same, Cross;
	TArray<float> RestGaps, InnerDepths;
	float WorstGap = TNumericLimits<float>::Max();
	FString Csv = TEXT("x,y,z,rest_gap,delta,deformed_gap,cross_chart,inner_off_surface,outer_off_surface,inner_push,outer_push\n");
	for (const int32 V : InnerVerts)
	{
		const FVector3d P(PB.VertexPosition(V));
		double DistanceSq = 0.0;
		const int32 Tri = OuterTree.FindNearestTriangle(P, DistanceSq);
		if (Tri == IndexConstants::InvalidID || DistanceSq > (double)MaxGap * MaxGap)
		{
			continue;
		}
		const FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Outer, Tri, P);
		const FIndex3i TriV = Outer.GetTriangle(Tri);
		// 법선: 안쪽 정점의 바깥 방향과 같은 쪽으로 맞춘 바깥 삼각형 법선
		FVector3f N = (FVector3f)Outer.GetTriNormal(Tri);
		const FVector4f InnerTangentZ = VB.VertexTangentZ(V);
		const FVector3f InnerNormal(InnerTangentZ.X, InnerTangentZ.Y, InnerTangentZ.Z);
		if (FVector3f::DotProduct(N, InnerNormal) < 0.f)
		{
			N = -N;
		}
		FVector3f OuterDisp = FVector3f::ZeroVector;
		float OuterOff = 0.f;
		bool bCross = false;
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 C = OuterToRender[TriV[K]];
			OuterDisp += VertDisp[C] * (float)Query.TriangleBaryCoords[K];
			OuterOff = FMath::Max(OuterOff, VertOffSurface[C]);
			bCross |= CrossChart(V, C);
		}
		const float RestGap = (float)(Query.ClosestTrianglePoint - P).Dot((FVector3d)N);
		const float Delta = FVector3f::DotProduct(OuterDisp - VertDisp[V], N);
		const float Deformed = RestGap + Delta;
		FGroup& G = bCross ? Cross : Same;
		++G.Pairs;
		G.Pokes += Deformed < 0.f ? 1 : 0;
		G.AbsDelta.Add(FMath::Abs(Delta));
		RestGaps.Add(RestGap);
		InnerDepths.Add(-FVector3f::DotProduct(VertDisp[V], N));
		WorstGap = FMath::Min(WorstGap, Deformed);
		if (!CsvPath.IsEmpty())
		{
			const FVector3d W = Body->GetComponentTransform().TransformPosition(P);
			Csv += FString::Printf(TEXT("%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%d,%.3f,%.3f,%.4f,%.4f\n"), W.X, W.Y, W.Z,
				RestGap, Delta, Deformed, bCross ? 1 : 0, VertOffSurface[V], OuterOff,
				-FVector3f::DotProduct(VertDisp[V], N), -FVector3f::DotProduct(OuterDisp, N));
		}
	}

	// 안쪽 층 한 겹: 차트 경계를 건너는 변과 아닌 변의 변위 차(법선 성분)
	TArray<float> EdgeSame, EdgeCross;
	TSet<uint64> SeenEdges;
	for (const TPair<int32, int32>& Range : InnerRanges)
	{
		for (int32 Tri = 0; Tri < Range.Value; ++Tri)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A = (int32)Indices[Range.Key + Tri * 3 + K];
				const int32 B = (int32)Indices[Range.Key + Tri * 3 + (K + 1) % 3];
				if (!InnerVerts.Contains(A) || !InnerVerts.Contains(B))
				{
					continue;
				}
				const uint64 Key = ((uint64)FMath::Min(A, B) << 32) | (uint64)FMath::Max(A, B);
				if (SeenEdges.Contains(Key))
				{
					continue;
				}
				SeenEdges.Add(Key);
				const FVector4f TangentZ = VB.VertexTangentZ(A);
				const FVector3f N(TangentZ.X, TangentZ.Y, TangentZ.Z);
				const float Jump = FMath::Abs(FVector3f::DotProduct(VertDisp[A] - VertDisp[B], N));
				(CrossChart(A, B) ? EdgeCross : EdgeSame).Add(Jump);
			}
		}
	}

	UE_LOG(LogSoftbodyBake, Display,
		TEXT("LayerGap %s over %s (radius %.1f, cell %.2f cm): %d pairs, rest gap median %.3f / 5%% %.3f cm, inner dent max %.2f cm, deepest poke-through gap %.3f cm"),
		*OuterSlot.ToString(), *InnerSlot.ToString(), Radius, TexelSize, Same.Pairs + Cross.Pairs,
		GapPercentile(RestGaps, 0.5f), GapPercentile(RestGaps, 0.05f), GapPercentile(InnerDepths, 1.f), WorstGap);
	for (const TPair<const TCHAR*, FGroup*>& Entry : {TPair<const TCHAR*, FGroup*>(TEXT("same map piece"), &Same),
		TPair<const TCHAR*, FGroup*>(TEXT("across a map seam"), &Cross)})
	{
		const FGroup& G = *Entry.Value;
		UE_LOG(LogSoftbodyBake, Display,
			TEXT("LayerGap   %s: %d pairs, %d poke through (%.1f%%), displacement difference between layers |Δ| median %.3f / 95%% %.3f / max %.3f cm"),
			Entry.Key, G.Pairs, G.Pokes, G.Pairs > 0 ? 100.f * G.Pokes / G.Pairs : 0.f,
			GapPercentile(G.AbsDelta, 0.5f), GapPercentile(G.AbsDelta, 0.95f), GapPercentile(G.AbsDelta, 1.f));
	}
	UE_LOG(LogSoftbodyBake, Display,
		TEXT("LayerGap   %s edge displacement difference within the layer: %d same-piece edges median %.3f / 95%% %.3f / max %.3f, %d seam-crossing edges median %.3f / 95%% %.3f / max %.3f cm"),
		*InnerSlot.ToString(), EdgeSame.Num(), GapPercentile(EdgeSame, 0.5f), GapPercentile(EdgeSame, 0.95f), GapPercentile(EdgeSame, 1.f),
		EdgeCross.Num(), GapPercentile(EdgeCross, 0.5f), GapPercentile(EdgeCross, 0.95f), GapPercentile(EdgeCross, 1.f));
	if (!CsvPath.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Csv, *CsvPath);
	}
	return true;
}
