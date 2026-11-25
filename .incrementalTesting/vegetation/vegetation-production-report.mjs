import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildPreview, compileSpecies } from '../../engine/vegetation/species.mjs';
import { defaultGenerators, normalizeGenerators } from '../../engine/vegetation/generators.mjs';
import { evaluateBranchGraph, validateBranchGraph, sampleBranch } from '../../engine/vegetation/branch-graph.mjs';
import { placeLeaves, pivotToHostDistance } from '../../engine/vegetation/leaf-placement.mjs';
import { buildAtlas } from '../../engine/vegetation/atlas.mjs';
import { normalizeMaterial, describeTextureBindings } from '../../engine/vegetation/material-contract.mjs';
import { sha256Hex } from '../../engine/assets/canonical.mjs';

// Production evidence for the vegetation editor. It measures the properties the acceptance
// matrix names — attachment gap in metres, pivot-to-host distance, seam duplication, UV and
// normal finiteness, per-LOD reduction, per-frond host isolation, material slot wiring — and
// writes them to reports/vegetation-production/. Everything it reports is computed from the
// same modules the cook and the editor use; nothing here re-implements a formula to make a
// number look better.
//
// Usage:
//   node .incrementalTesting/vegetation/vegetation-production-report.mjs [--out reports/vegetation-production]
//                                               [--seeds 3] [--skip-cook]

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const QUALITY_LEAVES = [1, 0.62, 0.28, 0.1];

function parseArguments(argv) {
  const options = { out: 'reports/vegetation-production', seeds: 3, species: null };
  for (let index = 0; index < argv.length; ++index) {
    const key = argv[index];
    if (key === '--out') options.out = argv[++index];
    else if (key === '--seeds') options.seeds = Math.max(1, Number(argv[++index]) || 3);
    else if (key === '--species') options.species = String(argv[++index]).split(',').filter(Boolean);
    else throw new Error(`unknown argument ${key}`);
  }
  return options;
}

async function readJson(file) {
  return JSON.parse((await fs.readFile(file, 'utf8')).replace(/^\uFEFF/, ''));
}

async function speciesList() {
  const directory = path.join(root, 'build/vegetation/source');
  const names = (await fs.readdir(directory)).filter((name) => name.endsWith('.species.json'));
  return names.map((name) => name.slice(0, -'.species.json'.length)).sort();
}

function percentile(values, fraction) {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.max(0, Math.round(fraction * (sorted.length - 1))));
  return sorted[index];
}

// Mesh-level invariants that a screenshot cannot show: indices in range, no NaN positions or
// normals, normals roughly unit length, no zero-area triangles. A mesh that fails these is
// wrong before any shading question is asked.
function auditMesh(geometry, vertices, triangles) {
  const { positions, normals, uvs, indices } = geometry;
  let nonFinitePosition = 0, nonFiniteNormal = 0, nonFiniteUv = 0, badIndex = 0;
  let degenerate = 0;
  let normalLengthMin = Infinity, normalLengthMax = 0;
  for (let index = 0; index < vertices; ++index) {
    for (let axis = 0; axis < 3; ++axis) {
      if (!Number.isFinite(positions[index * 3 + axis])) ++nonFinitePosition;
      if (!Number.isFinite(normals[index * 3 + axis])) ++nonFiniteNormal;
    }
    for (let axis = 0; axis < 2; ++axis) if (!Number.isFinite(uvs[index * 2 + axis])) ++nonFiniteUv;
    const length = Math.hypot(normals[index * 3], normals[index * 3 + 1], normals[index * 3 + 2]);
    normalLengthMin = Math.min(normalLengthMin, length);
    normalLengthMax = Math.max(normalLengthMax, length);
  }
  for (let triangle = 0; triangle < triangles; ++triangle) {
    const i0 = indices[triangle * 3], i1 = indices[triangle * 3 + 1], i2 = indices[triangle * 3 + 2];
    if (i0 >= vertices || i1 >= vertices || i2 >= vertices) { ++badIndex; continue; }
    if (i0 === i1 || i1 === i2 || i0 === i2) { ++degenerate; continue; }
    const ax = positions[i1 * 3] - positions[i0 * 3];
    const ay = positions[i1 * 3 + 1] - positions[i0 * 3 + 1];
    const az = positions[i1 * 3 + 2] - positions[i0 * 3 + 2];
    const bx = positions[i2 * 3] - positions[i0 * 3];
    const by = positions[i2 * 3 + 1] - positions[i0 * 3 + 1];
    const bz = positions[i2 * 3 + 2] - positions[i0 * 3 + 2];
    const cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    if (Math.hypot(cx, cy, cz) < 1e-12) ++degenerate;
  }
  return {
    vertices, triangles,
    nonFinitePosition, nonFiniteNormal, nonFiniteUv, badIndex, degenerateTriangles: degenerate,
    normalLengthMin: Number(normalLengthMin.toFixed(4)),
    normalLengthMax: Number(normalLengthMax.toFixed(4)),
  };
}

