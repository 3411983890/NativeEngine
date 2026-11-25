import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const put = (path, data) => { const target=resolve(root,path); mkdirSync(dirname(target),{recursive:true}); writeFileSync(target,JSON.stringify(data,null,2)+'\n'); };
const field=(type,def,extra={})=>({type,default:def,...extra});
const str=def=>field('string',def,{maxLength:1024});
const int=(def,max=100000000)=>field('integer',def,{minimum:0,maximum:max});
const bool=def=>field('boolean',def);
const object=properties=>({type:'object',properties,required:[],additionalProperties:false});
const strings=()=>field('array',[],{items:str(''),maxItems:32});
const descriptor=(name,properties,state,update=false)=>put(`assets/scripts/fogtown/${name}.behavior.json`,{apiVersion:1,version:1,...(update?{update:true}:{}),properties:object(properties),state:object(state)});
descriptor('action',{quest:str(''),action:str('briefing'),value:int(0,3)},{});
descriptor('door',{actor:str(''),item:str(''),flag:str(''),message:str('门已开启。'),lockedMessage:str('暂时无法开启。')},{opened:bool(false)});
descriptor('plate',{actor:str(''),weights:strings()},{latched:bool(false)},true);
descriptor('light',{actor:str(''),flag:str('repaired')},{});
descriptor('quest',{actor:str(''),rubble:strings(),challengeTicks:int(5400,36000)},{stage:int(0,5),node:int(0,3),relay:int(0,3),deadline:int(0),running:bool(false),demolished:bool(false),cache:bool(false)});

const catalogPath=resolve(root,'assets/gameplay/components.json');
const catalog=JSON.parse(readFileSync(catalogPath,'utf8'));
catalog.objective={version:1,schema:object({title:str(''),detail:str(''),position:field('array',[0,0,0],{items:{type:'number',minimum:-100000,maximum:100000},minItems:3,maxItems:3}),step:int(1,100),total:int(5,100)})};
put('assets/gameplay/components.json',catalog);

const source=JSON.parse(readFileSync(resolve(root,'.screen/town.scene.json'),'utf8'));
const materials={};
for(const name of ['brick_red','brick_grey','plaster','roof_tile','wood','metal','cobble','stone','grass','water','lamp','awning','flat']) {
 materials[name]=source.materials[name]??{albedo:'flat',normal:'flat',orm:'flat',uvScale:1,roughness:1,metalness:1};
}
materials.dark_glass={albedo:'flat',normal:'flat',orm:'flat',uvScale:1,roughness:.16,metalness:.8};
const entities=[];const ids={};let serial=0;
const id=name=>ids[name]??= `f0900000-0000-4000-8000-${(++serial).toString(16).padStart(12,'0')}`;
const behavior=(script,properties={},instance='main')=>({instances:[{id:instance,script:`fogtown/${script}`,properties}]});
const box=(name,position,scale,material='stone',options={})=>{
 const {prompt,...entityOptions}=options;
 const e={id:id(name),name,kind:'prop',position,scale,color:[1,1,1],collider:false,material,...entityOptions};entities.push(e);return e;
};
const actor=id('player'),quest=id('quest');
const action=(name,position,actionName,value=0,options={})=>box(name,position,[.65,1.25,.55],'metal',{
 color:[.45,.62,.65],...options,components:{interactable:{range:2.6,prompt:options.prompt??name},behavior:behavior('action',{quest,action:actionName,value})}
});
const door=(name,position,scale,properties)=>box(name,position,scale,'metal',{kind:'door',collider:true,color:[.35,.48,.53],components:{interactable:{range:2.6,prompt:properties.item?'用钥匙打开 '+name:'检查 '+name},behavior:behavior('door',{actor,...properties})}});
const powered=(name,position,flag,color=[1,.66,.3],intensity=9,range=10)=>box(name,position,[.3,.3,.3],'lamp',{kind:'prop',color,components:{illumination:{enabled:false,color,intensity,range},behavior:behavior('light',{actor,flag})}});

box('雾镇地基',[0,-.3,-22],[40,.6,100],'grass',{kind:'ground',collider:true});
box('湿石主街',[0,.025,-22],[10,.05,86],'cobble');
for(const side of [-1,1]){
 box('人行道 '+side,[side*6,.08,-22],[2,.16,86],'stone');
 box('边界围墙 '+side,[side*19,2,-22],[.5,4,100],'brick_grey',{kind:'wall',collider:true});
}
box('镇口封闭护栏',[0,1.1,18],[38,2.2,.4],'metal',{collider:true});
box('塔后围墙',[0,2,-65],[38,4,.5],'brick_grey',{collider:true});
box('player',[0,0,12],[.6,1.8,.6],'flat',{kind:'player',components:{camera:{mode:'firstPerson',eyeHeight:1.64,fov:1.15,worldExtent:90}}});
box('quest',[0,-3,0],[.01,.01,.01],'flat',{components:{renderable:{enabled:false},behavior:behavior('quest',{actor,rubble:[id('塌方 西'),id('塌方 中'),id('塌方 东')]})}});

