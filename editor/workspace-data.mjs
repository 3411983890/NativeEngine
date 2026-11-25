import * as fs from 'node:fs/promises';
import path from 'node:path';
import { lintScene } from '../engine/scene/lint.mjs';
import { sceneResourceRoot } from '../engine/scene/paths.mjs';

const read = async file => JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
async function optional(file, fallback) {
  try { return await read(file); } catch (error) { if (error.code === 'ENOENT') return fallback; throw error; }
}

/** Read authored metadata, never expose arbitrary filesystem reads to the renderer. */
export async function workspaceData(root, scene, scenePath) {
  const characters = [];
  const refs = [...new Set(scene.entities.filter(e => e.character).map(e => e.character.file))];
  if (!refs.length) refs.push('build/animation/mannequin/character.json');
  for (const relative of refs) {
    const file = path.resolve(scenePath && refs[0] !== 'build/animation/mannequin/character.json' ? sceneResourceRoot(scenePath) : root, relative);
    if (!file.startsWith(path.resolve(root) + path.sep)) continue;
    try {
      const data = await read(file);
      characters.push({ file: relative, name: path.basename(path.dirname(file)), bones: data.bones?.length ?? data.skeleton?.bones?.length ?? 0,
        sampleRate: data.sampleRate ?? 60, clips: (data.clips ?? []).map(c => ({ name: c.name, frames: c.frames ?? c.frameCount, loop: c.loop,
          duration: ((c.frames ?? c.frameCount ?? 1) - 1) / (data.sampleRate ?? 60), notifies: c.notifies ?? [] })) });
    } catch (error) { characters.push({ file: relative, error: error.message, clips: [] }); }
  }
  const effects = [];
  try {
    for (const entry of await fs.readdir(path.join(root, 'assets/effects'), { withFileTypes: true })) {
      if (!entry.isFile() || !entry.name.endsWith('.json')) continue;
      const data = await read(path.join(root, 'assets/effects', entry.name));
      effects.push({ file: `assets/effects/${entry.name}`, name: data.name ?? data.id ?? entry.name, ...data });
    }
  } catch (error) { if (error.code !== 'ENOENT') throw error; }
  return { characters, effects, audio: scene.audio ?? { emitters: [] }, diagnostics: lintScene(scene),
    materials: scene.materials ?? {}, settings: scene.settings, lighting: scene.lighting ?? {},
    capabilities: { sceneTransactions: true, nativeScene: process.platform === 'win32', animation: true,
      rotation: false, audioAuthoring: true, nativeVegetation: true, sharedTextures: false } };
}
