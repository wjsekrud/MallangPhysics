// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"

class FRHITexture;
class FRHISamplerState;

/**
 * 살층 응답 표(ADR-0037 §3.2): 3-D 살층 경계 요소에서 만든 무차원 표를 렌더 스레드에 둔다. 표 자료는 Tools/contact_response/table.py --emit이 만든
 * SoftbodyContactResponseTable.inl(셰이더 짝은 SoftbodyContactResponse.ush)에 있다.
 *  - 긴 몫 커널 ĝ_long(ρ; v, τ): 3차원 텍스처(x = ρ, y = τ, z = v, R16F, 하드웨어 세 겹 선형) — 압력 행렬·긴 몫 모으기가 칸 쌍마다 읽어 셰이더 상수로
 *    두면 갈래진 읽기라 느리다. 모듈이 처음 쓸 때 한 번 만든다(켜기·끄기·스폰에 새 할당이 없다).
 *  - 가는 창 폭 ℓ₁/h: C++는 함몰 체비쇼프 범위·거친 패스 수에 가장 넓은 폭만 읽는다.
 */
namespace SoftbodyContactResponse
{
	/** 긴 몫 커널 표 텍스처(렌더 스레드). */
	FRHITexture* GetLongTexture();
	/** 세 겹 선형, 가장자리 고정. */
	FRHISamplerState* GetLongSampler();
	/**
	 * 압력 행렬 성분 양 표 텍스처(렌더 스레드, R32F 3차원 x = 성분, y = 두께, z = ν × 겉껍질 길이 — 일지 289). 압력 칸 목록이 칸마다 한 번 Load로 읽는다.
	 * 모듈이 처음 쓸 때 한 번 만든다.
	 */
	FRHITexture* GetAmountTexture();
	/**
	 * 굽힘(Flex 칠, 연속 굽힘) 성분 양 표(렌더 스레드, R16F 3차원 x = 성분, y = 굽힘 매듭 1..11 × 두께, z = ν × 겉껍질 길이). 매듭 0은 GetAmountTexture
	 * 그대로다(흰색은 지금 표만 읽는다). 표 자료는 table.py --bend가 만든 SoftbodyContactResponseBendTable.inl(base64) — 모듈이 처음 쓸 때 한 번 푼다.
	 */
	FRHITexture* GetBendAmountTexture();
	/** 굽힘 전체 커널 ĝ_full(렌더 스레드, R16F 3차원 x = ρ, y = τ, z = δ 매듭 × v — 세 겹 선형은 GetLongSampler와 같은 것). 거친 해 W_c를 모은다. */
	FRHITexture* GetBendFullTexture();
	/** 표의 가는 창 폭 ℓ₁/h 가운데 τ가 TauMax까지인 칸의 최대(부피 보존 전부). TauMax를 감싸는 표 칸의 위끝까지 본다. */
	float WindowRatioMax(float TauMax);
	/**
	 * 부피 보존 v(0 폼 … 1 살) → 살의 푸아송 비 ν(셰이더 SB_ResponseNu와 같은 곡선). v는 기준 누름(살 L0, 깊이 L0/3인 손)에서 살 대비 둘레로
	 * 솟는 부피 몫이다 — ν와 셀이 접히는 변형률을 같이 다시 맞췄다(Tools/contact_response/volume_refit.py). 측정 굽기가 살 채움의
	 * 푸아송 비로 쓴다 — 굽기·함몰·솟음이 한 곡선을 본다. v 0 → 0, 1 → 0.49.
	 */
	SOFTBODY_API float PoissonOfVolume(float Volume);

	/**
	 * 겉 재료 길이 눈금 L0(메시 로컬). Skin Tension의 장력 길이 L_T = 4 s² × L0와 Flex 판의 굽힘 길이
	 * ℓ_b = 0.6 × L0 × √((1 − S)/S)를 이 고정 길이로 잰다. 전에는 칠한 살 두께의 중앙값이라 한 부위의 Depth 칠이 다른 부위의 겉껍질과 판
	 * 굳기를 바꿨다(원칙 P03). 3은 캐릭터 살 두께 중앙값(2.3~3.6 cm)의 가운데라 캐릭터는 전과 비슷하게 보인다. 메시 로컬 단위라 액터를
	 * 키우면 살과 같이 커져 같은 모습이고, 굽기도 같은 단위로 판 모드를 굽는다(굽기는 컴포넌트 크기를 볼 수 없다).
	 */
	constexpr float ReferenceLength = 3.f;

	/**
	 * 붙은 Flex 조각이 강체 판이 되는 크기 비. 조각의 가장 연한 칠(+ Flex Offset)로 잰 굽힘 길이 ℓ_b가 조각 반지름의 이
	 * 배수 이상이면 강체 판으로 그린다. 굽힘 길이보다 훨씬 작은 조각은 끝없이 단단하지 않아도 한 몸으로 움직인다(질긴 가죽에 박힌 동전) — 굽는
	 * 겉과 강체가 이미 비슷한 자리에서 바뀌어 칠이나 Flex Offset이 문턱을 넘을 때 모양이 덜 튄다. 검정(칠 + Flex Offset ≤ 0)은 크기와 상관없이
	 * 강체다. 값은 3-D 기준(plate_ref)과 재현 두 표현(replica·replica_plate)을 견줘 정했다.
	 * 셰이더 SB_HardKindOf·재현 replica_plate.hard_kind와 짝이다.
	 */
	constexpr float RigidPieceRatio = 1.4f;

	/**
	 * 붙은 조각이 강체가 되는 가장 큰 S(= 칠 + Flex Offset): ℓ_b(S) = 0.6·L0·√((1 − S)/S)가 RigidPieceRatio × 반지름과 같은 S = 1/(1 + q²),
	 * q = RigidPieceRatio × 반지름/(0.6·L0). 반지름은 조각 가운데에서 가장 먼 칸까지(메시 로컬, FSoftbodyHardPiece::Radius) — L0도 메시 로컬이라
	 * 액터 크기와 상관없다.
	 */
	inline float RigidFlexLimit(float PieceRadius)
	{
		const float Q = RigidPieceRatio * FMath::Max(PieceRadius, 0.f) / (0.6f * ReferenceLength);
		return 1.f / (1.f + Q * Q);
	}

	/** 조각 표 틀 셋째 줄 w의 떠 있는 장식 표지. 붙은 조각의 w(가장 큰 칠 − RigidFlexLimit)는 −1 … 1이라 겹치지 않는다(셰이더는 −1.5 밑을 떠 있음으로 읽는다). */
	constexpr float FloatingPieceKind = -2.f;
}
