# 实时多光源与 Lumen 式全局光照：完整方案、技术路线与创新点

- 版本：v0.1（设计文档）
- 日期：2026-10-21
- 基线：NativeGPU 0.10.0（D3D12、DXR 1.2、compute 队列、SDF 烘焙、内容寻址证据）
- 关系：本文补 [NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md) 里“直接光/多光源”的缺口；Lumen 部分与其 L0–L4 对齐，不重复 Nanite/几何设计。体积雾/光轴另见 [VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md)。现状与限制见 [RT-SHADOWS.md](RT-SHADOWS.md) 和 [MATERIAL-PIPELINE.md](MATERIAL-PIPELINE.md)。
- 读法：第 0 节给结论；第 2–4 节回答“10,000 盏灯怎么实时”；第 5 节给 Lumen 路线；第 6 节给创新点；第 7 节给排期。

---

## 0. 结论先行

1. **10,000 盏灯不是“10,000 张阴影图”的问题，而是四级预算问题**：剔除（哪些灯可能影响画面）→ 分箱（每像素/每 cluster 看哪些灯）→ 采样（每像素实际算哪几盏）→ 可见性（哪几盏值得追阴影）。任何一级用错，后面都会崩。
2. **推荐的主架构是混合式**：GPU light buffer + 3D clustered forward 处理大量无阴影局部光，light BVH/light tree + ReSTIR DI 处理“多而需要阴影”的灯，DDGI/辐照度探针处理漫反射 GI，屏幕追踪 + HWRT/SDF 处理镜面与远场，程序化天空 + IBL 提供环境光，体积雾/光轴作为独立并行线（[VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md)）。**没有单一算法能同时覆盖 10k 灯、阴影、GI 和反射。**
3. **本引擎已经有的底座**：DXR 1.2、TLAS/BLAS、单射线 key 阴影、compute 队列、SDF 烘焙、内容寻址包、帧预算管理器、图像回归与收据，以及局部粒子体积烟（FIELD F1–F3）。**缺的是**：light buffer、cluster 分箱、light tree、ReSTIR、DDGI、HZB、TAA/历史缓冲、bindless 主路径、真正的天空/IBL、大气体积雾/光轴。
4. **Lumen 不能“启用”**。它是 UE5 渲染器的一部分，依赖 Surface Cache、Radiance Cache、Screen Probe、HWRT 回退等一整套内部结构，不是可链接的库。可行路线是 **Lumen-like**：表面缓存/辐射缓存 + 屏幕追踪 + SDF/HWRT 回退 + 最终采集 + 降噪，按档位交付。
5. **最值得做的创新**：确定性 reservoir 渲染（可回放 ReSTIR）、逐灯收据/预算、cluster + light tree 混合重要性采样、把自发光材质当一等光源（窗户/霓虹）、低端机 cluster 辐射缓存、AI 灯光作者化与逆光照。
6. **粗估**（独显 RTX 5060 Laptop，1080p，10k 灯）：cluster 分箱 0.3–0.6 ms、直接光 1–2 ms、太阳阴影 0.3 ms、ReSTIR 1.5–3 ms、DDGI 0.5–1 ms、降噪 0.5–1 ms，总计约 **4–8 ms**，给 16.6 ms 预算留出余量。数字是量级估算，必须用基准场景实测冻结。

---

## 1. 问题定义：10,000 盏灯到底难在哪

### 1.1 什么算一盏“灯”

| 类型 | 例子 | 特点 | 是否适合逐灯阴影 |
| --- | --- | --- | --- |
| 点光 | 路灯、灯泡 | 位置 + 半径 + 颜色/强度 | 只对屏幕贡献大的少数 |
| 聚光 | 车灯、舞台灯 | 方向 + 内外锥角 | 只对少数 |
| 面光 | 窗、广告牌、霓虹 | 矩形/三角形 + 发光材质 | 用 light tree/面积采样，不做逐灯阴影图 |
| 自发光网格 | 10k 个窗户、屏幕 | 由材质 emissive 产生 | 进入 light tree，按功率采样 |
| 方向光/太阳 | 1 盏 | 全局 | 一张 RT 阴影/CSM |
| 天空/IBL | 1 个 | 环境 | 探针/球谐 |

“10,000 盏灯”通常不是 10,000 盏都需要阴影，而是 10,000 个发光体/光源条目，其中几十到几百个对当前画面有显著贡献。

### 1.2 三个独立预算

```text
总成本 ≈ 像素数 × 每像素评估灯数 × 单灯 ALU
       + 像素数 × 每像素阴影射线数 × 单射线成本
       + 探针数 × 每探针射线数 × 单射线成本
       + 降噪/后处理
```

- **提交预算**：场景里有多少灯（10k）。
- **每像素预算**：一个像素实际评估几盏（目标 8–32 盏无阴影 + 1–4 盏有阴影）。
- **射线预算**：每像素几条阴影/GI 射线（目标 1–4 条）。
- **内存预算**：灯记录、cluster 列表、reservoir、探针、阴影图集。

10k 灯在 1080p 上“每像素都遍历 10k”是 20 亿次评估/帧，不可行；必须把 10k 压到每像素几十。

### 1.3 目标与指标

