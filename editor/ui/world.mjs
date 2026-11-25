// World workspace: the native map editing surface.
//
// This is a self-contained overlay rather than a panel bolted into app.mjs, for one reason:
// world authoring has its own coordinate space, its own undo stack and its own task model,
// and threading all three through the scene editor's state would have made both harder to
// reason about. The overlay talks to the same `/api/call` endpoint and the same native
// viewport as everything else, so it is not a separate tool — it is a separate view of the
// same controlled API.
//
// Everything the user can do here maps one-to-one onto a `world_*` action, which is also
// what the AI tool surface calls. There is no UI-only capability.

const API = '/api/call';

const state = {
  token: null,
  open: false,
  worldId: null,
  revision: 0,
  draftSeq: 0,
  layers: [],
  chunks: [],
  extent: [256, 256],
  origin: [-128, -128],
  chunkSize: 128,
  activeLayer: null,
  tool: 'brush',
  brush: { radiusMetres: 12, strength: 1, falloffMetres: 6, erase: false, mode: 'raise' },
  pathWidth: 6,
  task: null,
  lastApply: null,
  stroke: [],
  drawing: false,
  view: { scale: 2, offsetX: 0, offsetY: 0 },
  dragging: null,
};

let root = null;
let canvas = null;
let context = null;

async function session() {
  if (state.token) return state.token;
  const response = await fetch('/api/session');
  const body = await response.json();
  state.token = body.token;
  return state.token;
}

async function call(action, args = {}) {
  const token = await session();
  const response = await fetch(API, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-NativeGPU-Token': token },
    body: JSON.stringify({ action, args }),
  });
  const body = await response.json();
  if (body && body.ok === false) {
    const error = new Error(body.error?.message ?? '世界操作失败');
    error.code = body.error?.code;
    error.details = body.error?.details;
    throw error;
  }
  return body;
}

// ---------------------------------------------------------------- DOM

function element(tag, attributes = {}, children = []) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) {
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else node.setAttribute(key, value);
  }
  for (const child of [].concat(children)) if (child) node.append(child);
  return node;
}

function buildOverlay() {
  const worldSelect = element('select', { id: 'world-select' });
  const status = element('div', { class: 'world-status', id: 'world-status', text: '尚未打开世界' });
  const layerList = element('div', { class: 'world-layers', id: 'world-layers' });
  const diagnostics = element('div', { class: 'world-diagnostics', id: 'world-diagnostics' });
  const taskPanel = element('div', { class: 'world-task', id: 'world-task' });

  const brushRadius = element('input', { type: 'range', min: '1', max: '120', value: '12', id: 'brush-radius' });
  const brushFalloff = element('input', { type: 'range', min: '0', max: '60', value: '6', id: 'brush-falloff' });
  const brushStrength = element('input', { type: 'range', min: '0', max: '100', value: '100', id: 'brush-strength' });
  const brushMode = element('select', { id: 'brush-mode' },
    ['raise', 'lower', 'smooth', 'flatten', 'paint'].map(mode => element('option', { value: mode, text: mode })));
  const pathWidth = element('input', { type: 'range', min: '1', max: '60', value: '6', id: 'path-width' });

  brushRadius.addEventListener('input', () => { state.brush.radiusMetres = Number(brushRadius.value); draw(); });
  brushFalloff.addEventListener('input', () => { state.brush.falloffMetres = Number(brushFalloff.value); });
  brushStrength.addEventListener('input', () => { state.brush.strength = Number(brushStrength.value) / 100; });
  brushMode.addEventListener('change', () => { state.brush.mode = brushMode.value; });
  pathWidth.addEventListener('input', () => { state.pathWidth = Number(pathWidth.value); });

  const overlay = element('div', { class: 'world-workspace', id: 'world-workspace', hidden: '' }, [
    element('header', { class: 'world-header' }, [
      element('strong', { text: '世界生成' }),
      worldSelect,
      element('button', { class: 'world-action', id: 'world-refresh', text: '刷新', onclick: refreshWorlds }),
      element('button', { class: 'world-action', id: 'world-create', text: '新建世界', onclick: createWorld }),
      element('span', { class: 'world-spacer' }),
      element('button', { class: 'world-action', id: 'world-viewport', text: '在原生视口查看', onclick: showInViewport }),
      element('button', { class: 'world-action', id: 'world-close', text: '关闭', onclick: close }),
    ]),
    element('div', { class: 'world-body' }, [
      element('aside', { class: 'world-side' }, [
        element('h3', { text: '图层' }),
        layerList,
        element('h3', { text: '画笔' }),
        element('label', { class: 'world-field' }, [element('span', { text: '半径 (m)' }), brushRadius]),
        element('label', { class: 'world-field' }, [element('span', { text: '边缘衰减 (m)' }), brushFalloff]),
        element('label', { class: 'world-field' }, [element('span', { text: '强度' }), brushStrength]),
        element('label', { class: 'world-field' }, [element('span', { text: '模式' }), brushMode]),
        element('label', { class: 'world-field' }, [element('span', { text: '路径宽度 (m)' }), pathWidth]),
        element('div', { class: 'world-tools' }, [
          element('button', { class: 'world-action active', 'data-tool': 'brush', text: '区域笔刷', onclick: () => setTool('brush') }),
          element('button', { class: 'world-action', 'data-tool': 'path', text: '路径', onclick: () => setTool('path') }),
          element('button', { class: 'world-action', 'data-tool': 'erase', text: '擦除', onclick: () => setTool('erase') }),
        ]),
      ]),
      element('main', { class: 'world-canvas-wrap' }, [
        element('canvas', { id: 'world-canvas' }),
        status,
      ]),
      element('aside', { class: 'world-side world-right' }, [
        element('h3', { text: '任务' }),
        taskPanel,
        element('div', { class: 'world-tools' }, [
          element('button', { class: 'world-action', id: 'world-preview', text: '预览', onclick: preview }),
          element('button', { class: 'world-action primary', id: 'world-apply', text: '应用', onclick: apply }),
          element('button', { class: 'world-action', id: 'world-undo', text: '撤销', onclick: () => history('undo') }),
          element('button', { class: 'world-action', id: 'world-redo', text: '重做', onclick: () => history('redo') }),
        ]),
        element('h3', { text: '诊断' }),
        diagnostics,
      ]),
    ]),
  ]);

  worldSelect.addEventListener('change', () => openWorld(worldSelect.value));
  canvas = overlay.querySelector('#world-canvas');
  context = canvas.getContext('2d');
  canvas.addEventListener('pointerdown', onPointerDown);
  canvas.addEventListener('pointermove', onPointerMove);
  canvas.addEventListener('pointerup', onPointerUp);
  canvas.addEventListener('pointercancel', onPointerUp);
  canvas.addEventListener('wheel', onWheel, { passive: false });
  return overlay;
}

