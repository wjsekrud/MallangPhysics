import { h } from 'vue'
import DefaultTheme from 'vitepress/theme'
import type { Theme } from 'vitepress'
// 한국어 쪽 글꼴. 글자 범위별로 나뉜 파일이라 쪽에 쓰인 글자의 파일만 받는다
import 'pretendard/dist/web/variable/pretendardvariable-dynamic-subset.css'
import './style.css'
import HomeFeatures from './HomeFeatures.vue'
import SiteFooter from './SiteFooter.vue'

export default {
  extends: DefaultTheme,
  // 첫 화면 히어로 아래 Features(그림 + 글 줄, index.md 머리말의 showcase)와 모든 쪽 맨 아래 링크 묶음
  Layout: () =>
    h(DefaultTheme.Layout, null, { 'home-features-after': () => h(HomeFeatures), 'layout-bottom': () => h(SiteFooter) }),
  enhanceApp({ router }) {
    // npm run dev에서만 한국어 쪽을 브라우저로 고친다. 빌드에는 들어가지 않는다
    if (import.meta.env.DEV && typeof window !== 'undefined') import('./editor').then((m) => m.installEditor(router))
  },
} satisfies Theme
