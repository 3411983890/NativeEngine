(() => {
  'use strict';

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

  const overlay = $('#search-overlay');
  const input = $('#search-input');
  const resultsEl = $('#search-results');
  const sidebar = $('#sidebar');
  const scrim = $('#scrim');
  const menuToggle = $('#menu-toggle');

  const QUICK = [
    'overview',
    'external-assets',
    'physics-destruction',
    'editor',
    'simulation-plan',
    'ai-tools',
  ];

  let index = null;
  let indexPromise = null;
  let activeResult = -1;
  let visibleResults = [];
  let lastTrigger = null;

  function bodyLock() {
    const navOpen = document.body.classList.contains('nav-open');
    const searchOpen = overlay && !overlay.hidden;
    document.body.classList.toggle('no-scroll', navOpen || searchOpen);
  }

  function loadIndex() {
    if (index) return Promise.resolve(index);
    if (indexPromise) return indexPromise;
    resultsEl.innerHTML = '<div class="search-empty">正在加载索引…</div>';
    indexPromise = fetch('assets/search.json', { credentials: 'same-origin' })
      .then((response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then((data) => {
        index = Array.isArray(data) ? data : [];
        return index;
      })
      .catch(() => {
        indexPromise = null;
        resultsEl.innerHTML =
          '<div class="search-empty">索引加载失败。请确认通过本地服务器或已发布的站点访问。</div>';
        throw new Error('search index unavailable');
      });
    return indexPromise;
  }

  function escapeRe(value) {
    return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  }

  function escapeHtml(value) {
    return String(value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function highlight(text, terms) {
    const safe = escapeHtml(text);
    if (!terms.length) return safe;
    const re = new RegExp(`(${terms.map(escapeRe).join('|')})`, 'gi');
    return safe.replace(re, '<mark>$1</mark>');
  }

  function termsFor(query) {
    return query
      .toLowerCase()
      .split(/\s+/)
      .map((term) => term.trim())
      .filter(Boolean);
  }

  function snippetFor(entry, query, terms) {
    const text = `${entry.desc || ''} ${entry.text || ''}`.trim();
    if (!text) return '';
    const lower = text.toLowerCase();
    let pos = query ? lower.indexOf(query.toLowerCase()) : -1;
    if (pos < 0) {
      for (const term of terms) {
        pos = lower.indexOf(term);
        if (pos >= 0) break;
      }
    }
    if (pos < 0) pos = 0;
    const start = Math.max(0, pos - 42);
    const end = Math.min(text.length, pos + 150);
    return `${start > 0 ? '…' : ''}${text.slice(start, end)}${end < text.length ? '…' : ''}`;
  }

  function rank(entry, query, terms) {
    if (!terms.length) return 0;
    const title = entry.title.toLowerCase();
    const headings = (entry.headings || []).join(' ').toLowerCase();
    const desc = (entry.desc || '').toLowerCase();
    const text = (entry.text || '').toLowerCase();
    let score = 0;
    if (title === query) score += 140;
    if (title.startsWith(query)) score += 70;
    if (title.includes(query)) score += 45;
    for (const term of terms) {
      if (title.includes(term)) score += 34;
      else if (headings.includes(term)) score += 15;
      else if (desc.includes(term)) score += 10;
      else if (text.includes(term)) score += 4;
      else return -1;
    }
    return score;
  }

  function search(query) {
    if (!index) return [];
    const q = query.trim().toLowerCase();
    if (!q) {
      return QUICK.map((slug) => index.find((entry) => entry.slug === slug))
        .filter(Boolean)
        .map((entry) => ({ entry, score: 0 }));
    }
    const terms = termsFor(q);
    return index
      .map((entry) => ({ entry, score: rank(entry, q, terms) }))
      .filter((item) => item.score >= 0)
      .sort((a, b) => b.score - a.score || a.entry.title.localeCompare(b.entry.title, 'zh-Hans-CN'))
      .slice(0, 24);
  }

  function renderResults(items, query) {
    const terms = termsFor(query);
    visibleResults = items.map((item) => item.entry);
    activeResult = items.length ? 0 : -1;

    if (!items.length) {
      resultsEl.innerHTML = `<div class="search-empty">没有找到与“${escapeHtml(
        query
      )}”匹配的文档。试试系统名、文件名或错误码。</div>`;
      return;
    }

    const heading = query ? '' : '<div class="search-empty" style="padding:12px 13px 6px;text-align:left">常用入口</div>';
    resultsEl.innerHTML =
      heading +
      items
        .map((item, i) => {
          const entry = item.entry;
          return `<a class="search-result${i === 0 ? ' is-active' : ''}" href="${entry.slug}.html" role="option" aria-selected="${
            i === 0
          }"><span class="search-result-title">${highlight(entry.title, terms)}<span class="search-result-group">${
            escapeHtml(entry.group)
          }</span></span><span class="search-result-snippet">${highlight(
            snippetFor(entry, query, terms),
            terms
          )}</span></a>`;
        })
        .join('');
  }

  function setActive(next) {
    if (!visibleResults.length) return;
    const links = $$('.search-result', resultsEl);
    if (!links.length) return;
    activeResult = (next + links.length) % links.length;
    links.forEach((link, i) => {
      const active = i === activeResult;
      link.classList.toggle('is-active', active);
      link.setAttribute('aria-selected', String(active));
      if (active) link.scrollIntoView({ block: 'nearest' });
    });
  }

  function openSearch(trigger, initialQuery = '') {
    if (!overlay) return;
    lastTrigger = trigger || document.activeElement;
    overlay.hidden = false;
    bodyLock();
    input.value = initialQuery;
    resultsEl.innerHTML = '<div class="search-empty">正在加载索引…</div>';
    window.requestAnimationFrame(() => input.focus());
    loadIndex()
      .then(() => renderResults(search(input.value), input.value.trim()))
      .catch(() => {});
  }

  function closeSearch() {
    if (!overlay || overlay.hidden) return;
    overlay.hidden = true;
    input.value = '';
    resultsEl.innerHTML = '';
    visibleResults = [];
    activeResult = -1;
    bodyLock();
    if (lastTrigger && typeof lastTrigger.focus === 'function') lastTrigger.focus();
  }

  function openNav() {
    document.body.classList.add('nav-open');
    bodyLock();
    if (menuToggle) menuToggle.setAttribute('aria-expanded', 'true');
  }

  function closeNav() {
    document.body.classList.remove('nav-open');
    bodyLock();
    if (menuToggle) menuToggle.setAttribute('aria-expanded', 'false');
  }

  if (menuToggle) {
    menuToggle.addEventListener('click', () => {
      if (document.body.classList.contains('nav-open')) closeNav();
      else openNav();
    });
  }

  if (scrim) {
    scrim.addEventListener('click', closeNav);
  }

  $$('.nav-link', sidebar || document).forEach((link) => {
    link.addEventListener('click', () => {
      if (window.matchMedia('(max-width: 980px)').matches) closeNav();
    });
  });

  document.addEventListener('click', (event) => {
    const trigger = event.target.closest('[data-search-open]');
    if (trigger) {
      event.preventDefault();
      openSearch(trigger);
      return;
    }
    if (event.target.closest('[data-search-close]')) {
      event.preventDefault();
      closeSearch();
    }
  });

  if (input) {
    input.addEventListener('input', () => {
      if (!index) return;
      renderResults(search(input.value), input.value.trim());
    });

    input.addEventListener('keydown', (event) => {
      if (event.key === 'ArrowDown') {
        event.preventDefault();
        setActive(activeResult + 1);
      } else if (event.key === 'ArrowUp') {
        event.preventDefault();
        setActive(activeResult - 1);
      } else if (event.key === 'Enter') {
        event.preventDefault();
        const link = $$('.search-result', resultsEl)[activeResult];
        if (link) window.location.href = link.getAttribute('href');
      } else if (event.key === 'Escape') {
        event.preventDefault();
        closeSearch();
      }
    });
  }

  document.addEventListener('keydown', (event) => {
    const typing =
      event.target instanceof HTMLElement &&
      (event.target.matches('input, textarea, select') || event.target.isContentEditable);

    if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
      event.preventDefault();
      openSearch();
      return;
    }
    if (event.key === '/' && !typing && !event.metaKey && !event.ctrlKey && !event.altKey) {
      event.preventDefault();
      openSearch();
      return;
    }
    if (event.key === 'Escape') {
      if (overlay && !overlay.hidden) {
        closeSearch();
      } else if (document.body.classList.contains('nav-open')) {
        closeNav();
      }
    }
  });

  $$('.nav-section').forEach((section) => {
    const button = $('.nav-section-toggle', section);
    if (!button) return;
    const group = section.dataset.group;
    let collapsed = new Set();
    try {
      collapsed = new Set(JSON.parse(localStorage.getItem('ngdocs-collapsed') || '[]'));
    } catch {
      /* ignore */
    }
    if (collapsed.has(group)) {
      section.classList.add('is-collapsed');
      button.setAttribute('aria-expanded', 'false');
    }
    button.addEventListener('click', () => {
      const isCollapsed = section.classList.toggle('is-collapsed');
      button.setAttribute('aria-expanded', String(!isCollapsed));
      try {
        const stored = new Set(JSON.parse(localStorage.getItem('ngdocs-collapsed') || '[]'));
        if (isCollapsed) stored.add(group);
        else stored.delete(group);
        localStorage.setItem('ngdocs-collapsed', JSON.stringify([...stored]));
      } catch {
        /* ignore */
      }
    });
  });

  const activeNav = $('.nav-link.is-active', sidebar || document);
  if (activeNav && sidebar) {
    const navRect = activeNav.getBoundingClientRect();
    const sidebarRect = sidebar.getBoundingClientRect();
    if (navRect.top < sidebarRect.top + 40 || navRect.bottom > sidebarRect.bottom - 40) {
      sidebar.scrollTop += navRect.top - sidebarRect.top - sidebarRect.height / 3;
    }
  }

  const tocLinks = $$('.toc-link');
  if (tocLinks.length) {
    const targets = tocLinks
      .map((link) => document.getElementById(link.dataset.toc))
      .filter(Boolean);

    let ticking = false;
    const updateToc = () => {
      ticking = false;
      if (!targets.length) return;
      const marker = window.scrollY + 130;
      let current = targets[0];
      for (const target of targets) {
        if (target.getBoundingClientRect().top + window.scrollY <= marker) current = target;
        else break;
      }
      tocLinks.forEach((link) => {
        link.classList.toggle('is-active', link.dataset.toc === current.id);
      });
    };

    window.addEventListener(
      'scroll',
      () => {
        if (!ticking) {
          ticking = true;
          window.requestAnimationFrame(updateToc);
        }
      },
      { passive: true }
    );
    updateToc();
  }

  document.addEventListener('click', (event) => {
    const button = event.target.closest('.copy-code');
    if (!button) return;
    const block = button.closest('.code-block');
    const code = block ? $('code', block) : null;
    if (!code) return;
    const text = code.textContent || '';
    const done = () => {
      const original = button.textContent;
      button.textContent = '已复制';
      window.setTimeout(() => {
        button.textContent = original;
      }, 1600);
    };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done).catch(() => {});
    } else {
      const area = document.createElement('textarea');
      area.value = text;
      area.setAttribute('readonly', '');
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.appendChild(area);
      area.select();
      try {
        document.execCommand('copy');
        done();
      } catch {
        /* ignore */
      }
      document.body.removeChild(area);
    }
  });

  window.addEventListener('resize', () => {
    if (window.innerWidth > 980) closeNav();
  });

  const initialQuery = new URLSearchParams(window.location.search).get('q');
  if (initialQuery) openSearch(null, initialQuery);
})();
