# 发型生成器 AI 使用手册

版本：2026-10-28，对应 `nativegpu.hairstyle/1`、`nativegpu.hair-cook/1`、`nativegpu.hair-gpu-pack/1`、`nativegpu.hair-gpu-pack-file/1`（磁盘魔数 `HGP1`）。本手册的每个数字都能用第 12 节的命令复现。

## 0. 先读这一段

**已经能用**：参考图登记与多视图融合、结构化发型规格、真实头皮三角形绑定、guide 生成与渲染发丝插值、局部 patch 与区域锁定、cook 内容寻址包、绑定/穿透/LOD/预算校验、**GPU 上传契约（固定步长、切线、宽度、根尖属性）**、**磁盘包 `HGP1` 与原生严格读取**、**CPU 参考渲染（覆盖率、透明层溢出、自阴影、背光透射）**、**D3D12 发丝绘制（覆盖率修正 ribbon、纤维着色、深遮罩自阴影、背光、调试通道、发际线近景，离屏真帧验证）**、**宿主 `--groom` 场景帧（与角色共用 HDR 目标、深度与环境光，同相机有/无发型像素差证据）**、**CPU guide 模拟（固定 tick、根固定、头球碰撞、风、逐帧插值上传；拉伸 1.000016、穿透 0、根漂移 0）**、**后到前深度排序透明（order 索引缓冲，排序相对上传顺序改变 20,119 个离屏像素）**、**发根绑定头部骨骼（head skin matrix；25° 转头改变 6,437 个宿主像素 / 40,787 个离屏像素）**、原生兼容 groom 导出。

**还不能用**（不要在报告里写成已完成）：

| 缺口 | 现状 |
| --- | --- |
| 走跑/跳跃动作序列 | 头部骨骼绑定与 25° 转头已实现（头网格与发丝共用同一 skin matrix）；但没有走跑、跳跃、急转下的发丝运动与闪烁序列 |
| 顺序无关/有界透明缓存 | 没有。已实现按根点视深的后到前排序（`stats.transparency === 'depth-sorted-alpha'`），但交叠发丝仍可能按错误次序合成，也没有逐像素有界缓存与溢出报告 |
| 毛发向场景投影 | 没有。深遮罩只让发丝自阴影，不投到身体或场景 |
| GPU guide 模拟 | 不存在。CPU 求解器（`renderer/hair_draw.cpp` 内的 guide 状态）有测试；GPU 版本没有 |
| 闪烁/时序/显存 | 未测。`hair_validate` 的 `unmeasured` 列表逐条列出，GPU/宿主证据 JSON 也把 `gpuGuideSolve`、`orderIndependentTransparency`、`sceneShadowCast` 标为 `false` |

因此：**“发丝绘制与 guide 模拟完成”可以说“D3D12 通道在离屏与宿主场景帧中验证，静止/风下求解稳定”，但不能说“动画头部、OIT 与硬件稳定性已验收”**。镜头运动闪烁、OIT、场景投影、GPU guide、显存与 GPU 耗时仍无证据。

## 1. 工具发现

调用顺序永远从能力查询开始，不要假设版本：

| 工具 | 关键输入 | 输出要点 |
| --- | --- | --- |
| `hair_capabilities` | `projectId` | 已注册插件、版本、schema、支持的姿态/通道、明确的能力缺口 |
| `hair_reference_register` | `path`、`view`、`authorized: true` | 图片哈希、资源 ID；**不做视觉识别** |
| `hair_analysis_reconcile` | `references`、`analyses` | 冲突字段、推断字段（`inferred`）与置信度 |
| `hair_create` | `id`、`characterAsset`、`regions` 或 `spec`、`seed`、`dryRun` | revision、guide、绑定诊断、推断区域 |
| `hair_patch` | `id`、`operations`、`expectedRevision`、`dryRun` | 差异、被重生成的区域、新 revision |
| `hair_capture` | `id`、`views`、`channels`、`lighting` | 每张图的相机/姿态/revision/资产哈希元数据 |
| `hair_validate` | `id`、`averageCoverage`、`layersPerTile`、`resolution` | 绑定漂移、guide 穿透、LOD 身份、预算、透明层需求、**未测项** |
| `hair_export` | `id`、`includeCompatibleGroom` | 发布包；兼容 groom 附带三条已知损失 |

