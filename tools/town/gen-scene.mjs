#!/usr/bin/env node
// Builds .screen/town.scene.json from a compact layout description.
//
// The town is deliberately box-only: the renderer's unit cube is the only mesh, and the
// material pipeline supplies the detail. Keeping the layout in code makes the scene
// reproducible and lets the same material names drive the texture cook.
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const MATERIALS = {
  default: { albedo: 'flat', normal: 'flat', orm: 'flat', uvScale: 1.0, roughness: 0.8, metalness: 0.0, wetness: 0.0, wear: 0.0 },
  brick_red: { albedo: 'brick_red', normal: 'brick_red', orm: 'brick_red', uvScale: 2.2, roughness: 1.0, metalness: 1.0, wetness: 0.0, wear: 0.16 },
  brick_grey: { albedo: 'brick_grey', normal: 'brick_grey', orm: 'brick_grey', uvScale: 2.4, roughness: 1.0, metalness: 1.0, wetness: 0.05, wear: 0.12 },
  plaster: { albedo: 'plaster', normal: 'plaster', orm: 'plaster', uvScale: 2.6, roughness: 1.0, metalness: 1.0, wetness: 0.0, wear: 0.20 },
  roof_tile: { albedo: 'roof_tile', normal: 'roof_tile', orm: 'roof_tile', uvScale: 1.6, roughness: 1.0, metalness: 1.0, wetness: 0.12, wear: 0.10 },
  wood: { albedo: 'wood', normal: 'wood', orm: 'wood', uvScale: 1.2, roughness: 1.0, metalness: 1.0, wetness: 0.05, wear: 0.18 },
  metal: { albedo: 'metal', normal: 'metal', orm: 'metal', uvScale: 1.5, roughness: 1.0, metalness: 1.0, wetness: 0.0, wear: 0.25 },
  cobble: { albedo: 'cobble', normal: 'cobble', orm: 'cobble', uvScale: 2.0, roughness: 1.0, metalness: 1.0, wetness: 0.35, wear: 0.10 },
  stone: { albedo: 'stone', normal: 'stone', orm: 'stone', uvScale: 2.2, roughness: 1.0, metalness: 1.0, wetness: 0.20, wear: 0.10 },
  grass: { albedo: 'grass', normal: 'grass', orm: 'grass', uvScale: 3.0, roughness: 1.0, metalness: 1.0, wetness: 0.10, wear: 0.0 },
  water: { albedo: 'water', normal: 'water', orm: 'water', uvScale: 2.5, roughness: 1.0, metalness: 1.0, wetness: 1.0, wear: 0.0 },
  window: { albedo: 'window', normal: 'window', orm: 'window', uvScale: 1.4, roughness: 1.0, metalness: 1.0, wetness: 0.0, wear: 0.0 },
  foliage: { albedo: 'foliage', normal: 'foliage', orm: 'foliage', uvScale: 1.8, roughness: 1.0, metalness: 1.0, wetness: 0.15, wear: 0.0 },
  lamp: { albedo: 'lamp', normal: 'flat', orm: 'lamp', uvScale: 0.8, roughness: 1.0, metalness: 1.0, wetness: 0.0, wear: 0.0 },
  awning: { albedo: 'awning', normal: 'awning', orm: 'awning', uvScale: 1.4, roughness: 1.0, metalness: 1.0, wetness: 0.10, wear: 0.0 },
  glass: { albedo: 'flat', normal: 'flat', orm: 'flat', uvScale: 1.0, roughness: 0.12, metalness: 0.0, wetness: 0.0, wear: 0.0 },
};

const entities = [];
let serial = 0;
function id() {
  serial += 1;
  return `b1000000-0000-4000-8000-${serial.toString(16).padStart(12, '0')}`;
}

function add(name, kind, position, scale, material, options = {}) {
  const entity = {
    id: id(),
    name,
    kind,
    position,
    scale,
    color: options.color ?? [1, 1, 1],
    material,
    collider: options.collider ?? false,
  };
  if (options.light) entity.light = options.light;
  if (options.vehicle) entity.vehicle = options.vehicle;
  entities.push(entity);
  return entity;
}

function box(name, material, x, y, z, sx, sy, sz, options = {}) {
  return add(name, options.kind ?? 'prop', [x, y, z], [sx, sy, sz], material, options);
}

