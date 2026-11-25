import { app, BrowserWindow, Menu, nativeTheme, session, dialog, ipcMain } from 'electron';
import { spawn } from 'node:child_process';
import net from 'node:net';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startEditorServer } from './server.mjs';

// Native viewport host (path C in .developmentAgent/editor/EDITOR-VIEWPORT.md): a separate process owns a
// WS_CHILD window inside this BrowserWindow and presents D3D12 into it. The page reports a
// rectangle; this file forwards it down a named pipe and keeps the process alive.
//
// Why a separate process instead of an N-API addon: a device-lost or a driver hang inside
// the editor's main process takes the whole UI with it. Here it costs a respawn.
const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(directory);
const HOST_EXE = path.join(root, 'build/viewport-host/nativegpu_viewport_host.exe');
const PIPE_NAME = `nativegpu-forge-${process.pid}`;
app.setName('NativeEngine');

let editor;
let window;
let exiting = false;
let viewportHost = null;
let viewportSocket = null;
let pipeServer = null;
let viewportProjectRoot = root;
// Documents what has actually been applied in the native window. The page is told this number
// so it can label the viewport "revision N" only after the host has said so.
let nativeState = {
  mode: 'vegetation', species: null, lod: 0, revision: 0, digest: null, debug: 'shaded', ok: false, error: null, frames: 0,
  play: 'edit', paused: false, orthographic: false, camera: { yaw: 0, pitch: 0, distance: 0 }, overlayRevision: 0,
};
let nextRequestId = 1;

function parseAck(line) {
  // `ack=1;req=..;rev=..;state=ok;...` — the same key=value dialect the host reads, so one
  // format describes both directions.
  if (!line.startsWith('ack=1;')) return null;
  const fields = {};
  for (const pair of line.split(';')) {
    const index = pair.indexOf('=');
    if (index <= 0) continue;
    fields[pair.slice(0, index)] = pair.slice(index + 1);
  }
  return fields;
}

function applyAck(fields) {
  const ok = fields.state === 'ok';
  nativeState = {
    mode: fields.mode ?? nativeState.mode,
    species: fields.species && fields.species !== 'none' ? fields.species : nativeState.species,
    lod: Number(fields.lod ?? nativeState.lod),
    revision: Number(fields.rev ?? nativeState.revision),
    digest: fields.digest && fields.digest !== 'none' ? fields.digest : nativeState.digest,
    debug: fields.debug ?? nativeState.debug,
    ok,
    error: ok ? null : (fields.err || 'native viewport reported an error'),
    frames: Number(fields.frames ?? nativeState.frames),
    requestId: Number(fields.req ?? 0),
    wantedRevision: Number(fields.want ?? 0),
    play: fields.play ?? nativeState.play,
    paused: fields.paused === '1',
    orthographic: fields.ortho === '1',
    camera: {
      yaw: Number(fields.camyaw ?? nativeState.camera?.yaw ?? 0),
      pitch: Number(fields.campitch ?? nativeState.camera?.pitch ?? 0),
      distance: Number(fields.camdist ?? nativeState.camera?.distance ?? 0),
    },
    overlayRevision: Number(fields.overlay ?? nativeState.overlayRevision ?? 0),
  };
  if (window && !window.isDestroyed()) window.webContents.send('forge:viewport-ack', nativeState);
  if (!ok) console.warn(`[viewport] ${nativeState.error}`);
  return nativeState;
}

function stopViewportHost() {
  if (viewportSocket) {
    viewportSocket.destroy();
    viewportSocket = null;
  }
  if (pipeServer) {
    pipeServer.close();
    pipeServer = null;
  }
  if (viewportHost) {
    // The child is parented to our window, so it also dies with the window; killing it
    // explicitly just makes the exit deterministic when the window is not destroyed first.
    const child = viewportHost;
    viewportHost = null;
    child.kill();
  }
}

