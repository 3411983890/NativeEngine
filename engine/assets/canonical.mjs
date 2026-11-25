import { createHash } from 'node:crypto';

// Canonical JSON is the identity of every cooked asset in this project. Key order,
// whitespace and platform number formatting must never change a hash, so the encoding
// is defined here once instead of being re-implemented per asset type.
export function canonicalJson(value) {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(',')}]`;
  if (value && typeof value === 'object') return `{${Object.keys(value).sort().map((key) =>
    `${JSON.stringify(key)}:${canonicalJson(value[key])}`).join(',')}}`;
  if (value === undefined || (typeof value === 'number' && !Number.isFinite(value)))
    throw new Error('non-JSON value');
  const result = JSON.stringify(value);
  if (typeof result !== 'string') throw new Error('non-JSON value');
  return result;
}

export function sha256Hex(value) {
  return createHash('sha256').update(value).digest('hex');
}
