# Weave（簇织）与 Xihe（曦和）原生实现技术方案（原 Nanite/Lumen 方案）

- 版本：草案 v0.2（命名落定；设计文档，运行时未开始，离线 N0/L0/B7/INN-14 已部分落地）
- 日期：2026-10-27
- 目标：在 NativeGPU 引擎（D3D12、C++17、mesh shader、DXR 1.1）上原生实现 Weave 式虚拟几何与 Xihe 式动态全局光照
- 前置状态：G1 已通过（commit `f34499ec526f`，tag `g1`），图形扩展在 G1 后暂停；本方案是**恢复图形扩展后的路线图**，不改变"先确认再开工"的约定
- 参考：Lumen 侧事实来自 Epic 官方《Lumen 技术细节》（[中文 5.7](https://dev.epicgames.com/documentation/zh-cn/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.7)）；Nanite 侧来自公开课程/讲义（见第 11 节，未逐页核对，标注为"需核实"的条目在实现前必须用实测或官方文档确认）
- 多光源/10k 灯的直接光设计见 [LIGHTING-SYSTEM-RESEARCH.md](LIGHTING-SYSTEM-RESEARCH.md)（cluster、light tree、ReSTIR、DDGI、逐灯收据）。

---

## 命名：Weave（簇织）与 Xihe（曦和）

> 文件名 `NANITE-LUMEN-PLAN.md` 保留为历史索引；从本版起，对外特性名、代码目录、CLI 和报告字段统一使用 Weave / Xihe。正文其余部分仍保留 Nanite/Lumen 作为 UE5 对照术语，实现时按本表替换，避免把参考对象的品牌名写进本引擎的特性名。

| UE5 对照 | 本项目特性 | 中文名 | 代码 / CLI | 对应内容 |
| --- | --- | --- | --- | --- |
| Nanite | **Weave** | **簇织** | `renderer/weave`、`--weave` | 虚拟几何：cluster DAG、GPU 驱动 LOD、两阶段遮挡、软/硬混合光栅、visibility buffer、页流送 |
| Lumen | **Xihe** | **曦和** | `renderer/xihe`、`--xihe` | 动态 GI：屏幕追踪、逐网格/全局 SDF、HWRT、探针与辐射缓存、时域降噪、反射与远场 |
| Lumen Lite / SWRT / HWRT | Xihe Lite / SWRT / HWRT | 曦和轻量 / 软件 / 硬件档 | `--xihe lite` / `swrt` / `hwrt` | 质量档位；不再使用 `lumen-*` |
| Surface Cache | Xihe Atlas | 曦和光照图集 | `xihe.atlas` | 卡片/图集；Lite 档由簇辐射缓存替代 |
| Cluster Radiance Cache | Xihe Cluster Cache | 曦和簇辐射缓存 | `xihe.clusterCache` | 低端档的内存换画质方案 |

命名理由：

- **Weave**：cluster 层级被"织"成一张连续的可见表面，强调 LOD 无跳变与零 draw call 膨胀；短、可直接作为 CLI 开关。与引擎已有的布料模块区分时，文档写"Weave 几何 / 簇织几何"，代码目录仍为 `renderer/weave`。
- **Xihe / 曦和**：取"曦"（日光）与"和"（调和、融合），对应动态 GI 中光在表面之间传播并收敛；与 Lumen 没有词根或拼写关联，也避开了已有明显占用的 Aura / Aurora / Halo 等名字。`xihe-lite/swrt/hwrt` 与现有档位一一对应。
- 不在新代码、CLI、报告字段和对外文档中使用 Nanite/Lumen 作为本引擎特性名；它们只出现在 UE5 对照、参考资料和本文件的历史文件名中。

---

## 0. 结论摘要

1. **能做成什么**：可以在本引擎复刻 Nanite 的**几何虚拟化主干**（cluster 层级、GPU 驱动 LOD、两阶段遮挡、软/硬混合光栅、visibility buffer、按页流送）与 Lumen 的**动态 GI 主干**（屏幕追踪 + 距离场软件追踪、表面缓存/卡片、探针与辐照度缓存、降噪、双弹射），但**不可能 1:1 复刻 UE5**，因为 UE5 的 Lumen 依赖大量我们尚未具备的子系统（虚拟纹理、TSR、异步 compute 调度、成熟资产管线）。
2. **必须先补的底座**（三条共享关键路径）：compute 管线与异步 compute 队列、`ExecuteIndirect` GPU 工作图、无界/bindless 描述符 + GPU 场景缓冲、HZB/深度金字塔、visibility buffer、TAA/历史缓冲、最小资产导入（glTF）。
3. **并行开发的真正价值**不在"两条线同时写代码"，而在**共享数据与共享证据**：Weave 与 Xihe 需要的是同一批几何的三种派生物（cluster DAG、SDF、BLAS），分开做必然漂移；并行做才能设计出"一次构建、三处消费"的内容寻址几何包（第 5.1 节）。
4. **硬件目标（2026-10-20 实测修订）**：主目标改为**独显 NVIDIA RTX 5060 Laptop（8.28 GB 显存、DXR 1.2、Mesh Tier 10、SM 6.8、Binding Tier 3）**，同机位 GPU P95 从 0.72 ms 降到 0.117 ms、DispatchRays 从 0.158 ms 降到 0.049 ms；AMD Radeon 890M 核显降为**低端回退目标**。因此完整档（`xihe-swrt` / `xihe-hwrt`）成为主路径，`xihe-lite` 保留用于 iGPU 与低端机（第 3.3.4 节）；"cluster 辐射缓存"（第 5.11 节）从"必需"降级为"低端档差异化"。**DXR 1.2 还解锁了用 `ExecuteIndirect` 派发 `DispatchRays`**，这正是第 5.4 / 5.5 节合并光线与 GPU 工作图的前提。
5. **建议的启动顺序**：先做共享底座（4–6 人周量级），再并行开 Weave 与 Xihe 两条线；中间用 3 个可验证切片（cluster→软光栅→vis buffer；SDF→GI；两者合帧）防跑偏。

---

## 1. 我们的起点（实测，不是估计）

| 能力 | 现状 | 对 Weave/Xihe 的影响 |
| --- | --- | --- |
| 图形 API | D3D12，单一 DIRECT 队列 | 缺 compute 队列 → 软光栅与 GI 探针更新无处安放 |
| 命令 | `DrawIndexedInstanced` / `DrawInstanced` / `DispatchRays` / `DispatchMesh` | **无** `ExecuteIndirect`、无 compute 派发 → 无法做 GPU 驱动工作图 |
| 管线 | 光栅（2 MRT）、合成、DXR（RTPSO+SBT）、Mesh（AS+MS+PS stream PSO） | 已有两条非传统管线，说明 stream PSO/SBT 基础设施可复用 |
| 描述符 | 固定槽位 shader-visible 堆（8 槽材质表 + 每帧块） | **无**无界范围/`ResourceDescriptorHeap` 索引 → 材质 resolve 需要先扩 |
| 深度 | `R32_TYPELESS`，可作 SRV | **无** HZB/深度金字塔 → 遮挡剔除要从零做 |
| 时间与历史 | 每帧 GPU 时间戳（含 RT 窗口） | **无**历史缓冲/TAA → Xihe 的时域降噪与 Weave 稳定化都要新建 |
| 场景规模 | 33 个刚体方盒，TLAS 每帧重建，GPU P95 0.49 ms | 几何量比 Nanite 目标低 4–6 个数量级，不能证明算法可扩展 |
| 资产 | 无导入，程序化几何 | 离线 cluster 切分与 SDF 烘焙都需要先有"网格资产"概念 |
| 证据/CI | 版本化证据、23+22+27 项检查、固定机位截图 diff、P50/P95 与预算 | **这是本项目最大的现成优势**，可直接升级为画质/预算门禁 |
| 硬件 | 主目标 RTX 5060 Laptop：DXR 1.2、Mesh Tier 10、SM 6.8、Binding Tier 3、8.28 GB 显存；回退 AMD 890M 核显 | 主目标能力与显存都够；仍需 P0 探测 64 位原子等细节特性 |

> P0 必须补一条实测：`D3D12_FEATURE_D3D12_OPTIONS1.Int64ShaderOps` 与 64 位原子能力（Nanite 式 visibility buffer 打包常用 64 位原子），以及 `D3D12_OPTIONS7`/`OPTIONS12` 的相关位。能力查询写入报告字段，不允许"假设支持"。

---

## 2. UE5 侧的效果拆解

### 2.1 Nanite：虚拟几何

公开资料一致的骨架如下（细节数字以官方/实测为准）：

```text
离线（编辑器）                          运行时（每帧）
─────────────                          ─────────────
网格 → 切分为 cluster（约 128 三角形级）
     → 自底向上合并成 DAG/层级（带 LOD 误差）
     → 每层存 AABB、父指针、页 ID
                                        实例剔除（视锥/距离）
                                        → 遍历 cluster 层级，按屏幕误差选 LOD 层
                                        → 两阶段遮挡：上一帧 HZB 剔除 + 本帧已写深度二次剔除
                                        → 小三角形 → 软件光栅（compute，tile 化）
                                        → 大三角形 → 硬件光栅
                                        → 合并进 visibility buffer（深度 + clusterID + triID）
                                        → 延迟材质 resolve（每个可见像素取一次材质）
                                        → 按页流送缺失 cluster
```

效果特征（"Nanite 感"从哪来）：**像素级 LOD 无跳变**（每 cluster 独立误差阈值）、**零 draw call 膨胀**（几何量增长不增加 CPU 提交）、**微三角形不丢细节也不闪烁**（软光栅 + 稳定化）、**内存按需**（页流送）。

约束（必须接受，否则会设计出错误的东西）：只支持不透明/遮罩材质；世界位置偏移、骨骼变形、透明与半透明、逐物体排序基本不适用；对 UV/材质层数有隐含限制；软光栅对 64 位原子与原子吞吐敏感。

### 2.2 Lumen：动态 GI 与反射

Epic 官方文档明确的分层（已读原文，见第 11 节链接）：

| 层 | 机制 | 关键事实 |
| --- | --- | --- |
| 1 | **屏幕追踪** | 先做屏幕空间追踪，找不到再走更可靠的方法；支持所有几何类型，用来弥合"Lumen 场景"与"三角形场景"的不一致 |
| 2 | **表面缓存（Surface Cache）** | 对附近表面做自动参数化；每个网格体离线生成 **卡片**（默认 12 张）；从多个角度捕获材质属性；多帧分散更新；覆盖不到的区域显示为粉色，反射里是黑的 |
| 3 | **软件光线追踪（SWRT，默认）** | 网格体距离场（SDF）合并成全局距离场；**细节追踪**在前 1–2 m 用逐网格 SDF，之外用全局距离场；**全局追踪**只用全局距离场；SDF 打包进图集并按距离流送 |
| 4 | **硬件光线追踪（HWRT，可选）** | 直接追踪三角形，支持蒙皮网格；可用"击中照射"提升反射画质；重叠实例多时开销大；Nanite 网格使用**退却网格（fallback mesh）**参与追踪 |
| 5 | 时域与降噪 | 最终采集、辐照度缓存/探针、时域累积与空间滤波；光照变化传播速度可调 |
| 6 | 远场 | HWRT + HLOD 可延伸到约 1 km；SWRT 场景约 200 m（可调到 800 m） |

效果特征：**间接光随日照/材质实时变化**、**多次弹射**、**粗糙度正确的镜面反射**（表面缓存廉价 / 击中照射高质）、**室内外过渡自然**。
约束：不兼容前向着色；SWRT 只支持静态网格/实例化静态网格/地形，**不支持世界位置偏移**，薄壁 < 10 cm 会漏光，单面网格背面不可见；需要"模块化几何"（墙/地板/天花板分开）；不适用于 VR。

### 2.3 两者在 UE5 里本来就是耦合的（这就是并行开发的依据）

官方文档直接写明三处耦合：

1. **Nanite 加速表面缓存捕获**——Lumen 的场景捕获依赖 Nanite 的 LOD 与多视图光栅化，否则高模场景捕获极慢；
2. **HWRT 使用 Nanite 退却网格**——Lumen 的硬件追踪与 Nanite 全细节网格之间存在不一致，需要 fallback mesh 折中，屏幕追踪再掩盖残余差异；
3. **远场/大世界**——Lumen 的远场依赖 HLOD 与 Nanite 的 LOD 体系。

结论：**几何中枢不统一，Lumen 一定会在"光线看不到 Nanite 全细节"这件事上翻车**。这决定了第 5.1 节"一次构建、三处消费"的几何包不是锦上添花，而是必要架构。

---

## 3. 原生实现架构

### 3.1 共享底座（P0，必须先做，两条线的公共依赖）

| 编号 | 能力 | 为什么两条线都需要 | 验收 |
| --- | --- | --- | --- |
| B1 | compute 管线 + 独立 COMPUTE 队列（含 fence 互联） | Weave 软光栅、Xihe SDF 追踪/探针更新/降噪 | 一个 compute pass 能读写 UAV 并与图形队列正确同步（证据：fence 时序 + 结果校验） |
| B2 | `ExecuteIndirect` + GPU 计数器/参数缓冲 | cluster 派发、探针派发的数量在 GPU 上决定 | 无 CPU 回读的间接派发，参数由 GPU 写入（证据：间接参数 dump） |
| B3 | 无界/大堆描述符 + bindless 材质访问（SM 6.6 路径 + 有界回退） | 材质 resolve、卡片捕获、cluster 材质 ID | `ResourceDescriptorHeap` 路径可用且有有界回退；报告 `bindless.mode` |
| B4 | HZB / 深度金字塔（每帧构建） | Weave 两阶段遮挡、Xihe 屏幕追踪 | HZB 与深度逐级一致（证据：逐级采样对比） |
| B5 | visibility buffer（64 位：深度 + cluster + 三角） | Weave 输出、Xihe 屏幕追踪复用 | 与 CPU 参考光栅逐像素一致（小场景） |
| B6 | TAA / 历史缓冲 + 相机抖动 | Xihe 降噪与稳定、Weave 稳定化 | 固定种子下截图可复现（证据：两次运行 diff=0） |
| B7 | 最小资产管线：glTF 顶点/索引/材质 → 引擎网格 | 没有真实网格就无法证明 cluster 与 SDF | 导入 1 个 >10 万三角形模型并渲染 |
| B8 | 能力探测与降级矩阵（含 `Int64ShaderOps`） | 决定软光栅与 HWRT 能否启用 | 报告能力位 + 降级原因，不静默 |

> 底座验收建议沿用 G1 风格：每项一个小门禁脚本，写入 `reports/<version>/<commit>/`，不允许"口头完成"。

### 3.2 Weave 管线设计（N0–N4）

```text
N0 离线构建（CPU，可并行/可缓存）
   输入：网格（glTF）
   输出（内容寻址，见 5.1）：cluster 列表 + 层级 DAG + 每 cluster AABB/误差/页 ID + 材质区间
   验收：同一输入重复构建 hash 一致；篡改一字节后重建必须报冲突（复用现有 BUILD_CONFLICT 机制）

N1 GPU 场景与剔除
   instance buffer（矩阵、材质、cluster 根）
   → 视锥/距离剔除（compute）
   → cluster 层级遍历 + 屏幕误差选层（compute，输出可见 cluster 列表 + 间接参数）
   验收：与 CPU 参考选择的 cluster 集合一致（允许误差阈值内差 1 层）

N2 两阶段遮挡
   pass1：用上一帧 HZB 剔除（保守）
   pass2：用本帧已写深度重建 HZB 的粗糙层，再剔除新出现的 cluster
   验收：遮挡剔除率可测；不出现"该出现的被剔掉"（用深度参考对比）

N3 光栅化（软 + 硬混合）
   软：compute 按 64×64 tile 做保守光栅 + 深度/ID 原子写入
   硬：边长 > 阈值的三角形走硬件光栅（可复用现有 mesh shader 路径）
   合并：同一 visibility buffer，深度比较决定胜负
   验收：软/硬边界处无重复着色、无空洞；微三角形场景截图无闪烁（连续 60 帧 diff 指标）

N4 材质 resolve + 流送
   resolve：每像素读 cluster/triID → 取属性 → 走与现有光栅一致的 BRDF/光照
   流送：页表 + 缺失页请求（优先复用现有内容寻址存储）
   验收：resolve 结果与"直接用全分辨率网格光栅"的参考图 SSIM ≥ 阈值；缺页时降级到最近可用 LOD 且报告
```

与现有代码的复用点：stream PSO 与 SBT 基础设施（已具备）、DXR 阴影 pass（可直接对同一 cluster 层级发射光线，见 5.3.3）、证据/截图/timing 体系（直接升级为门禁）。

### 3.3 Xihe 管线设计（L0–L4）

#### 3.3.1 场景表示

| 表示 | 用途 | 本引擎做法 |
| --- | --- | --- |
| 网格 SDF | 近场精确追踪 | 离线烘焙，复用 5.1 的几何包；分辨率按导入尺度分配 |
| 全局 SDF（clipmap） | 远场廉价追踪 | 每帧增量合并，clip 随相机移动 |
| 表面缓存/卡片 | 击中点取光照 | **Lite 档先不做**（内存），用 5.3.5 的 cluster 辐射缓存替代；完整档再补卡片图集 |
| 探针/辐照度缓存 | 最终采集 | 屏幕探针（低分辨率）+ 世界空间探针（两级） |
| 屏幕追踪 | 首选命中 | 复用 HZB + visibility buffer（5.2） |

#### 3.3.2 追踪顺序（与 UE5 对齐）

```text
每像素/每探针：
  1) 屏幕空间追踪（HZB march，用上一帧深度 + 本帧 vis buffer）
  2) 未命中 → 软件追踪：近场逐网格 SDF（1–2 m），远场全局 SDF
  3) 仍未命中 → 天空/远场（HWRT 远场可选）
  4) 命中点取辐照度：cluster 辐射缓存（Lite）或表面缓存卡片（完整）
  5) 时域累积 + 空间滤波 + 最终采集
```

#### 3.3.3 质量档位

| 档 | 追踪 | 弹射 | 探针 | 内存目标 | 目标硬件 |
| --- | --- | --- | --- | --- | --- |
| `xihe-off` | 无 | 0 | 无 | 0 | 全部 |
| `xihe-lite` | 屏幕 + 近场 cluster DAG | 1 | 屏幕探针 1/4 分辨率 | < 64 MB | iGPU / 低端机回退 |
| `xihe-swrt` | 屏幕 + 逐网格 SDF + 全局 SDF | 2 | 屏幕 + 世界探针 | < 192 MB | 独显 |
| `xihe-hwrt` | 屏幕 + HWRT（triangle） | 2–3 | 同上 + 击中照射 | 视 BVH 而定 | 独显 |

每档都必须能截图对比、能回退、有预算数据；**默认档位按适配器能力与显存自动选择，并把选择理由写进报告**。

#### 3.3.4 为什么保留 Lite 档（低端回退）

主目标已改为独显（8.28 GB 显存），完整档不再受内存墙限制。Lite 档保留两个用途：一是同一份代码在 AMD 890M 核显（约 94 MB 占用、共享内存）上仍可运行，二是作为"预算调控器"的降级阶梯目标——超预算时可以自动降到 Lite 而不是掉帧。Lite 档用 cluster 级辐射缓存换内存（5.11 节），牺牲高频细节但保住"间接光随日照变化"的核心观感。

### 3.4 接口契约（先冻结接口，再并行实现）

```cpp
// renderer/gi/contracts.h（设计稿，未实现）
struct ClusterScene;      // cluster 层级、页表、材质区间；由几何包加载
struct GpuScene;          // 实例、材质、变换、可见性位
struct VisibilityBuffer;  // 深度 + clusterID + triID，双消费者（Weave resolve / Xihe 屏幕追踪）
struct RayTracingContext; // TLAS/BLAS/SBT 与光线预算，统一 shadow/GI/反射
struct RadianceCache;     // 探针或 cluster 辐射，提供 Fetch(位置, 法线)
struct FrameBudget;       // 分阶段 GPU 时间预算，供两条线共同申领
```

契约冻结后，Weave 只依赖 `GpuScene/VisibilityBuffer`，Xihe 只依赖 `ClusterScene/VisibilityBuffer/RadianceCache`，两条线可以真正并行。

### 3.5 Weave 扩展边界：植被 / 布料 / 水体 / 角色形变

结论：**可以扩展，但不能把所有动态内容都塞进 cluster DAG。** Weave 的核心收益来自"静止顶点 + 不透明/遮罩 + 可流送页"；一旦每帧改顶点、改拓扑或做透明混合，cluster 的 LOD 误差、页命中和两阶段遮挡都会失效。扩展按三条通道划分：

| 通道 | 几何来源 | 适用内容 | 不适用 / 代价 |
| --- | --- | --- | --- |
| **Weave Static** | cluster DAG + 页流送 | 树干/岩石/地形/建筑/静态道具/静态旗帜/河床 | 不透明或 alpha 遮罩；顶点静止；透明、折射、逐顶点形变不进 |
| **Weave Dynamic**（新增，可选） | 常驻 cluster 缓冲 + 每帧 bounds + compute 形变 | 近景 hero 树风动、VAT 群众、可 cluster 化的动态道具 | 不做页流送；每帧重建 cluster bounds；LOD 误差按静止姿态，动态姿态要留保守余量 |
| **独立管线** | 各自求解/渲染网格 | 模拟布料、水面/液体、英雄角色蒙皮、面部 morph、毛发、粒子 | 共享 GpuScene / VisibilityBuffer 标签 / HZB / FrameBudget / Xihe 代理，不共享 cluster DAG |

逐项判断：

- **植被**：树干、岩石、远景树冠可进 Weave Static；近景 hero tree 可进 cluster DAG。叶片/草不建议逐片进 cluster：alpha-test overdraw、实例数和 draw call 才是主成本，cluster DAG 只解决几何复杂度（[VEGETATION-PLAN.md:14](../vegetation/VEGETATION-PLAN.md#L14)）。第一版保持"实例化 + LOD + impostor + alpha-test + 阴影代理"，Weave 只接管 hero tree；风动先用实例/顶点级 WPO，真正需要 cluster 级风动时再开 Weave Dynamic。
- **水体**：水面不是 Weave 的好载体。水面是透明/折射、每帧高度场/波谱驱动、需要独立透明排序和前后表面规则；进 visbuffer 会破坏不透明假设。Weave 只接管河床、岸线、水下地形、礁石等静态几何；水面继续走 WaterBody 的解析/浅水/频谱/局部液体管线和透明 pass（[PARTICLES-FIELDS-CLOTH-WATER-PLAN.md:206](../simulation-effects/PARTICLES-FIELDS-CLOTH-WATER-PLAN.md#L206)、[WATER-REFERENCE.md:3](../core/WATER-REFERENCE.md#L3)）。Xihe 侧给水面一个代理平面/粗略 SDF，避免 GI 和反射把水当空气。
- **布料**：模拟布料每帧改顶点，不能用 Weave 的静态 cluster 页；现有 GPU 布料求解器/绘制路径已经独立且写共享深度（[CLOTH-GPU.md:97](CLOTH-GPU.md#L97)、[CLOTH-RENDERING.md:30](CLOTH-RENDERING.md#L30)）。推荐"布料独立求解 + 共享 visbuffer 标签/深度/材质 resolve"，而不是把布料塞进 cluster DAG。静态旗帜、棚布可以走 Weave Static；角色披风/裙摆先补渲染网格重心映射、LOD 拓扑切换和动态 bounds，再谈 Weave Dynamic。
- **动作 / 人物形变**：英雄角色的骨骼蒙皮、morph、软组织和面部形变都是逐顶点、逐帧变化，Weave 的 cluster LOD/流送不适用；现有 `VSSkin`/morph/软组织路径应保留（[ANIMATION-M1-M4.md:28](../animation-audio/ANIMATION-M1-M4.md#L28)）。可进 Weave 的是角色盔甲、刚性道具、群众 VAT/impostor 的静态部分；群众角色若要 cluster 化，走 Weave Dynamic，且 LOD 误差要按动画包围盒留余量。面部、毛发、皮肤不进入 Weave。

**准入条件（任何新内容进 Weave 前必须回答）**：

1. 材质是否不透明/遮罩？透明、折射、OIT 一律留在独立透明管线。
2. 顶点是否静止？每帧骨骼/morph/拓扑变化只能走 Weave Dynamic 或独立管线。
3. 是否可流送？Weave Dynamic 必须常驻，不能按页缺失降级。
4. 阴影和 Xihe 是否有代理？动态内容必须有 proxy BLAS/SDF，否则 GI 看不到。
5. 是否有回退和证据？每项扩展都要有 `--weave`/独立开关、截图、预算和误差门禁。

**建议顺序**：W1 静态 Weave + 植被 hero tree（先扩 UV/切线/双面/alpha 字段）；W2 Weave Dynamic（每帧 bounds + compute 形变 + VAT）；W3 布料/角色通过共享 visbuffer 标签接入；W4 水体只做静态河床/岸线 + 水面代理；W5 Xihe 代理与预算门禁。

---

## 4. 并行开发策略

### 4.1 依赖图

```text
                 ┌──────────── P0 共享底座（B1..B8）────────────┐
                 │ compute+indirect → bindless → HZB → visbuf → TAA │
                 └───────────────┬───────────────────────────────┘
         ┌───────────────────────┴────────────────────────┐
         ▼                                                ▼
   N0 离线 cluster 构建                          L0 离线 SDF 烘焙
   N1 剔除/LOD 选择                              L1 追踪（屏幕+SDF）
   N2 两阶段遮挡  ────────┐                      L2 探针/辐射缓存
   N3 软+硬光栅          │ 共享 VisibilityBuffer │ L3 降噪/最终采集
   N4 resolve + 流送     └──────────┬────────────┘ L4 反射/远场
                                    ▼
                         I1 合帧 + FrameBudget 调控器
                         I2 图像回归 + ground truth 误差门禁
```

可**立刻并行**（不依赖 GPU 底座）：N0 离线切分工具、L0 SDF 烘焙、参考路径追踪器、证据脚手架、调试可视化（cluster 边界、SDF 切片、卡片覆盖）。
**必须等底座**：N1–N4、L1–L4 的 GPU 部分。
**必须串行**：I1 合帧（合成顺序、预算分配）、I2 门禁阈值标定。

### 4.2 里程碑与验收门

| 里程碑 | 内容 | 验收（可回归证据） |
| --- | --- | --- |
| M0 | 底座 B1–B8 | 8 个门禁脚本全绿；能力矩阵 JSON；无静默降级 |
| M1 | 几何包 + 契约冻结 | 同一网格重复构建 hash 一致；篡改检测；`contracts.h` 冻结并由两条线各自 mock |
| M2 | Weave 切片：1M 三角形单物体 | vis buffer 与 CPU 参考一致；cluster 选择与参考一致；截图 + 剔除率统计 |
| M3 | Xihe 切片：单点光 GI | 与 path-traced 参考 relMSE < 阈值；无粉区；截图 + 探针数/ray 数 |
| M4 | 两者合帧 | P95 预算达标；图像回归通过；两条路径均可开关 |
| M5 | 流送 + 大场景 | 缺页降级不崩；页命中率；内存峰值 < 档位上限 |
| M6 | 画质/性能门禁固化 | 每版本自动跑：预算 + SSIM + relMSE；超阈值即失败 |

### 4.3 防跑偏的三条硬规则

1. **每个里程碑必须能在同一机位截出图**，没有图的"完成"不算完成（沿用 G1 文化）。
2. **每个新能力必须有回退开关**（`--weave off`、`--xihe off/lite/swrt/hwrt`），回退路径必须和原路径一样的证据标准。
3. **能力查询与效果实现分字段**（`probeAvailable` / `effectImplemented` / `fallbackUsed`），沿用 G1 的既有约定，禁止把"设备支持"写成"已实现"。

---

## 5. 并行开发能增加的创新点（重点）

下面分三类：**只有并行才成立的**（A）、**单线也能做但并行收益更大的**（B）、**面向本机约束的差异化**（C）。每项给出代价、收益与验收指标，避免"听起来很酷但没法验证"。

### 5.1 A1 内容寻址几何包：一次构建、三处消费

- **想法**：离线一次构建同时产出 `cluster DAG + 网格 SDF + BLAS + 每 cluster AABB + 卡片描述`，全部以源网格内容哈希为键打包；运行时只加载一个 bundle，并校验各派生物与源哈希一致。
- **为什么并行才成立**：Weave 与 Xihe 对同一网格需要不同派生物，分开构建必然漂移（SDF 与 cluster 不一致 → 漏光/自遮挡；fallback mesh 与全细节不一致 → HWRT 与屏幕追踪接缝）。统一构建把"一致性"变成结构保证，而不是靠调参。
- **代价/收益**：M 量级（离线工具 + 格式 + 校验）；收益是消除一整类接缝 bug，并让 CI 可复现（改一个网格 → 所有派生物确定性重建）。
- **验收指标**：`bundleHash` 写入报告；加载时断言 cluster 数/SDF 维度/材质区间与清单一致；篡改一字节必须报 `BUILD_CONFLICT`（复用现有机制）。

### 5.2 A2 cluster-native SWRT：用 cluster 层级做近场追踪，SDF 只负责远场

- **想法**：Lumen 的 SWRT 用 SDF 是因为它便宜且通用，代价是薄壁漏光（官方明确 < 10 cm 壁厚会漏）。我们已有 cluster 层级（带 AABB），可直接作为**软件 BVH** 做近场光线求交，SDF clipmap 只处理 20 m 之外；两者按距离混合。
- **为什么并行才成立**：它把 Weave 的数据结构变成 Xihe 的追踪结构——只有两条线一起设计才会想到复用 DAG 而不是另建一套 BVH。
- **代价/收益**：L 量级（光线-三角形求交 + 遍历优化）；收益是**近场零漏光**、免去近场 SDF 烘焙与内存，且与 Weave 几何天然一致。
- **验收指标**：rays/ms（每个 SM 每毫秒光线数）；薄壁（5 cm）测试场景漏光像素数（目标 0，SDF 对照组 > 0）；GI relMSE。

### 5.3 A3 Visibility buffer 双消费者

- **想法**：Weave 的 resolve 与 Xihe 的屏幕追踪/反射都从同一张 64 位 visibility buffer 取几何，不再各存一套 G-buffer。
- **收益**：省带宽（对核显最关键）、省 RT；且"看到什么"与"GI 追踪到什么"天然一致，减少屏幕追踪与 Xihe 场景的接缝。
- **代价**：M（resolve 与追踪共享解码函数；法线/材质按需取）。
- **验收**：bytes/pixel、pass 数、屏幕追踪命中率；与"独立 G-buffer"对照组做画质对比。

### 5.4 A4 合并光线预算：一次 DispatchRays 同时做阴影 / GI / 反射

- **想法**：把三类光线请求打包进 StructuredBuffer，一个 raygen 循环处理，用不同 hit group / miss 索引分流，结果写不同类型输出。我们已有 RTPSO+SBT（阴影 0.16 ms），扩成多 hit group 很自然。
- **收益**：派发次数 3→1，减少 iGPU 上的派发与 SBT 切换开销；光线预算可统一分配（重要的 GI 光线优先）。
- **代价**：M（SBT 组织、payload 统一、调试可视化）。
- **验收**：派发次数、GPU 时间、与分开派发的画质一致性（SSIM）。

### 5.5 A5 GPU 工作图：用 ExecuteIndirect 串起两条线

- **想法**：剔除 → 压缩 → 间接软光栅；探针选择 → 压缩 → 间接追踪。两条线共用一套"计数器 + 间接参数"调度，帧预算调控器可以直接改写参数里的数量。
- **收益**：无 CPU 回读、无同步停顿；**预算调控可以在 GPU 侧生效**（超预算就少派发），这是仅靠 CPU 调参做不到的。
- **代价**：M（命令签名 + 计数器管理 + 调试 dump）。
- **验收**：CPU 提交时间（当前 0.53 ms）不随几何量增长；帧间方差；间接参数 dump 可作为证据。

### 5.6 B1 帧预算调控器（FrameBudget governor）

- **想法**：把 Weave 的 cluster 误差阈值、Xihe 的 ray/探针数量、降噪强度统一到一个 GPU-ms 预算下，用 PI 控制器按 P95 目标动态调整，每帧输出调参记录。
- **收益**：画质档位从"静态预设"变成"动态满足预算"；直接复用我们的 P50/P95 采集与证据目录。
- **验收**：超预算帧占比 < 1%；调参不引起可见跳变（帧间亮度差阈值）；调参日志可回放。

### 5.7 B2 图像回归门禁（SSIM / ΔE，而不是"看起来对"）

- **想法**：固定机位 + 固定种子 + 历史重置，输出参考图集；CI 用 SSIM + 最大 ΔE 判定回归。
- **关键细节**：Weave/Xihe 都带时域，**必须固定种子并显式重置历史**，否则截图不可复现——这会迫使我们把 TAA 设计成可确定性重置（本身就是好设计）。
- **验收**：同输入两次运行 diff=0；改动前后 SSIM 与 ΔE 写入报告；阈值标定过程留档。

### 5.8 B3 自带 ground truth：引擎内路径追踪参考

- **想法**：用现有 DXR 写一个低分辨率单向路径追踪器（1–4 弹射，N spp），对固定场景生成参考辐照度/图像；Xihe 的误差用 relMSE/ΔE 直接度量。
- **为什么难得**：多数自研引擎没有可信的 GI 参考，"GI 看起来对"无法证伪。我们有 DXR，做这件事的成本远低于它的价值——它同时也能验证 Weave 的 vis buffer resolve（与精确三角形路径追踪对比）。
- **验收**：参考图收敛曲线（spp vs 噪声）；Xihe 各档 relMSE 排序合理（lite > swrt > hwrt 误差递减）。

### 5.9 B4 双路径一致性（SWRT vs HWRT 差分测试）

- **想法**：同一帧同时跑 SDF 与 HWRT 两种追踪，输出差异可视化，并断言差异在已知限制内（薄壁、WPO 除外）。
- **收益**：自动暴露 SDF 偏差与漏光，不必靠人眼找粉区。

### 5.10 B5 分阶段 GPU 时间戳仪表盘

- **想法**：把现有时间戳从"整帧 + DispatchRays"扩展到 cluster 选择 / 软光栅 / resolve / SDF 追踪 / 探针更新 / 降噪 / 时间累积 各段，超预算自动触发截图归档。
- **验收**：每段有 P50/P95；超预算时证据目录里能直接找到那一帧的图与时序。

### 5.11 C1 Cluster 辐射缓存（替代表面缓存图集，核显关键）

- **想法**：Lumen 的表面缓存是 texel 图集，内存 O(表面积×精度)。在核显上换一种表示：给每个 cluster 存一份 SH2/RGB 辐照度（外加少量方向样本），按可见性/变化率轮转更新。内存 O(cluster 数)，远小于图集；代价是高频方向细节丢失（粗糙反射更糊、锐利反射需要 HWRT 补）。
- **为什么是差异化**：这是在"内存受限硬件上跑动态 GI"的务实解法，UE5 不做是因为主机/独显不缺这点显存。
- **验收**：内存峰值；GI relMSE 与图集方案对比；轮转更新延迟（光照变化到收敛的帧数）。

### 5.12 C2 薄壁/单面几何诊断视图

- **想法**：照 Lumen 的粉区思路做本引擎的诊断覆盖层：无 cluster 覆盖、SDF 薄壁风险、缺卡片、退化 cluster 分别用固定颜色标记，并可截图归档。
- **收益**：把"美术/资产问题"和"算法问题"分开，减少误判；成本极低（一个 debug pass）。

### 5.13 C3 Weave 阴影复用 cluster 层级（cluster 级可见性缓存）

- **想法**：阴影光线已经走 DXR（1 条/像素）。进一步：按 cluster 缓存"朝向 key light 的可见性位图/标量"，太阳或 cluster 变换不变时复用，只对变化 cluster 重追。
- **风险**：动态太阳/动态实例会让缓存频繁失效；作为研究项，不进主路径。
- **验收**：静态太阳下阴影 GPU 时间下降幅度；切换太阳时的失效与重建成本；画质与全量追踪 SSIM。

### 5.14 D1 与 Chromium（S2）的协同契约

- **想法**：把 `composite + fence` 作为 S2 的共享纹理契约（我们已经把合成结果留在独立纹理上），并可选暴露 visibility buffer 供宿主侧调试。这样第 5 项不需要为 Weave/Xihe 返工。
- **收益**：跨阶段复用；宿主的预算面板可以直接读我们的 JSON。

---

## 6. 工作量与风险

### 6.1 粗估（人周，含调试与证据；仅作排序参考，不作为承诺）

| 阶段 | 内容 | 规模 | 粗估 |
| --- | --- | --- | --- |
| P0 | 共享底座 B1–B8 | XL | 6–10 |
| M1 | 几何包 + 契约 | M | 2–3 |
| N | Weave N0–N4（含流送） | XL | 8–12 |
| L | Xihe L0–L4（lite→swrt→hwrt） | XL | 10–16 |
| I | 合帧 + 预算 + 门禁 | M | 3–5 |
| 合计 | | | **29–46 人周** |

说明：这是"能跑、可回退、有证据"的成本，不是"做到 UE5 画质"的成本。UE5 级 Lumen 的质量还依赖 TSR、虚拟纹理、材质系统与大量调优，不在本估算内。

### 6.2 风险登记

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 低端/核显内存带宽不足 | Lite 档跑不动 | 预算调控器自动降档 + cluster 辐射缓存 + 硬内存上限（超限即降档并报告） |
| 软光栅原子吞吐低 | Weave 光栅变慢 | 64×64 tile + 深度粗筛 + 大三角形走硬件；先做能力探测 |
| SDF 漏光/薄壁 | GI 错误 | cluster-native 近场（5.2）+ 诊断视图 + 壁厚约束写入资产规范 |
| 时域非确定性 | 证据不可复现 | 固定种子 + 历史重置 + 双重运行 diff=0 门禁 |
| 两条线数据结构漂移 | 接缝 bug | 几何包（5.1）+ 契约冻结（3.4） |
| 资产管线缺失 | 无法证明可扩展 | B7 最小 glTF + 程序化百万三角形生成器（CI 友好） |
| 范围蔓延 | 永远做不完 | 档位化交付（lite 先跑通）+ 5 个有效工作日止损规则（沿用 NEXT-PHASE） |

### 6.3 止损规则（沿用现有约定）

同一驱动/互操作问题投入 5 个有效工作日无进展：保存最小复现、缩小范围、记录架构变更，不继续堆功能。没有截图/时序证据不能宣称效果完成。

---

## 7. 明确的"不做"与"不承诺"

- 不做 UE5 的完整材质图/虚拟纹理/TSR 复刻；材质仍走现有有界/无界表 + 程序化或 glTF 基础材质。
- 不做 Weave 的编辑器工具链（切分预览、误差可视化可以有，但不做资产编辑器）。
- 不承诺像素级与 UE5 一致；目标是"同类效果、可测量、可回退"。
- 不在 Xihe 第一版做半透明/体积雾/毛发/植被；这些依赖大量前置系统。
- 不为通过测试放宽证据标准（沿用 G1 约束）。

---

## 8. 下一步：三个可选启动包

| 包 | 内容 | 适合什么情况 | 首个可见成果 |
| --- | --- | --- | --- |
| **A（推荐）** | P0 底座 + 几何包 + Weave 切片 | 想先把"几何虚拟化"立住，Xihe 随后 | 百万三角形单物体在 60 fps 下无 draw call 膨胀，vis buffer 截图 + 剔除率 |
| B | P0 底座 + Xihe-lite（SDF + cluster 近场） | 想先看到"动态 GI"的观感 | 单点光/太阳下的间接光截图 + relMSE |
| C | 只做离线工具 + 参考路径追踪 + 证据门禁 | 想先建立"可测量"的地基 | 几何包与 ground truth 参考图集，CI 可跑 |

### 需要你决定的 3 个问题

1. **画质目标**：主目标改为独显完整档（`xihe-swrt` 起步、`xihe-hwrt` 可选），Lite 档作为低端回退——同意这个调整吗？
2. **顺序**：按 A（Weave 先）还是 B（Xihe 先）？两者共享底座相同，只影响第一个可见成果。
3. **资产来源**：先接 glTF 最小导入，还是继续用"程序化百万三角形生成器"作为唯一输入？（我建议先做生成器 + 一个 glTF 用例，兼顾 CI 与真实感）

---

## 9. 与现有路线的关系

- 本方案不改变 `docs/NEXT-PHASE.md` 的第 5 项优先顺序：Chromium S2 仍等你确认；Weave/Xihe 是 S2 之后的图形扩展，可与 S2 并行推进**离线部分**（几何包、SDF 烘焙、参考路径追踪），因为那些不占用 Chromium 宿主的关键路径。
- 若你希望先做 Weave/Xihe 再进 Chromium，需要显式调整 NEXT-PHASE 的顺序，我会把变更写进文档与 tag，不会静默改计划。

---

## 10. 术语

| 术语 | 含义 |
| --- | --- |
| cluster | 几何的最小可剔除/可光栅单元（UE5 约 128 三角形级） |
| DAG | cluster 的层级结构（父 cluster 是子 cluster 的简化） |
| HZB | Hierarchical Z-Buffer，深度金字塔，用于遮挡剔除与屏幕追踪 |
| visibility buffer | 记录每像素命中三角形/聚簇 ID 与深度的缓冲，延迟材质 |
| SWRT / HWRT | 软件（距离场）/ 硬件（三角形）光线追踪 |
| 表面缓存 / 卡片 | Lumen 对表面做自动参数化，用离线生成的卡片捕获材质光照 |
| 退却网格（fallback mesh） | Nanite 为光线追踪准备的简化网格 |

---

## 11. 参考资料

- Epic，《Lumen 技术细节》（本次已读，用于第 2.2 节事实）：[中文 5.7](https://dev.epicgames.com/documentation/zh-cn/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.7)
- Epic，《Lumen 技术细节》（英文 5.3，作为版本对照）：[English 5.3](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.3)
- Nanite 公开讲义（**未逐页核对**，仅作进一步阅读）：[UCSD CSE272 Nanite 讲义](https://cseweb.ucsd.edu/~tzli/cse272/wi2025/lectures/27_nanite.pdf)、[LMU Nanite 讲义](http://www.medien.ifi.lmu.de/lehre/ws2122/gp/slides/gp-ws2122-extra-nanite.pdf)、[UE5 Nanite 系统综述](https://raw.githubusercontent.com/AndrewAltimit/Media/main/ue5_nanite_system.pdf)
- UE5 渲染管线概览（社区教程，**未逐页核对**）：[Pipeline de renderização da UE5](https://dev.epicgames.com/community/learning/tutorials/Lee9b/unreal-engine-pipeline-de-renderizacao-ue5)

> 标注"需核实"的 Nanite 细节（cluster 三角形上限、软光栅 tile 尺寸、两阶段遮挡的具体实现顺序、64 位原子打包格式）在进入 M2 前必须用官方文档或实测确认；本方案不把这些数字当作既定事实。
