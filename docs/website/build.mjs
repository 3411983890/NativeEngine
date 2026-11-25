import { readFile, writeFile, mkdir, rm, cp, stat } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { groups, docs as manifestDocs, featured } from './manifest.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const outDir = path.join(here, 'dist');
const themeDir = path.join(here, 'theme');
const buildDate = new Date();
const SITE_NAME = 'NativeEngine';

const BRAND_GLYPH =
  '<path d="M260 282 Q268 256 300 256 H392 Q416 256 432 276 L764 742 Q756 768 724 768 H632 Q608 768 592 748 Z"/><path d="M256 306 L404 514 V704 Q404 728 382 734 L280 764 Q256 770 256 744 Z"/><path d="M256 306 L404 514 V704 Q404 728 382 734 L280 764 Q256 770 256 744 Z" transform="rotate(180 512 512)"/>';

const esc = (s) =>
  String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const attr = (s) => esc(s).replace(/"/g, '&quot;');
const norm = (s) => s.replace(/\\/g, '/');
const stripTags = (s) =>
  s
    .replace(/<script[\s\S]*?<\/script>/gi, ' ')
    .replace(/<style[\s\S]*?<\/style>/gi, ' ')
    .replace(/<[^>]+>/g, ' ')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/\s+/g, ' ')
    .trim();

const stripInline = (s) =>
  String(s)
    .replace(/!\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
    .replace(/`/g, '')
    .replace(/[*_~]/g, '')
    .replace(/\s+/g, ' ')
    .trim();

const version = (await readFile(path.join(root, 'VERSION'), 'utf8')).trim();
let commit = '';
try {
  commit = execFileSync('git', ['rev-parse', '--short', 'HEAD'], {
    cwd: root,
    encoding: 'utf8',
  }).trim();
} catch {
  commit = '';
}

const groupById = new Map(groups.map((g) => [g.id, g]));

const docs = [];
for (const doc of manifestDocs) {
  try {
    await stat(path.join(root, doc.file));
    docs.push(doc);
  } catch {
    console.warn(`docs site: skip missing ${doc.file}`);
  }
}

const docByFile = new Map(docs.map((d) => [norm(d.file).toLowerCase(), d]));
const docBySlug = new Map(docs.map((d) => [d.slug, d]));
const flat = groups.flatMap((g) => docs.filter((d) => d.group === g.id));

/* ---------------------------------------------------------------- markdown */

const CJK = /[\u3000-\u303f\u3400-\u9fff\uff00-\uffef]/;

function joinLines(lines) {
  let out = '';
  for (const raw of lines) {
    const line = raw.trim();
    if (!line) continue;
    if (!out) {
      out = line;
      continue;
    }
    const last = out[out.length - 1];
    const first = line[0];
    out += CJK.test(last) || CJK.test(first) ? '' : ' ';
    out += line;
  }
  return out;
}

function indentOf(line) {
  return (/^ */.exec(line) || [''])[0].length;
}

function matchListItem(line) {
  const m = /^( *)([-*+]|\d+[.)])([ \t]+)(.*)$/.exec(line);
  if (!m) return null;
  return {
    indent: m[1].length,
    marker: m[2],
    ordered: /^\d/.test(m[2]),
    contentIndent: m[1].length + m[2].length + m[3].length,
    content: m[4],
  };
}

function splitRow(line) {
  let s = line.trim();
  if (s.startsWith('|')) s = s.slice(1);
  if (s.endsWith('|')) s = s.slice(0, -1);
  const cells = [];
  let cur = '';
  let inCode = false;
  for (let i = 0; i < s.length; i += 1) {
    const ch = s[i];
    if (ch === '\\' && s[i + 1] === '|') {
      cur += '|';
      i += 1;
      continue;
    }
    if (ch === '`') inCode = !inCode;
    if (ch === '|' && !inCode) {
      cells.push(cur);
      cur = '';
    } else {
      cur += ch;
    }
  }
  cells.push(cur);
  return cells.map((c) => c.trim());
}

function isTableSeparator(line) {
  if (!line || !line.includes('-')) return false;
  const cells = splitRow(line);
  return cells.length > 0 && cells.every((c) => /^:?-{2,}:?$/.test(c.replace(/\s/g, '')));
}

function slugifyHeading(text) {
  return (
    stripInline(text)
      .toLowerCase()
      .replace(/[^\p{L}\p{N}\s_-]/gu, '')
      .trim()
      .replace(/[\s_]+/g, '-')
      .replace(/-+/g, '-') || 'section'
  );
}

function makeContext(file) {
  return { file, headings: [], usedIds: new Map() };
}

function headingId(ctx, text) {
  const base = slugifyHeading(text);
  let id = base;
  let n = 2;
  while (ctx.usedIds.has(id)) {
    id = `${base}-${n}`;
    n += 1;
  }
  ctx.usedIds.set(id, true);
  return id;
}

