# 场与粒子系统技术方案（v0.2：开源复用 + 确定性创新）

> 2026-10-22：完整生产化方案见 [粒子、共享场、布料与水](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md)。新方案沿用本文的共享空间数据底座、LEF/WF、租约与可观察性设计，并明确贡献所有权、按需分配、CPU 权威数据来源和可验收的一致性合同；本文的历史状态、性能估算与接口示例不代表新增功能已经实现。

> 同日增量：区域租约、LEF/WF 来源投影、原子多通道发布与观察状态机已有独立 [CPU 参考及专项](FIELD-REFERENCE.md)。GPU 稀疏场、覆盖遮罩、求解权接管及宿主适配仍未实现，不能用下方历史 smoke/fire 切片代替这些目标。

- 版本：草案 v0.2（F1–F3 首个切片已实现，见下）
- 日期：2026-10-21
- 基线：**NativeGPU 0.10.0**（D3D12、Mesh Shader、DXR 静态阴影、compute 队列、SM 6.6 堆索引、RHI、帧预算管理器、回放/黑匣子、图像回归、确定性契约、Web 编辑器）
- 目标硬件：RTX 5060（8 GB，主目标）；Radeon 890M（低端回退验证）
- 状态：待评审。不改变 `docs/NEXT-PHASE.md` 的顺序，也不改变 `docs/EDITOR.md` 的"Web 编辑器 + 原生引擎"路线
- 前置文档：[NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md)（共享图形底座）、[ANIMATION-PLAN.md](ANIMATION-PLAN.md)（"显式声明数据访问"的同一套纪律）、[BUDGET.md](BUDGET.md)、[IMAGE-REGRESSION.md](IMAGE-REGRESSION.md)、[DETERMINISM.md](DETERMINISM.md)、[EDITOR.md](EDITOR.md)
- 定位：**不是 VFX 工具，是渲染 / 仿真 / 玩法共享的空间真相层**

## 实现状态（2026-10-21）

首个切片已落地：F1 的运行时核心 + F2 的粒子核心 + F3 的求解器、涡量约束与体积渲染，外加一个可验证的篝火场景。

| 项 | 状态 | 位置 |
|---|---|---|
| GPU 粒子池（SoA，65536 × 32 B） | ✅ | `renderer/particles.cpp` |
| compute 模拟 + **整数原子** splat | ✅ | `shaders/particles.hlsl` `CSSimulate` |
| **场景 AABB 碰撞**（最浅轴推出 + 阻尼反弹） | ✅ | `CSSimulate` + `setFieldColliders` |
| 整数体积 → RGBA16F 解析 | ✅ | `CSResolve` |
| 96³ 体积步进（自阴影、热辐射、深度裁剪、边界淡出） | ✅ | `PSVolume` |
| **浮力 + 平流 + 散度 + Jacobi 压力投影** | ✅ | `shaders/fluid.hlsl` |
| **涡量约束**（curl + N×ω confinement） | ✅ | `CSCurl` / `CSVorticity` |
| **篝火预设 + 爆炸冲击**（源放大 + 速度场径向 blast） | ✅ | `--fire` / `--explode-at` |
| 报告字段 `probes.particles` | ✅ | `particlesJson()` |
| 回退开关 `--particles`、`--particle-colliders`、`--particle-emitter` | ✅ | `host/main.cpp` |
| 证据脚本（粒子 21 项、篝火 17 项） | ✅ | `.incrementalTesting/simulation/particles.ps1`、`.incrementalTesting/rendering/campfire.ps1` |
| **通道租约 + 一致性等级**（引用计数，缺租约即跳过求解） | ✅ | `acquireFieldLease` / `fieldLeasesJson` |
| **爆炸 → 场景焦化**（宿主轮询 `particleBurst()`） | ✅ | `host/main.cpp` |
| 提升/降级（LEF→WF 迁移） | ❌ | 本方案 F4 余下部分 |
| 与刚体的双向耦合 | ❌ | 未排期 |
| 烟/火对参考渲染器的 relMSE | ❌ | 本方案 F5 |

**口径**：splat 是确定性的（整数原子），求解器是单精度、**不承诺确定性**。求解器有浮力、平流、涡量约束、散度、压力投影和场景 AABB 碰撞，但不与刚体双向耦合——它是"能立住的流体主干"，不是完整的燃烧化学模型。租约目前是"单层场"的引用计数与一致性声明，尚未实现两级场之间的数据迁移。

---

## 0. 结论摘要

