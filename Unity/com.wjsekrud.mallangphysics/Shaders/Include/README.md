# Softbody shader functions

The Softbody component moves vertices and bends normals from a material, not by changing the mesh.
**Softbody/Lit** (URP Lit plus the Softbody functions) does this out of the box. Use the files in this folder when your own shader should
dent and jiggle the same way: a hand-written URP shader, or a Shader Graph.
(The rise around a press, skin drag and hard-surface plates come with Contact Detail Full; these functions draw them too.)

| File | What it is |
|---|---|
| `SoftbodyMaterialInput.hlsl` | The Softbody property declarations for a hand-written shader: `SOFTBODY_PER_MATERIAL_PROPERTIES` (put it inside your `UnityPerMaterial` buffer) and the texture declarations. |
| `SoftbodyMaterial.hlsl` | The functions: vertex move, vertex shading values, pixel normal, base color. Needs URP `Core.hlsl` and the property declarations first. |
| `SoftbodyURP.hlsl` | URP helpers on top of `SoftbodyMaterial.hlsl`: `SoftbodyGetVertexPositionInputs`, interpolator macros. |
| `SoftbodyShaderGraph.hlsl` | Shader Graph Custom Function entry points (`SoftbodyVertex`, `SoftbodyMotionVector`, `SoftbodyPixel`, `SoftbodyBaseColor`). |

The property names must match exactly (`SoftbodyShaderIds.Names` in the runtime assembly): the Softbody component looks them up by name on its
copy of the material, and the bake and the component recognize a Softbody material by its textures `_SoftbodyContactData`,
`_SoftbodyInertiaResponseTex`, `_SoftbodySurfaceDispTex` and `_SoftbodyCarrierUVTex`.

Rules every Softbody shader follows:

1. Every pass that outputs a position (forward, deferred, shadow caster, depth, depth normals, motion vectors, scene selection) moves the vertex
   with the same function. Pick one of `SoftbodyDisplacedPositionOS` and `SoftbodyDisplacedPositionWS` and use that one in every pass
   (Softbody/Lit and the Shader Graph functions use `SoftbodyDisplacedPositionOS`; `SoftbodyGetVertexPositionInputs` calls it too). The two give
   the same position through a different order of operations, so mixing them can change the depth by a bit: pixels then fail the depth
   prepass (ZTest Equal) and the motion vector pass. Moving every pass the same way keeps shadows, the depth prepass and motion vectors on the
   drawn surface. The Meta pass (lightmap baking) does not move vertices.
2. The per-vertex tables are read in the vertex shader only. Pass the results to the pixel shader through interpolators.
3. Normals are not replaced: the pixel shader rotates your normal (normal map included) with `SoftbodyPixelNormalWS`.
4. The dent darkening multiplies the base color (`SoftbodyBaseColor`), not ambient occlusion — ambient occlusion only affects indirect light.
5. No `DOTS_INSTANCING_ON` variant: the GPU Resident Drawer must not take these renderers (it renumbers vertices). Do not use static or
   dynamic batching on Softbody renderers either. Shader Graph cannot follow this rule — see "GPU Resident Drawer" under Shader Graph.

## Hand-written URP shader

Properties block (copy into your shader; the defaults matter — the textures start black, the paint white):

