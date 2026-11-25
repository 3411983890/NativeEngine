# 植被系统扩展实施记录：部件层、排布规则与笔刷合同

- 日期：2026-10-28。
- 状态：**代码实施记录**。本文描述已落地的实现与已验证的性质；未实现项逐条列出，不冒充完成。
- 对应设计：[植被系统扩展开发文档](VEGETATION-SYSTEM-EXTENSION-DEVELOPMENT-2026-10-28.md)。
- 范围：仅 JS 侧生成、排布与笔刷数据层。**未改动原生渲染、材质 ABI、编辑器 UI 与地图服务**，因此不能据此宣称 P2/P5 已交付。

## 1. 本次落地内容

| 文件 | 变化 | 对应设计要求 |
| --- | --- | --- |
| `engine/vegetation/components.mjs` | 新增，784 行 | §3.1 部件层；V02、V03、V04（部分）、V05（部分）、V08（浮叶部分） |
| `engine/vegetation/stroke.mjs` | 新增，166 行 | §4.2 笔刷输入采样与覆盖组合 |
| `engine/vegetation/scatter.mjs` | 重写，145 → 481 行 | §4.3 自动排布、§4.4 表面与生态约束（排除区部分） |
| `engine/vegetation/region.mjs` | 修改 | §5 资源身份、拒绝计数、稳定 ID |
| `engine/vegetation/species.mjs` | 修改 | §3.1 部件接入 cook、§3.2 兼容旧物种 |
| `tests/vegetation.test.mjs` | 29 → 46 个用例 | §9 验收用例 1、2、4、5、7 的 JS 侧 |

## 2. 部件层（`components.mjs`）

### 2.1 已实现的部件类型

| 类型 | 几何 | 典型清单项 |
| --- | --- | --- |
| `fruit` | 低面数椭球（极点单顶点，无退化三角形） | 柿、枇杷、枸杞、南天竹 |
| `flower` | 花心扇面 + 花瓣三角对，`cup` 控制碗状 | 桃花期、油菜花期 |
| `seedHead` | 穗轴三棱柱 + 小穗；`form: spike` 直穗、`panicle` 侧枝穗 | 狗尾草、芒草、芦苇、水稻、香蒲 |
| `pod` | 四棱透镜，两端收成单顶点 | 大豆、豆科 |
| `compoundLeaf` | 叶轴 + 对生小叶 + 顶生小叶 | 苦楝、枫杨、水芹、蕨类（部分） |
| `floatingLeaf` | 带缺刻的浮叶圆盘，落在声明的水平面 | 浮萍、芡实、荷（浮叶阶段） |
| `culm` | 竹节环带（沿宿主枝条的三段环） | 毛竹、杂竹 |
| `imported` | 外部网格实例适配，由调用方提供 `positions/indices` | 包心白菜、英雄枯树 |

设计文档中列出但本层未实现的类型，**按名字拒绝并给出替代路径**，不接受后静默丢弃：

```
leaf / frond        → 用 generators.fronds
stem / branch       → 用 generators.levels
root                → 未实现，无 emitter
surfacePatch        → 未实现，苔藓/藻类需要表面贴片通道而非实例化部件
```

### 2.2 挂载与随机流

- 分布：`along`（弧长均匀）、`tip`（末端簇生）、`node`（按米制节距，竹节/稻穗节点）。
- 叶序：`alternate` / `opposite` / `whorled`（含 `whorlCount`）/ `spiral`（黄金角）。
- 方向：外向径向 × `spreadRadians` + 宿主切向 + `upBias - droop`，向下垂果与向上花共用同一段代码。
- 随机流按 `(plant seed, component id, host id)` 派生，**同一部件的第 n 个挂点与其它部件无关**。测试直接验证：把果实数量从 1 改成 7，叶数与树冠实测半径不变，且 LOD0 顶点前缀逐字节相同。
- 浮叶的 `attach.planeHeightMetres` 把部件钉在声明的水平面上，这是"浮叶对水面"的可表达形式；水深与岸线距离仍未接入（见 §6）。

### 2.3 阶段与 LOD

- `components[].stages` 是过滤器：物种 `stage` 不在列表内则整件跳过，并在 cook 元数据里记 `skippedStage: true`，不静默消失。
- `components[].lodLevels` 可按 LOD 排除整件；`QUALITY` 新增 `components` 存活比例 `[1, 0.7, 0.35, 0.15]`，与 `leaves` 分开，避免树先掉果后掉叶。
- 存活用每部件独立 `keep` 排名，粗 LOD 是细 LOD 的**严格子集**且位置不变（有测试）。

### 2.4 cook 元数据

仅在物种声明了 `components` 时出现，因此**没有部件的旧物种元数据与几何都不变**（有测试对比三角形与顶点数）：

```json
"structure": {
  "components": {
    "fruit_1": { "type": "fruit", "parts": 118, "hosts": 36, "triangles": 4248,
                 "skippedStage": false, "skippedLod": false }
  },
  "stage": null
}
```

## 3. 排布规则（`scatter.mjs`）

### 3.1 候选身份

`candidateKey({layerId, species, cellX, cellZ, slot})` 在**任何过滤之前**赋值。`region.mjs` 用 `hashString(candidateKey)` 得到打包用的 `stableId`，并检测碰撞：两株植物哈希相同会以 `NG-E-VEG-ID` 失败，而不是合并成一个身份。

测试验证：加一个圆形排除区后，存活植株的 `candidateId`、坐标与朝向与未裁剪时完全一致；区域 cook 的 `stableId` 集合是原集合的子集。

### 3.2 密度遮罩

