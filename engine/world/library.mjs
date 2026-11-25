// Default component library: the ancient-Chinese kit the first sample world is built from.
//
// Every component here is procedural geometry (`geometry.shape`) rather than an imported
// asset, for one reason: a world must be cookable and playable on a machine where nobody
// has imported a single mesh yet. The editor shows `stand-in` for these and the moment an
// imported asset is attached the same component id keeps its place in every saved world.
//
// Sizes are metres. Heights are `sizeMetres[1]`; the pivot is the ground contact point, so
// placing a component is `position + (0, 0, 0)` on the terrain height.

import { LIBRARY_SCHEMA, COMPONENT_CATEGORIES } from './schema.mjs';

function component(id, category, size, extra = {}) {
  return {
    id,
    category,
    sizeMetres: size,
    axis: '+Y',
    pivot: 'base',
    styleTags: ['ancient'],
    scaleRange: [0.85, 1.15],
    ...extra,
  };
}

export const DEFAULT_COMPONENTS = Object.freeze([
  // ---------------------------------------------------------------- ground cover
  component('grass-tuft', 'grass', [0.45, 0.35, 0.45], {
    geometry: { shape: 'cross', sides: 3 },
    material: { albedo: [0.31, 0.42, 0.19], roughness: 0.9, metalness: 0 },
    placement: { maxSlopeDegrees: 34, densityPerHectare: 26000, alignToSlope: true, exclusionRadiusMetres: 0.1 },
    styleTags: ['ancient', 'meadow'],
  }),
  component('fern-clump', 'ground-cover', [0.8, 0.55, 0.8], {
    geometry: { shape: 'cross', sides: 4 },
    material: { albedo: [0.22, 0.34, 0.16], roughness: 0.88, metalness: 0 },
    placement: { maxSlopeDegrees: 30, densityPerHectare: 5200, alignToSlope: true },
    styleTags: ['ancient', 'shade'],
  }),

  // ---------------------------------------------------------------- trees
  component('pine-ancient', 'tree', [3.2, 11.5, 3.2], {
    geometry: { shape: 'cone', sides: 8, taper: 0.18 },
    material: { albedo: [0.16, 0.27, 0.15], roughness: 0.82, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [0.22, 5.75, 0.22] },
    placement: { maxSlopeDegrees: 38, minSpacingMetres: 4.6, densityPerHectare: 210, alignToSlope: false, exclusionRadiusMetres: 1.2 },
    markers: { groundContact: [0, 0, 0], forward: [0, 0, 1] },
    styleTags: ['ancient', 'mountain'],
  }),
  component('maple-broadleaf', 'tree', [5.4, 9.0, 5.4], {
    geometry: { shape: 'sphere', segments: 10, taper: 0.7 },
    material: { albedo: [0.29, 0.36, 0.16], roughness: 0.8, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [0.3, 4.5, 0.3] },
    placement: { maxSlopeDegrees: 26, minSpacingMetres: 6.2, densityPerHectare: 130, alignToSlope: false, exclusionRadiusMetres: 1.6 },
    markers: { groundContact: [0, 0, 0], forward: [0, 0, 1] },
    styleTags: ['ancient', 'valley'],
  }),
  component('willow-river', 'tree', [4.6, 7.2, 4.6], {
    geometry: { shape: 'sphere', segments: 9, taper: 0.55 },
    material: { albedo: [0.33, 0.42, 0.19], roughness: 0.86, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [0.28, 3.6, 0.28] },
    placement: { maxSlopeDegrees: 16, minSpacingMetres: 5.4, densityPerHectare: 90, alignToSlope: false, exclusionRadiusMetres: 1.4 },
    styleTags: ['ancient', 'riverbank'],
  }),

  // ---------------------------------------------------------------- rocks
  component('granite-boulder', 'rock', [2.4, 1.6, 2.2], {
    geometry: { shape: 'sphere', segments: 7, taper: 0.82 },
    material: { albedo: [0.42, 0.41, 0.39], roughness: 0.94, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [1.2, 0.8, 1.1] },
    placement: { maxSlopeDegrees: 42, minSpacingMetres: 3.4, densityPerHectare: 48, alignToSlope: true, exclusionRadiusMetres: 1.0 },
    styleTags: ['ancient', 'mountain'],
  }),
  component('river-stone', 'rock', [0.7, 0.4, 0.6], {
    geometry: { shape: 'sphere', segments: 5, taper: 0.7 },
    material: { albedo: [0.5, 0.49, 0.46], roughness: 0.9, metalness: 0 },
    collision: { mode: 'none' },
    placement: { maxSlopeDegrees: 20, minSpacingMetres: 1.2, densityPerHectare: 620, alignToSlope: true },
    styleTags: ['ancient', 'riverbank'],
  }),

  // ---------------------------------------------------------------- roads
  component('stone-road', 'road-piece', [3.0, 0.12, 3.0], {
    geometry: { shape: 'box' },
    material: { albedo: [0.46, 0.44, 0.4], roughness: 0.88, metalness: 0 },
    collision: { mode: 'terrain' },
    styleTags: ['ancient', 'road'],
  }),
  component('dirt-road', 'road-piece', [3.2, 0.08, 3.2], {
    geometry: { shape: 'box' },
    material: { albedo: [0.38, 0.31, 0.22], roughness: 0.95, metalness: 0 },
    collision: { mode: 'terrain' },
    styleTags: ['ancient', 'road'],
  }),

  // ---------------------------------------------------------------- bridges
  component('timber-bridge', 'bridge', [4.0, 2.6, 12.0], {
    geometry: { shape: 'box' },
    material: { albedo: [0.33, 0.24, 0.16], roughness: 0.86, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [2.0, 0.25, 6.0] },
    markers: {
      walkable: [{ position: [0, 0.3, 0], direction: [0, 0, 1], radiusMetres: 1.8 }],
      connections: [
        { position: [0, 0.2, -6], direction: [0, 0, -1], radiusMetres: 1.8 },
        { position: [0, 0.2, 6], direction: [0, 0, 1], radiusMetres: 1.8 },
      ],
    },
    styleTags: ['ancient', 'bridge'],
  }),

  // ---------------------------------------------------------------- buildings
  component('courtyard-house', 'building-prefab', [11.0, 6.4, 9.0], {
    geometry: { shape: 'box' },
    material: { albedo: [0.72, 0.69, 0.62], roughness: 0.85, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [5.5, 3.2, 4.5] },
    markers: {
      groundContact: [0, 0, 0],
      forward: [0, 0, 1],
      entrances: [{ position: [0, 0, 4.5], direction: [0, 0, 1], radiusMetres: 1.6, level: 0 }],
      occupied: [{ position: [0, 0, 0], radiusMetres: 7.2 }],
      walkable: [{ position: [0, 0, 5.6], radiusMetres: 1.4 }],
    },
    placement: { requiresFlat: true, maxSlopeDegrees: 8 },
    styleTags: ['ancient', 'residential'],
  }),
  component('two-storey-inn', 'building-prefab', [16.0, 9.2, 12.0], {
    geometry: { shape: 'box' },
    material: { albedo: [0.68, 0.6, 0.5], roughness: 0.84, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [8.0, 4.6, 6.0] },
    markers: {
      groundContact: [0, 0, 0],
      forward: [0, 0, 1],
      entrances: [{ position: [0, 0, 6.0], direction: [0, 0, 1], radiusMetres: 2.0, level: 0 }],
      occupied: [{ position: [0, 0, 0], radiusMetres: 10.4 }],
      stairs: [
        { position: [4.5, 0, 0], direction: [0, 1, 0], level: 0 },
        { position: [4.5, 4.6, 0], direction: [0, 1, 0], level: 1 },
      ],
    },
    placement: { requiresFlat: true, maxSlopeDegrees: 6 },
    styleTags: ['ancient', 'inn', 'two-storey'],
    notes: 'Stand-in shell. The full interior (hall, counter, dining, kitchen, six guest rooms) ships with the building phase; this footprint only reserves the plot and the entrance.',
  }),
  component('wall-segment', 'building-module', [6.0, 3.0, 0.5], {
    geometry: { shape: 'box' },
    material: { albedo: [0.7, 0.68, 0.62], roughness: 0.86, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [3.0, 1.5, 0.25] },
    styleTags: ['ancient', 'module'],
  }),
  component('gate-arch', 'building-module', [7.0, 6.0, 1.6], {
    geometry: { shape: 'prism', sides: 3, hollow: 1 },
    material: { albedo: [0.55, 0.35, 0.28], roughness: 0.8, metalness: 0 },
    collision: { mode: 'box', halfExtentsMetres: [3.5, 3.0, 0.8] },
    markers: {
      entrances: [{ position: [0, 0, 0], direction: [0, 0, 1], radiusMetres: 2.2 }],
      connections: [{ position: [0, 0, 1.6], direction: [0, 0, 1], radiusMetres: 1.6 }],
    },
    styleTags: ['ancient', 'module'],
  }),
]);

