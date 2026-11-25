const fs = require('node:fs');
const sharp = require('sharp');
(async()=>{
 const base='C:/Users/34119/Desktop/引擎编辑器 · 设计/assets';
 const paths=fs.readdirSync(base).filter(p=>p.endsWith('.png'));
 const images=[];
 for(let i=0;i<paths.length;i++){
  const metadata=await sharp(base+'/'+paths[i]).metadata(); console.log(paths[i],metadata.width,metadata.height);
  const tile=await sharp(base+'/'+paths[i]).resize(400,240,{fit:'inside'}).extend({top:0,bottom:0,left:0,right:0}).png().toBuffer();
  images.push({input:tile,left:(i%3)*420,top:Math.floor(i/3)*270});
 }
 fs.mkdirSync('reports/editor-design',{recursive:true});
 await sharp({create:{width:1260,height:Math.ceil(paths.length/3)*270,channels:4,background:'#303030'}}).composite(images).png().toFile('reports/editor-design/source-assets.png');
 const s=fs.readFileSync('C:/Users/34119/Desktop/引擎编辑器 · 设计/index.html','utf8');
 const start=s.indexOf('data-name="01 ·');
 console.log('start',start);
 const texts=[...s.matchAll(/data-pagx-text="([^"]*)"/g)].map(x=>x[1]); console.log(texts.slice(80,240).join('\n'));
})();
