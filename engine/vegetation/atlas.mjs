// Leaf atlas metadata.
//
// A leaf atlas is a grid of separate leaves in one image. Three things have to be known
// about each cell before a card can be placed correctly, and none of them can be inferred
// from the image at runtime:
//
//   * the cell rectangle, in normalised UV;
//   * the pivot — where the petiole attaches inside the cell. A card that is centred on the
//     branch instead of pivoted on its petiole is the "floating leaf" defect;
//   * the source size in metres, so a leaf is as big as the species says it is.
//
// The V axis convention is stated once, here: cell.v0 is the *bottom* of the cell in UV
// space, and image row 0 is the top of the sheet, so row r maps to v0 = 1 - (r+1)/rows.
// The texture cook, the D3D upload and this module all use that single convention; flipping
// V again anywhere downstream mirrors every leaf.

export const ATLAS_SCHEMA = 'nativegpu.vegetation-atlas/1';
export const ATLAS_SCHEMA_VERSION = 2;
// The coordinate contract of the atlas. `legacy/v1` keeps the old default pivot at the cell's
// bottom edge for sheets that were authored before the canonical leaf slot existed; `canonical/v2`
// uses the 1/16 safe margin and per-cell content bounds. This is a migration, not a global default
// change: an old sheet must keep sampling where it always did or every existing tree shifts.
export const ATLAS_COORDINATES = Object.freeze({ legacy: 'legacy/v1', canonical: 'canonical/v2' });
const CANONICAL_ROOT_PIVOT = [0.5, 0.0625];

function fail(message) {
  const error = new Error(message);
  error.code = 'NG-E-VEG-ATLAS';
  throw error;
}

function pair(value, label) {
  if (!Array.isArray(value) || value.length !== 2
    || !Number.isFinite(value[0]) || !Number.isFinite(value[1])
    || value[0] < 0 || value[0] > 1 || value[1] < 0 || value[1] > 1) {
    fail(`${label} must be a [u,v] pair inside the cell`);
  }
  return value;
}

