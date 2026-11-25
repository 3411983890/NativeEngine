# NativeGPU / AI 原生游戏引擎

版本 **0.11.0**。一个可运行、可验证的 Windows D3D12 原生游戏样板，以及供 Web 编辑器和 AI Agent 共用的确定性场景事务层。当前重点不是宣称“已经完成 AI 引擎”，而是把一条真实闭环做通：

> 读取场景 → 预览事务 → 原子提交 → 运行中的原生编辑器热重载 → 原生冒烟测试 → 返回可审计报告

## 现在能做什么

- **外部资产导入**：`npm run assets -- import --in model.glb` 接入模型、材质贴图、角色动作、音频及自有特效/布料/水体库；`append-animation --in walk.bvh --target <角色包>` 追加独立动作并重定向。模型、角色和动作支持统一单位/轴向覆盖；角色支持代理拟合、脚底锚点与控制器体型自适应。图片含 DDS（BC1–5/BC7）、KTX2（RGBA/BC/ETC1S/UASTC）、Basis、TGA/BMP，独立贴图可预览并绑定场景材质，支持逐像素排序的半透明混合、顶点颜色、无光照材质、彩色发光、颜色因子、发光强度和独立 AO 贴图/强度；多套源 UV 支持各贴图独立选择、变换、边界和过滤设置。编辑器提供导入、动作追加、预览、模型/角色/音频入场景和玩家替换入口，支持 64 个独立角色实例、声音实体跟随、场景整体导入与依赖打包；原生 `--asset` 加载校验后的包。支持矩阵、公开角色验收和剩余工作见 [EXTERNAL-ASSETS.md](docs/EXTERNAL-ASSETS.md)。按用户要求仅做增量验证，入口 `npm run assets:verify`。

- **效果资产与原生精灵预览**：效果校验/cook、hash 锁定依赖、固定 120 Hz CPU 多实例参考，以及 GPU 非受光精灵的加法/预乘/裁切绘制。构建后运行 `build/NativeGPU.exe --effect sprite_demo --effect-count 4`；`npm run effects:runtime -- -SkipBuild` 生成数值、截图与重建证据。通用 GPU 模拟、完整共享场、布料、水和创作工具仍在开发，见 [开发记录](docs/SIMULATION-DEVELOPMENT.md)。

- **共享场参考**：CPU 区域租约、来源事务、LEF/WF 投影与观察策略；D3D12 稀疏只读快照和跨砖块定点插值，支持带预算的邻接读取。`npm run fields`、`npm run fields:gpu`、`npm run fields:adapters` 分别验证 CPU、GPU 和布料风/水查询代理。GPU 写求解与游戏宿主仍未接入，见 [场参考契约](docs/FIELD-REFERENCE.md)。
- **通用 GPU 粒子池**：独立 D3D12 SoA、存活/空闲列表、紧致回收与 GPU 计数间接派发，支持五种发射形状和曲线/力模块；`npm run effects:gpu` 生成实机证据。效果实例管理器和宿主尚未切换到这个后端，见 [GPU 池参考](docs/EFFECT-GPU-REFERENCE.md)。

- **非机器学习动作 M1–M4**：传统动作匹配、程序化双骨 IK / 脚接触、GPU morph / 规则肌肉、局部 XPBD 软组织，接入默认角色与完整存档回放。`npm run animation` 生成原生 / GPU / 严格回放证据；原创夹具与能力限制见 [docs/ANIMATION-M1-M4.md](docs/ANIMATION-M1-M4.md)。面部仍为后续计划。

- **《雾镇重启》第一人称示例**：`npm run fogtown` 启动五区域短篇冒险，包含鼠标视角、中文 HUD、视线交互、推箱双门、受控爆破、顺序供能、45 秒中继挑战、检查点与完整结局。`npm run fogtown:tour` 加载完整导览检查点；`npm run fogtown:verify` 验证通关、恢复、重试与固定输入复现；`npm run fogtown:package` 生成独立运行包。内容和限制见 [samples/fogtown/README.md](samples/fogtown/README.md)。