`authorized` 必须显式为 `true`：引擎不会自己上传项目或图片去调用云端视觉模型。分析由**你**（调用方 AI）完成，引擎只接收结构化结果。

## 2. 单位、坐标与方向

- 长度单位一律**米**。`lengthMetres`、`widthMetres`、`radiusMetres`、`offsetMetres`、`maxDistanceMetres` 都是米，不是厘米。
- 坐标系与角色一致：+Y 向上，+Z 为角色正面，`head` 骨骼为头皮根。相机 yaw 0 在角色正前方（+Z）。
- 左右按**角色自身**命名：`left_side` 是角色左半边，不是画面左边。镜像操作必须显式给出左右骨骼映射，不能按坐标取负。
- 发丝宽度是**直径**：`strands.widthMetres = 0.00007` 表示 70 µm 直径；GPU 包里的 `widthMetres` 属性是直径，误当半径会用一半厚度绘制且不报错。

## 3. 参数与取值范围

发型源是一个 `nativegpu.hairstyle/1` 文档，`hair_create` 接受整份 `spec` 或由区域参数拼装。下表是可安全使用的范围（越界会被 `normalizeHairstyle` 拒绝）：

| 字段 | 范围 | 说明 |
| --- | --- | --- |
| `regions[].densityPerCm2` | 5–400 | 每平方厘米 guide 数，直接决定生成时间 |
| `regions[].lengthMetres` | 0.01–0.6 | 发长；0.6 m 已到肩胛以下 |
| `regions[].comb` | 单位向量 | 梳理方向；不要传零向量 |
| `regions[].parting.widthDegrees` | 0–40 | 分缝留白宽度 |
| `guides.segments` | 4–32 | 每根 guide 的段数；渲染发丝与之相同 |
| `strands.streamsPerGuide` | 1–64 | 每 guide 插值出的渲染发丝数 |
| `strands.widthMetres` | 2e-5–1e-3 | 单根发丝直径 |
| `strands.rootJitterMetres` | 0–0.01 | 发根散布，超过 5 mm 会露出头皮 |
| `lod[].strandFraction` | 0.05–1 | 逐级递减，`lod[0]` 必须为 1 |
| `lod[].maxDistanceMetres` | 递增 | LOD 切换距离 |
| `budget.maxStrands` | ≥ 实际发丝数 | 超预算会被 cook 拒绝，不静默截断 |

默认值（沿用 `hairStyleSpec` 夹具）：5 个区域、`densityPerCm2` 30–34、`lengthMetres` 0.115–0.17、`segments` 8、`streamsPerGuide` 8、`widthMetres` 70 µm。

## 4. 标准工作流

```
1. hair_capabilities                      → 确认版本与缺口
2. hair_reference_register (每张图)        → 图片哈希 + 视角 + 授权
3. hair_analysis_reconcile                → 冲突/推断字段
4. hair_create (dryRun=true → false)      → revision + 绑定诊断
5. hair_validate                          → 绑定/穿透/LOD/预算/未测项
6. hair_capture                           → 多视角截图计划
7. hair_patch (expectedRevision)          → 局部修改 → 回到 5
8. hair_export                            → 发布 cooked 包
```

第 4 步的 `dryRun` 不是可选项：它让你在写盘之前看到将生成多少 guide、绑定有多少根落在头皮外、哪些区域会被推断。

## 5. 读懂校验报告

`hair_validate` 返回 `passes`、`failures`、逐姿态明细和 `unmeasured`。判读顺序：

1. `rootsOffSurface` 必须为 0。非 0 表示发根没有落在头皮三角形上，属于绑定错误，不是抖动。
2. `maxSurfaceErrorMetres` 是姿态变化后发根离开头皮的最大距离。本仓库实测：rest 0.00013 mm、`head_turn` 0.162 mm、`head_tilt` 0.109 mm。超过 1 mm 说明绑定用了错误的重心坐标。
3. `penetration.p99Metres` 是 guide 钻进身体代理的深度。**注意 `enforced` 与 `requiresSolver`**：`enforced: false` 表示这一姿态需要 guide 求解器，而静态几何校验只能报告、不能修正。实测 `neck_bend` 9.4 mm、`shoulder_raise` 18.7 mm 都属于这一类——它们不是“通过”，是“待求解”。
4. `lod` 表里的 `maxRootDriftMetres` 必须是 0，`sharedIdentity` 必须是 `true`。`naiveIndexDrift` 是按索引直接复用 LOD 会产生的漂移（实测 0.124 m），它证明为什么不能那样做。
5. `unmeasured` 一律原样写进你的报告，不要删。

