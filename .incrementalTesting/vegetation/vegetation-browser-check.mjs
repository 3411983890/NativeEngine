import { spawn } from 'node:child_process';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// Browser-side verification of the vegetation page.
//
// Earlier rounds changed the page (native debug-view dropdown, ACK status line, undo shortcuts,
// cache-hit hint) and could only syntax-check it. This launches the editor's web server, drives
// headless Edge over CDP, and asks the real page whether those things exist and whether the 3D
// context came up — then takes a screenshot. No Electron: the native viewport is absent here, so
// this checks the WebGL path and the DOM, not the Win32 child window.
//
// Usage: node .incrementalTesting/vegetation/vegetation-browser-check.mjs

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const reportDirectory = path.join(root, 'reports/vegetation-production');
const imagePath = path.join(reportDirectory, 'images/browser-vegetation.png');
const edgeCandidates = [
  path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'Microsoft/Edge/Application/msedge.exe'),
  path.join(process.env.ProgramFiles ?? 'C:\\Program Files', 'Microsoft/Edge/Application/msedge.exe'),
];

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function fetchJson(url, attempts = 40) {
  for (let attempt = 0; attempt < attempts; ++attempt) {
    try {
      const response = await fetch(url);
      if (response.ok) return await response.json();
    } catch { /* not up yet */ }
    await sleep(250);
  }
  throw new Error(`cannot reach ${url}`);
}

// One CDP connection, request/response by id. Node's built-in WebSocket is enough; adding a
// library for four messages would be more code than the protocol.
async function connect(webSocketDebuggerUrl) {
  const socket = new WebSocket(webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', () => reject(new Error('CDP socket failed')), { once: true });
  });
  let nextId = 1;
  const pending = new Map();
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    const waiter = pending.get(message.id);
    if (!waiter) return;
    pending.delete(message.id);
    if (message.error) waiter.reject(new Error(`${message.error.message}`));
    else waiter.resolve(message.result);
  });
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    socket.send(JSON.stringify({ id, method, params }));
  });
  return { send, close: () => socket.close() };
}

