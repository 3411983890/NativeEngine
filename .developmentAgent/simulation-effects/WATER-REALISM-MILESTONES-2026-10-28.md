# 真实水体 M0–M7 审计与实现记录（2026-10-28）

> 对应方案：[WATER-REALISM-DEEPSEEK-PROMPT-2026-10-28.md](WATER-REALISM-DEEPSEEK-PROMPT-2026-10-28.md)。
> 本文记录本轮实际核查、改动、增量验证、仍缺项与下一步。历史验收数字不代表本轮构建。

## M0 现状审计

### 数据链（已核实）

1. 场景 `water` 依赖由 `host/scene_loader.cpp` 严格校验后解出 cooked 字节。
2. `engine::water::parseCookedWater` 校验容器哈希、payload 哈希与全部字段，得到 `Asset`。
3. `host::PondDemo` 持有 `water::Body`，供 `Game::setWater` 共享同一个权威对象。
4. 固定 tick 内 `Game::tick` 依次执行：脚掌/根部接触 → 涉水状态 → 浮力/阻力 → `Body::step(dt)`。
5. 渲染帧调用 `PondDemo::surface` 生成 `WaterSurfaceFrame`（顶点、事件、速度场、epoch）。
6. `WaterRipple` 按 `(timelineEpoch, sequence)` 一次性注入 GPU 残差；`WaterDraw` 把 CPU 顶点与残差纹理合成为水面。
7. 喷溅只消费 `Body::events()` 历史；GPU 视觉预算不影响 CPU 水量账本。

### 进入本轮时的主要缺口

| 缺口 | 影响 |
| --- | --- |
| 事件去重依赖 `tick` | 同 tick 多接触只能有一个身份；超批次容量可能被永久跳过 |
| 无 timeline epoch | 重置/倒带后 GPU 可能把新事件当作旧事件 |
| 仅根部步距接触 | 左右脚不能在不同位置扰动 |
| `impactSpeed` 只来自水平步距 | 跃入水面与快步无法区分 |
| 全局均匀 inflow/outflow | 无法表达河道入口、方向与受限出流 |
| 无本地化床高/障碍 | 石头不阻挡水流；场景碰撞与水无关 |
| 无水体反作用 | 浮力/阻力是单向读取 |
| 泡沫不随流搬运 | 白水钉在世界坐标 |
| 无转移账本 | 瀑布只能靠视觉粒子，无法守恒 |
| 残差只传播不平流 | 河流上的细波不随主流移动 |

## M1 多接触事件与真实脚掌

- `InteractionEvent` 新增：`sequence`、`timelineEpoch`、`contactId`、`actorId`、`kind`、`source`、`normalSpeed`、`displacedVolumeProxy`。
- `Contact` 输入结构承载完整语义；旧 `interact(x,z,…)` 保留为 `Touch/ActorRoot` 兼容入口。
- 事件历史环 32 → 128；被淘汰事件计入 `Diagnostics::droppedEvents`，不再静默消失。
- `Body::reset/setLevel/addObstacle` 推进 `timelineEpoch` 并重置 `sequence`；序列从 1 开始，避免空游标误判。
- GPU `WaterRippleEvent` 44 字节：`tick` 只表示发生时间，`sequence` 才是交付身份。`WaterRipple` 只在成功拷贝后推进游标；单批 16 条，余量在下一次渲染帧续传。epoch 变化清空旧场与游标。
- `coupling` 新增 `updateFootContact` 状态机：`Dry → SurfaceCrossing → SubmergedContact → Dragging → Leaving → Dry`。站立不重复脉冲；拖曳按距离积分；出水弱于入水；瞬移只更新状态不造假事件。
- `Game` 在存在动画骨架时使用 `featureState().feet[side].pivot` 作为左右脚掌，带接触 ID 与来源；缺失或过期姿态时退回根部步距。根部路径现在把 `verticalVelocity_` 作为法向速度传入。
- 落水判定：法向相对速度 > 2 m/s 记为 `Landing`，否则 `FootPlant`；半径与位移随法向速度增长。

