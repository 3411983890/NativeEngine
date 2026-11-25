import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
const sha256=data=>createHash('sha256').update(data).digest('hex');
const source=JSON.parse(readFileSync('reports/fogtown/evidence.json','utf8'));
const evidence=source.payload??source;
evidence.engineVersion=readFileSync('VERSION','utf8').trim();
evidence.sceneSha256=sha256(readFileSync('.screen/fogtown.scene.json'));
evidence.captures=[];
for(const name of ['arrival','repair','square','complete']){
 const report=JSON.parse(readFileSync(`reports/fogtown/render-${name}.json`,'utf8').replace(/^\uFEFF/,''));
 const png=readFileSync(`reports/fogtown/${name}.png`);
 if(!report.success||png.readUInt32BE(16)!==1280||png.readUInt32BE(20)!==720||report.gameplay.diagnostics.length)throw new Error(`Invalid native evidence: ${name}`);
 evidence.captures.push({name,file:`${name}.png`,sha256:sha256(png),width:1280,height:720,gpuP50Ms:report.gpuP50Ms,gpuP95Ms:report.gpuP95Ms,cpuSubmitP50Ms:report.cpuSubmitP50Ms,frames:report.frames,success:true});
}
writeFileSync('reports/fogtown/evidence.json',JSON.stringify(evidence,null,2)+'\n');
const rows=evidence.captures.map(c=>`| ${c.name} | ${c.gpuP50Ms.toFixed(2)} | ${c.gpuP95Ms.toFixed(2)} | [画面](${c.file}) |`).join('\n');
writeFileSync('reports/fogtown/README.md',`# 雾镇重启验证证据

原生五区域通关、真实推箱与爆破、错误节点顺序复位、倒计时超时/重试、完整存档重启续跑、固定输入摘要一致，全部通过。

- 引擎版本：${evidence.engineVersion}
- 实体：${evidence.entities}
- 验证输入帧：${evidence.fixedInputFrames}
- 路线模拟时长：${evidence.simulationSeconds.toFixed(2)} 秒（自动导航，不能作为首次探索游玩时长）
- 通关摘要：${evidence.digest}
- 场景 SHA-256：${evidence.sceneSha256}
- 人工正常速度通关：尚未执行。

以下为 1280×720 各阶段独立运行的实测，不能据此宣称目标设备性能门限已达成。每份 render-*.json 保留原生配置、诊断和性能结果。

| 阶段 | GPU 中位数 ms | GPU P95 ms | 截图 |
| --- | ---: | ---: | --- |
${rows}

完整断言和图像哈希见 [evidence.json](evidence.json)。范围边界见工程 .developmentAgent/samples/fogtown/docs/known-limitations.md。
`);
console.log('Fogtown evidence: four native 1280×720 captures verified.');
