# 粒子、场、布料、水开发记录

依据：[完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md)。开始：2026-10-22。

本记录只以代码、实际测试和版本化报告更新状态。完整方案 A–G 尚未完成，既有 smoke/fire 不等于通用粒子与共享世界场。

**当前回归执行约束（用户于 2026-10-23 指定）**：仅执行与本次改动相关的增量回归；没有用户明确命令，不执行全量回归。下文较早的全量结果仅为历史证据，不表示后续改动又执行了全量。构建/门禁入口保留，不能以本约束关闭 CI 或将未运行项记为通过。

| 工作包 | 状态 | 证据与剩余工作 |
|---|---|---|
| A0 当前代码与验证基线 | 部分完成 | 初始 153/153 JS、原生完整构建、movement/physics/animation 原生测试、粒子 GPU 21/21 通过；新增模块另有来源清单；仍需冻结标准负载下完整时序 |
| A1 时钟、RHI 与异步设施 | 部分完成 | 整数有理时钟已通过不同渲染频率/追帧债务/暂停/恢复检查；尚未替换既有宿主时钟。GPU 写入派发参数并 ExecuteIndirect，旧帧非阻塞校验与独立 compute 时间戳已在宿主通过；活跃列表和通用 RHI 资源接口仍缺 |
| A2 资产与局部热重载 | 部分完成 | 效果资产校验/SoA 布局编译/依赖 hash/原子 cook 发布已实现；原生 /2 认证载荷加载、实例代际、兼容布局保留与显式重置已接入 CPU 参考；宿主编辑器事务、GPU 状态迁移未实现 |
| A3 材质前置 | 部分完成 | 布料路径的 UV/tangent、双面法线与 sheen 已有解析像素/实际绘制证据；全部绘制路径、完整透明/材质一致性仍待验收 |
| B 场核心 | 部分完成 | CPU 租约/分源事务、LEF/WF 投影与观察参考，D3D12 稀疏只读快照、CPU/GPU Q8 三线性、读取 halo、布料风与水代理，以及 GPU 粒子宿主发布/只读速度消费和作者预览源已有；新增 GPU 私有量化 LEF 粒子注入、不可变粗投影/损失诊断，以及 GPU 视觉 WF 分源替换/撤销、稀疏快照聚合与版本查询；新增 GPU 可观察性共享控制器的迟滞/驻留/稳定配额与提交确认。已有发布事务的租约/来源/共享状态回滚、预算恢复、移动覆盖交接与无数据页撤销确认。已有逐来源投影前预算预估、可承受候选继续更新、等待排序与来源积压/超期诊断。另有求解后烟密度的数值桥和自动共享服务；GPU 页池/多级分辨率、流体写求解/迭代 halo、完整宿主调度、玩法权威接入与存档仍缺 |
| C 通用粒子 | 部分完成 | CPU 多实例/事件/碰撞和 11 个生产精灵资产已有；D3D12 SoA 池、实例管理、共享预算、异步回收、wind/flow 只读速度模块，以及宿主 GPU 模拟/场绑定/汇集/全局透明排序/间接精灵绘制已有。已有直接读取 GPU 池的 LEF 写入/投影原生适配，新增认证 fieldDeposit 资产、实例后模拟写回调与生产 LEF/WF 服务/renderer 接入；已增加发射前私有资源预留、移动拒绝与输入背压恢复；联合预算/动态降档与 solver 调度、GPU 热迁移、threshold、场景/编辑器生产者、其余渲染器与工具仍缺 |
| D 烟火与统一体积 | 部分完成 | CPU/GPU 求解、不可变粒子源、认证资产/实例服务、宿主调度与光学已有；密度已接入自动共享、预算延期与按源 LEF/WF 二选一。新增 froxel 入射光缓存/像素区间积分，真实 8→1、小烟域、背景雾及重建共 33 场景/134 检查通过。烟服务使用独立视觉 WF；统一全局服务、温度/速度联合共享、任意网格重采样、云烟完整混合、动态时域/热浪、玩法适配及联合预算仍缺；见 [共享](SMOKE-SHARING.md)、[实际渲染](SMOKE-RENDERING.md) |
| E 布料 | 部分完成 | /2 CPU/GPU、离线着色、静态接触、共享风与真实 GPU 绘制/宿主生命周期已有；新增 ≤512 点组内完整 tick、严格浮点编译、原派发回退及配对计时。粒子与布料已接入同一宿主只读风快照；四系统预算、自碰撞/CCD、骨骼/渲染映射、LOD、温度/湿润/水耦合、完整材质和编辑器仍缺；见 [求解](CLOTH-GPU.md)、[绘制](CLOTH-RENDERING.md)、[组内优化](CLOTH-GROUPED.md) |
| F 水 | 部分完成 | 解析/谱求和/浅水 CPU 后端、资产、守恒、查询与浮力已有；新增高度/深度/XZ 流速四通道原子权威投影及 source 隔离。PBF、GPU 求解、水面/水下渲染、湿润与双向反馈仍缺 |
| G 工具与整合 | 部分完成 | 版本化验收与运行包效果依赖已有；simulation:suite 现汇总五类原生参考与资产合同（6/6，51,907 次原生检查）。三类创作面板、场调试、完整联合场景和完整生产库未实现 |

2026-10-26 场整合增量：真实 GPU 粒子与烟求解输出已在一个有类型目录/视觉世界场内共同提交，并具有全部参与者回滚、未参与者/原始写接口拒绝和全局只读订阅需求查询。粒子沉积与烟共享生产服务都已新增外部世界场、显式通道及本地准备/暂存/确认阶段；真实 runtime 双用途资产同 tick 联合 2 组/124 检查通过。共同 renderer 调度、全局新视图失效与 CPU 权威尚未迁移，B 仍为部分完成。详见 [共同发布](FIELD-JOINT-PUBLICATION.md)、[粒子阶段](FIELD-DEPOSIT-PHASES.md)、[双服务联合](FIELD-SERVICE-JOINT-PUBLICATION.md)。

开发原则：保持现有游戏路径可运行；新增模块未接入宿主时明确记录；不得把独立单元测试当成 GPU/画面/性能验收。基线报告包含实际源码指纹，避免脏工作区下仅凭 commit 标识误认二进制来源。

**2026-10-27 本轮收尾**：共同场协调器及按区域/通道的新视图退休路由已完成，当前原生 3 组/103 检查、22 次 GPU 比较通过；renderer 已接线。旧编译宿主两档实际绘制/烟 WF 光学通过，但快速检查发现跨时刻统计不一致，源码已修正、运行未复验；最新宿主构建被并行资产 package 头文件路径错误阻断，未记为通过。完整边界和证据见 [协调器收尾](FIELD-EFFECT-COORDINATOR.md)。本轮没有全量回归，A–G goal 保持 active。

## 首批可重放入口

```powershell
npm run simulation:baseline
npm run fields
node --test tests/effects.test.mjs
npm run effects -- validate --in assets/effects/spark_reference.effect.json
npm run effects -- cook --in assets/effects --out build/effects
```

`fields` 在运行前校验源码与二进制 hash，拒绝过期二进制；证据在版本化 simulation 目录。首批 8 组专项、21,504 次检查包括 10,000 次租约循环与 100 次权威摘要对拍，不是 21,504 个独立测试场景；最新投影参考已扩展到下文的 20 组。JSON 报告始终明确 `gpuIntegrated=false`、`hostIntegrated=false`。

`World` 当前限定单线程拥有者；commit 是该线程内的可见性屏障，不提供多线程并发读取或任意历史 tick 的 MVCC。未知/过期查询返回显式状态。同分辨率插值已由下文增量实现；CPU 跨线程读取与多分辨率覆盖仍待开发。

效果 cook 的 `runtimeIntegrated=false` 表示离线工具不会激活运行时，不是某个后端对全部资产的支持承诺。宿主当前只允许下述 CPU 参考支持的资产，其余类型显式拒绝。编译声明某类 renderer 不代表对应 GPU renderer 已实现。依赖字节改变会改变 packageHash；同内容重编得到相同产物；非法修改不替换已发布 library.json。作者层 `preserve-layout` 只表示布局兼容；原生实例层的布局兼容替换另有专项测试，GPU 求解历史迁移仍未实现。

## 首批原生接入证据

新增 `shaders/particle_dispatch.hlsl` 在 GPU 写入 DISPATCH 参数，粒子模拟改用 ExecuteIndirect，派发控制不依赖 CPU 读回。当前 work count 来源仍是既有固定容量，不是已经完成 GPU alive/dead 紧致。报告用 `countSource=configured-capacity` 区分。

计算队列独立计时覆盖 simulate+splat+resolve+solver；时间戳和参数通过三帧环形缓冲，在 fence 已完成时才映射，禁止同步等待获取遥测。首轮专项 25/25 通过：80 次间接派发、77 次参数读回、零不匹配；on/off/no-colliders 图像 hash 与迁移前一致。随后扩展专项为 27/27，含启用粒子的 5 次渲染器重建，每次 shutdown 后 live GPU objects=0；证据与三张截图已归档版本目录。

这次短测 compute 平均约 6.96 ms、最后样本约 9.28 ms，场为 96³；不是固定标准负载下的 P95 或整帧帧时。既有 graphics 命令列表时序未包含前置 compute 等待，不能把 graphics 数值单独当作整个粒子/场成本。后续预算接入必须测队列临界路径与重叠，不能直接把不同时刻的 lastMs 相加。

当前 JavaScript 161/161（新增 8 个效果资产专项）通过；原生完整重建成功，38 个 shader 编译；13 个原生测试程序全部通过（含 gameplay/integration、simulation、physics、destruction、animation、scene、audio、budget、RHI、renderer math、fogtown 与 fields）。源码/构建 manifest 为零不符、零未登记原生输入。

本批全量 `.incrementalTesting/core/verify.ps1 -SkipBuild` 为 **25/25**，报告：`reports/0.11.0/31c00bc53c0b/20261022T074644Z/verify.json`，包含图像合同/回归、回放、资源生命周期、设备注入恢复和打包。随后独立运行扩展粒子专项 **27/27**；已把该专项加入后续 verify，因此新版 SkipBuild 门禁应为 26 步，不能将本次旧 25 步报告改写为 26 步通过。CPU 参考证据示例：`reports/0.11.0/31c00bc53c0b/20261022T074428Z/simulation/fields-reference-evidence.json`。

## 原生效果实例与精灵增量

`engine/native/effects` 增加经 SHA256 验证的 /2 cook 载荷、依赖锁定和原生实例池。整数发射相位在固定 120 Hz 下消费；独立随机流、局部/世界坐标、暂停、移动、自然耗尽、立即停止、代际句柄与预算丢弃有数值参考。兼容布局替换保留现有粒子，不兼容替换要求调用方显式重置。它是 CPU 参考与回退后端，不能作为 GPU alive/dead 列表完成证据。

`renderer/effect_sprites.cpp` 与 `shaders/effect_sprites.hlsl` 实现 GPU 非受光精灵：纹理依赖、相机朝向四边形、加法/预乘/裁切三种混合、预乘深度排序、三帧独立上传缓冲和深度测试。该通路由宿主 `--effect sprite_demo --effect-count 4` 启用，按游戏 tick 推进；预览效果尚未纳入 Game 存档/摘要与编辑器事务。GPU 精灵渲染和 CPU 粒子模拟在报告中分别标注。

加载器接受源 schema `/1` 经离线生成的 cook `/2`，旧 cook `/1` 需要重编。外层重复元数据不作为认证源，以 `payloadJson` 的原始 UTF8 字节验证 hash，避免跨语言浮点 JSON 格式差异。原生支持集合之外的场写入、事件、网格发射、受光精灵及非 sprite renderer 在 spawn 时返回明确错误，不会静默降级成其他效果。

复现入口：

```powershell
npm run effects:runtime -- -SkipBuild
```

专项 **23/23** 已通过，报告：`reports/0.11.0/31c00bc53c0b/20261022T104733Z/effects-runtime-evidence.json`。包含原生 7 组/13,199 次数值检查（100 次确定性重复、128 实例）、实际截图、停止恢复基线、同 seed 重现、三种 blend、启用效果的 5 次生命周期重建和 resize/restart 后继续绘制 80 帧。5 次 shutdown 的 tracked GPU objects 均为零。计数是检查/重复次数，不是 13,199 个独立场景。两份数值/场参考和一份连续精灵演示不是生产效果库。

重建专项在第 5 次重新初始化后立即退出，最终 draw 计数因此为零；专项确认该时刻纹理与管线已建立，另用常规压力运行确认重启后真实绘制，避免把资源可用误写成已经绘制。原生预览曾发现初始化的 `shutdown()` 重置丢弃效果纹理配置，现已按原有材质配置的生命周期保留，并通过上述重建检查。

运行包增加 `effects/library.json`、被引用的不可变 cook 对象及 hash 锁定纹理依赖。包内入口为 `NativeGPU.exe --effect sprite_demo --effect-count 4 --effect-library effects/library.json`；该入口仍是独立预览，不等于场景组件或编辑器生产工作流已完成。

本轮完整构建成功（40 个 shader blob）；修复配置保留后重建原生程序，源码/构建 manifest 零不符、零未登记原生输入。JavaScript **161/161**、独立原生 **15/15 个测试程序**通过；原生日志及二进制指纹在 `reports/0.11.0/31c00bc53c0b/20261022T105444Z/`。本轮全量 `.incrementalTesting/core/verify.ps1 -SkipBuild` **27/27** 通过，报告为 `reports/0.11.0/31c00bc53c0b/20261022T104700Z/verify.json`，包括新增效果专项、既有粒子 27 项、50 次默认资源重建、设备注入恢复、严格回放和图像回归。前述首批 25 步报告保留其历史口径。

上述 27 步属于当时构建的历史证据。独立运行包 **116 个文件**校验无误，包内精灵绘制 240 帧，截图与源码目录精确相同；报告为 `reports/0.11.0/31c00bc53c0b/20261022T105751Z/effects-package-evidence.json`。该版本的 zip 已冻结到 `reports/0.11.0/31c00bc53c0b/20261022T110042Z/artifacts/NativeGPU-effects-validated-win64.zip`。回归期间同工作区的物理源码又被更新，后续一次全量重建也因编译中输入变化而失败，不能将旧报告套到这些新输入上。

## LEF/WF 投影与观察参考增量

契约与复现用法见 [FIELD-REFERENCE.md](FIELD-REFERENCE.md)。`local.hpp/.cpp` 保留 LEF 的求解权，只发布带 source/epoch 的世界粗投影。局部未被共享时不申请 WF 页；共享移动以同一事务撤销旧足迹并写入新足迹，远距离移动不分配中间空白页。预算不足保持原来的局部位置、版本与旧投影。

