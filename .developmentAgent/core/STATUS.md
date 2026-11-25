# 项目状态与移交说明

- 版本：0.11.0（刚体物理、物体破坏、结构坍塌、物理证据门禁）
- 日期：2026-10-22
- 当前形态：Windows 11 x64 + D3D12 独立原生宿主（光栅 + DXR 静态阴影 + 确定性刚体破坏）+ Web/Electron 编辑器 + Node.js 确定性场景事务层 + 本地 MCP 工具服务 + 可重放 CI
- 不包含：内嵌实时 GPU 视口、定制 Chromium、SM 6.6 直接堆索引材质主路径、放大剔除、运行时 glTF 导入、面部、模型调用、运行时 Nanite/Lumen、凸 Voronoi 碎片渲染与凸包碰撞、场景级关节、全身软体/布料、双向角色刚体

## 物理规模化：宽相 / 关节 / 键合破坏 / Voronoi 烘焙（2026-10-22）

按 [PHYSICS-SCALE-PLAN.md](PHYSICS-SCALE-PLAN.md) 的顺序落地四项：

1. **BVH 宽相**（已完成）：`engine/native/broadphase.hpp` 确定性动态 AABB 树，小场景自动退回 O(n²)；候选对排序后进窄相，与旧路径逐位一致。167 体 1.71→1.24 ms/步，1,001 体 46.6→13.1 ms，候选对 500,500→1,039，接触数不变。
2. **XPBD 关节**（已完成）：Point/Hinge/Rope，铰链用两点约束 + 角度限位，`breakForce` 断裂、`collideConnected` 接触过滤；作为位置阶段的附加约束层，不替换顺序冲量。已进场景格式（`joints` 数组，按 entity id 引用刚体，三处校验），支持**弹簧**（`compliance`）、**马达**（`motorSpeed` + `motorMaxTorque`，位置级驱动）与**关节摩擦**。演示场景 `.screen/joint-yard.scene.json`（马达闸门 / 可撞开的门 / 绳索吊灯）；关节状态进 `snapshot` 与 digest（`nativegpu.game/4`）。
3. **键合破坏**（已完成）：`destructible.mode: "bonded"`，cell + bond + 能量阈值 + 并查集；打一枪开局部洞、墙不倒，失去支撑的岛转动态。`native_fracture_tests` 6 项 + `native_destruction` 键合用例（含存读档往返）；演示场景 `.screen/bonded-wall.scene.json`。证据用可复现的 `--shot frame:x,y,z` 采集：同相机下两发偏移命中使 `bodies 9→57`、`bondedHoles 0→4`、`bondedBrokenBonds 0→14`，而 `fractures` 仍为 `0`、`intactDestructibles` 仍为 `2`。
4. **离线凸 Voronoi**（4a/4b/4c/4d 全部完成）：`tools/offline/voronoi.mjs` 输出凸单元 + 邻接 bond，schema `nativegpu.voronoi/1`，6 项 Node 测试。**4b** 凸网格实例渲染：所有碎片拼进一对顶点/索引缓冲，绘制只改字节偏移；碎片按真实凸包渲染（4c 之后物理也用凸包，包围盒只留作宽相与回退）。**4d** 断面 UV 按实体包围盒投影（碎片与完好墙采样同一片纹理）、切割面单独成网格并加深染色、`bake-scene` 支持改单元数后一键重烘。**4c** 凸-凸窄相：`engine/native/convex.hpp` 提供 GJK 重叠判定 + EPA 穿透 + 面裁剪流形，碎片按其真实凸包碰撞，三态返回（分离/接触/失败）保证失败时回退 SAT 而不丢碰撞；8 项解析解单测 + 行为测试（盒代理永不倾倒、凸包会倒且重心更低）。

## 非机器学习动作 M1–M4（2026-10-22）

新增传统动作匹配、程序化双骨 IK / Look-at、脚锁定与移动表面查询、GPU morph 与规则肌肉，以及局部四面体 XPBD / 胶囊碰撞 / LOD / 存档续跑。已接入默认角色与原生 GPU 路径；M5 面部未实现。默认原创夹具不是生产动捕库，通用重定向、全身 IK 与大型动作索引仍为扩展。运行 `npm run animation` 验收，接口和限制见 [ANIMATION-M1-M4.md](ANIMATION-M1-M4.md)。