// Buildings are assembled from the engine's boxes, with cooked surface materials.
function building(name,x,z,w=8,d=8,h=6,wall='brick_red'){
 box(name+' 主体',[x,h/2,z],[w,h,d],wall,{kind:'wall',collider:true});
 box(name+' 台基',[x,.25,z],[w+.2,.5,d+.2],'stone');
 box(name+' 檐口',[x,h+.15,z],[w+.8,.3,d+.8],'stone');
 for(let tier=0;tier<4;tier++)box(name+' 屋顶 '+tier,[x,h+.4+tier*.32,z],[w+.6-tier*.9,.34,d+.6-tier*.9],'roof_tile');
 box(name+' 烟囱',[x+2,h+1,z-2],[.6,2,.65],'brick_grey');
 const face=x>0?x-w/2-.04:x+w/2+.04;
 for(let floor=0;floor<2;floor++)for(const offset of [-2.4,0,2.4]){
  box(name+' 窗框 '+floor+offset,[face,1.7+floor*2.5,z+offset],[.16,1.55,1.25],'wood');
  box(name+' 玻璃 '+floor+offset,[face+(x>0?-.06:.06),1.7+floor*2.5,z+offset],[.13,1.3,1.02],'dark_glass',{color:[.045,.09,.11]});
 }
}
for(const side of [-1,1])for(const z of [6,-16,-32,-43,-56]) {
 if(side<0&&z===6)continue;
 building((side<0?'西街':'东街')+z,side*12,z,8,8,z===-43?9:6.5,z%3===0?'plaster':'brick_red');
}
// The watch station and a block-built keeper have no separate character dependency.
box('值守亭 背墙',[0,1.7,5.8],[4,3.4,.25],'wood',{collider:true});
box('值守亭 顶棚',[0,3.5,7],[4.7,.25,3],'roof_tile');
for(const x of [-1.9,1.9])box('值守亭 立柱 '+x,[x,1.7,7],[.18,3.4,.18],'wood',{collider:true});
action('值守员',[0,1.1,8],'briefing',0,{scale:[.65,1.15,.4],material:'flat',color:[.12,.32,.36],prompt:'与值守员交谈'});
box('值守员 头部',[0,1.88,8],[.42,.42,.42],'flat',{color:[.76,.59,.43]});
box('值守员 帽檐',[0,2.12,8],[.54,.13,.48],'metal',{color:[.15,.24,.25]});
for(const x of [-.18,.18])box('值守员 腿 '+x,[x,.35,8],[.22,.7,.28],'flat',{color:[.09,.16,.2]});
box('值守亭灯',[0,3,7.6],[.3,.25,.3],'lamp',{kind:'light',light:{color:[1,.65,.3],intensity:8,range:9}});

// An enterable warehouse; its plate operates two independent door instances.
for(const x of [-11,-5])box('仓库侧墙 '+x,[x,1.9,-6.8],[.3,3.8,12.4],'brick_grey',{kind:'wall',collider:true});
box('仓库后墙',[-8,1.9,-13],[6,3.8,.3],'brick_grey',{kind:'wall',collider:true});
for(const x of [-10.2,-5.8])box('仓库前墙 '+x,[x,1.9,-1],[1.6,3.8,.3],'brick_grey',{kind:'wall',collider:true});
box('仓库门楣',[-8,3.3,-1],[2.8,1,.3],'brick_grey',{collider:true});
box('仓库屋面',[-8,4,-7],[6.5,.3,12.7],'roof_tile');
box('仓库地面',[-8,.02,-7],[6,.04,12],'stone');
door('仓库正门',[-8,1.4,-1],[2.8,2.8,.22],{item:'warehouse_key',message:'仓库已打开。把右侧木箱推到黄色压力板上。',lockedMessage:'仓库上锁了，先向镇口值守员领取钥匙。'});
door('后室闸门',[-8,1.4,-9.3],[2.5,2.8,.2],{flag:'plate',lockedMessage:'将木箱推上黄色压力板，两道闸门会同时打开。'});
door('侧窗闸门',[-10.2,1.4,-9.3],[1.4,2.8,.2],{flag:'plate',lockedMessage:'与后室闸门共用压力板。'});
box('后室隔墙',[-5.6,1.6,-9.3],[1.2,3.2,.3],'brick_grey',{collider:true});
const crate=box('仓库木箱',[-6.6,.6,-4.7],[1.1,1.2,1.1],'wood',{collider:true,physics:{mode:'dynamic',mass:9,friction:.5,restitution:0}});
box('黄色压力板',[-6.6,.045,-7.8],[1.65,.09,1.65],'metal',{color:[1,.66,.12],components:{behavior:behavior('plate',{actor,weights:[crate.id]})}});
action('维修工具箱',[-8,1,-11],'tool',0,{scale:[.8,.45,.5],color:[.95,.53,.12],prompt:'拿取维修工具'});
box('工具台',[-8,.6,-11],[1.8,.75,.8],'wood',{collider:true});
box('仓库工作灯',[-8,3.3,-6],[.3,.25,.3],'lamp',{kind:'light',light:{color:[.65,.78,1],intensity:5,range:8}});
action('备用钥匙',[-9.8,.8,-11],'cache',0,{kind:'key',scale:[.15,.3,.15],color:[.4,.8,1],prompt:'拾取备用钥匙'});
door('维修柜',[5.5,1,-37],[1.2,2,.6],{item:'service_key',message:'备用维修柜已开启。供能线索：水井、钟楼、风车。',lockedMessage:'这把锁需要仓库后室的备用钥匙。'});

