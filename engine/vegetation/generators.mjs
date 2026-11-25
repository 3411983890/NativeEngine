// Generator graph for the vegetation editor.
//
// A species used to be described only by scalars (height, crown radius, trunk radius) and
// the skeleton was a fixed recipe. That is enough to bake content and not enough to *edit*
// it: "this branch level splits 38 degrees with 18 children" has nowhere to live.
//
// This module defines that layer. It is deliberately free of imports so the editor page and
// the cook read the same defaults, and it keeps the old behaviour as the default: a species
// without `generators` still goes down the original code path, byte for byte, because the
// cook digests and the existing packages depend on that.

export const GENERATOR_SCHEMA = 'nativegpu.vegetation-generators/1';

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

function fail(message) {
  const error = new Error(message);
  error.code = 'NG-E-VEG-SPECIES';
  throw error;
}

const isFiniteNumber = (value) => typeof value === 'number' && Number.isFinite(value);
const isRange = (value) => Array.isArray(value) && value.length === 2
  && isFiniteNumber(value[0]) && isFiniteNumber(value[1]) && value[0] <= value[1];
const mid = (range) => (range[0] + range[1]) / 2;
// 1..depth inclusive, used to describe "every branch level" without hand-listing them.
const normalizedLevels = (depth) => Array.from({ length: Math.max(0, depth) }, (_, index) => index + 1);