- **可扩展玩法运行时**：类型组件、Lua 5.4.9 行为、显式状态、事件/触发/计时器、事务错误隔离、热重载迁移、完整模拟存档与内容锁定回放。现有钥匙/门/终点由脚本实现；场景工具和 AI 共享字段契约。运行 `npm run gameplay` 验证，见 [docs/GAMEPLAY.md](docs/GAMEPLAY.md)。

- **真实刚体物理与物体破坏（0.11.0 新增）**：120 Hz 固定步长、15 轴 SAT 盒体接触、顺序冲量、摩擦/恢复/休眠、自适应子步防穿透；投射物、径向爆炸、冲量驱动的伤害、盒式预碎裂、142 块碎片的碰撞与阴影，以及“打掉底层 → 上层失去支撑 → 结构坍塌”的支撑图。破坏状态进入确定性摘要和 replay v3；`npm run physics` 生成前后截图、性能与确定性证据。方案、论文对照、创新点与限制见 [docs/PHYSICS-DESTRUCTION.md](docs/PHYSICS-DESTRUCTION.md)。
- **粒子场与体积烟**：`--particles on` 起一条 GPU 粒子管线——65536 个结构化粒子在 compute 队列积分，用整数原子加写入 96³ 的密度/热量体积，再经浮力、半拉格朗日平流、涡量约束与 20 次 Jacobi 压力投影的欧拉求解器，最后由全屏 ray march 合成；整数 splat 与顺序无关，求解器本身不承诺确定性。`.incrementalTesting/simulation/particles.ps1` 跑三遍并断言报告字段与图像差异。
- **篝火场景与爆炸（新增）**：`--scene .screen/campfire.scene.json --particles on --fire` 渲染火塘、柴堆与围石，`--explode-at N` 在第 N 个粒子帧起爆——源半径与外抛速度放大，并向速度场注入一次径向冲击把已有的烟推开。`.incrementalTesting/rendering/campfire.ps1` 校验场景并对比稳态火与爆炸两帧。方案与未实现项见 [docs/FIELD-PLAN.md](docs/FIELD-PLAN.md)。
- **纹理与材质管线 + 小镇（新增）**：PNG → mip → `.ngtex`（RGBA8/BC1/BC3/BC5/BC7）→ 预算求解 → SM 6.6 bindless 独立纹理 → 场景 v2 材质表（albedo/normal/ORM、wetness/wear、自发光）；`npm run town` 一键生成并截图 608 个实体、4.5 MiB 纹理预算的“石桥小镇”。`material_patch` MCP 工具支持预览/应用材质修改，Web 编辑器有材质参数面板。后续任务板见 [docs/MATERIAL-ROADMAP.md](docs/MATERIAL-ROADMAP.md)。见 [docs/MATERIAL-PIPELINE.md](docs/MATERIAL-PIPELINE.md)。
- **光照系统方案（新增）**：10k 光源的完整设计——cluster 分箱、light tree/ReSTIR DI、DDGI、面光/自发光、天空/IBL、逐灯收据，以及 Lumen-like 分档路线。见 [docs/LIGHTING-SYSTEM-RESEARCH.md](docs/LIGHTING-SYSTEM-RESEARCH.md)。
- **体积雾与体积光（并行方案）**：froxel 底座 + 高度雾 + 太阳光轴 + 局部灯 top-K 注入；V0–V7 并行工作流、接口契约、验收门限与性能预算。见 [docs/VOLUMETRICS-PLAN.md](docs/VOLUMETRICS-PLAN.md)。
- **音频系统（A0–A3 已实现）**：固定 48 kHz 混音图、6 总线树、无锁采样精确命令环、64 物理 + 1024 虚拟 voice、距离衰减 / VBAP 7.1.2 声像 / 多普勒 / 程序化 HRTF / 遮挡射线、WASAPI 共享模式事件驱动后端，以及场景 `audio` 块驱动的发射器与触发路由。`npm run audio` 跑 40 项混音器检查 + 52 项宿主检查；A4–A9（Atmos 元数据流、ADM 导出、SOFA、音频图、编辑器面板、`audio_patch`）见 [docs/AUDIO-PLAN.md](docs/AUDIO-PLAN.md)。
- **光照质量实现**：局部灯独立阴影、froxel 光轴、天空 IBL、一次反弹 DDGI，以及带自阴影/投影/时域重建的体积云；质量档与复现验收见 [docs/LIGHTING-QUALITY.md](docs/LIGHTING-QUALITY.md)。
- **Web 编辑器 + 原生引擎**：Electron 桌面界面与本机浏览器界面共用场景服务，支持实体选择、属性草稿、差异预览、提交、撤销和原生窗口控制；二维布局与按需原生快照明确区分，尚无内嵌实时视口。见 [docs/EDITOR.md](docs/EDITOR.md)。
- **原生第三人称样板**：D3D12 交换链、深度测试、固定 120 Hz 物理（走跑跳、扫掠碰撞、拾取钥匙、开门、到达终点）。
- **材质与灯光**：3 槽有界材质纹理表（checker/brick/metal）、粗糙度/金属度、最多 4 盏场景点光；场景 JSON 直接携带材质和灯光。
- **DXR 加速结构与静态阴影**：真实创建 BLAS/TLAS（本机 33 个方盒实例），用固定 DXC 编译 ray generation / miss / closest-hit 库并创建 RTPSO 与 SBT，逐像素追踪一条硬阴影光线；`F10` 或 `--rt-shadows on/off` 切换，关闭时与纯光栅图像逐像素一致。细节见 [docs/RT-SHADOWS.md](docs/RT-SHADOWS.md)。
- **确定性场景工具**：校验、查询、预览、提交、撤销、崩溃恢复、内容寻址构建；失败不会半写入，旧 revision 会被拒绝。
- **运行中热重载**：`scene apply` 写入成功后，原生编辑器自动加载新 revision；无效写入会保留上一有效场景并报告错误，不弹模态框阻塞运行。
- **AI 工具入口**：本地 MCP stdio 服务暴露 8 个工具，Agent 可独立调用，不需要聊天壳或模型凭证。
- **可重复验证**：Node 测试、原生物理/破坏测试、原生冒烟、S1 探针、热重载集成测试和 MCP 协议冒烟都在 `.incrementalTesting/core/verify.ps1` 中执行。
- **DXR 静态阴影证据（0.4.0 新增）**：`.incrementalTesting/rendering/rt-shadows.ps1` 用固定机位与固定分辨率跑开/关两遍，保存 final/mask/key 截图、逐像素对比并断言报告字段，23 项检查全部通过。
- **可重放工程基线（0.3.1 新增）**：Git 版本冻结、固定 DXC 1.9.2609 与哈希、`.incrementalTesting/core/ci.ps1` 一条命令从干净状态重放全部检查、证据按 `<version>/<commit>/<timestamp>` 归档且不覆盖历史。

