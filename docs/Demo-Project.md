# Demo Project

The demo is the `SoftbodyDemo` project, downloaded separately from the plugin. The [GitHub releases](https://github.com/wjsekrud/MallangPhysics/releases) have one zip per engine version (`MallangPhysicsDemo-1.0.0-UE5.6.zip`, `UE5.7`, `UE5.8`) with the same content. It is a Blueprint project, so it opens without Visual Studio. It shows props and a character, one scene per feature.

## What is in it {#what-is-in-it}

- `Plugins/SoftbodySystem`: the prebuilt plugin for that engine version (no C++ source). The body and props are all baked.
- `Content/Demo`: the demo level, the props, the body (`SK_Body`), the hand (`SK_GraspHand`), the character that sits on the sofa (`SK_Sitter`), and the Blueprints (`BP_SoftbodyDemo*`).
- `Content/ContactLab`, `Content/Profiling`: levels for comparison and measurement.

## Levels {#maps}

| Level | What it shows |
|---|---|
| `/Game/Demo/L_SoftbodyDemo` | The main demo. Each cell of a grid wall holds one scene, with its title and description written on the cell's back wall. |
| `/Game/ContactLab/L_ContactMaterials` | A comparison of 12 materials pressed to the same depth: Skin Tension, Flex, Volume Preservation, recovery speed, foam and memory foam. |
| `/Game/ContactLab/L_ContactCube` | Eight combinations of Skin Tension, Flex and Volume Preservation, each set to 0 or 1. |
| `/Game/Profiling/L_SoftbodyProfile` | The same scenes as the main demo, which you can switch on and measure one cell at a time. Use it with the console commands in [Performance](Performance). |

The cells of the main demo are:

| Cell | Scene |
|---|---|
| 1.1 | Vertex spacing: mesh density comparison |
| 2.1 to 2.5 | Volume Preservation, recovery speed, a long-held dent, two materials on one mesh, Skin Tension |
| 3.1, 3.2 | Squeezing from both sides, sharp corners |
| 4.1 to 4.4 | Gripping with a hand, rubbing with a hand, sitting on a sofa, a fast impact |
| 5.1, 6.1, 6.2 | Small props |
| 7.1, 7.2 | A rigid ball on foam, soft pieces pressing each other |
| 8.1 | Skeletal mesh: its own hands press its body |
| 9.1, 9.2 | Thin sheets: a spinning plate, an unfolded sheet |

## Opening it {#open}

Open `SoftbodyDemo.uproject` with the engine version in the zip name, pick a level and press Play. To view one cell at a time, use `Softbody.Scenario <cell>` in `L_SoftbodyProfile`.

## Reusing it {#reuse}

The demo's props and body are baked the same way as in [Quick Start](Quick-Start). Open a prop's `SA_*` authoring asset to see the values it uses.
