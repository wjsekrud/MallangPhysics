// 기본 테마의 composables/langs 대신 쓴다(config.mts의 vite.resolve.alias가 바꿔 끼운다).
// locale이 엔진·언어마다 하나라서(root, ko, unity, ko/unity), 기본 그대로면 언어 메뉴에 '한국어'가 두 번 뜨고
// 하나는 다른 엔진 쪽으로 간다. 그래서 같은 엔진의 locale만 남긴다.
// 또 그 언어 판이 아직 없는 쪽(영어 번역이 없는 Unity 쪽)에서는 그 언어를 메뉴에서 뺀다(없는 주소로 가지 않게).
// 이 둘 말고는 VitePress 1.6.4의 원본과 같다.
import { computed } from 'vue'
import { useData } from 'vitepress'
import { hasTranslation, localeEngine } from './engines'

export function useLangs({ correspondingLink = false } = {}) {
  const { site, localeIndex, page, theme, hash } = useData()
  const currentLang = computed(() => ({
    label: site.value.locales[localeIndex.value]?.label,
    link: site.value.locales[localeIndex.value]?.link || (localeIndex.value === 'root' ? '/' : `/${localeIndex.value}/`),
  }))

  const translated = (key: string) => {
    const pages = (theme.value as { enginePages?: string[] }).enginePages
    return !pages || hasTranslation(page.value.relativePath, key.startsWith('ko') ? 'ko' : 'en', pages)
  }

  const localeLinks = computed(() =>
    Object.entries(site.value.locales).flatMap(([key, value]) =>
      currentLang.value.label === value.label || localeEngine(key) !== localeEngine(localeIndex.value) || !translated(key)
        ? []
        : {
            text: value.label,
            link:
              normalizeLink(
                value.link || (key === 'root' ? '/' : `/${key}/`),
                theme.value.i18nRouting !== false && correspondingLink,
                page.value.relativePath.slice(currentLang.value.link.length - 1),
                !site.value.cleanUrls,
              ) + hash.value,
          },
    ),
  )

  return { localeLinks, currentLang }
}

function normalizeLink(link: string, addPath: boolean, path: string, addExt: boolean) {
  if (!addPath) return link
  const p = path.replace(/(^|\/)index\.md$/, '$1').replace(/\.md$/, addExt ? '.html' : '')
  return link.replace(/\/$/, '') + (p.startsWith('/') ? p : `/${p}`)
}