// --- ground, street and sidewalks -------------------------------------------------
add('Town meadow', 'ground', [0, -0.3, 4], [96, 0.6, 96], 'grass', { collider: true });
add('Cobbled high street', 'ground', [0, 0.02, 2], [9, 0.08, 78], 'cobble');
add('West sidewalk', 'ground', [-6, 0.06, 2], [3, 0.12, 78], 'stone');
add('East sidewalk', 'ground', [6, 0.06, 2], [3, 0.12, 78], 'stone');
add('Market square', 'ground', [0, 0.07, 33], [24, 0.14, 16], 'stone');

// --- buildings ---------------------------------------------------------------------
// Each lot is 9 m wide along the street and 9 m deep. The facade sits at |x| = 7.5, so
// the camera can see both rows from a three-quarter angle.
const buildings = [
  { side: -1, z: -26, width: 9, height: 7.0, wall: 'brick_red', roof: 'roof_tile', floors: 2, awning: true },
  { side: -1, z: -16, width: 9, height: 8.6, wall: 'plaster', roof: 'roof_tile', floors: 3, awning: false },
  { side: -1, z: -6, width: 9, height: 12.0, wall: 'brick_grey', roof: 'stone', floors: 4, awning: true, tower: true },
  { side: -1, z: 4, width: 9, height: 6.4, wall: 'brick_red', roof: 'roof_tile', floors: 2, awning: true },
  { side: -1, z: 14, width: 9, height: 9.2, wall: 'plaster', roof: 'roof_tile', floors: 3, awning: false },
  { side: 1, z: -26, width: 9, height: 9.0, wall: 'plaster', roof: 'roof_tile', floors: 3, awning: false },
  { side: 1, z: -16, width: 9, height: 6.8, wall: 'brick_red', roof: 'roof_tile', floors: 2, awning: true },
  { side: 1, z: -6, width: 9, height: 10.2, wall: 'brick_grey', roof: 'roof_tile', floors: 3, awning: false },
  { side: 1, z: 4, width: 9, height: 7.6, wall: 'plaster', roof: 'stone', floors: 2, awning: true },
  { side: 1, z: 14, width: 9, height: 8.8, wall: 'brick_red', roof: 'roof_tile', floors: 3, awning: false },
];

for (const building of buildings) {
  const direction = building.side;
  const centerX = direction * 12;
  const facadeX = direction * 7.5;
  const label = `${direction < 0 ? 'West' : 'East'} ${building.z < 0 ? 'north' : 'south'} building`;
  box(`${label} body`, building.wall, centerX, building.height / 2, building.z,
    building.width, building.height, building.width, { kind: 'wall', collider: true });
  box(`${label} roof`, building.roof, centerX, building.height + 0.2, building.z,
    building.width + 0.9, 0.4, building.width + 0.9);
  box(`${label} plinth`, 'stone', centerX, 0.25, building.z,
    building.width + 0.3, 0.5, building.width + 0.3);

  // Door and canopy face the street.
  box(`${label} door`, 'wood', facadeX - direction * 0.07, 1.2, building.z,
    0.14, 2.4, 1.4, { collider: false });
  box(`${label} lintel`, 'stone', facadeX - direction * 0.1, 2.55, building.z,
    0.22, 0.18, 1.9);
  // Dark shop glass gives the night street a second reflective surface and exercises the
  // glass material independently of the emissive window plates.
  box(`${label} shop glass`, 'glass', facadeX - direction * 0.05, 1.45,
    building.z + 3.0, 0.08, 1.9, 1.8, { color: [0.04, 0.07, 0.10] });
  if (building.awning) {
    box(`${label} awning`, 'awning', facadeX - direction * 0.55, 3.0, building.z,
      1.0, 0.1, 3.4);
    box(`${label} awning rail`, 'metal', facadeX - direction * 1.02, 2.9, building.z,
      0.06, 0.12, 3.4);
  }

  // Window grid on the street facade, one row per floor above the ground floor.
  const columns = 3;
  for (let floor = 0; floor < building.floors; floor += 1) {
    const y = 2.1 + floor * 2.15;
    if (y + 0.7 > building.height) break;
    for (let column = 0; column < columns; column += 1) {
      const z = building.z + (column - 1) * 2.45;
      box(`${label} window ${floor}-${column}`, 'window',
        facadeX - direction * 0.06, y, z, 0.12, 1.15, 0.95);
      box(`${label} sill ${floor}-${column}`, 'stone',
        facadeX - direction * 0.12, y - 0.68, z, 0.22, 0.14, 1.15);
    }
  }

  if (building.tower) {
    box(`${label} clock face`, 'lamp', facadeX - direction * 0.1, building.height - 1.6,
      building.z, 0.12, 1.5, 1.5);
    box(`${label} clock ring`, 'metal', facadeX - direction * 0.05, building.height - 1.6,
      building.z, 0.06, 1.9, 1.9);
    box(`${label} spire`, 'roof_tile', centerX, building.height + 1.4, building.z,
      4.2, 2.4, 4.2);
    box(`${label} spire tip`, 'metal', centerX, building.height + 2.9, building.z,
      0.18, 1.0, 0.18);
  } else {
    box(`${label} chimney`, 'brick_grey', centerX + 2.2, building.height + 1.1,
      building.z - 2.2, 0.8, 1.8, 0.8);
    box(`${label} chimney cap`, 'stone', centerX + 2.2, building.height + 2.05,
      building.z - 2.2, 1.0, 0.16, 1.0);
  }

  // Side elevations get their own window row so a three-quarter camera does not stare
  // at a blank gable end.
  for (let floor = 0; floor < building.floors; floor += 1) {
    const y = 2.1 + floor * 2.15;
    if (y + 0.7 > building.height) break;
    for (const sideSign of [-1, 1]) {
      const zFace = building.z + sideSign * (building.width / 2 + 0.06);
      for (const column of [-1, 1]) {
        box(`${label} side window ${floor}-${sideSign}-${column}`, 'window',
          centerX + column * 2.4, y, zFace, 0.95, 1.15, 0.12);
        box(`${label} side sill ${floor}-${sideSign}-${column}`, 'stone',
          centerX + column * 2.4, y - 0.68, zFace + sideSign * 0.05, 1.15, 0.14, 0.2);
      }
    }
  }
}

