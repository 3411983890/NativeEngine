import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createToolService, ToolError } from '../ai/tool-service.mjs';
import { loadScene, commitScene, commitSceneUpdate, undoSceneFile, sceneHash, buildScene, validateScene } from '../engine/scene/index.mjs';
import { workspaceData } from './workspace-data.mjs';
import { textureNames } from '../tools/material.mjs';
import { previewMaterialInstance, applyMaterialInstance } from '../tools/material.mjs';
import { ASSET_CAPABILITIES, importAsset, inspectAsset, listImports, attachModel, attachAudio, attachTexture, attachEnvironment, appendAnimation, fitSceneCharacter, verifyImport, checkoutSceneImport } from '../engine/assets/import-service.mjs';
import {reimportAsset} from '../engine/assets/reimport.mjs';
import {updateSceneImport} from '../engine/assets/reimport-scene.mjs';
import {createHairService} from './hair-service.mjs';

const DEFAULT_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const DEFAULT_SCENE = '.screen/courtyard.scene.json';
const SCENE_ROOTS = ['assets', '.screen'];
const ACTIONS = new Set([
  'project_info', 'scene_open', 'scene_preview', 'scene_apply', 'scene_undo',
  'runtime_status', 'runtime_start', 'runtime_stop', 'runtime_capture', 'material_patch',
  'material_instance',
  'asset_capabilities', 'asset_inspect', 'asset_import', 'asset_list', 'asset_attach', 'asset_preview', 'asset_append_animation', 'asset_fit_character', 'asset_open_scene',
  'asset_reimport', 'asset_update',
  'workspace_data', 'scene_configure', 'scene_build',
  // Hair and garment authoring runs through the plugin service. Listing the actions here
  // is what makes them discoverable in the editor and what keeps them behind the same
  // revision/undo contract as scene edits.
  'hair_capabilities', 'hair_reference_register', 'hair_analysis_reconcile', 'hair_create',
  'hair_load', 'hair_patch', 'hair_undo', 'hair_redo', 'hair_capture', 'hair_validate',
  'hair_export', 'hair_selection',
  'garment_inspect', 'garment_bind', 'garment_physics', 'garment_validate', 'garment_simulate',
  'weights_export', 'weights_patch', 'weights_binary',
  'plugin_list',
]);

// Editor action → hair service action. The editor action names use underscores because the
// UI and the existing service contract do; the service uses dotted names.
const HAIR_ACTIONS = {
  hair_capabilities: 'hair.capabilities',
  hair_reference_register: 'hair.reference.register',
  hair_analysis_reconcile: 'hair.analysis.reconcile',
  hair_create: 'hair.create',
  hair_load: 'hair.load',
  hair_patch: 'hair.patch',
  hair_undo: 'hair.undo',
  hair_redo: 'hair.redo',
  hair_capture: 'hair.capture',
  hair_validate: 'hair.validate',
  hair_export: 'hair.export',
  hair_selection: 'hair.selection',
  garment_inspect: 'garment.inspect',
  garment_bind: 'garment.bind',
  garment_physics: 'garment.physics',
  garment_validate: 'garment.validate',
  garment_simulate: 'garment.simulate',
  weights_export: 'weights.export',
  weights_patch: 'weights.patch',
  weights_binary: 'weights.binary',
  plugin_list: 'plugin.list',
};

function inside(parent, child) {
  const relative = path.relative(parent, child);
  return relative !== '' && relative !== '..' && !relative.startsWith(`..${path.sep}`)
    && !path.isAbsolute(relative);
}

function checkObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new ToolError('INVALID_ARGUMENTS', 'Arguments must be an object');
  }
}