function setTool(tool) {
  state.tool = tool;
  state.brush.erase = tool === 'erase';
  for (const button of root.querySelectorAll('[data-tool]')) {
    button.classList.toggle('active', button.dataset.tool === tool);
  }
}

// ---------------------------------------------------------------- world lifecycle

async function refreshWorlds() {
  const result = await call('world_list', {});
  const select = root.querySelector('#world-select');
  select.replaceChildren(...(result.worlds ?? []).map(world =>
    element('option', { value: world.worldId, text: `${world.worldId} · ${world.extentMetres?.[0] ?? '?'} m` })));
  if (state.worldId) select.value = state.worldId;
  return result.worlds ?? [];
}

async function openWorld(worldId) {
  if (!worldId) return;
  const result = await call('world_open', { worldId });
  const world = result.world;
  state.worldId = world.worldId;
  state.revision = world.revision;
  state.draftSeq = world.draftSeq ?? 0;
  state.layers = world.layers;
  state.extent = world.recipe?.extentMetres ?? [256, 256];
  state.origin = [world.recipe?.originMetres?.[0] ?? -128, world.recipe?.originMetres?.[2] ?? -128];
  state.chunkSize = world.recipe?.chunkSizeMetres ?? 128;
  state.activeLayer = state.layers.find(layer => ['scatter', 'terrain'].includes(layer.kind))?.id ?? state.layers[0]?.id ?? null;
  await refreshQuery();
  renderLayers();
  draw();
  setStatus(`已打开 ${state.worldId}，修订 ${state.revision}`);
}

async function refreshQuery() {
  const query = await call('world_query', {});
  state.chunks = query.chunks ?? [];
  state.extent = query.extentMetres ?? state.extent;
  state.origin = [query.originMetres?.[0] ?? state.origin[0], query.originMetres?.[2] ?? state.origin[1]];
  state.chunkSize = query.chunkSizeMetres ?? state.chunkSize;
  state.revision = query.revision;
  state.draftSeq = query.draftSeq ?? state.draftSeq;
  if (query.layers) state.layers = query.layers;
  renderLayers();
  return query;
}

