// Copyright (c) 2026 wjsekrud

#include "SoftbodyContactResponse.h"

#include "RenderResource.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "Math/Float16.h"

#include "Misc/Base64.h"

#include "SoftbodyContactResponseTable.inl"
#include "SoftbodyContactResponseBendTable.inl"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyContactResponse, Log, All);

namespace
{
	/** 긴 몫 커널 표(R16F 3차원): 반정밀 11비트 가수라 상대 5e-4 — 커널 값 0.1 자리에서 5e-5. */
	class FSoftbodyContactResponseTexture : public FTexture
	{
	public:
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			using namespace SoftbodyContactResponse;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(TEXT("SoftbodyContactResponseLong"),
				FIntVector(RhoCount, TauCount, VCount), PF_R16F).SetFlags(ETextureCreateFlags::ShaderResource);
			TextureRHI = RHICmdList.CreateTexture(Desc);
			TArray<FFloat16> Data;
			Data.SetNumUninitialized(RhoCount * TauCount * VCount);
			for (int32 Index = 0; Index < Data.Num(); ++Index)
			{
				Data[Index] = FFloat16(Long[Index]);
			}
			RHICmdList.UpdateTexture3D(TextureRHI, 0, FUpdateTextureRegion3D(0, 0, 0, 0, 0, 0, RhoCount, TauCount, VCount),
				RhoCount * sizeof(FFloat16), RhoCount * TauCount * sizeof(FFloat16), reinterpret_cast<const uint8*>(Data.GetData()));
			SamplerStateRHI = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}

