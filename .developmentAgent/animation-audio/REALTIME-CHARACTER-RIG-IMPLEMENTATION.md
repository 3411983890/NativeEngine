# 实时角色 Rig 首版实现（阶段 A–D）

状态：已实现运行时步行与原地转向的纵向链路；编辑器视口调参、移动平台专项与性能/回放门禁未包含。  
日期：2026-10-27  
范围确认（本次明确选择）：地面适配为平地与盒形碰撞体支持的台阶/斜坡；Rig 配置为资产默认值加场景实例覆盖；交付重点为运行时接触/转向加自动 Rig，编辑器只提供数据接口。

方案与验收边界见 [REALTIME-CHARACTER-RIG-DEVELOPMENT.md](REALTIME-CHARACTER-RIG-DEVELOPMENT.md)；既有 M1–M4 基线见 [ANIMATION-M1-M4.md](ANIMATION-M1-M4.md)。

## 离线资产（阶段 A/B）

- 新增 `engine/assets/bone-semantics.mjs`：骨骼命名语义（别名、左右侧识别）从中提取，重定向与 Rig 识别共用；`animation-retarget.mjs` 改为引用该模块，行为不变。
- 新增 `engine/assets/humanoid-rig.mjs`：
  - `detectHumanoidRig`：按命名（含 Mixamo 前缀与后缀回退）、层级和静息比例诊断骨盆、头/颈、脊柱、双腿链、可选脚趾；由髋–膝–踝平面推导膝盖 pole，由脚部权重顶点的最低点推导脚底高度，由脚–趾距离推导脚长；不猜测同名歧义、零长度骨或夹有中间骨的链，而是写入 `diagnostics.missing/warnings/ambiguous` 并关闭对应功能。
  - `inferFootContacts`：按片段实际脚部弧线推断接触区间与置信度；无清晰交替（双脚同相触地或覆盖过短）时保持低于运行时信任阈值，不默认半周期。
- `tools/offline/animation.mjs`：显式 `extras.nativegpu.rig` 仍是权威（`source:"authored"`），缺失时写入自动诊断结果；链字段新增 `toe`/`footLength`；设置块 `settings` 含转向阈值、步高、步时、冷却、最大扭转、斜坡余弦、摆动预测、脚部修正上限、骨盆上下限与开关；片段新增 `contactsSource`/`contactConfidence`。导入告警仍然只在有效时写入，不改源 GLB。
- `npm run builtin:prepare` 已重新生成内置机器人：自动 Rig confidence 0.95，`walk.001` 接触置信度 0.83、`run.001` 0.79（可用），`jump.001` 0.45（运行时忽略），合成的 `idle`/`fall` 声明自身接触。场景与清单哈希同步更新。

## 原生契约（阶段 A）

- `animation.hpp`：`RigChain` 增加 `toe`/`footLength`；新增 `RigSettings` 与 `RigDefinition.source/confidence/warnings/spine`；`Clip` 增加 `contactSource/contactConfidence/contactKnown()`；新增 `FootPhase`/`RigFallback` 与名称函数；`Intent.modelScale` 把模型空间尺寸换算到世界；`ControllerConfig` 可携带场景覆盖后的生效设置（`rigOverride`）。
- `animation_asset.cpp`：`nativegpu.animation/2` 的 `rig`/`contacts` 扩展字段全部可选解析；`/1` 与旧 `/2` 资产行为不变。旧资产没有接触元数据时不再拿半周期当默认值，而是按未知处理。
- 快照与摘要：脚状态、转向残差/计数、摆动计时、生效 Rig 设置全部进入 `digest()` 与存档快照，续跑确定。

## 运行时（阶段 C/D）

固定 tick 内 `Correct` 阶段的顺序：地面取样与接触期望 → 转向残差与换步判定 → 单脚摆动/锁脚 → 骨盆补偿 → 双腿 IK 与脚掌朝向。

