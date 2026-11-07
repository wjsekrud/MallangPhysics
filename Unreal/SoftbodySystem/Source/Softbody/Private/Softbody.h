// Copyright (c) 2026 wjsekrud

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

/** stat softbody 그룹. 컴포넌트(틱·GPU 시간·텍스처)와 월드 묶음(텍스처 풀)이 같이 쓴다. */
DECLARE_STATS_GROUP(TEXT("Softbody"), STATGROUP_Softbody, STATCAT_Advanced);
