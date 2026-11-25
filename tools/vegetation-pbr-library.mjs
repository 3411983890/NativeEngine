// Production authoring inventory and deterministic PBR packing; image creation uses image_gen.
import { readFileSync, writeFileSync, mkdirSync, existsSync, renameSync } from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { decodePng, encodePng } from './textures.mjs';
import { normalizeLeafSource } from '../engine/vegetation/leaf-source.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const library = join(root, 'assets/source/vegetation/qinglan-pbr-v1');
const catalogFile = join(library, 'catalog.json');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const json = file => JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
function saveJson(file, value) { mkdirSync(dirname(file), { recursive: true }); const tmp = `${file}.tmp`; writeFileSync(tmp, `${JSON.stringify(value, null, 2)}\n`); renameSync(tmp, file); }

// part-name:type; labels refer to the species row, not generic substitutes.
const blueprint = {
 T01:'leaf_mature:leaf,bark:tile,twig_cluster:cluster',
 T02:'leaf_mature:leaf,leaf_young:leaf,bark:tile',
 T03:'leaf_mature:leaf,leaf_autumn_red:leaf,leaf_autumn_yellow:leaf,bark:tile',
 T04:'leaf_mature:leaf,bark:tile',
 T05:'leaf_mature:leaf,leaf_lobed:leaf,bark:tile,mulberry_skin:surface,mulberry_fruit:part',
 T06:'leaflet:leaf,compound_leaf:frond,bark:tile,flower:flower,fruit_skin:surface',
 T07:'leaflet:leaf,compound_leaf:frond,fruit_catkin:seedHead,bark:tile',
 T08:'leaf_mature:leaf,bark:tile,persimmon_skin:surface,calyx:flower',
 T09:'leaf_mature:leaf,leaf_back:leaf,bark:tile,loquat_skin:surface,fruit_cluster:part',
 T10:'leaf_mature:leaf,leaf_autumn:leaf,bark:tile,short_shoot:cluster',
 T11:'leaf_mature:leaf,bark:tile,flower:flower,petal:leaf,peach_skin:surface',
 T12:'leaf_mature:leaf,bark_old:tile,flower:flower,petal:leaf',
 T13:'bark_dead:tile,bark_rotten:tile,wood_break:surface,leaf_remnant:leaf',
 B01:'leaf_mature:leaf,leaf_cluster:cluster,culm_node:culm,culm_plain:tile',
 B02:'leaf_mature:leaf,leaf_cluster:cluster,culm_node:culm,culm_young:tile',
 B03:'leaf_mature:leaf,flower:flower,petal:leaf,twig_bark:tile',
 B04:'leaf_mature:leaf,flower:flower,petal:leaf,twig_bark:tile',
 B05:'leaf_mature:leaf,flower:flower,petal:leaf,twig_bark:tile',
 B06:'leaf_mature:leaf,twig_bark:tile,fruit_skin:surface',
 B07:'leaflet:leaf,compound_leaf:frond,flower:flower,petal:leaf,vine_bark:tile',
 B08:'leaf_mature:leaf,twig_bark:tile,goji_skin:surface,fruit:part',
 B09:'leaflet:leaf,leaflet_autumn:leaf,compound_leaf:frond,berry_skin:surface,fruit_cluster:part,stem:tile',
 B10:'twig_dead:tile,wood_break:surface,leaf_remnant:leaf',
 G01:'blade:blade,grass_clump:cluster,stolon:tile',
 G02:'blade:blade,seed_head_green:seedHead,seed_head_dry:seedHead,stem:tile',
 G03:'blade:blade,blade_dry:blade,white_plume:seedHead',
 G04:'blade:blade,flower_head:seedHead,stem:tile',
 G05:'blade:blade,blade_dry:blade,plume:seedHead',
 G06:'blade:blade,digitate_seed_head:seedHead,stem:tile',
 G07:'leaf_mature:leaf,flower:flower,stem:tile',
 G08:'leaflet:leaf,trifoliate_leaf:frond,flower:flower',
 G09:'rosette_leaf:leaf,yellow_flower:flower,seed_ball:seedHead,stem:tile',
 G10:'leaf_mature:leaf,leaf_back:leaf,branch_cluster:cluster',
 G11:'rosette_leaf:leaf,stem_leaf:leaf,flower_head:seedHead,seed_pods:seedHead',
 G12:'leaf_mature:leaf,flower:flower,fleshy_stem:tile',
 G13:'frond:frond,pinna:leaf,young_fiddlehead:part',
 G14:'@G01',G15:'@G01,@G04',
 G16:'small_frond:frond,pinna:leaf',
 W01:'blade:blade,blade_dry:blade,reed_plume:seedHead,culm:culm',
 W02:'blade:blade,cattail_head:seedHead,stem:tile',
 W03:'sword_leaf:blade,flower_head:seedHead',
 W04:'round_stem:tile,flower_head:seedHead',
 W05:'broad_blade:blade,flower_head:seedHead,stem_base:surface',
 W06:'single_frond:floatingLeaf,duckweed_cluster:cluster,root:part',
 W07:'leaf_green:floatingLeaf,leaf_back:floatingLeaf,leaf_dry:floatingLeaf,petal:leaf,flower:flower,bud:part,seedpod:part,stem:tile,stem_dry:tile',
 W08:'leaf_top:floatingLeaf,leaf_back:floatingLeaf,flower:flower,fruit_skin:surface',
 W09:'leaflet:leaf,compound_leaf:frond,flower_head:seedHead,stem:tile',
 W10:'round_stem:tile,flower_head:seedHead',
 C01:'blade_green:blade,blade_yellow:blade,blade_dry:blade,panicle_green:seedHead,panicle_ripe:seedHead,panicle_dry:seedHead,culm_green:culm,culm_dry:culm,stubble_cut:surface',
 C02:'basal_leaf:leaf,stem_leaf:leaf,yellow_flower:flower,seed_pod:part,stem:tile',
 C03:'leaf_top:floatingLeaf,leaf_back:floatingLeaf,petiole:tile',
 C04:'tubular_leaf:tile,leaf_sheath:surface,flower_head:seedHead',
 C05:'outer_leaf:leaf,inner_leaf:leaf,leaf_back:leaf',
 C06:'basal_leaf:leaf,radish_skin:surface,petiole:tile',
 C07:'leaf_mature:leaf,flower:flower,vine:tile,pumpkin_skin_young:surface,pumpkin_skin_ripe:surface,stem:tile',
 C08:'leaflet:leaf,trifoliate_leaf:frond,flower:flower,pod_green:part,pod_ripe:part,stem:tile',
 V01:'leaf_mature:leaf,leaf_autumn:leaf,vine:tile',
 V02:'leaf_mature:leaf,vine_bark:tile,grape_skin:surface,tendril:part',
 V03:'leaf_mature:leaf,flower:flower,vine:tile',
 V04:'vine_dead:tile,wood_break:surface,leaf_remnant:leaf',
 V05:'moss_dry:coverage,moss_wet:coverage,moss_tuft:cluster',
 V06:'algae_film:coverage,algae_film_wet:coverage,filament_cluster:cluster',
};
const references = {
 T01:'Salix babylonica, one slender lanceolate mature weeping willow leaf with finely serrated margin',
 T02:'Cinnamomum camphora, camphor tree leaf with characteristic basal major veins',
 T03:'Triadica sebifera, Chinese tallow leaf with rhombic outline and acuminate tip',
 T04:'Celtis sinensis, Chinese hackberry, ovate serrated leaf with asymmetrical base',
 T05:'Morus alba, white mulberry, natural lobed and unlobed leaf forms',
 T06:'Melia azedarach, chinaberry, pinnate compound leaf and serrated leaflets',
 T07:'Pterocarya stenoptera, Chinese wingnut, pinnate compound leaves and hanging winged fruit catkins',
 T08:'Diospyros kaki, persimmon',T09:'Eriobotrya japonica, loquat, large strongly veined leathery serrated leaf',
 T10:'Ginkgo biloba, fan shaped ginkgo leaf',T11:'Prunus persica, peach',T12:'Prunus mume, plum blossom',
 T13:'old dead broadleaf tree, weathered wood',B01:'Phyllostachys edulis, moso bamboo',
 B02:'Pleioblastus fortunei green-leaf artistic reference, small understory bamboo; species choice is an authoring reference not botanical verification',
 B03:'Camellia japonica',B04:'Gardenia jasminoides',B05:'Hibiscus syriacus',B06:'Ligustrum sinense',
 B07:'Rosa multiflora',B08:'Lycium chinense',B09:'Nandina domestica',B10:'dead small woody shrub',
 G01:'Cynodon dactylon',G02:'Setaria viridis',G03:'Imperata cylindrica',G04:'Cyperus rotundus as selected sedge reference',
 G05:'Miscanthus sinensis',G06:'Digitaria sanguinalis',G07:'Commelina communis',G08:'Oxalis corniculata',
 G09:'Taraxacum officinale artistic dandelion reference',G10:'Artemisia argyi',G11:'Capsella bursa-pastoris',
 G12:'Portulaca oleracea',G13:'Dryopteris erythrosora as selected fern reference',G16:'Pteris multifida as selected small wall fern reference',
 W01:'Phragmites australis',W02:'Typha orientalis',W03:'Acorus calamus',W04:'Schoenoplectus tabernaemontani',
 W05:'Zizania latifolia',W06:'Lemna minor as selected duckweed reference',W07:'Nelumbo nucifera',W08:'Euryale ferox',
 W09:'Oenanthe javanica',W10:'Juncus effusus',C01:'Oryza sativa',C02:'Brassica napus',C03:'Colocasia esculenta',
 C04:'Allium fistulosum as selected green onion reference',C05:'Brassica rapa subsp. pekinensis, heading napa cabbage reference',
 C06:'Raphanus sativus, white daikon reference',C07:'Cucurbita moschata as selected pumpkin reference',C08:'Glycine max',
 V01:'Parthenocissus tricuspidata as selected creeper reference',V02:'Vitis amurensis as selected wild grape reference',
 V03:'Trachelospermum jasminoides',V04:'dead woody climbing vine',V05:'short green pleurocarpous moss, artistic reference',
 V06:'green surface biofilm and filamentous green algae, separate artistic material types',
};
const thin = new Set(['leaf','blade','frond','cluster','flower','seedHead','floatingLeaf','part']);
const dimensions = {T01:[0.12,0.024],T02:[0.08,0.045],T10:[0.06,0.08]};
const mapNames = ['basecolor','normal_dx','orm','height'];