function resolveHref(href, file, image = false) {
  if (!href) return { kind: 'plain' };
  const raw = href.trim();
  if (/^(https?:|mailto:|tel:)/i.test(raw)) return { kind: 'external', href: raw };
  if (raw.startsWith('#')) return { kind: 'hash', href: raw };
  if (/^(data:|javascript:)/i.test(raw)) return { kind: 'plain' };

  const hashIndex = raw.indexOf('#');
  const hash = hashIndex >= 0 ? raw.slice(hashIndex) : '';
  let target = hashIndex >= 0 ? raw.slice(0, hashIndex) : raw;
  target = target.split('?')[0];
  if (!target) return { kind: 'hash', href: hash || '#' };

  try {
    target = decodeURIComponent(target);
  } catch {
    /* keep raw */
  }

  const baseDir = path.posix.dirname(norm(file));
  const resolved = path.posix
    .normalize(target.startsWith('/') ? target.slice(1) : path.posix.join(baseDir, target))
    .replace(/^\.\//, '');

  if (image) {
    if (resolved.startsWith('docs/')) return { kind: 'image', href: resolved.slice(5) };
    return { kind: 'image', href: resolved };
  }

  const key = resolved.toLowerCase();
  const found = docByFile.get(key);
  if (found) return { kind: 'internal', href: `${found.slug}.html${hash}` };

  if (/\.md$/i.test(resolved)) {
    const byName = docByFile.get(path.posix.basename(key));
    if (byName) return { kind: 'internal', href: `${byName.slug}.html${hash}` };
  }

  return { kind: 'file', href: resolved, labelPath: resolved };
}

function inline(text, ctx) {
  let out = esc(text);
  const store = [];
  const put = (html) => {
    store.push(html);
    return `\u0000${store.length - 1}\u0000`;
  };

  out = out.replace(/`([^`\n]+)`/g, (m, code) => put(`<code>${code}</code>`));

  out = out.replace(
    /!\[([^\]]*)\]\(([^)\s]+)(?:\s+"([^"]*)")?\)/g,
    (m, alt, src, title) => {
      const r = resolveHref(src, ctx.file, true);
      if (r.kind !== 'image') return put(attr(alt));
      const titleAttr = title ? ` title="${attr(title)}"` : '';
      return put(
        `<img class="doc-image" src="${attr(r.href)}" alt="${attr(alt)}"${titleAttr} loading="lazy">`
      );
    }
  );

  out = out.replace(
    /\[([^\]]+)\]\(([^)\s]+)(?:\s+"([^"]*)")?\)/g,
    (m, label, href, title) => {
      const r = resolveHref(href, ctx.file);
      const labelHtml = inlineLabel(label, ctx).replace(
        /\u0000(\d+)\u0000/g,
        (token, n) => store[Number(n)] ?? token
      );
      const titleAttr = title ? ` title="${attr(title)}"` : '';
      if (r.kind === 'external')
        return put(
          `<a href="${attr(r.href)}" target="_blank" rel="noreferrer"${titleAttr}>${labelHtml}</a>`
        );
      if (r.kind === 'internal' || r.kind === 'hash')
        return put(`<a href="${attr(r.href)}"${titleAttr}>${labelHtml}</a>`);
      if (r.kind === 'file')
        return put(
          `<span class="file-ref" title="仓库文件：${attr(r.labelPath)}">${labelHtml}</span>`
        );
      return put(labelHtml);
    }
  );

  out = out.replace(/&lt;((?:https?:\/\/|mailto:)[^&\s]+)&gt;/g, (m, url) =>
    put(`<a href="${attr(url)}" target="_blank" rel="noreferrer">${esc(url)}</a>`)
  );

  out = out.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
  out = out.replace(/(^|[^\w])__([^_\n]+)__(?!\w)/g, '$1<strong>$2</strong>');
  out = out.replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, '$1<em>$2</em>');
  out = out.replace(/(^|[^\w])_([^_\n]+)_(?!\w)/g, '$1<em>$2</em>');
  out = out.replace(/~~([^~\n]+)~~/g, '<del>$1</del>');

  out = out.replace(/\u0000(\d+)\u0000/g, (m, n) => store[Number(n)] ?? m);
  return out;
}

function inlineLabel(text, ctx) {
  let out = esc(text);
  out = out.replace(/`([^`\n]+)`/g, '<code>$1</code>');
  out = out.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
  out = out.replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, '$1<em>$2</em>');
  return out;
}

function renderCode(code, lang) {
  const label = (lang || 'text').trim();
  return `<figure class="code-block" data-lang="${attr(label)}"><div class="code-bar"><span>${esc(
    label
  )}</span><button class="copy-code" type="button" aria-label="复制代码">复制</button></div><pre><code class="language-${attr(
    label
  )}">${esc(code)}</code></pre></figure>`;
}

