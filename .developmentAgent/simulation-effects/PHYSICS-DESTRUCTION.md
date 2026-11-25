# 物理、破坏与结构坍塌（0.11.0）

本文记录 NativeGPU 新增的真实感刚体物理与物体破坏系统：设计取舍、求解器细节、场景创作接口、可复现证据，以及它与相关论文的关系。实现目标是**可运行、可测量、可回放**，而不是把“能力查询”写成“效果已完成”。

- 原生实现：`engine/native/physics.{hpp,cpp}`、`engine/native/game.cpp`
- 场景字段：`physics`、`destructible`（JSON Schema、C++ loader、Node validator 三处同步）
- 演示场景：`.screen/physics-lab.scene.json`
- 证据门禁：`.incrementalTesting/simulation/physics.ps1` → `reports/physics-evidence.json`
- 交互：`F` 投掷重弹，`G` 在面前引爆径向冲击
- 自动演示：`NativeGPU.exe --scene .screen/physics-lab.scene.json --physics-demo`
- 测试：`native_physics_tests.exe`、`native_destruction_tests.exe`

当前证据（本机 RTX 5060 Laptop，1280×720，0.11.0）：

| 指标 | 数值 |
| --- | --- |
| 固定步长 | 120 Hz，自适应 1–8 子步 |
| 场景刚体 / 活动刚体 | 166 / 166（含碎片） |
| 碎片生成 | 142 |
| 断裂对象 | 11 / 11 |
| 结构坍塌 | 3 |
| 峰值法向冲量 | 80.47 N·s（游戏单位） |
| 物理平均步耗时 | 约 0.6–1.0 ms（BVH 宽相；O(n²) 记录值 0.90 ms） |
| 物理最大步耗时 | 约 2.7–4.2 ms（O(n²) 记录值 3.6 ms） |
| 宽相候选对/子步 | 435（O(n²) 为 11,935） |
| 确定性重复 | 最终摘要逐位一致 |
| 前后截图差异 | 平均绝对误差 2.57，变化像素 9.64% |

> 这里的时间是 CPU 物理步，不含渲染。数值来自 `reports/physics-evidence.json`，每次重跑会重新生成。

---

## 1. 范围与边界

### 已实现

- 定向盒（OBB）刚体：质量、惯性张量、线速度、角速度、四元数姿态。
- 静态/动态刚体；动态体受重力、阻尼、摩擦、恢复系数影响。
- 15 轴 SAT 窄相 + 裁剪面接触流形 + 边-边接触。
- 顺序冲量求解器：12 次速度迭代、2 次位置修正、Baumgarte 偏置、库仑摩擦、恢复阈值。
- 休眠/唤醒，减少堆叠抖动与空转成本。
- 基于最大位移的自适应子步，防止高速投掷物穿墙。
- 冲量驱动的伤害累积与断裂；盒式预碎裂；碎片继承父体速度与角速度。
- 键合破坏（`destructible.mode: "bonded"`）：实体被切成 cell 网格，cell 之间是带面积的 bond；冲击能量先让单元损伤，单元粉碎则断开它的全部 bond 形成局部洞，其余结构继续站立。并查集把剩余 cell 分成岛，失去支撑的岛转为动态刚体。渲染零改动（cell 仍是盒体）。
- XPBD 关节（C++ API）：`Point`（悬挂）、`Hinge`（门，两点约束 + 角度限位）、`Rope`（距离链），支持 compliance、`breakForce` 断裂阈值与 `collideConnected` 接触过滤。关节作为**附加约束层**在位置阶段求解，不替换顺序冲量求解器。
- 离线凸 Voronoi 烘焙（`tools/offline/voronoi.mjs`）：确定性种子 → 半空间裁剪 → 凸单元（顶点/面/体积/质心/OBB 代理）+ 邻接 bond，schema `nativegpu.voronoi/1`。
- 结构支撑图：底层失去支撑后，上层对象逐级坍塌为独立刚体碎片。
- 径向爆炸：按距离衰减的速度冲量 + 对可破坏物的伤害。
- 弹道投掷：按目标点计算含重力的初速度。
- 角色碰撞：角色 AABB 扫掠直接查询静态/动态刚体的当前世界 AABB，可站上箱子和碎片、推动轻道具；物理步后另有穿透修正，避免角色被移动刚体“穿模”。
- 全部状态进入确定性摘要，`--record/--replay` 支持 fire/blast 边沿（replay v3）。
- 报告、前后截图、性能预算、重复摘要校验。

