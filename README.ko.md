# MallangPhysics

[English](README.md)

Windows의 Unreal Engine 5.6, 5.7, 5.8에서 스켈레탈 메시와 스태틱 메시를 말랑하게 만듭니다. 손과 물체가 누른 자리는 들어가고 둘레의 살은 솟으며, 움직이는 손은 피부를 끌고 가고, 몸이 움직이면 살이 출렁입니다. 에디터에서 메시를 한 번 베이크하고 컴포넌트 하나를 붙이면, 변형은 GPU에서 계산됩니다.

![데모 프로젝트에서 말랑한 방석을 누르는 손](docs/images/Home/hero.png)

**문서:** https://wjsekrud.github.io/MallangPhysics/ko/ (한국어, English)

## 내려받기

| | 위치 | 내용 |
|---|---|---|
| 무료판 | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysics-1.0.0-UE5.x-Win64.zip` | 엔진 버전 하나에 맞춰 미리 빌드한 플러그인. 셰이더와 공개 헤더가 들어 있고 C++ 소스는 없습니다. Development와 Shipping으로 빌드·패키징되며, DebugGame은 빌드되지 않습니다. |
| 소스판 | Fab에서 MallangPhysics 검색 | C++ 소스가 모두 든 플러그인. |
| 데모 프로젝트 | [Releases](https://github.com/wjsekrud/MallangPhysics/releases): `MallangPhysicsDemo-1.0.0-UE5.x.zip` | 무료판이 든 블루프린트 프로젝트. 같은 엔진 버전에서 Visual Studio 없이 열립니다. |
| Unity (베타) | [Unity 문서](https://wjsekrud.github.io/MallangPhysics/ko/unity/) | Unity 6.3 + URP, 소스 없음. |

## 무료판 설치

엔진 버전에 맞는 zip의 `SoftbodySystem` 폴더를 내 프로젝트의 `Plugins` 폴더에 풀고, [빠른 시작](https://wjsekrud.github.io/MallangPhysics/ko/Quick-Start)을 따릅니다.

## 라이선스

- Fab 판(C++ 소스 포함): Fab Standard 라이선스.
- 무료판, 데모 프로젝트, Unity 베타, 문서: Copyright © 2026 wjsekrud. All Rights Reserved.

데모 프로젝트에 든 다른 제작자의 콘텐츠와 Unity 패키지에 든 제3자 코드는 각자의 라이선스를 따릅니다: [크레딧과 라이선스](https://wjsekrud.github.io/MallangPhysics/ko/Credits-and-License).

## 참고

- 코드 주석은 한국어입니다(셰이더 파일 맨 위 설명만 영어). 주석의 일지·ADR 번호는 공개하지 않는 설계 기록을 가리킵니다.
- 버그 제보: [Issues](https://github.com/wjsekrud/MallangPhysics/issues).
