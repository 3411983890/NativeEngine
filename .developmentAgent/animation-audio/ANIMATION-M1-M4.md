# M1–M4 非机器学习动作系统

2026-10-22。实现代码与验收入口说明；验收结果见文末及版本化报告。M5 面部未实现。本次不依赖模型权重、训练环境或推理运行时。

## 运行与内容

```powershell
npm run build:native
npm start
npm run animation
```

默认角色使用原创工程夹具：16 骨骼、方向动作片段、上臂 morph 和两个局部组织代理。WASD/Shift/Space 驱动移动；E 触发上半身交互。固定输入示例和截图由 `npm run animation` 生成。`--animation-features off` 可对照关闭动作匹配、Rig、肌肉和软组织，保留基本动画与蒙皮。

## 实现与数据流

| 里程碑 | 当前运行路径 | 主要接口 |
|---|---|---|
| M1 | 骨骼语义 / 双腿链、局部模型世界空间转换、旋转式双骨 IK、pole 与弯曲限制、Look-at、脚接触锁定、骨盆调整、地面对齐 | `RigDefinition`、`solveTwoBone`、`solveLookAt`、`GroundQuery` |
| M2 | 26 维归一化量化特征、未来轨迹 / 朝向、姿态历史、接触状态、速度、标签过滤、继续播放偏置、最短保持与惯性化；固定顺序穷举搜索 | `MotionDatabase`、`Controller::selection()` |
| M3 | morph 位置 / 法线导入、版本化二进制稀疏记录、姿态驱动肌肉、绑定空间 GPU morph 后蒙皮 | `MorphTarget`、`MuscleDriver`、16 个权重槽 |
| M4 | 每区域四点四面体代理、阻尼速度、弹性附着、六条距离约束、体积 XPBD、胶囊碰撞、限幅、嵌入映射、模型空间后蒙皮位移与法线校正 | `SoftRegion`、`SoftState`、`VertexDelta` |

`Game` 提供碰撞后的实际速度、期望速度、角色位置与角速度。地面查询对当前刚体 OBB 做向下射线求交，输出位置、法线、表面速度与表面变换；脚锁点保存在表面局部空间。碰撞移动仍由玩法负责，IK 不写角色根。

选择 / 混合 / 修正 / 形变在固定 120 Hz 求值。形变历史通过显式外部摘要接入阶段哈希；脚锁点、匹配时间、软组织位置 / 速度、LOD 与资产指纹进入完整存档 / 摘要。恢复时重建派生渲染数据，不推进模拟。CPU 承诺范围为 D0；GPU 图像另按容差检查。

GPU 使用独立蒙皮入口：静态 morph delta 常驻、逐帧更新权重；软组织仅上传模型空间增量。三套帧资源由 fence 隔离，释放随渲染器生命周期执行。材质路径也使用独立的 `VSSkin`，避免普通顶点入口跳过骨骼变形。角色动态 BLAS / 自投影仍未实现。

## 资产合同

导入器写 `nativegpu.animation/2`，读取器兼容 `/1`。顺序为 mesh 顶点 / 索引 → morph 稀疏记录 → clip 量化采样；每段偏移连续校验，内容指纹覆盖头和二进制。保留单 skin、单 primitive、正均匀骨骼 scale 等原有限制。morph 支持 POSITION 与可选 NORMAL，不支持 TANGENT、glTF 稀疏 accessor 或非零默认权重；法线有显式路径，切线资产尚未引入。

glTF primitive 的 `targets` 与 mesh `extras.targetNames` 定义 morph。文档 `extras.nativegpu` 定义：

- `rig`：pelvis / lookBone 名称，以及最多两条 `{root,joint,tip,pole,minBend,maxBend,soleHeight}` 腿链。
- `muscles`：`{bone,target,angleStart,angleEnd,maxWeight}`；目标为 morph 名称，角度单位为弧度。
- `softRegions`：`{bone,center,radius,maxOffset,vertices}`；center 是骨骼局部空间，vertices 为渲染顶点索引，长度单位为米。
- 动画 `extras.nativegpu.motion`：`{matchable,velocity,turnRate,tags,contacts}`；velocity 为角色空间 in-place 速度，turnRate 为弧度每秒，contacts 为左右脚归一化接触区间。也可使用根轨道提供位移。

