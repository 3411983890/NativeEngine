# GPU 三角布料：实现、接口与验证边界

日期：2026-10-24。对应完整方案 E0/E1/E2 的增量；E 阶段与 A–G 总目标仍未完成。

`engine/native/cloth/solver_gpu.*` 实现 D3D12 视觉布料，`solver_cpu.*` 提供同模型数值参考。GPU 输出形变位置、速度、法线和切线；后续同日新增了真实 GPU 绘制与宿主 `--cloth` 预览，见 [CLOTH-RENDERING.md](CLOTH-RENDERING.md)。共享场的 CPU 权威数据保持只读，不把 GPU 布料结果作为玩法权威输入。

后续性能增量：[CLOTH-GROUPED.md](CLOTH-GROUPED.md)。容量内的网格默认采用组内完整 tick，原逐颜色路径保留作回退/对照，并统一严格浮点编译。下文求解模型和资源合同继续适用，13-kernel 数量与旧计时是该批历史记录；当前包含批处理在内为 15 个 compute blob，宿主统一派发详见 [CLOTH-BATCH.md](CLOTH-BATCH.md)。

## 资产与迁移

当前五个作者资产使用 `nativegpu.cloth/2`，离线产物为 `nativegpu.cloth-cook/2`。内容寻址的库清单仍是 `nativegpu.cloth-library/1`，库可包含两代 cook。原生加载器同时验证 /1、/2 的外层与载荷版本、SHA、顶点及约束。

| 参数 | /2 的含义和单位 |
|---|---|
| `stretchCompliance` | 经/纬膜应变顺应度，m/N；求解中除以静止三角面积 |
| `shearCompliance` | 膜剪切顺应度，m/N；求解中除以静止三角面积 |
| `bendAngleCompliance` | 四顶点二面角顺应度，rad/(N·m) |
| 面密度、厚度、坐标和时间 | kg/m²、m、m、s |

旧版 `bendCompliance` 是距离约束顺应度，不能把数值直接改名为角度参数。`migrateCloth(old, explicitAngularCompliance)` 与 CLI 均要求明确的新参数；CLI 以新文件写入，目标已存在时拒绝覆盖。

```powershell
npm run cloth -- migrate --in old.cloth.json --out new.cloth.json --bend-angle-compliance 1000
npm run cloth -- validate --in assets/cloth/flag_banner.cloth.json
npm run cloth -- cook --in assets/cloth --out build/my-cloth
```

五个示例的角度顺应度是新的作者选择，不是旧距离参数的物理等价转换。/1 仍由 CPU 距离后端执行；GPU 明确拒绝 /1。使用新资产时需同时重新烘焙并构建新加载器。

`topology.mjs` 离线烘焙材料空间逆矩阵、静止面积、带符号二面角、顶点邻接，以及膜/弯曲两套完整着色调度。运行时验证每条约束的覆盖、四顶点唯一性、流形/朝向、材料基、邻接和颜色内所有读写顶点的互斥，包括锚点。没有运行时补做着色或退回存在竞争的调度。

20×14 旗帜有 494 个膜三角形、709 个内边铰链、2191 个标量约束，当前离线调度为 7 个膜颜色、14 个铰链颜色。旧 `stretch/shear/bend` 边数组保留作旧版求解与几何诊断；/2 不再用跨两格的距离边冒充弯曲。

## 求解

令 `F=[p1−p0,p2−p0]·Dm⁻¹`，每个三角形依次求解 `|F0|−1`、`|F1|−1` 与 `dot(F0,F1)`。铰链以两个相邻面的带符号 `atan2` 二面角相对静止角求解，角差包裹到 [−π,π]。二面角使用解析四点梯度，增量测试用有限差分和刚体平移/转动不变性独立核验。

