# 体积雾与体积光：并行开发方案

- 版本：v0.1（设计文档）
- 日期：2026-10-21
- 基线：NativeGPU 0.10.0（D3D12、DXR 1.2、compute 队列、粒子体积、深度/阴影 mask、帧预算、图像回归）
- 关系：本文把 [LIGHTING-SYSTEM-RESEARCH.md](LIGHTING-SYSTEM-RESEARCH.md) §3.3 的“体积光/雾”展开成可并行开发的工作包；粒子密度场仍由 [FIELD-PLAN.md](FIELD-PLAN.md) 负责，本文只定义它如何接入统一体积。
- 目标：把**大气体积雾**与**体积光（太阳光轴/局部灯散射）**做成两条可并行、可回退、有证据的线，而不是等 10k 灯全部完成后再补。

## 实现状态（2026-10-21，v0.1 切片）

> 后续实现更新：`lighting.quality=1/2` 已加入 froxel、方向光遮挡光轴、局部灯散射、IBL/DDGI 和云光照，运行代码与验收入口见 [LIGHTING-QUALITY.md](LIGHTING-QUALITY.md)。下列 v0.1 状态记录兼容档的历史基线；V6 粒子统一与 V7 大量灯集成仍未完成。

- **已落地**：64 灯 buffer（点光 + 聚光）、夜间 `lighting` 块（环境/月光/雾/曝光/云参数）、4 辆循环车 + 12 盏动态车灯、屏幕空间体积雾（12 步高度密度积分 + 月光相位）、程序化体积云（14 步云层 ray march + 月亮/星星）、tone mapping。
- **不是 froxel**：当前雾/云在 composite 的全屏 pass 里做，未做 V0 的 froxel 网格、V3 的局部灯 top-K 注入、V2 的太阳/车灯光轴；云是程序化 3D 噪声云层，不是高精度体积云。
- **验收切片**：`reports/town/hero.png`、`reports/town/night-final3.png`；608 实体 / 16 材质 / 45 纹理（BC1/BC3/BC5，4.49 MiB）；AMD 890M 上 GPU P50 约 2.7 ms（1280×720 命令列表）。
- **下一轮**：V0 froxel 底座 + V2 太阳光轴 + V3 局部灯散射；把当前屏幕空间实现作为 `vol-screen` 回退档保留。

---

## 0. 结论先行

1. **体积雾和体积光共享同一套底座**：froxel 网格、密度注入、灯注入、阴影可见性、散射积分、时域/降噪、深度感知合成。底座先行，两条线才能并行。
2. **两条产品线分开验收**：
   - **体积雾**：高度/密度场 + 消光 + 环境/天空入射散射；不要求光轴。
   - **体积光**：太阳/局部灯在雾中的**阴影ed 散射**与光轴；依赖阴影可见性。
3. **并行拆分**：V0 底座 → V1 大气雾、V2 太阳光轴、V3 局部灯散射、V5 诊断/证据 可同时开工；V4 时域、V6 粒子统一、V7 10k 灯/Lumen 集成随后。
4. **10k 灯不能逐灯注入 froxel**：只注入 `volumetric` 标记的灯，每 froxel 取 cluster top-K；静态灯可烘焙成 3D 散射纹理。
5. **首个可交付切片**：V0 + V1 + V2（高度雾 + 太阳光轴），不依赖 10k 灯；预计 2–3 人周。完整 V0–V7 约 11–17 人周。
6. **最值得做的创新**：确定性 froxel、逐灯体积收据、cluster top-K 注入、粒子烟与大气雾统一密度场、RT 阴影光轴、AI 雾参数化。

---

## 1. 范围与定义

