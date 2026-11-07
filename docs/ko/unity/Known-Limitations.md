# 알려진 제한

이번 베타에서 되지 않는 것과 그 대신 할 일입니다. 로그 메시지와 해결 방법은 [문제 해결](./Troubleshooting)에 있습니다.

## 베타에 없는 것 {#not-in-the-beta}

- **자기 접촉이 없습니다.** 캐릭터가 자기 손으로 자기 배나 허벅지를 누르지 못합니다. 몸의 콜라이더와 손의 [셰이프 필드](./Colliders#shape-fields)는 **Ignore Root**에 걸려 제 살을 누르지 않고, 다른 Softbody 메시만 누릅니다.
- **속 메시를 따로 줄 수 없습니다.** 한 면짜리 시트나 열린 튜브처럼 닫히지 않은 메시는 살을 잴 수 없어 베이크가 멈춥니다. DCC 툴에서 메시를 닫습니다.
- **테셀레이션은 Softbody/Lit (Tessellated)에만 있습니다.** 셰이더 그래프와 내 셰이더에서는 버텍스 사이의 좁은 눌림(손끝)이 음영으로만 보입니다. 그런 메시는 촘촘하게 만듭니다. Metal에서는 확인하지 못했고, 테셀레이션을 지원하지 않는 장치에서는 Softbody/Lit으로 그립니다.
- **접촉 맵 이음매를 자동으로 만들지 않습니다.** **Contact Map Seams**는 Mesh UV0 Islands만 됩니다. UV0를 쓸 수 없으면(넓이가 0이거나 접힌 UV) 베이크가 멈추므로 UV0를 고칩니다. Automatic (XAtlas)을 고르면 베이크가 멈춥니다.
- **Softbody Authoring Data의 미리보기는 Rigid Core 보기 하나입니다.** 칠, 살 두께, Separated Pairs, 판을 보는 미리보기와 에디터 안 시뮬레이션 미리보기가 없습니다. 눌림은 Play 중에 봅니다. 판과 Separated Pairs의 베이크 결과는 Console 로그로 확인합니다.
- **Softbody Grabber가 없습니다.** 손으로 피부를 잡아당기는 도구입니다.
- **Flex Offset은 아직 완성되지 않은 기능이라 불안정합니다.** 주의해서 쓰세요.
- **기록·재생 같은 개발 도구가 없습니다.** 상태와 GPU 시간은 [Softbody Debugger](./Project-Settings#softbody-debugger)와 Unity Profiler로 봅니다.
- **C# 소스가 없습니다.** 런타임과 에디터 코드는 컴파일된 어셈블리(DLL)로만 들어 있어, 고쳐서 다시 컴파일하거나 소스로 디버깅할 수 없습니다. 셰이더와 인클루드 파일은 소스입니다.

## Contact Detail {#contact-detail}

- **Contact Detail Full은 D3D12, D3D11, Vulkan에서 돕니다.** Metal과, 컴퓨트 스레드 그룹을 1024(Z 8)까지 받지 못하는 GPU에서는 Basic으로 돌고 Console에 `Contact Detail Full cannot run on this graphics device, so this mesh runs as Basic`을 한 줄 남깁니다. 설정값은 그대로 저장됩니다. Basic에서는 솟음, 끌림, **Edge Smoothing**, **Lump Grip**, Flex 칠(휘는 겉과 판)이 꺼집니다.
- **Full을 돌릴 수 있는 GPU에서는 Full을 쓰지 않아도 메모리를 씁니다.** 처음 접촉을 켤 때 응답 표 약 8 MB를 GPU에 올리고, Full용 임시 버퍼(공유 약 4 MB, 접촉 텍스처 벌마다 약 2 MB)를 같이 만듭니다. 켜고 끄거나 처음 누를 때 끊기지 않게 하려는 것입니다.
- **D3D11과 Metal에서는 Separated Pairs가 듣지 않습니다.** 나눈 두 부위가 나누지 않은 메시처럼 함께 눌립니다. 이 API용 셰이더 컴파일러가 그 계산을 컴파일 한도 안에 끝내지 못하기 때문입니다. D3D12나 Vulkan을 씁니다(Unity 6.1부터 새 Windows 프로젝트의 기본은 D3D12입니다).
- **D3D11에서는 판(Flex를 칠한 조각)이 느린 CPU에서 꺼질 수 있습니다.** 판 계산 셰이더 하나가 처음 컴파일에 몇 분 걸려, 컴파일 한도를 넘으면 판이 살처럼 눌리고 Console에 경고가 한 줄 나옵니다. D3D12와 Vulkan에서는 이런 일이 없습니다.

## 접촉 {#contact}

- **서로 누르는 Softbody 메시 둘 사이에는 겹친 만큼 틈이 벌어집니다.** 한쪽만 누르게 합니다. 눌리기만 할 메시의 콜라이더를 누르는 쪽 **Collider Channel**에 없는 레이어로 옮기거나 **Softbody Ignore**를 붙입니다. [콜라이더](./Colliders#softbody-meshes-pressing-each-other)를 보세요.
- **얇은 Box Collider 위에 놓은 메시는 윗면이 그 콜라이더 쪽으로 꺼질 수 있습니다.** 메시 두께와 그 아래 Box Collider 두께를 더한 값이 **Max Contact Depth**의 1.3배(기본 15 cm면 19.5 cm)보다 작을 때 생깁니다. 바닥 콜라이더를 두껍게 하거나, 바닥을 **Collider Channel**에 없는 레이어로 옮기거나, 바닥에 **Softbody Ignore**를 붙입니다.
- **볼록하지 않은 Mesh Collider, Terrain Collider, Wheel Collider는 누르지 않습니다.** Sphere, Capsule, Box Collider, Convex Mesh Collider, Character Controller만 누릅니다. 그 물체에 이런 콜라이더를 더합니다.
- **드물게 갱신되는 메시에서는 넓게 눌린 자리가 조금 덜 퍼집니다**(화면에서 작아 **Full Rate Screen Size**에 걸리거나 **Limit Contact Update Rate**에 묶인 경우). 그 제한을 켰다면 **Contact Updates Per Second**를 올립니다.
- **눌린 채 멈춰 있어도 갱신을 건너뛰지 않습니다.** 콜라이더가 닿아 있는 동안에는 아무것도 움직이지 않아도 GPU 비용이 계속 듭니다. 오래 닿아 있는 메시가 많으면 [성능](./Performance)의 제한을 씁니다.
- **Skin Tension은 당겨진 겉껍질이지 단단한 껍데기가 아닙니다.** Skin Tension을 세게 준 작은 조각은 통째로 거의 수평으로 가라앉지만, 가운데를 비껴 눌러도 판처럼 기울지는 않습니다. 크렘 브륄레나 박힌 플라스틱 같은 딱딱한 껍질도 없습니다.
- **컴포넌트를 꺼도 접촉 맵은 메모리에 남습니다.** Mesh Data를 참조하는 동안 그 접촉 맵 텍스처는 늘 올라와 있습니다. 끄면 줄어드는 것은 눌림을 계산하는 텍스처 세트와 GPU 작업입니다.
- **길이가 약 10 m를 넘는 메시에서는 끌림이 꺼집니다**(끌림 격자가 한 변 1024칸을 넘음). 눌림과 솟음은 그대로 됩니다. 우회 방법은 없습니다.
- **아주 느린 끌림은 반올림 오차로 사라집니다.** 초당 접촉 갱신 60번일 때, 피부가 1 mm 뒤처진 상태에서 0.03 mm/s보다, 또는 10 mm 뒤처진 상태에서 0.29 mm/s보다 느리게 움직이는 손은 피부를 움직이지 못합니다. 우회 방법은 없습니다.
- **아주 빠른 손은 막 닿은 칸을 조금 덜 끕니다.** 접촉 갱신 한 번에 약 1.3 cm를 넘게 움직이면 생깁니다. 빠른 손이 중요하다면 접촉 갱신 빈도를 초당 60번 아래로 제한하지 마세요.
- **Lump Grip을 켠 덩어리는 편 손으로 아주 빠르게 쓸고 지나간 뒤 계속 떨릴 수 있습니다.** **Lump Grip**을 낮추거나(0이면 꺼짐), 손가락 사이 틈을 콜라이더로 메웁니다.

## 베이크 {#bake}

- **LOD0만 베이크하고 변형합니다.** LOD Group 안의 다른 LOD는 눌리지도 출렁이지도 않습니다. 가까이서 보는 거리에서는 LOD0가 보이게 합니다.
- **베이크한 값은 버텍스 번호로 읽습니다.** 베이크는 메시를 바꾸지 않지만, 베이크 뒤에 버텍스가 바뀌게 다시 임포트하거나 고치면 **Run Bake**를 다시 눌러야 합니다. 같은 까닭으로 정적 배칭, 동적 배칭, GPU Resident Drawer, 베이스 버텍스 오프셋이 있는 서브메시와 함께 쓰지 않습니다([설치](./Installation#after-installing)).
- **Create Softbody Material은 모델의 머티리얼 리맵을 바꿉니다.** 리맵은 원래 머티리얼 이름 하나를 모델 전체에서 바꾸므로, 같은 원래 머티리얼을 쓰는 다른 슬롯이나 다른 렌더러(굽지 않은 옷이나 머리카락)도 Softbody 머티리얼을 받습니다. 그때는 베이크가 경고합니다. DCC 툴에서 머티리얼을 나누거나, 프리팹에서 Softbody 머티리얼을 입힙니다([머티리얼](./Materials#let-the-bake-make-the-material)).
- **베이크는 Undo로 되돌릴 수 없습니다.** **Cancel**은 아무것도 바꾸지 않고 멈춥니다. 베이크는 한 번에 하나만 돌고, Play를 시작하면 취소됩니다. 메시를 소스 컨트롤에 두고, 되돌릴 때는 다시 베이크합니다.

## 머티리얼 {#materials}

- **셰이더 그래프로 만든 Softbody 머티리얼은 GPU Resident Drawer와 함께 쓸 수 없습니다.** URP 에셋에서 **GPU Resident Drawer**를 Disabled로 둡니다. Softbody/Lit과 직접 쓴 셰이더는 상관없습니다.
- **셰이더 그래프에서는 빠르게 도는 본의 출렁임 모션 벡터가 조금 틀립니다.** TAA나 모션 블러에서 보일 수 있습니다. Softbody/Lit에는 없습니다. [셰이더 그래프](./Materials#shader-graph)를 보세요.

## 플랫폼 {#platform}

- **Unity 6.3(6000.3.26f1에서 확인)과 URP 17.3, Windows의 D3D12, D3D11, Vulkan에서만 확인했습니다.** Metal은 확인하지 못했습니다. 빌트인 렌더 파이프라인과 HDRP에서는 돌지 않습니다. 컴퓨트 셰이더가 없는 그래픽 API에서는 모든 Softbody 컴포넌트에서 접촉이 꺼지고 출렁임만 돕니다.
- **어셈블리는 Unity 6000.3.26f1과 URP 17.3.0으로 컴파일했습니다.** 다른 Unity나 URP 버전에서 그 API가 바뀌면 패키지를 다시 컴파일할 수 없어 쓸 수 없습니다.
- **D3D11에서는 접촉 셰이더를 처음 컴파일하는 데 약 15분이 걸립니다(빠른 PC 기준).** D3D12와 Vulkan에서는 몇 초입니다. 한 번 컴파일하면 Library 캐시에 남습니다. 에디터는 D3D12로 여는 것을 권합니다.

## 결함이 아닌 설계 {#by-design-not-a-defect}

- **접촉은 Play 모드에서만 돕니다.** 편집 모드의 Scene 뷰에서는 눌림이 보이지 않습니다.
- **컴포넌트를 꺼도 머티리얼 사본은 남습니다.** 다시 켤 때 새로 만들지 않으려는 것입니다. 꺼진 동안 메시는 모델링한 모양으로 돌아오고, 버텍스 셰이더는 칠 텍스처 하나를 읽은 뒤 나머지를 건너뜁니다. 원래 머티리얼은 컴포넌트를 지우거나 Play를 끝낼 때 돌아옵니다.
- **솟음은 눌린 자리 가장자리에서 살 두께 몇 배까지만 닿습니다.** 콜라이더에서 멈추고 살 두께보다 높이 솟지 않습니다.
- **눌린 자리의 가장자리는 깊고 가파르게 누를수록 잘 붙습니다.** 가장자리의 마찰 한도는 가운데와 같이 **Drag Friction**에 손이 누르는 세기를 곱한 값이므로, 얕고 평평하게 누른 자리의 가장자리는 미끄러집니다.
