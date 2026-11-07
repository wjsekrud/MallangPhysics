# MallangPhysics docs site

The documentation in English and Korean, built with [VitePress](https://vitepress.dev). Every `.md` file here is one English page; `ko/` holds the Korean page with the same file name. The sidebar, top menu and the Korean labels are in `.vitepress/config.mts`. Screenshots go in `images/<Page>/` and are shared by both languages (Korean pages link them as `../images/...`; the list is in `../wiki-prep/wiki-images.md`; the files there now are placeholders).

## Two engines

Each engine has its own set of pages; one page never covers both. Unreal Engine pages are the files above (`/X`, `/ko/X`); Unity pages are in `unity/` and `ko/unity/` with the same file names (`/unity/X`, `/ko/unity/X`). Each engine and language is its own VitePress locale (`root`, `ko`, `unity`, `ko/unity`), so the sidebar, top menu, description and search index are separate per engine, and the language menu switches `/unity/X` ↔ `/ko/unity/X`. Unity images go in `images/unity/<Page>/`.

The English Unity pages (`unity/`) are drafts and are left out of the site until they are translated: `drafts` in `.vitepress/config.mts` excludes them from the build, menus, buttons and language menu. When the translation is done, remove `'unity/'` from `drafts`. Korean is the source and a page may exist in one language only, so every switch checks which files exist. The **UE | Unity** buttons next to the site title open the matching page of the other engine (same file name, or the pair below): in the same language if that file exists, else in the other language, else that engine's home page. The language menu leaves out a language the page has no file in. On an English page, a link to a page of the same folder that exists only in Korean is pointed at the Korean page at build time (`untranslatedLinks` in the config), so it opens and the dead-link check passes; once the English file exists the link goes there.

| Unreal Engine | Unity |
|---|---|
| Console-Reference | Project-Settings |
| Preview-and-Simulation | Baking (one way) |
| Demo-Project | Quick-Start (one way) |

To add a page: put the file in the engine's folders, add its titles to `titles` and its place to `groups` (sidebar) and `menus` (top menu) for that engine in `.vitepress/config.mts`, and add a pair to `pairs` in `.vitepress/theme/engines.ts` if the other engine has the same topic under another name. A page appears in the menus only once its file exists. The list of pages for the buttons is made when the server starts, so restart `npm run dev` after adding a page.

Two default-theme parts are swapped in through `vite.resolve.alias` (VitePress 1.6.4): `theme/EngineNavBarTitle.vue` replaces the title bar (the default slot would put the buttons inside the title link) and `theme/langs.ts` keeps the language menu to one engine. Check both when updating VitePress. At widths where the bar is full (373 px and below, 768–899 px) the site name is hidden on screen and kept for screen readers.

Korean headings carry the anchor of the English heading (`## 접촉 맵 {#contact-map}`), so a link such as `Colliders#recommended-setup` works the same in both languages. When you rename an English heading, change the anchor on the Korean heading too.

```
npm install
npm run dev       # live preview at http://localhost:5173/MallangPhysics/ (Korean: /MallangPhysics/ko/)
npm run build     # static site in .vitepress/dist (fails on a dead link, not on a missing anchor)
npm run preview   # serve the built site
```

While `npm run dev` runs, Korean pages can be edited in the browser: turn on **편집** at the bottom right, then click a paragraph, heading, list item or table row to edit its Markdown and save with Ctrl+Enter (**쪽 전체** opens the whole file). It writes straight to `ko/*.md` or `ko/unity/*.md` and the page redraws. The server side is `.vitepress/edit.ts`, the page side `.vitepress/theme/editor.ts`; neither is in the build.

The site is meant for GitHub Pages at `/MallangPhysics/` (the `base` in the config). GitHub Pages needs a public repository on a free account.
