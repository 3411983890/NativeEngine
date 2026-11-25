// Project-scoped path resolution for world files.
//
// Every world file lives inside the active project. The rule is not "looks relative" but
// "resolves inside": an absolute path, a `..` segment, or a directory junction that points
// out of the project are all refused, because the world service writes files and a path
// escape there is a write outside the project.

import path from 'node:path';
import fs from 'node:fs/promises';
import { WORLD_ERROR_CODES, WorldError } from './errors.mjs';

export const WORLD_LAYOUT = Object.freeze({
  scene: '.screen/main.scene.json',
  worldDir: '.screen/world',
  recipe: '.screen/world/main.world.json',
  layers: '.screen/world/layers',
  overrides: '.screen/world/overrides',
  libraries: 'assets/world-libraries',
  rules: 'assets/world-rules',
  cooked: 'assets/world-cooked',
  cache: 'build/world-cache',
  reports: 'reports/world-generation',
  outputs: 'outputs',
});

function normalizeSeparators(value) {
  return String(value ?? '').replace(/\\/g, '/');
}

export function isInside(parent, candidate) {
  const relative = path.relative(parent, candidate);
  return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative));
}

/**
 * Resolves `relative` against `projectRoot` and refuses anything that leaves it.
 * `allowMissing` lets a caller name a file it is about to create.
 */
export async function resolveProjectPath(projectRoot, relative, { allowMissing = true, mustBeFile = false } = {}) {
  if (typeof projectRoot !== 'string' || projectRoot.length === 0) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, 'project root is not configured', { stage: 'path' });
  }
  const normalized = normalizeSeparators(relative);
  if (!normalized || normalized.length > 240) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, 'world path is empty or too long', { stage: 'path' });
  }
  if (normalized.startsWith('/') || /^[A-Za-z]:/.test(normalized) || normalized.startsWith('//')) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, `world paths must be project-relative: ${normalized}`, { stage: 'path' });
  }
  if (/[\u0000-\u001f]/.test(normalized)) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, 'world path contains control characters', { stage: 'path' });
  }
  const segments = normalized.split('/');
  if (segments.some(part => part === '..')) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, `world path may not traverse: ${normalized}`, { stage: 'path' });
  }

  const root = path.resolve(projectRoot);
  const target = path.resolve(root, ...segments.filter(part => part !== '' && part !== '.'));
  if (!isInside(root, target)) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, `world path escapes the project: ${normalized}`, { stage: 'path' });
  }

  // A junction inside the project can still point outside it. `realpath` on the nearest
  // existing ancestor is what catches that; the leaf may legitimately not exist yet.
  let existing = target;
  for (;;) {
    try {
      const real = await fs.realpath(existing);
      const realRoot = await fs.realpath(root).catch(() => root);
      if (!isInside(realRoot, real)) {
        throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED,
          `world path resolves outside the project through a link: ${normalized}`, { stage: 'path' });
      }
      break;
    } catch (error) {
      if (error instanceof WorldError) throw error;
      const parent = path.dirname(existing);
      if (parent === existing) break;
      existing = parent;
    }
  }

  if (!allowMissing) {
    const stat = await fs.stat(target).catch(() => null);
    if (!stat) throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, `world file does not exist: ${normalized}`, { stage: 'path' });
    if (mustBeFile && !stat.isFile()) {
      throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, `world path is not a file: ${normalized}`, { stage: 'path' });
    }
  }

  return { absolute: target, relative: segments.filter(part => part !== '' && part !== '.').join('/') };
}

export function recipePath(worldId) {
  return `${WORLD_LAYOUT.worldDir}/${sanitizeId(worldId)}.world.json`;
}

export function layerPath(layerId) {
  return `${WORLD_LAYOUT.layers}/${sanitizeId(layerId)}.json`;
}

export function overridePath(regionId) {
  return `${WORLD_LAYOUT.overrides}/${sanitizeId(regionId)}.json`;
}

export function libraryPath(libraryId) {
  return `${WORLD_LAYOUT.libraries}/${sanitizeId(libraryId)}/library.json`;
}

export function rulesPath(rulesetId) {
  return `${WORLD_LAYOUT.rules}/${sanitizeId(rulesetId)}.json`;
}

export function cookedPath(manifestHash) {
  if (!/^[a-f0-9]{64}$/.test(String(manifestHash ?? ''))) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, 'cooked directory needs a 64-hex manifest hash', { stage: 'path' });
  }
  return `${WORLD_LAYOUT.cooked}/${manifestHash}`;
}

export function cachePath(taskId) {
  return `${WORLD_LAYOUT.cache}/${sanitizeId(taskId)}`;
}

export function reportPath(taskId) {
  return `${WORLD_LAYOUT.reports}/${sanitizeId(taskId)}`;
}

export function sanitizeId(value) {
  const text = String(value ?? '');
  if (!/^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$/.test(text)) {
    throw new WorldError(WORLD_ERROR_CODES.PATH_REJECTED, `'${text.slice(0, 32)}' is not a valid world identifier`, { stage: 'path' });
  }
  return text;
}

// Directories a world operation may create. Everything else is refused so a bug cannot
// start writing `reports/` files from the world service.
export const WORLD_WRITABLE_DIRECTORIES = Object.freeze([
  WORLD_LAYOUT.worldDir,
  WORLD_LAYOUT.layers,
  WORLD_LAYOUT.overrides,
  WORLD_LAYOUT.libraries,
  WORLD_LAYOUT.rules,
  WORLD_LAYOUT.cooked,
  WORLD_LAYOUT.cache,
  WORLD_LAYOUT.reports,
]);

export function isWritableWorldPath(relative) {
  const normalized = normalizeSeparators(relative);
  return WORLD_WRITABLE_DIRECTORIES.some(directory => normalized === directory || normalized.startsWith(`${directory}/`));
}
