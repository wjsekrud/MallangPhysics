// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SoftbodyMeshData.h"
class UMaterialInterface;

#include "SoftbodyBakeLibrary.generated.h"

class UPhysicsAsset;
class USkeletalMesh;
class USoftbodyColliderField;
class UStaticMesh;
class USoftbodyMeshData;
class UTexture2D;

// 개발: 굽기 안에서 칠 한 종류를 실제로 어디서 읽는가(텍스처 채널·상수·칠 안 함). 일지 298(ADR-0038)부터 사용자가 고르지 않는다 — 고정 배치와
// 채널마다의 Fill에서 정해진다(SoftbodyBakeShared::PaintSource). 옛 슬롯 마스크 필드(이관 전용)도 이 값을 쓴다.
/** Where one kind of paint is read from inside the bake. */
UENUM(BlueprintType)
enum class ESoftbodyMaskSource : uint8
{
	Keep UMETA(DisplayName = "Keep (unpainted, white)"),
	R UMETA(DisplayName = "Texture R"),
	G UMETA(DisplayName = "Texture G"),
	B UMETA(DisplayName = "Texture B"),
	A UMETA(DisplayName = "Texture A"),
	One UMETA(DisplayName = "White (1) everywhere"),
	Zero UMETA(DisplayName = "Black (0) everywhere"),
};

// 개발(일지 298, ADR-0038 §3.1): 칠 채널 하나를 텍스처에서 읽을지, 슬롯 전체를 흰색(칠 안 함)·검정으로 둘지.
/** How one channel of a slot's paint is filled. */
UENUM(BlueprintType)
enum class ESoftbodyPaintFill : uint8
{
	Texture UMETA(DisplayName = "Paint Texture", ToolTip = "Read this channel from the slot's Paint Texture. With no texture it is white (unpainted)."),
	White UMETA(DisplayName = "White everywhere (unpainted)"),
	Black UMETA(DisplayName = "Black everywhere"),
};

// 개발(일지 298, ADR-0038, 원칙 P08): 슬롯마다 RGBA 칠 한 장, 채널 자리는 고정 — R Depth(살 두께, 뼈는 여기에만) · G Jiggle(살의 무름) ·
// B Volume(압축성) · A Flex(겉 굽힘, 코드 이름 Softness — 저장된 자산·스크립트가 읽는 속성 이름이라 그대로 둔다). 흰색은 어느 채널이든
// "칠 안 함"이다. 채널마다 Fill로 텍스처 대신 슬롯 전체 흰색·검정을 고를 수 있다.
// 굽기 안의 페인트 층(PaintTex)은 이와 따로다: R = Jiggle(캐리어 굽기가 덩어리 손 밑 살층 강성에 쓴다), G = Depth, B = 관성량(칠 아님 — 기준 맞춤·
// Physics Driven Bones), A = Volume(GeneratePaintInternal).
// 아래 "옛 배치" 필드는 일지 298 전 자산을 읽어 이관 함수(USoftbodyAuthoringData::ConvertLegacyPaint)만 쓴다 — 굽기는 읽지 않는다.
/**
 * Paint for one material slot: one RGBA texture in the slot's UV0 layout. White means unpainted in every channel.
 * R Depth: flesh depth before bone. G Jiggle: how soft it is. B Volume: whether it keeps its volume. A Flex: how the surface bends.
 */
USTRUCT(BlueprintType)
struct FSoftbodyPaintSlotMask
{
	GENERATED_BODY()

	/** One texture holds all four paints in the slot's UV0 layout. R is Depth, G Jiggle, B Volume and A Flex. Turn off sRGB. A texture without alpha reads as bare skin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Paint Texture"))
	TObjectPtr<UTexture2D> Texture;

	/**
	 * For bone closer under the skin than the skeleton's bones, such as ribs, shins and the head. White = flesh down to the rigid core.
	 * Gray = bone that much closer, so dents are narrower and the flesh jiggles less. Black = bone right under the skin: no dent, no jiggle.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Depth (R)"))
	ESoftbodyPaintFill DepthFill = ESoftbodyPaintFill::Texture;

	/**
	 * For flesh firmer in some places than others, such as a belly over muscle next to a breast. White = soft flesh, gray = firmer flesh.
	 * Black = flesh as firm as bone that does not jiggle but still dents. Paint bone in Depth instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Jiggle (G)"))
	ESoftbodyPaintFill JiggleFill = ESoftbodyPaintFill::Texture;

	/**
	 * For parts made of a different material, such as a foam base under a jelly toy.
	 * White = flesh that keeps its volume, so skin around a dent rises. Black = foam that sinks in place. It multiplies Volume Preservation.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Volume (B)"))
	ESoftbodyPaintFill VolumeFill = ESoftbodyPaintFill::Texture;

	/**
	 * For skin that bends stiffer than bare flesh, such as leather or the pips of a soft die. White = bare skin, darker gray = stiffer.
	 * Black = a rigid plate: each connected black area sinks and tilts as one piece. Needs Contact Detail Full.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Flex (A)"))
	ESoftbodyPaintFill SoftnessFill = ESoftbodyPaintFill::Texture;

	// --- 옛 배치(일지 298 전, 이관 전용 — 굽기는 읽지 않는다). 기본값은 옛 기본 그대로 둔다(저장하지 않은 기본값도 옛 뜻으로 읽히게). ---
	/** Old paint layout, kept only to convert assets saved before the fixed RGBA layout. The bake does not read it. */
	UPROPERTY()
	ESoftbodyMaskSource NormalGain = ESoftbodyMaskSource::G;
	UPROPERTY()
	ESoftbodyMaskSource Inertia = ESoftbodyMaskSource::Keep;
	UPROPERTY()
	ESoftbodyMaskSource Volume = ESoftbodyMaskSource::Keep;
	UPROPERTY()
	ESoftbodyMaskSource SkinTension = ESoftbodyMaskSource::Keep;
	UPROPERTY()
	ESoftbodyMaskSource FleshSoftness = ESoftbodyMaskSource::Keep;
	UPROPERTY()
	ESoftbodyMaskSource HardSurface = ESoftbodyMaskSource::Zero;
	UPROPERTY()
	TObjectPtr<UTexture2D> HardSurfaceTexture;
};

