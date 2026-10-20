# 图像回归门禁（Q08 / INN-11）

- 版本：0.10.0 起
- 状态：**`courtyard-default` 基线已在真实捕获上验收**；阈值来自 2026-10-20 本机实测（实测值 + 明确余量，见 §4）
- 基线文件：`tests/image-baselines.json`；执行器：`tools/image-regression.ps1`
- 基线图片：`reports/image-baselines/courtyard-default.png`（`reports/` 被 gitignore，属于本机证据）
- 证据入口：`reports/latest/image-regression-evidence.json`（别名 `reports/image-regression-evidence.json`）
- 本机环境：NVIDIA GeForce RTX 5060 Laptop GPU，驱动 32.0.16.1714，1280×720

## 1. 目的与边界

`tools/error-contract.ps1` 比较的是**同一个构建的两条运行**（例如 DXR 阴影 on/off），没有历史概念。
本工具补上另一半：把**今天**的固定机位截图与**更早 commit 验收过的基线 PNG**比较，
让"这次改动有没有把画面弄坏"变成可执行的、带历史的门禁。

范围仍然只限**图像指标与证据字段**：

- 不断言帧时/性能：GPU 可能被其他进程共享，时间数字不可靠；
- 不判断"哪张图物理正确"：基线是人工验收的参照，不是 ground truth；
- 不做跨 GPU/驱动的可移植性承诺（见 §8）。

与 `error-contract.ps1` 的分工：

| | `error-contract.ps1` | `image-regression.ps1` |
| --- | --- | --- |
| 比较对象 | 同一构建的两条 CLI 状态 | 今天 vs 历史验收基线 |
| 历史 | 无 | `sourceCommit` + `acceptedAtUtc` + PNG SHA-256 |
| 指标 | 3 个（L1 类） | 4 个（含 8×8 block SSIM） |
| 失败处理 | 任一契约越界退出 1 | 任一阈值越界退出 1，并打印越界指标 |

## 2. 基线文件模式

`tests/image-baselines.json` 顶层字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `schema` | string | 固定 `nativegpu.image-baselines/1` |
| `frames` | int | 每次捕获的帧数；`--probe` 下至少 240 |
| `pixelThreshold` | int | `changedPercent` 的逐像素判定门限（0–255），当前为 8 |
| `ssim` | object | SSIM 常量声明：`blockSize`、`k1`、`k2`、`dynamicRange`、`luminanceWeights`；执行器会校验它们与实现的常量一致 |
| `measurement` | object | 实测环境与方法记录（适配器、驱动、日期、工具、方法） |
| `baselines` | array | 基线条目；当前只有一个 `courtyard-default` |

每个基线条目：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `id` | string | 唯一标识，也是 `-Baseline` 的取值；小写字母/数字/连字符 |
| `description` | string | 这个基线在钉住什么；失败时直接可读 |
| `scene` | string | 项目相对场景路径 |
| `camera` | string | `x,y,z,tx,ty,tz` 固定机位 |
| `resolution` | string | `宽x高`；必须是 8 的倍数（见 §3） |
| `screenshotFrame` | int | 截图帧号，当前 200 |
| `captureArgs` | string[] | 追加到捕获命令的开关；不允许覆盖 `--probe/--scene/--frames/--render-size/--camera/--screenshot-frame/--screenshot/--report`，也不允许绝对路径 |
| `metrics` | object | 四个指标的阈值与实测记录，见下 |
| `sourceCommit` | string | 验收该基线的完整 commit；由 `-Accept` 写入 |
| `acceptedAtUtc` | string | 验收时间（UTC ISO 8601）；由 `-Accept` 写入 |
| `sha256` | string | 基线 PNG 的 SHA-256（64 位小写十六进制）；由 `-Accept` 写入 |

每个指标对象的字段：

| 字段 | 含义 |
| --- | --- |
| `min` | 可选下界；测得值 `< min` 即失败 |
| `max` | 可选上界；测得值 `> max` 即失败 |
| `measured` | 定阈值时的实测值，供复盘；执行器只读不写 |

`min`/`max` 至少有一个，范围必须在指标合法区间内（§3），且 `min <= max`。
四个指标都必须声明，不能少也不能多。`measured` 不会被 `-Accept` 自动改写：
**阈值与实测值是人审字段**，`-Accept` 只写 `sourceCommit`、`acceptedAtUtc`、`sha256`。

当前基线的关键声明：

```text
id            courtyard-default
scene         assets/courtyard.scene.json
camera        9,5.5,10,-2,0.8,0
resolution    1280x720
frame         200
capture       --probe --frames 240 --render-size 1280x720 --camera <camera>
```

## 3. 四个指标

