# 植被系统方案（Vegetation Plan）

- 版本：设计稿 v0.1（**未实现**，不是能力声明）
- 基线：NativeGPU 0.10.0（D3D12、C++17、compute 队列、mesh shader 样例、几何包 v2、DXR 静态阴影）
- 日期：2026-10-21
- 目标：在现有引擎上做到 **10,000 棵木本植被 + 50,000 丛地被草**，1080p / 60 Hz 主目标（RTX 5060 Laptop 8 GB），并且每一档都能回退、能截图、能写进报告
- 参考：Nanite 的**虚拟几何原则**（cluster LOD、屏幕误差、GPU 驱动剔除、按页流送），不是照抄 UE5 的完整实现
- 相关文档：[NANITE-LUMEN-PLAN.md](NANITE-LUMEN-PLAN.md)、[GEOMETRY-LAYOUT.md](GEOMETRY-LAYOUT.md)、[GEOMETRY-PACKAGE.md](GEOMETRY-PACKAGE.md)、[MESH-BINDLESS.md](MESH-BINDLESS.md)、[LIGHTING-QUALITY.md](LIGHTING-QUALITY.md)、[BUDGET.md](BUDGET.md)

---

## 0. 结论摘要

1. **10k 植被可行，但瓶颈不是 Nanite。** 植被的主要成本是实例数、CPU 提交、alpha-test 的像素 overdraw、阴影和带宽；Nanite 只解决“几何复杂度”。直接引入 cluster DAG 而不解决实例化/间接绘制/材质，10k 仍然会卡在 CPU 和 draw call 上。
2. **场景里不要放 10,000 个植被实体。** 放少量 `vegetation` patch 实体，由离线工具按种子确定性散布成**内容寻址植被包**。场景 JSON 保持小而可编辑，10k 实例不经过事务层。
3. **分两级做，不要一套管线包打天下：**
   - **木本植被**（树/灌木）：共享物种资产 + 每实例 LOD + 远景 impostor + 屏幕误差选择；
   - **地被草**：GPU 草丛/卡片，按距离降密度，按需剔除，不逐株建模。
4. **Nanite 的 cluster DAG 只用于近景 hero tree 和后续优化，第一版不用于叶片。** alpha-test 叶片在软光栅里仍要付出 overdraw，且 cluster 数会爆炸；“每片叶子都是 Nanite cluster”是错误的起点。
5. **先做 V0（离线包）和 V1（单物种 1k 实例出图），再谈 10k。** 没有截图、没有报告、没有回退，就不算完成——沿用本项目既有约定。

---

## 1. 现状与硬约束（来自代码，不是估计）

| 事实 | 代码/证据 | 对植被系统的影响 |
| --- | --- | --- |
| 场景最多 **10,000 实体**、JSON ≤ 16 MiB，实体只有 9 种 kind，无旋转，AABB 语义 | `assets/scene.schema.json`、`engine/scene/index.mjs`、`host/scene_loader.cpp` | 10k 棵树若做成实体，会占满全部实体预算；且每棵树一个 UUID/事务对象不可接受 |
| 每帧构造 `std::vector<DrawItem>`，**每个 item 一次 `DrawIndexedInstanced(...,1,...)`**，每 draw 上传 58 个 DWORD 根常量 | `host/main.cpp` 的 draw 循环；`renderer/d3d12_renderer.cpp` 的 raster pass | 10k 棵树 = 10k 次绘制 + 10k 次根常量写；CPU 提交必然爆炸，必须先做实例化/间接绘制 |
| 渲染器只有**单位立方体**顶点/索引缓冲；任意网格没有通用上传路径 | `renderer/d3d12_renderer.cpp`、`renderer/d3d12_renderer_internal.h` | 植被的第一前置不是 Nanite，而是“任意 mesh + 材质 + 实例数据”的绘制路径 |
| TLAS 实例上限 `MaxInstances = 1024`，且**每帧按刚体变换重建** | `renderer/d3d12_renderer_internal.h`、`updateTopLevelAccelerationStructure` | 10k 树不能逐棵进 TLAS；需要分块代理 + 静态 TLAS |
| 没有 `ExecuteIndirect`、没有 HZB、没有 visibility buffer、没有 TAA | `NANITE-LUMEN-PLAN.md` §1、`GEOMETRY-LAYOUT.md` §1 | GPU 剔除结果无法直接驱动绘制；第一版必须补 B2；HZB 是 V3 的优化，不是 V1 的阻塞 |
| 材质像素着色器用**世界空间平面投影 UV**（`planar`），不读顶点 UV；没有 alpha test、没有双面、`GpuMaterial.flags` 预留但未使用 | `shaders/material.hlsl`、`renderer/d3d12_renderer.cpp` 的 material record 构建 | 叶片用平面投影会直接坏掉；alpha-test 和顶点 UV 是植被材质的最低要求 |
| Mesh Shader 路径已跑通，但整体偏暗、绕序/法线未对齐，且 cooked 材质仍走光栅路径 | `docs/MESH-BINDLESS.md` | 第一版植被走**顶点/实例化路径**；mesh shader 只作为后续优化，不能作为前置 |
| DXR 阴影是 1 ray/pixel、二值可见性；DDGI 为 16×8×16、4 m 间距，覆盖相机附近约 60×28×60 m；froxel 范围 96 m；相机 far = 120 m | `docs/LIGHTING-QUALITY.md`、`host/main.cpp` | 植被**接收**阴影是免费的（深度已在）；但**投射**阴影必须用代理，不能让 10k 棵树逐棵进 TLAS；草不投射阴影 |
| compute 队列已可用；几何包 v2 已有 `lodError`/`flags`/`materialHandle` | `docs/COMPUTE.md`、`docs/GEOMETRY-PACKAGE.md` §9 | 可以直接复用 `lodError` 概念，但 v2 的 `lodError = diagonal / maxLod` 只是 AABB 代理误差，植被 LOD 必须换成**实测简化误差** |
| 项目有确定性事务、回放、预算管理器、版本化证据 | `docs/DETERMINISM.md`、`docs/REPLAY.md`、`docs/BUDGET.md` | 植被散布必须种子化；每档必须有报告字段和固定机位截图 |

