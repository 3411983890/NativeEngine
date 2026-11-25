// Minimal BC1/BC3/BC4/BC5 block codecs used by the texture cook.
//
// The encoders are deliberately simple (min/max endpoints, nearest palette index) rather
// than rate-distortion optimal. They exist so the cook can produce deterministic,
// measureable compressed candidates and the runtime can be tested against a software
// decoder; BC7 quality remains a later upgrade.

const BLOCK = 4;

function clampByte(value) {
  return value < 0 ? 0 : value > 255 ? 255 : value;
}

function pack565(r, g, b) {
  const r5 = Math.round((r * 31) / 255);
  const g6 = Math.round((g * 63) / 255);
  const b5 = Math.round((b * 31) / 255);
  return ((r5 << 11) | (g6 << 5) | b5) >>> 0;
}

function unpack565(value) {
  const r5 = (value >> 11) & 31;
  const g6 = (value >> 5) & 63;
  const b5 = value & 31;
  return [
    (r5 << 3) | (r5 >> 2),
    (g6 << 2) | (g6 >> 4),
    (b5 << 3) | (b5 >> 2),
  ];
}

function mix(a, b, wa, wb, divisor) {
  return [
    Math.round((a[0] * wa + b[0] * wb) / divisor),
    Math.round((a[1] * wa + b[1] * wb) / divisor),
    Math.round((a[2] * wa + b[2] * wb) / divisor),
  ];
}

function distance(a, b) {
  const dr = a[0] - b[0];
  const dg = a[1] - b[1];
  const db = a[2] - b[2];
  return dr * dr + dg * dg + db * db;
}

/** Encode RGBA8 as BC1 (RGB, 1-bit alpha unused). */
export function encodeBc1(image) {
  const { width, height, data } = image;
  const blocksX = Math.ceil(width / BLOCK);
  const blocksY = Math.ceil(height / BLOCK);
  const out = Buffer.alloc(blocksX * blocksY * 8);
  let cursor = 0;
  for (let by = 0; by < blocksY; by += 1) {
    for (let bx = 0; bx < blocksX; bx += 1) {
      const pixels = [];
      for (let y = 0; y < BLOCK; y += 1) {
        for (let x = 0; x < BLOCK; x += 1) {
          const sx = Math.min(width - 1, bx * BLOCK + x);
          const sy = Math.min(height - 1, by * BLOCK + y);
          const offset = (sy * width + sx) * 4;
          pixels.push([data[offset], data[offset + 1], data[offset + 2]]);
        }
      }
      let min = [255, 255, 255];
      let max = [0, 0, 0];
      for (const pixel of pixels) {
        for (let c = 0; c < 3; c += 1) {
          if (pixel[c] < min[c]) min[c] = pixel[c];
          if (pixel[c] > max[c]) max[c] = pixel[c];
        }
      }
      let c0 = pack565(max[0], max[1], max[2]);
      let c1 = pack565(min[0], min[1], min[2]);
      if (c0 < c1) {
        const swap = c0;
        c0 = c1;
        c1 = swap;
      }
      const p0 = unpack565(c0);
      const p1 = unpack565(c1);
      const palette = c0 > c1
        ? [p0, p1, mix(p0, p1, 2, 1, 3), mix(p0, p1, 1, 2, 3)]
        : [p0, p1, mix(p0, p1, 1, 1, 2), [0, 0, 0]];
      let bits = 0;
      for (let i = 0; i < 16; i += 1) {
        let best = 0;
        let bestDistance = Infinity;
        for (let index = 0; index < 4; index += 1) {
          const d = distance(pixels[i], palette[index]);
          if (d < bestDistance) {
            bestDistance = d;
            best = index;
          }
        }
        bits = (bits | (best << (2 * i))) >>> 0;
      }
      out.writeUInt16LE(c0, cursor);
      out.writeUInt16LE(c1, cursor + 2);
      out.writeUInt32LE(bits, cursor + 4);
      cursor += 8;
    }
  }
  return out;
}

