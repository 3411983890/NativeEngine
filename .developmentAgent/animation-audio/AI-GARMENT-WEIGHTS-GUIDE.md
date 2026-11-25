# 成衣刷权重与布料 AI 使用手册

版本：2026-10-28，对应 `nativegpu.garment-inspection/1`、`nativegpu.garment-weights/1`、`nativegpu.cloth-cook/3`（原生蒙皮布料）。本手册的数字都能用第 12 节的命令复现。

## 0. 先读这一段

**已经能用**：成衣几何诊断、身体与衣服的分区对应、带侧别与区域约束的权重转移、锁定值、受约束平滑、影响数裁剪、拓扑哈希与过期拒绝、左右镜像（显式骨骼映射）、物理合同（混合/位移/厚度/锚定/质量）、七姿态模拟与报告、权重视图与数值导出、**原生蒙皮布料 cook 与 CPU 求解**（骨架驱动、锚点、椭球碰撞、逐帧诊断）。

**还不能用**（不要在报告里写成已完成）：

| 缺口 | 现状 |
| --- | --- |
| GPU 蒙皮布料 | 不存在。`host/cloth_preview.cpp` 对 `skinned` 直接抛 `NG-E-CLOTH-UNSUPPORTED` |
| 蒙皮网格绘制 | 不存在。`renderer/gpu_cloth_draw.cpp` 从网格布料缓冲取顶点并按 `index % columns` 推 UV，成衣没有网格维度 |
| 身体网格精确穿透 | 原生侧未移植。当前穿透是相对**内切骨骼椭球**测的“到骨骼核心”距离，不等于到身体网格的距离 |
| 自碰撞与层间碰撞 | 显式报错，不假装支持 |
| 姿态过渡 | 夹具是单 tick 硬切姿态，没有插值 |

所以“成衣布料完成”只能说成：**权重与物理合同完整、CPU 蒙皮求解通过数值验收**；画面、GPU、精确穿透都还没有证据。

## 1. 工具

| 工具 | 关键输入 | 输出要点 |
| --- | --- | --- |
| `garment_inspect` | `characterAsset`、`garmentPath` 或 `garment`（内联网格） | 退化面、非流形、绕序、初始穿透、薄层、适配诊断；**从不擅自缩小衣服** |
| `garment_bind` | `characterAsset`、成衣、`allowedRegions`、`influenceRegions`、`locks`、`physics` | 蒙皮权重、物理合同、低置信度区域、影响数直方图 |
| `garment_physics_patch` | `garmentId`、`changes[]`（区域或顶点 + 值） | 更新后的物理通道，`dryRun` 返回差异 |
| `garment_validate` | `garmentId`、`ticks`、`poses`、`penetrationToleranceMetres` | 逐姿态穿透 P95/P99/最大、拉伸、发散、未支持项 |
| `weights_export` | `garmentId`、`channels[]`、`views[]`、`bone`、`resolution` | 热图 + 图例 + ID 缓冲 + 数值；两者来自同一 `weightsSnapshot` |
| `weights_patch` | `garmentId`、`expectedTopology`、`operations[]`（set/add/scale） | 权重差异、归一化与锁定冲突 |

`garment_bind` 的两个区域参数不是可选的细节：`allowedRegions` 限制衣服**能**绑到哪些身体区域（披风绑躯干/髋/肩，不绑手臂）；`influenceRegions` 限制哪些区域**可以**提供权重（裙子声明 hips/torso，否则下摆会跟着大腿走）。

## 2. 单位、姿态与左右

- 长度一律**米**：`maxDisplacementMetres`、`collisionThicknessMetres`、`backsideLimitMetres` 都是米，不要和 0–1 权重混用。
- 左右按**角色自身**命名。镜像必须给显式骨骼映射（`mirrorBoneMap`），按坐标取负会在不对称骨架上出错。
- 衣服与角色必须在同一静止姿态、同一单位；单位不一致先修导入，不要靠刷权重补偿。

## 3. 六个独立通道

| 通道 | 作用 | 范围 |
| --- | --- | --- |
| 骨骼蒙皮权重 | 决定基础姿态 | 每顶点 ≤ 4 影响、和为 1 |
| 动态混合 `blend` | 0 = 跟随蒙皮，1 = 自由模拟 | 0–1 |
| 最大位移 `maxDisplacement` | 相对蒙皮目标可移动的距离 | 米，建议 ≤ 半个网格边长 |
| 锚定 `anchors` | 领口/腰带/肩部的固定或柔性连接 | 0–1（1 = 完全焊死） |
| 碰撞厚度 `thickness` | 与身体保持的距离 | 米，按区域调 |
| 质量 `mass` | 每顶点粒子质量 | kg，与面料面密度一致 |

