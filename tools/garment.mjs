import fs from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { canonicalJson, sha256Hex } from '../engine/assets/canonical.mjs';
import { loadCharacterAsset, axisAngleQuaternion } from '../engine/character/rig-mesh.mjs';
import { bindGarment } from '../engine/garment/validate.mjs';
import { GARMENT_TEST_POSES } from '../engine/garment/physics.mjs';
import { compileGarmentLibrary } from '../engine/garment/cook.mjs';
import { GarmentError } from '../engine/garment/inspect.mjs';

// Native garment fixtures: two fitted garments generated from the sample character's own
// cross-sections, cooked into the document the native solver consumes.
//
// These prove the native skinning contract — skin target per tick, dynamic blend,
// displacement limit, anchors, bone-driven collision and pose-set switching — on a mesh
// whose ground truth (body cross-section + 12 mm clearance) is measurable. They do not
// prove that an imported artist shirt binds correctly, and they say nothing about
// self-collision, which the solver still refuses to pretend it has.

const ROOT = path.resolve(import.meta.dirname, '..');
const FAIL = (ok, message) => { if (!ok) throw new GarmentError('NG-E-GARMENT-FIXTURE', message); };

const character = await loadCharacterAsset(path.join(ROOT, 'assets/builtin/robot/character.json'));
const boneIndex = (name) => {
  const bone = character.bones.find((entry) => entry.name === name || entry.key === name);
  FAIL(bone, `the sample character has no bone ${name}`);
  return bone.index;
};

function sectionOf(boneNames, y, halfHeight = 0.025) {
  const bones = new Set(boneNames.map(boneIndex));
  let centreX = 0, centreZ = 0, count = 0, radiusX = 0, radiusZ = 0;
  const points = [];
  for (let vertex = 0; vertex < character.vertices; ++vertex) {
    if (!bones.has(character.joints[vertex * 4]) || character.weights[vertex * 4] < 0.5) continue;
    if (Math.abs(character.positions[vertex * 3 + 1] - y) > halfHeight) continue;
    points.push([character.positions[vertex * 3], character.positions[vertex * 3 + 2]]);
    centreX += character.positions[vertex * 3];
    centreZ += character.positions[vertex * 3 + 2];
    ++count;
  }
  FAIL(count > 0, `no body vertices near y=${y} for ${boneNames.join('+')}`);
  centreX /= count; centreZ /= count;
  for (const [x, z] of points) {
    radiusX = Math.max(radiusX, Math.abs(x - centreX));
    radiusZ = Math.max(radiusZ, Math.abs(z - centreZ));
  }
  return { centreX, centreZ, radiusX, radiusZ };
}

// Open elliptical tube, wound outward. The winding fix is a signed-volume test because a
// garment whose normals face inward makes the membrane material frame left-handed and the
// native topology validator rejects the asset for the wrong reason.
function tube(stations, { segments = 24 } = {}) {
  const positions = [], indices = [];
  for (const station of stations) {
    for (let segment = 0; segment <= segments; ++segment) {
      const angle = (segment / segments) * Math.PI * 2;
      positions.push(
        station.centre[0] + Math.cos(angle) * station.radiusX,
        station.centre[1],
        station.centre[2] + Math.sin(angle) * station.radiusZ);
    }
  }
  const stride = segments + 1;
  for (let ring = 0; ring < stations.length - 1; ++ring) {
    for (let segment = 0; segment < segments; ++segment) {
      const a = ring * stride + segment, b = a + 1, c = a + stride, d = c + 1;
      indices.push(a, b, c, b, d, c);
    }
  }
  let volume = 0;
  for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
    const [i0, i1, i2] = [indices[triangle * 3], indices[triangle * 3 + 1], indices[triangle * 3 + 2]];
    const p0 = [positions[i0 * 3], positions[i0 * 3 + 1], positions[i0 * 3 + 2]];
    const p1 = [positions[i1 * 3], positions[i1 * 3 + 1], positions[i1 * 3 + 2]];
    const p2 = [positions[i2 * 3], positions[i2 * 3 + 1], positions[i2 * 3 + 2]];
    volume += (p0[0] * (p1[1] * p2[2] - p1[2] * p2[1])
      - p0[1] * (p1[0] * p2[2] - p1[2] * p2[0])
      + p0[2] * (p1[0] * p2[1] - p1[1] * p2[0])) / 6;
  }
  if (volume < 0) {
    for (let triangle = 0; triangle < indices.length / 3; ++triangle) {
      const swap = indices[triangle * 3 + 1];
      indices[triangle * 3 + 1] = indices[triangle * 3 + 2];
      indices[triangle * 3 + 2] = swap;
    }
  }
  return { positions: Float64Array.from(positions), indices: Uint32Array.from(indices) };
}

function fitted(boneNames, heights, clearanceMetres, segments = 24, halfHeight = 0.025) {
  return tube(heights.map((y) => {
    const section = sectionOf(boneNames, y, halfHeight);
    return {
      centre: [section.centreX, y, section.centreZ],
      radiusX: section.radiusX + clearanceMetres,
      radiusZ: section.radiusZ + clearanceMetres,
    };
  }), { segments });
}

function merge(...meshes) {
  const positions = [], indices = [];
  for (const mesh of meshes) {
    const offset = positions.length / 3;
    positions.push(...mesh.positions);
    indices.push(...mesh.indices.map((index) => index + offset));
  }
  return { positions: Float64Array.from(positions), indices: Uint32Array.from(indices) };
}

