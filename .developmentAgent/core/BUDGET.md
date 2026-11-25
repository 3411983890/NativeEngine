# 帧预算管理器（R08 / INN-04）

预算管理器把"性能"从口头目标变成可执行的策略：滚动窗口持续超目标时，按固定阶梯降低质量，并把每一步的原因写进报告。它只决定**何时**降档，具体降什么由宿主执行。

## 1. 决策规则

| 项 | 规则 |
| --- | --- |
| 决策指标 | 滚动窗口的**中位数**（GPU 与 CPU submit），不是 P95 |
| 理由 | 短窗口的 P95 只要一帧毛刺就拉满，会在单帧卡顿时误降档；中位数代表"典型帧" |
| 触发条件 | 中位数连续 `sustainFrames` 帧超目标（且窗口已有足够样本） |
| 阶梯 | 单调、每档只应用一次、不可回退（回退会来回抖动，留给后续版本） |
| 预算耗尽 | 走完阶梯仍超目标 → `status=exhausted`，只记录不再动作 |
| 视频内存 | 用窗口内峰值与目标比较，0 表示不启用该维度 |

## 2. 阶梯（当前两档）

| 档位 | 名称 | 宿主动作 |
| --- | --- | --- |
| 0 | `rt-shadows-off` | `Renderer::setRtShadows(false)`，回到纯光栅 + 全可见性 mask |
| 1 | `mesh-path-off` | `Renderer::setMeshShaders(false)`，回到顶点路径 |

阶梯只包含本次运行里真正可用的档：`--rt-shadows off` 或 `--mesh-shaders off` 时对应档不会进入阶梯。

## 3. CLI 与报告

```
build\NativeGPU.exe --smoke --no-watch --frames 240 --budget auto \
  --budget-target-ms 16.6 --budget-window 120 --budget-sustain 30
```

| 参数 | 含义 |
| --- | --- |
| `--budget auto` / `off` | 启用 / 关闭（默认 off，不改变既有证据） |
| `--budget-target-ms` | GPU 中位数目标（默认 16.6） |
| `--budget-cpu-ms` | CPU submit 中位数目标，0 = 不启用 |
| `--budget-vram-mb` | 显存峰值上限，0 = 不启用 |
| `--budget-window` | 滚动窗口帧数（默认 120） |
| `--budget-sustain` | 连续超目标帧数（默认 30） |

报告新增 `budget` 段：`enabled`、`status`（`disabled`/`within`/`downgraded`/`exhausted`）、`level`、`targetGpuMs`、`windowFrames`、`sustainFrames`、`samples`、`gpuSamples`、`overBudgetFrames`、`gpuP95Ms`、`cpuP95Ms`、`vramPeakBytes`、`ladder[]`、`steps[]`（每步含 `name`、`frame`、`reason`、`gpuMedianBeforeMs`、`cpuMedianBeforeMs`）。

## 4. 证据

`.incrementalTesting/rendering/budget.ps1`（18 项断言，全部来自真实运行）：

```
powershell -File .incrementalTesting\rendering\budget.ps1 -Frames 240
```

三次 smoke：目标 `0.0001 ms`（必然超）、目标 `100000 ms`（必然不超）、不带预算参数。断言覆盖：

- 强制超预算时走到阶梯末端（`exhausted`、`level=2`、两步依次为 `rt-shadows-off` → `mesh-path-off`）；
- 每步都带"over target"原因与当时的窗口中位数；
- 降档后报告的 `rtShadows.enabled` 确实变成 `false`；
- 宽松目标下 `status=within`、零步骤、阴影保持开启、仍有 GPU 百分位；
- 不带参数时 `status=disabled`、零步骤；
- 阶梯名与报告字段一致。

产物：`reports/latest/budget-evidence.json`。

单元测试在 `tests/budget_tests.cpp`（28 项，`npm run test:native` 会跑）：禁用、within、阶梯走完、短毛刺不触发、样本不足不决策、CPU 维度、显存维度、非法 GPU 样本不计数。

## 5. 已知限制

- **没有渲染缩放档**：阶梯只有两个"开关式"档位；按 0.75/0.5 渲染再上采样需要中间目标 + 放大 pass，属于后续工作。
- **降档不可回退**：超预算后即使恢复也不会自动升档（避免抖动），需要重启会话。
- **显存只观测不回收**：`vramPeakBytes` 只用于触发降档，没有真正的显存回收。
- **每插件性能契约未接**：R08 的"每插件预算"部分要等图形扩展（N1/L1）落地后才有对象可约束。
- **未在 1080p 目标下实测**：证据用的是 1280×720 的 smoke 路径；1080p 基线仍是 KPI 缺口。