**结论：** 植被系统的第一优先级不是“写一个 Nanite”，而是补四个前置：

1. **任意网格绘制路径**（顶点/索引/材质/UV）；
2. **GPU 驱动的实例化 + 间接绘制**（B2 `ExecuteIndirect`）；
3. **alpha-test / 双面 / 顶点 UV 的植被材质**（含 alpha coverage mip）；
4. **阴影代理**（分块合并的低模 BLAS），而不是 10k 棵树进 TLAS。

---

## 2. 成本模型：为什么“参考 Nanite”不等于“上 Nanite”

### 2.1 10k 棵树的成本拆解

假设一棵树 LOD0 约 20,000 三角形，LOD1 约 4,000，LOD2 约 800，远景 impostor 2 个三角形：

| 成本项 | 10k 全 LOD0（不可行） | 分级后（可行目标） | 主要手段 |
| --- | --- | --- | --- |
| 三角形 | 200,000,000 | 约 3–6,000,000 | 距离/屏幕误差 LOD + impostor |
| CPU 提交 | 10k draw + 10k 根常量 | < 64 draw，实例数据零上传 | 实例化 + `ExecuteIndirect` + 常驻实例缓冲 |
| 像素 overdraw | 叶片/草大量 alpha-test | 通过距离降密度、卡片、遮挡剔除控制 | 材质、密度、HZB、深度预 pass |
| 阴影 | 10k caster，TLAS/BVH 爆炸 | 每 32–64 m 分块合并代理，几十个 TLAS 实例 | 离线合并 shadow proxy + 静态 TLAS |
| GI | 10k 实例记录 + 探针遍历 | 分块 canopy 代理 + 少量近景树 | DDGI 代理颜色 |
| 内存 | 每树独立网格 | 物种共享，约 50–100 MB | 共享 mesh + 实例缓冲 + impostor 图集 |

### 2.2 Nanite 能帮什么、不能帮什么

| 成本 | Nanite 是否解决 | 说明 |
| --- | --- | --- |
| 单物体三角形过多 | ✅ | cluster 层级 + 屏幕误差 LOD |
| 每帧 draw call 膨胀 | ✅（配合 GPU 驱动） | 但需要我们自己实现 `ExecuteIndirect` |
| 小三角形光栅浪费 | ✅ | 软件光栅 + visibility buffer |
| 实例数量巨大 | ❌ | 仍需要实例化/剔除；Nanite 不替你减少实例 |
| alpha-test 叶片 overdraw | ❌ | 遮罩材质仍要逐像素 discard，overdraw 仍在 |
| 阴影成本 | ❌ | Nanite 的阴影也是另一套系统 |
| 10k 棵树的 CPU 提交 | ❌ | 必须靠实例化/间接绘制 |
| 草地的密度 | ❌ | 草通常不是 Nanite 几何，是程序化卡片 |

**所以本方案的口号是：用 Nanite 的“误差驱动 LOD + GPU 驱动”思想，不用 Nanite 的“所有东西都进 cluster”做法。**

### 2.3 验收预算（先写目标，后由报告填实测）

| 指标 | 目标（1080p，RTX 5060 Laptop） |
| --- | --- |
| 木本植被实例 | 10,000（场景总量；单帧可见量由相机与剔除决定） |
| 地被草 clump | 50,000（可裁到 20,000） |
| 可见三角形 | < 6,000,000 |
| 植被 draw call | < 64（含阴影代理） |
| 植被 GPU 时间 | < 8 ms（P95） |
| CPU 提交增量 | < 0.3 ms（相对无植被场景） |
| 植被显存 | < 128 MB |
| 每帧 CPU 实例上传 | **0 字节**（加载时上传一次） |
| 实例丢失/溢出 | 0；若溢出必须报告 `overflow`，不允许静默截断 |

这些数字是**门限目标**，不是当前实测；最终必须写进 `reports/latest/foliage-evidence.json`。

---

## 3. 总体架构

```text
离线（Node，确定性）                          运行时（D3D12）                        编辑（场景事务）
─────────────────────────                    ─────────────────────                  ─────────────────
glTF / 程序化物种                            加载植被包（常驻）                      vegetation patch 实体
  → LOD1..3 简化                               → 实例/分块/物种缓冲                   → 改 density/seed/species
  → LOD0 cluster DAG（可选）                    → 分块 shadow proxy BLAS              → 事务预览（实例数/包围盒）
  → impostor 图集                              → 静态 TLAS                           → 提交后重建包
  → shadow proxy                              每帧：
  → 碰撞胶囊                                    CS 剔除 + LOD 选择
                                               → 可见实例桶 + 间接参数
生物群系参数 + seed                             → ExecuteIndirect 实例化绘制
  → 确定性散布                                  → DXR 阴影 / DDGI 代理
  → 分块实例包（内容寻址）                       → 报告
```

三个关键设计决定：

1. **数据与场景分离**：场景只存 patch 参数与包引用；10k 实例在二进制包里。
2. **CPU 零逐实例工作**：加载时上传一次；每帧只有相机常量、风时间、交互点。
3. **渲染按桶批处理**：物种 × LOD 一个桶，最多几十个 draw；绝不逐棵 draw。

---

## 4. 场景模型与植被包

### 4.1 新实体 kind：`vegetation`

场景里一个 patch 代表一片树林/草地，而不是一棵树。建议复用现有 `position`/`scale` 语义：

- `position`：patch 中心（y 为地面高度）；
- `scale`：`[width, height, depth]`，x/z 是散布范围，y 是高度包络（用于剔除/报告，不是逐棵高度）；
- `collider`：patch 级别不参与逐棵碰撞（见 §10）；
- 新增 `vegetation` 对象。

```json
{
  "id": "c1000000-0000-4000-8000-000000000001",
  "name": "北坡混交林",
  "kind": "vegetation",
  "position": [0, 0, 60],
  "scale": [120, 25, 80],
  "color": [1, 1, 1],
  "collider": false,
  "vegetation": {
    "package": "meadow",
    "species": ["oak", "pine", "shrub"],
    "density": 0.012,
    "grassDensity": 0.8,
    "seed": 20261021,
    "chunkSize": 32,
    "lodBias": 1.0,
    "slopeLimit": 0.45,
    "scaleRange": [0.75, 1.35]
  }
}
```

