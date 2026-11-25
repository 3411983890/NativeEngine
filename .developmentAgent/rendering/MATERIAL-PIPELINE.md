# 纹理与材质管线（v0.3）

2026-10-23 正确性更新：[材质修复说明](MATERIAL-FIXES-2026-10-23.md)。颜色缩放/mip 改为线性光过滤；参数/复制写入要求 expectedRevision 并进入统一撤销；蒙皮使用模型 UV，普通方盒仍平面映射；直接光改为 GGX；cook 支持逐纹理候选缓存。旧测量不代表本轮修复后的测量。

- 版本：v0.3
- 日期：2026-10-21
- 基线：NativeGPU 0.10.0
- 相关：[TEXTURE-MATERIAL-PLAN.md](TEXTURE-MATERIAL-PLAN.md)、[MATERIAL-PIPELINE-RESEARCH.md](MATERIAL-PIPELINE-RESEARCH.md)

![小镇主视图](images/town-hero.jpg)

## 0. 一句话

PNG / 程序化源图 → 语义校验 → 全 mip → `.ngtex`（RGBA8 / BC1 / BC3 / BC5 / BC7）→ 预算求解 → 每张纹理独立 `Texture2D` + SM 6.6 bindless → 场景 v2 材质表（albedo / normal / ORM / wetness / wear）→ 608 个实体、16 种材质的小镇。legacy 着色器未改；Mesh 路径已验证与光栅一致，但默认仍走光栅以保持图像基线。

## 1. 能力清单

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| PNG 解码、语义校验、通道统计 | 已实现 | `tools/textures.mjs`、`tests/textures.test.mjs` |
| 全 mip 链（法线重归一化） | 已实现 | 9 项纹理单测 |
| `.ngtex` 容器（格式 / sRGB 标志） | 已实现 | `readNgtex` 与 `host/texture_asset.cpp` 双重校验 |
| BC1 / BC3 / BC5 / BC7 编解码 | 已实现 | `tools/bc.mjs`、9 项 BC 单测；BC7 mode5/6 已用 GPU 渲染验证 |
| sRGB albedo + 线性数据贴图 | 已实现 | `DXGI_FORMAT_*_SRGB` + composite gamma |
| 每纹理独立尺寸 / 格式（bindless） | 已实现 | 45 张纹理，64/128/256 混合，`materialBindless=true` |
| 预算 / 质量求解（格式 + 分辨率降档） | 已实现 | 4.5 MiB 预算，11 次降档，`overBudget=false` |
| 场景 v2 材质表 + 字符串引用 | 已实现 | `.screen/town.scene.json`、`host/scene_loader.cpp` |
| albedo / normal / ORM / ORM.a 自发光 | 已实现 | 材质 shader |
| wetness / wear 表面经历 | 已实现 | 材质标量 + shader 耦合规则 |
| clearcoat 清漆层 | 已实现 | 因子与三种独立 UV/采样贴图、表层法线、基础层/发光衰减；`build/asset-import-smoke/clearcoat/acceptance.json` 及 `public/acceptance.json` |
| 介质折射率 IOR | 已实现 | `KHR_materials_ior`、MIR/图/场景/编辑器、直接光和环境反射；`build/asset-import-smoke/ior/acceptance.json` 及 `hot/acceptance.json` |
| MIR v0 + 最小材质图 | 已实现 | `engine/material/mir.mjs`、`graph.mjs`；canonical hash、常量折叠 |
| cook 内容缓存 | 已实现 | 内容指纹命中复用 45 张；`reports/material/cook-cache.json` |
| 材质记录 | 已实现 | `GpuMaterial` StructuredBuffer + `albedoIndex` 记录索引 |
| 材质 CLI / MCP / 编辑器入口 | 已实现 | `tools/material.mjs`、`material_patch`、编辑器材质区 |
| 旧场景兼容 | 已实现 | legacy 着色器未改；庭院 raster 基线已重新接受 |
| Mesh / 光栅材质一致性 | 已验证 | 同一机位 meanAbsError≈0.0002，差异像素≈0.001% |
| 对象空间 UV | 已修复 | 平移整个场景 +5 X，全画面 maxChannelDelta=1；`reports/material/uv-object-space.json` |
| 虚拟纹理 | 未实现 | 见第 10 节 |

