# 植被系统：缺陷分析与改进方案（v1）

- 日期：2026-10-26。
- 依据：`renderer/vegetation_draw.*`、`shaders/vegetation.hlsl`、`engine/vegetation/*`、`engine/native/vegetation/*`、`build/vegetation/**` 实际产物，以及 [VEGETATION-HAIR-STATUS.md](VEGETATION-HAIR-STATUS.md) v3 的实测数据。
- 口径：每条缺陷都给出**可在代码或产物里指认的证据**。区分三种确定性：
  - **实测**：有数字，来自 `reports/vegetation-hair/evidence.json` 或本次重新量测。
  - **代码可推**：从代码结构直接推出，不需运行。
  - **待验证**：合理怀疑但没有测量，标注出来，不当结论用。
- 本文不修改计划的验收口径，只回答两个问题：**现在缺什么**、**按什么顺序补**。

---

## 0. 结论（四句话）

1. **植被绘制模块没有被任何渲染器代码引用**——`grep VegetationDraw` 在 `renderer/` 下只命中它自己。目前引擎里没有一帧画过植被，所有像素证据都来自一个自带 rig 的独立测试程序。这是最大的缺陷，且它不是"画质问题"。
2. **一帧只能画一页、一个物种**：常量缓冲是单块 256 B，`writeConstants()` 每次绘制前 `memcpy` 进去，同一条命令列表里的第二次绘制会让第一次绘制读到第二次的值。多页绘制在当前结构下不是"慢"，是**错**。
3. **LOD 只有数据，没有执行**：`lodBias` 写进了 32 B 实例记录但着色器只解码前 4 个 DWORD；`selectLod()` 只被 `tests/native_vegetation.cpp` 调用。5,223 实例的区域包在运行时无法按级绘制，只能整株 LOD0（70,008 tri × 5,223 = **3.66 亿三角形**）。
4. **LOD 的误差度量对"删叶子"是盲的**：LOD1/LOD2 的实现是随机丢弃 38%/72% 的叶片卡片，而误差定义为"稠密顶点到粗网格的最近距离"——删掉一张叶子卡不会移动任何剩余顶点，所以 oak 的 LOD1 误差只有 0.085 m。用它算屏幕误差阈值，会在冠形已经明显变薄时仍判"无需升 LOD"。

---

## 1. 缺陷总览

| 编号 | 缺陷 | 确定性 | 影响 |
| --- | --- | --- | --- |
| **A1** | 绘制模块未接入帧图 | 代码可推 | 引擎里看不到植被 |
| **A2** | 单常量块 → 单页单物种 | 代码可推 | 区域包无法整帧绘制 |
| **A3** | 无每实例 LOD 执行路径 | 代码可推 | 千株级场景三角形爆炸 |
| **A4** | LOD 误差度量对叶片删除不敏感 | 代码可推 + 实测 | LOD 阈值失效、切换过早 |
| **A5** | impostor 已烘焙未绘制 | 代码可推 | 远景无终极降级档 |
| **A6** | 统计量是"提交量"不是"绘制量" | 代码可推 | 剔除效果无法验收 |
| **B1** | 叶片法线切空间错误 | 代码可推 | 法线贴图方向随机 |
| **B2** | 叶片是 4 顶点平面、单一法线 | 代码可推 | 叶片无曲率、光照发平 |
| **B3** | 无 AO，`depthTint` 是高度函数 | 代码可推 | 冠内不透气、体积感缺失 |
| **B4** | 风是绝对振幅 + 单频正弦 | 代码可推 | 草与大树摆幅相同、无阵风 |
| **B5** | 阴影单级 2048²、caster 复用相机列表 | 代码可推 + 实测 | 阴影精度与覆盖双重不足 |
| **B6** | 无 MSAA、无 TAA、无速度 | 代码可推 | 近景叶片闪烁 |
| **B7** | alpha test + 无深度预 pass | 代码可推 | early-Z 失效、overdraw 全价 |
| **B8** | 材质分类靠数组下标 | 代码可推 | 草叶走树皮分支 |
| **C1** | 顶点 48 B、物种包 13.06 MB | 实测 | 显存/带宽浪费 4× |
| **C2** | 贴图 RGBA8、8.4 MB，无 BC | 实测 | 显存浪费 4–8× |
| **C3** | 单株 31,488 张叶片卡片 | 实测 | 顶点预算失控 |
| **C4** | 编辑器预览 2.77 MB JSON 往返 | 实测 | 交互延迟 273 ms 起 |
| **C5** | 编辑器与运行时是两套着色器 | 代码可推 | 所见非所得 |
| **C6** | 无 GPU 计时、无 golden image | 代码可推 | 性能与回归都无门禁 |