## M2 传播质量与主流平流

- `WaterVertex` 40 字节，新增 `velocity[2]`；`WaterRipple` 上传 `float4(depth, vx, vz, foam)`。
- `water_ripple.hlsl` 采用半拉格朗日回溯（双线性采样）搬运高度与垂直速度，再用九点拉普拉斯演化；零流速时退回原单元，保持既有对称性。
- CPU：方向接触在 0° 与 45° 都沿运动方向拉伸，比例一致（本轮 `rotationAgreement = 0.0053`）；8×4 长条域径向接触 `xx/zz = 1.0013`。
- GPU：同场景 `squareBias = 0.000855`（阈值 0.1），`directedAspect = 2.433`（阈值 1.6），`flowShift = 0.0963 m`（静水基线 < 0.05 m，均匀 0.5 m/s 流 24 帧）。
- 未加入色散扩展；残差仍是最低阶线性波。

## M3 局部进出口与河床

资源可选字段（缺省即旧行为，schema 仍为 `nativegpu.water/1`，cook 仍为 `/1`）：

```text
sources[]: { x, z, radiusMetres, flowCubicMetresPerSecond, speedMetresPerSecond?, direction? }
drains[] : { x, z, radiusMetres, flowCubicMetresPerSecond }
bathymetry.type = "channel" | "mounds"
```

- 入口在子步内按 `dV = Q·dt` 与开口有效非固体面积归一化注入，并同时注入 `dV·u` 的水平动量。
- 出口受区域可用水量约束；账本记录请求、实际与受限体积。干出口不会抽出负水量。
- 全局 `inflow/outflow` 保留兼容，与局部开口共用同一账本。
- `channel` 提供沿局部 X 的河槽与两岸坡；`mounds` 提供圆形床高凸起。
- 数值证据：坡床静水 4 s 最大流速 < 0.002 m/s、自由表面保持水平；河道入口/出口 2 s 的 `added − removed` 与库存差 `2.9e-4 m³`；指定流量在 0.125 m 与 0.25 m 网格下一致。
- 新样例资产 `assets/water/river_flow.water.json`：河槽床、局部入口/出口、一块水下石与一块出水石；`node tools/water.mjs validate --in assets/water/river_flow.water.json` 通过（192×64，哈希 `ae27448f…`）。

## M4 静态石头与岸壁

- `obstacles[]: { x, z, radiusMetres, topHeightMetres }`（水域局部坐标与局部高度）。
- 低于静水位的障碍抬高床面，水可从顶部通过；达到或高于静水位的格子标记为固体，相关面质量/平流/压力通量全部阻断，格子内水深恒为 0。
- 障碍换算在 `reset` 之前完成，被排开的水量进入初始基线，不会被报告为漏水。
- `Body::bedAt` 浅水路径改为读取网格床高，浮力查询能看到石头。
- 场景耦合为显式开关 `water.obstaclesFromColliders: true`（默认关闭，避免旧场景行为漂移）：静态 `prop/wall` 碰撞列投影到水域成为障碍。桥面/悬空几何按实心柱处理，当前不做桥洞分类。
- 数值证据：出水石头 2 s 质量误差 0，固体格始终干；低石头床高 0.2 m、仍有水覆盖且质量守恒；出水石头存在绕流速度。

## M5 动态物体反作用与泡沫搬运

