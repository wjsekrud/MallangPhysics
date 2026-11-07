<script setup lang="ts">
// 왼쪽 위 엔진 단추 둘. 누르면 지금 쪽에 맞는 다른 엔진의 쪽으로 간다(같은 언어, 짝 표는 engines.ts).
// 지금 엔진은 바탕을 채워 진하게, 다른 엔진은 흐리게 그리고 마우스를 올리면 진해진다.
import { computed } from 'vue'
import { useData, withBase } from 'vitepress'
import { engines, parsePath, switchTarget, type Engine } from './engines'

const { page, theme } = useData()

const here = computed(() => parsePath(page.value.relativePath))
const items = computed(() => {
  const pages: string[] = (theme.value as { enginePages?: string[] }).enginePages ?? []
  return (Object.keys(engines) as Engine[]).map((key) => ({
    key,
    ...engines[key],
    current: key === here.value.engine,
    href: withBase(switchTarget(page.value.relativePath, key, pages)),
  }))
})
const groupLabel = computed(() => (here.value.lang === 'ko' ? '엔진' : 'Engine'))
</script>

<template>
  <div class="EngineSwitch" role="group" :aria-label="groupLabel">
    <a
      v-for="e in items"
      :key="e.key"
      class="engine"
      :class="{ current: e.current }"
      :href="e.href"
      :title="e.label"
      :aria-label="e.label"
      :aria-current="e.current ? 'true' : undefined"
      @click="e.current && $event.preventDefault()"
    >
      <template v-if="e.logo">
        <img class="logo light" :src="withBase(e.logo.light)" alt="" />
        <img class="logo dark" :src="withBase(e.logo.dark)" alt="" />
      </template>
      <span v-else>{{ e.short }}</span>
    </a>
  </div>
</template>

<style scoped>
.EngineSwitch {
  display: flex;
  flex-shrink: 0;
  align-items: center;
  padding: 2px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 8px;
}

.engine {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 24px;
  padding: 0 6px;
  border-radius: 6px;
  font-size: 12px;
  font-weight: 500;
  line-height: 24px;
  /* 흐린 쪽도 글자 대비 4.5 이상(밝은 화면 5.7, 어두운 화면 6.6)인 text-2 */
  color: var(--vp-c-text-2);
  transition: color 0.25s, background-color 0.25s;
}

.engine:hover {
  color: var(--vp-c-text-1);
}

.engine.current {
  font-weight: 600;
  color: var(--vp-c-text-1);
  background-color: var(--vp-c-default-soft);
  cursor: default;
}

.engine:focus-visible {
  outline: 2px solid var(--vp-c-brand-1);
  outline-offset: 1px;
}

/* 로고를 쓸 때: 밝은 화면·어두운 화면용 파일을 바꿔 끼운다. 흐린 쪽은 투명도로 */
.logo {
  height: 16px;
  opacity: 0.5;
  transition: opacity 0.25s;
}
.engine:hover .logo,
.engine.current .logo {
  opacity: 1;
}
.logo.dark,
:global(.dark) .logo.light {
  display: none;
}
:global(.dark) .logo.dark {
  display: block;
}
</style>