// Trunk ratios differ between a broadleaf and a conifer: a conifer keeps a shorter, stiffer
// trunk and a dominant leader, a broadleaf spends more of its height before the first split.
export function defaultGenerators(spec) {
  const conifer = spec?.profile === 'conifer';
  const shrub = spec?.kind === 'shrub';
  const grass = spec?.kind === 'grass';
  // A grass clump is not a small tree: it is N blades from one base, with no branch levels at
  // all. Modelling it as a trunk with children produced a 5-level "tree" 0.4 m tall and left
  // grass as the only kind still running the legacy per-cylinder mesher.
  if (grass) {
    return {
      schema: GENERATOR_SCHEMA,
      levels: [{
        id: 'blade',
        type: 'trunk',
        level: 0,
        // `count` on a trunk is the number of stems from the base; for a clump that is the
        // blade count, which is exactly what `bladesPerClump` already meant.
        count: Math.max(1, Math.min(16, Math.round(spec?.dimensions?.bladesPerClump ?? 12))),
        startHeightRatio: 0.95,
        steps: 5,
        lean: 0.16,
        segmentTaper: 0.12,
        radiusRatio: 1,
        wind: 1,
      }],
      // Blades are the geometry, not foliage attached to it: a frond on a grass clump would
      // add leaf cards to something whose leaves already are the stems.
      fronds: [],
    };
  }
  const depthLimit = conifer ? 4 : (shrub ? 2 : 5);
  const levels = [{
    id: 'trunk',
    type: 'trunk',
    level: 0,
    // A shrub is several stems from one base, not one small trunk. Building it that way is what
    // makes `crownRadiusMetres` reachable: with a single trunk the limbs are parent-relative and
    // a 2.1 m shrub with a declared 1.5 m crown measured 0.77 m, because a 0.88 m trunk cannot
    // carry a 1.5 m crown however far the limbs spread.
    count: shrub ? 5 : 1,
    startHeightRatio: shrub ? 0.42 : (conifer ? 0.46 : 0.52),
    steps: 8,
    lean: 0.04,
    // Trunk taper is gentle: a 0.34 m trunk does not halve over 7 m, and a trunk that does
    // is the reason the old tree looked like a bundle of sticks.
    segmentTaper: 0.62,
    radiusRatio: 0.86,
    wind: 0.08,
  }];
  for (let level = 1; level <= depthLimit; ++level) {
    const first = level === 1;
    // Spread decays with depth. Holding it constant fans every level sideways, which is what
    // builds two flat rings of leaves instead of a crown: a real branch takes its parent's
    // direction and deviates less the further out it is.
    const decay = level - 1;
    levels.push({
      id: `branch_${level}`,
      type: 'branch',
      level,
      parent: level === 1 ? 'trunk' : `branch_${level - 1}`,
      count: first ? (conifer ? 4 : 3) : 2,
      spread: first
        ? (conifer ? [0.95, 1.25] : [0.72, 1.05])
        : [Math.max(0.16, 0.52 - decay * 0.08), Math.max(0.3, 0.92 - decay * 0.11)],
      // Children occupy the upper part of the parent, not its whole length: limbs that start
      // at the very bottom of the trunk read as a shrub, not a tree.
      attachArc: first ? [0.42, 1.0] : [0.55, 1.0],
      lengthRatio: shrub && first ? [0.9, 1.08] : [0.66, 0.82],
      // Two separate ratios. `childRadiusScale` is the child root against the parent radius
      // at the attachment; `segmentTaper` is the child's own tip/root ratio. Sharing one
      // number for both is what makes every level thin out at the same rate.
      childRadiusScale: 0.62,
      segmentTaper: 0.5,
      radiusRatio: 0.62,
      // Golden-angle phyllotaxis around the parent axis, plus a bounded upward bias so a
      // branch arcs out and then up instead of drooping like a willow.
      divergence: 2.399963,
      apicalDominance: first ? 0.55 : 0.3,
      upBias: 0.1,
      droop: 0.02 * level,
      // Wind weight climbs toward the tips; the editor paints over these per level.
      wind: clamp(0.15 + level * 0.17, 0, 1),
    });
  }
  return {
    schema: GENERATOR_SCHEMA,
    levels,
    fronds: [{
      id: 'frond_1',
      type: 'frond',
      // Foliage belongs on the outer levels. `depthLimit - 3` put it three levels in, which left
      // the last two levels of every default species as bare twigs sticking out of the crown —
      // visible as the dark spikes above a shrub and as the "feather" on a conifer. The authored
      // oak overrides this with explicit fronds; the default now matches what those fronds say.
      //
      // A conifer is the exception in the other direction: its needles run along the whole limb,
      // not only at the tip, so every level below the first one carries foliage. That is what
      // turns the "feather" into a cone.
      parentLevel: conifer ? 2 : Math.max(1, depthLimit - 1),
      hostLevels: conifer
        ? normalizedLevels(depthLimit).filter((level) => level >= 2)
        : (depthLimit >= 2 ? [Math.max(1, depthLimit - 1), depthLimit] : [1]),
      // Density is the difference between a crown that reads as a mass and a shell of
      // floating cards; the legacy recipe effectively ran 64 per segment plus a half-density
      // level below it.
      cardsPerSegment: conifer ? 72 : 96,
      cardWidth: spec?.dimensions?.leafWidthMetres ?? 0.34,
      cardHeight: spec?.dimensions?.leafHeightMetres ?? 0.46,
      scale: [0.8, 1.15],
      petioleLengthMetres: conifer ? 0.004 : 0.022,
      petioleAngle: 0.95,
      divergence: 2.399963,
      bladeSegments: 3,
      bladeCurl: 0.35,
      atlasRole: null,
      droop: [0.01, 0.09],
      wind: 0.95,
      // Shoot structure. Needles are carried on many short shoots along the limb, so a conifer
      // uses more leaves per shoot and almost no empty buds; a broadleaf carries fewer, longer
      // shoots and leaves a tenth of its buds bare, which is what opens real gaps in a crown.
      leavesPerShoot: conifer ? 8 : 5,
      shootLengthMetres: conifer ? 0.12 : 0.18,
      shootAngle: conifer ? 1.25 : 1.0,
      innerDensity: conifer ? 0.7 : 0.5,
      emptyShootFraction: conifer ? 0.02 : 0.1,
      minLeafSpacingMetres: conifer ? 0.02 : 0,
    }],
  };
}

