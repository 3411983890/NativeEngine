import http from 'node:http';
import { randomBytes, timingSafeEqual } from 'node:crypto';
import { access, readFile, realpath, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createEditorService } from './service.mjs';
import { createProjectManager, ProjectError } from './project-manager.mjs';
import { createVegetationService } from './vegetation-service.mjs';
import { createWorldService } from './world-service.mjs';
import { loadScene, commitSceneUpdate } from '../engine/scene/index.mjs';
import { findGameplayAssets } from '../engine/scene/gameplay.mjs';

const directory = path.dirname(fileURLToPath(import.meta.url));
const defaultRoot = path.dirname(directory);
const staticFiles = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/index.html', ['index.html', 'text/html; charset=utf-8']],
  ['/styles.css', ['styles.css', 'text/css; charset=utf-8']],
  ['/app.mjs', ['app.mjs', 'text/javascript; charset=utf-8']],
  ['/world.mjs', ['world.mjs', 'text/javascript; charset=utf-8']],
  ['/media/NativeEngine.svg', ['media/NativeEngine.svg', 'image/svg+xml']],
  ['/media/NativeEngine.png', ['media/NativeEngine.png', 'image/png']],
  ['/media/NativeEngine.ico', ['media/NativeEngine.ico', 'image/x-icon']],
  ['/media/icons.json', ['media/icons.json', 'application/json; charset=utf-8']],
]);
const MAX_BODY = 1024 * 1024;

// Switching a scene's world reference goes through the scene transaction boundary: the
// expected revision is checked, the revision advances, and an undo record is written. A
// world switch is a scene edit, so it must be undoable like one.
async function commitWorldReference(scenePath, worldRef, expectedRevision) {
  const file = path.resolve(projectRoot, scenePath);
  const scene = await loadScene(file, { assetsDirectory: findGameplayAssets(file) });
  const target = Number.isSafeInteger(expectedRevision) ? expectedRevision : scene.revision;
  const next = await commitSceneUpdate(
    file,
    { projectId: scene.projectId, expectedRevision: target },
    // The update callback must return the mutated scene: the transaction stores the return
    // value, not the argument it passed in.
    draft => { draft.world = worldRef; return draft; },
  );
  return { projectId: next.projectId, revision: next.revision };
}

function failure(code, message) {
  return { ok: false, error: { code, message, details: [] } };
}

function json(response, status, value) {
  response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  response.end(JSON.stringify(value));
}

async function requestJSON(request) {
  if (request.headers['content-type']?.split(';')[0].trim() !== 'application/json') {
    throw Object.assign(new Error('请求必须使用 JSON。'), { code: 'INVALID_CONTENT_TYPE', status: 415 });
  }
  let size = 0;
  const chunks = [];
  for await (const chunk of request) {
    size += chunk.length;
    if (size > MAX_BODY) {
      throw Object.assign(new Error('请求超过 1 MiB。'), { code: 'REQUEST_TOO_LARGE', status: 413 });
    }
    chunks.push(chunk);
  }
  try {
    const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    if (!body || typeof body !== 'object' || Array.isArray(body)) throw new Error();
    return body;
  } catch {
    throw Object.assign(new Error('请求不是有效的 JSON 对象。'), { code: 'INVALID_JSON', status: 400 });
  }
}

async function exists(file) {
  try { await access(file); return true; } catch { return false; }
}

async function projectVersion(engineRoot) {
  try { return (await readFile(path.join(engineRoot, 'VERSION'), 'utf8')).trim(); }
  catch { return 'unknown'; }
}

function publicProject(project) {
  return {
    folder: project.folder,
    name: project.name,
    scene: project.scene,
    template: project.template,
    pipeline: project.pipeline,
    createdAt: project.createdAt,
    location: project.path,
  };
}

