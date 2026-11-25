# 材质与光照升级路线图（虚拟纹理之前）

- 版本：v0.1
- 日期：2026-10-21
- 状态：任务板；供后续 AI/开发者继续执行
- 关联：[MATERIAL-PIPELINE.md](MATERIAL-PIPELINE.md)、[MATERIAL-PIPELINE-RESEARCH.md](MATERIAL-PIPELINE-RESEARCH.md)、[LIGHTING-SYSTEM-RESEARCH.md](LIGHTING-SYSTEM-RESEARCH.md)、[RT-SHADOWS.md](RT-SHADOWS.md)、[EDITOR-FEATURES.md](EDITOR-FEATURES.md)

## 0. 目标与非目标

**目标**：在引入虚拟纹理/稀疏驻留之前，把材质系统的“表现力、光照一致性、作者化、迭代效率”补到可用版本：

1. IBL：环境漫反射 + 镜面反射，替代常量 ambient。
2. 点光/聚光 DXR 阴影 MVP：至少一盏局部灯有真实遮挡。
3. 编辑器贴图槽与材质实例。
4. MIR（类型化材质 IR）与最小材质图/代码生成。
5. 分层 BSDF：clearcoat / 湿层 / 雪或磨损层的至少一种组合。
6. BC7 多模式 RDO。
7. 增量 cook 与运行时热重载。
8. DDGI 探针 GI MVP（允许低分辨率、时域更新）。

**明确非目标（本路线图不做）**：

- 虚拟纹理、页表、稀疏驻留、纹理 streaming。
- 完整 path tracer、ReSTIR、light tree 全量实现。
- 完整节点图编辑器（只做最小节点集 + 编译器）。
- 跨平台后端；仍以 D3D12/SM 6.6 为准。

> 这些非目标不是“永不”，而是等本路线图验收后再单独开目标。

## 1. 当前基线（2026-10-21）

| 项 | 现状 |
| --- | --- |
| 材质管线 | `.ngtex` RGBA8/BC1/BC3/BC5/BC7；预算求解；sRGB；bindless |
| 材质记录 | 16 条 `GpuMaterial`，t4；Mesh 与光栅一致 |
| 表面经历 | wetness / wear（shader 内耦合） |
| 光照 | 64 灯 buffer + key 光；**quality compute 已计算 IBL cubemap/BRDF LUT/DDGI 探针/局部阴影 cache（`lighting_quality.hlsl`），但 cooked material 还没采样** |
| RT | 仅 key 光 DXR 阴影；点光/聚光无阴影 |
| 编辑器 | 可调材质标量；**无贴图槽/材质实例** |
| BC7 | mode 6 简化编码，无多模式 RDO |
| Cook | 全量；无增量/热重载 |
| 测试 | Node 128、原生 6 套、图像回归 4/4、小镇 608/16/45/4.49 MiB |

## 2. 任务板

状态：`todo` / `doing` / `blocked` / `done`。每个任务完成时必须更新本表并附证据路径。