`World` 增加多通道原子批次、分开的 sourceTick/publishTick、已提交来源版本与单来源采样。空撤销也受提交屏障约束；延迟数据不会伪装新鲜度。来源墓碑按自身写租约代际保活，修复同 owner 常驻效果让其他已结束来源记录不能回收的问题。

观察参考包含订阅/worldVisible、分离阈值、最短驻留、每 tick 提升配额与稳定优先级；CPU 路由每个来源只选择局部或世界表示之一。投影只支持声明的非负密度平均与最大值通道；量化残差进入结果，温度/速度加权与真正守恒求解权转移仍待实现。这些状态机和 fence 账本不是实际 GPU 同步或覆盖遮罩。

专项 **20 组、37,252 次检查**通过，报告：`reports/0.11.0/31c00bc53c0b/20261022T113303Z/simulation/fields-reference-evidence.json`。包含新增 100 次观察排列对拍、1,000 次投影生命周期，以及常驻来源存在时额外 1,000 次短生命周期；和首批 10,000 次租约循环、100 次权威聚合顺序对拍一起运行。独立编译的输入不依赖物理模块，来源和二进制 SHA256 一致。GPU/宿主集成仍为 false。

构建门禁同时补强：同一输出目录禁止并行原生构建；building/failed 状态拒绝当作验证产物；成功后记录源码清单与全部可执行文件 hash；打包、启动、原生专项复用该检查。全量验证在结束时再次核对来源，防止运行期间发生的并行编辑被算成同一版本通过。新版 SkipBuild 为 28 步，需新一轮真实报告，不能把旧 27 步改写为 28 步。

该批已验证门禁的失败/篡改路径：`reports/0.11.0/31c00bc53c0b/20261022T113906Z/native-provenance-evidence.json`；另有真实在途构建期间的打包拒绝和并行构建拒绝，既有 zip 未被覆盖。当时新版 28 步全量验收被中断，没有完整成功报告；不能视为新工作树通过。

后续先将页/来源/快照版本接入 GPU 稀疏存储并与 CPU 参考对拍，继续 A1 通用资源/异步队列与帧时归属、C 的 GPU 通用模拟及游戏/编辑器接入、B 的权威 tick/存档/运行时适配，逐步替换单源 legacy 场。独立参考不能作为 GPU 场、布料或水的完成证据。

## 布料与水 CPU 权威增量（2026-10-22 晚）

本轮把 E、F 两阶段从“未开始”推进到“CPU 权威参考可用”。范围是资产合同、离线烘焙、确定性求解与数值证据；GPU 求解、渲染、宿主与编辑器仍未接入，因此 E、F 不能标为完成。

### 布料

- 资产：`assets/cloth/*.cloth.json`（`nativegpu.cloth/1`）→ `tools/cloth.mjs` 烘焙 `nativegpu.cloth-cook/1`，SHA-256 内容寻址。库 hash `9804d4d3b09bf694df84a145c78115338974f8db87344b180f0a9b7e0586844b`，包 hash `c42c9554fa72fff21336d04cef267cb4d45cacf65f7b931ec859f17bb27a3f2e`。
- 资产：`flag_banner`、`canopy_tarp`、`curtain_drape`、`cape_cloak`、`skirt_panel`，共 1,448 顶点、7,858 约束、183,056 字节求解池。烘焙期生成静止位置、三角形、按面积分摊的顶点质量、拉伸/剪切/弯曲三族约束与锚点；运行时不重新推导拓扑。
- 求解：`engine/native/cloth/solver_cpu.*` 为 XPBD 膜求解，固定 tick、子步上限、λ 累积、风按 `0.5·ρ·Cd·A·|w−v|²`、平面/球/胶囊碰撞、传送清零速度、`setSelfCollision(true)` 显式失败。
- 证据：原生 7 组 150 项，含 1 m 方布 5 s 边长误差 P95 ≤5%、接触穿透 P99 ≤2 mm/最大 ≤10 mm、锚点零漂移、100 次重放 digest 一致；`node --test tests/cloth.test.mjs` 6 项。
- 文档：[CLOTH-REFERENCE.md](CLOTH-REFERENCE.md)。

### 水

- 资产：`assets/water/*.water.json`（`nativegpu.water/1`）→ `tools/water.mjs`。库 hash `85714aefd1dcfac63172e711f513aad8c3b30bcaa61e40804eca7bb5099a3048`，包 hash `a4f96a744871326107851a49b9a4f3ab2358974364a4705a6082bcf5a7b14f6a`。
- 资产：`pond_still`、`well_water`（解析）、`river_shallow`（浅水）、`coast_spectrum`（谱求和，`fft=false`），共 36,416 单元、728,320 字节状态。`pbf` 在编译与原生加载两处都返回 `NG-E-WATER-UNSUPPORTED`，不产出不可用资产。
- 求解：`engine/native/water/body.*` 为守恒有限体积 + 静水重构 + Rusanov 通量，CFL 子步、封闭墙/透射边界、入流出流账本、干湿钳制、双线性查询与足迹采样浮力。
- 关键缺陷与修正：封闭墙最初把质量与动量通量同时置零，边界静压失衡导致静水池被持续泵高（60 s 后水深从 1 m 涨到 100 m，质量仍守恒，因此只查守恒不会发现）。现按 `0.5·g·h²` 保留墙面压力项，静水 60 s 漂移为 0，并保留为回归。
- 证据：原生 9 组 207 项，含封闭 10 m 水池 60 s 质量误差 ≤1%、静水漂移 ≤1 mm、溃坝质量守恒与前沿推进、排水体积误差 ≤5%、域外/NaN 显式 invalid、100 次重放一致；`node --test tests/water.test.mjs` 4 项。
- 文档：[WATER-REFERENCE.md](WATER-REFERENCE.md)。

### 集成

- `tools/build.ps1` 现在先烘焙布料与水资产，并新增 `cloth-tests`、`water-tests` 两个原生目标；`.incrementalTesting/core/test-native.ps1` 相应增加到 15 个测试程序。
- 共享 SHA-256 移到 `engine/native/assets/sha256.*`，效果、布料、水三套 cook 容器复用同一实现；效果参考在重构后仍为 7 组 13,199 项通过。
- 新增 `npm run simulation:suite`（`.incrementalTesting/simulation/simulation-suite.ps1`）汇总四类 CPU 参考与资产合同：**5/5**，50,808 次检查（效果 13,199、场 37,252、布料 150、水 207），JS 合同 18 项。报告：`reports/0.11.0/31c00bc53c0b/20261022T142427Z/simulation-suite-evidence.json`。
- JavaScript 当前 **177/177** 通过（本轮新增布料 6、水 4；其余增量来自并行编辑，不归入本轮）。
- 完整原生构建（`-SkipShaders`，复用已编译 41 个 shader blob）成功，版本 0.11.0、commit `31c00bc53c0b`、dirty 1。

### 未完成与风险

- E：GPU 约束分组/自碰撞/CCD、蒙皮与绑定空间、渲染网格重心映射、法线切线、双面织物材质、宿主预览与存档、编辑器面板。
- F：局部 PBF 与表面重建、GPU 浅水/FFT 海面/尾流、水面与水下渲染、湿润与火/导航/声学联动、宿主与编辑器。
- D（烟火独立 solver 与统一体积）与 C 的 GPU 通用模拟、五类渲染器、生产效果库仍未开始或部分完成，G 的创作面板与联合场景未实现。A–G 整体仍未完成。
- 本轮验证时点，`.incrementalTesting/core/test-native.ps1` 在 `native_destruction` 的存档恢复断言（`joints == 1`）失败；该程序只链接 game/physics/animation/determinism，与本轮布料/水/效果改动无交集，同期工作区中的 `game.cpp`、`physics.cpp`、`gameplay_snapshot.cpp` 正被并行编辑。不能据此宣称 15 个程序全部通过，也不能把失败归给已通过的四个参考。

## 粒子事件组合与生产效果库增量（2026-10-22 深夜）

本轮补齐 C5 的事件组合与 C2 的静态碰撞，并把效果库从 3 个参考资产扩充到 13 个（其中 11 个可生成）。仍不包含 GPU 通用模拟与其余五类渲染器。

- 事件：`spawn`、`collision`、`death`、`timeline` 四类已实现；`threshold` 因 schema 没有比较值参数，在 spawn 时显式返回 `NG-E-EFFECT-UNSUPPORTED`，不会静默当作无条件触发。
- 顺序与延迟：事件按实例槽位、粒子索引、规则数组顺序产生；子效果在下一个 tick 边界派发，`delayTicks` 作为额外延迟，`timeline` 的 `delayTicks` 是标记时刻而非延迟。
- 预算：`maxEventsPerTick` 限制每实例每 tick 的子效果生成数，`maxDepth` 限制嵌套深度，`limit` 限制单条规则每 tick 的生成数；被拒绝的生成计入 `eventsDropped`，不会静默丢失。
- 碰撞：`setColliders` 接受球与盒，最多 64 个；每 tick 在提交位置上检测一次，法向冲量按 0.25 恢复系数反射，碰撞事件带法向与接近速度；无效碰撞体直接拒绝。
- 库：`assets/effects` 现有 13 个资产，库 hash `9f8231a63d0674cf0a6906246499b55730dbd7bf12e37e4ddd8507e10a6fc917`，包 hash `ac02f1d88e4d41ba0a66f17b65adb791aca135742f873d541189f0f2b439eac3`，求解池 1,166,880 字节。新增 `sparks_burst`、`explosion_core`、`muzzle_flash`、`campfire_loop`、`steam_vent`、`dust_puff`、`blood_spray`、`rain_loop`、`snow_loop`、`leaves_drift`。
- 证据：原生效果专项扩至 **9 组 13,360 项**（原 7 组 13,199 项），新增生产库可生成性、爆炸 death→sparks 组合、枪口 spawn→sparks 单次、碰撞反射与事件法向、时间线单次触发、死亡风暴预算与丢弃计数、100 次事件图重放。
- 未完成：`ribbon`/`beam`/`mesh`/`light`/`decal`/`volumetric` 六类中的五类渲染器、受光精灵、GPU 通用模拟与 alive/dead 紧致、场写入适配、效果编辑器事务；效果库 11/16，缺刀光、弹道尾迹、魔法光束、水花、命中火花等需要 ribbon/beam 或事件的条目。
- 该增量未改变 GPU 宿主行为：宿主仍只绘制 unlit sprite，事件产生的子效果同样是精灵，因此预览可组合但渲染器种类未增加。

## GPU 场读取与布料/水适配（2026-10-23）

接续用户新增的 CPU 布料、水和粒子事件实现，本轮推进 B3/B5、E3/F 的共享访问部分，保留各求解器自己的状态与所有权。详细合同、边界和复现入口见 [FIELD-REFERENCE.md](FIELD-REFERENCE.md)。

- CPU：sampleLinear/sampleSourceLinear，以完整 Q24 权重累加后一次有符号取整；只读 halo 纳入租约页预算，查询中心不能借用 halo 权限。任一有权重角点未知则返回 Unavailable，过期角点保留 Stale，sourceTick 使用最旧参与角点。专项 **23 组、37,290 次检查**通过；`reports/0.11.0/31c00bc53c0b/20261023T045841Z/simulation/fields-reference-evidence.json`。
- GPU：GpuFieldStorage 的真实 D3D12 不可变只读稀疏快照，重叠租约共享页表/描述符，支持权限、known/clamped、64 位 tick、旧快照与真实 fence 延迟回收；shader 公共读取函数支持整数与 Q8 三线性。使用相邻 16³ 整页覆盖读取 halo，不是 18³ 算子副本或迭代 halo 交换。
- 实机：AMD Radeon(TM) 890M Graphics，**7 组、25,760 次检查、4,150 次 CPU/GPU 对拍**；debugLayer=true、debugErrors=0，含 4,096 个分数位置与完整 int32 极值。报告：`reports/0.11.0/31c00bc53c0b/20261023T045142Z/fields-gpu-evidence.json`。DXGI 仅枚举 AMD 与软件适配器，未宣称 NVIDIA 通过。
- 布料：显式米制网格和 m/s 三分量，逐顶点读取后整批替换风输入；缺失/旧数据/部分覆盖不改原输入，常量设置/reset/传送清除空间样本。digest 升到 `/2`，包括空间风输入。权威读取要求 CPU 域和指定 tick 的新鲜数据，不通过 Stale 租约绕过。
- 水：高度、深度、X/Z 流速在单水平层按 source/epoch 原子发布；CPU 权威、单位、量化和采样预算均验证。重叠水体先由对象层选 Body/source，再按来源查询；不插值对象 ID，不把代理当成水量转移。Body 本地 tick 与调度器映射的来源/发布时间分别记录，函数不隐式 commit。
- 耦合专项：**5 组、900 次检查**通过，含 120 tick 实际布料求解与常量风对拍、失败保留、传送清理、四通道回滚、来源隔离和 32 次水场重放。报告：`reports/0.11.0/31c00bc53c0b/20261023T045946Z/field-adapters-evidence.json`。
- 新增 `npm run fields:gpu`、`npm run fields:adapters`；CPU 独立入口可用 `npm run fields -- -OutputDirectory build/fields-reference`。完整构建与原生门禁加入耦合程序，simulation:suite 加入第五类原生参考；支持独立输出目录，以便保留正在使用的旧预览程序。

GPU 快照是读取镜像，没有 GPU 求解器写入或 CPU 权威回写。多级分辨率、solver halo、实际守恒接管、烟火统一体积、粒子场模块、GPU 布料/水、宿主/存档与创作面板仍缺，不能用这些参考专项宣称 A–G 完成。

整合结果：独立 `build-simulation` 完整原生构建通过，复用已编译的 41 个 shader blob；**20/20 原生测试程序**（含用户新增物理/关节/断裂及销毁存档路径）、**177/177 JS** 通过。`simulation:suite -SkipBuild -OutputDirectory build-simulation` 为 **6/6**，五类原生检查合计 **51,907**，资产合同 18 项；报告：`reports/0.11.0/31c00bc53c0b/20261023T050539Z/simulation-suite-evidence.json`。旧版销毁存档失败不再复现。

新预览程序同时启用 legacy GPU 粒子与四实例精灵，实际各绘制 **240 帧**，GPU 间接参数不匹配为 0；随后实际 5 次重建的 tracked liveObjectsAfterShutdown 均为 0。重建报告结束在重新初始化后，最终 draw 计数为 0，不将其称为重建后已继续绘制。截图位于 `reports/0.11.0/31c00bc53c0b/20261023T050646Z/combined-preview.png`。GPU 场独立报告与整合源码的公共输入 SHA 已逐项一致性核对。

封存汇总：`reports/0.11.0/31c00bc53c0b/20261023T050909Z/simulation-finalization.json`，连同源码/二进制来源清单、专项报告与原始日志索引。此处是增量整合和联合启动/生命周期检查，不是新版 28 步 full verify，也不是 A–G 完成证据。

