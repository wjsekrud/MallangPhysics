# 데모 프로젝트

데모는 플러그인과 따로 받는 `SoftbodyDemo` 프로젝트입니다. [GitHub 릴리스](https://github.com/wjsekrud/MallangPhysics/releases)에 엔진 버전마다 zip(`MallangPhysicsDemo-1.0.0-UE5.6.zip`, `UE5.7`, `UE5.8`)이 있고, 콘텐츠는 셋 모두 같습니다. 블루프린트 프로젝트라 Visual Studio 없이 열립니다. 소품과 캐릭터를 기능마다 한 장면씩 보여 줍니다.

## 들어 있는 것 {#what-is-in-it}

- `Plugins/SoftbodySystem`: 그 엔진 버전의 GitHub 무료판(미리 빌드한 플러그인, C++ 소스 없음). 몸·소품은 모두 베이크되어 있습니다.
- `Content/Demo`: 데모 레벨, 소품, 몸(`SK_Body`)과 손(`SK_GraspHand`), 소파에 앉는 캐릭터(`SK_Sitter`), 블루프린트(`BP_SoftbodyDemo*`).
- `Content/ContactLab`, `Content/Profiling`: 비교·측정용 레벨.

## 레벨 {#maps}

| 레벨 | 보여 주는 것 |
|---|---|
| `/Game/Demo/L_SoftbodyDemo` | 데모 본편. 격자 벽의 칸마다 한 장면이 있고, 제목과 설명은 칸 뒷벽에 적혀 있습니다. |
| `/Game/ContactLab/L_ContactMaterials` | 재료 12종을 같은 깊이로 누르는 비교: Skin Tension, Flex, Volume Preservation, 복원 속도, 폼과 메모리폼. |
| `/Game/ContactLab/L_ContactCube` | Skin Tension, Flex, Volume Preservation을 각각 0과 1로 나눈 8가지 조합. |
| `/Game/Profiling/L_SoftbodyProfile` | 본편과 같은 장면을 칸마다 따로 켜고 잴 수 있게 한 레벨. [성능](Performance)의 콘솔 명령과 함께 씁니다. |

본편의 칸은 다음과 같습니다.

| 칸 | 장면 |
|---|---|
| 1.1 | Vertex spacing: 메시 밀도 비교 |
| 2.1~2.5 | Volume Preservation, 복원 속도, 오래 누른 자국, 한 메시에 재료 둘, Skin Tension |
| 3.1, 3.2 | 양쪽에서 짜기, 각진 모서리 |
| 4.1~4.4 | 손으로 쥐기, 손으로 문지르기, 소파에 앉기, 빠른 충격 |
| 5.1·6.1·6.2 | 작은 소품 |
| 7.1, 7.2 | 폼 위의 강체 공, 서로 눌리는 말랑한 조각 |
| 8.1 | 스켈레탈 메시: 제 손으로 제 몸 누르기 |
| 9.1, 9.2 | 얇은 막: 도는 접시, 펼친 시트 |

## 열기 {#open}

`SoftbodyDemo.uproject`를 zip 이름의 엔진 버전으로 열고 레벨을 골라 플레이합니다. 칸을 하나씩 보려면 `L_SoftbodyProfile`에서 `Softbody.Scenario <칸>`을 씁니다.

## 직접 쓰기 {#reuse}

데모의 소품과 몸은 [빠른 시작](Quick-Start)과 같은 방법으로 베이크되어 있습니다. 소품 `SA_*` 저작 에셋을 열면 쓴 값을 그대로 볼 수 있습니다.
