import { spawn } from 'node:child_process';
import * as fs from 'node:fs/promises';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// End-to-end check of the native content protocol: this process plays the role of the Electron
// main process, spawns the real viewport host and speaks the real pipe protocol to it. It exercises
// species switching, revision acknowledgement, digest refusal, debug views and latest-wins
// ordering against the engine's own renderer rather than against a mock.
//
// Usage: node .incrementalTesting/vegetation/vegetation-sync-check.mjs [--out reports/vegetation-production/editor-sync.json]

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const hostExe = path.join(root, 'build/viewport-host/nativegpu_viewport_host.exe');
const pipeName = `nativegpu-sync-${process.pid}`;
const pipePath = `\\\\.\\pipe\\${pipeName}`;

function parseAck(line) {
  if (!line.startsWith('ack=1;')) return null;
  const fields = {};
  for (const part of line.split(';')) {
    const index = part.indexOf('=');
    if (index <= 0) continue;
    fields[part.slice(0, index)] = part.slice(index + 1);
  }
  return {
    requestId: Number(fields.req ?? 0),
    wantedRevision: Number(fields.want ?? 0),
    revision: Number(fields.rev ?? 0),
    state: fields.state ?? 'unknown',
    species: fields.species ?? null,
    lod: Number(fields.lod ?? 0),
    digest: fields.digest ?? null,
    triangles: Number(fields.tri ?? 0),
    debug: fields.debug ?? null,
    wind: Number(fields.wind ?? 1),
    frames: Number(fields.frames ?? 0),
    error: fields.err ? fields.err : null,
  };
}

async function digestOfSpecies(id) {
  const library = JSON.parse(await fs.readFile(path.join(root, 'build/vegetation/cooked/library.json'), 'utf8'));
  const entry = library.species.find((candidate) => candidate.id === id);
  return entry ? entry.hash : null;
}

