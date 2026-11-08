<script setup lang="ts">
// 모든 쪽 맨 아래의 링크 묶음과 저작권 줄. 문서 링크는 지금 보는 엔진·언어의 쪽으로 간다.
// 사이드바가 있는 쪽에서는 본문과 같은 자리에 둔다(사이드바가 화면 왼쪽을 덮고 있어서)
import { computed } from 'vue'
import { useData } from 'vitepress'
import { VPLink, useSidebar } from 'vitepress/theme'

const REPO = 'https://github.com/wjsekrud/MallangPhysics'

const { lang, page } = useData()
const { hasSidebar } = useSidebar()
const ko = computed(() => lang.value.startsWith('ko'))
const unity = computed(() => /^(ko\/)?unity\//.test(page.value.relativePath))
const docs = (p: string) => (ko.value ? '/ko/' : '/') + (unity.value ? 'unity/' : '') + p

const columns = computed(() => {
  const k = ko.value
  return [
    {
      title: k ? '문서' : 'Docs',
      links: [
        { text: k ? '빠른 시작' : 'Quick Start', href: docs('Quick-Start') },
        { text: k ? '설치' : 'Installation', href: docs('Installation') },
        { text: k ? '알려진 제한' : 'Known Limitations', href: docs('Known-Limitations') },
        { text: k ? '문제 해결' : 'Troubleshooting', href: docs('Troubleshooting') },
        { text: k ? '크레딧과 라이선스' : 'Credits and License', href: docs('Credits-and-License') },
      ],
    },
    {
      title: k ? '문의' : 'Contact',
      links: [
        { text: k ? '버그 신고' : 'Report Issues', href: `${REPO}/issues` },
        { text: k ? '메일 문의' : 'Contact Me', href: 'mailto:ivrpt2141@gmail.com' },
      ],
    },
    {
      title: k ? '참고' : 'References',
      links: [
        { text: 'KawaiiPhysics (pafuhana1213)', href: 'https://github.com/pafuhana1213/KawaiiPhysics' },
        { text: 'Quaternius', href: 'https://quaternius.com/' },
      ],
    },
    {
      title: k ? '더 보기' : 'More',
      links: [
        { text: 'GitHub', href: REPO },
        { text: k ? '릴리스' : 'Releases', href: `${REPO}/releases` },
        unity.value
          ? { text: k ? 'Unreal Engine 문서' : 'Unreal Engine docs', href: k ? '/ko/' : '/' }
          : { text: k ? 'Unity 문서(베타)' : 'Unity docs (beta)', href: k ? '/ko/unity/' : '/unity/' },
      ],
    },
  ]
})
</script>

<template>
  <footer class="SiteFooter" :class="{ 'has-sidebar': hasSidebar }">
    <div class="container">
      <div class="columns">
        <nav v-for="col in columns" :key="col.title" class="column" :aria-label="col.title">
          <p class="title">{{ col.title }}</p>
          <ul>
            <li v-for="l in col.links" :key="l.href">
              <VPLink class="link" :href="l.href">{{ l.text }}</VPLink>
            </li>
          </ul>
        </nav>
      </div>
      <p class="copyright">Code released under the MIT License. Copyright © 2026 wjsekrud.</p>
    </div>
  </footer>
</template>

<style scoped>
.SiteFooter {
  position: relative;
  border-top: 1px solid var(--vp-c-gutter);
  background-color: var(--vp-c-bg);
  padding: 40px 24px 32px;
}
@media (min-width: 768px) {
  .SiteFooter { padding: 48px 48px 32px; }
}
/* VPContent.has-sidebar와 같은 왼쪽 자리 */
@media (min-width: 960px) {
  .SiteFooter.has-sidebar { padding-left: calc(var(--vp-sidebar-width) + 48px); }
}
@media (min-width: 1440px) {
  .SiteFooter.has-sidebar {
    padding-left: calc((100vw - var(--vp-layout-max-width)) / 2 + var(--vp-sidebar-width) + 48px);
    padding-right: calc((100vw - var(--vp-layout-max-width)) / 2 + 48px);
  }
}

.container {
  max-width: 1152px;
  margin: 0 auto;
}

.columns {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 28px 32px;
}
@media (min-width: 768px) {
  .columns { grid-template-columns: repeat(4, minmax(0, 1fr)); }
}

.title {
  margin-bottom: 8px;
  font-size: 14px;
  line-height: 24px;
  font-weight: 600;
  color: var(--vp-c-text-1);
}

.link {
  font-size: 14px;
  line-height: 28px;
  color: var(--vp-c-text-2);
  transition: color 0.25s;
}
.link:hover {
  color: var(--vp-c-brand-1);
}

.copyright {
  margin-top: 36px;
  text-align: center;
  font-size: 13px;
  line-height: 24px;
  color: var(--vp-c-text-2);
}
</style>