## 2. 端到端

```text
tools/town/gen-textures.mjs     程序化 albedo / normal / ORM（256²，固定 seed）
        ↓
assets/source/textures/*.png    <name>.<semantic>.png
        ↓
tools/textures.mjs cook         解码 → mip → 格式选择 → 质量门禁 → 预算求解
        ↓
assets/textures/*.ngtex         45 个容器；当前 4.49 MiB
        ↓
host/texture_asset.cpp          逐纹理校验格式、mip 链、字节数、sRGB
        ↓
renderer/d3d12_renderer.cpp     每张纹理一个 Texture2D + SRV（ResourceDescriptorHeap）
        ↓
scene.materials                 材质表；wetness/wear 与贴图引用
        ↓
material shader (SM 6.6)        平面 UV、法线、ORM、自发光、湿润/磨损、雾
        ↓
composite                       gamma 回写（仅 cooked 路径）
        ↓
截图 + reports/town/evidence.json
```

## 3. 纹理契约

| 语义 | 文件 | 通道 | 解释 |
| --- | --- | --- | --- |
| albedo | `<name>.albedo.png` | RGB | sRGB，硬件解码到线性 |
| normal | `<name>.normal.png` | RGB | 切空间，+Y 向上；BC5 只存 XY，shader 重建 Z |
| orm | `<name>.orm.png` | R=AO，G=roughness，B=metalness，A=emissive | 线性；A 为自发光覆盖 |
| mask/emissive | 预留 | — | 语义码已定义，首版未用 |

`--format auto` 规则：

- albedo / emissive → BC1 sRGB；PSNR < 28 dB 回退 RGBA8 sRGB。`--format bc7` 可切 BC7 sRGB（8 bpp，质量更高、体积约为 BC1 的两倍）。
- normal → BC5；角度 P95 > 2° 或 max > 8° 回退 RGBA8。
- orm → BC3（BC1 颜色 + BC4 alpha）；PSNR < 26 dB 回退 RGBA8。
- mask → RGBA8。

当前小镇：BC1 ×13、BC7 ×2、BC3 ×15、BC5 ×8、RGBA8 ×7；尺寸 256/128/64 混合，mip 链完整。auto 会按 PSNR 提升把少量颜色贴图提升到 BC7，再由预算求解决定是否保留。

## 4. `.ngtex` 容器

v1 头 32 字节，`format`/`flags` 向后兼容旧 RGBA8 文件：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | char[4] | `NGTX` |
| 4 | u16 | 版本 1 |
| 6 | u16 | 语义码（0 albedo、1 normal、2 orm、3 mask、4 emissive） |
| 8 / 10 | u16 | 宽 / 高 |
| 12 | u8 | mip 数 |
| 13 | u8 | 格式：0 RGBA8、1 BC1、2 BC3、3 BC5、4 BC7 |
| 14 | u16 | flags：bit0 = sRGB |
| 16 | u32 | payload 字节数 |
| 20 | u32 | mip 表起始（32 + 8×mips） |
| 24/28 | u32 | 保留 |
| 32 | (u32 offset, u32 size) × mips | 每个 mip 的绝对偏移与字节数 |
| 32+8m | bytes | mip 0 … n-1 紧排 |

BC3 的块布局是 **alpha 块在前、BC1 颜色块在后**；写反会让 GPU 把 alpha 当 RGB 读，表现为 `ao = 0`、阴影全黑。

## 5. 场景 v2：材质与表面经历

```json
{
  "schemaVersion": 2,
  "materials": {
    "cobble": {
      "albedo": "cobble", "normal": "cobble", "orm": "cobble",
      "uvScale": 2.0, "roughness": 1.0, "metalness": 1.0,
      "wetness": 0.35, "wear": 0.10,
      "clearcoat": 0.0, "clearcoatRoughness": 0.1
    }
  },
  "entities": [
    { "id": "…", "name": "Cobbled high street", "kind": "ground",
      "position": [0, 0.02, 2], "scale": [9, 0.08, 78],
      "color": [1, 1, 1], "material": "cobble", "collider": false }
  ]
}
```

