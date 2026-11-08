---
layout: home
title: MallangPhysics
titleTemplate: Unreal Engine 5 소프트바디 플러그인

hero:
  name: MallangPhysics
  tagline: 스켈레탈 메시와 스태틱 메시를 위한 Unreal Engine 5.6~5.8 오픈 소스 소프트바디 플러그인(MIT)
  image:
    src: /logo.png
    alt: MallangPhysics
  actions:
    - theme: brand
      text: 빠른 시작
      link: /ko/Quick-Start
    - theme: alt
      text: 설치
      link: /ko/Installation
    - theme: alt
      text: 영상
      link: https://www.youtube.com/watch?v=n5HpN2XGicA

showcase:
  title: 기능
  items:
    - title: 관성
      link: /ko/Inertia
      details: 몸이 움직이거나 돌거나 착지하면 살이 뒤처졌다가 출렁이며 제자리로 돌아옵니다. 부위마다 크기와 리듬을 따로 주고, 덜 움직일 곳은 칠해서 정합니다.
      media:
        - src: images/Inertia/jiggle-walk.webp
          alt: Simulation 탭에서 걷기 애니메이션을 돌리고 메시를 끌어 본 출렁임
    - title: 접촉
      link: /ko/Contact
      details: 손과 물체가 누른 자리는 들어가고 둘레의 살은 솟습니다. 미끄러지는 손은 피부를 끌고 가고, 넓게 밀면 본에 매달린 살 덩어리가 통째로 밀려납니다. 캐릭터는 자기 손으로 제 몸을 누르고, 온몸으로 다른 Softbody 메시를 누릅니다.
      media:
        - src: images/Contact/hand-and-ball.png
          alt: 말랑한 공을 쥔 손가락과 옆을 누르는 원뿔
        - src: images/Characters/whole-body-press.png
          alt: Softbody 방석에 앉은 캐릭터
    - title: 성능
      link: /ko/Performance
      details: 접촉은 무언가 닿아 있는 동안에만 GPU에서 계산합니다. 눌린 채 멈추면 갱신을 쉬고, 작게 보이거나 화면 밖인 메시도 쉬며, 군중은 출렁임만 남길 수 있습니다.
      media:
        - src: images/Home/performance.png
          alt: 한 장면에 놓인 여러 Softbody 메시
    - title: 편의성
      link: /ko/Quick-Start
      details: 에디터에서 한 번 베이크하고 컴포넌트 하나만 붙이면 됩니다. 베이크 결과는 베이크 에셋 창의 Preview와 Simulation 탭에서 플레이하지 않고 확인합니다. 스켈레탈 메시와 스태틱 메시 모두, 나나이트 테셀레이션, 몸에 붙은 옷, KawaiiPhysics 본과 함께 쓸 수 있고, 머티리얼도 베이크가 만들어 줍니다.
      media:
        - src: images/Home/usability.png
          alt: 베이크 설정 옆에 단단한 코어를 보여 주는 베이크 에셋 창
---
