import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { HairError } from './spec.mjs';

// Capture plan, camera math and screenshot-selection mapping.
//
// The plan's requirement is not "take a screenshot". It is: a screenshot must be
// reproducible (same camera, same pose, same revision), must say which debug channel
// it shows, and must be usable to address a patch — which means a 2D selection has to
// resolve to a real guide id through a depth or id buffer, and a selection made from an
// outdated screenshot must be refused rather than applied to whatever is there now.

const fail = (ok, code, message, details) => { if (!ok) throw new HairError(code, message, details); };

export const CAPTURE_VIEWS = Object.freeze({
  front: { yawDegrees: 0, pitchDegrees: 0, distanceScale: 2.6, fovYDegrees: 32 },
  back: { yawDegrees: 180, pitchDegrees: 0, distanceScale: 2.6, fovYDegrees: 32 },
  left: { yawDegrees: 90, pitchDegrees: 0, distanceScale: 2.6, fovYDegrees: 32 },
  right: { yawDegrees: -90, pitchDegrees: 0, distanceScale: 2.6, fovYDegrees: 32 },
  three_quarter_left: { yawDegrees: 45, pitchDegrees: 8, distanceScale: 2.4, fovYDegrees: 32 },
  three_quarter_right: { yawDegrees: -45, pitchDegrees: 8, distanceScale: 2.4, fovYDegrees: 32 },
  hairline_closeup: { yawDegrees: 12, pitchDegrees: -14, distanceScale: 1.35, fovYDegrees: 26 },
  top: { yawDegrees: 0, pitchDegrees: 62, distanceScale: 2.2, fovYDegrees: 34 },
});

// Every channel the plan lists. `beauty` is what a person looks at; the rest are what
// makes a claim checkable: `region_id` proves the region split, `root_binding` proves
// roots sit on the scalp, `coverage` exposes the sub-pixel path, `lod` proves a level
// switch. A capture that only produced beauty could not support any of those claims.
export const DEBUG_CHANNELS = Object.freeze([
  'beauty', 'silhouette', 'region_id', 'guide', 'root_binding', 'tangent',
  'coverage', 'collision', 'lod', 'depth', 'normal',
]);

export const LIGHTING_SETUPS = Object.freeze({
  neutral: {
    keyIntensity: 3.0, fillIntensity: 1.1, rimIntensity: 0.6,
    keyAzimuthDegrees: 35, keyElevationDegrees: 30, colourTemperatureK: 5600,
  },
  // Backlight exists because hair transmission is invisible under a neutral key: a
  // fibre shader that has no transmission term looks acceptable at the front and
  // obviously wrong the moment the light is behind the head.
  backlight: {
    keyIntensity: 0.7, fillIntensity: 0.35, rimIntensity: 6.0,
    keyAzimuthDegrees: 180, keyElevationDegrees: 12, colourTemperatureK: 5200,
  },
  // A dim setup is the only way to see a dark hairstyle's silhouette and its
  // deep-opacity shadow separately from its specular response.
  silhouette: {
    keyIntensity: 0.0, fillIntensity: 0.0, rimIntensity: 0.0,
    keyAzimuthDegrees: 0, keyElevationDegrees: 0, colourTemperatureK: 6500,
  },
});

export function cameraForView(view, { headCentre, headRadiusMetres, targetOffsetMetres = 0 } = {}) {
  const preset = CAPTURE_VIEWS[view];
  fail(preset, 'NG-E-HAIR-CAPTURE', `unknown capture view ${view}; use ${Object.keys(CAPTURE_VIEWS).join(', ')}`);
  fail(Array.isArray(headCentre) && headCentre.length === 3 && headCentre.every(Number.isFinite),
    'NG-E-HAIR-CAPTURE', 'headCentre must be a 3-vector in metres');
  fail(typeof headRadiusMetres === 'number' && headRadiusMetres > 0.01 && headRadiusMetres < 1,
    'NG-E-HAIR-CAPTURE', 'headRadiusMetres must be a plausible head radius');
  const yaw = (preset.yawDegrees * Math.PI) / 180;
  const pitch = (preset.pitchDegrees * Math.PI) / 180;
  const distance = headRadiusMetres * preset.distanceScale + targetOffsetMetres;
  const target = [headCentre[0], headCentre[1], headCentre[2]];
  // Camera sits on a sphere around the head. yaw 0 is in front of the character (+Z),
  // matching the character frame used by the scalp regions.
  const position = [
    target[0] + Math.sin(yaw) * Math.cos(pitch) * distance,
    target[1] + Math.sin(pitch) * distance,
    target[2] + Math.cos(yaw) * Math.cos(pitch) * distance,
  ];
  return {
    view, position, target, up: [0, 1, 0],
    fovYDegrees: preset.fovYDegrees,
    nearMetres: 0.01, farMetres: 40,
    yawDegrees: preset.yawDegrees, pitchDegrees: preset.pitchDegrees,
    distanceMetres: distance,
  };
}