## 通用 GPU 粒子池增量（2026-10-23）

`engine/native/effects/gpu_pool.*` 与六个 `effect_pool.hlsl` compute kernel 新增真正的视觉 GPU 模拟底座。七属性 SoA 按 cook 对齐规则分配，双存活列表紧致、空闲列表回收、GPU 发射容量预留与计数、ExecuteIndirect 模拟/发射、GPU 绘制参数均已实机执行。这里的 work count 来自 alive/free 计数，与 legacy configured-capacity 派发明确区分。

支持点/球/锥/盒/线，initialize/acceleration/drag/颜色与大小曲线/现有 curlNoise 形式，局部与世界坐标、立即清空、自然排空及读取/计算状态切换。CPU 只提供固定 tick 的请求序号与发射请求，库没有等待或读回；GPU 接受量由容量决定，掉落请求不改变后续随机序号。GPU 顺序不保证逐位确定性。

`npm run effects:gpu -- -Adapter AMD`：**6 组、41,592 次检查、388 次 CPU/GPU 粒子状态对拍**通过，AMD Radeon(TM) 890M Graphics，debugLayer=true、debugErrors=0；报告 `reports/0.11.0/31c00bc53c0b/20261023T052245Z/effects-gpu-evidence.json`。包含 4,097 容量的 128 tick 回收、五形状、跨 2³² 序号随机流、UINT_MAX 请求与饱和统计、同 tick 回收再发射、真实 queue gate 下的提前释放拒绝。同步读回只存在于验证程序。

完整合同与已知限制见 [EFFECT-GPU-REFERENCE.md](EFFECT-GPU-REFERENCE.md)。这还是每发射器池，尚无 GPU Asset/实例管理器、跨发射器 maxAlive/maxSpawnPerTick、宿主 GPU 绘制、碰撞/事件队列、场绑定或 GPU 热迁移。原宿主的 CPU Runtime + GPU sprite 路径保持原 backend。spawnId 当前只存低 32 位，长周期精确统计与完整稳定标识仍待实例层补齐，不能当成 C 阶段完成。

标准构建新增 `native_effect_gpu_tests.exe`，`test-native -IncludeGpu` 和 full verify 的 Native tests 门禁执行该程序。构建来源清单与未登记输入检查增加 tests 目录，覆盖实际编译的测试源和头文件，避免只绑定引擎源码却遗漏验收本身。新 GPU 代码使用 6 个额外 shader blob，总数变为 47；本轮整合验证结果另记。

随后扩展到完整一小时寿命区间时，实际复现 GPU float 区间乘法向上舍入，让部分粒子比 CPU 多存活一 tick。改为精确 24 位随机整数 × 区间长度再右移，保持 CPU double 的 floor 结果。专项现为 **7 组、49,793 次检查**（仍含 388 次运动/属性对拍），新增 4,097 个跨 2⁴⁰ 序号的长寿命样本全部一致；报告：`reports/0.11.0/31c00bc53c0b/20261023T053350Z/effects-gpu-evidence.json`，修正前失败的可执行文件、shader/字节码和日志在其 `regressions/lifetime/` 冻结。旧六组报告不改写为七组。

该轮随后完成的历史整合为 **21/21 原生程序、177/177 JS**，47 个 shader blob；冻结汇总位于 `reports/0.11.0/31c00bc53c0b/20261023T054035Z/effects-gpu-integration-evidence.json`。这发生在用户指定“仅增量回归”之前，不是以下新代码的全量验证。

## GPU 粒子实例与共享预算增量（2026-10-23）

新增 `engine/native/effects/gpu_runtime.*`：不可变资产注册、多实例/多发射器、generation 句柄、120 Hz 整数 rate/burst/循环、暂停/恢复、局部/世界移动、停止发射与立即停止。现有 CPU 资产语义接入 GPU 池，随机流按资产/实例/发射器/完整请求序号派生；预算丢弃不会移动后续随机序号。热重载尚未接入，替换已注册内容显式拒绝。

实例与全局分别有 GPU 共享 ledger，限制 maxAlive 和 maxSpawnPerTick。所有池先模拟/回收，再按实例槽与作者发射器顺序预留发射量，避免较早实例看不到较晚实例同 tick 释放的容量。预算按实际接受数消耗；库不等 GPU 来决定派发。新增第七个预算 kernel，manifest 总计 48 个 blob，但本次仅编译 `effect_pool.hlsl` 的 **7 个**，独立 header 位于 `build/effects-gpu/generated/`，没有重编其他 shader 或全量宿主。

遥测使用固定三槽异步 readback，仅映射 completedFence 已完成的资源；满环时跳过本次遥测。自然停止按完成的空池快照回收，快照携带 generation 和出生序列，防止同 fence 的旧空结果误杀后来发射的粒子。立即停止先使句柄失效，再由 flush/step 记录 GPU clear；旧池与实例 ledger 一直保留并计入内存预算，直到真实 fence 完成。实际 buffer 分配统计包括全局/实例 ledger、三份 readback 和每池九份资源的对齐分配；不含驱动管线内部内存。

增量入口：`npm run effects:gpu -- -Adapter AMD`；仅实例专项可加 `-RuntimeOnly`，复用产物可加 `-SkipBuild`，来源不一致时拒绝执行。工具冻结测试二进制、编译日志、结果、shader 清单和源码/二进制 SHA，并在编译/运行/两个目标之间核对输入。

最新证据：`reports/0.11.0/31c00bc53c0b/20261023T060229Z/effects-gpu-evidence.json`。

- GPU 池：**8 组、49,842 次检查、388 次 CPU/GPU 状态对拍**，增加共享 ledger 接受量、模拟/发射分阶段状态拒绝与回收后再预留。
- GPU 实例：**7 组、1,158 次检查、219 次 CPU/GPU 位置对拍**，含 rate/burst/循环/暂停/移动/停止、两层预算、同 fence 空快照、真实 queue gate、三槽耗尽、预算计入在途资源和旧句柄拒绝。
- **128 个混合实例、18 tick、累计 175 次创建**，反复立即停止/重建，存活数/分配上限与账本一致；结束后实例资源归还。该短时压力不等于长期性能或整帧预算验收。
- AMD Radeon(TM) 890M Graphics；两个程序均 debugLayer=true、debugErrors=0。本轮没有执行全量原生、JS 或 full verify。

仍为独立 GPU 视觉实例后端，宿主继续使用 CPU Runtime + GPU sprite。GPU 模拟后的实际绘制、透明排序、热迁移、碰撞/事件、场读写、完整 64 位粒子标识、长周期精确遥测与性能归属尚待开发；不标记 C 或 A–G 完成。标准构建和可选 GPU 门禁增加实例测试目标，但本轮未运行这些完整入口。

## GPU 模拟粒子的实际绘制与宿主增量（2026-10-23）

新增 `renderer/gpu_effect_sprites.*`、`gpu_effect_bridge.cpp` 和九个绘制相关 shader blob。直接从 GPU SoA/存活列表汇集记录，GPU 计数驱动 ExecuteIndirect；跨实例、发射器和贴图的预乘透明按 z/w 排序，cutout/预乘/additive 分三段间接绘制。每记录保存贴图索引，PS 使用 NonUniformResourceIndex，避免按贴图分批破坏透明顺序。没有 CPU 粒子位置读回或逐粒子上传。绘制容量、来源数量、输入容量之和与实际内存均有上限；详细状态与失败合同见 [EFFECT-GPU-RENDERING.md](EFFECT-GPU-RENDERING.md)。

宿主新增 `--effect-backend gpu`，默认 CPU 路径仍保留。帧回调将逻辑创建/停止与 fixed tick 按原顺序记录到 renderer 的图形时间线；renderer/设备重建时视觉 seed 重启，不改游戏权威。模拟/汇集/排序及绘制分别有已完成 fence 下读取的 GPU 时间戳，并在整帧预算区间内。GPU 活跃热迁移与完整编辑器控制桥未实现。

真实像素夹具复现了混合模式分段偏移问题：cutout 被裁剪后，后面的预乘绘制读错分段，图像缺少粒子。现由 GPU 显式保存分段偏移，VS 与间接参数不再混淆索引语义。旧可执行文件、shader、测试与失败日志在 `reports/gpu-effect-draw-failure-20261023/` 冻结，旧二进制实际在第 575 次检查失败。还修复宿主初始化丢失 validation 开关，以及覆盖效果专用错误码的问题；不能把此前传入 --validation 但 validationLayer=false 的结果当作开启调试层的证据。

最终报告：`reports/0.11.0/31c00bc53c0b/20261023T063945Z/effects-gpu-draw-evidence.json`。入口为 `npm run effects:gpu:draw -- -WithHost -Adapter AMD`，复用产物加 `-SkipBuild`。上一轮完成后材质/mesh shader 与主渲染源码又被更新，已保留并重新构建受影响依赖、重跑相同范围；宿主全部源码输入另冻结到报告目录。

- 独立绘制 **5 组、604 次检查**，257 条记录跨实例/贴图全局排序、16 项实际解析像素对拍、真实 VS/PS 查询、空帧/溢出/局部坐标/分配与 fence 拒绝；AMD 890M、debugLayer=true、debugErrors=0。
- 入口 **25/25 项检查**通过，包含专项结果与 off/CPU/GPU/三种混合/停止/恢复/拒绝路径。正常宿主运行均实际启用验证层，errors=0；off 723 条 warning、GPU 760 条 warning，保留原始信息，没有把性能提示当作错误或隐去。
- GPU 实际绘制 **240 帧**，效果改变截图；立即停止后截图 SHA 与 off 完全一致。GPU 模式 CPU sprite 上传绘制帧为 0；默认 CPU 模式仍绘制 240 帧。一次 resize 和 renderer 重建后继续绘制 80 帧。
- GPU 记录 237 次已完成计时样本，最新模拟/排序 0.70008 ms、绘制 0.00216 ms；这里只证明时间归属，未声称高负载帧预算通过。
- 专项仅编译受影响的两个 shader 文件，共 16 个 blob。宿主依赖构建使用全部 57 个 blob，不构建其他测试、不运行其他回归。C++ 对象缓存按真实编译依赖 SHA 校验，最终重编 10 个、复用 23 个对象；源码/二进制、独立宿主、截图均冻结到报告目录。

本轮未运行全量原生、JS、完整 simulation:suite 或 full verify。C 仍部分完成，布料/水 GPU 求解、统一体积、场读写调度、其余 renderer、热迁移、创作工具与完整联合场景等原始 A–G 目标继续保留。

## GPU 粒子只读风/流场增量（2026-10-23 晚）

`velocityField` 模块与 `fields/gpu_vector.*` 将 GPU 粒子接入共享场读取快照：显式米制原点、网格大小、三个 m/s 标量分量与权限，复用 Q8 跨砖/halo 采样。逐模块按指数响应改变视觉速度；缺分量、Eventual 过期和越界时保留原速度并计数，Stale 明确允许的旧值另计。CPU 权威数据只镜像，GPU 不写回。完整合同、调用顺序和限制见 [EFFECT-FIELD-CONSUMER.md](EFFECT-FIELD-CONSUMER.md)。

fieldTick 独立于效果年龄，含场实例必须提供与不可变绑定匹配且不倒退的时间；缺少映射在本次 GPU 工作之前拒绝。新旧绑定分别持有快照，实际使用 fence 未完成时保留页和元数据；空间租约释放不会破坏已取得的读取 view。元数据按实际 committed allocation 计费并设在途上限，场页与向量绑定不重复算进粒子池预算。

新增 `wind_drift` 作者资产及独立 cook 目录。现有 `fieldCoupling` 仍明确拒绝，CPU 参考后端也未自动接受速度场资产。独立 GpuRuntime 可注入绑定；宿主当前没有场生产/绑定驱动，CLI 启用该资产明确拒绝，不能把独立测试写成游戏宿主已受风场驱动。已有水查询代理的 XZ 单层流速也不等同于三维 waterVelocity 数据。

汇总证据：`reports/0.11.0/31c00bc53c0b/20261023T102539Z/effects-field-incremental.json`。收尾核对 **362 项当前源码 SHA** 与 **119 项已冻结输入 SHA**，均匹配；这是各专项清单的校验条目数，不是不同文件总数。宿主源码/二进制与当前资产另外冻结在绘制报告目录。

| 本轮受影响增量 | 结果 | 报告时间目录 |
|---|---|---|
| GPU 速度场消费 | 8 组/1659 次、826 次 CPU 向量对拍 | `20261023T102327Z` |
| 效果作者合同 | 9/9，含新模块权限/参数/布局/烘焙 | 同上 `authoring.log` |
| 共享场 GPU | 7 组/25760 次 | `20261023T100302Z` |
| 原 GPU 粒子池与实例 | 8 组/49842 次、7 组/1158 次 | `20261023T101523Z` |
| GPU 绘制与受影响宿主 | 5 组/604 次，宿主入口 26/26 | `20261023T102116Z` |
| CPU 粒子参考与加载 | 9 组/13360 次，100 次确定性重复 | `20261023T102155Z` |

AMD 890M 的各 GPU 程序均实际启用 debugLayer，debugErrors=0。宿主 off/GPU 的 warning 分别为 723/760，保留现有性能提示。GPU 路径实际绘制 240 帧、720 次间接 draw，CPU sprite 上传帧为 0；停止后截图与 off 一致，resize/重建后继续绘制。测试分别声明验证专用同步读回、运行时计数异步遥测和无场读回，没有将它们合成一个“无读回”承诺。

独立入口为 `npm run effects:fields`、`npm run effects:cpu`，均支持 `-SkipBuild` 来源核对，不构建/运行其他测试。CPU 专项发现旧全局 cook 的 lamp 贴图 SHA 已与用户新资产不符，随后只在专项目录重新烘焙并验证，未替换原库。宿主首次构建期间用户又更新了 host/main.cpp，门禁正确拒绝混合来源；保留新修改、按最新输入重建后，上表宿主专项通过。完整 shader 依赖现在是 58 个 blob；新场专项只编译相关 9 个，绘制专项只编译相关 17 个。

本轮没有执行全量回归。A–G goal 继续 active；接下来需要场发布/绑定的宿主流程、场写求解与调度、其余粒子模块/渲染器、GPU 布料/水、统一体积及创作工具，未将 B、C 或完整方案标记完成。

## 共享风/流场的宿主发布与按需回收（2026-10-23 深夜）

渲染器新增显式 World 配置和帧回调：申请/释放只读区域租约、发布已提交的 CPU 快照、返回不可变向量绑定、按独立 fieldTick 执行粒子 tick。上传与消费使用实际图形队列/fence，宿主不拥有原生命令列表；场 heap 使用后恢复贴图 heap。World 与预算在 renderer 重建中保留，旧 view 的共享 World 所有权避免悬空引用。