| 概念 | 定义 | 本文是否负责 |
| --- | --- | --- |
| 大气体积雾 | 全局/局部密度场，沿视线做消光与入射散射；可有高度衰减、噪声、风 | **是** |
| 体积光 / 光轴 | 太阳或局部灯在雾中的散射，包含阴影可见性、相位函数、god rays | **是** |
| 粒子体积烟 | 现有 F1–F3：粒子 → 96³ 密度/热量 → ray march | 只定义“如何接入统一密度”，不重做 |
| 距离雾 | 材质着色器里的颜色 lerp | 保留为最低档回退 |
| 体积云 | 天空中的云层 | 否（另立） |
| 火/燃烧 | 温度/燃料/化学反应 | 否（FIELD-PLAN） |

### 1.1 质量档位

| 档 | 内容 | 目标硬件 | 备注 |
| --- | --- | --- | --- |
| `vol-off` | 只有距离雾 | 全部 | 当前状态 |
| `vol-analytic` | 解析高度雾 + 天空/环境散射，无光轴 | 全部 | 最便宜，可在材质/composite 做 |
| `vol-fog` | froxel 密度 + 太阳散射，无局部灯 | 核显/低端 | 统一底座的第一档 |
| `vol-shafts` | 上述 + 太阳阴影可见性（光轴） | 独显 | 首个“体积光”档 |
| `vol-full` | + 局部灯 top-K 散射 + 时域/降噪 | 独显 | 与 10k 灯/Lumen 对接 |

每档都必须能截图对比、能回退、有预算数据；默认档位按适配器能力选择，理由写进报告。

---

## 2. 现状核对

| 能力 | 现状 | 位置 | 对本方案的影响 |
| --- | --- | --- | --- |
| 粒子体积烟 | ✅ 96³、48 步、消光/透射、自阴影、热辐射、深度裁剪 | `shaders/particles.hlsl` `PSVolume`、`renderer/particles.cpp` | 可作为密度源接入，但不能替代大气雾 |
| 距离雾 | ✅ 材质里 `saturate((dist-28)/80)*0.45` 颜色 lerp | `shaders/material.hlsl` | 保留为 `vol-off` 回退 |
| 全局密度场 | ❌ | — | 需要新建 |
| Froxel 网格 | ❌ | — | V0 |
| 灯注入 | ❌ | — | V3 |
| 太阳阴影ed 散射 | ❌ | — | V2 |
| 局部灯散射 | ❌ | — | V3 |
| 时域/蓝噪声 | ❌ | — | V4 |
| 体积调试视图 | 部分（粒子） | — | V5 |
| 深度/阴影 mask | ✅ 深度 SRV、`shadowMask` R16 | 渲染器 | 可直接采样 |
| DXR/TLAS | ✅ 1.2 | `rt_shadows.cpp` | 可用于高精度太阳/局部灯可见性 |
| Compute 队列 | ✅ | `compute_pass.cpp` | froxel 注入/积分可用 |
| 帧预算/图像回归 | ✅ | `BUDGET.md`、`IMAGE-REGRESSION.md` | 直接接入 |

---

## 3. 架构

```text
Depth SRV ─┐
ShadowMask ─┤
LightBuffer ─┼─► V0 Froxel clear / params
             │        ↓
             │   V1 密度注入（高度雾 + 局部体积 + 粒子场）
             │        ↓
             │   V2/V3 灯注入（太阳阴影 + cluster top-K 局部灯）
             │        ↓
             │   V4 散射积分（front-to-back, HG phase）
             │        ↓
             │   V4 时域重投影 / 空间滤波
             │        ↓
             └─► V5 深度感知合成（scene * T + L）
```

### 3.1 Froxel 网格

- 视空间 3D 网格：**160×90×64**（1080p）或 192×108×64（1440p）。
- Z 轴指数分布：近处密、远处疏；范围 0.5–64 m；可配置。
- 每 froxel 存：RGB 入射散射 + A 透射率（RGBA16F），或 RGB + 密度/消光。
- 分辨率可半分辨率（80×45×64）作为低档。

### 3.2 Pass 列表

