// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "SoftbodyColliderShapes.h"
#include "UObject/ObjectKey.h"
#include "SoftbodyComponent.generated.h"

struct FSoftbodyWakeBoxes;

class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class USoftbodyColliderField;
class USoftbodySurfaceField;
class UMaterialInterface;
class UMeshComponent;
class USkinnedMeshComponent;
class USoftbodyMeshData;
class UTexture2D;
class USoftbodyGrabberComponent;
struct FSoftbodySurfaceFieldInput;
enum class ESoftbodyGrabEndReason : uint8;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSoftbodyUpdatedDelegate);

// 개발: 접촉 단계 고르기(일지 222). Basic은 끌림·접촉 압력(솟음·긴 몫) 패스를 돌리지 않는다 — 재료값은 그대로 두고 단계만 뺀다.
/** Which contact effects run. Basic drops the effects you only notice up close, to save GPU time on meshes seen from farther away. */
UENUM(BlueprintType)
enum class ESoftbodyContactDetail : uint8
{
	/**
	 * Every contact effect: dents, lumps pushed aside and bulges around a press, plus skin dragged by a moving hand and smoothed dent
	 * edges (Edge Smoothing). On Nanite meshes the dent wall along a moving hand's edge also jitters less.
	 */
	Full,
	/** Dents and lumps pushed aside only. No bulge, no skin drag, no Edge Smoothing and no Flex paint. */
	Basic,
};

/**
 * 관성 구동점 하나(스켈레탈 본 그룹 또는 스태틱 메시 전체)의 움직임 기록과 흔들림 상태 (ADR-0005).
 * Load 0~2 = 기본 자세 축에서 살이 받는 가속·중력 변화 (g 단위),
 * Load 3~5 = 회전 가속 (1/cm — 정점에서 구동점까지 거리를 곱해 g 단위가 된다).
 */
struct FSoftbodyInertiaDriver
{
	FVector PrevLoc = FVector::ZeroVector;
	FVector PrevVel = FVector::ZeroVector;
	FQuat PrevRotation = FQuat::Identity;
	FVector PrevAngularVelocity = FVector::ZeroVector;
	/** 컴포넌트 공간 본 포즈 기록 (스켈레탈 — 애니메이션 몫을 액터 몫과 나누는 데 쓴다). */
	FVector PrevPoseLoc = FVector::ZeroVector;
	FVector PrevPoseVel = FVector::ZeroVector;
	FQuat PrevPoseRotation = FQuat::Identity;
	FVector PrevPoseAngularVelocity = FVector::ZeroVector;
	/**
	 * 속도 기록이 맞는가. 처음·히치·순간이동 뒤에는 자리만 기록했으니 다음 틱은 속도만 재고 가속은 0으로 둔다 — 속도 기록을 0으로 두고
	 * 바로 가속을 재면 움직이던 중이었을 때 (속도 / 프레임 시간)만큼의 가짜 가속이 들어갔다(600 cm/s면 약 38 g).
	 */
	bool bHasVelocity = false;
	bool bHasPoseVelocity = false;
	float Load[6] = {};
	float LoadVel[6] = {};
	/** 이번 틱의 기본 자세→월드 회전과 월드 구동점 (브릿지 업로드용). */
	FQuat ReferenceToWorld = FQuat::Identity;
	FVector Pivot = FVector::ZeroVector;
	/** 이 구동점의 흔들림 크기(cm/g, InertiaAmount × 그룹 배율)와 회전 몫에 곱할 도달 거리(월드 cm). 브릿지 업로드용. */
	float Amount = 0.f;
	float Reach = 0.f;
	/** 이 묶음의 처짐 몫(기준 살 = 1, 빠르기용)과 가장 많이 움직이는 정점의 몫(흔들림 상한용). 옛 굽기는 둘 다 1이다. */
	float Sag = 1.f;
	float SagMax = 1.f;

	void ClearLoad()
	{
		FMemory::Memzero(Load, sizeof(Load));
		FMemory::Memzero(LoadVel, sizeof(LoadVel));
	}
};

// 개발: 관성 그룹(본) 하나의 덮어쓰기(ADR-0015). 켜지 않은 항목은 컴포넌트 값을 쓴다.
/**
 * Jiggle settings for one body part (inertia group), for example a slow chest and a quick belly on the same character. Settings
 * that are not ticked use the component's values.
 */
USTRUCT(BlueprintType)
struct FSoftbodyInertiaGroupOverride
{
	GENERATED_BODY()

	// 개발: 빠르기도 크기에서 나온다(ADR-0015 §2.5, InertiaAmount 설명의 표).
	/** Multiplies Inertia Amount for this part. It also changes the wobble speed: a part that moves more wobbles more slowly. 0 = this part does not jiggle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (ClampMin = "0.0"))
	float AmountScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (InlineEditConditionToggle))
	bool bOverrideDamping = false;

	/**
	 * For how springy this part is: how many times its wobble swings past before it stops. 0.25 to 0.4 gives one or two bounces.
	 * Near 0.1 it keeps swinging about seven times.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody",
		meta = (EditCondition = "bOverrideDamping", ClampMin = "0.01", ClampMax = "0.99"))
	float Damping = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody", meta = (InlineEditConditionToggle))
	bool bOverrideGravityScale = false;

	/**
	 * How strongly gravity pulls this part (1 = real gravity, 0 = none). Above 1 it also hangs lower while upright. Replaces Gravity
	 * Scale for this part. Parts on the skeleton such as the torso sag only when they are given a value here.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody",
		meta = (EditCondition = "bOverrideGravityScale", ClampMin = "0.0"))
	float GravityScale = 1.f;
};

/**
 * 들어온 면 표지(일지 313)의 콜라이더 고정 번호: (컴포넌트, INDEX_NONE — 바깥 콜라이더 하나) 또는 (자기 몸, 부위 번호) → 0~126. 표면 장 GPU에 올리는 콜라이더 차례는
 * 프레임마다 바뀌므로 칸에 남은 표지가 어느 콜라이더 것인지 이 번호로 잇는다. 새 콜라이더는 쓰이지 않는 번호를 돌아가며 받아(지운 번호를 바로
 * 다시 주지 않는다), 번호가 다 쓰이면 이번 틱에 보지 않은 것 가운데 가장 오래 안 보인 번호를 넘겨받는다. 그것도 없으면 번호 없음(127 — 그 콜라이더는
 * 표지 없이 지금 규칙으로 잰다).
 */
struct FSoftbodyColliderLabelIds
{
	static constexpr uint32 NumIds = 127;
	static constexpr uint32 NoId = 127;          // 셰이더 SB_ID_NONE
	static constexpr uint32 NewCollider = 0x80;  // 셰이더 SB_ID_NEW: 지난 갱신에 없던 콜라이더(지난 자세를 모른다)

	/** 틱마다 번호를 고르기 전에 한 번. */
	void BeginTick() { ++Tick; }
	uint32 Get(const TPair<FObjectKey, int32>& Key);

private:
	struct FEntry
	{
		uint32 Id = 0;
		uint64 LastTick = 0;
	};
	TMap<TPair<FObjectKey, int32>, FEntry> Entries;
	TArray<TPair<FObjectKey, int32>> Owners;   // 번호마다 지금 주인(쓰이지 않으면 빈 키)
	TBitArray<> Used;
	uint64 Tick = 0;
	uint32 Next = 0;
};

// 개발: 런타임 오케스트레이터. 접촉 = 콜라이더를 모아 표면 장(ADR-0003, GPU)을 갱신하고 머티리얼이 캐리어 UV로 읽는다.
// 관성 = 구동점(본 그룹 또는 메시 전체)의 흔들림을 CPU 해석해로 갱신해 관성 브릿지 텍스처로 올린다.
/**
 * Makes a mesh soft: it dents, bulges and drags where hands and objects press it, and jiggles when it moves.
 * Add one per mesh and set Mesh Data to the Baked Data of that mesh's Softbody Authoring Data.
 */
UCLASS(ClassGroup = (Softbody), meta = (BlueprintSpawnableComponent))
class SOFTBODY_API USoftbodyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	/**
	 * 관성 브릿지(머티리얼 파라미터 ContactData, PF_FloatRGBA, 가로 BridgeWidth × 세로 BridgeRows 텍셀):
	 * 행0 텍셀0.A = 스켈레탈 관성 그룹 수, 행0 텍셀4~7 = 스태틱 메시의 구동점,
	 * 행1~8 = 스켈레탈 관성 그룹 — 그룹 g는 행1부터 이어 센 4g~4g+3번 텍셀.
	 * 구동점 4텍셀: 가속·중력 xyz + 회전 x, 회전 yz, 기본 자세→월드 회전, 액터 상대 구동점 위치(w = 메시 스케일).
	 * 흔들림 값은 cm로(InertiaAmount를 곱해) 올린다. 셰이더 쪽은 SoftbodyKernel.ush의 SB_LoadInertiaEntry.
	 */
	static constexpr int32 BridgeWidth = 8;
	static constexpr int32 BridgeRows = 9;
	/** 살의 무름 슬라이스 수: 체적력 +X, +Y, +Z (ADR-0005). */
	static constexpr int32 NumInertiaResponses = 3;
	/** 관성 그룹 용량 (행 1~8, 그룹당 4텍셀). */
	static constexpr int32 MaxInertiaGroups = 16;

