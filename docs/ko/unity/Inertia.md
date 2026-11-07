# 관성

출렁임을 조절하는 방법입니다. 설정은 Softbody 컴포넌트의 **Inertia** 칸에 있고, 기본값과 범위는 [Softbody 컴포넌트](./Softbody-Component)에 있습니다. 출렁임은 Play 중에만 돕니다.

## 살이 움직이는 곳 {#where-the-flesh-moves}

부위마다 얼마나 움직일지는 베이크에서 정해집니다. 본에서 먼 무른 살(**Jiggle** 칠에서 흰 곳)이 가장 많이 움직이고, 단단한 곳, 본에 가까운 곳, 어둡게 칠한 곳은 덜 움직이며, 단단한 코어는 움직이지 않습니다. 배나 덩어리 살은 전체가 한 조각으로 흔들리고, 밑동 쪽으로 갈수록 부드럽게 잦아듭니다.

Play는 가만히 있는 상태에서 시작합니다. 첫 프레임에 살은 지금 포즈의 처짐을 떨어지는 과정 없이 그대로 받으므로, 누운 채 시작하는 캐릭터도 튀지 않습니다. 컴포넌트를 껐다 켜도 가만히 있는 상태에서 다시 시작합니다.

Softbody Authoring Data의 다음 값이 컴포넌트 설정보다 먼저 출렁임을 정합니다.

- **Bone Thickness**(3. Rigid Core (Bones)): 클수록 살이 얇아져 덜 출렁입니다. 본마다 **Bone Overrides**에서 정할 수 있습니다.
- **Jiggle** 칠(4. Paint, Slot Masks 텍스처의 G 채널): 살의 굳기입니다. 근육 위의 배처럼 단단한 살은 회색으로 칠해 덜 출렁이게 합니다. 검은색은 뼈만큼 단단한 살로, 출렁이지 않지만 눌리기는 합니다. 뼈 자체는 **Depth**(R 채널)에 칠합니다([페인트](./Painting)).
- **Flesh Reach**(3. Rigid Core (Bones)): 두꺼운 살이 두께가 아니라 칠대로 출렁이게 합니다. 사람 크기의 몸은 3(기본값)입니다.
- **Physics Driven Bones**(4. Paint): 스프링 본, 천, 물리 컴포넌트처럼 다른 시스템이 이미 움직이는 본입니다. 이 본과 그 아래 본에 스키닝된 살에는 Softbody 출렁임을 넣지 않아, 같은 살이 두 번 흔들리지 않습니다.

칠과 Flesh Reach는 **Run Bake**로, Physics Driven Bones는 **Reapply Paint**로 적용됩니다.

## 관성 그룹 {#inertia-groups}

관성 그룹은 함께 흔들리는 살을 끄는 본 하나입니다. 베이크는 스켈레탈 메시(Skinned Mesh Renderer)의 관성 그룹을 스킨 웨이트에서 고르며, **Max Inertia Groups**(2. Bake, 기본값 16, 최댓값도 16)까지 만듭니다.

1. 루트 본(뼈대 맨 위 본)은 늘 그룹 0입니다.
2. 그다음은 Bone Overrides에서 **Role**을 **Lump**로 둔 체인마다 맨 위 본입니다. 버텍스 수와 상관없이 그룹을 받고, 그 아래의 덩어리 본은 이 그룹에 들어갑니다. 이것이 덩어리 그룹입니다. **Accessory**로 둔 본은 제 그룹을 받지 않고, 그 버텍스는 위쪽 그룹을 따릅니다.
3. 남은 그룹은 버텍스를 가장 많이 가진 본에 돌아갑니다.
4. 나머지 본은 부모 체인을 따라 올라가며 처음 만나는 그룹 본의 그룹에 들어갑니다.

베이크가 끝나면 Console에 결과가 나오고(`[Softbody] N inertia groups (in parentheses: distance from the bone to its farthest vertex): ...`), 덩어리 그룹마다 뒤에 `lump`가 붙습니다. 그룹이 다 차서 덩어리 본이 그룹을 받지 못하면 `Inertia groups: all 16 groups are taken, so lump bone ... gets no group of its own. Raise Max Inertia Groups.` 경고가 나옵니다. 그룹 수는 컴포넌트 인스펙터 맨 위 **Setup** 상자(`Inertia: on · 16 groups`)에도 나옵니다.

## 조절 {#tuning}

**Inertia Amount**는 Jiggle을 흰색으로 칠한 두꺼운 살이 1 g에서 처지는 거리(cm)라서, 같은 값이면 어느 캐릭터에서나 같은 살입니다. 부위마다 제 처짐에 맞는 빠르기로 흔들리므로 Inertia Amount가 클수록 크고 느리게 흔들립니다. 먼저 살이 얼마나 무른지로 이 값을 고른 뒤 따로 다듬습니다. 부위마다의 빠르기는 베이크에서 정해집니다. 그 부위에 스키닝된 살의 처짐을 스킨 웨이트로 가중해 가운데 값을 기준으로 삼으므로, 단단한 살이 많이 섞인 부위도 출렁이는 살에 맞는 빠르기로 흔들립니다. 칠이나 본 설정을 바꾸면 Run Bake나 **Reapply Paint**를 다시 눌러야 빠르기가 바뀝니다.

