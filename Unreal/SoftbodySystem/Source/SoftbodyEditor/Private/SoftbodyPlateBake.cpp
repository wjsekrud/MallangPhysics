// Copyright (c) 2026 wjsekrud

#include "SoftbodyPlateBake.h"

#include "Misc/FileHelper.h"

THIRD_PARTY_INCLUDES_START
#include <Eigen/Dense>
#include <Eigen/SparseCore>
THIRD_PARTY_INCLUDES_END

namespace SoftbodyPlateBake
{
	namespace PlateBakeLocal
	{
		using FSparse = Eigen::SparseMatrix<double>;
		using FTriplet = Eigen::Triplet<double>;

		/** 값을 지나는 3차(Catmull-Rom) 네 무게, t ∈ [0, 1). 실행 중 가는 창이 거친 마디에서 판을 읽는 무게와 같다. */
		void CatmullRom(double T, double W[4])
		{
			const double T2 = T * T;
			const double T3 = T2 * T;
			W[0] = 0.5 * (-T3 + 2.0 * T2 - T);
			W[1] = 0.5 * (3.0 * T3 - 5.0 * T2 + 2.0);
			W[2] = 0.5 * (-3.0 * T3 + 4.0 * T2 + T);
			W[3] = 0.5 * (T3 - T2);
		}

		/** 가는 칸 → 거친 연속 좌표(마디 = 거친 칸 정수 자리, 칸 가운데). */
		double CoarseCoord(int32 Texel, int32 Block)
		{
			return (Texel + 0.5) / Block - 0.5;
		}
	}

	double BendingLength(double Softness, double HRef)
	{
		// 0.6·L0·√((1 − S)/S) — 셰이더 SB_HardBendRatio와 같은 식(L0 = HRef).
		const double Black = BlackEll * HRef;
		if (Softness <= 0.0)
		{
			return Black;
		}
		return FMath::Min(0.6 * HRef * FMath::Sqrt(FMath::Max(1.0 - Softness, 0.0) / Softness), Black);
	}

	bool NodesInsideGrid(TConstArrayView<FIntPoint> Texels, int32 Block, int32 CoarseResolution)
	{
		using namespace PlateBakeLocal;
		for (const FIntPoint& T : Texels)
		{
			const int32 X0 = FMath::FloorToInt32(CoarseCoord(T.X, Block));
			const int32 Y0 = FMath::FloorToInt32(CoarseCoord(T.Y, Block));
			if (X0 - 1 < 0 || Y0 - 1 < 0 || X0 + 2 >= CoarseResolution || Y0 + 2 >= CoarseResolution)
			{
				return false;
			}
		}
		return true;
	}