### 明确未实现（避免“看起来像”）

- 只有盒体碰撞；没有球、胶囊、凸包、三角网、复合形状。
- 碎片是**递归二分的长方体碎块**，不是真正的凸 Voronoi 单元；没有裂纹 UV、断面材质或体积网格。
- 伤害是冲量阈值模型，不是连续介质应力/应变场。
- 爆炸伤害没有遮挡测试，墙后对象也会受伤。
- 角色不是动态胶囊：使用 AABB 而非胶囊/OBB，旋转碎片的 AABB 偏保守；角色能被箱子阻挡，但不会被箱子真正“推着走”，也没有角色质量/惯性。
- 关节只有 Point/Hinge/Rope 三种，没有马达、弹簧驱动、关节摩擦；关节尚未进场景格式，内容侧还不能直接摆门。
- 键合破坏的 cell 是均匀盒格，不是 Voronoi 单元；离线 Voronoi 只做到烘焙与资产契约，凸网格渲染与凸-凸窄相未做，所以运行时仍用 OBB 代理与盒体渲染。
- 没有布料、软体、流体-刚体双向耦合。
- 单线程 CPU；没有 GPU 宽相、并行约束或确定性并行归约。
- 休眠判定是逐体阈值，不是严格的岛（island）级休眠。

---

## 2. 架构

```text
scene JSON
  ├─ physics { mode, mass, friction, restitution }
  └─ destructible { enabled, strength, fragments, debrisLifetime, structural, seed }
          │
          ▼
host/scene_loader.cpp  ──►  engine::Entity
          │
          ▼
Game::initializePhysics()
  ├─ 每个 collider 实体 → 静态或动态 Body
  ├─ retain entity → body index（不随碎片追加而失效）
  └─ 可破坏实体登记 damage / fractured 状态
          │
          ▼
每 1/120 s：
  Game::step
    ├─ 玩家移动/跳跃/交互（原有扫掠求解）
    ├─ fire / blast 边沿
    ├─ physics_.step(dt)
    │    ├─ 自适应子步
    │    ├─ 积分速度 / 重力 / 阻尼
    │    ├─ 宽相 AABB + SAT
    │    ├─ 接触流形 + 顺序冲量
    │    ├─ 位置修正 + 休眠
    │    └─ 生成 Impact 事件
    ├─ processImpacts(): 冲量 → 伤害 → 断裂
    ├─ collapseUnsupported(): 支撑图 → 级联坍塌
    └─ digest(): 姿态/速度/损伤/碎片全部入哈希
          │
          ▼
renderPoses()
  ├─ 未破坏实体：原场景姿态（受损伤变暗）
  ├─ 动态实体：由 Body 四元数转 yaw/pitch/roll
  └─ 碎片/投掷物：独立 Body 位姿与材质
```

### 为什么不用 XPBD 作为主求解器？

XPBD 在关节、软体和高度约束化场景中非常稳健，但本引擎当前只有盒体接触，且需要与既有 120 Hz 手感、扫掠角色和确定性回放共存。顺序冲量求解器可以直接消费接触流形并给出真实冲量（用于伤害触发），实现更小、可审计。XPBD 的“小步长、少迭代”思想被吸收到自适应子步里；未来做关节/可断裂 bond 时可以增加 XPBD 路径，而不替换刚体接触层。

---

## 3. 刚体求解器

### 3.1 状态与积分

每个动态体保存：

```text
position x, orientation q, linear velocity v, angular velocity ω
mass m, inverse inertia in local space I⁻¹_local
friction μ, restitution e, damping
```

盒体局部惯性：

```text
Ixx = m/12 · (sy² + sz²)
Iyy = m/12 · (sx² + sz²)
Izz = m/12 · (sx² + sy²)
```

世界空间逆惯性每步刷新：

```text
I⁻¹_world = R · I⁻¹_local · Rᵀ
```

速度积分包含重力与指数近似阻尼；位置积分后对四元数做一阶更新并归一化：

```text
q ← normalize(q + 0.5 · ω_quat · q · h)
```

### 3.2 宽相与窄相