---

## 2. A 类：结构性缺陷（不修则无法进场景）

### A1 绘制模块未接入帧图

**证据**：`grep -rln "VegetationDraw" renderer engine` 只返回 `renderer/vegetation_draw.{hpp,cpp}`；`renderer/d3d12_renderer.*` 里没有任何 `vegetation` 符号。`native_vegetation_draw_tests.exe` 自己创建 RT/DSV、自己算光源矩阵、自己 `begin/submit`。

**后果**：`VegetationFrameInput::lightViewProjection` **没有生产者**——阴影矩阵目前只存在于测试的 `orthographic()` 调用里（`tests/native_vegetation_draw.cpp:372`）。接进真实渲染器时，缺的不是"插一行调用"，而是：光源矩阵构造、RT 绑定契约（`drawShadow` 会把 RT 解绑，调用方必须重绑）、以及和主场景深度缓冲的共享。

**修法**：在帧图里加三段槽位——`vegetation.shadow`（深度预 pass 之后、主 pass 之前）、`vegetation.depth`（写深度、不写色）、`vegetation.color`（depth EQUAL）。光源矩阵由渲染器的方向光生成，植被模块只消费。同时给 `draw()` 加一个 `BindTargets` 前置检查：若未绑定色/深目标则返回 `NG-E-VEG-STATE` 而不是静默画到上一帧的目标上。

**验收**：引擎主循环里出现 1 株植被并有截图；`evidence.json` 增加 `"integrated": true` 与帧图槽位名。

---

### A2 单常量块 → 一帧只能画一页一个物种

**证据**：`renderer/vegetation_draw.cpp:466` 的 `writeConstants()` 往 `constantMapped`（单块 256 B）写；文件自己的注释（:463-465）写明"两次不同的写入会追溯性改变第一趟"。`draw()` 里 `constants.counts[2] = 1`（speciesCount 固定 1），`counts[3] = kMaterialBase`。

**后果**：区域包 `meadow_ridge` 是 25 页。要画完整片草地，需要 25 次 `draw()`；当前结构下第 25 次写入会把前 24 次的页原点、页尺寸全部覆盖，**所有页都按最后一页的坐标解码**。这不是性能问题，是坐标错误。测试之所以通过，是因为它每帧只画一页。

**修法**（按代价从低到高）：
1. **常量环**：每帧 × 每页一个 256 B 槽（如 32 页 × 256 B = 8 KB），`SetGraphicsRootConstantBufferView(0, base + slot*256)`。改动最小，仍受页数上限约束。
2. **页表进 SRV（推荐）**：`StructuredBuffer<PageRecord>`（64 B/页，与 `region.mjs` 的 `PAGE_LAYOUT` 同构），实例记录里带 `pageIndex`，顶点/剔除着色器按页取原点。这样**一次 draw 覆盖所有页**，多物种也能靠 `species` 字段分派网格（需 SRV 索引顶点缓冲，或退化为每物种一次 draw）。
3. 短期折中：每页一次 draw + 常量环，先把 5,223 实例正确画出来。

**验收**：单帧绘制 25 页 / 5,223 实例，任取两页的实例世界坐标与 CPU 参考实现的差 ≤ 量化步长（0.478 mm）；draw call 数 ≤ 页数（方案 3）或 ≤ 物种数（方案 2）。

---

### A3 无每实例 LOD 执行路径

**证据**：
- `engine/vegetation/region.mjs:31` 定义了 `lodBias: u8`，`asset.cpp:325` 解码它，但全仓没有任何消费者。
- `lod.hpp:30 selectLod()` 只出现在 `tests/native_vegetation.cpp`。
- `vegetation.hlsl:73 decodeInstance()` 只读 `base + 0..3` 四个 DWORD；`lodBias/flags/palette` 在第 5 个 DWORD 里，从未被读。
- `VegetationDraw::draw()` 每帧只绑定一份 `vertexBuffer/indexBuffer`——一次 draw 就是一个 LOD。

**后果**：见 §0 第 3 条。`instanceBounds.z` 是**距离剔除**，不是 LOD 切换；越过它直接消失。

**修法**：在 `CSCull` 里算 `errorPixels = worldError * scale * viewportHeight / (2·tanHalfFov·depth)`（公式已在 `lod.hpp:21`，移到 HLSL 并加单元测试对齐），按 `selectLod()` 的结果 `InterlockedAdd` 到 3–4 个独立计数器，每个计数器写自己的 `DrawIndexedInstanced` 参数，`ExecuteIndirect` 调 3–4 次、每次绑对应 LOD 的索引缓冲与顶点偏移。`lodBias` 作为每实例偏移加在阈值上（这是它存在的意义）。