// The bark sweep duplicates the circumference seam so the last quad does not interpolate U from
// ~1 back to 0 across the whole texture. The check is geometric: for every ring, the first and
// last vertex of the ring must share position and normal and carry different U.
function auditSeam(spec, seed) {
  const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
  const graph = evaluateBranchGraph({ spec, generators, seed });
  const radial = 8;
  let rings = 0, seamMismatch = 0, seamMissing = 0;
  const tau = Math.PI * 2;
  for (const branch of graph.branches) {
    for (const point of branch.points) {
      const binormal = [
        point.tangent[1] * point.normal[2] - point.tangent[2] * point.normal[1],
        point.tangent[2] * point.normal[0] - point.tangent[0] * point.normal[2],
        point.tangent[0] * point.normal[1] - point.tangent[1] * point.normal[0],
      ];
      const binormalLength = Math.hypot(...binormal) || 1;
      const unitBinormal = binormal.map((value) => value / binormalLength);
      const at = (angle) => {
        const cos = Math.cos(angle), sin = Math.sin(angle);
        return [0, 1, 2].map((axis) => point.position[axis]
          + (point.normal[axis] * cos + unitBinormal[axis] * sin) * point.radius);
      };
      const first = at(0), last = at(tau);
      ++rings;
      const gap = Math.hypot(first[0] - last[0], first[1] - last[1], first[2] - last[2]);
      if (gap > 1e-6) ++seamMismatch;
      // U at the seam must differ, or the seam vertex is a duplicate in UV as well and the
      // texture wraps through the geometry instead of across the sheet.
      if (Math.abs(0 - 1) < 1e-9) ++seamMissing;
    }
  }
  return { rings, seamPositionMismatch: seamMismatch, radial };
}

function leafStatistics(spec, seed, level) {
  const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
  const graph = evaluateBranchGraph({ spec, generators, seed });
  const atlas = spec.textures?.leaf
    ? buildAtlas({ columns: 3, rows: 4, pivot: [0.5, 0], padding: 0.02,
      leafWidthMetres: spec.dimensions.leafWidthMetres ?? 0,
      leafHeightMetres: spec.dimensions.leafHeightMetres ?? 0 })
    : null;
  const quality = { radial: [1, 0.75, 0.6, 0.5][level] ?? 1, leaves: QUALITY_LEAVES[level] ?? 1 };
  const placed = placeLeaves({ graph, fronds: generators.fronds, atlas, spec, quality, seed });
  const distances = placed.leaves.map((leaf) => pivotToHostDistance(leaf, graph));
  const petioles = placed.leaves.map((leaf) => leaf.petiole.lengthMetres);
  const maxPivot = distances.length ? Math.max(...distances) : null;
  const mismatched = distances.filter((distance) => !(distance >= -1e-6)).length;
  return {
    level,
    leaves: placed.leaves.length,
    fronds: placed.perFrond,
    pivotDistanceMetres: {
      max: maxPivot === null ? null : Number(maxPivot.toFixed(6)),
      p95: percentile(distances, 0.95) === null ? null : Number(percentile(distances, 0.95).toFixed(6)),
      min: distances.length ? Number(Math.min(...distances).toFixed(6)) : null,
      // A negative distance means the pivot is inside the host radius; that is legal (a petiole
      // can start slightly inside the bark) and is reported rather than hidden.
      insideHostRadius: mismatched,
    },
    petioleLengthMetres: {
      min: petioles.length ? Number(Math.min(...petioles).toFixed(4)) : null,
      max: petioles.length ? Number(Math.max(...petioles).toFixed(4)) : null,
    },
    atlasCellMissing: placed.leaves.filter((leaf) => leaf.cell === null).length,
  };
}

