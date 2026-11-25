import { canonicalJson, sha256Hex } from '../assets/canonical.mjs';
import { encodePng } from './png.mjs';
import { GarmentError } from '../garment/inspect.mjs';

// Weight and physics export for AI review.
//
// The plan is explicit that a coloured image is not an answer: the AI needs the numbers
// behind the pixels, an unambiguous legend, and an ID buffer so a 2D selection maps back
// to a vertex. Every export here therefore produces four things together —
// image, legend, metadata, and numeric/id buffers — and all four are derived from one
// snapshot so they cannot disagree.

const fail = (ok, code, message, details) => { if (!ok) throw new GarmentError(code, message, details); };

// A single fixed 0–1 ramp, used by every heatmap. A per-image auto-range would make two
// screenshots incomparable, which is exactly how a review of "the weights changed" ends
// up comparing two different colour scales.
export const WEIGHT_RAMP = Object.freeze([
  // The ramp bottom is deliberately brighter than the canvas: a zero weight drawn as
  // near-black is indistinguishable from "no geometry here", which is the difference
  // between "this bone has no influence" and "nothing was exported".
  [0.0, [0.19, 0.23, 0.34]],
  [0.25, [0.10, 0.35, 0.72]],
  [0.5, [0.16, 0.78, 0.62]],
  [0.75, [0.96, 0.82, 0.22]],
  [1.0, [0.93, 0.24, 0.18]],
]);

export function rampColor(value) {
  const clamped = Math.max(0, Math.min(1, value));
  for (let index = 1; index < WEIGHT_RAMP.length; ++index) {
    const [stop, colour] = WEIGHT_RAMP[index];
    const [previousStop, previousColour] = WEIGHT_RAMP[index - 1];
    if (clamped <= stop) {
      const t = (clamped - previousStop) / (stop - previousStop);
      return previousColour.map((channel, axis) => channel + (colour[axis] - channel) * t);
    }
  }
  return WEIGHT_RAMP[WEIGHT_RAMP.length - 1][1];
}

export const EXPORT_CHANNELS = Object.freeze([
  'skin_weight', 'dominant_bone', 'influence_count', 'blend', 'max_displacement',
  'anchors', 'collision_thickness', 'penetration', 'confidence',
]);

export const VIEWS = Object.freeze(['front', 'back', 'left', 'right']);

// Orthographic camera. Perspective distorts a weight map (a vertex at the edge of the
// frame looks further from its neighbour than it is), and a review compares distances.
export function orthoView(view, bounds, { margin = 0.05 } = {}) {
  const centre = [
    (bounds.min[0] + bounds.max[0]) / 2,
    (bounds.min[1] + bounds.max[1]) / 2,
    (bounds.min[2] + bounds.max[2]) / 2,
  ];
  const size = [0, 1, 2].map((axis) => Math.max(1e-3, bounds.max[axis] - bounds.min[axis]));
  const scale = Math.max(size[0], size[1], size[2]) * (1 + margin * 2);
  // Direction the camera looks *from*, in the character frame (+Z forward, +X left).
  const direction = { front: [0, 0, 1], back: [0, 0, -1], left: [1, 0, 0], right: [-1, 0, 0] }[view];
  fail(direction, 'NG-E-WEIGHTS-EXPORT', `view must be one of ${VIEWS.join(', ')}`);
  return {
    view, centre, scale, direction,
    right: view === 'front' ? [-1, 0, 0] : view === 'back' ? [1, 0, 0] : [0, 0, 1],
    up: [0, 1, 0],
  };
}

// World → pixel. `depth` grows away from the camera and is used for the paint order.
export function projectOrtho(point, camera, { width, height }) {
  const relative = [
    point[0] - camera.centre[0], point[1] - camera.centre[1], point[2] - camera.centre[2],
  ];
  const right = relative[0] * camera.right[0] + relative[1] * camera.right[1] + relative[2] * camera.right[2];
  const up = relative[1];
  const depth = -(relative[0] * camera.direction[0] + relative[1] * camera.direction[1] + relative[2] * camera.direction[2]);
  const pixelsPerMetre = Math.min(width, height) / camera.scale;
  return {
    x: width / 2 + right * pixelsPerMetre,
    y: height / 2 - up * pixelsPerMetre,
    depth,
    pixelsPerMetre,
  };
}