## 刚体物理与破坏（0.11.0，2026-10-21）

新增 `engine/native/physics.{hpp,cpp}`：120 Hz 固定步长、15 轴 SAT 盒体接触、裁剪面流形、顺序冲量、摩擦/恢复/休眠、自适应子步防穿透。`Game` 层提供冲量驱动的伤害累积、盒式预碎裂、碎片继承速度、径向爆炸、弹道投掷、结构支撑图与级联坍塌；`physics`/`destructible` 两个场景字段在 JSON Schema、C++ loader、Node validator 三处同步，并通过事务层提交。

本机证据（`reports/physics-evidence.json`）：11/11 可破坏对象断裂、142 块碎片、3 次结构坍塌、166 个活动刚体、物理平均约 0.6–1.0 ms/步、最大约 2.7–4.2 ms（宽相换 BVH 后；断裂计数与峰值冲量不变，同一二进制多次运行的差异来自机器负载）；两次运行最终摘要一致，前后截图平均绝对误差 2.57。交互 `F` 投掷、`G` 爆炸；`npm run physics` 重放全部检查。

边界：只有盒体；碎片是递归二分盒块而非凸 Voronoi；爆炸伤害无遮挡；角色用 AABB 扫掠刚体当前 AABB（不是动态胶囊）。宽相已从 O(n²) 换成确定性动态 AABB 树（小场景自动退回 O(n²)，1k 体实测 46.6 → 13.1 ms/步，接触流逐位不变）。论文对照、创新点与改进路线见 [PHYSICS-DESTRUCTION.md](PHYSICS-DESTRUCTION.md)，BVH / XPBD 关节 / 键合破坏 / 凸 Voronoi 四阶段方案见 [PHYSICS-SCALE-PLAN.md](PHYSICS-SCALE-PLAN.md)。

## 粒子场首条链路（F1–F3，2026-10-21）

`--particles on` 打开一条完整的 GPU 粒子 → 场 → 求解器 → 体积渲染链路：65536 个结构化粒子在 compute 队列上按固定步长积分，用**整数原子加**写进 96³ 的 `R32_UINT` 密度/热量体积；整数体素解析成浮点状态后，进入一个真正的欧拉求解器——浮力、半拉格朗日速度/状态平流、**涡量约束**、散度、20 次 Jacobi 压力迭代、压力梯度扣除——最后交给全屏 ray march 合成进 base colour。粒子还与场景碰撞体求交：AABB 从场景图上传，穿入的粒子沿最浅轴推出并阻尼反弹。

整数加满足结合律与交换律，所以 **splat 源项**与线程调度顺序无关，这是 [FIELD-PLAN.md](FIELD-PLAN.md) 第 7.2 节的确定性主张。**求解器本身是单精度的，不承诺确定性**，两者在文档里分开写。

篝火场景 [.screen/campfire.scene.json](../.screen/campfire.scene.json)（16 实体：地面、火塘、四根柴、八块围石、坐墩、玩家、火光）配合 `--fire` 预设与 `--explode-at N`：

- `--fire`：更小的发射半径、更高浮力、更强消光，火焰更紧凑、更亮；
- `--explode-at N`：第 N 个粒子帧起爆，源半径 ×9、外抛速度 ×7、温度 ×3.5，并向**速度场**注入一次径向冲击（`CSBuoyancy` 里的 blast 项），把已经存在的烟整体推开。

**分辨率跟随效果**：`--fire` 现在把场的包围盒同步收缩到 6 m（`setFieldBounds`），在同样的 96³ 网格下体素从 **16.7 cm 变成 6.25 cm**，火焰第一次有形状。默认烟羽仍用 16 m 盒子。这是稀疏砖块之前的第一步：先把"域"变成效果的属性，再谈"域内的按需分配"。

**通道租约（F4 最小切片）**：六个场通道（density/heat/velocity/pressure/divergence/curl）以引用计数 + 一致性等级（strong/eventual/stale）出租。模拟在创建时持有全部六个；体积渲染每帧临时租用 density/heat 并在绘制后归还。求解器先检查租约完整性，缺任何一个通道就跳过该帧并把 `leaseStalls` 加一——租约是**合同**，不是记账。报告里逐通道输出 `refs`/`level`/`acquisitions`。

