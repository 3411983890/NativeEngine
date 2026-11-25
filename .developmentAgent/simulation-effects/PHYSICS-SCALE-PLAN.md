# 物理规模化与真实破坏：四阶段实施分析

- 状态：F1 已落地；F2（关节）与 F3（键合破坏）已实现并有测试；F4 的离线烘焙（4a）已实现，4b/4c/4d 未开始
- 基线：0.11.0 确定性刚体 + 盒式破坏（[PHYSICS-DESTRUCTION.md](PHYSICS-DESTRUCTION.md)）
- 日期：2026-10-22
- 原则：每一步独立可回滚、有测试与报告、不破坏逐位确定性契约

本文回答一个问题：在当前这套**单线程、120 Hz、顺序冲量、盒体**的物理上，按什么顺序加 BVH 宽相、XPBD 关节、能量/键合破坏和离线凸 Voronoi，才能每一步都可用、可测、可退。

---

## 0. 结论先行

| 功能 | 依赖 | 风险 | 状态 | 判断 |
| --- | --- | --- | --- | --- |
| F1 BVH 宽相 | 无 | 低 | **已完成** | 纯过滤层，接触流不变，立刻解决规模问题 |
| F2 XPBD 关节 | 无（与破坏解耦） | 中 | **已实现**（C++ API + 测试，未进场景格式） | 必须是**叠加约束层**，不能替换主求解器 |
| F3 能量/键合破坏 | F1 让碎片规模可行 | 中 | **已实现**（模块 + 场景字段 + 测试） | 复用现有盒格，**不需要 Voronoi** |
| F4 离线凸 Voronoi | F3 的 cell/bond 图 + 渲染 + 烘焙 | 高 | **4a 已完成**，4b/4c/4d 未开始 | 最后做，且要拆成 4 个可停的步骤 |

三条关键判断：

1. **不要用 XPBD 替换顺序冲量求解器。** 现有伤害链消费的是接触求解器给出的真实法向冲量（`physics::Impact::normalImpulse`），XPBD 的位置投影不直接产生这个量。XPBD 应作为关节/键合的独立约束层，与接触层交替迭代。
2. **F3 与 F4 共用一套抽象。** 键合破坏的 `cell + bond + 并查集` 正是 Voronoi 预碎裂的数据结构。F3 先把这套结构跑通（盒格），F4 只是把 cell 的形状从盒换成凸多面体、把烘焙搬到离线，**不是重写**。
3. **F4 的成本不在算法，在工具链。** Voronoi 切分本身一周能写完；真正卡住的是凸包碰撞、网格实例渲染、BLAS/TLAS、材质断面和"美术能预览重烘焙"的内容管线。

---

## 1. 现状盘点（以代码为准）

### 1.1 宽相：O(n²)，已是明确瓶颈

- 旧实现是 `World::generateContacts()` 里的双重循环，对每对活动体先做世界轴 AABB 拒绝，再进 15 轴 SAT。
- 166 个活动刚体时约 **13.8k 对/子步**；1k 刚体时 **500k 对/子步**，全部浪费在 AABB 拒绝上。
- `PHYSICS-DESTRUCTION.md` §10 已把"宽相 BVH / sweep-and-prune"列为优化顺序第 1，本次实施的就是这一条。

### 1.2 关节：完全缺失

- `physics::Body` 只有 6 个自由度，没有任何约束数组；`Settings` 里没有 joint 迭代次数；`World` 没有约束求解入口。
- `docs/PHYSICS-DESTRUCTION.md` §2 已解释过为什么当时不上 XPBD：主求解器需要真实冲量、需要与 120 Hz 手感和回放共存。这个理由今天仍然成立，但它**不阻止**把 XPBD 作为附加层。

### 1.3 破坏：对象级 health，不是对象内结构

- `Game::processImpacts()` 把 `Impact::normalImpulse` 换算成标量伤害，超过 1 就调用 `fractureEntity()`。
- `fractureEntity()` 一次性把**整个实体**替换成 `splitBox()` 递归二分出的 N 个盒碎片，原实体标记 `fractured_` 并停用。
- `collapseUnsupported()` 是**对象之间**的支撑图（顶面/底面高差 + XZ 投影重叠），不是对象内部的键合图。
- 结论：现在打一枪，墙要么完整、要么整面消失。缺的正是"局部破洞、整体仍站立"。

### 1.4 渲染：只有实例化 OBB

- `Pose` 只有 position/scale/yaw/pitch/roll + 材质槽；渲染路径按定向盒实例化，DXR TLAS 也是盒实例。
- 所以任何"凸网格碎片"都必须新增网格实例路径、新的 BLAS 构建和材质断面处理。这是 F4 被排到最后的技术原因。

### 1.5 确定性：所有新状态都要进哈希

