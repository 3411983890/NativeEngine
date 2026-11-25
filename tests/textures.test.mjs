import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  analyze,
  compressMips,
  cookDirectory,
  decodePng,
  encodePng,
  makeMips,
  NGTEX_FORMAT_BC7,
  readNgtex,
  resizeRgba,
  textureName,
  writeNgtex,
} from '../tools/textures.mjs';

function gradient(width, height) {
  const data = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 4;
      data[offset] = x * 255 / (width - 1);
      data[offset + 1] = y * 255 / (height - 1);
      data[offset + 2] = 128;
      data[offset + 3] = 255;
    }
  }
  return { width, height, data };
}

test('colour resizing and mips filter in linear light; data and alpha stay linear', () => {
  const image = { width: 2, height: 2, data: Buffer.from([
    0,0,0,0, 255,255,255,255, 255,255,255,255, 0,0,0,0,
  ]) };
  for (const semantic of ['albedo', 'emissive']) {
    assert.deepEqual([...makeMips(image, semantic)[1].data], [188,188,188,128]);
    assert.deepEqual([...resizeRgba(image, 1, 1, semantic).data], [188,188,188,128]);
  }
  assert.deepEqual([...makeMips(image, 'orm')[1].data], [128,128,128,128]);
});

test('changing one texture only recooks that texture, including budget candidates', () => {
  const directory=mkdtempSync(join(tmpdir(),'nativegpu-incremental-'));
  try {
    const source=join(directory,'source'), out=join(directory,'out');mkdirSync(source);
    writeFileSync(join(source,'a.albedo.png'),encodePng(gradient(8,8)));
    writeFileSync(join(source,'b.albedo.png'),encodePng(gradient(8,8)));
    cookDirectory({source,out,size:8});
    const changed=gradient(8,8);changed.data[0]=240;
    writeFileSync(join(source,'a.albedo.png'),encodePng(changed));
    const result=cookDirectory({source,out,size:8});
    assert.equal(result.cookedTextures,1);
    assert.equal(result.reusedTextures,1);
    const fresh=cookDirectory({source,out:join(directory,'fresh'),size:8,incremental:false});
    assert.deepEqual(result.manifest,fresh.manifest);
  } finally {rmSync(directory,{recursive:true,force:true});}
});

test('every RGB colour format preserves sRGB and BC5 colour is rejected', () => {
  const directory = mkdtempSync(join(tmpdir(), 'nativegpu-colour-'));
  try {
    const source = join(directory, 'source'); mkdirSync(source);
    writeFileSync(join(source, 'test.albedo.png'), encodePng(gradient(4,4)));
    for (const format of ['auto','rgba8','bc1','bc3','bc7']) {
      const { manifest } = cookDirectory({ source, out: join(directory,format), size: 4, format });
      assert.equal(manifest.textures[0].srgb, true, format);
      assert.equal(readNgtex(readFileSync(join(directory,format,'test.albedo.ngtex'))).srgb, true);
    }
    assert.throws(() => cookDirectory({ source, out: join(directory,'bc5'), size:4, format:'bc5' }), /BC5/);
  } finally { rmSync(directory, { recursive:true, force:true }); }
});

test('PNG encode/decode round-trips RGBA', () => {
  const image = gradient(16, 8);
  const png = encodePng(image);
  const decoded = decodePng(png, 'roundtrip');
  assert.equal(decoded.width, 16);
  assert.equal(decoded.height, 8);
  assert.deepEqual(decoded.data, image.data);
});

test('mip chain halves to 1x1 and normal mips stay normalized', () => {
  const image = gradient(8, 8);
  const mips = makeMips(image, 'albedo');
  assert.deepEqual(mips.map((mip) => `${mip.width}x${mip.height}`), [
    '8x8', '4x4', '2x2', '1x1',
  ]);
  const normal = { width: 2, height: 2, data: Buffer.from([
    128, 128, 255, 255, 128, 128, 255, 255,
    128, 128, 255, 255, 128, 128, 255, 255,
  ]) };
  const normalMips = makeMips(normal, 'normal');
  const last = normalMips.at(-1);
  assert.equal(last.width, 1);
  assert.equal(last.data[0], 128);
  assert.equal(last.data[2], 255);
});

test('ngtex container validates and rejects corruption', () => {
  const image = gradient(4, 4);
  const container = writeNgtex({ mips: makeMips(image, 'albedo'), semantic: 'albedo' });
  const parsed = readNgtex(container, 'test');
  assert.equal(parsed.width, 4);
  assert.equal(parsed.height, 4);
  assert.equal(parsed.mipCount, 3);
  assert.equal(parsed.semantic, 'albedo');
  const broken = Buffer.from(container);
  broken[0] = 0;
  assert.throws(() => readNgtex(broken, 'broken'), /bad magic/);
});