async function createWorld() {
  const id = window.prompt('新世界标识（字母、数字、下划线）', 'ancient-valley-2');
  if (!id) return;
  const extent = Number(window.prompt('世界边长（米）', '256') ?? 256);
  try {
    await call('world_create', { worldId: id, extentMetres: extent });
    await refreshWorlds();
    await openWorld(id);
  } catch (error) {
    setStatus(error.message, true);
  }
}

// ---------------------------------------------------------------- layers

function renderLayers() {
  const list = root.querySelector('#world-layers');
  list.replaceChildren(...state.layers.map(layer => {
    const button = element('button', {
      class: `world-layer${layer.id === state.activeLayer ? ' active' : ''}`,
      onclick: () => { state.activeLayer = layer.id; renderLayers(); },
    }, [
      element('span', { class: 'world-layer-kind', text: layer.kind }),
      element('span', { class: 'world-layer-id', text: layer.id }),
    ]);
    const visible = element('input', { type: 'checkbox', title: '可见' });
    visible.checked = layer.visible !== false;
    visible.addEventListener('click', event => event.stopPropagation());
    visible.addEventListener('change', async () => {
      await call('world_preview', { layers: [{ id: layer.id, visible: visible.checked }] });
      await refreshQuery();
    });
    const locked = element('input', { type: 'checkbox', title: '锁定' });
    locked.checked = layer.locked === true;
    locked.addEventListener('click', event => event.stopPropagation());
    locked.addEventListener('change', async () => {
      await call('world_preview', { layers: [{ id: layer.id, locked: locked.checked }] });
      await refreshQuery();
    });
    button.append(visible, locked);
    return button;
  }));
}

// ---------------------------------------------------------------- canvas

function resizeCanvas() {
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.max(1, Math.round(rect.width * dpr));
  canvas.height = Math.max(1, Math.round(rect.height * dpr));
  context.setTransform(dpr, 0, 0, dpr, 0, 0);
  fitView();
  draw();
}

function fitView() {
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  const [width, depth] = state.extent;
  const scale = Math.min(rect.width / width, rect.height / depth) * 0.86;
  state.view.scale = scale;
  state.view.offsetX = rect.width * 0.5 - (state.origin[0] + width * 0.5) * scale;
  state.view.offsetY = rect.height * 0.5 - (state.origin[1] + depth * 0.5) * scale;
}

function toScreen(x, z) {
  return [state.view.offsetX + x * state.view.scale, state.view.offsetY + z * state.view.scale];
}

function toWorld(clientX, clientY) {
  const rect = canvas.getBoundingClientRect();
  const x = (clientX - rect.left - state.view.offsetX) / state.view.scale;
  const z = (clientY - rect.top - state.view.offsetY) / state.view.scale;
  return [x, z];
}

function draw() {
  if (!context) return;
  const rect = canvas.getBoundingClientRect();
  context.clearRect(0, 0, rect.width, rect.height);
  context.fillStyle = '#141518';
  context.fillRect(0, 0, rect.width, rect.height);

  const [originX, originZ] = state.origin;
  const [width, depth] = state.extent;
  const [x0, y0] = toScreen(originX, originZ);
  context.fillStyle = '#1d2a1c';
  context.fillRect(x0, y0, width * state.view.scale, depth * state.view.scale);

  // Chunk grid: the unit the compiler and the runtime actually work in, so it is the grid
  // the user sees. A brush that spans two chunks is visibly a two-chunk edit.
  context.strokeStyle = 'rgba(255,255,255,0.12)';
  context.lineWidth = 1;
  for (let x = originX; x <= originX + width + 0.001; x += state.chunkSize) {
    const [sx] = toScreen(x, 0);
    context.beginPath(); context.moveTo(sx, y0); context.lineTo(sx, y0 + depth * state.view.scale); context.stroke();
  }
  for (let z = originZ; z <= originZ + depth + 0.001; z += state.chunkSize) {
    const [, sy] = toScreen(0, z);
    context.beginPath(); context.moveTo(x0, sy); context.lineTo(x0 + width * state.view.scale, sy); context.stroke();
  }

  context.strokeStyle = 'rgba(255,255,255,0.35)';
  context.strokeRect(x0, y0, width * state.view.scale, depth * state.view.scale);

  for (const layer of state.layers) {
    const active = layer.id === state.activeLayer;
    context.strokeStyle = active ? '#7fd1a6' : 'rgba(160,200,255,0.55)';
    context.lineWidth = active ? 2 : 1;
    for (const path of layer.paths ?? []) {
      if (!Array.isArray(path.points) || path.points.length < 2) continue;
      context.beginPath();
      path.points.forEach((point, index) => {
        const [sx, sy] = toScreen(point[0], point[1]);
        if (index === 0) context.moveTo(sx, sy); else context.lineTo(sx, sy);
      });
      context.stroke();
    }
    context.fillStyle = active ? 'rgba(127,209,166,0.14)' : 'rgba(160,200,255,0.08)';
    for (const region of layer.regions ?? []) {
      if (region.shape === 'circle' && Array.isArray(region.center)) {
        const [sx, sy] = toScreen(region.center[0], region.center[1]);
        context.beginPath();
        context.arc(sx, sy, Math.max(2, (region.radiusMetres ?? 0) * state.view.scale), 0, Math.PI * 2);
        context.fill();
      }
    }
  }

  if (state.stroke.length > 0) {
    context.strokeStyle = state.brush.erase ? '#e2795f' : '#ffe08a';
    context.lineWidth = Math.max(1, state.brush.radiusMetres * 2 * state.view.scale * 0.5);
    context.globalAlpha = 0.4;
    context.beginPath();
    state.stroke.forEach((point, index) => {
      const [sx, sy] = toScreen(point[0], point[1]);
      if (index === 0) context.moveTo(sx, sy); else context.lineTo(sx, sy);
    });
    context.stroke();
    context.globalAlpha = 1;
  }

  const [cx, cz] = toScreen(0, 0);
  context.fillStyle = 'rgba(255,255,255,0.45)';
  context.fillRect(cx - 3, cz - 3, 6, 6);
}

