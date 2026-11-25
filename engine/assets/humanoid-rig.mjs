import {requireAsset} from './gltf-document.mjs';
import {globalTransforms, quaternion, product, rotate, add, sub, mul} from './animation-sampling.mjs';
import {boneKey, identifier, sideOf} from './bone-semantics.mjs';

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const length = v => Math.hypot(...v);
const unit = (v, fallback = [0, 0, 1]) => {
  const l = length(v);
  return l > 1e-8 ? v.map(x => x / l) : [...fallback];
};
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const distance = (a, b) => length(sub(a, b));

export function defaultRigSettings(height = 1.8) {
  return {
    height,
    turnThreshold: 0.35,
    turnStepHeight: clamp(0.055 * height, 0.03, 0.18),
    turnStepDuration: 0.28,
    turnCooldown: 0.12,
    maxTwist: 1.0,
    maxSlopeCosine: Math.cos(45 * Math.PI / 180),
    swingPrediction: 0.45,
    maxFootCorrection: clamp(0.22 * height, 0.08, 0.6),
    pelvisLower: -0.12 * height,
    pelvisUpper: 0.05 * height,
    maxStepHeight: 0.14 * height,
    maxPelvisShift: clamp(0.04 * height, 0.03, 0.1),
    soleRoll: 0.45,
    footLock: true,
    turnInPlace: true,
  };
}

function meshHeight(positions, bones, rest) {
  let min = Infinity, max = -Infinity;
  if (positions?.length) for (const p of positions) { min = Math.min(min, p[1]); max = Math.max(max, p[1]); }
  else for (const bone of rest) { min = Math.min(min, bone.translation[1]); max = Math.max(max, bone.translation[1]); }
  return Number.isFinite(min) && max > min ? max - min : 1.8;
}

