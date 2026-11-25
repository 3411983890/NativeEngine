// Deterministic data-texture packing of image_gen-authored source maps.
// Does not paint features, synthesize geometry, or modify the original images.
import {readFileSync,writeFileSync,mkdirSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {join,dirname} from 'node:path';
import {createHash} from 'node:crypto';
import {decodePng,encodePng} from '../../../../tools/textures.mjs';

const root=dirname(fileURLToPath(import.meta.url));
const out=join(root,'complete');mkdirSync(out,{recursive:true});
const files=[];
const read=name=>decodePng(readFileSync(join(root,name)),name);
const save=(name,image,semantic,extra={})=>{
  const bytes=encodePng(image);writeFileSync(join(out,name),bytes);
  files.push({file:name,width:image.width,height:image.height,semantic,
    colorSpace:semantic==='baseColor'?'srgb':'linear',
    sha256:createHash('sha256').update(bytes).digest('hex'),...extra});
};
const sameSize=(a,b)=>{if(a.width!==b.width||a.height!==b.height)throw Error('Source dimensions differ: registration requires explicit authoring repair');};
const gray=(image,values)=>{
  const data=Buffer.alloc(image.width*image.height*4);
  for(let p=0;p<values.length;p++){data[p*4]=data[p*4+1]=data[p*4+2]=values[p];data[p*4+3]=255;}
  return {width:image.width,height:image.height,data};
};

for(const part of ['bark','leaves']){
  const leaf=part==='leaves', prefix=`oak_${part}`;
  const albedo=read(`${prefix}_basecolor${leaf?'_alpha':''}_prototype.png`);
  const rawOrm=read(`raw/${prefix}_orm.png`), rawNormal=read(`raw/${prefix}_normal_dx.png`);
  sameSize(albedo,rawOrm);sameSize(albedo,rawNormal);
  save(`${prefix}_basecolor${leaf?'_alpha':''}.png`,albedo,'baseColor', {alpha:leaf?'original generated coverage':'opaque'});
  const count=albedo.width*albedo.height;
  const ao=new Uint8Array(count),roughness=new Uint8Array(count),metalness=new Uint8Array(count);
  const normal=Buffer.alloc(count*4),orm=Buffer.alloc(count*4);
  let maxNormalLengthError=0;
  for(let p=0;p<count;p++){
    const i=p*4;
    // Authoring ranges, not measurements: bark 0.75–0.95, leaves 0.35–0.75.
    ao[p]=leaf?Math.round(217+rawOrm.data[i]*38/255):rawOrm.data[i];
    roughness[p]=Math.round((leaf?0.35:0.75)*255+rawOrm.data[i+1]*(leaf?0.40:0.20));
    orm[i]=ao[p];orm[i+1]=roughness[p];orm[i+2]=0;orm[i+3]=255;
    let x=rawNormal.data[i]/127.5-1,y=rawNormal.data[i+1]/127.5-1;
    let z=Math.max(rawNormal.data[i+2]/127.5-1,0.001);
    if(leaf&&albedo.data[i+3]===0){x=0;y=0;z=1;}
    const length=Math.hypot(x,y,z);
    normal[i]=Math.round((x/length+1)*127.5);normal[i+1]=Math.round((y/length+1)*127.5);
    normal[i+2]=Math.round((z/length+1)*127.5);normal[i+3]=255;
    const encodedLength=Math.hypot(normal[i]/127.5-1,normal[i+1]/127.5-1,normal[i+2]/127.5-1);
    maxNormalLengthError=Math.max(maxNormalLengthError,Math.abs(encodedLength-1));
  }
  save(`${prefix}_normal_dx.png`,{...albedo,data:normal},'normal',{convention:'DirectX -Y requested from image_gen; sign must be confirmed in renderer preview',maxNormalLengthError});
  save(`${prefix}_orm.png`,{...albedo,data:orm},'orm',{channels:{R:'ambient occlusion',G:'roughness',B:'metalness = 0',A:'1'}});
  save(`${prefix}_ao.png`,gray(albedo,ao),'occlusion');
  save(`${prefix}_roughness.png`,gray(albedo,roughness),'roughness');
  save(`${prefix}_metallic.png`,gray(albedo,metalness),'metallic');
  const scalar=read(`raw/${prefix}_${leaf?'thickness':'height'}.png`);sameSize(albedo,scalar);
  const values=new Uint8Array(count),coverage=new Uint8Array(count);
  for(let p=0;p<count;p++){
    const i=p*4;coverage[p]=albedo.data[i+3];
    values[p]=leaf&&coverage[p]===0?0:Math.round((scalar.data[i]+scalar.data[i+1]+scalar.data[i+2])/3);
  }
  save(`${prefix}_${leaf?'thickness':'height'}.png`,gray(albedo,values),leaf?'thickness':'height',
    {encoding:leaf?'white = relatively thick, black = thin/empty; not calibrated metres':'white = raised, black = recessed; not calibrated metres'});
  if(leaf)save(`${prefix}_opacity.png`,gray(albedo,coverage),'opacity',{encoding:'exact source alpha, white = covered'});
}
const manifest={version:1,status:'complete-authoring-prototype-not-engine-validated',date:'2026-10-25',
  generation:'built-in image_gen; original prompts in ../generation.json',
  preparation:'deterministic channel packing, scalar extraction, roughness range mapping, vector normalization; no repainting or UV warping',
  warnings:['AI-inferred data are not physical measurements or geometry bakes.',
    'Exact cross-map feature registration, tile-edge continuity, normal Y sign and visual quality require material preview.',
    'Original leaf alpha is preserved, including fractional coverage; choose MASK cutoff in preview.',
    'No mips, runtime cooking, compression, engine material assignment or render validation performed.',
    'Thickness shader hookup is planned; these source files do not implement that feature.',
    'Whole-tree impostors and voxel representations require a finished 3D tree and are not part of the leaf/bark source set.'],files};
writeFileSync(join(out,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
console.log(JSON.stringify({files:files.length,dimensions:[...new Set(files.map(f=>`${f.width}x${f.height}`))],output:out},null,2));
