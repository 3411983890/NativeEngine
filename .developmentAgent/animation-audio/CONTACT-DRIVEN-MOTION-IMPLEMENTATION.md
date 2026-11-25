# 接触驱动动作系统实现（P1 单支撑 · P2 双支撑与转向）

状态：已实现求解器侧的单支撑稳定（滑动收敛见 P1.1）、共享骨盆补偿、双支撑同时评估、脚掌滚动与接触度量；参考库（P3）、编辑器时间轴（P5）与多接触（后续）未包含。
日期：2026-10-27
范围确认：在既有实时角色 Rig 与示例工程 `.screen/character-rig.scene.json` 上继续推进，不重写骨架与导入链。

方案与验收边界见 [CONTACT-DRIVEN-MOTION-DEVELOPMENT.md](CONTACT-DRIVEN-MOTION-DEVELOPMENT.md)；首版 Rig 记录见 [REALTIME-CHARACTER-RIG-IMPLEMENTATION.md](REALTIME-CHARACTER-RIG-IMPLEMENTATION.md)。

## 1. 接触约束契约

`engine/native/animation.hpp` 新增接触层，取代“锁脚 = 一个踝目标”的隐含约定：

- `ContactMode`：`Free` / `Flat` / `Heel` / `Toe`。整脚、脚跟、脚尖三种接地方式，`Free` 表示不受地面约束。
- `ContactRelease`：`None` / `Lift` / `Drift` / `Overreach` / `Turn` / `Surface` / `Unwalkable` / `GroundLost` / `Teleport` / `Blend`。每次解除锚点都带原因，可在报告里逐脚读取，并计入 `FeatureMetrics::releaseCounts`。
- `FootState` 增加 `mode`、`release`、`roll`、`pivot`/`previousPivot`、`pivotValid`、`pivotRoll`、`pivoting`。
- `FeatureState::pelvisShift`：世界空间水平骨盆补偿（y 恒为 0），参与摘要与存档。
- `RigSettings` 增加 `maxPelvisShift`（模型单位，默认 0.07）与 `soleRoll`（弧度，默认 0.45）。两者支持资产默认值、场景实例覆盖与 `humanoid-rig.mjs` 自动诊断默认值。
- `FeatureMetrics` 增加 `anchorError`、`contactSlip`、`pivotSlip`、`soleAngle`、`switchJump`、`pelvisShiftUsed`、`supportSwitches`、`toeOffs`、`heelStrikes`、`releaseCounts`。均按 tick 统计，不进入模拟摘要。

## 2. 求解顺序（`Correct` 阶段）

1. 地面取样与接触意图：脚本相位窗口只作意图，落锁要求“踝在鞋底平面附近且世界速度足够小”。新增**落锁前可达性预测**：目标超出 `legReach × 1.01 + 1 cm × scale` 时不再起锚，把腿交还片段并报告 `unreachable`。
2. 转向残差与换步判定：不变；解除锚点补上 `release = Turn`。
3. 逐脚目标：锚点 → 转向枢轴 → 静止补扭 → 接触模式与脚掌滚动。
4. **共享骨盆求解**：所有有效接触一起投票。垂直取最深需求（哪只脚要求下沉就下沉），水平取锚点需要的平均位移，仅在 `required / reach > 0.93` 时介入并按 `maxPelvisShift × scale` 限幅；有超伸时先按超伸量下沉骨盆，仍不可达才释放。
5. 双腿 IK 与脚掌朝向：保留原有两骨求解与鞋底对齐，增加绕接触边的滚动俯仰，脚趾保持静息平面（鞋在跖趾处折弯而不是整只翘起）。
6. 接触度量：在**鞋底接触点**而不是踝上测量滑动；主动枢轴转动（转向外旋、静止补扭）单独计入 `pivotSlip`，不算意外滑动。

### 2.1 脚掌滚动

接触窗口内按相位分三段：前 18% 允许脚跟先落（负向滚动 0.35 × 上限），72% 之后进入脚尖滚动（上限 `soleRoll`）。滚动是绕接触边的刚体旋转：

```text
surface = target - normal × soleHeight
edge    = surface + forward × (roll > 0 ? 0.55 : roll < 0 ? -0.35 : 0) × footLength
target  = edge + R(forward × normal 轴, roll) × (target - edge)
```

踝目标与脚骨朝向用同一角度、同一力臂；测量用同一力臂反解接触点，因此“滚过去”不会报成滑动。滚动只在 `Walk`/`Run` 状态生效：声明整周期接触的 idle 片段不会周期性踮脚。

### 2.2 维持与释放

- 维持：高度滞回 + 锚点漂移（`maxFootDrift = max(0.06, min(maxFootCorrection, 0.3)) × scale`）。漂移上限现在也约束“片段把脚画在踏面里”的重锁路径，避免身体走远后仍保持旧锚点。
- 采集与维持分离：已锁定的脚只由滞回决定去留，不再被“相位窗口 + 落地速度”每 tick 重新起锚。此前这正是“脚被拖在身后 0.4 m”的来源。
- IK 超伸：仍按 `legReach + 4 cm × scale` 释放并报告 `Overreach`，不做无限拉伸、不隐藏误差。

