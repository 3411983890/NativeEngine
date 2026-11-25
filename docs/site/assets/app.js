(() => {
  const root = document.documentElement;

  const themeToggle = document.getElementById('theme-toggle');
  themeToggle?.addEventListener('click', () => {
    const next = root.dataset.theme === 'light' ? 'dark' : 'light';
    root.dataset.theme = next;
    try { localStorage.setItem('ng-docs-theme', next); } catch (e) {}
  });

  const body = document.body;
  const menuButton = document.getElementById('menu-button');
  const scrim = document.getElementById('scrim');
  const closeNav = () => {
    body.classList.remove('nav-open');
    menuButton?.setAttribute('aria-expanded', 'false');
  };
  menuButton?.addEventListener('click', () => {
    const open = body.classList.toggle('nav-open');
    menuButton.setAttribute('aria-expanded', String(open));
  });
  scrim?.addEventListener('click', closeNav);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeNav();
  });

  document.querySelectorAll('.prose pre').forEach((pre) => {
    const button = document.createElement('button');
    button.className = 'copy-button';
    button.type = 'button';
    button.textContent = '复制';
    button.addEventListener('click', async () => {
      const text = pre.querySelector('code')?.innerText ?? pre.innerText;
      try {
        await navigator.clipboard.writeText(text);
        button.textContent = '已复制';
      } catch (e) {
        button.textContent = '复制失败';
      }
      setTimeout(() => { button.textContent = '复制'; }, 1400);
    });
    pre.appendChild(button);
  });

  const tocLinks = [...document.querySelectorAll('.toc-link')];
  if (tocLinks.length && 'IntersectionObserver' in window) {
    const byId = new Map(tocLinks.map((a) => [a.getAttribute('href').slice(1), a]));
    const visible = new Set();
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) visible.add(entry.target.id);
          else visible.delete(entry.target.id);
        });
        let current = null;
        for (const link of tocLinks) {
          const id = link.getAttribute('href').slice(1);
          if (visible.has(id)) { current = link; break; }
        }
        if (!current) {
          const passed = [...byId.keys()].filter((id) => {
            const el = document.getElementById(id);
            return el && el.getBoundingClientRect().top < 120;
          });
          current = passed.length ? byId.get(passed[passed.length - 1]) : null;
        }
        tocLinks.forEach((a) => a.classList.toggle('is-active', a === current));
      },
      { rootMargin: '-72px 0px -62% 0px', threshold: [0, 1] }
    );
    document.querySelectorAll('.prose h2[id], .prose h3[id]').forEach((h) => observer.observe(h));
  }

  const input = document.getElementById('search-input');
  const panel = document.getElementById('search-results');
  const index = window.NG_SEARCH || [];
  let cursor = 0;
  let rows = [];

  const escapeHtml = (s) =>
    String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

  const closeSearch = () => {
    panel.hidden = true;
    rows = [];
  };

  const render = (query) => {
    const q = query.trim().toLowerCase();
    if (!q) { closeSearch(); return; }
    const scored = [];
    for (const page of index) {
      let score = 0;
      if (page.title.toLowerCase().includes(q)) score += 40;
      if (page.desc.toLowerCase().includes(q)) score += 14;
      for (const h of page.headings) if (h.toLowerCase().includes(q)) score += 12;
      if (page.text.toLowerCase().includes(q)) score += 6;
      if (page.group.toLowerCase().includes(q)) score += 2;
      if (score) scored.push({ page, score });
    }
    scored.sort((a, b) => b.score - a.score);
    rows = scored.slice(0, 8).map((s) => s.page);

    if (!rows.length) {
      panel.innerHTML = `<div class="sr-empty">没有匹配“${escapeHtml(query)}”的页面</div>`;
      panel.hidden = false;
      return;
    }
    cursor = 0;
    panel.innerHTML = rows
      .map(
        (page, i) =>
          `<a class="sr-item${i === 0 ? ' is-cursor' : ''}" href="${page.slug}.html">
            <span class="sr-head"><span class="sr-title">${escapeHtml(page.title)}</span><span class="sr-group">${escapeHtml(page.group)}</span></span>
            <span class="sr-desc">${escapeHtml(page.desc)}</span>
          </a>`
      )
      .join('');
    panel.hidden = false;
  };

  const move = (delta) => {
    if (!rows.length) return;
    cursor = (cursor + delta + rows.length) % rows.length;
    panel.querySelectorAll('.sr-item').forEach((el, i) => el.classList.toggle('is-cursor', i === cursor));
    panel.querySelectorAll('.sr-item')[cursor]?.scrollIntoView({ block: 'nearest' });
  };

  input?.addEventListener('input', () => render(input.value));
  input?.addEventListener('focus', () => { if (input.value.trim()) render(input.value); });
  input?.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { e.preventDefault(); move(1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); move(-1); }
    else if (e.key === 'Enter' && rows[cursor]) { window.location.href = `${rows[cursor].slug}.html`; }
    else if (e.key === 'Escape') { input.blur(); closeSearch(); }
  });

  document.addEventListener('click', (e) => {
    if (!e.target.closest('.search')) closeSearch();
  });

  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      input?.focus();
      input?.select();
    }
  });
})();
