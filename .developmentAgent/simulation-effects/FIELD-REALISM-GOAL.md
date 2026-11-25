# 场系统真实感 Goal（2026-10-21 设立）

> 状态：**四批改进已落地并通过验收（2026-10-21）**。本文件是目标与验收标准，
> 结果见「验收」一节与 `reports/campfire-night-evidence.json`。
> 上游文档：[FIELD-PLAN.md](FIELD-PLAN.md)（架构不变，本 goal 只提升真实感与视觉质量）。

## 目标

让整个场系统**以及后续新建的每一个粒子效果**默认达到物理可信的视觉效果，
真实感来自系统属性而不是单效果手调：

1. **光学真实**：黑体辐射（非手工调色）、相位函数（逆光发亮）、多重散射补偿（浓烟不发灰）、
   发光与阳光解耦（夜里火不变暗）、火从内部照亮烟。
2. **运动真实**：MacCormack 二阶平流保留细节、curl-noise 湍流让火舌撕裂、
   高斯核 splat 消除单点沉积的颗粒感。
3. **采样真实**：ray march 起点逐像素抖动 + 半分辨率 + 时域累积 + 深度感知上采样，
   消除分层条带（banding）与颗粒噪声。
4. **上下文真实**：火光作为动态点光源照亮场景并参与体积雾散射（复用光照系统），
   爆炸冲击同步增强光照。
5. **制度保障**：新效果通过渲染器插件自动继承以上全部能力；
   每个效果必须过三视角（顺光/逆光/侧光）截图验收。

## 范围与边界

- 改动集中在 `shaders/particles.hlsl`、`shaders/fluid.hlsl`、`renderer/particles.cpp`、
  `renderer/d3d12_renderer_internal.h`、`renderer/d3d12_renderer.cpp`（仅 RTV 堆 +1 槽）、
  `renderer/rhi.hpp` / `rhi_d3d12.cpp`（新开关透传）、`host/main.cpp`（火光联动 + CLI）。
- 不改求解器根签名的堆索引模式，不改描述符前 13 槽布局（新增槽只追加）。
- **确定性口径不变**：高斯 splat 的权重只依赖粒子位置，整数原子加保持顺序无关；
  MacCormack / 湍流 / 时域累积属于非确定性视觉通道，文档与报告分字段标注。
- 回退开关：`--particle-temporal off` 退回全分辨率直绘（着色模型不变）。

## 明确不做（本轮）

- 场景阴影贴图进入体积步进（需要世界空间阴影采样，留给 P3 余下部分）。
- 屏幕空间热浪折射（需要场景颜色拷贝，留给后续）。
- 稀疏砖块场（P2，重写 12 个 pass 的寻址，另排期）。
- 燃烧化学模型（P6）。

## 验收（全部由真实运行产生）

结果回填于 2026-10-21，证据文件 `reports/campfire-night-evidence.json`（21/21 通过）、
`reports/particles-evidence.json`（21/21）、`reports/campfire-evidence.json`（17/17）。

| 项 | 标准 | 实测 | 证据 |
|---|---|---|---|
| 既有门禁 | `node --test`、原生测试、particles.ps1、campfire.ps1 全绿 | `node --test` 148/148；原生 28+13+… 0 failed；particles 21/21；campfire 17/17 | 各自输出 |
| 图像回归 | courtyard 基线不因本批改动漂移 | meanAbsError 0、changedPercent 0、maxChannelDelta 0、SSIM 1.0 | `.incrementalTesting/rendering/image-regression.ps1` |
| 新着色 | 报告 `shading=blackbody+hg+multiscatter+fireglow`、确定性 splat 核 | `shading=blackbody+hg+multiscatter+fireglow`、`splatKernel=trilinear2x2x2`、`splat=integer-atomic` | particles-evidence.json |
| 发射模型 | 黑体色相与亮度解耦 | `emissionModel=planck-chroma+heat-radiance` | 同上 |
| MacCormack | 报告 `macCormack=true`，solverPasses 每帧 +2 | `macCormack=true`，solverPasses 2460（20 迭代） | 同上 |
| 湍流 | curl-noise 开启 | `turbulence=1.6` | 同上 |
| 时域 | `temporal.active=true`、`accumulatedFrames>0`；回退可用 | active=true、82 帧、半分辨率 382x344；`off` 时 requested=false/active=false/0 帧且图像不同 | campfire-night-evidence.json |
| 火光联动 | 火驱动场景光，强度随帧闪烁、爆炸增强 | `fireLight.applied=true`、intensity 5.02、flicker 0.836；爆炸帧 burstPeak 1.0 时增强 | campfire-night-evidence.json / campfire-evidence.json |
| 三视角 | 顺光/逆光/侧光三张互不相同 | front `F116C088B536`、back `7E095E393E89`、side `28F5B742EB15` | campfire-night-evidence.json |
| 爆炸联动 | 爆炸帧光照增强 + 焦化仍触发 | scorched=14、scorchApplied=true | campfire-evidence.json |

### 本轮修正的两个根因

1. **黑体发射不可见**。原实现取 `pow(blackbody(T), 0.35)`，而 1400 K 处 Planck 比值只有
   6500 K 参考的 1e-6，任何幂次都抬不到可见亮度，冷焰区等于全黑。改为**色相与亮度解耦**：
   `blackbody(T)` 归一化到单位亮度只负责色相，亮度由 `0.12 + 2.4·heat²` 给出。于是焰心
   3200 K 暖白、焰身 1900 K 橙、焰梢 1200 K 暗红，色温梯度按物理走。
2. **夜景无法变暗**。legacy 光栅着色器把主光项硬编码成 `albedo * keyDiffuse * 0.95`，
   完全忽略 `lighting.keyIntensity`，所以夜里地面仍是白昼亮度。把旧的 padding 槽位改成
   `keyTint = keyColor * (keyIntensity / 1.30)` 并乘进主光项：默认环境（白色、1.30）下
   乘数为 1.0，courtyard 基线字节不变；夜景 0.09 时主光降到 7%，火才成为唯一主光源。


## 验收场景

`.screen/campfire-night.scene.json`：夜间林间篝火，体积雾开启、质量光照开启、
火光为唯一点光源，验证"火照亮场景 + 火光照雾 + 黑体火焰 + 湍流火舌"的完整链路。
