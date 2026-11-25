import { gameplayCatalog, gameplayDependencies, validateComponents, findGameplayAssets } from '../engine/scene/gameplay.mjs';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  SceneError, validateScene, loadScene, queryScene, previewTransaction,
  commitScene, commitSceneUpdate, undoSceneFile, buildScene, sceneHash,
} from '../engine/scene/index.mjs';
import { applyMaterialPatch, previewMaterialPatch } from '../tools/material.mjs';

const DEFAULT_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const MAX_SCENE_BYTES = 16 * 1024 * 1024;
const MAX_SMOKE_FRAMES = 3600;
const MAPPING_SCHEMA={type:'object',additionalProperties:false,properties:{offset:{type:'array',minItems:2,maxItems:2,items:{type:'number',minimum:-1000000,maximum:1000000}},scale:{type:'array',minItems:2,maxItems:2,items:{type:'number',minimum:-1000000,maximum:1000000}},rotation:{type:'number',minimum:-1000000,maximum:1000000},wrapS:{enum:['repeat','clamp','mirror']},wrapT:{enum:['repeat','clamp','mirror']},magFilter:{enum:['nearest','linear']},minFilter:{enum:['nearest','linear','nearest-mipmap-nearest','linear-mipmap-nearest','nearest-mipmap-linear','linear-mipmap-linear']}}};

export class ToolError extends Error {
  constructor(code, message, details = []) {
    super(message);
    this.name = 'ToolError';
    this.code = code;
    this.details = details;
  }
}

const SCENE_PATH = {
  type: 'string',
  description: 'Scene path relative to the project root. Only assets/ and .screen/ are writable through this tool.',
};
const TRANSACTION = {
  type: 'object',
  description: 'Atomic scene transaction: {projectId, expectedRevision, commands[]}',
  additionalProperties: false,
  required: ['projectId', 'expectedRevision', 'commands'],
  properties: {
    projectId: { type: 'string' },
    expectedRevision: { type: 'integer', minimum: 0 },
    commands: { type: 'array', minItems: 1, maxItems: 1000, items: { type: 'object' } },
  },
};

// ---------------------------------------------------------------- hair & garment
// Tool contracts from the development plan, section 7. Every mutating tool shares
// projectId / assetId / expectedRevision / requestId / dryRun; the shared shape is built
// once so a new tool cannot forget one of them.
const ASSET_COMMON = {
  projectId: { type: 'string', description: 'Project id of the open project.' },
  expectedRevision: { type: 'string', description: 'Revision the caller last read. A mismatch is refused.' },
  requestId: { type: 'string', description: 'Idempotency key: a retry returns the first result instead of re-applying.' },
  dryRun: { type: 'boolean', default: false, description: 'Return the diff without writing anything.' },
};

