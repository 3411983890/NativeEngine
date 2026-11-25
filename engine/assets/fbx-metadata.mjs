// FBX container metadata.
//
// The converter (Assimp) triangulates FBX geometry correctly but drops two
// document-level facts that decide how the result must be placed in the engine:
// `GlobalSettings.UnitScaleFactor` and the declared up/front/coord axes. A Maya
// export authored in centimetres therefore arrives 100x too large, and a Z-up
// export arrives lying on its side. Reading only the container header and
// GlobalSettings keeps this module small: geometry still comes from the mature
// converter, and no vendor-specific mesh decoding is reimplemented here.
import { inflateSync } from 'node:zlib';
import { requireAsset } from './gltf-document.mjs';

const BINARY_MAGIC = 'Kaydara FBX Binary  ';
const AXIS_NAMES = ['X', 'Y', 'Z'];

function readBinaryProperty(buffer, cursor) {
  const type = String.fromCharCode(buffer[cursor]);
  const at = cursor + 1;
  switch (type) {
    case 'Y': return [buffer.readInt16LE(at), at + 2];
    case 'C': return [buffer.readUInt8(at) !== 0, at + 1];
    case 'I': return [buffer.readInt32LE(at), at + 4];
    case 'F': return [buffer.readFloatLE(at), at + 4];
    case 'D': return [buffer.readDoubleLE(at), at + 8];
    case 'L': return [Number(buffer.readBigInt64LE(at)), at + 8];
    case 'S': case 'R': {
      const length = buffer.readUInt32LE(at), start = at + 4;
      requireAsset(start + length <= buffer.length, 'FBX property exceeds file', 'FORMAT');
      return [type === 'S' ? buffer.toString('utf8', start, start + length) : buffer.subarray(start, start + length), start + length];
    }
    case 'f': case 'd': case 'l': case 'i': case 'b': {
      const count = buffer.readUInt32LE(at), encoding = buffer.readUInt32LE(at + 4), bytes = buffer.readUInt32LE(at + 8), start = at + 12;
      requireAsset(start + bytes <= buffer.length && count <= 64 * 1024 * 1024, 'FBX array property exceeds file', 'FORMAT');
      const raw = encoding === 1 ? inflateSync(buffer.subarray(start, start + bytes)) : buffer.subarray(start, start + bytes);
      const element = { f: 4, d: 8, l: 8, i: 4, b: 1 }[type];
      requireAsset(raw.length >= count * element, 'FBX array payload is short', 'FORMAT');
      const values = [];
      for (let i = 0; i < count; i += 1) {
        if (type === 'f') values.push(raw.readFloatLE(i * 4));
        else if (type === 'd') values.push(raw.readDoubleLE(i * 8));
        else if (type === 'l') values.push(Number(raw.readBigInt64LE(i * 8)));
        else if (type === 'i') values.push(raw.readInt32LE(i * 4));
        else values.push(raw[i] !== 0);
      }
      return [values, start + bytes];
    }
    default: throw new Error(`Unknown FBX property type ${JSON.stringify(type)}`);
  }
}

function readBinaryNode(buffer, offset, wide) {
  const header = wide ? 25 : 13;
  if (offset + header > buffer.length) return null;
  const endOffset = wide ? Number(buffer.readBigUInt64LE(offset)) : buffer.readUInt32LE(offset);
  const propertyCount = wide ? Number(buffer.readBigUInt64LE(offset + 8)) : buffer.readUInt32LE(offset + 4);
  const nameLength = buffer.readUInt8(offset + header - 1);
  // A record whose offsets are all zero terminates the current child list.
  if (endOffset === 0 && propertyCount === 0 && nameLength === 0) return null;
  requireAsset(endOffset > offset + header && endOffset <= buffer.length, 'FBX node record is out of range', 'FORMAT');
  const name = buffer.toString('utf8', offset + header, offset + header + nameLength);
  let cursor = offset + header + nameLength;
  const properties = [];
  for (let i = 0; i < propertyCount; i += 1) {
    const [value, next] = readBinaryProperty(buffer, cursor);
    properties.push(value);
    cursor = next;
  }
  const children = [];
  while (cursor < endOffset) {
    const record = readBinaryNode(buffer, cursor, wide);
    if (!record) break;
    children.push(record.node);
    cursor = record.end;
  }
  return { node: { name, properties, children }, end: endOffset };
}

