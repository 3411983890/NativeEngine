# 图像误差契约（R07 / INN-03）

- 版本：0.8.0 起
- 状态：**首批 5 个契约已在真实捕获上全部通过**；阈值全部来自 2026-10-20 的本机实测，没有手写估计值
- 契约文件：`tests/image-contracts.json`；执行器：`.incrementalTesting/rendering/error-contract.ps1`
- 证据入口：`reports/latest/error-contract-evidence.json`（别名 `reports/error-contract-evidence.json`）
- 本机基线：NVIDIA GeForce RTX 5060 Laptop GPU，驱动 32.0.16.1714，DXC 1.9.2609，1280×720

## 1. 目的与边界

把"两张截图看起来差不多"变成可执行的契约：阈值只写在版本化的 `tests/image-contracts.json` 里，
执行器负责跑两条真实捕获、算指标、比对阈值，并把实测值、阈值、通过/失败和截图路径写成机器可读证据。
任何阈值都必须先有实测值，再取整、再加边界，见 §4。

范围只限**图像指标与报告字段**。不断言帧时/性能：GPU 可能被其他进程共享，时间数字在这种契约里不可靠。

## 2. 契约文件模式

顶层字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `schema` | string | 固定 `nativegpu.image-contracts/1` |
| `scene` | string | 项目相对路径，默认 `.screen/courtyard.scene.json` |
| `frames` | int | 每次捕获的帧数；`--probe` 下至少 240 |
| `pixelThreshold` | int | `changedPercent` 的逐像素判定门限（0–255） |
| `measurement` | object | 实测环境记录（适配器、驱动、DXC、日期、方法） |

每个契约：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `id` | string | 唯一标识，也是 `-Contract` 的取值 |
| `description` | string | 这个契约在证明什么；失败时直接可读 |
| `reference` | string[] | 参考运行的**附加**命令行参数 |
| `candidate` | string[] | 候选运行的附加命令行参数 |
| `metric` | enum | `meanAbsError` / `changedPercent` / `maxChannelDelta` |
| `expect` | enum | `identical` / `changed` |
| `tolerance` | number 或 `{min,max}` | `identical` 用数字上限；`changed` 用闭区间 |
| `resolution` | string | `宽x高`，两条运行都使用 |
| `camera` | string | `x,y,z,tx,ty,tz` 固定机位 |
| `screenshotFrame` | int | 截图帧号（两条运行相同） |
| `measured` | number | **实测值**，发布时填写；运行时执行器不会改写契约文件 |
| `measuredMetrics` | object | 三个指标的完整实测记录，便于复盘 |

执行器固定注入下面这些参数，契约的 `reference`/`candidate` 只能追加开关：

```text
--probe --scene <scene> --frames <frames>
--render-size <resolution> --camera <camera> --screenshot-frame <screenshotFrame>
--screenshot <本次运行目录>/stage/<id>-<role>.png
--report <本次运行目录>/stage/<id>-<role>.json
```

脚本会拒绝契约覆盖 `--probe`、`--scene`、`--frames`、`--render-size`、`--camera`、
`--screenshot-frame`、`--screenshot`、`--report`，也会拒绝绝对路径；所有传给
`Start-Process` 的路径都是项目相对路径（项目目录本身带空格）。

## 3. 三个指标

三条指标在同一遍全分辨率扫描里一起算出来，不抽样：

| 指标 | 定义 | 范围/精度 | 用途 |
| --- | --- | --- | --- |
| `meanAbsError` | 所有像素、R/G/B 三个通道的绝对差 `abs(a-b)` 的算术平均 | 0–255，4 位小数 | **0 表示逐字节一致**；对整体亮度/颜色偏移敏感 |
| `changedPercent` | 逐像素取三通道最大差，`> pixelThreshold` 记为变化；变化像素占全图百分比 | 0–100，2 位小数 | 衡量"变了多少面积"；用于 `changed` 契约的下限/上限 |
| `maxChannelDelta` | 全图内最大的单通道绝对差 | 0–255，整数 | 捕捉"面积很小但很亮"的错误；`meanAbsError` 会把它稀释 |

`pixelThreshold` 取 **8**（8/255 ≈ 3%）：低于它的差异属于写入/编解码舍入噪声量级；
把它设成 0 会让压缩噪声也算"变化"，设得更高则会漏掉真正的细线/边缘回归。

与 `.incrementalTesting/rendering/rt-shadows.ps1` 的差异（两个指标同名但语义不同，不要混用）：

- 旧脚本的 `maxChannelDelta` 是三通道差**之和**（0–765），这里的是**单通道**最大差（0–255）；
- 旧脚本的亮度统计每 4 个像素采样一个，这里的三个指标都是全像素；
- 旧脚本只做"有没有变"，这里还要求"变在声明的区间内"。