- `Game::digest()` 覆盖位置、四元数、速度、休眠、损伤、断裂计数。
- 新增关节（λ 累积、约束状态）和键合（bond 完整性、并查集根）**必须**进入摘要，否则回放会静默漂移。

---

## 2. F1：BVH 宽相（已落地）

### 2.1 设计

新增 `engine/native/broadphase.hpp`（header-only，零构建脚本改动）：

```text
AabbTree
  ├─ fat AABB 叶子：只有紧包围盒离开存储盒才重新插入
  ├─ SAH 下行插入（表面积增量最小），不做 AVL 旋转
  ├─ 重建启发：insertions > max(64, 4n) 或 height > 48 → 中位数重建
  ├─ proxy 句柄稳定：重建只换节点，不换句柄
  └─ queryPairs() 输出重叠叶子对（遍历序，调用方排序）
```

`World` 侧：

```text
generateContacts()
  ├─ updateBroadphase()        活动体建/更新 proxy，非活动体销毁
  ├─ 模式 0（brute）：所有 (i,j) 按索引序入候选
  ├─ 模式 1（BVH）  ：queryPairs → 按 (a,b) 排序 → 去重
  └─ 逐对 collidePair()        与原内层循环完全相同的过滤 + SAT + 流形
```

### 2.2 为什么能保持逐位一致

树**只做候选过滤**，不做任何几何判定：

1. 叶子存的是紧 AABB 外扩 `margin`（默认 0.02，最小 0.004），因此树输出的对**必然包含**精确 AABB 测试会接受的所有对。
2. 候选对按 `(bodyA, bodyB)` 排序后再跑窄相，接触生成顺序与 O(n²) 路径完全相同。
3. 窄相、求解、位置修正、休眠、冲量收集的代码路径一份不改。

测试 `testWorldEquivalence` 直接断言：同一场景分别用 `broadphase=1` 与 `broadphase=2` 跑 900 tick，每一步接触数相等，每 30 tick 的位置/四元数/速度/休眠**按位相等**。

### 2.3 回退与开关

```cpp
struct Settings {
    int broadphase = 0;            // 0 auto, 1 brute, 2 bvh
    int broadphaseThreshold = 64;  // auto 模式下活动体数量阈值
    float broadphaseMargin = 0.02f;
};
```

- 小场景（< 64 活动体）自动走 O(n²)，避免树的固定开销。
- 运行时可切换；切回 brute 会销毁全部 proxy，不会留下幽灵叶子。
- 报告新增 `broadphase` / `broadphasePairs` / `broadphaseNodes` / `broadphaseRebuilds`。

### 2.4 证据（本机实测，1280×720 无关，纯 CPU 物理）

| 场景 | 模式 | 活动体 | 候选对/子步 | 平均步耗时 | 接触数 |
| --- | --- | --- | --- | --- | --- |
| 5 层箱堆 + 散块 | brute | 167 | 13,861 | 1.711 ms | 595 |
| 同上 | BVH | 167 | 174 | **1.243 ms** | 595 |
| 10³ 箱堆 | brute | 1,001 | 500,500 | 46.638 ms | 3,472 |
| 同上 | BVH | 1,001 | 1,039 | **13.105 ms** | 3,472 |
| `physics-lab` 破坏演示 | BVH | 155 | 435 | **0.58–1.03 ms**（多次运行） | 492 |

- 167 体：**-27%**；1,001 体：**-72%**，候选对减少 **99.8%**，接触数完全一致。
- `physics-lab` 证据门禁（`npm run physics`）通过 11/11：11/11 断裂、142 碎片、3 次坍塌、峰值冲量 80.4663、两次运行摘要一致（hash `10ae33d50b76fe3b`）；平均步耗时落在 **0.58–1.03 ms**、最大步 **2.73–4.22 ms**（同一二进制多次运行，差异来自机器负载），而断裂/碎片/坍塌计数与峰值冲量完全不变——提速没有换来任何行为变化。1k 体的 A/B 基准是同一进程内背靠背测量，不受跨天负载影响，可信度更高。
- 测试：`native_broadphase_tests`（超集保证、proxy 生命周期、逐位等价、auto 切换、400 体稀疏过滤）。
- 注意：1k 体 13 ms 里大部分是求解器（3,472 接触 × 12 迭代），宽相已不是瓶颈；下一步要动的是岛级休眠与约束批处理。

### 2.5 已知边界

- 单线程；每子步仍要 O(n) 更新所有活动体 proxy（这一步无法用树省掉）。
- 休眠体仍占叶子（`moveProxy` 会早退，成本是一次包含测试）。真正的岛级休眠是独立工作包。
- 树质量靠重建而非旋转；极端插入顺序下会有周期性 O(n log² n) 重建，实测 1k 体 120 步内 25 次，可接受。