**爆炸的场景后果**：宿主每帧轮询 `Renderer::particleBurst()`，一旦爆发强度超过 0.5，就把火塘 4.5 m 内的实体按距离衰减**焦化**（`scorched=14`）。这是纯视觉后果：场景文件不被修改，重新加载即恢复。

证据：`.incrementalTesting/simulation/particles.ps1` 21/21（开/关/关碰撞三遍）与 `.incrementalTesting/rendering/campfire.ps1` 17/17（场景校验 + 稳态火 + 爆炸帧 + 焦化后帧 + 租约断言）；数字来自 `reports/particles-evidence.json`、`reports/campfire-evidence.json`。

未实现且不声称：租约的**提升/降级**与 stale 通道的自动回收（只有 strong/eventual 两种等级在跑）。碰撞只对轴对齐盒生效（场景实体无旋转，AABB 是精确的），不与刚体双向耦合；火的形态是浮力 + 涡量 + 投影 + 爆发冲击的结果，不是完整的燃烧化学模型。

注意：`node tools/scene.mjs validate .screen/courtyard.scene.json` 目前失败，原因是材质校验器与 schema 的 wetness/wear 字段还在并行迁移中；原生加载路径不受影响，粒子证据仍用 courtyard 跑通。

## 场系统真实感四批（2026-10-21）

目标与验收标准见 [FIELD-REALISM-GOAL.md](FIELD-REALISM-GOAL.md)。四批改动都落在"系统属性"上，新效果不调色也继承。

**第一批 · 光学。** 发射项换成 Planck 黑体：`blackbody(T)` 在三波长（612/545/465 nm）采样并以 6500 K 归一，色相由温度唯一决定，不再有手调三段色带。散射用 Henyey-Greenstein 相位（`g=0.45` 主光 + `g=0.12` 补光两个 octave），逆光时边缘发亮。自阴影累积器另跑一套**半消光系数**的透射率，作为第二散射 octave 的代理，浓烟不会塌成一片灰。火本身作为一个位于发射器的内部点光，按距离衰减 + 朝火方向的 Beer 近似照亮自己的烟。

**第二批 · 模拟。** 状态平流升级为 MacCormack 二阶：前向半拉格朗日到 B、反向到 scratch、再用 `phi1 + 0.5·(phi0 - phi2)` 修正，并 clamp 到 6 邻域 min/max 防振铃，抑制一阶格式的数值耗散。湍流用 curl-noise：三个去相关值噪声势场取旋度，得到**无散度**扰动，火舌会撕裂游走而不会凭空压缩。splat 从单格原子加改为 2×2×2 三线性 tent 核——每个权重都是粒子位置的确定性函数，整数原子加仍与线程顺序无关，确定性合同不变。

**第三批 · 采样与时域。** 体积先在半分辨率 ray march，指数累积进历史缓冲，再用**深度感知双边上采样**合成回全分辨率；相机移动时 blend 置 1 硬重置，避免拖影。march 起点加逐像素逐帧哈希抖动，固定步长的 banding 被打散成噪声，由时域累积平均掉。`--particle-temporal off` 退回全分辨率直绘，着色模型不变。

**第四批 · 上下文。** 宿主每帧找距发射器最近的场景光源，按确定性噪声闪烁（0.82±0.14）、爆炸时强度 ×3.6、位置抖动几厘米，几何、雾、烟羽共享同一个光照事件。

**两个根因修正（本轮才发现）**

1. *黑体发射根本不可见*：原实现 `pow(blackbody(T), 0.35)`，而 1400 K 的 Planck 比值只有参考的 1e-6，任何幂次都抬不到可见亮度，冷焰区等于全黑，画面上只剩灰白烟柱。改为**色相与亮度解耦**——`blackbody` 归一化到单位亮度只负责色相，亮度走 `0.12 + 2.4·heat²`。现在焰心 3200 K 暖白、焰身 1900 K 橙、焰梢 1200 K 暗红，色温梯度按物理走。
2. *夜景无法变暗*：legacy 光栅着色器把主光项硬编码为 `albedo · keyDiffuse · 0.95`，完全忽略 `lighting.keyIntensity`，所以 ambient 调到 0.014 地面仍是白昼亮度。把旧的 padding 槽位改成 `keyTint = keyColor · (keyIntensity / 1.30)` 并乘进主光项（纹理路径的 `1.30` 同样处理）：默认环境乘数为 1.0，courtyard 基线**字节不变**（图像回归 SSIM 1.0、meanAbsError 0），夜景 0.09 时主光降到 7%，火才成为唯一主光源。