四个指标在同一次全分辨率扫描里一起算出，不抽样。图片按 `System.Drawing` 的
`LockBits(Format32bppArgb)` 读成 BGRA 字节；只比较 R/G/B，alpha 不参与。

| 指标 | 定义 | 范围/精度 | 方向 |
| --- | --- | --- | --- |
| `meanAbsError` | 所有像素、R/G/B 三通道绝对差 `abs(a-b)` 的算术平均 | 0–255，4 位小数 | 越低越好 |
| `changedPercent` | 逐像素取三通道最大差，`> pixelThreshold` 记为变化；变化像素占全图百分比 | 0–100，2 位小数 | 越低越好 |
| `maxChannelDelta` | 全图内最大的**单通道**绝对差（不是三通道之和） | 0–255，整数 | 越低越好 |
| `ssim` | 8×8 分块的 SSIM 近似，亮度通道，块内总体矩，最后对块取平均 | -1–1，8 位小数 | 越高越好（1 = 相同） |

`pixelThreshold` 取 8（8/255 ≈ 3%）：低于它的差异属于写入/编解码舍入噪声量级。
注意 `rt-shadows.ps1` 的 `maxChannelDelta` 是三通道差之和（0–765），本工具与
`error-contract.ps1` 一致，用的是单通道最大差（0–255），两个数字不可直接比较。

### 3.1 Block SSIM 公式

对每个完整的 8×8 块，先把像素转成 Rec.709 亮度（0–255 量纲）：

```text
Y = 0.2126*R + 0.7152*G + 0.0722*B
```

设块内两幅图亮度分别为 `x`、`y`，`N = 64`，使用**总体矩**（除以 N，不是 N-1）：

```text
muX  = (1/N) * sum(x)
muY  = (1/N) * sum(y)
varX = (1/N) * sum(x*x) - muX*muX
varY = (1/N) * sum(y*y) - muY*muY
covXY = (1/N) * sum(x*y) - muX*muY

C1 = (0.01 * 255)^2 = 6.5025
C2 = (0.03 * 255)^2 = 58.5225

SSIM_block = ((2*muX*muY + C1) * (2*covXY + C2))
             / ((muX^2 + muY^2 + C1) * (varX + varY + C2))

ssim = (1 / blockCount) * sum(SSIM_block)
```

1280×720 时 `blockCount = (1280/8) * (720/8) = 160 * 90 = 14400`。
每个像素都必须属于完整块，因此基线分辨率被限制为 8 的倍数；1280×720 没有余数。

这是**近似**，不是标准 SSIM 实现：

- 没有高斯窗，块内等权；
- 只算亮度，不算色度；
- 没有多尺度；
- 均值对"少数像素的局部变化"不敏感：一个块坏掉时，14400 个块的均值只会轻微下降。
  这正是同时保留 `maxChannelDelta` 和 `meanAbsError` 的原因：局部亮斑由前者抓，
  面积/整体偏移由后者抓。

## 4. 阈值策略与本次实测

原则：**先测量，再取整/加余量；没有运行过的数字不写进文件**。

1. 先 `-Accept` 生成基线（见 §6）；
2. 无 `-Accept` 跑一次比较，读控制台或证据里的四个实测值；
3. 每个上界/下界 = 实测值 + 能吸收合法波动的余量（余量必须写进文档，不能悄悄调大）；
4. 重跑一次确认通过；
5. 以后改阈值必须同时更新 `measured` 与本表；不允许为了让门禁变绿而单方面放宽。

2026-10-20 在本机（RTX 5060 Laptop，驱动 32.0.16.1714）实测，基线图与重新捕获**逐字节相同**：

| 指标 | 实测 | 阈值 | 余量 | 说明 |
| --- | --- | --- | --- | --- |
| `meanAbsError` | 0 | `max 0.1` | +0.1 / 255 | 允许极少量 1 级舍入；整体亮度偏移仍会立刻越界 |
| `changedPercent` | 0 | `max 0.01` | +0.01 个百分点（≈92 px / 921600） | 面积型回归的最小可见余量 |
| `maxChannelDelta` | 0 | `max 1` | +1 个 8-bit 级 | 任何可见的局部亮斑（≥2）都会失败 |
| `ssim` | 1.00000000 | `min 0.999` | -0.001 | 1.0 为逐字节相同；留出重构误差量级的余量 |

捕获参数：`--probe --scene assets/courtyard.scene.json --frames 240 --render-size 1280x720
--camera 9,5.5,10,-2,0.8,0 --screenshot-frame 200`。两次独立进程的 PNG SHA-256 均为
`baca62f4a954864ff5177b7775ce5b19b5e811cb07a3e273f4bbea25a24088f3`，
因此 0 不是"这次运气好"，而是当前渲染器在该机位上的真实可复现结果。

