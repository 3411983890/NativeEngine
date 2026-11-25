# NativeGPU 开发任务台账

- 版本：0.11.0（刚体物理、破坏、结构坍塌与证据门禁）
- 台账更新：2026-10-22
- 依据：`outputs\AI原生游戏引擎独立制作人开发流程.docx`（v1.0，**只读参考，未修改**）的 S0–S6 阶段、G0–G6 关口与工作包编号
- 事实来源：Git 提交与 tag、`reports/` 证据、`docs/` 文档、实测探针（RTX 5060 Laptop，2026-10-20）
- 维护规则：**每个工作包结束必须更新本文件**；状态只能由证据支撑，不能由"已投入时间"推断

## 状态图例

2026-10-22 粒子/共享场/布料/水完整开发目标已启动，依据 [完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md)，逐包进度见 [开发记录](SIMULATION-DEVELOPMENT.md)。首批交付 CPU 区域场与固定时钟参考、效果资产校验与编译；新模块未接入宿主的部分单独标注，A–G 总目标仍未完成。GPU 间接派发、非阻塞遥测和独立计算计时已通过原生粒子 27/27 验收（含 5 次启用粒子的生命周期重建）；全量工程门禁 25/25 通过。活跃列表与共享世界场尚未接入。

同日增量：原生效果资产 /2 加载、多实例 CPU 参考及 GPU 非受光精灵接入独立宿主预览，效果专项 **23/23** 通过（含三种 blend 的真实图像、同 seed 重现、停止恢复基线、五次重建无泄漏及重启后继续绘制）。JavaScript **161/161**、原生 **15/15 个测试程序**、本轮全量工程门禁 **27/27** 通过。运行包携带效果库与纹理依赖。它尚未接入场景/编辑器事务与 Game 存档，通用 GPU 模拟、共享场适配、布料与水仍未完成，不能标为完整粒子系统。

2026-10-23 接续已有布料/水 CPU 求解、粒子事件与生产效果库：共享场增加 D3D12 不可变稀疏读取、跨砖块 Q8 插值与读取 halo，AMD 实机 7 组 25,760 次检查、4,150 次 CPU/GPU 对拍，debugErrors=0；新增布料逐顶点风场和水四通道原子权威查询投影，5 组 900 项通过。契约见 [FIELD-REFERENCE.md](FIELD-REFERENCE.md)，真实证据与剩余范围见 [开发记录](SIMULATION-DEVELOPMENT.md)。GPU 写求解、迭代 halo、宿主/存档与统一体积仍缺，A–G 保持未完成。

同日 GPU 粒子增量：新增真正的 SoA 发射器池、双存活列表/空闲列表、GPU 容量预留与 ExecuteIndirect 模拟/发射，包含五形状和力/曲线模块。扩展一小时寿命测试并修正 GPU 区间取整后，AMD 实机 7 组 49,793 次检查、388 次 CPU 粒子状态对拍，debugErrors=0，见 [GPU 池合同](EFFECT-GPU-REFERENCE.md)。尚未接效果实例管理器、跨发射器资产预算或宿主绘制，不能把宿主 CPU backend 改报 GPU。

后续增量：CPU LEF/WF 粗投影、原子多通道提交、跨区域移动/撤销、来源级表示选择和观察配额/迟滞参考已完成 **20 组、37,252 次检查**，见 [场参考契约](FIELD-REFERENCE.md)。仍未接入 GPU/宿主。先前 27 步报告对应旧构建；并行物理更新后已增加构建状态/二进制 hash 与结束来源复核，新版 28 步验收须使用新报告，当前仍在执行。A–G 总目标保持未完成。

| 标记 | 含义 |
| --- | --- |
| ✅ | 已完成，有可重放证据（提交/报告/截图/测试） |
| 🟡 | 部分完成，缺口写在"缺口/备注"列 |
| ⬜ | 未开始 |
| ⏸ | 阻塞或等待确认（不是没做，是不能做） |
| ❌ | 首版明确不做（写清理由） |

## 0. 进度总览

| 阶段 | 关口 | 已完成 | 部分完成 | 未开始/阻塞 | 关口状态 |
| --- | --- | --- | --- | --- | --- |
| S0 准备 | G0 | 2 | 5 | 2 | 🟡 |
| S1 原生图形 | G1 | 14 | 7 | 1 | ✅ 已放行 |
| S1+ 工程能力（docx 未列） | — | 8 | 0 | 0 | ✅ |
| S2 Chromium | G2 | 1 | 0 | 6 | ⏸ 未开始 |
| S3 游戏核心 | G3 | 8 | 6 | 3 | 🟡 |
| S4 AI/Blender | G4 | 0 | 1 | 7 | ⬜ |
| S5 闭环整合 | G5 | 0 | 3 | 3 | ⬜ |
| S6 稳定发布 | G6 | 0 | 3 | 6 | ⬜ |
| X1–X6 扩展 | — | 3 | 3 | 10 | ⬜ |

20 条创新点**不再单列**，已全部落成上表中的工作包（R07/R08、E07–E10、S3E-07、H07、A07/A08、Q07–Q09、X1 的 B/N/L 子项），每条的先决条件、优先级与并行泳道见 §12，泳道划分见 §13。

计数口径：只数工作包行，关口（G0–G6）单列在"关口状态"列。

当前补充（2026-10-20）：**E04 动作系统第一条纵向链路已接入原生小院：骨架/glTF 蒙皮/60 Hz 片段、Trait DAG、惯性化、五状态、分层 Montage、固定 tick Notify 和阶段哈希。E04 保持 🟡，高级动作匹配、脚 IK、肌肉和面部未完成；详见 `docs/ANIMATION.md`，验证入口 `npm run animation`。既有图形与编辑器工作继续保留。**

规划调整（2026-10-22）：**E04 保持 🟡，本次仅修改计划。后续先做传统动作匹配、程序化 Rig / IK 与移动反馈，再补共享 morph、规则肌肉、局部弹簧 / XPBD 软组织及手工面部绑定 / 校正 / 人工时间轴口型；任何必须依赖机器学习或神经网络的功能暂缓。阶段依赖、M1–M5 和质量合同见 `docs/ANIMATION-PLAN.md` v0.3 第 9、10、12、15 节。**

实现补充（2026-10-22）：**M1–M4 的非机器学习首版已接入原生角色：Rig / 双骨 IK / 脚接触、传统动作匹配、GPU morph / 肌肉、局部 XPBD / LOD / 完整存档历史。153 项 JavaScript 测试、原生几何 / 接触 / 状态续跑及 GPU 严格回放专项已通过。E04 仍保持 🟡：M5 面部、通用重定向、全身交互求解和生产内容工具未完成，不能宣称整个 A0–A12 方案已完成。详见 `docs/ANIMATION-M1-M4.md`。**