async function startViewportHost(target, initialSpecies, projectRoot = viewportProjectRoot) {
  if (viewportHost) return;
  if (pipeServer) {
    const previous = pipeServer;
    pipeServer = null;
    await new Promise(resolve => previous.close(resolve));
  }
  pipeServer = net.createServer(socket => {
    viewportSocket = socket;
    socket.on('error', () => { viewportSocket = null; });
    socket.on('close', () => { viewportSocket = null; });
  });
  pipeServer.on('error', error => console.error('[viewport] pipe:', error.message));
  await new Promise(resolve => pipeServer.listen(`\\\\.\\pipe\\${PIPE_NAME}`, resolve));

  const handle = target.getNativeWindowHandle();
  // Pointer-sized handle: 8 bytes on x64. Passing the integer value through JSON would be a
  // different process's handle number, so the child receives it as a decimal string and casts.
  const parent = handle.length >= 8 ? handle.readBigUInt64LE(0) : BigInt(handle.readUInt32LE(0));
  const projectHost = path.join(projectRoot, 'build/viewport-host/nativegpu_viewport_host.exe');
  const child = spawn(existsSync(projectHost) ? projectHost : HOST_EXE, [
    '--parent', parent.toString(),
    '--pipe', PIPE_NAME,
    '--project', projectRoot,
    // Engine-owned vegetation packages are read-only runtime data; project scenes and outputs
    // stay under --project.
    '--cooked', path.join(root, 'build/vegetation/cooked'),
    '--textures', path.join(root, 'build/vegetation/textures'),
    // The species is the *initial* content, not a pin: the page switches it over the pipe and
    // the ack reports which package is actually on screen.
    '--species', initialSpecies || 'oak_broadleaf',
    '--instances', '1',
  ], { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  viewportHost = child;
  child.stdout.setEncoding('utf8');
  let pending = '';
  child.stdout.on('data', chunk => {
    pending += chunk;
    for (;;) {
      const newline = pending.indexOf('\n');
      if (newline < 0) break;
      const line = pending.slice(0, newline).trim();
      pending = pending.slice(newline + 1);
      if (!line) continue;
      const ack = parseAck(line);
      if (ack) { applyAck(ack); continue; }
      console.log(`[viewport] ${line}`);
    }
  });
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', chunk => process.stderr.write(`[viewport:err] ${chunk}`));
  child.on('error', error => {
    // A missing exe is the common case on a fresh checkout: report it once and let the page
    // keep using WebGL instead of failing the editor launch.
    console.error('[viewport] spawn failed:', error.message);
    viewportHost = null;
  });
  child.on('exit', (code, signal) => {
    console.log(`[viewport] exited code=${code} signal=${signal ?? ''}`);
    if (viewportHost !== child) return;
    viewportHost = null;
    viewportSocket = null;
    if (exiting || !window || window.isDestroyed()) return;
    nativeState = { ...nativeState, ok: false, error: '原生宿主已退出，正在重启' };
    window.webContents.send('forge:viewport-ack', nativeState);
    // A crashed host is recoverable. Respawn it, put the child window back where it was and
    // replay the last content command, so a driver hiccup does not leave the page claiming a
    // revision nothing is drawing.
    setTimeout(() => {
      startViewportHost(window, lastContentCommand?.species ?? nativeState.species ?? 'oak_broadleaf', viewportProjectRoot)
        .then(() => {
          if (lastRectValue) applyViewportRect(lastRectValue);
          if (lastContentCommand) sendViewportCommand(lastContentCommand);
        })
        .catch(error => console.error('[viewport] restart:', error.message));
    }, 500);
  });
}

let warnedNoHost = false;
let lastRectLine = '';
let lastRectValue = null;
function applyViewportRect(rect) {
  if (!rect) return;
  if (!viewportSocket || viewportSocket.destroyed) {
    // The page has no way to tell that the host is missing: say it once here instead of
    // leaving an empty rectangle on screen with no explanation.
    if (!warnedNoHost) {
      warnedNoHost = true;
      console.warn('[viewport] 宿主未就绪：原生视口不会显示画面（检查 build/viewport-host/nativegpu_viewport_host.exe）');
    }
    return;
  }
  // One line per update, device pixels relative to the window's client area. The host parses
  // `key=value;` pairs, so a dropped or truncated line cannot leave a half-applied rectangle.
  if (rect.visible === false) {
    // Hide carries no geometry; sending the other keys would put `undefined` on the wire.
    // Also forget the last shown rect: re-showing the same rectangle must still be sent,
    // or the host would stay hidden after a hide/show cycle with no size change.
    lastRectLine = '';
    lastRectValue = null;
    viewportSocket.write('v=0\n');
    return;
  }
  lastRectValue = rect;
  const line = `x=${rect.x | 0};y=${rect.y | 0};w=${rect.width | 0};h=${rect.height | 0};v=1`;
  if (line !== lastRectLine) {
    // The ResizeObserver fires far more often than the rectangle actually changes.
    lastRectLine = line;
    process.stderr.write(`[viewport] rect ${line}\n`);
  }
  viewportSocket.write(line + '\n');
}

// The most recent content command, replayed after a host restart so "the native window crashed"
// does not silently turn into "the native window shows a different plant".
let lastContentCommand = null;

function sanitizeToken(value, length = 64) {
  return String(value ?? '').replace(/[^A-Za-z0-9_.-]/g, '').slice(0, length);
}

function sanitizeRelativePath(value) {
  const normalized = String(value ?? '').replace(/\\/g, '/');
  if (!normalized || normalized.length > 240 || normalized.startsWith('/') || /^[A-Za-z]:/.test(normalized)
      || normalized.split('/').includes('..') || /[;\r\n]/.test(normalized)) return null;
  return normalized;
}

// Forwards one structured command to the host. The renderer cannot compose a raw pipe line: it
// names a command and a value, and this function builds the line from a closed set of keys. A
// page that could write arbitrary bytes to the pipe could also send `species=` with a path in it.
function sendViewportCommand(command) {
  if (!command || typeof command !== 'object') return { ok: false, error: 'invalid command' };
  if (!viewportSocket || viewportSocket.destroyed) return { ok: false, error: '原生宿主未就绪' };
  const name = String(command.command ?? 'status');
  if (!['open', 'load', 'view', 'capture', 'status', 'play', 'camera', 'preview', 'previewclear', 'world', 'worldclose'].includes(name)) return { ok: false, error: `unknown command ${name}` };
  const requestId = nextRequestId++;
  const fields = [`req=${requestId}`, `cmd=${name}`];
  if (command.revision !== undefined) fields.push(`rev=${Math.max(0, Math.trunc(Number(command.revision) || 0))}`);
  if (name === 'open') {
    if (!['scene', 'material', 'character', 'effect', 'vegetation', 'world'].includes(command.mode)) return { ok: false, error: 'invalid viewport mode' };
    fields.push(`mode=${command.mode}`);
    if (command.play !== undefined) {
      if (!['edit', 'simulate', 'run'].includes(command.play)) return { ok: false, error: 'invalid play mode' };
      fields.push(`play=${command.play}`);
    }
    for (const key of ['scene', 'library', 'cooked']) {
      if (command[key] === undefined) continue;
      const value = sanitizeRelativePath(command[key]);
      if (!value) return { ok: false, error: `invalid ${key} path` };
      fields.push(`${key}=${value}`);
    }
    for (const key of ['asset', 'material', 'clip', 'effect']) {
      if (command[key]) fields.push(`${key}=${sanitizeToken(command[key], key === 'clip' ? 128 : 64)}`);
    }
  }
  if (name === 'play') {
    const play = command.play === undefined ? 'edit' : String(command.play);
    if (!['edit', 'simulate', 'run'].includes(play)) return { ok: false, error: 'invalid play mode' };
    fields.push(`play=${play}`, `pause=${command.pause ? 1 : 0}`);
    if (command.step) fields.push('step=1');
  }
  if (name === 'camera') {
    if (command.view !== undefined) {
      if (!['perspective', 'top', 'front', 'side'].includes(command.view)) return { ok: false, error: 'invalid camera view' };
      fields.push(`view=${command.view}`);
    }
    if (command.focus) fields.push('focus=1');
    if (command.entity) fields.push(`entity=${sanitizeToken(command.entity, 64)}`);
  }
  if (name === 'preview') {
    const entity = sanitizeToken(command.entity, 64);
    if (!entity) return { ok: false, error: 'preview needs an entity id' };
    const numbers = ['px', 'py', 'pz', 'sx', 'sy', 'sz'].map((key, index) => {
      const value = Number(command[key]);
      if (!Number.isFinite(value)) return null;
      const limit = index < 3 ? 100000 : 10000;
      return Math.min(limit, Math.max(index < 3 ? -limit : 0.001, value));
    });
    if (numbers.some(value => value === null)) return { ok: false, error: 'preview transform must be finite' };
    fields.push(`entity=${entity}`, `active=${command.active === false ? 0 : 1}`);
    for (const [index, key] of ['px', 'py', 'pz', 'sx', 'sy', 'sz'].entries()) fields.push(`${key}=${numbers[index].toFixed(4)}`);
  }
  if (name === 'world') {
    // A cooked world is addressed by a project-relative directory; the host re-checks that
    // it stays inside the project before it opens the manifest.
    const cooked = sanitizeRelativePath(command.cooked);
    if (!cooked) return { ok: false, error: 'invalid cooked directory' };
    fields.push(`cooked=${cooked}`);
  }
  if (command.species) fields.push(`species=${sanitizeToken(command.species, 64)}`);
  if (command.lod !== undefined) fields.push(`lod=${Math.max(0, Math.trunc(Number(command.lod) || 0))}`);
  if (command.digest) fields.push(`digest=${sanitizeToken(command.digest, 64).toLowerCase()}`);
  if (command.debug) fields.push(`debug=${sanitizeToken(command.debug, 32).toLowerCase()}`);
  if (command.path) fields.push(`path=${String(command.path).replace(/[;\r\n]/g, '').slice(0, 260)}`);
  if (command.wind !== undefined) fields.push(`wind=${Math.min(2, Math.max(0, Number(command.wind) || 0)).toFixed(3)}`);
  if (command.shadow !== undefined) fields.push(`shadow=${Math.min(1, Math.max(0, Number(command.shadow) || 0)).toFixed(3)}`);
  const line = fields.join(';');
  // A control line is bounded by the host as well; refusing here keeps the failure in the log
  // rather than in the pipe.
  if (line.length > 900) return { ok: false, error: 'command is too long' };
  if (name === 'load' || name === 'open') lastContentCommand = command;
  viewportSocket.write(line + '\n');
  return { ok: true, requestId };
}

async function start() {
  try {
    editor = await startEditorServer({
      projectsRoot: path.join(app.getPath('documents'), 'nativeEngineProject'),
    });
    viewportProjectRoot = editor.activeProject()?.location ?? root;
    nativeTheme.themeSource = 'dark';
    Menu.setApplicationMenu(null);
    session.defaultSession.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
    session.defaultSession.setPermissionCheckHandler(() => false);
    window = new BrowserWindow({
      title: 'NativeEngine',
      icon: path.join(directory, 'ui', 'media', 'NativeEngine.ico'),
      width: 1480,
      height: 960,
      minWidth: 1000,
      minHeight: 700,
      backgroundColor: '#181819',
      show: false,
      webPreferences: {
        nodeIntegration: false,
        contextIsolation: true,
        sandbox: true,
        preload: path.join(directory, 'preload.cjs'),
      },
    });
    window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    window.webContents.on('will-prevent-unload', event => {
      const choice = dialog.showMessageBoxSync(window, {
        type: 'question', title: '尚有未保存修改', message: '放弃当前草稿并关闭编辑器？',
        buttons: ['继续编辑', '放弃并关闭'], defaultId: 0, cancelId: 0, noLink: true,
      });
      if (choice === 1) event.preventDefault();
    });
    const allowed = new Set([`${editor.url}/`, `${editor.url}/index.html`]);
    window.webContents.on('will-navigate', (event, url) => {
      if (!allowed.has(url)) event.preventDefault();
    });
    ipcMain.on('forge:viewport', (event, rect) => {
      if (event.sender !== window.webContents) return;
      applyViewportRect(rect);
    });
    ipcMain.handle('forge:viewport-command', (event, command) => {
      if (event.sender !== window.webContents) return { ok: false, error: 'not authorised' };
      return sendViewportCommand(command);
    });
    ipcMain.handle('forge:viewport-project', async (event, folder) => {
      if (event.sender !== window.webContents) return { ok: false, error: 'not authorised' };
      try {
        const nextRoot = await editor.resolveProject(String(folder ?? ''));
        if (nextRoot === viewportProjectRoot && viewportHost) return { ok: true };
        viewportProjectRoot = nextRoot;
        lastContentCommand = null;
        lastRectLine = '';
        lastRectValue = null;
        nativeState = { ...nativeState, mode: 'vegetation', revision: 0, ok: false, error: null, frames: 0, requestId: 0, play: 'edit', paused: false, overlayRevision: 0 };
        stopViewportHost();
        await startViewportHost(window, 'oak_broadleaf', viewportProjectRoot);
        return { ok: true };
      } catch (error) {
        return { ok: false, error: error.message };
      }
    });
    ipcMain.handle('forge:viewport-state', (event) => {
      if (event.sender !== window.webContents) return null;
      return nativeState;
    });
    window.once('ready-to-show', () => window.show());
    await window.loadURL(editor.url);
    // Started after the window exists: the host needs its HWND, and a child window cannot be
    // parented before the parent has one.
    await startViewportHost(window).catch(error => console.error('[viewport] start:', error.message));
    console.log(`[editor] ready at ${editor.url}`);
  } catch (error) {
    // stderr is unbuffered and reaches the launcher even when the window never appears, so a
    // startup failure is visible before the modal box blocks everything.
    process.stderr.write(`[editor] startup failed: ${error.stack || error.message}\n`);
    dialog.showErrorBox('NativeEngine 启动失败', error.message);
    app.quit();
  }
}

// `await app.whenReady()` at the top level of an ESM main script deadlocks in Electron 44.9.1:
// the module is evaluated before the app is ready, and the loader is still holding the loop
// that would deliver the ready event. Registering a callback is the form that works.
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!window) return;
    if (window.isMinimized()) window.restore();
    window.show();
    window.focus();
  });
  app.on('before-quit', event => {
    if (exiting) return;
    exiting = true;
    stopViewportHost();
    if (!editor) return;
    event.preventDefault();
    editor.close().catch(error => console.error('Editor cleanup:', error.message)).finally(() => app.quit());
  });
  app.on('window-all-closed', () => app.quit());
  app.whenReady().then(start);
}