function isBlockStart(line, lines, i) {
  if (!line.trim()) return true;
  if (/^ {0,3}(#{1,6})[ \t]+/.test(line)) return true;
  if (/^ {0,3}(`{3,}|~{3,})/.test(line)) return true;
  if (/^ {0,3}((\*\s*){3,}|(-\s*){3,}|(_\s*){3,})$/.test(line.trim())) return true;
  if (/^ {0,3}>/.test(line)) return true;
  if (matchListItem(line)) return true;
  if (line.includes('|') && i + 1 < lines.length && isTableSeparator(lines[i + 1])) return true;
  return false;
}

function parseList(lines, start, ctx) {
  const first = matchListItem(lines[start]);
  const ordered = first.ordered;
  const baseIndent = first.indent;
  const items = [];
  let i = start;

  while (i < lines.length) {
    const item = matchListItem(lines[i]);
    if (!item || item.indent !== baseIndent || item.ordered !== ordered) break;
    const contentIndent = item.contentIndent;
    const content = [item.content];
    i += 1;

    while (i < lines.length) {
      const line = lines[i];
      if (!line.trim()) {
        let j = i + 1;
        while (j < lines.length && !lines[j].trim()) j += 1;
        if (j < lines.length) {
          const nextIndent = indentOf(lines[j]);
          if (nextIndent >= Math.min(contentIndent, baseIndent + 2)) {
            content.push('');
            i += 1;
            continue;
          }
        }
        break;
      }

      const indent = indentOf(line);
      const nested = matchListItem(line);

      if (indent >= contentIndent) {
        content.push(line.slice(Math.min(indent, contentIndent)));
        i += 1;
        continue;
      }
      if (nested && nested.indent > baseIndent && indent >= baseIndent + 2) {
        content.push(line.slice(baseIndent + 2));
        i += 1;
        continue;
      }
      if (indent > baseIndent) {
        content.push(line.slice(baseIndent + 2));
        i += 1;
        continue;
      }
      break;
    }

    let inner = renderBlocks(content, ctx).trim();
    inner = inner.replace(/^<p>([\s\S]*?)<\/p>$/, '$1');
    items.push(`<li>${inner}</li>`);

    let j = i;
    while (j < lines.length && !lines[j].trim()) j += 1;
    if (j < lines.length) {
      const next = matchListItem(lines[j]);
      if (next && next.indent === baseIndent && next.ordered === ordered) {
        i = j;
      } else {
        break;
      }
    } else {
      i = j;
    }
  }

  const tag = ordered ? 'ol' : 'ul';
  return { html: `<${tag}>${items.join('')}</${tag}>`, next: i };
}

function renderBlocks(lines, ctx) {
  const out = [];
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];
    if (!line.trim()) {
      i += 1;
      continue;
    }

    const heading = /^ {0,3}(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$/.exec(line);
    if (heading) {
      const level = heading[1].length;
      const text = heading[2].trim();
      const plain = stripInline(text);
      const id = headingId(ctx, text);
      ctx.headings.push({ level, id, text: plain });
      out.push(
        `<h${level} id="${attr(id)}">${inline(text, ctx)}<a class="heading-anchor" href="#${attr(
          id
        )}" aria-label="本节链接" tabindex="-1">#</a></h${level}>`
      );
      i += 1;
      continue;
    }

    const fence = /^ {0,3}(`{3,}|~{3,})[ \t]*(.*)$/.exec(line);
    if (fence) {
      const marker = fence[1][0];
      const size = fence[1].length;
      const lang = fence[2].trim().split(/\s+/)[0] || '';
      const code = [];
      i += 1;
      while (i < lines.length) {
        const end = /^ {0,3}(`{3,}|~{3,})[ \t]*$/.exec(lines[i]);
        if (end && end[1][0] === marker && end[1].length >= size) {
          i += 1;
          break;
        }
        code.push(lines[i]);
        i += 1;
      }
      out.push(renderCode(code.join('\n'), lang));
      continue;
    }

    if (/^ {0,3}((\*\s*){3,}|(-\s*){3,}|(_\s*){3,})$/.test(line.trim())) {
      out.push('<hr>');
      i += 1;
      continue;
    }

    if (/^ {0,3}>/.test(line)) {
      const quoted = [];
      while (i < lines.length && (/^ {0,3}>/.test(lines[i]) || !lines[i].trim())) {
        if (!lines[i].trim()) {
          const next = lines[i + 1];
          if (next && /^ {0,3}>/.test(next)) {
            quoted.push('');
            i += 1;
            continue;
          }
          break;
        }
        quoted.push(lines[i].replace(/^ {0,3}> ?/, ''));
        i += 1;
      }
      out.push(`<blockquote>${renderBlocks(quoted, ctx)}</blockquote>`);
      continue;
    }

    if (line.includes('|') && i + 1 < lines.length && isTableSeparator(lines[i + 1])) {
      const headerCells = splitRow(line);
      const sepCells = splitRow(lines[i + 1]);
      const align = sepCells.map((c) => {
        const left = c.startsWith(':');
        const right = c.endsWith(':');
        return left && right ? 'center' : right ? 'right' : left ? 'left' : '';
      });
      const rows = [];
      i += 2;
      while (i < lines.length && lines[i].trim() && lines[i].includes('|')) {
        if (isTableSeparator(lines[i])) {
          i += 1;
          continue;
        }
        rows.push(splitRow(lines[i]));
        i += 1;
      }
      const cell = (tag, value, index) => {
        const style = align[index] ? ` style="text-align:${align[index]}"` : '';
        return `<${tag}${style}>${inline(value, ctx)}</${tag}>`;
      };
      const thead = `<thead><tr>${headerCells.map((c, n) => cell('th', c, n)).join('')}</tr></thead>`;
      const tbody = rows.length
        ? `<tbody>${rows
            .map((row) => `<tr>${row.map((c, n) => cell('td', c, n)).join('')}</tr>`)
            .join('')}</tbody>`
        : '';
      out.push(`<div class="table-wrap"><table>${thead}${tbody}</table></div>`);
      continue;
    }

    const item = matchListItem(line);
    if (item) {
      const list = parseList(lines, i, ctx);
      out.push(list.html);
      i = list.next;
      continue;
    }

    const paragraph = [line];
    i += 1;
    while (i < lines.length && lines[i].trim() && !isBlockStart(lines[i], lines, i)) {
      paragraph.push(lines[i]);
      i += 1;
    }
    out.push(`<p>${inline(joinLines(paragraph), ctx)}</p>`);
  }

  return out.join('\n');
}