/** Encode one 8-bit channel as BC4 (8-value mode when possible). */
export function encodeBc4(channel, width, height) {
  const blocksX = Math.ceil(width / BLOCK);
  const blocksY = Math.ceil(height / BLOCK);
  const out = Buffer.alloc(blocksX * blocksY * 8);
  let cursor = 0;
  for (let by = 0; by < blocksY; by += 1) {
    for (let bx = 0; bx < blocksX; bx += 1) {
      const values = [];
      let min = 255;
      let max = 0;
      for (let y = 0; y < BLOCK; y += 1) {
        for (let x = 0; x < BLOCK; x += 1) {
          const sx = Math.min(width - 1, bx * BLOCK + x);
          const sy = Math.min(height - 1, by * BLOCK + y);
          const value = channel[sy * width + sx];
          values.push(value);
          if (value < min) min = value;
          if (value > max) max = value;
        }
      }
      if (min === max) {
        out[cursor] = max;
        out[cursor + 1] = max;
        out.fill(0, cursor + 2, cursor + 8);
      } else {
        const a0 = max;
        const a1 = min;
        const palette = [
          a0, a1,
          Math.round((6 * a0 + 1 * a1) / 7),
          Math.round((5 * a0 + 2 * a1) / 7),
          Math.round((4 * a0 + 3 * a1) / 7),
          Math.round((3 * a0 + 4 * a1) / 7),
          Math.round((2 * a0 + 5 * a1) / 7),
          Math.round((1 * a0 + 6 * a1) / 7),
        ];
        let bits = 0n;
        for (let i = 0; i < 16; i += 1) {
          let best = 0;
          let bestDistance = Infinity;
          for (let index = 0; index < 8; index += 1) {
            const d = Math.abs(values[i] - palette[index]);
            if (d < bestDistance) {
              bestDistance = d;
              best = index;
            }
          }
          bits |= BigInt(best) << BigInt(3 * i);
        }
        out[cursor] = a0;
        out[cursor + 1] = a1;
        for (let i = 0; i < 6; i += 1) {
          out[cursor + 2 + i] = Number((bits >> BigInt(8 * i)) & 0xFFn);
        }
      }
      cursor += 8;
    }
  }
  return out;
}

/** BC3 = BC1 colour block + BC4 alpha block. */
export function encodeBc3(image) {
  const color = encodeBc1(image);
  const alpha = new Uint8Array(image.width * image.height);
  for (let i = 0; i < alpha.length; i += 1) alpha[i] = image.data[i * 4 + 3];
  const alphaBlocks = encodeBc4(alpha, image.width, image.height);
  const out = Buffer.alloc(color.length + alphaBlocks.length);
  // BC3 blocks are alpha first, then the BC1 colour block. Reversing this makes GPU
  // sampling read alpha bytes as RGB, which shows up as ao = 0 in the shader.
  for (let block = 0; block < color.length / 8; block += 1) {
    alphaBlocks.copy(out, block * 16, block * 8, block * 8 + 8);
    color.copy(out, block * 16 + 8, block * 8, block * 8 + 8);
  }
  return out;
}

/** BC5 = two BC4 channels (normal X/Y). */
export function encodeBc5(image) {
  const red = new Uint8Array(image.width * image.height);
  const green = new Uint8Array(image.width * image.height);
  for (let i = 0; i < red.length; i += 1) {
    red[i] = image.data[i * 4];
    green[i] = image.data[i * 4 + 1];
  }
  const redBlocks = encodeBc4(red, image.width, image.height);
  const greenBlocks = encodeBc4(green, image.width, image.height);
  const out = Buffer.alloc(redBlocks.length + greenBlocks.length);
  for (let block = 0; block < redBlocks.length / 8; block += 1) {
    redBlocks.copy(out, block * 16, block * 8, block * 8 + 8);
    greenBlocks.copy(out, block * 16 + 8, block * 8, block * 8 + 8);
  }
  return out;
}

