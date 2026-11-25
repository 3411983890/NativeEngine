import { readFile, writeFile, mkdir, cp, rm } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const contentDir = path.join(here, 'content');
const assetDir = path.join(here, 'assets');
const outDir = path.join(here, 'dist');
const repoRoot = path.resolve(here, '..', '..');

const esc = (s) =>
  String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

const stripTags = (s) => s.replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim();

const nav = JSON.parse(await readFile(path.join(here, 'nav.json'), 'utf8'));
const version = (await readFile(path.join(repoRoot, 'VERSION'), 'utf8')).trim();
const flat = nav.flatMap((g) => g.items.map((it) => ({ ...it, group: g.group })));

function extractToc(html) {
  const toc = [];
  let i = 0;
  const out = html.replace(/<h([23])([^>]*)>([\s\S]*?)<\/h\1>/g, (m, level, attrs, inner) => {
    const text = stripTags(inner);
    let id = /id="([^"]+)"/.exec(attrs)?.[1];
    if (!id) {
      i += 1;
      id = `sec-${i}`;
      attrs = `${attrs} id="${id}"`;
    }
    toc.push({ level: Number(level), id, text });
    return `<h${level}${attrs}>${inner}</h${level}>`;
  });
  return { html: out, toc };
}

function navHtml(currentSlug) {
  return nav
    .map((group) => {
      const items = group.items
        .map((it) => {
          const active = it.slug === currentSlug;
          return `<a class="nav-link${active ? ' is-active' : ''}" href="${it.slug}.html"${
            active ? ' aria-current="page"' : ''
          }>${esc(it.title)}</a>`;
        })
        .join('\n');
      return `<div class="nav-group"><p class="nav-group-title">${esc(group.group)}</p>${items}</div>`;
    })
    .join('\n');
}

function pagerHtml(idx) {
  const prev = flat[idx - 1];
  const next = flat[idx + 1];
  const card = (p, dir) =>
    p
      ? `<a class="pager-card pager-${dir}" href="${p.slug}.html"><span class="pager-dir">${
          dir === 'prev' ? '上一页' : '下一页'
        }</span><span class="pager-title">${esc(p.title)}</span></a>`
      : '<span class="pager-card pager-empty"></span>';
  return `<nav class="pager" aria-label="翻页">${card(prev, 'prev')}${card(next, 'next')}</nav>`;
}

function tocHtml(toc) {
  if (!toc.length) return '';
  const items = toc
    .map(
      (h) =>
        `<a class="toc-link toc-l${h.level}" href="#${h.id}">${esc(h.text)}</a>`
    )
    .join('\n');
  return `<p class="toc-title">本页目录</p>${items}`;
}

function shell({ page, body, toc, idx }) {
  const tocBlock = tocHtml(toc);
  return `<!doctype html>
<html lang="zh-CN" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(page.title)} · NativeGPU 开发文档</title>
<meta name="description" content="${esc(page.desc || '')}">
<meta name="color-scheme" content="dark light">
<link rel="stylesheet" href="assets/style.css">
<script>try{var t=localStorage.getItem('ng-docs-theme');if(t){document.documentElement.dataset.theme=t}}catch(e){}</script>
</head>
<body>
<a class="skip-link" href="#main">跳到正文</a>
<header class="topbar">
  <button class="menu-button" id="menu-button" aria-label="打开导航" aria-expanded="false" aria-controls="sidebar">
    <span></span><span></span><span></span>
  </button>
  <a class="brand" href="index.html">
    <span class="brand-mark" aria-hidden="true">N<span>G</span></span>
    <span class="brand-text">NativeGPU<small>开发文档</small></span>
  </a>
  <span class="version-chip">v${esc(version)}</span>
  <div class="topbar-spacer"></div>
  <div class="search" role="search">
    <svg class="search-icon" viewBox="0 0 16 16" aria-hidden="true"><circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M10.5 10.5L14 14" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/></svg>
    <input id="search-input" type="search" placeholder="搜索文档…" aria-label="搜索文档" autocomplete="off" spellcheck="false">
    <kbd>Ctrl K</kbd>
    <div class="search-results" id="search-results" hidden></div>
  </div>
  <button class="icon-button" id="theme-toggle" aria-label="切换主题" title="切换明暗主题">
    <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 2.2v1.4M8 12.4v1.4M2.2 8h1.4M12.4 8h1.4M3.9 3.9l1 1M11.1 11.1l1 1M12.1 3.9l-1 1M4.9 11.1l-1 1" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/><circle cx="8" cy="8" r="2.6" fill="none" stroke="currentColor" stroke-width="1.3"/></svg>
  </button>
</header>
<div class="layout">
  <aside class="sidebar" id="sidebar">
    <nav aria-label="文档导航">
${navHtml(page.slug)}
    </nav>
    <div class="sidebar-foot">
      <span class="dot"></span> 文档随代码同步维护
    </div>
  </aside>
  <div class="scrim" id="scrim" hidden></div>
  <main class="main" id="main">
    <div class="crumbs"><a href="index.html">文档</a><span aria-hidden="true">/</span><span>${esc(
      page.group
    )}</span><span aria-hidden="true">/</span><span class="crumb-current">${esc(page.title)}</span></div>
    <article class="prose">
      <h1>${esc(page.title)}</h1>
${body}
    </article>
${pagerHtml(idx)}
    <footer class="page-foot">
      <span>NativeGPU ${esc(version)}</span>
      <span>本页内容与 <code>docs/</code> 下的设计文档同源维护</span>
    </footer>
  </main>
  <aside class="toc-rail" aria-label="本页目录">
    <div class="toc" id="toc">${tocBlock}</div>
  </aside>
</div>
<script src="assets/search-index.js"></script>
<script src="assets/app.js"></script>
</body>
</html>
`;
}

await rm(outDir, { recursive: true, force: true });
await mkdir(path.join(outDir, 'assets'), { recursive: true });

const index = [];

for (let i = 0; i < flat.length; i += 1) {
  const page = flat[i];
  const raw = await readFile(path.join(contentDir, `${page.slug}.html`), 'utf8');
  const { html, toc } = extractToc(raw);
  const body = html
    .trim()
    .replace(/<table>/g, '<div class="table-wrap"><table>')
    .replace(/<\/table>/g, '</table></div>');
  await writeFile(path.join(outDir, `${page.slug}.html`), shell({ page, body, toc, idx: i }), 'utf8');

  index.push({
    slug: page.slug,
    title: page.title,
    group: page.group,
    desc: page.desc || '',
    headings: toc.map((t) => t.text),
    text: stripTags(body).slice(0, 1200),
  });
}

await cp(assetDir, path.join(outDir, 'assets'), { recursive: true });
await writeFile(
  path.join(outDir, 'assets', 'search-index.js'),
  `window.NG_SEARCH = ${JSON.stringify(index)};\n`,
  'utf8'
);

console.log(`docs: ${flat.length} pages -> ${path.relative(repoRoot, outDir)}`);
