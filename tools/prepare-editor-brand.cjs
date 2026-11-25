const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
async function prepare() {
 const root=path.resolve(__dirname,'..'), out=path.join(root,'editor/ui/media');fs.mkdirSync(out,{recursive:true});
 const brand='C:/Users/34119/Documents/ChatGPT/GameDevelopmentSkill/output/nativeengine-icons/svg-balanced';
 for(const extension of ['svg','png'])fs.copyFileSync(path.join(brand,'NativeEngine-transparent.'+extension),path.join(out,'NativeEngine.'+extension));
 const png=await sharp(path.join(out,'NativeEngine.png')).resize(256,256).png().toBuffer();
 const header=Buffer.alloc(22);header.writeUInt16LE(1,2);header.writeUInt16LE(1,4);header.writeUInt16LE(1,10);header.writeUInt16LE(32,12);header.writeUInt32LE(png.length,14);header.writeUInt32LE(22,18);
 fs.writeFileSync(path.join(out,'NativeEngine.ico'),Buffer.concat([header,png]));
 const source='C:/Users/34119/Desktop/引擎编辑器 · 设计/assets';
 for(const [from,to]of [['img78.png','scene-reference.png'],['img124.png','stone-base.png'],['img126.png','stone-normal.png'],['img128.png','stone-orm.png']])fs.copyFileSync(path.join(source,from),path.join(out,to));
 console.log('NativeEngine brand and supplied reference assets prepared.');
}
prepare().catch(error=>{console.error(error);process.exitCode=1;});
