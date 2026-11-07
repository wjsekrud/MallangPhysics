# 크레딧과 라이선스

## 라이선스 {#license}

- Fab 판(C++ 소스 포함): Fab Standard 라이선스.
- GitHub 무료판(소스 없이 미리 빌드한 판), 데모 프로젝트, 이 문서: Copyright © 2026 wjsekrud. All Rights Reserved.

데모 프로젝트는 GitHub 릴리스에 엔진 버전마다 따로 있습니다. 데모에 든 다른 제작자의 콘텐츠는 아래 [데모 프로젝트의 콘텐츠](#content-in-the-demo-project)에 적은 라이선스를 따릅니다.

## 데모 프로젝트의 콘텐츠 {#content-in-the-demo-project}

- 소품: 제작자가 Blender 스크립트로 처음부터 만들었습니다. 내려받은 모델이나 텍스처는 없습니다.
- 몸 메시(`SK_Body`): 제작자가 만들었습니다.
- 손(`SK_GraspHand`): 제작자의 캐릭터 모델에서 손만 떼어 리깅하고 균일하게 1.116배 키운 것입니다. 외부에서 받은 파일은 섞여 있지 않습니다.
- 4.3에서 소파에 앉는 캐릭터(`SK_Sitter`): Quaternius의 Universal Base Characters(머리, 머리카락)와 Modular Character Outfits - Fantasy(Peasant 옷)를 합친 것입니다(CC0).
- 캐릭터의 앉기 동작(`A_Sitter_Sit`)과 몸의 걷기·달리기 애니메이션: Quaternius, Universal Animation Library (CC0).
- 앉기 애니메이션(`body_sit.fbx`): 제작자가 몸의 스켈레톤에 맞춰 만들었습니다.
- 샘플 방석(`/SoftbodySystem/Samples/SM_SampleCushion`): 제작자의 스크립트로 만들었고, 플러그인 안에 들어 있습니다.

## 문서 사진 {#documentation-photos}

문서의 일부 사진에는 Selestia 캐릭터(©ジンゴ)가 옷을 입은 모습으로 나옵니다. Selestia는 데모 프로젝트에 들어 있지 않습니다.

## 참고 {#references}

- [KawaiiPhysics](https://github.com/pafuhana1213/KawaiiPhysics) (pafuhana1213, MIT 라이선스): 개발과 시험에 썼습니다. 플러그인과 데모 프로젝트에는 들어 있지 않습니다.

## 플러그인이 쓰는 엔진 기능 {#engine-features-the-plugin-builds-on}

- 플러그인 디스크립터가 켜는 Unreal Engine 플러그인: Tet Meshing, Geometry Processing (Epic Games).
- 자동 접촉 맵 이음매는 엔진에 통합된 XAtlas를 씁니다.

## 문서 사이트 {#documentation-site}

- 한국어 쪽 글꼴: Pretendard (길형진, SIL Open Font License 1.1).
