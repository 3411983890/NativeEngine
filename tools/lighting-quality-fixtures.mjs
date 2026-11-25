import { readFileSync,writeFileSync,mkdirSync } from 'node:fs';
import { validateScene } from '../engine/scene/index.mjs';
const directory='reports/lighting-quality';mkdirSync(directory,{recursive:true});
const town=JSON.parse(readFileSync('.screen/town.scene.json','utf8'));
let sequence=1;
function entity(name,kind,position,scale,color=[1,1,1],extra={}) {
  return {id:`c2000000-0000-4000-8000-${String(sequence++).padStart(12,'0')}`,
    name,kind,position,scale,color,collider:kind!=='light',material:'default',...extra};
}
function base() {
  sequence=1;
  return {schemaVersion:2,projectId:'c1000000-0000-4000-8000-000000000001',name:'Lighting acceptance',revision:0,
    settings:{walkSpeed:3,runSpeed:6,jumpSpeed:7,gravity:20},
    materials:{default:town.materials.default,metal:town.materials.metal},
    lighting:{quality:1,keyDirection:[0.3,0.8,-0.5],keyColor:[1,1,1],keyIntensity:0,
      ambient:[0,0,0],skyColor:[0,0,0],fogDensity:0,fogHeightFalloff:0,fogBaseHeight:0,
      cloudCoverage:0,cloudHeight:40,cloudThickness:20,cloudSpeed:0,
      toneMap:true,exposure:1,localShadows:false,ibl:false,ddgi:false,cloudShadows:false,temporal:true,maxShadowLights:4},
    entities:[entity('Ground','ground',[0,-0.5,0],[100,1,100]),
      entity('Player','player',[0,1,10],[0.8,1.8,0.8]),
      entity('Receiver','wall',[0,2,0],[12,8,1])]};
}
const cases=[];
function save(id,scene,{camera='0,2,20,0,2,0',view,frames=100,shot=80,size='640x360',cameraCut}={}) {
  const result=validateScene(scene);if(!result.valid)throw new Error(`${id}: ${result.errors.join('\n')}`);
  const file=`${directory}/${id}.scene.json`;writeFileSync(file,JSON.stringify(scene,null,2)+'\n');
  cases.push({id,scene:file,camera,view,frames,shot,size,cameraCut});
}
const uniform=base();uniform.lighting.fogDensity=0.03;save('fog-uniform',uniform,{view:'fog'});
const localFog=base();localFog.lighting.fogVolumes=[{center:[0,2,8],extent:[20,20,6],density:0.03}];
save('fog-local',localFog,{view:'fog'});
const local=base();local.entities.push(entity('Point','light',[-3,2,6],[0.1,0.1,0.1],[1,1,1],
  {light:{color:[1,0.8,0.6],intensity:6,range:20,castSurfaceShadow:true,castVolumetricShadow:true,volumetricIntensity:1}}));
local.entities.push(entity('Blocker','prop',[-1.5,2,3],[0.8,1.2,0.8]));
local.lighting.localShadows=true;save('local-shadow-on',local);
const localOff=structuredClone(local);localOff.lighting.localShadows=false;save('local-shadow-off',localOff);
const clearLocal=structuredClone(local);clearLocal.entities=clearLocal.entities.filter(e=>e.name!=='Blocker');
save('local-unoccluded-on',clearLocal);
const clearLocalOff=structuredClone(clearLocal);clearLocalOff.lighting.localShadows=false;
save('local-unoccluded-off',clearLocalOff);
const minimumRange=structuredClone(clearLocal);minimumRange.entities.find(e=>e.kind==='light').light.range=0.001;
save('local-minimum-range',minimumRange);
const vol=structuredClone(local);vol.lighting.localShadows=false;vol.lighting.fogDensity=0.04;
save('volume-shadow-on',vol,{view:'scatter'});
const volOff=structuredClone(vol);volOff.entities.find(e=>e.kind==='light').light.castVolumetricShadow=false;
save('volume-shadow-off',volOff,{view:'scatter'});
const sun=base();sun.lighting.fogDensity=0.04;sun.lighting.keyIntensity=8;
save('sun-open',sun,{view:'scatter'});
const sunRoof=structuredClone(sun);sunRoof.entities.push(entity('Roof occluder','prop',[0,7,5],[8,1,12]));
save('sun-occluded',sunRoof,{view:'scatter'});
const ibl=base();ibl.lighting.skyColor=[0.2,0.3,0.5];ibl.lighting.ibl=true;
ibl.entities.push(entity('Metal','prop',[0,2,4],[2,2,2],[1,1,1],{material:'metal'}));
save('ibl-on',ibl);const iblOff=structuredClone(ibl);iblOff.lighting.ibl=false;save('ibl-off',iblOff);
const gi=base();gi.entities=gi.entities.filter(e=>e.name!=='Receiver');gi.lighting.keyDirection=[0.6,0.7,0.3];
gi.lighting.keyIntensity=3;gi.lighting.ddgi=true;
gi.entities.push(entity('Red bounce wall','wall',[-3,2,0],[0.5,5,10],[1,0.015,0.015]));
gi.entities.push(entity('White receiver','wall',[3,2,0],[0.5,5,10]));
save('ddgi-on',gi,{camera:'0,3,12,0,0,0',frames:360,shot:320});
const giOff=structuredClone(gi);giOff.lighting.ddgi=false;
save('ddgi-off',giOff,{camera:'0,3,12,0,0,0',frames:360,shot:320});
const cloud=base();Object.assign(cloud.lighting,{skyColor:[0.08,0.12,0.2],keyDirection:[0.2,0.75,-0.4],
  keyIntensity:4,cloudCoverage:0.65,cloudHeight:40,cloudThickness:25,cloudShadows:true});
save('cloud-on',cloud,{camera:'0,2,10,0,12,-10'});
const cloudOff=structuredClone(cloud);cloudOff.lighting.cloudShadows=false;
save('cloud-off',cloudOff,{camera:'0,2,10,0,12,-10'});
save('cloud-ground-on',cloud,{camera:'0,6,20,0,0,0',view:'cloud-shadow'});
save('cloud-ground-off',cloudOff,{camera:'0,6,20,0,0,0',view:'cloud-shadow'});
save('camera-cut',cloud,{camera:'0,2,10,0,12,-10',cameraCut:'80,10,5,10,0,12,-20',shot:81});
const noHistory=structuredClone(cloud);noHistory.lighting.temporal=false;
save('camera-cut-reference',noHistory,{camera:'0,2,10,0,12,-10',cameraCut:'80,10,5,10,0,12,-20',shot:81});
save('repeat',uniform,{view:'fog'});
save('town-balanced',town,{camera:'0,2.4,30,0,2.2,5',frames:180,shot:150,size:'1600x900'});
save('town-repeat',town,{camera:'0,2.4,30,0,2.2,5',frames:180,shot:150,size:'1600x900'});
const high=structuredClone(town);high.lighting.quality=2;
save('town-high',high,{camera:'0,2.4,30,0,2.2,5',frames:120,shot:100,size:'960x540'});
writeFileSync(`${directory}/cases.json`,JSON.stringify(cases,null,2)+'\n');
console.log(`Prepared ${cases.length} lighting acceptance captures.`);