## 6. 截图与通道

`hair_capture` 生成的是**计划**：每个条目带相机参数、姿态、revision、资产哈希、随机种子、分辨率、曝光、灯光。可用视角：`front`、`back`、`left`、`right`、`three_quarter_left`、`three_quarter_right`、`hairline_closeup`、`top`。通道：`beauty`、`silhouette`、`region_id`、`guide`、`root_binding`、`tangent`、`coverage`、`collision`、`lod`、`depth`、`normal`。

读图规则：

- 只看 `beauty` 无法判断根绑定；必须同时有 `coverage`（亚像素发丝是否被丢掉）和 `root_binding`。
- 选区映射必须带截图资产哈希与 revision，过期截图会被拒绝（`resolveSelection` 校验）。
- 截图是**定位问题**用的，修改发生在三维资产上，然后重新截图。不要修饰截图冒充结果。
- D3D12 绘制通道的调试视图与截图计划是两套名字，不要混用。`HairFrameInput::debugView` 支持：`shaded`(0)、`coverage`(1)、`tangent`(2)、`roottotip`(3)、`strandid`(4)、`material`(5)、`depth`(6)、`shadow`(7)。`test:hair:gpu` 会输出 `debug-coverage.png`、`debug-tangent.png`、`debug-roottip.png`、`debug-shadow.png`；这些是**离屏 D3D12 帧**，不是宿主场景截图。`region_id`、`guide`、`root_binding` 仍由 CPU/数据侧报告提供，GPU 尚未实现对应通道。

## 7. 局部修改闭环

```json
{
  "id": "fixture_bob",
  "expectedRevision": "db4089b3…",
  "note": "把刘海修短，让右眼露出来",
  "operations": [
    { "kind": "region", "region": "bangs", "fields": ["lengthMetres"], "changes": { "lengthMetres": 0.095 } }
  ]
}
```

规则：

- `expectedRevision` 不匹配立即失败，不做“尽力合并”。
- `fields` 必须显式列出：这是一个白名单，防止一次 patch 顺手改掉材质或种子。
- `lock` / `unlock` 是独立操作；被锁定的区域不会被重生成。
- 返回的 `diff.regeneratedRegions` 与 `diff.untouchedRegions` 是验收依据。本仓库实测：`bangs` 长度 0.115 → 0.095 m，285 根刘海 guide 的末端移动，`back` 区域移动数 **0**。
- 需要重做时先 `dryRun` 再执行；撤销走编辑器服务的 revision 事务，不要自己回滚资产文件。

## 8. 错误码与恢复

| 错误 | 含义 | 处理 |
| --- | --- | --- |
| `NG-E-HAIR-FORMAT` | 规格字段缺失/越界/未知字段 | 按报错字段改规格，不要加字段绕过 |
| `NG-E-HAIR-REVISION` | `expectedRevision` 过期 | 重新读取当前 revision 再提交 |
| `NG-E-HAIR-LOCK` | 修改了被锁定区域/字段 | 先 `unlock`，或改别的区域 |
| `NG-E-HAIR-BUDGET` | 超过 `budget.maxStrands/maxPoints` | 降低密度或发丝数，不要放宽预算掩盖 |
| `NG-E-HAIR-GPU-GEOMETRY` | 曲线含零长段、非有限坐标或非法半径 | 修生成参数；包里的坏曲线不该上传 |
| `NG-E-HAIR-GPU-BUDGET` | GPU 点/发丝数超限 | 降 LOD 或降密度 |
| `NG-E-HAIR-CAPTURE` | 视角/相机参数非法、点位于相机后方 | 用 `cameraForView` 生成的相机，不要手写 |
| `NG-E-HAIR-RENDER` | 参考渲染输入非法（含零方向光） | 修正后重试，不要忽略返回的黑帧 |

