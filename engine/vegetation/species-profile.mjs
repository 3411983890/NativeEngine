// Species profiles: the part of a tree's identity that is not a random draw.
//
// Before this module every broadleaf was "an ellipsoid plus numbers", so a pine and an oak
// differed only in scale. A profile states the shape family once — where the crown starts, how
// wide it is at the top, how it tapers — and the generator consumes it instead of testing the
// species name at each site.
//
// Scope note: this is the *morphology* half of the profile in the task book (§4.1). Age bands,
// branch-order distributions and material identities are not here yet; they land with the
// generator work that consumes them, rather than being declared now and never read.

export const PROFILE_SCHEMA = 'nativegpu.vegetation-profile/1';

// Envelope shapes, expressed as the parameters the growth shell actually uses:
//   shape 'ellipsoid': radius = crown * sqrt(1 - t^2) with t = (y - centre) / halfHeight
//   shape 'cone':      radius = crown * max(apexFloor, (1 - t)^taper) with t measured from crownBase
//   shape 'cylinder':  radius = crown at every height (a grass clump)
//
// `crownRadiusPerHeight` is what makes the crown a *shape* rather than a bound. The shell scale is
// derived from the plant's height and this species trait, never from `crownRadiusMetres`: that
// declaration is the author's expectation and is validated after growth (see crownMismatch).
// Before the split, reducing the declared crown bent every branch inward — a declaration silently
// acted as a clamp, and "the tree reached its declared crown" was a property of the clamp.
//
// Values are measured outcomes, not taste: oak 13.5 m / 5.3 m ≈ 0.39, pine 18 m / 3.6 m = 0.20,
// hazel 2.1 m / 1.5 m ≈ 0.71, grass 0.42 m / 0.19 m ≈ 0.45 (reports/vegetation-realism/morphology.json).
const PROFILES = {
  broadleaf: {
    profile: 'broadleaf',
    crownRadiusPerHeight: 0.39,
    envelope: { shape: 'ellipsoid', centreFraction: 0.58, halfHeightFraction: 0.46 },
    // A broadleaf keeps a living leader but its crown is not a spike: the top tapers over the
    // last fifth rather than being cut off.
    heightSuppressionFraction: 0.1,
    // No modulation: [1, 1] means the authored generator numbers are used as written, which keeps
    // existing broadleaf trees bit-identical to before this table existed.
    branchOrder: { lengthAlongParent: [1, 1], spreadAlongParent: [1, 1] },
  },
  conifer: {
    profile: 'conifer',
    crownRadiusPerHeight: 0.2,
    envelope: {
      shape: 'cone',
      crownBaseFraction: 0.22,
      // 0.6 instead of 0.85: the cone narrows more slowly, which is what a spruce actually does
      // near its top. Measured reason: at 0.85 the apex level had 62% of its growth steps being
      // corrected by the shell, and every correction is a turn.
      taper: 0.6,
      // A conifer apex is a leader with needles on it, not a mathematical point. Without this
      // floor the shell radius collapses to zero near the tip, every branch slams into it, and
      // the apical region turns into a zigzag.
      apexFloorFraction: 0.12,
    },
    heightSuppressionFraction: 0.12,
    // Apical dominance as a direction: children attached at fraction f along their parent are
    // biased toward +Y by this × f². Zero for every other profile, where the authored spread is the
    // whole story.
    apicalUpBias: 0.9,
    // Apical dominance, stated where it belongs instead of being tuned inside the growth loop:
    // low branches are long and near-horizontal (they carry the crown's width), branches near the
    // leader are short and steeply angled (they form the spire). Without this, every level sampled
    // the same spread and length, so the cone had to be enforced by the shell alone — which is
    // what produced the zigzag near the apex and a crown that never reached its declared radius.
    // Apical dominance, stated where it belongs instead of being tuned inside the growth loop:
    // low branches are long and near-horizontal (they carry the crown's width), branches near the
    // leader are short (they form the spire).
    //
    // A spread multiplier was tried here too (×1.15 low, ×0.45 high) and measured worse — 22 hard
    // turns and the same 58/96 corrected apex steps — because `spread` is the angle to the *parent
    // tangent*, and at high levels the parent tangent is itself pointing outward. Scaling that
    // angle keeps the child outward. Directional apical dominance has to be stated as "bias toward
    // the vertical axis", not as "scale the spread"; see acceptance.md R1-14.
    branchOrder: { lengthAlongParent: [1.5, 0.85], spreadAlongParent: [1, 1] },
  },
  shrub: {
    profile: 'shrub',
    crownRadiusPerHeight: 0.71,
    envelope: { shape: 'ellipsoid', centreFraction: 0.48, halfHeightFraction: 0.55 },
    heightSuppressionFraction: 0.14,
    branchOrder: { lengthAlongParent: [1, 1], spreadAlongParent: [1, 1] },
  },
  grass: {
    profile: 'grass',
    crownRadiusPerHeight: 0.45,
    envelope: { shape: 'cylinder' },
    heightSuppressionFraction: 0.2,
    branchOrder: { lengthAlongParent: [1, 1], spreadAlongParent: [1, 1] },
  },
};