| 指标 | 目标（建议） |
| --- | --- |
| 灯数 | 10,000 个条目；其中 100–500 个需要逐像素评估，其余靠 LOD/烘焙/淡出 |
| 分辨率 | 1080p（4K 作为后续） |
| 帧率 | 60 fps（16.6 ms），GPU 预算 ≤ 12 ms |
| 阴影 | 太阳 + 屏幕贡献前 K 的局部灯；其余用解析/烘焙 |
| GI | 至少 1 次弹射的漫反射；镜面反射可选 |
| 确定性 | 固定种子下可回放；截图 diff=0（允许降噪器内部时域） |
| 证据 | 每盏灯的贡献/成本、每 pass 的 GPU 时间、降级原因 |

---

## 2. 方案空间：八类选择

### 2.1 着色架构

| 方案 | 代表 | 多光源适配 | 代价 |
| --- | --- | --- | --- |
| Forward + | 经典前向 | 每物体绑定灯列表；透明友好 | 灯多时 per-object 列表/分支 |
| Deferred | G-buffer + 光照 pass | 每像素读 G-buffer，灯列表灵活 | 带宽大、MSAA/透明弱 |
| Clustered Forward | 3D cluster + 灯列表 | **10k 灯的默认解** | cluster 构建、每像素列表遍历 |
| Visibility Buffer + Deferred Material | Nanite 路线 | 几何极多时省带宽 | 需要 vis buffer/resolve |
| Ray-traced direct | RTXDI/ReSTIR | 阴影灯很多时最优 | 噪声、降噪、历史 |
| Path tracing | 参考/离线 | 真值 | 实时太慢 |

**判断**：本引擎短期用 **clustered forward + 少量 RT 阴影**；中期把“需要阴影的灯”交给 **ReSTIR DI**；长期如果几何走 visibility buffer，再切 deferred material。

### 2.2 光源剔除与分箱

| 方案 | 做法 | 适用 | 备注 |
| --- | --- | --- | --- |
| 2D tile | 屏幕 tile 存灯列表 | 灯少、均匀 | 深度方向不细分，易过度包含 |
| 3D cluster | 视空间 3D 网格，灯 AABB 分箱 | 10k 灯主力 | 需要 compute 分箱 |
| Light BVH | 对灯建 BVH，按视锥/贡献查询 | 极多灯、需要重要性 | 与 light tree 可复用 |
| Light tree | 按功率层次采样 | 随机采样/ReSTIR | 不是穷举列表 |
| Portal/房间 | 室内可见性 | 建筑室内 | 需要场景分区 |
| Light LOD | 屏幕立体角/贡献阈值 | 城市夜景 | 小灯淡出到烘焙项 |

### 2.3 直接光采样

| 方案 | 每像素采样 | 质量 | 成本 |
| --- | --- | --- | --- |
| 全遍历 | cluster 列表全部 | 无噪声 | 列表长时爆 |
| 截断 Top-K | 取最近/最亮 K 盏 | 有偏但可控 | 便宜 |
| RIS | 从候选分布采样 1 盏 | 无偏（带 MIS） | 需要 PDF/权重 |
| ReSTIR DI | 时域/空域复用 reservoir | 1 spp 可接近全遍历 | 需要历史/降噪 |
| Light tree + MIS | 按功率层次采样 | 大规模灯的最优采样 | 树构建/遍历 |
| Many-light cut | 层次聚类近似 | 离线/预览 | 实时更新难 |

### 2.4 阴影

| 方案 | 内存/成本 | 10k 灯可行性 |
| --- | --- | --- |
| 每灯 shadow map | 10k × 6 × 512² × 4B ≈ 63 GB | **不可行** |
| 阴影图集（静态灯） | 共享 atlas | 可行，需分配/更新 |
| Cube map（点光） | 6 面/灯 | 只适合少量 |
| Virtual Shadow Maps | 虚拟页 | 适合太阳/少量灯 |
| RT 阴影（每像素 1 射线） | TLAS | 10k 灯也只需“选中的那盏” |
| ReSTIR 阴影 | 1 射线/像素 + 复用 | **10k 阴影灯的主力** |
| 屏幕空间阴影 | 便宜 | 局限 |

### 2.5 间接光

| 方案 | 动态 | 质量 | 成本 | 适用 |
| --- | --- | --- | --- | --- |
| Lightmap | 否 | 高（静态） | 烘焙 | 静态场景 |
| Irradiance Volume | 部分 | 中 | 低 | 大世界/室内 |
| DDGI/RTXGI | 是 | 中高 | 中 | **Lumen-lite 的务实替代** |
| SSGI | 是 | 低中 | 低 | 屏幕内补光 |
| Lumen-like | 是 | 高 | 高 | 主目标 |
| Path tracing | 是 | 参考 | 极高 | 离线/真值 |

### 2.6 天空与 IBL

- **程序化天空**：解析梯度 + 太阳盘 + 大气近似；零资产，和 key light 方向一致。
- **IBL**：预过滤 cubemap / 球谐；天空盒渲染到 cubemap，或离线烘焙。
- **雾/空气透视**：和天空颜色一致，隐藏远景接缝。
- 本引擎目前**没有天空盒**，只有清屏色 + 雾；这是首先要补的。

### 2.7 时域、降噪、预算

- TAA/历史缓冲是 ReSTIR、DDGI、Lumen 的共同前提。
- 降噪：SVGF、BMFR、À-Trous、ReSTIR 时域复用；没有降噪就没有 1 spp。
- 预算调控器：把“灯数、射线数、探针更新数、降噪强度”统一到 GPU-ms 目标下。
- 确定性：固定种子、固定候选顺序、历史可重置；否则证据不可复现。

### 2.8 三种典型组合