## 快速开始

要求：Windows 11 x64、支持 D3D12 的显卡、Node.js 22+、PowerShell。首次构建会下载项目内的 LLVM-MinGW 工具链与固定版本 DXC，不修改系统环境。

```powershell
# 0. 工具链 bootstrap（首次；也可让 build.ps1 自动触发）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap-dxc.ps1

# 1. 构建原生程序与测试（会先用 DXC 编译并内嵌着色器）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1

# 2. 启动交互式样板
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1

# 2.5 物理与破坏证据（前后截图、确定性重复、物理步预算）
npm run physics

# 3. 在另一个终端提交一次场景修改，运行中的窗口会自动热重载
node tools/scene.mjs apply .screen/courtyard.scene.json ai/example-transaction.json

# 4. 运行 S1 探针（材质表、场景灯光、DXR 加速结构）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/probe.ps1

# 5. 完整验证（会启动隐藏窗口做原生冒烟与探针）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/verify.ps1

# 6. 从干净状态重放 CI（构建、测试、探针、热重载、MCP、打包）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/ci.ps1 -Clean

# 7. 生成可分发包和 zip
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package.ps1
```

交互按键：`WASD` 移动、`Shift` 跑、`Space` 跳、`E` 交互、`F` 投掷重弹、`G` 在前方引爆、右键拖动或方向键转镜头、`F5` 重置玩法（恢复所有破坏）、`F6` 暂停、`F8` 重建渲染器、`F9` 开关热重载、`F10` 开关 DXR 阴影、`F11` 切换 Mesh Shader 路径。

### 启动 Web 编辑器

