# Known Limitations

What does not work in this beta, and what to do instead. Log messages and fixes are in [Troubleshooting](./Troubleshooting).

## Not in the beta {#not-in-the-beta}

- **There is no self contact.** A character cannot press its own belly or thigh with its own hand. The colliders of the body and the [shape field](./Colliders#shape-fields) of the hand are under **Ignore Root** and do not press their own flesh, and only press other Softbody meshes.
- **An inner mesh cannot be given separately.** A mesh that is not closed, such as a one-sided sheet or an open tube, cannot have its flesh measured, so the bake stops. Close the mesh in a DCC tool.
- **Tessellation exists only in Softbody/Lit (Tessellated).** In Shader Graph and in your own shaders, a narrow dent between vertices (a fingertip) shows only as shading. Make such meshes dense. Not verified on Metal, and on devices that do not support tessellation it is drawn with Softbody/Lit.
- **Contact map seams are not made automatically.** **Contact Map Seams** supports only Mesh UV0 Islands. If UV0 cannot be used (zero-area or folded UVs), the bake stops, so fix UV0. If you choose Automatic (XAtlas), the bake stops.
- **The preview of Softbody Authoring Data has only the Rigid Core view.** There is no preview for paint, flesh thickness, Separated Pairs or plates, and no in-editor simulation preview. Check dents during Play. Check the bake results of plates and Separated Pairs in the Console log.
- **There is no Softbody Grabber.** It is a tool that pulls skin with a hand.
- **Flex Offset is an unfinished feature and is unstable.** Use it with care.
- **There are no development tools such as record and replay.** Check state and GPU time with the [Softbody Debugger](./Project-Settings#softbody-debugger) and the Unity Profiler.
- **There is no C# source.** The runtime and editor code are included only as compiled assemblies (DLLs), so you cannot edit and recompile them or debug with source. The shaders and include files are source.

## Contact Detail {#contact-detail}

- **Contact Detail Full runs on D3D12, D3D11 and Vulkan.** On Metal, and on GPUs that cannot run compute thread groups of 1024 threads (8 along Z), it runs as Basic and the Console gets one line, `Contact Detail Full cannot run on this graphics device, so this mesh runs as Basic`. The setting value is saved as it is. In Basic, rise, drag, **Edge Smoothing**, **Lump Grip** and Flex paint (bending surface and plates) are turned off.
- **On a GPU that can run Full, memory is used even if Full is not used.** When contact is first turned on, about 8 MB of response tables are uploaded to the GPU, and temporary buffers for Full (about 4 MB shared, about 2 MB per set of contact textures) are created together. This avoids hitches when toggling or pressing for the first time.
- **Separated Pairs does not work on D3D11 and Metal.** The two separated parts are pressed together like an unseparated mesh. The shader compiler for these APIs cannot finish that calculation within the compile limit. Use D3D12 or Vulkan (since Unity 6.1, D3D12 is the default for new Windows projects).
- **On D3D11, plates (pieces painted with Flex) can be turned off on a slow CPU.** One plate compute shader takes several minutes to compile for the first time, and if it exceeds the compile limit, plates are pressed like flesh and the Console gets one warning line. This does not happen on D3D12 and Vulkan.

## Contact {#contact}

- **Between two Softbody meshes that press each other, a gap opens by the amount they overlap.** Let only one side press. Move the colliders of the mesh that is only to be pressed to a layer that is not in the pressing side's **Collider Channel**, or add **Softbody Ignore**. See [Colliders](./Colliders#softbody-meshes-pressing-each-other).
- **A mesh resting on a thin Box Collider can sink toward that collider.** This happens when the mesh thickness plus the thickness of the Box Collider under it is less than 1.3 times **Max Contact Depth** (19.5 cm at the default 15 cm). Make the floor collider thicker, move the floor to a layer that is not in **Collider Channel**, or add **Softbody Ignore** to the floor.
- **Non-convex Mesh Colliders, Terrain Colliders and Wheel Colliders do not press.** Only Sphere, Capsule, Box, Convex Mesh Colliders and Character Controllers press. Add such a collider to the object.
- **On a mesh that updates rarely, a wide dent spreads a little less** (when it is small on screen and caught by **Full Rate Screen Size**, or held by **Limit Contact Update Rate**). If you turned on that limit, raise **Contact Updates Per Second**.
- **A pressed mesh that stands still does not skip updates.** While a collider touches, GPU cost continues even if nothing moves. If many meshes stay in contact for a long time, use the limits in [Performance](./Performance).
- **Skin Tension is stretched skin, not a hard shell.** A small piece with strong Skin Tension sinks almost horizontally as a whole, but does not tilt like a plate when pressed off center. There is no hard shell either, such as crème brûlée or embedded plastic.
- **Turning off the component leaves the contact map in memory.** While Mesh Data is referenced, its contact map texture stays loaded. What shrinks when turned off is the set of textures that compute dents and the GPU work.
- **Drag is turned off on meshes longer than about 10 m** (the drag grid exceeds 1024 cells per side). Dent and rise work as before. There is no workaround.
- **Very slow drag disappears from rounding error.** At 60 contact updates per second, a hand that moves slower than 0.03 mm/s with the skin lagging by 1 mm, or slower than 0.29 mm/s with the skin lagging by 10 mm, cannot move the skin. There is no workaround.
- **A very fast hand drags a freshly touched cell a little less.** It happens when the hand moves more than about 1.3 cm in one contact update. If fast hands matter, do not limit the contact update rate below 60 per second.
- **A lump with Lump Grip on can keep trembling after an open hand sweeps past it very fast.** Lower **Lump Grip** (0 is off), or fill the gaps between fingers with colliders.

## Bake {#bake}

- **Only LOD0 is baked and deformed.** The other LODs in an LOD Group neither dent nor jiggle. Make LOD0 visible at close viewing distances.
- **Baked values are read by vertex index.** The bake does not change the mesh, but if you reimport or edit it after baking so that its vertices change, press **Run Bake** again. For the same reason, do not use it with static batching, dynamic batching, GPU Resident Drawer, or submeshes with a base vertex offset ([Installation](./Installation#after-installing)).
- **Create Softbody Material changes the material remap of the model.** A remap changes one original material name across the whole model, so other slots or other renderers (unbaked clothes or hair) that use the same original material also receive the Softbody material. The bake warns in that case. Split the materials in a DCC tool, or apply the Softbody material in the prefab ([Materials](./Materials#let-the-bake-make-the-material)).
- **The bake cannot be undone with Undo.** **Cancel** stops without changing anything. Only one bake runs at a time, and starting Play cancels it. Keep the mesh in source control, and bake again to revert.

## Materials {#materials}

- **A Softbody material made with Shader Graph cannot be used with GPU Resident Drawer.** Set **GPU Resident Drawer** to Disabled in the URP asset. Softbody/Lit and hand-written shaders are not affected.
- **In Shader Graph, the motion vectors of the jiggle of fast-moving bones are slightly wrong.** It can show with TAA or motion blur. Softbody/Lit does not have this. See [Shader Graph](./Materials#shader-graph).

## Platform {#platform}

- **Verified only on Unity 6.3 (6000.3.26f1), URP 17.3, and D3D12, D3D11 and Vulkan on Windows.** Metal is not verified. It does not run on the Built-in Render Pipeline or HDRP. On a graphics API without compute shaders, contact is turned off on every Softbody component and only the jiggle runs.
- **The assemblies were compiled with Unity 6000.3.26f1 and URP 17.3.0.** If that API changes in another Unity or URP version, the package cannot be recompiled and cannot be used.
- **On D3D11, the first compile of the contact shaders takes about 15 minutes (on a fast PC).** On D3D12 and Vulkan it takes a few seconds. After compiling once it stays in the Library cache. Opening the editor with D3D12 is recommended.

## By design, not a defect {#by-design-not-a-defect}

- **Contact runs only in Play mode.** Dents are not visible in the Scene view in edit mode.
- **The material copy remains when the component is turned off.** This avoids creating it again when turned back on. While off, the mesh returns to its modeled shape, and the vertex shader reads one paint texture and skips the rest. The original material returns when the component is removed or Play ends.
- **The rise reaches only a few flesh thicknesses from the edge of the dent.** It stops at the collider and does not rise higher than the flesh thickness.
- **The edge of a dent holds better the deeper and steeper the press.** The friction limit at the edge is **Drag Friction** times the pressing strength of the hand, same as at the center, so the edge of a shallow, flat dent slips.