当前补充（2026-10-21）：**刚体物理与破坏纵向链路已完成：盒体 SAT/顺序冲量/摩擦/恢复/休眠/自适应子步、冲量伤害、142 块碎片、径向爆炸、弹道投掷、结构支撑图与级联坍塌；replay 升级 v3。证据入口 `npm run physics`。边界：无凸 Voronoi、无关节/软体、无爆炸遮挡、角色仍为运动学推动者。**

四阶段补充（2026-10-22 晚）：**F2 XPBD 关节（Point/Hinge/Rope + 限位 + 断裂 + 接触过滤 + 马达/摩擦 + 场景格式 `joints`，`native_joints_tests` 与 `.screen/joint-yard.scene.json`）、F3 键合破坏（`destructible.mode: "bonded"`，cell+bond+能量阈值+并查集，`native_fracture_tests` + 键合墙体用例 + 键合状态存读档往返 + `.screen/bonded-wall.scene.json`）、F4a 离线凸 Voronoi 烘焙、F4b 凸网格实例渲染（一对顶点/索引缓冲 + 字节偏移绘制，物理仍是包围盒代理）、F4c 凸-凸窄相（`engine/native/convex.hpp`：GJK + EPA + 面裁剪，三态返回，碎片按真实凸包碰撞）、F4d 断面 UV / 内部染色 / `bake-scene` 重烘焙已完成。四阶段全部落地。** 宿主新增可复现的 `--shot frame:x,y,z`（走与 F 键相同的射击路径）。详见 `docs/PHYSICS-SCALE-PLAN.md`。

规模化补充（2026-10-22）：**宽相已从 O(n²) 换成确定性动态 AABB 树（`engine/native/broadphase.hpp`），小场景自动回退；新增 `native_broadphase_tests`（超集保证 / proxy 生命周期 / 与 O(n²) 逐位等价 / auto 切换 / 400 体过滤）。同进程 A/B 基准：167 体 1.71→1.24 ms/步，1,001 体 46.6→13.1 ms/步，候选对 500,500→1,039，接触数不变；`npm run physics` 11/11 通过，断裂/碎片/坍塌/峰值冲量不变。XPBD 关节、能量/键合破坏、离线凸 Voronoi 仍为方案，见 `docs/PHYSICS-SCALE-PLAN.md`。**

## 1. 里程碑与提交对照

| tag/提交 | 内容 | 证据 |
| --- | --- | --- |
| `v0.3.0` `31e3463` | 0.3.0 基线（光栅 + DXR 加速结构探针） | `docs/STATUS.md` 历史章节 |
| `v0.3.1` `ee915ff` | 工作包 1：Git/CI/DXC 冻结/版本化报告 | `reports/0.3.1/ee915ffa4481/…` |
| `v0.4.0` `77db7d6` | 工作包 2：DXR 静态阴影（RTPSO/SBT/合成/回退） | `reports/latest/rt-shadows-evidence.json`（23 项） |
| `v0.6.0` `67a6b33` | 工作包 3+4：G1 可靠性、Mesh Shader、Bindless 槽位 | `reports/latest/g1-reliability.json`（27 项）、`mesh-bindless-evidence.json`（22 项） |
| `g1` `f34499e` | G1 放行 + Chromium 启动清单 | `reports/g1.json`（6/6 通过）、`docs/CHROMIUM-START.md` |
| `v0.7.0` `ba44592` | 第一批：E07 确定性、R08 预算管理器、INN-14+N0 离线几何、H07 输出契约 | `reports/0.7.0/ba44592111d9/…`（verify 15/15；determinism 19/19；budget 18/18；offline 6/6） |
| `v0.8.0` | 第二批：R06 RHI 边界、R07 误差契约、E08 回放与黑匣子、B7 glTF、L0 SDF、S3E-07 设计期 lint | `reports/0.8.0/…`（verify 全绿；rhi 13/13；replay 25/25；error-contract 5/5；gltf 6/6；sdf 8/8；lint 11/11） |
| `8bf0f01` / `16a0c27` | Nanite/Lumen 方案与创新清单取舍（设计文档） | `docs/NANITE-LUMEN-PLAN.md`、`docs/IDEA-TRIAGE.md` |
| 当前工作区（未提交，0.10.0） | E04 动作系统首条纵向链路（方案 A0/A1 核心 + A2 部分） | `.incrementalTesting/core/animation.ps1`（17 项导入测试、5,350 条原生断言、严格回放）、`reports/latest/animation-evidence.json`；完整 verify 21/21，版本化报告 `reports/0.10.0/31c00bc53c0b/20261020T153544Z/verify.json`；实现边界见 `docs/ANIMATION.md` |

---

## 2. S0 准备与范围冻结（docx §03，预计 2–3 周）

| ID | 任务 | 状态 | 证据/产物 | 缺口/备注 |
| --- | --- | --- | --- | --- |
| S0-01 | 验收样板、支持范围、不做事项、成功条件写成一页说明 | 🟡 | `README.md`、`docs/STATUS.md`、`docs/NEXT-PHASE.md` | 内容齐全但分散在三份文档，缺一份冻结的一页版 |
| S0-02 | 独立原生项目完成首次构建 | ✅ | `tools/build.ps1`、`.incrementalTesting/core/ci.ps1` | — |
| S0-03 | 选定 Chromium 分支并完成首次构建 | ⏸ | `docs/CHROMIUM-START.md`（冻结清单） | 未冻结 commit，按约定等你确认后才开始 |
| S0-04 | 仓库、备份、密钥隔离可用 | 🟡 | Git `main` + 5 个 tag、`.gitignore` | 无远程仓库、无备份策略文档、无密钥清单 |
| S0-05 | 目录与构建边界（host/engine/renderer/editor/ai/adapters/assets/tests/tools/docs） | 🟡 | 已有 8 个目录 | 缺 `editor/`、`adapters/`；无 RHI 边界（见 R06） |
| S0-06 | 从干净目录重建最小项目 | ✅ | `.incrementalTesting/core/ci.ps1` 在删除 `build/`、`outputs/`、`reports/` 后成功 | 远程 CI 未跑（无 remote） |
| S0-07 | 版本冻结（Chromium/SDK/DXC/Blender/Python/关键库） | 🟡 | `docs/TOOLCHAIN.md`、`tools/bootstrap-dxc.ps1` | DXC 1.9.2609、llvm-mingw 20261006、Node 24 已冻结并带 SHA-256；Chromium/Blender/Python 未冻结 |
| S0-08 | 时间与现金上限、每周工时记录 | ⬜ | — | 无工时/预算台账 |
| S0-09 | "每加一个需求必须移走另一个"的范围纪律 | 🟡 | `docs/STATUS.md` §5 维护约束 | 未形成可执行的变更流程 |
| **G0** | 关口 | 🟡 | 除 Chromium 构建外基本满足 | Chromium 是 S2 前置，不影响 S1 放行 |