```
_SoftbodySkirtStrength("Contact Strength", Float) = 1.0
[HideInInspector] _SoftbodySurfaceBetweenWeight("Contact Shading Inside Triangles", Float) = 1.0
[HideInInspector] _SoftbodySurfaceBetweenEdgeTexels("Contact Shading Edge Texels", Float) = 2.0
[HideInInspector][NoScaleOffset] _SoftbodyContactData("Softbody Inertia Bridge", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyPrevContactData("Softbody Previous Inertia Bridge", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyAttr2Tex("Softbody Inertia Groups", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyAttrDbgTex("Softbody Debug Values", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyPaintTex("Softbody Paint", 2D) = "white" {}
[HideInInspector][NoScaleOffset] _SoftbodyInertiaResponseTex("Softbody Flesh Softness", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyCarrierUVTex("Softbody Contact Map Points", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyCarrierVertexUVTex("Softbody Contact Map UVs", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodySurfaceDispTex("Softbody Contact Displacement", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodyPrevSurfaceDispTex("Softbody Previous Contact Displacement", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodySurfaceNormalTex("Softbody Contact Normals", 2D) = "black" {}
[HideInInspector][NoScaleOffset] _SoftbodySurfaceLumpNormalTex("Softbody Lump Normals", 2D) = "black" {}
[HideInInspector] _SoftbodyInertiaResponseCount("Softbody Softness Rows", Float) = 0.0
[HideInInspector] _SoftbodySurfaceFieldOn("Softbody Contact On", Float) = 0.0
[HideInInspector] _SoftbodySurfaceAttachRange("Softbody Follow Range", Float) = 10.0
[HideInInspector] _SoftbodyMaxDisplacement("Softbody Max Displacement", Float) = 150.0
[HideInInspector] _SoftbodyVisWeight("Softbody Debug View", Float) = 0.0
[HideInInspector] _SoftbodyVisMode("Softbody Debug View Mode", Float) = 0.0
[HideInInspector] _SoftbodyL2WX("Softbody Frame Row X", Vector) = (1, 0, 0, 0)
[HideInInspector] _SoftbodyL2WY("Softbody Frame Row Y", Vector) = (0, 1, 0, 0)
[HideInInspector] _SoftbodyL2WZ("Softbody Frame Row Z", Vector) = (0, 0, 1, 0)
[HideInInspector] _SoftbodyPrevL2WX("Softbody Previous Frame Row X", Vector) = (1, 0, 0, 0)
[HideInInspector] _SoftbodyPrevL2WY("Softbody Previous Frame Row Y", Vector) = (0, 1, 0, 0)
[HideInInspector] _SoftbodyPrevL2WZ("Softbody Previous Frame Row Z", Vector) = (0, 0, 1, 0)
[HideInInspector] _SoftbodyTextureSize("Softbody Table Size", Float) = 0.0
[HideInInspector] _SoftbodyOriginWS("Softbody Origin", Vector) = (0, 0, 0, 0)
[HideInInspector] _SoftbodyPrevOriginWS("Softbody Previous Origin", Vector) = (0, 0, 0, 0)
[HideInInspector] _SoftbodyCarrierVertexUVOn("Softbody Contact Map UVs On", Float) = 0.0
```

HLSL, in every pass (the same buffer layout in every pass keeps the SRP Batcher working):

```hlsl
#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
#include "Packages/com.wjsekrud.mallangphysics/Shaders/Include/SoftbodyMaterialInput.hlsl"

CBUFFER_START(UnityPerMaterial)
    float4 _BaseColor;              // your own properties
    SOFTBODY_PER_MATERIAL_PROPERTIES
CBUFFER_END

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Include/SoftbodyURP.hlsl"

struct Attributes
{
    float4 positionOS : POSITION;
    float3 normalOS   : NORMAL;
    uint   vertexID   : SV_VertexID;    // the per-vertex tables are addressed by vertex number
};

struct Varyings
{
    float4 positionCS : SV_POSITION;
    float3 normalWS   : TEXCOORD0;
    SOFTBODY_VARYINGS(1, 2, 3, 4)       // four interpolators; SOFTBODY_VARYINGS_NORMAL(…) for passes without color
};

Varyings vert(Attributes input)
{
    Varyings output;
    // Shading values from the unmoved vertex, then the moved position.
    SoftbodyVertexShadingData softbody = SoftbodyVertexShadingOS(input.vertexID, input.positionOS.xyz, input.normalOS);
    VertexPositionInputs position = SoftbodyGetVertexPositionInputs(input.positionOS.xyz, input.vertexID);
    output.positionCS = position.positionCS;
    output.normalWS = TransformObjectToWorldNormal(input.normalOS);   // the vertex normal is not changed
    SOFTBODY_TRANSFER_VARYINGS(softbody, output)
    return output;
}

half4 frag(Varyings input) : SV_Target
{
    SoftbodyVertexShadingData softbody = SOFTBODY_LOAD_VARYINGS(input);
    float occlusion;
    float3 baseNormalWS = input.normalWS;   // or your normal-mapped normal
    float3 normalWS = SoftbodyPixelNormalWS(softbody, input.normalWS, baseNormalWS, occlusion);
    float3 albedo = SoftbodyBaseColor(_BaseColor.rgb, occlusion, softbody.visColor);
    // … light with normalWS and albedo
}
```

