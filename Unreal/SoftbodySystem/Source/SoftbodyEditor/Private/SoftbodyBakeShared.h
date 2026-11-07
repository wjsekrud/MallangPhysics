// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "Materials/Material.h"

class UMaterialInterface;
class USkeletalMesh;
class UTexture2D;
class USoftbodyMeshData;
struct FMeshDescription;
struct FSoftbodyMeasureParams;
struct FSoftbodyPaintSlotMask;
struct FSoftbodySeparatedPair;
enum class ESoftbodyMaskSource : uint8;
namespace UE::Geometry { class FDynamicMesh3; }

/**
 * 베이크 파일들이 함께 쓰는 헬퍼. 정의는 SoftbodyBakeLibrary.cpp에 있다.
 */
namespace SoftbodyBakeShared
{
	/** 머티리얼 사용 플래그를 켠다(5.6·5.7은 다시 컴파일 여부를 돌려받는 옛 함수). */
	inline void SetMaterialUsage(UMaterial* Material, EMaterialUsage Usage)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, Usage);
#else
		Material->SetMaterialUsage(Usage);
#endif
	}

	/** RGBA16F 비압축·밉 없음·스트리밍 안 함 텍스처를 OutputFolder/Name에 만든다(같은 이름이면 갱신). */
	UTexture2D* CreateTexture16F(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<FFloat16Color>& Texels);

	/** 위와 같되 RGBA32F. 위치처럼 하프 정밀도가 모자란 값에 쓴다. */
	UTexture2D* CreateTexture32F(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<FLinearColor>& Texels);

	/** 위와 같되 한 채널 8비트(G8, 0~255 = 0~1). 부피 보존처럼 0~1 값 하나면 되는 칸에 쓴다(RGBA16F의 1/8). */
	UTexture2D* CreateTextureG8(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<uint8>& Texels);

	/** 위와 같되 한 채널 16비트(G16, 0~65535 = 0~1). 솟음 랜드마크 거리표(ADR-0031)처럼 정수 하나를 정확히 담을 칸에 쓴다. */
	UTexture2D* CreateTextureG16(const FString& OutputFolder, const FString& Name,
		int32 Width, int32 Height, const TArray<uint16>& Texels);

	/** 베이크 텍스처 원본(RGBA32F / RGBA16F, 앞쪽 NumTexels칸)을 읽는다. 정의는 SoftbodyCarrierBake.cpp. */
	bool ReadTextureSource32F(UTexture2D* Texture, int32 NumTexels, TArray<FLinearColor>& Out);
	bool ReadTextureSource16F(UTexture2D* Texture, int32 NumTexels, TArray<FFloat16Color>& Out);

	/**
	 * 정점 하나의 흔들림을 끄는 본 묶음(관성 그룹)과 몫. 스킨 웨이트를 묶음별로 더해 큰 것 셋을 남기고 셋의 합으로 나눈 값이다.
	 * 둘만 남기면 묶음 셋이 비슷하게 걸린 살(가슴 덩어리 + 가슴뼈 본 + 허리 본)에서 둘째와 셋째가 자리를 바꾸는 선을 따라 몫이
	 * 끊겨, 덩어리가 처지거나 흔들릴 때 그 선에서 살이 어긋난다(일지 130).
	 * 표(Attr2 텍셀)에 적는 자리: R = 첫째 묶음, A = 첫째의 몫, B = 둘째 묶음 + 16 × 셋째 묶음, G = 셋째의 몫(둘째의 몫 = 나머지).
	 * 묶음을 둘만 적던 전 베이크(G = 0, B = 둘째 묶음)도 같은 식으로 읽힌다. 셰이더는 SoftbodyKernel.ush가 같은 자리로 읽는다.
	 */
	struct FVertexGroups
	{
		static constexpr int32 Num = 3;
		int32 Group[Num] = {0, 0, 0};
		float Weight[Num] = {1.f, 0.f, 0.f};

		FLinearColor Encode() const
		{
			return FLinearColor((float)Group[0], Weight[2], (float)(Group[1] + 16 * Group[2]), Weight[0]);
		}

		static FVertexGroups Decode(const FLinearColor& Texel)
		{
			FVertexGroups Out;
			const int32 Packed = FMath::Max(FMath::RoundToInt32(Texel.B), 0);
			Out.Group[0] = FMath::RoundToInt32(Texel.R);
			Out.Group[1] = Packed % 16;
			Out.Group[2] = Packed / 16;
			Out.Weight[0] = FMath::Clamp(Texel.A, 0.f, 1.f);
			Out.Weight[2] = FMath::Clamp(Texel.G, 0.f, 1.f - Out.Weight[0]);
			Out.Weight[1] = 1.f - Out.Weight[0] - Out.Weight[2];
			return Out;
		}

		/**
		 * 이 정점의 덩어리: 덩어리 묶음 가운데 몫이 가장 큰 것(같은 묶음이 여러 자리에 있으면 합치고, 몫이 같으면 앞자리).
		 * 없으면 INDEX_NONE. 셰이더가 덩어리 몫을 뺄 때(SoftbodyInertiaLagExcept) 같은 규칙으로 고른다.
		 */
		template <typename IsLumpType>
		int32 PickLump(IsLumpType&& IsLump, float& OutWeight) const
		{
			int32 Best = INDEX_NONE;
			OutWeight = 0.f;
			for (int32 k = 0; k < Num; ++k)
			{
				bool bFirst = true;
				float Sum = 0.f;
				for (int32 j = 0; j < Num; ++j)
				{
					bFirst = bFirst && !(j < k && Group[j] == Group[k]);
					Sum += Group[j] == Group[k] ? Weight[j] : 0.f;
				}
				if (bFirst && Sum > OutWeight && IsLump(Group[k]))
				{
					Best = Group[k];
					OutWeight = Sum;
				}
			}
			return Best;
		}
	};

	/**
	 * 본별 굵기 표(본 이름 → 굵기 비율, 저작 에셋 Bone Overrides → Mesh Data RigidShrinkPerBone)에서 뼈대에서 뺀 본의 값. 0 이하면 뺀 본이고,
	 * 둘은 아래 본이 같이 빠지는지만 다르다(일지 236: Include Child Bones). 그 본만 빼면 그 위아래 선만 빠지고 아래 본은 뼈로 남는다.
	 */
	inline constexpr float BoneExcludedWithChildren = -1.f;
	inline constexpr float BoneExcludedAlone = -2.f;
	/**
	 * 장식 본(Bone Overrides의 Accessory): 뼈대에서 빠지는 것은 덩어리와 같지만 덩어리가 아니고, 제 관성 묶음도 갖지 않는다 — 그
	 * 정점은 부모 쪽 묶음을 따른다. 리본·목걸이 본의 구가 그 밑 살의 두께와 출렁임 고정점을 차지하던 것을 없앤다.
	 */
	inline constexpr float BoneAccessoryWithChildren = -3.f;
	inline constexpr float BoneAccessoryAlone = -4.f;

	/** 뼈대에서 빠지는 값인가(덩어리와 장식 모두, 옛 굽기의 0 이하 값도). */
	inline bool IsBoneRemoved(float Value) { return Value <= 0.f; }
	/** 아래 본까지 같이 빠지는가. */
	inline bool IsBoneRemovedBelow(float Value) { return Value <= 0.f && Value != BoneExcludedAlone && Value != BoneAccessoryAlone; }
	/** 장식 본인가. 뼈대에서 빠진 값 가운데 장식이 아닌 것이 덩어리다. */
	inline bool IsBoneAccessory(float Value) { return Value == BoneAccessoryWithChildren || Value == BoneAccessoryAlone; }
	inline bool IsBoneLump(float Value) { return IsBoneRemoved(Value) && !IsBoneAccessory(Value); }

	/**
	 * 뼈대의 본 선을 레퍼런스 포즈 공간 구 사슬로 바꾼다(스켈레탈 측정 베이크의 단단한 속과 같은 규칙, ADR-0026).
	 * 굵기 = 선에서 살갗까지 거리 × RadiusScale. PerBoneScale: 본 이름 → 굵기 비율(BoneExcludedWithChildren = 그 본과 아래 본을 뺌,
	 * BoneExcludedAlone = 그 본만 뺌). 미기재 본은 RadiusScale.
	 * VolumeMD: 속 메시(ADR-0028). 있으면 살갗까지 거리와 안팎을 이 메시에서 잰다.
	 * FleshSlots: 살 슬롯(일지 239, CoreSlotNames). 비어 있지 않으면 이 슬롯의 정점을 움직이는 본만 뼈로 친다 — 머리카락 가닥처럼 다른 슬롯만
	 *   움직이는 본은 그 속에 구를 두지 않는다(가닥이 가슴 위에 얹혀 있으면 그 구가 가슴의 살 두께와 출렁임 고정점이 되었다). 끝 본의 길이도
	 *   이 슬롯의 정점으로 잰다. 비어 있거나 nullptr이면 메시 전체(전과 같음).
	 */
	int32 CollectCoreSpheres(USkeletalMesh* Mesh, TArray<FVector4>& OutSpheres, float RadiusScale,
		const TMap<FName, float>* PerBoneScale = nullptr, const FMeshDescription* VolumeMD = nullptr, const TSet<FName>* FleshSlots = nullptr);

	/**
	 * 살 슬롯(일지 239): 소프트바디 머티리얼이 붙은 슬롯 + Extra(곧 소프트바디 머티리얼을 받을 슬롯 — 저작 에셋 Softbody Material Slots, 첫 굽기).
	 * 편집용 이름과 임포트 이름 둘 다 담는다(메시 설명의 폴리곤 그룹은 임포트 이름). 비면 메시 전체를 뜻한다(머티리얼을 입히기 전 첫 굽기).
	 * 정의는 SoftbodyBakeLibrary.cpp.
	 */
	TSet<FName> CoreSlotNames(const USkeletalMesh* Mesh, TConstArrayView<FName> Extra = {});

	/**
	 * 스태틱 메시의 단단한 속을 메시 모양에서 만든다(저작 에셋 Core Placement, 일지 231).
	 * bFloor = false (Center): 모양의 한가운데를 따라 구를 놓는다. 한가운데 = 가장 가까운 겉이 이웃 점과 반대쪽으로 바뀌는 안쪽 점
	 *   (판이면 가운데 면, 공이면 중심, 소시지면 축). 반지름 = 그 점에서 겉까지 거리 × RadiusScale — 스켈레탈의 본 선과 같은 규칙(ADR-0026).
	 * bFloor = true (Bottom): 가장 낮은 곳에서 측정 칸 절반 위까지가 윗면인 아주 큰 구 하나 — 바닥에 놓인 물체(일지 230).
	 * MD: 살을 채우는 메시(속 메시가 있으면 그 메시). CellSizeFrac: 측정 베이크와 같은 칸 비율.
	 * @return 놓은 구의 수(0 = 메시에 속이 없음)
	 */
	int32 CollectStaticCoreSpheres(const FMeshDescription& MD, bool bFloor, float CellSizeFrac, float RadiusScale,
		TArray<FVector4>& OutSpheres);

	/**
	 * 뼈대의 본 선과 Core Placement Center가 만든 속의 구 반지름 하한(측정 칸 단위). 손가락·가는 목처럼 격자 한 칸보다 가는 속이
	 * 격자 점 사이로 토막 나지 않게 한다. 측정 굽기와 SA 창의 미리보기가 같은 값을 쓴다.
	 */
	inline constexpr float CoreMinRadiusCells = 0.6f;

	/**
	 * 소스 메시의 winding number 0.5 등위면(몸의 바깥 봉투)을 뽑는다. 열린 셸(옷 조각, 머리카락 카드)은 들어오지 않고
	 * 작은 조각은 버린다. TargetTriangles > 0이면 그 수로 줄이고, SmoothIterationCount번 라플라시안 평활한다.
	 * 정의는 SoftbodyCarrierBake.cpp (캐리어 베이크와 같은 규칙).
	 */
	bool ExtractSolidEnvelope(const UE::Geometry::FDynamicMesh3& Source, double CellSize, int32 TargetTriangles,
		int32 SmoothIterationCount, UE::Geometry::FDynamicMesh3& OutEnvelope);

	/**
	 * 소프트바디 커널이 붙은 머티리얼인가(캐리어 UV를 읽는 슬롯 = 표면 장 변위가 실제로 보이는 슬롯).
	 * 정의는 SoftbodyCarrierBake.cpp.
	 */
	bool ReadsCarrier(const UMaterialInterface* Material);

	/**
	 * 메시의 소프트바디 머티리얼이 이 베이크의 UV 채널을 읽는가: MF_Softbody* 호출의 TexelUV·CarrierUV 입력에 물린 TexCoord 번호를
	 * 데이터의 채널과 견준다. 어긋난 곳마다 오류를 남기고 그 수를 돌려준다(Owner = 로그 앞머리). 같은 마스터 머티리얼을 채널이 다른
	 * 두 메시가 같이 쓰면 여기서 잡힌다. 정의는 SoftbodyCarrierBake.cpp.
	 */
	int32 CheckMaterialChannels(const class USoftbodyMeshData* Data, const UObject* Mesh, const TCHAR* Owner);

	/** 굽기가 고쳐도 되는 에셋인가: 엔진 콘텐츠와 엔진 플러그인 콘텐츠는 아니다(프로젝트로 복제해서 굽는다). */
	bool IsEditableContent(const UObject* Asset);

	/**
	 * 정적 라이팅의 라이트맵 UV를 지킨다. 메시가 라이트맵 UV를 빌드 때 생성하고 그 채널이 소프트바디 채널(OurChannels)과 겹치면,
	 * 생성 채널을 소프트바디 채널 뒤(NumChannels = 메시 설명의 채널 수)로 옮긴다. 남은 채널이 없으면(MaxChannels) 생성을 끄고 경고한다.
	 * 전에는 생성을 늘 껐다 — 정적 라이팅을 쓰는 프로젝트에서 라이트맵 UV가 사라졌다.
	 */
	void KeepLightmapUVsFree(class UStaticMesh* Mesh, int32 NumChannels, TConstArrayView<int32> OurChannels, int32 MaxChannels);

	/** LOD 수(스태틱 = 소스 모델 수). */
	int32 NumLods(const UObject* Mesh);

	/** LOD Lod가 제 메시 설명을 가진 LOD(직접 들여온 LOD)면 그 설명. LOD0에서 줄여 만드는 LOD는 LOD0의 채널을 물려받으므로 nullptr. */
	FMeshDescription* OwnLodDescription(UObject* Mesh, int32 Lod);

	/**
	 * 들여온 LOD(1 이상)에 텍셀 UV 채널을 쓴다: 정점마다 가장 가까운 LOD0 정점의 칸. 전에는 LOD0만 구워, 다른 LOD로 바뀌면 이 채널이
	 * 없어 모든 정점이 칸 0을 읽었다. 그 채널을 LOD가 제 데이터로 쓰고 있으면 건드리지 않고 경고한다. 쓴 LOD를 커밋한다(렌더 데이터
	 * 빌드는 호출자). 쓴 LOD 수를 돌려준다. BaseVertTexel = LOD0 정점 번호 → 칸(없으면 INDEX_NONE).
	 */
	int32 WriteLodTexelChannels(UObject* Mesh, const FMeshDescription& BaseMD, TConstArrayView<int32> BaseVertTexel, int32 TexSize,
		int32 TexelChannel, int32 MaxChannels);

	/** 들여온 LOD(1 이상)에 캐리어 UV 채널을 (−1, −1)로 채운다(그 LOD는 정점 표로 읽는다). 쓴 LOD를 커밋한다. */
	void WriteLodCarrierChannels(UObject* Mesh, int32 CarrierChannel, int32 MaxChannels);

	/** 칠 네 종류(일지 298, ADR-0038 — 원칙 P08의 고정 배치). 값은 칠 텍스처의 채널 순서(R G B A)와 같다. */
	enum class EPaintKind : uint8
	{
		Depth = 0,      // R — 살 두께(뼈는 여기에만)
		Jiggle = 1,     // G — 살의 무름
		Volume = 2,     // B — 압축성
		Softness = 3,   // A — 겉 굽힘(판)
	};
	constexpr int32 NumPaintKinds = 4;

	/** 칠 종류의 디테일 패널 이름(로그·경고 문장). */
	const TCHAR* PaintKindName(EPaintKind Kind);

	/** 측정 굽기가 읽은 칠(Depth·Jiggle·Volume 정점 값, 떠 있는 판 조각)의 지문(USoftbodyMeshData::MeasuredPaintHash). 정의는 SoftbodyBakeLibrary.cpp. */
	uint32 MeasurePaintHashOf(const ::FSoftbodyMeasureParams& Params);

	/**
	 * 지금 슬롯 칠로 측정 굽기를 하면 나올 칠 지문(측정과 같은 읽기, 로그 없음). Reapply Paint가 Run Bake로 돌지, Validate가 측정이 옛 칠인지 본다.
	 * Mesh = 대상 메시(스켈레탈·스태틱). 정의는 SoftbodyBakeLibrary.cpp.
	 */
	uint32 HashMeasuredPaint(const UObject* Mesh, const TMap<FName, FSoftbodyPaintSlotMask>& Masks);

	/**
	 * 칠 한 종류를 실제로 어디서 읽는가 — 고정 배치와 그 채널의 Fill에서 정한다(채널 고르기 없음). Fill = Paint Texture면 그 종류의 고정 채널,
	 * 슬롯에 텍스처가 없으면 Keep(= 흰색, 칠 안 함). White everywhere = One, Black everywhere = Zero. 굽기·측정·조각 찾기·미리보기가 같이 쓴다.
	 */
	ESoftbodyMaskSource PaintSource(const FSoftbodyPaintSlotMask& Mask, EPaintKind Kind);

	/**
	 * 저작 에셋 미리보기의 칠 보기: 칠 한 종류를 메시 설명 정점마다(정점 ID 자리) 굽기와 같은 함수로 읽는다.
	 * OutPainted = 그 정점이 그 칠을 텍스처·상수로 받은 슬롯에 있는가(아니면 기본값 1). 로그는 남기지 않는다. 정의는 SoftbodyBakeLibrary.cpp.
	 */
	TArray<float> SamplePaintKind(const UObject* Mesh, const FMeshDescription& MD, const TMap<FName, FSoftbodyPaintSlotMask>& Masks,
		EPaintKind Kind, TArray<bool>& OutPainted);

	/**
	 * 텍스처 채널(R·G·B·A)을 읽을 칠인데 그 슬롯에서 텍스처 픽셀을 읽지 못했으면(bTextureRead = false) Keep으로 읽는다: 칠하지 않은 슬롯과 같은
	 * 흰색 1(Softness도 흰색 = 맨살 — 일지 298부터 예외 없음). 같은 슬롯의 상수(White·Black)와 다른 칠은 그대로 든다.
	 */
	ESoftbodyMaskSource EffectiveMaskSource(ESoftbodyMaskSource Source, bool bTextureRead);

	/** 텍스처 채널(R·G·B·A)을 읽는 소스인가. Keep·White·Black은 텍스처가 필요 없다. */
	bool IsTextureMaskSource(ESoftbodyMaskSource Source);

	/** 굽기가 이 마스크 텍스처의 픽셀을 읽을 수 있는가(원본 데이터가 있는가). */
	bool CanReadMaskTexture(const UTexture2D* Texture);

	/**
	 * 슬롯 마스크가 고른 대로 읽히지 않는 곳을 사용자 문장(영어, 한 줄씩)으로 적는다. 칠 단계·Validate·미리보기가 같이 쓴다.
	 * OutUnread: 텍스처 채널을 고른 칠인데 그 슬롯에 텍스처가 없거나 원본 픽셀이 없어 칠하지 않은 슬롯처럼 읽히는 곳(EffectiveMaskSource와 같은 판정).
	 * OutChanged: 읽을 때 값이 바뀌는 텍스처 — sRGB가 켜져 있으면 굽기가 색으로 풀어 읽어 회색 0.5가 0.21이 된다(읽는 방식은 그대로 두고 알리기만).
	 *   Paint Texture로 읽는 채널이 텍스처 전체에서 0이면(쓰지 않는 채널을 0으로 둔 텍스처 — 조용히 뼈·폼·판이 된다) 여기에 적는다.
	 * Kind: 있으면 그 칠 하나만 본다(칠 보기). 없으면 네 칠 모두.
	 */
	void DescribeSlotMaskProblems(const TMap<FName, FSoftbodyPaintSlotMask>& Masks, TArray<FString>& OutUnread, TArray<FString>& OutChanged,
		TOptional<EPaintKind> Kind = {});

	/**
	 * 저작 에셋 칠 단계: USoftbodyBakeLibrary::GeneratePaintTextureSlots와 같되, 출렁임 기준(칠한 살 가운데 가장 무른 점 = 1)을 정할 흔들리는
	 * 섹션에 PendingSlots(같은 Run Bake의 머티리얼 단계가 곧 소프트바디 머티리얼을 입힐 슬롯 이름)도 센다. 그래야 첫 Run Bake가 두 번째와 같은
	 * 기준을 쓴다. 정의는 SoftbodyBakeLibrary.cpp.
	 */
	UTexture2D* GeneratePaintForSlots(USoftbodyMeshData* Data, const TMap<int32, FSoftbodyPaintSlotMask>& SlotMasks,
		TConstArrayView<FName> PendingSlots);

	/**
	 * 판 조각 찾기(ADR-0037 §3.7, 일지 298부터 Softness 칠 — ADR-0038 §3.5). Softness가 세 코너(UV0) 모두 문턱(SoftbodyHardSurface::PieceThreshold)
	 * 보다 작은 삼각형이 판이고(흰색만 맨살, 회색은 휘는 판, Black everywhere면 슬롯 전체), 같은 자리 정점(용접 — UV 이음매에서 갈라진 사본도
	 * 같은 점)으로 이어진 판 삼각형 무리 하나가 조각 하나다.
	 * 떠 있는 조각 = 단단하지 않은 삼각형과 같은 자리 정점을 나누지 않는 조각(다른 겉면 위에 따로 놓인 장식). 주사위 눈처럼 겉면에 칠한
	 * 조각은 둘레 살과 용접돼 떠 있지 않다. 측정 굽기(떠 있는 조각을 살갗으로 읽지 않음)와 칠 단계(접촉 지도 굽기로 넘김)가 같이 쓴다.
	 * SlotAliases = 메시 설명 폴리곤 그룹 이름(들여올 때의 슬롯 이름) → 슬롯 마스크 키(슬롯 이름). 돌려줌: 조각 수.
	 * OutTrianglePieces = 메시 설명 삼각형 자리(FTriangleID 값, GetArraySize 크기)마다 조각 번호(0 = 단단하지 않음, 1부터).
	 * OutFloating = 조각마다(번호 − 1) 떠 있는가. OutHash = 조각 소속과 판 코너 Softness의 지문(측정 굽기와 칠 단계가 같은 칠로 구웠는지
	 * 견준다 — 회색만 바꿔도 Reapply Paint가 Run Bake로 돈다). OutTriangleSoftness(있으면) = 메시 설명 삼각형 자리 × 3 + 코너마다 Flex(모든 삼각형 —
	 * Flex를 읽지 않는 슬롯은 1, 판이 없어도 흰색보다 어두운 코너가 있으면 채운다) — 접촉 지도 굽기가 모든 칸의 Flex(연속 굽힘)와 판 칸 값을 여기서 낸다.
	 * 정의는 SoftbodyBakeLibrary.cpp.
	 */
	int32 FindHardPieces(const FMeshDescription& MD, const TMap<FName, FSoftbodyPaintSlotMask>& Masks, const TMap<FName, FName>& SlotAliases,
		TArray<int32>& OutTrianglePieces, TArray<uint8>& OutFloating, uint32& OutHash, bool bLog = true,
		TArray<float>* OutTriangleSoftness = nullptr);

	/**
	 * 옛 배치(일지 298 전) 슬롯 칠 하나를 고정 배치(R Depth · G Jiggle · B Volume · A Softness)로 옮긴다 — 이관 전용(ADR-0038 §3.8, 시연 이관 뒤
	 * 지운다). 옛 매핑대로 새 RGBA 텍스처 `<옛 텍스처 이름>_Paint`(sRGB 끔)를 옛 텍스처 옆에 만들고 Fill을 정한다: R ← 옛 Press Depth, G ← 옛 Flesh
	 * Softness(없으면 옛 Jiggle Amount — 뜻이 다른 근사), B ← 옛 Volume Preservation, A ← 1 − 옛 Hard Surface(그 텍스처를 같은 UV0로 다시 표본).
	 * 옛 Skin Tension은 버린다. 상수 채널은 Fill로. OutNotes에 사용자에게 알릴 줄을 더한다. 옛 필드는 기본값으로 돌린다. 정의는 SoftbodyBakeLibrary.cpp.
	 */
	void ConvertLegacySlotMask(FSoftbodyPaintSlotMask& Mask, FName Slot, TArray<FString>& OutNotes);

	/** 칠한 조각이 판이 되지 않고 살로 남은 까닭(USoftbodyMeshData::HardPieceResults의 음수, 접촉 지도 굽기가 적고 미리보기가 읽는다). */
	constexpr int32 HardDroppedWrapped = -1;     // 몸을 둘러 감겼다(평평하지 않다)
	constexpr int32 HardDroppedNoSurface = -2;   // 밑에 누를 수 있는 겉면 칸이 없다
	constexpr int32 HardDroppedTooLarge = -3;    // 발자국 거친 칸이 SoftbodyPlateBake::MaxCells보다 많다(판 모드를 자르지 않고 뺀다)
	constexpr int32 HardDroppedAtlasEdge = -4;   // 접촉 지도 가장자리 두 거친 칸 안이라 판을 거친 마디에서 읽지 못한다

	/** 메시의 슬롯 이름 대응표(들여올 때의 이름 → 슬롯 이름) — 슬롯 마스크 키와 메시 설명 폴리곤 그룹을 잇는다. 정의는 SoftbodyBakeLibrary.cpp. */
	TMap<FName, FName> SlotAliasesOf(const UObject* Mesh);

	/**
	 * 메시가 실제로 그리는 노멀, 메시 설명 정점 인스턴스 번호마다(GetArraySize 크기, 빈 번호는 0). LOD 0 빌드 설정이 Recompute Normals면
	 * 렌더는 저장된 노멀을 쓰지 않고 모양에서 다시 계산하므로 여기서도 그렇게 한다 — 저장된 노멀이 엉터리일 수 있다(시연 몸은 들여올 때
	 * 전부 위쪽이었다, 일지 345). 아니면 저장된 노멀 그대로. 정의는 SoftbodyBakeLibrary.cpp.
	 */
	TArray<FVector3f> RenderedNormals(const UObject* Mesh, const FMeshDescription& MD);

	/** Separated Pairs를 본마다 두 묶음으로 나눈 결과(ResolveSeparatedPairs). */
	struct FSeparatedSides
	{
		/** 본마다(레퍼런스 스켈레톤 번호) 0 = 어느 쌍에도 없음, 1·2 = 두 묶음. 적은 본의 자식은 가장 가까운 적은 조상을 따른다. */
		TArray<uint8> BoneSide;
		/** 쓴 쌍의 수. */
		int32 NumPairsUsed = 0;
		/** 사용자에게 보일 문제(영어, 한 줄씩): 없는 본, 빈 쪽, 두 묶음으로 못 나눠 뺀 쌍, 쌍이 아닌데 떨어지게 된 맞닿은 부위. */
		TArray<FString> Problems;
	};

	/**
	 * 부위 나누기(Separated Pairs): 접촉 지도는 두 묶음 몫만 담으므로 모든 쌍을 두 묶음으로 나눈다. 한 쪽의 본은 같은 묶음, 쌍의 두 쪽은 다른
	 * 묶음이다(쌍이 본을 나눠 가지면 이어진다). 앞 쌍들과 모순되는 쌍(셋이 서로 떨어져야 하는 꼴)은 빼고 Problems에 적는다. 이어지지 않은
	 * 쌍 무리 사이에는 어느 쪽을 같은 묶음에 둘지 고를 수 있다 — 메시에서 맞닿은 쪽끼리 같은 묶음이 되게 고른다(쌍이 아닌 맞닿은 부위가 떨어지지
	 * 않게). 그래도 맞닿은 곳이 남으면 Problems에 적는다. 굽기(BakeSeparatedParts)와 저작 에셋 미리보기가 같이 쓴다. 정의는
	 * SoftbodyCarrierBake.cpp.
	 */
	FSeparatedSides ResolveSeparatedPairs(const USkeletalMesh* Mesh, TConstArrayView<FSoftbodySeparatedPair> Pairs);
}