	USoftbodyComponent();

	// 개발: 접촉과 관성을 합친 최종 변위의 폭주 가드(머티리얼 SB_CAP_SOFT_START 0.7부터 부드럽게 붙는다).
	// 개발: 상한이 휘기 시작하는 비율은 조절값(Soft Limit Start)이었다가 0.7로 고정 — SoftbodyInertiaRuntime::LimitEaseStart(일지 227).
	// 개발: 접촉 깊이 상한(살 두께·Max Contact Depth)은 이 비율을 쓰지 않는다 — 상한까지 그대로 그리고 그 너머에서 휜다(셰이더 SB_PressLimit, 일지 295).
	/**
	 * The farthest any vertex may move from where it was modeled, in cm, with dents and jiggle added together.
	 * Movement is unchanged up to 70% of this limit, then eases into it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Limits", meta = (ClampMin = "0.0", Units = "cm"))
	float MaxDisplacement = 50.f;

	// 개발: 비우면 MeshData의 소스 메시와 같은 소유 액터의 컴포넌트, 없으면 첫 SoftbodyIgnore 비지정 메시. 찾은 메시는 GetTargetMeshComponent.
	/** The mesh this component deforms. Empty = the mesh on this actor that Mesh Data was baked from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody")
	TObjectPtr<UMeshComponent> TargetMeshComponent;

	/** The baked data for the target mesh: the Baked Data of its Softbody Authoring Data asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody")
	TObjectPtr<USoftbodyMeshData> MeshData;

	// 개발: 끄면 표면 장을 만들지 않는다(GPU 패스·렌더 타깃·콜라이더 찾기 없음). 덩어리 밀기도 접촉이라 같이 꺼진다.
	/**
	 * Lets hands and objects dent, bulge and drag the surface. Off = all contact cost goes away and only the jiggle remains.
	 * Lumps are no longer pushed either. Nanite tessellation is a mesh setting and is not affected.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact", meta = (DisplayName = "Enable Contact"))
	bool bEnableContact = true;

	// 개발: Basic이면 Update가 끌림 마찰을 0으로, 접촉 압력·긴 몫을 끈 채 보낸다 — 그 패스가 돌지 않는다(일지 222, ADR-0037 §3.6). 기본값 Basic(일지 223).
	// 개발: 가장자리 다듬기(Edge Smoothing)도 Basic에서 0(일지 224).
	// 개발: 측정 — 일지 222(같은 멈춘 자세) 손이 쥔 블롭 −47~61%·시험 캐릭터 두 장 −18~22%, 일지 223(시연 프로파일 레벨 장면마다 10초씩) −23~78%·전체 −38%.
	// 개발: 일지 283(접촉 압력 풀이를 빠르게 한 뒤, 시연 프로파일링 레벨 600프레임 창) Full ÷ Basic 전체 1.89배, 장면마다 1.3~3.3배.
	// 개발: 일지 284(긴 몫 모으기 걸음 나누기 뒤, 같은 방법) 전체 1.60배, 장면마다 1.3~2.7배.
	/**
	 * Which contact effects run. Basic runs only dents and pushed lumps to save GPU time on meshes not seen up close.
	 * Full adds bulges, skin drag (with a pressing hand's Lump Grip), Edge Smoothing and Flex paint.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact",
		meta = (DisplayName = "Contact Detail", EditCondition = "bEnableContact"))
	ESoftbodyContactDetail ContactDetail = ESoftbodyContactDetail::Basic;

	// 개발(ADR-0037 §3.4): 겉껍질 장력 s. 굽기는 이 값을 쓰지 않는다. 칸별 칠은 일지 298(ADR-0038 §3.5)에 없어졌다 —
	// 메시 전체의 값 하나다. 실행 중 L_T = 4 s² × L0(고정 길이 SoftbodyContactResponse::ReferenceLength 3, 메시 로컬이라 Depth 칠이
	// 이 값의 뜻을 바꾸지 않는다). 바꾸면 거친 칸 표를 그 갱신에 다시 만든다(텍스처는 다시 쓴다).
	// 상한 2: L0이 고정되면서 살 6 cm에서는 1이 예전 최대 장력의 절반에만 닿는다. 2면 표가 덮는 τ 8까지 닿는다.
	/**
	 * Strength of a tight skin over the flesh, such as leather. 0 = bare flesh or jelly.
	 * A tighter skin makes dents rounder and wider and blurs what presses. It covers the whole mesh; Flex paint is for part of it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Flesh", meta = (DisplayName = "Skin Tension", ClampMin = "0.0", ClampMax = "2.0"))
	float SkinTension = 0.f;

	// 개발: 실행 중 더하는 값. 칸의 v(저작 에셋 값 × Volume 칠)에 더하고 0~1로 자른다(셰이더
	// SB_VolumeShifted). v는 기준 누름(살 L0, 깊이 L0/3인 손)에서 살 대비 둘레로 솟는 부피 몫이라 같은 폭이면 어느 칠에서나 같은 만큼 달라 보인다
	// (전의 배수는 부피 비율 K/G에 곱해 칠 안 한 살에서 1 위가 모두 죽어 있었다). 끝에서 잘리는 자리는 플레이 시작에 알린다(LogUnusedValues).
	// 함몰·솟음만 바뀌고 덩어리 모양(측정 굽기의 푸아송 비)은 구운 저작 에셋 값 그대로다 — 맞추려면 값을 저작 에셋에 옮기고 Run Bake.
	/**
	 * For trying while playing how much flesh rises around a dent. It adds to the Volume of every area: -1 = all foam, +1 = all flesh.
	 * A whole pushed lump still spreads by the baked value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Flesh",
		meta = (DisplayName = "Volume Offset", ClampMin = "-1.0", ClampMax = "1.0"))
	float VolumeOffset = 0.f;

	// 개발: 실행 중 칸의 Flex 칠 S에 더하고 0~1로 자른다(칠 안 한 살은 S = 1).
	// 굽힘 길이 0.6·L0·√((1 − S)/S)는 L0 두께 살에서 손 옆 띠가 같이 가라앉는 몫이 약 1 − S가 되게 맞춰, 같은 폭이면 어느 칠에서나 같은 만큼 달라
	// 보인다. 음수면 칠 안 한 살도 굽는다. 칠한 조각의 가장 큰 칠 + 값이 0 이하면 그 조각은 강체 판이다. 끝에서 잘리는 칠은 플레이 시작에 알린다.
	// Contact Detail Full에서만 듣는다.
	// 출시(일지 351): 완성되지 않은 기능 — 폼(Volume 1 아래) 위 굽힘이 틀린다. 툴팁·문서에는 내부 까닭 없이
	// "불안정, 주의"만 적는다. 근본 고침(겉 밑 살 줄)은 출시 뒤.
	/**
	 * For trying while playing how stiff the skin painted in Flex is. Not finished yet and can be unstable, so use it with care.
	 * Adds to the Flex paint: -1 turns painted pieces into rigid plates, +1 makes everything bare skin. Needs Contact Detail Full.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Flesh",
		meta = (DisplayName = "Flex Offset", ClampMin = "-1.0", ClampMax = "1.0"))
	float FlexOffset = 0.f;

	// 개발: 일지 295(관통 계약, 원칙 P06) — 이 깊이까지는 누름을 그대로 그리고, 그 너머는 휘어 1.3배에 붙는다(전에는 70 %부터 휘어 이 깊이에서
	// 0.075배가 덜 그려졌다). 살 두께에도 같은 꼴이 걸린다.
	// 개발: 하위 묶음(Softbody|...|...) 안의 AdvancedDisplay 값은 디테일 패널에 펼칠 곳이 없어 보이지 않는다(일지 319) — 이 파일의
	// Softbody 하위 묶음 값에는 AdvancedDisplay를 쓰지 않는다.
	/**
	 * How deep a collider can push the surface, in cm. Pushes up to this depth are exact, and deeper ones ease out and stop at 1.3 times it.
	 * For collision that sits inside the visible surface, such as a body capsule under the skin, set it to that gap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Dent", meta = (ClampMin = "0.0", Units = "cm"))
	float MaxContactDepth = 15.f;

	// 개발: 단위는 캐리어 칸 수(칸 크기 = CarrierTexelSize, capture.txt). 비 나나이트 시험 캐릭터 0.434 cm 칸이면 0.3~0.5.
	// 개발: 벽 몫(나오는 깊이 − 콜라이더 안을 지난 깊이)만 줄이고, 다듬기가 걸리는 칸은 그 깊이를 다시 찾는다(SB_TrimmedDepth·SB_FieldRayDeepest,
	// 일지 224). Basic이면 Update가 0을 보낸다.
	// 개발: 기본값 1.5 → 0.5(일지 227) — 시연 손(손가락 약 0.8 cm, 칸 0.159 cm)은 1.5부터 손가락이 통째로 다듬기 띠(2w) 안에 들어 잡음 칸이
	// 1.3%, 0.5면 0.4%(ES 0이 0.2%). 나나이트 메시는 벽을 칸 사이에 세우므로(ADR-0029) 작은 값으로 충분하다.
	// 개발: 기본 항목이다(일지 316) — Dent 묶음의 값이 모두 고급이면 디테일 패널에 묶음째 안 보인다.
	/**
	 * Smooths the steep dent wall along a collider's edge, in contact map cells. Flat and light presses keep their full depth.
	 * Objects thinner than twice this width press less deeply. 0 = off. Needs Contact Detail Full.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Dent",
		meta = (DisplayName = "Edge Smoothing", EditCondition = "ContactDetail == ESoftbodyContactDetail::Full", ClampMin = "0.0", UIMax = "3.0"))
	float SurfaceEdgeTrim = 0.5f;

	// 개발: 8방향 지평 앙각으로 하늘이 가려지는 몫(일지 98).
	// 개발: 머티리얼이 Occlusion을 안 쓰면 화면에 없다. 0이면 지평 탐색을 건너뛴다. 기본 항목이다(일지 316 — Dent 묶음이 보이게).
	/**
	 * How dark deep, narrow dents get (0 to 1), such as fingertip marks and the gaps between fingers.
	 * Needs a material that multiplies its base color by the Occlusion output of MF_SoftbodyContactNormal (the example material does).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Dent",
		meta = (DisplayName = "Dent Darkening", ClampMin = "0.0", ClampMax = "1.0"))
	float SurfaceOcclusionStrength = 0.6f;

	// 개발: 옛 조절값 Follow Range — 조절할 일이 거의 없어 지웠다(일지 316). 몸에서 떠 있는 머리카락 카드·장식이 봉투에서
	// 멀수록 덜 따라가는 거리(cm, 메시 로컬이라 액터 스케일이 곱해지지 않는다): 따라감 = 1(이 값의 절반 안) → 0(이 값), SB_SurfaceAttachWeight.
	// 몸·겹친 옷·단추는 절반 안이라 바뀌지 않는다(일지 227). 머티리얼 파라미터 SurfaceAttachRange, 굽기의 빈 칸 옮김 한도(SoftbodyCarrierBake),
	// 층 틈 진단(DumpLayerGap)이 이 값을 읽는다.
	static constexpr float SurfaceAttachRange = 10.f;

	// 개발: 빠르기 R(1/초) 하나. 함몰은 누른 자리 기억이 갱신마다 e^{−RΔt}배(ADR-0032), 끌림은 점탄성 한 걸음(ADR-0019) — 프레임 시간과 상관없다.
	/**
	 * How quickly a dent and dragged skin spring back after the collider leaves (per second), the same for every dent size.
	 * Lower values fill in slowly, so a fast hand leaves a trail. It does not change how wide the dent is.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Recovery",
		meta = (DisplayName = "Recovery Rate", ClampMin = "0.1", UIMax = "40.0"))
	float SurfaceRecoveryRate = 30.f;

	// 개발: 받아들인 깊이(ADR-0003 §14). 오래 누르면 눌린 깊이 × 사라지는 시간 / (이 시간 + 사라지는 시간)까지 받아들인다.
	/**
	 * Lets flesh pressed for a long time keep a mark, like memory foam. 0 = no marks.
	 * While pressed, the flesh closes half its gap to the pressed shape every this many seconds. Marks fade by Memory Fade Time.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Recovery",
		meta = (DisplayName = "Memory Time", ClampMin = "0.0", UIMax = "10.0", Units = "s"))
	float SurfaceMemoryTime = 0.f;

	/**
	 * How quickly lasting marks fade: a mark halves every this many seconds, even while the flesh is still pressed.
	 * So when it is shorter than Memory Time, a long press leaves only a shallow mark.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Recovery",
		meta = (DisplayName = "Memory Fade Time", EditCondition = "SurfaceMemoryTime > 0", ClampMin = "0.05", UIMax = "10.0", Units = "s"))
	float SurfaceMemoryFadeTime = 1.5f;

	// 개발: 쿨롱 마찰 한도 = 이 배수 × 누르는 힘(ADR-0008 §12). LumpGrip도 이 한도에서 미끄러진다.
	/**
	 * Friction between a moving collider and the skin, as a multiple of how hard it presses. 0 = off. Needs Contact Detail Full.
	 * The skin moves with the collider until the flesh pulls back harder, then slips. It also limits how hard a pressing hand pulls a lump.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Drag",
		meta = (DisplayName = "Drag Friction", EditCondition = "ContactDetail == ESoftbodyContactDetail::Full", ClampMin = "0.0", UIMax = "2.0"))
	float SurfaceDragRatio = 0.5f;

	// 개발: ADR-0013 화면 크기 끄기, ADR-0020 시야 밖·닿을 수 없는 콜라이더(캐리어의 본별 깨우기 상자)도 같은 길로 쉰다.
	// 개발: 멈추는 거리 d = 1.778 R / 값(가로 90°·16:9, ComputeBoundsScreenSize). 기준을 "1 cm가 몇 픽셀"로 바꾸면 작은 소품이 더 멀리서
	// 깨어 있어 비용이 늘어 그대로 두었다(일지 227) — 대신 툴팁에 거리로 적는다.
	/**
	 * Saves GPU time on meshes seen from far away: dents stop while the whole mesh looks smaller than this (LOD Screen Size).
	 * The dents ease back as if released, then GPU work stops until it looks larger again. 0 = never stop by size. Jiggle is not affected.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Performance",
		meta = (DisplayName = "Min Screen Size", ClampMin = "0.0", UIMax = "1.0"))
	float SurfaceMinScreenSize = 0.15f;

	// 개발: 타깃 메시의 모양 거리장 부위를 올린다(ADR-0004). 기본 포즈에서 붙어 있던 살은 끈다.
	/**
	 * Skeletal meshes only: the character's own hands dent its belly, chest or thighs as an outside hand would.
	 * Needs a shape field (Softbody Authoring Data: Bake Shape Field). Only Self Contact Bones such as hand_l and hand_r press the body.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Colliders", meta = (DisplayName = "Self Contact"))
	bool bSelfContact = true;

	/**
	 * Every frame, finds overlapping colliders and lets them press the mesh. Off = outside colliders are ignored (Self Contact still works).
	 * It skips the owning actor, anything tagged SoftbodyIgnore and collision with queries disabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Colliders",
		meta = (DisplayName = "Find Colliders Automatically"))
	bool bAutoDiscoverPressure = true;

	/**
	 * Meshes with a baked shape field press with their real shape (between the fingers, into the palm) instead of their simple collision.
	 * Off = simple collision, for comparison. To bake one, right-click the mesh and pick Softbody: Bake Shape Field.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Colliders", meta = (DisplayName = "Use Shape Fields"))
	bool bUseColliderFields = true;

	/** The collision channel used to find colliders. Anything whose collision blocks this channel presses the flesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Contact|Colliders", meta = (DisplayName = "Collider Channel"))
	TEnumAsByte<ECollisionChannel> PressureChannel = ECC_Visibility;

	// 개발: 살 한 점의 흔들림 = 그 점의 무름(베이크) × 그 점을 끄는 본이 주는 가속·중력 변화(ADR-0005).
	/** Jiggle: the flesh lags behind and wobbles back when the body or mesh speeds up, turns, stops or tilts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (DisplayName = "Enable Inertia"))
	bool bEnableInertia = true;

	// 개발: 재료값 — 기준 살(Jiggle 흰색·Volume 1이고 3 cm보다 두꺼운 살)이 1 g에서 처지는 거리. 메시 안의 한 점에 맞추지 않아
	// 어느 캐릭터든 같은 살이다. 묶음 빠르기 = √(g ÷ (Amount × 그 묶음 처짐 몫)) ÷ 2π × Jiggle Speed(ADR-0015 §2.5 고침).
	/**
	 * How soft the flesh is: how far thick flesh painted white in Jiggle sags under 1 g, in cm. Firmer and thinner parts move less.
	 * Each part wobbles at the speed of its own sag: the more it sags, the slower it wobbles.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (ClampMin = "0.0", Units = "cm"))
	float InertiaAmount = 2.5f;

	// 개발: 감쇠비 ζ. 한 번 넘칠 때마다 남는 비 = e^(−πζ/√(1−ζ²)) — 0.25면 44 %·19 %·8 %라 눈에 띄는 넘침(첫 높이의 10 % 넘음)이 둘,
	// 0.4면 25 %·6 %라 하나, 0.1이면 일곱(ZZZ "한두 번" = 0.25~0.4). 넘치는 횟수는 빠르기와 무관하고 멈추는 시간만 빠르기에 반비례한다.
	/**
	 * For how springy the flesh is: how many times a wobble swings past before it stops. 0.25 to 0.4 gives one or two bounces: 0.25
	 * about two and 0.4 about one. Near 0.1 it keeps swinging about seven times. It does not change the wobble speed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (ClampMin = "0.01", ClampMax = "0.99"))
	float InertiaDamping = 0.25f;

	// 개발: 연출 값이다 — 재료값이 아니다(빠르기는 처짐에서 나오는 결과값이라 원칙 P03의 예외). 묶음 진동자의 빠르기에
	// 곱하고 움직임 하중은 같은 비로 나눠, 급정지·착지 뒤 첫 튐 높이(≈ ω × 처짐 × 속도 변화 ÷ g)가 그대로다. 기울 때 중력 처짐도 그대로.
	/**
	 * Changes the rhythm of the jiggle without changing its size: 2 wobbles twice as fast and settles in half the time.
	 * The bounce after a landing and the sag when the body tilts stay the same.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (ClampMin = "0.1", UIMin = "0.25", UIMax = "4.0"))
	float JiggleSpeed = 1.f;

	/**
	 * Different jiggle per body part: amount, damping and gravity for one inertia group (a bone). Pick the bone from the dropdown, which
	 * lists the inertia groups found by the bake. Parts not listed, and settings not ticked, use the values above.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia",
		meta = (GetKeyOptions = "GetInertiaGroupBoneNames"))
	TMap<FName, FSoftbodyInertiaGroupOverride> InertiaGroupOverrides;

	/**
	 * Scales the jiggle along each axis of the mesh as modeled (Z is up), for example more bounce up and down when landing and less
	 * sideways sway when turning. Turning motion is not scaled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia")
	FVector InertiaDirectionScale = FVector(1.f, 1.f, 1.f);

	// 개발: 저장 이름은 옛 "포화"(g)지만 값은 단단함 F = 1/포화다(일지 297). 옛 값은 0 → 0.01에서 출렁임이 98 % 꺼지고
	// 양수끼리는 올릴수록 느슨했다. 이름을 두어 시연 몸의 저장값 1이 그대로 1이다.
	/**
	 * For flesh that firms up in big movements, such as a body that runs and stops. 0 = off (the wobble grows with the motion).
	 * At 1 a push as strong as gravity halves the wobble, and it never exceeds Inertia Amount divided by this value (cm).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (DisplayName = "Inertia Firmness", ClampMin = "0.0"))
	float InertiaSaturation = 0.f;

	// 개발: 덩어리(뼈대에서 뺀 본의 살)·스태틱 메시만 처진다. 뼈대 부위는 처지지 않는다 — 묶음 하나의 신호가 배·등 살을 한 판으로 밀어
	// 피부가 뼈 위를 미끄러지고 걸을 때 몸통이 기울 때마다 처졌다 돌아왔다(ADR-0015 §2.2). 그 배수였던 Skeleton Gravity Scale(기본 0)은 0이
	// 아니면 그 결함을 되살릴 뿐이고 쓰는 에셋이 없어 지웠다(일지 316). 부위 하나를 처지게 하려면 그룹 덮어쓰기의 Gravity Scale.
	/**
	 * How strongly gravity pulls lumps and static meshes (1 = real gravity, 0 = off). At 1 they sag only when leaning or lying down.
	 * Above 1 they also hang lower while upright. Parts on the skeleton sag only with a Gravity Scale in Inertia Group Overrides.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (DisplayName = "Gravity Scale", ClampMin = "0.0"))
	float InertiaGravityScale = 1.f;

	// 개발: 상한은 상태가 아니라 브릿지에 올리는 값에 건다(ADR-0015 §2.4).
	/**
	 * Safety limit for how far jiggle can move a vertex, in cm, for each inertia group (the whole mesh for a static mesh).
	 * Movement is unchanged up to 70% of it, then eases in, so reaching it flattens the peak without a pop.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia",
		meta = (DisplayName = "Max Inertia Offset", ClampMin = "0.0", Units = "cm"))
	float InertiaMaxOffset = 25.f;

	// 개발: 기본 패널로(일지 297) — Inertia Amount는 크기와 빠르기를 함께 바꾸므로 크기만 바꾸는 길이 이것이다.
	/**
	 * How much the actor moving or turning makes the flesh jiggle (1 = the real acceleration). Raise it for a bigger jiggle at the same
	 * speed. Tilting under gravity is not scaled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia",
		meta = (DisplayName = "Actor Motion Weight", ClampMin = "0.0"))
	float ComponentInertiaWeight = 1.f;

	/**
	 * How much the skeletal animation moving the bones makes the flesh jiggle (1 = the real acceleration). Raise it for a bigger jiggle
	 * at the same speed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia",
		meta = (DisplayName = "Animation Motion Weight", ClampMin = "0.0"))
	float AnimationInertiaWeight = 1.f;

	// 개발: 덩어리 밀림(ADR-0012), 살 두께 너머로는 손을 따라 비켜난다(ADR-0017). 캐리어에 덩어리 값이 구워져 있어야 한다.
	// 개발: 흐리는 조건 = 실행 중 조건(UpdateSurfaceField의 bLumpForce, ResolveLumpConstants의 흔들림 크기 0.01 cm 이상)에서 이 컴포넌트 값만 남긴 것.
	// 흔들림 크기는 그룹 배율이 곱해지므로 0일 때만 흐린다. 덩어리가 구워졌는지는 Mesh Data에 있어 흐리지 못하고 Validate Setup이 알린다.
	// 개발: 스태틱 메시에는 덩어리가 없다(일지 296) — 메시 전체가 덩어리 하나이던 것을 걷었다.
	/**
	 * How far pressing a lump (flesh skinned to a Lump bone, such as a breast) moves the whole lump aside, not only the dent.
	 * 0 = dent only, 1 = as far as the baked flesh allows. Needs Enable Contact, Enable Inertia and Inertia Amount above 0.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia|Lumps",
		meta = (EditCondition = "bEnableContact && bEnableInertia && !bContactOnlyValidation && InertiaAmount > 0", ClampMin = "0.0", UIMax = "4.0"))
	float LumpYield = 1.f;

	// 개발: 덩어리 쥐기(ADR-0012 §7, ADR-0008 §12). 덩어리 옆 몫이 구워져 있어야 한다.
	// 개발(ADR-0041 §3.4): Softbody Grabber의 잡기도 같은 세기로 덩어리를 끈다(세 방향 곧은 용수철, 마찰 한도 없음) — Basic·Drag Friction 0에서도. 그래서
	// 편집 조건에서 Full·Drag Friction을 뺐다(누르는 손의 쥐기만 그 둘이 필요하다 — 툴팁).
	/**
	 * How firmly holding a lump's skin carries the whole lump: 1 = as firmly as the flesh resists pressing, 0 = only the skin stretches.
	 * A pressing hand pulls it only with Contact Detail Full and Drag Friction above 0. Needs Lump Yield above 0 and Enable Inertia.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia|Lumps",
		meta = (EditCondition = "bEnableContact && LumpYield > 0 && bEnableInertia && !bContactOnlyValidation && InertiaAmount > 0",
		ClampMin = "0.0", UIMax = "4.0"))
	float LumpGrip = 1.f;

	// 개발: 조일수록 단단해지는 살(ADR-0017, ADR-0008 §12, Fung 할선). 두께 너머로는 두께 끝의 가파름으로 계속 커진다.
	// 개발: 읽는 곳은 끌림(Full이고 Drag Friction > 0)과 덩어리를 미는 힘(bLumpForce) 둘뿐이라 흐리는 조건도 그 둘의 이 컴포넌트 몫이다.
	/**
	 * How much harder the flesh pushes back when squeezed deep, like real flesh firming up near the bone. 0 = in proportion to depth.
	 * Shallow presses are hardly affected. It acts only on pushed lumps and on skin drag (Contact Detail Full, Drag Friction above 0).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia|Lumps",
		meta = (DisplayName = "Squeeze Stiffening", EditCondition = "bEnableContact && ((ContactDetail == ESoftbodyContactDetail::Full && SurfaceDragRatio > 0) || (LumpYield > 0 && bEnableInertia && !bContactOnlyValidation && InertiaAmount > 0))",
		ClampMin = "0.0", UIMax = "6.0"))
	float LumpStiffen = 3.f;

	// 개발: 진동자는 해석해라 프레임 시간이 변해도 발산하지 않는다. 히치 뒤 한 틱은 속도만 잰다(FSoftbodyInertiaDriver::bHasVelocity).
	/**
	 * A frame longer than this (seconds) counts as a hitch: its acceleration is ignored so a frame drop does not jolt the flesh. The
	 * wobble itself stays smooth at any frame rate.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia",
		meta = (DisplayName = "Hitch Guard", ClampMin = "0.01", ClampMax = "1.0", Units = "s"))
	float AccelerationDtGuard = 0.1f;

	// 개발: MeasureMotion이 지금 속도로 갈 수 있는 거리 + 이 값을 넘으면(또는 45° 넘게 더 돌면) 순간이동으로 본다.
	/**
	 * A move this much farther (cm) than the current speed allows in one frame counts as a teleport and does not jiggle the flesh.
	 * This covers SetActorLocation without the teleport flag, respawns and animation cuts. 0 = only moves flagged as teleports count.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Inertia", meta = (ClampMin = "0.0", Units = "cm"))
	float TeleportDistance = 50.f;

	// 개발: MF_SoftbodyDebugVis의 VisWeight 파라미터.
	/** Shows debug colors (Debug View Mode) instead of the normal shading, on materials that use MF_SoftbodyDebugVis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Debug", meta = (DisplayName = "Debug View"))
	bool bDebugVis = false;

	/**
	 * What the debug colors show, with the same colors as the Last Bake views in the Softbody Authoring Data Preview.
	 * 0 = Flesh Fit. 1 = Core Connection. 2 = Contact Map Seams. 3 = Distance to Outer Surface. 4 = Jiggle Size. 5 = Dents Between Vertices.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Debug",
		meta = (DisplayName = "Debug View Mode", ClampMin = "0", ClampMax = "5", EditCondition = "bDebugVis"))
	int32 DebugVisMode = 0;

	/** Debug: turns every jiggle off to look at contact alone. Not meant for shipping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Softbody|Debug",
		meta = (DisplayName = "Contact Only (Debug)"))
	bool bContactOnlyValidation = false;

	/** Called once per frame after the dents and jiggle have been updated and sent to the material. */
	UPROPERTY(BlueprintAssignable, Category = "Softbody")
	FSoftbodyUpdatedDelegate OnSoftbodyUpdated;