XPBD 每个子步清空 λ，每次迭代使用 `Δλ=(−C−αλ)/(Σ w|∇C|²+α)`；膜的 `α=compliance/(area·h²)`，铰链的 `α=bendAngleCompliance/h²`。理论依据为 [Macklin 等的 XPBD 论文](https://matthias-research.github.io/pages/publications/XPBD.pdf)。子步数仍为 `min(maxSubsteps,ceil(dt·480))`，并不消除迭代误差或接触顺序误差。

每子步先从同一份位置/速度状态计算三角气动力，再积分，避免逐顶点更新造成邻居数据竞争。面法向风压为 `0.5·1.225·Cd·A·vn·|vn|·n`，使用相对风速，每个面平分给三顶点；切向风不产生该模型中的法向压力。加速度有界到 4000 m/s²。然后按离线颜色求膜与铰链、回写锚点并投影平面/球/胶囊接触，最后重建速度。碰撞体为静态世界空间数据，使用半布厚度加作者间隙。

法线按当前三角面积累积，切线来自材料经向、正交化后附带手性。退化约束/法线和非有限状态有独立诊断；非有限恢复不能作为数值稳定性通过的证据。`previous` 保存上一子步位置，不提供上一渲染帧的运动矢量。

## GPU 合同与资源

调用者传入真实设备和 `ID3D12Fence*`。solver 持有设备、fence 与资产快照，不创建队列、不等待、不读回。命令必须位于同一实际队列时间线；未来提交 fence 必须大于已完成值且不倒退。调用者需提交已经成功记录的命令，再按同一队列顺序继续使用资源。

```text
创建 GpuSolver(device, actualFence, immutableAsset, limits, parameters)
initialize(commands, nextFence)
step(commands, nextFence, fixedDt [, explicitFieldTick])
prepareRead(commands, drawFence) → buffers() → 使用 GPU 顶点
实际队列提交并完成最后使用 fence → shutdown()
```

`prepareRead` 将顶点转换为 NON_PIXEL_SHADER_RESOURCE，并登记最后使用 fence；下次求解恢复 UAV。位置/previous/速度/法线/切线组成 80 字节顶点。暴露的 wind/diagnostics 仍是 UAV，膜表是不可变 upload 资源。绘制者必须使用这些声明状态、登记真实读使用期限，不能在已关闭 solver 后持有裸资源指针。

共有 12 个 committed buffer：6 个 DEFAULT 状态资源和 6 个不可变 UPLOAD 资源。DEFAULT 以 COMMON 创建后显式转换，实际分配按 `GetResourceAllocationInfo` 的对齐字节累计，分配前检查预算。默认上限 16384 顶点、每 tick 8192 dispatch、64 MiB；外部场页与绑定元数据由场服务另外计费。作者工具的 `poolBytes` 仅是平坦 CPU 数组估算，不能替代 GPU 实测分配。

无场 root 为 56 DWORD，读取场的变体为 58 DWORD，后者含场元数据 CBV 和直接索引 heap。十三个 compute kernel 涵盖初始化/传送、λ、场采样/整批提交、气动力/积分、膜/铰链、接触、速度与法线。每颜色派发后加 UAV barrier。每 tick 先估算全部派发量，超出预算在记录本次工作前拒绝。

支持暂停、传送、重置。传送整体移动形变并清零速度，清掉原位置的空间风；重置恢复当前 origin 下的静止形态。物理 tick 可重置，已用的场时钟仍保持单调。`shutdown` 在真实 fence 未完成时拒绝；析构不代替调用者等待，必须先正确结束在途使用。

## 共享风场

构造时可注入 `GpuVectorView`，只接受同设备的 `windVelocity`。米制坐标通过显式网格变为 Q8，复用 `field_constants.hlsli`、`field_sampling.hlsli` 和 192 字节向量元数据布局。三个 m/s 分量必须完整；支持负坐标、跨砖与 halo，网格映射及风速均有界。CPU 权威 World 只能镜像读取。

含场的 `step` 必须给出与不可变绑定一致且不倒退的独立 fieldTick。Eventual 过期、缺分量、越界或其他不可用查询使整件布料的候选输入失败：全部保留上一份成功的逐顶点风，没有部分顶点换成新输入。尚无成功样本时使用明确的 uniform fallback，默认为零。允许的 Stale 值与被拒绝的过期值分开诊断。

`setWind` 更新 uniform/fallback 并请求清除旧空间样本；已配置的场仍会在下个 tick 查询。场 root 模式在构造时确定，`bindWindField` 可替换兼容快照，不能传空解绑或临时给无场 root 增加场。旧快照由场服务按真实 fence 保留，空间租约的释放不使在途读取失效。

诊断槽 0–14 依次为：非有限状态、退化约束、场查询、不可用、整批拒绝、整批应用、Stale、clamp、批失败暂存、已安装空间风、退化法线、接触总投影、平面投影、球投影、胶囊投影。第 15 槽保留。它们是 GPU 累计计数，生产求解器不读取；专项通过验证专用 readback 检查。

## 增量入口与边界

```powershell
npm run cloth:gpu -- -Adapter AMD
npm run cloth:gpu -- -SkipBuild -Adapter AMD
npm run fields:adapters
```

布料入口只烘焙自己的 `build/cloth-gpu`、编译 `cloth.hlsl` 的 13 个 blob，运行布料 JS 合同、/2 与 /1 CPU 参考、GPU 布料专项；不构建宿主、其他测试或完整 shader 集。适配器入口使用 `build/field-adapters/cloth`。标准构建及可选 GPU native 门禁已登记新目标，本批没有执行这些完整入口。

证据包含输入/编译器/二进制 SHA，构建前后与测试前后核对，报告目录冻结源码、作者/烘焙资产、shader header、二进制和日志。`-SkipBuild` 遇到来源不符会拒绝；每份报告只证明其冻结版本。

2026-10-24 增量报告：[`cloth-gpu-evidence.json`](../reports/0.11.0/31c00bc53c0b/20261023T175805Z/cloth-gpu-evidence.json)。布料 JS 合同 8/8；/2 与 /1 CPU 各 7 组、150 次检查、100 次确定性重复；GPU 7 组、2905 次检查、160 次 CPU/GPU 顶点对拍。AMD 890M 实际启用调试层，debugErrors=0。最大 CPU/GPU 位置差 0.2403 mm、速度差 0.004318 m/s。

| GPU 验证对象 | 时长与采样 | 实测 |
|---|---|---|
| 20×14 旗帜，1.2×0.8 m，风速 4 m/s | 600 tick / 5 s 后的提交态 | 边长误差 P95 0.7024%，顶点最大速度 2.960 m/s，锚点漂移 0 |
| 4×4 方布，1×1 m，风速 4 m/s | 600 tick / 5 s 后的提交态 | 边长误差 P95 0.02884%，顶点最大速度 1.309 m/s，锚点漂移 0 |
| 静态平面/球/胶囊 | 240 tick / 2 s，采样 tick 1、2、30、120、240 | 采样提交态几何穿透 P99/最大值均 0；实际投影分别 80/164/55 次 |
| CPU 权威共享风 | 已知/部分缺失/过期/时钟拒绝/传送 | 整批应用或保留，CPU World 摘要不变，在途快照安全释放 |

两块布各实际分配 786432 字节（12 个 buffer 的对齐分配）。旗帜 600 tick 累计 435602 次 dispatch，方布 397202 次，包含初始化；没有由此推算生产负载性能。边长/速度只报告最后提交态，接触仅报告列出的采样点，不能将离散静态夹具解释为 CCD 或动态角色验收。共享场适配增量另通过 5 组、900 次检查，报告 [`field-adapters-evidence.json`](../reports/0.11.0/31c00bc53c0b/20261023T175430Z/field-adapters-evidence.json)。本批未运行全量回归。

仍缺 GPU 自碰撞、CCD、动态/扫掠碰撞体、最大位移约束、骨骼目标与坐标映射、渲染网格重心映射、LOD 实际拓扑切换、湿润/受热、完整材质资产、存档/编辑器事务与长时性能预算。双面基础织物着色和宿主真实绘制已在后续同日增量中实现，证据与限制见绘制文档。GPU 遇到 skinned 资产明确拒绝；五个库资产的存在不表示披风/裙摆已完成蒙皮行为。独立求解测试不证明角色运动穿透或完整生产布料管线达标。


2026-10-24 后续：已新增多实例完整 tick 与法线的统一派发、真实 fence 保护的有界元数据/描述符环，并接入宿主 `stepAll`。单独调用 solver 仍使用本页原路径；最新合同、逐位对拍和配对计时见 [CLOTH-BATCH.md](CLOTH-BATCH.md)。当前布料专项编译 15 个 compute＋2 个绘制 blob（17 个），完整清单在当轮为 75 个（GPU LEF 增量后当前为 80 个）。历史报告数值保持对应其冻结版本，不据此宣称完整 A–G 验收。


2026-10-24 后续共享场接入：新增 `--simulation-field`，无需 GPU 粒子也可为布料提供风，或让粒子/多布共享一份快照。旧入口与本页历史报告保留，最新时间/租约/后绑定/根签名合同及 100 项宿主证据见 [SIMULATION-FIELDS-HOST.md](SIMULATION-FIELDS-HOST.md)。
