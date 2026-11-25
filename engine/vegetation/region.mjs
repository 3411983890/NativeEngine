import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { buildPackage, digestPackage, packRecords, SectionKind } from '../assets/binary-package.mjs';
import { CoverageMask, normalizeExclusions, scatterRegion, validateScatterRule, SCATTER_SCHEMA } from './scatter.mjs';
import { createSurface, SurfaceSampler } from './surface.mjs';
import { hashString, mix32 } from './random.mjs';

// Region cook: turns a placement description into content-addressed pages of packed
// 32-byte instance records plus a page-level spatial index.
//
// The packed record is a contract shared with HLSL. Its quantization error is measured
// per page and written into the manifest, because "we quantized positions" without a
// number is how a plant ends up floating a centimetre above its terrain.

export const REGION_SCHEMA = 'nativegpu.vegetation-region/1';
export const REGION_COOK_SCHEMA = 'nativegpu.vegetation-region-cook/1';
export const INSTANCE_BYTES = 32;
export const PAGE_BYTES = 64;
export const NO_AGGREGATE = 0xFFFFFFFF;

// Field order is the ABI. Changing it changes every page ever cooked.
export const INSTANCE_LAYOUT = [
  { name: 'qx', type: 'u16', bytes: 2 },
  { name: 'qz', type: 'u16', bytes: 2 },
  { name: 'qy', type: 'u16', bytes: 2 },
  { name: 'yaw', type: 'u16', bytes: 2 },
  { name: 'tiltX', type: 'i16', bytes: 2 },
  { name: 'tiltZ', type: 'i16', bytes: 2 },
  { name: 'scale', type: 'u8', bytes: 1 },
  { name: 'species', type: 'u8', bytes: 1 },
  { name: 'windPhase', type: 'u16', bytes: 2 },
  { name: 'lodBias', type: 'u8', bytes: 1 },
  { name: 'flags', type: 'u8', bytes: 1 },
  { name: 'palette', type: 'u16', bytes: 2 },
  { name: 'stableId', type: 'u32', bytes: 4 },
  { name: 'aggregate', type: 'u32', bytes: 4 },
  { name: 'reserved', type: 'u32', bytes: 4 },
];

export const PAGE_LAYOUT = [
  { name: 'firstInstance', type: 'u32', bytes: 4 },
  { name: 'instanceCount', type: 'u32', bytes: 4 },
  { name: 'minX', type: 'f32', bytes: 4 },
  { name: 'minZ', type: 'f32', bytes: 4 },
  { name: 'maxX', type: 'f32', bytes: 4 },
  { name: 'maxZ', type: 'f32', bytes: 4 },
  { name: 'baseY', type: 'f32', bytes: 4 },
  { name: 'heightRange', type: 'f32', bytes: 4 },
  { name: 'cellX', type: 'u32', bytes: 4 },
  { name: 'cellZ', type: 'u32', bytes: 4 },
  { name: 'flags', type: 'u32', bytes: 4 },
  { name: 'pageIndex', type: 'u32', bytes: 4 },
  { name: 'lodMask', type: 'u32', bytes: 4 },
  { name: 'reserved', type: 'u32', bytes: 4 },
  { name: 'stableLo', type: 'u32', bytes: 4 },
  { name: 'stableHi', type: 'u32', bytes: 4 },
];

export const SPATIAL_LAYOUT = [
  { name: 'cellX', type: 'u32', bytes: 4 },
  { name: 'cellZ', type: 'u32', bytes: 4 },
  { name: 'firstPage', type: 'u32', bytes: 4 },
  { name: 'pageCount', type: 'u32', bytes: 4 },
];

export class RegionError extends Error {
  constructor(code, message) { super(message); this.name = 'RegionError'; this.code = code; }
}
const fail = (ok, code, message) => { if (!ok) throw new RegionError(code, message); };
const num = (value, low, high, label) => {
  fail(typeof value === 'number' && Number.isFinite(value) && value >= low && value <= high,
    'NG-E-VEG-SCATTER', `${label} must be finite and in [${low},${high}]`);
  return value;
};