const TOOLS = Object.freeze([
  {
    name: 'gameplay_catalog', description: 'Read registered component schemas and behavior property/state contracts.',
    inputSchema: {type:'object',additionalProperties:false,properties:{}}
  },
  {
    name: 'gameplay_validate', description: 'Validate component/behavior authoring and resolve hashed script dependencies without executing Lua.',
    inputSchema: {type:'object',additionalProperties:false,required:['scenePath'],properties:{scenePath:SCENE_PATH}}
  },
  {
    name: 'gameplay_inspect', description: 'Read the native gameplay state and diagnostics from a bounded report under reports/.',
    inputSchema: {type:'object',additionalProperties:false,properties:{reportPath:{type:'string',default:'reports/gameplay/live.json'}}}
  },
  {
    name: 'scene_validate',
    description: 'Validate a scene JSON file without modifying it. Returns structured errors instead of writing.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath'],
      properties: { scenePath: SCENE_PATH },
    },
  },
  {
    name: 'scene_query',
    description: 'Read entities from a validated scene. Never modifies the project.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath'],
      properties: {
        scenePath: SCENE_PATH,
        kind: { enum: ['ground', 'wall', 'prop', 'player', 'key', 'door', 'goal', 'light'] },
        name: { type: 'string' },
        entityId: { type: 'string' },
      },
    },
  },
  {
    name: 'scene_preview',
    description: 'Apply a transaction to an in-memory copy and return the diff. Does not write the scene.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath', 'transaction'],
      properties: {
        scenePath: SCENE_PATH,
        transaction: TRANSACTION,
        includeScene: { type: 'boolean', default: false },
      },
    },
  },
  {
    name: 'scene_apply',
    description: 'Atomically commit a transaction. The running editor hot-reloads the new revision.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath', 'transaction'],
      properties: { scenePath: SCENE_PATH, transaction: TRANSACTION },
    },
  },
  {
    name: 'material_patch',
    description: 'Preview or apply a typed change to one scene material (wetness, wear, roughness, metalness, uvScale, texture references). Validates and reports affected entities.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['scenePath', 'material', 'changes'],
      properties: {
        scenePath: { ...SCENE_PATH, default: '.screen/town.scene.json' },
        material: { type: 'string' },
        changes: {
          type: 'object', additionalProperties: false,
          properties: {
            albedo: { type: 'string' },
            normal: { type: 'string' },
            orm: { type: 'string' },
            emissive: { type: ['string','null'] },
            emissiveFactor: { type: 'array', minItems: 3, maxItems: 3, items: {type:'number',minimum:0,maximum:1} },
            emissiveStrength: { type:'number',minimum:0,maximum:1000000 },
            occlusion: {type:['string','null']},
            coat:{type:['string','null']},coatRoughness:{type:['string','null']},coatNormal:{type:['string','null']},
            occlusionStrength: {type:'number',minimum:0,maximum:1},
            unlit: {type:'boolean'},
            alphaMode: {type:'string',enum:['OPAQUE','MASK','BLEND']},
            alphaCutoff: {type:'number',minimum:0,maximum:1},
            opacity: {type:'number',minimum:0,maximum:1},
            textureMappings:{type:['object','null'],additionalProperties:false,properties:Object.fromEntries(['albedo','normal','orm','emissive','occlusion','coat','coatRoughness','coatNormal'].map(key=>[key,MAPPING_SCHEMA]))},
            layered:{type:['object','null']},
            uvScale: { type: 'number', minimum: 0.05, maximum: 1000 },
            roughness: { type: 'number', minimum: 0, maximum: 1 },
            metalness: { type: 'number', minimum: 0, maximum: 1 },
            wetness: { type: 'number', minimum: 0, maximum: 1 },
            wear: { type: 'number', minimum: 0, maximum: 1 },
            clearcoat: { type: 'number', minimum: 0, maximum: 1 },
            clearcoatRoughness: { type: 'number', minimum: 0, maximum: 1 },
            ior: { anyOf: [{const:0},{type:'number',minimum:1,maximum:3.4028234663852886e38}] },
          },
        },
        expectedRevision: { type: 'integer', minimum: 0 },
        apply: { type: 'boolean', default: false },
      },
    },
  },
  {
    name: 'scene_undo',
    description: 'Undo the last committed transaction. Revision still increases to reject stale requests.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['scenePath', 'projectId', 'expectedRevision'],
      properties: {
        scenePath: SCENE_PATH,
        projectId: { type: 'string' },
        expectedRevision: { type: 'integer', minimum: 0 },
      },
    },
  },
  {
    name: 'scene_build',
    description: 'Build a deterministic content-addressed scene package under outputs/.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['scenePath', 'outputDirectory'],
      properties: {
        scenePath: SCENE_PATH,
        outputDirectory: { type: 'string', description: 'Relative directory under outputs/.' },
        target: { const: 'windows-x64', default: 'windows-x64' },
      },
    },
  },
  {
    name: 'runtime_smoke',
    description: 'Run the native engine for a bounded smoke test and return its JSON report. Requires build/NativeGPU.exe.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        scenePath: { ...SCENE_PATH, default: '.screen/courtyard.scene.json' },
        frames: { type: 'integer', minimum: 240, maximum: MAX_SMOKE_FRAMES, default: 360 },
      },
    },
  },
  {
    name: 'hair_capabilities',
    description: 'Discover the registered hair/garment plugins, their versions, schemas, supported poses, channels and explicit non-capabilities before attempting any work.',
    inputSchema: { type: 'object', additionalProperties: false, properties: { projectId: { type: 'string' } } },
  },
  {
    name: 'hair_reference_register',
    description: 'Register a reference image for hair analysis: hashes the file, records the view, crop, subject and confidence. Does not perform visual recognition.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['path', 'view', 'authorized'],
      properties: {
        projectId: { type: 'string' },
        path: { type: 'string', description: 'Image path under the project root.' },
        id: { type: 'string' },
        view: { enum: ['front', 'back', 'left', 'right', 'three_quarter_left', 'three_quarter_right', 'top', 'unknown'] },
        crop: { type: 'object', additionalProperties: false, properties: { x: {type:'number'}, y: {type:'number'}, width: {type:'number'}, height: {type:'number'} } },
        subject: { type: 'string' },
        hairlineVisible: { type: 'boolean' },
        confidence: { type: 'number', minimum: 0, maximum: 1 },
        authorized: { type: 'boolean', description: 'Must be true: the engine never uploads a project on its own.' },
        license: { type: ['string', 'null'] },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'hair_create',
    description: 'Create an editable hairstyle from a reference analysis or an explicit region spec, binding guides to the real character scalp and cooking a package. Returns the source revision, binding diagnostics and inferred regions.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['id'],
      properties: {
        projectId: { type: 'string' },
        id: { type: 'string', description: 'Hairstyle id, lowercase identifier.' },
        characterAsset: { type: 'string', description: 'Character asset path under the project root.' },
        headBone: { type: 'string', default: 'head' },
        seed: { type: 'integer', minimum: 0 },
        references: { type: 'array', maxItems: 8, items: { type: 'object' } },
        reconciliation: { type: 'object', description: 'Result of hair_analysis_reconcile: observed fields with provenance.' },
        regions: { type: 'array', maxItems: 16, items: { type: 'object' } },
        spec: { type: 'object', description: 'Full nativegpu.hairstyle/1 document, for callers that already have one.' },
        expectedRevision: { type: ['string', 'null'] },
        requestId: { type: 'string' },
        dryRun: { type: 'boolean', default: false },
      },
    },
  },
  {
    name: 'hair_patch',
    description: 'Apply a structured patch to specific regions or guides. Refuses locked fields, requires expectedRevision, supports dryRun and reports which regions will be regenerated.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['id', 'operations', 'expectedRevision'],
      properties: {
        projectId: { type: 'string' },
        id: { type: 'string' },
        expectedRevision: { type: 'string' },
        requestId: { type: 'string' },
        dryRun: { type: 'boolean', default: false },
        note: { type: 'string' },
        operations: {
          type: 'array', minItems: 1, maxItems: 64,
          items: {
            type: 'object',
            properties: {
              kind: { enum: ['region', 'guide', 'lock', 'unlock'] },
              region: { type: 'string' },
              guide: { type: 'string', description: 'Stable guide id, "<region>#<hex>".' },
              fields: { type: 'array', items: { type: 'string' } },
              changes: { type: 'object' },
            },
            required: ['kind'],
          },
        },
      },
    },
  },
  {
    name: 'hair_capture',
    description: 'Plan a reproducible capture set: fixed views, lighting setups and debug channels, with per-entry camera/pose/revision/seed metadata that a later selection is validated against.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['id'],
      properties: {
        projectId: { type: 'string' },
        id: { type: 'string' },
        views: { type: 'array', items: { enum: ['front', 'back', 'left', 'right', 'three_quarter_left', 'three_quarter_right', 'hairline_closeup', 'top'] } },
        channels: { type: 'array', items: { enum: ['beauty', 'silhouette', 'region_id', 'guide', 'root_binding', 'tangent', 'coverage', 'collision', 'lod', 'depth', 'normal'] } },
        lighting: { type: 'array', items: { enum: ['neutral', 'backlight', 'silhouette'] } },
        resolution: { type: 'object', additionalProperties: false, properties: { width: {type:'integer'}, height: {type:'integer'} } },
        pose: { type: 'object' },
        frame: { type: 'integer', minimum: 1 },
        exposure: { type: 'number' },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'hair_validate',
    description: 'Validate binding on the real scalp under a pose set, guide penetration, LOD identity, budget and transparency demand. Reports what it did not measure instead of implying approval.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['id'],
      properties: {
        projectId: { type: 'string' },
        id: { type: 'string' },
        averageCoverage: { type: 'number', minimum: 0.01, maximum: 1 },
        layersPerTile: { type: 'integer', minimum: 1, maximum: 64 },
        resolution: { type: 'object', additionalProperties: false, properties: { width: {type:'integer'}, height: {type:'integer'} } },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'hair_analysis_reconcile',
    description: 'Merge AI analyses of one or more reference views into hairstyle parameters. Visible observations win; disagreeing views produce a conflict instead of an average; inferred fields are flagged and require a back view.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['references', 'analyses'],
      properties: {
        projectId: { type: 'string' },
        references: { type: 'array', minItems: 1, maxItems: 8, items: { type: 'object' } },
        analyses: { type: 'array', minItems: 1, maxItems: 8, items: { type: 'object' } },
        style: { type: 'string' },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'hair_export',
    description: 'Cook and publish a hairstyle package under assets/hair/cooked, optionally also emitting the lossy nativegpu.groom/1 compatibility package with its documented losses.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['id'],
      properties: {
        projectId: { type: 'string' },
        id: { type: 'string' },
        includeCompatibleGroom: { type: 'boolean', default: false },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'garment_inspect',
    description: 'Diagnose an existing garment mesh against the character: degenerate faces, non-manifold edges, winding, initial penetration, thin layers and body/garment fit. Never shrinks the garment.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['characterAsset'],
      properties: {
        projectId: { type: 'string' },
        garmentId: { type: 'string' },
        garmentPath: { type: 'string', description: 'JSON mesh file under the project root.' },
        garment: { type: 'object', description: 'Inline mesh: positions, indices, optional uvs/vertexIds/skin.' },
        characterAsset: { type: 'string' },
        thresholds: { type: 'object' },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'garment_bind',
    description: 'Transfer skin weights from the body to the garment with region and side constraints, smooth with locks, sparsify to the runtime influence limit, and build the physics contract.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['characterAsset'],
      properties: {
        projectId: { type: 'string' },
        garmentId: { type: 'string' },
        garmentPath: { type: 'string' },
        garment: { type: 'object' },
        characterAsset: { type: 'string' },
        maximumInfluences: { type: 'integer', minimum: 1, maximum: 8 },
        physics: { type: 'object' },
        material: { type: 'object' },
        smooth: { type: 'object' },
        regionPhysics: { type: 'array', items: { type: 'object' } },
        allowedRegions: { type: 'array', items: { type: 'string' }, description: 'Body regions this garment may bind to; a cape uses torso/hips/shoulders, not arms.' },
        influenceRegions: {
          type: 'array', items: { type: 'string' },
          description: 'Body regions allowed to supply skin weights. A skirt declares hips/torso: its region is compatible with the thighs by design, so without this the hem follows the legs.',
        },
        locks: {
          type: 'object',
          description: 'Locked weights, keyed by vertex index: { "12": [{ "bone": "mixamorig:Spine2", "weight": 1 }] }. Honoured through smoothing and reported as a conflict when the influence limit cannot hold them.',
        },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'garment_physics_patch',
    description: 'Change physics channels per region or per vertex: dynamic blend, maximum displacement, collision thickness, anchor stiffness.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['garmentId', 'changes'],
      properties: {
        projectId: { type: 'string' },
        garmentId: { type: 'string' },
        changes: {
          type: 'array', minItems: 1,
          items: {
            type: 'object',
            properties: {
              regions: { type: 'array', items: { type: 'string' } },
              vertexIds: { type: 'array', items: { type: 'integer' } },
              values: { type: 'object', description: '{blend, maxDisplacement, thickness, anchors, mass}' },
            },
          },
        },
        requestId: { type: 'string' },
        dryRun: { type: 'boolean', default: false },
      },
    },
  },
  {
    name: 'garment_validate',
    description: 'Simulate a motion set (idle, walk, run, turn, arms up, crouch, jump, teleport) and report penetration P95/P99/max against proxies and the body mesh, stretch, and divergences. States unsupported features explicitly.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['garmentId'],
      properties: {
        projectId: { type: 'string' },
        garmentId: { type: 'string' },
        ticks: { type: 'integer', minimum: 4, maximum: 240 },
        poses: { type: 'object' },
        penetrationToleranceMetres: { type: 'number', minimum: 0 },
        stretchTolerance: { type: 'number', minimum: 1 },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'weights_export',
    description: 'Export weight and physics views with fixed legends, id buffers and numeric data from one snapshot: skin weight, dominant bone, influence count, blend, displacement, anchors, thickness, penetration, confidence.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['garmentId', 'channels'],
      properties: {
        projectId: { type: 'string' },
        garmentId: { type: 'string' },
        channels: { type: 'array', items: { enum: ['skin_weight', 'dominant_bone', 'influence_count', 'blend', 'max_displacement', 'anchors', 'collision_thickness', 'penetration', 'confidence'] } },
        views: { type: 'array', items: { enum: ['front', 'back', 'left', 'right'] } },
        bone: { type: 'integer', description: 'Bone index, required by skin_weight.' },
        resolution: { type: 'integer', minimum: 64, maximum: 2048 },
        wireframe: { type: 'boolean', default: true },
        requestId: { type: 'string' },
      },
    },
  },
  {
    name: 'weights_patch',
    description: 'Set/add/scale one bone influence on stable vertex ids. Requires expectedTopology; dryRun returns the same diff without mutating the binding.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      required: ['garmentId', 'operations'],
      properties: {
        projectId: { type: 'string' },
        garmentId: { type: 'string' },
        expectedTopology: { type: 'string' },
        dryRun: { type: 'boolean', default: false },
        requestId: { type: 'string' },
        operations: {
          type: 'array', minItems: 1,
          items: {
            type: 'object',
            required: ['kind', 'bone'],
            properties: {
              kind: { enum: ['set', 'add', 'scale'] },
              vertexId: { type: 'integer' },
              vertexIds: { type: 'array', items: { type: 'integer' } },
              bone: { type: ['integer', 'string'] },
              value: { type: 'number' },
            },
          },
        },
      },
    },
  },
  {
    name: 'engine_probe',
    description: 'Run the native S1 capability probe and return the material table, scene lighting, DXR and Mesh Shader evidence.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        scenePath: { ...SCENE_PATH, default: '.screen/courtyard.scene.json' },
        frames: { type: 'integer', minimum: 240, maximum: MAX_SMOKE_FRAMES, default: 240 },
      },
    },
  },
  // ---------------------------------------------------------------- world authoring
  //
  // These are the same controlled calls the editor panel makes. A model is never allowed
  // to write a chunk, a layer or a scene directly: it produces a preview task, reads the
  // diagnostics, and applies with an expected revision like any other caller.
  {
    name: 'world_capabilities',
    description: 'Read the supported world generators, budget profiles, limits and the honest capability labels (interiors, river physics).',
    inputSchema: { type: 'object', additionalProperties: false, properties: {} },
  },
  {
    name: 'world_list',
    description: 'List the worlds authored in this project.',
    inputSchema: { type: 'object', additionalProperties: false, properties: {} },
  },
  {
    name: 'world_create',
    description: 'Create a world recipe with the default ancient-valley layers inside the project.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['worldId'],
      properties: {
        worldId: { type: 'string', pattern: '^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$' },
        extentMetres: { type: 'number', minimum: 16, maximum: 8192, default: 256 },
        preset: { type: 'string', enum: ['flat', 'mountain-valley', 'rolling-hills', 'ridge-basin', 'river-plain'], default: 'mountain-valley' },
        seed: { type: 'string', pattern: '^[0-9]{1,20}$', default: '947210' },
        budgetProfile: { type: 'string', enum: ['debug', 'desktop-balanced', 'desktop-quality', 'stress'], default: 'desktop-balanced' },
      },
    },
  },
  {
    name: 'world_open',
    description: 'Open a world and load its recipe and layers into the editing draft.',
    inputSchema: { type: 'object', additionalProperties: false, required: ['worldId'], properties: { worldId: { type: 'string' } } },
  },
  {
    name: 'world_library_inspect',
    description: 'Report component counts, stand-in vs imported status and missing assets for the open world.',
    inputSchema: { type: 'object', additionalProperties: false, properties: {} },
  },
  {
    name: 'world_preview',
    description: 'Apply brush strokes / path edits to the draft and cook it in the background. Returns a taskId; nothing is committed.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        strokes: {
          type: 'array', maxItems: 4096,
          items: {
            type: 'object', additionalProperties: false, required: ['layerId', 'points'],
            properties: {
              layerId: { type: 'string' },
              points: { type: 'array', minItems: 1, maxItems: 4096, items: { type: 'array', minItems: 2, maxItems: 2, items: { type: 'number' } } },
              radiusMetres: { type: 'number', minimum: 0.1, maximum: 8192, default: 8 },
              strength: { type: 'number', minimum: 0, maximum: 1, default: 1 },
              falloffMetres: { type: 'number', minimum: 0, maximum: 500 },
              erase: { type: 'boolean', default: false },
              shape: { type: 'string', enum: ['circle', 'rectangle', 'polygon', 'path'], default: 'circle' },
              widthMetres: { type: 'number', minimum: 0.5, maximum: 200 },
              mode: { type: 'string', enum: ['raise', 'lower', 'smooth', 'flatten', 'water', 'paint'] },
            },
          },
        },
        paths: {
          type: 'array', maxItems: 512,
          items: {
            type: 'object', additionalProperties: false, required: ['layerId', 'pathId'],
            properties: {
              layerId: { type: 'string' }, pathId: { type: 'string' },
              points: { type: 'array', maxItems: 4096, items: { type: 'array', minItems: 2, maxItems: 2, items: { type: 'number' } } },
              widthMetres: { type: 'number', minimum: 0.5, maximum: 200 },
              bedDepthMetres: { type: 'number', minimum: 0.05, maximum: 40 },
              remove: { type: 'boolean', default: false },
            },
          },
        },
        layers: {
          type: 'array', maxItems: 256,
          items: {
            type: 'object', additionalProperties: false, required: ['id'],
            properties: { id: { type: 'string' }, visible: { type: 'boolean' }, locked: { type: 'boolean' } },
          },
        },
        label: { type: 'string' },
      },
    },
  },
  {
    name: 'world_job_status',
    description: 'Read the state, progress, diagnostics and result of a world task.',
    inputSchema: { type: 'object', additionalProperties: false, required: ['taskId'], properties: { taskId: { type: 'string' } } },
  },
  {
    name: 'world_cancel',
    description: 'Cancel a world task that has not been committed.',
    inputSchema: { type: 'object', additionalProperties: false, required: ['taskId'], properties: { taskId: { type: 'string' } } },
  },
  {
    name: 'world_apply',
    description: 'Publish a ready task and switch the scene world reference. Requires the revision the preview was based on.',
    inputSchema: {
      type: 'object', additionalProperties: false, required: ['taskId', 'expectedRevision'],
      properties: {
        taskId: { type: 'string' },
        expectedRevision: { type: 'integer', minimum: 0 },
        sceneRevision: { type: 'integer', minimum: 0 },
        scenePath: SCENE_PATH,
      },
    },
  },
  {
    name: 'world_query',
    description: 'Report chunk coverage, layer state, locks and the last cook for the open world.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        region: {
          type: 'object', additionalProperties: false, required: ['minX', 'minZ', 'maxX', 'maxZ'],
          properties: { minX: { type: 'number' }, minZ: { type: 'number' }, maxX: { type: 'number' }, maxZ: { type: 'number' } },
        },
      },
    },
  },
  {
    name: 'world_regenerate',
    description: 'Regenerate the affected region and report the dependency scope that was actually rebuilt.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        region: {
          type: 'object', additionalProperties: false, required: ['minX', 'minZ', 'maxX', 'maxZ'],
          properties: { minX: { type: 'number' }, minZ: { type: 'number' }, maxX: { type: 'number' }, maxZ: { type: 'number' } },
        },
      },
    },
  },
  {
    name: 'world_lock',
    description: 'Lock or unlock a region or a set of stable object ids so regeneration cannot change them.',
    inputSchema: {
      type: 'object', additionalProperties: false,
      properties: {
        region: {
          type: 'object', additionalProperties: false, required: ['minX', 'minZ', 'maxX', 'maxZ'],
          properties: { minX: { type: 'number' }, minZ: { type: 'number' }, maxX: { type: 'number' }, maxZ: { type: 'number' } },
        },
        objectIds: { type: 'array', maxItems: 4096, items: { type: 'string' } },
        unlock: { type: 'boolean', default: false },
      },
    },
  },
  {
    name: 'world_undo',
    description: 'Restore the previous world draft. The cooked manifest it points at stays immutable.',
    inputSchema: { type: 'object', additionalProperties: false, properties: {} },
  },
  {
    name: 'world_redo',
    description: 'Reapply a world draft that was undone.',
    inputSchema: { type: 'object', additionalProperties: false, properties: {} },
  },
  {
    name: 'world_release',
    description: 'Report the release closure (chunk count and bytes) of the last committed cook.',
    inputSchema: { type: 'object', additionalProperties: false, properties: {} },
  },
]);