function initialize() {
 if (existsSync(catalogFile)) throw Error('Catalog already exists; retain generation state rather than overwrite.');
 const table = readFileSync(join(root,'.developmentAgent/vegetation/VEGETATION-ASSET-REQUIREMENTS-2026-10-28.md'),'utf8');
 const rows = [...table.matchAll(/^\| ([TBGWCV]\d{2}) \| ([^|]+) \|/gm)];
 if(rows.length!==63) throw Error(`Expected 63 species rows, found ${rows.length}`);
 const species=[],assets=[];
 for(const row of rows) {
  const id=row[1], recipe=blueprint[id];if(!recipe)throw Error(`Missing blueprint ${id}`);
  const entry={id,name:row[2].trim(),reference:references[id]??null,assets:[],reuses:[],status:'pending'};
  for(const token of recipe.split(',')) {
   if(token.startsWith('@')) {entry.reuses.push(token.slice(1));continue;}
   const [part,type]=token.split(':');
   // User priority: one authored texture per part first; extra shape variants are deferred.
   const count=1;
   for(let variant=1;variant<=count;variant++) {
    const assetId=`${id}_${part}${variant===1?'':`_v0${variant}`}`;
    entry.assets.push(assetId);
    assets.push({assetId,speciesId:id,name:entry.name,reference:entry.reference,part,type,variant,
     imagegenStatus:'pending',numericStatus:'pending',visualRegistration:'pending',engineVerified:false,
     requiredRawMaps:[...mapNames,...(thin.has(type)?['thickness']:[])],
     uv:{set:0,origin:'bottomLeft',u:'right',v:'up',domain:[0,0,1,1],
      sampler:thin.has(type)?'clamp':'wrap',pivot:['flower','floatingLeaf'].includes(type)?[0.5,0.5]:thin.has(type)?[0.5,0.0625]:null},
     sizeMetres:dimensions[id]??null,sizeProvenance:'art-direction values, not measured; null requires assignment',
     needsModelUV:type==='surface',normalConvention:'directX-',source:'built-in image_gen; AI inferred not measured'});
   }
  }
  species.push(entry);
 }
 saveJson(catalogFile,{schema:'qinglan.pbr-authoring-catalog/1',date:'2026-10-28',goalStatus:'active',
  assetStandard:'.developmentAgent/vegetation/VEGETATION-LEAF-GENERATION-STANDARD-v1.txt',species,assets});
 return status();
}