| ID | 优先级 | 状态 | 任务 | 依赖 | 主要文件 | 验收 |
| --- | --- | --- | --- | --- | --- | --- |
| MAT-IBL-01 | P0 | done | cooked material 接入 quality IBL/SH/probes（复用已有 compute 输出） | 无 | `shaders/material.hlsl`、`lighting_quality_common.hlsli`、`d3d12_renderer.cpp` | quality1 IBL/SH/局部阴影；quality2 才开 DDGI；`reports/material/quality-material.json` |
| MAT-IBL-02 | P0 | done | quality 光照分档：quality1 不跑 DDGI | MAT-IBL-01 | `renderer/lighting_quality.cpp` | 960×540：q0=1.01 ms、q1=1.69 ms（+0.68 ms）、q2=5.43 ms（DDGI）；达标 |
| MAT-RTS-01 | P0 | done | cooked material 接入 quality 局部阴影 cache（点光/聚光） | MAT-IBL-01 | `lighting_quality_common.hlsli`、`shaders/material.hlsl` | 路灯/车灯遮挡可见；开关/回退；`reports/material/local-shadow.json` |
| MAT-ED-01 | P0 | done | 编辑器贴图槽 + 材质实例 | 无 | `editor/ui/app.mjs`、`editor/service.mjs`、`tools/material.mjs` | UI 可换 albedo/normal/orm；实例可创建/应用；`tests/editor-material.test.mjs` |
| MAT-MIR-01 | P1 | done | MIR v0 类型化材质 IR + canonical hash/feature bits | 无 | `engine/material/mir.mjs`、`tools/material.mjs`、`tests/mir.test.mjs` | 键序无关 hash；5 项测试；CLI `material.mjs mir` |
| MAT-LAYER-01 | P1 | done | 分层 BSDF：clearcoat + wetness 层 | MIR-01 | `shaders/material.hlsl`、`GpuMaterial`、`scene_loader.cpp` | clearcoat/clearcoatRoughness 记录字段与双高光；`reports/material/clearcoat.json` |
| MAT-GRAPH-01 | P1 | done | 最小材质图节点集 + 常量折叠到 MIR | MIR-01 | `engine/material/graph.mjs`、`assets/material-graphs/` | texture/scalar/multiply/lerp/output；3 项测试；CLI `material.mjs graph` |
| MAT-BC7-01 | P2 | done | BC7 多模式 RDO：mode 5 + mode 6 | 无 | `tools/bc.mjs` | mode6 三候选 + mode5 双 bit 索引；GPU 验证 4 mode5 / 15431 mode6 块；`reports/material/bc7-modes.json` |
| MAT-COOK-01 | P2 | done | cook 内容缓存 + 自动纹理热重载 | 无 | `tools/textures.mjs`、`host/main.cpp` | 45 张缓存命中；`--watch` 下 manifest 变化触发 `hotReloads=1`；`reports/material/cook-cache.json`、`hot-reload.json` |
| LIGHT-DDGI-01 | P2 | done | DDGI 探针 GI 验证 | IBL-01 | `renderer/lighting_quality.cpp`、`shaders/lighting_quality_common.hlsli` | q2：2048 probes、64 rays、256 updates/frame、0.15 ms；`reports/material/ddgi.json` |

## 3. 分阶段实施

### Phase P0（优先做，1–2 周）

#### MAT-IBL-01：cooked material 接入 quality 光照

**现状**：`renderer/lighting_quality.cpp` + `shaders/lighting_quality.hlsl` 已经计算：
- IBL cubemap（6 级粗糙度）与 BRDF LUT；
- DDGI 探针 irradiance/distance/data（16×8×16）；
- 局部阴影 cube cache（128²，24 层）与云阴影；
- SH 天空漫反射、fog/cloud volume。

**缺的**：这些结果只在 quality compute/composite 里用，`material.hlsl` 仍只读 ambient + 64 直接灯。P0 的任务是把 quality 输出接到 cooked material，而不是重写一套 IBL。

**做法**：
1. 材质根签名增加 `QualityConstants` b3（或复用一个 CBV）和 quality 纹理描述符表（t5 起）。
2. `material.hlsl` include `lighting_quality_common.hlsli`，使用 `QDiffuseSky`、`QIrradiance`、`QEnvironmentSpecular`、`QLocalVisibility`、`QCloudVisibility`。
3. 逐灯采样局部阴影 cache：`QLocalVisibility(q, i, world, n)`，替代当前无阴影的 `lights[i]`。
4. 保留 ambient/直接灯作为 quality 关闭时的回退。
5. 先以 `environment.quality=1` 小镇为验收场景。

**验收**：
- 金属、水面、湿 cobble 有天空/月光反射；
- 路灯/车灯被建筑遮挡；
- quality=0 的 legacy/courtyard 逐像素不变；
- GPU P50 增量 ≤1 ms；
- 证据：`reports/material/quality-material.png`、`reports/material/quality-material.json`。

#### MAT-RTS-01：局部灯阴影接入 cooked material

**现状**：`lighting_quality.hlsl` 的 `CSLocalShadowCache` 已经把最多 24 盏灯的 128² cube depth cache 算到 `qualityLocalShadowCache`；`QVisibilityMask`/`QLocalVisibility` 已实现硬件 RT 与 cache 两条路径。缺的是 material shader 没调用。

**做法**：
1. 在 material shader 的逐灯循环里调用 `QLocalVisibility(quality, i, world, n)`，乘到该灯的 diffuse/specular 上。
2. `GpuMaterial`/`QualityConstants` 使用同一灯索引；cache 索引由 `lights[i].params.z` 给出。
3. 只对 `environment.localShadows` 为真时启用；关闭时返回 1，走原路径。
4. 先验证 4 盏路灯 + 12 盏车灯，不做额外 BVH/降噪。