| Pass | 队列 | 输入 | 输出 | 说明 |
| --- | --- | --- | --- | --- |
| V0 froxel clear | compute | 参数 | froxel 网格 | 清空/初始化 |
| V1 density | compute | 高度雾参数、粒子 3D 纹理、局部体积 | froxel 密度 | 每 froxel 采样 |
| V2 sun shadow | RT/compute | 世界坐标、TLAS/阴影图 | froxel 太阳可见性 | 太阳光轴 |
| V3 light inject | compute | cluster 灯列表、阴影 | froxel 灯散射 | top-K |
| V4 integrate | compute | froxel 密度/灯/相位 | froxel RGB/T | 沿 Z 前到后 |
| V4 temporal | compute | 当前/历史 froxel | 稳定 froxel | 重投影/clamp |
| V5 composite | graphics | froxel、深度、scene | 最终颜色 | 深度感知 upsample |

### 3.3 数据结构（设计稿）

```cpp
struct VolumeParams {
    float3 min;          // 世界空间 AABB 最小
    float  extent;       // 立方体边长
    uint3  grid;         // 160,90,64
    float  nearZ, farZ;  // 视空间范围
    float  densityScale; // 全局密度乘数
    float  extinction;   // sigma_t
    float  ambient;      // 环境散射
    float  phaseG;       // HG 各向异性
    uint   flags;        // 太阳/局部灯/时域
};

struct FroxelLight {      // 每 froxel 最多 8–16 条
    float3 position;  float radius;
    float3 color;     float intensity;
    uint   type;      uint shadowSlot;
    uint   flags;     uint pad;
};

struct FogProfile {
    float baseDensity;
    float heightFalloff;
    float groundY;
    float noiseScale;
    float noiseStrength;
    float albedo;      // 单次散射反照率
    float anisotropy;  // HG g
    float3 wind;
};
```

---

## 4. 并行开发拆分

### 4.0 先冻结接口（1 天，所有线的前置）

```cpp
// renderer/volumetrics/contracts.h（设计稿，未实现）
struct VolumeDensitySource;   // 高度雾 / 局部体积 / 粒子场
struct VolumeLightSource;     // 太阳 / cluster 灯
struct VolumeShadowSampler;   // shadowMask / shadow map / RT
struct VolumeGrid;            // froxel 资源与读写
struct VolumeComposite;       // 深度感知合成

// 数据导向的契约（GPU 侧对应 buffer/descriptor，不引入虚函数）
struct DensitySourceDesc {
    uint32_t type;        // 0 height, 1 local box/sphere, 2 particle 3D texture
    uint32_t textureSlot; // 粒子/局部体积纹理，0xFFFFFFFF 表示无
    float3   min; float extent;
    float    densityScale, albedo;
};

struct LightInjectionDesc {
    uint32_t lightBufferSlot;   // GpuLight[]
    uint32_t clusterListSlot;   // 每 cluster 灯索引
    uint32_t maxLightsPerFroxel;// top-K，建议 4–8
    uint32_t shadowMode;        // 0 none, 1 screen mask, 2 shadow map, 3 RT
};

struct ShadowSamplerDesc {
    uint32_t mode;        // 同 shadowMode
    uint32_t resourceSlot;// shadowMask / CSM / TLAS
    float    bias;
    float    maxDistance;
};

struct VolumeCompositeDesc {
    uint32_t depthSlot;
    uint32_t froxelSlot;
    float    nearZ, farZ;
    uint32_t halfRes;     // 0/1
};
```

接口冻结后再并行，避免 V1/V2/V3 各自定义一套密度/灯/阴影。冻结的标志是：字段名、单位、坐标系、资源槽位、pass 顺序都写进 `contracts.h` 并有编译期 `static_assert`。

### 4.1 七条工作流