function parseBinaryNodes(buffer) {
  const version = buffer.readUInt32LE(23);
  const wide = version >= 7500;
  const nodes = [];
  let offset = 27;
  while (offset + (wide ? 25 : 13) <= buffer.length) {
    const record = readBinaryNode(buffer, offset, wide);
    if (!record) break;
    nodes.push(record.node);
    offset = record.end;
  }
  return { version, nodes };
}

const NUMBER = /^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/;

function tokenize(text) {
  const tokens = [];
  let index = 0;
  while (index < text.length) {
    const character = text[index];
    if (character === ';') { while (index < text.length && text[index] !== '\n') index += 1; continue; }
    if (/\s/.test(character)) { index += 1; continue; }
    if (character === '"') {
      const end = text.indexOf('"', index + 1);
      requireAsset(end > index, 'Unterminated FBX ASCII string', 'FORMAT');
      tokens.push({ type: 'string', value: text.slice(index + 1, end) });
      index = end + 1;
      continue;
    }
    if ('{}:,*'.includes(character)) { tokens.push({ type: character }); index += 1; continue; }
    let start = index;
    while (index < text.length && !/[\s{}:,*"]/.test(text[index])) index += 1;
    const raw = text.slice(start, index);
    if (NUMBER.test(raw)) tokens.push({ type: 'number', value: Number(raw) });
    else tokens.push({ type: 'ident', value: raw });
  }
  return tokens;
}

function parseAsciiNodes(text) {
  const tokens = tokenize(text);
  let index = 0;
  const block = () => {
    const nodes = [];
    while (index < tokens.length && tokens[index].type !== '}') nodes.push(node());
    if (tokens[index]?.type === '}') index += 1;
    return nodes;
  };
  const values = () => {
    const list = [];
    while (index < tokens.length && tokens[index].type !== '}') {
      const token = tokens[index];
      if (token.type === ',') { index += 1; continue; }
      if (token.type === 'ident' && tokens[index + 1]?.type === ':') break;
      list.push(token.value ?? token.type);
      index += 1;
    }
    return list;
  };
  const node = () => {
    const name = tokens[index++];
    requireAsset(name?.type === 'ident', 'FBX ASCII node must start with an identifier', 'FORMAT');
    const result = { name: name.value, properties: [], children: [] };
    if (tokens[index]?.type !== ':') return result;
    index += 1;
    if (tokens[index]?.type === '*') {
      // Typed array shorthand: `Vertices: *12 { a: 1,2,3 }`.
      index += 1;
      if (tokens[index]?.type === 'number') index += 1;
      if (tokens[index]?.type === '{') {
        index += 1;
        if (tokens[index]?.type === 'ident' && tokens[index + 1]?.type === ':') { index += 2; result.properties.push(values()); }
        if (tokens[index]?.type === '}') index += 1;
      }
      return result;
    }
    while (index < tokens.length) {
      const token = tokens[index];
      if (token.type === '{') { index += 1; result.children = block(); break; }
      if (token.type === '}' || token.type === ':') break;
      if (token.type === 'ident' && tokens[index + 1]?.type === ':') break;
      if (token.type === ',') { index += 1; continue; }
      result.properties.push(token.value ?? token.type);
      index += 1;
    }
    return result;
  };
  return block();
}

const findNode = (nodes, name) => (nodes ?? []).find((node) => node.name === name);

function numberSetting(settings, key, fallback) {
  const value = settings.get(key);
  return Number.isFinite(value) ? value : fallback;
}

/**
 * Read the FBX document-level settings.
 *
 * Returns `undefined` when the file is not an FBX container so callers can share
 * one code path for every convertible format.
 */
export function readFbxMetadata(bytes) {
  if (!Buffer.isBuffer(bytes) || bytes.length < 27) return undefined;
  const binary = bytes.toString('latin1', 0, 20) === BINARY_MAGIC;
  if (!binary && !/^\s*;\s*FBX/i.test(bytes.toString('latin1', 0, 64)) && !bytes.toString('latin1', 0, 4096).includes('FBXHeaderExtension')) return undefined;
  const { version, nodes } = binary
    ? parseBinaryNodes(bytes)
    : (() => {
      requireAsset(bytes.length <= 256 * 1024 * 1024, 'FBX source exceeds 256 MiB', 'BUDGET');
      return { version: undefined, nodes: parseAsciiNodes(bytes.toString('utf8')) };
    })();
  const header = findNode(nodes, 'FBXHeaderExtension');
  const declaredVersion = Number(findNode(header?.children, 'FBXVersion')?.properties?.[0]);
  const settings = new Map();
  const properties = findNode(findNode(nodes, 'GlobalSettings')?.children, 'Properties70');
  for (const entry of properties?.children ?? []) {
    if (entry.name !== 'P' || typeof entry.properties[0] !== 'string') continue;
    settings.set(entry.properties[0], entry.properties[4]);
  }
  const unitScaleFactor = numberSetting(settings, 'UnitScaleFactor', 1);
  requireAsset(Number.isFinite(unitScaleFactor) && unitScaleFactor > 0 && unitScaleFactor <= 100000,
    'FBX UnitScaleFactor is out of range', 'FORMAT');
  return {
    format: binary ? 'binary' : 'ascii',
    version: version ?? (Number.isFinite(declaredVersion) ? declaredVersion : undefined),
    unitScaleFactor,
    // FBX expresses UnitScaleFactor in centimetres per file unit.
    metersPerUnit: unitScaleFactor / 100,
    upAxis: numberSetting(settings, 'UpAxis', 1),
    upAxisSign: numberSetting(settings, 'UpAxisSign', 1),
    frontAxis: numberSetting(settings, 'FrontAxis', 2),
    frontAxisSign: numberSetting(settings, 'FrontAxisSign', 1),
    coordAxis: numberSetting(settings, 'CoordAxis', 0),
    coordAxisSign: numberSetting(settings, 'CoordAxisSign', 1),
  };
}

const axisName = (index, sign) => `${sign < 0 ? '-' : '+'}${AXIS_NAMES[index] ?? 'Y'}`;
const axisVector = (index, sign) => [0, 1, 2].map((k) => (k === index ? Math.sign(sign) || 1 : 0));
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a, b) => a.reduce((sum, value, index) => sum + value * b[index], 0);

/**
 * Translate FBX document axes into a coordinate override for the importers.
 *
 * `coordinateTransform` derives its right axis as cross(up, front), so a Z-up /
 * Y-forward file (3ds Max, many DCC defaults) needs the mirrored handedness flag
 * to keep +X on the same side instead of producing a reflected model. The flag is
 * chosen from the file's own CoordAxis rather than assumed.
 */
export function fbxCoordinateOptions(metadata) {
  if (!metadata) return undefined;
  const up = axisName(metadata.upAxis, metadata.upAxisSign);
  const forward = axisName(metadata.frontAxis, metadata.frontAxisSign);
  requireAsset(up.at(-1) !== forward.at(-1), 'FBX up and front axes must be perpendicular', 'FORMAT');
  const upVector = axisVector(metadata.upAxis, metadata.upAxisSign);
  const frontVector = axisVector(metadata.frontAxis, metadata.frontAxisSign);
  const coordVector = axisVector(metadata.coordAxis, metadata.coordAxisSign);
  const handedness = dot(cross(upVector, frontVector), coordVector) >= 0 ? 'right' : 'left';
  const options = { metersPerUnit: metadata.metersPerUnit, upAxis: up, forwardAxis: forward, handedness };
  // Report only the settings that differ from the importer default, so a
  // conventional Y-up metre file keeps the documented default identity.
  return options.metersPerUnit === 1 && up === '+Y' && forward === '+Z' && handedness === 'right' ? undefined : options;
}

export function describeFbx(metadata) {
  if (!metadata) return undefined;
  return {
    format: metadata.format,
    version: metadata.version,
    unitScaleFactor: metadata.unitScaleFactor,
    metersPerUnit: metadata.metersPerUnit,
    upAxis: axisName(metadata.upAxis, metadata.upAxisSign),
    frontAxis: axisName(metadata.frontAxis, metadata.frontAxisSign),
    coordAxis: axisName(metadata.coordAxis, metadata.coordAxisSign),
  };
}
