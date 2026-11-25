// Material contract.
//
// One record describes what a vegetation surface *is*, and every layer reads that record:
// the source validator, the cook metadata, the native loader, the shader constant block, the
// editor panel and the debug views. A field that exists in only one of those layers is the
// defect this module is meant to prevent — the old code had `roughness` in the species file,
// a `roughness` texture role that was really a height map, and a shader that ignored both and
// used `r * 0.85 + 0.2`.
//
// Channel semantics are fixed here and nowhere else:
//   baseColor  sRGB RGB, alpha may carry coverage; no baked key light
//   normal     linear tangent-space RGB, explicit convention, scale applied once
//   orm        linear, R = ambient occlusion, G = roughness, B = metallic (plants: 0)
//   height     linear relative height, scaled by heightScaleMetres, parallax only
//   thickness  linear relative thickness, scaled by thicknessScaleMetres, transmission only
//   opacity    linear coverage, white keeps, black cuts, threshold = alphaCutoff

export const MATERIAL_SCHEMA = 'nativegpu.vegetation-material/1';

const KINDS = new Set(['bark', 'leaf', 'blade']);

function fail(message) {
  const error = new Error(message);
  error.code = 'NG-E-VEG-MATERIAL';
  throw error;
}

const number = (value, low, high, label) => {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < low || value > high) {
    fail(`${label} must be a number in ${low}..${high}`);
  }
  return value;
};

// Defaults are per kind, because a leaf and a trunk do not share a thickness or a roughness.
const DEFAULTS = {
  bark: { roughnessFactor: 0.85, thicknessScaleMetres: 0.02, transmission: [0.05, 0.04, 0.02], alphaMode: 'opaque', normalScale: 1, heightScaleMetres: 0.006 },
  leaf: { roughnessFactor: 0.55, thicknessScaleMetres: 0.0004, transmission: [0.24, 0.44, 0.11], alphaMode: 'mask', normalScale: 1, heightScaleMetres: 0 },
  blade: { roughnessFactor: 0.48, thicknessScaleMetres: 0.0003, transmission: [0.3, 0.55, 0.14], alphaMode: 'mask', normalScale: 1, heightScaleMetres: 0 },
};

export function normalizeMaterial(material, index = 0) {
  if (!material || typeof material !== 'object') fail(`materials[${index}] must be an object`);
  const kind = material.kind;
  if (!KINDS.has(kind)) fail(`materials[${index}].kind must be bark, leaf or blade`);
  const defaults = DEFAULTS[kind];
  const alphaMode = material.alphaMode ?? defaults.alphaMode;
  if (alphaMode !== 'opaque' && alphaMode !== 'mask') fail(`materials[${index}].alphaMode must be opaque or mask`);
  if (kind === 'bark' && alphaMode !== 'opaque') fail(`materials[${index}]: bark cannot be alpha masked`);
  if (kind !== 'bark' && alphaMode !== 'mask') fail(`materials[${index}]: ${kind} must be alpha masked`);
  // Plants have no metal. A metallic factor here would be a data error, not a taste choice.
  const metallicFactor = material.metallicFactor ?? 0;
  if (metallicFactor !== 0) fail(`materials[${index}].metallicFactor must be 0 for vegetation`);
  const normalConvention = material.normalConvention ?? 'directX-';
  if (normalConvention !== 'directX-' && normalConvention !== 'openGL+') {
    fail(`materials[${index}].normalConvention must be directX- or openGL+`);
  }
  return {
    schema: MATERIAL_SCHEMA,
    id: material.id,
    kind,
    baseColorFactor: material.baseColorFactor ?? [1, 1, 1],
    roughnessFactor: number(material.roughnessFactor ?? material.roughness ?? defaults.roughnessFactor, 0.02, 1, `materials[${index}].roughnessFactor`),
    metallicFactor: 0,
    normalScale: number(material.normalScale ?? defaults.normalScale, 0, 4, `materials[${index}].normalScale`),
    normalConvention,
    alphaMode,
    alphaCutoff: number(material.alphaCutoff ?? 0.35, 0, 1, `materials[${index}].alphaCutoff`),
    thicknessScaleMetres: number(material.thicknessScaleMetres ?? material.thicknessMetres ?? defaults.thicknessScaleMetres, 1e-6, 0.5, `materials[${index}].thicknessScaleMetres`),
    absorptionPerMetre: number(material.absorptionPerMetre ?? 900, 0, 20000, `materials[${index}].absorptionPerMetre`),
    transmission: material.transmission ?? defaults.transmission,
    heightScaleMetres: number(material.heightScaleMetres ?? defaults.heightScaleMetres, 0, 0.2, `materials[${index}].heightScaleMetres`),
    occlusionStrength: number(material.occlusionStrength ?? 1, 0, 1, `materials[${index}].occlusionStrength`),
    uvTileMetres: number(material.uvTileMetres ?? 1.6, 0.01, 100, `materials[${index}].uvTileMetres`),
    uvSet: 0,
    sampler: 'wrap-linear',
    doubleSided: material.doubleSided ?? kind !== 'bark',
    alphaMasked: alphaMode === 'mask',
  };
}

// Which texture slot feeds which shader input, and through which channel. This table is the
// single source for the report and for the debug views' labels.
export const SLOT_CONSUMERS = [
  { slot: 'baseColor', channel: 'RGB', shader: 'albedo', colorSpace: 'srgb' },
  { slot: 'baseColor', channel: 'A', shader: 'coverage fallback', colorSpace: 'linear' },
  { slot: 'normal', channel: 'RGB', shader: 'tangent-space normal', colorSpace: 'linear' },
  { slot: 'orm', channel: 'R', shader: 'ambient occlusion (indirect only)', colorSpace: 'linear' },
  { slot: 'orm', channel: 'G', shader: 'roughness', colorSpace: 'linear' },
  { slot: 'orm', channel: 'B', shader: 'metallic (must be 0)', colorSpace: 'linear' },
  { slot: 'height', channel: 'R', shader: 'bark parallax offset', colorSpace: 'linear' },
  { slot: 'thickness', channel: 'R', shader: 'transmission optical depth', colorSpace: 'linear' },
  { slot: 'opacity', channel: 'R', shader: 'alpha cutout', colorSpace: 'linear' },
];

// Produces the mapping table for a report: source file, semantic, channel, digest and the
// shader input that consumes it. Missing slots are reported as null, never as "ok".
export function describeTextureBindings(textures) {
  if (!textures) return [];
  const rows = [];
  for (const group of ['bark', 'leaf']) {
    const entry = textures[group];
    if (!entry) continue;
    for (const slot of ['baseColor', 'normal', 'orm', 'height', 'thickness', 'opacity']) {
      const role = entry[slot];
      if (!role) continue;
      const consumers = SLOT_CONSUMERS.filter((item) => item.slot === slot);
      for (const consumer of consumers) {
        rows.push({
          group, slot, role: role.role, semantic: role.semantic ?? null,
          colorSpace: role.colorSpace ?? null, digest: role.digest,
          source: role.source ?? null, channel: consumer.channel, shader: consumer.shader,
          resolution: role.width && role.height ? `${role.width}x${role.height}` : null,
          mips: role.mipCount ?? null,
        });
      }
    }
  }
  return rows;
}
