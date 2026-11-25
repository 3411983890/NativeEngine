// File-backed dependency locking for the versioned layered material contract.
// This module does not imply that layered GPU rendering is available.
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { sha256Hex } from '../assets/canonical.mjs';
import { readNgtex } from '../../tools/textures.mjs';
import { compileLayeredMaterial, normalizeLayeredMaterial, LayeredMaterialError } from './layered.mjs';

/** Compile against cooked files in a project texture directory, never caller-supplied hashes. */
export async function compileLayeredMaterialAssets(value, { textureDirectory, shaderFingerprint, uvSets, expectedDependencies } = {}) {
  const material = normalizeLayeredMaterial(value);
  if (typeof textureDirectory !== 'string' || !textureDirectory) throw new LayeredMaterialError('$.dependencies', 'textureDirectory is required');
  let base;
  try { base = await fs.realpath(textureDirectory); }
  catch { throw new LayeredMaterialError('$.dependencies', 'texture directory is unavailable'); }
  const references = new Map();
  for (const layer of material.layers) {
    for (const [semantic, ref] of Object.entries(layer.textures)) references.set(`${semantic}/${ref.name}`, { semantic, name: ref.name });
    if (layer.blend.texture) references.set(`mask/${layer.blend.texture.name}`, { semantic: 'mask', name: layer.blend.texture.name });
  }
  const expected = new Map();
  if (expectedDependencies !== undefined) {
    if (!Array.isArray(expectedDependencies)) throw new LayeredMaterialError('$.dependencies', 'expected dependencies must be an array');
    for (const item of expectedDependencies) {
      if (!item || typeof item.semantic !== 'string' || typeof item.name !== 'string' || !/^[a-f0-9]{64}$/.test(item.sha256 ?? '')) throw new LayeredMaterialError('$.dependencies', 'invalid expected dependency');
      const key = `${item.semantic}/${item.name}`;
      if (expected.has(key)) throw new LayeredMaterialError('$.dependencies', `duplicate dependency ${key}`);
      expected.set(key, item.sha256);
    }
    if (expected.size !== references.size || [...references.keys()].some(key => !expected.has(key))) throw new LayeredMaterialError('$.dependencies', 'dependency list does not match the material');
  }
  const hashes = {};
  for (const [key, ref] of references) {
    const filename = `${ref.name}.${ref.semantic}.ngtex`;
    let resolved, bytes;
    try { resolved = await fs.realpath(path.join(base, filename)); }
    catch { throw new LayeredMaterialError('$.dependencies', `missing texture ${key}`); }
    const relative = path.relative(base, resolved);
    if (!relative || relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) throw new LayeredMaterialError('$.dependencies', `texture ${key} escapes the project texture directory`);
    try { bytes = await fs.readFile(resolved); }
    catch { throw new LayeredMaterialError('$.dependencies', `cannot read texture ${key}`); }
    let image;
    try { image = readNgtex(bytes, key); }
    catch (error) { throw new LayeredMaterialError('$.dependencies', `invalid texture ${key}: ${error.message}`); }
    if (image.semantic !== ref.semantic) throw new LayeredMaterialError('$.dependencies', `texture ${key} has semantic ${image.semantic}`);
    const color = ref.semantic === 'albedo' || ref.semantic === 'emissive';
    if (image.srgb !== color) throw new LayeredMaterialError('$.dependencies', `texture ${key} requires ${color ? 'sRGB' : 'linear'} storage`);
    const completeMipCount=image.width&&image.height?1+Math.floor(Math.log2(Math.max(image.width,image.height))):0;
    if(!completeMipCount||image.mipCount!==completeMipCount||bytes.readUInt32LE(20)+bytes.readUInt32LE(16)!==bytes.length)throw new LayeredMaterialError('$.dependencies', `texture ${key} has invalid dimensions, mip chain or payload`);
    const hash = sha256Hex(bytes);
    if (expectedDependencies !== undefined && expected.get(key) !== hash) throw new LayeredMaterialError('$.dependencies', `texture ${key} content changed`);
    hashes[key] = hash;
  }
  return compileLayeredMaterial(material, { hashes, shaderFingerprint, uvSets });
}