```powershell
npm install
npm run editor                 # Electron 桌面编辑器
# 或 npm run editor:web        # 本机浏览器版本，打开终端输出的地址
```

原生预览和快照使用 `build/NativeGPU.exe`；缺失时先执行 `npm run build:native`。场景属性先保存为界面草稿，预览并提交后才写入文件；原生预览在独立窗口中热重载已提交场景。安装 Electron 不需要编译 Chromium 源码。具体流程和边界见 [docs/EDITOR.md](docs/EDITOR.md)。

## 场景事务

唯一事务格式：

```json
{
  "projectId": "a1e00000-0000-4000-8000-000000000001",
  "expectedRevision": 0,
  "commands": [
    {
      "op": "set",
      "entityId": "a1e00000-0000-4000-8000-000000000030",
      "changes": { "position": [-4, 0.6, 1] }
    }
  ]
}
```

命令支持 `create`、`set`、`remove`。整批命令先应用到副本，全部验证通过后才提交一次，revision 只增加一次。项目不匹配、旧 revision、未知字段、重复 UUID、非有限数值、批后不满足“恰有一个玩家 + 至少一个地面”都会拒绝。完整字段和范围见 [assets/scene.schema.json](assets/scene.schema.json) 和 [ai/README.md](ai/README.md)。

保存与恢复：

- `.history.json` 保留最多 100 步线性撤销；撤销后 revision 继续增加。
- `.snapshot.json` 保留上一有效场景；`.pending.json` 记录中断提交。
- `.lock` 阻止同一文件并行写入；崩溃遗留锁需确认进程停止后手动移除。
- `node tools/scene.mjs recover <scene>` 完成已提交的中断写入，或恢复最后有效快照。

## AI / MCP 接口

本地 MCP 服务只使用 stdio，不监听网络端口：

```powershell
node ai/mcp-server.mjs
```

工具：`scene_validate`、`scene_query`、`scene_preview`、`scene_apply`、`scene_undo`、`scene_build`、`runtime_smoke`、`engine_probe`。

在支持 MCP 的本地客户端中注册（Codex 的配置格式可能随版本变化，以下为常见形式）：

```toml
[mcp_servers.nativegpu]
command = "node"
args = ["ai/mcp-server.mjs"]
cwd = "C:\\Users\\34119\\Documents\\AI-Native Game Engine"
```

也可以直接测试协议：

```powershell
'{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | node ai/mcp-server.mjs
```

路径策略：场景只能位于 `assets/`，构建输出只能位于 `outputs/`，冒烟报告只能位于 `reports/`；拒绝 `..`、根目录外绝对路径和符号链接逃逸。工具不接受任意 shell 文本。

## 目录

```text
host/             Win32 宿主、场景加载、热重载与冒烟报告
renderer/         D3D12 渲染器（交换链、描述符、Fence、时间戳、DXR 加速结构）
shaders/          HLSL 源码与 shaders/manifest.json（DXC 编译清单）
engine/native/    固定步长玩法、碰撞、角色姿态
engine/scene/     场景校验、事务、保存/撤销/恢复、内容寻址构建
ai/               MCP 工具服务与工具契约说明
editor/           Web 编辑器、Electron 宿主与本机场景/原生进程服务
assets/           样板场景与 JSON Schema
tests/            Node 事务/工具测试与原生测试
tools/            构建、bootstrap、CI、运行、验证、打包、场景 CLI
docs/             当前阶段状态、工具链冻结表与 CI/证据说明
.github/          windows-latest CI 工作流（只调用 .incrementalTesting/core/ci.ps1）
```

## 工程基线（0.3.1）

- **Git**：`main` 分支，基线 tag `v0.3.0`，之后每个工作包一个提交并打 tag；`build/`、`reports/`、`outputs/`、`work/`、`.tools/` 都在 `.gitignore` 中，不进仓库。
- **CI**：`.incrementalTesting/core/ci.ps1` 是唯一入口，本地与 `.github/workflows/windows.yml` 调用同一脚本；失败保留 `logs/` 与 `ci-failure-<commit>-*.log`。
- **DXC 冻结**：`tools/bootstrap-dxc.ps1` 固定 DXC 1.9.2609 与 SHA-256，编译产物内嵌进可执行文件；缺失时报告 `dxcUnavailable`，不会静默假装实现。
- **版本化报告**：`reports/<version>/<commit>/<timestamp>/` + `reports/latest/` 别名，`run.json` 记录 commit、脏树状态、Node、适配器、DXC、场景哈希与每个产物的 SHA-256。
- 详细版本表见 [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md)，证据布局与重放命令见 [docs/CI.md](docs/CI.md)。