function readBc1Block(buffer, cursor) {
  const c0 = buffer.readUInt16LE(cursor);
  const c1 = buffer.readUInt16LE(cursor + 2);
  const bits = buffer.readUInt32LE(cursor + 4);
  const p0 = unpack565(c0);
  const p1 = unpack565(c1);
  const palette = c0 > c1
    ? [p0, p1, mix(p0, p1, 2, 1, 3), mix(p0, p1, 1, 2, 3)]
    : [p0, p1, mix(p0, p1, 1, 1, 2), [0, 0, 0]];
  const colors = [];
  for (let i = 0; i < 16; i += 1) colors.push(palette[(bits >>> (2 * i)) & 3]);
  return colors;
}

function readBc4Block(buffer, cursor) {
  const a0 = buffer[cursor];
  const a1 = buffer[cursor + 1];
  let bits = 0n;
  for (let i = 0; i < 6; i += 1) bits |= BigInt(buffer[cursor + 2 + i]) << BigInt(8 * i);
  let palette;
  if (a0 > a1) {
    palette = [
      a0, a1,
      Math.round((6 * a0 + 1 * a1) / 7),
      Math.round((5 * a0 + 2 * a1) / 7),
      Math.round((4 * a0 + 3 * a1) / 7),
      Math.round((3 * a0 + 4 * a1) / 7),
      Math.round((2 * a0 + 5 * a1) / 7),
      Math.round((1 * a0 + 6 * a1) / 7),
    ];
  } else {
    palette = [
      a0, a1,
      Math.round((4 * a0 + 1 * a1) / 5),
      Math.round((3 * a0 + 2 * a1) / 5),
      Math.round((2 * a0 + 3 * a1) / 5),
      Math.round((1 * a0 + 4 * a1) / 5),
      0, 255,
    ];
  }
  const values = [];
  for (let i = 0; i < 16; i += 1) values.push(palette[Number((bits >> BigInt(3 * i)) & 7n)]);
  return values;
}

export function decodeBc1(buffer, width, height) {
  const blocksX = Math.ceil(width / BLOCK);
  const blocksY = Math.ceil(height / BLOCK);
  const out = Buffer.alloc(width * height * 4);
  let cursor = 0;
  for (let by = 0; by < blocksY; by += 1) {
    for (let bx = 0; bx < blocksX; bx += 1) {
      const colors = readBc1Block(buffer, cursor);
      cursor += 8;
      for (let y = 0; y < BLOCK; y += 1) {
        for (let x = 0; x < BLOCK; x += 1) {
          const sx = bx * BLOCK + x;
          const sy = by * BLOCK + y;
          if (sx >= width || sy >= height) continue;
          const color = colors[y * BLOCK + x];
          const offset = (sy * width + sx) * 4;
          out[offset] = color[0];
          out[offset + 1] = color[1];
          out[offset + 2] = color[2];
          out[offset + 3] = 255;
        }
      }
    }
  }
  return out;
}

export function decodeBc4(buffer, width, height) {
  const blocksX = Math.ceil(width / BLOCK);
  const blocksY = Math.ceil(height / BLOCK);
  const out = new Uint8Array(width * height);
  let cursor = 0;
  for (let by = 0; by < blocksY; by += 1) {
    for (let bx = 0; bx < blocksX; bx += 1) {
      const values = readBc4Block(buffer, cursor);
      cursor += 8;
      for (let y = 0; y < BLOCK; y += 1) {
        for (let x = 0; x < BLOCK; x += 1) {
          const sx = bx * BLOCK + x;
          const sy = by * BLOCK + y;
          if (sx >= width || sy >= height) continue;
          out[sy * width + sx] = values[y * BLOCK + x];
        }
      }
    }
  }
  return out;
}

