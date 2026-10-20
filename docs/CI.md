# CI 与证据目录

## 1. 一条命令

```powershell
# 干净状态：删除 build/ outputs/，保留 reports/ 历史证据
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci.ps1 -Clean

# 完整验收：连 reports/ 一起删掉，从零重放
Remove-Item build,outputs,reports -Recurse -Force -ErrorAction SilentlyContinue
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci.ps1
```

`tools/ci.ps1` 依次执行：工具链 bootstrap → DXC bootstrap → `tools/verify.ps1`（构建、Node 测试、
DXC 负路径自检、原生测试、场景校验、原生 smoke、S1 探针、热重载、MCP 协议）→ DXR 阴影证据 →
G1 可靠性套件 → Mesh/Bindless 证据 → 打包 → 写 CI 报告。

远程 CI 只做三件事：检出、装 Node、调用 `tools/ci.ps1`，然后上传 `reports/`。
工作流见 `.github/workflows/windows.yml`；宿主 runner 没有 D3D12 GPU，因此显卡相关步骤用
`-AllowUnsupportedGpu` 变成允许失败，但它们必须仍然输出 `unsupported`，不得输出 `implemented`。

## 2. 证据目录布局

```text
reports/
  <version>/<commit12>/<UTC yyyyMMddTHHmmssZ>/
    run.json                    本次运行的环境 + 产物哈希清单
    verify.json                 各步骤 ok / 耗时 / 错误
    verify-native-smoke.json    原生 smoke（含 capabilities、probes、时序）
    verify-probe.json           S1/G1 探针（材质表、灯光、DXR AS、阴影、Mesh、Bindless）
    verify-hot-reload.json      热重载 revision 与 hotReloads
    rt-shadows-evidence.json    DXR 阴影开/关对比与截图路径
    g1-reliability.json         设备丢失、资源生命周期、错误输入结果
    mesh-bindless-evidence.json Mesh Shader 与 Bindless 槽位结果
    ci.json                     CI 汇总（步骤、G1 探针、包哈希）
    artifacts/                  截图（PNG）、捕获说明等
    logs/                       每个 CI 步骤的完整输出
  latest/                       最新一次运行的同名副本 + POINTER.json
  verify.json  verify-probe.json  verify-native-smoke.json  verify-hot-reload.json
  ci-<commit12>.json            兼容别名，指向最近一次对应产物
```

规则：

- 新运行永远新建 `<timestamp>` 目录，不覆盖旧证据；
- `run.json` 里 `worktreeDirty` / `staleBuild` 必须为 `false` 才算正式放行证据；
- 失败时把失败步骤的完整日志复制成 `ci-failure-<commit12>-<step>.log` 保留；
- `reports/`、`build/`、`outputs/` 都不进 Git。

## 3. 报告字段约定

能力查询与效果实现必须分开写，禁止把“设备支持”写成“效果已实现”：

| 字段 | 含义 |
| --- | --- |
| `capabilities.raytracingTier` / `meshShaderTier` / `shaderModel` | 适配器能力查询结果 |
| `probes.*.available` | 设备层面是否支持 |
| `probes.*.implemented` | 本版本是否真的接入并跑通 |
| `probes.raytracingShadows.effectImplemented` | RTPSO + SBT + 至少一次成功 DispatchRays |
| `probes.raytracingShadows.fallbackUsed` | 本次是否走了光栅回退 |
| `capabilities.implemented.raytracing` | 只有 `effectImplemented` 为 true 时才为 true |

## 4. 本地重放单个环节

```powershell
npm test                                            # 13 项 Node 测试
powershell -File tools/test-native.ps1               # 原生测试
powershell -File tools/probe.ps1                     # 写 reports/probe.json
powershell -File tools/compile-shaders.ps1 -SelfTest # DXC 负路径
powershell -File tools/rt-shadows.ps1                # DXR 阴影开/关证据
powershell -File tools/g1-reliability.ps1            # 可靠性套件
powershell -File tools/mesh-bindless.ps1             # Mesh + Bindless 证据
powershell -File tools/g1.ps1                        # G1 放行闸门
```