`fields/vector_source.*` 提供可创作的整数向量轨迹、米制网格和区域/halo。构造仅注册通道，真正被观察时才申请写需求；全部分量原子发布，固定 mm/s 量化与整数有理插值，调用方提交 World。预览源 `assets/fields/gust_wind.field.json` 通过 `--effect-field` 启用。四个粒子实例共享一份每 tick 的绑定。当前预览仅允许 visual 数据域，不把按 GPU 自然退休时间管理的视觉生命周期伪装成 Game 权威；通用渲染器仍可读取外部 CPU 权威 World 的镜像。

补齐了停止后的 CPU 写租约与来源撤销、同队列 CPU 在途页回收，以及不持有旧 GPU 设备的宿主弱引用。没有消费者后，CPU/GPU 页、租约、来源和向量元数据均归零；作者轨迹与通道定义仍可重新使用。重新申请使用新的未提交 tick，不改写已提交版本。完整接口、约束和复现见 [FIELD-HOST.md](FIELD-HOST.md)。

入口 `npm run effects:fields:host` 只构建参考源及宿主依赖，Windows PowerShell 下实际通过 **41/41**。报告：`reports/0.11.0/31c00bc53c0b/20261023T151631Z/field-host-evidence.json`，含十四个宿主运行的原始输出、截图、源码/二进制与资产快照。

- CPU 向量生产者 **6 组/2829 次**：合同拒绝、有符号舍入、64 位循环、跨通道/halo 发布、无消费者零分配、预算失败无半发布；30/60/144 Hz 分组得到相同 CPU 摘要，另有 8 次重复。
- 默认 CPU/GPU 路径不申请场；零风/动画风的实际粒子运动与截图不同，Game 摘要保持相同。动态风四实例真实绘制 240 帧、720 次间接 draw，479 次发布/绑定，三页、六个在途快照，在 16 MiB 场/2 MiB 元数据预算内。
- 立即停止后 CPU/GPU 页和全部发布资源归零；自然退休/重新发射运行产生 242 次源发布，最后没有空间租约。异步视觉退休次数不承诺跨 GPU 确定性。
- resize/renderer 重建后字段 tick 476、重建粒子 tick 158，CPU 场源继续而视觉池重启；独立 waterVelocity 作者场和中文文件路径通过。缺绑定、网格不一致、非法资产及用预览源声明 CPU 权威均明确拒绝。
- 页按 COPY_DEST 创建曾被驱动忽略，导致每次发布三条 warning；改为 COMMON 创建并显式转换后，动态场 warning 从 2197 降为 760，与普通 GPU 粒子相同。AMD 890M 各正常运行均实际启用验证层，errors=0；保留现有 warning，不过滤消息。修正前 `20261023T151016Z` 的二进制和原始报告保留。
- 受影响共享场 GPU **7 组/25760 次**（`20261023T151523Z`）、速度消费与 World 所有权 **9 组/1675 次/827 次对拍**（`20261023T151535Z`）重新通过；后者同时运行作者合同 9/9。

期间其他开发多次更新 host/main.cpp 与 renderer/d3d12_renderer.cpp，来源门禁正确拒绝混合输入；保留并重建最新输入后取得上述完整专项。报告对应冻结输入，后续其他修改不能自动继承这些结论。新向量源参考已登记标准 native 构建/测试入口，打包配方纳入字段资产及指南；本批没有执行全量测试或完整打包/发布门禁。

本轮只完成 B/C 的共享读取宿主增量。Game 权威时钟/存档/回放、统一预算降档、GPU 场写求解与 LEF/WF 所有权迁移、烟火统一体积、GPU 布料/水、其余渲染器及创作工具仍按原始 A–G 目标继续，goal 保持 active。

收尾指纹审计：`reports/0.11.0/31c00bc53c0b/20261023T152440Z/field-host-finalization.json`。36 项字段输入/冻结副本、176 项宿主冻结输入匹配。41 项专项归档后，用户新增了不经过字段测试路径的导入包身份校验；本次审计时字段集成段逐字未变，最新宿主已独立构建成功。该可执行文件和构建清单另作 build-only 归档；用户源码随后继续变化，没有完整冻结新构建的源码差量，也没有把原字段测试冒充新增导入分支的测试。41 项报告自身的源码与二进制快照完整保留。

## GPU 三角布料与共享风场（2026-10-24）

完成本批 E0/E1/E2 增量：`nativegpu.cloth/2` 与 cook `/2` 使用经/纬应变、剪切与真正的四点二面角 XPBD，替代旧版跨两格距离弯曲。离线烘焙静止材料矩阵、完整邻接、膜与铰链的无读写冲突颜色；运行时验证拓扑/朝向/覆盖/颜色，CPU 与 GPU 使用同一颜色顺序。旧 `/1` 继续走 CPU 距离模型，迁移要求显式给出不同单位的角度顺应度。五个示例资产已迁移，未覆盖旧全局烘焙库。

新增 D3D12 `GpuSolver` 与 13 个 cloth shader blob：面压力、积分、着色约束、静态平面/球/胶囊、锚点、速度及法线/材料切线输出。邻接气动力先单独计算，避免读到已积分邻居。实际 committed buffer 分配、顶点/dispatch 预算与真实 fence 在途期限均受检查；求解器不等待或读回 GPU。读资源前显式转换并登记 draw fence，shutdown 拒绝未完成使用。独立 GPU 程序的读回仅用于验证。

`GpuVectorView` 提供只读共享风，显式 fieldTick 和米制→Q8 映射。全部顶点/三个分量完整时才提交候选风，否则整批保留，过期/缺失/越界均计数；传送清掉原位置的空间输入。GPU 不改写 CPU 权威 World，替换后的 view 由实际场 fence 保留。GPU 仅接受环境挂点，蒙皮资产与自碰撞请求明确拒绝。

最终证据：`reports/0.11.0/31c00bc53c0b/20261023T175805Z/cloth-gpu-evidence.json`，冻结源码/二进制、作者/烘焙资产、shader header、日志及输入 SHA。入口 `npm run cloth:gpu -- -Adapter AMD`，支持 `-SkipBuild` 严格来源核对，独立目录 `build/cloth-gpu`。标准构建和可选 GPU native 门禁仅登记目标，本轮未执行这些完整入口。

- 布料 JS 8/8；/2 与 /1 CPU 各 7 组、150 次、100 次确定性重复。
- GPU **7 组、2905 次检查、160 次顶点对拍**，最大位置差 0.2403 mm、速度差 0.004318 m/s；AMD 890M、debugLayer=true、debugErrors=0。
- 旗帜 1.2×0.8 m 与方布 1×1 m 各 600 tick / 5 s，最后提交态边长误差 P95 分别为 0.7024% / 0.02884%，锚点漂移均 0。每块布 12 个 buffer 实际对齐分配 786432 字节，没有声称完整负载性能达标。
- 接触夹具持续 240 tick / 2 s，在 tick 1、2、30、120、240 采样，几何穿透 P99/最大值均 0；平面/球/胶囊实际投影 80/164/55 次。该离散静态夹具不代表运动角色或 CCD 验收。
- 新三角模型下共享场适配器 **5 组、900 次**通过：`20261023T175430Z/field-adapters-evidence.json`。入口已改用 `build/field-adapters/cloth`，不再覆盖全局布料库。

完整接口和限制见 [CLOTH-GPU.md](CLOTH-GPU.md)。本批没有宿主布料绘制；自碰撞/CCD、骨骼/渲染网格映射、LOD 实际切换、湿润/热耦合、双面织物材质、存档和编辑器仍待开发。A–G goal 继续 active，E 仍部分完成；没有运行全量回归。

## GPU 布料真实绘制与宿主生命周期（2026-10-24 后续）

新增 `gpu_cloth_draw.*`、`gpu_cloth_bridge.cpp`、`cloth_draw.hlsl` 和 `host/cloth_preview.*`。VS 直接读取 solver 的 GPU 形变和三角记录；双面 PS 使用朝向法线、网格 UV/材料切线、过滤织纹与基础 Charlie sheen，经纬方向粗糙度采用明确的近似。现有 surface/key MRT 和深度在不透明阶段写入，后续体积与 GPU 精灵继续使用同一管线。无逐帧 CPU 形变顶点上传或同步读回；静止数据初始化上传保留。

draw 与 solver 强制同设备、同实际 fence；绘制时为 solver 登记真实使用期限，queue gate 验证两者均拒绝提前释放。材质与光照在记录命令前校验，并遵守 scene 的线性 RGB/强度合同，避免 FP16 MRT 溢出。渲染器支持 1–32 个配置实例和合计 64 MiB committed 模拟 buffer；CLI 提供 1–16 个预览实例及原点/风/调试/暂停/恢复/传送/reset 控制。独立游戏年龄与物理年龄分开，renderer 重建重启视觉静止态，保留当前原点、暂停区间与已消费事件。

入口 `npm run cloth:draw -- -Adapter AMD` 与 `npm run cloth:host`，独立 `build/cloth-draw`、`build/cloth-host`，均支持 `-SkipBuild` 来源校验，不使用资产专项的 `build/assets-host`。标准构建和可选 GPU native 门禁已登记；本批未执行完整构建测试或全量回归。

- 绘制 **5 组、255 次检查、36 个解析像素分量**，实际 486 个 IA 顶点、8502 次 PS invocation；验证正反面/切线/UV、两张 MRT、深度顺序、GPU 形变/reset、拒绝与实际 fence，以及分组/重复/倒退游戏 tick 和设备 epoch。报告 `reports/0.11.0/31c00bc53c0b/20261023T182726Z/cloth-draw-evidence.json`，附受影响求解 **7 组/2905 次/160 次顶点对拍**。
- 宿主 **66/66**，18 个运行：off/零风/有风/四实例/法线/暂停/恢复/传送/reset/重建/粒子同屏/中文库及六种拒绝。报告 `reports/0.11.0/31c00bc53c0b/20261023T182726Z-1620b7/cloth-host-evidence.json`，冻结二进制、186 项宿主输入、32 项资产/门禁输入和原始截图。
- 单布实际绘制 240 帧、118560 个三角形；四布 960 次 draw、474240 个三角形、3145728 字节 committed buffer。暂停运行物理 tick 9、游戏年龄 478；重建后继续绘制 80 帧，事件只消费一次。
- 零/有风、四布、调试、暂停/恢复、传送/reset、GPU 粒子同屏及中文库的游戏摘要与 off 一致。GPU 精灵仍有 240 帧、720 次间接 draw；中文库与 ASCII 库截图 SHA 相同。
- AMD 890M，各正常运行实际启用调试层、errors=0。off/单布 warning 均为 723、混合 GPU 粒子为 736，保留原消息。最新计时样本单布求解/绘制 3.4778/0.00344 ms、四布 11.5052/0.01184 ms；这不是标准负载 P95，**尚未达到方案组合预算**，需优先减少颜色/实例/子步派发开销。

详细合同、CLI 和证据边界见 [CLOTH-RENDERING.md](CLOTH-RENDERING.md)。宿主当前只有均匀风，独立 solver 的共享场能力尚未接成布料租约/发布流程；蒙皮/渲染网格映射、自碰撞/CCD、湿润/热、LOD、完整材质资产/局部灯/IBL、保存/回放和编辑器仍缺。E 与 A–G 继续 active/部分完成，没有缩小原始 goal。

66 项专项归档后，其他开发继续修改 `game.cpp` 的角色 renderable offset 与 `d3d12_renderer.cpp` 的空 skin hash 缓存。没有覆盖这些修改；最新依赖已重新构建，并在测试前冻结源码和可执行文件，运行 off/单布/布料＋GPU 粒子三项 240 帧兼容检查，均成功、实际验证层错误 0、截图 SHA 与原对应夹具完全一致，玩法摘要一致。增量报告 `reports/0.11.0/31c00bc53c0b/20261023T183434Z/cloth-host-dependency-delta.json`；不将这三项兼容检查冒充新角色位移/空缓存分支的独立测试，也不将原 66 项结果归给后续任何源码修改。

## 布料组内完整 tick、严格浮点与配对性能（2026-10-24 后续）

新增 `GpuExecution::{Automatic,Dispatch,Grouped}`。容量 ≤512 顶点/4608 个标量约束时默认用 256 线程单工作组完成完整 tick，保留原子步/迭代/离线颜色顺序；位置/速度 SoA、λ 共 30 KiB TGSM，并对位置/速度槽做无损 bank 分散。大网格保留逐颜色路径，强制组内超容量明确拒绝；无场每 tick 3 次、含场 5 次派发。旗帜 600 tick 的派发从 435602 降为 1802，12 个 committed buffer 预算未变。

初版普通优化编译在 120 tick 出现 >3 mm 的累积位移差，两个复现一致，失败源/二进制/日志保留在 `reports/cloth-grouped-difference-20261024/`；局部 precise 未充分解决。统一布料 compute blob 的 `ieeeStrict=true` / DXC `-Gis` 后，同设备数值对拍差为 0。64 线程严格原型的 2048 点性能门槛失败记录保留在 `reports/cloth-grouped-gis64-20261024/`，随后调整线程布局和 bank 访问；没有放宽 3 mm/0.5 m/s 对拍门槛，也没有减少物理迭代。

入口 `npm run cloth:grouped -- -Adapter AMD`（可 `-SkipBuild`）先执行相关求解/绘制，再运行容量/回退和配对计时。独立专项只编译 16 个布料 blob，完整 shader 清单 74 个；标准构建登记 benchmark 目标，本轮未执行全量回归、完整 native/JS/CI。

- 正式组内专项 **5 组、15209 次检查、5544 次顶点采样对拍**：280 与 512 点各运行 600 tick / 5 s，在七个明确节点采样，位置/速度差均为 0。512 点使用组内、528 点回退，预算失败不修改状态，强制超容量和非法模式拒绝。报告 `reports/0.11.0/31c00bc53c0b/20261023T231022Z/cloth-grouped-evidence.json`，冻结 82 项输入、二进制及编译证据。
- 受影响求解 **7 组/2905 次/160 次 CPU 对拍**、实际绘制/预览时钟 **5 组/255 次/36 个解析像素分量**通过：`20261023T231001Z/cloth-draw-evidence.json`。CPU/GPU 最大位置差 0.2596 mm，旗帜与方布最终态边长误差 P95 0.7976% / 0.01815%，锚点漂移 0。
- 每个计时样本两次固定 tick，热身 8、测量 60，交替执行先后顺序，保留四份原始数组和真实频率。单布 GPU P50/P95 原 2.63220/9.46152 ms、组内 0.75980/0.76804 ms；2048 点原 9.71868/16.80896 ms、组内 3.39772/9.15800 ms。2048 点 CPU 命令记录 P95 从 6.2310 降为 0.6714 ms。两轮测量都保留，未将尾部波动隐藏或将测试同步等待算作生产求解行为。
- 宿主 **76/76、21 个运行**：`20261023T230320Z/cloth-host-evidence.json`，冻结 189 项宿主和 36 项夹具输入。`--cloth-execution` 三模式可选，两种执行模式截图 SHA 一致。2048 点四块布真实绘制 960 次 draw、892800 个三角形，使用 3145728 字节 committed buffer。重建/暂停/传送/中文库/粒子同屏与拒绝路径继续通过；各正常运行 debugLayer=true、errors=0，off/组内/2048 点 warning 均为 723。

