import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {randomUUID} from 'node:crypto';
import {validateScene,loadScene,buildScene} from '../../engine/scene/index.mjs';
import {compileWater,COOKED_WATER_SCHEMA} from '../../engine/water/asset.mjs';
import {canonicalJson,sha256Hex} from '../../engine/assets/canonical.mjs';

const root=path.resolve(import.meta.dirname,'../..');
const base=()=>({schemaVersion:2,projectId:randomUUID(),name:'Water scene contract',revision:0,settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},entities:[
    {id:randomUUID(),name:'Player',kind:'player',position:[0,0,5],scale:[.6,1.8,.6],color:[1,1,1],collider:true},
    {id:randomUUID(),name:'Bottom',kind:'ground',position:[0,-.5,0],scale:[8,.5,8],color:[.3,.3,.3],collider:true}
]});
test('water scene references validate paths, optical coefficients and coordinates',()=>{
    const scene=base();scene.water={file:'water/pond.ngwater.json',sha256:'a'.repeat(64),origin:[1,2,3]};assert.equal(validateScene(scene).valid,true);
    for(const changes of [{file:'../pond.ngwater.json'},{file:'C:/pond.ngwater.json'},{sha256:'bad'},{origin:[0,NaN,0]},{roughness:0},{scattering:[2,0,0]},{absorption:[-1,0,0]},{typo:1}]){
        assert.equal(validateScene({...scene,water:{...scene.water,...changes}}).valid,false,JSON.stringify(changes));
    }
});
test('water packages retain hashed cooked dependencies and reject changed or invalid payloads',async t=>{
    const temporary=await fs.mkdtemp(path.join(os.tmpdir(),'native-water-scene-'));t.after(()=>fs.rm(temporary,{recursive:true,force:true}));
    const assets=path.join(temporary,'assets');await fs.mkdir(path.join(assets,'water'),{recursive:true});await fs.mkdir(path.join(temporary,'.screen'));
    for(const directory of ['gameplay','scripts/gameplay'])await fs.cp(path.join(root,'assets',directory),path.join(assets,directory),{recursive:true});
    const source=JSON.parse(await fs.readFile(path.join(root,'assets/water/pond_still.water.json'),'utf8'));const cooked=compileWater(source);
    const bytes=canonicalJson({schema:COOKED_WATER_SCHEMA,payloadJson:canonicalJson(cooked.payload),hash:cooked.hash})+'\n';
    const file='water/pond.ngwater.json';await fs.writeFile(path.join(assets,file),bytes);
    const scene=base();scene.water={file,sha256:sha256Hex(bytes),origin:[0,2,0]};const scenePath=path.join(temporary,'.screen/main.scene.json');await fs.writeFile(scenePath,JSON.stringify(scene));
    const loaded=await loadScene(scenePath);assert.deepEqual(loaded.water,scene.water);
    const built=await buildScene(scenePath,path.join(temporary,'build'));assert.ok(built.manifest.includedAssetTypes.includes('water-body'));
    assert.equal(await fs.readFile(path.join(built.packageDirectory,file),'utf8'),bytes);
    await fs.appendFile(path.join(assets,file),' ');await assert.rejects(buildScene(scenePath,path.join(temporary,'changed')),/dependency changed/);
    const malformed=JSON.stringify({schema:COOKED_WATER_SCHEMA,payloadJson:'{}',hash:'b'.repeat(64)});await fs.writeFile(path.join(assets,file),malformed);scene.water.sha256=sha256Hex(malformed);await fs.writeFile(scenePath,JSON.stringify(scene));
    await assert.rejects(buildScene(scenePath,path.join(temporary,'malformed')),/Invalid water dependency/);
});