## 当前阶段与边界

| 能力 | 状态 | 证据 / 说明 |
| --- | --- | --- |
| D3D12 原生基线 | 已完成 | 设备、交换链、深度、纹理、Fence、时间戳、冒烟报告 |
| 可玩小院样板 | 已完成 | 走跑跳、碰撞、钥匙/门/终点，原生测试覆盖 |
| 确定性场景事务 | 已完成 | Node 测试覆盖冲突、原子性、撤销、恢复、构建 |
| 热重载 | 已完成 | `verify.ps1` 集成测试检查 revision 与 `hotReloads` |
| 材质表与场景灯光 | 已完成 | 3 槽纹理数组、粗糙度/金属度、最多 4 盏点光；探针 `implemented=true` |
| DXR 加速结构 | 已完成 | 单位方盒 BLAS + 33 实例 TLAS，每帧按刚体变换重建 |
| DXR 静态阴影 | 已完成（0.4.0） | DXC `lib_6_5` + RTPSO + SBT + 可见性贴图；`F10` 切换，关闭走纯光栅回退；23 项证据检查 |
| 刚体物理 | 已完成（0.11.0） | 盒体 SAT / 顺序冲量 / 摩擦 / 恢复 / 休眠 / 自适应子步；`native_physics_tests` |
| 破坏与结构坍塌 | 已完成（0.11.0） | 冲量伤害、142 块碎片、支撑图级联、投射物/爆炸、replay v3；见 [docs/PHYSICS-DESTRUCTION.md](docs/PHYSICS-DESTRUCTION.md) |
| 物理证据门禁 | 已完成（0.11.0） | `.incrementalTesting/simulation/physics.ps1`：前后截图差异、重复摘要一致、166 体平均约 0.9 ms |
| AI 工具入口 | 已完成 | MCP stdio，8 个工具，协议冒烟测试 |
| 版本冻结与 CI（0.3.1） | 已完成 | Git tag `v0.3.0`、固定 DXC 1.9.2609、`.incrementalTesting/core/ci.ps1` 可本机重放、版本化证据目录 |
| Mesh Shader / Meshlet | 已完成（0.6.0） | AS+MS+PS（stream PSO）、6 个面 meshlet、顶点路径回退、`F11` 切换；放大剔除当前禁用，见 docs/MESH-BINDLESS.md |
| Bindless 槽位管理 | 部分完成（0.6.0） | 8 槽有界表 + 代际 + 默认槽 + 复用 + 诊断；SM 6.6 直接堆索引评估为未实现 |
| 确定性契约（0.7.0） | 已完成 | `--seed` / `--hash-log`、命名 RNG 流、FNV-1a 量化摘要；两次运行逐字节相同，19 项证据检查，见 [docs/DETERMINISM.md](docs/DETERMINISM.md) |
| 帧预算管理器（0.7.0） | 部分完成 | 中位数决策 + 两档阶梯（阴影/网格路径）+ 报告 `budget` 段；18 项证据 + 28 项单元测试；无渲染缩放档，见 [docs/BUDGET.md](docs/BUDGET.md) |
| 离线几何包（0.7.0） | 部分完成 | 内容寻址包 + cluster DAG：37 cluster / 6 LOD / 确定性重建 / 篡改检测；无运行时 LOD 选择器，见 [docs/GEOMETRY-PACKAGE.md](docs/GEOMETRY-PACKAGE.md) |
| Chromium 输出契约（0.7.0） | 已完成（文档） | [docs/OUTPUT-CONTRACT.md](docs/OUTPUT-CONTRACT.md)：格式/同步/resize/验收清单，未实现项逐条标注 |
| RHI 边界（0.8.0） | 部分完成 | `nativegpu::rhi::Device` + d3d12 适配器 + 契约检查 13 项；只有一个后端，见 [docs/RHI.md](docs/RHI.md) |
| 回放与黑匣子（0.8.0） | 已完成 | `--record`/`--replay --replay-strict`/`--stall-*`；240 帧 0 不符；anomaly sidecar 可回放，见 [docs/REPLAY.md](docs/REPLAY.md) |
| 图像误差契约（0.8.0） | 已完成 | 5 条契约 + `.incrementalTesting/rendering/error-contract.ps1`，阈值来自实测，见 [docs/ERROR-CONTRACTS.md](docs/ERROR-CONTRACTS.md) |
| glTF 子集 + SDF 烘焙（0.8.0） | 部分完成 | 离线加载/烘焙可用；无非单位变换、无 GPU 追踪，见 [docs/GEOMETRY-PACKAGE.md](docs/GEOMETRY-PACKAGE.md)、[docs/SDF-BAKE.md](docs/SDF-BAKE.md) |
| 设计期 lint（0.8.0） | 已完成 | 12 条规则 + 可达性 BFS，`npm run scene -- lint`，见 [docs/SCENE-LINT.md](docs/SCENE-LINT.md) |
| compute 队列（0.9.0） | 部分完成 | 独立 COMPUTE 队列 + 跨队列 fence + 回读校验；结果尚未进入画面，见 [docs/COMPUTE.md](docs/COMPUTE.md) |
| SM 6.6 堆索引（0.9.0） | 部分完成 | `ResourceDescriptorHeap[32..35]` 与描述符表逐位一致；材质主路径迁移待 DXC 光栅变体 |
| 几何/材质架构（0.10.0） | 已冻结 | cluster 记录/工作队列/可见性缓冲/材质句柄 + 几何分类路由，见 [docs/GEOMETRY-LAYOUT.md](docs/GEOMETRY-LAYOUT.md) |
| 几何包 v2（0.10.0） | 已完成 | `lodError`/`flags`/`materialHandle`，v1 仍可读；本机 37 cluster / 6 LOD |
| 世界查询（0.10.0） | 已完成 | 9 种操作 + 聚合，`npm run scene -- query`，见 [docs/SCENE-QUERY.md](docs/SCENE-QUERY.md) |
| 植被系统 | 设计稿（未实现） | 10k 木本 + 50k 草的目标、GPU 剔除/LOD、impostor 与分块 shadow proxy；前置为任意 mesh、B2、alpha 材质，见 [docs/VEGETATION-PLAN.md](docs/VEGETATION-PLAN.md) |
| Web 编辑器 + 原生引擎 | 初版 | 场景编辑与事务闭环、原生独立窗口和按需快照，见 [docs/EDITOR.md](docs/EDITOR.md) |
| 内嵌实时视口与共享纹理 | 未实现 | 后续先验证成熟容器的 GPU 导入能力；定制 Chromium 方案暂缓 |
| Blender/资产管线 | 未实现 | 文档中的 S4 阶段 |
| 模型接入 | 未接模型 | 只提供模型无关的 MCP 工具面；不伪造 AI 能力 |

