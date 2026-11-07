// 로컬 미리보기(npm run dev)에서 한국어 쪽을 브라우저로 고친다. 빌드에는 들어가지 않는다.
// 오른쪽 아래 [편집]을 켜고 문단·제목·목록 항목·표 줄을 누르면 그 자리의 마크다운 원문이 열린다.
// 저장하면 ko/*.md 파일에 바로 쓰고, 화면은 저절로 다시 그려진다. 서버 쪽은 ../edit.ts.
import { watch } from 'vue'
import type { Router } from 'vitepress'

const CSS = `
.wiki-edit-bar { position: fixed; right: 16px; bottom: 16px; z-index: 100; display: flex; gap: 8px; }
.wiki-edit-bar[hidden] { display: none; } /* 영어 쪽에서는 숨긴다(위 display: flex가 hidden을 덮지 않게) */
.wiki-edit-bar button, .wiki-edit-box button {
  font: 500 13px/1 var(--vp-font-family-base); padding: 9px 12px; border-radius: 8px; cursor: pointer;
  border: 1px solid var(--vp-c-divider); background: var(--vp-c-bg-elv); color: var(--vp-c-text-1);
}
.wiki-edit-bar button.on, .wiki-edit-box button.save { background: var(--vp-c-brand-1); border-color: transparent; color: var(--vp-c-white); }
html.wiki-editing .vp-doc [data-src] { cursor: text; }
html.wiki-editing .vp-doc [data-src]:hover:not(:has([data-src]:hover)) {
  outline: 2px dashed var(--vp-c-brand-1); outline-offset: 3px; border-radius: 2px;
}
.wiki-edit-box {
  position: absolute; z-index: 90; display: flex; flex-direction: column; gap: 6px; padding: 8px;
  background: var(--vp-c-bg); border: 1px solid var(--vp-c-brand-1); border-radius: 10px; box-shadow: var(--vp-shadow-3);
}
.wiki-edit-box.whole { position: fixed; top: calc(var(--vp-nav-height) + 8px); right: 16px; bottom: 64px; width: min(760px, 52vw); }
.wiki-edit-head { display: flex; justify-content: space-between; gap: 12px; font-size: 12px; color: var(--vp-c-text-2); }
.wiki-edit-msg { color: var(--vp-c-danger-1); }
.wiki-edit-box textarea {
  width: 100%; resize: vertical; overflow: hidden; padding: 8px 10px; border: 0; border-radius: 6px; outline: none;
  font: 15px/1.7 var(--vp-font-family-base); color: var(--vp-c-text-1); background: var(--vp-c-bg-soft);
}
.wiki-edit-box.whole textarea { flex: 1; resize: none; overflow: auto; font: 13px/1.6 var(--vp-font-family-mono); }
.wiki-edit-actions { display: flex; justify-content: flex-end; gap: 8px; }
.wiki-edit-toast {
  position: fixed; right: 16px; bottom: 64px; z-index: 100; padding: 8px 12px; border-radius: 8px; font-size: 13px;
  background: var(--vp-c-bg-elv); border: 1px solid var(--vp-c-divider); box-shadow: var(--vp-shadow-2);
}
`

type Range = { start?: number; end?: number }

const make = <K extends keyof HTMLElementTagNameMap>(tag: K, cls = '', text = '') => {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text) e.textContent = text
  return e
}

async function call(op: 'read' | 'write', data: object): Promise<{ text?: string; error?: string }> {
  try {
    const r = await fetch(`/__wiki_edit/${op}`, { method: 'POST', body: JSON.stringify(data) })
    return await r.json()
  } catch (e) {
    return { error: `미리보기 서버에 닿지 않습니다 (${e})` }
  }
}

