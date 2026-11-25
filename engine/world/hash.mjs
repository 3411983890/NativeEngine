// Deterministic hashing for world inputs.
//
// Two rules make the cache and the revision check trustworthy:
//   * The digest is computed from *canonical* JSON: keys sorted, numbers normalized to a
//     fixed decimal grid, no whitespace. Two authoring sessions that produce the same
//     recipe therefore produce the same digest even if the key order differs.
//   * File timestamps are never part of the digest. A `touch` must not invalidate a cook,
//     and a restored backup must not silently reuse a stale one.

import { createHash } from 'node:crypto';

// Numbers are quantized before hashing. Floats that differ in the 12th decimal are the
// same authored value; hashing their exact bits would make the digest depend on how the
// UI rounded a drag.
const QUANTIZE = 1e6;

export function canonicalNumber(value) {
  if (!Number.isFinite(value)) return '0';
  const scaled = Math.round(value * QUANTIZE);
  if (Object.is(scaled, -0)) return '0';
  // Integer formatting avoids "1e-7" vs "0.0000001" style differences between engines.
  const sign = scaled < 0 ? '-' : '';
  const absolute = Math.abs(scaled);
  const whole = Math.floor(absolute / QUANTIZE);
  const fraction = absolute % QUANTIZE;
  if (fraction === 0) return `${sign}${whole}`;
  return `${sign}${whole}.${String(fraction).padStart(6, '0').replace(/0+$/, '')}`;
}

export function canonicalize(value) {
  if (value === null) return 'null';
  if (value === undefined) return 'null';
  const type = typeof value;
  if (type === 'boolean') return value ? 'true' : 'false';
  if (type === 'number') return canonicalNumber(value);
  if (type === 'string') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(canonicalize).join(',')}]`;
  if (type === 'object') {
    const keys = Object.keys(value).filter(key => value[key] !== undefined).sort();
    return `{${keys.map(key => `${JSON.stringify(key)}:${canonicalize(value[key])}`).join(',')}}`;
  }
  // Functions, symbols and bigints are not authoring data; encoding them as null keeps a
  // stray field from changing the digest between two otherwise identical recipes.
  return 'null';
}

export function sha256Hex(bytes) {
  return createHash('sha256').update(bytes).digest('hex');
}

export function sha256Canonical(value) {
  return sha256Hex(Buffer.from(canonicalize(value), 'utf8'));
}

// Short digest used in file names and ACK lines. 16 hex digits is 64 bits: enough that a
// collision inside one project is not a practical concern, short enough to travel on the
// one-line pipe protocol.
export function shortDigest(value) {
  return sha256Canonical(value).slice(0, 16);
}

// The input digest of a cook. Everything that can change the output is in here, and
// nothing that cannot is. `overridesDigest` folds in the hand edits, which is what makes a
// re-cook after a lock/delete produce a new manifest instead of reusing the old one.
export function inputDigest({ recipe, generatorVersion, libraryHashes = [], layerHashes = [], overridesDigest = '', ruleHashes = [] }) {
  const digest = sha256Canonical({
    generatorVersion,
    seed: String(recipe?.seed ?? '0'),
    extentMetres: recipe?.extentMetres,
    originMetres: recipe?.originMetres,
    chunkSizeMetres: recipe?.chunkSizeMetres,
    terrain: recipe?.terrain,
    budgetProfile: recipe?.budgetProfile,
    libraries: [...libraryHashes].sort(),
    layers: [...layerHashes].sort(),
    rules: [...ruleHashes].sort(),
    overrides: overridesDigest,
  });
  return digest;
}

export const SHA256_PATTERN = /^[a-f0-9]{64}$/;
export const SHORT_DIGEST_PATTERN = /^[a-f0-9]{16}$/;

export function isSha256(value) {
  return typeof value === 'string' && SHA256_PATTERN.test(value);
}

// A seed is a decimal string, never a JS number. 64-bit seeds do not survive
// JSON.stringify(Number) above 2^53, and the C++ side reads the same string.
export function validSeed(value) {
  if (typeof value !== 'string' || value.length === 0 || value.length > 20) return false;
  if (!/^\d+$/.test(value)) return false;
  try {
    const parsed = BigInt(value);
    return parsed >= 0n && parsed <= 18446744073709551615n;
  } catch {
    return false;
  }
}