## 3. S1 原生图形验证（docx §04–§05，G1 已放行）

### 3.1 工作包

| ID | 任务 | 状态 | 证据/产物 | 缺口/备注 |
| --- | --- | --- | --- | --- |
| R01 | 创建设备、查询特性、输出适配器信息 | ✅ | `capabilities` JSON：RTX 5060 Laptop、DXR 1.2、Mesh Tier 10、SM 6.8、Binding Tier 3、8.28 GB | 单 GPU 验证；未做第二套硬件组合 |
| R02 | Buffer/Texture 上传、分配与延迟回收 | 🟡 | 上传路径、对象计数、50 次重建后存活对象 0、显存波动 0 | **无延迟回收**：shutdown 用 `waitIdle` 一次性回收，未做逐资源 fence 追踪 |
| R03 | 命令列表、队列、Fence、状态转换 | ✅ | 3 帧在途、tracked resource state、fence 超时诊断 | 只有一条 DIRECT 队列；无 compute 队列 |
| R04 | 相机、PBR 网格、深度、基础光照 | ✅ | 固定机位截图、`gpuP95Ms`/`cpuSubmitP50Ms`、PNG 截图 | 光照是 Lambert+Blinn 近似，不是完整 PBR（无 IBL/能量守恒） |
| R05 | 高级探针：Bindless / DXR / Mesh Shader | 🟡 | DXR 阴影 ✅（RTPSO+SBT+回退）；Mesh ✅（AS+MS+PS+回退）；Bindless 有界表 8 槽 ✅ | SM 6.6 直接堆索引**未实现**（评估为支持）；放大剔除禁用；Mesh 路径亮度与顶点路径不一致 |
| R06 | 最小 RHI 与后端扩展点 | ✅ | `renderer/rhi.{hpp,cpp}`、`rhi_d3d12.cpp`、`tests/rhi_conformance.cpp`（13 项）、报告 `rhi` 段；smoke 经接口跑 240 帧、摘要与迁移前逐位一致 | 只有 d3d12 一个后端；接口只覆盖帧路径与生命周期，不是完整 RHI。见 `docs/RHI.md` |
| R07 | 渲染误差契约（创新 INN-03；PT 参考部分仍待 B1） | ✅ | `tests/image-contracts.json`（5 条契约）、`.incrementalTesting/rendering/error-contract.ps1`（5/5 通过）、`docs/ERROR-CONTRACTS.md` | 阈值全部实测：阴影开关 changedPercent 11.73、网格回退 59.25、阴影/网格回退逐像素相同（meanAbsError=0）；**引擎内 path-traced 参考未做，需 compute 管线 B1** |
| R08 | 统一预算管理器 + 每插件性能契约（创新 INN-04） | 🟡 | `.incrementalTesting/rendering/budget.ps1`（18 项）、`tests/budget_tests.cpp`（28 项）、`reports/latest/budget-evidence.json` | 中位数决策 + 两档阶梯（阴影/网格路径）已落地；**无渲染缩放档、不可回退、每插件契约未接**。见 `docs/BUDGET.md` |
| S1-01 | 资源记录类型/尺寸/用途/状态/最后 Fence + 延迟回收 + GPU 调试工具与命名 | 🟡 | 状态跟踪、错误码、`NG-E-*` 诊断 | 无 `SetName` 命名、无 PIX/RenderDoc、无延迟回收 |
| S1-02 | Bindless 限定为材质纹理表（上限/默认槽/代际/缺失/复用/在途更新） | 🟡 | 8 槽、代际、默认槽（品红棋盘）、复用与失效诊断 | "在途帧更新"未做真实并发测试（只有同步自检） |
| S1-03 | 静态 DXR 效果（BLAS/TLAS + 阴影 + 成本 + 回退） | ✅ | GPU P95 +0.10 ms；DispatchRays 0.049 ms（独显）；关闭走纯光栅 | 蒙皮网格加速结构仍未实现；新骨骼角色使用 VS 路径，只接收静态物体阴影，不伪装成方盒投影 |
| S1-04 | 可切换 Mesh Shader 示例（切分/剔除/绘制/回退/帧时对比） | 🟡 | 6 个面 meshlet、240 帧、网格路径 GPU P95 0.388 vs 顶点 0.464 ms | 切分是程序化生成而非离线资产管线；**放大剔除禁用**（payload 一致性） |
| S1-05 | 保留跨后端接口但延后第二套实现 | ⬜ | — | 同 R06 |
| S1-06 | 固定可测图形样板（低复杂度/复杂材质/静态光追三场景） | 🟡 | 小院场景 + 固定机位 + 场景哈希 + 驱动/分辨率/DXC 记录 | 只有一套场景；缺"复杂材质"与独立"静态光追"场景 |
| **G1** | 关口 | ✅ | `reports/g1.json`：6/6 通过（本机 12 步 verify、23+22+27 项证据） | 图形捕获（PIX/RenderDoc）因工具缺失不计入通过项，已在 `docs/CAPTURE.md` 明确 |

### 3.2 docx 未列、我们已完成的工程能力（S1+）

| ID | 能力 | 状态 | 证据 |
| --- | --- | --- | --- |
| S1P-01 | 版本冻结与工具链可重建（DXC/llvm-mingw/Node 版本表 + SHA-256） | ✅ | `docs/TOOLCHAIN.md`、`tools/bootstrap-dxc.ps1` |
| S1P-02 | 一条命令从干净状态重放 CI | ✅ | `.incrementalTesting/core/ci.ps1`、`.github/workflows/windows.yml` |
| S1P-03 | 版本化证据目录（append-only + latest 别名 + run.json） | ✅ | `tools/report-lib.ps1`、`reports/<version>/<commit>/<stamp>/` |
| S1P-04 | DXR 阴影证据套件（开/关/mask/key + 逐像素对比 + 时序 + 23 项断言） | ✅ | `.incrementalTesting/rendering/rt-shadows.ps1` |
| S1P-05 | Mesh/Bindless 证据套件（22 项断言） | ✅ | `.incrementalTesting/rendering/mesh-bindless.ps1` |
| S1P-06 | G1 可靠性套件（设备丢失/生命周期/错误码/损坏包/27 项断言） | ✅ | `.incrementalTesting/core/g1-reliability.ps1` |
| S1P-07 | 性能基线与捕获可用性探测 | ✅ | `docs/PERF-BASELINE.md`、`reports/capture-status.json`、`docs/CAPTURE.md` |
| S1P-08 | 可运行分包（zip + manifest + 内容寻址场景包 + 篡改检测） | ✅ | `tools/package.ps1`、`NativeGPU-0.6.0-win64.zip` |

