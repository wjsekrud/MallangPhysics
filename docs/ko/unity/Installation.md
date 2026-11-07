# 설치

Unity 6.3(6000.3.26f1에서 확인)과 URP 17.3이 필요합니다. 빌트인 렌더 파이프라인과 HDRP에서는 돌지 않고, GPU가 컴퓨트 셰이더를 돌려야 합니다.

## Package Manager {#package-manager}

Package Manager의 **Install package from git URL**에 다음 주소를 넣습니다.

```text
https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics
```

버전을 고정하려면 주소 끝에 태그를 붙입니다. 이번 베타의 태그는 `unity-v0.1.0-beta.1`입니다.

```text
https://github.com/wjsekrud/MallangPhysics.git?path=/Unity/com.wjsekrud.mallangphysics#unity-v0.1.0-beta.1
```

태그가 없는 주소는 저장소의 최신 커밋을 받습니다. 패키지는 **MallangPhysics**(`com.wjsekrud.mallangphysics`)로 나옵니다. Universal RP 17.3.0, Burst 1.8.30, Mathematics 1.3.3, Collections 2.6.8이 함께 들어옵니다.

패키지에는 C# 소스가 없습니다. 런타임과 에디터 코드는 컴파일된 어셈블리(DLL)로 들어 있고, 셰이더와 인클루드 파일은 소스로 들어 있습니다.

## 그래픽 API {#graphics-api}

에디터는 D3D12로 여는 것을 권합니다. 접촉 셰이더는 그래픽 API마다 한 번 컴파일되고, 그 뒤에는 프로젝트의 Library 캐시에 남습니다.

| API | 첫 컴파일 |
|---|---|
| D3D12 | 수 초 |
| Vulkan | 수 초 |
| D3D11 | 약 15분(빠른 PC 기준, 느린 PC는 더) |

컴파일은 Softbody가 있는 장면을 처음 Play할 때 일어납니다. 에디터는 Windows Player 설정의 Graphics APIs 목록에서 맨 위 API로 열립니다. Metal은 확인하지 못했고, **Contact Detail** Full은 Metal에서 돌지 않습니다([알려진 제한](./Known-Limitations#contact-detail)).

GPU나 그래픽 API가 접촉 셰이더를 돌리지 못하면 Console에 `Contact is OFF for every Softbody component` 경고가 한 번 나오고, 접촉이 꺼진 채 출렁임만 돕니다.

## 설치 뒤 확인할 것 {#after-installing}

- **Project Settings > Softbody**는 프로젝트 전체의 성능 설정입니다. 설정 에셋이 없으면 기본값으로 돕니다. 값을 바꾸려면 그 화면의 **Create Settings Asset**을 누릅니다. `Assets/Resources/SoftbodySettings.asset`이 생기며, 빌드에서 읽히려면 Resources 폴더 안에 이 이름으로 있어야 합니다.
- 패키지 안의 `Runtime/Resources/SoftbodyRuntimeResources.asset`이 접촉 셰이더를 빌드에 넣습니다. 고치거나 옮기지 않습니다. 이 에셋이 깨지면 Console 경고와 함께 접촉이 꺼집니다.
- Softbody는 베이크 때의 버텍스 번호로 값을 읽습니다. 그래서 버텍스 순서를 바꾸는 그리기 방식을 Softbody 메시에 쓰지 않습니다.
  - URP 에셋에서 **GPU Resident Drawer**를 켰다면 Softbody를 단 Mesh Renderer마다 **Disallow GPU Driven Rendering** 컴포넌트를 붙입니다.
  - Softbody 메시는 **Batching Static**을 끕니다. 정적 배칭된 메시는 접촉과 관성이 모두 꺼집니다.

  베이크 에셋의 **Validate**와 Softbody 컴포넌트의 **Validate Setup**이 둘 다 경고합니다.

## 스크립트 {#scripting}

런타임 어셈블리는 `Softbody.Runtime.dll`(네임스페이스 `Softbody`)이고, 컴포넌트 클래스는 `SoftbodyComponent`입니다. 베이크는 에디터 전용 어셈블리 `Softbody.Editor.dll`(네임스페이스 `Softbody.Editor`)에 있습니다. 둘 다 Auto Referenced라 asmdef 없는 스크립트와 **Override References**를 끈 asmdef에서 바로 씁니다. **Override References**를 켠 asmdef는 **Assembly References**에 이 DLL을 넣습니다.
