import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { startEditorServer } from '../editor/server.mjs';

async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'nativegpu-editor-http-'));
  const calls = [];
  let closed = 0;
  const editor = await startEditorServer({ root, service: {
    async call(action, args) { calls.push({ action, args }); return { ok: true, result: { action } }; },
    async close() { closed += 1; },
  } });
  t.after(async () => { await editor.close(); await rm(root, { recursive: true, force: true }); });
  const session = await fetch(`${editor.url}/api/session`).then(response => response.json());
  return { ...editor, root, calls, token: session.token, closed: () => closed };
}

test('editor transport requires local origin, host and session before dispatching writes', async t => {
  const editor = await fixture(t);
  const call = headers => fetch(`${editor.url}/api/call`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', ...headers },
    body: JSON.stringify({ action: 'scene_apply', args: { scenePath: 'assets/example.scene.json' } }),
  });
  assert.equal((await call({})).status, 403);
  assert.equal((await call({ 'X-NativeGPU-Token': editor.token, Origin: 'https://untrusted.example' })).status, 403);
  // fetch normalizes Host; use the HTTP client to exercise DNS-rebinding defense.
  const invalidHost = await new Promise((resolve, reject) => {
    const request = http.get(`${editor.url}/api/session`, { headers: { Host: 'untrusted.example' } }, response => {
      response.resume();
      response.on('end', () => resolve(response.statusCode));
    });
    request.on('error', reject);
  });
  assert.equal(invalidHost, 403);
  assert.equal((await call({ 'X-NativeGPU-Token': editor.token, 'Sec-Fetch-Site': 'cross-site' })).status, 403);
  assert.equal(editor.calls.length, 0);
  const result = await call({ 'X-NativeGPU-Token': editor.token, Origin: editor.url });
  assert.equal(result.status, 200);
  assert.deepEqual(await result.json(), { ok: true, result: { action: 'scene_apply' } });
  assert.equal(editor.calls.length, 1);
  assert.equal((await fetch(`${editor.url}/api/session`, { headers: { Origin: 'https://untrusted.example' } })).status, 403);
});

test('editor transport rejects malformed and oversized requests without dispatch', async t => {
  const editor = await fixture(t);
  const send = (body, type = 'application/json') => fetch(`${editor.url}/api/call`, {
    method: 'POST', headers: { 'Content-Type': type, 'X-NativeGPU-Token': editor.token }, body,
  });
  assert.equal((await send('{')).status, 400);
  assert.equal((await send('[]')).status, 400);
  assert.equal((await send('{}', 'text/plain')).status, 415);
  assert.equal((await send(JSON.stringify({ action: 'scene_open', extra: 'ignored?' }))).status, 400);
  assert.equal((await send('x'.repeat(1024 * 1024 + 1))).status, 413);
  assert.equal(editor.calls.length, 0);
});

test('editor static surface exposes only UI and bounded PNG captures', async t => {
  const editor = await fixture(t);
  assert.equal((await fetch(`${editor.url}/package.json`)).status, 404);
  assert.equal((await fetch(`${editor.url}/.screen/courtyard.scene.json`)).status, 404);
  assert.equal((await fetch(`${editor.url}/captures/../service.mjs`)).status, 404);
  const capture = '12345678-1234-4234-8234-123456789012.png';
  const directory = path.join(editor.root, 'reports', 'editor-captures');
  await mkdir(directory, { recursive: true });
  const png = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  await writeFile(path.join(directory, capture), png);
  const response = await fetch(`${editor.url}/captures/${capture}`);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('content-type'), 'image/png');
  assert.match(response.headers.get('content-security-policy'), /frame-ancestors 'none'/);
  assert.equal(response.headers.get('access-control-allow-origin'), null);
  assert.deepEqual(Buffer.from(await response.arrayBuffer()), png);
  assert.equal((await fetch(`${editor.url}/api/session`, { method: 'PUT' })).status, 405);
});

test('editor shutdown is idempotent and closes its owned service', async t => {
  const editor = await fixture(t);
  await Promise.all([editor.close(), editor.close()]);
  assert.equal(editor.closed(), 1);
  assert.equal(editor.server.listening, false);
});