function drawDisc(rgba, width, height, x, y, radius, colour, alpha = 1) {
  const minX = Math.max(0, Math.floor(x - radius)), maxX = Math.min(width - 1, Math.ceil(x + radius));
  const minY = Math.max(0, Math.floor(y - radius)), maxY = Math.min(height - 1, Math.ceil(y + radius));
  for (let py = minY; py <= maxY; ++py) {
    for (let px = minX; px <= maxX; ++px) {
      const distance = Math.hypot(px + 0.5 - x, py + 0.5 - y);
      if (distance > radius) continue;
      const coverage = Math.min(1, radius - distance + 0.5);
      const index = (py * width + px) * 4;
      for (let channel = 0; channel < 3; ++channel) {
        const previous = rgba[index + channel] / 255;
        rgba[index + channel] = Math.round((previous + (colour[channel] - previous) * coverage * alpha) * 255);
      }
      rgba[index + 3] = 255;
    }
  }
}

function drawLine(rgba, width, height, x0, y0, x1, y1, colour) {
  const steps = Math.max(1, Math.ceil(Math.hypot(x1 - x0, y1 - y0)));
  for (let step = 0; step <= steps; ++step) {
    const t = step / steps;
    drawDisc(rgba, width, height, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0.6, colour, 0.45);
  }
}

// Channel → per-vertex scalar. Kept as an explicit switch so an unknown channel throws
// rather than quietly exporting a flat grey image that looks like "all weights are 0.5".
export function channelValues(channel, { weights, contract, penetration = null, partition = null, bone = null }) {
  const values = new Float32Array(weights.vertices);
  const { maximumInfluences } = weights;
  for (let vertex = 0; vertex < weights.vertices; ++vertex) {
    switch (channel) {
      case 'skin_weight': {
        fail(typeof bone === 'number', 'NG-E-WEIGHTS-EXPORT', 'skin_weight needs a bone index');
        let total = 0;
        for (let slot = 0; slot < maximumInfluences; ++slot) {
          if (weights.joints[vertex * maximumInfluences + slot] === bone) {
            total += weights.weights[vertex * maximumInfluences + slot];
          }
        }
        values[vertex] = total;
        break;
      }
      case 'dominant_bone': {
        let best = 0, bestWeight = -1;
        for (let slot = 0; slot < maximumInfluences; ++slot) {
          const weight = weights.weights[vertex * maximumInfluences + slot];
          if (weight > bestWeight) { bestWeight = weight; best = weights.joints[vertex * maximumInfluences + slot]; }
        }
        // Normalised bone index so the ramp has a meaning: 0 is the first bone, 1 the last.
        values[vertex] = best;
        break;
      }
      case 'influence_count': {
        let count = 0;
        for (let slot = 0; slot < maximumInfluences; ++slot) {
          if (weights.weights[vertex * maximumInfluences + slot] > 0) ++count;
        }
        values[vertex] = count / maximumInfluences;
        break;
      }
      case 'blend': values[vertex] = contract.blend[vertex]; break;
      case 'max_displacement': values[vertex] = contract.maxDisplacement[vertex]; break;
      case 'anchors': values[vertex] = contract.anchors[vertex]; break;
      case 'collision_thickness': values[vertex] = contract.thickness[vertex]; break;
      case 'penetration': values[vertex] = penetration ? Math.max(0, -(penetration[vertex] ?? 0)) : 0; break;
      case 'confidence': values[vertex] = partition?.confidence ? partition.confidence[vertex] : 0; break;
      default:
        fail(false, 'NG-E-WEIGHTS-EXPORT', `unknown channel ${channel}; use ${EXPORT_CHANNELS.join(', ')}`);
    }
  }
  return values;
}

