// Copyright (c) 2026 wjsekrud

// 디버그: 사용자가 켜고 끄는 구간 기록(콘솔 명령 Softbody.Record)과, 그 기록을 창 없는 실행에 다시 거는 쪽(Softbody.Record apply).
#pragma once

#include "CoreMinimal.h"

class UWorld;
class USoftbodyComponent;

namespace SoftbodyRecord
{
	/**
	 * 에디터 모듈이 채운다: 이 월드를 보는 레벨 에디터 뷰포트의 카메라(자리·방향·가로 시야각 도). 뷰포트가 없거나 다른 월드면 false.
	 * PIE·게임 월드는 플레이어 카메라를 쓴다(런타임 모듈은 레벨 에디터 뷰포트를 모른다).
	 */
	extern SOFTBODY_API TFunction<bool(UWorld* World, FVector& OutLocation, FRotator& OutRotation, float& OutFov)> GetEditorView;
}

namespace SoftbodyCapture
{
	/**
	 * Softbody.CaptureField와 같은 캡처를 Folder에 저장한다: 다음에 도는 갱신 끝에 사본을 뜨고, 복사되면(보통 다음 틱) 저장한다.
	 * 지금 찍을 수 없는 장(잠듦·접촉 지도 올리는 중·앞 캡처 저장 중)은 빼고 요청한 수를 돌려준다. Notes는 capture.txt 머리 줄 끝에 붙는다.
	 */
	SOFTBODY_API int32 CaptureInto(const TArray<USoftbodyComponent*>& Components, bool bFullAtlas, const FString& Folder, const FString& Notes);
}