// A controlled native blast removes the physical road obstruction.
for(const [label,x] of [['西',-3.2],['中',0],['东',3.2]])box('塌方 '+label,[x,.85,-23],[3.15,1.7,.65],'brick_grey',{
 collider:true,physics:{mode:'static',mass:45,friction:.65,restitution:.04},destructible:{enabled:true,strength:8,fragments:7,debrisLifetime:9,structural:false,seed:21}
});
for(const side of [-1,1])box('维修院横墙 '+side,[side*12,1.8,-23],[14,3.6,.45],'brick_grey',{collider:true});
action('受控爆破起爆器',[3.5,1,-20],'demolish',0,{color:[.78,.14,.08],prompt:'使用维修工具 · 受控爆破'});
action('供能发电机',[-3,1,-28],'repair',0,{scale:[1.5,1.8,.9],color:[.22,.48,.38],prompt:'修复供能发电机'});
for(let i=0;i<3;i++)box('发电机散热片 '+i,[-3,1.1,-27.52],[1.25,.12,.12],'metal');
box('发电机排气管',[-3.6,2.3,-28],[.18,1,.18],'metal');
powered('发电机状态灯',[-3,2.15,-28],'repaired',[.22,1,.53],6,8);

// The fountain, bell and wind vane make the order spatially legible.
box('广场铺装',[0,.04,-36],[14,.08,13],'stone');
for(const [value,x,z,name,color] of [[2,-3.6,-35,'水井',[.15,.55,1]],[1,0,-38,'钟楼',[1,.63,.17]],[3,3.6,-35,'风车',[.25,1,.55]]]){
 box(name+' 节点台座',[x,.25,z],[1.5,.5,1.3],'stone',{collider:true});
 action(name+' 供能节点',[x,1.1,z],'node',value,{color,prompt:'启动 '+name+' 节点'});
 powered(name+' 节点灯',[x,2,z],'node'+value,color,12,10);
 if(value===2){box('水井井沿',[x,.6,z-1.8],[2,.45,1.6],'stone');box('井水',[x,.84,z-1.8],[1.55,.04,1.25],'water');}
 if(value===1){box('钟楼支架',[x,3.5,z-1.2],[.3,7,.3],'metal');box('钟体',[x,5.9,z-1.2],[1.1,1.3,.7],'metal',{color:[.78,.53,.17]});}
 if(value===3){box('风车立柱',[x,2,z-1.6],[.2,4,.2],'wood');box('风车横叶',[x,3.8,z-1.6],[2.3,.18,.15],'wood');box('风车竖叶',[x,3.8,z-1.6],[.18,2.3,.15],'wood');}
}
for(const side of [-1,1])box('信号塔围栏 '+side,[side*11,1.5,-43],[16,3,.3],'metal',{collider:true});
door('信号塔闸门',[0,1.4,-43],[6,2.8,.25],{flag:'square',lockedMessage:'广场的三个供能节点全部接通后，信号塔自动解锁。'});
action('信号塔启动控制台',[0,1,-46],'start',0,{color:[.25,.66,.72],prompt:'启动 45 秒中继连接'});
action('西侧中继',[-4,1,-49],'relay',1,{prompt:'连接西侧中继',color:[1,.58,.2]});
action('东侧中继',[4,1,-52],'relay',2,{prompt:'连接东侧中继',color:[.15,.65,1]});
action('塔基发射终端',[0,1,-57],'relay',3,{prompt:'连接塔基 · 发出信号',color:[.3,.9,.6]});
for(const x of [-1.2,1.2])for(const z of [-58,-60])box('信号塔支柱 '+x+z,[x,7,z],[.25,14,.25],'metal',{collider:true});
for(const y of [3,6,9,12,14])box('信号塔横梁 '+y,[0,y,-59],[2.8,.2,2.5],'metal');
box('信号塔天线',[0,16,-59],[.16,5,.16],'metal');
box('信号塔天线横杆',[0,17,-59],[5,.12,.12],'metal');
powered('信号塔蓝色信标',[0,18,-59],'complete',[.2,.65,1],40,28);
powered('信号塔工作灯',[0,3,-47],'tower_running',[.2,.65,1],15,14);

