// Binary record layouts of the groom package.
//
// They live in their own module so both cook paths (procedural `nativegpu.groom/1` and
// explicit `nativegpu.groom/2`) and the reader share one definition. A layout that only one
// path knew about is how a package ends up parseable by the writer and not by the runtime.
//
// Every layout is also the ABI of a `packRecords` call: the byte sizes here are what
// `static_assert` pins in `engine/native/groom/asset.hpp`, so a change has to be made in both
// places on purpose.

// Package section indices. 900 holds the scalp mesh, 1000 the guides, and each LOD level uses
// its own index. They are part of the format, not a local convention.
export const SCALP_SECTION = 900;
export const GUIDE_SECTION = 1000;

export const CURVE_HEADER_LAYOUT = [
  { name: 'firstPoint', type: 'u32', bytes: 4 },
  { name: 'pointCount', type: 'u32', bytes: 4 },
  { name: 'group', type: 'u32', bytes: 4 },
  { name: 'flags', type: 'u32', bytes: 4 },
];
export const BINDING_LAYOUT = [
  { name: 'triangle', type: 'u32', bytes: 4 },
  { name: 'bary0', type: 'f32', bytes: 4 }, { name: 'bary1', type: 'f32', bytes: 4 }, { name: 'bary2', type: 'f32', bytes: 4 },
  { name: 'rootU', type: 'f32', bytes: 4 }, { name: 'rootV', type: 'f32', bytes: 4 },
  { name: 'offsetMetres', type: 'f32', bytes: 4 }, { name: 'flags', type: 'u32', bytes: 4 },
];
export const CURVE_ATTRIBUTE_LAYOUT = [
  { name: 'arcLength', type: 'f32', bytes: 4 }, { name: 'rootToTip', type: 'f32', bytes: 4 },
  { name: 'group', type: 'u32', bytes: 4 }, { name: 'stableId', type: 'u32', bytes: 4 },
];
export const GUIDE_WEIGHT_LAYOUT = [
  { name: 'guide0', type: 'u32', bytes: 4 }, { name: 'guide1', type: 'u32', bytes: 4 }, { name: 'guide2', type: 'u32', bytes: 4 },
  { name: 'strand', type: 'u32', bytes: 4 },
  { name: 'weight0', type: 'f32', bytes: 4 }, { name: 'weight1', type: 'f32', bytes: 4 }, { name: 'weight2', type: 'f32', bytes: 4 },
  { name: 'reserved', type: 'u32', bytes: 4 },
];
export const CARD_LAYOUT = [
  { name: 'firstCurve', type: 'u32', bytes: 4 }, { name: 'curveCount', type: 'u32', bytes: 4 },
  { name: 'widthMetres', type: 'f32', bytes: 4 }, { name: 'coverage', type: 'f32', bytes: 4 },
  { name: 'layer', type: 'u32', bytes: 4 }, { name: 'material', type: 'u32', bytes: 4 },
  { name: 'flags', type: 'u32', bytes: 4 }, { name: 'reserved', type: 'u32', bytes: 4 },
];
