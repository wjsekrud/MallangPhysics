// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "SoftbodyBakeLibrary.h"
#include "SoftbodyMeshData.h"
#include "UObject/Object.h"
#include "SoftbodyAuthoringData.generated.h"

class UFactory;
class USoftbodyMeshData;
class UStaticMesh;
class UTexture2D;

// 개발: 스태틱 메시 관성 닻(메시 로컬 cm). 살 속 단단한 속이자 캐리어 두께의 기준.
/** A sphere in the mesh's own space (cm) that marks a part that never moves, like a bone inside the flesh. */
USTRUCT(BlueprintType)
struct FSoftbodyStaticRigidSphere
{
	GENERATED_BODY()

	/** Center of the sphere, in the mesh's own space (cm, as in the static mesh editor). */
	UPROPERTY(EditAnywhere, Category = "Softbody", meta = (Units = "cm"))
	FVector Center = FVector::ZeroVector;

	/** Radius of the sphere (cm). */
	UPROPERTY(EditAnywhere, Category = "Softbody",
		meta = (ClampMin = "0.1", Units = "cm"))
	float Radius = 10.f;
};

// 개발: 스태틱 메시의 단단한 속을 정하는 법(일지 231). 스켈레탈은 뼈대에서 읽으므로 쓰지 않는다.
/** Which part of a static mesh stays still while the rest dents and jiggles. */
UENUM(BlueprintType)
enum class ESoftbodyStaticCore : uint8
{
	/** Firm in the middle and soft toward the surface, like a ball or a toy. A core like a bone runs through the middle of the shape, as thick as Bone Thickness. */
	Center,
	/** Rests on a floor or a seat, like a cushion or a pudding. Only the bottom is held. */
	Bottom,
	/** Spheres you place yourself in Rigid Core Spheres. */
	Custom UMETA(DisplayName = "Custom Spheres"),
};

// 개발: 본 목록 항목이 그 본을 무엇으로 치는가. 셋은 서로 배타다 — 덩어리는 뼈가 아니고, 장식은 뼈도 덩어리도 아니다.
/** What a listed bone is in the body. */
UENUM(BlueprintType)
enum class ESoftbodyBoneRole : uint8
{
	/** For bones inside the flesh, like arms and the spine. There is bone here as thick as Bone Thickness. */
	Bone UMETA(DisplayName = "Rigid Core"),
	/** For breast and buttock bones. There is no bone inside, and the flesh it carries moves out of the way as a whole when pressed. */
	Lump UMETA(DisplayName = "Lump"),
	/** For ribbons, necklaces and belts that lie on the body. It is neither bone nor a lump, and it moves with the flesh it lies on. */
	Accessory UMETA(DisplayName = "Accessory"),
};

// 개발: 본별 뼈 설정(설계 §8.1-b) — DefaultRigidShrink를 본마다 바꾸거나, 그 본을 뼈대에서 빼 덩어리(ADR-0012)나 장식으로 친다.
/** What one bone is (rigid core, lump or accessory), and how thick it is when it is rigid core. */
USTRUCT(BlueprintType)
struct FSoftbodyRigidBoneOverride
{
	GENERATED_BODY()

	/** A bone of the target mesh's skeleton. */
	UPROPERTY(EditAnywhere, Category = "Softbody", meta = (GetOptions = "GetSkeletonBoneNames"))
	FName BoneName;

	/**
	 * Lets one entry cover a whole chain or limb from its top bone. The bones below take this entry unless they have one of their own.
	 * Turn it off to change this bone only.
	 */
	UPROPERTY(EditAnywhere, Category = "Softbody", meta = (DisplayName = "Include Child Bones"))
	bool bIncludeChildren = true;

	/**
	 * Pick by what the bone carries. Rigid Core: flesh with bone inside, like an arm. Lump: a breast or buttock with no bone inside, whose
	 * flesh moves out of the way as a whole. Accessory: a ribbon or necklace lying on the body, which moves with the flesh under it.
	 */
	UPROPERTY(EditAnywhere, Category = "Softbody")
	ESoftbodyBoneRole Role = ESoftbodyBoneRole::Bone;

	/**
	 * How thick this bone is, as a fraction of the distance from the bone to the skin. Larger = thinner flesh here: it jiggles less and
	 * its dents are narrower.
	 */
	UPROPERTY(EditAnywhere, Category = "Softbody",
		meta = (DisplayName = "Bone Thickness", ClampMin = "0.05", ClampMax = "1.0", EditCondition = "Role == ESoftbodyBoneRole::Bone"))
	float Shrink = 0.45f;