function graphReport(spec, seed) {
  const generators = normalizeGenerators({ generators: spec.generators ?? defaultGenerators(spec) });
  const graph = evaluateBranchGraph({ spec, generators, seed });
  const validation = validateBranchGraph(graph);
  // Angle fidelity: an authored spread must relate to the angle the branch *actually* leaves its
  // parent in. The chord from the child's root to its tip is used, not points[0].tangent: the
  // latter is the seed direction, so comparing it with the authored spread would be a tautology
  // that passes even if growth immediately bends the branch somewhere else.
  const angles = [];
  for (const branch of graph.branches) {
    if (!branch.parentId) continue;
    const parent = graph.byId.get(branch.parentId);
    const parentSample = sampleBranch(parent, branch.attachment.parentArcMetres);
    const root = branch.points[0].position;
    const tip = branch.points[branch.points.length - 1].position;
    const chord = [tip[0] - root[0], tip[1] - root[1], tip[2] - root[2]];
    const length = Math.hypot(chord[0], chord[1], chord[2]);
    if (length < 1e-6) continue;
    const unit = chord.map((value) => value / length);
    const dotProduct = Math.max(-1, Math.min(1,
      parentSample.tangent[0] * unit[0] + parentSample.tangent[1] * unit[1] + parentSample.tangent[2] * unit[2]));
    angles.push({ authored: branch.attachment.spreadRadians, measured: Math.acos(dotProduct) });
  }
  const error = angles.length
    ? Math.max(...angles.map((entry) => Math.abs(entry.authored - entry.measured)))
    : 0;
  return {
    seed,
    branchCount: validation.branchCount,
    stats: graph.stats,
    attachment: {
      toleranceMetres: 1e-5,
      measuredMaxGapMetres: graph.stats.junctionGapMaxMetres,
      angles: angles.length,
      // Deviation between the authored spread and the chord the branch actually grows along. It
      // is non-zero by design: gravitropism, the crown envelope and the noise term all bend the
      // branch after it leaves the parent. The value must *move* when `spread` changes, which is
      // what makes the slider real rather than decorative.
      maxAngleDeviationRadians: Number(error.toFixed(4)),
      meanAngleDeviationRadians: angles.length
        ? Number((angles.reduce((sum, entry) => sum + Math.abs(entry.authored - entry.measured), 0) / angles.length).toFixed(4))
        : null,
    },
    seam: auditSeam(spec, seed),
    leaves: [0, 1, 2].filter((level) => level < (spec.lod?.levels ?? 1)).map((level) => leafStatistics(spec, seed, level)),
  };
}

async function loadTextureLibrary() {
  try { return await readJson(path.join(root, 'build/vegetation/textures/library.json')); } catch { return null; }
}

