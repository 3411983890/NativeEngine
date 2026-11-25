# GPU 粒子到烟火源项

2026-10-25，D1 的原生输入适配。`engine/native/smoke/particle_source.*` 直接读取现有 GPU SoA 池的 alive 列表、位置和计数，生成不可变 `GpuSmokeRateView`；`GpuSmokeFireSolver::step` 可接收同 tick 的速率场。生产路径不等待队列、不回读数值，不向 CPU 权威场写入。

## 单位与离散

每个 batch 声明三个非负整数 quantum，顺序为 density、temperature、fuel；各自除以 `unitsPerSource` 得到每个存活粒子的每秒速率。密度/燃料的源贡献单位为 kg/s，温度源贡献为 K·m³/s（艺术控制的热体积速率，不是热量或焓）。源落入所在体素后，三者均除以体素体积，得到求解器需要的 kg/(m³·s)、K/s、kg/(m³·s)。没有把 legacy 无量纲 heat 解释成 Kelvin。

GPU 先在每通道 uint32 SoA 中作整数原子归约，再统一转换为 float 速率。不读取粒子颜色/大小代替质量或热量，也不将粒子当前分布直接覆盖成烟场状态。粒子即使静止，每 tick 仍按声明速率持续注入；同一个输入速率在各子步乘以子步 dt，总注入量对应本 tick dt。

输入与 `CellInput` 的 CPU 速率相加，反应/平流/上下限仍由烟火求解器处理。固体格按求解器边界合同忽略源项；生产器的 accepted 只表示已进入速率场，不等于最终保留的烟质量。现有平流/钳位也不承诺严格质量守恒。

位置以米解释，只有 local-space 粒子额外加池的 origin；world-space 粒子位置已是世界坐标，不随后续 origin 移动。体素用 `floor((position-gridOrigin)/cellMetres)`，采用最近所属单格注入，没有额外插值核。网格外和非有限位置均拒绝，不钳到边缘。

## 录制与寿命

1. 完成所有粒子池的 simulate/spawn；然后在同一个设备、队列与真实 fence 时间线上调用 `produce`。
2. 每次提交完整 batch 集合；同一次不允许重复池，每个生产 tick 严格递增。空集合生成已知为零的速率场，表示停止新的输入，烟场历史继续演化。
3. 将视图传给 `solver.step(..., error, view)`。设备、同一 fence 对象、sourceTick、dimensions、channels、cellMetres、origin 和 SoA 布局必须吻合；燃烧参数可以独立调整。旧 tick 视图、不同网格或不同时间线在求解器录制/默认上传之前拒绝。
4. 视图可在生产完成后被延后消费，但消费 fence 不能倒退。求解器自行登记使用并保留视图到消费 fence 实际完成。额外消费者须调用 `markUsed`，并自行持有视图。

每份视图拥有独立的 accumulator、rate 和 diagnostics 资源；生产器在任何命令之前持有 attributes/alive/counters/drawArguments 四个真实池资源引用。实际生产 fence 完成后可释放这些输入及 accumulator；即使粒子池随后销毁，已生成输出仍可被消费。最后外部引用释放且实际最后使用 fence 完成后才回收输出。输入在录制时读取，同队列后续池写入由其现有状态转换恢复，不发生并发 CPU 修改。

`shutdown` 拒绝在途工作或未释放的外部视图。析构不能代替排空合同；调用方必须让生产器和求解器存活到 shutdown 成功。中途录制异常会标记 producer failed，保留已经触及的资源，拒绝重试；提交已录制命令、等待真实完成后排空重建。元数据/预算预检拒绝不使实例失败。

## 上界与诊断

生产前按池容量而非 CPU 猜测的 alive 数量验证：每通道 `sum(capacity*quantum) <= UINT32_MAX`；同一单格最坏速率不超过 1,000,000。这样原子累加和损失计数不会溢出。只启用所声明的 smoke 通道，向禁用通道输入非零贡献会拒绝。每个 batch 不得三通道全零，应直接省略该源。

limits 限制帧快照数、每帧 batch 数、单池容量和真实 D3D12 对齐分配字节；现存输出、未完成输入阶段的 accumulator 都计费，借用的池内存不重复计费。求解器额外限制在途源视图数量；这还不是全引擎联合 GPU 时间预算或自动质量降档。

diagnostics 是 12 个 uint32（与 rate 一样保持不可变）：

| word | 意义 |
|---|---|
| 0 | 进入有效网格的粒子数 |
| 1 | 网格外粒子数 |
| 2 | 非有限位置或非法 alive slot 数 |
| 3 | alive count 超过容量的 batch 数；仅扫描安全容量内记录 |
| 4–6 | 网格外损失的 density/heat-volume/fuel 原始 quantum |
| 7 | 保留，零 |
| 8–10 | 非法位置/slot 损失的三通道原始 quantum |
| 11 | 保留，零 |

这是独立源速率场的诊断，不是最终烟火状态损失报告。元数据 stats 仅记录录制次数、资源字节与引用等 CPU 已知量，没有用回读实现生产准入。

## 增量入口与范围

`npm run smoke:particles` 独立构建 `build/smoke-particles`，只编译 smoke、smoke_particles、effect_pool 所需的 24 个 shader；`npm run smoke:gpu` 回归不传源视图的原独立求解器。二进制、递归代码输入及生成 shader 在执行前冻结，运行变化另记。标准 build/verify 只注册未来入口，本批没有运行全量门禁。

专项覆盖：三通道单位与 CPU 参考、local/world 移动、旧快照不变、空源保留历史、旧 tick/重复池/禁用通道/溢出/速率/预算拒绝、网格外损失、同 fence 两 tick、真实队列阻塞、延后消费与池提前正常销毁、部分录制失败，以及三子步按速率积分。来源与最终报告见 [开发记录](SIMULATION-DEVELOPMENT.md)。

后续已有 [烟火效果资产/cook 与原生实例服务](SMOKE-EFFECT-SERVICE.md)，包含预留、暂停、退休及尾烟；也已有 [实际宿主调度与兼容/froxel 光学接入](SMOKE-RENDERING.md)。已有 [密度自动共享与按源选择](SMOKE-SHARING.md)；温度/速度联合共享、任意网格投影、雾云烟完整混合、动态时域/热浪和玩法适配仍缺。D 和全部 A–G 继续为部分完成。
