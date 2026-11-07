# Credits and License

## License {#license}

Copyright © 2026 wjsekrud. All Rights Reserved. Third-party code in the package follows its own license, listed below.

The package ships as compiled assemblies (DLLs) without C# source. The shaders and include files are included as source.

## Third-party code in the package {#third-party-code-in-the-package}

The full notices and license text are in `Third Party Notices.md` in the package.

- **geometry3Sharp** (Ryan Schmidt / gradientspace, Boost Software License 1.0): `Editor/ThirdParty/geometry3Sharp/Softbody.Geometry3Sharp.dll`. An editor-only assembly used for the contact map bake, so it is not included in builds. It was compiled with only the namespace changed (`g3` → `Softbody.G3`) so that it does not clash with other copies, and the original license file is next to the DLL.
- **Universal Render Pipeline 17.3.0** (Unity Technologies, Unity Companion License): the Softbody/Lit shaders and part of their inspector copy or are based on URP's Lit shader and its editor code.

| Package file | URP original |
|---|---|
| `Shaders/SoftbodyLit.shader` | Properties and pass structure of `Shaders/Lit.shader` |
| `Shaders/SoftbodyLitTessellated.shader` | Properties and pass structure of `Shaders/Lit.shader` (via `SoftbodyLit.shader`) |
| `Shaders/Lit/SoftbodyLitInput.hlsl` | `Shaders/LitInput.hlsl` |
| `Shaders/Lit/SoftbodyObjectMotionVectors.hlsl` | `ShaderLibrary/ObjectMotionVectors.hlsl` |
| `Shaders/Lit/SoftbodyLitTessMotionVectors.hlsl` | `ShaderLibrary/ObjectMotionVectors.hlsl` (via `SoftbodyObjectMotionVectors.hlsl`) |
| `Shaders/Lit/SoftbodyLitEditorPass.hlsl` | Follows `Shaders/Particles/ParticlesEditorPass.hlsl` |
| `SoftbodyLitShaderGUI` in `Editor/Softbody.Editor.dll` | `Editor/ShaderGUI/Shaders/LitShader.cs`, `Editor/ShaderGUI/ShadingModels/LitDetailGUI.cs` |
| `SoftbodyLitMaterialKeywords` in `Editor/Softbody.Editor.dll` | `SetMaterialKeywords` in `Editor/ShaderGUI/ShadingModels/LitDetailGUI.cs` |

## Unity packages the package builds on {#unity-packages-the-package-builds-on}

Installed together by the Package Manager, and not included in this package (all Unity Technologies).

- Universal RP 17.3.0, Mathematics 1.3.3, Collections 2.6.8: Unity Companion License.
- Burst 1.8.30: Unity Companion License for the source, Unity Package Distribution License for the rest.

## Sample content {#sample-content}

- Quick Start sample (`Samples~/QuickStart/`): the cushion mesh `SM_SampleCushion.fbx` was made by the author with a Blender script. The prefab and scene in the same folder were also made by the author, and the ball and floor in the scene are Unity default meshes with URP default materials. There are no downloaded models or textures.

## Trademarks {#trademarks}

<!-- TRADEMARK: wiki-shell 초안 -->

## Documentation site {#documentation-site}

- Korean-side font: Pretendard (Kil Hyung-jin, SIL Open Font License 1.1).