---

## 3. F2：XPBD 关节（已实现）

### 3.1 定位：叠加，不替换

现有求解器给出的是**速度级冲量**，伤害、音效、粒子都依赖它。XPBD 给出的是**位置级投影 + λ 累积**。两者的正确关系是：

```text
每个子步 h：
  integrateVelocities(h)
  generateContacts()
  solveContacts(h)                 // 顺序冲量，产生 Impact 冲量
  for i in 0..positionIterations:  // 注意：correctPositions() 目前自带这个循环，
      correctPositionsOnce()       // 需先抽成单次迭代，才能与关节交替
      solveJoints(h)               // 新增：XPBD 位置投影
  integratePositions(h)
  updateSleep(h)
  collectImpacts()
```

关节放在位置迭代里，与位置修正交替，能让"门被箱子压住"这类耦合在 2–3 次迭代内收敛，而不需要把接触改成 XPBD。

### 3.2 数据模型

```cpp
enum class JointType { Point, Hinge, Rope };

struct JointDesc {
    JointType type = JointType::Point;
    unsigned bodyA = 0, bodyB = 0;   // bodyB = kInvalid 表示钉在世界点
    Vec3 localAnchorA, localAnchorB; // 体局部坐标
    Vec3 localAxisA;                 // Hinge 轴
    float restLength = 0.0f;         // Rope 段长
    float compliance = 0.0f;         // 0 = 刚性
    float lowerLimit = -3.14159f;    // Hinge 角度限位（弧度）
    float upperLimit = 3.14159f;
    float breakImpulse = 0.0f;       // 0 = 不可断
};
```

运行时每个约束只存：`lambda`（累积拉格朗日乘子）、`axis`（世界空间）、`有效质量`。

### 3.3 三种原语，覆盖需求里的门/绳/悬挂

| 需求 | 实现 | 说明 |
| --- | --- | --- |
| 悬挂（吊灯、吊桥板） | 1 个 Point 约束 | 锚点重合，`C = |pB - pA|` |
| 门/合页 | **2 个 Point 约束**，锚点沿铰链轴分开 | 两点约束消掉 5 个自由度，只剩绕轴旋转；比四元数铰链约束好实现、好调 |
| 绳索 | N 段距离约束 + 每段一个小动态体 | 复用刚体与渲染，不需要软体求解器 |
| 断裂连接 | `breakImpulse` 超过阈值时删除约束 | 与 F3 的 bond 用同一套"能量→断开"判定 |

### 3.4 与接触求解器耦合的三个坑

1. **关节把体推进墙**：XPBD 投影是纯位置的，会把穿透留给下一子步的 Baumgarte。缓解：关节投影后立即跑一次 `correctPositions()`，且关节体禁用恢复系数（`restitution = 0`）。
2. **大质量比抖动**：门 20 kg、箱子 200 kg 时，P2P 的有效质量差 10 倍。缓解：compliance 按 `1/(m_eff)` 归一，并对关节体做 mass splitting（把重体虚拟拆成两份）。
3. **顺序敏感**：joint 与 contact 的迭代顺序会改变结果。契约：**固定先 contact 后 joint**，并把关节 λ 与约束顺序写入 digest。

### 3.5 验收标准

- 门：绕轴开合 90°，限位处无抖动（角速度 < 0.02 rad/s 持续 1 s），被 5 kg 箱子撞击后不穿透。
- 绳：10 段 0.3 m 绳自然下垂，总长误差 < 5%，静置 10 s 不发散。
- 悬挂：20 kg 箱吊在 2 m 绳上，静置位移 < 1 cm。
- **回归**：无关节场景的 digest 与 0.11.0 逐位一致（这是硬门槛，防止"加了关节求解器就漂了"）。

### 3.6 风险与回滚

| 风险 | 缓解 |
| --- | --- |
| XPBD 与 SI 混用注入能量 | 位置投影 + 速度回写系数 0.9；关节体禁恢复系数 |
| 关节把休眠体永久唤醒 | 关节连接的岛统一休眠判定（与岛级休眠同一工作包） |
| 场景格式膨胀 | 第一版**不加场景字段**，只加 C++ API + 测试；确认手感后再进 Schema |

---

### 3.7 实现与证据