function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function normalize(a) {
  const length = Math.hypot(a[0], a[1], a[2]) || 1;
  return [a[0] / length, a[1] / length, a[2] / length];
}

// View matrix, column-major, row-vector convention (same as the renderer's
// `multiplyMatrix`): rows are the camera basis, so a point is transformed by
// x' = dot(right, p - eye) and so on.
export function viewMatrix(camera) {
  const forward = normalize(sub(camera.target, camera.position));
  const right = normalize(cross(camera.up, forward));
  const up = cross(forward, right);
  const eye = camera.position;
  return [
    right[0], up[0], forward[0], 0,
    right[1], up[1], forward[1], 0,
    right[2], up[2], forward[2], 0,
    -dot(right, eye), -dot(up, eye), -dot(forward, eye), 1,
  ];
}

export function projectPoint(point, camera, { width, height }) {
  const view = viewMatrix(camera);
  const x = view[0] * point[0] + view[4] * point[1] + view[8] * point[2] + view[12];
  const y = view[1] * point[0] + view[5] * point[1] + view[9] * point[2] + view[13];
  const z = view[2] * point[0] + view[6] * point[1] + view[10] * point[2] + view[14];
  fail(z > camera.nearMetres, 'NG-E-HAIR-CAPTURE', 'point is behind the capture camera');
  const focal = (height * 0.5) / Math.tan((camera.fovYDegrees * Math.PI) / 360);
  return {
    x: width * 0.5 + (x * focal) / z,
    y: height * 0.5 - (y * focal) / z,
    depthMetres: z,
  };
}

export function unprojectPixel(px, py, depthMetres, camera, { width, height }) {
  fail(Number.isFinite(depthMetres) && depthMetres > camera.nearMetres && depthMetres < camera.farMetres,
    'NG-E-HAIR-CAPTURE', 'depth must be inside the camera near/far range');
  const focal = (height * 0.5) / Math.tan((camera.fovYDegrees * Math.PI) / 360);
  const x = ((px - width * 0.5) * depthMetres) / focal;
  const y = -((py - height * 0.5) * depthMetres) / focal;
  const view = viewMatrix(camera);
  // Inverse of the orthonormal view basis: the point is basisᵀ · local + eye.
  const right = [view[0], view[4], view[8]];
  const up = [view[1], view[5], view[9]];
  const forward = [view[2], view[6], view[10]];
  return [
    camera.position[0] + right[0] * x + up[0] * y + forward[0] * depthMetres,
    camera.position[1] + right[1] * x + up[1] * y + forward[1] * depthMetres,
    camera.position[2] + right[2] * x + up[2] * y + forward[2] * depthMetres,
  ];
}