细节见 [CLOTH-GROUPED.md](CLOTH-GROUPED.md)。2048 点独立求解 P95 仍超过 Low 档四系统合计 4 ms 的目标，完整组合负载与更大网格优化没有通过；下一步继续实例批处理/调度和剩余 E/B/C/D/F/G 目标。严格编译不改变 GPU 视觉的权威归属，不作跨硬件逐位确定性承诺。A–G goal 保持 active。

76 项归档后检测到用户继续修改 `host/main.cpp`、`scene_loader.cpp`、`game.cpp/.hpp` 与 `gameplay_bridge.cpp`，包括轨道相机入口。保留全部修改，构建成功后立即冻结最新源码与二进制，再执行 off/grouped/dispatch 三项 240 帧兼容检查，均通过、实际验证层错误 0、两种模式截图 SHA 相同、三项玩法摘要一致。补充证据 `reports/0.11.0/31c00bc53c0b/20261023T232241Z/cloth-grouped-host-delta.json`，重放脚本在该目录；不把原 76 项或这三项兼容检查冒充新增相机/场景分支的独立验证。


## 多实例布料批处理、环生命周期与逐位对拍（2026-10-24 后续）

新增有界 `GpuBatch`：一次 Dispatch 为每块 ≤512 点/4608 标量约束的布料建立独立工作组，保留所有物理迭代；均匀风准备、完整 tick 和法线合为一次派发。含场实例先执行原三次采样/整布提交，之后参与共享求解。暂停跳过，大网格/显式 Dispatch 使用原路径。整批先验证，拒绝不会部分推进实例。单独 solver 的原派发预算和接口保留。

元数据与描述符按实际拥有的队列 fence 分槽；同 fence 多次调用不覆盖；真实队列阻塞时 shutdown/复用拒绝，完成后回收持有的资源引用。默认 32×32 服务 committed 元数据 196608 字节、描述符 stride 估算 393216 字节，合计预算 589824 字节；明确区分估算与实际分配。宿主按实例数量收缩服务，加入原 64 MiB 总准入，`stepAll` 接入统一游戏 tick，并报告实际共享派发和服务预算。没有生产等待、读回或 CPU 顶点上传。

增量入口 `npm run cloth:batch`；`-SkipDrawBuild` 校验并复用求解/绘制构建，`-SkipBuild` 校验复用全部专项构建。标准构建只登记新目标，本轮没有执行全量回归、完整 CI、npm test 或 npm run verify。

正式报告 `reports/0.11.0/31c00bc53c0b/20261023T234305Z/cloth-batch-evidence.json`：4 组/22939 检查/6712 次顶点采样比较；280/512 点 600 tick、528 点回退、独立 dt/原点、暂停/传送/reset/改风、CPU 权威风/整布失败/过期、时间拒绝、真实 fence 环占满/引用回收均通过。位置、previous、速度、法线和切线逐位相同，诊断一致，调试层实际开启 errors=0。受影响求解 7 组/2905 次、绘制/预览时钟 5 组/255 次继续通过；冻结源码、二进制、编译日志和样本数组。

890M，2048 点每样本两次 tick、热身 8 后交替测量 60：GPU P50/P95 逐实例组内 3.31976/4.16772 ms，批处理 0.96452/1.01220 ms；CPU 命令记录 P95 0.3026/0.2894 ms。上一轮 GPU P95 4.25436/0.99324 ms、CPU 0.3590/0.3799 ms 也保留，不隐藏波动或宣称 CPU 恒定改善。只有布料求解区间，**没有完成四系统合计 GPU P95≤4 ms 验收**。详情见 [CLOTH-BATCH.md](CLOTH-BATCH.md)。

剩余骨骼、自碰撞/CCD、湿润/热、LOD、宿主场生产/租约、完整材质/工具及 B/C/D/F/G 原目标仍需推进，A–G goal 保持 active。


本批宿主专项最终 **89/89、21 运行**，`reports/0.11.0/31c00bc53c0b/20261023T234725Z/cloth-host-evidence.json`。立即冻结 195 项宿主依赖和二进制后测试，另冻结 36 项资产输入；运行期间源码变化列表为空。每个正常运行实际调试层启用、errors=0，自动批处理/显式 Dispatch 截图 SHA 相同；2048 点真实绘制 960 次/892800 三角形，478 次共享 tick 共 486 次派发，实例 buffer 3145728＋服务准入 114688=3260416 字节。暂停/恢复/传送/reset、设备重建、中文路径、粒子同屏与非法输入均通过。

初次宿主构建因本批 fence 补充时源码变化而中止；下一次因用户并行 scene_loader 修改而拒绝归档。没有放宽校验或覆盖修改；入口改为构建成功后立即冻结输入/二进制，从冻结版本运行并记录随后源码变化。原始中止日志保留在 build/cloth-host/batch-gate.log 和 batch-gate-final.log，最终冻结版本结果不冒充用户后续修改的验证。


归档后检测到 host/main.cpp 的后续修改，保留并重建后立即冻结最新依赖，仅补跑 off/batch/dispatch 三项 240 帧兼容检查，均通过、调试层实际开启 errors=0、两种布料路径截图 SHA 相同、三项玩法摘要一致。补充证据 [cloth-batch-host-delta.json](../reports/0.11.0/31c00bc53c0b/20261024T035413Z/cloth-batch-host-delta.json)；不将这三项兼容性检查冒充用户新功能分支的独立回归，也不将原 89 项结果归给之后的修改。


## 独立宿主共享风场、后绑定与消费者撤销（2026-10-24 后续）

场服务从 GPU 粒子内部拆出为 Renderer 的共享仿真服务：`GpuFieldFrame` / `configureSimulationFields` / `gpu_field_bridge.cpp`。新增 `--simulation-field`，支持只有布料，或 GPU 粒子与多布共用一份风快照；作者源只提交一次，协调器读取租约聚合消费者兴趣。旧 `--effect-field` 保持独立时钟和接口，不允许同时配置两套来源。作者源只接受 visual windVelocity；不冒充 Game CPU 权威。

帧记录前以当前游戏 tick 采样，供本帧视觉补步使用；此近似规则明确暴露，布料年龄/暂停/reset 与字段时钟分离。没有消费者时撤销读取租约和源账本，实际 fence 完成后回收 GPU 页/上传/绑定元数据，不继续 GPU 快照发布。粒子停止但仍有布料时保留源。设备重建保留驱动配置、源时间和 CPU World，重新获得 GPU 租约。

布料可在创建时 reserveWindField 并晚绑定/清除；普通 root 56 DWORD，只有 SampleWind 使用 58 DWORD 直接索引变体，不创建占位 heap。清除回到均匀风并保留字段时钟。初版触发未绑定描述符堆的调试错误，失败二进制/源码/日志保存在 reports/cloth-late-binding-unbound-root-20261024/；后续将根签名分开后通过。宿主初版因初始化 reset 丢失新驱动而仅 57/87，通过失败报告 20261024T040449Z/simulation-fields-evidence.json 保留，修复驱动保留而非放宽断言。

- 根签名/后绑定与相关布料专项：`20261024T040311Z/cloth-batch-evidence.json`，5 组/23123 检查/6760 顶点采样，整个顶点状态逐位相同，位置/速度差 0，实际调试层 errors=0。受影响求解 7 组/2905 次、绘制/预览时钟 5 组/255 次通过。
- 共享宿主：`reports/0.11.0/31c00bc53c0b/20261024T041216Z/simulation-fields-evidence.json`，100/100、22 运行；预先冻结 198 项编译依赖和 41 项夹具，运行期间源码差异为空。常量空间风与相同均匀风截图一致，零风改变形变，覆盖外整布回退一致；新共享粒子路径与旧入口恒定源截图一致。全部正常场景实际 debugLayer=true/errors=0，玩法摘要不变。
- 240 帧时只提交 240 次源/绑定，单布、粒子＋布料和多布仍只有一组源历史；World tick 480。粒子最后停止夹具只发布 16 次，最终 CPU 页/租约/源记录、GPU 页/上传/快照/描述符/绑定元数据全为 0；仍有布料时保持可用。暂停/恢复、传送/reset、设备重建、中文路径、旧入口释放与非法配置拒绝通过。
- 协调器专项：`20261024T041604Z/shared-wind-evidence.json`，35 检查，真实 GPU/调试层 errors=0；零订阅分配、重复帧重用、向后时间/坏帧拒绝、恢复、GPU 发布失败重试、撤销/闲置/重订阅和已提交 tick 不可重写均验证。

独立入口 `npm run fields:shared-wind` 与 `npm run fields:simulation`，支持哈希校验的 -SkipBuild；后者仅构建 build/cloth-fields-host。标准构建登记新目标，本轮没有运行全量 native/JS/CI、npm test 或 npm run verify。详见 [SIMULATION-FIELDS-HOST.md](SIMULATION-FIELDS-HOST.md)。

仍未完成 GPU LEF/WF 写入/所有权迁移、逐 simulation tick 联合调度、温度/湿润/水联动、Game 权威集成/回放、烟体积、角色布料/自碰撞、完整工具/联合预算等原 B–G 目标；不得用本批 100 项宿主检查替代 A–G 总体验收。原 goal 继续 active。


## GPU 私有 LEF、真实粒子注入与粗投影（2026-10-24 后续）

新增 engine/native/fields/gpu_local.* 和 shaders/field_local.hlsl。局部字段按需分配量化 SoA，直接读取 GPU 池的存活列表/计数/位置；输入无需 CPU alive 回读或细网格上传。CPU 先按容量×quantum 证明每通道累计 ≤UInt32Max，GPU 完成加法后统一钳位。只允许 VisualReference 非负通道与匹配 owner，复制目录不修改 CPU World；加权温度/速度未实现。

支持 1/2/4/8/16 比例、负坐标及 full coarse volume 平均/Maximum。分组双字 UInt64 归约，不依赖 64 位原子。每个不可变投影保留源/epoch/源 tick/发布时间、单位/域/细粗覆盖、粗值、积分/量化残差及独立的钳位/覆盖外/无效输入损失快照。相同版本重复请求复用投影；LEF 保留细数据。这不是 WF 发布或求解权迁移。

真实 fence 管理借用的四个输入资源、退休实例与投影视图；旧/新代际并存时共同计入实际对齐预算。默认 32 活跃实例、1M 样本、64 MiB、16 投影视图和 128 输入批次；高通道大网格使用二维 Clear，不裁剪资源。显式拒绝未来/类型/权限/内存/计数上界/阶段错误，不隐式改变分辨率。

专项 `npm run fields:local:gpu -- -Adapter AMD`（支持哈希校验 -SkipBuild），只编译 5 个新 LEF＋8 个池 shader，完整清单现为 80 个；没有运行全量回归、完整 native/JS/CI、npm test 或 npm run verify。正式报告 `reports/0.11.0/31c00bc53c0b/20261024T045636Z/fields-local-gpu-evidence.json`：9 组/2363 检查/274 次 CPU 粗单元比较，34 项冻结输入，实际调试层开启 errors=0。

覆盖五种比例、实际 GPU 粒子、负坐标/local-space/非零原点、同通道多批次顺序、饱和/损失账本、GPU 坏计数/slot/NaN、pending 重试与真实 fence 生命周期。128³×3 通道（6291456 样本）验证二维清除与旧快照保持；UInt64 输入积分4294966784、重建4294967296、残差−512。源粒子内容不被消费端改写。完整合同与失败夹具修正记录见 [FIELD-LOCAL-GPU.md](FIELD-LOCAL-GPU.md)。

GPU WF 分源聚合/撤销、稀疏页/查询、LEF 移动/halo、多级分辨率、GPU 流体/所有权迁移、资产模块/宿主调度、统一体积和 A–G 全部剩余项仍未完成；原 goal 保持 active。


## GPU 视觉 WF 分源聚合、撤销与稀疏快照（2026-10-24 后续）

新增 engine/native/fields/gpu_world.* 与 shaders/field_world.hlsl，直接消费 GPU LEF 不可变粗投影。按 owner/source/写租约代际持有来源，WF epoch 独立于 LEF，重复替换幂等，撤销后重新归约；三路 INT32_MAX 使用双字 UInt64 和，最终统一钳位。只支持视觉非负 Sum/Maximum，明确拒绝 CPU 权威域和 Strong。CPU 只上传元数据，没有生产数值读回或体素上传。

租约覆盖共享每通道 16³ 砖块，按需构造 arena 与现有采样 ABI；不是 GPU 物理页空闲池。快照包含源 tick、known/clamped 位，Q8 三线性和缺贡献 Unavailable 与 CPU World 对拍。旧快照不可变，来源投影/临时上传/输出由真实 fence 保护；释放写者在下次提交自动撤销，退出墓碑有界。单快照额度允许已完成且无读者的 current 原子替换，峰值仍计算旧+新。

专项 npm run fields:world:gpu（支持哈希校验 SkipBuild），选编 16 个 shader，完整清单 v15/82。正式 reports/0.11.0/31c00bc53c0b/20261024T100133Z/fields-world-gpu-evidence.json：8 组/1124 检查/125 次 CPU World 查询比较，890M 实际调试层 errors=0。覆盖替换/饱和撤销/反序发布/旧快照、64 位时钟、通道权限、无订阅零分配与派发、真实 queue gate、64 次写者退出及内存/行/页/来源/租约/视图容量拒绝。

LEF 投影增加设备/timeline 身份；依赖回归 reports/0.11.0/31c00bc53c0b/20261024T100140Z/fields-local-gpu-evidence.json：9 组/2363 检查/274 次 CPU 粗单元比较，errors=0。CPU 参考的 release 不自动撤销贡献，首次测试差异保留原始失败证据后改为显式 withdraw+release；没有改参考实现或放宽缺贡献断言。

完整合同见 [FIELD-WORLD-GPU.md](FIELD-WORLD-GPU.md)。本轮仅增量测试，无全量 native/JS/CI、npm test 或 npm run verify。可观察性控制器/宿主与资产接入、GPU 页池/多级分辨率、流体/halo/所有权迁移、覆盖去重体积、布料/水与创作工具/联合预算等 A–G 剩余目标继续，goal 保持 active。


## GPU 可观察性共享状态机与提交确认（2026-10-24 后续）