export function listTools() {
  return TOOLS.map(tool => structuredClone(tool));
}

function asObject(value, label) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new ToolError('INVALID_ARGUMENTS', `${label} must be an object`);
  }
  return value;
}

function asString(value, label) {
  if (typeof value !== 'string' || value.trim() === '' || value.includes('\0')) {
    throw new ToolError('INVALID_ARGUMENTS', `${label} must be a non-empty string`);
  }
  return value;
}

async function realRoot(root) {
  try {
    return await fs.realpath(root);
  } catch (error) {
    if (error.code === 'ENOENT') return path.resolve(root);
    throw error;
  }
}

function inside(parent, child) {
  const relative = path.relative(parent, child);
  return relative !== '' && !relative.startsWith(`..${path.sep}`) && relative !== '..'
    && !path.isAbsolute(relative);
}

async function resolveProjectPath(root, input, { roots, label, mustExist = true }) {
  const rootPath = path.resolve(root);
  const absolute = path.resolve(rootPath, asString(input, label));
  const rootReal = await realRoot(rootPath);
  const relative = path.relative(rootReal, absolute);
  if (relative === '' || relative.startsWith(`..${path.sep}`) || relative === '..'
      || path.isAbsolute(relative)) {
    throw new ToolError('INVALID_PATH', `${label} escapes the project root`);
  }
  const top = relative.split(path.sep)[0].toLowerCase();
  if (!roots.includes(top)) {
    throw new ToolError('INVALID_PATH', `${label} must be under ${roots.join('/')}`);
  }
  if (!mustExist) {
    // Check the nearest existing ancestor so a symlinked parent cannot escape.
    let ancestor = absolute;
    while (true) {
      try {
        const real = await fs.realpath(ancestor);
        if (real !== rootReal && !inside(rootReal, real)) {
          throw new ToolError('INVALID_PATH', `${label} resolves outside the project root`);
        }
        break;
      } catch (error) {
        if (error.code !== 'ENOENT') throw error;
        const parent = path.dirname(ancestor);
        if (parent === ancestor) break;
        ancestor = parent;
      }
    }
    return absolute;
  }
  let real;
  try {
    real = await fs.realpath(absolute);
  } catch (error) {
    if (error.code === 'ENOENT') throw new ToolError('FILE_NOT_FOUND', `${label} does not exist`);
    throw error;
  }
  if (real !== rootReal && !inside(rootReal, real)) {
    throw new ToolError('INVALID_PATH', `${label} resolves outside the project root`);
  }
  return real;
}

