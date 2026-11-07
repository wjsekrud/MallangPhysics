// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SoftbodySurfaceField.generated.h"

class UTextureRenderTarget2D;
class USoftbodyColliderField;
class USoftbodyMeshData;
struct FSoftbodyColliderShape;
struct FSoftbodyFieldVoxelBuffer;
struct FSoftbodyLumpReadback;
struct FSoftbodyPressureCapture;
struct FSoftbodySurfaceRestState;
class FRHICommandListImmediate;
class FTextureResource;
class FTextureRenderTargetResource;
class USoftbodySurfaceBatch;

namespace SoftbodySurfaceFieldRows
{
	/** 모양 거리장 부위 하나가 FieldInstances에서 차지하는 float4 수. */
	constexpr int32 PerInstance = 7;
}

/**
 * 잡기(ADR-0041): 손이 피부의 둥근 자리 하나를 붙들고 끄는 입력(Softbody Grabber). 셰이더 "잡기" 머리 주석(SB_GRAB_*)과 같은 값·배치.
 * 메시마다 줄 MaxRows개 — 붙든 잡기는 MaxHolding개까지, 나머지는 놓는 중인 줄(놓은 뒤 Recovery Rate로 돌아오는 동안)이 쓴다.
 */
namespace SoftbodyGrab
{
	constexpr int32 MaxRows = 8;
	constexpr int32 MaxHolding = 4;
	/** 줄마다 CPU가 올리는 float4 수: (끄는 자리, R), (잡는 점, 번호), (트레이스 방향, 표지) — 모두 프레임 cm. */
	constexpr int32 InputRows = 3;
	/** 줄마다 GPU가 남기는 float4 수(셰이더 GrabState 배치 — [8]은 덩어리 잡기 용수철의 목표). */
	constexpr int32 StateRows = 9;
	constexpr uint32 FlagHold = 1u;
	constexpr uint32 FlagFacing = 2u;
	/** 줄을 바로 비운다(GPU 확인 없이 놓은 줄 — 컴포넌트를 끈 동안 남은 줄이 다시 켤 때 튀지 않게). */
	constexpr uint32 FlagClear = 4u;
	/** GPU 줄 상태(GrabState[0].x 아래 8비트). */
	enum class EStatus : uint32 { Empty = 0, Held = 1, Releasing = 2, Missed = 3 };
	/** 잡는 점에서 피부를 찾는 거리(cm, 셰이더 SB_GRAB_SEARCH). */
	constexpr float SearchDistance = 5.f;
	/** 잡은 자리(피부 + 덩어리)가 가는 끝 = 한도 min(살 두께, 원 반지름) × 이 배(셰이더 SB_PressLimit — 손이 더 가도 놓지 않고 여기 머문다). */
	constexpr float MaxStretchRatio = 1.3f;
}

/**
 * 디버그 캡처(Softbody.CaptureField)가 한 갱신 끝에 뜨는 사본의 차례(FSoftbodySurfaceFieldRender::CaptureCopies와 USoftbodySurfaceField의 사본 타깃).
 * 저장 파일 이름도 이 이름이다(Drag = 끌림 거친 격자 DragState, Long = 접촉 압력 거친 격자 ContactLong).
 */
namespace SoftbodyCapture
{
	enum ECopy : int32 { StateA, Contact, Position, Displacement, Normal, Accepted, Hold, Labels, Drag, Long, Bend, NumCopies };
}

/**
 * ADR-0003 표면 장 한 프레임의 입력. "프레임" 공간 = 타깃 컴포넌트의 회전만 적용한 cm 공간
 * (원점 = 컴포넌트 위치). 콜라이더와 결과 변위가 모두 이 공간이다.
 */
struct FSoftbodySurfaceFieldInput
{
	/**
	 * 형상당 3개: (중심, 종류 0구/1캡슐/2박스/3컨벡스), 회전 쿼터니언,
	 * (반경·반길이 / 박스 반폭 / 컨벡스는 Planes 시작·개수).
	 */
	TArray<FVector4f> Shapes;
	/** 컨벡스 평면(형상 로컬): xyz 바깥 노멀, w 거리. 안쪽은 모든 평면에서 dot(N, x) <= w. */
	TArray<FVector4f> Planes;
	/** 스켈레탈 전용, 본당 3개: 스키닝 행렬(레퍼런스→현재, 컴포넌트 공간)의 열 0~2. 스태틱은 비운다. */
	TArray<FVector4f> Bones;
	/**
	 * Bones와 같은 꼴, 지난 갱신(표면 장이 마지막으로 돈 갱신)의 자세. 손 닻이 피부가 본을 따라 움직인 거리를 뺀다(일지 336). 길이가 Bones와 다르면
	 * (처음·깨어난 뒤·메시를 바꾼 뒤) 모른다고 보고 Bones를 쓴다(움직임 0).
	 */
	TArray<FVector4f> PreviousBones;
	/**
	 * 모양 거리장 부위(ADR-0004), 부위당 SoftbodySurfaceFieldRows::PerInstance(7)개:
	 * 0~2 프레임 → 부위 메시 로컬 행렬의 열(셰이더가 dot(float4(p, 1), 열)), 3 (첫 칸 중심, 칸 크기),
	 * 4 (칸 수 xyz, 첫 칸 번호 = RegisterColliderField가 준 기준 + 부위 오프셋),
	 * 5 (메시 로컬 단위 → cm 배율, 자기 몸 부위 번호 — 외부 콜라이더는 -1, 0, 0),
	 * 6 (살이 든 칸을 감싸는 상자의 반폭 xyz, 메시 로컬 단위 — 덩어리 힘이 누르는 것의 가로 크기로 쓴다(일지 248 ②), 0).
	 */
	TArray<FVector4f> FieldInstances;
	/**
	 * 끌림(ADR-0008): Shapes의 형상마다 3개, 지금 프레임 점 → 그 형상에 붙은 같은 점의 지난 프레임 위치 행렬(행 벡터 규약)의
	 * 열 0~2. 지난 프레임에 없던 형상은 제자리(단위 행렬). 개수가 형상 수와 다르면 모두 제자리로 본다.
	 */
	TArray<FVector4f> ShapeMotion;
	/** 끌림: FieldInstances의 부위마다 3개, ShapeMotion과 같은 규약. */
	TArray<FVector4f> FieldMotion;
	/**
	 * 들어온 면 표지(일지 313): Shapes의 형상마다·FieldInstances의 부위마다 고정 번호(0~126, 127 = 번호 없음 — 바깥 콜라이더 하나의 형상·부위는 같은
	 * 번호, 자기 몸 부위는 부위마다) + 0x80(지난 갱신에 없던 형상·부위 — 지난 자세를 모른다). 칸에 남은 표지가 어느 콜라이더 것인지 갱신을 넘어
	 * 잇는다(올리는 차례는 프레임마다 바뀐다). 개수가 다르면 모두 번호 없음.
	 */
	TArray<uint32> ShapeIds;
	TArray<uint32> FieldIds;
	/** 콜라이더의 지난 자세(ShapeMotion·FieldMotion)가 표면 장의 지난 갱신 것인가 — 길 검사를 해도 된다(쉬다 깨어난 첫 갱신은 false). */
	bool bSweep = false;
	FVector3f ComponentScale = FVector3f::OneVector;
	float MaxContactDepth = 15.f;
	/** 콜라이더 가장자리에서 누른 깊이를 줄이는 폭 (칸 수). 0이면 다듬지 않는다. */
	float EdgeTrim = 1.5f;
	/** 함몰 어둡기 최대치 (0~1). */
	float OcclusionStrength = 0.6f;
	/** 복원율 (1/s): 살층 점탄성의 하나뿐인 시간 — 함몰·끌림이 제 평형으로 다가가는 빠르기(ADR-0019). */
	float RecoveryRate = 0.f;
	/** 이번 프레임 시간 (초). 이완의 암시적 한 걸음 크기(복원율 × 프레임 시간)가 여기서 정해진다. */
	float DeltaTime = 0.f;
	/**
	 * 살이 눌린 모양에 길든다(ADR-0003 §14): 프레임마다 칸의 받아들인 깊이가 (함몰 − 받아들인 깊이)의 이 비율만큼 함몰을
	 * 따라간다. 0이면 길들지 않는다(함몰이 늘 누르기 전 모양으로 돌아간다).
	 */
	float AcceptBlend = 0.f;
	/** 프레임마다 받아들인 깊이에 곱할 값 (떼고 남은 자국이 사라지는 빠르기). */
	float AcceptReturn = 1.f;
	/** 끌림: 누른 채 움직이는 콜라이더와 피부 사이의 마찰(피부가 옆으로 버티는 힘의 한도 = 누르는 힘 × 이 값, ADR-0008 §12). 0이면 끈다. */
	float DragRatio = 0.f;
	/**
	 * 접촉 압력과 긴 몫(ADR-0037): 손이 붙든 살의 접촉 힘을 풀어 살층 응답의 긴 몫(둘레 솟음)을 더한다. 접촉 단계 Basic이면 false(함몰만 — 붙든 칸 목표가
	 * 붙들 깊이).
	 */
	bool bContactLong = true;
	/** 옷 밑 피부 칸 베끼기가 함몰 벽을 둘레 4x4 칸으로 이어 읽는다(Contact Detail Full, 일지 359·361). false면 2x2 칸의 같은 쪽 평균(Basic). */
	bool bWallGhost = true;
	/**
	 * 덩어리(ADR-0012): 관성 브릿지(USoftbodyComponent::BridgeWidth 주석, 8 × 9 float4) — 머티리얼이 흔들림을 그리는 값과 같다.
	 * 덩어리 칸은 지금 덩어리가 비켜난 만큼 옮긴 자리에서 누른 깊이를 잰다. 비우면 제자리에서 잰다.
	 */
	TArray<FVector4f> InertiaBridge;
	/** 프레임 → 월드 회전. */
	FQuat4f ComponentRotation = FQuat4f::Identity;
	/** 프레임 원점(컴포넌트 위치) − 액터 위치 (cm). 브릿지의 구동점 위치가 액터 기준이다. */
	FVector3f FrameToActor = FVector3f::ZeroVector;
	/** 덩어리를 미는 힘을 잴지(LumpYield > 0). 재면 덩어리 평형을 GPU가 풀어 GetLumpSolution으로 몇 프레임 뒤에 읽힌다. */
	bool bLumpForce = false;
	/**
	 * 덩어리 평형(ADR-0017): 묶음마다 USoftbodySurfaceField::NumLumpGroupRows줄 (C00 C01 C02 s), (C11 C12 C22 A), (T, 1 = 푼다), 그 뒤 4줄에
	 * 국소 모양(일지 251)의 단단함 Λ 16개(넷씩 — 모양이 없는 자리는 1). C = 뿌리 스프링(덩어리 살의 평균 무름), s = 살 단단함 × 칸 넓이,
	 * A = 흔들림 크기, T = 이번 프레임 관성 목표(기본 자세 축 하중 g). bLumpForce일 때만 채운다.
	 */
	TArray<FVector4f> LumpGroups;
	/** 덩어리 쥐기(ADR-0012 §7): 붙은 피부가 덩어리를 손 쪽으로 당기는 단단함(누르는 살 대비 배수). 0이면 끈다. */
	float LumpGrip = 0.f;
	/** 조일수록 단단해지는 살(ADR-0012 §7.1, ADR-0008 §11): 깊이 변형률 ε = 깊이/살 두께에서 노멀 힘·끌림 한도의 배수 (e^{αε} − 1)/(αε)의 α. 0이면 선형. */
	float LumpStiffen = 0.f;
	/** 압축성에 더하는 값(컴포넌트 Volume Offset): 칸 부피 보존 v에 더하고 0~1로 자른다. 덩어리 모양은 구운 값 그대로다. */
	float VolumeOffset = 0.f;
	/** 판 칠에 더하는 값(컴포넌트 Flex Offset): 판 칸의 Flex 칠에 더하고 0~1로 자른다. */
	float FlexOffset = 0.f;
	/** 겉껍질 장력 s(컴포넌트 Skin Tension, ADR-0037 §3.4): 칸 칠에 곱한다. 굽기는 이 값을 쓰지 않는다. */
	float SkinTension = 0.f;
	/**
	 * 멈춤 비교용 원값(갱신은 이 값이 아니라 구운 데이터를 읽는다): 구운 데이터의 Volume Preservation. 저작 에셋에서 고치면 굽지 않고 바로 옮겨 와
	 * 실행 중에도 바뀐다(USoftbodyAuthoringData::SyncMaterialValues).
	 */
	float BakedVolumePreservation = 1.f;
	/** 통계를 보는 중이면(stat softbody 등) GPU 상태를 몇 프레임 뒤에 읽어 온다(GetGpuState). */
	bool bReadStats = false;
	/**
	 * 갱신률 LOD(ADR-0021): 메시가 화면에서 차지하는 크기(엔진이 LOD를 고르는 식, 화면 높이 대비 바운드 구 지름). 월드 묶음이
	 * 작게 보이는 표면 장을 몇 프레임에 한 번만 돌리는 데 쓴다. 지난 프레임에 그린 뷰가 없으면 1.
	 */
	float ScreenSize = 1.f;
	/**
	 * 메시가 지난 프레임에 그린 어느 뷰의 시야에든 들었는가(뷰가 없으면 true). 화면에 없는 표면 장은 갱신 차례에서 맨 뒤로 가고,
	 * 돌지 못한 틱도 복원이 끝난 것으로 센다(지워져도 보이지 않는다).
	 */
	bool bInView = true;
	/**
	 * 멈춤(일지 203, ADR-0021 §8): 회전·방향 성분의 허용 오차를 거리로 바꾸는 반지름(타깃 바운드 구, cm) — 그 안의 어느 점도
	 * softbody.Surface.StillDistance보다 더 움직이지 않으면 입력이 멈춘 것으로 본다.
	 */
	float BoundsRadius = 100.f;
	/** 멈춤 비교용 원값(초): AcceptBlend·AcceptReturn은 틱 시간에 따라 달라져 견줄 수 없다. 디테일 패널의 Memory Time, Memory Fade Time. */
	float MemoryTime = 0.f;
	float MemoryFadeTime = 0.f;
	/**
	 * 잡기(ADR-0041): 줄 SoftbodyGrab::MaxRows개 × SoftbodyGrab::InputRows(float4, 셰이더 GrabInput 배치). 비면 잡기 없음 — 셰이더가 잡기 코드를 모두
	 * 건너뛴다(지금 식 그대로). 붙든 잡기나 놓는 중인 줄이 있는 동안은 채운다(놓는 중 줄은 번호만, 표지 0). 잡기가 있으면 닿을 콜라이더가 없어도 표면 장이 쉬지 않는다.
	 */
	TArray<FVector4f> Grabs;
	/** 붙든 잡기가 있다 — 월드 묶음이 갱신 예산에서 먼저 돌린다(처음 닿는 순간과 같은 앞줄). */
	bool bGrabbing = false;
};

