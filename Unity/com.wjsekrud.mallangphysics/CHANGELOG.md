# Changelog

## [0.1.0-beta.1] - 2026-11-07

First public beta. Compiled assemblies without C# source; shaders are source.

- Bake: Softbody Authoring Data, Run Bake, Validate, paint masks, hard surface pieces, carrier and contact map.
- Runtime: Contact Detail Basic (dents, recovery, memory time, skin tension, lump shifting) and Full (rise around a dent, skin drag, Edge Smoothing, Lump Grip), hard surface plates (Flex paint, Flex Offset), Separated Pairs, inertia with groups. Full does not run on Metal.
- Shape fields: Softbody Shape Field on hands and props presses with the baked shape.
- Softbody/Lit and Softbody/Lit (Tessellated) shaders and Shader Graph include files.
- Project Settings > Softbody and the Softbody Debugger.
- Quick Start and Shader Graph samples.
- Not in this beta: self contact, volume mesh, automatic seams, skin grabbing (Softbody Grabber).