for(const side of [-1,1])for(const z of [1,-13,-29,-39,-50]){
 const x=side*5.7;
 box('街灯立柱 '+x+z,[x,2.1,z],[.13,4.2,.13],'metal',{collider:true});
 box('街灯横臂 '+x+z,[x-side*.4,4.1,z],[.9,.1,.1],'metal');
 powered('街灯光源 '+x+z,[x-side*.8,4,z],z>-25?'repaired':z>-43?'square':'complete',[1,.7,.36],10,12);
}
// Small street props and continuous edge detail provide scale at walking height.
for(const side of [-1,1])for(const z of [-15,-31,-40]){
 box('长椅 '+side+z,[side*6,.65,z],[.6,.15,2.1],'wood',{collider:true});
 box('长椅椅背 '+side+z,[side*6.22,1,z],[.12,.7,2.1],'wood');
 for(const dz of [-.75,.75])box('椅腿 '+side+z+dz,[side*6,.3,z+dz],[.5,.6,.12],'metal');
}
for(const z of [11,1,-14,-26,-34,-45])for(const x of [-4.5,4.5])box('排水沟 '+x+z,[x,.064,z],[.35,.035,2.2],'metal',{color:[.2,.23,.25]});
box('停靠维修车',[5.2,.9,11],[1.9,1.3,3.8],'metal',{kind:'vehicle',color:[.3,.38,.4],collider:true,vehicle:{speed:0,direction:1}});
const scene={schemaVersion:2,projectId:'f0900000-0000-4000-8000-000000000000',name:'雾镇重启 · 第一人称冒险',revision:0,
 settings:{walkSpeed:3.5,runSpeed:6,jumpSpeed:6.5,gravity:20},materials,
 lighting:{ambient:[.13,.18,.23],skyColor:[.035,.065,.095],keyDirection:[-.35,.7,-.25],keyColor:[.55,.7,1],keyIntensity:.7,fogColor:[.19,.28,.34],fogDensity:.022,fogHeightFalloff:.18,fogBaseHeight:0,toneMap:true,exposure:1.2,cloudCoverage:.65,cloudHeight:90,cloudThickness:35,cloudSpeed:.012,quality:1,localShadows:true,ibl:true,ddgi:true,cloudShadows:true,temporal:true,maxShadowLights:4},entities};
put('.screen/fogtown.scene.json',scene);
put('samples/fogtown/project.json',{format:'nativegpu.sample/1',name:'雾镇重启',scene:'assets/fogtown.scene.json',view:'firstPerson',engineVersion:readFileSync(resolve(root,'VERSION'),'utf8').trim(),scripts:['action','door','plate','light','quest'].map(s=>`assets/scripts/fogtown/${s}.lua`),ids,entrypoints:{adventure:'npm run fogtown',tour:'npm run fogtown:tour',verify:'npm run fogtown:verify',package:'npm run fogtown:package'}});

// Short, original PCM cues. Audio is presentation only and never drives quest state.
for(const [name,notes] of Object.entries({power:[523,659,784],door:[330,440],fail:[330,247],blast:[75,52],complete:[523,659,784,1047,1319]})){
 const rate=22050,duration=name==='complete'?.22:.14,count=Math.floor(rate*duration*notes.length),buffer=Buffer.alloc(44+count*2);
 buffer.write('RIFF');buffer.writeUInt32LE(36+count*2,4);buffer.write('WAVEfmt ',8);buffer.writeUInt32LE(16,16);buffer.writeUInt16LE(1,20);buffer.writeUInt16LE(1,22);buffer.writeUInt32LE(rate,24);buffer.writeUInt32LE(rate*2,28);buffer.writeUInt16LE(2,32);buffer.writeUInt16LE(16,34);buffer.write('data',36);buffer.writeUInt32LE(count*2,40);
 for(let i=0;i<count;i++){const note=Math.min(notes.length-1,Math.floor(i/rate/duration)),t=(i/rate)%duration,envelope=Math.min(1,t/.012)*Math.exp(-t*10);buffer.writeInt16LE(Math.round(Math.sin(2*Math.PI*notes[note]*t)*envelope*6500),44+i*2);}
 const target=resolve(root,`assets/fogtown/audio/${name}.wav`);mkdirSync(dirname(target),{recursive:true});writeFileSync(target,buffer);
}
console.log(`Fogtown: ${entities.length} entities, five connected areas, deterministic content generated.`);