本机探针证据（当前主适配器 RTX 5060 Laptop GPU，D3D12）：`raytracingTier=1.2`、`meshShaderTier=10`、`shaderModel=6.8`、`resourceBindingTier=3`；
`materialTable.implemented=true`、`sceneLights.implemented=true`、`accelerationStructures.built=true`；
开启阴影时 `probes.raytracingShadows.effectImplemented=true`、`rtpsoCreated=true`、`sbtCreated=true`、
`fallbackUsed=false`、`gpuRtMs≈0.10 ms`；关闭阴影时 `effectImplemented=false`、`fallbackUsed=true`。
下列时序数字来自**上一轮核显（890M）**的证据运行，独显复测见 `reports/latest/`：1280×720 下 GPU P95 开启 0.490 ms、关闭 0.387 ms、33 个 TLAS 实例；
本轮独显新证据：预算运行 GPU P95 0.117 ms（120 帧窗口）、CPU submit P95 1.58 ms。
能力查询与加速结构构建不等于效果完成，字段刻意分开。

## 测试与报告

```powershell
npm test                       # Node 测试
npm run test:editor            # 编辑器服务与传输测试
npm run animation              # 动作导入、原生契约、蒙皮截图与严格回放
npm run physics                # 刚体/破坏前后截图、重复摘要与物理步预算
npm run probe                  # S1 探针，写 reports/probe.json
npm run verify                 # 构建 + Node + 原生 + 冒烟 + 探针 + 热重载 + MCP
npm run ci                     # 干净状态完整 CI（verify + 打包 + CI 报告）
npm run package                # 在 outputs/ 生成目录与 zip
```