- 리듬만: **Jiggle Speed**. 크기는 그대로 두고 더 빠르게, 느리게 흔들리게 합니다.
- 크기만: **Actor Motion Weight**와 **Animation Motion Weight**. 같은 빠르기에서 더 크게 출렁이게 합니다. Actor Motion Weight는 오브젝트가 움직이고 도는 몫, Animation Motion Weight는 애니메이션이 본을 움직이는 몫입니다.
- 튀는 횟수: **Inertia Damping**. 0.25면 두 번쯤, 0.4면 한 번쯤 튀고 멈춥니다.
- 더 또렷하게: Jiggle Speed를 올리고, Inertia Damping을 0.4 쪽으로 올리고, 달릴 때 살이 늘어나지 않도록 **Inertia Firmness**를 1 안팎으로 둡니다.
- 더 부드럽게: Inertia Amount를 올리고(더 느리고 크게) Inertia Damping은 낮게 둡니다.
- 몸을 따라 덩어리만 움직이게: 스켈레톤에 붙은 부위는 처지지 않고, **Gravity Scale**은 덩어리와 스태틱 메시에만 듣습니다. 움직이지 않았으면 하는 부위는 오버라이드나 **Jiggle** 칠로 낮춥니다.
- 한 부위를 처지게: **Inertia Group Overrides**에서 그 부위에 Gravity Scale을 줍니다.
- 더 튀고 덜 흔들리게: **Inertia Direction Scale**에서 위아래 축을 올리고 옆 축을 낮춥니다. 축은 모델링된 메시의 로컬 X, Y, Z이고, 어느 축이 위인지는 모델을 들여온 방식에 따라 다릅니다. Mesh Data 에셋의 **Rest Down**이 메시의 아래 방향입니다.
- 리스폰이나 컷 전환에서 살이 튀면: 한 프레임에 지금 속도로 갈 수 있는 거리보다 [**Teleport Distance**](./Softbody-Component#teleport-distance)(기본값 50 cm) 이상 더 움직이면 순간이동으로 보고 흔들지 않습니다. 그보다 짧게 옮기는 순간이동은 옮길 때 `NotifyTeleported()`를 함께 부릅니다.

### 한 부위만 {#one-body-part}

**Inertia Group Overrides**에 항목을 추가하고 **Bone**을 고릅니다. 드롭다운에는 베이크가 찾은 그룹만 나옵니다. 목록에 없는 본은 다른 그룹에 속해 있거나(베이크 로그를 보세요), **Max Inertia Groups**를 올리고 다시 베이크해야 합니다. 같은 본을 두 번 넣으면 첫 항목만 쓰입니다.

**Amount Scale**은 Inertia Amount에 곱하는 값으로 늘 쓰이며, 0이면 그 부위는 출렁이지 않습니다. **Damping**과 **Gravity Scale**은 앞의 체크 칸을 켰을 때만 쓰입니다. 한 캐릭터에서 덩어리는 크고 느리게, 배는 작고 빠르게 할 때 씁니다.

## 덩어리 {#lumps}

**Lump Yield**, **Lump Grip**, **Squeeze Stiffening**(Inertia > Lumps)은 누르는 손이 덩어리를 미는 정도입니다. 스켈레탈 메시에서 본의 Role을 **Lump**로 둔 뒤 베이크해야 하고, **Enable Contact**와 **Enable Inertia**가 켜져 있고 Inertia Amount가 0보다 커야 합니다. 조건이 맞지 않으면 칸이 흐리게 보입니다. 스태틱 메시(Mesh Renderer)에는 덩어리가 없습니다.

- 깊이 누를 때 덩어리가 너무 많이 움직이면: Squeeze Stiffening보다 Lump Yield를 낮춥니다.
- **Lump Grip**은 손에 붙든 피부로 덩어리를 끄는 값이라 **Contact Detail** Full이고 **Drag Friction**이 0보다 클 때만 듣습니다. 편 손으로 아주 빠르게 쓸고 지나간 뒤 덩어리가 계속 떨리면 Lump Grip을 낮춥니다.

## 아무것도 출렁이지 않을 때 {#when-nothing-jiggles}

- 컴포넌트 인스펙터의 **Setup** 상자가 `Inertia: off (...)`이면 괄호 안이 까닭입니다. `the bake has no jiggle data`이면 Mesh Data를 **Run Bake**로 다시 만듭니다.
- 덩어리가 통째로 움직이지 않고, 베이크 로그에 `Lumps: skipped, no bone has the Lump role in Bone Overrides` 또는 `Lumps: skipped, the baked data has no flesh softness for inertia`가 나오면: 본을 **Lump**로 둔 뒤 **Run Bake**를 누릅니다.
- `Inertia groups: the mesh has no skin weights, so no inertia groups are made.`: 관성 그룹은 스킨 웨이트에서 고르므로, 스킨 웨이트가 없는 스켈레탈 메시는 아무것도 출렁이지 않습니다.
- Play를 시작할 때 `N of M inertia group bones were not found on the mesh's skeleton; those groups will not jiggle.`가 나오면: 베이크한 그룹의 본을 렌더러의 계층에서 찾지 못한 것입니다. 모델 임포트 설정에서 **Optimize Game Objects**를 켰다면 그 본을 **Extra Transforms to Expose**에 넣고, 스켈레톤이 바뀌었다면 Run Bake를 다시 누릅니다.
- 스프링 본이나 물리로 움직이는 본의 살이 출렁이지 않으면: **Physics Driven Bones** 때문입니다. Softbody가 움직여야 한다면 그 본을 목록에서 빼고 **Reapply Paint**를 누릅니다.

다른 메시지: [문제 해결](./Troubleshooting).