const TORSO = ['mixamorig:Hips', 'mixamorig:Spine', 'mixamorig:Spine1', 'mixamorig:Spine2'];
const LEFT_LEG = ['mixamorig:LeftUpLeg', 'mixamorig:LeftLeg'];
const RIGHT_LEG = ['mixamorig:RightUpLeg', 'mixamorig:RightLeg'];

// Ring heights stay inside the torso: 0.58 is above the crotch and 0.70 is below the
// armpit, so the fitted tube does not start life inside the thigh or shoulder mass.
const shirt = fitted(TORSO, [0.58, 0.62, 0.66, 0.70], 0.012);
const pants = merge(
  // Below the crotch, with a tight measurement band: a section taken at crotch height picks
  // up the pelvis, which makes the two legs wide enough to intersect at the midline.
  fitted(LEFT_LEG, [0.46, 0.40, 0.34, 0.28, 0.22, 0.16], 0.012, 18, 0.015),
  fitted(RIGHT_LEG, [0.46, 0.40, 0.34, 0.28, 0.22, 0.16], 0.012, 18, 0.015));

const POSES = Object.entries(GARMENT_TEST_POSES).map(([name, pose]) => ({
  name,
  pose: Object.fromEntries(Object.entries(pose).map(([bone, entry]) => [
    bone, Array.isArray(entry) ? entry : axisAngleQuaternion(entry.axis, entry.radians),
  ])),
}));

// A garment is carried by its top ring, not its hem. Welding the ring to the body
// (blend 0) is the strongest form of anchor: those vertices follow the skin target exactly,
// which is what a collar or a waistband does. The hem is left dynamic so the body can push
// it — a welded hem is a garment that is inside the thigh whenever the character crouches.
function anchorRing(contract, start, end) {
  for (let vertex = start; vertex <= end; ++vertex) {
    contract.anchors[vertex] = 1;
    contract.blend[vertex] = 0;
  }
}

// Displacement limits are chosen against the mesh resolution, not picked as a round number:
// a vertex allowed to travel further than its own edges can stretch spends the budget
// pulling the fabric apart. The shirt's rings are 4 cm apart and its pants 8 cm, with
// horizontal spacing of 3 cm / 2 cm, so 6 mm and 8 mm stay inside a 15% strain budget.
const SHIRT_SEGMENTS = 24;
const PANTS_SEGMENTS = 18;
const shirtBound = bindGarment({ garment: shirt, character, physics: { dynamicBlend: 0.5, maxDisplacementMetres: 0.006 } });
anchorRing(shirtBound.contract, 3 * (SHIRT_SEGMENTS + 1), 4 * (SHIRT_SEGMENTS + 1) - 1);
const pantsBound = bindGarment({ garment: pants, character, physics: { dynamicBlend: 0.5, maxDisplacementMetres: 0.008 } });
anchorRing(pantsBound.contract, 0, PANTS_SEGMENTS);
anchorRing(pantsBound.contract, 6 * (PANTS_SEGMENTS + 1), 7 * (PANTS_SEGMENTS + 1) - 1);

const library = compileGarmentLibrary([
  { id: 'fitted_shirt', character, bound: shirtBound, poses: POSES, seed: 7 },
  { id: 'fitted_pants', character, bound: pantsBound, poses: POSES, seed: 11 },
]);

async function main() {
  const output = path.resolve(process.argv[2] ?? path.join(ROOT, 'build/garment'));
  await fs.mkdir(output, { recursive: true });
  for (const garment of library.garments) {
    const payloadJson = canonicalJson(garment.payload);
    const file = path.join(output, `${garment.hash}.ngcloth.json`);
    const bytes = `${canonicalJson({ schema: garment.payload.schema, payloadJson, hash: garment.hash })}\n`;
    try { await fs.writeFile(file, bytes, { flag: 'wx' }); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      if (await fs.readFile(file, 'utf8') !== bytes) throw new GarmentError('NG-E-GARMENT-HASH', `Cooked object conflict: ${file}`);
    }
  }
  const receipt = {
    schema: 'nativegpu.cloth-library/1', libraryHash: library.hash,
    // The native loader reads a `cloths` list of content-addressed entries; a garment
    // package reuses that container so one loader and one hash check cover both.
    cloths: library.garments.map((garment) => ({ id: garment.source.id, hash: garment.hash, file: `${garment.hash}.ngcloth.json` })),
    runtimeIntegrated: false,
  };
  const packageHash = sha256Hex(canonicalJson(receipt));
  const manifest = {
    schema: receipt.schema, libraryHash: receipt.libraryHash, packageHash,
    cloths: receipt.cloths,
    runtimeIntegrated: false,
    payloadJson: canonicalJson(receipt),
  };
  const temporary = path.join(output, `.library-${process.pid}-${randomUUID()}.tmp`);
  try {
    await fs.writeFile(temporary, `${canonicalJson(manifest)}\n`, { flag: 'wx' });
    await fs.rename(temporary, path.join(output, 'library.json'));
  } finally { await fs.rm(temporary, { force: true }); }

  console.log(JSON.stringify({
    schema: 'nativegpu.garment-fixtures/1', outputDirectory: output, libraryHash: library.hash,
    garments: library.garments.map((garment) => ({
      id: garment.source.id, hash: garment.hash, vertices: garment.vertexCount, triangles: garment.triangleCount,
      edges: garment.edgeCount, bones: garment.boneCount, poses: garment.poseCount, proxies: garment.proxyCount,
      dynamicsVertices: garment.dynamicsVertices, anchoredVertices: garment.anchoredVertices,
      areaMetres2: Number(garment.totalAreaMetres2.toFixed(4)),
    })),
  }));
}

await main();