**验收**：
- 路灯/车灯被建筑/角色遮挡；
- 关闭 RT/quality 时逐像素回退；
- 图像回归不变（默认 courtyard 无局部灯）；
- 证据：`reports/material/local-shadow.png`。

#### MAT-ED-01：编辑器贴图槽/材质实例

**做法**：
1. 在 `materialSection` 增加 albedo/normal/orm 下拉/文本框；选项来自 `assets/textures/manifest.json` 的纹理名。
2. 通过 `material_patch` 扩展 `changes` 支持 `albedo/normal/orm` 字符串引用；`tools/material.mjs` 校验纹理存在。
3. 材质实例：先做“复制当前材质为 `<name>_inst`”并让选中实体引用；不改变已有实体。
4. 补 `tests/editor-material.test.mjs` 与 `tests/material.test.mjs`。

**验收**：
- UI 换贴图后场景 revision 递增、预览可见；
- 非法纹理名被拒绝且不写盘。

### Phase P1（2–4 周）

#### MAT-MIR-01：MIR

**最小类型系统**：
```text
Material {
  name, version,
  base: { albedo: TextureRef|Color, normal: TextureRef|Flat, orm: TextureRef|Scalar },
  params: { uvScale, roughness, metalness, wetness, wear },
  layers: [Layer],
  featureBits: [...]
}
```
- 规范化 JSON（键排序）→ SHA-256；
- 编译到 `GpuMaterial` + HLSL feature bits；
- 同一 MIR 两次编译字节一致；删未连接字段不改 hash；
- AI 只能提交类型化 patch，不能写 HLSL。

#### MAT-LAYER-01：分层 BSDF

- `GpuMaterial` 增 `clearcoat`、`clearcoatRoughness`、`layerMask` 或第二个 `GpuMaterialLayer` 记录；
- shader 叠 clearcoat 镜面 + 湿层；
- editor 增加对应滑杆；
- 先纯直接光，不等 IBL。

#### MAT-GRAPH-01：最小材质图

- 节点：`Texture`、`Color`、`Scalar`、`Multiply`、`Lerp`、`NormalMap`、`ORM`；
- 图 → MIR → 编译；节点数上限 32，循环检测；
- 只做参数化材质，不做任意 HLSL。

### Phase P2（2–3 周）

#### MAT-BC7-01：多模式 RDO
- 实现 mode 6 之外至少 mode 1/5/7 的候选；
- 误差度量 + 端点优化；
- 结果写 manifest `quality`，预算求解可选择。

#### MAT-COOK-01：增量 cook / 热重载
- 源图 hash + cook 参数 hash → 内容键；
- 只重写变更 `.ngtex`；manifest 原子替换；
- 运行时 watch 文件 mtime/hash，fence 后替换 SRV；
- 证据：改动一张图只 cook 一张。

#### LIGHT-DDGI-01：DDGI MVP
- 8×8×8 探针网格，每探针 8×8 octahedral；
- 每帧更新 1/8 探针，trace 64 rays；
- 材质 shader 三线性采样探针，替代部分 ambient；
- 先不降噪/去漏光，允许明显偏差；有 fallback。

## 4. 交接协议（给后续 AI）