- 宽相：动态 AABB 树（`engine/native/broadphase.hpp`）。叶子存紧包围盒外扩 0.02 的胖 AABB，SAH 下行插入，插入数超过 `max(64, 4n)` 或树高 > 48 时按质心中位数重建。活动体数量低于 64 时自动退回 O(n²) 参考路径，`Settings::broadphase` 也可强制指定。
- 宽相只做候选过滤，不做几何判定：候选对按 `(bodyA, bodyB)` 排序后进窄相，接触生成顺序与 O(n²) 路径完全一致，因此摘要与回放逐位不变（`native_broadphase_tests` 断言 900 tick 位置/速度按位相等）。
- 窄相：OBB 的 15 条候选分离轴（A 的 3 面、B 的 3 面、9 条叉积边轴）。
- 面接触：选择穿透最小的参考面，把入射面四边形对参考面的 4 个侧平面裁剪，最多保留 4 个接触点。
- 边接触：取两条支撑点的中点作为单点接触。
- 接触法线统一从 A 指向 B，后续冲量符号不会因交换顺序而翻转。

### 3.3 顺序冲量

对每个接触点，有效质量：

```text
k = 1/mA + 1/mB
  + n · ( (I⁻¹A (rA × n)) × rA )
  + n · ( (I⁻¹B (rB × n)) × rB )
```

法向冲量：

```text
λ = (v_target + bias − v_n) / k
bias = min(β/h · max(penetration − slop, 0), maxBiasSpeed)
v_target = −e · v_approach   (仅当接近速度超过阈值)
```

- 累积冲量钳制到 `λ ≥ 0`。
- 摩擦在双切向基上求解，合成切向冲量钳制到 `μ · λ_n`。
- 恢复系数只在接近速度 > 1.2 m/s 时生效，避免静止堆叠抖动。
- 位置修正按“每对刚体只取最深接触点”执行，避免 4 点流形把修正放大 4 倍。
- 默认 12 次速度迭代 / 2 次位置修正。

### 3.4 自适应子步与防穿透

每个物理步先扫描动态体的最大线速度与角速度，以“单子步最大位移 0.12 m”为上限：

```text
substeps = clamp(ceil(maxTravel / 0.12), 1, 8)
```

这条规则比固定子步数更适合爆炸碎片和高速投掷：慢速堆叠只跑 1 个子步，高速弹体自动细分。测试中 40 m/s 的弹体不会穿过 0.2 m 厚的墙。

### 3.5 关节（XPBD 附加层）

关节不在速度求解器里，而是在位置阶段：

```text
integrateVelocities → contacts(velocity) → correctPositions
→ captureJointBodies → integratePositions → solveJoints → sleep
```

- 每个点约束按 XPBD 投影：`Δλ = (−C − α̃λ)/(w + α̃)`，`w` 含两端线质量与角项。
- 投影后按 `v = (x_corrected − x_previous)/h` 回写速度，等价于"先预测再修正"的 XPBD 更新，不会把修正量重复计入下一帧。
- 门 = 沿铰链轴分开的两个点约束；两个偏移点各自随所属刚体旋转，绕轴转时保持一致，绕其它轴转时产生约束。角度限位先解、点约束后解，避免锚点漂移。
- 绳 = 单边距离约束（只在拉长时生效），每段一个动态刚体，复用刚体渲染。
- `collideConnected = false` 时窄相跳过该对：门与门框必然几何重叠，不跳过会把门推离铰链。
- 断裂阈值比较 `|λ|/h²`（λ 量纲是 kg·m），单位是力。

### 3.6 休眠

- 线速度 < 0.055、角速度 < 0.09 且持续 0.65 s → 休眠并清零速度。
- 运动体与休眠体接触时唤醒休眠体。
- 爆炸默认唤醒范围内全部动态体。
- 休眠显著降低 142 块碎片稳定后的 CPU 成本。

---

## 4. 破坏与结构坍塌

### 4.1 冲量 → 伤害

设接触法向冲量为 `I_n`，对象强度为 `s`：

```text
threshold = max(3, 0.20 · s)
damage   += (I_n − threshold) / (1.8 · s)
damage   ≥ 1  → 断裂
```

- 投射物 5.5 kg × 36 m/s 产生的冲量约 80 N·s，足以击穿强度 34–45 的砌块。
- 小碎片落地冲量远低于阈值，不会“连锁秒碎”整片场景。
- 未断裂但受损的对象在渲染时颜色变暗（最多 30%），提供可读的损伤反馈。
- 爆炸改用距离衰减伤害 `1.15·falloff + 0.25`，近距离直接超过阈值。