// 개발: 부위 나누기(Separated Pairs) 한 쌍 — 두 쪽 본 목록(자식 본 포함). 접촉 지도는 두 묶음만 담으므로 굽기가 모든 쌍을 두 묶음으로
// 나눈다(SoftbodyBakeShared::ResolveSeparatedPairs).
/** Two sides to keep apart. Each side lists bones, and their child bones are included. */
USTRUCT(BlueprintType)
struct FSoftbodySeparatedPair
{
	GENERATED_BODY()

	/** The top bones of one side, such as the left breast. Rigs that split one breast into several bone chains need each chain's top bone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Side A", GetOptions = "GetSkeletonBoneNames"))
	TArray<FName> SideA;

	/** The top bones of the other side, such as the right breast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (DisplayName = "Side B", GetOptions = "GetSkeletonBoneNames"))
	TArray<FName> SideB;
};

// 개발: 베이크 파이프라인 — 고유 텍셀 UV 채널, winding number 체적화 + tet FEM 관성 무름 3방향, 페인트 층, 표면 캐리어(ADR-0003),
// 모양 거리장(ADR-0004). 파이썬 커맨드릿에서 부르도록 정적 UFUNCTION으로 노출한다.
/**
 * Softbody bake steps, callable from Blueprint and Python. The Softbody Authoring Data asset's Run Bake button runs them in order,
 * so call these directly only for scripted or partial bakes.
 */
UCLASS()
class SOFTBODYEDITOR_API USoftbodyBakeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// 개발: 레퍼런스 포즈 기준, 스태틱과 같은 tet FEM 측정 백엔드. 단단한 속 = 뼈대의 본 선(ADR-0026, 피직스 에셋 안 씀): 스킨 웨이트
	// 본 조인트 → 스킨 웨이트 자식 본 조인트, 굵기 = 선에서 살갗까지 × RigidShrink, 선 가까운 격자 점을 고정. 관성 그룹 = 지배 스킨 본을
	// 상위 병합해 MaxInertiaGroups개(AttributeTex2 + InertiaGroupBones + InertiaGroupReach), 런타임 본 가속 구동용.
	/**
	 * Measure bake for a skeletal mesh in its reference pose: adds a UV channel to the mesh, bakes how soft the flesh is for inertia,
	 * and groups the vertices by the bones they jiggle with. The rigid core comes from the skeleton: each skin-weighted bone runs as a
	 * line to its skin-weighted child, as thick as a share of the distance to the skin. Wide bones with no bone of their own in the
	 * skeleton (ribs, pelvis, skull) are not found; paint them dark in Depth through the authoring asset instead.
	 * @param Mesh              Skeletal mesh to bake. It is modified (a UV channel is added).
	 * @param OutputFolder      Content folder for the baked data asset and its textures.
	 * @param CellSizeFrac      Measure cell size as a fraction of the mesh's longest side. Smaller is finer and slower.
	 * @param MaxInertiaGroups  Most bone groups that jiggle separately.
	 * @param RigidShrink       Bone thickness: the share of the distance from the bone line to the skin that counts as bone. Higher
	 *                          means thicker bones, thinner flesh and less jiggle.
	 * @param LayerMaxGap       Layered clothing: how far (cm) a covering surface such as underwear may sit above the flesh and still
	 *                          follow it. 0 = off.
	 * @return The baked data asset, or null if the bake failed.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static USoftbodyMeshData* BakeSkeletalMeshMeasured(USkeletalMesh* Mesh, const FString& OutputFolder,
		float CellSizeFrac = 0.02f, int32 MaxInertiaGroups = 16, float RigidShrink = 0.5f, float LayerMaxGap = 1.0f);

	/**
	 * 본별 shrink 오버라이드 버전 (저작 에셋 경로, UFUNCTION 아님 — 파이썬은 위 일괄 버전 사용).
	 * RigidShrinkPerBone: 본 이름 → 그 본의 뼈 굵기 비율. 값 ≤ 0이면 그 본을 뼈대에서 뺀다(살로만 — 덩어리, ADR-0012): BoneExcludedWithChildren이면 아래 본까지, BoneExcludedAlone이면 그 본만.
	 * 미기재 본은 RigidShrink(기본) 적용.
	 * LayerMaxGap: 겹친 층(ADR-0014) — 속옷처럼 살 위에 덮인 면이 바로 아래 살의 값을 따르게 찾는 거리(cm, 0 = 끔).
	 * SlotMasks: 슬롯 이름 → 마스크. 살의 말랑함(FleshSoftness)이 매핑된 슬롯만 읽는다(ADR-0027) — 무름을 재기 전에 필요한 값이라 여기서 받는다.
	 * FleshReach: 살이 한 덩어리로 흔들리는 두께(cm, 0 = 끔). 살의 말랑함 칠이 있을 때만 쓴다(USoftbodyAuthoringData::FleshReach).
	 * VolumeMesh: 속 메시(ADR-0028, 선택). 살을 채우는 안팎과 뼈의 굵기(살갗까지 거리)를 이 메시에서 읽는다. 닫히지 않은 얇은 막은
	 *   제 메시로는 속이 없다. 베이크 데이터에 남겨 뒤의 캐리어·거리장 굽기도 같은 속을 쓴다.
	 * CoreSlots: 살 슬롯(일지 239, SoftbodyBakeShared::CoreSlotNames). 이 슬롯의 정점을 움직이는 본만 뼈로 친다. nullptr이면 지금 소프트바디
	 *   머티리얼이 붙은 슬롯. 베이크 데이터의 RigidCoreSlots에 남는다.
	 * PendingSoftbodySlots: 같은 Run Bake의 머티리얼 단계가 곧 소프트바디 머티리얼을 입힐 슬롯 이름. 겹친 층이 따를 면(소프트바디 머티리얼이 붙은
	 *   슬롯)에 이것도 센다 — 첫 굽기가 두 번째 굽기와 같은 면을 고르게.
	 */
	static USoftbodyMeshData* BakeSkeletalMeshMeasuredOverrides(USkeletalMesh* Mesh, const FString& OutputFolder,
		float CellSizeFrac, int32 MaxInertiaGroups, float RigidShrink, const TMap<FName, float>& RigidShrinkPerBone,
		float LayerMaxGap = 1.0f, const TMap<FName, FSoftbodyPaintSlotMask>* SlotMasks = nullptr, float FleshReach = 0.f,
		UStaticMesh* VolumeMesh = nullptr, const TSet<FName>* CoreSlots = nullptr, float VolumePreservation = 1.f,
		TConstArrayView<FName> PendingSoftbodySlots = {});