字段约束（建议）：

| 字段 | 范围 | 含义 |
| --- | --- | --- |
| `package` | 1–64 字符 `[A-Za-z0-9_.-]` | 植被包名，对应 `assets/foliage/<name>/` |
| `species` | 1–8 个已知物种名 | 参与散布的物种 |
| `density` | 0–1 棵/m² | 木本密度；0 表示只有草 |
| `grassDensity` | 0–10 丛/m² | 草 clump 密度；0 表示无草 |
| `seed` | uint32 | 确定性散布种子 |
| `chunkSize` | 8–128 m | 剔除/阴影分块尺寸 |
| `lodBias` | 0.25–4 | 全局 LOD 偏移；>1 更早降级 |
| `slopeLimit` | 0–1 | 可散布坡度上限（1 = 不限） |
| `scaleRange` | `[min,max]`，0.1–5 | 每株缩放范围 |

> **密度与面积要匹配目标。** 现实森林的木本密度大致 0.01–0.1 棵/m²，所以 10,000 棵树需要约 10–50 万 m²（例如 700×700 m）的散布面积，通常由多个 patch 或一个大世界组成；上面的 `120×80` 示例只是单块示意，按 0.012 棵/m² 约 115 棵。`density` 的 1 棵/m² 是校验上限，不是推荐值。草同理：0.8 丛/m² 时 50,000 丛约需 6 万 m²。

> **地形前提：** 当前场景没有 heightmap/terrain，ground 是盒体，也没有逐顶点高度查询。V0/V1 的散布以 `patch.position.y` 为水平地面，`slopeLimit`/高度规则只保留接口并默认关闭；真正支持坡地需要先有 terrain/heightmap（对应 `ED-17 地形与世界搭建`），这是独立工作包，不能在植被系统里假装已支持。

**版本策略：** 新增 kind 与 `vegetation` 字段会改变场景契约，建议 `schemaVersion: 3`，读取端继续接受 1/2/3。必须同步四处：

1. `assets/scene.schema.json`；
2. `engine/scene/index.mjs`（`ENTITY_KINDS`、`ENTITY_OPTIONAL_KEYS`、校验）；
3. `host/scene_loader.cpp`（`kinds`、`exactKeys`、`vegetation` 解析）；
4. 文档与测试（`docs/SCENE-QUERY.md`、`docs/SCENE-LINT.md`、`tests/`）。

> 如果不愿意升 v3，最低限度也要在 v2 上加可选字段并让旧包可读；但“新增 kind 却不改版本”会让旧读取端直接拒绝，不能算兼容。

> **宿主过滤：** `vegetation` 不是盒子，必须从 `Game` 的实体姿态/盒子渲染/碰撞中排除；否则会被当成一个巨大的 prop 画出来。落点是 `host/main.cpp` 的 draw 循环与 `engine/native/game.cpp` 的场景实体转换，需要显式 `if (kind == "vegetation") continue;` 并加测试。

### 4.2 内容寻址植被包

```text
assets/foliage/<package>/
  manifest.json                  # 物种/LOD/分块/哈希/实例数
  instances.bin                  # FoliageInstance[]，按 chunk 连续
  chunks.bin                     # FoliageChunk[]
  species.json                   # 物种表 + LOD + 材质 + 碰撞代理
  meshes/
    <species>-lod0.bin           # 顶点/索引（或 cluster 包）
    <species>-lod1.bin
    <species>-lod2.bin
  impostors/
    <species>.ngtex              # 图集：albedo + normal
  shadows/
    chunk-<n>.bin                # 合并后的低模 shadow proxy
  collision/
    <species>.json               # 胶囊/圆柱代理
```

`manifest.json` 至少包含：

```json
{
  "formatVersion": 1,
  "kind": "nativegpu.foliage-package",
  "generator": "nativegpu-foliage/0.1.0",
  "source": { "patches": [], "contentHash": "..." },
  "counts": { "instances": 10000, "grass": 50000, "chunks": 32, "species": 6 },
  "species": [{ "name": "oak", "lods": 4, "lodError": [0, 0.08, 0.4, 1.6], "impostor": true }],
  "chunks": [{ "id": 0, "bounds": [], "instanceStart": 0, "instanceCount": 312 }],
  "sections": [{ "name": "instances", "path": "instances.bin", "bytes": 0, "sha256": "..." }],
  "packageDigest": "..."
}
```

规则沿用几何包：

- 键排序、无时间戳、无 locale 格式化、统一 `\n`；
- 相同 patch 参数 + 相同物种资产 + 相同工具版本 → **byte-identical**；
- `packageDigest` 覆盖除自身外的整个 manifest；
- 篡改/缺失/版本错分别给稳定错误码。

建议错误码：

| 错误码 | 触发 |
| --- | --- |
| `NG-E-FOLIAGE-MISSING` | manifest / section 缺失 |
| `NG-E-FOLIAGE-MANIFEST` | JSON 损坏、字段/偏移/分块非法 |
| `NG-E-FOLIAGE-TAMPERED` | section 哈希或 digest 不符 |
| `NG-E-FOLIAGE-STALE` | 场景参数与包 digest 不匹配（改了 density/seed 但没重建） |
| `NG-E-FOLIAGE-OVERFLOW` | 实例/分块超过运行时容量（拒绝加载或报告，不静默截断） |
| `NG-E-FOLIAGE-SPECIES` | 场景引用了包内不存在的物种 |

### 4.3 运行时记录布局（建议）

