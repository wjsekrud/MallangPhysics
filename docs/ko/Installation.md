# 설치

MallangPhysics의 설치 방법을 안내합니다.

Windows의 Unreal Engine 5.6, 5.7, 5.8과 호환됩니다. 화면 확인은 대부분 5.8의 D3D12와 D3D11에서 했습니다.

## 소스 {#source}

[GitHub 저장소](https://github.com/wjsekrud/MallangPhysics)의 `Unreal/SoftbodySystem` 폴더를 내 프로젝트의 `Plugins` 폴더에 복사하고 프로젝트를 빌드합니다. 같은 소스가 5.6, 5.7, 5.8에서 빌드됩니다. 빌드에는 C++ 컴파일러가 필요합니다. 라이선스는 [MIT](https://github.com/wjsekrud/MallangPhysics/blob/main/LICENSE)입니다.

## 미리 빌드한 판 {#github}

[GitHub 릴리스](https://github.com/wjsekrud/MallangPhysics/releases)에서 엔진 버전에 맞는 `MallangPhysics-1.0.0-UE5.x-Win64.zip`의 `SoftbodySystem` 폴더를 내 프로젝트의 `Plugins` 폴더에 풉니다. 블루프린트 전용 프로젝트에서도 쓸 수 있습니다. 바로 열어 볼 수 있는 장면은 따로 있는 [데모 프로젝트](Demo-Project)에 있습니다.

미리 빌드한 판에는 C++ 소스 없이 셰이더와 공개 헤더만 들어 있습니다. zip에 적힌 엔진 버전에서만 열립니다. C++ 프로젝트는 Development와 Shipping 구성으로 빌드하고 패키징할 수 있으며, DebugGame 구성은 빌드되지 않습니다.

## C++ {#c}

런타임 모듈은 `Softbody`입니다.

```csharp
PublicDependencyModuleNames.Add("Softbody");
```

헤더는 파일 이름으로 포함합니다(예: `#include "SoftbodyComponent.h"`). 베이크는 에디터 전용 모듈 `SoftbodyEditor`에 있습니다.

미리 빌드한 판에 든 헤더는 `SoftbodyComponent.h`, `SoftbodyGrabberComponent.h`, `SoftbodyMeshData.h`, `SoftbodySettings.h`, `SoftbodyColliderShapes.h`입니다.