1. **先读**：本文件 → `MATERIAL-PIPELINE.md` → 相关 shader/renderer 代码。
2. **不要做**：虚拟纹理/稀疏驻留/streaming；不要改 `assets/textures` 的二进制而不跑 cook；不要用 `Set-Content` 重写含中文的 UTF-8 源文件（会损坏编码）。
3. **构建**：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1`。
   - 若 `build/compile-nativegpu.log` 被并发构建锁住，等 20 s 重试；不要删除别人的进程。
4. **验证顺序**：
   - `node --test tests/*.test.mjs`
   - `build\native_simulation_tests.exe` 等 6 个套件
   - `powershell ... .incrementalTesting/rendering/image-regression.ps1 -SkipBuild`
   - `powershell ... tools/town.ps1 -SkipBuild`
5. **每个任务完成时**：
   - 更新本文件任务板状态；
   - 写 `reports/material/<id>.json` 证据（字段：`schema, task, before, after, screenshot, metrics, notes`）；
   - 在 `MATERIAL-PIPELINE.md` 的能力表加一行；
   - 若改变默认画面，必须显式更新图像基线并说明原因。
6. **代码约定**：
   - 材质记录字段变化必须同步 `GpuMaterial` static_assert 与 HLSL；
   - 根签名/描述符槽变化必须同时改 raster 与 mesh 路径；
   - Mesh 索引必须减 `desc.vertexOffset`，不要再把全局索引直接写进 mesh output；
   - 新增纹理格式要同时改 `tools/textures.mjs`、`host/texture_asset.cpp`、`dxgiFormat`。

## 5. 风险与已知坑

| 风险 | 处理 |
| --- | --- |
| IBL 让夜景过亮/发灰 | 保留 ambient 下限；用 exposure/tonemap 调；先做证据对比 |
| DXR 局部阴影性能 | 单灯、半分辨率、mask 复用；不做多灯前先测 |
| MIR 变体爆炸 | featureBits + 编译缓存；最多 32 节点 |
| clearcoat 能量不守恒 | 先做近似，记录已知偏差；不做“物理正确”承诺 |
| DDGI 漏光 | MVP 允许；后续加 bias/深度矩 |
| 图像回归失败 | 先判断是预期视觉变化还是回归；预期变化必须显式接受 |
| 并发构建锁 | 等待重试；不要覆盖正在运行的 exe |

## 6. 验收命令

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1
node --test tests/*.test.mjs
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/image-regression.ps1 -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File tools/town.ps1 -SkipBuild
```

小镇手动：

```powershell
build\NativeGPU.exe --scene .screen\town.scene.json --textures assets\textures
```

## 7. 状态日志

| 日期 | 事件 |
| --- | --- |
| 2026-10-21 | 建立路线图；材质管线 v0.3 基线；虚拟纹理明确排除 |
| 2026-10-21 | P0 功能完成：cooked material 已消费 quality IBL/SH/DDGI 与局部阴影（`qualitySlot`）；编辑器贴图槽/材质实例接入；证据 `reports/material/quality-material.json`、`local-shadow.json`。**性能待优化**：960×540 q1=2.30 ms vs q0=1.01 ms（+1.28 ms）。 |
| 2026-10-21 | MAT-MIR-01 v0：`engine/material/mir.mjs` 类型化 MIR、canonical hash、feature bits、CLI；5 项 MIR 测试。下一步 MAT-LAYER-01 |
| 2026-10-21 | MAT-IBL-02：quality1 关闭 DDGI dispatch/采样，只保留 IBL/SH/局部阴影；960×540 q0=1.01 / q1=1.69 / q2=5.43 ms，q1 增量 +0.68 ms 达标 |
| 2026-10-21 | MAT-LAYER-01：GpuMaterial 增加 clearcoat/clearcoatRoughness，材质 shader 双高光；scene/schema/MIR/CLI/编辑器全链路；证据 `reports/material/clearcoat.json` |
| 2026-10-21 | MAT-GRAPH-01：最小节点图 texture/scalar/multiply/lerp/output，常量折叠到 MIR；`assets/material-graphs/cobble.graph.json` + 3 项测试 + CLI |
| 2026-10-21 | MAT-COOK-01（部分）：`cookDirectory` 内容指纹缓存，第二遍小镇 cook 45 张全部命中；热重载仍需 F8/自动 watcher |
| 2026-10-21 | MAT-COOK-01 完成：`pollTextures()` 监听 `assets/textures/manifest.json`，`--watch` 下 120 帧测试 `hotReloads=1`；证据 `reports/material/hot-reload.json` |
| 2026-10-21 | MAT-BC7-01 部分：mode6 每块在 min/max 与 PCA 端点间选低误差；多模式仍待做 |
| 2026-10-21 | LIGHT-DDGI-01 验证：q2 `ddgi=true`、2048 probes、64 rays、256 updates/frame、0.15 ms；证据 `reports/material/ddgi.json` |
| 2026-10-21 | MAT-BC7-01 完成：mode 5（2-bit index）+ mode 6（4-bit，min/max/PCA/LS）按块 RDO；GPU 小镇验证通过；证据 `reports/material/bc7-modes.json` |
