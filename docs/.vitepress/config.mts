import { existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { defineConfig, type DefaultTheme, type MarkdownRenderer } from 'vitepress'
import { editServer, sourceLines } from './edit'
import { dirOf, type Engine } from './theme/engines'

// GitHub Pages 프로젝트 사이트 주소(wjsekrud.github.io/MallangPhysics/)에 맞춘 경로
const base = '/MallangPhysics/'
const SITE = 'https://wjsekrud.github.io/MallangPhysics/'

// 쪽은 엔진·언어마다 한 벌이다. 영어 쪽은 맨 위, 한국어 쪽은 /ko/ 아래에 같은 파일 이름으로 두고,
// Unity 쪽은 그 아래 unity/에 둔다: UE = /X, /ko/X, Unity = /unity/X, /ko/unity/X (theme/engines.ts)
type Lang = 0 | 1 // 0 = 영어, 1 = 한국어
const langs = ['en', 'ko'] as const
const prefix = (e: Engine, l: Lang) => '/' + dirOf(e, langs[l])

const titles: Record<string, [string, string]> = {
  'Overview': ['Overview', '개요'],
  'Installation': ['Installation', '설치'],
  'Quick-Start': ['Quick Start', '빠른 시작'],
  'Concepts': ['Concepts', '개념'],
  'Baking': ['Baking', '베이크'],
  'Preview-and-Simulation': ['Preview and Simulation', '미리보기와 시뮬레이션'],
  'Painting': ['Painting', '페인트'],
  'Characters': ['Characters', '캐릭터'],
  'Materials': ['Materials', '머티리얼'],
  'Colliders': ['Colliders', '콜라이더'],
  'Softbody-Component': ['Softbody Component', 'Softbody 컴포넌트'],
  'Contact': ['Contact', '접촉'],
  'Inertia': ['Inertia', '관성'],
  'Performance': ['Performance', '성능'],
  'Console-Reference': ['Console Reference', '콘솔 레퍼런스'],
  'Project-Settings': ['Project Settings', '프로젝트 설정'],
  'Troubleshooting': ['Troubleshooting', '문제 해결'],
  'Known-Limitations': ['Known Limitations', '알려진 제한'],
  'How-It-Works': ['How It Works', '작동 원리'],
  'Demo-Project': ['Demo Project', '데모 프로젝트'],
  'Credits-and-License': ['Credits and License', '크레딧과 라이선스'],
}

// 사이드바. 엔진마다 따로 둔다
const groups: Record<Engine, [[string, string], string[]][]> = {
  ue: [
    [['Getting started', '시작하기'], ['Overview', 'Installation', 'Quick-Start', 'Concepts']],
    [['Setting up a mesh', '메시 준비'], ['Baking', 'Preview-and-Simulation', 'Painting', 'Characters', 'Materials', 'Colliders']],
    [['Runtime', '런타임'], ['Softbody-Component', 'Contact', 'Inertia', 'Performance', 'Console-Reference']],
    [['Help', '도움말'], ['Troubleshooting', 'Known-Limitations', 'How-It-Works']],
    [['Project', '프로젝트'], ['Demo-Project', 'Credits-and-License']],
  ],
  unity: [
    [['Getting started', '시작하기'], ['Overview', 'Installation', 'Quick-Start', 'Concepts']],
    [['Setting up a mesh', '메시 준비'], ['Baking', 'Painting', 'Characters', 'Materials', 'Colliders']],
    [['Runtime', '런타임'], ['Softbody-Component', 'Inertia', 'Performance', 'Project-Settings']],
    [['Help', '도움말'], ['Troubleshooting', 'Known-Limitations', 'How-It-Works']],
    [['Project', '프로젝트'], ['Credits-and-License']],
  ],
}

// 위 메뉴. link는 쪽 하나, items는 펼치는 목록
const menus: Record<Engine, { text: [string, string]; link?: string; items?: string[] }[]> = {
  ue: [
    { text: ['Get started', '시작하기'], link: 'Installation' },
    { text: ['Guide', '가이드'], items: ['Baking', 'Preview-and-Simulation', 'Painting', 'Characters', 'Materials', 'Colliders'] },
    { text: ['Reference', '레퍼런스'], items: ['Softbody-Component', 'Console-Reference', 'How-It-Works'] },
    { text: ['Help', '도움말'], link: 'Troubleshooting' },
    { text: ['Demo', '데모'], link: 'Demo-Project' },
  ],
  unity: [
    { text: ['Get started', '시작하기'], link: 'Installation' },
    { text: ['Guide', '가이드'], items: ['Baking', 'Painting', 'Characters', 'Materials', 'Colliders'] },
    { text: ['Reference', '레퍼런스'], items: ['Softbody-Component', 'Project-Settings', 'How-It-Works'] },
    { text: ['Help', '도움말'], link: 'Troubleshooting' },
  ],
}

const item = (e: Engine, l: Lang, page: string) => ({ text: titles[page][l], link: prefix(e, l) + page })

// 한국어 쪽을 먼저 쓰고 영어는 나중에 맞춘다. 아직 그 엔진·언어로 없는 쪽은 메뉴에서 빼고, 쪽이 하나도 없는 묶음도 뺀다
const root = fileURLToPath(new URL('..', import.meta.url))
// 사이트에서 뺀 폴더(예: 번역 전 초안 'unity/'). 파일은 그대로 두고, 빠진 쪽은 메뉴·엔진 단추·언어 메뉴가 '없음'으로 본다
const drafts: string[] = []
const has = (e: Engine, l: Lang, page: string) =>
  !drafts.includes(dirOf(e, langs[l])) && existsSync(root + dirOf(e, langs[l]) + page + '.md')

const sidebar = (e: Engine, l: Lang): DefaultTheme.SidebarItem[] =>
  groups[e]
    .map(([group, pages]) => ({ text: group[l], items: pages.filter((p) => has(e, l, p)).map((p) => item(e, l, p)) }))
    .filter((g) => g.items.length > 0)

const nav = (e: Engine, l: Lang): DefaultTheme.NavItem[] =>
  menus[e].flatMap(({ text, link, items = [] }) => {
    if (link) return has(e, l, link) ? [{ text: text[l], link: prefix(e, l) + link }] : []
    const list = items.filter((p) => has(e, l, p)).map((p) => item(e, l, p))
    return list.length > 0 ? [{ text: text[l], items: list }] : []
  })

// 엔진 단추와 언어 메뉴가 갈 쪽을 고를 때 쓰는 '있는 쪽' 목록(theme/EngineSwitch.vue, theme/langs.ts).
// 'Baking', 'ko/unity/Baking' 꼴이고 첫 화면은 '', 'ko/', 'unity/', 'ko/unity/'.
// 서버를 띄운 뒤 새로 생긴 쪽은 다시 띄울 때까지 목록에 없다(그동안은 있는 쪽이나 첫 화면으로 간다)
const enginePages = (['ue', 'unity'] as Engine[]).flatMap((e) =>
  ([0, 1] as Lang[]).flatMap((l) => ['index', ...Object.keys(titles)].filter((p) => has(e, l, p)).map((p) => dirOf(e, langs[l]) + (p === 'index' ? '' : p))),
)

// 한국어가 정본이고 영어 쪽은 번역이 일부만 있을 수 있다(지금 영어 Unity 쪽).
// 영어 쪽에서 같은 폴더의 쪽으로 건 링크가 아직 영어로 없고 한국어로 있으면 한국어 판으로 잇는다.
// 깨진 링크 대신 한국어 쪽이 열리고, 빌드의 죽은 링크 검사도 지나간다. 영어 쪽이 생기면 저절로 영어로 간다
const untranslatedLinks = (md: MarkdownRenderer) => {
  md.core.ruler.push('untranslated_links', (state) => {
    const rel: string = (state.env as { relativePath?: string })?.relativePath ?? ''
    if (rel.startsWith('ko/')) return
    const e: Engine = rel.startsWith('unity/') ? 'unity' : 'ue'
    for (const block of state.tokens) {
      for (const t of block.children ?? []) {
        if (t.type !== 'link_open') continue
        const m = (t.attrGet('href') ?? '').match(/^(?:\.\/)?([A-Za-z0-9-]+)(?:\.md)?(#.*)?$/)
        if (m && !has(e, 0, m[1]) && has(e, 1, m[1])) t.attrSet('href', prefix(e, 1) + m[1] + (m[2] ?? ''))
      }
    }
  })
}

const description: Record<Engine, [string, string]> = {
  ue: [
    'Open-source soft body plugin for Unreal Engine 5.6–5.8 (MIT): real-time dents, bulges, skin drag, lumps, self contact and jiggle physics on skeletal and static meshes.',
    'Unreal Engine 5.6~5.8용 오픈 소스 소프트바디 플러그인(MIT): 스켈레탈 메시와 스태틱 메시의 실시간 눌림, 솟음, 끌림, 덩어리, 자기 접촉, 출렁임.',
  ],
  // FULL 11-06: Full(솟음·끌림)이 Unity 베타에 들어가면 여기에도 더한다
  unity: [
    'Real-time soft flesh for Unity 6.3 URP (beta): dents, lumps and jiggle on skinned and static meshes.',
    'Unity 6.3 URP용 실시간 말랑한 살(베타): 스킨드 메시와 스태틱 메시의 눌림, 덩어리, 출렁임.',
  ],
}

// 한국어 화면 글자. UE 쪽과 Unity 쪽이 같이 쓴다
const koLabels: DefaultTheme.Config = {
  docFooter: { prev: '이전', next: '다음' },
  darkModeSwitchLabel: '테마',
  lightModeSwitchTitle: '밝은 테마로',
  darkModeSwitchTitle: '어두운 테마로',
  sidebarMenuLabel: '메뉴',
  returnToTopLabel: '맨 위로',
  langMenuLabel: '언어',
  skipToContentLabel: '본문으로 건너뛰기',
  notFound: {
    title: '페이지를 찾을 수 없습니다',
    quote: '주소가 바뀌었거나 없는 페이지입니다.',
    linkLabel: '홈으로',
    linkText: '홈으로',
  },
}

// locale은 엔진·언어마다 하나다. 그래서 사이드바·위 메뉴·설명·검색 색인이 엔진마다 따로 생긴다.
// 언어 메뉴는 theme/langs.ts가 같은 엔진의 locale끼리만 잇는다(/unity/X ↔ /ko/unity/X)
const locale = (e: Engine, l: Lang) => ({
  label: ['English', '한국어'][l],
  lang: ['en-US', 'ko-KR'][l],
  link: prefix(e, l),
  title: 'MallangPhysics',
  // 브라우저 탭에서도 두 엔진의 같은 이름 쪽이 헷갈리지 않게
  ...(e === 'unity' ? { titleTemplate: ':title | MallangPhysics for Unity' } : {}),
  description: description[e][l],
  themeConfig: {
    nav: nav(e, l),
    sidebar: sidebar(e, l),
    outline: { level: [2, 3] as [number, number], label: ['On this page', '이 페이지에서'][l] },
    ...(l === 1 ? koLabels : {}),
  },
})

const koSearch = {
  translations: {
    button: { buttonText: '검색', buttonAriaLabel: '검색' },
    modal: {
      displayDetails: '자세히 보기',
      resetButtonTitle: '검색어 지우기',
      backButtonTitle: '닫기',
      noResultsText: '결과가 없습니다',
      footer: {
        selectText: '선택',
        selectKeyAriaLabel: 'Enter',
        navigateText: '이동',
        navigateUpKeyAriaLabel: '위 화살표',
        navigateDownKeyAriaLabel: '아래 화살표',
        closeText: '닫기',
        closeKeyAriaLabel: 'Esc',
      },
    },
  },
}

// 기본 테마 부품 둘을 바꿔 끼운다(VitePress 문서의 'Overriding Internal Components' 방법, 1.6.4 기준).
// 제목 줄 → 엔진 단추를 제목 링크 바깥에 둔다. 언어 메뉴 → 같은 엔진의 locale만 보인다
const override = (file: string) => fileURLToPath(new URL(`./theme/${file}`, import.meta.url))

export default defineConfig<DefaultTheme.Config & { enginePages?: string[] }>({
  base,
  cleanUrls: true,
  // 검색 엔진용 사이트 지도(sitemap.xml). Search Console에 제출한다
  sitemap: { hostname: SITE },
  srcExclude: ['README.md', ...drafts.map((d) => d + '**')],
  head: [['link', { rel: 'icon', type: 'image/png', href: `${base}favicon.png` }]],
  // 링크를 붙여 넣었을 때 뜨는 카드(제목·설명·그림)와 검색 엔진용 정식 주소
  transformHead: ({ pageData, title, description }) => {
    const url = SITE + pageData.relativePath.replace(/(^|\/)index\.md$/, '$1').replace(/\.md$/, '')
    return [
      ['link', { rel: 'canonical', href: url }],
      ['meta', { property: 'og:type', content: 'website' }],
      ['meta', { property: 'og:site_name', content: 'MallangPhysics' }],
      ['meta', { property: 'og:title', content: title }],
      ['meta', { property: 'og:description', content: description }],
      ['meta', { property: 'og:url', content: url }],
      ['meta', { property: 'og:image', content: `${SITE}og.png` }],
      ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
    ]
  },

  // 영어 번역이 없는 쪽으로 건 링크는 한국어 판으로(위 untranslatedLinks).
  // npm run dev에서만 한국어 쪽을 브라우저로 고친다(theme/editor.ts). 빌드에는 넣지 않는다
  markdown: {
    config: (md) => {
      untranslatedLinks(md)
      if (!process.argv.includes('build')) sourceLines(md)
    },
  },
  vite: {
    plugins: [editServer(root)],
    resolve: {
      alias: [
        { find: /^.*\/VPNavBarTitle\.vue$/, replacement: override('EngineNavBarTitle.vue') },
        { find: /^\.{1,2}\/composables\/langs$/, replacement: override('langs.ts') },
      ],
    },
  },

  // 순서가 뜻을 가진다: VitePress는 위에서부터 '/<이름>/'이 주소에 들어 있는 locale을 고른다.
  // 'ko/unity'가 'unity'·'ko'보다 먼저 와야 /ko/unity/X가 한국어 Unity 쪽이 된다
  locales: {
    root: locale('ue', 0),
    'ko/unity': locale('unity', 1),
    unity: locale('unity', 0),
    ko: locale('ue', 1),
  },

  themeConfig: {
    logo: '/logo.png',
    socialLinks: [{ icon: 'github', link: 'https://github.com/wjsekrud/MallangPhysics' }],
    // 검색 색인도 locale마다 따로라서, 지금 엔진·언어의 쪽만 찾는다
    search: {
      provider: 'local',
      options: { locales: { ko: koSearch, 'ko/unity': koSearch } },
    },
    enginePages, // 이 테마만 쓰는 값(theme/EngineSwitch.vue)
  },
})