```cpp
// 64 B，16 B 对齐；加载时上传，运行时只读
struct FoliageInstance {
    float    position[3];
    float    scale;
    float    rotation[4];   // 四元数；或 yawSin/yawCos + 两个保留
    uint32_t species;
    uint32_t chunk;         // 所属分块，供扁平 dispatch 直接查 chunk 可见性
    uint32_t lodBias;       // 逐实例偏移（0 = 默认）
    uint32_t phase;         // 风相位/随机稳定哈希
    uint32_t tint;          // RGBA8 颜色变化
    uint32_t padding[3];
};

// 48 B
struct FoliageChunk {
    float    boundsMin[3];
    float    boundsMax[3];
    uint32_t instanceStart;
    uint32_t instanceCount;
    uint32_t speciesMask;
    uint32_t shadowProxy;   // 该 chunk 的 BLAS/代理索引
    uint32_t padding[2];
};

// 约 96 B
struct FoliageSpecies {
    float    boundsMin[3];
    float    boundsMax[3];
    uint32_t lodCount;
    float    lodError[4];   // 实测简化误差（世界空间）
    uint32_t vertexOffset[4];
    uint32_t indexOffset[4];
    uint32_t indexCount[4];
    uint32_t materialHandle[4];
    uint32_t impostorAtlas;
    uint32_t shadowProxy;
    uint32_t flags;
};

// 8 B；GPU 压缩后的可见实例
struct VisibleFoliageInstance {
    uint32_t instance;
    uint32_t lod;           // 0..3 = mesh，0xFFFFFFFF = impostor
};
```

> `FoliageSpecies` 不做逐实例反查：可见实例记录里带 `lod`，绘制桶本身也知道物种，PS/VS 只读实例 + 物种表。避免 Nanite 文档里明确排除的“resolve 反查实例表”。

### 4.4 为什么不在运行时散布

- **确定性**：同一 patch 两次加载必须产生同一实例顺序；离线包可直接比较 digest。
- **CPU**：10k–50k 实例的散布、坡度/掩码采样、分块排序不应每帧做，甚至不应每次加载做。
- **事务**：编辑器预览只改参数，提交后由构建步骤重写包；运行时不会因为一次 undo 产生不同实例。
- **CI**：包可校验、可篡改检测、可缓存；与几何包/纹理 cook 的工程习惯一致。
- **后续**：动态生长、季节变化可以在此之上做“实例覆盖层”，但不是第一版。

---

## 5. 物种资产与 LOD 阶梯

### 5.1 LOD 阶梯

| 层级 | 用途 | 典型几何 | 切换依据 |
| --- | --- | --- | --- |
| LOD0 | 近景 hero tree / 相机 0–15 m | 10k–30k 三角形；可选 cluster DAG | 屏幕误差 ≤ 1 px |
| LOD1 | 中景 15–40 m | 2k–6k 三角形 | 屏幕误差 ≤ 1.5 px |
| LOD2 | 远景 40–80 m | 300–1k 三角形 | 屏幕误差 ≤ 2 px |
| LOD3 / impostor | 80 m 外或屏幕高度 < 48 px | 2–4 个交叉 quad | 屏幕高度阈值 |
| 草 clump | 0–50 m | 7 片叶 × 2 三角形 = 14 三角形 | 距离降密度 + 3 级 LOD |

**关键：** `lodError` 必须是**实测的简化误差**（顶点最大偏差或 QEM 误差），不能继续用 `diagonal / maxLod` 这种 AABB 代理。否则 LOD 切换与视觉误差无关，调参没有意义。几何包 v2 的公式只适合占位，植被包要单独计算。

### 5.2 离线构建步骤

```text
输入：glTF 子树 / 程序化树生成器 / 草 clump 生成器
1. 解析为按材质分组的 submesh（bark / leaf / grass）
2. 校验：非有限值、退化三角形、UV 缺失、alpha 纹理格式
3. 生成 LOD1..3：
   - QEM 或 meshoptimizer（固定版本 + SHA-256）
   - 锁定 UV seam / 材质边界 / 开放边界
   - 记录每个 LOD 的实测误差（max vertex deviation）
4. LOD0（可选）：复用 geometry-package 的 cluster DAG
5. impostor 烘焙：
   - 16 视角（4×4 图集）正交渲染 LOD2
   - albedo(RGBA) + normal(octahedral) + coverage
   - 4 px dilation，避免采样接缝
6. shadow proxy：
   - 树干 8 边圆柱 + 树冠 1 级 icosphere（约 100 三角形）
   - 每 chunk 按实例变换合并成一个 mesh
7. 碰撞代理：每棵树 1 个胶囊（半径/高度），草无碰撞
8. 散布：按 patch 参数 + seed + 高度/坡度/掩码，写入实例与分块
9. 写包、算 digest、跑自检
```

**散布算法（V0 建议）：** 用“抖动网格”而不是 O(n²) 拒绝采样：

```text
cell = 1 / sqrt(max(density, 1e-6))       // 例如 0.02 棵/m² → 7.07 m
for gx, gz over patch bounds:
    u0 = hash01(seed, gx, gz)             // splitmix32 → [0,1)
    u1 = hash01(seed ^ 0x9e3779b9, gx, gz)
    u2 = hash01(seed ^ 0x85ebca6b, gx, gz)
    candidate = cellCenter + (u0 - 0.5) * cell
    if (u1 > mask(candidate)): reject      // mask 0..1，0 = 完全不长
    if slope(candidate) > slopeLimit: reject
    species = speciesTable[hash32(seed ^ 0xc2b2ae35, gx, gz) % speciesCount]
    instance = { position, yaw = u2 * 2π, scale = lerp(min,max,u1), ... }
```

- 每格最多 1 棵木本；草用更细网格或每格 N 丛。
- 严格 Poisson disk 不是第一版目标；抖动网格已经足够均匀、可复现，且 O(n)。
- 生成后按 `chunk`、再按 `species`、再按位置排序；实例顺序固定，便于 diff 和回放。
- 所有哈希必须是整数/固定算法，不能用 `Math.random()` 或依赖 locale 的浮点格式化。

> 注意：现有 `geometry-package` 的 cluster section 顶点只有 `position + normal`（stride 24），没有 UV、顶点色、材质区间；V4 的 hero tree cluster 化必须先扩展几何包格式并升 `formatVersion`，不能直接复用当前产物。

### 5.3 alpha coverage mip（容易被忽略，但决定远景是否“秃”）

普通 mip 平均会让 alpha 逐渐变小；alpha-test 阈值 0.5 时，远处的叶子会成片消失，边缘闪烁。纹理 cook 必须支持：

```text
对每个 mip：
  targetCoverage = mip0 中 alpha >= threshold 的像素比例
  调整该 mip 的 alpha（线性缩放/直方图匹配），使 coverage 接近 targetCoverage
  记录每 mip 的 coverage 到 manifest
```

同时：