// `pivot` is in cell space: (0,0) is the bottom-left corner of the cell, (0.5,0) is the base
// centre where a petiole meets the edge of the art, (1,1) the top-right. A legacy definition keeps
// the old bottom-edge default; a version-2 definition defaults to the canonical 1/16 root slot.
export function buildAtlas(definition) {
  if (!definition || typeof definition !== 'object') fail('atlas definition must be an object');
  const columns = Math.round(Number(definition.columns));
  const rows = Math.round(Number(definition.rows));
  if (!Number.isInteger(columns) || columns < 1 || columns > 16) fail('atlas.columns must be 1..16');
  if (!Number.isInteger(rows) || rows < 1 || rows > 16) fail('atlas.rows must be 1..16');
  const width = Math.round(Number(definition.width ?? 0));
  const height = Math.round(Number(definition.height ?? 0));
  if (width < 0 || height < 0) fail('atlas width/height must be non-negative');
  const version = Number(definition.schemaVersion ?? 1);
  if (version !== 1 && version !== ATLAS_SCHEMA_VERSION) fail(`unsupported atlas schemaVersion ${definition.schemaVersion}`);
  const canonical = version >= ATLAS_SCHEMA_VERSION;
  const pivot = definition.pivot ?? (canonical ? CANONICAL_ROOT_PIVOT : [0.5, 0]);
  pair(pivot, 'atlas.pivot');
  // Padding is in normalised cell units: the margin of a cell that carries no leaf pixels.
  // It is recorded so a mip level cannot bleed a neighbouring leaf into a card.
  const padding = Number(definition.padding ?? 0);
  if (!Number.isFinite(padding) || padding < 0 || padding > 0.4) fail('atlas.padding must be 0..0.4');
  const leafWidth = Number(definition.leafWidthMetres ?? 0);
  const leafHeight = Number(definition.leafHeightMetres ?? 0);
  const overrides = new Map();
  if (definition.cells !== undefined) {
    if (!Array.isArray(definition.cells)) fail('atlas.cells must be an array');
    for (const [position, entry] of definition.cells.entries()) {
      if (!entry || typeof entry !== 'object') fail(`atlas.cells[${position}] must be an object`);
      const index = Math.round(Number(entry.index ?? position));
      if (!Number.isInteger(index) || index < 0 || index >= columns * rows) fail(`atlas.cells[${position}].index is outside the grid`);
      if (overrides.has(index)) fail(`atlas.cells declares index ${index} twice`);
      if (entry.pivot !== undefined) pair(entry.pivot, `atlas.cells[${position}].pivot`);
      if (entry.contentBounds !== undefined) {
        const bounds = entry.contentBounds;
        const keysOk = bounds && ['u0', 'v0', 'u1', 'v1'].every((key) => Number.isFinite(bounds[key]));
        if (!keysOk || bounds.u0 < 0 || bounds.v0 < 0 || bounds.u1 > 1 || bounds.v1 > 1 || bounds.u0 >= bounds.u1 || bounds.v0 >= bounds.v1) {
          fail(`atlas.cells[${position}].contentBounds must be a rect inside [0,1]`);
        }
      }
      overrides.set(index, entry);
    }
  }

  const cells = [];
  for (let row = 0; row < rows; ++row) {
    for (let column = 0; column < columns; ++column) {
      const index = row * columns + column;
      const override = overrides.get(index) ?? {};
      const cellPivot = override.pivot ?? pivot;
      const u0 = column / columns;
      const u1 = (column + 1) / columns;
      // Row 0 is the top of the sheet; UV v grows upward.
      const v1 = 1 - row / rows;
      const v0 = 1 - (row + 1) / rows;
      const du = u1 - u0;
      const dv = v1 - v0;
      // Content bounds are in cell space (v up). `null` means the cell's whole rect, which is
      // what a legacy sheet means. A canonical cell carries the margin-normalised content rect.
      const content = override.contentBounds ?? null;
      const contentU0 = content ? u0 + du * content.u0 : null;
      const contentV0 = content ? v0 + dv * content.v0 : null;
      const contentU1 = content ? u0 + du * content.u1 : null;
      const contentV1 = content ? v0 + dv * content.v1 : null;
      cells.push({
        index,
        column, row,
        u0, v0, u1, v1,
        du, dv,
        // Absolute sheet UV of the pivot.
        pivotU: u0 + du * cellPivot[0],
        pivotV: v0 + dv * cellPivot[1],
        // Absolute content rect. Null for a legacy/full-cell sheet; the mesher then maps the
        // blade across the whole cell, exactly as it always did.
        contentU0, contentV0, contentU1, contentV1,
        // Inner rect after padding, used for mip-safe sampling of the cell.
        contentPaddingU0: u0 + du * padding,
        contentPaddingV0: v0 + dv * padding,
        contentPaddingU1: u1 - du * padding,
        contentPaddingV1: v1 - dv * padding,
        leafProfile: override.leafProfile ?? null,
        sourceId: override.sourceId ?? null,
        damageVariant: override.damageVariant ?? null,
        // Pixel rect, for reports and for the diagnostic overlay. A 1254 px sheet split into
        // 3 columns is not an integer number of pixels, so these are fractional by design and
        // the renderer never uses them for addressing.
        pixelX: (width * column) / columns,
        pixelY: (height * row) / rows,
        pixelWidth: width / columns,
        pixelHeight: height / rows,
        widthMetres: leafWidth,
        heightMetres: leafHeight,
      });
    }
  }
  return {
    schema: ATLAS_SCHEMA,
    schemaVersion: version,
    coordinatesVersion: canonical ? ATLAS_COORDINATES.canonical : ATLAS_COORDINATES.legacy,
    columns, rows, width, height,
    pivot: [pivot[0], pivot[1]],
    padding,
    leafWidthMetres: leafWidth,
    leafHeightMetres: leafHeight,
    cells,
  };
}

// Stable cell choice. The same leaf keeps the same cell when the tree is rebuilt, and a
// coarser LOD keeps a subset instead of re-rolling every leaf onto a different image.
export function cellForIndex(atlas, index) {
  if (!atlas || !atlas.cells.length) return null;
  return atlas.cells[((index % atlas.cells.length) + atlas.cells.length) % atlas.cells.length];
}

// Bounding box of the union of all cells, in UV. Used by the debug view to draw cell
// boundaries without knowing the grid.
export function atlasRect(atlas) {
  return { u0: 0, v0: 0, u1: 1, v1: 1, columns: atlas.columns, rows: atlas.rows };
}