新增 engine/native/fields/gpu_sharing.*，在 GPU WF 上登记有界 CPU 元数据，不预申请租约或 GPU 数据。LocalOnly → PromotePending → Shared → DemotePending → LocalOnly/Retired，按精确已提交来源 epoch/时间/贡献状态 settle；驻留从确认版本 snapshotTick 开始，更新不重置共享驻留。降级撤销后释放写者，LEF 细节/求解仍由生产者持有。注册 source ID 单调、句柄带代际；没有历史 owner 墓碑持续增长。

沿用 CPU PromotionPolicy：浓度高低阈值迟滞、worldVisible/订阅触发、最短局部/共享驻留、priority/source 稳定排序及每 tick 提升配额；同 tick 多次调用不重置。无效批次在决策前拒绝，容量/缺投影/发布失败不消费成功提升额度。浓度目前是外部显式观察，不是实际 GPU 阈值归约/异步遥测已完成；候选投影生成仍由调用者负责。

专项继续 npm run fields:world:gpu，只添加控制器依赖和相关测试，未运行全量回归。完整合同、已验证证据和剩余边界见 [FIELD-SHARING-GPU.md](FIELD-SHARING-GPU.md)。资产/宿主调度、覆盖去重体积、移动/多级分辨率、GPU 页池/流体、玩法权威与存档、四系统完整联合预算仍待完成，原 A–G goal 保持 active。

本批正式 reports/0.11.0/31c00bc53c0b/20261024T101517Z/fields-world-gpu-evidence.json：13 组/1601 检查/128 次 CPU 数值查询比较，冻结 40 项来源，890M 实际 debugLayer=true/errors=0，空警告编译日志。包括原 WF 八组和控制器五组；32 次共享/撤销从局部 epoch1 推进 WF 至64，无来源/写者泄漏，旧快照保持。延迟提交 4→9、12→15 验证驻留从 snapshotTick 起算，失败发布回收临时写者，真实 queue gate 下状态确认不绕过 fence。LEF 数值/shader 未改，未重复执行不受影响的独立门禁。


## 认证粒子资产的整数场沉积与原生实例驱动（2026-10-24 后续）

新增 fieldDeposit(channel,quantum)，JS 与原生认证读者分别校验声明通道、正 UInt32 quantum 及每通道 asset.maxAlive×跨发射器/模块 quantum 总和上界。只接受非负 density/moisture；访问合同明确 postSimulation，普通模拟模块依赖更晚沉积阶段会被拒绝。原 fieldCoupling 未定义量值，继续显式不支持。

GpuRuntime 可显式配置 GpuFieldDepositDriver，所有 GPU simulate/spawn 完成后、时钟推进前回调一次，传递完整活跃写者集合、暂停标记、代际/serial、不可变资产和短期借用的 GPU 池；flush 只同步退出，不再沉积。无驱动的 GPU/CPU 路径仍明确拒绝，不静默丢掉写模块。驱动错误/异常后禁止继续录制，要求调用者排空/放弃录制并重建，防止部分 tick 重复模拟。

原生专项用验证协调器串联真实 GPU 实例池→LEF→WF：两实例/双发射器密度500，停止一个后250；暂停保留旧 sourceTick，local-space 移动正确，重复 flush 不再次沉积，旧视图不变。验证协调器尚不是宿主默认生产服务；生产驱动、完整网格/目录/观察映射、renderer 配置、体积覆盖和编辑器接入仍待完成。合同见 [EFFECT-FIELD-DEPOSITS.md](EFFECT-FIELD-DEPOSITS.md)。本批仅增量测试，原 A–G goal 保持 active。

本批正式沉积 reports/0.11.0/31c00bc53c0b/20261024T141840Z/effects-deposits-evidence.json：5 组/121 检查。相关粒子池/实例 reports/0.11.0/31c00bc53c0b/20261024T141900Z-7d98c5/effects-gpu-evidence.json：8/49842、7/1158；WF reports/0.11.0/31c00bc53c0b/20261024T141900Z/fields-world-gpu-evidence.json：13/1601，全部实际调试层 errors=0。JS reports/0.11.0/31c00bc53c0b/20261024T142129Z/effect-deposits-js-evidence.json：两个相关测试文件15/15，预先冻结9项输入。没有运行全量回归。


## 生产 GPU 粒子沉积服务、移动重建与真实宿主（2026-10-24 后续）

新增 engine/native/effects/field_deposits.*，复制视觉目录、解析资产 voxel/尺寸/通道和 owner，按实际对齐范围创建私有场。local 无订阅不投影、不申请 WF；worldVisible 或区域读租约（含 halo）触发共享，配额满时不生成多余投影。全 local-space 随原点重建，world-space/mixed 保留作者覆盖扫掠并集；这仅重建粒子快照，不迁移流体历史。旧来源撤销与新来源发布同次 WF 提交，Shared 重建不再等待一次提升驻留。

renderer 对含 fieldDeposit 的库自动创建服务，并用自己的真实 graphics fence 配置 GpuRuntime 驱动；写场命令纳入原效果 GPU 时间戳，原精灵准备恢复 heap/root。无写模块不创建服务，local 没有 WF 数据。默认私有64 MiB+WF64 MiB的分区总准入128 MiB，旧/新代际和在途快照都计费；不是完整四系统联合预算。

原生 reports/0.11.0/31c00bc53c0b/20261024T144711Z/effects-deposit-service-evidence.json：7 组/98检查、50项冻结来源、890M实际debug errors=0。宿主 reports/0.11.0/31c00bc53c0b/20261024T144503Z/effects-deposit-host-evidence.json：首轮9运行31/32，210依赖+26夹具冻结；唯一失败为既有验证InfoQueue创建漏计导致关闭计数−1/−2。无写场同binary也复现；补一行track后仅增量两组生命周期，reports/0.11.0/31c00bc53c0b/20261024T144947Z/effects-deposit-host-evidence.json：7/7，两次关闭各为0。原始失败证据保留，没有改写通过。

4实例240帧/478视觉tick，1912次LEF重写/1904投影，476WF提交/952派发，4源共享4砖块，拥有字节4850176B；local模式1835008B且WF/投影0。stop20后私有/世界/来源/租约/快照全回收，只19次WF提交；base/local/world截图hash与Game摘要相同，中文路径、短效果复用和恢复通过。正常实际调试层errors=0，既有sampler警告原样记录。

命令 npm run effects:deposits:service / effects:deposits:host，独立build目录；只增量构建/验证相关依赖，无全量回归。详见 [EFFECT-DEPOSIT-SERVICE.md](EFFECT-DEPOSIT-SERVICE.md)。私有容量/录制失败仍需重建，完整spawn预留/动态降档、统一体积、一般LEF/流体迁移、权威集成、其余粒子/布料/水/编辑器和联合预算仍待完成，原A–G goal保持active。


## 写场发射前准入、私有预留与容量恢复（2026-10-25）

GpuRuntime 在 GPU 池分配前调用写场准入并持有 RAII permit；按实际对齐网格预留 mutable 字节/样本/槽。非法网格或容量不足不推进实例身份、不分配该实例池；池分配失败/取消/退休释放 CPU 预留，在途真实资源仍按 fence 计费。move 先核对扫掠对齐和旧/新代际峰值，拒绝与异常保持原位置。renderer 选择资产时检查后端支持，未使用非法场网格不再阻止整库运行。

粗投影尊重未兑现私有预留；输入批次不足在 clear 前延迟整源重写，保持真实来源时间，完成 fence 后追上当前粒子快照。报告新增预留/尚缺额度、准入拒绝与延迟写入/更新。完整合同与证据见 [EFFECT-DEPOSIT-ADMISSION.md](EFFECT-DEPOSIT-ADMISSION.md)。

原生 20261025T025629Z 为 13 组/188 检查，新增六组边界/异常/恢复；RuntimeOnly 20261025T025636Z 为 7/1158；私有 LEF 20261024T152936Z 为 9/2363、274 次 CPU 比较。宿主仅四个准入场景，原报告 20261025T025441Z 保留 11/14；三处误把 CLI 最少 240 帧写成 80 帧，全部实际运行成功。独立复核冻结二进制/来源/原始 JSON/图像后 14/14，没有重复画面运行或改写失败记录。32 个接受写者继续共享/绘制，32 个请求提前拒绝，478 tick/15296 次重写，场资源 23988480B；CPU 摘要保持，实际验证错误 0，已有 sampler 警告保留。

只增量测试。此项未解决 WF 提交预算的完整事务背压、四系统动态降档/联合预算，也未完成其余 A–G 目标，goal 保持 active。

最终来源核对：[20261025T030209Z-admission-final](../reports/0.11.0/31c00bc53c0b/20261025T030209Z-admission-final/effect-deposit-admission-finalization.json)，361 次当前比较/583 次冻结核验、4 个二进制、251 项匹配输入。粒子/场专项依赖一致；宿主运行之后另有 12 项并行输入变化，已单列并保留内容，宿主通过仅对应冻结版本，未声称最新整个宿主已验证。


## WF 发布事务、覆盖交接与可恢复容量（2026-10-25 后续）

GpuVisualWorld.tryCommit 区分 Committed/Deferred/Invalid/ResourceFailure；快照、行和对齐字节容量不足不录制新的聚合命令。GpuFieldSharing 可选事务同时保存/恢复写租约、来源、共享状态、观察时钟与配额；生产服务在失败后保留已提交 WF，局部模拟继续，查询 sourceTick/age 保持真实。Shared 退出队列与连续移动的继承共享/投影预留保留到原子撤销/新发布成功。

撤销中的控制器写覆盖退出下一版本页准入，旧快照物理字节仍按实际 fence 计费；恰好 8 页也能交换相离区域。没有下一版本覆盖时只保存撤销确认，不分配空 GPU payload，不派发 clear/aggregate。

原生服务 20261025T031344Z 为 20 组/314 检查，WF 20261025T031350Z 为 14/1642、128 次 CPU 查询比较，实际调试错误为 0。宿主仅重跑正常/停止两组：20261025T031420Z 为 8/8；源/二进制/夹具预先冻结，运行结束来源变化为空。4 源正常 478 tick/1912 次重写、476 次 WF 提交/952 派发；停止路径 19 次提交/36 派发，场字节、预留、来源、租约、快照与退休队列全归零。实际画面已查看，CPU 摘要保持，旧 sampler 警告保留。

合同与证据见 [FIELD-PUBLICATION-TRANSACTIONS.md](FIELD-PUBLICATION-TRANSACTIONS.md)。本批仅相关增量专项；未执行全量测试/回归/CI。整批事务尚无逐候选公平裁剪和动态质量降档；GPU 页池/多层、流体/统一体积、玩法权威与存档及其余 A–G 目标仍未完成，goal 保持 active。

最终仅扩展实际队列阻塞背压：20261025T031846Z 服务专项 21 组/329 检查，同 fence 4 tick/3 次延期，停止后仍持有输入与旧来源，真实完成后退休恢复，debug errors=0。此后没有改生产代码或重复宿主运行。

最终来源核对：[20261025T032021Z-publication-final](../reports/0.11.0/31c00bc53c0b/20261025T032021Z-publication-final/field-publication-finalization.json)，343 次当前比较/598 次冻结核验、3 个二进制、259 项匹配输入；核对时 concurrentChanges 为空。池/LEF/WF/查询相关 shader 数学未改，没有重复无关独立门禁。


## WF 逐来源预算、等待队列与可承受更新（2026-10-25 后续）

GpuVisualWorld.budget 在申请写者和生成粗投影前，按 CPU 区域/通道/原子撤销组和真实资源对齐预估下一版本成本；实际提交核对行数、描述符与分配布局。服务跳过持续超预算候选，不消耗提升配额、不生成拒绝投影，已有可承受来源继续更新。移动的旧 Shared 撤销只与获选的新代际一起提交，不因其他来源成功就丢失退出队列。

排序为 priority、同级最早 pendingSince、稳定 source/serial。每源提供 pending/overdue、首次等待/最近尝试/实际发布时间、被延迟 tick 数和原因；默认延期阈值是 6 个 simulation tick，不承诺任意帧率三帧完成。publicationBacklog 有界并带 handle generation/source/priority，renderer 报告积压/超期清单。暂停生产者可补发先前已计算的版本且不刷新 sourceTick；取消观察清理等待。

最终原生服务 20261025T033600Z 为 27 组/432 检查；WF 20261025T033432Z 为 15/1680、128 次 CPU 查询比较，实际调试层错误为 0。新增超额首位候选跳过、配额为 1 的旧等待优先、移动/字节隔离恢复、暂停补发/取消，以及预测峰值对真实持有旧视图分配的检查。详见 [FIELD-PUBLICATION-SCHEDULER.md](FIELD-PUBLICATION-SCHEDULER.md)。

只做相关增量回归。抢占、动态降档、完整联合 CPU/GPU 时间预算、GPU 页池/多级分辨率、流体/统一体积、CPU 权威/存档及其余 A–G 目标仍未完成，goal 保持 active。

最终来源：服务 20261025T034235Z 为 27/432，WF 20261025T034218Z 为 15/1681；只补已有来源单源页上限的预估边界。宿主 20261025T034212Z 两组 8/8，15 单元编译/36 复用；正常无候选延期/等待/超期且清单为空，停止后全部场资源及清单归零，实际调试错误为 0，既有 sampler 警告保留。宿主编译阻塞仅拆开一处 originalOrder/binding 混合 auto 声明，保留其他工作。

[收尾核对](../reports/0.11.0/31c00bc53c0b/20261025T034514Z-scheduler-final/field-publication-scheduler-finalization.json)：344 次当前比较/592 次冻结核验，3 个二进制/252 项匹配来源；当前 shader 清单 v15/83，相关六份数值 shader 数学未改。宿主通过后又有 8 项其他输入变化已单列保存，currentHostMatches=false；粒子/场专项仍匹配，宿主结果只对应冻结版本。原测试编译/峰值观察及清单并行变化的拒绝记录均保留。


## 独立烟火 CPU/GPU 局部求解后端（2026-10-25）

新增 engine/native/smoke、13 个 smoke.hlsl 计算核。密度/真实 Kelvin/燃料按启用通道建立 SoA，速度为正侧 MAC 面速度，投影势/散度/旋度/残差为私有临时量；D/G 使用同一离散 Laplacian。封闭外墙、体素固体法向不可穿透与规定障碍速度，CFL 有界子步，源项/冷却/艺术燃烧和上下限，半拉格朗日/MacCormack 上游限幅、涡量约束已有。

实际 D3D12 后端使用不可变输入快照、真实 fence、分配/派发预算；无数值回读、无 CPU 权威写回。SourceTick/version/grid 随借用缓冲提供，消费者必须在同队列顺序中 prepareRead，不是永久历史视图。运行中初态重置拒绝，显式排空重建策略保留。