## 9. 验收清单

交给用户之前逐条填写，未测项写“未测”，不要留空：

- [ ] 绑定：`rootsOffSurface = 0`，`maxSurfaceError` < 1 mm（实测 ≤ 0.162 mm）
- [ ] LOD：`maxRootDriftMetres = 0`，`sharedIdentity = true`
- [ ] 预算：发丝数、点数 ≤ 预算；cook 字节数记录在案（实测 4,889,520 B）
- [ ] guide 穿透：`enforced: true` 的姿态必须 0；`enforced: false` 的姿态列出深度并标注“需求解器”
- [ ] 绘制契约：GPU 包 `uploadBytes`、`draw.vertices`、最大段长比（实测 5,576,928 B / 693,504 / 2.114）
- [ ] 覆盖率：中性光正面 `coverageFraction` 在 0.2–0.6（实测 0.464）
- [ ] 透明层：`overflowFraction` 与 `spillPixels` 记录；溢出必须走兜底层而不是丢片
- [ ] 自阴影：开/关阴影的均值亮度差与“内部比边缘更暗”的趋势
- [ ] 背光：浅发色比深发色透射更多（实测浅色/深色亮度比 > 1.3）
- [ ] D3D12 绘制（只在 `test:hair:gpu` 通过时勾选）：IA 顶点 = 6×段数；VS/PS 计数 > 0；三视图不同且同输入逐字节一致；实测 14 帧 / 9,015,552 IA 顶点 / 141 检查 / 9 组
- [ ] 覆盖率与自阴影（GPU）：正面覆盖率 0.497；深遮罩 17,544 texels、0.089 m 最大光学厚度；自阴影均值亮度 0.035 < 无阴影 0.059（同一侧视图）
- [ ] 损坏包拒绝：`HGP1` 的魔数、版本、步长、分节尺寸、截断、载荷哈希、包围盒、重复稳定 ID、guide 越界、非有限点共 10 类拒绝
- [ ] 宿主场景帧（只在 `test:hair:host` 通过时勾选）：`groomRuntime.configured = true`；240 帧全部绘制；同相机有/无发型的像素差 >1000 且压暗 >500；实测 4,459 / 1,425
- [ ] guide 模拟：`simulating = true`；每帧递增 tick；最大拉伸 ≤ 1.05（实测 1.000014）；最终穿透 ≤ 1 mm（实测 0）；根漂移 = 0；陈旧上传 = 0；有风截图相对无风 >1000 像素（宿主 3,417，离屏 41,534）
- [ ] 透明排序：`depthSorted = true`；每帧排序全部发丝；`orderStaleUploads = 0`；排序相对上传顺序至少改变 100 像素（离屏实测 20,119）
- [ ] 头部绑定：`headAttached = true`；25° 转头截图相对无转头 >1000 像素（宿主 6,437，离屏 40,787）；模拟拉伸与根漂移不因转头恶化
- [ ] 未测项：走跑/跳跃动作序列、硬件闪烁、顺序无关/有界透明缓存、毛发向场景投影、GPU guide 模拟、GPU 时序、显存——原样列出

## 10. 提示词模板

**模板 A：从参考图做发型**

> 目标角色在 `<项目>/assets/builtin/robot/character.json`。先调用 `hair_capabilities` 确认版本与缺口，再逐张 `hair_reference_register`（`authorized: true`），记录视角与裁剪。分析可见发际线、分缝、长度、轮廓、发束流向、卷曲与颜色，把不可见区域标为推断并给出置信度；多图冲突时保留冲突，不要取平均。用 `hair_analysis_reconcile` 合成规格，固定种子与预算，`hair_create` 先 `dryRun` 再执行。用 `hair_validate` 报告绑定、穿透、LOD 与预算，`hair_capture` 导出正面/侧面/背面/四分之三/发际线近景。每轮只改明确区域并带 `expectedRevision`，保留已确认区域；改完重新校验与重拍。报告里写明资产版本、实测数值与未测项，未通过项不得描述为完成。

**模板 B：截图驱动局部修正（刘海遮眼）**

