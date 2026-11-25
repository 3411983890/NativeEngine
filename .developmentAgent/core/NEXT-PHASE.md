# 下一阶段目标：完成 1–4，随后进入 5

- 状态：待启动（下一对话）
- 基线：NativeGPU 0.3.0，G1 未通过
- 目标：按顺序完成工程基线、DXR 静态阴影、G1 可靠性、Mesh Shader + Bindless；G1 关口通过后再开始 Chromium（S2）

## 建议的下个对话开场目标

```text
基于当前 AI-Native Game Engine 0.3.0 基线，按 docs/NEXT-PHASE.md 依次完成 1–4：
1) Git/CI/版本冻结与版本化报告
2) DXR 静态阴影 pass（DXC、RTPSO、SBT、阴影输出、回退）
3) G1 可靠性补偿（设备丢失、资源生命周期、非法状态、捕获与性能基线）
4) Mesh Shader 样例与 Bindless 槽位管理
每完成一个工作包必须构建、测试、更新报告并保留可运行版本；G1 关口通过后停止扩展图形，进入第 5 项 Chromium 宿主。
不要提前开始 Chromium，也不要把“能力查询”写成“效果已实现”。
```

## 0. 基线现状

已完成：

- D3D12 光栅基线、方盒玩法、材质表 3 槽、场景灯光、热重载。
- DXR BLAS/TLAS 构建（4 个实例），探针报告。
- MCP 8 工具、13 项 Node 测试、19 项原生场景检查、8 步验证。
- 0.3.0 可运行包。

未完成：

- 没有 Git 仓库和 CI。
- DXR 没有追踪着色器和阴影/反射输出。
- 没有设备丢失、资源泄漏压力、非法状态、PIX/RenderDoc 捕获。
- 没有 Mesh Shader 样例；Bindless 只有 3 槽纹理表。
- Chromium、资产、动画、音频、脚本、模型、发布均未开始。

## 1. 工程基线（先做）

### 1.1 Git 与版本冻结

- 在项目根目录初始化 Git；确认 `.gitignore` 排除 `.tools/`、`build/`、`reports/`、`outputs/`、`work/`、临时文件。
- 建立 `main` 分支并提交 0.3.0 基线，打 tag `v0.3.0`。
- 后续每个工作包一个提交，提交信息包含工作包编号；禁止把构建产物和报告提交进仓库。
- 记录 `git rev-parse HEAD`、构建工具版本、运行报告中的 commit。

### 1.2 CI

- 新增 `.incrementalTesting/core/ci.ps1`：从干净状态执行 build、Node 测试、原生测试、smoke、probe、hot reload、MCP、package。
- 新增 `.github/workflows/windows.yml`（或等价本地 CI 配置），至少覆盖 `windows-latest`；如果暂时没有远程仓库，CI 脚本必须能在本机独立重放。
- CI 失败时保留日志；成功时产出 `reports/ci-<commit>.json`。
- 验收：删除 `build/`、`outputs/`、`reports/` 后，从干净目录执行 `.incrementalTesting/core/ci.ps1` 成功；报告包含 commit、Node 版本、适配器、G1 探针结果。

### 1.3 DXC 与 SDK 冻结

- 固定 DXC 版本和 SHA-256，放入 `.tools/dxc/`（不提交二进制）或由 bootstrap 下载；提供 `tools/bootstrap-dxc.ps1`。
- 记录 Windows SDK、MinGW、Node、Chromium 目标 commit 的版本表。
- 如果 DXC 不可用，DXR 相关工作必须报告 `dxcUnavailable`，不能静默回退成“已实现”。
- 验收：`tools/bootstrap.ps1` 或专用脚本可重建；版本表写入 `docs/TOOLCHAIN.md`。

### 1.4 版本化报告

- 报告目录从固定的 `reports/verify.json` 扩展为 `reports/<version>/<commit>/...`，同时保留最新别名。
- 每次 G1 证据包含：commit、场景 hash、分辨率、驱动、DXC 版本、截图/捕获路径、CPU/GPU 时序。
- 验收：同一次输入重跑不会覆盖历史证据。

## 2. DXR 静态阴影 pass

### 2.1 着色器与管线

- 用固定 DXC 编译一个 ray tracing library：
  - ray generation；
  - closest-hit（方盒几何）；
  - miss；
  - 至少一个阴影 payload。
- 创建 DXR 状态对象（RTPSO）和 Shader Binding Table。
- 使用现有 BLAS/TLAS；先做静态硬阴影，不做动态蒙皮加速结构。
- 输出阴影纹理，并在光栅 pass 后合成；保留纯光栅回退。

### 2.2 开关与报告