export function decodeBc3(buffer, width, height) {
  const colorBlocks = Buffer.alloc(buffer.length / 2);
  const alphaBlocks = Buffer.alloc(buffer.length / 2);
  for (let block = 0; block < buffer.length / 16; block += 1) {
    buffer.copy(alphaBlocks, block * 8, block * 16, block * 16 + 8);
    buffer.copy(colorBlocks, block * 8, block * 16 + 8, block * 16 + 16);
  }
  const color = decodeBc1(colorBlocks, width, height);
  const alpha = decodeBc4(alphaBlocks, width, height);
  for (let i = 0; i < width * height; i += 1) color[i * 4 + 3] = alpha[i];
  return color;
}

export function decodeBc5(buffer, width, height) {
  const redBlocks = Buffer.alloc(buffer.length / 2);
  const greenBlocks = Buffer.alloc(buffer.length / 2);
  for (let block = 0; block < buffer.length / 16; block += 1) {
    buffer.copy(redBlocks, block * 8, block * 16, block * 16 + 8);
    buffer.copy(greenBlocks, block * 8, block * 16 + 8, block * 16 + 16);
  }
  const red = decodeBc4(redBlocks, width, height);
  const green = decodeBc4(greenBlocks, width, height);
  const out = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i += 1) {
    const x = red[i] / 127.5 - 1;
    const y = green[i] / 127.5 - 1;
    const z = Math.sqrt(Math.max(0, 1 - x * x - y * y));
    out[i * 4] = Math.round((x * 0.5 + 0.5) * 255);
    out[i * 4 + 1] = Math.round((y * 0.5 + 0.5) * 255);
    out[i * 4 + 2] = Math.round((z * 0.5 + 0.5) * 255);
    out[i * 4 + 3] = 255;
  }
  return out;
}

const BC7_WEIGHTS_4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];
const BC7_WEIGHTS_2 = [0, 21, 43, 64];

function expand7(value) {
  return (value << 1) | (value >> 6);
}

function quantize7(value) {
  return Math.max(0, Math.min(127, Math.round((value * 127) / 255)));
}

function bc7Candidate(pixels, low, high) {
  const q0 = low.map(quantize7);
  const q1 = high.map(quantize7);
  const e0 = q0.map(expand7);
  const e1 = q1.map(expand7);
  const indices = new Array(16).fill(0);
  let error = 0;
  for (let i = 0; i < 16; i += 1) {
    const pixel = pixels[i];
    let best = Infinity;
    let bestIndex = 0;
    for (let index = 0; index < 16; index += 1) {
      const w = BC7_WEIGHTS_4[index];
      let candidate = 0;
      for (let c = 0; c < 4; c += 1) {
        const value = Math.round((e0[c] * (64 - w) + e1[c] * w + 32) / 64);
        const d = value - pixel[c];
        candidate += d * d;
      }
      if (candidate < best) {
        best = candidate;
        bestIndex = index;
      }
    }
    indices[i] = bestIndex;
    error += best;
  }
  return { q0, q1, indices, error };
}

function bc7PcaEndpoints(pixels) {
  const mean = [0, 0, 0, 0];
  for (const pixel of pixels) for (let c = 0; c < 4; c += 1) mean[c] += pixel[c] / 16;
  const covariance = Array.from({ length: 4 }, () => [0, 0, 0, 0]);
  for (const pixel of pixels) {
    for (let a = 0; a < 4; a += 1) {
      for (let b = 0; b < 4; b += 1) {
        covariance[a][b] += (pixel[a] - mean[a]) * (pixel[b] - mean[b]);
      }
    }
  }
  let axis = [0.5, 0.5, 0.5, 0.5];
  for (let iteration = 0; iteration < 8; iteration += 1) {
    const next = [0, 0, 0, 0];
    for (let a = 0; a < 4; a += 1) {
      for (let b = 0; b < 4; b += 1) next[a] += covariance[a][b] * axis[b];
    }
    const length = Math.hypot(...next) || 1;
    axis = next.map((value) => value / length);
  }
  let low = Infinity;
  let high = -Infinity;
  for (const pixel of pixels) {
    let projection = 0;
    for (let c = 0; c < 4; c += 1) projection += (pixel[c] - mean[c]) * axis[c];
    low = Math.min(low, projection);
    high = Math.max(high, projection);
  }
  const lowColor = mean.map((value, c) => Math.max(0, Math.min(255, value + axis[c] * low)));
  const highColor = mean.map((value, c) => Math.max(0, Math.min(255, value + axis[c] * high)));
  return [lowColor, highColor];
}

