// World error contract.
//
// Every world failure carries a stable code so the editor, the AI tool surface and the
// native host can agree on what went wrong without parsing prose. The codes are the same
// strings the diagnostics table in the development plan names; adding one here is the
// only way to add one to the system.

export const WORLD_ERROR_CODES = Object.freeze({
  SCHEMA_INVALID: 'WORLD_SCHEMA_INVALID',
  BUDGET_EXCEEDED: 'WORLD_BUDGET_EXCEEDED',
  ASSET_MISSING: 'WORLD_ASSET_MISSING',
  CONSTRAINT_UNSATISFIED: 'WORLD_CONSTRAINT_UNSATISFIED',
  CHUNK_CORRUPT: 'WORLD_CHUNK_CORRUPT',
  TASK_STALE: 'WORLD_TASK_STALE',
  NATIVE_UNAVAILABLE: 'WORLD_NATIVE_UNAVAILABLE',
  REVISION_CONFLICT: 'REVISION_CONFLICT',
  PATH_REJECTED: 'WORLD_PATH_REJECTED',
  TASK_NOT_FOUND: 'WORLD_TASK_NOT_FOUND',
  GENERATOR_VERSION: 'WORLD_GENERATOR_VERSION',
});

export class WorldError extends Error {
  /**
   * @param {string} code one of WORLD_ERROR_CODES
   * @param {string} message human readable, safe to show in the editor
   * @param {object} [details] structured diagnostics: taskId, layerId, featureId,
   *   chunk, stage, inputVersion, cause, suggestion
   */
  constructor(code, message, details = {}) {
    super(message);
    this.name = 'WorldError';
    this.code = code;
    this.details = details;
  }

  toJSON() {
    return { code: this.code, message: this.message, details: this.details };
  }
}

export function isWorldError(value) {
  return value instanceof WorldError;
}

// A diagnostic is the user-facing projection of a WorldError or of a constraint failure
// produced by the compiler. Keeping the shape stable is what lets `world_query` return a
// list the editor can render as clickable markers.
export function diagnostic(code, message, extra = {}) {
  return {
    code,
    message,
    taskId: extra.taskId ?? null,
    layerId: extra.layerId ?? null,
    featureId: extra.featureId ?? null,
    chunk: extra.chunk ?? null,
    stage: extra.stage ?? null,
    inputVersion: extra.inputVersion ?? null,
    suggestion: extra.suggestion ?? null,
  };
}

export function fail(code, message, details) {
  throw new WorldError(code, message, details);
}