未标记 matchable 的动画继续作为状态 / Slot 片段，不自动进入动作库。缺少 Rig 或肌肉 / 软组织元数据的资产保持相应能力关闭。原生宿主保留其胸部和右手附着命名约定；通用角色重定向、可视化 Rig 作者工具与全身 IK 不在本次完成范围。

## 质量范围与限制

首版采用有界解析与数值方法，肌肉是视觉形变，软组织是局部代理，均不求解真实生理肌力。组织只与绑定区域的骨骼胶囊碰撞，不包含全身自碰撞、布料、全身 FEM 或身体物理控制。

动作库在角色初始化时按固定顺序构建；当前规模使用穷举作为精确参考，无 KD-tree、后台建库或大型数据库性能承诺。数据覆盖决定动作质量，工程夹具不等同于生产动捕库。脚锁定有真实可达性残差报告，不能把静止 / 平台 / 斜坡测试的阈值泛化到所有高速移动、急转和任意角色。

软组织距离 LOD 通过 `Intent.cameraDistance` 输入并渐变，支持传送重置和暂停 / 存档续跑；宿主当前使用默认近景输入，应用可按自己的相机提供实际距离。顶点增量限制与骨骼增量预算分开计算。

## 验收入口

`tests/animation.test.mjs` 检查资产确定性和损坏输入拒绝；`tests/animation_features_tests.hpp` 检查独立几何关系、精确数据库查询、移动平台 / 斜坡接触、形变预算、LOD / 传送、跨运行和存档续跑。阈值：接触脚滑 < 0.05 m/s、脚效应器穿透 < 2 mm、示例组织位移 ≤ 2.5 cm、代理体积误差 < 1%。这些阈值限定于固定测试资产和序列，不在失败后放宽。

`.incrementalTesting/core/animation.ps1` 检查 GPU 实际 skin / morph / soft draw、能力报告、Notify、严格录制回放及五阶段哈希，并保存截图和版本化证据。既有玩法、物理、生命周期、热重载与图像门禁继续使用原入口。

## 2026-10-22 验收结果

最终构建成功，`build/source-inputs.json` 对照当前原生 / shader 源码为零不符；153 项 JavaScript 测试、玩法 / 物理 / 完整存档集成回归通过。动画专项 26/26 通过，含 5,350 条既有原生断言及 265,334 条新增检查（包括逐帧 / 逐顶点有限值和哈希检查，不等同于同数量的独立测试用例）。240 帧实际 skin / morph / soft 绘制、严格回放与功能关闭对照通过；本机没有启用 D3D12 validation layer，不能将冒烟称为 validation layer 验收。

| 固定夹具指标 | 实测 |
|---|---|
| 移动平台站立脚滑峰值 | 0.0000783 m/s |
| 平台脚效应器穿透 | 0 m |
| 斜坡稳定期脚滑 | 0 m/s |
| 组织最大代理位移 | 0.02500006 m（浮点容差范围内） |
| 代理体积误差峰值 | 0.16264% |
| 跨运行对拍 / 存档续跑 | 720 tick / 90 tick |

首次全量 `verify` 为 21/24：预算脚本没有显式开启其要关闭的 mesh 路径；两个图像合同使用了更早的阴影覆盖 / mesh 变暗数据；基线另有此次角色形变产生的差异。已分别修正并复测：预算 18/18，图像合同 5/5，图像基线 4/4。这里记录的是首次全量及随后单项复测，没有将旧的失败总报告改为通过。

图像基线逐图检查后显式接受：差异局限在人偶，阈值像素比例约 0.07%，原有四个基线门限未改。图像合同则保留旧测量历史并迁移到当前实际路径：明确 mesh 开关，mesh / vertex 平均绝对误差 ≤ 0.001（实测 0.0002、3 / 921600 个差异像素），当前场景阴影覆盖为 3.04 ± 0.10 个百分点。不是继续要求复现已不存在的 50–70% mesh 变暗；迁移依据和原值保留于 `tests/image-contracts.json` 与报告中。

汇总证据：[animation-m1-m4-completion.json](../reports/animation-m1-m4-completion.json)。最终动画专项目录：`reports/0.11.0/31c00bc53c0b/20261022T061900Z/`，保留启用 / 关闭截图、原生数据和严格回放。首次全量报告：`reports/0.11.0/31c00bc53c0b/20261022T055556Z/verify.json`。