- 脚步状态机：`Support/Release/Landing/Swing`；支撑脚锁定在 `surfaceToWorld` 局部锚点，表面身份变化或传送时重锁；摆动期解除锁定并按实际速度预测落点（受 `maxFootCorrection` 限幅）；离地、无地面、超过坡度时释放并记录回退原因。
- 转向换步：用支撑锚点与「静息站姿 × 当前朝向」的角度残差驱动；超过 `turnThreshold` 且另一脚处于支撑时，单脚沿平滑抬脚弧线换位，落地后按剩余残差与冷却决定是否换另一只脚；普通情况不会双脚同时摆动；连续快转时支撑残差超过 `maxTwist` 只报告 `unreachable`，不强行拉骨。
- 可达性：IK 目标按腿长截断，超出即计入 `ikClamped`，不拉伸骨骼。
- 脚掌：保留原动画脚部旋转后对齐地面法线；存在脚趾骨时按静息前向做绕法线修正（限幅 0.5 rad）。
- 尺寸：`Intent.modelScale` 参与角色矩阵、地面探针与脚底偏移；场景缩放的导入角色也能落在正确高度。该字段由 `Game` 从角色引用的 `scale.y` 提供。
- 诊断接口：`featureState()`（相位、锚点、目标、法线、接触、转向残差、换步脚、回退原因）与 `featureMetrics()`（锁脚数、滑动、穿透、IK 误差、地面查询数、摆动次数、换步次数、IK 截断数）；`rigSettings()` 暴露生效配置。

## 场景覆盖与报告（阶段 E 数据部分）

- 场景 `character.rig` 可覆盖 `enabled`、`footLock`、`turnInPlace`、`turnThreshold`、`turnStepHeight`、`turnStepDuration`、`turnCooldown`、`maxTwist`、`maxSlopeCosine`、`swingPrediction`、`maxFootCorrection`、`pelvisLower`、`pelvisUpper`、`maxStepHeight`、`height`。宿主解析、JS 校验与 `assets/scene.schema.json` 同步；`enabled:false` 关闭程序化修正并回退基础动画。
- 宿主报告新增：`footLock`/`turnInPlace`、`rigSource`/`rigConfidence`/`rigWarnings`、`groundQueries`、`swingCount`、`turnSteps`/`turnStepsTotal`、`ikClamped`、`turnResidualRad`、`steppingFoot`、`fallback`，以及逐脚 `phase/locked/contact/surface/weight/turnStep/cooldown/anchor/target/normal` 的 `animation.feet` 数组。
- 新增验收场景 `.screen/character-rig.scene.json`：平地、四级盒形台阶、转向平台与阻挡墙；玩家引用内置机器人并在场景内覆盖 `turnCooldown` 与 `maxSlopeCosine`。

## 测试与验证

- `tests/rig-detection.test.mjs`（已加入 `npm run test:assets` 与 `.incrementalTesting/commands.json`）：普通骨架自动诊断、内置机器人可用步态、显式 Rig 优先、同名/中间骨/缺骨保守回退、接触推断不臆造步态、重复导入确定性。
- `tests/character-fit.test.mjs`：`character.rig` 附着选项与 schema 校验（越界、错误类型、未知键被拒）。
- `tests/animation_features_tests.hpp`：新增平地支撑/摆动交替、单脚换步、保守未知接触、模型缩放、场景覆盖等定向检查；原有 M1–M4 断言保持通过。
- `tests/native_scene.cpp`：新增场景内 Rig 覆盖加载与越界/未知键拒绝。
- 本次运行：`node --test tests/rig-detection.test.mjs tests/character-fit.test.mjs tests/animation.test.mjs`、`node --test tests/animation-retarget.test.mjs`、原生 `native_animation_tests`（5,350 基础断言 + 266,877 特性检查）与 `native_scene_tests` 全部通过；`host/main.cpp` 通过语法检查。未运行 `npm run animation`（GPU 截图/录制/回放）与完整 CI。

## 待机恢复与脚底对齐（2026-10-27 增量）

本轮修复了两个运行时问题：静止后姿态冻结，以及导入角色的脚掌没有落在地面上。