| 线 | 范围 | 依赖 | 交付 | 验收 | 粗估 |
| --- | --- | --- | --- | --- | --- |
| **V0 底座** | froxel 资源、参数、clear、调试视图、composite 钩子 | 无 | `VolumeGrid` + `VolumeParams` + debug | 可清空/可视化 froxel；不改变 `vol-off` 像素 | 1 周 |
| **V1 大气雾** | 高度密度、噪声/风、相位、天空/环境入射 | V0 | 解析 + froxel 两档 | 与解析参考的透射率误差；无光轴也能看 | 1–2 周 |
| **V2 太阳光轴** | 太阳可见性采样（shadowMask/阴影图/RT）、HG、光轴 | V0 | `vol-shafts` | 光轴方向与 key 光一致；遮挡正确 | 1–2 周 |
| **V3 局部灯散射** | cluster top-K 注入、点/聚光/面光、阴影可选 | V0 + 灯 buffer/cluster（LIGHTING P0/P1） | `vol-full` 局部灯 | 每 froxel ≤K 灯；10k 灯不爆 | 2–3 周 |
| **V4 时域/降噪** | 重投影、蓝噪声、clamp、历史重置 | V1/V2/V3 | 稳定 froxel | 60 帧闪烁指标；cut 后无残影 | 1–2 周 |
| **V5 诊断/证据** | froxel 切片、透射率、光轴 mask、参考对比、CI | V0 | 截图 + JSON | 两次运行 diff=0；超预算可定位 | 1 周 |
| **V6 粒子统一** | 粒子 3D 纹理 → 密度源；避免重复计数 | V0 + V1 | 统一密度 | 烟/雾边界无叠加过亮；查询密度=渲染密度 | 1–2 周 |
| **V7 10k/Lumen** | light buffer/cluster 接入、静态烘焙、DDGI/天空耦合 | V3 + LIGHTING P0/P1 | 城市夜景档 | 10k 灯预算；降档可回退 | 3–4 周 |

### 4.2 依赖图

```text
V0 ──┬── V1 ──┬── V4 ── V5 ── ship
     ├── V2 ──┘
     ├── V3 ────── V7
     └── V6（V0+V1）
LIGHTING P0/P1 ── V3/V7
FIELD F1–F3 ──── V6
```

- **可立即并行**：V1（雾）、V2（太阳光轴）、V5（诊断）在 V0 完成后同时开工。
- **必须等灯**：V3 需要 LIGHTING 的 light buffer/cluster；没有它只能做“少量灯列表”。
- **必须等粒子**：V6 需要 FIELD 的 3D 密度纹理；现有 F1–F3 已具备。
- **收口**：V4 与 V5 一起验收；V7 最后接 10k/Lumen。

### 4.3 并行开发的硬规则

1. **先冻结接口再并行**：密度/灯/阴影/合成四类契约没有冻结，不允许 V1/V2/V3 各写一套。
2. **`vol-off` 必须逐像素不变**：体积 pass 只能作为新增分支；旧场景/旧截图基线不能被静默改动。
3. **每档必须有截图 + JSON + 成本**：不能“口头完成”；沿用现有图像回归与收据格式。
4. **体积线不占 10k 灯关键路径**：V1/V2/V5 不依赖 light buffer；V3/V7 才能在 LIGHTING P0/P1 之后接入。
5. **密度只有一个真相**：大气雾、局部体积、粒子烟都走 `VolumeDensitySource`；不允许“烟一层、雾一层”各算一遍。
6. **降档必须记录**：分辨率/步数/灯 top-K/时域的每次降档都写原因与图像差异。

### 4.4 示例排期（并行）

| 周 | V0 | V1 | V2 | V3 | V4 | V5 | V6 | V7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | ● |  |  |  |  |  |  |  |
| 2–3 | ● | ● | ● |  |  | ● |  |  |
| 4–5 |  | ● | ● | ● | ● | ● | ● |  |
| 6–7 |  |  |  | ● | ● |  | ● |  |
| 8–11 |  |  |  |  |  |  |  | ● |

粗估合计 **11–17 人周**；首个可交付（V0+V1+V2）约 **2–3 人周**。