- `roughness` / `metalness` 是 ORM 通道乘数；烘焙 ORM 的材质填 1.0。
- `wetness`：压暗 albedo、降低粗糙度，形成湿表面高光。
- `wear`：提高金属度、提高粗糙度，并把 albedo 往暴露基底色偏。
- 两者 0..1，写入 `GpuMaterial` 记录的 `wetness` / `wear` 字段，不再做字节打包。
- `clearcoat`：0 关闭、1 全清漆；`clearcoatRoughness` 控制第二高光，直接光和 IBL specular 都叠一层。
- `coat` / `coatRoughness` / `coatNormal` 是可选独立清漆贴图名，分别读取线性 R/G/法线，支持各自 UV 与采样设置。未提供表层法线时使用几何法线；表层保持 1.5 IOR，并按 Fresnel 衰减基础层及发光。
- `ior`：省略为 1.5，可为 0 或 ≥1 的有限 Float32 数。直接光和环境反射由介质 F0 决定，金属按自身颜色反射，清漆表层的 IOR 仍为 1.5。0 表示 F0=1、无漫反射；透射及体积折射需要各自的运行时支持。导入的 `KHR_materials_ior` 与编辑器数字输入、MIR 的可选 `params.ior`、材质图的 `ior` 标量端口使用同一校验。
- v1 内联材质对象继续兼容。

## 6. 运行时

- **legacy**：无 `materials` 的场景（庭院）继续使用 64×64 有界纹理数组与旧像素着色器；Mesh 路径已验证但默认关闭，`F11` / `--mesh-shaders on` 可切换。
- **cooked**：每张 `.ngtex` 创建独立 `Texture2D`，SRV 位于 `CookedTextureSrvBase + i`；shader 用 `ResourceDescriptorHeap[index]` 读取，尺寸、mip 数、格式不再需要一致。
- 根签名必须带 `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`，否则 `ResourceDescriptorHeap` 管线创建失败。
- 材质 shader 编译为 `vs_6_6` / `ps_6_6`（DXC），DXC 缺失时回退数组路径。
- **光照已接入新系统**：shader 读取 `SceneConstants` 的 `lights[64]`（点光 + 聚光，含车辆前照灯）、`keyDirectionIntensity` / `keyColor`；环境光走夜间 `ambient`，雾、天空、云和 tone mapping 由 composite 统一处理，材质 shader 不再自己叠雾。
- `DrawConstants` 58 个 float，加两项根 CBV 和两项描述表共 64 DWORD。bindless 时 `albedoIndex` 是 `GpuMaterial` 记录索引；352 字节记录容纳最多八张贴图、八套采样变换及材质参数。基础和表层法线索引的 bit31 为 BC5 标志，扩展贴图没有增加根常量。
- **UV 固定在对象空间**：VS 传 `materialPosition = localPosition * modelScale` 和 `materialNormal`；PS 用它构平面 UV / TBN，再经 normalMatrix 转回世界。移动/旋转物体不会让贴图在表面上滑动，`uvScale` 仍按米控制重复。
- cooked 路径 composite 做 gamma 回写；legacy 路径不变。

## 7. 预算求解

`tools/textures.mjs cook --budget-mib N`：

1. 每张纹理生成 256/128/64 三档候选。
2. 候选解码后上采样回 256，与全分辨率源图比较，得到 PSNR 或法线角度。
3. 从最高质量档开始，每次选择“质量损失 / 节省字节”最小的纹理降一档，直到落入预算。
4. `manifest.json` 输出 `budgetBytes`、`totalBytes`、`overBudget`、`decisions[]`。

这是贪心解，不是全局最优；报告如实写明。

## 8. 材质编辑入口

