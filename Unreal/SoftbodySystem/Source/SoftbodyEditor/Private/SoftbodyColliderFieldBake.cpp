// Copyright (c) 2026 wjsekrud

// ADR-0004 모양 거리장 베이크: 누르는 쪽(또는 자기 접촉을 쓸 몸) 메시의 실제 모양을 부위별 3D 거리 격자로 굽는다.
//
// 1. 소스 메시의 바깥 봉투(winding 0.5 등위면, 캐리어와 같은 규칙)를 뽑는다. 봉투까지의 거리가 칸 값이고
//    봉투 안쪽이 음수다.
// 2. 스켈레탈은 칸마다 가장 가까운 소스 표면 점의 스킨 웨이트에서 가장 큰 본을 "주인"으로 정한다. 본마다 부위 하나이고,
//    부위의 모양 = 몸 ∩ 그 본이 주인인 영역. 영역 경계까지의 거리는 격자 거리 변환으로 구하고, 두 거리의 큰 쪽이
//    부위의 거리다(교집합 — 실제 거리보다 작거나 같아 광선이 부위를 건너뛰지 않는다). 누르는 본을 주면 그 본(과 자식 본)만
//    부위가 되고, 나머지 본의 영역은 누르지 않고 눌리기만 한다(자기 접촉에서 손만 몸을 누르게).
// 3. 부위마다 살 표면 바깥 MarginVoxels칸까지 감싸는 상자만 잘라 반정밀로 저장한다. 자동 칸이면 격자는 손가락이 담기게
//    촘촘하게 굽고, 부위마다 그 부위 두께에 맞춰 칸을 건너뛰어 저장한다(손가락은 촘촘하게, 몸통은 성기게).
#include "SoftbodyBakeLibrary.h"

#include "Async/ParallelFor.h"
#include "BoneWeights.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Math/Float16.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "MeshQueries.h"
#include "SkeletalMeshAttributes.h"
#include "SoftbodyBakeShared.h"
#include "SoftbodyColliderField.h"
#include "Spatial/FastWinding.h"
#include "StaticMeshAttributes.h"

using namespace UE::Geometry;

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyColliderField, Log, All);

namespace
{
	/** 부위 격자가 살 표면 바깥으로 두는 여유 (칸). 표면 장이 "콜라이더까지 남은 거리"를 읽는 띠다. */
	constexpr int32 MarginVoxels = 4;
	/** 자동 칸: 가장 성긴 부위 칸 = 바운드 최장변 / 이 값. 격자는 그 1/MaxPartStride로 굽는다. */
	constexpr double AutoVoxelsPerMaxDim = 96.0;
	/**
	 * 자동 칸에서 부위가 격자 칸을 건너뛰어 저장하는 최대 간격. 부위 칸은 그 부위 가장 깊은 살(두께의 절반)의 절반을
	 * 넘지 않는 가장 큰 배수다 — 손바닥·손가락은 격자 그대로, 몸통·허벅지는 이 배수.
	 */
	constexpr int32 MaxPartStride = 3;
	/**
	 * 전체 격자 칸 수 상한. 넘으면 칸을 키운다. 몸 전체를 격자로 굽기 때문에 손 칸을 0.23 cm로 주면 약 7천만 칸이 된다
	 * (굽는 동안 약 3 GB). 누르는 본 주변만 굽게 바꾸면 다시 줄인다(ADR-0006 §10).
	 */
	constexpr int64 MaxGlobalVoxels = 128ll * 1024 * 1024;
	/**
	 * 이보다 적은 칸의 주인인 본은 부위를 따로 두지 않고 가장 가까운 조상 부위에 합친다(웨이트가 한두 칸에만 잡힌 본).
	 * 손가락 끝마디처럼 작은 마디도 제 부위여야 손을 쥐었을 때 거리장이 곧게 뻗은 채 남지 않는다.
	 */
	constexpr int32 MinPartVoxels = 2;
	/** 거리 변환의 "씨앗 없음" 비용 (칸² 단위). 격자 거리² 최대보다 충분히 크고 double에서 정확하다. */
	constexpr double NoSeed = 1e12;

	/** Felzenszwalb 1D 제곱 거리 변환. F: 입력 비용, D: 결과, V·Z: 작업 버퍼(Z는 N + 1). */
	void DistanceTransform1D(const double* F, int32 N, double* D, int32* V, double* Z)
	{
		int32 K = 0;
		V[0] = 0;
		Z[0] = -TNumericLimits<double>::Max();
		Z[1] = TNumericLimits<double>::Max();
		for (int32 Q = 1; Q < N; ++Q)
		{
			double S = ((F[Q] + (double)Q * Q) - (F[V[K]] + (double)V[K] * V[K])) / (2.0 * (Q - V[K]));
			while (S <= Z[K])
			{
				--K;
				S = ((F[Q] + (double)Q * Q) - (F[V[K]] + (double)V[K] * V[K])) / (2.0 * (Q - V[K]));
			}
			++K;
			V[K] = Q;
			Z[K] = S;
			Z[K + 1] = TNumericLimits<double>::Max();
		}
		K = 0;
		for (int32 Q = 0; Q < N; ++Q)
		{
			while (Z[K + 1] < Q)
			{
				++K;
			}
			D[Q] = (double)(Q - V[K]) * (Q - V[K]) + F[V[K]];
		}
	}