- `Body::applyImpulse` 按湿格水深权重分配水平冲量，记录 `reactionImpulseNs`，不改变水量。
- `Game::applyWaterCoupling(dt)` 在施加浮力/阻力后，把同一 tick 的水平阻力积分以反向冲量还给水体；垂直浮力不转写为水平动量。每 tick 仅一处调用，未重复施力。
- 泡沫改为被动标量：用上一帧速度快照半拉格朗日搬运，按 0.45 s 时间常数衰减。3×3 扩张只作用于新源，不再每帧扩张整个场（旧实现每帧膨胀一格，数值上压过平流）。
- 泡沫激活阈值改为高于常规波速（`knee ≥ 0.25 m/s`），表面偏离需超过 3.5 cm 才计入；否则footstep 产生的全池水体运动会把整片水涂成白水。调整后截图只保留脚边尾迹与岸边泡沫。
- 数值证据：移除 4 N·s 反作用后水体动量减少 4.000；均匀 1 m/s 流中泡沫在 0.2 s 内到达下游 0.2 m，静水中同一位置不漂移。
- 动态物体排水仍使用代理体积，未把物体占据写入自由面积；该限制保留在方案 10.3 的近似范围内。

## M6 落差与瀑布水量转移

新增 `engine/native/water/transfer.{hpp,cpp}`：

- `TransferLink` 连接上游/下游 `Body`，以局部源区扣水、落点区放水。
- 支持瞬时与在途两种模式；在途体积累积到 `inTransitVolumeCubicMetres`，按到达时间交付。
- 扣水不超过区域可用量的 95%；落点交付失败计入 `lostVolume`，不静默丢弃。
- 每次落地向水体发一个零净体积 `Landing` 接触（`source = Flow`，法向速度为配置值），供波纹、泡沫与喷溅消费；动量只由入水时的速度注入一次。
- 视觉落点接触按 `landingEventIntervalSeconds`（默认 0.08 s）合并；每个到达体积仍逐 tick 入账，喷溅不再被 120 Hz 的落水事件淹没。
- `setEnabled(false)` 停止新扣水但仍交付已在途水量；`reset()` 把在途水退回上游，防止重载重复交付。
- 场景 `water.links`（当前恰好一条）引用第二份 cooked 水体及原点、局部源/落点、流量、在途时间、水片宽度与法向速度；加载器校验路径、哈希和 cooked schema。
- 宿主在固定 tick 观察器中步进上游 `Body` 与 `TransferLink`；上游水面走第二个完整 `WaterRipple + WaterDraw` 通道，落差水片由 `WaterfallDraw` 与 [waterfall.hlsl](../../shaders/waterfall.hlsl) 绘制。
- 数值证据：2 s 会话 `requested→withdrawn→delivered+inTransit+lost` 逐项平衡（< 1e-6 m³），上下游加在途总水量差 < 5e-3 m³；两水域平移 (3,2.5,-4) 与 (-5,1.2,6) 后，落点接触仍出现在世界坐标的配置位置。
- 实景证据：`waterfall.scene.json` 运行 180 帧，456 次体积到达、46 次合并落点接触、喷溅 51 次且 `capacityRejected=0`，`withdrawn=0.6000` 与 `delivered+inTransit` 对齐。
- 限制：水片是单层竖直带，没有三维翻卷/多层自由落水；源区需人工放在上游水体边缘，尚未自动识别地形瀑布口。

## M7 光学、LOD 与性能