- **待机不再冻结**：`Controller::select` 在状态进入 `Idle` 时关闭 motion matching。此前零速度下匹配器会持续把播放时钟拉回最近的触地帧，角色停在走路姿态；现在 `Idle` 始终使用 idle 片段，并用 `0.10 / 0.18 m/s` 的进入 / 退出滞回避免状态抖动。
- **静态 idle 的程序化待机层**：`Clip::staticPose()` 在加载时识别所有帧完全相同的 idle 片段。`Controller::applyIdleMotion` 在 `Correct` 阶段、脚部求解之前叠加骨盆左右重心转移、髋部偏航、胸椎呼吸和头部慢速扫视；权重按停止后 0.35 s 平滑进入。相位只依赖已参与摘要的 `tick_` 与 `stateTicks_`，不新增可失步状态。有真实 idle 动作的资产不会叠加该层。
- **脚底朝向以静息姿态为基准**：旧实现把脚骨的局部 +Y 当作“上”去对齐地面法线。Mixamo 等骨架的脚骨沿脚趾方向，局部 +Y 并不是脚底法线，结果脚掌被翻转。现在 `prepareRig` 缓存脚与脚趾的静息全局旋转，`correct` 用 `groundRotation × restFootRotation` 设置脚掌全局朝向，并把脚趾加入修正混合掩码，避免脚趾穿透。
- **重置后的脚锁锚点重算**：`Controller::reset` 在 gameplay 提供 `modelScale` 之前求值一次图，旧逻辑会以 1 倍尺寸锁脚；之后同一表面上不再重锚，缩放角色会整体下沉（内置机器人约 6 cm）。现在首帧若锁定目标与实测鞋底高度的法线误差超过 `0.02 × modelScale` 就重锚；后续 tick 保持锚点，避免台阶边缘因单点射线换面而抖动。
- 首帧脚重不再从绑定姿态淡入：无姿态历史时直接把脚权重置 1，避免出生 / 重置第一帧脚掌短暂穿地。

测试增量：`tests/animation_features_tests.hpp` 新增静态 idle 必须产生可见待机运动、待机脚掌必须保持静息鞋底平面与鞋底高度、“先装地面查询再初始化”的缩放角色必须重锚，以及内置 Mixamo 风格机器人（脚骨局部 +Y 沿脚趾）必须保持静息鞋底平面四项断言。原生动画测试现为 5,350 基础断言 + 266,894 特性检查。

## 步态延迟与转身穿模修复（2026-10-27 第三轮）

用户报告三个现象：动作明显延迟、转身时腿部穿模且像“没有转身”、奔跑时腿偶尔卡住不跟随。三者是同一层的两个缺陷。

### 1. 支撑脚钉在不该钉的位置（延迟 / 卡腿）

旧判定只看片段接触相位窗口，加一个很松的高度门槛（`sole + 0.06 × scale`，内置机器人约 11 cm）。片段相位与真实足弧并不一致：脚已经抬起、甚至已经向前甩动，仍然被判定为支撑并被锚定。随后 IK 把整条腿拖到世界锚点上，实测：

- 支撑脚踝被拉到离地 19 cm，脚踝与锚点水平偏差最大 0.97 m；
- 躯干带动骨盆补偿一起下沉（髋部 1.02 m → 0.83 m），角色像在“拖着一条腿滑动”；
- 每 200 tick 出现 14 次 IK 截断（`fallback = unreachable`）。

现在的接触判定改成几何事实，相位窗口只作为意图：

- **落锁**要求脚踝确实在鞋底平面附近（容差 `max(0.02, 0.3 × sole)`）**且**该脚踝在世界中几乎静止。速度阈值按重定时的误差放宽（`1 + |实速 − 播放率 × 片段步速|`），因此没有 `locomotionSpeed` 元数据的资产仍按片段窗口保守工作，而不会被误判为“永不落地”。
- **保持**用滞回：高度放宽到 2.5 倍容差、锚点漂移不超过 `maxFootCorrection`（上限 0.30）。速度不参与保持判定，否则离地瞬间（支撑脚在 toe-off 时世界速度可达 2.4 m/s）会每个循环抖一次 IK 权重。
- **释放**：脚踝抬起、锚点漂移超限，或 IK 需要的距离超过腿长（`legReach + 4 cm`）。最后一条关键——旧实现只是把目标截断再强行拉直，现在直接放弃锚点把腿交还片段，下一段接触再重锚。
- `ikClamped` 只统计真实超伸；直腿站立时“达到 99.5% 腿长”不再算截断（此前每个 tick 都计数）。

### 2. 转身：身体瞬间旋转 + 支撑脚不动 = 腿部交叉

旧实现里角色朝向直接等于相机 yaw，一帧可以转 180°；两只脚却仍锚在原处，腿部扭转实测到 2.73 rad（156°）。换步状态机又要求“另一只脚处于 Support”，而快速转身时另一只脚往往正在摆动，于是换步从不触发，只剩 `unreachable` 报告。