// Normalizes whatever the file or the editor sent into one shape the builders can read
// without re-checking every field. Range fields accept a scalar (fixed value) or a pair.
export function normalizeGenerators(spec) {
  const source = spec?.generators;
  if (!source) return null;
  const levels = Array.isArray(source.levels) ? source.levels : null;
  if (!levels || levels.length === 0) fail('generators.levels must contain at least the trunk');

  const seen = new Set();
  const normalized = levels.map((entry, index) => {
    if (!entry || typeof entry !== 'object') fail(`generators.levels[${index}] must be an object`);
    const type = entry.type === 'trunk' ? 'trunk' : 'branch';
    const level = type === 'trunk' ? 0 : Math.round(Number(entry.level ?? index));
    if (!Number.isInteger(level) || level < 0 || level > 8) fail(`generators.levels[${index}].level out of range`);
    if (seen.has(level)) fail(`generators declares level ${level} twice`);
    seen.add(level);
    const id = String(entry.id ?? `${type}_${level}`);
    const count = clamp(Math.round(Number(entry.count ?? 1)), type === 'trunk' ? 1 : 1, 16);
    const spread = entry.spread ?? (type === 'trunk' ? 0 : [0.38, 0.72]);
    const lengthRatio = entry.lengthRatio ?? [0.58, 0.72];
    const spreadRange = isRange(spread) ? spread : [Number(spread), Number(spread)];
    const lengthRange = isRange(lengthRatio) ? lengthRatio : [Number(lengthRatio), Number(lengthRatio)];
    if (!isRange(spreadRange)) fail(`generators.levels[${index}].spread must be a number or [min,max]`);
    if (!isRange(lengthRange)) fail(`generators.levels[${index}].lengthRatio must be a number or [min,max]`);
    if (spreadRange[0] < 0 || spreadRange[1] > 1.5) fail(`generators.levels[${index}].spread out of range`);
    if (lengthRange[0] < 0.05 || lengthRange[1] > 1.4) fail(`generators.levels[${index}].lengthRatio out of range`);
    const attachArc = entry.attachArc ?? (type === 'trunk' ? [0, 0] : (level === 1 ? [0.42, 1.0] : [0.55, 1.0]));
    const attachRange = isRange(attachArc) ? attachArc : [Number(attachArc), Number(attachArc)];
    if (!isRange(attachRange) || attachRange[0] < 0 || attachRange[1] > 1) {
      fail(`generators.levels[${index}].attachArc must be a [min,max] pair inside 0..1`);
    }
    const childRadiusScale = Number(entry.childRadiusScale ?? entry.radiusRatio ?? 0.62);
    const segmentTaper = Number(entry.segmentTaper ?? (type === 'trunk' ? 0.62 : 0.5));
    if (!isFiniteNumber(childRadiusScale) || childRadiusScale <= 0.05 || childRadiusScale > 1) {
      fail(`generators.levels[${index}].childRadiusScale out of range`);
    }
    if (!isFiniteNumber(segmentTaper) || segmentTaper <= 0.05 || segmentTaper > 1) {
      fail(`generators.levels[${index}].segmentTaper out of range`);
    }
    return {
      id, type, level,
      parent: entry.parent ?? null,
      count,
      startHeightRatio: clamp(Number(entry.startHeightRatio ?? 0.42), 0.02, 0.98),
      steps: clamp(Math.round(Number(entry.steps ?? (type === 'trunk' ? 6 : 2))), 1, 16),
      lean: clamp(Number(entry.lean ?? 0.05), 0, 0.4),
      spread: spreadRange,
      lengthRatio: lengthRange,
      attachArc: attachRange,
      childRadiusScale,
      segmentTaper,
      divergence: clamp(Number(entry.divergence ?? 2.399963), 0, Math.PI * 2),
      apicalDominance: clamp(Number(entry.apicalDominance ?? 0.3), 0, 0.9),
      upBias: clamp(Number(entry.upBias ?? 0.1), -0.3, 0.5),
      droop: clamp(Number(entry.droop ?? 0), -0.2, 0.5),
      radiusRatio: clamp(Number(entry.radiusRatio ?? 0.62), 0.2, 1.0),
      wind: clamp(Number(entry.wind ?? 0.5), 0, 1),
    };
  }).sort((a, b) => a.level - b.level);

  if (normalized[0].type !== 'trunk' || normalized[0].level !== 0) fail('generators must start with a trunk at level 0');

  const fronds = (Array.isArray(source.fronds) ? source.fronds : []).map((entry, index) => {
    if (!entry || typeof entry !== 'object') fail(`generators.fronds[${index}] must be an object`);
    const parentLevel = Math.round(Number(entry.parentLevel ?? normalized[normalized.length - 1].level));
    if (!Number.isInteger(parentLevel) || parentLevel < 1 || parentLevel > normalized[normalized.length - 1].level) {
      fail(`generators.fronds[${index}].parentLevel must be an existing branch level`);
    }
    const cardWidth = clamp(Number(entry.cardWidth ?? 0.34), 0.01, 3);
    const cardHeight = clamp(Number(entry.cardHeight ?? 0.46), 0.01, 3);
    const droop = entry.droop ?? [0.01, 0.09];
    const droopRange = isRange(droop) ? droop : [Number(droop), Number(droop)];
    if (!isRange(droopRange)) fail(`generators.fronds[${index}].droop must be a number or [min,max]`);
    const scale = entry.scale ?? [0.8, 1.15];
    const scaleRange = isRange(scale) ? scale : [Number(scale), Number(scale)];
    if (!isRange(scaleRange) || scaleRange[0] <= 0 || scaleRange[1] > 4) {
      fail(`generators.fronds[${index}].scale must be a positive [min,max] pair`);
    }
    // A leaf is hung from a petiole of a real length, in metres. Zero is legal and means the
    // blade starts on the twig surface; the old random centre offset was neither.
    const petioleLength = Number(entry.petioleLengthMetres ?? 0.02);
    if (!isFiniteNumber(petioleLength) || petioleLength < 0 || petioleLength > 0.5) {
      fail(`generators.fronds[${index}].petioleLengthMetres out of range`);
    }
    // Shoot structure. Foliage is carried by short current-year axes, not painted onto the limb:
    // the count is leaves per metre (still `cardsPerSegment`), but they are grouped into shoots of
    // `leavesPerShoot` leaves with `emptyShootFraction` of buds left bare. Shaded shoots inside the
    // crown are shortened and thinned by `innerDensity` rather than by a global density cut.
    const leavesPerShoot = Math.round(Number(entry.leavesPerShoot ?? 5));
    if (!Number.isInteger(leavesPerShoot) || leavesPerShoot < 1 || leavesPerShoot > 12) {
      fail(`generators.fronds[${index}].leavesPerShoot must be an integer in [1,12]`);
    }
    const shootLengthMetres = Number(entry.shootLengthMetres ?? cardHeight * 1.1);
    if (!isFiniteNumber(shootLengthMetres) || shootLengthMetres < 0.02 || shootLengthMetres > 1.0) {
      fail(`generators.fronds[${index}].shootLengthMetres out of range`);
    }
    const shootAngle = Number(entry.shootAngle ?? 1.0);
    if (!isFiniteNumber(shootAngle) || shootAngle < 0 || shootAngle > Math.PI * 0.5) {
      fail(`generators.fronds[${index}].shootAngle out of range`);
    }
    const innerDensity = Number(entry.innerDensity ?? 0.5);
    if (!isFiniteNumber(innerDensity) || innerDensity < 0 || innerDensity > 1) {
      fail(`generators.fronds[${index}].innerDensity out of range`);
    }
    const emptyShootFraction = Number(entry.emptyShootFraction ?? 0.1);
    if (!isFiniteNumber(emptyShootFraction) || emptyShootFraction < 0 || emptyShootFraction > 0.6) {
      fail(`generators.fronds[${index}].emptyShootFraction out of range`);
    }
    const minLeafSpacingMetres = Number(entry.minLeafSpacingMetres ?? 0);
    if (!isFiniteNumber(minLeafSpacingMetres) || minLeafSpacingMetres < 0 || minLeafSpacingMetres > 1) {
      fail(`generators.fronds[${index}].minLeafSpacingMetres out of range`);
    }
    return {
      id: String(entry.id ?? `frond_${index + 1}`),
      type: 'frond',
      parentLevel,
      // Optional explicit host levels, so two fronds can target different branch levels
      // instead of both piling onto the same one.
      hostLevels: Array.isArray(entry.hostLevels) ? entry.hostLevels.map((value) => Math.round(Number(value))) : [parentLevel],
      cardsPerSegment: clamp(Math.round(Number(entry.cardsPerSegment ?? 64)), 0, 512),
      cardWidth, cardHeight,
      scale: scaleRange,
      petioleLengthMetres: petioleLength,
      // Angle between the petiole and the host branch tangent.
      petioleAngle: clamp(Number(entry.petioleAngle ?? 0.95), 0.05, Math.PI * 0.95),
      // Phyllotaxis around the host axis, radians per leaf.
      divergence: clamp(Number(entry.divergence ?? 2.399963), 0, Math.PI * 2),
      bladeSegments: clamp(Math.round(Number(entry.bladeSegments ?? 3)), 1, 6),
      bladeCurl: clamp(Number(entry.bladeCurl ?? 0.35), -1, 1),
      atlasRole: typeof entry.atlasRole === 'string' ? entry.atlasRole : null,
      droop: droopRange,
      wind: clamp(Number(entry.wind ?? 0.95), 0, 1),
      leavesPerShoot,
      shootLengthMetres,
      shootAngle,
      innerDensity,
      emptyShootFraction,
      minLeafSpacingMetres,
    };
  });

  return {
    schema: GENERATOR_SCHEMA,
    levels: normalized,
    fronds,
    byLevel: new Map(normalized.map((entry) => [entry.level, entry])),
    depthLimit: normalized[normalized.length - 1].level,
    // A level that declares wind 0..1 is a target for the tips of that level; the trunk is
    // the anchor everything else interpolates from.
    frondHostLevel: fronds.length ? Math.min(...fronds.map((frond) => frond.parentLevel)) : null,
    frondCount: fronds.reduce((sum, frond) => sum + frond.cardsPerSegment, 0),
    defaultSpread: (level) => {
      const entry = normalized.find((candidate) => candidate.level === level);
      return entry ? mid(entry.spread) : 0.55;
    },
  };
}

