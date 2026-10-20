# 场景世界查询语言（A07 / INN-08）

- 状态：**纯函数查询引擎 + CLI**，不依赖渲染器、宿主、MCP 或 D3D12
- 代码：`engine/scene/query.mjs`（零 import、无 I/O、不修改输入）
- CLI：`node tools/scene.mjs query <scene.json> --query '<json>'` 或 `--file <query.json>`
- 测试：`tests/query.test.mjs`（18 项，`node --test tests/query.test.mjs`）
- 定位：让 Agent 用**声明式、可校验**的对象查询场景，例如
  "玩家 4 m 内的道具"、"出生点 5 秒步行半径内的东西"、"哪些材质被多个实体使用"。
  本工作包只落地查询语言；A07 标题里的"类型化动作 DSL"不在本包内（见 §6）。

## 1. API 与返回结构

```js
import { queryScene } from './engine/scene/query.mjs';

queryScene(document, query) -> {
  ok: true,
  rows: [...],                 // 行结构由 select 决定，见 §2.2
  count: 3,                    // rows.length（已应用 limit）
  aggregate: { value: 5 },     // 未请求时为 null；见 §2.6
  appliedQuery: {              // 归一化后的查询（默认值已补齐）
    select: 'entity',
    where: [{ field: 'kind', op: 'eq', value: 'wall' }],
    orderBy: null,
    limit: null,
    aggregate: null,
  },
}
```

- **纯函数**：不读文件、不写文件、不修改 `document` 或 `query`；`rows` 是深拷贝，
  调用方修改结果不会回写文档。模块零 import（`structuredClone` 是运行时全局）。
- **不做 schema 校验**：`validateScene` / `loadScene` 仍然是合法性的唯一入口。
  查询对畸形实体宽容：字段读不到就是 `null`，值谓词自然不匹配；`document.entities`
  不是数组则抛 `NG-E-QUERY-SHAPE`。
- **错误抛出 `QueryError`**（`name: 'QueryError'`），`code` 是稳定错误码，`details`
  给出允许值等结构化证据。CLI 捕获后输出既有错误信封并退出 1。
- **确定性**：同一文档 + 同一查询永远得到逐字节相同的 JSON；除非 `orderBy`，
  否则保持输入顺序；平局按输入顺序；`material` 分组按首次出现顺序。
- **与旧查询共存**：`engine/scene/index.mjs` 的 `queryScene(scene, {kind, entityId, name})`
  未改动，旧 CLI 形式也未改动；新语言由 `--query` / `--file` 显式触发。

## 2. 查询语法

### 2.1 顶层对象

```json
{
  "select": "entity | material | light",
  "where": [{ "field": "x", "op": "within", "value": { "x": 0, "y": 0, "z": 6, "radius": 4 } }],
  "orderBy": { "field": "z", "direction": "asc" },
  "limit": 3,
  "aggregate": "count"
}
```

| 键 | 必填 | 默认 | 说明 |
| --- | --- | --- | --- |
| `select` | 否 | `entity` | 行类型：实体 / 材质分组 / 灯光 |
| `where` | 否 | `[]` | 谓词数组，**AND** 连接；空数组表示不过滤 |
| `orderBy` | 否 | 无（输入顺序） | `{field, direction}`；`direction` 可省略，默认 `asc` |
| `limit` | 否 | 无 | 非负安全整数；只截断返回行 |
| `aggregate` | 否 | 无 | `count` / `sum:<field>` / `min:<field>` / `max:<field>` |

未知顶层键、未知 `select`、类型错误 → `NG-E-QUERY-SHAPE`。
`query` 本身必须是普通对象（`null`、数组、字符串都拒绝）。

### 2.2 `select`：返回什么行

| 取值 | 行结构 | 语义 |
| --- | --- | --- |
| `entity`（默认） | 实体的**完整深拷贝**，字段与场景文档一致 | `where` 命中的所有实体 |
| `light` | 实体的完整深拷贝 | 先按 `where` 过滤，再只保留 `kind === "light"` 的实体；`where` 里的 `lightIntensity` 可直接用 |
| `material` | `{ material, count, entityIds }` | 对 `where` 命中的实体按 `material.pattern` 分组。`material` 是 pattern 字符串，`count` 是使用该 pattern 的实体数，`entityIds` 按输入顺序。没有 `material` 的实体不产生分组；**默认按首次出现顺序**，可用 `orderBy` 改排 |