export function buildCapturePlan({
  views = Object.keys(CAPTURE_VIEWS), channels = ['beauty', 'silhouette', 'region_id', 'root_binding'],
  lighting = ['neutral'], resolution = { width: 1024, height: 1024 },
  headCentre, headRadiusMetres, revision, assetHash, seed, pose = {}, frame = 3, exposure = 0,
} = {}) {
  fail(Array.isArray(views) && views.length > 0, 'NG-E-HAIR-CAPTURE', 'views must be a non-empty array');
  fail(Array.isArray(channels) && channels.length > 0, 'NG-E-HAIR-CAPTURE', 'channels must be a non-empty array');
  fail(Array.isArray(lighting) && lighting.length > 0, 'NG-E-HAIR-CAPTURE', 'lighting must be a non-empty array');
  for (const view of views) fail(CAPTURE_VIEWS[view], 'NG-E-HAIR-CAPTURE', `unknown view ${view}`);
  for (const channel of channels) {
    fail(DEBUG_CHANNELS.includes(channel), 'NG-E-HAIR-CAPTURE',
      `unknown channel ${channel}; use ${DEBUG_CHANNELS.join(', ')}`);
  }
  for (const setup of lighting) fail(LIGHTING_SETUPS[setup], 'NG-E-HAIR-CAPTURE', `unknown lighting setup ${setup}`);
  fail(Number.isInteger(resolution.width) && resolution.width >= 64 && resolution.width <= 4096,
    'NG-E-HAIR-CAPTURE', 'resolution.width must be 64–4096');
  fail(Number.isInteger(resolution.height) && resolution.height >= 64 && resolution.height <= 4096,
    'NG-E-HAIR-CAPTURE', 'resolution.height must be 64–4096');
  fail(typeof revision === 'string' && revision.length >= 8, 'NG-E-HAIR-CAPTURE',
    'revision is required: a capture whose revision is unknown cannot be used to map a selection');
  fail(typeof assetHash === 'string' && assetHash.length >= 8, 'NG-E-HAIR-CAPTURE', 'assetHash is required');

  const entries = [];
  for (const view of views) {
    const camera = cameraForView(view, { headCentre, headRadiusMetres });
    for (const setup of lighting) {
      for (const channel of channels) {
        // Ids are content-derived, so re-planning the same capture yields the same
        // names and a stale plan cannot masquerade as a new one.
        const id = sha256Hex(canonicalJson({ view, setup, channel, revision, seed, pose, resolution })).slice(0, 16);
        entries.push({
          id,
          view, channel, lighting: setup,
          camera,
          resolution: { ...resolution },
          pose: structuredClone(pose),
          frame,
          exposure,
          revision,
          assetHash,
          seed,
          requiresIdBuffer: channel === 'region_id' || channel === 'guide' || channel === 'lod',
          requiresDepthBuffer: channel === 'depth' || channel === 'collision' || channel === 'root_binding',
        });
      }
    }
  }
  return {
    schema: 'nativegpu.hair-capture-plan/1',
    revision, assetHash, seed,
    lighting: lighting.map((name) => ({ name, ...LIGHTING_SETUPS[name] })),
    entries,
    count: entries.length,
    note: 'Beauty views are for review; region_id, root_binding, coverage and lod channels exist to make claims checkable.',
  };
}

export function captureMetadata(entry, extra = {}) {
  fail(entry && typeof entry === 'object', 'NG-E-HAIR-CAPTURE', 'capture entry is required');
  const metadata = {
    schema: 'nativegpu.hair-capture/1',
    id: entry.id,
    view: entry.view,
    channel: entry.channel,
    lighting: entry.lighting,
    camera: {
      position: entry.camera.position,
      target: entry.camera.target,
      up: entry.camera.up,
      fovYDegrees: entry.camera.fovYDegrees,
      nearMetres: entry.camera.nearMetres,
      farMetres: entry.camera.farMetres,
      yawDegrees: entry.camera.yawDegrees,
      pitchDegrees: entry.camera.pitchDegrees,
      distanceMetres: entry.camera.distanceMetres,
    },
    resolution: entry.resolution,
    pose: entry.pose,
    exposure: entry.exposure,
    frame: entry.frame,
    assetHash: entry.assetHash,
    revision: entry.revision,
    seed: entry.seed,
    ...extra,
  };
  metadata.digest = sha256Hex(canonicalJson(metadata)).slice(0, 32);
  return metadata;
}