**验收场景** [.screen/campfire-night.scene.json](../.screen/campfire-night.scene.json)：夜间林间空地，21 实体（地面、火塘、柴堆、八块围石、两个树桩、三棵松、帐篷、玩家、Firelight 点光源），`fogDensity=0.012`、`exposure=0.7`、火光强度 6.0 / 距离 7.0。火光强到能把围石和地面照出一圈暖色光池，地面在线性空间约 0.07，火焰核心约 4。

证据：`.incrementalTesting/rendering/campfire-night.ps1` 21/21（场景校验 + 顺光/逆光/侧光三视角 + 时域回退 + 21 项断言），`reports/campfire-night-evidence.json`；`.incrementalTesting/simulation/particles.ps1` 21/21、`.incrementalTesting/rendering/campfire.ps1` 17/17 仍全绿；`node --test` 148/148；原生测试 0 failed；图像回归 4/4。

明确未做：场景阴影贴图进入体积步进、屏幕空间热浪折射、稀疏砖块场、燃烧化学模型——这四项写在 goal 的"明确不做（本轮）"里，不在本轮声称范围内。

## 动作系统首条链路（E04，2026-10-20）

默认原生小院已使用离线导入的原创 16 骨骼角色和六段 60 Hz 动作，通过 VS 蒙皮绘制。固定 120 Hz 状态机、SoA 双缓冲、Trait DAG、阶段哈希、解析惯性化、分层 Montage 和 Notify 已接入 Game 与回放摘要；根位移可提取，小院仍由碰撞控制器驱动平移。`npm run animation` 生成独立证据，`verify` 和运行包已接入。详细支持子集、命令与限制见 [ANIMATION.md](ANIMATION.md)。

此段为 2026-10-20 的 E04 历史范围。2026-10-22 已补齐 M1–M4 首版：动作数据库 / 精确搜索、Rig / 脚接触、GPU morph / 肌肉和局部软组织，见本页顶部更新。E04 仍有 KD-tree、通用重定向、距离匹配 / 全身运动扭曲、生产内容与面部等扩展；DXR 不包含角色变形 BLAS。证据以实际报告为准，不把空阶段哈希当成功能实现。

## Web 编辑器初版（2026-10-20）

用户明确选择“Web 编辑器 + 原生引擎”，现行架构与启动入口见 [EDITOR.md](EDITOR.md)。`npm run editor` 启动 Electron，`npm run editor:web` 提供本机浏览器版本。界面支持实体选择、属性草稿、事务差异预览、提交、撤销、道具新增与实体删除；场景修改复用现有 Node 事务层。

编辑器可控制自己启动的独立原生预览窗口，并按需生成真实原生 PNG 快照。二维布局不是三维渲染；快照不是实时流。尚无共享纹理、内嵌实时视口或新的模型调用，G2 共享纹理验收仍未完成。原先“先冻结并构建 Chromium 源码”的路线暂缓，不再作为当前界面开发前置。

## 1c. 第一批并行工作包（0.7.0）

| 工作包 | 产物 | 证据 |
| --- | --- | --- |
| E07 确定性契约 | `engine/native/determinism.*`、`Game::digest`、`--hash-log`/`--seed` | `.incrementalTesting/core/determinism.ps1` 19 项；两次运行逐字节相同；分辨率/调试视图无关；静态门禁 |
| R08 帧预算管理器 | `host/budget.*`、`--budget*`、报告 `budget` 段 | `.incrementalTesting/rendering/budget.ps1` 18 项；`tests/budget_tests.cpp` 28 项；强制超预算走到阶梯末端 |
| INN-14 几何包 + N0 骨架 | `tools/offline/geometry-package.mjs`、`cluster-dag.mjs` | 确定性重建、37 cluster / 6 LOD、覆盖与篡改检测；`tests/offline.test.mjs` 6 项 |
| H07 输出契约 | `docs/OUTPUT-CONTRACT.md` | 逐条对照真实渲染器符号，未实现项全部按"S2 要求"标注 |

详见 `docs/DETERMINISM.md`、`docs/BUDGET.md`、`docs/GEOMETRY-PACKAGE.md`、`docs/OUTPUT-CONTRACT.md`。

