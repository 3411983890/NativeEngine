# 确定性契约（E07 / INN-01）

原生模拟必须"同样输入 → 同样结果"。这不是为了好看：回放证据、世界分叉 CI、图像回归门禁、崩溃复现全都依赖它。本文件说明契约内容、CLI 用法、证据位置和已知限制。

## 1. 规则（代码层面的硬约束）

1. **模拟代码禁止进程级随机源**：`std::rand`、`srand`、`std::random_device` 一律不允许出现在 `engine/native/`。`.incrementalTesting/core/determinism.ps1` 会静态扫描并在违规时失败。
2. **模拟代码禁止读墙上时钟**：`std::time`、`GetTickCount`、`QueryPerformanceCounter`。帧间隔只允许由宿主传进 `Game::update(input, dt)`。
3. **所有随机消费必须命名**：Lua 使用 `ctx:random(name,bound)`，随机流状态进入快照；原生每个消费者从 `determinism::registry().stream("<name>")` 取流。新增消费者不会移动其它流已经依赖的序列。
4. **摘要编码固定**：原生数值按 1e-4 网格量化；玩法组件、Lua 显式状态和队列按有序 JSON 序列化后取摘要。非有限状态拒绝提交。同机、同构建保证，不承诺跨平台或优化级别一致。
5. **渲染不得反馈进模拟**：分辨率、`--debug-view`、DXR/Mesh 开关都不允许改变摘要（已作为证据断言）。

## 2. 组成

| 组件 | 文件 | 作用 |
| --- | --- | --- |
| `Hasher` | `engine/native/determinism.hpp/.cpp` | FNV-1a 64，整数运算，跨编译器一致；`quantized()` 做网格量化 |
| `Stream` | 同上 | xoshiro128\*\*，splitmix64 播种；`next/unit/range` + 计数器 |
| `Registry` | 同上 | 按名字持有流（`std::deque`，引用稳定）；`fingerprint()` 覆盖种子、流名与计数器 |
| `Game::digest()` | `engine/native/game.cpp` | 对 tick、玩家位置/速度、相机角、动画相位/速度、反馈计时、玩法标志、物理刚体状态（位置/四元数/速度/休眠）、损伤与断裂计数，以及派生出的渲染姿态列表取摘要 |
| `--hash-log` | `host/main.cpp` | 每帧一行 `frame,tick,hash` + 输入列；文件头记录 `seed`、固定步长与 replay 版本 |

摘要**不包含**渲染状态、窗口尺寸、GPU 时序或墙钟时间——它们不是模拟输入。

## 3. CLI

```
build\NativeGPU.exe --smoke --no-watch --frames 240 --hash-log reports/x.log --seed 20261020
```

- `--seed <u64>`：本次运行的种子（默认 `20261020`），重置所有命名流。
- `--hash-log <path>`：逐帧摘要日志，`\n` 行尾、无 BOM。
- 报告新增 `determinism` 段：`seed`、`fixedStepHz`、`ticks`、`hashLines`、`finalHash`、`rng.streams[]`、`rng.fingerprint`。

## 4. 证据

`.incrementalTesting/core/determinism.ps1`（19 项断言，全部来自真实运行）：

```
powershell -File .incrementalTesting\core\determinism.ps1 -Frames 240
```

它跑 5 次 smoke：基线两次、640×360 一次、`--debug-view mask` 一次、`--seed 7` 一次，然后断言：

- 两次基线摘要日志**逐字节相同**；
- 分辨率与调试视图不改变摘要；
- 240 帧 = 480 tick（120 Hz 固定步长，2 tick/帧）；
- 报告 `ticks` / `finalHash` 与日志末行一致；
- 两个命名流（`gameplay`、`cosmetic`）已注册，`fingerprint` 为 16 位十六进制；
- 换种子后 `fingerprint` 变化；
- 静态扫描：`engine/native/` 无全局随机源与墙上时钟调用。

产物：`reports/latest/determinism-evidence.json`（含 `checks[]`、`identicalRuns`、`resolutionIndependent`、`seedSensitive`）。

## 5. 已知限制（不要在别处夸大）

- **输入日志尚未记录**：当前日志只有 `frame,tick,hash`，不带输入列。输入记录与回放读取留给 E08（黑匣子）。
- **目前还没有消费者抽随机数**：`gameplay` / `cosmetic` 两个流已注册但计数为 0，所以换种子时摘要不变（脚本把这条当作显式预期断言，改变的那天会立刻暴露）。
- **只验证了同机同二进制**：量化网格为跨平台留了余量，但没有在第二台机器/第二个编译器上实测。
- **摘要覆盖的是模拟状态**，不是完整世界状态：材质、灯光、场景文件不在其中，它们由场景哈希单独覆盖。
