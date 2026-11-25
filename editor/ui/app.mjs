const $ = id => document.getElementById(id);
const bridge = window.forgeViewport ?? null;
const KIND = {
  scene: { tab: '主视图', icon: '▱', title: '大纲', views: ['透视', '顶视', '前视', '运行画面'], inspectors: ['对象', '材质', '光照', '玩法'] },
  material: { tab: '材质', icon: '●', title: '材质库', views: ['材质球', '场景预览', '前后对比', '通道'], inspectors: ['对象', '材质', '光照', '玩法'] },
  character: { tab: '角色', icon: '♙', title: '片段', views: ['运行画面', '骨架', '蒙皮', '前后对比'], inspectors: ['属性', '事件', '混合'] },
  animation: { tab: '动画', icon: '◇', title: '片段', views: ['运行画面', '骨架', '蒙皮', '前后对比'], inspectors: ['属性', '事件', '混合'] },
  effect: { tab: '特效', icon: '✧', title: '特效与植被', views: ['运行画面', '粒子调试', '植被密度', '风场'], inspectors: ['参数', '曲线', '植被'] },
  vegetation: { tab: '特效', icon: '♧', title: '特效与植被', views: ['运行画面', '粒子调试', '植被密度', '风场'], inspectors: ['参数', '曲线', '植被'] },
  audio: { tab: '音频', icon: '≋', title: '声源', views: ['俯视声场', '运行画面', '遮挡调试', '混音'], inspectors: ['声源', '遮挡', '混音'] },
  diagnostics: { tab: '诊断', icon: '⚠', title: '大纲', views: ['叠加视图', '遮挡视图', '可达性', '预算热区'], inspectors: ['问题详情', '场景设置'] },
};
const ENTITY = { ground: '地面', wall: '墙', prop: '道具', player: '玩家', key: '钥匙', door: '门', goal: '终点', light: '光照', vehicle: '载具' };
const TYPE = { scene: '关卡', material: '材质', character: '角色', animation: '动画', effect: '粒子', vegetation: '植被', audio: '音频', model: '模型', texture: '贴图', environment: '环境' };
const ICON = { scene: 'folder', material: 'circle', character: 'user', animation: 'user', effect: 'sparkles', vegetation: 'trees', audio: 'chart-bar', diagnostics: 'alert-triangle', model: 'box', texture: 'photo', environment: 'world' };
const TOOL_ICON = { select: 'point', move: 'arrows-move', rotate: 'rotate', scale: 'arrows-maximize', grid: 'layout-grid' };
let icons = {};
const state = {
  project: null, scene: null, scenePath: '', textures: [], workspace: null, imports: [], vegetation: null,
  assets: [], tabs: [], activeId: null, selectedEntity: null, canUndo: false, drafts: new Map(), created: new Map(), removed: new Set(),
  history: [], redo: [],
  token: null, busy: false, logs: [], bottom: 'scene', managerCategory: 'all', managerSelected: null,
  projects: [], activeProject: null, projectsRoot: '',
  nativeRevision: 0, nativeAck: null, nativeRequest: 0, pendingPreviewSync: 0,
  materialLayer: 0, materialDebug: '',
  runtime: { running: false },
};
let toastTimer;

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = String(text);
  return node;
}
function clear(node) { node.replaceChildren(); }
function icon(name) {
  const node = el('span', 'ui-icon');
  const source = icons[name] ?? icons.file;
  if (source) {
    node.style.maskImage = `url("${source}")`;
    node.style.webkitMaskImage = `url("${source}")`;
  } else node.textContent = '◇';
  return node;
}
async function loadIcons() {
  try {
    const response = await fetch('/media/icons.json', { cache: 'no-store' });
    if (response.ok) icons = await response.json();
  } catch (error) { log(`图标资源读取失败：${error.message}`, true); }
}
function activeTab() { return state.tabs.find(tab => tab.id === state.activeId) ?? null; }
function dirty() { return state.drafts.size || state.created.size || state.removed.size; }
function fmt(value) { return Number.isFinite(Number(value)) ? Number(value).toLocaleString('zh-CN', { maximumFractionDigits: 3 }) : '—'; }
function showError(error) {
  clearTimeout(toastTimer);
  $('toast').textContent = `${error.code ? `[${error.code}] ` : ''}${error.message ?? error}`;
  $('toast').className = 'toast error'; $('toast').hidden = false;
  toastTimer = setTimeout(() => { $('toast').hidden = true; }, 6500);
  log(error.message ?? String(error), true);
}
function notify(message) {
  clearTimeout(toastTimer); $('toast').textContent = message; $('toast').className = 'toast'; $('toast').hidden = false;
  toastTimer = setTimeout(() => { $('toast').hidden = true; }, 3500);
}
function log(message, error = false) {
  state.logs.unshift({ time: new Date().toLocaleTimeString('zh-CN', { hour12: false }), message, error });
  state.logs.length = Math.min(state.logs.length, 120);
  if (state.bottom === 'console') renderBottom();
}
async function call(action, args = {}) {
  let result;
  if (window.nativegpu?.call) result = await window.nativegpu.call(action, args);
  else {
    if (!state.token) {
      const response = await fetch('/api/session', { cache: 'no-store' });
      if (!response.ok) throw new Error('无法连接本地编辑器服务');
      state.token = (await response.json()).token;
    }
    const response = await fetch('/api/call', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-NativeGPU-Token': state.token }, body: JSON.stringify({ action, args }) });
    result = await response.json();
  }
  if (!result.ok) {
    const error = new Error(result.error?.message ?? '操作失败'); error.code = result.error?.code; error.details = result.error?.details; throw error;
  }
  return result.result;
}
async function operation(work) {
  if (state.busy) return;
  state.busy = true; renderStatus();
  try { return await work(); } catch (error) { showError(error); }
  finally { state.busy = false; renderStatus(); }
}

function assetId(kind, value) { return `${kind}:${value}`; }
function importedId(file) { return file?.match(/(?:^|assets\/)imported\/([a-f0-9]{64})\//i)?.[1] ?? null; }
function buildAssets() {
  const assets = [];
  for (const scene of state.project?.scenes ?? []) assets.push({ id: assetId('scene', scene.path), kind: 'scene', name: scene.name ?? scene.path, path: scene.path, scenePath: scene.path, detail: '关卡 · 原生场景' });
  for (const [name, material] of Object.entries(state.workspace?.materials ?? {})) assets.push({ id: assetId('material', name), kind: 'material', name, material: name, scenePath: state.scenePath, path: `${state.scenePath}#${name}`, detail: `材质 · ${material.alphaMode ?? 'OPAQUE'}` });
  for (const item of state.imports) {
    if (item.error || !['character', 'model'].includes(item.kind)) continue;
    assets.push({ id: assetId('character', item.id), kind: 'character', name: item.name ?? item.id.slice(0, 8), asset: item.id, scenePath: state.scenePath, path: `assets/imported/${item.id}`, detail: `${TYPE[item.kind]} · 导入资产` });
  }
  for (const character of state.workspace?.characters ?? []) {
    const asset = importedId(character.file);
    if (!asset && !character.error) assets.push({ id: assetId('character', character.file), kind: 'character', name: character.file.startsWith('builtin/') ? '基础机器人' : character.name, file: character.file, scenePath: state.scenePath, path: `assets/${character.file}`, detail: '角色 · 引擎基础资源' });
    for (const clip of character.clips ?? []) assets.push({ id: assetId('animation', `${character.file}/${clip.name}`), kind: 'animation', name: clip.name, character: character.name, file: character.file, asset, clip: clip.name, clipData: clip, scenePath: state.scenePath, path: character.file, detail: `动画 · ${fmt(clip.duration)} s` });
  }
  for (const effect of state.workspace?.effects ?? []) assets.push({ id: assetId('effect', effect.file), kind: 'effect', name: effect.name, effect: effect.id ?? effect.name, library: effect.file, effectData: effect, scenePath: state.scenePath, path: effect.file, detail: '粒子 · 特效定义' });
  for (const species of state.vegetation?.species ?? []) assets.push({ id: assetId('vegetation', species.id), kind: 'vegetation', name: species.id, species: species.id, digest: species.baked?.hash, speciesData: species, scenePath: state.scenePath, path: `assets/vegetation/species/${species.id}.species.json`, detail: species.baked ? '植被 · 已烘焙' : '植被 · 未烘焙' });
  for (const emitter of state.workspace?.audio?.emitters ?? []) assets.push({ id: assetId('audio', emitter.id), kind: 'audio', name: emitter.id, emitter, scenePath: state.scenePath, path: emitter.sound, detail: `音频 · ${emitter.bus ?? 'sfx'}` });
  state.assets = assets;
  $('issue-count').textContent = state.workspace?.diagnostics?.length ?? 0;
}
async function loadCatalogs() {
  const [workspace, imports, vegetation] = await Promise.all([
    call('workspace_data', { scenePath: state.scenePath }), call('asset_list'), call('vegetation.list'),
  ]);
  state.workspace = workspace; state.imports = imports; state.vegetation = vegetation; buildAssets();
}
function adopt(result) {
  state.scene = result.scene; state.scenePath = result.scenePath ?? state.scenePath; state.textures = result.textures ?? [];
  state.canUndo = Boolean(result.canUndo); state.drafts.clear(); state.created.clear(); state.removed.clear();
  state.history.length = 0; state.redo.length = 0;
  if (!state.scene.entities.some(entity => entity.id === state.selectedEntity)) state.selectedEntity = state.scene.entities[0]?.id ?? null;
}
async function openScene(path) {
  if (dirty()) throw new Error('请先保存当前地图修改，再打开另一张地图。');
  adopt(await call('scene_open', { scenePath: path })); await loadCatalogs();
  state.tabs = [];
  const sceneAsset = state.assets.find(asset => asset.kind === 'scene' && asset.scenePath === path);
  openAsset(sceneAsset, false);
  log(`已打开地图 ${state.scene.name} · revision ${state.scene.revision}`);
}

function openAsset(asset, activate = true) {
  if (!asset) return;
  if (asset.kind === 'scene' && activate && asset.scenePath !== state.scenePath) { operation(() => openScene(asset.scenePath)); return; }
  let tab = state.tabs.find(item => item.id === asset.id);
  if (!tab) { tab = { ...asset, view: 0, inspector: asset.kind === 'material' ? 1 : 0 }; state.tabs.push(tab); }
  if (asset.kind === 'scene') state.selectedEntity = state.scene.entities[0]?.id ?? null;
  if (activate || !state.tabs.some(item => item.id === state.activeId)) state.activeId = tab.id;
  renderAll(); activateNative();
  if ($('asset-manager').open) $('asset-manager').close();
}
function openDiagnostics(issue = null) {
  const asset = { id: 'diagnostics:scene', kind: 'diagnostics', name: '诊断', scenePath: state.scenePath, issue };
  openAsset(asset);
}
function closeTab(id) {
  const index = state.tabs.findIndex(tab => tab.id === id); if (index < 0) return;
  const closingActive = state.activeId === id; state.tabs.splice(index, 1);
  if (!state.tabs.length) {
    const sceneAsset = state.assets.find(asset => asset.kind === 'scene' && asset.scenePath === state.scenePath);
    if (sceneAsset) state.tabs.push({ ...sceneAsset, view: 0, inspector: 0 });
  }
  if (closingActive) { state.activeId = state.tabs[Math.max(0, index - 1)]?.id ?? state.tabs[0]?.id; activateNative(); }
  renderAll();
}

function renderAll() {
  renderTabs(); renderLeft(); renderInspector(); renderBottom(); renderViewStrip(); renderStatus();
  const tab = activeTab();
  $('context-select').replaceChildren();
  const option = el('option', '', TYPE[tab?.kind] ?? '关卡'); $('context-select').append(option);
  $('left-search').placeholder = ({ scene: '搜索实体或组件', material: '搜索材质或参数', character: '搜索片段或事件', animation: '搜索片段或事件', effect: '搜索特效或植被', vegetation: '搜索特效或植被', audio: '搜索声源或事件', diagnostics: '搜索实体或组件' })[tab?.kind] ?? '搜索';
}
function renderTabs() {
  const container = $('resource-tabs'); clear(container);
  for (const tab of state.tabs) {
    const button = el('button', `resource-tab${tab.id === state.activeId ? ' active' : ''}`);
    button.setAttribute('role', 'tab'); button.setAttribute('aria-selected', String(tab.id === state.activeId));
    button.append(icon(assetIcon(tab)), el('span', 'tab-name', tab.name));
    const close = el('span', 'close', '×'); close.addEventListener('click', event => { event.stopPropagation(); closeTab(tab.id); });
    button.append(close); button.addEventListener('click', () => { state.activeId = tab.id; renderAll(); activateNative(); });
    button.title = tab.name; container.append(button);
  }
}
function renderViewStrip() {
  const tab = activeTab(); const modes = $('view-modes'); clear(modes);
  for (const [index, label] of (KIND[tab?.kind]?.views ?? []).entries()) {
    const button = el('button', index === (tab.view ?? 0) ? 'active' : '', label);
    button.addEventListener('click', () => { tab.view = index; renderViewStrip(); sendViewMode(); }); modes.append(button);
  }
  const metas = {
    scene: '编辑相机 · 物理暂停 · 原生实时', material: 'PBR · 实时原生材质预览',
    character: '实时预览 · 原生骨骼动画', animation: '实时预览 · 原生骨骼动画',
    effect: '实时 · 原生粒子模拟', vegetation: '实时 · 原生植被与 LOD',
    audio: '俯视 · 声压着色 · 听者 · 遮挡线', diagnostics: '着色 · 问题标记 · 网格',
  };
  $('view-meta').textContent = metas[tab?.kind] ?? '';
  const selection = $('viewport-selection'); selection.hidden = !tab;
  if (tab) selection.textContent = `${tab.name} · ${TYPE[tab.kind] ?? '诊断'} · 原生实时`;
  $('viewport-camera').hidden = tab?.kind !== 'scene';
  const play = state.nativeAck?.play ?? 'edit';
  const projection = state.nativeAck?.orthographic ? '正交' : '透视';
  $('viewport-camera').textContent = `编辑相机 · ${projection} · ${play === 'run' ? '运行画面' : play === 'simulate' ? '物理模拟' : '编辑'} · Alt+左键环绕 · 中键平移 · 右键漫游 · 滚轮缩放 · F 聚焦`;
}
function sendViewMode() {
  const tab = activeTab(); if (!bridge || !tab) return;
  const mode = KIND[tab.kind].views[tab.view ?? 0];
  const revision = state.nativeRevision;
  if (['scene', 'diagnostics', 'audio'].includes(tab.kind)) {
    // Scene tabs map the strip to the real edit camera and play state.
    if (mode === '运行画面') {
      bridge.command({ command: 'play', play: 'run', pause: false, revision }).then(result => { if (!result?.ok) showError(new Error(result?.error ?? '无法进入运行画面')); });
      return;
    }
    if (state.nativeAck?.play === 'run' || state.nativeAck?.play === 'simulate') {
      bridge.command({ command: 'play', play: 'edit', pause: false, revision });
    }
    const view = ({ '透视': 'perspective', '顶视': 'top', '前视': 'front' })[mode] ?? 'perspective';
    bridge.command({ command: 'camera', view, revision }).then(result => { if (!result?.ok) showError(new Error(result?.error ?? '相机切换失败')); });
    return;
  }
  const debug = ({ '通道': 'basecolor', '骨架': 'normal', '蒙皮': 'normal', '粒子调试': 'normal', '植被密度': 'wind', '风场': 'wind' })[mode] ?? 'shaded';
  bridge.command({ command: 'view', debug, revision }).then(result => { if (!result?.ok) showError(new Error(result?.error ?? '原生视图切换失败')); });
}

function sceneTree(container, diagnostics = false) {
  const root = el('div', 'tree-root', state.scene?.name ?? '未打开地图'); root.append(el('span', '', state.scene?.entities.length ?? 0)); container.append(root);
  const search = $('left-search').value.trim().toLowerCase();
  for (const entity of state.scene?.entities ?? []) {
    if (state.removed.has(entity.id)) continue;
    const label = `${ENTITY[entity.kind] ?? ''} ${entity.name}`.toLowerCase(); if (search && !label.includes(search)) continue;
    const row = el('button', `tree-row${entity.id === state.selectedEntity ? ' active' : ''}`);
    row.append(el('span', 'name', `${ENTITY[entity.kind] ?? ''} ${entity.name}`), el('span', 'meta', entity.kind));
    row.addEventListener('click', () => {
      state.selectedEntity = entity.id;
      if (diagnostics) {
        const issue = state.workspace.diagnostics.find(item => item.entityId === entity.id);
        if (issue) activeTab().issue = issue;
      }
      renderLeft(); renderInspector();
    }); container.append(row);
  }
}
function materialList(container) {
  const entries = Object.entries(state.workspace?.materials ?? {});
  const root = el('div', 'tree-root', `全部材质 ${entries.length}`); container.append(root);
  const search = $('left-search').value.trim().toLowerCase();
  for (const [name, material] of entries) {
    if (search && !name.toLowerCase().includes(search)) continue;
    const row = el('button', `resource-line${activeTab()?.material === name ? ' active' : ''}`);
    row.append(el('span', 'name', name), el('span', 'meta', material.unlit ? 'Unlit' : 'PBR'));
    row.addEventListener('dblclick', () => openAsset(state.assets.find(asset => asset.id === assetId('material', name))));
    row.addEventListener('click', () => { const asset = state.assets.find(item => item.id === assetId('material', name)); if (asset) openAsset(asset); });
    container.append(row);
  }
}
function animationList(container) {
  const search = $('left-search').value.trim().toLowerCase();
  for (const character of state.workspace?.characters ?? []) {
    const root = el('div', 'tree-root', `角色 ${character.name}`); root.append(el('span', '', `${character.clips.length} 片段`)); container.append(root);
    for (const clip of character.clips) {
      if (search && !`${clip.name} ${character.name}`.toLowerCase().includes(search)) continue;
      const asset = state.assets.find(item => item.kind === 'animation' && item.file === character.file && item.clip === clip.name);
      const row = el('button', `resource-line${activeTab()?.id === asset?.id ? ' active' : ''}`);
      row.append(el('span', 'name', clip.name), el('span', 'meta', `${fmt(clip.duration)} s`));
      row.addEventListener('click', () => openAsset(asset)); container.append(row);
    }
  }
}
function effectList(container) {
  const root = el('div', 'tree-root', `粒子 · ${state.workspace?.effects.length ?? 0}`); container.append(root);
  for (const effect of state.workspace?.effects ?? []) {
    const asset = state.assets.find(item => item.kind === 'effect' && item.library === effect.file);
    const row = el('button', `resource-line${activeTab()?.id === asset?.id ? ' active' : ''}`);
    row.append(el('span', 'name', effect.name), el('span', 'meta', effect.id ?? 'ngfx'));
    row.addEventListener('click', () => openAsset(asset)); container.append(row);
  }
  container.append(el('div', 'tree-root', `植被 · ${state.vegetation?.species.length ?? 0} 层`));
  for (const species of state.vegetation?.species ?? []) {
    const asset = state.assets.find(item => item.kind === 'vegetation' && item.species === species.id);
    const row = el('button', `resource-line${activeTab()?.id === asset?.id ? ' active' : ''}`);
    row.append(el('span', 'name', species.id), el('span', 'meta', species.baked ? '已烘焙' : '源文件'));
    row.addEventListener('click', () => openAsset(asset)); container.append(row);
  }
}
function audioList(container) {
  const root = el('div', 'tree-root', `声场 ${state.scene.name}`); root.append(el('span', '', `${state.workspace?.audio.emitters?.length ?? 0} 声源`)); container.append(root);
  for (const emitter of state.workspace?.audio.emitters ?? []) {
    const asset = state.assets.find(item => item.kind === 'audio' && item.emitter.id === emitter.id);
    const row = el('button', `resource-line${activeTab()?.id === asset?.id ? ' active' : ''}`);
    row.append(el('span', 'name', emitter.id), el('span', 'meta', emitter.bus ?? 'sfx'));
    row.addEventListener('click', () => openAsset(asset)); container.append(row);
  }
}
function renderLeft() {
  const tab = activeTab(); const container = $('left-content'); clear(container);
  $('left-title').textContent = KIND[tab?.kind]?.title ?? '大纲';
  if (!tab) return;
  if (['scene', 'diagnostics'].includes(tab.kind)) sceneTree(container, tab.kind === 'diagnostics');
  else if (tab.kind === 'material') materialList(container);
  else if (['character', 'animation'].includes(tab.kind)) animationList(container);
  else if (['effect', 'vegetation'].includes(tab.kind)) effectList(container);
  else if (tab.kind === 'audio') audioList(container);
  renderSecondary(tab);
}
function renderSecondary(tab) {
  const secondary = $('left-secondary'); secondary.hidden = tab.kind === 'scene';
  if (secondary.hidden) return;
  const titles = { material: '贴图', character: '动画资产', animation: '动画资产', effect: '植被资产', vegetation: '植被资产', audio: '音频资产', diagnostics: '资产库' };
  $('secondary-title').textContent = titles[tab.kind];
  const tabs = $('secondary-tabs'); clear(tabs); const content = $('secondary-content'); clear(content);
  const labels = { material: ['贴图', '图集', '环境', '掩码'], character: ['骨架', '蒙皮', '混合'], animation: ['骨架', '蒙皮', '混合'], effect: ['树', '灌木', '草'], vegetation: ['树', '灌木', '草'], audio: ['循环', '单次', '3D 声场', '2D'], diagnostics: ['模型', '材质', '音频', '特效'] }[tab.kind];
  labels.forEach((label, index) => { const button = el('button', index === 0 ? 'active' : '', label); tabs.append(button); });
  let items = [];
  if (tab.kind === 'material') items = state.textures.map(name => [name, 'ngtex']);
  else if (['character', 'animation'].includes(tab.kind)) items = (state.workspace?.characters ?? []).flatMap(c => [`${c.file} · ${c.bones} 骨骼`, ...c.clips.map(clip => `${clip.name} · ${clip.frames} 帧`)]);
  else if (['effect', 'vegetation'].includes(tab.kind)) items = (state.vegetation?.species ?? []).map(s => `${s.id} · ${s.baked ? 'LOD 已烘焙' : '未烘焙'}`);
  else if (tab.kind === 'audio') items = (state.workspace?.audio.emitters ?? []).map(e => `${e.sound} · ${e.bus ?? 'sfx'}`);
  else items = state.assets.slice(0, 20).map(a => `${a.name} · ${TYPE[a.kind]}`);
  $('secondary-count').textContent = `${items.length} 项`;
  for (const item of items) content.append(el('div', 'secondary-asset', item));
}

function section(title) { const node = el('section', 'property-section'); node.append(el('h3', '', title)); return node; }
function row(label, value) { const node = el('div', 'property-row'); node.append(el('span', '', label), el('span', 'grow', value)); return node; }
function inputRow(label, input, output = null) { const node = el('div', 'property-row'); node.append(el('span', '', label), input); if (output) node.append(output); return node; }
function vectorEditor(entity, field) {
  const wrap = el('div', 'vector-values');
  for (let axis = 0; axis < 3; axis += 1) {
    const label = el('label', '', 'XYZ'[axis]); const input = el('input'); input.type = 'number'; input.step = 'any'; input.value = entity[field][axis];
    input.addEventListener('change', () => { const value = Number(input.value); if (!Number.isFinite(value)) { input.value = entity[field][axis]; return; } editEntity(entity.id, field, axis, value); });
    label.append(input); wrap.append(label);
  }
  return wrap;
}
function originalEntity(id) { return state.created.get(id) ?? state.scene?.entities.find(entity => entity.id === id) ?? null; }
function effectiveEntity(entity) {
  const draft = state.drafts.get(entity.id); return draft ? { ...entity, ...draft, position: draft.position ?? entity.position, scale: draft.scale ?? entity.scale, color: draft.color ?? entity.color } : entity;
}
function cloneMap(map) { return new Map([...map].map(([key, value]) => [key, structuredClone(value)])); }
function draftSnapshot() {
  return { drafts: cloneMap(state.drafts), created: cloneMap(state.created), removed: new Set(state.removed), selected: state.selectedEntity };
}
function pushHistory() {
  state.history.push(draftSnapshot());
  if (state.history.length > 100) state.history.shift();
  state.redo.length = 0;
}
function restoreDraftSnapshot(snapshot) {
  state.drafts = cloneMap(snapshot.drafts); state.created = cloneMap(snapshot.created); state.removed = new Set(snapshot.removed);
  state.selectedEntity = snapshot.selected;
}
// The native viewport owns the pixels; the page owns the draft. A draft is pushed as a
// transform override so the user sees the new placement immediately without writing the
// scene file and without rebuilding the game. Physics stays frozen in edit mode, so this
// cannot make a dynamic body fall while the user is still typing a value.
function canPreviewScene() {
  const tab = activeTab();
  return Boolean(bridge && tab && ['scene', 'diagnostics', 'audio'].includes(tab.kind) && state.nativeAck?.ok);
}
function previewOverride(entity, active = true) {
  if (!canPreviewScene() || !entity) return;
  bridge.command({
    command: 'preview', entity: entity.id, active,
    px: entity.position[0], py: entity.position[1], pz: entity.position[2],
    sx: entity.scale[0], sy: entity.scale[1], sz: entity.scale[2],
    revision: state.nativeRevision,
  }).then(result => { if (!result?.ok) log(`原生预览未应用：${result?.error ?? '未知错误'}`, true); });
}
function syncPreviewOverrides() {
  if (!canPreviewScene()) return;
  bridge.command({ command: 'previewclear', revision: state.nativeRevision });
  for (const entity of state.scene?.entities ?? []) {
    const draft = state.drafts.get(entity.id);
    if (state.removed.has(entity.id)) previewOverride({ ...entity, position: entity.position, scale: entity.scale }, false);
    else if (draft) previewOverride({ ...entity, ...draft });
  }
}
function undoDraft() {
  if (!state.history.length) return false;
  const current = draftSnapshot();
  restoreDraftSnapshot(state.history.pop());
  state.redo.push(current);
  syncPreviewOverrides(); renderAll();
  return true;
}
function redoDraft() {
  if (!state.redo.length) return false;
  const current = draftSnapshot();
  restoreDraftSnapshot(state.redo.pop());
  state.history.push(current);
  syncPreviewOverrides(); renderAll();
  return true;
}
function scenePlaybackLocked() {
  return ['simulate', 'run'].includes(state.nativeAck?.play);
}
function editEntity(id, field, axis, value) {
  if (scenePlaybackLocked()) { notify('请先停止模拟或运行，再修改场景'); return; }
  const entity = originalEntity(id); if (!entity) return;
  const draft = state.drafts.get(id) ?? { name: entity.name, position: [...entity.position], scale: [...entity.scale], color: [...entity.color], collider: entity.collider, character: entity.character ? structuredClone(entity.character) : undefined };
  if (axis === null) draft[field] = value; else draft[field][axis] = value;
  pushHistory();
  state.drafts.set(id, draft); renderStatus(); renderLeft();
  previewOverride({ ...entity, ...draft });
}
function renderObjectInspector(container, entity) {
  if (!entity) { container.append(el('p', 'inspector-note', '在大纲中选择一个对象。')); return; }
  const draft = { ...entity, ...(state.drafts.get(entity.id) ?? {}) };
  container.append(el('h2', 'inspector-title', `${ENTITY[entity.kind] ?? ''} ${draft.name}`), el('p', 'inspector-subtitle', `${entity.kind} · ${entity.id}`));
  const transform = section('变换'); transform.append(inputRow('位置', vectorEditor(draft, 'position')), inputRow('缩放', vectorEditor(draft, 'scale')), row('旋转', 'Y 0.0°')); container.append(transform);
  const material = typeof entity.material === 'string' ? entity.material : entity.material?.name ?? (entity.material?.pattern ? `内置 · ${entity.material.pattern}` : entity.character ? '角色自带材质' : '基础网格 · 白底灰线');
  const rendering = section('渲染'); rendering.append(row('材质', material), row('颜色', (draft.color ?? entity.color).map(v => fmt(v)).join(' · '))); container.append(rendering);
  const physics = section('物理与碰撞');
  const toggle = el('button', `toggle${draft.collider ? ' on' : ''}`); toggle.title = '启用碰撞'; toggle.setAttribute('aria-label', '启用碰撞');
  toggle.addEventListener('click', () => { editEntity(entity.id, 'collider', null, !draft.collider); renderInspector(); });
  physics.append(inputRow('启用碰撞', toggle), row('碰撞代理', entity.character ? '角色代理' : 'Box'));
  if (entity.physics) {
    physics.append(row('刚体', entity.physics.mode === 'dynamic' ? '动态 · 受重力影响' : '静态'));
    if (entity.physics.mode === 'dynamic') physics.append(row('质量', `${fmt(entity.physics.mass ?? 14)} kg`));
  }
  container.append(physics);
  const notes = section('备注'); notes.append(row('Revision', state.scene.revision));
  if (entity.kind !== 'ground' && entity.kind !== 'player') {
    const remove = el('button', 'inspector-button secondary', '删除实体'); remove.addEventListener('click', () => operation(() => removeEntity(entity.id))); notes.append(remove);
  }
  container.append(notes);
}
const LAYER_SLOTS = [['albedo', '基色'], ['normal', '法线'], ['orm', 'ORM'], ['roughness', '粗糙度'], ['metallic', '金属度'], ['occlusion', 'AO'], ['emissive', '自发光'], ['height', '高度']];
const LAYER_ID = /^[A-Za-z0-9_.-]{1,64}$/;
function layeredIdentifier(value) { const text = String(value ?? '').replace(/[^A-Za-z0-9_.-]/g, '_').slice(0, 64); return LAYER_ID.test(text) ? text : 'layered'; }
function numberField(label, value, onChange, step = '0.01') {
  const input = el('input'); input.type = 'number'; input.step = step; input.value = String(value ?? 0);
  input.addEventListener('change', () => { const next = Number(input.value); if (Number.isFinite(next)) operation(() => onChange(next)); });
  return inputRow(label, input);
}
function sliderField(label, value, min, max, onChange) {
  const input = el('input'); input.type = 'range'; input.min = min; input.max = max; input.step = '.01'; input.value = String(value ?? min);
  const output = el('output', '', fmt(input.value));
  input.addEventListener('input', () => { output.textContent = fmt(input.value); });
  input.addEventListener('change', () => operation(() => onChange(Number(input.value))));
  return inputRow(label, input, output);
}
function selectField(label, options, value, onChange) {
  const select = el('select');
  for (const [key, text] of options) { const option = el('option', '', text); option.value = key; select.append(option); }
  select.value = value;
  select.addEventListener('change', () => operation(() => onChange(select.value)));
  return inputRow(label, select);
}
function layerMappingDefaults(mapping = {}) {
  return { offset: [...(mapping.offset ?? [0, 0])], scale: [...(mapping.scale ?? [1, 1])], rotation: mapping.rotation ?? 0,
    wrapS: mapping.wrapS ?? 'repeat', wrapT: mapping.wrapT ?? 'repeat', magFilter: mapping.magFilter ?? 'linear', minFilter: mapping.minFilter ?? 'linear-mipmap-linear' };
}
async function saveLayered(name, mutate) {
  const material = state.workspace?.materials?.[name];
  if (!material?.layered) return;
  const next = structuredClone(material.layered);
  mutate(next);
  await patchMaterial(name, { layered: next });
}
function reliefDefaults(layerId) {
  return { mode: 'off', heightSource: { kind: 'layer', layer: layerId }, amplitudeMetres: 0, referenceHeight: 1, invert: false,
    minSteps: 8, maxSteps: 24, refineSteps: 2, fadeStartMetres: 6, fadeEndMetres: 14, grazingFade: .1, selfShadow: false, shadowSteps: 4,
    displacementBake: { tileMetres: 2, cellsPerTile: 64, lodLevels: 2, maxErrorMetres: .005, composite: null } };
}
async function saveSurfaceRelief(name, mutate) {
  await saveLayered(name, next => {
    if (!next.surfaceRelief) { next.schema = 'nativegpu.layered-material/2'; next.surfaceRelief = reliefDefaults(next.layers[0].id); }
    mutate(next.surfaceRelief);
  });
}
function checkField(label, checked, onChange) {
  const input = el('input'); input.type = 'checkbox'; input.checked = Boolean(checked);
  input.addEventListener('change', () => onChange(input.checked));
  return inputRow(label, input);
}
function renderSurfaceRelief(container, name, material) {
  const layered = material.layered;
  const panel = section('表面深度（厘米）');
  if (layered.schema !== 'nativegpu.layered-material/2' || !layered.surfaceRelief) {
    panel.append(row('状态', 'v1 分层材质 · 高度表现关闭'));
    const migrate = el('button', 'inspector-button secondary', '启用表面深度（迁移到 v2）');
    migrate.addEventListener('click', () => operation(() => saveLayered(name, next => {
      next.schema = 'nativegpu.layered-material/2';
      next.surfaceRelief = reliefDefaults(next.layers[0].id);
    })));
    panel.append(migrate); container.append(panel); return;
  }
  const relief = layered.surfaceRelief;
  const heightfieldEntity = (state.scene?.entities ?? []).find(entity => entity.material === name && entity.heightfield);
  panel.append(row('碰撞跟随地形', heightfieldEntity ? (heightfieldEntity.heightfield.enabled ? '开启' : '关闭') : '无高度场'));
  const mode = selectField('模式', [['off', '关闭'], ['pom', 'POM 视差'], ['bakedDisplacement', '静态几何置换']], relief.mode, value => saveSurfaceRelief(name, next => { next.mode = value; }));
  panel.append(mode);
  panel.append(row('高度来源', relief.heightSource?.kind === 'texture' ? `贴图 ${relief.heightSource.texture?.name ?? ''}` : `层 ${relief.heightSource?.layer ?? ''}`));
  if (relief.heightSource?.kind !== 'texture') {
    const source = el('select');
    for (const layer of layered.layers) { const option = el('option', '', layer.name ?? layer.id); option.value = layer.id; source.append(option); }
    source.value = relief.heightSource?.layer ?? layered.layers[0].id;
    source.addEventListener('change', () => saveSurfaceRelief(name, next => { next.heightSource = { kind: 'layer', layer: source.value }; }));
    panel.append(inputRow('驱动层', source));
  }
  panel.append(numberField('深度（厘米）', (relief.amplitudeMetres ?? 0) * 100, value => saveSurfaceRelief(name, next => { next.amplitudeMetres = Math.max(0, value) / 100; })));
  panel.append(sliderField('参考高度', relief.referenceHeight ?? 1, 0, 1, value => saveSurfaceRelief(name, next => { next.referenceHeight = value; })));
  panel.append(checkField('高度反向（白低黑高）', relief.invert, value => saveSurfaceRelief(name, next => { next.invert = value; })));
  panel.append(numberField('最小步数', relief.minSteps ?? 8, value => saveSurfaceRelief(name, next => { next.minSteps = Math.max(1, Math.round(value)); })));
  panel.append(numberField('最大步数', relief.maxSteps ?? 24, value => saveSurfaceRelief(name, next => { next.maxSteps = Math.max(1, Math.round(value)); })));
  panel.append(numberField('细化次数', relief.refineSteps ?? 2, value => saveSurfaceRelief(name, next => { next.refineSteps = Math.max(0, Math.round(value)); })));
  panel.append(numberField('开始淡出（米）', relief.fadeStartMetres ?? 6, value => saveSurfaceRelief(name, next => { next.fadeStartMetres = Math.max(0, value); })));
  panel.append(numberField('完全关闭（米）', relief.fadeEndMetres ?? 14, value => saveSurfaceRelief(name, next => { next.fadeEndMetres = Math.max(0, value); })));
  panel.append(sliderField('极低视角渐隐', relief.grazingFade ?? .1, 0, 1, value => saveSurfaceRelief(name, next => { next.grazingFade = value; })));
  panel.append(checkField('主光自遮蔽', relief.selfShadow, value => saveSurfaceRelief(name, next => { next.selfShadow = value; })));
  panel.append(numberField('遮蔽步数', relief.shadowSteps ?? 4, value => saveSurfaceRelief(name, next => { next.shadowSteps = Math.max(1, Math.round(value)); })));
  const bake = relief.displacementBake ?? {};
  panel.append(numberField('分块尺寸（米）', bake.tileMetres ?? 2, value => saveSurfaceRelief(name, next => { next.displacementBake = { ...reliefDefaults(layered.layers[0].id).displacementBake, ...next.displacementBake, tileMetres: Math.max(.25, value) }; })));
  panel.append(numberField('每边单元', bake.cellsPerTile ?? 64, value => saveSurfaceRelief(name, next => { next.displacementBake = { ...reliefDefaults(layered.layers[0].id).displacementBake, ...next.displacementBake, cellsPerTile: Math.max(4, Math.round(value)) }; })));
  panel.append(numberField('LOD 级数', bake.lodLevels ?? 2, value => saveSurfaceRelief(name, next => { next.displacementBake = { ...reliefDefaults(layered.layers[0].id).displacementBake, ...next.displacementBake, lodLevels: Math.max(1, Math.round(value)) }; })));
  container.append(panel);
}
async function convertToLayered(name) {
  const material = state.workspace?.materials?.[name];
  if (!material || material.layered) return;
  const layers = [{ id: 'base', name: '基础层', baseColor: [1, 1, 1], roughness: material.roughness ?? 1, metallic: material.metalness ?? 0,
    blend: { mode: 'mask', source: 'constant', weight: 1 }, ...(typeof material.albedo === 'string' ? { textures: { albedo: { name: material.albedo } } } : {}) }];
  await patchMaterial(name, { layered: { schema: 'nativegpu.layered-material/1', name: layeredIdentifier(name), layers } });
}
function renderLayerStack(container, name, material) {
  const layered = material.layered;
  const stack = section('层堆栈');
  const toolbar = el('div', 'property-row');
  const add = el('button', 'inspector-button', '添加层'); add.disabled = layered.layers.length >= 4;
  add.addEventListener('click', () => operation(() => saveLayered(name, next => {
    let index = 1; while (next.layers.some(layer => layer.id === 'layer-' + index)) index += 1;
    next.layers.push({ id: 'layer-' + index, name: '新图层', baseColor: [1, 1, 1], blend: { mode: 'mask', source: 'constant', weight: 0 } });
  })));
  const undo = el('button', 'inspector-button secondary', '撤销上一步保存');
  undo.disabled = !state.canUndo;
  undo.addEventListener('click', () => operation(async () => {
    adopt(await call('scene_undo', { scenePath: state.scenePath, projectId: state.scene.projectId, expectedRevision: state.scene.revision }));
    await loadCatalogs(); renderAll(); activateNative(); log('已撤销上一步保存');
  }));
  toolbar.append(add, undo); stack.append(toolbar);

  layered.layers.forEach((layer, index) => {
    const header = el('div', 'property-row');
    const enabled = el('input'); enabled.type = 'checkbox'; enabled.checked = layer.enabled !== false; enabled.title = '启用/禁用层';
    enabled.disabled = index === 0;
    enabled.addEventListener('change', () => operation(() => saveLayered(name, next => { next.layers[index].enabled = enabled.checked; })));
    const title = el('button', 'inspector-button secondary', `${index + 1}. ${layer.name ?? layer.id}`);
    title.addEventListener('click', () => { state.materialLayer = index; renderAll(); });
    const up = el('button', 'inspector-button secondary', '↑'); up.disabled = index <= 1;
    up.addEventListener('click', () => operation(() => saveLayered(name, next => { [next.layers[index - 1], next.layers[index]] = [next.layers[index], next.layers[index - 1]]; })));
    const down = el('button', 'inspector-button secondary', '↓'); down.disabled = index === 0 || index === layered.layers.length - 1;
    down.addEventListener('click', () => operation(() => saveLayered(name, next => { [next.layers[index + 1], next.layers[index]] = [next.layers[index], next.layers[index + 1]]; })));
    const copy = el('button', 'inspector-button secondary', '复制'); copy.disabled = layered.layers.length >= 4;
    copy.addEventListener('click', () => operation(() => saveLayered(name, next => {
      let id = layer.id + '-copy', suffix = 1; while (next.layers.some(item => item.id === id)) id = layer.id + '-copy' + (++suffix);
      next.layers.push({ ...structuredClone(layer), id: id.slice(0, 64), name: (layer.name ?? layer.id) + ' 副本' });
    })));
    const remove = el('button', 'inspector-button secondary', '删除'); remove.disabled = index === 0;
    remove.addEventListener('click', () => operation(() => saveLayered(name, next => { next.layers.splice(index, 1); })));
    header.append(enabled, title, up, down, copy, remove); stack.append(header);

    if (state.materialLayer !== index) return;
    const detail = el('div', 'property-section');
    const nameInput = el('input'); nameInput.value = layer.name ?? layer.id;
    nameInput.addEventListener('change', () => operation(() => saveLayered(name, next => { const text = nameInput.value.trim().slice(0, 128); if (text) next.layers[index].name = text; })));
    detail.append(inputRow('名称', nameInput));
    const color = el('div', 'property-row'); color.append(el('span', '', '基色'));
    const colorInput = el('input'); colorInput.type = 'color';
    const rgb = layer.baseColor ?? [1, 1, 1];
    colorInput.value = '#' + rgb.map(v => Math.round(Math.min(1, Math.max(0, v)) * 255).toString(16).padStart(2, '0')).join('');
    colorInput.addEventListener('change', () => operation(() => saveLayered(name, next => {
      const value = colorInput.value; next.layers[index].baseColor = [1, 3, 5].map(offset => parseInt(value.slice(offset, offset + 2), 16) / 255);
    })));
    color.append(colorInput); detail.append(color);
    detail.append(sliderField('粗糙度', layer.roughness ?? 1, 0, 1, value => saveLayered(name, next => { next.layers[index].roughness = value; })));
    detail.append(sliderField('金属度', layer.metallic ?? 0, 0, 1, value => saveLayered(name, next => { next.layers[index].metallic = value; })));
    detail.append(sliderField('AO 强度', layer.occlusionStrength ?? 1, 0, 1, value => saveLayered(name, next => { next.layers[index].occlusionStrength = value; })));
    detail.append(sliderField('法线强度', layer.normalStrength ?? 1, 0, 8, value => saveLayered(name, next => { next.layers[index].normalStrength = value; })));
    detail.append(numberField('高度缩放', layer.heightScale ?? 1, value => saveLayered(name, next => { next.layers[index].heightScale = value; })));
    detail.append(numberField('高度偏移', layer.heightOffset ?? 0, value => saveLayered(name, next => { next.layers[index].heightOffset = value; })));

    const uv = layer.uv ?? {};
    const uvSet = el('select'); for (let set = 0; set < 8; set++) { const option = el('option', '', 'UV ' + set); option.value = String(set); uvSet.append(option); }
    uvSet.value = String(uv.set ?? 0);
    uvSet.addEventListener('change', () => operation(() => saveLayered(name, next => { next.layers[index].uv = { set: Number(uvSet.value), mapping: layerMappingDefaults(next.layers[index].uv?.mapping) }; })));
    detail.append(inputRow('默认 UV 集', uvSet));
    detail.append(numberField('UV 缩放 U', (uv.mapping?.scale ?? [1, 1])[0], value => saveLayered(name, next => { const mapping = layerMappingDefaults(next.layers[index].uv?.mapping); mapping.scale[0] = value; next.layers[index].uv = { set: next.layers[index].uv?.set ?? 0, mapping }; })));
    detail.append(numberField('UV 缩放 V', (uv.mapping?.scale ?? [1, 1])[1], value => saveLayered(name, next => { const mapping = layerMappingDefaults(next.layers[index].uv?.mapping); mapping.scale[1] = value; next.layers[index].uv = { set: next.layers[index].uv?.set ?? 0, mapping }; })));
    detail.append(numberField('UV 旋转', uv.mapping?.rotation ?? 0, value => saveLayered(name, next => { const mapping = layerMappingDefaults(next.layers[index].uv?.mapping); mapping.rotation = value; next.layers[index].uv = { set: next.layers[index].uv?.set ?? 0, mapping }; })));

    const textures = layer.textures ?? {};
    for (const [slot, label] of LAYER_SLOTS) {
      const select = el('select');
      const none = el('option', '', '未绑定'); none.value = ''; select.append(none);
      for (const texture of state.textures) { const option = el('option', '', texture); option.value = texture; select.append(option); }
      select.value = textures[slot]?.name ?? '';
      select.addEventListener('change', () => operation(() => saveLayered(name, next => {
        const target = next.layers[index];
        target.textures = target.textures ?? {};
        if (!select.value) delete target.textures[slot];
        else target.textures[slot] = { name: select.value, uv: { set: target.textures[slot]?.uv?.set ?? 0 } };
      })));
      const rowNode = inputRow(label, select);
      if (textures[slot]) {
        const set = el('select'); for (let value = 0; value < 8; value++) { const option = el('option', '', 'UV ' + value); option.value = String(value); set.append(option); }
        set.value = String(textures[slot].uv?.set ?? 0);
        set.addEventListener('change', () => operation(() => saveLayered(name, next => { next.layers[index].textures[slot].uv = { set: Number(set.value), ...(next.layers[index].textures[slot].uv?.mapping ? { mapping: next.layers[index].textures[slot].uv.mapping } : {}) }; })));
        rowNode.append(set);
        for (const [label, key, componentIndex] of [["×U", "scale", 0], ["×V", "scale", 1], ["↻", "rotation", 2]]) {
          const input = el("input"); input.type = "number"; input.step = "0.01"; input.title = label;
          input.value = String(key === "rotation" ? (textures[slot].uv?.mapping?.rotation ?? 0) : (textures[slot].uv?.mapping?.scale?.[componentIndex] ?? 1));
          input.addEventListener("change", () => operation(() => saveLayered(name, next => {
            const target = next.layers[index].textures[slot]; if (!target) return;
            const mapping = layerMappingDefaults(target.uv?.mapping);
            if (key === "rotation") mapping.rotation = Number(input.value) || 0; else mapping.scale[componentIndex] = Number(input.value) || 0;
            target.uv = { set: target.uv?.set ?? 0, mapping };
          })));
          rowNode.append(input);
        }
      }
      detail.append(rowNode);
    }

    const blend = layer.blend ?? {};
    detail.append(selectField('混合模式', [['mask', '遮罩'], ['height', '高度']], blend.mode ?? 'mask', value => saveLayered(name, next => { next.layers[index].blend = { ...next.layers[index].blend, mode: value }; })));
    detail.append(selectField('覆盖来源', [['constant', '常量'], ['textureMask', '遮罩贴图'], ['vertexColor', '顶点色']], blend.source ?? 'constant', value => saveLayered(name, next => {
      const layerNext = next.layers[index]; if (!layerNext.blend) layerNext.blend = { mode: 'mask', source: 'constant', weight: 1 };
      layerNext.blend.source = value;
      if (value !== 'textureMask') delete layerNext.blend.texture;
    })));
    detail.append(sliderField('覆盖权重', blend.weight ?? 1, 0, 1, value => saveLayered(name, next => { next.layers[index].blend = { ...next.layers[index].blend, weight: value }; })));
    detail.append(selectField('权重通道', [['r', 'R'], ['g', 'G'], ['b', 'B'], ['a', 'A']], blend.channel ?? 'r', value => saveLayered(name, next => { next.layers[index].blend = { ...next.layers[index].blend, channel: value }; })));
    if (blend.mode === 'height') {
      detail.append(numberField('过渡宽度', blend.width ?? 0.2, value => saveLayered(name, next => { next.layers[index].blend = { ...next.layers[index].blend, width: value }; })));
      detail.append(numberField('高度偏置', blend.heightBias ?? 0, value => saveLayered(name, next => { next.layers[index].blend = { ...next.layers[index].blend, heightBias: value }; })));
    }
    if (blend.source === 'textureMask') {
      const maskSelect = el('select');
      for (const texture of state.textures) { const option = el('option', '', texture); option.value = texture; maskSelect.append(option); }
      maskSelect.value = blend.texture?.name ?? '';
      maskSelect.addEventListener('change', () => operation(() => saveLayered(name, next => {
        const layerNext = next.layers[index]; if (!layerNext.blend) layerNext.blend = { mode: 'mask', source: 'textureMask', weight: 1 };
        if (maskSelect.value) layerNext.blend.texture = { name: maskSelect.value, uv: { set: layerNext.blend.texture?.uv?.set ?? 0 } };
      })));
      detail.append(inputRow('遮罩贴图', maskSelect));
    }
    stack.append(detail);
  });
  container.append(stack);
}
function renderMaterialDebug(container, tab) {
  const debug = section('调试视图');
  const mode = el('select');
  for (const [key, text] of [['', '关闭'], ['mask', '层覆盖率'], ['height', '高度'], ['albedo', '层基色'], ['reliefdepth', '表面深度热图'], ['reliefoffset', 'UV 位移热图'], ['reliefsteps', '步数热图']]) { const option = el('option', '', text); option.value = key; mode.append(option); }
  const [modeKey, layerKey] = String(state.materialDebug).split(':');
  mode.value = modeKey ?? '';
  const layer = el('select'); for (let index = 0; index < 4; index++) { const option = el('option', '', '层 ' + index); option.value = String(index); layer.append(option); }
  layer.value = layerKey ?? '0';
  const apply = () => {
    state.materialDebug = mode.value === 'mask' || mode.value === 'albedo' ? mode.value + ':' + layer.value : mode.value;
    activateNative();
  };
  mode.addEventListener('change', apply); layer.addEventListener('change', apply);
  debug.append(inputRow('视图', mode), inputRow('层', layer), row('说明', '预览专用，不写入场景'));
  if (tab.kind === 'material') container.append(debug);
}
function renderMaterialInspector(container, tab) {
  const name = tab.material; const material = state.workspace?.materials?.[name];
  if (!material) { container.append(el('p', 'inspector-note', '选择材质。')); return; }
  container.append(el('h2', 'inspector-title', name), el('p', 'inspector-subtitle', `命名材质 · ${state.scene.name} · 实时原生预览`));
  const base = section('基础'); base.append(row('基色', material.albedo ?? 'flat'), row('着色', material.unlit ? 'Unlit' : 'PBR / sRGB')); container.append(base);
  const surface = section('表面');
  for (const [key, label, min, max] of [['roughness', '粗糙度', 0, 1], ['metalness', '金属度', 0, 1], ['wetness', '湿润度', 0, 1], ['wear', '磨损', 0, 1], ['emissiveStrength', '自发光', 0, 100]]) {
    const input = el('input'); input.type = 'range'; input.min = min; input.max = max; input.step = '.01'; input.value = material[key] ?? (key === 'emissiveStrength' ? 1 : 0);
    const output = el('output', '', fmt(input.value)); input.addEventListener('input', () => { output.textContent = fmt(input.value); });
    input.addEventListener('change', () => operation(() => patchMaterial(name, { [key]: Number(input.value) }))); surface.append(inputRow(label, input, output));
  }
  container.append(surface);
  const textures = section('贴图槽');
  for (const key of ['albedo', 'normal', 'orm', 'occlusion', 'emissive']) textures.append(row(({ albedo: '基色', normal: '法线', orm: 'ORM', occlusion: 'AO', emissive: '自发光' })[key], material[key] ?? (key === 'occlusion' ? '未绑定 · 使用 ORM' : '未绑定')));
  container.append(textures);
  const uv = section('UV 与采样'); uv.append(row('缩放', `U ${fmt(material.uvScale ?? 1)} · V ${fmt(material.uvScale ?? 1)}`), row('透明方式', material.alphaMode ?? 'OPAQUE')); container.append(uv);
  if (material.layered) {
    if (state.materialLayer >= material.layered.layers.length) state.materialLayer = 0;
    renderLayerStack(container, name, material);
    renderSurfaceRelief(container, name, material);
    const reset = section('分层');
    const remove = el('button', 'inspector-button secondary', '移除分层（恢复单层材质）');
    remove.addEventListener('click', () => operation(() => patchMaterial(name, { layered: null })));
    reset.append(remove); container.append(reset);
  } else {
    const convert = section('分层材质');
    convert.append(row('状态', '当前为单层 PBR 材质'));
    const button = el('button', 'inspector-button', '转换为分层材质');
    button.addEventListener('click', () => operation(() => convertToLayered(name)));
    convert.append(button); container.append(convert);
  }
  renderMaterialDebug(container, tab);
  const budget = section('预算'); budget.append(row('贴图数量', state.textures.length)); container.append(budget);
}
async function patchMaterial(name, changes) {
  if (dirty()) throw new Error('请先保存地图实体修改，再应用材质。');
  const result = await call('material_patch', { scenePath: state.scenePath, material: name, changes, expectedRevision: state.scene.revision, apply: true });
  adopt(result); await loadCatalogs(); renderAll(); activateNative(); log(`已保存材质 ${name}`);
}
function currentCharacterClip(tab) {
  if (tab.clipData) return { clip: tab.clipData, character: state.workspace.characters.find(c => c.file === tab.file) };
  const character = state.workspace.characters.find(c => importedId(c.file) === tab.asset) ?? state.workspace.characters[0];
  return { character, clip: character?.clips[0] };
}
function renderAnimationInspector(container, tab) {
  const { character, clip } = currentCharacterClip(tab);
  if (!clip) { container.append(el('p', 'inspector-note', '此角色没有动作片段。')); return; }
  container.append(el('h2', 'inspector-title', clip.name), el('p', 'inspector-subtitle', `${character.file} · ${clip.frames} 帧 · ${character.sampleRate} fps · ${fmt(clip.duration)} s`));
  const sampling = section('采样'); sampling.append(row('循环播放', clip.loop ? '开启' : '关闭'), row('播放速度', '1.00 ×'), row('根运动', '按资产数据')); container.append(sampling);
  const events = section(`事件 Notify · ${clip.notifies?.length ?? 0}`);
  for (const notify of clip.notifies ?? []) events.append(row(notify.name ?? notify.type ?? 'Notify', `${fmt(notify.time ?? notify.frame ?? 0)}${notify.time !== undefined ? ' s' : ' 帧'}`));
  container.append(events);
  const blend = section('混合'); blend.append(row('骨骼', character.bones), row('采样率', `${character.sampleRate} Hz`)); container.append(blend);
}
function renderEffectInspector(container, tab) {
  if (tab.kind === 'vegetation') {
    const species = tab.speciesData;
    container.append(el('h2', 'inspector-title', species.id), el('p', 'inspector-subtitle', `ngveg · ${species.baked ? '原生包已烘焙' : '尚未烘焙'}`));
    const vegetation = section('植被'); vegetation.append(row('三角形', species.baked?.triangles), row('LOD', species.baked?.lodTriangles?.map((v, i) => `${i}: ${fmt(v)}`).join(' / ') ?? '—'), row('包摘要', species.baked?.hash?.slice(0, 16) ?? '—')); container.append(vegetation);
    const wind = section('风场'); wind.append(row('预览', '原生风动与阴影'), row('视口', 'D3D12 · WS_CHILD')); container.append(wind); return;
  }
  const effect = tab.effectData;
  container.append(el('h2', 'inspector-title', effect?.name ?? tab.name), el('p', 'inspector-subtitle', `${tab.library} · 原生粒子模拟`));
  const emission = section('发射');
  for (const [key, value] of Object.entries(effect ?? {}).slice(0, 10)) if (!['name', 'id'].includes(key)) emission.append(row(key, typeof value === 'object' ? JSON.stringify(value) : value));
  container.append(emission);
  const wind = section('风场'); wind.append(row('场景', state.scene.name), row('后端', '由原生宿主选择')); container.append(wind);
  const vegetation = section('植被'); vegetation.append(row('区域', '主地图同屏流送')); container.append(vegetation);
}
function renderAudioInspector(container, tab) {
  const emitter = state.workspace.audio.emitters?.find(item => item.id === tab.emitter.id) ?? tab.emitter;
  container.append(el('h2', 'inspector-title', emitter.id), el('p', 'inspector-subtitle', `${emitter.sound} · 总线 ${emitter.bus ?? 'sfx'}`));
  const spatial = section('空间化'); spatial.append(row('衰减', `${fmt(emitter.maxDistance)} m`), row('遮挡', emitter.occlusion ? '开启' : '关闭'), row('空间声', emitter.spatial === false ? '2D' : '3D')); container.append(spatial);
  const volume = section('音量与响度');
  const gain = el('input'); gain.type = 'range'; gain.min = -60; gain.max = 6; gain.step = .5; gain.value = emitter.gainDb ?? 0;
  const output = el('output', '', `${fmt(gain.value)} dB`); gain.addEventListener('input', () => { output.textContent = `${fmt(gain.value)} dB`; });
  gain.addEventListener('change', () => operation(async () => {
    const audio = structuredClone(state.workspace.audio); audio.emitters = audio.emitters.map(item => item.id === emitter.id ? { ...item, gainDb: Number(gain.value) } : item);
    await configureScene({ audio });
  })); volume.append(inputRow('基准音量', gain, output), row('目标响度', `${state.workspace.audio.loudnessTargetLufs ?? -16} LUFS`)); container.append(volume);
  const trigger = section('触发'); trigger.append(row('事件', emitter.trigger ?? 'ambient')); container.append(trigger);
}
function renderDiagnosticsInspector(container, tab) {
  const issue = tab.issue ?? state.workspace.diagnostics[0];
  if (!issue) { container.append(el('h2', 'inspector-title', '场景检查通过'), el('p', 'inspector-note', '当前场景没有 lint 诊断。')); return; }
  container.append(el('h2', 'inspector-title', issue.code), el('p', 'inspector-subtitle', `${issue.severity} · ${issue.entityId ?? 'scene'}`), el('p', 'diagnostic-detail', issue.message));
  const details = section('问题详情'); for (const [key, value] of Object.entries(issue.detail ?? {})) details.append(row(key, JSON.stringify(value))); container.append(details);
  if (issue.entityId) {
    const locate = section('定位'); const button = el('button', 'inspector-button secondary', '在大纲中显示 · 聚焦视口');
    button.addEventListener('click', () => { state.selectedEntity = issue.entityId; renderLeft(); notify('已在大纲中定位实体'); }); locate.append(button); container.append(locate);
  }
}
function renderInspector() {
  const tab = activeTab(); const container = $('inspector-content'); clear(container); const tabs = $('inspector-tabs'); clear(tabs);
  if (!tab) return;
  for (const [index, label] of KIND[tab.kind].inspectors.entries()) {
    const button = el('button', index === (tab.inspector ?? 0) ? 'active' : '', label);
    button.addEventListener('click', () => { tab.inspector = index; renderInspector(); }); tabs.append(button);
  }
  const selectedInspector = KIND[tab.kind].inspectors[tab.inspector ?? 0];
  if (tab.kind === 'scene') {
    if (selectedInspector === '材质' && originalEntity(state.selectedEntity)?.material) {
      const asset = state.assets.find(item => item.kind === 'material' && item.material === originalEntity(state.selectedEntity).material);
      if (asset) renderMaterialInspector(container, { ...tab, ...asset }); else renderObjectInspector(container, originalEntity(state.selectedEntity));
    } else if (selectedInspector === '光照') renderLightingInspector(container);
    else if (selectedInspector === '玩法') renderSettingsInspector(container);
    else renderObjectInspector(container, originalEntity(state.selectedEntity));
  }
  else if (tab.kind === 'material') renderMaterialInspector(container, tab);
  else if (['character', 'animation'].includes(tab.kind)) renderAnimationInspector(container, tab);
  else if (['effect', 'vegetation'].includes(tab.kind)) renderEffectInspector(container, tab);
  else if (tab.kind === 'audio') renderAudioInspector(container, tab);
  else if (tab.kind === 'diagnostics') selectedInspector === '场景设置' ? renderSettingsInspector(container) : renderDiagnosticsInspector(container, tab);
}
function renderLightingInspector(container) {
  container.append(el('h2', 'inspector-title', '光照'), el('p', 'inspector-subtitle', `${state.scene.name} · 场景环境`));
  const lighting = section('环境');
  for (const [key, value] of Object.entries(state.workspace.lighting ?? {})) lighting.append(row(key, typeof value === 'object' ? JSON.stringify(value) : value));
  if (!Object.keys(state.workspace.lighting ?? {}).length) lighting.append(row('环境', '使用场景默认光照')); container.append(lighting);
}
function renderSettingsInspector(container) {
  container.append(el('h2', 'inspector-title', '场景设置'), el('p', 'inspector-subtitle', state.scene.name));
  const settings = section('玩法移动');
  for (const [key, label] of [['walkSpeed', '步行速度'], ['runSpeed', '奔跑速度'], ['jumpSpeed', '跳跃速度'], ['gravity', '重力']]) {
    const input = el('input'); input.type = 'number'; input.step = 'any'; input.value = state.workspace.settings[key];
    input.addEventListener('change', () => operation(() => configureScene({ settings: { ...state.workspace.settings, [key]: Number(input.value) } })));
    settings.append(inputRow(label, input));
  }
  container.append(settings);
}
async function configureScene(changes) {
  if (dirty()) throw new Error('请先保存地图实体修改，再修改场景设置。');
  adopt(await call('scene_configure', { scenePath: state.scenePath, projectId: state.scene.projectId, expectedRevision: state.scene.revision, changes, apply: true }));
  await loadCatalogs(); renderAll(); activateNative(); log('场景设置已保存');
}

function assetIcon(asset) { return ICON[asset.kind] ?? 'file'; }
function assetCard(asset, manager = false) {
  const card = el('button', manager ? 'manager-card' : 'asset-card'); card.dataset.assetId = asset.id;
  const thumb = el('div', 'asset-thumb'); thumb.append(icon(assetIcon(asset)));
  card.append(thumb, el('span', 'asset-name', asset.name));
  if (manager) card.append(el('small', '', asset.detail));
  card.addEventListener('click', () => {
    if (manager) { state.managerSelected = asset.id; renderManager(); }
    else { for (const other of document.querySelectorAll('.asset-card')) other.classList.toggle('selected', other === card); }
  });
  card.addEventListener('dblclick', () => openAsset(asset)); return card;
}
function renderAssetBrowser() {
  const browser = el('div', 'asset-browser'); const crumb = el('div', 'asset-breadcrumb');
  crumb.append(el('span', '', '‹  ›  ↑'), el('span', '', '工程'), el('span', '', '›'), el('span', '', '关卡'), el('span', '', '›'), el('strong', '', state.scene.name));
  const search = el('div', 'search-box'); search.append(el('span', '', '⌕')); const input = el('input'); input.placeholder = '搜索资源'; search.append(input); crumb.append(search); browser.append(crumb);
  const grid = el('div', 'asset-grid scroll');
  const draw = () => { clear(grid); const term = input.value.trim().toLowerCase(); for (const asset of state.assets.filter(item => !term || `${item.name} ${item.path}`.toLowerCase().includes(term))) grid.append(assetCard(asset)); };
  input.addEventListener('input', draw); draw(); browser.append(grid); return browser;
}
function renderConsole() {
  const panel = el('div', 'console-log');
  for (const entry of state.logs) { const line = el('div', `console-line${entry.error ? ' error' : ''}`); line.append(el('span', 'console-time', entry.time), el('span', '', entry.message)); panel.append(line); }
  return panel;
}
function renderTimeline(tab) {
  const panel = el('div', 'keyframe-panel'); const toolbar = el('div', 'panel-toolbar');
  toolbar.append(el('button', 'active', '时间轴'), el('button', '', '曲线'), el('button', '', '事件轨'), el('span', '', `${tab.clipData?.duration ? fmt(tab.clipData.duration) : '—'} s · 原生动作数据`)); panel.append(toolbar);
  const { character, clip } = currentCharacterClip(tab); const timeline = el('div', 'timeline');
  if (clip) {
    for (const label of ['骨骼 · 位置', '骨骼 · 旋转', '根位移 XZ', '事件 Notify']) {
      const track = el('div', 'timeline-track'); track.append(el('label', '', label));
      const count = label.startsWith('事件') ? clip.notifies?.length ?? 0 : Math.min(8, Math.max(2, Math.ceil(clip.frames / 6)));
      for (let index = 0; index < count; index += 1) { const key = el('i', 'key'); key.style.left = `${count <= 1 ? 0 : index / (count - 1) * 100}%`; track.append(key); }
      timeline.append(track);
    }
    const head = el('div', 'playhead'); timeline.append(head);
  }
  panel.append(timeline); return panel;
}
function renderCurve(tab) {
  const panel = el('div', 'curve-panel'); const toolbar = el('div', 'panel-toolbar');
  toolbar.append(el('button', 'active', '透明度曲线'), el('button', '', '大小曲线'), el('button', '', '颜色渐变'), el('span', '', tab.name)); panel.append(toolbar);
  const curve = el('div', 'curve');
  const points = tab.effectData?.opacityCurve ?? tab.effectData?.alphaCurve;
  if (Array.isArray(points) && points.length > 1) {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.setAttribute('viewBox', '0 0 100 100'); svg.setAttribute('preserveAspectRatio', 'none');
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', points.map((point, index) => `${index ? 'L' : 'M'} ${index / (points.length - 1) * 100} ${100 - Number(Array.isArray(point) ? point[1] : point) * 100}`).join(' ')); svg.append(path); curve.append(svg);
  } else curve.append(el('p', 'inspector-note', '此资产没有可编辑的透明度曲线数据。'));
  panel.append(curve); return panel;
}
function renderMixer() {
  const panel = el('div', 'mixer-panel'); const toolbar = el('div', 'panel-toolbar'); toolbar.append(el('button', 'active', '混音总线'), el('button', '', '事件诊断'), el('button', '', '响度表'), el('span', '', `${state.workspace.audio.emitters?.length ?? 0} 个声源`)); panel.append(toolbar);
  const mixer = el('div', 'mixer');
  const master = el('div', 'mix-row'); master.append(el('span', '', '主 Master')); const masterInput = el('input'); masterInput.type = 'range'; masterInput.min = -60; masterInput.max = 6; masterInput.value = state.workspace.audio.masterGainDb ?? 0;
  const masterOut = el('output', '', `${fmt(masterInput.value)} dB`); masterInput.addEventListener('input', () => { masterOut.textContent = `${fmt(masterInput.value)} dB`; });
  masterInput.addEventListener('change', () => operation(() => configureScene({ audio: { ...state.workspace.audio, masterGainDb: Number(masterInput.value) } })));
  master.append(masterInput, masterOut); mixer.append(master);
  for (const bus of ['ambience', 'sfx', 'music', 'voice', 'ui']) {
    const emitters = state.workspace.audio.emitters.filter(e => (e.bus ?? 'sfx') === bus); const line = el('div', 'mix-row');
    line.append(el('span', '', ({ ambience: '环境 Ambience', sfx: '玩法 Gameplay', music: '音乐 Music', voice: '语音 Voice', ui: 'UI' })[bus]), el('span', '', `${emitters.length} 个声源`), el('output', '', emitters.length ? `${Math.max(...emitters.map(e => e.gainDb ?? 0))} dB` : '—')); mixer.append(line);
  }
  panel.append(mixer); return panel;
}
function renderDiagnosticsBottom() {
  const layout = el('div', 'diagnostics-layout'); const issues = el('div', 'issues-panel'); const toolbar = el('div', 'panel-toolbar');
  toolbar.append(el('button', 'active', '问题列表'), el('button', '', '性能'), el('button', '', '控制台'), el('span', '', `${state.workspace.diagnostics.length} 个问题 · lint`)); issues.append(toolbar);
  const table = el('div', 'issues-table');
  for (const issue of state.workspace.diagnostics) {
    const rowNode = el('button', `issue-row${activeTab()?.issue?.code === issue.code ? ' active' : ''}`);
    rowNode.append(el('span', 'warning', issue.severity === 'error' ? '错误' : '警告'), el('span', '', issue.code), el('span', '', issue.entityId ?? '场景'), el('span', '', issue.message));
    rowNode.addEventListener('click', () => { activeTab().issue = issue; renderInspector(); renderBottom(); }); table.append(rowNode);
  }
  issues.append(table); layout.append(issues);
  const perf = el('div', 'performance'); perf.append(el('strong', '', '原生视口'), el('p', 'inspector-note', `ACK revision ${state.nativeAck?.revision ?? '—'} · 帧 ${state.nativeAck?.frames ?? '—'}`));
  for (const label of ['CPU', 'GPU']) { const line = el('div', 'perf-row'); line.append(el('span', '', label), el('i'), el('span', '', '—')); perf.append(line); }
  perf.append(el('p', 'inspector-note', '性能计时等待宿主上报；不显示估算值。')); layout.append(perf); return layout;
}
function renderBottom() {
  const tab = activeTab(); const container = $('bottom-content'); clear(container);
  $('context-bottom').classList.toggle('special', tab?.kind !== 'scene');
  const bottomTabs = $('bottom-tabs'); bottomTabs.hidden = tab?.kind !== 'scene';
  if (!tab) return;
  if (tab.kind === 'scene') {
    if (state.bottom === 'console') container.append(renderConsole());
    else container.append(renderAssetBrowser());
  } else if (['character', 'animation'].includes(tab.kind)) container.append(renderTimeline(tab));
  else if (['effect', 'vegetation'].includes(tab.kind)) container.append(renderCurve(tab));
  else if (tab.kind === 'audio') container.append(renderMixer());
  else if (tab.kind === 'diagnostics') container.append(renderDiagnosticsBottom());
  else if (tab.kind === 'material') {
    const panel = el('div', 'curve-panel'); const toolbar = el('div', 'panel-toolbar'); toolbar.append(el('button', 'active', '贴图槽'), el('button', '', '通道'), el('button', '', '前后对比'), el('button', '', '采样'), el('span', '', '原生材质实时预览')); panel.append(toolbar);
    const browser = el('div', 'asset-grid scroll'); for (const texture of state.textures) browser.append(assetCard({ id: `texture:${texture}`, kind: 'texture', name: texture, detail: 'ngtex' })); panel.append(browser); container.append(panel);
  }
}
function renderPlayback() {
  const play = state.nativeAck?.play ?? 'edit';
  const button = $('simulate-button');
  if (button) {
    button.textContent = play === 'simulate' ? '停止模拟' : '物理模拟';
    button.classList.toggle('active', play === 'simulate');
    button.disabled = !state.activeProject || state.busy;
  }
  if (activeTab()?.kind === 'scene') {
    const projection = state.nativeAck?.orthographic ? '正交' : '透视';
    const label = play === 'run' ? '运行画面' : play === 'simulate' ? '物理模拟' : '编辑';
    $('viewport-camera').textContent = `编辑相机 · ${projection} · ${label} · Alt+左键环绕 · 中键平移 · 右键漫游 · 滚轮缩放 · F 聚焦`;
  }
}
function renderStatus() {
  $('run-button').textContent = state.runtime.running ? '停止运行' : '运行';
  $('run-button').disabled = !state.activeProject || state.busy;
  renderPlayback();
  if (!state.activeProject) {
    $('status-selection').textContent = state.busy ? '正在处理…' : '就绪 · 未打开项目';
    $('save-state').textContent = '未打开项目';
    $('status-metrics').textContent = 'D3D12 · 项目未打开';
    return;
  }
  const entity = originalEntity(state.selectedEntity);
  $('status-selection').textContent = state.busy ? '正在处理…' : dirty() ? `未保存修改 · ${state.drafts.size + state.created.size + state.removed.size} 项` : entity ? `选中 1 · ${entity.name} · revision ${state.scene?.revision ?? 0} · 已保存` : `revision ${state.scene?.revision ?? 0} · 已保存`;
  $('save-state').textContent = state.busy ? '处理中' : dirty() ? '有未保存修改' : `revision ${state.scene?.revision ?? 0} · 已保存`;
  $('status-metrics').textContent = state.nativeAck?.ok ? `D3D12 · ${state.nativeAck.mode ?? 'scene'} · 帧 ${state.nativeAck.frames ?? '—'}` : 'D3D12 · 原生视口连接中';
  $('preview-button').textContent = dirty() ? '保存' : '预览';
}

function nativeRect() {
  const rect = $('native-slot').getBoundingClientRect(); const scale = window.devicePixelRatio || 1;
  return { x: Math.round(rect.left * scale), y: Math.round(rect.top * scale), width: Math.round(rect.width * scale), height: Math.round(rect.height * scale), visible: true };
}
// The native viewport is a WS_CHILD window owned by the host process, so Windows composites it above
// the Chromium surface: no z-index can lift a <dialog> over it. When a dialog covers the viewport the
// child window is hidden instead and the rectangle is pushed again once the last one closes. A modal
// dialog's ::backdrop dims the whole window, so it hides the viewport even when its box misses the
// slot; a non-modal dialog only hides it when the two actually overlap.
function nativeOccluded() {
  const slot = $('native-slot').getBoundingClientRect();
  for (const dialog of document.querySelectorAll('dialog[open]')) {
    let modal = true;
    try { modal = dialog.matches(':modal'); } catch { modal = true; }
    if (modal) return true;
    const rect = dialog.getBoundingClientRect();
    if (rect.right > slot.left && rect.left < slot.right && rect.bottom > slot.top && rect.top < slot.bottom) return true;
  }
  return false;
}
// Mirrors what the host was last told, so a resize storm under an open dialog does not write one
// v=0 line per frame. `null` means "nothing sent yet": the host creates its child window visible,
// so the first hide must go on the wire even though the page has never shown a rectangle.
// Every hide goes through hideNative() to keep the mirror honest.
let nativeVisible = null;
function hideNative() {
  if (!bridge) { nativeVisible = false; return; }
  if (nativeVisible === false) return;
  nativeVisible = false;
  bridge.apply({ visible: false });
}
function syncNativeRect() {
  if (!bridge) return;
  if (!state.activeId || document.hidden || nativeOccluded()) { hideNative(); return; }
  nativeVisible = true;
  bridge.apply(nativeRect());
}
async function activateNative() {
  const tab = activeTab(); const request = ++state.nativeRequest;
  $('viewport').classList.remove('native-ready'); state.nativeAck = null; renderStatus();
  if (!tab) { hideNative(); return; }
  if (!bridge) { $('native-status').textContent = '所有预览均使用原生引擎，请在 Electron 中启动 NativeEngine。'; return; }
  const revision = ++state.nativeRevision;
  const mode = ['diagnostics', 'audio'].includes(tab.kind) ? 'scene' : tab.kind === 'animation' ? 'character' : tab.kind;
  const command = { command: 'open', mode, scene: tab.scenePath ?? state.scenePath, revision };
  if (mode === 'scene') command.play = 'edit';
  if (tab.material) command.material = tab.material;
  if (tab.kind === 'material' && state.materialDebug) command.debug = state.materialDebug;
  if (tab.asset) command.asset = tab.asset;
  if (tab.clip) command.clip = tab.clip;
  if (tab.effect) { command.effect = tab.effect; command.library = tab.library; }
  if (tab.species) { command.species = tab.species; command.digest = tab.digest; command.lod = 0; }
  $('native-status').textContent = `正在载入 ${tab.name} · 等待原生 ACK…`;
  syncNativeRect();
  try {
    const result = await bridge.command(command);
    if (!result?.ok) { $('native-status').textContent = result?.error ?? '原生宿主未就绪'; return; }
    if (request === state.nativeRequest) {
      log(`已请求原生预览 ${tab.name} · ${command.mode}`);
      state.pendingPreviewSync = result.requestId ?? 0;
      // The scene may already be open from an older request; replay drafts now and again
      // when this request's ACK lands.
      syncPreviewOverrides();
    }
  } catch (error) { $('native-status').textContent = error.message; }
}
if (bridge) {
  bridge.onAck(ack => {
    if (ack.requestId === 0 && !state.nativeAck) { activateNative(); return; }
    if (ack.revision !== state.nativeRevision) return;
    state.nativeAck = ack;
    $('viewport').classList.toggle('native-ready', Boolean(ack.ok));
    $('native-status').textContent = ack.ok ? `${ack.mode ?? ''} · 已确认` : (ack.error ?? '原生视口错误');
    renderStatus();
    renderPlayback();
    if (ack.ok && ack.requestId && ack.requestId === state.pendingPreviewSync) {
      state.pendingPreviewSync = 0;
      syncPreviewOverrides();
    }
    if (activeTab()?.kind === 'diagnostics') renderBottom();
  });
  new ResizeObserver(syncNativeRect).observe($('native-slot'));
  window.addEventListener('resize', syncNativeRect);
  document.addEventListener('visibilitychange', () => { if (document.hidden) hideNative(); else syncNativeRect(); });
  // showModal()/close() only flip the `open` attribute, so one attribute observer per dialog covers
  // every path — buttons, Escape and the backdrop — without patching each call site.
  const dialogWatcher = new MutationObserver(syncNativeRect);
  for (const dialog of document.querySelectorAll('dialog')) dialogWatcher.observe(dialog, { attributes: true, attributeFilter: ['open'] });
}

function transaction() {
  const commands = [];
  for (const entity of state.created.values()) commands.push({ op: 'create', entity: { ...entity, ...(state.drafts.get(entity.id) ?? {}) } });
  for (const entity of state.scene.entities) {
    if (state.removed.has(entity.id)) { commands.push({ op: 'remove', entityId: entity.id }); continue; }
    const draft = state.drafts.get(entity.id); if (!draft) continue;
    const changes = {}; for (const [key, value] of Object.entries(draft)) if (value !== undefined && JSON.stringify(value) !== JSON.stringify(entity[key])) changes[key] = value;
    if (Object.keys(changes).length) commands.push({ op: 'set', entityId: entity.id, changes });
  }
  return { projectId: state.scene.projectId, expectedRevision: state.scene.revision, commands };
}
async function saveScene() {
  const tx = transaction(); if (!tx.commands.length) { notify('没有需要保存的修改'); return; }
  await call('scene_preview', { scenePath: state.scenePath, transaction: tx });
  const result = await call('scene_apply', { scenePath: state.scenePath, transaction: tx }); adopt(result); await loadCatalogs(); renderAll(); activateNative(); log(`已保存 ${tx.commands.length} 条场景命令 · revision ${result.scene.revision}`);
}
function addEntity() {
  if (!state.scene) return;
  if (scenePlaybackLocked()) { notify('请先停止模拟或运行，再添加对象'); return; }
  pushHistory();
  const id = crypto.randomUUID(); const entity = { id, name: `新物体 ${state.created.size + 1}`, kind: 'prop', position: [0, .5, 0], scale: [1, 1, 1], color: [.5, .5, .5], collider: true };
  state.created.set(id, entity); state.selectedEntity = id;
  const sceneTab = state.tabs.find(tab => tab.kind === 'scene'); if (sceneTab) state.activeId = sceneTab.id;
  renderAll();
  notify('新物体已加入草稿：保存后出现在原生视口');
}
async function removeEntity(id) {
  if (scenePlaybackLocked()) { notify('请先停止模拟或运行，再删除对象'); return; }
  pushHistory();
  const entity = originalEntity(id);
  const wasCreated = state.created.has(id);
  if (wasCreated) state.created.delete(id); else state.removed.add(id);
  state.drafts.delete(id); state.selectedEntity = state.scene.entities.find(item => item.id !== id && !state.removed.has(item.id))?.id ?? null;
  if (entity && !wasCreated) previewOverride(entity, false);
  renderAll();
}

function renderManager() {
  const categories = ['all', 'scene', 'character', 'material', 'effect', 'audio', 'animation', 'vegetation'];
  const categoryContainer = $('manager-categories'); clear(categoryContainer);
  for (const category of categories) {
    const count = category === 'all' ? state.assets.length : state.assets.filter(asset => asset.kind === category).length;
    const button = el('button', `category-row${state.managerCategory === category ? ' active' : ''}`);
    button.append(icon(category === 'all' ? 'stack-2' : ICON[category]), el('span', 'category-name', category === 'all' ? '全部资产' : TYPE[category]), el('span', '', count));
    button.addEventListener('click', () => { state.managerCategory = category; renderManager(); }); categoryContainer.append(button);
  }
  const type = $('manager-type'); const selectedType = type.value;
  if (type.options.length === 1) for (const category of categories.slice(1)) { const option = el('option', '', `类型 · ${TYPE[category]}`); option.value = category; type.append(option); }
  type.value = selectedType;
  const grid = $('manager-grid'); clear(grid);
  const term = $('manager-search').value.trim().toLowerCase();
  const filtered = state.assets.filter(asset => (state.managerCategory === 'all' || asset.kind === state.managerCategory) && (type.value === 'all' || asset.kind === type.value) && (!term || `${asset.name} ${asset.path} ${asset.id}`.toLowerCase().includes(term)));
  for (const asset of filtered) { const card = assetCard(asset, true); card.classList.toggle('selected', asset.id === state.managerSelected); grid.append(card); }
  $('manager-total').textContent = `本地库 ${state.assets.length} 项 · 在线服务未配置`;
  $('manager-list-title').textContent = `${state.managerCategory === 'all' ? '全部资产' : TYPE[state.managerCategory]} · 共 ${filtered.length} 项`;
  $('manager-pagination').textContent = `已显示 ${filtered.length} / ${state.assets.length} 项`;
  $('storage-count').textContent = `${state.assets.length} 项`;
  const asset = state.assets.find(item => item.id === state.managerSelected); const details = $('manager-details'); clear(details);
  $('manager-selection').textContent = asset ? `已选 1 项 · ${asset.name}` : '未选择资产';
  if (!asset) { const empty = el('div', 'detail-preview'); empty.append(icon('file')); details.append(empty, el('p', 'inspector-note', '选择资产查看详情，双击直接在编辑器中打开。')); return; }
  const preview = el('div', 'detail-preview'); preview.append(icon(assetIcon(asset)));
  details.append(preview, el('h3', 'detail-name', asset.name), el('p', 'detail-path', asset.path ?? asset.id));
  const tags = el('div', 'detail-tags'); tags.append(el('span', '', TYPE[asset.kind] ?? asset.kind), el('span', '', '本地资产')); details.append(tags);
  details.append(el('div', 'detail-row', '类型'), el('div', 'detail-row', '原生预览'));
  const actions = el('div', 'detail-actions'); const open = el('button', '', '在编辑器中打开'); open.addEventListener('click', () => openAsset(asset)); actions.append(open);
  if (asset.asset && asset.kind === 'character') {
    const attach = el('button', 'secondary', '引用到当前场景'); attach.addEventListener('click', () => operation(async () => {
      if (dirty()) throw new Error('请先保存地图修改，再引用资产。');
      const result = await call('asset_attach', { id: asset.asset, scenePath: state.scenePath, expectedRevision: state.scene.revision });
      adopt(await call('scene_open', { scenePath: state.scenePath })); await loadCatalogs(); renderAll(); activateNative(); $('asset-manager').close(); notify('资产已引用到当前场景');
    })); actions.append(attach);
  }
  details.append(actions);
}
function openManager() {
  if (!state.activeProject) { notify('请先新建或打开项目。'); return; }
  state.managerSelected ??= state.assets[0]?.id ?? null;
  renderManager(); $('asset-manager').showModal();
}

function projectNameError(value) {
  const name = String(value ?? '').trim();
  if (!name || name.length > 80) return '项目名称长度必须为 1–80 个字符。';
  if (/[<>:"/\\|?*\x00-\x1f]/.test(name) || /[. ]$/.test(name) || name === '.' || name === '..'
      || /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$/i.test(name)) return '项目名称包含 Windows 不允许的字符或保留名称。';
  if (state.projects.some(project => project.name.localeCompare(name, 'zh-CN', { sensitivity: 'accent' }) === 0)) return '同名项目已存在。';
  return null;
}

function updateProjectForm() {
  const error = projectNameError($('project-name').value);
  $('project-create-status').textContent = error ?? '';
  $('new-project-submit').disabled = Boolean(error) || state.busy;
}

function showNewProjectDialog() {
  $('project-location').value = state.projectsRoot || '正在读取项目目录…';
  $('project-create-status').textContent = '';
  if (!$('project-name').value) $('project-name').value = 'Untitled_Project';
  updateProjectForm();
  if (!$('new-project-dialog').open) $('new-project-dialog').showModal();
  requestAnimationFrame(() => { $('project-name').focus(); $('project-name').select(); });
}

function projectLocation(project) {
  return project?.location ?? project?.path ?? (state.projectsRoot && project?.folder ? `${state.projectsRoot}${window.navigator.platform.startsWith('Win') ? '\\' : '/'}${project.folder}` : '');
}

function renderProjectManager() {
  const current = state.activeProject;
  $('project-current-name').textContent = current?.name ?? '未打开项目';
  $('project-current-path').textContent = current ? projectLocation(current) : (state.projectsRoot || '');
  $('project-list-count').textContent = `${state.projects.length} 个`;
  const list = $('project-list'); clear(list);
  if (!state.projects.length) {
    list.append(el('div', 'project-list-empty', '还没有项目，创建一个空白项目开始。'));
    return;
  }
  for (const project of state.projects) {
    const row = el('button', `project-row${project.folder === current?.folder ? ' current' : ''}`);
    row.append(icon('folder'));
    const copy = el('span', 'project-row-copy');
    copy.append(el('strong', '', project.name), el('small', '', projectLocation(project)));
    row.append(copy, el('span', 'project-row-state', project.folder === current?.folder ? '当前' : '打开'));
    row.addEventListener('click', () => operation(() => switchProject(project.folder)));
    list.append(row);
  }
}

function openProjectManager() {
  renderProjectManager();
  if (!$('project-manager').open) $('project-manager').showModal();
}

function renderEmptyProject() {
  document.body.classList.add('no-project');
  state.tabs = []; state.activeId = null; state.scene = null; state.scenePath = ''; state.assets = [];
  state.textures = []; state.workspace = null; state.imports = []; state.vegetation = null;
  state.selectedEntity = null; state.drafts.clear(); state.created.clear(); state.removed.clear();
  state.history.length = 0; state.redo.length = 0;
  clear($('resource-tabs'));
  $('left-title').textContent = '项目';
  const left = $('left-content'); clear(left); left.append(el('p', 'inspector-note', '尚未打开项目。使用“文件 → 项目管理”或新建项目开始。'));
  $('left-secondary').hidden = true;
  clear($('inspector-tabs')); const inspector = $('inspector-content'); clear(inspector);
  inspector.append(el('h2', 'inspector-title', '未打开项目'), el('p', 'inspector-note', '新建项目后，场景、资产和输出都会保存在项目目录中。'));
  const bottom = $('bottom-content'); clear(bottom); bottom.append(el('p', 'inspector-note', '尚未打开项目。'));
  $('bottom-tabs').hidden = true;
  clear($('context-select')); $('context-select').append(el('option', '', '未打开项目'));
  $('native-status').textContent = '请新建或打开项目';
  $('viewport').classList.remove('native-ready');
  $('status-selection').textContent = '就绪 · 未打开项目';
  $('save-state').textContent = '未打开项目';
  $('status-metrics').textContent = 'D3D12 · 项目未打开';
  $('preview-button').disabled = true;
  $('build-button').disabled = true;
  $('left-add').disabled = true;
  hideNative();
  renderProjectManager();
}

function resetWorkspace() {
  hideNative();
  state.scene = null; state.scenePath = ''; state.textures = []; state.workspace = null; state.imports = [];
  state.vegetation = null; state.assets = []; state.tabs = []; state.activeId = null; state.selectedEntity = null;
  state.canUndo = false; state.drafts = new Map(); state.created = new Map(); state.removed = new Set();
  state.history = []; state.redo = [];
  state.nativeAck = null; state.nativeRevision += 1;
  state.runtime = { running: false }; state.runtimeError = null;
  $('viewport').classList.remove('native-ready');
}

async function refreshProjectInfo() {
  const info = await call('project_info');
  state.project = info;
  state.projects = info.projects ?? [];
  state.activeProject = info.activeProject ?? null;
  state.projectsRoot = info.projectsRoot ?? '';
  $('project-location').value = state.projectsRoot || '';
  renderProjectManager();
  return info;
}

async function enterActiveProject(info) {
  if (!info.activeProject) { renderEmptyProject(); return false; }
  document.body.classList.remove('no-project');
  $('preview-button').disabled = false;
  $('build-button').disabled = false;
  $('left-add').disabled = false;
  if (bridge?.setProject) {
    const switched = await bridge.setProject(info.activeProject.folder);
    if (!switched?.ok) throw new Error(switched?.error ?? '无法切换原生项目');
  }
  if (!info.scenes?.length) throw new Error('项目中没有可编辑的场景。');
  adopt(await call('scene_open', { scenePath: info.scenes[0].path }));
  await loadCatalogs();
  openAsset(state.assets.find(asset => asset.kind === 'scene' && asset.scenePath === state.scenePath));
  log(`已打开项目 ${info.activeProject.name} · ${state.scenePath}`);
  return true;
}

async function switchProject(folder) {
  if (dirty()) throw new Error('请先保存当前项目修改。');
  $('project-manager').close();
  const result = await call('project_open', { folder });
  resetWorkspace();
  const info = await refreshProjectInfo();
  await enterActiveProject(info);
  notify(`已打开项目 ${result.activeProject.name}`);
}

async function createProject() {
  if (dirty()) throw new Error('请先保存当前项目修改。');
  const name = $('project-name').value.trim();
  const error = projectNameError(name);
  if (error) { $('project-create-status').textContent = error; return; }
  $('new-project-submit').disabled = true;
  $('project-create-status').textContent = '正在创建项目…';
  try {
    const result = await call('project_create', { name, template: 'blank', pipeline: 'standard' });
    $('new-project-dialog').close();
    resetWorkspace();
    const info = await refreshProjectInfo();
    await enterActiveProject(info);
    notify(`已创建项目 ${result.activeProject.name}`);
  } catch (failure) {
    $('project-create-status').textContent = failure.message;
    throw failure;
  } finally {
    updateProjectForm();
  }
}

$('menu-file').addEventListener('click', openProjectManager);
$('project-manager-close').addEventListener('click', () => $('project-manager').close());
$('project-manager-close-footer').addEventListener('click', () => $('project-manager').close());
$('project-create-new').addEventListener('click', () => { $('project-manager').close(); showNewProjectDialog(); });
$('new-project-close').addEventListener('click', () => $('new-project-dialog').close());
$('new-project-cancel').addEventListener('click', () => $('new-project-dialog').close());
$('project-name').addEventListener('input', updateProjectForm);
$('new-project-form').addEventListener('submit', event => { event.preventDefault(); operation(createProject); });

await loadIcons();
for (const node of document.querySelectorAll('[data-project-icon]')) node.append(icon(node.dataset.projectIcon));
for (const button of document.querySelectorAll('[data-tool]')) {
  button.replaceChildren(icon(TOOL_ICON[button.dataset.tool] ?? 'point'));
  button.addEventListener('click', () => { for (const other of document.querySelectorAll('[data-tool]')) other.classList.toggle('active', other === button); sendViewMode(); });
}
for (const button of document.querySelectorAll('[data-bottom]')) button.addEventListener('click', () => {
  const name = button.dataset.bottom;
  if (name === 'assets') { openManager(); return; }
  if (name === 'issues' || name === 'performance') { openDiagnostics(); return; }
  if (name === 'replay') { activateNative(); return; }
  state.bottom = name; for (const other of document.querySelectorAll('[data-bottom]')) other.classList.toggle('active', other === button); renderBottom();
});
$('left-search').addEventListener('input', renderLeft);
$('left-add').addEventListener('click', () => activeTab()?.kind === 'scene' ? addEntity() : $('import-dialog').showModal());
$('open-manager').addEventListener('click', openManager); $('manager-close').addEventListener('click', () => $('asset-manager').close()); $('details-close').addEventListener('click', () => { state.managerSelected = null; renderManager(); });
$('manager-search').addEventListener('input', renderManager); $('manager-type').addEventListener('change', renderManager);
$('menu-create').addEventListener('click', () => state.activeProject ? $('import-dialog').showModal() : showNewProjectDialog());
$('menu-view').addEventListener('click', () => activateNative());
$('menu-edit').addEventListener('click', () => operation(async () => { if (!state.canUndo) throw new Error('没有可撤销的场景保存'); adopt(await call('scene_undo', { scenePath: state.scenePath, projectId: state.scene.projectId, expectedRevision: state.scene.revision })); await loadCatalogs(); renderAll(); activateNative(); }));
async function build() { if (dirty()) throw new Error('请先保存地图修改，再构建。'); const result = await call('scene_build', { scenePath: state.scenePath }); log(`构建完成 · ${result.outputDirectory ?? result.directory ?? 'reports/editor-builds'}`); notify('场景构建完成'); }
$('menu-build').addEventListener('click', () => operation(build)); $('build-button').addEventListener('click', () => operation(build));
$('preview-button').addEventListener('click', () => operation(async () => { if (dirty()) await saveScene(); else activateNative(); }));
$('run-button').addEventListener('click', () => operation(async () => {
  if (state.runtime.running) {
    state.runtime = await call('runtime_stop');
  } else {
    if (!state.scenePath) throw new Error('请先打开场景。');
    if (dirty()) await saveScene();
    state.runtime = await call('runtime_start', { scenePath: state.scenePath });
    notify('已启动运行窗口：WASD 行走 · Shift 奔跑 · 空格跳跃 · 右键拖动视角');
  }
  renderStatus();
}));
$('simulate-button').addEventListener('click', () => operation(async () => {
  const tab = activeTab();
  if (!tab || !['scene', 'diagnostics', 'audio'].includes(tab.kind)) throw new Error('请先打开关卡视图。');
  if (!bridge) throw new Error('物理模拟需要 Electron 原生视口。');
  const play = state.nativeAck?.play === 'simulate' ? 'edit' : 'simulate';
  const result = await bridge.command({ command: 'play', play, pause: false, revision: state.nativeRevision });
  if (!result?.ok) throw new Error(result?.error ?? '物理模拟切换失败');
  notify(play === 'simulate' ? '物理模拟中：推进物理，不接管角色输入' : '已停止模拟，恢复编辑位置');
}));
setInterval(async () => {
  if (!state.activeProject || state.busy) return;
  try {
    state.runtime = await call('runtime_status');
    renderStatus();
    if (state.runtime.error && state.runtime.error !== state.runtimeError) showError(new Error(state.runtime.error));
    state.runtimeError = state.runtime.error;
  } catch { /* The connection indicator handles server reconnection. */ }
}, 1500);
for (const id of ['import-close', 'import-cancel']) $(id).addEventListener('click', () => $('import-dialog').close());
$('import-form').addEventListener('submit', event => { event.preventDefault(); operation(async () => {
  $('import-status').textContent = '正在导入…';
  const result = await call('asset_import', { source: $('import-source').value.trim(), semantic: $('import-semantic').value, size: $('import-texture-size')?.value ?? 'auto' });
  await loadCatalogs(); $('import-status').textContent = `已导入 ${result.manifest.sourceName}`; $('import-dialog').close();
  const asset = state.assets.find(item => item.asset === result.manifest.id); if (asset) openAsset(asset); else { state.managerSelected = state.assets.find(item => item.id.includes(result.manifest.id))?.id ?? null; openManager(); }
}); });
document.addEventListener('keydown', event => {
  const typing = ['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName) || document.activeElement?.isContentEditable;
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') { event.preventDefault(); if (dirty()) operation(saveScene); }
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') { event.preventDefault(); $('left-search').focus(); }
  if ((event.ctrlKey || event.metaKey) && !typing && event.key.toLowerCase() === 'z') {
    event.preventDefault();
    if (undoDraft() || !state.canUndo) return;
    operation(async () => { adopt(await call('scene_undo', { scenePath: state.scenePath, projectId: state.scene.projectId, expectedRevision: state.scene.revision })); await loadCatalogs(); renderAll(); activateNative(); });
    return;
  }
  if ((event.ctrlKey || event.metaKey) && !typing && event.key.toLowerCase() === 'y') {
    event.preventDefault(); redoDraft(); return;
  }
  if (event.key === 'Delete' && activeTab()?.kind === 'scene' && !typing && state.selectedEntity) operation(() => removeEntity(state.selectedEntity));
});
window.addEventListener('beforeunload', event => { if (dirty()) { event.preventDefault(); event.returnValue = ''; } });

let needsProject = false;
await operation(async () => {
  const info = await refreshProjectInfo();
  needsProject = !(await enterActiveProject(info));
});
if (needsProject) showNewProjectDialog();