**验收**：100 m 处 5,223 实例中 ≥ 90% 落在 LOD1 以上；同场景三角形 ≤ 800 万；LOD 切换点用固定相机扫距离，`lodBias` 改变时切换距离单调移动。

---

### A4 LOD 误差度量对"删叶子"是盲的

**证据**：`QUALITY`（`species.mjs:619-624`）只做两件事——径向段数 ×0.75/×0.6/×0.5、叶片按 `cardRng.unit() < leaves` 随机保留 62%/28%/10%。`measureLodError()`（`:457`）采样稠密网格 512 个点，量它们到粗网格三角形的最近距离。

**后果**：删掉 38% 的叶片不移动任何剩余顶点，误差就停在"叶片到枝条的距离"这个量级——oak LOD1 报 0.085 m，而实际视觉变化是**冠形薄了 38%**。以 0.085 m 算，13.5 m 的树在 45 m 处投影误差不到 2 px，于是"看起来还很清晰"，但叶片已经少了一半。这也解释了 §2.4 记录的"45 m 处叶量少 28%"——一半是几何稀疏，一半是度量放过了它。

**修法**：把误差改成**屏幕空间覆盖误差**，复用已有的软件光栅器（`bakeImpostor()`，`species.mjs:486`）：
- 16 个水平视角 + 4 个俯仰，128²，对 LOD_n 与 LOD_0 各渲一张 coverage + depth；
- 报 `coverageIoU`、`meanCoverageLoss`、`p95DepthErrorPixels`；
- LOD 切换阈值改用 `meanCoverageLoss ≤ 3%` 反解距离，而不是用顶点距离反解。

**验收**：oak 三级 LOD 的 `meanCoverageLoss` 单调递增且 LOD2 ≤ 8%；把叶片密度减半时该指标必须上升 ≥ 10 个百分点（否则指标无效）。这条断言是这套度量的"对照实验"。

---

### A5 impostor 已烘焙未绘制

**证据**：`compileSpecies` 产出 `representations: ['mesh','impostor']` 与 8 视角 128² 图集（包内可见），`vegetation_draw.*` 无任何 impostor 管线。

**后果**：最后的降级档缺失，远景只能停在 LOD2（22,110 tri）。300 m 处一株树投影不到 20 px，却仍是两万面。

**修法**：一个独立的 billboard PSO（2 tri/实例），顶点着色器按视线方向选视角、做视差偏移（用图集深度），阈值 `projectedRadius < 3 px`。与 A3 的 LOD 桶并列，作为第 4 个桶。

**验收**：300 m 处单株 ≤ 6 tri；与 LOD2 做同机位 A/B，`coverageIoU ≥ 0.92`。

---

### A6 统计量是提交量，不是绘制量

**证据**：`draw()` 尾部 `stats.drawnInstances += instanceCount; stats.triangles += indexCount/3 * instanceCount`——`instanceCount` 是**上传**的实例数，剔除结果 CPU 从未读过。`stats.overflow`（`hpp:37`）声明但从未写入；`vegetation.hlsl:166` 在 `slot >= capacity` 时静默丢弃。

**后果**：证据表里的"绘制实例/三角形"是上界，不能证明剔除生效；容量溢出也没有任何信号（当前每页 ~209 实例、容量 256，所以今天不触发，但扩容或改页大小时会静默丢株）。

**修法**：`CSArgs` 里顺带把 `max(0, visibleCount - capacity)` 写进 `drawArguments[4]`（或独立 UAV 槽），帧末一次 16 B 读回（延迟 1–2 帧，环形缓冲 8 帧）。`stats` 增加 `visibleInstances / overflowInstances / submittedInstances` 三字段，测试断言 `visible ≤ submitted` 且 `overflow == 0`。

**验收**：测试里构造"相机看向反方向"，`visibleInstances` 必须为 0（当前只能靠像素覆盖率间接证明）。

---

## 3. B 类：画质与物理正确性

### B1 叶片法线贴图切空间错误

**证据**：顶点记录 48 B = `{pos.xyz, wind}` + `{normal.xyz, material}` + `{uv.xy, color, 0}`（`species.mjs:76-83`，`vegetation.hlsl:13-16`）——**没有切线**。着色器用 `input.tangent = axisX`（实例的偏航轴，`hlsl:233`）再对几何法线正交化（`:328`）。但每张卡片的真实切线是 `card()` 里传入的 `tangent`（`species.mjs:114`），它随卡片随机朝向变化。

**后果**：同一株树上所有卡片的法线贴图切空间都不同，且与叶片走向无关——叶脉、叶面卷曲的方向变成噪声。这不是"轻微不准"：叶片法线贴图的主要信息就是方向性的。