- `physics.hpp`：`JointType{Point,Hinge,Rope}`、`JointDesc`/`Joint`、`World::addJoint/removeJoint/joint`、`Settings::jointIterations = 8`、`Stats::joints/brokenJoints`。
- 子步顺序：`integrateVelocities → generateContacts → solveContacts → correctPositions → captureJointBodies → integratePositions → solveJoints → updateSleep`。`captureJointBodies` 存的是积分前位姿，关节投影后按 `v = (x_corrected − x_previous)/h` 回写，避免把修正量重复计入下一帧。
- Hinge = 沿轴分开的两个点约束 + 角度限位；**限位先解、点约束后解**，否则限位绕质心旋转会把铰链锚点推离门框。
- `collideConnected` 默认 false：门与门框几何重叠，不跳过这对接触会把门从铰链上推走——这是实现时踩到的第一个坑，也是"关节必须与接触层协商"的具体例子。
- 断裂阈值 `breakForce` 比较 `|λ|/h²`（λ 是 kg·m 量纲的 XPBD 乘子，除以 h² 才是力）。12 kg 悬挂体在 120 N 阈值下第 14 个 tick 断开。
- 测试 `tests/native_joints.cpp`（7 项）：单摆（锚点误差 < 3 cm）、门（铰链位移 < 5 cm、摆幅 > 0.3 m）、角度限位、8 段绳（长度误差 < 15%）、断裂、逐位确定性、无关节场景回归。
- **未做**：岛级休眠（关节连接的岛仍按单刚体休眠判定）。

### 3.8 关节进场景格式 + 马达/弹簧

场景侧：

```json
{
  "joints": [
    { "type": "hinge", "bodyA": "<gate-id>", "bodyB": "<post-id>",
      "anchorA": [-1.5, 0, 0], "anchorB": [0.1, -0.1, 0], "axis": [0, 1, 0],
      "hingeSpan": 0.35, "lowerLimit": -1.5, "upperLimit": 0,
      "friction": 0.7, "motorSpeed": -0.9, "motorMaxTorque": 1200 }
  ]
}
```

- 刚体用 **entity id** 引用而不是 body 索引：重置时刚体世界会从场景重建，索引不跨重建存活。
- `anchorB` 省略 `bodyB` 时是**世界点**（求解器从 `anchorB` 读，不是 `anchorA`）。
- 三处校验（`assets/scene.schema.json`、`host/scene_loader.cpp`、`engine/scene/index.mjs`）同时检查：bodyA/bodyB 必须指向存在的实体、`axis` 非零、`lowerLimit ≤ upperLimit`、马达/摩擦只允许在 hinge 上。
- 引用了不存在实体的关节在 `createJoints()` 里被跳过而不是让整场加载失败，`Stats::joints` 会少一个，作者的拼写错误表现为"关节数不对"而不是静默无效。

驱动：

- **弹簧**直接用已有的 `compliance`（XPBD 柔度），场景字段 `compliance` 0..1，无需新积分器。
- **马达与摩擦是速度级的，跑在 XPBD 速度回写之后**。这一点是踩坑踩出来的：先做成位置级（每步把角度往目标推），回写会把这个修正转成角速度，下一步再积分一次，形成正反馈；实测闸门在撞到限位时角速度从 0.9 冲到 26 rad/s，最终整块飞出世界（`rigidbody.velocity` 越界）。改成回写后直接改 `angularVelocity`，角速度稳定等于 `motorSpeed`。
- **限位也要在速度层处理**。只靠位置限位时，马达持续顶着限位，限位每一步都要"纠正"，纠正又变回速度，两者互泵能量。现在按"接近限位就提前刹住"处理（`limitSlop` 约 3°），并把 `limitError`（位置求解测到的越界量）一并作为刹车依据；位置限位只作硬兜底，并加了单步修正上限。
- 马达、限位、摩擦三者合并成**一个目标相对角速度**再单次求解。分别计算再相加会把同一份速度扣两次，反向过冲。
- 关节状态进 `snapshot`/`digest`：`broken`、`angle`。恢复时先清空 `joints()` 再 `createJoints()` 回填——不清空会让每个铰链翻倍；**不存 joint id**，因为 id 是每个世界各自分配的，存了会让两个等价世界比对不等。
- 演示场景 `.screen/joint-yard.scene.json`：马达闸门（限位 −1.5..0、摩擦 0.7、力矩 5000）、可撞开的门（限位 ±1.3、摩擦 0.4）、绳索吊灯（`restLength` 2.3、`compliance` 0.0005）。俯视前后对照显示闸门从正对镜头转到约 85°，停在 −1.476 rad 且速度归零。
- 测试：`testHingeMotorAndFriction`（马达把闸门推到限位并停住；带摩擦的门摆幅严格小于自由门）+ `testSceneJointsAreCreated`（场景 → `Stats::joints`、缺失实体被跳过、存读档后 60 tick 逐帧快照相等）。
- 已知取舍：XPBD 铰链的投影本身就会耗散角速度（实测自由门 1 s 内从 4 rad/s 衰减到 0.95 rad 的摆幅），所以 `friction` 只在上面叠加一小块，测试断言因此是"严格更小"而不是某个比值。
- 摘要版本升到 `nativegpu.game/4`（新增关节状态），旧录像哈希会变。