		virtual uint32 GetSizeX() const override { return SoftbodyContactResponse::RhoCount; }
		virtual uint32 GetSizeY() const override { return SoftbodyContactResponse::TauCount; }
		virtual uint32 GetSizeZ() const override { return SoftbodyContactResponse::VCount; }
	};

	TGlobalResource<FSoftbodyContactResponseTexture> GSoftbodyContactResponseTexture;

	/** 압력 행렬 성분 양 표(R32F 3차원, 일지 289): 양은 1e-12 ~ 11이라 반정밀 아닌 float로 둔다 — 칸 기록에 쓸 때 반정밀로 묶는다(셰이더). */
	class FSoftbodyContactResponseAmountTexture : public FTexture
	{
	public:
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			using namespace SoftbodyContactResponse;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(TEXT("SoftbodyContactResponseAmounts"),
				FIntVector(AmountComponents, AmountThickness, AmountMaterials), PF_R32_FLOAT).SetFlags(ETextureCreateFlags::ShaderResource);
			TextureRHI = RHICmdList.CreateTexture(Desc);
			RHICmdList.UpdateTexture3D(TextureRHI, 0, FUpdateTextureRegion3D(0, 0, 0, 0, 0, 0, AmountComponents, AmountThickness, AmountMaterials),
				AmountComponents * sizeof(float), AmountComponents * AmountThickness * sizeof(float), reinterpret_cast<const uint8*>(Amount));
			SamplerStateRHI = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}

		virtual uint32 GetSizeX() const override { return SoftbodyContactResponse::AmountComponents; }
		virtual uint32 GetSizeY() const override { return SoftbodyContactResponse::AmountThickness; }
		virtual uint32 GetSizeZ() const override { return SoftbodyContactResponse::AmountMaterials; }
	};

	TGlobalResource<FSoftbodyContactResponseAmountTexture> GSoftbodyContactResponseAmountTexture;

	/**
	 * base64 줄들(굽힘 .inl) → R16F 비트. 줄마다 따로 푼다(줄 길이가 4의 배수라 이어 붙인 것과 같다). Count 값보다 짧거나 풀리지 않으면 0으로 채우고 알린다.
	 */
	void DecodeBendHalfs(const char* const* Lines, int32 NumLines, int32 Count, const TCHAR* Name, TArray<FFloat16>& Out)
	{
		Out.SetNumZeroed(Count);
		uint8* Dest = reinterpret_cast<uint8*>(Out.GetData());
		const int64 Bytes = (int64)Count * (int64)sizeof(FFloat16);
		int64 Written = 0;
		bool bOk = true;
		TArray<uint8> Chunk;
		for (int32 Line = 0; Line < NumLines && bOk; ++Line)
		{
			const uint32 Length = (uint32)FCStringAnsi::Strlen(Lines[Line]);
			const uint32 Size = FBase64::GetDecodedDataSize(Lines[Line], Length);
			Chunk.SetNumUninitialized((int32)Size);
			bOk = FBase64::Decode(Lines[Line], Length, Chunk.GetData()) && Written + Size <= Bytes;
			if (bOk)
			{
				FMemory::Memcpy(Dest + Written, Chunk.GetData(), Size);
				Written += Size;
			}
		}
		if (!bOk || Written != Bytes)
		{
			// 개발: 표는 플러그인 소스에 들어 있다 — 개발 저장소에서는 Tools/contact_response/table.py --bend로 다시 만든다.
			UE_LOG(LogSoftbodyContactResponse, Error, TEXT("%s: bending table is damaged (%lld of %lld bytes). Reinstall the plugin."),
				Name, Written, Bytes);
			Out.SetNumZeroed(Count);
		}
	}

	/** 값 합(시작 로그 — table.py가 같은 합을 알린다, 표가 바르게 풀렸는지 확인용). */
	double SumOfHalfs(const TArray<FFloat16>& Values)
	{
		double Sum = 0.0;
		for (const FFloat16& Value : Values)
		{
			Sum += (double)Value.GetFloat();
		}
		return Sum;
	}

	/** 굽힘 성분 양 표(R16F 3차원, 연속 굽힘): 매듭 1..11 — 매듭 0은 지금 성분 양 표(R32F). 압력 칸 기록이 어차피 반정밀이라 반정밀로 둔다. */
	class FSoftbodyContactResponseBendAmountTexture : public FTexture
	{
	public:
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			using namespace SoftbodyContactResponseBend;
			const int32 SizeY = AmountSlices * AmountThickness;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(TEXT("SoftbodyContactResponseBendAmounts"),
				FIntVector(AmountComponents, SizeY, AmountMaterials), PF_R16F).SetFlags(ETextureCreateFlags::ShaderResource);
			TextureRHI = RHICmdList.CreateTexture(Desc);
			TArray<FFloat16> Data;
			DecodeBendHalfs(AmountBase64, AmountChunks, AmountValues, TEXT("SoftbodyContactResponseBendAmounts"), Data);
			RHICmdList.UpdateTexture3D(TextureRHI, 0, FUpdateTextureRegion3D(0, 0, 0, 0, 0, 0, AmountComponents, SizeY, AmountMaterials),
				AmountComponents * sizeof(FFloat16), AmountComponents * SizeY * sizeof(FFloat16), reinterpret_cast<const uint8*>(Data.GetData()));
			SamplerStateRHI = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			UE_LOG(LogSoftbodyContactResponse, Log, TEXT("SoftbodyContactResponseBendAmounts: %d values, sum %.6f"), Data.Num(), SumOfHalfs(Data));
		}

		virtual uint32 GetSizeX() const override { return SoftbodyContactResponseBend::AmountComponents; }
		virtual uint32 GetSizeY() const override { return SoftbodyContactResponseBend::AmountSlices * SoftbodyContactResponseBend::AmountThickness; }
		virtual uint32 GetSizeZ() const override { return SoftbodyContactResponseBend::AmountMaterials; }
	};

	TGlobalResource<FSoftbodyContactResponseBendAmountTexture> GSoftbodyContactResponseBendAmountTexture;

	/** 굽힘 전체 커널 ĝ_full(R16F 3차원 x = ρ, y = τ, z = δ 매듭 × v): 긴 몫 표와 같은 반정밀·세 겹 선형. */
	class FSoftbodyContactResponseBendFullTexture : public FTexture
	{
	public:
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			using namespace SoftbodyContactResponseBend;
			const int32 SizeZ = FullDelta * FullV;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(TEXT("SoftbodyContactResponseBendFull"),
				FIntVector(FullRho, FullTau, SizeZ), PF_R16F).SetFlags(ETextureCreateFlags::ShaderResource);
			TextureRHI = RHICmdList.CreateTexture(Desc);
			TArray<FFloat16> Data;
			DecodeBendHalfs(FullBase64, FullChunks, FullValues, TEXT("SoftbodyContactResponseBendFull"), Data);
			RHICmdList.UpdateTexture3D(TextureRHI, 0, FUpdateTextureRegion3D(0, 0, 0, 0, 0, 0, FullRho, FullTau, SizeZ),
				FullRho * sizeof(FFloat16), FullRho * FullTau * sizeof(FFloat16), reinterpret_cast<const uint8*>(Data.GetData()));
			SamplerStateRHI = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			UE_LOG(LogSoftbodyContactResponse, Log, TEXT("SoftbodyContactResponseBendFull: %d values, sum %.6f"), Data.Num(), SumOfHalfs(Data));
		}

		virtual uint32 GetSizeX() const override { return SoftbodyContactResponseBend::FullRho; }
		virtual uint32 GetSizeY() const override { return SoftbodyContactResponseBend::FullTau; }
		virtual uint32 GetSizeZ() const override { return SoftbodyContactResponseBend::FullDelta * SoftbodyContactResponseBend::FullV; }
	};

	TGlobalResource<FSoftbodyContactResponseBendFullTexture> GSoftbodyContactResponseBendFullTexture;

	// 굽힘 표와 지금 표가 같은 축을 쓰는가(table.py가 두 .inl을 같이 만든다 — 손으로 한쪽만 바꾸면 여기서 멈춘다).
	static_assert(SoftbodyContactResponseBend::AmountComponents == SoftbodyContactResponse::AmountComponents, "bending amounts: component count");
	static_assert(SoftbodyContactResponseBend::AmountThickness == SoftbodyContactResponse::AmountThickness, "bending amounts: thickness knots");
	static_assert(SoftbodyContactResponseBend::AmountMaterials == SoftbodyContactResponse::AmountMaterials, "bending amounts: material knots");
	static_assert(SoftbodyContactResponseBend::FullTau == SoftbodyContactResponse::TauCount && SoftbodyContactResponseBend::FullV == SoftbodyContactResponse::VCount,
		"bending full kernel: v and tau axes");
}

