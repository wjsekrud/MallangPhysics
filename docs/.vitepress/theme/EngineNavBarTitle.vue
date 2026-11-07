<script setup lang="ts">
// 기본 테마의 VPNavBarTitle(왼쪽 위 로고와 사이트 제목) 대신 쓴다(config.mts의 vite.resolve.alias가 바꿔 끼운다).
// 기본 슬롯 nav-bar-title-after는 제목 링크 <a> 안에 그려져서, 거기 엔진 단추를 넣으면 링크 안에 링크가 생기고
// 단추를 눌러도 첫 화면 링크가 같이 눌린다. 그래서 제목 링크는 VitePress 1.6.4 원본 그대로 그리고, 엔진 단추를 그 바깥 옆에 둔다.
// 제목 링크는 지금 locale의 첫 화면으로 간다: UE 쪽은 /, /ko/, Unity 쪽은 /unity/, /ko/unity/.
import { computed } from 'vue'
import { useData } from 'vitepress'
import VPImage from 'vitepress/dist/client/theme-default/components/VPImage.vue'
import { useSidebar } from 'vitepress/dist/client/theme-default/composables/sidebar'
import { normalizeLink } from 'vitepress/dist/client/theme-default/support/utils'
import { useLangs } from './langs'
import EngineSwitch from './EngineSwitch.vue'

const { site, theme } = useData()
const { hasSidebar } = useSidebar()
const { currentLang } = useLangs()

const link = computed(() => (typeof theme.value.logoLink === 'string' ? theme.value.logoLink : theme.value.logoLink?.link))
const rel = computed(() => (typeof theme.value.logoLink === 'string' ? undefined : theme.value.logoLink?.rel))
const target = computed(() => (typeof theme.value.logoLink === 'string' ? undefined : theme.value.logoLink?.target))
</script>

<template>
  <div class="VPNavBarTitle" :class="{ 'has-sidebar': hasSidebar }">
    <a class="title" :href="link ?? normalizeLink(currentLang.link)" :rel="rel" :target="target">
      <slot name="nav-bar-title-before" />
      <VPImage v-if="theme.logo" class="logo" :image="theme.logo" />
      <span v-if="theme.siteTitle" v-html="theme.siteTitle"></span>
      <span v-else-if="theme.siteTitle === undefined">{{ site.title }}</span>
      <slot name="nav-bar-title-after" />
    </a>
    <EngineSwitch class="engines" />
  </div>
</template>

<style scoped>
.VPNavBarTitle {
  display: flex;
  align-items: center;
  border-bottom: 1px solid transparent;
  height: var(--vp-nav-height);
}

.title {
  display: flex;
  align-items: center;
  height: 100%;
  font-size: 16px;
  font-weight: 600;
  color: var(--vp-c-text-1);
  transition: opacity 0.25s;
}

.engines {
  margin-left: 8px;
}

@media (min-width: 960px) {
  .title {
    flex-shrink: 0;
  }

  /* 사이드바가 있는 쪽은 제목 칸 아래에 선을 긋는다(원본과 같은 자리, 사이드바 글자 폭만큼) */
  .VPNavBarTitle.has-sidebar {
    border-bottom-color: var(--vp-c-divider);
  }
}

:deep(.logo) {
  margin-right: 8px;
  height: var(--vp-nav-logo-height);
}

/* 제목 줄에 엔진 단추와 검색·메뉴 단추가 한 줄로 다 들어가지 않는 폭에서는 사이트 이름 글자를 화면에서만 숨긴다.
   로고는 남고, 읽기 프로그램에는 링크 이름으로 그대로 읽힌다.
   373px 이하 = 좁은 전화기. 768~899px = 위 메뉴가 펼쳐지는 태블릿 폭(영어 UE 쪽 메뉴 다섯 개가 896px까지 넘친다) */
@media (max-width: 373px), (min-width: 768px) and (max-width: 899px) {
  .title > span {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
  }

  :deep(.logo) {
    margin-right: 0;
  }
}
</style>