function bc7Refine(pixels, candidate) {
  let best = candidate;
  for (let iteration = 0; iteration < 3; iteration += 1) {
    const e0 = [];
    const e1 = [];
    for (let c = 0; c < 4; c += 1) {
      let aa = 0;
      let ab = 0;
      let bb = 0;
      let ap = 0;
      let bp = 0;
      for (let i = 0; i < 16; i += 1) {
        const t = BC7_WEIGHTS_4[best.indices[i]] / 64;
        const u = 1 - t;
        aa += u * u;
        ab += u * t;
        bb += t * t;
        ap += u * pixels[i][c];
        bp += t * pixels[i][c];
      }
      const determinant = aa * bb - ab * ab;
      if (Math.abs(determinant) < 1e-6) {
        e0[c] = best.q0[c] * 2;
        e1[c] = best.q1[c] * 2;
      } else {
        e0[c] = Math.max(0, Math.min(255, (ap * bb - bp * ab) / determinant));
        e1[c] = Math.max(0, Math.min(255, (bp * aa - ap * ab) / determinant));
      }
    }
    const next = bc7Candidate(pixels, e0, e1);
    if (next.error >= best.error) break;
    best = next;
  }
  return best;
}

function bc7Mode5Candidate(pixels) {
  const min = [255, 255, 255, 255];
  const max = [0, 0, 0, 0];
  for (const pixel of pixels) {
    for (let c = 0; c < 4; c += 1) {
      min[c] = Math.min(min[c], pixel[c]);
      max[c] = Math.max(max[c], pixel[c]);
    }
  }
  const q0 = [quantize7(min[0]), quantize7(min[1]), quantize7(min[2]), min[3]];
  const q1 = [quantize7(max[0]), quantize7(max[1]), quantize7(max[2]), max[3]];
  const e0 = [expand7(q0[0]), expand7(q0[1]), expand7(q0[2]), q0[3]];
  const e1 = [expand7(q1[0]), expand7(q1[1]), expand7(q1[2]), q1[3]];
  const indices = new Array(16).fill(0);
  let error = 0;
  for (let i = 0; i < 16; i += 1) {
    let best = Infinity;
    let bestIndex = 0;
    for (let index = 0; index < 4; index += 1) {
      const w = BC7_WEIGHTS_2[index];
      let candidate = 0;
      for (let c = 0; c < 4; c += 1) {
        const value = Math.round((e0[c] * (64 - w) + e1[c] * w + 32) / 64);
        const d = value - pixels[i][c];
        candidate += d * d;
      }
      if (candidate < best) {
        best = candidate;
        bestIndex = index;
      }
    }
    indices[i] = bestIndex;
    error += best;
  }
  return { q0, q1, indices, error };
}

function packBc7Mode5(candidate) {
  let block = 0x20n; // mode 5, one subset, rotation 0
  let shift = 9n; // 7 mode bits + 2 rotation bits
  block |= BigInt(candidate.q0[0]) << shift; shift += 7n;
  block |= BigInt(candidate.q1[0]) << shift; shift += 7n;
  block |= BigInt(candidate.q0[1]) << shift; shift += 7n;
  block |= BigInt(candidate.q1[1]) << shift; shift += 7n;
  block |= BigInt(candidate.q0[2]) << shift; shift += 7n;
  block |= BigInt(candidate.q1[2]) << shift; shift += 7n;
  block |= BigInt(candidate.q0[3]) << shift; shift += 8n;
  block |= BigInt(candidate.q1[3]) << shift; shift += 8n;
  for (let i = 0; i < 16; i += 1) {
    block |= BigInt(candidate.indices[i]) << (67n + BigInt(i) * 2n);
  }
  return block;
}