## 4. S2 Chromium 与 NativeGPU 接入（docx §06–§07，G2）

**状态：整体 ⏸，等用户确认后才开始**（`docs/CHROMIUM-START.md` 是启动清单，不是已开工）。

| ID | 任务 | 状态 | 前置/证据 | 备注 |
| --- | --- | --- | --- | --- |
| H01 | 宿主：冻结 Chromium 分支、构建本地编辑器页 | ⏸ | 需先冻结 commit（见 `docs/TOOLCHAIN.md` §5） | 冻结动作尚未执行 |
| H02 | 会话：Mojo 接口 + Browser Host 权限代理 | ⬜ | 清单已写接口草案 | 只有受信页面可建会话 |
| H03 | 服务：原生核心迁入隔离进程（启动/停止/崩溃/重连） | ⬜ | — | 需进程边界与句柄失效设计 |
| H04 | 纹理：共享纹理句柄与同步导入 SharedImage | ⬜ | 现有 composite + fence 可作输出契约 | 禁止整帧 CPU 回读 |
| H05 | 交互：窗口缩放、输入、坐标、视口状态 | ⬜ | — | 按缩放/最小化/DPI/色彩顺序验证 |
| H06 | 诊断：复制次数、合成与等待耗时 | ⬜ | 现有时间戳基础设施可复用 | 需与原生基线对比 |
| H07 | 输出契约：composite + fence 复用为渲染输出接口（创新 INN-20） | ✅ | `docs/OUTPUT-CONTRACT.md`（182 行，逐条对照真实渲染器符号） | 文档已冻结；**所有"当前未实现"项均标注，验证仍待 S2**；泳道 D；写契约不改变 S2 阻塞状态 |
| **G2** | 关口 | ⬜ | 受信页面稳定视口 + 普通页面被拒 + 强制停止可重建 + 无 CPU 回读 | 未开始，不能宣称任何一项 |

## 5. S3 最小游戏核心（docx §08–§09，G3）

### 5.1 工作包 E01–E10

| ID | 任务 | 状态 | 证据/产物 | 缺口/备注 |
| --- | --- | --- | --- | --- |
| E01 | 场景：实体 UUID、层级、组件、序列化、资产引用 | 🟡 | UUID + JSON 序列化 + 8 种实体类型 + 材质/灯光引用；保存重开一致（Node 测试） | **无层级、无组件系统**；资产引用只有材质槽 |
| E02 | 玩法：时间、输入、第三人称相机、角色控制 | ✅ | 120 Hz 固定步长、WASD/Shift/Space、扫掠碰撞、相机 | 角色是方盒，不是模板人形 |
| E03 | 物理与音频：碰撞、触发、音频组件 | 🟡 | 扫掠碰撞、钥匙/门/终点触发；音频 A0–A3：`engine/audio/*`（48 kHz 混音图、6 总线、虚拟 voice、VBAP/HRTF/遮挡）、`.incrementalTesting/core/audio.ps1`（52 项）、`reports/latest/audio-evidence.json`；`PlaySoundW` 已删除 | A4–A9 未做：Atmos 元数据流、ADM BWF 导出、SOFA HRTF、`.ngpatch` 音频图、编辑器音频面板、`audio_patch` MCP。见 `docs/AUDIO-PLAN.md` |
| E04 | 动画：骨架 / 混合 / M1–M4 非机器学习动作 | 🟡 | `engine/native/animation*`、`tools/offline/animation*.mjs`、`.incrementalTesting/core/animation.ps1`；16 骨骼 / 11 Clip、传统动作匹配 / Rig / 脚接触、GPU morph / 肌肉与局部 XPBD；26 项专项通过，汇总 `reports/animation-m1-m4-completion.json` | 原创工程内容、单蒙皮角色；M5 面部、通用重定向、距离匹配 / 全身运动扭曲与完整作者工具未完成；不宣称任意内容无脚滑或整个 A0–A12 完成 |
| E05 | 脚本：受限 TS/WASM、编译与错误定位 | ⬜ | — | 无脚本 VM；MCP 是外部 Node 工具，不是运行时脚本 |
| E06 | 运行包：资产依赖闭包、默认配置、离线启动 | ✅ | `tools/package.ps1`：zip + manifest（每文件 SHA-256）+ 内容寻址场景包 | 无安装器/签名 |
| E07 | 确定性契约：tick 哈希 + RNG 流注册表（创新 INN-01） | ✅ | `.incrementalTesting/core/determinism.ps1`（19 项）、`tests/native_simulation.cpp` 新增 3 组检查、`reports/latest/determinism-evidence.json` | 两次运行逐字节相同、分辨率/调试视图无关、静态门禁通过；**输入日志留给 E08**。见 `docs/DETERMINISM.md` |
| E08 | 常开黑匣子 + replay-as-evidence（创新 INN-02） | ✅ | `.incrementalTesting/core/replay.ps1`（25 项）、`docs/REPLAY.md`、报告 `replay`/`blackbox` 段 | 240 帧回放 0 不符；篡改第 100 帧摘要 → 严格回放 `NG-E-REPLAY-MISMATCH` 且精确定位该帧；注入 60 ms 停顿 → anomaly sidecar 可回放。**非环形缓冲**，长会话线性增长 |
| E09 | 世界分叉 CI：`--fork N` 并行确定性实例（创新 INN-06） | ⬜ | 内容寻址场景包已有（E06） | 先决：E07；泳道 B；P2 |
| E10 | 场抽象单消费者：导航/可达性（创新 INN-10） | ⬜ | — | 先决：S3E-07 + A07；泳道 B；P2 |

### 5.2 docx §09 编辑器与事务接口（工作包未编号，这里补 S3E）