### 4.5 首个可交付切片（V0 + V1 + V2）

- froxel 160×90×64，指数 Z，范围 0.5–64 m。
- V1：解析高度雾 + froxel 注入，HG 相位，天空/环境入射。
- V2：太阳可见性（优先 CSM/shadow map，screen mask 作为近场回退），光轴。
- V5 最小诊断：froxel 切片、透射率、光轴 mask。
- **不做**：局部灯散射（V3）、时域（V4）、粒子统一（V6）、10k 接入（V7）。
- 门禁：均匀密度解析对比、光轴方向与 key 光一致、`vol-off` 像素不变、1080p ≤1.5 ms、两次运行 diff=0。

这个切片可以在 10k 灯方案落地前完成，先把“空气感 + 太阳光轴”做出来。

---

## 5. 技术细节

### 5.1 密度模型

**解析高度雾**（`vol-analytic` 与 froxel 的基准）：

```text
density(y) = baseDensity * exp(-heightFalloff * max(0, y - groundY))
           * (1 + noiseStrength * fbm(world.xz * noiseScale + wind * time))
```

- 沿视线的解析积分可闭式求出，适合低档/远场。
- froxel 档把同一函数注入 3D 网格，保证两档观感一致。

**局部体积**：球/盒密度，按 SDF 或 AABB 衰减；与高度雾相加后 clamp。

**粒子场**：采样现有 96³ 密度纹理，只在其 16 m 盒子内生效；与大气密度用 `max` 或显式权重合并，避免双重计数。

### 5.2 灯注入

**太阳**：

- 低档：屏幕空间 `shadowMask`（现有 R16）在可见表面附近采样；深处不准确，但便宜。
- 中档：CSM/太阳 shadow map 按世界坐标采样。
- 高档：沿光线的 RT 阴影（每条 froxel 采样 1 条短射线），与现有 DXR 共用 TLAS。

**局部灯**：

- 从 cluster 列表取每 froxel 的 top-K（K=4–8），按 `power × attenuation × phase` 排序。
- 只注入 `flags & volumetric` 的灯；普通小灯不参与散射。
- 阴影：只对 top-K 中最重要的 1–2 盏追射线/查阴影图；其余用无阴影近似。
- 10k 灯：不为每灯建资源；静态灯可烘焙成 3D 散射纹理，动态灯走 top-K。

### 5.3 相位函数

Henyey-Greenstein：

```text
p(cosθ) = (1 - g²) / (4π (1 + g² - 2g cosθ)^(3/2))
```

- 太阳 g≈0.6–0.8（前向散射，光轴明显）；雾 g≈0.0–0.3。
- 双 lobe HG 近似多次散射；或加一个各向同性环境项。
- 没有相位函数就会“整片均匀发亮”，没有光轴方向感。

### 5.4 散射积分与合成

```text
for each froxel along Z (front to back):
    sigma_t = density * extinction
    T_step  = exp(-sigma_t * dt)
    L += T * (1 - T_step) * (sigma_s/sigma_t) * (sun + local + ambient)
    T *= T_step
```

- 前到后累积，early-out 当 `T < 0.01`。
- 合成：`scene = scene * T + L`；深度感知 upsample（bilateral）。
- 顺序：不透明之后、透明之前；粒子烟单独一层，避免与大气雾互相遮挡错误。

### 5.5 时域与降噪

- froxel 网格重投影（世界坐标/深度）到上一帧；clamp 历史。
- 蓝噪声抖动的步长/相位；固定序列可回放。
- 相机 cut、场景 revision 变化、froxel 范围变化时清空历史。
- 低档可以不做时域，只做空间滤波；高档才开。

### 5.6 与 10k 灯/Lumen 的接口