`select: "material"` 的 `orderBy` 只接受 `material` 和 `count`；其他字段名 →
`NG-E-QUERY-FIELD`。`select: "material"` 的 `aggregate` 只支持 `count`。

> 注意：材质分组的键是 `material.pattern`（`checker` / `brick` / `metal`），
> 不是完整的 albedo/roughness 对象。两个 pattern 相同但颜色不同的材质会被合并。

### 2.3 字段（`where` / `orderBy` / `aggregate`）

字段是实体模型的**扁平投影**；空值统一是 `null`。

| 字段 | 来源 | 类型 | 备注 |
| --- | --- | --- | --- |
| `kind` | `entity.kind` | string | 8 种实体类型之一 |
| `id` | `entity.id` | string | UUID；`eq` 是精确匹配（区分大小写），不像旧查询那样忽略大小写 |
| `name` | `entity.name` | string | 显示名 |
| `x` / `y` / `z` | `entity.position[0..2]` | number | 世界坐标 |
| `scaleX` / `scaleY` / `scaleZ` | `entity.scale[0..2]` | number | 尺寸 |
| `collider` | `entity.collider` | boolean | |
| `material` | `entity.material.pattern` | string \| null | 例：`"brick"`；无材质为 `null` |
| `lightIntensity` | `entity.light.intensity` | number \| null | 仅 light 实体有值 |

### 2.4 谓词（`where` 数组元素）

每个谓词必须是**恰好** `{field, op, value}` 三个键；多键/少键 → `NG-E-QUERY-SHAPE`。

| `op` | `value` 类型 | 语义 | 空值 / 类型不匹配时 |
| --- | --- | --- | --- |
| `eq` | string / number / boolean / null | 严格相等（`===`，无类型转换） | 不匹配 |
| `ne` | 同上 | 严格不等 | 匹配：`null !== 2` 为真，故无该字段的行也会命中 |
| `lt` / `lte` / `gt` / `gte` | 有限 number | 数值比较 | 字段为 `null` 或非 number 时谓词为 `false` |
| `in` | 标量数组（可空） | 字段值等于数组中任意一项（严格相等） | 空数组合法，匹配 0 行 |
| `contains` | string | 字符串子串，**大小写不敏感** | 字段非 string 时为 `false` |
| `within` | 恰好 `{x, y, z, radius}` | 实体 `(x,z)` 到 `(value.x,value.z)` 的 **XZ 平面欧氏距离 ≤ radius**；边界包含；`y` 被忽略 | 实体位置不完整时为 `false` |

`within` 的细节：

- `field` 仍必须是 §2.3 的合法字段（防止打错字），但**不参与计算**；距离始终用实体位置。
  因此 `field: "x"` 是惯例写法。
- `value` 的四个键必填、必须是有限 number，`radius >= 0`；多键/少键/负数 →
  `NG-E-QUERY-SHAPE`。
- 它是**半径查询，不是路径/可达性查询**：不管中间有没有墙。

谓词之间只有 AND，没有 OR / NOT / 嵌套。所有谓词在实体上求值后再分组/投影。

### 2.5 排序与截断

- `orderBy.field` 必须在当前 `select` 的字段表内（实体/灯光用 §2.3；材质用
  `material` / `count`），否则 `NG-E-QUERY-FIELD`。
- `direction` 只能是 `asc` / `desc`，省略为 `asc`；其他值 → `NG-E-QUERY-SHAPE`。
- 稳定排序：值相同按输入顺序（材质分组按首次出现顺序），`desc` 也不翻转平局。
- `null` / 缺失值**在升序和降序都排在最后**。
- `limit` 是 0 或正整数（安全整数）；负数、小数、字符串、`null` → `NG-E-QUERY-SHAPE`。
  `limit` 只影响 `rows` / `count`，不影响 `aggregate`。

### 2.6 聚合

