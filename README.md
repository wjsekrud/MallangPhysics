# MallangPhysics

[한국어](README.ko.md) · [日本語](README.ja.md)

An open-source soft body plugin for skeletal and static meshes in Unreal Engine 5.6, 5.7 and 5.8 on Windows (MIT License). Hands and objects dent the surface, the flesh around the dent bulges, a moving hand drags the skin along, and the flesh jiggles when the body moves. You bake a mesh once in the editor and add one component; the deformation runs on the GPU. A Unity 6.3 URP version is included as a beta.

[![A hand squeezing a soft ball and a hand pressing into a soft cushion](.github/media/hero.webp)](https://www.youtube.com/watch?v=n5HpN2XGicA)

**Video:** https://www.youtube.com/watch?v=n5HpN2XGicA

**Documentation:** https://wjsekrud.github.io/MallangPhysics/ (English, 한국어)

## Downloads

| | Where | Contents |
|---|---|---|
| Source | [`Unreal/SoftbodySystem`](Unreal/SoftbodySystem) in this repository | The plugin with the full C++ source. The same source builds on 5.6, 5.7 and 5.8. |
| Prebuilt plugin | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysics-1.0.0-UE5.x-Win64.zip` | The plugin prebuilt for one engine version, with shaders and public headers. Works in Blueprint-only projects. Builds and packages in Development and Shipping; DebugGame does not build. |
| Demo project | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysicsDemo-1.0.0-UE5.x.zip` | Blueprint-only project with the prebuilt plugin inside. Opens in the matching engine version without Visual Studio. |
| Unity (beta) | [`Unity/com.wjsekrud.mallangphysics`](Unity/com.wjsekrud.mallangphysics) in this repository | Unity 6.3 with URP. See the [Unity documentation](https://wjsekrud.github.io/MallangPhysics/unity/). |

## Installing

- Source: copy `Unreal/SoftbodySystem` into your project's `Plugins` folder and build the project.
- Prebuilt: unzip the `SoftbodySystem` folder from the zip for your engine version into your project's `Plugins` folder.
- Unity: in the Package Manager, add the git URL `https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics`.

Then follow [Quick Start](https://wjsekrud.github.io/MallangPhysics/Quick-Start).

## License

- The plugin source, the prebuilt plugin and the Unity package: [MIT License](LICENSE). Third-party code in the Unity package keeps its own license ([Third Party Notices](Unity/com.wjsekrud.mallangphysics/Third%20Party%20Notices.md)).
- The demo project and the documentation: Copyright © 2026 wjsekrud. All Rights Reserved. Content by other creators keeps its own license: see [Credits and License](https://wjsekrud.github.io/MallangPhysics/Credits-and-License).

## Notes

- The author may not be able to answer issues or pull requests for a long time from November 2026. Users can ask and answer each other in [Discussions](https://github.com/wjsekrud/MallangPhysics/discussions).
- Code comments are in Korean, except the English overview at the top of each shader file. Numbers such as 일지 123 or ADR-0012 in them point to the author's design records, which are not published.
- Bug reports: [Issues](https://github.com/wjsekrud/MallangPhysics/issues).