/**
 * 하드 서피스 조각 표(ADR-0037 §3.7)를 셰이더가 읽는 꼴로 — 표면 장이 구운 데이터에서 한 번 만들어 갱신마다 공유한다(렌더 스레드는 읽기만).
 * Frames: 조각마다 세 줄 (가운데 xyz, 자유도), (평면 축 U, 0), (평면 축 V, 0) — 메시 로컬. TileRange: 조각마다 함몰 묶음 목록의 (첫 자리, 수).
 * TileList: 함몰 묶음(x | y << 16).
 */
struct FSoftbodyHardPieceTable
{
	/** 조각마다 세 줄: (가운데, 0), (축 U, 반지름), (축 V, 0) — 메시 로컬. */
	TArray<FVector4f> Frames;
	TArray<FUintVector2> TileRange;
	TArray<uint32> TileList;
	/** 조각마다 발자국 거친 칸 목록 구간(첫 자리, 수)과 목록(x | y << 16) — 압력 풀이의 발자국 줄. */
	TArray<FUintVector2> CellRange;
	TArray<uint32> CellList;
	/** 발자국 칸마다 (판 x, 판 y, 넓이, 0) — 구운 USoftbodyMeshData::HardCellPlate(메시 로컬 단위). 압력 풀이의 받침 줄 자리. */
	TArray<FVector4f> CellPlate;
	/** 표 일련번호(만들 때마다 새로) — 렌더 쪽이 올린 판 버퍼가 이 표의 것인지 견준다. */
	uint32 Serial = 0;
};

/**
 * 디버그 캡처(Softbody.CaptureField)가 사본을 뜬 갱신의 접촉 압력 풀이 버퍼를 CPU로 읽은 값(USoftbodySurfaceField::ReadPressureCapture). 배열이 비었으면
 * 그 갱신에 그 버퍼가 없었다 — Basic이거나 거리표가 없으면 풀이가 없고, 조각이 없는 자산은 하드 서피스 버퍼가 없다. 배치는 셰이더 주석 그대로다.
 */
struct FSoftbodyPressureCaptureData
{
	/** 압력 머리(셰이더 SB_PRESSURE_HEADER_*): [0] 단계 0 칸 수, [8] 넘친 칸 수, [9]·[10] 단계 1·2 칸 수, [11] 고른 단계, [12] 행렬 줄 수 N_K. */
	TArray<uint32> Header;
	/** 압력 칸 기록 전체(칸마다 uint4 13줄, [단계 × 상한 + 번호] — 셰이더 SB_PressureIndex). */
	TArray<FUintVector4> Cells;
	/** 행렬 줄마다 풀린 값(PressureForce, μ·cm²): 조각 밖 칸은 접촉 힘, 발자국 줄은 판이 편 하중(붙은 판은 음수도). 손 칸 줄·모드 줄 값은 남지 않는다. */
	TArray<float> Force;
	/** 풀이 기록(PressureStats): [0] 활성 집합 바퀴 수, [1] CG 걸음 수. */
	TArray<uint32> Stats;
	/** 하드 서피스 일 버퍼(셰이더 SB_HARD_WORK_*). */
	TArray<uint32> HardWork;
	/** 조각 위 손 칸 줄(줄마다 float4 17개 — 머리 = (조각, 거친 칸 x + y × 4096, 표본 수, 가장 깊은 표본 깊이 cm)). */
	TArray<FVector4f> HardHand;
	/** 발자국 줄마다 (판 x cm, 판 y cm, asfloat(조각 안 칸 | 후보 << 16), 넓이 cm²). */
	TArray<FVector4f> FRow;
	/** 조각마다 (가라앉음 cm, U 기울기, V 기울기, 켜짐 — 1 회색 판, 2 붙은 강체, 3 떠 있음, 0 이번 풀이에 없음). */
	TArray<FVector4f> PieceMotion;
	/** 그 갱신의 조각 표와 Flex Offset — 조각 종류(SB_HardKindOf)와 발자국 칸 자리를 CPU에서 풀이와 같은 규칙으로 읽는다. */
	TSharedPtr<const FSoftbodyHardPieceTable, ESPMode::ThreadSafe> HardTable;
	float FlexOffset = 0.f;
};

/**
 * 표면 장 한 프레임의 GPU 갱신 입력(렌더 스레드로 넘기는 값, 내부용). USoftbodySurfaceField::Update가 채우고, 월드 묶음
 * (USoftbodySurfaceBatch, ADR-0021)이 이 월드의 것들을 모아 USoftbodySurfaceField::RenderBatch에 넘긴다.
 */