## 4. 阈值如何确定

1. 先真实跑两条捕获（`-Contract <id>` 只跑这一个契约）；
2. 记录 `measured` 与完整 `measuredMetrics`；
3. 按指标量级取整（`changedPercent` 取整数百分点，`meanAbsError` 保留到 0.1 或整数）；
4. 加一个能吸收合法波动的边界：`changed` 用实测值两侧的区间，`identical` 只在**两次独立进程**
   逐字节一致时才允许 `0`；
5. 把取整值与边界写回契约文件，再整跑一次确认；
6. 阈值变化必须同时更新 `measured`，否则下一个人无法判断这是回归还是重新基线。

本机实测与提交的阈值：

| 契约 | 指标 | 实测 | 其它实测 | 阈值 | 结果 |
| --- | --- | --- | --- | --- | --- |
| `shadows-off-vs-raster` | `meanAbsError` | 0 | changed 0 / max 0 | `0` | 通过 |
| `shadow-on-vs-off` | `changedPercent` | 11.73 | meanAbsError 6.0698 / max 121 | `[10, 14]` | 通过 |
| `mesh-off-vs-vertex` | `changedPercent` | 59.25 | meanAbsError 36.3978 / max 241 | `[50, 70]` | 通过（放宽，见 §7） |
| `mesh-fallback-stability` | `meanAbsError` | 0 | changed 0 / max 0 | `0` | 通过 |
| `debug-mask-only` | `changedPercent` | 100 | meanAbsError 212.2275 / max 255 | `[95, 100]` | 通过 |

取整与边界：

- `shadow-on-vs-off`：11.73 → 12，上下各留 2 个百分点；
- `mesh-off-vs-vertex`：59.25 → 60，上下各留 10 个百分点（该路径本身是已知差异，见 §7）；
- `debug-mask-only`：100 → 100，下限 95，留 5 个百分点；
- 两个 `identical` 契约：0，不留边界。重复运行已验证同一命令的 PNG SHA-256 完全相同
  （off×2、on×2、mesh-off×2），因此 0 不是"运气好"，任何非零都是真回归。

注意：`identical` 的 0 绑定到本机当前渲染器行为；换 GPU/驱动后若出现差异，按 §4 重新测量并裁决，
不要直接把 `tolerance` 调大来掩盖。

## 5. 运行方式与证据

```powershell
# 全部契约（默认先构建；构建失败不会产生假通过）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/error-contract.ps1

# 只跑一个契约（测量新契约时用）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/error-contract.ps1 -Contract shadow-on-vs-off

# 复用已有构建
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/error-contract.ps1 -SkipBuild
```

行为：

- 每个契约跑**两次** `Start-Process -Wait` 捕获；`--rt-shadows off` 对自身、`on` 对 `off` 等都是真实进程；
- 用 `System.Drawing` 的 `LockBits` 读回整帧（沿用 `rt-shadows.ps1` 的做法），算 §3 的三个指标；
- 控制台每个契约打印一行 `ok` / `FAIL`，任一违规退出码 1；
- 截图复制到本次运行目录的 `artifacts/<id>-reference.png` / `artifacts/<id>-candidate.png`；
- 证据写入 `reports/<version>/<commit12>/<stamp>/error-contract-evidence.json`，并注册
  `reports/latest/` 与 `reports/error-contract-evidence.json` 别名。

顶层证据字段：`schema`、`version`、`commit`、`worktreeDirty`、`contractsFile`（含 SHA-256）、
`scene`（含 SHA-256）、`frames`、`pixelThreshold`、`measurement`、`adapter`、`contracts[]`、
`violations[]`、`checks[]`、`success`。

每个契约项包含：`id`、`description`、`metric`、`expect`、`tolerance`、`measured`、`pass`、
`detail`、`metrics`（三个指标 + 变化像素数/总像素数）、`referenceArgs`、`candidateArgs`、
`resolution`、`camera`、`screenshotFrame`、`images`（artifact 相对路径）、`stageImages`、
`reports`（两次捕获的原始 JSON）、`imageSha256`、`reportFields`（帧数、分辨率、适配器、两次
`success` 与 `screenshotTaken`）。

失败时 `violations` 的形状（自描述，不依赖控制台文本）：

```json
{
  "contract": "shadow-on-vs-off",
  "metric": "changedPercent",
  "measured": 11.73,
  "expected": "changed",
  "tolerance": { "min": 12.0, "max": 14.0 }
}
```

