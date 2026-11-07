# Credits and License

## License {#license}

- The plugin (C++ source, shaders and plugin content) and the prebuilt version: [MIT License](https://github.com/wjsekrud/MallangPhysics/blob/main/LICENSE). Copyright © 2026 wjsekrud.
- The demo project and this documentation: Copyright © 2026 wjsekrud. All Rights Reserved. The plugin inside the demo is under the MIT License.

The demo project is on the GitHub release, one per engine version. Content by other creators in the demo follows the licenses listed under [Content in the demo project](#content-in-the-demo-project).

## Content in the demo project {#content-in-the-demo-project}

- Props: built from scratch with Blender scripts by the author. No downloaded models or textures.
- Body mesh (`SK_Body`): made by the author.
- Hand (`SK_GraspHand`): the hand taken from the author's character model, rigged and uniformly scaled up by 1.116. No files from outside sources are mixed in.
- The character that sits on the sofa in 4.3 (`SK_Sitter`): Quaternius' Universal Base Characters (head and hair) combined with Modular Character Outfits - Fantasy (the Peasant outfit), CC0.
- The character's sitting motion (`A_Sitter_Sit`) and the body's walk and run animations: Quaternius, Universal Animation Library (CC0).
- Sitting animation (`body_sit.fbx`): made by the author on the body's skeleton.
- Sample cushion (`/SoftbodySystem/Samples/SM_SampleCushion`): built by the author's script, shipped inside the plugin.

## Documentation photos {#documentation-photos}

Some photos in the documentation show the character Selestia (©ジンゴ) wearing clothes. Selestia is not included in the demo project.

## References {#references}

- [KawaiiPhysics](https://github.com/pafuhana1213/KawaiiPhysics) (pafuhana1213, MIT License): used in development and testing. Not included in the plugin or the demo project.

## Engine features the plugin builds on {#engine-features-the-plugin-builds-on}

- Unreal Engine plugins enabled by the plugin descriptor: Tet Meshing, Geometry Processing (Epic Games).
- Automatic contact map seams use the engine's XAtlas integration.

## Documentation site {#documentation-site}

- Font for the Korean pages: Pretendard (Kil Hyung-jin, SIL Open Font License 1.1).