/* ------------------------------------------------------------- page helpers */

function extractDescription(markdown) {
  const lines = markdown.split(/\r?\n/);
  const metadata =
    /^(?:[-*+]\s*)?(版本|日期|状态|更新|编制日期|作者|范围|目标|输入|依据|副标题|执行|验证)[:：]/;
  let buffer = [];
  const flush = () => {
    if (!buffer.length) return '';
    const text = stripInline(joinLines(buffer)).replace(/\s+/g, ' ').trim();
    buffer = [];
    return text;
  };

  for (const raw of lines) {
    const line = raw.trim();
    if (!line) {
      const text = flush();
      if (text.length >= 14) return truncate(text, 132);
      continue;
    }
    if (/^#{1,6}\s/.test(line) || line.startsWith('>') || line.startsWith('|')) {
      const text = flush();
      if (text.length >= 14) return truncate(text, 132);
      continue;
    }
    if (/^(`{3,}|~{3,})/.test(line)) {
      const text = flush();
      if (text.length >= 14) return truncate(text, 132);
      break;
    }
    if (metadata.test(line)) {
      flush();
      continue;
    }
    if (/^([-*+]|\d+[.)])\s/.test(line)) {
      const text = flush();
      if (text.length >= 14) return truncate(text, 132);
      const body = line.replace(/^([-*+]|\d+[.)])\s+/, '');
      if (!metadata.test(`- ${body}`) && body.length >= 14) return truncate(stripInline(body), 132);
      continue;
    }
    buffer.push(line);
  }
  const tail = flush();
  return tail.length >= 14 ? truncate(tail, 132) : '';
}

function truncate(text, max) {
  if (text.length <= max) return text;
  const slice = text.slice(0, max);
  const cut = Math.max(slice.lastIndexOf('。'), slice.lastIndexOf('；'), slice.lastIndexOf('. '));
  return `${(cut > max * 0.55 ? slice.slice(0, cut + 1) : slice).trim()}…`;
}

function readingMinutes(text) {
  const cjk = (text.match(/[\u3400-\u9fff]/g) || []).length;
  const words = (text.replace(/[\u3400-\u9fff]/g, ' ').match(/[A-Za-z0-9_]+/g) || []).length;
  return Math.max(1, Math.round(cjk / 420 + words / 220));
}

function formatDate(date) {
  const d = new Date(date);
  const y = d.getFullYear();
  const m = String(d.getMonth() + 1).padStart(2, '0');
  const day = String(d.getDate()).padStart(2, '0');
  return `${y}-${m}-${day}`;
}

const ICONS = {
  book: '<path d="M4 6.2A3.2 3.2 0 0 1 7.2 3H20v14.5H7.2A3.2 3.2 0 0 0 4 20.7z"/><path d="M4 6.2v14.5"/><path d="M8 7.2h8.5M8 10.6h6.5"/>',
  layers:
    '<path d="m12 3 8.5 4.6L12 12.2 3.5 7.6z"/><path d="m4.6 11.6 7.4 4 7.4-4"/><path d="m4.6 15.8 7.4 4 7.4-4"/>',
  sun: '<circle cx="12" cy="12" r="4.1"/><path d="M12 3v2M12 19v2M3 12h2M19 12h2M5.6 5.6 7 7M17 17l1.4 1.4M18.4 5.6 17 7M7 17l-1.4 1.4"/>',
  cube: '<path d="m12 3 7.5 4.3v9.4L12 21l-7.5-4.3V7.3z"/><path d="m4.5 7.3 7.5 4.3 7.5-4.3M12 11.6V21"/>',
  window:
    '<rect x="3.5" y="4.5" width="17" height="15" rx="2.4"/><path d="M3.5 9h17"/><path d="M6.4 6.8h.01M9 6.8h.01"/>',
  atom: '<circle cx="12" cy="12" r="1.6"/><ellipse cx="12" cy="12" rx="8.2" ry="3.6"/><ellipse cx="12" cy="12" rx="8.2" ry="3.6" transform="rotate(60 12 12)"/><ellipse cx="12" cy="12" rx="8.2" ry="3.6" transform="rotate(120 12 12)"/>',
  wind: '<path d="M3.5 8.5h10.2a2.6 2.6 0 1 0-2.6-2.6"/><path d="M3.5 12.5h14.2a2.4 2.4 0 1 1-2.4 2.4"/><path d="M3.5 16.5h6.3"/>',
  leaf: '<path d="M19.5 4.5C12 4.2 6.4 7.1 5.3 12.2c-.7 3.3 1.7 6.1 5 5.7 5.2-.6 8.5-5.9 9.2-13.4Z"/><path d="M5 20c1.6-4.6 4.7-8.1 9.4-10.6"/>',
  map: '<path d="m3.5 6.5 5.5-2 6 2 5.5-2v13l-5.5 2-6-2-5.5 2z"/><path d="M9 4.5v13M15 6.5v13"/>',
  wrench:
    '<path d="M14.7 6.2a4.2 4.2 0 0 0-5.4 5.4L4 16.9a2.1 2.1 0 0 0 3 3l5.3-5.3a4.2 4.2 0 0 0 5.4-5.4l-2.3 2.3-2.6-2.6z"/>',
};

const icon = (name) =>
  `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${
    ICONS[name] || ICONS.book
  }</svg>`;

const searchIcon =
  '<svg class="icon" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><circle cx="8.6" cy="8.6" r="5.4"/><path d="m12.7 12.7 4.1 4.1" stroke-linecap="round"/></svg>';
const menuIcon =
  '<svg class="icon" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" aria-hidden="true"><path d="M3 6h14M3 10h14M3 14h14"/></svg>';

function sidebarHtml(activeSlug) {
  return groups
    .map((group) => {
      const groupDocs = docs.filter((d) => d.group === group.id);
      const active = groupDocs.some((d) => d.slug === activeSlug);
      const links = groupDocs
        .map((d) => {
          const isActive = d.slug === activeSlug;
          return `<a class="nav-link${isActive ? ' is-active' : ''}" href="${d.slug}.html"${
            isActive ? ' aria-current="page"' : ''
          }>${esc(d.title)}</a>`;
        })
        .join('');
      return `<section class="nav-section${active ? ' is-active' : ''}" data-group="${
        group.id
      }"><button class="nav-section-toggle" type="button" aria-expanded="true"><span>${
        group.title
      }</span><svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m3 4.5 3 3 3-3"/></svg></button><div class="nav-list">${links}</div></section>`;
    })
    .join('');
}

