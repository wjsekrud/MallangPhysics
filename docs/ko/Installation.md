# 설치

MallangPhysics의 설치 방법을 안내합니다.

Windows의 Unreal Engine 5.6, 5.7, 5.8과 호환됩니다. 화면 확인은 대부분 5.8의 D3D12와 D3D11에서 했습니다.

## Fab {#fab}

Epic Games 런처에서 엔진에 설치합니다. C++ 소스가 들어 있습니다.

## GitHub {#github}

엔진 버전에 맞는 `MallangPhysics-1.0.0-UE5.x-Win64.zip`의 `SoftbodySystem` 폴더를 내 프로젝트의 `Plugins` 폴더에 풉니다. 바로 열어 볼 수 있는 장면은 따로 있는 [데모 프로젝트](Demo-Project)에 있습니다.

GitHub 판은 C++ 소스 없이 미리 빌드한 판이며, 셰이더는 들어 있습니다. zip에 적힌 엔진 버전에서만 열립니다. C++ 프로젝트는 Development와 Shipping 구성으로 빌드하고 패키징할 수 있으며, DebugGame 구성은 빌드되지 않습니다.

## C++ {#c}

런타임 모듈은 `Softbody`입니다.

```csharp
PublicDependencyModuleNames.Add("Softbody");
```

헤더는 파일 이름으로 포함합니다(예: `#include "SoftbodyComponent.h"`). 베이크는 에디터 전용 모듈 `SoftbodyEditor`에 있습니다.

GitHub 판에 든 헤더는 `SoftbodyComponent.h`, `SoftbodyGrabberComponent.h`, `SoftbodyMeshData.h`, `SoftbodySettings.h`, `SoftbodyColliderShapes.h`입니다.
