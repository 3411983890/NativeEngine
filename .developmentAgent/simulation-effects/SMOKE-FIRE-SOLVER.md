# 独立烟火局部场求解后端

2026-10-25，完整方案 D0/D1 的首批实现：`engine/native/smoke/solver.*` 为 CPU 视觉参考，`gpu.*` 为实际 D3D12 后端，`shaders/smoke.hlsl` 为 13 个计算核。2026-10-26 已补充 [宿主与体积光学](SMOKE-RENDERING.md)、[密度自动共享及代理选择](SMOKE-SHARING.md)；原 legacy 烟火仍为独立路径，温度/速度联合共享、云烟完整混合、动态时域与玩法权威尚未完成。

## 状态、坐标与单位

Spec 声明固定局部网格尺寸、originMetres、cellMetres 和 Density/Temperature/Fuel 通道掩码。实际只为启用标量建立 SoA 段；未启用段 offset=UINT32_MAX，不保留隐含燃料/温度槽。提供了不存在通道的非零源项会在录制前拒绝。

密度与燃料使用 kg/m³，温度使用 Kelvin；输入为每秒源项，分别为 kg/(m³·s)、K/s 和 m/s²。网格中心为 origin+(cell+.5)*cellMetres。速度保存每格三个正侧面的 MAC 面速度，单位 m/s；负外表面隐式为静止封闭墙。maxSpeed 是每个分量的上限，CFL 也按分量计算。

压力保存投影势，单位 m²/s，不是 Pascal。散度/旋度单位为 1/s。压力、散度、旋度和残差缓冲是私有临时数据；它们不申请 WF 通道。GpuBuffers 附带 Spec、Layout、sourceTick 和 version，供后续消费者明确采样布局和来源时间。

构造时先按 GetResourceAllocationInfo 核算真实对齐字段字节，再建立资源。GPU 默认字段/输入总上限 64 MiB、8 份输入快照、单步最多 4096 派发；不等于整个引擎联合预算。小 buffer 的 64 KiB 对齐计费也包括在内。9 组专项中的较大密度单通道配置实际比三通道少分配标量字节。

## 每个子步的求解

1. 按秒积分声明源项、冷却/衰减及艺术燃烧近似，并约束标量上下限；固体格清空密度/燃料并回到环境温度。
2. 对交错面速度半拉格朗日回溯，可选旋度/涡量约束，再施加源加速度、温差浮力及烟重量近似。
3. 用成对的有限体积散度与面压力梯度求解压力，采用松弛 Jacobi；固体/外墙面施加规定法向速度。
4. 输出每格投影前/后散度与 Poisson 残差，再用投影后的速度平流启用标量。
5. 可选前向/反向 MacCormack，修正按回溯位置八点上游邻域限幅。

步数由 ceil(dt·maxSpeed/(cellMetres·courant)) 决定，courant≤0.5；超过配置子步/派发预算时在录制前拒绝，不截掉剩余时间。回溯使用有界体素穿越，遇固体截断；标量插值跳过固体并归一化剩余权重，防止穿过一格厚墙。它不是 SDF cut-cell 或真实移动几何连续检测。

CPU 和 GPU 使用一致的离散 D/G 算子，因此不发生旧 collocated 中央差分压力与投影模板不一致的问题。每格 GPU diagnostics 是投影前散度、投影后散度、压力残差；只有验证程序读回这些值，生产类不做同步或异步数值读回。

涡量与 MacCormack 延续原算法方向，但独立后端改为面速度网格和上游限幅；未用改动新数值公式来声称旧 renderer 像素保持。半拉格朗日/MacCormack 不是严格质量守恒输运；静止均匀场守恒专项不代表任意运动下的质量守恒已完成。

## 火与固体的近似边界

达到 ignitionKelvin 时按 fuel·(1−exp(−burnRate·dt)) 消耗燃料，转成 sootPerFuel 和 heatPerFuel。后者是艺术控制的温升转换系数，不是完整反应化学或焓守恒。冷却到点火阈值以下会停止该反应。所有燃料、烟密度、温度和面速度有声明上限。

CellInput.solid/wallVelocity 提供本步体素障碍与规定速度，处理法向不可穿透，切向采用自由滑移近似。改变体素占据会清除新固体内部状态；尚无场景刚体/SDF 烘焙适配、固体扫掠或占据变化的守恒重建。它不等同于完整场景流体边界系统。

## 所有权、时间与真实 fence

每个 GPU 实例拥有私有标量、速度与求解临时量。setInputs 将源项/障碍/可选初态打包成不可变 UPLOAD 快照；快照通过真实 ID3D12Fence 保留，不会因 CPU 换了源数据而覆盖此前排队的 tick。已完成的活动输入可在单快照配置中被替换，实际新旧分配峰值仍计费。

