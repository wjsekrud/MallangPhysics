// 로컬 미리보기(npm run dev)에서 한국어 쪽을 브라우저로 고치는 기능의 서버 쪽.
// 빌드(npm run build)에는 들어가지 않는다. 화면 쪽은 theme/editor.ts.
import { readFile, writeFile } from 'node:fs/promises'
import type { IncomingMessage } from 'node:http'
import type { Plugin } from 'vite'
import type { MarkdownRenderer } from 'vitepress'

// 원본 줄 번호를 붙일 블록. 표는 줄마다, 목록은 항목마다 고친다
const BLOCKS = new Set(['paragraph_open', 'heading_open', 'list_item_open', 'tr_open', 'blockquote_open'])

// 각 블록에 원본 파일의 줄 범위(data-src="시작,끝", 0부터, 끝은 빼고)를 단다
export function sourceLines(md: MarkdownRenderer) {
  const render = md.render.bind(md)
  md.render = (src: string, env: any = {}) => {
    env.wikiSrc = src
    return render(src, env)
  }
  md.core.ruler.push('wiki_source_lines', (state) => {
    const env = state.env as any
    if (!env?.relativePath?.startsWith('ko/') || typeof env.wikiSrc !== 'string') return
    // 머리말(---)을 뗀 본문만 파싱하므로, 뗀 줄 수만큼 더한다
    const full = env.wikiSrc.replace(/\r\n?/g, '\n')
    if (!full.endsWith(state.src)) return
    const off = full.slice(0, full.length - state.src.length).split('\n').length - 1
    for (const t of state.tokens) {
      if (t.map && BLOCKS.has(t.type)) t.attrSet('data-src', `${t.map[0] + off},${t.map[1] + off}`)
    }
  })
}

const body = (req: IncomingMessage) =>
  new Promise<string>((ok, fail) => {
    let s = ''
    req.setEncoding('utf8')
    req.on('data', (c) => (s += c))
    req.on('end', () => ok(s))
    req.on('error', fail)
  })

// POST /__wiki_edit/read  { file, start?, end? }              → { text }
// POST /__wiki_edit/write { file, start?, end?, before, text } → { ok }
// start·end가 없으면 파일 전체. before가 지금 파일과 다르면 쓰지 않는다(그사이 바뀐 파일을 덮지 않게)
export function editServer(root: string): Plugin {
  return {
    name: 'wiki-edit',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use('/__wiki_edit', async (req, res) => {
        const reply = (status: number, data: object) => {
          res.statusCode = status
          res.setHeader('Content-Type', 'application/json; charset=utf-8')
          res.end(JSON.stringify(data))
        }
        try {
          if (req.method !== 'POST') return reply(405, { error: 'POST만 받습니다' })
          const q = JSON.parse(await body(req))
          // 한국어 쪽만: UE 쪽 ko/*.md, Unity 쪽 ko/unity/*.md
          if (typeof q.file !== 'string' || !/^ko\/(unity\/)?[A-Za-z0-9-]+\.md$/.test(q.file))
            return reply(400, { error: '한국어 쪽(ko/*.md, ko/unity/*.md)만 고칠 수 있습니다' })
          const path = root + q.file
          const raw = await readFile(path, 'utf8')
          const eol = raw.includes('\r\n') ? '\r\n' : '\n'
          const lines = raw.split(/\r?\n/)
          const whole = q.start === undefined
          const start = whole ? 0 : Number(q.start)
          const end = whole ? lines.length : Number(q.end)
          if (!(start >= 0 && end >= start && end <= lines.length)) return reply(400, { error: '줄 범위가 맞지 않습니다' })
          const now = lines.slice(start, end).join('\n')

          if (req.url?.startsWith('/read')) return reply(200, { text: now })
          if (!req.url?.startsWith('/write')) return reply(404, { error: '없는 주소입니다' })

          if (q.before !== now) return reply(409, { error: '그사이 파일이 바뀌었습니다. 닫고 다시 여세요' })
          const next = String(q.text).replace(/\r\n?/g, '\n')
          const repl = next === '' && !whole ? [] : next.split('\n')
          lines.splice(start, end - start, ...repl)
          // 블록을 통째로 지웠으면 앞뒤 빈 줄이 겹치지 않게 하나를 뺀다
          if (repl.length === 0 && start > 0 && lines[start - 1] === '' && lines[start] === '') lines.splice(start, 1)
          await writeFile(path, lines.join(eol), 'utf8')
          return reply(200, { ok: true })
        } catch (e) {
          return reply(500, { error: String(e) })
        }
      })
    },
  }
}