	bool BakePiece(const FPieceInput& In, FPieceOutput& Out)
	{
		using namespace PlateBakeLocal;
		Out = FPieceOutput();
		const int32 B = In.Block;
		const double C = In.TexelSize;
		const int32 Rc = In.CoarseResolution;
		const int32 NumFine = In.Texels.Num();
		const int32 NF = In.Cells.Num();
		if (NumFine == 0 || NF == 0 || B <= 0 || C <= 0.0 || Rc <= 0 || In.Plate.Num() != NumFine || In.Softness.Num() != NumFine
			|| In.Chart.Num() != NumFine)
		{
			Out.Error = TEXT("the plate has no cells");
			return false;
		}

		// --- 발자국 칸: 자리 → 조각 안 번호 f(둘레 세 칸까지 담은 상자 — 기울기 창이 7 × 7까지 넓어진다) ---
		FIntPoint CMin(MAX_int32, MAX_int32);
		FIntPoint CMax(MIN_int32, MIN_int32);
		for (const FIntPoint& Cell : In.Cells)
		{
			if (Cell.X < 0 || Cell.Y < 0 || Cell.X >= Rc || Cell.Y >= Rc)
			{
				Out.Error = TEXT("a cell under the plate is outside the coarse grid");
				return false;
			}
			CMin = CMin.ComponentMin(Cell);
			CMax = CMax.ComponentMax(Cell);
		}
		const FIntPoint GMin(FMath::Max(CMin.X - 3, 0), FMath::Max(CMin.Y - 3, 0));
		const FIntPoint GMax(FMath::Min(CMax.X + 3, Rc - 1), FMath::Min(CMax.Y + 3, Rc - 1));
		const int32 GW = GMax.X - GMin.X + 1;
		TArray<int32> Slot;
		Slot.Init(INDEX_NONE, GW * (GMax.Y - GMin.Y + 1));
		const auto SlotAt = [&](int32 X, int32 Y) -> int32
		{
			return X < GMin.X || Y < GMin.Y || X > GMax.X || Y > GMax.Y ? INDEX_NONE : Slot[(Y - GMin.Y) * GW + (X - GMin.X)];
		};
		for (int32 f = 0; f < NF; ++f)
		{
			Slot[(In.Cells[f].Y - GMin.Y) * GW + (In.Cells[f].X - GMin.X)] = f;
		}

		// --- 발자국 칸마다 줄 자리 x_F(조각 가는 칸 판 좌표 평균)와 넓이 A_F ---
		TArray<double> XF, YF, AF;
		XF.Init(0.0, NF);
		YF.Init(0.0, NF);
		AF.Init(0.0, NF);
		for (int32 k = 0; k < NumFine; ++k)
		{
			const int32 f = SlotAt(In.Texels[k].X / B, In.Texels[k].Y / B);
			if (f == INDEX_NONE)
			{
				Out.Error = TEXT("a plate cell is outside the coarse cells under the plate");
				return false;
			}
			XF[f] += In.Plate[k].X;
			YF[f] += In.Plate[k].Y;
			AF[f] += 1.0;
		}
		for (int32 f = 0; f < NF; ++f)
		{
			if (AF[f] <= 0.0)
			{
				Out.Error = TEXT("a coarse cell under the plate has no plate cell");
				return false;
			}
			XF[f] /= AF[f];
			YF[f] /= AF[f];
			AF[f] *= C * C;
		}

		// --- 아틀라스 칸 좌표 → 판 좌표 최소 제곱 아핀. 마디(거친 칸 가운데) 자리를 이것으로 낸다 — 빈 칸·다른 차트 칸도 같은 아핀 ---
		double MeanU = 0.0;
		double MeanV = 0.0;
		for (const FIntPoint& T : In.Texels)
		{
			MeanU += T.X + 0.5;
			MeanV += T.Y + 0.5;
		}
		MeanU /= NumFine;
		MeanV /= NumFine;
		Eigen::MatrixXd Design(NumFine, 3);
		Eigen::MatrixXd Target(NumFine, 2);
		for (int32 k = 0; k < NumFine; ++k)
		{
			Design(k, 0) = In.Texels[k].X + 0.5 - MeanU;
			Design(k, 1) = In.Texels[k].Y + 0.5 - MeanV;
			Design(k, 2) = 1.0;
			Target(k, 0) = In.Plate[k].X;
			Target(k, 1) = In.Plate[k].Y;
		}
		const Eigen::MatrixXd Affine = Design.colPivHouseholderQr().solve(Target);
		const auto PlateAt = [&](double U, double V)
		{
			return FVector2d(Affine(0, 0) * (U - MeanU) + Affine(1, 0) * (V - MeanV) + Affine(2, 0),
				Affine(0, 1) * (U - MeanU) + Affine(1, 1) * (V - MeanV) + Affine(2, 1));
		};

		// --- 1. 국소 판 기울기 무게: g_F = (Σ A_G d dᵀ)⁺ Σ A_G d (w_G − w_F), G = 3 × 3 이웃 발자국 칸, 조건수가 1e4를 넘으면 5 × 5, 7 × 7 ---
		// 선 모양이면 의사 역행렬(rcond 1e-4)로 그 방향 기울기만. 1차식에 정확하다. 항마다 (이웃 f, Gx, Gy), 첫 항이 제 칸 몫(−Σ).
		TArray<TArray<TPair<int32, FVector2d>>> Grad;
		Grad.SetNum(NF);
		{
			TArray<int32> Nb;
			Eigen::Matrix2d Mt;
			for (int32 f = 0; f < NF; ++f)
			{
				const FIntPoint Cf = In.Cells[f];
				const auto Gather = [&](int32 Rad)
				{
					Nb.Reset();
					for (int32 Y = FMath::Max(Cf.Y - Rad, 0); Y <= Cf.Y + Rad; ++Y)
					{
						for (int32 X = FMath::Max(Cf.X - Rad, 0); X <= Cf.X + Rad; ++X)
						{
							const int32 g = SlotAt(X, Y);
							if (g != INDEX_NONE && g != f)
							{
								Nb.Add(g);
							}
						}
					}
				};
				const auto Moment = [&]()
				{
					Mt.setZero();
					for (const int32 g : Nb)
					{
						const Eigen::Vector2d D(XF[g] - XF[f], YF[g] - YF[f]);
						Mt += AF[g] * D * D.transpose();
					}
				};
				for (int32 Rad = 1; Rad <= 3; ++Rad)
				{
					Gather(Rad);
					if (Nb.Num() >= 2)
					{
						Moment();
						const Eigen::Vector2d L = Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d>(Mt, Eigen::EigenvaluesOnly).eigenvalues();
						const double Hi = FMath::Max(FMath::Abs(L(0)), FMath::Abs(L(1)));
						const double Lo = FMath::Min(FMath::Abs(L(0)), FMath::Abs(L(1)));
						if (Lo > 0.0 && Hi / Lo < 1e4)
						{
							break;
						}
					}
				}
				if (Nb.Num() == 0)
				{
					continue;
				}
				Moment();
				const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> Es(Mt);
				const Eigen::Vector2d L = Es.eigenvalues();
				const double Hi = FMath::Max(FMath::Abs(L(0)), FMath::Abs(L(1)));
				Eigen::Matrix2d Pinv = Eigen::Matrix2d::Zero();
				for (int32 i = 0; i < 2; ++i)
				{
					if (FMath::Abs(L(i)) > 1e-4 * Hi)
					{
						Pinv += Es.eigenvectors().col(i) * Es.eigenvectors().col(i).transpose() / L(i);
					}
				}
				TArray<TPair<int32, FVector2d>>& Row = Grad[f];
				Row.Add({ f, FVector2d::ZeroVector });
				for (const int32 g : Nb)
				{
					const Eigen::Vector2d W = Pinv * (AF[g] * Eigen::Vector2d(XF[g] - XF[f], YF[g] - YF[f]));
					Row.Add({ g, FVector2d(W(0), W(1)) });
					Row[0].Value -= FVector2d(W(0), W(1));
				}
			}
		}

		// --- 2. 마디 = 발자국 + 둘레 두 칸(8-이웃 두 번 넓힘), 거친 격자 안. 마디 값은 원천 발자국 칸 국소 판들을 마디 자리에서 읽은 평균 ---
		// 원천: 마디가 발자국 칸이면 그 칸 하나, 아니면 3 × 3(없으면 5 × 5) 안 발자국 칸들. E = 마디 × F 무게(replica E와 같다).
		const FIntPoint NMin(FMath::Max(CMin.X - 2, 0), FMath::Max(CMin.Y - 2, 0));
		const FIntPoint NMax(FMath::Min(CMax.X + 2, Rc - 1), FMath::Min(CMax.Y + 2, Rc - 1));
		Out.NodeBoxMin = NMin;
		Out.NodeBoxSize = NMax - NMin + FIntPoint(1, 1);
		Out.NodeSlots.Init(INDEX_NONE, Out.NodeBoxSize.X * Out.NodeBoxSize.Y);
		TArray<FTriplet> ETrip;
		{
			TArray<int32> Src;
			for (int32 Y = NMin.Y; Y <= NMax.Y; ++Y)
			{
				for (int32 X = NMin.X; X <= NMax.X; ++X)
				{
					bool bNear = false;
					for (int32 DY = -2; DY <= 2 && !bNear; ++DY)
					{
						for (int32 DX = -2; DX <= 2 && !bNear; ++DX)
						{
							bNear = SlotAt(X + DX, Y + DY) != INDEX_NONE;
						}
					}
					if (!bNear)
					{
						continue;
					}
					Src.Reset();
					if (SlotAt(X, Y) != INDEX_NONE)
					{
						Src.Add(SlotAt(X, Y));
					}
					else
					{
						for (int32 Rad = 1; Rad <= 2 && Src.Num() == 0; ++Rad)
						{
							for (int32 SY = FMath::Max(Y - Rad, 0); SY <= Y + Rad; ++SY)
							{
								for (int32 SX = FMath::Max(X - Rad, 0); SX <= X + Rad; ++SX)
								{
									if (SlotAt(SX, SY) != INDEX_NONE)
									{
										Src.Add(SlotAt(SX, SY));
									}
								}
							}
						}
					}
					const int32 Node = Out.Nodes.Num();
					Out.NodeSlots[(Y - NMin.Y) * Out.NodeBoxSize.X + (X - NMin.X)] = Node;
					const FVector2d At = PlateAt((X + 0.5) * B, (Y + 0.5) * B);
					Out.Nodes.Add(FVector4f((float)At.X, (float)At.Y, FMath::AsFloat((uint32)Out.NodeSrc.Num()), FMath::AsFloat((uint32)Src.Num())));
					const double Share = 1.0 / Src.Num();
					for (const int32 s : Src)
					{
						Out.NodeSrc.Add((uint32)s);
						ETrip.Emplace(Node, s, Share);
						for (const TPair<int32, FVector2d>& Term : Grad[s])
						{
							ETrip.Emplace(Node, Term.Key, Share * ((At.X - XF[s]) * Term.Value.X + (At.Y - YF[s]) * Term.Value.Y));
						}
					}
				}
			}
		}
		const int32 NumNodes = Out.Nodes.Num();
		FSparse E(NumNodes, NF);
		E.setFromTriplets(ETrip.GetData(), ETrip.GetData() + ETrip.Num());

		// --- 3. 판 읽기 P = Cn·E: 조각 가는 칸마다 둘레 4 × 4 마디에서 Catmull-Rom(가는 창과 같은 읽기) ---
		const auto NodeAt = [&](int32 X, int32 Y) -> int32
		{
			return X < NMin.X || Y < NMin.Y || X > NMax.X || Y > NMax.Y ? INDEX_NONE
				: Out.NodeSlots[(Y - NMin.Y) * Out.NodeBoxSize.X + (X - NMin.X)];
		};
		TArray<FTriplet> CTrip;
		CTrip.Reserve(NumFine * 16);
		for (int32 k = 0; k < NumFine; ++k)
		{
			const double QX = CoarseCoord(In.Texels[k].X, B);
			const double QY = CoarseCoord(In.Texels[k].Y, B);
			const int32 X0 = FMath::FloorToInt32(QX);
			const int32 Y0 = FMath::FloorToInt32(QY);
			double WX[4], WY[4];
			CatmullRom(QX - X0, WX);
			CatmullRom(QY - Y0, WY);
			for (int32 a = 0; a < 4; ++a)
			{
				for (int32 b = 0; b < 4; ++b)
				{
					const int32 Node = NodeAt(X0 - 1 + b, Y0 - 1 + a);
					if (Node == INDEX_NONE)
					{
						Out.Error = TEXT("the plate is too close to the edge of the contact map to read it from the coarse cells");
						return false;
					}
					CTrip.Emplace(k, Node, WY[a] * WX[b]);
				}
			}
		}
		FSparse Cn(NumFine, NumNodes);
		Cn.setFromTriplets(CTrip.GetData(), CTrip.GetData() + CTrip.Num());
		const FSparse P = Cn * E;

		// --- 4. 가는 칸 판 굽힘 c²·B_fine: 두 번 차분 둘 + 비틀림(Kirchhoff, 푸아송 0). 세 칸·2 × 2가 모두 조각(같은 차트)일 때만 센다(자유단), ---
		// 무게 = 그 칸들 D/μ = ℓ_b³의 최솟값. 그다음 거친 칸으로 B_c = Pᵀ(c²B_fine)P(가는 칸 D를 갈레르킨으로 거친 칸에).
		FIntPoint TMin(MAX_int32, MAX_int32);
		FIntPoint TMax(MIN_int32, MIN_int32);
		for (const FIntPoint& T : In.Texels)
		{
			TMin = TMin.ComponentMin(T);
			TMax = TMax.ComponentMax(T);
		}
		const int32 TW = TMax.X - TMin.X + 1;
		TArray<int32> FineAt;
		FineAt.Init(INDEX_NONE, TW * (TMax.Y - TMin.Y + 1));
		TArray<double> D;
		D.SetNumUninitialized(NumFine);
		for (int32 k = 0; k < NumFine; ++k)
		{
			FineAt[(In.Texels[k].Y - TMin.Y) * TW + (In.Texels[k].X - TMin.X)] = k;
			D[k] = FMath::Cube(BendingLength(In.Softness[k], In.HRef));
		}
		// 조각 칸이면 같은 차트일 때 그 칸, 아니면 그 자리 고스트가 가리키는 조각 칸(이음매 건너편) — 차분이 이음매를 건너간다.
		TMap<FIntPoint, int32> GhostAt;
		for (int32 g = 0; g < In.GhostTexels.Num() && g < In.GhostMember.Num(); ++g)
		{
			GhostAt.Add(In.GhostTexels[g], In.GhostMember[g]);
		}
		const auto Fine = [&](int32 X, int32 Y, int32 Chart) -> int32
		{
			if (X >= TMin.X && Y >= TMin.Y && X <= TMax.X && Y <= TMax.Y)
			{
				const int32 k = FineAt[(Y - TMin.Y) * TW + (X - TMin.X)];
				if (k != INDEX_NONE)
				{
					return In.Chart[k] == Chart ? k : INDEX_NONE;
				}
			}
			const int32* Ghost = GhostAt.Find(FIntPoint(X, Y));
			return Ghost ? *Ghost : INDEX_NONE;
		};
		TArray<FTriplet> BTrip;
		const double InvC2 = 1.0 / (C * C);
		const auto AddStencil = [&](const int32* Ks, const double* Coef, int32 N, double W)
		{
			for (int32 i = 0; i < N; ++i)
			{
				for (int32 j = 0; j < N; ++j)
				{
					BTrip.Emplace(Ks[i], Ks[j], W * Coef[i] * Coef[j] * InvC2);
				}
			}
		};
		{
			const double Second[3] = { 1.0, -2.0, 1.0 };
			const double Twist[4] = { 1.0, -1.0, -1.0, 1.0 };
			for (int32 k = 0; k < NumFine; ++k)
			{
				const int32 X = In.Texels[k].X;
				const int32 Y = In.Texels[k].Y;
				const int32 Ch = In.Chart[k];
				const int32 KX[3] = { k, Fine(X + 1, Y, Ch), Fine(X + 2, Y, Ch) };
				if (KX[1] != INDEX_NONE && KX[2] != INDEX_NONE)
				{
					AddStencil(KX, Second, 3, FMath::Min3(D[KX[0]], D[KX[1]], D[KX[2]]));
				}
				const int32 KY[3] = { k, Fine(X, Y + 1, Ch), Fine(X, Y + 2, Ch) };
				if (KY[1] != INDEX_NONE && KY[2] != INDEX_NONE)
				{
					AddStencil(KY, Second, 3, FMath::Min3(D[KY[0]], D[KY[1]], D[KY[2]]));
				}
				const int32 KT[4] = { k, Fine(X + 1, Y, Ch), Fine(X, Y + 1, Ch), Fine(X + 1, Y + 1, Ch) };
				if (KT[1] != INDEX_NONE && KT[2] != INDEX_NONE && KT[3] != INDEX_NONE)
				{
					AddStencil(KT, Twist, 4, 2.0 * FMath::Min(FMath::Min(D[KT[0]], D[KT[1]]), FMath::Min(D[KT[2]], D[KT[3]])));
				}
			}
		}
		FSparse Bf(NumFine, NumFine);
		Bf.setFromTriplets(BTrip.GetData(), BTrip.GetData() + BTrip.Num());
		const FSparse BfP = Bf * P;
		const FSparse Pt = P.transpose();
		const FSparse BcSparse = Pt * BfP;
		Eigen::MatrixXd Bc = BcSparse.toDense();
		Bc = (0.5 * (Bc + Bc.transpose())).eval();

		// --- 5. 판 모드: 강체 셋 Q3 = orth(1, x_F − x̄, y_F − ȳ)의 직교 여공간에서 B_c를 고유 분해 → ψ(Σ_F ψ² = 1), β(오름차순) ---
		double MeanX = 0.0;
		double MeanY = 0.0;
		for (int32 f = 0; f < NF; ++f)
		{
			MeanX += XF[f];
			MeanY += YF[f];
		}
		MeanX /= NF;
		MeanY /= NF;
		Eigen::MatrixXd Base(NF, 3);
		for (int32 f = 0; f < NF; ++f)
		{
			Base(f, 0) = 1.0;
			Base(f, 1) = XF[f] - MeanX;
			Base(f, 2) = YF[f] - MeanY;
		}
		const Eigen::JacobiSVD<Eigen::MatrixXd> Svd(Base, Eigen::ComputeThinU);
		const Eigen::VectorXd& Sv = Svd.singularValues();
		int32 Rank = 0;
		for (int32 i = 0; i < Sv.size(); ++i)
		{
			Rank += Sv(i) > 1e-8 * Sv(0) ? 1 : 0;
		}
		const Eigen::MatrixXd Q3 = Svd.matrixU().leftCols(Rank);
		Out.RigidRank = Rank;
		const int32 M = FMath::Max(NF - Rank, 0);
		Out.NumModes = M;
		Eigen::VectorXd Beta(M);
		Eigen::MatrixXd Psi(NF, M);
		if (M > 0)
		{
			const Eigen::HouseholderQR<Eigen::MatrixXd> Qr(Q3);
			const Eigen::MatrixXd Q = Qr.householderQ();
			const Eigen::MatrixXd Qc = Q.rightCols(M);
			Eigen::MatrixXd Ms = Qc.transpose() * Bc * Qc;
			Ms = (0.5 * (Ms + Ms.transpose())).eval();
			const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> Es(Ms);
			if (Es.info() != Eigen::Success)
			{
				Out.Error = TEXT("the plate bending eigen decomposition did not converge");
				return false;
			}
			Beta = Es.eigenvalues();
			Psi = Qc * Es.eigenvectors();
		}

		// --- 6. 모드 파수 k² = ψᵀLψ(L = 발자국 네 이웃 그래프 라플라시안 ÷ (B·c)²) — 실행 중 모드 줄 앞처리 어림 ---
		Out.Beta.SetNum(M);
		Out.WaveNumber.SetNum(M);
		Out.Shapes.SetNum(NF * M);
		const double MaxBeta = M > 0 ? Beta.maxCoeff() : 0.0;
		// 굽기 확인: 굽힘 에너지는 무게 ≥ 0인 제곱 합이라 β ≥ 0이고, 음수는 반올림 크기(굽힘 행렬 크기 × 기계 정밀도)여야 한다. 그 판의 가장 큰 β로
		// 나누면 굽힘이 거의 없는 판(가장 큰 β ≈ 0, 이음매로 갈린 작은 판)에서 반올림 음수가 −97, −2e16 같은 거짓 경보가 됐다(일지 300) — 굽힘 행렬
		// 크기로도 재서 큰 쪽으로 나눈다.
		const double BetaScale = FMath::Max(MaxBeta, Bc.cwiseAbs().maxCoeff());
		Out.MinBetaRatio = M > 0 && BetaScale > 0.0 ? Beta.minCoeff() / BetaScale : 0.0;
		for (int32 i = 0; i < M; ++i)
		{
			double K2 = 0.0;
			for (int32 f = 0; f < NF; ++f)
			{
				const int32 Right = SlotAt(In.Cells[f].X + 1, In.Cells[f].Y);
				const int32 Down = SlotAt(In.Cells[f].X, In.Cells[f].Y + 1);
				if (Right != INDEX_NONE)
				{
					K2 += FMath::Square(Psi(f, i) - Psi(Right, i));
				}
				if (Down != INDEX_NONE)
				{
					K2 += FMath::Square(Psi(f, i) - Psi(Down, i));
				}
			}
			Out.WaveNumber[i] = (float)(FMath::Sqrt(FMath::Max(K2, 0.0)) / (B * C));
			Out.Beta[i] = (float)FMath::Max(Beta(i), 0.0);
			Out.ExtraNull += Beta(i) < 1e-9 * MaxBeta ? 1 : 0;
		}
		Eigen::MatrixXd PsiHalf(NF, M);
		for (int32 f = 0; f < NF; ++f)
		{
			for (int32 i = 0; i < M; ++i)
			{
				const FFloat16 H((float)Psi(f, i));
				Out.Shapes[f * M + i] = H.Encoded;
				PsiHalf(f, i) = H.GetFloat();
			}
		}
		if (M > 0)
		{
			const Eigen::MatrixXd Eye = Eigen::MatrixXd::Identity(M, M);
			Out.OrthoError = (Psi.transpose() * Psi - Eye).cwiseAbs().maxCoeff();
			Out.HalfOrthoError = (PsiHalf.transpose() * PsiHalf - Eye).cwiseAbs().maxCoeff();
			Out.RigidLeak = (Q3.transpose() * Psi).cwiseAbs().maxCoeff();
		}

		// --- 칸마다 줄 자리·기울기 항 ---
		Out.CellPlate.SetNum(NF);
		Out.GradRange.SetNum(NF);
		for (int32 f = 0; f < NF; ++f)
		{
			Out.CellPlate[f] = FVector4f((float)XF[f], (float)YF[f], (float)AF[f], 0.f);
			Out.GradRange[f] = FUintVector2((uint32)Out.GradEntries.Num(), (uint32)Grad[f].Num());
			for (const TPair<int32, FVector2d>& Term : Grad[f])
			{
				Out.GradEntries.Add(FVector4f(FMath::AsFloat((uint32)Term.Key), (float)Term.Value.X, (float)Term.Value.Y, 0.f));
			}
		}
		return true;
	}

