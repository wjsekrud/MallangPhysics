// [sync_shared_shaders] 자동 생성 사본 — 손으로 고치지 마라. Tools/sync_shared_shaders.py가 UE 원본에서 #include 줄만 바꿔 복사한다.
// [sync_shared_shaders] 원본: Plugins/SoftbodySystem/Shaders/SoftbodySurfaceField.usf
// [sync_shared_shaders] 원본 SHA-256(줄 끝 LF 기준): 8590b4e5f9d85840da4a74331e49f3484d34e6e770daa7a9326c5f9bbd799723
// [sync_shared_shaders] 원본 git blob SHA-1(git hash-object): 946fc663149b2e9e14ce62030fa9992b7521d127
#line 1
// ADR-0003 표면 장 갱신 (글로벌 컴퓨트 셰이더, USoftbodySurfaceField::RenderBatch가 깨어 있는 인스턴스마다 아래 차례로 디스패치). 세 층으로 읽는다:
//   (가) 모델 — 살층 한 식(ADR-0019: 폭은 살 두께와 겉껍질 장력 길이에서, 시간은 복원율 R 하나). 손이 붙든 살의 접촉 압력 하나에 살층의 실제 응답(3-D 살층에서
//        만든 표, ADR-0037)을 입혀 함몰과 둘레 솟음을 한 번에 그린다 — 짧은 몫은 정적 해 x − ℓ₁²∇²x = 0(누른 자리 기억 + 붙든 칸 목표, ADR-0032), 긴 몫은 압력의
//        커널 합. 끌림(붙든 자리 기억 + 원천 커널 합, ADR-0033)도 같은 살층 폭. 뗀 자리는 모두 같은 R로 돌아온다(갱신률·반복 수·해상도에 안 흔들린다).
//   (나) 화면 결함을 막는 규칙 — 가장자리 다듬기·칸 사이 벽(함몰 벽이 격자가 아니라 콜라이더 가장자리를 따른다, ADR-0010 §9·ADR-0029), 제 살 깊이(묻힌 콜라이더가
//        옆면을 같이 파지 않는다, ADR-0003 §16), 이음매 고스트 칸(ADR-0007)과 아틀라스 이웃 가드(옆에 놓였을 뿐 정지 위치가 먼 다른 차트를 이웃으로 읽지 않는다),
//        랜드마크 거리 하한·마주봄 가림(접힌 곳·다른 부위로 새지 않는다), 자기 몸은 모양 거리장으로(ADR-0004), 손이 덩어리를 지나가지 않는 덩어리 평형(ADR-0017).
//   (다) GPU 배관 — 눌리거나 남은 묶음만 도는 목록과 간접 인자, 다 돌아오면 비우고 쉬기(ADR-0013·ADR-0020), 묶음마다 공유 메모리 창, 접촉 압력·끌림·함몰 거친
//        해가 같이 쓰는 거친 격자, 끌림 걸음 조각 나누기, D3D11 제약(패스당 쓰기 버퍼·텍스처 8개, 장벽은 균일한 흐름에만, 두 채널 UAV는 읽지 못함 — ADR-0025).
// 패스 차례(캐리어 아틀라스 칸마다, 갱신 하나):
//   ContactCS : 캐리어 점을 (스켈레탈이면 스키닝해서) 현재 위치로 옮기고 콜라이더(단순 형상 구·캡슐·박스·컨벡스와 모양 거리장 — 자기 몸의 다른 부위 포함) 관통
//               깊이 d를 잰다. 눌린 칸이 콜라이더를 따라 움직인 거리와 마찰 한도를 끌림 거친 칸으로 모은다(ADR-0008). 콜라이더 가장자리 다듬기 폭(`SurfaceEdgeTrim`칸,
//               기본 1.5) 안쪽은 깊이를 줄이고 가장자리 부호 거리를 적는다. 묻힌 콜라이더는 칸마다 구운 "제 살 깊이"까지만 찾는다. 덩어리 칸(ADR-0012)은 덩어리가
//               비켜난 자리에서 깊이를 잰다. 스레드 묶음(8×8칸)마다 칸 점을 감싸는 상자로 먼 형상·부위를 묶음째 건너뛴다(ADR-0013).
//   ContactGroupsCS : 처음 한 번(캐리어가 바뀌면 다시). 표면·고스트 칸이 든 8×8 묶음 목록과 덩어리 칸이 든 묶음 목록 — 접촉·옮겨 두기·합치기·노멀은 앞 목록만,
//               덩어리 힘은 뒤 목록만 돈다(ADR-0013 §8). 빈칸의 출력은 늘 0이라 쓰지 않는다.
//   ActivityArgsCS·CopyCS : 쉬기. 접촉 패스가 적은 "눌린 칸이 있는가"와 지난 갱신 끝에 남은 것으로 뒤 패스들의 디스패치 크기를 정한다 — 눌리지 않고 복원도 끝났으면
//               뒤 패스는 0개 묶음으로 돌아 아무 일도 하지 않는다(GPU가 같은 프레임에 정하므로 새로 닿는 순간 늦지 않는다). CopyCS는 지난 변위를 보관한다(속도 버퍼).
//   TileListCS : 함몰 묶음(16×16칸) 목록 — 이번에 눌렸거나 붙든 자리(기억·받아들인 깊이)가 남은 묶음과 그 둘레 1겹, 이음새 건너편(고스트 칸이 가리키는 묶음, 쌍은
//               TileLinksCS가 처음 한 번). 함몰 거친 해의 거친 묶음 목록과 0으로 비울 묶음 목록도 같이 만든다. 어디에도 눌린 칸이나 남은 값이 없으면 이 패스도 0개
//               묶음으로 돈다(가만히 있을 때 비용이 거의 0). 접촉 압력 칸도 이 목록만 돈다 — 붙든 칸은 다 그 안에 있다.
//   LumpStartCS → (LumpForceCS → LumpSolveCS) × NumLumpSteps : 덩어리 밀림(ADR-0012·ADR-0017). 눌린 덩어리(가슴 등)가 통째로 비켜나는 양을 GPU 뉴턴으로 푼다 —
//               덩어리 칸마다 누르는 힘과 붙은 손이 당기는 힘을 묶음별로 더하고(LumpForceCS) 한 걸음 푼다(LumpSolveCS). 지난 프레임 결과에서 이어 풀고 마지막 걸음을
//               CPU가 비동기로 읽어 다음 프레임의 콜라이더 자리를 옮긴다 — 손이 덩어리를 지나가지 않는다.
//   ContactPressureCellsCS → ContactPressureArgsCS → ContactPressureMatrixCS → ContactPressureSolveCS → ContactLongGatherCS : 접촉 압력(ADR-0037, Contact Detail Full).
//               함몰 목록 묶음의 붙든 칸을 거친 칸(B×B)마다 모아 압력 칸(붙든 넓이·무게 중심·목표 평균)으로 두고, 칸 쌍마다 살층 응답 K(i, j)를 표에서 셈해 한
//               스레드 묶음이 활성 집합 켤레 기울기로 접촉 힘을 푼다(0에서 — 상태 없음, 셀이 접히는 폼은 힘 상한). 그 힘으로 거친 칸마다 긴 몫 L(둘레 솟음)과
//               거친 압력 p를 모은다. 함몰 목표와 합치기가 읽는다. Basic은 돌지 않는다(함몰만).
//   DentCoarseRestrictCS → DentCoarseCS × DentCoarsePasses → DentCoarseAddCS : 함몰 거친 해(일지 199·200). 함몰 폭이 묶음보다 넓으면(두꺼운 폼) 묶음 창만으로는
//               갱신마다 함몰 폭만큼 못 퍼져 묶음 경계가 보인다. 같은 식을 거친 격자(솟음·끌림 격자)에서 먼저 풀고, 가는 묶음은 그 해를 더한 자리에서 출발하며 목록
//               밖은 그 해로 읽는다. 이음매는 거친 칸 링크로 잇고 열린 끝은 자유단이다(거친 격자 한 변이 이웃 표 한도를 넘는 메시만 건너뛴다 — DentCoarseOn 0, 끌림과 같이).
//               바닥은 가는 창과 같은 목표로 정한다(평형에서 거친 해가 밀리지 않는다).
//   DentCS × 2 : 함몰(ADR-0032·ADR-0037). 칸마다 누른 자리 기억 F = max(이번 누른 깊이, F × e^{−RΔt})를 두고, 붙들 깊이 P = max(F, 받아들인 깊이)인 칸을 목표
//               T = P + L − γ·h·A₁·p(긴 몫을 더하고 칸보다 짧은 압축 몫을 거친 압력으로 뺀다)에 붙든 정적 해 x − ℓ₁²∇²x = 0을 묶음 32×32 창(공유 메모리)에서
//               패스 둘로 푼다. 폭 ℓ₁은 응답 표의 (ℓ₁/h)(v, τ) × 두께, 시간은 복원율 R 하나. 셀이 접히는 폼은 붙든 칸의 원천이 상한을 넘지 않는다(투영의 상한).
//               첫 패스가 (목표, 붙들 깊이)를 쓰고 둘째 패스가 기억·받아들인 깊이(보이는 깊이를 따른다)·남은 표지를 제 칸에서 고친다. 부위 나누기가 구워진 장
//               (순열 SB_SEPARATION)은 같은 반복에서 출처 무게별 채널 셋을 같이 풀어, 상태 A에 다른 부위에서 온 꼬리를 뺀 함몰을 쓴다(아래 "부위 나누기").
//   DentCopyCS : 함몰 둘째 패스 결과를 상태 A(다음 갱신의 출발 값이자 뒤 패스가 읽는 곳)로 옮긴다. 고스트 칸은 가리키는 점의 값으로(차트 끝에서 옆 차트와 같은 값),
//               붙든 자리 없이 함몰만 남은 채 목록에서 빠지는 묶음은 0으로 비운다.
//   DragSourceCS → DragCorrectCS × softbody.Drag.CorrectPasses → DragGatherCS : 끌림(피부가 표면을 따라 끌려간 거리, ADR-0008·ADR-0033), 거친 격자 전체. 거친
//               칸마다 붙든 자리 H(누른 동안 콜라이더를 따라 쌓인 자리)와 끄는 힘 Q를 기억한다. 원천 모으기: 계속 누른 칸은 H를 콜라이더 움직임만큼 옮기고 Q = 지난 Q +
//               w(H − 지난 G), 버틸 힘 한도 Q_lim(쿨롱 마찰 한도를 살의 법칙으로 뉴턴 넷)을 넘으면 그 길이로 잘려 미끄러진다. 뗀 칸은 Q가 e^{−RΔt}배. 원천(눌린 칸과
//               기억이 남은 칸)을 묶음째 목록에 넣고 먼 묶음이 읽을 뭉친 원천도 만든다(일지 187). 바로잡기: 붙은 칸의 커널 합 G를 H에 맞추는 걸음을 더 밟는다(멈춘
//               누름은 거의 돌지 않는다, 일지 183). 모으기: 모든 살 칸에 G = Σ 원천 K0 커널 × 넓이 × Q(가려진 푸아송 식의 그린 함수, 거리 = max(3D 직선, 랜드마크
//               하한) + 마주봄 가림) — 붙은 칸·미끄러지는 칸·꼬리가 같은 식이라 눌림 경계에서 끊기지 않는다. 묶음(거친 8×8칸)의 걸음을 조각 1·2·4로 나눈다(일지 191).
//   ComposeCS : 최종 변위 = (함몰 u − 긴 몫 L)만큼 안쪽(−노멀)으로 + 끌림(거친 격자를 쌍선형으로 읽은 값)만큼 표면을 따라. 붙든 칸은 손 깊이에 선다(목표에서 뺀
//               몫은 살 속에서 일어난다). 솟음은 콜라이더 위 빈 높이·살 두께 한도 안에서. 접촉 묶음 목록만 돈다.
//   NormalCS(+ClearCS) : 이웃 칸의 변위 전후 위치로 노멀이 얼마나 기울었는지(회전)와 함몰 어둡기를 구한다. 머티리얼이 이 회전만큼 원래 노멀을 돌리므로 노멀맵·저작
//               노멀이 유지된다(ADR-0018). 베이크가 구운 화면 노멀이 있으면 그 노멀을 누르기 전 노멀로 삼는다(ADR-0007 §3.6). ClearCS는 쉬기 시작하는 첫 프레임에 한 번,
//               복원이 끝나 남은 아주 작은 함몰·기억·끌림·변위·노멀을 0으로 비운다.
// 좌표계: "프레임" = 타깃 컴포넌트의 회전만 적용한 cm 공간(원점 = 컴포넌트 위치). 콜라이더는 CPU가 이 공간으로 옮겨 올리고, 결과 변위도 이 공간이다(커널이 회전만
//   곱해 월드로 보낸다).
#include "../SoftbodyUnityCommon.hlsl"
#include "SoftbodyFieldCommon.hlsl"
#include "SoftbodyContactResponse.hlsl"   // 살층 응답 표(ADR-0037 §3.2, Tools/contact_response/table.py가 만든다)

// 부위 나누기 순열(C++ FSoftbodySeparationDim): 1 = 캐리어에 부위 나누기가 구워져 있고 켜져 있다. 공유 메모리·장벽 꼴이 바뀌는 패스(함몰·압력 칸·긴 몫 모으기)만
// 순열로 가르고, 나머지는 bSeparation으로 가른다. 순열이 없는 패스에서는 0이다.
#ifndef SB_SEPARATION
#define SB_SEPARATION 0
#endif

// 하드 서피스 순열(C++ FSoftbodyHardSurfaceDim, ADR-0037 §3.7): 1 = 이 장의 자산에 단단한 조각이 구워져 있다. 조각 코드와 그 자원 선언은 모두
// #if SB_HARD_SURFACE 안에 둔다 — 조각이 없는 자산이 도는 순열 0은 이 줄들이 없던 때와 같은 셰이더다(비용 분리, 원칙 E06). 순열이 없는 패스에서는 0이다.
#ifndef SB_HARD_SURFACE
#define SB_HARD_SURFACE 0
#endif

#define SB_KIND_SURFACE 1
#define SB_KIND_GHOST 2
#define SB_SHAPE_SPHERE 0
#define SB_SHAPE_CAPSULE 1
#define SB_SHAPE_BOX 2
#define SB_SHAPE_CONVEX 3

// --- 모든 패스 공용 ---
Texture2D<float4> CarrierPosition;   // xyz 정지 위치(메시 로컬), w 칸 종류
Texture2D<float4> CarrierNormal;     // xyz 미는 방향(두께 정도 범위의 평균 바깥 노멀, 메시 로컬), w 두께(로컬 단위)
uint Resolution;

uint SB_KindAt(int2 Texel)
{
	return (uint)round(CarrierPosition.Load(int3(Texel, 0)).w);
}

bool SB_IsCarried(uint Kind)
{
	return Kind == SB_KIND_SURFACE || Kind == SB_KIND_GHOST;
}

Texture2D<float4> CarrierRedirect;   // xy 값을 가져올 칸 좌표(정수 = 칸 한가운데, 고스트 칸은 옆 차트), z 렌더 삼각형 변 길이(메시 로컬), w 화면 노멀(SB_ScreenNormal)

/**
 * 가운데 칸에서 Dir 쪽으로 Radius칸 떨어진 이웃. 그 칸이 아틀라스 밖이거나 빈 칸이면(차트 가장자리) 가운데 쪽으로 한 칸씩
 * 당겨 찾고, 바로 옆도 없으면 가운데 칸을 쓴다(한쪽 차분). 차트 안쪽 칸은 첫 번째에서 바로 찾는다.
 */
int2 SB_NeighborAt(int2 Center, int2 Dir, int Radius)
{
	for (int r = Radius; r >= 1; --r)
	{
		int2 Texel = Center + Dir * r;
		if (all(Texel >= int2(0, 0)) && all(Texel < int2(Resolution, Resolution)) && SB_IsCarried(SB_KindAt(Texel)))
		{
			return Texel;
		}
	}
	return Center;
}

/**
 * 베이크가 CarrierRedirect w에 둔 화면 노멀(렌더 정점 노멀 보간, 메시 로컬 기본 자세, 팔면체로 펼친 두 값을 12비트씩 묶은 정수 + 1)을
 * 푼다(ADR-0007 §3.6). 옛 베이크(w = 0)는 false.
 */
bool SB_DecodeScreenNormal(int2 Texel, out float3 Normal)
{
	Normal = float3(0, 0, 0);
	float Code = CarrierRedirect.Load(int3(Texel, 0)).w;
	if (Code < 0.5)
	{
		return false;
	}
	uint Bits = (uint)round(Code) - 1u;
	float2 E = float2((float)(Bits >> 12), (float)(Bits & 4095u)) * (2.0 / 4095.0) - 1.0;
	float3 N = float3(E, 1.0 - abs(E.x) - abs(E.y));
	float Fold = saturate(-N.z);
	N.x += N.x >= 0.0 ? -Fold : Fold;
	N.y += N.y >= 0.0 ? -Fold : Fold;
	Normal = N;
	return true;
}

/** 회전 C(축 × sin, 90° 이하)만큼 V를 돌린다(로드리게스). 머티리얼의 SB_RotateByTilt와 같은 식. */
float3 SB_TurnByTilt(float3 C, float3 V)
{
	float SinSq = min(dot(C, C), 1.0);
	if (SinSq < 1e-10)
	{
		return V;
	}
	float CosTheta = sqrt(1.0 - SinSq);
	return CosTheta * V + cross(C, V) + C * (dot(C, V) / (1.0 + CosTheta));
}

// 이완 묶음(ADR-0013 2단계): 이완 반복 SB_TILE번이 한 프레임에 번지는 거리가 묶음 한 변이라, 값이 있는 묶음의 둘레
// TileReach겹(= 반복 수 / SB_TILE)까지 돌면 빠지는 곳이 없다.
#define SB_TILE 16
uint TilesPerSide;                   // 캐리어 한 변의 묶음 수

// --- 살층 폭 (ADR-0019·ADR-0037) ---
// 모든 폭은 그 자리 살 두께 h와 겉껍질 장력 길이 L_T에서 나온다(P02). 표(SoftbodyContactResponse.ush)는 3-D 살층 응답에서 만든 무차원 값이다.
//  - 함몰 가는 창 폭 ℓ₁ = (ℓ₁/h)(v, τ)·h, τ = L_T/h — 살층 응답의 짧은 몫(로렌츠 첫 항). 살 v 1·τ 0에서 0.23h, 폼 v 0에서 0.28h, 겉껍질이 넓힌다.
//  - 옆 끌림 폭 ℓ_t = √(h²/3 + L_T·h) — 옆 강성 μ/h(뼈까지 전단)와 μh/3(옆 살이 함께 끌림)에 겉껍질 면내 강성을 더한 것(ADR-0019 §2.1, ADR-0037 §3.4).
float ThicknessScale;  // 메시 로컬 → cm

// 부피 보존 v(ADR-0024·ADR-0037 §3.3): 눌린 살 부피가 옆으로 비켜나 솟는 몫(1 = 살·젤리, 0 = 폼 — 그 자리에서 줄어든다). v = 저작 에셋의
// Volume Preservation 값 × 칸 칠. 표가 이 값으로 바로 읽혀 함몰 폭·솟음·셀이 접히는 압력 상한이 함께 정해진다(측정 굽기도 같은 v를 같은 곡선으로).
// 캐리어에 칠이 없으면(옛 베이크, 칠하지 않은 메시) 칠은 전부 1이다.
Texture2D<float4> CarrierVolume;   // r 부피 보존 칠. bCarrierVolume이 0이면 묶이지 않은 자리 텍스처라 읽지 않는다
uint bCarrierVolume;
float VolumeValue;                 // 저작 에셋의 Volume Preservation 값(0~1, 메시 전체)
float VolumeOffset;                // 컴포넌트 Volume Offset(−1~1): 칸의 v에 더한다, 0이면 그대로

// 컴포넌트 Volume Offset(배치 2, F12·F13): 칸의 v(값 × 칠)에 더하고 0~1로 자른다. v는 기준 누름(살 L0, 깊이 L0/3인 손)에서 살 대비 둘레로 솟는
// 부피 몫이라(표의 v → ν·접힘 곡선, Tools/contact_response/volume_refit.py) 같은 폭이면 어느 칠에서나 같은 만큼 달라 보인다. 끝에서 잘리는 자리는
// 플레이 시작 알림이 적는다(USoftbodyComponent::LogUnusedValues).
float SB_VolumeShifted(float V)
{
	return saturate(V + VolumeOffset);
}

/** 이 칸의 부피 보존 v (0 폼 … 1 살) = 값 × 칠 + 컴포넌트 Volume Offset(SB_VolumeShifted). */
float SB_VolumeAt(int2 Texel)
{
	float Paint = bCarrierVolume != 0u ? saturate(CarrierVolume.Load(int3(Texel, 0)).r) : 1.0;
	return SB_VolumeShifted(saturate(VolumeValue) * Paint);
}

// 겉껍질 장력(ADR-0037 §3.4): 겉껍질 장력 길이 L_T = 4 s² × L0, s = 컴포넌트 Skin Tension 값, L0 = 고정 길이(SoftbodyContactResponse::
// ReferenceLength, 메시 로컬 — 배치 2). 장력은 한 재료의 상수라 칸 두께가 아니라 한 눈금으로 잰다 — 살이 얇은 곳(뼈 위)일수록 τ = L_T/h가 커져
// 껍질이 더 지배한다(물리). 칸별 칠은 없다(일지 298, ADR-0038 §3.5 — 부분 겉은 Flex 칠의 판이 맡는다). 칸을 받는 꼴은 읽는 곳이 많아 그대로 둔다.
float SkinLength;                  // 겉껍질 장력 길이 L_T(메시 로컬). 0이면 맨살

/** 이 칸의 겉껍질 장력 길이 L_T(메시 로컬). */
float SB_SkinLengthAt(int2 Texel)
{
	return max(SkinLength, 0.0);
}

/** 함몰 가는 창 폭 ℓ₁(Layer와 같은 단위) = (ℓ₁/h)(v, τ)·h. Layer = 살 두께, Skin = 겉껍질 장력 길이(같은 단위). */
float SB_WindowLength(float Volume, float Layer, float Skin)
{
	float H = max(Layer, 1e-6);
	return SB_ResponseWindow(Volume, Skin / H).x * H;
}

/** 옆 끌림 폭 ℓ_t(Layer와 같은 단위) = √(h²/3 + L_T·h). */
float SB_ShearLength(float Layer, float Skin)
{
	float H = max(Layer, 0.0);
	return sqrt(H * H * (1.0 / 3.0) + max(Skin, 0.0) * H);
}

// --- 끌림 거친 격자 (일지 161, 풀이는 ADR-0033) ---
// 끌림(ADR-0008)은 캐리어 칸이 아니라 거친 격자에서 푼다 — 솟음과 같은 격자(캐리어 칸 DragBlock × DragBlock = 솟음 CoarseBlock, 굽기가 거리표와 같이
// 정한다, 거친 칸 1 cm 이하). 함몰은 가는 격자에 그대로 남는다.
//  - 가는 격자에서 풀던 끌림은 반복 한 번에 칸 하나씩만 번져서, 결과가 식이 아니라 반복 수·갱신 빈도·캐리어 해상도로 정해졌다(손가락으로
//    문지를 때 식의 답 0.52 mm 대신 2.3 mm, 1024² 캐리어에서 쥔 끌림이 2.2배 — 일지 158·160). 거친 칸으로 옮기고(ADR-0030) 이제는 반복 없이
//    원천 커널 합으로 푸니(ADR-0033, 아래 끌림 패스) 식의 답 그대로다.
//  - 가는 격자 이완이 칸마다 읽고 쓰는 상태가 네 채널에서 한 채널(함몰)로 준다(일지 160).
//  - 대가: 움직이는 손바닥 앞 가장자리처럼 끌림이 칸 하나 사이에 크게 바뀌는 곳은 거친 칸 크기만큼 뭉툭하다.
// 거친 칸 값의 자리는 칸 한가운데(가는 칸 좌표 (거친 칸 + 0.5) × DragBlock − 0.5)이고, 가는 칸은 둘레 거친 칸 넷을 쌍선형으로 읽는다.
uint DragBlock;                      // 거친 칸 하나에 묶는 캐리어 칸 수(한 변) = 솟음 CoarseBlock
uint DragResolution;                 // 거친 격자 한 변의 칸 수 = 솟음 CoarseResolution(1024 이하여야 한다 — 링크의 칸 번호는 20비트, C++가 가린다)
float DragTexelSize;                 // 거친 칸 크기 (cm)
/**
 * 거친 칸 이웃 표(DragLinksCS가 캐리어에서 한 번 만든다). 칸마다 +x·−x·+y·−y 네 방향, 방향마다 uint 하나:
 *  - 같은 차트 옆 칸이나 벽(제 칸): 칸 번호 x + y × DragResolution.
 *  - 이음매(SB_DRAG_SEAM): 이 칸 면 가운데가 건너편 차트에서 닿는 점을, 그 차트 가장자리를 따라 이웃한 두 거친 칸 사이로 보간한다 —
 *    첫 칸 번호(비트 0~19), 첫 칸에서 둘째 칸 쪽 방향 k(비트 20~21, SB_DragDirection), 둘째 칸 몫 × 255(비트 22~29).
 *  - 표면 칸이 없는 칸: 네 방향 모두 SB_DRAG_EMPTY.
 * 올려 읽기(SB_DragAt)와 끌림 원천 패스의 이웃 눌림 표지에 쓴다(ADR-0033 — 끌림은 이웃 값을 읽어 푸는 반복이 아니다).
 */
Texture2D<uint4> DragLinks;
Texture2D<float4> DragState;         // 끌림: xyz 프레임 cm — 원천 커널 합(ADR-0033, 어디서나 한 식 — 일지 183). w = 그 갱신에 눌렸으면 1 + 덮인 몫 c, 아니면 0.
                                     // 접촉·원천 패스는 지난 갱신 값, 합치기는 이번 갱신 값을 읽는다
#define SB_DRAG_SEAM 0x80000000u
#define SB_DRAG_EMPTY 0xffffffffu

/** 거친 칸 방향 k(0~3): +x, −x, +y, −y. */
int2 SB_DragDirection(uint k)
{
	return int2(k < 2u ? 1 - 2 * (int)k : 0, k < 2u ? 0 : 5 - 2 * (int)k);
}

/** 링크 하나 → 읽을 거친 칸 둘과 둘째 칸 몫. 같은 차트 옆 칸·벽은 한 칸(몫 0)이다. */
void SB_DragLinkCells(uint Link, out int2 Cell0, out int2 Cell1, out float Share)
{
	uint Index = Link & 0xfffffu;
	Cell0 = int2(Index % DragResolution, Index / DragResolution);
	Cell1 = Cell0;
	Share = 0.0;
	if ((Link & SB_DRAG_SEAM) != 0u)
	{
		Cell1 = Cell0 + SB_DragDirection((Link >> 20u) & 3u);
		Share = (float)((Link >> 22u) & 255u) / 255.0;
	}
}

float3 SB_DragLoad(int2 Cell)
{
	return DragState.Load(int3(Cell, 0)).xyz;
}

/** 거친 칸 Home의 링크 하나가 가리키는 이웃 값: 같은 차트 옆 칸, 이음매면 건너편 두 칸 사이 보간, 벽이면 Home 그대로. */
float3 SB_DragAcross(int2 Home, uint Link, float3 HomeValue)
{
	int2 Cell0;
	int2 Cell1;
	float Share;
	SB_DragLinkCells(Link, Cell0, Cell1, Share);
	return all(Cell0 == Home) ? HomeValue : lerp(SB_DragLoad(Cell0), SB_DragLoad(Cell1), Share);
}

/**
 * 캐리어 칸 좌표 Texel(정수 = 칸 한가운데, 고스트 칸은 가리키는 점의 소수 좌표)의 끌림: 둘레 거친 칸 넷을 쌍선형으로 읽는다. 축 방향
 * 두 칸은 이 칸이 든 거친 칸(Home)의 링크로 읽는다 — 차트 끝 너머(표면 칸이 없는 거친 칸)면 이음매 건너편 차트에서 보간한 값이라
 * 이음매 양쪽 칸이 같은 거친 칸들 사이를 보간해 이음매에서 갈라지지 않는다. 대각선 칸은 표면이 있으면 그 칸, 없으면 두 축 값에서
 * 평면으로 잇는다. 배치가 차트 사이에 9칸 이상 비워 두어 거친 칸 하나 떨어진 표면 칸은 늘 같은 차트다(MaxDragBlock 4).
 */
float3 SB_DragAt(float2 Texel)
{
	float2 Fine = Texel + 0.5;
	int2 Home = clamp((int2)floor(Fine / (float)DragBlock), int2(0, 0), int2(DragResolution - 1, DragResolution - 1));
	uint4 Links = DragLinks.Load(int3(Home, 0));
	if (Links.x == SB_DRAG_EMPTY)
	{
		return float3(0, 0, 0);
	}
	float2 Q = Fine / (float)DragBlock - 0.5;
	int2 Base = (int2)floor(Q);
	float2 F = Q - (float2)Base;
	// Home 열·줄이 Base 쪽이면 다른 탭은 +1 쪽, 아니면 −1 쪽이다. 다른 탭 쪽 무게 W.
	int2 Side = int2(Home.x == Base.x ? 1 : -1, Home.y == Base.y ? 1 : -1);
	float2 W = float2(Home.x == Base.x ? F.x : 1.0 - F.x, Home.y == Base.y ? F.y : 1.0 - F.y);
	float3 Here = SB_DragLoad(Home);
	float3 AlongX = SB_DragAcross(Home, Side.x > 0 ? Links.x : Links.y, Here);
	float3 AlongY = SB_DragAcross(Home, Side.y > 0 ? Links.z : Links.w, Here);
	int2 Corner = Home + Side;
	bool bCorner = all(Corner >= int2(0, 0)) && all(Corner < int2(DragResolution, DragResolution))
		&& DragLinks.Load(int3(Corner, 0)).x != SB_DRAG_EMPTY;
	float3 Diagonal = bCorner ? SB_DragLoad(Corner) : AlongX + AlongY - Here;
	return lerp(lerp(Here, AlongX, W.x), lerp(AlongY, Diagonal, W.x), W.y);
}

// --- ContactCS ---
Texture2D<float4> CarrierSkinIndex;  // 본 인덱스 4개 (스켈레탈)
Texture2D<float4> CarrierSkinWeight; // 가중치 4개 (스켈레탈)
StructuredBuffer<float4> Shapes;     // 형상당 3개: (중심, 종류), 회전 쿼터니언, (크기 파라미터)
StructuredBuffer<float4> Bones;      // 본당 3개: 스키닝 행렬(레퍼런스→현재, 컴포넌트 공간)의 열 0~2
StructuredBuffer<float4> PreviousBones; // Bones와 같은 꼴, 지난 갱신(PositionOut을 쓴 갱신)의 자세 — 손 닻이 피부가 본을 따라 움직인 거리를 뺀다(일지 336). 모르면 Bones
StructuredBuffer<float4> Planes;    // 컨벡스 평면(형상 로컬): xyz 바깥 노멀, w 거리. 컨벡스마다 평면 뒤에 한 줄 — 로컬 정점 상자의 반폭 xyz(SB_PresserBreadth)
uint NumShapes;
uint bSkinned;
float3 ComponentScale;

/**
 * 노멀을 메시 로컬에서 프레임으로 옮길 때 나눌 스케일(역전치). 부호를 지킨다: 거울 스케일(음수)로 놓은 메시는 노멀도 같이 뒤집혀야
 * 바깥을 본다 — 크기만으로 나누면 거울 축의 노멀이 안쪽을 봐서 광선이 살 밖으로 나가 접촉이 사라졌다.
 */
float3 SB_NormalScale()
{
	float3 Sign = float3(ComponentScale.x < 0.0 ? -1.0 : 1.0, ComponentScale.y < 0.0 ? -1.0 : 1.0, ComponentScale.z < 0.0 ? -1.0 : 1.0);
	return Sign * max(abs(ComponentScale), 1e-4);
}
float MaxContactDepth;
float TexelSize;                     // 캐리어 칸 크기 (cm)
RWTexture2D<float4> ContactOut;      // xyz 현재 바깥 노멀(프레임), w 관통 깊이 d(cm) — 관통이 없으면 이 칸의 피부 점에서 콜라이더까지 거리의 음수(솟는 살의 한도)
RWTexture2D<float> EdgeOut;          // 콜라이더 가장자리까지 부호 거리(칸 수, 눌린 안쪽 +, ±SB_EDGE_RANGE에서 자름, ADR-0029)와 벽 없음 양(ADR-0035)을 SB_PackEdge로 묶은 값 — 합치기가 변위 A로 옮긴다
RWTexture2D<float4> PositionOut;     // xyz 현재 피부 자리(프레임 cm, 스킨 + 덩어리 이동, 파임 전), w 1 = 칸 있음 (+ 누른 것의 번호, 규약은 SB_PackTouch) — 뒤 패스가 이웃 칸 사이 거리를 잰다. 끌림은 덮어쓰기 전 값(지난 프레임 자리)도 읽는다
// 끌림(ADR-0008)
StructuredBuffer<float4> ShapeMotion; // 형상당 3개: 지금 프레임 점 → 그 형상에 붙은 같은 점의 지난 프레임 위치 (행렬 열 0~2)
StructuredBuffer<float4> FieldMotion; // 거리장 부위당 3개: 같은 규약
Texture2D<float> StatePrevious;      // 지난 프레임 이완 결과: 함몰 u(cm)
float DragRatio;                     // 끌림 마찰: 피부가 옆으로 버티는 힘의 한도 = 누르는 힘 × 이 값 (0 = 끔, ADR-0008 §12·§13)
float LumpStiffen;                   // 살의 힘 법칙의 가파름 α(SB_TissueLaw): 누르는 힘(ADR-0017)과 피부를 옆으로 미는 힘(ADR-0008 §12)이 같은 값을 쓴다
uint bMotionHistory;                 // 1 = PositionOut에 지난 프레임 위치가 있다(표면 장이 비워진 뒤 첫 프레임은 0)
/**
 * 덩어리 마찰은 덩어리 평형 안에서 푼다(일지 201): 눌린 칸마다 손 닻(아래 LumpAnchor)을 기억하고 덩어리 평형(LumpForceCS)이 붙음·미끄러짐을
 * 제 풀이 안에서 E의 함수로 판정한다 — 편 손으로 누른 가슴(덩어리)이 떨지 않는다(일지 180~182의 고리: 접촉 패스가 CPU의 늦은 덩어리 자리로
 * 판정한 붙음·닻·한도가 덩어리 평형에 몇 프레임 늦게 되먹였다). 접촉 패스는 붙음을 판정하지 않는다.
 * 손 닻(갱신을 넘어 남는다 — 덩어리 값이 있는 캐리어만, RG32F): 눌린 칸마다 "덩어리가 안 움직였다면 손이 이 칸의 피부를 끌고 간 자리"를
 * 덩어리 기본 자세 축의 접선 두 성분(SB_LumpBasis(정지 노멀)의 축 1·2, cm)으로. 손 위 닿은 점이 그 칸 피부(본을 따라간 자리)에 대해 움직인 만큼만
 * 쌓이고 덩어리 이동은 더하지 않는다(덩어리 이동은 평형 안의 E로만 든다 — ADR-0017의 d0와 같은 원칙, 견주는 덩어리 이동이 본 기준이다 — 일지 336).
 * 마찰을 재는 면(손 부위의 면, 일지 338)은 갱신마다 새로 정해지므로 닻은 그 면이 아니라 늘 피부 접평면의 두 성분으로 둔다: 면 위의 닻을 피부 노멀을 따라
 * 피부 접평면에 비춘 값이라, 같은 면이면 그대로 되살아나고(SB_GripLift) 면이 돌면 피부 위 자리를 지킨 채 새 면으로 옮겨 간다.
 * 접촉 패스는 읽기만 한다: 지난 갱신에도 눌린 칸은 이 값과 이번 손 움직임을, 새로 눌린 칸은 보이는 피부 자리(끌림 + 덩어리 옆 이동)를 LumpDepthOut으로
 * 넘긴다(SB_PackLumpDepth). 쓰는 것은 LumpForceCS 첫 걸음(이번 뉴턴 출발점에서 마찰 한도 안으로 자른 값). 뗀 칸의 값은 남아 있어도 읽히지 않는다(다시
 * 누르면 새로 시작).
 */
Texture2D<float2> LumpAnchor;
// 끌림 거친 칸 입력(일지 161, ADR-0033): 접촉 패스가 거친 칸마다 3줄을 쓰고 끌림 원천·모으기 패스가 읽는다(갱신마다 새로 잡는 임시 버퍼 — 표면
// 칸이 있는 거친 칸만 쓰고 읽는다).
//   [0] xyz 누른 칸들이 콜라이더를 따라 움직인 거리의 평균(프레임 cm, 누른 칸이 없으면 0), w 버틸 수 있는 힘의 한도(살의 힘 단위, 음수 = 누르지
//       않은 칸 또는 끌림 마찰 0)
//   [1] xyz 바깥 노멀(표면 칸 평균), w 살 두께 평균(cm)
//   [2] xyz 지금 피부 자리 평균(프레임 cm), w 겉껍질 장력 길이 평균(cm) — 함몰 거친 해가 거친 칸 사이 거리를 잰다(일지 199)
#define SB_DRAG_INPUT_ROWS 3u
RWStructuredBuffer<float4> DragInputOut;
RWBuffer<uint> ActivityOut;          // [0] 1 = 이번 프레임 눌린 칸이 있다(쉬기, ADR-0013). 매 프레임 0으로 비운 뒤 쓴다
                                     // [SB_ACTIVITY_DEEP] 살 두께보다 깊이 눌린 표면 칸 수, [+1] 그 가운데 가장 깊이 넘은 양(cm, float 비트) — 관통 계약
                                     // 밖의 누름을 Softbody.Stats로 알린다(원칙 P06, 일지 295). [SB_ACTIVITY_LABEL_OVERFLOW] 들어온 면 표지 자리가
                                     // 모자란 표면 칸 수(개발 통계, 아래 들어온 면 표지). [SB_PRESSER_TABLE_BASE..] 누른 것 표(아래). [TilePressedBase..] 이완
                                     // 묶음마다 1 = 이번 프레임 눌린 칸이 있다 — 전에는 따로 버퍼였으나 접촉 패스의 쓰기 슬롯 8개(D3D11 한도) 가운데 하나를 표지
                                     // 텍스처에 내주려고 합쳤다
uint TilePressedBase;
// 접촉 패스 묶음 목록(ADR-0013 3단계): 표면·고스트 칸이 든 8×8 묶음. 목록 밖 묶음과 빈칸의 출력은 만들 때의 0 그대로다.
#define SB_CONTACT_ROW 256u           // 목록 디스패치의 한 줄 스레드 묶음 수(ContactGroupsCS가 같은 값으로 인자를 쓴다)
Buffer<uint> ContactGroups;          // 묶음 좌표 (x | y << 16), + SB_GROUP_LUMP_NEAR
Buffer<uint> ContactGroupCount;      // [0] 목록 길이
/** 접촉 묶음 표지: 묶음 둘레 8칸 안에 덩어리 칸이 있다 — 덩어리 노멀(SB_LumpNormal)이 0이 아닐 수 있는 묶음이다. */
#define SB_GROUP_LUMP_NEAR 0x80000000u

/** 묶음 목록 항목(x | y << 16, 위 비트는 표지) → 묶음 첫 칸 좌표(8×8칸 묶음). */
uint2 SB_GroupOrigin(uint Packed)
{
	return uint2(Packed & 0xffffu, (Packed >> 16u) & 0x7fffu) * 8u;
}

/** 접촉 묶음 목록을 도는 패스(한 줄 SB_CONTACT_ROW 묶음): 스레드 → 캐리어 칸. 목록 밖이거나 아틀라스 밖이면 false. */
bool SB_ListedTexel(uint3 GroupId, uint3 LocalId, out int2 Texel)
{
	uint Entry = GroupId.y * SB_CONTACT_ROW + GroupId.x;
	Texel = int2(0, 0);
	if (Entry >= ContactGroupCount[0])
	{
		return false;
	}
	Texel = int2(SB_GroupOrigin(ContactGroups[Entry]) + LocalId.xy);
	return all(Texel < int2(Resolution, Resolution));
}

// =====================================================================================================================
// 잡기 (ADR-0041): 손이 피부의 둥근 자리 하나를 붙들고 끈다(Softbody Grabber). 잡기마다 줄 하나 — 원 안 칸은 둥근 손끝처럼 붙들리고(가운데는 손을
// 그대로 따라가고 가장자리로 갈수록 느슨하다), 원 밖은 누를 때와 같은 살층 응답(함몰 창·긴 몫)으로 따라 솟거나 꺼진다. 칸마다 상태는 없다 — 잡기마다
// 줄 하나(잡은 칸, 시작 간격, 놓는 중 남은 당김)가 메시의 작은 버퍼에 남는다. 잡기가 없으면(GrabCount 0) 아래 함수는 모두 건너뛴다(균일 분기 — 지금 식 그대로).
//   ContactCS : 새 잡기의 잡은 점을 고른다(칸마다 지금 피부 자리에서 잡는 점까지 거리, 가장 가까운 칸 — ActivityOut의 잡기 낱말에 원자 최댓값). 원 안 칸은
//               콜라이더 누름을 지운다(잡기가 이긴다). 원 둘레 칸이 든 묶음을 함몰 목록에 올린다.
//   GrabResolveCS : 줄마다 시작 간격 기억·잡은 자리를 끈 양의 한도(피부와 덩어리가 함께, 넘어도 놓지 않고 머문다)·덩어리 용수철 목표(한도 안 당김)·
//                   표면 장 몫 D(한도 안 당김 − 덩어리가 따라 나온 몫)·놓는 중 감쇠(e^{−RΔt}).
//   ContactPressureCellsCS·SolveCS : 원 안 거친 칸은 잡기 줄(늘 활성·부호 자유·대각에 손끝 무름) — 음의 힘이 둘레를 꺼뜨린다(Full).
//   DragSourceCS : 원 안 거친 칸은 붙든 칸(붙든 자리 = 당김의 접선 몫, 힘 한도 없음 — Full).
//   LumpForceCS : 원 안 덩어리 칸은 손 쪽 세 방향 용수철(Lump Grip — Basic에서도).
//   DentCS : 원 안 칸은 목표 P_g + L 쪽 용수철 줄(P_g = −D·n, 바깥이 음수 — 판 칸과 같은 꼴), 원 둘레가 든 창은 바닥 없음(0 밑으로도).
//   ComposeCS : 원 둘레는 U − L(솟음 한도는 누름의 솟음 몫에만 — 당김은 콜라이더 위 빈 높이에 묶이지 않는다).
// GrabInput(C++가 갱신마다, 줄마다 float4 셋): [0] 끄는 자리(프레임 cm), 원 반지름 R(cm) / [1] 잡는 점(프레임 cm), asfloat(잡기 번호) / [2] 트레이스 방향
//   (프레임, Grab At Hit만), asfloat(표지 SB_GRAB_HOLD·SB_GRAB_FACING). 쓰지 않는 줄은 번호 0·표지 0.
// GrabState(갱신을 넘어 남는다, 줄마다 float4 아홉 — 읽기 고리가 CPU로 복사한다):
//   [0] asfloat(상태 | 까닭 << 8), asfloat(잡은 칸 x | y << 16), asfloat(잡기 번호), R
//   [1] 잡은 칸 정지 자리(메시 로컬), 살 두께 h(cm)   [2] 정지 노멀(메시 로컬, 단위), 한도 min(h, R)(cm)
//   [3] 지금 피부 자리(프레임 cm), 잡은 자리를 잡은 뒤 끈 길이(한도 전)   [4] 지금 노멀(프레임), 보이는 길이(잡은 자리가 잡은 뒤 움직인 길이 = 덩어리 +
//       표면 장, 한도 뒤 — 놓는 중은 표면 장의 남은 길이)
//   [5] 시작 간격(잡은 점의 노멀·접선 두 축 성분, cm), 그 자리 함몰 폭 ℓ₁(cm)   [6] 표면 장이 그리는 당김 D(프레임 cm — 한도 안 당김 − 덩어리가 따라 나온
//       몫, Basic은 노멀 몫만), 0
//   [7] 놓는 중 남은 당김(노멀·접선 두 축 성분), 그 길이 — 붙든 동안은 잡을 때 잡은 점의 덩어리 이동(같은 성분), 0
//   [8] 덩어리 잡기 용수철의 목표(덩어리 이동 없는 피부에서 잰 덩어리 이동, 프레임 cm — 잡을 때 덩어리 이동 + 한도 안 당김, 붙든 줄만), 잡은 뒤 덩어리가 따라
//       나온 길이(cm). 손–피부가 표면 장, 피부–뼈가 덩어리 — 둘이 직렬로 한도 안 당김을 나눈다. 목표는 덩어리의 지금 자리를 보지 않는다(일지 350). 놓으면 0:
//       용수철이 끊긴다
#define SB_GRAB_ROWS 8u                  // C++ SoftbodyGrab::MaxRows
#define SB_GRAB_INPUT_ROWS 3u            // C++ SoftbodyGrab::InputRows
#define SB_GRAB_STATE_ROWS 9u            // C++ SoftbodyGrab::StateRows
#define SB_GRAB_HOLD 1u                  // 입력 표지: 붙들고 있다(C++ SoftbodyGrab::FlagHold)
#define SB_GRAB_FACING 2u                // 입력 표지: 트레이스가 들어온 쪽을 보는 칸만 고른다(C++ SoftbodyGrab::FlagFacing)
#define SB_GRAB_CLEAR 4u                 // 입력 표지: 줄을 바로 비운다 — CPU가 GPU 확인 없이 놓은 줄(컴포넌트를 끈 동안 남은 줄이 다시 켤 때 튀지 않게, C++ SoftbodyGrab::FlagClear)
#define SB_GRAB_EMPTY 0u                 // 상태(C++ SoftbodyGrab::EStatus와 같은 차례)
#define SB_GRAB_HELD 1u
#define SB_GRAB_RELEASING 2u
#define SB_GRAB_MISSED 3u
#define SB_GRAB_REASON_RELEASED 1u       // 까닭(C++ ESoftbodyGrabEndReason과 같은 값 — 0 없음)
#define SB_GRAB_REASON_MISSED 3u
#define SB_GRAB_SEARCH 5.0               // 잡는 점에서 피부를 찾는 거리(cm, 기본) — 피직스 에셋 캡슐 위 히트는 피부와 몇 cm 떨어진다. 표지 비트 8~15가 0이
                                         // 아니면 그 값 × 0.1 cm(프리뷰는 렌더 메시 위를 맞혀 1 cm — 눈·머리카락을 눌러 옆 피부를 잡지 않게)
#define SB_GRAB_NORMAL_COS 0.5           // 원 안 칸: 정지 노멀이 잡은 점의 정지 노멀과 60° 안(얇은 곳의 반대쪽 겉은 원에 들지 않는다)
// 손끝 세기 β: 원 안 칸 용수철 κ = β(1 − x²)/x²(x = 잡은 점에서 거리 / R — 가운데는 사실상 핀, 가장자리 0), 함몰 창 줄의 제 몫 단위(줄 대각에서 이웃 몫을
// 뺀 1 — 칸 크기에 안 흔들린다). 3-D 살층 모의(scratchpad grab/pull_grip.py)의 손끝(세기 3)에 닮게 고른 값 — 재현(ADR-0041 §10 단계 7)으로 다시 정한다.
#define SB_GRAB_GRIP 4.0
#define SB_GRAB_GRIP_MAX 1e6
#define SB_GRAB_NEAR_WINDOWS 6.0         // 원 둘레 = R + 이 수 × 함몰 폭 ℓ₁ — 그 밖의 당김 꼬리는 끈 양의 0.25 % 아래
StructuredBuffer<float4> GrabInput;
StructuredBuffer<float4> GrabState;
uint GrabCount;                          // 0 = 잡기 없음(아래를 모두 건너뛴다), 아니면 SB_GRAB_ROWS
uint GrabPickBase;                       // ActivityOut에서 잡기 고르기 낱말(줄마다 하나)이 시작하는 자리

/** 줄의 상태(아래 8비트). */
uint SB_GrabStatusOf(float4 Head)
{
	return asuint(Head.x) & 0xffu;
}

/** 원이 살아 있는 줄(붙듦·놓는 중)인가. */
bool SB_GrabLive(uint Status)
{
	return Status == SB_GRAB_HELD || Status == SB_GRAB_RELEASING;
}

/**
 * 칸(정지 자리 RestP, 정지 노멀 RestN — 메시 로컬, 노멀은 단위)을 붙든 잡기들의 손끝 세기 합 κ(0 = 어느 원에도 들지 않음)와 세기 무게로 섞은 당김
 * Pull(프레임 cm). 원은 재료 좌표라 살이 움직여도 같은 칸이 붙들린다. 원이 겹치면 용수철을 더한다(두 손으로 벌려도 가운데 금이 없다).
 */
float SB_GrabGrip(float3 RestP, float3 RestN, out float3 Pull)
{
	Pull = float3(0, 0, 0);
	float Sum = 0.0;
	for (uint Row = 0u; Row < GrabCount; ++Row)
	{
		uint Base = Row * SB_GRAB_STATE_ROWS;
		float4 Head = GrabState[Base];
		if (!SB_GrabLive(SB_GrabStatusOf(Head)))
		{
			continue;
		}
		float3 Offset = (RestP - GrabState[Base + 1u].xyz) * ComponentScale;
		float R2 = Head.w * Head.w;
		float D2 = dot(Offset, Offset);
		if (D2 >= R2 || dot(RestN, GrabState[Base + 2u].xyz) < SB_GRAB_NORMAL_COS)
		{
			continue;
		}
		float X2 = D2 / max(R2, 1e-8);
		float K = min(SB_GRAB_GRIP * (1.0 - X2) / max(X2, 1e-6), SB_GRAB_GRIP_MAX);
		Sum += K;
		Pull += K * GrabState[Base + 6u].xyz;
	}
	Pull = Sum > 0.0 ? Pull / Sum : float3(0, 0, 0);
	return Sum;
}

/**
 * 덩어리 잡기 용수철(ADR-0041 §3.4): 칸을 붙든 붙듦 줄(놓는 중은 끊겼다)의 손끝 세기 합과 세기 무게로 섞은 전체 당김(줄 [8], 프레임 cm).
 */
float SB_GrabLumpTarget(float3 RestP, float3 RestN, out float3 Pull)
{
	Pull = float3(0, 0, 0);
	float Sum = 0.0;
	for (uint Row = 0u; Row < GrabCount; ++Row)
	{
		uint Base = Row * SB_GRAB_STATE_ROWS;
		float4 Head = GrabState[Base];
		if (SB_GrabStatusOf(Head) != SB_GRAB_HELD)
		{
			continue;
		}
		float3 Offset = (RestP - GrabState[Base + 1u].xyz) * ComponentScale;
		float R2 = Head.w * Head.w;
		float D2 = dot(Offset, Offset);
		if (D2 >= R2 || dot(RestN, GrabState[Base + 2u].xyz) < SB_GRAB_NORMAL_COS)
		{
			continue;
		}
		float X2 = D2 / max(R2, 1e-8);
		float K = min(SB_GRAB_GRIP * (1.0 - X2) / max(X2, 1e-6), SB_GRAB_GRIP_MAX);
		Sum += K;
		Pull += K * GrabState[Base + 8u].xyz;
	}
	Pull = Sum > 0.0 ? Pull / Sum : float3(0, 0, 0);
	return Sum;
}

/**
 * 노멀을 보지 않는 SB_GrabGrip — 거친 칸(정지 자리 평균만 있다)이 쓴다(끌림 원천·덩어리). 원 반지름 안이면 든다.
 */
float SB_GrabGripLoose(float3 RestP, out float3 Pull)
{
	Pull = float3(0, 0, 0);
	float Sum = 0.0;
	for (uint Row = 0u; Row < GrabCount; ++Row)
	{
		uint Base = Row * SB_GRAB_STATE_ROWS;
		float4 Head = GrabState[Base];
		if (!SB_GrabLive(SB_GrabStatusOf(Head)))
		{
			continue;
		}
		float3 Offset = (RestP - GrabState[Base + 1u].xyz) * ComponentScale;
		float R2 = Head.w * Head.w;
		float D2 = dot(Offset, Offset);
		if (D2 >= R2)
		{
			continue;
		}
		float X2 = D2 / max(R2, 1e-8);
		float K = min(SB_GRAB_GRIP * (1.0 - X2) / max(X2, 1e-6), SB_GRAB_GRIP_MAX);
		Sum += K;
		Pull += K * GrabState[Base + 6u].xyz;
	}
	Pull = Sum > 0.0 ? Pull / Sum : float3(0, 0, 0);
	return Sum;
}

/**
 * 원 안 칸의 목표 P_g(바깥이 음수)를 그 칸의 살 두께 Flesh(cm) 한도 곡선에 — 잡은 점 한도·둘레 솟음과 같은 "살은 제 두께보다 더 움직이지 못한다".
 * 살 없는 칸(뼈)은 0이라 원이 뼈를 걸쳐도 그 자리는 들리지 않는다(ADR-0041 §3.5, 일지 353).
 */
float SB_GrabAimLimit(float Aim, float Flesh)
{
	return sign(Aim) * SB_PressLimit(abs(Aim), max(Flesh, 0.0));
}

/** 칸이 어느 잡기의 둘레(원 + SB_GRAB_NEAR_WINDOWS × ℓ₁ + 두 칸) 안인가 — 그 칸이 든 함몰 창은 바닥 없이 풀고, 접촉 패스가 그 묶음을 함몰 목록에 올린다. */
bool SB_GrabNear(float3 RestP)
{
	for (uint Row = 0u; Row < GrabCount; ++Row)
	{
		uint Base = Row * SB_GRAB_STATE_ROWS;
		float4 Head = GrabState[Base];
		if (!SB_GrabLive(SB_GrabStatusOf(Head)))
		{
			continue;
		}
		float3 Offset = (RestP - GrabState[Base + 1u].xyz) * ComponentScale;
		float Reach = Head.w + SB_GRAB_NEAR_WINDOWS * GrabState[Base + 5u].w + 2.0 * TexelSize;
		if (dot(Offset, Offset) < Reach * Reach)
		{
			return true;
		}
	}
	return false;
}

/** 콜라이더가 없거나 아주 멀 때의 d: 막는 것 없음 (반정밀 텍스처에 들어가는 큰 음수, cm). */
#define SB_NO_OBSTACLE -10000.0

/**
 * 가장자리 다듬기 폭(cm) = `SurfaceEdgeTrim`(칸 수) × 칸 크기. 둥근 콜라이더를 반지름보다 깊이 누르면 콜라이더가 원래
 * 표면 아래로 불룩해서, 누른 깊이(광선이 콜라이더를 빠져나오는 곳까지)가 가장자리 칸 하나 사이에 0에서 콜라이더 밑까지
 * 뛴다(벽). 벽이 칸 격자에 찍혀 톱니로 보였다(ADR-0010 §8·§9). 그래서 광선이 콜라이더 안을 지나는 깊이 — 가장자리에서
 * 안쪽으로 들어온 거리와 같다 — 가 이 폭보다 작은 칸만 누른 깊이를 매끈하게 줄인다(SB_TrimmedDepth). 이 칸 위 콜라이더 모양만
 * 보므로 벽이 칸 사이 실제 가장자리를 따라가고, 이웃 칸(손가락 사이 틈)의 영향을 받지 않는다. 대가: 콜라이더 옆면이 이 폭만큼
 * 피부에 덮인다(겹침), 이 폭의 두 배보다 얇은 물체는 벽 쪽 몫만큼 덜 눌린다. 0이면 다듬지 않는다. Contact Detail이 Basic이면
 * 컴포넌트가 0을 보낸다(일지 224).
 */
float EdgeTrimWidth;

/**
 * 긴 몫(솟음)이 켜졌나(Contact Detail Full, ADR-0037). 꺼지면 솟는 살의 한도를 읽는 패스가 없으므로 거리장 부위는 위로 빈 높이를 재는
 * 걸음(SB_FieldRoom)을 건너뛰고 피부 점 거리를 그대로 적는다.
 */
uint bContactLongRoom;

/** 광선이 콜라이더 안을 지나는 깊이 Margin(cm)에 따른 벽 몫의 배율 (가장자리 0 → 다듬기 폭 안쪽 1). */
float SB_EdgeTrim(float Margin)
{
	float T = saturate(Margin / max(EdgeTrimWidth, 1e-6));
	return T * T * (3.0 - 2.0 * T);
}

/**
 * 다듬은 누른 깊이(cm): 광선이 콜라이더를 나오는 깊이 Exit 가운데 콜라이더 안을 지난 깊이 Margin까지는 그대로 두고, 그보다 깊은
 * 몫(벽 — 가장자리 칸 하나 사이에 뛰는 몫)만 SB_EdgeTrim으로 줄인다. 가장자리에서 Margin이 0으로 줄어 이어진다.
 * 전에는 깊이 전체에 배율을 곱해, 평평하거나 얕게 누른 곳(Margin = 누른 깊이)도 다듬기 폭 안이면 덜 눌렸다 — 기본값에서 1 mm
 * 누름이 0.38 mm로 보였다(일지 221).
 */
float SB_TrimmedDepth(float Exit, float Margin)
{
	float Floor = clamp(Margin, 0.0, Exit);
	return Floor + (Exit - Floor) * SB_EdgeTrim(Floor);
}

/**
 * 살의 힘 법칙(ADR-0017): 누른 깊이 D(cm), 살 두께 H에서 단위 넓이 힘 Phi(살 단단함 E/H 단위 — 작은 누름에서 Phi ≈ D),
 * 기울기 Slope = dPhi/dD, 적분 Energy = ∫Phi. 살 두께까지(ε = D/H ≤ 1)는 조일수록 단단해지는 Fung: Phi = H(e^{αε} − 1)/α
 * (할선 계수 (e^{αε} − 1)/(αε)). 그 너머(콜라이더가 코어를 지나 겹친 몫)는 코어에서의 기울기 e^α로 곧게 잇는다 —
 * 살은 코어보다 더 눌릴 수 없으니 손이 그만큼 들어오면 덩어리가 비켜나야 하고, 비켜날 수 없는 곳은 겹친다. α = 0이면 선형.
 * 옆 방향도 같은 법칙이다(ADR-0008 §12): 손 밑 피부가 밀린 거리 D를 피부 밑 살층이 같은 Phi로 되민다 — 미끄러지는 한도 거리
 * φ⁻¹(비율 × φ(깊이))가 여기서 나온다. 덩어리 쥐기의 붙은 칸은 그 한도에서 같은 힘에 닿는 곧은 스프링이다(일지 254, LumpForceCS).
 */
void SB_TissueLaw(float D, float H, float Alpha, out float Phi, out float Slope, out float Energy)
{
	float Core = min(D, H);
	float X = Alpha * Core / H;
	float Grow;
	float Rise;
	if (X > 1e-3)
	{
		float Ex = exp(X);
		Grow = (Ex - 1.0) / X;
		Rise = (Ex - 1.0 - X) / (X * X);
		Slope = Ex;
	}
	else
	{
		Grow = 1.0 + 0.5 * X;
		Rise = 0.5 + X / 6.0;
		Slope = 1.0 + X;
	}
	Phi = Core * Grow;
	Energy = Core * Core * Rise;
	float Beyond = max(D - H, 0.0);
	Energy += (Phi + 0.5 * Slope * Beyond) * Beyond;
	Phi += Slope * Beyond;
}

/** 살의 힘 법칙의 힘만(SB_TissueLaw의 Phi). */
float SB_TissueForce(float D, float H, float Alpha)
{
	float Phi;
	float Slope;
	float Energy;
	SB_TissueLaw(D, H, Alpha, Phi, Slope, Energy);
	return Phi;
}

/** 살의 힘 법칙의 역: 힘 Phi(≥ 0)를 내는 거리. 살 두께까지는 (H/α)·ln(1 + αPhi/H), 그 너머는 코어 기울기로 곧게. */
float SB_TissueInverse(float Phi, float H, float Alpha)
{
	float CorePhi;
	float CoreSlope;
	float CoreEnergy;
	SB_TissueLaw(H, H, Alpha, CorePhi, CoreSlope, CoreEnergy);
	Phi = max(Phi, 0.0);
	if (Phi > CorePhi)
	{
		return H + (Phi - CorePhi) / CoreSlope;
	}
	return Alpha > 1e-3 ? H / Alpha * log(1.0 + Alpha * Phi / H) : Phi;
}

/**
 * 콜라이더가 살 두께(코어) 너머로 들어갈 때(ADR-0003 §15). 광선은 살 두께의 (1 + 이 값)배까지 콜라이더를 찾고, 콜라이더가
 * 들어오는 곳이 살 두께를 넘으면 그 너머 이 비율 × 살 두께에 걸쳐 누름이 비껴간 값으로 이어진다. 누른 깊이는 살 두께에
 * 붙이지 않는다(일지 299 — 피부는 콜라이더 면을 따라갈 수 있는 만큼 따라가고, 압력·끌림 힘 같은 응답만 살 두께에 붙는다).
 * 전에는 살 두께에서 딱 끊겨, 코어 근처에 걸린 콜라이더가 가장자리 다듬기 폭(칸 1.5개, 0.24 cm)만 움직여도 누른 깊이가
 * 콜라이더 밑까지(수 cm)와 0 사이를 오갔다. 살이 얇은 곳(가슴 뿌리)과, 덩어리가 비켜나 손이 원래 표면보다 깊이 있는 덩어리
 * 힘(d0)에서 끼인 충돌체처럼 떨렸다. 코어 반대편 표면(살 두께의 두 배쯤)의 콜라이더는 닿지 않는다.
 */
#define SB_CORE_FADE 0.5

/** 콜라이더가 들어오는 곳 Entry(cm)에서의 누름 몫: 살 두께 안이면 1, 너머 SB_CORE_FADE × 살 두께에서 0. */
float SB_CoreFade(float Entry, float Layer)
{
	float T = saturate((Layer * (1.0 + SB_CORE_FADE) - Entry) / max(SB_CORE_FADE * Layer, 1e-4));
	return T * T * (3.0 - 2.0 * T);
}

/**
 * 제 살 깊이(ADR-0003 §16): 칸 밑(−미는 방향)의 살 가운데, 다른 쪽을 향한 면보다 이 칸에 더 가까운 깊이까지만 이 칸의 살이다.
 * 상자 옆면에서 윗면 모서리 가까운 칸은 조금만 들어가도 윗면이 더 가깝다 — 거기 묻힌 콜라이더는 윗면으로 들어온 것이라 옆면이
 * 꺼질 일이 아니다(전에는 옆면이 살 두께 × 1.5 안의 공을 제 살 속 콜라이더로 보고 같이 파였다). 캐리어 굽기가 이 깊이를 살 두께에
 * 대한 비율(0~1, 1 = 살 두께 그대로)로 구워 두고, 접촉은 표면을 뚫지 않고 살 속에 묻힌 콜라이더를 찾을 때 살 두께 대신
 * 살 두께 × 이 비율을 쓴다(SB_CoreFade도 같은 값으로). 칸 점이 콜라이더 안이면(콜라이더가 이 면을 뚫고 들어왔으면) 비율과
 * 상관없이 전처럼 눌리고, 누른 깊이의 상한은 Max Contact Depth다(일지 299). 텍스처가 없으면(둥근 몸, 옛 베이크) 전부 1이다.
 * 단위 주의: 구운 비율은 "찾는 끝"(살 두께 × (1 + SB_CORE_FADE))에 대한 것이다. 그래서 코드의 Own(살 두께 × 비율)은 제 살
 * 깊이의 2/3이고, 묻힌 콜라이더를 찾는 끝 Own × (1 + SB_CORE_FADE)가 제 살 깊이 그대로다. 서서히 놓는 구간은 그 안쪽이다.
 */
Texture2D<float4> CarrierColumn;   // r 제 살 깊이 비율. bCarrierColumn이 0이면 묶이지 않은 자리 텍스처라 읽지 않는다
uint bCarrierColumn;

#if SB_HARD_SURFACE
// 하드 서피스(ADR-0037 §3.7): 떠 있는 조각(보석) 밑 칸의 접촉 시작점 올림 — G16 × HardLiftScale = 메시 로컬 거리(굽기가 칸마다 노멀 쪽 조각 윗면까지 잰
// 값, 고스트는 가리킨 칸의 값). bCarrierLift 0 = 떠 있는 조각이 없다(묶이지 않은 자리 텍스처라 읽지 않는다).
Texture2D<float> CarrierLift;
float HardLiftScale;
uint bCarrierLift;

/** 칸의 접촉 시작점 올림(메시 로컬). */
float SB_HardLiftAt(int2 Texel)
{
	return bCarrierLift != 0u ? CarrierLift.Load(int3(Texel, 0)) * HardLiftScale : 0.0;
}
#endif

float SB_ColumnAt(int2 Texel)
{
	return bCarrierColumn != 0u ? max(CarrierColumn.Load(int3(Texel, 0)).r, 1.0 / 255.0) : 1.0;
}

// --- 들어온 면 표지 (일지 313 — ADR-0039 2단계의 앞 표지만 되살렸다, 일지 314 — 들어온 방향을 더했다) ---
// 화면: 손을 제 살 깊이보다 깊이 밀어 넣어도 손이 들어온 면은 손 뒤로 따라 들어간다. 전에는 손 뒷면이 제 살 깊이(SB_ColumnAt)를 넘으면 누름을
// 놓아 손등 앞으로 살이 비쳐 보였다(시험 캐릭터 가슴 앞을 손바닥으로 4 cm 누름 — 둥근 끝은 제 살 깊이가 굽은 반지름쯤으로 얕다).
// 제 살 깊이는 다른 면으로 들어와 살 속에 묻힌 콜라이더(배를 누른 손 뒤의 등, 상자 윗면으로 들어온 공 곁의 옆면)를 거르려고 찾는 깊이를 줄인 것인데,
// 한 프레임의 자리만으로는 "이 면으로 들어와 깊이 가라앉은 손"과 "다른 면으로 들어와 묻힌 손"을 가를 수 없다. 그래서 칸마다 "이 콜라이더는 이
// 면으로, 이 방향으로 들어왔다"는 표지(앞 표지)를 남긴다.
//  - 다는 때(사건): 콜라이더가 이 칸의 쉬는 피부 점을 새로 품은 갱신(지난 갱신에는 그 점이 콜라이더 밖), 또는 지난 갱신과 이번 갱신 사이에 콜라이더
//    겉이 그 점을 지나간 갱신(길 검사 — 한 갱신에 겉을 통째로 지나간 손가락). 이미 품고 있던 점은 사건이 아니다(그 점을 품은 채 손이 방향을 바꾸어도
//    그 면으로 들어온 것이 아니다). 쉬는 피부 점 = 광선 출발점에서 끌림을 뺀 자리(끌림은 지난 갱신의 상태라 사건 판정에 쓰지 않는다 — 끌린 출발점은
//    손과 같이 옆으로 가 손이 그 점을 지나지 않는다). 제 살 깊이 안에서 누르기만 하고 그 점을 지나지 않은 콜라이더(상자 옆면 가까이 윗면으로 묻힌
//    공, 함몰 둘레 벽)는 표지 없이 지금 규칙이다 — 그 공이 옆면에서 멀어지면 옆면은 전처럼 돌아온다.
//  - 들어온 방향: 사건 갱신에 콜라이더가 그 점에 대해 움직인 쪽(콜라이더에 붙어 옮겨진 지난 점 − 이번 점). 콜라이더가 이 면을 따라 지나가면(그 쪽이
//    안쪽 노멀과 60° 넘게 기울면) 이 면으로 들어온 것이 아니라 표지를 달지 않는다. 움직임을 모르면(새 콜라이더의 처음 겹침, 지난 피부 자리를 모름,
//    거의 안 움직임)도 달지 않는다 — 그 점은 콜라이더 안이라 표지 없이도 눌리고, 콜라이더가 움직이는 갱신에 단다(순간이동한 처음 겹침 = 지금 규칙).
//  - 지키는 때: 쉬는 피부 점에서 들어온 방향으로 쏜 광선(살 두께 + Max Contact Depth)이 콜라이더(형상·부위 가운데 어느 것이든 — 기둥이 드는 손가락과
//    이 광선이 드는 손바닥이 달라도 같은 손이다)에 들거나 반 칸 + 0.05 cm 안을 스치는 동안(점을 품고 있어도, SB_LabelsCover). 곧게 누르는 손은
//    지나간 점이 늘 손이 지나온 길 위라 깊이 눌러도 지킨다. 손이 옆으로 미끄러져 그 점 "밑"(들어온 쪽)을 떠나면 지운다
//    — 전에는 −노멀 기둥이 콜라이더에 닿는 한 지켜, 기운 칸(가슴 비탈)의 기둥이 옆으로 간 손등에 비스듬히 닿아 손이 지나간 자리가 손 밑으로 끌려
//    들어가 구겨졌다(손을 한쪽 가슴에서 다른 쪽으로 쓸 때, 일지 314).
//  - 표지가 있는 콜라이더는 살 두께 + Max Contact Depth까지 찾고, 제 살 깊이 너머에서는 기둥(−노멀)이 들어온 방향과 이루는 각만큼 따라간다
//    (SB_FrontFacing: 45°까지 다, 표지를 다는 문턱인 60°로 가며 0). 칸·표지마다 정해진 값이라 손이 움직여도 흔들리지 않는다 — 전에는 기둥이 들어가는
//    콜라이더 겉의 기울기로 정해, 손가락 옆·틈이 기둥을 지날 때마다 따라가는 몫이 0과 1 사이를 오가 눌린 둘레가 떨렸다(일지 314).
//  - 정하는 재료는 입력뿐이다: 쉬는 피부 자리, 콜라이더의 지난·지금 자세, 지난 갱신의 이 칸 표지(번호·들어온 방향). 그린 모양(함몰)·끌림은 갱신을
//    넘겨 쓰지 않는다 — 출력이 입력으로 되먹지 않아 쏠림이 쌓일 길이 없다.
//  - 한계: 표면 장이 쉬거나 화면 LOD로 텍스처를 내주면 표지를 잃는다. 깨어난 뒤 이미 제 살 깊이보다 깊이 가라앉아 있던 콜라이더는 다시 제 살 깊이
//    규칙으로 잰다(손을 조금 빼었다 다시 밀면 표지가 다시 붙는다).
// 번호는 바깥 콜라이더 하나(컴포넌트 — 장갑 손의 거리장 부위 16개가 한 번호)마다, 자기 몸 부위는 부위마다다: 쉬는 점을 지난 손가락 부위가 기운 기둥에서
// 옆으로 빠지고 그 기둥에 더 깊이 든 손바닥 부위는 그 점을 지난 적이 없어도 같은 손이라 따라간다.
// 저장(ContactLabels, R32F 한 채널, 갱신을 넘어 남는다): 칸마다 콜라이더 둘까지 — 자리마다 코드(1 + 고정 번호, 0 = 빈 자리, 7비트)와 들어온 방향
// (8비트, SB_LabelDirCode). 넷을 30비트 정수로 묶어 float의 비트로 둔다(SB_LabelsWord: 지수를 1~128로만 써 늘 보통 수라, 0 가까운 수가 0으로 깎이거나
// NaN이 되지 않아 쓰고 읽는 동안 비트가 그대로다). 0 = 표지 없음.
// 칸 스레드가 제 칸만 제자리에서 읽고 쓴다(이웃을 읽지 않아 갱신 시작 사본이 필요 없다. 32비트 한 채널은 UAV 읽기가 모든 GPU에서 된다).
RWTexture2D<float> ContactLabels;
StructuredBuffer<uint> ColliderIds;  // 형상 → 거리장 부위 순으로 고정 번호(아래 7비트, SB_ID_NONE = 번호 없음, 한 콜라이더의 부위들은 같은 번호) + SB_ID_NEW
uint bLabels;                        // 1 = 표지 텍스처가 묶였다(0이면 지금 규칙 그대로)
uint bLabelHistory;                  // 1 = ContactLabels가 지난 갱신 값이다(장이 비워진 뒤 첫 갱신은 0)
uint bSweep;                         // 1 = PositionOut의 지난 자리와 콜라이더의 지난 자세가 같은 갱신 것이다(길 검사를 할 수 있다)
#define SB_ID_NONE 127u
#define SB_ID_NEW 0x80u              // 지난 갱신에 없던 콜라이더 — 지난 자세를 몰라 길 검사를 하지 않는다(처음 겹침 = 지금 규칙)
#define SB_LABEL_MARGIN 0.05         // 표지를 지우는 여유(cm, + 반 칸)
#define SB_LABEL_TILT_STEP 0.2       // 들어온 방향의 기울기(접선 몫 ÷ 안쪽 몫)를 축마다 이 간격으로 적는다(4비트: −1.4~+1.4, 가운데 0)
#define SB_LABEL_ENTER_COS 0.5       // 들어온 방향이 안쪽 노멀과 이 코사인(60°)보다 기울면 이 면으로 들어온 것이 아니다
#define SB_LABEL_MOVE 1e-3           // 이보다 짧은 움직임(cm)은 방향을 모른다고 본다(표지를 달지 않는다)

/** 칸 하나의 표지 작업 상태: 지난 갱신 자리 둘, 이번에 지킨 자리, 새로 단 표지, 쉬는 피부 점, 들어온 방향을 적는 칸의 축. */
struct FSBLabels
{
	uint Old0;         // 지난 갱신 자리 둘의 코드(1 + 고정 번호, 0 = 빈 자리)
	uint Old1;
	uint OldDir0;      // 지난 갱신 자리 둘의 들어온 방향(SB_LabelDirCode)
	uint OldDir1;
	uint Kept;         // 비트 k = 지난 자리 k를 이번에도 지킨다
	uint Added0;
	uint Added1;
	uint AddedDir0;
	uint AddedDir1;
	uint NumAdded;
	uint bPath;        // 1 = 지난 갱신 쉬는 피부 점을 안다(길 검사)
	float3 Point;      // 이번 갱신 쉬는 피부 점(사건 판정 점, 프레임 cm)
	float3 Before;     // 지난 갱신 쉬는 피부 점
	float3 N;          // 이번 갱신 바깥 노멀(프레임)
	float3 T1;         // 칸에 붙은 접선 두 축(정지 노멀에서 만든 축을 스키닝으로 옮긴 것 — 칸이 돌면 같이 돈다)
	float3 T2;
	float Reach;       // 앞 표지가 따라가는 깊이(cm) = 살 두께 + Max Contact Depth
	float Margin;      // 표지를 지우는 여유(cm)
};

/**
 * 거리장 부위 광선이 덤으로 알려 주는 것: 피부 점에서 부위까지 거리(안이면 음수), 들어간 깊이, 찾는 깊이 안에서 들어갔나, 찾은 구간에서 부위에 가장 가까이 간 거리,
 * 덩어리를 미는 겹침과 방향 Push(SB_ShapePush와 같은 뜻, 일지 317 — 부르는 쪽이 bPush를 켰을 때만 면 노멀을 읽고, 아니면 광선 쪽 값).
 */
struct FSBRay
{
	float Surface;
	float Entry;
	bool bEntered;
	float Closest;
	bool bPush;
	float4 Push;
	float Support;   // 피부가 부위 면을 따라갈 수 있는 깊이(cm): 바깥 콜라이더는 Max Contact Depth, 자기 몸 부위는 살 두께 ÷ 1.3(일지 324, SB_FieldDepth)
};

/** 지난 갱신에 이 칸이 콜라이더 번호 Id에 단 자리(0·1), 없으면 -1. */
int SB_LabelSlot(FSBLabels L, uint Id)
{
	return L.Old0 == Id + 1u ? 0 : (L.Old1 == Id + 1u ? 1 : -1);
}

/** 지난 자리 Slot의 들어온 방향 코드. */
uint SB_LabelOldDir(FSBLabels L, int Slot)
{
	return Slot == 0 ? L.OldDir0 : L.OldDir1;
}

void SB_LabelAdd(inout FSBLabels L, uint Id, uint Dir)
{
	// 한 콜라이더의 부위 여럿이 같은 갱신에 같은 번호를 달 수 있다 — 한 번만(먼저 단 부위의 방향).
	if ((L.NumAdded > 0u && L.Added0 == Id + 1u) || (L.NumAdded > 1u && L.Added1 == Id + 1u))
	{
		return;
	}
	if (L.NumAdded == 0u)
	{
		L.Added0 = Id + 1u;
		L.AddedDir0 = Dir;
	}
	else if (L.NumAdded == 1u)
	{
		L.Added1 = Id + 1u;
		L.AddedDir1 = Dir;
	}
	L.NumAdded += 1u;
}

/** 표지 30비트 정수 → 저장 float: 0은 0.0, 나머지는 지수 1~128의 보통 수(비트가 그대로 남는다). */
float SB_LabelsWord(uint K)
{
	return K == 0u ? 0.0 : asfloat((((K >> 23u) + 1u) << 23u) | (K & 0x7fffffu));
}

/** 저장 float → 표지 30비트 정수(SB_LabelsWord의 역). 그 꼴이 아닌 값(0, 큰 수)은 표지 없음. */
uint SB_LabelsUnword(float V)
{
	uint Bits = asuint(V);
	uint Exponent = (Bits >> 23u) & 0xffu;
	return (Bits >> 31u) != 0u || Exponent == 0u || Exponent > 128u ? 0u : ((Exponent - 1u) << 23u) | (Bits & 0x7fffffu);
}

/**
 * 이번 갱신 표지 값: 지킨 자리 먼저, 그다음 새로 단 표지. 둘을 넘으면 넘친 표지는 버리고 Overflow = 1(개발 통계 — 버린 콜라이더는 다음 갱신에
 * 다시 사건을 기다린다).
 */
float SB_LabelsPack(FSBLabels L, out uint Overflow)
{
	uint Codes[4] = {(L.Kept & 1u) != 0u ? L.Old0 : 0u, (L.Kept & 2u) != 0u ? L.Old1 : 0u, L.NumAdded > 0u ? L.Added0 : 0u,
		L.NumAdded > 1u ? L.Added1 : 0u};
	uint Dirs[4] = {L.OldDir0, L.OldDir1, L.AddedDir0, L.AddedDir1};
	uint Out0 = 0u;
	uint Out1 = 0u;
	uint Dir0 = 0u;
	uint Dir1 = 0u;
	uint Count = 0u;
	UNROLL
	for (uint k = 0u; k < 4u; ++k)
	{
		if (Codes[k] == 0u)
		{
			continue;
		}
		if (Count == 0u)
		{
			Out0 = Codes[k];
			Dir0 = Dirs[k];
		}
		else if (Count == 1u)
		{
			Out1 = Codes[k];
			Dir1 = Dirs[k];
		}
		Count += 1u;
	}
	Overflow = Count > 2u || L.NumAdded > 2u ? 1u : 0u;
	return SB_LabelsWord(Out0 | (Out1 << 7u) | (Dir0 << 14u) | (Dir1 << 22u));
}

/**
 * 앞 표지 콜라이더가 쉬는 점보다 깊이 있을 때(광선이 공기를 지나 들어갈 때) 깊이 따라가는 몫(0~1): 이 칸의 기둥(−노멀)과 들어온 방향의 코사인
 * CosEntry로 정한다. 들어온 방향에서 45°까지 기운 기둥은 1이고, 표지를 다는 문턱인 60°(SB_LABEL_ENTER_COS)로 가며 0이 되어 지금 규칙(코어 너머
 * 잇기)으로 이어진다 — 많이 기운 기둥으로 콜라이더 아래까지 따라가면 높이만 바꾸는 이 모델에서 칸이 대각선으로 수 cm 끌려간다(일지 306).
 * 콜라이더 면 따라가기 한도(SB_FollowLimit)와 같은 생각이다.
 */
float SB_FrontFacing(float CosEntry)
{
	return smoothstep(SB_LABEL_ENTER_COS, 0.7071, CosEntry);
}

/** 들어온 방향 코드(8비트)의 축마다 기울기(접선 몫 ÷ 안쪽 몫). 코드 0 = 안쪽 노멀 그대로(0, 0). */
float2 SB_LabelTilt(uint Code)
{
	return Code == 0u ? float2(0.0, 0.0) : (float2((float)(Code >> 4u), (float)(Code & 15u)) - 8.0) * SB_LABEL_TILT_STEP;
}

/** 들어온 방향(프레임, 단위): 안쪽 노멀을 칸의 두 접선 축 쪽으로 코드만큼 기울인 방향. */
float3 SB_LabelDirection(FSBLabels L, uint Code)
{
	float2 Tilt = SB_LabelTilt(Code);
	return normalize(-L.N + Tilt.x * L.T1 + Tilt.y * L.T2);
}

/** 들어온 방향 U(단위, 안쪽 노멀과 코사인 SB_LABEL_ENTER_COS 이상) → 코드(축마다 4비트 1~15, 가운데 8 = 기울지 않음; 둘 다 가운데면 0). */
uint SB_LabelDirCode(FSBLabels L, float3 U)
{
	float In = max(dot(U, -L.N), 1e-3);
	int A = clamp((int)round(dot(U, L.T1) / (In * SB_LABEL_TILT_STEP)), -7, 7) + 8;
	int B = clamp((int)round(dot(U, L.T2) / (In * SB_LABEL_TILT_STEP)), -7, 7) + 8;
	return A == 8 && B == 8 ? 0u : ((uint)A << 4u) | (uint)B;
}

/** 표지가 제 살 깊이 너머에서 따라가는 몫: 기둥과 들어온 방향의 코사인 = 1 / √(1 + 기울기²)로 SB_FrontFacing. */
float SB_LabelShare(uint Code)
{
	float2 Tilt = SB_LabelTilt(Code);
	return SB_FrontFacing(rsqrt(1.0 + dot(Tilt, Tilt)));
}

/**
 * 칸의 표지 작업을 시작한다. Point = 이번 쉬는 피부 점(광선 출발점 − 끌림), 지난 점 = 덮어쓰기 전 PositionOut(지난 갱신 피부 자리) + Point가 피부
 * 자리에서 벗어난 몫(올림·덩어리 국소 모양 — 같다고 본다). N = 바깥 노멀, T1·T2 = 칸에 붙은 접선 두 축(들어온 방향을 적고 읽는 축).
 */
FSBLabels SB_LabelsBegin(int2 Texel, float3 Point, float3 Skin, float Layer, float3 N, float3 T1, float3 T2)
{
	FSBLabels L;
	L.Old0 = 0u;
	L.Old1 = 0u;
	L.OldDir0 = 0u;
	L.OldDir1 = 0u;
	L.Kept = 0u;
	L.Added0 = 0u;
	L.Added1 = 0u;
	L.AddedDir0 = 0u;
	L.AddedDir1 = 0u;
	L.NumAdded = 0u;
	L.Reach = Layer + MaxContactDepth;
	L.Margin = 0.5 * TexelSize + SB_LABEL_MARGIN;
	float4 Last = bMotionHistory != 0u ? PositionOut[Texel] : float4(0, 0, 0, 0);
	L.bPath = bSweep != 0u && bMotionHistory != 0u && Last.w > 0.5 ? 1u : 0u;
	L.Point = Point;
	L.Before = Last.xyz + (Point - Skin);
	L.N = N;
	L.T1 = T1;
	L.T2 = T2;
	if (bLabels != 0u && bLabelHistory != 0u)
	{
		uint K = SB_LabelsUnword(ContactLabels[Texel]);
		L.Old0 = K & 0x7fu;
		L.Old1 = (K >> 7u) & 0x7fu;
		L.OldDir0 = (K >> 14u) & 0xffu;
		L.OldDir1 = (K >> 22u) & 0xffu;
	}
	return L;
}

float3 SB_InverseRotate(float4 Q, float3 V)
{
	float3 U = -Q.xyz;
	return V + 2.0 * cross(U, cross(U, V) + Q.w * V);
}

float SB_ShapeSignedDistance(uint Shape, float3 P)
{
	float4 Row0 = Shapes[Shape * 3 + 0];
	float4 Row1 = Shapes[Shape * 3 + 1];
	float4 Row2 = Shapes[Shape * 3 + 2];
	float3 Local = SB_InverseRotate(Row1, P - Row0.xyz);
	uint Type = (uint)round(Row0.w);
	if (Type == SB_SHAPE_SPHERE)
	{
		return length(Local) - Row2.x;
	}
	if (Type == SB_SHAPE_CAPSULE)
	{
		Local.z -= clamp(Local.z, -Row2.y, Row2.y);
		return length(Local) - Row2.x;
	}
	if (Type == SB_SHAPE_CONVEX)
	{
		// 평면 거리 최댓값: 안쪽(관통 영역)에서는 가장 가까운 면까지의 정확한 거리, 바깥 모서리 근처는 하한.
		uint Start = (uint)round(Row2.x);
		uint Count = (uint)round(Row2.y);
		float Distance = -1e20;
		for (uint Plane = 0; Plane < Count; ++Plane)
		{
			float4 PlaneData = Planes[Start + Plane];
			Distance = max(Distance, dot(PlaneData.xyz, Local) - PlaneData.w);
		}
		return Distance;
	}
	float3 Q = abs(Local) - Row2.xyz;
	return length(max(Q, 0.0)) + min(max(Q.x, max(Q.y, Q.z)), 0.0);
}

/** 광선 구간 두 개의 합(볼록한 조각들로 나눈 볼록 형상이라 합도 한 구간이다). 빈 구간은 (1e20, -1e20). */
float2 SB_UnionInterval(float2 A, float2 B)
{
	return float2(min(A.x, B.x), max(A.y, B.y));
}

float2 SB_RaySphere(float3 O, float3 D, float3 C, float R)
{
	float3 L = O - C;
	float B = dot(L, D);
	float Disc = B * B - (dot(L, L) - R * R);
	if (Disc < 0.0)
	{
		return float2(1e20, -1e20);
	}
	float S = sqrt(Disc);
	return float2(-B - S, -B + S);
}

/** 볼록 형상 로컬에서 광선 O + tD(|D| = 1)가 형상 안에 있는 t 구간. 안 지나면 (1e20, -1e20). */
float2 SB_ShapeRayInterval(uint Shape, float3 P, float3 Dir)
{
	float4 Row0 = Shapes[Shape * 3 + 0];
	float4 Row1 = Shapes[Shape * 3 + 1];
	float4 Row2 = Shapes[Shape * 3 + 2];
	float3 O = SB_InverseRotate(Row1, P - Row0.xyz);
	float3 D = SB_InverseRotate(Row1, Dir);
	uint Type = (uint)round(Row0.w);
	const float2 Empty = float2(1e20, -1e20);
	if (Type == SB_SHAPE_SPHERE)
	{
		return SB_RaySphere(O, D, float3(0, 0, 0), Row2.x);
	}
	if (Type == SB_SHAPE_CAPSULE)
	{
		// 캡슐 = 양 끝 구 두 개 ∪ (무한 원기둥 ∩ |z| ≤ 반길이).
		float R = Row2.x;
		float H = Row2.y;
		float2 Result = SB_UnionInterval(SB_RaySphere(O, D, float3(0, 0, H), R), SB_RaySphere(O, D, float3(0, 0, -H), R));
		float A = dot(D.xy, D.xy);
		float2 Cylinder;
		if (A < 1e-8)
		{
			Cylinder = dot(O.xy, O.xy) <= R * R ? float2(-1e20, 1e20) : Empty;
		}
		else
		{
			float B = dot(O.xy, D.xy);
			float Disc = B * B - A * (dot(O.xy, O.xy) - R * R);
			Cylinder = Disc < 0.0 ? Empty : float2((-B - sqrt(Disc)) / A, (-B + sqrt(Disc)) / A);
		}
		float2 Slab;
		if (abs(D.z) < 1e-6)
		{
			Slab = abs(O.z) <= H ? float2(-1e20, 1e20) : Empty;
		}
		else
		{
			float T0 = (-H - O.z) / D.z;
			float T1 = (H - O.z) / D.z;
			Slab = float2(min(T0, T1), max(T0, T1));
		}
		float2 Body = float2(max(Cylinder.x, Slab.x), min(Cylinder.y, Slab.y));
		return Body.x <= Body.y ? SB_UnionInterval(Result, Body) : Result;
	}
	if (Type == SB_SHAPE_CONVEX)
	{
		uint Start = (uint)round(Row2.x);
		uint Count = (uint)round(Row2.y);
		float2 Range = float2(-1e20, 1e20);
		for (uint Plane = 0; Plane < Count; ++Plane)
		{
			float4 PlaneData = Planes[Start + Plane];
			float Denom = dot(PlaneData.xyz, D);
			float Num = PlaneData.w - dot(PlaneData.xyz, O);
			if (abs(Denom) < 1e-8)
			{
				if (Num < 0.0)
				{
					return Empty;
				}
			}
			else if (Denom > 0.0)
			{
				Range.y = min(Range.y, Num / Denom);
			}
			else
			{
				Range.x = max(Range.x, Num / Denom);
			}
		}
		return Range.x <= Range.y ? Range : Empty;
	}
	// 박스: 축마다 판 사이 구간의 교집합.
	float2 Range = float2(-1e20, 1e20);
	UNROLL
	for (int Axis = 0; Axis < 3; ++Axis)
	{
		if (abs(D[Axis]) < 1e-8)
		{
			if (abs(O[Axis]) > Row2[Axis])
			{
				return Empty;
			}
		}
		else
		{
			float T0 = (-Row2[Axis] - O[Axis]) / D[Axis];
			float T1 = (Row2[Axis] - O[Axis]) / D[Axis];
			Range = float2(max(Range.x, min(T0, T1)), min(Range.y, max(T0, T1)));
		}
	}
	return Range.x <= Range.y ? Range : Empty;
}

/**
 * 광선 O − N·t 의 구간 T0 ≤ t ≤ T1 에서 형상까지 부호 거리의 최솟값(cm).
 *  - 광선이 형상을 지나지 않으면 광선이 형상에 가장 가까이 간 거리(0 이상): 덩어리가 이 형상에 닿기까지 남은 틈으로 쓴다
 *    (SB_ShapeMissed). 솟는 살의 한도는 이 값이 아니다.
 *  - 지나면(형상 안 구간) 음수이고, 그 크기가 광선이 형상 안을 지나는 깊이 = 가장자리에서 안쪽으로 들어온 거리다(가장자리 다듬기).
 * 볼록 형상은 광선을 따라 거리가 한 번 줄었다가 늘기만 하므로 구·캡슐은 식으로, 박스·컨벡스는 황금 분할로 찾는다.
 * MaxContactDepth보다 먼 값은 쓰이지 않으므로 대강 둔다.
 */
float SB_ShapeRayClosest(uint Shape, float3 O, float3 N, float T0, float T1)
{
	float4 Row0 = Shapes[Shape * 3 + 0];
	float4 Row2 = Shapes[Shape * 3 + 2];
	uint Type = (uint)round(Row0.w);
	if (Type == SB_SHAPE_SPHERE)
	{
		float T = clamp(dot(O - Row0.xyz, N), T0, T1);
		return length(O - N * T - Row0.xyz) - Row2.x;
	}
	if (Type == SB_SHAPE_CAPSULE)
	{
		// 광선 구간과 캡슐 축 구간 사이 가장 가까운 거리 − 반지름 (두 선분 최근접점, 형상 로컬).
		float3 P1 = SB_InverseRotate(Shapes[Shape * 3 + 1], O - N * T0 - Row0.xyz);
		float3 D1 = SB_InverseRotate(Shapes[Shape * 3 + 1], -N) * (T1 - T0);
		float3 P2 = float3(0.0, 0.0, -Row2.y);
		float3 D2 = float3(0.0, 0.0, 2.0 * Row2.y);
		float3 R = P1 - P2;
		float A = dot(D1, D1);
		float E = max(dot(D2, D2), 1e-8);
		float F = dot(D2, R);
		float C = dot(D1, R);
		float B = dot(D1, D2);
		float Denom = A * E - B * B;
		float S = Denom > 1e-8 ? saturate((B * F - C * E) / Denom) : 0.0;
		float T = (B * S + F) / E;
		if (T < 0.0)
		{
			T = 0.0;
			S = saturate(-C / max(A, 1e-8));
		}
		else if (T > 1.0)
		{
			T = 1.0;
			S = saturate((B - C) / max(A, 1e-8));
		}
		return length(P1 + D1 * S - P2 - D2 * T) - Row2.x;
	}
	float Start = SB_ShapeSignedDistance(Shape, O - N * T0);
	if (Start - (T1 - T0) >= MaxContactDepth)
	{
		return Start;
	}
	// 구간 첫머리에서 거리가 늘면(형상이 바깥쪽에 있으면) 가장 가까운 곳은 첫머리다.
	if (SB_ShapeSignedDistance(Shape, O - N * (T0 + min(0.01, T1 - T0))) >= Start)
	{
		return Start;
	}
	const float Golden = 0.618034;
	float Low = T0;
	float High = T1;
	float X1 = High - Golden * (High - Low);
	float X2 = Low + Golden * (High - Low);
	float F1 = SB_ShapeSignedDistance(Shape, O - N * X1);
	float F2 = SB_ShapeSignedDistance(Shape, O - N * X2);
	UNROLL
	for (int Step = 0; Step < 10; ++Step)
	{
		if (F1 < F2)
		{
			High = X2;
			X2 = X1;
			F2 = F1;
			X1 = High - Golden * (High - Low);
			F1 = SB_ShapeSignedDistance(Shape, O - N * X1);
		}
		else
		{
			Low = X1;
			X1 = X2;
			F1 = F2;
			X2 = Low + Golden * (High - Low);
			F2 = SB_ShapeSignedDistance(Shape, O - N * X2);
		}
	}
	return min(Start, min(F1, F2));
}

// --- 콜라이더 면 따라가기 한도 (일지 299) ---
// 피부는 미는 방향으로만 움직인다. 콜라이더 면이 미는 방향을 마주 보면 그 면까지 딱 붙어 따라가지만, 면이 미는 방향과 나란할수록 같은 겹침을
// 풀려고 옆으로 멀리 가야 한다. 벽·바닥에 나란한 옆면 칸은 광선이 콜라이더 속을 옆으로 지나 몸 밖에서야 나와, 피부 자리 깊이를 살 두께에 붙이지
// 않은 뒤 Max Contact Depth까지 옆으로 파였다(시험 캐릭터 벽 팔 옆면 2.6 cm, 반쯤 묻은 주사위 옆면 19.5 cm). 그래서 따라가는 깊이를 겹침을 면
// 노멀 쪽으로 잰 양 Along = 겹침 × cos ψ(ψ = 미는 방향과 면 노멀 사이)에 묶는다.
//  - 큰 이동은 Along × SB_FOLLOW_STEEP까지 — ψ ≤ 45°인 면에는 딱 붙는다.
//  - 살 두께 × SB_FOLLOW_ALLOW 안의 작은 이동은 Along × SB_FOLLOW_SMALL까지 — ψ ≤ 72°. 공을 깊이 누른 둘레 테처럼 조금만 움직이면 되는 칸은
//    가파른 면에도 붙는다. 두 갈래 사이(Along이 살 두께의 0.03~0.3배)는 작은 이동 크기에 Along을 더해 늘 오르게 한다 — 그 크기에서 멈추면
//    공을 깊이 누른 둘레 안쪽 몇 칸이 같은 깊이에 서서 평평한 톱니 띠가 보였다(일지 356, 시험 캐릭터 아랫배 공 10 cm 4.5 cm 누름 808칸 → 5칸).
//    양 끝에서 두 갈래와 만나고 바뀌는 몫은 살 두께의 0.15배까지다.
//  - 나란한 면(cos ψ = 0)은 따라가지 않고 겹친 채 둔다(틈은 금지, 겹침은 허용).
// 평면에서는 나가는 면 갈래와 가장 가까운 면 갈래가 같은 값이라 이어진다. 모의(이미 찍은 장면, 셰이더와 같은 꼴 min(다듬은 깊이, 한도)):
// 벽 옆 칸 p95 2.60 → 1.27 cm, 주사위 바닥선 위 보이는 튐 2만여 → 0~56칸, 공 둘레 테 겹침 그대로(0.080 / 0.010 cm).
#define SB_FOLLOW_STEEP 2.0     // 큰 이동: Along의 이 배까지(cos² ψ ≥ 1/2 → 45°)
#define SB_FOLLOW_SMALL 10.0    // 작은 이동: Along의 이 배까지(cos² ψ ≥ 1/10 → 72°)
#define SB_FOLLOW_ALLOW 0.3     // 작은 이동의 크기: 살 두께의 이 몫

/** 따라가는 깊이 한도(cm): 광선이 나가는 깊이 Exit, 면 노멀 쪽 겹침 Along, 살 두께 Layer. */
float SB_FollowLimit(float Exit, float Along, float Layer)
{
	return min(Exit, max(SB_FOLLOW_STEEP * Along, min(SB_FOLLOW_ALLOW * Layer + Along, SB_FOLLOW_SMALL * Along)));
}

/**
 * 형상 하나의 따라가는 깊이 한도(cm, SB_FollowLimit). 나가는 곳이 따라가는 끝(Max Contact Depth × SB_PRESS_OVERSHOOT) 안이면 그 면에서,
 * 너머면(닿을 수 없는 면) 피부 점에서 가장 가까운 면에서 겹침과 cos ψ를 잰다. cos ψ는 부호 거리를 광선 쪽(−N)으로 미분한 값이다 — 나가는 면에서는
 * 마주 보면 1, 나란하면 0이다. 피부 점에서는 크기만 본다(SB_FarCos): 깊이 들어간 공·엄지는 피부 점 가장 가까운 면이 피부 바깥을 보는데(뒤를 봄),
 * 이를 0으로 두면 함몰이 지워져 더 누를수록 얕아졌다(일지 318). 나란한 옆면(벽·반쯤 묻은 주사위)은 크기도 0이라 그대로 겹친 채 둔다.
 * 피부 점이 콜라이더 밖이면(살 속에 다 묻힌 누르개) 가장 가까운 면이 누름을 말해 주지 않으므로 나가는 면에서 잰다. 어느 쪽이든 깊이는 뒤에
 * SB_PressLimit가 Max Contact Depth에서 멈춘다.
 */
float SB_FarCos(float Slope, bool bAtExit)
{
	return bAtExit ? saturate(Slope) : saturate(abs(Slope));
}

/**
 * 다 묻힌 누르개 몫(일지 369): 피부 점이 콜라이더 밖이고 나가는 면이 따라가는 끝 너머일 때, 나가는 면 규칙(일지 341)은 콜라이더가 제 살 속에서
 * 들어올 때만 쓴다. 들어오는 곳 Entry가 제 살 깊이 Own의 2/3 안이면 1, Own에서 0(SB_CoreFade 꼴). 이 면으로 들어온 앞 표지 콜라이더는
 * 제 살 너머에서도 FrontShare만큼(코어 너머 잇기와 같은 규칙). 바닥에 놓은 방석처럼 코어 밑에서 들어오는 큰 콜라이더는 나가는 면(4 m 아래
 * 바닥 상자 아랫면)이 정면이라 윗면 전체가 바닥까지 꺼졌다 — 341 전에는 0이었다.
 */
float SB_BuriedShare(float Entry, float Own, bool bFront, float FrontShare)
{
	float Share = SB_CoreFade(Entry, Own / (1.0 + SB_CORE_FADE));
	return bFront ? max(Share, FrontShare) : Share;
}

/** Buried = 다 묻힌 누르개 몫(SB_BuriedShare) — 피부 점이 콜라이더 밖이고 나가는 면이 따라가는 끝 너머일 때만 쓴다. */
float SB_ShapeFollow(uint Shape, float3 Origin, float3 N, float Exit, float Layer, float Buried)
{
	float Eps = 0.25 * TexelSize;
	float Inside = max(-SB_ShapeSignedDistance(Shape, Origin), 0.0);
	bool bReach = Exit <= SB_PRESS_OVERSHOOT * MaxContactDepth;
	bool bAtExit = bReach || Inside <= 0.0;
	float3 At = bAtExit ? Origin - N * Exit : Origin;
	float CosPsi = SB_FarCos((SB_ShapeSignedDistance(Shape, At - N * Eps) - SB_ShapeSignedDistance(Shape, At + N * Eps)) / (2.0 * Eps), bAtExit);
	float Over = bAtExit ? Exit * CosPsi * (bReach ? 1.0 : Buried) : Inside;
	return SB_FollowLimit(Exit, Over * CosPsi, Layer);
}

/**
 * 덩어리를 미는 겹침과 방향(일지 317). 덩어리 힘을 받는 칸만 잰다. SB_ShapeFollow와 같은 곳(나가는 곳이 따라가는 끝 안이면 그 면, 너머면 피부 점에서
 * 가장 가까운 면)에서 거리의 기울기로 콜라이더 면 노멀을 잰다. 돌려주는 값: xyz = 그 면 노멀의 반대(프레임 단위 벡터 — 피부 점이 이쪽으로 가면 겹침이
 * 는다), w = 그 방향으로 잰 겹침(cm, 가장자리 다듬기 Trimmed를 거친 광선 깊이 × cos ψ). 평평한 면이면 피부 점이 면을 따라 미끄러져도 w + xyz·이동이
 * 그대로다 — 덩어리 평형이 이 방향으로 다시 재므로 덩어리의 옆 이동이 미는 깊이를 되먹이지 않는다. 광선 길이는 비스듬한 피부에서 겹침 ÷ cos ψ로
 * 부풀어, 덩어리 국소 모양의 옆 몫이 출발점을 벽 속으로 옮기면 다음 갱신의 미는 힘이 커져 옆 몫이 다시 커졌다(벽에 한 갱신 4 cm로 대면 상한까지).
 */
float4 SB_ShapePush(uint Shape, float3 Origin, float3 N, float Exit, float Trimmed)
{
	float Eps = 0.25 * TexelSize;
	bool bReach = Exit <= SB_PRESS_OVERSHOOT * MaxContactDepth;
	float3 At = bReach ? Origin - N * Exit : Origin;
	float3 Gradient = float3(
		SB_ShapeSignedDistance(Shape, At + float3(Eps, 0, 0)) - SB_ShapeSignedDistance(Shape, At - float3(Eps, 0, 0)),
		SB_ShapeSignedDistance(Shape, At + float3(0, Eps, 0)) - SB_ShapeSignedDistance(Shape, At - float3(0, Eps, 0)),
		SB_ShapeSignedDistance(Shape, At + float3(0, 0, Eps)) - SB_ShapeSignedDistance(Shape, At - float3(0, 0, Eps)));
	float Size = length(Gradient);
	float3 Away = Size > 1e-8 ? -Gradient / Size : N;
	float Over = bReach ? Trimmed * saturate(dot(Away, N)) : min(Trimmed, max(-SB_ShapeSignedDistance(Shape, Origin), 0.0));
	return float4(Away, Over);
}

/**
 * 형상이 이 칸을 누르지 않을 때의 값 둘(음수, cm).
 *  x = 솟는 살의 한도: 위로 빈 높이(피부 점 Origin에서 바깥 노멀로 곧게 올라가 형상에 닿기까지)의 음수. 솟는 살은 그 방향으로만
 *      움직이므로 이 모델에서 정확한 답이다(ADR-0003 §18, 일지 229). 안쪽으로 쏜 광선 구간 Hit(SB_ShapeRayInterval, 바깥쪽이 t < 0)의 부호만
 *      읽는다. 형상이 그 위에 없으면(구간이 비었거나 안쪽에 묻혔으면) 막지 않는다 — 살 두께 Layer로 둔다(바깥 한도는 살 두께가 정한다).
 *      전에는 피부 점에서 형상까지 가장 가까운 거리였다(§17). 위가 비어도 형상 곁에서 45° 원뿔로 막아, 가까운 두 콜라이더 사이 능선이 틈의
 *      절반에서 멈춰 두 비탈이 선에서 만나는 지붕이 되었다(일지 228 §7).
 *  y = 덩어리가 형상에 닿기까지 남은 틈(ADR-0017): 전과 같이 안쪽 광선이 제 살 깊이 안에서 가장 가까이 간 거리의 음수.
 *      덩어리 힘을 받는 칸(bLump)만 재고, 나머지 칸은 쓰지 않는 값이라 x를 그대로 둔다.
 */
float2 SB_ShapeMissed(uint Shape, float3 Origin, float3 N, float Own, bool bLump, float2 Hit, float Layer)
{
	float Limit = -(Hit.x <= Hit.y && Hit.y <= 0.0 ? min(-Hit.y, Layer) : Layer);
	return float2(Limit, bLump ? -SB_ShapeRayClosest(Shape, Origin, N, 0.0, Own) : Limit);
}

float3 SB_TransformByBone(uint Bone, float4 V)
{
	return float3(dot(V, Bones[Bone * 3 + 0]), dot(V, Bones[Bone * 3 + 1]), dot(V, Bones[Bone * 3 + 2]));
}

// --- 모양 거리장 콜라이더 (ADR-0004) ---
// 누르는 쪽의 실제 모양을 부위별 3D 격자(부호 있는 거리, 메시 로컬 단위, 안쪽 음수)로 구워 두고, 부위마다 "프레임 →
// 부위 메시 로컬" 행렬로 칸 점을 옮겨 읽는다. 스켈레탈은 본마다 부위 하나가 그 본을 따라 딱딱하게 움직인다.
// 외부 콜라이더(손 등)는 단순 형상과 같은 규칙(−노멀 광선이 살 두께 안에서 들어가면 먼 쪽 면까지)으로 누른다.
// 자기 몸은 베이크에서 고른 누르는 부위(손 등)만 같은 규칙으로 살을 누르고, 기본 포즈에서 이미 붙어 있던 살
// (관절로 이어진 옆 살, 그 부위 자신의 살)은 누르지 않는다.
StructuredBuffer<uint> FieldVoxels;       // 반정밀 거리 두 개씩 (낮은 16비트가 짝수 번 칸)
StructuredBuffer<float4> FieldInstances;  // 부위당 SB_FIELD_ROWS개 — SoftbodySurfaceField.h FieldInstances 주석
uint NumFieldInstances;

#define SB_FIELD_ROWS 7
/** 광선이 부위를 지나는 곳을 찾는 걸음 수 상한. 거리만큼 건너뛰므로 대개 몇 걸음에 끝난다. */
#define SB_FIELD_MAX_STEPS 32
/**
 * 위로 빈 높이(솟는 살의 한도, SB_FieldRoom)를 재는 걸음 수 상한(일지 229). 부위가 위에 있거나 멀면 두세 걸음에 끝나고, 부위 옆을 나란히 올라가는
 * 광선(손가락 곁)만 걸음마다 그 틈만큼 나아가 상한에 닿는다. 그때는 온 데까지를 한도로 두므로(실제 높이보다 작거나 같다) 살이 부위를 뚫지 않는다.
 */
#define SB_FIELD_ROOM_STEPS 8
/**
 * 부위를 나오는 곳을 다시 찾는 반 나누기 횟수. 걸음은 거리만큼(최소 칸의 1/4) 건너뛰어 부위를 나온 첫 점이 면을 0~1/4칸 넘고,
 * 넘는 양이 칸마다 달라 눌린 면에 잔물결이 생겼다 — 깊이 누를수록 광선이 면을 비스듬히 나가 커진다(오프라인 흉내: 공이 칸 12개
 * 깊이로 누르면 노멀 차이 90%가 6~9°). 세 번 나눈 뒤 직선으로 이으면 0.06° 이하다.
 */
#define SB_FIELD_EXIT_SPLITS 3
/**
 * 나오는 곳을 면보다 이 비율(거리장 칸 대비)만큼 더 깊게 둔다. 삼선형 거리의 면은 콜라이더가 오목한 곳(손가락 사이)에서 실제
 * 메시보다 조금 바깥에 놓여 틈이 보일 수 있으므로 늘 조금 겹치게 한다. 칸마다 같은 값이라 물결을 만들지 않는다.
 */
#define SB_FIELD_EXIT_OVERLAP 0.125
/**
 * 가장자리 다듬기가 걸리는 칸(광선이 부위 안을 지난 깊이 < 다듬기 폭)에서 그 깊이를 다시 찾는 황금 분할 읽기 수(일지 224).
 * 부위 안 걸음은 거리만큼 건너뛰어 가장 깊은 곳을 지나치고(볼록 단면이면 최대 1/3 덜 잼), 다듬기는 그 오차를 누른 깊이 ÷ 다듬기
 * 폭만큼 키워 자국이 울퉁불퉁해졌다(일지 221 — 실제 손 Edge Smoothing 3에서 손가락 가운데가 0.55~0.95배로 줄마다 다름).
 * 네 번이면 대칭 단면에서 가장 깊은 곳의 94% 이상을 찾는다(오프라인 흉내: Edge Smoothing 3 음영 오차 5~6° → 2°).
 */
#define SB_FIELD_DEEPEST_READS 4

float SB_FieldVoxel(uint Index)
{
	uint Pair = FieldVoxels[Index >> 1];
	return f16tof32((Index & 1u) != 0u ? (Pair >> 16) : (Pair & 0xffffu));
}

float3 SB_FieldToLocal(uint Instance, float3 P)
{
	float4 P1 = float4(P, 1.0);
	uint Row = Instance * SB_FIELD_ROWS;
	return float3(dot(P1, FieldInstances[Row + 0]), dot(P1, FieldInstances[Row + 1]), dot(P1, FieldInstances[Row + 2]));
}

/** 부위 로컬 점의 부호 있는 거리(메시 로컬 단위). 격자 밖은 가장 가까운 칸 값에 격자까지의 거리를 더한다. */
float SB_FieldDistanceLocal(uint Instance, float3 Q)
{
	uint Row = Instance * SB_FIELD_ROWS;
	float4 Grid = FieldInstances[Row + 3];   // 첫 칸 중심, 칸 크기
	float4 Dims = FieldInstances[Row + 4];   // 칸 수, 첫 칸 번호
	float3 G = (Q - Grid.xyz) / Grid.w;
	float3 C = clamp(G, 0.0, Dims.xyz - 1.0);
	float Outside = length(G - C) * Grid.w;
	uint3 N = (uint3)Dims.xyz;
	uint3 I = min((uint3)C, N - 2u);
	float3 T = C - (float3)I;
	uint SY = N.x;
	uint SZ = N.x * N.y;
	uint Base = (uint)Dims.w + I.x + SY * I.y + SZ * I.z;
	float V00 = lerp(SB_FieldVoxel(Base), SB_FieldVoxel(Base + 1u), T.x);
	float V10 = lerp(SB_FieldVoxel(Base + SY), SB_FieldVoxel(Base + SY + 1u), T.x);
	float V01 = lerp(SB_FieldVoxel(Base + SZ), SB_FieldVoxel(Base + SZ + 1u), T.x);
	float V11 = lerp(SB_FieldVoxel(Base + SZ + SY), SB_FieldVoxel(Base + SZ + SY + 1u), T.x);
	return lerp(lerp(V00, V10, T.y), lerp(V01, V11, T.y), T.z) + Outside;
}

/** 부위 격자 상자까지의 거리 (메시 로컬 단위, 상자 안이면 0). 먼 부위를 빨리 건너뛰는 데 쓴다. */
float SB_FieldBoxGap(uint Instance, float3 Q)
{
	uint Row = Instance * SB_FIELD_ROWS;
	float4 Grid = FieldInstances[Row + 3];
	float4 Dims = FieldInstances[Row + 4];
	float3 G = (Q - Grid.xyz) / Grid.w;
	return length(G - clamp(G, 0.0, Dims.xyz - 1.0)) * Grid.w;
}

/**
 * 광선 P + Dir·t 의 구간 Low ≤ t ≤ High에서 부위까지 거리의 최솟값(cm, 안이면 음수)을 황금 분할 SB_FIELD_DEEPEST_READS번으로
 * 찾는다. 볼록한 부위 단면에서는 거리가 한 번 줄었다 늘기만 하므로(SB_ShapeRayClosest와 같은 꼴) 가장 깊은 곳으로 좁혀 간다.
 */
float SB_FieldRayDeepest(uint Instance, float3 P, float3 Dir, float Scale, float Low, float High)
{
	const float Golden = 0.618034;
	float X1 = High - Golden * (High - Low);
	float X2 = Low + Golden * (High - Low);
	float F1 = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * X1)) * Scale;
	float F2 = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * X2)) * Scale;
	float Best = min(F1, F2);
	UNROLL
	for (int Read = 2; Read < SB_FIELD_DEEPEST_READS; ++Read)
	{
		if (F1 < F2)
		{
			High = X2;
			X2 = X1;
			F2 = F1;
			X1 = High - Golden * (High - Low);
			F1 = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * X1)) * Scale;
			Best = min(Best, F1);
		}
		else
		{
			Low = X1;
			X1 = X2;
			F1 = F2;
			X2 = Low + Golden * (High - Low);
			F2 = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * X2)) * Scale;
			Best = min(Best, F2);
		}
	}
	return Best;
}

/**
 * 피부 점 P에서 바깥(Out)으로 곧게 올라가 부위에 닿기까지의 거리(cm, 위로 빈 높이 — 솟는 살의 한도, 일지 229)를 Cap까지 잰다. 거리만큼
 * 건너뛰는 걸음이라(거리가 실제보다 작거나 같아 부위를 건너뛰지 않는다) 마지막 점까지 온 거리 + 그 점의 거리는 늘 실제 높이보다 작거나 같다.
 * SB_FIELD_ROOM_STEPS번 안에 Cap에 닿거나 부위에 닿지 못하면 그 값을 돌려준다. 걸음 0번이면 피부 점 거리 Surface(일지 228까지의 한도)다.
 */
float SB_FieldRoom(uint Instance, float3 P, float3 Out, float Scale, float Surface, float Cap)
{
	float T = 0.0;
	float S = Surface;
	LOOP
	for (int Step = 0; Step < SB_FIELD_ROOM_STEPS; ++Step)
	{
		if (T + S >= Cap || S <= 1e-3)
		{
			break;
		}
		T += S;
		S = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Out * T)) * Scale;
	}
	return T + max(S, 0.0);
}

/**
 * 부위 하나에 쏜 −노멀 광선의 결과(형상 루프와 같은 규약, cm): 살 두께 안에서 광선이 부위로 들어가면(점이 부위 안이면
 * 그 점부터) 부위를 빠져나오는 곳까지의 거리를 가장자리 다듬기(SB_TrimmedDepth, 살 두께 안에서 광선이 부위 안을 지난 가장
 * 깊은 곳으로 잰다 — 다듬기가 걸리면 그 깊이를 SB_FieldRayDeepest로 다시 찾는다)로 줄인 값, 아니면 비껴간 값(음수) —
 * SB_ShapeMissed와 같은 둘: x는 위로 빈 높이(솟는 살의 한도, SB_FieldRoom — 솟음이 꺼지면 피부 점에서 부위까지 거리),
 * y는 광선이 제 살 깊이 안에서 부위에 가장 가까이 간 거리(덩어리가 닿기까지 남은 틈).
 * 광선은 거리만큼 건너뛰며(거리가 실제보다 작거나 같아 부위를 건너뛰지 않는다) 들어가는 곳과 나오는 곳을 찾는다.
 * 걸음은 살 두께 끝에서 한 번 멈춰 끝점도 재므로, 가장 가까운 거리가 이웃 칸 사이에서 튀지 않는다.
 * 나오는 곳은 걸음이 넘어선 만큼 두지 않고 마지막 안쪽 점과 첫 바깥 점 사이에서 다시 찾는다(SB_FIELD_EXIT_SPLITS).
 * 코어 너머(SB_CORE_FADE): 살 두께의 (1 + SB_CORE_FADE)배까지 들어가는 곳을 찾고, 누른 깊이는 살 두께에 붙이지 않고 부위 면을 따라갈 수 있는 만큼이며(일지 299),
 * 들어가는 곳이 살 두께를 넘으면 비껴간 값으로 잇는다. PressGain = 누른 깊이에 곱할 몫(자기 몸 부위가 기본 포즈에서
 * 떨어진 정도, 살 두께에 붙이기 전), MissScale = 비껴간 값에 곱할 값.
 * Own = 제 살 깊이(살 두께 × SB_ColumnAt): 들어가는 곳을 찾는 깊이와 코어 너머 잇기는 살 두께 대신 이 값으로 잰다(ADR-0003 §16).
 * 돌려주는 값: x = 피부 자리 깊이(파임 — 다듬은 깊이와 따라가는 한도 가운데 작은 쪽), y = 다듬은 깊이(덩어리를 미는 힘, ADR-0017 — 같은 다듬기·코어 너머 잇기).
 * Edge(bEdge일 때만): 가장자리 부호 거리(cm, 안쪽 +, ADR-0029)를 최댓값으로 합친다. 지나면 광선이 부위 안을 지난 가장 깊은 곳
 * (걸음이 본 값 — 다듬기는 다시 찾은 값을 쓴다) × 코어 너머 몫, 비껴가면 −(광선이 부위에 가장 가까이 간 거리). 둘 다 광선 걸음이
 * 이미 구한 값이다.
 * Wall: 지나면 (나오는 깊이, 그 가운데 안을 지난 깊이) × PressGain × 코어 너머 몫을 성분마다 최댓값으로 합친다(벽 몫, ADR-0035).
 */
float2 SB_FieldRay(uint Instance, float3 P, float3 Dir, float Layer, float Own, float Scale, float PressGain, float MissScale,
	bool bEdge, inout float Edge, inout float2 Wall, float FrontSight, float FrontShare, inout FSBRay Ray)
{
	float Voxel = FieldInstances[Instance * SB_FIELD_ROWS + 3].w * Scale;
	float MinStep = max(0.25 * Voxel, 0.01);
	float Reach = Layer * (1.0 + SB_CORE_FADE);   // 가장자리 다듬기를 재는 깊이
	// 묻힌 부위로 들어가는 곳을 찾는 깊이. 앞 표지(들어온 면 표지)가 붙은 부위는 FrontSight(살 두께 + Max Contact Depth)까지 찾고 제 살 깊이 너머에서
	// FrontShare(SB_LabelShare)만큼 따라간다(아래 Fade).
	bool bFront = FrontSight > 0.0;
	float Sight = bFront ? max(FrontSight, Own) : Own * (1.0 + SB_CORE_FADE);
	float S = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P)) * Scale;
	Ray.Surface = S;
	Ray.Entry = 0.0;
	Ray.bEntered = S <= 0.0;
	Ray.Closest = S;
	float Surface = S;        // 피부 점에서 부위까지 거리 — 비껴갈 때 솟는 살의 한도(위로 빈 높이)를 재는 첫 걸음
	float Closest = S;        // 코어 너머까지 광선에서 부위까지 거리의 최솟값 (안이면 음수) — 가장자리 다듬기
	float ClosestLayer = S;   // 제 살 깊이 안에서만 — 비껴갈 때 덩어리가 닿기까지 남은 틈
	float T = 0.0;
	float Entry = 0.0;        // 부위로 들어가는 곳 (점이 부위 안이면 0)
	if (S > 0.0)
	{
		bool bEntered = false;
		LOOP
		for (int Step = 0; Step < SB_FIELD_MAX_STEPS; ++Step)
		{
			float TPrev = T;
			float SPrev = S;
			T = min(T + max(S, MinStep), TPrev < Own ? Own : Sight);
			S = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * T)) * Scale;
			Closest = min(Closest, S);
			ClosestLayer = T <= Own ? min(ClosestLayer, S) : ClosestLayer;
			if (S <= 0.0)
			{
				bEntered = true;
				Entry = TPrev + (T - TPrev) * SPrev / max(SPrev - S, 1e-6);
				break;
			}
			if (T >= Sight)
			{
				break;
			}
		}
		Ray.bEntered = bEntered;
		Ray.Entry = Entry;
		Ray.Closest = Closest;
		if (!bEntered)
		{
			Edge = bEdge ? max(Edge, -Closest) : Edge;
			Ray.Push = float4(-Dir, -ClosestLayer * MissScale);
			float Room = bContactLongRoom != 0u ? SB_FieldRoom(Instance, P, -Dir, Scale, Surface, Layer) : Surface;
			return float2(-Room, -ClosestLayer) * MissScale;
		}
	}
	float ExitLimit = Layer + 2.0 * Ray.Support;
	// 마지막 안쪽 점(TIn, SIn ≤ 0)과 첫 바깥 점(T, S > 0) 사이에 면이 있다.
	float TIn = T;
	float SIn = S;
	bool bExited = false;
	LOOP
	for (int Step = 0; Step < SB_FIELD_MAX_STEPS; ++Step)
	{
		TIn = T;
		SIn = S;
		T += max(-S, MinStep);
		if (T >= ExitLimit)
		{
			T = ExitLimit;
			break;
		}
		S = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * T)) * Scale;
		if (S > 0.0)
		{
			bExited = true;
			break;
		}
		// 앞 표지가 다듬기 깊이 너머에서 들어온 부위는 안을 지난 깊이를 구간 전체에서 잰다(지금 규칙은 찾는 깊이가 다듬기 깊이 안이라 같다).
		Closest = T <= Reach || Entry >= Reach ? min(Closest, S) : Closest;
	}
	if (bExited)
	{
		float TOut = T;
		float SOut = S;
		UNROLL
		for (int Split = 0; Split < SB_FIELD_EXIT_SPLITS; ++Split)
		{
			float TMid = 0.5 * (TIn + TOut);
			float SMid = SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, P + Dir * TMid)) * Scale;
			if (SMid > 0.0)
			{
				TOut = TMid;
				SOut = SMid;
			}
			else
			{
				TIn = TMid;
				SIn = SMid;
				Closest = TMid <= Reach || Entry >= Reach ? min(Closest, SMid) : Closest;
			}
		}
		T = TIn + (TOut - TIn) * saturate(-SIn / max(SOut - SIn, 1e-6)) + SB_FIELD_EXIT_OVERLAP * Voxel;
	}
	// 다듬기가 걸리는 칸만 부위 안 구간(들어간 곳 ~ 나온 곳, 다듬기를 재는 깊이까지)에서 가장 깊은 곳을 다시 찾는다(SB_FIELD_DEEPEST_READS).
	// 가장자리 부호 거리(Edge)는 걸음이 본 값 그대로 둔다 — 벽 자리는 0 가까운 값만 쓰고 거기서는 둘이 같다.
	float Margin = -Closest;
	// 다시 찾는 칸: 다듬기 띠 안(여유 < 폭)과, 벽 몫 어림(나오는 깊이 − 여유)이 2칸 아래인 가장자리 띠 칸(ADR-0035 — 걸음 오차 1.4 mm가
	// 벽 없음 양이 바뀌는 벽 몫 1~2칸과 같은 크기라 어림으로는 못 잰다. 벽 몫이 2칸 넘는 칸은 오차가 있어도 양이 0으로 같다).
	bool bNoWallCandidate = Margin < SB_EDGE_RANGE * TexelSize && T - Margin < 2.0 * TexelSize;
	if (((EdgeTrimWidth > 0.0 && Margin < EdgeTrimWidth) || bNoWallCandidate) && Entry < Reach)
	{
		Margin = max(Margin, -SB_FieldRayDeepest(Instance, P, Dir, Scale, Entry, min(T, Reach)));
	}
	float Trimmed = SB_TrimmedDepth(T, Margin);
	float Through = Trimmed * PressGain;
	// 피부 자리 깊이는 살 두께에 붙이지 않고 부위 면을 따라갈 수 있는 만큼(SB_FollowLimit, 일지 299). 따라가는 끝 안에서 나오면 나가는 곳,
	// 그 너머거나 못 나오면 피부 점에서 cos ψ를 잰다(가장 가까운 면 — 거리장은 기울기 1). cos ψ는 거리를 광선 쪽으로 반 칸 간격 중앙 차분한 값이다.
	// 전에는 나가는 곳을 찾은 마지막 구간의 할선이었는데, 그 구간 폭이 걸음에 따라 바뀌어 출발점이 조금만 움직여도 값이 뛰었다(가슴골 손 누름이
	// 덩어리 되먹임에서 흔들림). 반 칸 간격은 삼선형 거리의 기울기가 칸 경계에서 꺾이는 것도 고르게 한다.
	// 너머 갈래는 SB_ShapeFollow와 같다: 피부 점이 부위 안이면 가장 가까운 면의 기울기 크기로, 밖이면(다 묻힌 부위) 나가는 면에서(일지 318).
	bool bReach = bExited && T <= SB_PRESS_OVERSHOOT * Ray.Support;
	bool bAtExit = bReach || (bExited && Surface >= 0.0);
	float3 At = bAtExit ? P + Dir * T : P;
	float Eps = 0.5 * Voxel;
	float CosPsi = SB_FarCos((SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At + Dir * Eps))
		- SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At - Dir * Eps))) * Scale / (2.0 * Eps), bAtExit);
	float Follow = SB_FollowLimit(T, (bAtExit ? T * CosPsi * (bReach ? 1.0 : SB_BuriedShare(Entry, Own, bFront, FrontShare)) : max(-Surface, 0.0)) * CosPsi, Layer);
	float Drawn = min(Trimmed, Follow);
	float2 Pressed = float2(Drawn * PressGain, Through);
	// 덩어리를 미는 겹침과 방향(일지 317, SB_ShapePush와 같은 뜻): cos ψ를 잰 곳(나가는 곳, 닿을 수 없으면 피부 점)에서 거리의 기울기로 면 노멀을 잰다.
	float4 Pushing = float4(-Dir, Through);
	if (Ray.bPush)
	{
		float3 Gradient = float3(
			SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At + float3(Eps, 0, 0)))
				- SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At - float3(Eps, 0, 0))),
			SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At + float3(0, Eps, 0)))
				- SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At - float3(0, Eps, 0))),
			SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At + float3(0, 0, Eps)))
				- SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, At - float3(0, 0, Eps))));
		float Size = length(Gradient);
		float3 Away = Size > 1e-8 ? -Gradient / Size : -Dir;
		Pushing = float4(Away, (bReach ? Trimmed * saturate(dot(Away, -Dir)) : min(Trimmed, max(-Surface, 0.0))) * PressGain);
	}
	// 앞 표지: 코어 너머 잇기 대신 기둥이 들어온 방향과 이루는 각만큼(칸·표지마다 정해진 값 — 손가락 옆·틈이 지나가도 흔들리지 않는다).
	float Fade = bFront ? max(SB_CoreFade(Entry, Own), FrontShare) : SB_CoreFade(Entry, Own);
	Edge = bEdge ? max(Edge, max(-Closest, 0.0) * Fade) : Edge;
	// 벽 몫(ADR-0035)을 재는 두 깊이: 나오는 깊이와 그 가운데 안을 지난 깊이. 콜라이더마다 최댓값으로 합친다(SB_PressedDepth).
	Wall = max(Wall, float2(T, clamp(Margin, 0.0, T)) * (PressGain * Fade));
	if (Fade >= 1.0)
	{
		Ray.Push = Pushing;
		return Pressed;
	}
	Ray.Push = float4(Pushing.xyz, lerp(-ClosestLayer * MissScale, Pushing.w, Fade));
	float Room = bContactLongRoom != 0u ? SB_FieldRoom(Instance, P, -Dir, Scale, Surface, Layer) : Surface;
	return lerp(float2(-Room, -ClosestLayer) * MissScale, Pressed, Fade);
}

// --- 들어온 면 표지: 길 검사와 콜라이더마다 표지 (위 SB_ColumnAt 뒤의 절) ---

/** SB_PreviousOnCollider의 역: 지난 갱신의 점 X가 콜라이더(형상 번호, 또는 NumShapes + 부위 번호)에 붙어 이번 갱신까지 따라왔다면의 자리. */
float3 SB_FollowCollider(uint Collider, float3 X)
{
	float4 R0;
	float4 R1;
	float4 R2;
	if (Collider < NumShapes)
	{
		R0 = ShapeMotion[Collider * 3 + 0];
		R1 = ShapeMotion[Collider * 3 + 1];
		R2 = ShapeMotion[Collider * 3 + 2];
	}
	else
	{
		uint Row = (Collider - NumShapes) * 3;
		R0 = FieldMotion[Row + 0];
		R1 = FieldMotion[Row + 1];
		R2 = FieldMotion[Row + 2];
	}
	// 지난 자리 = A·X + b (A의 행 = R.xyz, b = R.w). A⁻¹의 열은 행끼리의 외적 / 행렬식.
	float3 V = X - float3(R0.w, R1.w, R2.w);
	float3 C0 = cross(R1.xyz, R2.xyz);
	float3 C1 = cross(R2.xyz, R0.xyz);
	float3 C2 = cross(R0.xyz, R1.xyz);
	float Det = dot(R0.xyz, C0);
	return (V.x * C0 + V.y * C1 + V.z * C2) / (abs(Det) > 1e-12 ? Det : 1.0);
}

float SB_FieldDistanceAt(uint Instance, float Scale, float3 X)
{
	return SB_FieldDistanceLocal(Instance, SB_FieldToLocal(Instance, X)) * Scale;
}

/**
 * 길 검사(형상): 콜라이더에서 본 이 점의 길 — 지난 갱신 광선 출발점을 콜라이더에 붙여 옮긴 자리 A에서 지금 출발점 B까지 — 이 콜라이더 겉을 지나
 * 들어갔나. 1.6 cm 손가락이 한 갱신에 3 cm 움직여 두 끝이 다 밖이어도 잡는다. A가 이미 안이면 지나간 것이 아니다.
 */
bool SB_ShapeSwept(uint Shape, float3 A, float3 B)
{
	float3 D = B - A;
	float Len = length(D);
	if (Len < 1e-4)
	{
		return false;
	}
	D /= Len;
	float2 I = SB_ShapeRayInterval(Shape, A, D);
	return I.x <= I.y && I.x > 0.0 && I.x <= Len;
}

/** 길 검사(거리장 부위): SB_ShapeSwept와 같은 뜻, 거리만큼 건너뛰는 걸음으로 들어가는 곳을 찾는다. */
bool SB_FieldSwept(uint Instance, float Scale, float3 A, float3 B)
{
	float3 D = B - A;
	float Len = length(D);
	if (Len < 1e-4)
	{
		return false;
	}
	D /= Len;
	float Voxel = FieldInstances[Instance * SB_FIELD_ROWS + 3].w * Scale;
	float MinStep = max(0.25 * Voxel, 0.01);
	float S = SB_FieldDistanceAt(Instance, Scale, A);
	if (S <= 0.0)
	{
		return false;
	}
	float T = 0.0;
	LOOP
	for (int Step = 0; Step < SB_FIELD_MAX_STEPS; ++Step)
	{
		T = min(T + max(S, MinStep), Len);
		S = SB_FieldDistanceAt(Instance, Scale, A + D * T);
		if (S <= 0.0)
		{
			return true;
		}
		if (T >= Len)
		{
			break;
		}
	}
	return false;
}

/**
 * 지키기(형상): 쉬는 피부 점 Point에서 들어온 방향 U로 쏜 광선이 Reach 안에서 형상에 들거나(점이 안이어도) Margin 안을 스치는가 —
 * 콜라이더가 아직 이 점 "밑"(들어온 쪽)에 있다.
 */
bool SB_ShapeCover(uint Shape, float3 Point, float3 U, float Reach, float Margin)
{
	float2 I = SB_ShapeRayInterval(Shape, Point, U);
	return (I.x <= I.y && I.y > 0.0 && I.x < Reach) || SB_ShapeRayClosest(Shape, Point, -U, 0.0, Reach) < Margin;
}

/** 지키기(거리장 부위): SB_ShapeCover와 같은 뜻, 거리만큼 건너뛰는 걸음으로 잰다(부위를 건너뛰지 않는다). */
bool SB_FieldCover(uint Instance, float Scale, float3 Point, float3 U, float Reach, float Margin)
{
	float Voxel = FieldInstances[Instance * SB_FIELD_ROWS + 3].w * Scale;
	float MinStep = max(0.25 * Voxel, 0.01);
	float T = 0.0;
	float S = SB_FieldDistanceAt(Instance, Scale, Point);
	LOOP
	for (int Step = 0; Step < SB_FIELD_MAX_STEPS; ++Step)
	{
		if (S < Margin)
		{
			return true;
		}
		T += max(S, MinStep);
		if (T > Reach)
		{
			break;
		}
		S = SB_FieldDistanceAt(Instance, Scale, Point + U * T);
	}
	return false;
}

/**
 * 사건 때 들어온 방향 코드(SB_LabelDirCode)를 정한다: 콜라이더가 이 갱신에 이 점에 대해 움직인 쪽(A − 이번 점, A = 콜라이더에 붙어 옮겨진 지난 점).
 * 돌려주는 값: false = 표지를 달지 않는다 — 콜라이더가 이 면을 따라 또는 밖으로 지나갔거나(그 쪽이 안쪽 노멀과 60° 넘게 기울었다), 거의 안 움직였다.
 */
bool SB_LabelEntry(FSBLabels L, float3 A, out uint Code)
{
	Code = 0u;
	float3 Move = A - L.Point;
	float Len = length(Move);
	if (Len < SB_LABEL_MOVE)
	{
		return false;
	}
	float3 U = Move / Len;
	if (dot(U, -L.N) < SB_LABEL_ENTER_COS)
	{
		return false;
	}
	Code = SB_LabelDirCode(L, U);
	return true;
}

/**
 * 형상 하나의 앞 표지: 지난 표지가 있으면 지키거나(쉬는 점이 들어온 방향으로 아직 형상 밑이면) 지우고, 없으면 사건(쉬는 피부 점을 품음, 또는 한
 * 갱신에 그 점을 지나 들어감)에서 단다. 돌려주는 값: true = 앞 표지가 있다(깊이와 상관없이 찾는다). Share = 제 살 깊이 너머에서 따라가는 몫(SB_LabelShare).
 */
bool SB_ShapeLabel(uint Shape, inout FSBLabels L, out float Share)
{
	Share = 0.0;
	uint Key = bLabels != 0u ? ColliderIds[Shape] : SB_ID_NONE;
	uint Id = Key & 0x7fu;
	if (Id == SB_ID_NONE)
	{
		return false;
	}
	int Slot = SB_LabelSlot(L, Id);
	if (Slot >= 0)
	{
		// 지킬지는 콜라이더마다 한 번 정해 두었다(SB_LabelsCover).
		if (((L.Kept >> (uint)Slot) & 1u) != 0u)
		{
			Share = SB_LabelShare(SB_LabelOldDir(L, Slot));
			return true;
		}
		return false;
	}
	// 사건: 지난 점(콜라이더에 붙여 옮긴 자리 A)은 밖이었고 지금 점은 안이거나, A에서 지금 점까지 겉을 지나갔다. 지난 자리·지난 자세를 모르면(처음 겹침)
	// 달지 않는다 — 그 점은 표지 없이도 눌리고, 순간이동한 처음 겹침이 지금 규칙과 같다.
	bool bEnter = false;
	uint Dir = 0u;
	if (L.bPath != 0u && (Key & SB_ID_NEW) == 0u)
	{
		float3 A = SB_FollowCollider(Shape, L.Before);
		float Gap = SB_ShapeSignedDistance(Shape, L.Point);
		bEnter = SB_ShapeSignedDistance(Shape, A) > 0.0 && (Gap <= 0.0 || (Gap <= length(L.Point - A) && SB_ShapeSwept(Shape, A, L.Point)));
		if (bEnter)
		{
			bEnter = SB_LabelEntry(L, A, Dir);
		}
	}
	if (!bEnter)
	{
		return false;
	}
	SB_LabelAdd(L, Id, Dir);
	Share = SB_LabelShare(Dir);
	return true;
}

/**
 * 거리장 부위 하나를 앞 표지와 함께 잰다(SB_FieldRay를 감싼다). 번호가 없는 부위는 지금 규칙 그대로. 앞 표지가 있고 쉬는 점이 들어온 방향으로 아직
 * 이 부위 밑이면 살 두께 + Max Contact Depth까지 찾고 제 살 깊이 너머에서 SB_LabelShare만큼 따라간다. 표지가 없으면 지금 규칙으로 재고, 사건(쉬는
 * 피부 점을 품음, 또는 한 갱신에 그 점을 지나 들어감)이면 표지를 단다.
 */
float2 SB_FieldLabeled(uint Collider, uint Instance, float3 P, float3 N, float Layer, float Own, float Scale, float PressGain, float MissScale,
	float Support, bool bEdge, inout float Edge, inout float2 Wall, inout FSBLabels L, bool bPush, out float4 Pushing)
{
	FSBRay Ray = (FSBRay)0;
	Ray.bPush = bPush;
	Ray.Support = Support;
	uint Key = bLabels != 0u ? ColliderIds[Collider] : SB_ID_NONE;
	uint Id = Key & 0x7fu;
	if (Id == SB_ID_NONE)
	{
		float2 Plain = SB_FieldRay(Instance, P, -N, Layer, Own, Scale, PressGain, MissScale, bEdge, Edge, Wall, 0.0, 0.0, Ray);
		Pushing = Ray.Push;
		return Plain;
	}
	int Slot = SB_LabelSlot(L, Id);
	if (Slot >= 0)
	{
		// 앞 표지: 쉬는 점이 들어온 방향으로 아직 이 콜라이더 밑이면(SB_LabelsCover가 콜라이더마다 한 번 정해 둔다) 따라가는 깊이까지 잰다. 아니면(손이
		// 옆으로 비켜났으면) 지금 규칙이고 표지는 지워진다.
		float2 Again;
		if (((L.Kept >> (uint)Slot) & 1u) != 0u)
		{
			Again = SB_FieldRay(Instance, P, -N, Layer, Own, Scale, PressGain, MissScale, bEdge, Edge, Wall, L.Reach,
				SB_LabelShare(SB_LabelOldDir(L, Slot)), Ray);
		}
		else
		{
			Again = SB_FieldRay(Instance, P, -N, Layer, Own, Scale, PressGain, MissScale, bEdge, Edge, Wall, 0.0, 0.0, Ray);
		}
		Pushing = Ray.Push;
		return Again;
	}
	// 처음 드는 부위: 지금 규칙으로 잰다.
	float EdgeNow = Edge;
	float2 WallNow = Wall;
	float2 Now = SB_FieldRay(Instance, P, -N, Layer, Own, Scale, PressGain, MissScale, bEdge, EdgeNow, WallNow, 0.0, 0.0, Ray);
	// 사건: 지난 점(이 부위에 붙여 옮긴 자리 A)은 밖이었고 지금 점은 안이거나, A에서 지금 점까지 겉을 지나갔다(형상과 같은 뜻).
	bool bEnter = false;
	uint Dir = 0u;
	if (L.bPath != 0u && (Key & SB_ID_NEW) == 0u)
	{
		float3 A = SB_FollowCollider(Collider, L.Before);
		float Gap = SB_FieldDistanceAt(Instance, Scale, L.Point);
		bEnter = SB_FieldDistanceAt(Instance, Scale, A) > 0.0
			&& (Gap <= 0.0 || (Gap <= length(L.Point - A) && SB_FieldSwept(Instance, Scale, A, L.Point)));
		if (bEnter)
		{
			bEnter = SB_LabelEntry(L, A, Dir);
		}
	}
	if (bEnter)
	{
		// 사건은 피부 점의 일이라 광선이 이 부위에 들든 말든 단다(끌린 출발점의 광선이 스쳐 비껴가도). 제 살 깊이 안에서 온전히 누르면 지금 규칙과
		// 앞 표지의 값이 같고, 아니면 따라가는 깊이까지 다시 잰다.
		SB_LabelAdd(L, Id, Dir);
		if (!(Ray.bEntered && Ray.Entry <= Own))
		{
			float EdgeFront = Edge;
			float2 WallFront = Wall;
			FSBRay Far = (FSBRay)0;
			Far.bPush = bPush;
			Far.Support = Support;
			float2 Front = SB_FieldRay(Instance, P, -N, Layer, Own, Scale, PressGain, MissScale, bEdge, EdgeFront, WallFront, L.Reach,
				SB_LabelShare(Dir), Far);
			if (Far.bEntered && Far.Entry < L.Reach)
			{
				Edge = EdgeFront;
				Wall = WallFront;
				Pushing = Far.Push;
				return Front;
			}
		}
	}
	Edge = EdgeNow;
	Wall = WallNow;
	Pushing = Ray.Push;
	return Now;
}

// --- 콜라이더 묶음 거르기 (ADR-0013) ---
// 스레드 묶음(8×8칸, 표면에서 1 cm 남짓)마다 칸 광선 출발점들을 감싸는 상자를 만들고, 그 상자에서 (묶음의 가장 두꺼운 살 +
// MaxContactDepth)보다 먼 형상·부위는 묶음 전체가 건너뛴다. 부위는 칸마다 하던 거리 검사(SB_FieldDepth의 SB_FieldBoxGap)를
// 상자 전체에 대해 한 번 하는 것이라 결과가 같다. 형상은 원래 칸마다 모두 돌았는데, 그만큼 먼 형상은 누르지 못하고 솟는 살의
// 한도(위로 빈 높이의 음수 — 형상까지 거리보다 크거나 같다)만 주므로 "막는 것 없음"과 화면이 같다.
#define SB_MAX_GROUP_SHAPES 32
#define SB_MAX_FIELD_WORDS 16
groupshared uint SB_GroupLo[3];
groupshared uint SB_GroupHi[3];
groupshared uint SB_GroupLayer;
groupshared uint SB_GroupShapeMask;
groupshared uint SB_GroupFieldMask[SB_MAX_FIELD_WORDS];
groupshared uint SB_GroupPressed;
groupshared uint SB_GroupDeepCount;   // 살 두께보다 깊이 눌린 표면 칸 수(일지 295)
groupshared uint SB_GroupDeepMost;    // 그 가운데 가장 깊이 넘은 양(cm, 양수 float 비트는 uint 크기 순서와 같다)
groupshared uint SB_GroupLabelOverflow;   // 들어온 면 표지 자리가 모자란 표면 칸 수

/** 부호 있는 float을 크기 순서가 같은 uint로(공유 메모리 원자 최소·최대용). */
uint SB_OrderedFromFloat(float F)
{
	uint U = asuint(F);
	return (U & 0x80000000u) != 0u ? ~U : (U | 0x80000000u);
}

float SB_FloatFromOrdered(uint U)
{
	return asfloat((U & 0x80000000u) != 0u ? (U & 0x7fffffffu) : ~U);
}

void SB_GroupInclude(float3 P)
{
	UNROLL
	for (int k = 0; k < 3; ++k)
	{
		uint Ordered = SB_OrderedFromFloat(P[k]);
		InterlockedMin(SB_GroupLo[k], Ordered);
		InterlockedMax(SB_GroupHi[k], Ordered);
	}
}

/**
 * 묶음 상자(프레임 cm) 안 어느 점에서도 부위 격자 상자까지의 거리가 이보다 작지 않다(메시 로컬 단위). 상자를 부위 로컬로 옮긴
 * 상자(아핀 변환이라 반폭 = |행| · 반폭)와 격자 상자 사이 거리다.
 */
float SB_FieldGroupGap(uint Instance, float3 Lo, float3 Hi)
{
	uint Row = Instance * SB_FIELD_ROWS;
	float3 Center = SB_FieldToLocal(Instance, 0.5 * (Lo + Hi));
	float3 Half = 0.5 * (Hi - Lo);
	float3 Extent = float3(dot(abs(FieldInstances[Row + 0].xyz), Half), dot(abs(FieldInstances[Row + 1].xyz), Half),
		dot(abs(FieldInstances[Row + 2].xyz), Half));
	float4 Grid = FieldInstances[Row + 3];
	float4 Dims = FieldInstances[Row + 4];
	float3 BoxLo = Grid.xyz;
	float3 BoxHi = Grid.xyz + (Dims.xyz - 1.0) * Grid.w;
	float3 Gap = max(max(BoxLo - (Center + Extent), (Center - Extent) - BoxHi), 0.0);
	return length(Gap);
}

/** 묶음 상자 안 어느 점에서도 형상까지의 거리가 이보다 작지 않다(cm). 컨벡스는 평면 거리가 정확한 거리가 아니라 늘 가깝다고 본다. */
float SB_ShapeGroupGap(uint Shape, float3 Lo, float3 Hi)
{
	if ((uint)round(Shapes[Shape * 3 + 0].w) == SB_SHAPE_CONVEX)
	{
		return -1e20;
	}
	return SB_ShapeSignedDistance(Shape, 0.5 * (Lo + Hi)) - length(0.5 * (Hi - Lo));
}

/** 묶음이 볼 형상·부위를 고른다(묶음의 64스레드가 나눠 검사). 묶음에 칸이 없으면 아무것도 고르지 않는다. */
void SB_GroupCull(uint LocalIndex)
{
	if (SB_GroupLayer == 0u)
	{
		return;
	}
	float3 Lo = float3(SB_FloatFromOrdered(SB_GroupLo[0]), SB_FloatFromOrdered(SB_GroupLo[1]), SB_FloatFromOrdered(SB_GroupLo[2]));
	float3 Hi = float3(SB_FloatFromOrdered(SB_GroupHi[0]), SB_FloatFromOrdered(SB_GroupHi[1]), SB_FloatFromOrdered(SB_GroupHi[2]));
	float Reach = asfloat(SB_GroupLayer) + MaxContactDepth;
	uint GroupShapes = min(NumShapes, (uint)SB_MAX_GROUP_SHAPES);
	for (uint Shape = LocalIndex; Shape < GroupShapes; Shape += 64u)
	{
		if (SB_ShapeGroupGap(Shape, Lo, Hi) <= Reach)
		{
			InterlockedOr(SB_GroupShapeMask, 1u << Shape);
		}
	}
	uint GroupInstances = min(NumFieldInstances, (uint)SB_MAX_FIELD_WORDS * 32u);
	for (uint Instance = LocalIndex; Instance < GroupInstances; Instance += 64u)
	{
		if (SB_FieldGroupGap(Instance, Lo, Hi) * FieldInstances[Instance * SB_FIELD_ROWS + 5].x <= Reach)
		{
			InterlockedOr(SB_GroupFieldMask[Instance >> 5u], 1u << (Instance & 31u));
		}
	}
}

/**
 * 모든 거리장 부위를 Depth에 합친다. P·N은 현재 프레임 위치·바깥 노멀, RestP는 기본 포즈 메시 로컬 위치.
 * Depth를 키운 부위는 Winner에 NumShapes + 부위 번호로 적는다(끌림이 그 부위의 움직임을 쓴다).
 * bPush면 덩어리를 미는 겹침이 가장 큰 부위의 겹침·방향을 Push에 합친다(SB_PressedDepth).
 */
float SB_FieldDepth(float3 P, float3 N, float3 RestP, float Layer, float Own, float Depth, inout uint Winner, inout float Through,
	inout float Edge, inout float2 Wall, inout FSBLabels L, bool bPush, inout float4 Push)
{
	// 스레드 묶음이 거른 부위만 번호 순서대로 돈다(SB_GroupCull). 칸마다 하는 거리 검사는 그대로라 결과는 같다.
	uint NumWords = min((NumFieldInstances + 31u) >> 5u, (uint)SB_MAX_FIELD_WORDS);
	LOOP
	for (uint Word = 0; Word < NumWords; ++Word)
	{
		uint Mask = SB_GroupFieldMask[Word];
		LOOP
		while (Mask != 0u)
		{
			uint Instance = Word * 32u + firstbitlow(Mask);
			Mask &= Mask - 1u;
			float4 Info = FieldInstances[Instance * SB_FIELD_ROWS + 5];   // cm 배율, 자기 몸 부위 번호(외부 -1)
			float Scale = Info.x;
			float3 Q = SB_FieldToLocal(Instance, P);
			// 먼 부위는 건너뛴다: 외부는 광선이 닿을 수 없고(살 두께 너머), 자기 몸 부위는 살 두께보다 멀면 영향이 없다.
			if (SB_FieldBoxGap(Instance, Q) * Scale > Layer + MaxContactDepth)
			{
				continue;
			}
			float PressGain = 1.0;
			float MissScale = 1.0;
			bool bEdge = true;
			// 피부가 부위 면을 따라갈 수 있는 깊이: 바깥 콜라이더는 Max Contact Depth(일지 299). 자기 몸 부위는 살 두께(뼈 표면)에서 멈춘다(일지 324) —
			// 그 너머 겹침은 스키닝이 겹쳐 놓은 살이다. 관절을 건너 누르는 본(허벅지가 엉덩이를, 아랫마디가 윗마디를)이 깊게 겹친 만큼 피부를 밀면
			// 뼈 너머 반대편까지 가서 겉면이 접혔고(10~18 cm), 깊이만 자르면 광선이 상대 마디 반대편 면(10 cm 넘게)에서 잰 기울기로 따라갈 깊이를
			// 정해 칸마다 들쭉날쭉한 주름이 남았다. 끝이 살 두께가 되게 SB_PressLimit의 Exact를 살 두께 ÷ 1.3로 둔다.
			float Support = Info.y >= 0.0 ? Layer / SB_PRESS_OVERSHOOT : MaxContactDepth;
			if (Info.y >= 0.0)
			{
				// 누르는 부위는 외부 콜라이더와 같은 광선 규칙으로 누른다. 기본 포즈에서 이 칸이 그 부위와 살 두께 안에 붙어 있었으면
				// (손목 옆 팔뚝, 손가락 마디 사이, 그 부위 자신의 살) 누르지 않고, 두 배 거리까지 서서히 누르게 한다 — 부위는 본을
				// 따라 딱딱하게 움직여 관절을 굽히면 옆 살 속으로 겹쳐 들어가는데, 그건 스키닝이 섞은 살이지 눌린 것이 아니다.
				// 겹친 깊이는 받되(누르는 부위끼리도 — 누르는 부위는 눌림 0 가까이 칠한다) 살 두께에서 멈춘다(아래 광선 뒤).
				float Rest = -SB_FieldDistanceLocal(Instance, RestP) * Scale;
				float Apart = saturate(-Rest / Layer - 1.0);
				if (Apart <= 0.0)
				{
					continue;
				}
				// 광선이 비껴가면 비껴간 값(솟는 살의 한도와 덩어리 틈, 둘 다 음수)을 Apart로 나눠, 붙어 있던 살에 가까울수록 한도가
				// 멀어져 사라지게 한다(건너뛰는 칸과 이어지게).
				// 가장자리 부호 거리(ADR-0029)는 기본 포즈에서 살 두께의 두 배 넘게 떨어져 있던 부위만 낸다 — 붙어 있던 부위가 낸 안쪽은
				// 함몰 벽이 아닌 곳에 가장자리를 세운다.
				PressGain = Apart;
				MissScale = 1.0 / max(Apart, 1e-3);
				bEdge = Apart >= 1.0;
			}
			// 바깥 콜라이더와 자기 몸 부위가 같은 들어온 면 표지 길을 지난다.
			float4 Pushing = float4(N, SB_NO_OBSTACLE);
			float2 Pressed = SB_FieldLabeled(NumShapes + Instance, Instance, P, N, Layer, Own, Scale, PressGain, MissScale, Support, bEdge, Edge, Wall,
				L, bPush, Pushing);
			// 자기 몸 부위의 깊이도 그 곡선으로 살 두께에 붙인다(바깥 콜라이더는 아래 합치기 뒤에 Max Contact Depth로). 음수(비껴간 틈)는 그대로.
			if (Info.y >= 0.0)
			{
				Pressed = float2(Pressed.x > 0.0 ? SB_PressLimit(Pressed.x, Support) : Pressed.x, Pressed.y > 0.0 ? SB_PressLimit(Pressed.y, Support) : Pressed.y);
			}
			Winner = Pressed.x > Depth ? NumShapes + Instance : Winner;
			Depth = max(Depth, Pressed.x);
			Through = max(Through, Pressed.y);
			Push = bPush && Pushing.w > Push.w ? Pushing : Push;
		}
	}
	return Depth;
}

// --- 끌림 (ADR-0008) ---
#define SB_NO_WINNER 0xffffffffu

/** 누른 콜라이더(Winner: 형상 번호, 또는 NumShapes + 거리장 부위 번호)에 붙은 점 X의 지난 프레임 위치. */
float3 SB_PreviousOnCollider(uint Winner, float3 X)
{
	float4 X1 = float4(X, 1.0);
	if (Winner < NumShapes)
	{
		uint Row = Winner * 3;
		return float3(dot(X1, ShapeMotion[Row + 0]), dot(X1, ShapeMotion[Row + 1]), dot(X1, ShapeMotion[Row + 2]));
	}
	uint Row = (Winner - NumShapes) * 3;
	return float3(dot(X1, FieldMotion[Row + 0]), dot(X1, FieldMotion[Row + 1]), dot(X1, FieldMotion[Row + 2]));
}

// --- 접촉 패스가 PositionOut.w에 적는 값의 규약(ADR-0012 §8, 일지 201). 적는 곳과 읽는 곳이 모두 이 함수들을 거친다 ---
// w = 1 + 누른 것의 번호(검수용: 안 눌리면 0, 거리장 부위면 1 + 인스턴스 번호, 형상이면 1001 + 형상 번호). 읽는 쪽은 "눌렸는가"(2 이상)만 본다.
// 캡처 분석(Position.exr의 A)은 번호를 그대로 읽는다. (일지 201까지 있던 미끄러짐 0.5 표지는 붙음·미끄러짐 판정이 덩어리 평형 안으로 들어가 없어졌다.)
float SB_PackTouch(float Presser)
{
	return 1.0 + Presser;
}

/** PositionOut.w가 "눌린 칸"인가(안 눌린 칸은 1). */
bool SB_TouchPressed(float Touch)
{
	return Touch >= 1.75;
}

// --- 덩어리 (ADR-0012) ---
// 가슴·엉덩이처럼 뼈대에서 뺀 본에 매달린 살. 누르면 관성 스프링이 덩어리를 통째로 비켜나게 하고(CPU), 그만큼 덜 파인다.
Texture2D<float4> CarrierLump;          // RGB 바깥 노멀 쪽으로 밀리는 정도(칠 × 노멀·무름_x/y/z, 기본 자세 축), A 덩어리 묶음 + 0.99 × 무게
Texture2D<float4> CarrierLumpSideA;     // 옆 몫(§3.5): RGB 축1·(칠 × 무름_x/y/z), A 축2·(칠 × 무름_x) — 축은 SB_LumpBasis
Texture2D<float4> CarrierLumpSideB;     // RG 축2·(칠 × 무름_y/z), B 1 − Jiggle(그 칸 살의 단단함, 일지 298 — 옛 굽기·흰 살은 0)
uint bLumpSide;                         // 1 = 옆 몫이 구워져 있다(없으면 노멀 몫만 옮긴다)
StructuredBuffer<float4> InertiaBridge; // 관성 브릿지 8 × 9 (SoftbodyComponent::BridgeWidth 주석) — 머티리얼의 ContactData와 같은 값
uint LumpMode;                          // 0 = 덩어리 없음, 1 = 비켜난 자리에서 파임만 잰다, 2 = 덩어리가 안 움직였다면의 깊이도 적는다
float4 ComponentRotation;               // 프레임 → 월드 회전(쿼터니언)
float3 FrameToActor;                    // 프레임 원점(컴포넌트 위치) − 액터 위치 (cm)
/** 덩어리 평형 반복 상태(LumpState, LumpSolveCS 주석)의 읽기. 덩어리 힘·국소 모양 패스가 읽는다. */
StructuredBuffer<float4> LumpIterate;
/**
 * 덩어리 이동이 기울인 노멀(ADR-0018): xyz 화면 노멀 → 덩어리가 옮긴 면 노멀의 회전(축 × sin, 프레임), w 덩어리 이동의 노멀 성분(cm,
 * 나나이트 정점 사이 높이). 접촉 패스가 매 프레임 쓴다(쉬는 동안에도) — 머티리얼은 SurfaceFieldOn = 2일 때 이것을 읽고, 관성 음영은
 * 덩어리 몫을 빼고 그린다. 노멀 패스는 이 면에서 접촉이 더 기울인 몫만 NormalOut에 쓴다.
 */
RWTexture2D<float4> LumpNormalOut;
/** 덩어리 노멀을 재는 이웃 거리의 최대 칸 수. 기본은 그 자리 렌더 삼각형 변 길이 — 정점에서만 정해진 무름이 삼각형마다 꺾이는 것을 고르게 편다. */
#define SB_LUMP_STENCIL_MAX 8
/**
 * 덩어리 칸의 접촉 값(갱신마다 새로 잡는 임시 텍스처 — 접촉 패스가 쓰고 덩어리 힘·묶음 고르기 패스가 SB_UnpackLumpDepth로 읽는다). 반정밀 여섯 값과 미는 방향:
 *  - d0: 덩어리가 안 움직였다면 누른 깊이(cm, 살 두께에 붙이기 전·Max Contact Depth 곡선을 거친 값, 음수 = 틈, SB_LUMP_FAR = 콜라이더가 살 두께보다 멂,
 *    ADR-0017·일지 317)
 *  - 지난 손 닻(LumpAnchor 그대로, 피부 접평면 두 성분 — 지난 갱신에도 눌린 칸만, 아니면 0)
 *  - 손 몫(덩어리 기본 자세 축 cm): 지난 갱신에도 눌린 칸은 손 위 닿은 점이 이번 갱신에 이 칸 피부에 대해 움직인 거리(피부가 본을 따라 움직인 몫은 뺀다,
 *    일지 336), 새로 눌린 칸은 보이는 피부 자리(끌림 + 덩어리 옆 이동 — ADR-0012 §7, 일지 201). 마찰 면에 비추는 것은 덩어리 힘 패스가 이번 갱신의 면으로
 *    한다(일지 338): 손 부위마다 모으는 미는 방향 평균은 이 패스가 다 끝나야 나온다.
 *  - 미는 방향(SB_PackPushDir, 덩어리 기본 자세 축 — 일지 317)
 * 일지 338 전에는 반정밀 RGBA 한 장(d0, 면에 비춘 닻 두 성분, 미는 방향)이라 접촉 패스가 면을 지난 갱신 값으로 정해 닻을 옛 면에서 새 면으로 옮겼다.
 */
RWTexture2D<uint4> LumpDepthOut;
/** LumpDepthOut의 d0: 이 칸은 덩어리 평형에 들지 않는다(덩어리가 아니거나 콜라이더가 살 두께보다 멂). */
#define SB_LUMP_FAR -10000.0
/** 덩어리 평형 묶음 수(USoftbodySurfaceField::NumLumpGroups). */
#define SB_LUMP_GROUPS 16

// --- 누른 것마다 닿은 크기 (일지 248 ②, ADR-0036) ---
// 손 밑 살이 덩어리를 미는 단단함은 닿은 크기가 정한다. 살이 닿은 반지름 a보다 훨씬 두꺼우면 두께가 아니라 a에 비례한다(반무한체).
// 접촉 패스가 누른 것(형상 → 거리장 부위 순, 광선이 고른 Winner 번호) × 덩어리 묶음마다 SB_PRESSER_WORDS개 값을 ActivityOut의 SB_PRESSER_TABLE_BASE부터
// 쌓는다:
//   [0] 그 묶음의 덩어리 칸 가운데 그것이 누른 칸 수, [1] 그것의 가로 크기 a_pr(cm, 칸들의 최솟값 — SB_PresserBreadth, 비트를 뒤집어 최댓값으로
//   쌓는다: 비운 버퍼 0에서 시작할 수 있게). 최솟값인 까닭: 볼록한 살에서 주머니 가장자리 칸은 노멀이 기울어 손가락 길이까지 비쳐 보인다(가슴 앞
//   20°쯤 — 반지름 1 cm 손가락이 1.5~1.7 cm). 누르는 것의 축과 가장 나란한 칸이 그 단면을 본다.
//   [2..4] 그 칸들의 미는 방향 합(일지 317의 미는 방향, 덩어리 기본 자세 축, 칸마다 무게 1 — 부호 있는 고정소수점 1 = 1/SB_GRIP_DIR_SCALE): 손 마찰 면
//   (SB_GripNormal, 일지 338).
// 덩어리 힘(LumpForceCS)이 같은 갱신에 읽는다(SB_LumpPressStiffness·SB_GripNormal). 덩어리가 없는 캐리어는 표가 없다(버퍼는 첫 칸뿐).
#define SB_ACTIVITY_DEEP 1u
#define SB_ACTIVITY_LABEL_OVERFLOW 3u
#define SB_PRESSER_TABLE_BASE 4u   // C++ SoftbodySurfaceActivityHeaderWords
#define SB_PRESSER_WORDS 5u        // C++ PresserWords
#define SB_GRIP_DIR_SCALE 64.0
/** 접촉 묶음(8×8칸) 하나가 공유 메모리로 모으는 누른 것 수. 넘치는 칸은 전역 버퍼에 바로 더한다. */
#define SB_PRESSER_LOCAL_SLOTS 4u
/**
 * 작은 접촉의 칸 강성 1/min(두께, 이 값 × a). 지금의 넓이 법칙(넓이 × 1/두께)은 넓은 접촉(a ≥ 0.3 × 두께)에서 바닥이 붙은 살층의 물리 강성
 * (축대칭 FEM, ν 0.49)의 0.25~0.34배다. 3.7이면 작은 접촉도 같은 비율이 되어 크기 사이의 비가 물리와 같다(일지 248 ② 설계 메모 §2).
 */
#define SB_CONTACT_REACH 3.7

/**
 * 누르는 것의 가로 크기 a_pr(cm): 노멀 N에 수직인 면에 비친 넓이와 넓이가 같은 원의 반지름. 손가락 끝으로 누르면 손가락 반지름이고,
 * 눕혀 누르면 길이까지 담는다. 구·캡슐·상자는 제 모양 그대로, 컨벡스는 로컬 정점 상자(평면 뒤 한 줄), 거리장 부위는 살이 든 칸을 감싸는
 * 상자(FieldInstances 줄 6 — 격자 상자는 바깥 여백이 붙어 너무 넓다)를 비춘다. 상자는 기울어진 모양을 넓게 잡는다(예산이 덜 걸리는 쪽).
 */
float SB_PresserBreadth(uint Winner, float3 N)
{
	float3 Half;
	float3 Local;
	if (Winner < NumShapes)
	{
		float4 Row0 = Shapes[Winner * 3 + 0];
		float4 Row2 = Shapes[Winner * 3 + 2];
		Local = SB_InverseRotate(Shapes[Winner * 3 + 1], N);
		uint Type = (uint)round(Row0.w);
		if (Type == SB_SHAPE_SPHERE)
		{
			return Row2.x;
		}
		if (Type == SB_SHAPE_CAPSULE)
		{
			// 원 + 축이 노멀에서 기운 만큼의 띠(지름 × 원기둥 길이 × sin).
			float Sin = sqrt(saturate(1.0 - Local.z * Local.z));
			return sqrt(Row2.x * Row2.x + 4.0 * Row2.x * Row2.y * Sin / PI);
		}
		Half = Type == SB_SHAPE_CONVEX ? Planes[(uint)round(Row2.x) + (uint)round(Row2.y)].xyz : Row2.xyz;
	}
	else
	{
		uint Row = (Winner - NumShapes) * SB_FIELD_ROWS;
		Local = float3(dot(N, FieldInstances[Row + 0].xyz), dot(N, FieldInstances[Row + 1].xyz), dot(N, FieldInstances[Row + 2].xyz));
		Local *= rsqrt(max(dot(Local, Local), 1e-12));
		Half = FieldInstances[Row + 6].xyz * FieldInstances[Row + 5].x;
	}
	float3 Tilt = abs(Local);
	return sqrt(4.0 * (Half.y * Half.z * Tilt.x + Half.x * Half.z * Tilt.y + Half.x * Half.y * Tilt.z) / PI);
}

/** 덩어리 무게(스킨 웨이트 중 덩어리 몫, 0 = 덩어리 아님). */
float SB_LumpWeight(float4 Lump)
{
	return saturate(frac(Lump.a) / 0.99);
}

float3 SB_Rotate(float4 Q, float3 V)
{
	return V + 2.0 * cross(Q.xyz, cross(Q.xyz, V) + Q.w * V);
}

/** 관성 브릿지 텍셀. Group < 0 = 스태틱 메시 전체(행0 텍셀4~7), 그 밖 = 스켈레탈 그룹(행1부터 그룹당 4텍셀). */
float4 SB_BridgeEntry(int Group, int Entry)
{
	return InertiaBridge[Group < 0 ? 4 + Entry : 8 + Group * 4 + Entry];
}

/**
 * 덩어리 옆 몫의 두 축(ADR-0012 §3.5): 미는 방향 N(단위)에 수직인 두 축. 베이크 LumpBasis와 같은 식(Duff 외 2017 — z의 부호
 * 비트로만 갈린다)이라 같은 16비트 값에서 같은 축이 나온다.
 */
void SB_LumpBasis(float3 N, out float3 T1, out float3 T2)
{
	float Sign = (asuint(N.z) >> 31u) != 0u ? -1.0 : 1.0;
	float A = -1.0 / (Sign + N.z);
	float B = N.x * N.y * A;
	T1 = float3(1.0 + Sign * N.x * N.x * A, Sign * B, -Sign * N.x);
	T2 = float3(B, Sign + N.y * N.y * A, -N.y);
}

/** 덩어리 평형 반복 상태(LumpState)의 묶음마다 줄 수(LumpSolveCS 주석). C++ LumpStateRows. */
#define SB_LUMP_STATE_ROWS 30

/**
 * 이 칸의 덩어리가 지금 옮겨 간 양(프레임 cm). 머티리얼이 그리는 관성 흔들림(SoftbodyKernel.ush SB_InertiaDriverLag)과 같은
 * 브릿지 값·같은 식이다: 칸의 무름 = 노멀 몫(Lump.rgb) + 옆 몫(CarrierLumpSideA·B) → 기본 자세 축의 흔들림 → 묶음 회전으로
 * 월드 → 프레임. 옆 몫이 없는 옛 베이크는 노멀 몫만 지금 노멀 N 쪽으로 옮긴다(덩어리가 옆으로 비켜나면 자국이 따라 미끄러진다).
 * P = 프레임 위치.
 */
float3 SB_LumpOffset(int2 Texel, float4 Lump, float3 P, float3 N)
{
	float Weight = SB_LumpWeight(Lump);
	int Count = (int)round(InertiaBridge[0].w);
	int Group = Count > 0 ? (int)floor(Lump.a) : -1;
	if (Weight <= 0.0 || Group >= Count)
	{
		return float3(0, 0, 0);
	}
	float4 E0 = SB_BridgeEntry(Group, 0);
	float4 E1 = SB_BridgeEntry(Group, 1);
	float4 Rotation = SB_BridgeEntry(Group, 2);
	float4 Pivot = SB_BridgeEntry(Group, 3);
	float3 RelPos = SB_Rotate(ComponentRotation, P) + FrameToActor;
	float3 Arm = SB_InverseRotate(Rotation, RelPos - Pivot.xyz);
	float3 Load = E0.xyz + cross(float3(E0.w, E1.x, E1.y), Arm);
	if (bLumpSide == 0u)
	{
		return N * (Weight * dot(Lump.rgb, Load));
	}
	float3 RestN = normalize(CarrierNormal.Load(int3(Texel, 0)).xyz);
	float3 T1;
	float3 T2;
	SB_LumpBasis(RestN, T1, T2);
	float4 SideA = CarrierLumpSideA.Load(int3(Texel, 0));
	float2 SideB = CarrierLumpSideB.Load(int3(Texel, 0)).xy;
	float3 Lag = RestN * dot(Lump.rgb, Load) + T1 * dot(SideA.xyz, Load) + T2 * dot(float3(SideA.w, SideB), Load);
	return Weight * SB_InverseRotate(ComponentRotation, SB_Rotate(Rotation, Lag));
}

/**
 * 손 밑 살층 강성 배수(재검토 R12 ③, 일지 298): 측정 FEM이 그 칸 살을 1/Jiggle만큼 단단하게 풀므로(뼈 강성 1000에서 자름) 손이 미는 살층도 같다.
 * 굽기가 CarrierLumpSideB.b에 1 − Jiggle을 적는다 — 옛 굽기·흰 살·덩어리 옆 몫이 없는 자산(검정 텍스처)은 0이라 1.
 */
float SB_LumpFleshStiffness(int2 Texel)
{
	return min(1.0 / max(1.0 - CarrierLumpSideB.Load(int3(Texel, 0)).z, 1e-3), 1000.0);
}

/** 프레임 벡터 V를 이 칸 덩어리 묶음의 기본 자세 축(브릿지 회전·구운 무름의 축, SB_LumpOffset의 역)으로 옮긴다. 덩어리가 아니면 0. */
float3 SB_LumpRestVector(float4 Lump, float3 V)
{
	int Count = (int)round(InertiaBridge[0].w);
	int Group = Count > 0 ? (int)floor(Lump.a) : -1;
	if (SB_LumpWeight(Lump) <= 0.0 || Group >= Count)
	{
		return float3(0, 0, 0);
	}
	float4 Rotation = SB_BridgeEntry(Group, 2);
	return SB_InverseRotate(Rotation, SB_Rotate(ComponentRotation, V));
}

/**
 * 덩어리를 미는 방향(단위 벡터, 덩어리 기본 자세 축 — 일지 317)을 반정밀 값 하나(LumpDepthOut)에 담는다. 팔면체 좌표를 축마다 7비트로 묶어 정상
 * 반정밀 값의 비트로 쓴다(0·비정상 수·무한이 없어 쓰기·읽기에서 비트가 그대로다). 방향 오차는 1° 남짓이다 — 접촉 패스와 덩어리 평형이 같은 풀린 값을
 * 쓰므로 둘 사이 어긋남은 없다.
 */
float SB_PackPushDir(float3 V)
{
	V /= max(abs(V.x) + abs(V.y) + abs(V.z), 1e-6);
	float2 Octa = V.z >= 0.0 ? V.xy : (1.0 - abs(V.yx)) * float2(V.x >= 0.0 ? 1.0 : -1.0, V.y >= 0.0 ? 1.0 : -1.0);
	uint2 Cell = (uint2)round(saturate(Octa * 0.5 + 0.5) * 127.0);
	uint Index = Cell.x * 128u + Cell.y;
	return f16tof32((((Index >> 10u) + 1u) << 10u) | (Index & 1023u));
}

/** SB_PackPushDir의 역(단위 벡터). */
float3 SB_UnpackPushDir(float Packed)
{
	uint Bits = f32tof16(Packed);
	uint Index = (((Bits >> 10u) & 31u) - 1u) * 1024u + (Bits & 1023u);
	float2 Octa = float2(Index >> 7u, Index & 127u) * (2.0 / 127.0) - 1.0;
	float3 V = float3(Octa, 1.0 - abs(Octa.x) - abs(Octa.y));
	float Fold = saturate(-V.z);
	V.x += V.x >= 0.0 ? -Fold : Fold;
	V.y += V.y >= 0.0 ? -Fold : Fold;
	return normalize(V);
}

/** LumpDepthOut 한 칸을 묶는다(선언 주석의 차례). 미는 방향은 SB_PackPushDir의 반정밀 비트 그대로, 나머지는 반정밀(일지 338 전 텍스처와 같은 정밀도). */
uint4 SB_PackLumpDepth(float RestDepth, float2 Anchor, float3 Hand, float PushCode)
{
	return uint4(f32tof16(RestDepth) | (f32tof16(Anchor.x) << 16u), f32tof16(Anchor.y) | (f32tof16(Hand.x) << 16u),
		f32tof16(Hand.y) | (f32tof16(Hand.z) << 16u), f32tof16(PushCode));
}

/** SB_PackLumpDepth의 역. */
void SB_UnpackLumpDepth(uint4 Packed, out float RestDepth, out float2 Anchor, out float3 Hand, out float PushCode)
{
	RestDepth = f16tof32(Packed.x);
	Anchor = float2(f16tof32(Packed.x >> 16u), f16tof32(Packed.y));
	Hand = float3(f16tof32(Packed.y >> 16u), f16tof32(Packed.z), f16tof32(Packed.z >> 16u));
	PushCode = f16tof32(Packed.w);
}

// --- 거친 격자 공용 선언 (솟음 ADR-0031·끌림 ADR-0033·함몰 거친 해 일지 199·덩어리 국소 모양 일지 251) ---
// 캐리어 칸 B×B를 거친 칸 하나로 묶은 격자(B = 1·2·4, 거친 칸이 1 cm를 넘지 않게 — 굽기가 거리표와 같이 정한다). 접촉 패스가 국소 모양을
// 읽으므로 여기 둔다. 거리표와 그 읽기는 아래 "거친 격자 공용"에 있다.
uint CoarseResolution;   // 거친 격자 한 변의 칸 수
uint CoarseBlock;        // B: 거친 칸 하나에 묶는 캐리어 칸 수(한 변)
float CoarseTexelSize;   // 거친 칸 크기 (cm)
Texture2D<float4> CoarseInfo;        // xyz 묶음 살 칸 정지 위치의 평균(메시 로컬), w 거친 칸 넓이(메시 로컬², 살 칸 넓이의 합, 0 = 살 없는 칸)

bool SB_InCoarse(int2 Cell)
{
	return all(Cell >= int2(0, 0)) && all(Cell < int2(CoarseResolution, CoarseResolution));
}

/**
 * 가는 칸(정지 위치 RestP)이 거친 칸 Cell의 값을 읽을 때 그 탭의 무게 배수(일지 201 — 쌍선형·B-스플라인 읽기가 같은 규칙을 이 한 곳에서 쓴다).
 * 살 없는 거친 칸(넓이 0)과 격자 밖은 0. 정지 위치가 먼 거친 칸(아틀라스에서 옆에 놓였을 뿐인 다른 차트)은 거친 칸 4개부터 6개 사이에서
 * 서서히 0으로. CoarseLocal = 거친 칸 한 변(메시 로컬 cm). 읽는 쪽은 탭 무게의 합으로 다시 나눈다.
 */
float SB_CoarseTapWeight(int2 Cell, float3 RestP, float CoarseLocal)
{
	if (!SB_InCoarse(Cell))
	{
		return 0.0;
	}
	float4 Info = CoarseInfo.Load(int3(Cell, 0));
	if (Info.w <= 0.0)
	{
		return 0.0;
	}
	return saturate(3.0 - 0.5 * length(Info.xyz - RestP) / CoarseLocal);
}

// --- 덩어리 국소 모양 (일지 251, ③A, C++ SoftbodyLumpRegion) ---
// 덩어리가 출렁임 모양 셋으로만 비켜나면, 그 모양에서 조금만 움직이는 밑동을 눌렀을 때 그 자리를 비키려고 덩어리 전체가 크게 움직여 끝이
// 지렛대처럼 흔들렸다. 굽기가 덩어리마다 원판 누름의 FEM 응답에서 출렁임 모양이 이미 그리는 몫을 빼 고유 모양(SB_REGION_MODES개까지)과 그
// 단단함을 남긴다. 덩어리 평형(LumpForceCS·LumpSolveCS)이 모양마다 크기 q를 같이 풀고, 국소 모양 패스(LumpRegionCS)가 거친 칸마다 지금
// 옮긴 양을 적어 접촉 패스(광선 출발점 — 손과 피부 사이에 틈이 생기지 않게)와 합치기(변위)가 읽는다. 머티리얼은 변위로만 보므로 그대로다.
#define SB_REGION_MODES 16
#define SB_REGION_RECORD_TEXELS 13          // C++ SoftbodyLumpRegion::RecordTexels
#define SB_REGION_RECORDS_PER_ROW 80        // C++ SoftbodyLumpRegion::RecordsPerRow
/** 덩어리 무게가 이 값 아래인 덩어리 가장자리에서 국소 모양을 0으로 잇는다(덩어리 밖에는 줄이 없다). */
#define SB_REGION_EDGE_WEIGHT 0.1
uint bLumpRegion;                    // 1 = 국소 모양이 구워져 있고 켜져 있다(0이면 덩어리는 출렁임 모양으로만 비켜난다 — 전과 같다)
Texture2D<float> LumpRegionIndex;    // 거친 칸마다 줄 번호 + 1(G16 unorm, 0 = 없음)
Texture2D<float4> LumpRegionTable;   // 줄 텍스처: 텍셀 k < 12 = 성분 k / 4(메시 로컬 기본 자세 축 x·y·z)의 모양 4(k % 4) ~ +3, 텍셀 12 R = 덩어리 묶음
Texture2D<float4> LumpRegionOffset;  // LumpRegionCS가 이번 갱신 앞에 쓴 거친 칸마다 국소 모양이 옮긴 양(xyz 프레임 cm, w 1 = 줄이 있다)

/** 거친 칸 Cell의 국소 모양 줄 번호(없으면 −1). */
int SB_RegionRecord(int2 Cell)
{
	return (int)round(LumpRegionIndex.Load(int3(Cell, 0)) * 65535.0) - 1;
}

/** 줄 Record의 텍셀 K. */
float4 SB_RegionTexel(int Record, int K)
{
	int Row = Record / SB_REGION_RECORDS_PER_ROW;
	int Column = (Record - Row * SB_REGION_RECORDS_PER_ROW) * SB_REGION_RECORD_TEXELS + K;
	return LumpRegionTable.Load(int3(Column, Row, 0));
}

/** 가는 칸이 덩어리 가장자리에서 국소 모양을 받는 몫(0~1). */
float SB_RegionEdge(float4 Lump)
{
	return saturate(SB_LumpWeight(Lump) / SB_REGION_EDGE_WEIGHT);
}

/**
 * 가는 칸 자리 Where(칸 좌표 — 고스트 칸은 가리키는 점, 정수 = 칸 한가운데)의 국소 모양이 지금 옮긴 양(프레임 cm): LumpRegionCS가 거친 칸마다 쓴
 * 값의 쌍선형(살 없는 칸·정지 위치가 먼 칸·줄 없는 칸은 빼고 다시 나눈다 — SB_CoarseBilinear와 같은 규칙) × 덩어리 가장자리 몫. 덩어리가 아니면 0.
 */
float3 SB_RegionOffsetAt(float2 Where, float3 RestP, float4 Lump)
{
	if (bLumpRegion == 0u)
	{
		return float3(0, 0, 0);
	}
	float Edge = SB_RegionEdge(Lump);
	if (Edge <= 0.0)
	{
		return float3(0, 0, 0);
	}
	float2 Q = (Where + 0.5) / (float)CoarseBlock - 0.5;
	int2 Base = (int2)floor(Q);
	float2 T = Q - (float2)Base;
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	float3 Sum = float3(0, 0, 0);
	float Weight = 0.0;
	UNROLL
	for (int Y = 0; Y < 2; ++Y)
	{
		UNROLL
		for (int X = 0; X < 2; ++X)
		{
			int2 Cell = Base + int2(X, Y);
			float Near = SB_CoarseTapWeight(Cell, RestP, CoarseLocal);
			if (Near <= 0.0)
			{
				continue;
			}
			float4 Value = LumpRegionOffset.Load(int3(Cell, 0));
			if (Value.w <= 0.0)
			{
				continue;
			}
			float W = (X == 0 ? 1.0 - T.x : T.x) * (Y == 0 ? 1.0 - T.y : T.y) * Near;
			Sum += W * Value.xyz;
			Weight += W;
		}
	}
	return Weight > 1e-6 ? Sum * (Edge / Weight) : float3(0, 0, 0);
}

/**
 * 가는 칸 Texel(정지 위치 RestP, 덩어리 값 Lump)의 국소 모양 SB_REGION_MODES개(메시 로컬 기본 자세 축): 둘레 거친 칸 넷의 줄을 SB_RegionOffsetAt과
 * 같은 탭으로 섞고(평형이 재는 모양과 그리는 모양이 같다) 덩어리 가장자리 몫을 곱한다. 덩어리 묶음이 다른 줄은 뺀다.
 * Shape[k] = 성분 k / 4의 모양 4(k % 4) ~ +3. 모양이 없으면 false(Shape는 0).
 */
bool SB_RegionShapes(int2 Texel, float3 RestP, float4 Lump, out float4 Shape[12])
{
	UNROLL
	for (int Clear = 0; Clear < 12; ++Clear)
	{
		Shape[Clear] = float4(0, 0, 0, 0);
	}
	if (bLumpRegion == 0u)
	{
		return false;
	}
	float Edge = SB_RegionEdge(Lump);
	if (Edge <= 0.0)
	{
		return false;
	}
	int Group = (int)floor(Lump.a);
	float2 Q = (float2(Texel) + 0.5) / (float)CoarseBlock - 0.5;
	int2 Base = (int2)floor(Q);
	float2 T = Q - (float2)Base;
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	float Weight = 0.0;
	UNROLL
	for (int Y = 0; Y < 2; ++Y)
	{
		UNROLL
		for (int X = 0; X < 2; ++X)
		{
			int2 Cell = Base + int2(X, Y);
			float Near = SB_CoarseTapWeight(Cell, RestP, CoarseLocal);
			if (Near <= 0.0)
			{
				continue;
			}
			int Record = SB_RegionRecord(Cell);
			if (Record < 0 || (int)round(SB_RegionTexel(Record, 12).r) != Group)
			{
				continue;
			}
			float W = (X == 0 ? 1.0 - T.x : T.x) * (Y == 0 ? 1.0 - T.y : T.y) * Near;
			UNROLL
			for (int k = 0; k < 12; ++k)
			{
				Shape[k] += W * SB_RegionTexel(Record, k);
			}
			Weight += W;
		}
	}
	if (Weight <= 1e-6)
	{
		return false;
	}
	float Scale = Edge / Weight;
	UNROLL
	for (int j = 0; j < 12; ++j)
	{
		Shape[j] *= Scale;
	}
	return true;
}

/** 국소 모양 16개를 방향 Axis(단위)로 잰 성분 넷씩(모양 4j ~ 4j + 3). */
void SB_RegionAlong(float4 Shape[12], float3 Axis, out float4 Along[4])
{
	UNROLL
	for (int j = 0; j < 4; ++j)
	{
		Along[j] = Shape[j] * Axis.x + Shape[4 + j] * Axis.y + Shape[8 + j] * Axis.z;
	}
}

/** 정지 위치 차이를 프레임으로: 가운데 칸의 스키닝 행렬(본 무게로 섞은 행)로 이웃까지 옮긴다 — 이웃 거리 안에서 스킨 무게가 거의 같다. */
float3 SB_SkinDelta(float3 Delta, float4 Row0, float4 Row1, float4 Row2)
{
	float4 V = float4(Delta, 0.0);
	return float3(dot(V, Row0), dot(V, Row1), dot(V, Row2)) * ComponentScale;
}

/**
 * 칸의 스키닝 행렬: 본 행렬(BoneRows = Bones 또는 PreviousBones)을 칸의 본 무게로 섞은 세 행. 본마다 옮겨 무게로 더한 것과 같다(옮기기가 선형이라
 * 섞은 행으로 한 번에 옮긴다). 쓴 무게의 합으로 나눈다(일지 299): 구운 무게는 반정밀이라 합이 1에서 1e-3쯤 어긋나고, 그 차가 프레임 원점까지 거리에
 * 곱해져 피부 자리가 칸마다 다르게 밀렸다(원점에서 10 m면 2칸 — 벽에 댄 면이 우글거렸다). 엔진 스키닝은 합이 정확히 1이라 나눠야 같은 자리에 선다.
 * 이번 자세와 지난 자세가 같은 셈을 지나므로 본이 그대로면 두 자리가 같다(SB_SkinMotion이 0 — 전에는 나누는 순서가 달라 갱신마다 1e-5 cm쯤 남았다).
 */
void SB_SkinRows(int2 Texel, StructuredBuffer<float4> BoneRows, out float4 Row0, out float4 Row1, out float4 Row2)
{
	float4 Index = CarrierSkinIndex.Load(int3(Texel, 0));
	float4 Weight = CarrierSkinWeight.Load(int3(Texel, 0));
	Row0 = float4(0, 0, 0, 0);
	Row1 = float4(0, 0, 0, 0);
	Row2 = float4(0, 0, 0, 0);
	float WeightSum = 0.0;
	UNROLL
	for (int k = 0; k < 4; ++k)
	{
		if (Weight[k] <= 1e-4)
		{
			continue;
		}
		uint Bone = (uint)round(Index[k]);
		Row0 += BoneRows[Bone * 3 + 0] * Weight[k];
		Row1 += BoneRows[Bone * 3 + 1] * Weight[k];
		Row2 += BoneRows[Bone * 3 + 2] * Weight[k];
		WeightSum += Weight[k];
	}
	if (WeightSum > 0.0)
	{
		float InvSum = 1.0 / WeightSum;
		Row0 *= InvSum;
		Row1 *= InvSum;
		Row2 *= InvSum;
	}
}

/**
 * 칸 피부가 지난 갱신부터 본을 따라 움직인 거리(프레임 cm, 덩어리 이동은 들지 않는다 — 일지 336). P = 이번 갱신의 스키닝 자리(접촉 패스의 P, 스케일 곱한 값),
 * RestP = 기본 포즈 자리. 지난 자리는 지난 갱신의 본 행렬(PreviousBones)로 접촉 패스와 같은 셈(SB_SkinRows)을 지난다. 스키닝이 아니면 0.
 */
float3 SB_SkinMotion(int2 Texel, float3 RestP, float3 P)
{
	if (bSkinned == 0)
	{
		return float3(0, 0, 0);
	}
	float4 Row0;
	float4 Row1;
	float4 Row2;
	SB_SkinRows(Texel, PreviousBones, Row0, Row1, Row2);
	float4 P1 = float4(RestP, 1.0);
	return P - float3(dot(P1, Row0), dot(P1, Row1), dot(P1, Row2)) * ComponentScale;
}

/**
 * 덩어리 이동이 기울인 노멀(ADR-0018). 머티리얼의 관성 음영은 정점에서만 흔들림을 구해 삼각형 안을 직선으로 이으므로 노멀 회전이
 * 삼각형마다 한 값이라, 덩어리가 크게 옮겨 가면 폴리곤 모양의 선이 보였다. 여기서는 덩어리 이동(SB_LumpOffset, 머티리얼과 같은 식)을
 * 이웃 칸에서 구해 변형 전후 접선으로 노멀을 옮긴다(노멀 패스와 같은 역전치 식). 이웃 거리는 그 자리 렌더 삼각형 변 길이 — 삼각형
 * 하나를 넘겨 재므로 칸마다 고르게 이어진다. 이웃 칸의 위치는 가운데 칸의 스키닝 행렬로 옮긴다.
 * 반환: xyz 화면 노멀 → 덩어리가 옮긴 면 노멀의 회전(축 × sin, 프레임), w 덩어리 이동의 노멀 성분(cm).
 */
float4 SB_LumpNormal(int2 Texel, float3 RestP, float3 P, float3 N, float4 Row0, float4 Row1, float4 Row2, float3 Offset)
{
	float Height = dot(Offset, N);
	float ScaleMean = (abs(ComponentScale.x) + abs(ComponentScale.y) + abs(ComponentScale.z)) / 3.0;
	float Edge = CarrierRedirect.Load(int3(Texel, 0)).z * ScaleMean;
	int Radius = clamp((int)round(Edge / max(TexelSize, 1e-4)), 1, SB_LUMP_STENCIL_MAX);
	int2 Around[4] = {SB_NeighborAt(Texel, int2(1, 0), Radius), SB_NeighborAt(Texel, int2(-1, 0), Radius),
		SB_NeighborAt(Texel, int2(0, 1), Radius), SB_NeighborAt(Texel, int2(0, -1), Radius)};
	float4 Lumps[4];
	bool bAny = SB_LumpWeight(CarrierLump.Load(int3(Texel, 0))) > 0.0;
	UNROLL
	for (int k = 0; k < 4; ++k)
	{
		Lumps[k] = CarrierLump.Load(int3(Around[k], 0));
		bAny = bAny || SB_LumpWeight(Lumps[k]) > 0.0;
	}
	if (!bAny)
	{
		return float4(0, 0, 0, Height);
	}
	float3 Arm[4];
	float3 Moved[4];
	UNROLL
	for (int m = 0; m < 4; ++m)
	{
		Arm[m] = SB_SkinDelta(CarrierPosition.Load(int3(Around[m], 0)).xyz - RestP, Row0, Row1, Row2);
		Moved[m] = SB_LumpOffset(Around[m], Lumps[m], P + Arm[m], N);
	}
	float3 E1 = Arm[0] - Arm[1];
	float3 E2 = Arm[2] - Arm[3];
	float3 F1 = E1 + Moved[0] - Moved[1];
	float3 F2 = E2 + Moved[2] - Moved[3];
	float3 Screen = N;
	float3 Rest;
	if (SB_DecodeScreenNormal(Texel, Rest))
	{
		float4 V = float4(Rest, 0.0);
		float3 Skinned = float3(dot(V, Row0), dot(V, Row1), dot(V, Row2)) / SB_NormalScale();
		Screen = dot(Skinned, Skinned) > 1e-12 ? normalize(Skinned) : N;
	}
	float3 Before = cross(E1, E2);
	float3 After = cross(F1, F2);
	float BeforeLength = length(Before);
	float AfterLength = length(After);
	if (BeforeLength <= 1e-8 || AfterLength <= 1e-8)
	{
		return float4(0, 0, 0, Height);
	}
	// 차트가 아틀라스에서 뒤집혀 있을 수 있으므로 바깥 노멀 쪽으로 맞춘다. 변형 J: E1 → F1, E2 → F2, 전 노멀 → 후 노멀.
	float Orientation = dot(Before, N) < 0.0 ? -1.0 : 1.0;
	Before *= Orientation / BeforeLength;
	After *= Orientation / AfterLength;
	float3 Deformed = Orientation * (dot(E1, Screen) * cross(F2, After) + dot(E2, Screen) * cross(After, F1)
		+ dot(Before, Screen) * cross(F1, F2));
	float DeformedLength = length(Deformed);
	if (DeformedLength <= 1e-8)
	{
		return float4(0, 0, 0, Height);
	}
	Deformed /= DeformedLength;
	float3 Tilt = cross(Screen, Deformed);
	if (dot(Screen, Deformed) < 0.0)
	{
		Tilt *= rsqrt(max(dot(Tilt, Tilt), 1e-12));
	}
	return float4(Tilt, Height);
}

/**
 * 광선이 지나지 않는 형상의 가장자리 부호 거리(cm, 음수, ADR-0029): −(광선이 들어갔다면 누를 구간에서 형상에 가장 가까이 간 거리).
 * 가장자리 바로 바깥에서 0으로 줄어 안쪽 값(광선이 형상 안을 지난 깊이)과 이어진다(ADR-0010 §8.1). 구·캡슐은 식 하나라 피부 점에서
 * 형상까지 거리 Gap이 가장자리 띠(SB_EDGE_RANGE칸) 안일 때 잰다. 박스·컨벡스는 황금 분할 대신 Gap을 그대로 쓴다 — 평평한 옆면에서는
 * 둘이 같다. 띠 밖에서도 Gap을 쓴다: 광선 쪽 거리가 Gap보다 작아 띠 안일 수 있는 곳은 살 속에 묻힌 형상 위뿐이고, 거기는 누른
 * 깊이가 코어 너머로 서서히 빠져 세울 벽이 없다.
 */
float SB_ShapeEdgeMissed(uint Shape, float3 Origin, float3 N, float Own, float Gap)
{
	uint Type = (uint)round(Shapes[Shape * 3 + 0].w);
	if (Gap >= SB_EDGE_RANGE * TexelSize || (Type != SB_SHAPE_SPHERE && Type != SB_SHAPE_CAPSULE))
	{
		return -Gap;
	}
	return -max(SB_ShapeRayClosest(Shape, Origin, N, 0.0, Own * (1.0 + SB_CORE_FADE)), 0.0);
}

/**
 * 앞 표지 지키기(일지 314): 지난 갱신 자리마다, 그 콜라이더의 형상·거리장 부위(묶음 거르기를 지난 것) 가운데 하나라도 쉬는 피부 점을 들어온 방향으로
 * 덮으면(SB_ShapeCover·SB_FieldCover) L.Kept에 적는다. 한 콜라이더의 모든 부위가 이 결정을 같이 쓴다.
 */
void SB_LabelsCover(inout FSBLabels L)
{
	UNROLL
	for (uint k = 0u; k < 2u; ++k)
	{
		uint Code = k == 0u ? L.Old0 : L.Old1;
		if (Code == 0u)
		{
			continue;
		}
		float3 U = SB_LabelDirection(L, k == 0u ? L.OldDir0 : L.OldDir1);
		bool bCover = false;
		LOOP
		for (uint Shape = 0; Shape < NumShapes && !bCover; ++Shape)
		{
			if ((Shape < SB_MAX_GROUP_SHAPES && ((SB_GroupShapeMask >> Shape) & 1u) == 0u) || (ColliderIds[Shape] & 0x7fu) + 1u != Code)
			{
				continue;
			}
			bCover = SB_ShapeCover(Shape, L.Point, U, L.Reach, L.Margin);
		}
		uint NumWords = min((NumFieldInstances + 31u) >> 5u, (uint)SB_MAX_FIELD_WORDS);
		LOOP
		for (uint Word = 0; Word < NumWords && !bCover; ++Word)
		{
			uint Mask = SB_GroupFieldMask[Word];
			LOOP
			while (Mask != 0u && !bCover)
			{
				uint Instance = Word * 32u + firstbitlow(Mask);
				Mask &= Mask - 1u;
				if ((ColliderIds[NumShapes + Instance] & 0x7fu) + 1u != Code)
				{
					continue;
				}
				float Scale = FieldInstances[Instance * SB_FIELD_ROWS + 5].x;
				if (SB_FieldBoxGap(Instance, SB_FieldToLocal(Instance, L.Point)) * Scale > L.Reach)
				{
					continue;
				}
				bCover = SB_FieldCover(Instance, Scale, L.Point, U, L.Reach, L.Margin);
			}
		}
		L.Kept |= bCover ? 1u << k : 0u;
	}
}

/**
 * Origin에서 −노멀로 쏜 광선의 누른 깊이(형상과 모양 거리장, 소프트 상한 전). 관통하면 양수, 아니면 위로 빈 높이(바깥 노멀로
 * 곧게 올라가 콜라이더에 닿기까지)의 음수(솟는 살의 한도, SB_ShapeMissed·SB_FieldRoom). Depth를 키운 콜라이더를 Winner에 적는다.
 * Through: 살 두께에 붙이기 전 깊이의 최댓값(덩어리를 미는 힘, ADR-0017). 부르는 쪽이 SB_NO_OBSTACLE로 두고 넘긴다.
 *          누르지 않는 콜라이더는 덩어리가 닿기까지 남은 틈의 음수다. bLump(덩어리 힘을 받는 칸)일 때만 맞는 값이다.
 * Own: 제 살 깊이(살 두께 × SB_ColumnAt). 표면을 뚫지 않고 살 속에 묻힌 콜라이더는 이 깊이의 (1 + SB_CORE_FADE)배까지만 찾는다.
 * Edge: 가장자리 부호 거리(cm, 눌린 안쪽이 +, ADR-0029)의 최댓값. 부르는 쪽이 SB_NO_OBSTACLE로 두고 넘긴다.
 * Wall: 벽 몫(ADR-0035)을 재는 두 깊이(cm) — x = 나오는 깊이, y = 그 가운데 안을 지난 깊이 — 의 지나는 콜라이더 모두에 걸친 최댓값.
 *       벽 몫 = x − y. 가장자리 부호 거리처럼 합집합으로 모아, 이긴 콜라이더가 바뀌는 칸에서도 튀지 않는다. 부르는 쪽이 0으로 두고 넘긴다.
 * Push: 덩어리를 미는 겹침(w, 콜라이더 면 노멀 쪽으로 잰 cm — 지나지 않으면 Through와 같은 틈의 음수)과 방향(xyz, SB_ShapePush)의, 겹침이 가장 큰
 *       콜라이더 값(일지 317). bLump일 때만 잰다. 부르는 쪽이 (N, SB_NO_OBSTACLE)로 두고 넘긴다.
 */
float SB_PressedDepth(float3 Origin, float3 N, float3 RestP, float Layer, float Own, bool bLump, inout uint Winner, inout float Through,
	inout float Edge, inout float2 Wall, inout FSBLabels L, inout float4 Push)
{
	SB_LabelsCover(L);
	float Depth = SB_NO_OBSTACLE;
	bool bPush = bLump;
	for (uint Shape = 0; Shape < NumShapes; ++Shape)
	{
		if (Shape < SB_MAX_GROUP_SHAPES && ((SB_GroupShapeMask >> Shape) & 1u) == 0u)
		{
			continue;
		}
		float Before = Depth;
		float2 Hit = SB_ShapeRayInterval(Shape, Origin, -N);
		float Reach = Layer * (1.0 + SB_CORE_FADE);
		// 표면을 뚫지 않고 살 속에 묻힌 콜라이더(Hit.x > 0)는 제 살 깊이까지만 찾는다(SB_ColumnAt) — 다른 면으로 들어온 콜라이더에
		// 이 면이 같이 꺼지지 않는다. 이 면으로 들어온 콜라이더(앞 표지, SB_ShapeLabel)는 살 두께 + Max Contact Depth까지 찾는다. 가장자리 다듬기는
		// 전처럼 살 두께의 (1 + SB_CORE_FADE)배까지 재고, 그 너머에서 들어온 앞 표지 콜라이더만 구간 전체에서 잰다.
		float FrontShare;
		bool bFront = SB_ShapeLabel(Shape, L, FrontShare);
		bool bHit = Hit.x <= Hit.y && Hit.y > 0.0 && Hit.x < (bFront ? L.Reach : Own * (1.0 + SB_CORE_FADE));
		// 지나면 나오는 곳까지를 가장자리 다듬기(형상 안을 지난 깊이로, SB_TrimmedDepth)로 줄인다 — 살 두께에 붙이지 않는다(피부는 콜라이더 면까지
		// 따라가고, 응답만 붙인 깊이를 쓴다 — 일지 299). 아니면 비껴간 값(SB_ShapeMissed). 들어오는 곳이 제 살 깊이 너머면 둘 사이를 잇는다(SB_CoreFade).
		// 가장자리 부호 거리(ADR-0029): 지나면 광선이 형상 안을 지난 가장 깊은 곳(다듬기와 같은 값) × 코어 너머 몫.
		float2 Pressed;
		float4 Pushing = float4(N, SB_NO_OBSTACLE);
		if (bHit)
		{
			float Margin = -SB_ShapeRayClosest(Shape, Origin, N, max(Hit.x, 0.0), Hit.x < Reach ? min(Hit.y, Reach) : Hit.y);
			float Trimmed = SB_TrimmedDepth(Hit.y, Margin);
			// 피부 자리 깊이는 콜라이더 면을 따라갈 수 있는 만큼(SB_ShapeFollow — 스치는 면은 옆으로 파지 않는다). 덩어리를 미는 깊이(y)는 그대로.
			float Follow = SB_ShapeFollow(Shape, Origin, N, Hit.y, Layer, SB_BuriedShare(Hit.x, Own, bFront, FrontShare));
			float Drawn = min(Trimmed, Follow);
			Pressed = float2(Drawn, Trimmed);
			if (bPush)
			{
				Pushing = SB_ShapePush(Shape, Origin, N, Hit.y, Trimmed);
			}
			// 앞 표지: 코어 너머 잇기 대신 기둥이 들어온 방향과 이루는 각만큼(SB_LabelShare — 칸·표지마다 정해진 값).
			float Fade = bFront ? max(SB_CoreFade(Hit.x, Own), FrontShare) : SB_CoreFade(Hit.x, Own);
			if (Fade < 1.0)
			{
				float2 Missed = SB_ShapeMissed(Shape, Origin, N, Own, bLump, Hit, Layer);
				Pressed = lerp(Missed, Pressed, Fade);
				Pushing.w = lerp(Missed.y, Pushing.w, Fade);
			}
			Edge = max(Edge, max(Margin, 0.0) * Fade);
			Wall = max(Wall, float2(Hit.y, clamp(Margin, 0.0, Hit.y)) * Fade);
		}
		else
		{
			Pressed = SB_ShapeMissed(Shape, Origin, N, Own, bLump, Hit, Layer);
			Pushing.w = Pressed.y;
			Edge = max(Edge, SB_ShapeEdgeMissed(Shape, Origin, N, Own, SB_ShapeSignedDistance(Shape, Origin)));
		}
		Depth = max(Depth, Pressed.x);
		Through = max(Through, Pressed.y);
		Push = bPush && Pushing.w > Push.w ? Pushing : Push;
		Winner = Depth > Before ? Shape : Winner;
	}
	if (NumFieldInstances > 0)
	{
		Depth = SB_FieldDepth(Origin, N, RestP, Layer, Own, Depth, Winner, Through, Edge, Wall, L, bPush, Push);
	}
	return Depth;
}

// 끌림 거친 칸 모으기(일지 161): 스레드(가는 칸)마다 값을 올려 두고, 거친 칸마다 첫 스레드가 DragBlock × DragBlock을 더한다. 접촉
// 묶음(8×8칸)이 거친 칸(2×2 또는 4×4칸)을 통째로 담아 묶음 밖 칸을 볼 일이 없다. 표면 칸만 센다(고스트 칸은 옆 차트의 점이다).
groupshared float4 SB_DragSkin[64];    // xyz 피부 자리, w 살 두께(표면 칸이 아니면 0)
groupshared float4 SB_DragPress[64];   // xyz 바깥 노멀, w 누르는 힘 φ(d)(누르지 않았으면 0)
groupshared float4 SB_DragMotion[64];  // 누른 칸: xyz 콜라이더가 움직인 거리 − 피부 자리가 움직인 거리, w 둘레 함몰이 받치는 힘(아래)

/**
 * 누른 칸 둘레의 함몰이 이 칸을 끌어올리는 몫 ℓ_u²W·(u − 이웃 평균)(살의 힘 단위, 지난 갱신의 함몰). 옆 끌림은 둘레 살이 당기는
 * 몫까지 세는데 누르는 힘은 제 칸만 세면, 넓은 끌림 폭에서 가장자리가 늘 미끄러진다(ADR-0019). 이웃 거리는 칸 크기로 본다 —
 * 마찰 한도 하나를 고치는 몫이라 이완처럼 이웃 거리를 재지 않는다.
 */
float SB_DentSupport(int2 Texel, float Layer)
{
	float U = StatePrevious.Load(int3(Texel, 0));
	float Around = 0.0;
	UNROLL
	for (int k = 0; k < 4; ++k)
	{
		int2 Next = Texel + int2(k < 2 ? 1 - 2 * k : 0, k < 2 ? 0 : 5 - 2 * k);
		Around += all(Next >= int2(0, 0)) && all(Next < int2(Resolution, Resolution)) ? StatePrevious.Load(int3(Next, 0)) : 0.0;
	}
	float ReachU = max(SB_WindowLength(SB_VolumeAt(Texel), Layer, SB_SkinLengthAt(Texel) * ThicknessScale), 1e-3);
	return ReachU * ReachU * 4.0 / max(TexelSize * TexelSize, 1e-8) * (U - 0.25 * Around);
}

// 누른 것 표(일지 248 ②)를 묶음 안에서 먼저 모은다: 한 누른 것의 칸들이 같은 전역 주소에 원자 덧셈을 몰지 않게(일지 169·170).
groupshared uint SB_GroupPresserKey[SB_PRESSER_LOCAL_SLOTS];      // 누른 것 × SB_LUMP_GROUPS + 덩어리 묶음, 빈 자리 0xffffffff
groupshared uint SB_GroupPresserCount[SB_PRESSER_LOCAL_SLOTS];
groupshared uint SB_GroupPresserBreadth[SB_PRESSER_LOCAL_SLOTS];  // a_pr의 뒤집은 float 비트(양수라 뒤집은 uint 최댓값 = float 최솟값)
groupshared int SB_GroupPresserDir[3u * SB_PRESSER_LOCAL_SLOTS];  // 미는 방향 합(고정소수점, 표의 [2..4]와 같은 차례)

/**
 * 덩어리 칸 하나를 누른 것 표에 센다(묶음 공유 메모리, 자리가 차면 전역 버퍼에 바로). 가로 크기는 칸들의 최솟값(뒤집은 비트의 최댓값), 미는 방향은 합
 * (Dir = 미는 방향 × SB_GRIP_DIR_SCALE, 모르면 0 — 2의 보수 덧셈이라 음수도 그대로 더해진다).
 */
void SB_CountPresser(uint Key, float Breadth, int3 Dir)
{
	uint Flipped = ~asuint(Breadth);
	bool bStored = false;
	for (uint Slot = 0u; Slot < SB_PRESSER_LOCAL_SLOTS && !bStored; ++Slot)
	{
		uint Old;
		InterlockedCompareExchange(SB_GroupPresserKey[Slot], 0xffffffffu, Key, Old);
		if (Old == 0xffffffffu || Old == Key)
		{
			InterlockedAdd(SB_GroupPresserCount[Slot], 1u);
			InterlockedMax(SB_GroupPresserBreadth[Slot], Flipped);
			InterlockedAdd(SB_GroupPresserDir[3u * Slot + 0u], Dir.x);
			InterlockedAdd(SB_GroupPresserDir[3u * Slot + 1u], Dir.y);
			InterlockedAdd(SB_GroupPresserDir[3u * Slot + 2u], Dir.z);
			bStored = true;
		}
	}
	if (!bStored)
	{
		uint Index = SB_PRESSER_TABLE_BASE + Key * SB_PRESSER_WORDS;
		InterlockedAdd(ActivityOut[Index], 1u);
		InterlockedMax(ActivityOut[Index + 1u], Flipped);
		InterlockedAdd(ActivityOut[Index + 2u], asuint(Dir.x));
		InterlockedAdd(ActivityOut[Index + 3u], asuint(Dir.y));
		InterlockedAdd(ActivityOut[Index + 4u], asuint(Dir.z));
	}
}

// 잡기 고르기(ADR-0041)를 묶음 안에서 먼저 모은다: 줄마다 묶음에서 가장 가까운 칸(뒤집은 열쇠의 최댓값, 0 = 없음). 묶음마다 원자 하나만 전역으로 간다.
groupshared uint SB_GroupGrabPick[SB_GRAB_ROWS];

/**
 * 새 잡기의 잡은 점 고르기(접촉 패스 — 붙드는 줄인데 잡기 번호가 GrabState와 다를 때): 이 표면 칸(지금 피부 자리 Skin, 노멀 N, 살 두께 Flesh cm)이 잡는
 * 점에서 얼마나 먼지. 잡는 점이 겉 아래 살 두께 안이면(손가락이 이미 누른 채 꼬집기) 노멀 몫은 세지 않는다. Grab At Hit는 트레이스가 들어온 쪽을 보는
 * 칸만. 거리 10비트와 칸 번호 22비트를 한 열쇠로 묶어 뒤집은 값의 최댓값 = 가장 가까운 칸(같은 거리면 번호가 작은 칸 — 64비트 원자 없이). 캐리어 한
 * 변이 2048까지라(Contact Map Resolution 상한) 칸 번호가 22비트에 든다.
 */
void SB_GrabPick(uint2 Texel, float3 Skin, float3 N, float Flesh)
{
	for (uint Row = 0u; Row < GrabCount; ++Row)
	{
		float4 In1 = GrabInput[Row * SB_GRAB_INPUT_ROWS + 1u];
		float4 In2 = GrabInput[Row * SB_GRAB_INPUT_ROWS + 2u];
		uint Flags = asuint(In2.w);
		if ((Flags & SB_GRAB_HOLD) == 0u || asuint(In1.w) == asuint(GrabState[Row * SB_GRAB_STATE_ROWS].z))
		{
			continue;
		}
		if ((Flags & SB_GRAB_FACING) != 0u && dot(N, In2.xyz) >= 0.0)
		{
			continue;
		}
		float3 V = In1.xyz - Skin;
		float Inward = -dot(V, N);
		float Distance = Inward > 0.0 && Inward < Flesh ? length(V + N * Inward) : length(V);
		uint SearchTenths = (Flags >> 8u) & 0xffu;
		float Search = SearchTenths != 0u ? (float)SearchTenths * 0.1 : SB_GRAB_SEARCH;
		if (!(Distance < Search))
		{
			continue;
		}
		uint Quantized = min((uint)(Distance * (1022.0 / Search)), 1022u);
		uint Key = (Quantized << 22u) | ((Texel.y * Resolution + Texel.x) & 0x3fffffu);
		InterlockedMax(SB_GroupGrabPick[Row], ~Key);
	}
}

[numthreads(8, 8, 1)]
void ContactCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	// 목록의 몇 번째 묶음인지 → 캐리어 칸 좌표. 목록 길이를 넘는 스레드 묶음(마지막 줄의 나머지)은 칸이 없다.
	uint Entry = GroupId.y * SB_CONTACT_ROW + GroupId.x;
	bool bListed = Entry < ContactGroupCount[0];
	uint Packed = bListed ? ContactGroups[Entry] : 0u;
	uint3 Id = uint3(SB_GroupOrigin(Packed) + LocalId.xy, 0u);
	// 둘레 8칸 안에 덩어리 칸이 없는 묶음은 덩어리 노멀이 늘 0이라 재지 않는다(출력은 만들 때 0).
	bool bLumpNear = (Packed & SB_GROUP_LUMP_NEAR) != 0u;
	if (LocalIndex == 0u)
	{
		UNROLL
		for (int k = 0; k < 3; ++k)
		{
			SB_GroupLo[k] = 0xffffffffu;
			SB_GroupHi[k] = 0u;
		}
		SB_GroupLayer = 0u;
		SB_GroupShapeMask = 0u;
		SB_GroupPressed = 0u;
		SB_GroupDeepCount = 0u;
		SB_GroupDeepMost = 0u;
		SB_GroupLabelOverflow = 0u;
	}
	if (LocalIndex < SB_MAX_FIELD_WORDS)
	{
		SB_GroupFieldMask[LocalIndex] = 0u;
	}
	if (LocalIndex < SB_PRESSER_LOCAL_SLOTS)
	{
		SB_GroupPresserKey[LocalIndex] = 0xffffffffu;
		SB_GroupPresserCount[LocalIndex] = 0u;
		SB_GroupPresserBreadth[LocalIndex] = 0u;
	}
	if (LocalIndex < 3u * SB_PRESSER_LOCAL_SLOTS)
	{
		SB_GroupPresserDir[LocalIndex] = 0;
	}
	if (LocalIndex < SB_GRAB_ROWS)
	{
		SB_GroupGrabPick[LocalIndex] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	// 고스트 칸도 계산한다: 베이크가 가리키는 표면 칸의 위치·노멀·스킨을 복사해 두었으므로 결과가 그 칸과 같고,
	// 뒤 패스들이 차트 경계에서 이웃 칸을 그대로 읽을 수 있다.
	// 묶음 거르기 때문에 모든 스레드가 장벽을 지나야 하므로 일찍 돌아가지 않는다.
	bool bInside = bListed && Id.x < Resolution && Id.y < Resolution;
	uint Kind = 0u;
	bool bCarried = false;
	float3 RestP = float3(0, 0, 0);
	float3 P = float3(0, 0, 0);
	float3 N = float3(0, 0, 1);
	float3 RestN = float3(0, 0, 1);   // 정지 노멀(스키닝 전): 손 닻의 접선 축(SB_LumpBasis)이 덩어리 힘 패스와 같은 축이어야 한다
	float Layer = 1e-3;
	// 살 두께 그대로(cm). 0 = 살이 없는 칸: 캐리어 굽기가 살 두께에 눌리는 정도 칠을 곱해 두므로(ADR-0016 §5) 칠이 0인 곳은 0이다.
	// Layer는 같은 값을 나눗셈에 쓰려고 아래를 자른 것이다.
	float Flesh = 0.0;
	float3 Drag = float3(0, 0, 0);
	// 끌림 거친 칸으로 모을 값(SB_DragSkin·SB_DragPress·SB_DragMotion). 표면 칸이 아니면 0.
	float4 DragSkin = float4(0, 0, 0, 0);
	float4 DragPress = float4(0, 0, 0, 0);
	float3 DragMoved = float3(0, 0, 0);
	float DragSupport = 0.0;
	float3 Origin = float3(0, 0, 0);
#if SB_HARD_SURFACE
	// 하드 서피스: 떠 있는 조각 밑 칸의 접촉 시작점 올림(cm). 그 밖 칸은 0.
	float Lift = 0.0;
#endif
	float3 LumpOffset = float3(0, 0, 0);
	// 출렁임 모양 몫만(머티리얼이 그리는 덩어리 이동) — 덩어리 노멀이 쓴다. 국소 모양 몫은 변위로 그려 노멀 패스가 센다.
	float3 LumpShake = float3(0, 0, 0);
	float4 LumpData = float4(0, 0, 0, 0);
	bool bLumpForce = false;
	// 스키닝 행렬(본 무게로 섞은 세 행): 덩어리 노멀이 이웃 칸 위치를 이 행렬로 옮긴다(ADR-0018).
	float4 SkinRow0 = float4(1, 0, 0, 0);
	float4 SkinRow1 = float4(0, 1, 0, 0);
	float4 SkinRow2 = float4(0, 0, 1, 0);
	if (bInside)
	{
		float4 Position = CarrierPosition.Load(int3(Id.xy, 0));
		Kind = (uint)round(Position.w);
		bCarried = SB_IsCarried(Kind);
		if (bCarried)
		{
			RestP = Position.xyz;
			P = RestP;
			float4 NormalData = CarrierNormal.Load(int3(Id.xy, 0));
			N = NormalData.xyz;
			RestN = NormalData.xyz;
			if (bSkinned != 0)
			{
				SB_SkinRows(int2(Id.xy), Bones, SkinRow0, SkinRow1, SkinRow2);
				float4 P1 = float4(P, 1.0);
				float4 N0 = float4(N, 0.0);
				P = float3(dot(P1, SkinRow0), dot(P1, SkinRow1), dot(P1, SkinRow2));
				N = float3(dot(N0, SkinRow0), dot(N0, SkinRow1), dot(N0, SkinRow2));
			}
			P *= ComponentScale;
			N /= SB_NormalScale();
			N *= rsqrt(max(dot(N, N), 1e-8));

			// 누른 깊이: 이 점에서 몸 안쪽(−노멀)으로 살 두께만큼 광선을 쏴서, 콜라이더를 지나면 그 콜라이더의 먼 쪽 면까지의
			// 거리. 콜라이더가 원래 표면보다 완전히 깊이 들어가 이 점이 더는 콜라이더 안이 아니어도, 표면은 콜라이더를 감싸며
			// 그 아래까지 밀려 있어야 하기 때문이다(점이 콜라이더 안이면 광선이 나가는 곳까지 — 밀린 점은 늘 콜라이더 밖이다).
			// 살 두께(코어까지)보다 깊이 들어오는 콜라이더는 그 너머 절반 두께에 걸쳐 서서히 이 점을 덜 밀고(SB_CoreFade), 누른 깊이는
			// 살 두께에 붙이지 않는다 — 피부는 콜라이더 면까지 따라가고, 압력·끌림 힘 같은 응답만 살 두께에 붙인 깊이를 쓴다(일지 299).
			// 콜라이더 가장자리에서 다듬기 폭 안쪽까지는 깊이를 줄인다(SB_TrimmedDepth). 관통하면 양수 깊이(Max Contact Depth 상한), 아니면 이 피부 점에서 콜라이더까지 거리의
			// 음수 — 솟는 살(ADR-0011)은 이만큼만 솟는다(솟은 점이 콜라이더에 닿기 전까지). 피부를 가로지른 콜라이더의 가장자리
			// 바로 바깥에서는 0으로 줄어 안쪽 값과 이어진다. 이완은 max(d, λ·avg ≥ 0)이라 음수를 무시한다.
			Flesh = max(NormalData.w, 0.0) * (abs(ComponentScale.x) + abs(ComponentScale.y) + abs(ComponentScale.z)) / 3.0;
			Layer = max(Flesh, 1e-3);
			// 끌린 피부는 칸 자리가 아니라 끌린 자리에서 콜라이더에 닿는다. 칸 자리에서 재면 끌린 만큼 자국이 콜라이더보다 앞서
			// 그려진다(앞쪽은 콜라이더를 파고들고 뒤쪽은 뜬다). 끌림은 지난 갱신의 거친 격자 값을 이 칸 자리(고스트 칸은 가리키는 점)에서
			// 읽어 접평면에 눕힌다(일지 161).
			float2 Where = (float2)Id.xy;
			if (Kind == SB_KIND_GHOST)
			{
				Where = CarrierRedirect.Load(int3(Id.xy, 0)).xy;
			}
			Drag = SB_DragAt(Where);
			Drag -= N * dot(Drag, N);
			Origin = P + Drag;
#if SB_HARD_SURFACE
			// 하드 서피스(ADR-0037 §3.7): 떠 있는 조각(보석) 밑 칸은 손을 조각 윗면에서 잰다 — 칸 자리는 밑 겉면 그대로 두고 광선 출발점만 굽기가 칸마다
			// 적은 올림만큼 노멀 쪽으로 올린다(칸 자리를 옮기면 함몰 창 이웃 간격이 튄다). 찾는 깊이도 그만큼 는다(아래 SB_PressedDepth).
			Lift = SB_HardLiftAt(int2(Id.xy)) * (abs(ComponentScale.x) + abs(ComponentScale.y) + abs(ComponentScale.z)) / 3.0;
			Origin += N * Lift;
#endif
			// 덩어리(ADR-0012): 화면의 피부 = 제자리 + 관성 흔들림 − 파임이다. 덩어리 칸은 지금 덩어리가 옮겨 간 만큼(옆 방향
			// 포함, §3.5) 광선 출발점을 옮겨 재야 파인 피부가 콜라이더 면에 맞고(틈 없음) 자국이 콜라이더 자리에 남는다. 덩어리를 미는
			// 힘은 같은 광선의 깊이에서 덩어리 노멀 이동을 빼 "덩어리가 안 움직였다면의 깊이"로 바꿔 적는다(ADR-0017) — 이동이 힘을
			// 다시 바꾸지 않아 몇 프레임 늦게 읽어도 떨리지 않고, 손이 덩어리를 따라 깊이 들어가도 광선은 손 가까이에서 잰다.
			if (LumpMode != 0)
			{
				LumpData = CarrierLump.Load(int3(Id.xy, 0));
				LumpShake = SB_LumpOffset(int2(Id.xy), LumpData, P, N);
				LumpOffset = LumpShake;
				bLumpForce = LumpMode == 2 && SB_LumpWeight(LumpData) > 0.0;
				// 국소 모양(일지 251)이 옮긴 양도 광선 출발점에 든다 — 덩어리 평형이 d = d0 + A·(g·E + r·q)로 다시 더하므로 d0에서도 뺀다(아래 RestDepth).
				if (LumpMode == 2)
				{
					LumpOffset += SB_RegionOffsetAt(Where, RestP, LumpData);
				}
			}
			SB_GroupInclude(Origin);
			SB_GroupInclude(Origin + LumpOffset);
#if SB_HARD_SURFACE
			InterlockedMax(SB_GroupLayer, asuint(Layer + Lift));
#else
			InterlockedMax(SB_GroupLayer, asuint(Layer));
#endif
		}
	}
	GroupMemoryBarrierWithGroupSync();
	SB_GroupCull(LocalIndex);
	GroupMemoryBarrierWithGroupSync();

	if (bCarried)
	{
		Origin += LumpOffset;
		// 지금 피부 자리(파임 전): 스킨 + 덩어리 이동(출렁임 모양 몫). 끌림의 기준이고 PositionOut에 남는다. 국소 모양 몫(일지 251)은 넣지 않는다 —
		// 합치기가 변위로 그리므로, 노멀 패스가 "PositionOut → + 변위"를 접촉이 더한 변형으로 재려면 PositionOut은 출렁임 모양까지만이어야 한다.
		float3 Skin = P + LumpShake;
		// 잡기(ADR-0041): 원 안 칸은 아래에서 콜라이더 누름을 지운다(잡기가 이긴다 — 꼬집는 손가락이 바로 거기 있다). 원 둘레 칸이 든 묶음은 함몰 목록에
		// 올린다(눌린 묶음과 같이). 새 잡기면 잡은 점 후보로 센다 — 살이 없는 칸(Depth 검정)과 떠 있는 장식 밑 칸은 잡을 수 없다.
		float GrabWeight = 0.0;
		bool bGrabNear = false;
		BRANCH
		if (GrabCount != 0u)
		{
			float3 GrabPull;
			GrabWeight = SB_GrabGrip(RestP, RestN * rsqrt(max(dot(RestN, RestN), 1e-12)), GrabPull);
			bGrabNear = GrabWeight > 0.0 || SB_GrabNear(RestP);
#if SB_HARD_SURFACE
			if (Kind == SB_KIND_SURFACE && Flesh > 0.0 && Lift <= 0.0)
#else
			if (Kind == SB_KIND_SURFACE && Flesh > 0.0)
#endif
			{
				SB_GrabPick(Id.xy, Skin, N, Flesh);
			}
		}
		// 들어온 면 표지: 지난 갱신 표지·쉬는 피부 점(광선 출발점 − 끌림). 앞 표지가 따라가는 깊이는 살 두께 + Max Contact Depth(묶음 거르기와 같은 끝).
		// 들어온 방향은 칸에 붙은 접선 두 축으로 적는다: 정지 노멀에서 만든 축(SB_LumpBasis)을 스키닝으로 옮겨 지금 노멀에 맞춘다(칸이 돌면 같이 돈다).
		float3 LabelT1 = float3(1, 0, 0);
		float3 LabelT2 = float3(0, 1, 0);
		if (bLabels != 0u)
		{
			float3 RestT1;
			float3 RestT2;
			SB_LumpBasis(normalize(RestN), RestT1, RestT2);
			float3 T = SB_SkinDelta(RestT1, SkinRow0, SkinRow1, SkinRow2);
			T -= N * dot(T, N);
			if (dot(T, T) < 1e-8)
			{
				SB_LumpBasis(N, T, RestT2);
			}
			LabelT1 = normalize(T);
			LabelT2 = cross(N, LabelT1);
		}
#if SB_HARD_SURFACE
		FSBLabels Labels = SB_LabelsBegin(int2(Id.xy), Origin - Drag, Skin, Layer + Lift, N, LabelT1, LabelT2);
#else
		FSBLabels Labels = SB_LabelsBegin(int2(Id.xy), Origin - Drag, Skin, Layer, N, LabelT1, LabelT2);
#endif
		uint Winner = SB_NO_WINNER;
		float Through = SB_NO_OBSTACLE;
		float Edge = SB_NO_OBSTACLE;
		float2 Wall = float2(0.0, 0.0);
		float4 Push = float4(N, SB_NO_OBSTACLE);
		// 제 살 깊이(ADR-0003 §16): 묻힌 콜라이더를 찾는 깊이. 굽지 않은 캐리어는 살 두께 그대로다.
		float Own = Layer * SB_ColumnAt(int2(Id.xy));
#if SB_HARD_SURFACE
		// 조각 윗면에서 잰 깊이 = 조각이 가라앉는 양 = 밑 살이 눌리는 양. 찾는 깊이·코어 너머 줄임은 올림만큼 더 아래에서 시작한다.
		float Depth = SB_PressedDepth(Origin, N, RestP, Layer + Lift, Own + Lift, bLumpForce, Winner, Through, Edge, Wall, Labels, Push);
#else
		float Depth = SB_PressedDepth(Origin, N, RestP, Layer, Own, bLumpForce, Winner, Through, Edge, Wall, Labels, Push);
#endif
		// 살이 없는 칸은 눌리지 않는다(파이지도, 손에 붙어 끌리지도 않는다). 덩어리를 미는 깊이(아래 RestDepth)는 Through로 따로
		// 재므로 그대로다: 살이 없는 덩어리는 파이지 않고 통째로 비켜나기만 한다. 가장자리 부호 거리도 바깥이다(누른 이웃과의 사이에
		// 벽이 선다).
		if (Flesh <= 0.0)
		{
			Depth = min(Depth, 0.0);
			Edge = -SB_EDGE_RANGE * TexelSize;
		}
		// 잡기의 원 안 칸(ADR-0041): 콜라이더가 눌러도 잡기가 이긴다 — 누름·덩어리 밀기·끌림·가장자리 벽 모두 없는 칸으로(위로 빈 높이는 그대로).
		if (GrabWeight > 0.0)
		{
			Depth = min(Depth, 0.0);
			Edge = -SB_EDGE_RANGE * TexelSize;
			Push.w = SB_NO_OBSTACLE;
		}
		float Raw = Depth;
		// 누르지 않은 칸의 음수는 둘레로 솟는 살의 한도다(SB_RaisedDepth). 위로 빈 높이(콜라이더까지)와 이 칸의 살 두께 가운데 작은 쪽을
		// 적는다: 살은 제 두께보다 더 솟지 못한다. 얇게 칠한 살은 그만큼 낮게 솟고,
		// 살이 없는 칸은 한도가 0이라 솟지 않는다 — 칠이 0으로 잦아드는 경계에서 솟음도 같이 잦아든다.
		Depth = Depth > 0.0 ? SB_PressLimit(Depth, MaxContactDepth) : max(Depth, -Flesh);
		// 덩어리를 미는 깊이(ADR-0017): 살 두께에 붙이기 전 깊이 − 덩어리가 이 칸을 미는 방향으로 옮긴 양 = 덩어리가 안 움직였다면의
		// 깊이 d0. 덩어리 평형(LumpForceCS)은 d = d0 + 덩어리 이동의 미는 방향 몫으로 다시 옮겨 잰다. 콜라이더가 살 두께보다 먼 칸은 빠진다.
		// 미는 방향은 누르는 콜라이더 면의 노멀이고 겹침도 그 쪽으로 잰다(일지 317, SB_ShapePush). 평평한 면이면 덩어리가 면을 따라 옆으로 미끄러져도
		// d0가 그대로다. 전에는 피부 노멀 광선의 길이에서 노멀 몫만 뺐다 — 비스듬한 피부에서 국소 모양의 옆 몫이 광선 출발점을 벽 속으로 옮겨 다음 갱신의
		// d0를 키우고, 그 힘이 옆 몫을 다시 키웠다(벽에 한 갱신 4 cm로 대면 상한까지 달아나 떼도 남았다). Max Contact Depth 상한도 빼기 전이 아니라
		// 뺀 뒤(덩어리가 안 움직였다면의 깊이)에 건다 — 상한 너머 깊은 칸에서도 덩어리 이동이 d0를 바꾸지 않는다. 평형의 힘 법칙은 그대로라 볼록하다
		// (ADR-0017 §2.1). 미는 방향은 덩어리 기본 자세 축으로 LumpDepthOut에 담고(SB_PackPushDir), 여기서도 담긴 값을 풀어 뺀다(평형과 같은 방향).
		float RestDepth = SB_LUMP_FAR;
		float PushCode = 0.0;
		if (bLumpForce && Push.w > -Layer)
		{
			PushCode = SB_PackPushDir(SB_LumpRestVector(LumpData, Push.xyz));
			float Rest = Push.w - dot(SB_UnpackPushDir(PushCode), SB_LumpRestVector(LumpData, LumpOffset));
			RestDepth = Rest > 0.0 ? SB_PressLimit(Rest, MaxContactDepth) : Rest;
		}
		// 끌림(ADR-0008): 누른 칸의 피부는 콜라이더를 따라간다. 칸 피부 자리의 움직임은 덮어쓰기 전 PositionOut(지난 프레임
		// 자리)과의 차다 — 캐릭터가 걸어가며 손과 몸이 같이 움직이면 둘이 상쇄되고, 덩어리가 손을 따라 비켜난 만큼도 상쇄된다
		// (ADR-0012 §7: 덩어리가 흡수한 이동은 무늬 끌림이 아니다).
		// 끌리는 거리 한도(ADR-0008 §12): 손 밑 피부가 옆으로 밀린 거리 G는 피부 밑 살층이 누르는 힘과 같은 법칙 φ(G)로
		// 되민다(SB_TissueLaw, 같은 두께·가파름). 그 힘이 비율 × 노멀 힘 φ(깊이)를 넘으면 미끄러지므로(쿨롱) 한도 거리는
		// φ⁻¹(비율 × φ(깊이))다 — 비율 1이면 누른 깊이와 같다. 꽉 쥘수록 마찰 힘은 가파르게 커져도 끌리는 거리는 살층이 잠기며
		// 천천히 는다. 끌림 거친 칸(일지 161)은 같은 법칙으로 칸들의 누르는 힘을 평균해 한도를 정한다.
		float PressPhi;
		float PressSlope;
		float PressEnergy;
		// 누르는 힘은 응답이라 살 두께에 붙인 깊이로 잰다(일지 299 — 피부 자리 깊이는 붙이지 않는다).
		SB_TissueLaw(SB_PressLimit(max(Depth, 0.0), Layer), Layer, LumpStiffen, PressPhi, PressSlope, PressEnergy);
		// 손 닻(일지 201): 손 위 닿은 점이 이번 갱신에 움직인 거리(프레임)와 "지난 갱신에도 눌려 닻이 있다".
		float3 HandMoved = float3(0, 0, 0);
		bool bAnchored = false;
		if (DragRatio > 0.0 && Raw > 0.0)
		{
			// 콜라이더 위 닿은 점은 광선이 콜라이더를 나간 곳이다. 칸 피부 자리의 움직임은 덮어쓰기 전 PositionOut(지난 갱신 자리)과의
			// 차다. 거친 칸은 이 움직임을 누른 칸끼리 평균해 쌓되 지난 갱신에도 눌린 거친 칸만 쌓는다(새로 닿은 곳은 닿기 전의 움직임 —
			// 누르며 들어온 움직임, 순간이동 — 에 끌리지 않고 지금 자리에서 붙기 시작한다).
			// 손 닻(덩어리 쥐기)은 지난 갱신에도 눌린 칸만 손의 움직임을 쌓는다. 지난 갱신에 눌렸는지는
			// 덮어쓰기 전 PositionOut.w로 본다(SB_TouchPressed). Contact는 갱신 안에서만 사는 임시
			// 텍스처라 지난 값이 없다(ADR-0022).
			bool bWasPressed = false;
			if (bMotionHistory != 0 && Winner != SB_NO_WINNER)
			{
				float4 Last = PositionOut[Id.xy];
				float3 Touch = Origin - N * Raw;
				HandMoved = Touch - SB_PreviousOnCollider(Winner, Touch);
				DragMoved = HandMoved - (Skin - Last.xyz);
				bWasPressed = SB_TouchPressed(Last.w);
			}
			bAnchored = bWasPressed;
			if (Depth > 0.0 && Kind == SB_KIND_SURFACE)
			{
				DragSupport = SB_DentSupport(int2(Id.xy), Layer);
			}
		}
		if (Kind == SB_KIND_SURFACE)
		{
			DragSkin = float4(Skin, Layer);
			DragPress = float4(N, Depth > 0.0 ? PressPhi : 0.0);
		}
		// 쉬기(ADR-0013): 눌린 칸(덩어리를 미는 칸 포함)이 있으면 이번 프레임 뒤 패스들이 돈다. 잡기 둘레 칸도 눌린 칸처럼(그 묶음이 함몰 목록에 든다).
		if (Raw > 0.0 || RestDepth > 0.0 || bGrabNear)
		{
			InterlockedOr(SB_GroupPressed, 1u);
		}
		// 살 두께보다 깊은 누름(원칙 P06): 피부는 콜라이더 면까지 따라가지만 응답(압력·끌림 힘)은 살 두께에 붙는다. 조용히 넘기지 않고 칸 수와
		// 가장 깊이 넘은 양을 Softbody.Stats에 낸다. Press Depth 0 칸(살 없음)도 센다 — 그 자리는 어떤 누름도 메시를 뚫는다.
#if SB_HARD_SURFACE
		float Beyond = Through - (Flesh + Lift);
#else
		float Beyond = Through - Flesh;
#endif
		if (Kind == SB_KIND_SURFACE && Beyond > 0.0)
		{
			InterlockedAdd(SB_GroupDeepCount, 1u);
			InterlockedMax(SB_GroupDeepMost, asuint(Beyond));
		}
		ContactOut[Id.xy] = float4(N, Depth);
		float EdgeCells = clamp(Edge / max(TexelSize, 1e-6), -SB_EDGE_RANGE, SB_EDGE_RANGE);
		// 벽 없음 양(ADR-0035): 눌린 가장자리 띠 칸의 벽 몫(나오는 깊이 − 안을 지난 깊이)이 낮으면 떠오르는 가장자리라 세울 벽이 없다 —
		// 쪼갠 점이 첫 칸 값을 가장자리까지 끌고 가 칸마다 높이가 다른 벽을 세우지 않게 한다. 벽 몫 1칸 아래는 다 풀고(보간이 이미 담는
		// 높이), 2칸(가장자리 띠 폭)까지 서서히 다시 세운다. 좁은 틈·섬의 몫은 합치기가 더한다(SB_ComposeEdge).
		float NoWall = Raw > 0.0 && EdgeCells < SB_EDGE_RANGE ? saturate(2.0 - (Wall.x - Wall.y) / max(TexelSize, 1e-6)) : 0.0;
		EdgeOut[Id.xy] = SB_PackEdge(EdgeCells, NoWall);
		// 누른 것의 번호(안 눌리면 0, 거리장 부위면 1 + 인스턴스 번호 — 외부 거리장 뒤에 자기 부위, 형상이면 1001 + 형상 번호)를 w에 묶는다(규약은 SB_PackTouch).
		float Presser = Raw > 0.0 && Winner != SB_NO_WINNER ? (Winner < NumShapes ? 1001.0 + (float)Winner : 1.0 + (float)(Winner - NumShapes)) : 0.0;
		PositionOut[Id.xy] = float4(Skin, SB_PackTouch(Presser));
		if (bLabels != 0u)
		{
			uint Overflow;
			ContactLabels[Id.xy] = SB_LabelsPack(Labels, Overflow);
			if (Overflow != 0u && Kind == SB_KIND_SURFACE)
			{
				InterlockedAdd(SB_GroupLabelOverflow, 1u);
			}
		}
		// 누른 것 표(일지 248 ②): 덩어리 힘이 이 칸을 그 누른 것의 접촉 하나로 묶어 재도록 덩어리 칸(표면 칸만, 덩어리 힘 패스와 같은 칸)을 센다.
		// 미는 방향(일지 317, 기본 자세 축)도 칸마다 무게 1로 더한다 — 덩어리 힘 패스가 같은 갱신에 손 부위마다의 마찰 면을 잰다(SB_GripNormal, 일지 338).
		// 겹침으로 무게를 매기던 덩어리 묶음 평균(일지 334)은 그 무게가 늦게 보이는 덩어리 자리의 깊이에 기댔다.
		uint LumpGroup = (uint)max(floor(LumpData.a), 0.0);
		if (bLumpForce && Presser > 0.0 && Kind == SB_KIND_SURFACE && LumpGroup < SB_LUMP_GROUPS)
		{
			int3 PushSum = PushCode != 0.0 ? int3(round(SB_UnpackPushDir(PushCode) * SB_GRIP_DIR_SCALE)) : int3(0, 0, 0);
			SB_CountPresser(Winner * SB_LUMP_GROUPS + LumpGroup, SB_PresserBreadth(Winner, N), PushSum);
		}
		if (LumpMode == 2)
		{
			// 붙은 손이 끈 거리(ADR-0012 §7) = "덩어리가 안 움직였다면의 손 자리"(손 닻). §3.3의 d0처럼 덩어리 이동이 힘을 되먹이지 않는다.
			// 붙은 칸은 손이 가만히 있어도 붙은 피부가 덩어리의 옆 미끄러짐을 붙든다(접선 스프링).
			// 마찰을 평형 안에서(일지 201): 여기서는 붙음을 판정하지 않고 마찰 면도 모른다(일지 338 — 면은 이 패스가 다 끝나야 나오는 손 부위마다 미는 방향
			// 평균이다). 덩어리 힘 패스가 면에 비추고 자르도록 두 몫을 넘긴다:
			//  - 지난 갱신에도 눌린 칸: 기억한 닻(LumpAnchor, 덩어리 힘 패스 첫 걸음이 마찰 한도 안으로 잘라 둔 값 — 피부 접평면 두 성분)과 손 위 닿은 점이
			//    이번 갱신에 이 칸 피부에 대해 움직인 거리(기본 자세 축 세 성분) — 피부가 본을 따라 움직인 몫은 빼고(일지 336: 닻과 견주는 덩어리 이동이 본
			//    기준이다), 덩어리가 움직인 거리는 빼지 않는다(덩어리 이동은 평형 안의 E로만 든다).
			//  - 새로 눌린 칸: 보이는 피부 자리(끌림 + 덩어리 옆 이동, 세 성분)에서 붙기 시작한다(일지 201의 Stick + 덩어리 옆 이동에서 자르기만 뺀 것).
			// 덩어리 평형이 돌지 않은 갱신(읽기 칸이 다 찬 프레임)에는 그 갱신의 손 움직임이 닻에 남지 않는다(한 프레임 몫, 고리는 아니다).
			float2 Anchor = float2(0, 0);
			float3 Hand = float3(0, 0, 0);
			// 잡기(ADR-0041 §3.4): 원 안 덩어리 칸 — 닻 자리(누르지 않은 칸이라 비어 있다)에 손끝 몫 κ/(1 + κ)와 표지 1, 손 몫 자리에 덩어리 용수철의 목표(잡을 때
			// 덩어리 이동 + 한도 안 당김, 덩어리 기본 자세 축 — 일지 350). 덩어리 힘 패스가 Lump Grip 세기의 세 방향 곧은 용수철로 덩어리를 손 쪽으로 끈다(마찰
			// 한도 없음 — Basic에서도).
			BRANCH
			if (bLumpForce && GrabWeight > 0.0)
			{
				float3 GrabLumpPull;
				float GrabLumpGrip = SB_GrabLumpTarget(RestP, RestN * rsqrt(max(dot(RestN, RestN), 1e-12)), GrabLumpPull);
				if (GrabLumpGrip > 0.0)
				{
					Anchor = float2(GrabLumpGrip / (1.0 + GrabLumpGrip), 1.0);
					Hand = SB_LumpRestVector(LumpData, GrabLumpPull);
				}
			}
			if (bLumpForce && DragRatio > 0.0 && Raw > 0.0)
			{
				if (bAnchored)
				{
					Anchor = LumpAnchor.Load(int3(Id.xy, 0)).xy;
					Hand = SB_LumpRestVector(LumpData, HandMoved - SB_SkinMotion(int2(Id.xy), RestP, P));
				}
				else
				{
					Hand = SB_LumpRestVector(LumpData, Drag + LumpOffset);
				}
			}
			LumpDepthOut[Id.xy] = SB_PackLumpDepth(RestDepth, Anchor, Hand, PushCode);
		}
		if (LumpMode != 0 && bLumpNear)
		{
			LumpNormalOut[Id.xy] = SB_LumpNormal(int2(Id.xy), RestP, P, N, SkinRow0, SkinRow1, SkinRow2, LumpShake);
		}
	}
	SB_DragSkin[LocalIndex] = DragSkin;
	SB_DragPress[LocalIndex] = DragPress;
	SB_DragMotion[LocalIndex] = float4(DragPress.w > 0.0 ? DragMoved : float3(0, 0, 0), DragSupport);
	// 빈칸은 쓰지 않는다. 갱신을 넘어 남는 Position·LumpNormal은 만들 때와 묶음 목록을 다시 만들 때 0이고 빈칸에는 아무 패스도 다른 값을
	// 쓰지 않는다. Contact·LumpDepth는 갱신마다 새로 잡는 임시 텍스처라(ADR-0022) 빈칸에 아무 값이나 남아 있다 — 읽는 패스는 모두 칸
	// 종류를 먼저 보고 빈칸을 읽지 않는다.
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u && SB_GroupDeepCount != 0u)
	{
		InterlockedAdd(ActivityOut[SB_ACTIVITY_DEEP], SB_GroupDeepCount);
		InterlockedMax(ActivityOut[SB_ACTIVITY_DEEP + 1u], SB_GroupDeepMost);
	}
	if (LocalIndex == 0u && SB_GroupLabelOverflow != 0u)
	{
		InterlockedAdd(ActivityOut[SB_ACTIVITY_LABEL_OVERFLOW], SB_GroupLabelOverflow);
	}
	// 잡기 고르기(ADR-0041): 묶음에서 가장 가까운 칸을 전역 낱말로(줄마다 원자 하나).
	if (LocalIndex < GrabCount && SB_GroupGrabPick[LocalIndex] != 0u)
	{
		InterlockedMax(ActivityOut[GrabPickBase + LocalIndex], SB_GroupGrabPick[LocalIndex]);
	}
	if (LocalIndex == 0u && SB_GroupPressed != 0u)
	{
		InterlockedOr(ActivityOut[0], 1u);
		uint2 Tile = Id.xy / SB_TILE;
		InterlockedOr(ActivityOut[TilePressedBase + Tile.y * TilesPerSide + Tile.x], 1u);
	}
	if (LocalIndex < SB_PRESSER_LOCAL_SLOTS && SB_GroupPresserKey[LocalIndex] != 0xffffffffu)
	{
		uint Index = SB_PRESSER_TABLE_BASE + SB_GroupPresserKey[LocalIndex] * SB_PRESSER_WORDS;
		InterlockedAdd(ActivityOut[Index], SB_GroupPresserCount[LocalIndex]);
		InterlockedMax(ActivityOut[Index + 1u], SB_GroupPresserBreadth[LocalIndex]);
		// 미는 방향 합: 2의 보수 덧셈이라 음수도 그대로 더해진다.
		InterlockedAdd(ActivityOut[Index + 2u], asuint(SB_GroupPresserDir[3u * LocalIndex + 0u]));
		InterlockedAdd(ActivityOut[Index + 3u], asuint(SB_GroupPresserDir[3u * LocalIndex + 1u]));
		InterlockedAdd(ActivityOut[Index + 4u], asuint(SB_GroupPresserDir[3u * LocalIndex + 2u]));
	}

	// 끌림 거친 칸(일지 161, ADR-0033): 거친 칸마다 첫 스레드가 칸 DragBlock × DragBlock을 모아 끌림 원천 패스가 읽을 입력 두 줄과 함몰 거친 해가 읽을 피부 자리 평균(셋째 줄,
	// 일지 199)을 쓴다. 셋째 줄 w는 살 칸의 겉껍질 장력 길이 평균(cm) — 끌림 폭 ℓ_t가 읽는다(ADR-0037 §3.4).
	//  - 움직임 = 누른 칸들이 콜라이더를 따라 움직인 거리의 평균. 원천 패스가 지난 갱신에도 눌린 칸의 붙든 자리에 쌓고, 미끄러져 들어온 앞 가장자리
	//    칸에는 절반을 준다(새로 닿은 곳은 닿기 전의 움직임 — 누르며 들어온 움직임, 순간이동 — 에 끌리지 않는다).
	//  - 버틸 수 있는 힘 = 마찰 × (누르는 힘 평균 + 둘레 함몰이 받치는 힘)(원천 패스의 쿨롱 검사가 읽는다). 누르는 힘은 칸 넓이마다의 힘이라 표면 칸
	//    수로 평균한다(누르지 않은 칸은 0) — 거친 칸 일부만 누르면 그만큼 약하게 붙든다.
	// 끌림 마찰이 0이면 누른 칸이 없는 것과 같다(지난 끌림은 기억처럼 잦아든다).
	if (bListed && all((LocalId.xy % DragBlock) == 0u))
	{
		uint2 Cell = Id.xy / DragBlock;
		float4 SumSkin = float4(0, 0, 0, 0);
		float4 SumPress = float4(0, 0, 0, 0);
		float4 SumMotion = float4(0, 0, 0, 0);
		float Surface = 0.0;
		float Pressed = 0.0;
		float SumTension = 0.0;
		for (uint Y = 0u; Y < DragBlock; ++Y)
		{
			for (uint X = 0u; X < DragBlock; ++X)
			{
				uint Slot = (LocalId.y + Y) * 8u + LocalId.x + X;
				float4 SkinHere = SB_DragSkin[Slot];
				if (SkinHere.w <= 0.0)
				{
					continue;
				}
				float4 Press = SB_DragPress[Slot];
				Surface += 1.0;
				SumSkin += SkinHere;
				SumTension += SB_SkinLengthAt(int2(Id.xy) + int2(X, Y));
				SumPress += Press;
				if (Press.w > 0.0)
				{
					Pressed += 1.0;
					SumMotion += SB_DragMotion[Slot];
				}
			}
		}
		uint Row = (Cell.y * DragResolution + Cell.x) * SB_DRAG_INPUT_ROWS;
		if (Cell.x < DragResolution && Cell.y < DragResolution && Surface > 0.0)
		{
			float LayerMean = SumSkin.w / Surface;
			float3 Outward = SumPress.xyz * rsqrt(max(dot(SumPress.xyz, SumPress.xyz), 1e-12));
			float PressMean = SumPress.w / Surface;
			float4 Motion = float4(0, 0, 0, -1);
			if (Pressed > 0.0 && DragRatio > 0.0)
			{
				Motion = float4(SumMotion.xyz / Pressed, max(DragRatio * (PressMean + SumMotion.w / Surface), 0.0));
			}
			DragInputOut[Row + 0u] = Motion;
			DragInputOut[Row + 1u] = float4(Outward, LayerMean);
			DragInputOut[Row + 2u] = float4(SumSkin.xyz / Surface, SumTension / Surface * ThicknessScale);
		}
	}
}

RWBuffer<uint> ContactGroupsOut;
RWBuffer<uint> ContactGroupCountOut; // [0] 목록 길이 (0으로 비운 뒤 센다)
RWBuffer<uint> ContactArgsOut;       // 접촉 패스 간접 디스패치 인자 (0으로 비운 뒤 채운다)
RWBuffer<uint> LumpListOut;          // 덩어리 힘 묶음 목록 (x | y << 16): 덩어리 무게가 있는 표면 칸이 든 8×8 묶음
RWBuffer<uint> LumpListCountOut;     // [0] 목록 길이 (0으로 비운 뒤 센다)
groupshared uint SB_GroupOccupied;
groupshared uint SB_GroupLumpNear;
groupshared uint SB_GroupLumpForce;

/**
 * 처음 한 번(캐리어가 바뀌면 다시): 표면·고스트 칸이 하나라도 든 8×8 묶음을 접촉 패스 목록에 넣는다(ADR-0013 3단계). 둘레 8칸
 * (덩어리 노멀 이웃 거리 최대 SB_LUMP_STENCIL_MAX)까지 덩어리 칸이 있으면 SB_GROUP_LUMP_NEAR를 붙이고, 덩어리 무게가 있는 표면
 * 칸이 든 묶음은 덩어리 힘 목록에도 넣는다(ADR-0013 §8). 둘 다 구운 값으로만 정해진다.
 */
[numthreads(8, 8, 1)]
void ContactGroupsCS(uint3 GroupId : SV_GroupID, uint3 Id : SV_DispatchThreadID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex == 0u)
	{
		SB_GroupOccupied = 0u;
		SB_GroupLumpNear = 0u;
		SB_GroupLumpForce = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (Id.x < Resolution && Id.y < Resolution)
	{
		uint Kind = SB_KindAt(int2(Id.xy));
		if (SB_IsCarried(Kind))
		{
			InterlockedOr(SB_GroupOccupied, 1u);
		}
		if (Kind == SB_KIND_SURFACE && SB_LumpWeight(CarrierLump.Load(int3(Id.xy, 0))) > 0.0)
		{
			InterlockedOr(SB_GroupLumpForce, 1u);
		}
		// 이 칸에서 묶음 한 변(8칸)씩 떨어진 아홉 칸: 아홉 칸이 묶음과 둘레 8칸을 모두 덮는다.
		for (int Y = -1; Y <= 1; ++Y)
		{
			for (int X = -1; X <= 1; ++X)
			{
				int2 Texel = int2(Id.xy) + int2(X, Y) * SB_LUMP_STENCIL_MAX;
				if (all(Texel >= int2(0, 0)) && all(Texel < int2(Resolution, Resolution))
					&& SB_LumpWeight(CarrierLump.Load(int3(Texel, 0))) > 0.0)
				{
					InterlockedOr(SB_GroupLumpNear, 1u);
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u)
	{
		if (all(GroupId.xy == 0u))
		{
			ContactArgsOut[2] = 1u;
		}
		uint Packed = GroupId.x | (GroupId.y << 16u);
		if (SB_GroupOccupied != 0u)
		{
			uint Slot;
			InterlockedAdd(ContactGroupCountOut[0], 1u, Slot);
			ContactGroupsOut[Slot] = Packed | (SB_GroupLumpNear != 0u ? SB_GROUP_LUMP_NEAR : 0u);
			// 한 줄에 SB_CONTACT_ROW개씩. 0부터 길이 - 1까지 모든 자리가 채워지므로 최댓값이 곧 min(길이, 한 줄)과 줄 수다.
			InterlockedMax(ContactArgsOut[0], min(Slot + 1u, SB_CONTACT_ROW));
			InterlockedMax(ContactArgsOut[1], Slot / SB_CONTACT_ROW + 1u);
		}
		if (SB_GroupLumpForce != 0u)
		{
			uint Slot;
			InterlockedAdd(LumpListCountOut[0], 1u, Slot);
			LumpListOut[Slot] = Packed;
		}
	}
}

// --- ContactCS 뒤 패스들 공용 ---
Texture2D<float4> ContactIn;
Texture2D<float4> PositionIn;        // ContactCS의 PositionOut (w 1 = 칸 있음)

// --- 거친 격자 공용 (솟음 ADR-0031·끌림 ADR-0033·함몰 거친 해 일지 199) ---
// 캐리어 칸 B×B를 거친 칸 하나로 묶은 격자(B = 1·2·4, 거친 칸이 1 cm를 넘지 않게 — 굽기가 거리표와 같이 정한다)의 칸 표와 랜드마크 거리표.
// 셋이 같이 쓴다(아래 솟음 주석). 함몰 패스가 거친 해를 읽으려고 함몰 부분 앞에 둔다.
// CoarseResolution·CoarseBlock·CoarseTexelSize·CoarseInfo는 덩어리 국소 모양(접촉 패스가 읽는다) 앞에 선언한다.
Texture2D<uint> CoarseRow;         // 거친 칸마다 거리표 줄 번호(살 없는 칸은 SB_LANDMARK_NO_ROW)
// 랜드마크 거리표(G16 unorm, 굽기 SoftbodyBulgeTable): 줄 r의 랜드마크 m 값은 텍셀 (r × L + m) % 폭, / 폭. 값 × 65535 = 메시 로컬 0.01 단위,
// 0xFFFF = 닿지 않음(다른 살 조각).
Texture2D<float> LandmarkDistance;
uint NumLandmarks;                 // L
uint LandmarkRowTexels;            // 줄 하나의 텍셀 수(C++ SoftbodyBulgeTable::RowTexels — 판 4: L + 16, 판 5: + 1 붙은 살갗까지 거리 b, 판 6: + 1 살 조각 번호)
#define SB_LANDMARK_TABLE_WIDTH 2048u   // 거리표 한 줄의 텍셀 수(C++ SoftbodyBulgeTable::Width)
#define SB_LANDMARK_UNIT 0.01           // 거리표 한 단위(메시 로컬, C++ SoftbodyBulgeTable::Unit)
#define SB_LANDMARK_MAX 128u  // 공유 메모리 크기(C++ SoftbodyBulgeTable::MaxLandmarks)
#define SB_LANDMARK_NEAREST 16u         // 원천마다 거리에 쓰는 가까운 랜드마크 수 = 거리표 줄 끝에 굽기가 적는 번호 수(C++ SoftbodyBulgeTable::NearestLandmarks)
#define SB_LANDMARK_UNREACHABLE 0xffffu
#define SB_LANDMARK_NO_ROW 0xffffffffu
#define SB_COARSE_GROUP 8u            // 정규화·모으기 패스의 거친 칸 묶음 한 변
#define SB_COARSE_MAX_BLOCK 4u        // 거친 칸 한 변에 묶는 캐리어 칸 수의 최대(C++ SoftbodyBulgeTable::MaxCoarseBlock) — 내리기의 펼친 읽기 수
/** 부호 있는 float를 크기 순서가 그대로인 uint로(원자 최소·최대용). */
uint SB_OrderedFloat(float F)
{
	uint U = asuint(F);
	return (U & 0x80000000u) != 0u ? ~U : (U | 0x80000000u);
}

float SB_OrderedToFloat(uint U)
{
	return asfloat((U & 0x80000000u) != 0u ? (U & 0x7fffffffu) : ~U);
}

/**
 * 거리표 줄 Row의 텍셀 Offset 값. 줄 하나 = 랜드마크 NumLandmarks개까지의 거리(0.01 메시 로컬 단위의 정수, 0xFFFF = 닿지 않음) 뒤에
 * 그 줄에서 가까운 랜드마크 SB_LANDMARK_NEAREST개의 번호(거리 작은 순, C++ SoftbodyBulgeTable::RowTexels).
 */
uint SB_LandmarkRaw(uint Row, uint Offset)
{
	uint Index = Row * LandmarkRowTexels + Offset;
	return (uint)round(LandmarkDistance.Load(int3(Index % SB_LANDMARK_TABLE_WIDTH, Index / SB_LANDMARK_TABLE_WIDTH, 0)) * 65535.0);
}

/** 거리표 줄 Row의 랜드마크 M까지의 거리. */
uint SB_LandmarkValue(uint Row, uint M)
{
	return SB_LandmarkRaw(Row, M);
}

/** 거리표 줄 Row에서 K번째로 가까운 랜드마크의 번호(굽기가 적어 둔 것). */
uint SB_LandmarkNearest(uint Row, uint K)
{
	return SB_LandmarkRaw(Row, NumLandmarks + K);
}

// SB_InCoarse·SB_CoarseTapWeight는 덩어리 국소 모양(접촉 패스가 읽는다) 앞으로 옮겼다.

/** 변형 베셀 함수 K0(x)(Abramowitz–Stegun 9.8.5·9.8.6, 절대 오차 1e-7 안). */
float SB_BesselK0(float X)
{
	X = max(X, 1e-6);
	if (X <= 2.0)
	{
		float T = X / 3.75;
		float T2 = T * T;
		float I0 = 1.0 + T2 * (3.5156229 + T2 * (3.0899424 + T2 * (1.2067492 + T2 * (0.2659732 + T2 * (0.0360768 + T2 * 0.0045813)))));
		float Y = 0.25 * X * X;
		return -log(0.5 * X) * I0
			+ (-0.57721566 + Y * (0.42278420 + Y * (0.23069756 + Y * (0.03488590 + Y * (0.00262698 + Y * (0.00010750 + Y * 0.00000740))))));
	}
	float Z = 2.0 / X;
	return exp(-X) * rsqrt(X) * (1.25331414 + Z * (-0.07832358 + Z * (0.02189568 + Z * (-0.01062446 + Z * (0.00587872 + Z * (-0.00251540 + Z * 0.00053208))))));
}

/** K0(x) = Value인 x(0.01~40 사이, 로그 이분 14번 — 값이 K0(0.01)보다 크면 0.01). */
float SB_BesselK0Inverse(float Value)
{
	float Lo = log(0.01);
	float Hi = log(40.0);
	LOOP
	for (int Step = 0; Step < 14; ++Step)
	{
		float Mid = 0.5 * (Lo + Hi);
		if (SB_BesselK0(exp(Mid)) > Value)
		{
			Lo = Mid;
		}
		else
		{
			Hi = Mid;
		}
	}
	return exp(0.5 * (Lo + Hi));
}


// --- 함몰(DentCS·DentCopyCS) ---
// 살층 한 식(ADR-0019): 함몰·끌림 모두 뼈에 붙은 살층(두께 h)이 표면에 남긴 모양이다. 살층을 옆으로 밀면 긴 파장에서
// 강성 μ/h(뼈까지 전단)와 μh/3(옆 살이 함께 끌림)이 서고, 둘레는 폭 ℓ ≈ h/√3로 잦아든다(블라소프 기초, R11 §2). 누르는 응답은 3-D 살층에서
// 만든 표의 짧은 몫(폭 ℓ₁ = 살 0.23h, 폼 0.28h — 겉껍질이 넓힌다)과 접촉 압력의 긴 몫으로 나눈다(ADR-0037). 정적 평형은 x − ℓ₁²∇²x = 0(붙든 칸 제외)이다.
// 함몰은 캐리어 칸(가는 격자)에서 DentCS가 정적 해를 체비쇼프로 풀고(ADR-0032), 끌림은 거친 격자에서 원천 커널 합으로 푼다(ADR-0033, 아래 끌림
// 패스). 둘은 서로를 읽지 않는다 — 끌림의 마찰 한도는 접촉 패스가 지난 갱신의 함몰로 정해 둔다(DragInput).
// 함몰의 체비쇼프(Saad 12.1, D⁻¹A의 고윳값 범위 [λ_lo, 2]): x_{k+1} = x_k + α_k·(x_k − x_{k−1}) + β_k·(J(x_k) − x_k). 범위 위끝 2는 대각 우세라 늘 맞고,
// 아래끝을 실제보다 높게 잡은 넓은 모양은 덜 다가갈 뿐 커지지 않는다. 닿은 칸·붙들 깊이는 반복마다 제자리로 돌린다.
Texture2D<float> StateIn;            // DentCS: 패스의 출발 함몰(cm, 첫 패스는 지난 갱신 결과 A, 둘째 패스는 첫 패스 결과). 옮기기: 둘째 패스 결과
RWTexture2D<float> StateOut;
// 이번 프레임 묶음 목록 비트(TileListCS가 쓴다, 함몰을 푸는 가는 묶음마다 1비트). 패스가 쓰는 임시 텍스처(ADR-0022)는 목록 밖 묶음에 아무 값이나
// 남아 있고, 상태 A에는 목록에서 막 빠진 묶음의 값이 옮기기 전까지 남는다. 이웃으로 읽을 때 목록 밖 묶음은 거친 해로 본다(DentCoarse, 아래). 거친
// 해를 끈 장(DentCoarseOn 0)은 0으로 본다 — 일지 197까지의 꼴(목록 밖이 0인 정적 해, ADR-0032).
Buffer<uint> DentTileBits;
// 묶음마다 다음 갱신의 목록에 남길 것(TileListCS가 읽는다). 가는 묶음은 비트: SB_RESIDUAL_HOLD = 붙든 자리(기억 F 또는 받아들인 깊이)가 있다,
// SB_RESIDUAL_DENT = 함몰이 0이 아니다(DentCS 둘째 패스가 쓰고, 옮기기가 0으로 비운 묶음은 0).
RWBuffer<uint> TileResidualOut;
#define SB_RESIDUAL_HOLD 1u
#define SB_RESIDUAL_DENT 2u
// 칸 종류 표(R8, 캐리어가 바뀔 때만 CarrierKindCS가 만든다). 캐리어 위치(RGBA32F)의 w로 읽으면 칸마다 16 B라 이완이 읽는 바이트의 1/8쯤이
// 칸 종류였다(일지 156). 아래 2비트가 SB_KindAt, 비트 2(SB_KIND_OUTER_GHOST)는 고스트 띠의 바깥 고리 — 네 이웃 가운데 빈칸이나 아틀라스 밖이
// 있는 고스트 칸이다(함몰 창이 가리키는 점의 값으로 얼린다, ADR-0032).
Texture2D<uint> CarrierKind;
#define SB_KIND_OUTER_GHOST 4u
// 살이 눌린 모양에 길든다(ADR-0003 §14). 칸마다 받아들인 깊이 a(cm): 함몰은 누르기 전(0)이 아니라 a까지만 돌아가서, 오래
// 누른 자리는 떼도 자국이 남았다가 천천히 차오른다. a는 갱신마다 함몰 쪽으로 AcceptBlend만큼 따라가고 AcceptReturn을 곱해
// 0으로 돌아간다. DentCS 첫 패스가 창에서 읽고(붙들 깊이), 둘째 패스가 제 칸만 고친다(한 장으로 제자리 갱신).
RWTexture2D<float> AcceptedOut;
float AcceptBlend;     // 갱신마다 (함몰 − a)에서 a로 옮기는 비율 (0 = 길들지 않는다)
float AcceptReturn;    // 갱신마다 a에 곱할 값 (남은 자국이 사라지는 빠르기)
// 끌림 거친 칸 입력(접촉 패스의 DragInputOut, SB_DRAG_INPUT_ROWS줄): 끌림 원천·모으기 패스가 읽는다(ADR-0033).
StructuredBuffer<float4> DragInput;

/** 이완의 칸 종류(CarrierKind 아래 2비트, SB_KindAt과 같다). */
uint SB_CarrierKindAt(int2 Texel)
{
	return CarrierKind.Load(int3(Texel, 0)) & 3u;
}

/** 칸이 든 묶음이 이번 프레임 함몰 목록(푸는 묶음)에 있는가. */
bool SB_TileListed(int2 Texel)
{
	uint Tile = (uint)(Texel.y / SB_TILE) * TilesPerSide + (uint)(Texel.x / SB_TILE);
	return ((DentTileBits[Tile >> 5u] >> (Tile & 31u)) & 1u) != 0u;
}

// --- 함몰 거친 해 (일지 199, ADR-0032 §3.1) ---
// 함몰 폭 ℓ이 창(32칸)보다 넓으면(폼·두꺼운 살 — 시연 막은 ℓ 7.5 cm = 63칸) 창 두 패스가 갱신마다 넓은 모양을 다 세우지 못한다. 한 패스에 값이
// 묶음 하나쯤만 건너가서, 누르거나 뗄 때 아직 서로 맞추지 못한 묶음 경계가 노멀 꺾임으로 보였다(일지 199 모의: 판 9.2를 뗄 때 21~28°). 그래서 같은
// 식을 거친 격자(칸 B×B, 거친 칸 ≤ 1 cm — 함몰 폭이 칸 수로 B배 좁다)에서 먼저 풀고, 가는 창은 그 해를 더한 자리에서 출발해 가까운 모양만
// 고친다. 목록 밖은 거친 해다.
//  1. DentCoarseRestrictCS: 거친 칸마다 지난 갱신의 가는 해 평균 Ū(가는 목록 밖은 지난 거친 해)와, 붙든 칸이 있으면 아래 한계
//     Ψ = Ū + max(붙들 깊이 − 지난 가는 해).
//  2. DentCoarseCS(거친 패스 1~4번, 함몰 폭에서 정한다): 거친 묶음 창(32×32칸)에서 x − ℓ²∇²x = 0, x ≥ Ψ를 체비쇼프로. 이음매는 거친 칸 링크
//     (DragLinks — 건너편 두 칸 보간)로, 열린 끝(링크가 제 칸)은 자유단으로. 이웃 거리는 지금 피부 자리(접촉 패스의 DragInput 셋째 줄).
//  3. DentCoarseAddCS: 목록 묶음의 상태 A를 출발 값으로 바꿔 둔다 — 지난 갱신에 푼 묶음은 A + 쌍선형(새 거친 해 − Ū), 새로 든 묶음은 거친 해 쌍선형.
//     칸마다 한 번이다(창은 겹쳐 칸 하나를 넷이 읽고 고스트는 넷씩 따라가므로, 창이 읽을 때마다 더하면 그만큼 되풀이한다 — 일지 200).
//  4. 가는 두 패스: 목록 묶음은 저장된 값, 목록 밖은 거친 해 쌍선형. 옮기기가 갱신 끝에 목록 묶음의 A를 모두 다시 쓴다.
// 모의(일지 199, 셰이더 꼴): 판 9.2 누르고 떼는 동안 노멀 차 ≤ 0.9°(전 21~28°), 높이 차 ≤ 0.6 mm(전 3.5), 머묾 0.04 mm. 그릇 9.1 ≤ 0.58 mm.
// 좁은 함몰(ℓ 0.7 cm)은 0.04 mm로 거친 해가 없을 때(0.2 mm)보다 낫다.
#define SB_COARSE_NONE -1e30         // 아래 한계 없음
#define SB_COARSE_TAU 5e-3           // 거친 묶음이 둘레로 목록을 넓히는 꼬리 크기(cm, 0.05 mm): 목록 끝의 0 경계가 화면에 보이지 않을 만큼
#define SB_COARSE_BUSY_ANY 1u        // 남은 표지: 거친 해가 0이 아니다(0까지 잦아들 때까지 목록에 남는다)
#define SB_COARSE_BUSY_WIDE 2u       // 남은 표지: 꼬리가 SB_COARSE_TAU를 넘는다(다음 갱신 목록을 한 겹 넓힌다)
uint DentCoarseOn;                   // 1 = 목록 밖을 거친 해로 읽는다(0 = 일지 197까지의 꼴, 목록 밖 0)
Texture2D<float> DentCoarse;         // 거친 해 U(cm, 거친 칸, 갱신을 넘어 남는다 — 이번 갱신의 거친 패스가 쓴 값)
Texture2D<float> DentCoarseFix;      // 이번 갱신의 거친 고침 U − Ū(DentCoarseAddCS가 목록 묶음의 상태 A에 더한다, 거친 목록 밖 0)
RWTexture2D<float> DentStateInOut;   // DentCoarseAddCS: 상태 A(R32F — 제 칸만 읽고 쓴다)

/**
 * 가는 칸 Texel 자리의 거친 칸 값(Source = 거친 해 또는 고침): 둘레 거친 칸 넷의 쌍선형(살 없는 거친 칸과 정지 위치가 먼 칸 — 아틀라스에서 옆에 놓였을
 * 뿐인 다른 차트 — 은 빼고 무게를 다시 나눈다, SB_CoarseSpline과 같은 규칙). 창의 경계·출발 값이라 쌍선형이면 된다(합치기는 3차 B-스플라인 —
 * SB_DentCoarseSpline; 모의에서 창이 쌍선형으로 읽어도 결과가 같았다).
 */
float SB_CoarseBilinear(Texture2D<float> Source, int2 Texel)
{
	float2 Q = (float2(Texel) + 0.5) / (float)CoarseBlock - 0.5;
	int2 Base = (int2)floor(Q);
	float2 T = Q - (float2)Base;
	float3 RestP = CarrierPosition.Load(int3(Texel, 0)).xyz;
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	float Sum = 0.0;
	float Weight = 0.0;
	UNROLL
	for (int Y = 0; Y < 2; ++Y)
	{
		UNROLL
		for (int X = 0; X < 2; ++X)
		{
			int2 Cell = Base + int2(X, Y);
			float Near = SB_CoarseTapWeight(Cell, RestP, CoarseLocal);
			if (Near <= 0.0)
			{
				continue;
			}
			float W = (X == 0 ? 1.0 - T.x : T.x) * (Y == 0 ? 1.0 - T.y : T.y) * Near;
			Sum += W * Source.Load(int3(Cell, 0));
			Weight += W;
		}
	}
	return Weight > 1e-6 ? Sum / Weight : 0.0;
}

/**
 * 칸의 저장된 함몰. 값과 목록 비트를 함께 읽는다(서로 기다리지 않게). 목록 밖 묶음은 거친 해(거친 해를 끈 장은 0, DentTileBits 주석). 목록 묶음의
 * 거친 고침·새로 든 묶음의 출발 값은 DentCoarseAddCS가 첫 패스 앞에 상태 A에 넣어 둔다. 거친 읽기는 실제 갈래로 — HLSL 2021 전 규칙(FXC)에서 ?:·&&는 두 쪽을 다 셈한다.
 */
float SB_LoadState(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	float Value = StateIn.Load(int3(Texel, 0));
	bool bListed = SB_TileListed(Texel);
	BRANCH
	if (bListed || DentCoarseOn == 0u)
	{
		return bListed ? Value : 0.0;
	}
	return SB_CoarseBilinear(DentCoarse, Texel);
}

/** 칸 좌표(정수 = 칸 한가운데)의 함몰을 이웃 네 칸에서 bilinear로. 가리키는 점은 차트 안이라 네 칸 모두 칸이 있다. */
float SB_SampleState(float2 Target)
{
	int2 Base = (int2)floor(Target);
	float2 F = Target - (float2)Base;
	float Bottom = lerp(SB_LoadState(Base), SB_LoadState(Base + int2(1, 0)), F.x);
	float Top = lerp(SB_LoadState(Base + int2(0, 1)), SB_LoadState(Base + int2(1, 1)), F.x);
	return lerp(Bottom, Top, F.y);
}

/** 고스트 칸이 가리키는 점의 함몰을 그 점 둘레 네 칸에 저장된 값으로(bilinear). */
float SB_GhostTargetStored(int2 Ghost)
{
	return SB_SampleState(CarrierRedirect.Load(int3(Ghost, 0)).xy);
}

/** 표면 칸은 저장된 값, 고스트 칸은 가리키는 점의 저장된 값. */
float SB_StateFollowOnce(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	return SB_CarrierKindAt(Texel) == SB_KIND_GHOST ? SB_GhostTargetStored(Texel) : SB_LoadState(Texel);
}

/**
 * 칸의 함몰. 고스트 칸은 옆 차트에서 가리키는 점의 값이다 — 함몰 창이 고스트 칸의 출발 값으로, 옮기기 패스가 고스트 칸에 적을 값으로
 * 쓴다. 고스트 칸에 저장된 값은 옆 차트 값을 복사해 둔 것이라 한 패스 늦다. 늦은 값을 읽으면 이음매 양쪽이 반 박자 어긋나 흔들리고
 * 양쪽이 이음매인 좁은 차트(폭 8칸 이하)에서는 프레임마다 몇 배씩 커진다(일지 95, Tools/ghost_relax_sim.py). 그래서 고스트
 * 칸은 가리키는 점의 값을 직접 읽고, 그 점 둘레 네 칸에 다시 고스트가 섞이면 한 번 더 따라간다. 거기서도 남는 늦은 몫은 두
 * bilinear 무게의 곱이라 작다(모의에서 늦은 몫이 절반까지는 늦음이 없을 때와 같다).
 */
float SB_CurrentState(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	if (SB_CarrierKindAt(Texel) != SB_KIND_GHOST)
	{
		return SB_LoadState(Texel);
	}
	float2 Target = CarrierRedirect.Load(int3(Texel, 0)).xy;
	int2 Base = (int2)floor(Target);
	float2 F = Target - (float2)Base;
	float Bottom = lerp(SB_StateFollowOnce(Base), SB_StateFollowOnce(Base + int2(1, 0)), F.x);
	float Top = lerp(SB_StateFollowOnce(Base + int2(0, 1)), SB_StateFollowOnce(Base + int2(1, 1)), F.x);
	return lerp(Bottom, Top, F.y);
}

/** 이 칸의 살층 두께 (cm). */
float SB_LayerAt(int2 Texel)
{
	return max(CarrierNormal.Load(int3(Texel, 0)).w * ThicknessScale, 1e-3);
}

/** 이웃 칸 k(0~3)의 방향: +x, −x, +y, −y. */
int2 SB_NeighborOffset(int k)
{
	return int2(k < 2 ? 1 - 2 * k : 0, k < 2 ? 0 : 5 - 2 * k);
}

/**
 * 변마다 재료 폭 비의 제곱(ℓ₁/h)²: 두 칸 평균(이웃이 없으면 제 값). 재료가 칠로 바뀌는 곳(겉껍질 눈, 살 ↔ 폼)에서 정적 해가 발산꼴
 * ∇·(ℓ²∇x)가 되어 장력이 경계를 건너는 몫이 양쪽에서 같다 — 칸마다 제 폭(ℓᵢ²∇²x)이면 센 겉껍질 조각이 눌린 자리에서 깔때기처럼 휘고
 * 조각 가장자리가 덜 내려간다(주사위 눈, 일지 280: 3-D 기준과 견줌). 재료가 고른 곳은 칸마다 제 폭과 같다. 두께는 칸마다 제 값 그대로다.
 */
float4 SB_EdgeRatio(float Own, float4 Around)
{
	return 0.5 * (Own + float4(Around.x > 0.0 ? Around.x : Own, Around.y > 0.0 ? Around.y : Own,
		Around.z > 0.0 ? Around.z : Own, Around.w > 0.0 ? Around.w : Own));
}

/** 칸의 현재 위치(ContactCS의 PositionOut, 프레임 cm). 아틀라스 밖이나 빈칸은 w = 0. */
float4 SB_LoadPosition(int2 Texel)
{
	if (any(Texel < int2(0, 0)) || any(Texel >= int2(Resolution, Resolution)))
	{
		return float4(0, 0, 0, 0);
	}
	return PositionIn.Load(int3(Texel, 0));
}

/**
 * 이웃 네 칸(+x, −x, +y, −y)까지의 실제 거리(cm). 칸 크기는 자리·방향마다 조금씩 다르다 — UV0 아일랜드는 다시 펴도 크게 굽은
 * 곳이 고르게 펴지지 않고, 이음매 양쪽 아일랜드의 칸 크기도 다르다(ADR-0007 §3.7). 이완과 퍼짐이 칸 수가 아니라 이 거리로
 * 재면 함몰 둘레 폭과 솟는 폭이 칸 크기와 상관없이 cm로 같고, 이음매를 걸친 함몰이 이음매에서 꺾이지 않는다(일지 59).
 * 이웃이 빈칸이거나 거리가 칸 크기 Cell의 1/4 ~ SB_NEIGHBOR_BAND배 밖이면(같은 점을 가리키는 고스트 등) 칸 크기로 본다. 이 띠가
 * "이웃"의 정의다 — 이웃 표를 만드는 쪽(SB_DragLink)도 같은 띠 밖의 점은 이웃으로 받지 않는다(일지 217: 띠 밖 점을 받으면 여기서
 * 칸 크기로 잘려 먼 칸이 온전한 이웃이 된다).
 */
#define SB_NEIGHBOR_BAND 4.0
float4 SB_NeighborSpacing(float3 Here, float4 There[4], float Cell)
{
	float4 Spacing;
	UNROLL
	for (int k = 0; k < 4; ++k)
	{
		float Length = length(There[k].xyz - Here);
		Spacing[k] = There[k].w > 0.0 && Length > 1.0 / SB_NEIGHBOR_BAND * Cell && Length < SB_NEIGHBOR_BAND * Cell ? Length : Cell;
	}
	return Spacing;
}

/**
 * 막 방정식(u = ℓ²∇²u)을 이웃 거리가 고르지 않은 차분으로 쓸 때 이웃 하나의 몫 = 이 값 × 1/(그 이웃까지 거리). 이 값은 네 이웃 거리
 * 평균의 역수라 칸 크기 h로 고르면 몫이 모두 1/h²이다. 1/거리는 두 칸이 같은 두 점 사이를 재므로 똑같고, 칸마다 하나인 이 값만
 * 다르다 — 몫이 대칭(칸마다 배수 × 대칭인 값)이라 함몰·끌림이 어떤 캐리어에서도 커지지 않는다(이음매 표 주석, 일지 163). 전에는 축마다
 * 2/(a(a + b))(그 축의 두 이웃 거리 a, b)로 축 방향 칸 크기를 따로 맞췄는데, 이웃마다 다른 배수라 대칭이 아니었다. 두 식의 차는 한 축
 * 칸 크기와 네 이웃 평균의 차이고, 시험 캐릭터 캐리어에서 80% 칸이 6% 안이다.
 */
float SB_RowScale(float4 Spacing)
{
	return 4.0 / dot(Spacing, float4(1, 1, 1, 1));
}

/**
 * 누르지 않은 칸에서 이 값(cm)보다 작은 함몰·기억·끌림 원천은 0으로 자른다(1 µm, 화면에 보이지 않는다). 복원이 끝난 묶음이 정확히 0이
 * 되어 목록에서 빠진다.
 */
#define SB_REST_EPSILON 1e-4
/** 묶음 목록 디스패치의 한 줄 스레드 묶음 수(TileListCS가 같은 값으로 인자를 쓴다). */
#define SB_TILE_ROW 256u
// 이번 프레임 묶음 목록(x | y << 16): [0, 함몰 묶음 수) 함몰을 푸는 가는 묶음, 그 뒤 0으로 비울 가는 묶음.
Buffer<uint> DentTiles;
Buffer<uint> DentTileCount;         // [0] 함몰을 푸는 가는 묶음 수, [1] 0으로 비울 가는 묶음 수, [SB_TILE_COUNT_PARTS] 그 앞쪽 부위 묶음 수
// 묶음 수 버퍼(TileCountOut)의 검토 R03 낱말(나머지는 SB_TILE_COUNT_WORDS 주석). ActivityArgsCS가 갱신마다 0으로 비우고 목록 패스가 쓴다.
//   [SB_TILE_COUNT_PARTS] 부위 나누기 장(bDentParts): 함몰 목록 앞쪽의 부위 묶음 수 — 채널 순열이 [0, 이 수)를, 채널 없는 순열이 그 뒤를 푼다.
//   [SB_TILE_COUNT_WHY + 0..5] Softbody.Stats가 읽는 갱신(bTileStats)만: 목록이 왜 이만큼인지. 0 켜짐(이번에 눌렸거나 붙든 자리가 남은 묶음),
//     1 그 둘레(넓힘 한 겹), 2 이음새로 든 묶음(고스트 칸이 켜짐 둘레 안을 가리키는 묶음 — 앞의 둘 밖만), 3 늘 켬 묶음(이음새 칸이 넘친 묶음 — 앞의
//     셋 밖만), 4 하드 서피스 조각을 더하기 전 목록 수, 5 = 1이면 부위 나누기 장. 이음새·늘 켬 묶음을 넓힌 둘레 = 4 − (0~3의 합), 조각이 더한
//     묶음 = [0] − 4(CPU가 뺀다 — 목록 규칙이 바뀌어도 뺄셈은 맞다).
#define SB_TILE_COUNT_PARTS 17u
#define SB_TILE_COUNT_WHY 18u
RWBuffer<uint> TileAnyResidualOut;   // [0] 1 = 어느 묶음이든 남았다(함몰 전에 0으로 비운다)
// 함몰 패스가 목록의 어느 몫을 도는가(부위 나누기 장, 검토 R03 E): 0 = 목록 전체, 1 = 앞쪽 부위 묶음 [0, TileCount[SB_TILE_COUNT_PARTS]) — 채널 순열,
// 2 = 그 뒤 나머지 — 채널 없는 순열. 둘의 결과가 순열 하나로 목록 전체를 돈 것과 같은 까닭은 TileListCS의 부위 나누기 주석.
uint DentListPart;

// --- DentCS: 누른 자리 기억과 붙든 자리의 정적 해 (ADR-0032) ---
// 상태는 칸마다 누른 자리 기억 F 하나다(HoldState): 갱신마다 F = max(이번 누른 깊이, F × e^{−RΔt}) — 받아들인 깊이처럼 칸 하나만 본다. 함몰
// 모양은 피부를 붙들 깊이 P = max(F, 받아들인 깊이)에 붙든 살층의 정적 해다(x − ℓ²∇²x = 0, 붙든 칸은 x ≥ P). 지난 모양이 답을 정하지 않으므로
// 누르고 있으면 둘레가 바로 서고 손 모양에 딱 붙는다. 손이 떠난 기억 칸(점성 칸, SB_DentSoftAt)은 붙들지 않고 켈빈-보이트 한 걸음으로 푼다 —
// 넓은 자국은 복원율 R 그대로, 테두리·손가락 끝·쓸고 간 자리의 마디처럼 좁은 모양은 더 빨리 돌아온다(ADR-0032 §9, 일지 287: 기억을 칸마다 같은
// 빠르기로 줄이면 손 모양 테두리가 깊이만 줄며 남아 지나간 자리에 둥근 테두리로 보였다).
// 정적 해는 목록 묶음(16×16)마다 둘레 8칸까지 32×32칸 창을 공유 메모리에 올려 체비쇼프 SB_DENT_ITERATIONS번으로 푼다. 창 바깥 고리 한 줄은
// 패스 처음 값으로 얼린다. 갱신마다 패스 둘: 첫 패스는 지난 갱신의 모양에서 출발하고(답이 아니라 출발점 — 둘이면 창마다 정확한 해와 0.41 mm
// 안, 일지 172), 둘째 패스는 첫 패스 값에서 다시 출발해 얼린 창 경계를 이웃 묶음의 새 값으로 고친다.
// 이음매: 창 안 고스트 칸은 산 이웃으로 같이 푼다(제 UV 이웃·두께·붙들 깊이, 출발 값은 가리키는 점). 고스트 띠의 바깥 고리(SB_KIND_OUTER_GHOST)만
// 가리키는 점의 값으로 얼린다. 창 안 몫은 두 칸 사이 거리 하나(SB_RowScale은 칸마다 배수)라 대칭과 닮은꼴이고, 패스 사이는 무게 합 1의 평균이라
// 이음매 표 없이도 어떤 캐리어·두께에서 커지지 않는다(일지 172 noise 시험).
// 스레드 256개 = 창 한 줄(32칸 = 워프 하나)씩 8줄, 스레드마다 같은 열의 칸 넷(8줄 간격). 공유 메모리를 읽는 워프는 한 줄 32칸(이웃은 한 칸
// 옆이나 한 줄 위아래)을 읽어 뱅크가 겹치지 않는다(일지 170의 32배 충돌). 전역 읽기는 창을 올릴 때 칸마다 한 번이고(고스트 칸은 가리키는 점까지
// 사슬 2~3), 반복문 안에는 전역 읽기가 없다(일지 171의 사슬).
#define SB_DENT_HALO 8
#define SB_DENT_SIDE 32              // SB_TILE + 2 × SB_DENT_HALO: 창 한 줄 = 워프 하나
#define SB_DENT_ROWS 8
#define SB_DENT_CELLS (SB_DENT_SIDE / SB_DENT_ROWS)
#define SB_DENT_AREA (SB_DENT_SIDE * SB_DENT_SIDE)
/** 패스마다 체비쇼프 반복 수(C++ DentIterations — 고윳값 범위 아래끝의 8/N²). */
#define SB_DENT_ITERATIONS 32u
uint DentPass;                       // 1 = 첫 패스(붙들 깊이를 DentPins에 쓴다), 2 = 둘째 패스(기억·받아들인 깊이·남은 것을 쓴다)
float HoldDecay;                     // e^{−R·Δt}: 기억이 갱신 하나에 줄어드는 배수
float DentTheta;                     // 체비쇼프 범위 [Low, 2]의 가운데 (2 + Low)/2
float DentDelta;                     // 반폭 (2 − Low)/2
// 누른 자리 기억 F(cm, 갱신을 넘어 남는다). 첫 패스는 창에서 읽기만 하고 둘째 패스가 제 칸만 고친다 — 첫 패스에서 고치면 아직 읽지 않은
// 이웃 묶음이 줄어든 값을 한 번 더 줄인다.
RWTexture2D<float> HoldState;
// 붙든 칸의 목표 T와 붙들 깊이 P(cm, 갱신 안 임시 RG — ADR-0037). 첫 패스가 제 칸에 쓰고(DentPins) 둘째 패스가 창에서, 합치기가 P를 읽는다(DentPinsIn —
// D3D11은 두 채널 UAV를 읽지 못해 읽기는 따로 묶는다).
RWTexture2D<float2> DentPins;
Texture2D<float2> DentPinsIn;
// 창 공유 메모리. 부위 나누기가 없는 장(순열 0): 위치 16 KB + 반복 값 두 벌 8 KB + 재료 4 KB.
// 부위 나누기(순열 SB_SEPARATION): 32 KB 한 덩이를 나눠 쓴다 — 창 올리기 동안 [0, 2A) 반복 값 두 벌, [2A, 6A) 위치(성분마다 한 줄 — 워프가 읽는 한
// 줄이 이어져 뱅크가 겹치지 않는다), [6A, 7A) 재료. 이웃 몫을 다 셈한 뒤 [2A, 8A)를 채널 셋 × 두 벌로 다시 쓴다. 남은 표지는 반복이 끝난 뒤 [0]에 모은다.
#if SB_SEPARATION
groupshared uint SB_DentShared[8 * SB_DENT_AREA];
#define SB_DENT_BUSY SB_DentShared[0]
float SB_DentValueGet(uint Index) { return asfloat(SB_DentShared[Index]); }
void SB_DentValueSet(uint Index, float Value) { SB_DentShared[Index] = asuint(Value); }
float4 SB_DentPositionGet(uint Slot)
{
	return asfloat(uint4(SB_DentShared[2u * SB_DENT_AREA + Slot], SB_DentShared[3u * SB_DENT_AREA + Slot], SB_DentShared[4u * SB_DENT_AREA + Slot],
		SB_DentShared[5u * SB_DENT_AREA + Slot]));
}
void SB_DentPositionSet(uint Slot, float4 Position)
{
	SB_DentShared[2u * SB_DENT_AREA + Slot] = asuint(Position.x);
	SB_DentShared[3u * SB_DENT_AREA + Slot] = asuint(Position.y);
	SB_DentShared[4u * SB_DENT_AREA + Slot] = asuint(Position.z);
	SB_DentShared[5u * SB_DENT_AREA + Slot] = asuint(Position.w);
}
float SB_DentMaterialGet(uint Slot) { return asfloat(SB_DentShared[6u * SB_DENT_AREA + Slot]); }
void SB_DentMaterialSet(uint Slot, float Material) { SB_DentShared[6u * SB_DENT_AREA + Slot] = asuint(Material); }
/** 채널 k(0~2)의 반복 값: Half는 0 또는 SB_DENT_AREA(반복 값과 같은 번갈아 읽기). Buffer는 HLSL 형 이름이라 FXC가 받지 않는다. */
float SB_DentChannelGet(uint k, uint Half, uint Slot) { return asfloat(SB_DentShared[(2u + 2u * k) * SB_DENT_AREA + Half + Slot]); }
void SB_DentChannelSet(uint k, uint Half, uint Slot, float Value) { SB_DentShared[(2u + 2u * k) * SB_DENT_AREA + Half + Slot] = asuint(Value); }
#else
groupshared float4 SB_DentPosition[SB_DENT_AREA];   // 칸의 현재 위치(이웃 거리, w > 0 = 칸 있음), 16 KB
groupshared float SB_DentValue[2 * SB_DENT_AREA];   // 반복 값 두 벌(번갈아 읽고 쓴다), 8 KB
// 칸의 재료 폭 비의 제곱 (ℓ₁/h)²(v, τ) — 0 = 칸 없음, 4 KB. 이웃 몫은 변마다 두 칸 평균으로 정한다(아래 SB_EdgeRatio).
groupshared float SB_DentMaterial[SB_DENT_AREA];
groupshared uint SB_DentBusy;
#define SB_DENT_BUSY SB_DentBusy
float SB_DentValueGet(uint Index) { return SB_DentValue[Index]; }
void SB_DentValueSet(uint Index, float Value) { SB_DentValue[Index] = Value; }
float4 SB_DentPositionGet(uint Slot) { return SB_DentPosition[Slot]; }
void SB_DentPositionSet(uint Slot, float4 Position) { SB_DentPosition[Slot] = Position; }
float SB_DentMaterialGet(uint Slot) { return SB_DentMaterial[Slot]; }
void SB_DentMaterialSet(uint Slot, float Material) { SB_DentMaterial[Slot] = Material; }
#endif
/** 이번 갱신의 기억: 누른 칸은 max(누른 깊이, 줄인 지난 값), 아니면 줄인 지난 값(1 µm 아래는 0). */
float SB_HoldNow(float Pressed, float Held)
{
	float Decayed = Held * HoldDecay;
	return Pressed > 0.0 ? max(Pressed, Decayed) : (Decayed < SB_REST_EPSILON ? 0.0 : Decayed);
}

/** 첫 패스의 붙들 깊이: max(이번 기억, 받아들인 깊이). */
float SB_DentPinNow(int2 Texel)
{
	float Pressed = max(ContactIn.Load(int3(Texel, 0)).w, 0.0);
	return max(SB_HoldNow(Pressed, HoldState[Texel]), AcceptedOut[Texel]);
}

/**
 * 압력이 실리는 붙들 깊이(ADR-0037, 일지 287): 지금 손이 닿은 칸만 SB_DentPinNow, 손이 떠난 칸은 0이다. 기억만 남은 칸에는 실린 힘이 없다 —
 * 넣으면 지나간 자국의 가는 띠가 압력 칸이 되어 풀이가 튀었고, 뗀 뒤에도 둘레 솟음이 남았다. 압력 칸·조각 위 손 칸이 이 값을 쓴다.
 */
float SB_PressurePinNow(int2 Texel)
{
	// 압력은 응답이라 살 두께에 붙인 깊이로 낸다(일지 299). 붙들 깊이 자체(피부 자리)는 붙이지 않는다.
	return ContactIn.Load(int3(Texel, 0)).w > 0.0 ? SB_PressLimit(SB_DentPinNow(Texel), SB_LayerAt(Texel)) : 0.0;
}

/**
 * 점성 칸인가: 지금 닿지 않고 기억이 받아들인 깊이보다 깊은 칸(손이 떠난 자리). 이 칸은 붙들지 않고 함몰 창이 켈빈-보이트 한 걸음으로 푼다 —
 * 칸 줄: U(1 + G) = (1 + c)·T + G·J, c = e^{RΔt} − 1, G = c(1 + ΣShare), T = 이번 기억(줄인 값)의 목표(SB_DentTarget — 실린 힘이 없어
 * 압축 몫 없이 기억 + 긴 몫), J = 이웃 몫. 살이 되미는 힘
 * (1 + ΣShare)U − Σ Share·U_j가 점성을 이겨 기억을 한 걸음 줄인 꼴이라, 평평한 자국은 정확히 e^{−RΔt}배(지금 붙든 것과 같다), 둘레보다 튀어나온
 * 칸일수록 더 줄어든다. 갱신 빈도가 낮아도 넘치지 않는다(암시적 걸음). 새 기억은 줄인 지난 기억을 넘지 않는다(누를 때만 깊어진다 — 어느 칸도
 * R보다 느리게 돌아오지 않는다). 줄지 않는 설정(복원율 0)에서는 점성 칸이 없다.
 */
bool SB_DentSoftAt(int2 Texel)
{
	return HoldDecay < 1.0 && ContactIn.Load(int3(Texel, 0)).w <= 0.0 && SB_HoldNow(0.0, HoldState[Texel]) > AcceptedOut[Texel];
}

/** 굳은 붙들 깊이: 점성 칸은 받아들인 깊이만(기억은 점성으로 푼다), 나머지는 SB_DentPinNow. 거친 함몰의 아래 한계가 쓴다. */
float SB_DentHardNow(int2 Texel)
{
	return SB_DentSoftAt(Texel) ? AcceptedOut[Texel] : SB_DentPinNow(Texel);
}

// --- 긴 몫과 거친 압력 읽기 (ADR-0037 §3.1) ---
// ContactLongGatherCS가 거친 칸마다 쓴 (L, p): L = 살층 응답의 긴 몫(솟음 +, cm), p = 그 거친 칸 압력 칸의 접촉 압력(μ 단위). 가는 칸에서는 값을 지나는
// 3차(Catmull-Rom)로 읽는다 — 근사 B-스플라인은 얇은 살에서 가까운 모양을 깎아 모양 오차가 두 배였고(일지 275), 쌍선형은 거친 칸 경계에서 기울기가
// 꺾인다. 탭 무게는 SB_CoarseTapWeight로 거르고 무게 합으로 다시 나눈다(합이 절반 밑이면 남은 탭의 쌍선형). DentCS·함몰 거친 바닥·합치기가 이 한 읽기를 같이 쓴다.
Texture2D<float2> ContactLong;
uint bContactLong;                   // 1 = 거친 압력 풀이와 긴 몫이 돈다(Full). 0이면 목표 T = P(Basic)

// --- 부위 나누기(Separated Parts) ---
// 겉면은 이어져 있어도 속살은 따로인 두 부위(옷이 메운 가슴골의 두 가슴 등 — 메시에 없는 정보라 저작자가 저작 에셋의 Separated Parts로 정한다)는 서로
// 솟음·함몰 꼬리·끌림을 건네지 않는다. 살층 식은 그대로이고 칸 사이 전달에 배율 하나를 곱한다: 출처 칸 j가 받는 칸 i에 주는 몫 × m_ij,
//   m_ij = 1 − (s_ia s_jb − s_ib s_ja)² / ((s_ia + s_ib)(s_ja + s_jb))     (s_a, s_b = 칸의 두 부위 본 웨이트, 굽기가 CarrierSeparation에 담는다)
// 같은 칸끼리 1, 순수 a ↔ 순수 b는 0, 어느 쪽이든 부위 밖(s_a + s_b = 0)이면 1이다. 옷이 두 부위에 섞여 걸린 띠는 섞인 만큼 건넨다. 셰이더가 부위 쌍을 고르는
// 규칙이 아니라 굽기 입력이 정한 나눔이고, 배율은 모든 칸 쌍에 같은 식으로 든다(ADR-0037 부위 나누기).
// 배율이 받는 칸 몫과 출처 몫의 곱으로 풀려, 출처 무게가 다른 채널 셋(s_a, s_b, q = (s_a² + s_b²)/(s_a + s_b))을 같은 식으로 같이 모으거나 풀고 받는 칸에서
// SB_SeparationCorrection을 빼면 정확하다 — 솟음(긴 몫)은 모으기가 채널을 같이 모으고, 함몰 꼬리는 DentCS가 같은 반복·같은 붙든 칸으로 채널을 같이 푼다.
// 끌림은 원천을 하나씩 세므로 원천마다 배율을 곱한다.
Texture2D<float> CarrierSeparation;   // G16 칸 몫: 낮은 8비트 s_a, 높은 8비트 s_b(0~255 → 0~1). bSeparation이 0이면 묶이지 않은 자리 텍스처라 읽지 않는다
uint bSeparation;                     // 1 = 캐리어에 부위 나누기가 구워져 있고 켜져 있다(순열 SB_SEPARATION과 같은 값)
Texture2D<float4> ContactLongSeparation;   // 거친 칸마다 솟음 채널(출처 무게 s_a, s_b, q로 모은 긴 몫, w 쓰지 않음 — ContactLongGatherCS)

/** 칸의 두 부위 몫 (s_a, s_b). */
float2 SB_SeparationAt(int2 Texel)
{
	uint Packed = (uint)round(CarrierSeparation.Load(int3(Texel, 0)) * 65535.0);
	return float2((float)(Packed & 255u), (float)(Packed >> 8u)) * (1.0 / 255.0);
}

/** 채널 셋의 출처 무게 (s_a, s_b, q). 부위 밖 칸은 0. */
float3 SB_SeparationWeights(float2 S)
{
	float G = S.x + S.y;
	return float3(S, G > 1e-6 ? dot(S, S) / G : 0.0);
}

/** 출처 칸(몫 Sj)이 받는 칸(몫 Si)에 주는 몫의 배율 m_ij — 끌림처럼 원천을 하나씩 셀 때. */
float SB_SeparationMultiplier(float2 Si, float2 Sj)
{
	float Gi = Si.x + Si.y;
	float Gj = Sj.x + Sj.y;
	if (Gi <= 1e-6 || Gj <= 1e-6)
	{
		return 1.0;
	}
	float X = Si.x * Sj.y - Si.y * Sj.x;
	return saturate(1.0 - X * X / (Gi * Gj));
}

/**
 * 받는 칸(몫 S)에서 채널 값 C = (C_a, C_b, C_q)로 막힌 몫: ½q(C_a + C_b) + ½g·C_q − s_a·C_a − s_b·C_b (g = s_a + s_b, q = 그 칸의 출처 무게 q).
 * 채널 없이 푼 값에서 빼면 배율을 곱한 합이다. 채널이 칸 자신의 무게대로면(C_k = 무게_k × U — 붙든 칸) 0이다.
 */
float SB_SeparationCorrection(float2 S, float3 C)
{
	float G = S.x + S.y;
	if (G <= 1e-6)
	{
		return 0.0;
	}
	float Q = dot(S, S) / G;
	return 0.5 * Q * (C.x + C.y) + 0.5 * G * C.z - S.x * C.x - S.y * C.y;
}

float4 SB_CatmullRomWeights(float T)
{
	float T2 = T * T;
	float T3 = T2 * T;
	return float4(-T3 + 2.0 * T2 - T, 3.0 * T3 - 5.0 * T2 + 2.0, -3.0 * T3 + 4.0 * T2 + T, T3 - T2) * 0.5;
}

/** 가는 칸 자리 Where(정수 = 칸 한가운데 — 고스트 칸은 가리키는 점)의 (L, p). RestP = 그 칸의 정지 위치(탭이 같은 차트인지 거른다). */
float2 SB_ContactLongAt(float2 Where, float3 RestP)
{
	float2 Q = (Where + 0.5) / (float)CoarseBlock - 0.5;
	int2 First = (int2)floor(Q) - 1;
	float2 T = Q - floor(Q);
	float4 WX = SB_CatmullRomWeights(T.x);
	float4 WY = SB_CatmullRomWeights(T.y);
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	float2 Sum = float2(0, 0);
	float Weight = 0.0;
	float2 SumLin = float2(0, 0);
	float WeightLin = 0.0;
	float2 SumAny = float2(0, 0);
	float WeightAny = 0.0;
	// 부위 나누기: 솟음 채널 셋도 같은 탭 무게로 읽는다(합·쌍선형·가까운 평균 세 벌).
	bool bChannels = bSeparation != 0u;
	float3 ChanSum = float3(0, 0, 0);
	float3 ChanLin = float3(0, 0, 0);
	float3 ChanAny = float3(0, 0, 0);
	UNROLL
	for (int Y = 0; Y < 4; ++Y)
	{
		UNROLL
		for (int X = 0; X < 4; ++X)
		{
			int2 Cell = First + int2(X, Y);
			float Near = SB_CoarseTapWeight(Cell, RestP, CoarseLocal);
			if (Near <= 0.0)
			{
				continue;
			}
			float2 Value = ContactLong.Load(int3(Cell, 0));
			float3 Chan = float3(0, 0, 0);
			BRANCH
			if (bChannels)
			{
				Chan = ContactLongSeparation.Load(int3(Cell, 0)).xyz;
			}
			float W = WX[X] * WY[Y] * Near;
			Sum += W * Value;
			Weight += W;
			ChanSum += W * Chan;
			if (X >= 1 && X <= 2 && Y >= 1 && Y <= 2)
			{
				float WL = (X == 1 ? 1.0 - T.x : T.x) * (Y == 1 ? 1.0 - T.y : T.y) * Near;
				SumLin += WL * Value;
				WeightLin += WL;
				ChanLin += WL * Chan;
			}
			float2 Off = float2(X, Y) - 1.0 - T;
			float WA = Near / (1.0 + dot(Off, Off));
			SumAny += WA * Value;
			WeightAny += WA;
			ChanAny += WA * Chan;
		}
	}
	// 차트 가장자리에서 3차 탭이 빠지면 쌍선형으로 — 문턱 하나에서 갈아타면 그 선이 계단으로 보여(일지 277) 무게 합 0.3 ~ 0.7에서 섞는다. 가운데 네 탭이 다
	// 빠진 칸(빈 거친 칸 쪽 고스트)은 있는 탭의 가까운 평균.
	float2 Linear = WeightLin > 1e-6 ? SumLin / WeightLin : (WeightAny > 1e-6 ? SumAny / WeightAny : float2(0, 0));
	float Blend = saturate((Weight - 0.3) / 0.4);
	float2 Result = Blend > 0.0 ? lerp(Linear, Sum / max(Weight, 1e-6), Blend) : Linear;
	BRANCH
	if (bChannels)
	{
		// 받는 칸의 몫으로 막힌 솟음을 뺀다(거친 칸에서 끊으면 3차 보간이 끊긴 계단을 넘쳐 반대쪽이 꺼진다 — 채널째 올려 가는 칸에서 합친다).
		float3 Lin = WeightLin > 1e-6 ? ChanLin / WeightLin : (WeightAny > 1e-6 ? ChanAny / WeightAny : float3(0, 0, 0));
		float3 Chan = Blend > 0.0 ? lerp(Lin, ChanSum / max(Weight, 1e-6), Blend) : Lin;
		int2 Home = clamp((int2)round(Where), int2(0, 0), int2(Resolution - 1, Resolution - 1));
		Result.x -= SB_SeparationCorrection(SB_SeparationAt(Home), Chan);
	}
	return Result;
}

/**
 * 붙든 칸(붙들 깊이 P > 0)의 가는 창 목표 T = P + L − γ·h·A₁·p(ADR-0037 §3.1 ⑤): 긴 몫 L만큼 더 눌러 두고(합치기가 U − L), 칸보다 짧은 압축 몫 γ·h·A₁·p는
 * 살 속에서 일어나는 몫이라 거친 압력으로 뺀다(γ를 빼지 않으면 손 옆이 1 mm 깊다 — 일지 275). 압축 몫은 그 칸에 실린 힘의 몫이라 지금 손이 닿은 칸만
 * 뺀다 — 압력 칸과 같은 조건(SB_PressurePinNow)이고, 손이 떠난 칸(점성 칸·받아들인 깊이만 남은 칸)에는 실린 힘이 없다(일지 287: 둘레 압력을 보간한
 * p를 빼면 점성 칸의 기억이 갱신마다 그만큼 깊어져 손을 흔들며 누를 때 튀었다). Basic(긴 몫 없음)은 P. Where = 긴 몫을 읽는 자리(고스트 칸은
 * 가리키는 점 — 함몰·끌림처럼 이음매 양쪽이 같은 값을 읽는다, 일지 277).
 */
float SB_DentTarget(int2 Texel, float2 Where, float P)
{
	if (bContactLong == 0u)
	{
		return P;
	}
	float3 RestP = CarrierPosition.Load(int3(Texel, 0)).xyz;
	float2 LP = SB_ContactLongAt(Where, RestP);
	float Layer = SB_LayerAt(Texel);
	float3 Window = SB_ResponseWindow(SB_VolumeAt(Texel), SB_SkinLengthAt(Texel) * ThicknessScale / Layer);
	float Loaded = ContactIn.Load(int3(Texel, 0)).w > 0.0 ? max(LP.y, 0.0) : 0.0;
	return P + LP.x - Window.y * Layer * Window.z * Loaded;
}

#if SB_HARD_SURFACE
// 하드 서피스(ADR-0037 §3.7, ADR-0038 §4·§4.2): 칸마다 조각 번호(G16, 굽기 — 발자국 칸·조각 칸·그 고스트), 조각마다 틀(메시 로컬 3줄: 가운데,
// 평면 축 U, 축 V). 압력 풀이 꼬리가 강체 후보 조각마다 자세 평면을 쓰고 조각을 켠다(HardPieceMotion.w — 2 붙은 강체, 3 떠 있음). 켜진 조각의
// 발자국 칸은 판에 붙는다 — 살이 판에서 떨어지지도 판 위로 솟지도 않는다(원칙 E07).
Texture2D<float> CarrierPiece;
StructuredBuffer<float4> HardPieceFrames;
StructuredBuffer<float4> HardPieceMotion;
RWStructuredBuffer<float4> HardPieceMotionOut;   // 압력 풀이 꼬리가 쓴다: (가라앉음 cm, U 기울기, V 기울기, 켜짐 — 2 붙은 강체, 3 떠 있음)
Texture2D<float> CarrierSoftness;                // 칸마다 Flex 칠(G16 — 옛 굽기는 조각 칸만 칠, 나머지 1)
uint bCarrierSoftness;                           // 1 = CarrierSoftness가 묶여 있다(0 = Flex 칠 없음 — 모든 칸 칠 1, 연속 굽힘은 Flex Offset만 본다)
Texture2D<float> ContactBend;                    // 연속 굽힘: 굽힘 거친 해 W_c(거친 칸, cm, 아래로 + — 모으기 패스). 함몰 창 용수철의 목표
// 연속 굽힘 함몰 창 용수철 κ = HOLD·h·A₁·ℓ_b³/(거친 칸)⁴(재현 replica.BEND_HOLD — 짝 L03). 판 붙듦(SB_HARD_HOLD 64)보다 약하다: 굽는 칸은 따로 움직이는
// 판이 아니라 거친 풀이가 이미 굽힘을 담은 살이라, 함몰 창은 거친 해에서 칸보다 짧은 몫만 고친다(실험 HOLD 2~64 훑기에서 16).
#define SB_BEND_HOLD 16.0
float HardRefThickness;                          // 굽힘 길이 눈금 L0(고정 길이 SoftbodyContactResponse::ReferenceLength, 메시 로컬 — 배치 2)
float FlexOffset;                                // 컴포넌트 Flex Offset(−1~1): 판 칠에 더한다
uint HardPieceCount;
// 가는 창 판 붙듦(재현 replica_plate.py HOLD·BLACK_ELL — 짝 L03, 굽기 SoftbodyCarrierBake의 검정 ℓ_b와 같은 값)
#define SB_HARD_HOLD 64.0        // 붙듦 κ = HOLD·h·A₁·D/(거친 칸)⁴ — 재현 §10.1: 32~128에서 최악 10.4~14.2 %
#define SB_HARD_BLACK_ELL 50.0   // 검정(ℓ_b > 50 L0)은 ℓ_b = 50 L0인 아주 센 판

/**
 * Flex 칠 S → 굽힘 길이 ÷ L0(배치 2): 0.6·√((1 − S)/S), 흰색 1은 0(맨살), 검정 쪽은 SB_HARD_BLACK_ELL에서 멈춘다. L0 두께 살에서 손 옆 판 띠가
 * 판째 가라앉는 몫이 약 1 − S가 되게 맞춘 곡선이다(3-D 판 기준 — 띠 몫 ≈ x²/(x² + 0.36), x = ℓ_b/h). 굽기 SoftbodyPlateBake::BendingLength,
 * 재현 plate_ref.ell_b와 같은 곡선(짝 L03).
 */
float SB_HardBendRatio(float S)
{
	return S >= 1.0 ? 0.0 : min(0.6 * sqrt((1.0 - S) / max(S, 1e-6)), SB_HARD_BLACK_ELL);
}
// 조각마다 함몰 묶음 목록 구간(첫 자리, 수)과 목록(x | y << 16) — 목록 패스가 조각 묶음을 통째로 넣는다.
StructuredBuffer<uint2> HardPieceTileRange;
StructuredBuffer<uint> HardPieceTileList;

// 조각 번호 텍스처의 비트 15: 판을 그리는 칸(굽기 SoftbodyHardSurface::DrawPlateBit와 짝, 판 5 — fb/orn 안 A). 떠 있는 장식의 렌더 정점이 읽는 칸이다 —
// 장식 칸(장식 삼각형 위의 칸), 장식 칸이 없는 판(자동 섬 지도)은 발자국 칸. 아래 15비트가 조각 번호다.
#define SB_HARD_DRAW_PLATE 0x8000u

/** 칸의 조각 번호 원값(아래 15비트 = 조각 번호, 비트 15 = 판을 그리는 칸). */
uint SB_HardPieceRawAt(int2 Texel)
{
	return (uint)round(CarrierPiece.Load(int3(Texel, 0)) * 65535.0);
}

/** 칸의 조각 번호(1부터, 0 = 조각 아님). 판을 그리는 칸 표지는 뺀다 — 풀이·함몰은 장식 칸과 발자국 칸을 같은 조각으로 본다. */
uint SB_HardPieceAt(int2 Texel)
{
	return SB_HardPieceRawAt(Texel) & (SB_HARD_DRAW_PLATE - 1u);
}

/** 판을 그리는 칸인가(SB_HARD_DRAW_PLATE): 합치기가 이 칸에만 판 자세를 그린다. 발자국(장식 밑 셔츠) 칸은 살로 그린다. */
bool SB_HardDrawsPlateCell(int2 Texel)
{
	return (SB_HardPieceRawAt(Texel) & SB_HARD_DRAW_PLATE) != 0u;
}

/**
 * 조각(0부터)의 종류 — 0 칠대로 굽는 겉(판 아님, 연속 굽힘, ADR-0038 §4.2), 1 붙은 강체, 2 떠 있는 장식(늘 강체, 얹힘). 조각 틀 셋째 줄 w = 떠 있는
 * 조각이면 −2(C++ SoftbodyContactResponse::FloatingPieceKind), 아니면 조각 자기 칠의 가장 큰 값(굽기 PieceOwnFlex — 판 테두리가 아닌 정점 코너)
 * − 강체 문턱 칠(C++ RigidFlexLimit: 칠 + Flex Offset의 굽힘 길이가 조각 반지름의 RigidPieceRatio배 이상이 되는 가장 큰 칠). w + Flex Offset ≤ 0이면
 * 강체다 — 굽힘 길이보다 훨씬 작은 조각은 한 몸으로 움직여, 굽는 겉과 강체가 이미 비슷한 자리에서 바뀐다. 검정은 크기와 상관없이 강체. 굽기 데이터는
 * 그대로라 Flex Offset이 바뀌어도 다시 만들 것이 없다.
 */
uint SB_HardKindOf(uint Piece)
{
	float Kind = HardPieceFrames[Piece * 3u + 2u].w;
	return Kind < -1.5 ? 2u : (Kind + FlexOffset <= 0.0 ? 1u : 0u);
}

/**
 * 칸의 겉 굽힘 길이 ℓ_b(cm, Flex 칠의 연속 굽힘 — scratchpad f11/bend, 재현 짝 replica.Replica.bend_length): S = saturate(칠 + Flex Offset)에서
 * ℓ_b = L0·SB_HardBendRatio(S)(판과 같은 곡선), 굽힘 표 끝 매듭(SB_ResponseBendLength 끝)에서 멈춘다. 흰색(S = 1)은 0이라 지금 그대로다.
 * 강체 판의 칸도 0이다: 붙은 검정 판(SB_HardKindOf 1)의 칸과 판을 그리는 칸(SB_HARD_DRAW_PLATE — 떠 있는 장식 칸)은 그 굳기를 판이 맡는다(판 위에
 * 굽힘을 한 번 더 세지 않는다). 떠 있는 장식의 발자국(장식 밑 셔츠) 칸은 살로 그리므로 셔츠 칠대로 휜다(fb/orn 안 A — 둘레 셔츠와 같은 굳기).
 * 조각 없는 자산은 HardPieceCount가 0이라 조각 번호를 보지 않는다(CarrierPiece가 빈 자리 텍스처여도 된다).
 */
float SB_BendLengthAt(int2 Texel)
{
	float Paint = bCarrierSoftness != 0u ? CarrierSoftness.Load(int3(Texel, 0)) : 1.0;
	float S = saturate(Paint + FlexOffset);
	if (S >= 1.0)
	{
		return 0.0;
	}
	uint PieceRaw = SB_HardPieceRawAt(Texel);
	uint Piece = PieceRaw & (SB_HARD_DRAW_PLATE - 1u);
	if (Piece != 0u && Piece <= HardPieceCount && ((PieceRaw & SB_HARD_DRAW_PLATE) != 0u || SB_HardKindOf(Piece - 1u) == 1u))
	{
		return 0.0;
	}
	return min(max(HardRefThickness, 0.0) * ThicknessScale * SB_HardBendRatio(S), SB_ResponseBendLength[SB_RESPONSE_BEND_COUNT - 1]);
}

/** 판 좌표(cm): 조각 Piece(0부터) 틀의 가운데에서 축 U·V로 잰 자리. */
float2 SB_HardPlateXY(uint Piece, float3 Rest)
{
	uint Row = Piece * 3u;
	float3 D = (Rest - HardPieceFrames[Row].xyz) * ThicknessScale;
	return float2(dot(D, HardPieceFrames[Row + 1u].xyz), dot(D, HardPieceFrames[Row + 2u].xyz));
}

/**
 * 켜진 조각의 판 깊이(cm, 함몰과 같은 부호 — 아래로 +): 압력 풀이가 쓴 자세 평면을 이 칸 쉼 자리의 판 좌표에서 읽는다(차트 이음매 단이 없다).
 * 판은 강체뿐이다(붙은 검정 w 2·떠 있는 장식 w 3 — ADR-0038 §4.2). 조각 아닌 칸·꺼진 조각이면 false.
 */
bool SB_HardPlateAt(int2 Texel, out float Depth)
{
	Depth = 0.0;
	uint Piece = SB_HardPieceAt(Texel);
	if (Piece == 0u || Piece > HardPieceCount || HardPieceMotion[Piece - 1u].w <= 1.5)
	{
		return false;
	}
	float4 Motion = HardPieceMotion[Piece - 1u];
	Depth = Motion.x + dot(Motion.yz, SB_HardPlateXY(Piece - 1u, CarrierPosition.Load(int3(Texel, 0)).xyz));
	return true;
}

/**
 * 켜진 강체 조각의 발자국 칸(ADR-0038 §4): 가는 창은 이 칸을 판 목표 쪽 용수철 하나로 붙든다(재현 replica_plate.py fine='kappa',
 * gamma_fine='split'). 붙듦 κ = HOLD·h·A₁·D/(거친 칸)⁴(D = ℓ_b³, ℓ_b = L0·SB_HardBendRatio(S + Flex Offset) — 강체 판 칸은 칠과 상관없이 검정이라
 * 50 L0)와 짧은 압축 무름 γ를 잇따라 둔다: 세기 Spring = κ/(1 + γκ)(가는 창 줄 대각 단위), 압축이 가져가는 몫 Share = w = γ·Spring. 목표
 * Aim = 판 + L − (1 − w)γ·h·A₁·p — 나머지 몫은 거친 p에서. 검정(w → 1)은 세기 1/γ인 무른 핀이다(재현 §10.2: 검정 고리 0.0042). Long = L, Squeeze = γhA₁p.
 * p는 0에서 자르지 않는다 — 판의 들린 쪽은 거친 압력이 당김(음수)이다. 조각 아닌 칸·꺼진 조각이면 false.
 */
bool SB_HardSpringAt(int2 Texel, float2 Where, out float Spring, out float Aim, out float Share, out float Long, out float Squeeze)
{
	Spring = 0.0;
	Aim = 0.0;
	Share = 0.0;
	Long = 0.0;
	Squeeze = 0.0;
	// 떠 있는 장식(조각 움직임 w = 3)의 발자국은 용수철이 아니라 한쪽 핀이다(SB_HardFloatAt, 배치 3)
	uint PieceS = SB_HardPieceAt(Texel);
	if (PieceS != 0u && PieceS <= HardPieceCount && HardPieceMotion[PieceS - 1u].w > 2.5)
	{
		return false;
	}
	float Plate;
	if (!SB_HardPlateAt(Texel, Plate))
	{
		return false;
	}
	float Layer = SB_LayerAt(Texel);
	float3 Window = SB_ResponseWindow(SB_VolumeAt(Texel), SB_SkinLengthAt(Texel) * ThicknessScale / Layer);
	// 강체 판(조각 움직임 w ≥ 2)의 칸은 칠과 상관없이 검정으로 붙든다 — 크기 비로 강체가 된 회색 조각도 판 그대로 그린다(SB_HardKindOf).
	float S = HardPieceMotion[PieceS - 1u].w > 1.5 ? 0.0 : saturate(CarrierSoftness.Load(int3(Texel, 0)) + FlexOffset);
	float Bend = max(HardRefThickness, 0.0) * ThicknessScale * SB_HardBendRatio(S);
	float Cell = (float)CoarseBlock * TexelSize;
	float Raw = min(SB_HARD_HOLD * Layer * Window.z * Bend * Bend * Bend / max(Cell * Cell * Cell * Cell, 1e-12), 1e20);
	Spring = Raw / (1.0 + Window.y * Raw);
	Share = Window.y * Spring;
	if (bContactLong != 0u)
	{
		float2 LP = SB_ContactLongAt(Where, CarrierPosition.Load(int3(Texel, 0)).xyz);
		Long = LP.x;
		Squeeze = Window.y * Layer * Window.z * LP.y;
	}
	Aim = Plate + Long - (1.0 - Share) * Squeeze;
	return true;
}

/**
 * 연속 굽힘 용수철의 목표(함몰 창 U 공간, cm): 가는 칸 자리 Where에서 W_c + L(긴 몫)을 SB_ContactLongAt과 같은 탭으로 읽는다(값을 지나는 3차, 다른 차트
 * 탭 빼기, 무게 합 0.3~0.7에서 쌍선형과 섞기). 합치기가 U − L을 그리므로 그린 깊이가 W_c 쪽으로 간다. L은 부위 나누기로 고치지 않은 값이다: W_c는 부위를
 * 모르고 먼 몫이 맨 긴 몫과 같아(굽힘은 짧은 파장만 바꾼다), 합치기가 고친 L을 빼면 다른 부위로 넘어간 솟음이 그대로 빠진다.
 */
float SB_BendAimAt(float2 Where, float3 RestP)
{
	float2 Q = (Where + 0.5) / (float)CoarseBlock - 0.5;
	int2 First = (int2)floor(Q) - 1;
	float2 T = Q - floor(Q);
	float4 WX = SB_CatmullRomWeights(T.x);
	float4 WY = SB_CatmullRomWeights(T.y);
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	float Sum = 0.0;
	float Weight = 0.0;
	float SumLin = 0.0;
	float WeightLin = 0.0;
	float SumAny = 0.0;
	float WeightAny = 0.0;
	UNROLL
	for (int Y = 0; Y < 4; ++Y)
	{
		UNROLL
		for (int X = 0; X < 4; ++X)
		{
			int2 Cell = First + int2(X, Y);
			float Near = SB_CoarseTapWeight(Cell, RestP, CoarseLocal);
			if (Near <= 0.0)
			{
				continue;
			}
			float Value = ContactBend.Load(int3(Cell, 0)) + ContactLong.Load(int3(Cell, 0)).x;
			float W = WX[X] * WY[Y] * Near;
			Sum += W * Value;
			Weight += W;
			if (X >= 1 && X <= 2 && Y >= 1 && Y <= 2)
			{
				float WL = (X == 1 ? 1.0 - T.x : T.x) * (Y == 1 ? 1.0 - T.y : T.y) * Near;
				SumLin += WL * Value;
				WeightLin += WL;
			}
			float2 Off = float2(X, Y) - 1.0 - T;
			float WA = Near / (1.0 + dot(Off, Off));
			SumAny += WA * Value;
			WeightAny += WA;
		}
	}
	float Linear = WeightLin > 1e-6 ? SumLin / WeightLin : (WeightAny > 1e-6 ? SumAny / WeightAny : 0.0);
	float Blend = saturate((Weight - 0.3) / 0.4);
	return Blend > 0.0 ? lerp(Linear, Sum / max(Weight, 1e-6), Blend) : Linear;
}

/**
 * 연속 굽힘(Flex 칠, scratchpad f11/bend — 재현 replica.Replica.fine의 kv·t): 굽는 칸(SB_BendLengthAt > 0)이면 함몰 창 용수철 하나로 거친 굽힘 해 쪽으로
 * 당긴다. 세기 Spring = κ = HOLD·h·A₁·ℓ_b³/(거친 칸)⁴(함몰 창 줄 대각 단위 — 창이 (1 + ΣShare)로 나눈다), 목표 Aim = W_c + L(SB_BendAimAt). 연한 회색은
 * κ가 작아 지금 함몰 창에 가깝고, 짙을수록 그린 깊이가 거친 해 W_c를 따른다. 판 칸과 같은 용수철 줄로 풀어 0 위로도 간다(굽은 겉이 솟는 쪽). Full만
 * (긴 몫이 도는 갱신).
 */
bool SB_BendSpringAt(int2 Texel, float2 Where, out float Spring, out float Aim)
{
	Spring = 0.0;
	Aim = 0.0;
	if (bContactLong == 0u)
	{
		return false;
	}
	float Bend = SB_BendLengthAt(Texel);
	if (!(Bend > 0.0))
	{
		return false;
	}
	float Layer = SB_LayerAt(Texel);
	float3 Window = SB_ResponseWindow(SB_VolumeAt(Texel), SB_SkinLengthAt(Texel) * ThicknessScale / Layer);
	float Cell = (float)CoarseBlock * TexelSize;
	Spring = min(SB_BEND_HOLD * Layer * Window.z * Bend * Bend * Bend / max(Cell * Cell * Cell * Cell, 1e-12), 1e20);
	Aim = SB_BendAimAt(Where, CarrierPosition.Load(int3(Texel, 0)).xyz);
	return true;
}

/**
 * 켜진 떠 있는 장식(조각 움직임 w = 3, 배치 3)의 칸(발자국 = 장식 밑 셔츠, 장식 칸)이면 그 칸의 판 깊이(cm, 자세 평면을 칸 쉼 자리의 판 좌표에서). 가는
 * 창은 발자국 칸을 손 핀 대신 판 한쪽 핀으로 붙든다(판이 셔츠를 누른 만큼만 — 들린 쪽은 당기지 않는다). 합치기는 판을 그리는 칸(SB_HardDrawsPlateCell —
 * 장식 칸)에만 이 깊이를 그리고, 발자국 셔츠는 살로 그린다(fb/orn 안 A).
 */
bool SB_HardFloatAt(int2 Texel, out float Plate)
{
	Plate = 0.0;
	uint Piece = SB_HardPieceAt(Texel);
	if (Piece == 0u || Piece > HardPieceCount)
	{
		return false;
	}
	float4 Motion = HardPieceMotion[Piece - 1u];
	if (Motion.w < 2.5)
	{
		return false;
	}
	Plate = Motion.x + dot(Motion.yz, SB_HardPlateXY(Piece - 1u, CarrierPosition.Load(int3(Texel, 0)).xyz));
	return true;
}

/**
 * 떠 있는 장식 밑 셔츠 칸의 가는 창 목표 = 판 + L − γ·h·A₁·max(p, 0) — SB_DentTarget과 같은 식에 붙들 깊이 대신 판, 압축 몫은 받침 압력(판이 누르는
 * 곳만). Basic(긴 몫 없음)은 판.
 */
float SB_HardFloatTarget(int2 Texel, float2 Where, float Plate)
{
	if (bContactLong == 0u)
	{
		return Plate;
	}
	float2 LP = SB_ContactLongAt(Where, CarrierPosition.Load(int3(Texel, 0)).xyz);
	float Layer = SB_LayerAt(Texel);
	float3 Window = SB_ResponseWindow(SB_VolumeAt(Texel), SB_SkinLengthAt(Texel) * ThicknessScale / Layer);
	return Plate + LP.x - Window.y * Layer * Window.z * max(LP.y, 0.0);
}

/** 켜진 붙은 강체 판(크기 비 — SB_HardKindOf, 조각 움직임 w = 2)의 칸인가 — 그 판이 든 함몰 창은 보통 칸의 0 바닥을 걷는다(배치 3 안 1). */
bool SB_HardBondedRigidAt(int2 Texel)
{
	uint Piece = SB_HardPieceAt(Texel);
	if (Piece == 0u || Piece > HardPieceCount)
	{
		return false;
	}
	float W = HardPieceMotion[Piece - 1u].w;
	return W > 1.5 && W < 2.5;
}
#endif


/**
 * 칸의 셀이 접히는 원천 상한 q_c = h·A₁·p_c(cm, ADR-0037 §2.2·§3.1 ⑤). 투영이 붙든 칸 원천을 이 값 위로 올리지 않는다 — 넘는 몫은 셀이 접혀 그 자리에서
 * 사라진다(둘레는 p_c만 받는다). 살 끝(v ≥ 1)은 아주 큰 값. Layer·SkinCm은 cm.
 */
float SB_CrushSource(float Volume, float Layer, float SkinCm)
{
	float Pc = SB_ResponseCrush(Volume);
	if (Pc > 1e20)
	{
		return 1e30;
	}
	float3 Window = SB_ResponseWindow(Volume, SkinCm / max(Layer, 1e-6));
	return Layer * Window.z * Pc;
}

// 부위 나누기 채널(순열 SB_SEPARATION, 상태 A와 같은 칸): 출처 무게 s_a, s_b, q로 나눠 푼 함몰 (C_a, C_b, C_q, w 쓰지 않음). 갱신을 넘어 남지 않는다 —
// 첫 패스는 상태 A의 함몰 e에서 출발하고 채널은 칸 제 무게 × e(이러면 보정이 0이라 U = e, 출발 값이 서로 맞는다), 첫 패스가 쓴 채널을 둘째 패스가 읽는다
// (갱신 안 임시 텍스처). 상태 A에는 늘 다른 부위에서 온 꼬리를 뺀 함몰 e가 있고, 둘째 패스는 첫 패스의 e에 보정을 되돌린 U = e + SB_SeparationCorrection에서
// 이어 푼다. 목록 밖 묶음은 채널 0이다(거친 해는 나누지 않는다). 채널을 갱신 사이에 남겨 이어 푼 판과 정지한 누름에서 0.04 mm 안이었다.
Texture2D<float4> SeparationStateIn;
RWTexture2D<float4> SeparationStateOut;

#if SB_SEPARATION
/** 칸이 든 묶음이 이번 갱신에 채널 순열로 푼 묶음인가(목록 비트 뒤쪽 절반 — TileListCS 부위 나누기, 검토 R03 E). */
bool SB_TileListedParts(int2 Texel)
{
	uint Tile = (uint)(Texel.y / SB_TILE) * TilesPerSide + (uint)(Texel.x / SB_TILE);
	uint Words = (TilesPerSide * TilesPerSide + 31u) / 32u;
	return ((DentTileBits[Words + (Tile >> 5u)] >> (Tile & 31u)) & 1u) != 0u;
}

/**
 * 저장된 칸 하나의 (U, C_a, C_b, C_q): 목록 밖 묶음은 거친 해와 채널 0. 채널 없는 순열로 푼 목록 묶음도 채널 0이다 — 그 묶음은 창 안 모든 칸의 출처
 * 무게가 0이라 채널 순열로 풀었어도 첫 패스 채널이 정확히 0이었다(그 묶음의 채널 텍스처에는 아무 값이나 남아 있어 읽지 않는다).
 */
float4 SB_SeparationLoadOne(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	float E = SB_LoadState(Texel);
	float3 Chan = SB_TileListedParts(Texel) ? SeparationStateIn.Load(int3(Texel, 0)).xyz : float3(0, 0, 0);
	return float4(E + SB_SeparationCorrection(SB_SeparationAt(Texel), Chan), Chan);
}

/** 칸 좌표의 (U, C)를 이웃 네 칸에서 bilinear로 — 칸마다 U를 되돌린 뒤 섞는다(SB_SampleState와 같은 탭). */
float4 SB_SeparationSample(float2 Target)
{
	int2 Base = (int2)floor(Target);
	float2 F = Target - (float2)Base;
	float4 Bottom = lerp(SB_SeparationLoadOne(Base), SB_SeparationLoadOne(Base + int2(1, 0)), F.x);
	float4 Top = lerp(SB_SeparationLoadOne(Base + int2(0, 1)), SB_SeparationLoadOne(Base + int2(1, 1)), F.x);
	return lerp(Bottom, Top, F.y);
}

float4 SB_SeparationFollowOnce(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	return SB_CarrierKindAt(Texel) == SB_KIND_GHOST ? SB_SeparationSample(CarrierRedirect.Load(int3(Texel, 0)).xy) : SB_SeparationLoadOne(Texel);
}

/** 칸의 (U, C): SB_CurrentState와 같은 따라가기(고스트 칸은 가리키는 점, 그 탭이 다시 고스트면 한 번 더). */
float4 SB_SeparationCurrent(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	if (SB_CarrierKindAt(Texel) != SB_KIND_GHOST)
	{
		return SB_SeparationLoadOne(Texel);
	}
	float2 Target = CarrierRedirect.Load(int3(Texel, 0)).xy;
	int2 Base = (int2)floor(Target);
	float2 F = Target - (float2)Base;
	float4 Bottom = lerp(SB_SeparationFollowOnce(Base), SB_SeparationFollowOnce(Base + int2(1, 0)), F.x);
	float4 Top = lerp(SB_SeparationFollowOnce(Base + int2(0, 1)), SB_SeparationFollowOnce(Base + int2(1, 1)), F.x);
	return lerp(Bottom, Top, F.y);
}
#endif

// 바닥 없는 함몰 창: 보통 칸의 0 바닥을 걷는다. 붙은 강체 판이 든 창(하드 서피스 순열, 배치 3 안 1 — 판의 들린 쪽 둘레 살이 판을 따라 오른다, 바닥이
// 있으면 한 칸 벽이 섰다, F16)과 잡기 둘레가 든 창(ADR-0041, 모든 순열 — 잡기가 있는 갱신만 표지를 비우는 장벽을 낸다).
// 부위 나누기 순열은 공유 메모리 32 KB가 다 차 있어 창 올리기 동안 비어 있는 자리 [7A]를 빌린다(채널은 이웃 몫을 다 셈한 뒤에 그 자리를 쓴다).
#if SB_SEPARATION
#define SB_DENT_NO_FLOOR SB_DentShared[7u * SB_DENT_AREA]
#else
groupshared uint SB_DentNoFloor;
#define SB_DENT_NO_FLOOR SB_DentNoFloor
#endif

/**
 * 목록의 스레드 묶음 하나 = 함몰을 푸는 가는 묶음 하나(창 32×32). 목록 밖 칸은 쓰지 않는다.
 * 부위 나누기(순열 SB_SEPARATION): 같은 창·같은 이웃 몫·같은 체비쇼프 반복에서 채널 셋을 같이 푼다. 채널은 붙든 칸에서 그 반복의 투영을 따른다 — 목표에
 * 붙은 칸은 출처 무게 × 목표, 원천 상한에 걸린 칸은 이웃 합 + 무게 × 상한, 나머지는 같은 걸음(투영이 없어 출처 무게에 선형이다). 끝에서 칸마다
 * e = U − 보정을 상태로 쓴다 — 합치기·노멀·접촉·받아들인 깊이·다음 갱신의 출발 값이 모두 이 한 함몰을 읽는다. 부위 나누기 장은 부위 몫이 창에 닿는
 * 묶음(목록 앞쪽)만 이 순열로 돌고 나머지는 순열 0으로 돈다(DentListPart, 검토 R03 E).
 */
[numthreads(SB_DENT_SIDE, SB_DENT_ROWS, 1)]
void DentCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	// 모든 스레드가 장벽을 지나야 하므로 일찍 돌아가지 않는다(D3D11 X3663). 목록 항목은 스레드 묶음마다 하나라 bListed는 묶음 전체가 같다.
	uint Entry = GroupId.y * SB_TILE_ROW + GroupId.x;
	// 부위 나누기 장은 목록을 두 몫으로 나눠 돈다(DentListPart): 앞쪽 부위 묶음은 채널 순열, 그 뒤는 채널 없는 순열.
	uint ListEnd = DentTileCount[0];
	BRANCH
	if (DentListPart != 0u)
	{
		uint PartsEnd = DentTileCount[SB_TILE_COUNT_PARTS];
		Entry += DentListPart == 2u ? PartsEnd : 0u;
		ListEnd = DentListPart == 1u ? PartsEnd : ListEnd;
	}
	bool bListed = Entry < ListEnd;
	uint Packed = bListed ? DentTiles[Entry] : 0u;
	uint2 Tile = uint2(Packed & 0xffffu, Packed >> 16u);
	int2 Origin = int2(Tile * SB_TILE) - SB_DENT_HALO;
	bool bSecond = DentPass == 2u;
#if SB_HARD_SURFACE
	if (LocalIndex == 0u)
	{
		SB_DENT_NO_FLOOR = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
#else
	// 잡기(ADR-0041)가 있는 갱신만 바닥 표지를 쓴다 — 균일 분기라 잡기가 없으면 장벽도 없다(지금 셰이더 그대로).
	BRANCH
	if (GrabCount != 0u)
	{
		if (LocalIndex == 0u)
		{
			SB_DENT_NO_FLOOR = 0u;
		}
		GroupMemoryBarrierWithGroupSync();
	}
#endif
	// 점성 칸의 제 몫 1 + c = e^{RΔt}(SB_DentSoftAt — 점성 칸이 있으면 HoldDecay는 0과 1 사이)
	float SoftKeep = HoldDecay > 0.0 ? 1.0 / HoldDecay : 1.0;
#if !SB_SEPARATION
	if (LocalIndex == 0u)
	{
		SB_DentBusy = 0u;
	}
#endif
	// 1. 창 올리기: 칸마다 출발 값(표면 칸은 저장된 값, 고스트 칸은 가리키는 점 — 목록 밖 묶음은 거친 해), 붙들 깊이, 산 칸인지(창 바깥 고리·목록 밖
	//    표면 칸·고스트 띠 바깥 고리·빈칸은 얼린다). 산 칸의 출발 값은 붙들 깊이 아래로 내려가지 않는다. 얼린 칸은 두 벌에 같은 값을 둔다.
	float Pin[SB_DENT_CELLS];       // 붙든 칸의 목표 T(cm)
	float Held[SB_DENT_CELLS];      // 붙들 깊이 P(cm, 0 = 붙들지 않은 칸)
	float Cap[SB_DENT_CELLS];       // 붙든 칸: 셀이 접히는 원천 상한 ÷ (1 + ΣShare)(cm, 살은 아주 큰 값). 점성 칸: 받아들인 깊이의 목표(바닥, 없으면 0)
	float Soft[SB_DENT_CELLS];      // 점성 칸의 이웃 몫 G = c(1 + ΣShare)(0 = 점성 칸 아님)
	float Cur[SB_DENT_CELLS];
	float Prev[SB_DENT_CELLS];
	bool bLive[SB_DENT_CELLS];
	bool bCarried[SB_DENT_CELLS];
#if SB_SEPARATION
	// 채널의 지금 값·지난 값은 레지스터에 두지 않고 공유 메모리 두 벌에서 읽는다(쓰기 벌은 쓰기 전까지 지난 반복 값이다) — 레지스터가 늘면 SM에 드는
	// 스레드 묶음이 줄어 함몰 풀이 전체가 느려진다. 출발 값은 창 올리기에서 공유 메모리에 넣을 때까지만 든다.
	float3 ChanInit[SB_DENT_CELLS];
	float3 ChanWeight[SB_DENT_CELLS];   // 칸의 출처 무게 (s_a, s_b, q) — 목록 밖·빈칸은 0
#endif
	// 펼칠 횟수를 적어 둔다(SB_DENT_CELLS = 4): 그냥 UNROLL이면 FXC가 부위 나누기·하드 서피스 Basic 순열에서 X3511로 펼치지 못한다.
#if SB_DENT_CELLS != 4
#error SB_DENT_CELLS changed: update [unroll(4)] below to match
#endif
	[unroll(4)]
	for (int Fill = 0; Fill < SB_DENT_CELLS; ++Fill)
	{
		int2 W = int2(LocalId.x, LocalId.y + SB_DENT_ROWS * Fill);
		int2 T = Origin + W;
		bool bInside = bListed && all(T >= int2(0, 0)) && all(T < int2(Resolution, Resolution));
		int2 C = clamp(T, int2(0, 0), int2(Resolution - 1, Resolution - 1));
		uint Code = bInside ? CarrierKind.Load(int3(C, 0)) : 0u;
		uint Kind = Code & 3u;
		bool bCarriedHere = Kind == SB_KIND_SURFACE || Kind == SB_KIND_GHOST;
		bool bTileListed = SB_TileListed(C);
#if SB_SEPARATION
		// 첫 패스: 상태 A의 e, 채널 = 칸 제 무게 × e. 둘째 패스: 첫 패스의 (e, 채널)에서 U를 되돌린다.
		float3 OwnWeight = SB_SeparationWeights(SB_SeparationAt(C));
		float4 Start;
		BRANCH
		if (bSecond)
		{
			Start = bCarriedHere ? SB_SeparationCurrent(C) : float4(0, 0, 0, 0);
		}
		else
		{
			float E = bCarriedHere ? SB_CurrentState(C) : 0.0;
			Start = float4(E, OwnWeight * E);
		}
		float Value = Start.x;
		ChanInit[Fill] = Start.yzw;
		ChanWeight[Fill] = bCarriedHere && bTileListed ? OwnWeight : float3(0, 0, 0);
#else
		float Value = bCarriedHere ? SB_CurrentState(C) : 0.0;
#endif
		float HeldHere = 0.0;
		float Target = 0.0;
		bool bSoftHere = false;
		float FloorHere = 0.0;
		if (bCarriedHere && bTileListed)
		{
			if (bSecond)
			{
				float2 Pins = DentPinsIn.Load(int3(C, 0));
				Target = Pins.x;
				HeldHere = Pins.y;
				// 첫 패스가 점성 칸을 −2 − 바닥으로 적었다(붙들 깊이 > 0 = 붙든 칸, 0 = 붙들지 않음)
				if (HeldHere <= -2.0)
				{
					bSoftHere = true;
					FloorHere = -2.0 - HeldHere;
					HeldHere = 0.0;
				}
			}
			else
			{
				HeldHere = SB_DentPinNow(C);
				float2 Where = Kind == SB_KIND_GHOST ? CarrierRedirect.Load(int3(C, 0)).xy : (float2)C;
				Target = HeldHere > 0.0 ? SB_DentTarget(C, Where, HeldHere) : 0.0;
				// 손이 떠난 기억 칸은 점성 칸(SB_DentSoftAt): 붙들지 않고 목표 쪽으로 당긴다. 받아들인 깊이(메모리폼)가 있으면 그 목표가 바닥이다.
				if (HeldHere > 0.0 && SB_DentSoftAt(C))
				{
					float AcceptedHere = AcceptedOut[C];
					FloorHere = AcceptedHere > 0.0 ? max(Target - HeldHere + AcceptedHere, 0.0) : 0.0;
					bSoftHere = true;
					HeldHere = 0.0;
				}
#if SB_HARD_SURFACE
				// 떠 있는 장식 밑 셔츠 칸(배치 3): 손 핀 대신 판 한쪽 핀 — 붙들 깊이 = 판 깊이(> 0인 곳), 목표 = 판 + L − γhA₁max(p, 0). 손은 장식
				// 윗면을 누르고 장식이 셔츠를 판이 내려간 만큼만 누른다 — 들린 쪽 셔츠는 당겨 올리지 않는다.
				float FloatPlate;
				if (SB_HardFloatAt(C, FloatPlate))
				{
					HeldHere = max(FloatPlate, 0.0);
					Target = HeldHere > 0.0 ? SB_HardFloatTarget(C, Where, FloatPlate) : 0.0;
					bSoftHere = false;
					FloorHere = 0.0;
				}
#endif
			}
		}
#if SB_HARD_SURFACE
		// 켜진 조각의 발자국 칸(휘는 판 κ 붙듦, SB_HardSpringAt): 판 목표 쪽 용수철(Soft < 0 표지 — 세기는 아래 2에서 (1 + ΣShare)로 나눈다, 목표는 Cap
		// 자리). 손이 누른 칸은 손 목표에 한쪽 핀 그대로이고 놓이면 용수철 줄이다. 점성 칸이 되지 않는다. Full만 이 순열을 쓴다 — Basic은 판 항이 없다.
		float PlateSpring = 0.0;
		float PlateAim = 0.0;
		bool bPlateHere = false;
		bool bBendHere = false;   // 연속 굽힘 칸(판 칸과 같은 용수철 줄 — 출발 값만 다르다)
		if (bCarriedHere && bTileListed)
		{
			float2 PlateWhere = Kind == SB_KIND_GHOST ? CarrierRedirect.Load(int3(C, 0)).xy : (float2)C;
			float PlateShare;
			float PlateLong;
			float PlateSqueeze;
			bPlateHere = SB_HardSpringAt(C, PlateWhere, PlateSpring, PlateAim, PlateShare, PlateLong, PlateSqueeze);
			bSoftHere = bSoftHere && !bPlateHere;
			if (bPlateHere && SB_HardBondedRigidAt(C))
			{
				InterlockedOr(SB_DENT_NO_FLOOR, 1u);
			}
			// 연속 굽힘: 판이 아닌 굽는 칸도 같은 용수철 줄로 거친 굽힘 해 쪽(SB_BendSpringAt — 목표는 Cap 자리, 세기는 −Soft). 손이 누른 칸은 손 목표에
			// 한쪽 핀 그대로이고 셀이 접히는 상한을 걸지 않는다(판 칸과 같다 — 짙은 칠 아래 폼이 접히는 몫은 거친 풀이의 힘 상한만 남는다). 손이 떠나
			// 돌아가는 기억 칸(점성 칸)은 점성 그대로다 — 자국 기억(Memory Time, 복원율)이 굽힘보다 먼저다(그동안 둘레 굽은 겉은 거친 해를 따른다).
			float BendSpring;
			float BendAim;
			if (!bPlateHere && !bSoftHere && SB_BendSpringAt(C, PlateWhere, BendSpring, BendAim))
			{
				bPlateHere = true;
				bBendHere = true;
				PlateSpring = BendSpring;
				PlateAim = BendAim;
			}
		}
#endif
		// 잡기(ADR-0041): 원 안 칸은 목표 P_g + L 쪽 용수철 줄(판 칸과 같은 꼴 — Soft < 0, 목표는 Cap 자리, 세기는 아래 2에서 줄 대각으로 나눈다). P_g = −D·n은
		// 당긴 벡터의 이 칸 노멀 몫(바깥이 음수), L은 긴 몫(Full) — 합치기가 U − L을 그려 원 안은 P_g 쪽이다. 콜라이더 누름은 접촉 패스가 이미 지웠고 기억·
		// 받아들인 깊이도 붙들지 않는다(당긴 자리는 자국으로 남지 않는다). 원 둘레가 든 창은 바닥을 걷는다 — 붙들지 않은 둘레 칸도 0 밑(바깥)으로 따라온다.
		float GrabSpring = 0.0;
		float GrabAim = 0.0;
		BRANCH
		if (GrabCount != 0u && bCarriedHere && bTileListed)
		{
			float3 GrabRest = CarrierPosition.Load(int3(C, 0)).xyz;
			float4 GrabCarrier = CarrierNormal.Load(int3(C, 0));
			float3 GrabNormal = GrabCarrier.xyz;
			float3 GrabPull;
			GrabSpring = SB_GrabGrip(GrabRest, GrabNormal * rsqrt(max(dot(GrabNormal, GrabNormal), 1e-12)), GrabPull);
			if (GrabSpring > 0.0)
			{
				float2 GrabWhere = Kind == SB_KIND_GHOST ? CarrierRedirect.Load(int3(C, 0)).xy : (float2)C;
				GrabAim = SB_GrabAimLimit(-dot(GrabPull, ContactIn.Load(int3(C, 0)).xyz), GrabCarrier.w * ThicknessScale) + (bContactLong != 0u ? SB_ContactLongAt(GrabWhere, GrabRest).x : 0.0);
				HeldHere = 0.0;
				Target = 0.0;
				bSoftHere = false;
				FloorHere = 0.0;
			}
			if (GrabSpring > 0.0 || SB_GrabNear(GrabRest))
			{
				InterlockedOr(SB_DENT_NO_FLOOR, 1u);
			}
		}
		bool bInner = all(W > int2(0, 0)) && all(W < int2(SB_DENT_SIDE - 1, SB_DENT_SIDE - 1));
		bool bLiveHere = bInner && (Kind == SB_KIND_SURFACE ? bTileListed : (Kind == SB_KIND_GHOST && (Code & SB_KIND_OUTER_GHOST) == 0u));
		Pin[Fill] = Target;
		Held[Fill] = HeldHere;
		Cap[Fill] = bSoftHere ? FloorHere : 1e30;
		Soft[Fill] = bSoftHere ? 1.0 : 0.0;   // 이웃 몫은 아래 2에서
#if SB_HARD_SURFACE
		Cap[Fill] = bPlateHere ? PlateAim : Cap[Fill];
		Soft[Fill] = bPlateHere ? -max(PlateSpring, 1e-12) : Soft[Fill];
#endif
		// 잡기 용수철은 판·굽힘보다 앞선다(손이 그 자리를 붙들었다).
		Cap[Fill] = GrabSpring > 0.0 ? GrabAim : Cap[Fill];
		Soft[Fill] = GrabSpring > 0.0 ? -GrabSpring : Soft[Fill];
		bLive[Fill] = bLiveHere;
		bCarried[Fill] = bCarriedHere;
		Cur[Fill] = bLiveHere && HeldHere > 0.0 ? max(Value, Target) : Value;
#if SB_HARD_SURFACE
		Cur[Fill] = bLiveHere && bPlateHere && !bBendHere && HeldHere <= 0.0 && GrabSpring <= 0.0 ? PlateAim : Cur[Fill];
#endif
		Prev[Fill] = Cur[Fill];
		uint Slot = (uint)(W.y * SB_DENT_SIDE + W.x);
		SB_DentValueSet(Slot, Cur[Fill]);
		SB_DentValueSet(SB_DENT_AREA + Slot, Cur[Fill]);
		SB_DentPositionSet(Slot, bCarriedHere ? SB_LoadPosition(C) : float4(0, 0, 0, 0));
		// 재료 폭 비: 칠도 겉껍질도 없으면 메시 전체에서 같아(값 하나) 창 칸마다 읽지 않는다 — 고른 재료는 지금 비용 그대로.
		float Ratio = 0.0;
		if (bCarriedHere)
		{
			BRANCH
			if (bCarrierVolume == 0u && SkinLength <= 0.0)
			{
				Ratio = SB_ResponseWindow(SB_VolumeShifted(saturate(VolumeValue)), 0.0).x;
			}
			else
			{
				float LayerHere = SB_LayerAt(C);
				Ratio = max(SB_ResponseWindow(SB_VolumeAt(C), SB_SkinLengthAt(C) * ThicknessScale / LayerHere).x, 1e-3 / LayerHere);
			}
		}
		SB_DentMaterialSet(Slot, Ratio * Ratio);
	}
	GroupMemoryBarrierWithGroupSync();
#if SB_HARD_SURFACE
	bool bNoFloor = SB_DENT_NO_FLOOR != 0u;
#else
	bool bNoFloor = GrabCount != 0u && SB_DENT_NO_FLOOR != 0u;   // 잡기가 없는 갱신은 표지를 비우지 않았다 — 읽지 않는다
#endif
	// 2. 산 칸의 이웃 몫(SB_RowScale): 차트 안·고스트 이웃 모두 지금 자리 사이 거리로(SB_NeighborSpacing). 빈 이웃은 값 0으로 당긴다.
	float4 Weight[SB_DENT_CELLS];
	UNROLL
	for (int Link = 0; Link < SB_DENT_CELLS; ++Link)
	{
		Weight[Link] = float4(0, 0, 0, 0);
		if (bLive[Link])
		{
			int2 W = int2(LocalId.x, LocalId.y + SB_DENT_ROWS * Link);
			uint Slot = (uint)(W.y * SB_DENT_SIDE + W.x);
			float4 There[4];
			There[0] = SB_DentPositionGet(Slot + 1u);
			There[1] = SB_DentPositionGet(Slot - 1u);
			There[2] = SB_DentPositionGet(Slot + SB_DENT_SIDE);
			There[3] = SB_DentPositionGet(Slot - SB_DENT_SIDE);
			float4 Spacing = SB_NeighborSpacing(SB_DentPositionGet(Slot).xyz, There, TexelSize);
			int2 T = Origin + W;
			float Layer = SB_LayerAt(T);
			float Volume = SB_VolumeAt(T);
			float SkinCm = SB_SkinLengthAt(T) * ThicknessScale;
			// 변마다 폭 ℓ_e = h·√(두 칸 재료 비² 평균)(SB_EdgeRatio — 칠 경계에서 발산꼴). 두께는 제 칸 값.
			float4 Reach = Layer * sqrt(SB_EdgeRatio(SB_DentMaterialGet(Slot), float4(SB_DentMaterialGet(Slot + 1u), SB_DentMaterialGet(Slot - 1u),
				SB_DentMaterialGet(Slot + SB_DENT_SIDE), SB_DentMaterialGet(Slot - SB_DENT_SIDE))));
			float4 Share = Reach * Reach * SB_RowScale(Spacing) / Spacing;
			float SumShare = dot(Share, float4(1, 1, 1, 1));
			Weight[Link] = Share / (1.0 + SumShare);
#if SB_HARD_SURFACE
			// 판 칸(Soft < 0): Cap은 용수철 목표 그대로, 세기 κ_v를 줄 대각(1 + ΣShare)로 나눈다
			bool bPlateLink = Soft[Link] < 0.0;
			Cap[Link] = Held[Link] > 0.0 && !bPlateLink ? SB_CrushSource(Volume, Layer, SkinCm) / (1.0 + SumShare) : Cap[Link];
			Soft[Link] = Soft[Link] > 0.0 ? (SoftKeep - 1.0) * (1.0 + SumShare) : (bPlateLink ? Soft[Link] / (1.0 + SumShare) : 0.0);
#else
			// 잡기 용수철 칸(Soft < 0, ADR-0041): Cap은 용수철 목표 그대로, 세기 κ를 줄 대각(1 + ΣShare)로 나눈다(판 칸과 같은 꼴).
			Cap[Link] = Held[Link] > 0.0 ? SB_CrushSource(Volume, Layer, SkinCm) / (1.0 + SumShare) : Cap[Link];
			Soft[Link] = Soft[Link] > 0.0 ? (SoftKeep - 1.0) * (1.0 + SumShare) : (Soft[Link] < 0.0 ? Soft[Link] / (1.0 + SumShare) : 0.0);
#endif
		}
	}
#if SB_SEPARATION
	// 위치·재료 자리를 채널 값 두 벌로 다시 쓴다(모든 스레드가 이웃 몫을 다 셈한 뒤). 얼린 칸도 두 벌에 같은 값.
	GroupMemoryBarrierWithGroupSync();
	UNROLL
	for (int Seed = 0; Seed < SB_DENT_CELLS; ++Seed)
	{
		uint Slot = (LocalId.y + SB_DENT_ROWS * (uint)Seed) * SB_DENT_SIDE + LocalId.x;
		UNROLL
		for (uint k = 0u; k < 3u; ++k)
		{
			SB_DentChannelSet(k, 0u, Slot, ChanInit[Seed][k]);
			SB_DentChannelSet(k, SB_DENT_AREA, Slot, ChanInit[Seed][k]);
		}
	}
	GroupMemoryBarrierWithGroupSync();
#endif
	// 3. 체비쇼프(패스마다 처음부터 — 첫 반복은 운동량 0). 공유 메모리 두 벌을 번갈아 읽고 써서 장벽은 반복마다 하나다.
	float Sigma = DentTheta / DentDelta;
	float Rho = 1.0 / Sigma;
	LOOP
	for (uint Iteration = 0u; Iteration < SB_DENT_ITERATIONS; ++Iteration)
	{
		float Momentum = 0.0;
		float Step = 1.0 / DentTheta;
		if (Iteration > 0u)
		{
			float RhoNext = 1.0 / (2.0 * Sigma - Rho);
			Momentum = RhoNext * Rho;
			Step = 2.0 * RhoNext / DentDelta;
			Rho = RhoNext;
		}
		uint Read = (Iteration & 1u) * SB_DENT_AREA;
		uint Write = SB_DENT_AREA - Read;
		UNROLL
		for (int Sweep = 0; Sweep < SB_DENT_CELLS; ++Sweep)
		{
			if (bLive[Sweep])
			{
				uint Slot = (LocalId.y + SB_DENT_ROWS * (uint)Sweep) * SB_DENT_SIDE + LocalId.x;
				float4 Around = float4(SB_DentValueGet(Read + Slot + 1u), SB_DentValueGet(Read + Slot - 1u),
					SB_DentValueGet(Read + Slot + SB_DENT_SIDE), SB_DentValueGet(Read + Slot - SB_DENT_SIDE));
				float Self = Cur[Sweep];
				float Jacobi = dot(Weight[Sweep], Around);
				// 점성 칸은 제 목표 몫을 더한 줄(SB_DentSoftAt)을 같은 체비쇼프 걸음으로 푼다 — 줄의 값은 목표와 이웃의 무게 평균이라 범위 안이다.
#if SB_HARD_SURFACE
				// 판 칸(Soft < 0)은 판 목표(Cap 자리) 쪽 용수철 k̂ = −Soft를 더한 줄
				float Aim = Soft[Sweep] > 0.0 ? (Pin[Sweep] * SoftKeep + Soft[Sweep] * Jacobi) / (1.0 + Soft[Sweep])
					: (Soft[Sweep] < 0.0 ? (Jacobi - Soft[Sweep] * Cap[Sweep]) / (1.0 - Soft[Sweep]) : Jacobi);
#else
				// 잡기 용수철 칸(Soft < 0)은 목표(Cap 자리) 쪽 용수철 k̂ = −Soft를 더한 줄 — 잡기가 없으면 Soft < 0인 칸이 없다
				float Aim = Soft[Sweep] > 0.0 ? (Pin[Sweep] * SoftKeep + Soft[Sweep] * Jacobi) / (1.0 + Soft[Sweep])
					: (Soft[Sweep] < 0.0 ? (Jacobi - Soft[Sweep] * Cap[Sweep]) / (1.0 - Soft[Sweep]) : Jacobi);
#endif
				float Next = Self + Momentum * (Self - Prev[Sweep]) + Step * (Aim - Self);
				// 붙든 칸은 목표 T 아래로 돌아가지 않되, 원천(그 칸이 둘레에 내는 몫 (1 + ΣShare)(U − J))이 셀이 접히는 상한을 넘지 않는다 — 상한은 J에 건다
				// (체비쇼프 갱신값에 걸면 고정점이 걸음만큼 어긋난다, 일지 275). 붙들 깊이가 없는 칸은 0 밑으로 가지 않고 1 µm 아래는 0(묶음이 목록에서 빠진다).
				float U;
#if SB_SEPARATION
				uint HeldCode = 0u;   // 1 = 목표에 붙음, 2 = 원천 상한에 걸림, 3 = 점성 칸이 바닥에 걸림, 0 = 그 밖(목표보다 깊이 뜬 칸은 둘레처럼)
#endif
				if (Held[Sweep] > 0.0)
				{
					float Low = max(Pin[Sweep], Next);
#if SB_HARD_SURFACE
					float High = Soft[Sweep] < 0.0 ? 1e30 : Jacobi + Cap[Sweep];   // 판 칸의 Cap은 용수철 목표다(셀 접힘 상한 없음)
#else
					float High = Jacobi + Cap[Sweep];
#endif
					U = min(Low, High);
#if SB_SEPARATION
					HeldCode = High < Low ? 2u : (Next <= Pin[Sweep] ? 1u : 0u);
#endif
				}
				else
				{
					// 붙들지 않은 칸은 0 밑으로, 점성 칸은 바닥(받아들인 깊이의 목표) 밑으로 가지 않는다. 판 칸은 판의 들린 쪽이라 0 위로도 간다.
#if SB_HARD_SURFACE
					float Floor = Soft[Sweep] > 0.0 ? Cap[Sweep] : (Soft[Sweep] < 0.0 || bNoFloor ? -1e30 : 0.0);
#else
					// 잡기 용수철 칸과 잡기 둘레가 든 창(ADR-0041)은 0 밑(바깥)으로도 간다
					float Floor = Soft[Sweep] > 0.0 ? Cap[Sweep] : (Soft[Sweep] < 0.0 || bNoFloor ? -1e30 : 0.0);
#endif
					U = max(Next, Floor);
#if SB_SEPARATION
#if SB_HARD_SURFACE
					HeldCode = Soft[Sweep] > 0.0 && Next < Floor ? 3u : (Soft[Sweep] < 0.0 ? 1u : 0u);
#else
					HeldCode = Soft[Sweep] > 0.0 && Next < Floor ? 3u : 0u;
#endif
#endif
				}
#if SB_HARD_SURFACE
				// 판·굽는 칸(Soft < 0)은 0 위로도 가 자르지 않되, 값과 목표(Cap 자리)가 둘 다 1 µm 안이면 0 — 손을 뗀 뒤 굽은 둘레가 0으로 끝나 묶음이 목록에서 빠진다.
				U = Held[Sweep] <= 0.0 && (Soft[Sweep] >= 0.0 ? (bNoFloor ? abs(U) : U) : max(abs(U), abs(Cap[Sweep]))) < SB_REST_EPSILON ? 0.0 : U;
#else
				// 바닥 없는 창·잡기 용수철 칸은 하드 서피스 판과 같은 자르기(|U|, 용수철 칸은 목표까지 1 µm 안) — 잡기가 없으면 지금 식 그대로
				U = Held[Sweep] <= 0.0 && (Soft[Sweep] >= 0.0 ? (bNoFloor ? abs(U) : U) : max(abs(U), abs(Cap[Sweep]))) < SB_REST_EPSILON ? 0.0 : U;
#endif
				Prev[Sweep] = Self;
				Cur[Sweep] = U;
				SB_DentValueSet(Write + Slot, U);
#if SB_SEPARATION
				// 채널 셋: 같은 걸음, 붙든 칸은 이번 반복의 투영 그대로(출처 무게만큼)
				float3 JacobiC;
				float3 SelfC;
				float3 PrevC;
				UNROLL
				for (uint k = 0u; k < 3u; ++k)
				{
					float4 AroundC = float4(SB_DentChannelGet(k, Read, Slot + 1u), SB_DentChannelGet(k, Read, Slot - 1u),
						SB_DentChannelGet(k, Read, Slot + SB_DENT_SIDE), SB_DentChannelGet(k, Read, Slot - SB_DENT_SIDE));
					JacobiC[k] = dot(Weight[Sweep], AroundC);
					SelfC[k] = SB_DentChannelGet(k, Read, Slot);
					PrevC[k] = SB_DentChannelGet(k, Write, Slot);   // 아직 쓰지 않은 쓰기 벌 = 지난 반복 값(이 칸은 이 스레드만 쓴다)
				}
				// 용수철 칸(Soft < 0 — 잡기, ADR-0041)은 같은 줄을 출처 무게만큼: 목표 몫이 제 칸 무게로 든다
				float3 AimC = Soft[Sweep] > 0.0 ? (ChanWeight[Sweep] * (Pin[Sweep] * SoftKeep) + Soft[Sweep] * JacobiC) / (1.0 + Soft[Sweep])
					: (Soft[Sweep] < 0.0 ? (JacobiC - Soft[Sweep] * ChanWeight[Sweep] * Cap[Sweep]) / (1.0 - Soft[Sweep]) : JacobiC);
				float3 NextC = SelfC + Momentum * (SelfC - PrevC) + Step * (AimC - SelfC);
				float3 UC = HeldCode == 1u ? ChanWeight[Sweep] * Pin[Sweep] : (HeldCode == 2u ? JacobiC + ChanWeight[Sweep] * Cap[Sweep]
					: (HeldCode == 3u ? ChanWeight[Sweep] * Cap[Sweep] : NextC));
				UNROLL
				for (uint k2 = 0u; k2 < 3u; ++k2)
				{
					SB_DentChannelSet(k2, Write, Slot, UC[k2]);
				}
#endif
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
#if SB_SEPARATION
	// 남은 표지는 반복이 끝난 공유 메모리 첫 자리에 모은다(반복의 마지막 장벽 뒤라 아무도 값 자리를 읽지 않는다).
	if (LocalIndex == 0u)
	{
		SB_DENT_BUSY = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
#endif
	// 4. 제 묶음 칸(창 줄·열 8~23)을 쓴다. 첫 패스: 결과와 붙들 깊이. 둘째 패스: 결과, 기억, 받아들인 깊이(제 칸만 읽고 쓴다), 남은 것.
	UNROLL
	for (int Own = 1; Own <= 2; ++Own)
	{
		int2 W = int2(LocalId.x, LocalId.y + SB_DENT_ROWS * Own);
		int2 T = Origin + W;
		if (bListed && W.x >= SB_DENT_HALO && W.x < SB_DENT_HALO + SB_TILE && all(T < int2(Resolution, Resolution)))
		{
#if SB_SEPARATION
			// 상태 = 다른 부위에서 온 꼬리를 뺀 함몰(붙든 칸은 채널이 제 무게대로라 보정 0 — 손 깊이 그대로). 채널 끝 값은 마지막 반복이 쓴 벌에 있다.
			uint OwnSlot = (uint)(W.y * SB_DENT_SIDE + W.x);
			uint Final = SB_DENT_AREA - ((SB_DENT_ITERATIONS - 1u) & 1u) * SB_DENT_AREA;
			float3 Chan = float3(SB_DentChannelGet(0u, Final, OwnSlot), SB_DentChannelGet(1u, Final, OwnSlot), SB_DentChannelGet(2u, Final, OwnSlot));
			float Dent = Cur[Own] - SB_SeparationCorrection(SB_SeparationAt(T), Chan);
			if (!bSecond)
			{
				SeparationStateOut[T] = float4(Chan, 0.0);
			}
#else
			float Dent = Cur[Own];
#endif
			StateOut[T] = Dent;
			if (!bSecond)
			{
				DentPins[T] = float2(Pin[Own], Soft[Own] > 0.0 ? -2.0 - Cap[Own] : Held[Own]);
			}
			else if (bCarried[Own])
			{
				float HeldNow = SB_HoldNow(max(ContactIn.Load(int3(T, 0)).w, 0.0), HoldState[T]);
				// 점성 칸의 새 기억 = 풀린 깊이 − 긴 몫(목표는 이번 기억 + 긴 몫이었다, 첫 패스와 같은 HoldNow). 다음 갱신은 이 자리에서 다시 한 걸음 푼다.
				// 기억은 누를 때만 깊어진다 — 줄인 지난 기억(HoldNow)을 넘지 않는다. 둘레가 더 깊어 끌려간 칸도 R로는 줄고, 한 갱신 튄 풀이 값이 기억에
				// 남지 않는다(일지 287 — 넘게 두면 흔들어 누를 때 튄 값이 R로 천천히 줄며 남았다).
				if (Soft[Own] > 0.0 && bLive[Own])
				{
					HeldNow = clamp(Cur[Own] - (Pin[Own] - HeldNow), 0.0, HeldNow);
					HeldNow = HeldNow < SB_REST_EPSILON ? 0.0 : HeldNow;
				}
				HoldState[T] = HeldNow;
				float Old = AcceptedOut[T];
				// 받아들인 깊이는 보이는 깊이 U − L을 따른다(붙든 칸은 목표에 L을 더해 두었으므로 U를 따르면 갱신마다 L만큼 깊어지고, 둘레 칸은 보이는
				// 모양보다 좁게 남는다). 받아들이기가 꺼져 있으면(AcceptBlend 0 — 기본) 읽지 않는다.
				float Shown = Dent;
				BRANCH
				if (bContactLong != 0u && AcceptBlend > 0.0)
				{
					float2 Where = (CarrierKind.Load(int3(T, 0)) & 3u) == SB_KIND_GHOST ? CarrierRedirect.Load(int3(T, 0)).xy : (float2)T;
					Shown -= SB_ContactLongAt(Where, CarrierPosition.Load(int3(T, 0)).xyz).x;
				}
				// 붙든 칸의 보이는 깊이는 max(U − L, P)다(합치기와 같다 — 손이 γ·접힌 몫을 뺀 목표보다 깊다). 이음(일지 323, SmoothSqueeze)이 손 둘레로 이어
				// 그리는 몫은 넣지 않는다 — 넣으면 둘레 칸의 받아들인 깊이가 다음 갱신의 붙들 깊이가 되어 갱신마다 그 몫만큼 번진다(일지 287과 같은 꼴).
				Shown = Held[Own] > 0.0 ? max(Shown, Held[Own]) : Shown;
				float Accepted = (Old + AcceptBlend * max(Shown - Old, 0.0)) * AcceptReturn;
#if SB_HARD_SURFACE
				// 붙은 판 칸은 받아들인 깊이를 쌓지 않는다 — 쌓으면 다음 갱신에 발자국 전체가 손 칸(붙들 깊이)이 된다(조각 자국은 남지 않는다).
				Accepted = Held[Own] < 0.0 ? Old * AcceptReturn : Accepted;
#endif
				Accepted = Accepted < SB_REST_EPSILON ? 0.0 : Accepted;
				AcceptedOut[T] = Accepted;
				// 함몰이 남았는지는 나누기 전 U로 본다 — 다른 부위 꼬리만 남은 묶음도 목록에 남아야 그 꼬리를 계속 빼 준다.
				uint Busy = (HeldNow > 0.0 || Accepted > 0.0 ? SB_RESIDUAL_HOLD : 0u) | (Cur[Own] != 0.0 ? SB_RESIDUAL_DENT : 0u);
				if (Busy != 0u)
				{
					InterlockedOr(SB_DENT_BUSY, Busy);
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (bSecond && bListed && LocalIndex == 0u)
	{
		uint Busy = SB_DENT_BUSY;
		TileResidualOut[Tile.y * TilesPerSide + Tile.x] = Busy;
		if (Busy != 0u)
		{
			InterlockedOr(TileAnyResidualOut[0], 1u);
		}
	}
}

/**
 * 함몰 거친 해(일지 199·200): 첫 패스 앞, 함몰을 푸는 가는 묶음마다 상태 A를 이번 출발 값으로 바꾼다 — 지난 갱신에 푼 묶음(남은 것이 있다)은
 * A + 쌍선형(고침 U − Ū), 목록에 새로 든 묶음(남은 것 0 — A가 0)은 거친 해 쌍선형(공이 구르며 새로 든 묶음이 0에서 출발하지 않게). 칸마다 한 번
 * 셈해, 창이 칸을 읽을 때마다(겹친 창 넷, 고스트가 따라가는 점 넷) 되풀이하던 쌍선형 읽기를 없앤다. 묶음의 모든 칸을 쓴다 — 옮기기가 갱신 끝에
 * 같은 칸을 모두 다시 쓴다. 남은 것은 묶음째라 갈래가 묶음 안에서 같다.
 */
[numthreads(SB_TILE, SB_TILE, 1)]
void DentCoarseAddCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID)
{
	uint Entry = GroupId.y * SB_TILE_ROW + GroupId.x;
	if (Entry >= DentTileCount[0])
	{
		return;
	}
	uint Packed = DentTiles[Entry];
	uint2 Tile = uint2(Packed & 0xffffu, Packed >> 16u);
	int2 T = int2(Tile * SB_TILE + LocalId.xy);
	if (any(T >= int2(Resolution, Resolution)))
	{
		return;
	}
	BRANCH
	if (TileResidualOut[Tile.y * TilesPerSide + Tile.x] != 0u)
	{
		DentStateInOut[T] = DentStateInOut[T] + SB_CoarseBilinear(DentCoarseFix, T);
	}
	else
	{
		DentStateInOut[T] = SB_CoarseBilinear(DentCoarse, T);
	}
}

/**
 * 옮기기(ADR-0032): 목록 앞은 함몰을 푸는 가는 묶음 — 함몰 둘째 패스 결과를 상태 A(다음 갱신의 출발 값이자 뒤 패스들이 읽는 곳)로 옮긴다. 고스트 칸은
 * 가리키는 점의 값으로 옮겨 옆 차트와 같은 값이 된다(합치기·노멀이 차트 끝에서 이웃 칸을 그대로 읽는다). 그 뒤는 붙든 자리 없이 함몰만 남은 채 목록에서
 * 빠지는 묶음 — 0으로 비우고 남은 것도 0으로 적는다(그 묶음의 정적 해는 0이다). 목록 밖 칸은 건드리지 않는다(ADR-0022). 끌림은 제 패스가 쓴다(ADR-0033).
 */
[numthreads(SB_TILE, SB_TILE, 1)]
void DentCopyCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	uint Entry = GroupId.y * SB_TILE_ROW + GroupId.x;
	uint SolveCount = DentTileCount[0];
	if (Entry >= SolveCount + DentTileCount[1])
	{
		return;
	}
	uint Packed = DentTiles[Entry];
	uint2 Tile = uint2(Packed & 0xffffu, Packed >> 16u);
	int2 C = int2(Tile * SB_TILE + LocalId.xy);
	if (any(C >= int2(Resolution, Resolution)))
	{
		return;
	}
	bool bZero = Entry >= SolveCount;
	StateOut[C] = bZero ? 0.0 : SB_CurrentState(C);

	if (bZero && LocalIndex == 0u)
	{
		TileResidualOut[Tile.y * TilesPerSide + Tile.x] = 0u;
	}
}

// --- GrabResolveCS: 잡기 줄 풀기 (ADR-0041, 접촉 패스 뒤 스레드 묶음 하나 — 줄마다 스레드 하나) ---
RWStructuredBuffer<float4> GrabStateOut;   // GrabState와 같은 버퍼(이 패스만 쓴다)
Buffer<uint> GrabPicks;                    // 접촉 패스의 ActivityOut(잡기 고르기 낱말은 GrabPickBase부터 — 뒤집은 열쇠, 0 = 찾는 거리 안 피부 없음)
Texture2D<float> GrabFlexPaint;            // Flex 칠(G16). bGrabFlexPaint가 0이면 묶인 자리 텍스처라 읽지 않는다
uint bGrabFlexPaint;
float GrabFlexOffset;                      // 컴포넌트 Flex Offset
#define SB_GRAB_PLATE_FLEX 0.05            // 칠 + Flex Offset이 이 아래인 칸은 판(Flex 검정) — 잡을 수 없다(판 들기는 다음 판: 붙은 판 줄로)

/** 잡은 칸의 덩어리 국소 모양이 옮긴 양(프레임 cm — 합치기가 변위로 그리는 몫). 피부는 이 자리까지 있다(PositionOut에는 출렁임 모양 몫까지만). */
float3 SB_GrabRegionAt(int2 Texel, float3 RestP)
{
	BRANCH
	if (bLumpRegion == 0u)
	{
		return float3(0, 0, 0);
	}
	float2 Where = SB_KindAt(Texel) == SB_KIND_GHOST ? CarrierRedirect.Load(int3(Texel, 0)).xy : (float2)Texel;
	return SB_RegionOffsetAt(Where, RestP, CarrierLump.Load(int3(Texel, 0)));
}

/**
 * 잡은 칸의 접선 두 축: 정지 노멀에서 만든 축(SB_LumpBasis)을 스키닝으로 옮겨 지금 노멀 N(프레임)에 맞춘다 — 칸이 돌면 같이 돈다(들어온 면 표지와 같은 꼴).
 * 시작 간격과 놓는 중 남은 당김이 이 축으로 남아 몸이 움직여도 잡은 자리를 따라 돈다.
 */
void SB_GrabBasis(int2 Texel, float3 RestN, float3 N, out float3 T1, out float3 T2, out float3 Bare)
{
	float3 RestT1;
	float3 RestT2;
	SB_LumpBasis(RestN, RestT1, RestT2);
	float4 Row0 = float4(1, 0, 0, 0);
	float4 Row1 = float4(0, 1, 0, 0);
	float4 Row2 = float4(0, 0, 1, 0);
	if (bSkinned != 0u)
	{
		SB_SkinRows(Texel, Bones, Row0, Row1, Row2);
	}
	// 덩어리 이동 없는 피부 자리(스킨만) — 덩어리 잡기 용수철의 목표가 덩어리 몫을 빼기 전 전체 당김이다.
	float4 P1 = float4(CarrierPosition.Load(int3(Texel, 0)).xyz, 1.0);
	Bare = float3(dot(P1, Row0), dot(P1, Row1), dot(P1, Row2)) * ComponentScale;
	float3 T = SB_SkinDelta(RestT1, Row0, Row1, Row2);
	T -= N * dot(T, N);
	if (dot(T, T) < 1e-8)
	{
		SB_LumpBasis(N, T, RestT2);
	}
	T1 = normalize(T);
	T2 = cross(N, T1);
}

/**
 * 줄마다(GrabState — 배치는 위 "잡기" 머리 주석):
 *  - 새 잡기(붙드는 줄인데 번호가 다르다): 접촉 패스가 고른 칸의 정지 자리·정지 노멀·살 두께와 시작 손–피부 간격(잡은 점의 노멀·접선 성분)을 적는다. 다음
 *    갱신부터 붙든다 — 피부는 잡은 뒤 손이 움직인 만큼만 따라간다(툭 튀지 않는다). 찾는 거리 안에 피부가 없거나 판(Flex 검정)이면 못 잡음.
 *  - 붙듦: 잡은 자리를 잡은 뒤 끈 양(끄는 자리 − 덩어리 이동 없는 잡은 점 − 기억한 간격 − 잡을 때 덩어리 이동)이 한도 min(h, R)까지 그대로, 그 1.3배까지
 *    서서히(SB_PressLimit — Max Contact Depth와 같은 곡선 — 손이 한도의 1.6배에 오면 1.3배에 닿는다). 손이 그보다 멀리 가도 놓지 않고 1.3배에 머문다(사용자
 *    11-05 — 손과 피부 사이 틈은 Grabber 쪽이 Get Stretch로 본다). 피부와 덩어리가 함께 이 곡선에 머문다(일지 350): 덩어리 용수철은 한도 안 당김 쪽으로,
 *    표면 장은 그 나머지를 그린다. Basic은 표면 장의 옆 몫을 그리지 않는다.
 *  - 놓음(Release·대상 사라짐·다른 잡기가 줄을 가져감): 그 순간의 당김을 잡은 점의 노멀·접선 성분으로 둔 채 갱신마다 e^{−RΔt}(누른 자리 기억과 같은 감쇠 —
 *    뗀 누름이 돌아오는 것과 같은 빠르기), 1 µm 아래에서 줄을 비운다.
 */
[numthreads(SB_GRAB_ROWS, 1, 1)]
void GrabResolveCS(uint Row : SV_GroupIndex)
{
	if (Row >= GrabCount)
	{
		return;
	}
	uint Base = Row * SB_GRAB_STATE_ROWS;
	float4 S[SB_GRAB_STATE_ROWS];
	UNROLL
	for (uint k = 0u; k < SB_GRAB_STATE_ROWS; ++k)
	{
		S[k] = GrabStateOut[Base + k];
	}
	float4 In0 = GrabInput[Row * SB_GRAB_INPUT_ROWS + 0u];
	float4 In1 = GrabInput[Row * SB_GRAB_INPUT_ROWS + 1u];
	float4 In2 = GrabInput[Row * SB_GRAB_INPUT_ROWS + 2u];
	uint SerialIn = asuint(In1.w);
	bool bHold = (asuint(In2.w) & SB_GRAB_HOLD) != 0u;
	uint Status = SB_GrabStatusOf(S[0]);
	uint Reason = (asuint(S[0].x) >> 8u) & 0xffu;
	uint SerialHere = asuint(S[0].z);
	float MeanScale = (abs(ComponentScale.x) + abs(ComponentScale.y) + abs(ComponentScale.z)) / 3.0;
	if ((asuint(In2.w) & SB_GRAB_CLEAR) != 0u)
	{
		UNROLL
		for (uint k4 = 0u; k4 < SB_GRAB_STATE_ROWS; ++k4)
		{
			S[k4] = float4(0, 0, 0, 0);
		}
		Status = SB_GRAB_EMPTY;
		Reason = 0u;
	}
	else if (bHold && SerialIn != SerialHere)
	{
		uint Key = GrabPicks[GrabPickBase + Row];
		uint2 Texel = uint2(0, 0);
		bool bFound = Key != 0u;
		if (bFound)
		{
			uint Index = (~Key) & 0x3fffffu;
			Texel = uint2(Index % Resolution, Index / Resolution);
			if (bGrabFlexPaint != 0u && saturate(GrabFlexPaint.Load(int3(Texel, 0)) + GrabFlexOffset) < SB_GRAB_PLATE_FLEX)
			{
				bFound = false;
			}
		}
		UNROLL
		for (uint k2 = 1u; k2 < SB_GRAB_STATE_ROWS; ++k2)
		{
			S[k2] = float4(0, 0, 0, 0);
		}
		if (!bFound)
		{
			Status = SB_GRAB_MISSED;
			Reason = SB_GRAB_REASON_MISSED;
			S[0] = float4(0.0, asfloat(0u), asfloat(SerialIn), In0.w);
		}
		else
		{
			float3 RestP = CarrierPosition.Load(int3(Texel, 0)).xyz;
			float4 RestNormal = CarrierNormal.Load(int3(Texel, 0));
			float3 RestN = RestNormal.xyz * rsqrt(max(dot(RestNormal.xyz, RestNormal.xyz), 1e-12));
			float Flesh = max(RestNormal.w, 0.0) * MeanScale;
			float3 Skin = PositionIn.Load(int3(Texel, 0)).xyz + SB_GrabRegionAt(int2(Texel), RestP);
			float3 N = ContactIn.Load(int3(Texel, 0)).xyz;
			float3 T1;
			float3 T2;
			float3 Bare;
			SB_GrabBasis(int2(Texel), RestN, N, T1, T2, Bare);
			float3 Gap = In0.xyz - Skin;
			float Window = SB_WindowLength(SB_VolumeAt(int2(Texel)), max(Flesh, 1e-3), SB_SkinLengthAt(int2(Texel)) * ThicknessScale);
			Status = SB_GRAB_HELD;
			Reason = 0u;
			S[0] = float4(0.0, asfloat(Texel.x | (Texel.y << 16u)), asfloat(SerialIn), In0.w);
			S[1] = float4(RestP, Flesh);
			S[2] = float4(RestN, min(Flesh, In0.w));
			S[3] = float4(Skin, 0.0);
			S[4] = float4(N, 0.0);
			S[5] = float4(dot(Gap, N), dot(Gap, T1), dot(Gap, T2), Window);
			// 붙든 동안 [7]은 잡을 때 덩어리 이동(국소 모양·출렁임 몫 — 노멀·접선 성분): 덩어리가 따라 나온 길이를 그 뒤 움직인 만큼으로 잰다(처짐 같은 늘 있는 몫은 빠진다).
			float3 LumpStart = Skin - Bare;
			S[7] = float4(dot(LumpStart, N), dot(LumpStart, T1), dot(LumpStart, T2), 0.0);
		}
	}
	else if (SB_GrabLive(Status))
	{
		uint Packed = asuint(S[0].y);
		int2 Texel = int2(Packed & 0xffffu, Packed >> 16u);
		float3 Skin = PositionIn.Load(int3(Texel, 0)).xyz + SB_GrabRegionAt(Texel, S[1].xyz);
		float3 N = ContactIn.Load(int3(Texel, 0)).xyz;
		float3 T1;
		float3 T2;
		float3 Bare;
		SB_GrabBasis(Texel, S[2].xyz, N, T1, T2, Bare);
		S[3].xyz = Skin;
		S[4].xyz = N;
		S[8] = float4(0, 0, 0, 0);
		if (Status == SB_GRAB_HELD && bHold && SerialIn == SerialHere)
		{
			// 잡은 자리를 잡은 뒤 끈 양 = 끄는 자리 − (덩어리 이동 없는 잡은 점 + 간격 + 잡을 때 덩어리 이동). 피부와 덩어리가 함께 이 한도 곡선에 머문다(사용자
			// 11-05 "가슴 쪽의 경우 한도가 꽤나 멀리까지" — 일지 350): 한도 min(h, R)까지 그대로, 손이 1.6배에 오면 1.3배에 닿고, 그 너머는 머문다.
			float3 LumpStart = S[7].x * N + S[7].y * T1 + S[7].z * T2;
			float3 Pulled = In0.xyz - (Bare + S[5].x * N + S[5].y * T1 + S[5].z * T2) - LumpStart;
			float Limit = max(S[2].w, 1e-3);
			float PulledLength = length(Pulled);
			float3 Capped = PulledLength > 1e-6 ? Pulled * (SB_PressLimit(PulledLength, Limit) / PulledLength) : float3(0, 0, 0);
			// 덩어리 용수철의 목표 = 잡을 때 덩어리 이동 + 한도 안 당김 — 덩어리의 지금 자리를 넣지 않는다. 일지 348은 한도 너머에서 지금 자리를 넣어 목표가
			// 덩어리를 쫓았다: 덩어리가 아무리 가도 같은 힘에 끌렸고(무른 가슴은 손을 끝까지 따라감), 몇 프레임 늦게 오는 출렁이는 자리를 되먹여 터졌다(일지 350).
			// 덩어리는 Lump Grip과 덩어리 단단함의 비만큼 이 목표 쪽으로 오고, 표면 장이 나머지(한도 안 당김 − 덩어리가 따라 나온 몫)를 그린다.
			float3 Follow = (Skin - Bare) - LumpStart;
			float3 SkinPull = Capped - Follow;
			float SkinMax = SB_PRESS_OVERSHOOT * Limit;
			float SkinLength = length(SkinPull);
			SkinPull *= SkinLength > SkinMax ? SkinMax / SkinLength : 1.0;   // 덩어리가 출렁여 반대로 나가도 표면 장 몫은 한도 안
			float3 Drawn = bContactLong != 0u ? SkinPull : N * dot(SkinPull, N);   // Basic은 옆 몫을 그리지 않는다
			S[6] = float4(Drawn, 0.0);
			S[3].w = PulledLength;
			S[4].w = length(Follow + Drawn);   // 보이는 길이: 잡은 자리가 잡은 뒤 움직인 길이(덩어리 + 표면 장)
			S[8] = float4(LumpStart + Capped, length(Follow));
		}
		else if (Status == SB_GRAB_HELD)
		{
			float3 Last = S[6].xyz;
			S[7] = float4(dot(Last, N), dot(Last, T1), dot(Last, T2), length(Last));
			Status = SB_GRAB_RELEASING;
			Reason = SB_GRAB_REASON_RELEASED;
		}
		if (Status == SB_GRAB_RELEASING)
		{
			float3 Left = S[7].xyz * HoldDecay;
			float Length = length(Left);
			if (Length < SB_REST_EPSILON)
			{
				Left = float3(0, 0, 0);
				Length = 0.0;
				Status = SB_GRAB_EMPTY;
			}
			S[7] = float4(Left, Length);
			S[6] = float4(Left.x * N + Left.y * T1 + Left.z * T2, 0.0);
			S[4].w = Length;
		}
	}
	else if (Status == SB_GRAB_MISSED && !(bHold && SerialIn == SerialHere))
	{
		// CPU가 못 잡음을 받아 줄을 놓았다(까닭·번호는 남겨 둔다 — 읽기가 본다).
		Status = SB_GRAB_EMPTY;
	}
	S[0].x = asfloat(Status | (Reason << 8u));
	UNROLL
	for (uint k3 = 0u; k3 < SB_GRAB_STATE_ROWS; ++k3)
	{
		GrabStateOut[Base + k3] = S[k3];
	}
}


RWTexture2D<uint> CarrierKindOut;

/** 칸 종류 표(위 CarrierKind 주석)를 모든 칸에 쓴다. 캐리어가 바뀔 때만 돈다. */
[numthreads(8, 8, 1)]
void CarrierKindCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= Resolution || Id.y >= Resolution)
	{
		return;
	}
	int2 T = int2(Id.xy);
	uint Kind = SB_KindAt(T);
	uint Outer = 0u;
	if (Kind == SB_KIND_GHOST)
	{
		UNROLL
		for (int k = 0; k < 4; ++k)
		{
			int2 Next = T + SB_NeighborOffset(k);
			if (any(Next < int2(0, 0)) || any(Next >= int2(Resolution, Resolution)) || !SB_IsCarried(SB_KindAt(Next)))
			{
				Outer = SB_KIND_OUTER_GHOST;
			}
		}
	}
	CarrierKindOut[T] = Kind | Outer;
}

// --- 접촉 압력 하나에서 함몰과 솟음을 (ADR-0037) ---
// 손이 붙든 살의 거친 칸마다 접촉 힘 f_j를 풀고, 그 힘에 살층의 실제 응답(표 SoftbodyContactResponse.ush — 3-D 살층 경계 요소에서 만든 것)을 입힌다.
// 한 응답에서 꺼짐·0 지점·솟음·솟는 부피 몫이 같이 나온다(그 전 솟음은 함몰 꼬리가 끝난 자리에서 켜지는 문턱으로 흉내 냈다 — 일지 272).
// 응답은 두 폭으로 나눈다:
//  - 짧은 몫(가는 창): (1 − ℓ₁²∇²)u = 원천 — 함몰 DentCS·DentCoarseCS가 그대로 푼다(폭 ℓ₁ = (ℓ₁/h)(v, τ)·h, 딱 붙는 핀, 셀이 접히는 원천 상한).
//  - 긴 몫 L(솟음 +, cm): 표의 실공간 커널 ĝ_long을 힘으로 모은 것. 붙든 칸 목표가 T = P + L − γ·h·A₁·p, 합치기가 U − L.
// 패스(갱신마다 함몰 앞, Contact Detail Full):
//   ContactPressureCellsCS : 함몰 묶음 목록의 가는 묶음마다 거친 칸(B×B)의 붙든 칸(붙들 깊이 P > 0)을 모아 압력 칸 목록에 넣는다 — 단계 0 = 거친 칸,
//                            1 = 2×2, 2 = 4×4(같은 가는 묶음 안). 칸마다 붙든 넓이·무게 중심·목표 g = P − s·a의 평균·두께·흐린 재료·가까운 랜드마크 16개.
//                            칸 가운데·온 넓이로 두면 테두리 칸이 반 칸 넓게 눌려 얇은 살에서 거친 칸 ≤ h/6이 필요했다(일지 275).
//   ContactPressureArgsCS  : 칸 수가 상한 안인 가장 가는 단계를 고르고 행렬·풀이 인자를 쓴다.
//   ContactPressureMatrixCS: 쌍마다 처짐/힘 K(i, j)(대칭) — 짧은 몫은 원판끼리 평균한 K0 닫힌 꼴, 긴 몫은 표(가까운 쌍·제 칸은 원판 위 구적).
//   ContactPressureSolveCS : 한 스레드 묶음이 활성 집합 켤레 기울기로 0 ≤ f ≤ p_c·A를 푼다(예산 바퀴 × CG, 0에서 — 상태 없음). 행렬은 반정밀.
//   ContactLongGatherCS    : 거친 칸마다 L = −Σ f_j ĝ_long(d/h̄)/h̄(원천에서 6h̄ 안)과 그 칸 압력 칸의 거친 압력 p를 ContactLong(RG)에 쓴다.
// 쌍의 재료: 두께 h̄ = min(h_i, h_j)(두께가 바뀌는 층 2-D FEM에서 단순 규칙 가운데 가장 가깝다 — 얇은 살이 응답을 묶는다, 일지 275), 부피 보존·겉껍질
//   길이는 두 칸의 흐린 값(CoarseMaterialCS) 평균. 거리 d = max(직선, 랜드마크 하한) — 행렬은 두 칸 모두의 가까운 랜드마크로(대칭).
// 거친 격자 크기·칸 표(CoarseInfo·CoarseRow)·랜드마크 거리표와 그 읽기 함수는 "거친 격자 공용"(함몰 부분 앞)에 있다.
RWTexture2D<float4> CoarseInfoOut;
Texture2D<float4> CoarseLayer;       // x 살 칸 두께 평균(메시 로컬), y 렌더 삼각형 변 평균(메시 로컬), z 살 칸 함몰 가는 창 폭 ℓ₁의 평균(메시 로컬 — 함몰 거친 해의
                                     // 폭), w 살 칸 부피 보존 v(0 폼 … 1 살)의 평균
RWTexture2D<float4> CoarseLayerOut;
RWTexture2D<uint> CoarseRowOut;
// 거친 칸 재료(처음 한 번 — CoarseMaterialCS): x 흐린 부피 보존 v(0.5h 폭), y 흐린 겉껍질 장력 길이(메시 로컬, 제 칸 가는 창 폭 ℓ₁의 0.5배 폭). 정지 위치
// 거리 가우스로 같은 차트 안에서 흐린다 — 받는 칸 재료가 칸 단위로 바뀌면 칠 경계에서 긴 몫이 끊긴다(살 ↔ 폼 경계 손 밖 0.45 mm 계단, 일지 275). 겉껍질
// 길이를 0.5h로 흐리면 센 겉껍질 안 맨살 자리가 둘레 겉껍질을 보고 단단하다고 여긴다(일지 281). 칠은 실행 중에 바뀌지 않는다 — 저작 에셋
// 값이 바뀌면(에디터) C++가 표를 다시 만든다.
Texture2D<float2> CoarseMaterial;
RWTexture2D<float2> CoarseMaterialOut;
// 거친 칸마다 살 칸의 겉껍질 장력 길이 평균(메시 로컬, 흐리기 전): CoarseInitCS가 쓰고 CoarseMaterialCS만 읽는 임시 텍스처.
Texture2D<float> CoarseSkin;
RWTexture2D<float> CoarseSkinOut;
// 부위 나누기(처음 한 번, 캐리어에 구워져 있을 때만): 거친 칸마다 살 칸의 두 부위 몫 평균 — 끌림이 원천·받는 칸 몫으로 읽는다(CoarseSeparation).
RWTexture2D<float2> CoarseSeparationOut;
uint bCarrierSeparation;             // 1 = 캐리어에 부위 나누기가 구워져 있다(켜고 끄기와 상관없이 표를 만든다)
// 연속 굽힘(Flex 칠, scratchpad f11/bend): 거친 칸마다 살 칸 겉 굽힘 길이 ℓ_b(cm) 평균 — 흐리기 전은 CoarseInitCS가 쓰고 CoarseMaterialCS만 읽는 임시,
// 흐린 값(CoarseBend, 부피 보존과 같은 0.5h 폭 — 받는 칸 굽힘이 칸 단위로 바뀌어 칠 경계에서 끊기지 않게)은 압력 칸 성분 양·긴 몫 모으기·함몰 거친 창이
// 읽는다. 흰 칸은 정확히 0. Flex Offset이 바뀌면 C++가 표를 다시 만든다(캐시 열쇠).
RWTexture2D<float> CoarseBendRawOut;
Texture2D<float> CoarseBendRaw;
RWTexture2D<float> CoarseBendOut;
Texture2D<float> CoarseBend;
// 원천 머리(끌림 ADR-0033이 쓴다): [0] 원천 수, [1..3] 정지 위치 최소(순서 보존 정수 SB_OrderedFloat), [4..6] 최대, [7] 폭 최대(cm × 1000), [8] 넘친 원천 수.
// 압력 머리도 같은 앞 낱말을 쓴다: [0] 단계 0 칸 수, [1..3]·[4..6] 단계 0 무게 중심 상자, [7] 살 두께 최대(cm × 1000), [8] 넘친 칸 수, [9] 단계 1 칸 수,
// [10] 단계 2 칸 수, [11] 고른 단계, [12] 고른 단계 칸 수 N. ActivityArgsCS가 갱신마다 비운다.
#define SB_SOURCE_HEADER_COUNT 0u
#define SB_SOURCE_HEADER_MIN 1u
#define SB_SOURCE_HEADER_MAX 4u
#define SB_SOURCE_HEADER_SIGMA 7u
#define SB_SOURCE_HEADER_OVERFLOW 8u
#define SB_SOURCE_HEADER_WORDS 12u    // C++ SourceHeaderWords
#define SB_PRESSURE_HEADER_LEVEL1 9u
#define SB_PRESSURE_HEADER_LEVEL2 10u
#define SB_PRESSURE_HEADER_LEVEL 11u
#define SB_PRESSURE_HEADER_COUNT 12u
#define SB_PRESSURE_HEADER_WORDS 16u  // C++ PressureHeaderWords
#define SB_PRESSURE_HEADER_BEND 13u   // 1 = 이번 갱신 압력 칸에 겉 굽힘(ℓ_b > 0)이 있다 — 풀이 예산을 12 × 8로(연속 굽힘, 조건수가 100배 큰다)
#define SB_PRESSURE_LEVELS 3u
#define SB_PRESSURE_CAP 768u          // 한 단계의 압력 칸 상한(C++ PressureCapacity) — 풀이 한 묶음이 반정밀 행렬 N²을 곱셈마다 읽는다(일지 275·283)
// 칸 가우스 분산 ÷ a²(a² = A/π, 일지 289, replica.py CELL_SPREAD). 칸 하나를 가우스 하나로 보면 2차 모멘트(원판 0.25)에서 칸 격자의 가우스 합이 19 %
// 물결이라 넓은 누름의 힘이 3-D 기준보다 2~8 % 작고, 물결이 1 % 밑인 0.733이면 한 칸에 몰린 누름(손가락)의 제 칸 값이 작아 힘이 4~9 % 크다. 0.35 +
// 칸 몫 나누기(SB_PressureAmountRows)에서 몸·손바닥·손 힘 −2.1~+1.4 %, 하드 서피스 판 가장자리 손바닥 가라앉음 −1.1 %(나누기 없으면 +11.7 %).
#define SB_PRESSURE_CELL_SPREAD 0.35
#define SB_PRESSURE_GEOM_ROWS 7u      // 칸 기록 앞 일곱 줄(자리·넓이·두께·목표·재료·랜드마크) — 행렬·모으기가 공유 메모리에 올리는 줄
#define SB_PRESSURE_AMOUNT_WORDS SB_RESPONSE_AMOUNT_M   // 성분 양 2M개를 반정밀 둘씩(일지 289)
#define SB_PRESSURE_LOCAL_ROW 12u     // 칸 기록 [12]: x = asuint(칸 안 대각 몫 Σ α_m a_m / A), y = 미는 방향, z = 조각 번호(F10)
#define SB_PRESSURE_ROWS 13u          // 압력 칸 하나의 uint4 줄 수(C++ PressureRows) = 일곱 + 성분 양 다섯(M = 20) + 칸 안 대각 하나
#define SB_PRESSURE_BOUND_TOL 1.03   // 랜드마크 하한을 이 값으로 나눈다 — 판 4 표의 평면 과대 ≤ 2.75 %(끌림 SB_DRAG_BOUND_TOL과 같은 값, 나누면 직선과 이어진다)
#define SB_PRESSURE_NO_SLOT 1023u     // 거친 칸 → 압력 칸 번호(단계마다 10비트)에서 빈 자리
// 압력 칸(uint4 × SB_PRESSURE_ROWS, [단계 × 상한 + 번호]): [0] xyz = asuint(붙든 칸 정지 위치 무게 중심, 메시 로컬), w = asuint(붙든 넓이 cm²);
// [1] x = asuint(살 두께 cm), y = 거리표 줄(무게 중심에 가장 가까운 거친 칸), z = asuint(목표 깊이 g cm), w = asuint(셀이 접히는 힘 상한 p_c·A, 살은 아주 큰 값);
// [2] x = asuint(흐린 v), y = asuint(흐린 겉껍질 길이 cm), z = 대표 거친 칸(x | y << 16), w = 붙든 가는 칸 수;
// [3..6] 그 줄의 가까운 랜드마크 16개: 번호 | (거리, 0.01 메시 로컬 단위) << 8 — 가까운 순(끌림 원천과 같은 꼴);
// [7..11] 칸 재료(두께·v·겉껍질 길이)의 성분 양(SB_ResponseAmounts, 일지 289): 낱말 w = 양 2w | 양 2w + 1 << 16(반정밀), 양 0..M−1 = 넓은 가우스 몫
// (1 − α)a, M..2M−1 = b/s²; [12] x = asuint(칸 안 대각 몫 Σ α_m a_m / A)(SB_PressureAmountRows), y = 붙든 칸 미는 방향 평균
// (SB_PressurePackNormal), z = 조각 번호(SB_PressurePiece — 이어진 살 겉면마다 하나), w = asuint(겉 굽힘 길이 cm — 연속 굽힘, 흰 칸 0). y·z는 행렬만
// 읽는다(F10), w는 모으기가 읽는다.
RWBuffer<uint> PressureHeaderOut;
Buffer<uint> PressureHeader;
RWStructuredBuffer<uint4> PressureCellsOut;
StructuredBuffer<uint4> PressureCells;
RWTexture2D<uint> PressureSlotOut;   // 거친 칸마다 그 칸이 든 압력 칸 번호(단계 0 | 단계 1 << 10 | 단계 2 << 20). 함몰 목록 묶음 안 칸만 쓴다
Texture2D<uint> PressureSlot;
RWBuffer<uint> PressureMatrixOut;    // K(i, j) = 처짐/힘(cm per μ·cm²): 낱말 [i × P + j/2]에 j 짝·홀 둘(반정밀, P = ⌈N/2⌉, 일지 283). 대칭이라 열로 읽어도 같다.
                                     // C++ F.PressureMatrix 할당 = 상한 × ⌈상한/2⌉ 낱말(SB_PRESSURE_CAP = PressureCapacity)
Buffer<uint> PressureMatrix;
RWBuffer<float> PressureForceOut;    // 칸마다 접촉 힘 f(μ·cm²)
Buffer<float> PressureForce;
RWBuffer<uint> PressureArgsOut;      // [0..2] 행렬 패스 묶음 수(⌈N/32⌉, ⌈N/32⌉, 1), [3..5] 풀이(N > 0이면 1, 1, 1)
RWBuffer<uint> PressureStatsOut;     // [0] 풀이 바퀴 수, [1] CG 걸음 수(stat softbody)
RWTexture2D<float2> ContactLongOut;  // 모으기: 거친 칸마다 (L, p)
// 연속 굽힘(Flex 순열 모으기): 거친 칸마다 굽힘 거친 해 W_c = Σ_j f_j ĝ_full(d/h̄; v̄, τ̄, δ̄)/h̄(cm, 아래로 +) — 받는 칸이 굽을 때만(흐린 굽힘 길이 > 0), 나머지 0.
// 가는 창 κ(4단계)가 W_c + L 쪽으로 당긴다. 쌍 δ̄ = ½(ℓ̄_b 받는 칸 + ℓ_b 원천)/h̄, 표 끝 = max(6, 3δ̄)·h̄(긴 몫의 6h̄와 같고 센 굽힘만 3ℓ_b까지).
RWTexture2D<float> ContactBendOut;
Texture3D<float> ContactResponseBendFull;   // 굽힘 전체 커널 ĝ_full(R16F — ContactResponseSampler 세 겹 선형, 표 SoftbodyContactResponse.ush 굽힘 절)
// 이 자산 거친 칸 흐린 굽힘 길이의 최대(cm × 1000 — CoarseMaterialCS가 원자 최대로, 표를 만들 때 0으로 비운다). 모으기가 셈할 칸 상자를 넓힌다.
RWBuffer<uint> CoarseBendMaxOut;
Buffer<uint> CoarseBendMax;
// 부위 나누기(순열 SB_SEPARATION): 압력 칸마다 붙든 가는 칸의 두 부위 몫 평균([단계 × 상한 + 번호] — 압력 칸 패스가 쓰고 모으기가 읽는다)과,
// 모으기가 거친 칸마다 쓰는 솟음 채널(ContactLongSeparation).
RWStructuredBuffer<float2> PressureSeparationOut;
StructuredBuffer<float2> PressureSeparation;
RWTexture2D<float4> ContactLongSeparationOut;
Texture3D<float> ContactResponseLong;      // 긴 몫 커널 표(x = ρ, y = τ, z = v, R16F — C++ SoftbodyContactResponse 전역 텍스처)
Texture3D<float> ContactResponseAmounts;   // 압력 행렬 성분 양 표(x = 성분, y = 두께, z = ν × L_T, R32F — 압력 칸 목록·하드 서피스 펼치기만, 일지 289)
Texture3D<float> ContactResponseBendAmounts;   // 굽힘 성분 양 표(x = 성분, y = 굽힘 매듭 1..11 × 두께, z = ν × L_T, R16F — 연속 굽힘, Flex 순열의 압력 칸 목록만)
SamplerState ContactResponseSampler;       // 세 겹 선형, 가장자리 고정
uint PressureRounds;                 // 활성 집합 바퀴 예산(C++ PressureRounds)
uint PressureSteps;                  // 바퀴마다 CG 걸음(C++ PressureSteps)
float PressureReachLayers;           // 모으기 반경 = 이 값 × h̄(6 — 5면 얇은 살 센 겉껍질 꼭대기 −20 %, 일지 275)
float PlasticShare;                  // 받아들인 깊이 가운데 압력 없이 모양만 바뀐 몫(압력 목표에서만 뺀다, ADR-0037 §3.6)

bool SB_BlockHasSurface(int2 Cell)
{
	int2 Base = Cell * (int)CoarseBlock;
	for (uint Y = 0; Y < CoarseBlock; ++Y)
	{
		for (uint X = 0; X < CoarseBlock; ++X)
		{
			if (SB_KindAt(Base + int2(X, Y)) == SB_KIND_SURFACE)
			{
				return true;
			}
		}
	}
	return false;
}

/**
 * 처음 한 번(캐리어나 거친 격자가 바뀌면 다시): 거친 칸마다 살 칸(표면 칸이고 두께 > 0 — 눌리는 정도 칠 0인 칸은 살이 없다)의 정지 위치
 * 평균·넓이의 합·두께 평균·삼각형 변 평균. 칸 넓이는 칸이 있는 이웃 네 칸까지 거리의 평균을 제곱한 것(메시 로컬²). 살 칸이 없는 거친 칸은
 * 넓이 0 — 합치기의 B-스플라인 탭에서 빠지고 커널이 더하지 않는다. 캐리어로만 정해지는 값이다.
 */
[numthreads(8, 8, 1)]
void CoarseInitCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= CoarseResolution || Id.y >= CoarseResolution)
	{
		return;
	}
	int2 Base = int2(Id.xy) * (int)CoarseBlock;
	float3 SumRest = float3(0, 0, 0);
	float SumArea = 0.0;
	float SumThickness = 0.0;
	float SumReach = 0.0;
	float SumEdge = 0.0;
	float SumVolume = 0.0;
	float SumSkin = 0.0;
	float2 SumSeparation = float2(0, 0);
	float SumBend = 0.0;   // 겉 굽힘 길이(cm, 연속 굽힘 — 이 패스는 C++가 하드 서피스 정의를 켜서 컴파일한다)
	float Count = 0.0;
	for (uint Y = 0; Y < CoarseBlock; ++Y)
	{
		for (uint X = 0; X < CoarseBlock; ++X)
		{
			int2 Texel = Base + int2(X, Y);
			float4 Rest = CarrierPosition.Load(int3(Texel, 0));
			float Thickness = CarrierNormal.Load(int3(Texel, 0)).w;
			if ((uint)round(Rest.w) != SB_KIND_SURFACE || Thickness <= 0.0)
			{
				continue;
			}
			float Spacing = 0.0;
			float Neighbors = 0.0;
			UNROLL
			for (int k = 0; k < 4; ++k)
			{
				int2 N = Texel + SB_NeighborOffset(k);
				if (all(N >= int2(0, 0)) && all(N < int2(Resolution, Resolution)) && SB_IsCarried(SB_KindAt(N)))
				{
					Spacing += length(CarrierPosition.Load(int3(N, 0)).xyz - Rest.xyz);
					Neighbors += 1.0;
				}
			}
			Spacing = Neighbors > 0.0 ? Spacing / Neighbors : TexelSize / ThicknessScale;
			SumRest += Rest.xyz;
			SumArea += Spacing * Spacing;
			SumThickness += Thickness;
			float Volume = SB_VolumeAt(Texel);
			float Skin = SB_SkinLengthAt(Texel);
			SumReach += SB_WindowLength(Volume, Thickness, Skin);
			SumEdge += CarrierRedirect.Load(int3(Texel, 0)).z;
			SumVolume += Volume;
			SumSkin += Skin;
			BRANCH
			if (bCarrierSeparation != 0u)
			{
				SumSeparation += SB_SeparationAt(Texel);
			}
#if SB_HARD_SURFACE
			SumBend += SB_BendLengthAt(Texel);
#endif
			Count += 1.0;
		}
	}
	if (Count <= 0.0)
	{
		CoarseInfoOut[Id.xy] = float4(0, 0, 0, 0);
		CoarseLayerOut[Id.xy] = float4(0, 0, 0, 0);
		CoarseSkinOut[Id.xy] = 0.0;
		if (bCarrierSeparation != 0u)
		{
			CoarseSeparationOut[Id.xy] = float2(0, 0);
		}
#if SB_HARD_SURFACE
		CoarseBendRawOut[Id.xy] = 0.0;
#endif
		return;
	}
	CoarseInfoOut[Id.xy] = float4(SumRest / Count, SumArea);
	CoarseLayerOut[Id.xy] = float4(SumThickness / Count, SumEdge / Count, SumReach / Count, SumVolume / Count);
	CoarseSkinOut[Id.xy] = SumSkin / Count;
	if (bCarrierSeparation != 0u)
	{
		CoarseSeparationOut[Id.xy] = SumSeparation / Count;
	}
#if SB_HARD_SURFACE
	CoarseBendRawOut[Id.xy] = SumBend / Count;
#endif
}

#define SB_COARSE_INDEX_THREADS 1024
groupshared uint SB_BulgeIndexCounts[SB_COARSE_INDEX_THREADS];

/**
 * 처음 한 번(CoarseInitCS 다음): 살 있는 거친 칸(넓이 > 0)에 줄 우선 순서로 번호를 매긴다 — 거리표의 줄 번호다(굽기가 같은 규칙으로 센다).
 * 스레드 묶음 하나가 칸을 이어진 조각으로 나눠 세고 앞 조각들의 합(Hillis–Steele 누적)을 더한다.
 */
[numthreads(SB_COARSE_INDEX_THREADS, 1, 1)]
void CoarseRowCS(uint LocalIndex : SV_GroupIndex)
{
	uint NumCells = CoarseResolution * CoarseResolution;
	uint Chunk = (NumCells + SB_COARSE_INDEX_THREADS - 1u) / SB_COARSE_INDEX_THREADS;
	uint First = LocalIndex * Chunk;
	uint Last = min(First + Chunk, NumCells);
	uint Count = 0u;
	for (uint Cell = First; Cell < Last; ++Cell)
	{
		Count += CoarseInfo.Load(int3(Cell % CoarseResolution, Cell / CoarseResolution, 0)).w > 0.0 ? 1u : 0u;
	}
	SB_BulgeIndexCounts[LocalIndex] = Count;
	GroupMemoryBarrierWithGroupSync();
	for (uint Stride = 1u; Stride < SB_COARSE_INDEX_THREADS; Stride <<= 1u)
	{
		uint Add = LocalIndex >= Stride ? SB_BulgeIndexCounts[LocalIndex - Stride] : 0u;
		GroupMemoryBarrierWithGroupSync();
		SB_BulgeIndexCounts[LocalIndex] += Add;
		GroupMemoryBarrierWithGroupSync();
	}
	uint Row = SB_BulgeIndexCounts[LocalIndex] - Count;
	for (uint Listed = First; Listed < Last; ++Listed)
	{
		int2 C = int2(Listed % CoarseResolution, Listed / CoarseResolution);
		bool bFlesh = CoarseInfo.Load(int3(C, 0)).w > 0.0;
		CoarseRowOut[C] = bFlesh ? Row : SB_LANDMARK_NO_ROW;
		Row += bFlesh ? 1u : 0u;
	}
}

/**
 * 처음 한 번(CoarseInitCS 다음 — 그 패스가 새 표의 ℓ₁로 CoarseLayer.z를 채운다): 거친 칸 재료를 정지 위치 거리 가우스로 흐린다(같은 차트 — 정지
 * 위치가 먼 칸은 3σ 밖이라 빠진다). 접촉 압력 행렬과 긴 몫 모으기가 쌍의 재료로 읽는다. 부피 보존 v는 0.5h 폭 — 받는 칸 재료가 칸 단위로 바뀌면 칠
 * 경계에서 긴 몫이 끊긴다(살 ↔ 폼, 일지 275). 겉껍질 길이는 제 칸 가는 창 폭 ℓ₁의 0.5배 — 짧은 몫이 닿는 넓이의 겉껍질 평균이다. 0.5h로 흐리면 센
 * 겉껍질 안 맨살 자리의 접촉이 둘레 겉껍질을 보고 단단하다고 여겨 힘이 세 배가 되고, 3-D에 없는 솟은 고리(0.6 cm)가 구멍 둘레에 섰다(일지 281).
 * 살 없는 칸은 살(v 1)·맨살.
 */
[numthreads(8, 8, 1)]
void CoarseMaterialCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= CoarseResolution || Id.y >= CoarseResolution)
	{
		return;
	}
	float4 Info = CoarseInfo.Load(int3(Id.xy, 0));
	if (Info.w <= 0.0)
	{
		CoarseMaterialOut[Id.xy] = float2(1.0, 0.0);
		CoarseBendOut[Id.xy] = 0.0;
		return;
	}
	float4 LayerHere = CoarseLayer.Load(int3(Id.xy, 0));
	float SigmaVolume = max(0.5 * LayerHere.x, 1e-6);   // 메시 로컬: 0.5h
	float SigmaSkin = max(0.5 * LayerHere.z, 1e-6);     // 메시 로컬: 0.5ℓ₁(제 칸 가는 창 폭)
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	// 아래 3σ 자름과 같은 반경(두꺼운 살은 8칸에서 자른다 — 처음 한 번이라 값싸다. 16칸·자르지 않음은 더 낫지 않았다)
	int Radius = clamp((int)ceil(3.0 * max(SigmaVolume, SigmaSkin) / max(CoarseLocal, 1e-6)), 0, 8);
	float2 Sum = float2(0, 0);
	float2 SumWeight = float2(0, 0);
	float SumBend = 0.0;   // 겉 굽힘 길이(연속 굽힘) — 부피 보존과 같은 무게(0.5h)
	for (int DY = -Radius; DY <= Radius; ++DY)
	{
		for (int DX = -Radius; DX <= Radius; ++DX)
		{
			int2 C = int2(Id.xy) + int2(DX, DY);
			if (!SB_InCoarse(C))
			{
				continue;
			}
			float4 Other = CoarseInfo.Load(int3(C, 0));
			if (Other.w <= 0.0)
			{
				continue;
			}
			float2 D = length(Other.xyz - Info.xyz) / float2(SigmaVolume, SigmaSkin);
			float2 W = float2(D.x > 3.0 ? 0.0 : exp(-0.5 * D.x * D.x), D.y > 3.0 ? 0.0 : exp(-0.5 * D.y * D.y)) * Other.w;
			Sum += W * float2(CoarseLayer.Load(int3(C, 0)).w, CoarseSkin.Load(int3(C, 0)));
			SumBend += W.x * CoarseBendRaw.Load(int3(C, 0));
			SumWeight += W;
		}
	}
	float2 Own = float2(LayerHere.w, CoarseSkin.Load(int3(Id.xy, 0)));
	CoarseMaterialOut[Id.xy] = float2(SumWeight.x > 0.0 ? Sum.x / SumWeight.x : Own.x, SumWeight.y > 0.0 ? Sum.y / SumWeight.y : Own.y);
	float BendHere = SumWeight.x > 0.0 ? SumBend / SumWeight.x : CoarseBendRaw.Load(int3(Id.xy, 0));
	CoarseBendOut[Id.xy] = BendHere;
	if (BendHere > 0.0)
	{
		InterlockedMax(CoarseBendMaxOut[0], (uint)ceil(BendHere * 1000.0));
	}
}

RWTexture2D<uint4> DragLinksOut;

/**
 * 끌림 거친 칸 Cell의 방향 k 링크(DragLinks 주석). 옆 묶음에 표면 칸이 있으면 그 칸(같은 차트)이다. 없으면 차트 끝이라, 묶음의 줄마다
 * (방향에 수직) 그 방향 마지막 표면 칸 바로 바깥의 고스트 칸이 가리키는 점을 모아 평균한다 — 이 칸 면 가운데가 이음매 건너편에서
 * 닿는 점이다. 그 점을 건너편 가장자리를 따라(줄을 따라 가리키는 점이 움직인 축) 이웃한 두 거친 칸 사이로 보간한다. 이음매 양쪽이
 * 같은 규칙으로 서로를 보간하므로 두 칸 사이에 오가는 몫이 같다 — 가는 격자의 고스트 칸이 가리키는 점을 bilinear로 읽는 것
 * (SB_CurrentState)과 같은 규칙이다.
 * 솟음의 이웃 표(SB_SpreadLink)는 묶음 가운데 줄의 점이 든 칸 하나를 고른다. 그러면 이음매 양쪽이 서로 다른 칸을 가리키는 곳이 많고
 * (시험 캐릭터 캐리어에서 이음매 링크 4815개 중 2723개가 한쪽으로만 이어졌다), 점이 건너편 표면 밖 텍셀로 반올림되면 표면 칸이 없는
 * 거친 칸을 가리켜(105개) 접촉 패스가 쓰지 않은 입력 줄을 읽었다(일지 162). 보간하면 한쪽 링크가 줄고(2060개) 빈 칸은 가리키지 않는다.
 * 남는 한쪽 링크는 괜찮다: 끌림은 이웃을 읽어 푸는 반복이 아니라 원천 커널 합이고(ADR-0033), 링크는 올려 읽기와 이웃의 눌림 표지에만 쓴다.
 * 가리키는 점을 찾지 못하거나(열린 가장자리) 제 칸에 떨어지면 벽(제 번호)이다.
 * 차트 모서리 묶음(가장자리 줄들이 두 이음매에 걸친 묶음)은 줄마다 가리키는 점이 두 곳이라 그 평균이 엉뚱한 표면 묶음에 떨어졌다
 * (시험 캐릭터 캐리어에서 이음매 링크 4343개 중 226개가 정지 위치로 2.5 cm ~ 1.1 m 먼 칸). 거친 해는 거친 칸 4개보다 먼 이웃을 칸 크기
 * 거리로 보므로(SB_NeighborSpacing) 그 링크가 온전한 이웃이 되어, 넓은 함몰이 손가락에서 14 cm 떨어진 곳에 헛 함몰(1.4 mm)로 샜다(일지 217).
 * 그래서 가리키는 점의 정지 위치가 이 가장자리 칸에서 거친 칸 4개(메시 로컬)보다 먼 줄과, 처음 받아들인 줄과 다른 이음매를 가리키는
 * 줄(두 점 사이가 2B 텍셀 밖)은 버리고 남은 줄만 평균·보간한다. 남는 줄이 없으면 벽이다.
 */
uint SB_DragLink(int2 Cell, uint k)
{
	int2 Dir = SB_DragDirection(k);
	uint Self = (uint)(Cell.x + Cell.y * (int)CoarseResolution);
	int2 Next = Cell + Dir;
	if (SB_InCoarse(Next) && SB_BlockHasSurface(Next))
	{
		return (uint)(Next.x + Next.y * (int)CoarseResolution);
	}
	int B = (int)CoarseBlock;
	int2 Across = int2(abs(Dir.y), abs(Dir.x));
	int2 Far = Cell * B + int2(Dir.x > 0 ? B - 1 : 0, Dir.y > 0 ? B - 1 : 0);
	// 줄을 받는 한도(일지 217)는 둘 다 이 표를 읽는 쪽의 정의에서 온다: 가리키는 점의 정지 위치는 거친 해가 이웃으로 보는 띠 안
	// (SB_NeighborSpacing의 SB_NEIGHBOR_BAND × 거친 칸, 메시 로컬 — 그 밖의 점은 거기서 칸 크기로 잘려 온전한 이웃이 된다), 처음 받은 줄의
	// 점과는 2B 텍셀 안(링크 하나가 담는 건너편은 Home과 옆 칸 하나뿐이라 그 밖의 점은 다른 이음매다).
	float NearLocal = SB_NEIGHBOR_BAND * (float)CoarseBlock * TexelSize / ThicknessScale;
	float SameSeam = 2.0 * (float)B;
	float2 Sum = float2(0, 0);
	float2 First = float2(0, 0);
	float2 Last = float2(0, 0);
	float Count = 0.0;
	for (int Row = 0; Row < B; ++Row)
	{
		int2 RowStart = Far + Across * Row;
		for (int Step = 0; Step < B; ++Step)
		{
			int2 Texel = RowStart - Dir * Step;
			if (SB_KindAt(Texel) != SB_KIND_SURFACE)
			{
				continue;
			}
			int2 Outside = Texel + Dir;
			if (all(Outside >= int2(0, 0)) && all(Outside < int2(Resolution, Resolution)) && SB_KindAt(Outside) == SB_KIND_GHOST)
			{
				float2 Target = CarrierRedirect.Load(int3(Outside, 0)).xy;
				int2 TargetTexel = clamp((int2)round(Target), int2(0, 0), int2((int)Resolution - 1, (int)Resolution - 1));
				float Apart = distance(CarrierPosition.Load(int3(TargetTexel, 0)).xyz, CarrierPosition.Load(int3(Texel, 0)).xyz);
				bool bNear = Apart <= NearLocal;
				bool bSameSeam = Count <= 0.0 || distance(Target, First) <= SameSeam;
				if (bNear && bSameSeam)
				{
					First = Count > 0.0 ? First : Target;
					Last = Target;
					Sum += Target;
					Count += 1.0;
				}
			}
			break;
		}
	}
	if (Count <= 0.0)
	{
		return Self;
	}
	// 평균한 점이 든 거친 칸. 굽은 가장자리에서 평균이 표면 없는 묶음에 떨어지면 첫 줄의 점이 든 칸으로.
	float2 Target = Sum / Count;
	int2 Home = (int2)floor((Target + 0.5) / (float)B);
	if (!SB_InCoarse(Home) || !SB_BlockHasSurface(Home))
	{
		Target = First;
		Home = (int2)floor((Target + 0.5) / (float)B);
		if (!SB_InCoarse(Home) || !SB_BlockHasSurface(Home))
		{
			return Self;
		}
	}
	if (all(Home == Cell))
	{
		return Self;
	}
	uint Link = SB_DRAG_SEAM | (uint)(Home.x + Home.y * (int)CoarseResolution);
	float2 Run = Last - First;
	if (Count > 1.0 && dot(Run, Run) > 1e-6)
	{
		// 건너편 가장자리를 따라가는 축에서 점이 Home 한가운데를 벗어난 거리(거친 칸, −0.5 ~ 0.5)만큼 그쪽 옆 칸을 섞는다.
		uint Axis = abs(Run.x) >= abs(Run.y) ? 0u : 1u;
		float Offset = (Target[Axis] + 0.5) / (float)B - 0.5 - (float)Home[Axis];
		uint Toward = Axis * 2u + (Offset >= 0.0 ? 0u : 1u);
		int2 Other = Home + SB_DragDirection(Toward);
		uint Share = (uint)round(saturate(abs(Offset)) * 255.0);
		if (Share > 0u && SB_InCoarse(Other) && SB_BlockHasSurface(Other))
		{
			Link |= (Toward << 20u) | (Share << 22u);
		}
	}
	return Link;
}

/** 끌림 거친 격자(일지 161)의 이웃 표: 처음 한 번(캐리어나 거친 격자가 바뀌면 다시). 끌림 격자 = 솟음 격자(CoarseBlock·CoarseResolution). */
[numthreads(8, 8, 1)]
void DragLinksCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= CoarseResolution || Id.y >= CoarseResolution)
	{
		return;
	}
	int2 Cell = int2(Id.xy);
	if (!SB_BlockHasSurface(Cell))
	{
		DragLinksOut[Id.xy] = uint4(SB_DRAG_EMPTY, SB_DRAG_EMPTY, SB_DRAG_EMPTY, SB_DRAG_EMPTY);
		return;
	}
	DragLinksOut[Id.xy] = uint4(SB_DragLink(Cell, 0u), SB_DragLink(Cell, 1u), SB_DragLink(Cell, 2u), SB_DragLink(Cell, 3u));
}

// --- 함몰 거친 해 패스 (일지 199, 위 "함몰 거친 해" 주석) ---
// 거친 묶음 = 거친 칸 16×16(= 가는 묶음 B×B). 목록은 TileListCS가 함몰 묶음 목록 버퍼(TileListOut) 뒤쪽에 쓴다 — 그 패스의 쓰기 버퍼를 늘리지
// 않는다(D3D11 한도 8, ADR-0025). DentCoarseList = 그 버퍼: [Base] 수, [Base + 1..] 묶음 x | y << 16, 그 뒤 목록 비트(Base = DentCoarseListBase).
Buffer<uint> DentCoarseList;
uint DentCoarseListBase;             // 가는 묶음 수(C++ F.NumTiles)
Buffer<uint> DentCoarseBusy;          // 지난 갱신 끝의 남은 표지(SB_COARSE_BUSY_*, 거친 묶음마다)
RWBuffer<uint> DentCoarseBusyOut;
RWTexture2D<float4> DentCoarseStartOut;
Texture2D<float4> DentCoarseStart;    // x 출발 값(Ū), y 아래 한계 Ψ(없으면 SB_COARSE_NONE), z·w 쓰지 않음
Texture2D<float> DentCoarseIn;        // 둘째 거친 패스부터: 앞 패스 결과
RWTexture2D<float> DentCoarseOut;
RWTexture2D<float> DentCoarseFixOut;
uint DentCoarseFirst;                 // 1 = 첫 거친 패스(출발 값 = Ū)
uint DentCoarseLast;                  // 1 = 마지막 거친 패스(거친 해·고침·남은 표지를 쓴다)
float DentCoarseTheta;                // 거친 칸 체비쇼프 범위 [Low, 2]의 가운데
float DentCoarseHalf;                 // 반폭

/** 거친 묶음 한 변의 수. */
uint SB_CoarseTilesPerSide()
{
	return (CoarseResolution + SB_TILE - 1u) / SB_TILE;
}

/** 거친 칸이 이번 거친 목록에 있는가. */
bool SB_CoarseListed(int2 Cell)
{
	uint Wc = SB_CoarseTilesPerSide();
	uint Tile = (uint)(Cell.y / SB_TILE) * Wc + (uint)(Cell.x / SB_TILE);
	return ((DentCoarseList[DentCoarseListBase + 1u + Wc * Wc + (Tile >> 5u)] >> (Tile & 31u)) & 1u) != 0u;
}

/** 거친 패스가 읽는 칸 값: 거친 목록 밖은 0(그 묶음은 0까지 잦아든 뒤에 빠졌다). */
float SB_CoarseInput(int2 Cell)
{
	if (!SB_InCoarse(Cell) || !SB_CoarseListed(Cell))
	{
		return 0.0;
	}
	return DentCoarseFirst != 0u ? DentCoarseStart.Load(int3(Cell, 0)).x : DentCoarseIn.Load(int3(Cell, 0));
}

/** 거친 칸의 재료 폭 비의 제곱 (살 칸 ℓ₁ 평균 / 두께 평균)² — 함몰 거친 단계의 변마다 폭(SB_EdgeRatio). 살 없는 칸은 0. */
float SB_CoarseRatio(int2 Cell)
{
	float4 Layer = CoarseLayer.Load(int3(Cell, 0));
	return Layer.x > 0.0 ? (Layer.z / Layer.x) * (Layer.z / Layer.x) : 0.0;
}

/** 거친 칸의 지금 피부 자리(접촉 패스의 DragInput 셋째 줄, 프레임 cm). */
float3 SB_CoarseSkin(int2 Cell)
{
	return DragInput[((uint)Cell.y * CoarseResolution + (uint)Cell.x) * SB_DRAG_INPUT_ROWS + 2u].xyz;
}

/** 거친 칸 살 칸의 겉껍질 장력 길이 평균(cm, 같은 줄의 w — ADR-0037 §3.4). 셀이 접히는 상한이 가는 창과 같은 재료를 보게 한다. */
float SB_CoarseSkinLength(int2 Cell)
{
	return max(DragInput[((uint)Cell.y * CoarseResolution + (uint)Cell.x) * SB_DRAG_INPUT_ROWS + 2u].w, 0.0);
}

/**
 * 거친 칸마다 출발 값과 아래 한계(거친 묶음마다 스레드 묶음 하나, 칸마다 스레드 하나). 가는 목록 묶음이 덮는 칸: 지난 갱신에 푼 묶음이면 Ū = 표면 칸
 * 함몰(상태 A) 평균, 아니면 지난 거친 해. 붙든 칸이 있으면 Ψ = Ū + max(붙들 깊이 − 그 칸의 지난 해) — 거친 해가 붙든 칸을 덮을 만큼은 내려간다(위로는
 * 막지 않는다). 가는 목록 밖 칸은 지난 거친 해, 한계 없음.
 */
[numthreads(SB_TILE, SB_TILE, 1)]
void DentCoarseRestrictCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID)
{
	uint Entry = GroupId.y * SB_TILE_ROW + GroupId.x;
	if (Entry >= DentCoarseList[DentCoarseListBase])
	{
		return;
	}
	uint Packed = DentCoarseList[DentCoarseListBase + 1u + Entry];
	int2 Cell = int2(Packed & 0xffffu, Packed >> 16u) * SB_TILE + int2(LocalId.xy);
	if (!SB_InCoarse(Cell))
	{
		return;
	}
	float Before = DentCoarse.Load(int3(Cell, 0));
	float Start = Before;
	float Floor = SB_COARSE_NONE;
	int2 Base = Cell * (int)CoarseBlock;
	if (all(Base < int2(Resolution, Resolution)) && SB_TileListed(Base))
	{
		bool bSolved = TileResidualOut[(uint)(Base.y / SB_TILE) * TilesPerSide + (uint)(Base.x / SB_TILE)] != 0u;
		float Sum = 0.0;
		float Count = 0.0;
		float Gap = SB_COARSE_NONE;
#if SB_HARD_SURFACE
		float GluedSum = 0.0;
		float GluedCount = 0.0;
		bool bRigidGlue = false;   // 붙은 강체 판 칸이 든 거친 칸 — 거친 함몰 창이 보통 칸의 0 바닥을 걷는다(배치 3)
#endif
		for (uint Y = 0u; Y < CoarseBlock; ++Y)
		{
			for (uint X = 0u; X < CoarseBlock; ++X)
			{
				int2 T = Base + int2(X, Y);
				if (any(T >= int2(Resolution, Resolution)) || SB_CarrierKindAt(T) != SB_KIND_SURFACE)
				{
					continue;
				}
				float U = bSolved ? StateIn.Load(int3(T, 0)) : Before;
				float P = SB_DentHardNow(T);   // 점성 칸의 기억은 한계가 아니다(가는 창이 점성으로 푼다)
				Sum += U;
				Count += 1.0;
#if SB_HARD_SURFACE
				// 센 판 칸(용수철 몫 w ≥ 0.5 — 강체 판은 검정이라 늘 그렇다): 가는 창과 같은 판 목표. 거친 칸이 통째로 그런 칸이면 아래에서 두 쪽으로 붙든다
				// (창보다 넓은 판이 한 몸으로).
				float PlateSpring;
				float PlateAim;
				float PlateShare;
				float PlateLong;
				float PlateSqueeze;
				if (SB_HardSpringAt(T, (float2)T, PlateSpring, PlateAim, PlateShare, PlateLong, PlateSqueeze) && PlateShare >= 0.5)
				{
					float Glued = PlateAim - U;
					GluedSum += Glued;
					GluedCount += 1.0;
					Gap = max(Gap, Glued);
					bRigidGlue = bRigidGlue || SB_HardBondedRigidAt(T);
					continue;
				}
				// 떠 있는 장식 밑 셔츠 칸(배치 3): 손 핀 대신 판 한쪽 바닥(판 깊이 > 0인 곳) — 가는 창과 같은 목표
				float FloatPlate;
				if (SB_HardFloatAt(T, FloatPlate))
				{
					if (FloatPlate > 0.0)
					{
						Gap = max(Gap, SB_HardFloatTarget(T, (float2)T, FloatPlate) - U);
					}
					continue;
				}
#endif
				BRANCH
				if (P > 0.0)
				{
					// 가는 창과 같은 목표(ADR-0037 — 긴 몫을 더하고 칸보다 짧은 압축 몫을 뺀 것). 평형에서 바닥이 Ū가 되어 거친 해가 밀리지 않는다.
					Gap = max(Gap, SB_DentTarget(T, (float2)T, P) - U);
				}
			}
		}
		if (Count > 0.0)
		{
			Start = bSolved ? Sum / Count : Before;
			Floor = Gap > 0.5 * SB_COARSE_NONE ? Start + Gap : SB_COARSE_NONE;
		}
#if SB_HARD_SURFACE
		if (Count > 0.0 && GluedCount == Count)
		{
			DentCoarseStartOut[Cell] = float4(Start, Start + GluedSum / GluedCount, 1.0, bRigidGlue ? 1.0 : 0.0);
			return;
		}
#endif
	}
	DentCoarseStartOut[Cell] = float4(Start, Floor, 0.0, 0.0);
}

/**
 * 거친 패스 한 번(거친 묶음마다 스레드 묶음 하나, 창 32×32 거친 칸 — 가는 DentCS와 같은 꼴). 산 칸 = 창 안쪽이고 표면이 있고 거친 목록에 든 칸. 이웃은
 * 거친 칸 링크(DragLinks)로: 같은 차트 옆 칸은 창에서, 이음매 건너편은 이 패스 처음 값으로 얼려서, 링크가 제 칸이면(열린 끝) 제 값으로 읽는다(자유단 —
 * 굽기가 열린 끝에 둔 고스트와 같은 꼴). 몫은 가는 창과 같은 식(SB_NeighborSpacing·SB_RowScale, 지금 피부 자리 사이 거리, 칸마다 함몰 폭).
 * 가는 창과 같이 0 밑으로 가지 않고, 붙든 칸이 없는 거친 칸의 1 µm 밑은 반복마다 0이다. 가는 목록이 비면(붙든 자리가 하나도 없다) 정적 해가 0이라
 * 반복 없이 0을 쓴다. 마지막 패스는 고침(U − Ū)과 남은 표지를 쓰고, 남은 묶음이 있으면 TileAnyResidual도 켠다 — 다음 갱신에 목록 패스가 돌아 거친 해가
 * 0까지 잦아들거나(가는 목록이 비면) 0으로 비워진다. 비우기 패스(ClearCS)는 쓰기 버퍼 8개가 차 있어 거친 해를 비우지 않는다.
 */
[numthreads(SB_DENT_SIDE, SB_DENT_ROWS, 1)]
void DentCoarseCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	// 모든 스레드가 장벽을 지나야 하므로 일찍 돌아가지 않는다. bListed·bClear는 SRV에서 읽어 묶음 전체가 같다.
	uint Entry = GroupId.y * SB_TILE_ROW + GroupId.x;
	bool bListed = Entry < DentCoarseList[DentCoarseListBase];
	uint Packed = bListed ? DentCoarseList[DentCoarseListBase + 1u + Entry] : 0u;
	uint2 Tile = uint2(Packed & 0xffffu, Packed >> 16u);
	int2 Origin = int2(Tile * SB_TILE) - SB_DENT_HALO;
	bool bClear = DentTileCount[0] == 0u;
	if (LocalIndex == 0u)
	{
		SB_DENT_BUSY = 0u;
	}
#if SB_HARD_SURFACE
	if (LocalIndex == 0u)
	{
		SB_DENT_NO_FLOOR = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
#endif
	// 1. 창 올리기.
	float Floor[SB_DENT_CELLS];
	float Cur[SB_DENT_CELLS];
	float Prev[SB_DENT_CELLS];
	bool bLive[SB_DENT_CELLS];
	bool bOn[SB_DENT_CELLS];
#if SB_HARD_SURFACE
	bool TwoSided[SB_DENT_CELLS];
	bool NoPin[SB_DENT_CELLS];   // 붙든 칸이 없는 거친 칸(아래 한계 없음) — 붙은 강체 판이 든 창에서 0 바닥을 걷는 칸
	float BendC[SB_DENT_CELLS];  // 연속 굽힘: 거친 칸 용수철 세기(창 올리기에서 κ, 몫을 센 뒤 κ/(1 + ΣShare)) — 0 = 굽지 않는 칸
	float AimC[SB_DENT_CELLS];   // 그 목표 W_c + L(거친 칸 값 그대로)
#endif
	UNROLL
	for (int Fill = 0; Fill < SB_DENT_CELLS; ++Fill)
	{
		int2 W = int2(LocalId.x, LocalId.y + SB_DENT_ROWS * Fill);
		int2 C = Origin + W;
		bool bInGrid = bListed && SB_InCoarse(C);
		bool bSurface = bInGrid && DragLinks.Load(int3(clamp(C, int2(0, 0), int2(CoarseResolution - 1, CoarseResolution - 1)), 0)).x != SB_DRAG_EMPTY;
		bool bIn = bSurface && SB_CoarseListed(C);
		float Value = bIn ? SB_CoarseInput(C) : 0.0;
		// 아래 한계: 붙든 칸이 있으면 Ψ, 없으면 0(가는 창의 붙들 깊이가 늘 0 이상인 것과 같다 — 함몰은 0 밑으로 가지 않는다).
		float Low = bIn ? max(DentCoarseStart.Load(int3(C, 0)).y, 0.0) : 0.0;
#if SB_HARD_SURFACE
		// 통째로 붙은 판인 거친 칸(하드 서피스): 두 쪽 — 판 값 그대로(들린 쪽은 0 위로도). 붙은 강체 판이면(시작 w) 이 창은 보통 칸 0 바닥을 걷는다.
		float4 StartHard = bIn ? DentCoarseStart.Load(int3(C, 0)) : float4(0, 0, 0, 0);
		TwoSided[Fill] = StartHard.z > 0.5;
		NoPin[Fill] = !TwoSided[Fill] && StartHard.y <= 0.5 * SB_COARSE_NONE;
		Low = TwoSided[Fill] ? StartHard.y : Low;
		if (TwoSided[Fill] && StartHard.w > 0.5)
		{
			InterlockedOr(SB_DENT_NO_FLOOR, 1u);
		}
		// 연속 굽힘: 굽는 거친 칸(흐린 굽힘 길이 > 0)은 가는 창과 같은 κ로 W_c + L 쪽 — 가는 목록 밖 묶음은 이 거친 해를 그리므로 먼 굽힘 몫이 여기서
		// 선다. 붙든 칸이 없는 굽는 칸은 0 위로도 간다(솟는 쪽). 흰 칸은 0이라 지금 그대로다.
		BendC[Fill] = 0.0;
		AimC[Fill] = 0.0;
		if (bIn && !TwoSided[Fill] && bContactLong != 0u)
		{
			float BendLength = CoarseBend.Load(int3(C, 0));
			if (BendLength > 0.0)
			{
				float4 LayerHere = CoarseLayer.Load(int3(C, 0));
				float LayerCmHere = max(LayerHere.x * ThicknessScale, 1e-3);
				float3 WindowHere = SB_ResponseWindow(LayerHere.w, SB_CoarseSkinLength(C) / LayerCmHere);
				float CellCm = CoarseTexelSize;
				BendC[Fill] = min(SB_BEND_HOLD * LayerCmHere * WindowHere.z * BendLength * BendLength * BendLength
					/ max(CellCm * CellCm * CellCm * CellCm, 1e-12), 1e20);
				AimC[Fill] = ContactBend.Load(int3(C, 0)) + ContactLong.Load(int3(C, 0)).x;
				Low = NoPin[Fill] ? SB_COARSE_NONE : Low;
			}
		}
#endif
		bool bInner = all(W > int2(0, 0)) && all(W < int2(SB_DENT_SIDE - 1, SB_DENT_SIDE - 1));
		bOn[Fill] = bIn;
		bLive[Fill] = bInner && bIn && !bClear;
		Floor[Fill] = Low;
		Cur[Fill] = bLive[Fill] ? max(Value, Low) : Value;
#if SB_HARD_SURFACE
		Cur[Fill] = bLive[Fill] && TwoSided[Fill] ? Low : Cur[Fill];
#endif
		Prev[Fill] = Cur[Fill];
		uint Slot = (uint)(W.y * SB_DENT_SIDE + W.x);
		SB_DentValueSet(Slot, Cur[Fill]);
		SB_DentValueSet(SB_DENT_AREA + Slot, Cur[Fill]);
		SB_DentPositionSet(Slot, bSurface ? float4(SB_CoarseSkin(C), 1.0) : float4(0, 0, 0, 0));
		SB_DentMaterialSet(Slot, bSurface ? SB_CoarseRatio(C) : 0.0);
	}
	GroupMemoryBarrierWithGroupSync();
#if SB_HARD_SURFACE
	bool bNoFloor = SB_DENT_NO_FLOOR != 0u;
#endif
	// 2. 산 칸의 몫: 창 이웃 몫(Weight), 제 값으로 읽는 열린 끝 몫(SelfShare), 얼린 이음매 몫의 합(Fixed).
	float4 Weight[SB_DENT_CELLS];
	float SelfShare[SB_DENT_CELLS];
	float Fixed[SB_DENT_CELLS];
	float CapCoarse[SB_DENT_CELLS];   // 바닥이 있는 거친 칸의 셀이 접히는 원천 상한 ÷ (1 + ΣShare)(cm, 가는 창과 같은 식)
	UNROLL
	for (int Link = 0; Link < SB_DENT_CELLS; ++Link)
	{
		Weight[Link] = float4(0, 0, 0, 0);
		SelfShare[Link] = 0.0;
		Fixed[Link] = 0.0;
		CapCoarse[Link] = 1e30;
		if (bLive[Link])
		{
			int2 W = int2(LocalId.x, LocalId.y + SB_DENT_ROWS * Link);
			int2 C = Origin + W;
			uint Slot = (uint)(W.y * SB_DENT_SIDE + W.x);
			uint4 Links = DragLinks.Load(int3(C, 0));
			float3 Here = SB_DentPositionGet(Slot).xyz;
			float4 There[4];
			float4 Across = float4(0, 0, 0, 0);
			float4 bWindow = float4(0, 0, 0, 0);
			float4 bWall = float4(0, 0, 0, 0);
			float4 RatioThere = float4(0, 0, 0, 0);   // 이웃 칸 재료 비²(0 = 제 값으로)
			UNROLL
			for (int k = 0; k < 4; ++k)
			{
				int2 Cell0;
				int2 Cell1;
				float Share;
				SB_DragLinkCells(Links[k], Cell0, Cell1, Share);
				int Offset = k == 0 ? 1 : (k == 1 ? -1 : (k == 2 ? SB_DENT_SIDE : -SB_DENT_SIDE));
				if (all(Cell0 == C))
				{
					There[k] = float4(Here, 0.0);
					bWall[k] = 1.0;
				}
				else if ((Links[k] & SB_DRAG_SEAM) != 0u || any(Cell0 != C + SB_DragDirection((uint)k)))
				{
					// 몫이 0이면 둘째 칸을 읽지 않는다 — 표면이 없는 칸일 수 있고, 그 칸의 DragInput 줄은 접촉 패스가 쓰지 않는다(갱신 안 임시).
					There[k] = float4(Share > 0.0 ? lerp(SB_CoarseSkin(Cell0), SB_CoarseSkin(Cell1), Share) : SB_CoarseSkin(Cell0), 1.0);
					Across[k] = Share > 0.0 ? lerp(SB_CoarseInput(Cell0), SB_CoarseInput(Cell1), Share) : SB_CoarseInput(Cell0);
					RatioThere[k] = Share > 0.0 ? lerp(SB_CoarseRatio(Cell0), SB_CoarseRatio(Cell1), Share) : SB_CoarseRatio(Cell0);
				}
				else
				{
					There[k] = SB_DentPositionGet((uint)((int)Slot + Offset));
					RatioThere[k] = SB_DentMaterialGet((uint)((int)Slot + Offset));
					bWindow[k] = 1.0;
				}
			}
			float4 Spacing = SB_NeighborSpacing(Here, There, CoarseTexelSize);
			float4 Layer = CoarseLayer.Load(int3(C, 0));
			float LayerCm = Layer.x * ThicknessScale;
			// 변마다 폭 = 두께 × √(두 칸 재료 비² 평균) — 가는 창과 같은 발산꼴(SB_EdgeRatio).
			float4 Reach = max(LayerCm * sqrt(SB_EdgeRatio(SB_DentMaterialGet(Slot), RatioThere)), 1e-3);
			float4 Share4 = Reach * Reach * SB_RowScale(Spacing) / Spacing;
			float SumShare = dot(Share4, float4(1, 1, 1, 1));
			float4 Part = Share4 / (1.0 + SumShare);
			Weight[Link] = Part * bWindow;
			SelfShare[Link] = dot(Part, bWall);
			Fixed[Link] = dot(Part * (1.0 - bWindow - bWall), Across);
			CapCoarse[Link] = Floor[Link] > 0.0 ? SB_CrushSource(Layer.w, LayerCm, SB_CoarseSkinLength(C)) / (1.0 + SumShare) : 1e30;
#if SB_HARD_SURFACE
			// 굽는 거친 칸: 용수철 세기를 줄 대각으로 나누고, 셀이 접히는 상한은 걸지 않는다(가는 창 굽는 칸과 같다)
			BendC[Link] = BendC[Link] / (1.0 + SumShare);
			CapCoarse[Link] = BendC[Link] > 0.0 ? 1e30 : CapCoarse[Link];
#endif
		}
	}
	// 3. 체비쇼프(가는 창과 같은 꼴, 범위는 거친 칸의 함몰 폭에서).
	if (!bClear)
	{
		float Sigma = DentCoarseTheta / DentCoarseHalf;
		float Rho = 1.0 / Sigma;
		LOOP
		for (uint Iteration = 0u; Iteration < SB_DENT_ITERATIONS; ++Iteration)
		{
			float Momentum = 0.0;
			float Step = 1.0 / DentCoarseTheta;
			if (Iteration > 0u)
			{
				float RhoNext = 1.0 / (2.0 * Sigma - Rho);
				Momentum = RhoNext * Rho;
				Step = 2.0 * RhoNext / DentCoarseHalf;
				Rho = RhoNext;
			}
			uint Read = (Iteration & 1u) * SB_DENT_AREA;
			uint Write = SB_DENT_AREA - Read;
			UNROLL
			for (int Sweep = 0; Sweep < SB_DENT_CELLS; ++Sweep)
			{
				if (bLive[Sweep])
				{
					uint Slot = (LocalId.y + SB_DENT_ROWS * (uint)Sweep) * SB_DENT_SIDE + LocalId.x;
					float4 Around = float4(SB_DentValueGet(Read + Slot + 1u), SB_DentValueGet(Read + Slot - 1u),
						SB_DentValueGet(Read + Slot + SB_DENT_SIDE), SB_DentValueGet(Read + Slot - SB_DENT_SIDE));
					float Self = Cur[Sweep];
					float Target = dot(Weight[Sweep], Around) + SelfShare[Sweep] * Self + Fixed[Sweep];
#if SB_HARD_SURFACE
					// 굽는 거친 칸: 용수철 k̂ 쪽 줄(가는 창 판·굽는 칸과 같은 꼴 — (J + k̂·목표)/(1 + k̂))
					Target = BendC[Sweep] > 0.0 ? (Target + BendC[Sweep] * AimC[Sweep]) / (1.0 + BendC[Sweep]) : Target;
#endif
					float Next = Self + Momentum * (Self - Prev[Sweep]) + Step * (Target - Self);
					// 바닥이 있는 칸은 바닥 아래로 가지 않되 원천이 셀이 접히는 상한을 넘지 않는다(가는 창과 같은 투영 — 상한은 이웃 가중 평균에 건다).
					float FloorHere = Floor[Sweep];
#if SB_HARD_SURFACE
					FloorHere = bNoFloor && NoPin[Sweep] ? -1e30 : FloorHere;
#endif
					float U = min(max(FloorHere, Next), Target + CapCoarse[Sweep]);
#if SB_HARD_SURFACE
					// 가는 창과 같다(굽는 칸은 0 위로도 가 반복 중에는 자르지 않는다 — 마지막 패스가 쓸 때 1 µm 안은 0)
					U = FloorHere <= 0.0 && BendC[Sweep] <= 0.0 && (bNoFloor ? abs(U) : U) < SB_REST_EPSILON ? 0.0 : U;
					U = TwoSided[Sweep] ? Floor[Sweep] : U;
#else
					U = Floor[Sweep] <= 0.0 && U < SB_REST_EPSILON ? 0.0 : U;   // 가는 창과 같다
#endif
					Prev[Sweep] = Self;
					Cur[Sweep] = U;
					SB_DentValueSet(Write + Slot, U);
				}
			}
			GroupMemoryBarrierWithGroupSync();
		}
	}
	// 4. 제 묶음 칸(창 줄·열 8~23)을 쓴다.
	UNROLL
	for (int Own = 1; Own <= 2; ++Own)
	{
		int2 W = int2(LocalId.x, LocalId.y + SB_DENT_ROWS * Own);
		int2 C = Origin + W;
		if (bListed && W.x >= SB_DENT_HALO && W.x < SB_DENT_HALO + SB_TILE && SB_InCoarse(C))
		{
			float Out = bOn[Own] && !bClear ? Cur[Own] : 0.0;
			if (DentCoarseLast != 0u)
			{
				Out = abs(Out) < SB_REST_EPSILON ? 0.0 : Out;
				DentCoarseFixOut[C] = bOn[Own] && !bClear ? Out - DentCoarseStart.Load(int3(C, 0)).x : 0.0;
				uint Busy = (Out != 0.0 ? SB_COARSE_BUSY_ANY : 0u) | (abs(Out) > SB_COARSE_TAU ? SB_COARSE_BUSY_WIDE : 0u);
				if (Busy != 0u)
				{
					InterlockedOr(SB_DENT_BUSY, Busy);
				}
			}
			DentCoarseOut[C] = Out;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (DentCoarseLast != 0u && bListed && LocalIndex == 0u)
	{
		uint Busy = SB_DENT_BUSY;
		DentCoarseBusyOut[Tile.y * SB_CoarseTilesPerSide() + Tile.x] = Busy;
		if (Busy != 0u)
		{
			InterlockedOr(TileAnyResidualOut[0], 1u);   // 거친 해가 남았다 — 다음 갱신에도 목록 패스가 돈다
		}
	}
}

// --- 거친 격자 공용 공유 메모리(끌림 바로잡기·모으기와 접촉 긴 몫 모으기가 같이 쓴다) ---
// 거친 칸 8×8 묶음(스레드 64개)이 제 칸들의 거리표 줄을 공유 메모리에 올리고, 원천을 64개씩 올려 묶음 칸 상자에서 닿는 것만 돈다.
// 공유 메모리 배치: 줄 값은 (랜드마크 쌍, 칸) 차례다 — 같은 랜드마크를 읽는 64개 스레드가 이웃 워드를 읽어 뱅크가 겹치지 않는다(일지 170).
#define SB_COARSE_CHUNK 64u   // 한 번에 공유 메모리에 올리는 원천 수(= 묶음 스레드 수)
#define SB_COARSE_CELLS (SB_COARSE_GROUP * SB_COARSE_GROUP)   // 묶음의 거친 칸 수 = 스레드 수
groupshared uint SB_CoarseRows[SB_LANDMARK_MAX / 2u * SB_COARSE_CELLS];   // [랜드마크 쌍][칸] 랜드마크 값 둘씩 uint 하나(16 KB)
groupshared uint SB_CoarseActiveCells;
groupshared uint SB_CoarseBoxLo[3];     // 셈할 칸 정지 위치의 상자(순서 보존 정수 SB_OrderedFloat)
groupshared uint SB_CoarseBoxHi[3];

uint SB_LandmarkRowValue(uint Local, uint M)
{
	uint Word = SB_CoarseRows[(M >> 1u) * SB_COARSE_CELLS + Local];
	return (M & 1u) != 0u ? (Word >> 16u) : (Word & 0xffffu);
}

// =====================================================================================================================
// 접촉 압력 (ADR-0037 §3.1 ①~④) — 머리 주석은 위 "접촉 압력 하나에서 함몰과 솟음을"
// =====================================================================================================================

// 단위 원판 위 구적점 31개(가운데 + 같은 넓이 고리 셋 — 6·10·14점, Tools/contact_response/replica.py DISC)의 반. 고리마다 0°에서 시작하는 짝수 점이라
// x축에 대해 짝인 두 점은 (Dist, 0)에서 거리가 같다 — y > 0인 점은 무게 z = 2, y = 0인 점은 무게 1로 19점(무게 합 31). 긴 몫 모으기의 가까운
// 원천에서 원판 평균을 낸다(SB_PressureLongDisc). 31점을 다 읽던 것과 평균이 같고 표 읽기는 19번이다(더하는 차례만 다르다, 일지 284).
static const float3 SB_PressureDiscHalf[19] =
{
	float3(0.000000, 0.000000, 1.0), float3(0.400000, 0.000000, 1.0), float3(0.200000, 0.346410, 2.0), float3(-0.200000, 0.346410, 2.0),
	float3(-0.400000, 0.000000, 1.0), float3(0.710000, 0.000000, 1.0), float3(0.574402, 0.417328, 2.0), float3(0.219402, 0.675250, 2.0),
	float3(-0.219402, 0.675250, 2.0), float3(-0.574402, 0.417328, 2.0), float3(-0.710000, 0.000000, 1.0), float3(0.920000, 0.000000, 1.0),
	float3(0.828891, 0.399173, 2.0), float3(0.573611, 0.719285, 2.0), float3(0.204719, 0.896934, 2.0), float3(-0.204719, 0.896934, 2.0),
	float3(-0.573611, 0.719285, 2.0), float3(-0.828891, 0.399173, 2.0), float3(-0.920000, 0.000000, 1.0)
};
/** 긴 몫 커널 ĝ_long(ρ; v, τ). */
float SB_PressureLong(float Rho, float Volume, float Tau)
{
	return SB_ResponseLong(ContactResponseLong, ContactResponseSampler, Rho, Volume, Tau);
}

/** 굽힘 전체 커널 ĝ_full(ρ; v, τ, δ)(연속 굽힘 — SB_ResponseFull, δ 매듭 둘을 따로 읽어 섞는다). */
float SB_PressureFull(float Rho, float Volume, float Tau, float Delta)
{
	return SB_ResponseFull(ContactResponseBendFull, ContactResponseSampler, Rho, Volume, Tau, Delta);
}

/** SB_PressureLongDisc와 같은 원판 31점 평균의 굽힘 전체 커널(가까운 원천 — 재현 replica.Replica.gather의 원판 평균과 짝). */
float SB_PressureFullDisc(float Dist, float Rad, float Hb, float Vb, float Tau, float Delta)
{
	float Sum = 0.0;
	UNROLL
	for (int k = 0; k < 19; ++k)
	{
		float3 U = SB_PressureDiscHalf[k];
		Sum += U.z * SB_PressureFull(length(float2(Dist - Rad * U.x, Rad * U.y)) / Hb, Vb, Tau, Delta);
	}
	return Sum * (1.0 / 31.0);
}

/** 반지름 Rad인 원판 위 31점(SB_PressureDiscHalf)에서 가운데로부터 Dist 떨어진 점까지의 긴 몫 커널 평균 ĝ_long(r/h̄) — 짝인 점은 한 번 읽어 두 번 센다. */
float SB_PressureLongDisc(float Dist, float Rad, float Hb, float Vb, float Tau)
{
	float Sum = 0.0;
	UNROLL
	for (int k = 0; k < 19; ++k)
	{
		float3 U = SB_PressureDiscHalf[k];
		Sum += U.z * SB_PressureLong(length(float2(Dist - Rad * U.x, Rad * U.y)) / Hb, Vb, Tau);
	}
	return Sum * (1.0 / 31.0);
}

/**
 * 칸 재료 → 성분 양 줄 다섯(반정밀 둘씩, 칸 기록 [7..11])과 칸 안 대각 몫(Local). 압력 칸 목록·하드 서피스 펼치기가 칸마다 한 번 부른다(표 Load 8 × 2M,
 * 일지 289). Layer = 살 두께 cm, V = 흐린 부피 보존, SkinCm = 흐린 겉껍질 장력 길이 cm, Area = 붙든 넓이 cm².
 * 칸 몫 나누기: a 몫 a_m 가운데 α_m은 칸 안에서만 작용하는 대각(α_m a_m / A), 나머지는 칸 가우스(분산 SB_PRESSURE_CELL_SPREAD·a²). α_m은 홀로 누른 칸의 제 칸
 * 값이 원판 정확값 Ψ(s/a)/A가 되고 고른 누름의 격자 합 1/A도 지키게 정한다 — 칸보다 가는 성분은 칸 안에서만 작용한다(가우스 칸 격자의 물결이 고른 누름을
 * 무르게 만들던 몫, 일지 289). 대각 양수를 더하는 것이라 양의 정부호 그대로다. b 몫(k² 꼴, 고른 누름 0)은 나누지 않는다.
 */
void SB_PressureAmountRows(float Layer, float V, float SkinCm, float Bend, float Area, out uint4 Rows[SB_PRESSURE_AMOUNT_WORDS / 4u], out float Local)
{
	float Amount[2 * SB_RESPONSE_AMOUNT_M];
#if SB_HARD_SURFACE
	// 겉 굽힘 길이 Bend(cm, 연속 굽힘): 0이면 SB_ResponseAmounts 그대로(흰색은 지금과 같다), 아니면 굽힘 매듭 둘을 섞은 양(SB_ResponseAmountsBend)
	SB_ResponseAmountsBend(ContactResponseAmounts, ContactResponseBendAmounts, Layer, V, SkinCm, Bend, Amount);
#else
	SB_ResponseAmounts(ContactResponseAmounts, Layer, V, SkinCm, Amount);
#endif
	float A = max(Area, 1e-6);
	float A2 = A / PI;
	float RadInv = rsqrt(A2);
	Local = 0.0;
	UNROLL
	for (uint m = 0u; m < SB_RESPONSE_AMOUNT_M; ++m)
	{
		float S = SB_ResponseAmountWidth[m];
		float SelfDisc = SB_ResponseDiscSelfAt(S * RadInv) / A;
		float SelfBlob = 1.0 / (4.0 * PI * (S * S + SB_PRESSURE_CELL_SPREAD * A2));
		float Alpha = saturate((SelfDisc - SelfBlob) / max(1.0 / A - SelfBlob, 1e-30));
		Local += Amount[m] * Alpha / A;
		Amount[m] *= 1.0 - Alpha;
	}
	UNROLL
	for (uint R = 0u; R < SB_PRESSURE_AMOUNT_WORDS / 4u; ++R)
	{
		uint W[4];
		UNROLL
		for (uint c = 0u; c < 4u; ++c)
		{
			uint K = (R * 4u + c) * 2u;
			W[c] = f32tof16(max(Amount[K], 0.0)) | (f32tof16(max(Amount[K + 1u], 0.0)) << 16u);
		}
		Rows[R] = uint4(W[0], W[1], W[2], W[3]);
	}
}

/** 성분 양 낱말에서 양 하나(K = 0..2M−1). */
float SB_PressureAmount(uint Words[SB_PRESSURE_AMOUNT_WORDS], uint K)
{
	uint Word = Words[K >> 1u];
	return f16tof32((K & 1u) != 0u ? (Word >> 16u) : Word);
}

/** 단위 벡터를 낱말 하나로: 8면체 좌표 둘을 반정밀로(각 오차 0.05° 안). 길이가 0이면 +z로 둔다(드묾 — 행렬 꼴은 단위 벡터면 무엇이든 성립한다). */
uint SB_PressurePackNormal(float3 N)
{
	float L1 = abs(N.x) + abs(N.y) + abs(N.z);
	N = L1 > 1e-6 ? N / L1 : float3(0, 0, 1);
	float2 P = N.xy;
	if (N.z < 0.0)
	{
		P = (1.0 - abs(N.yx)) * float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
	}
	return f32tof16(P.x) | (f32tof16(P.y) << 16u);
}

/** SB_PressurePackNormal을 되돌린다(단위 벡터). */
float3 SB_PressureUnpackNormal(uint Word)
{
	float2 P = float2(f16tof32(Word), f16tof32(Word >> 16u));
	float3 N = float3(P, 1.0 - abs(P.x) - abs(P.y));
	if (N.z < 0.0)
	{
		N.xy = (1.0 - abs(P.yx)) * float2(P.x >= 0.0 ? 1.0 : -1.0, P.y >= 0.0 ? 1.0 : -1.0);
	}
	return normalize(N);
}

/**
 * 거리표 줄 Row의 살 조각 번호(F10, motive-mem ②): 같은 조각 문(압력 행렬 SB_PressureEntry, 긴 몫 모으기 ContactLongGatherCS)이 견주는 번호.
 * 판 6 표는 굽기가 줄 끝에 적은 번호를 읽는다 — 거리를 재는 살 그래프의 이어진 덩어리마다 하나라, 번호가 같으면 두 줄 사이에 표면 거리가 있다.
 * 옛 표(판 4·5)는 실행 중에 유도한다: 거리가 적힌(닿는) 첫 랜드마크의 번호. 굽기가 아직 닿지 않은 조각부터 랜드마크를 뽑아 그런 조각끼리는 갈리지만,
 * 랜드마크가 없는 작은 조각(거친 칸 4개 미만)은 모두 0xffff 하나로 묶인다. 판은 줄 길이로 가린다(판 4: L + 16, 판 5: L + 17, 판 6: L + 18).
 */
uint SB_PressurePiece(uint Row)
{
	uint Piece = 0xffffu;
	if (LandmarkRowTexels > NumLandmarks + SB_LANDMARK_NEAREST + 1u)
	{
		Piece = SB_LandmarkRaw(Row, NumLandmarks + SB_LANDMARK_NEAREST + 1u);   // 판 6: b 다음 텍셀(C++ SoftbodyBulgeTable::PieceOffset)
	}
	else
	{
		for (uint M = 0u; M < NumLandmarks; ++M)
		{
			if (SB_LandmarkValue(Row, M) != SB_LANDMARK_UNREACHABLE)
			{
				Piece = M;
				break;
			}
		}
	}
	return Piece;
}

/**
 * 압력 칸 i·j 사이 처짐/힘 K(i, j)(cm per μ·cm², 일지 289 — Codex 검토 일지 287의 1위 안). 칸마다 재료(두께·v·겉껍질 길이)의 성분 양 u를 표에서 읽어
 * 두고(두께에 따라 늘기만 한다), 쌍은 성분마다 두 칸 가운데 작은 양 × 성분 커널. 칸 하나는 분산 v = s_m² + σ²의 가우스 φ다(σ² =
 * SB_PRESSURE_CELL_SPREAD·a², a² = A/π, 칸보다 가는 몫은 칸 안 대각으로 — SB_PressureAmountRows). F10에서 두 몫을 칸마다 정한 함수의 내적으로 바꿨다:
 *  - G = φ_i·φ_j(3-D): e^{−d²/4t}/(4πt) × (v_i v_j)^¼/√t, t = (v_i + v_j)/2, d = 두 칸 정지 위치의 직선(3-D 벡터). 넓이가 같으면 끝 곱이 1이라 옛 G와
 *    같고, 넓이가 달라도 성분 폭 0.5 cm 이상에서는 0.3 % 안으로 작아질 뿐이다.
 *  - H = 미는 방향의 접면에 내린 기울기(P∇φ, P = I − nnᵀ)끼리의 내적: (G/t)(tr(P_iP_j)/2 − dᵀP_iP_jd/4t). 같은 평면 위 칸끼리는 옛 H =
 *    (G/t)(1 − d²/4t) 그대로다. 옛 H를 3-D 직선에 쓰면 1~2 cm 떨어져 겹친 두 겉면에서 행렬이 음의 방향을 얻는다(F10 검토: λmin/λmax −0.18).
 * min(u_i, u_j)·G·H 모두 내적 꼴이라 행렬이 어떤 배치·두께·재료에서도 양의 정부호다(SB_PressureEntry가 곱하는 두 문도 같은 꼴). 같은 재료면 min(h_i, h_j)
 * 규칙의 커널과 같다. 옛 짧은·긴 몫 행렬은 원판 근사·쌍 재료 평균이 이 성질을 깨 묶은 단계에서 풀이가 터졌다(일지 287).
 */
float SB_PressureKernelAmounts(float3 D, float3 Ni, float3 Nj, float Ai, float Aj, uint AmountI[SB_PRESSURE_AMOUNT_WORDS], uint AmountJ[SB_PRESSURE_AMOUNT_WORDS])
{
	float D2 = dot(D, D);
	float DotI = dot(D, Ni);
	float DotJ = dot(D, Nj);
	float NN = dot(Ni, Nj);
	float Flat2 = D2 - DotI * DotI - DotJ * DotJ + DotI * NN * DotJ;   // dᵀP_iP_jd: 두 접면에 내린 거리²
	float Trace = 0.5 * (1.0 + NN * NN);                               // tr(P_iP_j)/2 — 같은 면이면 1
	float SigI = SB_PRESSURE_CELL_SPREAD * Ai / PI;                    // σ_i²
	float SigJ = SB_PRESSURE_CELL_SPREAD * Aj / PI;
	float Sum = 0.0;
	UNROLL
	for (uint m = 0u; m < SB_RESPONSE_AMOUNT_M; ++m)
	{
		float S2 = SB_ResponseAmountWidth[m] * SB_ResponseAmountWidth[m];
		float Vi = S2 + SigI;
		float Vj = S2 + SigJ;
		float InvT = 2.0 / (Vi + Vj);
		float G = exp(-0.25 * D2 * InvT) * InvT * (0.25 / PI) * sqrt(sqrt(Vi * Vj) * InvT);
		float Ua = min(SB_PressureAmount(AmountI, m), SB_PressureAmount(AmountJ, m));
		float Ub = min(SB_PressureAmount(AmountI, SB_RESPONSE_AMOUNT_M + m), SB_PressureAmount(AmountJ, SB_RESPONSE_AMOUNT_M + m));
		Sum += G * (Ua + Ub * S2 * InvT * (Trace - 0.25 * Flat2 * InvT));
	}
	return Sum;
}

/** 단계 Level의 압력 칸 Slot의 줄 R. */
uint SB_PressureIndex(uint Level, uint Slot, uint R)
{
	return (Level * SB_PRESSURE_CAP + Slot) * SB_PRESSURE_ROWS + R;
}

#if SB_HARD_SURFACE
// =====================================================================================================================
// 하드 서피스 압력 풀이(ADR-0037 §3.7, ADR-0038 §4·§4.2, 일지 298, 배치 3 판 자유도 꼴) — 이 순열에만 있다(원칙 E06). 재현 Tools/contact_response/
// replica_plate.py의 run_b3(emit_rows_b3·solve_b3·draw_b3)와 같은 식·같은 차례다(짝 L03).
//  - 조각 종류(실행 중, SB_HardKindOf): 붙은 강체(가장 연한 칠의 굽힘 길이 ≥ RigidPieceRatio × 조각 반지름, 검정은 늘), 떠 있는 장식(늘 강체, 얹힘 —
//    받침 무게 중심에 가장 가까운 받침 줄 하나만 옷에 붙어 당길 수 있다, SB_HardCore). 그 밖의 칠한 조각은 판이 아니라 칸 칠대로 굽는 겉이다(연속
//    굽힘 — 후보가 아니다).
//  - 행렬 줄(K): 조각 밖 압력 칸 O(0 .. N_O − 1) 뒤에 후보 조각의 발자국 거친 칸 F(단계 0, 굽기 목록 차례)가 보통 압력 칸으로 이어진다(N_K = N_O + ΣF).
//  - 풀이 줄(스레드 차례): 조각 밖 칸 힘 f_O(≥ 0, 셀 접힘 상한) · 발자국 받침 λ_F(제 행렬 줄 스레드 — 붙은 판은 부호 없음, 떠 있는 판은 ≥ 0) ·
//    조각 위 손 칸 힘 f_k(≥ 0, N_K부터 — 손이 든 발자국 거친 칸마다 한 줄, 깊은 SB_HARD_HAND_CAP줄).
//    강체 조각은 평형 Σ_F φ_F λ_F = Σ_k φ_k f_k(φ = 1, 판 x, 판 y — 받침 무게 중심에서, 선 모양은 긴 축만)를 투영으로 늘 지킨다. 자세(가라앉음·
//    기울기) = 그 투영의 곱수. 강체 손 칸 줄은 ½ε c_k f_k²로 정칙화한다(c_k = 1차식으로 편 하중의 살 무름 C3 — 손이 판에 그만큼 덜 들어간다).
//  - 풀이: 투영 앞처리 CG(앞처리 = 제 대각). 걸음은 첫 경계에서 자르고 그 줄을 묶는다 — 바퀴 끝에 음수 힘을 0으로 자르지 않는다(배치 3: 자르면 판
//    평형이 깨져 예산이 다할 때 튀었다). 모든 반복이 제약 안이라 예산이 다해도 "조금 덜 눌림, 튐 없음". 바퀴 시작에 손 칸 줄 접촉 점을 다시 고르고,
//    강체 평형을 되살리고, 곱수 뺀 기울기로 묶인 줄을 푼다.
//  - 꼬리: 판 = 자세 평면(HardPieceMotion — 붙은 강체 w 2, 떠 있음 w 3), 가는 칸은 칸 쉼 자리의 판 좌표에서 읽는다(SB_HardPlateAt).
// 짝(L03): C++ HardCandidates·HardHandCapacity(= SB_HARD_HAND_EMIT)·HardWorkWords·HardHandRows, 재현 replica_hard.py의 PICK_DELTA,
// replica_plate.py의 HAND_EMIT·HAND_CAP·HAND_EPS·RANK_RATIO·RANK_TINY·INV_CUT·PREFIX_CAP·RESTORE_CAP·CG_STOP.
#define SB_HARD_CANDIDATES 32u
#define SB_HARD_HAND_EMIT 512u                     // 압력 칸 패스가 내보내는 손 칸 줄 상한(C++ HardHandCapacity) — 넘친 줄은 빠진다(넘침 비트 1)
#define SB_HARD_HAND_CAP 256u                      // 풀이가 받는 손 칸 줄 — 깊은 줄부터, 나머지는 누르지 않는다(덜 눌림, 판 밖 살 줄로 되돌리지 않음)
#define SB_HARD_SAMPLES 16u                        // 거친 칸 가는 칸 수 상한(CoarseBlock ≤ 4)
#define SB_HARD_HAND_ROWS 17u                      // 조각 위 손 칸 한 줄의 float4 수: 머리 + 표본
#define SB_HARD_PICK_DELTA 0.05                    // cm — 접촉 점 무게의 깊이 폭
#define SB_HARD_PICK_SETTLE 1e-4                   // cm — 다시 고른 목표가 이보다 더 바뀌면 그 바퀴에서 끝내지 않는다
// 판 줄이 있는 풀이의 바퀴·CG 걸음 상한 배수(일지 291). 재현에서 센 큰 판(10 cm, S ≤ 0.3) 가장자리 누름은 6 × 4면 끝까지 푼 해와 4~7 %, 12 × 8이면
// 1~2.8 % 다르다(plate_report.md §5.4). 판 줄이 없는 풀이(칠하지 않은 자산·조각에 닿지 않은 갱신)는 그대로다.
#define SB_HARD_ROUND_SCALE 2u
#define SB_HARD_STEP_SCALE 2u
// 강체 판(배치 3, 재현 replica_plate.py와 같은 값)
#define SB_HARD_HAND_EPS 0.03                      // 강체 손 칸 줄 정칙화 ε(손이 판에 c_k·ε·f만큼 덜 들어간다)
#define SB_HARD_RANK_RATIO 0.01                    // 받침 분산의 작은 축 ÷ 큰 축이 이보다 작으면 선 모양(긴 축 기울기만)
#define SB_HARD_RANK_TINY 1e-6                     // cm² — 받침 분산이 이보다 작으면 한 점(가라앉음만)
#define SB_HARD_INV_CUT 1e-5                       // 투영 2 × 2 분산의 작은 고윳값이 큰 것의 이 몫보다 작으면 그 축은 풀지 않는다(자유 줄이 한 줄 위)
#define SB_HARD_PREFIX_CAP 4u                      // 경계에 있고 투영 방향이 경계 밖인 줄 묶기 되풀이 상한(남은 줄은 길이 0 걸음이 묶는다)
#define SB_HARD_RESTORE_CAP 3u                     // 바퀴 시작 평형 되살리기 되풀이 상한
#define SB_HARD_CG_STOP 1e-8                       // rᵀz가 바퀴 처음의 이 몫 밑이면 그 바퀴 CG를 멈춘다(더 돌면 셈 오차가 평형 밖으로 샌다)
// 일 버퍼(HardWork) 낱말
#define SB_HARD_WORK_HANDS 0u          // 조각 위 손 칸 수(압력 칸 패스가 센다)
#define SB_HARD_WORK_CANDIDATES 1u     // 후보 조각 수 T
#define SB_HARD_WORK_TOUCHED 2u        // 닿은 후보 수 — 후보 앞쪽이다
#define SB_HARD_WORK_FROWS 3u          // 발자국 줄 수 ΣF(후보 합)
#define SB_HARD_WORK_ROWS 4u           // 풀이가 받는 조각 위 손 칸 수
#define SB_HARD_WORK_OVERFLOW 5u       // 넘침 비트(1 손 칸, 2 후보, 4 닿은 조각이 줄 상한에 못 듦)
#define SB_HARD_WORK_FREE 6u           // 조각 밖 압력 칸 수 N_O — 발자국 줄이 이 자리부터
// 낱말 7은 비어 있다(옛 판 모드 줄 수)
#define SB_HARD_WORK_BITS 8u           // 닿은 조각 비트(조각 4096개 → 128 낱말)
#define SB_HARD_WORK_PIECE 136u        // 후보 → 조각 번호(0부터)
#define SB_HARD_WORK_START 168u        // 후보 → 첫 발자국 줄(N_O에서 셈)
#define SB_HARD_WORK_COUNT 200u        // 후보 → 발자국 거친 칸 수
#define SB_HARD_WORK_WORDS 232u        // C++ HardWorkWords

RWBuffer<uint> HardWorkOut;                    // 압력 칸·인자 패스
Buffer<uint> HardWork;                         // 펼치기·행렬·풀이·모으기
RWStructuredBuffer<float4> HardHandOut;        // 조각 위 손 칸 [줄][머리 + 표본]: 머리 = (조각, 거친 칸 x + y × 4096, 표본 수, 가장 깊은 표본 깊이 cm)
StructuredBuffer<float4> HardHand;
StructuredBuffer<uint2> HardPieceCellRange;    // 조각마다 발자국 거친 칸 목록 구간(첫 자리, 수)
StructuredBuffer<uint> HardPieceCellList;      // 발자국 거친 칸(x | y << 16)
// 판 굽기(ADR-0038 §4 — 굽기 USoftbodyMeshData::HardCellPlate, 메시 로컬 단위). 발자국 칸 번호는 전역(HardPieceCellRange.x + 조각 안 칸).
StructuredBuffer<float4> HardCellPlate;        // 발자국 칸마다 (판 x, 판 y, 넓이, 0)
RWStructuredBuffer<float4> FRowOut;            // 펼치기: 발자국 줄마다 (판 x cm, 판 y cm, asfloat(조각 안 칸 | 후보 << 16), 넓이 cm²)
StructuredBuffer<float4> FRow;
RWTexture2D<float2> TiedPressureOut;           // 풀이 꼬리: 발자국 거친 칸의 (판이 편 하중 λ_F, 넓이) — 가는 창 압력
Texture2D<float2> TiedPressure;
RWBuffer<uint> HardArgsOut;                    // 인자 패스: 펼치기 간접 인자

#endif

#if SB_HARD_SURFACE
/**
 * 거친 칸 Cell의 조각 Piece(0부터) 위 붙든 가는 칸을 조각 위 손 칸 한 줄로 쓴다(표본마다 판 좌표와 목표 깊이 g = P − s·a — 압력 칸과 같은 목표). 닿은 조각
 * 비트를 켜고, 압력 칸 상자·두께에 이 줄의 무게 중심을 넣는다(후보 조각 거리와 모으기 상자). 머리 w = 가장 깊은 표본 깊이 — 풀이가 깊은 줄부터 받는다.
 * 내보내기 상한(SB_HARD_HAND_EMIT)을 넘으면 넘침 비트를 켜고 false — 그 줄은 빠진다(판 밖 살 줄로 되돌리지 않는다, 배치 3: 되돌리면 판 발자국이 살처럼
 * 눌려 판이 찌그러졌다). 머리의 정수(조각·거친 칸·표본 수)는 float 값으로 둔다(비트로 두면 비정규 수라 0으로 깎일 수 있다).
 */
bool SB_HardEmitHand(int2 Cell, int2 Base, uint Piece)
{
	uint Slot;
	InterlockedAdd(HardWorkOut[SB_HARD_WORK_HANDS], 1u, Slot);
	if (Slot >= SB_HARD_HAND_EMIT)
	{
		InterlockedOr(HardWorkOut[SB_HARD_WORK_OVERFLOW], 1u);
		return false;
	}
	InterlockedOr(HardWorkOut[SB_HARD_WORK_BITS + (Piece >> 5u)], 1u << (Piece & 31u));
	uint Count = 0u;
	float3 Sum = float3(0, 0, 0);
	float Layer = 0.0;
	float Deepest = 0.0;
	for (uint Y = 0u; Y < CoarseBlock; ++Y)
	{
		for (uint X = 0u; X < CoarseBlock; ++X)
		{
			int2 T = Base + int2(X, Y);
			if (any(T >= int2(Resolution, Resolution)) || SB_HardPieceAt(T) != Piece + 1u)
			{
				continue;
			}
			float4 Rest = CarrierPosition.Load(int3(T, 0));
			float Thickness = CarrierNormal.Load(int3(T, 0)).w;
			float P = SB_PressurePinNow(T);
			if ((uint)round(Rest.w) != SB_KIND_SURFACE || Thickness <= 0.0 || P <= 0.0 || Count >= SB_HARD_SAMPLES)
			{
				continue;
			}
			float Depth = max(P - PlasticShare * AcceptedOut[T], 0.0);
			HardHandOut[Slot * SB_HARD_HAND_ROWS + 1u + Count] = float4(SB_HardPlateXY(Piece, Rest.xyz), Depth, 0.0);
			Deepest = max(Deepest, Depth);
			Sum += Rest.xyz;
			Layer += Thickness * ThicknessScale;
			++Count;
		}
	}
	float Inv = 1.0 / max((float)Count, 1.0);
	HardHandOut[Slot * SB_HARD_HAND_ROWS] = float4((float)Piece, (float)((uint)Cell.x + (uint)Cell.y * 4096u), (float)Count, Deepest);
	if (Count > 0u)
	{
		float3 Center = Sum * Inv;
		UNROLL
		for (uint Axis = 0; Axis < 3u; ++Axis)
		{
			InterlockedMin(PressureHeaderOut[SB_SOURCE_HEADER_MIN + Axis], SB_OrderedFloat(Center[Axis]));
			InterlockedMax(PressureHeaderOut[SB_SOURCE_HEADER_MAX + Axis], SB_OrderedFloat(Center[Axis]));
		}
		InterlockedMax(PressureHeaderOut[SB_SOURCE_HEADER_SIGMA], (uint)(Layer * Inv * 1000.0));
	}
	return true;
}
#endif

groupshared float4 SB_PressureSumA[SB_TILE * SB_TILE];   // 거친 칸: xyz 붙든 칸 정지 위치 합(메시 로컬), w 붙든 칸 수
groupshared float4 SB_PressureSumB[SB_TILE * SB_TILE];   // x 붙든 칸 두께 합(cm), y 목표 합(cm), z 붙든 넓이(cm²), w 살 칸 수
groupshared float3 SB_PressureSumN[SB_TILE * SB_TILE];  // 거친 칸: 붙든 칸 미는 방향 합(메시 로컬, F10 — 행렬의 접면 꼴·마주봄 문)
groupshared uint SB_PressureSlots[SB_TILE * SB_TILE];    // 거친 칸의 압력 칸 번호(단계 0 | 1 << 10 | 2 << 20)
groupshared float SB_PressureSumG[SB_TILE * SB_TILE];    // 잡기(ADR-0041): 거친 칸의 잡힌 가는 칸 손끝 세기 합 Σκ(0 = 잡기 줄 아님)
// 잡기 줄로 셀 가는 칸의 손끝 세기 하한(원 가장자리 칸은 붙드는 힘이 0에 가깝다 — 그 칸 하나로 거친 칸 전체를 잡기 줄로 만들지 않는다)
#define SB_GRAB_ROW_GRIP 1e-3
// 잡기 줄 대각에 더하는 손끝 무름의 상한(cm per μ·cm²) — 원 가장자리 거친 칸이 반정밀 행렬을 넘치지 않게(그만큼이면 이미 힘이 없는 칸이다)
#define SB_GRAB_ROW_COMPLIANCE_MAX 1e4
#if SB_SEPARATION
groupshared float2 SB_PressureSumS[SB_TILE * SB_TILE];   // 부위 나누기: 거친 칸의 붙든 칸 두 부위 몫 합
#endif

/**
 * 함몰 목록 묶음(가는 16×16 = 거친 Side×Side, Side = 16/B)마다 거친 칸의 붙든 칸을 모아 압력 칸 목록에 넣는다(단계 0·1·2 모두 — 넘치는 단계는
 * ContactPressureArgsCS가 고르지 않는다). 붙든 칸 = 지금 손이 닿은 칸의 붙들 깊이 P > 0(SB_PressurePinNow — 손이 떠난 기억 칸은 압력을 내지 않는다).
 * 목표 g = P − s·a의 평균(s = PlasticShare — 받아들인 깊이의 소성 몫은 압력 없이 모양만 바뀐 것). 넓이 = 거친 칸 살 넓이 × 붙든 칸 몫.
 */
[numthreads(SB_TILE, SB_TILE, 1)]
void ContactPressureCellsCS(uint3 GroupId : SV_GroupID, uint LocalIndex : SV_GroupIndex)
{
	// 묶음 전체가 같은 값(SRV)이라 장벽 앞 갈래가 묶음 안에서 갈리지 않는다(X3663).
	uint Entry = GroupId.y * SB_TILE_ROW + GroupId.x;
	bool bListed = Entry < DentTileCount[0];
	uint Packed = bListed ? DentTiles[Entry] : 0u;
	uint2 Tile = uint2(Packed & 0xffffu, Packed >> 16u);
	uint Side = SB_TILE / CoarseBlock;
	int2 CoarseOrigin = int2(Tile * Side);
	float4 SumA = float4(0, 0, 0, 0);
	float4 SumB = float4(0, 0, 0, 0);
	float3 SumN = float3(0, 0, 0);   // 붙든 칸 미는 방향 합(F10)
	float SumG = 0.0;                // 잡기(ADR-0041): 잡힌 가는 칸 손끝 세기 합
#if SB_SEPARATION
	float2 SumS = float2(0, 0);
#endif
	if (bListed && LocalIndex < Side * Side)
	{
		int2 Cell = CoarseOrigin + int2(LocalIndex % Side, LocalIndex / Side);
		if (SB_InCoarse(Cell))
		{
			int2 Base = Cell * (int)CoarseBlock;
#if SB_HARD_SURFACE
			uint HardPiece = 0u;   // 이 거친 칸의 조각 위 붙든 칸 조각 번호 + 1(0 = 없음)
#endif
			// 잡기(ADR-0041): 잡힌 가는 칸 모음 — 하나라도 있으면 이 거친 칸은 잡기 줄이다(누른 칸은 가는 창에서만 붙든다)
			float4 GrabA = float4(0, 0, 0, 0);
			float3 GrabN = float3(0, 0, 0);
			float GrabLayer = 0.0;
			float GrabAim = 0.0;    // Σ κ·P_g
			float GrabGrip = 0.0;   // Σ κ
#if SB_SEPARATION
			float2 GrabS = float2(0, 0);   // 잡힌 칸 두 부위 몫 합(부위 나누기 — 잡기 줄의 긴 몫도 다른 부위로 건너지 않게)
#endif
			for (uint Y = 0u; Y < CoarseBlock; ++Y)
			{
				for (uint X = 0u; X < CoarseBlock; ++X)
				{
					int2 T = Base + int2(X, Y);
					if (any(T >= int2(Resolution, Resolution)))
					{
						continue;
					}
					float4 Rest = CarrierPosition.Load(int3(T, 0));
					float4 Push = CarrierNormal.Load(int3(T, 0));   // xyz 미는 방향, w 두께
					float Thickness = Push.w;
					if ((uint)round(Rest.w) != SB_KIND_SURFACE || Thickness <= 0.0)
					{
						continue;
					}
					SumB.w += 1.0;
					// 잡기(ADR-0041): 원 안 칸은 잡기 줄의 몫 — 목표 P_g = −D·n(바깥이 음수), 손끝 세기 κ 무게. 콜라이더 누름은 접촉 패스가 지웠다.
					BRANCH
					if (GrabCount != 0u)
					{
						float3 GrabPull;
						float Kappa = SB_GrabGrip(Rest.xyz, Push.xyz * rsqrt(max(dot(Push.xyz, Push.xyz), 1e-12)), GrabPull);
						if (Kappa >= SB_GRAB_ROW_GRIP)
						{
							GrabA += float4(Rest.xyz, 1.0);
							GrabN += Push.xyz;
							GrabLayer += Thickness * ThicknessScale;
							GrabAim += Kappa * SB_GrabAimLimit(-dot(GrabPull, ContactIn.Load(int3(T, 0)).xyz), Thickness * ThicknessScale);
							GrabGrip += Kappa;
#if SB_SEPARATION
							GrabS += SB_SeparationAt(T);
#endif
							continue;
						}
					}
					float P = SB_PressurePinNow(T);
					if (P > 0.0)
					{
#if SB_HARD_SURFACE
						// 조각 위 붙든 칸은 압력 칸에 넣지 않는다 — 아래 조각 위 손 칸 줄이 받는다(판이 받아 발자국에 편다).
						// 한 거친 칸을 두 조각이 나누면(굽기 경고) 처음 만난 조각만 줄로 받고 다른 조각 칸은 조각 밖 칸으로 둔다.
						// 굽는 조각(SB_HardKindOf 0 — 크기에 비해 칠이 연한 조각)은 판이 아니다: 그 칸은 보통 압력 칸이고 칸 칠로 굽는다(연속 굽힘).
						uint OnPiece = SB_HardPieceAt(T);
						if (OnPiece != 0u && OnPiece <= HardPieceCount && (HardPiece == 0u || HardPiece == OnPiece) && SB_HardKindOf(OnPiece - 1u) != 0u)
						{
							HardPiece = OnPiece;
							continue;
						}
#endif
						SumA += float4(Rest.xyz, 1.0);
						SumN += Push.xyz;
						SumB.x += Thickness * ThicknessScale;
						SumB.y += max(P - PlasticShare * AcceptedOut[T], 0.0);
#if SB_SEPARATION
						SumS += SB_SeparationAt(T);
#endif
					}
				}
			}
			// 잡기 줄: 누른 칸 몫을 버리고 잡힌 칸으로 — 목표는 손끝 세기 무게 평균 P_g(단계 합이 칸 수 무게 평균이라 목표 × 칸 수로 둔다)
			if (GrabA.w > 0.0)
			{
				SumA = GrabA;
				SumN = GrabN;
				SumB.x = GrabLayer;
				SumB.y = GrabAim / GrabGrip * GrabA.w;
#if SB_SEPARATION
				SumS = GrabS;
#endif
			}
			SumG = GrabA.w > 0.0 ? GrabGrip : 0.0;
			float4 Info = CoarseInfo.Load(int3(Cell, 0));
			SumB.z = SumB.w > 0.0 ? Info.w * ThicknessScale * ThicknessScale * SumA.w / SumB.w : 0.0;
#if SB_HARD_SURFACE
			// 조각 위 손 칸 줄. 넘치면 그 줄은 빠진다 — 판 밖 살 줄로 되돌리지 않는다(배치 3: 되돌리면 판 발자국이 살처럼 눌려 판이 찌그러졌다).
			if (HardPiece != 0u)
			{
				SB_HardEmitHand(Cell, Base, HardPiece - 1u);
			}
#endif
		}
	}
	SB_PressureSumA[LocalIndex] = SumA;
	SB_PressureSumB[LocalIndex] = SumB;
	SB_PressureSumN[LocalIndex] = SumN;
	SB_PressureSumG[LocalIndex] = SumG;
#if SB_SEPARATION
	SB_PressureSumS[LocalIndex] = SumS;
#endif
	SB_PressureSlots[LocalIndex] = SB_PRESSURE_NO_SLOT | (SB_PRESSURE_NO_SLOT << 10u) | (SB_PRESSURE_NO_SLOT << 20u);
	GroupMemoryBarrierWithGroupSync();
#if SB_HARD_SURFACE
	// Flex 순열: 굽힘 성분 양 읽기(SB_ResponseAmountsBend)가 커서 단계 셋을 펼치면 FXC가 X3511로 멈춘다 — 펼치지 않고 돈다(단계 수가 상수라 장벽이 균일한 흐름에 있다)
	LOOP
#else
	UNROLL
#endif
	for (uint Level = 0u; Level < SB_PRESSURE_LEVELS; ++Level)
	{
		uint BlockSide = 1u << Level;
		uint Blocks = Side >> Level;
		if (bListed && LocalIndex < Blocks * Blocks)
		{
			uint2 Corner = uint2(LocalIndex % Blocks, LocalIndex / Blocks) * BlockSide;
			float4 A = float4(0, 0, 0, 0);
			float4 B = float4(0, 0, 0, 0);
			float3 NSum = float3(0, 0, 0);
#if SB_SEPARATION
			float2 Shares = float2(0, 0);
#endif
			float2 Material = float2(0, 0);
			float MaterialBend = 0.0;   // 넓이 × 흐린 겉 굽힘 길이(연속 굽힘 — Flex 순열만 읽는다)
			float Grip = 0.0;           // 잡기(ADR-0041): 잡힌 가는 칸 손끝 세기 합 — 0이 아니면 이 압력 칸은 잡기 줄
			for (uint MY = 0u; MY < BlockSide; ++MY)
			{
				for (uint MX = 0u; MX < BlockSide; ++MX)
				{
					uint Local = (Corner.y + MY) * Side + Corner.x + MX;
					float4 MA = SB_PressureSumA[Local];
					if (MA.w > 0.0)
					{
						float4 MB = SB_PressureSumB[Local];
						A += MA;
						B.xyz += MB.xyz;
						NSum += SB_PressureSumN[Local];
						Grip += SB_PressureSumG[Local];
#if SB_SEPARATION
						Shares += SB_PressureSumS[Local];
#endif
						int2 Cell = CoarseOrigin + int2(Corner + uint2(MX, MY));
						Material += MB.z * CoarseMaterial.Load(int3(Cell, 0));
#if SB_HARD_SURFACE
						MaterialBend += MB.z * CoarseBend.Load(int3(Cell, 0));
#endif
					}
				}
			}
			if (A.w > 0.0)
			{
				float3 Center = A.xyz / A.w;
				// 대표 거친 칸: 무게 중심에 가장 가까운 칸(거리표 줄을 읽는다 — 가장 무거운 칸으로 하면 하한이 그 칸에 묶인다, 일지 168)
				float Best = 1e30;
				int2 Rep = CoarseOrigin + int2(Corner);
				for (uint RY = 0u; RY < BlockSide; ++RY)
				{
					for (uint RX = 0u; RX < BlockSide; ++RX)
					{
						uint Local = (Corner.y + RY) * Side + Corner.x + RX;
						if (SB_PressureSumA[Local].w > 0.0)
						{
							int2 Cell = CoarseOrigin + int2(Corner + uint2(RX, RY));
							float3 Away = CoarseInfo.Load(int3(Cell, 0)).xyz - Center;
							float D = dot(Away, Away);
							if (D < Best)
							{
								Best = D;
								Rep = Cell;
							}
						}
					}
				}
				uint Row = CoarseRow.Load(int3(Rep, 0));
				if (Row != SB_LANDMARK_NO_ROW)
				{
					uint CountWord = Level == 0u ? SB_SOURCE_HEADER_COUNT : (Level == 1u ? SB_PRESSURE_HEADER_LEVEL1 : SB_PRESSURE_HEADER_LEVEL2);
					uint Slot;
					InterlockedAdd(PressureHeaderOut[CountWord], 1u, Slot);
					float Layer = B.x / A.w;
					if (Level == 0u)
					{
						// 모으기 상자는 붙든 칸 전부로 — 단계 0이 넘쳐 거친 단계를 고르면 상한 밖 칸 둘레도 받아야 한다
						UNROLL
						for (uint Axis = 0; Axis < 3u; ++Axis)
						{
							InterlockedMin(PressureHeaderOut[SB_SOURCE_HEADER_MIN + Axis], SB_OrderedFloat(Center[Axis]));
							InterlockedMax(PressureHeaderOut[SB_SOURCE_HEADER_MAX + Axis], SB_OrderedFloat(Center[Axis]));
						}
						InterlockedMax(PressureHeaderOut[SB_SOURCE_HEADER_SIGMA], (uint)(Layer * 1000.0));
					}
					if (Slot < SB_PRESSURE_CAP)
					{
						float Area = B.z;
						float2 Mat = Area > 0.0 ? Material / Area : float2(1.0, 0.0);
						float SkinCm = Mat.y * ThicknessScale;
						float Bend = Area > 0.0 ? MaterialBend / Area : 0.0;   // 압력 칸 겉 굽힘 길이(cm — 조각 없는 순열은 0)
						float Crush = SB_ResponseCrush(Mat.x);
						float ForceMax = Crush > 1e20 ? 1e30 : Crush * Area;
						// 잡기 줄(ADR-0041): 힘 상한 자리에 손끝 무름의 음수(늘 활성·부호 자유·상한 없음 — 풀이·행렬이 이 부호로 가른다). 가는 칸 κ는 함몰 창의 제 몫
						// 단위라 물리 세기는 κ/(h·A₁)(μ/cm — 가는 창 압축 몫 γ·h·A₁·p와 같은 눈금), 칸 넓이를 곱해 더한 것의 역수가 무름.
						if (Grip > 0.0)
						{
							float3 GripWindow = SB_ResponseWindow(Mat.x, SkinCm / max(Layer, 1e-3));
							float GripStiffness = Grip * (Area / A.w) / max(Layer * GripWindow.z, 1e-6);
							ForceMax = -clamp(1.0 / max(GripStiffness, 1e-12), 1e-20, SB_GRAB_ROW_COMPLIANCE_MAX);
						}
						uint Nearest[SB_LANDMARK_NEAREST];
						UNROLL
						for (uint k = 0; k < SB_LANDMARK_NEAREST; ++k)
						{
							Nearest[k] = SB_LandmarkNearest(Row, k);
						}
						UNROLL
						for (uint k2 = 0; k2 < SB_LANDMARK_NEAREST; ++k2)
						{
							Nearest[k2] = (SB_LandmarkValue(Row, Nearest[k2]) << 8u) | Nearest[k2];
						}
						uint Base7 = SB_PressureIndex(Level, Slot, 0u);
						PressureCellsOut[Base7] = uint4(asuint(Center.x), asuint(Center.y), asuint(Center.z), asuint(Area));
						PressureCellsOut[Base7 + 1u] = uint4(asuint(Layer), Row, asuint(B.y / A.w), asuint(ForceMax));
						float RepOff = length(CoarseInfo.Load(int3(Rep, 0)).xyz - Center) * ThicknessScale;   // 거리표 줄은 대표 칸 것이다
						PressureCellsOut[Base7 + 2u] = uint4(asuint(Mat.x), asuint(SkinCm), (uint)Rep.x | ((uint)Rep.y << 16u), asuint(RepOff));
#if SB_SEPARATION
						PressureSeparationOut[Level * SB_PRESSURE_CAP + Slot] = Shares / A.w;
#endif
						UNROLL
						for (uint R = 0; R < 4u; ++R)
						{
							PressureCellsOut[Base7 + 3u + R] = uint4(Nearest[R * 4u], Nearest[R * 4u + 1u], Nearest[R * 4u + 2u], Nearest[R * 4u + 3u]);
						}
						uint4 AmountRows[SB_PRESSURE_AMOUNT_WORDS / 4u];
						float LocalSelf;
						SB_PressureAmountRows(Layer, Mat.x, SkinCm, Bend, Area, AmountRows, LocalSelf);
						UNROLL
						for (uint R3 = 0; R3 < SB_PRESSURE_AMOUNT_WORDS / 4u; ++R3)
						{
							PressureCellsOut[Base7 + SB_PRESSURE_GEOM_ROWS + R3] = AmountRows[R3];
						}
						// [12].w = 겉 굽힘 길이(모으기가 W_c의 쌍 δ로 읽는다 — 연속 굽힘). 흰 칸은 0이라 지금 기록과 같다.
						PressureCellsOut[Base7 + SB_PRESSURE_LOCAL_ROW] = uint4(asuint(LocalSelf), SB_PressurePackNormal(NSum), SB_PressurePiece(Row), asuint(Bend));
#if SB_HARD_SURFACE
						if (Bend > 0.0)
						{
							InterlockedOr(PressureHeaderOut[SB_PRESSURE_HEADER_BEND], 1u);
						}
#endif
						for (uint SY = 0u; SY < BlockSide; ++SY)
						{
							for (uint SX = 0u; SX < BlockSide; ++SX)
							{
								uint Local = (Corner.y + SY) * Side + Corner.x + SX;
								uint Shift = 10u * Level;
								SB_PressureSlots[Local] = (SB_PressureSlots[Local] & ~(SB_PRESSURE_NO_SLOT << Shift)) | (Slot << Shift);
							}
						}
					}
					else if (Level == 0u)
					{
						InterlockedAdd(PressureHeaderOut[SB_SOURCE_HEADER_OVERFLOW], 1u);
					}
				}
			}
		}
		GroupMemoryBarrierWithGroupSync();   // 단계마다 칸 번호 낱말을 한 스레드만 고친다
	}
	if (bListed && LocalIndex < Side * Side)
	{
		int2 Cell = CoarseOrigin + int2(LocalIndex % Side, LocalIndex / Side);
		if (SB_InCoarse(Cell))
		{
			PressureSlotOut[Cell] = SB_PressureSlots[LocalIndex];
		}
	}
}

#if SB_HARD_SURFACE
groupshared uint SB_HardCandCount;
groupshared uint SB_HardCandPiece[256];
groupshared float SB_HardCandKey[256];   // 닿은 조각 −1, 아니면 상자까지 틈(메시 로컬)
groupshared uint SB_HardRanked[SB_HARD_CANDIDATES];

/**
 * 하드 서피스 인자(순열 1, 스레드 256): 후보 조각을 고르고(닿은 것 먼저, 그다음 압력 칸 상자에서 가까운 순 — 같은 틈은 조각 번호 순, 실행마다 같다),
 * 조각 밖 압력 칸 단계를 닿은 후보의 발자국 줄까지 상한 안에 드는 가장 가는 것으로 고른다. 후보는 강체 조각(붙은 검정·떠 있는 장식)만이고 차례대로
 * 행렬 줄(N_O + ΣF ≤ SB_PRESSURE_CAP)·풀이 스레드(N_K + 손 칸 줄 ≤ 1024 — 받침 줄은 제 행렬 줄 스레드, 손 칸 줄은 N_K부터) 안에 들 때만 든다.
 * 행렬 줄 수 N_K = N_O + ΣF를 머리에 쓴다 — 행렬·풀이·모으기는 발자국 줄을 보통 압력 칸으로 본다.
 */
void SB_HardPressureArgs(uint Local)
{
	if (Local == 0u)
	{
		SB_HardCandCount = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	float3 Lo = float3(SB_OrderedToFloat(PressureHeaderOut[SB_SOURCE_HEADER_MIN]), SB_OrderedToFloat(PressureHeaderOut[SB_SOURCE_HEADER_MIN + 1u]),
		SB_OrderedToFloat(PressureHeaderOut[SB_SOURCE_HEADER_MIN + 2u]));
	float3 Hi = float3(SB_OrderedToFloat(PressureHeaderOut[SB_SOURCE_HEADER_MAX]), SB_OrderedToFloat(PressureHeaderOut[SB_SOURCE_HEADER_MAX + 1u]),
		SB_OrderedToFloat(PressureHeaderOut[SB_SOURCE_HEADER_MAX + 2u]));
	bool bBox = all(Lo <= Hi);
	float Reach = PressureReachLayers * (float)PressureHeaderOut[SB_SOURCE_HEADER_SIGMA] * 0.001 / ThicknessScale;
	for (uint Piece = Local; Piece < HardPieceCount; Piece += 256u)
	{
		bool bTouched = ((HardWorkOut[SB_HARD_WORK_BITS + (Piece >> 5u)] >> (Piece & 31u)) & 1u) != 0u;
		float3 Center = HardPieceFrames[Piece * 3u].xyz;
		float Gap = length(max(max(Lo - Center, Center - Hi), float3(0, 0, 0))) - HardPieceFrames[Piece * 3u + 1u].w;
		// 굽는 조각(SB_HardKindOf 0)은 후보가 아니다 — 판이 아니라 칸 칠로 굽는 살이다(연속 굽힘).
		if ((bTouched || (bBox && Gap <= Reach)) && HardPieceCellRange[Piece].y > 0u && SB_HardKindOf(Piece) != 0u)
		{
			uint Slot;
			InterlockedAdd(SB_HardCandCount, 1u, Slot);
			if (Slot < 256u)
			{
				SB_HardCandPiece[Slot] = Piece;
				SB_HardCandKey[Slot] = bTouched ? -1.0 : max(Gap, 0.0);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	uint Count = min(SB_HardCandCount, 256u);
	if (Local < Count)
	{
		float Key = SB_HardCandKey[Local];
		uint Mine = SB_HardCandPiece[Local];
		uint Rank = 0u;
		for (uint k = 0u; k < Count; ++k)
		{
			float Other = SB_HardCandKey[k];
			Rank += (Other < Key || (Other == Key && SB_HardCandPiece[k] < Mine)) ? 1u : 0u;
		}
		if (Rank < SB_HARD_CANDIDATES)
		{
			SB_HardRanked[Rank] = Mine | (Key < 0.0 ? 0x80000000u : 0u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (Local == 0u)
	{
		uint Overflow = SB_HardCandCount > SB_HARD_CANDIDATES ? 2u : 0u;
		uint Ranked = min(Count, SB_HARD_CANDIDATES);
		uint TouchedCells = 0u;
		for (uint k1 = 0u; k1 < Ranked; ++k1)
		{
			uint Entry1 = SB_HardRanked[k1];
			TouchedCells += (Entry1 >> 31u) != 0u ? HardPieceCellRange[Entry1 & 0x7fffffffu].y : 0u;
		}
		uint C0 = PressureHeaderOut[SB_SOURCE_HEADER_COUNT];
		uint C1 = PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL1];
		uint C2 = PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL2];
		uint Level = C0 + TouchedCells <= SB_PRESSURE_CAP ? 0u : (C1 + TouchedCells <= SB_PRESSURE_CAP ? 1u : 2u);
		uint NO = min(Level == 0u ? C0 : (Level == 1u ? C1 : C2), SB_PRESSURE_CAP);
		uint Hands = min(HardWorkOut[SB_HARD_WORK_HANDS], SB_HARD_HAND_CAP);
		uint FRows = 0u;
		uint T = 0u;
		uint Touched = 0u;
		for (uint k2 = 0u; k2 < Ranked; ++k2)
		{
			uint Entry = SB_HardRanked[k2];
			uint Piece = Entry & 0x7fffffffu;
			uint Cells = HardPieceCellRange[Piece].y;
			if (NO + FRows + Cells > SB_PRESSURE_CAP || NO + FRows + Cells + Hands > 1024u)
			{
				// 못 드는 후보는 건너뛴다(차례는 그대로라 닿은 후보가 앞쪽에 남는다). 닿은 조각이면 그 손 칸 줄은 이번 갱신에 풀리지 않는다.
				Overflow |= (Entry >> 31u) != 0u ? 4u : 0u;
				continue;
			}
			HardWorkOut[SB_HARD_WORK_PIECE + T] = Piece;
			HardWorkOut[SB_HARD_WORK_START + T] = FRows;
			HardWorkOut[SB_HARD_WORK_COUNT + T] = Cells;
			FRows += Cells;
			Touched += Entry >> 31u;
			++T;
		}
		PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL] = Level;
		PressureHeaderOut[SB_PRESSURE_HEADER_COUNT] = NO + FRows;
		HardWorkOut[SB_HARD_WORK_CANDIDATES] = T;
		HardWorkOut[SB_HARD_WORK_TOUCHED] = Touched;
		HardWorkOut[SB_HARD_WORK_FROWS] = FRows;
		HardWorkOut[SB_HARD_WORK_FREE] = NO;
		HardWorkOut[SB_HARD_WORK_ROWS] = Touched > 0u ? min(HardWorkOut[SB_HARD_WORK_HANDS], SB_HARD_HAND_EMIT) : 0u;   // 풀이가 깊은 줄을 고른다
		if (Overflow != 0u)
		{
			InterlockedOr(HardWorkOut[SB_HARD_WORK_OVERFLOW], Overflow);
		}
		uint Groups = (NO + FRows + 31u) / 32u;
		PressureArgsOut[0] = Groups;
		PressureArgsOut[1] = Groups;
		PressureArgsOut[2] = 1u;
		PressureArgsOut[3] = NO + FRows > 0u ? 1u : 0u;
		PressureArgsOut[4] = 1u;
		PressureArgsOut[5] = 1u;
		HardArgsOut[0] = T;
		HardArgsOut[1] = 1u;
		HardArgsOut[2] = 1u;
	}
}
#endif

/** 칸 수가 상한 안인 가장 가는 단계를 고르고 행렬·풀이 인자를 쓴다(스레드 하나 — 하드 서피스 순열은 묶음 하나가 후보 조각과 발자국 줄도 고른다). */
#if SB_HARD_SURFACE
[numthreads(256, 1, 1)]
void ContactPressureArgsCS(uint LocalIndex : SV_GroupIndex)
{
	SB_HardPressureArgs(LocalIndex);
}
#else
[numthreads(1, 1, 1)]
void ContactPressureArgsCS()
{
	uint C0 = PressureHeaderOut[SB_SOURCE_HEADER_COUNT];
	uint C1 = PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL1];
	uint C2 = PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL2];
	uint Level = C0 <= SB_PRESSURE_CAP ? 0u : (C1 <= SB_PRESSURE_CAP ? 1u : 2u);
	uint N = min(Level == 0u ? C0 : (Level == 1u ? C1 : C2), SB_PRESSURE_CAP);
	PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL] = Level;
	PressureHeaderOut[SB_PRESSURE_HEADER_COUNT] = N;
	uint Groups = (N + 31u) / 32u;
	PressureArgsOut[0] = Groups;
	PressureArgsOut[1] = Groups;
	PressureArgsOut[2] = 1u;
	PressureArgsOut[3] = N > 0u ? 1u : 0u;
	PressureArgsOut[4] = 1u;
	PressureArgsOut[5] = 1u;
}
#endif

#if SB_HARD_SURFACE
/**
 * 하드 서피스 펼치기(새 디스패치, 후보마다 묶음 하나): 후보 조각의 발자국 거친 칸을 압력 칸과 같은 줄로 고른 단계의 자리 N_O + 첫 줄 + f에 쓴다(넓이 =
 * 거친 칸 살 넓이 × 조각 가는 칸 몫, 무게 중심·두께는 조각 가는 칸 평균, 재료·랜드마크는 그 거친 칸, 성분 양은 그 두께·재료, 접힘 상한 없음). 살이 없는
 * 칸은 넓이 0인 줄(행렬 0). 풀이가 읽는 판 자리(굽기의 발자국 칸 판 좌표, cm)·(조각 안 칸, 후보)·넓이를 FRowOut에, 무게 중심을 모으기 상자에 넣는다.
 */
[numthreads(256, 1, 1)]
void ContactPressurePiecesCS(uint3 GroupId : SV_GroupID, uint Local : SV_GroupIndex)
{
	uint C = GroupId.x;
	uint Piece = HardWork[SB_HARD_WORK_PIECE + C];
	uint Start = HardWork[SB_HARD_WORK_START + C];
	uint Count = HardWork[SB_HARD_WORK_COUNT + C];
	uint NO = HardWork[SB_HARD_WORK_FREE];
	uint Level = PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL];
	uint First = HardPieceCellRange[Piece].x;
	for (uint Item = Local; Item < Count; Item += 256u)
	{
		uint Slot = NO + Start + Item;
		uint4 Rows[SB_PRESSURE_ROWS];
		UNROLL
		for (uint R0 = 0u; R0 < SB_PRESSURE_ROWS; ++R0)
		{
			Rows[R0] = uint4(0, 0, 0, 0);
		}
		float Area = 0.0;
		uint Packed = HardPieceCellList[First + Item];
		int2 Cell = int2(Packed & 0xffffu, Packed >> 16u);
		float4 Info = SB_InCoarse(Cell) ? CoarseInfo.Load(int3(Cell, 0)) : float4(0, 0, 0, 0);
		if (Info.w > 0.0)
		{
			int2 Base = Cell * (int)CoarseBlock;
			uint Surface = 0u;
			uint OnPiece = 0u;
			float3 SumRest = float3(0, 0, 0);
			float3 SumPush = float3(0, 0, 0);   // 조각 칸 미는 방향 합(F10)
			float SumLayer = 0.0;
			for (uint Y = 0u; Y < CoarseBlock; ++Y)
			{
				for (uint X = 0u; X < CoarseBlock; ++X)
				{
					int2 T = Base + int2(X, Y);
					if (any(T >= int2(Resolution, Resolution)))
					{
						continue;
					}
					float4 Rest = CarrierPosition.Load(int3(T, 0));
					float4 Push = CarrierNormal.Load(int3(T, 0));   // xyz 미는 방향, w 두께
					float Thickness = Push.w;
					if ((uint)round(Rest.w) != SB_KIND_SURFACE || Thickness <= 0.0)
					{
						continue;
					}
					++Surface;
					if (SB_HardPieceAt(T) != Piece + 1u)
					{
						continue;
					}
					++OnPiece;
					SumRest += Rest.xyz;
					SumPush += Push.xyz;
					SumLayer += Thickness * ThicknessScale;
				}
			}
			uint Row = CoarseRow.Load(int3(Cell, 0));
			if (OnPiece > 0u && Row != SB_LANDMARK_NO_ROW)
			{
				Area = Info.w * ThicknessScale * ThicknessScale * (float)OnPiece / (float)Surface;
				float3 Center = SumRest / (float)OnPiece;
				float Layer = SumLayer / (float)OnPiece;
				float2 Mat = CoarseMaterial.Load(int3(Cell, 0));
				uint Nearest[SB_LANDMARK_NEAREST];
				UNROLL
				for (uint k1 = 0; k1 < SB_LANDMARK_NEAREST; ++k1)
				{
					Nearest[k1] = SB_LandmarkNearest(Row, k1);
				}
				UNROLL
				for (uint k2 = 0; k2 < SB_LANDMARK_NEAREST; ++k2)
				{
					Nearest[k2] = (SB_LandmarkValue(Row, Nearest[k2]) << 8u) | Nearest[k2];
				}
				float RepOff = length(Info.xyz - Center) * ThicknessScale;
				Rows[0] = uint4(asuint(Center.x), asuint(Center.y), asuint(Center.z), asuint(Area));
				Rows[1] = uint4(asuint(Layer), Row, 0u, asuint(1e30));
				Rows[2] = uint4(asuint(Mat.x), asuint(Mat.y * ThicknessScale), Packed, asuint(RepOff));
				UNROLL
				for (uint R = 0; R < 4u; ++R)
				{
					Rows[3u + R] = uint4(Nearest[R * 4u], Nearest[R * 4u + 1u], Nearest[R * 4u + 2u], Nearest[R * 4u + 3u]);
				}
				uint4 AmountRows[SB_PRESSURE_AMOUNT_WORDS / 4u];
				float LocalSelf;
				SB_PressureAmountRows(Layer, Mat.x, Mat.y * ThicknessScale, 0.0, Area, AmountRows, LocalSelf);   // 강체 판 밑 살은 굽지 않는다(판이 맡는다)
				UNROLL
				for (uint R3 = 0; R3 < SB_PRESSURE_AMOUNT_WORDS / 4u; ++R3)
				{
					Rows[SB_PRESSURE_GEOM_ROWS + R3] = AmountRows[R3];
				}
				Rows[SB_PRESSURE_LOCAL_ROW] = uint4(asuint(LocalSelf), SB_PressurePackNormal(SumPush), SB_PressurePiece(Row), 0u);
				UNROLL
				for (uint Axis = 0; Axis < 3u; ++Axis)
				{
					InterlockedMin(PressureHeaderOut[SB_SOURCE_HEADER_MIN + Axis], SB_OrderedFloat(Center[Axis]));
					InterlockedMax(PressureHeaderOut[SB_SOURCE_HEADER_MAX + Axis], SB_OrderedFloat(Center[Axis]));
				}
				InterlockedMax(PressureHeaderOut[SB_SOURCE_HEADER_SIGMA], (uint)(Layer * 1000.0));
			}
		}
		UNROLL
		for (uint R1 = 0u; R1 < SB_PRESSURE_ROWS; ++R1)
		{
			PressureCellsOut[SB_PressureIndex(Level, Slot, R1)] = Rows[R1];
		}
		float4 Plate = HardCellPlate[First + Item];
		FRowOut[Start + Item] = float4(Plate.xy * ThicknessScale, asfloat(Item | (C << 16u)), Area);
	}
}
#endif

#define SB_PRESSURE_BLOCK 32u
groupshared uint2 SB_PressureFaceI[SB_PRESSURE_BLOCK];   // i 쪽 칸 기록 [12].yz: 미는 방향·조각 번호(F10)
groupshared uint2 SB_PressureFaceJ[SB_PRESSURE_BLOCK];   // j 쪽
groupshared uint4 SB_PressureCellI[SB_PRESSURE_BLOCK * SB_PRESSURE_GEOM_ROWS];
groupshared uint4 SB_PressureCellJ[SB_PRESSURE_BLOCK * SB_PRESSURE_GEOM_ROWS];
groupshared uint SB_PressureAmountJ[SB_PRESSURE_BLOCK * SB_PRESSURE_AMOUNT_WORDS];   // j 쪽 성분 양(2.5 KB) — i 쪽은 스레드마다 레지스터(공유 메모리 32 KB 안)

/** 반정밀 둘을 낱말 하나로(A 아래 16비트). 반정밀 범위 밖은 끝값으로 — 행렬 값은 ±20 안팎이다. */
uint SB_PressurePackHalf2(float A, float B)
{
	return f32tof16(clamp(A, -65504.0, 65504.0)) | (f32tof16(clamp(B, -65504.0, 65504.0)) << 16u);
}

/** 압력 칸(Index < N, 하드 서피스 순열은 발자국 줄까지)의 성분 양 낱말을 Words의 Sub번째 자리에, 칸 안 대각 몫을 끝 두 낱말의 Sub번째에. 그 밖은 0. */
void SB_PressureLoadAmounts(uint Index, uint N, uint Level, uint FreeAligned, uint Sub, inout uint Words[2u * SB_PRESSURE_AMOUNT_WORDS + 2u])
{
	UNROLL
	for (uint R = 0u; R <= SB_PRESSURE_AMOUNT_WORDS / 4u; ++R)   // 마지막 줄 = 칸 안 대각 몫([12])
	{
		uint4 Value = uint4(0, 0, 0, 0);
		if (Index < N)
		{
			Value = PressureCells[SB_PressureIndex(Level, Index, SB_PRESSURE_GEOM_ROWS + R)];
		}
		if (R == SB_PRESSURE_AMOUNT_WORDS / 4u)
		{
			Words[2u * SB_PRESSURE_AMOUNT_WORDS + Sub] = Value.x;
			continue;
		}
		uint At = Sub * SB_PRESSURE_AMOUNT_WORDS + R * 4u;
		Words[At] = Value.x;
		Words[At + 1u] = Value.y;
		Words[At + 2u] = Value.z;
		Words[At + 3u] = Value.w;
	}
}

/**
 * 올린 칸 LI(i 쪽, 성분 양은 이 스레드의 레지스터 AmountI의 SubI번째)와 LJ(j 쪽)의 K(i, j)(F10). 거리는 두 칸 정지 위치의 직선(3-D 벡터)이고, 두 문을
 * 곱한다: 같은 조각(이어진 살 겉면)이 아니면 0, 마주 보는 겉면은 (1 + n_i·n_j)/2(맞서면 0 — 가슴골·허벅지 사이·팔과 몸통). 두 문 모두 칸마다 정한
 * 벡터의 내적 꼴이라 곱해도 행렬이 양의 정부호다. 거리에 랜드마크 하한을 쓰던 옛 행렬은 이음매가 많거나 굽은 곳에서 쌍마다 거리가 서로 맞지 않아
 * 음의 방향을 얻었고, 겉껍질 장력이 그 방향을 키워 판 누름이 터졌다(시험 캐릭터, Skin Tension 0.2부터). 하한은 긴 몫 모으기에만 남는다.
 */
float SB_PressureEntry(uint LI, uint LJ, bool bSelf, uint AmountI[2u * SB_PRESSURE_AMOUNT_WORDS + 2u], uint SubI)
{
	uint4 I0 = SB_PressureCellI[LI * SB_PRESSURE_GEOM_ROWS];
	uint4 J0 = SB_PressureCellJ[LJ * SB_PRESSURE_GEOM_ROWS];
	uint2 FaceI = SB_PressureFaceI[LI];
	uint2 FaceJ = SB_PressureFaceJ[LJ];
	float3 Ni = SB_PressureUnpackNormal(FaceI.x);
	float3 Nj = bSelf ? Ni : SB_PressureUnpackNormal(FaceJ.x);
	float3 D = bSelf ? float3(0, 0, 0) : (asfloat(I0.xyz) - asfloat(J0.xyz)) * ThicknessScale;
	float Gate = bSelf ? 1.0 : (FaceI.y == FaceJ.y ? 0.5 * (1.0 + dot(Ni, Nj)) : 0.0);
	uint MineI[SB_PRESSURE_AMOUNT_WORDS];
	uint AmountJ[SB_PRESSURE_AMOUNT_WORDS];
	UNROLL
	for (uint W = 0u; W < SB_PRESSURE_AMOUNT_WORDS; ++W)
	{
		MineI[W] = AmountI[SubI * SB_PRESSURE_AMOUNT_WORDS + W];
		AmountJ[W] = SB_PressureAmountJ[LJ * SB_PRESSURE_AMOUNT_WORDS + W];
	}
	float Local = bSelf ? asfloat(AmountI[2u * SB_PRESSURE_AMOUNT_WORDS + SubI]) : 0.0;   // 칸 안 대각 몫(SB_PressureAmountRows)
	// 잡기 줄(ADR-0041): 대각에 손끝 무름(칸 기록 [1].w의 음수 — 행렬은 그대로 양의 정부호)
	float GripI = asfloat(SB_PressureCellI[LI * SB_PRESSURE_GEOM_ROWS + 1u].w);
	Local += bSelf && GripI < 0.0 ? -GripI : 0.0;
	float Kernel = 0.0;
	BRANCH
	if (Gate > 0.0)
	{
		Kernel = SB_PressureKernelAmounts(D, Ni, Nj, asfloat(I0.w), asfloat(J0.w), MineI, AmountJ);
	}
	return Gate * Kernel + Local;
}


/**
 * 압력 칸 32 × 32 쌍 묶음마다 K(i, j). 두 쪽 칸 32개씩(앞 일곱 줄과 칸 기록 [12].yz)과 j 쪽 성분 양을 공유 메모리에 올리고(약 10 KB), 스레드
 * (16 × 16)마다 쌍 넷(SB_PressureEntry — i 쪽 성분 양은 레지스터). 거리표는 읽지 않는다(F10). 하드 서피스 순열은 조각 밖 칸 뒤에 발자국 줄이 보통
 * 압력 칸으로 이어진다.
 */
[numthreads(16, 16, 1)]
void ContactPressureMatrixCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	uint N = PressureHeader[SB_PRESSURE_HEADER_COUNT];
	uint Level = PressureHeader[SB_PRESSURE_HEADER_LEVEL];
	uint BaseI = GroupId.y * SB_PRESSURE_BLOCK;
	uint BaseJ = GroupId.x * SB_PRESSURE_BLOCK;
	// 칸 올리기(스레드 64개가 칸 하나씩)
	if (LocalIndex < 2u * SB_PRESSURE_BLOCK)
	{
		bool bI = LocalIndex < SB_PRESSURE_BLOCK;
		uint Local = LocalIndex & (SB_PRESSURE_BLOCK - 1u);
		uint Index = (bI ? BaseI : BaseJ) + Local;
		UNROLL
		for (uint R = 0; R < SB_PRESSURE_ROWS; ++R)
		{
			if (bI && R >= SB_PRESSURE_GEOM_ROWS && R < SB_PRESSURE_LOCAL_ROW)
			{
				continue;   // i 쪽 성분 양은 스레드마다 레지스터로 읽는다
			}
			uint4 Value = Index < N ? PressureCells[SB_PressureIndex(Level, Index, R)] : uint4(0, 0, 0, 0);
			if (R == SB_PRESSURE_LOCAL_ROW)
			{
				// 칸 기록 [12].yz: 미는 방향·조각 번호(F10) — 두 쪽 다. 칸 안 대각(.x)은 i 쪽만 레지스터로 읽는다(SB_PressureLoadAmounts).
				if (bI)
				{
					SB_PressureFaceI[Local] = Value.yz;
				}
				else
				{
					SB_PressureFaceJ[Local] = Value.yz;
				}
			}
			else if (R >= SB_PRESSURE_GEOM_ROWS)
			{
				uint Word = Local * SB_PRESSURE_AMOUNT_WORDS + (R - SB_PRESSURE_GEOM_ROWS) * 4u;
				SB_PressureAmountJ[Word] = Value.x;
				SB_PressureAmountJ[Word + 1u] = Value.y;
				SB_PressureAmountJ[Word + 2u] = Value.z;
				SB_PressureAmountJ[Word + 3u] = Value.w;
			}
			else if (bI)
			{
				SB_PressureCellI[Local * SB_PRESSURE_GEOM_ROWS + R] = Value;
			}
			else
			{
				SB_PressureCellJ[Local * SB_PRESSURE_GEOM_ROWS + R] = Value;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	// i 쪽 성분 양(이 스레드의 두 줄)
	uint AmountI[2u * SB_PRESSURE_AMOUNT_WORDS + 2u];
	UNROLL
	for (uint SubA = 0; SubA < 2u; ++SubA)
	{
		SB_PressureLoadAmounts(BaseI + LocalId.y * 2u + SubA, N, Level, 0xffffffffu, SubA, AmountI);
	}
	// 스레드마다 i 둘 × 이웃한 j 둘. j 둘을 반정밀 낱말 하나로 쓴다 — 풀이가 열 하나에서 줄 쌍을 한 번에 읽는다(일지 283).
	uint Pairs = (N + 1u) / 2u;
	UNROLL
	for (uint SubI = 0; SubI < 2u; ++SubI)
	{
		uint LI = LocalId.y * 2u + SubI;
		uint I = BaseI + LI;
		uint J0 = BaseJ + LocalId.x * 2u;
		if (I >= N || J0 >= N)
		{
			continue;
		}
		float Value[2] = { 0.0, 0.0 };   // N이 홀수면 마지막 낱말의 위 반은 0
		UNROLL
		for (uint SubJ = 0; SubJ < 2u; ++SubJ)
		{
			uint LJ = LocalId.x * 2u + SubJ;
			if (BaseJ + LJ < N)
			{
				Value[SubJ] = SB_PressureEntry(LI, LJ, I == BaseJ + LJ, AmountI, SubI);
			}
		}
		PressureMatrixOut[I * Pairs + J0 / 2u] = SB_PressurePackHalf2(Value[0], Value[1]);
	}
}

// 풀이는 스레드 묶음 하나(1024)가 한다. 줄(압력 칸) 하나는 같은 번호의 스레드가 맡고, 곱셈은 모든 스레드가 (목록 조각, 줄 쌍)으로 나눠 한다.
// 일지 283: 전에는 줄 셋씩 맡은 256 스레드가 float 행렬을 곱셈마다 K 차례로 하나씩 읽어 읽기 지연에 묶였다(눌린 장 하나 0.35~1.8 ms).
#define SB_PRESSURE_SOLVE_THREADS 1024u
#define SB_PRESSURE_SLICES_MAX 32u    // 곱셈 목록을 나누는 조각 수 상한(칸이 적으면 조각을 늘려 스레드를 더 쓴다)
#define SB_PRESSURE_UNROLL 8u         // 곱셈에서 한꺼번에 띄우는 읽기 수
groupshared float SB_PressureVector[SB_PRESSURE_CAP];                   // 곱할 벡터(줄 번호로)
groupshared uint SB_PressureList[SB_PRESSURE_CAP];                      // 곱할 성분 목록(0이 아닌 줄, 번호 순)
groupshared uint SB_PressureMask[SB_PRESSURE_CAP / 32u];                // 목록 만들기 비트
groupshared float SB_PressurePartial[2u * SB_PRESSURE_SOLVE_THREADS];   // 조각마다 줄의 부분 합 [조각 × 2P + 줄]
groupshared float SB_PressureSum[SB_PRESSURE_SOLVE_THREADS];
groupshared float SB_PressureSum32[32];
groupshared uint SB_PressureChanged;
// 이 스레드 줄의 반정밀 반올림 상한 Σ_j |Q_ij − K_ij| ≤ 2⁻¹¹Σ_j|Q_ij| + N·2⁻²⁵(Q = 저장한 반정밀 행렬). 곱셈마다 이 값 × 제 성분을 더한다 —
// 바탕 행렬 K가 양의 정부호면 Q + diag(상한)도 양의 정부호다(대각 우세한 오차 + 상한, Codex 검토 일지 287). 증상 문턱이 아니라 저장 꼴의 일부(일지 289).
static float SB_PressureRowBound = 0.0;

/** 묶음 합(모든 스레드가 부른다 — 장벽 둘). 더하는 차례가 정해져 있어 실행마다 같다. */
float SB_PressureReduce(float Value, uint Local)
{
	SB_PressureSum[Local] = Value;
	GroupMemoryBarrierWithGroupSync();
	if (Local < 32u)
	{
		float Part = 0.0;
		UNROLL
		for (uint k = 0; k < SB_PRESSURE_SOLVE_THREADS / 32u; ++k)
		{
			Part += SB_PressureSum[k * 32u + Local];
		}
		SB_PressureSum32[Local] = Part;
	}
	GroupMemoryBarrierWithGroupSync();
	float Total = 0.0;
	UNROLL
	for (uint k2 = 0; k2 < 32u; ++k2)
	{
		Total += SB_PressureSum32[k2];
	}
	return Total;
}

/** bFlag가 켜진 줄(스레드 번호 = 줄 번호)을 번호 순으로 SB_PressureList에 모으고 그 수를 돌려준다(모든 스레드가 부른다 — 장벽 셋). */
uint SB_PressureListOf(bool bFlag, uint Local)
{
	if (Local < SB_PRESSURE_CAP / 32u)
	{
		SB_PressureMask[Local] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (bFlag)
	{
		InterlockedOr(SB_PressureMask[Local >> 5u], 1u << (Local & 31u));
	}
	GroupMemoryBarrierWithGroupSync();
	uint Count = 0u;
	uint Before = 0u;
	UNROLL
	for (uint W = 0; W < SB_PRESSURE_CAP / 32u; ++W)
	{
		uint Bits = countbits(SB_PressureMask[W]);
		Before += W < (Local >> 5u) ? Bits : 0u;
		Count += Bits;
	}
	if (bFlag)
	{
		Before += countbits(SB_PressureMask[Local >> 5u] & ((1u << (Local & 31u)) - 1u));
		SB_PressureList[Before] = Local;
	}
	GroupMemoryBarrierWithGroupSync();
	return Count;
}

/**
 * 제 줄의 (K v)를 돌려준다. v = SB_PressureVector, 0이 아닌 성분은 SB_PressureList의 앞 Count개(모든 스레드가 부른다 — 장벽 하나).
 * 대칭이라 열 K를 줄 K로 읽는다: 낱말 [K × P + 쌍]에 줄 2쌍·2쌍 + 1의 값(반정밀 둘, P = ⌈N/2⌉). 스레드마다 (조각, 쌍) 하나 — 한 워프가 이웃한
 * 쌍을 읽어 읽기가 이어지고, 조각마다 목록을 나눠 맡아 칸이 적어도 스레드를 다 쓴다.
 */
float SB_PressureMultiply(uint Local, uint N, uint Count)
{
	uint Pairs = (N + 1u) / 2u;
	uint Slices = clamp(SB_PRESSURE_SOLVE_THREADS / Pairs, 1u, SB_PRESSURE_SLICES_MAX);
	uint Slice = Local / Pairs;
	uint Pair = Local - Slice * Pairs;
	if (Slice < Slices)
	{
		uint Chunk = (Count + Slices - 1u) / Slices;
		uint M = Slice * Chunk;
		uint End = min(Count, M + Chunk);
		float2 Acc = float2(0, 0);
		for (; M + SB_PRESSURE_UNROLL <= End; M += SB_PRESSURE_UNROLL)
		{
			uint Words[SB_PRESSURE_UNROLL];
			float D[SB_PRESSURE_UNROLL];
			UNROLL
			for (uint u = 0; u < SB_PRESSURE_UNROLL; ++u)
			{
				uint K = SB_PressureList[M + u];
				D[u] = SB_PressureVector[K];
				Words[u] = PressureMatrix[K * Pairs + Pair];
			}
			UNROLL
			for (uint u2 = 0; u2 < SB_PRESSURE_UNROLL; ++u2)
			{
				Acc += float2(f16tof32(Words[u2]), f16tof32(Words[u2] >> 16u)) * D[u2];
			}
		}
		for (; M < End; ++M)
		{
			uint K = SB_PressureList[M];
			uint Word = PressureMatrix[K * Pairs + Pair];
			Acc += float2(f16tof32(Word), f16tof32(Word >> 16u)) * SB_PressureVector[K];
		}
		SB_PressurePartial[Slice * 2u * Pairs + 2u * Pair] = Acc.x;
		SB_PressurePartial[Slice * 2u * Pairs + 2u * Pair + 1u] = Acc.y;
	}
	GroupMemoryBarrierWithGroupSync();
	float Out = 0.0;
	if (Local < N)
	{
		for (uint S = 0; S < Slices; ++S)
		{
			Out += SB_PressurePartial[S * 2u * Pairs + Local];
		}
		Out += SB_PressureRowBound * SB_PressureVector[Local];   // 부르기 전에 줄마다 제 성분을 썼다(목록 밖 줄은 0)
	}
	return Out;
}

#if SB_HARD_SURFACE
groupshared float2 SB_HardPhi[SB_HARD_HAND_CAP];           // 손 칸 줄의 지금 접촉 점(판 x, y cm)
groupshared uint2 SB_HardRange[SB_HARD_CANDIDATES];        // 닿은 후보마다 손 칸 줄 [첫, 끝)
groupshared float4 SB_HardFit[SB_HARD_CANDIDATES];         // 후보마다 강체 자세(가라앉음, x 기울기, y 기울기, 0 — 곱수)
// 강체 후보(배치 3 — 붙은 검정·떠 있는 장식): 종류·받침 무게 중심·C3·투영 꾸러미·조각별 합
groupshared uint SB_HardKind[SB_HARD_CANDIDATES];          // 후보마다 종류(아래 4비트: 1 붙은 강체, 2 떠 있음) | 모양 축 수 << 4(1 점, 2 선, 3 면)
groupshared float4 SB_HardRig[SB_HARD_CANDIDATES];         // 강체 후보마다 (받침 무게 중심 x, y cm, 선 모양의 긴 축 x, y)
groupshared uint SB_HardCore[SB_HARD_CANDIDATES];          // 떠 있는 후보마다 옷에 붙은 받침 줄(무게 중심에 가장 가까운 넓이 있는 발자국 줄, 전역 번호 — 없으면 0xffffffff)
groupshared float4 SB_HardG3A[SB_HARD_CANDIDATES];         // 강체 후보마다 C3(1차식으로 편 하중의 살 무름) g00, g01, g02, g11
groupshared float2 SB_HardG3B[SB_HARD_CANDIDATES];         //   g12, g22
groupshared float4 SB_HardPackA[SB_HARD_CANDIDATES];       // 투영 꾸러미(SB_HardInvert3): 무게 합, 무게 중심 u, v, 역 분산 i11
groupshared float2 SB_HardPackB[SB_HARD_CANDIDATES];       //   i12, i22
groupshared float4 SB_HardSum[SB_HARD_CANDIDATES];         // 조각별 합을 꾸러미로 푼 값(SB_HardCandSolve)

/** 저장한 반정밀 행렬 K(I, J)(반올림 보호 몫 없이) — 앞처리 대각에만 쓴다. */
float SB_HardK(uint I, uint J, uint Pairs)
{
	uint Word = PressureMatrix[I * Pairs + (J >> 1u)];
	return f16tof32((J & 1u) != 0u ? (Word >> 16u) : Word);
}

/** 발자국 줄 J(N_O에서 셈)의 후보·조각 안 칸·판 자리(cm). */
void SB_HardFRow(uint J, out uint Cand, out uint F, out float2 XY)
{
	float4 Row = FRow[J];
	uint Packed = asuint(Row.z);
	F = Packed & 0xffffu;
	Cand = Packed >> 16u;
	XY = Row.xy;
}

/** 묶음 가장 작은 값(모든 스레드가 부른다 — 장벽 둘, SB_PressureReduce와 같은 자리). */
float SB_PressureReduceMin(float Value, uint Local)
{
	SB_PressureSum[Local] = Value;
	GroupMemoryBarrierWithGroupSync();
	if (Local < 32u)
	{
		float Part = 1e30;
		UNROLL
		for (uint k = 0; k < SB_PRESSURE_SOLVE_THREADS / 32u; ++k)
		{
			Part = min(Part, SB_PressureSum[k * 32u + Local]);
		}
		SB_PressureSum32[Local] = Part;
	}
	GroupMemoryBarrierWithGroupSync();
	float Lowest = 1e30;
	UNROLL
	for (uint k2 = 0; k2 < 32u; ++k2)
	{
		Lowest = min(Lowest, SB_PressureSum32[k2]);
	}
	return Lowest;
}

/** 묶음 비트 합(모든 스레드가 부른다 — 장벽 셋, 지난 읽기가 끝난 뒤 지운다). */
uint SB_HardAnyBits(uint Bits, uint Local)
{
	GroupMemoryBarrierWithGroupSync();
	if (Local == 0u)
	{
		SB_PressureChanged = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (Bits != 0u)
	{
		InterlockedOr(SB_PressureChanged, Bits);
	}
	GroupMemoryBarrierWithGroupSync();
	return SB_PressureChanged;
}

/** 강체 후보 C의 1차식 φ = (1, u, v) — 받침 무게 중심에서 잰 판 자리(cm). 선 모양은 긴 축 거리만, 한 점은 1만(재현 B3Pieces.basis). */
float3 SB_HardBasis(uint C, float2 XY)
{
	float4 Rig = SB_HardRig[C];
	uint Rank = SB_HardKind[C] >> 4u;
	float2 D = XY - Rig.xy;
	return Rank >= 3u ? float3(1.0, D.x, D.y) : (Rank == 2u ? float3(1.0, dot(D, Rig.zw), 0.0) : float3(1.0, 0.0, 0.0));
}

/** 강체 자세(φ 계수)의 판 기울기(cm/cm, 판 x·y). */
float2 SB_HardPoseGrad(uint C, float3 Pose)
{
	uint Rank = SB_HardKind[C] >> 4u;
	return Rank >= 3u ? Pose.yz : (Rank == 2u ? Pose.y * SB_HardRig[C].zw : float2(0, 0));
}

/** 강체 자세를 판 틀 가운데(판 좌표 0) 기준으로: 깊이 = x + (y, z)·판 좌표 — 가는 창·합치기가 칸 쉼 자리에서 읽는다(SB_HardPlateAt). */
float3 SB_HardPoseToFrame(uint C, float3 Pose)
{
	float2 Grad = SB_HardPoseGrad(C, Pose);
	return float3(Pose.x - dot(Grad, SB_HardRig[C].xy), Grad);
}

/** C3 이차식 φᵀG₃φ. */
float SB_HardQuad(uint C, float3 Ph)
{
	float4 A = SB_HardG3A[C];
	float2 B = SB_HardG3B[C];
	return A.x * Ph.x * Ph.x + A.w * Ph.y * Ph.y + B.y * Ph.z * Ph.z + 2.0 * (A.y * Ph.x * Ph.y + A.z * Ph.x * Ph.z + B.x * Ph.y * Ph.z);
}

/**
 * 대칭 3 × 3 S = Σ w φφᵀ(s00, s01, s02, s11, s12, s22)를 풀 꾸러미로(재현 inv3): 무게 합 W, 무게 중심 (u, v), 무게 중심에서 잰 2 × 2 분산의 유사 역 —
 * 작은 고윳값이 큰 것의 SB_HARD_INV_CUT 밑이면 그 축은 0. 손 칸 줄 무게(1/εc)가 받침 줄의 수천 배라 S를 그대로 역으로 하면 거의 특이하다(무게
 * 중심으로 옮기면 대각에 가깝다). 쓰는 축(Rank)만. W = 0이면 빈 꾸러미(자유 줄 없음).
 */
void SB_HardInvert3(float S00, float S01, float S02, float S11, float S12, float S22, uint Rank, out float4 PackA, out float2 PackB)
{
	PackA = float4(0, 0, 0, 0);
	PackB = float2(0, 0);
	if (!(S00 > 1e-30))
	{
		return;
	}
	float Mu = Rank >= 2u ? S01 / S00 : 0.0;
	float Mv = Rank >= 3u ? S02 / S00 : 0.0;
	float C11 = Rank >= 2u ? S11 - S00 * Mu * Mu : 0.0;
	float C12 = Rank >= 3u ? S12 - S00 * Mu * Mv : 0.0;
	float C22 = Rank >= 3u ? S22 - S00 * Mv * Mv : 0.0;
	float Disc = sqrt(max(0.25 * (C11 - C22) * (C11 - C22) + C12 * C12, 0.0));
	float L1 = 0.5 * (C11 + C22) + Disc;
	float L2 = 0.5 * (C11 + C22) - Disc;
	float3 Inv = float3(0, 0, 0);
	if (L1 > 1e-30)
	{
		float2 E = C11 >= C22 ? float2(L1 - C22, C12) : float2(C12, L1 - C11);
		float N = length(E);
		E = N > 1e-30 ? E / N : float2(1, 0);
		Inv = float3(E.x * E.x, E.x * E.y, E.y * E.y) / L1;
		if (L2 > SB_HARD_INV_CUT * L1)
		{
			Inv += float3(E.y * E.y, -E.x * E.y, E.x * E.x) / L2;
		}
	}
	PackA = float4(S00, Mu, Mv, Inv.x);
	PackB = Inv.yz;
}

/** μ = S⁻¹b(꾸러미 SB_HardInvert3, 재현 solve3). */
float3 SB_HardApply3(float4 PackA, float2 PackB, float3 B)
{
	if (!(PackA.x > 0.0))
	{
		return float3(0, 0, 0);
	}
	float Pu = B.y - PackA.y * B.x;
	float Pv = B.z - PackA.z * B.x;
	float Tu = PackA.w * Pu + PackB.x * Pv;
	float Tv = PackB.x * Pu + PackB.y * Pv;
	return float3(B.x / PackA.x - PackA.y * Tu - PackA.z * Tv, Tu, Tv);
}

/**
 * 강체 후보마다 S = Σ_i w_i φ_iφ_iᵀ와 b = Σ_i s_i φ_i v_i를 한 번에 모아(재현 seg_inv + seg_sum) μ = S⁻¹b를 푼다(SB_HardInvert3·SB_HardApply3) — 줄 = 받침 줄
 * (N_O + 첫 발자국 줄 + f, s = +1)과 손 칸 줄(N_K + h, s = −1), w·v = 그 줄 스레드의 Weight·Value(쓰지 않을 줄은 0). bPose면 μ를 SB_HardFit(강체 자세)에,
 * 아니면 SB_HardSum에 쓰고 꾸러미를 SB_HardPackA·B에 둔다. 후보마다 10스레드가 줄을 나눠 더한다. 모든 스레드가 부른다(장벽 넷) — 합 자리
 * (SB_PressureSum·SB_PressurePartial)를 빌린다. 투영 꾸러미는 부를 때마다 새로 만든다(묶인 줄이 바뀌어도 따로 다시 만들 것이 없다).
 */
void SB_HardCandSolve(uint Local, float Weight, float Value, uint NO, uint NK, uint Cands, bool bPose)
{
	SB_PressureSum[Local] = Weight;
	SB_PressurePartial[Local] = Value;
	GroupMemoryBarrierWithGroupSync();
	uint C = Local / 10u;
	uint Lane = Local - C * 10u;
	float3 A1 = float3(0, 0, 0);
	float3 A2 = float3(0, 0, 0);
	float3 A3 = float3(0, 0, 0);
	if (C < Cands && (SB_HardKind[C] & 15u) != 0u)
	{
		uint Start = HardWork[SB_HARD_WORK_START + C];
		uint Count = HardWork[SB_HARD_WORK_COUNT + C];
		uint2 Hands = SB_HardRange[C];
		uint Total = Count + Hands.y - Hands.x;
		for (uint i = Lane; i < Total; i += 10u)
		{
			bool bSupp = i < Count;
			uint Row = bSupp ? NO + Start + i : NK + Hands.x + i - Count;
			float3 Ph = SB_HardBasis(C, bSupp ? FRow[Start + i].xy : SB_HardPhi[Hands.x + i - Count]);
			float W = SB_PressureSum[Row];
			A1 += W * Ph;
			A2 += W * float3(Ph.y * Ph.y, Ph.y * Ph.z, Ph.z * Ph.z);
			A3 += Ph * ((bSupp ? 1.0 : -1.0) * SB_PressurePartial[Row]);
		}
	}
	GroupMemoryBarrierWithGroupSync();   // 줄 값을 다 읽은 뒤 같은 자리에 스레드마다 아홉 값(32 × 10 × 9 ≤ 1024 + 2048)
	if (Local < SB_HARD_CANDIDATES * 10u)
	{
		uint B = Local * 9u;
		float Nine[9] = { A1.x, A1.y, A1.z, A2.x, A2.y, A2.z, A3.x, A3.y, A3.z };
		UNROLL
		for (uint q = 0u; q < 9u; ++q)
		{
			if (B + q < 2048u)
			{
				SB_PressurePartial[B + q] = Nine[q];
			}
			else
			{
				SB_PressureSum[B + q - 2048u] = Nine[q];
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (Local < Cands && (SB_HardKind[Local] & 15u) != 0u)
	{
		float Sum[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		for (uint k = 0u; k < 10u; ++k)
		{
			uint B2 = (Local * 10u + k) * 9u;
			UNROLL
			for (uint q2 = 0u; q2 < 9u; ++q2)
			{
				Sum[q2] += B2 + q2 < 2048u ? SB_PressurePartial[B2 + q2] : SB_PressureSum[B2 + q2 - 2048u];
			}
		}
		float4 PkA;
		float2 PkB;
		SB_HardInvert3(Sum[0], Sum[1], Sum[2], Sum[3], Sum[4], Sum[5], SB_HardKind[Local] >> 4u, PkA, PkB);
		SB_HardPackA[Local] = PkA;
		SB_HardPackB[Local] = PkB;
		float3 Mu = SB_HardApply3(PkA, PkB, float3(Sum[6], Sum[7], Sum[8]));
		if (bPose)
		{
			SB_HardFit[Local] = float4(Mu, 0.0);
		}
		else
		{
			SB_HardSum[Local] = float4(Mu, 0.0);
		}
	}
	GroupMemoryBarrierWithGroupSync();
}

/** 투영 앞처리(재현 project): 자유 줄 z̃ = M⁻¹(r − sφᵀμ), μ = S⁻¹ Σ sφM⁻¹r(강체 후보 줄만 — 나머지 줄은 M⁻¹r, S = Σ M⁻¹φφᵀ 자유 줄). 모든 스레드가 부른다. */
float SB_HardProject(uint Local, float Res, float MInv, bool bFreeRow, bool bRigidRow, uint RowCand, float Sign, float2 RowXY, bool bAnyRigid,
	uint NO, uint NK, uint Cands)
{
	float Z = bFreeRow ? MInv * Res : 0.0;
	if (bAnyRigid)
	{
		SB_HardCandSolve(Local, bRigidRow && bFreeRow ? MInv : 0.0, bRigidRow && bFreeRow ? MInv * Res : 0.0, NO, NK, Cands, false);
		if (bRigidRow && bFreeRow)
		{
			Z = MInv * (Res - Sign * dot(SB_HardBasis(RowCand, RowXY), SB_HardSum[RowCand].xyz));
		}
	}
	return Z;
}

/**
 * 손이 닿지 않은 떠 있는 후보 C의 자세(재현 solve_b3 꼬리): 셔츠 처짐 s_F(발자국 줄 SB_PressureVector — 강체 줄은 SB_HardPlates가 판 값으로 덮지 않는다)의
 * 평면 맞춤을 셔츠를 파고들지 않게 올린 것(무게 없는 얹힌 몸). 후보 스레드 하나가 부른다.
 */
float3 SB_HardRestingPose(uint C, uint NO)
{
	uint Start = HardWork[SB_HARD_WORK_START + C];
	uint Count = HardWork[SB_HARD_WORK_COUNT + C];
	float3 M1 = float3(0, 0, 0);
	float3 M2 = float3(0, 0, 0);
	float3 B = float3(0, 0, 0);
	for (uint f = 0u; f < Count; ++f)
	{
		float3 Ph = SB_HardBasis(C, FRow[Start + f].xy);
		float Sag = SB_PressureVector[NO + Start + f];
		M1 += Ph.x * Ph;
		M2 += float3(Ph.y * Ph.y, Ph.y * Ph.z, Ph.z * Ph.z);
		B += Ph * Sag;
	}
	float4 PkA;
	float2 PkB;
	SB_HardInvert3(M1.x, M1.y, M1.z, M2.x, M2.y, M2.z, SB_HardKind[C] >> 4u, PkA, PkB);
	float3 Pose = SB_HardApply3(PkA, PkB, B);
	float Over = 0.0;
	for (uint f2 = 0u; f2 < Count; ++f2)
	{
		Over = max(Over, dot(SB_HardBasis(C, FRow[Start + f2].xy), Pose) - SB_PressureVector[NO + Start + f2]);
	}
	Pose.x -= Over;
	return Pose;
}

/**
 * 조각 위 손 칸 줄 고르기·차례(배치 3): 후보 조각 줄 가운데 가장 깊은 SB_HARD_HAND_CAP줄(머리 w = 가장 깊은 표본 깊이 — 같으면 열쇠 차례, 실행마다
 * 같다)을 받아 열쇠 (후보, 거친 칸 y, x)로 센 차례에 둔다. 후보마다 줄 구간을 SB_HardRange에(손 칸 줄이 없는 후보는 빈 구간). 이 스레드(N_K + h)가
 * 맡을 원래 줄과 후보를 돌려준다. 받는 줄 수를 돌려준다. 모든 스레드가 부른다(장벽 다섯) — 목록 자리(SB_PressureList)와 부분 합 자리를 잠깐 빌린다.
 */
uint SB_HardSortHands(uint Local, uint Rows, uint Cands, uint NK, out uint Raw, out uint Cand)
{
	if (Local < Rows)
	{
		float4 Head = HardHand[Local * SB_HARD_HAND_ROWS];
		uint Piece = (uint)Head.x;
		uint Packed = (uint)Head.y;   // 거친 칸 x + y × 4096
		uint Mine = 0xffu;
		for (uint c = 0u; c < Cands; ++c)
		{
			Mine = HardWork[SB_HARD_WORK_PIECE + c] == Piece ? c : Mine;
		}
		SB_PressureList[Local] = Mine < Cands ? (Mine << 24u) | ((Packed >> 12u) << 12u) | (Packed & 0xfffu) : 0xffffffffu;
		SB_PressurePartial[Local] = Mine < Cands ? Head.w : -1.0;
	}
	GroupMemoryBarrierWithGroupSync();
	bool bIn = false;
	if (Local < Rows && SB_PressureList[Local] != 0xffffffffu)
	{
		uint Key = SB_PressureList[Local];
		float Depth = SB_PressurePartial[Local];
		uint Deeper = 0u;
		for (uint k = 0u; k < Rows; ++k)
		{
			uint KeyK = SB_PressureList[k];
			float DepthK = SB_PressurePartial[k];
			Deeper += KeyK != 0xffffffffu && (DepthK > Depth || (DepthK == Depth && KeyK < Key)) ? 1u : 0u;
		}
		bIn = Deeper < SB_HARD_HAND_CAP;
	}
	GroupMemoryBarrierWithGroupSync();
	if (Local < Rows)
	{
		SB_PressurePartial[Local] = bIn ? 1.0 : 0.0;
	}
	GroupMemoryBarrierWithGroupSync();
	// 받은 줄의 차례는 목록 자리 [SB_HARD_HAND_EMIT, SB_HARD_HAND_EMIT + SB_HARD_HAND_CAP)에(= SB_PRESSURE_CAP까지)
	if (bIn)
	{
		uint Key = SB_PressureList[Local];
		uint Rank = 0u;
		for (uint k2 = 0u; k2 < Rows; ++k2)
		{
			Rank += SB_PressurePartial[k2] > 0.5 && SB_PressureList[k2] < Key ? 1u : 0u;
		}
		SB_PressureList[SB_HARD_HAND_EMIT + Rank] = Local;
	}
	if (Local < Cands)
	{
		uint Lo = 0u;
		uint Hi = 0u;
		for (uint k3 = 0u; k3 < Rows; ++k3)
		{
			uint C3 = SB_PressureList[k3] >> 24u;
			bool bTaken = SB_PressurePartial[k3] > 0.5;
			Lo += bTaken && C3 < Local ? 1u : 0u;
			Hi += bTaken && C3 <= Local ? 1u : 0u;
		}
		SB_HardRange[Local] = uint2(Lo, Hi);
	}
	GroupMemoryBarrierWithGroupSync();
	uint Valid = Cands > 0u ? SB_HardRange[Cands - 1u].y : 0u;
	Raw = 0u;
	Cand = 0u;
	if (Local >= NK && Local - NK < Valid)
	{
		uint h = Local - NK;
		Raw = SB_PressureList[SB_HARD_HAND_EMIT + h];
		for (uint c2 = 0u; c2 < Cands; ++c2)
		{
			Cand = SB_HardRange[c2].x <= h ? c2 : Cand;
		}
	}
	GroupMemoryBarrierWithGroupSync();   // 목록 자리를 풀이가 다시 쓰기 전에
	return Valid;
}

/**
 * 손 칸 줄 Raw의 접촉 점을 제 발자국 칸 국소 판(값 W, 기울기 Grad cm/cm, 자리 XF cm)에 대해 고른다: m = max(g − 판), 표본 무게 w = max(0, g − 판 − (m − δ)),
 * 자리 = 무게 중심, 목표 = 그 자리 판 + m(재현 PlateReplica.pick couple='plane'). 첫 바퀴는 판 0이라 깊이 무게 중심·가장 깊은 깊이다.
 */
void SB_HardPick(uint Raw, float W, float2 Grad, float2 XF, out float2 Phi, out float G)
{
	uint Count = (uint)HardHand[Raw * SB_HARD_HAND_ROWS].z;
	float Top = -1e30;
	for (uint s = 0u; s < Count; ++s)
	{
		float4 Sm = HardHand[Raw * SB_HARD_HAND_ROWS + 1u + s];
		Top = max(Top, Sm.z - (W + dot(Grad, Sm.xy - XF)));
	}
	float Wt = 0.0;
	float2 XY = float2(0, 0);
	for (uint s2 = 0u; s2 < Count; ++s2)
	{
		float4 Sm = HardHand[Raw * SB_HARD_HAND_ROWS + 1u + s2];
		float Wk = max(Sm.z - (W + dot(Grad, Sm.xy - XF)) - (Top - SB_HARD_PICK_DELTA), 0.0);
		Wt += Wk;
		XY += Wk * Sm.xy;
	}
	Phi = Wt > 0.0 ? XY / Wt : XF;
	G = W + dot(Grad, Phi - XF) + Top;
}

/** 풀이 줄 벡터 쓰기: 조각 밖·받침 줄 = SB_PressureVector(제 행렬 줄, 역할 1·4). 손 칸 줄 값은 곱(정칙화)에서 바로 쓴다. */
void SB_HardWriteVector(uint Local, uint Role, float V)
{
	if (Role == 1u || Role == 4u)
	{
		SB_PressureVector[Local] = V;
	}
}

/**
 * 하드 서피스 곱(모든 스레드가 부른다 — 부르기 전에 SB_HardWriteVector와 장벽): 풀이 줄 벡터를 발자국 줄 하중 λ_F(= 받침 줄 값)로 펴서
 * SB_PressureVector[N_O, N_K)에 쓰고 s = K λ를 곱한다(지금 반정밀 곱, 반올림 보호 포함). 풀이 줄 출력을 돌려준다: 조각 밖·받침 줄 = s, 손 칸 줄 =
 * ε c_k v(Reg — 살과는 평형으로만 잇는다). 행렬 줄의 s·λ는 SRow·LRow로(행렬 줄이 아니면 0). 끝나면 SB_PressureVector[N_O, N_K)는 s_F다.
 */
float SB_HardMultiply(uint Local, uint NO, uint NK, uint Role, float V, float Reg, out float SRow, out float LRow)
{
	LRow = Local < NO ? V : 0.0;
	if (Local >= NO && Local < NK)
	{
		float Lambda = Role == 4u ? V : 0.0;   // 하중 = 제 받침 줄 값(받침 줄이 아닌 발자국 줄은 0)
		SB_PressureVector[Local] = Lambda;
		LRow = Lambda;
	}
	uint Count = SB_PressureListOf(Local < NK && LRow != 0.0, Local);
	float S = NK > 0u ? SB_PressureMultiply(Local, NK, Count) : 0.0;
	SRow = Local < NK ? S : 0.0;
	if (Local >= NO && Local < NK)
	{
		SB_PressureVector[Local] = S;   // 곱셈의 끝 장벽 뒤에는 저마다 제 자리만 읽는다
	}
	GroupMemoryBarrierWithGroupSync();
	return Role == 1u || Role == 4u ? S : (Role == 2u ? Reg * V : 0.0);
}

/** 발자국 줄마다 지금 s_F를 SB_PressureVector[N_O + 줄]에 둔다(모든 스레드가 부른다, 장벽 하나) — 강체 자세(SB_HardFit·얹힘)가 읽는다. */
void SB_HardPlates(uint Local, uint NO, uint NK, float SRow)
{
	if (Local >= NO && Local < NK)
	{
		SB_PressureVector[Local] = SRow;
	}
	GroupMemoryBarrierWithGroupSync();
}
/**
 * 하드 서피스 풀이(ContactPressureSolveCS의 순열 1 몸체, 배치 3 — 재현 replica_plate.solve_b3와 같은 차례): 위 머리 주석의 풀이 줄을 투영 앞처리 CG로
 * 푼다. 바퀴마다 a. 판(강체 자세)에 대해 b. 손 칸 줄의 접촉 점·목표를 다시 고르고, c. 강체 평형을 되살리고, d. 처짐을 다시 곱하고, e. 곱수
 * (강체 자세)를 구해 f. 곱수 뺀 기울기로 묶인 줄을 푼 뒤, g. 걸음 자르기 CG. 0에서 출발 — 지난 갱신의 해를 쓰지 않는다(상태 없음). 꼬리: 발자국 줄
 * 하중 λ_F를 원천 힘으로, 강체 자세를 조각 움직임에(w = 2 붙은 강체, 3 떠 있음), (λ_F, 넓이)를 발자국 거친 칸에 쓴다.
 */
void SB_HardSolve(uint LocalIndex)
{
	uint NK = PressureHeader[SB_PRESSURE_HEADER_COUNT];
	uint Level = PressureHeader[SB_PRESSURE_HEADER_LEVEL];
	uint NO = HardWork[SB_HARD_WORK_FREE];
	uint Pairs = (NK + 1u) / 2u;
	bool bRow = LocalIndex < NK;
	bool bFree = LocalIndex < NO;
	if (bRow)
	{
		float AbsSum = 0.0;
		for (uint PB = 0u; PB < Pairs; ++PB)
		{
			uint WordB = PressureMatrix[LocalIndex * Pairs + PB];
			AbsSum += abs(f16tof32(WordB)) + abs(f16tof32(WordB >> 16u));
		}
		SB_PressureRowBound = AbsSum * exp2(-11.0) + (float)NK * exp2(-25.0);
	}
	uint4 Cell1 = bFree ? PressureCells[SB_PressureIndex(Level, LocalIndex, 1u)] : uint4(0, 0, 0, 0);
	float G = asfloat(Cell1.z);
	float FMax = bFree ? asfloat(Cell1.w) : 1e30;
	// 잡기 줄(ADR-0041, 힘 상한 자리가 음수): 아래·위 경계 없이 푼다
	bool bGrabRow = bFree && FMax < 0.0;
	FMax = bGrabRow ? 1e30 : FMax;
	uint Touched = HardWork[SB_HARD_WORK_TOUCHED];
	uint Cands = HardWork[SB_HARD_WORK_CANDIDATES];
	// 후보 종류(인자 패스와 같은 판정 — 후보는 강체뿐이다). 강체 자세는 0에서.
	if (LocalIndex < Cands)
	{
		SB_HardKind[LocalIndex] = SB_HardKindOf(HardWork[SB_HARD_WORK_PIECE + LocalIndex]);
		SB_HardFit[LocalIndex] = float4(0, 0, 0, 0);
	}
	uint HandRaw = 0u;
	uint HandCand = 0u;
	uint ValidRows = SB_HardSortHands(LocalIndex, Touched > 0u ? HardWork[SB_HARD_WORK_ROWS] : 0u, Cands, NK, HandRaw, HandCand);
	// 강체 후보 준비(후보마다 한 스레드, 재현 B3Pieces): 받침 무게 중심(넓이 무게), 모양 축 수(분산 비), 선 모양의 긴 축
	if (LocalIndex < Cands && (SB_HardKind[LocalIndex] & 15u) != 0u)
	{
		uint StartR = HardWork[SB_HARD_WORK_START + LocalIndex];
		uint CountR = HardWork[SB_HARD_WORK_COUNT + LocalIndex];
		float SumA = 0.0;
		float2 SumX = float2(0, 0);
		float2 SumU = float2(0, 0);
		for (uint f = 0u; f < CountR; ++f)
		{
			float4 RowR = FRow[StartR + f];
			SumA += RowR.w;
			SumX += RowR.w * RowR.xy;
			SumU += RowR.xy;
		}
		bool bArea = SumA > 0.0;
		float2 Cen = bArea ? SumX / SumA : SumU / max((float)CountR, 1.0);
		float Cxx = 0.0;
		float Cxy = 0.0;
		float Cyy = 0.0;
		float CoreD2 = 1e30;
		uint Core = 0xffffffffu;
		for (uint f2 = 0u; f2 < CountR; ++f2)
		{
			float4 RowR = FRow[StartR + f2];
			float Wt = bArea ? RowR.w / SumA : 1.0 / max((float)CountR, 1.0);
			float2 D = RowR.xy - Cen;
			Cxx += Wt * D.x * D.x;
			Cxy += Wt * D.x * D.y;
			Cyy += Wt * D.y * D.y;
			// 떠 있는 장식이 옷에 붙은 자리: 무게 중심에 가장 가까운 넓이 있는 줄(같으면 앞 줄 — 재현 float_core)
			if (RowR.w > 0.0 && dot(D, D) < CoreD2)
			{
				CoreD2 = dot(D, D);
				Core = StartR + f2;
			}
		}
		SB_HardCore[LocalIndex] = Core;
		float Disc = sqrt(max(0.25 * (Cxx - Cyy) * (Cxx - Cyy) + Cxy * Cxy, 0.0));
		float L1 = 0.5 * (Cxx + Cyy) + Disc;
		float L2 = 0.5 * (Cxx + Cyy) - Disc;
		uint Rank = L1 <= SB_HARD_RANK_TINY ? 1u : (L2 < SB_HARD_RANK_RATIO * L1 ? 2u : 3u);
		float2 Axis = abs(Cxy) > 1e-12 * max(L1, 1e-30) ? float2(Cxy, L1 - Cxx) : (Cxx >= Cyy ? float2(1, 0) : float2(0, 1));
		Axis /= max(length(Axis), 1e-30);
		SB_HardRig[LocalIndex] = float4(Cen, Axis);
		SB_HardKind[LocalIndex] = (SB_HardKind[LocalIndex] & 15u) | (Rank << 4u);
	}
	GroupMemoryBarrierWithGroupSync();
	bool bAnyRigid = false;
	for (uint cr = 0u; cr < Cands; ++cr)
	{
		bAnyRigid = bAnyRigid || (SB_HardKind[cr] & 15u) != 0u;
	}
	// 줄 역할: 1 조각 밖 칸, 2 손 칸 줄, 4 받침 줄, 0 없음(받침이 아닌 발자국 줄은 행렬 줄만)
	bool bFoot = LocalIndex >= NO && LocalIndex < NK;
	uint FootCand = 0u;
	uint FootF = 0u;
	float2 FootXY = float2(0, 0);
	if (bFoot)
	{
		SB_HardFRow(LocalIndex - NO, FootCand, FootF, FootXY);
	}
	uint FootKind = bFoot ? (SB_HardKind[FootCand] & 15u) : 0u;
	uint2 FootHands = bFoot ? SB_HardRange[FootCand] : uint2(0, 0);
	// 손 칸 줄이 없는 떠 있는 후보는 받침이 0이라 받침 줄을 두지 않는다(꼬리에서 셔츠에 얹는다). 넓이 0인 줄(살 없음)은 판에 붙지 않는다.
	bool bSupport = bFoot && FootKind != 0u && FRow[bFoot ? LocalIndex - NO : 0u].w > 0.0 && (FootKind != 2u || FootHands.y > FootHands.x);
	bool bHandThread = LocalIndex >= NK && LocalIndex - NK < ValidRows;
	uint Hand = bHandThread ? LocalIndex - NK : 0u;
	bool bHand = bHandThread && (SB_HardKind[HandCand] & 15u) != 0u;
	uint Role = bFree ? 1u : (bSupport ? 4u : (bHand ? 2u : 0u));
	// 떠 있는 장식의 받침은 밀기만 한다. 다만 옷에 붙은 줄 하나(SB_HardCore)는 당길 수도 있다 — 모두 밀기만 하면 가장자리 누름에서 기울기를 끝없이
	// 키워도 힘이 맞지 않아 풀이가 예산에서 멈춘 자세를 그렸다(반대쪽이 수~수십 cm 솟고 받침 압력이 가장자리 몇 줄에 몰림, 일지 301).
	bool bLower = (Role == 1u && !bGrabRow) || Role == 2u || (Role == 4u && FootKind == 2u && LocalIndex - NO != SB_HardCore[FootCand]);
	bool bUpper = Role == 1u && FMax < 1e29;
	bool bRigidRow = Role == 4u || Role == 2u;
	uint RowCand = Role == 4u ? FootCand : HandCand;
	float Sign = Role == 2u ? -1.0 : 1.0;
	// 앞처리 대각: 조각 밖·받침 줄 K_ii(+ 반올림 보호). 손 칸 줄은 바퀴마다 고른 뒤.
	float MInv = 1.0;
	if (Role == 1u || Role == 4u)
	{
		MInv = 1.0 / max(SB_HardK(LocalIndex, LocalIndex, Pairs) + SB_PressureRowBound, 1e-12);
	}
	// 강체 후보 C3(재현 B3Pieces G3): 받침 줄마다 w_F = Σ_G K_FG A_G φ_G를 합 자리에, 후보 스레드가 Φ = Σ A φφᵀ, W = Σ A φ w_Fᵀ로 G₃ = Φ⁻¹WΦ⁻¹
	if (bAnyRigid)
	{
		if (bFoot && FootKind != 0u)
		{
			uint StartW = HardWork[SB_HARD_WORK_START + FootCand];
			uint CountW = HardWork[SB_HARD_WORK_COUNT + FootCand];
			float3 WF = float3(0, 0, 0);
			for (uint g = 0u; g < CountW; ++g)
			{
				float4 RowG = FRow[StartW + g];
				uint ColG = NO + StartW + g;
				float Kfg = SB_HardK(LocalIndex, ColG, Pairs) + (ColG == LocalIndex ? SB_PressureRowBound : 0.0);
				WF += (Kfg * RowG.w) * SB_HardBasis(FootCand, RowG.xy);
			}
			uint JW = LocalIndex - NO;
			SB_PressurePartial[2u * JW] = WF.x;
			SB_PressurePartial[2u * JW + 1u] = WF.y;
			SB_PressureSum[JW] = WF.z;
		}
		GroupMemoryBarrierWithGroupSync();
		if (LocalIndex < Cands && (SB_HardKind[LocalIndex] & 15u) != 0u)
		{
			uint StartG = HardWork[SB_HARD_WORK_START + LocalIndex];
			uint CountG = HardWork[SB_HARD_WORK_COUNT + LocalIndex];
			float3 P1 = float3(0, 0, 0);
			float3 P2 = float3(0, 0, 0);
			float3 W0 = float3(0, 0, 0);
			float3 W1 = float3(0, 0, 0);
			float3 W2 = float3(0, 0, 0);
			for (uint f3 = 0u; f3 < CountG; ++f3)
			{
				float4 RowF = FRow[StartG + f3];
				float3 Ph = SB_HardBasis(LocalIndex, RowF.xy);
				uint JF = StartG + f3;
				float3 Wv = float3(SB_PressurePartial[2u * JF], SB_PressurePartial[2u * JF + 1u], SB_PressureSum[JF]);
				P1 += RowF.w * Ph.x * Ph;
				P2 += RowF.w * float3(Ph.y * Ph.y, Ph.y * Ph.z, Ph.z * Ph.z);
				W0 += RowF.w * Ph.x * Wv;
				W1 += RowF.w * Ph.y * Wv;
				W2 += RowF.w * Ph.z * Wv;
			}
			float4 PkA;
			float2 PkB;
			SB_HardInvert3(P1.x, P1.y, P1.z, P2.x, P2.y, P2.z, SB_HardKind[LocalIndex] >> 4u, PkA, PkB);
			float3x3 Q = float3x3(SB_HardApply3(PkA, PkB, float3(1, 0, 0)), SB_HardApply3(PkA, PkB, float3(0, 1, 0)), SB_HardApply3(PkA, PkB, float3(0, 0, 1)));
			float3x3 Wm = float3x3(W0, W1, W2);
			Wm = 0.5 * (Wm + transpose(Wm));
			float3x3 G3 = mul(Q, mul(Wm, Q));
			SB_HardG3A[LocalIndex] = float4(G3[0][0], G3[0][1], G3[0][2], G3[1][1]);
			SB_HardG3B[LocalIndex] = float2(G3[1][2], G3[2][2]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	float F = 0.0;       // 이 풀이 줄의 값 z(힘·받침) — 늘 경계 안
	float S = 0.0;       // 이 풀이 줄의 (A z)
	float SF = 0.0;      // 이 행렬 줄의 s = K λ(z) — 강체 자세용(CG 걸음마다 이어서 고친다)
	float Res = 0.0;     // 자유 줄 잔차 r = g − A z
	float Dir = 0.0;
	float Z = 0.0;
	float Reg = 0.0;     // 강체 손 칸 줄 정칙화 ε c_k(그 밖 0)
	float2 RowXY = FootXY;   // 강체 줄의 판 자리(받침 = 발자국 칸, 손 칸 = 고른 접촉 점)
	bool bFixedLo = false;
	bool bFixedHi = false;
	bool bPickMoved = false;
	bool bPrevTrunc = false;     // 지난 바퀴 CG가 걸음을 잘랐다
	bool bPrevDone = false;      // 지난 바퀴 CG가 잔차·곡률로 끝났다(걸음 상한이 아니라)
	// 겉 굽힘이 있는 압력 칸(연속 굽힘)도 12 × 8 — 굽힘이 행렬 조건수를 100배 키워 6 × 4는 목표에서 0.04~0.38 cm 벗어난다(시험 캐릭터 가슴 오프라인).
	bool bHardRows = ValidRows > 0u || bAnyRigid || PressureHeader[SB_PRESSURE_HEADER_BEND] != 0u;
	uint RoundCap = bHardRows ? SB_HARD_ROUND_SCALE * PressureRounds : PressureRounds;
	uint StepCap = bHardRows ? SB_HARD_STEP_SCALE * PressureSteps : PressureSteps;
	uint Rounds = 0u;
	uint Steps = 0u;
	// 바퀴 RoundCap + 1번: 마지막 바퀴는 a ~ e(고르기·되살리기·곱하기·자세)만 하고 CG 없이 끝난다 — 꼬리의 자세가 끝 값과 맞는다. 끝내기 판정은 다음
	// 바퀴 시작(f)에서 한다. 도우미(조각별 합·투영·비트 합)를 부르는 자리를 하나씩만 두어 컴파일 시간·레지스터를 줄인 차례(재현 solve_b3와 같다).
	LOOP
	for (uint Round = 0u; Round <= RoundCap; ++Round)
	{
		// a. 판: 발자국 줄 s_F를 판 값 자리에. 강체 자세는 지난 바퀴의 곱수(SB_HardFit, 첫 바퀴 0).
		SB_HardPlates(LocalIndex, NO, NK, SF);
		// b. 손 칸 줄: 접촉 점·목표를 다시 고르고 앞처리 1/(ε c_k)
		if (bHand)
		{
			float GPrev = G;
			float2 Phi;
			float3 Pose = SB_HardFit[HandCand].xyz;
			SB_HardPick(HandRaw, Pose.x, SB_HardPoseGrad(HandCand, Pose), SB_HardRig[HandCand].xy, Phi, G);
			Reg = SB_HARD_HAND_EPS * max(SB_HardQuad(HandCand, SB_HardBasis(HandCand, Phi)), 1e-12);
			MInv = 1.0 / Reg;
			SB_HardPhi[Hand] = Phi;
			RowXY = Phi;
			bPickMoved = Round == 0u || abs(G - GPrev) > SB_HARD_PICK_SETTLE;
		}
		// c. 강체 평형 되살리기(둘째 바퀴부터 — 고른 점이 옮겨 평형 묶음이 바뀌었다): 자유 받침 줄만 M 계량으로 가장 적게 고치고, 떠 있는 받침이 음수가
		//    되면 0에 묶고 다시. 자유 받침 줄이 없는 후보는 손 칸 힘을 0으로(덜 눌림).
		if (Round > 0u && bAnyRigid)
		{
			LOOP
			for (uint Fix = 0u; Fix < SB_HARD_RESTORE_CAP; ++Fix)
			{
				bool bFreeNow = Role != 0u && !bFixedLo && !bFixedHi;
				SB_HardCandSolve(LocalIndex, Role == 4u && bFreeNow ? MInv : 0.0, bRigidRow ? F : 0.0, NO, NK, Cands, false);
				if (bRigidRow)
				{
					if (!(SB_HardPackA[RowCand].x > 0.0))
					{
						F = Role == 2u ? 0.0 : F;
					}
					else if (Role == 4u && bFreeNow)
					{
						F -= MInv * dot(SB_HardBasis(RowCand, RowXY), SB_HardSum[RowCand].xyz);
					}
				}
				bool bNeg = Role == 4u && bLower && !bFixedLo && F < 0.0;
				if (bNeg)
				{
					F = 0.0;
					bFixedLo = true;
				}
				if (SB_HardAnyBits(bNeg ? 1u : 0u, LocalIndex) == 0u)
				{
					break;
				}
			}
		}
		// d. 처짐 다시 곱하기(고른 점·되살림으로 행렬·값이 바뀌었다 — 첫 바퀴는 0)
		if (Round > 0u)
		{
			SB_HardWriteVector(LocalIndex, Role, F);
			GroupMemoryBarrierWithGroupSync();
			float LRow0;
			S = SB_HardMultiply(LocalIndex, NO, NK, Role, F, Reg, SF, LRow0);
		}
		// e. 투영 꾸러미와 곱수(강체 자세): 자유 줄의 Σ wφφᵀ, Σ sφM⁻¹(Az − g)
		bool bFreeRow = Role != 0u && !bFixedLo && !bFixedHi;
		if (bAnyRigid)
		{
			SB_HardCandSolve(LocalIndex, bRigidRow && bFreeRow ? MInv : 0.0, bRigidRow && bFreeRow ? MInv * (S - G) : 0.0, NO, NK, Cands, true);
		}
		// f. 묶인 줄 풀기(곱수 뺀 기울기가 그 줄을 경계 안쪽으로 당기면), 끝내기: 풀 줄도 옮겨 간 접촉 점도 없고 지난 바퀴 CG가 자르지 않고 끝났으면
		float Red = (S - G) - (bRigidRow ? Sign * dot(SB_HardBasis(RowCand, RowXY), SB_HardFit[RowCand].xyz) : 0.0);
		float Tol = 1e-5 * max(abs(G), abs(S)) + 1e-7;
		bool bRelease = Round > 0u && ((bFixedLo && Red < -Tol) || (bFixedHi && Red > Tol));
		if (bRelease)
		{
			bFixedLo = false;
			bFixedHi = false;
		}
		uint Bits = SB_HardAnyBits(bRelease || (bHand && bPickMoved) ? 1u : 0u, LocalIndex);
		if ((Round > 0u && Bits == 0u && bPrevDone && !bPrevTrunc) || Round == RoundCap)
		{
			break;
		}
		++Rounds;
		// g. 투영 PCG — 다시 시작(바퀴 처음·자른 걸음 뒤)은 걸음 고리 안 한 자리에서. 걸음은 첫 경계에서 자르고 그 줄을 묶는다(같은 비는 함께).
		bool bRestart = true;
		bool bTruncated = false;
		bool bDone = false;
		float RZ = 0.0;
		float RZ0 = 0.0;
		LOOP
		for (uint Step = 0u; Step <= StepCap; ++Step)
		{
			// 투영 앞처리. 다시 시작이면 r = g − Az를 새로 잡고, 경계에 있고 투영 방향이 경계 밖인 줄을 묶고 다시(SB_HARD_PREFIX_CAP번까지 — 남은 줄은
			// 길이 0 걸음이 묶는다).
			LOOP
			for (uint Pre = 0u; Pre <= SB_HARD_PREFIX_CAP; ++Pre)
			{
				bFreeRow = Role != 0u && !bFixedLo && !bFixedHi;
				Res = bRestart ? (bFreeRow ? G - S : 0.0) : Res;
				Z = SB_HardProject(LocalIndex, Res, MInv, bFreeRow, bRigidRow, RowCand, Sign, RowXY, bAnyRigid, NO, NK, Cands);
				if (!bRestart || Pre == SB_HARD_PREFIX_CAP)
				{
					break;
				}
				bool bBadLo = bFreeRow && bLower && F <= 0.0 && Z < 0.0;
				bool bBadHi = bFreeRow && bUpper && F >= FMax && Z > 0.0;
				if (SB_HardAnyBits(bBadLo || bBadHi ? 1u : 0u, LocalIndex) == 0u)
				{
					break;
				}
				bFixedLo = bFixedLo || bBadLo;
				bFixedHi = bFixedHi || bBadHi;
			}
			bFreeRow = Role != 0u && !bFixedLo && !bFixedHi;
			float RZNext = SB_PressureReduce(Res * Z, LocalIndex);
			Dir = bRestart ? Z : (bFreeRow ? Z + (RZNext / max(RZ, 1e-30)) * Dir : 0.0);
			RZ0 = Step == 0u ? RZNext : RZ0;
			RZ = RZNext;
			bRestart = false;
			if (!(RZ > max(1e-24, SB_HARD_CG_STOP * RZ0)))
			{
				bDone = true;
				break;
			}
			if (Step == StepCap)
			{
				break;
			}
			SB_HardWriteVector(LocalIndex, Role, Dir);
			GroupMemoryBarrierWithGroupSync();
			float SRow;
			float LRow;
			float Prod = SB_HardMultiply(LocalIndex, NO, NK, Role, Dir, Reg, SRow, LRow);
			float DKD = SB_PressureReduce(bFreeRow ? Dir * Prod : 0.0, LocalIndex);
			if (!(DKD > 1e-24))
			{
				bDone = true;
				break;
			}
			float Alpha = RZ / DKD;
			bool bHitLo = bFreeRow && bLower && Dir < 0.0;
			bool bHitHi = bFreeRow && bUpper && Dir > 0.0;
			float Ratio = bHitLo ? F / -Dir : (bHitHi ? (FMax - F) / Dir : 1e30);
			float AMax = SB_PressureReduceMin(Ratio, LocalIndex);
			++Steps;
			float Take = min(AMax, Alpha);
			F += Take * Dir;
			S += Take * Prod;
			SF += Take * SRow;
			if (AMax < Alpha)
			{
				// 첫 경계까지만 가고 그 줄을 묶는다 — 다음 걸음이 다시 시작한다
				if (Ratio == AMax)
				{
					F = bHitLo ? 0.0 : FMax;
					bFixedLo = bFixedLo || bHitLo;
					bFixedHi = bFixedHi || bHitHi;
				}
				bTruncated = true;
				bRestart = true;
			}
			else
			{
				Res -= bFreeRow ? Alpha * Prod : 0.0;
			}
		}
		bPrevTrunc = bTruncated;
		bPrevDone = bDone;
	}
	// 꼬리: 끝 값으로 한 번 더 곱해 발자국 줄 하중 λ_F와 s_F를 얻는다(값은 늘 경계 안 — 자르지 않는다)
	SB_HardWriteVector(LocalIndex, Role, F);
	GroupMemoryBarrierWithGroupSync();
	float SEnd;
	float LEnd;
	SB_HardMultiply(LocalIndex, NO, NK, Role, F, Reg, SEnd, LEnd);
	if (bRow)
	{
		PressureForceOut[LocalIndex] = bFree ? F : LEnd;   // 발자국 줄은 판이 편 하중(붙은 판은 음수도) — 긴 몫 모으기의 원천
	}
	if (LocalIndex == 0u)
	{
		PressureStatsOut[0] = Rounds;
		PressureStatsOut[1] = Steps;
	}
	SB_HardPlates(LocalIndex, NO, NK, SEnd);
	if (LocalIndex >= NO && LocalIndex < NK)
	{
		uint JT = LocalIndex - NO;
		uint CandT;
		uint FT;
		float2 XT;
		SB_HardFRow(JT, CandT, FT, XT);
		uint Packed = HardPieceCellList[HardPieceCellRange[HardWork[SB_HARD_WORK_PIECE + CandT]].x + FT];
		TiedPressureOut[int2(Packed & 0xffffu, Packed >> 16u)] = float2(LEnd, FRow[JT].w);
	}
	if (LocalIndex < Cands)
	{
		uint KindT = SB_HardKind[LocalIndex] & 15u;
		uint PieceT = HardWork[SB_HARD_WORK_PIECE + LocalIndex];
		float3 Pose = SB_HardFit[LocalIndex].xyz;
		uint2 HandsT = SB_HardRange[LocalIndex];
		if (KindT == 2u && HandsT.y == HandsT.x)
		{
			Pose = SB_HardRestingPose(LocalIndex, NO);
		}
		HardPieceMotionOut[PieceT] = float4(SB_HardPoseToFrame(LocalIndex, Pose), KindT == 2u ? 3.0 : 2.0);
	}
}
#endif

/**
 * 한 스레드 묶음이 0 ≤ f ≤ f_max(셀이 접힘), K f = g(닿은 칸), K f ≥ g(빠진 칸 — 겉면이 목표보다 깊다)를 활성 집합 켤레 기울기로 푼다(ADR-0037 §3.1 ③).
 * 바퀴마다: 닿은 칸의 잔차에서 CG를 PressureSteps번, 그 뒤 처짐 s = K v(v = 접힌 칸은 상한, 닿은 칸은 f, 빠진 칸은 0)로 집합을 고친다(음 힘 → 뺌, 상한
 * 넘음 → 접힘, 접힌 칸의 s > g → 되돌림, 빠진 칸의 s < g → 넣음). 집합이 그대로면 끝. 0에서 출발 — 지난 갱신의 해를 쓰지 않는다(상태 없음).
 * s는 따로 곱하지 않고 CG 곱셈 결과로 이어서 고친다(바퀴마다 온 행렬 곱셈 둘이 빠진다, 일지 283) — 집합을 고칠 때 v가 바뀌는 칸(뺌·접힘)만 곱해 더한다.
 * 반복 수 상한·조기 끝은 장벽 뒤 공유 메모리 값이라 묶음 안에서 같다(X3663). dᵀKd ≤ 0(랜드마크 하한 행렬은 양의 정부호가 보장되지 않는다)이면 그 바퀴의
 * CG를 멈춘다. 하드 서피스 순열은 판 줄과 함께 푼다(SB_HardSolve).
 */
[numthreads(SB_PRESSURE_SOLVE_THREADS, 1, 1)]
void ContactPressureSolveCS(uint LocalIndex : SV_GroupIndex)
{
#if SB_HARD_SURFACE
	SB_HardSolve(LocalIndex);
#else
	uint N = PressureHeader[SB_PRESSURE_HEADER_COUNT];
	uint Level = PressureHeader[SB_PRESSURE_HEADER_LEVEL];
	bool bRow = LocalIndex < N;
	uint4 Cell1 = bRow ? PressureCells[SB_PressureIndex(Level, LocalIndex, 1u)] : uint4(0, 0, 0, 0);
	if (bRow)
	{
		uint PairsB = (N + 1u) / 2u;
		float AbsSum = 0.0;
		for (uint PB = 0u; PB < PairsB; ++PB)
		{
			uint WordB = PressureMatrix[LocalIndex * PairsB + PB];
			AbsSum += abs(f16tof32(WordB)) + abs(f16tof32(WordB >> 16u));
		}
		SB_PressureRowBound = AbsSum * exp2(-11.0) + (float)N * exp2(-25.0);
	}
	float G = asfloat(Cell1.z);
	float FMax = bRow ? asfloat(Cell1.w) : 0.0;
	// 잡기 줄(ADR-0041, 힘 상한 자리가 음수): 늘 활성, 부호 자유, 상한 없음 — 판 풀이의 "붙은 판은 음수도"와 같은 꼴
	bool bGrabRow = bRow && FMax < 0.0;
	FMax = bGrabRow ? 1e30 : FMax;
	float F = 0.0;
	float Res = 0.0;
	float Dir = 0.0;
	float S = 0.0;   // 이 줄의 처짐 s = K v
	bool bActive = bRow;
	bool bCrushed = false;
	uint Rounds = 0u;
	uint Steps = 0u;
#define SB_PRESSURE_ROUND_CAP PressureRounds
#define SB_PRESSURE_STEP_CAP PressureSteps
	for (uint Round = 0u; Round < SB_PRESSURE_ROUND_CAP; ++Round)
	{
		// 잔차 r = g − K v(닿은 칸). CG 방향은 닿은 칸에서만 0이 아니라 곱셈 목록도 닿은 칸이다.
		Res = bActive ? G - S : 0.0;
		Dir = Res;
		float RR = SB_PressureReduce(Res * Res, LocalIndex);
		uint Count = SB_PressureListOf(bActive, LocalIndex);
		for (uint Step = 0u; Step < SB_PRESSURE_STEP_CAP; ++Step)
		{
			if (!(RR > 1e-24))
			{
				break;
			}
			if (bRow)
			{
				SB_PressureVector[LocalIndex] = Dir;
			}
			GroupMemoryBarrierWithGroupSync();
			float Prod = SB_PressureMultiply(LocalIndex, N, Count);
			float DKD = SB_PressureReduce(bActive ? Dir * Prod : 0.0, LocalIndex);
			if (!(DKD > 1e-24))
			{
				break;
			}
			float Alpha = RR / DKD;
			if (bActive)
			{
				F += Alpha * Dir;
				Res -= Alpha * Prod;
			}
			S += Alpha * Prod;   // v가 닿은 칸에서 α d만큼 바뀌었다 — 모든 줄의 처짐이 α K d만큼 바뀐다
			float RRNext = SB_PressureReduce(Res * Res, LocalIndex);
			float Beta = RRNext / RR;
			Dir = bActive ? Res + Beta * Dir : 0.0;
			RR = RRNext;
			++Steps;
		}
		// 집합 고치기. Delta = 이 칸의 v가 바뀐 양(뺌·접힘만 — 되돌린 칸은 상한 그대로, 넣은 칸은 0 그대로라 v가 같다).
		float Slack = 1e-6 * max(abs(G), 1e-3);
		float Delta = 0.0;
		bool bChanged = false;
		if (bRow && !bGrabRow)
		{
			if (bActive && F < 0.0)
			{
				Delta = -F;
				bActive = false;
				F = 0.0;
				bChanged = true;
			}
			else if (bActive && F > FMax)
			{
				Delta = FMax - F;
				bActive = false;
				bCrushed = true;
				F = FMax;
				bChanged = true;
			}
			else if (bCrushed && S > G + Slack)
			{
				bCrushed = false;
				bActive = true;
				bChanged = true;
			}
			else if (!bActive && !bCrushed && S < G - Slack)
			{
				bActive = true;
				bChanged = true;
			}
		}
		if (LocalIndex == 0u)
		{
			SB_PressureChanged = 0u;
		}
		GroupMemoryBarrierWithGroupSync();
		if (bChanged)
		{
			InterlockedOr(SB_PressureChanged, 1u);
		}
		uint DeltaCount = SB_PressureListOf(Delta != 0.0, LocalIndex);   // 안의 장벽 뒤라 표지가 모두 모였다
		uint Changed = SB_PressureChanged;
		++Rounds;
		if (Changed == 0u)
		{
			break;
		}
		if (DeltaCount > 0u && Round + 1u < SB_PRESSURE_ROUND_CAP)
		{
			if (bRow)
			{
				SB_PressureVector[LocalIndex] = Delta;
			}
			GroupMemoryBarrierWithGroupSync();
			S += SB_PressureMultiply(LocalIndex, N, DeltaCount);
		}
	}
	if (bRow)
	{
		PressureForceOut[LocalIndex] = bGrabRow ? F : (bCrushed ? FMax : (bActive ? clamp(F, 0.0, FMax) : 0.0));
	}
	if (LocalIndex == 0u)
	{
		PressureStatsOut[0] = Rounds;
		PressureStatsOut[1] = Steps;
	}
#undef SB_PRESSURE_ROUND_CAP
#undef SB_PRESSURE_STEP_CAP
#endif
}

// 긴 몫 걸음 나누기(일지 284): 묶음 하나(거친 칸 8×8)를 조각 SB_LONG_SLICES개(스레드 64 × 조각)로 돌린다 — 끌림 걸음 나누기(일지 191)와 같은 꼴이다.
// 조각마다 같은 칸 64개를 맡아 올린 압력 칸을 비트 자리로 번갈아 나눠(낱말 안 자리 % 조각 수) 돌고, 끝에 조각 0이 다른 조각의 부분합을 조각 차례로
// 더한다(원자 없이 정해진 차례라 결정적이다). 압력 칸 하나의 걸음은 앞 결과를 기다리는 사슬(랜드마크 하한 → 표 읽기)이고 셈할 묶음이 SM 수 안팎이라,
// 나누기 전에는 SM 하나에 워프 둘이 홀로 걸었다. 나누면 가장 긴 사슬이 1/조각 수가 되고, 늘어난 워프가 서로의 기다림을 가린다.
//  - 공유 메모리 표지·상자·압력 칸 올림·칸 출력은 조각 0만 한다. 거리표 줄 올리기는 조각끼리 랜드마크 쌍을 나눈다.
//  - 8인 까닭(개발 PC, 멈춘 누름, 눌린 장 하나): 조각 1 → 4 → 8 → 16이 0.14~0.33 → 0.05~0.11 → 0.03~0.08 → 0.03~0.06 ms. 16은 묶음이 1024 스레드라
//    SM 하나에 스레드 1536개인 GPU(GeForce RTX 30~50)에 묶음이 하나만 들어, 셈할 묶음이 SM보다 많으면 오히려 덜 찬다 — 512면 셋이 든다. C++는 조각 수를
//    모른다(디스패치는 거친 묶음 수 그대로).
#define SB_LONG_SLICES 8
#if SB_LONG_SLICES != 1 && SB_LONG_SLICES != 2 && SB_LONG_SLICES != 4 && SB_LONG_SLICES != 8 && SB_LONG_SLICES != 16
#error SB_LONG_SLICES must be 1, 2, 4, 8 or 16 (slice bit masks and the landmark pair split assume it)
#endif
groupshared float4 SB_LongCell[SB_COARSE_CELLS];      // xyz 정지 위치(메시 로컬), w = 1 셈할 칸
groupshared float4 SB_LongCellMat[SB_COARSE_CELLS];   // x 두께 cm, y 흐린 v, z 흐린 겉껍질 cm, w 흐린 겉 굽힘 길이 cm(Flex 순열만 — 아니면 0)
groupshared uint4 SB_LongSource[SB_COARSE_CHUNK * SB_PRESSURE_GEOM_ROWS];   // 올린 압력 칸 64개(7 KB) — [1].w 자리에 힘 f
groupshared uint SB_LongNearMask[2][2];   // [홀짝][낱말] 올린 칸 가운데 상자에서 닿는 것
groupshared uint SB_LongSourcePiece[SB_COARSE_CHUNK];   // 같은 조각 문: 올린 압력 칸의 살 조각 번호(칸 기록 [12].z, SB_PressurePiece)
groupshared uint SB_LongCellPiece[SB_COARSE_CELLS];     // 같은 조각 문: 묶음 칸의 살 조각 번호(그 칸의 거리표 줄, SB_PressurePiece)
#if SB_SEPARATION
groupshared float3 SB_LongSeparationW[SB_COARSE_CHUNK];   // 부위 나누기: 올린 압력 칸의 채널 출처 무게 (s_a, s_b, q)
#endif
#if SB_LONG_SLICES > 1
groupshared float SB_LongPartial[(SB_LONG_SLICES - 1) * SB_COARSE_CELLS];   // 조각 1..의 부분합 [조각 − 1][칸]
#endif
#if SB_HARD_SURFACE
groupshared float SB_LongSourceBend[SB_COARSE_CHUNK];   // 연속 굽힘: 올린 압력 칸의 겉 굽힘 길이(cm, 칸 기록 [12].w)
#if SB_LONG_SLICES > 1
groupshared float SB_LongPartialBend[(SB_LONG_SLICES - 1) * SB_COARSE_CELLS];   // W_c 조각 1..의 부분합(1.75 KB — 묶음 공유 메모리 최대 30,764 B, 32 KB 한도 안: 하드 서피스 + 부위 나누기 순열, 같은 조각 문 포함, fxc 목록으로 잼)
#endif
#endif

/** 조각 Slice가 걷는 올린 압력 칸 비트(낱말 안 자리 % SB_LONG_SLICES == Slice). 이어진 칸이 조각마다 고루 나뉜다. */
uint SB_LongSliceMask(uint Slice)
{
#if SB_LONG_SLICES == 16
	return 0x00010001u << Slice;
#elif SB_LONG_SLICES == 8
	return 0x01010101u << Slice;
#elif SB_LONG_SLICES == 4
	return 0x11111111u << Slice;
#elif SB_LONG_SLICES == 2
	return 0x55555555u << Slice;
#else
	return 0xffffffffu;
#endif
}

/**
 * 묶음의 칸들을 준비한다. 셈할 칸 = 살이 있고 압력 칸 상자(무게 중심의 최소·최대를 반경 PressureReachLayers × 살 두께 최대만큼 넓힌 것) 안인 칸.
 * 압력 칸이 없거나 셈할 칸이 없으면 false(모든 스레드가 같은 값 — 장벽 뒤). 셈할 칸의 거리표 줄을 SB_CoarseRows에 올린다(조각끼리 랜드마크 쌍을
 * 나눈다). 칸 자리·재료·상자·표지는 조각 0이 쓴다.
 */
bool SB_LongStage(int2 Cell, uint Local, uint Slice, uint N)
{
	bool bLead = Slice == 0u;
	bool bInside = false;
	if (bLead && Local == 0u)
	{
		SB_CoarseActiveCells = 0u;
	}
	if (bLead && Local < 2u)
	{
		SB_LongNearMask[0][Local] = 0u;
	}
	if (bLead && Local < 3u)
	{
		SB_CoarseBoxLo[Local] = 0xffffffffu;
		SB_CoarseBoxHi[Local] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	float4 Here = float4(0, 0, 0, 0);
	float4 Mat = float4(0, 0, 0, 0);
	if (N > 0u && SB_InCoarse(Cell))
	{
		float4 Info = CoarseInfo.Load(int3(Cell, 0));
		if (Info.w > 0.0)
		{
			float Reach = PressureReachLayers * (float)PressureHeader[SB_SOURCE_HEADER_SIGMA] * 0.001 / ThicknessScale;
#if SB_HARD_SURFACE
			// 연속 굽힘: W_c는 센 굽힘에서 3ℓ_b까지 닿는다(자산 거친 칸 흐린 굽힘 길이 최대 — 흰 자산은 0이라 상자 그대로)
			Reach = max(Reach, 3.0 * (float)CoarseBendMax[0] * 0.001 / ThicknessScale);
#endif
			float3 Lo = float3(SB_OrderedToFloat(PressureHeader[SB_SOURCE_HEADER_MIN]), SB_OrderedToFloat(PressureHeader[SB_SOURCE_HEADER_MIN + 1u]),
				SB_OrderedToFloat(PressureHeader[SB_SOURCE_HEADER_MIN + 2u]));
			float3 Hi = float3(SB_OrderedToFloat(PressureHeader[SB_SOURCE_HEADER_MAX]), SB_OrderedToFloat(PressureHeader[SB_SOURCE_HEADER_MAX + 1u]),
				SB_OrderedToFloat(PressureHeader[SB_SOURCE_HEADER_MAX + 2u]));
			bInside = all(Info.xyz >= Lo - Reach) && all(Info.xyz <= Hi + Reach);
			if (bInside && bLead)
			{
				float2 Material = CoarseMaterial.Load(int3(Cell, 0));
				Here = float4(Info.xyz, 1.0);
#if SB_HARD_SURFACE
				float BendHere = CoarseBend.Load(int3(Cell, 0));   // 받는 칸 흐린 겉 굽힘 길이(cm — 0이면 W_c를 모으지 않는다)
#else
				float BendHere = 0.0;
#endif
				Mat = float4(max(CoarseLayer.Load(int3(Cell, 0)).x * ThicknessScale, 1e-3), Material.x, Material.y * ThicknessScale, BendHere);
				InterlockedAdd(SB_CoarseActiveCells, 1u);
				UNROLL
				for (uint Axis = 0; Axis < 3u; ++Axis)
				{
					InterlockedMin(SB_CoarseBoxLo[Axis], SB_OrderedFloat(Info[Axis]));
					InterlockedMax(SB_CoarseBoxHi[Axis], SB_OrderedFloat(Info[Axis]));
				}
			}
		}
	}
	if (bLead)
	{
		SB_LongCell[Local] = Here;
		SB_LongCellMat[Local] = Mat;
	}
	if (bInside)
	{
		// 거리표 줄: 랜드마크 쌍 (Pair × 조각 수 + 조각)을 이 조각이 올린다(쌍마다 한 조각이 한 번)
		uint Row = CoarseRow.Load(int3(Cell, 0));
		if (bLead)
		{
			SB_LongCellPiece[Local] = SB_PressurePiece(Row);   // 같은 조각 문 — 압력 칸과 같은 규칙으로 이 칸의 줄에서
		}
		UNROLL
		for (uint Pair = 0u; Pair < SB_LANDMARK_MAX / 2u / SB_LONG_SLICES; ++Pair)
		{
			uint M = 2u * (Pair * SB_LONG_SLICES + Slice);
			uint Lo16 = M < NumLandmarks ? SB_LandmarkValue(Row, M) : SB_LANDMARK_UNREACHABLE;
			uint Hi16 = M + 1u < NumLandmarks ? SB_LandmarkValue(Row, M + 1u) : SB_LANDMARK_UNREACHABLE;
			SB_CoarseRows[(M >> 1u) * SB_COARSE_CELLS + Local] = Lo16 | (Hi16 << 16u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	return SB_CoarseActiveCells != 0u;
}

/**
 * 압력 칸 Chunk번째 64개를 올리고 묶음 상자에서 닿는 것(반경 PressureReachLayers × 그 칸 두께)만 표지를 켠다. 모든 스레드가 부른다(장벽 둘), 올림은
 * 조각 0이. 일곱 줄과 힘을 한꺼번에 읽어 전역 읽기 왕복이 하나다 — 닿지 않는 칸의 줄도 자리에 쓰지만 표지가 꺼져 있어 아무도 읽지 않는다.
 */
void SB_LongStageChunk(uint Chunk, uint N, uint Level, uint Local, uint Slice)
{
	uint Parity = Chunk & 1u;
	GroupMemoryBarrierWithGroupSync();
	if (Slice == 0u)
	{
		if (Local < 2u)
		{
			SB_LongNearMask[1u - Parity][Local] = 0u;
		}
		uint Source = Chunk * SB_COARSE_CHUNK + Local;
		if (Source < N)
		{
#if SB_HARD_SURFACE
			// 하드 서피스: 조각 밖 압력 칸 다음 자리는 발자국 줄 — 판이 편 하중 λ_F(기울기·굽힘 몫은 음수도)
			bool bTiedSource = Source >= HardWork[SB_HARD_WORK_FREE];
#endif
			float Force = PressureForce[Source];
			uint4 Rows[SB_PRESSURE_GEOM_ROWS];
			UNROLL
			for (uint R = 0u; R < SB_PRESSURE_GEOM_ROWS; ++R)
			{
				Rows[R] = PressureCells[SB_PressureIndex(Level, Source, R)];
			}
			uint Slot7 = Local * SB_PRESSURE_GEOM_ROWS;
			UNROLL
			for (uint R2 = 0u; R2 < SB_PRESSURE_GEOM_ROWS; ++R2)
			{
				SB_LongSource[Slot7 + R2] = R2 == 1u ? uint4(Rows[1].xyz, asuint(Force)) : Rows[R2];
			}
			SB_LongSourcePiece[Local] = PressureCells[SB_PressureIndex(Level, Source, SB_PRESSURE_LOCAL_ROW)].z;   // 같은 조각 문 — 칸 기록 [12].z
#if SB_SEPARATION
#if SB_HARD_SURFACE
			// 발자국 줄은 부위 몫을 모르므로 부위 밖으로 본다(배율 1 — 양쪽에 건넨다)
			SB_LongSeparationW[Local] = bTiedSource ? float3(0, 0, 0) : SB_SeparationWeights(PressureSeparation[Level * SB_PRESSURE_CAP + Source]);
#else
			SB_LongSeparationW[Local] = SB_SeparationWeights(PressureSeparation[Level * SB_PRESSURE_CAP + Source]);
#endif
#endif
			float3 Position = asfloat(Rows[0].xyz);
			float3 Lo = float3(SB_OrderedToFloat(SB_CoarseBoxLo[0]), SB_OrderedToFloat(SB_CoarseBoxLo[1]), SB_OrderedToFloat(SB_CoarseBoxLo[2]));
			float3 Hi = float3(SB_OrderedToFloat(SB_CoarseBoxHi[0]), SB_OrderedToFloat(SB_CoarseBoxHi[1]), SB_OrderedToFloat(SB_CoarseBoxHi[2]));
			float3 Outside = max(max(Lo - Position, Position - Hi), float3(0, 0, 0));
			float Gap = length(Outside) * ThicknessScale;
#if SB_HARD_SURFACE
			// 연속 굽힘: 원천 겉 굽힘 길이(발자국 줄은 0)와, W_c가 닿는 반경 3·max(원천, 받는 칸 최대) 굽힘 길이(흰 자산은 둘 다 0 — 지금 반경 그대로)
			float SourceBend = asfloat(PressureCells[SB_PressureIndex(Level, Source, SB_PRESSURE_LOCAL_ROW)].w);
			SB_LongSourceBend[Local] = SourceBend;
			float BendReachCm = 3.0 * max(SourceBend, (float)CoarseBendMax[0] * 0.001);
			if (Force != 0.0 && Gap <= max(PressureReachLayers * asfloat(Rows[1].x), BendReachCm))
#else
			// 잡기 줄(ADR-0041)은 음의 힘도 — 둘레가 꺼진다
			if (Force != 0.0 && Gap <= PressureReachLayers * asfloat(Rows[1].x))
#endif
			{
				InterlockedOr(SB_LongNearMask[Parity][Local >> 5u], 1u << (Local & 31u));
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
}

/**
 * 고른 단계 Level(≥ 1)의 압력 칸 격자에서 거친 칸 Cell의 압력 p를 값을 지나는 3차로 읽는다. 붙들지 않은 묶음은 0(재현과 같다 — 가장자리 압력이 바깥 0으로
 * 당겨진다), 다른 차트·목록 밖 묶음은 빼고 무게로 다시 나눈다. 묶음 하나에 한 값을 채우면 손 가장자리의 압력 몰림이 묶음째 펴져 가장자리 칸 목표가 묶음
 * 격자 모양으로 몇 mm 어긋났다(일지 277 리뷰).
 */
float SB_BlockPressureAt(int2 Cell, uint Level, uint N, float3 RestP)
{
	uint Side = 1u << Level;
	float2 Q = ((float2)Cell + 0.5) / (float)Side - 0.5;
	int2 First = (int2)floor(Q) - 1;
	float2 T = Q - floor(Q);
	float4 WX = SB_CatmullRomWeights(T.x);
	float4 WY = SB_CatmullRomWeights(T.y);
	float BlockLocal = (float)(CoarseBlock * Side) * TexelSize / ThicknessScale;
	float Sum = 0.0;
	float Weight = 0.0;
	float SumLin = 0.0;
	float WeightLin = 0.0;
	UNROLL
	for (int Y = 0; Y < 4; ++Y)
	{
		UNROLL
		for (int X = 0; X < 4; ++X)
		{
			int2 Corner = (First + int2(X, Y)) * (int)Side;
			if (any(Corner < int2(0, 0)) || !SB_InCoarse(Corner))
			{
				continue;
			}
			// 같은 차트인가: 붙든 묶음은 압력 칸 무게 중심, 아니면 묶음 가운데 거친 칸(빈 칸이면 뺀다)
			float Value = 0.0;
			float Near = 0.0;
			bool bListedBlock = SB_TileListed(Corner * (int)CoarseBlock);
			uint Slot = bListedBlock ? (PressureSlot.Load(int3(Corner, 0)) >> (10u * Level)) & SB_PRESSURE_NO_SLOT : SB_PRESSURE_NO_SLOT;
			if (Slot < N)
			{
				uint4 Row0 = PressureCells[SB_PressureIndex(Level, Slot, 0u)];
				Value = PressureForce[Slot] / max(asfloat(Row0.w), 1e-6);
				Near = saturate(3.0 - 0.5 * length(asfloat(Row0.xyz) - RestP) / BlockLocal);
			}
			else if (bListedBlock)
			{
				int2 Mid = min(Corner + (int)(Side / 2u), (int)CoarseResolution - 1);
				Near = SB_CoarseTapWeight(Mid, RestP, BlockLocal);
			}
			if (Near <= 0.0)
			{
				continue;
			}
			float W = WX[X] * WY[Y] * Near;
			Sum += W * Value;
			Weight += W;
			if (X >= 1 && X <= 2 && Y >= 1 && Y <= 2)
			{
				float WL = (X == 1 ? 1.0 - T.x : T.x) * (Y == 1 ? 1.0 - T.y : T.y) * Near;
				SumLin += WL * Value;
				WeightLin += WL;
			}
		}
	}
	float Linear = WeightLin > 1e-6 ? SumLin / WeightLin : 0.0;
	float Blend = saturate((Weight - 0.3) / 0.4);
	return Blend > 0.0 ? lerp(Linear, Sum / max(Weight, 1e-6), Blend) : Linear;
}

/**
 * 거친 칸마다 긴 몫 L = −Σ_j f_j ĝ_long(d/h̄)/h̄(솟음 +, 원천에서 PressureReachLayers × h̄ 안 — 그 밖은 표의 끝자락)과, 그 칸이 든 압력 칸의 거친 압력
 * p = f/A(함몰 목록 묶음 안, 고른 단계)를 쓴다. 모든 칸에 쓴다(원천이 닿지 않는 칸은 (0, 0) — 합치기·함몰이 어디서나 읽는다). 거리 = max(직선, 랜드마크
 * 하한 — 이 칸의 줄 전부와 원천의 가까운 랜드마크), 가까운 원천(d < 2·지름)은 원천 원판 위 31점 평균. 다른 살 조각의 원천은 더하지 않는다(같은 조각
 * 문 — 압력 행렬과 같은 SB_PressurePiece 번호). 묶음 하나를 조각 SB_LONG_SLICES개가 압력 칸을
 * 나눠 돈다(걸음 나누기 — SB_LONG_SLICES 주석).
 */
[numthreads(SB_COARSE_GROUP, SB_COARSE_GROUP, SB_LONG_SLICES)]
void ContactLongGatherCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID)
{
	int2 Cell = int2(GroupId.xy * SB_COARSE_GROUP + LocalId.xy);
	uint Slice = LocalId.z;                                // 걸음 나누기 조각(SB_LONG_SLICES)
	uint Local = LocalId.y * SB_COARSE_GROUP + LocalId.x;   // 묶음 안 칸 — 조각마다 같은 칸 64개
	uint N = PressureHeader[SB_PRESSURE_HEADER_COUNT];
	uint Level = PressureHeader[SB_PRESSURE_HEADER_LEVEL];
	bool bGroupActive = SB_LongStage(Cell, Local, Slice, N);   // 하드 서피스 순열은 N에 발자국 줄이 든다(조각 밖 칸 다음 자리)
	uint Count = bGroupActive ? N : 0u;   // 셈할 칸이 없으면 돌지 않는다(장벽 뒤 공유 메모리 값 — 묶음 안에서 같다)
	float4 Here = SB_LongCell[Local];
	float4 Mat = SB_LongCellMat[Local];
	uint CellPiece = SB_LongCellPiece[Local];   // 셈하지 않는 칸(Here.w 0)은 읽지 않는다
	uint SliceMask = SB_LongSliceMask(Slice);
	float Long = 0.0;
#if SB_SEPARATION
	float3 Chan = float3(0, 0, 0);   // 부위 나누기: 출처 무게 (s_a, s_b, q)로 모은 긴 몫
#endif
#if SB_HARD_SURFACE
	float BendSum = 0.0;   // 굽힘 거친 해 W_c(연속 굽힘)
#endif
	for (uint Chunk = 0u; Chunk * SB_COARSE_CHUNK < Count; ++Chunk)
	{
		SB_LongStageChunk(Chunk, Count, Level, Local, Slice);
		if (Here.w > 0.0)   // 셈하지 않는 칸은 올리기만 돕는다(장벽 바깥의 분기)
		{
			UNROLL
			for (uint Word = 0u; Word < 2u; ++Word)
			{
				uint Mask = SB_LongNearMask[Chunk & 1u][Word] & SliceMask;
				while (Mask != 0u)
				{
					uint Bit = firstbitlow(Mask);
					Mask &= Mask - 1u;
					// 같은 조각 문(motive-mem ②): 이어지지 않은 다른 살 조각의 원천은 건너뛴다 — 압력 행렬(SB_PressureEntry)과 같은 번호를 본다.
					// 표면 거리 하한만으로는 랜드마크 없는 작은 조각끼리(둘 다 "닿지 않음"이라 하한 0) 직선으로 이어졌다.
					if (SB_LongSourcePiece[Word * 32u + Bit] != CellPiece)
					{
						continue;
					}
					uint Slot7 = (Word * 32u + Bit) * SB_PRESSURE_GEOM_ROWS;
					uint4 Row0 = SB_LongSource[Slot7];
					uint4 Row1 = SB_LongSource[Slot7 + 1u];
					uint4 Row2 = SB_LongSource[Slot7 + 2u];
					float Hb = max(min(Mat.x, asfloat(Row1.x)), 1e-3);
					float Cut = PressureReachLayers * Hb;   // 긴 몫 L의 반경
#if SB_HARD_SURFACE
					// 연속 굽힘: 받는 칸이 굽으면 W_c를 표 끝 max(6, 3δ̄)·h̄까지(흰 받는 칸은 0 — 반경·셈 지금 그대로)
					float DeltaBar = 0.5 * (Mat.w + SB_LongSourceBend[Word * 32u + Bit]) / Hb;
					float BendCut = Mat.w > 0.0 ? max(PressureReachLayers, 3.0 * DeltaBar) * Hb : 0.0;
					Cut = max(Cut, BendCut);
#endif
					float Chord = length(Here.xyz - asfloat(Row0.xyz)) * ThicknessScale;
					if (Chord > Cut)
					{
						continue;
					}
					uint Lower = 0u;
					UNROLL
					for (uint R = 0; R < 4u; ++R)
					{
						uint4 Near = SB_LongSource[Slot7 + 3u + R];
						UNROLL
						for (uint c = 0; c < 4u; ++c)
						{
							uint Dj = Near[c] >> 8u;
							uint Di = SB_LandmarkRowValue(Local, Near[c] & 0xffu);
							Lower = max(Lower, Di > Dj ? Di - Dj : Dj - Di);
						}
					}
					float Dist = max(Chord, (float)Lower * SB_LANDMARK_UNIT * ThicknessScale / SB_PRESSURE_BOUND_TOL - asfloat(Row2.w));
					if (Dist > Cut)
					{
						continue;
					}
					float Vb = 0.5 * (Mat.y + asfloat(Row2.x));
					float Tau = 0.5 * (Mat.z + asfloat(Row2.y)) / Hb;
					float Rad = sqrt(asfloat(Row0.w) / PI);
					BRANCH
					if (Dist <= PressureReachLayers * Hb)
					{
						float Kernel = 0.0;
						BRANCH
						if (Dist < 4.0 * Rad)
						{
							Kernel = SB_PressureLongDisc(Dist, Rad, Hb, Vb, Tau);
						}
						else
						{
							Kernel = SB_PressureLong(Dist / Hb, Vb, Tau);
						}
						float Term = asfloat(Row1.w) * Kernel / Hb;
						Long -= Term;
#if SB_SEPARATION
						Chan -= Term * SB_LongSeparationW[Word * 32u + Bit];
#endif
					}
#if SB_HARD_SURFACE
					// 굽힘 거친 해 W_c(부위 나누기 채널은 나누지 않는다 — 굽는 겉은 두 부위를 잇는다)
					BRANCH
					if (Dist < BendCut)
					{
						float Full = Dist < 4.0 * Rad ? SB_PressureFullDisc(Dist, Rad, Hb, Vb, Tau, DeltaBar) : SB_PressureFull(Dist / Hb, Vb, Tau, DeltaBar);
						BendSum += asfloat(Row1.w) * Full / Hb;
					}
#endif
				}
			}
		}
	}
#if SB_LONG_SLICES > 1
	// 부분합 더하기: 조각 1..이 제 부분합을 [조각 − 1][칸]에 쓰고 장벽 뒤 조각 0이 조각 차례로 더한다. 원천이 없는 묶음도 같은 길을 간다(0을 더한다 — 장벽이
	// 균일한 흐름에 있다). 이 뒤로 장벽이 없어 조각 1..은 여기서 끝난다.
	if (Slice != 0u)
	{
		SB_LongPartial[(Slice - 1u) * SB_COARSE_CELLS + Local] = Long;
#if SB_HARD_SURFACE
		SB_LongPartialBend[(Slice - 1u) * SB_COARSE_CELLS + Local] = BendSum;
#endif
	}
#if SB_SEPARATION
	// 부위 나누기: 채널 셋의 부분합(5.25 KB)은 따로 둘 자리가 없어(묶음 공유 메모리 32 KB 한도) 거리표 줄 자리(SB_CoarseRows, 16 KB)에 쓴다 — 모든
	// 조각이 줄을 다 읽은 뒤라 장벽이 하나 더 든다. 이 순열에만 있다(부위 나누기가 없는 장은 장벽 하나 그대로).
	GroupMemoryBarrierWithGroupSync();
	if (Slice != 0u)
	{
		uint Base = (Slice - 1u) * 3u * SB_COARSE_CELLS + Local;
		SB_CoarseRows[Base] = asuint(Chan.x);
		SB_CoarseRows[Base + SB_COARSE_CELLS] = asuint(Chan.y);
		SB_CoarseRows[Base + 2u * SB_COARSE_CELLS] = asuint(Chan.z);
	}
#endif
	GroupMemoryBarrierWithGroupSync();
	if (Slice != 0u)
	{
		return;
	}
	UNROLL
	for (uint Part = 1u; Part < SB_LONG_SLICES; ++Part)
	{
		Long += SB_LongPartial[(Part - 1u) * SB_COARSE_CELLS + Local];
#if SB_HARD_SURFACE
		BendSum += SB_LongPartialBend[(Part - 1u) * SB_COARSE_CELLS + Local];
#endif
#if SB_SEPARATION
		uint Base = (Part - 1u) * 3u * SB_COARSE_CELLS + Local;
		Chan += float3(asfloat(SB_CoarseRows[Base]), asfloat(SB_CoarseRows[Base + SB_COARSE_CELLS]), asfloat(SB_CoarseRows[Base + 2u * SB_COARSE_CELLS]));
#endif
	}
#endif
	if (SB_InCoarse(Cell))
	{
		float Pressure = 0.0;
		BRANCH
		if (N > 0u && SB_TileListed(Cell * (int)CoarseBlock))
		{
			BRANCH
			if (Level == 0u)
			{
				// 단계 0: 압력 칸 = 거친 칸 — 가는 칸이 거친 칸 사이를 3차로 읽는다
				uint Slot = PressureSlot.Load(int3(Cell, 0)) & SB_PRESSURE_NO_SLOT;
				if (Slot < N)
				{
					Pressure = PressureForce[Slot] / max(asfloat(PressureCells[SB_PressureIndex(0u, Slot, 0u)].w), 1e-6);
				}
			}
			else
			{
				Pressure = SB_BlockPressureAt(Cell, Level, N, CoarseInfo.Load(int3(Cell, 0)).xyz);
			}
		}
#if SB_HARD_SURFACE
		// 발자국 거친 칸: 묶인 힘·넓이를 더한다(재현 — 블록마다 실린 힘 ÷ 붙든 넓이, 닿지 않은 조각은 힘 0). 단계 ≥ 1은 묶인 칸 압력만.
		float2 TiedHere = TiedPressure.Load(int3(Cell, 0));
		if (TiedHere.y > 0.0)
		{
			float FreeForce = 0.0;
			float FreeArea = 0.0;
			if (N > 0u && Level == 0u && SB_TileListed(Cell * (int)CoarseBlock))
			{
				uint FreeSlot = PressureSlot.Load(int3(Cell, 0)) & SB_PRESSURE_NO_SLOT;
				if (FreeSlot < N)
				{
					FreeForce = PressureForce[FreeSlot];
					FreeArea = asfloat(PressureCells[SB_PressureIndex(0u, FreeSlot, 0u)].w);
				}
			}
			else if (Level > 0u)
			{
				// 묶은 단계: 이 거친 칸의 조각 밖 몫(칸 넓이 − 묶인 넓이)은 블록 압력으로 본다 — 발자국 가장자리 칸의 둘레 살 압력이 빠지지 않게
				FreeArea = max(CoarseInfo.Load(int3(Cell, 0)).w * ThicknessScale * ThicknessScale - TiedHere.y, 0.0);
				FreeForce = Pressure * FreeArea;
			}
			Pressure = (FreeForce + TiedHere.x) / max(FreeArea + TiedHere.y, 1e-6);
		}
#endif
		ContactLongOut[Cell] = float2(Long, Pressure);
#if SB_HARD_SURFACE
		ContactBendOut[Cell] = BendSum;
#endif
#if SB_SEPARATION
		ContactLongSeparationOut[Cell] = float4(Chan, 0.0);
#endif
	}
}

// --- 끌림: 원천 커널 합, 붙은 칸은 바로잡아 붙든 자리에 맞춘다 (ADR-0033, 일지 183·184) ---
// 끌림 G는 어디서나 한 식 — 원천(눌린 거친 칸과 뗀 뒤 줄어드는 기억)들의 K0 커널 합이다(솟음 ADR-0031과 같은 꼴, 반복 없음). 원천의 끄는 힘 Q는
// 칸마다 갱신을 넘어 남고 갱신마다 지난 Q에서 이어 고친다. 패스 셋: 원천 모으기 DragSourceCS, 바로잡기 DragCorrectCS(C++ DragCorrectPasses번),
// 모으기 DragGatherCS.
//  - 식: 살층 한 식(ADR-0019)의 정적 평형 G − ℓ²∇²G = Q의 그린 함수가 K(d) = K0(d/ℓ)/(2πℓ²)다(평면에서 ∫K = 1). 원천이 성기므로(손 하나에 10~300칸)
//    커널 합이 한 패스에 끝난다. 뗀 칸의 Q는 함몰 기억(ADR-0032)과 같이 e^{−RΔt}로 줄어 모든 모양이 정확히 복원율 R로 돌아온다.
//  - 붙음·미끄러짐(쿨롱): 눌린 칸은 붙든 자리 H(손을 따라 쌓인 자리)를 가진다. 붙은 칸은 G = H가 되게 Q를 고친다(Q += w (H − G)). 고친 Q가 버틸 수 있는
//    힘 Q_lim = w R(φ(R) + (w − 1)R = 마찰 한도, 살 법칙 SB_TissueLaw)을 넘으면 한도 길이로 잘리고 그 칸은 미끄러진다 — 미끄러지는 칸의 Q는 마찰 힘이고
//    붙든 자리는 보이는 자리를 따라간다. 판정이 칸이 실제로 내는 힘(이웃 칸들과 같이 푼 Q)을 보므로 붙은 칸 사이·가장자리의 당김이 다 든다.
//    w = max(1/덮인 몫, 가장자리 배수)는 바로잡기의 걸음 크기다.
//  - 지난 Q에서 잇기 때문에 칸이 눌림 집합을 드나들어도 G가 뛰지 않는다: 새로 누른 칸은 H = 지금 G라 고칠 것이 없다. 일지 179는 붙든 칸에 H, 꼬리에
//    커널 합을 따로 보여 경계에서 끊겼고(가장자리 칸이 드나들 때 0.75 mm 뜀 — 가슴의 깜빡임), 칸마다 따로 자른 H가 그대로 보였다(손 밑 이웃 단차
//    2.4 mm — 은박지). 일지 182·183.
//  - 거리 = 정지 위치 3D 직선. 굽기 랜드마크 하한(솟음 거리표)이 직선의 SB_DRAG_BOUND_TOL배를 넘으면(접힌 곳·다른 부위) 하한으로 — 이음매는 표 없이
//    건넌다. 마주 보는 칸(접힌 틈)은 가린다. 원천 벡터는 원천 노멀 → 칸 노멀 최소 회전으로 더하고 접평면에 눕힌다.
//  - 끌림 거친 격자 = 솟음 거친 격자(정지 위치·넓이 표 CoarseInfo, 거리표 줄 CoarseRow를 같이 쓴다). 거리표 없는 베이크는 끌림이 꺼진다(C++).
// 상태(갱신을 넘어 남는다, 두 장): DragState(xyz 끌림 G; w = 눌렸으면 1 + 덮인 몫 c, 아니면 0), DragHold(붙든 자리와 끌림의 차 H − G — 눌린 칸만,
// 미끄러지는 칸은 0 — 와 끄는 힘 Q — 원천 칸, 뗀 칸은 e^{−RΔt}로 줄어드는 기억 — 를 반정밀도로 묶은 것, SB_DragPackHold. 일지 183의 DragPull을 합쳤다).
//  - 원천마다 자르는 거리는 그 원천이 실제로 가진 힘에서 정한다(눌린 칸은 바로잡기가 키울 수 있는 이번 걸음만큼 넉넉히, 힘 한도로 상한) — 끌지 않는
//    눌린 칸은 거의 제 칸에만 닿는다. 덮인 몫은 그것을 읽는 눌린 칸에서만 눌린 원천을 SB_DRAG_CUT_MIN ℓ까지 센다. 일지 179~183은 눌린 칸 모두가 10 cm
//    넘게 닿아 묶음마다 원천을 다 훑었다(시연 맵 GPU Drag 0.37 → 1.59 ms, 일지 184).
//  - 먼 원천 뭉치기(일지 187): 원천 목록은 거친 8×8 묶음째 이어서 놓이고, 묶음마다 그 원천들을 뭉친 묶음 원천 하나가 있다. 받는 묶음에서 먼 묶음은
//    뭉친 원천 하나로, 가까운 묶음(정지 상자 사이 SB_DRAG_NEAR_TILES 묶음 한 변 안)과 여러 조각이 섞인 묶음은 원천마다 셈한다 — 가름은 정지 기하로만
//    해서 누름이 바뀌어도 뒤집히지 않는다. 뭉친 원천: 끌림 몫 = Σ A Q(가운데는 A|Q| 무게), 덮인 몫 = 눌린 넓이(가운데는 넓이 무게), 커널에
//    (1 + r²/4ℓ²)(K0는 가려진 푸아송의 그린 함수라 원점 밖에서 ∇²K = K/ℓ² — 고르게 퍼진 뭉치의 2차 몫). 시연 앉는 방석(눌린 칸 3천 개)에서 묶음 하나가
//    도는 걸음이 3,084 → 627, 끌림은 참 커널 합과 0.03 mm 안(원천마다 1 µm로 자르던 꼬리가 쌓여 0.5 mm 모자라던 것도 같이 풀렸다).
#define SB_DRAG_SOURCE_ROWS 7u       // 원천 하나의 uint4 줄 수(C++ DragSourceRows)
#define SB_DRAG_BOUND_TOL 1.03       // 랜드마크 하한이 직선의 이 배수를 넘을 때만 하한으로. 판 4(나이트 이웃) 표의 평면 과대가 ≤ 2.75%라 1.03(판 3 여덟 이웃은 ≤ 8.24%라 1.15였다 — 일지 201, 전 자산을 판 4로 다시 구운 뒤 내림)
#define SB_DRAG_GATE_LO 0.3          // 마주봄 가림: 두 칸 노멀이 직선 쪽으로 이만큼 마주 보기 시작하면 줄여
#define SB_DRAG_GATE_HI 0.5          // 이만큼이면 0
#define SB_DRAG_NEW_HALF_CELLS 2.0   // 미끄러져 들어온 앞 가장자리 칸: 이번 움직임이 거친 칸 이 수 안이면 절반을 붙든 자리에 더한다
#define SB_DRAG_HEADER_REACH 7u      // 원천 머리: 자르는 거리 최대(cm × 1000)
#define SB_DRAG_HEADER_LIVE 9u       // 원천 머리: 끄는 힘이나 붙든 자리가 1 µm 이상인 원천 수(0이면 바로잡기·모으기가 커널 합을 건너뛴다 — 끌림 0)
#define SB_DRAG_CUT_MIN 4.0          // 덮인 몫을 세는 거리(ℓ의 배수, ℓ = max(DragReachMax, 칸의 ℓ)): 4ℓ 안에 커널 질량 95%. 끌림 G는 원천마다 제 자르는 거리까지만
#define SB_DRAG_CORRECT_REL 1e-3     // 바로잡을 칸: 지난 차 |H − 지난 G|가 1 µm와 |H|의 이 몫보다 큰 붙은 칸(멈춘 누름은 건너뛴다 — 반정밀도 Q의 0.05% 잔차는 둔다)
#define SB_DRAG_PRESSED_BIT 0x80000000u   // 원천 줄 1의 w: 눌린 칸(덮인 몫에 든다)
#define SB_DRAG_STUCK_BIT 0x40000000u     // 공유 메모리에 올린 원천 줄 1의 w: 붙은 칸(이번 Q가 한도 안)
#define SB_DRAG_ROW_MASK 0x3fffffffu      // 원천 줄 1의 w에서 거리표 줄 번호
#define SB_DRAG_HEADER_CLUSTERS 10u  // 원천 머리: 묶음 원천 수
#define SB_DRAG_CLUSTER_ROWS 9u      // 묶음 원천 하나의 uint4 줄 수(C++ DragClusterRows)
#define SB_DRAG_NEAR_TILES 1.0       // 받는 묶음과 정지 상자 사이가 묶음 한 변(거친 칸 8개)의 이 배수 안인 묶음은 원천마다 셈한다(일지 187 모의: 2는 더 나쁘다 —
                                     // 가까운 원천마다 1 µm 꼬리를 자르는 몫이 늘어서)
#define SB_DRAG_COMPACT 1.5          // 묶음의 살 칸 정지 상자 대각선이 평평한 묶음(7√2 칸)의 이 배수 안일 때만 뭉친다 — 넘으면 아틀라스에 이웃한 다른 조각이
                                     // 섞인 묶음이라(시험 캐릭터 묶음의 12%) 늘 원천마다
#define SB_DRAG_COMPACT_BIT 0x80000000u   // 묶음 원천 줄 0의 w: 뭉쳐도 되는 묶음
#define SB_DRAG_COUNT_MASK 0xffu          // 묶음 원천 줄 0의 w에서 원천 수(≤ 64)
// 프로파일링 집계(일지 189, stat softbody·softbody.Stats): TileCountOut[SB_DRAG_STAT_MIXED] = 여러 조각이 섞여 늘 원천마다 세는 묶음 원천 수(원천 패스),
// 그 뒤로 바로잡기(SB_DRAG_STAT_CORRECT)·모으기(SB_DRAG_STAT_GATHER)마다 SB_DRAG_STAT_WORDS낱말: [0] 커널 합을 돈 묶음 수, [1] 묶음 하나가 차례로 도는 걸음
// 최대(가까운 원천 + 먼 묶음 원천 — 셈하는 칸은 모두 같은 표지를 돌아 묶음 안에서 같다, 패스 시간을 정하는 가장 긴 묶음), [2] 걸음 합, [3] 가까운 묶음을
// 올린 횟수 최대, [4] 가까운 원천 걸음 최대, [5] 먼 묶음 원천 걸음 최대. ActivityArgsCS가 갱신마다 비운다.
#define SB_DRAG_STAT_MIXED 3u
#define SB_DRAG_STAT_CORRECT 4u
#define SB_DRAG_STAT_GATHER 10u
#define SB_DRAG_STAT_WORDS 6u
#define SB_DRAG_STAT_END 16u              // 끌림 걸음 집계가 끝나는 낱말(StatsCS가 [SB_DRAG_STAT_MIXED, 여기) 낱말을 StatOut으로 옮긴다)
// 멈춤 표지(일지 203, ADR-0021 §8): [SB_TILE_COUNT_STILL] = SB_STILL_PRESSED(이번 갱신에 눌린 칸이 있다 — ActivityArgsCS) | SB_STILL_CHANGED(어느 칸의 변위 성분이
// StillChange보다 바뀌었다 — ComposeCS). CPU가 몇 프레임 뒤에 읽어, 눌린 채 입력이 멈추고 바뀐 것이 없는 장의 갱신을 쉰다.
#define SB_TILE_COUNT_STILL 16u
#define SB_STILL_PRESSED 1u
#define SB_STILL_CHANGED 2u
#define SB_TILE_COUNT_WORDS 24u           // TileCountOut 낱말 수(C++ TileCountWords). [17..23]은 검토 R03 낱말(DentTileCount 옆 SB_TILE_COUNT_PARTS 주석)
Texture2D<float4> DragHold;         // 붙든 자리와 끄는 힘(SB_DragPackHold: H − G와 Q, 프레임 cm). 원천 패스가 지난 갱신 값을 읽는다
Texture2D<float4> DragHoldNext;      // 원천 패스가 쓴 이번 갱신 값(갱신 안 임시): xyz H, w = 원천 목록 자리 + 1(0 = 원천 아님). 바로잡기·모으기가 읽고 모으기가
                                     // DragHold로 옮긴다(한 패스에서 같은 텍스처를 읽고 쓰지 않으려고)
RWTexture2D<float4> DragHoldNextOut;
RWTexture2D<float4> DragHoldOut;
RWTexture2D<float4> DragStateOut;
// 원천 목록(원천마다 uint4 SB_DRAG_SOURCE_ROWS줄, 갱신 안 임시 — 거친 8×8 묶음째 이어서 놓인다): [0] xyz = asuint(정지 위치, 메시 로컬), w = asuint(넓이 cm²);
// [1] x = asuint(바로잡기 걸음 w), y = asuint(힘 한도 Q_lim — 눌린 칸만), z = 제 묶음 원천 자리, w = 거리표 줄 번호 | SB_DRAG_PRESSED_BIT(눌린 칸);
// [2] xyz = asuint(바깥 노멀, 프레임), w = asuint(자르는 거리 cm — 이 밖은 몫이 1 µm 아래); [3..6] 그 줄의 가까운 랜드마크 16개: 번호 | (거리, 0.01 단위) << 8.
// 끄는 힘은 바로잡기가 고치므로 따로 둔다(DragSourceQ, 원천마다 xyz Q, w 2 = 붙음·바로잡을 것 / 1 = 붙음·맞음 / 0 = 미끄러짐·기억 — 바로잡기마다 두
// 버퍼를 번갈아 쓴다).
RWStructuredBuffer<uint4> DragSourcesOut;
StructuredBuffer<uint4> DragSources;
RWStructuredBuffer<float4> DragSourceQOut;
StructuredBuffer<float4> DragSourceQ;
// 부위 나누기: 원천마다 그 거친 칸의 두 부위 몫(원천 패스가 쓴다 — 바로잡기·모으기가 원천마다 배율 SB_SeparationMultiplier를 곱한다)과, 거친 칸마다
// 두 부위 몫(살 칸 평균 — 거친 칸 고정 표, CoarseInitCS가 캐리어에서 만든다).
RWStructuredBuffer<float2> DragSourceSeparationOut;
StructuredBuffer<float2> DragSourceSeparation;
Texture2D<float2> CoarseSeparation;
static float2 SB_DragHereSeparation = float2(0, 0);   // 받는 칸 몫(스레드마다 — 합 앞에 정한다)
uint DragSourceCapacity;             // 원천 목록 칸 수(C++ DragSourceCapacity). 넘치면 그 묶음의 원천이 모두 빠진다 — 그만큼 덜 끌린다
// 묶음 원천 목록(원천이 있는 거친 8×8 묶음마다 uint4 SB_DRAG_CLUSTER_ROWS줄, 원천 패스가 쓴다): [0] xyz = asuint(묶음 살 칸 정지 상자 최소, 메시 로컬),
// w = 원천 수 | SB_DRAG_COMPACT_BIT; [1] xyz = 상자 최대, w = 원천 목록 첫 자리; [2] xyz = 눌린 원천의 넓이 무게 가운데, w = 눌린 넓이(cm²); [3] xyz = 원천
// 넓이 무게 평균 노멀(프레임), w = 눌린 넓이의 퍼짐 r²(cm²); [4] xyz = 대표 칸(묶음 살 칸 정지 가운데에 가장 가까운 칸 — 정지 기하로만 정해진다) 정지 위치;
// [5..8] 대표 칸의 가까운 랜드마크 16개. 끄는 힘 몫은 바로잡기가 고치므로 따로(DragClusterQ, 묶음 원천마다 float4 두 줄: [0] xyz = A|Q| 무게 가운데,
// w = 그 퍼짐 r²(cm²); [1] xyz = Σ A Q(cm³), w = 자르는 거리(cm — Σ A × 닿게 할 힘으로 1 µm 규칙) — 원천 패스·바로잡기가 두 버퍼에 번갈아 쓴다).
RWStructuredBuffer<uint4> DragClustersOut;
StructuredBuffer<uint4> DragClusters;
RWStructuredBuffer<float4> DragClusterQOut;
StructuredBuffer<float4> DragClusterQ;
uint DragClusterCapacity;            // 묶음 원천 목록 칸 수(C++: 거친 격자의 8×8 묶음 수 — 넘치지 않는다)
// 원천 머리(uint, 솟음 머리와 같은 자리): [0] 원천 수, [1..3] 정지 위치 최소(SB_OrderedFloat), [4..6] 최대, [7] 자르는 거리 최대(cm × 1000), [8] 넘친 원천 수,
// [9] 산 원천 수(SB_DRAG_HEADER_LIVE), [10] 묶음 원천 수. ActivityArgsCS가 갱신마다 비운다. 바로잡기·모으기 패스는 SRV로 읽는다 — 묶음 원천 수가 장벽이 든
// 루프의 길이라 FXC(D3D11)가 균일로 봐야 한다(UAV 읽기는 비균일로 본다, X3663 — SRV·공유 메모리 값은 균일로 본다, 일지 187에 엔진 d3dcompiler_47로
// 확인). 끌림이 닿은 거친 묶음 수(stat)는 TileCountOut[2]에 센다.
RWBuffer<uint> DragHeaderOut;
Buffer<uint> DragHeader;
RWBuffer<uint> TileCountOut;         // 묶음 수: [0] 함몰 묶음 수, [1] 0으로 비울 묶음 수(ActivityArgsCS가 비우고 TileListCS가 쓴다), [2] 끌림이 닿은 거친 묶음 수(DragGatherCS, stat),
                                     // [3..15] 끌림 걸음 집계(SB_DRAG_STAT_*, 일지 189)
float DragReachMax;                  // 두꺼운 살(두께 상위 10%)의 끌림 폭 ℓ(cm): 원천마다 자르는 거리와 덮인 몫을 세는 거리를 넉넉히 잡는 데 쓴다(0 = 모름 — 제 ℓ로)

// DragHold 한 칸 = 붙든 자리와 끌림의 차 D = H − G와 끄는 힘 Q, 반정밀도 여섯을 32비트 낱말 셋에 둘씩(일지 184 — 갱신을 넘는 텍스처를 늘리지 않으려고
// 일지 183의 DragPull을 합쳤다, 원칙 P07). 텍스처는 RGBA32F 그대로다(렌더 타깃에 정수 포맷이 없다, FTextureRenderTargetResource::IsSupportedFormat).
// 낱말의 위 반쪽은 지수에 1을 더해(SB_DRAG_HALF_BIAS) 모든 낱말이 0이 아닌 보통 부동소수가 되게 한다 — 비정규·NaN 비트 꼴이 없어 어느 GPU에서도
// 비트 그대로 옮겨진다. 0 낱말(비운 텍스처)은 0 둘로 읽는다. 값은 ±SB_DRAG_HALF_LIMIT cm로 자른다(위 반쪽 지수 ≤ 28 + 1).
// 정밀도 2⁻¹¹(0.05%): 3 mm 끌림에 1.5 µm. D는 붙든 자리를 G(32비트, DragState)에서 잰 차라 손을 따라 쌓여도 작게 남는다.
#define SB_DRAG_HALF_BIAS 0x0400u
#define SB_DRAG_HALF_LIMIT 16000.0

float SB_DragPackPair(float Low, float High)
{
	uint L = f32tof16(clamp(Low, -SB_DRAG_HALF_LIMIT, SB_DRAG_HALF_LIMIT)) & 0xffffu;
	uint U = (f32tof16(clamp(High, -SB_DRAG_HALF_LIMIT, SB_DRAG_HALF_LIMIT)) & 0xffffu) + SB_DRAG_HALF_BIAS;
	return asfloat((U << 16u) | L);
}

float2 SB_DragUnpackPair(float Word)
{
	uint Bits = asuint(Word);
	if (Bits == 0u)
	{
		return float2(0, 0);
	}
	return float2(f16tof32(Bits & 0xffffu), f16tof32((Bits >> 16u) - SB_DRAG_HALF_BIAS));
}

float4 SB_DragPackHold(float3 D, float3 Q)
{
	return float4(SB_DragPackPair(D.x, D.y), SB_DragPackPair(D.z, Q.x), SB_DragPackPair(Q.y, Q.z), 0.0);
}

void SB_DragUnpackHold(float4 Packed, out float3 D, out float3 Q)
{
	float2 A = SB_DragUnpackPair(Packed.x);
	float2 B = SB_DragUnpackPair(Packed.y);
	float2 C = SB_DragUnpackPair(Packed.z);
	D = float3(A.x, A.y, B.x);
	Q = float3(B.y, C.x, C.y);
}

// 커널 표(ρ = 거친 칸 크기 / ℓ, 로그 간격 SB_DRAG_TABLE_N점, Tools/drag_kernel_check.py KernelTable이 만든 값):
//  - 제 칸 몫 K_self(ρ) × A/h²: 원천 제 칸이 받는 몫. 평평한 격자에서 Σ_칸 K·A = 1이 되게 맞춘 값이라 넓게 누른 안쪽 끌림이 정확히 붙든 자리다.
//  - 곧은 가장자리 배수 w_rim(ρ): 반평면을 눌렀을 때 가장자리 칸의 끌림이 붙든 자리 H가 되게 하는 원천 배수(바깥 이웃 하나마다 w_rim − 1).
#define SB_DRAG_TABLE_N 40
#define SB_DRAG_TABLE_LOG0 (-3.5065579)  // ln 0.03
#define SB_DRAG_TABLE_STEP 0.19489493    // ln(60/0.03) / 39
static const float SB_DragSelfShare[SB_DRAG_TABLE_N] = {
	0.000707, 0.001002, 0.001420, 0.002007, 0.002831, 0.003985, 0.005595, 0.007834, 0.010936, 0.015216,
	0.021091, 0.029112, 0.039991, 0.054636, 0.074204, 0.100042, 0.133726, 0.176958, 0.231393, 0.298321,
	0.378189, 0.469965, 0.570468, 0.673963, 0.772495, 0.857406, 0.921966, 0.963885, 0.986426, 0.996051,
	0.999161, 0.999879, 0.999989, 0.999999, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000, 1.000000};
static const float SB_DragRimWeight[SB_DRAG_TABLE_N] = {
	34.00204, 28.10179, 23.24574, 19.24917, 15.96003, 13.25319, 11.02562, 9.19257, 7.68424, 6.44324,
	5.42230, 4.58256, 3.89203, 3.32443, 2.85852, 2.47604, 2.16244, 1.90582, 1.69644, 1.52637,
	1.38917, 1.27964, 1.19357, 1.12758, 1.07880, 1.04466, 1.02259, 1.00987, 1.00357, 1.00101,
	1.00021, 1.00003, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000};

/** 표 자리: ρ의 로그 위치(표 밖은 끝 값). */
void SB_DragTableIndex(float Rho, out uint Index, out float Fraction)
{
	float T = clamp((log(max(Rho, 1e-6)) - SB_DRAG_TABLE_LOG0) / SB_DRAG_TABLE_STEP, 0.0, (float)(SB_DRAG_TABLE_N - 1));
	Index = min((uint)T, (uint)(SB_DRAG_TABLE_N - 2));
	Fraction = saturate(T - (float)Index);
}

float SB_DragSelfShareAt(float Rho)
{
	uint Index;
	float Fraction;
	SB_DragTableIndex(Rho, Index, Fraction);
	return lerp(SB_DragSelfShare[Index], SB_DragSelfShare[Index + 1u], Fraction);
}

float SB_DragRimWeightAt(float Rho)
{
	uint Index;
	float Fraction;
	SB_DragTableIndex(Rho, Index, Fraction);
	return lerp(SB_DragRimWeight[Index], SB_DragRimWeight[Index + 1u], Fraction);
}


/** 거친 칸의 이번 갱신 끌림 입력 줄(접촉 패스의 DragInputOut). 표면 칸이 있는 거친 칸만 뜻이 있다(링크가 가리키는 칸은 모두 그렇다). */
float4 SB_DragInputRow(int2 Cell, uint Row)
{
	return DragInput[((uint)Cell.y * DragResolution + (uint)Cell.x) * SB_DRAG_INPUT_ROWS + Row];
}

/** 거친 칸 살 칸의 겉껍질 장력 길이 평균(cm, ADR-0037 §3.4 — 끌림 폭 ℓ_t = √(h²/3 + L_T·h)). 접촉 패스가 셋째 줄 w에 적는다. */
float SB_DragSkinLength(int2 Cell)
{
	return max(SB_DragInputRow(Cell, 2u).w, 0.0);
}

/**
 * 거친 칸이 이번 갱신에 붙들렸는가(1/0): 콜라이더가 눌렀거나 잡기 원 안(ADR-0041) — 원천 패스의 bPressed와 같은 규칙. 원천 패스의 가장자리 배수가 이웃
 * 칸을 이것으로 센다. 일지 348까지는 콜라이더 누름만 세어 잡힌 칸마다 둘레가 비었다고 보고 외톨이 칸의 큰 걸음으로 고쳤다 — 힘 한도가 없는 잡기에서
 * 끄는 힘이 갱신마다 불어 Full이 폭발했다(일지 349).
 */
float SB_DragPressedNow(int2 Cell)
{
	if (SB_DragInputRow(Cell, 0u).w >= 0.0)
	{
		return 1.0;
	}
	float Held = 0.0;
	BRANCH
	if (GrabCount != 0u)
	{
		float3 Pull;
		Held = SB_GrabGripLoose(CoarseInfo.Load(int3(Cell, 0)).xyz, Pull) > 0.0 ? 1.0 : 0.0;
	}
	return Held;
}

/** 거친 칸이 지난 갱신에 눌렸는가(1/0, DragState.w). */
float SB_DragPressedBefore(int2 Cell)
{
	return DragState.Load(int3(Cell, 0)).w > 0.5 ? 1.0 : 0.0;
}

/** 칸 E가 맞댄 눌린 칸 수(E의 네 링크로 — 이음매 건너편은 두 칸 보간, 벽은 0). */
float SB_DragPressedAround(int2 E)
{
	uint4 Links = DragLinks.Load(int3(E, 0));
	float Count = 0.0;
	UNROLL
	for (uint k = 0u; k < 4u; ++k)
	{
		int2 Cell0;
		int2 Cell1;
		float Share;
		SB_DragLinkCells(Links[k], Cell0, Cell1, Share);
		if (!all(Cell0 == E))
		{
			Count += lerp(SB_DragPressedNow(Cell0), SB_DragPressedNow(Cell1), Share);
		}
	}
	return Count;
}

/**
 * 바깥 이웃 E가 눌린 이 칸을 당기는 몫 (1 − f(n))/(1 − f(1)) — E도 눌렸으면 0. n = E가 맞댄 눌린 칸 수. 둘레 값 어림 f(n) = a·n/(1 + 4a − a(4 − n)β_n)은
 * 이산 식(x − ℓ²∇²x = 0)을 바깥 칸 하나에서 닫은 것: 계단 모서리의 바깥 칸은 눌린 칸 둘에 끼어 더 높이 끌리므로 덜 당긴다(모든 바깥 이웃을 같게
 * 보면 볼록 모서리에서 끌림이 H를 40% 넘었다, 일지 177). a = (ℓ/h)², β_1 = (2 + e^{−ρ})/3(옆 둘은 제 값, 하나는 한 칸 더 밖), β_2·β_3 = e^{−ρ}.
 */
float SB_DragOutsidePull(int2 E, float A, float Decay, float Beta1, float F1)
{
	if (SB_DragPressedNow(E) > 0.0)
	{
		return 0.0;
	}
	float N = clamp(SB_DragPressedAround(E), 1.0, 4.0);
	float Beta = N <= 1.0 ? Beta1 : (N < 2.0 ? lerp(Beta1, Decay, N - 1.0) : Decay);
	float F = A * N / (1.0 + 4.0 * A - A * (4.0 - N) * Beta);
	return (1.0 - F) / max(1.0 - F1, 1e-6);
}

/** 눌린 칸의 가장자리 배수 w_notch = 1 + (w_rim(ρ) − 1) × Σ_바깥 이웃 당기는 몫(이음매 건너편은 두 칸 보간). 안쪽 칸(바깥 이웃 없음)은 1. */
float SB_DragRimWeightOf(int2 Cell, uint4 Links, float Ell)
{
	float Rho = DragTexelSize / Ell;
	float A = 1.0 / (Rho * Rho);
	float Decay = exp(-Rho);
	float Beta1 = (2.0 + Decay) / 3.0;
	float F1 = A / (1.0 + 4.0 * A - 3.0 * A * Beta1);
	float Excess = 0.0;
	UNROLL
	for (uint k = 0u; k < 4u; ++k)
	{
		int2 Cell0;
		int2 Cell1;
		float Share;
		SB_DragLinkCells(Links[k], Cell0, Cell1, Share);
		if (all(Cell0 == Cell))
		{
			continue;
		}
		Excess += (1.0 - Share) * SB_DragOutsidePull(Cell0, A, Decay, Beta1, F1);
		if (Share > 0.0)
		{
			Excess += Share * SB_DragOutsidePull(Cell1, A, Decay, Beta1, F1);
		}
	}
	return 1.0 + (SB_DragRimWeightAt(Rho) - 1.0) * Excess;
}

// 묶음 원천 만들기(원천 패스)와 다시 모으기(바로잡기): 묶음 칸마다 정지 위치·넓이·끄는 힘·노멀을 공유 메모리에 두고 정해진 차례로 더한다(일지 187).
groupshared float4 SB_DragTileRest[SB_COARSE_CELLS];     // xyz 정지 위치 − 기준점(묶음 살 칸 정지 상자 최소, 메시 로컬), w 넓이(cm², 0 = 살 없는 칸)
groupshared float4 SB_DragTilePull[SB_COARSE_CELLS];     // xyz 끄는 힘 Q(프레임 cm), w 2 = 눌린 원천, 1 = 그 밖의 원천(기억), 0 = 원천 아님
groupshared float4 SB_DragTileNormal[SB_COARSE_CELLS];   // xyz 바깥 노멀(프레임), w 넓이 × 닿게 할 힘(cm³ — 묶음 원천의 자르는 거리)
groupshared uint SB_DragTileSources;   // 묶음의 원천 수
groupshared uint SB_DragTileStart;     // 원천 목록에서 묶음의 첫 자리
groupshared uint SB_DragTileSlot;      // 묶음 원천 자리 + 1(0 = 없음 — 원천이 없거나 목록이 넘쳤다)
groupshared uint SB_DragTileEll;       // 원천의 ℓ(두꺼운 살 쪽으로 넉넉히, cm) 최대(asuint)
groupshared uint SB_DragTileGroup;     // 부위 나누기: 부위 몫이 있는 원천이 있다(그 묶음 원천은 뭉치지 않고 원천마다 센다)

/**
 * 묶음 원천의 끄는 힘 몫(SB_DragTile*의 원천만): A|Q| 무게 가운데(기준점에서, 메시 로컬)·그 퍼짐 r²(cm²)·Σ A Q(cm³). 칸 번호 차례로 더한다 — 같은 입력이면
 * 비트까지 같다. 원천 패스와 바로잡기가 같은 함수를 쓴다.
 */
void SB_DragClusterPull(out float3 Centre, out float Spread, out float3 SumAQ)
{
	float SumW = 0.0;
	float SumWSq = 0.0;
	float3 SumWX = float3(0, 0, 0);
	SumAQ = float3(0, 0, 0);
	for (uint k = 0u; k < SB_COARSE_CELLS; ++k)
	{
		float4 Rest = SB_DragTileRest[k];
		float4 Pull = SB_DragTilePull[k];
		if (Pull.w > 0.5)
		{
			float Weight = Rest.w * length(Pull.xyz);
			SumW += Weight;
			SumWX += Weight * Rest.xyz;
			SumWSq += Weight * dot(Rest.xyz, Rest.xyz);
			SumAQ += Rest.w * Pull.xyz;
		}
	}
	Centre = SumWX / max(SumW, 1e-30);
	Spread = SumW > 0.0 ? max(SumWSq / SumW - dot(Centre, Centre), 0.0) * ThicknessScale * ThicknessScale : 0.0;
}

/**
 * 묶음 원천 하나를 쓴다(원천 패스, 스레드 둘 — 칸 번호 차례로 더한다).
 *  - 0번: 묶음 살 칸 정지 상자(가까움 가름), 뭉쳐도 되는가(상자 대각선 ≤ SB_DRAG_COMPACT × 평평한 묶음 7√2 칸 — 넘으면 아틀라스에 이웃한 다른 조각이
 *    섞였다), 대표 칸(살 칸 넓이 무게 가운데에 가장 가까운 칸 — 접힘 가름의 랜드마크 하한을 잴 칸)의 자리와 가까운 랜드마크 16개. 모두 정지 기하로만
 *    정해진다(누름이 바뀌어도 그대로).
 *  - 1번: 끌림 몫(SB_DragClusterPull — DragClusterQ 첫 버퍼, 바로잡기가 고친 Q로 둘째 버퍼에 다시 모은다), 자르는 거리(Σ A × 닿게 할 힘으로 원천과 같은
 *    1 µm 규칙), 덮인 몫(눌린 넓이·그 가운데·퍼짐), 원천 넓이 무게 평균 노멀(가림·회전).
 */
void SB_DragBuildCluster(uint2 Group, uint Lane, uint Cluster, float3 Reference)
{
	uint Base = Cluster * SB_DRAG_CLUSTER_ROWS;
	if (Lane == 0u)
	{
		float3 Hi = float3(SB_OrderedToFloat(SB_CoarseBoxHi[0]), SB_OrderedToFloat(SB_CoarseBoxHi[1]), SB_OrderedToFloat(SB_CoarseBoxHi[2]));
		bool bCompact = length(Hi - Reference) * ThicknessScale <= SB_DRAG_COMPACT * 7.0 * sqrt(2.0) * DragTexelSize;
		// 부위 나누기: 부위 몫이 있는 원천의 묶음은 뭉치지 않는다 — 뭉친 원천 하나에는 받는 칸마다 원천별로 다른 배율을 곱할 수 없어서, 가까운 묶음처럼
		// 원천마다 센다(셈은 원천 수만큼 늘지만 근사가 아니다). 부위 몫이 없는 묶음은 그대로 뭉친다.
		bCompact = bCompact && !(bSeparation != 0u && SB_DragTileGroup != 0u);
		if (!bCompact)
		{
			InterlockedAdd(TileCountOut[SB_DRAG_STAT_MIXED], 1u);   // stat: 섞인 묶음 원천 수
		}
		float SumA = 0.0;
		float3 SumAX = float3(0, 0, 0);
		for (uint k = 0u; k < SB_COARSE_CELLS; ++k)
		{
			float4 Rest = SB_DragTileRest[k];
			SumA += Rest.w;
			SumAX += Rest.w * Rest.xyz;
		}
		float3 Centre = SumAX / max(SumA, 1e-30);
		uint Rep = 0u;
		float Best = 1e30;
		for (uint k2 = 0u; k2 < SB_COARSE_CELLS; ++k2)
		{
			float4 Rest = SB_DragTileRest[k2];
			float3 Away = Rest.xyz - Centre;
			float D = dot(Away, Away);
			if (Rest.w > 0.0 && D < Best)
			{
				Best = D;
				Rep = k2;
			}
		}
		uint RepRow = CoarseRow.Load(int3(Group * SB_COARSE_GROUP + uint2(Rep % SB_COARSE_GROUP, Rep / SB_COARSE_GROUP), 0));
		uint Nearest[SB_LANDMARK_NEAREST];
		UNROLL
		for (uint n = 0u; n < SB_LANDMARK_NEAREST; ++n)
		{
			Nearest[n] = SB_LandmarkNearest(RepRow, n);
		}
		UNROLL
		for (uint n2 = 0u; n2 < SB_LANDMARK_NEAREST; ++n2)
		{
			Nearest[n2] = (SB_LandmarkValue(RepRow, Nearest[n2]) << 8u) | Nearest[n2];
		}
		DragClustersOut[Base] = uint4(asuint(Reference), SB_DragTileSources | (bCompact ? SB_DRAG_COMPACT_BIT : 0u));
		DragClustersOut[Base + 1u] = uint4(asuint(Hi), SB_DragTileStart);
		DragClustersOut[Base + 4u] = uint4(asuint(SB_DragTileRest[Rep].xyz + Reference), 0u);
		UNROLL
		for (uint R4 = 0u; R4 < 4u; ++R4)
		{
			DragClustersOut[Base + 5u + R4] = uint4(Nearest[R4 * 4u], Nearest[R4 * 4u + 1u], Nearest[R4 * 4u + 2u], Nearest[R4 * 4u + 3u]);
		}
	}
	else
	{
		float3 CentreG;
		float SpreadG;
		float3 SumAQ;
		SB_DragClusterPull(CentreG, SpreadG, SumAQ);
		float SumP = 0.0;
		float SumPSq = 0.0;
		float3 SumPX = float3(0, 0, 0);
		float3 SumN = float3(0, 0, 0);
		float SumReach = 0.0;
		for (uint k = 0u; k < SB_COARSE_CELLS; ++k)
		{
			float4 Rest = SB_DragTileRest[k];
			float4 Pull = SB_DragTilePull[k];
			float4 Facing = SB_DragTileNormal[k];
			if (Pull.w > 0.5)
			{
				SumN += Rest.w * Facing.xyz;
				SumReach += Facing.w;
				if (Pull.w > 1.5)
				{
					SumP += Rest.w;
					SumPX += Rest.w * Rest.xyz;
					SumPSq += Rest.w * dot(Rest.xyz, Rest.xyz);
				}
			}
		}
		float3 CentreP = SumPX / max(SumP, 1e-30);
		float SpreadP = SumP > 0.0 ? max(SumPSq / SumP - dot(CentreP, CentreP), 0.0) * ThicknessScale * ThicknessScale : 0.0;
		float EllMax = asfloat(SB_DragTileEll);
		float Cut = SumReach > 0.0 ? EllMax * SB_BesselK0Inverse(SB_REST_EPSILON * 2.0 * PI * EllMax * EllMax / SumReach) : 0.0;
		DragClustersOut[Base + 2u] = uint4(asuint(CentreP + Reference), asuint(SumP));
		DragClustersOut[Base + 3u] = uint4(asuint(SumN / max(length(SumN), 1e-12)), asuint(SpreadP));
		DragClusterQOut[Cluster * 2u] = float4(CentreG + Reference, SpreadG);
		DragClusterQOut[Cluster * 2u + 1u] = float4(SumAQ, Cut);
	}
}

/**
 * 원천 모으기(거친 칸 전체, 살 있는 칸만): 붙든 자리 H와 끄는 힘 Q를 이어 받아 고치고, 원천(눌린 칸과 기억이 남은 칸)을 원천 목록에 넣는다.
 *  - 새로 누른 칸: H = 지금 그 자리의 끌림(지난 갱신의 G). 미끄러져 들어온 앞 가장자리(지난 갱신에 눌린 이웃이 있다)면 이번 움직임의 절반을 더한다 —
 *    닿은 지 평균 반 갱신이 지났다고 본다(없으면 20갱신/초의 빠른 손가락이 0.2 mm 덜 끌린다, 일지 177). 내려앉는 손(이웃 아무도 안 눌림)은 들어온
 *    움직임에 끌리지 않는다.
 *  - 계속 누른 칸: H += 누른 칸들이 콜라이더를 따라 움직인 거리 평균(접촉 패스). 접평면에 눕힌다.
 *  - 끄는 힘: Q = 지난 Q + w (H − 지난 G) — 지난 갱신이 남긴 차(G ≠ H)와 이번 움직임을 한 걸음에 고치는 시도. 버틸 수 있는 힘 Q_lim = w R를 넘으면 한도
 *    길이로 잘린다(미끄러짐). R: φ(R) + (w − 1)R = 마찰 한도(뉴턴 넷 — 선형 법칙이면 Q_lim이 한도 그대로, 가파른 법칙(얇은 살을 깊이 누름)이면
 *    R ≈ φ⁻¹(한도)에서 거의 안 줄어 가장자리도 안쪽만큼 끌린다). 새로 누른 칸은 H = 지난 G라 Q가 그대로다 — 칸이 집합에 들어와도 G가 뛰지 않는다.
 *    미끄러진 원천을 "한도 − (φ(G) − G)"처럼 지난 G로 두는 꼴은 가파른 법칙에서 발산했다(일지 177, ADR-0033 §7) — 한도는 R에서만 온다.
 *  - 배수 w = max(1/c, w_notch): c = 눌린 칸들이 이 칸에 차지하는 커널 질량(지난 모으기가 셈 — 작은 누름 안쪽은 1/c가 크고 넓은 누름 안쪽은 1),
 *    w_notch = 가장자리 배수(눌린 칸 집합의 경계). 새로 누른 칸은 c를 모르니 1/w_notch로 둔다. w는 바로잡기의 걸음 크기이면서 힘 한도 Q_lim = w·R에도 든다 —
 *    살의 법칙이 선형인 작은 누름에서는 한도가 w와 무관하고(w·R = 한도), 가파른 깊은 누름에서는 w에 비례한다(ADR-0033 §8). 어림이 틀리면 걸음 수가 늘고 깊은 누름의 버티는 힘도 그만큼 틀린다.
 *  - 뗀 칸: Q가 e^{−RΔt}배(HoldDecay — 함몰의 누른 자리 기억과 같은 시간 모델), 1 µm 아래 0. 붙든 자리는 없다.
 *  - 눌린 칸은 Q가 0이어도(수직으로만 누름) 원천에 든다 — 이웃의 c에 들고 바로잡기가 Q를 키울 수 있어야 하기 때문. 끄는 힘이나 붙든 자리가 1 µm
 *    이상인 원천 수를 따로 세어, 하나도 없으면 바로잡기·모으기가 커널 합을 건너뛴다(수직으로만 누른 넓은 누름은 거의 공짜).
 *  - 바로잡을 칸: 붙은 칸 가운데 지난 차 |H − 지난 G|가 1 µm와 |H|의 SB_DRAG_CORRECT_REL보다 큰 칸(DragSourceQ.w 2). 멈춘 누름은 바로잡기를 건너뛴다.
 * 원천마다 자르는 거리 = 커널 몫 K·A·|Q|가 1 µm 아래가 되는 거리(두꺼운 ℓ로 넉넉히). 눌린 칸은 바로잡기가 Q를 키울 수 있는 만큼(이번 걸음 w|H − 지난 G|)
 * 더 보되 힘 한도 Q_lim을 넘지 않는다 — 끌지 않는 눌린 칸은 거의 제 칸에만 닿는다(덮인 몫은 모으기가 따로 센다, 일지 184). 6 mm 원천이면 10 cm 안팎.
 * 묶음(스레드 64개 = 거친 8×8 칸 = 바로잡기·모으기의 묶음)이 제 원천들을 원천 목록에 이어서 넣고(자리 잡기 한 번), 그 원천들을 뭉친 묶음 원천을 묶음
 * 원천 목록에 넣는다(일지 187, SB_DragBuildCluster). 원천 목록이 넘치면 그 묶음의 원천이 모두 빠진다(stat 넘침 수 — 그만큼 덜 끌린다).
 */
[numthreads(SB_COARSE_GROUP, SB_COARSE_GROUP, 1)]
void DragSourceCS(uint3 Id : SV_DispatchThreadID, uint3 GroupId : SV_GroupID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex == 0u)
	{
		SB_DragTileSources = 0u;
		SB_DragTileSlot = 0u;
		SB_DragTileEll = 0u;
		SB_DragTileGroup = 0u;
	}
	if (LocalIndex < 3u)
	{
		SB_CoarseBoxLo[LocalIndex] = 0xffffffffu;
		SB_CoarseBoxHi[LocalIndex] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	int2 Cell = int2(Id.xy);
	bool bInGrid = Id.x < DragResolution && Id.y < DragResolution;
	// 살 없는 칸은 원천이 되지 않고 끌림도 받지 않는다(상태는 0 그대로). 장벽이 있어 돌아가지 않고 표지로 가른다.
	uint Row = bInGrid ? CoarseRow.Load(int3(Cell, 0)) : SB_LANDMARK_NO_ROW;
	float4 Info = float4(0, 0, 0, 0);
	float3 Outward = float3(0, 0, 1);
	float Ell = 1.0;
	float3 Q = float3(0, 0, 0);
	float3 H = float3(0, 0, 0);
	float W = 1.0;
	float QLimit = 0.0;
	float Lag = 0.0;      // 지난 차 |H − 지난 G|(이번 걸음 = w × 이것)
	float QLength = 0.0;
	float QReach = 0.0;   // 닿게 할 힘(자르는 거리를 정한다)
	float EllMax = 0.0;
	bool bPressed = false;
	bool bStuck = false;
	bool bCorrect = false;
	bool bSource = false;
	uint Local = 0u;      // 묶음 안 원천 차례
	if (Row != SB_LANDMARK_NO_ROW)
	{
		uint4 Links = DragLinks.Load(int3(Cell, 0));
		float4 Input0 = SB_DragInputRow(Cell, 0u);
		float4 Input1 = SB_DragInputRow(Cell, 1u);
		Outward = Input1.xyz;
		float Layer = max(Input1.w, 1e-3);
		Ell = max(SB_ShearLength(Layer, SB_DragSkinLength(Cell)), 1e-3);
		float LimitPhi = Input0.w;
		bPressed = LimitPhi >= 0.0 && Links.x != SB_DRAG_EMPTY;
		// 잡기(ADR-0041, Full): 원 안 거친 칸은 붙든 칸 — 붙든 자리 H = 당긴 벡터의 접선 몫 × 손끝 몫 κ/(1 + κ)(갱신마다 새로), 버틸 힘 한도 없음(Drag
		// Friction은 잡기에 듣지 않는다 — 얼마나 늘어날지는 잡기 한도가 맡는다). 끄는 힘 Q·바로잡기·모으기는 그대로라 둘레 피부가 같은 살층 폭으로 딸려 온다.
		// 이웃의 가장자리 배수(SB_DragPressedNow)와 모으기(원천 줄의 눌림 비트)도 잡힌 칸을 눌린 칸으로 센다 — 미끄러지지 않는 누름과 같은 걸음(일지 349).
		float GrabHold = 0.0;
		float3 GrabPull = float3(0, 0, 0);
		BRANCH
		if (GrabCount != 0u && Links.x != SB_DRAG_EMPTY)
		{
			float Kappa = SB_GrabGripLoose(CoarseInfo.Load(int3(Cell, 0)).xyz, GrabPull);
			GrabHold = Kappa / (1.0 + Kappa);
			bPressed = bPressed || GrabHold > 0.0;
		}
		float4 Last = DragState.Load(int3(Cell, 0));
		bool bWasPressed = Last.w > 0.5;
		float CoverLast = max(Last.w - 1.0, 0.0);
		// 지난 갱신이 남긴 붙든 자리와 끌림의 차(H − G)와 끄는 힘 Q. 끄는 힘: 계속 누른 칸은 지난 Q에서 잇고, 그 밖(뗀 칸, 기억이 남은 채 다시 누른 칸)은
		// e^{−RΔt}배. 뗀 칸은 1 µm 아래 0.
		float3 HoldLead;
		SB_DragUnpackHold(DragHold.Load(int3(Cell, 0)), HoldLead, Q);
		Q *= bPressed && bWasPressed ? 1.0 : HoldDecay;
		if (!bPressed && dot(Q, Q) < SB_REST_EPSILON * SB_REST_EPSILON)
		{
			Q = float3(0, 0, 0);
		}
		if (bPressed)
		{
			if (!bWasPressed)
			{
				H = Last.xyz;
				float Front = 0.0;
				UNROLL
				for (uint k = 0u; k < 4u; ++k)
				{
					int2 Cell0;
					int2 Cell1;
					float Share;
					SB_DragLinkCells(Links[k], Cell0, Cell1, Share);
					if (!all(Cell0 == Cell))
					{
						Front += lerp(SB_DragPressedBefore(Cell0), SB_DragPressedBefore(Cell1), Share);
					}
				}
				float3 Moved = Input0.xyz;
				float Near = SB_DRAG_NEW_HALF_CELLS * DragTexelSize;
				if (Front > 0.0 && dot(Moved, Moved) <= Near * Near)
				{
					H += 0.5 * Moved;
				}
			}
			else
			{
				H = Last.xyz + HoldLead + Input0.xyz;
			}
			H -= Outward * dot(H, Outward);
			if (GrabHold > 0.0)
			{
				H = GrabHold * (GrabPull - Outward * dot(GrabPull, Outward));
			}
			float WNotch = SB_DragRimWeightOf(Cell, Links, Ell);
			float Cover = bWasPressed && CoverLast > 1e-6 ? CoverLast : 1.0 / WNotch;
			W = max(1.0 / max(Cover, 1e-6), WNotch);
			// 버틸 수 있는 자리 R(뉴턴 넷, 선형 추정 한도/w에서 내려온다 — 왼쪽이 볼록해 넘치지 않는다)과 힘 한도 Q_lim = w R = 한도 − (φ(R) − R): 선형 법칙이면 w와 무관하게 한도 그대로, 가파른 법칙이면 w에 비례 — w 어림 오차가 한도에 드는 길(ADR-0033 §8).
			float R = LimitPhi / W;
			UNROLL
			for (int Step = 0; Step < 4; ++Step)
			{
				float Phi;
				float Slope;
				float Energy;
				SB_TissueLaw(R, Layer, LumpStiffen, Phi, Slope, Energy);
				R = max(R - (Phi + (W - 1.0) * R - LimitPhi) / (Slope + W - 1.0), 0.0);
			}
			QLimit = GrabHold > 0.0 ? 1e30 : W * R;
			Lag = length(H - Last.xyz);
			Q += W * (H - Last.xyz);
			Q -= Outward * dot(Q, Outward);
			float Trial = length(Q);
			bStuck = Trial <= QLimit;
			if (!bStuck)
			{
				Q *= QLimit / max(Trial, 1e-12);
			}
			bCorrect = bStuck && Lag > max(SB_REST_EPSILON, SB_DRAG_CORRECT_REL * length(H));
		}
		QLength = length(Q);
		// 원천 = 눌린 칸과 기억이 남은 칸. 눌린 칸은 Q가 0이어도 넣는다(이웃의 덮인 몫 c, 바로잡기).
		bSource = bPressed || QLength > 0.0;
		Info = CoarseInfo.Load(int3(Cell, 0));
		if (Info.w > 0.0)
		{
			UNROLL
			for (uint Axis = 0u; Axis < 3u; ++Axis)
			{
				InterlockedMin(SB_CoarseBoxLo[Axis], SB_OrderedFloat(Info[Axis]));
				InterlockedMax(SB_CoarseBoxHi[Axis], SB_OrderedFloat(Info[Axis]));
			}
		}
		if (bSource)
		{
			EllMax = max(DragReachMax, Ell);
			QReach = bPressed ? min(QLength + W * Lag, max(QLimit, QLength)) : QLength;
			InterlockedAdd(SB_DragTileSources, 1u, Local);
			InterlockedMax(SB_DragTileEll, asuint(EllMax));   // 양수 부동소수는 비트 순서가 크기 순서
			if (bSeparation != 0u && dot(CoarseSeparation.Load(int3(Cell, 0)), float2(1, 1)) > 0.01)
			{
				InterlockedOr(SB_DragTileGroup, 1u);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	// 원천 목록에 묶음째 자리를 잡는다 — 묶음 원천이 제 원천들을 첫 자리·수로 가리킨다(바로잡기·모으기가 가까운 묶음의 원천을 한 번에 올린다).
	if (LocalIndex == 0u && SB_DragTileSources > 0u)
	{
		uint Start;
		InterlockedAdd(DragHeaderOut[SB_SOURCE_HEADER_COUNT], SB_DragTileSources, Start);
		if (Start + SB_DragTileSources <= DragSourceCapacity)
		{
			uint Cluster;
			InterlockedAdd(DragHeaderOut[SB_DRAG_HEADER_CLUSTERS], 1u, Cluster);
			SB_DragTileStart = Start;
			SB_DragTileSlot = Cluster < DragClusterCapacity ? Cluster + 1u : 0u;
		}
		else
		{
			InterlockedAdd(DragHeaderOut[SB_SOURCE_HEADER_OVERFLOW], SB_DragTileSources);
		}
	}
	// 뭉치기 입력(칸 번호 자리에). 정지 위치는 묶음 살 칸 상자의 최소에서 잰다 — 메시 로컬 좌표 수백에서 퍼짐 몇 cm를 float로 빼면 자릿수가 모자란다.
	float3 Reference = float3(SB_OrderedToFloat(SB_CoarseBoxLo[0]), SB_OrderedToFloat(SB_CoarseBoxLo[1]), SB_OrderedToFloat(SB_CoarseBoxLo[2]));
	float Area = Info.w * ThicknessScale * ThicknessScale;
	SB_DragTileRest[LocalIndex] = Info.w > 0.0 ? float4(Info.xyz - Reference, Area) : float4(0, 0, 0, 0);
	SB_DragTilePull[LocalIndex] = float4(Q, bSource ? (bPressed ? 2.0 : 1.0) : 0.0);
	SB_DragTileNormal[LocalIndex] = float4(Outward, bSource ? Area * QReach : 0.0);
	GroupMemoryBarrierWithGroupSync();
	uint TileSlot = SB_DragTileSlot;
	if (Row != SB_LANDMARK_NO_ROW)
	{
		if (!bSource)
		{
			DragHoldNextOut[Cell] = float4(0, 0, 0, 0);
		}
		else if (TileSlot == 0u)
		{
			DragHoldNextOut[Cell] = float4(H, 0.0);   // 목록이 넘쳤다: 원천이 아니다
		}
		else
		{
			// 정지 위치·넓이(솟음 거친 칸 표), 걸음·힘 한도·제 묶음 원천 자리, 노멀, 자르는 거리, 가까운 랜드마크.
			uint Slot = SB_DragTileStart + Local;
			float Cut = EllMax * SB_BesselK0Inverse(SB_REST_EPSILON * 2.0 * PI * EllMax * EllMax / max(Area * QReach, 1e-30));
			if (QLength >= SB_REST_EPSILON || (bPressed && dot(H, H) >= SB_REST_EPSILON * SB_REST_EPSILON))
			{
				InterlockedAdd(DragHeaderOut[SB_DRAG_HEADER_LIVE], 1u);
			}
			DragHoldNextOut[Cell] = float4(H, (float)(Slot + 1u));
			DragSourceQOut[Slot] = float4(Q, bCorrect ? 2.0 : (bStuck ? 1.0 : 0.0));
			// 가까운 랜드마크 16개(거리 << 8 | 번호): 굽기가 줄마다 적어 둔 번호를 읽고 거리는 같은 줄에서 읽는다(SpreadRestrictCS와 같다).
			uint Nearest[SB_LANDMARK_NEAREST];
			UNROLL
			for (uint k = 0u; k < SB_LANDMARK_NEAREST; ++k)
			{
				Nearest[k] = SB_LandmarkNearest(Row, k);
			}
			UNROLL
			for (uint k2 = 0u; k2 < SB_LANDMARK_NEAREST; ++k2)
			{
				Nearest[k2] = (SB_LandmarkValue(Row, Nearest[k2]) << 8u) | Nearest[k2];
			}
			uint Base = Slot * SB_DRAG_SOURCE_ROWS;
			DragSourcesOut[Base] = uint4(asuint(Info.x), asuint(Info.y), asuint(Info.z), asuint(Area));
			if (bSeparation != 0u)
			{
				DragSourceSeparationOut[Slot] = CoarseSeparation.Load(int3(Cell, 0));
			}
			DragSourcesOut[Base + 1u] = uint4(asuint(W), asuint(QLimit), TileSlot - 1u, Row | (bPressed ? SB_DRAG_PRESSED_BIT : 0u));
			DragSourcesOut[Base + 2u] = uint4(asuint(Outward.x), asuint(Outward.y), asuint(Outward.z), asuint(Cut));
			UNROLL
			for (uint R4 = 0u; R4 < 4u; ++R4)
			{
				DragSourcesOut[Base + 3u + R4] = uint4(Nearest[R4 * 4u], Nearest[R4 * 4u + 1u], Nearest[R4 * 4u + 2u], Nearest[R4 * 4u + 3u]);
			}
			UNROLL
			for (uint Axis2 = 0u; Axis2 < 3u; ++Axis2)
			{
				InterlockedMin(DragHeaderOut[SB_SOURCE_HEADER_MIN + Axis2], SB_OrderedFloat(Info[Axis2]));
				InterlockedMax(DragHeaderOut[SB_SOURCE_HEADER_MAX + Axis2], SB_OrderedFloat(Info[Axis2]));
			}
			InterlockedMax(DragHeaderOut[SB_DRAG_HEADER_REACH], (uint)(Cut * 1000.0));
		}
	}
	if (LocalIndex < 2u && TileSlot != 0u)
	{
		SB_DragBuildCluster(GroupId.xy, LocalIndex, TileSlot - 1u, Reference);
	}
}

// 바로잡기·모으기(DragCorrectCS, DragGatherCS): 거친 칸 8×8 묶음(스레드 64 × 조각)이 제 칸들의 거리표 줄을 공유 메모리에 올리고(솟음과 같은 SB_CoarseRows
// 배치) 묶음 원천 목록을 SB_DRAG_CLUSTER_CHUNK개씩 올린다(일지 187). 묶음 원천마다 — 받는 묶음과의 가름은 둘의 정지 상자로만 해서 누름이 바뀌어도
// 뒤집히지 않는다:
//  - 가까운 묶음(정지 상자 사이가 묶음 한 변의 SB_DRAG_NEAR_TILES배 안)과 여러 조각이 섞인 묶음: 그 묶음의 원천들(목록에 이어서 놓였다, ≤ 64개)을 한 번에
//    올려 원천마다 셈한다(SB_DragSourceTerm — 일지 178~185와 같은 규칙).
//  - 먼 묶음: 뭉친 원천 하나로 셈한다(SB_DragClusterTerm).
// 장벽은 균일한 흐름에만(D3D11 X3663). 루프 길이는 SRV(묶음 원천 수)와 공유 메모리 값(셈할 칸이 있는가, 가까운 묶음 수)에서만 온다 — FXC는 둘 다
// 균일로 보고 UAV 읽기만 비균일로 본다(일지 187에 엔진 d3dcompiler_47로 확인. 일지 185의 "공유 메모리 값이면 거부"는 시험하지 않은 짐작이었다).
// 올림 표지(비트·가까운 묶음 목록)는 홀짝 두 벌을 번갈아 쓴다: 다음 올림의 표지를 이번 올림 안(장벽 둘 사이)에서 비운다. 장벽 앞에서 비우면 셈할 칸이 없어
// 먼저 끝낸 워프가 다음 표지를 비우는 동안 늦은 워프가 아직 이번 표지(둘째 낱말)를 읽지 않았을 수 있다(일지 186까지의 꼴 — 원천 32개를 빠뜨릴 수 있었다).
#define SB_DRAG_CLUSTER_CHUNK 32u   // 한 번에 공유 메모리에 올리는 묶음 원천 수
groupshared uint4 SB_DragSource[SB_COARSE_CHUNK * SB_DRAG_SOURCE_ROWS];   // 올린 원천 64개(7 KB). 줄 1은 xyz = asuint(이번 Q), w = 줄 번호 | 눌림·붙음 비트
groupshared uint SB_DragNearMask[2][2];   // [홀짝][낱말] 올린 원천 가운데 셈할 칸 상자에서 닿는 것
// 올린 먼 묶음 원천(4.5 KB): [0] = DragClusterQ 줄 0(A|Q| 무게 가운데, 퍼짐), [1] = 줄 1(Σ A Q, 자르는 거리), [2..8] = DragClusters 줄 2..8
groupshared uint4 SB_DragCluster[SB_DRAG_CLUSTER_CHUNK * SB_DRAG_CLUSTER_ROWS];
groupshared uint SB_DragFarMask[2];       // [홀짝] 올린 묶음 원천 가운데 먼 것으로 셈할 것
groupshared uint SB_DragNearTiles[2];     // [홀짝] 올린 묶음 원천 가운데 가까운 것의 수
groupshared uint2 SB_DragNearRange[2][SB_DRAG_CLUSTER_CHUNK];   // [홀짝][차례] 가까운 묶음의 원천 목록 (첫 자리, 수)
groupshared uint SB_DragGroupLo[3];       // 이 묶음 살 칸 정지 위치 상자(순서 보존 정수) — 가까움 가름
groupshared uint SB_DragGroupHi[3];
groupshared uint SB_DragTouched;          // 1 = 묶음의 어느 칸이 0이 아닌 몫을 받았다(stat, TileCountOut[2])
groupshared uint SB_DragCoverReach;       // 묶음의 셈할 눌린 칸이 덮인 몫을 세는 거리 최대(asuint cm, 0 = 안 센다)

// 걸음 나누기(일지 191): 바로잡기·모으기의 묶음 하나(거친 칸 8×8)를 조각 SB_DRAG_SLICES개로 돌린다(스레드 64 × 조각, 셰이더 순열 1·2·4 — C++
// FSoftbodyDragSlicesDim, 프로파일링 스위치 softbody.Drag.Slices). 조각마다 같은 칸 64개를 맡아 올린 원천(가까운 원천·먼 묶음 원천)을 비트 자리로
// 번갈아 나눠(낱말 안 자리 % 조각 수) 돌고, 끝에 조각 0이 다른 조각의 부분합을 조각 차례로 더한다(SB_DragSum). 걸음 하나 0.7 µs는 가장 긴 묶음의 워프
// 둘이 의존 사슬을 홀로 도는 기다림이라(일지 190) 차례 걷기가 1/조각 수가 되고, SM 하나의 워프도 조각 수배라 기다림을 서로 가린다.
//  - 공유 메모리 표지·상자·원천 올림·집계·칸의 전역 쓰기는 조각 0만 한다(올림 한 번은 전역 읽기 왕복 둘이라 스레드를 늘려도 짧아지지 않는다).
//    거리표 줄 올리기는 조각끼리 랜드마크 쌍을 나눈다. 조각마다 제 칸의 입력(정지 위치·노멀·ℓ·눌림)은 따로 읽는다.
//  - 공유 메모리는 늘리지 않는다(바로잡기가 SM5 한도 32 KB 가운데 약 31 KB) — 부분합은 걷기가 끝난 원천 올림 자리(SB_DragSource)에 쓴다.
//  - 합치기는 원자 없이 정해진 차례라 결정적이다(일지 170: 같은 주소 원자 더하기는 워프마다 32번 차례로 기다렸다). 1이면 지금까지와 비트까지 같다.
#ifndef SB_DRAG_SLICES
#define SB_DRAG_SLICES 1
#endif
#if SB_DRAG_SLICES != 1 && SB_DRAG_SLICES != 2 && SB_DRAG_SLICES != 4
#error SB_DRAG_SLICES must be 1, 2 or 4 (slice bit masks, landmark pair split and partial-sum rows assume it)
#endif

/** 조각 Slice가 걷는 올린 원천 비트(낱말 안 자리 % SB_DRAG_SLICES == Slice — 낱말이 32비트라 둘째 낱말도 같은 꼴). 이어진 원천이 조각마다 고루 나뉜다. */
uint SB_DragSliceMask(uint Slice)
{
#if SB_DRAG_SLICES == 4
	return 0x11111111u << Slice;
#elif SB_DRAG_SLICES == 2
	return 0x55555555u << Slice;
#else
	return 0xffffffffu;
#endif
}

/** 이번 갱신에 돌 묶음 원천 수: 끄는 힘이나 붙든 자리가 남은 원천이 하나도 없으면 0 — 커널 합이 0이다. 수직으로만 누른 넓은 누름이 거의 공짜가 된다. */
uint SB_DragClusterCount()
{
	return DragHeader[SB_DRAG_HEADER_LIVE] == 0u ? 0u : min(DragHeader[SB_DRAG_HEADER_CLUSTERS], DragClusterCapacity);
}

/**
 * 묶음 준비: 셈할 칸 = bWant(바로잡기는 바로잡을 붙은 칸, 모으기는 모든 칸)이고 살이 있고 원천 상자(정지 위치 최소·최대를 자르는 거리 최대만큼 넓힌
 * 것) 안인 칸. 원천이 없거나 셈할 칸이 없으면 false(모든 스레드가 같은 값 — 장벽 뒤). Here = 제 칸 정지 위치(메시 로컬), w 1 = 셈한다. 셈할 칸들의
 * 상자와 덮인 몫을 세는 거리(CoverReach — 눌린 칸만, 0이면 안 센다)의 최대(원천을 올릴 때 묶음째 거르는 데), 묶음 살 칸 모두의 정지 상자(가까운 묶음
 * 가름)도 모은다. Local = 묶음 안 칸(0~63), Slice = 걸음 나누기 조각 — 상자·표지는 조각 0이 쓰고, 거리표 줄은 조각끼리 나눠 올린다. Here는 조각마다.
 */
bool SB_DragStage(int2 Cell, uint Local, uint Slice, bool bWant, float CoverReach, out float4 Here)
{
	uint Count = SB_DragClusterCount();
	Here = float4(0, 0, 0, 0);
	bool bInside = false;
	bool bLead = Slice == 0u;
	if (bLead && Local == 0u)
	{
		SB_CoarseActiveCells = 0u;
		SB_DragTouched = 0u;
		SB_DragCoverReach = 0u;
		SB_DragFarMask[0] = 0u;
		SB_DragNearTiles[0] = 0u;
	}
	if (bLead && Local < 2u)
	{
		SB_DragNearMask[0][Local] = 0u;
	}
	if (bLead && Local < 3u)
	{
		SB_CoarseBoxLo[Local] = 0xffffffffu;
		SB_CoarseBoxHi[Local] = 0u;
		SB_DragGroupLo[Local] = 0xffffffffu;
		SB_DragGroupHi[Local] = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (all(Cell < int2(DragResolution, DragResolution)))
	{
		float4 Info = CoarseInfo.Load(int3(Cell, 0));
		if (Info.w > 0.0)
		{
			if (bLead)
			{
				UNROLL
				for (uint Axis = 0u; Axis < 3u; ++Axis)
				{
					InterlockedMin(SB_DragGroupLo[Axis], SB_OrderedFloat(Info[Axis]));
					InterlockedMax(SB_DragGroupHi[Axis], SB_OrderedFloat(Info[Axis]));
				}
			}
			if (bWant && Count > 0u)
			{
				float Reach = (float)DragHeader[SB_DRAG_HEADER_REACH] * 0.001 / ThicknessScale;   // 메시 로컬
				float3 Lo = float3(SB_OrderedToFloat(DragHeader[SB_SOURCE_HEADER_MIN]), SB_OrderedToFloat(DragHeader[SB_SOURCE_HEADER_MIN + 1u]),
					SB_OrderedToFloat(DragHeader[SB_SOURCE_HEADER_MIN + 2u]));
				float3 Hi = float3(SB_OrderedToFloat(DragHeader[SB_SOURCE_HEADER_MAX]), SB_OrderedToFloat(DragHeader[SB_SOURCE_HEADER_MAX + 1u]),
					SB_OrderedToFloat(DragHeader[SB_SOURCE_HEADER_MAX + 2u]));
				bInside = all(Info.xyz >= Lo - Reach) && all(Info.xyz <= Hi + Reach);
				if (bInside)
				{
					Here = float4(Info.xyz, 1.0);
					if (bLead)
					{
						InterlockedAdd(SB_CoarseActiveCells, 1u);
						if (CoverReach > 0.0)
						{
							InterlockedMax(SB_DragCoverReach, asuint(CoverReach));   // 양수 부동소수는 비트 순서가 크기 순서
						}
						UNROLL
						for (uint Axis2 = 0u; Axis2 < 3u; ++Axis2)
						{
							InterlockedMin(SB_CoarseBoxLo[Axis2], SB_OrderedFloat(Info[Axis2]));
							InterlockedMax(SB_CoarseBoxHi[Axis2], SB_OrderedFloat(Info[Axis2]));
						}
					}
				}
			}
		}
	}
	if (bInside)
	{
		// 거리표 줄: 랜드마크 쌍 (Pair × 조각 수 + 조각)을 이 조각이 올린다 — 쌍마다 한 조각이 한 번(조각 1이면 쌍 0~63 차례로, 지금까지와 같다).
		uint Row = CoarseRow.Load(int3(Cell, 0));
		UNROLL
		for (uint Pair = 0u; Pair < SB_LANDMARK_MAX / 2u / SB_DRAG_SLICES; ++Pair)
		{
			uint M = 2u * (Pair * SB_DRAG_SLICES + Slice);
			uint Lo16 = M < NumLandmarks ? SB_LandmarkValue(Row, M) : SB_LANDMARK_UNREACHABLE;
			uint Hi16 = M + 1u < NumLandmarks ? SB_LandmarkValue(Row, M + 1u) : SB_LANDMARK_UNREACHABLE;
			SB_CoarseRows[(M >> 1u) * SB_COARSE_CELLS + Local] = Lo16 | (Hi16 << 16u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	return SB_CoarseActiveCells != 0u;
}

/**
 * 묶음 원천 Chunk번째 SB_DRAG_CLUSTER_CHUNK개를 가른다(모든 스레드가 부른다, 장벽 둘). 가까운 것(SB_DragNearRange[Parity])과 셈할 칸 상자에서 닿는 먼 것
 * (SB_DragCluster, SB_DragFarMask[Parity]). 상자까지 거리는 어느 칸에서 잰 직선보다 짧고 직선은 표면 거리의 하한이라, 거른 먼 묶음은 어느 칸에도 몫이 없다.
 * 올림은 조각 0의 스레드가 한다(걸음 나누기).
 */
void SB_DragStageClusters(uint Chunk, uint Count, uint Local, uint Slice, uint Parity)
{
	GroupMemoryBarrierWithGroupSync();   // 앞 묶음 원천들(먼 것 걷기, 가까운 묶음의 원천 걷기)을 다 읽은 뒤에 덮어쓴다
	if (Slice == 0u && Local == 0u)
	{
		SB_DragFarMask[1u - Parity] = 0u;   // 다음 올림의 표지(이번 표지는 앞 올림 안에서 비워 두었다)
		SB_DragNearTiles[1u - Parity] = 0u;
	}
	uint Cluster = Chunk * SB_DRAG_CLUSTER_CHUNK + Local;
	if (Slice == 0u && Local < SB_DRAG_CLUSTER_CHUNK && Cluster < Count)
	{
		uint Base = Cluster * SB_DRAG_CLUSTER_ROWS;
		uint4 Row0 = DragClusters[Base];
		uint4 Row1 = DragClusters[Base + 1u];
		float3 GroupLo = float3(SB_OrderedToFloat(SB_DragGroupLo[0]), SB_OrderedToFloat(SB_DragGroupLo[1]), SB_OrderedToFloat(SB_DragGroupLo[2]));
		float3 GroupHi = float3(SB_OrderedToFloat(SB_DragGroupHi[0]), SB_OrderedToFloat(SB_DragGroupHi[1]), SB_OrderedToFloat(SB_DragGroupHi[2]));
		float Gap = length(max(max(asfloat(Row0.xyz) - GroupHi, GroupLo - asfloat(Row1.xyz)), float3(0, 0, 0))) * ThicknessScale;
		bool bNear = (Row0.w & SB_DRAG_COMPACT_BIT) == 0u || Gap <= SB_DRAG_NEAR_TILES * (float)SB_COARSE_GROUP * DragTexelSize;
		if (bNear)
		{
			uint Index;
			InterlockedAdd(SB_DragNearTiles[Parity], 1u, Index);
			SB_DragNearRange[Parity][Index] = uint2(Row1.w, Row0.w & SB_DRAG_COUNT_MASK);
		}
		else
		{
			float4 Pull0 = DragClusterQ[Cluster * 2u];
			float4 Pull1 = DragClusterQ[Cluster * 2u + 1u];
			uint4 Row2 = DragClusters[Base + 2u];
			float3 Lo = float3(SB_OrderedToFloat(SB_CoarseBoxLo[0]), SB_OrderedToFloat(SB_CoarseBoxLo[1]), SB_OrderedToFloat(SB_CoarseBoxLo[2]));
			float3 Hi = float3(SB_OrderedToFloat(SB_CoarseBoxHi[0]), SB_OrderedToFloat(SB_CoarseBoxHi[1]), SB_OrderedToFloat(SB_CoarseBoxHi[2]));
			float GapPull = length(max(max(Lo - Pull0.xyz, Pull0.xyz - Hi), float3(0, 0, 0))) * ThicknessScale;
			float GapCover = length(max(max(Lo - asfloat(Row2.xyz), asfloat(Row2.xyz) - Hi), float3(0, 0, 0))) * ThicknessScale;
			bool bPull = Pull1.w > 0.0 && dot(Pull1.xyz, Pull1.xyz) > 0.0 && GapPull <= Pull1.w;
			bool bCover = asfloat(Row2.w) > 0.0 && GapCover <= asfloat(SB_DragCoverReach);
			if (bPull || bCover)
			{
				uint Slot = Local * SB_DRAG_CLUSTER_ROWS;
				SB_DragCluster[Slot] = asuint(Pull0);
				SB_DragCluster[Slot + 1u] = asuint(Pull1);
				SB_DragCluster[Slot + 2u] = Row2;
				UNROLL
				for (uint R = 3u; R < SB_DRAG_CLUSTER_ROWS; ++R)
				{
					SB_DragCluster[Slot + R] = DragClusters[Base + R];
				}
				InterlockedOr(SB_DragFarMask[Parity], 1u << Local);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
}

/**
 * 원천 목록 [Start, Start + Count)(가까운 묶음 하나의 원천들, Count ≤ 64)를 공유 메모리에 올리고, 셈할 칸 상자에서 제 자르는 거리 안인 것(눌린 원천은
 * 묶음의 덮인 몫 거리 안인 것도)만 비트로 켠다. 줄 1에는 이번 Q(DragSourceQ)와 붙음 비트를 넣는다. 모든 스레드가 부른다(장벽 둘). 올림은 조각 0의
 * 스레드가 한다(걸음 나누기).
 */
void SB_DragStageRange(uint Start, uint Count, uint Local, uint Slice, uint Parity)
{
	GroupMemoryBarrierWithGroupSync();   // 앞 원천들을 다 읽은 뒤에 덮어쓴다
	if (Slice == 0u && Local < 2u)
	{
		SB_DragNearMask[1u - Parity][Local] = 0u;   // 다음 올림의 표지
	}
	if (Slice == 0u && Local < Count)
	{
		uint Source = Start + Local;
		uint Base = Source * SB_DRAG_SOURCE_ROWS;
		uint4 Row0 = DragSources[Base];
		uint Row1W = DragSources[Base + 1u].w;
		uint4 Row2 = DragSources[Base + 2u];
		float3 Position = asfloat(Row0.xyz);
		float3 Lo = float3(SB_OrderedToFloat(SB_CoarseBoxLo[0]), SB_OrderedToFloat(SB_CoarseBoxLo[1]), SB_OrderedToFloat(SB_CoarseBoxLo[2]));
		float3 Hi = float3(SB_OrderedToFloat(SB_CoarseBoxHi[0]), SB_OrderedToFloat(SB_CoarseBoxHi[1]), SB_OrderedToFloat(SB_CoarseBoxHi[2]));
		float3 Outside = max(max(Lo - Position, Position - Hi), float3(0, 0, 0));
		float Gap = length(Outside) * ThicknessScale;
		float CoverGap = (Row1W & SB_DRAG_PRESSED_BIT) != 0u ? asfloat(SB_DragCoverReach) : 0.0;
		if (Gap <= max(asfloat(Row2.w), CoverGap))
		{
			uint Slot7 = Local * SB_DRAG_SOURCE_ROWS;
			float4 Pull = DragSourceQ[Source];
			SB_DragSource[Slot7] = Row0;
			SB_DragSource[Slot7 + 1u] = uint4(asuint(Pull.xyz), (Row1W & ~SB_DRAG_STUCK_BIT) | (Pull.w > 0.5 ? SB_DRAG_STUCK_BIT : 0u));
			SB_DragSource[Slot7 + 2u] = Row2;
			UNROLL
			for (uint R = 3u; R < SB_DRAG_SOURCE_ROWS; ++R)
			{
				SB_DragSource[Slot7 + R] = DragSources[Base + R];
			}
			InterlockedOr(SB_DragNearMask[Parity][Local >> 5u], 1u << (Local & 31u));
		}
	}
	GroupMemoryBarrierWithGroupSync();
}

/** 셈할 칸(Local)과 원천(가까운 랜드마크 Near)의 랜드마크 하한 max_m |d_im − d_jm|(cm). */
float SB_DragLowerBound(uint Local, uint4 Near[4])
{
	uint Lower = 0u;
	UNROLL
	for (uint k = 0u; k < 4u; ++k)
	{
		UNROLL
		for (uint c = 0u; c < 4u; ++c)
		{
			uint Key = Near[k][c];
			uint Dj = Key >> 8u;
			uint Di = SB_LandmarkRowValue(Local, Key & 0xffu);
			Lower = max(Lower, Di > Dj ? Di - Dj : Dj - Di);
		}
	}
	return (float)Lower * SB_LANDMARK_UNIT * ThicknessScale;
}

/** 마주봄 가림: 원천 → 칸 방향 Dir에서 두 노멀이 서로 마주 보면(접힌 틈) 몫을 줄인다. */
float SB_DragFacingGate(float3 Dir, float3 SourceNormal, float3 Normal)
{
	float Facing = min(dot(Dir, SourceNormal), -dot(Dir, Normal));
	return 1.0 - saturate((Facing - SB_DRAG_GATE_LO) / (SB_DRAG_GATE_HI - SB_DRAG_GATE_LO));
}

/**
 * 올린 원천 하나(공유 메모리 자리 Slot7)의 몫을 셈할 칸에 더한다: 끌림 K A Q(원천 노멀 → 칸 노멀로 돌려 — 접평면에 눕히기 전, 원천의 자르는 거리 안만)와
 * 덮인 몫 K A(CoverCut 안, 눌린 원천만 — 모으기의 눌린 칸만 CoverCut > 0).
 *  - 거리: 직선 d_c(정지 위치)가 자르는 거리를 넘으면 표를 읽지 않는다. 랜드마크 하한 d_l = max_m |d_im − d_jm|이 SB_DRAG_BOUND_TOL × d_c를 넘으면
 *    d = d_l(접힌 곳·다른 부위 — 하한도 참값 아래라 과대평가는 없다), 아니면 d = d_c. 제 칸(같은 거리표 줄)은 제 칸 몫 표.
 *  - 마주봄 가림: 두 칸의 노멀이 둘을 잇는 직선 쪽으로 서로 마주 보면(접힌 틈) 몫을 줄인다(제 칸은 그대로).
 *  - 회전: 원천 벡터를 원천 노멀 → 칸 노멀 최소 회전(SB_TurnByTilt)으로. 칸 노멀이 뒤집혀 있으면(차트 끝 조각 칸의 평균 노멀) 같은 접평면인 −n으로 —
 *    둔각으로 돌리면 벡터가 반대로 뒤집힌다(일지 177).
 */
void SB_DragSourceTerm(uint Slot7, uint Source, uint LocalIndex, float4 Here, uint Row, float3 Normal, float Ell, float CoverCut, inout float3 G,
	inout float Cover)
{
	uint4 Row0 = SB_DragSource[Slot7];
	uint4 Row1 = SB_DragSource[Slot7 + 1u];
	uint4 Row2 = SB_DragSource[Slot7 + 2u];
	float3 Delta = (Here.xyz - asfloat(Row0.xyz)) * ThicknessScale;
	float Chord = length(Delta);
	float Cut = asfloat(Row2.w);
	bool bCoverPair = CoverCut > 0.0 && (Row1.w & SB_DRAG_PRESSED_BIT) != 0u;
	float Reach = bCoverPair ? max(Cut, CoverCut) : Cut;
	if (Chord > Reach)
	{
		return;
	}
	bool bOwn = (Row1.w & SB_DRAG_ROW_MASK) == Row;
	float Area = asfloat(Row0.w);
	float3 SourceNormal = asfloat(Row2.xyz);
	float K;
	float Dist = 0.0;
	if (bOwn)
	{
		K = SB_DragSelfShareAt(DragTexelSize / Ell) * Area / (DragTexelSize * DragTexelSize);
	}
	else
	{
		uint4 Near[4] = {SB_DragSource[Slot7 + 3u], SB_DragSource[Slot7 + 4u], SB_DragSource[Slot7 + 5u], SB_DragSource[Slot7 + 6u]};
		float LowerCm = SB_DragLowerBound(LocalIndex, Near);
		Dist = LowerCm > SB_DRAG_BOUND_TOL * Chord ? LowerCm : Chord;
		if (Dist > Reach)
		{
			return;
		}
		K = Area * SB_BesselK0(Dist / Ell) / (2.0 * PI * Ell * Ell);
		K *= SB_DragFacingGate(Delta / max(Chord, 1e-6), SourceNormal, Normal);
	}
	// 부위 나누기: 받는 칸과 원천의 두 부위 몫으로 배율(제 칸·부위 밖은 1) — 솟음·함몰 꼬리와 같은 m_ij.
	BRANCH
	if (bSeparation != 0u)
	{
		K *= SB_SeparationMultiplier(SB_DragHereSeparation, DragSourceSeparation[Source]);
	}
	if (Dist <= Cut)
	{
		float3 ToNormal = dot(Normal, SourceNormal) < 0.0 ? -Normal : Normal;
		G += K * SB_TurnByTilt(cross(SourceNormal, ToNormal), asfloat(Row1.xyz));
	}
	if (bCoverPair && Dist <= CoverCut)
	{
		Cover += K;   // 덮인 몫: 눌린 원천만(기억은 바로잡기 걸음에 들지 않는다), 끌림과 상관없이 정해진 거리까지 — c가 Q에 흔들리지 않는다
	}
}

/**
 * 올린 먼 묶음 원천 하나(공유 메모리 자리 Slot)의 몫(일지 187). 받는 칸과 묶음 사이는 묶음 한 변 넘게 떨어져 있다.
 *  - 거리: 끌림 몫은 A|Q| 무게 가운데까지, 덮인 몫은 눌린 넓이 가운데까지 직선. 접힘 가름은 대표 칸(정지 기하로 정해진 칸)으로 — 랜드마크 하한이 대표
 *    칸까지 직선의 SB_DRAG_BOUND_TOL배를 넘으면 접힌 쌍이라 거리 = max(하한, 직선).
 *  - 커널 × (1 + r²/4ℓ²): K0는 원점 밖에서 ∇²K = K/ℓ²라, 둥글게 고루 퍼진 뭉치의 평균이 가운데 값의 I0(r/ℓ)배다(2차까지 1 + r²/4ℓ², r² = 가운데에서 잰
 *    무게 평균 거리²). 이 몫을 빼면 덮인 몫이 1.6~2.1% 작다(앉는 방석 모의).
 *  - 가림·회전은 묶음의 원천 넓이 무게 평균 노멀로.
 */
void SB_DragClusterTerm(uint Slot, uint LocalIndex, float4 Here, float3 Normal, float Ell, float CoverCut, inout float3 G, inout float Cover)
{
	uint4 Row0 = SB_DragCluster[Slot];        // A|Q| 무게 가운데, 퍼짐
	uint4 Row1 = SB_DragCluster[Slot + 1u];   // Σ A Q, 자르는 거리
	uint4 Row2 = SB_DragCluster[Slot + 2u];   // 눌린 넓이 가운데, 눌린 넓이
	uint4 Row3 = SB_DragCluster[Slot + 3u];   // 평균 노멀, 눌린 넓이 퍼짐
	uint4 Row4 = SB_DragCluster[Slot + 4u];   // 대표 칸
	float3 DeltaPull = (Here.xyz - asfloat(Row0.xyz)) * ThicknessScale;
	float3 DeltaCover = (Here.xyz - asfloat(Row2.xyz)) * ThicknessScale;
	float ChordPull = length(DeltaPull);
	float ChordCover = length(DeltaCover);
	float Cut = asfloat(Row1.w);
	float AreaCover = asfloat(Row2.w);
	bool bPull = Cut > 0.0 && ChordPull <= Cut;
	bool bCover = CoverCut > 0.0 && AreaCover > 0.0 && ChordCover <= CoverCut;
	if (!bPull && !bCover)
	{
		return;
	}
	uint4 Near[4] = {SB_DragCluster[Slot + 5u], SB_DragCluster[Slot + 6u], SB_DragCluster[Slot + 7u], SB_DragCluster[Slot + 8u]};
	float LowerCm = SB_DragLowerBound(LocalIndex, Near);
	bool bFolded = LowerCm > SB_DRAG_BOUND_TOL * length((Here.xyz - asfloat(Row4.xyz)) * ThicknessScale);
	float3 SourceNormal = asfloat(Row3.xyz);
	float Norm = 1.0 / (2.0 * PI * Ell * Ell);
	float Second = 0.25 / (Ell * Ell);
	if (bPull)
	{
		float Dist = bFolded ? max(LowerCm, ChordPull) : ChordPull;
		if (Dist <= Cut)
		{
			float K = SB_BesselK0(Dist / Ell) * Norm * (1.0 + asfloat(Row0.w) * Second);
			K *= SB_DragFacingGate(DeltaPull / max(ChordPull, 1e-6), SourceNormal, Normal);
			float3 ToNormal = dot(Normal, SourceNormal) < 0.0 ? -Normal : Normal;
			G += K * SB_TurnByTilt(cross(SourceNormal, ToNormal), asfloat(Row1.xyz));
		}
	}
	if (bCover)
	{
		float Dist = bFolded ? max(LowerCm, ChordCover) : ChordCover;
		if (Dist <= CoverCut)
		{
			float K = AreaCover * SB_BesselK0(Dist / Ell) * Norm * (1.0 + asfloat(Row3.w) * Second);
			Cover += K * SB_DragFacingGate(DeltaCover / max(ChordCover, 1e-6), SourceNormal, Normal);
		}
	}
}

/**
 * 셈할 칸(Here.w > 0)의 커널 합 G = Σ_원천 K(d; ℓ_칸) A_s Q_s(원천 노멀 → 칸 노멀로 돌려 더한 것 — 접평면에 눕히기 전)와 덮인 몫 c = Σ_눌린 원천 K A
 * (CoverCut 안 — 모으기의 눌린 칸만 CoverCut > 0). 가까운 묶음의 원천은 하나씩(SB_DragSourceTerm), 먼 묶음은 뭉친 원천으로(SB_DragClusterTerm).
 * 모든 스레드가 부른다(장벽). 셈할 칸이 없는 묶음은 루프를 돌지 않는다. 조각(걸음 나누기)마다 제 비트의 원천만 걷고, 끝에 조각 0이 부분합을 조각 차례로
 * 더한다 — 다 더한 G·Cover는 조각 0에만 있다.
 */
void SB_DragSum(uint Count, uint Local, uint Slice, bool bGroupActive, float4 Here, uint Row, float3 Normal, float Ell, float CoverCut, uint StatBase,
	out float3 G, out float Cover)
{
	G = float3(0, 0, 0);
	Cover = 0.0;
	uint Clusters = bGroupActive ? Count : 0u;   // 공유 메모리 값(장벽 뒤) — 묶음 안에서 같다
	uint Phase = 0u;                             // 원천 올림 차례(홀짝 표지)
	uint SliceMask = SB_DragSliceMask(Slice);    // 이 조각이 걷는 올린 원천 비트
	uint StatFar = 0u;                           // 집계(일지 189): 이 묶음(모든 조각)이 도는 먼 묶음 원천·가까운 원천 걸음, 가까운 묶음 올림 횟수(표지에서 세어
	uint StatNear = 0u;                          // 묶음 안에서 같다). 조각 하나는 그 1/조각 수 안팎을 걷는다.
	uint StatTiles = 0u;
	for (uint Chunk = 0u; Chunk * SB_DRAG_CLUSTER_CHUNK < Clusters; ++Chunk)
	{
		uint Parity = Chunk & 1u;
		SB_DragStageClusters(Chunk, Clusters, Local, Slice, Parity);
		StatFar += countbits(SB_DragFarMask[Parity]);
		if (Here.w > 0.0)   // 셈하지 않는 칸은 올리기만 돕는다(장벽 바깥의 분기)
		{
			uint Far = SB_DragFarMask[Parity] & SliceMask;
			while (Far != 0u)
			{
				uint Bit = firstbitlow(Far);
				Far &= Far - 1u;
				SB_DragClusterTerm(Bit * SB_DRAG_CLUSTER_ROWS, Local, Here, Normal, Ell, CoverCut, G, Cover);
			}
		}
		uint Tiles = SB_DragNearTiles[Parity];
		for (uint Tile = 0u; Tile < Tiles; ++Tile)
		{
			uint2 Range = SB_DragNearRange[Parity][Tile];
			uint Turn = Phase & 1u;
			SB_DragStageRange(Range.x, Range.y, Local, Slice, Turn);
			StatNear += countbits(SB_DragNearMask[Turn][0]) + countbits(SB_DragNearMask[Turn][1]);
			++StatTiles;
			if (Here.w > 0.0)
			{
				UNROLL
				for (uint Word = 0u; Word < 2u; ++Word)
				{
					uint Mask = SB_DragNearMask[Turn][Word] & SliceMask;
					while (Mask != 0u)
					{
						uint Bit = firstbitlow(Mask);
						Mask &= Mask - 1u;
						SB_DragSourceTerm((Word * 32u + Bit) * SB_DRAG_SOURCE_ROWS, Range.x + Word * 32u + Bit, Local, Here, Row, Normal, Ell, CoverCut, G, Cover);
					}
				}
			}
			++Phase;
		}
	}
#if SB_DRAG_SLICES > 1
	// 부분합 더하기: 모든 조각이 마지막 올림을 다 읽은 뒤(장벽) 조각 1..의 부분합을 원천 올림 자리에 [조각 − 1][칸]으로 쓰고(조각 3개 × 칸 64 = 192줄,
	// 올림 자리 448줄), 장벽 뒤 조각 0이 조각 차례로 더한다. 셈하지 않는 칸·원천이 없는 묶음도 같은 길을 간다(0을 더한다 — 장벽이 균일한 흐름에 있다).
	GroupMemoryBarrierWithGroupSync();
	if (Slice != 0u)
	{
		SB_DragSource[(Slice - 1u) * SB_COARSE_CELLS + Local] = asuint(float4(G, Cover));
	}
	GroupMemoryBarrierWithGroupSync();
	if (Slice == 0u)
	{
		UNROLL
		for (uint Part = 1u; Part < SB_DRAG_SLICES; ++Part)
		{
			float4 Partial = asfloat(SB_DragSource[(Part - 1u) * SB_COARSE_CELLS + Local]);
			G += Partial.xyz;
			Cover += Partial.w;
		}
	}
#endif
	if (Slice == 0u && Local == 0u && Clusters > 0u)
	{
		InterlockedAdd(TileCountOut[StatBase], 1u);
		InterlockedMax(TileCountOut[StatBase + 1u], StatFar + StatNear);
		InterlockedAdd(TileCountOut[StatBase + 2u], StatFar + StatNear);
		InterlockedMax(TileCountOut[StatBase + 3u], StatTiles);
		InterlockedMax(TileCountOut[StatBase + 4u], StatNear);
		InterlockedMax(TileCountOut[StatBase + 5u], StatFar);
	}
}

/**
 * 바로잡기 뒤: 묶음 원천의 끄는 힘 몫(A|Q| 무게 가운데·퍼짐, Σ A Q)을 고친 Q로 다시 모아 둘째 버퍼에 쓴다 — 모으기의 먼 묶음이 읽는다. 원천 패스와 같은
 * 합(SB_DragClusterPull), 같은 기준점(묶음 살 칸 정지 상자 최소). 자르는 거리는 원천 패스 값 그대로(닿게 할 힘이 바로잡기가 키울 수 있는 몫까지 넉넉하다).
 * 모든 스레드가 부른다(장벽). 칸 표는 조각 0이 쓴다(걸음 나누기 — 다른 조각은 장벽만 지난다).
 */
void SB_DragRebuildPull(int2 Cell, uint Local, uint Slice, bool bSource, float3 Q, uint Cluster)
{
	if (Slice == 0u && Local == 0u)
	{
		SB_DragTileSlot = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (Slice == 0u)
	{
		float3 Reference = float3(SB_OrderedToFloat(SB_DragGroupLo[0]), SB_OrderedToFloat(SB_DragGroupLo[1]), SB_OrderedToFloat(SB_DragGroupLo[2]));
		float4 Info = bSource ? CoarseInfo.Load(int3(Cell, 0)) : float4(0, 0, 0, 0);
		SB_DragTileRest[Local] = bSource ? float4(Info.xyz - Reference, Info.w * ThicknessScale * ThicknessScale) : float4(0, 0, 0, 0);
		SB_DragTilePull[Local] = float4(Q, bSource ? 1.0 : 0.0);
		if (bSource)
		{
			InterlockedMax(SB_DragTileSlot, Cluster);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (Slice == 0u && Local == 0u && SB_DragTileSlot != 0u)
	{
		float3 Reference = float3(SB_OrderedToFloat(SB_DragGroupLo[0]), SB_OrderedToFloat(SB_DragGroupLo[1]), SB_OrderedToFloat(SB_DragGroupLo[2]));
		uint Slot = SB_DragTileSlot - 1u;
		float3 Centre;
		float Spread;
		float3 SumAQ;
		SB_DragClusterPull(Centre, Spread, SumAQ);
		DragClusterQOut[Slot * 2u] = float4(Centre + Reference, Spread);
		DragClusterQOut[Slot * 2u + 1u] = float4(SumAQ, DragClusterQ[Slot * 2u + 1u].w);
	}
}

/**
 * 바로잡기(거친 격자 전체, 바로잡을 붙은 칸만 셈): 같은 커널 합(SB_DragSum)으로 G를 세어 Q += w (H − G)로 한 걸음 더 고친다(접평면, 힘 한도 Q_lim로
 * 자름 — 넘으면 그 칸은 미끄러진다). 새 식이 아니라 원천 패스와 같은 식의 둘째 걸음이다. 그 밖의 원천(맞은 붙은 칸, 미끄러지는 칸, 기억)은 Q를 그대로
 * 옮긴다. 지난 갱신의 Q에서 이어 오므로 멈춘 누름은 갱신마다 H에 다가가 붙든 자리와 같아진다(시험 캐릭터 가슴 모의: 초당 60·20갱신 모두 멈추고 1초 안에
 * 0.001 mm 안 — 바로잡기가 없으면 20갱신에서 0.84 mm 남았다, 일지 183). 지난 차가 남은 칸만 세므로 멈춘 누름에서는 거의 돌지 않는다(일지 184). 끄는
 * 힘이 남은 원천이 없으면 커널 합을 건너뛴다. 끝에 묶음 원천의 끄는 힘 몫을 고친 Q로 다시 모은다(SB_DragRebuildPull).
 */
[numthreads(SB_COARSE_GROUP, SB_COARSE_GROUP, SB_DRAG_SLICES)]
void DragCorrectCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	int2 Cell = int2(GroupId.xy * SB_COARSE_GROUP + LocalId.xy);
	uint Slice = LocalId.z;                                // 걸음 나누기 조각(SB_DRAG_SLICES)
	uint Local = LocalId.y * SB_COARSE_GROUP + LocalId.x;   // 묶음 안 칸 — 조각마다 같은 칸 64개(조각 1이면 SV_GroupIndex와 같다)
	bool bLead = Slice == 0u;                              // 끄는 힘 쓰기·다시 모으기는 조각 0이(다 더한 G가 조각 0에 있다)
	bool bInGrid = all(Cell < int2(DragResolution, DragResolution));
	uint Row = bInGrid ? CoarseRow.Load(int3(Cell, 0)) : SB_LANDMARK_NO_ROW;
	float4 HoldNow = float4(0, 0, 0, 0);
	float4 Pull = float4(0, 0, 0, 0);
	uint Slot = 0u;
	if (Row != SB_LANDMARK_NO_ROW)
	{
		HoldNow = DragHoldNext.Load(int3(Cell, 0));
		if (HoldNow.w > 0.5)
		{
			Slot = (uint)HoldNow.w - 1u;
			Pull = DragSourceQ[Slot];
		}
	}
	bool bSource = HoldNow.w > 0.5;
	bool bCorrect = bSource && Pull.w > 1.5;
	float4 Here;
	bool bGroupActive = SB_DragStage(Cell, Local, Slice, bCorrect, 0.0, Here);
	uint Count = SB_DragClusterCount();
	float3 Normal = float3(0, 0, 1);
	float Ell = 1.0;
	if (bCorrect)
	{
		float4 Input1 = SB_DragInputRow(Cell, 1u);
		Normal = Input1.xyz;
		Ell = max(SB_ShearLength(max(Input1.w, 1e-3), SB_DragSkinLength(Cell)), 1e-3);
	}
	float3 G;
	float Cover;
	SB_DragHereSeparation = bSeparation != 0u && bInGrid ? CoarseSeparation.Load(int3(Cell, 0)) : float2(0, 0);
	SB_DragSum(Count, Local, Slice, bGroupActive, Here, Row, Normal, Ell, 0.0, SB_DRAG_STAT_CORRECT, G, Cover);
	float3 QNow = Pull.xyz;
	uint Cluster = 0u;
	if (bSource && bLead)
	{
		uint4 Row1 = DragSources[Slot * SB_DRAG_SOURCE_ROWS + 1u];   // x 걸음 w, y 힘 한도 Q_lim, z 제 묶음 원천 자리
		Cluster = Row1.z + 1u;
		if (bCorrect)
		{
			G -= Normal * dot(G, Normal);
			float2 Step = asfloat(Row1.xy);
			float3 Q = Pull.xyz + Step.x * (HoldNow.xyz - G);
			Q -= Normal * dot(Q, Normal);
			float QLength = length(Q);
			bool bStill = QLength <= Step.y;
			if (!bStill)
			{
				Q *= Step.y / max(QLength, 1e-12);
			}
			DragSourceQOut[Slot] = float4(Q, bStill ? 2.0 : 0.0);
			QNow = Q;
		}
		else
		{
			DragSourceQOut[Slot] = Pull;
		}
	}
	SB_DragRebuildPull(Cell, Local, Slice, bSource && bLead, QNow, Cluster);
}

/**
 * 모으기(거친 칸 전체): G = Σ_원천 K A Q(SB_DragSum)를 접평면에 눕힌다 — 붙은 칸·미끄러지는 칸·꼬리 모두 같은 식이라 눌림 경계에서 끊기지 않는다(일지 183).
 * 덮인 몫 c = 눌린 원천의 Σ K A(눌린 칸만, SB_DRAG_CUT_MIN ℓ 안 — 다음 갱신의 원천 패스가 읽는다). 모든 살 칸에 쓴다(원천이 닿지 않는 칸은 0). 붙든
 * 자리와 끌림의 차 H − G(붙은 칸은 원천 패스의 H, 미끄러지는 칸은 0 — 붙든 자리가 보이는 자리라 다시 붙을 때 그 자리에서 붙어 뛰지 않는다)와 끄는 힘
 * Q(바로잡기 뒤 값)를 DragHold 한 장에 묶어 옮긴다. 끄는 힘이 남은 원천이 없으면 원천을 올리지 않는다(SB_DragClusterCount).
 */
[numthreads(SB_COARSE_GROUP, SB_COARSE_GROUP, SB_DRAG_SLICES)]
void DragGatherCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	int2 Cell = int2(GroupId.xy * SB_COARSE_GROUP + LocalId.xy);
	uint Slice = LocalId.z;                                // 걸음 나누기 조각(SB_DRAG_SLICES)
	uint Local = LocalId.y * SB_COARSE_GROUP + LocalId.x;   // 묶음 안 칸 — 조각마다 같은 칸 64개(조각 1이면 SV_GroupIndex와 같다)
	bool bLead = Slice == 0u;                              // 칸의 출력은 조각 0이(다 더한 G·Cover가 조각 0에 있다)
	bool bInGrid = all(Cell < int2(DragResolution, DragResolution));
	// 제 칸: 거리표 줄, 노멀, ℓ, 눌림(살 칸만 읽는다). 눌린 칸은 덮인 몫을 SB_DRAG_CUT_MIN ℓ까지 센다(ℓ은 원천 패스처럼 두꺼운 살 쪽으로 넉넉히).
	uint Row = bInGrid ? CoarseRow.Load(int3(Cell, 0)) : SB_LANDMARK_NO_ROW;
	float3 Normal = float3(0, 0, 1);
	float Ell = 1.0;
	float Pressed = 0.0;
	float4 HoldNow = float4(0, 0, 0, 0);
	if (Row != SB_LANDMARK_NO_ROW)
	{
		float4 Input1 = SB_DragInputRow(Cell, 1u);
		Normal = Input1.xyz;
		Ell = max(SB_ShearLength(max(Input1.w, 1e-3), SB_DragSkinLength(Cell)), 1e-3);
		// 눌림 = 원천 패스가 정한 것 그대로(원천 줄의 눌림 비트 — 콜라이더 누름과 잡기 원, ADR-0041). 다음 갱신의 "지난 갱신에도 눌림"·덮인 몫·걸음이
		// 이것으로 정해진다(일지 349 — 콜라이더 누름만 다시 세면 잡힌 칸이 갱신마다 새로 눌린 칸이 되었다). 원천 목록이 넘친 칸만 콜라이더 누름으로 센다.
		HoldNow = DragHoldNext.Load(int3(Cell, 0));
		if (HoldNow.w > 0.5)
		{
			Pressed = (DragSources[((uint)HoldNow.w - 1u) * SB_DRAG_SOURCE_ROWS + 1u].w & SB_DRAG_PRESSED_BIT) != 0u ? 1.0 : 0.0;
		}
		else
		{
			Pressed = SB_DragInputRow(Cell, 0u).w >= 0.0 ? 1.0 : 0.0;
		}
	}
	float CoverCut = Pressed > 0.0 ? SB_DRAG_CUT_MIN * max(DragReachMax, Ell) : 0.0;
	float4 Here;
	bool bGroupActive = SB_DragStage(Cell, Local, Slice, true, CoverCut, Here);
	uint Count = SB_DragClusterCount();
	float3 G;
	float Cover;
	SB_DragHereSeparation = bSeparation != 0u && bInGrid ? CoarseSeparation.Load(int3(Cell, 0)) : float2(0, 0);
	SB_DragSum(Count, Local, Slice, bGroupActive, Here, Row, Normal, Ell, CoverCut, SB_DRAG_STAT_GATHER, G, Cover);
	if (bLead && dot(G, G) > 0.0)
	{
		InterlockedOr(SB_DragTouched, 1u);
	}
	if (bInGrid && bLead)
	{
		if (Row != SB_LANDMARK_NO_ROW)
		{
			// Pressed는 원천 패스의 bPressed 그대로(위).
			G -= Normal * dot(G, Normal);
			float3 Lead = Pressed > 0.0 ? HoldNow.xyz - G : float3(0, 0, 0);   // 붙든 자리 − 끌림
			float3 Q = float3(0, 0, 0);
			if (HoldNow.w > 0.5)
			{
				float4 Pull = DragSourceQ[(uint)HoldNow.w - 1u];
				Q = Pull.xyz;
				if (Pull.w <= 0.5)
				{
					Lead = float3(0, 0, 0);   // 미끄러지는 칸(과 기억): 붙든 자리 = 보이는 자리
				}
			}
			DragStateOut[Cell] = float4(G, Pressed > 0.0 ? 1.0 + Cover : 0.0);
			DragHoldOut[Cell] = SB_DragPackHold(Lead, Q);
		}
		else
		{
			DragStateOut[Cell] = float4(0, 0, 0, 0);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u && SB_DragTouched != 0u)
	{
		InterlockedAdd(TileCountOut[2], 1u);
	}
}

/** 3차 B-스플라인 무게 (t = 0~1, 탭 네 개). */
float4 SB_BSplineWeights(float T)
{
	float T2 = T * T;
	float T3 = T2 * T;
	float OneMinus = 1.0 - T;
	return float4(OneMinus * OneMinus * OneMinus, 3.0 * T3 - 6.0 * T2 + 4.0, -3.0 * T3 + 3.0 * T2 + 3.0 * T + 1.0, T3) / 6.0;
}

/**
 * 가는 칸 자리 Where(정수 = 칸 한가운데 — 정확한 고스트 칸은 가리키는 점)에서 거친 격자 Source(솟음 S·R 또는 함몰 거친 해 U)를 거친 칸 4×4의
 * 3차 B-스플라인으로 읽는다. 쌍선형은 거친 칸 경계마다 기울기가 꺾여 조명에 격자가 비친다. 탭 무게는 SB_CoarseTapWeight로 거른다(격자 밖·살 없는
 * 거친 칸은 0, 정지 위치가 먼 탭 — 아틀라스에서 옆에 놓였을 뿐인 다른 차트 — 은 거친 칸 4개부터 6개 사이에서 서서히 0) — 그 뒤 무게 합으로 다시
 * 나눈다. 솟음·함몰이 이 한 읽기를 같이 쓴다(일지 201).
 */
float SB_CoarseSpline(Texture2D<float> Source, float2 Where, float3 RestP)
{
	float2 Q = (Where + 0.5) / (float)CoarseBlock - 0.5;
	int2 First = (int2)floor(Q) - 1;
	float2 T = Q - floor(Q);
	float4 WX = SB_BSplineWeights(T.x);
	float4 WY = SB_BSplineWeights(T.y);
	float CoarseLocal = (float)CoarseBlock * TexelSize / ThicknessScale;
	float Sum = 0.0;
	float Weight = 0.0;
	UNROLL
	for (int Y = 0; Y < 4; ++Y)
	{
		UNROLL
		for (int X = 0; X < 4; ++X)
		{
			int2 Cell = First + int2(X, Y);
			float Near = SB_CoarseTapWeight(Cell, RestP, CoarseLocal);
			if (Near <= 0.0)
			{
				continue;
			}
			float W = WX[X] * WY[Y] * Near;
			Sum += W * Source.Load(int3(Cell, 0));
			Weight += W;
		}
	}
	return Weight > 1e-6 ? Sum / Weight : 0.0;
}


/**
 * 가는 칸 자리 Where(정수 = 칸 한가운데 — 정확한 고스트 칸은 가리키는 점)의 거친 해: 거친 칸 4×4를 3차 B-스플라인으로(SB_CoarseSpline — 솟음 S와 같은
 * 읽기). 가장 가까운 거친 칸이 0이면 0(1 µm 밑의 끝자락 — 모든 칸이 16탭을 읽지 않게). 이 조기 반환은 함몰만의 규칙이라 공용 읽기 밖에 둔다(솟음 S에는 없다).
 */
float SB_DentCoarseSpline(float2 Where, float3 RestP)
{
	float2 Q = (Where + 0.5) / (float)CoarseBlock - 0.5;
	int2 Nearest = clamp((int2)round(Q), int2(0, 0), int2(CoarseResolution - 1, CoarseResolution - 1));
	if (DentCoarse.Load(int3(Nearest, 0)) == 0.0)
	{
		return 0.0;
	}
	return SB_CoarseSpline(DentCoarse, Where, RestP);
}

/**
 * 합치기·노멀이 보이는 칸 S의 함몰(일지 198·199): 목록 묶음은 상태 A, 목록 밖은 거친 해. 정확한 고스트 칸은 가리키는 점에서 — 고스트 칸이 든 묶음이
 * 목록에 있으면 옮기기가 가리키는 점의 값을 적어 두었다. 목록 밖 고스트 칸이 목록 묶음을 가리키면(이음매 한쪽만 목록) 그 점 둘레 네 칸을 쌍선형으로(목록
 * 칸은 상태 A, 목록 밖 칸은 거친 해) — 거친 해만 읽으면 이음매에 |u − U|만큼 턱이 선다.
 */
float SB_ShownDent(int2 S)
{
	float U = StateIn.Load(int3(S, 0));
	if (DentCoarseOn == 0u || SB_TileListed(S))
	{
		return U;
	}
	float3 RestP = CarrierPosition.Load(int3(S, 0)).xyz;
	if (SB_KindAt(S) != SB_KIND_GHOST)
	{
		return SB_DentCoarseSpline((float2)S, RestP);
	}
	float2 Target = CarrierRedirect.Load(int3(S, 0)).xy;
	int2 Base = clamp((int2)floor(Target), int2(0, 0), int2(Resolution - 2, Resolution - 2));
	if (!SB_TileListed(Base) && !SB_TileListed(Base + int2(1, 1)) && !SB_TileListed(Base + int2(1, 0)) && !SB_TileListed(Base + int2(0, 1)))
	{
		return SB_DentCoarseSpline(Target, RestP);
	}
	float2 F = saturate(Target - (float2)Base);
	float Tap[4];
	UNROLL
	for (int k = 0; k < 4; ++k)
	{
		int2 C = Base + int2(k & 1, k >> 1);
		Tap[k] = SB_TileListed(C) ? StateIn.Load(int3(C, 0)) : SB_CoarseBilinear(DentCoarse, C);
	}
	return lerp(lerp(Tap[0], Tap[1], F.x), lerp(Tap[2], Tap[3], F.x), F.y);
}

// --- ComposeCS / NormalCS 공용: 이웃 칸 ---
Texture2D<float4> DisplacementIn;    // ComposeCS의 결과 (NormalCS가 읽는다)
Texture2D<float4> LumpNormalIn;      // ContactCS의 LumpNormalOut (ADR-0018, LumpMode가 0이면 읽지 않는다)

// --- ComposeCS ---
RWTexture2D<float4> DisplacementOut; // rgb 최종 변위(프레임 cm), a 가장자리 부호 거리 + SB_EDGE_RANGE(칸 수, ADR-0029)에 벽 없음 양(ADR-0035)을 묶은 값(SB_PackEdge) — 머티리얼 쪼갠 점이 읽는다
Texture2D<float> EdgeIn;             // ContactCS의 EdgeOut

/**
 * 긴 몫 Rise(cm, > 0 — 살층 응답이 둘레로 올리는 몫, ADR-0037)를 얹은 안쪽 깊이(cm, 안쪽이 +). 솟는 살은 한도 Limit(누른 깊이 d, 누르지 않은 칸은
 * 위로 빈 높이 — 피부 점에서 바깥 노멀로 곧게 올라가 콜라이더에 닿기까지 — 와 그 칸의 살 두께 가운데 작은 쪽의 음수, 접촉 패스가 적는다)을 넘지 않는다 —
 * 넘는 몫은 버린다. 한도에 닿는 경계는 틈이 아니라 조금 겹치는 쪽으로 둥글린다(높이 Round).
 */
float SB_RaisedDepth(float U, float Rise, float Limit, float Round)
{
	if (!(Rise > 0.0))
	{
		return U;
	}
	float Lifted = U - Rise;
	float Soft = min(Round, Rise);
	float Q = max(Soft - abs(Limit - Lifted), 0.0) / Soft;
	return max(Limit, Lifted) - 0.25 * Soft * Q * Q;
}

// 멈춤 표지(일지 203): 지난 갱신의 변위(복사 패스가 갱신 머리에서 옮긴 것)와 견줘 어느 칸의 성분이 StillChange(cm)보다 바뀌면 묶음마다 한 번 TileCountOut에 적는다.
// StillChange < 0이면 멈춤이 꺼진 것(견주지 않는다).
Texture2D<float4> PrevDisplacementIn;
float StillChange;
groupshared uint SB_ComposeChanged;

// --- 이음 압축 몫 (일지 323) ---
// 화면: 손이 지나간 쪽 함몰 벽이 칸 하나 안에서 0.6~1 cm 끊긴 절벽이었고, 나나이트 테셀레이션이 그 절벽을 길게 늘어난 잔삼각형으로 그려 결이 났다.
// 브라처럼 피부에 딱 붙은 천은 같은 절벽을 밑 피부와 다르게 쪼개 그려, 그 틈으로 피부가 이빨처럼 튀어나왔다(Edge Smoothing과 무관). 함몰 해 U는 손 둘레를
// 가로질러 매끈하다 — 절벽은 합치기가 손이 붙든 칸만 손 깊이 P에 세우고, 둘레 칸은 살 속 압축 몫 γ·h·A₁·p를 뺀 해 U − L로 그려 그 몫이 폭 없이
// 끊겨서 생긴다. 이음은 붙든 칸마다 손 깊이가 매끈한 면보다 깊은 단차 P − (U − L)을 재어, 둘레 SB_SQUEEZE_REACH칸 안으로 거리에 따라 줄며 이어
// 그린다(붙든 칸은 전처럼 손 깊이보다 얕게 그리지 않는다). 받는 칸은 붙들지 않은 칸과, 붙든 칸 가운데 가장자리 띠(안쪽 0.5칸까지 다 받고 1.5칸에서 0)
// 뿐이다 — 더 안쪽은 손 모양 그대로다(칸마다 단차가 들쭉날쭉해 평균을 얹으면 손 밑이 평균 0.2 cm 뜬다, 일지 323 셋째 시험). 손 가장자리에서 둘레가
// 손 깊이와 이어지고, Edge Smoothing이 얕게 다듬은 가장자리 칸도 안쪽 단차를 받아 절벽이 손 안쪽 칸 사이로 옮겨 서지 않는다. 압축 몫을 거친 압력 p로 바로 그리지 않는 것은 풀이의 p가 거친 칸마다 0과 큰 값을 오가서다(일지
// 323 첫 시험 — 둘레가 울퉁불퉁했다). 그리기만 바뀐다: 함몰·기억·받아들인 깊이는 그대로라 되먹임이 없다. 대가: 손 둘레 몇 칸이 그 단차만큼 더 꺼지고,
// 가장자리 띠에서 단차가 이웃보다 작은 붙든 칸(둥근 손가락 옆면)은 손보다 조금 깊게 그려진다. 칸마다 이웃 (2R + 1)²칸을 읽는다(R = 3, 비용은 일지 323).
#define SB_SQUEEZE_REACH 3
// 덮인 칸 베끼기(아래 GhostGatherCS·GhostApplyCS, 일지 329): 합치기는 고스트 칸의 멈춤 표지를 건너뛴다.

/**
 * 칸 S 둘레(SB_SQUEEZE_REACH칸 안)의 붙든 칸 단차 P − (U − L)의 거리 가중 평균에, 가장 가까운 붙든 칸까지 거리가 1칸이면 1·REACH + 1칸이면 0으로 줄어드는
 * 몫을 곱한 값(cm). L은 이 칸의 긴 몫을 그대로 쓴다(거친 칸 3차라 몇 칸 안에서는 거의 같다). 이번 갱신에 목록이 선 묶음의 붙든 칸만 본다.
 */
float SB_SqueezeAround(int2 S, float L)
{
	uint Width;
	uint Height;
	EdgeIn.GetDimensions(Width, Height);
	float SumStep = 0.0;
	float SumWeight = 0.0;
	float Nearest = 1e4;
	const int R = SB_SQUEEZE_REACH;
	LOOP
	for (int Y = -R; Y <= R; ++Y)
	{
		LOOP
		for (int X = -R; X <= R; ++X)
		{
			float Distance = length(float2(X, Y));
			int2 Q = S + int2(X, Y);
			if (Distance > (float)R + 0.5 || any(Q < 0) || Q.x >= (int)Width || Q.y >= (int)Height || !SB_IsCarried(SB_KindAt(Q)) || !SB_TileListed(Q))
			{
				continue;
			}
			float PinQ = DentPinsIn.Load(int3(Q, 0)).y;
			if (!(PinQ > 0.0))
			{
				continue;
			}
			float Step = max(PinQ - (StateIn.Load(int3(Q, 0)) - L), 0.0);
			float Weight = 1.0 - Distance / ((float)R + 1.0);
			Weight *= Weight;
			SumStep += Weight * Step;
			SumWeight += Weight;
			Nearest = min(Nearest, Distance);
		}
	}
	if (!(SumWeight > 0.0))
	{
		return 0.0;
	}
	return SumStep / SumWeight * (1.0 - smoothstep(1.0, (float)R + 1.0, Nearest));
}

/**
 * 칸 S(가장자리 부호 거리 E)에서 Step 방향으로 가장자리를 처음 건너는 곳까지 거리(칸 수). 건너는 곳은 같은 쪽 마지막 칸과 반대쪽 첫 칸의
 * 부호 거리 사이 0 자리 — 쪼갠 점이 벽을 세우는 자리와 같은 직선 보간이라, 칸마다 켜지고 꺼지지 않고 가장자리를 따라 이어진다.
 * 3칸 안에 없거나 캐리어 밖·텍스처 밖에 닿으면 3(모른다 — 좁지 않다). 고스트 칸은 제 광선 값이라 그대로 읽는다.
 */
float SB_CrossingAlong(int2 S, int2 Step, float E)
{
	uint Width;
	uint Height;
	EdgeIn.GetDimensions(Width, Height);
	float Prev = E;
	UNROLL
	for (int K = 1; K <= 3; ++K)
	{
		int2 Q = S + Step * K;
		if (any(Q < 0) || Q.x >= (int)Width || Q.y >= (int)Height || !SB_IsCarried(SB_KindAt(Q)))
		{
			return 3.0;
		}
		float Next;
		SB_UnpackEdge(EdgeIn.Load(int3(Q, 0)), Next);
		if ((Next > 0.0) != (E > 0.0))
		{
			return (float)(K - 1) + abs(Prev) / max(abs(Prev) + abs(Next), 1e-6);
		}
		Prev = Next;
	}
	return 3.0;
}

/**
 * 변위 A에 적는 값(SB_PackEdge): 접촉 패스의 가장자리 값과 벽 없음 양에, 좁은 틈·섬의 몫을 더한다(ADR-0035). 가장자리 띠 안 칸의 한 축에서
 * 양쪽 가장자리 사이 폭 W(칸 수, 연속)가 2칸 아래면 다 풀고 3칸까지 서서히 다시 세운다 — 두 칸 사이에 벽 둘을 세우면 칸 가운데가
 * 정렬에 따라 그 사이에 들거나 빠져 홈·날이 칸 격자에 묶이므로 보통 보간으로 둔다(폭 2~3칸의 둔덕). 두 몫 가운데 큰 쪽을 쓴다.
 */
float SB_ComposeEdge(int2 S)
{
	float Packed = EdgeIn.Load(int3(S, 0));
	float E;
	float NoWall = SB_UnpackEdge(Packed, E);
	if (abs(E) >= SB_EDGE_RANGE)
	{
		return Packed;
	}
	float W = min(SB_CrossingAlong(S, int2(1, 0), E) + SB_CrossingAlong(S, int2(-1, 0), E),
		SB_CrossingAlong(S, int2(0, 1), E) + SB_CrossingAlong(S, int2(0, -1), E));
	return SB_PackEdge(E, max(NoWall, saturate(3.0 - W)));
}

/**
 * 접촉 묶음 목록만 돈다: 목록 밖 묶음은 칸이 없어 변위가 늘 0이다(텍스처를 만들 때와 ClearCS가 0으로 둔다). 묶음 합(멈춤 표지)의 장벽을 모두 지나야 하므로
 * 일찍 돌아가지 않는다(D3D11 X3663).
 */
[numthreads(8, 8, 1)]
void ComposeCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex == 0u)
	{
		SB_ComposeChanged = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	// 이번 갱신에 함몰 목록이 섰는가(ActivityArgsCS가 비우고 TileListCS가 쓴다 — R32 한 채널이라 D3D11도 읽는다)
	bool bDentListed = TileCountOut[0] != 0u;
	int2 T;
	if (SB_ListedTexel(GroupId, LocalId, T))
	{
		float4 Out = float4(0, 0, 0, 0);
		if (SB_IsCarried(SB_KindAt(T)))
		{
			int2 S = T;
			float U = SB_ShownDent(S);   // 목록 밖은 거친 해(일지 199)
			float4 Contact = ContactIn.Load(int3(S, 0));
			// 함몰 u는 늘 관통 깊이 d 이상이라(DentCS의 max — 붙든 칸은 붙들 깊이 아래로 돌아가지 않는다) 닿은 곳은 콜라이더 모양 그대로, 둘레는 완만하게 잦아든다.
			// 둘레로 솟는 살(ADR-0011)을 빼고, 끌림(ADR-0008)은 표면을 따라 더한다 — 이웃 칸과 끌린 거리가 다르면
			// 늘어나거나 몰린 모양이 노멀에 따라온다.
			float Depth = U;
			// 끌림·국소 모양·긴 몫을 읽는 자리: 정확한 고스트 칸은 가리키는 점
			float2 Where = SB_KindAt(S) == SB_KIND_GHOST ? CarrierRedirect.Load(int3(S, 0)).xy : (float2)S;
			float LongHere = 0.0;   // 이 칸의 긴 몫 L(Basic은 0) — 아래 이음(일지 323)이 쓴다
			BRANCH
			if (bContactLong != 0u)
			{
				// 긴 몫 L(ADR-0037): 함몰 U는 붙든 칸 목표 P + L − γ·h·A₁·p에 맞춰 풀었으므로 L을 빼면 보이는 깊이다. 솟음(L > 0)은 콜라이더 위 빈 높이·살 두께
				// 한도 안에서(SB_RaisedDepth), 둥글림 높이는 그 칸의 함몰 둘레 폭 ℓ₁. 칸 읽기는 함몰과 같은 값을 지나는 3차(SB_ContactLongAt).
				float3 RestP = CarrierPosition.Load(int3(S, 0)).xyz;
				float L = SB_ContactLongAt(Where, RestP).x;
				LongHere = L;
				float Layer = max(CarrierNormal.Load(int3(S, 0)).w * ThicknessScale, 1e-3);
				float Round = max(SB_WindowLength(SB_VolumeAt(S), Layer, SB_SkinLengthAt(S) * ThicknessScale), 2.0 * TexelSize);
				Depth = L > 0.0 ? SB_RaisedDepth(U, L, Contact.w, Round) : U - L;
				// 잡기 둘레(ADR-0041): U − L 그대로 — 솟음 한도(콜라이더 위 빈 높이·살 두께)는 누름의 솟음 몫에 거는 것이라, 잡아 끈 살을 꼬집는 손 콜라이더
				// 밑에 붙들지 않는다(한도는 잡기 한도 min(h, R)가 맡는다).
				BRANCH
				if (GrabCount != 0u && SB_GrabNear(RestP))
				{
					Depth = U - L;
				}
			}
			// 붙든 칸(붙들 깊이 P > 0)만 손 깊이에 선다: 목표에서 뺀 칸보다 짧은 압축 몫(γ)과 셀이 접힌 몫은 살 속에서 일어나 겉면에 보이지 않는다(ADR-0037
			// §3.1 ⑥). 붙들지 않은 칸(P = 0)에 걸면 목록 묶음 안의 솟음이 0으로 잘려 목록 모양 계단이 선다(일지 277). 붙들 깊이는 함몰 첫 패스가 적은 것 —
			// 이번 갱신에 목록 패스가 돌았을 때만(안 돈 갱신의 목록 비트·DentPins는 아무도 쓰지 않은 임시 값이다).
			float Pinned = 0.0;
			BRANCH
			if (bDentListed && SB_TileListed(S))
			{
				Pinned = DentPinsIn.Load(int3(S, 0)).y;
			}
			// 이음(일지 323): 둘레 붙든 칸의 단차를 이어 그린다 — 붙들지 않은 칸과 붙든 칸의 가장자리 띠만.
			// Basic(긴 몫 없음)은 붙든 칸 목표가 손 깊이라 단차가 없어 건너뛴다.
			// 붙든 칸은 손 깊이보다 얕게 그리지 않는다.
			BRANCH
			if (bDentListed && bContactLong != 0u)
			{
				float EdgeCells;
				SB_UnpackEdge(EdgeIn.Load(int3(S, 0)), EdgeCells);
				float Share = Pinned > 0.0 ? 1.0 - smoothstep(0.5, 1.5, EdgeCells) : 1.0;
				BRANCH
				if (Share > 0.0)
				{
					// 붙든 칸은 매끈한 면 U − L에 얹는다 — 솟음(L > 0) 한도가 붙든 칸에서는 누른 깊이라, 한도 뒤 깊이에 얹으면 손보다 그만큼 깊다.
					float Join = Share * SB_SqueezeAround(S, LongHere);
					Depth = Pinned > 0.0 ? max(Depth, U - LongHere + Join) : Depth + Join;
				}
			}
			Depth = Pinned > 0.0 ? max(Depth, Pinned) : Depth;
#if SB_HARD_SURFACE
			// 켜진 강체 조각의 발자국(재현 gamma_fine='split'): 깊이 = U − L + w(판 목표 − U) + (1 − w)γhA₁p. 검정(w → 1)은 판 그대로라 그 안 장이
			// 판의 매끈한 면이고 정점이 제 자리를 읽어도 한 몸으로 움직인다(원칙 E07). 손이 누른 칸은 붙들 깊이 아래로 가지 않는다.
			float PlateSpring;
			float PlateAim;
			float PlateShare;
			float PlateLong;
			float PlateSqueeze;
			if (SB_HardSpringAt(S, Where, PlateSpring, PlateAim, PlateShare, PlateLong, PlateSqueeze))
			{
				float PlateDepth = U - PlateLong + PlateShare * (PlateAim - U) + (1.0 - PlateShare) * PlateSqueeze;
				float PinnedHere = bDentListed && SB_TileListed(S) ? DentPinsIn.Load(int3(S, 0)).y : 0.0;
				Depth = PinnedHere > 0.0 ? max(PlateDepth, PinnedHere) : PlateDepth;
			}
			// 판을 그리는 칸(fb/orn 안 A — 떠 있는 장식 칸, 장식 칸이 없는 판은 발자국 칸): 판 깊이를 그린다. 장식 렌더 정점이 이 칸을 읽어 장식이 판째 기울고
			// 들린다. 발자국(장식 밑 셔츠) 칸은 위의 살 값 그대로다 — 붙들 깊이가 판 깊이(> 0인 곳)라 눌린 쪽은 판까지 내려가고(장식 밑면을 뚫지 않는다), 들린
			// 쪽 셔츠는 판을 따라 오르지 않는다(재현 replica_plate.draw_b3: 셔츠 = max(U − L, 판), 장식 = 판).
			float FloatPlate;
			if (SB_HardDrawsPlateCell(S) && SB_HardFloatAt(S, FloatPlate))
			{
				Depth = FloatPlate;
			}
#endif
			// 끌림은 거친 격자를 이 칸 자리(정확한 고스트 칸은 가리키는 점)에서 쌍선형으로 읽어 이 칸 노멀의 접평면에 눕힌다(일지 161).
			// 노멀 패스는 변위에서 노멀 성분만 읽어 깊이를 되살린다.
			float3 Drag = SB_DragAt(Where);
			Drag -= Contact.xyz * dot(Drag, Contact.xyz);
			// 국소 모양(일지 251): 접촉 패스가 광선 출발점에 넣은 것과 같은 값. 머티리얼은 출렁임 모양만 그리므로 이 몫은 변위로 그린다.
			float3 Region = float3(0, 0, 0);
			BRANCH
			if (bLumpRegion != 0u)
			{
				Region = SB_RegionOffsetAt(Where, CarrierPosition.Load(int3(S, 0)).xyz, CarrierLump.Load(int3(S, 0)));
			}
			Out = float4(-Contact.xyz * Depth + Drag + Region, SB_ComposeEdge(S));
		}
		DisplacementOut[T] = Out;
		// 고스트 칸은 덮인 칸 베끼기(아래 GhostApplyCS)가 최종 값으로 견준다 — 여기서 견주면 지난 갱신의 베낀 값과 늘 달라 멈추지 않는다.
		bool bGhostLater = SB_KindAt(T) == SB_KIND_GHOST;
		if (StillChange >= 0.0 && !bGhostLater && any(abs(Out.xyz - PrevDisplacementIn.Load(int3(T, 0)).xyz) > StillChange))
		{
			InterlockedOr(SB_ComposeChanged, 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u && SB_ComposeChanged != 0u)
	{
		InterlockedOr(TileCountOut[SB_TILE_COUNT_STILL], SB_STILL_CHANGED);
	}
}

// --- 고스트 칸은 가리키는 점의 최종 변위를 (일지 329) ---
// 화면: 브라처럼 피부에 딱 붙은 천 밑 피부가 손이 누른 벽에서 천 앞으로 이빨처럼 튀어나왔고, 손이 움직이면 켜졌다 꺼졌다(일지 323·328). 덮인 피부 칸은
// 굽기에서 천 칸을 가리키는 고스트라(일지 210) 함몰·긴 몫·끌림·국소 모양은 가리키는 점에서 읽지만, 합치기의 붙들 깊이와 이음(SB_SqueezeAround)은 제 칸·
// 제 UV 이웃에서 잰다(ADR-0037·일지 323이 고스트를 따로 정하지 않고 끼어든 몫 — 같은 자리 두 층이 최대 0.3~0.5 cm 달랐다). 고스트 = 가리키는 점의 값(ADR-0007 §3.4,
// ADR-0032 옮기기)을 그려지는 변위까지 지키려고, 합치기 뒤에 고스트 칸의 변위를 가리키는 점의 변위로
// 바꾼다 — 그 점 둘레 칸을 머티리얼이 쪼갠 점을 그리는 규칙(가장자리 부호 거리 선에 벽을 세움 — Full은 SB_EdgeWallBlend(일지 359), Basic은
// SB_EdgeSameSideBlend(일지 361))으로 읽어, 천이 그 자리에 그리는 값을 피부도 갖는다. 깊이를 더하거나 빼지 않는다. 변위 A(가장자리 부호 거리·벽 없음 양)는 제 칸 값(같은 자리에서 제 광선이 잰 값) 그대로. 가리키는 네 칸이
// 다시 고스트면 그 칸의 합치기 값을 읽는다(한 단계). 이음매 고스트(차트 테두리)도 같은 규칙을 탄다. 이빨 자체는 이것으로 줄지 않았다(일지 329 — 남은 것은 두 층이
// 엇갈린 격자로 칸 사이를 그리는 몫). 합치기는 칸마다 동시에 돌아 다른 칸의 결과를 읽을 수 없고, 같은 텍스처를 읽으며 쓸 수 없어(D3D11은 RGBA16F를 쓰기
// 묶음으로 읽지 못한다) 모으기 → 덮어쓰기 두 패스와 그 사이 임시 텍스처 하나다. 칸 종류는 1바이트 표(CarrierKind)로 거른다.
RWTexture2D<float4> GhostDisplacementOut;   // 모으기 패스: 고스트 칸의 새 변위(임시, 고스트 칸만 쓴다)
Texture2D<float4> GhostDisplacementIn;      // 덮어쓰기 패스: 위 값
uint bWallGhost;                            // 모으기 패스: 1 = Contact Detail Full(벽을 4x4 칸으로 이어 읽음 — 머티리얼 SB_FullDetailOn과 같은 단계)

/** 칸 좌표 Target(정수 = 칸 가운데) 둘레 칸의 합치기 변위를 머티리얼 쪼갠 점과 같은 규칙으로 섞는다 — Full은 4x4 칸(SB_EdgeWallBlend), Basic은
 *  2x2 칸(SB_EdgeSameSideBlend). 캐리어 밖 칸은 빼고, 가운데 네 칸에 표면·고스트 칸이 하나도 없으면 Own. */
float4 SB_GhostFinalAt(float2 Target, float4 Own)
{
	int2 Base = (int2)floor(Target);
	float2 F = Target - (float2)Base;
	BRANCH
	if (bWallGhost != 0u)
	{
		float4 V[16];
		float Use[16];
		UNROLL
		for (int k = 0; k < 16; ++k)
		{
			int2 C = Base + int2(k & 3, k >> 2) - 1;
			bool bCarried = all(C >= int2(0, 0)) && all(C < int2(Resolution, Resolution)) && SB_IsCarried(SB_CarrierKindAt(C));
			V[k] = bCarried ? DisplacementIn.Load(int3(C, 0)) : float4(0, 0, 0, 0);
			Use[k] = bCarried ? 1.0 : 0.0;
		}
		float Coverage;
		float3 Drawn = SB_EdgeWallBlend(V, Use, F, Coverage);
		return Coverage > 1e-4 ? float4(Drawn, Own.a) : Own;
	}
	float4 B = float4((1.0 - F.x) * (1.0 - F.y), F.x * (1.0 - F.y), (1.0 - F.x) * F.y, F.x * F.y);
	float4 Corner[4];
	UNROLL
	for (int j = 0; j < 4; ++j)
	{
		int2 C = Base + int2(j & 1, j >> 1);
		bool bCarried = all(C >= int2(0, 0)) && all(C < int2(Resolution, Resolution)) && SB_IsCarried(SB_CarrierKindAt(C));
		Corner[j] = bCarried ? DisplacementIn.Load(int3(C, 0)) : float4(0, 0, 0, 0);
		B[j] = bCarried ? B[j] : 0.0;
	}
	float Sum = dot(B, float4(1, 1, 1, 1));
	return Sum > 1e-4 ? float4(SB_EdgeSameSideBlend(Corner, B / Sum), Own.a) : Own;
}

/** 모으기: 접촉 묶음의 고스트 칸마다 가리키는 점의 최종 변위(합치기 결과 DisplacementIn)를 임시 텍스처에. */
[numthreads(8, 8, 1)]
void GhostGatherCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID)
{
	int2 T;
	if (!SB_ListedTexel(GroupId, LocalId, T) || SB_CarrierKindAt(T) != SB_KIND_GHOST)
	{
		return;
	}
	float4 Own = DisplacementIn.Load(int3(T, 0));
	GhostDisplacementOut[T] = SB_GhostFinalAt(CarrierRedirect.Load(int3(T, 0)).xy, Own);
}

/** 덮어쓰기: 고스트 칸만 모은 값으로, 멈춤 표지(일지 203)도 이 값으로 견준다. 묶음 합의 장벽을 모두 지나야 하므로 일찍 돌아가지 않는다(D3D11 X3663). */
[numthreads(8, 8, 1)]
void GhostApplyCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex == 0u)
	{
		SB_ComposeChanged = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	int2 T;
	if (SB_ListedTexel(GroupId, LocalId, T) && SB_CarrierKindAt(T) == SB_KIND_GHOST)
	{
		float4 Out = GhostDisplacementIn.Load(int3(T, 0));
		DisplacementOut[T] = Out;
		if (StillChange >= 0.0 && any(abs(Out.xyz - PrevDisplacementIn.Load(int3(T, 0)).xyz) > StillChange))
		{
			InterlockedOr(SB_ComposeChanged, 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u && SB_ComposeChanged != 0u)
	{
		InterlockedOr(TileCountOut[SB_TILE_COUNT_STILL], SB_STILL_CHANGED);
	}
}

// --- NormalCS ---
RWTexture2D<float4> NormalOut;       // xyz 노멀 기울기 회전(축 × sin, 프레임), w 함몰 어둡기(0 = 그대로)
float OcclusionStrength; // 함몰 어둡기 최대치 (0~1)
/**
 * 붙지 않은 칸의 노멀 차분 폭 = 살층 두께 × 이 값(칸 수로 반올림, 1 ~ 최대 칸 수). 콜라이더에 닿지 않은 겉면에서 가장 좁은
 * 모양은 솟음이 둘레 벽에서 꼭대기로 오르는 비탈이고, 같은 살층 FEM에서 두께의 약 1/4이다(일지 96). 그 1/4 거리에서 잰다.
 */
#define SB_NORMAL_STENCIL_LAYER (1.0 / 16.0)
#define SB_NORMAL_STENCIL_MAX 4

/**
 * 칸 자리의 화면 노멀(ADR-0007 §3.6, 프레임 기준 단위 벡터): 베이크가 CarrierRedirect w에 둔 렌더 정점 노멀 보간(메시 로컬 기본
 * 자세, 팔면체로 펼친 두 값을 12비트씩 묶은 정수 + 1)을 접촉 패스와 같은 스킨·스케일로 옮긴다. 옛 베이크(w = 0)는 false.
 */
bool SB_ScreenNormal(int2 Texel, out float3 Normal)
{
	float3 N;
	if (!SB_DecodeScreenNormal(Texel, N))
	{
		Normal = float3(0, 0, 0);
		return false;
	}
	Normal = float3(0, 0, 0);
	if (bSkinned != 0)
	{
		float4 Index = CarrierSkinIndex.Load(int3(Texel, 0));
		float4 Weight = CarrierSkinWeight.Load(int3(Texel, 0));
		float3 Skinned = float3(0, 0, 0);
		UNROLL
		for (int k = 0; k < 4; ++k)
		{
			if (Weight[k] > 1e-4)
			{
				Skinned += SB_TransformByBone((uint)round(Index[k]), float4(N, 0.0)) * Weight[k];
			}
		}
		N = Skinned;
	}
	N /= SB_NormalScale();
	float LengthSq = dot(N, N);
	if (LengthSq < 1e-12)
	{
		return false;
	}
	Normal = N * rsqrt(LengthSq);
	return true;
}

// 함몰 어둡기(일지 98): 구덩이 바닥에서 하늘이 가려지는 몫. 한 방향에서 테두리가 앙각 α로 솟아 보이면 그 방향의 하늘은
// sin²α만큼 가려진다(코사인 무게). 각도는 이 칸의 눌린 면(접면)에서 잰다 — 기운 면 자체는 하늘을 가리지 않는다. 테두리까지의
// 거리는 구덩이 크기라, 같은 깊이라도 손가락 자국은 어둡고 손바닥 가운데는 거의 어둡지 않다. 높이는 변위만 쓴다(원래 몸의 굴곡이
// 만드는 그늘은 엔진 몫).
#define SB_HORIZON_DIRECTIONS 8
#define SB_HORIZON_SAMPLES 10
/** 방향마다 높이를 읽는 거리(칸 수). 가까운 곳은 촘촘히, 먼 곳은 성기게 읽는다 — 먼 테두리일수록 앙각이 작다. */
static const int SB_HorizonSteps[SB_HORIZON_SAMPLES] = {1, 2, 3, 4, 6, 8, 11, 16, 22, 32};
static const int2 SB_HorizonDirs[SB_HORIZON_DIRECTIONS] = {
	int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1), int2(-1, 0), int2(-1, -1), int2(0, -1), int2(1, -1)};

uint SB_KindInside(int2 Texel)
{
	return all(Texel >= int2(0, 0)) && all(Texel < int2(Resolution, Resolution)) ? SB_KindAt(Texel) : 0u;
}

/** 고스트 칸 Ghost가 가리키는 점이 Axis 한 칸에 따라 옆 차트에서 움직이는 양. 그 칸 앞뒤의 고스트로 잰다. */
bool SB_AcrossAxis(int2 Ghost, int2 Axis, float2 Target, out float2 Step)
{
	Step = float2(0, 0);
	if (SB_KindInside(Ghost + Axis) == SB_KIND_GHOST)
	{
		Step = CarrierRedirect.Load(int3(Ghost + Axis, 0)).xy - Target;
		return true;
	}
	if (SB_KindInside(Ghost - Axis) == SB_KIND_GHOST)
	{
		Step = Target - CarrierRedirect.Load(int3(Ghost - Axis, 0)).xy;
		return true;
	}
	return false;
}

/**
 * 가운데 칸 C에서 Dir 쪽으로 나가며 본 가장 높은 기울기(변위 높이 차 ÷ 거리). 이 차트 끝을 지나면 첫 고스트 줄(이음매 건너편을
 * 가리킨다)에서 옆 차트로 건너가, 고스트 줄이 가리키는 점이 움직이는 방향으로 이어 읽는다 — 이음매를 걸친 구덩이도 양쪽에서 같은
 * 테두리를 본다. 두 번째 차트 끝이나 메시 가장자리(고스트가 없는 끝)에서 멈춘다. 첫 칸의 높이 차·거리는 접면 기울기용으로 돌려준다.
 * 아무것도 못 읽으면 −1e9.
 */
float SB_HorizonSlope(int2 C, int2 Dir, float3 PC, float3 DC, float3 N, out float FirstRise, out float FirstRun)
{
	float Best = -1e9;
	FirstRise = 0.0;
	FirstRun = 0.0;
	int SurfaceSteps = 0;
	bool bAcross = false;
	float2 AcrossStart = float2(0, 0);
	float2 AcrossDir = float2(0, 0);
	int AcrossSteps = 0;
	LOOP
	for (int k = 0; k < SB_HORIZON_SAMPLES; ++k)
	{
		int n = SB_HorizonSteps[k];
		int2 X = C + Dir * n;
		if (!bAcross)
		{
			if (SB_KindInside(X) == SB_KIND_SURFACE)
			{
				SurfaceSteps = n;
			}
			else
			{
				int Edge = 0;
				for (int m = SurfaceSteps + 1; m <= n; ++m)
				{
					uint Kind = SB_KindInside(C + Dir * m);
					if (Kind != SB_KIND_SURFACE)
					{
						Edge = Kind == SB_KIND_GHOST ? m : 0;
						break;
					}
				}
				if (Edge == 0)
				{
					break;
				}
				int2 Ghost = C + Dir * Edge;
				float2 Target = CarrierRedirect.Load(int3(Ghost, 0)).xy;
				float2 StepX = float2(0, 0);
				float2 StepY = float2(0, 0);
				if ((Dir.x != 0 && !SB_AcrossAxis(Ghost, int2(Dir.x, 0), Target, StepX))
					|| (Dir.y != 0 && !SB_AcrossAxis(Ghost, int2(0, Dir.y), Target, StepY)))
				{
					break;
				}
				// 배치는 모든 차트의 칸 밀도가 같으므로 건너편 한 걸음도 칸 한두 개다. 아니면 가리키는 곳이 엉뚱한 것이다.
				float Length = length(StepX + StepY);
				if (Length < 0.25 || Length > 4.0)
				{
					break;
				}
				bAcross = true;
				AcrossStart = Target;
				AcrossDir = StepX + StepY;
				AcrossSteps = Edge;
			}
		}
		if (bAcross)
		{
			X = (int2)round(AcrossStart + AcrossDir * (float)(n - AcrossSteps));
			if (!SB_IsCarried(SB_KindInside(X)))
			{
				break;
			}
		}
		float3 Offset = PositionIn.Load(int3(X, 0)).xyz - PC;
		float Run = max(length(Offset - N * dot(Offset, N)), 1e-3);
		float Rise = dot(DisplacementIn.Load(int3(X, 0)).xyz - DC, N);
		if (k == 0)
		{
			FirstRise = Rise;
			FirstRun = Run;
		}
		Best = max(Best, Rise / Run);
	}
	return Best;
}

/** 칸 Center의 함몰 어둡기(0~1, 세기 곱하기 전). 고스트 칸은 가리키는 점(옆 차트)에서 잰다 — 이음매 양쪽이 같은 값이다. */
float SB_DentOcclusion(int2 Center)
{
	int2 O = Center;
	UNROLL
	for (int Hop = 0; Hop < 2; ++Hop)
	{
		if (SB_KindInside(O) == SB_KIND_GHOST)
		{
			O = (int2)round(CarrierRedirect.Load(int3(O, 0)).xy);
		}
	}
	if (SB_KindInside(O) != SB_KIND_SURFACE)
	{
		return 0.0;
	}
	float3 N = ContactIn.Load(int3(O, 0)).xyz;
	float3 PC = PositionIn.Load(int3(O, 0)).xyz;
	float3 DC = DisplacementIn.Load(int3(O, 0)).xyz;
	float Horizon[SB_HORIZON_DIRECTIONS];
	float Rise[SB_HORIZON_DIRECTIONS];
	float Run[SB_HORIZON_DIRECTIONS];
	UNROLL
	for (int d = 0; d < SB_HORIZON_DIRECTIONS; ++d)
	{
		Horizon[d] = SB_HorizonSlope(O, SB_HorizonDirs[d], PC, DC, N, Rise[d], Run[d]);
	}
	float Sum = 0.0;
	UNROLL
	for (int e = 0; e < SB_HORIZON_DIRECTIONS; ++e)
	{
		if (Horizon[e] < -1e8)
		{
			continue;
		}
		// 접면 기울기: 앞뒤 첫 칸의 높이 차(가운데 차분). 한쪽이 없으면 있는 쪽으로.
		int Back = (e + SB_HORIZON_DIRECTIONS / 2) % SB_HORIZON_DIRECTIONS;
		bool bFront = Run[e] > 0.0;
		bool bBack = Run[Back] > 0.0;
		float Tangent = bFront && bBack ? (Rise[e] - Rise[Back]) / (Run[e] + Run[Back])
			: (bFront ? Rise[e] / Run[e] : (bBack ? -Rise[Back] / Run[Back] : 0.0));
		float Angle = max(atan(Horizon[e]) - atan(Tangent), 0.0);
		float Sine = sin(Angle);
		Sum += Sine * Sine;
	}
	return Sum / (float)SB_HORIZON_DIRECTIONS;
}

/**
 * 칸 S가 콜라이더 면에 선 정도(0~1, 일지 299). 합치기가 붙든 칸을 붙들 깊이 P에 세운 기록을 같은 값으로 다시 읽는다: 그린 깊이(변위의 노멀 성분에서
 * 국소 모양 몫을 뺀 값 — 합치기와 같은 식)가 P와 같으면 1, P보다 깊어질수록 한 칸 안에서 0으로 잦아든다. 변위는 반정밀이라 그 저장 오차만큼은 같다고
 * 본다. 붙들지 않은 칸, 목록 밖 칸, 목록이 서지 않은 갱신은 0이다.
 * 전에는 접촉 패스의 관통 깊이와 견줬다 — 덩어리 칸은 국소 모양 몫, 기억이 남은 칸은 기억 깊이만큼 꺼졌고, 피부 자리 떨림에 칸마다 켜지고 꺼졌다.
 */
float SB_ConformAt(int2 S, float3 N)
{
	if (DentTileCount[0] == 0u || !SB_TileListed(S))
	{
		return 0.0;
	}
	float Pinned = DentPinsIn.Load(int3(S, 0)).y;
	if (!(Pinned > 0.0))
	{
		return 0.0;
	}
	float3 Shown = DisplacementIn.Load(int3(S, 0)).xyz;
	BRANCH
	if (bLumpRegion != 0u)
	{
		float2 Where = SB_KindAt(S) == SB_KIND_GHOST ? CarrierRedirect.Load(int3(S, 0)).xy : (float2)S;
		Shown -= SB_RegionOffsetAt(Where, CarrierPosition.Load(int3(S, 0)).xyz, CarrierLump.Load(int3(S, 0)));
	}
	float Stored = length(Shown) * (1.0 / 1024.0);
	return saturate(1.0 - max(-dot(Shown, N) - Pinned - Stored, 0.0) / max(TexelSize, 1e-4));
}

/**
 * 가장자리 띠 안 칸의 노멀 면 안 방향(일지 327·330).
 * 화면: 깊이 박힌 엄지의 옆 벽을 손을 숨기고 보면 벽을 따라 점이 늘어선 빗살(일지 320·326). 벽이 칸 격자에 비스듬히 지나면 칸 값은 계단이고, 앞뒤 한 칸
 * 차분은 그 계단을 가로·세로로 따로 읽어 칸마다 노멀이 기운 면 안 방향이 흔들린다 — 기운 크기는 거의 안 흔들린다(스텐실을 가로지른 낙차는 계단 위상과 무관).
 * 노멀 텍셀이 담을 값은 렌더러가 그리는 면의 노멀을 칸 넓이로 평균한 것이고, 띠 안에서 렌더러는 가장자리 부호 거리 s = 0 선에 벽을 세우므로(ADR-0029,
 * SB_EdgeSideField) 그 면의 노멀이 기우는 면 안 방향은 그 선에 수직 = ∇s다. 그래서 벽을 세우는 칸(벽 없음 양 k가 작을수록)은 누른 뒤 노멀의 면 안 성분을 ∇s
 * 방향으로 돌리고 기운 각은 그대로 둔다. ∇s는 제 칸이 ±2칸 안에 있을 때(잘리지 않았을 때)만, 스텐실 폭과 상관없이 한 칸 이웃 넷의 s(변위 A의 .w)로 잰다 —
 * s = 0 선의 방향은 국소 성질이라서. 이웃이 양쪽 다 잘리지 않았으면 가운데 차분, 한쪽만이면 다른 쪽 한 칸 차분(제 칸이 안 잘렸으니 그 값은 맞다). 제 칸이 잘린
 * 칸(띠에서 두 칸 넘게 들어간 칸)은 그대로 — 한 칸 차분이 이미 매끈한 면을 읽고, 잘린 칸의 한쪽 차분은 한 축이 0이 되어 방향을 축 쪽으로 치우치게 한다(첫 시험에서
 * 본 것). |∇s|가 작은 곳(두 가장자리 사이 능선)도 그대로. 돌리는 쪽 부호는 지금 방향과 가까운 쪽(벽이 올라가든 내려가든 지금 차분이 아는 것). 거의 안 기운
 * 노멀(sin < 0.06)은 방향이 뜻이 없어 그대로. 재생(일지 330): 엄지 윗벽 빗살 칸 27 → 8, 띠 밖 변화 0.
 * Before·After = 누르기 전·후 단위 노멀, E1·E2 = 스텐실 가로·세로 변(쉬는 자리, 방향만 쓴다), Sc·Kc = 제 칸의 부호 거리(칸)·벽 없음 양, Sn = 한 칸 이웃 넷(+x, −x, +y, −y)의 부호 거리.
 */
float3 SB_EdgeBandDirection(float3 Before, float3 After, float3 E1, float3 E2, float Sc, float Kc, float4 Sn)
{
	const float Clip = SB_EDGE_RANGE - 1.0 / 64.0;
	if (abs(Sc) >= Clip)
	{
		return After;
	}
	bool4 bIn = abs(Sn) < Clip;
	float Gx = bIn.x && bIn.y ? 0.5 * (Sn.x - Sn.y) : (bIn.x ? Sn.x - Sc : (bIn.y ? Sc - Sn.y : 0.0));
	float Gy = bIn.z && bIn.w ? 0.5 * (Sn.z - Sn.w) : (bIn.z ? Sn.z - Sc : (bIn.w ? Sc - Sn.w : 0.0));
	float G = sqrt(Gx * Gx + Gy * Gy);
	float Up = dot(After, Before);
	float3 Side = After - Before * Up;
	float SideLength = length(Side);
	float Trust = (1.0 - saturate(Kc)) * smoothstep(0.3, 0.6, G) * smoothstep(0.02, 0.06, SideLength);
	if (Trust <= 0.0)
	{
		return After;
	}
	float3 Along = Gx * (E1 * rsqrt(max(dot(E1, E1), 1e-12))) + Gy * (E2 * rsqrt(max(dot(E2, E2), 1e-12)));
	Along -= Before * dot(Along, Before);
	float AlongLength = length(Along);
	if (AlongLength < 1e-6)
	{
		return After;
	}
	Along /= AlongLength;
	float Cos = dot(Side, Along) / SideLength;
	// 잰 기울기가 벽과 거의 수직인 칸(엄지 끝처럼 면이 벽을 따라 기운 곳)은 벽이 지배하는 칸이 아니라 돌리지 않는다 — 돌리면 이웃끼리 반대쪽으로
	// 90° 꺾여 검은 조각이 됐다(첫 시험). 벽의 계단 흔들림은 ∇s에서 30° 안이었다(|cos| ≥ 0.88).
	Trust *= smoothstep(0.5, 0.8, abs(Cos));
	float Sign = Cos < 0.0 ? -1.0 : 1.0;
	float3 Turned = normalize(Before * Up + Along * (Sign * SideLength));
	return normalize(lerp(After, Turned, Trust));
}

/** 접촉 묶음 목록만 돈다(ComposeCS와 같은 이유). 이웃은 칸이 있는 곳만 고르므로(SB_NeighborAt) 늘 목록 안이다. */
[numthreads(8, 8, 1)]
void NormalCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID)
{
	int2 T;
	if (!SB_ListedTexel(GroupId, LocalId, T))
	{
		return;
	}
	if (!SB_IsCarried(SB_KindAt(T)))
	{
		NormalOut[T] = float4(0, 0, 0, 0);
		return;
	}
	int2 S = T;
	float4 Contact = ContactIn.Load(int3(S, 0));
	// 차분 폭(일지 67·96): 콜라이더에 붙은 칸(관통이 있는 칸)은 한 칸 — 콜라이더 자국의 날과 가장자리 모양을 그대로 잰다.
	// 붙지 않은 칸(둘레 벽·솟음)은 살층 두께 × SB_NORMAL_STENCIL_LAYER를 칸 수로 바꿔(1~SB_NORMAL_STENCIL_MAX칸) 양옆을 멀리서
	// 잰다. 실제 겉면 모양은 이보다 넓어서 그대로이고(둘레 벽은 함몰 폭 0.12h에서 기울기 오차 5% 안), 한 칸 차분이 그대로
	// 기울기로 키우던 칸 크기의 요철(이완 결과의 칸 단위 들쭉날쭉, 칸 점이 놓인 삼각형의 꺾임)은 폭에 반비례해 준다. 솟음 높이가
	// 제자리 함몰의 세제곱이라 그 요철이 솟음에 세기의 3배로 들어가는 자리에서 특히 완만한 솟음의 음영이 거칠었다. 폭을 함몰
	// 폭에 묶으면(일지 94~95) 함몰이 짧아진 만큼 솟음을 좁게 재서 띠가 또렷해졌다. 읽는 횟수는 같다(같은 네 이웃을 멀리서 읽을 뿐).
	float Layer = SB_LayerAt(S);
	float Conform = SB_ConformAt(S, Contact.xyz);
	int Radius = Contact.w > 0.0 || Conform > 0.0 ? 1 : clamp((int)round(SB_NORMAL_STENCIL_LAYER * Layer / max(TexelSize, 1e-4)), 1, SB_NORMAL_STENCIL_MAX);
	int2 XP = SB_NeighborAt(S, int2(1, 0), Radius);
	int2 XM = SB_NeighborAt(S, int2(-1, 0), Radius);
	int2 YP = SB_NeighborAt(S, int2(0, 1), Radius);
	int2 YM = SB_NeighborAt(S, int2(0, -1), Radius);
	float3 E1 = PositionIn.Load(int3(XP, 0)).xyz - PositionIn.Load(int3(XM, 0)).xyz;
	float3 E2 = PositionIn.Load(int3(YP, 0)).xyz - PositionIn.Load(int3(YM, 0)).xyz;
	float4 DXP = DisplacementIn.Load(int3(XP, 0));
	float4 DXM = DisplacementIn.Load(int3(XM, 0));
	float4 DYP = DisplacementIn.Load(int3(YP, 0));
	float4 DYM = DisplacementIn.Load(int3(YM, 0));
	float3 F1 = E1 + DXP.xyz - DXM.xyz;
	float3 F2 = E2 + DYP.xyz - DYM.xyz;

	// 변위 전후 노멀을 같은 차분으로 구해 둘 사이 회전만 남긴다. 변위가 없으면 정확히 0이다.
	// 차트가 아틀라스에서 뒤집혀 있을 수 있으므로 현재 바깥 노멀 쪽으로 방향을 맞춘다.
	float3 Before = cross(E1, E2);
	float3 After = cross(F1, F2);
	float Orientation = dot(Before, Contact.xyz) < 0.0 ? -1.0 : 1.0;
	float BeforeLength = length(Before);
	float AfterLength = length(After);
	float3 Tilt = float3(0, 0, 0);
	if (BeforeLength > 1e-8 && AfterLength > 1e-8)
	{
		Before *= Orientation / BeforeLength;
		After *= Orientation / AfterLength;
		// 누르기 전 노멀 = 화면 노멀(ADR-0007 §3.6 결정 2). 한 칸 차분으로 구한 누르기 전 노멀은 칸 점이 놓인 면의 굴곡을 갖는데,
		// 회전을 받는 화면 노멀은 정점 노멀을 삼각형 안에서 보간한 매끈한 값이라, 전후 차분만 쓰면 그 굴곡이 음영에 찍힌다.
		//  - 콜라이더 면에 붙은 칸: 누른 뒤 모양이 콜라이더 면이다. 회전 = 화면 노멀 → 한 칸 차분의 누른 뒤 노멀(콜라이더 면)이라
		//    눌린 자리는 콜라이더 모양에 노멀맵만 얹힌다(일지 54).
		//  - 붙지 않은 칸(둘레 벽·솟음·손가락 사이 다리): 한 칸 차분이 잰 변형(이웃 칸 사이 두 변이 어떻게 늘고 기울었는지)을
		//    화면 노멀에 그대로 적용한다. 전후 차분만 쓰면 칸 점 면의 굴곡이 변형 크기에 비례해(깊이 × 굽음) 우글거림으로
		//    남아, 세게 누를수록 둘레가 거칠어졌다(일지 56).
		//  - 둘 사이는 콜라이더 면에 선 정도(SB_ConformAt — 합치기가 붙들 깊이에 세운 칸이 1)로 섞는다.
		// 변위가 없는 칸(누르지 않은 칸, 이웃과 똑같이 옮겨진 칸)은 회전 0 그대로, 화면 노멀이 없는 옛 베이크는 한 칸 차분 그대로다.
		float3 Screen = float3(0, 0, 0);
		bool bMoved = any(F1 != E1) || any(F2 != E2);
		bool bScreen = SB_ScreenNormal(S, Screen);
		// 덩어리가 옮긴 면(ADR-0018): 화면 노멀을 접촉 패스가 구한 덩어리 회전만큼 먼저 돌려 두고, 접촉이 그 면에서 더 기울인 몫만
		// 남긴다. 머티리얼은 덩어리 회전과 이 회전을 차례로 적용한다. 전에는 콜라이더에 붙은 칸이 화면 노멀에서 콜라이더 면까지를
		// 통째로 돌려, 머티리얼이 따로 돌리는 덩어리 기울기가 두 번 들어갔다.
		if (bScreen && LumpMode != 0)
		{
			Screen = normalize(SB_TurnByTilt(LumpNormalIn.Load(int3(S, 0)).xyz, Screen));
		}
		if (bMoved && bScreen && dot(Screen, Before) > 0.0)
		{
			// 변형 J: E1 → F1, E2 → F2, 누르기 전 노멀 → 누른 뒤 노멀. 노멀은 J의 역전치로 옮는다 — (F1, F2, 누른 뒤 노멀)의
			// 쌍대 기저로 풀어 쓴 식이다(크기는 버린다). 화면 노멀이 칸 노멀과 같으면 누른 뒤 노멀 그대로다.
			float3 Deformed = Orientation * (dot(E1, Screen) * cross(F2, After) + dot(E2, Screen) * cross(After, F1)
				+ dot(Before, Screen) * cross(F1, F2));
			float DeformedLength = length(Deformed);
			if (DeformedLength > 1e-8)
			{
				After = normalize(lerp(Deformed / DeformedLength, After, Conform));
				Before = Screen;
			}
		}
		// 가장자리 띠 안 칸은 노멀의 면 안 방향을 벽(s = 0 선)에 수직으로(일지 327·330, SB_EdgeBandDirection).
		float Sc;
		float Kc = SB_UnpackEdge(DisplacementIn.Load(int3(S, 0)).w, Sc);
		BRANCH
		if (abs(Sc) < SB_EDGE_RANGE - 1.0 / 64.0 && Kc < 1.0)
		{
			// 이웃의 s는 스텐실 폭과 상관없이 한 칸 이웃에서(폭이 1이면 이미 읽은 값).
			float4 Sn;
			float4 A1 = Radius == 1 ? float4(DXP.w, DXM.w, DYP.w, DYM.w)
				: float4(DisplacementIn.Load(int3(SB_NeighborAt(S, int2(1, 0), 1), 0)).w, DisplacementIn.Load(int3(SB_NeighborAt(S, int2(-1, 0), 1), 0)).w,
					DisplacementIn.Load(int3(SB_NeighborAt(S, int2(0, 1), 1), 0)).w, DisplacementIn.Load(int3(SB_NeighborAt(S, int2(0, -1), 1), 0)).w);
			SB_UnpackEdgeK(A1, Sn);
			After = SB_EdgeBandDirection(Before, After, E1, E2, Sc, Kc, Sn);
		}
		Tilt = cross(Before, After);
		if (dot(Before, After) < 0.0)
		{
			// 90°를 넘는 꺾임은 90°로 자른다(머티리얼은 cos을 sin에서 복원한다).
			Tilt *= rsqrt(max(dot(Tilt, Tilt), 1e-12));
		}
	}

	// 함몰 어둡기(SB_DentOcclusion): 구덩이 테두리가 가리는 하늘. 전에는 테두리까지 거리를 함몰 벽 폭 하나로 두어(구덩이 크기와
	// 상관없이 0.12h) 얕게 남은 자국도 진하게 어두웠다(일지 97). 솟음을 얹기 전 함몰(이완 결과)이 있는 칸만 잰다. 그 꼬리 끝은
	// 솟음 바깥쪽 완만한 곳이라 어둡기가 이미 0이다. 최종 깊이로 가르면 솟음이 시작하는 선(깊이 0)에서 어둡기가 한 칸에 끊겨
	// 솟음 테두리가 선으로 보였고, 솟음이 셀수록 또렷했다(일지 99).
	// 함몰이 있는 칸 = 합치기와 같은 값(목록 밖은 거친 해, 일지 198·199 — 상태 A로 가르면 어둡기가 묶음 경계에서 잘려 네모가 음영으로 남는다).
	float Occlusion = OcclusionStrength > 0.0 && SB_ShownDent(S) > SB_REST_EPSILON
		? saturate(OcclusionStrength) * SB_DentOcclusion(S) : 0.0;
	NormalOut[T] = float4(Tilt, Occlusion);
}

// --- 덩어리 평형 (ADR-0012, ADR-0017, 국소 모양 일지 251) ---
// 덩어리(뿌리 스프링 C, 관성 목표 T)와 손 사이 살이 같이 버티는 자리를 GPU에서 뉴턴으로 푼다. 미지수 x = (q, E): 국소 모양(SB_REGION_MODES개,
// 굽기 SoftbodyLumpRegion)의 크기 q와 출렁임 모양의 크기 E(기본 자세 축 하중, g). 에너지
//   Ψ(x) = ½ Σ Λ_m q_m² + ½(E − T)ᵀC(E − T) + s/A² · Σ_칸 [K·W(d) + 붙은 피부 몫],   d = d0 + A·(r·q + g·E)
// Λ = 국소 모양의 단단함(굽기), g·r = 출렁임 모양(무게 × 노멀 몫 Lump.rgb와 옆 몫 §3.5)·국소 모양(SB_RegionShapes)이 칸을 미는 방향(누르는
// 콜라이더 면의 노멀, 일지 317)으로 옮기는 성분, A = 흔들림
// 크기, K = 칸 강성(ADR-0036), s = 살 단단함 × 칸 넓이(CPU), d0 = 덩어리가 안 움직였다면 누른 깊이(접촉 패스 LumpDepthOut의 d0 — 국소 모양이 옮긴
// 양도 뺀 값), W = 살의 힘 법칙 φ의 적분(SB_TissueLaw). 칸마다 v = (r, g)(미는 방향 쪽),
// v1·v2 = 접선 두 축(SB_LumpBasis) 쪽이면
//   ∇Ψ = (Λq, C(E − T)) + s/A · R,   ∇²Ψ = blockdiag(Λ, C) + s · J
//   R = Σ K φ(d) v + 붙은 칸 접선 몫,   J = Σ K φ'(d) v vᵀ + 붙은 칸 접선 2×2 몫
// 붙은 칸(§7)은 손이 끈 거리 Ĝ와 덩어리 옆 이동의 차 Δ = A·(v1·x, v2·x) − Ĝ만큼 피부 밑 살층이 곧은 스프링으로 당기고(K × LumpGrip × Hold × Δ,
// Hold = 한도 힘 ÷ 한도 거리 — 일지 254), 미끄러지는 칸(§8)은 한도 힘만큼 닻 쪽으로 미는 일정한 힘이다(아래). 붙음·미끄러짐(일지 201): 이 패스가
// 걸음마다 지금 x에서 판정한다 — Δ의 닻 Ĝ는 첫 걸음에 이번 출발점에서 한도 안으로 잘라 LumpAnchorOut에 남긴 값, 한도 = φ⁻¹(DragRatio·φ(d̄)),
// d̄ = 살 두께에 붙인 d(보이는 끌림과 같은 누르는 힘, 일지 336). 평형에서 안 눌린 칸(d ≤ 0)은 한도 힘이 0이라 쥐지 않는다.
// |Δ| ≤ 한도면 붙은 칸, 넘으면 힘 크기 Kt·DragRatio·φ(d̄)로 닻 쪽 — 한도에서 붙은 칸 힘과 이어지고 크기가 x에 기댄다. J는 붙은 칸 스프링과 누름만이다 —
// Hold와 미끄러지는 힘이 d를 거쳐 x에 기대는 몫은 넣지 않는다(장난감에서 되돌림이 잔차면 충분했다, Tools/lump_friction_sim.py·일지 254).
// S1·S2 = 구운 옆 몫(§3.5) × 무게.
// 국소 모양이 없으면(옛 굽기, 줄 없는 칸, bLumpRegion 0) r = 0이라 q는 0에 머물고 E는 전(미지수 셋)과 같다.
// LumpForceCS 한 번 = 칸마다 v·v1·v2와 계수를 공유 메모리에 적고, 값(기울기 19, 헤세 위 삼각 190, 에너지 1)마다 스레드가 그 묶음의 눌린 칸들을
// 더해 덩어리 묶음마다 전역 버퍼에 부호 있는 64비트(아래·위 32비트, 올림수 처리) 고정소수점(1 = 1/SB_LUMP_SCALE)으로 더한다 — 몸 전체를 눌러도
// 넘치지 않는다. 칸마다 값 210개를 같은 주소에 원자로 더하면 묶음 안 64칸이 한 주소에 몰려 줄을 선다(일지 169·170). 한 걸음 풀이는 LumpSolveCS.
#define SB_LUMP_SCALE 4096.0
/** 뼈에 얇게 덮인 곳도 힘이 끝없이 커지지 않게 두께 하한(cm). */
#define SB_LUMP_MIN_THICKNESS 0.5
/**
 * 덩어리 평형 미지수 수 = 국소 모양 크기 q(SB_LUMP_MODES) + 출렁임 모양 크기 E(3). x = (q, E) 순서 — LumpSolveCS가 q를 먼저 지워 E의 헤세가 남는다.
 * 국소 모양이 구워져 켜진 장만 모양 SB_REGION_MODES개를 같이 푼다(덩어리 패스 순열 SB_LUMP_REGION 1). 아닌 장은 E 셋만 풀어 칸 합 값이
 * 210 → 10개, 풀이가 19×19 → 3×3이다(일지 254 — 국소 모양이 없는 자산이 그 비용을 내지 않게). 같은 식이라 국소 모양이 없을 때 두 판의 결과가 같다.
 * 순열이 없는 커널(덩어리 패스 밖)은 국소 모양이 있는 판의 크기를 본다(상태 줄 배치는 두 판이 같다).
 */
#ifndef SB_LUMP_REGION
#define SB_LUMP_REGION 1
#endif
#if SB_LUMP_REGION
#define SB_LUMP_MODES SB_REGION_MODES
#else
#define SB_LUMP_MODES 0
#endif
#define SB_LUMP_DIM (SB_LUMP_MODES + 3)
#define SB_LUMP_E SB_LUMP_MODES
/** 헤세 위 삼각 값 수(SB_LUMP_DIM·(SB_LUMP_DIM + 1)/2)와 묶음마다 칸 합 값 수(기울기 + 헤세 + 에너지 — 큰 판이 CPU NumLumpValues). */
#define SB_LUMP_HESS (SB_LUMP_DIM * (SB_LUMP_DIM + 1) / 2)
#define SB_LUMP_VALUES (SB_LUMP_DIM + SB_LUMP_HESS + 1)
#define SB_LUMP_VALUE_ENERGY (SB_LUMP_VALUES - 1)
/** 덩어리 묶음마다 상수 줄 수(LumpGroups, CPU NumLumpGroupRows)·결과 값 수(LumpOut, CPU NumLumpOutputs). 반복 상태 줄 수 SB_LUMP_STATE_ROWS는 쥐기 노멀과 같이 앞에. */
#define SB_LUMP_GROUP_ROWS 7
#define SB_LUMP_OUTPUTS 16
/** LumpForceCS가 칸마다 공유 메모리에 적는 값 수: v·v1·v2 SB_LUMP_DIM개씩(큰 판 19), 계수 8(SB_LUMP_ROW_* 자리), 상수 1·0. */
#define SB_LUMP_ROW_FLOATS (3 * SB_LUMP_DIM + 10)
#define SB_LUMP_ROW_WN (3 * SB_LUMP_DIM)          // K φ'(d) — 노멀 헤세 몫
#define SB_LUMP_ROW_GN (3 * SB_LUMP_DIM + 1)      // K φ(d) — 노멀 기울기 몫
#define SB_LUMP_ROW_MXX (3 * SB_LUMP_DIM + 2)     // 붙은 칸 접선 2×2 헤세 몫(× Kt)
#define SB_LUMP_ROW_MXY (3 * SB_LUMP_DIM + 3)
#define SB_LUMP_ROW_MYY (3 * SB_LUMP_DIM + 4)
#define SB_LUMP_ROW_G1 (3 * SB_LUMP_DIM + 5)      // 접선 두 축 기울기 몫
#define SB_LUMP_ROW_G2 (3 * SB_LUMP_DIM + 6)
#define SB_LUMP_ROW_W (3 * SB_LUMP_DIM + 7)       // 에너지 몫("힘이 있었다" 표지)
#define SB_LUMP_ROW_ONE (3 * SB_LUMP_DIM + 8)     // 1 — 더하기가 기울기·헤세·에너지를 한 식으로 더하게(LumpForceCS)
#define SB_LUMP_ROW_ZERO (3 * SB_LUMP_DIM + 9)    // 0
Texture2D<uint4> LumpDepthIn;           // ContactCS의 LumpDepthOut(SB_UnpackLumpDepth)
float LumpGrip;                         // 붙은 피부가 덩어리를 당기는 단단함(누르는 살 대비 배수, 0 = 끔). LumpStiffen(§7.1)은 접촉 패스와 같은 선언
uint bFirstStep;                        // 1 = 이번 프레임 첫 걸음: 손 닻을 이번 뉴턴 출발점에서 마찰 한도 안으로 잘라 LumpAnchorOut에 남긴다
Texture2D<float2> LumpAnchorIn;         // 첫 걸음이 잘라 둔 손 닻(둘째 걸음부터 읽는다). 첫 걸음에는 한 칸짜리 빈 텍스처가 묶인다
RWTexture2D<float2> LumpAnchorOut;      // 첫 걸음이 쓰는 손 닻(갱신을 넘어 남는다 — 접촉 패스의 LumpAnchor). 둘째 걸음부터는 한 칸짜리 빈 텍스처
// 묶음마다 SB_LUMP_GROUP_ROWS줄: (C00 C01 C02 s), (C11 C12 C22 A), (T, 1 = 푼다), 국소 모양 단단함 Λ(넷씩 4줄 — 모양이 없는 자리는 1).
// 기본 자세 축, CPU가 프레임마다.
StructuredBuffer<float4> LumpGroups;
// LumpIterate(LumpState를 읽기로 — 줄 0의 xyz가 이번에 잴 E, 줄 6~9가 q, 국소 모양 패스는 지난 결과(줄 2·14~29))는 접촉 패스도 읽어 앞(덩어리 절)에 둔다.
RWBuffer<uint> LumpSumsOut;           // 묶음마다 값 SB_LUMP_VALUES개 × 64비트(아래·위 32비트). LumpStartCS가 비우고 LumpSolveCS가 읽은 뒤 비운다
Buffer<uint> LumpList;                  // ContactGroupsCS의 덩어리 힘 묶음 목록 (x | y << 16)
Buffer<uint> LumpListCount;             // [0] 목록 길이
Buffer<uint> PresserTable;              // 접촉 패스의 누른 것 표(ActivityOut, SB_PRESSER_TABLE_BASE부터 — 일지 248 ②·338)
groupshared float SB_LumpRows[64 * SB_LUMP_ROW_FLOATS];   // 힘을 받는 칸의 줄, 받은 차례대로 빈틈없이
groupshared uint SB_LumpRowGroup[64];
groupshared uint SB_LumpActiveCount;
groupshared uint SB_LumpGroupMask;

/** 이 칸을 누른 것(PositionIn.w, SB_PackTouch) × 덩어리 묶음의 누른 것 표 자리. */
uint SB_PresserIndex(float Touch, uint Group)
{
	float Presser = round(Touch - 1.0);
	uint Winner = Presser >= 1001.0 ? (uint)(Presser - 1001.0) : NumShapes + (uint)max(Presser - 1.0, 0.0);
	return SB_PRESSER_TABLE_BASE + (Winner * SB_LUMP_GROUPS + Group) * SB_PRESSER_WORDS;
}

/**
 * 손 밑 살이 덩어리를 미는 칸 강성(일지 248 ②, ADR-0036). 누른 것 하나는 그것과 같은 크기의 접촉 하나만큼 민다: 닿은 크기 a = min(그것이 누른
 * 덩어리 칸 넓이로 잰 반지름 a_넓이, 그것의 가로 크기 a_pr), 강성 = 넓이 πa² / min(두께, SB_CONTACT_REACH × a). 눌린 칸들이 이 몫을 나눠 가져
 * 칸마다 (a / a_넓이)² / min(두께, SB_CONTACT_REACH × a)다.
 *  - 깊이 묻힌 손가락은 볼록한 살에서 안쪽 광선이 모여 손가락보다 넓은 주머니를 눌린 칸으로 센다(시험 캐릭터 가슴 앞, 반지름 1 cm를 3.5 cm:
 *    a_넓이 1.94 cm). 주머니 칸마다 1/두께로 밀면 손가락이 손바닥처럼 덩어리를 밀었다.
 *  - 손끝이 두꺼운 살을 누르면 두께가 아니라 a가 단단함을 정한다(반무한체). 넓은 접촉·얇은 살은 지금과 같다(1/두께).
 * 칸 수를 못 찾으면(표 밖) 지금 법칙이다.
 */
float SB_LumpPressStiffness(float Touch, uint Group, float H)
{
	uint Index = SB_PresserIndex(Touch, Group);
	uint Count = PresserTable[Index];
	if (Count == 0u)
	{
		return 1.0 / H;
	}
	float Area = TexelSize * sqrt((float)Count / PI);
	float Reach = min(Area, asfloat(~PresserTable[Index + 1u]));
	float Share = Reach / Area;
	return Share * Share / max(min(H, SB_CONTACT_REACH * Reach), SB_LUMP_MIN_THICKNESS);
}

/**
 * 손 마찰 면이 피부 접평면에서 기울 수 있는 한도(cos, 75°). 닻을 피부 접평면 두 성분으로 두므로(LumpAnchor) 면이 피부에 거의 서면 되살리는 셈(SB_GripLift의
 * 1/cos)이 커진다 — 4배까지만. 손가락 옆면이 피부를 스치듯 미는 가장자리 칸에서만 걸린다.
 */
#define SB_GRIP_FACE_MIN_COS 0.25

/**
 * 손 마찰(덩어리 쥐기)을 재는 면의 노멀(덩어리 기본 자세 축, 피부 바깥 쪽 — 일지 338). 마찰은 이 면을 따라 미끄러지는 움직임만 붙든다.
 * 이 칸을 누른 것(손 부위 — 손가락 하나, 손바닥) × 덩어리 묶음이 이번 갱신에 누른 칸들의 미는 방향 평균 m(누른 것 표, 칸 수 무게 — 길이 = 방향이 모인
 * 정도 c)에 이 칸 피부 노멀을 1 − c만큼 섞은 방향. 손 부위 하나는 한쪽에서 누르므로(0638에서 c 0.75~1) 그 부위가 미는 움직임을 붙들지 않는다 — 피부
 * 접평면으로 재면 비탈을 누르는 손가락의 누름 절반쯤을 미끄러짐으로 붙들어 손가락이 가슴을 옆으로 쟁기질했고(일지 321·334), 칸마다 미는 방향으로 재면
 * 깊이 든 손가락 옆면이 손의 옆 이동을 누름으로 빼 끌림이 사라졌다(일지 322). 면은 같은 갱신 접촉 패스가 쌓은 값이라 늦지 않고, 무게가 칸 수라 덩어리가
 * 늦게 보이는 자리에 기대는 몫은 눌린 칸 집합뿐이다(334는 겹침 깊이로 매겼다). 표에 이 칸을 누른 것이 없으면 피부 노멀.
 * 피부 접평면에서 SB_GRIP_FACE_MIN_COS보다 기운 면은 그 각으로 눕힌다.
 */
float3 SB_GripNormal(float Touch, uint Group, float3 RestN)
{
	float3 G = RestN;
	uint Index = SB_PresserIndex(Touch, Group);
	uint Count = PresserTable[Index];
	if (Count > 0u)
	{
		float3 M = float3((float)asint(PresserTable[Index + 2u]), (float)asint(PresserTable[Index + 3u]), (float)asint(PresserTable[Index + 4u]))
			/ (SB_GRIP_DIR_SCALE * (float)Count);
		G = M + (1.0 - saturate(length(M))) * RestN;
		G = dot(G, G) > 1e-6 ? normalize(G) : RestN;
	}
	float Cos = dot(G, RestN);
	if (Cos < SB_GRIP_FACE_MIN_COS)
	{
		float3 Side = G - RestN * Cos;
		float Length = length(Side);
		G = Length > 1e-4 ? RestN * SB_GRIP_FACE_MIN_COS + Side * (sqrt(1.0 - SB_GRIP_FACE_MIN_COS * SB_GRIP_FACE_MIN_COS) / Length) : RestN;
	}
	return G;
}

/**
 * 마찰 면 Face 위의 두 축(덩어리 기본 자세 축). 피부 축 T1(SB_LumpBasis(RestN))을 그 면에 눕혀 만든다 — 면이 조금 기울면 축도 조금만 돈다. 면이 피부 접평면에
 * 거의 서 있어(T1이 Face와 나란) 눕힐 수 없을 때만 T2에서 만든다. 축은 한 갱신 안에서만 쓴다(닻은 피부 접평면 성분으로 남는다 — SB_GripLift).
 */
void SB_GripAxes(float3 RestN, float3 Face, out float3 A1, out float3 A2)
{
	SB_LumpBasis(RestN, A1, A2);
	float3 U = A1 - Face * dot(A1, Face);
	if (dot(U, U) > 1e-3)
	{
		A1 = normalize(U);
		A2 = cross(Face, A1);
	}
	else
	{
		float3 V = A2 - Face * dot(A2, Face);
		A2 = normalize(V);
		A1 = cross(A2, Face);
	}
}

/**
 * 피부 접평면 두 성분으로 둔 손 닻(LumpAnchor — 축 T1·T2 = SB_LumpBasis(RestN))을 마찰 면 Face 위로 되살린 점(덩어리 기본 자세 축). 피부 노멀을 따라 면까지
 * 옮긴다: 면 위의 점을 피부 접평면에 비춘 두 성분은 다시 그 점으로 돌아오고(면이 같으면 닻이 갱신을 넘어 그대로), 면이 돌면 피부 위 자리를 지킨 채 새 면으로
 * 간다. 면은 피부 접평면에서 75° 안이라(SB_GripNormal) 나눗셈이 4배를 넘지 않는다.
 */
float3 SB_GripLift(float2 Anchor, float3 RestN, float3 T1, float3 T2, float3 Face)
{
	float3 OnSkin = T1 * Anchor.x + T2 * Anchor.y;
	return OnSkin - RestN * (dot(OnSkin, Face) / dot(RestN, Face));
}

void SB_AddLump64(uint Index, int Value)
{
	uint Low = asuint(Value);
	uint Old;
	InterlockedAdd(LumpSumsOut[Index * 2], Low, Old);
	uint High = (Value < 0 ? 0xffffffffu : 0u) + (Old + Low < Old ? 1u : 0u);
	if (High != 0u)
	{
		InterlockedAdd(LumpSumsOut[Index * 2 + 1], High);
	}
}

/** 위 삼각 번호에서 행 Row의 첫 번호(그 앞 행들의 칸 수 Row(2n + 1 − Row)/2, n = SB_LUMP_DIM) — 행 Row부터 끝까지가 이어진 번호다. */
uint SB_LumpPairStart(uint Row)
{
	return Row * SB_LUMP_DIM - (Row * (Row - 1u)) / 2u;
}

/**
 * 헤세 위 삼각 번호 Hess(0 ~ SB_LUMP_HESS − 1, 행 우선)의 행 A·열 B(A ≤ B). 행은 SB_LumpPairStart(A) ≤ Hess인 가장 큰 A — 근의 공식으로 셈하고
 * 제곱근 어림을 한 칸씩 고친다(0~189 모두 맞음, 제곱근이 2 ulp 틀려도). 번호표(상수 배열)는 스레드마다 다른 자리를 읽으면 GPU가 차례로 읽어
 * 풀이가 오히려 느려졌다(일지 251). 번호를 범위 안으로 자르는 것은 FXC가 고리를 미리 셈할 때 범위 밖 번호를 만들기 때문이다.
 */
void SB_LumpHessPair(uint Hess, out uint A, out uint B)
{
	uint Index = min(Hess, SB_LUMP_HESS - 1u);
	float Span = 2.0 * SB_LUMP_DIM + 1.0;
	uint Row = (uint)max((Span - sqrt(max(Span * Span - 8.0 * (float)Index, 0.0))) * 0.5, 0.0);
	Row = SB_LumpPairStart(Row + 1u) <= Index ? Row + 1u : Row;
	Row = SB_LumpPairStart(Row) > Index ? Row - 1u : Row;
	A = Row;
	B = Row + (Index - SB_LumpPairStart(Row));
}

// --- 덩어리 힘 묶음 고르기 (일지 251) ---
// 덩어리 칸이 든 묶음(LumpList — 구운 값으로만 정해진다) 가운데 이번 프레임 덩어리 평형에 힘을 보탤 칸(손 밑 누름·쥐기)이 있는 묶음만 고른다.
// 누름(LumpDepthIn)과 쥐기(PositionIn.w)는 접촉 패스가 정해 뉴턴 걸음 사이에 바뀌지 않는다 — 프레임에 한 번 골라 두면 걸음마다 덩어리
// 칸 묶음 전체를 돌지 않는다.
RWBuffer<uint> LumpActiveTilesOut;      // 고른 묶음 (x | y << 16)
RWBuffer<uint> LumpActiveCountOut;      // [0] 그 수 (0으로 비운 뒤 센다)
Buffer<uint> LumpActiveTiles;
Buffer<uint> LumpActiveCount;
groupshared uint SB_LumpTileForce;

/**
 * 칸 T가 이번 프레임 덩어리 평형에 드는가: 표면 칸, 손 밑 누름(덩어리가 안 움직였다면의 깊이가 있다) 또는 쥐기, 덩어리 무게, 푸는 덩어리 묶음.
 * LumpActiveCS(묶음 고르기)와 LumpForceCS(걸음마다)가 같은 조건을 쓴다.
 */
bool SB_LumpForceTexel(int2 T, out float4 Pressed, out float2 Anchor, out float3 Hand, out float4 Lump, out float Touch, out bool bNear, out bool bGrip,
	out uint Group, out bool bGrab)
{
	// Pressed: r d0, a 미는 방향(SB_PackPushDir) — 일지 338 전 텍스처의 두 채널 자리 그대로. Anchor·Hand: 손 닻 두 몫(LumpDepthOut 주석).
	float RestDepth;
	float PushCode;
	SB_UnpackLumpDepth(LumpDepthIn.Load(int3(T, 0)), RestDepth, Anchor, Hand, PushCode);
	Pressed = float4(RestDepth, 0.0, 0.0, PushCode);
	Lump = CarrierLump.Load(int3(T, 0));
	Touch = PositionIn.Load(int3(T, 0)).w;
	Group = (uint)max(floor(Lump.a), 0.0);
	bNear = Pressed.r > 0.5 * SB_LUMP_FAR;
	bGrip = SB_TouchPressed(Touch) && DragRatio > 0.0 && bLumpSide != 0u && LumpGrip > 0.0;
	// 잡기(ADR-0041): 접촉 패스가 원 안 덩어리 칸의 닻 자리에 표지(1)를 적었다 — 누르지 않은 칸(닻이 비어 있다)만.
	bGrab = !SB_TouchPressed(Touch) && Anchor.y > 0.5 && bLumpSide != 0u && LumpGrip > 0.0;
	return SB_KindAt(T) == SB_KIND_SURFACE && (bNear || bGrip || bGrab) && SB_LumpWeight(Lump) > 0.0 && Group < SB_LUMP_GROUPS
		&& LumpGroups[Group * SB_LUMP_GROUP_ROWS + 2].w > 0.0;
}

/** 덩어리 칸 묶음 목록(ADR-0013 §8)을 돌며 덩어리 평형에 드는 칸이 하나라도 있는 묶음을 고른다. 프레임에 한 번. */
[numthreads(8, 8, 1)]
void LumpActiveCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex == 0u)
	{
		SB_LumpTileForce = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	// 모든 스레드가 장벽을 지나야 하므로 일찍 돌아가지 않는다.
	uint Entry = GroupId.y * SB_CONTACT_ROW + GroupId.x;
	bool bListed = Entry < LumpListCount[0];
	uint Packed = bListed ? LumpList[Entry] : 0u;
	int2 T = int2(SB_GroupOrigin(Packed) + LocalId.xy);
	float4 Pressed;
	float2 Anchor;
	float3 Hand;
	float4 Lump;
	float Touch;
	bool bNear;
	bool bGrip;
	uint Group;
	bool bGrab;
	if (bListed && all(T < int2(Resolution, Resolution)) && SB_LumpForceTexel(T, Pressed, Anchor, Hand, Lump, Touch, bNear, bGrip, Group, bGrab))
	{
		InterlockedOr(SB_LumpTileForce, 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u && SB_LumpTileForce != 0u)
	{
		uint Slot;
		InterlockedAdd(LumpActiveCountOut[0], 1u, Slot);
		LumpActiveTilesOut[Slot] = Packed;
	}
}

/**
 * 덩어리 힘 패스는 고른 묶음(8×8칸) 하나를 스레드 묶음 SB_LUMP_FORCE_PARTS개가 나눠 맡는다(디스패치 z): 저마다 칸마다의 몫(줄)을 다 만들고,
 * 칸 합 값을 나눠 값마다 스레드 하나가 눌린 칸들을 더한다. 묶음 하나가 값 210개를 다 더하면 그 묶음이 도는 SM 하나의 공유 메모리 읽기가
 * 걸음 시간을 정했다(일지 251) — 줄을 만드는 텍스처 읽기는 넷이 겹쳐 읽어도 캐시에서 나온다. 국소 모양이 없는 판은 값이 10개라 나누지 않는다.
 */
#if SB_LUMP_MODES > 0
#define SB_LUMP_FORCE_PARTS 4u
#else
#define SB_LUMP_FORCE_PARTS 1u
#endif
#define SB_LUMP_PART_VALUES ((SB_LUMP_VALUES + SB_LUMP_FORCE_PARTS - 1u) / SB_LUMP_FORCE_PARTS)

/** LumpActiveCS가 고른 묶음(이번 프레임 덩어리 평형에 드는 칸이 있는 8×8 묶음)만 돈다 — 다른 칸은 더할 몫이 없다. */
[numthreads(8, 8, 1)]
void LumpForceCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex == 0u)
	{
		SB_LumpActiveCount = 0u;
		SB_LumpGroupMask = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	// 목록 항목은 스레드 묶음마다 하나다. 모든 스레드가 장벽을 지나야 하므로 일찍 돌아가지 않는다.
	uint Entry = GroupId.y * SB_CONTACT_ROW + GroupId.x;
	bool bListed = Entry < LumpActiveCount[0];
	int2 T = int2((bListed ? SB_GroupOrigin(LumpActiveTiles[Entry]) : uint2(0, 0)) + LocalId.xy);
	bool bActive = false;
	uint Group = 0u;
	float4 Pressed;
	float2 SkinAnchor;
	float3 HandMove;
	float4 Lump;
	float Touch;
	bool bNear;
	bool bGrip;
	bool bGrab;
	if (bListed && all(T < int2(Resolution, Resolution)) && SB_LumpForceTexel(T, Pressed, SkinAnchor, HandMove, Lump, Touch, bNear, bGrip, Group, bGrab))
	{
		float Weight = SB_LumpWeight(Lump);
		{
			float4 NormalData = CarrierNormal.Load(int3(T, 0));
			float H = max(NormalData.w * ThicknessScale, SB_LUMP_MIN_THICKNESS);
			// 손 밑 살의 칸 강성은 닿은 크기로(일지 248 ②, ADR-0036) — 누르는 힘과 쥐기가 같은 값을 쓴다. 그래서 Lump Grip은 늘 누르는 살 대비
			// 배수이고, Drag Friction은 칸마다 미끄럼 힘 ÷ 누르는 힘 그대로다. 보이게 눌리지 않은 칸(덩어리가 비켜나 손이 떨어진 가장자리 — 쥐지
			// 않는다)은 누른 것을 모른다 — 1/두께.
			float K = (SB_TouchPressed(Touch) ? SB_LumpPressStiffness(Touch, Group, H) : 1.0 / H) * SB_LumpFleshStiffness(T);
			float A = LumpGroups[Group * SB_LUMP_GROUP_ROWS + 1].w;
			uint StateRow = Group * SB_LUMP_STATE_ROWS;
			float3 E = LumpIterate[StateRow].xyz;
#if SB_LUMP_MODES > 0
			float4 Qs[4] = {LumpIterate[StateRow + 6], LumpIterate[StateRow + 7], LumpIterate[StateRow + 8], LumpIterate[StateRow + 9]};
#endif
			float3 G = Lump.rgb * Weight;
			float3 RestN = normalize(NormalData.xyz);
			float3 T1;
			float3 T2;
			SB_LumpBasis(RestN, T1, T2);
			// 미는 방향(일지 317): 접촉 패스가 잰 누르는 콜라이더 면의 노멀(덩어리 기본 자세 축). 미는 깊이 d가 덩어리 이동의 이 방향 몫으로 바뀐다 —
			// 출렁임 모양은 노멀 몫과 옆 몫(§3.5)으로, 국소 모양은 세 성분으로 잰다(bNear가 아닌 칸은 정지 노멀).
			bool bFace = bNear;
			float3 Away = bFace ? SB_UnpackPushDir(Pressed.a) : RestN;
			float3 S1 = float3(0, 0, 0);
			float3 S2 = float3(0, 0, 0);
			if (bGrip || bFace || bGrab)
			{
				float4 SideA = CarrierLumpSideA.Load(int3(T, 0));
				S1 = Weight * SideA.xyz;
				S2 = Weight * float3(SideA.w, CarrierLumpSideB.Load(int3(T, 0)).xy);
			}
			float3 GPush = bFace ? dot(Away, RestN) * G + dot(Away, T1) * S1 + dot(Away, T2) * S2 : G;
			// 손 마찰을 재는 면(SB_GripNormal — 이번 갱신 이 칸을 누른 손 부위의 미는 방향 평균, 일지 338)과 그 위 두 축: 덩어리 이동(노멀 몫 G·RestN + 옆 몫
			// S1·T1 + S2·T2)을 그 축으로 다시 잰다.
			float3 GripN = RestN;
			float3 A1 = T1;
			float3 A2 = T2;
			if (bGrip)
			{
				GripN = SB_GripNormal(Touch, Group, RestN);
				SB_GripAxes(RestN, GripN, A1, A2);
				float3 V1 = dot(RestN, A1) * G + dot(T1, A1) * S1 + dot(T2, A1) * S2;
				float3 V2 = dot(RestN, A2) * G + dot(T1, A2) * S1 + dot(T2, A2) * S2;
				S1 = V1;
				S2 = V2;
			}
#if SB_LUMP_MODES > 0
			// 국소 모양(일지 251): 이 칸에서 모양마다 미는 방향·접선 두 축 성분. 줄이 없는 칸은 0이라 q가 끼지 않는다.
			float4 Rn[4] = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};
			float4 R1[4] = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};
			float4 R2[4] = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};
			float4 Shape[12];
			if (SB_RegionShapes(T, CarrierPosition.Load(int3(T, 0)).xyz, Lump, Shape))
			{
				SB_RegionAlong(Shape, Away, Rn);
				SB_RegionAlong(Shape, A1, R1);
				SB_RegionAlong(Shape, A2, R2);
			}
			float RegionN = dot(Rn[0], Qs[0]) + dot(Rn[1], Qs[1]) + dot(Rn[2], Qs[2]) + dot(Rn[3], Qs[3]);
#else
			// 국소 모양이 없는 판(일지 254): q가 없다.
			float RegionN = 0.0;
#endif
			float Wn = 0.0;
			float Gn = 0.0;
			float Mxx = 0.0;
			float Mxy = 0.0;
			float Myy = 0.0;
			float G1 = 0.0;
			float G2 = 0.0;
			float Energy = 0.0;
			float D = bNear ? Pressed.r + A * (dot(GPush, E) + RegionN) : -1.0;
			if (D > 0.0)
			{
				float Phi;
				float Slope;
				float PressEnergy;
				SB_TissueLaw(D, H, LumpStiffen, Phi, Slope, PressEnergy);
				Gn = K * Phi;
				Wn = K * Slope;
				Energy += K * PressEnergy;
			}
			if (bGrip)
			{
				float Kt = K * LumpGrip;
				float2 Hand;
#if SB_LUMP_MODES > 0
				float Region1 = dot(R1[0], Qs[0]) + dot(R1[1], Qs[1]) + dot(R1[2], Qs[2]) + dot(R1[3], Qs[3]);
				float Region2 = dot(R2[0], Qs[0]) + dot(R2[1], Qs[1]) + dot(R2[2], Qs[2]) + dot(R2[3], Qs[3]);
#else
				float Region1 = 0.0;
				float Region2 = 0.0;
#endif
				float2 Moved = A * float2(dot(S1, E) + Region1, dot(S2, E) + Region2);
				// 마찰을 평형 안에서(일지 201). 한도 힘 = DragRatio × 지금 x에서 누르는 힘(접촉 패스의 보이는 깊이가 아니라 평형의 d — CPU가
				// 늦게 준 덩어리 자리가 한도에 들지 않는다). 손 닻 Hand = 덩어리가 안 움직였다면의 손 자리(면 위 두 축, 아래 첫 걸음).
				// 그 누르는 힘은 보이는 피부 끌림처럼 살 두께에 붙인 깊이로 잰다(일지 336). 평형의 누르는 힘은 코어 너머로 기울기 e^α
				// (Squeeze Stiffening 3이면 20배)로 곧게 자라 덩어리를 비키게 하지만(ADR-0017), 그 힘을 마찰 한도에도 그대로 쓰면 깊이 든 손이 덩어리를
				// 끝없이 세게 붙들었다(원칙 P06: 두께를 넘는 누름을 살의 추가 반응으로 만들지 않는다). 한도는 뉴턴 헤세에 들지 않아 볼록함은 그대로다.
				// 평형에서 눌리지 않은 칸(d ≤ 0 — 덩어리가 비켜나 손과 떨어진 가장자리)은 한도 힘이 0이다.
				float LimitPhi = D > 0.0 ? DragRatio * SB_TissueForce(SB_PressLimit(D, H), H, LumpStiffen) : 0.0;
				float Limit = SB_TissueInverse(LimitPhi, H, LumpStiffen);
				// 붙은 칸 스프링은 곧다(일지 254): 한도 거리에서 한도 힘에 닿는 직선이고, 단단함 Hold = 한도 힘 ÷ 한도 거리(살 법칙의 그 거리 할선, 1 이상).
				// 눌린 살층은 옆으로도 처음부터 단단하다 — FEM에서 붙은 살의 옆 단단함은 누르는 힘처럼 깊이에 따라 자랐다. 전의 굽은 법칙 φ(|Δ|)는 처음이
				// 쉬는 살만큼 물러, 2 cm 누른 손이 한도 거리의 절반을 가도 덩어리를 한도 힘의 1/3만 끌었다. 한도 거리·한도 힘은 그대로라 보이는 끌림과
				// 같은 자리에서 미끄러진다. 살 법칙이 곧으면(Lump Stiffen 0) Hold = 1이다. 한도 힘이 0인 칸은 Hold도 0이라 힘 없이 뉴턴 걸음을 붙들지 않고,
				// 1 µm보다 짧은 한도는 로그 정밀도가 모자라 1 µm로 나눈다(그 위와 이어진다).
				float Hold = LimitPhi / max(Limit, 1e-4);
				float Reach = Limit;
				float SlipForce = Kt * LimitPhi;
				if (bFirstStep != 0u)
				{
					// 손 닻(일지 201·338): 지난 닻(피부 접평면 두 성분)을 이 면 위로 되살리고(SB_GripLift) 접촉 패스가 넘긴 이번 손 몫의 면 성분을 더한다 — 손이
					// 이 면 노멀 쪽으로 움직인 몫(더 누르거나 떼는 움직임)은 누름이라 닻에 들지 않는다. 새로 눌린 칸은 지난 닻이 0이고 손 몫이 보이는 피부 자리다.
					float3 HandOnFace = SB_GripLift(SkinAnchor, RestN, T1, T2, GripN) + HandMove - GripN * dot(HandMove, GripN);
					Hand = float2(dot(HandOnFace, A1), dot(HandOnFace, A2));
					// 첫 걸음: 닻을 이번 뉴턴 출발점 x(지난 프레임 결과를 관성 목표만큼 옮긴 자리)에서 한도 안으로 자른다 — 지난 갱신에
					// 미끄러진 만큼 손이 피부 위를 지나간 것. 그래서 걸음은 늘 "모두 붙은" 점에서 출발한다(장난감에서 푼 뒤 그 자리에서 자르면
					// 뉴턴이 안 모였다). 자른 닻을 피부 접평면 두 성분으로 남겨 둘째 걸음부터와 다음 갱신의 접촉 패스가 읽는다.
					float2 Delta0 = Moved - Hand;
					float Stretch0 = length(Delta0);
					if (Stretch0 > Reach)
					{
						Hand = Moved - Delta0 * (Reach / max(Stretch0, 1e-6));
					}
					float3 Kept = A1 * Hand.x + A2 * Hand.y;
					LumpAnchorOut[T] = float2(dot(Kept, T1), dot(Kept, T2));
				}
				else
				{
					float3 Kept = SB_GripLift(LumpAnchorIn.Load(int3(T, 0)).xy, RestN, T1, T2, GripN);
					Hand = float2(dot(Kept, A1), dot(Kept, A2));
				}
				float2 Delta = Moved - Hand;
				float Stretch = length(Delta);
				if (Stretch <= Reach)
				{
					// 붙은 칸: 손이 끈 자리와 덩어리 옆 이동의 차 Δ만큼 살층이 곧은 스프링으로 당긴다. 힘 Kt·Hold·Δ, 접선 2×2 강성은 두 방향 다 Kt·Hold.
					Mxx = Kt * Hold;
					Myy = Kt * Hold;
					G1 = Kt * Hold * Delta.x;
					G2 = Kt * Hold * Delta.y;
					Energy += 0.5 * Kt * Hold * Stretch * Stretch;
				}
				else
				{
					// 미끄러지는 칸(일지 201): 힘 크기 = 미끄러지는 힘 SlipForce(지금 x의 누름에 기댄다), 방향 = 닻 쪽. 버티는 거리 Reach에서 붙은 칸의
					// 힘 Kt·Hold·Reach와 같아 이어진다. 에너지 몫은 그 너머를 미끄러지는 힘으로 곧게 이은 값("힘이 있었다" 표지용 — 받아들임은 LumpSolveCS가
					// 잔차로 잰다).
					float2 Along = Delta / max(Stretch, 1e-6);
					G1 = SlipForce * Along.x;
					G2 = SlipForce * Along.y;
					Energy += SlipForce * (Stretch - 0.5 * Reach);
				}
			}
			if (bGrab)
			{
				// 잡기(ADR-0041 §3.4): 덩어리 이동(노멀 몫 + 옆 몫, 국소 모양 포함)을 한도 안 당김 쪽으로(목표는 덩어리의 지금 자리를 보지 않는다 — 일지 350) — 세
				// 방향 곧은 용수철, 세기 Kg = Lump Grip × 칸 강성 ×
				// 손끝 몫(누르는 손의 쥐기와 같은 눈금: 1 = 살이 누름을 버티는 만큼), 마찰 한도 없음. 손–피부(표면 장)와 직렬이라 덩어리가 Lump Grip과 덩어리
				// 단단함의 비만큼 따라 나오고 나머지를 표면 장이 그린다. 원 안 칸은 누르지 않은 칸이라 축은 피부 축 그대로다(A1 = T1, A2 = T2).
				float Kg = LumpGrip * K * SkinAnchor.x;
#if SB_LUMP_MODES > 0
				float GrabRegion1 = dot(R1[0], Qs[0]) + dot(R1[1], Qs[1]) + dot(R1[2], Qs[2]) + dot(R1[3], Qs[3]);
				float GrabRegion2 = dot(R2[0], Qs[0]) + dot(R2[1], Qs[1]) + dot(R2[2], Qs[2]) + dot(R2[3], Qs[3]);
#else
				float GrabRegion1 = 0.0;
				float GrabRegion2 = 0.0;
#endif
				float ResN = A * (dot(GPush, E) + RegionN) - dot(HandMove, RestN);
				float Res1 = A * (dot(S1, E) + GrabRegion1) - dot(HandMove, T1);
				float Res2 = A * (dot(S2, E) + GrabRegion2) - dot(HandMove, T2);
				Wn += Kg;
				Gn += Kg * ResN;
				Mxx += Kg;
				Myy += Kg;
				G1 += Kg * Res1;
				G2 += Kg * Res2;
				Energy += 0.5 * Kg * (ResN * ResN + Res1 * Res1 + Res2 * Res2);
			}
			bActive = Wn != 0.0 || Gn != 0.0 || Mxx != 0.0 || Myy != 0.0 || G1 != 0.0 || G2 != 0.0 || Energy != 0.0;
			if (bActive)
			{
				// 힘을 받는 칸만 받은 차례대로 줄을 적는다 — 더하기가 빈칸 없이 줄을 바로 읽는다.
				uint Slot;
				InterlockedAdd(SB_LumpActiveCount, 1u, Slot);
				SB_LumpRowGroup[Slot] = Group;
				InterlockedOr(SB_LumpGroupMask, 1u << Group);
				uint RowBase = Slot * SB_LUMP_ROW_FLOATS;
#if SB_LUMP_MODES > 0
				UNROLL
				for (int Quad = 0; Quad < 4; ++Quad)
				{
					UNROLL
					for (int Lane = 0; Lane < 4; ++Lane)
					{
						SB_LumpRows[RowBase + 4 * Quad + Lane] = Rn[Quad][Lane];
						SB_LumpRows[RowBase + SB_LUMP_DIM + 4 * Quad + Lane] = R1[Quad][Lane];
						SB_LumpRows[RowBase + 2 * SB_LUMP_DIM + 4 * Quad + Lane] = R2[Quad][Lane];
					}
				}
#endif
				UNROLL
				for (int Axis = 0; Axis < 3; ++Axis)
				{
					SB_LumpRows[RowBase + SB_LUMP_E + Axis] = GPush[Axis];
					SB_LumpRows[RowBase + SB_LUMP_DIM + SB_LUMP_E + Axis] = S1[Axis];
					SB_LumpRows[RowBase + 2 * SB_LUMP_DIM + SB_LUMP_E + Axis] = S2[Axis];
				}
				SB_LumpRows[RowBase + SB_LUMP_ROW_WN] = Wn;
				SB_LumpRows[RowBase + SB_LUMP_ROW_GN] = Gn;
				SB_LumpRows[RowBase + SB_LUMP_ROW_MXX] = Mxx;
				SB_LumpRows[RowBase + SB_LUMP_ROW_MXY] = Mxy;
				SB_LumpRows[RowBase + SB_LUMP_ROW_MYY] = Myy;
				SB_LumpRows[RowBase + SB_LUMP_ROW_G1] = G1;
				SB_LumpRows[RowBase + SB_LUMP_ROW_G2] = G2;
				SB_LumpRows[RowBase + SB_LUMP_ROW_W] = Energy;
				SB_LumpRows[RowBase + SB_LUMP_ROW_ONE] = 1.0;
				SB_LumpRows[RowBase + SB_LUMP_ROW_ZERO] = 0.0;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	// 이 스레드 묶음이 맡은 값마다 스레드 하나가 그 덩어리 묶음의 눌린 칸들을 더한다(묶음 안에 덩어리가 둘이면 덩어리마다 한 번씩).
	uint Count = SB_LumpActiveCount;
	uint Remaining = SB_LumpGroupMask;
	bool bOneGroup = (Remaining & (Remaining - 1u)) == 0u;
	uint Value = GroupId.z * SB_LUMP_PART_VALUES + LocalIndex;
	if (LocalIndex < SB_LUMP_PART_VALUES && Value < SB_LUMP_VALUES)
	{
		// 값 하나 = 줄마다 w1·u1·v1 + w2·u2·v2 + w3·(u2·v3 + u3·v2) + w4·u3·v3의 합. 헤세 (a, b)는 노멀·접선 두 축 몫 그대로,
		// 기울기 a는 v = 1·1·0(w = φ 몫·접선 두 기울기 몫), 에너지는 w1 = 에너지 몫·u1 = v1 = 1에 나머지 0 — 갈래 없이 같은 식이라 고리를 펴 겹쳐 읽는다.
		uint W1 = SB_LUMP_ROW_W;
		uint W2 = SB_LUMP_ROW_ZERO;
		uint W3 = SB_LUMP_ROW_ZERO;
		uint W4 = SB_LUMP_ROW_ZERO;
		uint U1 = SB_LUMP_ROW_ONE;
		uint V1 = SB_LUMP_ROW_ONE;
		uint U2 = SB_LUMP_ROW_ZERO;
		uint V2 = SB_LUMP_ROW_ZERO;
		uint U3 = SB_LUMP_ROW_ZERO;
		uint V3 = SB_LUMP_ROW_ZERO;
		if (Value < SB_LUMP_DIM)
		{
			W1 = SB_LUMP_ROW_GN;
			W2 = SB_LUMP_ROW_G1;
			W3 = SB_LUMP_ROW_G2;
			U1 = Value;
			U2 = SB_LUMP_DIM + Value;
			U3 = 2u * SB_LUMP_DIM + Value;
			V2 = SB_LUMP_ROW_ONE;
		}
		else if (Value < SB_LUMP_VALUE_ENERGY)
		{
			uint IA;
			uint IB;
			SB_LumpHessPair(Value - SB_LUMP_DIM, IA, IB);
			W1 = SB_LUMP_ROW_WN;
			W2 = SB_LUMP_ROW_MXX;
			W3 = SB_LUMP_ROW_MXY;
			W4 = SB_LUMP_ROW_MYY;
			U1 = IA;
			V1 = IB;
			U2 = SB_LUMP_DIM + IA;
			V2 = SB_LUMP_DIM + IB;
			U3 = 2u * SB_LUMP_DIM + IA;
			V3 = 2u * SB_LUMP_DIM + IB;
		}
		LOOP
		while (Remaining != 0u)
		{
			uint Target = firstbitlow(Remaining);
			Remaining &= Remaining - 1u;
			float4 Sums = float4(0, 0, 0, 0);
			LOOP
			for (uint Item = 0u; Item < Count; Item += 4u)
			{
				UNROLL
				for (uint Lane = 0u; Lane < 4u; ++Lane)
				{
					uint Row = Item + Lane;
					uint B0 = min(Row, 63u) * SB_LUMP_ROW_FLOATS;
					float Term = SB_LumpRows[B0 + W1] * SB_LumpRows[B0 + U1] * SB_LumpRows[B0 + V1]
						+ SB_LumpRows[B0 + W2] * SB_LumpRows[B0 + U2] * SB_LumpRows[B0 + V2]
						+ SB_LumpRows[B0 + W3] * (SB_LumpRows[B0 + U2] * SB_LumpRows[B0 + V3] + SB_LumpRows[B0 + U3] * SB_LumpRows[B0 + V2])
						+ SB_LumpRows[B0 + W4] * SB_LumpRows[B0 + U3] * SB_LumpRows[B0 + V3];
					bool bTake = Row < Count && (bOneGroup || SB_LumpRowGroup[min(Row, 63u)] == Target);
					Sums[Lane] += bTake ? Term : 0.0;
				}
			}
			float Sum = (Sums.x + Sums.y) + (Sums.z + Sums.w);
			// 묶음 하나의 합이라 칸 하나보다 크다: 고정소수점이 32비트에 들게 자른다(4096 × 500000 < 2³¹).
			int Fixed = (int)round(clamp(Sum, -500000.0, 500000.0) * SB_LUMP_SCALE);
			if (Fixed != 0)
			{
				SB_AddLump64(Target * SB_LUMP_VALUES + Value, Fixed);
			}
		}
	}
}

// --- 덩어리 평형 한 걸음 (ADR-0017, 국소 모양 일지 251) ---
// 프레임마다 LumpStartCS 한 번, 그 뒤 (LumpForceCS → LumpSolveCS)를 CPU가 정한 횟수만큼. LumpSolveCS는 덩어리 묶음마다 스레드 묶음 하나.
// 칸 합 버퍼(LumpSumsOut)는 LumpStartCS가 비우고, LumpSolveCS가 제 묶음 값을 읽은 뒤 다시 비워 다음 걸음이 바로 더한다.
// LumpState 묶음마다 SB_LUMP_STATE_ROWS줄:
//   0 (이번에 잴 E, 1 = 받아들인 점이 있다)        1 (받아들인 E, 그 |∇Ψ|)
//   2 (지난 프레임 결과 E, 1 = 힘이 있었다)         3 (지난 프레임 T)
//   4 (받아들인 점의 살 헤세 H_eff: xx xy xz yy)    5 (H_eff: yz zz, 1 = 힘이 있었다, 0)
//   6~9 이번에 잴 q(넷씩)   10~13 받아들인 q   14~17 지난 프레임 결과 q
//   18~29 받아들인 점에서 q가 E를 따라가는 비율 M = −H_qq⁻¹·H_qE: E 성분 c마다 넷씩 4줄(18 + 4c + j = 모양 4j ~ 4j + 3)
//   (일지 334~337의 줄 30·31 — 묶음마다 미는 방향 평균 — 은 일지 338에서 지웠다: 손 마찰 면은 같은 갱신 누른 것 표에서 잰다, SB_GripNormal.)
// H_eff = q를 지운 E의 헤세(q가 따라 움직인 뒤) − C: CPU 모드(쥔 살의 강성, ADR-0012 §8)가 읽는다 — 국소 모양이 직렬로 끼어 그만큼 무르다.
// 뉴턴 걸음이 기울기 크기 |∇Ψ|를 늘렸으면(지나침 — 손이 떨어지거나 붙는 칸이 바뀔 때) 받아들인 점 쪽으로 반 되돌린다.
RWStructuredBuffer<float4> LumpState;
RWBuffer<float> LumpOut;                // 묶음마다 SB_LUMP_OUTPUTS: E(3), H_eff(6: xx xy xz yy yz zz), T(3), 1 = 힘이 있었다
uint bLastStep;                         // 1 = 이번 프레임 마지막 걸음: LumpOut과 다음 프레임 시작점을 적는다
RWBuffer<uint> LumpForceArgsOut;        // LumpStartCS: 덩어리 힘 패스의 간접 인자(LumpActiveCS가 고른 묶음 수, 한 줄 SB_CONTACT_ROW 묶음)

/**
 * 64비트 고정소수점 합(아래·위 32비트, 2의 보수)을 실수로. 음수는 부호를 떼고 옮긴다 — 아래·위를 따로 실수로 바꿔 더하면 작은 음수가
 * 2³²에 가까운 두 수의 차가 되어 1/16 눈금으로 뭉개진다.
 */
float SB_Fixed64(uint Low, uint High)
{
	bool bNegative = (High & 0x80000000u) != 0u;
	if (bNegative)
	{
		Low = ~Low + 1u;
		High = ~High + (Low == 0u ? 1u : 0u);
	}
	float Value = ((float)High * 4294967296.0 + (float)Low) / SB_LUMP_SCALE;
	return bNegative ? -Value : Value;
}

[numthreads(SB_LUMP_GROUPS, 1, 1)]
void LumpStartCS(uint Group : SV_GroupIndex)
{
	// 지난 프레임 결과에서 이어 푼다(관성 목표가 움직인 만큼 E를 같이 옮김, q는 그대로). 지난 프레임에 힘이 없었으면 관성 목표와 q = 0에서.
	uint Row = Group * SB_LUMP_STATE_ROWS;
	float3 T = LumpGroups[Group * SB_LUMP_GROUP_ROWS + 2].xyz;
	float4 Last = LumpState[Row + 2];
	bool bLast = Last.w > 0.5;
	float3 Start = bLast ? Last.xyz + (T - LumpState[Row + 3].xyz) : T;
	LumpState[Row + 0] = float4(Start, 0.0);
	LumpState[Row + 3] = float4(T, 0.0);
#if SB_LUMP_MODES > 0
	UNROLL
	for (uint Quad = 0u; Quad < 4u; ++Quad)
	{
		LumpState[Row + 6u + Quad] = bLast ? LumpState[Row + 14u + Quad] : float4(0, 0, 0, 0);
	}
#endif
	for (uint Word = 0u; Word < SB_LUMP_VALUES * 2u; ++Word)
	{
		LumpSumsOut[Group * SB_LUMP_VALUES * 2u + Word] = 0u;
	}
	if (Group == 0u)
	{
		// 고른 묶음마다 스레드 묶음 SB_LUMP_FORCE_PARTS개(z).
		uint Count = LumpActiveCount[0];
		LumpForceArgsOut[0] = min(Count, SB_CONTACT_ROW);
		LumpForceArgsOut[1] = (Count + SB_CONTACT_ROW - 1u) / SB_CONTACT_ROW;
		LumpForceArgsOut[2] = SB_LUMP_FORCE_PARTS;
	}
}

float SB_LumpSum(uint Group, uint Value)
{
	uint Index = (Group * SB_LUMP_VALUES + Value) * 2u;
	return SB_Fixed64(LumpSumsOut[Index], LumpSumsOut[Index + 1u]);
}

/** 덩어리 묶음 Group의 국소 모양 Mode의 단단함 Λ(굽기, 모양이 없는 자리는 CPU가 1). */
float SB_RegionStiffness(uint Group, uint Mode)
{
	return LumpGroups[Group * SB_LUMP_GROUP_ROWS + 3u + Mode / 4u][Mode % 4u];
}

groupshared float SB_SolveH[SB_LUMP_DIM * SB_LUMP_DIM];   // 헤세 아래 삼각(대각 크기 맞춤 뒤 제자리 촐레스키 — L). 위 삼각은 쓰지 않는다
groupshared float SB_SolveG[SB_LUMP_DIM];                 // 기울기
groupshared float SB_SolveX[SB_LUMP_DIM];                 // 지금 반복 값 (q, E)
groupshared float SB_SolveScale[SB_LUMP_DIM];             // 대각 크기 맞춤 1/√H_ii
groupshared float SB_SolveSum[SB_LUMP_VALUES];            // 칸 합
groupshared float SB_SolveNext[SB_LUMP_DIM];              // 다음 반복 값
// 삼각 풀기 네 줄(줄마다 SB_LUMP_DIM): 0 = 뉴턴 걸음(촐레스키와 같이 앞에서부터, 그 뒤 뒤에서부터), 1~3 = E 성분마다 M 한 열(q 16개)
groupshared float SB_SolveY[4 * SB_LUMP_DIM];
groupshared float SB_SolveHeff[7];                        // 받아들인 점의 H_eff 6값과 "힘이 있었다"
#if SB_LUMP_MODES > 0
groupshared float SB_SolveKeep[SB_LUMP_E];                // 받아들인 q(되돌리는 걸음이 반 돌아갈 자리)
groupshared float SB_SolveLambda[SB_LUMP_E];              // 국소 모양마다 단단함 Λ
#endif
groupshared uint SB_SolvePairs[SB_LUMP_HESS];             // 헤세 위 삼각 번호 → 행 | 열 << 8(처음에 한 번 풀어 둔다 — 촐레스키 열마다 다시 셈하지 않게)
groupshared uint SB_SolveAccept;                          // 1 = 이번 점을 받아들여 뉴턴 걸음

/** LumpSolveCS 스레드 수: 웨이브 하나 — 촐레스키 열마다 장벽이 셋이라 묶음이 웨이브 하나면 장벽이 거의 공짜다. 미지수 수(SB_LUMP_DIM)보다 커야 한다. */
#define SB_LUMP_SOLVE_THREADS 32u

[numthreads(SB_LUMP_SOLVE_THREADS, 1, 1)]
void LumpSolveCS(uint3 GroupId : SV_GroupID, uint LocalIndex : SV_GroupIndex)
{
	// 장벽은 모든 스레드가 같은 흐름에서 지나야 한다(D3D11 X3663): 풀지 않는 묶음·되돌리는 걸음도 촐레스키를 같은 차례로 돌고 결과만 쓰지 않는다.
	uint Group = GroupId.x;
	uint Row = Group * SB_LUMP_STATE_ROWS;
	float4 Info0 = LumpGroups[Group * SB_LUMP_GROUP_ROWS + 0u];
	float4 Info1 = LumpGroups[Group * SB_LUMP_GROUP_ROWS + 1u];
	float4 Info2 = LumpGroups[Group * SB_LUMP_GROUP_ROWS + 2u];
	float3 T = Info2.xyz;
	bool bSolved = Info2.w > 0.0;
	float S = Info0.w;
	float A = max(Info1.w, 1e-3);
	float3x3 C = float3x3(Info0.x, Info0.y, Info0.z, Info0.y, Info1.x, Info1.y, Info0.z, Info1.y, Info1.z);
	// 읽기는 모두 처음에 한꺼번에 낸다(고리를 펴 기다림이 한 번이 되게): 칸 합, 받아들인 점, 국소 모양 단단함.
	float4 Iterate = LumpState[Row];
	float4 Accepted = LumpState[Row + 1u];
	UNROLL
	for (uint SumRound = 0u; SumRound < (SB_LUMP_VALUES + SB_LUMP_SOLVE_THREADS - 1u) / SB_LUMP_SOLVE_THREADS; ++SumRound)
	{
		uint SumIndex = LocalIndex + SumRound * SB_LUMP_SOLVE_THREADS;
		if (SumIndex < SB_LUMP_VALUES)
		{
			SB_SolveSum[SumIndex] = SB_LumpSum(Group, SumIndex);
		}
	}
#if SB_LUMP_MODES > 0
	if (LocalIndex < SB_LUMP_E)
	{
		SB_SolveKeep[LocalIndex] = LumpState[Row + 10u + LocalIndex / 4u][LocalIndex % 4u];
		SB_SolveLambda[LocalIndex] = SB_RegionStiffness(Group, LocalIndex);
	}
#endif
	LOOP
	for (uint Decode = LocalIndex; Decode < SB_LUMP_HESS; Decode += SB_LUMP_SOLVE_THREADS)
	{
		uint DA;
		uint DB;
		SB_LumpHessPair(Decode, DA, DB);
		SB_SolvePairs[Decode] = DA | (DB << 8u);
	}
	if (LocalIndex < SB_LUMP_DIM)
	{
		// FXC는 ?:의 두 쪽을 다 셈한다 — 범위 밖 첨자가 생기지 않게 갈래로 나눈다.
		uint I0 = LocalIndex;
#if SB_LUMP_MODES > 0
		if (I0 < SB_LUMP_E)
		{
			SB_SolveX[I0] = LumpState[Row + 6u + I0 / 4u][I0 % 4u];
			SB_SolveNext[I0] = 0.0;
		}
		else
#endif
		{
			SB_SolveX[I0] = LumpState[Row][I0 - SB_LUMP_E];
			SB_SolveNext[I0] = T[I0 - SB_LUMP_E];
		}
	}
	if (LocalIndex == 0u)
	{
		SB_SolveAccept = 0u;
		float4 Heff0 = LumpState[Row + 4u];
		float4 Heff1 = LumpState[Row + 5u];
		SB_SolveHeff[0] = Heff0.x;
		SB_SolveHeff[1] = Heff0.y;
		SB_SolveHeff[2] = Heff0.z;
		SB_SolveHeff[3] = Heff0.w;
		SB_SolveHeff[4] = Heff1.x;
		SB_SolveHeff[5] = Heff1.y;
		SB_SolveHeff[6] = Heff1.z;
	}
	GroupMemoryBarrierWithGroupSync();

	// 기울기·헤세(아래 삼각만 — 아래 풀이가 아래 삼각만 읽는다). 풀지 않는 묶음은 단위 행렬로 채운다.
	float3 Off = float3(SB_SolveX[SB_LUMP_E], SB_SolveX[SB_LUMP_E + 1u], SB_SolveX[SB_LUMP_E + 2u]) - T;
	if (LocalIndex < SB_LUMP_DIM)
	{
		uint I1 = LocalIndex;
		float Own;
#if SB_LUMP_MODES > 0
		if (I1 < SB_LUMP_E)
		{
			Own = SB_SolveLambda[I1] * SB_SolveX[I1];
		}
		else
#endif
		{
			Own = dot(C[I1 - SB_LUMP_E], Off);
		}
		SB_SolveG[I1] = bSolved ? Own + (S / A) * SB_SolveSum[I1] : 0.0;
	}
	LOOP
	for (uint Pair = LocalIndex; Pair < SB_LUMP_HESS; Pair += SB_LUMP_SOLVE_THREADS)
	{
		uint IA = SB_SolvePairs[Pair] & 0xffu;
		uint IB = SB_SolvePairs[Pair] >> 8u;
		float Value = S * SB_SolveSum[SB_LUMP_DIM + Pair];
#if SB_LUMP_MODES > 0
		if (IA == IB && IA < SB_LUMP_E)
		{
			Value += SB_SolveLambda[IA];
		}
		if (IA >= SB_LUMP_E)
#endif
		{
			Value += C[IA - SB_LUMP_E][IB - SB_LUMP_E];
		}
		SB_SolveH[IB * SB_LUMP_DIM + IA] = bSolved ? Value : (IA == IB ? 1.0 : 0.0);
	}
	GroupMemoryBarrierWithGroupSync();

	if (LocalIndex == 0u && bSolved)
	{
		// 걸음을 받아들일지 재는 값(일지 201): 기울기 크기 |∇Ψ| — 커지면 지나친 것이라 반 되돌린다. 에너지 Ψ는 미끄러지는 칸의 힘이 보존력이
		// 아니라 뜻이 없다. 칸 합의 크기(기울기·헤세 대각·에너지)는 "힘이 있었다" 표지에만 든다.
		float MeritSq = 0.0;
		float Activity = abs(SB_SolveSum[SB_LUMP_VALUE_ENERGY]);
		for (uint I2 = 0u; I2 < SB_LUMP_DIM; ++I2)
		{
			MeritSq += SB_SolveG[I2] * SB_SolveG[I2];
			Activity += abs(SB_SolveSum[I2]) + abs(SB_SolveSum[SB_LUMP_DIM + I2 * SB_LUMP_DIM - (I2 * (I2 - 1u)) / 2u]);
		}
		float Merit = sqrt(MeritSq);
		bool bHas = Iterate.w > 0.5;
		if (bHas && Merit > Accepted.w + 1e-4 * abs(Accepted.w) + 1e-7)
		{
#if SB_LUMP_MODES > 0
			for (uint I3 = 0u; I3 < SB_LUMP_E; ++I3)
			{
				SB_SolveNext[I3] = 0.5 * (SB_SolveX[I3] + SB_SolveKeep[I3]);
			}
#endif
			for (uint I3E = 0u; I3E < 3u; ++I3E)
			{
				SB_SolveNext[SB_LUMP_E + I3E] = 0.5 * (SB_SolveX[SB_LUMP_E + I3E] + Accepted[I3E]);
			}
		}
		else
		{
			LumpState[Row + 1u] = float4(Off + T, Merit);
#if SB_LUMP_MODES > 0
			for (uint Quad0 = 0u; Quad0 < 4u; ++Quad0)
			{
				LumpState[Row + 10u + Quad0] = float4(SB_SolveX[4u * Quad0], SB_SolveX[4u * Quad0 + 1u], SB_SolveX[4u * Quad0 + 2u],
					SB_SolveX[4u * Quad0 + 3u]);
			}
#endif
			SB_SolveHeff[6] = Activity > 0.0 ? 1.0 : 0.0;
			SB_SolveAccept = 1u;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	bool bStep = bSolved && SB_SolveAccept != 0u;

	// 대각 크기 맞춤: 헤세 원소가 덩어리·눌린 칸 수에 따라 몇 자리씩 달라 단정밀 촐레스키가 흔들리지 않게 H' = D H D, D = diag(1/√H_ii).
	if (LocalIndex < SB_LUMP_DIM)
	{
		SB_SolveScale[LocalIndex] = rsqrt(max(SB_SolveH[LocalIndex * SB_LUMP_DIM + LocalIndex], 1e-20));
	}
	GroupMemoryBarrierWithGroupSync();
	LOOP
	for (uint Cell = LocalIndex; Cell < SB_LUMP_HESS; Cell += SB_LUMP_SOLVE_THREADS)
	{
		uint CA = SB_SolvePairs[Cell] & 0xffu;
		uint CB = SB_SolvePairs[Cell] >> 8u;
		SB_SolveH[CB * SB_LUMP_DIM + CA] *= SB_SolveScale[CA] * SB_SolveScale[CB];
	}
	if (LocalIndex < SB_LUMP_DIM)
	{
		// 뉴턴 걸음의 오른쪽 D·∇Ψ — 촐레스키가 열마다 앞에서부터 풀어 L·y = D·∇Ψ의 y로 만든다.
		SB_SolveY[LocalIndex] = SB_SolveG[LocalIndex] * SB_SolveScale[LocalIndex];
	}
	GroupMemoryBarrierWithGroupSync();
	// 촐레스키(아래 삼각, 제자리, 열 차례로)와 뉴턴 걸음의 앞에서부터 풀기를 같은 장벽 안에서. 열마다 장벽 둘: 스레드마다 대각 제곱근을 같이
	// 셈해 제 행을 나누고(대각은 다음 단계에 적는다), 남은 아래 삼각을 고친다. q 열을 다 지운 뒤(Col = SB_LUMP_E) 남은 3×3이 q가 따라 움직인 뒤의
	// E 헤세다 — CPU 모드가 쓰는 H_eff(그 − C)를 그 열 앞에서 적는다. 크기 맞춤을 되돌린다: H = D⁻¹H'D⁻¹.
	LOOP
	for (uint Col = 0u; Col < SB_LUMP_DIM; ++Col)
	{
		if (Col == SB_LUMP_E)
		{
			if (LocalIndex == 0u && bStep)
			{
				uint E0 = SB_LUMP_E;
				float Sx = 1.0 / SB_SolveScale[E0];
				float Sy = 1.0 / SB_SolveScale[E0 + 1u];
				float Sz = 1.0 / SB_SolveScale[E0 + 2u];
				SB_SolveHeff[0] = SB_SolveH[E0 * SB_LUMP_DIM + E0] * Sx * Sx - C[0][0];
				SB_SolveHeff[1] = SB_SolveH[(E0 + 1u) * SB_LUMP_DIM + E0] * Sy * Sx - C[1][0];
				SB_SolveHeff[2] = SB_SolveH[(E0 + 2u) * SB_LUMP_DIM + E0] * Sz * Sx - C[2][0];
				SB_SolveHeff[3] = SB_SolveH[(E0 + 1u) * SB_LUMP_DIM + E0 + 1u] * Sy * Sy - C[1][1];
				SB_SolveHeff[4] = SB_SolveH[(E0 + 2u) * SB_LUMP_DIM + E0 + 1u] * Sz * Sy - C[2][1];
				SB_SolveHeff[5] = SB_SolveH[(E0 + 2u) * SB_LUMP_DIM + E0 + 2u] * Sz * Sz - C[2][2];
			}
			GroupMemoryBarrierWithGroupSync();
		}
		float Pivot = sqrt(max(SB_SolveH[Col * SB_LUMP_DIM + Col], 1e-12));
		if (LocalIndex > Col && LocalIndex < SB_LUMP_DIM)
		{
			SB_SolveH[LocalIndex * SB_LUMP_DIM + Col] /= Pivot;
		}
		else if (LocalIndex == Col)
		{
			SB_SolveY[Col] /= Pivot;
		}
		GroupMemoryBarrierWithGroupSync();
		if (LocalIndex == Col)
		{
			SB_SolveH[Col * SB_LUMP_DIM + Col] = Pivot;
		}
		// 남은 아래 삼각(행·열 모두 Col 뒤) — 위 삼각 번호에서 행 Col + 1부터가 이어진 번호다.
		LOOP
		for (uint Pair1 = SB_LumpPairStart(Col + 1u) + LocalIndex; Pair1 < SB_LUMP_HESS; Pair1 += SB_LUMP_SOLVE_THREADS)
		{
			uint J4 = SB_SolvePairs[min(Pair1, SB_LUMP_HESS - 1u)] & 0xffu;
			uint I4 = SB_SolvePairs[min(Pair1, SB_LUMP_HESS - 1u)] >> 8u;
			SB_SolveH[I4 * SB_LUMP_DIM + J4] -= SB_SolveH[I4 * SB_LUMP_DIM + Col] * SB_SolveH[J4 * SB_LUMP_DIM + Col];
		}
		if (LocalIndex > Col && LocalIndex < SB_LUMP_DIM)
		{
			SB_SolveY[LocalIndex] -= SB_SolveH[LocalIndex * SB_LUMP_DIM + Col] * SB_SolveY[Col];
		}
		GroupMemoryBarrierWithGroupSync();
	}
#if SB_LUMP_MODES > 0
	// q가 E를 따라가는 비율 M = −H_qq⁻¹·H_qE(국소 모양 패스가 쓴다): E 성분마다 L_qqᵀ·m = −(L_Eq의 그 행). 오른쪽을 채운다.
	LOOP
	for (uint Fill = LocalIndex; Fill < 3u * SB_LUMP_E; Fill += SB_LUMP_SOLVE_THREADS)
	{
		uint FillAxis = Fill / SB_LUMP_E;
		uint FillMode = Fill % SB_LUMP_E;
		SB_SolveY[(1u + FillAxis) * SB_LUMP_DIM + FillMode] = -SB_SolveH[(SB_LUMP_E + FillAxis) * SB_LUMP_DIM + FillMode];
	}
	GroupMemoryBarrierWithGroupSync();
#endif
	// 뒤에서부터 풀기 네 줄을 열 차례로 같이(열마다 장벽 둘): 줄 0 = 뉴턴 걸음 Lᵀ·z = y(미지수 19개), 줄 1~3 = M의 열(q 16개 — L_qq만 쓴다).
	// 열 Back의 값을 정하고(스레드 넷), 그 앞 행들에서 그 몫을 뺀다(행마다 스레드 하나, 네 줄).
	LOOP
	for (int Back = SB_LUMP_DIM - 1; Back >= 0; --Back)
	{
		uint B = (uint)Back;
#if SB_LUMP_MODES > 0
		if (bStep && LocalIndex < 4u && (LocalIndex == 0u || B < SB_LUMP_E))
#else
		if (bStep && LocalIndex == 0u)
#endif
		{
			SB_SolveY[LocalIndex * SB_LUMP_DIM + B] /= SB_SolveH[B * SB_LUMP_DIM + B];
		}
		GroupMemoryBarrierWithGroupSync();
		if (bStep && LocalIndex < B)
		{
			float Below = SB_SolveH[B * SB_LUMP_DIM + LocalIndex];
			SB_SolveY[LocalIndex] -= Below * SB_SolveY[B];
#if SB_LUMP_MODES > 0
			if (B < SB_LUMP_E)
			{
				UNROLL
				for (uint Column = 1u; Column < 4u; ++Column)
				{
					SB_SolveY[Column * SB_LUMP_DIM + LocalIndex] -= Below * SB_SolveY[Column * SB_LUMP_DIM + B];
				}
			}
#endif
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (bStep && LocalIndex < SB_LUMP_DIM)
	{
		// 뉴턴 걸음: Δ = D·z, 다음 = x − Δ.
		SB_SolveNext[LocalIndex] = SB_SolveX[LocalIndex] - SB_SolveY[LocalIndex] * SB_SolveScale[LocalIndex];
	}
#if SB_LUMP_MODES > 0
	if (bStep && LocalIndex < 12u)
	{
		// M 한 줄(E 성분 Axis, 모양 넷). 크기 맞춤을 되돌린다: M = D_q·M'·D_E⁻¹.
		uint Axis = LocalIndex / 4u;
		uint Quad1 = LocalIndex % 4u;
		uint Y0 = (1u + Axis) * SB_LUMP_DIM + 4u * Quad1;
		uint M0 = 4u * Quad1;
		float Undo = 1.0 / SB_SolveScale[SB_LUMP_E + Axis];
		LumpState[Row + 18u + 4u * Axis + Quad1] = float4(SB_SolveY[Y0] * SB_SolveScale[M0], SB_SolveY[Y0 + 1u] * SB_SolveScale[M0 + 1u],
			SB_SolveY[Y0 + 2u] * SB_SolveScale[M0 + 2u], SB_SolveY[Y0 + 3u] * SB_SolveScale[M0 + 3u]) * Undo;
	}
#endif
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex == 0u && bSolved)
	{
		// 수치 안전장: E는 관성 목표에서 20 g 안(CPU가 쓰던 상한과 같다), q도 크기 20 안.
		float3 NextE = float3(SB_SolveNext[SB_LUMP_E], SB_SolveNext[SB_LUMP_E + 1u], SB_SolveNext[SB_LUMP_E + 2u]);
		float3 Shift = NextE - T;
		float Length = length(Shift);
		NextE = Length > 20.0 ? T + Shift * (20.0 / Length) : NextE;
		SB_SolveNext[SB_LUMP_E] = NextE.x;
		SB_SolveNext[SB_LUMP_E + 1u] = NextE.y;
		SB_SolveNext[SB_LUMP_E + 2u] = NextE.z;
#if SB_LUMP_MODES > 0
		float QSq = 0.0;
		for (uint I8 = 0u; I8 < SB_LUMP_E; ++I8)
		{
			QSq += SB_SolveNext[I8] * SB_SolveNext[I8];
		}
		float QScale = QSq > 400.0 ? 20.0 * rsqrt(QSq) : 1.0;
		for (uint Quad2 = 0u; Quad2 < 4u; ++Quad2)
		{
			uint Q0 = 4u * Quad2;
			SB_SolveNext[Q0] *= QScale;
			SB_SolveNext[Q0 + 1u] *= QScale;
			SB_SolveNext[Q0 + 2u] *= QScale;
			SB_SolveNext[Q0 + 3u] *= QScale;
			LumpState[Row + 6u + Quad2] = float4(SB_SolveNext[Q0], SB_SolveNext[Q0 + 1u], SB_SolveNext[Q0 + 2u], SB_SolveNext[Q0 + 3u]);
		}
#endif
		LumpState[Row] = float4(NextE, 1.0);
		if (bStep)
		{
			LumpState[Row + 4u] = float4(SB_SolveHeff[0], SB_SolveHeff[1], SB_SolveHeff[2], SB_SolveHeff[3]);
			LumpState[Row + 5u] = float4(SB_SolveHeff[4], SB_SolveHeff[5], SB_SolveHeff[6], 0.0);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (bLastStep != 0u && LocalIndex == 0u)
	{
		bool bForce = bSolved && SB_SolveHeff[6] > 0.5;
		float3 NextE = float3(SB_SolveNext[SB_LUMP_E], SB_SolveNext[SB_LUMP_E + 1u], SB_SolveNext[SB_LUMP_E + 2u]);
		uint Out = Group * SB_LUMP_OUTPUTS;
		LumpOut[Out + 0] = NextE.x;
		LumpOut[Out + 1] = NextE.y;
		LumpOut[Out + 2] = NextE.z;
		LumpOut[Out + 3] = bForce ? SB_SolveHeff[0] : 0.0;
		LumpOut[Out + 4] = bForce ? SB_SolveHeff[1] : 0.0;
		LumpOut[Out + 5] = bForce ? SB_SolveHeff[2] : 0.0;
		LumpOut[Out + 6] = bForce ? SB_SolveHeff[3] : 0.0;
		LumpOut[Out + 7] = bForce ? SB_SolveHeff[4] : 0.0;
		LumpOut[Out + 8] = bForce ? SB_SolveHeff[5] : 0.0;
		LumpOut[Out + 9] = T.x;
		LumpOut[Out + 10] = T.y;
		LumpOut[Out + 11] = T.z;
		LumpOut[Out + 12] = bForce ? 1.0 : 0.0;
		LumpState[Row + 2u] = float4(NextE, bForce ? 1.0 : 0.0);
#if SB_LUMP_MODES > 0
		for (uint Quad3 = 0u; Quad3 < 4u; ++Quad3)
		{
			uint Q1 = 4u * Quad3;
			LumpState[Row + 14u + Quad3] = bForce ? float4(SB_SolveNext[Q1], SB_SolveNext[Q1 + 1u], SB_SolveNext[Q1 + 2u], SB_SolveNext[Q1 + 3u])
				: float4(0, 0, 0, 0);
		}
#else
		// 국소 모양이 없는 판(일지 254): 지난 결과 q와 M을 비운다 — 국소 모양을 켜는 프레임(같은 장이 큰 판으로 바뀜)이 q = 0에서 이어 풀고,
		// 국소 모양 패스가 오래된 M을 쓰지 않게.
		for (uint Clear = 14u; Clear < SB_LUMP_STATE_ROWS; ++Clear)
		{
			LumpState[Row + Clear] = float4(0, 0, 0, 0);
		}
#endif
	}
	// 이 묶음의 칸 합은 다 읽었다: 다음 걸음의 LumpForceCS가 0에서 더한다.
	LOOP
	for (uint Word = LocalIndex; Word < SB_LUMP_VALUES * 2u; Word += SB_LUMP_SOLVE_THREADS)
	{
		LumpSumsOut[Group * SB_LUMP_VALUES * 2u + Word] = 0u;
	}
}

// --- 국소 모양 패스 (일지 251) ---
RWTexture2D<float4> LumpRegionOffsetOut;

/**
 * 거친 칸마다 국소 모양이 지금 옮긴 양(xyz 프레임 cm, w 1 = 줄이 있다)을 적는다 — 이번 갱신의 접촉 패스(광선 출발점)와 합치기(변위)가
 * 쌍선형으로 읽는다(SB_RegionOffsetAt). 모양 크기 q(t) = 지난 덩어리 평형의 q + M·(L(t) − E): L(t) = 지금 그리는 출렁임(브릿지의 선형 몫 ÷
 * 흔들림 크기). 누른 채 출렁일 때 국소 모양이 출렁임을 거슬러 움직여 누른 자리 둘레는 붙들리고 먼 곳은 출렁인다. 지난 풀이에 힘이 없었으면 0.
 * 거친 격자 전체를 돈다(줄 없는 칸은 0을 쓴다).
 */
[numthreads(8, 8, 1)]
void LumpRegionCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= uint2(CoarseResolution, CoarseResolution)))
	{
		return;
	}
	float4 Out = float4(0, 0, 0, 0);
	int Record = SB_RegionRecord(int2(Id.xy));
	if (Record >= 0 && CoarseInfo.Load(int3(Id.xy, 0)).w > 0.0)
	{
		int Group = (int)round(SB_RegionTexel(Record, 12).r);
		if (Group >= 0 && Group < SB_LUMP_GROUPS)
		{
			Out.w = 1.0;
			uint Row = (uint)Group * SB_LUMP_STATE_ROWS;
			float4 Last = LumpIterate[Row + 2u];
			if (Last.w > 0.5)
			{
				int Count = (int)round(InertiaBridge[0].w);
				int BridgeGroup = Count > 0 ? Group : -1;
				float A = max(LumpGroups[(uint)Group * SB_LUMP_GROUP_ROWS + 1u].w, 1e-3);
				float3 Gap = SB_BridgeEntry(BridgeGroup, 0).xyz / A - Last.xyz;
				float3 Rest = float3(0, 0, 0);
				UNROLL
				for (int Quad = 0; Quad < 4; ++Quad)
				{
					float4 Q = LumpIterate[Row + 14u + (uint)Quad] + LumpIterate[Row + 18u + (uint)Quad] * Gap.x
						+ LumpIterate[Row + 22u + (uint)Quad] * Gap.y + LumpIterate[Row + 26u + (uint)Quad] * Gap.z;
					Rest += float3(dot(SB_RegionTexel(Record, Quad), Q), dot(SB_RegionTexel(Record, 4 + Quad), Q), dot(SB_RegionTexel(Record, 8 + Quad), Q));
				}
				float4 Rotation = SB_BridgeEntry(BridgeGroup, 2);
				Out.xyz = A * SB_InverseRotate(ComponentRotation, SB_Rotate(Rotation, Rest));
			}
		}
	}
	LumpRegionOffsetOut[Id.xy] = Out;
}

// --- 이완 묶음 목록 (ADR-0013 2단계, §8) ---
// 묶음을 켜는 조건: 이번 프레임 눌린 칸이 있거나(TilePressed) 지난 프레임 끝에 상태가 남았다(TileResidual). 켜진 묶음의 둘레
// TileReach겹과, 고스트 칸이 그 근처를 가리키는 묶음(이음새 건너편)과 그 둘레가 목록에 든다.
// 묶음마다 이음새 쌍을 적는 칸 수(C++ TileLinksPerTile — 측정 최대와 근거는 그 주석).
#define SB_TILE_LINKS 32
#define SB_TILE_NO_LINK 0xffffffffu
// 고스트 칸이 가리키는 묶음이 SB_TILE_LINKS개를 넘는 묶음: 목록 패스가 돌 때(어디든 눌렸거나 상태가 남았을 때) 늘 켠다.
#define SB_TILE_ALWAYS 0xffffu
RWBuffer<uint> TileLinkPairsOut;     // 이음새 쌍: 묶음 | 고스트 칸이 값을 가져오는 다른 묶음 << 16 (SB_TILE_ALWAYS = 늘 켬)
RWBuffer<uint> TileLinkPairCountOut; // [0] 쌍 수 (0으로 비운 뒤 센다)
Buffer<uint> TileLinkPairs;
Buffer<uint> TileLinkPairCount;
Buffer<uint> TilePressed;            // ContactCS의 ActivityOut — TilePressedBase부터 이완 묶음마다 1 = 이번 프레임 눌린 칸이 있다
Buffer<uint> TileResidual;           // 지난 갱신의 TileResidualOut(묶음마다 SB_RESIDUAL_* 비트)
// TileCountOut([0] 함몰 묶음 수, [1] 0으로 비울 묶음 수 — ActivityArgsCS가 0으로 비운 뒤 TileListCS가 쓴다, [2] 끌림이 닿은 거친 묶음 수)은 끌림 패스 쪽에 선언했다.
RWBuffer<uint> TileListOut;          // 함몰 묶음 목록 (x | y << 16): 푸는 묶음, 그 뒤 비울 묶음. DentCoarseListBase부터 함몰 거친 묶음 목록(일지 199)
RWBuffer<uint> TileBitsOut;          // 함몰 목록 비트(판 0의 낱말 그대로). 함몰 창이 목록 밖 묶음을 0으로 읽는 데 쓴다(ADR-0022)
int TileReach;                       // 함몰 묶음을 둘레 몇 겹까지 켤지(C++ DentTileReach — 함몰 창이 묶음 둘레 반 묶음까지 푼다, ADR-0032)
uint ArgsStride;                     // 간접 인자 한 벌의 uint 수(플랫폼 패딩 포함)
uint bDentParts;                     // 1 = 이 장의 함몰이 부위 나누기 순열을 쓴다(C++ F.bSeparation — 함몰 패스의 bSeparation은 솟음 채널 읽기라 이름을 가른다)
uint bTileStats;                     // 1 = Softbody.Stats가 읽는 갱신: 목록 까닭별 수를 센다(SB_TILE_COUNT_WHY, 검토 R03)
groupshared uint SB_TileLinkSlots[SB_TILE_LINKS];
groupshared uint SB_TileLinkOverflow;
// 부위 나누기(검토 R03 E): 묶음마다 1비트 — 이 묶음에 함몰 채널에 몫을 낼 수 있는 칸이 있다(SB_PartReachAt). 이음새 쌍과 같이 처음 한 번 쓴다(0으로
// 비운 뒤). 목록 패스가 한 겹 넓혀 채널 순열로 풀 묶음을 고른다.
RWBuffer<uint> TilePartsOut;
Buffer<uint> TileParts;
groupshared uint SB_TilePartFlag;

/** 칸에 부위 몫이 있는가(s_a + s_b > 0 — SB_SeparationCorrection·SB_SeparationWeights가 0이 아닐 수 있는 칸과 같은 문턱). */
bool SB_PartShareAt(int2 Texel)
{
	Texel = clamp(Texel, int2(0, 0), int2(Resolution - 1, Resolution - 1));
	float2 S = SB_SeparationAt(Texel);
	return S.x + S.y > 1e-6;
}

/**
 * 칸이 함몰 채널에 몫을 낼 수 있는가: 살 칸(표면·고스트)이 제 부위 몫을 가졌거나(첫 패스 채널 출발 값·출처 무게, 끝의 보정), 고스트 칸이 둘째 패스에서
 * 출발 값을 따라 읽는 칸(SB_SeparationCurrent와 같은 사슬: 가리키는 점 둘레 네 칸, 그 칸이 다시 고스트면 그 점 둘레 네 칸)에 몫이 있다. 아니면 그
 * 칸이 읽고 쓰는 보정은 늘 0이다.
 */
bool SB_PartReachAt(int2 Texel)
{
	uint Kind = SB_KindAt(Texel);
	if (!SB_IsCarried(Kind))
	{
		return false;
	}
	bool bPart = SB_PartShareAt(Texel);
	if (Kind == SB_KIND_GHOST)
	{
		int2 Base = (int2)floor(CarrierRedirect.Load(int3(Texel, 0)).xy);
		for (int Tap = 0; Tap < 4; ++Tap)
		{
			int2 Near = clamp(Base + int2(Tap & 1, Tap >> 1), int2(0, 0), int2(Resolution - 1, Resolution - 1));
			if (SB_KindAt(Near) == SB_KIND_GHOST)
			{
				int2 Base2 = (int2)floor(CarrierRedirect.Load(int3(Near, 0)).xy);
				for (int Tap2 = 0; Tap2 < 4; ++Tap2)
				{
					bPart = bPart || SB_PartShareAt(Base2 + int2(Tap2 & 1, Tap2 >> 1));
				}
			}
			else
			{
				bPart = bPart || SB_PartShareAt(Near);
			}
		}
	}
	return bPart;
}

/**
 * 처음 한 번(캐리어 고스트 표가 바뀌면 다시): 묶음마다 고스트 칸이 값을 가져오는 다른 묶음들을 이음새 쌍으로 적는다(묶음 하나 =
 * 스레드 묶음 하나). 넘치는 묶음은 늘 켬 쌍 하나만 적는다. 부위 나누기가 구워진 캐리어는 묶음마다 부위 몫 비트도 적는다(TilePartsOut).
 */
[numthreads(SB_TILE, SB_TILE, 1)]
void TileLinksCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID, uint LocalIndex : SV_GroupIndex)
{
	if (LocalIndex < SB_TILE_LINKS)
	{
		SB_TileLinkSlots[LocalIndex] = SB_TILE_NO_LINK;
	}
	if (LocalIndex == 0u)
	{
		SB_TileLinkOverflow = 0u;
		SB_TilePartFlag = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	uint Self = GroupId.y * TilesPerSide + GroupId.x;
	int2 T = int2(GroupId.xy * SB_TILE + LocalId.xy);
	// 부위 나누기가 없는 캐리어는 읽지 않는다(FXC는 &&의 두 쪽을 다 셈하므로 갈래로 가른다).
	BRANCH
	if (bCarrierSeparation != 0u && all(T < int2(Resolution, Resolution)))
	{
		if (SB_PartReachAt(T))
		{
			InterlockedOr(SB_TilePartFlag, 1u);
		}
	}
	if (all(T < int2(Resolution, Resolution)) && SB_KindAt(T) == SB_KIND_GHOST)
	{
		uint2 Target = (uint2)clamp(round(CarrierRedirect.Load(int3(T, 0)).xy), 0.0, (float)(Resolution - 1u));
		uint2 TargetTile = Target / SB_TILE;
		uint Link = TargetTile.y * TilesPerSide + TargetTile.x;
		if (Link != Self)
		{
			// 빈 칸에 넣거나 이미 있으면 그만. 칸은 앞에서부터 채워진다.
			bool bStored = false;
			LOOP
			for (uint Slot = 0; Slot < SB_TILE_LINKS && !bStored; ++Slot)
			{
				uint Previous;
				InterlockedCompareExchange(SB_TileLinkSlots[Slot], SB_TILE_NO_LINK, Link, Previous);
				bStored = Previous == SB_TILE_NO_LINK || Previous == Link;
			}
			if (!bStored)
			{
				InterlockedOr(SB_TileLinkOverflow, 1u);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (LocalIndex < SB_TILE_LINKS)
	{
		uint Link = SB_TileLinkSlots[LocalIndex];
		if (SB_TileLinkOverflow != 0u)
		{
			Link = LocalIndex == 0u ? SB_TILE_ALWAYS : SB_TILE_NO_LINK;
		}
		if (Link != SB_TILE_NO_LINK)
		{
			uint Slot;
			InterlockedAdd(TileLinkPairCountOut[0], 1u, Slot);
			TileLinkPairsOut[Slot] = Self | (Link << 16u);
		}
	}
	if (LocalIndex == 0u && SB_TilePartFlag != 0u)
	{
		InterlockedOr(TilePartsOut[Self >> 5u], 1u << (Self & 31u));
	}
}

// --- 이완 묶음 목록 (ADR-0013 §8) ---
// 스레드 묶음 하나(SB_LIST_THREADS)가 묶음마다 비트 하나씩 공유 메모리에 올려 넓히기를 낱말 단위 비트 연산으로 한다. 전에는
// 묶음마다 스레드 하나가 둘레와 이음새 건너편 둘레를 수백 번 차례로 읽어(TileLinkedCS·TileListCS) 일이 없어도 수십 µs가 들었다.
//   켜짐 F = 눌림 | 붙든 자리가 남음(SB_RESIDUAL_HOLD)
//   함몰 목록 = 넓힘(F ∪ {가리키는 묶음이 넓힘(F, R) 안인 묶음}, R)                 R = TileReach(1)
//   비울 목록 = 함몰이 남았는데(SB_RESIDUAL_DENT) 함몰 목록 밖인 묶음 — 옮기기 패스가 0으로 비운다
// 넓힘(X, R) = X에서 가로·세로 R겹(체비쇼프 거리) 안. 붙든 자리가 아니라 함몰 값으로 켜면(전 모델) 꼬리가 갱신마다 목록을 한 겹씩 넓혔다(일지 172).
// 솟음 내리기(ADR-0031)는 같은 함몰 목록을 돈다(옮긴 뒤 함몰이 0이 아닌 칸은 모두 그 안).
#define SB_LIST_THREADS 1024
/** 묶음 비트 판 하나의 낱말 수 상한: 캐리어 2048² = 묶음 128² = 16384개 / 32. */
#define SB_TILE_WORDS_MAX 512
/** 판 네 장: 0 = 켜짐 → 목록, 1 = 넓히는 중간, 2 = 넓힌 결과, 3 = 이음새 건너편을 더한 것. */
groupshared uint SB_TileBits[4 * SB_TILE_WORDS_MAX];
groupshared uint SB_TileListCount;
groupshared uint SB_ZeroListCount;
groupshared uint SB_CoarseListCount;
// 목록 패스가 목록 길이로 채우는 간접 디스패치 인자 두 벌(ActivityArgsCS가 먼저 0으로 비운다). 한 버퍼에 모은 까닭: D3D11은 패스
// 하나가 쓰는 버퍼·텍스처를 8개까지만 받는다.
#define SB_LIST_ARGS_FINE 0u         // DentCS·DentCoarseAddCS(함몰 묶음마다 스레드 묶음 하나)
#define SB_LIST_ARGS_COPY 1u         // DentCopyCS 옮기기(함몰·비울 묶음마다 하나)
#define SB_LIST_ARGS_COARSE 2u       // DentCoarseRestrictCS·DentCoarseCS(거친 묶음마다 하나, 일지 199)
#define SB_LIST_ARGS_PARTS 3u        // 부위 나누기 장의 DentCS 채널 순열(함몰 목록 앞쪽 부위 묶음마다 하나, 검토 R03 E)
#define SB_LIST_ARGS_SINGLE 4u       // 부위 나누기 장의 DentCS 채널 없는 순열(그 뒤 나머지 묶음마다 하나)
RWBuffer<uint> TileListArgsOut;

uint SB_TileBit(uint Plane, uint Tile)
{
	return (SB_TileBits[Plane * SB_TILE_WORDS_MAX + (Tile >> 5u)] >> (Tile & 31u)) & 1u;
}

/** 한 변 W묶음인 판의 낱말 수. */
uint SB_TileWords(uint W)
{
	return (W * W + 31u) / 32u;
}

/**
 * 판 Src(한 변 W묶음)를 가로·세로 Reach겹(체비쇼프 거리) 넓혀 판 Dst에 둔다. Tmp는 가로만 넓힌 중간. 모든 스레드가 부른다(장벽 둘).
 */
void SB_DilateTiles(uint Src, uint Tmp, uint Dst, uint W, int Reach, uint LocalIndex)
{
	uint Words = SB_TileWords(W);
	int R = clamp(Reach, 0, 31);
	// 한 줄이 낱말 경계에 맞으면(묶음 32개 이상 = 캐리어 512² 이상) 낱말째 밀어 넓힌다. 더 작은 캐리어는 묶음마다 센다.
	bool bAligned = (W & 31u) == 0u;
	uint RowWords = max(W >> 5u, 1u);
	for (uint Word = LocalIndex; Word < Words; Word += SB_LIST_THREADS)
	{
		uint Bits = 0u;
		if (bAligned)
		{
			// 비트 b = 줄 안 묶음 (낱말 번호 × 32 + b). x − s의 값이 x로(왼쪽 밀기), x + s의 값이 x로(오른쪽 밀기) 온다.
			uint Column = Word % RowWords;
			uint Here = SB_TileBits[Src * SB_TILE_WORDS_MAX + Word];
			uint Before = Column > 0u ? SB_TileBits[Src * SB_TILE_WORDS_MAX + Word - 1u] : 0u;
			uint After = Column + 1u < RowWords ? SB_TileBits[Src * SB_TILE_WORDS_MAX + Word + 1u] : 0u;
			Bits = Here;
			for (int Shift = 1; Shift <= R; ++Shift)
			{
				Bits |= (Here << Shift) | (Before >> (32 - Shift)) | (Here >> Shift) | (After << (32 - Shift));
			}
		}
		else
		{
			for (uint Bit = 0u; Bit < 32u; ++Bit)
			{
				uint Tile = Word * 32u + Bit;
				if (Tile >= W * W)
				{
					break;
				}
				int X = (int)(Tile % W);
				uint RowStart = Tile - (uint)X;
				uint On = 0u;
				for (int D = max(-R, -X); D <= min(R, (int)W - 1 - X); ++D)
				{
					On |= SB_TileBit(Src, RowStart + (uint)(X + D));
				}
				Bits |= On << Bit;
			}
		}
		SB_TileBits[Tmp * SB_TILE_WORDS_MAX + Word] = Bits;
	}
	GroupMemoryBarrierWithGroupSync();
	for (uint Word2 = LocalIndex; Word2 < Words; Word2 += SB_LIST_THREADS)
	{
		uint Bits = 0u;
		if (bAligned)
		{
			int Row = (int)(Word2 / RowWords);
			uint Column = Word2 % RowWords;
			for (int D = max(-R, -Row); D <= min(R, (int)W - 1 - Row); ++D)
			{
				Bits |= SB_TileBits[Tmp * SB_TILE_WORDS_MAX + (uint)(Row + D) * RowWords + Column];
			}
		}
		else
		{
			for (uint Bit = 0u; Bit < 32u; ++Bit)
			{
				uint Tile = Word2 * 32u + Bit;
				if (Tile >= W * W)
				{
					break;
				}
				int X = (int)(Tile % W);
				int Y = (int)(Tile / W);
				uint On = 0u;
				for (int D = max(-R, -Y); D <= min(R, (int)W - 1 - Y); ++D)
				{
					On |= SB_TileBit(Tmp, (uint)(Y + D) * W + (uint)X);
				}
				Bits |= On << Bit;
			}
		}
		SB_TileBits[Dst * SB_TILE_WORDS_MAX + Word2] = Bits;
	}
	GroupMemoryBarrierWithGroupSync();
}

/** 가는 묶음 번호 → 한 변 W묶음 판의 번호(가는 묶음 Span × Span을 한 묶음으로). */
uint SB_TileIn(uint Tile, uint W, uint Span)
{
	return (Tile / TilesPerSide / Span) * W + (Tile % TilesPerSide) / Span;
}

/**
 * 판 Dst = 판 Src ∪ {이음새 쌍에서 가리키는 묶음이 판 Near에 켜진 묶음, 늘 켬 묶음}. 판은 한 변 W묶음이고, 이음새 쌍의 가는 묶음은
 * Span × Span씩 판의 묶음 하나로 본다(이완 목록은 1). 모든 스레드가 부른다(장벽 둘).
 */
void SB_AddLinkedTiles(uint Src, uint Near, uint Dst, uint W, uint Span, uint LocalIndex)
{
	uint Words = SB_TileWords(W);
	for (uint Word = LocalIndex; Word < Words; Word += SB_LIST_THREADS)
	{
		SB_TileBits[Dst * SB_TILE_WORDS_MAX + Word] = SB_TileBits[Src * SB_TILE_WORDS_MAX + Word];
	}
	GroupMemoryBarrierWithGroupSync();
	uint Pairs = TileLinkPairCount[0];
	for (uint Pair = LocalIndex; Pair < Pairs; Pair += SB_LIST_THREADS)
	{
		uint Packed = TileLinkPairs[Pair];
		uint Tile = SB_TileIn(Packed & 0xffffu, W, Span);
		uint Link = Packed >> 16u;
		if (Link == SB_TILE_ALWAYS || SB_TileBit(Near, SB_TileIn(Link, W, Span)) != 0u)
		{
			InterlockedOr(SB_TileBits[Dst * SB_TILE_WORDS_MAX + (Tile >> 5u)], 1u << (Tile & 31u));
		}
	}
	GroupMemoryBarrierWithGroupSync();
}

/** 목록 패스 인자 한 벌(SB_LIST_ARGS_*, 한 줄 Row 묶음): 목록 길이 Count. */
void SB_WriteTileListArgs(uint Slot, uint Count, uint Row)
{
	uint Base = Slot * ArgsStride;
	TileListArgsOut[Base + 0] = min(Count, Row);
	TileListArgsOut[Base + 1] = (Count + Row - 1u) / Row;
	TileListArgsOut[Base + 2] = 1u;
}

/** 어디든 눌렸거나 지난 갱신 끝에 상태가 남은 갱신에만 돈다(ActivityArgsCS 인자 SB_ARGS_TILES, 스레드 묶음 하나). 솟음·끌림은 목록 없이 거친 격자 전체를 돈다(ADR-0031·ADR-0033). */
[numthreads(SB_LIST_THREADS, 1, 1)]
void TileListCS(uint LocalIndex : SV_GroupIndex)
{
	uint W = TilesPerSide;
	uint NumTiles = W * W;
	uint Words = SB_TileWords(W);
	for (uint Word = LocalIndex; Word < Words; Word += SB_LIST_THREADS)
	{
		SB_TileBits[Word] = 0u;
	}
	if (LocalIndex == 0u)
	{
		SB_TileListCount = 0u;
		SB_ZeroListCount = 0u;
		SB_CoarseListCount = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	for (uint Tile = LocalIndex; Tile < NumTiles; Tile += SB_LIST_THREADS)
	{
		if ((TilePressed[TilePressedBase + Tile] | (TileResidual[Tile] & SB_RESIDUAL_HOLD)) != 0u)
		{
			InterlockedOr(SB_TileBits[Tile >> 5u], 1u << (Tile & 31u));
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// 함몰 목록: 판 0(켜짐) → 판 2 = 넓힘(켜짐, R) → 판 3 = 켜짐 ∪ 이음새 건너편 → 판 0 = 넓힘(판 3, R).
	SB_DilateTiles(0u, 1u, 2u, W, TileReach, LocalIndex);
	SB_AddLinkedTiles(0u, 2u, 3u, W, 1u, LocalIndex);
	SB_DilateTiles(3u, 1u, 0u, W, TileReach, LocalIndex);
	// Softbody.Stats만(검토 R03): 하드 서피스 조각을 더하기 전 목록 수. 판 0을 읽기만 한다 — 다음 장벽 전에는 아무도 판 0을 쓰지 않는다.
	BRANCH
	if (bTileStats != 0u)
	{
		uint StatCore = 0u;
		for (uint StatCoreWord = LocalIndex; StatCoreWord < Words; StatCoreWord += SB_LIST_THREADS)
		{
			StatCore += countbits(SB_TileBits[StatCoreWord]);
		}
		if (StatCore != 0u)
		{
			InterlockedAdd(TileCountOut[SB_TILE_COUNT_WHY + 4u], StatCore);
		}
	}
#if SB_HARD_SURFACE
	// 하드 서피스(ADR-0037 §3.7 ⑥): 조각의 묶음이 하나라도 함몰 목록에 들면 그 조각 묶음 전부를 넣는다 — 판 전체가 한 몸으로 붙들려야 한다. 판 0을 판 2에
	// 베껴 두고 그것을 읽어(조각끼리 서로 켜는 차례가 실행마다 달라지지 않게) 판 0에 쓴다.
	for (uint CopyWord = LocalIndex; CopyWord < Words; CopyWord += SB_LIST_THREADS)
	{
		SB_TileBits[2u * SB_TILE_WORDS_MAX + CopyWord] = SB_TileBits[CopyWord];
	}
	GroupMemoryBarrierWithGroupSync();
	for (uint PieceIndex = LocalIndex; PieceIndex < HardPieceCount; PieceIndex += SB_LIST_THREADS)
	{
		uint2 Range = HardPieceTileRange[PieceIndex];
		bool bAnyOn = false;
		// 회색 조각은 판이 아니라(연속 굽힘) 묶음을 통째로 넣지 않는다
		Range.y = SB_HardKindOf(PieceIndex) != 0u ? Range.y : 0u;
		for (uint k = 0u; k < Range.y && !bAnyOn; ++k)
		{
			uint Packed = HardPieceTileList[Range.x + k];
			bAnyOn = SB_TileBit(2u, (Packed >> 16u) * W + (Packed & 0xffffu)) != 0u;
		}
		for (uint k2 = 0u; k2 < Range.y && bAnyOn; ++k2)
		{
			uint Packed = HardPieceTileList[Range.x + k2];
			uint Tile = (Packed >> 16u) * W + (Packed & 0xffffu);
			InterlockedOr(SB_TileBits[Tile >> 5u], 1u << (Tile & 31u));
		}
	}
	GroupMemoryBarrierWithGroupSync();
#endif
	// 부위 나누기(검토 R03 E): 함몰 채널이 0이 아닐 수 있는 묶음 = 넓힘(부위 묶음, 1). 부위 묶음(TileParts — TileLinksCS가 처음 한 번)은 부위 몫이 있는
	// 살 칸이나 그런 칸을 따라 읽는 고스트 칸이 든 묶음이고, 함몰 창은 묶음 둘레 8칸이라 이웃 묶음 하나 안이다. 그 밖의 묶음은 창 안 모든 칸의 출처 무게가
	// 0이라 채널이 늘 0이고 보정도 0이다 — 함몰 반복(U)은 채널을 읽지 않으므로 채널 없는 순열과 결과가 같다. 그래서 그 묶음들을 목록 앞에 두어 채널
	// 순열로 풀고(인자 SB_LIST_ARGS_PARTS) 나머지는 채널 없는 순열로 푼다(SB_LIST_ARGS_SINGLE). 판 1 = 목록 ∩ 넓힌 부위 묶음 — 목록 비트 뒤쪽 절반에
	// 남긴다(채널 순열의 둘째 패스가 그 밖 묶음의 채널을 0으로 읽는다). 판 3 = 나머지 목록. 부위 나누기가 없는 장은 판 0을 그대로 쓴다.
	uint PartsCount = 0u;
	uint RestPlane = 0u;
	BRANCH
	if (bDentParts != 0u)
	{
		for (uint PartWord = LocalIndex; PartWord < Words; PartWord += SB_LIST_THREADS)
		{
			SB_TileBits[3u * SB_TILE_WORDS_MAX + PartWord] = TileParts[PartWord];
		}
		GroupMemoryBarrierWithGroupSync();
		SB_DilateTiles(3u, 1u, 2u, W, 1, LocalIndex);
		for (uint PartWord2 = LocalIndex; PartWord2 < Words; PartWord2 += SB_LIST_THREADS)
		{
			uint PartListed = SB_TileBits[PartWord2];
			uint PartReach = SB_TileBits[2u * SB_TILE_WORDS_MAX + PartWord2];
			SB_TileBits[SB_TILE_WORDS_MAX + PartWord2] = PartListed & PartReach;
			SB_TileBits[3u * SB_TILE_WORDS_MAX + PartWord2] = PartListed & ~PartReach;
			TileBitsOut[Words + PartWord2] = PartListed & PartReach;
		}
		GroupMemoryBarrierWithGroupSync();
		for (uint PartTile = LocalIndex; PartTile < NumTiles; PartTile += SB_LIST_THREADS)
		{
			if (SB_TileBit(1u, PartTile) != 0u)
			{
				uint Slot;
				InterlockedAdd(SB_TileListCount, 1u, Slot);
				TileListOut[Slot] = (PartTile % W) | ((PartTile / W) << 16u);
			}
		}
		GroupMemoryBarrierWithGroupSync();
		PartsCount = SB_TileListCount;
		RestPlane = 3u;
		GroupMemoryBarrierWithGroupSync();
	}
	for (uint Listed = LocalIndex; Listed < NumTiles; Listed += SB_LIST_THREADS)
	{
		if (SB_TileBit(RestPlane, Listed) != 0u)
		{
			uint Slot;
			InterlockedAdd(SB_TileListCount, 1u, Slot);
			TileListOut[Slot] = (Listed % W) | ((Listed / W) << 16u);
		}
	}
	// 목록 비트(판 0)를 버퍼에 남긴다: 함몰 창이 이웃으로 읽는 목록 밖 묶음을 0으로 보는 데 쓴다(ADR-0022).
	for (uint BitWord = LocalIndex; BitWord < Words; BitWord += SB_LIST_THREADS)
	{
		TileBitsOut[BitWord] = SB_TileBits[BitWord];
	}
	GroupMemoryBarrierWithGroupSync();
	// 비울 목록(ADR-0032): 함몰이 남았는데 함몰 목록에 들지 않은 묶음 — 붙든 자리가 둘레 1겹 밖으로 물러났다. 함몰 목록 바로 뒤에 둔다.
	for (uint Stale = LocalIndex; Stale < NumTiles; Stale += SB_LIST_THREADS)
	{
		if ((TileResidual[Stale] & SB_RESIDUAL_DENT) != 0u && SB_TileBit(0u, Stale) == 0u)
		{
			uint Slot;
			InterlockedAdd(SB_ZeroListCount, 1u, Slot);
			TileListOut[SB_TileListCount + Slot] = (Stale % W) | ((Stale / W) << 16u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	// Softbody.Stats만(검토 R03): 목록이 왜 이만큼인지 까닭별로 센다 — 목록 규칙과 같은 켜짐·넓힘·이음새 쌍에서 다시 센다(목록은 건드리지 않는다).
	// 판 1 = 켜짐 → 이음새로 든 묶음, 판 2 = 켜짐 둘레, 판 3 = 넓히는 중간 → 늘 켬 묶음. 아래 거친 목록이 판 1~3을 다시 비우고 쓴다.
	BRANCH
	if (bTileStats != 0u)
	{
		for (uint StatClear = LocalIndex; StatClear < Words; StatClear += SB_LIST_THREADS)
		{
			SB_TileBits[SB_TILE_WORDS_MAX + StatClear] = 0u;
		}
		GroupMemoryBarrierWithGroupSync();
		for (uint StatTile = LocalIndex; StatTile < NumTiles; StatTile += SB_LIST_THREADS)
		{
			if ((TilePressed[TilePressedBase + StatTile] | (TileResidual[StatTile] & SB_RESIDUAL_HOLD)) != 0u)
			{
				InterlockedOr(SB_TileBits[SB_TILE_WORDS_MAX + (StatTile >> 5u)], 1u << (StatTile & 31u));
			}
		}
		GroupMemoryBarrierWithGroupSync();
		SB_DilateTiles(1u, 3u, 2u, W, TileReach, LocalIndex);
		uint StatOn = 0u;
		uint StatRing = 0u;
		for (uint StatWord = LocalIndex; StatWord < Words; StatWord += SB_LIST_THREADS)
		{
			uint StatOnBits = SB_TileBits[SB_TILE_WORDS_MAX + StatWord];
			StatOn += countbits(StatOnBits);
			StatRing += countbits(SB_TileBits[2u * SB_TILE_WORDS_MAX + StatWord] & ~StatOnBits);
		}
		GroupMemoryBarrierWithGroupSync();
		for (uint StatClear2 = LocalIndex; StatClear2 < Words; StatClear2 += SB_LIST_THREADS)
		{
			SB_TileBits[SB_TILE_WORDS_MAX + StatClear2] = 0u;
			SB_TileBits[3u * SB_TILE_WORDS_MAX + StatClear2] = 0u;
		}
		GroupMemoryBarrierWithGroupSync();
		uint StatPairs = TileLinkPairCount[0];
		for (uint StatPair = LocalIndex; StatPair < StatPairs; StatPair += SB_LIST_THREADS)
		{
			uint StatPacked = TileLinkPairs[StatPair];
			uint StatSelf = StatPacked & 0xffffu;
			uint StatLink = StatPacked >> 16u;
			if (StatLink == SB_TILE_ALWAYS)
			{
				InterlockedOr(SB_TileBits[3u * SB_TILE_WORDS_MAX + (StatSelf >> 5u)], 1u << (StatSelf & 31u));
			}
			else if (SB_TileBit(2u, StatLink == SB_TILE_ALWAYS ? 0u : StatLink) != 0u)   // FXC가 두 쪽을 다 셈해도 판 밖을 읽지 않게
			{
				InterlockedOr(SB_TileBits[SB_TILE_WORDS_MAX + (StatSelf >> 5u)], 1u << (StatSelf & 31u));
			}
		}
		GroupMemoryBarrierWithGroupSync();
		uint StatLinked = 0u;
		uint StatAlways = 0u;
		for (uint StatWord2 = LocalIndex; StatWord2 < Words; StatWord2 += SB_LIST_THREADS)
		{
			uint StatNearBits = SB_TileBits[2u * SB_TILE_WORDS_MAX + StatWord2];
			uint StatLinkedBits = SB_TileBits[SB_TILE_WORDS_MAX + StatWord2];
			StatLinked += countbits(StatLinkedBits & ~StatNearBits);
			StatAlways += countbits(SB_TileBits[3u * SB_TILE_WORDS_MAX + StatWord2] & ~StatNearBits & ~StatLinkedBits);
		}
		if (StatOn != 0u)
		{
			InterlockedAdd(TileCountOut[SB_TILE_COUNT_WHY], StatOn);
		}
		if (StatRing != 0u)
		{
			InterlockedAdd(TileCountOut[SB_TILE_COUNT_WHY + 1u], StatRing);
		}
		if (StatLinked != 0u)
		{
			InterlockedAdd(TileCountOut[SB_TILE_COUNT_WHY + 2u], StatLinked);
		}
		if (StatAlways != 0u)
		{
			InterlockedAdd(TileCountOut[SB_TILE_COUNT_WHY + 3u], StatAlways);
		}
		if (LocalIndex == 0u)
		{
			TileCountOut[SB_TILE_COUNT_WHY + 5u] = bDentParts != 0u ? 1u : 0u;
		}
		GroupMemoryBarrierWithGroupSync();
	}
	// 거친 목록(일지 199): 함몰 거친 해를 푸는 거친 묶음. 씨앗 = 가는 목록 묶음이 든 거친 묶음 ∪ 지난 갱신 끝에 꼬리가 SB_COARSE_TAU를 넘은 거친 묶음.
	// 씨앗과 이음새 건너편을 한 겹 넓히고, 거친 해가 0이 아닌 묶음을 더한다 — 0까지 잦아든 뒤에 빠지므로 빠진 묶음은 늘 0이다(비울 필요가 없다).
	// 판 0(가는 목록)은 위에서 다 썼으므로 판 1~3을 쓴다. DentCoarseOn은 상수라 장벽이 든 분기도 묶음 전체가 같다.
	if (DentCoarseOn != 0u)
	{
		uint Wc = (CoarseResolution + SB_TILE - 1u) / SB_TILE;
		uint WordsC = SB_TileWords(Wc);
		for (uint Word = LocalIndex; Word < WordsC; Word += SB_LIST_THREADS)
		{
			SB_TileBits[SB_TILE_WORDS_MAX + Word] = 0u;
		}
		GroupMemoryBarrierWithGroupSync();
		for (uint Fine = LocalIndex; Fine < NumTiles; Fine += SB_LIST_THREADS)
		{
			if (SB_TileBit(0u, Fine) != 0u)
			{
				uint Seed = SB_TileIn(Fine, Wc, CoarseBlock);
				InterlockedOr(SB_TileBits[SB_TILE_WORDS_MAX + (Seed >> 5u)], 1u << (Seed & 31u));
			}
		}
		for (uint Wide = LocalIndex; Wide < Wc * Wc; Wide += SB_LIST_THREADS)
		{
			if ((DentCoarseBusy[Wide] & SB_COARSE_BUSY_WIDE) != 0u)
			{
				InterlockedOr(SB_TileBits[SB_TILE_WORDS_MAX + (Wide >> 5u)], 1u << (Wide & 31u));
			}
		}
		GroupMemoryBarrierWithGroupSync();
		SB_AddLinkedTiles(1u, 1u, 2u, Wc, CoarseBlock, LocalIndex);
		SB_DilateTiles(2u, 3u, 1u, Wc, 1, LocalIndex);
		for (uint Any = LocalIndex; Any < Wc * Wc; Any += SB_LIST_THREADS)
		{
			if ((DentCoarseBusy[Any] & SB_COARSE_BUSY_ANY) != 0u)
			{
				InterlockedOr(SB_TileBits[SB_TILE_WORDS_MAX + (Any >> 5u)], 1u << (Any & 31u));
			}
		}
		GroupMemoryBarrierWithGroupSync();
		for (uint Coarse = LocalIndex; Coarse < Wc * Wc; Coarse += SB_LIST_THREADS)
		{
			if (SB_TileBit(1u, Coarse) != 0u)
			{
				uint Slot;
				InterlockedAdd(SB_CoarseListCount, 1u, Slot);
				TileListOut[DentCoarseListBase + 1u + Slot] = (Coarse % Wc) | ((Coarse / Wc) << 16u);
			}
		}
		for (uint CoarseWord = LocalIndex; CoarseWord < WordsC; CoarseWord += SB_LIST_THREADS)
		{
			TileListOut[DentCoarseListBase + 1u + Wc * Wc + CoarseWord] = SB_TileBits[SB_TILE_WORDS_MAX + CoarseWord];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (LocalIndex == 0u)
	{
		uint Count = SB_TileListCount;
		TileCountOut[0] = Count;
		TileCountOut[1] = SB_ZeroListCount;
		// 함몰(DentCS)·접촉 압력 칸(ADR-0037): 함몰 묶음마다 스레드 묶음 하나.
		SB_WriteTileListArgs(SB_LIST_ARGS_FINE, Count, SB_TILE_ROW);
		// 옮기기(DentCopyCS): 함몰·비울 묶음마다 하나.
		SB_WriteTileListArgs(SB_LIST_ARGS_COPY, Count + SB_ZeroListCount, SB_TILE_ROW);
		// 거친 해(일지 199): 거친 묶음마다 하나.
		TileListOut[DentCoarseListBase] = SB_CoarseListCount;
		SB_WriteTileListArgs(SB_LIST_ARGS_COARSE, SB_CoarseListCount, SB_TILE_ROW);
		// 부위 나누기 장의 함몰(검토 R03 E): 앞쪽 부위 묶음은 채널 순열, 나머지는 채널 없는 순열. 다른 목록 패스는 위 0번 인자로 목록 전체를 돈다.
		// 부위 나누기가 없는 장은 쓰지 않는다(ActivityArgsCS가 0으로 비워 둔다).
		if (bDentParts != 0u)
		{
			TileCountOut[SB_TILE_COUNT_PARTS] = PartsCount;
			SB_WriteTileListArgs(SB_LIST_ARGS_PARTS, PartsCount, SB_TILE_ROW);
			SB_WriteTileListArgs(SB_LIST_ARGS_SINGLE, Count - PartsCount, SB_TILE_ROW);
		}
	}
}

// --- 쉬기 (ADR-0013) ---
// 눌린 칸이 없는 갱신들이 복원을 다 끝내면 뒤 패스들(복사·덩어리·이완·솟음·합치기·노멀)을 0개 묶음으로 돌려
// 쉬게 하고, 쉬기 시작하는 프레임에 한 번 비운다(ClearCS). 새로 눌리면 같은 프레임에 다시 돈다. 복원이 끝난 정도는 갱신 수가
// 아니라 갱신마다 실제로 줄인 몫(SettleStep)을 더해 센다: 미룬 갱신(ADR-0021)은 걸음이 커서 한 번에 더 많이 줄인다. 간접 디스패치 인자 네 벌:
// SB_ARGS_LISTED = 접촉 묶음 목록 패스(복사·합치기·노멀), SB_ARGS_LUMP = 덩어리 힘 묶음 목록, SB_ARGS_CLEAR = 비우기(전체 해상도),
// SB_ARGS_TILES = 목록 패스(스레드 묶음 하나, 어디든 눌렸거나 지난 프레임 끝에 상태가 남았을 때만). 이완·솟음 내리기의 인자는
// TileListCS가 목록 길이로 따로 채운다(TileListArgsOut — 여기서는 0으로 비운다). 솟음·끌림 원천 머리도 여기서 비운다.
#define SB_ARGS_LISTED 0u
#define SB_ARGS_LUMP 1u
#define SB_ARGS_CLEAR 2u
#define SB_ARGS_TILES 3u
Buffer<uint> ActivityIn;             // ContactCS의 ActivityOut
RWStructuredBuffer<uint> RestFrames; // [0] 마지막으로 눌린 뒤 복원이 끝난 정도(SB_SETTLED = 다 끝남, SB_RESTED에서 멈춤), [1] 1 = 비워서 쉬는 중
RWBuffer<uint> ArgsOut;
uint FullGroups;                     // 전체 해상도 패스(비우기)의 한 변 묶음 수
uint SettleStep;                     // 이번 갱신이 복원을 끝내는 몫(SB_SETTLED = 한 번에 다 끝남, CPU의 SettleOfStep)
uint bResetRest;                     // 1 = 표면 장을 막 만들었거나 CPU가 비웠다(모든 장이 0)

#define SB_SETTLED 0x1000000u
#define SB_RESTED 0x7fffffffu

void SB_WriteArgs(uint Slot, uint Groups)
{
	uint Base = Slot * ArgsStride;
	ArgsOut[Base + 0] = Groups;
	ArgsOut[Base + 1] = Groups;
	ArgsOut[Base + 2] = 1u;
}

/** 목록 디스패치 인자(한 줄 SB_CONTACT_ROW 묶음): 목록 길이 Count. */
void SB_WriteListArgs(uint Slot, uint Count)
{
	uint Base = Slot * ArgsStride;
	ArgsOut[Base + 0] = min(Count, SB_CONTACT_ROW);
	ArgsOut[Base + 1] = (Count + SB_CONTACT_ROW - 1u) / SB_CONTACT_ROW;
	ArgsOut[Base + 2] = 1u;
}

[numthreads(1, 1, 1)]
void ActivityArgsCS()
{
	bool bPressed = ActivityIn[0] != 0u;
	// 지난 갱신까지 끝난 정도. 덜 끝났으면 이번 갱신이 돌고, 이번 걸음의 몫을 더해 둔다.
	uint Settled = bPressed ? 0u : (bResetRest != 0u ? SB_RESTED : RestFrames[0]);
	bool bActive = bPressed || Settled < SB_SETTLED;
	// 비워서 쉬는 중이었는지. 복원 시간이 프레임 시간에 따라 바뀌어도 쉬기 시작하는 첫 프레임에 꼭 한 번 비운다.
	bool bWasCleared = bResetRest != 0u || RestFrames[1] != 0u;
	bool bClearNow = !bActive && !bWasCleared;
	RestFrames[0] = bPressed ? 0u : min(Settled + SettleStep, SB_RESTED);
	RestFrames[1] = bActive ? 0u : 1u;
	SB_WriteListArgs(SB_ARGS_LISTED, bActive ? ContactGroupCount[0] : 0u);
	SB_WriteListArgs(SB_ARGS_LUMP, bActive ? LumpListCount[0] : 0u);
	SB_WriteArgs(SB_ARGS_CLEAR, bClearNow ? FullGroups : 0u);
	// 이완 묶음(2단계): 어디든 눌렸거나 지난 프레임 끝에 상태가 남은 묶음이 있을 때만 목록을 만든다. 아니면 목록이 비는 것과
	// 같으므로 목록 패스도 이완도 0개 묶음으로 돈다.
	bool bTiles = bPressed || TileAnyResidualOut[0] != 0u;
	SB_WriteArgs(SB_ARGS_TILES, bTiles ? 1u : 0u);
	// 이번 프레임 목록 패스와 이완이 다시 채운다.
	TileAnyResidualOut[0] = 0u;
	TileCountOut[0] = 0u;
	TileCountOut[1] = 0u;
	UNROLL
	for (uint Word = 2u; Word < SB_TILE_COUNT_WORDS; ++Word)
	{
		TileCountOut[Word] = 0u;   // [2] 끌림이 닿은 거친 묶음 수, [3..] 끌림 걸음 집계(stat)
	}
	// 멈춤 표지(일지 203): 눌렸는지를 적어 둔다. 합치기 패스가 변위가 바뀐 묶음이 있으면 SB_STILL_CHANGED를 더한다.
	TileCountOut[SB_TILE_COUNT_STILL] = bPressed ? SB_STILL_PRESSED : 0u;
	SB_WriteTileListArgs(SB_LIST_ARGS_FINE, 0u, SB_TILE_ROW);
	SB_WriteTileListArgs(SB_LIST_ARGS_COPY, 0u, SB_TILE_ROW);
	SB_WriteTileListArgs(SB_LIST_ARGS_COARSE, 0u, SB_TILE_ROW);
	SB_WriteTileListArgs(SB_LIST_ARGS_PARTS, 0u, SB_TILE_ROW);
	SB_WriteTileListArgs(SB_LIST_ARGS_SINGLE, 0u, SB_TILE_ROW);
	// 접촉 압력 머리(ADR-0037)와 끌림 원천 머리(ADR-0033): 칸·원천 수 0, 상자 최소는 가장 큰 순서 정수·최대는 0(원자 최소·최대가 채운다), 두께·자르는
	// 거리 최대 0, 넘침 0.
	PressureHeaderOut[SB_SOURCE_HEADER_COUNT] = 0u;
	PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL1] = 0u;
	PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL2] = 0u;
	PressureHeaderOut[SB_PRESSURE_HEADER_LEVEL] = 0u;
	PressureHeaderOut[SB_PRESSURE_HEADER_COUNT] = 0u;
	PressureHeaderOut[SB_PRESSURE_HEADER_BEND] = 0u;
	DragHeaderOut[SB_SOURCE_HEADER_COUNT] = 0u;
	UNROLL
	for (uint Axis = 0u; Axis < 3u; ++Axis)
	{
		PressureHeaderOut[SB_SOURCE_HEADER_MIN + Axis] = 0xffffffffu;
		PressureHeaderOut[SB_SOURCE_HEADER_MAX + Axis] = 0u;
		DragHeaderOut[SB_SOURCE_HEADER_MIN + Axis] = 0xffffffffu;
		DragHeaderOut[SB_SOURCE_HEADER_MAX + Axis] = 0u;
	}
	PressureHeaderOut[SB_SOURCE_HEADER_SIGMA] = 0u;
	PressureHeaderOut[SB_SOURCE_HEADER_OVERFLOW] = 0u;
	DragHeaderOut[SB_DRAG_HEADER_REACH] = 0u;
	DragHeaderOut[SB_DRAG_HEADER_LIVE] = 0u;
	DragHeaderOut[SB_DRAG_HEADER_CLUSTERS] = 0u;
	DragHeaderOut[SB_SOURCE_HEADER_OVERFLOW] = 0u;
}

Texture2D<float4> CopyIn;
RWTexture2D<float4> CopyOut;

/** 지난 변위 보관(머티리얼의 지난 프레임 계산 = 속도가 읽는다). 접촉 묶음 목록만 — 목록 밖은 둘 다 늘 0이다. */
[numthreads(8, 8, 1)]
void CopyCS(uint3 GroupId : SV_GroupID, uint3 LocalId : SV_GroupThreadID)
{
	int2 T;
	if (SB_ListedTexel(GroupId, LocalId, T))
	{
		CopyOut[T] = CopyIn.Load(int3(T, 0));
	}
}

RWTexture2D<float> ClearStateA;
RWTexture2D<float4> ClearDisplacement;
RWTexture2D<float4> ClearPreviousDisplacement;
RWTexture2D<float4> ClearNormal;
RWTexture2D<float> ClearAccepted;
RWTexture2D<float> ClearHold;        // 누른 자리 기억(ADR-0032)
RWTexture2D<float4> ClearDrag;       // 끌림 거친 격자(DragResolution², 일지 161): 끌림 G
RWTexture2D<float4> ClearDragHold;   // 붙든 자리와 끄는 힘(ADR-0033, SB_DragPackHold — 0 낱말은 0으로 읽는다)

/** 쉬기 시작: 복원이 끝나 남은 아주 작은 함몰·기억·끌림(과 끄는 힘)·받아들인 깊이·변위·노멀을 0으로(머티리얼이 읽는 장이 정확히 제자리). */
[numthreads(8, 8, 1)]
void ClearCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x < Resolution && Id.y < Resolution)
	{
		ClearStateA[Id.xy] = 0.0;
		ClearDisplacement[Id.xy] = float4(0, 0, 0, 0);
		ClearPreviousDisplacement[Id.xy] = float4(0, 0, 0, 0);
		ClearNormal[Id.xy] = float4(0, 0, 0, 0);
		ClearAccepted[Id.xy] = 0.0;
		ClearHold[Id.xy] = 0.0;
	}
	if (Id.x < DragResolution && Id.y < DragResolution)
	{
		ClearDrag[Id.xy] = float4(0, 0, 0, 0);
		ClearDragHold[Id.xy] = float4(0, 0, 0, 0);
	}
}

// --- stat softbody (통계를 보는 중일 때만) ---
Buffer<uint> StatActivity;           // ContactCS의 ActivityOut
StructuredBuffer<uint> StatRest;     // ActivityArgsCS가 쓴 RestFrames
Buffer<uint> StatTileCount;          // 이번 프레임 묶음 수(TileCountOut)
Buffer<uint> StatContactGroupCount;  // 접촉 패스 묶음 수
Buffer<uint> StatDragHeader;         // 끌림 원천 머리(DragHeaderOut, ADR-0033)
RWBuffer<uint> StatOut;              // [0] 1 = 쉰다, [1] 함몰 묶음 수, [2] 접촉 묶음 수, [3] 1 = 눌린 칸이 있다, [4] 끌림 원천 수(끄는 힘이 남은 원천이 없으면 0 — 모으기가
                                     // 건너뛴다), [5] 끌림이 닿은 거친 묶음 수(TileCount[2]), [6] 끌림 묶음 원천 수, [7] 섞인 묶음 원천 수, [8..13] 바로잡기·[14..19] 모으기
                                     // 걸음 집계(SB_DRAG_STAT_WORDS낱말씩, 일지 189), [20] 부위 묶음 수, [21..25] 목록 까닭별 수, [26] 1 = 부위 나누기 장(검토 R03),
                                     // [27] 살 두께보다 깊이 눌린 표면 칸 수, [28] 가장 깊이 넘은 양(cm, float 비트 — 일지 295),
                                     // [29] 들어온 면 표지 자리가 모자란 표면 칸 수

/** 표면 장 GPU 상태를 한 버퍼에 모은다(CPU가 몇 프레임 뒤에 읽는다). */
[numthreads(1, 1, 1)]
void StatsCS()
{
	StatOut[0] = StatRest[1];
	StatOut[1] = StatTileCount[0];
	StatOut[2] = StatContactGroupCount[0];
	StatOut[3] = StatActivity[0];
	StatOut[4] = StatDragHeader[SB_DRAG_HEADER_LIVE] > 0u ? StatDragHeader[SB_SOURCE_HEADER_COUNT] : 0u;
	StatOut[5] = StatTileCount[2];
	StatOut[6] = StatDragHeader[SB_DRAG_HEADER_CLUSTERS];
	UNROLL
	for (uint Word = SB_DRAG_STAT_MIXED; Word < SB_DRAG_STAT_END; ++Word)
	{
		StatOut[Word + 4u] = StatTileCount[Word];   // [7] 섞인 묶음 원천 수, [8..13] 바로잡기, [14..19] 모으기 걸음 집계(일지 189)
	}
	UNROLL
	for (uint Why = SB_TILE_COUNT_PARTS; Why < SB_TILE_COUNT_WORDS; ++Why)
	{
		StatOut[Why + 3u] = StatTileCount[Why];   // [20] 부위 묶음 수, [21..25] 목록 까닭별 수, [26] 부위 나누기 장(검토 R03)
	}
	StatOut[27] = StatActivity[SB_ACTIVITY_DEEP];
	StatOut[28] = StatActivity[SB_ACTIVITY_DEEP + 1u];
	StatOut[29] = StatActivity[SB_ACTIVITY_LABEL_OVERFLOW];
}