function headerHtml() {
  return `<header class="topbar">
  <div class="topbar-inner">
    <button class="icon-button menu-toggle" id="menu-toggle" type="button" aria-label="打开文档导航" aria-expanded="false" aria-controls="sidebar">${menuIcon}</button>
    <a class="brand" href="index.html" aria-label="NativeEngine 开发文档首页">
      <span class="brand-mark" aria-hidden="true"><svg viewBox="200 200 624 624" fill="currentColor" aria-hidden="true">${BRAND_GLYPH}</svg></span>      <span class="brand-name">NativeEngine<small>开发文档</small></span>
    </a>
    <button class="search-trigger" type="button" data-search-open aria-label="搜索文档">
      ${searchIcon}<span>搜索文档</span><kbd>Ctrl K</kbd>
    </button>
    <div class="top-actions">
      <button class="icon-button search-mobile" type="button" data-search-open aria-label="搜索文档">${searchIcon}</button>
      <a class="version-chip" href="overview.html" title="当前版本">v${esc(version)}</a>
    </div>
  </div>
</header>`;
}

function shell({ title, description, bodyClass = '', body, activeSlug = '', withToc = false, toc = '' }) {
  const pageTitle = title === '开发文档' ? `${SITE_NAME} 开发文档` : `${title} · ${SITE_NAME} 开发文档`;
  return `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light">
<meta name="theme-color" content="#ffffff">
<title>${esc(pageTitle)}</title>
<meta name="description" content="${attr(description || `${SITE_NAME} 开发文档`)}">
<meta property="og:site_name" content="${attr(SITE_NAME)}">
<meta property="og:title" content="${attr(pageTitle)}">
<meta property="og:description" content="${attr(description || `${SITE_NAME} 开发文档`)}">
<meta property="og:type" content="website">
<link rel="icon" href="assets/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="assets/style.css">
</head>
<body class="${attr(bodyClass)}">
<a class="skip-link" href="#main">跳到正文</a>
${headerHtml()}
<div class="shell${withToc ? ' has-toc' : ''}">
  <aside class="sidebar" id="sidebar">
    <nav class="sidebar-nav" aria-label="文档导航">
      ${sidebarHtml(activeSlug)}
    </nav>
    <div class="sidebar-foot"><span class="dot"></span>${docs.length} 篇文档 · v${esc(version)}</div>
  </aside>
  <div class="scrim" id="scrim"></div>
  <main class="main" id="main">
${body}
  </main>
  ${
    withToc
      ? `<aside class="toc-rail" aria-label="本页目录"><nav class="toc">${toc}</nav></aside>`
      : ''
  }
</div>
<div class="search-overlay" id="search-overlay" role="dialog" aria-modal="true" aria-label="搜索文档" hidden>
  <div class="search-backdrop" data-search-close></div>
  <div class="search-panel">
    <div class="search-input-row">
      ${searchIcon}
      <input class="search-input" id="search-input" type="search" placeholder="搜索文档、标题或系统…" aria-label="搜索文档" autocomplete="off" spellcheck="false">
      <button class="search-cancel" type="button" data-search-close>取消</button>
    </div>
    <div class="search-results" id="search-results" role="listbox" aria-label="搜索结果"></div>
    <div class="search-hint"><span>↑↓ 选择</span><span>↵ 打开</span><span>esc 关闭</span></div>
  </div>
</div>
<script src="assets/app.js" defer></script>
</body>
</html>`;
}