- 吸收路径改为折射路径的近似长度：按 Snell 定律用 `1/cosT` 修正弦长，并明确是深度样本近似，不是光线追踪。
- 焦散为有界视觉近似：残差曲率聚焦、随厚度指数衰减，只调制已透射光；不是光传输求解。新增调试视图 7 单独显示焦散项，`--water-debug 7` 可分离检查。
- 泡沫滚动使用同一速度快照，与水波/流场方向一致。
- 喷溅消费改为 `(timelineEpoch, sequence)` 游标：同一 tick 左右脚都能生成水滴；拖曳接触的强度乘 0.35，避免涉水行走时全强度连环爆发。
- `PondDemo::surface(debugMode, lod)` 提供 129/65/33/17 顶点网格；`--water-lod` 可切换。LOD 不触碰权威体：本轮测试确认切换前后 digest 与体积完全一致。
- 表现隔离：同一玩家轨迹下，每 tick 生成水面/泡沫与完全不生成水面的两个实例得到相同 `Body::digest()` 与相同扰动计数，证明泡沫、LOD 和绘制不会回写权威。
- `Diagnostics` 新增 `lastStepMilliseconds`；宿主报告新增 epoch、进/出流实际值、受限量、固体/源/汇格数、丢弃事件、步耗时与 LOD。
- 动态湿润度：`host/wetness.hpp` 用五个水面探针维护每实体湿润历史。地块只有全footprint 覆盖才算湿，避免一个池塘把整片庭院涂暗；岩石/墙体只要有探针触水就算湿。湿润立即达到 1，离开水后按 6 s 指数干燥。
- 渲染通道：`DrawItem::wetness` 在 bindless 不透明路径复用空闲的 `ormIndex` 根常量（`0xFFFFFFFF` 表示沿用材质值），`material.hlsl` 用它覆盖 `material.wetness`；静态场景 pose 现在携带实体 id，湿润值才能按实体匹配。`--wetness off` 可关闭整条通道做 A/B 对照。
- 湿润证据：瀑布场景开启/关闭湿润的水底区域平均亮度差 −6.76（约 6.9%），44,475 个像素发生变化，最大单像素差 19；关闭时报告 `wetEntities=0`。证据：`reports/water-realism/wetness-on.png` 与 `wetness-off.png`。
- 时钟独立性：把同一秒的渲染时间分别按 30/60/144 帧投递到 `engine::simulation::Clock`，逻辑时钟仍产生 120 个固定 tick，三档帧率得到完全相同的 `Body::digest()`。
- 多视角/距离：同一瀑布场景在默认、侧向 (9,4.5,4) 与远景 (0,6.5,18) 三个机位各运行 240 帧，`arrivals=456`、`landingEvents=46`、`disturbances=26`、`tick=480` 逐项一致；证据 `multiview-side.png/far.png` 及同名 JSON。
- 限制：湿润度按实体统一，不是逐像素水线；高于水面 1.2 m 的墙体不参与，避免整面墙被均匀涂湿。
- 未加入法线/位移/折射相位统一的高级色散；现有残差、解析细波和泡沫已共享流速。

## 资源与兼容

- 新字段全部可选；未写入的资产 payload 与旧版完全一致，内容哈希不变（测试覆盖）。
- 单位：位置/床高/水深 m；速度 m/s；流量 m³/s；冲量 N·s；泡沫与湿润度无量纲。
- 坐标：`sources/drains/obstacles/bathymetry` 使用水体局部米；`origin` 为水域中心；GPU footprint 发布角点。
- 权威：CPU `Body` 是水量与水平动量的唯一权威；GPU 残差、泡沫、喷溅、LOD 均不可回写。
- 生命周期：`reset/setLevel/addObstacle` 推进 `timelineEpoch`；事件 `sequence` 在 epoch 内单调。转移在 `reset` 时退回在途水量。

## 本轮运行的增量验证

```text
node .incrementalTesting/simulation/water-realism.mjs --cpu-only   # 34,271 checks + 8 JS contracts
node .incrementalTesting/simulation/water-gpu.mjs                  # 66,063 checks
node .incrementalTesting/simulation/water-realism.mjs              # 240-frame host + 7 AOV views
dxc -T cs_6_0 water_ripple.hlsl / -T vs_6_0 / -T ps_6_0 water_surface.hlsl
```

完整宿主验证使用隔离快照（`build/water-snapshot3`）运行，避免并行改动触发来源哈希守卫。结果：

```json
{"success":true,"water":{"frames":240,"residual":{"dispatches":240,"eventsInjected":29}},
 "splash":{"contacts":29,"spawned":4,"capacityRejected":0,"failed":0},
 "disturbances":29,
 "debugViews":[1,2,3,4,5,6,7]}
```

