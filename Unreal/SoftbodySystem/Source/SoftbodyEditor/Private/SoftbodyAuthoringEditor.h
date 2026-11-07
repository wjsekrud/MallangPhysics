// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "SoftbodyComponent.h"
#include "SoftbodySimulationPreview.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/StrongObjectPtr.h"

class FToolBarBuilder;
class IDetailsView;
class SDockTab;
class SSoftbodyCorePreviewViewport;
class USoftbodyAuthoringData;

/**
 * Softbody Authoring Data 창(일지 232): 왼쪽은 지금까지의 속성 패널(버튼 포함) 그대로, 오른쪽은 미리보기.
 * Rigid Core 보기는 Target Mesh를 반투명으로, 굽기가 붙들 속을 주황 구로, Volume Mesh를 파란 선으로 그린다.
 * 구는 USoftbodyAuthoringData::PreviewRigidCore — 측정 굽기와 같은 함수·같은 반지름 하한이다.
 * 굽기 전 보기는 둘이 더 있다: Separated Parts(Separated Pairs의 두 묶음)와 칠 넷(슬롯 칠 텍스처의 Depth·Jiggle·Volume·
 * Softness, ADR-0038) — 굽기와 같은 함수로 지금 값을 메시 사본의 정점 색으로 칠한다.
 * 나머지 보기(일지 233)는 마지막 굽기 결과를 컴포넌트 Debug View와 같은 색으로 메시에 칠한다. Flesh Thickness(ADR-0037 §3.5)와 Lump Base
 * (ADR-0034 덩어리 받침 막, 굽기가 남긴 에디터 전용 기록)는 Debug View에 없는 보기로, CPU가 정점 색으로 칠한다(cm, 배율 1).
 * Simulation 탭(일지 234, 오른쪽 Preview 뒤)은 마지막 굽기를 게임처럼 돌린다: 미리보기 월드에 메시 + 컴포넌트, Ctrl + 좌클릭으로
 * 누르고 Ctrl + 우클릭으로 원 안 살갗을 잡아 끌고(Shift를 더하면 메시 전체 — 피직스 에셋 에디터와 같은 손), 클릭해 고르면 기즈모로 옮기고, 스켈레탈은 애니메이션을 튼다. 처음 보일 때 월드를 만들고, 탭을 닫으면 월드째 지운다.
 * Simulation Details 탭(왼쪽 Details 뒤)은 그 미리보기의 설정과 컴포넌트 값이다. 값은 이 창에서만 쓰고(창을 닫으면 사라진다) 내보내기는
 * 없다 — 디테일 패널의 Shift + 우클릭 복사가 그 길이다. 도구 막대 Simulate는 두 탭을 앞으로 가져온다.
 */
class FSoftbodyAuthoringEditor : public FAssetEditorToolkit
{
public:
	void InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host, USoftbodyAuthoringData* InAsset);

	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;

	/** Simulation 탭과 Simulation Details 탭을 앞으로 가져온다(닫혔으면 연다). */
	void OpenSimulation();

private:
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnPreviewTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnSimulationTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnSimulationDetailsTab(const FSpawnTabArgs& Args);
	void FillToolbar(FToolBarBuilder& Builder);

	TWeakObjectPtr<USoftbodyAuthoringData> Asset;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SSoftbodyCorePreviewViewport> Preview;
	// Simulation Details 탭 내용(설정 + 컴포넌트 값). 값 패널은 시뮬레이션이 돌면 그 컴포넌트를, 아니면 보관용을 보인다.
	TSharedPtr<SWidget> SimulationDetailsPanel;
	TSharedPtr<IDetailsView> SimulationValueDetails;
	// Simulation 탭의 값: 탭을 닫았다 열어도 남고 SA 창을 닫으면 사라진다. 컴포넌트는 등록하지 않은 값 보관용이다.
	TStrongObjectPtr<USoftbodySimulationPreviewSettings> SimulationSettings;
	TStrongObjectPtr<USoftbodyComponent> SimulationValues;
};
