// [sync_shared_shaders] 자동 생성 사본 — 손으로 고치지 마라. Tools/sync_shared_shaders.py가 UE 원본에서 #include 줄만 바꿔 복사한다.
// [sync_shared_shaders] 원본: Plugins/SoftbodySystem/Shaders/SoftbodyVis.ush
// [sync_shared_shaders] 원본 SHA-256(줄 끝 LF 기준): 758a5b24ce78d63ba972571a72b067b0f0444be0ef1720fcc5dc410971b2e6bc
// [sync_shared_shaders] 원본 git blob SHA-1(git hash-object): 7a2aa03e1be82bd3e5cb5f8fbe50a9dbed2940d7
#ifndef SB_SHARED_SOFTBODYVIS_HLSL
#define SB_SHARED_SOFTBODYVIS_HLSL
#line 1
// 베이크 커버리지 검수 시각화 (베이스 컬러, VisMode로 채널 전환 — 디렉팅/검수용).
// 0: 체적 임베딩 (빨강=실패, 노랑=외삽, 초록=tet 내부 포함)
// 1: 물리 코어 연결 (자홍=미연결/실패, 청록=연결)
// 2: 표면 캐리어 체커 (ADR-0003) — 정점이 읽는 캐리어 점의 아틀라스 좌표를 체커로 칠한다. 같은 자리의
//    층·단추·레이스가 몸과 같은 무늬면 같은 점을 읽는 것이고, 무늬가 끊기는 선이 차트 경계다. 회색 = 캐리어 없음.
// 3: 봉투까지 거리 — 초록=봉투 위, 빨강=바깥으로 멀어짐, 파랑=안쪽으로 깊어짐(AttachRange에서 최대색).
// 4: 관성 무름 (ADR-0005) — 남색=안 흔들림 → 초록 → 빨강=가장 무름, 자홍=당긴 쪽과 반대로 밀림(있으면 베이크 결함).
// 5: 정점 사이 함몰 무게 (ADR-0006) — 초록=삼각형 안까지 표면 장을 읽음(테셀레이션·픽셀 음영), 빨강=캐리어 차트
//    경계라 정점에서만 읽음(지금 화면), 그 사이는 섞임. 옛 베이크는 모두 빨강. 회색 = 캐리어 없음.
//    정점 밀도 무게(일지 74)도 곱해 보인다: 삼각형이 BetweenEdgeTexels 칸 이하로 작은 곳(손·얼굴)은 빨강(퐁 곡면만).
// CarrierUV: 메시 캐리어 UV 채널(ADR-0007, 인스턴스별 차트 UV, 음수 = 없음). 있으면 2의 체커가 이 UV로 칠해져 UV0 이음매
//    양쪽 무늬가 이어지는지 볼 수 있고, 5는 초록(걸친 삼각형 없음)이며 UV0 이음매 정점만 빨강이다(틈 방지).
// 속성 텍스처는 정점 인덱스 테이블 — 반드시 정점 셰이더에서 평가하고 VertexInterpolator로
// 결과 색만 보간해 넘긴다 (MF_SoftbodyDebugVis가 이 규약을 감싼다).
#pragma once

float3 SoftbodyBakeVis(
	float2 TexUV, float2 CarrierUV,
	Texture2D AttrDbgTex, SamplerState AttrDbgTexSampler,
	Texture2D CarrierUVTex, SamplerState CarrierUVTexSampler,
	float VisMode, float AttachRange, float BetweenEdgeTexels)
{
if (VisMode > 4.5)
{
	float4 carrier = CarrierUVTex.SampleLevel(CarrierUVTexSampler, TexUV.xy, 0);
	if (carrier.a < 0.5)
	{
		return float3(0.35, 0.35, 0.35);
	}
	// 표 A − 2 = 무게(0/1) + 4(이음매) + 8 × round(변 길이 칸 수 × 64) — SoftbodySurfaceCarrierVS와 같은 풀이
	float code = carrier.a - 2.0;
	float edgeTexels = code >= 8.0 ? floor(code / 8.0) / 64.0 : 1e4;
	float density = BetweenEdgeTexels > 0.0 ? saturate(edgeTexels / BetweenEdgeTexels - 1.0) : 1.0;
	code = code >= 8.0 ? code - 8.0 * floor(code / 8.0) : code;
	bool seam = code > 3.0;
	float weight = (CarrierUV.x >= 0.0 ? (seam ? 0.0 : 1.0) : saturate(seam ? code - 4.0 : code)) * density;
	return lerp(float3(1.0, 0.1, 0.05), float3(0.1, 0.85, 0.2), weight);
}
if (VisMode > 3.5)
{
	float softness = AttrDbgTex.SampleLevel(AttrDbgTexSampler, TexUV.xy, 0).g;
	if (softness < -0.02)
	{
		return float3(1.0, 0.05, 1.0);
	}
	float t = saturate(softness);
	return t < 0.5
		? lerp(float3(0.05, 0.08, 0.45), float3(0.1, 0.85, 0.2), t * 2.0)
		: lerp(float3(0.1, 0.85, 0.2), float3(1.0, 0.1, 0.05), t * 2.0 - 1.0);
}
if (VisMode > 1.5)
{
	float4 carrier = CarrierUVTex.SampleLevel(CarrierUVTexSampler, TexUV.xy, 0);
	if (carrier.a < 0.5)
	{
		return float3(0.35, 0.35, 0.35);
	}
	if (VisMode < 2.5)
	{
		carrier.xy = CarrierUV.x >= 0.0 ? CarrierUV : carrier.xy;
		float2 cell = floor(carrier.xy * 16.0);
		float checker = fmod(cell.x + cell.y, 2.0);
		float3 hue = frac(float3(carrier.x * 3.1, carrier.y * 2.3, (carrier.x + carrier.y) * 1.7));
		return lerp(0.3, 1.0, checker) * (0.35 + 0.65 * hue);
	}
	float t = saturate(abs(carrier.z) / max(AttachRange, 1e-3));
	return carrier.z >= 0.0
		? lerp(float3(0.05, 1.0, 0.1), float3(1.0, 0.05, 0.05), t)
		: lerp(float3(0.05, 1.0, 0.1), float3(0.1, 0.3, 1.0), t);
}
float4 dbg = AttrDbgTex.SampleLevel(AttrDbgTexSampler, TexUV.xy, 0);
if (VisMode < 0.5)
{
	if (dbg.b < 0.25) return float3(1.0, 0.02, 0.02);
	if (dbg.b < 0.75) return float3(1.0, 0.55, 0.02);
	return float3(0.02, 1.0, 0.08);
}
return dbg.a > 0.5
	? float3(0.02, 0.85, 1.0)
	: float3(1.0, 0.02, 0.75);
}
#endif // [sync_shared_shaders] SB_SHARED_SOFTBODYVIS_HLSL