	// 개발: 옛 저작 에셋의 "Exclude (Make Lump)" 체크. 읽을 때만 쓰고 USoftbodyAuthoringData::PostLoad가 Role = Lump로 옮긴 뒤 끈다.
	/** The old Exclude (Make Lump) check box, read only to move older assets to Role Lump. */
	UPROPERTY()
	bool bExclude = false;
};

// 개발: 저작 에셋(설계 §8.4) — 저작 입력 전체가 이 한 에셋, 베이크 산출물(USoftbodyMeshData)이 파생 에셋. 에디터 전용(IsEditorOnly).
// RunBake = 측정 베이크 → 페인트 층 → 머티리얼(선택) → 표면 캐리어 → 모양 거리장(선택) → 나나이트 테셀레이션(선택) → 저장.
/**
 * Everything needed to make one mesh soft. Set Target Mesh and the settings, then press Run Bake. The result is the Baked Data asset
 * that the Softbody component uses. This asset is editor-only and is not packaged.
 */
UCLASS(BlueprintType)
class SOFTBODYEDITOR_API USoftbodyAuthoringData : public UObject
{
	GENERATED_BODY()

public:
	// --- 대상 ---

	/** The skeletal or static mesh to make soft. Baking adds UV channels to this mesh, so use a mesh in your project's Content folder. */
	UPROPERTY(EditAnywhere, Category = "1. Target", meta = (AllowedClasses = "/Script/Engine.SkeletalMesh,/Script/Engine.StaticMesh"))
	TObjectPtr<UObject> TargetMesh;

	// 개발: 속 메시(ADR-0028). 봉투·살 채움·거리장은 이 메시의 속, 칸 위치·UV·스킨·칠·정점 표는 대상 메시. 패키지에 들어가지 않는다.
	/**
	 * Optional. A closed static mesh that fills the inside of an open or hollow target, such as a trampoline sheet or a hollow can.
	 * Model it with the same position and pivot as the target (a skeletal target can use its bind pose). The surface dents as a thin skin.
	 */
	UPROPERTY(EditAnywhere, Category = "1. Target")
	TObjectPtr<UStaticMesh> VolumeMesh;

	/** Where the baked assets are written. Empty = the folder of this asset. */
	UPROPERTY(EditAnywhere, Category = "1. Target")
	FString OutputFolder;

	// --- 베이크 프리셋 (설계 §8.1-b③) ---

	// 개발: tet 셀 크기 = 바운드 최장변 × 이 값(키 180 cm 캐릭터면 0.015 ≈ 2.7 cm).
	/**
	 * How finely the flesh is measured, as a fraction of the mesh's longest side.
	 * Smaller = more detail in how soft each area is, but a slower bake.
	 */
	UPROPERTY(EditAnywhere, Category = "2. Bake", meta = (DisplayName = "Measure Cell Size", ClampMin = "0.005", ClampMax = "0.1"))
	float CellSizeFrac = 0.015f;