// ------------------------------------------------------- selection mapping
// A 2D selection is only meaningful against the buffers it was drawn on. The rules:
//
//  * the metadata's revision and asset hash must match the hairstyle being patched,
//  * the buffer dimensions must match the metadata resolution,
//  * `visible` needs a depth buffer (depth test decided what the pixel shows),
//  * `through` needs an id buffer (it deliberately picks strands behind the surface),
//  * a selection drawn on a beauty image with no buffer is refused, not guessed.
export function resolveSelection({
  selection, metadata, currentRevision, currentAssetHash, idBuffer = null, depthBuffer = null,
  guides = [], maxDepthMetres = 0.05,
}) {
  fail(selection && typeof selection === 'object', 'NG-E-HAIR-SELECTION', 'selection is required');
  const mode = selection.mode ?? 'visible';
  fail(['visible', 'through'].includes(mode), 'NG-E-HAIR-SELECTION', 'selection.mode must be visible or through');
  fail(metadata && typeof metadata === 'object', 'NG-E-HAIR-SELECTION',
    'a selection must name the capture metadata it was drawn on');
  if (metadata.revision !== currentRevision || metadata.assetHash !== currentAssetHash) {
    throw new HairError('NG-E-HAIR-SCREENSHOT-STALE',
      `the screenshot was taken at revision ${String(metadata.revision).slice(0, 12)} but the hairstyle is at `
      + `${String(currentRevision).slice(0, 12)}; capture again before selecting`,
      [{ screenshotRevision: metadata.revision, currentRevision, screenshotAsset: metadata.assetHash, currentAsset: currentAssetHash }]);
  }
  fail(Array.isArray(selection.rect) && selection.rect.length === 4 && selection.rect.every(Number.isFinite),
    'NG-E-HAIR-SELECTION', 'selection.rect must be [x, y, width, height] in pixels');
  const [rx, ry, rw, rh] = selection.rect;
  fail(rw > 0 && rh > 0, 'NG-E-HAIR-SELECTION', 'selection.rect must have a positive size');
  const { width, height } = metadata.resolution;
  fail(rx >= 0 && ry >= 0 && rx + rw <= width && ry + rh <= height, 'NG-E-HAIR-SELECTION',
    'selection.rect must be inside the capture resolution');

  if (mode === 'visible') {
    fail(depthBuffer, 'NG-E-HAIR-SELECTION',
      'visible selection needs a depth buffer: without depth the selection cannot tell a strand from the wall behind it');
    fail(depthBuffer.width === width && depthBuffer.height === height, 'NG-E-HAIR-SELECTION',
      'depth buffer dimensions do not match the capture resolution');
    fail(depthBuffer.data && depthBuffer.data.length >= width * height, 'NG-E-HAIR-SELECTION',
      'depth buffer is truncated');
    const points = [];
    for (let y = Math.floor(ry); y < Math.ceil(ry + rh); ++y) {
      for (let x = Math.floor(rx); x < Math.ceil(rx + rw); ++x) {
        const depth = depthBuffer.data[y * width + x];
        if (!(depth > 0)) continue;
        points.push(unprojectPixel(x + 0.5, y + 0.5, depth, metadata.camera, { width, height }));
      }
    }
    fail(points.length > 0, 'NG-E-HAIR-SELECTION', 'the selection covers no depth pixels');
    const matched = matchGuidesToPoints(points, guides, maxDepthMetres);
    return { mode, points: points.length, ...matched, screenshot: metadata.id, revision: currentRevision };
  }

  fail(idBuffer, 'NG-E-HAIR-SELECTION',
    'through selection needs an id buffer: a beauty image cannot say which guide is behind a strand');
  fail(idBuffer.width === width && idBuffer.height === height, 'NG-E-HAIR-SELECTION',
    'id buffer dimensions do not match the capture resolution');
  const ids = new Map();
  for (let y = Math.floor(ry); y < Math.ceil(ry + rh); ++y) {
    for (let x = Math.floor(rx); x < Math.ceil(rx + rw); ++x) {
      const value = idBuffer.data[y * width + x];
      if (!value) continue;
      ids.set(value, (ids.get(value) ?? 0) + 1);
    }
  }
  fail(ids.size > 0, 'NG-E-HAIR-SELECTION', 'the selection covers no guide ids');
  const ordered = [...ids.entries()].sort((a, b) => b[1] - a[1]);
  return {
    mode, guides: ordered.map(([id, pixels]) => ({ guideId: id, pixels })),
    screenshot: metadata.id, revision: currentRevision,
  };
}

// Point-cloud → guide matching for the depth path. A guide is selected when the
// selected pixels come within `maxDepthMetres` of it; the threshold is a distance in
// metres, not a pixel radius, so the same selection means the same thing at 512 and
// 2048 pixels.
export function matchGuidesToPoints(points, guides, maxDepthMetres = 0.05) {
  const hits = new Map();
  for (const guide of guides) {
    const samples = guide.points ?? [];
    for (const point of points) {
      for (const sample of samples) {
        const distance = Math.hypot(sample[0] - point[0], sample[1] - point[1], sample[2] - point[2]);
        if (distance <= maxDepthMetres) {
          const entry = hits.get(guide.stableId) ?? { guideId: guide.stableId, region: guide.region, distanceMetres: Infinity, count: 0 };
          entry.distanceMetres = Math.min(entry.distanceMetres, distance);
          entry.count += 1;
          hits.set(guide.stableId, entry);
          break;
        }
      }
    }
  }
  return {
    guides: [...hits.values()].sort((a, b) => a.distanceMetres - b.distanceMetres),
  };
}