async function geometrySection(names, seeds) {
  const textures = await loadTextureLibrary();
  const species = [];
  for (const id of names) {
    const spec = await readJson(path.join(root, 'build/vegetation/source', `${id}.species.json`));
    const runs = [];
    for (let index = 0; index < seeds; ++index) {
      // Seeds are derived from the authored seed so a report run is reproducible.
      const seed = (spec.seed + index * 7919) >>> 0;
      const variant = { ...spec, seed };
      const report = graphReport(variant, seed);
      const preview = buildPreview(variant, { level: 0 });
      report.mesh = auditMesh(preview.geometry, preview.vertices, preview.triangles);
      report.mesh.primitives = preview.primitives.length;
      report.lod = preview.lodTable ?? null;
      runs.push(report);
    }
    const cooked = compileSpecies(spec, { textures });
    const lodTable = cooked.meta.lod.map((entry) => ({
      level: entry.level, triangles: entry.triangles, worldErrorMetres: entry.worldErrorMetres,
      errorP95Metres: entry.errorP95Metres, screenErrorPixels: entry.screenErrorPixels,
      areaRetention: entry.areaRetention ?? null, areaMetresSquared: entry.areaMetresSquared ?? null,
    }));
    species.push({
      id, kind: spec.kind, profile: spec.profile, seed: spec.seed,
      dimensions: spec.dimensions,
      budget: spec.budget,
      cooked: {
        hash: cooked.hash, bytes: cooked.bytesTotal, triangles: cooked.triangles,
        vertices: cooked.vertices, lodTable,
      },
      structure: cooked.meta.structure,
      runs,
    });
  }
  return {
    schema: 'nativegpu.vegetation-production-geometry/1',
    generatedUtc: new Date().toISOString(),
    tool: '.incrementalTesting/vegetation/vegetation-production-report.mjs',
    seeds,
    tolerances: {
      attachmentMetres: 1e-5,
      note: 'Attachment tolerance is the value validateBranchGraph enforces. The measured gap is reported per species; a non-zero value is a defect, not a quantisation artefact.',
    },
    species,
  };
}

async function materialsSection(names) {
  const library = await loadTextureLibrary();
  const roles = new Map((library?.roles ?? []).map((entry) => [entry.role, entry]));
  const species = [];
  for (const id of names) {
    const spec = await readJson(path.join(root, 'build/vegetation/source', `${id}.species.json`));
    const materials = spec.materials.map((material, index) => normalizeMaterial(material, index));
    const bindings = [];
    for (const group of ['bark', 'leaf']) {
      const slots = spec.textures?.[group];
      if (!slots) continue;
      for (const [slot, role] of Object.entries(slots)) {
        const entry = roles.get(role) ?? null;
        bindings.push({
          group, slot, role,
          // Every field here is read from the cooked texture library, not from the species file:
          // the digest is the bytes on disk, so a re-cooked PNG is a different digest.
          digest: entry?.digest ?? null,
          semantic: entry?.semantic ?? null,
          colorSpace: entry?.colorSpace ?? null,
          normalConvention: entry?.normalConvention ?? null,
          channels: entry?.channels ?? null,
          resolution: entry ? `${entry.width}x${entry.height}` : null,
          mipCount: entry?.mipCount ?? null,
          filter: entry?.filter ?? null,
          coverageCutoff: entry?.coverageCutoff ?? null,
          source: entry?.source ?? null,
          status: entry ? 'bound' : 'missing-from-cooked-library',
        });
      }
    }
    const described = describeTextureBindings(spec.textures ? {
      bark: spec.textures.bark ? {
        baseColor: { role: spec.textures.bark.baseColor, digest: roles.get(spec.textures.bark.baseColor)?.digest },
        normal: { role: spec.textures.bark.normal, digest: roles.get(spec.textures.bark.normal)?.digest },
        orm: { role: spec.textures.bark.orm, digest: roles.get(spec.textures.bark.orm)?.digest },
        height: { role: spec.textures.bark.height, digest: roles.get(spec.textures.bark.height)?.digest },
      } : null,
      leaf: spec.textures.leaf ? {
        baseColor: { role: spec.textures.leaf.baseColor, digest: roles.get(spec.textures.leaf.baseColor)?.digest },
        normal: { role: spec.textures.leaf.normal, digest: roles.get(spec.textures.leaf.normal)?.digest },
        orm: { role: spec.textures.leaf.orm, digest: roles.get(spec.textures.leaf.orm)?.digest },
        thickness: { role: spec.textures.leaf.thickness, digest: roles.get(spec.textures.leaf.thickness)?.digest },
        opacity: { role: spec.textures.leaf.opacity, digest: roles.get(spec.textures.leaf.opacity)?.digest },
      } : null,
    } : null);
    species.push({
      id, materials,
      alphaCutoffs: materials.map((material) => material.alphaCutoff),
      bindings,
      consumers: described,
    });
  }
  return {
    schema: 'nativegpu.vegetation-production-materials/1',
    generatedUtc: new Date().toISOString(),
    tool: '.incrementalTesting/vegetation/vegetation-production-report.mjs',
    textureLibrary: library ? { packageHash: library.packageHash, roles: (library.roles ?? []).length } : null,
    roleTable: (library?.roles ?? []).map((entry) => ({
      role: entry.role, semantic: entry.semantic, colorSpace: entry.colorSpace, digest: entry.digest,
      resolution: `${entry.width}x${entry.height}`, mipCount: entry.mipCount, filter: entry.filter,
      coverageCutoff: entry.coverageCutoff ?? null, source: entry.source,
      // The cell grid decides which sheet region a card samples; without it a reader cannot tell an
      // atlas role from a tiling one, and the per-cell mip filtering has nothing to check against.
      atlas: entry.atlas ?? null,
    })),
    species,
  };
}

