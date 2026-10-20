import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  SceneError, validateScene, loadScene, queryScene, previewTransaction,
  commitScene, undoSceneFile, buildScene, sceneHash,
} from '../engine/scene/index.mjs';

const DEFAULT_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const MAX_SCENE_BYTES = 16 * 1024 * 1024;
const MAX_SMOKE_FRAMES = 3600;

export class ToolError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'ToolError';
    this.code = code;
    this.details = details;
  }
}

const SCENE_PATH = {
  type: 'string',
  description: 'Scene path relative to the project root. Only assets/ is writable through this tool.',
};
const TRANSACTION = {
  type: 'object',
  description: 'Atomic scene transaction: {projectId, expectedRevision, commands[]}',
  additionalProperties: false,
  required: ['projectId', 'expectedRevision', 'commands'],
  properties: {
    projectId: { type: 'string' },
    expectedRevision: { type: 'integer', minimum: 0 },
    commands: { type: 'array', minItems: 1, maxItems: 1000, items: { type: 'object' } },
  },
};

const TOOLS = Object.freeze([
  {
    name: 'scene_validate',
    description: 'Validate a scene JSON file without modifying it. Returns structured errors instead of writing.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath'],
      properties: { scenePath: SCENE_PATH },
    },
  },
  {
    name: 'scene_query',
    description: 'Read entities from a validated scene. Never modifies the project.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath'],
      properties: {
        scenePath: SCENE_PATH,
        kind: { enum: ['ground', 'wall', 'prop', 'player', 'key', 'door', 'goal', 'light'] },
        name: { type: 'string' },
        entityId: { type: 'string' },
      },
    },
  },
  {
    name: 'scene_preview',
    description: 'Apply a transaction to an in-memory copy and return the diff. Does not write the scene.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath', 'transaction'],
      properties: {
        scenePath: SCENE_PATH,
        transaction: TRANSACTION,
        includeScene: { type: 'boolean', default: false },
      },
    },
  },
  {
    name: 'scene_apply',
    description: 'Atomically commit a transaction. The running editor hot-reloads the new revision.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath', 'transaction'],
      properties: { scenePath: SCENE_PATH, transaction: TRANSACTION },
    },
  },
  {
    name: 'scene_undo',
    description: 'Undo the last committed transaction. Revision still increases to reject stale requests.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['scenePath', 'projectId', 'expectedRevision'],
      properties: {
        scenePath: SCENE_PATH,
        projectId: { type: 'string' },
        expectedRevision: { type: 'integer', minimum: 0 },
      },
    },
  },
  {
    name: 'scene_build',
    description: 'Build a deterministic content-addressed scene package under outputs/.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['scenePath', 'outputDirectory'],
      properties: {
        scenePath: SCENE_PATH,
        outputDirectory: { type: 'string', description: 'Relative directory under outputs/.' },
        target: { const: 'windows-x64', default: 'windows-x64' },
      },
    },
  },
  {
    name: 'runtime_smoke',
    description: 'Run the native engine for a bounded smoke test and return its JSON report. Requires build/NativeGPU.exe.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        scenePath: { ...SCENE_PATH, default: 'assets/courtyard.scene.json' },
        frames: { type: 'integer', minimum: 240, maximum: MAX_SMOKE_FRAMES, default: 360 },
      },
    },
  },
  {
    name: 'engine_probe',
    description: 'Run the native S1 capability probe and return the material table, scene lighting, DXR and Mesh Shader evidence.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        scenePath: { ...SCENE_PATH, default: 'assets/courtyard.scene.json' },
        frames: { type: 'integer', minimum: 240, maximum: MAX_SMOKE_FRAMES, default: 240 },
      },
    },
  },
]);

export function listTools() {
  return TOOLS.map(tool => structuredClone(tool));
}

function asObject(value, label) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new ToolError('INVALID_ARGUMENTS', `${label} must be an object`);
  }
  return value;
}

function asString(value, label) {
  if (typeof value !== 'string' || value.trim() === '' || value.includes('\0')) {
    throw new ToolError('INVALID_ARGUMENTS', `${label} must be a non-empty string`);
  }
  return value;
}

async function realRoot(root) {
  try {
    return await fs.realpath(root);
  } catch (error) {
    if (error.code === 'ENOENT') return path.resolve(root);
    throw error;
  }
}

function inside(parent, child) {
  const relative = path.relative(parent, child);
  return relative !== '' && !relative.startsWith(`..${path.sep}`) && relative !== '..'
    && !path.isAbsolute(relative);
}