| 组合 | 适用 | 能力 | 代价 |
| --- | --- | --- | --- |
| A. 静态烘焙 | 小场景/移动端 | lightmap/辐照度体积 + 阴影图 + 少量动态灯 | 灯不能动态增删；10k 灯只能烘 |
| B. 混合实时（推荐） | 本引擎主路径 | cluster 处理大量无阴影灯 + light tree/ReSTIR 处理阴影灯 + DDGI 处理 GI + 天空/IBL | 需要 light buffer/cluster/降噪/历史 |
| C. Lumen-like | 高质量独显 | 表面缓存 + 辐射缓存 + 屏幕追踪 + HWRT，接近 Lumen 的漫反射/镜面 | 内存/工期/复杂度最高；仍需自建 |

结论：**B 是 10k 灯的性价比拐点**；A 只能做小场景，C 是后续质量档。不要把 A 的“烘焙”当成 10k 灯方案，也不要把 C 当成第一版。

---

## 3. 推荐架构：分层混合

```text
场景灯表（10k，SoA）
   ↓  CPU 粗剔除（视锥/距离/层）
GPU light buffer（稳定 ID、静态/动态分桶）
   ↓  compute：灯 AABB → 3D cluster 分箱
Cluster 列表（每 cluster ≤ 64 盏）
   ↓
┌───────────────────────────── 直接光 ─────────────────────────────┐
│ Clustered forward：每像素 ≤ 32 盏无阴影解析光                      │
│ Light tree + ReSTIR DI：每像素 1 盏阴影灯 + 时域/空域复用          │
│ 太阳：RT 阴影 / CSM；局部重点灯：RT 阴影或阴影图集                 │
└──────────────────────────────────────────────────────────────────┘
   ↓
┌───────────────────────────── 间接光 ─────────────────────────────┐
│ DDGI 探针（32×16×32，1/8 轮转）→ 漫反射 GI                        │
│ 屏幕追踪（HZB）+ SDF/HWRT 回退 → 镜面/远场                         │
│ Lumen-like：表面缓存/辐射缓存（后续）                              │
└──────────────────────────────────────────────────────────────────┘
   ↓
天空/IBL + 雾
   ↓
降噪（TAA/À-Trous）+ 曝光 + ACES tone mapping
   ↓
Composite + 收据/时间戳/图像回归
```

### 3.1 为什么是混合

- **Clustered forward** 对“多但不需要阴影”的灯最便宜：每像素只遍历本 cluster 的几十盏，零噪声。
- **ReSTIR** 对“多且需要阴影”的灯最划算：每像素只追 1 条阴影射线，靠时域/空域复用收敛。
- **DDGI** 对“间接光”最划算：探针数量与灯数解耦，10k 灯不会让 GI 成本线性增长。
- **Light tree** 解决“候选太多、采样效率低”：按功率分层，比均匀随机少几个数量级的方差。
- **Lumen-like** 解决“高质量镜面 + 多次弹射”：DDGI 只有漫反射，镜面必须靠表面缓存/屏幕追踪。

### 3.2 Pass 列表（建议）

| Pass | 队列 | 输入 | 输出 | 说明 |
| --- | --- | --- | --- | --- |
| Light update | compute | CPU 灯表 | GpuLight[] | 静态灯只更新一次；动态灯 ring buffer |
| Light cull | compute | 相机、灯 BVH | 可见灯列表 | 视锥/距离/层/重要性 |
| Cluster bin | compute | 可见灯 AABB | cluster 列表 | 原子追加，超限截断并计数 |
| Shadow (sun) | RT/compute | depth/TLAS | sunMask | 现有 DXR pass |
| Direct | graphics | cluster 列表、灯表 | baseColor/keyLight | 解析光 + 重点灯 |
| ReSTIR DI | compute/RT | 候选灯、历史 | reservoir/阴影 | 1 射线/像素 |
| DDGI update | compute/RT | 探针、TLAS | 探针图集 | 1/8 轮转 |
| Screen trace | compute | HZB、vis buffer | 反射/GI | 后续 |
| Denoise | compute | 颜色/历史 | 稳定颜色 | TAA/À-Trous |
| Composite | graphics | base/key/GI | 交换链 | 曝光/tonemap |

### 3.3 完整系统还需要、但常被漏掉的层

| 层 | 方案 | 为什么 10k 灯场景必须考虑 |
| --- | --- | --- |
| 体积光/雾 | froxel volumetrics：把灯注入 3D 网格，沿视线积分 | 路灯多 + 雾 = 城市夜景的灵魂；逐灯 ray march 不可行 |
| 透明/半透明 | forward + per-object 灯列表 | deferred 不适合透明；clustered forward 可以复用 |
| 贴花/投影光 | decal/projected light 类型 | 招牌、车灯投影、窗影 |
| 灯光层/链接 | `layerMask`，渲染层与灯层匹配 | 避免室内灯照亮室外、角色灯照亮全场 |
| 曝光/tonemap | 手动/自动曝光 + ACES | 10k 灯下高光极易过曝；没有 tonemap 会“灯很亮但周围死黑” |
| 调试视图 | 灯复杂度热图、cluster 占用、阴影射线数、逐灯贡献 | 没有这些无法定位“为什么这盏灯没生效” |
| 验证 | 能量守恒、firefly clamp、参考路径追踪 | 多光源 + 时域最容易“看起来对但能量错” |