- 灯注入读 `GpuLight`/cluster 列表；只读 top-K，不遍历 10k。
- 静态灯烘焙：把静态灯的散射贡献烘到 3D 纹理，运行时只加动态灯。
- 天空/DDGI：体积雾的环境项取天空颜色/球谐/DDGI 探针；Lumen 的 radiance cache 可作为高档环境项。
- 体积雾不参与 ReSTIR；它是在直接光/GI 之后的独立 pass。

### 5.7 确定性

- 固定蓝噪声序列、固定步数、固定 top-K 顺序。
- froxel 积分是逐格确定性的；时域重投影用固定历史规则。
- 收据记录：网格尺寸、步数、相位 g、灯 top-K、历史长度、种子。

---

## 6. 质量、性能与预算

### 6.1 成本估算（1080p，独显）

| 项 | 估算 |
| --- | --- |
| Froxel 160×90×64 = 921,600 格 | — |
| 密度注入 | 0.1–0.2 ms |
| 太阳可见性（shadow map/RT） | 0.2–0.5 ms |
| 局部灯 top-K 注入 | 0.2–0.6 ms |
| 散射积分（32 步） | 0.4–0.8 ms |
| 时域/空间滤波 | 0.2–0.4 ms |
| 合成 | 0.1–0.2 ms |
| **合计** | **1.2–2.7 ms** |

低档（半分辨率、16 步、无局部灯）可压到 **0.5–1.0 ms**。

### 6.2 内存

| 资源 | 估算 |
| --- | --- |
| Froxel RGBA16F | 921,600 × 8 B ≈ 7.4 MB |
| 历史 froxel | 7.4 MB |
| 每 froxel 灯列表（K=8） | 921,600 × 8 × 16 B ≈ 118 MB（太大） |
| 改为每 cluster 灯列表 + froxel 索引 | 2–8 MB |
| 静态灯烘焙 3D 纹理（64³） | 64³ × 8 B ≈ 2 MB |

**结论**：不要给每个 froxel 存完整灯列表；用 cluster 列表 + 每 froxel 的 top-K 索引（或直接存 8 个灯 ID）。

### 6.3 预算阶梯

- 超预算：降 froxel 分辨率 → 降步数 → 关局部灯 → 关时域 → 回退 `vol-analytic`。
- 每级都记录原因与图像差异；不能静默降档。

---

## 7. 验收门禁

| 检查 | 方法 | 门限（建议） |
| --- | --- | --- |
| 透射率正确 | 均匀密度解析解 vs froxel | 相对误差 < 2% |
| 入射散射 | 参考路径追踪（雾中单次散射） | relMSE < 阈值 |
| 光轴方向 | 太阳方向与 key 光一致 | 固定机位截图 + mask |
| 阴影正确 | 建筑遮挡后的光轴消失 | 与 RT 参考对比 |
| 无双重计数 | 粒子烟 + 大气雾边界 | 亮度不跳变 |
| 时域稳定 | 60 帧闪烁指标 | 低于阈值；cut 后无残影 |
| 成本 | GPU P50/P95 | 1080p ≤ 2.7 ms（full） |
| 确定性 | 两次运行 diff | 0 |
| 回退 | `vol-off`/`vol-analytic` | 像素与旧版一致 |

---

## 8. 创新点

### V-I1. 确定性 froxel volumetrics

固定蓝噪声、固定步数、固定 top-K 顺序、历史可重置；让体积雾/光也能进 diff=0 回归。多数引擎接受噪声/时域抖动，本引擎可以把“可回放”做成差异点。

### V-I2. 逐灯体积收据

每盏灯在体积里的贡献、覆盖 froxel 数、散射能量、GPU 时间分摊；预算器按收据决定是否让某盏灯参与体积散射。比“整帧雾成本”更可解释。

### V-I3. Cluster top-K froxel 注入

不遍历 10k 灯；每 froxel 从 cluster 列表取 top-K，按功率/衰减/相位排序。与 10k 灯方案共享 light buffer/cluster，避免为体积另建一套灯结构。

### V-I4. 粒子烟与大气雾统一密度场