- 植被 albedo 使用 **BC7 或 BC3**；BC1 的 1-bit alpha 不适合叶片，cook 阶段直接拒绝；
- impostor 图集必须 dilation；否则 alpha-test 边缘出现黑边/透明缝；
- 报告 `alphaCoverage` 每 mip 的实测值，作为证据。

### 5.4 材质扩展

现有 `namedMaterial` 只有 albedo/normal/orm/uvScale/roughness/metalness/wetness/wear。植被需要新增：

```json
{
  "albedo": "oak_leaf",
  "normal": "oak_leaf_normal",
  "orm": "oak_leaf_orm",
  "uvScale": 1.0,
  "uvMode": "vertex",
  "alphaTest": 0.42,
  "twoSided": true,
  "wind": 0.25,
  "translucency": 0.35
}
```

| 字段 | 范围 | 说明 |
| --- | --- | --- |
| `uvMode` | `planar` / `vertex` | 默认 `planar` 保持旧场景逐像素不变；植被用 `vertex` |
| `alphaTest` | 0–1 | 0 = 不透明；>0 时 `albedo.a < alphaTest` discard |
| `twoSided` | bool | 叶片双面；实现上建议统一 `CULL_NONE`，PS 用 `SV_IsFrontFace` 翻法线 |
| `wind` | 0–1 | 风强度；0 = 静态 |
| `translucency` | 0–1 | 背光透射近似（wrap/transmission lobe） |

`GpuMaterial.flags` 已有 32 位但当前写 0，可直接用：

| bit | 含义 |
| --- | --- |
| 0 | alphaTest 启用 |
| 1 | twoSided |
| 2 | vertexUV |
| 3 | foliage（走植被 PS） |
| 4 | wind |
| 5 | translucency |
| 6+ | 保留 |

**着色器改动：**

1. `material.hlsl` 目前忽略顶点 UV，硬编码平面投影；植被 PS 必须使用顶点 UV。
2. 采样 albedo 的 alpha，alpha-test discard。
3. 背面翻转法线；叶片用 wrap diffuse（`NdotL * 0.5 + 0.5`）和透射项。
4. 建议把现有材质 PS 的光照/IBL/局部灯代码抽成 `surface_shading.hlsli`，植被 PS 复用，避免复制 200 行后两套光照漂移。
5. 风：VS 中按顶点权重弯曲；不要用 CPU 更新实例矩阵。

### 5.5 风与交互

- 顶点色打包：`R = 刚度`、`G = 相位`、`B = 抖动频率`、`A = AO`。植被专用顶点格式（position/normal/uv + RGBA8 color）即可，不必改现有 `Vertex`。
- 树：树干低频摆动，树枝/叶片高频抖动；`amplitude * height^2 * stiffness`。
- 草：`uv.y^2` 弯曲，两三个正弦叠加，按世界位置错相。
- 交互：一个小的 `interactor` 常量缓冲（玩家/车辆位置、速度、半径），草和灌木在 shader 里被推开；10k 株不产生 CPU 工作。
- 时间：`frameIndex * fixedDt` 或场景仿真时间；回放时与现有确定性契约一致。
- **限制：** 阴影代理是静态的，风不会反映到阴影里；这是第一版明确接受的限制。

---

## 6. GPU 运行时

### 6.1 帧图

```text
每帧 CPU：
  相机矩阵、时间、风参数、交互点、LOD bias、质量档
  （不上传实例数据）

compute：
  1. ClearIndirectArgs
  2. CullChunks        （视锥 + 距离 + 可选 HZB）
  3. CullInstances     （视锥 + 屏幕误差选 LOD + 原子压缩）
  4. BuildHZB          （本帧深度之后；V3）

graphics：
  5. 现有不透明场景（盒子）
  6. 植被近/中/远桶 + 草（alpha-test，写深度）
  7. 粒子/雾/云合成（既有）

DXR：
  8. 阴影（读深度，射向 key light；植被通过 proxy 投射，自身免费接收）
  9. DDGI（命中 proxy 读 canopy 颜色）
```

> 因为现有 DXR 阴影 pass 是**从深度重建世界坐标再发射光线**，任何写入深度的植被都会自动**接收**阴影；植被只需通过 proxy **投射**阴影。这是本引擎结构上的一个便宜点，应该利用，而不是另做一套植被阴影。

### 6.2 剔除与 LOD 选择

第一版不需要 GPU work graph；10k–100k 实例用一个扁平 dispatch 足够：

```hlsl
// 伪代码：一个线程一个实例
uint i = DispatchIndex;
FoliageInstance inst = instances[i];
FoliageChunk chunk = chunks[inst.chunk];
if (!chunkVisible[inst.chunk]) return;

float3 center = mul(instanceMatrix(inst), species.center);
float radius = species.radius * inst.scale;
if (!SphereInFrustum(center, radius)) return;

float dist = max(distance(center, camera), 1e-3);
float projScale = viewportHeight / (2.0 * tan(fovY * 0.5));

uint lod = species.lodCount - 1;
for (uint l = 0; l < species.lodCount; ++l) {
    float errPx = species.lodError[l] * inst.scale * projScale / dist;
    if (errPx <= lodTargetPixels) { lod = l; break; }
}

// 远距离改 impostor
float screenH = species.height * inst.scale * projScale / dist;
if (screenH < impostorPixels) lod = IMPOSTOR;

uint bucket = species.bucket[lod];
uint slot = InterlockedAdd(counter[bucket], 1);
if (slot < capacity[bucket]) visible[bucketBase[bucket] + slot] = {i, lod};
else InterlockedAdd(overflow, 1);
```

- `lodTargetPixels` 建议 1.0–1.5 px，暴露为 `lodBias`。
- 加 10–15% 滞回，避免相机轻微移动导致 LOD 抖动。
- 没有 TAA 时**不要做 alpha 抖动淡入淡出**：会产生明显的 screen-door 噪点。第一版用滞回硬切；近景 hero tree 的连续 LOD 留给 V4 cluster 方案。
- HZB 两阶段遮挡是 V3 的优化项；没有它，10k 也能跑，只是林下/山后实例会被多画。

### 6.3 绘制批次与 `ExecuteIndirect`