// --- market square -----------------------------------------------------------------
box('Fountain base', 'stone', 0, 0.35, 33, 4.6, 0.7, 4.6, { collider: true });
box('Fountain water', 'water', 0, 0.78, 33, 3.9, 0.16, 3.9);
box('Fountain column', 'stone', 0, 1.5, 33, 0.9, 2.0, 0.9);
box('Fountain bowl', 'stone', 0, 2.55, 33, 2.2, 0.24, 2.2);
box('Fountain cap', 'water', 0, 2.72, 33, 1.9, 0.1, 1.9);

// Market stall: four posts, a striped canopy and a counter.
for (const x of [-7.2, -5.2]) {
  for (const z of [38, 41]) {
    box('Stall post', 'wood', x, 1.1, z, 0.14, 2.2, 0.14, { collider: true });
  }
}
box('Stall canopy', 'awning', -6.2, 2.25, 39.5, 2.6, 0.12, 3.4);
box('Stall counter', 'wood', -6.2, 0.85, 39.5, 2.2, 0.14, 2.8, { collider: true });
box('Stall crate', 'wood', -6.2, 0.45, 38.2, 0.8, 0.8, 0.8);

for (const side of [-1, 1]) {
  for (const offset of [-1, 1]) {
    const x = side * 9.5;
    const z = 33 + offset * 5.5;
    box('Bench seat', 'wood', x, 0.62, z, 0.55, 0.16, 3.0, { collider: true });
    box('Bench back', 'wood', x + side * 0.26, 0.95, z, 0.1, 0.55, 3.0);
    for (const leg of [-1, 1]) {
      box('Bench leg', 'metal', x, 0.3, z + leg * 1.1, 0.4, 0.6, 0.14);
    }
  }
}

// --- trees, lamps, street furniture ------------------------------------------------
for (const side of [-1, 1]) {
  for (const z of [-24, -16, -8, 0, 8, 16, 24]) {
    const x = side * 6.9;
    box('Tree trunk', 'wood', x, 1.35, z, 0.34, 2.7, 0.34, { collider: true });
    box('Tree crown lower', 'foliage', x, 3.5, z, 2.4, 1.9, 2.4, { color: [0.42, 0.52, 0.38] });
    box('Tree crown upper', 'foliage', x + 0.2, 4.7, z - 0.1, 1.5, 1.3, 1.5, { color: [0.48, 0.58, 0.42] });
  }
}

const lampPositions = [
  [-5.3, -21, false], [5.3, -21, false],
  [-5.3, -11, true], [5.3, -11, false],
  [-5.3, -1, false], [5.3, -1, true],
  [-5.3, 9, false], [5.3, 9, false],
  [-5.3, 19, true], [5.3, 19, false],
  [-5.3, 29, false], [5.3, 29, true],
];
for (const [x, z, isLight] of lampPositions) {
  box('Lamp post', 'metal', x, 2.2, z, 0.14, 4.4, 0.14, { collider: true });
  box('Lamp base', 'stone', x, 0.2, z, 0.4, 0.4, 0.4);
  if (isLight) {
    add('Street lamp', 'light', [x, 4.45, z], [0.42, 0.42, 0.42], 'lamp', {
      light: { color: [1.0, 0.78, 0.48], intensity: 1.1, range: 9 },
    });
  } else {
    box('Lamp head', 'lamp', x, 4.45, z, 0.42, 0.42, 0.42);
  }
}