function tocHtml(headings) {
  const items = headings.filter((h) => h.level === 2 || h.level === 3);
  if (!items.length) return '';
  return `<p class="toc-title">本页目录</p>${items
    .map(
      (h) =>
        `<a class="toc-link toc-l${h.level}" href="#${attr(h.id)}" data-toc="${attr(
          h.id
        )}">${esc(h.text)}</a>`
    )
    .join('')}`;
}

function pagerHtml(index) {
  const prev = flat[index - 1];
  const next = flat[index + 1];
  const card = (doc, dir) => {
    if (!doc) return '<span class="pager-spacer"></span>';
    return `<a class="pager-card pager-${dir}" href="${doc.slug}.html"><span class="pager-dir">${
      dir === 'prev' ? '上一页' : '下一页'
    }</span><span class="pager-title">${esc(doc.title)}</span></a>`;
  };
  return `<nav class="pager" aria-label="翻页">${card(prev, 'prev')}${card(next, 'next')}</nav>`;
}

/* ---------------------------------------------------------------- build docs */

const pages = [];
for (const doc of docs) {
  const abs = path.join(root, doc.file);
  let markdown;
  try {
    markdown = (await readFile(abs, 'utf8')).replace(/\r\n/g, '\n');
  } catch {
    console.warn(`docs site: skip unreadable ${doc.file}`);
    continue;
  }
  const info = await stat(abs);
  const lines = markdown.split('\n');
  const h1Index = lines.findIndex((line) => /^#\s+/.test(line));
  const sourceTitle = h1Index >= 0 ? lines[h1Index].replace(/^#\s+/, '').trim() : '';
  const bodyLines = h1Index >= 0 ? lines.filter((_, i) => i !== h1Index) : lines;
  const ctx = makeContext(norm(doc.file));
  const html = renderBlocks(bodyLines, ctx);
  const plain = stripTags(html);
  const description = extractDescription(markdown) || `${doc.title} · ${SITE_NAME} 开发文档`;
  pages.push({
    ...doc,
    sourceTitle,
    html,
    headings: ctx.headings,
    plain,
    description,
    reading: readingMinutes(plain),
    date: formatDate(info.mtime),
    mtime: info.mtimeMs,
  });
}

await rm(outDir, { recursive: true, force: true, maxRetries: 6, retryDelay: 250 });
await mkdir(path.join(outDir, 'assets'), { recursive: true });

const searchIndex = [];

for (let index = 0; index < pages.length; index += 1) {
  const page = pages[index];
  const group = groupById.get(page.group);
  const meta = [
    `<span class="meta-chip">${esc(group.title)}</span>`,
    `<span>约 ${page.reading} 分钟</span>`,
    `<span class="meta-source" title="源文件">${esc(norm(page.file))}</span>`,
  ].join('');
  const body = `    <div class="page">
      <div class="page-head">
        <nav class="breadcrumbs" aria-label="位置"><a href="index.html">文档</a><span aria-hidden="true">/</span><a href="group-${group.id}.html">${esc(
          group.title
        )}</a></nav>
        <h1 class="page-title">${esc(page.title)}</h1>
        <div class="page-meta">${meta}</div>
      </div>
      <article class="prose">