**修法**：`v2.w` 目前空闲（第三行 float4 的第 4 个分量），用它存八面体编码的切线（或直接存 `axisX` 在卡片空间的投影角，1 个 float）。着色器恢复真实切线，`axisX` 只作为退化时的回退。这是**零字节增长**的修复。

**验收**：把 `leaf_normal` 换成正弦条纹图，旋转单张卡片 90° 后，画面高光带必须跟着转 90°（当前会随机）。写成一个渲染测试用例。

---

### B2 叶片是 4 顶点平面、单一法线

**证据**：`card()` 用 4 个顶点，`droop` 只把上边缘向下推（`species.mjs:117,125-126`），但四个顶点的法线都是同一个 `normal`；着色器再统一处理。

**后果**：下垂的叶片在下半部分仍然按"平的法线"受光，叶片像硬纸板；配合 wrap diffuse 0.45，整片叶亮度几乎均匀。

**修法**：卡片改成 2×2 或 1×3 网格（8–12 顶点），按折角给每行法线；或保持 4 顶点但用 `uv.y` 在像素着色器里扰动法线（近似曲面，零几何成本）。后者更划算：`normal = normalize(normal + geometric * (uv.y-0.5) * curl)`。

**验收**：单叶片大特写（相机 0.6 m）能看出上暗下亮或上亮下暗的连续过渡，而不是一条硬边。

---

### B3 无 AO，`depthTint` 是高度函数

**证据**：`hlsl:354` `depthTint = lerp(0.72, 1.0, saturate(localPosition.y * 0.9 + 0.25))`——只依赖**物种局部高度**，同一高度处冠表与冠内完全一样。

**后果**：树冠内部不会更暗。阴影贴图能解决"叶片投到叶片上"，但解决不了"接触遮蔽"（叶片与枝条交界、冠心），而后者是植被体积感的主要来源。这也是"树冠读起来平"的第二个成因（第一个是 A4 的叶片密度）。

**修法**：**烘焙顶点 AO**——对每个顶点向半球投 32 条射线（只对自身网格做 BVH，已有 `SurfaceSampler` 的 BVH 可复用），存进 `color.a`（当前固定 255，未使用）。像素着色器 `albedo *= ao`。成本在 cook 侧（oak 133k 顶点 × 32 射线，预计 2–4 s），运行期零成本，且 LOD 之间可继承（粗级从细级采样）。

**验收**：冠心/冠表亮度比 ≥ 1.35（当前 ≈ 1.0，只有阴影贡献）；顶点 AO 的直方图不能塌成单峰。

---

### B4 风是绝对振幅 + 单频正弦

**证据**：`hlsl:219-222`——`wave = sin(t·freq + phase·2π + local.y·0.35)`，`world += windDirection * wave * gust * windWeight * amplitude * scale`，其中 `amplitude = constants.pageOrigin.w = frame.windAmplitude`（米）。`windSpeed`（`windDirection.w`）根本没参与位移。

**后果**：
1. 0.42 m 的草和 13.5 m 的橡树**摆同样的米数**（0.1 m）——相对摆幅差 32 倍，草会像硬纸片一样横移，树几乎不动。
2. 单一频率 + 单相正弦，整片林子同相位（`phase` 是每实例随机的，所以不完全同步，但没有阵风前沿）。
3. 只有水平平移，没有绕基部旋转，也没有枝条扭转，所以大型乔木的"整冠扫过"感缺失。
4. `windSpeed` 是死参数——编辑器里改了它，画面不变。

**修法**：
- 振幅按物种高度归一：`amplitude = windAmplitude * heightRatio`，让 `windAmplitude` 语义变成"尖端相对摆幅（%）"。
- 两个八度：低频阵风（0.05–0.1 Hz，空间上随位置偏移相位）+ 高频抖动（叶片专属，权重只取叶片）。
- `windSpeed` 进入相位：`t * windSpeed * k`，让风速真的有物理意义。
- 加 15–25% 垂直分量，模拟枝条被压弯后的回弹。
- 阵风用一张 2D 噪声（CPU 每帧算 4 个角点、以常量传入，或用 1D 纹理），否则一棵树一个正弦，两棵树还是一样。

**验收**：同风速下，草与树的尖端相对摆幅差 ≤ 2 倍；把 `windSpeed` 从 0.6 改到 3.0，位移频谱的主频必须上升（不是只改振幅）；连续 300 帧的顶点位置无跳变。

---

### B5 阴影单级 2048²、caster 复用相机列表