## 4. F3：能量/键合破坏（已实现）

### 4.1 目标与不变量

把"整对象 health"换成"对象内 cell 图"：

- 打一枪 → 局部几个 bond 断 → 墙上出现一个洞，**墙还在**。
- 洞扩到失去支撑 → 那一片 cell 组成的连通分量整体脱落，成为刚体。
- 质量、体积、动量守恒；cell 数量与 bond 数量进报告。

### 4.2 数据结构（为 F4 预留）

```cpp
struct Cell {                 // 现在 = 盒格；F4 = 凸多面体
    Vec3 center, half;        // 局部坐标
    float mass = 0.0f;
    float volume = 0.0f;
    int body = -1;            // 脱落后的刚体索引
    unsigned island = 0;      // 并查集根
};

struct Bond {
    int cellA = -1, cellB = -1;
    float area = 0.0f;        // 共享面面积，决定承载力
    float toughness = 1.0f;   // 材料韧性
    float damage = 0.0f;      // 累积损伤 0..1
    bool broken = false;
};
```

**关键**：`Cell` 只暴露 `center/half/volume/mass`，F4 换成凸多面体时只替换几何查询（支撑函数、AABB、惯量），bond 图与并查集不动。

### 4.3 能量模型

现有 `Impact` 已经给出 `normalImpulse` 与 `approachSpeed`，直接用：

```text
E_contact = 0.5 · J_n · v_approach          // 冲击能量（与现有 totalImpactEnergy_ 同源）
分配：以接触点为中心，半径 r 内的 bond 按 1/(1+d²) 权重分摊
bond.damage += E_bond / (toughness · area · k)
bond.damage ≥ 1 → broken
```

比"冲量阈值"多两个性质：**面积大的 bond 更结实**（墙角和墙面不同）、**能量会分摊**（同一枪打不断整面墙）。

### 4.4 断裂流程

```text
1. 断开的 bond 置 broken
2. 并查集（路径压缩，按 cell 索引升序 union）求连通分量
3. 每个分量：
   - 若仍与静态支撑接触 → 保持静态，只更新可见 cell 集合（洞）
   - 否则 → 生成一个动态刚体（质量/惯量/质心由 cell 求和）
4. 原 entity 的碰撞体从 World 移除，cell 刚体接管
5. 分量刚体再次受击 → 递归走同一流程（子图）
```

### 4.5 为什么它排在 Voronoi 之前

- cell 就是盒体 → **现有实例化 OBB 渲染路径直接可用**，渲染器零改动。
- 碰撞仍是 OBB SAT → 物理零改动。
- 洞的视觉是"少画几个盒子"，不需要新网格、新材质、新 BLAS。

这三条让它成为"用现有渲染/碰撞能力就能拿到的最大真实感提升"。

### 4.6 验收标准

- 单发投射物：墙上出现 ≤ 6 个 cell 的洞，墙体其余部分保持静态，`fractures` 计数不增加（不是整对象碎裂）。
- 连续三发同点：洞扩大，最终该列 cell 失去支撑，整列脱落为一个动态刚体。
- 质量守恒：Σ cell 质量 = 原实体质量，误差 < 1%。
- 性能：单面 12×8 cell 墙，断 20 个 bond 的并查集 < 0.05 ms。
- 确定性：bond 顺序、并查集顺序固定，两次运行 digest 一致。

### 4.7 风险

| 风险 | 缓解 |
| --- | --- |
| cell 数量爆炸（塔 5 层 × 12 格 = 60） | 每实体 cap（如 128 cell），超出退回整对象碎裂 |
| 洞边缘 cell 抖动 | 与静态邻居保留"虚拟 bond"，脱落前不参与求解 |
| 与现有支撑图冲突 | `destructible.mode` 区分 `entity` / `bonded`，默认 `entity` 保持 0.11 行为 |
| 场景格式与三处校验 | 字段先只进 C++，稳定后再同步 Schema/loader/Node 三处 |

---

### 4.8 实现与证据