1. **场不是求解器集合，是数据底座**。它回答"空间某点的某个连续量是多少"，仅此而已。
2. **两级场**：局部效果场（LEF，私有、便宜、可丢弃）+ 世界场（WF，全局、稀疏、多消费者共享），按"是否被观察到"提升/降级。
3. **通道租约（lease）** 取代全局静态通道表：没有消费者就没有内存。
4. **粒子直接渲染为主**，场只负责体积渲染与空间查询；渲染器做成插件。
5. **确定性只给玩法通道**：整数原子加实现顺序无关的 splat；视觉通道仍用浮点。
6. **最大化复用**：外部用 OpenVDB / Jolt / Steam Audio / cgltf / FastNoise2，内部复用 RHI、预算阶梯、图像回归、回放、确定性契约、SDF 烘焙、查询 DSL。
7. **五个自研创新**：整数原子确定性沉积、可观察性驱动的提升/降级、跨域一致性等级、热-声耦合、代理在环调参。
8. **不替代 Niagara/Effekseer 这类 VFX 工具**，只做它们读得到的那一层。

---

## 1. 背景与边界

### 1.1 要解决的问题

今天引擎里，"空间中的连续量"被六套互不相干的数据重复维护：

| 系统 | 自己维护的数据 | 问题 |
|---|---|---|
| 渲染 | 体积雾、粒子 | 与玩法无关 |
| 破坏 | 碎块、遮挡 | 不通知导航/音频 |
| 音频 | 遮挡体、portal | 单独一套几何 |
| 光照 | 探针、SDF | 不感知破坏 |
| 导航 | navmesh、代价场 | 不感知烟雾/水 |
| AI | 视线、掩体、威胁 | 各自计算 |

**目标是让它们读同一份数据**，而不是"用一个求解器替代六个系统"。

### 1.2 明确的边界

| 属于场 | 不属于场 |
|---|---|
| 密度、温度、速度、遮挡、可通行成本、辐照度、湿度、威胁 | 门、触发器、道具、NPC、任务 |
| 连续量的空间采样与梯度 | 事件、状态机、离散逻辑 |
| 多系统共享的介质 | 各系统私有的算法结构 |

**导航仍需要 navmesh，声学仍需要几何声学/portal，光照仍需要 GI 求解器。** 场给它们输入，不替代它们。

---

## 2. 现状核对（0.10.0 实测，不是假设）

### 2.1 已经具备、可以直接复用

| 能力 | 位置 | 对本方案的作用 |
|---|---|---|
| **Compute 队列（B1）** | `renderer/compute_pass.cpp`、`shaders/compute_probe.hlsl` | 场求解、粒子平流、排序的宿主，**不用重做** |
| **SM 6.6 堆索引（B3）** | `ResourceDescriptorHeap[32..35]` | brick 页表与大堆描述符可走堆索引 |
| **RHI 抽象** | `renderer/rhi.hpp`、`rhi_d3d12.cpp` | 场代码走接口，不直接摸 D3D12 |
| **帧预算管理器** | `host/budget.*`、[BUDGET.md](BUDGET.md) | 场的降档阶梯接进去，**不新写预算系统** |
| **图像回归门禁** | [IMAGE-REGRESSION.md](IMAGE-REGRESSION.md) | 效果截图回归直接复用 SSIM/ΔE |
| **回放 / 黑匣子** | [REPLAY.md](REPLAY.md) `--record/--replay` | 效果参数与随机种子进回放 |
| **确定性契约** | [DETERMINISM.md](DETERMINISM.md) `--hash-log/--seed` | 玩法通道哈希接进去 |
| **SDF 烘焙** | `tools/offline/sdf-bake.mjs` | 粒子碰撞直接用现成 SDF |
| **glTF 子集导入** | `tools/offline/gltf.mjs` | 发射器网格、碰撞体来源 |
| **几何包 v2** | `tools/offline/geometry-package.mjs` | 发射器/遮挡体元数据 |
| **查询 DSL** | `engine/scene/query.mjs` | 玩法查询的对外表达面 |
| **Web 编辑器** | [EDITOR.md](EDITOR.md) | 效果参数编辑的落点 |
| **MCP 工具面** | `ai/mcp-server.mjs` | 代理调参与截图对比 |

### 2.2 仍然缺的

| 缺口 | 严重度 | 本方案阶段 |
|---|---|---|
| `ExecuteIndirect` | 🔴 粒子数由 GPU 决定，必须 | F0 |
| 分帧异步回读（现在只有一次性 probe 回读） | 🔴 玩法查询需要低延迟 | F0 |
| 三维纹理 / 体积渲染路径 | 🔴 体积步进的前提 | F0/F1 |
| GPU 粒子管线 | 🔴 目前只有刚体方盒 | F2 |
| compute 与图形**重叠**执行（现在只有跨队列等待） | 🟠 影响吞吐 | F4 |