export function validateRegion(region, speciesIds) {
  fail(region && typeof region === 'object', 'NG-E-VEG-SCATTER', 'region must be an object');
  fail(region.schema === REGION_SCHEMA, 'NG-E-VEG-SCATTER', `unsupported region schema ${region.schema}`);
  fail(typeof region.id === 'string' && /^[a-z][a-z0-9_-]{0,63}$/.test(region.id), 'NG-E-VEG-SCATTER', 'region.id is not a valid identifier');
  fail(Number.isSafeInteger(region.seed) && region.seed >= 0 && region.seed <= 0xFFFFFFFF, 'NG-E-VEG-SCATTER', 'region.seed out of range');
  const bounds = region.bounds;
  fail(bounds && typeof bounds === 'object', 'NG-E-VEG-SCATTER', 'region.bounds is required');
  for (const key of ['minX', 'minZ', 'maxX', 'maxZ']) num(bounds[key], -1e6, 1e6, `bounds.${key}`);
  fail(bounds.maxX > bounds.minX && bounds.maxZ > bounds.minZ, 'NG-E-VEG-SCATTER', 'region bounds are empty or inverted');
  const areaMetres2 = (bounds.maxX - bounds.minX) * (bounds.maxZ - bounds.minZ);
  fail(areaMetres2 > 1 && areaMetres2 <= 4e10, 'NG-E-VEG-CAPACITY', 'region area out of range');
  num(region.cellSizeMetres ?? 8, 1, 512, 'cellSizeMetres');
  num(region.pageCellMetres ?? 32, 2, 2048, 'pageCellMetres');
  const pageSize = region.pageSize ?? 8192;
  fail(Number.isSafeInteger(pageSize) && pageSize >= 16 && pageSize <= 65536, 'NG-E-VEG-CAPACITY', 'pageSize must be an integer in [16,65536]');
  fail(Array.isArray(region.species) && region.species.length >= 1 && region.species.length <= 64, 'NG-E-VEG-SCATTER', 'region.species needs 1–64 rules');
  fail(speciesIds.size >= 1 && speciesIds.size <= 255, 'NG-E-VEG-SCATTER', 'a region page addresses at most 255 species');
  const rules = region.species.map((rule, index) => validateScatterRule(rule, index, speciesIds));
  fail(region.surface && typeof region.surface === 'object', 'NG-E-VEG-SURFACE', 'region.surface is required');
  // Brush masks and exclusion shapes live on the region, not on the rule: a stroke is one edit
  // that many layers obey, and a road is one obstacle that every layer must avoid.
  const masks = (region.masks ?? []).map((entry, index) => {
    fail(entry && typeof entry === 'object', 'NG-E-VEG-SCATTER', `region.masks[${index}] must be an object`);
    return new CoverageMask({ ...entry, id: entry.id ?? `mask_${index}` });
  });
  const maskIds = new Set(masks.map((mask) => mask.id));
  fail(maskIds.size === masks.length, 'NG-E-VEG-SCATTER', 'region.masks declares an id twice');
  for (const rule of rules) {
    if (rule.mask) fail(maskIds.has(rule.mask.id), 'NG-E-VEG-SCATTER', `rule ${rule.species} references missing mask ${rule.mask.id}`);
  }
  return {
    id: region.id, seed: region.seed, bounds, areaMetres2,
    cellSizeMetres: region.cellSizeMetres ?? 8,
    pageCellMetres: region.pageCellMetres ?? 32,
    pageSize, rules, surface: region.surface,
    masks, exclusions: normalizeExclusions(region.exclusions ?? []),
    budget: { maxInstances: region.budget?.maxInstances ?? 2_000_000 },
  };
}

export function quantizeInstance(instance, page, speciesIndex) {
  const spanX = Math.max(1e-6, page.maxX - page.minX);
  const spanZ = Math.max(1e-6, page.maxZ - page.minZ);
  const spanY = Math.max(1e-6, page.heightRange);
  const logScale = Math.log2(Math.max(1e-3, instance.scale));
  return {
    qx: Math.max(0, Math.min(65535, Math.round(((instance.x - page.minX) / spanX) * 65535))),
    qz: Math.max(0, Math.min(65535, Math.round(((instance.z - page.minZ) / spanZ) * 65535))),
    qy: Math.max(0, Math.min(65535, Math.round(((instance.y - page.baseY) / spanY) * 65535))),
    yaw: Math.max(0, Math.min(65535, Math.round((instance.yaw / (Math.PI * 2)) * 65535) % 65536)),
    tiltX: Math.max(-32767, Math.min(32767, Math.round((instance.tiltX / (Math.PI / 2)) * 32767))),
    tiltZ: Math.max(-32767, Math.min(32767, Math.round((instance.tiltZ / (Math.PI / 2)) * 32767))),
    scale: Math.max(0, Math.min(255, Math.round(((logScale + 4) / 8) * 255))),
    species: speciesIndex,
    windPhase: instance.windPhase & 0xFFFF,
    lodBias: instance.lodBias & 0xFF,
    flags: instance.flags & 0xFF,
    palette: instance.palette & 0xFFFF,
    stableId: instance.stableId >>> 0,
    aggregate: NO_AGGREGATE,
    reserved: 0,
  };
}

