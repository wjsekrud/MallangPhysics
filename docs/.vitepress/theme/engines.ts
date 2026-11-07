// 엔진 갈래. 한 사이트 안에 Unreal Engine 쪽과 Unity 쪽을 따로 두고, 한 쪽 안에 두 엔진을 섞지 않는다.
// UE 쪽은 맨 위(X.md, ko/X.md), Unity 쪽은 unity/ 아래(unity/X.md, ko/unity/X.md)에 둔다.
// config.mts(사이드바·메뉴·locale)와 테마(엔진 단추·언어 메뉴)가 이 파일을 같이 쓴다.

export type Engine = 'ue' | 'unity'
export type Lang = 'en' | 'ko'

// 엔진 단추에 쓰는 이름. logo가 없으면 글자(short)로 그린다.
// 로고를 쓰게 되면 public/engines/에 밝은 화면용·어두운 화면용 파일을 두고 경로를 적는다(색을 바꾸지 않고 파일을 바꾼다)
export const engines: Record<Engine, { label: string; short: string; logo?: { light: string; dark: string } }> = {
  ue: { label: 'Unreal Engine', short: 'UE' },
  unity: { label: 'Unity (Beta)', short: 'Unity' },
}

// 쪽 파일이 있는 폴더(사이트 안 경로, 앞 / 없음): '', 'ko/', 'unity/', 'ko/unity/'
export const dirOf = (e: Engine, l: Lang) => (l === 'ko' ? 'ko/' : '') + (e === 'unity' ? 'unity/' : '')

// locale 이름 → 엔진. locale은 엔진·언어마다 하나다: root, ko, unity, ko/unity
export const localeEngine = (key: string): Engine => (key === 'unity' || key === 'ko/unity' ? 'unity' : 'ue')

// 엔진을 바꿀 때 이름이 다른 짝. 여기 없으면 같은 이름 쪽으로, 그것도 없으면 그 엔진의 첫 화면으로 간다
export const pairs: Record<Engine, Record<string, string>> = {
  ue: { 'Console-Reference': 'Project-Settings', 'Preview-and-Simulation': 'Baking', 'Demo-Project': 'Quick-Start' },
  unity: { 'Project-Settings': 'Console-Reference' },
}

// 'ko/unity/Baking.md' → { engine: 'unity', lang: 'ko', page: 'Baking' }. 첫 화면은 page가 ''
export function parsePath(relativePath: string) {
  let p = relativePath.replace(/\.md$/, '')
  const lang: Lang = p.startsWith('ko/') ? 'ko' : 'en'
  if (lang === 'ko') p = p.slice(3)
  const engine: Engine = p.startsWith('unity/') ? 'unity' : 'ue'
  if (engine === 'unity') p = p.slice(6)
  return { engine, lang, page: p === 'index' ? '' : p }
}

// 지금 쪽에서 엔진 단추를 누르면 갈 주소(base 없이).
// pages = 있는 쪽 목록('Baking', 'ko/unity/Baking' 꼴, 첫 화면은 '', 'ko/', 'unity/', 'ko/unity/'). 빌드 때 config.mts가 만든다.
// 고르는 차례: 같은 언어의 그 쪽 → 다른 언어의 그 쪽(영어 번역이 아직 없는 Unity 쪽) → 같은 언어의 첫 화면 → 다른 언어의 첫 화면
export function switchTarget(relativePath: string, to: Engine, pages: readonly string[]): string {
  const { engine, lang, page } = parsePath(relativePath)
  if (engine === to) return '/' + dirOf(to, lang) + page
  const name = pairs[engine][page] ?? page
  const other: Lang = lang === 'ko' ? 'en' : 'ko'
  const found = [dirOf(to, lang) + name, dirOf(to, other) + name, dirOf(to, lang), dirOf(to, other)].find((p) => pages.includes(p))
  return '/' + (found ?? dirOf(to, lang))
}

// 같은 쪽의 다른 언어 판이 있는지(언어 메뉴가 없는 쪽으로 가지 않게, theme/langs.ts)
export function hasTranslation(relativePath: string, to: Lang, pages: readonly string[]): boolean {
  const { engine, page } = parsePath(relativePath)
  return pages.includes(dirOf(engine, to) + page)
}