// A few props keep the street from reading as an empty corridor.
for (const [x, z] of [[-6.4, 6.5], [6.4, -3.5], [-6.4, 20.5], [6.4, 22.5]]) {
  box('Planter box', 'stone', x, 0.35, z, 1.1, 0.7, 1.1, { collider: true });
  box('Planter shrub', 'foliage', x, 1.05, z, 0.95, 0.8, 0.95, { color: [0.42, 0.52, 0.38] });
}
for (const [x, z, size] of [[-6.2, -18.5, 1.0], [6.2, 11.5, 1.2], [-6.2, 12.5, 0.9]]) {
  box('Crate', 'wood', x, size / 2 + 0.12, z, size, size, size, { collider: true });
}

// --- gate arch at the north end ----------------------------------------------------
for (const side of [-1, 1]) {
  box('Gate pillar', 'brick_grey', side * 4.2, 2.2, -32, 1.2, 4.4, 1.2, { kind: 'wall', collider: true });
  box('Gate pillar cap', 'stone', side * 4.2, 4.5, -32, 1.5, 0.3, 1.5);
}
box('Gate beam', 'wood', 0, 4.9, -32, 9.6, 0.5, 0.9);
box('Gate sign', 'awning', 0, 5.4, -32, 6.0, 0.7, 0.16);

// --- distant silhouette -----------------------------------------------------------
// Low, fog-blended masses break the horizon without becoming walkable geometry.
for (const [x, z, sx, sy, sz] of [
  [-34, -66, 30, 5, 14],
  [0, -72, 38, 8, 16],
  [34, -64, 26, 4, 12],
  [-52, -28, 16, 3, 34],
  [52, -22, 16, 4, 30],
]) {
  box('Distant hill', 'grass', x, sy / 2, z, sx, sy, sz);
}

// --- traffic -----------------------------------------------------------------------
// Cars loop along the two street lanes. Headlights/taillights are generated by Game from
// the vehicle entity itself, so the scene stays declarative: speed + direction only.
const cars = [
  { x: -2.3, z: -30, direction: 1, speed: 6.5, color: [0.72, 0.10, 0.08] },
  { x: -2.3, z: 2, direction: 1, speed: 5.2, color: [0.12, 0.28, 0.58] },
  { x: 2.3, z: -12, direction: -1, speed: 7.0, color: [0.70, 0.62, 0.40] },
  { x: 2.3, z: 22, direction: -1, speed: 5.8, color: [0.15, 0.42, 0.28] },
];
for (const [index, car] of cars.entries()) {
  add(`Traffic car ${index + 1}`, 'vehicle', [car.x, 0.72, car.z],
    [1.8, 1.2, 4.0], 'default', {
      color: car.color,
      vehicle: { speed: car.speed, direction: car.direction },
    });
}

// --- spawn -------------------------------------------------------------------------
add('Traveller spawn', 'player', [0, 0, 39], [0.6, 1.8, 0.6], 'default', {
  collider: true,
  color: [0.2, 0.8, 0.95],
});

function main() {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
  const output = join(root, 'assets', 'town.scene.json');
  mkdirSync(dirname(output), { recursive: true });
  const scene = {
    schemaVersion: 2,
    projectId: 'b1000000-0000-4000-8000-000000000001',
    name: '石桥小镇',
    revision: 0,
    settings: { walkSpeed: 3.4, runSpeed: 7.2, jumpSpeed: 7.0, gravity: 20 },
    lighting: {
      quality: 1,
      ambient: [0.015, 0.022, 0.045],
      skyColor: [0.004, 0.008, 0.018],
      keyDirection: [0.15, 0.38, -0.91],
      keyColor: [0.40, 0.52, 0.90],
      keyIntensity: 0.08,
      fogColor: [0.065, 0.085, 0.125],
      fogDensity: 0.010,
      fogHeightFalloff: 0.040,
      fogBaseHeight: 0.0,
      toneMap: true,
      exposure: 1.45,
      cloudCoverage: 0.55,
      cloudHeight: 92.0,
      cloudThickness: 36.0,
      cloudSpeed: 0.02,
    },
    materials: MATERIALS,
    entities,
  };
  writeFileSync(output, `${JSON.stringify(scene, null, 2)}\n`);
  console.log(`wrote ${entities.length} entities / ${Object.keys(MATERIALS).length} materials -> ${output}`);
}

main();