- `Game` 新增 `bodyYaw_`：相机仍然是操控输入，身体按 ≤12 rad/s（移动）/ 8 rad/s（站立）追随；第一人称直接对齐。动画 `Intent.heading`、渲染朝向、摘要与存档都改用 `bodyYaw_`，移动方向仍按相机，因此转向不再把骨盆从脚下拽走。
- rig 侧三道保险：支撑残差超过 `turnThreshold` 时让**脚掌原地外旋**（目标绕身体竖轴偏航，扭矩上限 = `maxTwist − turnThreshold`，可见腿部扭转不超过 `turnThreshold`）；超过 `maxTwist` 直接释放锚点；身体转速快过换步能力时让正在摆动的那只脚**提前落地**（`hurry`），避免“一只脚在空中、另一只脚已经扭到底”。换步进行中不释放支撑脚，保持“始终有一只脚支撑”的不变量。

### 3. 数据（内置机器人，平地，模型缩放 1.84）

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 支撑脚踝与锚点水平偏差（最大） | 0.97 m | 0.15 m |
| 支撑脚踝高于鞋底平面（最大） | 0.19 m | 0.03 m |
| 每 200 tick IK 截断次数 | 14 | 0 |
| 转身残差（最大） | 2.73 rad | 1.15 rad（可见扭转被限制在 0.35 rad） |
| 转身换步 | 0 次 | 连续 |

证据：临时诊断程序（已在 `build/` 内清理）逐 tick 记录 `phase/locked/contact/weight/above/drift` 对比修复前后；宿主回放 `.screen/basic.scene.json` + 脚本化输入（走 80 帧 → 跑 60 帧 → 原地快转 70 帧），`reports/animation/fix-walk|fix-run|fix-turn|fix-idle.{png,json}` 为四个时点的画面与报告，末帧 `turnResidualRad = 0`、`lockedFeet = 2`、`turnStepsTotal = 6`、无 fallback；严格回放 240 帧 0 偏差。

### 4. 契约变更

- `FootState.animated` 记录“片段画出的脚踝世界位置”，参与摘要与存档；旧存档缺该键会按 `NG-E-SNAPSHOT-ANIMATION` 拒绝（不静默猜测）。
- `foot.contact` 改为片段意图，`foot.locked` 才是求解器真正锚定的结果。两者分离后，“接触但钉不上”的资产（如工程夹具 `walk`，足弧整体悬空 8–11 cm）可以如实报告而不再拖腿。
- 相应更新 `tests/animation_features_tests.hpp`：行走段改为统计接触边沿与“锚定期间的拖拽上限 ≤ 0.32 m”，重定时断言固定在片段驱动路径（匹配器自带帧时钟，按设计 1× 播放）。

## 转身"抬脚才动"与台阶过渡（2026-10-27 第四轮）

反馈：转身仍是"整个模型旋转带脚滑"，没有"抬脚才动"；上楼梯卡腿。

### 1. 转身改为抬脚驱动

上一轮只是限制了身体的旋转角速度，身体仍在**连续**跟随相机，支撑脚只能靠原地外旋补偿——视觉上就是模型在脚上打转。现在：

- `Intent` 增加 `hasTurnTarget`/`turnTarget`（相机朝向）。`Game` 每 tick 传 `turnTarget = yaw_`，`heading` 仍是身体实际朝向。
- rig 的换步判定改用**需求残差**（目标朝向 − 落锁时朝向）而不是身体已转过的角度；换步落点也按目标朝向预置。这样即使身体还没动，只要相机差超过 `turnThreshold` 就先起脚。
- `Game` 的身体旋转被脚的状态门控：`Controller::feetAirborne()`（`steppingFoot >= 0` 或有脚处于 Swing/Release）为真时才转，站立时速率 4 rad/s、移动时 10 rad/s；双脚落地且玩家已松手、残差又小于一步（0.30 rad）时用 1.2 rad/s 慢速补正，避免朝向永久偏离相机。

实测（90° 单帧甩视角，`build/` 内临时探针，已清理）：tick 60 相机跳 +90°，身体保持 0°；tick 60 起脚；tick 61-79 身体以 4 rad/s 转到 38°（全程一脚在空）；tick 80 落地即停止；tick 81 换另一只脚继续，最终 90°/残差 0.00/3 次换步/`footSlip` 0.0000。