	/** Skeletal meshes: how many bone groups can jiggle separately (each group gets its own entry in Inertia Group Overrides). */
	UPROPERTY(EditAnywhere, Category = "2. Bake", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxInertiaGroups = 16;

	// 개발: 겹친 층(ADR-0014) — 옷 정점이 밑 살의 칠·출렁임 그룹·모양·덩어리 값을 따르는 거리. 이 거리의 절반부터 서서히 덜 따른다. 0이면 끈다.
	// 함께 눌리는 칸은 이 값을 쓰지 않고 접촉 지도 굽기가 정한다(규칙 1: 봉투 칸 둘 안 바깥 층, 규칙 3: 봉투 안에 묻힌 칸 — ADR-0007 §3.4·ADR-0028).
	// 이 값을 그 판정에도 거는 안(규칙 1만은 빠지는 칸 대부분을 규칙 3이 다시 잡아 효과가 작고, 규칙 3까지면 옷 밑 피부 수만 칸이 따로 눌림)은
	// 설계 과제(일지 332).
	/**
	 * For cloth on the skin: a cloth vertex with skin within this distance (cm) under it takes that flesh's Depth, Jiggle and Volume paint.
	 * It jiggles with that flesh, and cloth farther than half this distance follows less. 0 = off. Denting with the cloth does not use it.
	 */
	UPROPERTY(EditAnywhere, Category = "2. Bake", meta = (ClampMin = "0.0", ClampMax = "10.0", Units = "cm"))
	float LayerMaxGap = 1.0f;

	// 개발: 스태틱 메시의 속(일지 231). 바운드 중심·피벗으로 짐작하지 않고, 어떻게 놓이는지를 고른다(놓임은 모양만으로 알 수 없다).
	// C++ 기본값은 Custom — 이 값이 생기기 전에 저장한 에셋은 적어 둔 구를 그대로 쓴다. 새 에셋은 팩토리가 Center로 만든다.
	/**
	 * Static meshes: which part stays still while the rest dents and jiggles.
	 * Center runs a core through the middle of the shape, Bottom holds only the bottom, and Custom Spheres uses Rigid Core Spheres.
	 */
	UPROPERTY(EditAnywhere, Category = "3. Rigid Core (Bones)", meta = (DisplayName = "Core Placement", EditCondition = "bStaticTarget", EditConditionHides))
	ESoftbodyStaticCore StaticCore = ESoftbodyStaticCore::Custom;

	// 개발: Core Placement가 Custom일 때만. 하나 이상 필수.
	/**
	 * Core Placement Custom Spheres: spheres (mesh space, cm) for the part that never moves. At least one must sit inside the mesh.
	 * Flesh far from the spheres jiggles and dents more. A flat base is the top of a very large sphere.
	 */
	UPROPERTY(EditAnywhere, Category = "3. Rigid Core (Bones)",
		meta = (DisplayName = "Rigid Core Spheres", EditCondition = "bStaticTarget && StaticCore == ESoftbodyStaticCore::Custom", EditConditionHides))
	TArray<FSoftbodyStaticRigidSphere> StaticRigidSpheres;

	// --- 강체 본 (설계 §8.1-b①) ---

	// 개발: 뼈 = 스킨 웨이트가 걸린 본 → 스킨 웨이트가 걸린 자식 본의 선, 반지름 = 선에서 살갗까지 거리 × 이 값(ADR-0026). 피직스 에셋 불필요.
	/**
	 * Skeletal meshes and Core Placement Center: how thick the bones or the core are, as a fraction of the distance to the skin.
	 * Larger = thinner flesh that jiggles less and dents narrower. Paint bones the skeleton lacks (ribs, pelvis, skull) dark in Depth.
	 */
	UPROPERTY(EditAnywhere, Category = "3. Rigid Core (Bones)",
		meta = (DisplayName = "Bone Thickness", ClampMin = "0.05", ClampMax = "1.0", EditCondition = "!bStaticTarget || StaticCore == ESoftbodyStaticCore::Center", EditConditionHides))
	float DefaultRigidShrink = 0.45f;

	/**
	 * For bones skinned to Softbody slots that differ from the rest: breasts and buttocks (Lump), ribbons (Accessory) or another thickness.
	 * Unlisted bones are rigid core. Bones that move only other slots (hair, skirt) need no entry. No role takes flesh out of Softbody slots.
	 */
	UPROPERTY(EditAnywhere, Category = "3. Rigid Core (Bones)",
		meta = (DisplayName = "Bone Overrides", TitleProperty = "BoneName", EditCondition = "!bStaticTarget", EditConditionHides))
	TArray<FSoftbodyRigidBoneOverride> RigidBoneOverrides;

	// 개발: 디테일 패널에서 스태틱 메시에만 쓰는 값과 스켈레탈에만 쓰는 값을 가르는 표지(저장 안 함, 일지 231). 대상이 바뀌거나 에셋을 읽을 때 정한다.
	UPROPERTY(Transient)
	bool bStaticTarget = false;

	// 개발: 살이 한 덩어리로 흔들리는 두께(메시 로컬 cm, ADR-0027). 0보다 크면 쓴다(일지 298 전에는 Flesh Softness를 칠했을 때만).
	// 개발: 기본값 3(일지 316) — Inertia Amount 눈금의 기준 살(ReferenceFleshReach 3 cm)과 같게. 이 판 전에 저장된 에셋은
	// Serialize가 옛 기본값 0을 지킨다.
	/**
	 * Flesh thicker than this (cm) does not jiggle more for being thicker, so areas with the same Jiggle paint jiggle about the same.
	 * 0 = off: the deepest flesh jiggles the most, so a belly with no bone inside jiggles far more than a chest painted the same.
	 */
	UPROPERTY(EditAnywhere, Category = "3. Rigid Core (Bones)", meta = (ClampMin = "0.0", UIMax = "10.0", Units = "cm"))
	float FleshReach = 3.f;

	// --- 페인트 (설계 §8.2/8.3) ---

	// 개발(ADR-0037 §3.3): 압축성 재료값 하나. 실행 중 v = 이 값 × 슬롯 칠(캐리어 칸), 컴포넌트 Volume Preservation Scale은 그 v의 부피 비율 K/G에
	// 곱한다(일지 298, 원칙 P08). 측정 굽기는
	// 이 값 × 칠을 곡선 v → ν로 살 채움 푸아송 비에. 바꾸면 BakedData에 바로 옮긴다(함몰·솟음은 굽지 않고 바뀜, 덩어리는 Run Bake 뒤).
	// 컴포넌트 배수는 굽기가 볼 수 없어 덩어리 모양에 들지 않는다(실행 중 튜닝용).
	/**
	 * Volume preservation: 1 for flesh or jelly, 0 for foam or a sponge, and values in between act like a denser foam.
	 * Flesh bulges around a dent and a pushed lump spreads sideways, while foam sinks in place. The Volume paint multiplies this value.
	 */
	UPROPERTY(EditAnywhere, Category = "4. Paint", meta = (DisplayName = "Volume Preservation", ClampMin = "0.0", ClampMax = "1.0"))
	float VolumePreservation = 1.f;

	// 개발: 키는 슬롯 이름(리임포트로 순서가 바뀌어도 유지). 미기재 슬롯은 칠 안 함(흰색). 소스 리임포트 시 자동 재변환. 배치는 고정(ADR-0038, 원칙 P08).
	/**
	 * Paint for each material slot: one RGBA texture in the slot's UV0 layout with R Depth, G Jiggle, B Volume and A Flex.
	 * White is unpainted in every channel, and slots not listed are unpainted.
	 */
	UPROPERTY(EditAnywhere, Category = "4. Paint", meta = (GetKeyOptions = "GetMaterialSlotNameOptions"))
	TMap<FName, FSoftbodyPaintSlotMask> SlotMasks;

	/**
	 * Bones already moved by another jiggle system, such as KawaiiPhysics or simulated physics bodies.
	 * Flesh skinned to them and the bones below gets no Softbody jiggle, or less where only partly weighted. Applied with Reapply Paint.
	 */
	UPROPERTY(EditAnywhere, Category = "4. Paint", meta = (GetOptions = "GetSkeletonBoneNames"))
	TArray<FName> PhysicsDrivenBones;

	// --- 머티리얼 (R6 §2) ---

	// 개발: /SoftbodySystem/M_SoftbodyHost를 산출물 폴더로 복제(M_<메시>_Softbody), 슬롯마다 MI_<메시>_<슬롯>. 이미 MF_SoftbodyKernel이 든 슬롯은 건너뛴다.
	/**
	 * Run Bake also puts an instance of the plugin's example material on each of the Softbody Material Slots.
	 * It copies only the base color, normal and roughness textures. Leave it off if your master material has the Softbody functions.
	 */
	UPROPERTY(EditAnywhere, Category = "5. Material")
	bool bCreateSoftbodyMaterial = false;

	/**
	 * Slots that get the Softbody material (skin, tight clothing). Only bones skinned to Softbody slots become rigid core and inertia groups.
	 * Bones that only move other slots (hair, skirt) are left out. A removed slot stays a Softbody slot until its original material is back.
	 */
	UPROPERTY(EditAnywhere, Category = "5. Material", meta = (GetOptions = "GetMaterialSlotNameOptions", EditCondition = "bCreateSoftbodyMaterial"))
	TArray<FName> SoftbodyMaterialSlots;

	// --- 표면 접촉 (ADR-0003 표면 캐리어) ---

	// 개발: 표면 캐리어. 없으면 흔들림만 되고 눌림은 전혀 없다.
	/** Bakes the contact map: the grid on the surface where dents, bulges and drag are worked out. Without it the mesh only jiggles. */
	UPROPERTY(EditAnywhere, Category = "6. Contact Map", meta = (DisplayName = "Bake Contact Map"))
	bool bBakeSurfaceCarrier = true;

	/** Cells along one side of the contact map (a power of two, 64 to 2048). Higher = sharper small dents but more GPU time and memory. */
	UPROPERTY(EditAnywhere, Category = "6. Contact Map",
		meta = (DisplayName = "Contact Map Resolution", ClampMin = "64", ClampMax = "2048", EditCondition = "bBakeSurfaceCarrier"))
	int32 CarrierResolution = 512;

	/**
	 * The material slots that can be pressed. Empty = the slots that use a Softbody material (or the whole mesh if none do yet). Leaving
	 * out slots that are never pressed, such as the face or hair, gives the pressable slots more detail.
	 */
	UPROPERTY(EditAnywhere, Category = "6. Contact Map",
		meta = (DisplayName = "Pressable Slots", GetOptions = "GetMaterialSlotNameOptions", EditCondition = "bBakeSurfaceCarrier"))
	TArray<FName> CarrierEnvelopeSlots;

	// 개발: ADR-0007. 기본은 메시 UV0 아일랜드 이음매를 차트 경계로.
	/**
	 * Where the contact map is split into pieces. Dents are slightly less exact across a split, so by default it follows the UV0 seams
	 * the artist already hid on the mesh.
	 */
	UPROPERTY(EditAnywhere, Category = "6. Contact Map", meta = (DisplayName = "Contact Map Seams", EditCondition = "bBakeSurfaceCarrier"))
	ESoftbodyCarrierCharts CarrierCharts = ESoftbodyCarrierCharts::MeshUV0;

	// 개발: Thickness Core Spheres(두께를 재는 구 목록, CarrierCoreSpheres)는 쓰는 에셋이 없어 지웠다(일지 316). 두께는
	// 스켈레탈이면 뼈대의 본 선, 스태틱이면 Core Placement의 속까지 잰다.

	// 개발: 코어가 바닥에만 있는 물체는 옆면 살 두께가 실제보다 두꺼워져 윗면을 누르면 옆면까지 파였다(일지 102).
	/**
	 * Upper limit on flesh thickness (cm, mesh space): deeper than this from the surface counts as solid. 0 = no limit.
	 * Use it on boxes and cushions whose core sits only at the bottom, so pressing the top does not dent the sides.
	 */
	UPROPERTY(EditAnywhere, Category = "6. Contact Map",
		meta = (DisplayName = "Max Flesh Thickness", ClampMin = "0.0", Units = "cm", EditCondition = "bBakeSurfaceCarrier"))
	float CarrierMaxThickness = 0.f;

	// 개발: 부위 나누기(셰이더 "부위 나누기", ADR-0037). 겉면은 이어져도 속살은 따로라는 것은 메시에 없는 정보라 저작자가 정한다. 굽기가 칸 스킨
	// 웨이트로 칸마다 두 묶음 몫을 담고(USoftbodyMeshData::CarrierSeparationTex) 실행 중에는 읽기만 한다. 묶음은 둘뿐이다 — 두 묶음이면 함몰 채널 셋이
	// 함몰 풀이 한 디스패치 안에 들어간다(공유 메모리 32 KB, 셋이면 6채널이라 두 번째 풀이가 필요하다). 쌍이 여럿이면 굽기가 두 묶음으로 나눈다
	// (SoftbodyBakeShared::ResolveSeparatedPairs — 셋이 서로 떨어져야 하는 쌍은 빼고 경고). 본은 Physics Driven Bones처럼 고르고 자식 본은 늘 같이
	// 든다(부위를 본 하나 아래에서 끊을 일이 없다). 미리보기 Separated Parts 보기가 같은 함수로 지금 값을 칠한다.
	/**
	 * Skeletal meshes: soft parts that share one surface but not the flesh under it, such as two breasts under a shirt bridging the cleavage.
	 * Pick the top bones of each side. Pressing one side does not raise, dent or drag the other. Applied with Rebake Contact Map.
	 */
	UPROPERTY(EditAnywhere, Category = "6. Contact Map", meta = (DisplayName = "Separated Pairs", EditCondition = "bBakeSurfaceCarrier"))
	TArray<FSoftbodySeparatedPair> SeparatedPairs;

	/** Advanced: cell size (cm, mesh space) for finding the mesh's outer surface. 0 = automatic (longest side / 256). */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "6. Contact Map",
		meta = (DisplayName = "Envelope Cell Size", ClampMin = "0.0", Units = "cm", EditCondition = "bBakeSurfaceCarrier"))
	float CarrierEnvelopeCellSize = 0.f;