体积光建议单独排期：它和 10k 灯共享 light buffer/cluster 结构，但需要额外的 froxel 网格与积分 pass；不要把它塞进 ReSTIR。

已拆成独立并行方案：[VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md)。V0 冻结 froxel/密度/灯/阴影/合成接口；V1 大气雾、V2 太阳光轴、V5 诊断可并行；V3 局部灯散射等 LIGHTING P0/P1 的 light buffer/cluster；V4 时域收口；V6 把粒子烟并入统一密度；V7 接 10k 灯/Lumen。首个切片是“高度雾 + 太阳光轴”，约 2–3 人周。

---

## 4. 10,000 盏灯的具体设计

### 4.1 灯数据布局

```cpp
// 64 B，16 B 对齐；面积光/自发光三角形的扩展数据放独立数组，按 lightId 索引。
struct GpuLight {
    float3 position;   float radius;      // 点/聚光：位置与影响半径
    float3 color;      float intensity;   // 线性颜色与强度
    float3 direction;  float cosOuter;    // 聚光方向与外锥
    uint32_t type;     // 0 point, 1 spot, 2 rect, 3 emissive triangle
    uint32_t flags;    // shadow candidate, static, layers...
    uint32_t shadowSlot;  // 阴影图集/RT 记录索引，0xFFFFFFFF 表示无
    uint32_t layerMask;   // 灯光层/通道
};
```

- **SoA 变体**：位置、颜色、范围拆成独立 StructuredBuffer，便于 compute 只读需要的字段。
- **稳定 ID**：灯 ID 一旦分配不随排序变化；所有 pass、收据、回放都用同一个 ID。
- **静态/动态分桶**：静态灯可预计算贡献/阴影；动态灯单独 ring buffer，避免每帧全量上传。
- **面积光扩展**：rect light 的切线/副切线、emissive triangle 的三个顶点与法线，放 `LightExtra[]`，按需读取。

### 4.2 剔除与分箱

**CPU 粗剔除**：

- 对 10k 灯建 BVH（或均匀网格）；每帧视锥查询 + 距离/层过滤。
- 输出可见灯列表（通常几百到几千），写入 GPU buffer。
- 静态灯的结果可缓存；相机移动超过阈值再重建。

**GPU cluster 分箱**：

- 视空间 3D 网格：屏幕 tile **64×64**，深度 **24** 个对数切片。
- 每个 cluster 记录 `offset/count`，灯列表最大 64 盏；超限时按重要性截断并计数（不能静默丢）。
- 每盏灯计算视空间 AABB，遍历覆盖的 cluster，`InterlockedAdd` 追加。
- 分箱成本与“灯 AABB 覆盖的 cluster 数”成正比，不是 10k × 全部 cluster。

**重要性**：

```text
importance(l) ≈ power(l) × solidAngle(l) × falloff(distance) × shadowWeight
```

- 屏幕立体角小于阈值的小灯，淡出到“烘焙辐照度/环境项”，避免闪烁。
- 阴影候选只保留 importance 前 K 的灯（K 由预算决定）。

### 4.3 Light tree / light BVH

- 对 10k 灯建层次结构：叶=灯，内部节点=包围盒 + 总功率。
- 构建：按最长轴/SAH 分裂；动态灯每帧 refit，静态灯构建一次。
- 采样：从根按功率下降，直到叶；得到 PDF；配合 MIS。
- 用途：
  - ReSTIR 初始候选；
  - DDGI 探针 ray 的灯采样；
  - 面积光/自发光三角形的层次采样。
- 与 cluster 的分工：cluster 负责“局部穷举”，tree 负责“全局随机采样”；两者用 MIS 合并，避免重复计算。

### 4.4 ReSTIR DI（有阴影的 10k 灯）

```text
每像素：
  reservoir r = loadHistory(pixel)
  for i in 0..M-1:                     // M = 8..16
     light = sampleLightTree(pixel, rng)
     w = p̂(light) / pdf(light)
     update(r, light, w)
  if r.selected != none:
     vis = traceShadowRay(pixel, r.selected)
  r = temporalReuse(r, reproject(prev))  // 运动向量 + 校验
  r = spatialReuse(r, neighbors)         // 可选 3×3
  shade(r)
```

- 目标函数 `p̂ = Φ_l × f_r × G`；`Φ_l` 是灯功率，`G` 是几何项。
- 时域复用要 clamp（防历史拖影/鬼影）；disocclusion 重置。
- 1 条阴影射线/像素/帧；靠时域累积收敛。
- 降噪：ReSTIR 自带时域 + 空间滤波；再加 À-Trous/SVGF。
- 预算：`M`、射线数、邻居数由帧预算调控器动态调整。

### 4.5 阴影策略

| 灯 | 策略 |
| --- | --- |
| 太阳 | RT 硬阴影（现有）+ 时域抖动软阴影；或 CSM/VSM |
| 屏幕贡献前 K 的局部灯 | RT 阴影射线（每个 reservoir 1 条） |
| 静态重点灯 | 阴影图集（只更新一次） |
| 小灯/远灯 | 不投影，用解析衰减 + AO 近似 |
| 面积光/自发光 | 用 light tree 采样 + 可见性；不建逐灯阴影图 |

**不要做 10k 张阴影图**：内存和采样都不成立。阴影是“按需可见性”，不是“每灯一份资源”。

### 4.6 确定性与可回放

