# MallangPhysics (Unity beta)

Real-time soft flesh for skinned and static meshes in URP. Colliders dent the surface, lumps of flesh shift when pushed wide, and the flesh jiggles as the character moves. You bake a mesh once in the editor and add one component; the deformation runs on the GPU.

This is the Unity beta of MallangPhysics. Docs: https://wjsekrud.github.io/MallangPhysics/unity/ (Korean: https://wjsekrud.github.io/MallangPhysics/ko/unity/).

The package ships compiled assemblies (`Softbody.Runtime.dll`, `Softbody.Editor.dll`) without C# source. Shaders and include files are source.

## What the beta does

- Bake: one **Run Bake** on a **Softbody Authoring Data** asset measures the flesh and bakes paint, materials and the contact map.
- **Contact Detail**: Basic gives dents, recovery, memory-foam marks, skin tension and lump shifting. Full adds the rise around a dent, skin dragged by a moving hand, Edge Smoothing and Lump Grip (a gripping hand pulls a lump along).
- Hard surface: paint Flex black on gems, buckles and plates and they sink and tilt as one rigid piece (Contact Detail Full).
- **Separated Pairs**: two parts that share one surface but have separate flesh underneath, such as a left and right lump under one garment, are pressed separately.
- Inertia: jiggle, with per-part groups.
- **Softbody/Lit** and **Softbody/Lit (Tessellated)** shaders, plus include files for your own URP or Shader Graph materials.
- Shape fields: hands and props press with their baked shape (between the fingers, a cupped palm) instead of their colliders.

## What it does not do yet

- Self contact (a character pressing its own body), volume meshes, automatic contact-map seams, skin grabbing (Softbody Grabber).
- Contact Detail Full on Metal: a mesh set to Full runs as Basic there and the Console says so. The same happens on a GPU that cannot run 1024-thread compute groups.

The full list is in Known Limitations on the documentation site.

## Requirements

Unity 6.3 (6000.3.26f1 tested), URP 17.3, a GPU with compute shaders. Windows with D3D12, D3D11 or Vulkan was tested; Metal was not (Contact Detail Full does not run there).

## Install

Package Manager > **+** > **Install package from git URL**:

```text
https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics#unity-v0.1.0-beta.1
```

Without `#unity-v0.1.0-beta.1` you get the latest commit of the repository.

Then open the package in Package Manager, Samples tab, and import **Quick Start**. Its `README.md` walks through baking the cushion and pressing it with a ball. The **Shader Graph** sample is a Shader Graph with the Softbody Custom Function nodes already wired, to copy for your own materials.

## Notes

- The first Play in a scene with a Softbody compiles the contact shaders for your graphics API (seconds on D3D12 and Vulkan, about 15 minutes on D3D11 on a fast PC), then caches them in `Library`.

Third-party code and licenses: `Third Party Notices.md`. License of this package: see `LICENSE.md`.