export const PROFILE_NAMES = Object.keys(PROFILES);

// `kind` is the coarse category older sources carry (tree / shrub / grass); `profile` is the
// species family. A source that predates profiles still resolves through its category, but a
// *named* profile that does not exist is still an error — the mapping covers "no opinion
// expressed", not "asked for something we do not have".
const KIND_TO_PROFILE = { tree: 'broadleaf', shrub: 'shrub', grass: 'grass', conifer: 'conifer' };

// Unknown profiles are an error, not a default. Silently generating a broadleaf for a species
// that asked for something else is exactly the class of bug this module exists to prevent.
export function resolveProfile(spec) {
  const named = spec?.profile;
  const name = named ?? KIND_TO_PROFILE[spec?.kind] ?? 'broadleaf';
  const profile = PROFILES[name];
  if (!profile) {
    const error = new Error(`unknown vegetation profile '${name}'; expected one of ${PROFILE_NAMES.join(', ')}`);
    error.code = 'NG-E-VEG-SPECIES';
    throw error;
  }
  return {
    schema: PROFILE_SCHEMA,
    name: profile.profile,
    envelope: profile.envelope,
    crownRadiusPerHeight: profile.crownRadiusPerHeight,
    heightSuppressionFraction: profile.heightSuppressionFraction,
    branchOrder: profile.branchOrder,
    // A species may override the profile's value: a tall spruce wants a stiffer spire than a
    // compact fir, and a sweep of this number is how that decision gets evidence instead of taste.
    apicalUpBias: spec?.apicalUpBias ?? profile.apicalUpBias ?? 0,
  };
}

// Multipliers for a child attached at `fraction` along its parent, interpolated from the profile's
// base→tip pair. A profile that declares [1, 1] returns exactly 1, so consuming this is free for
// the species that do not need modulation.
export function branchOrderAt(profile, fraction) {
  const t = Math.min(1, Math.max(0, fraction));
  const pair = profile.branchOrder;
  return {
    length: pair.lengthAlongParent[0] + (pair.lengthAlongParent[1] - pair.lengthAlongParent[0]) * t,
    spread: pair.spreadAlongParent[0] + (pair.spreadAlongParent[1] - pair.spreadAlongParent[0]) * t,
  };
}

// Builds the radius function the growth code calls. Kept pure and separate from the growth loop so
// a test can assert the shape without building a tree.
//
// The scale comes from the profile and the plant's height, not from the authored crown radius: a
// declaration must not be able to bend the geometry. `crownRadiusMetres` is compared against the
// grown result afterwards (`crownMismatch`).
export function shapeCrownRadius(profile, height) {
  return Math.max(0, Math.max(0.05, height) * profile.crownRadiusPerHeight);
}

export function envelopeOf(profile, height) {
  const safeHeight = Math.max(0.05, height);
  const crown = shapeCrownRadius(profile, safeHeight);
  const shape = profile.envelope.shape;
  if (shape === 'cone') {
    const base = safeHeight * profile.envelope.crownBaseFraction;
    const span = Math.max(0.05, safeHeight - base);
    const taper = profile.envelope.taper;
    const floor = profile.envelope.apexFloorFraction;
    return (position) => {
      if (position[1] < base) return crown * floor;
      const t = Math.min(1, Math.max(0, (position[1] - base) / span));
      return crown * Math.max(floor, Math.pow(1 - t, taper));
    };
  }
  if (shape === 'cylinder') return () => crown;
  const centre = safeHeight * profile.envelope.centreFraction;
  const half = Math.max(0.1, safeHeight * profile.envelope.halfHeightFraction);
  return (position) => {
    const t = (position[1] - centre) / half;
    return crown * Math.sqrt(Math.max(0.04, 1 - Math.min(1, t * t)));
  };
}