// The editor delegates all scene writes to the same tools used by CLI and MCP.
// Its own native process API accepts only a scene path, never an executable or arguments.
export function createEditorService({ root = DEFAULT_ROOT, executable, spawnProcess = spawn } = {}) {
  const projectRoot = path.resolve(root);
  const tools = createToolService({ root: projectRoot, executable });
  // One instance shared by the whole editor session so undo history, the idempotency log
  // and bound garments are consistent between panel clicks and AI calls.
  const hair = createHairService({ root: projectRoot });
  const captureDirectory = path.join(projectRoot, 'reports', 'editor-captures');
  let runtime = null;
  let runtimeInfo = { running: false, pid: null, scenePath: null, startedAt: null, lastExit: null, error: null, mode: 'external-window' };
  let runtimeQueue = Promise.resolve();
  let closed = false;
  let closePromise;
  const captures = new Set();

  async function sceneFile(input = DEFAULT_SCENE, {editable=false}={}) {
    if (typeof input !== 'string' || !input || path.isAbsolute(input)
        || input.split(/[\\/]/).includes('..') || !input.endsWith('.scene.json')) {
      throw new ToolError('INVALID_PATH', 'scenePath must be under assets/ or .screen/');
    }
    const absolute = path.resolve(projectRoot, input);
    const rootReal = await fs.realpath(projectRoot);
    const logical = path.relative(rootReal, absolute);
    const top = logical.split(path.sep)[0]?.toLowerCase();
    if (!SCENE_ROOTS.includes(top)) throw new ToolError('INVALID_PATH', 'scenePath must be under assets/ or .screen/');
    let real;
    try { real = await fs.realpath(absolute); }
    catch (error) {
      if (error.code === 'ENOENT') throw new ToolError('FILE_NOT_FOUND', 'Scene does not exist');
      throw error;
    }
    let sceneRoot = null;
    for (const name of SCENE_ROOTS) {
      const candidate = path.join(projectRoot, name);
      try {
        const candidateReal = await fs.realpath(candidate);
        if (inside(rootReal, candidateReal) && inside(candidateReal, real)) { sceneRoot = candidateReal; break; }
      } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    if (!sceneRoot) throw new ToolError('INVALID_PATH', 'scenePath resolves outside assets/ or .screen/');
    if(editable){const imported=path.join(projectRoot,'assets','imported');if(inside(imported,real))throw new ToolError('INVALID_PATH','Imported packages are immutable; open a scene copy before editing');}
    const stat = await fs.stat(real);
    if (!stat.isFile()) throw new ToolError('INVALID_PATH', 'scenePath must identify a file');
    if (stat.size > 16 * 1024 * 1024) throw new ToolError('FILE_TOO_LARGE', 'Scene exceeds 16 MiB');
    return { file: real, scenePath: path.relative(projectRoot, absolute).split(path.sep).join('/') };
  }

  async function safeProjectDirectory(directory) {
    const rootReal = await fs.realpath(projectRoot);
    let ancestor = directory;
    while (true) {
      try {
        const real = await fs.realpath(ancestor);
        if (real !== rootReal && !inside(rootReal, real)) {
          throw new ToolError('INVALID_PATH', 'Output directory resolves outside the project');
        }
        break;
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        ancestor = path.dirname(ancestor);
      }
    }
    await fs.mkdir(directory, { recursive: true });
  }

  async function binary() {
    for (const candidate of executable ? [path.resolve(executable)] : [
      path.join(projectRoot, 'build', 'assets-host', 'NativeGPU.exe'),
      path.join(projectRoot, 'build', 'NativeGPU.exe'), path.join(projectRoot, 'NativeGPU.exe'),
    ]) {
      try { if ((await fs.stat(candidate)).isFile()) return candidate; }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    throw new ToolError('NATIVE_BINARY_MISSING', 'NativeGPU.exe is missing; run npm run build:native first');
  }

  function sceneDependencyDirectory(file) {
    return path.dirname(file).split(path.sep).includes('.screen') ? path.join(projectRoot, 'assets') : path.dirname(file);
  }

  async function openScene(input) {
    const { file, scenePath } = await sceneFile(input);
    const scene = await loadScene(file);
    let history = [];
    try {
      const saved = JSON.parse(await fs.readFile(`${file}.history.json`, 'utf8'));
      // loadScene already validates that history matches this exact revision.
      history = saved.undoStack.map((record, index, records) => ({
        revision: index + 1 < records.length ? records[index + 1].before.revision : scene.revision,
        label: `场景修改 · ${record.before.name}`,
      })).reverse();
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    return { scene, scenePath, textures: textureNames(projectRoot,file), canUndo: history.length > 0, history };
  }

  async function projectInfo() {
    const scenes = [];
    async function walk(directory) {
      let entries;
      try { entries = await fs.readdir(directory, { withFileTypes: true }); }
      catch (error) { if (error.code === 'ENOENT') return; throw error; }
      for (const entry of entries) {
        if (entry.isSymbolicLink()) continue;
        const file = path.join(directory, entry.name);
        if(entry.isDirectory()&&path.resolve(file)===path.join(projectRoot,'assets/imported'))continue;
        if (entry.isDirectory()) { await walk(file); continue; }
        if (!entry.isFile() || !entry.name.endsWith('.scene.json')) continue;
        const relative = path.relative(projectRoot, file).split(path.sep).join('/');
        try {
          const opened = await openScene(relative);
          scenes.push({ path: relative, name: opened.scene.name });
        } catch {
          // Keep invalid scenes visible so opening one produces its real diagnostic.
          scenes.push({ path: relative, name: entry.name });
        }
      }
    }
    const rootReal = await fs.realpath(projectRoot);
    for (const name of SCENE_ROOTS) {
      try {
        const sceneRoot = await fs.realpath(path.join(projectRoot, name));
        if (!inside(rootReal, sceneRoot)) throw new ToolError('INVALID_PATH', `${name}/ resolves outside the project`);
        await walk(path.join(projectRoot, name));
      } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    scenes.sort((a, b) => a.path.localeCompare(b.path));
    let version = 'unknown';
    try { version = (await fs.readFile(path.join(projectRoot, 'VERSION'), 'utf8')).trim(); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    let nativeAvailable = true;
    try { await binary(); } catch (error) { if (error.code !== 'NATIVE_BINARY_MISSING') throw error; nativeAvailable = false; }
    return { name: 'NativeEngine', version, scenes, nativeAvailable, mode: 'external-window' };
  }

  function status() { return { ...runtimeInfo }; }

  function serialized(action) {
    const pending = runtimeQueue.then(action, action);
    runtimeQueue = pending.catch(() => {});
    return pending;
  }

  async function stopRuntime() {
    const child = runtime;
    if (!child) return status();
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.off('exit', done);
        reject(new ToolError('RUNTIME_STOP_FAILED', 'The native preview did not stop'));
      }, 3000);
      function done() { clearTimeout(timer); resolve(); }
      child.once('exit', done);
      try { child.kill(); }
      catch (error) { clearTimeout(timer); child.off('exit', done); reject(error); }
    });
    return status();
  }

  async function startRuntime(input, assetDirectory) {
    const { file, scenePath } = await sceneFile(input);
    const scene=await loadScene(file);
    const executablePath = await binary();
    if (runtime && runtimeInfo.scenePath === scenePath && runtimeInfo.assetDirectory===assetDirectory) return status();
    await stopRuntime();
    const reportDirectory = path.join(projectRoot, 'reports');
    await safeProjectDirectory(reportDirectory);
    const report = path.join(reportDirectory, `editor-session-${randomUUID()}.json`);
    let child;
    try {
      const assetArgs=assetDirectory?['--asset',assetDirectory]:Object.keys(scene.materials??{}).length||scene.entities.some(e=>e.mesh||e.character)?['--textures',path.join(projectRoot,'assets','textures')]:[];
      child = spawnProcess(executablePath, ['--scene', file, '--report', report, '--watch',...assetArgs], {
        cwd: projectRoot, stdio: 'ignore', windowsHide: false, shell: false,
      });
    } catch (error) { throw new ToolError('NATIVE_START_FAILED', error.message); }
    runtime = child;
    let spawned = false;
    child.once('spawn', () => { spawned = true; });
    runtimeInfo = { running: true, pid: child.pid ?? null, scenePath, ...(assetDirectory?{assetDirectory}:{}), startedAt: new Date().toISOString(), lastExit: null, error: null, mode: 'external-window' };
    child.once('exit', (code, signal) => {
      if (runtime !== child) return;
      runtime = null;
      runtimeInfo = { ...runtimeInfo, running: false, pid: null, lastExit: { code, signal, at: new Date().toISOString() },
        error: code !== null && code !== 0 ? `Native preview exited with code ${code}` : null };
    });
    child.on('error', error => {
      if (runtime !== child) return;
      // A later error can mean a kill failed, not that the process exited. Keep
      // ownership until an exit event, so another preview is never started over it.
      if (!spawned) runtime = null;
      runtimeInfo = { ...runtimeInfo, running: spawned, pid: spawned ? child.pid : null, error: error.message };
    });
    await new Promise((resolve, reject) => {
      child.once('spawn', resolve);
      child.once('error', error => reject(new ToolError('NATIVE_START_FAILED', error.message)));
    });
    return status();
  }

  async function captureRuntime(input) {
    if (captures.size > 0) throw new ToolError('CAPTURE_BUSY', 'A native capture is already running');
    // Reserve the slot before async path resolution so simultaneous calls cannot race.
    const slot = {};
    slot.done = new Promise(resolve => { slot.resolveDone = resolve; });
    captures.add(slot);
    try {
      const { file } = await sceneFile(input);
      const scene = await loadScene(file);
      const executablePath = await binary();
      await safeProjectDirectory(captureDirectory);
      const id = randomUUID();
      const image = path.join(captureDirectory, `${id}.png`);
      const report = path.join(captureDirectory, `${id}.json`);
      // Freeze exactly the revision described by this capture. A simultaneous editor
      // commit must not change what the short-lived native renderer will load.
      let snapshot = path.join(captureDirectory, `${id}.scene.json`),assetArgs=[];
      await fs.writeFile(snapshot, `${JSON.stringify(scene)}\n`);
      if(scene.entities.some(e=>e.mesh||e.character)) {
        const bundle=await buildScene(scene,path.join(captureDirectory,`${id}.assets`),{assetsDirectory:path.join(projectRoot,'assets'),sceneDirectory:sceneDependencyDirectory(file)});
        snapshot=path.join(bundle.packageDirectory,'scene.json');assetArgs=['--textures',path.join(bundle.packageDirectory,'textures')];
      }
      if (closed) throw new ToolError('EDITOR_CLOSED', 'Editor service has closed');
      const child = spawnProcess(executablePath, [
        '--smoke', '--frames', '240', '--scene', snapshot, '--report', report,
        '--screenshot', image, '--screenshot-frame', '3', '--render-size', '1280x720', '--no-watch',...assetArgs,
      ], { cwd: projectRoot, stdio: 'ignore', windowsHide: true, shell: false });
      slot.child = child;
      const exitCode = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          child.kill();
          reject(new ToolError('NATIVE_TIMEOUT', 'Native capture exceeded 30 seconds'));
        }, 30_000);
        child.once('error', error => { clearTimeout(timer); reject(new ToolError('NATIVE_START_FAILED', error.message)); });
        child.once('exit', code => { clearTimeout(timer); resolve(code); });
      });
      if (exitCode !== 0) throw new ToolError('CAPTURE_FAILED', `Native capture exited with code ${exitCode}`);
      const contents = await fs.readFile(image).catch(() => null);
      if (!contents || contents.length < 8 || !contents.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) {
        throw new ToolError('CAPTURE_FAILED', 'The native renderer did not produce a valid PNG');
      }
      return { imagePath: `/captures/${id}.png`, capturedAt: new Date().toISOString(), revision: scene.revision };
    } finally { captures.delete(slot); slot.resolveDone(); }
  }

  async function dispatch(action, args) {
    if (HAIR_ACTIONS[action]) {
      const result = await hair.call(HAIR_ACTIONS[action], args);
      if (result && result.ok === false) throw new ToolError(result.error.code, result.error.message, result.error.details);
      return result;
    }
    if (action === 'workspace_data') { const {file} = await sceneFile(args.scenePath); return workspaceData(projectRoot, await loadScene(file), file); }
    if (action === 'scene_build') {
      const {file} = await sceneFile(args.scenePath);
      return serialized(async () => {
        const directory = path.join(projectRoot, 'reports/editor-builds');
        await safeProjectDirectory(directory);
        return buildScene(await loadScene(file), directory, { target: 'windows-x64', assetsDirectory: path.join(projectRoot, 'assets'), sceneDirectory: sceneDependencyDirectory(file) });
      });
    }
    if (action === 'scene_configure') {
      const {file, scenePath} = await sceneFile(args.scenePath, {editable:true});
      const keys = Object.keys(args.changes ?? {});
      if (!keys.length || keys.some(key => !['settings','lighting','audio','materials'].includes(key))) throw new ToolError('INVALID_ARGUMENTS', 'Unsupported scene setting');
      if (args.apply !== true) {
        const scene = await loadScene(file);
        if (scene.revision !== args.expectedRevision || scene.projectId !== args.projectId) throw new ToolError('REVISION_CONFLICT', '场景版本已改变，请先检查外部修改。');
        const candidate = {...scene, ...structuredClone(args.changes)};
        const result = validateScene(candidate);
        if (!result.valid) throw new ToolError('INVALID_SCENE', '场景设置无效', result.errors);
        return {scene:candidate, applied:false};
      }
      return serialized(async () => {
        await commitSceneUpdate(file, args, scene => Object.assign(scene, structuredClone(args.changes)));
        return {...await openScene(scenePath), applied:true};
      });
    }
    const importDirectory=path.join(projectRoot,'assets/imported');
    if(action==='asset_capabilities')return ASSET_CAPABILITIES;
    if(action==='asset_list')return listImports(importDirectory);
    if(action==='asset_inspect')return inspectAsset(args.source);
    if(action==='asset_import')return serialized(async()=>{await safeProjectDirectory(importDirectory);return importAsset({source:args.source,output:importDirectory,size:args.size??'auto',semantic:args.semantic??'albedo',coordinates:args.coordinates});});
    if(action==='asset_reimport'){
      if(!/^[a-f0-9]{64}$/.test(args.id))throw new ToolError('INVALID_ARGUMENTS','Invalid asset id');
      return serialized(async()=>{await safeProjectDirectory(importDirectory);return reimportAsset({directory:path.join(importDirectory,args.id),source:args.source,output:importDirectory});});
    }
    if(action==='asset_update'){
      if(!/^[a-f0-9]{64}$/.test(args.id)||!/^[a-f0-9]{64}$/.test(args.previousId))throw new ToolError('INVALID_ARGUMENTS','Invalid asset version IDs');
      const {file}=await sceneFile(args.scenePath,{editable:true});
      return serialized(()=>updateSceneImport({previousDirectory:path.join(importDirectory,args.previousId),directory:path.join(importDirectory,args.id),scenePath:file,expectedRevision:args.expectedRevision,entityIds:args.entityIds}));
    }
    if(action==='asset_append_animation') {
      if(!/^[a-f0-9]{64}$/.test(args.id))throw new ToolError('INVALID_ARGUMENTS','Invalid asset id');
      return serialized(()=>appendAnimation({directory:path.join(importDirectory,args.id),source:args.source,output:importDirectory,options:args.options??{}}));
    }
    if(action==='asset_preview') {
      if(!/^[a-f0-9]{64}$/.test(args.id))throw new ToolError('INVALID_ARGUMENTS','Invalid asset id');
      const directory=path.join(importDirectory,args.id);
      return serialized(()=>startRuntime(`assets/imported/${args.id}/preview.scene.json`,directory));
    }
    if(action==='asset_open_scene') {
      if(!/^[a-f0-9]{64}$/.test(args.id))throw new ToolError('INVALID_ARGUMENTS','Invalid asset id');
      return serialized(async()=>{const outputDirectory=path.join(projectRoot,'assets/scenes');await safeProjectDirectory(outputDirectory);const result=await checkoutSceneImport({directory:path.join(importDirectory,args.id),outputDirectory});return {scenePath:path.relative(projectRoot,result.scenePath).split(path.sep).join('/')};});
    }
    if(action==='asset_attach') {
      if(!/^[a-f0-9]{64}$/.test(args.id))throw new ToolError('INVALID_ARGUMENTS','Invalid asset id');
      const {file}=await sceneFile(args.scenePath,{editable:true});
      const directory=path.join(importDirectory,args.id),manifest=await verifyImport(directory);
      if(manifest.kind==='audio')return serialized(()=>attachAudio({directory,scenePath:file,expectedRevision:args.expectedRevision,options:args.audioOptions??{}}));
      if(manifest.kind==='texture')return serialized(()=>attachTexture({directory,scenePath:file,expectedRevision:args.expectedRevision,material:args.material}));
      if(manifest.kind==='environment')return serialized(()=>attachEnvironment({directory,scenePath:file,expectedRevision:args.expectedRevision,options:args.environmentOptions??{}}));
      return serialized(()=>attachModel({directory:path.join(importDirectory,args.id),scenePath:file,expectedRevision:args.expectedRevision,collider:args.collider??false,characterOptions:args.characterOptions??{}}));
    }
    if(action==='asset_fit_character') {const {file}=await sceneFile(args.scenePath,{editable:true});return serialized(()=>fitSceneCharacter({scenePath:file,entityId:args.entityId,expectedRevision:args.expectedRevision,mode:args.mode,alignModel:args.alignModel??true}));}
    if (action === 'project_info') return projectInfo();
    if (action === 'scene_open') return openScene(args.scenePath);
    if (action === 'runtime_status') return status();
    if (action === 'runtime_start') return serialized(() => startRuntime(args.scenePath));
    if (action === 'runtime_stop') return serialized(stopRuntime);
    if (action === 'runtime_capture') return captureRuntime(args.scenePath);
    const { file, scenePath } = await sceneFile(args.scenePath,{editable:['scene_apply','scene_undo','material_patch','material_instance'].includes(action)});
    if (action === 'material_patch') {
      const result = await tools.callTool('material_patch', { ...args, scenePath });
      if (!result.ok) throw new ToolError(result.error.code, result.error.message, result.error.details);
      return result.result;
    }
    if (action === 'material_instance') {
      const options = {
        scenePath:file, source: args.source, name: args.name,
        entityId: args.entityId ?? null, root: projectRoot, expectedRevision: args.expectedRevision,
        sourceSlot:args.sourceSlot??null,
      };
      const result = args.apply === true ? await applyMaterialInstance(options) : previewMaterialInstance(options);
      return result;
    }
    if (action === 'scene_preview') {
      const result = await tools.callTool(action, { ...args, scenePath, includeScene: true });
      if (!result.ok) throw new ToolError(result.error.code, result.error.message, result.error.details);
      return result.result;
    }
    // Keep the exact immutable result of the authoritative core operation. Reading
    // history afterwards is only enrichment: another MCP client may already be
    // committing the next revision, which must not turn this success into failure.
    let committed;
    let summary;
    if (action === 'scene_apply') {
      committed = await commitScene(file, args.transaction);
      summary = {
        projectId: committed.projectId, baseRevision: committed.baseRevision,
        revision: committed.revision, changes: committed.changes, sceneHash: sceneHash(committed.scene),
      };
    } else {
      committed = await undoSceneFile(file, { projectId: args.projectId, expectedRevision: args.expectedRevision });
      summary = { revision: committed.revision, sceneHash: sceneHash(committed.scene) };
    }
    try {
      const opened = await openScene(scenePath);
      if (opened.scene.revision === committed.revision && sceneHash(opened.scene) === summary.sceneHash) {
        return { ...summary, ...opened };
      }
    } catch { /* The write succeeded; a refresh failure is not a failed transaction. */ }
    return {
      ...summary, scene: committed.scene, scenePath, canUndo: action === 'scene_apply',
      history: action === 'scene_apply' ? [{ revision: committed.revision, label: `鍦烘櫙淇敼 路 ${committed.scene.name}` }] : [],
      refreshRequired: true,
    };
  }

  return {
    root: projectRoot,
    captureDirectory,
    async call(action, args = {}) {
      try {
        if (closed) throw new ToolError('EDITOR_CLOSED', 'Editor service has closed');
        if (!ACTIONS.has(action)) throw new ToolError('UNKNOWN_ACTION', `Unknown editor action: ${action}`);
        checkObject(args);
        return { ok: true, result: await dispatch(action, args) };
      } catch (error) {
        return { ok: false, error: { code: error.code ?? 'EDITOR_FAILURE', message: error.message ?? String(error), details: error.details ?? [] } };
      }
    },
    async close() {
      if (closed) return closePromise;
      closed = true;
      closePromise = (async () => {
        await runtimeQueue;
        const captureStops = [...captures].map(async slot => {
          slot.child?.kill();
          await slot.done;
        });
        await Promise.all([stopRuntime(), ...captureStops]);
      })();
      return closePromise;
    },
  };
}