| `aggregate` | 结果 |
| --- | --- |
| `count` | `{ value: 行数 }` |
| `sum:x` | 对行的 `x` 数值求和；无有效值时为 `0` |
| `min:x` | 最小值；无有效值时为 `null` |
| `max:x` | 最大值；无有效值时为 `null` |

- `<field>` 必须是 §2.3 的字段且是**数值字段**：`x`、`y`、`z`、`scaleX`、`scaleY`、
  `scaleZ`、`lightIntensity`。非数值字段（如 `name`）→ `NG-E-QUERY-SHAPE`；
  未知字段 → `NG-E-QUERY-FIELD`。
- 值为 `null` 的行在 sum/min/max 中被跳过（例如非 light 实体的 `lightIntensity`）。
- **聚合在 `limit` 之前**：`limit` 是展示层截断，`aggregate` 汇总所有命中行。
  `count`（返回行数）与 `aggregate.value` 在带 `limit` 时可能不同，这是有意设计。

### 2.7 求值顺序

1. 校验并归一化 `query`（补默认值）；
2. 按 `where` 的 AND 谓词过滤实体；
3. 按 `select` 投影：实体拷贝 / 仅灯光 / 材质分组；
4. `orderBy` 排序；
5. 对排序后的行计算 `aggregate`；
6. 应用 `limit`，返回 `rows`、`count`、`aggregate`、`appliedQuery`。

## 3. 错误码

| 错误码 | 何时出现 | 例子 |
| --- | --- | --- |
| `NG-E-QUERY-FIELD` | `where.field`、`orderBy.field` 或 `aggregate` 的字段未知，或对该 `select` 不可用 | `{"where":[{"field":"colour","op":"eq","value":"red"}]}`；`select:"material"` + `orderBy.field:"x"` |
| `NG-E-QUERY-OP` | `where.op` 不在 9 个受支持操作内 | `{"where":[{"field":"name","op":"like","value":"x%"}]}` |
| `NG-E-QUERY-SHAPE` | 查询/谓词/值/`select`/`limit`/`aggregate` 的结构或类型不合法；也用于文档没有 `entities` 数组 | 未知顶层键、`where` 不是数组、`within` 缺 `radius`、`limit:-1`、`aggregate:"avg:x"` |

错误对象：

```json
{
  "ok": false,
  "error": {
    "code": "NG-E-QUERY-FIELD",
    "message": "Unknown query field: colour",
    "details": [{ "index": 0, "field": "colour", "allowed": ["kind", "id", "..."] }]
  }
}
```

CLI 层还有两类既有错误，与查询语义无关：

| 错误码 | 原因 |
| --- | --- |
| `INVALID_JSON` | `--query` 不是合法 JSON，或 `--file` 指向的文件不是合法 JSON / 带 BOM |
| `INVALID_ARGUMENT` | 同时给了 `--query` 和 `--file`、选项缺值、未知选项 |

退出码：查询成功 `0`；任何查询/参数/IO 错误 `1`（外层信封 `ok: false`）。
外层 `ok` 表示命令是否执行成功，内层 `result.ok` 是查询结果（成功恒为 `true`）。

## 4. 小院（`assets/courtyard.scene.json`）实测

小院共 17 个实体：1 ground、5 wall、1 player、5 prop、1 key、1 door、1 goal、2 light。

以下命令使用 POSIX / Git Bash 的单引号写法；Windows PowerShell 5.1 下 `--query` 的引号问题见 §5 末尾。

### 4.1 所有墙：5 个

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"where":[{"field":"kind","op":"eq","value":"wall"}]}'
```

```json
{
  "ok": true,
  "result": {
    "ok": true,
    "rows": ["…5 个 wall 实体深拷贝…"],
    "count": 5,
    "aggregate": null,
    "appliedQuery": {
      "select": "entity",
      "where": [{ "field": "kind", "op": "eq", "value": "wall" }],
      "orderBy": null,
      "limit": null,
      "aggregate": null
    }
  }
}
```

| name | id |
| --- | --- |
| West wall | `a1e00000-0000-4000-8000-000000000011` |
| East wall | `a1e00000-0000-4000-8000-000000000012` |
| South wall | `a1e00000-0000-4000-8000-000000000013` |
| Exit wall left | `a1e00000-0000-4000-8000-000000000014` |
| Exit wall right | `a1e00000-0000-4000-8000-000000000015` |

### 4.2 指定点 4 m 内 + 按 z 排序 + 前 3 条

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"where":[{"field":"x","op":"within","value":{"x":0,"y":0,"z":5,"radius":4}}],"orderBy":{"field":"z","direction":"asc"},"limit":3}'
```