async function resolveProjectPath(root, input, { roots, label, mustExist = true }) {
  const rootPath = path.resolve(root);
  const absolute = path.resolve(rootPath, asString(input, label));
  const rootReal = await realRoot(rootPath);
  const relative = path.relative(rootReal, absolute);
  if (relative === '' || relative.startsWith(`..${path.sep}`) || relative === '..'
      || path.isAbsolute(relative)) {
    throw new ToolError('INVALID_PATH', `${label} escapes the project root`);
  }
  const top = relative.split(path.sep)[0].toLowerCase();
  if (!roots.includes(top)) {
    throw new ToolError('INVALID_PATH', `${label} must be under ${roots.join('/')}`);
  }
  if (!mustExist) {
    // Check the nearest existing ancestor so a symlinked parent cannot escape.
    let ancestor = absolute;
    while (true) {
      try {
        const real = await fs.realpath(ancestor);
        if (real !== rootReal && !inside(rootReal, real)) {
          throw new ToolError('INVALID_PATH', `${label} resolves outside the project root`);
        }
        break;
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        const parent = path.dirname(ancestor);
        if (parent === ancestor) break;
        ancestor = parent;
      }
    }
    return absolute;
  }
  let real;
  try {
    real = await fs.realpath(absolute);
  } catch (error) {
    if (error.code === 'ENOENT') throw new ToolError('FILE_NOT_FOUND', `${label} does not exist`);
    throw error;
  }
  if (real !== rootReal && !inside(rootReal, real)) {
    throw new ToolError('INVALID_PATH', `${label} resolves outside the project root`);
  }
  return real;
}

async function readSceneJSON(file) {
  const stat = await fs.stat(file);
  if (stat.size > MAX_SCENE_BYTES) throw new ToolError('FILE_TOO_LARGE', `Scene exceeds ${MAX_SCENE_BYTES} bytes`);
  let value;
  try {
    value = JSON.parse(await fs.readFile(file, 'utf8'));
  } catch (error) {
    if (error instanceof SyntaxError) throw new ToolError('INVALID_JSON', 'Scene is not valid JSON');
    throw error;
  }
  return value;
}

function scenePath(root, input) {
  return resolveProjectPath(root, input, { roots: ['assets'], label: 'scenePath' });
}