`CoverageMask`：双线性采样的 0..1 栅格，`stampCircle` 写软边圆盘。规则通过 `mask: { id, strength, invert }` 引用；`strength: 0` 表示该层忽略遮罩（与"遮罩为空"是两件事）。引用了未提供的遮罩会失败，不会退化成"整片都种"。

### 3.3 排除区

`circle` / `rect` / `polyline`（道路）三类，各带 `marginMetres`。规则可用 `avoidExclusions: false` 退出。测试验证道路两侧 `半宽 + 退让` 内无植株。

### 3.4 行植（农田）

`rows: { spacingMetres, plantSpacingMetres, headingRadians, skipFraction, ridgeInsetFraction }`：

- 格点锚定在**区域原点**并按区域种子取相位，不是每格重置，因此行跨格连续、不重不漏（测试验证跨 `z=8/16` 边界的行距仍为 2 m、行内株距仍为 1 m）。
- `skipFraction` 是确定性缺株，重烤保留同样空洞。
- `ridgeInsetFraction` 是田埂退让：行带两侧留出不种植宽度。

### 3.5 跨格、跨物种间距

`spacingScope: 'region'` 触发 `resolveSpacing`：按 `(规则优先级, cellZ, cellX, slot)` 排序，逐候选检查已接受邻居，半径取 `max(自身间距, 邻居间距)`。

局部性是结构性保证而非近似：接受与否只取决于半径内优先级更高的候选，因此"受影响分块 + `maxSpacing` halo"重算与全局结果一致。测试用 `scatterCell` + halo 重建单格，实例数与全局结果相同。

默认仍是 `cell`，旧区域 cook 结果不变（原"每格独立"测试仍通过）。

### 3.6 拒绝计数

`scatterRegion` 返回 `rejectedBySurface / rejectedByMask / rejectedByExclusion / rejectedBySpacing`，并写入区域 `meta.counts.rejected`。稀疏不再是唯一诊断。

## 4. 笔刷合同（`stroke.mjs`）

| 要求（§4.2） | 实现 |
| --- | --- |
| 按世界距离重采样，不按事件数累加 | `resamplePath` 用累积弧长 + 1 µm 量化；同一路径 2 个采样点与 201 个采样点得到**逐值相同**的遮罩（有测试） |
| 绘制为目标覆盖度，不无限叠加 | `mode: 'paint'` 取 `max`，同处重复绘制不提高覆盖度 |
| 加密是显式操作 | `mode: 'densify'` 是唯一累加模式 |
| 擦除持久化 | `mode: 'erase'` 写 `min(current, 1 - weight)`，中心为 0 并保留 0 |
| 只重建受影响分块 | `strokeBounds` + `strokeCells` 给出脏块集合；测试验证脏块外遮罩未变 |

## 5. 验证

本次只跑与改动范围相关的增量入口，未执行全量测试、`npm test`、`npm run verify` 或完整 CI。

| 命令 | 结果 |
| --- | --- |
| `node --test tests/vegetation.test.mjs` | 46 通过 / 0 失败（原 29 + 新增 17） |
| `node --test tests/vegetation-editor.test.mjs tests/vegetation-leaf-source.test.mjs` | 17 通过 / 0 失败（三文件合计 63） |
| `node tools/vegetation-fixtures.mjs <临时目录>` | 4 物种 + 1 区域 cook 成功 |

未验证：原生绘制、GPU 绑定、编辑器 UI、发布运行。本文不据此宣称任何原生侧结论。

## 6. 明确未实现

按设计文档的编号逐条列出，避免把能力矩阵读成已完成：

- **V01 物种档案**：`species-profile.mjs` 仍只有 broadleaf/conifer/shrub/grass 四种冠形；垂柳式下垂冠需要新的 profile 或更强的 `droop` 组合，未做。
- **V04 竹类**：只有 `culm` 节环部件；竹秆的分节间几何、节处分枝、丛生弯曲未做，需要新的生成路径。
- **V06 莲座/包心**：未做；包心白菜只能走 `imported` 部件。
- **V07 藤蔓**：未做；无引导线、无表面攀附。
- **V08 水生**：只有浮叶的声明水平面；底床高度、水深、岸线距离查询未接入，`surface.mjs` 仍只做竖直射线。
- **V09 苔藓/藻类**：`surfacePatch` 明确拒绝，未做。
- **V10 生长阶段**：只有静态 `stage` 过滤；连续生长、落叶、果实脱落未做。
- **V11/V12 地图闭环**：无资源库注册、无 `vegetation.registerToWorld` / `preview` / `commit` 服务、无原生笔刷轮廓、无分块增量编译、无实例 LOD 绘制。`stroke.mjs` 只提供笔迹数据，不提供事务与撤销。
- **原生侧**：未新增材质 kind（果实/花复用现有 bark/leaf kind），未新增部件批次，未改 ABI。因此果实当前走不透明 bark 着色路径，不能宣称材质合同已扩展。
- **物种内容**：未新增任何物种资产。设计文档要求"不能把有贴图标成有完整物种"，在缺少对应贴图与模型前不产出柿树/垂柳等档案。
- **编辑器 UI**：`components` 与 `stage` 可经 `vegetation.load/save` 原样往返，但没有编辑面板。

## 7. 已知限制

1. `stableId` 是 32 位哈希。区域 cook 会检测碰撞并以 `NG-E-VEG-ID` 失败；3000 万实例级别需要更宽的 ID（记录里 `reserved` u32 尚未启用）。
2. `resolveSpacing` 为 O(n log n) + 空间哈希；halo 重建的正确性依赖调用方提供 `maxSpacing` 范围内的全部已接受实例。
3. `CoverageMask` 目前只在内存中，尚未写入区域包；区域 JSON 内联栅格仅适合小区域。
4. 行植格点用 `row * 100003 + plant` 作 slot，负坐标区域仍唯一，但极端跨度下需复核整数范围。