	/** Advanced: triangle count of the simplified outer surface. Raise it if flesh thickness on large curved areas looks stepped. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "6. Contact Map",
		meta = (DisplayName = "Envelope Triangles", ClampMin = "1000", ClampMax = "400000", EditCondition = "bBakeSurfaceCarrier"))
	int32 CarrierTargetTriangles = 40000;

	// --- 모양 거리장 (ADR-0004 자기 접촉·실제 모양으로 누르기) ---

	/**
	 * Bakes this mesh's real shape so it presses other Softbody meshes with that shape instead of its simple collision or physics asset.
	 * On a skeletal mesh the Self Contact Bones also press the character's own flesh. The shape field is stored on the Target Mesh.
	 */
	UPROPERTY(EditAnywhere, Category = "7. Shape Field", meta = (DisplayName = "Bake Shape Field"))
	bool bBakeColliderField = false;

	// 개발: 굽기는 모든 본을 부위로 만들고 이 본들 아래 부위에만 bPressesSelf를 켠다(ADR-0004 §11). 프로퍼티 이름은 저장된 에셋 때문에 그대로.
	/**
	 * Skeletal meshes: the bones (child bones included) that also press the character's own flesh, such as hand_l and hand_r.
	 * Empty = every bone presses, for a mesh that only presses, such as a hand mesh. Other Softbody meshes are pressed by the whole body.
	 */
	UPROPERTY(EditAnywhere, Category = "7. Shape Field",
		meta = (DisplayName = "Self Contact Bones", GetOptions = "GetSkeletonBoneNames", EditCondition = "bBakeColliderField"))
	TArray<FName> PresserBones;