function status() {
 const c=json(catalogFile);const rawCounts={};let numeric=0,visual=0,complete=0;
 const required=c.assets.filter(a=>a.requiredForGoal!==false);
 for(const a of required) {
  for(const map of a.requiredRawMaps) if(existsSync(join(library,'raw',a.assetId,`${map}.png`)))rawCounts[map]=(rawCounts[map]??0)+1;
  const file=join(library,'complete',a.assetId,'asset.json');
  if(existsSync(file)) { const m=json(file);if(m.validation.numericPass)numeric++;if(m.validation.visualRegistration==='passed')visual++;if(m.validation.numericPass&&m.validation.visualRegistration==='passed')complete++; }
 }
 const isMvp=c.currentPriority?.phase==='texture-mvp';
 const priority=(isMvp?c.currentPriority.assets:(c.productionPlan?.firstPassAssets??[])).map(id=>required.find(a=>a.assetId===id)).filter(Boolean);
 const work=isMvp?priority:[...priority,...required];
 const nextBase=work.find(a=>!existsSync(join(library,'raw',a.assetId,'basecolor.png')));
 const next=nextBase??work.find(a=>a.requiredRawMaps.some(map=>!existsSync(join(library,'raw',a.assetId,`${map}.png`))));
 return {species:c.species.length,textureSets:required.length,deferredVariants:c.assets.length-required.length,
  firstPassAssets:priority.length,firstPassBasecolors:priority.filter(a=>existsSync(join(library,'raw',a.assetId,'basecolor.png'))).length,
  rawMaps:rawCounts,numericPass:numeric,visualPass:visual,completeSets:complete,
  goalComplete:complete===required.length,priorityPhase:c.currentPriority?.phase??'full-library',nextAsset:next?.assetId??null,nextMap:nextBase?'basecolor':null};
}