function legendFor(channel, { bone = null, bones = [], contract = null, weights = null } = {}) {
  const base = {
    channel,
    // Each channel gets its own legend text: sharing one legend between "skin weight" and
    // "collision thickness" is how a reviewer reads 4 mm as a weight of 4.
    scale: `${WEIGHT_RAMP[0][0]} → ${WEIGHT_RAMP[WEIGHT_RAMP.length - 1][0]}`,
    stops: WEIGHT_RAMP.map(([value, colour]) => ({ value, rgb: colour.map((c) => Math.round(c * 255)) })),
    units: 'dimensionless 0–1',
    rampId: 'nativegpu.weights.linear-v1',
  };
  if (channel === 'skin_weight') {
    return {
      ...base,
      title: `bone weight: ${bones[bone]?.name ?? bone}`,
      boneIndex: bone, boneName: bones[bone]?.name ?? null,
      note: 'fixed 0–1 scale, not auto-ranged, so two exports are comparable',
    };
  }
  if (channel === 'dominant_bone') {
    return {
      ...base,
      title: 'dominant bone (categorical, mapped onto the ramp for the image only)',
      note: 'read the numeric buffer for the actual bone index; the colour is not a weight',
      categorical: true,
      units: 'bone index',
    };
  }
  if (channel === 'influence_count') {
    return {
      ...base,
      title: 'influence count / max influences',
      note: 'a value near the top of the ramp means the vertex is at the runtime influence limit',
      units: 'count / max',
    };
  }
  if (channel === 'blend') {
    return { ...base, title: 'dynamic blend weight (0 = skin, 1 = simulated)', note: 'physics channel, not a skin weight' };
  }
  if (channel === 'max_displacement') {
    return {
      ...base, title: 'maximum displacement',
      note: `value is metres scaled by ${contract ? (1 / Math.max(...contract.maxDisplacement, 1e-6)).toFixed(2) : '1'}; read the numeric buffer for metres`,
      units: 'metres',
    };
  }
  if (channel === 'collision_thickness') {
    return { ...base, title: 'collision thickness', units: 'metres', note: 'scaled for display; numeric buffer is authoritative' };
  }
  if (channel === 'anchors') {
    return { ...base, title: 'anchor stiffness (0 = free, 1 = pinned)', units: '0–1' };
  }
  if (channel === 'penetration') {
    return { ...base, title: 'penetration depth', units: 'metres', note: '0 at the ramp bottom; empty space is a vertex outside the body' };
  }
  return { ...base, title: 'region confidence', note: 'low confidence means the region was inherited, not measured' };
}