${page.html}
      </article>
${pagerHtml(index)}
      <footer class="page-footer"><span>${SITE_NAME} ${esc(version)}</span><span>内容与仓库 Markdown 同源维护</span></footer>
    </div>`;

  const html = shell({
    title: page.title,
    description: page.description,
    bodyClass: 'page-doc',
    body,
    activeSlug: page.slug,
    withToc: page.headings.some((h) => h.level === 2 || h.level === 3),
    toc: tocHtml(page.headings),
  });

  await writeFile(path.join(outDir, `${page.slug}.html`), html, 'utf8');

  searchIndex.push({
    slug: page.slug,
    title: page.title,
    group: group.title,
    desc: page.description,
    headings: page.headings.filter((h) => h.level <= 3).map((h) => h.text).slice(0, 20),
    text: page.plain.slice(0, 4200),
  });
}

/* --------------------------------------------------------------- group pages */

for (const group of groups) {
  const groupDocs = pages.filter((p) => p.group === group.id);
  const rows = groupDocs
    .map(
      (doc) => `<a class="doc-row" href="${doc.slug}.html">
        <span class="doc-row-main">
          <span class="doc-row-title">${esc(doc.title)}</span>
          <span class="doc-row-desc">${esc(doc.description)}</span>
        </span>
        <span class="doc-row-meta"><span>约 ${doc.reading} 分钟</span><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 3.5 4.5 4.5L6 12.5"/></svg></span>
      </a>`
    )
    .join('');
  const first = groupDocs[0];
  const body = `    <div class="page group-page">
      <div class="page-head">
        <nav class="breadcrumbs" aria-label="位置"><a href="index.html">文档</a><span aria-hidden="true">/</span><span>${esc(
          group.title
        )}</span></nav>
        <h1 class="page-title">${esc(group.title)}</h1>
        <p class="page-lede">${esc(group.blurb)}</p>
        <div class="page-meta"><span>${groupDocs.length} 篇文档</span><span>按主题收录</span></div>
      </div>
      <div class="doc-list">${rows}</div>
      <footer class="page-footer"><span>${SITE_NAME} ${esc(version)}</span><span><a href="index.html">返回文档首页</a></span></footer>
    </div>`;

  await writeFile(
    path.join(outDir, `group-${group.id}.html`),
    shell({
      title: group.title,
      description: `${group.title} — ${group.blurb}`,
      bodyClass: 'page-group',
      body,
      activeSlug: first ? first.slug : '',
    }),
    'utf8'
  );
}

/* ----------------------------------------------------------------- home page */

const recent = [...pages].sort((a, b) => b.mtime - a.mtime).slice(0, 6);
const featuredPages = featured.map((slug) => pages.find((p) => p.slug === slug)).filter(Boolean);
const totalReading = pages.reduce((sum, p) => sum + p.reading, 0);

const groupCards = groups
  .map((group) => {
    const groupDocs = pages.filter((p) => p.group === group.id);
    const samples = groupDocs.slice(0, 3).map((d) => d.title).join(' · ');
    return `<a class="group-card" href="group-${group.id}.html">
      <span class="group-card-top"><span class="group-icon">${icon(group.icon)}</span><span class="group-count">${groupDocs.length} 篇</span></span>
      <span class="group-title">${esc(group.title)}</span>
      <span class="group-blurb">${esc(group.blurb)}</span>
      <span class="group-samples">${esc(samples)}</span>
    </a>`;
  })
  .join('');

const featuredCards = featuredPages
  .map((doc) => {
    const group = groupById.get(doc.group);
    return `<a class="featured-card" href="${doc.slug}.html">
      <span class="featured-group">${esc(group.title)}</span>
      <h3>${esc(doc.title)}</h3>
      <p>${esc(doc.description)}</p>
      <span class="featured-meta"><span>约 ${doc.reading} 分钟</span><span class="featured-arrow" aria-hidden="true">→</span></span>
    </a>`;
  })
  .join('');

const recentRows = recent
  .map((doc) => {
    const group = groupById.get(doc.group);
    return `<a class="recent-item" href="${doc.slug}.html"><span class="recent-date">${doc.date}</span><span class="recent-body"><span class="recent-title">${esc(
      doc.title
    )}</span><span class="recent-group">${esc(group.title)}</span></span></a>`;
  })
  .join('');

const homeBody = `    <div class="home">
      <section class="hero">
        <p class="eyebrow"><span class="dot"></span>NativeEngine · v${esc(version)}</p>
        <h1>开发文档</h1>
        <p class="hero-lede">一个可运行、可验证的 Windows / D3D12 原生游戏引擎，以及让 AI Agent 与人类编辑器共用同一条场景事务、确定性模拟与渲染证据闭环的工程记录。</p>
        <button class="hero-search" type="button" data-search-open>
          ${searchIcon}<span>搜索 ${pages.length} 篇文档</span><kbd>Ctrl K</kbd>
        </button>
        <div class="quick-row">
          <a class="quick-link" href="overview.html">项目概览</a>
          <a class="quick-link" href="overview.html#快速开始">快速开始</a>
          <a class="quick-link" href="group-architecture.html">架构与运行时</a>
          <a class="quick-link" href="ai-tools.html">AI 工具面</a>
          <a class="quick-link" href="next-phase.html">下一阶段</a>
        </div>
      </section>

      <section class="stats" aria-label="文档统计">
        <div class="stat"><b>${pages.length}</b><span>篇源文档</span></div>
        <div class="stat"><b>${groups.length}</b><span>个主题分区</span></div>
        <div class="stat"><b>v${esc(version)}</b><span>当前版本</span></div>
        <div class="stat"><b>${totalReading}</b><span>分钟总阅读量</span></div>
      </section>

      <section class="section" id="topics">
        <div class="section-heading"><h2>按主题浏览</h2><p>从概览到仿真，覆盖仓库中的全部设计、实现与验收文档</p></div>
        <div class="group-grid">${groupCards}</div>
      </section>

      <section class="section" id="featured">
        <div class="section-heading"><h2>精选文档</h2><p>进入系统前最值得先读的几篇</p></div>
        <div class="featured-grid">${featuredCards}</div>
      </section>

      <section class="showcase" aria-label="示例场景">
        <img src="images/town-hero.jpg" alt="石桥小镇渲染截图" loading="lazy">
        <div class="showcase-body">
          <p class="eyebrow"><span class="dot"></span>示例场景</p>
          <h2>石桥小镇与雾夜街区</h2>
          <p>608 个实体、4.5 MiB 纹理预算、bindless 材质表；同一份场景数据在 Web 编辑器、原生宿主与图像门禁之间流转。</p>
          <div class="showcase-links"><a href="material-pipeline.html">材质管线</a><a href="night-town.html">雾夜小镇</a></div>
        </div>
      </section>

      <section class="split">
        <div>
          <div class="section-heading"><h2>最近更新</h2><p>按源文件修改时间</p></div>
          <div class="recent-list">${recentRows}</div>
        </div>
        <div>
          <div class="section-heading"><h2>快速开始</h2><p>Windows 11 x64 · Node.js 22+</p></div>
          <figure class="code-block quickstart"><div class="code-bar"><span>powershell</span><button class="copy-code" type="button" aria-label="复制代码">复制</button></div><pre><code>powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1</code></pre></figure>
          <p class="quick-note">完整工具链、验证与打包步骤见 <a href="overview.html#快速开始">项目概览</a>。</p>
        </div>
      </section>

      <footer class="site-footer">
        <span>NativeEngine ${esc(version)}${commit ? ` · ${esc(commit)}` : ''}</span>
        <span>由仓库 Markdown 构建于 ${formatDate(buildDate)}</span>
      </footer>
    </div>`;

await writeFile(
  path.join(outDir, 'index.html'),
  shell({
    title: '开发文档',
    description:
      'NativeEngine 开发文档：Windows / D3D12 原生游戏引擎的场景事务、渲染、仿真、编辑器与 AI 工具面。',
    bodyClass: 'page-home',
    body: homeBody,
  }),
  'utf8'
);

/* -------------------------------------------------------------------- 404 */

await writeFile(
  path.join(outDir, '404.html'),
  shell({
    title: '页面未找到',
    description: '页面未找到',
    bodyClass: 'page-404',
    body: `    <div class="page simple-page"><div class="page-head"><p class="eyebrow"><span class="dot"></span>404</p><h1 class="page-title">页面未找到</h1><p class="page-lede">这个地址没有对应的文档。可以返回首页，或直接搜索。</p><div class="simple-actions"><a class="button-primary" href="index.html">返回文档首页</a><button class="button-secondary" type="button" data-search-open>搜索文档</button></div></div></div>`,
  }),
  'utf8'
);

/* --------------------------------------------------------------- assets etc. */

await cp(themeDir, path.join(outDir, 'assets'), { recursive: true });
try {
  await cp(path.join(root, 'docs', 'images'), path.join(outDir, 'images'), { recursive: true });
} catch {
  /* no images */
}
await writeFile(
  path.join(outDir, 'assets', 'search.json'),
  JSON.stringify(searchIndex),
  'utf8'
);

const totalBytes = pages.reduce((sum, p) => sum + Buffer.byteLength(p.html, 'utf8'), 0);
console.log(
  `docs site: ${pages.length} docs, ${groups.length} groups, ${pages.length + groups.length + 2} pages`
);
console.log(`content: ${(totalBytes / 1024 / 1024).toFixed(2)} MiB, build ${formatDate(buildDate)}`);
console.log(`output: ${path.relative(root, outDir)}`);
