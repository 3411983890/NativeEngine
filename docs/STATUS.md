# 项目状态与移交说明

- 版本：0.3.0
- 日期：2026-10-20
- 当前形态：Windows 11 x64 + D3D12 独立原生宿主 + Node.js 确定性场景事务层 + 本地 MCP 工具服务
- 不包含：Chromium 接入、DXR 追踪效果、Mesh Shader 效果、完整 Bindless、资产导入、骨骼动画、模型调用

## 1. 本次推进内容

1. 场景新增可选 `material` 与 `light` 实体：3 槽材质表、粗糙度/金属度、最多 4 盏点光；JS/schema/原生加载器对齐。
2. 渲染器改为材质纹理数组 + 每绘制材质参数；方向光、环境光和场景点光进入着色器，材质缺失时回退到默认材质。
3. 新增真实 DXR 加速结构探针：为方盒创建 BLAS，为 4 个实例创建 TLAS；报告字节数、实例数和错误，失败不阻塞光栅路径。
4. 新增 `--probe` 模式与 `tools/probe.ps1`，一次输出材质表、灯光、DXR AS 和 Mesh Shader 证据。
5. MCP 工具从 7 个扩展到 8 个，新增 `engine_probe`；场景工具支持 `light` 查询。
6. 原生测试扩展到 19 项场景加载检查；`verify.ps1` 增加 S1 探针关口。
7. 保留 0.2 的 13 项 Node 测试、热重载、原子事务、打包和 MCP 协议测试。
8. 宿主启动窗口居中并限制在工作区内，避免多显示器/高 DPI 下跑到屏幕外。

## 2. 阶段关口

| 阶段 | 状态 | 现有证据 | 主要缺口 |
| --- | --- | --- | --- |
| S0 准备 | 部分完成 | 独立可构建仓库、工具链 bootstrap、无外部 npm 依赖、干净构建命令 | 未冻结 Chromium commit、Windows SDK/DXC 版本；未建立 Git 版本库 |
| S1 原生图形 | 部分完成 | D3D12 设备/交换链/深度/纹理数组/Fence/时间戳；3 槽材质表与场景灯光；真实 BLAS/TLAS 构建；探针报告 | 尚无 DXR 追踪着色器、Mesh Shader 样例、完整 Bindless；无 PIX/RenderDoc 捕获 |
| S2 Chromium | 未开始 | 无 | Mojo、隔离服务、共享纹理、合成与恢复 |
| S3 游戏核心 | 部分完成 | 第三人称走跑跳、扫掠碰撞、交互、重置、场景事务、构建包 | 无资产导入、动画、音频、脚本、编辑器 UI；场景工具与原生运行仍是文件级同步 |
| S4 AI/Blender | 未开始 | MCP 工具面可被 Agent 调用 | 无 Blender Worker、无资产规范、无质量门禁、无模型适配器 |
| S5 闭环整合 | 部分完成 | 预览→提交→热重载→冒烟→报告的本地闭环 | 无自然语言任务编排、无代码补丁、无候选预览 UI、无费用/取消 |
| S6 稳定发布 | 未开始 | 可生成 zip 包 | 无签名、安装器、更新、长时稳定性与安全评审 |

## 3. 本机验证证据

运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify.ps1
```

检查：

- `reports/verify.json`：每一步的 ok、耗时、错误。
- `reports/verify-native-smoke.json`：原生运行的成功、帧数、revision、能力查询、CPU/GPU 时序。
- `reports/verify-probe.json`：材质表、场景灯光、DXR 加速结构与 Mesh Shader 探针结果。
- `reports/verify-hot-reload.json`：热重载后的 revision 与 `hotReloads` 证据。
- 热重载测试会在临时目录中启动原生进程，提交一次事务，并要求 `hotReloads >= 1`、`revision >= 1`。

本机探针信息只作为测试环境记录：AMD Radeon 890M，DXR 1.1，Mesh Shader Tier 10，Shader Model 6.8，Resource Binding Tier 3。`materialTable.implemented=true`、`sceneLights.implemented=true`、`accelerationStructures.built=true`（BLAS 2492 字节，TLAS 6336 字节，4 个实例）；但 `effectImplemented=false`、`implemented.raytracing=false`、`implemented.meshShaders=false`，因为尚无追踪或 Mesh Shader 着色器。

## 4. 下一阶段建议

按风险和证据顺序，不要同时展开：

### 工作包 A：DXR 静态阴影效果（优先）

- BLAS/TLAS 已落地；下一步引入可复现的 DXC 构建路径（固定版本，不依赖开发机 UE 安装）。
- 增加 ray generation / closest-hit / miss shader、DXR 状态对象、Shader Binding Table 与输出纹理。
- 先做静态硬阴影，保留光栅回退；报告分别记录 `probeAvailable`、`effectImplemented`、`fallbackUsed`。
- 验收：同一场景可切换开/关，截图与 GPU 时序可保存；设备不支持时明确显示 unsupported，不伪造成功。

### 工作包 B：Mesh Shader 样例

- 用 DXC 编译一个 `amplification`/`mesh` 样例，输入为离线或运行时切分的简单 meshlet。
- 保留传统顶点路径作为回退；记录同一模型的帧时和剔除数量。
- 验收：能力查询、编译日志、运行截图、回退路径四者都有。

### 工作包 C：Bindless 槽位管理

- 3 槽材质表已落地；下一步增加槽位代际、缺纹理回退、槽位复用和在途帧更新。
- 再评估 SM 6.6 直接堆索引；当前报告明确标注 `bindless=false`。
- 验收：至少两种材质在同一批绘制中正确采样；越界/失效槽位有诊断。

### 工作包 D：Chromium 宿主

- 在 A-C 有稳定证据后再开始；先做最小补丁集与静态纹理共享，再接入三缓冲和 Fence。
- 不以 CPU 回读或截图刷新替代实时合成。
- 如果 5 个有效工作日没有最小共享路径，按文档缩小范围并记录架构变更。

## 5. 维护约束

- 不允许模型直接写 `assets/`；必须走 `scene_preview`/`scene_apply`，并携带项目 ID 与 expectedRevision。
- 不允许为了通过测试删除测试或放宽 schema；范围变化要更新 `assets/scene.schema.json`、JS 校验器、原生加载器和文档四处。
- 热重载失败必须保留上一有效场景；不得用空场景或静默重置掩盖损坏。
- 高级图形能力只有在“可运行 + 可回退 + 有报告”后才把 `implemented` 改为 true。
- 场景工具是受信本地工具；在加入网络服务、模型凭证或远程执行前，必须补权限、配额、审计和超时。

## 6. 下个对话目标

完成 1–4 工作包后再进入 Chromium S2，详细目标、验收标准和启动提示见 [NEXT-PHASE.md](NEXT-PHASE.md)。