export function renderWeightsView({
  channel, view = 'front', width = 512, height = 512, snapshot, garment, weights, contract,
  penetration = null, partition = null, bone = null, bones = [], wireframe = true, showAnchors = true,
}) {
  fail(snapshot, 'NG-E-WEIGHTS-EXPORT', 'a weights snapshot is required; the image and the numbers must share one');
  fail(VIEWS.includes(view), 'NG-E-WEIGHTS-EXPORT', `view must be one of ${VIEWS.join(', ')}`);
  const values = channelValues(channel, { weights, contract, penetration, partition, bone });
  let minimum = 0, maximum = 1;
  if (channel === 'dominant_bone') {
    maximum = Math.max(1, bones.length - 1);
  } else if (channel === 'max_displacement' || channel === 'collision_thickness') {
    // Physics channels are in metres; scale them so the ramp is usable, and say so in the
    // legend. 2 cm of displacement or thickness covers every value a garment needs.
    const scale = 0.02;
    for (let index = 0; index < values.length; ++index) values[index] /= scale;
  } else if (channel === 'penetration') {
    const scale = 0.01;
    for (let index = 0; index < values.length; ++index) values[index] /= scale;
  }

  const bounds = (() => {
    const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
    for (let index = 0; index < garment.positions.length; index += 3) {
      for (let axis = 0; axis < 3; ++axis) {
        min[axis] = Math.min(min[axis], garment.positions[index + axis]);
        max[axis] = Math.max(max[axis], garment.positions[index + axis]);
      }
    }
    return { min, max };
  })();
  const camera = orthoView(view, bounds);
  const rgba = new Uint8Array(width * height * 4);
  for (let index = 0; index < width * height; ++index) {
    rgba[index * 4] = 8; rgba[index * 4 + 1] = 9; rgba[index * 4 + 2] = 12; rgba[index * 4 + 3] = 255;
  }
  const idBuffer = new Uint32Array(width * height);

  // Painter's order: far vertices first. Without it the near side of a skirt is drawn over
  // by the far side and the heatmap shows the wrong surface.
  const order = Array.from({ length: garment.vertices }, (_, index) => index)
    .map((index) => ({
      index,
      projected: projectOrtho([
        garment.positions[index * 3], garment.positions[index * 3 + 1], garment.positions[index * 3 + 2],
      ], camera, { width, height }),
    }))
    .sort((a, b) => b.projected.depth - a.projected.depth);

  const flat = weights ? Array.from({ length: weights.vertices }, (_, index) => index) : [];
  if (wireframe && flat.length) {
    for (let triangle = 0; triangle < garment.triangles; ++triangle) {
      const a = garment.indices[triangle * 3], b = garment.indices[triangle * 3 + 1], c = garment.indices[triangle * 3 + 2];
      const pa = projectOrtho([garment.positions[a * 3], garment.positions[a * 3 + 1], garment.positions[a * 3 + 2]], camera, { width, height });
      const pb = projectOrtho([garment.positions[b * 3], garment.positions[b * 3 + 1], garment.positions[b * 3 + 2]], camera, { width, height });
      const pc = projectOrtho([garment.positions[c * 3], garment.positions[c * 3 + 1], garment.positions[c * 3 + 2]], camera, { width, height });
      drawLine(rgba, width, height, pa.x, pa.y, pb.x, pb.y, [0.35, 0.38, 0.45]);
      drawLine(rgba, width, height, pb.x, pb.y, pc.x, pc.y, [0.35, 0.38, 0.45]);
      drawLine(rgba, width, height, pc.x, pc.y, pa.x, pa.y, [0.35, 0.38, 0.45]);
    }
  }
  const radius = Math.max(1.2, Math.min(3.5, width / 220));
  for (const entry of order) {
    const value = maximum > 0 ? values[entry.index] / maximum : 0;
    const colour = rampColor(value);
    drawDisc(rgba, width, height, entry.projected.x, entry.projected.y, radius, colour, 1);
    // Id buffer: vertexId + 1, so 0 means "no geometry here" and a 2D selection can name an
    // exact vertex instead of a colour.
    const px = Math.round(entry.projected.x), py = Math.round(entry.projected.y);
    for (let dy = -Math.ceil(radius); dy <= Math.ceil(radius); ++dy) {
      for (let dx = -Math.ceil(radius); dx <= Math.ceil(radius); ++dx) {
        if (Math.hypot(dx, dy) > radius) continue;
        const x = px + dx, y = py + dy;
        if (x < 0 || y < 0 || x >= width || y >= height) continue;
        idBuffer[y * width + x] = garment.vertexIds[entry.index] + 1;
      }
    }
  }
  if (showAnchors && contract) {
    for (let index = 0; index < garment.vertices; ++index) {
      if (contract.anchors[index] <= 0) continue;
      const projected = projectOrtho([
        garment.positions[index * 3], garment.positions[index * 3 + 1], garment.positions[index * 3 + 2],
      ], camera, { width, height });
      drawDisc(rgba, width, height, projected.x, projected.y, radius * 1.3, [1, 1, 1], 0.9);
    }
  }

  const png = encodePng({ width, height, rgba });
  const legend = legendFor(channel, { bone, bones, contract, weights });
  return {
    png, idBuffer, width, height, legend, values, camera,
    imageHash: sha256Hex(png),
    snapshotDigest: snapshot.digest,
  };
}