// Editor-side helper: fill in the parts of a graph that were never authored, then hand the
// result back as a plain JSON value (no Maps) so it can be written to a species file.
export function withDefaults(spec) {
  const base = defaultGenerators(spec);
  if (!spec?.generators) return base;
  const normalized = normalizeGenerators(spec);
  return {
    schema: GENERATOR_SCHEMA,
    levels: normalized.levels.map((entry) => ({
      id: entry.id, type: entry.type, level: entry.level, parent: entry.parent, count: entry.count,
      startHeightRatio: entry.startHeightRatio, steps: entry.steps, lean: entry.lean,
      spread: entry.spread, lengthRatio: entry.lengthRatio, attachArc: entry.attachArc,
      childRadiusScale: entry.childRadiusScale, segmentTaper: entry.segmentTaper,
      divergence: entry.divergence, apicalDominance: entry.apicalDominance,
      upBias: entry.upBias, droop: entry.droop,
      radiusRatio: entry.radiusRatio, wind: entry.wind,
    })),
    fronds: normalized.fronds.map((frond) => ({
      id: frond.id, type: 'frond', parentLevel: frond.parentLevel, hostLevels: frond.hostLevels,
      cardsPerSegment: frond.cardsPerSegment, cardWidth: frond.cardWidth, cardHeight: frond.cardHeight,
      scale: frond.scale, petioleLengthMetres: frond.petioleLengthMetres, petioleAngle: frond.petioleAngle,
      divergence: frond.divergence, bladeSegments: frond.bladeSegments, bladeCurl: frond.bladeCurl,
      atlasRole: frond.atlasRole, droop: frond.droop, wind: frond.wind,
      leavesPerShoot: frond.leavesPerShoot, shootLengthMetres: frond.shootLengthMetres,
      shootAngle: frond.shootAngle, innerDensity: frond.innerDensity,
      emptyShootFraction: frond.emptyShootFraction, minLeafSpacingMetres: frond.minLeafSpacingMetres,
    })),
  };
}