function onWheel(event) {
  event.preventDefault();
  const factor = event.deltaY < 0 ? 1.12 : 1 / 1.12;
  const rect = canvas.getBoundingClientRect();
  const px = event.clientX - rect.left;
  const py = event.clientY - rect.top;
  state.view.offsetX = px - (px - state.view.offsetX) * factor;
  state.view.offsetY = py - (py - state.view.offsetY) * factor;
  state.view.scale *= factor;
  draw();
}

function onPointerDown(event) {
  if (event.button === 1 || (event.button === 0 && event.shiftKey)) {
    state.dragging = { x: event.clientX, y: event.clientY, offsetX: state.view.offsetX, offsetY: state.view.offsetY };
    canvas.setPointerCapture(event.pointerId);
    return;
  }
  if (event.button !== 0) return;
  state.drawing = true;
  state.stroke = [toWorld(event.clientX, event.clientY)];
  canvas.setPointerCapture(event.pointerId);
  draw();
}

function onPointerMove(event) {
  if (state.dragging) {
    state.view.offsetX = state.dragging.offsetX + (event.clientX - state.dragging.x);
    state.view.offsetY = state.dragging.offsetY + (event.clientY - state.dragging.y);
    draw();
    return;
  }
  if (!state.drawing) return;
  const point = toWorld(event.clientX, event.clientY);
  const previous = state.stroke[state.stroke.length - 1];
  // Resampling happens in the service, but the canvas should not accumulate a point per
  // pointer event either: the preview line would cost more than the cook.
  if (!previous || Math.hypot(point[0] - previous[0], point[1] - previous[1]) * state.view.scale > 3) {
    state.stroke.push(point);
    draw();
  }
}

function onPointerUp(event) {
  if (state.dragging) {
    state.dragging = null;
    try { canvas.releasePointerCapture(event.pointerId); } catch { /* already released */ }
    return;
  }
  if (!state.drawing) return;
  state.drawing = false;
  try { canvas.releasePointerCapture(event.pointerId); } catch { /* already released */ }
  const stroke = state.stroke;
  state.stroke = [];
  draw();
  if (stroke.length === 0 || !state.activeLayer) return;
  if (state.tool === 'path') {
    submitPath(stroke);
    return;
  }
  submitStroke(stroke);
}

// ---------------------------------------------------------------- actions

async function submitStroke(points) {
  const layer = state.layers.find(entry => entry.id === state.activeLayer);
  const isPathLayer = layer && ['water', 'road'].includes(layer.kind);
  const stroke = {
    layerId: state.activeLayer,
    points,
    radiusMetres: state.brush.radiusMetres,
    falloffMetres: state.brush.falloffMetres,
    strength: state.brush.strength,
    erase: state.brush.erase || state.tool === 'erase',
    mode: state.brush.mode,
    shape: isPathLayer ? 'path' : 'circle',
    widthMetres: state.pathWidth,
  };
  setStatus('正在生成预览…');
  try {
    const result = await call('world_preview', { strokes: [stroke], label: 'brush' });
    applyTaskResult(result.task);
  } catch (error) {
    setStatus(error.message, true);
  }
}

