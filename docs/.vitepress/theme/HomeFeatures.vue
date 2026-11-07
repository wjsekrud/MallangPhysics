<script setup lang="ts">
// 첫 화면 아래 Features. index.md 머리말의 showcase(제목, 항목마다 제목·설명·쪽 링크·그림)를
// 그림 + 글 한 줄씩으로 그린다. showcase가 없는 첫 화면(Unity 쪽)에는 아무것도 그리지 않는다.
import { computed } from 'vue'
import { useData } from 'vitepress'
import { VPLink } from 'vitepress/theme'
import FeatureMedia from './FeatureMedia.vue'

type Item = { title: string; details: string; link?: string; media: { src: string; alt: string }[] }

// 그림 경로는 위키 images/ 기준(예: images/Inertia/jiggle-walk.webp)이라 쪽과 같은 파일을 쓴다
const files = import.meta.glob('../../images/**/*.{png,webp,gif,jpg}', { eager: true, query: '?url', import: 'default' }) as Record<string, string>
const url = (p: string) => files['../../' + p.replace(/^\/+/, '')] ?? ''

const { frontmatter, lang } = useData()
const showcase = computed(() => frontmatter.value.showcase as { title: string; items: Item[] } | undefined)
const ko = computed(() => lang.value.startsWith('ko'))
</script>

<template>
  <section v-if="showcase" class="HomeFeatures">
    <div class="container">
      <h2 class="heading">{{ showcase.title }}</h2>
      <div v-for="item in showcase.items" :key="item.title" class="row">
        <FeatureMedia
          :slides="item.media.map((m) => ({ src: url(m.src), alt: m.alt }))"
          :labels="ko ? { prev: '이전 그림', next: '다음 그림', slide: '그림' } : { prev: 'Previous picture', next: 'Next picture', slide: 'Picture' }"
        />
        <div class="text">
          <h3 class="title">
            <VPLink v-if="item.link" :href="item.link" no-icon>{{ item.title }}</VPLink>
            <template v-else>{{ item.title }}</template>
          </h3>
          <p class="details">{{ item.details }}</p>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
/* 히어로와 갈라 보이게 바탕색이 다른 띠로 둔다(그라데이션 없음) */
.HomeFeatures {
  margin-top: 48px;
  border-top: 1px solid var(--vp-c-divider);
  background-color: var(--vp-c-bg-soft);
  padding: 0 24px 48px;
}
@media (min-width: 640px) {
  .HomeFeatures { padding: 0 48px 64px; }
}
@media (min-width: 960px) {
  .HomeFeatures { margin-top: 64px; padding: 0 64px 80px; }
}

.container {
  max-width: 1152px;
  margin: 0 auto;
}

.heading {
  margin: 48px 0 8px;
  text-align: center;
  font-size: 28px;
  line-height: 36px;
  font-weight: 700;
}
@media (min-width: 768px) {
  .heading { margin-top: 72px; font-size: 32px; line-height: 40px; }
}

.row {
  display: grid;
  gap: 20px;
  padding: 32px 0;
}
@media (min-width: 960px) {
  .row {
    grid-template-columns: 2fr 1fr;
    gap: 48px;
    align-items: center;
    padding: 40px 0;
  }
}

.text {
  text-align: center;
}

.title {
  font-size: 22px;
  line-height: 30px;
  font-weight: 700;
  text-wrap: balance;
}
.title :deep(a) {
  color: var(--vp-c-text-1);
  transition: color 0.25s;
}
.title :deep(a:hover) {
  color: var(--vp-c-brand-1);
}

.details {
  margin-top: 12px;
  font-size: 15px;
  line-height: 26px;
  color: var(--vp-c-text-2);
  text-wrap: pretty;
}
</style>
