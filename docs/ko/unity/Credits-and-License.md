# 크레딧과 라이선스

## 라이선스 {#license}

[MIT 라이선스](https://github.com/wjsekrud/MallangPhysics/blob/main/Unity/com.wjsekrud.mallangphysics/LICENSE.md). Copyright © 2026 wjsekrud. 패키지에 든 제3자 코드는 아래 각 라이선스를 따릅니다.

패키지는 C# 소스 없이 컴파일된 어셈블리(DLL)로 배포합니다. 셰이더와 인클루드 파일은 소스로 들어 있습니다.

## 패키지에 든 제3자 코드 {#third-party-code-in-the-package}

전체 고지와 라이선스 문구는 패키지의 `Third Party Notices.md`에 있습니다.

- **geometry3Sharp** (Ryan Schmidt / gradientspace, Boost Software License 1.0): `Editor/ThirdParty/geometry3Sharp/Softbody.Geometry3Sharp.dll`. 접촉 맵 베이크에 쓰는 에디터 전용 어셈블리라 빌드에 들어가지 않습니다. 다른 사본과 겹치지 않게 네임스페이스만 바꿔(`g3` → `Softbody.G3`) 컴파일했고, 원래 라이선스 파일이 DLL 옆에 있습니다.
- **Universal Render Pipeline 17.3.0** (Unity Technologies, Unity Companion License): Softbody/Lit 셰이더와 그 인스펙터의 일부는 URP의 Lit 셰이더와 그 에디터 코드를 복사하거나 바탕으로 했습니다.

| 패키지 파일 | URP 원본 |
|---|---|
| `Shaders/SoftbodyLit.shader` | `Shaders/Lit.shader`의 속성과 패스 구성 |
| `Shaders/SoftbodyLitTessellated.shader` | `Shaders/Lit.shader`의 속성과 패스 구성(`SoftbodyLit.shader`를 거쳐) |
| `Shaders/Lit/SoftbodyLitInput.hlsl` | `Shaders/LitInput.hlsl` |
| `Shaders/Lit/SoftbodyObjectMotionVectors.hlsl` | `ShaderLibrary/ObjectMotionVectors.hlsl` |
| `Shaders/Lit/SoftbodyLitTessMotionVectors.hlsl` | `ShaderLibrary/ObjectMotionVectors.hlsl`(`SoftbodyObjectMotionVectors.hlsl`을 거쳐) |
| `Shaders/Lit/SoftbodyLitEditorPass.hlsl` | `Shaders/Particles/ParticlesEditorPass.hlsl`를 따름 |
| `Editor/Softbody.Editor.dll`의 `SoftbodyLitShaderGUI` | `Editor/ShaderGUI/Shaders/LitShader.cs`, `Editor/ShaderGUI/ShadingModels/LitDetailGUI.cs` |
| `Editor/Softbody.Editor.dll`의 `SoftbodyLitMaterialKeywords` | `Editor/ShaderGUI/ShadingModels/LitDetailGUI.cs`의 `SetMaterialKeywords` |

## 패키지가 쓰는 Unity 패키지 {#unity-packages-the-package-builds-on}

Package Manager가 함께 설치하며, 이 패키지에 들어 있지 않습니다(모두 Unity Technologies).

- Universal RP 17.3.0, Mathematics 1.3.3, Collections 2.6.8: Unity Companion License.
- Burst 1.8.30: 소스는 Unity Companion License, 그 밖은 Unity Package Distribution License.

## 견본 콘텐츠 {#sample-content}

- Quick Start 견본(`Samples~/QuickStart/`): 방석 메시 `SM_SampleCushion.fbx`는 제작자가 Blender 스크립트로 만들었습니다. 같은 폴더의 프리팹과 장면도 제작자가 만들었고, 장면의 공과 바닥은 Unity 기본 메시와 URP 기본 머티리얼입니다. 내려받은 모델이나 텍스처는 없습니다.

## 상표 {#trademarks}

<!-- TRADEMARK: wiki-shell 초안 -->

## 문서 사이트 {#documentation-site}

- 한국어 쪽 글꼴: Pretendard (길형진, SIL Open Font License 1.1).