/** Local transport only; scene authority stays in the existing transaction service. */
export async function startEditorServer({ root = defaultRoot, projectsRoot, port = 0, service } = {}) {
  const engineRoot = path.resolve(root);
  const version = await projectVersion(engineRoot);
  const projectManager = service ? null : createProjectManager({ engineRoot, projectsRoot, version });
  const runtimes = await Promise.all(['build/assets-host/NativeGPU.exe', 'build/NativeGPU.exe'].map(async relative => {
    const file = path.join(engineRoot, relative);
    try { return { file, modified: (await stat(file)).mtimeMs }; }
    catch (error) { if (error.code === 'ENOENT') return null; throw error; }
  }));
  const executable = runtimes.filter(Boolean).sort((a, b) => b.modified - a.modified)[0]?.file ?? path.join(engineRoot, 'build/NativeGPU.exe');
  let projectRoot = engineRoot;
  let editor = service ?? null;
  let vegetation = createVegetationService({ root: projectRoot });
  // The world service holds an editing draft, so it is per project exactly like the scene
  // and vegetation services: switching projects must not carry a draft across.
  let world = createWorldService({ root: projectRoot, commitScene: commitWorldReference });
  let active = null;
  const projectQueue = [];
  let projectBusy = false;

  async function activate(project) {
    const projectExecutable = path.join(project.path, 'build/NativeGPU.exe');
    const nextEditor = createEditorService({ root: project.path, executable: await exists(projectExecutable) ? projectExecutable : executable });
    const nextVegetation = createVegetationService({ root: project.path });
    const previousWorld = world;
    const previousEditor = editor;
    editor = nextEditor;
    vegetation = nextVegetation;
    world = createWorldService({ root: project.path, commitScene: commitWorldReference });
    if (previousWorld) previousWorld.close();
    projectRoot = project.path;
    active = publicProject(project);
    if (previousEditor) await previousEditor.close();
    return active;
  }

  async function serializedProject(operation) {
    const run = Promise.resolve().then(async () => {
      while (projectBusy) await new Promise(resolve => projectQueue.push(resolve));
      projectBusy = true;
      try { return await operation(); }
      finally {
        projectBusy = false;
        projectQueue.shift()?.();
      }
    });
    return run;
  }

  if (projectManager) {
    const latest = await projectManager.latest();
    if (latest) await activate(latest);
  }

  async function projectInfo() {
    const projects = projectManager ? await projectManager.list() : [];
    if (!editor) {
      return {
        ok: true,
        result: {
          name: 'NativeEngine', version,
          scenes: [], nativeAvailable: await exists(executable),
          activeProject: null, projects, projectsRoot: projectManager?.base ?? null,
          mode: 'project',
        },
      };
    }
    const info = await editor.call('project_info', {});
    if (!info.ok) return info;
    return {
      ok: true,
      result: {
        ...info.result,
        activeProject: active,
        projects,
        projectsRoot: projectManager?.base ?? null,
        mode: projectManager ? 'project' : info.result.mode,
      },
    };
  }

  async function projectAction(action, args) {
    if (!projectManager) return failure('PROJECT_MODE_UNAVAILABLE', '项目操作只在编辑器项目管理模式中可用。');
    try {
      if (action === 'project_list') return { ok: true, result: { projects: await projectManager.list(), activeProject: active } };
      if (action === 'project_create') {
        return await serializedProject(async () => {
          const created = await projectManager.create(args ?? {});
          const current = await projectManager.project(created.folder);
          return { ok: true, result: { activeProject: await activate(current), projects: await projectManager.list() } };
        });
      }
      if (action === 'project_open') {
        return await serializedProject(async () => {
          const current = await projectManager.open(args?.folder);
          return { ok: true, result: { activeProject: await activate(current), projects: await projectManager.list() } };
        });
      }
      return failure('UNKNOWN_ACTION', `Unknown project action: ${action}`);
    } catch (error) {
      return failure(error.code ?? 'PROJECT_FAILURE', error.message ?? String(error));
    }
  }

  const token = randomBytes(32).toString('hex');
  let origin;
  let closing;
  const server = http.createServer(async (request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('Referrer-Policy', 'no-referrer');
    response.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
    response.setHeader('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    try {
      // Host validation prevents DNS rebinding; no cross-origin API access is offered.
      if (request.headers.host !== new URL(origin).host ||
          (request.headers.origin && request.headers.origin !== origin) ||
          request.headers['sec-fetch-site'] === 'cross-site') {
        json(response, 403, failure('UNTRUSTED_ORIGIN', '编辑器只接受当前本地页面的请求。'));
        return;
      }
      const url = new URL(request.url, origin);
      if (request.method === 'GET' && url.pathname === '/api/session') {
        json(response, 200, { token });
        return;
      }
      if (request.method === 'POST' && url.pathname === '/api/call') {
        const supplied = Buffer.from(String(request.headers['x-nativegpu-token'] ?? ''));
        const expected = Buffer.from(token);
        if (supplied.length !== expected.length || !timingSafeEqual(supplied, expected)) {
          json(response, 403, failure('INVALID_SESSION', '编辑器会话已失效，请重新打开页面。'));
          return;
        }
        const body = await requestJSON(request);
        if (typeof body.action !== 'string' || Object.keys(body).some(key => !['action', 'args'].includes(key))) {
          json(response, 400, failure('INVALID_REQUEST', '需要 action 与 args。'));
          return;
        }
        if (projectManager && body.action === 'project_info') {
          json(response, 200, await projectInfo());
          return;
        }
        if (projectManager && body.action.startsWith('project_')) {
          json(response, 200, await projectAction(body.action, body.args ?? {}));
          return;
        }
        if (!editor) {
          json(response, 200, failure('NO_ACTIVE_PROJECT', '请先新建或打开项目。'));
          return;
        }
        // Two workspaces share one transport: the scene editor and Vegetation Forge. Splitting
        // by action prefix keeps the scene service's action whitelist untouched.
        if (world.handles(body.action)) {
          json(response, 200, await world.call(body.action, body.args ?? {}).catch(error => failure(error.code ?? 'WORLD_FAILURE', error.message)));
          return;
        }
        if (vegetation.handles(body.action)) {
          json(response, 200, await vegetation.call(body.action, body.args ?? {}));
          return;
        }
        json(response, 200, await editor.call(body.action, body.args ?? {}));
        return;
      }
      if (request.method !== 'GET' && request.method !== 'HEAD') {
        json(response, 405, failure('METHOD_NOT_ALLOWED', '不支持此请求方法。'));
        return;
      }
      const asset = staticFiles.get(url.pathname);
      let file;
      let contentType;
      if (asset) {
        file = path.join(directory, 'ui', asset[0]);
        contentType = asset[1];
      } else if (/^\/captures\/[0-9a-f-]{36}\.png$/.test(url.pathname)) {
        const captures = path.join(projectRoot, 'reports', 'editor-captures');
        file = path.join(captures, path.basename(url.pathname));
        // A replaced capture directory or file must never expose another directory.
        const resolved = await realpath(file);
        const canonicalRoot = await realpath(projectRoot);
        const relative = path.relative(canonicalRoot, resolved);
        if (relative.startsWith('..') || path.isAbsolute(relative) ||
            !resolved.toLowerCase().startsWith(`${path.resolve(captures).toLowerCase()}${path.sep}`)) {
          json(response, 403, failure('PATH_NOT_ALLOWED', '无法读取此快照。'));
          return;
        }
        contentType = 'image/png';
      } else {
        json(response, 404, failure('NOT_FOUND', '找不到此资源。'));
        return;
      }
      const data = await readFile(file);
      response.writeHead(200, { 'Content-Type': contentType, 'Content-Length': data.length });
      response.end(request.method === 'HEAD' ? undefined : data);
    } catch (error) {
      if (response.destroyed || response.writableEnded) return;
      const missing = error.code === 'ENOENT';
      json(response, missing ? 404 : (error.status ?? 500), failure(
        missing ? 'NOT_FOUND' : (error.status ? error.code : 'EDITOR_FAILURE'),
        missing ? '找不到此资源。' : (error.status ? error.message : '编辑器请求失败，请重试。'),
      ));
    }
  });
  server.requestTimeout = 30_000;
  server.headersTimeout = 10_000;
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => {
      origin = `http://127.0.0.1:${server.address().port}`;
      server.removeListener('error', reject);
      resolve();
    });
  });
  return {
    url: origin,
    server,
    get service() { return editor; },
    projectsRoot: projectManager?.base ?? null,
    activeProject: () => active,
    async resolveProject(folder) {
      if (!projectManager) return engineRoot;
      return (await projectManager.project(folder)).path;
    },
    close() {
      closing ??= (async () => {
        const listenerClosed = new Promise(resolve => { server.close(resolve); server.closeIdleConnections(); });
        const current = editor;
        editor = null;
        const results = await Promise.allSettled([listenerClosed, current?.close()]);
        const failed = results.find(result => result.status === 'rejected');
        if (failed) throw failed.reason;
      })();
      return closing;
    },
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const port = Number(process.env.NATIVEGPU_EDITOR_PORT ?? 4180);
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('Invalid editor port');
  const editor = await startEditorServer({ port });
  process.stdout.write(`NativeEngine Web 编辑器: ${editor.url}\n`);
  let stopped = false;
  const stop = async () => {
    if (stopped) return;
    stopped = true;
    await editor.close();
  };
  process.once('SIGINT', () => { void stop(); });
  process.once('SIGTERM', () => { void stop(); });
}