struct FSoftbodySurfaceFieldRender
{
	// 갱신을 넘어 남는 텍스처(ADR-0022). 갱신 안에서만 쓰는 접촉 노멀·끌림 입력·끌림 원천·덩어리 깊이·함몰 패스 결과·붙들 깊이는
	// RenderBatch가 그래프 안 임시 텍스처로 잡는다.
	FTextureRenderTargetResource* StateA = nullptr;
	/** 누른 자리 기억 F(ADR-0032). */
	FTextureRenderTargetResource* Hold = nullptr;
	FTextureRenderTargetResource* Position = nullptr;
	FTextureRenderTargetResource* Displacement = nullptr;
	FTextureRenderTargetResource* PreviousDisplacement = nullptr;
	FTextureRenderTargetResource* Normal = nullptr;
	FTextureRenderTargetResource* Accepted = nullptr;
	/** 들어온 면 표지(일지 313·314, R32F): 칸마다 콜라이더 둘까지 자리 코드(1 + 고정 번호)와 들어온 방향 — 30비트 정수를 float 비트로(셰이더 SB_LabelsWord). */
	FTextureRenderTargetResource* Labels = nullptr;
	FTextureResource* CarrierPosition = nullptr;
	FTextureResource* CarrierNormal = nullptr;
	FTextureResource* Redirect = nullptr;
	FTextureResource* SkinIndex = nullptr;
	FTextureResource* SkinWeight = nullptr;
	TArray<FVector4f> Shapes;
	TArray<FVector4f> Bones;
	/** 지난 갱신 자세(FSoftbodySurfaceFieldInput::PreviousBones). 모르면 Bones와 같은 값. */
	TArray<FVector4f> PreviousBones;
	TArray<FVector4f> Planes;
	TArray<FVector4f> FieldInstances;
	TArray<FVector4f> ShapeMotion;
	TArray<FVector4f> FieldMotion;
	/** 들어온 면 표지: 콜라이더(형상 → 거리장 부위 차례)마다 고정 번호(FSoftbodySurfaceFieldInput::ShapeIds·FieldIds를 이은 것). */
	TArray<uint32> ColliderIds;
	uint32 bSweep = 0;
	/** 새 거리장이 들어온 프레임만 채운다: 칸 값 전체를 다시 올린다. */
	TArray<uint32> FieldVoxelUpload;
	TSharedPtr<FSoftbodyFieldVoxelBuffer, ESPMode::ThreadSafe> FieldVoxelState;
	FVector3f ComponentScale = FVector3f::OneVector;
	uint32 Resolution = 0;
	float TexelSize = 1.f;
	/** 가장자리 다듬기 폭 (cm) = 칸 수 × 칸 크기. */
	float EdgeTrimWidth = 0.f;
	float ThicknessScale = 1.f;
	float MaxContactDepth = 0.f;
	float OcclusionStrength = 0.f;
	// 살이 눌린 모양에 길든다(ADR-0003 §14). 갱신률 LOD가 미룬 시간까지 한 걸음으로 거듭제곱한 값.
	float AcceptBlend = 0.f;
	float AcceptReturn = 1.f;
	float DragRatio = 0.f;
	// 접촉 압력(ADR-0037): 거친 칸마다 (긴 몫 L, 거친 압력 p) — 갱신마다 모든 칸에 새로 쓴다.
	FTextureRenderTargetResource* ContactLong = nullptr;
	/** 연속 굽힘의 거친 해 W_c(모으기 패스, Flex 순열만 쓴다). */
	FTextureRenderTargetResource* ContactBend = nullptr;
	/** 1 = 접촉 압력·긴 몫이 돈다(Contact Detail Full이고 거리표가 있다). */
	uint32 bContactLong = 0;
	/** 1 = 고스트 칸 베끼기가 함몰 벽을 4x4 칸으로 이어 읽는다(FSoftbodySurfaceFieldInput::bWallGhost). */
	uint32 bWallGhost = 0;
	FTextureRenderTargetResource* CoarseInfo = nullptr;
	FTextureRenderTargetResource* CoarseLayer = nullptr;
	/** 랜드마크 거리표(캐리어 굽기, SoftbodyBulgeTable)와 랜드마크 수. 없으면 긴 몫·끌림이 꺼진다. */
	FTextureResource* LandmarkDistance = nullptr;
	uint32 NumLandmarks = 0;
	/** 거리표 줄 하나의 텍셀 수(판 4: 랜드마크 수 + 16, 판 5: + 1 — 줄 끝이 붙은 살갗까지 거리, 일지 252). */
	uint32 LandmarkRowTexels = 0;
	uint32 CoarseBlock = 1;
	uint32 CoarseResolution = 0;
	float CoarseTexelSize = 1.f;
	// 끌림 거친 격자(일지 161, ADR-0033) = 솟음 거친 격자: 끌림 G(DragState), 붙든 자리와 끄는 힘(DragHold — 반정밀도로 묶음, 일지 184) — 둘 다 갱신을
	// 넘어 남는다.
	// 이웃 표는 캐리어로만 정해져 렌더 스레드의 쉬기 상태가 쥔다.
	FTextureRenderTargetResource* DragState = nullptr;
	FTextureRenderTargetResource* DragHold = nullptr;
	uint32 DragBlock = 4;
	uint32 DragResolution = 0;
	float DragTexelSize = 1.f;
	/** 두꺼운 살(두께 상위 10%)의 끌림 폭 ℓ(cm): 원천마다 자르는 거리를 넉넉히 잡는 데 쓴다(0 = 모름). */
	float DragReachMax = 0.f;
	/** 1 = 거리표가 있어 끌림 패스가 돈다(0이면 DragRatio도 0이다). */
	uint32 bDragTable = 0;
	/**
	 * 함몰 거친 해(일지 199): 같은 함몰 식을 거친 격자(끌림·솟음 격자)에서 먼저 푼 값(거친 칸마다 cm, 갱신을 넘어 남는다). 가는 창은 그 해를 더한 자리에서
	 * 출발하고 목록 밖을 그 해로 읽는다. 1 = 쓴다(거친 격자 한 변이 이웃 표 한도 안 — 넘는 메시는 끌림과 같이 꺼진다). 0이면 목록 밖 함몰은 0이다
	 * (함몰 폭이 묶음보다 넓으면 묶음 경계에서 잘린다).
	 */
	FTextureRenderTargetResource* DentCoarse = nullptr;
	uint32 bDentCoarse = 0;
	/** 갱신마다 거친 패스 수(1~4 — 두꺼운 칸의 함몰 폭이 거친 칸 몇 개인가에서)와 거친 칸 체비쇼프 고윳값 범위의 아래끝. */
	uint32 DentCoarsePasses = 1;
	double DentCoarseChebyshevLow = 0.5;
	uint32 bMotionHistory = 0;
	// 살층 한 식의 시간(ADR-0019·ADR-0032·ADR-0033): 누른 자리 기억과 끌림 끄는 힘이 이번 걸음에 줄어드는 배수 e^{−R·Δt}(미룬 시간 포함), 함몰 정적 해
	// 체비쇼프 고윳값 범위의 아래끝(위끝은 2)
	float HoldDecay = 1.f;
	double DentChebyshevLow = 0.5;
	// 덩어리(ADR-0012)
	FTextureResource* CarrierLump = nullptr;
	FTextureResource* CarrierLumpSideA = nullptr;
	FTextureResource* CarrierLumpSideB = nullptr;
	FTextureRenderTargetResource* LumpNormal = nullptr;
	/**
	 * 손 닻(일지 201, 덩어리 값이 있는 캐리어만, RG32F): 눌린 칸마다 덩어리가 안 움직였다면의 손 자리(기본 자세 축 접선 두 성분). 덩어리 평형이
	 * 붙음·미끄러짐을 제 풀이 안에서 판정한다(편 손으로 누른 가슴이 떨지 않는다). 덩어리 값이 없는 캐리어는 null.
	 */
	FTextureRenderTargetResource* LumpAnchor = nullptr;
	float LumpGrip = 0.f;
	float LumpStiffen = 0.f;
	/** 부피 보존 칠(ADR-0024): 칸별 부피 보존 칠(R, 0 폼 … 1 살). null이면 칠이 없어 전부 1로 본다. */
	FTextureResource* CarrierVolume = nullptr;
	/** 저작 에셋의 Volume Preservation 값(ADR-0037 §3.3, 구운 데이터에 옮겨 둔 것). 칸의 부피 보존 v = 이 값 × 칠. */
	float VolumeValue = 1.f;
	/** 컴포넌트 Volume Offset(−1~1): 셰이더가 칸 v에 더하고 0~1로 자른다(SB_VolumeShifted). */
	float VolumeOffset = 0.f;
	/** 컴포넌트 Flex Offset(−1~1): 셰이더가 판 칸 칠과 조각 대표 칠에 더한다(SB_HardSpringAt·SB_HardInvDBeta). */
	float FlexOffset = 0.f;
	/** 겉껍질 장력 칠(ADR-0037 §3.4): 칸별 칠(R, 0 … 1). null이면 칠이 없어 전부 1로 본다. */
	/**
	 * 칠 1에서의 겉껍질 장력 길이 L_T(메시 로컬) = 4 s² × L0(s = 컴포넌트 Skin Tension 값, L0 = SoftbodyContactResponse::ReferenceLength).
	 * 칸의 L_T = 이 값 × 칠². 0이면 맨살(겉껍질 없음).
	 */
	float SkinLength = 0.f;
	/** 제 살 깊이(ADR-0003 §16): 접촉이 살 속에 묻힌 콜라이더를 찾는 깊이의 칸별 비율(R). null이면 전부 1(살 두께 그대로). */
	FTextureResource* CarrierColumn = nullptr;
	/** 덩어리 평형 상수(FSoftbodySurfaceFieldInput::LumpGroups). 비면 덩어리 평형을 풀지 않는다. */
	TArray<FVector4f> LumpGroups;
	/**
	 * 덩어리 국소 모양(일지 251, SoftbodyLumpRegion): 거친 칸마다 줄 번호(G16)와 줄 텍스처. bLumpRegion 1 = 덩어리 힘을 재고, 굽기가 이 장의 거친
	 * 격자에 줄을 적어 두었다 — 덩어리 평형이 모양마다 크기를 같이 풀고 접촉·합치기가 옮긴 양을 쓴다.
	 */
	FTextureResource* LumpRegionIndex = nullptr;
	FTextureResource* LumpRegionTable = nullptr;
	uint32 bLumpRegion = 0;
	TArray<FVector4f> InertiaBridge;
	FVector4f ComponentRotation = FVector4f(0.f, 0.f, 0.f, 1.f);
	FVector3f FrameToActor = FVector3f::ZeroVector;
	/** 0 = 덩어리 없음, 1 = 비켜난 자리에서 파임만, 2 = 덩어리 힘도 잰다(셰이더 LumpMode). */
	uint32 LumpMode = 0;
	TSharedPtr<FSoftbodyLumpReadback, ESPMode::ThreadSafe> LumpReadback;
	// 쉬기(ADR-0013)
	TSharedPtr<FSoftbodySurfaceRestState, ESPMode::ThreadSafe> RestState;
	/** 이번 갱신이 복원을 끝내는 몫(셰이더 SB_SETTLED = 한 번에 다 끝남). GPU 쉬기가 눌린 칸이 없는 갱신마다 더해 센다. */
	uint32 SettleStep = 0;
	// stat softbody
	bool bReadStats = false;
	/** 멈춤(일지 203): 이 신청의 번호(읽기 표지와 맞춘다)와 변위 변화 문턱(cm, 음수 = 멈춤 끔). */
	uint64 Serial = 0;
	float StillChange = -1.f;
	/** 프로파일 범위 이름(일지 189): ProfileGPU·Nsight에서 이 장의 패스를 액터 이름 아래 묶는다. */
	FString DebugName;
	/**
	 * 디버그 캡처(Softbody.CaptureField): 있으면 이번 갱신 끝에 저장할 장들을 여기에 복사해 둔다(차례 SoftbodyCapture::ECopy). 접촉은 갱신 안 임시
	 * 텍스처라(ADR-0022) 그때밖에 없고, 갱신을 넘어 남는 장도 같은 갱신에서 떠야 한 폴더가 한 순간이 된다. 요청이 없으면 비어 있다.
	 */
	TArray<FTextureRenderTargetResource*> CaptureCopies;
	/**
	 * 디버그 캡처: 있으면 이번 갱신의 접촉 압력 풀이 버퍼(풀이 줄·판 상태·수렴 기록)를 CPU 읽기 칸으로 복사한다. 사본과 같은 갱신이고, 요청이 없으면 null이라
	 * 평소 갱신에는 패스도 할당도 없다.
	 */
	TSharedPtr<FSoftbodyPressureCapture, ESPMode::ThreadSafe> PressureCapture;
	/**
	 * 부위 나누기(Separated Parts, 셰이더 "부위 나누기"): 굽기가 담은 칸마다 두 부위 몫(G16, USoftbodyMeshData::CarrierSeparationTex). bSeparation 1 =
	 * 몫이 있다. 갱신을 넘어 남는 상태는 없다(함몰 채널은 갱신마다 상태 A의 함몰에서 다시 푼다).
	 */
	FTextureResource* CarrierSeparation = nullptr;
	uint32 bSeparation = 0;
	/**
	 * 하드 서피스(ADR-0037 §3.7): 칸마다 조각 번호(G16)와 떠 있는 조각 밑 칸의 접촉 시작점 올림(G16, 떠 있는 조각이 있을 때만), 올림 눈금(메시 로컬).
	 * bHardSurface 1 = 자산에 조각이 구워져 있다 — 이 장의 표면 장 패스가 조각 순열(SB_HARD_SURFACE)을 쓴다. 조각이 없는 자산은 0이고 조각 코드가 없는
	 * 순열만 돈다(비용 분리, 원칙 E06).
	 */
	FTextureResource* CarrierPiece = nullptr;
	FTextureResource* CarrierLift = nullptr;
	/** 휘는 판(ADR-0038 §4): 칸마다 Flex 칠(G16)과 굽힘 길이 눈금 L0(SoftbodyContactResponse::ReferenceLength, 메시 로컬). */
	FTextureResource* CarrierSoftness = nullptr;
	float HardRefThickness = 0.f;
	float CarrierLiftScale = 0.f;
	uint32 bHardSurface = 0;
	/**
	 * 연속 굽힘만 있는 장(조각 없음, Flex 칠이 구워져 있거나 Flex Offset < 0): 1 = 조각 순열(이제 판 + 굽힘을 맡는 "Flex 순열")이 준비돼
	 * 이번 갱신에 그 순열을 쓴다. 준비 전이면 0 — 굽힘 없이 돌고 준비를 민다(끊김 없음, 굽힘이 늦게 켜진다).
	 */
	uint32 bFlexOnly = 0;
	TSharedPtr<const FSoftbodyHardPieceTable, ESPMode::ThreadSafe> HardTable;
	/** 잡기(ADR-0041): FSoftbodySurfaceFieldInput::Grabs 그대로. 비면 잡기 없음(GrabCount 0 — 잡기 패스·읽기 없음). */
	TArray<FVector4f> GrabInput;
};