`count = 1`，唯一命中的是玩家出生点（`a1e00000-0000-4000-8000-000000000020`，
position `[0, 0, 6]`，到 `(0, 5)` 的 XZ 距离为 1）。小院里的道具都在 4 m 之外，
所以这个点没有道具。`y` 不参与距离，`orderBy` 和 `limit` 对 1 行是恒等操作。

### 4.3 玩家出生点 6.5 m 内的道具：2 个

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"where":[{"field":"kind","op":"eq","value":"prop"},{"field":"x","op":"within","value":{"x":0,"y":0,"z":6,"radius":6.5}}],"orderBy":{"field":"z","direction":"asc"}}'
```

| name | id | position | XZ 距离 |
| --- | --- | --- | --- |
| Crate | `a1e00000-0000-4000-8000-000000000030` | `[-4, 0.6, 2]` | 5.66 |
| Low platform | `a1e00000-0000-4000-8000-000000000034` | `[-6, 0.3, 6]` | 6.00 |

`Column`（`[7, 1.25, 5]`）距离 7.07，被 6.5 的半径排除。查询语言本身不能按
"距离"排序（没有计算字段），这里的顺序是 `orderBy z`；距离由调用方或测试计算。

### 4.4 哪些材质被多个实体使用：3 种都 > 1

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"select":"material","orderBy":{"field":"count","direction":"desc"}}'
```

```json
{
  "ok": true,
  "result": {
    "rows": [
      { "material": "brick", "count": 8, "entityIds": ["…8 个…"] },
      { "material": "metal", "count": 6, "entityIds": ["…6 个…"] },
      { "material": "checker", "count": 3, "entityIds": ["…3 个…"] }
    ],
    "count": 3,
    "aggregate": null,
    "appliedQuery": {
      "select": "material",
      "where": [],
      "orderBy": { "field": "count", "direction": "desc" },
      "limit": null,
      "aggregate": null
    }
  }
}
```

- brick：floor + 5 面墙 + Stone bench + Column = 8；
- metal：player + key + door + goal + 2 盏灯 = 6；
- checker：Crate + Planter + Low platform = 3。

三种材质都 `count > 1`。"按分组后的 count 过滤"（HAVING 语义）尚未支持，见 §6。

### 4.5 灯光强度求和

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"select":"light","aggregate":"sum:lightIntensity"}'
```

`rows` 是 2 盏灯（Courtyard lamp 5.5、Gate lamp 4.2），`count = 2`，
`aggregate = {"value": 9.7}`。

### 4.6 "5 秒内能到的东西" ≈ 15 m 半径

查询语言没有时间/路径概念。若按步行速度 3 m/s 直算，`3 × 5 = 15 m`：

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"where":[{"field":"x","op":"within","value":{"x":0,"y":0,"z":6,"radius":15}}]}'
```

`count = 15`：17 个实体中有 15 个落在半径内（包括 Finish marker，距离恰好 15，
边界包含）；只有两段 Exit wall 被排除（距离 `sqrt(5.5² + 14²) = 15.04`）。这只是一个
**直线 XZ 半径**，不等于"绕过障碍 5 秒内可达"；门/钥匙/跳跃都不建模。

### 4.7 总数

```bash
node tools/scene.mjs query assets/courtyard.scene.json --query '{"aggregate":"count"}'
```

返回 17 个实体，`count = 17`，`aggregate = {"value": 17}`。

## 5. CLI

```text
node tools/scene.mjs query <scene.json> --query '<json>'
node tools/scene.mjs query <scene.json> --file <query.json>
```