> 正面截图中刘海遮住右眼。用该截图的资产哈希与 revision 确认未过期，通过 `guide` 通道与 `bangs` 区域定位可见 guide。只调整这些 guide 的 `lengthMetres` 与 `comb`，锁定 `crown` 与 `back`。提交 `hair_patch`（`expectedRevision` + `dryRun`），确认 `diff.regeneratedRegions = ["bangs"]`、其他区域未动。重新渲染正面与四分之三视图，核对右眼露出、左右轮廓对称与分缝未变。

**模板 C：材质与背光检查**

> 用同一相机与同一 revision 渲染 `neutral` 与 `backlight` 两组，比较发束体积、根尖明暗与轮廓。若背光下发束消失，先查覆盖率与透明层是否溢出，再查材质 `melanin`/`redness`。不要通过加宽发丝或提高曝光“修好”背光。

## 11. 文件落位

- 发型源与 cooked 包：`<项目>/assets/hair/source/`、`<项目>/assets/hair/cooked/`
- 截图与指标：`<项目>/reports/hair/`
- 引擎内项目必须位于 `%USERPROFILE%/Documents/nativeEngineProject/<项目名>`，不能写回引擎源码目录。

## 12. 完整调用记录（可复现）

```bash
# 1) 生成器与绑定契约（15 项，纯 JS）
npm run test:hair

# 2) 绘制契约、磁盘包与参考渲染（17 项，输出 PNG）
npm run test:hair:draw

# 3) 原生 groom：把发型 cook 成 groom 包并在原生求解器里跑起来（3 项，含一次 clang 编译）
npm run test:hair:groom

# 4) 一次完整样例：cook → 校验 → GPU 打包 → 截图计划 → 参考出图 → 局部 patch
npm run hair:sample

# 5) 只导出 groom 包（默认 explicit，加 --compatible 走球冠路径）
npm run hair:groom

# 6) D3D12 发丝绘制：HGP1 → 着色器 → 离屏真帧与像素证据（需要硬件 GPU）
npm run test:hair:gpu

# 7) 宿主场景帧：--groom 接进角色所在场景，同相机有/无发型像素差（需要硬件 GPU）
npm run test:hair:host
```

`npm run test:hair:host` 实测（`build/hair-host/evidence.json`，AMD Radeon 890M）：

| 项目 | 实测值 |
| --- | --- |
| 场景 | `.screen/hair-preview.scene.json`，角色与发丝同场景；640×360 |
| 帧 | 240 帧全部绘制发丝（`--no-stress`，无中途重建） |
| 提交 | 3,467,520 次发丝 / 166,440,960 个 IA 顶点 / 55,480,320 三角形 |
| 像素差 | 4,459 像素变化，1,425 像素被发丝压暗（同相机、无 `--groom` 控制帧） |
| guide 模拟 | 240 tick；拉伸 1.000016；最终穿透 0；根漂移 0；陈旧上传 0；无风 0.45 m/s |
| 风 | `5,0,2` m/s：速度升到 1.66 m/s，相对无风改变 3,417 像素 |
| 头部绑定 | `headAttached = true`；25° 转头改变 6,437 像素；模拟拉伸 1.000016 |
| 透明排序 | 240 帧全部 `depth-sorted-alpha`；每帧 14,448 个 strand 排序；陈旧 order 上传 0 |
| 自阴影 | 17,544 texels / 0.089 m 最大光学厚度 / 793,905 层溢出 |
| 透明 | `depth-sorted-alpha`（根点视深，远→近） |

宿主命令可直接复现：

```bash
build/hair-host/NativeGPU.exe --smoke --validation --no-watch --no-stress --audio off \
  --frames 120 --render-size 640x360 --scene .screen/hair-preview.scene.json \
  --camera "<自动或显式 e0,e1,e2,t0,t1,t2>" --groom build/hair-host/hair-pack.hgp \
  --report build/hair-host/with-groom.json \
  --screenshot build/hair-host/with-groom.png --screenshot-frame 90
```