## 1f. 第 4 批（0.10.0）

| 工作包 | 产物 | 证据 |
| --- | --- | --- |
| 架构冻结 | `docs/GEOMETRY-LAYOUT.md`：48B ClusterRecord、InstanceRef/ClusterRef、可见性缓冲、32 位材质句柄、v1→v2 迁移、B2→B4→B5→B3b→N1→L1 硬门 | 文档 + `GeometryClass` 路由与句柄单测（17 项断言） |
| 几何分类 | `engine/native/geometry_class.*`、报告 `geometryClasses` | 小院 17 实体：16 Simple + 1 Deformable + **0 Cluster**（12 三角形方盒无 cluster 收益） |
| 材质句柄 | `engine/native/material_handle.*`、报告 `materialHandle` | 越界编码返回 false 而非截断；极值往返正确 |
| 几何包 v2 | `lodError`/`flags`/`materialHandle` + `capabilities`，v1 可读 | 摘要 `f3ddb1e4…`；v1 verify exit 0；6 项 v2 测试 |
| 查询 DSL | `engine/scene/query.mjs` + `scene query` | 18 项测试；小院墙 5、材质 3 组、灯光强度和 9.7 |

## 1e. 第三批（0.9.0）

| 工作包 | 产物 | 证据 |
| --- | --- | --- |
| B1 compute 队列 | `renderer/compute_pass.cpp`、`shaders/compute_probe.hlsl`（cs_6_6） | `.incrementalTesting/rendering/compute.ps1` 15/15；240 帧 240 次派发、240 次跨队列等待；结果未进画面 |
| B3 SM 6.6 堆索引 | `ResourceDescriptorHeap[32..35]` vs 描述符表 | 4/4 采样逐位相同（maxDelta 0）；材质主路径仍是有界表 |

## 1d. 第二批并行工作包（0.8.0）

| 工作包 | 产物 | 证据 |
| --- | --- | --- |
| R06 RHI 边界 | `renderer/rhi.{hpp,cpp}`、`rhi_d3d12.cpp`；帧路径经接口，报告 `rhi` 段 | `tests/rhi_conformance.cpp` 13/13；smoke 摘要与迁移前逐位一致；`docs/RHI.md` |
| R07 误差契约 | `tests/image-contracts.json`（5 条）、`.incrementalTesting/rendering/error-contract.ps1` | 5/5 通过；阴影开关 11.73% 变化、回退逐像素相同；`docs/ERROR-CONTRACTS.md` |
| E08 回放与黑匣子 | `--record` / `--replay` / `--replay-strict` / `--stall-*`、anomaly sidecar | `.incrementalTesting/core/replay.ps1` 25/25；240 帧 0 不符；篡改定位到帧；`docs/REPLAY.md` |
| B7 glTF 子集 | `tools/offline/gltf.mjs` + 可复现 fixtures | 6/6 测试；quad 4 顶点 2 三角形；包摘要 `9e9a242a…` |
| L0 SDF 烘焙 | `tools/offline/sdf-bake.mjs` | 8/8 测试；球面中心最大误差 0.0587 体素；`docs/SDF-BAKE.md` |
| S3E-07 设计期 lint | `engine/scene/lint.mjs`（12 规则）、`tools/scene.mjs lint` | 11/11 测试；小院 0 findings；`docs/SCENE-LINT.md` |

## 1. 本次推进内容（工作包 2：DXR 静态阴影）

1. **着色器与管线**：新增 `shaders/rt_shadows.hlsl`（raygen / miss / closest-hit），用固定 DXC 1.9.2609
   编译为 `lib_6_5` DXIL 并内嵌；创建 RTPSO（DXIL library + hit group + shader config + pipeline config +
   global root signature）和 192 字节 Shader Binding Table。
2. **渲染帧图**：光栅 pass 改为双渲染目标（`sceneBaseColor` = 环境光 + 点光，`sceneKeyLight` = key light 项），
   深度改为 `R32_TYPELESS`（DSV `D32_FLOAT` / SRV `R32_FLOAT`）；DXR pass 从深度重建世界坐标逐像素发射遮挡光线，
   写入 `R16_FLOAT` 可见性贴图；合成 pass 计算 `base + key * mask` 并复制到交换链。
