# Known Limitations

What does not work in this release.

## Contact {#contact}

- **Two Softbody meshes that both press each other open a gap** as wide as their overlap. Only one can press: turn off collision on the mesh that should only be dented (a mesh with collision off is still dented), or tag it `SoftbodyIgnore`. [Colliders](Colliders)
- **Complex (triangle mesh) collision does not press.** Only simple collision shapes and shape fields do.
- **A mesh resting on thin collision can sink toward that collision.** This happens when the mesh thickness plus the thickness of the collision shape under it is less than 1.3 times **Max Contact Depth** (19.5 cm at the default 15 cm). You can make the floor collision thicker, make the floor not block the **Collider Channel**, or add the `SoftbodyIgnore` tag to the floor.
- **Drag is off on meshes longer than about 10 m** (over the drag grid's 1024 cells per side). Dents and bulges still work.
- **Very slow drag is lost to rounding.** At 60 contact updates per second, a hand moving slower than 0.03 mm/s with the skin 1 mm behind it, or 0.29 mm/s at 10 mm behind, does not move the skin.
- **A very fast hand drags the cells it has just reached a little less.** It starts above about 1.3 cm of movement per contact update: a fingertip at 50 cm/s drags about 0.2 mm less at 20 updates per second, and only 0.01 mm less at 60 (0.8 cm per update).
- **Wide dents spread slightly less on meshes that update rarely** (small on screen, or held back by **Limit Contact Update Rate**).
- **A lump with Lump Grip on can keep trembling after a very fast sweep of an open hand.** Seen only in a simulation at ten times the measured flesh stiffness. **Lump Grip** can be lowered, or the finger gaps in the hand's collision shapes smoothed.
- **Skin Tension is a stretched skin, not a rigid shell.** A stiff painted patch (the pips of a soft die) sinks level as a whole, but it does not tilt like a plate when pressed off-center. There is no hard crust either, such as crème brûlée or a plastic inlay.
- **Flex Offset is not finished yet and is unstable.**
- **A thin band floating over the body (belt, buckle, strap) can let a finger pass through.** The band and the skin under it are treated as one line of flesh, so pressing deeper than the band is thick only dents the band's front face and the finger slips behind it. With Contact Detail Full, the area around the band can also sink deeper than the finger.
- **A finger pressed in deep and dragged sideways for a long time can stay buried under the skin.** It happens only when the whole finger is deeper than the original skin.
- **Two runs of the same scene are not bit-identical.** The difference is 0.00001 to 0.001 cm, not visible, but a few cells can flip between sticking and slipping. Replays that need bit-exact results will not match.

## Bake {#bake}

- **Full contact detail is on LOD 0 only.** LOD 1 and up read the Softbody data of the nearest LOD 0 vertex, so their dents are coarser. A LOD that already uses the contact map UV channel is left unchanged, with a warning. Nanite meshes have one LOD.
- **Reimporting a baked mesh removes the UV channels the bake added.** The log says "Paint: the mesh no longer has the baked UV channel N. Press Run Bake again." After a reimport, you must press **Run Bake**.
- **Meshes that share one parent material can break each other's dents.** The bake refuses to change a shared material's UV channel when another Softbody mesh reads a different one, but it finds those meshes through the asset registry, so it can miss meshes not scanned yet or in another project. Each Softbody mesh must have its own material, which **Create Softbody Material** provides.
- **Undo after a bake is untested.** Baking again can replace undo.

## Nanite tessellation {#nanite-tessellation}

- **A tessellated mesh shows holes when the camera is very close to it.** The engine's buffer for tessellation patches (2 million by default) overflows and drops patches. The engine assumes the mesh can move by **Nanite Max World Position Offset** and culls fewer off-screen patches, so a larger value makes it more likely. You can lower that value and press **Reapply Nanite Tessellation**, raise the near clip plane (Project Settings, **Near Clip Plane**), or raise `r.Nanite.MaxCandidatePatches` and `r.Nanite.MaxVisiblePatches` under `[SystemSettings]` in DefaultEngine.ini.

## Platform {#platform}

- **Windows only, Unreal Engine 5.6, 5.7 and 5.8. Most visual checks were done on 5.8 with D3D12 (SM6) and D3D11 (SM5).** Contact needs Shader Model 5; below it (mobile, ES3.1) contact is off for every Softbody component. The mobile preview rendering level is untested. Vulkan compiles but has not been run.
- **The GitHub version (prebuilt, without source) does not build in the DebugGame configuration.** Development and Shipping build and package. The Fab version, which includes the source, does not have this limit. [Installation](Installation#github)

## By design, not a defect {#by-design-not-a-defect}

- **The bulge reaches only a few flesh thicknesses from the edge of a dent.** It stops at the collider and never rises higher than the flesh is thick.
- **The edge of a pressed area sticks better the deeper and steeper the press.** The edge has the same friction limit as the middle, **Drag Friction** times how hard the hand presses, so the edge of a shallow, flat press slips.
- **A deactivated component leaves the Softbody nodes in the material.** The vertex shader reads one paint texture, finds both switches at 0 and skips the rest. This lives in the material, so turning off one component cannot remove it. Nanite tessellation set up by the bake also stays.
- **A pressed mesh that stops moving stops updating.** While colliders, bones and settings move less than 0.1 cm (`softbody.Surface.StillDistance`) and the last update moved nothing by more than 0.001 cm, updates are skipped. Smaller movements add up and show once they pass that. An animated character keeps updating as long as its bones move.