**证据**：`vegetation_draw.cpp:80 kShadowMapSize = 2048`；`drawShadow()` 里 `cullPass()` 用的是 `writeConstants()` 写入的 `frame.viewProjection`（相机矩阵），不是光源矩阵；`hlsl:278` 出图即"全亮"。

**后果**：
1. 一张 2048² 覆盖整个页（`pageSize` 下的 200 m 级），texel ≈ 10 cm，叶片阴影必然锯齿/漏光；3×3 PCF 只能糊掉一部分。
2. 相机背后的植株不投影——转身时阴影会突然出现。
3. 级联、光源视锥剔除、阴影缓存都没有，`shadowStrength` 只有一个全局值。

**修法**（分两步，别一次做完）：
- **第一步**：给阴影单独一次剔除，用光源正交视锥；矩阵由渲染器提供，范围拟合到"当前相机可见植被的包围盒"（比整页小一个量级）。
- **第二步**：2 级级联（近 30 m / 远 120 m，各 2048²），级联接缝用 `shadowStrength` 淡出；PCF 半径按 `ddx(uv)` 缩放做斜率自适应。

**验收**：同机位下，阴影贴图的有效 texel 密度（可见像素/阴影 texel）≥ 4；转身 180° 前后，地面/冠层的阴影总量差 ≤ 5%；近景叶片阴影边缘的阶梯 ≤ 2 px。

---

### B6 无 MSAA、无 TAA、无速度

**证据**：主 PSO `graphics.SampleDesc.Count = 1`（`vegetation_draw.cpp:207`）；`VsOutput` 无 `SV_Position` 历史输出，`VegetationFrameInput` 无上一帧矩阵。

**后果**：alpha-tested 叶片是抗锯齿的最坏情况——每片叶子都有硬边。mip 链解决了远景，近景仍是锯齿；一旦加 TAA，没有速度缓冲就会拖影。

**修法**：顺序不能反——
1. 先 **alpha-to-coverage + 4× MSAA**（不需要历史，立竿见影；遮罩值直接送 A2C，无需 `discard`）；
2. 再 **每实例速度**（当前世界位置 - 上一帧世界位置，含风位移，1 个 half4 RT）；
3. 最后 TAA（含风的 velocity 才有意义）。

**验收**：静止画面下，叶片轮廓边缘的梯度宽度 ≥ 1.5 px；相机匀速横移 1 px/帧 时，无 A2C 与有 A2C 的覆盖率差 ≤ 3%。

---

### B7 alpha test + 无深度预 pass

**证据**：`draw()` 只有一条 graphics PSO，`DepthWriteMask = ALL`，像素着色器开头 `discard`（`hlsl:298-303`）。没有独立的 depth-only pass。

**后果**：`discard` 会让硬件关闭 early-Z（保守实现），被完全遮挡的像素仍会跑完整个像素着色器；叠加 3 万张卡片的 overdraw，这是当前最大的像素成本项。而这一点**从未被测过**（台账 §V2 明确写"overdraw 未测"）。

**修法**：深度预 pass（alpha test、无色输出、`DepthFunc LESS_EQUAL`）+ 主 pass（`DepthFunc EQUAL`、去掉 `discard`，用 A2C 处理边缘）。预 pass 的像素着色器只采遮罩，成本约为完整着色的 1/10。

**验收**：用 `RasterizerStats` 或 PIX 抓一次 draw，报出 overdraw 倍数；预 pass 后总像素着色器调用数下降 ≥ 40%。

---

### B8 材质分类靠数组下标

**证据**：`hlsl:292 isLeaf = (material == 1)`；材质号来自 `spec.materials.findIndex(...)`（`species.mjs` 内 `buildPreview`/cook 处）。实测四个物种的材质顺序：oak `0:bark 1:leaf`、hazel `0:stem 1:leaf`、pine `0:bark 1:needle`、**meadow_grass `0:blade`**。

**后果**：草的 `blade` 在索引 0 → 走树皮分支：没有遮罩裁切、没有厚度透射、`wrap` 只有 0.12、粗糙度取树皮。草叶因此被当成"木头"着色。今天因为草物种没绑贴图、且单物种绘制，症状被掩盖；一旦多物种共页（A2 的方案 2）或给草绑贴图，草会采样树皮 albedo/normal，UV 又是 0..1 全覆盖，直接错。

**修法**：把"材质类别"从数组下标改成**烘焙进顶点的语义位**（`v1.w` 现在存材质索引，改成 `index | class<<8`），或增加一张 per-draw 的材质表 SRV（`{albedoSlot, maskSlot, class}`）。推荐前者：零成本，且类别不再依赖材质数组顺序。

**验收**：新增一个 grass 渲染用例，断言草叶走了透射分支（背光亮度 > 正面 0.2 倍，当前几乎为 0）；给草绑 oak 的贴图后不出现树皮纹理。

