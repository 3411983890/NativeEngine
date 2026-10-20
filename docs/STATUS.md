# 项目状态与移交说明

- 版本：0.4.0（工作包 2/4：DXR 静态阴影）
- 日期：2026-10-20
- 当前形态：Windows 11 x64 + D3D12 独立原生宿主（光栅 + DXR 静态阴影）+ Node.js 确定性场景事务层 + 本地 MCP 工具服务 + 可重放 CI
- 不包含：Chromium 接入、Mesh Shader 效果、完整 Bindless、设备丢失/生命周期压力、资产导入、骨骼动画、模型调用

## 1. 本次推进内容（工作包 2：DXR 静态阴影）

1. **着色器与管线**：新增 `shaders/rt_shadows.hlsl`（raygen / miss / closest-hit），用固定 DXC 1.9.2609
   编译为 `lib_6_5` DXIL 并内嵌；创建 RTPSO（DXIL library + hit group + shader config + pipeline config +
   global root signature）和 192 字节 Shader Binding Table。
2. **渲染帧图**：光栅 pass 改为双渲染目标（`sceneBaseColor` = 环境光 + 点光，`sceneKeyLight` = key light 项），
   深度改为 `R32_TYPELESS`（DSV `D32_FLOAT` / SRV `R32_FLOAT`）；DXR pass 从深度重建世界坐标逐像素发射遮挡光线，
   写入 `R16_FLOAT` 可见性贴图；合成 pass 计算 `base + key * mask` 并复制到交换链。
3. **回退与开关**：`F10` 与 `--rt-shadows on/off`；关闭时把 mask 清成 1，结果与 0.3.0 纯光栅图像逐像素一致，
   不需要第二条渲染路径。
4. **真实实例输入**：TLAS 从当前帧的刚体 `DrawItem` 变换构建（本机 33 个实例），每帧重建；
   单位方盒 BLAS 复用，不含蒙皮。
5. **报告字段**：`probes.raytracingShadows` 区分 `probeAvailable`、`dxcAvailable`、`requested`、`enabled`、
   `rtpsoCreated`、`sbtCreated`、`effectImplemented`、`fallbackUsed`、`shadowTextureBytes`、`dispatchedFrames`、
   `fallbackFrames`、`dispatchFailures`、`gpuRtMs`、`tlasInstances`、`tlasRebuilds`、`error`。
   `capabilities.implemented.raytracing` 只在本次运行真的派发过光线时为 true。
6. **截图**：`--screenshot` / `--screenshot-frame` / `--camera` / `--render-size` / `--debug-view`；
   PNG 由内置无依赖编码器写出，截图复制在帧内记录、帧栅栏后读取。
7. **证据脚本**：`tools/rt-shadows.ps1` 跑开启/关闭/mask/key 四遍，逐像素对比、记录时序、断言报告字段，
   23 项检查全部通过；`tools/ci.ps1` 已把它纳入 CI。
8. **渲染数学测试**：新增 `renderer/matrix_math.{h,cpp}` 与 `tests/renderer_math.cpp`，
   覆盖单位矩阵、奇异矩阵、view-projection 往返、仿射往返（11 项），防止阴影重建矩阵写错。

本机数字（AMD Radeon 890M，1280x720，33 实例）：开启阴影 GPU P95 0.717 ms，关闭 0.467 ms
（+0.250 ms）；DispatchRays 窗口 0.105 ms；可见性贴图 5,529,600 字节；3.30% 像素因阴影改变。
CPU P95 约 17.2 ms，含 `Present(1,0)` 垂直同步等待，不代表 CPU 成本。

## 2. 阶段关口

| 阶段 | 状态 | 现有证据 | 主要缺口 |
| --- | --- | --- | --- |
| S0 准备 | 已完成（0.3.1） | Git + tag、可重放 CI、固定 DXC/工具链版本表、版本化证据目录 | 远程仓库尚未建立，CI 仅在本地完整重放 |
| S1 原生图形 | 部分完成 | D3D12 双目标光栅、深度可采样、3 槽材质表、场景灯光、DXR 静态阴影（RTPSO/SBT/mask/合成/回退/截图/时序）、DXC 管线 | 尚无 Mesh Shader 样例、完整 Bindless、PIX/RenderDoc 捕获、设备丢失与生命周期证据 |
| S2 Chromium | 未开始 | 无 | Mojo、隔离服务、共享纹理、合成与恢复；Chromium commit 未冻结 |
| S3 游戏核心 | 部分完成 | 第三人称走跑跳、扫掠碰撞、交互、重置、场景事务、构建包 | 无资产导入、动画、音频、脚本、编辑器 UI |
| S4 AI/Blender | 未开始 | MCP 工具面可被 Agent 调用 | 无 Blender Worker、无资产规范、无质量门禁、无模型适配器 |
| S5 闭环整合 | 部分完成 | 预览→提交→热重载→冒烟→报告的本地闭环 | 无自然语言任务编排、无代码补丁、无候选预览 UI |
| S6 稳定发布 | 未开始 | 可生成 zip 包 | 无签名、安装器、更新、长时稳定性与安全评审 |

## 3. 本机验证证据

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci.ps1 -Clean
powershell -NoProfile -ExecutionPolicy Bypass -File tools/rt-shadows.ps1
```

检查：

- `reports/<version>/<commit>/<timestamp>/run.json`：环境、DXC、适配器、场景哈希、产物 SHA-256。
- `rt-shadows-evidence.json`：开关两遍的报告字段、时序、截图路径、逐像素对比、23 项检查。
- `artifacts/shadow-on.png`、`shadow-off.png`、`shadow-mask.png`、`shadow-key.png`：固定机位证据。
- `verify.json` / `verify-native-smoke.json` / `verify-probe.json` / `verify-hot-reload.json`：既有各层证据。
- `reports/latest/` 与 `reports/verify.json` 等别名：最新一次运行。

阴影设计、字段表、验收对照与踩坑记录见 [RT-SHADOWS.md](RT-SHADOWS.md)。

## 4. 后续工作包

按 [NEXT-PHASE.md](NEXT-PHASE.md) 顺序推进：

- 工作包 3：G1 可靠性（设备丢失、资源生命周期 50 次、非法状态与错误输入、PIX/RenderDoc 捕获、性能基线）。
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

依次完成工作包 3–4 并通过 G1 放行检查；G1 通过后停止图形扩展，等用户确认再进入第 5 项 Chromium。

