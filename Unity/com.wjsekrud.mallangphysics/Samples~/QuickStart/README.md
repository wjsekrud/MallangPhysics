# Quick Start

A plain cushion mesh (60 × 40 × 8 cm, origin at the middle of the bottom, one material slot named `Soft`) to try MallangPhysics with.
Import this sample from the Package Manager (MallangPhysics > Samples > Quick Start); Unity copies it to `Assets/Samples/MallangPhysics/<version>/Quick Start/`.

What is in it:

- `SM_SampleCushion.fbx` — the cushion, imported with Unity's default model settings. Its mesh has Z up (it was made in Blender), so the import puts a −90° X rotation on the model.
- `SampleCushion.prefab` — the cushion with a Box Collider from the bottom to 2 cm below the top, so a falling ball sinks 2 cm into it and the dent shows at once.
- `QuickStart.unity` — the cushion on a floor, a ball with a Rigidbody above it, a light and a camera.

Steps:

1. Create a bake asset: select `SM_SampleCushion.fbx` in the Project window and use **Assets > Softbody > Create Authoring Data**. It creates `SA_SM_SampleCushion` next to the model.
2. Check that its **Target Mesh** is the cushion: the `SM_SampleCushion` renderer inside `SM_SampleCushion.fbx`. (If you made the bake asset with Assets > Create > Softbody > Softbody Authoring Data instead, drag `SM_SampleCushion.fbx` from the Project window onto **Target Mesh**.)
3. Set **Core Placement** (3. Rigid Core (Bones)) to **Bottom**. Bottom holds only the bottom of the cushion, which rests on the floor; the rest is flesh.
4. Turn on **Create Softbody Material** and set **Softbody Material Slots** to `Soft`.
5. Click **Run Bake** (about 15 seconds), then click **Validate**: it should report no errors. The bake writes `SD_SM_SampleCushion` (the **Baked Data**) and the materials `M_SM_SampleCushion_Softbody` and `MI_SM_SampleCushion_Soft` next to the bake asset. The cushion has no textures, so its material is plain white.
6. Open `QuickStart.unity`, add the **Softbody** component to `SampleCushion`, and set its **Mesh Data** to the bake asset's **Baked Data**.
7. Press Play: the ball falls and dents the cushion. Any collider presses it the same way.
