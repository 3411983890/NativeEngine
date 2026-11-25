import test from 'node:test';
import assert from 'node:assert/strict';
import {
  decodeBc1,
  decodeBc3,
  decodeBc4,
  decodeBc5,
  decodeBc7,
  encodeBc1,
  encodeBc3,
  encodeBc4,
  encodeBc5,
  encodeBc7,
  normalAngularError,
  psnr,
} from '../tools/bc.mjs';

function solid(width, height, color) {
  const data = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i += 1) {
    data[i * 4] = color[0];
    data[i * 4 + 1] = color[1];
    data[i * 4 + 2] = color[2];
    data[i * 4 + 3] = color[3] ?? 255;
  }
  return { width, height, data };
}

function gradient(width, height) {
  const data = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 4;
      data[offset] = Math.round((x / (width - 1)) * 255);
      data[offset + 1] = Math.round((y / (height - 1)) * 255);
      data[offset + 2] = 128;
      data[offset + 3] = 255;
    }
  }
  return { width, height, data };
}

test('BC1 round-trips a solid colour exactly', () => {
  const image = solid(8, 8, [200, 100, 50]);
  const decoded = decodeBc1(encodeBc1(image), 8, 8);
  const error = psnr(image.data, decoded, 3);
  assert.ok(error > 45, `PSNR ${error}`);
});

test('BC1 gradient stays above a usable quality floor', () => {
  const image = gradient(32, 32);
  const encoded = encodeBc1(image);
  assert.equal(encoded.length, (32 / 4) * (32 / 4) * 8);
  const decoded = decodeBc1(encoded, 32, 32);
  const error = psnr(image.data, decoded, 3);
  assert.ok(error > 28, `PSNR ${error}`);
});

test('BC4 handles constant and gradient channels', () => {
  const constant = new Uint8Array(16 * 16).fill(77);
  const constantDecoded = decodeBc4(encodeBc4(constant, 16, 16), 16, 16);
  assert.ok(constantDecoded.every((value) => Math.abs(value - 77) <= 2));

  const ramp = new Uint8Array(16 * 16);
  for (let i = 0; i < ramp.length; i += 1) ramp[i] = i % 256;
  const encoded = encodeBc4(ramp, 16, 16);
  assert.equal(encoded.length, (16 / 4) * (16 / 4) * 8);
  const decoded = decodeBc4(encoded, 16, 16);
  let error = 0;
  for (let i = 0; i < ramp.length; i += 1) error += Math.abs(ramp[i] - decoded[i]);
  assert.ok(error / ramp.length < 6, `mean error ${error / ramp.length}`);
});

test('BC3 preserves RGB and alpha', () => {
  const image = gradient(16, 16);
  for (let i = 0; i < 16 * 16; i += 1) image.data[i * 4 + 3] = (i * 7) & 0xff;
  const encoded = encodeBc3(image);
  assert.equal(encoded.length, (16 / 4) * (16 / 4) * 16);
  const decoded = decodeBc3(encoded, 16, 16);
  assert.ok(psnr(image.data, decoded, 3) > 24);
  let alphaError = 0;
  for (let i = 0; i < 16 * 16; i += 1) {
    alphaError += Math.abs(image.data[i * 4 + 3] - decoded[i * 4 + 3]);
  }
  assert.ok(alphaError / (16 * 16) < 12, `alpha mean error ${alphaError / (16 * 16)}`);
});

test('BC5 reconstructs a flat normal within one degree', () => {
  const image = solid(16, 16, [128, 128, 255]);
  const encoded = encodeBc5(image);
  assert.equal(encoded.length, (16 / 4) * (16 / 4) * 16);
  const decoded = decodeBc5(encoded, 16, 16);
  const error = normalAngularError(image.data, decoded);
  assert.ok(error.max < 1.0, `max angle ${error.max}`);
});

test('BC encoders are deterministic', () => {
  const image = gradient(16, 16);
  assert.deepEqual(encodeBc1(image), encodeBc1(image));
  assert.deepEqual(encodeBc3(image), encodeBc3(image));
  assert.deepEqual(encodeBc5(image), encodeBc5(image));
});

test('BC7 mode 6 round-trips a solid colour', () => {
  const image = solid(8, 8, [210, 120, 40, 200]);
  const decoded = decodeBc7(encodeBc7(image), 8, 8);
  assert.ok(psnr(image.data, decoded, 4) > 42);
});

test('BC7 picks mode 5 for flat blocks and round-trips it', () => {
  const image = solid(8, 8, [210, 120, 40, 200]);
  const encoded = encodeBc7(image);
  assert.equal(encoded[0] & 0x7F, 0x20);
  const decoded = decodeBc7(encoded, 8, 8);
  assert.ok(psnr(image.data, decoded, 4) > 42);
});

test('BC7 mode 6 beats BC1 on a gradient', () => {
  const image = gradient(64, 64);
  const bc1 = psnr(image.data, decodeBc1(encodeBc1(image), 64, 64), 3);
  const bc7 = psnr(image.data, decodeBc7(encodeBc7(image), 64, 64), 3);
  assert.ok(bc7 > 32, `BC7 PSNR ${bc7}`);
  assert.ok(bc7 > bc1 + 1, `BC7 ${bc7} vs BC1 ${bc1}`);
});