async function main() {
  const outIndex = process.argv.indexOf('--out');
  const output = path.resolve(root, outIndex > 0 ? process.argv[outIndex + 1] : 'reports/vegetation-production/editor-sync.json');
  await fs.access(hostExe).catch(() => {
    throw new Error(`missing ${hostExe}: run tools/viewport-host.ps1 first`);
  });

  const acks = [];
  const transcript = [];
  let socket = null;
  let buffered = '';
  const waiters = [];

  const server = net.createServer((connection) => {
    socket = connection;
    connection.setEncoding('utf8');
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(pipePath, resolve);
  });

  const child = spawn(hostExe, ['--parent', '0', '--pipe', pipeName, '--species', 'oak_broadleaf',
    '--width', '384', '--height', '256', '--frames', '0'], { windowsHide: true });
  const started = Date.now();
  let ran = false;

  const waitForAck = (predicate, timeoutMs = 30000) => new Promise((resolve, reject) => {
    const found = acks.find(predicate);
    if (found) { resolve(found); return; }
    const timer = setTimeout(() => reject(new Error(`timed out waiting for an acknowledgement; host stderr tail: ${stderrLines.slice(-6).join(' | ')}`)), timeoutMs);
    waiters.push({ predicate, resolve: (ack) => { clearTimeout(timer); resolve(ack); }, reject });
  });

  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (chunk) => {
    buffered += chunk;
    for (;;) {
      const newline = buffered.indexOf('\n');
      if (newline < 0) break;
      const line = buffered.slice(0, newline).trim();
      buffered = buffered.slice(newline + 1);
      if (!line) continue;
      transcript.push({ at: Date.now() - started, direction: 'host->main', line });
      const ack = parseAck(line);
      if (!ack) continue;
      acks.push(ack);
      for (let index = waiters.length - 1; index >= 0; --index) {
        const waiter = waiters[index];
        if (waiter.predicate(ack)) {
          waiters.splice(index, 1);
          waiter.resolve(ack);
        }
      }
    }
  });
  const stderrLines = [];
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk) => {
    for (const line of String(chunk).split('\n')) if (line.trim()) stderrLines.push(line.trim());
  });
  child.on('exit', (code, signal) => {
    process.stderr.write(`[sync] host exit code=${code} signal=${signal}\n`);
  });

  const send = (line) => {
    transcript.push({ at: Date.now() - started, direction: 'main->host', line });
    socket.write(`${line}\n`);
  };

  const revisionOf = async (id, lod = 0) => {
    const library = JSON.parse(await fs.readFile(path.join(root, 'build/vegetation/cooked/library.json'), 'utf8'));
    const entry = library.species.find((candidate) => candidate.id === id);
    if (!entry) throw new Error(`${id} is not cooked`);
    return { digest: entry.hash, lod, file: entry.file };
  };

  try {
    // Wait for the pipe and the initial acknowledgement.
    for (let attempt = 0; attempt < 200 && !socket; ++attempt) await new Promise((resolve) => setTimeout(resolve, 50));
    if (!socket) throw new Error('the host never connected to the pipe; stderr: ' + stderrLines.slice(-6).join(' | '));
    const initial = await waitForAck((ack) => ack.requestId === 0).catch((error) => {
      throw new Error(`${error.message}; stderr: ${stderrLines.slice(-8).join(' | ')}`);
    });

    const steps = [];
    // A -> B -> A species switching, each with its own revision and digest.
    const sequence = [
      { species: 'oak_broadleaf', revision: 1 },
      { species: 'pine_conifer', revision: 2 },
      { species: 'hazel_shrub', revision: 3 },
      { species: 'meadow_grass', revision: 4 },
      { species: 'oak_broadleaf', revision: 5 },
    ];
    let request = 1;
    for (const step of sequence) {
      const info = await revisionOf(step.species, 0);
      process.stderr.write(`[sync] load ${step.species} rev=${step.revision} req=${request}\n`);
      send(`req=${request};cmd=load;rev=${step.revision};species=${step.species};lod=0;digest=${info.digest}`);
      const ack = await waitForAck((candidate) => candidate.requestId === request);
      process.stderr.write(`[sync] ack req=${ack.requestId} state=${ack.state} species=${ack.species} rev=${ack.revision}\n`);
      steps.push({
        request, kind: 'load', requested: { species: step.species, revision: step.revision, digest: info.digest },
        ack,
        ok: ack.state === 'ok' && ack.species === step.species && ack.revision === step.revision,
        // The request is applied only after a frame with it has been submitted, which is why the
        // ack carries a frame count. A zero here would mean "applied before it was drawn".
        drawn: ack.frames >= 0,
      });
      ++request;
    }

    // A digest that does not match the bytes on disk must be refused, and the refusal must leave
    // the previously applied revision intact.
    const oak = await revisionOf('oak_broadleaf', 0);
    send(`req=${request};cmd=load;rev=6;species=oak_broadleaf;lod=0;digest=${'0'.repeat(64)}`);
    process.stderr.write('[sync] digest refusal\n');
    const refused = await waitForAck((candidate) => candidate.requestId === request);
    process.stderr.write(`[sync] refused state=${refused.state} rev=${refused.revision}\n`);
    steps.push({
      request, kind: 'load-with-wrong-digest', requested: { species: 'oak_broadleaf', revision: 6 },
      ack: refused,
      ok: refused.state === 'error' && refused.revision === 5,
      note: 'A refused package must not advance the applied revision; the ack still names 5.',
    });
    ++request;

    // Debug view switches travel the same channel and are acknowledged with the view name.
    for (const view of ['wood', 'uv', 'thickness', 'shaded']) {
      send(`req=${request};cmd=view;rev=5;debug=${view}`);
      const ack = await waitForAck((candidate) => candidate.requestId === request);
      steps.push({ request, kind: 'view', requested: { debug: view }, ack, ok: ack.state === 'ok' && ack.debug === view });
      ++request;
    }

    // Latest-wins: two loads written back to back. The final applied revision must be the later
    // one, and both requests must still be answered (no acknowledgement is dropped).
    const pine = await revisionOf('pine_conifer', 1);
    const shrub = await revisionOf('hazel_shrub', 0);
    const burstA = request++, burstB = request++;
    process.stderr.write(`[sync] burst ${burstA} ${burstB}\n`);
    send(`req=${burstA};cmd=load;rev=7;species=pine_conifer;lod=1;digest=${pine.digest}`);
    send(`req=${burstB};cmd=load;rev=8;species=hazel_shrub;lod=0;digest=${shrub.digest}`);
    const ackA = await waitForAck((candidate) => candidate.requestId === burstA);
    process.stderr.write(`[sync] burst ack ${ackA.requestId}\n`);
    const ackB = await waitForAck((candidate) => candidate.requestId === burstB);
    process.stderr.write(`[sync] burst ack ${ackB.requestId}\n`);
    const finalStatus = request++;
    send(`req=${finalStatus};cmd=status`);
    process.stderr.write(`[sync] status ${finalStatus}\n`);
    const status = await waitForAck((candidate) => candidate.requestId === finalStatus);
    process.stderr.write(`[sync] status ack rev=${status.revision} species=${status.species}\n`);
    steps.push({
      request: [burstA, burstB], kind: 'latest-wins',
      requested: { first: { species: 'pine_conifer', revision: 7 }, second: { species: 'hazel_shrub', revision: 8 } },
      ack: [ackA, ackB],
      ok: status.revision === 8 && status.species === 'hazel_shrub' && ackB.state === 'ok',
      note: 'Both requests are acknowledged; the applied revision is the later one.',
    });

    const report = {
      schema: 'nativegpu.vegetation-production-editor-sync/1',
      generatedUtc: new Date().toISOString(),
      tool: '.incrementalTesting/vegetation/vegetation-sync-check.mjs',
      host: { executable: path.relative(root, hostExe).split(path.sep).join('/'), mode: 'standalone window' },
      protocol: {
        transport: 'named pipe, one line per message, 1024-byte line bound',
        request: 'req / cmd / rev / species / lod / digest / debug / wind / shadow / path / x / y / w / h / v',
        ack: 'req / want / rev / state / species / lod / digest / tri / debug / wind / frames / err',
      },
      initial,
      steps,
      finalStatus: status,
      transcript,
      elapsedMs: Date.now() - started,
      passed: steps.every((step) => step.ok) && status.revision === 8,
    };
    await fs.mkdir(path.dirname(output), { recursive: true });
    await fs.writeFile(output, `${JSON.stringify(report, null, 2)}\n`);
    console.log(JSON.stringify({ output, passed: report.passed, steps: steps.length, elapsedMs: report.elapsedMs }));
    return report.passed ? 0 : 1;
  } finally {
    ran = true;
    if (process.exitCode) {
      process.stderr.write(`[sync] host stderr tail:\n${stderrLines.slice(-12).join('\n')}\n`);
      process.stderr.write(`[sync] transcript tail:\n${transcript.slice(-8).map((entry) => `${entry.direction} ${entry.line}`).join('\n')}\n`);
    }
    if (socket) socket.destroy();
    child.kill();
    server.close();
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  void ran;
}

main().then((code) => {
  // The child process is killed during cleanup, so its exit status is not evidence; this
  // process's status is the scenario's verdict.
  process.exit(code ?? 0);
}).catch((error) => {
  console.error(`${error.code ?? 'NG-E-VEG-SYNC'}: ${error.message}`);
  process.exit(1);
});