**最大位移必须按网格分辨率选**，不能直接填 3 cm：本仓库的衬衫环距 4 cm、横向 3 cm，因此用 6 mm；长裤纵向 8 cm、横向 2 cm，用 8 mm。位移上限大于半个边长时，限制器会把拉伸预算花在把布拉开上——实测那样做的衬衫 stretch 到 1.9，skin error 吃掉整个上限。

## 4. 工作流

```
1. garment_inspect            → 拓扑/绕序/初始穿透/适配
2. garment_bind (dryRun)      → allowedRegions / influenceRegions / locks
3. weights_export             → 热图 + 数值
4. weights_patch (dryRun)     → 局部补刷，带 expectedTopology
5. garment_physics_patch      → blend / 位移 / 厚度 / 锚定
6. garment_validate           → 七姿态数值报告
7. 复拍同一相机同姿态          → 画面复核（注意：原生绘制通道尚未实现）
```

## 5. 读校验报告

`garment_validate` 的每个姿态单独一行，**不做平均**。判读顺序：

1. `nonFiniteEvents` 必须为 0。
2. `worstPenetrationP99Metres`：先看它相对哪个代理测的。原生 CPU 路径对**内切骨骼椭球**测量，数值为 0 表示“没有穿进骨骼核心”，不等于“没穿进身体网格”。
3. `maxStretchRatio` 要和**蒙皮目标自身的拉伸**一起看。本仓库实测目标拉伸 1.0–4.34，求解拉伸 1.0–2.26；求解值恒小于目标值，说明限制器在工作。目标本身就 4 倍拉伸时，问题在权重或静止姿态，不在求解器。
4. `passes` 为 false 时要看 `failures[].code`，不要只看布尔值。
5. `SUPPORTED.selfCollision === false` 必须写进报告：长裙、宽袖的通过记录不构成认证。

## 6. 权重视图

`weights_export` 的通道：`skin_weight`（需 `bone`）、`dominant_bone`、`influence_count`、`blend`、`max_displacement`、`anchors`、`collision_thickness`、`penetration`、`confidence`。视图：`front`/`back`/`left`/`right`。

规则：

- **热图不能用来反推精确权重**。要数值就读同一份 `weightsSnapshot` 导出的数据；两者不一致说明导出链路有问题。
- 每个通道独立图例，禁止把物理权重和蒙皮权重画在同一张图上——这正是方案第 6.1 节点名禁止的做法。
- 网格 ID 选择支持稳定顶点 ID、语义区域、拓扑邻域；截图选区映射要带资产哈希与拓扑哈希。

## 7. 补刷闭环

```json
{
  "garmentId": "fitted_shirt",
  "expectedTopology": "9f2c…",
  "operations": [
    { "kind": "scale", "vertexIds": [12, 13, 14], "bone": "mixamorig:Spine2", "value": 0.5 }
  ],
  "dryRun": true
}
```

- `expectedTopology` 不匹配立即失败：重新导入或改拓扑后，旧顶点索引不再指向同一批顶点。
- 补刷后一定重新归一化；锁定顶点保持锁定值，冲突要报 `NG-E-GARMENT-LOCK` 而不是静默放开。
- 改完权重必须重跑 `garment_validate`：转移成功不等于动作形变成功。

## 8. 错误码

| 错误 | 含义 | 处理 |
| --- | --- | --- |
| `NG-E-GARMENT-INSPECT` | 拓扑/绕序/退化面 | 修网格，不要用权重掩盖 |
| `NG-E-GARMENT-BIND` | 缺少角色、顶点数不符、分区失败 | 检查单位与静止姿态 |
| `NG-E-GARMENT-UNRESOLVED` | 有顶点既无区域也无继承权重 | 补分区或指定 `influenceRegions` |
| `NG-E-GARMENT-PROPAGATED` | 宽松区域的权重由邻居继承 | 低置信度，复核下摆 |
| `NG-E-GARMENT-LOCK` | 锁定值放不进影响数上限 | 提高影响数或减少锁定骨骼 |
| `NG-E-GARMENT-TOPOLOGY` | 拓扑哈希过期 | 重新绑定 |
| `NG-E-CLOTH-UNSUPPORTED` | 宿主拒绝蒙皮布料 / 请求自碰撞 | 明确记录为未支持，不要绕过 |

## 9. 验收清单

- [ ] 权重合法性：负数/NaN/零总权重/越界骨骼数 = 0，和误差 ≤ 1e-4
- [ ] 侧别：左右肢体无串权（`filteredSideInfluences` 记录在案）
- [ ] 分区：低置信度顶点占比与位置（宽松服装必然有）
- [ ] 影响数：≤ 运行时上限（本仓库 4）
- [ ] 物理：`blend`/`maxDisplacement`/`thickness`/`anchors` 四通道都有值，位移 ≤ 半个网格边长
- [ ] 动作：idle、walk、run、turn、arms_up、crouch、jump 逐姿态 P99 与拉伸
- [ ] 未测项：GPU 蒙皮、蒙皮绘制、身体网格精确穿透、自碰撞——原样列出