| ID | 任务 | 状态 | 证据/产物 | 缺口/备注 |
| --- | --- | --- | --- | --- |
| S3E-01 | 最小编辑界面：场景树、属性面板、资产列表、运行/停止、日志 | 🟡 | Win32 宿主：实体列表、任务/状态/统计面板、暂停/重置/重载/打开/重建按钮 | 无属性编辑面板、无资产列表、无日志窗；不是 Chromium 编辑器 |
| S3E-02 | 编辑命令：创建/设置/附加/引用/删除 + 项目/UUID/revision + 原子提交回滚 | ✅ | `engine/scene/index.mjs` 事务、`tools/scene.mjs`、13 项 Node 测试（冲突/原子性） | 无"附加组件"命令（无组件系统） |
| S3E-03 | 保存与撤销：线性撤销、临时文件、异常快照 | ✅ | `.history.json`（100 步）、`.snapshot.json`、`.pending.json`、`.lock`、`recover` | 无并发合并（首版不做） |
| S3E-04 | 编辑态与运行态分离 | 🟡 | F5 重置、热重载重新初始化玩法 | 运行中修改不回写项目；热重载会重置玩法状态 |
| S3E-05 | 确定性资产构建（内容哈希缓存键、manifest、依赖、清缓存重建） | ✅ | `scene build` 内容寻址 + `BUILD_CONFLICT` 篡改检测 + 打包 manifest | 只覆盖场景 JSON，未覆盖纹理/动画资产 |
| S3E-06 | 给 AI 留可调用入口（输入 schema/输出/超时/错误码/副作用） | 🟡 | MCP stdio 8 个工具、结构化错误、路径策略 | **无超时、无取消、无费用记录、无副作用说明字段** |
| S3E-07 | 设计期 lint + 可达性证明（创新 INN-07） | ✅ | `engine/scene/lint.mjs`（12 条规则）、`tests/lint.test.mjs`（11 项）、`tools/scene.mjs lint`、`docs/SCENE-LINT.md` | 小院 0 findings；可达性用 40×40 网格 BFS；**无 3D/跳跃/高度推理**，门按可通行处理 |
| **G3** | 关口 | 🟡 | 走跑跳+碰撞+交互 ✅、离线运行包 ✅、场景诊断 ✅ | 手工导入角色与 5 件道具 ❌；15 分钟连续游玩未做正式验收；缺脚本/动画错误诊断 |

## 6. S4 AI 编排与 Blender 管线（docx §10–§11，G4）

| ID | 任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| A01 | 工具契约：结构化输入/输出/取消/超时/错误码 | 🟡 | 8 个 MCP 工具已有 schema 与错误码；**缺取消/超时/费用** |
| A02 | Blender Worker：后台进程、项目副本、固定配方与导出 | ⬜ | 未开始 |
| A03 | 资产规范：UUID 来源、哈希、USD/glTF 支持子集 | ⬜ | 未开始（场景 JSON 有 UUID，但无资产规范） |
| A04 | 编排：任务状态、检查点、幂等、成本上限 | ⬜ | 未开始 |
| A05 | 模型适配：先一个模型，再同类替换与本地探针 | ⬜ | 未开始 |
| A06 | 质量门禁：几何/权重/动画/编译/运行检查 | ⬜ | 未开始 |
| A07 | 世界查询语言（创新 INN-08 前半；动作 DSL 仍未做） | ✅ | `engine/scene/query.mjs`、`tests/query.test.mjs`（18 项）、`tools/scene.mjs query`、`docs/SCENE-QUERY.md` | 9 种操作 + select entity/material/light + 聚合；小院：墙 5、材质 brick 8/metal 6/checker 3、灯光强度和 9.7。**线性扫描无空间索引；类型化动作 DSL 未做** |
| A08 | 语义 diff 影响分析（创新 INN-09） | ⬜ | 先决：A07；泳道 C；P2 |
| **G4** | 关口 | ⬜ | 不满足 |

## 7. S5 需求到可玩闭环（docx §12–§13，G5）

| ID | 任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| I01 | 需求单：解析目标/资产/玩法/预算/约束 | ⬜ | 未开始 |
| I02 | 任务链：道具→角色→动画→验证→导入 | ⬜ | 未开始 |
| I03 | 场景编辑：AI 生成 ScenePatch 预览与提交 | 🟡 | `scene_preview`/`scene_apply` + revision 冲突拒绝已可用；无 UI 预览 |
| I04 | 代码编辑：限定目录补丁 + 编译测试 + 失败不覆盖 | ⬜ | 未开始（当前提交由人/Agent 直接改仓库，无补丁门禁） |
| I05 | 可玩验证：相机/角色/交互/完成条件 + 固定回放 | 🟡 | `--smoke` 冒烟 + 热重载集成测试；无固定回放/人工游玩记录 |
| I06 | 打包回归：运行包 + 缺网/故障恢复 + 干净环境重放 | 🟡 | 打包 + 冒烟；未做缺网/故障恢复与干净环境重放验证 |
| **G5** | 关口 | ⬜ | 不满足 |

## 8. S6 稳定性与首版发布（docx §14–§15，G6）

| ID | 任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| Q01 | 回归：图形/资产/脚本/编排/权限全流程 | 🟡 | `verify.ps1` 12 步 + 三套证据；无资产/脚本/编排/权限链路 |
| Q02 | 长时：视口循环、反复任务、加载释放（8 小时） | ⬜ | 只做了 50 次重建与 240 帧冒烟；**无 8 小时测试** |
| Q03 | 恢复：服务/DCC/模型断网/任务取消与重启 | 🟡 | 设备丢失注入 + 渲染器重建 ✅；无服务/DCC/模型链路 |
| Q04 | 安全：权限、最小目录、密钥、插件与输入检查 | ⬜ | 无安全评审；MCP 路径策略是唯一约束 |
| Q05 | 安装更新：签名/校验、发布包、升级回滚 | ⬜ | 只有 zip，无签名/安装器/更新/回滚 |
| Q06 | 文档：教程、支持矩阵、限制、错误与诊断 | 🟡 | `README`/`STATUS`/`CI`/`TOOLCHAIN`/`CAPTURE`/`PERF-BASELINE` 齐全；无用户教程 |
| Q07 | 契约注册表：确定性/误差/性能统一登记与执行（创新 INN-05） | ⬜ | 先决：E07 + R07 + R08 三条契约各落地；泳道 C；P2 |
| Q08 | 图像回归门禁：块 SSIM + 历史基线（创新 INN-11） | ✅ | `.incrementalTesting/rendering/image-regression.ps1`、`tests/image-baselines.json`、`docs/IMAGE-REGRESSION.md` | 4 项指标（meanAbsError/changedPercent/maxChannelDelta/块 SSIM）+ 差异图；`-Accept` 才更新基线；本机基线 ssim 1.0。**基线在 gitignored 的 reports/ 下，干净检出首次运行会自动引导基线并如实报告** |
| Q09 | 能力-契约-证据发布门（G1 模式产品化，创新 INN-13） | ⬜ | 先决：Q07 + Q08；泳道 C；P3 |
| **G6** | 关口 | ⬜ | 不满足 |

## 9. 扩展路线 X1–X6（docx §16，不属于首版）