async function readSceneJSON(file) {
  const stat = await fs.stat(file);
  if (stat.size > MAX_SCENE_BYTES) throw new ToolError('FILE_TOO_LARGE', `Scene exceeds ${MAX_SCENE_BYTES} bytes`);
  let value;
  try {
    value = JSON.parse(await fs.readFile(file, 'utf8'));
  } catch (error) {
    if (error instanceof SyntaxError) throw new ToolError('INVALID_JSON', 'Scene is not valid JSON');
    throw error;
  }
  return value;
}

function scenePath(root, input) {
  return resolveProjectPath(root, input, { roots: ['assets', '.screen'], label: 'scenePath' });
}

// Tool name → hair service action. Kept as an explicit table so a tool without an
// implementation fails loudly instead of falling through to "unknown tool".
const HAIR_TOOL_ACTIONS = {
  hair_capabilities: 'hair.capabilities',
  hair_reference_register: 'hair.reference.register',
  hair_analysis_reconcile: 'hair.analysis.reconcile',
  hair_create: 'hair.create',
  hair_patch: 'hair.patch',
  hair_capture: 'hair.capture',
  hair_validate: 'hair.validate',
  hair_export: 'hair.export',
  garment_inspect: 'garment.inspect',
  garment_bind: 'garment.bind',
  garment_physics_patch: 'garment.physics',
  garment_validate: 'garment.validate',
  weights_export: 'weights.export',
  weights_patch: 'weights.patch',
};