```powershell
node tools/material.mjs list --scene .screen/town.scene.json
node tools/material.mjs preview --scene .screen/town.scene.json --material cobble --wetness 0.6
node tools/material.mjs set --scene .screen/town.scene.json --material cobble --wetness 0.6 --revision <当前版本> --apply
```

MCP `material_patch`：`{scenePath, material, changes, apply}`，返回 before/after、受影响实体数、该材质的三张纹理预算；默认只预览。

Web 编辑器：选中引用命名材质的实体时，检查器出现“材质”区，可调 `wetness` / `wear` / `roughness` / `metalness` / `uvScale`，点“应用材质参数”通过同一个 `material_patch` 动作原子写入并刷新场景。

## 9. 小镇与证据

| 项 | 值 |
| --- | --- |
| 实体 | 608 |
| 材质 | 16 |
| 纹理 | 45（BC1/BC3/BC5/RGBA8 混合） |
| 纹理净负载 | 4,704,876 字节 ≈ 4.49 MiB |
| 预算 | 4.5 MiB，11 次降档，未超 |
| 尺寸 | 256 / 128 / 64 混合 |
| Draw calls | 644 |
| 三角形 | 7,896 |
| 材质记录 | 16 条 `GpuMaterial`（48 B/条） |
| Mesh / 光栅 | meanAbsError 0.0002，差异像素 0.001%（960×540，RT 阴影开） |
| GPU P50 | ≈5–10 ms（AMD Radeon 890M，1600×900，随并发负载波动） |
| 材质路径 | `materialPath=true`、`materialBindless=true`、45 个 SRV |

![街道细节](images/town-detail.jpg)

![俯视布局](images/town-aerial.jpg)

## 10. 限制

1. **BC7 是两模式 RDO**。mode 6 用 4-bit 索引（min/max / PCA / least-squares 端点候选），mode 5 用 2-bit 索引；每块选误差更低者。没有覆盖全部 8 个 BC7 mode，也没有 partition 搜索。
2. **没有虚拟纹理 / 流式加载 / 稀疏驻留**。45 张纹理整包上传。
3. **Mesh 路径默认走光栅**。根因已修复：mesh shader 的 `gTriangles` 是全局顶点索引，必须先减 `desc.vertexOffset` 才是本地输出索引；材质记录也已在 mesh 根签名 `t4` 绑定。`F11` / `--mesh-shaders on` 已验证与光栅一致；默认关闭只是为了保持图像回归基线。
4. **编辑器材质面板已接入**：贴图槽、wetness/wear/roughness/metalness/uvScale、clearcoat 与材质实例都可编辑；复杂材质图仍走 CLI/MIR。
5. **MIR / 材质图是 v0**。MIR 有类型、canonical hash 和 feature bits；图只支持 texture/scalar/multiply/lerp/output 的常量折叠，不是任意 shader DAG。分层 BSDF 目前是 clearcoat + wetness/wear，不是完整闭包树。
6. **IBL / GI / 局部灯阴影已有接入**。quality 分档控制 IBL、DDGI 和局部阴影；先前“未接”的描述已经过时。范围和质量限制见光照文档。
7. **数组回退要求 RGBA8 同尺寸**；bindless 不可用时只能加载统一尺寸、未压缩纹理。

## 11. 命令

```powershell
npm run textures
npm run town
npm run material -- list --scene .screen/town.scene.json
```

手动 cook：

```powershell
node tools/textures.mjs cook --source assets/source/textures --out assets/textures `
  --report reports/textures --size 256 --format auto --budget-mib 4.5
```

## 12. 参考

- [TEXTURE-MATERIAL-PLAN.md](TEXTURE-MATERIAL-PLAN.md)、[MATERIAL-PIPELINE-RESEARCH.md](MATERIAL-PIPELINE-RESEARCH.md)
- [GEOMETRY-LAYOUT.md](GEOMETRY-LAYOUT.md)
- `tools/textures.mjs`、`tools/bc.mjs`、`tools/material.mjs`、`tools/town/*`
- `host/texture_asset.cpp`、`renderer/d3d12_renderer.cpp`、`shaders/material.hlsl`