`expected` 是契约的期望类型（`identical` / `changed`），`tolerance` 是提交的阈值（数字或区间）。
执行器还会校验捕获本身：退出码 0、报告 `success=true`、分辨率与契约一致、`screenshotFrame` 一致、
`rtShadows.screenshotTaken=true`、截图文件存在；任何一步不满足都直接报错，不会拿半张图去比对。

## 6. 如何新增一个契约

1. 选两条**已存在**的 CLI 状态（例如 `--rt-shadows on` 与 `--rt-shadows off`），只追加开关；
2. 在 `tests/image-contracts.json` 里先加条目，`tolerance` 临时放一个必过的宽区间
   （`changed` 用 `[0,100]`，`identical` 用该指标最大值），只为测量；
3. `-Contract <new-id>` 跑一次，读控制台或证据里的 `measured` / `measuredMetrics`；
4. 按 §4 取整、加边界，写回 `tolerance` 与 `measured`；
5. 整跑一次；必要时更新 `measurement` 的适配器/驱动/日期；
6. 如果要改**已有**契约，先判断是回归还是合法变化。合法变化必须同时更新 `measured` 与
   `tolerance`，并在描述里写清原因；不允许只调阈值让其变绿。

CI 接线由父工作包负责；本工作包不修改 `.incrementalTesting/core/verify.ps1`、`.incrementalTesting/core/ci.ps1` 或 `package.json`。

## 7. 已放宽的契约（如实记录，不假装通过）

原任务希望 `mesh-off-vs-vertex` 是 `identical`，但本机实测**无法成立**：

- `--mesh-shaders off`（顶点路径）vs 默认 mesh 路径：`changedPercent = 59.25`、
  `meanAbsError = 36.3978`、`maxChannelDelta = 241`；
- 用 `--debug-view base` 把 key light 项拿掉后仍有 59.13% 的像素不同、`meanAbsError = 15.86`，
  说明差异不只来自阴影合成项，几何/法线/绕序本身也不一致；
- `docs/MESH-BINDLESS.md` §1「已知差异」已经写明 mesh 路径整体偏暗、法线与绕序尚未对齐。

因此：

- `mesh-off-vs-vertex` 保留 id，但期望从 `identical` **放宽为 `changed`**，阈值固定为实测的
  `[50, 70]`。它现在的职责是"把这条已知差异钉住"：mesh 路径被悄悄换成顶点路径（值趋近 0）
  或画面进一步坏掉（越过上限）都会失败；将来修好差异后，该契约也应失败并促使我们重新基线。
- 额外增加 `mesh-fallback-stability`（两条 `--mesh-shaders off` 独立运行，`meanAbsError = 0`），
  覆盖原意中"回退路径位稳定"的部分。
- 其余三个必需契约（`shadows-off-vs-raster`、`shadow-on-vs-off`、`debug-mask-only`）按原意通过，
  没有放宽或删除。

## 8. 明确"尚未实现"

| 项 | 现状 | 依赖/说明 |
| --- | --- | --- |
| 路径追踪参考渲染器 | 无。`reference` 是"另一条真实 CLI 运行"，不是 ground truth；它能证明两条路径的差异，不能证明哪条"物理正确" | compute pipeline **B1** 落地后才有可用的参考帧 |
| SSIM / ΔE / 感知指标 | 只有逐像素 L1 类指标；没有结构相似度、没有 Lab/ΔE、没有色彩管理 | 需要先冻结色彩空间与参考图；当前输出连 sRGB 标注都没有（见 `docs/OUTPUT-CONTRACT.md`） |
| 历史重置（history reset） | 无显式重置 API。每个契约都起新进程、天然没有跨运行历史；但这不等于实现了"同一进程内清历史" | 需要持久会话 + reset 接口；TAA/降噪等有状态效果出现后必须补 |
| 跨 GPU/驱动基线 | 阈值只在本机 RTX 5060 Laptop + 驱动 32.0.16.1714 上实测 | 需要多机型矩阵；CI 目前只跑本机 |
| 性能/时序断言 | 有意不做 | 其他进程可能共享 GPU；本文件只约束图像与报告字段 |
| 自动更新阈值 | 有意不做 | `measured`/`tolerance` 必须人工审阅，防止"改到通过" |

## 9. 相关文件

- `tests/image-contracts.json`：契约与阈值（本工作包新增）
- `.incrementalTesting/rendering/error-contract.ps1`：执行器（本工作包新增）
- `tools/report-lib.ps1`：运行目录、`Register-Report`、`Complete-Run`
- `.incrementalTesting/rendering/rt-shadows.ps1`：DXR 阴影的既有证据脚本（本工作包未修改）
- `docs/MESH-BINDLESS.md`：mesh 路径已知差异
- `docs/BUDGET.md`：同一批工作包的预算管理器文档，本文按其风格书写