async function submitPath(points) {
  setStatus('正在生成路径预览…');
  try {
    const result = await call('world_preview', {
      strokes: [{
        layerId: state.activeLayer,
        points,
        shape: 'path',
        widthMetres: state.pathWidth,
      }],
      label: 'path',
    });
    applyTaskResult(result.task);
  } catch (error) {
    setStatus(error.message, true);
  }
}

function applyTaskResult(task) {
  state.task = task;
  const panel = root.querySelector('#world-task');
  if (!task) { panel.replaceChildren(); return; }
  const lines = [
    `状态 ${task.state}`,
    `任务 ${task.taskId.slice(0, 8)}`,
    task.result ? `块 ${task.result.chunks} · 实例 ${task.result.instances} · 建筑 ${task.result.buildings}` : null,
    task.result ? `耗时 ${task.result.cookMillis} ms` : null,
    task.result && task.result.budgetScale < 1 ? `密度削减至 ${Math.round(task.result.budgetScale * 100)}%` : null,
  ].filter(Boolean);
  panel.replaceChildren(...lines.map(text => element('div', { class: 'world-task-line', text })));
  const diagnostics = root.querySelector('#world-diagnostics');
  diagnostics.replaceChildren(...(task.diagnostics ?? []).map(entry =>
    element('div', { class: `world-diagnostic ${entry.code}`, text: `${entry.code}: ${entry.message}` })));
  if (task.state === 'ready') setStatus(`预览就绪：${task.result?.instances ?? 0} 个实例`);
  else if (task.state === 'failed') setStatus(task.error?.message ?? '预览失败', true);
  refreshQuery().catch(() => {});
}

async function preview() {
  setStatus('正在重新生成…');
  try {
    const result = await call('world_preview', { label: 'refresh' });
    applyTaskResult(result.task);
  } catch (error) {
    setStatus(error.message, true);
  }
}

async function apply() {
  if (!state.task || state.task.state !== 'ready') { setStatus('先预览，再应用', true); return; }
  try {
    const result = await call('world_apply', {
      taskId: state.task.taskId,
      expectedRevision: state.task.baseRevision,
      // The world draft revision and the scene revision are separate counters; the scene
      // transaction resolves its own current revision when this is absent.
      sceneRevision: state.sceneRevision ?? undefined,
    });
    state.revision = result.revision;
    state.lastApply = result.world;
    setStatus(`已应用：清单 ${result.world.manifestHash.slice(0, 12)}`);
    await refreshQuery();
  } catch (error) {
    setStatus(error.message, true);
  }
}

async function history(kind) {
  try {
    const result = await call(`world_${kind}`, {});
    setStatus(result.ok ? `${kind === 'undo' ? '已撤销' : '已重做'}（草稿 ${result.revision}）` : result.reason);
    await refreshQuery();
    draw();
  } catch (error) {
    setStatus(error.message, true);
  }
}

async function showInViewport() {
  if (!state.lastApply) { setStatus('先应用一次世界，再在视口中查看', true); return; }
  const directory = state.lastApply.directory
    ?? `assets/${state.lastApply.cookedManifest.file.replace(/\/manifest\.json$/, '')}`;
  if (!window.forgeViewport?.command) { setStatus('当前环境没有原生视口', true); return; }
  const result = await window.forgeViewport.command({ command: 'world', cooked: directory, revision: state.revision });
  setStatus(result?.ok ? '原生视口已切换到世界' : (result?.error ?? '原生视口拒绝了请求'), !result?.ok);
}

function setStatus(message, error = false) {
  const node = root?.querySelector('#world-status');
  if (!node) return;
  node.textContent = message;
  node.classList.toggle('error', Boolean(error));
}

// ---------------------------------------------------------------- lifecycle

export async function openWorkspace() {
  if (!root) {
    root = buildOverlay();
    document.body.append(root);
    new ResizeObserver(() => resizeCanvas()).observe(canvas);
  }
  root.hidden = false;
  state.open = true;
  resizeCanvas();
  try {
    const worlds = await refreshWorlds();
    if (worlds.length && !state.worldId) await openWorld(worlds[0].worldId);
  } catch (error) {
    setStatus(error.message, true);
  }
}

export function closeWorkspace() {
  if (root) root.hidden = true;
  state.open = false;
}

export function toggleWorkspace() {
  if (state.open) closeWorkspace(); else openWorkspace();
}

export function isOpen() { return state.open; }

// A toolbar button in index.html calls this through a module import; the overlay is only
// built the first time, so opening it twice does not create a second canvas.
window.worldWorkspace = { open: openWorkspace, close: closeWorkspace, toggle: toggleWorkspace, isOpen };
