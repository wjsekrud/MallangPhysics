// Copyright (c) 2026 wjsekrud

// 다른 흔들림 솔버와 함께 쓰기: 머리카락·가슴처럼 뼈대가 흔드는 부위는 후처리 애니메이션 블루프린트의 노드(예: KawaiiPhysics)가
// 본을 옮기고, 표면 장은 그 본 위 피부에 함몰을 그린다(캐리어가 같은 본으로 스키닝된다). 여기서는 그 노드가 부딪힐 몸 충돌체와
// 후처리 블루프린트를 만든다. 노드 종류와 값은 호출자(세팅 스크립트)가 정하므로 이 모듈은 특정 솔버 플러그인에 의존하지 않는다.
#include "SoftbodyBakeLibrary.h"
#include "ScopedTransaction.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "AnimationGraphSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
#include "Misc/StringOutputDevice.h"
#endif
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyPhysicsSetup, Log, All);

namespace
{
	/** 레퍼런스 포즈의 컴포넌트 공간 본 트랜스폼. */
	TArray<FTransform> RefComponentPose(const FReferenceSkeleton& Ref)
	{
		TArray<FTransform> Pose;
		Pose.SetNum(Ref.GetNum());
		for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
		{
			const int32 Parent = Ref.GetParentIndex(Bone);
			Pose[Bone] = Parent >= 0 ? Ref.GetRefBonePose()[Bone] * Pose[Parent] : Ref.GetRefBonePose()[Bone];
		}
		return Pose;
	}

	/** 기본 자세에서 가장 가까이 있는 비울 본 이름과 그 거리(형상 표면이 아니라 거리 함수 값). */
	struct FNearest
	{
		float Distance = TNumericLimits<float>::Max();
		FName Bone;
	};

	template <typename DistanceFn>
	FNearest NearestPoint(const TArray<TPair<FName, FVector>>& Points, DistanceFn&& Distance)
	{
		FNearest Result;
		for (const TPair<FName, FVector>& Point : Points)
		{
			const float D = Distance(Point.Value);
			if (D < Result.Distance)
			{
				Result.Distance = D;
				Result.Bone = Point.Key;
			}
		}
		return Result;
	}

	/** 상자 부호 거리(안쪽 음수). Local은 상자 중심·축 기준 위치, Half는 반 크기. */
	float BoxSignedDistance(const FVector& Local, const FVector& Half)
	{
		const FVector Q = Local.GetAbs() - Half;
		const FVector Outside(FMath::Max(Q.X, 0.0), FMath::Max(Q.Y, 0.0), FMath::Max(Q.Z, 0.0));
		return Outside.Size() + FMath::Min(Q.GetMax(), 0.0);
	}
}