余量的含义要说清楚：它不是从多次噪声采样统计出来的置信区间，而是人工选择的容忍带；
当前实测是 0，所以任何余量都无法被"实测"本身证明，只能由后续运行检验。若未来合法改动
（驱动升级、有意改渲染）导致 1 级以内的微小差异，按 §6 重新验收；若差异超过上表，先当作
回归排查，不要直接放宽。

## 5. 运行方式与证据

```powershell
# 构建 + 验收：生成/更新 reports/image-baselines/courtyard-default.png，并写回 commit/时间/SHA-256
powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 -Accept

# 构建 + 门禁：今天 vs 已验收基线，任一阈值越界退出 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1

# 复用已有构建，指定基线 id
powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 -Baseline courtyard-default -SkipBuild

# 用另一份阈值文件（负测试/临时实验）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 -Baselines reports/tmp/image-baselines-negative.json -SkipBuild
```

行为：

- 默认先调 `tools/build.ps1`；`-SkipBuild` 只复用 `build/NativeGPU.exe`，文件不存在时仍会构建。
- 每次运行创建一个新的 `reports/<version>/<commit12>/<stamp>/` 目录，绝不覆盖旧证据。
- 捕获校验：进程退出码 0、报告 `success=true`、`resolution` 与配置一致、
  `rtShadows.screenshotFrame` 一致、`rtShadows.screenshotTaken=true`、PNG 存在；
  任何一步失败都直接报错，不会拿半张图去比较。
- 比较前校验基线 PNG 的 SHA-256 必须等于 JSON 的 `sha256`；缺失或不匹配都直接报错退出 1，
  提示先审查图片再 `-Accept`。
- 四次指标计算 + diff PNG 生成在同一次扫描中完成。

运行目录里的产物：

| 产物 | 路径 | 说明 |
| --- | --- | --- |
| 捕获 PNG | `artifacts/<id>-captured.png` | 本次真实截图 |
| diff PNG | `artifacts/<id>-diff.png` | 逐像素通道绝对差，`min(255, delta*4)`，alpha 不透明；全黑 = 相同 |
| 证据 JSON | `image-regression-evidence.json` | 指标、阈值、通过/失败、产物路径、环境 |
| 原始捕获报告 | `stage/<id>-capture.json` | 渲染器自己的 `--report` |
| 运行清单 | `run.json` | `report-lib.ps1` 的标准 manifest |

`Register-Report` 会把证据复制到 `reports/latest/image-regression-evidence.json`，
并写别名 `reports/image-regression-evidence.json`。基线 PNG 固定在
`reports/image-baselines/<id>.png`，不复制进每次运行目录（证据里给路径和 SHA-256）。

### 5.1 证据字段

顶层：`schema`、`version`、`commit`、`worktreeDirty`、`mode`（`accept`/`compare`）、
`baselinesFile` + `baselinesFileSha256`、`baseline`（id、路径、当前/声明/上一份 SHA-256、
sourceCommit、acceptedAtUtc、camera、resolution、阈值）、`scene` + `sceneSha256`、`frames`、
`pixelThreshold`、`measurement`、`ssim`、`adapter`、`capture`（路径、SHA-256、报告、分辨率、
帧数、适配器、截图帧、截图成功、完整参数）、`metrics`（四个指标 + 变化像素数/总像素数/块数）、
`metricsBasis`、`thresholds`、`checks[]`、`violations[]`、`gateEvaluated`、`accepted`、
`acceptance`、`diff`、`artifacts`、`success`、`failure`。

`metricsBasis` 说明本次指标是相对谁算的：

| 值 | 含义 |
| --- | --- |
| `accepted-baseline` | 门禁模式：已验收基线 vs 本次捕获 |
| `previous-baseline` | `-Accept` 且旧基线存在：旧基线 vs 本次捕获（记录 drift，但不据此判失败） |
| `self-bootstrap` | `-Accept` 且没有旧基线（或旧基线分辨率不兼容）：本次捕获与自己比较，diff 必然全黑 |

失败时 `violations[]` 的形状（自描述，不依赖控制台文本）：

```json
{
  "baseline": "courtyard-default",
  "metric": "changedPercent",
  "measured": 0.0,
  "bounds": { "min": 100.0, "max": 100.0 },
  "detail": "measured 0 is below floor 100"
}
```

`checks[]` 对四个指标逐项记录 `metric`、`measured`、`bounds`、`pass`、`detail`，
即使全部通过也会写全，便于趋势复盘。

## 6. `-Accept` 工作流

`-Accept` 是**显式的人工验收动作**，不是自动更新：

1. 跑一次捕获并校验成功；
2. 如果已有旧基线，先算"旧基线 vs 新捕获"的 drift，写进证据（`metricsBasis=previous-baseline`）；
   旧基线尺寸不兼容时跳过比较并在 `acceptance.previousBaselineProblem` 记录；