### 4.2 盒式预碎裂

受当前渲染器“实例化定向盒”限制，碎片必须是盒体。做法是对父盒做**递归二分**：

1. 选当前体积最大的碎块；
2. 沿最长轴在 36%–64% 处切分（确定性随机）；
3. 重复到目标碎片数；
4. 质量按 `m / N` 分配，保持总质量；
5. 碎片继承父体线速度、角速度和接触点产生的径向踢出速度；
6. 材质、粗糙度、金属度、颜色、纹理槽继承父体；
7. 活跃碎片上限 220，超过后不再生成；每块有生命周期并在到期时移除。

这不是 Voronoi，但保留了实时破坏的三个关键性质：**质量守恒、体积分割、可独立碰撞**。真正的凸 Voronoi 需要新的网格/凸包渲染与碰撞路径，已列入路线图。

### 4.3 结构支撑图

每个 `structural: true` 的可破坏对象是图节点。一个节点“有支撑”当且仅当：

- 底面高度 ≤ 0.14 m（地面），或
- 存在另一个未破坏的支撑体，其顶面与本节点底面高差 ≤ 0.16 m，且 XZ 投影重叠 > 0.035 m。

求解是一个自底向上的不动点：

```text
supported = grounded nodes
repeat:
  for node not supported:
    if any supported support below → supported
until no change
```

断裂后重新计算；所有 `not supported` 的结构节点一起转为碎片，于是“打掉底层 → 上层整体失去支撑 → 逐块坍塌”成为物理结果，而不是脚本动画。当前演示中打断中央塔底层后，其余 4 层全部转为碎片；三座塔共触发 3 次结构坍塌。

### 4.4 键合破坏（bonded）

`destructible.mode: "bonded"` 时，实体在初始化阶段被切成 cell 网格（`fracture::chooseDivisions` 按体积预算选格数，薄轴保持 1 格）：

```text
impact → energy = 0.5 · J_n · v_approach · 0.02
       → 按 1/(1+d²) 分给半径内 cell
       → cell.damage += E_share / (toughness · faceArea)
       → damage ≥ 1：粉碎该 cell，断开它的全部 bond（洞）
       → 环带 bond 另计损伤：重复命中扩洞
       → 并查集求岛；最大岛保留，其它岛不着地则转动态
```

- 首次断键时把实体刚体换成每 cell 一个静态刚体，`renderPoses` 改为输出每 cell 位姿。
- 质量守恒：cell 质量 = 密度 × 体积，密度由 `physics.mass / 实体体积` 得出。
- 场景字段 `destructible.mode` / `destructible.cells` 与 `physics` 一样在三处校验。
- 演示场景：`.screen/bonded-wall.scene.json`。
- 证据复现（`--shot frame:x,y,z` 可重复，走与 F 键相同的射击代码）：

```
.\build\NativeGPU.exe --scene .screen\bonded-wall.scene.json --smoke --no-stress ^
  --frames 240 --camera 1.6,1.9,1.6,0.0,1.6,-3.0 --render-size 1280x720 ^
  --screenshot reports\bonded-wall\intact.png --screenshot-frame 120 ^
  --report reports\bonded-wall\intact.json

.\build\NativeGPU.exe --scene .screen\bonded-wall.scene.json --smoke --no-stress ^
  --frames 420 --camera 1.6,1.9,1.6,0.0,1.6,-3.0 --render-size 1280x720 ^
  --shot 60:-0.9,2.0,-3.0 --shot 170:0.9,1.2,-3.0 ^
  --screenshot reports\bonded-wall\holes.png --screenshot-frame 400 ^
  --report reports\bonded-wall\holes.json
```

  同一相机下两发偏移命中后：`bodies 9→57`、`bondedHoles 0→4`、`bondedBrokenBonds 0→14`，
  而 `fractures` 仍为 `0`、`intactDestructibles` 仍为 `2`——即“局部开洞、整体不倒”。
- 键合状态随存读档往返：`native_destruction` 的 `testBondedSnapshotRoundTrip` 在开洞后
  `snapshot`/`restore`，再同输入推进 240 tick，两个世界的快照逐帧相等。

### 4.5 与碎裂物体的交互

碎片是完整动态刚体，会：