/**
 * 표면 장이 갱신을 넘어 쥐고 있는 텍스처 한 벌(ADR-0022). 처음 닿을 때 월드 풀(USoftbodySurfaceBatch)에서 잡고, 콜라이더 없이
 * 복원이 끝나면 0으로 비워 돌려준다 — 메모리는 배치한 인스턴스 수가 아니라 닿은 수에 비례한다. 1024² 덩어리 캐릭터 한 벌 약 60 MB(함몰과 누른 자리 기억이 한 채널씩, 일지 161·ADR-0032).
 * 풀은 한 벌을 프레임 예산만큼 한 장씩 만든다(CreateNextTarget) — 한 프레임에 몰아 만들면 렌더 스레드와 GPU가 멈칫한다(일지 235).
 */
USTRUCT()
struct FSoftbodySurfaceTextures
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> StateA;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Hold;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Position;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Displacement;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> PreviousDisplacement;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Normal;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Accepted;
	/** 들어온 면 표지(일지 313, R32F). 1024²에서 4 MB. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Labels;
	/** 덩어리 값이 있는 캐리어만. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> LumpNormal;
	/** 손 닻(일지 201, RG32F): 덩어리 값이 있는 캐리어만. 1024²에서 8 MB. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> LumpAnchor;
	int32 Resolution = 0;
	bool bLump = false;
	/** 풀에서 쉬기 시작한 때(초, FPlatformTime). 오래 쉰 벌부터 놓는다. */
	double IdleSince = 0.0;

	/** 다 만들어졌다(만드는 중인 벌은 아직 쓰지 않는다). */
	bool IsValid() const { return IsComplete(); }
	/** 렌더 타깃 수(덩어리 값이 있으면 둘 더 — 덩어리 둘이 늘 끝이다). */
	static int32 NumTargets(bool bInLump) { return bInLump ? 10 : 8; }
	/** Index번째 자리와 그 이름·형식. */
	TObjectPtr<UTextureRenderTarget2D>* Slot(int32 Index);
	const TObjectPtr<UTextureRenderTarget2D>* Slot(int32 Index) const { return const_cast<FSoftbodySurfaceTextures*>(this)->Slot(Index); }
	/** 빈 자리 하나를 만든다(0으로 비운 것). 만든 바이트, 다 찼으면 0. */
	int64 CreateNextTarget(UObject* Outer);
	bool IsComplete() const;
	/** 한 벌을 한 번에 만든다(월드 풀이 없을 때만 — 풀은 프레임 예산만큼 나눠 만든다). */
	static FSoftbodySurfaceTextures Create(UObject* Outer, int32 Resolution, bool bLump);
	/** 모두 0으로(렌더 명령). 다음에 쓰는 표면 장이 "모든 장이 0"에서 시작한다. */
	void Clear() const;
	/** 여러 벌의 렌더 타깃을 그래프 하나로 0으로(렌더 명령, 풀이 한 프레임에 돌려받은 벌을 모아 비운다). */
	static void ClearTargets(TConstArrayView<TObjectPtr<UTextureRenderTarget2D>> Targets);
	/** GPU 메모리를 바로 돌려주고 참조를 놓는다(오브젝트는 GC가 거둔다). */
	void Destroy();
	int64 Bytes() const;
	/** 렌더 타깃 한 장의 GPU 바이트 / GC를 기다리지 않고 GPU 메모리를 놓는다(풀이 프레임 예산만큼 나눠 놓을 때). */
	static int64 TargetBytes(const UTextureRenderTarget2D* Target);
	static void ReleaseTarget(UTextureRenderTarget2D* Target);
};

/**
 * 거친 격자 텍스처 한 벌(솟음 ADR-0031·끌림 ADR-0033·함몰 거친 해 일지 199가 같이 쓴다, 캐리어 칸 Block × Block이 한 칸). 가는 벌과 같이 처음 닿을 때
 * 풀에서 잡고 돌려준다(일지 235 — 전에는 표면 장마다 첫 갱신에 만들어 끌 때까지 쥐었다). 끌림·거친 해는 새로 잡은 첫 갱신(장이 모두 0)에 렌더 스레드가
 * 0으로 비우고, 거친 칸 고정 표(Info·Layer)는 그때 캐리어에서 다시 만든다 — 다른 표면 장이 쓰던 벌이어도 된다. 솟음 둘은 갱신마다 모든 칸을 새로 쓴다.
 */
USTRUCT()
struct FSoftbodySurfaceCoarseTextures
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ContactLong;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> CoarseInfo;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> CoarseLayer;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DragState;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DragHold;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DentCoarse;
	/** 연속 굽힘의 거친 해 W_c(R32F, 모으기 패스가 쓰고 함몰 창이 읽는다 — 굽는 칸이 없으면 쓰지 않는다). */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ContactBend;
	/** 거친 격자 한 변 칸 수. */
	int32 Resolution = 0;
	double IdleSince = 0.0;

	bool IsValid() const { return IsComplete(); }
	static int32 NumTargets() { return 7; }
	TObjectPtr<UTextureRenderTarget2D>* Slot(int32 Index);
	const TObjectPtr<UTextureRenderTarget2D>* Slot(int32 Index) const { return const_cast<FSoftbodySurfaceCoarseTextures*>(this)->Slot(Index); }
	int64 CreateNextTarget(UObject* Outer);
	bool IsComplete() const;
	static FSoftbodySurfaceCoarseTextures Create(UObject* Outer, int32 Resolution);
	void Destroy();
	int64 Bytes() const;
};

/**
 * ADR-0003 표면 캐리어 접촉장의 GPU 상태. 캐리어 아틀라스와 같은 크기의 RT에 함몰(막 평형)을 유지하고
 * 매 틱 글로벌 컴퓨트 셰이더로 갱신한다. 머티리얼은 렌더 정점의 캐리어 UV로 결과 RT 두 장을 읽는다.
 *   GetDisplacementTexture: RGB 프레임 공간 변위(cm, 함몰 − 둘레 솟음 + 끌림), A 콜라이더 가장자리까지 부호 거리 + 2(칸 수, ADR-0029).
 *   GetNormalTexture: RGB 노멀 기울기 회전(축 × sin, 프레임), A 함몰 어둡기(0 = 그대로).
 *   GetLumpNormalTexture(덩어리가 있을 때): RGB 덩어리 이동이 기울인 노멀 회전, A 덩어리 이동의 노멀 성분(cm, ADR-0018).
 */