export function installEditor(router: Router) {
  document.head.append(make('style', '', CSS))

  const file = () => String(router.route.data.relativePath || '')
  const isKo = () => file().startsWith('ko/')
  let on = localStorage.getItem('wiki-edit') === '1'

  const bar = make('div', 'wiki-edit-bar')
  const wholeBtn = make('button', '', '쪽 전체')
  const toggle = make('button')
  bar.append(wholeBtn, toggle)
  document.body.append(bar)

  let box: HTMLElement | null = null
  let dirty = false
  let flash = (_: string) => {}

  const refresh = () => {
    bar.hidden = !isKo()
    wholeBtn.hidden = !on
    toggle.textContent = on ? '편집 끄기' : '편집'
    toggle.classList.toggle('on', on)
    document.documentElement.classList.toggle('wiki-editing', on && isKo())
  }

  const toast = (text: string) => {
    const t = make('div', 'wiki-edit-toast', text)
    document.body.append(t)
    setTimeout(() => t.remove(), 2000)
  }

  const close = () => {
    box?.remove()
    box = null
    dirty = false
  }

  async function open(range: Range, anchor?: HTMLElement) {
    const f = file()
    const got = await call('read', { file: f, ...range })
    if (got.error !== undefined) return toast(got.error)
    const before = got.text ?? ''
    close()

    box = make('div', anchor ? 'wiki-edit-box' : 'wiki-edit-box whole')
    const name = f.slice(3)
    const where =
      range.start === undefined
        ? `${name} 전체`
        : range.end! - range.start > 1
          ? `${name} ${range.start + 1}–${range.end}줄`
          : `${name} ${range.start + 1}줄`
    const msg = make('span', 'wiki-edit-msg')
    const head = make('div', 'wiki-edit-head')
    head.append(make('span', '', where), msg)
    const ta = make('textarea')
    ta.value = before
    ta.spellcheck = false
    ta.rows = 1
    const save = make('button', 'save', '저장 (Ctrl+Enter)')
    const cancel = make('button', '', '취소 (Esc)')
    const actions = make('div', 'wiki-edit-actions')
    actions.append(cancel, save)
    box.append(head, ta, actions)
    document.body.append(box)

    let escOnce = false
    flash = (text: string) => {
      msg.textContent = text
      ta.focus()
    }
    const fit = () => {
      if (!anchor) return
      ta.style.height = 'auto'
      ta.style.height = ta.scrollHeight + 'px'
    }
    if (anchor) {
      const r = anchor.getBoundingClientRect()
      const width = Math.min(Math.max(r.width + 16, 480), document.documentElement.clientWidth - 32)
      const left = Math.max(16, Math.min(r.left - 8, document.documentElement.clientWidth - width - 16))
      box.style.top = `${r.top + window.scrollY - 8}px`
      box.style.left = `${left + window.scrollX}px`
      box.style.width = `${width}px`
    }
    fit()
    ta.focus()
    if (!anchor) {
      ta.setSelectionRange(0, 0)
      ta.scrollTop = 0
    }

    const doSave = async () => {
      const r = await call('write', { file: f, ...range, before, text: ta.value })
      if (r.error !== undefined) return flash(r.error)
      close()
      toast('저장했습니다')
    }
    ta.addEventListener('input', () => {
      dirty = ta.value !== before
      escOnce = false
      msg.textContent = ''
      fit()
    })
    ta.addEventListener('keydown', (e) => {
      if ((e.ctrlKey || e.metaKey) && (e.key === 'Enter' || e.key === 's')) {
        e.preventDefault()
        doSave()
      } else if (e.key === 'Escape') {
        e.preventDefault()
        if (!dirty || escOnce) close()
        else {
          escOnce = true
          flash('한 번 더 Esc를 누르면 고친 내용을 버립니다')
        }
      }
    })
    save.addEventListener('click', doSave)
    cancel.addEventListener('click', close)
  }

  toggle.addEventListener('click', () => {
    if (on && dirty) return flash('저장하거나 취소한 뒤 끄세요')
    on = !on
    localStorage.setItem('wiki-edit', on ? '1' : '0')
    if (!on) close()
    refresh()
  })
  wholeBtn.addEventListener('click', () => {
    if (dirty) return flash('저장하거나 취소한 뒤 여세요')
    open({})
  })

  // 편집 중에는 쪽 안의 링크도 따라가지 않고 그 블록을 연다
  document.addEventListener(
    'click',
    (e) => {
      if (!on || !isKo()) return
      const t = e.target as HTMLElement
      if (box?.contains(t) || bar.contains(t)) return
      const block = t.closest<HTMLElement>('.vp-doc [data-src]')
      if (!block) return
      e.preventDefault()
      e.stopPropagation()
      if (dirty) return flash('저장하거나 취소한 뒤 다른 곳을 여세요')
      const [start, end] = block.dataset.src!.split(',').map(Number)
      open({ start, end }, block)
    },
    true,
  )

  const prev = router.onBeforeRouteChange
  router.onBeforeRouteChange = async (to) => {
    if (dirty) {
      flash('저장하거나 취소한 뒤 옮기세요')
      return false
    }
    close()
    return prev?.(to)
  }
  window.addEventListener('beforeunload', (e) => {
    if (dirty) e.preventDefault()
  })

  watch(() => router.route.data.relativePath, refresh, { immediate: true })
}