// Numeric export. Three parallel arrays rather than one packed struct, because the AI
// reads JSON and a binary dump of an interleaved layout it cannot parse is not "numbers".
export function exportWeightsBundle({
  garment, weights, snapshot, contract, bones, penetration = null, partition = null,
  views = ['front'], channels = ['skin_weight'], bone = null, resolution = 512, wireframe = true,
}) {
  fail(snapshot && snapshot.topologyHash, 'NG-E-WEIGHTS-EXPORT',
    'a weights snapshot is required; images and numbers must come from the same one');
  const images = [];
  for (const view of views) {
    for (const channel of channels) {
      const rendered = renderWeightsView({
        channel, view, width: resolution, height: resolution, snapshot, garment, weights, contract,
        penetration, partition, bone, bones, wireframe,
      });
      images.push({
        view, channel,
        imageBase64: rendered.png.toString('base64'),
        imageHash: rendered.imageHash,
        legend: rendered.legend,
        idBufferBase64: Buffer.from(new Uint32Array(rendered.idBuffer).buffer).toString('base64'),
        idBufferType: 'u32le',
        width: rendered.width, height: rendered.height,
      });
    }
  }

  // Numeric data, indexed by stable vertex id.
  const numeric = {
    vertexIds: Array.from(garment.vertexIds),
    indices: Array.from(garment.indices),
    joints: Array.from(weights.joints),
    weights: Array.from(weights.weights),
    maximumInfluences: weights.maximumInfluences,
    blend: contract ? Array.from(contract.blend) : null,
    maxDisplacementMetres: contract ? Array.from(contract.maxDisplacement) : null,
    collisionThicknessMetres: contract ? Array.from(contract.thickness) : null,
    anchorStiffness: contract ? Array.from(contract.anchors) : null,
    penetrationMetres: penetration ? Array.from(penetration) : null,
    regionConfidence: partition?.confidence ? Array.from(partition.confidence) : null,
    regions: partition?.regions ?? null,
  };
  const anomalies = [];
  for (let vertex = 0; vertex < weights.vertices; ++vertex) {
    let total = 0, count = 0;
    for (let slot = 0; slot < weights.maximumInfluences; ++slot) {
      const weight = weights.weights[vertex * weights.maximumInfluences + slot];
      if (!Number.isFinite(weight) || weight < 0) {
        anomalies.push({ vertexId: garment.vertexIds[vertex], kind: 'invalid-weight', value: weight });
        continue;
      }
      if (weight > 0) { total += weight; ++count; }
    }
    if (count === 0) anomalies.push({ vertexId: garment.vertexIds[vertex], kind: 'zero-weight' });
    else if (Math.abs(total - 1) > 1e-4) {
      anomalies.push({ vertexId: garment.vertexIds[vertex], kind: 'unnormalised', residual: total - 1 });
    }
    if (penetration && penetration[vertex] < -0.002) {
      anomalies.push({ vertexId: garment.vertexIds[vertex], kind: 'penetration', depthMetres: -penetration[vertex] });
    }
  }

  const metadata = {
    schema: 'nativegpu.weights-export/1',
    topologyHash: snapshot.topologyHash,
    snapshotDigest: snapshot.digest,
    boneTable: bones.map((bone) => ({ index: bone.index, name: bone.name })),
    bone,
    images: images.map(({ imageBase64, idBufferBase64, ...rest }) => rest),
    units: { weights: '0–1', displacement: 'metres', thickness: 'metres', penetration: 'metres' },
    anomalies,
    unsupported: ['self-collision', 'cloth-cloth collision'],
    note: 'Images, legends, id buffers and numeric arrays all come from the same snapshot digest.',
  };
  return {
    metadata, images, numeric,
    digest: sha256Hex(canonicalJson({ metadata, numeric: numeric.weights.slice(0, 4096) })).slice(0, 32),
  };
}

// Binary form for large meshes: a header JSON followed by tightly packed arrays, so a
// consumer can read either the JSON summary or mmap the arrays.
export function encodeWeightsBinary(bundle) {
  const header = {
    schema: 'nativegpu.weights-binary/1',
    topologyHash: bundle.metadata.topologyHash,
    vertexCount: bundle.numeric.vertexIds.length,
    maximumInfluences: bundle.numeric.maximumInfluences,
    arrays: [
      { name: 'vertexIds', type: 'u32', count: bundle.numeric.vertexIds.length },
      { name: 'indices', type: 'u32', count: bundle.numeric.indices.length },
      { name: 'joints', type: 'u32', count: bundle.numeric.joints.length },
      { name: 'weights', type: 'f32', count: bundle.numeric.weights.length },
    ],
  };
  const blocks = [
    Buffer.from(`${JSON.stringify(header)}\n`, 'utf8'),
    Buffer.from(new Uint32Array(bundle.numeric.vertexIds).buffer),
    Buffer.from(new Uint32Array(bundle.numeric.indices).buffer),
    Buffer.from(new Uint32Array(bundle.numeric.joints).buffer),
    Buffer.from(new Float32Array(bundle.numeric.weights).buffer),
  ];
  return Buffer.concat(blocks);
}