function uniqueIndex(map, keys, diagnostics, label) {
  let ambiguous = false;
  for (const key of keys) {
    const hits = map.get(key) ?? [];
    if (hits.length === 1) return hits[0];
    if (hits.length > 1) {
      ambiguous = true;
      diagnostics.ambiguous.push(`${label} matches ${hits.map(i => `#${i}`).join(', ')}`);
    }
  }
  if (ambiguous) diagnostics.warnings.push(`Ambiguous ${label}; provide an explicit rig mapping.`);
  return -1;
}

function indexesByKey(bones) {
  const map = new Map();
  for (const [index, bone] of bones.entries()) {
    const key = boneKey(bone.name);
    if (!map.has(key)) map.set(key, []);
    map.get(key).push(index);
  }
  return map;
}

function semanticIndex(bones, byKey, keys, wantSide, suffixes, diagnostics, label) {
  const direct = uniqueIndex(byKey, keys, diagnostics, label);
  if (direct >= 0) return direct;
  const hits = [];
  for (const [index, bone] of bones.entries()) {
    if (wantSide && sideOf(bone.name) !== wantSide) continue;
    const id = identifier(bone.name);
    if (suffixes.some(suffix => id.endsWith(suffix))) hits.push(index);
  }
  if (hits.length === 1) return hits[0];
  if (hits.length > 1) diagnostics.ambiguous.push(`${label} suffix matches ${hits.map(index => `#${index}`).join(', ')}`);
  return -1;
}

function soleOffset(positions, joints, weights, foot, toe) {
  if (!positions?.length) return null;
  let min = Infinity;
  for (let i = 0; i < positions.length; ++i) {
    let share = 0;
    for (let k = 0; k < 4; ++k) {
      const joint = joints?.[i]?.[k];
      if (joint === foot || (toe >= 0 && joint === toe)) share += weights?.[i]?.[k] ?? 0;
    }
    if (share >= 0.25) min = Math.min(min, positions[i][1]);
  }
  return Number.isFinite(min) ? min : null;
}

// Left/right arm chains, used by the generic contact layer: a palm on a wall, a
// hand on a rail or a climbing hold is driven by the same two-bone solve the
// legs use. Detection is best-effort and never fatal — a rig without arms keeps
// its procedural feet and simply reports no hand contacts. The chain is stored
// with the same shape as a leg chain so the native side needs one parser.
export function detectArmChains(bones, byKey, rest, height, diagnostics) {
  const arms = [];
  const minLength = Math.max(1e-5, height * 1e-4);
  for (const side of ['left', 'right']) {
    const upper = semanticIndex(bones, byKey, [`${side}arm`, `${side}upperarm`], side, ['upperarm'], diagnostics, `${side} arm`);
    const fore = semanticIndex(bones, byKey, [`${side}forearm`, `${side}lowerarm`], side, ['forearm', 'lowerarm'], diagnostics, `${side} forearm`);
    const hand = semanticIndex(bones, byKey, [`${side}hand`], side, ['hand'], diagnostics, `${side} hand`);
    // A rig that never mentions arms at all is not an error; one that mentions
    // part of a chain is, and the reason has to be readable.
    if (upper < 0 && fore < 0 && hand < 0) continue;
    if (upper < 0 || fore < 0 || hand < 0) {
      diagnostics.warnings.push(`Incomplete ${side} arm chain; hand contacts are disabled.`);
      continue;
    }
    if (bones[fore].parent !== upper || bones[hand].parent !== fore) {
      diagnostics.warnings.push(`Unsupported ${side} arm hierarchy: upper arm/forearm/hand must be direct parents (intermediate helper bones need an explicit rig).`);
      continue;
    }
    const shoulder = rest[upper].translation, elbow = rest[fore].translation, wrist = rest[hand].translation;
    const l1 = distance(shoulder, elbow), l2 = distance(elbow, wrist);
    if (l1 < minLength || l2 < minLength) {
      diagnostics.warnings.push(`Zero-length ${side} arm bone.`);
      continue;
    }
    // Elbows bend backwards. A straight rest arm (T-pose) has no lateral offset
    // to read, so the pole falls back to the body's back direction instead of a
    // degenerate zero vector.
    const back = [0, 0, 1];
    const axis = unit(sub(wrist, shoulder), [0, -1, 0]);
    const offset = sub(elbow, shoulder);
    const lateral = sub(offset, mul(axis, dot(offset, axis)));
    const pole = length(lateral) > 1e-5
      ? add(offset, mul(unit(lateral, back), (l1 + l2) * 0.75))
      : mul(back, l1 + l2);
    arms.push({root: upper, joint: fore, tip: hand, pole, minBend: 0.01, maxBend: 2.9});
  }
  return arms;
}

// Builds a persistent rig description from bone semantics, hierarchy and rest
// proportions. It deliberately never guesses between duplicate names or through
// an intermediate twist bone; those cases return diagnostics instead.
export function detectHumanoidRig({bones, positions, joints, weights}) {
  requireAsset(Array.isArray(bones) && bones.length > 0, 'Rig detection requires bones');
  const diagnostics = {source: 'auto', missing: [], warnings: [], ambiguous: []};
  const byKey = indexesByKey(bones);
  const locals = bones.map(bone => ({translation: bone.rest.slice(0, 3), rotation: quaternion(bone.rest.slice(3, 7)), scale: bone.rest.slice(7)}));
  const rest = globalTransforms(bones, bones.map((_, i) => i), locals);
  const height = meshHeight(positions, bones, rest);
  const settings = defaultRigSettings(height);
  const rig = {version: 2, source: 'auto', confidence: 0, pelvis: -1, lookBone: -1, spine: -1, legs: [], arms: [], settings, diagnostics};
  rig.pelvis = semanticIndex(bones, byKey, ['hips', 'pelvis', 'hip'], '', ['hips', 'pelvis'], diagnostics, 'pelvis');
  rig.lookBone = semanticIndex(bones, byKey, ['head'], '', ['head'], diagnostics, 'head');
  if (rig.lookBone < 0) rig.lookBone = semanticIndex(bones, byKey, ['neck'], '', ['neck'], diagnostics, 'neck');
  rig.spine = semanticIndex(bones, byKey, ['chest', 'spine2', 'spine01', 'spine'], '', ['chest', 'spine'], diagnostics, 'spine');
  if (rig.pelvis < 0) diagnostics.missing.push('pelvis');
  if (rig.lookBone < 0) diagnostics.warnings.push('No head or neck bone; look control is disabled.');
  // Detected before the legs: an arm-only rig still gets hand contacts even when
  // the leg chains are rejected below.
  rig.arms = detectArmChains(bones, byKey, rest, height, diagnostics);

  const legs = [];
  for (const side of ['left', 'right']) {
    const thigh = semanticIndex(bones, byKey, [`${side}thigh`], side, ['thigh', 'upleg', 'upperleg'], diagnostics, `${side} thigh`);
    const calf = semanticIndex(bones, byKey, [`${side}calf`], side, ['calf', 'shin', 'lowerleg'], diagnostics, `${side} calf`);
    const foot = semanticIndex(bones, byKey, [`${side}foot`], side, ['foot'], diagnostics, `${side} foot`);
    const toe = semanticIndex(bones, byKey, [`${side}toe`], side, ['toe'], diagnostics, `${side} toe`);
    for (const [name, index] of [['thigh', thigh], ['calf', calf], ['foot', foot]]) if (index < 0) diagnostics.missing.push(`${side} ${name}`);
    if (thigh < 0 || calf < 0 || foot < 0) continue;
    if (bones[calf].parent !== thigh || bones[foot].parent !== calf) {
      diagnostics.warnings.push(`Unsupported ${side} leg hierarchy: thigh/calf/foot must be direct parents (intermediate helper bones need an explicit rig).`);
      continue;
    }
    const hip = rest[thigh].translation, knee = rest[calf].translation, ankle = rest[foot].translation;
    const l1 = distance(hip, knee), l2 = distance(knee, ankle);
    if (l1 < Math.max(1e-5, height * 1e-4) || l2 < Math.max(1e-5, height * 1e-4)) {
      diagnostics.warnings.push(`Zero-length ${side} leg bone.`);
      continue;
    }
    const toeIndex = toe >= 0 && bones[toe].parent === foot ? toe : -1;
    if (toe >= 0 && toeIndex < 0) diagnostics.warnings.push(`${side} toe is not parented to the foot; toe control is disabled.`);
    let forward = [0, 0, -1];
    if (toeIndex >= 0) forward = unit(sub(rest[toeIndex].translation, ankle), forward);
    const axis = unit(sub(ankle, hip), [0, -1, 0]);
    const offset = sub(knee, hip);
    const lateral = sub(offset, mul(axis, dot(offset, axis)));
    const pole = length(lateral) > 1e-5
      ? add(offset, mul(unit(lateral, forward), (l1 + l2) * 0.75))
      : mul(forward, l1 + l2);
    const soleY = soleOffset(positions, joints, weights, foot, toeIndex);
    const soleHeight = soleY === null ? clamp(0.03 * height, 0.015, 0.12) : clamp(rest[foot].translation[1] - soleY, 0, 0.3 * height);
    const footLength = toeIndex >= 0 ? clamp(distance(ankle, rest[toeIndex].translation), 0.02, 0.6 * height) : clamp(0.14 * height, 0.04, 0.5 * height);
    if (soleY === null) diagnostics.warnings.push(`No ${side} foot vertices; sole height uses a ${soleHeight.toFixed(3)} m fallback.`);
    legs.push({root: thigh, joint: calf, tip: foot, ...(toeIndex >= 0 ? {toe: toeIndex} : {}), pole, minBend: 0.01, maxBend: 2.9, soleHeight, footLength});
  }

  if (legs.length !== 2) {
    diagnostics.missing.push('complete biped leg chains');
    diagnostics.warnings.push('Procedural feet and turn stepping are disabled; base clips still import.');
    rig.confidence = rig.pelvis >= 0 ? 0.35 : 0.15;
    return rig;
  }
  const left = rest[legs.find(leg => sideOf(bones[leg.root].name) === 'left')?.root ?? legs[0].root].translation;
  const right = rest[legs.find(leg => sideOf(bones[leg.root].name) === 'right')?.root ?? legs[1].root].translation;
  const pelvis = rig.pelvis >= 0 ? rest[rig.pelvis].translation : [(left[0] + right[0]) * 0.5, 0, (left[2] + right[2]) * 0.5];
  const leftIsLeft = left[0] - pelvis[0] < 0, rightIsRight = right[0] - pelvis[0] > 0;
  if (!leftIsLeft && !rightIsRight && Math.abs(left[0] - right[0]) < 0.02 * height) {
    diagnostics.warnings.push('Left and right legs occupy the same side; rig mapping is ambiguous.');
    diagnostics.ambiguous.push('left/right rest sides');
    rig.legs = [];
    rig.confidence = 0.25;
    return rig;
  }
  rig.legs = legs;
  rig.confidence = clamp(0.55 + (rig.pelvis >= 0 ? 0.2 : 0) + (rig.lookBone >= 0 ? 0.1 : 0) + (legs.every(leg => leg.toe !== undefined) ? 0.1 : 0) - diagnostics.warnings.length * 0.03, 0.2, 1);
  if (rig.pelvis < 0) {
    const common = commonAncestor(bones, legs[0].root, legs[1].root);
    if (common >= 0) {
      rig.pelvis = common;
      diagnostics.warnings.push('Pelvis inferred from the thigh common ancestor.');
    }
  }
  return rig;
}

function commonAncestor(bones, a, b) {
  const seen = new Set();
  for (let i = a; i >= 0; i = bones[i].parent) seen.add(i);
  for (let i = b; i >= 0; i = bones[i].parent) if (seen.has(i)) return i;
  return -1;
}

const readTransform = (binary, at) => {
  const values = Array.from({length: 10}, (_, k) => binary.readInt32LE(at + k * 4) / 10000);
  return {
    translation: values.slice(0, 3),
    rotation: quaternion(values.slice(3, 7)).map(v => v),
    scale: values[7],
  };
};

function pathsToRoot(bones, index) {
  const path = [];
  for (let i = index; i >= 0; i = bones[i].parent) path.push(i);
  return path.reverse();
}

function globalPath(bones, binary, boneCount, path, frame) {
  const output = new Map();
  for (const bone of path) {
    const local = readTransform(binary, (frame * boneCount + bone) * 40);
    const parent = bones[bone].parent >= 0 ? output.get(bones[bone].parent) : null;
    output.set(bone, parent
      ? {translation: add(parent.translation, rotate(parent.rotation, mul(local.translation, parent.scale))), rotation: quaternion(product(parent.rotation, local.rotation)), scale: parent.scale * local.scale}
      : {translation: local.translation, rotation: local.rotation, scale: local.scale});
  }
  return output;
}

function longestTrueRun(mask, loop) {
  const n = mask.length;
  if (!n) return null;
  if (loop) {
    const doubled = [...mask, ...mask];
    let best = null, start = -1;
    for (let i = 0; i <= doubled.length; ++i) {
      if (i < doubled.length && doubled[i]) { if (start < 0) start = i; }
      else if (start >= 0) {
        const run = i - start;
        if (run > 0 && (!best || Math.min(run, n) > best.length)) best = {start: start % n, length: Math.min(run, n)};
        start = -1;
      }
    }
    return best;
  }
  let best = null, start = -1;
  for (let i = 0; i <= n; ++i) {
    if (i < n && mask[i]) { if (start < 0) start = i; }
    else if (start >= 0) {
      if (!best || i - start > best.length) best = {start, length: i - start};
      start = -1;
    }
  }
  return best;
}

function circularOverlap(a, b, n) {
  if (!a || !b) return 0;
  const mark = new Uint8Array(n);
  for (let k = 0; k < a.length; ++k) mark[(a.start + k) % n] = 1;
  let overlap = 0;
  for (let k = 0; k < b.length; ++k) if (mark[(b.start + k) % n]) ++overlap;
  return overlap;
}

// Conservative contact inference from foot clearance. It only returns a usable
// interval when the gait is clearly alternating (or both feet are planted).
export function inferFootContacts({bones, rig, binary, frameCount, boneCount, loop}) {
  if (rig.legs.length < 2 || frameCount < 3) return null;
  const paths = rig.legs.slice(0, 2).map(leg => pathsToRoot(bones, leg.tip));
  const tip = [0, 1].map(side => rig.legs[side].tip);
  const heights = [[], []], horizontal = [[], []];
  for (let frame = 0; frame < frameCount; ++frame) {
    for (let side = 0; side < 2; ++side) {
      const globals = globalPath(bones, binary, boneCount, paths[side], frame);
      const ankle = globals.get(tip[side]).translation, root = globals.get(0).translation;
      heights[side].push(ankle[1]);
      horizontal[side].push([ankle[0] - root[0], ankle[2] - root[2]]);
    }
  }
  const minHeight = Math.min(...heights[0], ...heights[1]);
  // Contact clearance follows the observed foot arc instead of a fixed sole
  // height: clips with a small lift still show a support phase, tall steps do
  // not classify the whole cycle as planted.
  const lift = Math.max(...heights[0], ...heights[1]) - minHeight;
  const clearance = Math.min(Math.max(0.012, lift * 0.4), 0.08 * (rig.settings?.height ?? 1.8));
  const masks = heights.map(values => values.map(y => y <= minHeight + clearance));
  for (let side = 0; side < 2; ++side) for (let frame = 0; frame < frameCount; ++frame) {
    const prev = masks[side][(frame + frameCount - 1) % frameCount], next = masks[side][(frame + 1) % frameCount];
    if (!loop && (frame === 0 || frame === frameCount - 1)) continue;
    if (masks[side][frame] && !prev && !next) masks[side][frame] = false;
    if (!masks[side][frame] && prev && next) masks[side][frame] = true;
  }
  const runs = masks.map(mask => longestTrueRun(mask, loop));
  const supportSpeeds = [];
  for (let side = 0; side < 2; ++side) for (let frame = 1; frame < frameCount; ++frame) {
    if (!masks[side][frame] || !masks[side][frame - 1]) continue;
    const a = horizontal[side][frame - 1], b = horizontal[side][frame];
    const speed = Math.hypot(b[0] - a[0], b[1] - a[1]) * 60;
    if (Number.isFinite(speed) && speed > 0.02) supportSpeeds.push(speed);
  }
  supportSpeeds.sort((a, b) => a - b);
  // Median support-phase foot speed is the clip's authored locomotion speed.
  // It lets the runtime play the gait at the speed the character actually moves.
  const locomotionSpeed = supportSpeeds.length >= 3
    ? Math.min(20, supportSpeeds[Math.floor(supportSpeeds.length / 2)])
    : 0;
  if (!runs[0] && !runs[1]) return {contacts: [[0, 0], [0, 0]], confidence: 0.2, source: 'inferred', locomotionSpeed: 0};
  if (!runs[0] || !runs[1]) return {contacts: [[0, 0], [0, 0]], confidence: 0.25, source: 'inferred', locomotionSpeed: 0};
  const denominator = Math.max(1, frameCount - 1);
  const phases = runs.map(run => {
    if (!run) return [0, 0];
    const endFrame = run.start + run.length;
    const start = run.start / denominator;
    const end = endFrame >= frameCount ? (loop && endFrame - frameCount > 0 ? (endFrame - frameCount) / denominator : 1) : endFrame / denominator;
    return [start, end];
  });
  const coverage = runs.map(run => run.length / frameCount);
  const planted = coverage[0] > 0.97 && coverage[1] > 0.97;
  let confidence = 0.55;
  if (planted) confidence = 0.98;
  else {
    if (Math.min(...coverage) < 0.08) confidence -= 0.25;
    const overlap = circularOverlap(runs[0], runs[1], frameCount) / Math.max(1, Math.min(runs[0].length, runs[1].length));
    if (overlap > 0.5) confidence -= 0.35;
    const margins = [0, 1].map(side => {
      let sum = 0;
      for (let k = 0; k < runs[side].length; ++k) {
        const frame = (runs[side].start + k) % frameCount;
        sum += clamp((minHeight + clearance - heights[side][frame]) / clearance, 0, 1);
      }
      return sum / runs[side].length;
    });
    confidence += 0.35 * (margins[0] + margins[1]) * 0.5;
  }
  return {contacts: phases, confidence: clamp(confidence, 0, 1), source: 'inferred', locomotionSpeed: planted ? 0 : locomotionSpeed};
}
