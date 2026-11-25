# 接触驱动动作系统 · 多接触层（阶段四：手臂链与滑移接触）

日期：2026-10-28
上一轮记录：[支撑脚滑动收敛（P1.1）](CONTACT-DRIVEN-MOTION-STANCE-BUDGET.md)
验收对照：[《动态支撑点与多接触约束》开发完成度验收](CONTACT-DRIVEN-MOTION-TXT-ACCEPTANCE.md)

本轮只做阶段四缺的那一层：把“两只脚 + 双腿两骨 IK”之外的接触变成引擎能力。求解器（骨盆投票、两骨 IK、表面局部锚点、释放原因）沿用上一轮，没有重写。

## 1. 手臂链识别与资产契约

此前 `RigDefinition` 只有 `legs`，手只能由调用方手工 `addContactChain()` 注册，资产无法表达“这个角色有手臂、可以扶墙”。现在：

- `engine/assets/humanoid-rig.mjs` 新增 `detectArmChains()`：按骨骼语义找 `upperarm → forearm → hand`，校验三级直接父子、零长度和肘部极向。直臂（T/A-pose 无弯曲）时极向退回身体后方，不退化成零向量。
  - 只缺手臂的骨架不报警（无手角色合法）；只缺其中一段才写入诊断 `Incomplete <side> arm chain`。
  - 在腿部判定**之前**执行，因此腿部被判废的骨架仍保留手部接触能力。
- `tools/offline/animation.mjs`：`rig.arms` 与 `legs` 共用同一个链校验器（`mapChain`）。手工 rig 没写 `arms` 时继承自动识别结果——作者不必记得列手臂。`rig.arms` 上限 2。
- `engine/assets/character-source.mjs`：手工 rig 的手臂极向与脚底高度同样参与坐标系转换。
- 原生 `animation.hpp` / `animation_asset.cpp`：`RigDefinition::arms`，解析为**可选**字段；缺失即空，旧资产照常加载。

## 2. 原生接触链注册

`Controller::prepareRig()` 在初始化时把 `rig.arms` 注册进 `contactChains_`，顺序固定为左=0、右=1，并记录到 `armChains_`：

```cpp
int Controller::armChain(unsigned side) const;   // -1 表示该资产没有这只手
```

注册发生在腿部早退之前，所以只有手臂的骨架也能扶。应用仍可继续 `addContactChain()` 追加别的链（尾巴、攀爬工具），索引从手臂之后开始。

## 3. 接触契约补全：允许滑移

方案要求“动态指定高优先级接触与**允许滑动**的接触”。优先级上一轮已有；本轮补上滑移语义：

| 字段 | 含义 |
| --- | --- |
| `ContactConstraint::slip` | 该接触允许沿表面滑移 |
| `setContact(..., bool allowSlip = false)` | 发起/刷新接触时指定；改变该标志会重新绑定锚点 |
| `FeatureMetrics::holdSlip` | 钉住型接触的滑动速度（m/s），滑移接触不计入 |

实现上，允许滑移的接触是**没有位置约束的接触**：不做 IK、不参与骨盆投票、不做可达性释放，`pose` 每 tick 跟随当前骨端，`error` 恒为 0。这样“手掌贴墙滑过去”不会被记成跟踪误差，也不会把身体拽向墙面。方案 §9 要求“主动滑移与意外滑动分开统计”，脚用 `pivotSlip` 拆分，手用这个标志拆分。

存档：`gameplay_snapshot.cpp` 的通用接触序列化补上 `slip`；旧存档缺该字段时恢复为钉住（即录制时的行为）。

## 4. 测试

- `tests/rig-detection.test.mjs`：自动 rig 必须产出两条手臂链，root/tip 分别落在 `left_arm/left_hand`、`right_arm/right_hand`，父子关系成立，极向离开手臂轴线且指向身体后方。5/5 通过。
- `tests/animation_features_tests.hpp`：新增“允许滑移的接触”用例——身体以 1.2 m/s 走开 120 tick，接触必须持续有效（>100 tick）、`holdSlip` 为 0、`contactError` < 0.05 m。
- `native_animation_tests`：**通过**，5,350 条基础断言 + 266,788 项 M1–M4 检查（上一轮 266,491，增量即本轮接触用例）。
- 未运行 `npm run animation`（GPU 截图/录制/回放）、完整 CI 与全量测试。

## 5. 明确未做

- **Game 层门面未加**：`Game` 仍只暴露 `animationController()` 的只读视图，场景脚本还不能直接 `setContact`。需要时补 `setCharacterContact/releaseCharacterContact` 与报告透出。
- **四足**：`features_.feet` 仍是 2 个元素。四条腿的角色只能把前肢当接触链用，没有四足步态机。
- **攀爬/手扶行为**：只有数据与求解层，没有行为状态机（抓取判定、沿墙移动、上下攀爬）。
- **内置机器人资产未补 `arms`**：`assets/builtin/robot/character.json` 由 `npm run builtin:prepare` 重新导入生成，本轮未重跑（会连带重烹纹理并改动场景引用的哈希）。重跑一次即自动获得 `arms`；在此之前内置机器人仍靠手工注册链。
- 编辑器可视化（P5）仍未做，只有报告字段。