async function main() {
  const existing = await Promise.all(edgeCandidates.map(async (candidate) =>
    candidate ? fs.access(candidate).then(() => candidate).catch(() => null) : null));
  const edge = existing.find(Boolean);
  if (!edge) throw new Error('headless Edge is not installed; cannot verify the page here');

  await fs.mkdir(path.dirname(imagePath), { recursive: true });
  // The editor server is started by the caller (or here, if ED I TOR_URL is absent).
  const editorUrl = process.env.NATIVE_GPU_EDITOR_URL ?? 'http://127.0.0.1:4180';

  const profile = path.join(root, 'build/vegetation-browser');
  await fs.mkdir(profile, { recursive: true });
  const browser = spawn(edge, [
    '--headless=new', '--disable-gpu-sandbox', '--no-first-run', '--no-default-browser-check',
    '--use-angle=swiftshader', '--window-size=1280,800', '--remote-debugging-port=9333',
    `--user-data-dir=${profile}`, 'about:blank',
  ], { stdio: 'ignore', windowsHide: true });

  try {
    const targets = await fetchJson('http://127.0.0.1:9333/json/list');
    const page = targets.find((target) => target.type === 'page');
    if (!page) throw new Error('no page target');
    const client = await connect(page.webSocketDebuggerUrl);
    await client.send('Page.enable');
    await client.send('Runtime.enable');

    const failures = [];
    client.send('Runtime.exceptionThrown').catch(() => {});
    await client.send('Page.navigate', { url: `${editorUrl}/vegetation` });
    // Wait for the page to say it is ready rather than for a fixed delay: boot fetches a session
    // token, the species list and then a full preview (the oak preview with analysis takes
    // seconds). A fixed sleep made this check report a failure that was only slowness.
    let ready = false;
    for (let attempt = 0; attempt < 60; ++attempt) {
      await sleep(500);
      try {
        const probe = await client.send('Runtime.evaluate', {
          expression: `Boolean(window.__forge && window.__forge.state && window.__forge.state.spec)`,
          returnByValue: true,
        });
        if (probe.result?.value === true) { ready = true; break; }
      } catch { /* navigating */ }
    }
    // One more beat for the first preview to land in the HUD.
    await sleep(1500);

    const evaluate = async (expression) => {
      const result = await client.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
      if (result.exceptionDetails) throw new Error(result.exceptionDetails.text ?? 'evaluation failed');
      return result.result.value;
    };

    const dom = await evaluate(`(() => {
      const view = document.getElementById('native-view');
      const state = document.getElementById('native-state');
      const canvas = document.getElementById('gl');
      const gl = canvas && (canvas.getContext('webgl2') || canvas.getContext('webgl'));
      return {
        url: location.pathname,
        title: document.title,
        debugViewOptions: view ? view.options.length : -1,
        nativeStateText: state ? state.textContent.trim() : null,
        hasCanvas: Boolean(canvas),
        webgl: gl ? gl.getParameter(gl.VERSION) : null,
        speciesRows: document.querySelectorAll('#species-list .sp, #species-list > *').length,
        statVerts: document.getElementById('stat-verts')?.textContent?.trim() ?? null,
        hudTri: document.getElementById('hud-tri')?.textContent?.trim() ?? null,
        forge: typeof window.__forge === 'object' && window.__forge !== null,
        revision: window.__forge?.state?.revision ?? null,
        level: window.__forge?.state?.level ?? null,
        bridgePresent: Boolean(window.forgeViewport),
      };
    })()`);

    const expectations = [
      ['page is /vegetation', dom.url === '/vegetation'],
      ['WebGL context came up', typeof dom.webgl === 'string' && dom.webgl.length > 0],
      ['the native debug-view dropdown is populated', dom.debugViewOptions >= 15],
      ['the native status line exists', typeof dom.nativeStateText === 'string' && dom.nativeStateText.length > 0],
      ['the page state hook is exposed', dom.forge === true],
      ['a species was loaded with a revision', typeof dom.revision === 'string' && dom.revision.length === 64],
      ['the HUD reports triangles', typeof dom.hudTri === 'string' && dom.hudTri !== '—'],
    ];
    for (const [label, ok] of expectations) if (!ok) failures.push(label);

    // The undo shortcut must reach the service; without Electron the native bridge is absent, so
    // pressing it must not throw and must leave the page alive.
    const shortcut = await evaluate(`(async () => {
      const forge = window.__forge;
      if (!forge || !forge.state) return { before: null, alive: false, toast: '', status: document.getElementById('st-text')?.textContent ?? '', body: document.body.innerText.slice(0, 300) };
      const before = forge.state.spec?.generators?.levels?.[1]?.count ?? null;
      const event = new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true });
      window.dispatchEvent(event);
      await new Promise((resolve) => setTimeout(resolve, 1200));
      return { before, alive: Boolean(window.__forge?.state?.spec), toast: document.getElementById('toast')?.textContent ?? '', status: document.getElementById('st-text')?.textContent ?? '' };
    })()`);
    if (!shortcut.alive) failures.push(`page did not finish booting: ${shortcut.status ?? ''} ${shortcut.body ?? ''}`);

    const shot = await client.send('Page.captureScreenshot', { format: 'png' });
    await fs.writeFile(imagePath, Buffer.from(shot.data, 'base64'));

    const report = {
      schema: 'nativegpu.vegetation-browser-check/1',
      generatedUtc: new Date().toISOString(),
      tool: '.incrementalTesting/vegetation/vegetation-browser-check.mjs',
      editorUrl,
      browser: { executable: path.basename(edge), flags: ['--headless=new', '--use-angle=swiftshader'] },
      dom,
      shortcut,
      expectations: expectations.map(([label, ok]) => ({ label, ok })),
      failures,
      screenshot: path.relative(root, imagePath).split(path.sep).join('/'),
      limits: [
        'No Electron: window.forgeViewport is absent, so the native viewport path, its ACK status text and the cache-hit hint cannot be exercised here.',
        'SwiftShader software rasteriser: frame rate is meaningless; only DOM and context readiness are asserted.',
      ],
    };
    await fs.writeFile(path.join(reportDirectory, 'browser-check.json'), `${JSON.stringify(report, null, 2)}\n`);
    client.close();
    console.log(JSON.stringify({ ok: failures.length === 0, failures, screenshot: report.screenshot, webgl: dom.webgl }));
    if (failures.length) process.exitCode = 1;
  } finally {
    browser.kill();
  }
}

main().catch((error) => {
  console.error(`${error.code ?? 'NG-E-VEG-BROWSER'}: ${error.message}`);
  process.exitCode = 1;
});