/**
 * BC7 mode 6: one subset, RGBA endpoints at 7 bits, sixteen 4-bit indices.
 * RDO v1 evaluates mode 6 (min/max, PCA, least-squares) and mode 5 candidates per block
 * and keeps the lower squared-error encoding.
 */
export function encodeBc7(image) {
  const { width, height, data } = image;
  const blocksX = Math.ceil(width / BLOCK);
  const blocksY = Math.ceil(height / BLOCK);
  const out = Buffer.alloc(blocksX * blocksY * 16);
  let cursor = 0;
  for (let by = 0; by < blocksY; by += 1) {
    for (let bx = 0; bx < blocksX; bx += 1) {
      const pixels = [];
      const min = [255, 255, 255, 255];
      const max = [0, 0, 0, 0];
      for (let y = 0; y < BLOCK; y += 1) {
        for (let x = 0; x < BLOCK; x += 1) {
          const sx = Math.min(width - 1, bx * BLOCK + x);
          const sy = Math.min(height - 1, by * BLOCK + y);
          const offset = (sy * width + sx) * 4;
          const pixel = [data[offset], data[offset + 1], data[offset + 2], data[offset + 3]];
          pixels.push(pixel);
          for (let c = 0; c < 4; c += 1) {
            if (pixel[c] < min[c]) min[c] = pixel[c];
            if (pixel[c] > max[c]) max[c] = pixel[c];
          }
        }
      }
      const minMax = bc7Candidate(pixels, min, max);
      const pca = bc7Candidate(pixels, ...bc7PcaEndpoints(pixels));
      const best6 = bc7Refine(pixels, pca.error < minMax.error ? pca : minMax);
      const best5 = bc7Mode5Candidate(pixels);
      let block;
      if (best5.error < best6.error) {
        block = packBc7Mode5(best5);
      } else {
        const { q0, q1, indices } = best6;
        block = 0x40n; // mode 6, one subset
        let shift = 7n;
        for (let c = 0; c < 4; c += 1) {
          block |= BigInt(q0[c]) << shift; shift += 7n;
          block |= BigInt(q1[c]) << shift; shift += 7n;
        }
        // 7 mode bits + 56 endpoint bits = 63; the 64 index bits end at bit 126.
        for (let i = 0; i < 16; i += 1) {
          block |= BigInt(indices[i]) << (63n + BigInt(i) * 4n);
        }
      }
      for (let byte = 0; byte < 16; byte += 1) {
        out[cursor + byte] = Number((block >> BigInt(byte * 8)) & 0xFFn);
      }
      cursor += 16;
    }
  }
  return out;
}