UCLASS(Transient)
class SOFTBODY_API USoftbodySurfaceField : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * 이 GPU에서 접촉(표면 장)을 돌릴 수 있는가. 접촉 패스는 지난 프레임 피부 자리를, 새 값을 쓰려고 묶어 둔 네 채널 텍스처에서
	 * 읽는다. D3D11·D3D12에서는 GPU가 따로 지원해야 하는 기능이고, 지원하지 않는 GPU에서 그 셰이더를 만들면 엔진이 멈춘다 — 그래서
	 * 그런 GPU에서는 접촉을 끈다(관성은 그대로 돈다). 처음 부를 때 결과를 로그에 한 번 남긴다(못 돌리면 경고).
	 */
	static bool IsSupported();
	/**
	 * 접촉 패스들의 컴퓨트 파이프라인(PSO)을 백그라운드에서 미리 만든다(한 번만, 렌더 스레드에 맡긴다). 드라이버 캐시가 빈 첫 실행에서 접촉 셰이더 하나가 몇 초씩
	 * 걸려, 처음 닿는 프레임에 그 자리에서 만들면 게임이 그만큼 멈췄다(쿡 빌드 3.6초, 일지 235). 엔진의 PSO 미리 만들기 설정과 상관없이(에디터 포함) 돈다.
	 */
	static void PrecachePipelines();
	/**
	 * 미리 만든 파이프라인이 다 준비됐는가(드라이버 컴파일이 끝났고 런타임 캐시에도 들어갔다). 아니면 월드 묶음이 이번 프레임 접촉 갱신을 미룬다 — 멈추는
	 * 대신 접촉이 늦게 시작한다. 준비 중이면 부를 때마다 런타임 캐시에 몇 개씩 더 넣는다.
	 */
	static bool ArePipelinesReady();
	/** 쓰지 않아 엔진 런타임 캐시에서 빠지지 않게 가끔 다시 찾는다(월드 묶음이 프레임마다 부른다, 20초마다 한 번 돈다). */
	static void KeepPipelinesAlive();
	/**
	 * 하드 서피스 순열(SB_HARD_SURFACE 1)의 파이프라인이 준비됐는가. 이 순열은 시작 때 전역으로 데우지 않는다 — 조각을 칠하지 않은 프로젝트의 접촉
	 * 시작을 늦추지 않게(비용 분리, 원칙 E06). 조각이 있는 장이 켜지면(ReservePool) 미리 만들기를 시작하고, 준비될 때까지 그 장의 갱신만 미룬다.
	 */
	static bool AreHardPipelinesReady();
	/** 이 장의 자산에 하드 서피스 조각이 구워져 있는가(표면 장 패스가 조각 순열을 쓴다). */
	bool UsesHardSurface() const;
	/**
	 * 캐리어가 있는 베이크 데이터를 받는다. 텍스처는 처음 닿을 때 잡는다(ADR-0022). 이미 같은 데이터면 아무것도 하지 않는다.
	 * 캐리어가 없거나 GPU가 접촉을 돌릴 수 없으면(IsSupported) false. 접촉 지도는 월드 묶음이 비동기로 나눠 올린다(일지 222·235 — 그동안 접촉 없이
	 * 관성만 돈다). bReservePool이면 게임 월드의 풀에 이 캐리어 종류를 쓰는 장으로 적는다(ReservePool) — 끈 채 세팅한 컴포넌트는 false로 두고 켤 때 부른다.
	 * 이 프레임에 무거운 일(렌더 타깃 만들기, 텍스처 올리기)은 하지 않는다 — 언제 켜고 끄든 프레임이 끊기지 않게(일지 235).
	 */
	bool Initialize(USoftbodyMeshData* InData, bool bReservePool = true);
	/**
	 * 월드 풀에 이 캐리어 종류(가는 벌·거친 벌)를 쓰는 장으로 적는다(게임 월드만): 풀이 쉬는 벌을 softbody.Surface.TexturePool 수까지 프레임 예산 안에서
	 * 미리 만들어 둔다 — 처음 닿는 순간 기다리지 않게. 한 번만 적는다(Shutdown이 지운다).
	 */
	void ReservePool();
	/**
	 * 표면 장을 내린다(소유 컴포넌트가 끄거나 접촉을 끌 때, 일지 222): 텍스처 두 벌(가는·거친)을 풀에 돌려주고, 풀 예약과 접촉 지도를 놓는다. GPU 갱신은
	 * 바로 0이 되고, 메모리는 월드 묶음이 softbody.Surface.ReleaseDelay초 동안 쓰이지 않으면 프레임마다 조금씩 놓는다(일지 235 — 그 안에 다시 켜면 다시
	 * 만들거나 읽지 않는다). 그 뒤로는 쓰지 않는다 — 다시 켤 때는 새 표면 장을 만든다.
	 */
	void Shutdown();
	/**
	 * 재운다(소유 컴포넌트를 끄거나 접촉을 끌 때, 일지 235): Shutdown처럼 텍스처 두 벌을 풀에, 풀 예약과 접촉 지도를 놓고 GPU 갱신을 멈춘다. 다만 렌더 스레드
	 * 상태(칸 종류 표·이웃 표·묶음 목록)와 이 오브젝트는 남겨, softbody.Surface.ReleaseDelay초 안에 다시 켜면(Wake) 첫 갱신이 그것을 다시 만들지 않는다 —
	 * 여럿을 한꺼번에 켜는 프레임이 렌더 스레드에서 끊기지 않게. 그 뒤에는 월드 묶음이 ReleaseRenderState를 부른다.
	 */
	void Sleep();
	/** 깨운다: 접촉 지도와 풀 예약을 다시 적는다(이 프레임에 무거운 일은 없다). 다음에 닿는 순간 "모든 장이 0"에서 시작한다. */
	void Wake(bool bReservePool);
	bool IsAsleep() const { return bAsleep; }
	/** 마지막으로 GPU 갱신한 월드 시간(초, 음수 = 아직 없음) — 구간 기록(Softbody.Record)이 갱신이 돈 틱을 적는다. */
	double GetLastUpdateTime() const { return LastUpdateTime; }
	/** 잠든 때(초, FPlatformTime). */
	double GetSleepSince() const { return SleepSince; }
	/** 렌더 스레드 상태(칸 종류 표·이웃 표·묶음 목록, 모양 거리장 버퍼)를 놓는다 — 잠든 채 ReleaseDelay가 지났을 때 월드 묶음이 부른다. */
	void ReleaseRenderState();
	/** 렌더 스레드 상태가 아직 없다: 다음 갱신이 표들을 처음 만든다(월드 묶음이 이런 갱신을 프레임마다 몇 개까지만 돌린다, softbody.Surface.FirstUpdates). */
	bool NeedsFirstUpdate() const { return !bRenderStateBuilt; }

	/**
	 * 한 프레임 갱신을 월드 묶음(USoftbodySurfaceBatch, ADR-0021)에 신청한다. 콜라이더가 없고 복원이 끝났으면 RT를 한 번 비우고
	 * 이후 갱신을 멈춘다. 반환값은 이번 프레임에 깨어 있어 신청했는지 — 실제로 GPU에 올렸는지는 액터 틱이 다 끝난 뒤 묶음이
	 * 갱신률 LOD로 정해 OnScheduled로 알려 준다(미루면 그 시간은 다음 갱신이 한 걸음에 푼다).
	 * 콜라이더가 있어도 눌린 칸이 없고 복원이 끝났으면 GPU가 접촉 패스 뒤의 패스들을 스스로 건너뛴다(ADR-0013).
	 */
	bool Update(const FSoftbodySurfaceFieldInput& Input);
	/**
	 * 묶음이 부른다(게임 스레드, 같은 프레임): 이번 프레임 GPU 갱신을 돌렸는가. 미룬 시간을 모으고 소유 컴포넌트에 알린다. 돌렸으면
	 * "장이 0" 표시를 풀고 실어 보낸 거리장 칸 값을 올라간 것으로 적는다 — 미룬 신청은 실은 것째로 버려지므로 장 쪽 상태는 여기서만 옮긴다.
	 */
	void OnScheduled(bool bRan);
	/** 렌더 스레드: 표면 장들의 한 프레임 갱신을 그래프 하나로 돌린다(인스턴스마다 패스들을 차례로 — 임시 텍스처를 돌려 쓴다, ADR-0022). */
	static void RenderBatch(FRHICommandListImmediate& RHICmdList, TArray<FSoftbodySurfaceFieldRender>& Inputs);

	UTextureRenderTarget2D* GetDisplacementTexture() const { return Displacement; }
	/** 지난 갱신의 변위(모션 벡터용). 머티리얼이 지난 프레임 정점 위치를 구할 때 읽는다. */
	UTextureRenderTarget2D* GetPreviousDisplacementTexture() const { return PreviousDisplacement; }
	UTextureRenderTarget2D* GetNormalTexture() const { return Normal; }
	/** 덩어리 이동이 기울인 노멀(ADR-0018). 캐리어에 덩어리 값이 없으면 nullptr. */
	UTextureRenderTarget2D* GetLumpNormalTexture() const { return LumpNormal; }
	/** 마지막 Update가 디스패치했고 그 프레임에 덩어리 노멀을 썼는가 — 머티리얼이 덩어리 음영을 표면 장에서 읽어도 되는가. */
	bool HasLumpNormals() const { return bLumpNormals; }

	/** 덩어리 평형 묶음 수 (셰이더 SB_LUMP_GROUPS). */
	static constexpr int32 NumLumpGroups = 16;
	/**
	 * 묶음마다 GPU 칸 합 값 수 (셰이더 SB_LUMP_VALUES): 미지수 19개(국소 모양 크기 16 + 출렁임 모양 크기 3, 일지 251)의 에너지 기울기 R(19),
	 * 헤세 J 위 삼각(190), 에너지 W(1). 칸 합 버퍼는 이 큰 판 크기로 잡는다 — 국소 모양이 없는 장은 미지수 셋 판(값 10개, 일지 254)이 앞쪽만 쓴다.
	 */
	static constexpr int32 NumLumpValues = 210;
	/** 묶음마다 상수 줄 수 (FSoftbodySurfaceFieldInput::LumpGroups, 셰이더 SB_LUMP_GROUP_ROWS). */
	static constexpr int32 NumLumpGroupRows = 7;
	/** 묶음마다 국소 모양 수(셰이더 SB_REGION_MODES, SoftbodyLumpRegion::MaxModes). */
	static constexpr int32 NumLumpRegionModes = 16;
	/** 묶음마다 결과 값 수 (GetLumpSolution, 셰이더 SB_LUMP_OUTPUTS). */
	static constexpr int32 NumLumpOutputs = 16;
	/**
	 * 갱신마다 덩어리 평형의 뉴턴 걸음 수. 지난 갱신의 답에서 이어 풀므로 2번이면 8번과 같은 자리에 닿는다(일지 254: 빠르게 밀고 떼는
	 * 누름·쥐고 끌기까지, 비용은 4번의 60 %). 처음(ADR-0017)에는 4였다.
	 */
	static constexpr int32 NumLumpSteps = 2;

	/**
	 * 덩어리 평형(ADR-0012, ADR-0017): GPU가 가장 최근에 푼 묶음별 결과(묶음 × NumLumpOutputs). 값: 평형 E(3, 기본 자세 축 하중 g),
	 * 그 자리의 살 강성 H_eff(6: xx xy xz yy yz zz — 국소 모양이 따라 움직인 뒤의 값, 일지 251), 풀 때 쓴 관성 목표 T(3), 1 = 힘이 있었다.
	 * InOutSerial보다 새 값이 있으면 채우고 true.
	 */
	bool GetLumpSolution(TArray<double>& OutSolution, uint64& InOutSerial) const;

	/**
	 * 잡기(ADR-0041): GPU가 가장 최근에 쓴 잡기 줄(SoftbodyGrab::MaxRows × SoftbodyGrab::StateRows float4, 셰이더 GrabState 배치 — 1~3프레임 늦다).
	 * InOutSerial보다 새 값이 있으면 채우고 true. 잡기가 있는 갱신만 읽는다.
	 */
	bool GetGrabState(TArray<FVector4f>& OutRows, uint64& InOutSerial) const;

	/** stat softbody: 표면 장 GPU 시간 줄(stat gpu의 Softbody 줄들과 같은 순서). */
	enum class EGpuTime : uint8
	{
		Contact,
		Tiles,
		Dent,
		Drag,
		Pressure,
		Compose,
		Lump,
		Num
	};

	/**
	 * stat softbody: 표면 장 GPU 시간(ms, 모든 표면 장의 합, GPU가 끝낸 가장 최근 프레임) — stat gpu의 Softbody 줄과 같은 값.
	 * 0.5초 넘게 새 값이 없는 줄(돌지 않는 패스)은 0.
	 */
	static void GetGpuTimes(double (&OutMs)[(int32)EGpuTime::Num]);

	/** stat softbody: GPU가 적은 이 표면 장의 상태(Input.bReadStats로 돈 프레임만 갱신, 몇 프레임 늦다). */
	struct FGpuState
	{
		/** 1 = 눌린 칸이 없고 복원이 끝나 뒤 패스들이 쉰다. */
		uint32 bResting = 0;
		/** 함몰을 푸는 16×16칸 묶음 수(ADR-0032). */
		uint32 DentTiles = 0;
		/**
		 * 끌림 원천(붙든 거친 칸 — 눌렸거나 기억이 남은 칸) 수(ADR-0033). 끄는 힘이 1 µm 이상인 원천이 하나도 없으면 0이다 — 그때는 모으기가 원천을 올리지
		 * 않아 비용이 바닥값이다(일지 179).
		 */
		uint32 DragSources = 0;
		/** 끌림이 닿은 거친 8×8칸 묶음 수(ADR-0033). */
		uint32 DragGroups = 0;
		/** 접촉 패스가 도는 8×8칸 묶음 수(표면·고스트 칸이 든 묶음). */
		uint32 ContactGroups = 0;
		/** 1 = 눌린 칸이 있다. */
		uint32 bPressed = 0;
		/** 끌림 묶음 원천 수(원천이 있는 거친 8×8 묶음, 일지 187). */
		uint32 DragClusters = 0;
		/** 그 가운데 아틀라스의 여러 조각이 섞여 뭉치지 않고 늘 원천마다 세는 묶음 수. */
		uint32 DragMixedClusters = 0;
		/**
		 * 끌림 바로잡기·모으기 패스 하나의 걸음 집계(일지 189). 묶음(8×8칸) 하나가 원천을 차례로 도는 걸음이 그 묶음의 시간이고, 패스 시간은 가장 긴
		 * 묶음이 정한다(개발 PC). 걸음 = 가까운 원천 하나 또는 먼 묶음 원천 하나를 세는 일.
		 */
		struct FDragWalk
		{
			/** 커널 합을 돈 묶음 수. */
			uint32 Groups = 0;
			/** 묶음 하나의 걸음 최대. */
			uint32 MaxSteps = 0;
			/** 모든 묶음의 걸음 합(GPU가 작아 묶음들이 차례로 돌 때의 비용). */
			uint32 SumSteps = 0;
			/** 묶음 하나가 가까운 묶음의 원천을 올린 횟수 최대(올릴 때마다 장벽 둘과 메모리 읽기를 기다린다). */
			uint32 MaxNearTiles = 0;
			/** 묶음 하나의 가까운 원천 걸음 최대. */
			uint32 MaxNear = 0;
			/** 묶음 하나의 먼 묶음 원천 걸음 최대. */
			uint32 MaxFar = 0;
		};
		FDragWalk DragCorrect;
		FDragWalk DragGather;
		/**
		 * 함몰 목록이 왜 이만큼인지(Softbody.Stats가 읽는 갱신만 센다). 서로 겹치지 않는다: 켜짐(이번에 눌렸거나 붙든 자리가 남은 묶음),
		 * 그 둘레(한 겹), 이음새로 든 묶음(고스트 칸이 켜짐 둘레 안을 가리키는 묶음, 앞의 둘 밖), 늘 켬 묶음(이음새 칸이 넘친 묶음, 앞의 셋 밖).
		 * DentCoreTiles = 하드 서피스 조각을 더하기 전 목록 수: 이음새·늘 켬 묶음을 넓힌 둘레 = 이것 − 앞 넷, 조각이 더한 묶음 = DentTiles − 이것.
		 */
		uint32 DentPressedTiles = 0;
		uint32 DentRingTiles = 0;
		uint32 DentLinkedTiles = 0;
		uint32 DentAlwaysTiles = 0;
		uint32 DentCoreTiles = 0;
		/** 1 = 부위 나누기(Separated Parts)가 구워져 켜진 장. DentPartsTiles = 함몰 목록 가운데 부위 채널 순열로 푼 묶음 수(나머지는 채널 없는 순열). */
		uint32 bDentParts = 0;
		uint32 DentPartsTiles = 0;
		/**
		 * 관통 계약 밖의 누름(원칙 P06, 일지 295): 그 갱신에 살 두께(Depth 칠을 곱한 두께)보다 깊이 눌린 표면 칸 수와, 그 가운데 가장 깊이
		 * 넘은 양(cm). 살 두께 안의 누름은 피부가 콜라이더 면에 닿고, 그 너머는 덜 그려져 콜라이더가 피부를 뚫는다. Depth 0 칸은 어떤 누름도 센다.
		 */
		uint32 DeepCells = 0;
		float DeepestBeyond = 0.f;
		/** 들어온 면 표지(일지 313) 자리 둘이 모자란 표면 칸 수 — 넘친 콜라이더는 그 갱신에 표지 없이 지금 규칙으로 잰다. */
		uint32 LabelOverflowCells = 0;
		/** 이 값을 적은 갱신 뒤로 지난 프레임 수(대략). */
		uint32 AgeFrames = 0;
	};
	bool GetGpuState(FGpuState& Out) const;

	/** 프로파일 이름(액터 이름, 한 액터에 소프트바디가 여럿이면 컴포넌트 이름까지). 첫 갱신에서 정한다. */
	const FString& GetDebugName() const { return DebugName; }
	/** 마지막 갱신의 표면 장 해상도와 끌림 거친 격자 해상도(0 = 아직 돌지 않음). */
	uint32 GetLastResolution() const { return LastResolution; }
	uint32 GetLastDragResolution() const { return LastDragResolution; }

	/** 이 표면 장이 만든 실행 중 텍스처의 메모리(바이트). 메시마다 하나인 캐리어 베이크 텍스처는 빼고. */
	int64 GetTextureMemoryBytes() const;

	virtual void BeginDestroy() override;
	/**
	 * 캐리어가 있는 데이터를 받았고 접촉 지도 텍스처가 메모리에 올라왔다(일지 222 — 월드 묶음이 비동기로 올린다. 그동안은 접촉 없이 관성만 돈다).
	 * 갱신을 넘어 남는 텍스처 두 벌은 닿을 때 잡는다(HasTextures).
	 */
	bool IsReady() const;
	/** 콜라이더 없이 복원이 끝나 장들을 0으로 비우고 멈춰 있는가(처음도 그렇다). 그동안 머티리얼은 표면 장을 읽을 필요가 없다. */
	bool IsCleared() const { return bCleared; }
	/** 이번 틱에 눌린 채 멈춰 갱신을 신청하지 않았는가(일지 203 — 깨어 있지만 GPU 비용 0, stat softbody "Still"). */
	bool IsStill() const { return bStill; }
	/** 갱신을 넘어 남는 텍스처 한 벌을 쥐고 있는가(닿은 뒤 복원이 끝나 비우기 전까지, ADR-0022). */
	bool HasTextures() const { return StateA != nullptr; }
	/** 텍스처 한 벌을 0으로 비워 월드 풀에 돌려주고 머티리얼 바인딩을 푼다(소유 컴포넌트가 표면 장을 내릴 때도 부른다). */
	void ReleaseTextures();
	/** 디버그 캡처(Softbody.CaptureField): 다음에 실제로 도는 갱신 끝에 저장할 장들의 사본을 떠 둔다 — 복사된 뒤 CaptureToFolder. */
	void RequestCapture();
	/** 디버그 캡처가 끝나 사본을 지운다. 이번 프레임 신청에 사본이 실려 있으면(IsCaptureSent) 부르지 않는다. */
	void ReleaseCapture();
	/** 디버그 캡처: 사본 요청이 아직 복사되지 않았다. */
	bool IsCapturePending() const { return bCapturePending; }
	/** 디버그 캡처: 이번 프레임 신청에 사본 타깃의 렌더 자원을 실었다 — 묶음이 돌기 전에 지우면 렌더 스레드가 지운 자원을 쓴다. */
	bool IsCaptureSent() const { return bCaptureSent; }
	/** 디버그 캡처: 지난 틱에 닿을 수 있는 콜라이더·손 부위가 있었다 — 텍스처가 아직 없어도 곧 잡아 돌 수 있다(캡처가 기다릴지 정한다). */
	bool HasCandidates() const { return bHadCandidates; }
	/** 디버그 캡처: 앞 캡처가 아직 저장되지 않았다(요청·복사·저장 대기 가운데 하나) — 새 요청은 이 장을 건너뛰고 그 수를 알린다. */
	bool IsCaptureBusy() const { return bCapturePending || bCaptureSent || bCaptureCopied; }
	/** 디버그 캡처: 사본을 뜬 갱신 때의 조절값 줄("  settings: ...", 사본이 없으면 지금 값). 정의는 SoftbodyFieldCapture.cpp. */
	FString GetCaptureSettings() const;
	/** 디버그 캡처: 소유 컴포넌트(USoftbodyComponent)의 조절값을 capture.txt 한 줄로 적는다. 정의는 SoftbodyFieldCapture.cpp. */
	static FString CaptureSettingsLine(const UObject* Owner);
	/**
	 * 디버그 캡처: 접촉 계산 파이프라인(PSO)이 준비됐는지 읽기만 한다. ArePipelinesReady·AreHardPipelinesReady는 묻는 것만으로 미리 만들기를 시작하거나
	 * 한 걸음 밀어서 캡처가 부르면 안 된다. 하드 서피스 파이프라인은 조각이 있는 장만 기다린다(UsesHardSurface).
	 */
	static void ReadPipelineStateForCapture(bool& bOutContactReady, bool& bOutHardReady);

	/**
	 * 콜라이더 형상을 프레임 공간 3×float4로 옮긴다(컨벡스는 평면을 OutPlanes에 덧붙이고, 그 뒤에 로컬 정점 상자 반폭 한 줄 — 셰이더 SB_PresserBreadth).
	 * MaxShapes·MaxPlanes를 넘는 형상은 건너뛰며 건너뛴 수를 돌려준다. OutSourceIndices를 주면 실은 형상마다
	 * WorldShapes 번호를 적는다(끌림이 콜라이더의 지난 프레임 위치를 찾는 데 쓴다).
	 */
	static int32 PackShapes(TConstArrayView<FSoftbodyColliderShape> WorldShapes, const FTransform& ComponentTransform,
		int32 MaxShapes, int32 MaxPlanes, TArray<FVector4f>& OutShapes, TArray<FVector4f>& OutPlanes,
		TArray<int32>* OutSourceIndices = nullptr);

	/**
	 * 모양 거리장의 칸 값을 GPU 버퍼에 올릴 목록에 넣고, 그 첫 칸 번호를 돌려준다(이미 넣은 거리장이면 그 번호).
	 * 버퍼는 새 거리장이 들어올 때만 다시 올린다(실제로 돌린 갱신에 — 미룬 신청은 싣기만 하고 버려진다).
	 */
	int32 RegisterColliderField(const USoftbodyColliderField* Field);

	/**
	 * 디버그(콘솔 명령 Softbody.CaptureField): 요청 뒤 첫 갱신 끝에 뜬 사본들을 Folder에 EXR로, 눌린 곳을 키운 PNG로 저장하고 요약을
	 * OutReport에 덧붙인다. 정의는 SoftbodyFieldCapture.cpp.
	 */
	bool CaptureToFolder(const FString& Folder, const FString& Prefix, bool bFullAtlas, FString& OutReport) const;