---

## 4. C 类：资产与工具链

### C1 顶点 48 B、物种包 13.06 MB

**实测**：oak LOD0 `132,984` 顶点 × 48 B = 6.38 MB；索引 `70,008×3×4` = 0.84 MB；加 impostor/碰撞/阴影代理后整包 **13.06 MB**（`build/vegetation/cooked/library.json`）。四物种合计 19.6 MB。

**修法**：
| 字段 | 现在 | 目标 | 省 |
| --- | --- | --- | --- |
| position | 3×f32 = 12 B | 3×u16 归一化到物种包围盒 = 6 B | 6 B |
| normal | 3×f32 = 12 B | 八面体 2×i16 = 4 B | 8 B |
| uv | 2×f32 = 8 B | 2×u16（图集内）= 4 B | 4 B |
| color | RGBA8 = 4 B | RGBA8 = 4 B（alpha 让给 AO） | 0 |
| wind | 1 B | 1 B（可并入 color.a 的 spare bit） | 0 |
| material | 混在 normal.w (4 B) | 打包进 normal 的 spare bit | 4 B |
| **合计** | **48 B** | **20 B** | **−58%** |

索引：LOD1/LOD2 顶点 < 65,535 时可改 u16（LOD0 保持 u32，或分块）。综合后 oak 包 ≤ 3.5 MB。

**验收**：包字节数与顶点记录布局写进证据；解压后的位置误差 ≤ 0.5 mm（与现有量化误差同量级）。

---

### C2 贴图 RGBA8、8.4 MB、无 BC

**实测**：8 张 512²×10 级，单张 1,398,132 B；6 张绑定 → 约 8.4 MB 常驻。

**修法**：albedo → BC7（sRGB），normal → BC5（重建 z），mask → BC4，roughness → BC4 或并进 albedo.a。压缩比 4:1（BC7 8 bpp vs 32 bpp），目标 ≤ 2.5 MB。**必须先复测遮罩覆盖率**：BC4 有损压缩会改变阈值附近的纹素，A4 的覆盖率表要在压缩后重跑；允许 ±3%，超了就提高边缘 alpha 的量化步长或改 BC7 的 alpha 通道。

**验收**：压缩后 512→8 级的覆盖率最差偏离 ≤ 3%（当前 −1.7%）；远景 45 m 覆盖像素下降 ≤ 5%。

---

### C3 单株 31,488 张叶片卡片

**实测**：oak LOD0 叶片 62,976 tri = **31,488 张卡**（每卡 2 tri），占顶点总数 125,952/132,984。生成器默认图是 17,280 张（34,560 tri）。速度树类工具的英雄树通常在 2,000–6,000 张卡（每张卡内含一簇叶）。

**后果**：这是 A3 之外第二个三角形爆炸源，也是 B7 overdraw 的主因。

**修法**（两条并行）：
1. **图集从"单片叶"改成"叶簇"**：现在 3×4=12 格、每格 170×128 px 只放一片叶；改成 2×2 或 4×4 的**叶簇**（每格 5–8 片叶），一张卡片承载一簇。同等叶量下卡片数降 5–8 倍。
2. **卡片实例化**：一张卡只需 4 个顶点 + 每卡一个 `float4`（位置 + 朝向 + 尺寸），或直接把卡片做成 GPU 实例（`SV_InstanceID` 索引卡片表），顶点数从 12 B/顶点 的 4 个退化成 16 B/卡。

**验收**：oak LOD0 ≤ 18,000 tri（含枝干），单株卡片 ≤ 6,000；同机位 `coverageIoU` 相对当前 ≥ 0.95（叶量不能明显变少）。

---

### C4 编辑器预览 2.77 MB JSON 往返 / 273 ms

**实测**：`docs/VEGETATION-EDITOR.md` §6——预览 payload 2.77 MB，`273 ms`（LOD1、69,610 tri），首屏含误差测量 1,362 ms。

**后果**：拖滑杆时的反馈延迟落在"能感觉到卡"的区间（>150 ms），且 base64 让字节量再涨 33%。

**修法**：
1. 传输改二进制（`application/octet-stream`：header JSON + `Float32Array` 拼接），省掉 base64 与 `JSON.parse`；单次可降到 ~700 KB。
2. 顶点先按 C1 量化成 u16 + 八面体法线，再降 60%。
3. 视口只重建变化的部分：改风权 → 只更新 wind 缓冲；改叶片参数 → 全部重建（这类操作本来就慢，用进度指示）。
4. 前端 16 位索引 + 一次 `bufferSubData`。

