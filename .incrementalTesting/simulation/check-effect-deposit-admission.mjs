import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';

// Re-evaluate frozen admission runs after fixing the test's frame expectation.
// The host CLI clamps --frames to at least 240; no renderer run is repeated.
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args=process.argv.slice(2);assert.equal(args.length,2,'Expected reference-report.json output-report.json');
const reference=path.resolve(args[0]),output=path.resolve(args[1]);assert.notEqual(reference,output,'Preserve the original report');
const json=async p=>JSON.parse((await fs.readFile(p,'utf8')).replace(/^\uFEFF/,''));
const hash=async p=>crypto.createHash('sha256').update(await fs.readFile(p)).digest('hex').toUpperCase();
const report=await json(reference),directory=path.dirname(reference);
assert.equal(report.admissionDeltaOnly,true);assert.equal(report.fullRegressionRun,false);
assert.equal(report.buildSnapshotFrozenBeforeRuns,true);assert.deepEqual(report.sourceChangesAfterFreeze,[]);
let frozenFiles=0;
async function verify(file,expected){assert.equal(await hash(file),expected.toUpperCase(),`Frozen input mismatch: ${file}`);++frozenFiles;}
await verify(path.join(directory,'NativeGPU.exe'),report.hostBuild.binaryHash);
for(const [file,expected] of Object.entries(report.hostBuild.sources))await verify(path.join(directory,'host-source-snapshot',path.relative(root,file)),expected);
for(const [file,expected] of Object.entries(report.inputs))await verify(path.join(directory,'asset-source-snapshot',file),expected);
const names=['admission-base','admission-unused-bad','admission-capacity','admission-selected-bad'];
assert.deepEqual(Object.keys(report.runs),names);
for(const name of names){assert.deepEqual(await json(path.join(directory,name+'.json')),report.runs[name].data,'Embedded run differs from original host JSON');if(report.runs[name].imageHash)await verify(path.join(directory,name+'.png'),report.runs[name].imageHash);}
const runs=report.runs,checks=[];
function check(name,ok,detail){checks.push({name,ok:!!ok,detail});}
for(const name of names.slice(0,3)){
  const run=runs[name],data=run.data,g=data.probes.gpuEffects,v=data.probes.renderTargets;
  check(`${name} successful actual draw`,run.exit===0&&data.success&&g.frames===240&&g.drawIndirects>0&&!g.deposits.failed,`frames=${g.frames}, draws=${g.drawIndirects}`);
  check(`${name} actual debug layer has no errors`,v.validationLayer&&v.validationErrors===0,`errors=${v.validationErrors}, warnings=${v.validationWarnings}`);
  check(`${name} preserves CPU authority`,!!data.determinism.finalHash&&data.determinism.finalHash===runs['admission-base'].data.determinism.finalHash,'same game scene/input');
}
const unused=runs['admission-unused-bad'].data.probes.gpuEffects.deposits;
check('unused unsupported writer allocates no fields or reservations',unused.available&&unused.instances===0&&unused.reservations===0&&unused.allocationBytes===0&&unused.admissionRejected===0,'mixed library selects a non-writing sprite');
check('unused unsupported writer preserves sprite pixels',!!runs['admission-base'].imageHash&&runs['admission-base'].imageHash===runs['admission-unused-bad'].imageHash,'same selected asset and frame');
const cap=runs['admission-capacity'].data,d=cap.probes.gpuEffects.deposits;
check('64 requested writers admit 32 and reject 32 before recording',cap.effectRuntime.instances===32&&cap.effectRuntime.rejectedInstances===32&&d.reservations===32&&d.admissionRejected===32&&d.instances===32,`instances=${d.instances}, rejected=${d.admissionRejected}`);
check('accepted sources keep drawing and publishing under capacity rejection',d.shared===32&&d.rewrites===32*d.frames&&d.worldSources===32&&d.lastAdmissionError.includes('count')&&d.allocationBytes<=128*1024*1024&&!d.failed,`shared=${d.shared}, bytes=${d.allocationBytes}`);
const bad=runs['admission-selected-bad'],bd=bad.data.probes.gpuEffects.deposits;
check('selected unsupported writer rejects spawn without poisoning recorder',bad.exit!==0&&!bad.data.success&&bad.data.error.includes('ratio')&&bd.available&&!bd.failed&&bd.admissionRejected===1&&bd.reservations===0&&bd.allocationBytes===0,bad.data.error);
const result={schema:'nativegpu.effect-deposit-admission-recheck/1',ok:checks.every(c=>c.ok),reference:path.relative(root,reference).replaceAll('\\','/'),referenceHash:await hash(reference),verifier:{path:'.incrementalTesting/simulation/check-effect-deposit-admission.mjs',hash:await hash(fileURLToPath(import.meta.url))},frozenFiles,checks,rendererRerun:false,fullRegressionRun:false,correction:'Only the three 80-frame expectations were wrong: host/main.cpp clamps --frames to at least 240. All 14 assertions were re-evaluated from unchanged frozen raw host JSON and images. The original 11/14 report is preserved.'};
await fs.mkdir(path.dirname(output),{recursive:true});await fs.writeFile(output,JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify({ok:result.ok,checks:checks.length,frozenFiles,output}));if(!result.ok)process.exitCode=1;