### 2. 上楼梯：根因不在 IK，而在角色控制器

`tryStepUp` 在身体还**没走到**台阶上方时就按前探射线把人抬到台面高度。此后十几帧里脚下没有任何支撑，`grounded_` 反复变 false，`Controller::correct` 每帧走 `NotGrounded` 分支（`resetFeet` + 直接返回）——两只脚全部被重置为摆动，骨盆补偿失效。实测登梯过程中 `fallback=not-grounded`、`feet=swing/swing` 连续出现。

三处修正：

- **台阶过渡算作支撑**：`tryStepUp` 成功抬升后给 `stepGrace_` 0.25 s；这段时间不施加重力、`grounded_` 保持 true，覆盖"抬上去 → 前移踩到台面"的滑入阶段。新状态进摘要与存档。
- **接触高度改用模型空间**：原先用脚踝的**世界**高度判断"是否在地面附近"，身体一抬高整副骨架都跟着抬高，支撑脚就被判成离地。改为 `(模型脚踝高度 − 静息鞋底高度) × 缩放`——接触是姿态属性，不是世界位置属性。这也让斜坡上的判定不再受地面高度差影响。
- **接触窗口不铺满时由几何决定**：内置机器人 walk 的两个接触区间只覆盖 64% 周期，剩下的 36% 两只脚都没有窗口，脚锁整段失效。现在若当前相位不在任何一只脚的窗口内，就交给几何判定（真正的腾空仍会被"脚踝是否履地/静止"否掉）。
- **踏步穿台面**：抬脚时片段只抬约 10 cm，而台阶高 25 cm，脚踝会被画到台面**之下**。判定为接触并给 0.85 权重后由 IK 把鞋底抬到台面上，同时保留 `pelvisLower` 的骨盆下沉来吸收身体升高。实测穿透从 0.36 m 降到 ≤0.08 m（12 个采样点中 10 个为 0）。

数据（`.screen/character-rig.scene.json`，四级 0.25 m 台阶）：修复前登梯 12 个采样点全部 `locked≤1` 且周期性 `not-grounded`，穿透峰值 0.36 m；修复后每点都有支撑脚、`ikClamped` 全 0、穿透峰值 0.08 m、登到顶面 y=1.000。

证据：`reports/animation/rig-stairA|B.png`（登梯两个时点）。

### 3. 场景与测试

- `samples/启动动作验收.cmd`：双击用验收场景（地面 + 四级台阶 + 转向平台 + 阻挡墙）启动，方便手动验证。
- 原生动画测试保持通过：5,350 基础断言 + 266,663 特性检查；rig 场景严格回放 330 帧 0 偏差。

## 未完成与限制

- 编辑器视口确认、脚底/pole 可视调参、接触时间轴编辑界面未实现；当前只提供上述诊断数据。
- 移动平台专项验收（平台相对滑动阈值、平台消失释放）未作为首版门禁；锚点已保存在表面局部空间并记录相对滑动，但没有独立测试。
- 脊柱扭转只表现为腿部落差报告；上身没有独立的扭转分配（身体整体按 `bodyYaw_` 滞后追随相机）。快速连续旋转时支撑残差可能短时超过 `maxTwist` 并报告 `unreachable`，此时可见扭转仍被脚掌外旋限制在 `turnThreshold` 内，属诊断而非失败。
- 中间扭转骨（大腿/小腿/脚之间的辅助骨）仍不支持，只给出警告；需要专门的链折叠或新求解器后再声明支持。
- 工程夹具 `walk` 的双脚足弧几乎同相，保守推断置信度 0.43（运行时忽略）；内置机器人为首个可用步态样例。阈值应在真实角色与场景上重新测量后再固化。
- 内置机器人 walk / run 片段推断步速约 0.63 / 1.76（模型单位），控制器按 `实速 ÷ (步速 × 模型缩放)` 重定时（本场景约 3.0× / 1.85×）。推断步速与真实支撑脚速度仍有约 10–15% 偏差，表现为支撑末期几厘米的脚滑，属于导入侧 `locomotionSpeed` 估计精度问题。
- 片段驱动的路径才重定时；`motionMatching` 路径按设计保持 1×（匹配器持有自己的帧时钟），因此可匹配资产在速度差大时仍会有滑动。
- NPC 仍走原固定片段路径，未接入程序化修正；网格地形与任意碰撞面不在首版范围。