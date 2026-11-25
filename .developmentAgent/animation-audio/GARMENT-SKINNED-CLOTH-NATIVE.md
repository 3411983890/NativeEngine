# 原生蒙皮布料（阶段 E 第一增量）

日期：2026-10-28。本文记录 [图片驱动发型生成器与角色服装权重系统开发方案](AI-HAIR-GENERATOR-AND-GARMENT-WEIGHTS-PLAN.md) 阶段 E 的第一个可验证增量：**CPU 蒙皮布料求解 + 原生数值验证**。它没有交付 GPU 蒙皮路径、蒙皮网格绘制或身体网格精确穿透，三者都在第 5 节列为未完成。

## 1. 交付内容

| 层 | 文件 | 说明 |
| --- | --- | --- |
| 服装 cook | `engine/garment/cook.mjs` | 把 `bindGarment` 的结果（成衣网格、蒙皮权重、物理通道、骨架、姿态集）烘焙成 `nativegpu.cloth-cook/3` / 源格式 `nativegpu.garment/1` |
| 夹具与 cook 入口 | `tools/garment.mjs` | 用内置角色实测截面外扩 12 mm 生成衬衫与长裤，写出 `build/garment/library.json` 与内容寻址对象 |
| 原生资产 | `engine/native/cloth/asset.{hpp,cpp}` | `/3` 解析：任意三角网格、膜/铰链拓扑、皮肤关节与权重、四个物理通道、骨骼代理、姿态集、静边表；严格字段校验，未声明字段一律拒绝 |
| 原生求解 | `engine/native/cloth/solver_cpu.{hpp,cpp}`、`skin.hpp` | `setSkeleton` / `setPose` / `reseed`，逐 tick 蒙皮目标、动态混合、锚定、软位移上限、应变限制、骨骼驱动椭球碰撞与逐帧诊断 |
| 验证入口 | `tests/native_garment.cpp`、`.incrementalTesting/simulation/garment-skinned.ps1`、`npm run test:garment:native` | 9 组、约 1350 项断言，只编译 5 个翻译单元 |

资产边界：`nativegpu.cloth-cook/3` 与 GPU 布料宿主使用的 `/1`、`/2` 共用容器、哈希与库清单（`nativegpu.cloth-library/1`），但求解器拒绝把 `/3` 交给网格路径，反之亦然。`/2` 的 `columns × rows` 与三角形数绑定关系对成衣无意义，因此校验里对 `garment` 分支改为“三角形数必须等于膜数”，并要求 `columns/rows` 为 0。

## 2. 求解顺序与它换来的结论

每个子步：积分 → 膜/铰链约束 → 蒙皮投影（锚定 + 位移上限）→ 应变限制 → 碰撞。

顺序是测出来的，不是抄来的：

- 先应变后蒙皮（最初的实现）会让焊接缝边在蒙皮投影之后被拉开，报告的拉伸里混进了求解器没处理过的量。
- 先蒙皮后应变把拉伸压到限值内，代价是碰撞必须最后执行——因此**碰撞优先于位移预算**，这一点在报告里显式写出，而不是让两个预算互相掩盖。
- 两条端点都是 `blend = 0` 的边是运动学边：由骨架驱动，求解器不能也不该“修”。它们单独计入 `seamStretchMax`，不再冒充求解拉伸。

## 3. 换掉圆胶囊的理由（实测）

角色的身体代理（`engine/hair/scalp.mjs` 的 `characterCollisionProxies`）是**圆胶囊**：半径取成员顶点到骨轴的 P85 距离。该角色躯干在 Z 方向约是 X 方向的 3 倍深，一个能包住胸部 Z 向的圆胶囊在 X 向会凸出 6 cm。实测（同一件 12 mm 外扩的衬衫、静止姿态）：

| 代理类型 | 躯干各高度环的最大初始穿透 |
| --- | --- |
| 圆胶囊（hair 路径） | 胸部 71 mm，腰 50 mm，肩 32 mm |
| 单骨椭球（P90 半径） | 躯干 0 mm，但大腿/胯部仍有 44 mm（长骨一个椭球太粗） |
| **分段内切椭球（P55，当前）** | 两件夹具全高度 **0 mm** |

