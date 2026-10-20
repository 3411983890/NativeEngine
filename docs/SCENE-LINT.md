# 场景设计期 lint 与可达性检查（S3E-07 / INN-07）

- 状态：**离线规则引擎 + CLI**，不依赖渲染器、宿主或 D3D12
- 代码：`engine/scene/lint.mjs`（纯函数、零 import、无 I/O）
- CLI：`tools/scene.mjs lint <scene.json> [--json] [--max-entities N]`
- 测试：`tests/lint.test.mjs`（`node --test tests/lint.test.mjs`）
- 定位：报告 `validateScene` 这类 schema 校验器**看不见**的结构、引用与可达性问题。
  校验器回答"文档是否合法"，lint 回答"这关能不能玩"。

## 1. 返回结构与约定

```js
lintScene(document, options) -> {
  ok: boolean,                       // 不存在 severity === 'error' 的 finding
  findings: [{
    code: 'NG-LINT-...',             // 稳定规则码
    severity: 'error' | 'warning',
    entityId: string | null,         // 文档级问题为 null
    message: string,                 // 面向人的一句话
    detail: object,                  // 结构化证据，字段随规则而定
  }],
}
```

- **纯函数**：不读文件、不写文件、不修改输入文档、不依赖时间/随机数/环境。
- **对畸形文档宽容**：`document` 为 `null`、`entities` 缺失、实体不是对象时返回
  findings 而不是抛异常；schema 级错误仍由 `validateScene` 负责。
- 规则码只在 `LINT_RULES` 注册表中出现；每条 finding 的 severity 由注册表决定，
  规则函数不能自行发明 severity。
- 规则按固定顺序输出：结构 → id/kind → 世界范围 → 重叠 → 可达性 → 门/钥匙 → 预算。
  同一文档多次调用输出完全一致。

## 2. 规则表

| 规则码 | severity | 检测什么 | 误报风险 |
| --- | --- | --- | --- |
| `NG-LINT-NO-PLAYER` | error | 没有任何 `kind: "player"` 实体 | 纯沙盒/过场文档会被判错；本引擎的玩法要求恰好一个出生点，因此定级 error |
| `NG-LINT-NO-GOAL` | error | 没有任何 `kind: "goal"` 实体 | 纯展示场景会被判错；同上，首版以"可完成"为前提 |
| `NG-LINT-MULTI-PLAYER` | error | 多于一个 player；每个多余 player 各报一条，`entityId` 指向多余实体 | 无（玩法只支持一个出生点） |
| `NG-LINT-DUP-ID` | error | 两个实体 id 相同（按小写比较，与校验器一致）；报后出现者 | 无；id 只差大小写也算重复，这是运行时的真实语义 |
| `NG-LINT-BAD-KIND` | error | `kind` 不在 8 种内（缺失、非字符串、未知值都算） | 无；新增 kind 必须先改 schema 与 `ENTITY_KINDS` |
| `NG-LINT-OUT-OF-WORLD` | error | 位置 `abs(x) > 10` 或 `abs(z) > 10`，或 `y` 低于地面 | 世界被硬编码为 ±10 正方形、地面取所有 ground 顶面的最高值；非正方形关卡或高低差地形会误报。ground 自身不参与判定 |
| `NG-LINT-OVERLAP` | error | 两个 `collider: true` 的实体 AABB 在三个轴上穿透都超过 `overlapTolerance`（默认 0.2） | 只做轴对齐盒子；墙与墙的转角搭接（穿透 0.2）不算。player 与 ground 不参与，因为角色/道具贴地是正常摆放。小于 0.2 单位的浅浅嵌入不会报 |
| `NG-LINT-KEY-UNREACHABLE` | error | 从出生点出发的可达网格到不了 key 中心 | 只有 2D 粗网格；台阶、跳跃、斜向移动、动态门状态都不建模。key 在 0.5 格边界上可能因格子取整而误报 |
| `NG-LINT-GOAL-UNREACHABLE` | error | 同上，目标是 goal 中心 | 同上；会穿过"本应由钥匙打开"的门（门在 BFS 中视为可通行），门相关的逻辑由下面两条规则负责 |
| `NG-LINT-DOOR-WITHOUT-KEY` | error | 场景有 door 但没有任何 key；每扇门一条 | 场景里只要存在任意 key 就算"有钥匙"——本模型没有 key→door 的引用字段，无法判断某把钥匙是否真能开这扇门 |
| `NG-LINT-KEY-AFTER-DOOR` | warning | 离出生点最近的 key 比某扇 door 更远（XZ 距离），疑似"钥匙在门后" | 纯距离排序启发式：绕路可达、多个门/钥匙、侧门都会误报，因此只给 warning |
| `NG-LINT-ENTITY-BUDGET` | warning | 实体总数超过 `maxEntities`（默认 200） | 只是实体计数，不代表三角形数 / draw call / 显存；预算应按项目调整 |

可达性 finding 的 `detail` 一定包含：

```json
{
  "reason": "separated | blocked-cell | outside-world | player-spawn-blocked",
  "reachablePercent": 72.5,
  "reachableAreaPercent": 72.5,
  "reachableCells": 1058,
  "freeCells": 1460,
  "grid": { "worldExtent": 10, "cellSize": 0.5, "cellsPerAxis": 40, "playerRadius": 0.3 }
}
```