- 固定每像素/每帧 RNG 种子：`hash(pixel, frame, sampleIndex, lightTreeVersion)`。
- 固定候选顺序、固定 M、固定复用邻居；不依赖线程调度。
- 浮点权重量化到固定网格后再比较/累积（沿用现有 `Hasher` 思路）。
- 历史缓冲可显式重置；相机 cut、场景 revision 变化时清空。
- CI：同一输入两次运行 diff=0；不同 GPU 允许微小差异，但报告要注明。

### 4.7 性能模型（量级估算，需实测）

| 项 | 估算 | 说明 |
| --- | --- | --- |
| Light buffer 上传 | 0.05 ms | 10k × 64 B = 640 KB，动态灯增量 |
| CPU 粗剔除 | 0.1–0.3 ms | BVH 查询 |
| Cluster 分箱 | 0.3–0.6 ms | compute，10k 灯 |
| Clustered direct | 1–2 ms | 16–32 盏/像素 |
| 太阳 RT 阴影 | 0.2–0.4 ms | 1 射线/像素 |
| ReSTIR DI | 1.5–3 ms | 8 候选 + 1 射线 + 复用 |
| DDGI | 0.5–1 ms | 1/8 探针轮转 |
| 降噪 | 0.5–1 ms | À-Trous/TAA |
| 合计 | **4–8 ms** | 不含基础几何/材质 |

内存：

| 资源 | 估算 |
| --- | --- |
| 灯记录 | 640 KB |
| cluster 列表 | 2–8 MB |
| reservoir | 16–64 MB（半分辨率可减半） |
| DDGI 探针 | 8–32 MB |
| 阴影图集 | 16–64 MB |
| 表面缓存（Lumen-like） | 100–300 MB |

---

## 5. Lumen 技术路线

### 5.1 Lumen 是什么、不是什么

- 是 UE5 的**动态全局光照 + 反射**系统，不是单一算法，也不是可链接的库。
- 依赖：Surface Cache、Radiance Cache、Screen Probe、Screen Trace、SDF/HWRT、Final Gather、Denoiser、Temporal。
- 不能直接移植；只能按层重建，并接受“近似 Lumen”而不是“等于 Lumen”。

### 5.2 六层拆解与本引擎落点

| 层 | UE5 做法 | 本引擎落点 | 现状 |
| --- | --- | --- | --- |
| 1 屏幕追踪 | HZB 屏幕空间 ray march | HZB + visibility buffer | 未实现 |
| 2 表面缓存 | 每网格卡片，捕获材质，多帧更新 | 卡片图集 / cluster 辐射缓存 | 未实现 |
| 3 软件追踪 | 逐网格 SDF + 全局 SDF clipmap | 已有 SDF 烘焙，可扩展 clipmap | 部分 |
| 4 硬件追踪 | TLAS + fallback mesh | 已有 DXR 1.2/TLAS | 已有基础 |
| 5 时域/降噪 | 屏幕探针、辐照度缓存、TAA | 需新建历史缓冲/TAA | 未实现 |
| 6 远场 | HLOD + HWRT，约 1 km | 远景 SDF/HLOD | 未实现 |

### 5.3 与现有 L0–L4 的映射

[NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md) 的 L0–L4 是：

- L0 离线 SDF 烘焙
- L1 追踪（屏幕 + SDF）
- L2 探针/辐射缓存
- L3 降噪/最终采集
- L4 反射/远场

本文建议在 L1 之前插入 **L1.5 DDGI-lite**，作为“没有完整 Lumen 也能先有动态 GI”的中间档；并把 10k 灯的直接光作为独立 D 线，不与 GI 混在一起。

### 5.4 质量档位

| 档 | 追踪 | 弹射 | 探针 | 内存目标 | 目标硬件 |
| --- | --- | --- | --- | --- | --- |
| `gi-off` | 无 | 0 | 无 | 0 | 全部 |
| `gi-ao` | 1 射线 AO | 0 | 无 | 小 | 全部 |
| `gi-ddgi-lite` | DDGI 探针 | 1 | 32×16×32 | < 64 MB | 核显/低端 |
| `gi-ddgi` | DDGI + 屏幕追踪 | 1–2 | 更高密度 | < 128 MB | 独显 |
| `lumen-swrt` | 屏幕 + SDF | 2 | 屏幕 + 世界 | < 192 MB | 独显 |
| `lumen-hwrt` | 屏幕 + HWRT | 2–3 | 同上 + 击中照射 | 视 BVH | 独显 |

每档必须能截图对比、能回退、有预算数据；默认档位按适配器能力/显存选择，理由写进报告。

### 5.5 Lumen-like 管线

```text
1. Surface cache / cluster radiance cache
   每网格卡片（或每 cluster SH）捕获 albedo/normal/rough/emissive
   多帧分散更新；缺覆盖区域用诊断色标记

2. Probe update
   屏幕探针（1/16 分辨率）+ 世界探针
   每探针：屏幕追踪 → SDF/HWRT 回退 → 天空
   写辐照度/深度（八面体图集）

3. Final gather
   每像素取附近探针，按深度/法线权重混合
   加一次弹射的间接光；粗糙度决定探针/反射权重

4. Screen trace / reflection
   HZB march；命中取表面缓存颜色；未命中取天空/探针

5. Denoise + temporal
   TAA/历史缓冲；固定种子可重置

6. Composite
   直接光 + 间接光 + 反射；曝光/tonemap
```

### 5.6 与 10k 灯的结合