Depth-only and shadow passes only need `SoftbodyDisplacedPositionOS(vertexID, positionOS)` in place of the position. A motion vector pass computes
the previous position as `previousPositionWS + SoftbodyPreviousDisplacementWS(vertexID, previousPositionWS)`, where `previousPositionWS` is last
frame's unmoved world position (see `Shaders/Lit/SoftbodyObjectMotionVectors.hlsl`). Give the SubShader the tag
`"AlwaysRenderMotionVectors" = "true"` so motion vectors are drawn while the flesh jiggles on a renderer that does not move.

The textures are read with the inline samplers `sampler_PointClamp` (tables) and `sampler_LinearClamp` (contact maps); the shader does not
declare samplers of its own.

## Shader Graph

**GPU Resident Drawer**: the URP Shader Graph targets add a `DOTS_INSTANCING_ON` variant to every pass, so the GPU Resident Drawer takes a
renderer with a Shader Graph Softbody material and draws it with renumbered vertices — the Softbody tables then read the wrong vertices.
Use Shader Graph Softbody materials only with **GPU Resident Drawer** set to **Disabled** in the URP asset (Rendering). Softbody/Lit and
hand-written shaders have no such variant and work either way.

**Precision**: keep the graph at **Single** (Graph Settings → Precision). `SoftbodyVertex`, `SoftbodyMotionVector` and `SoftbodyPixel` exist
only at Single precision: half precision would cut the vertex number (wrong table cell above vertex 2048) and the positions before the
function sees them. If the rest of the graph must stay Half, set the **Precision** of each of these Custom Function nodes to **Single**;
a node left at Half stops the graph with "undeclared identifier 'SoftbodyVertex_half'" (or `SoftbodyPixel_half`,
`SoftbodyMotionVector_half`). `SoftbodyBaseColor` also works at Half.

1. **Blackboard**: add a property for every name in the list above with the same **Reference** (for example `_SoftbodyContactData`), turn
   **Exposed** on, and keep the defaults (textures Black, `_SoftbodyPaintTex` White; floats and vectors as listed). Use Texture2D for the
   textures, Float for the numbers and Vector4 for the vectors. Set their precision to Single. The functions read these properties by name,
   so a missing one stops the graph from compiling with "undeclared identifier".
2. **Vertex**: add a Custom Function node, Type File, Source `Packages/com.wjsekrud.mallangphysics/Shaders/Include/SoftbodyShaderGraph.hlsl`,
   Name `SoftbodyVertex`, Precision Single (or Inherit in a Single graph). Inputs: `VertexID` (Float, from the Vertex ID node),
   `PositionOS` (Vector3, Position node in Object space), `NormalOS` (Vector3, Normal Vector node in Object space).
   Outputs: `PositionOSOut` (Vector3) → Vertex block Position; `Carrier` (Vector4), `Shading` (Vector4), `InertiaTilt` (Vector3),
   `VisColor` (Vector3) → four Custom Interpolators in the Vertex block.
3. **Fragment**: Custom Function `SoftbodyPixel` (Precision Single) with inputs `Carrier` (Vector4), `Shading` (Vector4), `InertiaTilt`
   (Vector3) from the interpolators, `VertexNormalWS` (Vector3, Normal Vector node in World space) and `BaseNormalWS` (Vector3, your
   normal map transformed to World space, or the same World normal). Outputs `NormalWS` (Vector3) → Fragment block Normal, with the
   graph's **Fragment Normal Space** set to **World**, and `Occlusion` (Float).
4. **Base color**: Custom Function `SoftbodyBaseColor` with inputs `BaseColor` (Vector3, your color), `Occlusion` (Float, from step 3) and
   `VisColor` (Vector3, interpolator). Output `BaseColorOut` (Vector3) → Fragment block Base Color.
5. **Motion vectors** (TAA, STP, motion blur): in the Graph Settings set **Additional Motion Vectors** to **Custom** and connect a Custom
   Function `SoftbodyMotionVector` (Precision Single; inputs `VertexID`, `PositionOS`; output `MotionVectorOS` Vector3) to the Vertex block
   Motion Vector. Shader Graph has no previous vertex position in the vertex stage, so the previous jiggle is evaluated at the current
   position relative to the current renderer position — a fast turning bone gives slightly wrong jiggle motion vectors (moving the whole
   renderer does not). Softbody/Lit does not have this approximation.

A Shader Graph sub graph with these nodes already wired is planned for a later version.