每次 `verify.ps1` / `ci.ps1` 都在 `reports/<version>/<commit12>/<UTC时间戳>/` 新建一份证据目录，
并刷新 `reports/latest/` 与 `reports/verify*.json` 等别名；同一次输入重跑不会覆盖历史证据。
`run.json` 记录 commit、`worktreeDirty`、`staleBuild`、Node、适配器、DXC、场景哈希与产物 SHA-256。
`reports/`、`build/`、`outputs/` 和工具链目录都被 `.gitignore` 排除。

## 已知限制

- 支持 Windows 11 x64、D3D12、静态方盒和单角色 VS 骨骼蒙皮；离线 glTF 导入子集、60 Hz Clip、状态机/分层混合与 Notify 见 [docs/ANIMATION.md](docs/ANIMATION.md)。尚无动作匹配、脚 IK、肌肉/面部、音频、脚本系统或网络。
- 物理只有盒体：碎片是递归二分的盒块或均匀 cell 网格，不是凸 Voronoi（离线 Voronoi 烘焙已具备，渲染与凸碰撞未做）；关节只有 Point/Hinge/Rope 的 C++ API，尚未进场景格式；爆炸伤害不做遮挡；角色用 AABB 扫掠当前刚体 AABB，可站上箱子/碎片，但不是动态胶囊，旋转碎片的 AABB 偏保守；宽相是单线程 CPU 动态 AABB 树（小场景自动退回 O(n²)）。完整边界与论文改进路线见 [docs/PHYSICS-DESTRUCTION.md](docs/PHYSICS-DESTRUCTION.md)，四阶段扩展方案见 [docs/PHYSICS-SCALE-PLAN.md](docs/PHYSICS-SCALE-PLAN.md)。
- 场景工具与 Web 编辑器服务是受信本地开发工具；浏览器入口仅供本机使用，不要暴露到不可信网络。
- 场景/角色资源重载会重新初始化玩法；Lua 行为热重载保留显式状态，并支持版本迁移，见 [玩法运行时](docs/GAMEPLAY.md)。
- 材质表是 8 槽有界表（0 = 缺材质默认槽，1–3 常驻，4–7 可分配），带代际与复用；仍不是 SM 6.6 descriptor heap 直接索引。
- DXR 阴影是静态硬阴影：一条光线/像素、可见性 0/1，没有软阴影、降噪、透明投影或点光源投影；TLAS 每帧按刚体变换重建，不含蒙皮。
- 打包产物没有代码签名、安装器、自动更新和崩溃上传。
- CI 工作流只在本地完整重放过；GitHub 托管 runner 没有 D3D12 GPU，显卡相关步骤在远程会退化为 WARP 并允许失败，不能替代本机显卡证据。
- 编辑器后续优先验证成熟容器的共享纹理导入、输入与恢复，再决定是否内嵌原生视口；当前不以定制 Chromium 为前置。见 [docs/EDITOR.md](docs/EDITOR.md)。

详细状态见 [docs/STATUS.md](docs/STATUS.md)；逐条任务的完成/未完成台账见 [docs/DEV-TASKS.md](docs/DEV-TASKS.md)；下一阶段 1–4 的目标、验收标准和启动提示见 [docs/NEXT-PHASE.md](docs/NEXT-PHASE.md)。

## 开发文档站点

`npm run docs` 把 `docs/site/content/` 下的页面生成到 `docs/site/dist/`，直接用浏览器打开 `docs/site/dist/index.html` 即可离线阅读（无需服务器）。

文档分区参照 UE5.8 官方文档的分类方式，逐系统标注实现状态、证据来源与明确未实现项；`docs/site/nav.json` 控制侧边栏，新增页面只需加一项并放一个同名片段。