- **直接光**：clustered forward + ReSTIR DI，独立于 GI。
- **自发光**：材质 emissive 进入 light tree，窗户/霓虹作为面积光；不再需要“每盏灯一个实体”。
- **GI 的灯数**：DDGI 探针不需要遍历 10k 灯；它只追场景射线，命中点取表面缓存/辐照度。静态灯可以烘焙进探针，动态灯按重要性更新。
- **共享结构**：light tree 同时服务 ReSTIR 和 DDGI 的灯采样；cluster DAG 同时服务 Nanite、Lumen、cluster 辐射缓存。

### 5.7 成本与风险

| 项 | 估算/风险 |
| --- | --- |
| Surface cache 内存 | 100–300 MB；卡片分辨率/数量可调 |
| 探针更新 | 16k 探针 × 64 射线 / 8 帧 ≈ 128k 射线/帧 |
| 屏幕追踪 | 1–2 射线/像素；HZB 缺失会漏光 |
| SDF 薄壁 | <10 cm 漏光；用 cluster-native 近场或加厚 |
| 时域 | 拖影/鬼影；需要 clamp/重置 |
| 确定性 | TAA/ReSTIR 与 diff=0 冲突；需固定种子+历史重置 |

---

## 6. 创新点

> 按“对本引擎的性价比”排序；每项给机制、为什么新、验收、风险。

### I1. 确定性 reservoir 渲染（可回放 ReSTIR）

**主张**：ReSTIR 不必是“不可复现的噪声”；用固定种子、固定候选顺序、量化权重、可重置历史，做出**可回放**的 reservoir 渲染。

**机制**：
- 每像素/帧种子由 `hash(pixel, frame, sampleIndex, lightTreeVersion)` 生成。
- 候选灯顺序固定（按 light ID 排序），M 固定。
- reservoir 权重与累积量化到固定网格；比较/合并不依赖浮点尾数。
- 历史缓冲带 revision；相机 cut/场景变更清空。
- 收据记录种子、M、树版本、历史长度。

**为什么值得做**：本引擎的证据文化要求 diff=0；多数 ReSTIR 实现接受随机噪声，无法做严格回归。可回放 ReSTIR 是差异化能力，也让 AI 编辑可验证。

**验收**：同一输入两次运行 diff=0；关闭时域后与参考路径追踪的 relMSE 在阈值内；相机 cut 后无残影。

**风险**：确定性会限制某些优化（如随机邻居）；用固定模式换取可复现。

---

### I2. 逐灯收据与预算

**主张**：每盏灯都有“成本收据”：屏幕覆盖、评估次数、阴影射线数、GPU 时间分摊、被截断次数。预算求解器按收据决定 LOD/流送/降级。

**机制**：
- cluster 分箱时累计每灯覆盖的 cluster/像素数。
- ReSTIR 记录每灯被选中的次数/权重。
- 时间戳按 pass 分摊；用保守估算给每灯记账。
- 报告输出 top-N 昂贵灯、被截断灯、可降级灯。

**为什么值得做**：现有引擎的预算是“整帧”级；灯级收据让“这盏灯值不值”可回答，也直接支撑 AI 灯光编辑。

**验收**：10k 灯场景输出每灯成本表；删除/降低昂贵灯后预算可预测；截断有稳定诊断码。

**风险**：记账本身有开销；采样估算，不追求精确。

---

### I3. Cluster + light tree 混合重要性采样

**主张**：cluster 负责“局部穷举”，light tree 负责“全局随机”，两者用 MIS 合并；不为多光源再建一套独立结构。

**机制**：
- cluster 列表提供局部候选与 PDF；
- light tree 提供全局候选与 PDF；
- MIS 权重按两者 PDF 组合；
- 同一棵 light tree 也服务 DDGI 探针采样。

**为什么值得做**：纯 cluster 在灯极多时列表截断；纯 light tree 在局部高密度时采样效率低。混合能同时覆盖“街边 50 盏路灯”和“全城 10k 灯”。

**验收**：与全遍历参考的 relMSE；截断率；每像素候选数。

---

### I4. 自发光优先的光源模型（Emissive-first）

**主张**：窗户、霓虹、屏幕不是“额外灯实体”，而是材质的 emissive 直接进入 light tree/面积光采样；10k 个窗户就是 10k 个面积光，无需 10k 个灯对象。

**机制**：
- 材质 emissive 通道（已有 ORM.a）作为光源功率；
- 离线/加载时把发光三角形加入 light tree；
- ReSTIR/DDGI 直接采样发光三角形；
- 编辑器只调材质，不调灯。

**为什么值得做**：小镇/城市夜景的光源绝大多数是“发光表面”，不是点光。把材质和灯统一，能大幅减少手工灯实体，也让 AI 编辑“把这扇窗调暖”直接生效。

**验收**：10k 发光窗户场景，无手工灯实体；直接光/ GI 能量守恒；开关 emissive 只重建 light tree 的相关子树。

---

### I5. Cluster 辐射缓存（低端档）

**主张**：用 cluster DAG 的每个 cluster 存 SH2/少量方向样本，替代表面缓存图集；内存 O(cluster 数)，适合核显。

**机制**：
- 每个 cluster 存辐照度/朝向；按可见性/变化率轮转更新；
- 命中点按 cluster 取辐照度；
- 高频镜面用 HWRT 补。

**为什么值得做**：这是 [NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md) 已有的 C1，和 10k 灯结合后更重要：灯多时表面缓存图集内存爆炸，cluster 缓存是可行替代。

**验收**：内存峰值；GI relMSE；光照变化到收敛帧数。