export function defaultLibrary(id = 'ancient-kit') {
  return {
    schema: LIBRARY_SCHEMA,
    id,
    name: 'Ancient Valley Kit',
    styleTags: ['ancient'],
    components: DEFAULT_COMPONENTS.map(entry => ({ ...entry })),
  };
}

export function componentById(library, componentId) {
  return library?.components?.find(entry => entry.id === componentId) ?? null;
}

export function componentsByCategory(library, category) {
  return (library?.components ?? []).filter(entry => entry.category === category);
}

// What the editor shows in the library panel. `standIn` is the honest label the plan asks
// for: a procedural box is not a finished building and must not be presented as one.
export function summarizeLibrary(library) {
  const components = library?.components ?? [];
  const byCategory = {};
  for (const category of COMPONENT_CATEGORIES) byCategory[category] = 0;
  let standIn = 0;
  let imported = 0;
  let missing = 0;
  for (const entry of components) {
    byCategory[entry.category] = (byCategory[entry.category] ?? 0) + 1;
    if (entry.missing) missing += 1;
    else if (entry.asset) imported += 1;
    else standIn += 1;
  }
  return {
    id: library?.id ?? null,
    total: components.length,
    byCategory,
    standIn,
    imported,
    missing,
    // The plan is explicit: the editor must label capabilities rather than call a shell a
    // finished building.
    interiorSupport: components.some(entry => entry.markers?.stairs?.length) ? 'reserved' : 'none',
  };
}

// Adapts a library into the shape the recipe references. Returns the reference entry and
// the payload the caller writes to `assets/world-libraries/<id>/library.json`.
export function adaptLibraryToRecipe(library, file, sha256) {
  return {
    reference: { id: library.id, file, sha256 },
    payload: { ...library, schema: LIBRARY_SCHEMA },
  };
}