- `engine/native/fracture.hpp`：`Cell`/`Bond`/`Structure`，`buildBox`、`applyEnergy`、`islands`（并查集）、`chooseDivisions`。
- 能量模型：`cell.damage += E_share / (toughness · faceArea)`；单元损伤 ≥ 1 时**粉碎**并断开它的全部 bond，形成洞；半径内的 bond 另外累积损伤，所以重复命中会扩洞而不是停在一个格子。
- `Game`：`Entity.bonded/bondedCells`；`destructible.mode`/`cells` 在 Schema、C++ loader、Node validator 三处同步；首次断键把实体刚体换成每 cell 一个静态刚体；最大岛保持，其它岛若不着地则转动态并继承父体速度。
- 渲染零改动：cell 就是盒体，走现有实例化 OBB 路径。这正是它排在 Voronoi 之前的原因。
- 测试：`native_fracture_tests`（6 项）+ `native_destruction` 的 bonded 用例（开洞、墙不倒、`fractureCount == 0`、穿过洞不受伤、第二发扩洞）+ `testBondedSnapshotRoundTrip`（开洞后 `snapshot`/`restore`，再同输入推进 240 tick，两个世界快照逐帧相等）。
- 场景：`.screen/bonded-wall.scene.json`（`scene.mjs validate` 与 `lint` 均通过）。
- 证据：宿主新增 `--shot frame:x,y,z`（可重复，走与 F 键相同的 `fireProjectileAt`），因此任意场景都能产出同机位的完好/破坏对照。`.screen/bonded-wall.scene.json` 同相机两发偏移命中：`bodies 9→57`、`bondedHoles 0→4`、`bondedBrokenBonds 0→14`，`fractures` 仍为 `0`、`intactDestructibles` 仍为 `2`；截图见 `reports/bonded-wall/{intact,holes}.png`，命令见 `docs/PHYSICS-DESTRUCTION.md` 第 4.4 节。
- 存档：键合结构随 `snapshot`/`restore` 往返。bond 图由场景确定性重建，只回填可变状态（损伤、断键、cell 刚体索引），并把形状与计数与重建网格比对，不匹配则拒绝而不是静默采用。
- **未做**：cell 尺寸是均匀网格，不是 Voronoi；碎片没有断面材质；authored `--shot` 不进 replay 输入流（replay 只录键盘输入），所以键合破坏的 replay 仍需用 `--physics-demo` 或手按 F。

## 5. F4：离线凸 Voronoi（4a 已实现）

### 5.1 为什么必须最后

它同时锁死四个子系统，任何一个没通都会让整体不可用：

| 子系统 | 现状 | 缺口 |
| --- | --- | --- |
| 碰撞 | OBB SAT | 凸-凸 GJK/EPA 或通用 SAT + 流形 |
| 渲染 | 实例化 OBB | 网格实例、逐 cell BLAS、断面材质 |
| 资产 | 场景 JSON + 纹理 | 离线烘焙产物格式、版本、校验 |
| 工具链 | 场景编辑器 | 预览、调参、重烘焙、diff |

### 5.2 四个可停步骤

**4a 离线烘焙（无运行时改动）**

```text
输入：box/网格 + 种子 + 单元数
输出：cells[]（凸包顶点/面/体积/惯量/质心）+ bonds[]（邻接面）+ 元数据
工具：node tools/offline/voronoi.mjs --in ... --out ... --seed ...
门禁：同输入两次烘焙 SHA-256 一致；Σ 体积 = 原体积 ±0.5%
```

**4b 视觉先行（物理仍是 OBB 代理）**

- 渲染器新增"凸网格实例"路径；物理仍用每个 cell 的 OBB 代理。
- 这一步就能看到真正的 Voronoi 断面，物理手感与盒碎片一致。
- 风险最低，收益最直观，**建议在这里停一次**给美术评估。

**4c 凸碰撞替换代理**

- 实现凸-凸窄相（GJK 距离 + EPA 穿透 + 面裁剪流形）。
- 保留 OBB 路径作为 `collision=obb` 回退，A/B 对比穿透深度与堆叠稳定性。
- 门禁：与 OBB 代理相比，静止穿透深度下降 ≥ 50%，堆叠高度误差 < 3%。

**4d 断面与内容**

- 裂纹面 UV、内部颜色、湿/干差异；烘焙参数进场景事务层。
- 门禁：美术可在编辑器内改单元数/种子并重烘焙，diff 可读。

### 5.3 风险

- **工具链是主要成本**，不是算法。没有预览与重烘焙，Voronoi 只能做一次性 demo。
- 凸包碰撞的流形生成比 SAT 难一个量级；先只做"单点 + 面裁剪"两种，边-边退化用 EPA 法线兜底。
- 逐 cell BLAS 会让 TLAS 实例数从几十涨到几千，需要按"同一实体的 cell"合并 BLAS 或做实例 LOD。

---

### 5.5 4a 实现与证据

- `tools/offline/voronoi.mjs`：用半空间裁剪求每个种子的 Voronoi 单元，输出顶点、面、体积、质心、**OBB 代理**（协方差主轴 + 紧致半长）和邻接 bond 面积。
- 资产 schema `nativegpu.voronoi/1`；`bake` 自带体积校验，`verify` 可对已有文件复核。24 格墙体积和 1.800002，箱体 1.8。
- 测试 `tests/voronoi.test.mjs`（6 项）：同种子逐字节一致、体积守恒、单元凸性（所有顶点在面内侧）、邻接有序且面积为正、退化请求被拒绝、OBB 包住全部顶点且不小于单元体积。
- 复现：`npm run voronoi -- bake --half 1.5,1,0.15 --cells 24 --seed 7 --out build/voronoi/wall.json`。