---

### I6. 光源 LOD 与流送

**主张**：城市级 10k 灯按 cell 流送；远处/小灯淡出到烘焙辐照度，近处/重要灯保留实时；切换确定性、无 popping。

**机制**：
- 灯按空间 cell 分组；相机移动加载/卸载；
- 屏幕立体角/贡献阈值决定 LOD；
- 淡出用能量守恒的“剩余项”加到环境/探针；
- 切换记录 revision，回放一致。

**为什么值得做**：10k 灯不可能全部常驻；流送是城市规模的前提。

**验收**：内存上限；切换帧无可见跳变；回放 diff=0。

---

### I7. 类型化 AI 灯光作者化

**主张**：自然语言 → 类型化灯光 patch（位置/颜色/强度/类型/层），引擎校验、预览、比较后提交；不让模型直接写 shader。

**机制**：
- LLM 通过 MCP 读场景灯表、材质、预算；
- 输出 `LightPatch`（增删改、约束、理由）；
- 引擎做范围/预算/层校验，渲染预览，图像对比；
- 成功才提交事务，写收据。

**为什么值得做**：这是“AI 原生引擎”的差异点；灯光比材质更适合语言描述（“巷口加一盏暖灯”）。

**验收**：一条自然语言生成可解释的灯 patch；预览=提交；撤销恢复；预算不超。

---

### I8. 逆光照（Inverse Lighting）

**主张**：给一张参考照片/目标氛围，反解灯的位置、颜色、强度，而不是手工调。

**机制**：
- 低维参数化（灯位、强度、色温、层）；
- 可微渲染或导数无关优化；
- 先验：灯类型/场景语义/预算；
- 输出 patch + 残差 + 置信度。

**为什么值得做**：与材质逆渲染同一套思路；对“把小镇调成黄昏”这类需求非常直接。

**验收**：合成数据恢复已知灯参；真实照片残差可解释；结果可进入预算求解器。

---

### I9. 程序化天空 + 太阳模型

**主张**：不依赖贴图，解析天空 + 太阳盘 + 大气近似；key light 方向与太阳一致，IBL 从同一模型采样。

**机制**：
- 全屏天空 pass（逆视图投影）；
- 太阳盘 + 米氏/瑞利近似；
- 预过滤 cubemap 或球谐；
- 雾色与天空一致。

**为什么值得做**：目前没有天空盒；程序化天空零资产、可复现、和光照一致。

**验收**：固定机位截图；太阳方向与阴影一致；IBL 能量与太阳照度可对账。

---

### I10. 面光源 + LTC + 多光源采样

**主张**：窗户/霓虹/广告牌用 LTC 面光源，进入 light tree；ReSTIR 采样面积光，而不是近似点光。

**机制**：
- LTC 矩阵预计算；
- 面积光采样 + MIS；
- 与 emissive 材质统一。

**为什么值得做**：城市夜景的主要光源是面；点光近似会让高光/软阴影不对。

**验收**：面光 vs 点光参考图差异；能量守恒；软阴影质量。

---

### I11. GPU 工作图与预算写入

**主张**：用 `ExecuteIndirect` 串起灯剔除、cluster 分箱、ReSTIR 派发、DDGI 探针派发；预算调控器直接改写间接参数，无需 CPU 回读。

**机制**：
- GPU 计数器 + 间接参数；
- 预算器写 max lights/rays/probes；
- 超预算降档并记录。

**为什么值得做**：10k 灯下 CPU 提交会成为瓶颈；GPU 工作图是规模化的前提。

**验收**：CPU 提交不随灯数增长；间接参数 dump；降档可回放。

---

### I12. 光照证据与回归门禁

**主张**：每帧输出灯数、候选数、截断数、射线数、每 pass 时间、每灯 top 成本；CI 用固定种子 + 图像 diff。

**机制**：
- 时间戳分 pass；
- JSON 收据；
- 固定机位/种子/历史重置；
- SSIM/ΔE 阈值。

**为什么值得做**：这是本引擎最大的现成优势；多光源系统没有证据就会变成“看起来对”。

**验收**：超预算帧可定位到 pass/灯；两次运行 diff=0；降级原因可读。

---

### 6.1 优先级矩阵

| 项 | 影响 | 工作量 | 风险 | 建议 |
| --- | --- | --- | --- | --- |
| I9 天空/太阳 | 高 | 低 | 低 | P0 |
| 灯 buffer + cluster | 极高 | 中 | 低 | P0 |
| I2 逐灯收据 | 高 | 中 | 低 | P0 |
| I3 cluster+tree | 高 | 中高 | 中 | P1 |
| I1 确定性 ReSTIR | 极高 | 高 | 中 | P1 |
| I10 面光源 | 高 | 中 | 中 | P1 |
| I5 cluster 辐射缓存 | 中高 | 中 | 中 | P2 |
| I6 灯 LOD/流送 | 高 | 中 | 中 | P2 |
| I4 emissive-first | 高 | 中 | 低 | P1 |
| I7 AI 灯光 | 极高 | 高 | 中 | P3 |
| I8 逆光照 | 高 | 高 | 高 | P3 |
| I11 GPU 工作图 | 高 | 高 | 中 | P2 |
| I12 证据门禁 | 极高 | 低中 | 低 | P0 |

---

## 7. 分阶段路线图