UPhysicsAsset* USoftbodyBakeLibrary::CreateLimitsPhysicsAsset(USkeletalMesh* Mesh, const FString& AssetPath,
	const TArray<FName>& KeepBones, const TArray<FName>& ClearBones, float Clearance, float RadiusScale)
{
	UPhysicsAsset* Source = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!Source)
	{
		UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreateLimitsPhysicsAsset: no mesh, or the mesh has no physics asset. Assign a physics asset to the mesh first."));
		return nullptr;
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const TArray<FTransform> Pose = RefComponentPose(Ref);
	TArray<TPair<FName, FVector>> Clear;
	for (const FName& Name : ClearBones)
	{
		const int32 Bone = Ref.FindBoneIndex(Name);
		if (Bone != INDEX_NONE)
		{
			Clear.Emplace(Name, Pose[Bone].GetLocation());
		}
	}

	UPackage* Package = CreatePackage(*AssetPath);
	UPhysicsAsset* PhysicsAsset = DuplicateObject<UPhysicsAsset>(Source, Package, FName(*FPackageName::GetShortName(AssetPath)));
	PhysicsAsset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	PhysicsAsset->PreviewSkeletalMesh = Mesh;
	for (int32 Index = PhysicsAsset->SkeletalBodySetups.Num() - 1; Index >= 0; --Index)
	{
		if (!KeepBones.Contains(PhysicsAsset->SkeletalBodySetups[Index]->BoneName))
		{
			FPhysicsAssetUtils::DestroyBody(PhysicsAsset, Index);
		}
	}
	for (const FName& Name : KeepBones)
	{
		if (PhysicsAsset->FindBodyIndex(Name) == INDEX_NONE)
		{
			UE_LOG(LogSoftbodyPhysicsSetup, Warning, TEXT("CreateLimitsPhysicsAsset: bone '%s' has no body in the physics asset; skipped"), *Name.ToString());
		}
	}

	// 형상마다 RadiusScale을 곱한 뒤, 기본 자세의 비울 본이 표면에서 Clearance 이상 밖에 있도록 줄인다. 너무 작아지면(0.5 cm)
	// 지운다. 흔들리는 본이 서 있기만 해도 밀려나지 않게 하는 것이 목적이다.
	constexpr float MinSize = 0.5f;
	for (USkeletalBodySetup* Body : PhysicsAsset->SkeletalBodySetups)
	{
		const int32 Bone = Ref.FindBoneIndex(Body->BoneName);
		if (Bone == INDEX_NONE)
		{
			continue;
		}
		const FTransform& BoneTM = Pose[Bone];
		if (!BoneTM.GetScale3D().Equals(FVector::OneVector, 0.01))
		{
			UE_LOG(LogSoftbodyPhysicsSetup, Warning, TEXT("CreateLimitsPhysicsAsset: bone '%s' has scale %s; shapes are shrunk as if the scale were 1"),
				*Body->BoneName.ToString(), *BoneTM.GetScale3D().ToString());
		}
		Body->Modify();
		FKAggregateGeom& Geom = Body->AggGeom;
		for (int32 Elem = Geom.SphereElems.Num() - 1; Elem >= 0; --Elem)
		{
			FKSphereElem& Sphere = Geom.SphereElems[Elem];
			const float Before = Sphere.Radius * RadiusScale;
			const FVector Center = BoneTM.TransformPosition(Sphere.Center);
			const FNearest Near = NearestPoint(Clear, [&](const FVector& P) { return float((P - Center).Size()); });
			Sphere.Radius = FMath::Min(Before, Near.Distance - Clearance);
			UE_LOG(LogSoftbodyPhysicsSetup, Display, TEXT("  %s sphere: radius %.2f -> %.2f (nearest bone %s)"),
				*Body->BoneName.ToString(), Before, Sphere.Radius, *Near.Bone.ToString());
			if (Sphere.Radius < MinSize)
			{
				Geom.SphereElems.RemoveAt(Elem);
			}
		}
		for (int32 Elem = Geom.SphylElems.Num() - 1; Elem >= 0; --Elem)
		{
			FKSphylElem& Capsule = Geom.SphylElems[Elem];
			const float Before = Capsule.Radius * RadiusScale;
			const FVector Axis = Capsule.Rotation.RotateVector(FVector::UpVector) * (0.5f * Capsule.Length);
			const FVector A = BoneTM.TransformPosition(Capsule.Center + Axis);
			const FVector B = BoneTM.TransformPosition(Capsule.Center - Axis);
			const FNearest Near = NearestPoint(Clear, [&](const FVector& P)
			{
				return float((P - FMath::ClosestPointOnSegment(P, A, B)).Size());
			});
			Capsule.Radius = FMath::Min(Before, Near.Distance - Clearance);
			UE_LOG(LogSoftbodyPhysicsSetup, Display, TEXT("  %s capsule: radius %.2f -> %.2f, length %.2f (nearest bone %s)"),
				*Body->BoneName.ToString(), Before, Capsule.Radius, Capsule.Length, *Near.Bone.ToString());
			if (Capsule.Radius < MinSize)
			{
				Geom.SphylElems.RemoveAt(Elem);
			}
		}
		for (int32 Elem = Geom.BoxElems.Num() - 1; Elem >= 0; --Elem)
		{
			FKBoxElem& Box = Geom.BoxElems[Elem];
			FVector Half = FVector(Box.X, Box.Y, Box.Z) * (0.5f * RadiusScale);
			const FTransform BoxTM = FTransform(Box.Rotation, Box.Center) * BoneTM;
			FNearest Near;
			for (int32 Iteration = 0; Iteration < 8; ++Iteration)
			{
				Near = NearestPoint(Clear, [&](const FVector& P) { return BoxSignedDistance(BoxTM.InverseTransformPosition(P), Half); });
				if (Near.Distance >= Clearance - KINDA_SMALL_NUMBER)
				{
					break;
				}
				Half -= FVector(Clearance - Near.Distance);
			}
			UE_LOG(LogSoftbodyPhysicsSetup, Display, TEXT("  %s box: %.1f x %.1f x %.1f -> %.1f x %.1f x %.1f (nearest bone %s)"),
				*Body->BoneName.ToString(), Box.X * RadiusScale, Box.Y * RadiusScale, Box.Z * RadiusScale,
				2.0 * Half.X, 2.0 * Half.Y, 2.0 * Half.Z, *Near.Bone.ToString());
			Box.X = 2.f * Half.X;
			Box.Y = 2.f * Half.Y;
			Box.Z = 2.f * Half.Z;
			if (Half.GetMin() < 0.5f * MinSize)
			{
				Geom.BoxElems.RemoveAt(Elem);
			}
		}
		Body->PhysicsType = PhysType_Kinematic;
	}
	PhysicsAsset->UpdateBodySetupIndexMap();
	PhysicsAsset->UpdateBoundsBodiesArray();
	PhysicsAsset->RefreshPhysicsAssetChange();
	PhysicsAsset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(PhysicsAsset);
	UE_LOG(LogSoftbodyPhysicsSetup, Display, TEXT("Collider physics asset %s: %d bodies, %d bones kept clear (clearance %.2f cm), size scale %.2f"),
		*AssetPath, PhysicsAsset->SkeletalBodySetups.Num(), Clear.Num(), Clearance, RadiusScale);
	return PhysicsAsset;
}