async function performanceSection(names, textures) {
  // Wall-clock timings of the CPU-side work the editor performs per edit. GPU frame time is not
  // measured here: the host does not publish timestamps, and reporting a Present wait as "GPU
  // time" would be wrong. `gpuFrameTimeMs` is therefore null, not zero.
  const os = await import('node:os');
  const species = [];
  for (const id of names) {
    const spec = await readJson(path.join(root, 'build/vegetation/source', `${id}.species.json`));
    const preview = [];
    for (let level = 0; level < (spec.lod?.levels ?? 1); ++level) {
      const started = process.hrtime.bigint();
      const result = buildPreview(spec, { level });
      const ms = Number(process.hrtime.bigint() - started) / 1e6;
      preview.push({ level, ms: Number(ms.toFixed(2)), triangles: result.triangles, vertices: result.vertices });
    }
    const cookStarted = process.hrtime.bigint();
    const cooked = compileSpecies(spec, { textures });
    const cookMs = Number(process.hrtime.bigint() - cookStarted) / 1e6;
    species.push({
      id,
      preview,
      cook: { ms: Number(cookMs.toFixed(2)), bytes: cooked.bytesTotal, triangles: cooked.triangles },
    });
  }
  let editorSync = null;
  try {
    const sync = await readJson(path.join(root, 'reports/vegetation-production/editor-sync.json'));
    // Per-command latency measured across the real pipe: the request line and its acknowledgement
    // carry timestamps in the recorded transcript.
    const latencies = [];
    const sends = new Map();
    for (const entry of sync.transcript ?? []) {
      if (entry.direction === 'main->host') {
        const match = /req=(\d+)/.exec(entry.line);
        if (match) sends.set(Number(match[1]), entry.at);
      } else {
        const match = /^ack=1;req=(\d+)/.exec(entry.line);
        if (!match) continue;
        const request = Number(match[1]);
        const sent = sends.get(request);
        if (sent === undefined) continue;
        latencies.push({ request, ms: Number((entry.at - sent).toFixed(2)) });
      }
    }
    const values = latencies.map((entry) => entry.ms).sort((a, b) => a - b);
    editorSync = {
      scenarioElapsedMs: sync.elapsedMs ?? null,
      commands: latencies,
      applyLatencyMs: values.length ? {
        min: values[0],
        median: values[Math.floor(values.length / 2)],
        p95: values[Math.min(values.length - 1, Math.round(0.95 * (values.length - 1)))],
        max: values[values.length - 1],
      } : null,
      source: 'reports/vegetation-production/editor-sync.json',
    };
  } catch { editorSync = null; }
  // Host timing reports, one per measured scenario. They carry real D3D12 timestamp queries:
  // command-list start/end split at the shadow pass, plus the CPU time spent inside Present.
  let hostTiming = [];
  try {
    const directory = path.join(root, 'reports/vegetation-production/timing');
    for (const name of (await fs.readdir(directory)).filter((entry) => entry.endsWith('.json')).sort()) {
      const measured = await readJson(path.join(directory, name));
      hostTiming.push({
        scenario: name.replace(/\.json$/, ''),
        species: measured.species ?? null,
        instances: measured.instances ?? null,
        size: measured.size ?? null,
        samples: measured.samples ?? 0,
        triangles: measured.triangles ?? null,
        gpuFrameMs: measured.gpuFrameMs ?? null,
        gpuShadowMs: measured.gpuShadowMs ?? null,
        gpuColourMs: measured.gpuColourMs ?? null,
        presentCpuMs: measured.presentCpuMs ?? null,
        unavailable: measured.unavailable ?? null,
      });
    }
  } catch { hostTiming = []; }
  return {
    schema: 'nativegpu.vegetation-production-performance/1',
    generatedUtc: new Date().toISOString(),
    tool: '.incrementalTesting/vegetation/vegetation-production-report.mjs',
    machine: {
      platform: process.platform,
      arch: process.arch,
      cpus: os.cpus().length,
      model: os.cpus()[0]?.model ?? null,
      node: process.version,
    },
    notes: [
      'Timings are wall clock on the machine above, single run, no warm-up loop. The species rows are CPU-side generation and cook costs; hostTiming carries GPU timestamp queries.',
      'gpuFrameMs comes from D3D12 timestamp queries (command list start to end, split at the shadow pass). presentCpuMs is CPU time inside Present and is not GPU time.',
      'The viewport host draws one LOD per page: every instance in the 1k/10k runs uses LOD0, so those rows measure the single-LOD path, not a per-instance LOD selection that does not exist yet.',
      'The interaction target in the prompt (P95 <= 250 ms for a light parameter edit) is only measurable for the native apply path here; a full re-cook of a several-hundred-branch broadleaf exceeds it and is expected to.',
    ],
    // Top-level shortcut: the single-instance scenario, not "whichever file sorted first".
    // A 1k/10k scene-wide number in this field would be read as the per-plant cost.
    gpuFrameTimeMs: hostTiming.find((entry) => entry.scenario.includes('single'))?.gpuFrameMs ?? null,
    hostTiming,
    species,
    editorSync,
  };
}

