#!/usr/bin/env node
// N0 skeleton: consume an INN-14 geometry package and emit the cluster DAG in
// a sidecar a future GPU culling pass can read. No runtime LOD selection here.
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ERROR_CODES, GeometryError, MAX_CLUSTER_TRIANGLES, MAX_CLUSTER_VERTICES,
  stableJSON, summarizeLodLevels, verifyPackage,
} from './geometry-package.mjs';

export const DAG_FORMAT_VERSION = 1;
export const DAG_KIND = 'nativegpu.cluster-dag';

export function buildClusterDag(manifest) {
  const levels = summarizeLodLevels(manifest);
  return {
    formatVersion: DAG_FORMAT_VERSION,
    kind: DAG_KIND,
    package: {
      formatVersion: manifest.formatVersion,
      packageDigest: manifest.packageDigest,
      counts: manifest.counts,
      capabilities: manifest.capabilities ?? null,
    },
    clusterLimits: { maxVertices: MAX_CLUSTER_VERTICES, maxTriangles: MAX_CLUSTER_TRIANGLES },
    bounds: manifest.bounds,
    roots: manifest.clusters.filter(cluster => cluster.parent === -1).map(cluster => cluster.id),
    lodLevels: manifest.lodLevels.map((level, index) => ({
      level: level.level,
      clusterStart: level.clusterStart,
      clusterCount: level.clusterCount,
      clusterIds: manifest.clusters
        .slice(level.clusterStart, level.clusterStart + level.clusterCount)
        .map(cluster => cluster.id),
      triangleCount: levels[index].triangleCount,
      vertexCount: levels[index].vertexCount,
      bounds: levels[index].bounds,
    })),
    clusters: manifest.clusters.map(cluster => ({
      id: cluster.id,
      lod: cluster.lod,
      parent: cluster.parent,
      children: cluster.children,
      vertexOffset: cluster.vertexOffset,
      vertexCount: cluster.vertexCount,
      indexOffset: cluster.indexOffset,
      triangleCount: cluster.triangleCount,
      bounds: cluster.bounds,
      lodError: cluster.lodError ?? 0,
      flags: cluster.flags ?? 0,
      materialHandle: cluster.materialHandle ?? 0,
    })),
    runtimeLodSelector: false,
    streaming: false,
    notes: [
      'Skeleton output only: LOD ranges and DAG links are fixed offline.',
      'No runtime LOD selection, GPU paging, streaming or visibility culling is implemented.',
      'Parent geometry is a deterministic decimation proxy, not a quality-guaranteed simplification.',
    ],
  };
}

export function clusterStats(manifest) {
  const levels = summarizeLodLevels(manifest);
  return {
    formatVersion: manifest.formatVersion,
    capabilities: manifest.capabilities ?? null,
    packageDigest: manifest.packageDigest,
    clusters: manifest.counts.clusters,
    sourceVertices: manifest.counts.sourceVertices,
    sourceTriangles: manifest.counts.sourceTriangles,
    totalVertices: manifest.clusters.reduce((sum, cluster) => sum + cluster.vertexCount, 0),
    totalTriangles: manifest.clusters.reduce((sum, cluster) => sum + cluster.triangleCount, 0),
    bounds: manifest.bounds,
    lodLevels: levels.map(level => ({
      level: level.level,
      clusterStart: level.clusterStart,
      clusterCount: level.clusterCount,
      vertices: level.vertexCount,
      triangles: level.triangleCount,
      bounds: level.bounds,
      lodError: level.lodError,
    })),
    runtimeLodSelector: false,
  };
}

export async function emitClusterDag(manifestPath, outputPath) {
  const verified = await verifyPackage(manifestPath);
  const sidecar = buildClusterDag(verified.manifest);
  const target = outputPath
    ? path.resolve(outputPath)
    : path.join(path.dirname(verified.manifestPath), 'cluster-dag.json');
  await fs.mkdir(path.dirname(target), { recursive: true });
  await fs.writeFile(target, `${stableJSON(sidecar)}\n`, 'utf8');
  return { target, sidecar, packageDigest: verified.packageDigest };
}

const USAGE = `Cluster DAG skeleton (N0, JSON output)
  node tools/offline/cluster-dag.mjs emit --package <geometry.package.json> [--out <cluster-dag.json>]
  node tools/offline/cluster-dag.mjs stats --package <geometry.package.json>
`;

function parseOptions(args, allowed) {
  if (args.length % 2) throw new GeometryError(ERROR_CODES.ARGUMENT, 'Every option requires a value');
  const parsed = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!allowed.includes(args[i]) || Object.hasOwn(parsed, args[i])) {
      throw new GeometryError(ERROR_CODES.ARGUMENT, `Unknown or duplicate option: ${args[i]}`);
    }
    parsed[args[i]] = args[i + 1];
  }
  return parsed;
}

async function main(args) {
  const [command, ...rest] = args;
  if (!command || command === 'help' || command === '--help') return USAGE;
  if (command === 'emit') {
    const options = parseOptions(rest, ['--package', '--out']);
    if (!options['--package']) throw new GeometryError(ERROR_CODES.ARGUMENT, 'emit requires --package');
    const result = await emitClusterDag(options['--package'], options['--out']);
    return {
      out: result.target,
      packageDigest: result.packageDigest,
      clusters: result.sidecar.clusters.length,
      lodLevels: result.sidecar.lodLevels.length,
      runtimeLodSelector: result.sidecar.runtimeLodSelector,
    };
  }
  if (command === 'stats') {
    const options = parseOptions(rest, ['--package']);
    if (!options['--package']) throw new GeometryError(ERROR_CODES.ARGUMENT, 'stats requires --package');
    const verified = await verifyPackage(options['--package']);
    return clusterStats(verified.manifest);
  }
  throw new GeometryError(ERROR_CODES.ARGUMENT, `Unknown command: ${command}`);
}

const invoked = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (invoked) {
  main(process.argv.slice(2)).then(result => {
    if (result !== null) console.log(JSON.stringify({ ok: true, result }, null, 2));
  }).catch(error => {
    console.error(JSON.stringify({
      ok: false,
      error: { code: error.code ?? 'NG-E-GEOMETRY-IO', message: error.message, details: error.details ?? [] },
    }, null, 2));
    process.exitCode = 1;
  });
}