| 阶段 | 目标 | 交付 | 门禁 | 粗估 |
| --- | --- | --- | --- | --- |
| P0 天空与灯表 | 有天空、有 10k 灯表、有 cluster 分箱 | 程序化天空/IBL、GpuLight、cluster pass、调试视图 | 10k 灯 1080p ≥60fps；天空/阴影方向一致；灯表无静默截断 | 2–3 周 |
| P1 直接光与阴影 | 无阴影解析光 + 太阳/重点灯阴影 | clustered direct、RT 重点灯阴影、灯 LOD | 每像素 ≤32 灯；阴影预算可调；无穿墙 | 3–4 周 |
| P1.5 体积雾/光（并行） | 大气雾 + 太阳光轴；局部灯散射预留 | V0 froxel 底座、V1 雾、V2 光轴、V5 诊断 | 透射率/光轴/成本门限；不阻塞 10k 灯 | 3–5 周（并行） |
| P2 ReSTIR DI | 10k 阴影灯可实时 | light tree、确定性 ReSTIR、降噪、收据 | 1 射线/像素；diff=0；relMSE 达标 | 4–6 周 |
| P3 DDGI | 动态漫反射 GI | 探针网格、轮转更新、屏幕采样 | 1 次弹射；内存 ≤64 MB；无严重漏光 | 3–5 周 |
| P4 Lumen-like | 屏幕追踪 + 表面缓存 + 反射 | HZB、vis buffer、surface cache、final gather | 2 次弹射；反射质量档位；可回退 | 8–12 周 |
| P5 AI/逆光照 | 语言与照片驱动灯光 | LightPatch、逆光照、验证闭环 | 预览=提交；误差可解释 | 研究 |

**首个切片建议**：P0 + P1 的“太阳 + 4 盏重点灯 + cluster 10k 无阴影灯”，先把 10k 灯跑起来，再上 ReSTIR。不要一上来做 Lumen。体积雾/光按 [VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md) 作为并行线，V0–V2 与 P0/P1 同时开工，不占 10k 灯关键路径。

---

## 8. 反模式与风险

1. **10k 张阴影图**：内存/采样都不成立；阴影是可见性，不是每灯资源。
2. **每像素遍历全部灯**：必须 cluster/tree 压到几十。
3. **没有 TAA/历史就上 ReSTIR**：1 spp 噪声不可用。
4. **没有降噪就上 DDGI**：探针噪声会闪。
5. **Lumen 一步到位**：先把 DDGI-lite 跑通，再谈表面缓存。
6. **灯 ID 不稳定**：收据/回放/缓存全失效。
7. **静默截断**：cluster 列表超限必须计数+诊断，不能悄悄丢灯。
8. **CPU 每帧更新 10k 灯**：用增量/GPU buffer。
9. **只做白天**：夜景才是 10k 灯的主场，天空/曝光要一起做。
10. **确定性 vs 时域**：先决定是否要求 diff=0；这会影响 ReSTIR/TAA 设计。

---

## 9. 开放问题

1. 10k 灯里有多少是动态的？有多少需要阴影？
2. 目标是夜景城市、室内、还是白天小镇？
3. 是否要求跨机器确定性，还是同机 diff=0 即可？
4. 显存上限（8 GB / 4 GB / 核显共享）？
5. 是否接受 1–2 帧的 GI 延迟？
6. Lumen-like 的目标是“观感接近”还是“物理可比”？

---

## 10. 参考

**本仓库**

- [NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md)：Lumen L0–L4、共享底座、创新清单。
- [RT-SHADOWS.md](RT-SHADOWS.md)：当前单射线 key 阴影的事实与限制。
- [GEOMETRY-LAYOUT.md](GEOMETRY-LAYOUT.md)：cluster/instance/材质句柄契约。
- [MATERIAL-PIPELINE.md](MATERIAL-PIPELINE.md)：材质/自发光/纹理管线。
- [MATERIAL-PIPELINE-RESEARCH.md](MATERIAL-PIPELINE-RESEARCH.md)：材质侧方案与创新点。

**外部**

- Epic Games，*Lumen Technical Details*（UE 5.x 官方文档）：<https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine>
- Bitterli et al., *Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting*（ReSTIR DI, SIGGRAPH 2020）
- Ouyang et al., *ReSTIR GI: Path Resampling for Real-Time Path Tracing*（2021）
- Majercik et al., *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*（DDGI, JCGT 2019）：<https://jcgt.org/published/0008/02/01/>
- Walter et al., *Lightcuts: A Scalable Approach to Illumination*（SIGGRAPH 2005）
- Conty Estevez & Kulla, *Importance Sampling of Many Lights with Adaptive Tree Splitting*（HPG 2018）
- Olsson et al., *Clustered Deferred and Forward Shading*（HPG 2012）
- NVIDIA, *RTXDI*（ReSTIR 的工业实现与文档）
- UE5 *Virtual Shadow Maps* 文档（作为阴影图集/虚拟页的对照）

---

## 附：一句话回答

**10k 灯的实时方案不是“更强的阴影”，而是“剔除 → 分箱 → 采样 → 可见性”四级预算**：cluster 解决“每像素看哪些”，light tree/ReSTIR 解决“每像素算哪几盏、追哪条射线”，DDGI 解决“不用逐灯算的间接光”，天空/IBL 解决“环境光”。**Lumen 不能直接用，但可以按“DDGI-lite → 屏幕追踪 → 表面缓存/辐射缓存 → HWRT”分档重建**；本引擎最值得的创新是**确定性 reservoir、逐灯收据、cluster+tree 混合、emissive-first 光源**。