	/**
	 * BakeStaticMeshMeasured에 슬롯 마스크의 살의 말랑함(ADR-0027)과 속 메시(ADR-0028, 선택 — 살을 채우는 안팎을 읽는 메시)를 더한
	 * 버전 (저작 에셋 경로, UFUNCTION 아님).
	 * RigidMinRadiusCells: 구 반지름의 하한(측정 칸 단위). 모양에서 만든 가는 속(Core Placement Center)이 격자 점 사이로 토막 나지 않게
	 *   스켈레탈 본과 같은 0.6을 준다. 직접 놓은 구는 0(그대로).
	 * PendingSoftbodySlots: 위 스켈레탈 버전과 같다(겹친 층이 따를 면에 곧 소프트바디 머티리얼을 받을 슬롯도 센다).
	 */
	static USoftbodyMeshData* BakeStaticMeshMeasuredMasks(UStaticMesh* Mesh, const FString& OutputFolder,
		const TArray<FVector4>& RigidSpheres, float CellSizeFrac, float LayerMaxGap,
		const TMap<FName, FSoftbodyPaintSlotMask>& SlotMasks, float FleshReach, UStaticMesh* VolumeMesh = nullptr,
		float RigidMinRadiusCells = 0.f, float VolumePreservation = 1.f, TConstArrayView<FName> PendingSoftbodySlots = {});