**注意**：`ExecuteIndirect` 与异步回读都不在现有代码里，F0 必须补齐，不能假设。

---

## 3. 设计原则

1. **数据共享优先于算法统一**。不追求"一个求解器解所有"，追求"一份数据被所有系统读"。
2. **没有消费者就没有内存**。一切场资源通过租约获取，引用计数归零即释放。
3. **小效果不进世界场**。局部细节留在 LEF，只有"能被别人观察到"的量才提升。
4. **确定性分层**。玩法关键走 CPU 粗场；视觉走 GPU，不承诺确定性。
5. **显式数据访问声明**。每个 solver/消费者声明读哪些通道、写哪些通道，越界即报错（与 `ANIMATION-PLAN.md` 的 Trait 掩码同一纪律）。
6. **开源与既有设施优先**。能复用的绝不重写；自研只投在"没有现成方案且是差异化"的地方。
7. **失败要显式**。缺通道、超额、降级必须写进报告，沿用 `probeAvailable / effectImplemented / fallbackUsed` 的分字段约定。

---

## 4. 架构

### 4.1 三层结构

```text
┌─ 源 ────────────────────────────────────────────────────────┐
│ 发射器 / 灯光 / 破碎 / 水 / 风 / 音频 / 玩家 / 天气           │
└───────────────────────┬─────────────────────────────────────┘
                        ▼
┌─ L1 局部效果场 LEF ─────────────────────────────────────────┐
│ 每个效果私有 · 64³ @ 12.5 cm · GPU · 可丢弃 · 无跨系统职责    │
│ 粒子在这里 splat；渲染优先读这里                              │
└───────────────────────┬─────────────────────────────────────┘
                        │ 提升 / 降级（按可观察性）
                        ▼
┌─ L2 世界场 WF ──────────────────────────────────────────────┐
│ 全局稀疏 · 1 m 粗 · 持久 · 多消费者共享 · 多分辨率层级         │
└───────────────────────┬─────────────────────────────────────┘
                        │ 查询
                        ▼
┌─ L3 消费者 ─────────────────────────────────────────────────┐
│ 体积渲染 / AI 视线 / 潜行 / 导航 / 声遮挡 / 光照探针          │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 为什么分两级

| 问题 | 单级场 | 两级场 |
|---|---|---|
| 小效果开销 | 一律分配 brick | 只在 LEF，几 MB |
| 世界场分辨率 | 被细细节拖高 | 可以一直保持粗 |
| 内存 | 线性膨胀 | 有界 |
| 细节质量 | 受限于全局分辨率 | 局部可以很细 |

---

## 5. 数据模型

### 5.1 分页稀疏体素

```cpp
constexpr int BrickDim    = 16;                      // 16³ = 4096 voxel
constexpr int BrickVoxels = BrickDim * BrickDim * BrickDim;

struct Brick {
    uint32_t version;      // 每次写入自增，供消费者判断新鲜度
    uint32_t dirtyMask;    // 哪些通道被改过
    uint8_t  lastWriter;   // 仅 debug：最后写它的 solver id
    uint16_t dataOffset;   // 通道池偏移
    uint8_t  lod;          // 0 细 / 1 中 / 2 粗
    uint8_t  flags;        // 提升中 / 降级中 / 只读
};

struct FieldLevel {
    std::unordered_map<uint64_t, uint32_t> brickIndex;  // 砖坐标 → 槽位
    std::vector<Brick> bricks;
    ChannelPool pool;                                   // SoA 大缓冲
    std::vector<uint32_t> freeList;
};
```

**用 brick 而非纯哈希**：brick 让 16³ 的局部数据连续，采样缓存友好；纯哈希每 voxel 一次查找，带宽爆炸。

### 5.2 通道租约

```cpp
struct FieldLease {
    Region    region;
    ChannelMask channels;
    Level     level;        // Local / World
    Rate      rate;         // PerFrame / 10Hz / 2Hz / OnDemand
    Budget    budget;       // maxBricks, maxMs
    uint32_t  refcount;
};