不给 `--camera` 时，宿主按 `HGP1` 包围盒自动取景（中心 + 2.2×最大边长）。`--groom-debug coverage|tangent|roottotip|strandid|material|depth|shadow` 切换通道，`--groom-shadow 0..1` 关闭/调节自阴影，`--groom-sim on|off` 开关 guide 模拟，`--groom-wind x,y,z` 设置目标风速（m/s，`5,0,2` 是有风证据使用的值），`--groom-head-turn <deg>` 让头部骨骼与发丝一起绕 posed 头原点转向（证据用 25°）。

`npm run test:hair:gpu` 在 AMD Radeon 890M 上的实测输出（`build/hair-draw/evidence.json`）：

| 项目 | 实测值 |
| --- | --- |
| 绘制 | 200 帧、138,007,296 IA 顶点（693,504/帧）、14,448 发丝、8 段/发丝 |
| 像素 | 覆盖率 0.497（256²，正面中性光）；15,045,502 次 PS 调用 |
| 自阴影 | 网格 192²×4 层、17,544 texels、1,001,914 次穿越、793,905 层溢出、最大光学厚度 0.089 m |
| 亮度 | 同一侧视图：自阴影 0.0352 vs 无阴影 0.0591 |
| guide 模拟 | 61 tick、拉伸 1.000014、最终穿透 0、根漂移 0；无风 0.45 m/s、风 1.66 m/s；风改变 41,534 像素 |
| 透明排序 | `depth-sorted-alpha`；排序相对上传顺序改变 20,119 像素；陈旧 order 上传 0 |
| 头部转头 | 25°：40,787 像素变化；`headTransform` 同时作用于头网格与发丝 |
| 损坏包 | 10 类拒绝（魔数/版本/步长/分节尺寸/包围盒/载荷哈希/截断/重复 ID/guide 越界/非有限点） |
| 边界 | `gpuGuideSolve: false`、`orderIndependentTransparency: false`、`sceneShadowCast: false`；`depthSortedTransparency: true` |

PNG 输出：`front.png`、`back.png`、`three-quarter.png`、`front-coverage.png`、`debug-coverage.png`、`debug-tangent.png`、`debug-roottip.png`、`debug-shadow.png`、`shadow-on.png`、`shadow-off.png`、`backlight-light.png`、`backlight-dark.png`、`tint-on.png`、`simulation-calm.png`、`simulation-wind.png`、`closeup.png`、`unsorted-front.png`、`head-turn.png`。**这些是离屏 D3D12 帧**，不是宿主场景帧；不要把它们写成引擎场景截图。

`npm run hair:sample` 的输出保存在 `build/hair-sample/`：

| 项目 | 实测值 |
| --- | --- |
| 发型 | `fixture_bob`，5 区域，1806 guide，14448 渲染发丝（L0）/ 7224（L1），头皮 2390 三角形 |
| cook | `nativegpu.hair-cook/1`，hash `ccfb67ce…`，4,889,520 B，8 段/发丝 |
| 校验 | `passes: true`；根离面 0；表面误差 0.00013–0.162 mm；`neck_bend` 穿透 9.4 mm、`shoulder_raise` 18.7 mm 均标 `requiresSolver` |
| LOD | 身份漂移 0，`sharedIdentity: true`，按索引复用会漂移 0.124 m |
| GPU 包 | 130,032 点 / 115,584 段 / 693,504 顶点 / 5,576,928 B |
| 参考渲染 | 正面中性光覆盖率 0.464、均值亮度 0.107、透明层溢出 0.489、533 ms（320²，16 层） |
| 局部 patch | 刘海 0.115 → 0.095 m，285 根刘海 guide 末端移动，`back` 移动 0，`regeneratedRegions = ["bangs"]` |
| 显式 groom | `nativegpu.groom/2`：1806 guide / 14448 发丝 / 2390 头皮三角形 / 5 区域组，4,830,720 B |
| 兼容 groom | 65 guide / 520 发丝，189,664 B，附带三条损失 |

截图见 `build/hair-sample/frames/`：`front-neutral.png`、`front-backlight.png`、`three_quarter_left-*.png`、`back-*.png` 以及 `front-neutral-coverage.png`。**这些是 CPU 参考渲染**，用于核对覆盖率、透明层与自阴影的数值关系，不是引擎帧，也不能替代引擎帧。

## 13. 已知缺陷与边界（写给下一个改这块的人）