- 砸到其他可破坏物并继续累积伤害；
- 被后续爆炸再次推开；
- 与地面、墙、动态箱子发生摩擦/恢复/堆叠；
- 进入 DXR TLAS 作为刚体实例投射阴影；
- 被计入确定性摘要与报告。

---

## 5. 场景创作接口

```json
{
  "kind": "prop",
  "collider": true,
  "physics": {
    "mode": "dynamic",
    "mass": 4.0,
    "friction": 0.66,
    "restitution": 0.12
  },
  "destructible": {
    "enabled": true,
    "strength": 40,
    "fragments": 12,
    "debrisLifetime": 20,
    "structural": true,
    "seed": 101
  }
}
```

约束：

| 字段 | 约束 |
| --- | --- |
| `physics.mode` | `static` / `dynamic`；ground 不能 dynamic |
| `physics.mass` | 0.05–1000 |
| `physics.friction` | 0–1.5 |
| `physics.restitution` | 0–0.95 |
| `destructible.enabled` | boolean |
| `destructible.strength` | 1–1,000,000 |
| `destructible.fragments` | 4–28 的整数 |
| `destructible.debrisLifetime` | 1–120 s |
| `destructible.structural` | boolean |
| `destructible.seed` | 0–2³²−1 |
| 通用 | 可破坏对象必须 `collider=true`；只允许 wall/prop |

同一套字段在三处校验：`assets/scene.schema.json`、`host/scene_loader.cpp`、`engine/scene/index.mjs`。事务层允许对 `physics` / `destructible` 做 `set`，并在提交前跑完整校验。

---

## 6. 确定性与回放

系统不依赖线程调度、系统时间或无序容器遍历：

- 刚体数组顺序稳定；追加碎片不会重排既有实体索引。
- 宽相/窄相/迭代顺序固定。
- 断裂随机数来自 `seed ^ entityIndex ^ tick`，不读全局熵。
- 物理状态（位置、四元数、线速度、角速度、休眠、损伤、断裂计数）进入 `Game::digest`。
- 同一命令两次运行最终摘要逐位一致；证据脚本显式比较。
- `--record` 格式升级到 replay v3，新增 fire/blast 两列；加载器仍接受 v2 的 10 列记录。

---

## 7. 复现

```powershell
# 只跑物理/破坏测试
npm run test:native

# 生成前后截图、报告与证据门禁
npm run physics

# 手动观察
.\build\NativeGPU.exe --scene .screen/physics-lab.scene.json --physics-demo
# 或交互：F 投掷，G 爆炸

# 报告与截图
reports/physics-evidence.json
reports/physics/intact.png
reports/physics/destroyed.png
```

`.incrementalTesting/simulation/physics.ps1` 的检查项：

1. 未破坏场景 11 个可破坏体、0 断裂；
2. 刚体世界构建（≥15 体、≥5 动态）；
3. 3 枚投射物；
4. 11 个对象全部断裂；
5. ≥2 次结构坍塌；
6. ≥60 块碎片且活跃碎片 ≥50；
7. 冲量确实绑定到伤害链；
8. 两次运行最终摘要一致；
9. 前后截图哈希不同且平均绝对误差 ≥1.0；
10. 平均物理步 <3 ms、最大 <25 ms；
11. 无错误码。

---

## 8. 论文对照与可借鉴点

以下不是“本实现已经复现论文”，而是逐项说明：借了什么、当前差在哪、下一步能怎么做。