test('texture names split semantic suffixes', () => {
  assert.deepEqual(textureName('brick.albedo.png'), { name: 'brick', semantic: 'albedo' });
  assert.deepEqual(textureName('brick.normal.png'), { name: 'brick', semantic: 'normal' });
  assert.deepEqual(textureName('brick.png'), { name: 'brick', semantic: 'albedo' });
});

test('cook emits deterministic containers and a manifest', () => {
  const directory = mkdtempSync(join(tmpdir(), 'nativegpu-textures-'));
  try {
    const source = join(directory, 'source');
    const out = join(directory, 'out');
    mkdirSync(source, { recursive: true });
    writeFileSync(join(source, 'sand.albedo.png'), encodePng(gradient(16, 16)));
    writeFileSync(join(source, 'sand.orm.png'), encodePng(gradient(16, 16)));
    const first = cookDirectory({ source, out, size: 8 });
    assert.equal(first.textures.length, 2);
    const second = cookDirectory({ source, out, size: 8 });
    assert.deepEqual(first.manifest, second.manifest);
    assert.equal(first.textures[0].mips, 4);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('budget cook downgrades rungs and stays under the limit', () => {
  const directory = mkdtempSync(join(tmpdir(), 'nativegpu-budget-'));
  try {
    const source = join(directory, 'source');
    const out = join(directory, 'out');
    mkdirSync(source, { recursive: true });
    for (const name of ['a', 'b', 'c']) {
      writeFileSync(join(source, `${name}.albedo.png`), encodePng(gradient(128, 128)));
      writeFileSync(join(source, `${name}.normal.png`), encodePng(gradient(128, 128)));
    }
    const full = cookDirectory({ source, out, size: 128 });
    const budget = Math.floor(full.manifest.totalBytes * 0.7);
    const result = cookDirectory({ source, out, size: 128, budgetBytes: budget });
    assert.equal(result.manifest.overBudget, false);
    assert.ok(result.manifest.totalBytes <= budget);
    assert.ok(result.manifest.decisions.length > 0);
    assert.ok(result.textures.some((entry) => entry.width < 128));
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('analyze reports normal vector length', () => {
  const image = { width: 1, height: 1, data: Buffer.from([128, 128, 255, 255]) };
  const stats = analyze(image, 'normal');
  assert.ok(stats.normalMeanLength > 0.99 && stats.normalMeanLength <= 1.01);
});

test('resize preserves dimensions and alpha', () => {
  const image = gradient(13, 7);
  const resized = resizeRgba(image, 6, 4);
  assert.equal(resized.width, 6);
  assert.equal(resized.height, 4);
  assert.equal(resized.data.length, 6 * 4 * 4);
  assert.equal(resized.data[3], 255);
});

test('BC7 container round-trips and reports the format', () => {
  const image = gradient(8, 8);
  const encoded = writeNgtex({
    mips: compressMips(makeMips(image, 'albedo'), NGTEX_FORMAT_BC7),
    semantic: 'albedo',
    format: NGTEX_FORMAT_BC7,
    srgb: true,
  });
  const parsed = readNgtex(encoded);
  assert.equal(parsed.format, NGTEX_FORMAT_BC7);
  assert.equal(parsed.srgb, true);
  assert.equal(parsed.mips[0].data.length, 2 * 2 * 16);
});

test('cook cache reuses unchanged sources and invalidates on content change', () => {
  const directory = mkdtempSync(join(tmpdir(), 'nativegpu-cache-'));
  const source = join(directory, 'source');
  const out = join(directory, 'out');
  mkdirSync(source);
  try {
    writeFileSync(join(source, 'one.albedo.png'), encodePng(gradient(32, 32)));
    const first = cookDirectory({ source, out, size: 32, format: 'rgba8' });
    assert.equal(first.cacheHit, false);
    const second = cookDirectory({ source, out, size: 32, format: 'rgba8' });
    assert.equal(second.cacheHit, true);
    assert.equal(second.manifest.totalBytes, first.manifest.totalBytes);
    const changed = gradient(32, 32);
    changed.data.fill(200);
    writeFileSync(join(source, 'one.albedo.png'), encodePng(changed));
    const third = cookDirectory({ source, out, size: 32, format: 'rgba8' });
    assert.equal(third.cacheHit, false);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('container hash is stable across repeated cooks', () => {
  const image = gradient(8, 8);
  const a = writeNgtex({ mips: makeMips(image, 'orm'), semantic: 'orm' });
  const b = writeNgtex({ mips: makeMips(image, 'orm'), semantic: 'orm' });
  assert.equal(
    createHash('sha256').update(a).digest('hex'),
    createHash('sha256').update(b).digest('hex'),
  );
});