	/** 3D 제곱 거리 변환 (칸 단위). Cost는 씨앗 0, 나머지 NoSeed로 채워 넘기고 결과로 덮인다. */
	void DistanceTransform3D(TArray<double>& Cost, const FIntVector& Dims)
	{
		auto Pass = [&Cost, &Dims](int32 Axis)
		{
			const int32 N = Dims[Axis];
			const int32 A = Axis == 0 ? 1 : 0;
			const int32 B = Axis == 2 ? 1 : 2;
			const int32 Lines = Dims[A] * Dims[B];
			const int32 Stride = Axis == 0 ? 1 : (Axis == 1 ? Dims.X : Dims.X * Dims.Y);
			ParallelFor(Lines, [&, N, A, B, Stride](int32 Line)
			{
				FIntVector Start(0, 0, 0);
				Start[A] = Line % Dims[A];
				Start[B] = Line / Dims[A];
				const int32 Base = Start.X + Dims.X * (Start.Y + Dims.Y * Start.Z);
				TArray<double, TInlineAllocator<512>> F;
				TArray<double, TInlineAllocator<512>> D;
				TArray<double, TInlineAllocator<513>> Z;
				TArray<int32, TInlineAllocator<512>> V;
				F.SetNumUninitialized(N);
				D.SetNumUninitialized(N);
				Z.SetNumUninitialized(N + 1);
				V.SetNumUninitialized(N);
				for (int32 i = 0; i < N; ++i)
				{
					F[i] = Cost[Base + i * Stride];
				}
				DistanceTransform1D(F.GetData(), N, D.GetData(), V.GetData(), Z.GetData());
				for (int32 i = 0; i < N; ++i)
				{
					Cost[Base + i * Stride] = D[i];
				}
			});
		};
		Pass(0);
		Pass(1);
		Pass(2);
	}

	/** 소스 정점(동적 메시 번호)마다의 본 웨이트. */
	using FVertexWeights = TArray<TPair<int32, float>, TInlineAllocator<8>>;

	/** 소스 표면의 점 가운데 P에서 가장 가까운 점의 가장 큰 웨이트 본. */
	int32 DominantBoneAt(const FDynamicMesh3& Source, const FDynamicMeshAABBTree3& SourceBVH,
		const TArray<FVertexWeights>& Weights, const FVector3d& P)
	{
		double DistanceSq = 0.0;
		const int32 Tri = SourceBVH.FindNearestTriangle(P, DistanceSq);
		if (Tri == IndexConstants::InvalidID)
		{
			return INDEX_NONE;
		}
		const FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Source, Tri, P);
		const FIndex3i T = Source.GetTriangle(Tri);
		TArray<TPair<int32, double>, TInlineAllocator<24>> Sum;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const double Bary = Query.TriangleBaryCoords[Corner];
			for (const TPair<int32, float>& Entry : Weights[T[Corner]])
			{
				TPair<int32, double>* Found = Sum.FindByPredicate([&Entry](const TPair<int32, double>& Item) { return Item.Key == Entry.Key; });
				if (Found)
				{
					Found->Value += Bary * Entry.Value;
				}
				else
				{
					Sum.Emplace(Entry.Key, Bary * Entry.Value);
				}
			}
		}
		int32 Best = INDEX_NONE;
		double BestWeight = -1.0;
		for (const TPair<int32, double>& Item : Sum)
		{
			if (Item.Value > BestWeight)
			{
				BestWeight = Item.Value;
				Best = Item.Key;
			}
		}
		return Best;
	}

	/** 격자(칸 중심 값)의 삼선형 보간. 격자 밖은 가장자리로 자른다. */
	double SampleGrid(const TArray<float>& Grid, const FIntVector& Dims, const FVector3d& Origin, double Voxel, const FVector3d& P)
	{
		const FVector3d G = (P - Origin) / Voxel;
		const FVector3d C(FMath::Clamp(G.X, 0.0, Dims.X - 1.0), FMath::Clamp(G.Y, 0.0, Dims.Y - 1.0), FMath::Clamp(G.Z, 0.0, Dims.Z - 1.0));
		const FIntVector I(FMath::Min((int32)C.X, Dims.X - 2), FMath::Min((int32)C.Y, Dims.Y - 2), FMath::Min((int32)C.Z, Dims.Z - 2));
		const FVector3d T = C - FVector3d(I);
		auto At = [&](int32 X, int32 Y, int32 Z) { return (double)Grid[X + Dims.X * (Y + Dims.Y * Z)]; };
		const double X00 = FMath::Lerp(At(I.X, I.Y, I.Z), At(I.X + 1, I.Y, I.Z), T.X);
		const double X10 = FMath::Lerp(At(I.X, I.Y + 1, I.Z), At(I.X + 1, I.Y + 1, I.Z), T.X);
		const double X01 = FMath::Lerp(At(I.X, I.Y, I.Z + 1), At(I.X + 1, I.Y, I.Z + 1), T.X);
		const double X11 = FMath::Lerp(At(I.X, I.Y + 1, I.Z + 1), At(I.X + 1, I.Y + 1, I.Z + 1), T.X);
		return FMath::Lerp(FMath::Lerp(X00, X10, T.Y), FMath::Lerp(X01, X11, T.Y), T.Z);
	}
}