function coveragePlan() {
 const c=json(catalogFile),firstPass=[];
 for(const a of c.assets){a.requiredForGoal=a.variant===1;if(a.variant>1)a.priority='deferred-extra-variant';}
 for(const s of c.species) {
  const owned=c.assets.filter(a=>a.speciesId===s.id&&a.variant===1);
  const leaf=owned.find(a=>['leaf','blade','frond','floatingLeaf'].includes(a.type))??owned.find(a=>['coverage','cluster'].includes(a.type))??owned[0];
  const bark=owned.find(a=>/bark|twig_dead|vine_dead|^vine$|^culm_node$/.test(a.part));
  for(const a of [leaf,bark])if(a&&!firstPass.includes(a.assetId))firstPass.push(a.assetId);
 }
 c.productionPlan={updated:'2026-10-28',userPriority:'One representative leaf and applicable bark per species first; cover all species before extra variants or PBR companion passes.',
  firstPassAssets:firstPass,phaseOrder:['representative-leaf-and-bark-basecolors','remaining-component-and-state-basecolors','registered-pbr-companions','qa-and-pack'],
  extraVariants:'deferred; existing files retained; not a completion requirement'};
 saveJson(catalogFile,c);return status();
}

function prepare(assetId) {
 const c=json(catalogFile),a=c.assets.find(x=>x.assetId===assetId);if(!a)throw Error('Unknown asset');
 // Floating leaves use a reviewed internal socket, never an inferred bottom contour.
 if(!['leaf','blade','floatingLeaf'].includes(a.type))throw Error(`No verified packing adapter for ${a.type}; do not apply a single-leaf transform blindly`);
 if(a.type==='floatingLeaf'&&!a.sourceRootUV)throw Error('Floating leaf requires a visually reviewed sourceRootUV for its internal petiole socket');
 const raw=join(library,'raw',assetId), out=join(library,'complete',assetId);
 const source=Object.fromEntries(a.requiredRawMaps.map(name=>[name,decodePng(readFileSync(join(raw,`${name}.png`)))]));
 const base=source.basecolor;
 for(const [name,img] of Object.entries(source))if(img.width!==base.width||img.height!==base.height)throw Error(`Source size mismatch for ${name}; re-edit with image_gen`);
 const opacity={...base,data:Buffer.alloc(base.data.length)};
 let minY=base.height,minX=base.width,maxY=-1,maxX=-1;
 for(let p=0;p<base.width*base.height;p++) {
  const i=p*4,alpha=base.data[i+3];opacity.data[i]=opacity.data[i+1]=opacity.data[i+2]=alpha;opacity.data[i+3]=255;
  if(alpha>=128){const x=p%base.width,y=Math.floor(p/base.width);minX=Math.min(minX,x);maxX=Math.max(maxX,x);minY=Math.min(minY,y);maxY=Math.max(maxY,y);}
 }
 if(maxY<0)throw Error('No valid alpha content');
 // Lowest alpha row is the end of the basal connection for upright single-leaf sources. Multi-subject/edge guards remain in the normalizer.
 let rootX=0,rootCount=0;
 for(let x=minX;x<=maxX;x++)if(base.data[(maxY*base.width+x)*4+3]>=128){rootX+=x+0.5;rootCount++;}
 rootX/=rootCount;
 if(!a.sourceRootUV&&Math.abs(rootX-base.width/2)>base.width*0.12)throw Error('Lowest contour is not a reliable central attachment; use image_gen to repair the base/petiole or supply a reviewed type-specific layout. Do not attach a basal lobe as the root.');
 const metadata={schema:'nativegpu.leaf-texture-source/1',assetKind:'leaf',leafProfile:a.leafProfile??'bladeOnly',speciesId:a.speciesId,
  imageSize:[base.width,base.height],coordinates:{origin:'bottomLeft',u:'right',v:'up'},rootPivotUV:a.type==='floatingLeaf'?[0.5,0.5]:[0.5,0.0625],tipDirectionUV:[0,1],
  safeMarginFraction:0.0625,baseDamageState:'healthy',normalConvention:'directX-',alphaCutoff:0.5,
  landmarks:{rootUV:a.sourceRootUV??[rootX/base.width,1-(maxY+0.5)/base.height]},
  ...(a.sizeMetres?{bladeLengthMetres:a.sizeMetres[0],bladeWidthMetres:a.sizeMetres[1]}:{}),
  maps:Object.fromEntries(['baseColor','normal','orm','opacity','thickness'].map(key=>[key,{source:key==='baseColor'?'basecolor.png':key==='normal'?'normal_dx.png':`${key}.png`}])),
  provenance:{source:'built-in image_gen',sizeProvenance:a.sizeProvenance,rootLandmark:a.sourceRootUV?'reviewed explicit socket':'detected lowest alpha row; single upright source visually inspected'}};
 const convert=img=>({width:img.width,height:img.height,pixels:img.data});
 const result=normalizeLeafSource({maps:{baseColor:convert(base),normal:convert(source.normal_dx),orm:convert(source.orm),opacity:convert(opacity),thickness:convert(source.thickness)},metadata});
 const heightResult=normalizeLeafSource({maps:{baseColor:convert(base),thickness:convert(source.height)},metadata});
 const n=result.maps.normal,orm=result.maps.orm,height=heightResult.maps.thickness,thickness=result.maps.thickness,b=result.maps.baseColor;
 const count=b.width*b.height;
 const channels=Object.fromEntries(['ao','roughness','metallic','opacity','uv-check'].map(key=>[key,Buffer.alloc(count*4)]));
 let maxNormalError=0;
 for(let p=0;p<count;p++) {
  const i=p*4,alpha=b.pixels[i+3];
  let x=n.pixels[i]/127.5-1,y=n.pixels[i+1]/127.5-1,z=Math.max(0.001,n.pixels[i+2]/127.5-1);
  if(alpha===0){x=0;y=0;z=1;orm.pixels[i]=255;orm.pixels[i+1]=200;height.pixels[i]=height.pixels[i+1]=height.pixels[i+2]=0;thickness.pixels[i]=thickness.pixels[i+1]=thickness.pixels[i+2]=0;}
  const length=Math.hypot(x,y,z);
  n.pixels[i]=Math.round((x/length+1)*127.5);n.pixels[i+1]=Math.round((y/length+1)*127.5);n.pixels[i+2]=Math.round((z/length+1)*127.5);n.pixels[i+3]=255;
  maxNormalError=Math.max(maxNormalError,Math.abs(Math.hypot(n.pixels[i]/127.5-1,n.pixels[i+1]/127.5-1,n.pixels[i+2]/127.5-1)-1));
  orm.pixels[i+2]=0;orm.pixels[i+3]=255;
  for(const m of [height,thickness]){const gray=Math.round((m.pixels[i]+m.pixels[i+1]+m.pixels[i+2])/3);m.pixels[i]=m.pixels[i+1]=m.pixels[i+2]=gray;m.pixels[i+3]=255;}
  for(const [key,value] of Object.entries({ao:orm.pixels[i],roughness:orm.pixels[i+1],metallic:0,opacity:alpha})) {const d=channels[key];d[i]=d[i+1]=d[i+2]=value;d[i+3]=255;}
  const d=channels['uv-check'];d[i]=Math.round((p%b.width+0.5)/b.width*255);d[i+1]=Math.round((1-(Math.floor(p/b.width)+0.5)/b.height)*255);d[i+2]=0;d[i+3]=alpha;
 }
 // These are mechanical consistency checks, never a substitute for visual feature registration.
 const all={basecolor:b,normal_dx:n,orm,height,thickness,...Object.fromEntries(Object.entries(channels).map(([key,pixels])=>[key,{width:b.width,height:b.height,pixels}]))};
 mkdirSync(out,{recursive:true});
 const files=[];
 for(const [key,img] of Object.entries(all)) {const bytes=encodePng({width:img.width,height:img.height,data:Buffer.from(img.pixels)});writeFileSync(join(out,`${key}.png`),bytes);files.push({file:`${key}.png`,sha256:sha(bytes),width:img.width,height:img.height,colorSpace:key==='basecolor'?'srgb':'linear'});}
 const generation=existsSync(join(raw,'generation.json'))?json(join(raw,'generation.json')):null;
 const rawFiles=a.requiredRawMaps.map(map=>({file:`../../raw/${assetId}/${map}.png`,sha256:sha(readFileSync(join(raw,`${map}.png`)))}));
 const manifest={schema:'qinglan.pbr-asset/1',assetId,speciesId:a.speciesId,part:a.part,variant:a.variant,source:'built-in image_gen',
  pbrData:'AI-inferred art data, not physical measurement or model bake',files,rawFiles,generation,
  uv:{...a.uv,diagnostic:'uv-check.png',modelUnwrap:false,attachmentKind:a.type==='floatingLeaf'?'internal-petiole-socket':'basal-root'},normalConvention:'directX-',ormChannels:{R:'ambientOcclusion',G:'roughness',B:'metallic=0'},
  sizeMetres:a.sizeMetres,sizeProvenance:a.sizeProvenance,canonical:result.metadata,
  validation:{numericPass:maxNormalError<0.015,rootErrorPixels:result.metadata.validation.rootErrorPixels,maxNormalLengthError:maxNormalError,
   sharedTransform:true,opacityEqualsBaseAlpha:true,metallicZero:true,visualRegistration:'pending',normalYRendererCheck:'pending',engineVerified:false},
  warnings:['Shared transform and dimensions do not prove AI feature registration.','Visual silhouette and vein alignment must pass before this set is complete.','No runtime cook, mip or engine material assignment performed.']};
 saveJson(join(out,'asset.json'),manifest);
 a.imagegenStatus='raw-maps-generated';a.numericStatus=manifest.validation.numericPass?'passed':'failed';saveJson(catalogFile,c);
 return {assetId,files:files.length,output:out,validation:manifest.validation};
}

const [command,assetId]=process.argv.slice(2);
let result;
if(command==='init')result=initialize();else if(command==='status')result=status();else if(command==='coverage-plan')result=coveragePlan();else if(command==='prepare')result=prepare(assetId);else throw Error('Usage: node tools/vegetation-pbr-library.mjs init|status|coverage-plan|prepare <assetId>');
console.log(JSON.stringify(result,null,2));