UObject* USoftbodyBakeLibrary::CreatePostProcessAnimBlueprint(USkeletalMesh* Mesh, const FString& AssetPath,
	const TArray<FString>& NodeClasses, const TArray<FString>& NodeSettings)
{
	if (!Mesh || !Mesh->GetSkeleton() || NodeClasses.IsEmpty() || NodeClasses.Num() != NodeSettings.Num())
	{
		UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: needs a mesh with a skeleton, and the same number of node classes and node settings"));
		return nullptr;
	}
	TArray<UClass*> Classes;
	for (const FString& Path : NodeClasses)
	{
		UClass* Class = LoadObject<UClass>(nullptr, *Path);
		if (!Class || !Class->IsChildOf(UAnimGraphNode_Base::StaticClass()))
		{
			UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: '%s' is not an anim graph node class (is its plugin enabled?)"), *Path);
			return nullptr;
		}
		Classes.Add(Class);
	}

	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->BlueprintType = BPTYPE_Normal;
	Factory->ParentClass = UAnimInstance::StaticClass();
	Factory->TargetSkeleton = Mesh->GetSkeleton();
	Factory->PreviewSkeletalMesh = Mesh;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(AssetTools.CreateAsset(FPackageName::GetShortName(AssetPath),
		FPackageName::GetLongPackagePath(AssetPath), UAnimBlueprint::StaticClass(), Factory));
	if (!Blueprint)
	{
		UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: could not create %s"), *AssetPath);
		return nullptr;
	}
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
		{
			AnimGraph = Graph;
		}
	}
	UAnimGraphNode_Root* Root = nullptr;
	if (AnimGraph)
	{
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			Root = Root ? Root : Cast<UAnimGraphNode_Root>(Node);
		}
	}
	if (!Root)
	{
		UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: could not find the AnimGraph output node"));
		return nullptr;
	}

	auto PosePin = [](UEdGraphNode* Node, EEdGraphPinDirection Direction) -> UEdGraphPin*
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && UAnimationGraphSchema::IsPosePin(Pin->PinType))
			{
				return Pin;
			}
		}
		return nullptr;
	};
	const UEdGraphSchema* Schema = AnimGraph->GetSchema();
	const int32 Spacing = 400;
	const int32 StartX = Root->NodePosX - Spacing * (Classes.Num() + 1);

	FGraphNodeCreator<UAnimGraphNode_LinkedInputPose> InputCreator(*AnimGraph);
	UAnimGraphNode_LinkedInputPose* Input = InputCreator.CreateNode();
	Input->NodePosX = StartX;
	Input->NodePosY = Root->NodePosY;
	InputCreator.Finalize();
	UEdGraphPin* Previous = PosePin(Input, EGPD_Output);

	// 노드 설정은 노드 구조체 속성 "Node"에 텍스트로 넣는다(에디터 복사·붙여넣기와 같은 형식, 적지 않은 값은 기본값).
	// 로컬 자세 ↔ 컴포넌트 공간 자세 변환 노드는 스키마가 연결할 때 끼운다.
	for (int32 Index = 0; Index < Classes.Num(); ++Index)
	{
		FGraphNodeCreator<UAnimGraphNode_Base> Creator(*AnimGraph);
		UAnimGraphNode_Base* Node = Creator.CreateNode(false, Classes[Index]);
		Node->NodePosX = StartX + Spacing * (Index + 1);
		Node->NodePosY = Root->NodePosY;
		if (!NodeSettings[Index].IsEmpty())
		{
			FStructProperty* NodeProperty = CastField<FStructProperty>(Classes[Index]->FindPropertyByName(TEXT("Node")));
			FStringOutputDevice Errors;
			const TCHAR* End = NodeProperty
				? NodeProperty->ImportText_Direct(*NodeSettings[Index], NodeProperty->ContainerPtrToValuePtr<void>(Node), Node, PPF_None, &Errors)
				: nullptr;
			if (!End || !Errors.IsEmpty())
			{
				UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: could not read the settings of node %d (%s): %s"),
					Index, *Classes[Index]->GetName(), Errors.IsEmpty() ? TEXT("no Node property, or the settings text is malformed") : *Errors);
				return nullptr;
			}
			// 들어간 값 확인용: 기본값과 다른 것만 다시 텍스트로.
			FStructOnScope Defaults(NodeProperty->Struct);
			FString Applied;
			NodeProperty->ExportText_Direct(Applied, NodeProperty->ContainerPtrToValuePtr<void>(Node), Defaults.GetStructMemory(), Node, PPF_None);
			UE_LOG(LogSoftbodyPhysicsSetup, Display, TEXT("Node %d (%s) settings: %s"), Index, *Classes[Index]->GetName(), *Applied);
		}
		Creator.Finalize();
		UEdGraphPin* In = PosePin(Node, EGPD_Input);
		UEdGraphPin* Out = PosePin(Node, EGPD_Output);
		if (!Previous || !In || !Out || !Schema->TryCreateConnection(Previous, In))
		{
			UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: could not connect the pose pin of node %d (%s)"),
				Index, *Classes[Index]->GetName());
			return nullptr;
		}
		Previous = Out;
	}
	UEdGraphPin* Result = Root->FindPin(TEXT("Result"), EGPD_Input);
	if (!Result || !Schema->TryCreateConnection(Previous, Result))
	{
		UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: could not connect to the output pose"));
		return nullptr;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
	{
		UE_LOG(LogSoftbodyPhysicsSetup, Error, TEXT("CreatePostProcessAnimBlueprint: the blueprint failed to compile"));
		return nullptr;
	}
	{
		// 메시에 다른 후처리 블루프린트가 걸려 있었으면 알리고, 되돌릴 수 있게 한다.
		FScopedTransaction Transaction(NSLOCTEXT("SoftbodyPhysicsSetup", "AssignPostProcessAnimBlueprint", "Softbody: Assign Post Process Anim Blueprint"));
		const UClass* PreviousBlueprint = Mesh->GetPostProcessAnimBlueprint().Get();
		if (PreviousBlueprint && PreviousBlueprint != Blueprint->GeneratedClass.Get())
		{
			UE_LOG(LogSoftbodyPhysicsSetup, Warning, TEXT("%s: post process anim blueprint %s replaced by %s (Edit > Undo restores it)."),
				*Mesh->GetName(), *PreviousBlueprint->GetPathName(), *Blueprint->GetPathName());
		}
		Mesh->Modify();
		Mesh->SetPostProcessAnimBlueprint(TSubclassOf<UAnimInstance>(Blueprint->GeneratedClass.Get()));
	}
	Mesh->MarkPackageDirty();
	Blueprint->MarkPackageDirty();
	UE_LOG(LogSoftbodyPhysicsSetup, Display, TEXT("Post process anim blueprint %s -> %s (%d nodes)"),
		*AssetPath, *Mesh->GetPathName(), Classes.Num());
	return Blueprint;
}
