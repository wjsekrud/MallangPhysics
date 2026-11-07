# Shader Graph

`Softbody Lit Graph.shadergraph` is a URP Lit Shader Graph with the four Softbody Custom Function nodes already wired. Copy it and change the surface (textures, colors) the way you would in any Shader Graph. The vertex and normal wiring is what makes the mesh dent and jiggle.

What is in it:

- **Blackboard**: every Softbody property with the exact **Reference** the component looks for (`_SoftbodyContactData` and so on), Exposed, with the default values the functions expect. The only properties you set yourself are `Base Color` and `Smoothness`.
- **Vertex**: `SoftbodyVertex` moves the vertex (Vertex ID, Position and Normal Vector in Object space in, `PositionOSOut` to the Position block) and sends `Carrier`, `Shading`, `InertiaTilt` and `VisColor` through four Custom Interpolators. `SoftbodyMotionVector` feeds the Motion Vector block (Graph Settings > Additional Motion Vectors = Custom).
- **Fragment**: `SoftbodyPixel` rotates the normal (Fragment Normal Space = World), `SoftbodyBaseColor` multiplies in the dent darkening and the Debug View colors.

How to use it:

1. Set **GPU Resident Drawer** to Disabled in the URP asset. Shader Graph adds a `DOTS_INSTANCING_ON` variant to every pass, and the GPU Resident Drawer would draw the mesh with renumbered vertices.
2. Create a material from the graph (Assets > Create > Material, pick `Shader Graphs/Softbody Lit Graph`) and put it in the mesh's material slot.
3. On the Softbody Authoring Data, leave **Create Softbody Material** off, run the bake, then press **Rebake Contact Map** after the material is in the slot. The bake and the Softbody component recognize the slot by the four Softbody textures in the graph.

Known limit: Shader Graph does not know the previous frame's vertex positions, so the jiggle motion vectors of a fast-turning bone are slightly off (TAA and motion blur). Softbody/Lit does not have this.

The functions are in `Shaders/Include/SoftbodyShaderGraph.hlsl`; the package `Shaders/Include/README.md` explains each node and the rules.
