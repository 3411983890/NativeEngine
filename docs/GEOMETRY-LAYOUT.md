# 几何与材质底层架构（Nanite / Lumen 共用）

- 状态：**冻结 v2**（2026-10-20，随 0.10.0）
- 适用范围：所有进入渲染的几何与材质，无论静态或运动
- 依据：本机实测能力（见 §1），不是照抄 UE 文档
- 变更规则：本文件定义的字节布局与句柄编码一旦冻结，修改必须升 formatVersion 并写迁移说明

## 1. 实测前提（决定了哪些能冻结、哪些还不能）

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| compute 队列 + 跨队列 fence | ✅ 可用 | `probes.compute`：240 帧 240 次派发 / 240 次等待 |
| SM 6.6 `ResourceDescriptorHeap` | ✅ 可用 | 索引 32–35 与描述符表采样逐位一致，maxDelta 0 |
| ExecuteIndirect（含 DispatchRays） | ❌ 未实现 | B2 |
| HZB | ❌ 未实现 | B4 |
| visibility buffer + resolve | ❌ 未实现 | B5 |
| 材质主路径 bindless | ❌ 仍是有界 8 槽表 | B3b 需要 DXC 光栅变体 |
| 低端回退 | 890M 核显 | 预算管理器已能降档，但没有几何降档档位 |

**结论**：可以冻结数据格式与句柄；不能冻结剔除策略、HZB 分辨率、软光栅 tile 大小。

## 2. 三个决定

1. **统一数据**：所有几何都进同一套 cluster/instance 布局与同一个材质句柄空间。
2. **统一着色**：两条光栅路径写同一个可见性缓冲/材质解析，光照只写一遍。
3. **分开光栅**：cluster 光栅与传统顶点光栅并存，按 `GeometryClass` 选择，而不是强制唯一。

> 理由：唯一光栅路径会让低模、透明、核显、碰撞四类东西同时变成例外；例外最终会逼你拆架构。详见 `docs/NANITE-LUMEN-PLAN.md` 的取舍讨论。

## 3. GeometryClass 路由

```cpp
enum class GeometryClass : uint32_t {
    Cluster = 0,      // 静态不透明 + 三角形数 ≥ 阈值：cluster 光栅
    Simple = 1,       // 静态不透明 + 低于阈值：顶点光栅（同一可见性缓冲）
    Deformable = 2,   // 蒙皮 / WPO：先顶点光栅，验证后再评估 cluster
    Transparent = 3,  // 半透明 / 镂空双面：前向 + 排序，独立路径
    Effect = 4        // 粒子 / 精灵 / UI：独立管线
};
```

判据（`GeometryHints{ triangles, animated, translucent, twoSided, alphaTest }`）：

| 条件 | 类别 |
| --- | --- |
| `translucent` | Transparent（无论三角形数） |
| `animated` | Deformable |
| `triangles >= kClusterTriangleThreshold` | Cluster |
| 其余 | Simple |

`kClusterTriangleThreshold` 初值 **5000**，必须由预算管理器与图像契约实测调整，不允许拍脑袋改。

**当前场景分布**：所有实体都是 12 三角形方盒 → **100% Simple，0 个 Cluster**。也就是说现在启用 Nanite 只会增加固定开销，没有任何收益。这个数字会写进每次运行的报告（`geometryClasses`）。

## 4. cluster 记录（48 B，16 B 对齐）

```cpp
struct ClusterRecord {
    float    boundsMin[3];
    float    lodError;        // 相对父 LOD 的世界空间误差；LOD0 为 0
    float    boundsMax[3];
    uint32_t materialHandle;  // 见 §7
    uint32_t vertexOffset;    // 指向 cluster 顶点池
    uint32_t indexOffset;     // 指向 cluster 索引池
    uint32_t vertexCount : 8;
    uint32_t triangleCount : 8;
    uint32_t flags : 16;      // bit0 skinned, bit1 wpo, bit2 twoSided,
                              // bit3 alphaTest, bit4 translucent, bit5 destructible
};
```

- 顶点上限 128、三角形上限 128（与离线工具一致）。
- `lodError` 是 LOD 选择唯一依据，禁止用三角形数反推。
- 48 B × 每 LOD 数百 cluster，规模在预算内。

## 5. GPU 工作队列（N1 的输出契约）

```cpp
struct InstanceRef { uint32_t instanceIndex, clusterStart, clusterCount, flags; };  // 16 B
struct ClusterRef  { uint32_t clusterIndex, instanceIndex, lodLevel, pad; };        // 16 B
```

三段 pass：

