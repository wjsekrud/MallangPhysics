---
layout: home
title: MallangPhysics
titleTemplate: Soft flesh for Unreal Engine 5.6–5.8

hero:
  name: MallangPhysics
  tagline: An open-source softbody plugin for skeletal and static meshes in Unreal Engine 5.6–5.8 (MIT)
  image:
    src: /logo.png
    alt: MallangPhysics
  actions:
    - theme: brand
      text: Quick Start
      link: /Quick-Start
    - theme: alt
      text: Installation
      link: /Installation
    - theme: alt
      text: Video
      link: https://www.youtube.com/watch?v=n5HpN2XGicA

showcase:
  title: Features
  items:
    - title: Inertia
      link: /Inertia
      details: Flesh lags behind when the body moves, turns or lands, then swings back and settles. Give each body part its own size and rhythm, and paint where flesh should move less.
      media:
        - src: images/Inertia/jiggle-walk.webp
          alt: Jiggle in the Simulation tab, with a walk animation playing and the mesh dragged around
    - title: Contact
      link: /Contact
      details: Hands and objects dent the skin, and the flesh around them bulges. A sliding hand drags the skin, and a wide push moves a whole lump of flesh aside. A character presses its own body with its hands, and other Softbody meshes with its whole body.
      media:
        - src: images/Contact/hand-and-ball.png
          alt: Fingers squeezing a soft ball while a cone presses its side
        - src: images/Characters/whole-body-press.png
          alt: A character sitting on a Softbody cushion
    - title: Performance
      link: /Performance
      details: Contact runs on the GPU only while something touches the mesh. A pressed mesh that stops moving stops updating, small or off-screen meshes pause, and crowds can keep only the jiggle.
      media:
        - src: images/Home/performance.png
          alt: Many Softbody meshes in one scene
    - title: Ease of use
      link: /Quick-Start
      details: Bake once in the editor and add one component. Check the bake in the Preview and Simulation tabs of the authoring asset window without playing. Works on skeletal and static meshes, with Nanite tessellation, clothing on the skin and KawaiiPhysics bones, and the bake can make the material.
      media:
        - src: images/Home/usability.png
          alt: The authoring asset window showing the rigid core beside the bake settings
---