**验收**：改单个风权滑块 → 网络字节 ≤ 64 KB、端到端 ≤ 50 ms；改分支数 → ≤ 150 ms。

---

### C5 编辑器与运行时是两套着色器

**证据**：`editor/ui/vegetation.mjs` 里是 WebGL2 + 自写着色器（无阴影、无 AO、程序生成图集）；运行时是 HLSL（有阴影/法线/透射）。

**后果**：**所见非所得**。在编辑器里把叶片调亮到满意，进引擎会变暗（引擎多乘 shadow × depthTint，且用真图集）；反之调好的冠形在引擎里可能因为 AO 而糊成一团。几何一致不代表外观一致——`VEGETATION-EDITOR.md` §7 只保证了前者。

**修法**（选一）：
- **推荐**：编辑器视口接原生渲染器的**离屏输出**（帧图渲染到纹理 → 通过 WebSocket 推 JPEG/WebP，或直接用 Win32 子窗口承载 D3D12 swapchain）。这样外观由同一份 HLSL 决定。
- 次选：把 HLSL 的着色模型同步到 WebGL（阴影、AO、透射、图集），并加一条"同一 spec / 同一相机 → 两边的平均色差 ≤ ΔE 3"的自动化对照。

**验收**：同一 spec、同一相机，编辑器截图与原生截图的 `mean ΔE ≤ 3`，冠部覆盖率差 ≤ 3%。

---

### C6 无 GPU 计时、无 golden image

**证据**：`vegetation_draw.*` 无 `QueryHeap`；`reports/vegetation-hair/evidence.json` 只有覆盖像素与断言数，无时间。测试断言是"覆盖率 > 阈值 / 亮度变化方向"，没有参考图。

**后果**：所有"性能"讨论都是空的（台账已诚实写明"无任何帧的 P95/P99"）；所有"画质"改动都无法回归——把叶片调暗 10% 不会让任何断言失败。

**修法**：
1. 加 `D3D12_QUERY_TYPE_TIMESTAMP` ×2（阴影 pass 前后、主 pass 前后），环形 8 帧读回，输出 P50/P95/P99 到 `evidence.json`。
2. 加 4 张 golden PNG（近景/远景/背光/阴影），用每像素 ΔE + 覆盖率双指标，容差写死（如 `p95 ΔE ≤ 4`），超差即失败并输出 diff 图。

**验收**：`evidence.json` 里出现 `"gpuMs": {"p50":…,"p95":…,"p99":…}`；人为把 `ambient` 改 20% 能让 golden 测试变红。

---

## 5. 改进路线

四波，按"能进场景 → 像植物 → 跑得动 → 可验收"排序。时间是人日区间，含测试与证据，不含美术调参。

### Wave 1：让它能进场景（P0，约 8–12 人日）

1. **A1 接入帧图**（3–4 d）：光源矩阵生产者、三段槽位、状态校验。
2. **A2 多页绘制**（3–4 d）：先常量环，再评估页表 SRV。
3. **A6 真实统计**（1 d）：可见/溢出/提交三计数 + 环形读回。
4. **A3 每实例 LOD**（2–3 d）：HLSL 侧投影误差 + 4 个 LOD 桶 + `lodBias` 消费。

> 这一波做完，`meadow_ridge` 的 5,223 实例应该能在**一帧**里正确画出来。这是"植被系统有没有"的分界线。

### Wave 2：让它像植物（画质根因，约 12–16 人日）

1. **B1 切线**（1 d，零字节）：`v2.w` 存切线，恢复法线贴图方向。
2. **B3 顶点 AO**（3–4 d）：cook 侧半球射线，存 `color.a`，LOD 间继承。
3. **B8 材质语义位**（1–2 d）：类别不再依赖数组顺序；草叶获得透射。
4. **B4 风重做**（3–4 d）：相对振幅、双八度、`windSpeed` 生效、阵风场。
5. **B2 叶片曲率**（1–2 d）：像素侧扰动法线，零几何成本。
6. **A4 覆盖率误差**（2–3 d）：复用软件光栅器，重算 LOD 阈值。

> 这一波之后才值得谈"像不像树"。**在这一波之前调 albedo、调 ambient 都是把错误调顺眼。**

### Wave 3：让它跑得动（约 15–20 人日）

1. **C3 叶簇图集 + 卡片实例化**（5–7 d）：卡片数 31,488 → ≤ 6,000。
2. **C1 顶点量化**（3–4 d）：48 B → 20 B，包 13 MB → ≤ 3.5 MB。
3. **C2 BC 压缩**（2–3 d）：8.4 MB → 2.5 MB，**同时复测覆盖率**。
4. **B7 深度预 pass**（2–3 d）：overdraw 可测、可降。
5. **A5 impostor 绘制**（3–4 d）：第 4 个 LOD 桶，300 m 处 6 tri。