	// 개발: M6 설계 §8.2/8.3. T_<메시>_Paint — R 예약, G 눌리는 정도(캐리어 굽기가 살 두께에 곱함), B 관성량, A 부피 보존. 초기 1,
	// 고정 배치(ADR-0038): 텍스처 R → Depth, B → Volume을 페인트 층에. 심 중복 사본은 평균. 관성량(페인트 B, 칠 아님)은 소프트바디 머티리얼
	// 섹션의 최대 흔들림이 1이 되게 배율(ADR-0005 §3.1).
	/**
	 * Creates or updates the mesh's paint texture from one paint texture shared by every material slot (R Depth, G Jiggle, B Volume).
	 * Flex is not read here. This does not measure the flesh again, so the jiggle follows new paint only after Run Bake. Without a
	 * texture everything is unpainted (white). The texture is read in UV0, so give its UV islands some padding. For meshes whose slots
	 * share UV0 space, use GeneratePaintTextureSlots instead.
	 * @param Data        Baked data to paint.
	 * @param SourceMask  Optional paint texture in UV0.
	 * @return The paint texture, or null on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UTexture2D* GeneratePaintTexture(USoftbodyMeshData* Data, UTexture2D* SourceMask = nullptr);

	// 개발: UV0는 머티리얼 섹션 안에서만 유일하고 슬롯끼리 0~1 공간을 겹쳐 쓰므로 조회는 렌더 섹션(머티리얼 인덱스) 단위.
	/**
	 * Creates or updates the paint texture with one paint entry per material slot, for meshes whose slots share UV0 space (most
	 * characters). Slots without an entry stay unpainted (white). So does a paint set to Paint Texture when the slot has no texture.
	 * Flex is not read here. This does not measure the flesh again, so the jiggle follows new paint only after Run Bake.
	 * @param Data       Baked data to paint.
	 * @param SlotMasks  Material slot index -> that slot's Paint Texture and how each channel is filled.
	 * @return The paint texture, or null on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UTexture2D* GeneratePaintTextureSlots(USoftbodyMeshData* Data, const TMap<int32, FSoftbodyPaintSlotMask>& SlotMasks);

	// 개발: M6 검수 뷰 데이터 소스.
	/**
	 * Logs the mean Jiggle, Depth and Volume paint of each render section. It also logs the section's mean jiggle scale and the share
	 * of the section that jiggles.
	 * @param Data  Baked data to inspect.
	 * @return False if the data or its paint texture cannot be read.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool DumpPaintSectionStats(USoftbodyMeshData* Data);

	// 개발: 진단 v1(임베딩·코어 연결·관성 무름 크기·페인트). 커버리지 부족은 시각 품질 정보이며 검증 실패로 승격하지 않는다.
	/**
	 * Logs bake coverage for the whole mesh and for each render section. It shows how many vertices lie inside the measured flesh
	 * and how many are connected to the rigid core. It also shows how strongly they respond to inertia and their mean paint. Low
	 * coverage is a quality hint, not an error.
	 * @param Data  Baked data to inspect.
	 * @return False if the data has no diagnostics (bake it again).
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool DumpBakeCoverageStats(USoftbodyMeshData* Data);

	// 개발: 값 = 가장 무른 점 1인 비율 × 칠(Paint.B). CSV 열 = 위치, 무름, 칠, 본, 당긴 방향 x·y·z마다 밀리는 벡터 9개, 본 묶음 셋과
	// 첫째·셋째 몫(둘째 = 나머지). _shading = InertiaResponseRows 3줄부터 머티리얼이 읽는 값 그대로.
	/**
	 * Logs how much each part was baked to jiggle: per bone (the bone each vertex is most weighted to), the mean and maximum distance
	 * the flesh moves under 1 g, relative to the softest point and multiplied by the jiggle paint. Multiply by Inertia Amount for cm.
	 * Static meshes get one overall line. A high value where bone should be close (forearm, hand) means there is no rigid core there:
	 * the bone has no skin weights, or its line passes outside the mesh (see the "Rigid core" line in the bake log).
	 * @param Data     Baked data to inspect.
	 * @param CsvPath  Optional file path. If set, writes one line per vertex, plus the triangles to "<name>_triangles.csv" and the
	 *                 jiggle shading values to "<name>_shading.csv".
	 * @return False if the data has no baked inertia or the file cannot be written.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool DumpInertiaSoftness(USoftbodyMeshData* Data, const FString& CsvPath = TEXT(""));

	// 개발: USoftbodyMeshData::InertiaResponseRows의 음영 줄만 — 무름·칠·본 묶음 몫이 면 위에서 바뀌는 기울기를 정점마다 적는다.
	/**
	 * Rebakes only the values that tilt the shading as the flesh jiggles. Bakes and paint updates already do this at the end; call it
	 * only to upgrade data baked before jiggle shading existed, without a full rebake (takes a few seconds).
	 * @param Data  Baked data to update.
	 * @return False if the data has no baked inertia.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool BakeInertiaShading(USoftbodyMeshData* Data);

	// 개발: 관성 그룹 셋과 몫은 AttributeTex2에서 읽는다.
	/**
	 * Checks a skeletal bake: every render vertex must point at a baked vertex, and that vertex's inertia groups and weights must be
	 * valid.
	 * @param Mesh  The baked skeletal mesh.
	 * @param Data  Its baked data.
	 * @return Number of mismatches (0 = OK), or -1 if an input is missing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static int32 VerifySkeletalBake(USkeletalMesh* Mesh, USoftbodyMeshData* Data);

	/**
	 * Quick check of the baked outputs for any mesh type: required textures, per-vertex arrays, the source mesh and the baked UV
	 * channel. It does not judge quality.
	 * @param Data  Baked data to check.
	 * @return Number of failed checks (0 = passed, -1 = no data).
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static int32 VerifyBakeIntegrity(USoftbodyMeshData* Data);

	// 개발: winding number 솔리드화 → 격자 tet(셀 = 바운드 최장변 × CellSizeFrac) → 선형 FEM → RigidSpheres를 Dirichlet 앵커로 하는
	// 관성 무름 3방향 + 임베딩 진단. 겹친 층은 ADR-0014.
	/**
	 * Measure bake for a static mesh: adds a UV channel to the mesh and bakes how soft the flesh is for inertia, held in place by a
	 * rigid core made of spheres.
	 * @param Mesh          Static mesh to bake. It is modified (a UV channel is added).
	 * @param OutputFolder  Content folder for the baked data asset and its textures.
	 * @param RigidSpheres  Rigid core spheres (xyz = local center, w = radius). At least one is needed for inertia.
	 * @param CellSizeFrac  Measure cell size as a fraction of the mesh's longest side. Smaller is finer and slower.
	 * @param LayerMaxGap   Layered clothing: how far (cm) a covering surface may sit above the flesh and still follow it. 0 = off.
	 * @return The baked data asset, or null if the bake failed.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static USoftbodyMeshData* BakeStaticMeshMeasured(UStaticMesh* Mesh, const FString& OutputFolder,
		const TArray<FVector4>& RigidSpheres, float CellSizeFrac = 0.025f, float LayerMaxGap = 1.0f);

	// 개발: ADR-0003 표면 캐리어. 봉투 = winding number 0.5 등위면 → 한 장의 UV 아틀라스. 칸마다 정지 위치·바깥 노멀·살 두께(코어까지
	// × 페인트 G, 칠 0 = 살 없는 칸, ADR-0016 §5)·고스트 연결(스켈레탈은 스킨 웨이트), 렌더 정점마다 캐리어 UV. 차트 = Data->CarrierCharts
	// (ADR-0007): UV0 아일랜드면 메시에 Data->CarrierUVChannel을 쓰고 호스트 머티리얼의 CarrierUV 입력에 잇는다. Resolution = 런타임 RT
	// 크기. MaxThickness: 접촉은 칸마다 제 노멀 방향으로 두께의 1.5배까지 보므로, 상한은 겉에서 그만큼 들어간 속이 있는 것과 같다.
	/**
	 * Bakes the contact map into existing baked data. It wraps the mesh in an outer surface and unfolds it into one texture atlas.
	 * Each cell stores its rest position, the outward normal and the flesh thickness (skin to rigid core, times Depth paint).
	 * Contact on the Softbody component needs it; without a contact map, contact is off. When the map follows the mesh's UV0 islands
	 * (Contact Map Seams), it also writes a UV channel into the mesh and connects it in the Softbody material, so save the mesh and
	 * the material as well.
	 * @param Data              Baked data to add the contact map to.
	 * @param Resolution        Atlas size in texels per side. Higher gives finer dents and uses more memory.
	 * @param CoreSpheres       Thickness core spheres (xyz = local center, w = radius). Empty: skeletal meshes use the skeleton's bone
	 *                          lines, static meshes measure half the distance through the mesh.
	 * @param EnvelopeCellSize  Grid cell size for building the outer surface (local units). 0 = longest side / 256.
	 * @param TargetTriangles   The outer surface is reduced to this many triangles before unfolding.
	 * @param CoreShrink        Skeletal meshes with no CoreSpheres: bone thickness as a share of the distance from bone line to skin.
	 * @param MaxThickness      Cap on flesh thickness (local units). 0 = no cap. Use it on objects whose core does not follow the
	 *                          surface (a box or cushion with a core only at the bottom): otherwise the sides measure too thick and
	 *                          dent along with a press on another face.
	 * @return True on success. The caller saves the packages.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool BakeSurfaceCarrier(USoftbodyMeshData* Data, int32 Resolution,
		const TArray<FVector4>& CoreSpheres, float EnvelopeCellSize = 0.f,
		int32 TargetTriangles = 40000, float CoreShrink = 0.5f, float MaxThickness = 0.f);

	// 개발: 캐리어 베이크와 같은 규칙으로 봉투를 펼치기만 하고 차트를 센다. 차트마다 봉투 면적·표면 칸·여백 포함 점유 칸·주 본,
	// "참여 영역"(소프트바디 머티리얼 슬롯이면서 페인트 G > 0인 정점 근처)과 나머지로 나눠 로그.
	/**
	 * Diagnostics: measures how the contact map atlas would be laid out, without changing the baked data. Unfolds the outer surface
	 * with the same rules as the bake and logs each atlas piece's area, cells used and main bone, split into the area where contact can
	 * show (Softbody material slots with Depth above 0) and the rest.
	 * @param Data              Baked data of a skeletal mesh, with paint.
	 * @param Resolution        Atlas size in texels per side.
	 * @param PruneMargin       If 0 or more, outer-surface triangles farther than this (cm) from any vertex where contact can show are
	 *                          cut away before unfolding. Negative = no cut.
	 * @param EnvelopeCellSize  Grid cell size for building the outer surface (local units). 0 = longest side / 256.
	 * @param TargetTriangles   The outer surface is reduced to this many triangles before unfolding.
	 * @param bUsePaintMask     If false, ignores Depth paint and decides by material slot only.
	 * @return False on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	static bool MeasureCarrierAtlas(USoftbodyMeshData* Data, int32 Resolution = 512, float PruneMargin = -1.f,
		float EnvelopeCellSize = 0.f, int32 TargetTriangles = 40000, bool bUsePaintMask = true);

	// 개발: 이완 묶음(ADR-0013 2단계) 측정 — 묶음마다 고스트 칸이 값을 가져오는 다른 묶음 수.
	/**
	 * Diagnostics: splits the contact map into tiles of TileSize cells and counts, per tile, how many other tiles its seam cells read
	 * from. Logs the distribution, the number of tiles over MaxLinks, and how much of the map those tiles and their border cover.
	 * Does not change the baked data.
	 * @param Data      Baked data with a contact map.
	 * @param TileSize  Tile size in cells.
	 * @param MaxLinks  Tiles linked to more tiles than this are counted separately.
	 * @return False on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	static bool DumpCarrierTileLinks(USoftbodyMeshData* Data, int32 TileSize = 16, int32 MaxLinks = 32);

	// 개발: ADR-0004 모양 거리장(에셋 유저 데이터 USoftbodyColliderField). 부위 = 본마다 그 본 웨이트가 가장 큰 살. VoxelSize 0 = 바운드
	// 최장변 / 288로 굽고 부위 두께에 맞춰 1~3배 칸으로 저장(몸통은 최장변 / 96). PartOverlap(일지 71·73): 부위가 마디 절단면에서 끝나면
	// SurfaceEdgeTrim이 그 면을 콜라이더 가장자리로 읽어 마디마다 고랑·굽힌 마디 바깥 쐐기 틈 → 겹치면 메워짐, 눌리는 쪽 다듬기 폭
	// (SurfaceEdgeTrim × 캐리어 칸)의 두 배 이상이면 고랑이 사라진다. VolumeMesh는 ADR-0028(봉투만 이 메시에서, 주인 본은 Mesh에서).
	/**
	 * Bakes a shape field: a copy of the mesh's real shape that it presses Softbody flesh with, instead of its simple collision. It is
	 * stored on the mesh asset. A skeletal mesh gets one part per bone, which follows that bone at runtime; a static mesh is one part.
	 * Every part presses other Softbody meshes. Baked on a Softbody character's own mesh, the parts under PresserBones (such as the
	 * hands) also press its own flesh (self contact).
	 * @param Mesh          Static or skeletal mesh.
	 * @param VoxelSize     Cell size (local units). 0 = automatic: fine for thin parts such as fingers, coarser for the torso. If you
	 *                      set it, about a quarter of the thinnest pressing part (a finger's thickness) is enough.
	 * @param PresserBones  Skeletal only: bones (with their children) that also press the mesh's own flesh, such as Hand_L and Hand_R
	 *                      (Self Contact Bones on the authoring asset). On its own body only these press, so soft parts such as elbows
	 *                      and waist do not push each other. Empty = every part presses the mesh itself (for a mesh that is all
	 *                      presser, such as a hand).
	 * @param PartOverlap   Skeletal only: how far (cm) each part reaches into its parent and child bones. The overlap closes grooves
	 *                      along joints and gaps on the outside of bent joints; side-by-side bones (the next finger) are not included.
	 *                      0 = no overlap.
	 * @param VolumeMesh    Optional Volume Mesh: the shape is taken from this filled mesh (for thin, open membranes). Which bone owns
	 *                      each part still comes from Mesh.
	 * @return The baked shape field. The caller saves the mesh package. The whole mesh is baked; the authoring asset's Shape Field Slots
	 *         bake only some material slots.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static USoftbodyColliderField* BakeColliderField(UObject* Mesh, float VoxelSize, const TArray<FName>& PresserBones,
		float PartOverlap = 1.f, UStaticMesh* VolumeMesh = nullptr);

	// 개발: 슬롯 고르기(저작 에셋 Shape Field Slots). UHT가 배열 인자 기본값을 받지 않아 블루프린트용과 따로 둔다.
	/** Same as above, baking only the triangles of MaterialSlots (slot or imported slot names). Empty = the whole mesh. */
	static USoftbodyColliderField* BakeColliderField(UObject* Mesh, float VoxelSize, const TArray<FName>& PresserBones,
		float PartOverlap, UStaticMesh* VolumeMesh, const TArray<FName>& MaterialSlots);