| 工作 | 核心思想 | 本实现采用/未采用 | 可借鉴的下一步 |
| --- | --- | --- | --- |
| Müller et al., *Position Based Dynamics*, 2007. [DOI](https://doi.org/10.1007/978-3-540-72917-0_12) | 位置约束投影，稳定、易组合 | 不采用为主求解器；概念上影响支撑图与位置修正 | 用 PBD 约束实现可断裂 bond、绳索、布料 |
| Macklin et al., *Small Steps in Physics Simulation*, 2019. [DOI](https://doi.org/10.1145/3309486.3340247) | 多小步 + 少迭代优于少大步 | 采用“小步”思想，但用几何位移阈值自适应决定子步 | 固定 240/480 Hz 子步并与当前自适应策略做 A/B |
| Müller et al., *Detailed Rigid Body Simulation with XPBD*, 2020. [DOI](https://doi.org/10.1111/cgf.14105) | XPBD 刚体、关节、摩擦统一框架 | 未采用；当前无关节 | 用 XPBD 做可断裂连接、门铰链、绳索 |
| Guendelman et al., *Nonconvex Rigid Bodies with Stacking*, 2003. [DOI](https://doi.org/10.1145/1201775.882358) | 冲击传播处理堆叠与非凸体 | 未做全局 shock propagation；用顺序冲量 + 休眠 | 岛级冲击传播，提升高塔/堆叠稳定性 |
| Tonge et al., *Mass Splitting for Jitter-Free Parallel Rigid Body Simulation*, 2012. [DOI](https://doi.org/10.1145/2185520.2185601) | 质量分裂消除并行/大质量比抖动 | 未采用；单线程确定性 | 若做并行求解，先引入 mass splitting 保持稳定 |
| O'Brien & Hodgins, *Graphical Modeling and Animation of Brittle Fracture*, 1999. [DOI](https://doi.org/10.1145/311535.311548) | 基于应变能的脆性断裂 | 未采用；使用冲量阈值代理 | 用有限元/应力场驱动裂纹，替代标量 health |
| Parker & O'Brien, *Real-time Deformation and Fracture in a Game Environment*, 2009. [DOI](https://doi.org/10.1145/1599470.1599492) | 预碎裂 + 运行时替换，适合游戏 | **采用其预碎裂思路**，但碎片是盒体而非任意网格 | 离线凸 Voronoi 预碎裂 + 运行时切换 |
| Sellán et al., *Breaking Good: Fracture Modes for Realtime Destruction*, 2022. [DOI](https://doi.org/10.1145/3549540) | 可控 fracture modes、艺术导向的破碎 | 未采用；目前只有一种盒式切分 | 提供“切割/压碎/撕裂”模式与美术参数 |
| *Energized Rigid Body Fracture*, 2018. [DOI](https://doi.org/10.1145/3203207) | 用能量而非纯力触发断裂 | 部分采用：记录 `0.5·I·v_approach` 作为冲击能量代理 | 将能量阈值、材料韧性与碎片尺寸分布绑定 |

### 这些论文给出的“好想法”，哪些最适合下一步

1. **预碎裂 + 运行时切换**（Parker & O'Brien）：与本引擎当前“盒体实例化”最兼容，先做离线凸包碎片，再扩展渲染与 SAT。
2. **艺术可控的 fracture modes**（Sellán et al.）：比“随机切”更能服务关卡设计；应暴露切面方向、碎片尺寸分布、材料韧性。
3. **小步长**（Macklin et al.）：当前自适应子步是工程折中；做固定高频率子步并用 replay 摘要验证，是低风险提升。
4. **冲击传播**（Guendelman et al.）：当前塔倒塌靠宏观支撑图，堆叠稳定性仍可提升；岛级传播能减少“软塔”抖动。
5. **能量/应力驱动**（O'Brien & Hodgins）：把标量伤害升级为材料韧性 + 冲击能量 + 裂纹方向，是“真实感”提升最大的一步。

---

## 9. 相对本引擎原有能力的创新点

> 这里的“创新”指**在本项目工程边界内新增的可用能力**，不是宣称学术首创。

1. **确定性的可破坏物理**：破坏不是仅视觉特效；断裂、碎片速度、损伤值和计数全部进入 120 Hz 摘要与 replay。当前引擎的其他视觉系统（粒子、体积云）明确不承诺确定性，物理破坏补上了这一块。
2. **宏观支撑图 + 微观刚体碎片的两层破坏**：对象级支撑图决定“哪一块应当塌”，刚体层决定“塌下来如何碰撞”。比单纯把整面墙换成碎片更能表达结构坍塌，也比脚本动画更可交互。
3. **冲量到伤害的物理绑定**：用接触求解器真实的法向冲量驱动伤害，而不是碰撞回调里拍一个固定伤害值；同一套代码同时服务投射物、坠落碎片、爆炸。
4. **自适应碎片的预算控制**：按最大体积切分、总质量守恒、全局 220 块上限、生命周期回收，兼顾视觉密度与帧预算。
5. **场景事务层的一等公民**：`physics` / `destructible` 不是渲染器私有开关，而是经过 Schema、C++、Node 三处校验、可事务提交、可 lint 的场景数据。
6. **证据门禁**：同一机位的前后截图、两次运行摘要一致、物理步预算、对象/碎片计数，全部写入一份可重放 JSON；不是“截一张图说完成了”。
7. **不伪造能力**：报告显式列出 box-only、非 Voronoi、无遮挡爆炸、运动学角色等限制，避免把“有字段”当成“有效果”。

---

## 10. 性能

本机 1280×720、166 个活动刚体（142 碎片）时：

- 平均物理步约 **0.90 ms**；
- 最大单步约 **5.54 ms**（爆炸/大量新碎片同帧）；
- 碎片稳定后进入休眠，空转成本下降；
- 宽相默认走动态 AABB 树（< 64 活动体自动退回 O(n²)）。本机基准：167 体 1.71 → 1.24 ms/步，1,001 体 46.6 → 13.1 ms/步，候选对 500,500 → 1,039，接触数不变。1k 体时瓶颈已从宽相转到求解器本身。

优化顺序建议：

1. ~~宽相 BVH / sweep-and-prune~~（已完成，见 `docs/PHYSICS-SCALE-PLAN.md`）；
2. 岛级休眠与约束批处理；
3. 固定 240 Hz 子步 + SIMD；
4. 碎片 LOD：远处碎片合并为简化凸块或休眠代理；
5. GPU 宽相/接触生成（保持 CPU 求解与摘要，先保证确定性）。

---

## 11. 路线图

**P1（最接近“真实破坏”）**

- 离线凸 Voronoi 预碎裂：烘焙已完成（4a），待凸网格渲染（4b）与凸包碰撞（4c）。
- 裂纹面材质、UV 与内部颜色。
- 冲击能量/材料韧性驱动的碎片尺寸分布。

**P2（结构）**

- ~~对象内 bond 图~~（已完成，`mode: "bonded"`）；岛级冲击传播与更严格的支撑/摩擦承载。
- ~~门、铰链、绳索的 XPBD 关节~~（C++ API 已完成，待进场景格式）。

**P3（角色与玩法）**

- 双向角色刚体/胶囊：角色质量、被箱子推动、斜坡与台阶连续接触（当前已有 AABB 阻挡/站立的简化版）。
- 拾取/投掷任意刚体；蓄力投掷与命中点预测。
- 爆炸遮挡与破片伤害。

**P4（规模）**

- ~~BVH 宽相~~（已完成）；并行约束、碎片 LOD、GPU 接触生成。
- 物理与粒子/体积烟耦合：冲击扬尘、火焰点燃、烟被碎片扰动。

---

## 12. 文件索引

| 路径 | 作用 |
| --- | --- |
| `engine/native/physics.hpp` | 刚体、接触、世界、统计接口 |
| `engine/native/physics.cpp` | SAT、流形、冲量求解、子步、休眠、爆炸 |
| `engine/native/broadphase.hpp` | 确定性动态 AABB 树（BVH 宽相） |
| `engine/native/fracture.hpp` | 键合破坏：cell + bond + 能量阈值 + 并查集 |
| `tests/native_joints.cpp` | 关节：摆/门/限位/绳/断裂/确定性 |
| `tests/native_fracture.cpp` | 键合破坏：拓扑/阈值/岛/扩洞 |
| `tools/offline/voronoi.mjs` | 离线凸 Voronoi 烘焙（4a） |
| `tests/voronoi.test.mjs` | Voronoi 资产：确定性/体积/凸性/OBB |
| `.screen/bonded-wall.scene.json` | 键合墙体演示 |
| `tests/native_broadphase.cpp` | 宽相超集保证、proxy 生命周期、与 O(n²) 逐位等价 |
| `docs/PHYSICS-SCALE-PLAN.md` | BVH / XPBD 关节 / 键合破坏 / 凸 Voronoi 四阶段方案 |
| `engine/native/vec3.hpp` | 玩法与物理共享向量类型 |
| `engine/native/game.cpp` | 场景→刚体、伤害、碎裂、支撑图、投射物、报告 |
| `host/scene_loader.cpp` | `physics` / `destructible` 解析与约束 |
| `engine/scene/index.mjs` | Node 侧同构校验 |
| `assets/scene.schema.json` | 场景 Schema |
| `.screen/physics-lab.scene.json` | 三塔 + 动态箱 + 爆炸桶演示 |
| `tests/native_physics.cpp` | 求解器单元测试 |
| `tests/native_destruction.cpp` | 投射物/坍塌/重置/确定性测试 |
| `.incrementalTesting/simulation/physics.ps1` | 前后截图与证据门禁 |
| `reports/physics-evidence.json` | 机器可读证据 |