粒子 3D 纹理作为 `VolumeDensitySource` 之一；大气雾、局部体积、烟共用一次积分，避免“烟一层、雾一层”的重复采样和排序错误。

### V-I5. 解析 + froxel 混合

远场/低档用解析高度雾，近场/高档用 froxel；两者用同一密度函数，切换无跳变。比纯 froxel 省，比纯解析多了光轴。

### V-I6. RT 阴影光轴

太阳可见性直接用现有 DXR 阴影（或 CSM 回退），不新建一套 shadow map；在光轴边缘用 RT 硬阴影 + 时域抖动软阴影。

### V-I7. AI 雾作者化

自然语言 → 类型化 `FogPatch`（高度、密度、噪声、风向、相位、光轴强度），预览+对比后提交；和 10k 灯/AI 灯光共用编辑闭环。

### V-I8. 体积与 GI/天空耦合

雾的环境项来自天空/DDGI/Lumen radiance cache；雾的多次散射用双 lobe HG 近似；避免“雾是均匀灰色”的假感。

### V-I9. 时域蓝噪声 + 重投影

固定序列、深度重投影、clamp；低档空间滤波，高档时域累积。目标是“没有鬼影的稳定光轴”。

### V-I10. 体积证据门禁

froxel 切片、透射率、光轴 mask、参考路径追踪对比、成本/内存；每次改动都有截图和 JSON，不靠“看起来对”。

---

## 9. 风险与反模式

1. **全屏逐像素 ray march**：1080p × 64 步 = 1.3 亿次采样，直接爆；必须 froxel/半分辨率。
2. **给每 froxel 存 10k 灯列表**：内存 100+ MB；用 cluster + top-K。
3. **粒子烟和大气雾各算一遍**：双重计数、排序错误；统一密度源。
4. **雾在 tonemap 之后叠加**：颜色/能量不对；必须在 HDR 线性空间合成。
5. **没有相位函数**：光轴没有方向感，整片雾均匀发亮。
6. **只用屏幕空间 shadowMask 做体积阴影**：深处/屏外不准确；需要 shadow map/RT 回退。
7. **时域不 clamp**：光轴拖影/鬼影。
8. **透明排序**：雾在透明前/后要明确；粒子烟单独层。
9. **静默降档**：必须记录原因与图像差异。
10. **把体积光塞进 ReSTIR**：职责混乱；体积是独立的散射 pass。

---

## 10. 参考

**本仓库**

- [LIGHTING-SYSTEM-RESEARCH.md](LIGHTING-SYSTEM-RESEARCH.md)：10k 灯架构、Lumen 路线、§3.3 体积层。
- [FIELD-PLAN.md](FIELD-PLAN.md)：粒子密度场 F1–F3、体积渲染、F4–F7。
- [RT-SHADOWS.md](RT-SHADOWS.md)：现有 DXR 单射线阴影。
- [BUDGET.md](BUDGET.md)、[IMAGE-REGRESSION.md](IMAGE-REGRESSION.md)：预算与图像门禁。
- `shaders/particles.hlsl` `PSVolume`、`renderer/particles.cpp`。

**外部**

- B. Wronski, *Volumetric Fog: Unified Compute-Based Solution for Atmospheric Scattering*（SIGGRAPH 2014）
- S. Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique*（2020）
- UE5 *Volumetric Fog* 官方文档
- Frostbite / Decima 的体积雾与光轴演讲（作为工业对照）
- Henyey & Greenstein, *Diffuse Radiation in the Galaxy*（1941）

---

## 附：一句话回答

**体积雾和体积光共用一套 froxel 底座**：V0 冻结接口，V1 大气雾、V2 太阳光轴、V5 诊断并行；V3 局部灯散射等 10k 灯的 light buffer/cluster，V4 时域收口，V6 把粒子烟并入统一密度，V7 接 Lumen/GI。首个切片是“高度雾 + 太阳光轴”，2–3 人周；完整 11–17 人周。