### 5.6 4b 实现与证据（凸网格实例渲染）

- `engine/native/convex_mesh.hpp`：`ConvexVertex`（position+normal+uv，32 字节，与渲染器立方体同布局）、`ConvexMesh`、`VoronoiShardProxy`、`VoronoiLink`、`VoronoiPackage`。物理与渲染共用这一个头，避免两套顶点约定。
- 渲染器：所有碎片网格**拼进一对顶点/索引缓冲**，绘制时只改字节偏移（`DrawItem::mesh`），不切换资源；`configureConvexMeshes` 在 `initialize()` 前调用则延迟到初始化上传，之后调用则立即上传并等 GPU。
- TLAS：凸网格 item 的 `model` 没有缩放，直接塞进单位立方体 BLAS 会得到错误尺寸的影子，因此用碎片包围盒 + 原旋转构造影子代理（`convexRanges[].boundsMin/Max`）。
- 加载器：把烘焙面片转成三角面，按记录的外法线**校正绕序**（烘焙不保证绕序，内翻会读成洞）；顶点平移使质心落在原点，使 `Pose` 只负责位姿。
- 物理代理：用平移后凸包的 **AABB**，不用烘焙的 OBB。刚体没有独立于实体朝向的坐标系，OBB 轴会被错误旋转；AABB 在刚体实际使用的坐标系里既保守又精确。
- 场景字段：`destructible.voronoi`（路径），Schema / C++ loader / Node validator 三处校验，且要求 `mode: "bonded"`。
- 测试：`testBakedShardsReachRenderPoses`（Game 层：包被采用、网格表大小、`Pose::mesh` 与 `scale=1` 约定、未破碎时不画 hull）+ `testAdoptBakedShards`（fracture 层：adopt 合法性、非法图被拒、重复 adopt 重置）。
- 证据：`.screen/bonded-wall.scene.json` 两面墙各挂一个烘焙包（48 / 40 单元），`--shot` 命中后墙面呈不规则凸块拼贴。

### 5.7 4d 实现（断面 UV 与重烘焙）

- UV：按面法线主轴在**实体自身坐标系**里投影，分母用烘焙包记录的 `half`，因此碎片与完好墙采样同一片纹理、接缝对得上。
- 断面：按"法线轴对齐 + 平面落在该半长上"判定外表面，其余为切割面，单独成网格；运行时作为第二个 pose 用更暗、更粗糙的染色绘制，不需要新的顶点属性或着色器分支。
- 重烘焙：`node tools/offline/voronoi.mjs bake-scene --scene <scene>` 读取每个 `destructible.voronoi` 引用，按实体 `scale` 与 `cells`/`seed` 重新烘焙，写回同一路径。改单元数不再是手工编辑资产。

### 5.8 4c 实现（凸-凸窄相）

- `engine/native/convex.hpp`：支撑函数、GJK 重叠判定、EPA 穿透深度、面裁剪流形。形状描述为「中心 + 正交基 + 半长（盒）或局部点云 + 三角形索引（凸包）」，不依赖物理世界，可单独测试。
- **为什么值得做**：烘焙资产上量过，碎片 AABB 体积 / 凸包体积中位 **1.97×**、p90 2.84×、最大 4.21×。包围盒有完整平底且质心必在底面内，所以碎片**永远站得住**——视觉上就是斜切的碎片靠一个尖角立着不动。
- 返回值是**三态**（`Disjoint` / `Overlapping` / `Failed`），不是 bool。只有 `Failed` 才回退到 SAT 盒路径：把"算法放弃"当成"分离"会静默丢碰撞，当成"接触"会凭空造碰撞。
- 接入点 `World::collidePair`：任一方带 `hullPoints` 就走凸路径，否则走原 SAT。盒子体的两条路径描述的是同一几何，所以盒子行为不变。
- 接触落地抽成 `World::emitContacts`，两条路径共用，避免复制那段摩擦/恢复系数/有效质量的代码。
- 踩到的坑：
  - GJK 四面体约简的判据必须是「原点在哪个面的外侧」，不能用「最小平面距离」——后者会让单纯形在 3/4 点之间无限振荡，最后被判成分离。
  - EPA 的法线**不需要翻转**：A⊖B 被推向 -B 侧，所以最靠近原点的边界本来就落在 B 一侧，正好是引擎"从 A 指向 B"的约定。
  - 裁剪出的接触点要投影到参考面上，否则接触点浮在入射面上。
  - 断面网格是开放壳体，渲染器的网格校验不能要求"封闭"。