- 发型进原生 groom 现在有两条路：`npm run hair:groom` 默认写 `nativegpu.groom/2`，把发型的真实头皮网格（2390 三角形）、1806 根绑定 guide、14448 发丝与引导权重原样转写（4,830,720 B）；`--compatible` 仍保留球冠路径（65 guide / 520 发丝），用于只认程序化源的旧运行时，并附带三条已知损失。原生解析器与 CPU 引导求解器验证见 `npm run test:hair:groom`。
- 引导求解器的**默认刚度不适合长发**：默认参数下 15 cm guide 在重力下拉伸 1.64 倍，12 次迭代 / 4 子步 / compliance 5e-7 后降到 1.04。默认值是为眉毛、胡须这类短毛调的，接到发型前必须显式设置。
- 引导权重曾经在 cook 时被切片后未归一化（14448 根全部只剩 84.5% 权重），已修复并由 `test:hair:draw` 中“权重和为 1”的断言守住。
- 参考渲染是单方向光、屏幕空间线性插值、单一光照遮挡网格，与 GPU 实现一致的部分只有：覆盖率算法、自阴影（深遮罩）与背光透射模型。GPU 侧**没有**有界透明层缓存：它按 strand 根点视深做后到前排序，再进行预乘 alpha 混合；同一像素上两根交叠发丝的次序仍可能错误。任何依赖透明层溢出兜底的断言只属于 CPU 参考。
- GPU 深遮罩网格由 CPU 在首次绘制（或光源方向/密度变化）时从发丝构建并上传，192²×4 层。夹具上 1,001,914 次穿越中有 793,905 层被丢弃（79%），说明 4 层在近景发型上严重不够：当前画面仍能变暗，但“哪一层被保留”取决于穿越顺序。提高层数或改成真正的阴影 pass 是下一步，不要用现在的溢出数字证明阴影精度。
- 宿主入口现在是 `--groom <HGP1>`：`renderer/d3d12_renderer.cpp` 在植被之后、体积合成之前绘制，深度和 HDR 目标与角色共用；`test:hair:host` 用同相机控制帧证明多出来的像素是发丝。但它仍是 **smoke 序列**：有 25° 转头与风，没有走跑/跳跃或镜头运动闪烁测试、没有 OIT，也没有把毛发投到身体或场景。
- 宿主预览场景 `.screen/hair-preview.scene.json` 把角色的 `character.scale` 设为 1，让 rig 的头骨位置（约 0.92 m）与 `HGP1` 的发根对齐。换成按 1.8 m 拟合的角色时，发丝会落在错误高度；这时要么调整场景 character scale/offset，要么在发丝 cook 前应用同一缩放。
- guide 模拟在 CPU：每帧最多 4 个 1/60 s tick，每 tick 4 子步 × 8 次约束迭代，然后插值 14,448 条发丝并把 130,032 个点写入三份轮换的 UPLOAD 缓冲（每份 4.16 MB）。它是数值权威，不是 GPU solve；GPU 版本仍需另写 compute。
- 发根绑定是**刚性**的：宿主取角色 head 骨骼的 skin matrix（`model * skin`）作为发丝变换，转头时把头骨子树绕 posed 头原点旋转，头网格与发丝共用同一变换。模拟仍在静止空间进行，重力/风按逆变换旋转。它不处理面部骨骼或蒙皮发根权重；换角色时若头骨名里没有 “head”，`headAttached` 会为 false 并回退到静止位置。
- 风是“目标速度 + 阻力”（默认 6/s），不是对场景流场的采样；宿主 `--groom-wind` 在整段运行中常驻，没有阵风或风向变化序列。
- 发丝几何是 cook 的静止姿态；guide 由 `renderer/hair_draw.cpp` 的 CPU 求解器每帧重解并插值，没有 GPU guide 求解；转头由 `headTransform` 施加，不重新生成发丝。
- 夹具发型本身偏稀（1806 guide、14k 发丝），出图看起来比真实头发“通透”。这是发型密度与生成参数的问题，不是渲染器的问题：覆盖率、透明层与自阴影的数值关系在 14k 上就能验证，但**美术效果验收需要 10 万发丝级别的发型**，并且必须在引擎帧上做。