private:
	UPROPERTY()
	TObjectPtr<USoftbodyMeshData> Data;

	/** 하드 서피스 조각 표(셰이더 꼴) — Data에 조각이 구워져 있으면 처음 갱신 때 한 번 만든다(Initialize가 데이터를 바꾸면 비운다). */
	TSharedPtr<const FSoftbodyHardPieceTable, ESPMode::ThreadSafe> HardTable;

	/**
	 * 접촉 지도 텍스처(일지 222): Mesh Data는 캐리어·거리표 텍스처를 약한 참조로 쥐어, 켜진 표면 장이 있을 때만 메모리에 있다. Initialize가 비동기로
	 * 올리고(이미 있으면 바로) 다 올라오면 여기 쥔다 — 이 장이 사라지면(Shutdown·GC) 쥔 것이 없는 텍스처는 다음 GC에 내려간다.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> ContactMapRefs;
	TSharedPtr<struct FStreamableHandle> ContactMapHandle;
	bool bContactMapLoaded = false;
	/** 비동기로 올린 접촉 지도를 쥐고 준비 표시를 한다. 하나라도 못 올렸으면 한 번 알리고 접촉을 끈 채 둔다. */
	void OnContactMapLoaded();
	/** 풀에 이 캐리어 종류를 쓰는 장으로 적었다(ReservePool) — 해상도와 덩어리 유무, 거친 격자 한 변. */
	bool bPoolReserved = false;
	int32 ReservedResolution = 0;
	bool bReservedLump = false;
	int32 ReservedCoarseResolution = 0;

	FString DebugName;
	uint32 LastResolution = 0;
	uint32 LastDragResolution = 0;

	// 갱신을 넘어 남는 텍스처(ADR-0022, FSoftbodySurfaceTextures 한 벌): 처음 닿을 때 월드 풀에서 잡고(AcquireTextures) 콜라이더 없이
	// 복원이 끝나면 0으로 비워 돌려준다(ReleaseTextures). 갱신 안에서만 쓰는 것(접촉 노멀·끌림·덩어리 깊이, 함몰·끌림 반복 결과)은
	// RenderBatch가 그래프 안 임시 텍스처로 잡아 인스턴스끼리 같은 메모리를 돌려 쓴다.
	/**
	 * 함몰 u(cm, 한 채널). 갱신마다 함몰 두 패스가 임시 텍스처에 풀고 끌림 마지막 패스가 A로 옮긴다 — 갱신이 끝나면 늘 A에 있다(다음 갱신의
	 * 출발 값, ADR-0032). 끌림은 거친 격자(DragState)에 있다(일지 161).
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> StateA;

	/**
	 * 누른 자리 기억 F(cm, 한 채널, ADR-0032): 칸마다 max(이번 누른 깊이, 지난 F × e^{−R·Δt}). 함몰은 피부를 max(F, 받아들인 깊이)에 붙든
	 * 살층의 정적 해다. 함몰 둘째 패스가 제자리에서 갱신한다.
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Hold;

	/**
	 * 접촉 패스 결과: xyz 현재 피부 위치(프레임 cm), w 1 + 누른 것 번호(안 눌리면 1). 이웃 칸 차분용이라 32비트. 다음 갱신의 접촉 패스가
	 * 덮어쓰기 전에 지난 위치와 지난 눌림으로 읽는다(끌림).
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Position;

	/** 머티리얼이 읽는 최종 변위. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Displacement;

	/**
	 * 지난 갱신의 최종 변위. 갱신마다 새 변위를 쓰기 전에 복사해 둔다. 머티리얼의 지난 프레임 계산(속도 버퍼)이 읽어서, 눌린
	 * 살이 움직이는 만큼 TSR·모션 블러가 속도를 안다 — 없으면 움직이는 함몰에 잔상이 남는다.
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> PreviousDisplacement;

	/** 머티리얼이 읽는 노멀 회전과 함몰 어둡기. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Normal;

	/**
	 * 살이 받아들인 깊이(cm, ADR-0003 §14). 함몰은 이 깊이까지만 돌아간다. 오래 누를수록 함몰을 따라 깊어지고, 떼면 천천히
	 * 0으로 돌아간다. 이완 패스가 제자리에서 갱신한다.
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Accepted;

	/**
	 * 들어온 면 표지(일지 313·314): 칸마다 콜라이더 둘까지 "이 콜라이더는 이 면으로, 이 방향으로 들어왔다". 접촉 패스가 사건(콜라이더가 쉬는 피부 점을
	 * 품거나 한 갱신에 지나 들어감)에서 달고, 그 점이 들어온 방향으로 더는 콜라이더 밑이 아니면 지운다. 다음 갱신의 접촉 패스가 같은 칸에서 제자리로 읽는다.
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> Labels;

	/** 덩어리 노멀(ADR-0018): 접촉 패스가 매 프레임 쓴다. 덩어리 값이 있는 캐리어만 만든다. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> LumpNormal;
	bool bLumpNormals = false;

	/**
	 * 손 닻(일지 201): 눌린 칸마다 "덩어리가 안 움직였다면 손이 이 칸의 피부를 끌고 간 자리"(덩어리 기본 자세 축 접선 두 성분, cm, RG32F — 반정밀도는
	 * 손 움직임을 프레임마다 더해 쌓는 값이라 표류한다). 접촉 패스가 읽고 덩어리 힘 패스 첫 걸음이 마찰 한도 안으로 잘라 쓴다. 덩어리 값이 있는
	 * 캐리어만 만든다(LumpNormal과 같은 벌).
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> LumpAnchor;

	/**
	 * 디버그 캡처(Softbody.CaptureField): 요청 뒤 실제로 돈 첫 갱신 끝에 뜬 사본(차례 SoftbodyCapture::ECopy). 접촉 텍스처는 임시라 지난 값이 없고, 나머지
	 * 장도 저장할 때 읽으면 사본을 뜬 뒤 돈 갱신의 값이라 한 폴더에 두 순간이 섞였다. 요청한 동안만 있고 캡처 뒤 지운다.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> CaptureCopies;
	/** 사본 요청이 살아 있다 / 이번 프레임 신청에 사본 요청을 실었다 / 사본이 복사됐다. */
	bool bCapturePending = false;
	bool bCaptureSent = false;
	bool bCaptureCopied = false;
	/** 사본을 뜬 갱신의 신청 번호와 프레임, 그 갱신의 거친 격자 값(저장할 때의 DragBlock·Spread*는 그 뒤 신청의 값일 수 있다). */
	uint64 CaptureSerial = 0;
	uint64 CaptureFrame = 0;
	int32 CaptureDragBlock = 0;
	int32 CaptureSpreadBlock = 0;
	float CaptureSpreadTexelSize = 0.f;
	int32 CaptureSpreadLandmarks = 0;
	/** 사본을 뜬 갱신 때의 조절값 줄(CaptureSettingsLine) — 저장할 때 읽으면 그 사이 바꾼 값이 섞인다. */
	FString CaptureSettings;
	/** 사본을 뜬 갱신이 본 콜라이더 행과 그때의 거리장 칸 값 수(박힘 분석, 일지 340 — 저장할 때의 LastRunInput은 그 뒤 갱신의 것일 수 있다). */
	TArray<FVector4f> CaptureShapes;
	TArray<FVector4f> CapturePlanes;
	TArray<FVector4f> CaptureFieldInstances;
	int32 CaptureVoxelWords = 0;
	/** 디버그 캡처: 사본 타깃을 지금 장과 같은 형식·크기로 맞추고(다르면 다시 만든다) 이번 신청에 싣는다. */
	void PrepareCaptureCopies(FSoftbodySurfaceFieldRender& RI);
	/** 디버그 캡처: 접촉 압력 풀이 읽기 칸(요청한 동안만 있고 렌더 스레드만 만진다 — FSoftbodyPressureCapture). */
	TSharedPtr<FSoftbodyPressureCapture, ESPMode::ThreadSafe> PressureCapture;
	/** 디버그 캡처: 사본을 뜬 갱신의 압력 풀이 버퍼를 CPU로 읽는다(렌더 스레드와 GPU를 기다린다 — 저장할 때만). 그 갱신에 풀이가 없었으면 false. */
	bool ReadPressureCapture(FSoftbodyPressureCaptureData& Out) const;

	/** 텍스처 풀과 접촉 지도(월드 묶음). 월드 밖이면 비어 있고 제 텍스처를 만들고 접촉 지도를 직접 올린다. */
	TWeakObjectPtr<USoftbodySurfaceBatch> Pool;
	/** 월드 묶음에 접촉 지도를 쓰는 장으로 적었다(Shutdown이 지운다). */
	bool bContactMapAcquired = false;
	/** 잠들었다(Sleep) / 잠든 때 / 렌더 스레드 상태를 만든 갱신이 돌았다(ReleaseRenderState가 지운다). */
	bool bAsleep = false;
	double SleepSince = 0.0;
	bool bRenderStateBuilt = false;
	/** 월드 묶음이 없을 때(월드 밖) 접촉 지도를 직접 비동기로 올린다. */
	void LoadContactMapDirect();
	/** 접촉 지도가 다 올라왔는지 묶음에 묻고, 올라왔으면 준비 표시를 하고 살 두께 백분위를 정한다. 아직이면 false. */
	bool PollContactMap();
	/** 이 크기(컴포넌트 스케일 평균)에서 거친 격자 한 변 칸 수. 굽기가 거리표와 같이 정한 묶음 크기(없으면 같은 규칙으로 여기서). */
	int32 CoarseResolutionFor(float ThicknessScale) const;
	/**
	 * 처음 닿는 순간: 풀에서 쉬는 두 벌(가는·거친)을 잡고 머티리얼에 잇는다. 풀에 없으면 false — 원한다고 적어 두면 풀이 프레임 예산 안에서 만들고, 그동안은
	 * 이 장이 갱신하지 않는다(몇 프레임 늦게 눌린다, 일지 235). 월드 밖이면 바로 만든다.
	 */
	bool AcquireTextures(int32 CoarseResolution);

	/** 덩어리 힘 합을 GPU에서 읽는 고리(렌더 스레드)와 가장 최근 값(게임 스레드가 읽는다). */
	TSharedPtr<FSoftbodyLumpReadback, ESPMode::ThreadSafe> LumpReadback;

	/**
	 * 접촉 압력(ADR-0037) 거친 격자(캐리어 칸 SpreadBlock × SpreadBlock이 한 칸): 거친 칸마다 (긴 몫 L — 살층 응답이 둘레로 올리는 몫(cm), 거친 압력 p)
	 * RG32F. 접촉 단계 Full의 갱신마다 모든 칸에 새로 쓴다.
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ContactLong;
	/** 연속 굽힘의 거친 해 W_c(cm, R32F — 거친 벌에서 ContactLong과 같이 잡는다). */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ContactBend;

	/**
	 * 솟음 거친 칸의 고정 표(CoarseInitCS가 캐리어에서 한 번 만든다): Info xyz 살 칸 정지 위치 평균(메시 로컬), w 칸 넓이(메시 로컬², 0 =
	 * 살 없는 칸). Layer x 두께 평균, y 렌더 삼각형 변 평균(메시 로컬).
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> CoarseInfo;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> CoarseLayer;

	/**
	 * 끌림 거친 격자(일지 161, ADR-0033) = 솟음 거친 격자(캐리어 칸 SpreadBlock × SpreadBlock이 한 칸). State xyz 끌림(프레임 cm), w = 지난 갱신에
	 * 눌렸으면 1 + 덮인 몫, 아니면 0. Hold = 붙든 자리와 끌림의 차(눌린 칸만)와 끄는 힘 Q(원천 칸, 뗀 칸은 줄어드는 기억)를 반정밀도로
	 * 묶은 것(셰이더 SB_DragPackHold, 일지 184 — 일지 183의 Pull을 합쳤다) — 둘 다 갱신을 넘어 남는다(표면 장이 처음 돌 때 0으로 비운다).
	 * 이웃 표(DragLinksCS)는 캐리어로만 정해지는 표라 칸 종류 표처럼 렌더 스레드의 쉬기 상태(RestState)가 쥔다.
	 */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DragState;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DragHold;

	/** 함몰 거친 해(일지 199): 거친 칸마다 함몰(cm, R32F, 끌림 격자와 같은 크기) — 갱신을 넘어 남는다(렌더 스레드가 처음 0으로 비운다). */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DentCoarse;

	/** 지난 갱신의 끌림 거친 격자 묶음 한 변 칸 수(= SpreadBlock, 캡처가 읽는다). */
	int32 DragBlock = 4;

	/** 캐리어 표면 칸 살 두께의 90번째 백분위(메시 로컬 단위, 0 = 모름). Initialize가 정한다(이완의 체비쇼프 범위에 쓴다). */
	float SpreadThicknessHigh = 0.f;
	/** 지난 갱신의 거친 격자: 묶음 한 변 칸 수, 거친 칸 크기(cm), 랜드마크 수(솟음이 꺼져 있으면 0). 캡처가 적는다. */
	int32 SpreadBlock = 1;
	float SpreadTexelSize = 0.f;
	int32 SpreadLandmarks = 0;
	/** 거리표 없는 옛 베이크라 솟음·끌림을 껐다는 경고를 한 번만 낸다. */
	bool bBulgeTableWarned = false;
	/** 판 2 미만 옛 캐리어라 접촉을 껐다는 경고를 한 번만 낸다. */
	bool bCarrierVersionWarned = false;

	/** 콜라이더가 떠난 뒤 복원이 끝난 정도(1 = 다 끝나 비워도 된다). 실제로 돌린 갱신이 줄인 몫을 더한다(SettleOfStep). */
	double IdleSettle = 0.0;
	/** 이번 틱에 신청한 갱신이 돌면 복원을 끝내는 몫(미룬 시간까지 한 걸음). */
	double StepSettle = 0.0;
	/** 이번 틱에 메시가 화면에 있었는가(FSoftbodySurfaceFieldInput::bInView). */
	bool bInView = true;
	/** 장이 모두 0인가(처음·복원 끝). 닿아서 신청해도 묶음이 실제로 돌린 뒤(OnScheduled)에야 풀린다 — 미룬 갱신은 장에 쓰지 않는다. */
	bool bCleared = true;
	// 갱신률 LOD(ADR-0021)
	/** 미룬 프레임들의 시간 합(초). 다음 갱신이 이번 틱 시간과 합쳐 한 걸음에 푼다. */
	float PendingDeltaTime = 0.f;
	/** 마지막 Update가 받은 틱 시간(초). */
	float LastDeltaTime = 0.f;
	/** 마지막 GPU 갱신 뒤 미룬 프레임 수. */
	int32 FramesSinceUpdate = 0;
	/** 마지막으로 GPU 갱신한 월드 시간(초, 음수 = 아직 없음)과 갱신 주기 상한의 차례(일지 156, FSoftbodySurfaceSchedule). */
	double LastUpdateTime = -1.0;
	float RatePhase = NextRatePhase();
	static float NextRatePhase();
	/** 지난 틱에 닿을 수 있는 콜라이더·손 부위가 있었는가(닿는 순간을 알아보는 데 쓴다). */
	bool bHadCandidates = false;
	// 멈춤(일지 203, ADR-0021 §8): 눌린 채 입력이 마지막으로 돌린 갱신과 같고(허용 오차 softbody.Surface.StillDistance) 그 뒤 돌린 갱신의 GPU 표지가
	// "눌림·변위 안 바뀜"이면 신청하지 않는다 — 평형이라 다시 돌려도 같은 값을 쓴다. 깨우기는 CPU가 같은 프레임에(입력이 움직이면 바로 신청), 재우기만 몇
	// 프레임 늦은 GPU 표지로.
	/** 신청 번호(신청마다 1씩, 읽기 표지와 맞춘다) / 이번 신청의 번호 / 마지막으로 실제 돌린 신청의 번호. */
	uint64 UpdateSerial = 0;
	uint64 PendingSerial = 0;
	uint64 LastRunSerial = 0;
	/** 이번 신청의 입력(돌렸으면 LastRunInput으로 옮긴다) / 마지막으로 실제 돌린 갱신의 입력(멈춤 비교의 기준). */
	FSoftbodySurfaceFieldInput PendingInput;
	FSoftbodySurfaceFieldInput LastRunInput;
	bool bHasLastRunInput = false;
	/** 입력이 마지막 돌린 갱신과 같아진 뒤 첫 신청 번호(0 = 움직이는 중). 이 번호 이상의 읽기 표지가 "눌림·안 바뀜"이면 멈춘다. */
	uint64 StillSinceSerial = 0;
	/** 이번 틱에 멈춰 신청하지 않았다. */
	bool bStill = false;
	/** 결과를 바꿀 수 있는 입력이 모두 허용 오차 안인가(거리 Distance cm, 회전은 바운드 반지름으로 거리로). */
	static bool InputStill(const FSoftbodySurfaceFieldInput& A, const FSoftbodySurfaceFieldInput& B, float Distance);
	/** 멈춘 틱: 미룬 틱과 같이 다루되 시간은 모으지 않는다. */
	void OnStill();
	/** 이번 프레임 신청을 받은 묶음(OnScheduled가 오면 비운다). 사라질 때 신청을 뺀다. */
	TWeakObjectPtr<USoftbodySurfaceBatch> PendingBatch;

	/** 올린 모양 거리장과 각각의 첫 칸 번호. 칸 값은 반정밀 두 개씩 묶어 FieldVoxelWords에 이어 붙인다. */
	TArray<TWeakObjectPtr<const USoftbodyColliderField>> RegisteredFields;
	TArray<int32> RegisteredFieldBases;
	TArray<uint32> FieldVoxelWords;
	/**
	 * 칸 값 목록의 판 번호: 새 거리장이 들어올 때마다 1씩 올라간다 / 마지막으로 신청에 실은 판 / GPU에 올라간 판. 미룬 신청은 실은
	 * 것째로 버려지므로 올라간 판은 실제로 돌린 뒤(OnScheduled)에만 옮긴다 — 둘이 다르면 다음 신청에 다시 싣는다.
	 */
	uint32 FieldVoxelVersion = 0;
	uint32 FieldVoxelSentVersion = 0;
	uint32 FieldVoxelUploadedVersion = 0;
	/** 렌더 스레드가 들고 있는 칸 값 버퍼 (새 거리장이 들어올 때만 다시 만든다). */
	TSharedPtr<FSoftbodyFieldVoxelBuffer, ESPMode::ThreadSafe> FieldVoxelBuffer;
	/** 쉬기(ADR-0013): 마지막으로 눌린 뒤 지난 프레임 수를 GPU에 들고 있는 버퍼(렌더 스레드). */
	TSharedPtr<FSoftbodySurfaceRestState, ESPMode::ThreadSafe> RestState;
};