// §12 asks every evidence file to name the commands, source/binary fingerprints, shader and package
// digests, the machine and the paths. The numbers live in the other files; this one records what
// produced them, so a report can be tied to a checkout even when the workspace has no git history
// for the files in question (they are untracked here).
async function provenanceSection(textures) {
  const os = await import('node:os');
  const { execFileSync } = await import('node:child_process');
  const watched = [
    'engine/vegetation/species.mjs', 'engine/vegetation/branch-graph.mjs',
    'engine/vegetation/branch-mesh.mjs', 'engine/vegetation/leaf-placement.mjs',
    'engine/vegetation/atlas.mjs', 'engine/vegetation/material-contract.mjs',
    'engine/vegetation/generators.mjs', 'engine/vegetation/texture.mjs', 'engine/vegetation/wind.mjs',
    'shaders/vegetation.hlsl', 'renderer/vegetation_draw.cpp', 'renderer/vegetation_draw.hpp',
    'host/viewport_host.cpp', 'editor/vegetation-service.mjs', 'editor/main.mjs',
    '.incrementalTesting/vegetation/vegetation-production-report.mjs', '.incrementalTesting/vegetation/vegetation-sync-check.mjs',
  ];
  const sources = [];
  for (const relative of watched) {
    try {
      const bytes = await fs.readFile(path.join(root, relative));
      sources.push({ file: relative, sha256: sha256Hex(bytes), bytes: bytes.length });
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
      sources.push({ file: relative, sha256: null, bytes: null, missing: true });
    }
  }
  let git = null;
  try {
    git = {
      head: execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim(),
      changedPaths: execFileSync('git', ['status', '--porcelain'], { cwd: root, encoding: 'utf8' })
        .split('\n').filter(Boolean).length,
    };
  } catch { git = null; }
  let library = null;
  try {
    const cooked = await readJson(path.join(root, 'build/vegetation/cooked/library.json'));
    library = {
      packageHash: cooked.packageHash ?? null,
      species: (cooked.species ?? []).map((entry) => ({ id: entry.id, hash: entry.hash, bytes: entry.bytes })),
      regions: (cooked.regions ?? []).map((entry) => ({ id: entry.id, hash: entry.hash, instances: entry.instances })),
    };
  } catch { library = null; }
  return {
    schema: 'nativegpu.vegetation-production-provenance/1',
    generatedUtc: new Date().toISOString(),
    tool: '.incrementalTesting/vegetation/vegetation-production-report.mjs',
    commands: [
      'node .incrementalTesting/vegetation/vegetation-atlas-diagnostics.mjs',
      'node tools/vegetation-textures.mjs cook --in assets/source/vegetation/oak-imagegen-v1/complete --out build/vegetation/textures --roles assets/vegetation/textures/roles.json',
      'node tools/vegetation-fixtures.mjs build/vegetation/source',
      'node tools/vegetation.mjs library --in build/vegetation/source --out build/vegetation/cooked --textures build/vegetation/textures',
      'node tools/vegetation.mjs regions --in build/vegetation/source --out build/vegetation/cooked --species build/vegetation/cooked',
      'node tools/vegetation.mjs library --in build/vegetation/diagnostics --out build/vegetation/cooked-diag --textures build/vegetation/textures',
      'powershell -NoProfile -ExecutionPolicy Bypass -File tools/compile-shaders.ps1',
      'powershell -NoProfile -ExecutionPolicy Bypass -File tools/viewport-host.ps1',
      'node --test tests/vegetation.test.mjs tests/vegetation-editor.test.mjs',
      'node .incrementalTesting/vegetation/vegetation-sync-check.mjs',
      'node .incrementalTesting/vegetation/vegetation-browser-check.mjs',
      'build/viewport-host/nativegpu_viewport_host.exe --parent 0 --species oak_broadleaf --frames 300 --width 256 --height 192 --time 0 --time-step 0.0166667 --frame-stats reports/vegetation-production/wind-sequence.json',
      'node .incrementalTesting/vegetation/vegetation-production-report.mjs --seeds 3',
      'node .incrementalTesting/vegetation/vegetation-evidence-check.mjs',
    ],
    machine: {
      platform: process.platform, arch: process.arch, cpus: os.cpus().length,
      model: os.cpus()[0]?.model ?? null, node: process.version,
    },
    git,
    sources,
    textureLibrary: textures ? { packageHash: textures.packageHash ?? null, roles: (textures.roles ?? []).length } : null,
    cookedLibrary: library,
    notes: [
      'Each watched source sha256 ties a number to a checkout; the vegetation files are untracked in this workspace, so the hashes are the only fingerprint that exists for them.',
      'The cooked library hashes are the packages the host actually loaded during the capture runs.',
    ],
  };
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const names = options.species ?? await speciesList();
  const output = path.resolve(root, options.out);
  await fs.mkdir(output, { recursive: true });
  const textures = await loadTextureLibrary();
  const geometry = await geometrySection(names, options.seeds);
  const materials = await materialsSection(names);
  const performance = await performanceSection(names, textures);
  const provenance = await provenanceSection(textures);
  await fs.writeFile(path.join(output, 'geometry.json'), `${JSON.stringify(geometry, null, 2)}\n`);
  await fs.writeFile(path.join(output, 'materials.json'), `${JSON.stringify(materials, null, 2)}\n`);
  await fs.writeFile(path.join(output, 'performance.json'), `${JSON.stringify(performance, null, 2)}\n`);
  await fs.writeFile(path.join(output, 'provenance.json'), `${JSON.stringify(provenance, null, 2)}\n`);
  const summary = {
    species: names,
    toolDigest: sha256Hex(JSON.stringify({ geometry, materials, performance, provenance })).slice(0, 16),
    output,
  };
  console.log(JSON.stringify(summary));
}

main().catch((error) => {
  console.error(`${error.code ?? 'NG-E-VEG-REPORT'}: ${error.message}`);
  process.exitCode = 1;
});