export function createToolService({ root = process.env.NATIVEGPU_PROJECT_ROOT || DEFAULT_ROOT, executable } = {}) {
  const projectRoot = path.resolve(root);
  let hairServicePromise = null;
  const hairService = async () => {
    hairServicePromise = hairServicePromise ?? (async () => {
      const { createHairService } = await import('../editor/hair-service.mjs');
      return createHairService({ root: projectRoot });
    })();
    return hairServicePromise;
  };
  // One world service per tool-service instance, so a model's preview and its apply refer
  // to the same draft, the same epoch and the same revision. A per-call instance would let
  // a model preview in one draft and apply in another.
  let worldServicePromise = null;
  const worldService = async () => {
    worldServicePromise = worldServicePromise ?? (async () => {
      const { createWorldService } = await import('../editor/world-service.mjs');
      const { commitScene, findGameplayAssets } = await import('../engine/scene/index.mjs');
      return createWorldService({
        root: projectRoot,
        // The scene commit goes through the same transaction boundary the editor uses:
        // a world switch is a scene edit, not a side channel into the file.
        commitScene: async (scenePath, worldRef, expectedRevision) => {
          const file = path.resolve(projectRoot, scenePath);
          const scene = await loadScene(file, { assetsDirectory: findGameplayAssets(file) });
          const target = Number.isSafeInteger(expectedRevision) ? expectedRevision : scene.revision;
          const next = await commitSceneUpdate(
            file,
            { projectId: scene.projectId, expectedRevision: target },
            // The update callback must return the mutated scene: the transaction stores the
            // return value, not the argument it passed in.
            draft => { draft.world = worldRef; return draft; },
          );
          return { revision: next.revision, projectId: next.projectId };
        },
      });
    })();
    return worldServicePromise;
  };
  // The project id is only used for diagnostics in this call path; the authoritative id
  // is the one in each hairstyle source.
  const projectIdOf = () => path.basename(projectRoot);
  const explicitExecutable = executable ? path.resolve(executable) : null;

  async function resolveExecutable() {
    const candidates = explicitExecutable
      ? [explicitExecutable]
      : [path.join(projectRoot, 'build', 'NativeGPU.exe'), path.join(projectRoot, 'NativeGPU.exe')];
    for (const candidate of candidates) {
      try {
        await fs.access(candidate);
        return candidate;
      } catch { /* Try the packaged layout next. */ }
    }
    throw new ToolError(
      'NATIVE_BINARY_MISSING',
      'NativeGPU.exe is missing; build it with tools/build.ps1 or use a complete package',
    );
  }

  async function runNative(mode, scenePathValue, frames) {
    const file = await scenePath(projectRoot, scenePathValue);
    const nativeExecutable = await resolveExecutable();
    const reportDirectory = await resolveProjectPath(projectRoot, 'reports', {
      roots: ['reports'], label: 'reports', mustExist: false,
    });
    await fs.mkdir(reportDirectory, { recursive: true });
    const reportName = `mcp-${mode}-${randomUUID()}.json`;
    const reportPath = path.join(reportDirectory, reportName);
    const started = Date.now();
    const modeArgument = mode === 'probe' ? '--probe' : '--smoke';
    const child = spawn(nativeExecutable, [
      modeArgument, '--frames', String(frames), '--scene', file, '--report', reportPath,
      '--no-watch',
    ], { cwd: projectRoot, stdio: 'ignore', windowsHide: true });
    const exitCode = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill();
        reject(new ToolError('NATIVE_TIMEOUT', `Native ${mode} run exceeded 180 seconds`));
      }, 180_000);
      child.once('error', error => {
        clearTimeout(timer);
        reject(new ToolError('NATIVE_START_FAILED', error.message));
      });
      child.once('exit', code => {
        clearTimeout(timer);
        resolve(code);
      });
    });
    let report;
    try {
      report = JSON.parse(await fs.readFile(reportPath, 'utf8'));
    } catch {
      throw new ToolError('NATIVE_REPORT_MISSING', `Native process exited ${exitCode} without a readable report`);
    }
    return {
      exitCode,
      success: exitCode === 0 && report.success === true,
      elapsedMs: Date.now() - started,
      reportPath: `reports/${reportName}`,
      report,
    };
  }

  async function dispatch(name, rawArguments) {
    const args = asObject(rawArguments ?? {}, 'arguments');
    if (name.startsWith('world_')) {
      const world = await worldService();
      return world.call(name, args);
    }
    if (name === 'gameplay_catalog') return gameplayCatalog(path.join(projectRoot,'assets'));
    if (name === 'gameplay_validate') {
      const file=await scenePath(projectRoot,args.scenePath),scene = await loadScene(file),assets=findGameplayAssets(file);
      for (const e of scene.entities) if(e.components) validateComponents(e.components,assets);
      return {valid:true,dependencies:gameplayDependencies(scene,assets).map(({data,...file})=>file),luaExecuted:false};
    }
    if (name === 'gameplay_inspect') {
      const file=await resolveProjectPath(projectRoot,args.reportPath??'reports/gameplay/live.json',{roots:['reports'],label:'reportPath'});
      if((await fs.stat(file)).size>16*1024*1024)throw new ToolError('FILE_TOO_LARGE','Gameplay report exceeds 16 MiB');
      const report=JSON.parse(await fs.readFile(file,'utf8'));
      return report.gameplay??report.payload?.gameplay??report;
    }
    if (name === 'scene_validate') {
      const file = await scenePath(projectRoot, args.scenePath);
      const scene = await readSceneJSON(file);
      const validation = validateScene(scene,{assetsDirectory:findGameplayAssets(file)});
      return {
        valid: validation.valid,
        errors: validation.errors,
        projectId: validation.valid ? scene.projectId : null,
        revision: validation.valid ? scene.revision : null,
        entityCount: validation.valid ? scene.entities.length : null,
        schema: 'assets/scene.schema.json',
      };
    }
    if (name === 'scene_query') {
      const file = await scenePath(projectRoot, args.scenePath);
      const query = {};
      if (args.kind !== undefined) query.kind = args.kind;
      if (args.name !== undefined) query.name = args.name;
      if (args.entityId !== undefined) query.entityId = args.entityId;
      return queryScene(await loadScene(file), query);
    }
    if (name === 'scene_preview' || name === 'scene_apply') {
      const file = await scenePath(projectRoot, args.scenePath);
      if (name === 'scene_preview') {
        const preview = previewTransaction(await loadScene(file), args.transaction);
        const output = {
          projectId: preview.projectId,
          baseRevision: preview.baseRevision,
          revision: preview.revision,
          changes: preview.changes,
        };
        if (args.includeScene === true) output.scene = preview.scene;
        return output;
      }
      const applied = await commitScene(file, args.transaction);
      return {
        projectId: applied.projectId,
        baseRevision: applied.baseRevision,
        revision: applied.revision,
        changes: applied.changes,
        sceneHash: sceneHash(applied.scene),
        hotReload: 'The running native editor reloads assets/*.scene.json and .screen/*.scene.json automatically.',
      };
    }
    if (name === 'material_patch') {
      const file = await scenePath(projectRoot, args.scenePath);
      const options = {
        scenePath: file,
        material: asString(args.material, 'material'),
        changes: asObject(args.changes, 'changes'),
        root: projectRoot,
        expectedRevision: args.expectedRevision,
      };
      return args.apply === true ? applyMaterialPatch(options) : previewMaterialPatch(options);
    }
    if (name === 'scene_undo') {
      const file = await scenePath(projectRoot, args.scenePath);
      const undone = await undoSceneFile(file, {
        projectId: asString(args.projectId, 'projectId'),
        expectedRevision: args.expectedRevision,
      });
      return { revision: undone.revision, sceneHash: sceneHash(undone.scene) };
    }
    if (name === 'scene_build') {
      const file = await scenePath(projectRoot, args.scenePath);
      const outputDirectory = await resolveProjectPath(projectRoot, args.outputDirectory, {
        roots: ['outputs'], label: 'outputDirectory', mustExist: false,
      });
      const options = { target: args.target ?? 'windows-x64' };
      if (path.dirname(file).split(path.sep).includes('.screen')) {
        options.assetsDirectory = path.join(projectRoot, 'assets');
        options.sceneDirectory = path.join(projectRoot, 'assets');
      }
      const built = await buildScene(file, outputDirectory, options);
      return {
        packageDirectory: path.relative(projectRoot, built.packageDirectory).split(path.sep).join('/'),
        cacheHit: built.cacheHit,
        manifest: built.manifest,
      };
    }
    if (HAIR_TOOL_ACTIONS[name]) {
      // One service instance per tool service: undo stacks, idempotency log and bound
      // garments are session state, and sharing it with the editor is what makes the
      // editor and the AI see the same revisions.
      const service = await hairService();
      const action = HAIR_TOOL_ACTIONS[name];
      const result = await service.call(action, {
        ...args,
        projectId: args.projectId ?? projectIdOf(),
        dryRun: args.dryRun === true,
      });
      if (result && result.ok === false) {
        throw new ToolError(result.error.code, result.error.message, result.error.details ?? []);
      }
      return result;
    }
    if (name === 'runtime_smoke' || name === 'engine_probe') {
      const mode = name === 'engine_probe' ? 'probe' : 'smoke';
      const frames = args.frames ?? (mode === 'probe' ? 240 : 360);
      if (!Number.isInteger(frames) || frames < 240 || frames > MAX_SMOKE_FRAMES) {
        throw new ToolError('INVALID_ARGUMENTS', `frames must be an integer between 240 and ${MAX_SMOKE_FRAMES}`);
      }
      return runNative(mode, args.scenePath ?? '.screen/courtyard.scene.json', frames);
    }
    throw new ToolError('UNKNOWN_TOOL', `Unknown tool: ${name}`);
  }

  return {
    root: projectRoot,
    listTools,
    async callTool(name, args) {
      try {
        if (typeof name !== 'string' || !TOOLS.some(tool => tool.name === name)) {
          throw new ToolError('UNKNOWN_TOOL', `Unknown tool: ${name}`);
        }
        return { ok: true, result: await dispatch(name, args) };
      } catch (error) {
        // World failures carry the same shape (code + details) as scene failures, so the
        // editor and a model see one error contract instead of two.
        if (error && error.name === 'WorldError') {
          return {
            ok: false,
            error: { code: error.code, message: error.message, details: error.details ?? {} },
          };
        }
        if (error instanceof SceneError || error instanceof ToolError) {
          return {
            ok: false,
            error: { code: error.code, message: error.message, details: error.details ?? [] },
          };
        }
        return {
          ok: false,
          error: { code: 'TOOL_FAILURE', message: error.message ?? String(error), details: [] },
        };
      }
    },
  };
}