输入初态只允许首次录制之前设置；运行中重置/传送/移动网格需调用者明确排空并重建，目前不提供历史搬迁。step 要求单拥有者、同设备 DIRECT/COMPUTE 命令列表、实际未完成且单调 fence、严格前进的 tick。录制后异常会使实例拒绝继续录制，防止重放半个 tick。shutdown 在实际 fence 未完成时拒绝。

GpuBuffers 为借用的可变结果，不是历史不可变视图；使用前 prepareRead 将输出准备为 NON_PIXEL_SHADER_RESOURCE，并在同一队列顺序中供 compute 消费。后续 tick 可以复用缓冲，调用者不能把旧裸指针当成永久快照。没有跨队列同步，也没有把 GPU 视觉结果写成 CPU 玩法权威。

## 增量入口与证据

```powershell
npm run smoke:gpu -- -Adapter AMD
npm run smoke:gpu -- -SkipBuild -Adapter AMD
```

默认自动选择可用硬件 GPU，-Adapter 可明确固定适配器；最终默认选择仍为 890M。独立 `build/smoke-gpu` 仅构建三个新 C++ 源和 13 个烟火 shader。新 shader 开启 ieeeStrict，CPU 使用 -ffp-contract=off。已登记标准构建目标与未来 verify 步骤；本轮只执行此专项，没有执行全量 build/test/verify/CI 或其他系统回归。

[正式报告](../reports/0.11.0/31c00bc53c0b/20261025T071418Z/smoke-gpu-evidence.json)：890M，**9 组/44672 检查/43130 次 CPU-GPU 标量/面速度比较**，实际 debugLayer=true、debugErrors=0。二进制、来源与生成 shader 在执行前冻结。包含均匀平衡、固体屏障、移动障碍、燃烧/熄灭、上下限/CFL 拒绝、可选通道与预算、真实队列输入寿命、非均匀多子步输运、单输入槽复用及重复 tick 拒绝。

| 场景 | 结果 |
|---|---|
| 固体屏障压力投影 | 散度 L2 1.20777069 → 0.00274632，比例约 0.002274；五 tick 未穿过一格厚墙 |
| 移动固体 | 16 个接触面满足规定法向速度，散度 L2 0.47296473 → 0.00071642 |
| 非均匀三维烟团 | 每 tick 3 子步，两 tick 共 452 次派发；MacCormack 与一阶密度结果最大差约 0.200812 |
| CPU/GPU 数值误差 | 本轮最大标量差 0.00012207，最大面速度差 2.3842e−7；不承诺跨 GPU 位确定性 |
| 燃烧/熄灭 | 0.5 kg/m³、burnRate=3、dt=0.02 的解析消耗约 0.0291177；降温后停止反应 |
| 真实队列 gate | 同 fence 两 tick 使用不同输入，密度为 0.01+0.03；第三快照拒绝，完成后更新到 0.13，未覆盖先前输入 |

首次八点采样完全展开的速度平流 shader 在 AMD 驱动 CreateComputePipelineState 内发生 access violation，LLDB 确认发生在第 3 个核（AdvectVelocity）的驱动编译阶段。保留八点循环后运行通过，算法与采样点未减少。失败 HLSL、该 DXIL 和调试堆栈保留；原崩溃 exe 未单独冻结，不将它描述成完整可重放失败包。早期 `dx` 与 DXC 内置命名空间冲突的编译日志也保留。

[最终来源核对](../reports/0.11.0/31c00bc53c0b/20261025T071648Z-smoke-final/smoke-solver-finalization.json) 完成 20 次当前输入比较、36 次冻结核验，含二进制及三个标准入口。新目录为 v15/96，仅增加 13 个烟火核，既有 83 个定义与基线一致；旧 fluid.hlsl、particles.hlsl 和 renderer/particles.cpp hash 不变。没有把新独立后端测试当作旧 renderer 回归或统一体积验收。

## 尚未完成的 D 阶段

后续已增加 [GPU 粒子源速率适配](SMOKE-PARTICLE-SOURCES.md)、[资产/cook 与实例服务](SMOKE-EFFECT-SERVICE.md)、[实际光学](SMOKE-RENDERING.md) 和 [密度自动共享](SMOKE-SHARING.md)。仍缺编辑器、真实求解所有权迁移、温度/速度联合共享、云烟完整混合、相机/介质时域历史、热浪、玩法烟遮挡/导航/声学、场景 SDF 与移动真实固体接入、四系统联合性能。当前发射是艺术 Kelvin 映射，不称为光谱黑体积分。D/R2 和 A–G goal 继续部分完成。