`reachablePercent` 是主字段（0–100，保留两位小数）；`reachableAreaPercent` 是同一数值的
显式别名，避免调用方对命名做猜测。

## 3. 可达性算法

1. **网格**：以原点为中心的 ±10 正方形 XZ 平面，默认格子边长 **0.5 世界单位**，
   即 40 × 40 = 1600 个格子。自定义 `worldExtent`/`cellSize` 时保持正方形，
   每轴最多 512 格（防止病态参数把 lint 变成大计算）。
2. **玩家半径**：默认取 player 的 `max(scale.x, scale.z) / 2`（小院场景为 0.3），
   可用 `playerRadius` 覆盖。
3. **障碍**：`collider: true` 且 `kind` 为 `wall`/`prop` 的实体。把它们的 AABB 在 XZ 上
   膨胀玩家半径（Minkowski 和），格子中心落入膨胀后的矩形即标记为 blocked。
   ground 是可站立平面、door 可被钥匙打开、player/key/goal/light 都不是静态障碍，
   因此都不参与 blocking。
4. **BFS**：从出生点所在格出发，4 邻域（上/下/左/右）洪泛，不跨 blocked 格。
   出生点若落在 blocked 格，最多向外 4 圈寻找最近空格；找不到则以
   `player-spawn-blocked` 报告所有 key/goal 不可达。
5. **判定**：key/goal 中心所在的格子——
   - 在世界外 → `outside-world`
   - 被障碍占住 → `blocked-cell`
   - 未被 BFS 访问 → `separated`
   - 被访问 → 可达，不产生 finding
6. **可达面积百分比**：`reachableCells / freeCells × 100`，即"从出生点能走到的空格
   占全部空格的比例"。它不是路径长度、不是导航网格覆盖率，也不是"关卡完成度"。

BFS 只回答**连通性**：能不能到达。它不产出路径、不计算最短路，也不判断到达后
是否好玩。

## 4. 选项与 CLI

| 选项 | 默认 | 含义 |
| --- | --- | --- |
| `maxEntities` | `200` | 实体预算；超过只给 warning |
| `overlapTolerance` | `0.2` | 三个轴都超过该穿透值才算 overlap |
| `worldExtent` | `10` | 正方形 play area 的半边长 |
| `cellSize` | `0.5` | 可达性网格分辨率 |
| `playerRadius` | `null` | `null` 表示从 player 的 XZ scale 推导 |

```powershell
node tools/scene.mjs lint assets/courtyard.scene.json --json
node tools/scene.mjs lint assets/courtyard.scene.json --max-entities 120
```

- 输出沿用现有 CLI 的 JSON 信封：`{ "ok": true, "result": { "ok": ..., "findings": [...] } }`。
  `tools/scene.mjs` 的所有子命令都只输出 JSON，`--json` 是显式兼容开关，不改变格式。
- **退出码**：存在 `severity: "error"` 的 finding → `1`；否则（含仅有 warning）→ `0`。
  命令本身执行失败（文件不存在、JSON 损坏、参数非法）仍走原有错误信封并退出 `1`。
  外层 `ok` 表示"命令执行成功"，内层 `result.ok` 才是 lint 结论。
- 不改变 `validate`/`query`/`preview`/`apply`/`undo`/`recover`/`build` 的任何行为。

## 5. 明确"还不是什么"（what this is NOT yet）

- **不是导航网格**：没有 navmesh、多边形分区、路径点、路径输出、路径长度、
  坡度代价或 agent 转向。它只回答"连不连通"。
- **不是 3D 可达性**：只对 XZ 平面做 BFS；`y` 只用于 out-of-world 的地面判定。
  没有多层楼、楼梯、坡道、桥、平台之间的垂直关系。低矮箱子和高墙在规则眼里一样挡路。
- **没有跳跃/高度推理**：不读取 `jumpSpeed`/`gravity`，没有最大跨距、最大台阶高度、
  落脚点或空中控制。0.3 高的台阶和 3 高的墙都按"不可走"处理。
- **没有动态状态**：door 在 BFS 中永远可通行，不模拟开/关/锁；key/goal 也没有触发半径、
  拾取条件或先后顺序，除了 `KEY-AFTER-DOOR` 这一条距离启发式。
- **没有 key→door 引用**：场景模型里 key 和 door 没有关联字段，所以
  `DOOR-WITHOUT-KEY` 只能判断"场景里有没有任意 key"，无法判断"这把钥匙能不能开这扇门"。
- **不是网格精确碰撞**：场景实体只有轴对齐 position/scale，没有旋转、层级、凸包或
  三角网格；overlap 与 blocking 都是 AABB 近似。
- **没有自动修复**：只报告，不改文档；没有 quick fix、没有批量 suppress/ignore 机制。
- **没有接编辑器/MCP**：MCP 工具目录保持不变；lint 目前只通过引擎函数与 CLI 使用。
  "改一个实体自动报错"需要后续在编辑器保存/预览路径里调用 `lintScene`，本工作包未接。
- **不是性能预算**：`NG-LINT-ENTITY-BUDGET` 只数实体，不统计三角形、材质、draw call、
  纹理内存或 GPU 时间；那些属于 R08/预算管理器。
