# NativeGPU / AI 原生游戏引擎

版本 **0.6.0**。一个可运行、可验证的 Windows D3D12 原生游戏样板，以及供编辑器和 AI Agent 共用的确定性场景事务层。当前重点不是宣称“已经完成 AI 引擎”，而是把一条真实闭环做通：

> 读取场景 → 预览事务 → 原子提交 → 运行中的原生编辑器热重载 → 原生冒烟测试 → 返回可审计报告

## 现在能做什么

- **原生第三人称样板**：D3D12 交换链、深度测试、固定 120 Hz 物理（走跑跳、扫掠碰撞、拾取钥匙、开门、到达终点）。
- **材质与灯光**：3 槽有界材质纹理表（checker/brick/metal）、粗糙度/金属度、最多 4 盏场景点光；场景 JSON 直接携带材质和灯光。
- **DXR 加速结构与静态阴影**：真实创建 BLAS/TLAS（本机 33 个方盒实例），用固定 DXC 编译 ray generation / miss / closest-hit 库并创建 RTPSO 与 SBT，逐像素追踪一条硬阴影光线；`F10` 或 `--rt-shadows on/off` 切换，关闭时与纯光栅图像逐像素一致。细节见 [docs/RT-SHADOWS.md](docs/RT-SHADOWS.md)。
- **确定性场景工具**：校验、查询、预览、提交、撤销、崩溃恢复、内容寻址构建；失败不会半写入，旧 revision 会被拒绝。
- **运行中热重载**：`scene apply` 写入成功后，原生编辑器自动加载新 revision；无效写入会保留上一有效场景并报告错误，不弹模态框阻塞运行。
- **AI 工具入口**：本地 MCP stdio 服务暴露 8 个工具，Agent 可独立调用，不需要聊天壳或模型凭证。
- **可重复验证**：13 项 Node 测试、原生测试、原生冒烟、S1 探针、热重载集成测试和 MCP 协议冒烟都在 `tools/verify.ps1` 中执行。
- **DXR 静态阴影证据（0.4.0 新增）**：`tools/rt-shadows.ps1` 用固定机位与固定分辨率跑开/关两遍，保存 final/mask/key 截图、逐像素对比并断言报告字段，23 项检查全部通过。
- **可重放工程基线（0.3.1 新增）**：Git 版本冻结、固定 DXC 1.9.2609 与哈希、`tools/ci.ps1` 一条命令从干净状态重放全部检查、证据按 `<version>/<commit>/<timestamp>` 归档且不覆盖历史。

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

# 3. 在另一个终端提交一次场景修改，运行中的窗口会自动热重载
node tools/scene.mjs apply assets/courtyard.scene.json ai/example-transaction.json

# 4. 运行 S1 探针（材质表、场景灯光、DXR 加速结构）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/probe.ps1

# 5. 完整验证（会启动隐藏窗口做原生冒烟与探针）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify.ps1

# 6. 从干净状态重放 CI（构建、测试、探针、热重载、MCP、打包）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci.ps1 -Clean

# 7. 生成可分发包和 zip
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package.ps1
```

交互按键：`WASD` 移动、`Shift` 跑、`Space` 跳、`E` 交互、右键拖动或方向键转镜头、`F5` 重置玩法、`F6` 暂停、`F8` 重建渲染器、`F9` 开关热重载、`F10` 开关 DXR 阴影、`F11` 切换 Mesh Shader 路径。

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
assets/           样板场景与 JSON Schema
tests/            Node 事务/工具测试与原生测试
tools/            构建、bootstrap、CI、运行、验证、打包、场景 CLI
docs/             当前阶段状态、工具链冻结表与 CI/证据说明
.github/          windows-latest CI 工作流（只调用 tools/ci.ps1）
```

## 工程基线（0.3.1）

- **Git**：`main` 分支，基线 tag `v0.3.0`，之后每个工作包一个提交并打 tag；`build/`、`reports/`、`outputs/`、`work/`、`.tools/` 都在 `.gitignore` 中，不进仓库。
- **CI**：`tools/ci.ps1` 是唯一入口，本地与 `.github/workflows/windows.yml` 调用同一脚本；失败保留 `logs/` 与 `ci-failure-<commit>-*.log`。
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
| AI 工具入口 | 已完成 | MCP stdio，8 个工具，协议冒烟测试 |
| 版本冻结与 CI（0.3.1） | 已完成 | Git tag `v0.3.0`、固定 DXC 1.9.2609、`tools/ci.ps1` 可本机重放、版本化证据目录 |
| Mesh Shader / Meshlet | 已完成（0.6.0） | AS+MS+PS（stream PSO）、6 个面 meshlet、顶点路径回退、`F11` 切换；放大剔除当前禁用，见 docs/MESH-BINDLESS.md |
| Bindless 槽位管理 | 部分完成（0.6.0） | 8 槽有界表 + 代际 + 默认槽 + 复用 + 诊断；SM 6.6 直接堆索引评估为未实现 |
| Chromium 编辑器与共享纹理 | 未实现 | 文档中的 S2 阶段，当前仍是独立 Win32 宿主 |
| Blender/资产管线 | 未实现 | 文档中的 S4 阶段 |
| 模型接入 | 未接模型 | 只提供模型无关的 MCP 工具面；不伪造 AI 能力 |

本机探针证据（AMD Radeon 890M，D3D12）：`raytracingTier=1.1`、`meshShaderTier=10`、`shaderModel=6.8`、`resourceBindingTier=3`；
`materialTable.implemented=true`、`sceneLights.implemented=true`、`accelerationStructures.built=true`；
开启阴影时 `probes.raytracingShadows.effectImplemented=true`、`rtpsoCreated=true`、`sbtCreated=true`、
`fallbackUsed=false`、`gpuRtMs≈0.10 ms`、1280×720 下 GPU P95 比关闭时多 0.25 ms；关闭阴影时
`effectImplemented=false`、`fallbackUsed=true`，`capabilities.implemented.raytracing=false`。
能力查询与加速结构构建不等于效果完成，字段刻意分开。

## 测试与报告

```powershell
npm test                       # Node 测试
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

- 仅支持 Windows 11 x64、D3D12 和方盒几何；没有骨骼动画、资产导入、音频、脚本系统或网络。
- 场景工具是受信本地开发工具，不是网络服务或安全沙箱；不要把它暴露到不可信网络。
- 热重载会重新初始化玩法状态；当前不做运行中状态迁移。
- 材质表是 8 槽有界表（0 = 缺材质默认槽，1–3 常驻，4–7 可分配），带代际与复用；仍不是 SM 6.6 descriptor heap 直接索引。
- DXR 阴影是静态硬阴影：一条光线/像素、可见性 0/1，没有软阴影、降噪、透明投影或点光源投影；TLAS 每帧按刚体变换重建，不含蒙皮。
- 打包产物没有代码签名、安装器、自动更新和崩溃上传。
- CI 工作流只在本地完整重放过；GitHub 托管 runner 没有 D3D12 GPU，显卡相关步骤在远程会退化为 WARP 并允许失败，不能替代本机显卡证据。
- 下一阶段优先级：用 DXC 编译一个可切换的 DXR 静态阴影 pass，再补 Mesh Shader 样例和完整 Bindless 槽位管理；之后进入 Chromium 共享纹理宿主。

详细状态见 [docs/STATUS.md](docs/STATUS.md)；逐条任务的完成/未完成台账见 [docs/DEV-TASKS.md](docs/DEV-TASKS.md)；下一阶段 1–4 的目标、验收标准和启动提示见 [docs/NEXT-PHASE.md](docs/NEXT-PHASE.md)。