// Inverse used by tests and by the editor preview. It reports exactly the same
// rounding the GPU decode does, so an editor can show the quantized result.
export function dequantizeInstance(record, page) {
  const spanX = page.maxX - page.minX, spanZ = page.maxZ - page.minZ;
  return {
    x: page.minX + (record.qx / 65535) * spanX,
    y: page.baseY + (record.qy / 65535) * page.heightRange,
    z: page.minZ + (record.qz / 65535) * spanZ,
    yaw: (record.yaw / 65536) * Math.PI * 2,
    tiltX: (record.tiltX / 32767) * (Math.PI / 2),
    tiltZ: (record.tiltZ / 32767) * (Math.PI / 2),
    scale: Math.pow(2, (record.scale / 255) * 8 - 4),
    species: record.species, windPhase: record.windPhase,
    lodBias: record.lodBias, flags: record.flags, palette: record.palette,
    stableId: record.stableId,
  };
}

export function compileRegion({ region, species, surface, spacingHalo = [] }) {
  fail(Array.isArray(species) && species.length >= 1, 'NG-E-VEG-SPECIES', 'region needs at least one cooked species');
  const speciesById = new Map(species.map((entry) => [entry.id, entry]));
  const parsed = validateRegion(region, new Set(speciesById.keys()));
  // A caller may hand in a raw adapter or a sampler. Only the sampler adds the
  // derived masks (slope, height, moisture) that every scatter rule reads, so wrapping
  // here is what keeps the CLI and the tests on the same contract.
  const adapter = surface ?? createSurface(parsed.surface);
  const sampler = adapter instanceof SurfaceSampler ? adapter : new SurfaceSampler(adapter);
  fail(sampler.version === (parsed.surface.version ?? sampler.version), 'NG-E-VEG-SURFACE', 'region surface version disagrees with the sampler');
  const scattered = scatterRegion({
    region: parsed, rules: parsed.rules, surface: sampler,
    masks: parsed.masks, exclusions: parsed.exclusions,
    spacingHalo,
  });
  const instances = scattered.instances;
  fail(instances.length <= parsed.budget.maxInstances, 'NG-E-VEG-CAPACITY',
    `region produced ${instances.length} instances, above budget.maxInstances=${parsed.budget.maxInstances}`);

  // Stable IDs come from the candidate identity — (layer, species, cell, slot) — not from the
  // position in the filtered array. Rejecting one candidate with a slope mask therefore cannot
  // renumber the rest, which is the property an erase, a move or a lock in the editor depends on.
  //
  // The packed field is 32 bits (see INSTANCE_LAYOUT), so a hash collision is possible in
  // principle. It is detected here and reported instead of silently merging two plants into one
  // identity: two different plants that claim the same id is a corrupted save, not a detail.
  const stableOwners = new Map();
  for (const instance of instances) {
    const key = instance.candidateId ?? `${instance.species}/${instance.cellX}/${instance.cellZ}/${instance.slot}`;
    const id = hashString(key);
    const owner = stableOwners.get(id);
    if (owner !== undefined && owner !== key) {
      fail(false, 'NG-E-VEG-ID',
        `stable id collision: ${owner} and ${key} both hash to ${id}; the packed id is 32 bits, so split the region or raise the id width`);
    }
    stableOwners.set(id, key);
    instance.stableId = id;
  }

  const pageCell = parsed.pageCellMetres;
  const pageCellsX = Math.ceil((parsed.bounds.maxX - parsed.bounds.minX) / pageCell);
  const pageCellsZ = Math.ceil((parsed.bounds.maxZ - parsed.bounds.minZ) / pageCell);
  const buckets = new Map();
  for (const instance of instances) {
    const px = Math.min(pageCellsX - 1, Math.max(0, Math.floor((instance.x - parsed.bounds.minX) / pageCell)));
    const pz = Math.min(pageCellsZ - 1, Math.max(0, Math.floor((instance.z - parsed.bounds.minZ) / pageCell)));
    const key = pz * pageCellsX + px;
    let bucket = buckets.get(key);
    if (!bucket) buckets.set(key, bucket = { px, pz, list: [] });
    bucket.list.push(instance);
  }

  const pages = [], records = [];
  const speciesIndex = new Map(species.map((entry, index) => [entry.id, index]));
  for (const key of [...buckets.keys()].sort((a, b) => a - b)) {
    const bucket = buckets.get(key);
    const chunks = [];
    for (let start = 0; start < bucket.list.length; start += parsed.pageSize) chunks.push(bucket.list.slice(start, start + parsed.pageSize));
    chunks.forEach((chunk, chunkIndex) => {
      let minX = Infinity, minZ = Infinity, maxX = -Infinity, maxZ = -Infinity, baseY = Infinity, maxY = -Infinity;
      for (const instance of chunk) {
        minX = Math.min(minX, instance.x); maxX = Math.max(maxX, instance.x);
        minZ = Math.min(minZ, instance.z); maxZ = Math.max(maxZ, instance.z);
        baseY = Math.min(baseY, instance.y); maxY = Math.max(maxY, instance.y);
      }
      // A page with zero extent in one axis would divide by zero during quantization;
      // pad it to the nominal page cell so the mapping stays invertible.
      if (maxX - minX < 1e-3) { minX -= pageCell / 2; maxX += pageCell / 2; }
      if (maxZ - minZ < 1e-3) { minZ -= pageCell / 2; maxZ += pageCell / 2; }
      const heightRange = Math.max(0.5, maxY - baseY);
      const page = {
        firstInstance: records.length, instanceCount: chunk.length,
        minX, minZ, maxX, maxZ, baseY, heightRange,
        cellX: bucket.px, cellZ: bucket.pz,
        flags: chunkIndex > 0 ? 1 : 0, pageIndex: pages.length,
        lodMask: 0, reserved: 0,
        stableLo: (pages.length * 2654435761) >>> 0, stableHi: 0,
      };
      let lodMask = 0;
      for (const instance of chunk) {
        const record = quantizeInstance(instance, page, speciesIndex.get(instance.species));
        records.push(record);
        lodMask |= 1 << Math.min(3, instance.lodBias);
      }
      page.lodMask = lodMask;
      pages.push(page);
    });
  }

  const spatial = [];
  const byCell = new Map();
  pages.forEach((page, index) => {
    const key = `${page.cellZ},${page.cellX}`;
    let entry = byCell.get(key);
    if (!entry) byCell.set(key, entry = { cellX: page.cellX, cellZ: page.cellZ, firstPage: index, pageCount: 0 });
    entry.pageCount += 1;
  });
  for (const key of [...byCell.keys()].sort((a, b) => {
    const [az, ax] = a.split(',').map(Number), [bz, bx] = b.split(',').map(Number);
    return (az - bz) || (ax - bx);
  })) spatial.push(byCell.get(key));

  const { buffer: instanceBytes } = packRecords(records, INSTANCE_LAYOUT);
  const { buffer: pageBytes } = packRecords(pages, PAGE_LAYOUT);
  const { buffer: spatialBytes } = packRecords(spatial, SPATIAL_LAYOUT);

  const perSpecies = {};
  for (const [id, count] of scattered.perSpecies) perSpecies[id] = count;
  const pagesByLod = [0, 0, 0, 0];
  for (const page of pages) for (let level = 0; level < 4; ++level) if (page.lodMask & (1 << level)) pagesByLod[level] += 1;

  const meta = {
    schema: REGION_COOK_SCHEMA,
    id: parsed.id, seed: parsed.seed,
    bounds: parsed.bounds,
    cellSizeMetres: parsed.cellSizeMetres,
    pageCellMetres: parsed.pageCellMetres,
    pageSize: parsed.pageSize,
    surface: { kind: sampler.kind, version: sampler.version },
    species: species.map((entry) => ({
      id: entry.id, digest: entry.hash, kind: entry.meta.kind,
      lodTriangles: entry.meta.lod.map((lod) => lod.triangles),
      lodErrorMetres: entry.meta.lod.map((lod) => lod.worldErrorMetres),
      heightMetres: entry.meta.lod[0].heightMetres,
      radiusMetres: entry.meta.lod[0].radiusMetres,
    })),
    counts: {
      worldInstances: instances.length,
      pages: pages.length,
      instancesPerPageMax: pages.reduce((max, page) => Math.max(max, page.instanceCount), 0),
      cellsX: scattered.cellsX, cellsZ: scattered.cellsZ,
      perSpecies, pagesByLod,
      // Every rejection is counted, by reason. "The field is sparse" is not a diagnosis; "the
      // slope mask rejected 412 and the road exclusion rejected 96" is.
      rejected: {
        surface: scattered.rejectedBySurface,
        mask: scattered.rejectedByMask,
        exclusion: scattered.rejectedByExclusion,
        spacing: scattered.rejectedBySpacing,
      },
      spacingScope: scattered.spacingScope,
      // The packed id is 32 bits. Stated in the package so a tool that needs more knows to look
      // elsewhere instead of assuming a wider field.
      stableIdBits: 32,
    },
    density: {
      areaHectares: parsed.areaMetres2 / 10000,
      instancesPerHectare: instances.length / (parsed.areaMetres2 / 10000),
      bySpecies: Object.fromEntries(Object.entries(perSpecies).map(([id, count]) => [id, count / (parsed.areaMetres2 / 10000)])),
    },
    quantization: {
      instanceBytes: INSTANCE_BYTES,
      positionBits: 16, heightBits: 16, yawBits: 16, tiltBits: 15, scaleBits: 8,
      // Worst-case decode error for the widest page in this region, in metres.
      positionErrorMetres: Math.max(...pages.map((page) => Math.max(page.maxX - page.minX, page.maxZ - page.minZ))) / 65535,
      heightErrorMetres: Math.max(...pages.map((page) => page.heightRange)) / 65535,
      yawErrorRadians: (Math.PI * 2) / 65536,
      scaleErrorRelative: Math.pow(2, 8 / 255) - 1,
    },
    bytes: {
      instances: instanceBytes.byteLength,
      pages: pageBytes.byteLength,
      spatial: spatialBytes.byteLength,
      total: instanceBytes.byteLength + pageBytes.byteLength + spatialBytes.byteLength,
      perInstance: INSTANCE_BYTES,
    },
    // Explicit: this region has no aggregate/voxel representation. The loader must
    // reject a page that claims one.
    representations: ['mesh', 'impostor'],
    scatterSchema: SCATTER_SCHEMA,
  };

  const bytes = buildPackage({
    schema: REGION_COOK_SCHEMA, meta, sections: [
      { kind: SectionKind.INSTANCE_PACKED, index: 0, data: instanceBytes },
      { kind: SectionKind.PAGE, index: 0, data: pageBytes },
      { kind: SectionKind.SPATIAL, index: 0, data: spatialBytes },
    ],
  });
  return {
    id: parsed.id, meta, bytes, hash: digestPackage(bytes),
    instances: instances.length, pages: pages.length,
    records, pageRecords: pages,
    instanceBytes: instanceBytes.byteLength,
  };
}

// Capacity maths without materialising the world index. A 30M-instance region is
// 960 MB of packed records; a tool that "checks" it must not need that much RAM to
// answer whether the layout is large enough.
export function regionCapacity({ worldInstances, pageSize = 8192, pageCellMetres = 32 }) {
  fail(Number.isSafeInteger(worldInstances) && worldInstances >= 0 && worldInstances <= 4_000_000_000, 'NG-E-VEG-CAPACITY', 'worldInstances out of range');
  const pages = Math.ceil(worldInstances / pageSize);
  const instanceBytes = worldInstances * INSTANCE_BYTES;
  const pageBytes = pages * PAGE_BYTES;
  return {
    worldInstances, pages, pageCellMetres,
    instanceBytes, pageBytes,
    totalBytes: instanceBytes + pageBytes,
    // Decimal MB, per the plan's accounting rule: 30M x 32 B = 960 MB, not 915 MiB.
    decimalMegabytes: (instanceBytes + pageBytes) / 1_000_000,
    fitsInVegetationBudget: (instanceBytes + pageBytes) / 1_000_000 <= 1200,
  };
}

export function regionDigest(region) { return sha256Hex(canonicalJson(region)); }
export { mix32 };
