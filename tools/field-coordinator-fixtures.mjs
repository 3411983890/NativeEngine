import fs from 'node:fs';
import { compileEffect } from '../engine/effects/asset.mjs';
import './field-services-fixtures.mjs';
const directory='build/field-coordinator-gpu';fs.mkdirSync(directory,{recursive:true});
const joint=JSON.parse(fs.readFileSync('build/fields-services-gpu/fixture.effect.json','utf8'));
const particle=structuredClone(joint);particle.id='coordinator_particle';delete particle.smoke;
const smoke=structuredClone(joint);smoke.id='coordinator_smoke';delete smoke.field;for(const emitter of smoke.emitters)emitter.modules=emitter.modules.filter(module=>module.type!=='fieldDeposit');
for(const [name,asset] of Object.entries({joint,particle,smoke})){fs.writeFileSync(directory+'/'+name+'.effect.json',JSON.stringify(asset,null,2)+'\n');fs.writeFileSync(directory+'/'+name+'.cooked.json',JSON.stringify(compileEffect(asset))+'\n');}