### Wave 4：闭环与验收（约 10–14 人日）

1. **C6 GPU 计时 + golden image**（3–4 d）。
2. **B5 阴影第二步**（4–5 d）：光源视锥剔除 + 2 级级联。
3. **B6 A2C + MSAA，再 TAA**（4–5 d，TAA 可选）。
4. **C4/C5 编辑器**（4–6 d）：二进制传输入、原生视口或着色对齐、撤销栈。

总计约 **45–62 人日**（不含毛发主线、不含 30M 规模验证）。这与计划 §10 的 67–118 人周不冲突：那是对全系统（含 strand 渲染器、体素聚合、30M 流送）的估算，本文只是植被主线的可达路径。

---

## 6. 预算目标（验收用）

| 项 | 现状 | 目标 | 依据 |
| --- | --- | --- | --- |
| oak LOD0 三角 | 70,008 | ≤ 18,000 | C3 |
| oak LOD1 / LOD2 | 44,872 / 22,110 | ≤ 8,000 / ≤ 2,500 | A3 + C3 |
| impostor | 未绘制 | 2 tri/实例 | A5 |
| 单株卡片数 | 31,488 | ≤ 6,000 | C3 |
| 顶点记录 | 48 B | 20 B | C1 |
| 物种包 | 13.06 MB | ≤ 3.5 MB | C1 |
| 6 张贴图常驻 | 8.4 MB | ≤ 2.5 MB | C2 |
| 25 页 / 5,223 实例 单帧三角 | 不可绘制 | ≤ 8,000,000 | A2+A3 |
| 编辑器预览 p50 | 273 ms | ≤ 60 ms | C4 |
| GPU 帧时间 | 无数据 | P95 有数（RTX 5060） | C6 |

---

## 7. 顺序陷阱（做了会更糟）

1. **先做 TAA**：没有速度缓冲（含风位移），叶片会拖影成雾。必须先 A2C+MSAA，再速度，再 TAA。
2. **先调颜色/ambient**：B1（切线错）与 B3（无 AO）没修之前，调色只是在补偿错误——而且每修一个根因，之前的调色都要推翻。
3. **先压 BC**：不改 C3 的卡片数就压贴图，省下的 6 MB 会被 13 MB 的顶点吃掉；且压缩后必须重跑覆盖率表，否则远景的洞会回来。
4. **先做 30M**：A2 没解决之前，30M 只是"更长的单页"，索引容量测了也没有绘制路径。
5. **先做级联阴影**：B5 第一步（光源视锥剔除）就能拿掉大部分问题，级联的收益要等 A1 接进帧图之后才能测。

---

## 8. 立即可执行的三项检查

```bash
# 1. 确认绘制模块确实没被引用（应只输出它自己两行）
grep -rln "VegetationDraw" renderer engine

# 2. 确认 LOD 选择没有运行时调用点（应只在 lod.hpp 与 tests/ 里）
grep -rn "selectLod\|projectErrorPixels" engine tests renderer shaders

# 3. 复核卡片与三角形占比（改任何画质参数前先跑）
node --input-type=module -e "const {buildPreview}=await import('./engine/vegetation/species.mjs');const fs=await import('node:fs/promises');const s=JSON.parse((await fs.readFile('build/vegetation/source/oak_broadleaf.species.json','utf8')).replace(/^\uFEFF/,''));const p=buildPreview(s,{level:0});console.log(p.triangles,p.vertices,p.primitives)"
```

第 3 条当前的输出是 `70008 132984 [{material:0,indexCount:21096},{material:1,indexCount:188928}]`——即叶片 62,976 tri、树皮 7,032 tri，与 §C3 的卡片数一致（62,976 / 2 = 31,488 张）。

---

## 9. 与既有文档的关系

- 本文不重复 [VEGETATION-HAIR-STATUS.md](VEGETATION-HAIR-STATUS.md) 的逐阶段台账，只做**缺陷归因与排序**。台账回答"做过什么"，本文回答"哪里不对、先修哪个"。
- 台账 §4 的 9 条已知缺陷仍然有效，本文在其上新增了三条它没登记的：**A1（未接入帧图）**、**A4（误差度量盲区）**、**B1（切线缺失）**。
- 台账 §5 的"下一步顺序"以本文件 §5 的四波取代：原第 1 项（着色器侧覆盖率合成）被并入 Wave 2 的 A4/B2，因为"轮廓薄 14%"的根因是 alpha 依赖硬阈值，而硬阈值又依赖误差度量；先修度量比先修着色器更不容易返工。