## 10. 提示词模板

**模板 A：为已有成衣绑定权重**

> 目标角色 `<项目>/assets/builtin/robot/character.json`，成衣是既有网格，不要重新设计衣服。先用 `garment_inspect` 检查单位、静止姿态、拓扑、绕序与初始穿透；明显体型不匹配先出诊断，不要刷权重。用 `garment_bind` 生成保留分区的权重：`allowedRegions` 限定衣服能绑的身体区域，`influenceRegions` 限定可提供权重的区域，避免左右肢体与内外层误配。分别设置 `blend`、`maxDisplacement`（按网格边长）、`thickness`、`anchors`。用 `weights_export` 导出相关骨骼热图、主导骨骼、影响数、混合、位移与穿透，并读取同一快照的数值。`weights_patch` 用稳定顶点 ID 局部补刷，带 `expectedTopology`，保护锁定值。最后 `garment_validate` 跑完七个姿态，逐姿态报告穿透与拉伸，把未支持项写清楚。不靠隐藏身体代替物理检查，不宣称所有动作绝对无穿模。

**模板 B：抬臂时袖子被胸骨拉扯**

> 抬左臂的校验里袖子被胸骨拉扯。先 `weights_export` 导出 `skin_weight`（左上臂、锁骨、胸骨）与 `dominant_bone`，读数值确认是权重错还是物理穿透。若是权重错：`weights_patch` 降低该区域胸骨影响并归一化，`expectedTopology` 必填，`dryRun` 先看差异。若 `blend` 区域把布料拉向蒙皮目标，则调整该区域 `blend` 与 `maxDisplacement`，不要用权重去补物理问题。改完重跑 `garment_validate` 的 arms_up 与 walk，确认穿透未变差。

**模板 C：裙摆与披风的低置信度处理**

> 裙子下摆与披风没有明确的身体对应。`garment_bind` 用 `allowedRegions: ["hips","torso"]` 与 `influenceRegions: ["hips","torso"]`，让下摆继承髋/躯干而不是贴到最近的大腿骨。导出 `confidence` 与 `dominant_bone` 视图，列出低置信度顶点数量与位置。物理上 `blend` 调高、`maxDisplacement` 放大到半个边长，但明确写出：宽摆与长裙的层间/自碰撞未支持，不能宣称通过。

## 11. 文件落位

- 成衣网格与 cook 结果：`<项目>/assets/garments/`、`<项目>/assets/cloth/cooked/`
- 报告：`<项目>/reports/weights/`、`<项目>/reports/garments/`
- 引擎内项目必须位于 `%USERPROFILE%/Documents/nativeEngineProject/<项目名>`

## 12. 复现记录

```bash
# 1) 权重、分区、物理合同（17 项，纯 JS，约 20 秒）
npm run test:garment

# 2) 原生蒙皮布料：cook 夹具 → 编译 5 个翻译单元 → 七姿态数值验收
npm run test:garment:native
```

本仓库实测（内置机器人 + 按实测截面外扩 12 mm 的两件夹具）：

| 项目 | 实测值 |
| --- | --- |
| JS 契约 | 17 项全通过 |
| 原生断言 | 9 组 / 1349 项全通过 |
| 碰撞代理 | 36 个分段内切椭球；圆胶囊方案在胸部初始穿透 71 mm，换成椭球后 **0 mm** |
| 姿态 P99（衬衫） | 7 个姿态全部 0 mm |
| 姿态 P99（长裤） | walk 5.1 mm、crouch 8.9 mm，**其余 0**；方案 2 mm 目标未达成，报告里计为 2 个超限姿态 |
| 拉伸 | 求解 1.00–2.26，对应目标拉伸 1.00–4.34（求解恒小于目标） |
| 位移上限 | 衬衫 6 mm、长裤 8 mm（按网格边长选取） |

## 13. 已知缺陷与边界

- 原生穿透是相对**内切椭球**测的“到骨骼核心”距离。给出与内切代理的对比：圆胶囊（沿发丝路径的旧方案）在这具身体上胸部初始穿透 71 mm，说明“一个圆半径”无法表达扁躯干；椭球是修正，但**仍不是身体网格**。
- 蒙皮目标本身会把边拉长 1.5–4.3 倍（单 tick 硬切姿态 + 粗网格线性蒙皮）。读到这种数值先修权重/静止姿态，不要调求解器。
- 位移上限是**软上限**（每次投影收回一半超出量），且碰撞优先于位移预算：被身体推开的顶点可以超出上限，报告里分开记录。
- 夹具成衣是按角色截面生成的，不是导入的美术成衣；导入网格的绕序、非流形与 UV 接缝由 `garment_inspect` 在 JS 侧把关，原生只接受已通过 cook 的结果。