| ID | 扩展 | 状态 | 启动条件 | 备注 |
| --- | --- | --- | --- | --- |
| X1 | 图形收敛：Nanite / Lumen / 流送 / 更多材质 | ⬜ | G1 稳定 + 回归集可复用（已满足） | 方案见 `docs/NANITE-LUMEN-PLAN.md`；子项 **B1–B8 / N0–N4 / L0–L4 / INN-12、14–19** 已拆进 §12 并标好先决条件 |
| X2 | Vulkan 后端 | ⬜ | D3D12 RHI 边界已证明 | 需先做 R06 |
| X3 | Maya 适配 | ⬜ | 合法安装 + 真实需求 | — |
| X4 | Houdini 适配 | ⬜ | 版本与授权清楚 | — |
| X5 | 更多模型与非模板角色 | ⬜ | 先增加评测样例 | — |
| X6 | 发布平台（Linux/受限网页预览） | ⬜ | 现有平台维护量可承受 | 依赖 S2 |
| N0 | 离线 cluster DAG 构建（Nanite 第 0 步） | 🟡 | 需 glTF 输入 + 几何包格式 | 产物已带 v2 字段：37 cluster / 6 LOD，lodError 按 LOD 单调（0 → 5.378）；**仍无运行时 LOD 选择器** |
| INN-14 | 内容寻址几何包格式（v2） | ✅ | — | `tools/offline/geometry-package.mjs` v2：cluster 级 `lodError`/`flags`/`materialHandle` + `capabilities`；v1 仍可读并填默认值；本机包摘要 `f3ddb1e4…`；6 项 v2 测试 |
| B7 | glTF 2.0 子集加载（离线） | ✅ | — | `tools/offline/gltf.mjs` + 可复现 fixtures + 6 项测试；支持嵌入/sibling buffer、索引三角形、POSITION/NORMAL；拒绝 skin/animation/扩展/稀疏/Draco/.glb 与非单位变换、多场景、实例化 |
| L0 | SDF 烘焙（远场） | ✅ | — | `tools/offline/sdf-bake.mjs` + 8 项测试 + `docs/SDF-BAKE.md`；32³ 球面中心最大误差 0.0587 体素、盒角三线性 0.4984（插值固有）；**当前场景网格不封闭，符号用最近三角形法向，不是可靠内部场** |
| B1 | compute 管线与 compute 队列 | 🟡 | — | `renderer/compute_pass.cpp` + `shaders/compute_probe.hlsl`（cs_6_6）；240 帧 240 次派发、240 次跨队列等待；报告 `probes.compute`；`.incrementalTesting/rendering/compute.ps1` 15 项。**结果尚未进入合成画面（B1b）**，无队列并行量化证据 |
| B3 | bindless 堆真实现（SM 6.6） | 🟡 | — | `ResourceDescriptorHeap` 索引 32–35 与描述符表采样**逐位一致**（4/4，maxDelta 0）；缺 `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` 时 PSO 报 E_INVALIDARG 已记录。**材质主路径仍在 8 槽有界表**，迁移需先用 DXC 编译光栅着色器（B3b） |
| B3b | 材质主路径 heap 索引（DXC 光栅变体） | ⬜ | — | 先决：B5；见 `docs/GEOMETRY-LAYOUT.md` §10 |
| B2 | ExecuteIndirect（含 DispatchRays） | ⬜ | — | 先决：B1 ✅；N1 的第一道硬门 |
| B4 | HZB 生成（上帧深度降采样） | ⬜ | — | 先决：B2；N2 两阶段遮挡的输入 |
| B5 | visibility buffer + resolve | ⬜ | — | 先决：B3 + B4；cluster 与顶点两条光栅的共同输出 |

## 10. 常驻工程任务（每周/每版本）

| ID | 任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| OPS-01 | 每工作日结束跑构建 + 冒烟 | 🟡 | 脚本已具备（`build.ps1`/`verify.ps1`），未形成每日节奏 |
| OPS-02 | 每周完整跑固定样板 + 预算报告 | 🟡 | `verify.ps1` 可一键，未按周留档 |
| OPS-03 | 证据归档与 tag 纪律 | ✅ | 每个工作包一个提交 + tag；`reports/` 版本化 |
| OPS-04 | 依赖与工具链更新审计 | ⬜ | 无定期审计（DXC/驱动/Node） |
| OPS-05 | 备份与恢复演练 | ⬜ | 无远程备份、无恢复演练 |
| OPS-06 | 文档同步（STATUS/README/TOOLCHAIN/台账） | 🟡 | 每次工作包已更新；本台账是新增项 |
| OPS-07 | 工时与费用记录 | ⬜ | 无记录 |
| OPS-08 | 风险复盘与范围冻结检查 | 🟡 | `docs/STATUS.md` §5 有约束，无定期复盘 |

## 11. KPI 对照（docx §19）

| 测试项 | docx 首版目标 | 现状 | 状态 |
| --- | --- | --- | --- |
| 图形能力 | Bindless 主线、DXR 单效果、Mesh 可回退 | DXR/ Mesh ✅；Bindless 为有界表（SM 6.6 未实现） | 🟡 |
| 视口 | 1080p P95 ≤ 33.3 ms | 只测 1280×720（GPU P95 0.117 ms、DispatchRays 0.049 ms）；**无 1080p 口径** | ⬜ |
| 合成 | 无整帧 CPU 回读、服务可恢复 | 依赖 S2，未开始 | ⬜ |
| 场景与运行包 | 保存重开、引用一致、离线运行 | ✅（Node 测试 + zip 运行包） | ✅ |
| 资产合法性 | 无 NaN/缺引用/超预算，权重归一 ≤ 0.0001 | 场景校验覆盖 NaN/引用/范围；动画导入与运行时校验骨架/片段/关节索引/有限值/归一权重，拒绝损坏文件；未覆盖 Blender/生产内容全链路 | 🟡 |
| 动画 | 模板动作零严重变形、脚滑 P95 ≤ 3 cm | 无动画系统 | ⬜ |
| AI 任务 | 20 个冻结任务 ×3 次、≥80% 成功率 | 无编排与任务集 | ⬜ |
| 恢复 | 故障不损坏项目、服务恢复 P95 ≤ 10 s | 设备丢失注入 + 重建 ✅；无 30 次强制中断统计 | 🟡 |
| 可靠性 | 8 小时无崩溃、内存/显存增长 < 5% | 50 次重建对象归零、显存波动 0；**无 8 小时** | 🟡 |
| 安全与费用 | 无严重漏洞、密钥不入包、可计量 | 无安全评审、无费用计量 | ⬜ |

## 12. 创新项 → 工作包映射（优先级 / 先决条件 / 泳道）

20 条创新点与 Nanite/Lumen 子项已经**全部落成带编号的工作包**，状态在 §3、§5、§6、§8、§9 的表里维护；本表只回答"先做哪个、被什么挡住、能和谁同时做"。

- **P0**：当前迭代就做，无先决条件
- **P1**：紧随 P0，或与 P0 并行（互不抢文件）
- **P2**：依赖 P1 的具体产出
- **P3**：后期/依赖多项完成

### 12.1 确定性、契约与工具链（非图形子树）