	/**
	 * The material slots whose shape presses, such as skin and clothing. Empty = the whole mesh. Leave out hair and accessories so they
	 * never dent other Softbody meshes.
	 */
	UPROPERTY(EditAnywhere, Category = "7. Shape Field",
		meta = (DisplayName = "Shape Field Slots", GetOptions = "GetMaterialSlotNameOptions", EditCondition = "bBakeColliderField"))
	TArray<FName> ShapeFieldSlots;

	/** Advanced: cell size of the shape field (cm, mesh space). 0 = automatic (fine enough for fingers). */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "7. Shape Field",
		meta = (DisplayName = "Shape Field Cell Size", ClampMin = "0.0", Units = "cm", EditCondition = "bBakeColliderField"))
	float ColliderFieldVoxelSize = 0.f;

	// 개발: 부위 절단면을 눌리는 쪽 가장자리 다듬기가 콜라이더 가장자리로 읽어 마디마다 고랑이 팠다(일지 71·73). 사슬 방향으로만 늘린다.
	/**
	 * Advanced, skeletal meshes: how far (cm) each bone's part of the shape reaches into its neighbor's. 0 = no overlap.
	 * Too little shows grooves at the finger joints in the dent.
	 */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "7. Shape Field",
		meta = (DisplayName = "Shape Field Part Overlap", ClampMin = "0.0", UIMax = "3.0", Units = "cm", EditCondition = "bBakeColliderField"))
	float ColliderFieldPartOverlap = 1.f;

	// --- 나나이트 테셀레이션 (ADR-0006 정점 사이 함몰) ---

	// 개발: 메시와 모든 슬롯 머티리얼을 바꾼다. 반투명 슬롯이면 실패, 스켈레탈 모프 타깃은 나나이트 스키닝이 무시한다.
	/**
	 * Sets the mesh up to draw with Nanite tessellation, so narrow dents such as fingertips appear between vertices on a low-density mesh.
	 * It changes the mesh and all of its slot materials. Fails if a slot is translucent. Nanite ignores morph targets on skeletal meshes.
	 */
	UPROPERTY(EditAnywhere, Category = "8. Nanite Tessellation", meta = (DisplayName = "Set Up Nanite Tessellation"))
	bool bConfigureNaniteTessellation = false;

	/** The depth range (cm) that tessellation can draw. Dents and bulges beyond it are cut off. */
	UPROPERTY(EditAnywhere, Category = "8. Nanite Tessellation", meta = (ClampMin = "0.1", ClampMax = "50.0", Units = "cm", EditCondition = "bConfigureNaniteTessellation"))
	float NaniteDisplacementMagnitude = 2.f;

	/** Where that range sits (0 to 1). 0.75 = three quarters of it inward for dents, one quarter outward for bulges. */
	UPROPERTY(EditAnywhere, Category = "8. Nanite Tessellation", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bConfigureNaniteTessellation"))
	float NaniteDisplacementCenter = 0.75f;

	/** How far (cm) Nanite expects vertices to move. Must be more than the largest dent plus jiggle, or the movement is cut off. */
	UPROPERTY(EditAnywhere, Category = "8. Nanite Tessellation", meta = (ClampMin = "1.0", ClampMax = "200.0", Units = "cm", EditCondition = "bConfigureNaniteTessellation"))
	float NaniteMaxWorldPositionOffset = 30.f;

	// --- 산출물 ---

	/** The baked result. Run Bake fills it in; put this asset in the Softbody component's Mesh Data. */
	UPROPERTY(EditAnywhere, Category = "9. Output")
	TObjectPtr<USoftbodyMeshData> BakedData;

	/** Problems found by the last Validate. 0 = all good. -1 = not checked since the last bake. Details are in the Output Log. */
	UPROPERTY(VisibleAnywhere, Category = "9. Output")
	int32 LastValidationErrors = -1;

	/**
	 * Saves what the bake changed as soon as it finishes: the baked assets, the target mesh, the materials it wired and this asset.
	 * Other assets that already had unsaved changes before the bake are left for you to save.
	 */
	UPROPERTY(EditAnywhere, Category = "9. Output")
	bool bSaveAfterBake = true;

	// --- 실행 ---

	/**
	 * Runs every bake stage that is turned on, from measuring the flesh to saving (takes minutes).
	 * Press it again on a skeletal mesh whose Softbody slots changed.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "1. Target", meta = (DisplayName = "Run Bake"))
	void RunBake();

	// 개발: 슬롯 칠 → T_*_Paint 재변환 + 캐리어가 있으면 캐리어도. 측정이 읽는 칠이 지난 측정 뒤 바뀌었으면 Run Bake로 돈다(ADR-0038 §3.6, P04).
	/**
	 * Converts the slot paint again and rebakes the contact map (seconds to tens of seconds).
	 * If the paint changed since the last Run Bake, it runs Run Bake instead.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "4. Paint", meta = (DisplayName = "Reapply Paint"))
	void RunPaintConvert();

	/** Creates and assigns the Softbody material again, then rebakes the contact map if there is one. Needs Baked Data. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "5. Material", meta = (DisplayName = "Recreate Material"))
	void RunCreateMaterial();

	/**
	 * Bakes only the contact map again (tens of seconds), after a change to the Softbody material or a contact map setting.
	 * On a skeletal mesh whose Softbody slots changed, press Run Bake instead.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "6. Contact Map", meta = (DisplayName = "Rebake Contact Map"))
	void RunSurfaceCarrier();

	/** Bakes only the shape field again (tens of seconds). Press it after changing Self Contact Bones or the shape field settings. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "7. Shape Field", meta = (DisplayName = "Rebake Shape Field"))
	void RunColliderField();

	/** Applies the Nanite tessellation settings again (needs Baked Data). Press it after changing the range settings. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "8. Nanite Tessellation", meta = (DisplayName = "Reapply Nanite Tessellation"))
	void RunNaniteTessellation();

	/** Checks the baked data, the mesh, its materials and shape field without baking again. Results go to the Output Log and Last Validation Errors. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "9. Output", meta = (DisplayName = "Validate"))
	void RunValidation();

	/** 소스 마스크 리임포트 시 자동 재변환 훅 (모듈이 UImportSubsystem에 등록) */
	static void HandleAssetPostImport(UFactory* Factory, UObject* CreatedObject);

	virtual bool IsEditorOnly() const override { return true; }
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override;

	// 개발(일지 298, ADR-0038 §3.8): 옛 배치 슬롯 칠을 고정 배치로 옮긴다 — 이관 전용, 시연 이관(6단계) 뒤 지운다. 굽기가 옛 배치 에셋을 만나면 먼저 부른다.
	/** Moves slot paint made before the fixed paint layout into that layout (R Depth, G Jiggle, B Volume and A Flex). Returns true if anything moved. */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	bool ConvertLegacyPaint();

	// 개발: 옛 배치로 저장된 에셋인가(PostLoad가 저장 판으로 정한다, 이관이 끈다). 저장해 두어 옮기기 전에 다시 저장해도 옛 배치로 남는다.
	/** True while the slot paint still uses the layout from before the fixed paint layout. */
	UPROPERTY()
	bool bLegacyPaintLayout = false;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

	/**
	 * 스태틱 메시의 단단한 속 구(메시 로컬 cm, w = 반지름)를 Core Placement대로 만든다. Center·Bottom은 메시(속 메시가 있으면 그것) 모양에서,
	 * Custom은 Rigid Core Spheres 그대로. 측정 굽기와 접촉 지도 굽기(살 두께 기준)가 같은 구를 쓴다.
	 * @param bLogEmpty    Custom인데 구가 없을 때 오류를 남길지(접촉 지도 굽기는 구 없이 광선 반 길이로 잰다).
	 * @param bLogFailure  Center·Bottom이 속을 못 놓았을 때 오류를 남길지(SA 창 미리보기는 창에 적는다).
	 * @return 구가 하나 이상이면 true
	 */
	bool ResolveStaticCoreSpheres(UStaticMesh* Static, TArray<FVector4>& OutSpheres, bool bLogEmpty, bool bLogFailure = true) const;

	// 개발: SA 창의 단단한 속 미리보기(일지 232). 측정 굽기와 같은 함수·같은 반지름 하한이라 보이는 구가 굽기가 붙드는 구다.
	/**
	 * The rigid core Run Bake would hold still with the current settings, as spheres in mesh space: xyz is the center and w the radius (cm).
	 * Out Status describes the core, or what is wrong when Out Problem is true.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	TArray<FVector4> PreviewRigidCore(FString& OutStatus, bool& bOutProblem) const;

	/** 실행 버튼(Run Bake·Reapply Paint 등)이 끝날 때마다 하나 오른다. SA 창 Simulation 탭이 보고 시뮬레이션을 새 굽기로 다시 세운다(일지 234). */
	uint32 GetRunSerial() const { return RunSerial; }

private:
	// 개발: 본 이름 드롭다운 옵션 = 대상 스켈레탈 메시의 모든 본 이름
	/** The bone names of the target mesh's skeleton, for the bone dropdowns. */
	UFUNCTION()
	TArray<FString> GetSkeletonBoneNames() const;

	// 개발: 슬롯 마스크 키 드롭다운 옵션 = 대상 메시의 머티리얼 슬롯 이름들
	/** The material slot names of the target mesh, for the slot dropdowns. */
	UFUNCTION()
	TArray<FString> GetMaterialSlotNameOptions() const;

	FString ResolveOutputFolder() const;

	/** 굽지 않고 구운 데이터로 옮기는 재료값(Volume Preservation, ADR-0037 §3.3)을 BakedData에 맞춘다. 다르면 Modify한다. */
	void SyncMaterialValues();

	/** Bone Overrides → 본 이름별 굵기(뺀 본 = −1). 측정 굽기와 미리보기가 같이 쓴다. */
	TMap<FName, float> BoneOverrideMap() const;

	/**
	 * 살 슬롯(일지 239): 단단한 속을 정하는 슬롯 = 소프트바디 머티리얼이 붙은 슬롯 + (Create Softbody Material이면) Softbody Material Slots.
	 * 측정 굽기와 미리보기가 같이 쓴다. 비면 메시 전체.
	 */
	TSet<FName> ResolveCoreSlots() const;

	/**
	 * 마지막 굽기가 지금과 다른 뼈대 설정(Bone Overrides·Default Bone Thickness·살 슬롯)으로 구워졌으면 그 내용을 한 줄로(같으면 빈 문자열).
	 * 굽기 데이터에 남은 표(RigidShrinkPerBone·RigidCoreShrink·RigidCoreSlots)와 견준다 — SA 변경을 저장하지 않은 채 크래시가 나면 SA 창과
	 * 구운 데이터가 말없이 어긋났다(일지 239). 스켈레탈만.
	 */
	FString DescribeStaleCore() const;

	/**
	 * 곧 소프트바디 머티리얼을 받을 슬롯: Run Bake의 머티리얼 단계(Create Softbody Material)가 입힐 Softbody Material Slots 가운데 아직 소프트바디
	 * 머티리얼이 아닌 것(이미 그러면 머티리얼 단계가 그대로 둔다). 측정·칠 단계가 머티리얼 단계보다 먼저 돌아도 출렁임 기준과 겹친 층이 따를 면을
	 * 두 번째 굽기와 같게 정하도록 넘긴다. Create Softbody Material이 꺼져 있으면 비어 있다.
	 */
	TArray<FName> PendingSoftbodySlots() const;

	// 단계. 각각 성공 여부를 돌려주고 실패 원인은 LogSoftbodyAuthoring에 남긴다.
	bool BakeMeasuredStage();
	/** PendingSlots: 같은 실행의 머티리얼 단계가 곧 입힐 슬롯(PendingSoftbodySlots — Run Bake만 넘긴다). */
	bool PaintConvertStage(TConstArrayView<FName> PendingSlots = {});
	bool CreateMaterialStage();
	bool SurfaceCarrierStage();
	bool ColliderFieldStage();
	bool NaniteTessellationStage();

	/**
	 * 산출물 폴더·대상 메시·슬롯 머티리얼·이 에셋 중 이번 실행이 더럽힌 패키지를 저장한다(BeginRun 전부터 더러웠던 것은 빼고 알린다).
	 * 이 에셋은 전부터 더러웠어도 저장한다 — 그 값이 이번 굽기의 입력이라, 구운 데이터만 저장되고 이 에셋이 크래시로 사라지면 둘이 어긋난다(일지 239).
	 */
	void SaveOutputs();

	/** 실행 버튼마다 처음에 부른다: 지금 저장하지 않은 패키지를 적어 둔다(SaveOutputs가 빼는 것). */
	void BeginRun();

	/** BeginRun 때 이미 저장하지 않은 상태였던 패키지 이름. */
	TSet<FName> DirtyBeforeRun;

	/** GetRunSerial. 저장하지 않는다. */
	uint32 RunSerial = 0;
};

// 개발: 콘텐츠 브라우저 신규 생성 팩토리
/** Creates a new Softbody Authoring Data asset from the Content Browser. */
UCLASS()
class USoftbodyAuthoringDataFactory : public UFactory
{
	GENERATED_BODY()

public:
	USoftbodyAuthoringDataFactory();
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName,
		EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
};
