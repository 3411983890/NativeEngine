import {inflateSync} from 'node:zlib';
import {requireAsset} from './gltf-document.mjs';
import {decodeContainerTexture,isContainerTexture} from './texture-decode.mjs';

import {ENVIRONMENT_HEADER,ENVIRONMENT_LIMIT,ENVIRONMENT_RADIANCE_MAX,environmentDimensions as dimensions,inspectEnvironment} from './environment-container.mjs';
export {ENVIRONMENT_HEADER,ENVIRONMENT_LIMIT,ENVIRONMENT_RADIANCE_MAX,inspectEnvironment};
const bounds=(bytes,at,n)=>requireAsset(Number.isSafeInteger(at)&&Number.isSafeInteger(n)&&at>=0&&n>=0&&at+n<=bytes.length,'Truncated environment source');

// Canonical pixels are linear Rec.709 RGB, row zero at +Y. Longitude zero is +X:
// u=.5+atan2(z,x)/(2*pi), v=acos(y)/pi. Alpha is not coverage for a sky panorama.
export function decodeRgbe(bytes){
  let at=0;const line=()=>{const end=bytes.indexOf(10,at);requireAsset(end>=at&&end-at<=65536&&end<1024*1024,'Invalid RGBE header');const s=bytes.subarray(at,end).toString('ascii').replace(/\r$/,'');at=end+1;return s;};
  requireAsset(/^#\?(RADIANCE|RGBE)$/.test(line()),'Invalid RGBE signature');let format,primaries,gamma=1;
  for(let row=line();row!=='';row=line()){
    if(row.startsWith('FORMAT='))format=row.slice(7).trim();
    if(row.startsWith('GAMMA='))gamma=Number(row.slice(6));
    if(row.startsWith('PRIMARIES='))primaries=row.slice(10).trim().split(/\s+/).map(Number);
  }
  requireAsset(['32-bit_rle_rgbe','32-bit_rle_xyze'].includes(format),'Unsupported Radiance pixel format','UNSUPPORTED');requireAsset(gamma===1,'Nonlinear RGBE gamma requires an explicit color conversion','UNSUPPORTED');
  const axis=/^([+-])([XY])\s+(\d+)\s+([+-])([XY])\s+(\d+)$/.exec(line().trim());requireAsset(axis&&axis[2]!==axis[5],'Invalid RGBE orientation');
  const rows=Number(axis[3]),columns=Number(axis[6]),width=axis[2]==='X'?rows:columns,height=axis[2]==='Y'?rows:columns;dimensions(width,height);
  const data=new Float32Array(width*height*4),scan=new Uint8Array(columns*4);
  for(let major=0;major<rows;major++){
    bounds(bytes,at,4);
    if(columns>=8&&columns<=32767&&bytes[at]===2&&bytes[at+1]===2&&!(bytes[at+2]&128)){
      requireAsset((bytes[at+2]*256+bytes[at+3])===columns,'Invalid RGBE scanline width');at+=4;
      for(let c=0;c<4;c++){let x=0;while(x<columns){bounds(bytes,at,1);const code=bytes[at++];requireAsset(code!==0,'Invalid RGBE run');if(code>128){const n=code-128;requireAsset(x+n<=columns,'RGBE run exceeds scanline');bounds(bytes,at,1);const v=bytes[at++];for(let i=0;i<n;i++)scan[(x++)*4+c]=v;}else{requireAsset(x+code<=columns,'RGBE literal exceeds scanline');bounds(bytes,at,code);for(let i=0;i<code;i++)scan[(x++)*4+c]=bytes[at++];}}}
    }else{
      let x=0,shift=0;while(x<columns){bounds(bytes,at,4);const rgba=bytes.subarray(at,at+4);at+=4;if(rgba[0]===1&&rgba[1]===1&&rgba[2]===1){requireAsset(x>0&&shift<=24,'Invalid legacy RGBE run');const count=rgba[3]*2**shift;requireAsset(x+count<=columns,'Legacy RGBE run exceeds scanline');for(let i=0;i<count;i++){scan.copyWithin(x*4,(x-1)*4,x*4);x++;}shift+=8;}else{scan.set(rgba,x*4);x++;shift=0;}}
    }
    for(let minor=0;minor<columns;minor++){
      const coords={};coords[axis[2]]=axis[1]==='+'?major:rows-1-major;coords[axis[5]]=axis[4]==='+'?minor:columns-1-minor;
      // Radiance +Y runs upward; our row zero is north.
      const p=((height-1-coords.Y)*width+coords.X)*4,s=minor*4,scale=scan[s+3]?2**(scan[s+3]-136):0;
      for(let c=0;c<3;c++)data[p+c]=scan[s+c]*scale;data[p+3]=1;
    }
  }
  requireAsset(at===bytes.length,'Trailing RGBE image data');return {width,height,data,primaries,xyz:format==='32-bit_rle_xyze',sourceFormat:'RGBE'};
}

// Inspect all attributes and chunk ranges before the third-party EXR decoder can
// allocate its output. Multipart/deep buffers need a separate layer-selection UI.
export function inspectExr(bytes){
  bounds(bytes,0,9);requireAsset(bytes.readUInt32LE(0)===20000630&&(bytes.readUInt32LE(4)&255)===2,'Invalid EXR signature/version');
  const flags=bytes.readUInt32LE(4)>>>8;requireAsset((flags&~6)===0,'Deep/multipart EXR requires a layer adapter','UNSUPPORTED');let at=8;const attrs=new Map();
  const string=()=>{const end=bytes.indexOf(0,at);requireAsset(end>=at&&end-at<=255&&end<1024*1024,'Invalid EXR attribute');const s=bytes.subarray(at,end).toString('utf8');at=end+1;return s;};
  while(bytes[at]!==0){const name=string(),type=string();bounds(bytes,at,4);const size=bytes.readUInt32LE(at);at+=4;bounds(bytes,at,size);requireAsset(!attrs.has(name)&&at+size<=1024*1024,'Invalid EXR header');attrs.set(name,{type,bytes:bytes.subarray(at,at+size)});at+=size;}at++;
  const dw=attrs.get('dataWindow');requireAsset(dw?.type==='box2i'&&dw.bytes.length===16,'Missing EXR data window');const [x0,y0,x1,y1]=[0,4,8,12].map(i=>dw.bytes.readInt32LE(i)),width=x1-x0+1,height=y1-y0+1;dimensions(width,height);
  const channels=attrs.get('channels');requireAsset(channels?.type==='chlist','Missing EXR channels');let pos=0,stride=0;const names=[];
  while(pos<channels.bytes.length&&channels.bytes[pos]!==0){const end=channels.bytes.indexOf(0,pos);requireAsset(end>pos&&end-pos<=255,'Invalid EXR channel name');const name=channels.bytes.subarray(pos,end).toString('utf8');pos=end+1;bounds(channels.bytes,pos,16);const type=channels.bytes.readUInt32LE(pos);requireAsset([1,2].includes(type)&&channels.bytes.readUInt32LE(pos+8)===1&&channels.bytes.readUInt32LE(pos+12)===1,'EXR channels must be full-resolution HALF/FLOAT','UNSUPPORTED');requireAsset(!names.includes(name)&&names.length<16,'Invalid EXR channels');names.push(name);stride+=type===1?2:4;pos+=16;}
  requireAsset(pos===channels.bytes.length-1&&((['R','G','B'].every(n=>names.includes(n)))||names.includes('Y')),'EXR requires RGB or luminance channels','UNSUPPORTED');requireAsset(width*height*stride<=ENVIRONMENT_LIMIT,'EXR channel budget exceeded','BUDGET');
  const compression=attrs.get('compression');requireAsset(compression?.bytes.length===1,'Missing EXR compression');const mode=compression.bytes[0],blockRows=[1,1,1,16,32,16,32,32,32,256][mode];requireAsset(blockRows,'Unsupported EXR compression','UNSUPPORTED');
  let chunks=Math.ceil(height/blockRows),tileW=width,tileH=blockRows;
  if(flags&2){const tile=attrs.get('tiles');requireAsset(tile?.type==='tiledesc'&&tile.bytes.length===9,'Invalid EXR tiles');tileW=tile.bytes.readUInt32LE(0);tileH=tile.bytes.readUInt32LE(4);requireAsset(tileW>0&&tileH>0&&tileW<=16384&&tileH<=16384&&(tile.bytes[8]&15)===0,'EXR environment tiles must have one level','UNSUPPORTED');chunks=Math.ceil(width/tileW)*Math.ceil(height/tileH);}
  bounds(bytes,at,chunks*8);const tableEnd=at+chunks*8,ranges=[],coordinates=new Set();
  for(let i=0;i<chunks;i++){
    const n=bytes.readBigUInt64LE(at+i*8);requireAsset(n<=BigInt(Number.MAX_SAFE_INTEGER),'Invalid EXR chunk offset');const offset=Number(n),header=flags&2?20:8;requireAsset(offset>=tableEnd,'EXR chunk overlaps its header');bounds(bytes,offset,header);const size=bytes.readUInt32LE(offset+header-4);requireAsset(size>0,'Empty EXR chunk');bounds(bytes,offset+header,size);ranges.push([offset,offset+header+size]);
    let rows=Math.min(blockRows,height-(bytes.readInt32LE(offset)-y0)),columns=width;
    let key;if(flags&2){const tx=bytes.readInt32LE(offset),ty=bytes.readInt32LE(offset+4);requireAsset(tx>=0&&ty>=0&&tx<Math.ceil(width/tileW)&&ty<Math.ceil(height/tileH)&&bytes.readInt32LE(offset+8)===0&&bytes.readInt32LE(offset+12)===0,'Invalid EXR tile coordinate');rows=Math.min(tileH,height-ty*tileH);columns=Math.min(tileW,width-tx*tileW);key=tx+','+ty;}else{key=bytes.readInt32LE(offset)-y0;requireAsset(rows>0&&rows<=blockRows&&key>=0&&key%blockRows===0,'Invalid EXR scanline');}requireAsset(!coordinates.has(key),'Duplicated EXR chunk coordinate');coordinates.add(key);
    const decoded=rows*columns*stride;requireAsset(size<=decoded,'EXR chunk exceeds its decoded budget','BUDGET');if([2,3].includes(mode)&&size!==decoded){let result;try{result=inflateSync(bytes.subarray(offset+header,offset+header+size),{maxOutputLength:decoded});}catch{requireAsset(false,'Invalid EXR ZIP payload');}requireAsset(result.length===decoded,'Invalid EXR ZIP size');}
    if(mode===0)requireAsset(size===decoded,'Truncated uncompressed EXR chunk');
  }
  ranges.sort((a,b)=>a[0]-b[0]);for(let i=1;i<ranges.length;i++)requireAsset(ranges[i][0]>=ranges[i-1][1],'Overlapping EXR chunks');
  const chroma=attrs.get('chromaticities');requireAsset(!chroma||chroma.type==='chromaticities'&&chroma.bytes.length===32,'Invalid EXR chromaticities');return {width,height,primaries:chroma?Array.from({length:8},(_,i)=>chroma.bytes.readFloatLE(i*4)):undefined};
}

const mul=(a,b)=>a.map(row=>b[0].map((_,j)=>row.reduce((sum,v,k)=>sum+v*b[k][j],0)));
const inverse=m=>{const [a,b,c]=m[0],[d,e,f]=m[1],[g,h,i]=m[2],det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);requireAsset(Number.isFinite(det)&&Math.abs(det)>1e-12,'Invalid environment primaries');return [[e*i-f*h,c*h-b*i,b*f-c*e],[f*g-d*i,a*i-c*g,c*d-a*f],[d*h-e*g,b*g-a*h,a*e-b*d]].map(row=>row.map(v=>v/det));};
const xyzToSrgb=[[3.2404542,-1.5371385,-.4985314],[-.969266,1.8760108,.041556],[.0556434,-.2040259,1.0572252]],rec709=[.64,.33,.30,.60,.15,.06,.3127,.3290];
// KHR Data Format 1.4 colorPrimaries values. Transfer must already be linear;
// ACEScc here names AP1 chromaticities, not the logarithmic ACEScc encoding.
const ktxPrimaries=new Map([
  [1,rec709],
  [4,[.708,.292,.170,.797,.131,.046,.3127,.3290]],
  [6,[.7347,.2653,0,1,.0001,-.077,.32168,.33767]],
  [7,[.713,.293,.165,.830,.128,.044,.32168,.33767]],
  [10,[.680,.320,.265,.690,.150,.060,.3127,.3290]],
  [11,[.640,.330,.210,.710,.150,.060,.3127,.3290]]
]);
function colorMatrix(image){
  if(image.xyz)return xyzToSrgb;
  const p=image.primaries;if(!p)return null;requireAsset(p.length===8&&p.every(v=>Number.isFinite(v)&&Math.abs(v)<=2)&&[1,3,5,7].every(i=>Math.abs(p[i])>1e-8)&&p[6]>0&&p[7]>0,'Invalid environment chromaticities');if(p.every((v,i)=>Math.abs(v-rec709[i])<.00001))return null;
  const xyz=(x,y)=>[x/y,1,(1-x-y)/y],columns=[0,2,4].map(i=>xyz(p[i],p[i+1])),m=[0,1,2].map(k=>columns.map(c=>c[k])),white=xyz(p[6],p[7]),scale=mul(inverse(m),white.map(v=>[v])).flat(),rgb=m.map(row=>row.map((v,i)=>v*scale[i]));
  const bradford=[[.8951,.2664,-.1614],[-.7502,1.7135,.0367],[.0389,-.0685,1.0296]],source=mul(bradford,white.map(v=>[v])).flat(),target=mul(bradford,xyz(.3127,.3290).map(v=>[v])).flat();requireAsset(source.every(v=>v>0),'Invalid environment white point');
  const adaptation=mul(inverse(bradford),bradford.map((row,i)=>row.map(v=>v*target[i]/source[i])));return mul(xyzToSrgb,mul(adaptation,rgb));
}
export async function decodeEnvironment(bytes,{extension}={}){
  requireAsset(bytes.length<=ENVIRONMENT_LIMIT,'Environment source exceeds 256 MiB','BUDGET');let image;
  if(['.hdr','.rgbe'].includes(extension))image=decodeRgbe(bytes);
  else if(extension==='.exr'){
    const metadata=inspectExr(bytes),[{EXRLoader},{FloatType}]=await Promise.all([import('three/examples/jsm/loaders/EXRLoader.js'),import('three')]);let parsed;try{parsed=new EXRLoader().setDataType(FloatType).parse(bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength));}catch(error){requireAsset(false,'Invalid EXR payload: '+error.message);}
    requireAsset(parsed.width===metadata.width&&parsed.height===metadata.height&&parsed.data.length===parsed.width*parsed.height*4,'EXR dimensions/channels differ');const data=new Float32Array(parsed.data.length);for(let y=0;y<parsed.height;y++)data.set(parsed.data.subarray((parsed.height-1-y)*parsed.width*4,(parsed.height-y)*parsed.width*4),y*parsed.width*4);image={...metadata,data,sourceFormat:'EXR'};
  }else if(isContainerTexture(bytes)){const decoded=await decodeContainerTexture(bytes,{allowHdr:true,semantic:'albedo'});requireAsset(decoded.hdr,'Environment container must contain linear floating-point pixels','UNSUPPORTED');image={...decoded,data:new Float32Array(decoded.data.buffer.slice(decoded.data.byteOffset,decoded.data.byteOffset+decoded.data.byteLength))};}
  else requireAsset(false,'Environment expects RGBE, EXR or a floating-point DDS/KTX2 panorama','UNSUPPORTED');
  const warnings=[];
  if(image.sourceFormat==='KTX2'){
    requireAsset(image.colorPrimaries===0||image.colorPrimaries===5||ktxPrimaries.has(image.colorPrimaries),'Unsupported KTX2 environment color primaries','UNSUPPORTED');
    if(image.colorPrimaries===0)warnings.push('KTX2 environment primaries are unspecified; linear Rec.709 is assumed.');
    else if(image.colorPrimaries===5)image.xyz=true;
    else image.primaries=ktxPrimaries.get(image.colorPrimaries);
  }
  dimensions(image.width,image.height);const matrix=colorMatrix(image),data=image.data;let negatives=0,alpha=0;
  for(let p=0;p<data.length;p+=4){const rgb=Array.from(data.subarray(p,p+3));requireAsset(rgb.every(Number.isFinite)&&Number.isFinite(data[p+3]),'Nonfinite environment sample');for(let c=0;c<3;c++){let v=matrix?matrix[c].reduce((sum,k,i)=>sum+k*rgb[i],0):rgb[c];requireAsset(Number.isFinite(v)&&v<=ENVIRONMENT_RADIANCE_MAX,'Environment radiance exceeds 1e12','BUDGET');if(v<0){v=0;negatives++;}data[p+c]=v;}if(data[p+3]!==1)alpha++;data[p+3]=1;}
  return {width:image.width,height:image.height,data,sourceFormat:image.sourceFormat,warnings:[...warnings,...(negatives?[`${negatives} negative environment radiance channels were clamped to zero.`]:[]),...(alpha?['Environment alpha is ignored; the panorama represents incident radiance.']:[])]};
}