只运行 npm run smoke:gpu：20261025T070607Z 正式 9 组/44672 检查、43130 次 CPU/GPU 标量和面速度比较，890M 调试错误 0，源码/二进制/生成 shader 执行前冻结。压力屏障散度 L2 1.20777→0.00274632，移动固体 0.472965→0.000716417；非均匀烟团每 tick3 子步、452 派发，MacCormack 与一阶最大差0.200812，最大 CPU/GPU 标量差0.00012207、面速度差2.3842e-7。真实 queue gate 下不同输入不覆盖前一排队 tick，单槽完成后可复用。

首次全展开八点采样在 AMD 驱动创建 AdvectVelocity PSO 时崩溃，LLDB 堆栈与失败 HLSL/DXIL 留存；保留循环后通过，没有减少采样或回退 CPU。完整合同见 [SMOKE-FIRE-SOLVER.md](SMOKE-FIRE-SOLVER.md)。标准 build/verify 只登记入口，未运行全量门禁。原 renderer/旧烟火未替换；新后端尚无粒子/资产/LEF-WF/renderer/froxel 接入，统一体积/时域/热浪/场景固体/玩法适配和联合预算仍缺，D 和全 A–G 均未完成。

最终自动适配器专项 20261025T071418Z 仍为 9/44672、43130 次比较、实际 890M 调试错误0；不把测试入口固定为 AMD-only。收尾 [20261025T071648Z-smoke-final](../reports/0.11.0/31c00bc53c0b/20261025T071648Z-smoke-final/smoke-solver-finalization.json)：20 次当前比较/36 次冻结核验，13 个新计算核、清单 v15/96；其余83定义和旧 fluid/particles shader、renderer/particles.cpp 与基线一致。只登记未来全量构建/verify 的烟火入口，本轮没有执行它们。


## GPU 粒子到独立烟火源速率（2026-10-25）

新增 GpuParticleSmokeSource / GpuSmokeRateView 和三个严格浮点计算核；直接读取 GPU alive/SoA 位置，以有上界的整数原子归约生成 density、temperature、fuel 不可变源速率。密度/燃料源为 kg/s/粒子，温度源为 K·m³/s/粒子，除以体素体积后与 CellInput 速率相加。源按子步 dt 消费，烟场历史不被快照覆盖；空完整源集合产生零输入。Local/world、网格外损失、通道/uint32/速率/真实对齐分配/帧容量均有明确合同。

生产前持有四个真实池资源引用，真实生产 fence 完成后独立释放输入和 accumulator；速率与诊断保留至最后消费 fence 及外部引用释放。求解器拒绝不同 tick、网格或 fence 时间线；延后消费者可在粒子池正常销毁后读取。部分录制失败拒绝重试，排空后重建。详见 [源项合同](SMOKE-PARTICLE-SOURCES.md)。

仅增量执行 npm run smoke:particles 与 npm run smoke:gpu。源项最终 [20261025T073945Z](../reports/0.11.0/31c00bc53c0b/20261025T073945Z/smoke-particles-evidence.json)：6 组/1308 检查、576 次独立 CPU/GPU 标量比较及 192 次 GPU 旧快照不变比较；实际 890M 调试错误 0。包括同 fence 两 tick 持有 8 个池输入引用、三子步 dt=0.07 总密度约0.07、负侧网格外两粒子损失 quantum=256/512/128；生产 fence=23 完成后销毁池，延后消费 fence=24 未完成时仍保留131072B输出，完成后归零。新旧快照和 solver 历史均正确。

原求解器最终 [20261025T074033Z](../reports/0.11.0/31c00bc53c0b/20261025T074033Z/smoke-gpu-evidence.json)：9 组/44672 检查、43130 次 CPU/GPU 比较，调试错误0。两份报告均执行前冻结二进制/递归来源/生成 shader，运行后输入变化为空；新 shader 清单共99核。专项脚本固定导入对应 PowerShell 安装的 Utility 模块，修复 npm 子进程继承其他版本模块路径而找不到 Get-FileHash 的入口问题。

只注册未来标准 build/verify 入口，未运行全量构建/测试/回归/CI。本批仅原生输入适配；烟火资产/cook/实例服务/renderer 调度、LEF/WF 发布、统一体积/时域/热浪/玩法及其余 A–G 仍未完成，goal 保持 active。


## 烟火认证资产与 GPU 实例服务（2026-10-25 后续）

效果 source 新增可选 smoke：米制网格/生成偏移、按需通道、完整求解参数、尾烟 tick、按 emitter 引用的 kg/s 与 K·m³/s 源率、量化比例；cook 记录归一化配置和量化源，原生认证加载器独立重建与验证。共享纯配置验证从 solver 提取，未改变数值算法。禁用/重复通道、未知/重复 emitter、源量化溢出/下溢、温度/float/固定120Hz CFL 错误均拒绝；烟火配置变化的 patch 为 reset-required。新模板为 smoke_solver_reference，旧 smoke_reference 保持此前证据的原字节/hash。

GpuSmokeService 通过显式 admission/record hooks 接入 GpuRuntime，粒子池分配前按真实对齐预留整个烟场及 maxFrames 源快照峰值；池分配失败由 permit 回收未录制资源并恢复可重试 serial。暂停保留旧 sourceTick；移动发射器不平移生成处锚定的历史网格；自然耗尽后独立零源尾烟；立即 stop/clear 在 flush 同步退休；flush 不重复求解。消费者按真实 fence 延长输出寿命，同服务跨实例 fence 也禁止倒退。资源/派发不足时显式 skippedSteps，不用新位置补旧 tick；稳定最早成功步进排序避免固定首位饿死其他来源。中途 driver 失败拒绝重放并要求排空重建。详见 [资产与服务合同](SMOKE-EFFECT-SERVICE.md)。

本批仅增量运行：

- [烟火服务 20261025T145854Z](../reports/0.11.0/31c00bc53c0b/20261025T145854Z/smoke-service-evidence.json)：7组/690检查、576次独立CPU/GPU标量比较，13项相关JS资产测试；实际890M调试错误0。二进制/递归来源/夹具/生成shader执行前冻结，运行变化为空。包含固定网格移动、暂停/恢复、三tick尾烟、未录制取消、预准入/池失败回收、真实queue gate下两runtime tick仅一fluid步进及后续恢复、17派发配额的两源轮流执行、跨实例消费fence和录制后失败。
- [CPU效果 20261025T145459Z](../reports/0.11.0/31c00bc53c0b/20261025T145459Z/effects-cpu-evidence.json)：9组/13360检查，相关认证加载/cook/CPU fallback回归。
- [GPU Runtime 20261025T145509Z](../reports/0.11.0/31c00bc53c0b/20261025T145509Z/effects-gpu-evidence.json)：仅RuntimeOnly，7组/1158检查。
- [原烟火 20261025T145456Z](../reports/0.11.0/31c00bc53c0b/20261025T145456Z/smoke-gpu-evidence.json)：9组/44672检查、43130次CPU/GPU比较。
- [原场发布服务 20261025T145753Z](../reports/0.11.0/31c00bc53c0b/20261025T145753Z/effects-deposit-service-evidence.json)：27组/432检查，确认新增hooks不破坏原LEF/WF生产服务。

各GPU专项实际调试错误为0；没有运行全量测试/全量build/verify/CI、资产导入或无关宿主专项。旧CPU/GPU Runtime入口补齐新的递归头依赖指纹与对应PowerShell Utility导入，不将其执行后冻结证据称为新服务的执行前冻结流程。标准build/verify只登记未来烟火服务入口。

本批没有renderer/统一froxel接入、烟场WF发布/覆盖去重、相机时域/热浪/玩法粗代理、四系统时间预算、GPU状态热迁移/编辑器以及其他A–G剩余目标；没有将独立服务称为已完成宿主产品路径，goal继续active。

## 烟火实际宿主与光学积分（2026-10-26）

新增 GPU 烟火宿主桥，直接借用服务标量缓冲和实际消费 fence；每帧独立 SRV，像素/计算联合读取后恢复求解器状态，不回读场值。兼容档按所有烟场区间合并消光、RGB 散射与 Kelvin 艺术发射后一次积分；froxel 档裁切每个深度段与烟盒的真实相交长度，按半格长最多八点构造平均源，复用统一前缀。三类雾纹理升为 RGBA32F；存在烟源时拒绝旧静态历史，未将此称为动态时域完成。截图记录实际源版本，不以程序结束时步数代替截图版本。

- [宿主 20261026T002737Z](../reports/0.11.0/31c00bc53c0b/20261026T002737Z/smoke-host-evidence.json)：16 个相关场景、50 项光学检查全部通过，实际 D3D12 错误 0。截图版本 478，均匀密度 0.4979167；两档透射均为 0.3686275，独立 Beer–Lambert 参考 0.3694155；双源为 0.1372549，参考 0.1364678。两档发射、RGB 散射、前景遮挡、立即 stop 后画面/资源归零及两次 renderer 重建均通过。已目视检查吸收与发射截图。
- [实例服务 20261026T001107Z](../reports/0.11.0/31c00bc53c0b/20261026T001107Z/smoke-service-evidence.json)：7 组/694 检查、576 次 CPU/GPU 比较及 14 项相关 JS 测试；新增光学配置的认证加载/非法参数拒绝，实际调试错误 0。
- [原场事务宿主 20261026T002855Z](../reports/0.11.0/31c00bc53c0b/20261026T002855Z/effects-deposit-host-evidence.json)：复用同一冻结宿主，2 场景/8 检查通过，正常发布、最后来源撤销、资源归零与 CPU 权威不变，实际调试错误 0；既有 warning 如实保留。
- [收尾来源审计](../reports/0.11.0/31c00bc53c0b/20261026T003614Z-smoke-render-final/smoke-render-finalization.json)：869 次冻结核验、880 项检查全部通过。并行植被开发使 renderer 两文件、vegetation 两文件和 shader 在捕获后改变，已单列；本批结果仅证明冻结源码版本，未声称整个最新宿主已验收。

保留了缺合成标志、创建时提前读取能力缓存、旧 froxel 边界采样偏差和并行 mesh shader 中间状态的失败证据；修复后重新捕获源码并验证，未覆盖失败报告。构建只包含宿主所需依赖，未运行全量测试、全量回归、verify 或 CI。

仍缺烟场 WF 发布/覆盖去重、云烟共同积分、烟自阴影及介质对灯光遮蔽、相机/速度/深度/版本辅助时域、热浪、场景 SDF、玩法粗代理、四系统时间预算与工具；D 以及 A–G 整体保持部分完成，goal 保持 active。

## 求解后烟密度到共享 WF 的数值桥（2026-10-26 后续）

新增 `smoke/field_input.hpp` 与 `GpuLocalFields::replaceScalar`。直接读取实际 GPU 求解后的 kg/m³ 密度，拒绝网格/设备/真实 fence 时间线不一致、部分覆盖和超量化范围；在细格量化后复用既有不可变投影、64 位积分/残差与视觉 WF 发布。LEF 保留独立 localVersion 和原求解 inputVersion，不将 kg/s 粒子源重复当成烟浓度；温度/速度加权仍是独立合同。元数据准入失败不调用生产者 prepareRead、不录制场命令。

纯 `current` 检查在解引用缓冲前拒绝已推进或关闭的求解器；未准入描述符不额外保留 GPU 分配。已准入输入持有一个实际资源引用，生产者同步登记消费 fence；完成前拒绝关闭，完成后释放。覆盖双 tick 同 fence 下先导入再推进、源关闭后旧投影继续读取。该描述符是同步借用，不是无限期历史快照。

- [烟密度桥 20261026T004900Z](../reports/0.11.0/31c00bc53c0b/20261026T004900Z/smoke-fields-evidence.json)：8 组/216 检查、87 次独立比较，实际 890M 调试错误 0。五种细/粗比例、负坐标、部分粗格、钳位与量化残差通过；双源读值 192，新版本替换后 256，撤销一个来源后 128，全部撤销后 Unavailable；旧视图保持原值。仍有读取租约时保留未知覆盖页，退还租约/视图后分配归零。
- [原 LEF 20261026T004855Z](../reports/0.11.0/31c00bc53c0b/20261026T004855Z/fields-local-gpu-evidence.json)：9 组/2363 检查、274 次 CPU 粗格比较；[原 WF 20261026T004904Z](../reports/0.11.0/31c00bc53c0b/20261026T004904Z/fields-world-gpu-evidence.json)：15 组/1681 检查、128 次 CPU 查询比较，两者实际调试错误 0。
- [最终来源审计](../reports/0.11.0/31c00bc53c0b/20261026T004939Z-smoke-fields-final/smoke-fields-finalization.json)：129 次当前比较/129 次冻结核验、143 检查通过，相关当前输入零差异。清单 108 核，仅新增 scalar import；原 107 定义、原场数值核和烟火后端算法保持不变。

保留 004104Z/004215Z/004453Z 三次失败报告：测试曾误把最终无贡献的读取状态当有效零，随后误要求仍持有读取租约时页数归零；按现有未知覆盖合同修正测试，未把实现改成无覆盖的已知空气。004604Z 和 004656Z 的中间通过也保留；之后调整借用描述符不持有预算外缓冲，并重新运行以上受影响专项。

详见 [密度投影合同](SMOKE-FIELD-PROJECTION.md)。本批仅构建必要依赖并做相关增量回归，标准 build/verify 只登记未来入口，未运行全量测试/全量回归/CI。宿主自动租约与发布调度、观察提升/降级、渲染 LEF/WF 同源覆盖去重、温度/速度、联合时间预算及其他 A–G 目标仍待完成，goal 保持 active。

## 烟密度自动共享、按源选择及小烟域光学修复（2026-10-26 后续）

新增 `GpuSmokeSharing`：通过服务弱引用获取当前密度，按视锥/订阅/显式观察提升，复用原字段事务的来源替换、撤销与确认；无观察者只登记元数据，降级确认后释放可选字段。发布按优先级、同级最早等待、serial 排序；预算不足不导入或伪造新源时间，继续处理可容纳候选，恢复后发布实际最新版本。暂停保留 sourceTick，尾烟与粒子句柄独立；同 tick 停止先拒绝新视图，下一个真实 tick 正式撤销，旧视图仍受真实读取 fence 保护。注册/局部分配在事务检查点外完成，避免回滚使本地句柄失效。

宿主每 serial 每帧只选一种表示：16 米内使用详细 LEF，较远且无需温度发光时使用当前已确认版本的 WF 分源密度投影；延期、网格不支持或版本不匹配时保留详细场。不是将所有来源聚合密度套用同一光学参数；不同来源分别保留各自消光/散射合同，再共同积分。截图提供 representation、scalarElements 和 cellMetres，证明实际读取的来源、量化格式与降采样尺寸。