- 桶 = 物种 × LOD（含 impostor）+ 草 LOD。6 个物种 × 4 = 24 桶，草 3 桶，共 27 个 draw；加上 shadow proxy 的少量 draw，仍 < 64。
- 每个桶一个 `DrawIndexedInstanced` 命令，`instanceCount` 由 GPU 写入间接参数；CPU 不回读。
- 实例数据从 `StructuredBuffer` 按 `SV_InstanceID + bucketBase` 取，**不放根常量**。这直接解决当前“每 draw 58 DWORD”不可扩展的问题。
- `ExecuteIndirect` 用一个 command signature + 一个参数缓冲，一次提交多个桶。
- 后续可选：mesh shader 每桶一个 `DispatchMesh`，AS 里做 meshlet 剔除；前提是先修复现有 mesh 路径的绕序/偏暗问题，不阻塞 V1。

### 6.4 草地的特殊路径

- 分块：8 m × 8 m tile；每 tile 预生成 clump 实例范围。
- 几何：一个 clump = 7 片交叉叶（14 三角形），近景；中景 3 片交叉卡片（6 三角形）；远景 1 个 billboard（2 三角形）。
- 密度：按距离做**稳定哈希降采样**（`hash(instance, cameraCell) < p`），不要随机每帧抖动；远处保留更少 clump，减少 overdraw。
- 不投射阴影（不进 TLAS）；通过深度自动接收树冠/建筑的阴影。
- 每顶点 AO + 地面颜色过渡，掩盖密度边缘。
- 草是 overdraw 大户；如果实测像素成本高，可在 V3 加一个**近景草深度预 pass**（只写深度 + alpha-test，不写颜色），把 overdraw 压到 1 次着色。是否值得由报告决定，不预设。

### 6.5 阴影代理（关键 scalability 手段）

**不要**把 10k 棵树逐棵放进 TLAS。做法：

1. 离线把每个 chunk 内所有树的 shadow proxy（树干圆柱 + 树冠椭球）按实例变换合并成一个 mesh；
2. 每个 chunk 一个 BLAS，TLAS 里一个实例；
3. 近景 hero tree（例如相机 40 m 内、最多 256 棵）可单独用更高精度的 proxy BLAS；
4. 静态树在加载时构建 TLAS，**不每帧重建**；动态车辆/门用 `ALLOW_UPDATE` 更新实例变换，或维持现有的动态实例重建路径，但只更新动态部分。

这样 10k 棵树在 DXR 里只有几十到几百个实例，BVH 遍历成本可控。代价：

- 阴影是“树冠团块”，没有叶片缝隙的 dapple；
- 可在 closest-hit 里给 canopy proxy 一个 `transmittance`（例如 0.55），让树冠阴影变浅；这是**近似**，不是真实透射，报告里要写明；
- 树干/树冠 proxy 不随风更新。

如果后续需要更高质量的植被阴影，再考虑 CSM / 软阴影 / 叶片 any-hit 透射；第一版不做。

### 6.6 GI / DDGI

- `QualityInstance` 每条 TLAS 实例记录一个颜色/材质；chunk proxy 用该 chunk 的平均 canopy 颜色，近景 hero tree 用单树颜色。
- DDGI 探针命中 canopy proxy 后得到绿色反弹光，这比“植被完全不参与 GI”好得多。
- 草不单独进 GI；靠地面 albedo + canopy 阴影 + 顶点 AO。
- `MaxInstances` 从 1024 提高到 2048（或按 chunk proxy 数量定），并拆出“静态植被实例记录”和“动态刚体记录”，避免每帧 memcpy 10k 条。

### 6.7 流送

**第一版不做流送。** 10k 木本 + 50k 草的常驻内存估算：

| 项 | 估算 |
| --- | --- |
| 实例 64 B × 60k | ≈ 3.8 MB |
| 分块/物种表 | < 1 MB |
| 物种 LOD 网格（6 物种 × 4 LOD，共享） | 30–60 MB |
| impostor 图集 | 8–16 MB |
| shadow proxy（分块合并） | 5–15 MB |
| 合计 | < 100 MB |

对 8 GB 显存完全可接受。分块先用于**剔除、阴影、GI**；流送（按页加载/卸载）留到 V5，等场景超过单区域或显存吃紧再做。这样避免在第一版同时引入“包格式 + 流送 + 页表”三个变量。

### 6.8 确定性与回放

- 散布：离线、种子化、字节可复现。
- 风：`frameIndex`/固定步长驱动；shader 内不用 `Time` 的浮点自由漂移。
- LOD：相机 + 滞回状态；固定机位回放确定。
- GPU 压缩顺序：原子追加顺序不稳定，但**不透明 alpha-test 绘制与深度测试结果与顺序无关**；报告计数（可见数）稳定。不要用可见实例数组的顺序做任何影响像素的决定。
- 报告字段：

```json
"foliage": {
  "enabled": true,
  "quality": "high",
  "packageDigest": "...",
  "instancesTotal": 60000,
  "instancesVisible": 18000,
  "chunksTotal": 32,
  "chunksVisible": 9,
  "lodCounts": [120, 380, 1200, 16300],
  "drawCalls": 27,
  "triangles": 4200000,
  "gpuCullMs": 0.18,
  "gpuRasterMs": 3.9,
  "gpuShadowMs": 1.1,
  "memoryBytes": 73400320,
  "overflow": 0,
  "fallbackUsed": false,
  "error": ""
}
```

`capabilities.implemented.foliage` 只有在本次运行真的绘制过植被时才为 true；`probeAvailable` 单独记录能力/包存在性。沿用“能力查询与效果实现分字段”的规则。

---

## 7. 与 Nanite 的关系：该抄什么、不该抄什么