	// 개발: ADR-0020 잠들기 상자 — 본마다 "그 본이 움직이는 칸 + 접촉 광선 끝"의 기본 포즈 상자, 타깃 스켈레탈 메시에 모양 거리장이
	// 있으면 자기 접촉 부위마다 "누를 수 있는 칸" 상자. 저작 에셋의 모양 거리장 단계가 다시 부른다.
	/**
	 * Bakes per-bone wake-up boxes from the contact map, used for sleeping: at runtime, contact does no GPU work while no collider can
	 * reach these boxes. If the target mesh has a shape field, self-contact boxes are baked too. Contact map bakes call this at the
	 * end; call it again if you bake a shape field afterwards.
	 * @param Data  Baked data with a contact map.
	 * @return False if the data has no contact map.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool BakeWakeBounds(USoftbodyMeshData* Data);

	// 개발: 부위 나누기(셰이더 "부위 나누기") — 칸 스킨 웨이트(상위 4본)에서 칸마다 두 묶음 몫을 G16 한 장(CarrierSeparationTex)에. 쌍들을 두 묶음으로
	// 나누는 것은 SoftbodyBakeShared::ResolveSeparatedPairs(미리보기와 같은 함수). 저작 에셋의 접촉 지도 단계가 캐리어 굽기 뒤에 부른다. 쌍이 없으면
	// 텍스처를 뗀다.
	/**
	 * Bakes the Separated Pairs into the contact map: for each cell, how much of it belongs to each side (bone weights, child bones
	 * included). The authoring asset calls this after the contact map bake. No usable pair removes the separation.
	 * @param Data   Baked data with a contact map of a skeletal mesh.
	 * @param Pairs  The pairs to keep apart.
	 * @return False if the contact map could not be read.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool BakeSeparatedParts(USoftbodyMeshData* Data, const TArray<FSoftbodySeparatedPair>& Pairs);

	// 개발: ADR-0031 솟음 랜드마크 거리표 — 솟음 거친 격자의 살 그래프(이웃 + 이음매, 살 없는 칸은 벽)에서 가장 먼 점 뽑기로 랜드마크를
	// 고르고 칸마다 거리(다익스트라)를 G16 텍스처에 적는다. 캐리어 굽기가 끝에 부른다. Tools/bake_bulge_landmarks.py가 옛 데이터에 붙인다.
	/**
	 * Bakes the surface distance table that the bulge (flesh pushed aside around a dent) and the drag spread over. Without it both
	 * stay off. Contact map bakes call this at the end, so run it only on data baked before the table existed.
	 * @param Data            Baked data with a contact map.
	 * @param LandmarkCount   How many reference points on the surface the distances are measured to (16 to 128). More is more
	 *                        accurate and uses more memory.
	 * @return False if the data has no contact map or no flesh, its textures cannot be read, or the table is too tall for one texture.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool BakeBulgeLandmarks(USoftbodyMeshData* Data, int32 LandmarkCount = 128);

	/** BakeBulgeLandmarks와 같다. bOutTooLarge = 표가 텍스처 한 장의 한도를 넘어 굽지 않았다(캐리어 굽기가 이때 굽기를 실패로 돌린다). */
	static bool BakeBulgeLandmarksSized(USoftbodyMeshData* Data, int32 LandmarkCount, bool& bOutTooLarge);

