# 문제 해결

메시지는 바뀌지 않는 부분만 인용합니다. 메시지는 모두 Console에 `[Softbody]`로 시작하고, 누르면 그 메시지를 낸 컴포넌트나 에셋이 골라집니다([로그](./Project-Settings#logs)). Softbody Authoring Data의 **Validate**와 Softbody 컴포넌트의 **Validate Setup**은 누를 때마다 검사를 실행하고, Play를 시작할 때도 **Validate Setup**과 같은 검사가 돕니다. 컴포넌트 인스펙터의 **Setup** 칸과 에셋 인스펙터의 **Last Validation** 목록에서는 문제 줄 옆 단추(**Find Baked Data**, **Use Softbody Materials**, **Run Bake** 등)로 바로 고칠 수 있습니다. 에셋의 **Last Validation Errors**: 0 = 문제 없음, -1 = 마지막 베이크 뒤로 검사하지 않음, 그 밖의 수 = 오류 줄 수(경고는 세지 않음).

![Mesh Data를 비운 방석에서 Validate Setup을 누른 뒤의 Setup 칸과 [Softbody]로 거른 Console](../../images/unity/Troubleshooting/setup-and-console.png)

## 처음 Play가 몇 분 멈출 때 {#first-play-takes-minutes}

에디터를 D3D11로 열었다면 접촉 셰이더를 처음 컴파일하는 데 약 15분(빠른 PC 기준)이 걸리고, 그동안 에디터가 멈춘 것처럼 보입니다. D3D12와 Vulkan에서는 몇 초입니다. 컴파일 결과는 프로젝트의 Library 폴더에 남으므로 다음부터는 기다리지 않습니다(Library를 지우면 다시 컴파일합니다). 에디터는 D3D12로 여는 것을 권합니다([설치](./Installation#graphics-api)).

## 플레이: 아무것도 눌리지 않을 때 {#playing-nothing-dents}

접촉을 처음 준비할 때(보통 Play를 시작할 때) Console에 GPU 판정 한 줄이 나옵니다. `Contact is available on this GPU (...)`이면 이 GPU에서 접촉이 돕니다. 그 대신 `Contact is OFF for every Softbody component`로 시작하는 경고가 나오면 모든 Softbody 컴포넌트에서 접촉이 꺼지고 출렁임만 돕니다.

| 메시지 | 원인 | 해결 |
|---|---|---|
| `Contact is OFF for every Softbody component: it needs Shader Model 5` | 이 그래픽 API나 GPU가 컴퓨트 셰이더를 돌리지 못합니다. | 데스크톱 API(D3D12, D3D11, Vulkan)를 씁니다. |
| `Contact is OFF for every Softbody component: the contact compute shader cannot run on this GPU or graphics API` | 이 그래픽 API로 접촉 셰이더를 만들지 못했습니다. 괄호 안에 API와 처음 실패한 커널 이름이 나옵니다. | D3D12, D3D11, Vulkan 가운데 하나를 씁니다. Metal은 확인하지 못했습니다. |
| `Contact is OFF for every Softbody component: this GPU or driver` | GPU나 드라이버가 쓰기용으로 묶은 4채널 float 텍스처를 읽지 못합니다(D3D11과 D3D12에서만 검사합니다). | 더 새 GPU 드라이버에서는 켜질 수 있습니다. |
| `Contact is OFF for every Softbody component: the package resource` | 패키지 안의 `SoftbodyRuntimeResources` 에셋이 없거나 접촉 셰이더를 가리키지 않습니다. | 패키지를 다시 설치합니다. |
| `Contact is off: no graphics device` | 그래픽 없이 실행했습니다(`-nographics`, 전용 서버). | 할 일이 없습니다. 접촉은 화면에 그리는 데만 쓰입니다. |
| `Contact is off for ...: its contact map was baked with an older plugin` | 그 **Mesh Data**를 이전 버전 패키지로 베이크했습니다. | 그 Mesh Data의 Softbody Authoring Data에서 **Rebake Contact Map**을 누릅니다. |
| `has no contact map (press Rebake Contact Map on its Softbody Authoring Data)` | 접촉 맵이 없습니다. **Bake Contact Map**이 꺼져 있었거나 접촉 맵 베이크가 실패했습니다. | **Bake Contact Map**을 켜고 **Rebake Contact Map**(또는 **Run Bake**)을 누릅니다. |
| `Mesh Data is not assigned.` | **Mesh Data**가 비어 있습니다. | **Find Baked Data**를 누르거나, 메시의 Softbody Authoring Data에 있는 **Baked Data**를 지정합니다. |
| `found no renderer to deform` 또는 `no target mesh component` | 이 오브젝트와 자식에 Skinned Mesh Renderer나 Mesh Renderer가 없고, **Target Mesh Component**도 비어 있습니다. Softbody Ignore가 붙은 렌더러는 고르지 않습니다. | 메시가 있는 오브젝트(또는 그 부모)에 컴포넌트를 붙이거나, **Find**를 누르거나, **Target Mesh Component**를 지정합니다. |
| `is not a Skinned Mesh Renderer or a Mesh Renderer, so it is ignored` | **Target Mesh Component**가 다른 종류의 렌더러입니다. | Skinned Mesh Renderer나 Mesh Renderer를 지정합니다. |
| `target mesh does not match Mesh Data source` | **Mesh Data**를 다른 메시에서 베이크했습니다. | **Find Baked Data**를 누르거나, 이 메시를 베이크합니다. |
| `has no material slot with a Softbody shader` | Softbody 셰이더를 쓰는 슬롯이 없습니다. | **Use Softbody Materials**를 누르거나, Softbody Authoring Data에서 **Recreate Material**을 누르거나, [내 셰이더에 Softbody 속성을 넣습니다](./Materials#use-your-own-shader). |
| `changed after the bake (reimported or edited)` 또는 `has no per-vertex tables from this version` | 베이크한 뒤 메시를 다시 임포트하거나 고쳐서 버텍스가 바뀌었거나, 이전 버전 패키지로 베이크했습니다. | **Run Bake**를 누릅니다. |
| `is part of a static batch` | 정적 배칭이 버텍스 순서를 바꿨습니다. 접촉과 출렁임이 모두 꺼집니다. | 그 오브젝트의 **Batching Static**을 끕니다. |
| `has a base vertex offset` | 서브메시에 베이스 버텍스 오프셋이 있습니다. Softbody는 버텍스를 번호로 읽습니다. | 오프셋 없이 다시 내보냅니다. |
| `another Softbody component (...) already deforms` | 한 메시에 Softbody 컴포넌트가 둘 이상입니다. 먼저 켜진 것만 돕니다. | 메시마다 컴포넌트를 하나만 둡니다. |

## 플레이: 콜라이더가 누르지 않을 때 {#playing-a-collider-does-not-press}

| 메시지 | 원인 | 해결 |
|---|---|---|
| `has no simple collision shape and no shape field` | 볼록하지 않은 Mesh Collider, Terrain Collider, Wheel Collider는 누르지 않습니다. | Sphere, Capsule, Box Collider나 **Convex**를 켠 Mesh Collider를 더합니다. Character Controller는 캡슐로 누릅니다. 메시 모양대로 눌러야 하면 [셰이프 필드](./Colliders#shape-fields)를 베이크합니다. |
| `shape field of ... skipped (over 512 parts per frame` | 한 프레임에 메시에 닿는 셰이프 필드 조각이 512개를 넘었습니다. | 한 번에 닿는 셰이프 필드를 줄입니다. |
| 메시지 없음, 셰이프 필드를 베이크했는데도 손이 콜라이더 모양(네모난 손바닥 자국)으로 누름 | 렌더러의 레이어가 **Collider Channel**에 없거나 렌더러가 **Softbody Ignore** 아래라 셰이프 필드가 걸러졌고, 그 아래 콜라이더가 대신 누릅니다. 또는 Softbody Shape Field가 렌더러의 오브젝트에 붙어 있어 본 아래 콜라이더가 함께 누릅니다. | 손 렌더러의 레이어를 Collider Channel에 넣습니다. 컴포넌트는 손의 Animator 오브젝트에 둡니다(메뉴로 베이크하면 그 자리에 붙습니다). 눌릴 메시의 **Use Shape Fields**가 켜져 있는지도 봅니다. |
| `of a convex Mesh Collider is not readable` | 컨벡스 Mesh Collider의 메시를 CPU에서 읽을 수 없습니다. | 그 메시의 임포트 설정에서 **Read/Write**를 켭니다. |
| `contact skipped ... collision shape(s) over the per-frame budget` | 한 프레임에 메시에 닿은 모양이 32개(컨벡스 평면은 모두 합쳐 2048개)를 넘었거나, 평면이 없는 컨벡스가 있습니다. | 한 번에 닿는 콜라이더를 줄입니다. 누르지 않아야 할 콜라이더는 **Collider Channel**에 없는 레이어로 옮기거나 **Softbody Ignore**를 붙입니다([콜라이더](./Colliders#per-frame-limits)). |
| 메시지 없음, 메시가 전혀 눌리지 않음 | | [체크리스트](#no-dents-checklist)를 위에서부터 차례로 확인합니다. |

## 첫 닿음이 몇 프레임 늦을 때 {#first-touch-is-late}

처음 닿은 메시는 풀에서 접촉 텍스처 한 세트를 받아야 갱신을 시작합니다. 같은 종류(접촉 맵 해상도, 덩어리 유무, 거친 격자 크기)의 메시가 켜져 있는 동안 풀은 빈 세트를 **Texture Pool**(기본값 2)개까지 미리 만들어 두지만, 텍스처는 프레임마다 **Create Budget**(기본값 4 MB)만큼만 만듭니다. 그래서 여러 메시가 한꺼번에 처음 닿거나 큰 캐릭터를 막 켠 뒤 바로 닿으면, 몇 프레임 늦게 눌리기 시작합니다. 출렁임은 기다리지 않습니다. 켜거나 스폰한 직후의 첫 갱신도 프레임마다 **First Updates**(기본값 2)개까지만 돌고, 나머지는 한두 프레임 뒤에 돕니다.

한꺼번에 닿는 메시가 많으면 Project Settings > Softbody에서 **Texture Pool**을 올립니다. **Create Budget**을 올리거나 0(제한 없음)으로 두면 더 빨리 만들지만, 그 프레임이 끊길 수 있습니다([프로젝트 설정](./Project-Settings#texture-pool)).

## Contact Detail Full이 Basic으로 돌 때 {#full-runs-as-basic}

Play를 시작할 때나 **Validate Setup**을 누를 때 `Contact Detail Full cannot run on this graphics device, so this mesh runs as Basic`이 나오면, **Contact Detail**을 Full로 골랐지만 이 GPU에 Full 패스가 없어 Basic으로 돕니다. Metal이거나, 컴퓨트 스레드 그룹을 1024(Z 8)까지 받지 못하는 GPU입니다. 눌림과 덩어리는 그대로 돌고, 솟음, 끌림, **Edge Smoothing**, Flex 칠(휘는 겉과 판)은 꺼집니다. 설정값은 그대로 저장됩니다.

## 베이크가 멈추거나 실패할 때 {#bake-stops-or-fails}

| 메시지 | 원인 | 해결 |
|---|---|---|
| `Target Mesh must be a Skinned Mesh Renderer or a Mesh Renderer inside a model or prefab asset` (인스펙터) 또는 `Target Mesh is not set` | **Target Mesh**가 비어 있거나, 장면 오브젝트이거나, 다른 종류입니다. | 모델(FBX)이나 프리팹 에셋 안의 렌더러를 지정합니다. |
| `is on a Skinned Mesh Renderer without bones (blend shapes only)` | 본 없이 블렌드 셰이프만 있는 Skinned Mesh Renderer입니다. | Mesh Renderer를 쓰거나, 메시를 본 하나 이상에 스키닝합니다. |
| `Core Placement is Custom Spheres but Rigid Core Spheres has no sphere` | Custom Spheres에는 구가 하나 이상 있어야 합니다. | 구를 추가하거나 **Core Placement**를 Center나 Bottom으로 바꿉니다([베이크](./Baking)). |
| `Could not place the rigid core` | Center와 Bottom은 메시의 속을 읽는데, 이 메시에는 닫힌 속이 없습니다. | 닫힌 메시를 쓰거나 Custom Spheres를 씁니다. |
| `Could not measure the flesh: the mesh encloses no volume` 또는 `Could not build the outer surface: it came out empty` | 메시가 닫혀 있지 않습니다(한 면짜리 시트, 열린 튜브). 속 메시를 따로 주는 기능은 아직 없습니다. | DCC 툴에서 메시를 닫습니다. |
| `Bake stopped: the contact map normals point inward` | 면이 안쪽을 향합니다. | 메시를 닫고 면이 바깥을 향하게 한 뒤 **Rebake Contact Map**을 누릅니다. |
| `Contact Map Seams is set to Automatic (XAtlas), which is not available yet` | 자동 이음매는 아직 만들지 못합니다. | **Contact Map Seams**를 Mesh UV0 Islands로 둡니다. |
| `Could not use the mesh's UV0 islands for the contact map` | UV0를 쓸 수 없습니다(넓이가 0이거나 접힌 UV, 까닭은 메시지에 나옵니다). 베이크가 멈춥니다. | DCC 툴에서 UV0를 고칩니다. |
| `has no UV0. The contact map and slot mask paint need UV0.` | 메시에 UV0가 없습니다. | UV0를 만들어 다시 임포트합니다. |
| `Could not fit ... contact map islands into one contact map` | 해상도에 비해 아일랜드가 너무 많습니다. | **Contact Map Resolution**을 올립니다. |
| `No rigid core` | 스킨 웨이트가 있는 본이 메시 안에 하나도 없습니다. 베이크는 출렁임 없이 계속됩니다. | 스켈레톤의 본이 메시 안에 있는지 확인합니다. |
| `Inertia bake failed: the rigid core holds too few grid points` | 코어가 측정 격자에 비해 너무 얇습니다. | **Bone Thickness**를 올리거나, **Rigid Core Spheres**(Custom Spheres)를 추가하거나, **Measure Cell Size**를 낮춥니다. |
| `Inertia bake failed: no surface vertex can jiggle` | 단단한 코어나 뼈로 칠한 살이 메시 전체를 덮습니다. | **Bone Thickness**를 낮추거나, 무른 살 위의 **Depth** 칠을 밝게 합니다. |
| `vertex positions that are not finite numbers` | 버텍스 위치에 NaN이나 무한대가 있습니다. | DCC 툴에서 메시를 고쳐 다시 임포트합니다. |
| `not triangles. Softbody bakes triangle meshes only.` | 서브메시가 선이나 점으로 되어 있습니다. | 삼각형 메시로 내보냅니다. |
| `skin weights that point outside the renderer's` | 스킨 웨이트가 렌더러에 없는 본을 가리킵니다. | 메시의 스키닝을 고쳐 다시 임포트합니다. |
| `remaps its material` 또는 `has a material slot with no material` | 모델이 지워진 머티리얼로 리맵해 슬롯이 비었습니다. 이름을 알 수 없는 슬롯은 Softbody Material Slots와 Slot Masks가 찾지 못해 변형되지 않습니다. | 모델 임포트 설정의 Materials(Remapped Materials)에서 머티리얼을 다시 정하고 **Run Bake**를 누릅니다. |
| `Contact map bake: the mesh has ... vertices but the baked data has` | 마지막 베이크 뒤에 메시가 바뀌었습니다. | **Run Bake**를 누릅니다. |
| `Output Folder already has ..., baked from a different mesh` | 이름이 같은 다른 메시의 베이크가 **Output Folder**에 있어, 덮어쓰면 그 메시가 망가집니다. | 다른 **Output Folder**를 정합니다. |
| `is not a folder the bake can write to` 또는 `Output Folder must be inside the Assets folder` | **Output Folder**가 Assets 폴더 밖에 있습니다. | Assets 폴더 안의 폴더를 씁니다. |
| `is package content and cannot be changed` | 모델이나 **Baked Data**가 패키지 안에 있습니다. | Package Manager에서 견본을 임포트하거나 모델을 Assets 폴더로 복사하고, 그 복사본을 베이크합니다. |
| `was canceled because Play mode started` | 베이크하는 동안 Play를 시작했습니다. 아무것도 바뀌지 않았습니다. | Play를 멈추고 같은 단추를 다시 누릅니다. |
| `Another Softbody bake is running.` | 베이크는 한 번에 하나만 돕니다. | 앞 베이크가 끝난 뒤 누릅니다. |
| `already had unsaved changes before the bake, so the bake did not save them` 또는 `Some assets could not be saved` | 베이크는 저장하지 않은 변경이 이미 있던 에셋을 저장하지 않습니다. | 직접 저장합니다(File > Save Project). |

## Validate가 문제를 알릴 때 {#validate-reports-problems}

| 메시지 | 원인 | 해결 |
|---|---|---|
| `Nothing to validate - Baked Data is empty` | 아직 베이크하지 않았습니다. | **Run Bake**를 누릅니다. |
| `Baked Data was baked from a different mesh than Target Mesh` | 베이크한 뒤 **Target Mesh**가 바뀌었습니다. | **Run Bake**를 누릅니다. |
| `The mesh changed after the bake (reimported or edited)` | 베이크한 뒤 메시를 다시 임포트하거나 고쳐서 버텍스가 바뀌었습니다. | **Run Bake**를 누릅니다. |
| `The flesh softness bake is missing or out of date, so nothing jiggles` | 출렁임 데이터가 없습니다. 스켈레탈 메시는 메시 안에 스킨 웨이트가 있는 본이, 스태틱 메시는 **Core Placement**로 정한 단단한 코어가 필요합니다. | 그것을 갖춘 뒤 **Run Bake**를 누릅니다. |
| `There is no contact map, so nothing dents` | **Bake Contact Map**은 켜져 있지만 접촉 맵이 없습니다. | **Rebake Contact Map**(또는 **Run Bake**)을 누릅니다. |
| `No slot uses a Softbody material, so the mesh will not deform` | 아직 메시에 Softbody 머티리얼이 없습니다. | **Recreate Material**을 누르거나, 머티리얼을 지정한 뒤 **Rebake Contact Map**을 누릅니다. |
| `Volume Preservation is ... but the flesh was measured with` | 베이크한 뒤 **Volume Preservation**을 바꿨습니다. 눌린 자리는 이미 새 값을 씁니다. | 덩어리 전체가 같은 재료로 퍼지도록 **Run Bake**를 누릅니다. |
| `the slot paint changed since the flesh was measured` | 마지막 Run Bake 뒤에 칠을 바꿔서, 출렁임과 접촉이 서로 다른 칠을 읽습니다. | **Run Bake**를 누릅니다. |
| `vertices on the tessellated Softbody slots have split normals` | 테셀레이션 머티리얼을 쓰는 슬롯에 노멀이 갈라진 모서리(하드 엣지)가 있어, 눌린 그 모서리에서 작은 틈이 벌어질 수 있습니다. | DCC 툴에서 그 슬롯의 노멀을 부드럽게 하거나, 그 슬롯은 Softbody/Lit을 씁니다. |
| `units long. Softbody expects 1 unit = 1 m` | 메시가 아주 작거나 큽니다. 단위를 잘못 임포트했을 수 있습니다. | 모델 임포트 설정의 **Scale Factor**와 **Convert Units**를 확인합니다. |
| `is in a LOD Group. Softbody bakes and deforms LOD0 only` | LOD0 밖의 LOD는 눌리지도 출렁이지도 않습니다. | 가까이서 보는 거리에서는 LOD0가 보이게 합니다. |
| `is marked Batching Static` | 정적 배칭이 버텍스 번호를 바꿔 Softbody가 엉뚱한 값을 읽습니다. | 그 오브젝트의 **Batching Static**을 끕니다. |
| `GPU Resident Drawer is on, and it may draw` | GPU Resident Drawer가 버텍스 번호를 바꿔 그릴 수 있습니다. | 그 Mesh Renderer에 **Disallow GPU Driven Rendering** 컴포넌트를 붙입니다. |
| `is imported with at most 4 bone weights per vertex (Skin Weights: Standard)` | 모델이 버텍스마다 본 웨이트를 4개까지만 임포트해, Softbody와 스키닝이 모델보다 적은 웨이트를 봅니다. | 모델 임포트 설정의 Rig에서 **Skin Weights**를 Custom, **Max Bones/Vertex**를 32로 두고 **Run Bake**를 누릅니다. |
| `VerifyIntegrity <Baked Data>: ...` 또는 `VerifySkeletal: ...` | 베이크한 텍스처나 표가 더는 메시와 맞지 않습니다(베이크 뒤에 메시를 고쳤거나 다시 임포트함). | **Run Bake**를 누릅니다. |

## 칠과 관성 그룹 {#paint-and-inertia-groups}

| 메시지 | 원인 | 해결 |
|---|---|---|
| `Could not read the slot paint, so Depth and Volume are baked as 1 everywhere` | 접촉 맵을 베이크할 때 칠을 읽지 못했습니다. | **Run Bake**를 누릅니다. |
| `its Paint Texture has no source pixels` 또는 `Paint: could not read the pixels of mask texture` | Paint Texture의 원본 픽셀을 읽지 못해, 그 슬롯은 칠하지 않은 것(흰색)으로 남습니다. | 텍스처를 다시 임포트하거나, 그 채널을 **White everywhere (unpainted)**나 **Black everywhere**로 둡니다. |
| `has sRGB on, so the bake reads it as a color` | Paint Texture의 sRGB가 켜져 있어 회색이 어둡게 읽힙니다(0.5가 0.21로). | 인스펙터의 **Turn Off sRGB**를 누르거나, 텍스처 설정에서 sRGB (Color Texture)를 끕니다. |
| `which is black everywhere, so the whole slot becomes` | 한 채널이 텍스처 전체에서 검은색이라 슬롯 전체가 뼈, 단단한 살, 폼, 판 하나 가운데 하나가 됩니다. | 그 채널을 쓰지 않는다면 **White everywhere (unpainted)**로 둡니다. |
| `Paint: the flesh on the Softbody material slots barely moves` | 출렁여야 할 살이 거의 움직이지 않습니다. | 무른 살 위의 **Depth**와 **Jiggle**이 검은색이 아닌지 확인합니다. |
| `Paint: the deepest flesh moves ...x as far as the reference flesh` | 깊은 살이 기준 살보다 훨씬 많이 움직여, Inertia Amount가 그만큼 더 흔듭니다. | 사람 크기의 몸이면 **Flesh Reach**를 약 3으로 두고 **Run Bake**를 누릅니다. |
| `Paint: the jiggle amount (after Physics Driven Bones) changes sharply over a narrow strip` | **Physics Driven Bones** 때문에 출렁임이 좁은 띠 안에서 급하게 바뀝니다. 그 자리는 출렁일 때 찢어지거나 접혀 보일 수 있습니다. 메시지에 메시 위치가 나옵니다. | 그 본들의 스킨 웨이트를 넓고 완만하게 줄어들게 합니다. |
| `in Physics Driven Bones is not in the skeleton` | **Physics Driven Bones**의 본 이름에 오타가 있습니다. | 이름을 고칩니다. |
| `Paint: Physics Driven Bones were skipped because the mesh has no skin weights` | 메시에 스킨 웨이트가 없습니다. | 메시의 스키닝을 확인하고 **Reapply Paint**를 누릅니다. |
| `The mesh has no material slot named` | **Slot Masks**, **Softbody Material Slots**, **Pressable Slots**에 메시에 없는 슬롯 이름이 있습니다. 메시지 끝에 어느 목록인지 나옵니다. | 이름을 고칩니다. |
| `Inertia groups: all ... groups are taken, so lump bone ... gets no group of its own` | 출렁이는 본이 **Max Inertia Groups**가 허용하는 수보다 많습니다. | **Max Inertia Groups**를 올립니다(최대 16). |
| `Lumps: could not read the baked inertia groups` 또는 `Paint: the inertia groups could not be read` | 접촉 맵이나 칠을 따로 다시 만들 때 관성 그룹을 읽지 못했습니다. | **Run Bake**를 누릅니다. |
| `The Flex paint dents like bare skin because Contact Detail is Basic` 또는 `Flex Offset has no effect because Contact Detail is Basic` (Play, Validate Setup) | Flex는 Contact Detail Full에서만 듣습니다. | 판이나 휘는 겉이 필요한 메시는 **Contact Detail**을 Full로 둡니다. |
| `Flex plates: ... painted pieces wrap around the body` | 몸을 둘러 감긴 조각은 판이 되지 못하고 칠대로 휩니다. | 칠에 가는 흰 틈을 두어 평평한 조각으로 나눕니다. |
| `Flex plates: ... painted pieces have no contact map cells under them` | 조각이 눌리는 슬롯 밖에 있거나, 떠 있는 조각 밑에 눌릴 겉면이 없습니다. | 밑의 슬롯을 **Pressable Slots**에 넣거나, 장식을 피부 가까이 둡니다. |
| `Flex plates: ... painted pieces cover more than` | 조각이 너무 커서 판이 되지 못하고 칠대로 휩니다. | 가는 흰 틈을 두어 작은 조각으로 나눠 칠합니다. |
| `Flex plates: ... painted pieces sit at the very edge of the contact map` | 조각이 접촉 맵 가장자리에 걸려 판으로 읽을 수 없습니다. | 다른 **Contact Map Resolution**으로 다시 베이크합니다. |
| `Flex plates: ... coarse cells ... are shared by two rigid pieces` | 두 판이 거친 칸 하나를 나눠 써서, 그 자리를 누르면 한쪽만 움직입니다. | 판 사이를 그 거친 칸보다 넓게 띄워 칠합니다. |

## 눌리지 않나요? 체크리스트 {#no-dents-checklist}

Softbody 컴포넌트와 메시의 Softbody Authoring Data에서:

1. Play 중입니다. 접촉은 Play 모드에서만 돕니다.
2. **Enable Contact**가 켜져 있습니다.
3. **Mesh Data**가 이 메시의 **Baked Data**입니다. **Validate Setup**을 누르면 Console에 `setup valid`와 `contact=on`이 나와야 합니다. `contact=off (...)`이면 괄호 안에 이유가 나옵니다.
4. 메시를 **Bake Contact Map**을 켠 채 베이크했고, **Validate**를 통과합니다.
5. 누르는 물체가 **Collider Channel**(기본값: Ignore Raycast를 뺀 모든 레이어)의 레이어에 있고, 켜져 있고, 트리거가 아니며, Sphere, Capsule, Box Collider, Convex Mesh Collider, Character Controller 가운데 하나입니다. 그 오브젝트와 부모에 **Softbody Ignore**가 없고, **Ignore Root**(비우면 가장 가까운 부모 Animator, 없으면 이 오브젝트) 아래에 있지 않습니다.
6. **Find Colliders Automatically**가 켜져 있습니다.
7. **Min Screen Size**(기본값 0.15): Scene 뷰를 포함한 모든 카메라에서 메시가 이 값보다 작게 보이는 동안에는 눌림이 멈춥니다. 이 원인을 배제하려면 0으로 설정합니다.
8. 눌리는 슬롯이 Softbody 셰이더 머티리얼을 씁니다. 머티리얼을 바꾼 뒤에는 **Rebake Contact Map**을 한 번 더 눌러 머티리얼을 접촉 맵에 연결합니다.
9. 두 Softbody 메시 사이에서는 한쪽만 누릅니다([콜라이더](./Colliders#softbody-meshes-pressing-each-other)).

## 눌림이 이상해 보일 때 {#dents-look-wrong}

- **표면을 가로지르는 선이나 계단.** 접촉 맵이 나뉜 곳을 가로지르는 눌림은 조금 덜 정확합니다. **Contact Map Seams**는 메시의 UV0 이음매에서 나눕니다. **Debug View**를 켜고 **Debug View Mode**를 2로 두면 나뉜 곳이 체커가 끊기는 선으로 보입니다.
- **손끝이 눌린 자리 대신 흐릿하게 번집니다.** **Contact Map Resolution**을 올립니다(사람 크기 캐릭터는 512, 가까이서 보면 1024). 눌린 모양은 버텍스에서만 움직이므로, 버텍스 사이의 좁은 눌림은 음영으로만 보입니다. 가까이서 보는 메시는 촘촘하게 만듭니다. **Debug View Mode** 5는 삼각형 안에서 눌림을 읽는 곳을 초록색으로, 이음매를 따라 버텍스만 움직이는 곳을 빨간색으로 보여 줍니다.
- **눌림이 덜 들어가거나, 콜라이더가 눌림 없이 파고듭니다.** **Max Contact Depth**(기본값 15 cm)까지는 누른 깊이를 그대로 그리고, 그 너머는 부드럽게 줄어 1.3배에서 멈춥니다. 콜리전이 보이는 피부보다 몇 cm 안쪽에 있다면(몸 캡슐), **Max Contact Depth**를 그 간격으로 설정합니다.
- **눌린 자리가 너무 넓거나 좁습니다.** 폭은 살 두께와 Softbody 컴포넌트의 **Skin Tension**이 정합니다([살의 재료](./Concepts#flesh-material)). 단단한 코어를 옮기거나(Core Placement, Rigid Core Spheres, Bone Thickness) **Max Flesh Thickness**나 Depth 칠을 바꿉니다. Softbody Authoring Data 미리보기의 Rigid Core 보기로 코어 자리를 확인합니다.
- **위를 누르면 옆도 눌립니다.** 단단한 코어가 바닥에만 있으면 옆면은 아주 두꺼운 살로 측정됩니다. **Max Flesh Thickness**를 가장 깊은 누름보다 조금 크고 물체 폭의 1/4보다 작게 설정합니다.
- **Flex를 칠한 곳이 뻣뻣하게 굽거나 판으로 움직이지 않습니다.** Flex는 **Contact Detail**이 Full일 때만 듣습니다. Full인데도 판이 되지 않았다면 베이크 로그의 `Flex plates:` 줄에 조각마다 까닭이 있습니다(위 표).
- **눌림이 엉뚱한 본을 따라갑니다.** `the bones of ... differ from the bones Mesh Data was baked with`가 나왔다면 베이크한 뒤 스켈레톤이 바뀌었습니다. **Run Bake**를 누릅니다.

## 출렁임이 이상할 때 {#jiggle-wrong}

- **아무것도 출렁이지 않습니다.** **Enable Inertia**와 **Contact Only (Debug)**를 확인합니다. 베이크에 출렁임 데이터가 없으면 **Validate Setup**이 `Enable Inertia is on but Mesh Data has no baked flesh softness`를 알립니다.
- **일부가 출렁이지 않습니다.** `inertia group bones were not found on the mesh's skeleton`은 베이크한 뒤 스켈레톤이 바뀌었거나, 모델의 **Optimize Game Objects** 때문에 본 오브젝트가 없다는 뜻입니다. **Run Bake**를 다시 누르거나, 그 본들을 Extra Transforms to Expose에 넣습니다.
- **살은 출렁이는데 음영이 따라오지 않습니다.** `was baked before jiggle shading existed`가 나왔다면 **Reapply Paint**를 누릅니다.
- **같은 Inertia Amount가 메시마다 다르게 흔듭니다.** `uses the old jiggle scale`이 나왔다면 **Reapply Paint**를 누릅니다.
- **Gravity Scale이나 Lump Yield가 듣지 않습니다.** `Gravity Scale has no effect because this mesh has no lumps` 또는 `Lump Yield and Lump Grip have no effect because`가 나옵니다. 스켈레탈 메시라면 가슴이나 엉덩이 본을 Softbody Authoring Data의 **Bone Overrides**에서 Lump로 두고 **Run Bake**를 누릅니다. 스태틱 메시에는 덩어리가 없습니다.
- **프레임이 떨어지면 살이 튑니다.** **Hitch Guard**(기본값 0.1초)보다 긴 프레임은 히치로 보고 그 가속도를 무시합니다. 그래도 튄다면 값을 낮춥니다.
- **리스폰이나 카메라 컷에서 살이 튑니다.** 현재 속도로 갈 수 있는 거리에 **Teleport Distance**(기본값 50 cm)를 더한 것보다 멀리 움직이거나, 지난 회전 속도로 갈 수 있는 것보다 45°를 넘게 더 돌면 순간이동으로 보고 출렁이지 않습니다. 값을 낮추거나, 옮긴 프레임에 스크립트에서 `NotifyTeleported()`를 부릅니다. 실제로 빠른 움직임에서 출렁임이 사라진다면 값을 올립니다.
- **큰 덩어리가 가장 크게 출렁일 때 납작해집니다.** **Max Inertia Offset**(기본값 25 cm)이 화면에 그리는 움직임을 자릅니다. 큰 캐릭터는 값을 올립니다. 큰 움직임의 느낌을 바꾸려면 **Inertia Firmness**를 씁니다.

## 빠른 시작 뒤 머티리얼이 흰색일 때 {#material-is-white-after-quick-start}

**Create Softbody Material**은 슬롯의 원래 머티리얼에서 베이스 컬러와 노멀 맵을 복사하는데, 견본 방석에는 둘 다 없어서 `Copied: base color (not found), normal map (not found)`가 나옵니다. 머티리얼 베리언트에서 **Base Map**과 **Normal Map**을 직접 설정합니다.

## 질문 {#questions}

**콜라이더 목록을 컴포넌트에 직접 줄 수 있나요?** 아니요. 그런 목록도, 스크립트 함수도 없습니다. **Find Colliders Automatically**를 끄면 바깥 콜라이더는 메시를 누르지 않습니다. 무엇이 누를지는 **Collider Channel**, **Ignore Root**, **Softbody Ignore**로 정합니다([콜라이더](./Colliders)).

**Play를 누르지 않아도 Scene 뷰에서 눌림이 보이나요?** 아니요. 컴포넌트는 Play 모드에서만 작동합니다. Play하는 동안에도 **Min Screen Size**는 Scene 뷰를 카메라로 셉니다.

**메시에서 Softbody를 빼려면 어떻게 하나요?** Softbody 컴포넌트를 지우고, 모델 임포트 설정의 Materials(Remapped Materials)나 프리팹에서 슬롯에 원래 머티리얼을 다시 넣습니다. 베이크는 메시를 바꾸지 않습니다. 베이크한 에셋(Softbody Authoring Data, Baked Data, 머티리얼)은 남겨도 되고 지워도 됩니다.

**패키지를 업데이트한 뒤에는 무엇을 다시 베이크하나요?** Validate나 Validate Setup이 이전 버전으로 베이크했다고 알리면, 그 메시지가 말하는 단추(**Run Bake**, **Rebake Contact Map**, **Reapply Paint**)를 누릅니다. 알림이 없으면 다시 베이크하지 않아도 됩니다.