| 工作包 | 创新/内容 | 优先级 | 先决条件（必须先完成） | 泳道 | 可并行于 |
| --- | --- | --- | --- | --- | --- |
| E07 | INN-01 确定性契约 + tick 哈希 + RNG 流注册表 | **P0** | — | B | R08、S3E-07、N0、L0 |
| R08 | INN-04 统一预算管理器 + 每插件性能契约 | **P0** | — | A | E07、S3E-07、N0 |
| R07 | INN-03 误差契约（PT 参考部分为 P2） | P1 | —（PT 参考需 B1） | A | E07、R08、Q08 |
| E08 | INN-02 常开黑匣子 + replay-as-evidence | P1 | E07 | B | R07、A07 |
| S3E-07 | INN-07 设计期 lint + 可达性证明 | P1 | —（schema 已有） | C | E07、R08、N0、L0 |
| H07 | INN-20 composite + fence 输出契约（**只写文档**） | P1 | — | D | 全部 |
| INN-14 | 内容寻址几何包格式 | P1 | — | C | N0、L0 |
| B7 | glTF 加载（含 meshopt 子集） | P1 | — | C | N0、L0、A03 |
| N0 | 离线 cluster DAG 构建 | P1 | B7 + INN-14 | C | L0、B1、B3 |
| L0 | SDF 烘焙（远场） | P1 | B7 | C | N0、B1、B3 |
| A07 | INN-08 世界查询语言 + 动作 DSL | P2 | S3E-07 | C | N1、L1 |
| A08 | INN-09 语义 diff 影响分析 | P2 | A07 | C | N2、L2 |
| E09 | INN-06 世界分叉 CI（`--fork N`） | P2 | E07 | B | Q08 |
| E10 | INN-10 场抽象单消费者（导航/可达性） | P2 | S3E-07 + A07 | B | N3、L1 |
| Q07 | INN-05 契约注册表（确定性/误差/性能） | P2 | E07 + R07 + R08 | C | INN-13 |
| Q08 | INN-11 图像回归门禁（SSIM/ΔE） | P2 | R07 + Q01 | C | E09 |
| Q09 | INN-13 能力-契约-证据发布门 | P3 | Q07 + Q08 | C | — |
| INN-12 | SWRT/HWRT 差分测试 | P3 | L1 + R07 | A | INN-15 |

### 12.2 图形子树：从共享底座向下推（按拓扑顺序）

顺序按拓扑排（先决条件列里的 ID 必须已完成）；**没有先决关系的行可以并行**，例如 B1 与 B3、N1 与 L1、N2 与 N3。

| 工作包 | 内容 | 优先级 | 先决条件 | 泳道 | 可并行于 |
| --- | --- | --- | --- | --- | --- |
| B1 | compute 管线 + compute 队列 | P1 | R03（DIRECT 队列已有） | A | B3、N0、L0 |
| B3 | bindless 堆真实现（SM 6.6，保留有界表回退） | P1 | R05 能力评估（已有） | A | B1、N0 |
| B2 | ExecuteIndirect（含 DispatchRays） | P2 | B1 | A | N3、INN-17 |
| B4 | HZB 生成（上帧深度降采样） | P2 | B1 | A | N2、L2 |
| B5 | visibility buffer | P2 | B3 + R04 | A | B6、N4 |
| B6 | TAA + 历史裁剪 | P2 | B5 | A | L3 |
| B8 | 能力降级矩阵（full/lite/fallback 三档） | P2 | R01 + R08 | A | L3、L4 |
| N1 | GPU 剔除 + cluster LOD 选择 | P2 | N0 + B1 + B3 | A | L1 |
| L1 | 屏幕空间 + SDF 光线追踪 | P2 | L0 + B1 + B3 | A | N1 |
| N2 | 两阶段遮挡（HZB + 二次剔除） | P2 | N1 + B4 | A | L2 |
| N3 | SW + HW 混合光栅 | P2 | N1 + B2 | A | L1 |
| L2 | 表面缓存 + 辐射探针 | P2 | L1 | A | N2 |
| N4 | visibility buffer resolve + cluster 流送 | P2 | N3 + B5 | A | L1 |
| L3 | 时空降噪 | P2 | L2 + B6 | A | N4 |
| L4 | 反射 + 远场 GI | P2 | L3 | A | — |
| INN-15 | cluster-native SWRT | P2 | N4 + L1 | A | INN-16、INN-17 |
| INN-16 | visibility buffer 双消费者 | P2 | N4 + L1 | A | INN-15、INN-17 |
| INN-17 | 合并光线预算（阴影+GI+反射单派发） | P2 | B2 + L1 + N4 | A | INN-18 |
| INN-18 | GPU 工作图（ExecuteIndirect 统一调度） | P2 | B2 + N3 | A | INN-17 |
| INN-19 | cluster 辐射缓存（低端档） | P2 | N1 + L2 | A | INN-16 |

## 13. 并行开发泳道与关键路径

### 13.1 五条泳道

| 泳道 | 独占目录/文件 | 现在可开工 | 阻塞在 | 提交节奏 |
| --- | --- | --- | --- | --- |
| **A 渲染器内核** | `renderer/`、`shaders/`、`.incrementalTesting/rendering/rt-shadows.ps1` 等渲染证据脚本 | R06、R07、R08；B1/B3 | N/L 要等 C 泳道的离线产物 | 一个 effect 一个提交 + 一份证据 |
| **B 引擎内核** | `host/`、`engine/`（玩法/确定性） | E07；E08/E09 等它 | E07 先行 | 每天最多一次 `host/main.cpp` 改动 |
| **C 离线工具** | `tools/offline/`（新建）、`ai/`、`engine/scene/` | S3E-07、INN-14、B7、N0、L0 | 无 GPU 依赖，**全程可并行** | 每个工具自带 `-SelfTest` |
| **D 宿主** | `host/chromium/`（新建） | H07（今天就能写契约文档） | H01–H06 需你确认 S2 | 只输出契约，不动渲染器 |
| **E 资产/AI** | `ai/worker/`（新建） | A01 补超时/取消 | A02–A06 需 Blender + A01 | 每个任务一份成本与耗时记录 |

### 13.2 依赖图（→ 表示"必须先完成左边"）

```
E07 ──> E08 ──> (S5 回放证据)
 └───> E09

R08 ─┐
R07 ─┼──> Q07 ──> Q09
E07 ─┘
R07 ──> Q08

S3E-07 ──> A07 ──> A08
      └───> E10

B7 ─┬──> N0 ──> N1 ──> N2
    │            ├──> N3 ──> N4 ──> INN-15/16/17
    │            └──> INN-19 (还需 L2)
    └──> L0 ──> L1 ──> L2 ──> L3 ──> L4

B1 ──> B2 ──> N3 / INN-17 / INN-18
B1 ──> B4 ──> N2
B3 ──> B5 ──> B6 ──> L3
INN-14 ──> N0（格式定稿后才能出产物）
H07 ──> S2（只写契约，不等于开工）
```

### 13.3 可以立刻并行的四条线（互不抢文件）

