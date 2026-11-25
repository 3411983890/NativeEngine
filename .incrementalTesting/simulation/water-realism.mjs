import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import assert from 'node:assert/strict';
import {promisify} from 'node:util';
import {execFile} from 'node:child_process';
import {createHash} from 'node:crypto';
import {readNgtex} from '../../tools/textures.mjs';

const root=path.resolve(import.meta.dirname,'../..');
const run=promisify(execFile),options={cwd:root,windowsHide:true,timeout:240000,maxBuffer:131072};
const cpuOnly=process.argv.includes('--cpu-only');
const native=path.join(root,'build/water-realism-native');
await fs.mkdir(native,{recursive:true});
try{
    const compiler=path.join(root,'.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe');
    const binary=path.join(native,'water-realism.exe');
    await run(compiler,['-std=c++17','-O2','-static','-DNOMINMAX','-DUNICODE','-D_UNICODE',
        '.incrementalTesting/simulation/water-realism.cpp','engine/native/water/body.cpp','engine/native/water/asset.cpp',
        // PondDemo's fixed-tick footprint lives in the coupling module; the CPU
        // harness must link it rather than silently dropping the entry point.
        'engine/native/water/coupling.cpp','engine/native/water/transfer.cpp',
        'engine/native/assets/sha256.cpp','engine/native/determinism.cpp','-lbcrypt','-o',binary],options);
    const contracts=JSON.parse((await run(binary,[],options)).stdout);
    assert.equal(contracts.success,true);
    const assets=await run(process.execPath,['--test','tests/water.test.mjs','.incrementalTesting/simulation/water-scene.test.mjs'],options);
    if(cpuOnly){console.log(JSON.stringify({success:true,scope:'water CPU interaction and asset contracts',contracts,assets:assets.stdout}));}
    else{
        const arg=process.argv.indexOf('--project');
        const project=path.resolve(arg>=0?process.argv[arg+1]:path.join(os.homedir(),'Documents/nativeEngineProject/InteractivePond'));
        const scene=path.join(project,'.screen/main.scene.json');await fs.access(scene);
        const hostArg=process.argv.indexOf('--host');
        const host=path.resolve(hostArg>=0?process.argv[hostArg+1]:path.join(root,'build/pond-host'));
        if(!process.argv.includes('--skip-build'))await run('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tools/build-effects-host.ps1','-OutputDirectory',path.relative(root,host)],{...options,timeout:1200000});
        const manifest=JSON.parse((await fs.readFile(path.join(host,'host-inputs.json'),'utf8')).replace(/^\uFEFF/,''));
        const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
        const executable=path.join(host,'NativeGPU.exe');assert.equal(hash(await fs.readFile(executable)),manifest.binaryHash.toLowerCase());
        for(const [file,sha] of Object.entries(manifest.sources))assert.equal(hash(await fs.readFile(file)).toUpperCase(),sha.toUpperCase(),`stale host: ${file}`);
        const rippleBinary=path.join(native,'water-ripple-gpu.exe');
        await run(compiler,['-std=c++17','-O2','-static','-DNOMINMAX','-DUNICODE','-D_UNICODE',`-I${path.join(host,'generated')}`,
            '.incrementalTesting/simulation/water-ripple-gpu.cpp','renderer/water_ripple.cpp','renderer/water_draw.cpp','renderer/matrix_math.cpp',
            '-ld3d12','-ldxgi','-ldxguid','-ld3dcompiler','-o',rippleBinary],options);
        const ripple=JSON.parse((await run(rippleBinary,[],options)).stdout);assert.equal(ripple.success,true);
        for(const name of ['pond_water','pond_bed']){const t=readNgtex(await fs.readFile(path.join(project,'assets/textures',name+'.orm.ngtex')));assert.equal(t.formatName,'rgba8');assert.equal(t.mips[0].data[3],0,'nonemissive ORM alpha');}
        const reports=path.join(project,'reports/water-realism');await fs.mkdir(reports,{recursive:true});
        await run(executable,['--animation-demo','--no-stress','--audio','off','--validation','--scene',scene,'--textures',path.join(project,'assets/textures'),
            '--frames','240','--screenshot-frame','110','--splash','water_splash','--screenshot',path.join(reports,'interactive.png'),'--report',path.join(reports,'interactive.json')],{...options,cwd:project});
        const report=JSON.parse(await fs.readFile(path.join(reports,'interactive.json'),'utf8'));
        assert.equal(report.success,true,JSON.stringify(report.errors??report.error));
        assert.equal(report.probes.water.enabled,true);assert.ok(report.probes.water.frames>=240);assert.ok(report.pondDemo.disturbances>0);
        assert.equal(report.probes.renderTargets.validationErrors,0);
        // Splash droplets are spawned from the recorded water contacts and nowhere else.
        // A missing counter here would let an invisible feature pass as a delivered one.
        assert.equal(report.splash.enabled,true,'splash runtime was not configured');
        assert.equal(report.splash.asset,'water_splash');
        assert.ok(report.splash.contacts>0,'water contacts never reached the splash stage');
        assert.ok(report.splash.spawned>0,'no splash instance was spawned');
        assert.equal(report.splash.failed,0,`splash spawn failed: ${report.error}`);
        assert.equal(report.splash.capacityRejected,0,'splash instance capacity was exhausted');
        assert.ok(report.splash.peakIntensity>0&&report.splash.peakIntensity<=1,'splash intensity left its [0,1] band');
        assert.ok(report.splash.runtimeTick>0,'splash particle clock never advanced');
        // Separable optical evidence. A single beauty frame cannot tell a wrong normal
        // from a wrong thickness, so every debug view is captured from the same frame
        // index and asserted independently.
        const views=[];
        if(!process.argv.includes('--skip-aov')){
            const aov=path.join(reports,'aov');await fs.mkdir(aov,{recursive:true});
            for(const mode of [1,2,3,4,5,6,7]){
                const image=path.join(aov,`debug-${mode}.png`),single=path.join(aov,`debug-${mode}.json`);
                await run(executable,['--animation-demo','--no-stress','--audio','off','--validation','--scene',scene,'--textures',path.join(project,'assets/textures'),
                    '--frames','112','--screenshot-frame','110','--water-debug',String(mode),'--screenshot',image,'--report',single],{...options,cwd:project});
                const produced=JSON.parse(await fs.readFile(single,'utf8'));
                assert.equal(produced.success,true,`water debug view ${mode}: ${produced.error}`);
                assert.ok(produced.probes.water.frames>=110,`water debug view ${mode} stopped early`);
                views.push({mode,image});
            }
        }
        const evidence={success:true,scope:'water CPU interaction, GPU residual readback, optical pass and contact splash',contracts,ripple,water:report.probes.water,splash:report.splash,disturbances:report.pondDemo.disturbances,hostHash:manifest.binaryHash,screenshot:path.join(reports,'interactive.png'),debugViews:views};
        await fs.writeFile(path.join(reports,'acceptance.json'),JSON.stringify(evidence,null,2)+'\n');console.log(JSON.stringify(evidence));
    }
}catch(e){console.error(e.stdout??'',e.stderr??'',e.stack);process.exitCode=1;}