FieldLease acquire(Region, ChannelMask, Level, Rate, Budget);
void       release(FieldLease);
```

**通道不是全局静态表，而是按需租用。** 没有租约的通道不分配内存，也不参与调度。

### 5.3 通道定义

| 通道 | 类型 | 层级 | 写入者 | 读取者 | 确定性 |
|---|---|---|---|---|---|
| `density` | f16 | LEF, WF | 烟/尘粒子 | 渲染、声学 | 否 |
| `temperature` | f16 | LEF, WF | 火 | 浮力、渲染、**声速** | 否 |
| `velocity` | 3×f16 | LEF, WF | 流体求解 | 粒子平流 | 否 |
| `occlusion` | u8 | WF | 几何/破碎 | 声学、AI 视线 | **是** |
| `navCost` | u8 | WF | 破碎/水 | 导航 | **是** |
| `threat` | u8 | WF | 玩家/灯光/声音 | AI | **是** |
| `irradiance` | SH9 f16 | WF | GI | 渲染 | 否 |
| `moisture` | u8 | WF | 水/雨 | 材质 | 否 |
| `provenance` | u8 | 全 | debug | 调试可视化 | — |

**确定性通道（u8 整数）走 CPU 粗场，见第 8 节。**

### 5.4 粒子池

```cpp
struct AttributeSchema {
    struct Attr { const char* name; AttrType type; uint8_t components; };
    Attr     attrs[32];
    uint32_t stride;
};

struct ParticlePool {
    uint8_t* data;              // SoA，按 stride 排列
    uint32_t capacity;
    uint32_t alive;
    uint32_t freeStack[];
};
```

**不写死 `payload[4]`**。每个效果声明自己的属性集（温度、质量、电荷、旋转、粘稠度……），互不干扰。

---

## 6. 外部开源组件选型

> 许可证以各仓库 `LICENSE` 为准；集成前逐一核对，并按 DXC 冻结的同一做法登记到 [TOOLCHAIN.md](TOOLCHAIN.md)（版本 + SHA-256）。

### 6.1 直接复用

| 用途 | 组件 | 许可证 | 替代方案 |
|---|---|---|---|
| 稀疏体数据（**离线**烘焙/交换） | **OpenVDB / NanoVDB** | MPL-2.0 | 运行时仍需自研 brick |
| glTF 资产导入 | **cgltf** | MIT | tinygltf（MIT） |
| 刚体 / 载具 / 破坏碎片 | **Jolt Physics** | MIT | PhysX 5（BSD-3） |
| 声学（作为消费者） | **Steam Audio** | Apache-2.0（需核对） | Project Acoustics（MIT） |
| 噪声 / 湍流 | **FastNoise2** | MIT | 自研 Perlin |
| JSON 解析 | **nlohmann/json** / **simdjson** | MIT / Apache-2.0 | 自研 |
| GPU 显存分配 | **D3D12MA** | MIT | 自研（不划算） |
| 性能时间线 | **Tracy** | BSD-3 | PIX（本机不可用，见 [CAPTURE.md](CAPTURE.md)） |
| 离线参考降噪 | **OpenImageDenoise** | Apache-2.0 | 无 |
| 离线求解原型 | **Taichi** | Apache-2.0 | 直接写 HLSL |
| 效果编辑器参考 | **Effekseer** | MIT | 无（仅参考数据模型） |

### 6.2 内部已有，直接用

见 2.1 的表格。**尤其注意三条**：

- **预算管理器**：场不新写预算系统，而是在现有阶梯上追加 `field-lite` / `field-off` 两档。
- **图像回归**：效果截图回归直接进 `tests/image-contracts.json`，不另建一套。
- **查询 DSL**：场的玩法查询通过 `scene query` 暴露，AI 与测试共用同一个入口。

### 6.3 自研（有差异化价值）

| 模块 | 为什么必须自研 |
|---|---|
| 运行时 bricked 稀疏场 | 需要 D3D12/RHI 绑定、租约、提升/降级；OpenVDB 是 CPU 库 |
| 通道租约与调度器 | 没有现成方案 |
| 确定性整数原子沉积 | **没有现成方案，这是核心创新** |
| 提升/降级与一致性等级 | 没有现成方案 |
| 粒子-场耦合管线 | 引擎相关，开源方案都不暴露这一层 |
| 热-声耦合 | 没有现成方案 |

### 6.4 决策规则

```text
能开源拿到的，绝不重写（cgltf、Jolt、FastNoise2、D3D12MA、Tracy）
项目里已有的，绝不重写（RHI、预算、图像回归、回放、确定性、SDF、查询 DSL）
引擎强耦合的，必须自研（场核心、粒子管线、调度）
构成差异化的，优先自研（确定性、提升/降级、一致性、热-声耦合）
```

---

## 7. 确定性设计

### 7.1 问题

GPU splat 用浮点原子加，而**浮点加法不满足结合律**：`(a+b)+c ≠ a+(b+c)`。顺序不同，结果不同，跨运行不可复现。现有引擎都接受这个事实。

### 7.2 方案：整数原子加

```text
contribution → 量化到 Q16.16 / scaled int32 → atomicAdd(int) → 反量化
```

整数加法是结合的、交换的，**结果与执行顺序无关**，因此天然确定。

```hlsl
int q = (int)round(contribution * 65536.0);
InterlockedAdd(densityUav[voxelIdx], q);
```

### 7.3 代价与对策

| 问题 | 对策 |
|---|---|
| 动态范围有限 | per-brick 指数（类似浮点指数位） |
| 量化误差 | 只在玩法通道使用；视觉通道仍用浮点 |
| 溢出 | 饱和加法 + 溢出计数写入报告 |
| 性能 | 整数原子吞吐通常高于浮点原子，需实测（见 F5 前置实验） |

### 7.4 两级确定性

| 层 | 更新者 | 确定性 | 用途 |
|---|---|---|---|
| LEF | GPU | 否 | 纯视觉 |
| WF 视觉通道 | GPU | 否 | 渲染 |
| **WF 玩法通道** | **CPU 粗场** | **是** | AI 视线、潜行、导航、声遮挡 |

玩法通道是独立的 `128×32×128` CPU 网格，由 WF 汇总而来，固定步长、固定顺序。**GPU 随便乱序，也不污染回放。**

### 7.5 与既有确定性契约的对接

- 玩法通道哈希接入 `Game::digest` 的既有链条；
- `--hash-log` 输出逐帧哈希，`--seed` 固定随机流；
- 回放（`--record/--replay`）记录效果参数与发射器种子。

### 7.6 验收

- 同输入跑 100 次，玩法通道哈希完全一致；
- 整数原子 vs 浮点原子：一致率、耗时、相对误差，写入报告；
- 视觉通道允许不一致，但必须在报告中标注。

---

## 8. 提升 / 降级与一致性

### 8.1 提升条件（任一）

- LEF brick 的 `density > worldThreshold`；
- 有消费者订阅了该区域（AI 视线、导航、声学）；
- 效果被标记 `worldVisible`（火、大烟柱、毒气）。

### 8.2 降级条件

- 低于阈值持续 N 帧；
- 区域离开所有订阅者范围；
- 全局预算不足时，按优先级淘汰。

### 8.3 一致性等级

| 等级 | 含义 | 典型消费者 |
|---|---|---|
| `Strong` | 当帧必须一致 | 玩家可见的碰撞判定 |
| `Eventual` | N 帧内收敛 | AI 视线、声遮挡 |
| `Stale` | 允许滞后，只用于表现 | 光照探针 |

每个消费者在租约里声明需要的等级与预算；调度器按等级分配更新顺序。

### 8.4 调度

```text
1. 收集 dirty bricks（按 (等级, 距离相机, 优先级) 排序）
2. 每个消费者按预算处理，剩余保持 dirty
3. 超出全局预算时，降级 Stale 消费者
4. 输出一致性状态供调试与断言
```

**没有这一步，炸一堵墙会瞬间触发六套系统全量重建。**

---

## 9. 粒子系统

### 9.1 管线

```text
发射 → 紧致 → 排序（按 cell）→ 平流（采样 velocity）
     → 碰撞（采样 SDF）→ splat（写 density/heat）→ 场求解
     → 渲染（直接 或 体积）
