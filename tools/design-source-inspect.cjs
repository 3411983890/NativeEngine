const fs = require('node:fs');
const source = fs.readFileSync('C:/Users/34119/Desktop/引擎编辑器 · 设计/index.html', 'utf8');
console.log('FRAMES', [...source.matchAll(/<div class="layer0"[^>]+/g)].map(x=>x[0]));
console.log('IMAGES', [...source.matchAll(/<img[^>]+/g)].map(x=>x[0].slice(0,300)));
console.log('CSS first frame', source.slice(source.indexOf('.layer0 {'), source.indexOf('.layer0 {')+300));
console.log('TEXT', [...source.matchAll(/<text\b[^>]*>([\s\S]*?)<\/text>/g)].map(x=>x[1].replace(/<[^>]*>/g,'')).slice(0,500));
console.log('ROOT DESIGNS', [...source.matchAll(/<div[^>]+data-name="([^"]+)"[^>]+data-page-offset="([^"]+)"[^>]*>/g)].filter(x=>/^E\d|场景|材质|动画|工作区|编辑器背景|顶栏|状态栏|资源|AI|视口/.test(x[1])).map(x=>`${x[1]} @ ${x[2]}`).slice(0,100));
console.log('ACTUAL TEXT', [...source.matchAll(/data-pagx-text="([^"]*)"/g)].map(x=>x[1]).slice(0,540));
console.log('IMAGES css', [...source.matchAll(/(?:url\([^)]*|href="[^"]*)assets[^\s<>]* /g)].map(x=>x[0].slice(0,100)).slice(0,20));