真实 8→1 场景暴露原 froxel XY 光学重建使小烟域变淡：透射 0.725054，参考 0.607716。修复为低频入射光缓存 + 最终像素/透明表面实际区间积分，雾与烟的消光/散射/发射仍共同积分一次，不靠提高整体网格分辨率。追加两个描述符，不移动原银行；80×45×48 RGBA32F 缓存本机对齐后增加 2,949,120 字节，进入纹理计费。随后组合场景发现关闭 toneMap 时兼容路径丢弃背景雾，已修复 DXIL 返回/透明分支和 DXBC 同等源逻辑；实际硬件执行的是 SM 6.6 路径，不将其当作低 SM 硬件实测。

本批仅执行相关增量：

- [原生自动共享 20261026T054417Z](../reports/0.11.0/31c00bc53c0b/20261026T054417Z/smoke-sharing-evidence.json)：6 组/240 检查、16 次密度/WF 比较，含显式优先级、旧等待、公平恢复、订阅/驻留/降级、失效网格、同 tick 停止、不可变旧视图、独立尾烟及实际队列阻塞，890M 实际调试错误 0。
- [原实例服务 20261026T054131Z](../reports/0.11.0/31c00bc53c0b/20261026T054131Z/smoke-service-evidence.json)：7 组/694 检查、576 次 CPU/GPU 比较及 14 项相关 JS 测试，实际调试错误 0。
- [最终宿主 20261026T062612Z](../reports/0.11.0/31c00bc53c0b/20261026T062612Z/smoke-host-evidence.json)：33 场景/134 光学检查全部通过，实际调试错误 0。详细 8 格/0.5m 与 WF 1 格/1m 的列积分参考分别 0.607796/0.607716，三张实测均 0.607843；混合来源、前景遮挡、温度发光回退、背景雾、停止后资源/画面归零、两档各两次 renderer 重建均通过。已目视检查最终真实粗格与混合来源截图。
- [原场事务宿主 20261026T061615Z](../reports/0.11.0/31c00bc53c0b/20261026T061615Z/effects-deposit-host-evidence.json)：复用同一最终冻结二进制，2 场景/8 检查通过，实际调试错误 0。
- [最终来源审计](../reports/0.11.0/31c00bc53c0b/20261026T063410Z-smoke-sharing-final/smoke-sharing-finalization.json)：1489 次冻结核验、1535 项检查通过；当前共享/光学入口文件及主 renderer 接线均保留。捕获后 host/scene loader、renderer 公共/内部、game、gameplay snapshot 和 material shader 等 8 项并行修改单列；currentWholeHostValidated=false，仅验收冻结版本。

保留 [055239Z 小烟域失败](../reports/0.11.0/31c00bc53c0b/20261026T055239Z/smoke-host-evidence.json)、[060711Z 背景雾失败](../reports/0.11.0/31c00bc53c0b/20261026T060711Z/smoke-host-evidence.json) 和 [061615Z-fc463c 来源拒绝](../reports/0.11.0/31c00bc53c0b/20261026T061615Z-fc463c/smoke-host-guard-rejection.json)。最后一次虽然 134 项画面检查通过，但作者模块 engine/scene/index.mjs 在执行期间变化，原门禁仍按失败保留；实际场景/资产/二进制均未变，未放宽检查，按稳定后的作者来源重新生成夹具并完整重跑了上述相关画面专项。没有把拒绝记录改成通过。

入口为 smoke:sharing 与 smoke:host；标准 build/verify 仅登记未来入口，未执行全量测试、全量回归或 CI。当前烟密度视觉 WF 尚未与粒子沉积/CPU 权威整合成全局统一服务；还缺任意网格守恒投影、温度/速度、可编辑渐变 LOD、跨域预算、烟自阴影、完整时域、云烟统一与玩法适配。低分辨率夹具不证明标准负载性能，B/D 以及 A–G goal 均保持部分完成。

## 单一视觉 WF 的双生产者共同发布与事务权限（2026-10-26 后续）

新增 GpuFieldTransaction，将同一 GpuVisualWorld 的多个共享控制器纳入一个检查点、一个提交和一组版本确认。局部求解不回滚；容量拒绝或作用域取消恢复全部参与者的写租约、来源、epoch、共享状态和观察配额。注册及消费者租约变更必须在共同事务外完成；未参与控制器、原始写接口和单独 commit 不能绕过共同边界，settle 必须在提交后执行。事务开始前已有未提交来源/撤销/删除，或参与者仍待确认时拒绝开始。控制器析构先取消活动共同事务再释放其资源，避免孤立检查点或泄漏写租约。

新增 context 元数据借用与全局 subscribers 查询。实际 GPU 粒子池与烟求解器用不同名称/单位/owner/定点比例的通道，共享一个世界场；两者 sourceId 都为 1，身份仍隔离。V1 为粒子 16/烟 256；错误单位的烟投影拒绝后，粒子暂存一起取消；保存正确投影重试得到 V2 的 32/512，无需重复求解。独立撤销后旧视图不变，最终两域 Unavailable，资源按真实 fence 回收。视觉场不接受 CPU 权威或 Strong 租约。

本批只运行以下相关增量：

- [场核心 20261026T072116Z](../reports/0.11.0/31c00bc53c0b/20261026T072116Z/fields-world-gpu-evidence.json)：17 组/1802 检查、128 次 CPU/GPU 查询比较，覆盖双控制器原子提交/固定快照预算回滚/重试、第三控制器和原始写接口隔离、事务前暂存/未确认/删除拒绝、析构取消与旧视图不变。
- [真实双生产者 20261026T072113Z](../reports/0.11.0/31c00bc53c0b/20261026T072113Z/fields-joint-gpu-evidence.json)：1 组/76 检查、14 次真实 GPU 查询比较，目录单位检查、共同版本、独立退休及资源归零通过。
- [原沉积生产服务 20261026T072124Z](../reports/0.11.0/31c00bc53c0b/20261026T072124Z/effects-deposit-service-evidence.json)：27 组/432 检查，原准入、移动、预算恢复和真实 fence 生命周期通过。
- [原烟自动共享 20261026T072124Z-bc1bd8](../reports/0.11.0/31c00bc53c0b/20261026T072124Z-bc1bd8/smoke-sharing-evidence.json)：6 组/240 检查、16 次密度/WF 比较通过。
- [来源审计 20261026T072346Z-fields-joint-final](../reports/0.11.0/31c00bc53c0b/20261026T072346Z-fields-joint-final/fields-joint-finalization.json)：250 项核验、212 项冻结来源与212 项当前来源比较全部通过；四份报告相关输入零差异，数值 shader 与本批权限修复前的通过版本相同。新双生产者/烟共享入口执行前冻结；旧场/沉积服务入口执行前后校验、执行后归档，未混淆证据顺序。

全部 GPU 专项在真实 AMD 890M 上运行，D3D12 调试错误均为 0。新入口为 fields:joint:gpu，标准 build/verify 只登记未来入口，没有执行全量构建、全量测试/回归、verify 或 CI。完整合同见 [FIELD-JOINT-PUBLICATION.md](FIELD-JOINT-PUBLICATION.md)。

本批尚未拆分现有生产服务的本地准备/事务内暂存/提交后确认，也未迁移 renderer；粒子沉积和烟自动共享在现有宿主中仍使用各自世界场。CPU 权威/玩法/存档、GPU 页池/多级分辨率、全局时间预算及其他 A–G 目标保持未完成，goal 保持 active。后续必须对齐真实 simulationTick、清除延后元数据中的回调期池指针、只计一次世界场预算并接入全局消费者，不能直接注入同一世界场后仍串行各自 commit。

## 粒子沉积生产服务阶段拆分与外部共同 WF（2026-10-26 后续）

GpuFieldDeposits 新增接收外部 GpuVisualWorld 和显式通道 ID 授予的构造方式，完整目录可包含其他视觉 owner 和 CPU 权威定义，但本服务只能写被授予的非负视觉标量。私有字段使用同一目录/网格/device/fence；全局只读租约直接触发发布需求，不再依赖仅本服务的读取包装器。外部世界场的显存不重复计入服务 allocationBytes，关闭本服务不销毁全局世界场。

生产步骤拆为 prepare、stagePublication、finishPublication。prepare 在原后模拟回调中消费借用粒子池并生成 LEF，保留的元数据全部清除 emitter 池指针；局部 tick 在此推进。注册/重建在共同检查点外完成，stage 只接受共同事务内相同命令列表与真实 fence，按原预算与队列暂存；统一 commit/rollback 后才 finish，成功快照 tick 必须与准备 tick 相同。失败保留实际最新 LEF，不能要求重新模拟。原 record 通过单参与者共同事务继续提供独立服务路径；外部共同模式拒绝自行 record 提交。flush 仍只同步生命周期。

- [最终服务 20261026T112705Z](../reports/0.11.0/31c00bc53c0b/20261026T112705Z/effects-deposit-service-evidence.json)：28 组/475 检查通过。新增真实共同模式验证 local 资产被全局订阅触发、两个 owner 同一快照 250/64、固定一个快照预算共同回滚、本地 sourceTick 保持前进、恢复到最新 tick 后 250/128、最后两域 Unknown/Unavailable、零保留池指针以及服务独立关闭后外部世界场继续可用。原 27 组全部保留，实际 890M 调试错误 0。
- [宿主增量 20261026T112840Z](../reports/0.11.0/31c00bc53c0b/20261026T112840Z/effects-deposit-host-evidence.json)：仅正常发布/停止回收两个场景，8/8；原独立路径 tick=478/commit=476，最后停止 commit=19/派发=36/字段资源归零，CPU 权威摘要一致。实际调试错误 0，原 760 条警告如实保留。仅构建独立 build/field-phase-host 宿主依赖，没有执行原生全部测试程序。
- [来源复核 112916Z](../reports/0.11.0/31c00bc53c0b/20261026T112916Z-fields-joint-final/fields-joint-finalization.json)：250 项通过，复用未改变依赖的场核心、真实粒子/烟共同发布和烟共享专项，与最新服务相关来源均匹配，未额外重复运行它们。
- [收尾审计 113034Z](../reports/0.11.0/31c00bc53c0b/20261026T113034Z-deposit-phases-final/deposit-phases-finalization.json)：360 项核验、342 项冻结来源核对全部通过，服务/宿主输入零差异。宿主二进制/来源执行前冻结；旧服务入口为执行前后校验、执行后归档，不混淆冻结顺序。

保留 [112347Z 失败](../reports/0.11.0/31c00bc53c0b/20261026T112347Z-deposit-phase-failure/failure.json)：新测试夹具假设 1:1 网格却沿用默认 2:1，原 27 组后新断言失败；失败源码/二进制匹配并归档，显式修正夹具后重跑，未改数值算法。随后让测试中的协调器先于服务销毁，再按最终来源重跑。较早 111856Z/112431Z 通过版本也保持原样。

详见 [FIELD-DEPOSIT-PHASES.md](FIELD-DEPOSIT-PHASES.md)。仅运行本次相关增量；没有全量构建、全量测试/回归、npm test、verify 或 CI。烟共享生产服务尚未拆分，renderer 尚未将两项生产服务接到同一真实 tick 的共同提交；CPU 权威/玩法/存档和其余 B–G 仍未完成，goal 保持 active。

## 烟共享生产阶段与真实双服务同 tick 联合发布（2026-10-26 后续）

GpuSmokeSharing 新增外部 GpuVisualWorld 与显式物理密度通道，严格核对 kg/m³、owner、视觉域、合并和量化比例；目录名称可以唯一重命名。阶段拆为 prepare、stagePublication、finishPublication，原 sync 内部通过单参与者共同事务保持独立路径。prepare 保存元数据后清除四个借用求解缓冲指针，需求读取全局只读租约；stage 验证当前生产签名与准备时 serial/sourceTick/version 完全一致，再按原预算导入/投影。simulated=false 只刷新生命周期，不建立待发布帧或虚构下一模拟 tick。外部共同模式不得自行 sync/commit，世界场字节不重复计费，服务退出不销毁全局场。

新增通过正式作者编译/cook 认证的双用途资产及 fields:services:gpu 专项。真实 GpuRuntime 先执行粒子 prepare，再执行实际烟求解 hooks 和烟共享 prepare；两个生产服务使用同一个实际 tick，随后共同暂存/提交并各自确认。全局目录保留粒子 density/density-q 与物理 smokeDensity/kg/m³ 两种明确不同的通道，未把相同整数误当相同物理量。

- [最终联合 20261026T114614Z](../reports/0.11.0/31c00bc53c0b/20261026T114614Z/fields-services-gpu-evidence.json)：2 组/124 检查、14 次 GPU 数值比较。tick0 V1=2/2；固定一个快照预算时 tick1 两域一起延期且实际求解继续；释放旧视图后 tick2 V2=2/6；暂停 tick3 V3=2/6 且 sourceTick 保持2；停止 flush 不提交虚构tick，固定旧视图使 tick4 撤销一起回滚，释放后 tick5 V4 两域 Unavailable。源在准备后停止时，先暂存的粒子发布与过期烟一起取消，未导入悬空数据，生命周期刷新后可正常回收。服务退出后外部场及消费者继续可用。
- [原烟共享 20261026T114226Z](../reports/0.11.0/31c00bc53c0b/20261026T114226Z/smoke-sharing-evidence.json)：6 组/240 检查、16 次密度/WF 比较通过，保留暂停、观察/驻留、配额/背压、旧视图、尾烟及真实队列验证。
- [联合来源审计 114737Z-field-services-final](../reports/0.11.0/31c00bc53c0b/20261026T114737Z-field-services-final/field-services-finalization.json)：149 项核验、132 项冻结来源核对通过，相关当前输入零差异，数值 shader 未改。
- [旧字段/粒子来源复核 114737Z-fields-joint-final](../reports/0.11.0/31c00bc53c0b/20261026T114737Z-fields-joint-final/fields-joint-finalization.json)：250 项核验通过，未改变依赖的原场核心/双求解者及 28 组粒子服务仍匹配。没有为没有变化的依赖重复运行 GPU 专项。旧审计的生产宿主未整合标记不表示本批两个服务的阶段接口未实现。

两个执行专项均在真实 AMD 890M 上运行，D3D12 调试错误0；二进制、递归来源、夹具/cook 和生成 shader 执行前冻结，执行期间零输入变化。较早 113626Z/114229Z 通过版本原样保留，后者为加入准备后生产者变化验证之前的1组版本。标准 build/verify 只登记未来入口，没有执行全量构建、全量回归、npm test、verify 或 CI。

完整合同见 [FIELD-SERVICE-JOINT-PUBLICATION.md](FIELD-SERVICE-JOINT-PUBLICATION.md)。renderer 仍使用原独立场及帧末烟 sync；全局协调器、查询路由/same-tick 新视图失效、跨域总预算和 CPU 权威整合尚未完成。当前测试验证烟服务包装器的退休拒绝，直接底层世界场仍可取旧提交快照，不声称全局查询生命周期已完成。B/D 和全部 A–G goal 保持部分完成/active。