export function decodeBc7(buffer, width, height) {
  const blocksX = Math.ceil(width / BLOCK);
  const blocksY = Math.ceil(height / BLOCK);
  const out = Buffer.alloc(width * height * 4);
  for (let by = 0; by < blocksY; by += 1) {
    for (let bx = 0; bx < blocksX; bx += 1) {
      const cursor = (by * blocksX + bx) * 16;
      let block = 0n;
      for (let byte = 0; byte < 16; byte += 1) {
        block |= BigInt(buffer[cursor + byte]) << BigInt(byte * 8);
      }
      const mode = Number(block & 0x7Fn);
      if (mode === 0x20) {
        const rotation = Number((block >> 7n) & 3n);
        let shift = 9n;
        const e0 = [];
        const e1 = [];
        e0[0] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e1[0] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e0[1] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e1[1] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e0[2] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e1[2] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e0[3] = Number((block >> shift) & 0xFFn); shift += 8n;
        e1[3] = Number((block >> shift) & 0xFFn); shift += 8n;
        for (let i = 0; i < 16; i += 1) {
          const index = Number((block >> (67n + BigInt(i) * 2n)) & 0x3n);
          const w = BC7_WEIGHTS_2[index];
          const y = i >> 2;
          const x = i & 3;
          const sx = Math.min(width - 1, bx * BLOCK + x);
          const sy = Math.min(height - 1, by * BLOCK + y);
          const offset = (sy * width + sx) * 4;
          const color = [0, 0, 0, 0];
          for (let c = 0; c < 4; c += 1) {
            color[c] = Math.round((e0[c] * (64 - w) + e1[c] * w + 32) / 64);
          }
          if (rotation === 0) {
            for (let c = 0; c < 4; c += 1) out[offset + c] = color[c];
          } else {
            // Rotate alpha into the selected RGB channel (BC7 rotation 1..3).
            const target = rotation - 1;
            out[offset + target] = color[3];
            out[offset + 3] = color[target];
            for (let c = 0; c < 4; c += 1) {
              if (c !== target && c !== 3) out[offset + c] = color[c];
            }
          }
        }
        continue;
      }
      if (mode !== 0x40) {
        // Only modes 5 and 6 are produced by this cook; fill opaque black otherwise.
        for (let y = 0; y < BLOCK; y += 1) {
          for (let x = 0; x < BLOCK; x += 1) {
            const sx = Math.min(width - 1, bx * BLOCK + x);
            const sy = Math.min(height - 1, by * BLOCK + y);
            const offset = (sy * width + sx) * 4;
            out[offset] = out[offset + 1] = out[offset + 2] = 0;
            out[offset + 3] = 255;
          }
        }
        continue;
      }
      let shift = 7n;
      const e0 = [];
      const e1 = [];
      for (let c = 0; c < 4; c += 1) {
        e0[c] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
        e1[c] = expand7(Number((block >> shift) & 0x7Fn)); shift += 7n;
      }
      for (let i = 0; i < 16; i += 1) {
        const index = Number((block >> (63n + BigInt(i) * 4n)) & 0xFn);
        const w = BC7_WEIGHTS_4[index];
        const y = i >> 2;
        const x = i & 3;
        const sx = Math.min(width - 1, bx * BLOCK + x);
        const sy = Math.min(height - 1, by * BLOCK + y);
        const offset = (sy * width + sx) * 4;
        for (let c = 0; c < 4; c += 1) {
          out[offset + c] = Math.round((e0[c] * (64 - w) + e1[c] * w + 32) / 64);
        }
      }
    }
  }
  return out;
}

export function psnr(reference, tested, channels = 4) {
  let sum = 0;
  let count = 0;
  for (let i = 0; i < reference.length; i += 4) {
    for (let c = 0; c < channels; c += 1) {
      const delta = reference[i + c] - tested[i + c];
      sum += delta * delta;
      count += 1;
    }
  }
  const mse = sum / Math.max(1, count);
  if (mse === 0) return Infinity;
  return 10 * Math.log10((255 * 255) / mse);
}

export function normalAngularError(reference, tested) {
  const errors = [];
  for (let i = 0; i < reference.length; i += 4) {
    const ax = reference[i] / 127.5 - 1;
    const ay = reference[i + 1] / 127.5 - 1;
    const az = reference[i + 2] / 127.5 - 1;
    const bx = tested[i] / 127.5 - 1;
    const by = tested[i + 1] / 127.5 - 1;
    const bz = tested[i + 2] / 127.5 - 1;
    const la = Math.hypot(ax, ay, az) || 1;
    const lb = Math.hypot(bx, by, bz) || 1;
    const dot = Math.max(-1, Math.min(1, (ax * bx + ay * by + az * bz) / (la * lb)));
    errors.push((Math.acos(dot) * 180) / Math.PI);
  }
  errors.sort((a, b) => a - b);
  const mean = errors.reduce((sum, value) => sum + value, 0) / Math.max(1, errors.length);
  const p95 = errors[Math.min(errors.length - 1, Math.floor(errors.length * 0.95))] ?? 0;
  const p99 = errors[Math.min(errors.length - 1, Math.floor(errors.length * 0.99))] ?? 0;
  return { mean, p95, p99, max: errors.at(-1) ?? 0 };
}
