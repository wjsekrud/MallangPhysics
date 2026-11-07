# Installation

How to install MallangPhysics.

It works with Unreal Engine 5.6, 5.7 and 5.8 on Windows. Most visual checks were done on 5.8 with D3D12 and D3D11.

## Source {#source}

Copy the `Unreal/SoftbodySystem` folder of the [GitHub repository](https://github.com/wjsekrud/MallangPhysics) into your own project's `Plugins` folder and build the project. The same source builds on 5.6, 5.7 and 5.8. Building needs a C++ compiler. The license is [MIT](https://github.com/wjsekrud/MallangPhysics/blob/main/LICENSE).

## Prebuilt {#github}

From the [GitHub releases](https://github.com/wjsekrud/MallangPhysics/releases), unzip the `SoftbodySystem` folder of `MallangPhysics-1.0.0-UE5.x-Win64.zip` for your engine version into your own project's `Plugins` folder. It also works in Blueprint-only projects. A scene you can open right away is in the separate [demo project](Demo-Project).

The prebuilt version contains the shaders and public headers without the C++ source. It opens only in the engine version named in the zip. C++ projects can build and package in the Development and Shipping configurations; the DebugGame configuration does not build.

## C++ {#c}

The runtime module is `Softbody`.

```csharp
PublicDependencyModuleNames.Add("Softbody");
```

Headers are included by file name, for example `#include "SoftbodyComponent.h"`. The bake is in the editor-only module `SoftbodyEditor`.

The prebuilt version includes these headers: `SoftbodyComponent.h`, `SoftbodyGrabberComponent.h`, `SoftbodyMeshData.h`, `SoftbodySettings.h` and `SoftbodyColliderShapes.h`.