```
pass1 实例剔除   → InstanceRef[]（写入 append buffer + 间接参数）
pass2 cluster 剔除 → ClusterRef[]（紧凑追加；自包含 instanceIndex）
pass3 光栅       → cluster 光栅 / 软光栅 → 可见性缓冲
```

- `ClusterRef` 必须自包含：resolve 不得反查实例表。
- 间接参数由 compute 写入，CPU 不回读（否则每帧一次同步）。
- 队列容量上限进预算管理器（超限降档而不是丢帧）。

## 6. 可见性缓冲

```
R32_UINT  visibility : (clusterRefIndex << 8) | triangleId
R32_FLOAT depth      : 复用现有 R32_TYPELESS 深度（DSV D32_FLOAT / SRV R32_FLOAT）
```

- 第一版只用 32 位 + 深度，resolve 用重心坐标重算；**不要**一开始上 64 位打包。
- 深度约定：reversed-Z，重建世界坐标的公式一次写死并加测试。
- 两条光栅路径写同一个 buffer，因此 resolve / 光照 / 阴影 / GI 只实现一遍。

## 7. 材质句柄（32 位）

```
handle = (generation << 16) | (sliceIndex & 0xFFFF)
```

- `sliceIndex` = 描述符堆中的 SRV 槽位；`generation` = 槽位代际。
- GPU 侧只做 `ResourceDescriptorHeap[handle & 0xFFFF]`，**不做代际比较**；代际校验在 CPU 分配/提交时完成（现有 `MaterialTable::resolve` 已具备）。
- `handle == 0` 表示默认材质（对应现有槽 0 的品红缺失纹理）。
- 有界 8 槽表继续作为回退：`ResourceDescriptorHeap` 不可用时句柄退化为 0–7。
- 编码/解码在引擎侧单点实现并加单测，禁止各处手写位移。

## 8. Lumen 侧共用

| Lumen 需要 | 挂在哪 | 本文档冻结的部分 |
| --- | --- | --- |
| 远场 SDF | cluster 记录索引 | `ClusterRecord.indexOffset` + L0 产物 `sdf.volume.json` |
| 表面缓存卡片 | cluster 为键 | `materialHandle` + `bounds` |
| 屏幕空间追踪 | 可见性缓冲深度 + 法线重建 | §6 的深度与打包 |
| 辐射探针 | cluster 分组（§4 `lodError` 不参与） | `flags` 预留 bit6+ |

**不在本文件冻结**：probe 网格分辨率、卡片尺寸、降噪方案——这些随 L1–L4 实测确定。

## 9. 版本与迁移（v1 → v2）

| 字段 | v1 | v2 默认 | 说明 |
| --- | --- | --- | --- |
| `formatVersion` | 1 | 2 | v1 仍可读 |
| `lodError` | 无 | 0 | v1 包按 0 处理，LOD 选择退化为"按三角形数" |
| `flags` | 无 | 0 | 全部视为静态不透明 |
| `materialHandle` | 无 | 0 | 默认材质 |

- **digest 会变**：v1 输入重建为 v2 后包摘要不同，缓存键必须包含 `formatVersion`。
- 旧缓存不删除，按版本目录隔离（`build/offline/v2`）。
- 读取端同时接受 1 和 2；写入端只写 2。

## 10. 前置工作包与顺序（N1/L1 的硬门）

| 序号 | 工作包 | 为什么必须先做 | 验收 |
| --- | --- | --- | --- |
| 1 | **B2** ExecuteIndirect（含 DispatchRays） | pass1/pass2/pass3 的间接参数与 draw 都靠它 | 间接参数由 GPU 写、CPU 零回读 |
| 2 | **B4** HZB 生成 | 两阶段遮挡的输入 | HZB 与深度逐像素一致（金字塔误差 < 1e-4） |
| 3 | **B5** visibility buffer + resolve | 两条光栅路径的共同输出 | 同一场景 cluster/顶点两路 resolve 结果一致 |
| 4 | **B3b** 材质主路径 heap 索引 | 句柄要真的被采样 | 材质路径与有界表逐像素一致 |
| 5 | **N1** cluster 剔除 + LOD 选择 | 真正开始用 cluster | 可见 cluster 数 == CPU 参考实现 |
| 6 | **L1** 屏幕 + SDF 追踪 | Lumen 第一条光线 | 与 path-traced 参考在阈值内 |

## 11. 明确排除

- ❌ 每帧重新聚类（变形靠顶点阶段，不靠重建拓扑）
- ❌ 用 cluster DAG 直接做碰撞（碰撞仍需独立简化网格）
- ❌ 半透明走 cluster 路径（必须前向 + 排序）
- ❌ 把 Nanite 当作"唯一光栅路径"（见 §2）
- ❌ 在 resolve 里反查实例表（`ClusterRef` 必须自包含）