截图：`Documents/nativeEngineProject/InteractivePond/reports/water-realism/interactive.png`；
分项视图：同目录 `aov/debug-1.png` … `debug-7.png`；机器证据：`acceptance.json`、`interactive.json`。
宿主哈希：`81761B99D41756EE59E413CD5529D6BE06A08C153CB05D2C1B827BEEA6B97BF8`；
隔离快照：`build/water-snapshot3`（仅用于本轮可复现验证）。
画面检查：人物涉水处有连续波面、尾迹与少量泡沫，岸线泡沫未整片覆盖；模式 7 显示焦散只出现在波面弯曲处，静水为黑，不再整体提亮。

瀑布场景（运行时项目 `.screen/waterfall.scene.json`，上游基座 + 第二水体 + `water.links`）：

```json
{"success":true,"waterLinks":[{"arrivals":456,"landingEvents":46,"withdrawn":0.60000004,
 "delivered":0.57000004,"inTransit":0.030000002,"lost":0,"shortfall":1.5e-8,
 "flow":0.15}],"wetEntities":1,"splash":{"contacts":72,"spawned":51,"capacityRejected":0}}
```

截图 `reports/water-realism/waterfall.png` 显示上游水面、支撑墙体、连续下落水片与落点水域。

湿润度 A/B（同一相机、帧与场景，仅切换 `--wetness on/off`）：水底区域平均亮度 91.90 → 98.66（差 −6.76），44,475 个像素变化，最大单像素差 19；关闭时 `wetEntities=0`。证据：`reports/water-realism/wetness-on.png` / `wetness-off.png`。

GPU 结果：

```json
{"success":true,"checks":66063,"squareBias":0.000855141,"directedAspect":2.43259,
 "firstMinimum":-0.0097134,"flowShift":0.096259}
```

CPU 新增证据（同一轮输出）：

```json
{"contacts":{"sameTick":2,"firstSequence":1,"lastSequence":2,"epochBefore":1,"epochAfter":2,
 "footEntries":2,"footDrags":2},
 "propagation":{"rotationAgreement":0.00531087,"nonSquareAspect":1.00131},
 "river":{"velocity":0.0164128,"ledgerError":0.000291824},
 "obstacleMassError":0,"reactionImpulse":4,
 "clock":{"fps":[30,60,144],"ticks":120,"digest":5637026023161830000},
 "foam":{"downstream":0.279939,"far":0},
 "transfer":{"before":51.202,"after":51.2,"inTransit":0.0125}}
```

## 未实现 / 未集成 / 未验证

| 项 | 状态 | 原因 |
| --- | --- | --- |
| 水片的三维翻卷、多层自由落水 | 未实现 | 当前是单层竖直带，满足连续主体与边缘破碎；复杂悬垂需独立三维表示 |
| 自动识别地形瀑布口 | 未实现 | 源/落点由场景显式配置，避免把任意高差误接成瀑布 |
| 湿润度按实体统一 | 近似 | 没有逐像素水线；高于水面 1.2 m 的墙体被排除，避免整墙均匀变湿 |
| 场景碰撞障碍开关的实景验收 | 未验证 | 引擎级障碍契约已通过，但 InteractivePond 场景未开启 `obstaclesFromColliders` |
| 夜间截图 | 未覆盖 | 白天默认/侧向/远景三机位已与账本对齐；夜间光照与逆光仍需单独回归 |
| CPU 高阶/色散格式 | 未实现 | 方案允许后续阶段；当前保持既有守恒格式 |
| FLIP/APIC/PBF | 未实现 | 不在本轮范围，资源声明仍不等于后端 |

## 下一阶段最小任务

1. 用真实河道项目验证 `obstaclesFromColliders` 的桥洞/悬空误判，再决定是否引入桥面语义。
2. 瀑布源区自动提取：从高差与岸线候选生成源/落点，再与显式配置对照。
3. 逐像素水线与局部湿润遮罩，替代当前整实体湿润值。
4. 夜间/逆光渲染回归，检查水片在低照度与不同 LOD 下的稳定性。
