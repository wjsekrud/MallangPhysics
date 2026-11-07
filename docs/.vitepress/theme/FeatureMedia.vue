<script setup lang="ts">
// Features 한 줄의 그림 칸. 그림이 둘 이상이면 넘겨 보는 칸이 된다: 첫 그림만 보이고,
// 화살표·점·밀기(터치, 가로 스크롤)로 다음 그림을 본다
import { ref } from 'vue'

const props = defineProps<{
  slides: { src: string; alt: string }[]
  labels: { prev: string; next: string; slide: string }
}>()

const track = ref<HTMLElement>()
const index = ref(0)

function go(i: number) {
  const el = track.value
  if (!el) return
  const n = props.slides.length
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches
  el.scrollTo({ left: ((i + n) % n) * el.clientWidth, behavior: reduce ? 'auto' : 'smooth' })
}

function onScroll() {
  const el = track.value
  if (el && el.clientWidth) index.value = Math.round(el.scrollLeft / el.clientWidth)
}
</script>

<template>
  <div class="FeatureMedia">
    <div class="frame">
      <div ref="track" class="track" @scroll.passive="onScroll">
        <div v-for="(s, i) in slides" :key="s.src" class="slide" :aria-hidden="slides.length > 1 && i !== index">
          <img :src="s.src" :alt="s.alt" :loading="i ? 'lazy' : undefined" decoding="async" />
        </div>
      </div>
      <template v-if="slides.length > 1">
        <button class="arrow prev" type="button" :aria-label="labels.prev" @click="go(index - 1)">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M15 5l-7 7 7 7" /></svg>
        </button>
        <button class="arrow next" type="button" :aria-label="labels.next" @click="go(index + 1)">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M9 5l7 7-7 7" /></svg>
        </button>
      </template>
    </div>
    <div v-if="slides.length > 1" class="dots">
      <button
        v-for="(s, i) in slides"
        :key="s.src"
        type="button"
        class="dot"
        :class="{ active: i === index }"
        :aria-label="`${labels.slide} ${i + 1} / ${slides.length}`"
        :aria-current="i === index"
        @click="go(i)"
      />
    </div>
  </div>
</template>

<style scoped>
.FeatureMedia {
  min-width: 0;
}

.frame {
  position: relative;
}

.track {
  display: flex;
  overflow-x: auto;
  scroll-snap-type: x mandatory;
  scrollbar-width: none;
  overscroll-behavior-x: contain;
}
.track::-webkit-scrollbar {
  display: none;
}

.slide {
  flex: 0 0 100%;
  scroll-snap-align: start;
  display: flex;
  align-items: center;
  justify-content: center;
  height: clamp(220px, 58vw, 460px);
}

.slide img {
  display: block;
  max-width: 100%;
  max-height: 100%;
  object-fit: contain;
  border-radius: 12px;
  /* 어두운 화면에서 어두운 에디터 사진이 바탕에 묻히지 않게 얇은 테두리 */
  box-shadow: 0 0 0 1px var(--vp-c-divider);
}

.arrow {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  display: flex;
  align-items: center;
  justify-content: center;
  width: 40px;
  height: 40px;
  border-radius: 50%;
  background: rgba(0, 0, 0, 0.45);
  color: #fff;
  transition: background 0.2s;
}
.arrow:hover {
  background: rgba(0, 0, 0, 0.65);
}
.arrow:focus-visible,
.dot:focus-visible {
  outline: 2px solid var(--vp-c-brand-1);
  outline-offset: 2px;
}
.arrow svg {
  width: 22px;
  height: 22px;
  fill: none;
  stroke: currentColor;
  stroke-width: 2.4;
  stroke-linecap: round;
  stroke-linejoin: round;
}
.prev { left: 8px; }
.next { right: 8px; }

.dots {
  display: flex;
  justify-content: center;
  gap: 10px;
  margin-top: 12px;
}
.dot {
  width: 9px;
  height: 9px;
  border-radius: 50%;
  background: var(--vp-c-text-3);
  opacity: 0.5;
  transition: opacity 0.2s, background 0.2s;
}
.dot.active {
  background: var(--vp-c-brand-1);
  opacity: 1;
}
</style>