结论写进 `cook.mjs` 的注释：碰撞代理必须**内切**于身体，否则“布料悬浮”会先于任何物理问题出现；长骨必须分段，否则髋部宽、膝部窄的一段大腿会把裤腿推开。当前方案：按主导骨骼分组、沿骨骼最长局部轴最多切 3 段、每段取各轴 |偏移| 的 55 分位作为半径，共 36 个椭球。

## 4. 实测数值（内置机器人与两件夹具）

`npm run test:garment:native` 的姿态集来自 `GARMENT_TEST_POSES`，每姿态 48 tick、`dt = 1/120`。下表的穿透是对**内切椭球**测量的 P99：

| 姿态 | 衬衫 stretch / 目标 stretch | 衬衫 P99 | 长裤 stretch / 目标 stretch | 长裤 P99 |
| --- | --- | --- | --- | --- |
| idle | 1.00 / 1.00 | 0.0 mm | 1.00 / 1.00 | 0.0 mm |
| walk_contact | 1.16 / 1.47 | 0.0 mm | 1.38 / 2.72 | 5.1 mm |
| run_stride | 1.53 / 3.06 | 0.0 mm | 1.65 / 3.80 | 0.0 mm |
| turn_fast | 1.17 / 1.52 | 0.0 mm | 1.04 / 1.00 | 0.0 mm |
| arms_up | 1.65 / 3.59 | 0.0 mm | 1.04 / 1.00 | 0.0 mm |
| crouch | 1.26 / 1.98 | 0.0 mm | 2.26 / 4.34 | 8.9 mm |
| jump | 1.03 / 1.00 | 0.0 mm | 1.04 / 1.00 | 0.0 mm |

- **目标 stretch 一列是重点**：夹具姿态是单 tick 直接切换的，蒙皮目标本身就把边拉长 1.5–4.3 倍。求解拉伸 1.0–2.3 恒小于它，说明限制器在起作用，而不是报告在自证。这一列同时暴露了权重/静止姿态的问题——真实导入成衣若出现同类数值，应先修权重或重做静止姿态，而不是调求解器。
- 达到方案 2 mm 目标的姿态：idle、turn_fast、arms_up、jump（衬衫 7 个姿态全部 0）。未达标的两个：walk_contact（长裤 5.1 mm）与 crouch（长裤 8.9 mm），证据 JSON 的 `posesOverPlanPenetrationTarget` 计为 2，不做平均、不隐藏。
- 位移上限只在**无接触**的 idle 上做严格断言；有接触的姿态里碰撞优先，漂移是报告项而非失败项。这一点与方案第 5.2 节“权重与物理数据分开”一致：混在一起就无法判断是被身体推开还是权重错。

## 5. 仍未实现（不得描述为完成）

- **GPU 蒙皮布料**：`host/cloth_preview.cpp` 仍对 `skinned` 抛 `NG-E-CLOTH-UNSUPPORTED`；`nativegpu::GpuClothConfig` 只吃网格资产，GPU 求解器没有皮肤、目标与锚点概念。
- **蒙皮网格绘制**：`renderer/gpu_cloth_draw.cpp` 从 GPU 求解缓冲直接取顶点并按 `index % columns` 推 UV，成衣没有网格维度，也没有绘制通道。本轮所有证据都是数值，没有帧截图。
- **身体网格精确穿透**：当前穿透是对内切椭球测的“到骨骼核心”距离。要给出方案要求的“衣服在身体外”的结论，需要把身体网格纳入测量（`engine/garment/physics.mjs` 的 `measureMeshPenetration` 已有 CPU 参考，尚未移植到原生）。
- **自碰撞与层间碰撞**：仍然显式报错。
- **姿态过渡**：夹具是单 tick 硬切姿态。真实动作需要按骨骼插值姿态，本轮没有实现插值，这也是剧烈姿态数值偏差的主因。
- **导入成衣**：夹具是按角色截面生成的；导入网格的绕序、非流形与 UV 接缝仍由 `garment_inspect` 在 JS 侧负责，原生侧只接受已通过的 cook 结果。

## 6. 复现

```
npm run test:garment        # JS：权重、分区、物理报告契约（17 项）
npm run test:garment:native # 原生：cook 夹具 → 编译求解器 → 姿态序列数值验证
```

`garment-skinned.ps1` 先删掉上一轮 cook 的内容寻址对象（cook 拒绝覆盖字节不同的同名对象），再核对来源哈希清单，最后把证据写入 `build/garment-reference/evidence.json`。