export function createToolService({ root = process.env.NATIVEGPU_PROJECT_ROOT || DEFAULT_ROOT, executable } = {}) {
  const projectRoot = path.resolve(root);
  const explicitExecutable = executable ? path.resolve(executable) : null;

  async function resolveExecutable() {
    const candidates = explicitExecutable
      ? [explicitExecutable]
      : [path.join(projectRoot, 'build', 'NativeGPU.exe'), path.join(projectRoot, 'NativeGPU.exe')];
    for (const candidate of candidates) {
      try {
        await fs.access(candidate);
        return candidate;
      } catch { /* Try the packaged layout next. */ }
    }
    throw new ToolError(
      'NATIVE_BINARY_MISSING',
      'NativeGPU.exe is missing; build it with tools/build.ps1 or use a complete package',
    );
  }

  async function runNative(mode, scenePathValue, frames) {
    const file = await scenePath(projectRoot, scenePathValue);
    const nativeExecutable = await resolveExecutable();
    const reportDirectory = await resolveProjectPath(projectRoot, 'reports', {
      roots: ['reports'], label: 'reports', mustExist: false,
    });
    await fs.mkdir(reportDirectory, { recursive: true });
    const reportName = `mcp-${mode}-${randomUUID()}.json`;
    const reportPath = path.join(reportDirectory, reportName);
    const started = Date.now();
    const modeArgument = mode === 'probe' ? '--probe' : '--smoke';
    const child = spawn(nativeExecutable, [
      modeArgument, '--frames', String(frames), '--scene', file, '--report', reportPath,
      '--no-watch',
    ], { cwd: projectRoot, stdio: 'ignore', windowsHide: true });
    const exitCode = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill();
        reject(new ToolError('NATIVE_TIMEOUT', `Native ${mode} run exceeded 180 seconds`));
      }, 180_000);
      child.once('error', error => {
        clearTimeout(timer);
        reject(new ToolError('NATIVE_START_FAILED', error.message));
      });
      child.once('exit', code => {
        clearTimeout(timer);
        resolve(code);
      });
    });
    let report;
    try {
      report = JSON.parse(await fs.readFile(reportPath, 'utf8'));
    } catch {
      throw new ToolError('NATIVE_REPORT_MISSING', `Native process exited ${exitCode} without a readable report`);
    }
    return {
      exitCode,
      success: exitCode === 0 && report.success === true,
      elapsedMs: Date.now() - started,
      reportPath: `reports/${reportName}`,
      report,
    };
  }

  async function dispatch(name, rawArguments) {
    const args = asObject(rawArguments ?? {}, 'arguments');
    if (name === 'scene_validate') {
      const file = await scenePath(projectRoot, args.scenePath);
      const scene = await readSceneJSON(file);
      const validation = validateScene(scene);
      return {
        valid: validation.valid,
        errors: validation.errors,
        projectId: validation.valid ? scene.projectId : null,
        revision: validation.valid ? scene.revision : null,
        entityCount: validation.valid ? scene.entities.length : null,
        schema: 'assets/scene.schema.json',
      };
    }
    if (name === 'scene_query') {
      const file = await scenePath(projectRoot, args.scenePath);
      const query = {};
      if (args.kind !== undefined) query.kind = args.kind;
      if (args.name !== undefined) query.name = args.name;
      if (args.entityId !== undefined) query.entityId = args.entityId;
      return queryScene(await loadScene(file), query);
    }
    if (name === 'scene_preview' || name === 'scene_apply') {
      const file = await scenePath(projectRoot, args.scenePath);
      if (name === 'scene_preview') {
        const preview = previewTransaction(await loadScene(file), args.transaction);
        const output = {
          projectId: preview.projectId,
          baseRevision: preview.baseRevision,
          revision: preview.revision,
          changes: preview.changes,
        };
        if (args.includeScene === true) output.scene = preview.scene;
        return output;
      }
      const applied = await commitScene(file, args.transaction);
      return {
        projectId: applied.projectId,
        baseRevision: applied.baseRevision,
        revision: applied.revision,
        changes: applied.changes,
        sceneHash: sceneHash(applied.scene),
        hotReload: 'The running native editor reloads assets/*.scene.json automatically.',
      };
    }
    if (name === 'scene_undo') {
      const file = await scenePath(projectRoot, args.scenePath);
      const undone = await undoSceneFile(file, {
        projectId: asString(args.projectId, 'projectId'),
        expectedRevision: args.expectedRevision,
      });
      return { revision: undone.revision, sceneHash: sceneHash(undone.scene) };
    }
    if (name === 'scene_build') {
      const file = await scenePath(projectRoot, args.scenePath);
      const outputDirectory = await resolveProjectPath(projectRoot, args.outputDirectory, {
        roots: ['outputs'], label: 'outputDirectory', mustExist: false,
      });
      const built = await buildScene(file, outputDirectory, { target: args.target ?? 'windows-x64' });
      return {
        packageDirectory: path.relative(projectRoot, built.packageDirectory).split(path.sep).join('/'),
        cacheHit: built.cacheHit,
        manifest: built.manifest,
      };
    }
    if (name === 'runtime_smoke' || name === 'engine_probe') {
      const mode = name === 'engine_probe' ? 'probe' : 'smoke';
      const frames = args.frames ?? (mode === 'probe' ? 240 : 360);
      if (!Number.isInteger(frames) || frames < 240 || frames > MAX_SMOKE_FRAMES) {
        throw new ToolError('INVALID_ARGUMENTS', `frames must be an integer between 240 and ${MAX_SMOKE_FRAMES}`);
      }
      return runNative(mode, args.scenePath ?? 'assets/courtyard.scene.json', frames);
    }
    throw new ToolError('UNKNOWN_TOOL', `Unknown tool: ${name}`);
  }

  return {
    root: projectRoot,
    listTools,
    async callTool(name, args) {
      try {
        if (typeof name !== 'string' || !TOOLS.some(tool => tool.name === name)) {
          throw new ToolError('UNKNOWN_TOOL', `Unknown tool: ${name}`);
        }
        return { ok: true, result: await dispatch(name, args) };
      } catch (error) {
        if (error instanceof SceneError || error instanceof ToolError) {
          return {
            ok: false,
            error: { code: error.code, message: error.message, details: error.details ?? [] },
          };
        }
        return {
          ok: false,
          error: { code: 'TOOL_FAILURE', message: error.message ?? String(error), details: [] },
        };
      }
    },
  };
}