1. **A**：R08 预算管理器 —— 只改 `renderer/` 的计时与降档逻辑。
2. **B**：E07 确定性契约 —— 只改 `host/` + 新增 `engine/determinism.*`。
3. **C**：INN-14 几何包格式 + N0 骨架 —— 纯 Node/Python，`tools/offline/`。
4. **D**：H07 输出契约文档 —— 只写 `docs/`。

### 13.4 共享文件冲突热点（并行前先约定）

| 文件 | 谁会改 | 约定 |
| --- | --- | --- |
| `host/main.cpp` | A（加 `--budget-*`）、B（加 `--hash`/`--replay`） | B 持有参数表，A 的开关必须走已存在的 `--debug-view` 风格；每天集中改一次 |
| `.incrementalTesting/core/verify.ps1` | 所有泳道 | 只允许**加一行**调用各自子脚本，子脚本归泳道所有 |
| `shaders/manifest.json` | 仅 A | 其他泳道不得触碰 |
| `engine/scene/index.mjs` | C（lint/查询）、B（E10 场数据） | schema 变更先写进 `docs/` 再改代码 |
| `docs/STATUS.md` | 所有人 | 合并时由一人统一更新 |
| `reports/` | 所有人 | 只 append（版本化目录天然无冲突） |

### 13.5 一人 + AI 的现实并行上限

同一个时间只有一个人，所以"并行"实际是**交错推进 + 把泳道委派给 Agent**。委派的前提是每项任务自带可独立运行的验收脚本（`tools/*.ps1` 风格），否则 Agent 无法自证完成。因此：

- 同时活跃泳道建议 **≤3**（A、B、C 各一条），D 只在写文档时插入。
- 每条泳道**第一个提交必须是验收脚本**，第二个提交才是功能。
- 拿不到验收脚本的任务不要并行，先串行补脚本。

## 14. 与 docx 的偏差及理由（需要你知道）

**我们比 docx 超前的地方**

1. docx 的 S1 只要求"三项高级特性探针"，我们做成了**可用效果**：DXR 阴影有开关与回退、Mesh Shader 有完整 AS+MS+PS 与回退、Bindless 有代际与诊断。
2. docx 没有要求 CI 与版本化证据，我们建了 `.incrementalTesting/core/ci.ps1`、版本化 `reports/`、G1 放行闸门（`.incrementalTesting/core/g1.ps1`）。
3. docx 的 S1 只要求"测试报告"，我们给了 23+22+27 项带断言的证据套件与固定机位截图对比。

**我们落后于 docx 的地方**

1. **R06 最小 RHI** 未做 → 影响 X2（Vulkan）启动条件，也影响 S4 的后端适配。
2. **图形捕获（PIX/RenderDoc）** 本机不可用，docx 的 S1 交付物"图形捕获"缺失（`docs/CAPTURE.md` 已如实记录）。
3. **三套固定图形场景** 只有一套小院。
4. **1080p P95** 口径未测（只有 1280×720）。
5. **S2 完全未开始**（按约定等你确认）。
6. **S3 的音频/脚本** 仍缺；动画与 glTF 蒙皮导入已有首条链路，但高级修正和生产动作库未完成；E01 无层级/组件。
7. **S4–S6** 未开始。

**顺序偏差（并说明为什么可接受）**

docx 的顺序是 S2 → S3，我们实际先做完了 S1 与 S3 的一部分（场景事务、玩法、打包），S2 未动。理由：docx §07 明确允许"先用原生窗口完成游戏核心"作为 S2 的替代路径，且 S3 的场景事务/确定性玩法正是 S2 与 S5 的地基。**但 S2 仍是下一个未过关口，不能因为 S3 有一部分就宣称 S2 可以跳过。**

**任务表比 docx 多出的部分**

R07/R08、E07–E10、S3E-07、H07、A07/A08、Q07–Q09 不是 docx 的要求，而是为了让后续阶段"可验证"而加的地基（确定性哈希、预算上限、误差阈值、图像回归门禁）。它们全部有先决条件与验收，不改变 docx 的阶段顺序，只增加并行度。如果时间紧张，最先砍的是 Q09/Q07（发布门）而不是 E07/R08（地基契约）。

## 15. 分批开工方案（按依赖排序，批次内可并行）

| 批次 | 并行任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| **第 1 批** | A: R08；B: E07；C: INN-14 + N0 骨架；D: H07 文档 | ✅ 0.7.0 | verify 15/15 |
| **第 2 批** | A: R06 → R07；B: E08；C: B7 + L0 + S3E-07 | ✅ 0.8.0 | verify 18/18 |
| **第 3 批** | A: B1 + B3；C: A07 + Q08 + N0 产物；B: E09 | 🟡 | B1/B3 已落地（0.9.0，compute 15/15）；A07/Q08 在 0.10.0；**E09 未开始** |
| **第 4 批** | A: B2 → B4 → B5 → B3b → N1 → N2/N3/N4；L: L1 → L2 → L3 → L4；B: E10；C: A08 + Q07 | ⬜ 待开工 | 顺序由 `docs/GEOMETRY-LAYOUT.md` §10 冻结的硬门决定 |
| **架构冻结** | `docs/GEOMETRY-LAYOUT.md` + 几何包 v2 + `GeometryClass` 路由 + 32 位材质句柄 | ✅ 0.10.0 | 数据/句柄格式已冻结；剔除策略与 HZB 参数未冻结 |

**S2 不在任何批次里。** H07 只是写输出契约文档（防止将来返工），不等于开工 Chromium；H01–H06 只有你确认后才会进入排期。

**第 4 批的顺序不是建议而是硬门**（`docs/GEOMETRY-LAYOUT.md` §10）：B2（ExecuteIndirect）→ B4（HZB）→ B5（visibility buffer + resolve）→ B3b（材质主路径 heap 索引）→ N1（cluster 剔除）→ L1（屏幕+SDF 追踪）。跳过任何一步，N1 都只能在没有间接绘制 / 没有共同输出 / 没有材质句柄的情况下空转。

## 16. 台账维护规则

1. 每个工作包结束：更新对应行的状态、证据路径、缺口；补一条里程碑表记录。
2. 状态只能由证据支撑：报告路径、提交哈希、截图、测试输出，缺一不可。
3. 新增任务必须给 ID（阶段-编号），并写明依赖与验收；不允许出现"无主任务"。
4. 移出范围的任务标记 ❌ 并写理由，不删除行（保留历史）。
5. 本文件与 `docs/STATUS.md` 同步更新；`docs/STATUS.md` 面向阶段结论，本文件面向逐条任务。
6. **创新点不能直接开工**：必须先在 §12 里落成一个工作包 ID（带优先级与先决条件），再进入 §13 的某条泳道。没有工作包 ID 的想法留在 `docs/IDEA-TRIAGE.md`，不进本台账。