| Nanite 机制 | 植被是否采用 | 说明 |
| --- | --- | --- |
| cluster DAG（约 128 三角形/cluster） | **V4，仅 hero tree** | 叶片卡片会让 cluster 数和 alpha-test 成本爆炸；树干/树枝适合 |
| 屏幕空间误差选 LOD | **✅ V1 必做** | 但误差必须是实测简化误差，不是 AABB 对角线 |
| GPU 驱动剔除 | **✅ V1/V2 必做** | 实例/分块剔除 + 间接绘制；否则 10k 不可扩展 |
| 两阶段 HZB 遮挡 | **V3** | 林下/山后收益大，但不是 10k 的阻塞 |
| visibility buffer + 延迟材质 | **V4+，可选** | 当前前向双目标 + alpha-test 已能出图；不要为了 Nanite 感先重构 G-buffer |
| 软件光栅小三角形 | **不用于叶片** | 叶片 overdraw 不因软光栅消失；草也不适合 |
| 按页流送 | **V5，可选** | 单区域 10k 全常驻足够；先不引入页表 |
| 遮罩材质（masked） | **✅ 必做** | alpha-test + alpha coverage mip + BC7/BC3 |
| World Position Offset | **✅ 但仅视觉** | 风在 VS；阴影 proxy 不随风更新 |
| 每 cluster 连续 LOD | **仅 hero tree** | 远景用离散 LOD + 滞回；没有 TAA 时不做抖动淡入 |

一句话：**Nanite 的“层级 + 误差 + GPU 驱动”是方法；Nanite 的“全场景 cluster + 软光栅 + 流送”是重量级实现。植被先拿方法，重量级实现按测量结果逐步加。**

---

## 8. 质量档、预算与回退

### 8.1 质量档

| 档 | 木本实例 | 草 clump | LOD bias | 阴影代理 | 目标 |
| --- | --- | --- | --- | --- | --- |
| `foliage-off` | 0 | 0 | — | 无 | 兼容基线，逐像素等于无植被 |
| `foliage-lite` | 2,000 | 10,000 | 2.0 | 仅 chunk 级 | iGPU / 低端回退 |
| `foliage-balanced` | 6,000 | 30,000 | 1.25 | chunk + 近景 64 棵 | 默认 |
| `foliage-high` | 10,000 | 50,000 | 1.0 | chunk + 近景 256 棵 | 主目标 |

- 默认按适配器显存/能力选档，并把选择理由写进报告；不静默降档。
- 帧预算管理器新增阶梯：`foliage-lod-bias+` → `foliage-grass-off` → `foliage-off`；每一步记录触发帧、原因、前后计数。
- 容量不足时**拒绝加载**并报 `NG-E-FOLIAGE-OVERFLOW`；运行中可见列表溢出时计 `overflow` 并报告，不静默丢实例。

### 8.2 回退路径

- `--foliage off`：完全不加载包、不建 proxy、不派发；图像必须与“无植被基线”逐像素一致（用现有图像契约工具验证）。
- `foliage-lite`：只画 impostor/chunk 级，不画近景 LOD0；用于对比。
- 阴影代理构建失败：植被仍绘制，阴影回退到现有 key 阴影（无植被投射），报告 `shadowFallback=true` 和原因。
- 包损坏：不崩溃；保留上一有效包或关闭植被，报稳定错误码。

---

## 9. 里程碑与验收

### 9.1 里程碑

| 里程碑 | 内容 | 可见成果 | 验收（可回归证据） | 粗估 |
| --- | --- | --- | --- | --- |
| **V0 离线包** | schema v3、植被包格式、确定性散布、物种 LOD/误差、impostor 烘焙、shadow proxy、包自检 | 暂无画面；`foliage.package.json` + 统计 | 同输入两次 byte-identical；实例数/分块/边界/坡度断言；篡改检测；离线测试 | 1–2 周 |
| **V1 可见切片** | 任意 mesh 上传、植被材质（顶点 UV/alpha-test/双面）、1 物种 × 1,000 实例、VRAM 常驻、固定机位截图 | 第一张真实树/草截图 | 与关闭植被的图像 diff；alpha 边缘无黑边；材质/UV 正确；帧时不崩 | 2 周 |
| **V2 10k 规模** | B2 `ExecuteIndirect`、GPU 剔除 + LOD 选择、实例桶、草、报告字段 | 10k 木本 + 20–50k 草，1080p | draw < 64；CPU 增量 < 0.3 ms；可见三角形 < 6M；overflow=0；固定机位三距离截图 | 2–3 周 |
| **V3 阴影/GI/档位** | chunk shadow proxy、静态 TLAS、DDGI canopy、质量档与预算阶梯 | 林下阴影、绿色反弹、lite/high 对比 | 阴影开关/档位对比；GPU P95 达标；回退图像一致；预算阶梯日志 | 2 周 |
| **V4 Nanite 式 hero LOD** | 近景树 cluster DAG + 屏幕误差 + 可选 vis buffer 实验 | 近景无跳变 hero tree | cluster 选择与 CPU 参考一致；连续 LOD 截图序列；与离散 LOD 的误差/成本对比 | 3–4 周 |
| **V5 流送（可选）** | 分页、LRU、缺页降级 | 更大世界 | 页命中率、内存峰值、缺页不崩 | 不承诺 |

总计到 V3 约 **7–9 人周**（含证据）；V4 另计。与 Nanite/Lumen 的 29–46 人周相比，这是一个可以独立交付的切片。

### 9.2 前置依赖

| 依赖 | 为什么 | 状态 |
| --- | --- | --- |
| 任意网格上传/绘制 | 当前只有单位立方体 | ❌ 需新增（V1 第一件事） |
| 顶点 UV + alpha-test + 双面材质 | 叶片的最低要求 | ❌ 需新增 |
| `ExecuteIndirect`（B2） | GPU 剔除结果驱动绘制 | ❌ 未实现 |
| 场景 schema v3 | `vegetation` kind + 材质字段 | ❌ 需新增 |
| 分块 shadow proxy + 静态 TLAS | 10k 阴影可扩展 | ❌ 需新增（V3） |
| HZB（B4） | 遮挡剔除 | ❌ 未实现；V3 可选 |
| TAA | 抖动 LOD/软边 | ❌ 无；第一版用滞回硬切规避 |
| mesh shader 修复 | 后续 GPU meshlet 路径 | ⚠️ 已有但偏暗/绕序问题；不阻塞 |

### 9.3 文件落点（V0/V1）