- 测试 `tests/native_convex.cpp`（8 项，全解析解）：分离（含对角）、两轴上的深度与法线、深穿透（质心在内部）、凸包对盒、凸包对凸包、逐位确定性、45° 旋转盒的半长（1.414 而不是 AABB 的 1.0）。
- 行为测试 `testHullCollidesAsItsShape`：同一四面体，盒代理落地后倾角 < 8°（平底，永不倾倒），凸包落地后倾角 > 15° 且重心更低——这才是 F4c 存在的理由，断言的是两者**确实不同**而不是"没崩"。
- 已知限制：惯性张量仍是盒体（烘焙里已有质心，可后续用上）；凸路径比 SAT 贵；碎片数量大时是实打实的开销。

## 6. 横切契约

| 契约 | 要求 |
| --- | --- |
| 确定性 | 新增状态（joint λ、bond damage、island 根）必须进 `Game::digest`；随机必须由 seed 派生 |
| 回退 | 每项一个 `Settings` 开关，旧路径保留并测试 |
| 证据 | 测试 + 报告字段 + 文档三件套；禁止"有字段 = 有效果" |
| 预算 | 物理平均步 < 3 ms（166 体现状 0.9 ms）；1k 体目标 < 8 ms |
| 兼容 | 无新特性的场景，digest 必须与上一版逐位一致 |

---

## 7. 顺序与门禁

```text
F1 BVH（已完成，独立）
        │
        ├──────────────┬──────────────┐
        ▼              ▼              │
F2 XPBD 关节      F3 键合破坏          │
（无共享状态，可并行）  │              │
        └──────┬───────┘              │
               ▼                      │
        F4a 离线烘焙 ◄────────────────┘
               ▼
        F4b 凸网格视觉（物理仍 OBB）
               ▼
        F4c 凸碰撞
               ▼
        F4d 断面与工具链
```

- F2 与 F3 可并行：关节是约束层，键合是几何/图结构，二者只在"断裂连接"处交汇（用同一套能量阈值判定）。
- F4a 依赖 F3 的 `Cell/Bond` 抽象，所以 F3 实现时就要把几何查询抽成函数，而不是把盒体假设写死。

**建议插入项：岛级休眠（F1.5）。** 1k 体实测 13.1 ms/步里，宽相只占很小一部分，大头是 3,472 个接触 × 12 次迭代。F3 会把一个实体炸成几十个 cell 刚体，接触数会再上一个量级。所以在 F3 之前应先做"接触图 → 岛 → 整岛休眠/唤醒"：岛内全部低于阈值才休眠，任一成员被撞则整岛唤醒。它同时解决关节体的休眠问题（§3.6），是 F2/F3 的共同前置。当前逐体休眠在堆叠场景会出现"底层睡着、上层还在抖"的假活跃，岛级判定能直接省掉这部分空转。

**停止条件**（沿用项目既有规则）：

- 构建或测试不通过，不进入下一项。
- 没有报告/截图/摘要证据，不宣称完成。
- 同一问题投入 5 个工作日无进展，保存最小复现并缩小范围。

---

## 8. 本轮已交付

| 文件 | 变更 |
| --- | --- |
| `engine/native/broadphase.hpp` | 新增：确定性动态 AABB 树 |
| `engine/native/physics.{hpp,cpp}` | 宽相接入、`collidePair` 抽取、`Settings`/`Stats` 扩展、`worldBounds` |
| `tests/native_broadphase.cpp` | 新增：超集、生命周期、逐位等价、auto 切换、400 体过滤 |
| `engine/native/fracture.hpp` | 新增：cell + bond + 能量阈值 + 并查集 |
| `tests/native_fracture.cpp` | 新增：拓扑、能量阈值、岛分裂、重复命中、确定性 |
| `tests/native_joints.cpp` | 新增：摆、门、限位、绳、断裂、确定性、无关节回归 |
| `.screen/bonded-wall.scene.json` | 新增：键合墙体演示场景 |
| `tools/offline/voronoi.mjs`、`tests/voronoi.test.mjs` | 新增：离线凸 Voronoi 烘焙与校验 |
| `tools/build.ps1`、`.incrementalTesting/core/test-native.ps1` | 新增 `native_broadphase_tests` |
| `host/main.cpp` | 报告新增 broadphase 四个字段 |

复现：

```powershell
npm run test:native          # 含 native_broadphase_tests
npm run physics              # 证据门禁，报告含 broadphase 字段
```

**下一步**（建议按此顺序开新工作包）：

1. F2 第一切片：`Joint` + Point/Hinge，只加 C++ API 与 `tests/native_joints.cpp`，不动场景格式。
2. F2 第二切片：Rope（距离链）+ 与接触的耦合测试。
3. F3 第一切片：把 `splitBox` 的产物升级为 `Cell[] + Bond[]`，只做"断 bond + 并查集 + 报告"，暂不接渲染。
