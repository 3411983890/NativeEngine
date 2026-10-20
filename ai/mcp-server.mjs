#!/usr/bin/env node
import * as path from 'node:path';
import * as readline from 'node:readline';
import { pathToFileURL } from 'node:url';
import { createToolService, listTools } from './tool-service.mjs';

export const SERVER_NAME = 'nativegpu-scene';
export const SERVER_VERSION = '0.3.0';
export const DEFAULT_PROTOCOL_VERSION = '2024-11-05';
const MAX_MESSAGE_BYTES = 1024 * 1024;

function reply(id, result) {
  return { jsonrpc: '2.0', id, result };
}

function failure(id, code, message, data) {
  const error = { code, message };
  if (data !== undefined) error.data = data;
  return { jsonrpc: '2.0', id, error };
}

export function createMcpHandler({ service, protocolVersion = DEFAULT_PROTOCOL_VERSION } = {}) {
  if (!service) throw new Error('createMcpHandler requires a tool service');
  return async function handle(message) {
    if (message === null || typeof message !== 'object' || Array.isArray(message)) {
      return failure(null, -32600, 'Invalid Request');
    }
    const { id, method, params } = message;
    if (message.jsonrpc !== '2.0' || typeof method !== 'string') {
      return failure(id ?? null, -32600, 'Invalid Request');
    }
    if (id === undefined) {
      // Notifications intentionally produce no response.
      return undefined;
    }
    if (method === 'initialize') {
      if (params !== undefined && (params === null || typeof params !== 'object')) {
        return failure(id, -32602, 'initialize params must be an object');
      }
      return reply(id, {
        protocolVersion: typeof params?.protocolVersion === 'string' ? params.protocolVersion : protocolVersion,
        capabilities: { tools: { listChanged: false } },
        serverInfo: { name: SERVER_NAME, version: SERVER_VERSION },
        instructions: 'NativeGPU deterministic scene tools. Preview before apply; the running editor hot-reloads assets/*.scene.json. Prefer scene_build and runtime_smoke for verification.',
      });
    }
    if (method === 'ping') return reply(id, {});
    if (method === 'tools/list') return reply(id, { tools: listTools() });
    if (method === 'tools/call') {
      if (params === null || typeof params !== 'object' || typeof params.name !== 'string') {
        return failure(id, -32602, 'tools/call requires params.name');
      }
      const response = await service.callTool(params.name, params.arguments ?? {});
      const text = JSON.stringify(response.ok ? response.result : response.error, null, 2);
      return reply(id, {
        content: [{ type: 'text', text }],
        isError: response.ok !== true,
      });
    }
    if (method === 'resources/list') return reply(id, { resources: [] });
    if (method === 'prompts/list') return reply(id, { prompts: [] });
    if (method === 'logging/setLevel') return reply(id, {});
    return failure(id, -32601, `Method not found: ${method}`);
  };
}

export async function serve({ input = process.stdin, output = process.stdout, service } = {}) {
  const toolService = service ?? createToolService();
  const handle = createMcpHandler({ service: toolService });
  const lines = readline.createInterface({ input, crlfDelay: Infinity });
  let queue = Promise.resolve();
  const processLine = async line => {
    if (!line.trim()) return;
    if (Buffer.byteLength(line, 'utf8') > MAX_MESSAGE_BYTES) {
      output.write(`${JSON.stringify(failure(null, -32700, 'Message exceeds 1 MiB'))}\n`);
      return;
    }
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      output.write(`${JSON.stringify(failure(null, -32700, 'Parse error'))}\n`);
      return;
    }
    try {
      const response = await handle(message);
      if (response !== undefined) output.write(`${JSON.stringify(response)}\n`);
    } catch (error) {
      output.write(`${JSON.stringify(failure(message?.id ?? null, -32603, 'Internal error', String(error)))}\n`);
    }
  };
  lines.on('line', line => {
    queue = queue.then(() => processLine(line)).catch(error => {
      process.stderr.write(`nativegpu mcp: ${error.message}\n`);
    });
  });
  await new Promise(resolve => lines.once('close', resolve));
  await queue;
  return { root: toolService.root };
}

async function main() {
  if (process.argv.includes('--help') || process.argv.includes('-h')) {
    process.stderr.write('NativeGPU scene MCP server (stdio)\nSet NATIVEGPU_PROJECT_ROOT to override the project root.\n');
    return;
  }
  const service = createToolService();
  if (!process.argv.includes('--quiet')) {
    process.stderr.write(`NativeGPU MCP ${SERVER_VERSION} ready; project ${service.root}\n`);
  }
  await serve({ service });
}

const invokedDirectly = process.argv[1]
  && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href;
if (invokedDirectly) {
  main().catch(error => {
    process.stderr.write(`nativegpu mcp: ${error.stack ?? error.message}\n`);
    process.exitCode = 1;
  });
}