	/**
	 * Checks the setup and writes any problem to the Output Log (LogSoftbodyRuntime). Returns true when nothing needs fixing.
	 * It checks Mesh Data, the Softbody material functions and the jiggle data, and names settings that do nothing on this mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "Softbody|Setup")
	bool ValidateSetup() const;

	/**
	 * Checks the setup and writes any problem to the Output Log (category LogSoftbodyRuntime): the target mesh matches Mesh Data, the
	 * mesh's materials contain the Softbody material functions, and the bake has jiggle data. It also names settings that do nothing on this mesh.
	 */
	UFUNCTION(CallInEditor, Category = "Softbody", meta = (DisplayName = "Validate Setup"))
	void ValidateSetupButton() const;

	/** 표면 장(v5) GPU 상태. 캐리어가 없거나 아직 만들어지지 않았으면 null. 진단 도구용. */
	USoftbodySurfaceField* GetSurfaceField() const { return SurfaceField; }

	/**
	 * 잡기(ADR-0041) — Softbody Grabber가 부른다. Location·TraceDirection은 월드(TraceDirection 0 = 보지 않음, Grab At Hit만 트레이스가 들어온 쪽을 보는 칸을
	 * 고른다), Radius는 월드 cm. CPU가 그 프레임에 아는 실패(이 메시의 붙든 잡기가 SoftbodyGrab::MaxHolding개, 접촉 꺼짐, 아직 준비 안 됨)면 INDEX_NONE과
	 * 까닭. 잡은 칸은 GPU가 다음 갱신에 고르고 못 찾으면 Grabber에 Missed를 알린다. 줄이 다 찼으면 놓는 중 줄 가운데 남은 당김이 가장 작은 줄을 쓴다.
	 */
	int32 BeginGrab(USoftbodyGrabberComponent* Grabber, const FVector& Location, const FVector& TraceDirection, float Radius, uint32& OutSerial,
		FString* OutReason = nullptr, float SearchDistance = 0.f);
	/** 잡기를 놓는다(Grabber의 Release) — 남은 당김은 Recovery Rate로 돌아온다. 칸·번호가 맞지 않으면 아무것도 하지 않는다. */
	void EndGrab(int32 Slot, uint32 Serial);
	/** 잡기 하나의 GPU 상태(1~3프레임 늦다). */
	struct FGrabInfo
	{
		/** GPU가 잡은 칸을 골랐다. */
		bool bHeld = false;
		/**
		 * 보이는 길이(cm — 잡은 자리가 잡은 뒤 움직인 길이 = 덩어리 + 표면 장, 한도 뒤)와 한도 min(살 두께, 원 반지름)(cm), 잡은 점 살 두께(cm), 잡은 뒤 덩어리가
		 * 따라 나온 길이(cm).
		 */
		float Stretch = 0.f;
		float Limit = 0.f;
		float Flesh = 0.f;
		float LumpFollow = 0.f;
		/** 잡은 점의 지금 피부 자리(월드). */
		FVector GrabbedPoint = FVector::ZeroVector;
	};
	/** GPU가 이 잡기(칸·번호)를 읽었으면 채우고 true. */
	bool GetGrabInfo(int32 Slot, uint32 Serial, FGrabInfo& Out) const;
	/** 소프트바디를 입힌 메시 컴포넌트. 진단 도구용. */
	UMeshComponent* GetTargetMeshComponent() const { return ResolveTargetMeshComponent(); }

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	/**
	 * 켜기·끄기(ADR-0020 §3.6): 엔진 표준 Activate/Deactivate/SetActive가 이 컴포넌트의 완전한 스위치다. 생성자가 Auto Activate를 켜고 틱은
	 * 활성화가 켜므로(bStartWithTickEnabled 0) Auto Activate를 끈 컴포넌트는 Activate 전까지 아무것도 하지 않는다. 끄면 틱이 멈추고(콜라이더 질의·
	 * 본 행렬·관성·브릿지 업로드·파라미터 밀기 없음) 표면 장을 재워 텍스처 벌을 풀에 돌려주고(GPU 갱신 없음) 관성 상태를 비운 뒤 머티리얼에 꺼진 값
	 * (브릿지 0, SurfaceFieldOn 0, InertiaResponseCount 0)을 한 번 밀어 메시가 모델링한 모양으로 돌아온다(SuspendRuntime). 켜면 첫 틱이 처음처럼
	 * 시작한다(속도 기록 없음, 끌림의 지난 위치 없음, 닿는 순간은 미루지 않음). 월드 풀의 미리 만든 벌(softbody.Surface.TexturePool)은 켜진
	 * 컴포넌트만 예약한다.
	 */
	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;