// Exact SH integration of each piecewise-constant latlong texel, including its
// spherical solid angle. Band convolution produces diffuse irradiance, not radiance.
export function integrateEnvironmentSH({width,height,data}){
  const sh=Array.from({length:9},()=>[0,0,0]);
  for(let y=0;y<height;y++){
    const a=y*Math.PI/height,b=(y+1)*Math.PI/height,ca=Math.cos(a),cb=Math.cos(b),sa=Math.sin(a),sb=Math.sin(b),s1=ca-cb,s2=(b-a)/2-(Math.sin(2*b)-Math.sin(2*a))/4,s3=ca-cb+(cb**3-ca**3)/3,cs2=(sb**3-sa**3)/3,c2s=(ca**3-cb**3)/3;
    for(let x=0;x<width;x++){
      const l=(x/width-.5)*2*Math.PI,r=((x+1)/width-.5)*2*Math.PI,d=r-l,c=Math.sin(r)-Math.sin(l),s=Math.cos(l)-Math.cos(r),ss=d/2-(Math.sin(2*r)-Math.sin(2*l))/4,cc=d-ss,sc=(Math.sin(r)**2-Math.sin(l)**2)/2,omega=s1*d;
      const basis=[.282095*omega,.488603*(sb*sb-sa*sa)*d/2,.488603*s2*s,.488603*s2*c,1.092548*cs2*c,1.092548*cs2*s,.315392*(3*s3*ss-omega),1.092548*s3*sc,.546274*(s3*cc-c2s*d)],p=(y*width+x)*4;
      for(let i=0;i<9;i++)for(let channel=0;channel<3;channel++)sh[i][channel]+=data[p+channel]*basis[i]*(i===0?Math.PI:i<4?2*Math.PI/3:Math.PI/4);
    }
  }return sh;
}
export function resizeEnvironment(image,size){
  const width=Math.min(size,image.width),height=width/2;requireAsset(Number.isInteger(height)&&height>0,'Invalid environment size');if(width===image.width)return image;
  const data=new Float32Array(width*height*4),sx=image.width/width,sy=image.height/height;
  for(let y=0;y<height;y++)for(let x=0;x<width;x++){const sums=[0,0,0];let weight=0;for(let iy=Math.floor(y*sy);iy<Math.ceil((y+1)*sy);iy++)for(let ix=Math.floor(x*sx);ix<Math.ceil((x+1)*sx);ix++){const w=(Math.min((x+1)*sx,ix+1)-Math.max(x*sx,ix))*(Math.cos(Math.max(y*sy,iy)*Math.PI/image.height)-Math.cos(Math.min((y+1)*sy,iy+1)*Math.PI/image.height));for(let c=0;c<3;c++)sums[c]+=image.data[(iy*image.width+ix)*4+c]*w;weight+=w;}const p=(y*width+x)*4;for(let c=0;c<3;c++)data[p+c]=sums[c]/weight;data[p+3]=1;}
  return {...image,width,height,data};
}
export function encodeEnvironment(image,diffuseSH=integrateEnvironmentSH(image)){
  dimensions(image.width,image.height);const bytes=Buffer.alloc(ENVIRONMENT_HEADER+image.width*image.height*16);bytes.write('NGE1');[1,image.width,image.height,4,ENVIRONMENT_HEADER,image.width*image.height*16,0].forEach((v,i)=>bytes.writeUInt32LE(v,4+i*4));diffuseSH.flat().forEach((v,i)=>bytes.writeFloatLE(v,32+i*4));image.data.forEach((v,i)=>bytes.writeFloatLE(v,ENVIRONMENT_HEADER+i*4));inspectEnvironment(bytes);return bytes;
}
export function environmentPreview(image){const data=Buffer.alloc(image.width*image.height*4);for(let p=0;p<data.length;p+=4){for(let c=0;c<3;c++){const value=image.data[p+c]/(1+image.data[p+c]),srgb=value<=.0031308?12.92*value:1.055*value**(1/2.4)-.055;data[p+c]=Math.round(srgb*255);}data[p+3]=255;}return {width:image.width,height:image.height,data};}
