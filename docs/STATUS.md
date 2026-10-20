# 项目状态与移交说明

- 版本：0.3.1（工程基线，工作包 1/4）
- 日期：2026-10-20
- 当前形态：Windows 11 x64 + D3D12 独立原生宿主 + Node.js 确定性场景事务层 + 本地 MCP 工具服务 + 可重放 CI
- 不包含：Chromium 接入、DXR 追踪效果、Mesh Shader 效果、完整 Bindless、资产导入、骨骼动画、模型调用

## 1. 本次推进内容（工作包 1：Git / CI / 版本冻结 / 版本化报告）

1. **Git 版本冻结**：项目根目录建立 `main` 分支，0.3.0 基线提交并打 annotated tag `v0.3.0`；
   `.gitignore` 排除 `.tools/`、`build/`、`reports/`、`outputs/`、`work/`、`dist/`、临时文件。
   之后每个工作包一个提交、一个 tag。
2. **DXC 与工具链冻结**：新增 `tools/bootstrap-dxc.ps1`，固定 DXC `1.9.2609`
   （归档 SHA-256 `ad31b1fc…a7e7f1`、`dxcompiler.dll`、`dxil.dll` 哈希都记入 `.tools/dxc/1.9.2609/dxc.json`）。
   新增 `tools/compile-shaders.ps1`，按 `shaders/manifest.json` 编译并内嵌 DXIL blob；DXC 缺失时生成
   `kDxcAvailable=false` 的头文件，运行报告必须给出 `dxcUnavailable`，不允许静默回退。
   版本表写入 [TOOLCHAIN.md](TOOLCHAIN.md)。
3. **CI**：新增 `tools/ci.ps1`（唯一入口，本机可完整重放）与 `.github/workflows/windows.yml`。
   CI 依次执行 bootstrap → DXC → `verify.ps1` → DXR 阴影证据 → G1 可靠性 → Mesh/Bindless → 打包，
   成功写 `reports/ci-<commit12>.json`，失败保留完整步骤日志。
4. **版本化报告**：`tools/report-lib.ps1` 把证据写入
   `reports/<version>/<commit12>/<UTC时间戳>/`，同时维护 `reports/latest/` 与旧别名；
   `run.json` 记录 commit、`worktreeDirty`、`staleBuild`、Node、PowerShell、OS、适配器、DXC、场景哈希和每个产物的 SHA-256。
   同一次输入重跑不会覆盖历史证据。布局见 [CI.md](CI.md)。
5. **构建可追溯**：`VERSION` 是唯一版本来源；构建生成 `build/generated/nativegpu_version.h`，
   原生报告带 `version` 与 `build.commit`/`build.dirty`；`package.json` 版本不一致时打包直接失败。
6. **DXC 负路径自检**：`tools/compile-shaders.ps1 -SelfTest` 故意编译坏着色器，要求 DXC 失败并保留诊断，
   已接入 `verify.ps1`，防止“编译器失败被当成成功”。
7. 保留 0.3.0 的 13 项 Node 测试、19 项原生场景检查、原生冒烟、S1 探针、热重载、MCP 协议与打包。

## 2. 阶段关口

| 阶段 | 状态 | 现有证据 | 主要缺口 |
| --- | --- | --- | --- |
| S0 准备 | 已完成（0.3.1） | Git + tag、可重放 CI、固定 DXC/工具链版本表、版本化证据目录 | 远程仓库尚未建立，CI 仅在本地完整重放 |
| S1 原生图形 | 部分完成 | D3D12 设备/交换链/深度/纹理数组/Fence/时间戳；3 槽材质表与场景灯光；真实 BLAS/TLAS 构建；探针报告；DXC 编译管线 | 尚无 DXR 追踪着色器、Mesh Shader 样例、完整 Bindless；无 PIX/RenderDoc 捕获 |
| S2 Chromium | 未开始 | 无 | Mojo、隔离服务、共享纹理、合成与恢复；Chromium commit 未冻结 |
| S3 游戏核心 | 部分完成 | 第三人称走跑跳、扫掠碰撞、交互、重置、场景事务、构建包 | 无资产导入、动画、音频、脚本、编辑器 UI；场景工具与原生运行仍是文件级同步 |
| S4 AI/Blender | 未开始 | MCP 工具面可被 Agent 调用 | 无 Blender Worker、无资产规范、无质量门禁、无模型适配器 |
| S5 闭环整合 | 部分完成 | 预览→提交→热重载→冒烟→报告的本地闭环 | 无自然语言任务编排、无代码补丁、无候选预览 UI、无费用/取消 |
| S6 稳定发布 | 未开始 | 可生成 zip 包 | 无签名、安装器、更新、长时稳定性与安全评审 |

## 3. 本机验证证据

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci.ps1 -Clean
```

检查：

- `reports/<version>/<commit>/<timestamp>/run.json`：环境、DXC、适配器、场景哈希、产物 SHA-256。
- 同目录 `verify.json`：每一步的 ok、耗时、错误。
- `verify-native-smoke.json`：帧数、revision、能力查询、CPU/GPU 时序、`build.commit`。
- `verify-probe.json`：材质表、场景灯光、DXR 加速结构与 Mesh Shader 探针。
- `verify-hot-reload.json`：热重载后的 revision 与 `hotReloads` 证据。
- `reports/latest/` 与 `reports/verify.json`：最新别名，方便旧文档引用。

本机探针信息只作为测试环境记录：AMD Radeon 890M，驱动 32.0.21036.1002，DXR 1.1，Mesh Shader Tier 10，
Shader Model 6.8，Resource Binding Tier 3。`materialTable.implemented=true`、`sceneLights.implemented=true`、
`accelerationStructures.built=true`（BLAS 2492 字节，TLAS 6336 字节，4 个实例）；
`effectImplemented=false`、`implemented.raytracing=false`、`implemented.meshShaders=false`，
因为尚无追踪或 Mesh Shader 着色器。能力查询和加速结构构建不等于效果已完成。

## 4. 后续工作包

按 [NEXT-PHASE.md](NEXT-PHASE.md) 顺序推进，每个工作包都必须构建、测试、更新报告并保留可运行版本：

- 工作包 2：DXR 静态阴影（DXC、RTPSO、SBT、阴影输出、光栅回退、开关与报告字段）。
- 工作包 3：G1 可靠性（设备丢失、资源生命周期、非法状态、PIX/RenderDoc 捕获、性能基线）。
- 工作包 4：Mesh Shader 样例与 Bindless 槽位管理。
- 工作包 5：只有 G1 放行后才开始 Chromium S2，且必须先冻结 Chromium commit。

## 5. 维护约束

- 不允许模型直接写 `assets/`；必须走 `scene_preview`/`scene_apply`，并携带项目 ID 与 expectedRevision。
- 不允许为了通过测试删除测试或放宽 schema；范围变化要更新 `assets/scene.schema.json`、JS 校验器、原生加载器和文档四处。
- 热重载失败必须保留上一有效场景；不得用空场景或静默重置掩盖损坏。
- 高级图形能力只有在“可运行 + 可回退 + 有报告”后才把 `implemented` 改为 true。
- 证据不得手写：`implemented`、`effectImplemented`、性能数字只能来自实际运行写出的报告。
- 场景工具是受信本地工具；在加入网络服务、模型凭证或远程执行前，必须补权限、配额、审计和超时。

## 6. 下个对话目标

按 NEXT-PHASE 依次完成工作包 2–4 并通过 G1 放行检查；G1 通过后停止图形扩展，等用户确认再进入第 5 项 Chromium。
