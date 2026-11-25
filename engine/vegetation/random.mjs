// Deterministic integer helpers shared by every vegetation/groom cook step.
//
// Node's Math.random and any float-derived seed are forbidden here for the same
// reason the native simulation forbids std::rand: two runs of the same source and
// seed must produce identical bytes, and a float hash differs between platforms.

export function mix32(value) {
  let x = value >>> 0;
  x ^= x >>> 16; x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15; x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}

export function mix64(hi, lo) {
  const a = mix32((hi ^ 0x9e3779b9) >>> 0);
  const b = mix32((lo ^ 0x85ebca6b) >>> 0);
  return [mix32(a ^ (b >>> 1)), mix32(b ^ (a << 1))];
}

export function hashString(text) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < text.length; ++i) { h ^= text.charCodeAt(i); h = Math.imul(h, 16777619) >>> 0; }
  return h >>> 0;
}

// Splits a 32-bit seed into an independent stream name, so adding a new consumer
// cannot shift the sequence an existing consumer already depends on.
export function deriveSeed(seed, name) {
  return mix32((seed ^ hashString(name)) >>> 0);
}

// xoshiro128** — fully specified by 32-bit integer arithmetic.
export class Stream {
  constructor(seed, name = '') {
    this.name = name;
    this.state = new Uint32Array(4);
    this.reseed(seed);
  }
  reseed(seed) {
    let x = mix32(seed >>> 0);
    for (let i = 0; i < 4; ++i) { x = mix32((x + 0x9e3779b9) >>> 0); this.state[i] = x; }
    if ((this.state[0] | this.state[1] | this.state[2] | this.state[3]) === 0) this.state[0] = 1;
    this.counter = 0;
    return this;
  }
  next() {
    const s = this.state;
    const result = (Math.imul(rotl(Math.imul(s[1], 5) >>> 0, 7), 9) >>> 0);
    const t = (s[1] << 9) >>> 0;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
    s[3] = rotl(s[3], 11);
    this.counter = (this.counter + 1) >>> 0;
    return result >>> 0;
  }
  // [0,1) from the top 24 bits: exactly representable, so the value is stable.
  unit() { return (this.next() >>> 8) / 16777216; }
  range(bound) { return bound <= 0 ? 0 : this.next() % bound; }
  span(low, high) { return low + (high - low) * this.unit(); }
  normal() {
    // Box–Muller with a clamped radius; no log(0) and no platform libm dependency
    // beyond Math.sqrt/log, which are correctly rounded for these inputs.
    const u = Math.max(this.unit(), 1e-7), v = this.unit();
    return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
  }
}

function rotl(value, count) { return ((value << count) | (value >>> (32 - count))) >>> 0; }

export function quantizeUnit(value, bits) {
  const max = (1 << bits) - 1;
  return Math.max(0, Math.min(max, Math.round(value * max)));
}

export function dequantizeUnit(value, bits) {
  const max = (1 << bits) - 1;
  return value / max;
}
