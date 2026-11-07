# Installation

Unity 6.3 (verified on 6000.3.26f1) and URP 17.3 are required. It does not run on the Built-in Render Pipeline or HDRP, and the GPU must run compute shaders.

## Package Manager {#package-manager}

In the Package Manager, enter the following address in **Install package from git URL**.

```text
https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics
```

To pin the version, append a tag to the end of the address. The tag for this beta is `unity-v0.1.0-beta.1`.

```text
https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics#unity-v0.1.0-beta.1
```

An address without a tag gets the latest commit of the repository. The package appears as **MallangPhysics** (`com.wjsekrud.mallangphysics`). Universal RP 17.3.0, Burst 1.8.30, Mathematics 1.3.3 and Collections 2.6.8 come with it.

The package has no C# source. The runtime and editor code are included as compiled assemblies (DLLs), and the shaders and include files are included as source.

## Graphics API {#graphics-api}

Opening the editor with D3D12 is recommended. The contact shaders are compiled once per graphics API, and then stay in the project's Library cache.

| API | First compile |
|---|---|
| D3D12 | A few seconds |
| Vulkan | A few seconds |
| D3D11 | About 15 minutes (on a fast PC, longer on a slow PC) |

Compilation happens the first time a scene with a Softbody enters Play. The editor opens with the top API in the Graphics APIs list of the Windows Player settings. Metal is not verified, and **Contact Detail** Full does not run on Metal ([Known Limitations](./Known-Limitations#contact-detail)).

If the GPU or graphics API cannot run the contact shaders, the Console shows the warning `Contact is OFF for every Softbody component` once, and only the jiggle runs with contact off.

## After installing {#after-installing}

- **Project Settings > Softbody** holds the project-wide performance settings. Without a settings asset, the defaults are used. To change values, press **Create Settings Asset** on that page. This creates `Assets/Resources/SoftbodySettings.asset`, which must have this name inside a Resources folder to be read in builds.
- `Runtime/Resources/SoftbodyRuntimeResources.asset` inside the package puts the contact shaders into the build. Do not edit or move it. If this asset is broken, contact is turned off with a Console warning.
- Softbody reads its values by the vertex index at bake time. For that reason, do not use rendering methods that reorder vertices on Softbody meshes.
  - If **GPU Resident Drawer** is on in the URP asset, add a **Disallow GPU Driven Rendering** component to each Mesh Renderer that has a Softbody.
  - Turn off **Batching Static** on Softbody meshes. A statically batched mesh has both contact and inertia turned off.

  **Validate** on the authoring asset and **Validate Setup** on the Softbody component both warn about these.

## Scripting {#scripting}

The runtime assembly is `Softbody.Runtime.dll` (namespace `Softbody`), and the component class is `SoftbodyComponent`. The bake is in the editor-only assembly `Softbody.Editor.dll` (namespace `Softbody.Editor`). Both are Auto Referenced, so scripts without an asmdef and asmdefs with **Override References** off use them directly. An asmdef with **Override References** on adds these DLLs to **Assembly References**.
