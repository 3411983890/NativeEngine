// Rebuild the engine starter assets from the bundled authoring sources.
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { importAsset } from '../../engine/assets/import-service.mjs';
import { binaryHash } from '../offline/animation.mjs';
import { fitCharacterBounds } from '../../engine/assets/character-bounds.mjs';
import { inferFootContacts } from '../../engine/assets/humanoid-rig.mjs';
import { sha256Hex } from '../../engine/assets/canonical.mjs';
import { cookDirectory, encodePng } from '../textures.mjs';

const root = path.resolve(import.meta.dirname, '../..');
const json = async (file, value) => fs.writeFile(file, JSON.stringify(value, null, 2) + '\n');

export async function prepareBuiltinAssets() {
  const source = path.join(root, 'assets/source/builtin');
  const output = path.join(root, 'assets/builtin');
  await fs.mkdir(path.join(root, 'build/builtin'), { recursive: true });
  const staging = await fs.mkdtemp(path.join(root, 'build/builtin/prepare-'));
  try {
    // Keep the authored GLB resolution: the source albedo is 4096² and its
    // normal/ORM maps are 2048². The cook preserves aspect and area-averages
    // when a cap applies, so no map is squashed or point-sampled.
    const imported = await importAsset({ source: path.join(source, 'robot.glb'), output: path.join(staging, 'imports'), size: 'auto' });
    const header = JSON.parse(await fs.readFile(path.join(imported.directory, imported.manifest.character), 'utf8'));
    let binary = await fs.readFile(path.join(imported.directory, 'character/animation.bin'));
    const walk = header.clips.find(c => /walk/i.test(c.name));
    const run = header.clips.find(c => /run/i.test(c.name));
    const jump = header.clips.find(c => /jump/i.test(c.name));
    if (!walk || !run || !jump) throw new Error('Bundled robot needs walk, run and jump clips');
    const stride = header.bones.length * 40;
    // This rig animates translation on Hips underneath a static Armature root.
    // Gameplay owns world movement, so bake that child root in place as well.
    const hips = header.bones.findIndex(bone => /(?:^|:)hips$/i.test(bone.name));
    if (hips < 0) throw new Error('Bundled robot needs its authored Hips joint');
    for (const clip of header.clips) for (let frame = 0; frame < clip.frames; frame++) {
      for (const axis of [0, 2]) binary.writeInt32LE(Math.round(header.bones[hips].rest[axis] * 10000), clip.offset + (frame * header.bones.length + hips) * 40 + axis * 4);
    }
    // Average one walk cycle, aligning quaternion hemispheres, to produce a
    // balanced standing pose with lowered arms rather than a frozen stride.
    const idle = Buffer.alloc(stride);
    for (let bone = 0; bone < header.bones.length; bone++) {
      const sums = Array(10).fill(0);
      const first = Array.from({ length: 4 }, (_, k) => binary.readInt32LE(walk.offset + bone * 40 + (3 + k) * 4));
      const count = walk.frames - 1;
      for (let frame = 0; frame < count; frame++) {
        const at = walk.offset + frame * stride + bone * 40;
        const pose = Array.from({ length: 10 }, (_, k) => binary.readInt32LE(at + k * 4));
        const sign = first.reduce((sum, v, k) => sum + v * pose[3 + k], 0) < 0 ? -1 : 1;
        pose.forEach((v, k) => { sums[k] += v * (k >= 3 && k < 7 ? sign : 1); });
      }
      const qlength = Math.hypot(...sums.slice(3, 7));
      sums.forEach((v, k) => idle.writeInt32LE(Math.round(k >= 3 && k < 7 ? v / qlength * 10000 : v / count), bone * 40 + k * 4));
    }
    const appendPose = (name, pose, frames, contacts) => {
      const offset = binary.length;
      binary = Buffer.concat([binary, ...Array.from({ length: frames }, () => pose)]);
      header.clips.push({ name, frames, offset, loop: true, stepChannels: header.bones.map(() => 0), notifies: [], ...contacts });
    };
    // Synthesized clips declare their own contact windows: the averaged idle
    // plants both feet, the fall pose has no support.
    appendPose('idle', idle, 61, { contacts: [[0, 1], [0, 1]], contactConfidence: 1, contactSource: 'authored' });
    const fallFrame = Math.floor((jump.frames - 1) * .5);
    appendPose('fall', binary.subarray(jump.offset + fallFrame * stride, jump.offset + (fallFrame + 1) * stride), 2, { contacts: [[0, 0], [0, 0]], contactConfidence: 1, contactSource: 'authored' });
    jump.loop = false;
    header.controller = ['idle', walk.name, run.name, jump.name, 'fall'];
    // Baking the hips in place changes the support-phase foot speed measured
    // relative to the root. Refresh it for every clip after the bake.
    for (const clip of header.clips) {
      if (clip.locomotionSpeed === undefined) continue;
      const samples = binary.subarray(clip.offset, clip.offset + clip.frames * header.bones.length * 40);
      const inferred = inferFootContacts({ bones: header.bones, rig: header.rig, binary: samples, frameCount: clip.frames, boneCount: header.bones.length, loop: clip.loop });
      if (inferred && Number.isFinite(inferred.locomotionSpeed)) clip.locomotionSpeed = inferred.locomotionSpeed;
    }
    header.rootMotion = 'extract';
    header.binaryBytes = binary.length;
    header.binaryHash = binaryHash(binary);
    const robot = path.join(output, 'robot');
    await fs.mkdir(robot, { recursive: true });
    await json(path.join(robot, 'character.json'), header);
    await fs.writeFile(path.join(robot, 'animation.bin'), binary);
    const fit = fitCharacterBounds(header, binary, { mode: 'humanoid' });
    const scale = 1.8 / fit.height;
    const fitted = fitCharacterBounds(header, binary, { mode: 'humanoid', scale: [scale, scale, scale] });
    const textures = path.join(output, 'textures');
    await fs.mkdir(textures, { recursive: true });
    const robotTextures = JSON.parse(await fs.readFile(path.join(imported.directory, 'textures/manifest.json'), 'utf8'));
    for (const texture of robotTextures.textures) await fs.copyFile(path.join(imported.directory, 'textures', texture.file), path.join(textures, texture.file));
    const gridSource = path.join(staging, 'grid');
    await fs.mkdir(gridSource);
    // Keep the imagegen original intact; resize it only for runtime mip cooking.
    const image = await sharp(path.join(source, 'grid-white-gray.png')).resize(1024, 1024).ensureAlpha().raw().toBuffer();
    await fs.writeFile(path.join(gridSource, 'engine_grid.albedo.png'), encodePng({ width: 1024, height: 1024, data: image }));
    for (const [semantic, color] of Object.entries({ normal: [128, 128, 255, 255], orm: [255, 255, 0, 255] })) {
      await fs.writeFile(path.join(gridSource, `engine_grid.${semantic}.png`), encodePng({ width: 1, height: 1, data: Buffer.from(color) }));
    }
    const grid = cookDirectory({ source: gridSource, out: path.join(staging, 'grid-cooked'), size: 512 }).manifest;
    for (const texture of grid.textures) await fs.copyFile(path.join(staging, 'grid-cooked', texture.file), path.join(textures, texture.file));
    await json(path.join(textures, 'manifest.json'), { ...robotTextures, textures: [...robotTextures.textures, ...grid.textures] });
    const headerBytes = await fs.readFile(path.join(robot, 'character.json'));
    const manifest = {
      schema: 'nativeengine.builtin/1',
      source: { robot: 'assets/source/builtin/robot.glb', sha256: sha256Hex(await fs.readFile(path.join(source, 'robot.glb'))) },
      character: { file: 'builtin/robot/character.json', sha256: sha256Hex(headerBytes), binarySha256: sha256Hex(binary),
        anchor: 'feet', rootMotion: 'extract', scale: [scale, scale, scale], offset: fitted.offset,
        controller: { stepHeight: .36, orbitDistance: 5, orbitHeight: 1.35, targetHeight: 1.05 } },
      collider: [fitted.radius * 2, fitted.height, fitted.radius * 2],
      materials: { ...imported.manifest.materials, default: { albedo: 'engine_grid', normal: 'engine_grid', orm: 'engine_grid', uvScale: 5, roughness: .88, metalness: 0 } },
      controller: header.controller,
    };
    await json(path.join(output, 'manifest.json'), manifest);
    const scenePath = path.join(root, '.screen/basic.scene.json');
    const scene = JSON.parse(await fs.readFile(scenePath, 'utf8'));
    scene.materials = manifest.materials;
    const player = scene.entities.find(entity => entity.kind === 'player');
    player.character = manifest.character;
    player.scale = manifest.collider;
    await json(scenePath, scene);
    // Other authored scenes share the built-in character. Refresh only the
    // dependency hashes so per-scene rig/controller overrides survive a rebuild.
    for (const file of await fs.readdir(path.join(root, '.screen'))) {
      if (file === 'basic.scene.json' || !file.endsWith('.scene.json')) continue;
      const otherPath = path.join(root, '.screen', file), other = JSON.parse(await fs.readFile(otherPath, 'utf8'));
      let changed = false;
      for (const entity of other.entities ?? []) {
        const character = entity.character;
        if (character?.file !== manifest.character.file) continue;
        if (character.sha256 === manifest.character.sha256 && character.binarySha256 === manifest.character.binarySha256) continue;
        entity.character = { ...character, sha256: manifest.character.sha256, binarySha256: manifest.character.binarySha256 };
        changed = true;
      }
      if (changed) await json(otherPath, other);
    }
    console.log(JSON.stringify({ output, bones: header.bones.length, vertices: header.mesh.vertices, controller: header.controller, collider: manifest.collider }));
    return manifest;
  } finally { await fs.rm(staging, { recursive: true, force: true }); }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  prepareBuiltinAssets().catch(error => { console.error(error); process.exitCode = 1; });
}