	// 개발: 저작 에셋의 Core Placement(Center·Bottom)가 놓을 속 구를 굽지 않고 본다(일지 231). 굽기와 같은 함수다.
	/**
	 * Returns the rigid core spheres that Core Placement Center (bFloor false) or Bottom (bFloor true) would place in a static mesh,
	 * without baking. Each entry is a center in mesh space (cm) with the radius in w.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	static TArray<FVector4> PreviewStaticCore(UStaticMesh* Mesh, bool bFloor, float CellSizeFrac = 0.015f, float Thickness = 0.45f);

	// 개발: 거리표 텍스처가 실제로 어떤 형식으로 올라갔는지(G16이어야 한다)와 첫 줄 몇 값을 로그로 확인한다(Tools/bake_bulge_landmarks.py 뒤 점검).
	/**
	 * Logs how the bulge distance table of the data is stored (pixel format, size, number of reference points) and a few of its values.
	 * @return The same summary as a string; empty if the data has no table.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	static FString DescribeBulgeTable(USoftbodyMeshData* Data);

	// 개발: Tools/bake_pivot_r3_validation.py. 내부 셸은 노멀이 안쪽 → winding number가 공동으로 인식.
	/**
	 * Test mesh for inertia checks: a sphere, hollow when InnerRadius > 0 (an inner shell facing inward). The rigid core is not
	 * geometry; pass it as RigidSpheres to BakeStaticMeshMeasured.
	 * @param AssetPath    Where to create the static mesh.
	 * @param OuterRadius  Outer radius (cm).
	 * @param InnerRadius  Radius of the hollow inside (cm). 0 = solid.
	 * @param Resolution   Segments around the sphere.
	 * @return The created static mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UStaticMesh* CreateHollowSphereTestMesh(const FString& AssetPath,
		float OuterRadius = 60.f, float InnerRadius = 45.f, int32 Resolution = 64);

	/**
	 * Test mesh for inertia checks: a closed ellipsoid with its own radius (cm) along local X, Y and Z. The same radii and Resolution
	 * always give the same vertices, so you can move the rigid core spheres between bakes without changing the surface.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UStaticMesh* CreateEllipsoidTestMesh(const FString& AssetPath,
		FVector Radii = FVector(90.f, 55.f, 45.f), int32 Resolution = 32);

	/**
	 * Dense test sphere for contact checks: a cube inflated into a sphere, so triangles are even and there are no poles. It has about
	 * 6 x EdgeVertices² vertices. Matching the vertex spacing to the contact map cell size shows dents almost exactly as computed.
	 * @param AssetPath     Where to create the static mesh.
	 * @param Radius        Sphere radius (cm).
	 * @param EdgeVertices  Vertices along one cube edge.
	 * @return The created static mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UStaticMesh* CreateUniformSphereTestMesh(const FString& AssetPath, float Radius = 60.f, int32 EdgeVertices = 64);

	/**
	 * Test mesh for inertia checks: two round lobes along local X joined by a smooth narrow waist. A rigid core sphere can be placed in
	 * either lobe.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UStaticMesh* CreateDumbbellTestMesh(const FString& AssetPath,
		float HalfLength = 90.f, float LobeRadius = 45.f, float NeckRadius = 18.f,
		int32 Resolution = 32);

	// 개발: ADR-0005. 배열/텍스처 정합성, 솔버 잔차, 앵커 오차, 도달 거리, 세 방향 공통 정규화(최대 1), 당기는 방향으로 밀리는지.
	/**
	 * Checks the baked flesh softness for inertia: array and texture sizes, solver accuracy and how well the rigid core holds. It also
	 * checks that the flesh moves the way it is pulled.
	 * @param Data  Baked data to check.
	 * @return Number of failed checks (0 = passed, -1 = no data).
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static int32 VerifyInertiaResponseBake(USoftbodyMeshData* Data);

	// 개발: MaterialEditingLibrary.recompile_material은 비동기라 커널 HLSL 오류를 못 잡는다 → Tools/verify_softbody_shaders.py가 쓴다.
	/**
	 * Recompiles the material's shaders (the parent's, for an instance) for the current platform and waits for them to finish.
	 * @param Material  Material or material instance to compile.
	 * @return Compile errors; empty on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	static TArray<FString> CompileMaterialShadersSync(UMaterialInterface* Material);

	// 개발: 표면 장 v5 겹친 층 진단. 정점 변위는 커널과 같은 식(캐리어 UV에서 변위 텍스처 쌍선형 × 부착, 눌리는 정도 칠 0 정점은 0)으로
	// CPU에서 다시 계산. 짝을 "같은 캐리어 차트" / "차트 경계 양쪽"으로 나눠 비교하고, 안쪽 층에서 차트 경계를 건너는 변의 변위 차도 잰다.
	/**
	 * Layered clothing diagnostics; call it in PIE while something is pressing. Pairs each inner-layer vertex (such as skin) with the
	 * nearest point of the outer layer above it (such as a bra) and logs how differently contact moves the two this frame, including
	 * pairs where the skin pokes through. Assumes the reference pose (no animation playing).
	 * @param Softbody     Softbody component with contact running.
	 * @param InnerSlot    Material slot of the inner layer.
	 * @param OuterSlot    Material slot of the outer layer.
	 * @param CenterWorld  Center of the area to inspect (world).
	 * @param Radius       Radius of the area to inspect (cm).
	 * @param MaxGap       Pairs farther apart than this (cm) are not treated as layered.
	 * @param CsvPath      Optional file path; writes one line per pair.
	 * @return False if the component or its data is not ready.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Diagnostics")
	static bool DumpLayerGap(class USoftbodyComponent* Softbody, FName InnerSlot, FName OuterSlot,
		FVector CenterWorld, float Radius = 8.f, float MaxGap = 0.5f, const FString& CsvPath = TEXT(""));

	// 개발: 엔진 자동 생성은 20 cm보다 짧은 본을 부모에 합치고, 헤드리스 커맨드릿에서는 결과 판정이 실패해 쓰지 않는다. 본마다 가장 큰
	// 웨이트로 붙은 정점의 주축(PCA)으로 감싼다.
	/**
	 * For presser meshes such as a hand: creates a physics asset with one body per bone and assigns it to the mesh. Bones with a
	 * parent (finger joints) get a capsule fitted to their vertices; the root bone (palm) gets a box. Not needed for the mesh you bake
	 * as Softbody; that bake reads its rigid core from the skeleton.
	 * @param Mesh         Skeletal mesh.
	 * @param AssetPath    Where to create the physics asset.
	 * @param MinVertices  Bones with fewer vertices than this get no body.
	 * @return The created physics asset, or null on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static UPhysicsAsset* CreateBonePhysicsAsset(USkeletalMesh* Mesh, const FString& AssetPath, int32 MinVertices = 8);

	// 개발: ADR-0006 정점 사이 함몰. 메시: 나나이트 켬, UV 보간 끔(텍셀 번호 UV가 먼 거리 단순화에서 섞이지 않게) + Max Edge Length Factor 4(0일 때만, 무늬 톱니 막기), 텍셀 UV 채널은 UV0~3.
	// 슬롯 머티리얼: "Nanite"(스켈레탈이면 "Skeletal Mesh"도) 사용 플래그. 소프트바디 슬롯: 테셀레이션, 변위 크기·중심, 최대 WPO,
	// MF_SoftbodyContactNormal Displacement 출력 → Displacement 핀(파이썬 열거형에 없는 핀). 반투명 슬롯이면 메시 전체가 나나이트를 잃는다.
	/**
	 * Sets the mesh up for Nanite tessellation so dents can be narrower than the spacing between vertices: the original vertices move
	 * as before, and the points tessellation adds inside each triangle read the contact directly. Turns on Nanite for the mesh, the
	 * Nanite usage flag on every slot material, and tessellation and displacement on the Softbody slots. Fails if any slot is
	 * translucent. Warns about morph targets (Nanite skinning ignores them) and hard-edged normals inside Softbody slots (they open
	 * gaps).
	 * @param Data                    Baked data whose mesh to set up. Its baked UV channel must be UV0 to UV3.
	 * @param Magnitude               Size of the displacement range (cm).
	 * @param Center                  Where zero sits in that range: displacement runs from Center x Magnitude inward to
	 *                                (1 - Center) x Magnitude outward. Values beyond are clipped.
	 * @param MaxWorldPositionOffset  How far (cm) Nanite culling widens the bounds for vertex motion. Motion is also clipped to this
	 *                                per axis, so keep it above the largest contact and jiggle motion.
	 * @return True on success. The caller saves the packages.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Bake")
	static bool ConfigureNaniteTessellation(USoftbodyMeshData* Data, float Magnitude = 2.f, float Center = 0.75f,
		float MaxWorldPositionOffset = 30.f);

	// --- 다른 흔들림 솔버와 함께 쓰기 (SoftbodyClassicPhysics.cpp) ---

	/**
	 * Creates body colliders for anim nodes that jiggle hair or breasts (such as KawaiiPhysics' Physics Asset for Limits) at AssetPath.
	 * It copies the mesh's physics asset, keeps only the KeepBones bodies (constraints are removed), and for each shape:
	 *  - scales its radius (or box size) by RadiusScale. Below 1 the shape sits inside the flesh, so a light touch only dents the skin
	 *    and the jiggling bone is pushed only by a deeper press.
	 *  - shrinks it until the ClearBones (jiggling bones) are at least Clearance outside it in the reference pose, so hair does not
	 *    puff out just from standing. Shapes smaller than 0.5 cm are removed.
	 * The amounts shrunk are logged. The caller saves the package.
	 * @param Mesh         Skeletal mesh whose physics asset to copy.
	 * @param AssetPath    Where to create the new physics asset.
	 * @param KeepBones    Bones whose bodies to keep.
	 * @param ClearBones   Jiggling bones that must stay clear of the shapes.
	 * @param Clearance    Required distance (cm), about the jiggling bones' collision radius.
	 * @param RadiusScale  Scale for every shape's radius or box size.
	 * @return The created physics asset, or null on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Physics")
	static UPhysicsAsset* CreateLimitsPhysicsAsset(USkeletalMesh* Mesh, const FString& AssetPath, const TArray<FName>& KeepBones,
		const TArray<FName>& ClearBones, float Clearance = 1.f, float RadiusScale = 1.f);

	/**
	 * Creates a post-process Animation Blueprint at AssetPath and assigns it to the mesh. It runs last, whatever animation plays
	 * (Sequencer, an Animation Blueprint or a single animation). The pose goes input -> NodeClasses[0] -> [1] -> ... -> output.
	 * The caller saves the package.
	 * @param Mesh          Skeletal mesh to assign it to.
	 * @param AssetPath     Where to create the Animation Blueprint.
	 * @param NodeClasses   Anim graph node class paths, such as "/Script/KawaiiPhysicsEd.AnimGraphNode_KawaiiPhysics".
	 * @param NodeSettings  Per node, the text for its node struct (property "Node"), in the format the editor uses when you copy a
	 *                      node: "(RootBone=(BoneName=\"Hair\"),Gravity=(X=0,Y=0,Z=-980))". Values left out keep their defaults; an
	 *                      unknown name fails.
	 * @return The created Animation Blueprint, or null on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Physics")
	static UObject* CreatePostProcessAnimBlueprint(USkeletalMesh* Mesh, const FString& AssetPath, const TArray<FString>& NodeClasses,
		const TArray<FString>& NodeSettings);

};