	void WriteDebugCsv(const FPieceInput& In, const FPieceOutput& Out, const FString& Path)
	{
		// 줄 머리: P = 굽기 값, T = 가는 칸(아틀라스 x, y, 판 x, y, S, 차트), C = 발자국 칸(거친 x, y, x_F, y_F, A_F), M = 모드(β, k)
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("P,%d,%.17g,%.17g,%d"), In.Block, In.TexelSize, In.HRef, In.CoarseResolution));
		for (int32 k = 0; k < In.Texels.Num(); ++k)
		{
			Lines.Add(FString::Printf(TEXT("T,%d,%d,%.17g,%.17g,%.17g,%d"), In.Texels[k].X, In.Texels[k].Y, In.Plate[k].X, In.Plate[k].Y,
				In.Softness[k], In.Chart[k]));
		}
		for (int32 f = 0; f < In.Cells.Num() && f < Out.CellPlate.Num(); ++f)
		{
			Lines.Add(FString::Printf(TEXT("C,%d,%d,%.9g,%.9g,%.9g"), In.Cells[f].X, In.Cells[f].Y, Out.CellPlate[f].X, Out.CellPlate[f].Y,
				Out.CellPlate[f].Z));
		}
		for (int32 i = 0; i < Out.NumModes; ++i)
		{
			Lines.Add(FString::Printf(TEXT("M,%.9g,%.9g"), Out.Beta[i], Out.WaveNumber[i]));
		}
		FFileHelper::SaveStringArrayToFile(Lines, *Path);
	}
}