3. 把捕获 PNG 复制到 `reports/image-baselines/<id>.png`（字节级复制并校验 SHA-256）；
4. 更新 `tests/image-baselines.json` 中该条目的 `sourceCommit`、`acceptedAtUtc`、`sha256`；
5. 写证据、`run.json`，退出 0。

`-Accept` **不会**因为指标越界而失败——验收本身就是"接受新画面"的决定。因此它：

- 不写 `checks[]`/`violations[]`（`gateEvaluated=false`）；
- 只把 drift 打印为 `drift` 行并写入证据；
- **不修改阈值和 `measured`**。如果这次改动是合法的，人需要随后按 §4 重新测量、更新阈值；
  如果只是想让失败的 CI 变绿，先停下来看 drift 指标。

验收后必须再跑一次无 `-Accept` 的比较，确认新基线能通过门禁。

## 7. 失败语义与负测试

无 `-Accept` 时：

- 任一指标越界 → 控制台打印 `FAIL <metric> <measured> (bounds ...) - <detail>`，退出 1；
- 基线 PNG 缺失、`sha256` 为空、SHA-256 不匹配、分辨率不一致 → 直接报错退出 1；
- 配置文件 schema/字段/范围/场景/相机/分辨率/`captureArgs` 不合法 → 直接报错退出 1。

负测试（2026-10-20 实际执行）：把基线文件复制到临时路径，把 `meanAbsError` 上界改成 `0`，
并给 `changedPercent` 加上不可能满足的 `min: 100`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/image-regression.ps1 `
  -SkipBuild -Baselines reports/tmp/image-baselines-negative.json
```

实际输出（退出码 1）：

```text
  ok   meanAbsError       0 (bounds max 0)
  FAIL changedPercent     0 (bounds min 100, max 100) - measured 0 is below floor 100
  ok   maxChannelDelta    0 (bounds max 1)
  ok   ssim               1.00000000 (bounds min 0.99900000)

Image regression: 1 check(s) FAILED
```

`meanAbsError` 实测正好 0，所以 `max 0` 仍然满足；真正越界的是 `changedPercent`。
这说明门禁在"数值恰好等于上界"时按 `<=` 通过、在低于下界时按 `<` 失败，且失败时会点名指标。

## 8. 明确"尚未实现"

| 项 | 现状 | 依赖/说明 |
| --- | --- | --- |
| 跨 GPU/驱动基线 | 无。基线 PNG 在 `reports/`（gitignore），只属于验收它的机器；驱动/适配器变了应重新 `-Accept` | 需要多机型矩阵；当前证据记录适配器与驱动版本，但没有自动比对 |
| 基线 PNG 入库 | 无。`reports/` 被 gitignore，新克隆没有基线；首次必须 `-Accept`，否则门禁报"baseline missing" | 若要把基线变成可分发资产，需要另建非 reports 的存储与 LFS 策略 |
| 分区掩码/ROI | 无。整帧一个阈值；HUD、UI、字幕等局部变化无法豁免 | 需要区域定义 + 每区域阈值 |
| CI 自动更新基线 | 有意不做。`-Accept` 只在人工审阅后运行；CI 只跑比较 | 防止"失败就自动接受"把回归洗掉 |
| 色度/ΔE/色彩管理 | 无。SSIM 只用亮度，L1 指标在显示空间算；没有 sRGB 标注/线性化 | 见 `docs/OUTPUT-CONTRACT.md` |
| 多尺度/感知 SSIM | 无。只有 8×8 单尺度近似 | 需要固定窗口与色彩空间后再升级 |
| 帧时/性能断言 | 有意不做 | 本文件只管图像与证据字段 |
| 自动阈值推荐 | 无。阈值必须人审；执行器不会根据实测自动放宽 | 与 §4 的策略一致 |

## 9. 相关文件

- `tests/image-baselines.json`：基线、阈值、`sourceCommit`/`acceptedAtUtc`/`sha256`（本工作包新增）
- `tools/image-regression.ps1`：捕获、四个指标、diff PNG、证据、`-Accept`（本工作包新增）
- `tools/report-lib.ps1`：运行目录、`Register-Report`、`Publish-Artifact`、`Complete-Run`
- `tools/error-contract.ps1` / `tests/image-contracts.json`：同一构建内的图像误差契约（本工作包未修改）
- `tools/rt-shadows.ps1`：DXR 阴影证据脚本（本工作包未修改）
- `docs/ERROR-CONTRACTS.md`：误差契约的指标定义与阈值方法
- `docs/OUTPUT-CONTRACT.md`：输出色彩/格式的现状
- `docs/BUDGET.md`：同一批工作包的文档风格参考