## 3. 诊断与报告

`host/main.cpp` 的报告新增 `animation.feet[].mode/release/roll/pivot`，以及 `anchorErrorM`、`contactSlipMps`、`pivotSlipMps`、`soleAngleRad`、`supportSwitchJumpM`、`supportSwitches`、`toeOffs`、`heelStrikes`、`pelvisShiftM`、`maxPelvisShift`、`soleRoll`。编辑器与外部工具可直接读取逐脚原因，不需要猜测。

存档兼容：`gameplay_snapshot.cpp` 把 `RigSettings`/`FootState`/`FeatureState` 改为容错读写，旧存档缺少 `mode/release/roll/pivot*/pelvisShift/maxPelvisShift/soleRoll` 时按默认值恢复，接触模式在下一 tick 由相位重新推导；不因新增字段拒绝整份存档。

## 4. 验收数据（内置机器人，modelScale 1.84，平地，3 m/s 行走 240 tick）

> 2026-10-28 更新：下表是本轮初次实现时的数据。支撑末期 1.05 m/s 的滑动已在 [支撑脚滑动收敛（P1.1）](CONTACT-DRIVEN-MOTION-STANCE-BUDGET.md) 中降到 0.13 m/s，踝残差 0.0276 → 0.0039 m，门禁同步收紧。下表保留为历史基线。

| 指标 | 数值 | 门禁 |
| --- | --- | --- |
| 有支撑脚的 tick 比例 | 210 / 240 (87.5%) | ≥ 85% |
| 支撑交接次数 | 9 | ≥ 4 |
| 鞋底接触点滑动（最大） | 1.05 m/s | < 1.2 |
| 锁定脚踝残差（最大） | 0.014 m | < 0.04 |
| 鞋底俯仰角（最大） | 0.399 rad | ≤ soleRoll + 0.02 |
| 交接瞬间踝位移 | 0.013 m | < 0.2 |
| 水平骨盆补偿（最大） | 0 m（平地常态不介入） | ≤ 0.129 m |
| 穿透 | 0.019 m | < 0.04 |
| 脚尖滚动次数 | 4 | > 0 |

证据：`build/tmp-animation-after.json` 的 `milestones.walk*` 字段（由 `native_animation_tests --evidence` 生成，正式留档请用 `npm run animation` 的报告目录）。

平地上的水平补偿常态为 0 是设计意图：只有当某条腿接近满伸时才把骨盆拉向锚点，否则每个步幅都会让身体落后于碰撞体。

## 5. 测试

`tests/animation_features_tests.hpp` 新增：

- 内置机器人：静止双脚落锁、静止零滑动、站立不需要水平补偿。
- 行走：支撑比例、交接次数、接触点滑动、踝残差、鞋底俯仰限幅、交接跳点、骨盆补偿限幅、脚尖滚动、穿透、释放计数。
- 快照：新格式往返 60 tick 摘要一致；删除全部新字段的旧格式仍可恢复并继续 tick（不要求摘要一致）。
- 传送：两只脚都报告 `Teleport` 释放。
- 行走双摆动：允许交接窗口内的短时重叠（≤ 8 tick），但每个重叠 tick 必须有明确释放原因。

`tests/character-fit.test.mjs` 增加 `soleRoll`/`maxPelvisShift` 的范围校验与合法覆盖用例。

本轮运行：`node --test tests/character-fit.test.mjs`、`node --test tests/rig-detection.test.mjs`、原生 `native_animation_tests`（5,350 基础断言 + 266,490 特性检查）、原生 `native_scene_tests` 全部通过；`host/main.cpp` 与 `host/scene_loader.cpp` 通过 `-fsyntax-only`。未运行 `npm run animation`（GPU 截图/录制/回放）与完整 CI。

## 6. 未完成与限制

- **参考库（P3）未实现**：仍按片段重定时驱动，内置机器人 walk 的重定时约 3×。原先归因于 `locomotionSpeed` 估计偏差的 1 m/s 滑动，实测是求解器侧的可达性问题（滚动令踝目标超腿长、水平骨盆补偿从未生效），已在 P1.1 修复到 0.13 m/s；但 4 m/s 这类超出片段表达能力的速度仍会退化（0.69 m/s），那部分确实需要参考库。
- **脚跟模式只有落地短暂使用**：整脚支撑仍是默认，脚掌滚动只在有接触窗口的步态片段上生效；没有接触元数据的资产保持平脚。
- **水平骨盆补偿未在真实崎岖地形上测量**：平地上不介入，斜坡/台阶只做了穿透与锁定检查。
- **多接触（手扶、攀爬、四足）未开始**：`ContactConstraint` 仍是双腿专用结构，没有通用接触集合与优先级。
- **编辑器可视化**未实现，只有报告字段；时间轴接触事件编辑属 P5。
- 工程夹具 `walk`（mannequin）的足弧悬空 8–11 cm，落锁会立刻超伸，测试中允许交接窗口内的短时双脚离地（≤ 8 tick，且必须有释放原因），这是内容问题而非求解器行为。