3. **回退与开关**：`F10` 与 `--rt-shadows on/off`；关闭时把 mask 清成 1，结果与 0.3.0 纯光栅图像逐像素一致，
   不需要第二条渲染路径。
4. **真实实例输入**：TLAS 从当前帧的刚体 `DrawItem` 变换构建（本机 33 个实例），每帧重建；
   单位方盒 BLAS 复用，不含蒙皮。
5. **报告字段**：`probes.raytracingShadows` 区分 `probeAvailable`、`dxcAvailable`、`requested`、`enabled`、
   `rtpsoCreated`、`sbtCreated`、`effectImplemented`、`fallbackUsed`、`shadowTextureBytes`、`dispatchedFrames`、
   `fallbackFrames`、`dispatchFailures`、`gpuRtMs`、`tlasInstances`、`tlasRebuilds`、`error`。
   `capabilities.implemented.raytracing` 只在本次运行真的派发过光线时为 true。
6. **截图**：`--screenshot` / `--screenshot-frame` / `--camera` / `--render-size` / `--debug-view`；
   PNG 由内置无依赖编码器写出，截图复制在帧内记录、帧栅栏后读取。
7. **证据脚本**：`.incrementalTesting/rendering/rt-shadows.ps1` 跑开启/关闭/mask/key 四遍，逐像素对比、记录时序、断言报告字段，
   23 项检查全部通过；`.incrementalTesting/core/ci.ps1` 已把它纳入 CI。
8. **渲染数学测试**：新增 `renderer/matrix_math.{h,cpp}` 与 `tests/renderer_math.cpp`，
   覆盖单位矩阵、奇异矩阵、view-projection 往返、仿射往返（11 项），防止阴影重建矩阵写错。

本机数字（AMD Radeon 890M，1280x720，33 实例）：开启阴影 GPU P95 0.717 ms，关闭 0.467 ms
（+0.250 ms）；DispatchRays 窗口 0.105 ms；可见性贴图 5,529,600 字节；3.30% 像素因阴影改变。
CPU P95 约 17.2 ms，含 `Present(1,0)` 垂直同步等待，不代表 CPU 成本。

## 1b. 工作包 3–4（0.5.0 / 0.6.0）

- **G1 可靠性**：注入式设备丢失（检测→停止→重建→继续，`NG-E-DEVICE-REMOVED`）、渲染器生命周期 50 次
  （对象计数归零、显存不增长、无锁/临时文件残留）、稳定错误码（`NG-E-PIPE-COMPILE`、`NG-E-MAT-STALE/MISSING`、
  `NG-E-IO-SCENE`、`NG-E-RT-*`、`NG-E-MESH-*` 等）、损坏包拒绝（`BUILD_CONFLICT`）、DXC 负路径、
  捕获工具探测（本机无 PIX/RenderDoc，见 docs/CAPTURE.md）、固定机位性能基线（docs/PERF-BASELINE.md）。
- **Mesh Shader**：AS+MS+PS 三阶段，stream PSO，6 面 meshlet，`F11`/`--mesh-shaders` 切换，顶点路径回退；
  GPU P95 0.388ms（mesh）vs 0.466ms（顶点）。放大剔除因 payload 一致性要求暂时禁用并如实报告。
- **Bindless 槽位**：8 槽有界表、代际、缺材质默认槽（品红棋盘）、复用与释放、越界/过期诊断，
  SM 6.6 直接堆索引评估为「支持但未实现」，报告 `bounded-table`。
- **证据套件**：`.incrementalTesting/rendering/rt-shadows.ps1`（23 项）、`.incrementalTesting/rendering/mesh-bindless.ps1`（22 项）、
  `.incrementalTesting/core/g1-reliability.ps1`（27 项）、`.incrementalTesting/core/g1.ps1`（G1 放行闸门）。

## 2. 阶段关口