```

### 9.2 七个阶段

| 阶段 | 做什么 | 成本（5060 / 500k 粒子） |
|---|---|---|
| Emit | 按预算生成 | 0.1–0.3 ms |
| Compact | 移除死亡，紧致数组 | 0.1–0.2 ms |
| Sort | 3D cell 计数排序 | 0.5–1 ms |
| Advect | 三线性采样 + 外力 | 0.3–0.6 ms |
| Collide | SDF（复用 `sdf-bake`）/ 高度场 | 0.2–0.5 ms |
| Splat | 整数原子 / 浮点原子 | 0.5–2 ms |
| Solve | 浮力、扩散、涡量 | 1–2 ms |

### 9.3 渲染器插件

| 渲染器 | 适用 | 是否经过场 |
|---|---|---|
| Sprite | 雨、雪、尘、血 | 否 |
| Ribbon | 拖尾、丝带 | 否 |
| Mesh | 碎屑、叶片 | 否 |
| Light | 火星、闪光 | 否 |
| Decal | 焦痕、血迹 | 否 |
| **Volumetric** | 烟、火、雾 | **是** |

**VFX 的 80% 不经过场。** 场只服务体积渲染与空间查询。

### 9.4 事件总线

```cpp
struct ParticleEvent {
    Type     type;     // Collision / Death / ThresholdCrossed / LifetimeEnd
    Vec3     position;
    Vec3     normal;
    float    energy;
    uint32_t source;
};
```

消费者：生成子效果、贴花、光源、声音、伤害判定。**爆炸的"十几个效果串联"靠事件，不靠场。**

---

## 10. 求解器

### 10.1 烟 / 火（第一版只做这一个）

经典管线（Fedkiw 2001 骨架，二十年未变）：

1. **平流**：semi-Lagrangian，固定步长；可选 MacCormack 提细节。
2. **浮力**：`f = α·T·(0,1,0) − β·density·(0,1,0)`。
3. **压力投影**：Jacobi 20–40 次，或红黑高斯-塞德尔。
4. **涡量约束**：保留火苗卷曲细节，否则会糊。
5. **燃烧**：`fuel + O2 → heat + smoke`，黑体色温辐射。

128³ 场在 5060 上约 2–3 ms/帧。

### 10.2 不做

- 不做完整 MPM（Jolt 已覆盖刚体/碎片）。
- 不做水面（高度场是另一套，见水方案）。
- 不做两相流（泡沫/气泡），除非有明确需求。

### 10.3 参考实现

- Fedkiw / Stam / Jensen 2001《Visual Simulation of Smoke》
- WebGL-Fluid-Simulation（MIT）：2D stable fluids 参考
- OpenVDB `vdb_tool`：离线验证与格式转换

---

## 11. 渲染对接

| 效果 | 路径 |
|---|---|
| 烟/火/雾 | 体积步进（复用毛发/水的 ray march 基础设施） |
| 火星/碎屑/雨 | 直接渲染 |
| 体积光 | 密度场 + 散射 |
| 热浪扭曲 | 温度梯度 → 屏幕空间折射 |

**关键约束**：体积步进的密度场必须来自 WF/LEF，不能另建一套，否则"看到的"和"查询到的"会不一致。

大气体积雾与体积光轴的独立并行方案见 [VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md)；本文只负责粒子密度场与求解器，不重复 froxel 网格、灯注入和光轴设计。

---

## 12. 作者工作流

这是 v0.1 最大的短板：美术没法快速迭代。v0.2 用三条补齐，全部贴合项目现状。

### 12.1 效果描述 = 热重载 JSON

```json
{
  "id": "campfire",
  "emitters": [{ "type": "point", "rate": 120, "attrs": { "temperature": 900 } }],
  "field": { "level": "local", "channels": ["density", "temperature"], "size": [64,32,64] },
  "renderers": ["volumetric"],
  "events": [{ "on": "death", "spawn": "ember" }]
}
```

改文件 → 运行中的引擎重载 → 立刻看到。**复用现有场景热重载机制**，不新写编辑器。

### 12.2 与 Web 编辑器对齐

`docs/EDITOR.md` 已确定"Web 编辑器 + 原生引擎"。效果编辑不走第二套 UI：

1. **短期**：`effects/*.effect.json` 手改 + 热重载；
2. **中期**：Web 编辑器加"效果"面板，复用现有事务/差异预览/提交语义；
3. **调试可视化**（通道切片、`lastWriter`、单通道开关）可先用 JSON dump + Web 面板，**不引入第二套 UI 栈**。

### 12.3 MCP 调参 + 截图对比

复用已有 MCP 工具面，新增：

- `effect_set_param(effectId, path, value)`
- `effect_capture(effectId, frame, cameras[])`
- `effect_diff(effectId, reference)` → 复用 [IMAGE-REGRESSION.md](IMAGE-REGRESSION.md) 的 SSIM / ΔE

于是调参可以脚本化、自动化，甚至让 AI 代理跑"改参数 → 截图 → 比参考图 → 再改"的循环。**这是 Niagara 没有的能力，也是本项目"AI 原生"叙事的落点。**

---

## 13. 创新点

| # | 创新 | 类型 | 新颖度 | 成本 |
|---|---|---|---|---|
| 1 | **整数原子确定性沉积** | 算法 | 高（未见公开方案） | 2–3 周 |
| 2 | **可观察性驱动的提升/降级** | 架构 | 中高 | 3–4 周 |
| 3 | **跨域一致性等级 + 预算调度** | 系统 | 中 | 4–6 周 |
| 4 | **热-声耦合**（温度场 → 声速 → 折射） | 算法 | 高（几乎无人做） | 1–2 周 |
| 5 | **代理在环 VFX 调参** | 工具/方法 | 中高 | 3–4 周 |

### 13.1 新颖度说明

- **#1**：确定性物理多见于定点数刚体；把整数原子用于**场沉积**并给出误差/性能分析，未见公开工作。
- **#2**：自适应网格（Losasso 2004）与 FLIP 混合（Losasso 2006）已存在；**按"玩法可观察性"决定精度**是新框架。
- **#3**：引擎里脏标记是常识；把它形式化成**一致性等级 + 预算 + 可断言状态**，是系统贡献。
- **#4**：声速 `c = √(γRT)`，热区折射声波。游戏与图形界几乎无人做。
- **#5**：仿真控制（Treuille 2003、伴随法 2004）是离线全局优化；**实时、代理驱动、视觉奖励**是新的。

### 13.2 不建议做的"创新"

- 再写一个欧拉/SPH 求解器：饱和。
- 用 NeRF/3DGS 做体积：卷死。
- 神经流体：需要大规模数据与算力，不适合小团队。
- "统一求解器"：Macklin 2014（Flex）已做过，不新。

---

## 14. 质量门禁

| 指标 | 阈值 | 测量 | 复用 |
|---|---|---|---|
| 确定性一致率 | 100% | 玩法通道跑 100 次哈希一致 | [DETERMINISM.md](DETERMINISM.md) |
| 整数原子相对误差 | < 0.5% | 与浮点参考对比 | 新增 |
| 提升延迟 | < 3 帧 | 从触发到 WF 可见 | 新增 |
| 降级残留 | 0 brick | 效果结束 N 帧后无泄漏 | 新增 |
| 预算超支 | < 1% 帧 | 全局预算统计 | [BUDGET.md](BUDGET.md) |
| 体积/查询一致性 | 逐像素一致 | 渲染密度 vs 查询密度 | 新增 |
| 内存峰值 | < 档位上限 | 见第 15 节 | 新增 |
| 效果截图回归 | SSIM ≥ 阈值 | 固定机位 + 固定种子 | [IMAGE-REGRESSION.md](IMAGE-REGRESSION.md) |

全部写入 `reports/<version>/<commit>/<timestamp>/`，超阈值 CI 红。

**沿用现有约定**：能力查询与效果实现分字段；没有截图/时序证据不能宣称完成；不允许为通过测试放宽阈值。

---

## 15. 内存与性能预算

### 15.1 内存（5060 / 8 GB）

| 项 | 估算 |
|---|---|
| LEF（8 并发 × 64³ × 4ch × 2B） | ~17 MB |
| WF 粗网格（256×64×256 × 6ch × ~1.5B） | ~25 MB |
| 玩法 CPU 场（128×32×128） | ~2 MB |
| 粒子（500k × 48B） | ~24 MB |
| 排序/临时 | ~16 MB |
| **合计** | **~85 MB** |

### 15.2 GPU 时间（1440p / 60 fps 预算 16.6 ms）

| 项 | ms |
|---|---|
| 粒子管线（500k） | 2.5–5 |
| 体积步进（半分辨率 + 时域） | 1.5–3 |
| 场求解（128³） | 2–3 |
| 提升/降级调度 | 0.2–0.5 |
| **合计** | **6–11** |

### 15.3 预算阶梯接入

在 [BUDGET.md](BUDGET.md) 的单调阶梯上追加两档：

| 档位 | 动作 |
|---|---|
| `field-lite` | 关闭 WF 提升、LEF 分辨率减半 |
| `field-off` | 关闭场求解，粒子回退到直接渲染 |

阶梯只包含本次运行真正可用的档，与现有 `rt-shadows-off` / `mesh-path-off` 一致。

---

## 16. 路线图

| 阶段 | 内容 | 依赖 | 粗估 | 验收 |
|---|---|---|---|---|
| **F0** | `ExecuteIndirect` + 分帧异步回读 + 三维纹理/体积通路；集成 cgltf / FastNoise2（D3D12MA / Tracy 可选） | 现有 compute 队列 | 2–3 周 | 无 CPU 回读的间接派发；读回延迟 < 2 帧；三维纹理可写可采 |
| **F1** | 运行时 bricked 稀疏场 + 通道租约 + 调试可视化 | F0 | 3–4 周 | 租约申请/释放无泄漏；通道切片可导出 |
| **F2** | 粒子核心：属性 schema、排序、平流、碰撞、splat、事件总线、渲染器插件 | F1 | 4–5 周 | 500k 粒子稳定 60 fps；五种渲染器可用 |
| **F3** | 烟/火求解器 + 效果模板 | F2 | 3–4 周 | 与参考图 SSIM 达标；128³ 稳定 |
| **F4** | WF + 提升/降级 + 一致性等级 + 预算阶梯接入 | F3 | 3–4 周 | 提升延迟 < 3 帧；六系统共享查询 |
| **F5** | 确定性：整数原子 + CPU 玩法场 + 对拍 | F4 | 2–3 周 | 100 次运行哈希一致 |
| **F6** | 作者工作流：effect JSON 热重载 + MCP 调参/截图 + Web 面板 | F3 | 2–3 周 | 改 JSON 即时生效；MCP 可调参并出 diff |
| **F7** | 第一个外部消费者（建议：AI 视线/威胁；可选 Steam Audio 声学） | F4 | 3–5 周 | 手电扫过 → 墙后代理不反应 |

**F0–F7 合计约 22–31 人周**；只做到 F5 约 17–23 人周。

---

## 17. 明确不做

- 不做 VFX 编辑器。
- 不做 Niagara 级模块栈。
- 不做完整 MPM / 两相流 / 水面。
- 不做布料、毛发（各有独立方案）。
- 不做神经流体。
- 不做"一个求解器解决一切"。
- 不承诺跨平台逐位一致（只承诺 D0/D1，见 [ANIMATION-PLAN.md](ANIMATION-PLAN.md) 的确定性等级）。

---

## 18. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| 场过度泛化 | 什么都存不好 | 通道必须有 owner；只存"被多方读"的量 |
| 调度复杂度吃掉收益 | 工期超支 | 先做两级 + 三等级，不追求通用 |
| 整数原子性能不达标 | 确定性路径失效 | F5 前先做 2 天基准实验 |
| 内存线性增长 | 8 GB 撞墙 | 通道预算表；租约强制上限 |
| 调试地狱 | 一个 bug 污染六系统 | 逐通道可视化 + lastWriter + 单通道开关 |
| 作者工作流仍不足 | 美术不用 | 定位为"程序员/TA 可用"；不承诺替代 DCC |
| 开源许可证风险 | 无法商用 | 逐一核对并登记到 [TOOLCHAIN.md](TOOLCHAIN.md) |
| 与编辑器路线冲突 | 两套 UI | 效果编辑并入 Web 编辑器，不引入第二套 UI 栈 |

---

## 19. 与现有路线的关系

- 不改变 [NEXT-PHASE.md](NEXT-PHASE.md) 的顺序。
- 不改变 [EDITOR.md](EDITOR.md) 的"Web 编辑器 + 原生引擎"路线。
- F0 的 `ExecuteIndirect` 与异步回读，与 [NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md) 的 B2 是同一笔投入，应合并排期。
- 与 [ANIMATION-PLAN.md](ANIMATION-PLAN.md) 共享"显式声明数据访问 + 分层哈希 + 确定性等级"的同一套纪律。
- 与水、毛发、皮肤方案共享体积步进与"可测量近似"的方法论。
- 场景事务仍是唯一写入 `assets/` 的通道；场状态不进事务，只进 `digest()`（仅玩法通道）。

---

## 20. 术语

| 术语 | 含义 |
|---|---|
| LEF | 局部效果场，效果私有、可丢弃 |
| WF | 世界场，全局稀疏、多消费者共享 |
| 租约（lease） | 消费者对场区域与通道的临时使用权 |
| 提升 / 降级 | 数据在 LEF 与 WF 之间迁移 |
| 一致性等级 | Strong / Eventual / Stale |
| 整数原子沉积 | 用量化整数做顺序无关的 splat |
| splat | 把粒子贡献写进场 |
| 可观察性 | 被渲染、AI、导航、声学之一读到的程度 |

---

## 21. 参考

1. J. Stam. *Stable Fluids*. SIGGRAPH 1999.
2. R. Fedkiw, J. Stam, H. Jensen. *Visual Simulation of Smoke*. SIGGRAPH 2001.
3. K. Museth. *VDB: High-Resolution Sparse Volumes with Dynamic Topology*. ACM TOG 2013.
4. F. Losasso et al. *Simulating Water and Smoke with an Octree Data Structure*. SIGGRAPH 2004.
5. F. Losasso et al. *Two-Way Coupled SPH and Particle Level Set Fluid Simulation*. 2006.
6. M. Macklin et al. *Unified Particle Physics for Real-Time Applications*. SIGGRAPH 2014.
7. M. Macklin, M. Müller. *Position Based Fluids*. SIGGRAPH 2013.
8. M. Ihmsen et al. *Unified Spray, Foam and Air Bubbles for Particle-Based Fluids*. 2012.
9. A. Kaplanyan, C. Dachsbacher. *Light Propagation Volumes*. 2010.
10. Z. Majercik et al. *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*. 2019.
11. B. Wronski. *Volumetric Fog*. SIGGRAPH 2014.
12. A. Treuille et al. *Keyframe Control of Smoke Simulations*. 2003.
13. A. McNamara et al. *Fluid Control Using the Adjoint Method*. 2004.
14. Valve. *Steam Audio*（开源 SDK）。
15. Microsoft. *Project Acoustics*。
16. Epic Games. *Niagara* 与 *Niagara Fluids*（工程参考，非开源）。

> 标注需核实的版本与许可证信息，在 F0 集成前必须逐一确认；不得把"能力查询"写成"效果已实现"。
