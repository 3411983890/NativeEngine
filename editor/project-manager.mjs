import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

export const PROJECT_SCHEMA = 'nativeengine.project/1';
export const PROJECTS_FOLDER = 'nativeEngineProject';
const STATE_FILE = '.editor-state.json';
const MANIFEST_FILE = 'nativeengine.project.json';
const RESERVED = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$/i;

export function defaultProjectsRoot(documents = path.join(os.homedir(), 'Documents')) {
  return path.join(documents, PROJECTS_FOLDER);
}

export class ProjectError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'ProjectError';
    this.code = code;
  }
}

function checkName(value) {
  const name = String(value ?? '').normalize('NFC').trim();
  if (!name || name.length > 80) throw new ProjectError('INVALID_PROJECT_NAME', '项目名称长度必须为 1–80 个字符。');
  if (/[<>:"/\\|?*\x00-\x1f]/.test(name) || /[. ]$/.test(name) || name === '.' || name === '..' || RESERVED.test(name)) {
    throw new ProjectError('INVALID_PROJECT_NAME', '项目名称包含 Windows 不允许的字符或保留名称。');
  }
  return name;
}

function inside(parent, child) {
  const relative = path.relative(parent, child);
  return relative !== '' && relative !== '..' && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative);
}

async function writeJSON(file, value) {
  const temporary = `${file}.tmp`;
  await fs.writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`, 'utf8');
  await fs.rename(temporary, file);
}

async function readJSON(file) {
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

async function blankScene(engine, name) {
  const scene = await readJSON(path.join(engine, '.screen/basic.scene.json'));
  scene.projectId = randomUUID();
  scene.name = name;
  scene.revision = 0;
  for (const entity of scene.entities) entity.id = randomUUID();
  return scene;
}

export function createProjectManager({ engineRoot, projectsRoot, version = 'unknown' } = {}) {
  const engine = path.resolve(engineRoot);
  const base = path.resolve(projectsRoot ?? defaultProjectsRoot());
  const statePath = path.join(base, STATE_FILE);

  async function ensureBase() {
    await fs.mkdir(base, { recursive: true });
  }

  async function readManifest(folder) {
    const directory = path.join(base, folder);
    const manifestPath = path.join(directory, MANIFEST_FILE);
    const manifest = await readJSON(manifestPath);
    if (manifest.schema !== PROJECT_SCHEMA || manifest.name !== folder || !manifest.scene) {
      throw new ProjectError('INVALID_PROJECT', `项目清单无效：${folder}`);
    }
    return { ...manifest, folder, path: directory, manifestPath };
  }

  async function project(folder) {
    if (typeof folder !== 'string' || !folder || path.basename(folder) !== folder || folder === '.' || folder === '..') {
      throw new ProjectError('INVALID_PROJECT_PATH', '项目标识无效。');
    }
    await ensureBase();
    const directory = path.join(base, folder);
    let real;
    try { real = await fs.realpath(directory); }
    catch (error) {
      if (error.code === 'ENOENT') throw new ProjectError('PROJECT_NOT_FOUND', '项目不存在。');
      throw error;
    }
    const baseReal = await fs.realpath(base);
    if (!inside(baseReal, real)) throw new ProjectError('INVALID_PROJECT_PATH', '项目目录必须位于 nativeEngineProject 内。');
    const manifest = await readManifest(folder);
    return { ...manifest, path: real };
  }

  async function list() {
    await ensureBase();
    const entries = await fs.readdir(base, { withFileTypes: true });
    const projects = [];
    for (const entry of entries) {
      if (!entry.isDirectory() || entry.name.startsWith('.')) continue;
      try {
        const item = await project(entry.name);
        const stat = await fs.stat(item.path);
        projects.push({
          folder: item.folder, name: item.name, path: item.path, scene: item.scene,
          template: item.template, pipeline: item.pipeline, createdAt: item.createdAt,
          modifiedAt: stat.mtime.toISOString(),
        });
      } catch { /* Ignore folders that are not NativeEngine projects. */ }
    }
    projects.sort((a, b) => String(b.modifiedAt).localeCompare(String(a.modifiedAt)));
    return projects;
  }

  async function state() {
    try { return await readJSON(statePath); } catch (error) { if (error.code !== 'ENOENT') throw error; return {}; }
  }

  async function remember(folder) {
    await ensureBase();
    await writeJSON(statePath, { activeFolder: folder, updatedAt: new Date().toISOString() });
  }

  async function latest() {
    const saved = await state();
    if (saved.activeFolder) {
      try { return await project(saved.activeFolder); } catch { /* Fall through to newest valid project. */ }
    }
    const projects = await list();
    if (!projects.length) return null;
    return project(projects[0].folder);
  }

  async function copyBaseAssets(destination) {
    const assets = path.join(destination, 'assets');
    await fs.mkdir(assets, { recursive: true });
    for (const source of ['gameplay', 'scripts/gameplay']) {
      const from = path.join(engine, 'assets', source);
      try { await fs.cp(from, path.join(assets, source), { recursive: true, errorOnExist: false }); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    try { await fs.copyFile(path.join(engine, 'assets', 'scene.schema.json'), path.join(assets, 'scene.schema.json')); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    await fs.mkdir(path.join(assets, 'textures'), { recursive: true });
    // Starter resources are project-owned: runtime never reads the Downloads
    // folder or writes imported assets back into the engine installation.
    await fs.cp(path.join(engine, 'assets/builtin'), path.join(assets, 'builtin'), { recursive: true });
    await fs.cp(path.join(engine, 'assets/builtin/textures'), path.join(assets, 'textures'), { recursive: true });
    for (const directory of ['imported', 'effects', 'fields', 'audio', 'vegetation']) {
      await fs.mkdir(path.join(assets, directory), { recursive: true });
    }
  }

  async function create({ name, template = 'blank', pipeline = 'standard' } = {}) {
    const projectName = checkName(name);
    if (template !== 'blank') throw new ProjectError('UNSUPPORTED_TEMPLATE', '当前提供第三人称基础场景模板。');
    if (pipeline !== 'standard') throw new ProjectError('UNSUPPORTED_PIPELINE', '当前仅支持标准渲染管线。');
    await ensureBase();
    const target = path.join(base, projectName);
    try { await fs.access(target); throw new ProjectError('PROJECT_EXISTS', '同名项目已存在。'); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    const temporary = path.join(base, `.creating-${randomUUID()}`);
    await fs.mkdir(temporary, { recursive: false });
    try {
      const createdAt = new Date().toISOString();
      const manifest = {
        schema: PROJECT_SCHEMA,
        id: randomUUID(),
        name: projectName,
        createdAt,
        updatedAt: createdAt,
        engineVersion: version,
        template: 'blank',
        pipeline: 'standard',
        scene: '.screen/main.scene.json',
      };
      await copyBaseAssets(temporary);
      await fs.mkdir(path.join(temporary, '.screen'), { recursive: true });
      await fs.mkdir(path.join(temporary, 'reports'), { recursive: true });
      await fs.mkdir(path.join(temporary, 'build'), { recursive: true });
      await fs.mkdir(path.join(temporary, 'outputs'), { recursive: true });
      await fs.mkdir(path.join(temporary, 'saves'), { recursive: true });
      const scene = await blankScene(engine, projectName);
      await writeJSON(path.join(temporary, '.screen', 'main.scene.json'), scene);
      await writeJSON(path.join(temporary, MANIFEST_FILE), manifest);
      await fs.writeFile(path.join(temporary, 'VERSION'), `${version}\n`, 'utf8');
      await fs.rename(temporary, target);
      await remember(projectName);
      return { ...manifest, folder: projectName, path: target, scene: manifest.scene };
    } catch (error) {
      await fs.rm(temporary, { recursive: true, force: true }).catch(() => {});
      throw error;
    }
  }

  async function open(folder) {
    const item = await project(folder);
    await remember(item.folder);
    return item;
  }

  return { base, engineRoot: engine, list, create, open, latest, project, state };
}
