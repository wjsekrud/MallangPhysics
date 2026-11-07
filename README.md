# MallangPhysics

[한국어](README.ko.md)

Soft flesh for skeletal and static meshes in Unreal Engine 5.6, 5.7 and 5.8 on Windows. Hands and objects dent the surface, the flesh around the dent bulges, a moving hand drags the skin along, and the flesh jiggles when the body moves. You bake a mesh once in the editor and add one component; the mesh deforms on the GPU.

![A hand pressing into a soft cushion in the demo project](docs/images/Home/hero.png)

**Documentation:** https://wjsekrud.github.io/MallangPhysics/ (English, 한국어)

## Downloads

| | Where | Contents |
|---|---|---|
| Free build | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysics-1.0.0-UE5.x-Win64.zip` | The plugin prebuilt for one engine version, with shaders and public headers, without C++ source. Builds and packages in Development and Shipping. DebugGame does not build. |
| Source version | Fab: search for MallangPhysics | The plugin with full C++ source. |
| Demo project | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysicsDemo-1.0.0-UE5.x.zip` | Blueprint-only project with the free build inside. Opens in the matching engine version without Visual Studio. |
| Unity (beta) | [Unity documentation](https://wjsekrud.github.io/MallangPhysics/unity/) | Unity 6.3 with URP, without source. |

## Installing the free build

Unzip the `SoftbodySystem` folder from the zip for your engine version into your project's `Plugins` folder, then follow [Quick Start](https://wjsekrud.github.io/MallangPhysics/Quick-Start).

## License

- Fab version (with the C++ source): Fab Standard License.
- Free build, demo project, Unity beta and documentation: Copyright © 2026 wjsekrud. All Rights Reserved.

Content by other creators in the demo project and third-party code in the Unity package keep their own licenses: see [Credits and License](https://wjsekrud.github.io/MallangPhysics/Credits-and-License).

## Notes

- Code comments are in Korean, except the English overview at the top of each shader file. Numbers such as 일지 123 or ADR-0012 in them point to the author's design records, which are not published.
- Bug reports: [Issues](https://github.com/wjsekrud/MallangPhysics/issues).