- 成功：`{ "ok": true, "result": { "ok": true, "rows": [...], "count": N, "aggregate": ..., "appliedQuery": ... } }`。
- 查询错误：`{ "ok": false, "error": { "code": "NG-E-QUERY-*", ... } }`，退出码 `1`。
- `--query` 与 `--file` 只能二选一；`--file` 复用现有 JSON 读取器（16 MiB 上限）。
- **旧子命令不变**：不带 `--query`/`--file` 时仍走原有
  `query <scene> [--kind] [--name] [--id]` 分支，输出结构不变。
  两者靠参数位置区分：选项位置上出现 `--query` 或 `--file` 才进入新语言。
- Windows PowerShell 5.1 的坑：把 `'{"where":...}'` 传给原生 exe 时，嵌入的 `"` 会被
  参数传递层吞掉，导致 `INVALID_JSON`。可选任一方式：
  - 用 `--file`（推荐，脚本/CI 最稳）；
  - 在 Git Bash / WSL / bash 里用上面的单引号形式；
  - 在 PowerShell 里用 `cmd /c "node ... --query \"{...}\""` 显式转义。

## 6. 明确"还不是什么"（what this is NOT yet）

- **没有 join**：不能从实体跳到材质定义、灯光、资产或其它实体；行是自包含的，
  实体引用只有 `id` 字符串。材质分组是内置投影，不是关系代数里的 JOIN。
- **没有路径查询**：没有图遍历、最短路、导航网格、可达性、门/钥匙状态或移动代价。
  `within` 只是 XZ 欧氏半径；"5 秒可达"必须由调用方用速度换算半径，且不保证绕障可达。
  真正的连通性判断仍在 `engine/scene/lint.mjs` 的 BFS 里。
- **没有 GROUP BY / HAVING**：`select: "material"` 是唯一的固定分组，不能按任意字段
  分组，也不能在查询里过滤 `count > 1`（需要调用方对返回行再过滤，或后续工作包加
  `having`）。**材质聚合只有 count**：没有 `sum:roughness`、`avg:metalness` 这类聚合；
  对 `select: "material"` 使用 `sum/min/max` 会得到 `NG-E-QUERY-SHAPE`。
- **没有空间索引**：每次查询对实体做**线性扫描**，每个谓词 O(n)，排序 O(n log n)；
  没有网格、BVH、R-tree 或 broadphase。当前 10k 实体上限下可用，但不是实时查询引擎。
- **没有布尔组合**：只有 AND；没有 OR、NOT、括号嵌套、子查询。
- **没有计算字段/表达式**：不能按距离排序、不能算 `distance / walkSpeed`、没有算术、
  字符串函数、正则；不能给字段起别名。
- **`contains` 固定大小写不敏感**：没有区分大小写的开关。
- **没有分页/流式**：只有 `limit`，没有 `offset`、游标、超时或取消。
- **不修改世界**：只读查询；没有 create/set/remove、没有事务、没有 undo，
  也没有"类型化动作 DSL"（A07 的另一半，本包未做）。
- **不接 MCP / 编辑器 / 打包**：没有新增 MCP 工具，没有改 `package.json`、
  `tools/verify.ps1`、CI 或渲染器；查询只在引擎函数和 `tools/scene.mjs` 里可用。
- **不做 schema 校验**：调用 API 前应自行 `validateScene`（CLI 走 `loadScene`，已校验）。
- **材质分组粒度是 pattern**：同 pattern、不同 albedo/roughness 的材质会合并；
  没有材质 id/句柄，也没有跨文档的材质表。
- **没有 3D 距离**：`within` 忽略 `y`；没有垂直距离、体积查询、朝向/锥体查询。

## 7. 相关文件

- `engine/scene/query.mjs`：查询引擎（本工作包新增）
- `tests/query.test.mjs`：18 项测试（本工作包新增）
- `tools/scene.mjs`：新增 `--query` / `--file` 分支；旧分支未改
- `assets/courtyard.scene.json`：本文实测所用场景
- `docs/SCENE-LINT.md`：可达性/连通性 lint，与本文的半径查询互补
- `engine/scene/index.mjs`：旧 `queryScene` 与场景 schema/事务，未改
