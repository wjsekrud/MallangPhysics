// Copyright (c) 2026 wjsekrud

// 디버그: PIE에서 표면 장이 매 틱 계산하는 RT를 파일로 저장한다(콘솔 명령 Softbody.CaptureField).
#include "SoftbodySurfaceField.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "SoftbodyComponent.h"
#include "SoftbodyContactResponse.h"
#include "SoftbodyMeshData.h"
#include "SoftbodyRecord.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyCapture, Log, All);

namespace
{
	/** 미리보기 PNG는 잘라낸 영역의 긴 변이 이 픽셀 수쯤 되게 칸을 키운다(칸 하나 = 정사각형 한 덩어리). */
	constexpr int32 PreviewSize = 1024;
	constexpr int32 MaxPreviewScale = 16;
	/** 눌리거나 솟은 칸 둘레로 더 잘라 넣는 칸 수. */
	constexpr int32 CropMargin = 16;
	/** 잘라낼 영역: 누른 칸과, 표면 높이 변화가 가장 큰 값의 이 비율을 넘는 칸. */
	constexpr float ActiveHeightRatio = 0.02f;
	/** 칸이 없는 곳(아틀라스 빈자리) 색. */
	const FColor EmptyColor(40, 16, 40);

	bool ReadTarget(UTextureRenderTarget2D* Target, FImage& Out)
	{
		if (!Target || !FImageUtils::GetRenderTargetImage(Target, Out))
		{
			return false;
		}
		Out.ChangeFormat(ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		return true;
	}

	FImage CropImage(const FImage& Source, const FIntRect& Rect)
	{
		FImage Out(Rect.Width(), Rect.Height(), ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		TArrayView64<const FLinearColor> From = Source.AsRGBA32F();
		TArrayView64<FLinearColor> To = Out.AsRGBA32F();
		for (int32 Y = 0; Y < Rect.Height(); ++Y)
		{
			for (int32 X = 0; X < Rect.Width(); ++X)
			{
				To[(int64)Y * Rect.Width() + X] = From[(int64)(Rect.Min.Y + Y) * Source.SizeX + Rect.Min.X + X];
			}
		}
		return Out;
	}

	uint8 ToByte(float Unit)
	{
		return (uint8)FMath::RoundToInt(255.f * FMath::Clamp(Unit, 0.f, 1.f));
	}

	FColor Gray(float Unit)
	{
		const uint8 V = ToByte(Unit);
		return FColor(V, V, V);
	}

	/** 양수는 주황, 음수는 파랑. 밝을수록 크다. */
	FColor Signed(float Value, float Scale)
	{
		const float T = FMath::Abs(Value) / FMath::Max(Scale, 1e-6f);
		return Value >= 0.f ? FColor(ToByte(T), ToByte(0.55f * T), ToByte(0.15f * T))
			: FColor(ToByte(0.15f * T), ToByte(0.55f * T), ToByte(T));
	}

	/** 머티리얼 SB_RotateByTilt와 같은 식: 회전 C(축 × sin)만큼 노멀 N을 돌린다. */
	FVector3f RotateByTilt(const FVector3f& C, const FVector3f& N)
	{
		const float SinSq = FMath::Min(C.SizeSquared(), 1.f);
		if (SinSq < 1e-10f)
		{
			return N;
		}
		const float Cos = FMath::Sqrt(1.f - SinSq);
		return Cos * N + FVector3f::CrossProduct(C, N) + C * (FVector3f::DotProduct(C, N) / (1.f + Cos));
	}

	/** Rect 영역의 칸 하나를 Scale×Scale 픽셀로 칠해 PNG로 저장한다. */
	bool SavePreview(const FString& Path, const FIntRect& Rect, int32 Scale, TFunctionRef<FColor(int64 Index)> Paint, int32 Size)
	{
		const int32 Width = Rect.Width() * Scale;
		FImage Out(Width, Rect.Height() * Scale, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		TArrayView64<FColor> Pixels = Out.AsBGRA8();
		for (int32 Y = 0; Y < Rect.Height(); ++Y)
		{
			for (int32 X = 0; X < Rect.Width(); ++X)
			{
				const FColor Color = Paint((int64)(Rect.Min.Y + Y) * Size + Rect.Min.X + X);
				for (int32 PY = 0; PY < Scale; ++PY)
				{
					for (int32 PX = 0; PX < Scale; ++PX)
					{
						Pixels[(int64)(Y * Scale + PY) * Width + X * Scale + PX] = Color;
					}
				}
			}
		}
		return FImageUtils::SaveImageByExtension(*Path, Out);
	}

	// 접촉 압력 풀이 버퍼 배치(셰이더 SB_PRESSURE_*·SB_HARD_*, SoftbodySurfaceField.cpp의 PressureCapacity 등과 짝)
	constexpr uint32 PressureCap = 768;            // SB_PRESSURE_CAP
	constexpr uint32 PressureCellRows = 13;        // SB_PRESSURE_ROWS
	constexpr uint32 PressureLevelCount = 3;      // SB_PRESSURE_LEVELS
	constexpr uint32 HardHandLineRows = 17;       // SB_HARD_HAND_ROWS
	constexpr uint32 HardHandEmitCap = 512;       // SB_HARD_HAND_EMIT
	constexpr uint32 HardHandSolveCap = 256;      // SB_HARD_HAND_CAP
	constexpr uint32 HardCandidateCap = 32;       // SB_HARD_CANDIDATES
	// 하드 서피스 일 버퍼 낱말(SB_HARD_WORK_* — 낱말 7은 비어 있다)
	constexpr uint32 WorkHands = 0, WorkCandidates = 1, WorkTouched = 2, WorkFRows = 3, WorkOverflow = 5, WorkFree = 6,
		WorkBits = 8, WorkPiece = 136, WorkStart = 168, WorkCount = 200, WorkWords = 232;

	uint32 FloatBits(float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	float BitsFloat(uint32 Bits)
	{
		float Value = 0.f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	/** 셰이더 SB_PressureUnpackNormal과 같은 식: 8면체 좌표 둘(반정밀)을 단위 벡터로. */
	FVector3f UnpackPushNormal(uint32 Word)
	{
		FFloat16 HalfX;
		FFloat16 HalfY;
		HalfX.Encoded = (uint16)(Word & 0xffffu);
		HalfY.Encoded = (uint16)(Word >> 16);
		const float PX = HalfX.GetFloat();
		const float PY = HalfY.GetFloat();
		FVector3f N(PX, PY, 1.f - FMath::Abs(PX) - FMath::Abs(PY));
		if (N.Z < 0.f)
		{
			N.X = (1.f - FMath::Abs(PY)) * (PX >= 0.f ? 1.f : -1.f);
			N.Y = (1.f - FMath::Abs(PX)) * (PY >= 0.f ? 1.f : -1.f);
		}
		return N.GetSafeNormal();
	}

	FString CsvNum(float Value)
	{
		return FString::Printf(TEXT("%.7g"), Value);
	}

	/**
	 * 접촉 압력 풀이(ADR-0037, 하드 서피스 ADR-0038 §4)를 CSV 둘로 저장하고 capture.txt 줄을 OutReport에 덧붙인다. 줄 번호는 풀이 스레드 차례 그대로다
	 * (셰이더 SB_HardSolve): 조각 밖 칸 0..N_O − 1, 발자국 줄 N_O..N_K − 1, 손 칸 줄 N_K부터(깊은 256줄 — 차례는 SB_HardSortHands를 여기서 다시 센다).
	 * 풀린 값은 행렬 줄(N_K 앞)만 버퍼에 남는다 — 손 칸 줄 힘 칸은 비운다.
	 */
	bool SavePressureCapture(const FString& Folder, const FString& Prefix, const FSoftbodyPressureCaptureData& P, FString& OutReport)
	{
		if (P.Header.Num() <= 12 || P.Cells.Num() < (int32)(PressureLevelCount * PressureCap * PressureCellRows) || P.Force.Num() < (int32)PressureCap)
		{
			OutReport += TEXT("  contact pressure solve: none in that update (Contact Detail Basic, or no bulge distance table) - no pressure CSVs\n");
			return true;
		}
		const uint32 Level = FMath::Min(P.Header[11], PressureLevelCount - 1);
		const uint32 NK = FMath::Min(P.Header[12], PressureCap);
		const bool bHard = P.HardWork.Num() >= (int32)WorkWords;
		const uint32 NO = bHard ? FMath::Min(P.HardWork[WorkFree], NK) : NK;
		const uint32 Cands = bHard ? FMath::Min(P.HardWork[WorkCandidates], HardCandidateCap) : 0u;
		const FSoftbodyHardPieceTable* Table = P.HardTable.Get();
		auto Cell = [&](uint32 Slot, uint32 Row) -> const FUintVector4& { return P.Cells[(int32)((Level * PressureCap + Slot) * PressureCellRows + Row)]; };
		auto CandPiece = [&](uint32 Cand) { return P.HardWork[WorkPiece + Cand]; };
		// 조각 종류(셰이더 SB_HardKindOf): 0 판이 아님(칠대로 굽는 조각 — 후보가 되지 않는다), 1 붙은 강체, 2 떠 있음 — 조각 틀과 Flex Offset으로 풀이와
		// 같게. 표가 없으면 풀이가 쓴 켜짐 값으로.
		auto KindOf = [&](uint32 Piece) -> int32
		{
			if (Table && Table->Frames.IsValidIndex((int32)(Piece * 3 + 2)))
			{
				const float Raw = Table->Frames[(int32)(Piece * 3 + 2)].W;
				return Raw < SoftbodyContactResponse::FloatingPieceKind + 0.5f ? 2 : (Raw + P.FlexOffset <= 0.f ? 1 : 0);
			}
			if (P.PieceMotion.IsValidIndex((int32)Piece) && P.PieceMotion[(int32)Piece].W > 0.5f)
			{
				return FMath::Clamp(FMath::RoundToInt(P.PieceMotion[(int32)Piece].W) - 1, 0, 2);
			}
			return -1;
		};
		auto KindName = [](int32 Kind) { return Kind == 0 ? TEXT("bending") : Kind == 1 ? TEXT("attached_rigid") : Kind == 2 ? TEXT("floating") : TEXT("unknown"); };
		TArray<int32> CandKind;
		for (uint32 Cand = 0; Cand < Cands; ++Cand)
		{
			CandKind.Add(KindOf(CandPiece(Cand)));
		}

		// 조각 위 손 칸 줄(압력 칸 패스가 쓴 차례). 풀이가 받는 줄은 셰이더 SB_HardSortHands처럼: 후보 조각 위 줄 가운데 깊은 256줄(같으면 열쇠가 작은
		// 줄), 받은 줄은 열쇠 (후보, 거친 칸 y, x) 차례로 N_K부터.
		struct FHandLine
		{
			uint32 Piece = 0;
			uint32 Packed = 0;
			float Samples = 0.f;
			float Depth = 0.f;
			int32 Cand = -1;
			uint32 Key = MAX_uint32;
			bool bTaken = false;
			int32 Row = -1;
		};
		TArray<FHandLine> Hands;
		// 쓴 줄은 모두 적는다(내보내기 상한 512 안). 풀이는 닿은 후보가 있을 때만 줄을 받는다 — 줄이 있는 조각은 늘 닿은 조각이라 후보가 아니면 받지 않는 것과 같다.
		const uint32 NumHandLines = bHard ? FMath::Min(P.HardWork[WorkHands], HardHandEmitCap) : 0u;
		for (uint32 Raw = 0; Raw < NumHandLines && (int32)((Raw + 1) * HardHandLineRows) <= P.HardHand.Num(); ++Raw)
		{
			const FVector4f& Head = P.HardHand[(int32)(Raw * HardHandLineRows)];
			FHandLine& Line = Hands.AddDefaulted_GetRef();
			Line.Piece = (uint32)Head.X;
			Line.Packed = (uint32)Head.Y;
			Line.Samples = Head.Z;
			Line.Depth = Head.W;
			for (uint32 Cand = 0; Cand < Cands; ++Cand)
			{
				Line.Cand = CandPiece(Cand) == Line.Piece ? (int32)Cand : Line.Cand;
			}
			Line.Key = Line.Cand >= 0 ? ((uint32)Line.Cand << 24) | ((Line.Packed >> 12) << 12) | (Line.Packed & 0xfffu) : MAX_uint32;
		}
		TArray<int32> Taken;
		for (int32 Index = 0; Index < Hands.Num(); ++Index)
		{
			FHandLine& Line = Hands[Index];
			if (Line.Key == MAX_uint32)
			{
				continue;
			}
			uint32 Deeper = 0;
			for (const FHandLine& Other : Hands)
			{
				Deeper += Other.Key != MAX_uint32 && (Other.Depth > Line.Depth || (Other.Depth == Line.Depth && Other.Key < Line.Key)) ? 1u : 0u;
			}
			Line.bTaken = Deeper < HardHandSolveCap;
			if (Line.bTaken)
			{
				Taken.Add(Index);
			}
		}
		Taken.Sort([&Hands](int32 A, int32 B) { return Hands[A].Key < Hands[B].Key; });
		TArray<int32> CandHands;
		CandHands.SetNumZeroed((int32)Cands);
		for (int32 Rank = 0; Rank < Taken.Num(); ++Rank)
		{
			FHandLine& Line = Hands[Taken[Rank]];
			Line.Row = (int32)NK + Rank;
			++CandHands[Line.Cand];
		}
		const uint32 Valid = (uint32)Taken.Num();

		// 풀이 줄 CSV: 칸 하나에 열 하나씩, 그 줄에 없는 값은 비운다
		enum EColumn : int32 { ColRow, ColRole, ColCand, ColPiece, ColCoarseX, ColCoarseY, ColFlesh, ColPushX, ColPushY, ColPushZ, ColCenterX, ColCenterY,
			ColCenterZ, ColArea, ColThickness, ColTarget, ColCap, ColBend, ColForce, ColPlateX, ColPlateY, ColInPiece, ColHandDepth, ColHandSamples,
			NumColumns };
		const TCHAR* const ColumnNames[NumColumns] = {TEXT("row"), TEXT("role"), TEXT("cand"), TEXT("hard_piece"), TEXT("coarse_x"), TEXT("coarse_y"),
			TEXT("flesh_piece"), TEXT("push_x"), TEXT("push_y"), TEXT("push_z"), TEXT("center_x"), TEXT("center_y"), TEXT("center_z"), TEXT("area_cm2"),
			TEXT("thickness_cm"), TEXT("target_cm"), TEXT("force_cap"), TEXT("bend_cm"), TEXT("force"), TEXT("plate_x_cm"), TEXT("plate_y_cm"),
			TEXT("cell_in_piece"), TEXT("hand_depth_cm"), TEXT("hand_samples")};
		FString RowsCsv;
		for (int32 Column = 0; Column < NumColumns; ++Column)
		{
			RowsCsv += Column == 0 ? TEXT("") : TEXT(",");
			RowsCsv += ColumnNames[Column];
		}
		RowsCsv += TEXT("\n");
		int32 NumRowLines = 0;
		auto EmitRow = [&](const TArray<FString>& Fields)
		{
			RowsCsv += FString::Join(Fields, TEXT(","));
			RowsCsv += TEXT("\n");
			++NumRowLines;
		};
		auto EmitHand = [&](const FHandLine& Line, const TCHAR* Role)
		{
			TArray<FString> Fields;
			Fields.SetNum(NumColumns);
			Fields[ColRow] = FString::FromInt(Line.Row);
			Fields[ColRole] = Role;
			Fields[ColCand] = FString::FromInt(Line.Cand);
			Fields[ColPiece] = FString::Printf(TEXT("%u"), Line.Piece);
			Fields[ColCoarseX] = FString::Printf(TEXT("%u"), Line.Packed & 0xfffu);
			Fields[ColCoarseY] = FString::Printf(TEXT("%u"), Line.Packed >> 12);
			Fields[ColHandDepth] = CsvNum(Line.Depth);
			Fields[ColHandSamples] = CsvNum(Line.Samples);
			EmitRow(Fields);
		};
		// 행렬 줄: 조각 밖 칸(outside), 발자국 줄(강체 받침 support, 받침이 없는 강체 칸 footprint_idle)
		for (uint32 Row = 0; Row < NK; ++Row)
		{
			TArray<FString> Fields;
			Fields.SetNum(NumColumns);
			const FUintVector4& R0 = Cell(Row, 0);
			const FUintVector4& R1 = Cell(Row, 1);
			const FUintVector4& R12 = Cell(Row, 12);
			uint32 Packed = Cell(Row, 2).Z;
			const TCHAR* Role = TEXT("outside");
			const float Area = BitsFloat(R0.W);
			const bool bFoot = Row >= NO && P.FRow.IsValidIndex((int32)(Row - NO));
			if (bFoot)
			{
				const FVector4f& Foot = P.FRow[(int32)(Row - NO)];
				const uint32 Bits = FloatBits(Foot.Z);
				const uint32 Cand = Bits >> 16;
				const uint32 InPiece = Bits & 0xffffu;
				const int32 Kind = Cand < Cands ? CandKind[(int32)Cand] : -1;
				const uint32 Piece = Cand < Cands ? CandPiece(Cand) : MAX_uint32;
				const bool bSupport = Kind > 0 && Foot.W > 0.f && (Kind != 2 || CandHands[(int32)Cand] > 0);
				Role = bSupport ? TEXT("support") : TEXT("footprint_idle");
				if (Table && Table->CellRange.IsValidIndex((int32)Piece) && Table->CellList.IsValidIndex((int32)(Table->CellRange[(int32)Piece].X + InPiece)))
				{
					Packed = Table->CellList[(int32)(Table->CellRange[(int32)Piece].X + InPiece)];
				}
				Fields[ColCand] = FString::Printf(TEXT("%u"), Cand);
				Fields[ColPiece] = Piece != MAX_uint32 ? FString::Printf(TEXT("%u"), Piece) : FString();
				Fields[ColPlateX] = CsvNum(Foot.X);
				Fields[ColPlateY] = CsvNum(Foot.Y);
				Fields[ColInPiece] = FString::Printf(TEXT("%u"), InPiece);
			}
			Fields[ColRow] = FString::Printf(TEXT("%u"), Row);
			Fields[ColRole] = Role;
			Fields[ColCoarseX] = FString::Printf(TEXT("%u"), Packed & 0xffffu);
			Fields[ColCoarseY] = FString::Printf(TEXT("%u"), Packed >> 16);
			// 살이 없는 발자국 칸(넓이 0)은 칸 기록이 모두 0이다 — 살 값은 비운다
			if (!bFoot || Area > 0.f)
			{
				const FVector3f Push = UnpackPushNormal(R12.Y);
				Fields[ColFlesh] = FString::Printf(TEXT("%u"), R12.Z);
				Fields[ColPushX] = CsvNum(Push.X);
				Fields[ColPushY] = CsvNum(Push.Y);
				Fields[ColPushZ] = CsvNum(Push.Z);
				Fields[ColCenterX] = CsvNum(BitsFloat(R0.X));
				Fields[ColCenterY] = CsvNum(BitsFloat(R0.Y));
				Fields[ColCenterZ] = CsvNum(BitsFloat(R0.Z));
				Fields[ColThickness] = CsvNum(BitsFloat(R1.X));
				Fields[ColTarget] = CsvNum(BitsFloat(R1.Z));
				Fields[ColCap] = CsvNum(BitsFloat(R1.W));
				Fields[ColBend] = CsvNum(BitsFloat(R12.W));
			}
			Fields[ColArea] = CsvNum(Area);
			Fields[ColForce] = CsvNum(P.Force[(int32)Row]);
			EmitRow(Fields);
		}
		// 손 칸 줄(풀이가 받은 줄, 행렬 줄 다음 차례)
		for (int32 Index : Taken)
		{
			EmitHand(Hands[Index], TEXT("hand"));
		}
		// 풀이에 들지 못한 손 칸 줄(row −1): 깊은 256줄 밖(hand_dropped), 후보가 아닌 조각 위(hand_no_candidate — 넘침 비트 2·4)
		int32 NumHandsDropped = 0;
		int32 NumHandsOffCandidate = 0;
		for (const FHandLine& Line : Hands)
		{
			if (!Line.bTaken)
			{
				EmitHand(Line, Line.Cand >= 0 ? TEXT("hand_dropped") : TEXT("hand_no_candidate"));
				NumHandsDropped += Line.Cand >= 0 ? 1 : 0;
				NumHandsOffCandidate += Line.Cand >= 0 ? 0 : 1;
			}
		}

		// 판 CSV(조각 순열만): 후보 조각마다 한 줄, 그 뒤 손이 닿았는데 풀이에 들지 못한 조각(in_solve 0)
		FString PlatesCsv = TEXT("hard_piece,cand,kind,touched,in_solve,support_rank,foot_first_row,foot_count,hand_rows,hand_rows_dropped,")
			TEXT("sink_cm,tilt_u,tilt_v,on,max_abs_load,max_abs_load_row,sum_load\n");
		int32 NumPlateLines = 0;
		int32 NumPiecesDropped = 0;
		auto IsTouched = [&](uint32 Piece) { return Piece < 4096u && ((P.HardWork[WorkBits + (Piece >> 5)] >> (Piece & 31u)) & 1u) != 0u; };
		auto MotionOf = [&](uint32 Piece) { return P.PieceMotion.IsValidIndex((int32)Piece) ? P.PieceMotion[(int32)Piece] : FVector4f(0.f, 0.f, 0.f, 0.f); };
		for (uint32 Cand = 0; Cand < Cands; ++Cand)
		{
			const uint32 Piece = CandPiece(Cand);
			const int32 Kind = CandKind[(int32)Cand];
			const uint32 Start = P.HardWork[WorkStart + Cand];
			const uint32 Count = P.HardWork[WorkCount + Cand];
			// 받침 하중: 발자국 줄의 풀린 값 — 가장 큰 크기(처음 만난 NaN·무한은 그대로 남긴다)와 합
			float MaxAbs = 0.f;
			int32 MaxRow = -1;
			double SumLoad = 0.0;
			for (uint32 Foot = 0; Foot < Count; ++Foot)
			{
				const uint32 Row = NO + Start + Foot;
				if (Row >= NK)
				{
					break;
				}
				const float Load = P.Force[(int32)Row];
				SumLoad += Load;
				if (FMath::IsFinite(MaxAbs) && !(FMath::Abs(Load) <= MaxAbs))
				{
					MaxAbs = FMath::Abs(Load);
					MaxRow = (int32)Row;
				}
			}
			// 받침 모양 수(강체만, 셰이더 SB_HardSolve의 강체 후보 준비와 같은 식): 1 한 점(가라앉음만), 2 선(긴 축 기울기만), 3 면
			FString Rank;
			if (Kind > 0 && Count > 0 && P.FRow.IsValidIndex((int32)(Start + Count - 1)))
			{
				float SumA = 0.f;
				FVector2f SumX(0.f, 0.f);
				FVector2f SumU(0.f, 0.f);
				for (uint32 Foot = 0; Foot < Count; ++Foot)
				{
					const FVector4f& FootRow = P.FRow[(int32)(Start + Foot)];
					SumA += FootRow.W;
					SumX += FootRow.W * FVector2f(FootRow.X, FootRow.Y);
					SumU += FVector2f(FootRow.X, FootRow.Y);
				}
				const bool bArea = SumA > 0.f;
				const FVector2f Center = bArea ? SumX / SumA : SumU / FMath::Max((float)Count, 1.f);
				float Cxx = 0.f;
				float Cxy = 0.f;
				float Cyy = 0.f;
				for (uint32 Foot = 0; Foot < Count; ++Foot)
				{
					const FVector4f& FootRow = P.FRow[(int32)(Start + Foot)];
					const float Weight = bArea ? FootRow.W / SumA : 1.f / FMath::Max((float)Count, 1.f);
					const FVector2f D = FVector2f(FootRow.X, FootRow.Y) - Center;
					Cxx += Weight * D.X * D.X;
					Cxy += Weight * D.X * D.Y;
					Cyy += Weight * D.Y * D.Y;
				}
				const float Disc = FMath::Sqrt(FMath::Max(0.25f * (Cxx - Cyy) * (Cxx - Cyy) + Cxy * Cxy, 0.f));
				const float L1 = 0.5f * (Cxx + Cyy) + Disc;
				const float L2 = 0.5f * (Cxx + Cyy) - Disc;
				Rank = FString::FromInt(L1 <= 1e-6f ? 1 : (L2 < 0.01f * L1 ? 2 : 3));
			}
			int32 HandsDropped = 0;
			for (const FHandLine& Line : Hands)
			{
				HandsDropped += !Line.bTaken && Line.Cand == (int32)Cand ? 1 : 0;
			}
			const FVector4f Motion = MotionOf(Piece);
			PlatesCsv += FString::Printf(TEXT("%u,%u,%s,%d,1,%s,%u,%u,%d,%d,%s,%s,%s,%s,%s,%d,%s\n"), Piece, Cand, KindName(Kind), IsTouched(Piece) ? 1 : 0,
				*Rank, NO + Start, Count, CandHands[(int32)Cand], HandsDropped,
				*CsvNum(Motion.X), *CsvNum(Motion.Y), *CsvNum(Motion.Z), *CsvNum(Motion.W), *CsvNum(MaxAbs), MaxRow, *CsvNum((float)SumLoad));
			++NumPlateLines;
		}
		if (bHard)
		{
			for (uint32 Word = 0; Word < 128u; ++Word)
			{
				uint32 Bits = P.HardWork[WorkBits + Word];
				while (Bits != 0u)
				{
					const uint32 Piece = Word * 32u + FMath::CountTrailingZeros(Bits);
					Bits &= Bits - 1u;
					bool bCandidate = false;
					for (uint32 Cand = 0; Cand < Cands; ++Cand)
					{
						bCandidate |= CandPiece(Cand) == Piece;
					}
					if (bCandidate)
					{
						continue;
					}
					int32 HandsOn = 0;
					for (const FHandLine& Line : Hands)
					{
						HandsOn += Line.Piece == Piece ? 1 : 0;
					}
					const FVector4f Motion = MotionOf(Piece);
					PlatesCsv += FString::Printf(TEXT("%u,-1,%s,1,0,,,,0,%d,%s,%s,%s,%s,,-1,\n"), Piece, KindName(KindOf(Piece)), HandsOn,
						*CsvNum(Motion.X), *CsvNum(Motion.Y), *CsvNum(Motion.Z), *CsvNum(Motion.W));
					++NumPlateLines;
					++NumPiecesDropped;
				}
			}
		}

		bool bSaved = FFileHelper::SaveStringToFile(RowsCsv, *(Folder / FString::Printf(TEXT("%s_PressureRows.csv"), *Prefix)),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (bHard)
		{
			bSaved &= FFileHelper::SaveStringToFile(PlatesCsv, *(Folder / FString::Printf(TEXT("%s_Plates.csv"), *Prefix)),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		// capture.txt: 고른 단계와 줄 수, 수렴 기록(풀이가 돌았을 때만 — 칸이 없으면 기록 버퍼는 쓰이지 않는다), 하드 서피스 요약, 파일
		OutReport += FString::Printf(TEXT("  contact pressure solve: level %u (rows of 1x1, 2x2, 4x4 coarse cells for 0, 1, 2), %u matrix rows (%u outside plates, %u plate footprint rows), ")
			TEXT("held coarse cells per level %u / %u / %u, %u level-0 cells over the cap of %u\n"),
			Level, NK, NO, NK - NO, P.Header[0], P.Header[9], P.Header[10], P.Header[8], PressureCap);
		OutReport += NK > 0u && P.Stats.Num() >= 2
			? FString::Printf(TEXT("  contact pressure convergence: %u active-set rounds, %u CG steps\n"), P.Stats[0], P.Stats[1])
			: FString(TEXT("  contact pressure convergence: no solve (no held cell)\n"));
		if (bHard)
		{
			OutReport += FString::Printf(TEXT("  hard surface solve: %u candidate pieces (%u touched, at most %u), %u footprint rows, ")
				TEXT("hand rows %u emitted (%u listed, %u in the solve, %d dropped below the %u deepest, %d on pieces outside the solve), ")
				TEXT("overflow bits %u (1 hand rows over %u, 2 over %u candidates, 4 a touched piece did not fit), %d touched pieces not in the solve\n"),
				Cands, P.HardWork[WorkTouched], HardCandidateCap, P.HardWork[WorkFRows], P.HardWork[WorkHands], (uint32)Hands.Num(), Valid,
				NumHandsDropped, HardHandSolveCap, NumHandsOffCandidate, P.HardWork[WorkOverflow], HardHandEmitCap, HardCandidateCap, NumPiecesDropped);
		}
		OutReport += bHard
			? FString::Printf(TEXT("  pressure CSVs: %s_PressureRows.csv (%d rows), %s_Plates.csv (%d pieces)\n"), *Prefix, NumRowLines, *Prefix, NumPlateLines)
			: FString::Printf(TEXT("  pressure CSVs: %s_PressureRows.csv (%d rows)\n"), *Prefix, NumRowLines);
		return bSaved;
	}
}

bool USoftbodySurfaceField::CaptureToFolder(const FString& Folder, const FString& Prefix, bool bFullAtlas, FString& OutReport) const
{
	struct FTarget
	{
		const TCHAR* Name;
		UTextureRenderTarget2D* Target;
	};
	// 저장하는 텍스처는 모두 요청 뒤 실제로 돈 첫 갱신 끝에 뜬 사본이다(차례 SoftbodyCapture::ECopy, RenderBatch의 노멀 패스 뒤). 저장할 때 장을 읽으면
	// 사본을 뜬 뒤 돈 갱신의 값이 섞인다 — 접촉은 눌렸는데 함몰·변위는 0인 폴더가 나왔다. 사본이 있으면 그 뒤 장이 잠들었거나 텍스처를 돌려줬어도
	// 저장한다.
	const bool bCopied = bCaptureCopied && CaptureCopies.Num() == SoftbodyCapture::NumCopies;
	if (!bCopied)
	{
		// 사본이 없는 까닭을 장의 상태에서 가려 적는다. 전에는 텍스처가 없으면 늘 "복원이 끝났다"고 적어, 파이프라인(PSO)을 기다리느라 한 번도 돌지 못한
		// 장도 그렇게 보였다. 파이프라인 상태는 읽기만 한다(ReadPipelineStateForCapture — 미리 만들기를 시작하거나 밀지 않는다).
		// 어느 줄에나 "nothing to save" 또는 "No contact copy"가 든다(바깥 측정 도구가 이 말로 실패를 알아본다).
		bool bContactPipelines = false;
		bool bHardPipelines = false;
		ReadPipelineStateForCapture(bContactPipelines, bHardPipelines);
		const TCHAR* PipelineNote = !bContactPipelines
			? TEXT(" Contact compute pipelines are still compiling: no contact update runs until the log says 'Contact compute pipelines ready'.")
			: UsesHardSurface() && !bHardPipelines
			? TEXT(" This mesh has Flex plates and their compute pipelines are still compiling: it does not update until the log says 'Hard surface compute pipelines ready'.")
			: TEXT("");
		const TCHAR* ScreenNote = TEXT(" Colliders are gathered only while the mesh is on screen and at least Surface Min Screen Size.");
		if (bAsleep)
		{
			OutReport += FString::Printf(TEXT("[%s] Contact is switched off or the component is inactive (its contact field is asleep) - nothing to save\n"), *Prefix);
		}
		else if (!IsReady())
		{
			OutReport += FString::Printf(TEXT("[%s] Contact is not set up yet: the contact map is still loading - nothing to save, run Softbody.CaptureField again\n"), *Prefix);
		}
		else if (!HasTextures() && bHadCandidates)
		{
			OutReport += FString::Printf(TEXT("[%s] A collider is near but the contact textures are not set up yet (the texture pool makes them over a few frames) - nothing to save, run Softbody.CaptureField again.%s\n"),
				*Prefix, PipelineNote);
		}
		else if (!HasTextures() && LastUpdateTime < 0.0)
		{
			OutReport += FString::Printf(TEXT("[%s] No update has run since contact started and no collider is near now - nothing to save.%s%s\n"),
				*Prefix, ScreenNote, PipelineNote);
		}
		else if (!HasTextures())
		{
			OutReport += FString::Printf(TEXT("[%s] Recovery finished with no collider nearby, so the contact textures were released - nothing to save.%s\n"),
				*Prefix, ScreenNote);
		}
		else
		{
			OutReport += FString::Printf(TEXT("[%s] No contact copy: no update of this field ran while the capture waited (%d frames since its last update).%s - run Softbody.CaptureField again\n"),
				*Prefix, FramesSinceUpdate, FCString::Strlen(PipelineNote) > 0 ? PipelineNote
				: TEXT(" Its updates were deferred (update rate limit or budget, see Softbody.Stats), or the component did not tick."));
		}
		return false;
	}
	// 접촉 노멀·끌림 입력·덩어리 깊이와 이완 반복 B·C·D는 갱신 안에서만 사는 임시 텍스처라(ADR-0022) 접촉만 사본이 있고 나머지는 없다(끌림은 거친 격자
	// DragState에 있다, 일지 161).
	const FTarget Targets[] = {
		{TEXT("StateA"), CaptureCopies[SoftbodyCapture::StateA]}, {TEXT("Contact"), CaptureCopies[SoftbodyCapture::Contact]},
		{TEXT("Position"), CaptureCopies[SoftbodyCapture::Position]}, {TEXT("Displacement"), CaptureCopies[SoftbodyCapture::Displacement]},
		{TEXT("Normal"), CaptureCopies[SoftbodyCapture::Normal]}, {TEXT("Accepted"), CaptureCopies[SoftbodyCapture::Accepted]},
		{TEXT("Hold"), CaptureCopies[SoftbodyCapture::Hold]}, {TEXT("Labels"), CaptureCopies[SoftbodyCapture::Labels]}};
	constexpr int32 NumTargets = UE_ARRAY_COUNT(Targets);
	static_assert(NumTargets == SoftbodyCapture::Drag, "Targets lists the contact map sized copies in SoftbodyCapture::ECopy order");
	TArray<FImage> Images;
	Images.SetNum(NumTargets);
	for (int32 Index = 0; Index < NumTargets; ++Index)
	{
		if (!ReadTarget(Targets[Index].Target, Images[Index]) || Images[Index].SizeX != Images[0].SizeX
			|| Images[Index].SizeY != Images[0].SizeY)
		{
			OutReport += FString::Printf(TEXT("[%s] Could not read %s\n"), *Prefix, Targets[Index].Name);
			return false;
		}
	}
	const int32 Size = Images[0].SizeX;
	TArrayView64<const FLinearColor> StateTexels = Images[SoftbodyCapture::StateA].AsRGBA32F();
	TArrayView64<const FLinearColor> PositionTexels = Images[SoftbodyCapture::Position].AsRGBA32F();
	TArrayView64<const FLinearColor> DisplacementTexels = Images[SoftbodyCapture::Displacement].AsRGBA32F();
	TArrayView64<const FLinearColor> NormalTexels = Images[SoftbodyCapture::Normal].AsRGBA32F();
	// 칸이 있는 곳: 이 갱신의 접촉 패스가 피부 자리를 쓴 칸. 위치 w = 1 + 누른 것 번호라 1 이상이고 빈 칸은 0이다(위치 텍스처는 만들 때와 풀에 돌려줄 때
	// 0으로 비우고, 빈 칸에는 아무 패스도 쓰지 않는다). 접촉 사본의 노멀 길이로 가리면 안 된다: 접촉은 갱신 안 임시 텍스처라 빈 칸에 아무 값이나(±65,500
	// 근처) 남아, 그 칸들이 쓰는 칸·눌린 칸·최대 누른 깊이·잘라낼 영역에 들어갔다(시험 캐릭터 눌린 칸 245,373·64,448 cm 대 실제 3,741·2.6 cm).
	// 오프라인 측정 도구(Tools/bench/measure/tm_field_metrics.py)와 같은 규칙이다. 차트 경계의 고스트 칸(옆 차트 값을 복사해 둔 칸)도 든다.
	auto IsCarried = [&](int64 Index)
	{
		const FLinearColor& P = PositionTexels[Index];
		return P.A >= 0.5f && FMath::IsFinite(P.R) && FMath::IsFinite(P.G) && FMath::IsFinite(P.B);
	};
	// 저장하는 접촉 EXR도 빈 칸을 0으로 비운다 — 뜻 없는 값이라, 읽는 도구가 같은 실수를 하지 않게.
	{
		TArrayView64<FLinearColor> ContactWrite = Images[SoftbodyCapture::Contact].AsRGBA32F();
		for (int64 Index = 0; Index < (int64)Size * Size; ++Index)
		{
			if (!IsCarried(Index))
			{
				ContactWrite[Index] = FLinearColor::Transparent;
			}
		}
	}
	TArrayView64<const FLinearColor> ContactTexels = Images[SoftbodyCapture::Contact].AsRGBA32F();
	auto ContactNormal = [&](int64 Index) { const FLinearColor& C = ContactTexels[Index]; return FVector3f(C.R, C.G, C.B); };
	// 누른 깊이(가장자리 다듬기 뒤). 음수는 피부 점에서 콜라이더까지의 거리(솟는 살의 한도).
	auto PressOf = [&](int64 Index) { return ContactTexels[Index].A; };
	auto DisplacementOf = [&](int64 Index) { const FLinearColor& D = DisplacementTexels[Index]; return FVector3f(D.R, D.G, D.B); };
	auto TiltOf = [&](int64 Index) { const FLinearColor& T = NormalTexels[Index]; return FVector3f(T.R, T.G, T.B); };
	// 노멀 방향 표면 높이 변화: 음수 = 꺼짐, 양수 = 솟음 (접선 밀림은 뺀다).
	auto HeightOf = [&](int64 Index) { return FVector3f::DotProduct(DisplacementOf(Index), ContactNormal(Index)); };
	// 끌림(ADR-0008): 움직이는 콜라이더에 끌려간 피부의 거리(cm). 거친 격자(일지 161) 값을 칸이 든 거친 칸에서 그대로 읽는다. 거친 칸 크기는 사본을 뜬
	// 갱신의 값이다.
	FImage DragImage;
	const bool bDrag = CaptureDragBlock > 0 && ReadTarget(CaptureCopies[SoftbodyCapture::Drag], DragImage) && DragImage.SizeX * CaptureDragBlock == Size;
	auto DragOf = [&](int64 Index)
	{
		if (!bDrag)
		{
			return 0.f;
		}
		const FLinearColor& D = DragImage.AsRGBA32F()[((Index / Size) / CaptureDragBlock) * DragImage.SizeX + (Index % Size) / CaptureDragBlock];
		return FVector3f(D.R, D.G, D.B).Size();
	};
	// 접촉 압력(ADR-0037): 거친 격자의 긴 몫 L(R, cm — 살층 응답이 둘레로 올리는 몫)과 거친 압력 p(G). 사본을 뜬 갱신이 Basic이었거나 거리표가 없으면 없다.
	FImage SpreadImage;
	const bool bSpread = CaptureSpreadLandmarks > 0 && CaptureSpreadBlock > 0 && ReadTarget(CaptureCopies[SoftbodyCapture::Long], SpreadImage)
		&& SpreadImage.SizeX * CaptureSpreadBlock == Size;
	auto SpreadOf = [&](int64 Index)
	{
		const int64 X = (Index % Size) / CaptureSpreadBlock;
		const int64 Y = (Index / Size) / CaptureSpreadBlock;
		return SpreadImage.AsRGBA32F()[Y * SpreadImage.SizeX + X].R;
	};

	int64 NumCarried = 0;
	int64 NumPressed = 0;
	float MaxDepth = 0.f;
	float MaxU = 0.f;
	float MinHeight = 0.f;
	float MaxHeight = 0.f;
	float MaxTilt = 0.f;
	float MaxOcclusion = 0.f;
	float MaxDrag = 0.f;
	// 긴 몫 L의 최대도 칸이 있는 거친 칸에서만 센다(그림 8_long에 칠하는 칸과 같다).
	float MaxSpread = 0.f;
	for (int64 Index = 0; Index < (int64)Size * Size; ++Index)
	{
		if (!IsCarried(Index))
		{
			continue;
		}
		++NumCarried;
		MaxDrag = FMath::Max(MaxDrag, DragOf(Index));
		if (bSpread)
		{
			MaxSpread = FMath::Max(MaxSpread, SpreadOf(Index));
		}
		const float Depth = PressOf(Index);
		const float Height = HeightOf(Index);
		NumPressed += Depth > 0.f ? 1 : 0;
		MaxDepth = FMath::Max(MaxDepth, Depth);
		MaxU = FMath::Max(MaxU, StateTexels[Index].R);
		MinHeight = FMath::Min(MinHeight, Height);
		MaxHeight = FMath::Max(MaxHeight, Height);
		MaxTilt = FMath::Max(MaxTilt, TiltOf(Index).Size());
		MaxOcclusion = FMath::Max(MaxOcclusion, NormalTexels[Index].A);
	}
	const float HeightScale = FMath::Max3(-MinHeight, MaxHeight, 1e-3f);
	FIntPoint ActiveMin(Size, Size);
	FIntPoint ActiveMax(-1, -1);
	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			const int64 Index = (int64)Y * Size + X;
			if (IsCarried(Index) && (PressOf(Index) > 0.f || FMath::Abs(HeightOf(Index)) > ActiveHeightRatio * HeightScale))
			{
				ActiveMin = FIntPoint(FMath::Min(ActiveMin.X, X), FMath::Min(ActiveMin.Y, Y));
				ActiveMax = FIntPoint(FMath::Max(ActiveMax.X, X), FMath::Max(ActiveMax.Y, Y));
			}
		}
	}
	const bool bActive = ActiveMax.X >= 0;
	const FIntRect Crop = bActive
		? FIntRect(FMath::Max(ActiveMin.X - CropMargin, 0), FMath::Max(ActiveMin.Y - CropMargin, 0),
			FMath::Min(ActiveMax.X + CropMargin + 1, Size), FMath::Min(ActiveMax.Y + CropMargin + 1, Size))
		: FIntRect(0, 0, Size, Size);
	const int32 Scale = FMath::Clamp(PreviewSize / FMath::Max(Crop.Width(), Crop.Height()), 1, MaxPreviewScale);

	IFileManager::Get().MakeDirectory(*Folder, true);
	bool bSaved = true;
	for (int32 Index = 0; Index < NumTargets; ++Index)
	{
		const FString Path = Folder / FString::Printf(TEXT("%s_%s.exr"), *Prefix, Targets[Index].Name);
		bSaved &= bFullAtlas ? FImageUtils::SaveImageByExtension(*Path, Images[Index])
			: FImageUtils::SaveImageByExtension(*Path, CropImage(Images[Index], Crop));
	}
	if (bDrag)
	{
		// 끌림 거친 격자 그대로(칸 하나 = 캐리어 칸 DragBlock × DragBlock, RGB 끌림 cm, A = 눌렸으면 1 + 덮인 몫, 아니면 0 — ADR-0033).
		const FString Path = Folder / FString::Printf(TEXT("%s_Drag.exr"), *Prefix);
		const FIntRect DragCrop(Crop.Min / CaptureDragBlock, (Crop.Max + FIntPoint(CaptureDragBlock - 1, CaptureDragBlock - 1)) / CaptureDragBlock);
		bSaved &= bFullAtlas ? FImageUtils::SaveImageByExtension(*Path, DragImage)
			: FImageUtils::SaveImageByExtension(*Path, CropImage(DragImage, DragCrop));
	}
	if (bSpread)
	{
		// 거친 격자 그대로(칸 하나 = 캐리어 칸 SpreadBlock × SpreadBlock). 잘라낼 때는 잘라낸 영역을 덮는 거친 칸들.
		const FString Path = Folder / FString::Printf(TEXT("%s_Long.exr"), *Prefix);
		const FIntRect CoarseCrop(Crop.Min / CaptureSpreadBlock, (Crop.Max + FIntPoint(CaptureSpreadBlock - 1, CaptureSpreadBlock - 1)) / CaptureSpreadBlock);
		bSaved &= bFullAtlas ? FImageUtils::SaveImageByExtension(*Path, SpreadImage)
			: FImageUtils::SaveImageByExtension(*Path, CropImage(SpreadImage, CoarseCrop));
	}

	// 연속 굽힘: 거친 격자의 굽힘 거친 해 W_c(R, cm — 굽는 칸이 없으면 0). 긴 몫과 같은 거친 칸·같은 자르기.
	FImage BendImage;
	if (bSpread && ReadTarget(CaptureCopies[SoftbodyCapture::Bend], BendImage) && BendImage.SizeX == SpreadImage.SizeX)
	{
		const FString Path = Folder / FString::Printf(TEXT("%s_Bend.exr"), *Prefix);
		const FIntRect CoarseCrop(Crop.Min / CaptureSpreadBlock, (Crop.Max + FIntPoint(CaptureSpreadBlock - 1, CaptureSpreadBlock - 1)) / CaptureSpreadBlock);
		bSaved &= bFullAtlas ? FImageUtils::SaveImageByExtension(*Path, BendImage)
			: FImageUtils::SaveImageByExtension(*Path, CropImage(BendImage, CoarseCrop));
	}

	const float DepthScale = FMath::Max3(MaxDepth, MaxU, 1e-3f);
	auto Preview = [&](const TCHAR* Name, TFunctionRef<FColor(int64 Index)> Paint)
	{
		const FString Path = Folder / FString::Printf(TEXT("%s_%s.png"), *Prefix, Name);
		bSaved &= SavePreview(Path, Crop, Scale, [&](int64 Index) { return IsCarried(Index) ? Paint(Index) : EmptyColor; }, Size);
	};
	// 누른 깊이(접촉 패스가 콜라이더·손 거리장에서 바로 구한 값, 가장자리 다듬기 뒤): 주황 = 누름, 파랑 = 아직 닿지 않음(가까울수록 밝다).
	Preview(TEXT("1_depth"), [&](int64 Index)
	{
		const float Depth = PressOf(Index);
		return Depth > 0.f ? Signed(Depth, DepthScale) : Signed(-FMath::Max(DepthScale + Depth, 0.f), DepthScale);
	});
	// 함몰 u(누른 깊이를 막처럼 퍼뜨린 뒤): 흰색일수록 깊다.
	Preview(TEXT("2_u"), [&](int64 Index) { return Gray(StateTexels[Index].R / DepthScale); });
	// 최종 표면 높이 변화(노멀 방향): 파랑 = 꺼짐, 주황 = 솟음.
	Preview(TEXT("3_height"), [&](int64 Index) { return Signed(HeightOf(Index), HeightScale); });
	// 노멀 기울기 크기(sin): 흰색일수록 많이 기운다. 가장 큰 값이 흰색.
	Preview(TEXT("4_tilt"), [&](int64 Index) { return Gray(TiltOf(Index).Size() / FMath::Max(MaxTilt, 1e-4f)); });
	// 함몰 어둡기: 흰색일수록 어둡게 칠해진다. 가장 큰 값이 흰색.
	Preview(TEXT("5_occlusion"), [&](int64 Index) { return Gray(NormalTexels[Index].A / FMath::Max(MaxOcclusion, 1e-4f)); });
	// 조명: 칸마다 원래 노멀에서 60° 기운 빛(기우는 쪽은 프레임 +Z를 접평면에 투영한 방향)을 비춘다. 몸의 원래 곡면은
	// 모두 같은 회색이 되고, 표면 장이 기울인 만큼만 밝아지거나 어두워진다. 머티리얼처럼 함몰 어둡기를 곱한다.
	Preview(TEXT("6_lit"), [&](int64 Index)
	{
		const FVector3f Base = ContactNormal(Index).GetSafeNormal();
		FVector3f Along = FVector3f::UnitZ() - Base * Base.Z;
		if (Along.SizeSquared() < 1e-4f)
		{
			Along = FVector3f::UnitX() - Base * Base.X;
		}
		const FVector3f Light = 0.5f * Base + 0.866f * Along.GetSafeNormal();
		const FVector3f Shaded = RotateByTilt(TiltOf(Index), Base).GetSafeNormal();
		const float Diffuse = FMath::Max(FVector3f::DotProduct(Shaded, Light), 0.f) * (1.f - NormalTexels[Index].A);
		return Gray(0.08f + 0.92f * Diffuse);
	});
	// 끌린 거리: 흰색일수록 많이 끌렸다. 가장 큰 값이 흰색.
	Preview(TEXT("7_drag"), [&](int64 Index) { return Gray(DragOf(Index) / FMath::Max(MaxDrag, 1e-4f)); });
	if (bSpread)
	{
		// 긴 몫 L(거친 칸 값 그대로, 흰색일수록 많이 올린다). 함몰 u 그림(2_u)과 같은 밝기 기준이다.
		Preview(TEXT("8_long"), [&](int64 Index) { return Gray(SpreadOf(Index) / DepthScale); });
	}

	OutReport += FString::Printf(
		TEXT("[%s]\n")
		TEXT("  data %s, resolution %d, cell %.3f (mesh units)\n")
		TEXT("  copied at the end of update #%llu (frame %llu): every EXR and PNG and the settings line of this component are from that one update; view.png is from the save frame\n")
		TEXT("  cells in use %lld, pressed cells %lld (cells the contact pass wrote, Position w >= 0.5, chart border copies included)\n")
		TEXT("  max press depth %.4f cm, max dent u %.4f cm, surface height change %.4f to +%.4f cm (+ = bulge)\n")
		TEXT("  max normal tilt (sin) %.4f, max dent darkening %.4f, max drag distance %.4f cm\n")
		TEXT("  cropped area (%d, %d) to (%d, %d), one cell = %d pixels in the images, EXR = %s\n"),
		*Prefix, *GetPathNameSafe(Data), Size, Data->CarrierTexelSize, CaptureSerial, CaptureFrame, NumCarried, NumPressed,
		MaxDepth, MaxU, MinHeight, MaxHeight, MaxTilt, MaxOcclusion, MaxDrag,
		Crop.Min.X, Crop.Min.Y, Crop.Max.X, Crop.Max.Y, Scale, bFullAtlas ? TEXT("whole atlas") : TEXT("cropped area"));
	OutReport += bSpread
		? FString::Printf(TEXT("  contact pressure coarse grid: %dx%d contact map cells per coarse cell, coarse cell %.3f cm, resolution %d, %d surface landmarks, max long part L %.4f cm\n"),
			CaptureSpreadBlock, CaptureSpreadBlock, CaptureSpreadTexelSize, SpreadImage.SizeX, CaptureSpreadLandmarks, MaxSpread)
		: FString(TEXT("  contact pressure off (Contact Detail Basic, or the contact map has no bulge distance table)\n"));
	// 살 조각 번호(_PressureRows.csv의 flesh_piece 열)가 어디서 왔나: 판 6 거리표는 굽기, 옛 표는 실행 중 랜드마크 유도
	if (bSpread)
	{
		if (Data->BulgeTableVersion >= SoftbodyBulgeTable::PieceVersion)
		{
			OutReport += TEXT("  flesh piece ids: baked per distance-table row (table 6) - the pressure matrix and the long-part gather both split pieces by them\n");
		}
		else
		{
			OutReport += FString::Printf(TEXT("  flesh piece ids: the first landmark each row reaches (table %d) - small pieces without landmarks share 65535; Rebake Contact Map for baked ids\n"),
				Data->BulgeTableVersion);
		}
	}
	OutReport += FString::Printf(TEXT("  drag coarse grid (= contact pressure grid): %dx%d contact map cells per coarse cell (%.3f mesh units), resolution %d\n"),
		CaptureDragBlock, CaptureDragBlock, CaptureDragBlock * Data->CarrierTexelSize, bDrag ? DragImage.SizeX : 0);
	// 접촉 압력 풀이(ADR-0037·ADR-0038 §4): 사본을 뜬 갱신의 풀이 줄·판 상태·수렴 기록. 렌더 스레드와 GPU를 기다려 읽는다(저장할 때만).
	FSoftbodyPressureCaptureData Pressure;
	ReadPressureCapture(Pressure);
	bSaved &= SavePressureCapture(Folder, Prefix, Pressure, OutReport);
	// 콜라이더(박힘 분석, 일지 340): 사본을 뜬 갱신이 본 형상·거리장 부위 행(프레임 좌표 — 셰이더 Shapes·Planes·FieldInstances와 같은 값, 사본을 뜰 때
	// 붙잡음)과 거리장 칸 값(반정밀 둘씩, FieldInstances 4행 w가 첫 칸 번호 — 칸 값은 덧붙이기만 하므로 저장할 때의 것을 쓴다).
	{
		auto Rows = [](const TArray<FVector4f>& A)
		{
			FString S = TEXT("[");
			for (int32 Row = 0; Row < A.Num(); ++Row)
			{
				S += FString::Printf(TEXT("%s[%.9g, %.9g, %.9g, %.9g]"), Row ? TEXT(", ") : TEXT(""), A[Row].X, A[Row].Y, A[Row].Z, A[Row].W);
			}
			return S + TEXT("]");
		};
		const bool bVoxelsKept = FieldVoxelWords.Num() >= CaptureVoxelWords;
		const FString Json = FString::Printf(TEXT("{\n\"copied_update\": %llu,\n\"voxels_kept\": %s,\n\"shape_rows\": 3,\n")
			TEXT("\"field_rows\": %d,\n\"shapes\": %s,\n\"planes\": %s,\n\"field_instances\": %s,\n\"voxel_words\": %d\n}\n"),
			(unsigned long long)CaptureSerial, bVoxelsKept ? TEXT("true") : TEXT("false"), SoftbodySurfaceFieldRows::PerInstance,
			*Rows(CaptureShapes), *Rows(CapturePlanes), *Rows(CaptureFieldInstances), FieldVoxelWords.Num());
		bSaved &= FFileHelper::SaveStringToFile(Json, *(Folder / FString::Printf(TEXT("%s_Colliders.json"), *Prefix)),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TArray<uint8> VoxelBytes;
		VoxelBytes.Append(reinterpret_cast<const uint8*>(FieldVoxelWords.GetData()), FieldVoxelWords.Num() * (int32)sizeof(uint32));
		bSaved &= FFileHelper::SaveArrayToFile(VoxelBytes, *(Folder / FString::Printf(TEXT("%s_FieldVoxels.bin"), *Prefix)));
		OutReport += FString::Printf(TEXT("  colliders of the copied update (_Colliders.json, _FieldVoxels.bin): %d shapes, %d shape field parts, %d voxel words%s\n"),
			CaptureShapes.Num() / 3, CaptureFieldInstances.Num() / SoftbodySurfaceFieldRows::PerInstance, FieldVoxelWords.Num(),
			bVoxelsKept ? TEXT("") : TEXT(" - the voxel list was rebuilt after the copy, voxel indices may not match"));
	}
	if (!bSaved)
	{
		OutReport += TEXT("  some files could not be saved\n");
	}
	return bSaved;
}

FString USoftbodySurfaceField::CaptureSettingsLine(const UObject* Owner)
{
	// 조절값(PIE에서 바꾼 값 포함) — 그림만으로는 어떤 값에서 찍었는지 알 수 없다. Basic이면 Edge Smoothing·Drag Friction은
	// 패널 값이고 실제로는 0으로 돈다(일지 222·224). Volume Preservation = 구운 데이터의 저작 에셋 값(칠이 곱하고 Volume Offset이 더한다),
	// Skin Tension = 컴포넌트 값(§3.4, 길이는 고정 L0). 두께 중앙값은 기록용으로 그대로 적는다(Tools/bench/invariance가 읽는다).
	const USoftbodyComponent* Component = Cast<USoftbodyComponent>(Owner);
	if (!Component)
	{
		return TEXT("  settings: unknown (the owner is not a Softbody component)\n");
	}
	const USoftbodyMeshData* CapturedData = Component->MeshData;
	const float CapturedSkin = FMath::Clamp(Component->SkinTension, 0.f, 2.f);
	return FString::Printf(
		TEXT("  settings: Contact Detail %s, Max Contact Depth %.2f, Edge Smoothing %.2f cells, ")
		TEXT("Recovery Rate %.2f, Drag Friction %.2f, Volume Preservation %.2f (authoring asset; paint multiplies it), Volume Offset %+.2f (adds to it), ")
		TEXT("Flex Offset %+.2f (adds to the Flex paint), Skin Tension %.2f (skin length = %.2f local units from the fixed length %.2f; ")
		TEXT("median thickness %.2f)\n"),
		Component->ContactDetail == ESoftbodyContactDetail::Full ? TEXT("Full") : TEXT("Basic (Edge Smoothing, Drag Friction run as 0)"),
		Component->MaxContactDepth, Component->SurfaceEdgeTrim,
		Component->SurfaceRecoveryRate, Component->SurfaceDragRatio, CapturedData ? CapturedData->VolumePreservation : 1.f,
		Component->VolumeOffset, Component->FlexOffset, CapturedSkin,
		4.f * FMath::Square(CapturedSkin) * SoftbodyContactResponse::ReferenceLength, SoftbodyContactResponse::ReferenceLength,
		CapturedData ? CapturedData->CarrierThicknessMedian : 0.f);
}

FString USoftbodySurfaceField::GetCaptureSettings() const
{
	// 사본이 있으면 사본을 뜬 갱신 때 적어 둔 값(OnScheduled), 없으면 지금 값. 저장은 보통 다음 틱이라 그 사이 바뀐 값이 섞이지 않게 한다.
	return bCaptureCopied && !CaptureSettings.IsEmpty() ? CaptureSettings : CaptureSettingsLine(GetOuter());
}

namespace
{
	/** 사본을 기다리는 틱 수 상한: 쉬거나 갱신이 계속 미뤄지는 컴포넌트는 이만큼 기다린 뒤 사본 없이 적는다. */
	constexpr int32 CaptureWaitTicks = 30;

	/**
	 * 사본이 복사되면(보통 다음 틱) 저장하고 사본을 지운다. 콘솔 명령이 월드 틱보다 먼저 처리되면 "다음 틱" 타이머가 같은 프레임,
	 * 묶음이 이번 프레임 신청을 돌리기 전에 불린다. 그 신청은 사본 타깃의 렌더 자원을 들고 있어서, 이때 지우면 렌더 스레드가 지운
	 * 자원에 복사하다 멈췄다(AddSurfaceNormalPasses). 신청에 사본이 실려 있는 동안은 지우지 않고 다음 틱으로 미룬다.
	 * Notes는 capture.txt 머리 줄 끝에 붙인다(명령이 고른 방식·고른 것·건너뛴 것). Folder가 비었으면 Saved/SoftbodyCapture/<시각_프레임>에 둔다
	 * (구간 기록 Softbody.Record의 표시는 기록 폴더 안에 둔다).
	 */
	void SaveCapturesWhenCopied(TWeakObjectPtr<UWorld> World, TArray<TWeakObjectPtr<USoftbodyComponent>> Requested, bool bFullAtlas,
		int32 TicksLeft, FString Notes, FString Folder)
	{
		if (!World.IsValid())
		{
			return;
		}
		bool bWait = false;
		for (const TWeakObjectPtr<USoftbodyComponent>& Weak : Requested)
		{
			const USoftbodyComponent* Component = Weak.Get();
			const USoftbodySurfaceField* Field = Component ? Component->GetSurfaceField() : nullptr;
			// 텍스처도 없고 닿는 것도 없는 장(안 닿았거나 복원이 끝나 텍스처를 돌려준 장)과 잠든 장은 돌 일이 없어 기다리지 않는다. 전에는 이런 장 하나가 같이
			// 고른 장들의 저장을 기다림 상한(30틱)까지 늦췄다(요청 2,995 → 저장 3,030프레임 — 화면과 보고가 그만큼 늦은 순간의 것이 됐다).
			const bool bMayRun = Field && !Field->IsAsleep() && (Field->HasTextures() || Field->HasCandidates());
			if (Field && (Field->IsCaptureSent() || (TicksLeft > 0 && bMayRun && Field->IsCapturePending())))
			{
				bWait = true;
			}
		}
		if (bWait)
		{
			World->GetTimerManager().SetTimerForNextTick([World, Requested, bFullAtlas, TicksLeft, Notes, Folder]()
			{
				SaveCapturesWhenCopied(World, Requested, bFullAtlas, TicksLeft - 1, Notes, Folder);
			});
			return;
		}
		if (Folder.IsEmpty())
		{
			Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SoftbodyCapture")
				/ FString::Printf(TEXT("%s_f%llu"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), (uint64)GFrameCounter));
		}
		FString Body;
		int32 Saved = 0;
		for (const TWeakObjectPtr<USoftbodyComponent>& Weak : Requested)
		{
			USoftbodyComponent* Component = Weak.Get();
			USoftbodySurfaceField* Field = Component ? Component->GetSurfaceField() : nullptr;
			if (!Field)
			{
				continue;
			}
			const FString OwnerName = Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : TEXT("NoOwner");
			const FString Prefix = FPaths::MakeValidFileName(OwnerName + TEXT("_") + Component->GetName(), TEXT('_'));
			if (Field->CaptureToFolder(Folder, Prefix, bFullAtlas, Body))
			{
				++Saved;
			}
			// 조절값 줄: 사본을 뜬 갱신 때 적어 둔 값(사본이 없으면 지금 값). 사본을 지우기 전에 읽는다.
			Body += Field->GetCaptureSettings();
			Field->ReleaseCapture();
		}
		// 머리: 저장 프레임, 저장한 수, 명령이 고른 것. 하나도 저장하지 못했으면 경고로 남긴다 — 빈 캡처가 성공처럼 보이지 않게.
		const FString Report = FString::Printf(TEXT("Frame %llu\nsaved %d of %d requested components%s\n"), (uint64)GFrameCounter, Saved,
			Requested.Num(), *Notes) + Body;
		FFileHelper::SaveStringToFile(Report, *(Folder / TEXT("capture.txt")), FFileHelper::EEncodingOptions::ForceUTF8);
		// 저장하는 순간의 화면도 함께 남긴다(다음 프레임 렌더가 끝나면 저장된다 — 사본을 뜬 갱신보다 보통 1~2프레임 뒤).
		FScreenshotRequest::RequestScreenshot(Folder / TEXT("view.png"), false, false);
		if (Saved > 0)
		{
			UE_LOG(LogSoftbodyCapture, Log, TEXT("[Softbody.CaptureField] Saved %d components -> %s\n%s"), Saved, *Folder, *Report);
		}
		else
		{
			UE_LOG(LogSoftbodyCapture, Warning, TEXT("[Softbody.CaptureField] Saved %d components -> %s\n%s"), Saved, *Folder, *Report);
		}
	}

	void CaptureFieldCommand(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		bool bFullAtlas = false;
		FString Filter;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("full"), ESearchCase::IgnoreCase))
			{
				bFullAtlas = true;
			}
			else
			{
				Filter = Arg;
			}
		}

		// PIE 월드의 컴포넌트를 먼저 찾고, 없으면 명령을 받은 월드에서 찾는다. 이름은 액터·컴포넌트 이름이 인자와 꼭 같은 것이 있으면 그것만, 없으면 인자가
		// 든 것 모두다(부분 일치만 쓰면 TM_C가 TM_C_Step 같은 액터까지 골랐다). 고른 것 가운데 지금 찍을 수 없는 것(접촉을 꺼 잠든 장, 접촉 지도를
		// 올리는 중인 장, 앞 캡처가 아직 저장되지 않은 장)은 빼고 그 수를 알린다 — 전에는 조용히 빠져, 다 빠지면 "접촉이 켜진 컴포넌트가 없다"고 틀리게 말했다.
		TArray<USoftbodyComponent*> Components;
		bool bExact = true;
		int32 NumAsleep = 0;
		int32 NumLoading = 0;
		int32 NumBusy = 0;
		for (int32 Pass = 0; Pass < 2 && Components.Num() == 0; ++Pass)
		{
			TArray<USoftbodyComponent*> Exact;
			TArray<USoftbodyComponent*> Partial;
			for (TObjectIterator<USoftbodyComponent> It; It; ++It)
			{
				USoftbodyComponent* Component = *It;
				UWorld* ComponentWorld = IsValid(Component) && !Component->IsTemplate() ? Component->GetWorld() : nullptr;
				// 둘째 차례는 명령 월드가 게임 월드가 아닐 때만 뜻이 있다(게임 월드는 첫 차례가 이미 봤다 — 두 번 세지 않는다).
				if (!ComponentWorld || (Pass == 0 ? !ComponentWorld->IsGameWorld() : (ComponentWorld != World || ComponentWorld->IsGameWorld()))
					|| !Component->GetSurfaceField())
				{
					continue;
				}
				const FString OwnerName = Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString();
				if (Filter.IsEmpty() || OwnerName.Equals(Filter, ESearchCase::IgnoreCase) || Component->GetName().Equals(Filter, ESearchCase::IgnoreCase))
				{
					Exact.Add(Component);
				}
				else if (OwnerName.Contains(Filter) || Component->GetName().Contains(Filter))
				{
					Partial.Add(Component);
				}
			}
			bExact = Exact.Num() > 0 || Partial.Num() == 0;
			for (USoftbodyComponent* Component : bExact ? Exact : Partial)
			{
				const USoftbodySurfaceField* Field = Component->GetSurfaceField();
				if (Field->IsAsleep())
				{
					++NumAsleep;
				}
				else if (!Field->IsReady())
				{
					++NumLoading;
				}
				else if (Field->IsCaptureBusy())
				{
					++NumBusy;
				}
				else
				{
					Components.Add(Component);
				}
			}
		}
		TArray<FString> Skipped;
		if (NumAsleep > 0)
		{
			Skipped.Add(FString::Printf(TEXT("%d asleep (contact switched off or the component inactive)"), NumAsleep));
		}
		if (NumLoading > 0)
		{
			Skipped.Add(FString::Printf(TEXT("%d with the contact map still loading"), NumLoading));
		}
		if (NumBusy > 0)
		{
			Skipped.Add(FString::Printf(TEXT("%d still saving an earlier capture (that request saves them)"), NumBusy));
		}
		const FString SkippedNote = Skipped.Num() > 0 ? TEXT("; skipped ") + FString::Join(Skipped, TEXT(", ")) : FString();
		if (Components.Num() == 0)
		{
			if (Skipped.Num() == 0)
			{
				Ar.Logf(TEXT("[Softbody.CaptureField] No Softbody component with contact turned on was found (name filter: '%s')"), *Filter);
			}
			else
			{
				Ar.Logf(TEXT("[Softbody.CaptureField] No Softbody component could be captured now (name filter: '%s')%s"), *Filter, *SkippedNote);
			}
			return;
		}
		FString Names;
		for (const USoftbodyComponent* Component : Components)
		{
			if (!Names.IsEmpty())
			{
				Names += TEXT(", ");
			}
			Names += Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : FString(TEXT("NoOwner"));
			Names += TEXT(".");
			Names += Component->GetName();
		}
		const FString Picked = Filter.IsEmpty() ? FString(TEXT("no name filter"))
			: FString::Printf(TEXT("name filter '%s' (%s)"), *Filter, bExact ? TEXT("exact name") : TEXT("no exact name, so every name containing it"));
		const FString Notes = FString::Printf(TEXT("; %s: %s%s"), *Picked, *Names, *SkippedNote);

		// 저장할 장들은 이번 프레임(미뤄지면 다음에 실제로 도는) 갱신 끝에 한꺼번에 사본을 떠 두고, 복사된 뒤(보통 다음 틱) 저장한다 — 접촉은 갱신 안에서만
		// 사는 임시 텍스처라(ADR-0022) 그때밖에 없고, 나머지도 같은 갱신에서 떠야 한 폴더가 한 순간이다.
		TArray<TWeakObjectPtr<USoftbodyComponent>> Requested;
		for (USoftbodyComponent* Component : Components)
		{
			Component->GetSurfaceField()->RequestCapture();
			Requested.Add(Component);
		}
		SaveCapturesWhenCopied(Components[0]->GetWorld(), Requested, bFullAtlas, CaptureWaitTicks, Notes, FString());
		Ar.Logf(TEXT("[Softbody.CaptureField] %d components (%s%s): copying the contact textures at the end of the next update that runs, all from that one update; they are saved once copied, usually on the next tick (results in the LogSoftbodyCapture log)"),
			Components.Num(), *Names, *SkippedNote);
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSoftbodyCaptureFieldCommand(
		TEXT("Softbody.CaptureField"),
		TEXT("Debug: copies the contact textures at the end of the next update that runs and saves them to Saved/SoftbodyCapture/<time_frame>/ (usually the next tick; ")
		TEXT("capture.txt names the update). Arguments: [full] [actor or component name - an exact name wins, otherwise every name containing it]. Raw data is EXR (only the pressed area by default, the whole atlas with full); ")
		TEXT("the PNGs enlarge the pressed area for viewing, and view.png is the screen when they are saved."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&CaptureFieldCommand));
}

int32 SoftbodyCapture::CaptureInto(const TArray<USoftbodyComponent*>& Components, bool bFullAtlas, const FString& Folder, const FString& Notes)
{
	// Softbody.CaptureField와 같은 차례: 지금 찍을 수 없는 장은 빼고, 한 갱신 끝에 함께 사본을 뜬다.
	TArray<TWeakObjectPtr<USoftbodyComponent>> Requested;
	UWorld* World = nullptr;
	for (USoftbodyComponent* Component : Components)
	{
		USoftbodySurfaceField* Field = IsValid(Component) ? Component->GetSurfaceField() : nullptr;
		if (!Field || Field->IsAsleep() || !Field->IsReady() || Field->IsCaptureBusy())
		{
			continue;
		}
		Field->RequestCapture();
		Requested.Add(Component);
		World = World ? World : Component->GetWorld();
	}
	if (World)
	{
		SaveCapturesWhenCopied(World, Requested, bFullAtlas, CaptureWaitTicks, Notes, Folder);
	}
	return Requested.Num();
}