USoftbodyColliderField* USoftbodyBakeLibrary::BakeColliderField(UObject* Mesh, float VoxelSize, const TArray<FName>& PresserBones,
	float PartOverlap, UStaticMesh* VolumeMesh)
{
	return BakeColliderField(Mesh, VoxelSize, PresserBones, PartOverlap, VolumeMesh, TArray<FName>());
}

USoftbodyColliderField* USoftbodyBakeLibrary::BakeColliderField(UObject* Mesh, float VoxelSize, const TArray<FName>& PresserBones,
	float PartOverlap, UStaticMesh* VolumeMesh, const TArray<FName>& MaterialSlots)
{
	UStaticMesh* Static = Cast<UStaticMesh>(Mesh);
	USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh);
	if (!Static && !Skeletal)
	{
		UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: needs a static or skeletal mesh (got %s)"), *GetNameSafe(Mesh));
		return nullptr;
	}
	FMeshDescription* MD = Static ? Static->GetMeshDescription(0) : Skeletal->GetMeshDescription(0);
	if (!MD || MD->Triangles().Num() == 0)
	{
		UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: %s has no LOD0 source geometry. Reimport the mesh with its source data."), *Mesh->GetName());
		return nullptr;
	}
	const double StartSeconds = FPlatformTime::Seconds();

	// --- 1. 소스 메시와 봉투 ---
	FDynamicMesh3 Source;
	FMeshDescriptionToDynamicMesh Converter;
	Converter.bCalculateMaps = true;
	Converter.bDisableAttributes = true;
	Converter.Convert(MD, Source);
	// 슬롯 고르기(저작 에셋 Shape Field Slots): 고른 슬롯의 삼각형만 남긴다 — 머리카락·장식이 누르지 않고, 머리는 두피로 누른다.
	// 슬롯 이름은 편집용 이름과 임포트 이름 둘 다 받는다(메시 설명의 폴리곤 그룹은 임포트 이름을 단다).
	TArray<FName> BakedSlots;
	if (MaterialSlots.Num() > 0)
	{
		TArray<TPair<FName, FName>> MeshSlots;   // (편집용 이름, 임포트 이름)
		if (Static)
		{
			for (const FStaticMaterial& M : Static->GetStaticMaterials())
			{
				MeshSlots.Emplace(M.MaterialSlotName, M.ImportedMaterialSlotName);
			}
		}
		else
		{
			for (const FSkeletalMaterial& M : Skeletal->GetMaterials())
			{
				MeshSlots.Emplace(M.MaterialSlotName, M.ImportedMaterialSlotName);
			}
		}
		TSet<FName> Keep;
		for (const FName Slot : MaterialSlots)
		{
			BakedSlots.AddUnique(Slot);
			const TPair<FName, FName>* Found = MeshSlots.FindByPredicate([Slot](const TPair<FName, FName>& S) { return S.Key == Slot || S.Value == Slot; });
			if (!Found)
			{
				UE_LOG(LogSoftbodyColliderField, Warning, TEXT("BakeColliderField: %s has no material slot named %s; check Shape Field Slots"), *Mesh->GetName(), *Slot.ToString());
				continue;
			}
			for (const FName Name : {Found->Key, Found->Value})
			{
				if (!Name.IsNone())
				{
					Keep.Add(Name);
				}
			}
		}
		const FMeshDescription& ConstMD = *MD;
		TPolygonGroupAttributesConstRef<FName> SlotNames =
			ConstMD.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		if (!SlotNames.IsValid())
		{
			UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: %s has no material slot names in its source geometry, so Shape Field Slots cannot be used. Reimport the mesh, or clear Shape Field Slots."), *Mesh->GetName());
			return nullptr;
		}
		TArray<int32> Drop;
		for (const int32 Tri : Source.TriangleIndicesItr())
		{
			const FTriangleID SourceTri = Converter.TriIDMap.IsValidIndex(Tri) ? Converter.TriIDMap[Tri] : FTriangleID(Tri);
			if (!Keep.Contains(SlotNames[MD->GetTrianglePolygonGroup(SourceTri)]))
			{
				Drop.Add(Tri);
			}
		}
		for (const int32 Tri : Drop)
		{
			Source.RemoveTriangle(Tri, /*bRemoveIsolatedVertices=*/true, /*bPreserveManifold=*/false);
		}
		if (Source.TriangleCount() == 0)
		{
			UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: %s - none of the Shape Field Slots has any triangles. Pick the slots of the body, or clear Shape Field Slots."), *Mesh->GetName());
			return nullptr;
		}
	}
	// 속 메시(ADR-0028): 봉투(안팎·거리)는 이 메시에서 뽑는다. 부위의 주인 본(스킨 웨이트)과 정점 검사는 대상 메시(Source) 그대로다.
	FDynamicMesh3 VolumeSource;
	if (VolumeMesh)
	{
		const FMeshDescription* VolumeMD = VolumeMesh->GetMeshDescription(0);
		if (!VolumeMD || VolumeMD->Triangles().Num() == 0)
		{
			UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: volume mesh %s has no LOD0 source geometry. Reimport it with its source data, or clear Volume Mesh."), *VolumeMesh->GetName());
			return nullptr;
		}
		FMeshDescriptionToDynamicMesh VolumeConverter;
		VolumeConverter.bDisableAttributes = true;
		VolumeConverter.Convert(VolumeMD, VolumeSource);
		UE_LOG(LogSoftbodyColliderField, Display, TEXT("BakeColliderField: taking the outer surface from volume mesh %s (%d triangles)"),
			*VolumeMesh->GetName(), VolumeSource.TriangleCount());
	}
	const FDynamicMesh3& Solid = VolumeMesh ? VolumeSource : Source;
	const FAxisAlignedBox3d SourceBounds = Solid.GetBounds();
	// 자동 칸: 격자는 가장 성긴 부위 칸의 1/MaxPartStride로 촘촘하게 굽고(손가락이 담기게), 부위마다 두께에 맞춰 건너뛴다.
	// 칸을 주면 모든 부위가 그 칸이다.
	const int32 MaxStride = VoxelSize > 0.f ? 1 : MaxPartStride;
	double Voxel = VoxelSize > 0.f ? (double)VoxelSize : SourceBounds.MaxDim() / (AutoVoxelsPerMaxDim * MaxStride);
	{
		const FVector3d Extent = SourceBounds.Diagonal() + FVector3d(2.0 * (MarginVoxels + 1) * MaxStride * Voxel);
		const double Count = (Extent.X / Voxel + 1.0) * (Extent.Y / Voxel + 1.0) * (Extent.Z / Voxel + 1.0);
		if (Count > (double)MaxGlobalVoxels)
		{
			Voxel *= FMath::Pow(Count / (double)MaxGlobalVoxels, 1.0 / 3.0) * 1.01;
			UE_LOG(LogSoftbodyColliderField, Warning, TEXT("BakeColliderField: the grid was too large, so the cell size was raised to %.3f cm"), Voxel);
		}
	}
	// 봉투는 칸 크기로 뽑고 평활하지 않는다(얇은 손가락이 줄어들지 않게 — 등위면 근을 이분법으로 찾아 계단이 작다).
	FDynamicMesh3 Envelope;
	if (!SoftbodyBakeShared::ExtractSolidEnvelope(Solid, Voxel, 0, 0, Envelope))
	{
		return nullptr;
	}
	FDynamicMeshAABBTree3 EnvelopeBVH(&Envelope, true);
	TFastWindingTree<FDynamicMesh3> EnvelopeWinding(&EnvelopeBVH, true);

	// --- 2. 전체 격자: 봉투까지 부호 거리 (+ 스켈레탈은 칸 주인 본) ---
	FAxisAlignedBox3d GridBounds = Envelope.GetBounds();
	GridBounds.Expand(Voxel * MaxStride * (MarginVoxels + 1));
	const FVector3d GridOrigin = GridBounds.Min;
	const FIntVector Dims(
		FMath::Max(2, (int32)FMath::CeilToInt(GridBounds.Width() / Voxel) + 1),
		FMath::Max(2, (int32)FMath::CeilToInt(GridBounds.Height() / Voxel) + 1),
		FMath::Max(2, (int32)FMath::CeilToInt(GridBounds.Depth() / Voxel) + 1));
	const int32 NumVoxels = Dims.X * Dims.Y * Dims.Z;
	auto VoxelCenter = [&](int32 Index)
	{
		const int32 X = Index % Dims.X;
		const int32 Y = (Index / Dims.X) % Dims.Y;
		const int32 Z = Index / (Dims.X * Dims.Y);
		return GridOrigin + FVector3d(X, Y, Z) * Voxel;
	};
	// 가장 성긴 부위의 여유 띠까지 주인을 정한다(부위 상자는 자기 칸 크기의 MarginVoxels칸까지만 쓴다).
	const double BandDistance = Voxel * MaxStride * MarginVoxels;

	TArray<float> Distance;
	Distance.SetNumUninitialized(NumVoxels);
	ParallelFor(NumVoxels, [&](int32 Index)
	{
		const FVector3d P = VoxelCenter(Index);
		double DistanceSq = 0.0;
		const int32 Tri = EnvelopeBVH.FindNearestTriangle(P, DistanceSq);
		const double Unsigned = Tri != IndexConstants::InvalidID ? FMath::Sqrt(DistanceSq) : BandDistance * 4.0;
		Distance[Index] = (float)(EnvelopeWinding.FastWindingNumber(P) > 0.5 ? -Unsigned : Unsigned);
	});

	TArray<int32> Owner;
	TArray<int32> OwnerBone;   // 칸의 주인 본(부위로 바꾸기 전) — 부위 겹침을 본 사슬 방향으로만 주는 데 쓴다(일지 73)
	TArray<int32> BoneToPart;
	TArray<int32> PartBones;
	TArray<bool> PartSelf;     // 부위가 제 몸도 누르는가(자기 접촉)
	if (Skeletal)
	{
		FSkeletalMeshConstAttributes SkinAttributes(*MD);
		FSkinWeightsVertexAttributesConstRef SkinWeights = SkinAttributes.GetVertexSkinWeights();
		if (!SkinWeights.IsValid())
		{
			UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: %s has no skin weights"), *Mesh->GetName());
			return nullptr;
		}
		TArray<FVertexWeights> Weights;
		Weights.SetNum(Source.MaxVertexID());
		for (const int32 Vertex : Source.VertexIndicesItr())
		{
			const FVertexID SourceVertex = Converter.VertIDMap.IsValidIndex(Vertex) ? Converter.VertIDMap[Vertex] : FVertexID(Vertex);
			for (UE::AnimationCore::FBoneWeight BoneWeight : SkinWeights.Get(SourceVertex))
			{
				Weights[Vertex].Emplace((int32)BoneWeight.GetBoneIndex(), BoneWeight.GetWeight());
			}
		}
		FDynamicMeshAABBTree3 SourceBVH(&Source, true);

		// 살 안쪽과 바깥 여유 띠의 칸만 주인을 정한다(먼 칸은 어느 부위 상자에도 들어가지 않는다).
		Owner.Init(INDEX_NONE, NumVoxels);
		ParallelFor(NumVoxels, [&](int32 Index)
		{
			if (Distance[Index] < BandDistance)
			{
				Owner[Index] = DominantBoneAt(Source, SourceBVH, Weights, VoxelCenter(Index));
			}
		});

		// 제 몸도 누르는 본 = 목록의 본과 그 자식 본(목록이 비면 모든 본). 부위는 모든 본에서 만든다: 다른 소프트바디는 몸 전체로
		// 눌리고(소파에 앉은 엉덩이), 자기 접촉만 이 본들의 부위를 싣는다(ADR-0004 §11).
		const FReferenceSkeleton& RefSkeleton = Skeletal->GetRefSkeleton();
		const int32 NumBones = RefSkeleton.GetNum();
		TArray<bool> Presses;
		Presses.Init(PresserBones.Num() == 0, NumBones);
		for (const FName Name : PresserBones)
		{
			const int32 Bone = RefSkeleton.FindBoneIndex(Name);
			if (Bone == INDEX_NONE)
			{
				UE_LOG(LogSoftbodyColliderField, Warning, TEXT("BakeColliderField: %s has no bone named %s; check Self Contact Bones"), *Mesh->GetName(), *Name.ToString());
				continue;
			}
			Presses[Bone] = true;
		}
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			// 부모 본은 늘 자식보다 앞 번호다
			const int32 Parent = RefSkeleton.GetParentIndex(Bone);
			Presses[Bone] = Presses[Bone] || (Parent != INDEX_NONE && Presses[Parent]);
		}

		// 부위 = 살 안쪽 칸을 MinPartVoxels개 이상 가진 본. 부위가 못 된 본의 칸은 뼈대를 따라 가장 가까운 조상 부위로 합친다 — 제 몸을
		// 누르는지가 같은 조상을 먼저 찾아, 손 칸이 팔뚝 부위로 들어가 자기 접촉에서 빠지거나 몸통 칸이 손 부위로 들어가 제 몸을 누르지
		// 않게. 같은 쪽 조상 부위가 없으면 그 위의 조상 부위로(그 칸은 남만 누른다). 조상 부위가 하나도 없는 본의 칸은 어느 부위에도
		// 넣지 않고 경고한다: 뼈대에 없는 관계(크기로 고른 먼 부위)를 짐작해 만들지 않는다(ADR-0004 §11). 스킨 웨이트를 고칠 일이다.
		TArray<int32> InsideCount;
		InsideCount.Init(0, NumBones);
		for (int32 Index = 0; Index < NumVoxels; ++Index)
		{
			if (Distance[Index] <= 0.f && InsideCount.IsValidIndex(Owner[Index]))
			{
				++InsideCount[Owner[Index]];
			}
		}
		TArray<int32> OwnPart;
		OwnPart.Init(INDEX_NONE, NumBones);
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (InsideCount[Bone] >= MinPartVoxels)
			{
				OwnPart[Bone] = PartBones.Add(Bone);
			}
		}
		if (PartBones.Num() == 0)
		{
			UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: %s - no bone is the main skin weight inside the mesh. Check the skin weights."), *Mesh->GetName());
			return nullptr;
		}
		int32 NumSelfParts = 0;
		for (int32 Part = 0; Part < PartBones.Num(); ++Part)
		{
			PartSelf.Add(Presses[PartBones[Part]]);
			NumSelfParts += Presses[PartBones[Part]] ? 1 : 0;
		}
		if (PresserBones.Num() > 0 && NumSelfParts == 0)
		{
			UE_LOG(LogSoftbodyColliderField, Warning, TEXT("BakeColliderField: %s - none of the Self Contact Bones (or their child bones) is the main skin weight inside the mesh, so self contact is off. Add the bones that actually move the hands."), *Mesh->GetName());
		}
		BoneToPart.Init(INDEX_NONE, NumBones);
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			int32 Walk = Bone;
			while (Walk != INDEX_NONE && OwnPart[Walk] == INDEX_NONE && Presses[Walk] == Presses[Bone])
			{
				Walk = RefSkeleton.GetParentIndex(Walk);
			}
			// 같은 쪽 사슬이 끝났다(Self Contact Bones의 뿌리 위): 그 위의 조상 부위로
			while (Walk != INDEX_NONE && OwnPart[Walk] == INDEX_NONE)
			{
				Walk = RefSkeleton.GetParentIndex(Walk);
			}
			if (Walk != INDEX_NONE)
			{
				BoneToPart[Bone] = OwnPart[Walk];
			}
			else if (InsideCount[Bone] > 0)
			{
				UE_LOG(LogSoftbodyColliderField, Warning, TEXT("BakeColliderField: %s - bone %s owns %d cell(s) of flesh, too few to be its own part, and no parent bone is a part, so that flesh is left out of the shape field. Check the skin weights."),
					*Mesh->GetName(), *RefSkeleton.GetBoneName(Bone).ToString(), InsideCount[Bone]);
			}
		}
		OwnerBone = Owner;
		for (int32& Label : Owner)
		{
			Label = BoneToPart.IsValidIndex(Label) ? BoneToPart[Label] : INDEX_NONE;
		}
	}
	else
	{
		PartBones.Add(INDEX_NONE);
		PartSelf.Add(true);
		Owner.Init(0, NumVoxels);
	}

	// --- 3. 부위마다 상자를 잘라 교집합 거리를 굽는다 ---
	USoftbodyColliderField* Field = USoftbodyColliderField::FindOnAsset(Mesh);
	if (!Field)
	{
		IInterface_AssetUserData* Holder = Cast<IInterface_AssetUserData>(Mesh);
		Field = Holder ? Cast<USoftbodyColliderField>(Holder->GetAssetUserDataOfClass(USoftbodyColliderField::StaticClass())) : nullptr;
		if (!Field && Holder)
		{
			Field = NewObject<USoftbodyColliderField>(Mesh, NAME_None, RF_Public | RF_Transactional);
			Mesh->Modify();
			Holder->AddAssetUserData(Field);
		}
	}
	if (!Field)
	{
		UE_LOG(LogSoftbodyColliderField, Error, TEXT("BakeColliderField: could not attach the shape field to %s (the asset does not accept asset user data)"), *Mesh->GetName());
		return nullptr;
	}
	Field->Modify();
	Field->Parts.Reset();
	Field->Voxels.Reset();
	const FReferenceSkeleton* RefSkeleton = Skeletal ? &Skeletal->GetRefSkeleton() : nullptr;
	// 부위 칸 간격: 가장 깊은 살(표면에서 가장 먼 안쪽 칸 = 두께의 절반)의 절반을 넘지 않는 격자 칸 배수.
	TArray<float> PartDepth;
	PartDepth.Init(0.f, PartBones.Num());
	for (int32 Index = 0; Index < NumVoxels; ++Index)
	{
		if (PartDepth.IsValidIndex(Owner[Index]))
		{
			PartDepth[Owner[Index]] = FMath::Max(PartDepth[Owner[Index]], -Distance[Index]);
		}
	}
	// 부위 겹침(일지 71·73): 영역을 이 폭만큼 본 사슬 방향(부모·자식 본이 주인인 칸)으로만 키운다. 옆에 붙은 형제 본(이웃
	// 손가락)의 살은 끌어안지 않는다 — 모든 방향으로 키우면 기본 포즈에서 맞닿은 손가락 살이 서로의 부위에 들어가, 손가락을
	// 벌린 포즈에서 자국이 손가락보다 1 cm씩 굵어졌다. 부위가 둘 이상일 때만 뜻이 있다.
	const double Overlap = PartBones.Num() > 1 ? FMath::Max(0.0, (double)PartOverlap) : 0.0;
	const int32 NumSkeletonBones = RefSkeleton ? RefSkeleton->GetNum() : 0;
	auto IsAncestorOrSelf = [&](int32 Ancestor, int32 Bone)
	{
		for (int32 Walk = Bone; Walk != INDEX_NONE; Walk = RefSkeleton->GetParentIndex(Walk))
		{
			if (Walk == Ancestor)
			{
				return true;
			}
		}
		return false;
	};
	const int32 OverlapVoxels = (int32)FMath::CeilToDouble(Overlap / Voxel);
	// 부위마다 저장 칸 간격과 여유 띠, 그 띠 안 칸을 담는 상자. 격자를 한 번만 훑는다 — 부위마다 전 격자를 훑으면 본이 많은 캐릭터에서
	// 부위 수 × 격자 칸이 된다. 결과는 같다.
	const int32 NumParts = PartBones.Num();
	TArray<int32> PartStride;
	TArray<double> PartBandOf;
	TArray<FIntVector> PartLo;
	TArray<FIntVector> PartHi;
	PartStride.SetNumUninitialized(NumParts);
	PartBandOf.SetNumUninitialized(NumParts);
	PartLo.Init(FIntVector(TNumericLimits<int32>::Max()), NumParts);
	PartHi.Init(FIntVector(TNumericLimits<int32>::Min()), NumParts);
	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		PartStride[Part] = FMath::Clamp((int32)(PartDepth[Part] * 0.5 / Voxel), 1, MaxStride);
		PartBandOf[Part] = Voxel * PartStride[Part] * MarginVoxels;
	}
	for (int32 Index = 0; Index < NumVoxels; ++Index)
	{
		const int32 Part = Owner[Index];
		if (!PartBandOf.IsValidIndex(Part) || Distance[Index] >= PartBandOf[Part])
		{
			continue;
		}
		const FIntVector C(Index % Dims.X, (Index / Dims.X) % Dims.Y, Index / (Dims.X * Dims.Y));
		FIntVector& Lo = PartLo[Part];
		FIntVector& Hi = PartHi[Part];
		Lo = FIntVector(FMath::Min(Lo.X, C.X), FMath::Min(Lo.Y, C.Y), FMath::Min(Lo.Z, C.Z));
		Hi = FIntVector(FMath::Max(Hi.X, C.X), FMath::Max(Hi.Y, C.Y), FMath::Max(Hi.Z, C.Z));
	}
	int64 TotalBoxVoxels = 0;
	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		const int32 Stride = PartStride[Part];
		FIntVector Lo = PartLo[Part];
		FIntVector Hi = PartHi[Part];
		if (Lo.X > Hi.X)
		{
			continue;
		}
		// 저장 칸은 격자 Lo에서 Stride칸마다. 한 저장 칸 여유를 두고 격자 안에 들게 자른다. 겹침 폭만큼 이웃 영역의 칸도 담는다.
		FIntVector Cells;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Lo[Axis] = FMath::Max(0, Lo[Axis] - Stride - OverlapVoxels);
			const int32 Top = FMath::Min(Dims[Axis] - 1, Hi[Axis] + Stride + OverlapVoxels);
			Cells[Axis] = FMath::Max(2, FMath::DivideAndRoundUp(Top - Lo[Axis], Stride) + 1);
			// 격자 안에 드는 칸 수까지만: 부위가 격자 끝까지 닿으면(막대처럼 부위가 단면 전체를 덮고 겹침 폭이 여유 띠를 넘을 때) 올림한
			// 칸 수가 격자보다 길어 상자 끝이 격자 밖을 읽었다(일지 324 — 굽기가 멈춤). 못 담는 끝은 Stride칸 미만의 바깥 여유 띠다.
			Cells[Axis] = FMath::Min(Cells[Axis], (Dims[Axis] - 1) / Stride + 1);
			Lo[Axis] = FMath::Max(0, FMath::Min(Lo[Axis], Dims[Axis] - 1 - (Cells[Axis] - 1) * Stride));
			Hi[Axis] = Lo[Axis] + (Cells[Axis] - 1) * Stride;
		}
		const FIntVector BoxDims = Hi - Lo + FIntVector(1);
		const int32 BoxCount = BoxDims.X * BoxDims.Y * BoxDims.Z;
		auto GlobalIndex = [&](int32 BoxIndex)
		{
			const int32 X = Lo.X + BoxIndex % BoxDims.X;
			const int32 Y = Lo.Y + (BoxIndex / BoxDims.X) % BoxDims.Y;
			const int32 Z = Lo.Z + BoxIndex / (BoxDims.X * BoxDims.Y);
			return X + Dims.X * (Y + Dims.Y * Z);
		};

		// 영역(이 부위가 주인인 칸)의 부호 거리 (칸 경계는 중심 사이 반 칸). 겹침은 영역을 겹침 폭만큼 키우되 이 부위 본의
		// 조상·자손 본이 주인인 칸으로만 키운다(관절 너머 이웃 마디·손목 너머 팔뚝). 키운 영역의 부호 거리를 쓰므로 경계가
		// 이어지고, 겹침 0이면 원래 영역과 같다. 몸 표면까지 거리는 그대로라 늘어난 구간도 몸 모양을 따른다.
		TArray<float> Region;
		Region.Init(-TNumericLimits<float>::Max(), BoxCount);
		if (PartBones.Num() > 1)
		{
			TArray<bool> ChainBone;
			if (Overlap > 0.0 && NumSkeletonBones > 0 && PartBones[Part] != INDEX_NONE)
			{
				ChainBone.Init(false, NumSkeletonBones);
				for (int32 Bone = 0; Bone < NumSkeletonBones; ++Bone)
				{
					ChainBone[Bone] = IsAncestorOrSelf(PartBones[Part], Bone) || IsAncestorOrSelf(Bone, PartBones[Part]);
				}
			}
			TArray<double> ToMember;
			ToMember.SetNumUninitialized(BoxCount);
			for (int32 i = 0; i < BoxCount; ++i)
			{
				ToMember[i] = Owner[GlobalIndex(i)] == Part ? 0.0 : NoSeed;
			}
			DistanceTransform3D(ToMember, BoxDims);
			// 키운 영역 = 제 칸 + (사슬 본의 칸 중 제 칸에서 겹침 폭 안)
			TArray<bool> Extended;
			Extended.SetNumUninitialized(BoxCount);
			for (int32 i = 0; i < BoxCount; ++i)
			{
				const int32 Global = GlobalIndex(i);
				bool bIn = Owner[Global] == Part;
				if (!bIn && ChainBone.Num() > 0 && OwnerBone.IsValidIndex(Global) && ChainBone.IsValidIndex(OwnerBone[Global])
					&& ChainBone[OwnerBone[Global]])
				{
					bIn = FMath::Sqrt(FMath::Min(ToMember[i], NoSeed)) * Voxel <= Overlap;
				}
				Extended[i] = bIn;
			}
			TArray<double> ToInside;
			TArray<double> ToOutside;
			ToInside.SetNumUninitialized(BoxCount);
			ToOutside.SetNumUninitialized(BoxCount);
			for (int32 i = 0; i < BoxCount; ++i)
			{
				ToInside[i] = Extended[i] ? 0.0 : NoSeed;
				ToOutside[i] = Extended[i] ? NoSeed : 0.0;
			}
			DistanceTransform3D(ToInside, BoxDims);
			DistanceTransform3D(ToOutside, BoxDims);
			for (int32 i = 0; i < BoxCount; ++i)
			{
				Region[i] = (float)(Extended[i]
					? (ToOutside[i] >= NoSeed * 0.5 ? -TNumericLimits<float>::Max() : -(FMath::Sqrt(ToOutside[i]) - 0.5) * Voxel)
					: (FMath::Sqrt(FMath::Min(ToInside[i], NoSeed)) - 0.5) * Voxel);
			}
		}

		FSoftbodyColliderFieldPart& Out = Field->Parts.AddDefaulted_GetRef();
		Out.Bone = RefSkeleton && PartBones[Part] != INDEX_NONE ? RefSkeleton->GetBoneName(PartBones[Part]) : NAME_None;
		Out.bPressesSelf = PartSelf[Part];
		Out.Origin = FVector3f(GridOrigin + FVector3d(Lo) * Voxel);
		Out.VoxelSize = (float)(Voxel * Stride);
		Out.Dims = Cells;
		Out.FirstVoxel = Field->Voxels.Num();
		Field->Voxels.Reserve(Field->Voxels.Num() + Cells.X * Cells.Y * Cells.Z);
		for (int32 Z = 0; Z < Cells.Z; ++Z)
		{
			for (int32 Y = 0; Y < Cells.Y; ++Y)
			{
				for (int32 X = 0; X < Cells.X; ++X)
				{
					const int32 i = X * Stride + BoxDims.X * (Y * Stride + BoxDims.Y * Z * Stride);
					const float Value = FMath::Max(Distance[GlobalIndex(i)], Region[i]);
					Field->Voxels.Add(FFloat16(FMath::Clamp(Value, -60000.f, 60000.f)).Encoded);
				}
			}
		}
		TotalBoxVoxels += Cells.X * Cells.Y * Cells.Z;
	}
	Field->BakedVoxelSize = (float)Voxel;
	Field->MaterialSlots = BakedSlots;
	Field->Version = USoftbodyColliderField::CurrentVersion;
	Field->MarkPackageDirty();
	Mesh->MarkPackageDirty();

	// 검사: 소스 정점은 봉투 위에 있어야 한다(거리 ≈ 0). 봉투가 옷 층을 건너뛴 곳은 멀 수 있다.
	TArray<double> VertexDistance;
	for (const int32 Vertex : Source.VertexIndicesItr())
	{
		VertexDistance.Add(FMath::Abs(SampleGrid(Distance, Dims, GridOrigin, Voxel, Source.GetVertex(Vertex))));
	}
	VertexDistance.Sort();
	const auto Percentile = [&VertexDistance](double Q)
	{
		return VertexDistance.Num() > 0 ? VertexDistance[FMath::Clamp((int32)(Q * (VertexDistance.Num() - 1)), 0, VertexDistance.Num() - 1)] : 0.0;
	};
	FString PresserLabel;
	for (const FName Name : PresserBones)
	{
		PresserLabel += (PresserLabel.IsEmpty() ? TEXT("") : TEXT(", ")) + Name.ToString();
	}
	FString PartList;
	int32 NumSelf = 0;
	for (const FSoftbodyColliderFieldPart& Part : Field->Parts)
	{
		PartList += FString::Printf(TEXT(" %s%s(%dx%dx%d, %.2f)"), Part.bPressesSelf ? TEXT("*") : TEXT(""), *Part.Bone.ToString(),
			Part.Dims.X, Part.Dims.Y, Part.Dims.Z, Part.VoxelSize);
		NumSelf += Part.bPressesSelf ? 1 : 0;
	}
	UE_LOG(LogSoftbodyColliderField, Display,
		TEXT("Shape field bake: %s - self contact bones %s, slots %s, grid cell %.3f cm, %d parts (%d press the mesh itself), %lld stored cells (%.1f KB), outer surface triangles %d, source vertex to surface distance median %.3f / 95%% %.3f, %.1f s\nParts (* = presses itself; cells, cell size):%s"),
		*Mesh->GetName(), Skeletal && !PresserLabel.IsEmpty() ? *PresserLabel : TEXT("whole mesh"),
		BakedSlots.Num() > 0 ? *FString::JoinBy(BakedSlots, TEXT(", "), [](const FName Name) { return Name.ToString(); }) : TEXT("whole mesh"),
		Voxel, Field->Parts.Num(), NumSelf, TotalBoxVoxels,
		Field->Voxels.Num() * 2.0 / 1024.0, Envelope.TriangleCount(), Percentile(0.5), Percentile(0.95), FPlatformTime::Seconds() - StartSeconds, *PartList);
	return Field;
}
