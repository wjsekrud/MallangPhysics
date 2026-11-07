# Inertia

How to tune the jiggle. The settings are in the **Inertia** fields of the Softbody component; defaults and ranges are in [Softbody Component](./Softbody-Component). The jiggle runs only during Play.

## Where the flesh moves {#where-the-flesh-moves}

How much each part moves is set in the bake. Soft flesh far from the bones (white in the **Jiggle** paint) moves the most; firm areas, areas close to a bone and areas painted dark move less, and the rigid core does not move. Belly or lump flesh swings as one piece and fades out smoothly toward its base.

Play starts at rest. In the first frame the flesh takes the sag of the current pose as it is, without falling into it, so a character that starts lying down does not pop. Switching the component off and on also restarts from rest.

These values in Softbody Authoring Data decide the jiggle before the component settings do.

- **Bone Thickness** (3. Rigid Core (Bones)): the larger it is, the thinner the flesh and the less it jiggles. It can be set per bone in **Bone Overrides**.
- **Jiggle** paint (4. Paint, G channel of the Slot Masks texture): the firmness of the flesh. Paint firm flesh, such as a belly over muscle, gray so that it jiggles less. Black is flesh as firm as bone: it does not jiggle but still dents. The bone itself is painted in **Depth** (R channel) ([Painting](./Painting)).
- **Flesh Reach** (3. Rigid Core (Bones)): makes thick flesh jiggle by the paint instead of by its thickness. 3 (the default) for a human-size body.
- **Physics Driven Bones** (4. Paint): bones that another system already moves, such as spring bones, cloth or physics components. Flesh skinned to these bones and the bones below them gets no Softbody jiggle, so the same flesh is not shaken twice.

Paint and Flesh Reach are applied by **Run Bake**, Physics Driven Bones by **Reapply Paint**.

## Inertia groups {#inertia-groups}

An inertia group is one bone that drives flesh that jiggles together. The bake picks the inertia groups of a skeletal mesh (Skinned Mesh Renderer) from the skin weights, up to **Max Inertia Groups** (2. Bake, default 16, maximum also 16).

1. The root bone (the top bone of the skeleton) is always group 0.
2. Next is the top bone of each chain whose **Role** is set to **Lump** in Bone Overrides. It gets a group whatever its vertex count, and the lump bones below it join this group. These are the lump groups. A bone set to **Accessory** does not get its own group, and its vertices follow the group above.
3. The remaining groups go to the bones with the most vertices.
4. The other bones walk up their parent chain and join the group of the first group bone they meet.

When the bake finishes, the Console shows the result (`[Softbody] N inertia groups (in parentheses: distance from the bone to its farthest vertex): ...`), and each lump group is followed by `lump`. When the groups are full and a lump bone gets none, the warning `Inertia groups: all 16 groups are taken, so lump bone ... gets no group of its own. Raise Max Inertia Groups.` appears. The number of groups is also shown in the **Setup** box at the top of the component inspector (`Inertia: on · 16 groups`).

## Tuning {#tuning}

**Inertia Amount** is the distance (cm) that thick flesh painted white in Jiggle sags at 1 g, so the same value means the same flesh on any character. Each part swings at a speed that matches its own sag, so the larger Inertia Amount is, the bigger and slower the swing. Pick this value first by how soft the flesh is, then refine separately. The speed of each part is set in the bake. It takes the median of the sag of the flesh skinned to that part, weighted by skin weight, so a part with a lot of firm flesh mixed in still swings at the speed that suits the jiggling flesh. After changing paint or bone settings, press Run Bake or **Reapply Paint** again to change the speed.

- Rhythm only: **Jiggle Speed**. Keeps the size and makes the swing faster or slower.
- Size only: **Actor Motion Weight** and **Animation Motion Weight**. Make it jiggle bigger at the same speed. Actor Motion Weight is the share from the object moving and turning; Animation Motion Weight is the share from animation moving the bones.
- Number of bounces: **Inertia Damping**. At 0.25 it bounces about twice, at 0.4 about once, then stops.
- Crisper: raise Jiggle Speed, raise Inertia Damping toward 0.4, and set **Inertia Firmness** around 1 so the flesh does not stretch while running.
- Softer: raise Inertia Amount (slower and bigger) and keep Inertia Damping low.
- Only lumps follow the body: parts attached to the skeleton do not sag, and **Gravity Scale** affects only lumps and static meshes. Lower a part that should not move with an override or the **Jiggle** paint.
- Make one part sag: give that part a Gravity Scale in **Inertia Group Overrides**.
- Bounce more and sway less: in **Inertia Direction Scale**, raise the up-down axis and lower the side axes. The axes are the modeled mesh's local X, Y, Z, and which axis is up depends on how the model was imported. **Rest Down** on the Mesh Data asset is the mesh's down direction.
- If the flesh jumps on a respawn or a cut: a move of [**Teleport Distance**](./Softbody-Component#teleport-distance) (default 50 cm) or more beyond what the current speed allows in one frame counts as a teleport and does not shake the flesh. For a shorter teleport, call `NotifyTeleported()` when moving.

### One body part {#one-body-part}

Add an entry to **Inertia Group Overrides** and choose a **Bone**. The dropdown shows only the groups the bake found. A bone not in the list belongs to another group (see the bake log), or you need to raise **Max Inertia Groups** and bake again. If the same bone is added twice, only the first entry is used.

**Amount Scale** is always applied as a multiplier on Inertia Amount; at 0 that part does not jiggle. **Damping** and **Gravity Scale** are used only when the checkbox in front is on. Use it for a character whose lumps should swing big and slow and whose belly small and fast.

## Lumps {#lumps}

**Lump Yield**, **Lump Grip** and **Squeeze Stiffening** (Inertia > Lumps) set how much a pressing hand pushes a lump. On a skeletal mesh, set the bone's Role to **Lump** and bake; **Enable Contact** and **Enable Inertia** must be on, and Inertia Amount must be above 0. When the conditions are not met, the fields are dimmed. A static mesh (Mesh Renderer) has no lumps.

- If the lump moves too much under a deep press: lower Lump Yield rather than Squeeze Stiffening.
- **Lump Grip** drags the lump with the skin the hand holds, so it works only when **Contact Detail** is Full and **Drag Friction** is above 0. If the lump keeps trembling after an open hand sweeps past very fast, lower Lump Grip.

## When nothing jiggles {#when-nothing-jiggles}

- If the **Setup** box in the component inspector shows `Inertia: off (...)`, the reason is in the parentheses. For `the bake has no jiggle data`, rebuild Mesh Data with **Run Bake**.
- If a lump does not move as a whole and the bake log shows `Lumps: skipped, no bone has the Lump role in Bone Overrides` or `Lumps: skipped, the baked data has no flesh softness for inertia`: set the bone to **Lump** and press **Run Bake**.
- `Inertia groups: the mesh has no skin weights, so no inertia groups are made.`: inertia groups are picked from skin weights, so a skeletal mesh without skin weights does not jiggle at all.
- If Play start shows `N of M inertia group bones were not found on the mesh's skeleton; those groups will not jiggle.`: the bones of the baked groups were not found in the renderer's hierarchy. If you turned on **Optimize Game Objects** in the model import settings, put those bones in **Extra Transforms to Expose**; if the skeleton changed, press Run Bake again.
- If the flesh of a spring bone or a physics-driven bone does not jiggle: that is **Physics Driven Bones**. If Softbody should move it, remove the bone from the list and press **Reapply Paint**.

For other messages, see [Troubleshooting](./Troubleshooting).
