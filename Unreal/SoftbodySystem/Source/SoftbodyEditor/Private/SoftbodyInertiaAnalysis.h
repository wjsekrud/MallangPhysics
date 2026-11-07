// Copyright (c) 2026 wjsekrud

#pragma once

class USoftbodyMeshData;

/** 관성용 살의 무름(ADR-0005)의 배열·솔버·정규화·방향 계약을 검사한다. */
int32 VerifySoftbodyInertiaResponseBake(const USoftbodyMeshData* Data);