- 增加运行时开关（例如 `F10` 或 `--rt-shadows`）。
- 报告字段必须区分：
  - `probeAvailable`
  - `effectImplemented`
  - `fallbackUsed`
  - `dxcVersion`
  - `rtpsoCreated`
  - `sbtCreated`
  - `shadowTextureBytes`
  - `gpuRtMs`（如果时间戳可分离）
- 设备不支持时显示 unsupported；编译失败时回退且保留错误文本，不能崩溃。

### 2.3 验收

- 同一场景可切换开/关；截图可保存。
- 至少一个固定机位截图证明阴影存在。
- 回退路径在同一机器可运行。
- 与光栅基线的帧时对比写入报告；不能只写“成功”。
- `implemented.raytracing` 只有在上述证据齐全后才改为 true。

## 3. G1 可靠性补偿

### 3.1 设备丢失

- 增加可重复的设备丢失测试：用调试层/工具触发或模拟 `DXGI_ERROR_DEVICE_REMOVED`，验证：
  - 渲染循环停止并报告错误；
  - 不写入损坏的包/场景；
  - 重建渲染器后可恢复（如果驱动允许）。
- 报告字段从 `deviceRemovalTested=false` 改为真实结果。

### 3.2 资源生命周期

- 反复初始化/关闭渲染器（至少 50 次），记录显存/对象计数趋势。
- 反复切换场景/材质/灯光，确认无持续增长。
- 检验在途帧资源在 Fence 完成后才释放。
- 场景文件反复 apply/undo，确认无临时文件/lock 残留。

### 3.3 非法状态与错误输入

- 覆盖：无效 PSO、错误描述符、越界槽位、缺失纹理、无效场景、损坏包、DXC 编译失败。
- 每种错误必须有稳定错误码和诊断文本，不静默忽略。

### 3.4 捕获与性能基线

- 至少保存一次 PIX 或 RenderDoc 捕获路径/说明。
- 固定机位和固定分辨率下记录 CPU/GPU 帧时 P50/P95、三角形数、绘制数、材质槽使用数。
- 将基线写入 `docs/PERF-BASELINE.md`。

### 3.5 G1 放行

G1 通过条件：

- DXR 静态阴影有可切换效果和回退。
- Mesh Shader 有可运行样例和回退（见第 4 项）。
- Bindless/材质槽有缺失、复用、越界诊断。
- 设备丢失、资源释放、错误输入测试至少各一个。
- 截图/捕获/时序报告可重放。

## 4. Mesh Shader 与 Bindless

### 4.1 Mesh Shader

- 用 DXC 编译 amplification/mesh 样例；输入为离线或运行时切分的简单 meshlet。
- 保留传统顶点路径回退。
- 记录同一模型、同画质下的帧时和剔除数量。
- 验收：能力查询、编译日志、运行截图、回退路径四者都有。

### 4.2 Bindless 槽位管理

- 在当前 3 槽纹理表上增加：
  - 槽位代际；
  - 缺纹理默认槽；
  - 槽位复用与释放；
  - 在途帧更新测试。
- 评估 SM 6.6 直接堆索引；如果实现，报告 `bindless=true`；如果不实现，明确写 `boundedTable`。
- 验收：至少两种材质同批正确采样；越界/失效槽位有诊断；无资源泄漏。

## 5. 进入 Chromium（S2）

只有 G1 通过后才开始。第一步只做：

1. 冻结 Chromium commit，完成最小宿主构建。
2. 定义 `NativeGpuSession` Mojo 接口和 Browser Host 权限代理。
3. 把原生核心迁入隔离进程，完成启动/停止/崩溃重连。
4. 先传一张静态共享纹理，再接入三缓冲和 Fence。
5. 记录复制次数、同步与合成耗时；禁止用 CPU 回读或截图刷新冒充实时合成。

S2 的第一个验收不是“能显示”，而是“受信本地页面可稳定显示连续渲染视口，普通页面不能调用，强制停止服务后可重建会话”。

## 6. 执行顺序与停止条件

```text
1 工程基线
  ↓
2 DXR 静态阴影
  ↓
3 G1 可靠性
  ↓
4 Mesh Shader + Bindless
  ↓
G1 放行检查
  ↓
5 Chromium S2
```

停止条件：

- 同一互操作/驱动问题投入 5 个有效工作日仍无进展：保存最小复现、缩小范围，不继续堆功能。
- 构建/测试不通过：不进入下一工作包。
- 没有截图/捕获/时序证据：不能宣称效果完成。
- 没有 Git 提交和报告归档：不能宣称工作包完成。

## 7. 当前证据入口

- `reports/verify.json`
- `reports/verify-probe.json`
- `reports/verify-hot-reload.json`
- `reports/verify-native-smoke.json`
- `docs/STATUS.md`
- `README.md`