| 层 | 新文件/改动 | 说明 |
| --- | --- | --- |
| 场景契约 | `assets/scene.schema.json`、`engine/scene/index.mjs`、`host/scene_loader.cpp` | v3、`vegetation` kind、材质字段；四处同步 |
| 离线工具 | `tools/offline/foliage-package.mjs`、`tools/offline/foliage-lod.mjs`、`tools/offline/foliage-impostor.mjs` | 包格式、简化、图集；不依赖 GPU |
| CLI | `tools/foliage.mjs`（`build`/`verify`/`stats`） | 对应 `tools/scene.mjs` 的风格 |
| 测试 | `tests/foliage.test.mjs`、`tests/scene-v3.test.mjs` | 确定性、分块、篡改、schema |
| 渲染器 | `renderer/foliage.cpp`、`renderer/foliage.h`、`d3d12_renderer_internal.h`、`d3d12_renderer.cpp` | 实例/物种缓冲、间接绘制、桶 |
| 着色器 | `shaders/foliage.hlsl`、`shaders/foliage_common.hlsli`、`shaders/material.hlsl`（共享光照 include） | VS 风/实例矩阵、PS alpha-test/双面/UV |
| 材质 | `renderer/d3d12_renderer.h`、`renderer/d3d12_renderer.cpp`、`tools/textures.mjs` | 材质字段、flags、alpha coverage mip |
| 证据 | `tools/foliage.ps1` | 三档/三机位/报告/图像契约 |

**V0 不碰 `renderer/` 与 `shaders/`**；V1 才引入渲染路径。这样包格式和确定性可以先独立验证。

### 9.4 证据脚本

建议新增 `tools/foliage.ps1`：

1. `node tools/foliage.mjs build --scene <scene> --out assets/foliage/<name>`；
2. `node tools/foliage.mjs verify --package ...`（digest/分块/实例数）；
3. 原生跑三档（off/lite/high）+ 三个固定机位（近/中/远），保存 PNG；
4. 断言报告：实例数、可见数、draw call、三角形、overflow=0、fallback、GPU 时间；
5. 两次运行同参数：包 digest 相同、截图逐像素相同（或误差 < 契约阈值）；
6. 图像契约：off 与无植被基线逐像素一致；on 与 off 的变化像素 > 阈值；
7. 写 `reports/latest/foliage-evidence.json`，并把 `foliage` 段并入 `run.json`。

离线测试（Node）：

- `tests/foliage.test.mjs`：确定性、分块连续性、边界、坡度、物种引用、LOD 误差单调、impostor 覆盖、alpha coverage；
- 篡改一字节 → `NG-E-FOLIAGE-TAMPERED`；
- patch 参数变化 → digest 变化；场景引用旧 digest → `NG-E-FOLIAGE-STALE`。

### 9.5 验收门（硬）

- 没有固定机位截图 = 未完成；
- 没有 `foliage` 报告段 = 未完成；
- `--foliage off` 与基线不一致 = 未完成；
- 10k 场景出现 > 0 overflow 或静默截断 = 未完成；
- 只写“GPU 很快”而没有 P50/P95、draw call、三角形数 = 未完成。

---

## 10. 风险与止损

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| alpha-test 无 TAA → 远景闪烁/噪点 | 画质差 | alpha coverage mip + impostor dilation + 滞回硬切；TAA 作为后续独立项 |
| 草 overdraw 高 | 像素成本爆炸 | 距离降密度、卡片 LOD、近景深度预 pass（按实测决定） |
| 阴影 proxy 太粗糙 | 树冠阴影成团块 | 近景 hero tree 单独 proxy；canopy transmittance；报告写明近似 |
| BVH 实例/三角形过多 | DXR 变慢 | 分块合并（几十个 BLAS）+ 静态 TLAS；不逐棵进 TLAS |
| 任意 mesh 路径改动面大 | V1 延期 | 先复用蒙皮角色的“自定义 VB/IB + PSO”模式，只做非蒙皮静态 mesh |
| 场景 schema 变更波及四处 | 旧测试/工具失败 | 升 v3、读取 1/2/3、先加校验测试再改原生 |
| 包/流送范围蔓延 | 永远做不完 | V0–V3 全常驻；流送 V5，默认不做 |
| Nanite 式连续 LOD 期望过高 | 第一版跳变被误判为 bug | 文档写清“离散 LOD + 滞回”；hero tree 连续 LOD 放 V4 |
| 碰撞/玩法同步 10k 树 | 物理成本爆炸 | patch 不产生逐棵碰撞；只加载玩家附近胶囊，或第一版不与树碰撞 |
| 驱动/DXR 问题 | 阻塞 | 沿用 5 个有效工作日止损规则：保存最小复现、缩小范围、记录架构变更 |

---

## 11. 明确的“不做”

- ❌ 每棵植物一个场景实体、一个 UUID、一次事务。
- ❌ 每棵植物一个 draw call。
- ❌ 每帧 CPU 剔除/LOD/上传实例矩阵。
- ❌ 把 10k 棵树逐棵放进 TLAS/BLAS。
- ❌ 第一版用半透明排序做植被（10k 排序不可接受）；一律 alpha-test 或 impostor cutout。
- ❌ 第一版把每片叶子做成 Nanite cluster 或软光栅。
- ❌ 第一版做植被流送/虚拟纹理/页表。
- ❌ 草投射逐株阴影、草参与 DDGI。
- ❌ 为了“看起来像 UE5”而先重构 visibility buffer / TSR / Lumen。
- ❌ 在没有截图、报告、回退的情况下宣称“10k 已优化”。

---

## 12. 需要你决定的 3 个问题

1. **计数口径**：目标是 **10,000 棵木本（树+灌木）+ 50,000 丛草**，还是“总共 10,000 株植被”（后者容易得多）？我推荐前者，因为“上万棵植被”通常指可见的木本植物，草单独按 clump 计。
2. **质量/硬件目标**：是否确认 **RTX 5060 Laptop、1080p、60 Hz、植被 GPU < 8 ms**？如果要 1440p 或 120 Hz，预算和档位要重算。
3. **作者方式**：接受“**少量 vegetation patch + 离线确定性散布 + 内容寻址包**”吗？还是希望运行时程序化生成、编辑器里逐棵手摆？我推荐前者；后者会把编辑器、事务和性能同时复杂化。

如果这三点确认，我建议下一步直接做 **V0**：先写 schema v3 + `tools/foliage.mjs build/verify` + `tests/foliage.test.mjs`，不碰渲染器；等包和确定性证据立住，再做 V1 的任意 mesh + alpha 材质 + 首张截图。