	/**
	 * 표면 장이 부른다(ADR-0021, 액터 틱이 다 끝난 뒤 같은 프레임): 이번 프레임 GPU 갱신을 돌렸는가. 미뤘으면(갱신률 LOD) 콜라이더의
	 * 지난 위치를 그대로 두고 머티리얼의 지난 변위를 지금 변위로 묶는다(화면이 안 바뀌었으니 속도 0).
	 */
	void OnSurfaceFieldScheduled(bool bRan);
	/**
	 * 표면 장이 부른다(일지 203): 눌린 채 입력이 멈추고 평형이라 이번 프레임 갱신을 신청하지 않았다. 미룬 프레임과 같이 지난 변위를 지금 변위로 묶고 콜라이더의
	 * 지난 위치를 그대로 둔다(stat softbody "Still").
	 */
	void OnSurfaceFieldStill();
	/**
	 * 표면 장의 렌더 타깃을 머티리얼에 잇거나(텍스처 한 벌을 잡았을 때) 푼다(접촉 끔·텍스처를 풀에 돌려줄 때 — 풀어야 머티리얼이 타깃을
	 * 붙들지 않는다, ADR-0022). 표면 장이 부른다.
	 */
	void BindSurfaceFieldTextures(bool bBind);
	/** 표면 장이 쥔 텍스처를 월드 풀에 돌려준다(액터 삭제·재등록). */
	virtual void OnUnregister() override;
	/** SurfaceField를 GC에 알린다. 그 형식은 Internal 헤더에 있어 UPROPERTY로 두면 Public 헤더만 있는 바이너리판에서 UHT가 형식을 찾지 못한다. */
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
#if WITH_EDITOR
	/**
	 * 편집 알림(디테일 패널·Python set_editor_property). 실행 중(게임 월드) 이 컴포넌트의 값을 고치면 엔진의 다시 등록(블루프린트 액터는 컴포넌트를 통째로
	 * 새로 만들기)을 건너뛴다 — 남은 자국·끌림·쥔 덩어리·흔들림을 두고 새 값으로 이어 간다. 엔진 값(Auto Activate·틱 설정 등)과 에디터 월드의 편집은 엔진 길
	 * 그대로다.
	 */
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PreEditChange(class FEditPropertyChain& PropertyAboutToChange) override;
#endif

private:
	/** 표면 장(v5)이 동작하는가 (캐리어가 있어 GPU 상태를 만들었다). 아니면 접촉이 꺼져 있다. */
	bool IsSurfaceFieldMode() const;
	/**
	 * 메시가 화면에 파임이 보일 만큼 크게 보이는가(SurfaceMinScreenSize, ADR-0013). 경계에서 켜졌다 꺼졌다 하지 않게 켤 때는
	 * 20% 더 커야 한다. 지난 프레임에 이 월드를 그린 뷰가 없으면(첫 프레임, 렌더링 없는 자동 테스트) 켠다.
	 */
	bool UpdateSurfaceFieldNear();
	/**
	 * 채널 오버랩으로 누를 수 있는 콜라이더의 단순 형상을 모은다. OutFieldColliders를 주면 모양 거리장이 구워진
	 * 콜라이더는 형상 대신 그 목록에 넣는다. 오버랩 후보 수를 돌려준다.
	 */
	int32 GatherPressureShapes(TArray<FSoftbodyColliderShape>& OutShapes,
		TArray<const UPrimitiveComponent*>* OutFieldColliders = nullptr);
	/**
	 * 자기 접촉: 타깃 메시 거리장의 부위들을 올린다. Skinning은 타깃 본의 기본 포즈 → 현재 행렬(컴포넌트 공간).
	 * 부위마다 이번 프레임 위치를 CurrentFieldFrames에 적는다(끌림이 다음 프레임에 쓴다). 잠들기 상자(Wake)에 닿을 수 없는 부위는
	 * 올리지 않고 OutGated에 센다.
	 */
	void AppendSelfFieldInstances(TConstArrayView<FMatrix44f> Skinning, const FMatrix& WorldToFrame,
		FSoftbodySurfaceFieldInput& Input, TMap<TPair<FObjectKey, int32>, FMatrix>& CurrentFieldFrames,
		const FSoftbodyWakeBoxes& Wake, int32& OutGated);
	/**
	 * 실행 상태를 만든다(이미 있으면 걷고 다시): 변형할 메시를 찾아 그 머티리얼에 값을 넣을 통로(MID)를 걸고, 표면 장을 만들고,
	 * 흔들림을 멈춘 상태에서 시작한다. 게임이 시작할 때와, 세팅한 대상이 바뀌었을 때(IsSetupStale) 부른다.
	 */
	void SetupRuntime();
	/**
	 * 세팅한 뒤에 그 대상이 바뀌었는가: MeshData, 변형할 메시(지워졌거나 다른 것을 골랐다), 메시에 걸어 둔 머티리얼. 틱마다 본다.
	 * 블루프린트 액터의 값을 실행 중에 디테일 패널에서 고치면 엔진이 컴포넌트를 전부 새로 만드는데, 그 경우도 여기서 잡힌다.
	 */
	bool IsSetupStale() const;
	/** ValidateSetup의 몸통. bAllUnusedValues면 이 메시에서 아무 일도 하지 않는 값을 모두, 아니면(게임 시작 때) 기본값에서 바꾼 것만 알린다. */
	bool ValidateSetupInternal(bool bAllUnusedValues) const;
	/**
	 * 이 메시에서 아무 일도 하지 않는 값을 디테일 패널 이름으로 로그에 적는다. 메시 종류·구운 데이터·모양 거리장처럼 다른 오브젝트에 달린 조건만 본다 — 같은
	 * 컴포넌트의 다른 값에 달린 조건은 디테일 패널이 흐린다(EditCondition).
	 */
	void LogUnusedValues(const UMeshComponent* Target, bool bAll) const;
#if WITH_EDITOR
	/** 실행 중 편집을 다시 등록 없이 받을 값인가: 이 컴포넌트(또는 그 블루프린트 자식)가 선언한 값이고, 게임 월드에 등록돼 있다(PreEditChange). */
	bool KeepsRuntimeOnEdit(const FProperty* Property) const;
#endif
	/** 표면 장이 있어야 하는가: 접촉이 켜져 있고 컴포넌트가 켜져 있다(일지 222 — 끈 컴포넌트는 표면 장을 두지 않는다). */
	bool WantsSurfaceField() const;
	/** 끄거나 접촉을 끌 때 표면 장을 재운다(지우지 않는다 — 다시 켜면 깨운다, 일지 235). */
	void SleepSurfaceField();
	/** 접촉 스위치(bEnableContact)나 켜기·끄기가 바뀌면 표면 장을 만들거나 깨우거나 재운다. */
	void SyncSurfaceField();
	/**
	 * 끌 때(Deactivate, 또는 켜지지 않은 채 세팅한 뒤): 표면 장을 지우고(텍스처·접촉 지도·풀 예약을 놓는다) 관성 상태·끌림의 지난 위치를 비우고
	 * 머티리얼에 꺼진 값을 한 번 민 뒤, 대상 메시의 World Position Offset 계산을 끈다(일지 222).
	 */
	void SuspendRuntime();
	/**
	 * 대상 메시의 World Position Offset 계산을 끄거나(false — 지금 값을 적어 둔다) 적어 둔 값으로 되돌린다(true). 끈 컴포넌트의 머티리얼 정점
	 * 함수가 아예 돌지 않게 한다(일지 222). 스태틱·스킨드 메시만.
	 */
	void SetTargetWorldPositionOffset(bool bRestore);
	/** 표면 장을 지우고(텍스처는 풀에 돌려준다) 콜라이더의 지난 위치(끌림)를 잊는다. 없으면 아무 일도 없다. */
	void ReleaseSurfaceField();
	/** 머티리얼의 "지난 변위" 텍스처를 지금 변위(true, 미룬 프레임)나 지난 갱신의 변위(false)로 묶는다. 바뀔 때만 밀어 넣는다. */
	void BindPreviousDisplacement(bool bToCurrent);
	/** 잠들기(ADR-0020): 덩어리가 흔들려 비켜날 수 있는 거리의 상한(cm) — 잠들기 상자를 이만큼 더 키운다. 덩어리가 없으면 0. */
	float WakeLumpLagBound() const;
	/** 표면 장 한 틱: 콜라이더·본 행렬을 모아 GPU 갱신을 올린다. */
	void UpdateSurfaceField(float Dt);
	void StepInertia(float Dt);
	/** 덩어리(ADR-0012, ADR-0017): GPU가 푼 덩어리 평형과 그 자리의 살 강성을 읽어 덩어리마다 목표와 쥔 조직의 모드(§8)를 둔다. */
	void UpdateLumpContact();
	/**
	 * 덩어리 평형의 상수(ADR-0017): 뿌리 스프링 C(덩어리 살의 부피 평균 무름, 대칭), 흔들림 크기 A, 살 단단함 × 칸 넓이 s
	 * (= LumpYield·σ/V × 칸 넓이). 풀 수 없는 덩어리(부피·무름 0, 흔들림 크기 0, 묶음 밖)는 false.
	 */
	bool ResolveLumpConstants(const struct FSoftbodyInertiaLump& Lump, double& OutStiffness, double& OutAmount, double OutCompliance[3][3]) const;
	/** 구동점 Group의 선형 목표: 관성 목표 T에 덩어리 평형(ADR-0017)을 얹는다. 쥔 손이 없으면 T 그대로. */
	FVector ResolveLumpTarget(int32 Group, const FVector& InertialTarget) const;
	/**
	 * 쥔 덩어리(ADR-0012 §8): 접촉 조직의 강성을 넣어 구동점의 선형 흔들림 세 축을 모드별로 한 프레임 적분한다. 한 주기가 표면 장 갱신 네 번보다 짧은
	 * 모드(UpdateInterval = 표면 장 갱신 간격, 초)는 떨지 않고 평형을 따라간다(일지 337, ADR-0040 §4.4).
	 */
	void AdvanceLumpModes(struct FSoftbodyInertiaDriver& Driver, const FVector& Target, int32 Group, float FrequencyHz, float DampingRatio, float Dt,
		float UpdateInterval) const;
	/** 관성 브릿지 값(BridgeWidth × BridgeRows, 배치는 BridgeWidth 주석). 머티리얼 텍스처와 표면 장이 같은 값을 쓴다. */
	void BuildInertiaBridge(TArray<FVector4f>& Out) const;
	/** 관성 그룹(스태틱은 0)의 덮어쓰기. 없으면 nullptr. */
	const FSoftbodyInertiaGroupOverride* FindInertiaGroupOverride(int32 Group) const;
	/** 관성 그룹의 흔들림 크기(cm/g) = InertiaAmount × 그룹 배율. */
	float ResolveInertiaGroupAmount(int32 Group) const;
	/** 관성 그룹(스태틱은 0)의 처짐 몫과 가장 큰 정점 몫(굽기 InertiaGroupSag·SagMax). 옛 굽기는 1, 1(전과 같은 동작). */
	void ResolveInertiaGroupSag(int32 Group, float& OutSag, float& OutSagMax) const;
	/** 관성 그룹의 중력 처짐 배율: 덮어쓰기 > (덩어리·스태틱이면 InertiaGravityScale, 뼈대 부위면 0 — 일지 316). */
	float ResolveInertiaGroupGravityScale(int32 Group) const;
	/** InertiaGroupOverrides 키 드롭다운 = 베이크의 관성 그룹 본 이름. */
	UFUNCTION()
	TArray<FString> GetInertiaGroupBoneNames() const;
	void ResetInertiaState();
	/** 타겟 메시의 트랜스폼이 바뀔 때마다 불린다. 엔진이 순간이동으로 표시한 이동이면 적어 둔다(StepInertia가 쓴다). */
	void OnTargetTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);
	bool HasInertiaResponseBake() const;
	UMeshComponent* ResolveTargetMeshComponent() const;
	FTransform GetTargetTransform() const;
	bool IsMaterialCompatible(const UMaterialInterface* Material) const;
	void UploadContactTexture();
	void PushMaterialParams();
	/** 잡기(ADR-0041): GPU 잡기 줄 읽기를 받아 칸 상태를 옮기고 Grabber에 못 잡음을 알린다(틱마다, 표면 장 갱신 앞). */
	void PollGrabs();
	/** 잡기 줄을 이번 갱신 입력에 싣는다(붙든·놓는 중 줄이 하나라도 있을 때만 — 없으면 셰이더가 잡기 코드를 건너뛴다). */
	void BuildGrabRows(const FTransform& TargetTransform, FSoftbodySurfaceFieldInput& Input);
	/** 표면 장을 재우거나 지울 때: 붙든 잡기를 대상 사라짐으로 끝내고 모든 칸을 비운다(GPU 줄은 다음에 깨면 비우기 표지로 지운다). */
	void DropGrabs();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> MIDs;
	/** MIDs와 같은 순서로, 그 머티리얼을 건 메시의 슬롯 번호. */
	TArray<int32> MIDSlots;
	/** 세팅할 때 찾은 변형할 메시(TargetMeshComponent를 비워 둔 경우에는 찾아낸 것). */
	TWeakObjectPtr<UMeshComponent> SetupTarget;
	/** 세팅할 때 변형할 메시가 있었는가(없었으면 "지워졌다"로 읽지 않는다). */
	bool bSetupHadTarget = false;
	/** 세팅할 때의 MeshData. */
	FObjectKey SetupMeshData;
	/** 세팅을 첫 틱으로 미뤘다(BeginPlay가 인스턴스 값이 돌아오기 전에 불렸다). */
	bool bSetupPending = false;
	/** 지금 편집 알림이 고치는 값이 이 컴포넌트의 값인가 — 구조체 안쪽 값이면 사슬 판 PreEditChange가 멤버로 가려 FProperty 판에 넘긴다. */
	bool bEditingOwnValue = false;

	/** 표면 장(v5) GPU 상태. 캐리어가 있을 때만 존재한다. UPROPERTY가 아니며 AddReferencedObjects가 GC에 알린다(저장·복제되지 않는 것은 Transient와 같다). */
	TObjectPtr<USoftbodySurfaceField> SurfaceField;
	bool bLoggedSurfaceSkippedShapes = false;
	bool bLoggedFieldBudget = false;
	bool bLoggedSelfFieldMismatch = false;
	bool bLoggedWakeSelfMismatch = false;
	bool bLoggedWakeMissing = false;
	/** 마지막으로 적용한 WantsSurfaceField(실행 중 바뀌면 SyncSurfaceField가 맞춘다). */
	bool bContactApplied = false;
	/** World Position Offset 계산을 끈 메시와 끄기 전 값(SetTargetWorldPositionOffset). */
	TWeakObjectPtr<UMeshComponent> WorldPositionOffsetTarget;
	bool bSavedEvaluateWorldPositionOffset = true;
	bool bWorldPositionOffsetSuspended = false;
	/**
	 * 끌림(ADR-0008): 지난 프레임 콜라이더 위치(형상·거리장 부위 로컬 → 표면 장 프레임 행렬). 단순 형상은 (컴포넌트,
	 * 바디/요소 번호), 거리장은 (컴포넌트, 부위 번호)로 찾는다. 매 표면 장 틱에 이번 프레임 것으로 바꾼다.
	 */
	TMap<TPair<FObjectKey, int32>, FMatrix> PreviousShapeFrames;
	TMap<TPair<FObjectKey, int32>, FMatrix> PreviousFieldFrames;
	/** 이번 틱 위치. 표면 장이 이번 프레임 GPU 갱신을 돌렸을 때만 Previous가 된다(미룬 프레임은 건너뛴다, ADR-0021). */
	TMap<TPair<FObjectKey, int32>, FMatrix> PendingShapeFrames;
	TMap<TPair<FObjectKey, int32>, FMatrix> PendingFieldFrames;
	/**
	 * 손 닻(일지 336): 지난 갱신의 스키닝 행렬(FSoftbodySurfaceFieldInput::Bones와 같은 꼴). 콜라이더의 지난 위치와 같은 때에 바뀐다 — 이번 틱 것은
	 * Pending에 두고, 표면 장이 이번 프레임 GPU 갱신을 돌렸을 때만(또는 장이 멈춰 틱마다 따라갈 때) Previous가 된다.
	 */
	TArray<FVector4f> PreviousSurfaceBones;
	TArray<FVector4f> PendingSurfaceBones;
	/** 들어온 면 표지(일지 313)의 콜라이더 고정 번호. */
	FSoftbodyColliderLabelIds ColliderLabelIds;
	/** Previous*Frames가 표면 장이 실제로 돌린 지난 갱신 것인가(들어온 면 표지의 길 검사). 돌리지 않은 틱이 끼면 false. */
	bool bMotionFromLastUpdate = false;

	/** 스켈레탈 타깃 (본 행렬·스킨 웨이트를 읽는다). 스태틱 메시면 비어 있다. */
	TWeakObjectPtr<USkinnedMeshComponent> SkinnedTarget;

	/** 관성 브릿지 (BridgeWidth×BridgeRows PF_FloatRGBA) — 배치는 BridgeWidth 주석 참조 */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ContactTexture;

	/**
	 * 지난 프레임의 관성 브릿지(모션 벡터용). 머티리얼이 지난 프레임 흔들림 변위를 구해 속도 버퍼에 쓴다 — 없으면
	 * 흔들리는 살이 속도 0으로 잡혀 TSR 잔상이 남는다.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> PreviousContactTexture;
	/** 지난 프레임에 올린 브릿지 값(다음 프레임에 PreviousContactTexture로 올린다). */
	TArray<FFloat16Color> LastContactData;
	/** 지난 프레임 타깃 회전(L2W 열 세 개, 모션 벡터용). */
	FLinearColor LastL2W[3];
	bool bHasLastL2W = false;

	/** 단순 콜리전도 거리장도 없어 누르지 못하는 콜라이더(경고를 한 번만 남긴다). */
	TSet<TWeakObjectPtr<const UPrimitiveComponent>> ComplexOnlyLogged;

	// 관성 구동점: 스켈레탈은 본 그룹마다 하나, 스태틱은 [0] 하나(앵커 중심). 평형은 가속·회전 가속·중력 방향 변화와
	// 덩어리를 누르는 손(ADR-0012, ADR-0017 — LumpSolutions)이 같이 정한다.
	FSoftbodyInertiaDriver InertiaDrivers[MaxInertiaGroups];
	/** 구동점마다 이번 프레임 관성 목표(기본 자세 축 하중 g, 손 몫 전). 덩어리 평형을 풀라고 GPU에 올린다(ADR-0017). */
	FVector LumpInertialTarget[MaxInertiaGroups];
	/**
	 * 덩어리 평형(ADR-0017): GPU가 몇 프레임 전 관성 목표 TargetUsed로 푼 평형 Equilibrium(기본 자세 축 하중 g). 손이 없으면
	 * bValid = false.
	 */
	struct FLumpSolution
	{
		bool bValid = false;
		FVector Equilibrium = FVector::ZeroVector;
		FVector TargetUsed = FVector::ZeroVector;
	};
	FLumpSolution LumpSolutions[MaxInertiaGroups];
	/**
	 * 쥔 덩어리(ADR-0012 §8): 덩어리 운동 C·L''/ω0² = −(C + sQ)(L − E) − 감쇠·L'의 모드. 일반 고유문제 sQ·v = Λ·C·v의
	 * v(열, VᵀCV = I)와 Λ. 감쇠는 모드마다 자유 출렁임과 같은 감쇠비다(같은 살의 손실, §8.1). 접촉이 없으면 bValid = false.
	 */
	struct FLumpModes
	{
		bool bValid = false;
		double Basis[3][3] = {};
		double Compliance[3][3] = {};
		double Stiffness[3] = {};
	};
	FLumpModes LumpModes[MaxInertiaGroups];
	/** 가장 최근에 받아 쓴 덩어리 평형의 번호(USoftbodySurfaceField::GetLumpSolution). */
	uint64 LumpSerial = 0;
	/** 지난 틱에 표면 장이 깨어 있어 갱신을 신청했는지. 쉬었으면(콜라이더 없음) 누르는 힘도 없다. */
	bool bSurfaceFieldDispatched = false;
	/** 지난 틱에 묶음이 이 표면 장의 GPU 갱신을 실제로 돌렸는지(false = 갱신률 LOD가 미뤘다, ADR-0021). */
	bool bSurfaceFieldRan = false;
	/**
	 * 표면 장 갱신 간격(초, 일지 337): 깨어 있는 동안 GPU 갱신이 실제로 돈 사이의 시간(갱신률 LOD가 미룬 틱을 담는다). 쥔 덩어리 모드 가운데 한 주기가
	 * 이것의 네 배보다 짧은 것은 평형을 따라간다(AdvanceLumpModes). 0 = 아직 모름(틱 시간을 쓴다).
	 */
	float SurfaceUpdateInterval = 0.f;
	/** 마지막 GPU 갱신 뒤 흐른 시간(초). 표면 장이 쉬면 0으로 돌린다. */
	float SurfaceSinceUpdate = 0.f;
	/** 머티리얼의 "지난 변위"가 지금 변위에 묶여 있는지(미룬 프레임). */
	bool bPrevDispBoundToCurrent = false;
	/** 메시가 파임이 보일 만큼 크게 보였는지(UpdateSurfaceFieldNear). */
	bool bSurfaceFieldNear = true;
	/** 메시가 지난 프레임에 그린 어느 뷰의 시야에든 들었는지(UpdateSurfaceFieldNear, 뷰가 없으면 true). 갱신 차례에 쓴다. */
	bool bSurfaceFieldInView = true;
	/** 지난 프레임 뷰에서 잰 화면 크기(UpdateSurfaceFieldNear, 뷰가 없으면 1). 갱신률 LOD가 쓴다. */
	float SurfaceScreenSize = 1.f;
	bool bMotionInit = false;
	/** 지난 관성 계산 뒤에 타겟이 순간이동했다(리스폰처럼 텔레포트로 표시한 이동). 건너뛴 거리는 가속으로 읽지 않는다. */
	bool bTargetTeleported = false;
	FDelegateHandle TargetTransformHandle;

	// 잡기(ADR-0041): 메시마다 GPU 줄 MaxGrabRows개(SoftbodyGrab::MaxRows와 같다)의 CPU 칸. 놓는 중 줄은 GPU가 1 µm 아래로 비웠다고 읽을 때 빈다.
	static constexpr int32 MaxGrabRows = 8;
	struct FGrabSlot
	{
		enum class EState : uint8 { Free, Holding, Releasing };
		EState State = EState::Free;
		/** 잡기 번호(1부터, 0 = 없음) — GPU 줄의 번호와 견준다. */
		uint32 Serial = 0;
		TWeakObjectPtr<USoftbodyGrabberComponent> Grabber;
		/** 잡는 점과 트레이스 방향(월드, 방향 0 = 보지 않음), 원 반지름(월드 cm). */
		FVector Location = FVector::ZeroVector;
		FVector TraceDirection = FVector::ZeroVector;
		float Radius = 1.f;
		/** 잡는 점에서 피부를 찾는 거리(cm, 0 = 기본 SoftbodyGrab::SearchDistance). */
		float SearchDistance = 0.f;
		/** GPU 확인 없이 비운 칸 — 다음 갱신에 GPU 줄을 바로 비우게 한다(SoftbodyGrab::FlagClear). */
		bool bClear = false;
		/** GPU가 이 번호를 읽었다(아래 값이 있다). */
		bool bSeen = false;
		bool bGpuHeld = false;
		float Stretch = 0.f;
		float Limit = 0.f;
		float Flesh = 0.f;
		float LumpFollow = 0.f;
		/** 놓는 중 남은 당김(cm) — 줄이 모자랄 때 가장 작은 것을 내준다. */
		float Remaining = 0.f;
		/** 잡은 점의 지금 피부 자리(프레임 cm). */
		FVector3f GrabbedFrame = FVector3f::ZeroVector;
	};
	FGrabSlot GrabSlots[MaxGrabRows];
	uint32 NextGrabSerial = 1;
	/** 마지막으로 받은 GPU 잡기 읽기 번호(USoftbodySurfaceField::GetGrabState). */
	uint64 GrabReadSerial = 0;

	int32 NumInertiaGroups = 0;
	int32 GroupBoneIndices[MaxInertiaGroups] = {};
	/** 그룹 본의 기본 포즈 컴포넌트 회전 — 살의 무름이 구워진 축. */
	FQuat GroupReferenceRotation[MaxInertiaGroups];
	bool bLoggedInertiaActivity = false;
};