| 阶段 | 状态 | 现有证据 | 主要缺口 |
| --- | --- | --- | --- |
| S0 准备 | 已完成（0.3.1） | Git + tag、可重放 CI、固定 DXC/工具链版本表、版本化证据目录 | 远程仓库尚未建立，CI 仅在本地完整重放 |
| S1 原生图形 | 部分完成 | D3D12 双目标光栅、深度可采样、3 槽材质表、场景灯光、DXR 静态阴影（RTPSO/SBT/mask/合成/回退/截图/时序）、DXC 管线 | 尚无 Mesh Shader 样例、完整 Bindless、PIX/RenderDoc 捕获、设备丢失与生命周期证据 |
| S2 编辑器宿主 | Web/Electron 初版；GPU 接入未完成 | 本机场景服务、独立原生窗口控制、按需快照，见 `docs/EDITOR.md` | 尚无内嵌共享纹理、合成与 GPU 会话恢复；不要求先定制 Chromium |
| S3 游戏核心 | 部分完成 | 第三人称走跑跳、扫掠碰撞、交互、重置、场景事务、构建包、基础 Web 场景编辑 UI | 无运行时资产导入、动画、音频、脚本、三维操纵柄 |
| S4 AI/Blender | 未开始 | MCP 工具面可被 Agent 调用 | 无 Blender Worker、无资产规范、无质量门禁、无模型适配器 |
| S5 闭环整合 | 部分完成 | 预览→提交→热重载→冒烟→报告的本地闭环 | 无自然语言任务编排、无代码补丁、无候选预览 UI |
| S6 稳定发布 | 未开始 | 可生成 zip 包 | 无签名、安装器、更新、长时稳定性与安全评审 |

## 3. 本机验证证据

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/ci.ps1 -Clean
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/rt-shadows.ps1
```

检查：

- `reports/<version>/<commit>/<timestamp>/run.json`：环境、DXC、适配器、场景哈希、产物 SHA-256。
- `rt-shadows-evidence.json`：开关两遍的报告字段、时序、截图路径、逐像素对比、23 项检查。
- `artifacts/shadow-on.png`、`shadow-off.png`、`shadow-mask.png`、`shadow-key.png`：固定机位证据。
- `verify.json` / `verify-native-smoke.json` / `verify-probe.json` / `verify-hot-reload.json`：既有各层证据。
- `reports/latest/` 与 `reports/verify.json` 等别名：最新一次运行。

阴影设计、字段表、验收对照与踩坑记录见 [RT-SHADOWS.md](RT-SHADOWS.md)。

## 4. 后续工作包

工作包 1–4 已完成、G1 已放行（证据见 `reports/g1.json`）。逐条任务状态、先决条件与并行泳道见 [DEV-TASKS.md](DEV-TASKS.md)。

- **第 1 批（可并行）**：R08 预算管理器 · E07 确定性契约 · INN-14 几何包格式 + N0 离线 cluster DAG · H07 输出契约文档（仅文档）。
- **第 2 批**：R06 最小 RHI → R07 误差契约 · E08 黑匣子回放 · B7 glTF + L0 SDF 烘焙 + S3E-07 设计期 lint。
- **第 3 批**：B1 compute 管线 + B3 bindless 堆 → N1 GPU 剔除 / L1 屏幕+SDF 追踪。
- **编辑器后续**：先单独验证成熟容器的共享纹理导入、帧同步、输入与恢复，再决定内嵌视口实现。旧 H01–H06 定制 Chromium 路线暂缓，见 [EDITOR.md](EDITOR.md) 与 [CHROMIUM-START.md](CHROMIUM-START.md)。

## 5. 维护约束

- 不允许模型直接写 `assets/`；必须走 `scene_preview`/`scene_apply`，并携带项目 ID 与 expectedRevision。
- 不允许为了通过测试删除测试或放宽 schema；范围变化要更新 `assets/scene.schema.json`、JS 校验器、原生加载器和文档四处。
- 热重载失败必须保留上一有效场景；不得用空场景或静默重置掩盖损坏。
- 高级图形能力只有在“可运行 + 可回退 + 有报告”后才把 `implemented` 改为 true。
- 证据不得手写：`implemented`、`effectImplemented`、性能数字只能来自实际运行写出的报告。
- 场景工具是受信本地工具；在加入网络服务、模型凭证或远程执行前，必须补权限、配额、审计和超时。

## 6. 下个对话目标

继续完善 Web 编辑器与原生引擎的编辑闭环，并单独验证成熟容器内嵌 GPU 视口的可行性，见 [EDITOR.md](EDITOR.md)。[DEV-TASKS.md](DEV-TASKS.md) 与 [NEXT-PHASE.md](NEXT-PHASE.md) 中原先要求先构建定制 Chromium 的顺序，已由本次明确选择的宿主路线取代；其余图形工作包按各自实际证据维护。