FRHITexture* SoftbodyContactResponse::GetLongTexture()
{
	return GSoftbodyContactResponseTexture.TextureRHI;
}

FRHISamplerState* SoftbodyContactResponse::GetLongSampler()
{
	// 쓸 때 가져온다: 플러그인이 PostConfigInit에 실려 표의 InitRHI가 엔진 고정 샘플러보다 먼저 돌 수 있다(5.6에서 null → Full 첫 갱신 멈춤).
	return TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
}

FRHITexture* SoftbodyContactResponse::GetAmountTexture()
{
	return GSoftbodyContactResponseAmountTexture.TextureRHI;
}

FRHITexture* SoftbodyContactResponse::GetBendAmountTexture()
{
	return GSoftbodyContactResponseBendAmountTexture.TextureRHI;
}

FRHITexture* SoftbodyContactResponse::GetBendFullTexture()
{
	return GSoftbodyContactResponseBendFullTexture.TextureRHI;
}

float SoftbodyContactResponse::PoissonOfVolume(float Volume)
{
	const float X = FMath::Clamp(Volume, 0.f, 1.f);
	float Nu = NuNu[0];
	for (int32 K = 1; K < NuCount; ++K)
	{
		if (X >= NuV[K - 1])
		{
			Nu = FMath::Lerp(NuNu[K - 1], NuNu[K], FMath::Clamp((X - NuV[K - 1]) / (NuV[K] - NuV[K - 1]), 0.f, 1.f));
		}
	}
	return Nu;
}

float SoftbodyContactResponse::WindowRatioMax(float TauMax)
{
	// τ 축: 0, 1/16, 1/8, … 8(셰이더 SB_ResponseTauIndex). 표 사이는 선형이라 TauMax를 감싸는 칸의 위끝까지 보면 그 안의 폭을 다 덮는다
	// — 칸 T는 바로 아래 칸의 τ가 TauMax보다 작으면 든다.
	float Best = 0.f;
	for (int32 V = 0; V < VCount; ++V)
	{
		for (int32 T = 0; T < TauCount; ++T)
		{
			const float TauBelow = T <= 1 ? 0.f : FMath::Pow(2.f, (float)T - 6.f);
			if (T == 0 || TauBelow < TauMax - 1e-6f)
			{
				Best = FMath::Max(Best, WindowLength[V * TauCount + T]);
			}
		}
	}
	return Best;
}
