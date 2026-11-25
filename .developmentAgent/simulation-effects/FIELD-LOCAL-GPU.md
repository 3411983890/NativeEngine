# GPU 私有 LEF 与不可变粗投影

2026-10-24，B2/C 场写入的基础增量。实现位于 `engine/native/fields/gpu_local.*`、`shaders/field_local.hlsl`，并直接读取已有 `GpuParticlePool`。这是 GPU 私有量化场与投影生产者，尚不是 GPU WF 聚合器、流体求解器或求解权迁移。

## 数据与接口

`GpuLocalFields` 复制通道目录，拒绝 CPU 权威域、错误 owner、负值通道和不匹配的归约。当前支持非负 Sum 的 VolumeAverage 与 Maximum 通道的 Maximum；温度热容量、速度质量加权及有符号场仍需后续算子，不能用本模块冒充。

`create(LocalSpec)` 按需分配局部 SoA：uint 累加平面、已钳位 int32 值平面、32 DWORD 诊断。构造管理器只创建管线，不提前分配通道 buffer。句柄带 generation；source ID 单调且不复用。默认 32 个活跃实例、1M 个局部通道样本、64 MiB committed buffer 上限；显式可配置到 256 实例、16M 样本，每轴最多 128。

调用顺序为 `create → begin(sourceTick) → splat* → resolve → project(publishTick)`。每步检查返回值；拒绝不会偷偷清除已完成状态或推进版本。`begin` 不允许覆盖尚在累加的 tick，新的源 tick 严格增加。只在 resolve 后暴露可读 buffer；外部读取须注册 `markUsed(handle,fence)` 并维护 UAV 资源状态。

`splat` 直接读取 GPU 粒子池的 attributes、alive 列表和 GPU 存活计数，实际数量不回到 CPU。调用会先让池进入读取状态，读取后恢复 counter 的 UAV 状态；后续池模拟仍能恢复其自身资源。它尊重池的 local/world space 和原点，不上传 CPU 计算的细场数组。未完成 simulate/spawn 的池被拒绝。

一个输入量是作者声明的整数场 quantum，按粒子所在细单元沉积，不是自动推导的物理质量。位置变换为 `(worldPosition-gridOrigin)/worldCellMetres*finePerWorld` 后 floor，支持负坐标。局部网格内未注入处定义为空密度 0；粗单元中不属于该 LEF 的体积贡献按该源的 0 计入完整平均体积。这不代表 WF 缺页可以当作零。

## 有界累加与损失账本

每通道每 tick 在记录命令前证明所有已接受批次的 `capacity × quantum` 总和 ≤UINT32_MAX。这里使用池容量上界，可能保守拒绝实际很稀疏的批次；不会通过同步回读 alive count 放宽准入。核内按真实存活数工作，损坏的超容量计数被限制并报告，坏 slot、非有限/超坐标位置不会读取越界。

所有整数贡献先完整累加，再在 resolve 中按声明通道上限统一钳位。量化前的 GPU 浮点运动/位置映射不保证跨硬件确定性；相同量化贡献集合的整数和与执行顺序无关。无需 64 位原子。

| 诊断 word | 内容 |
|---|---|
| 0–4 | 接受沉积数、覆盖外数、无效输入数、被钳位细胞数、损坏计数事件 |
| 5 | 诊断计数饱和标记，计数不回绕 |
| 8–15 | 每通道 resolve 钳位损失 quantum |
| 16–23 | 每通道覆盖外损失 quantum |
| 24–31 | 每通道无效输入损失 quantum |

每通道的损失总量也受同一 UInt32 上界保护；跨通道总计数使用饱和 CAS。损坏 GPU 存活计数产生明确错误事件，不把虚构的超容量条目当成已知粒子质量。

## 粗投影与所有权

比例支持 1/2/4/8/16；粗覆盖使用负坐标 floor 除法。VolumeAverage 按完整粗单元体积求和、最近取整，Maximum 使用覆盖的最大值。边界不截短平均分母。GPU 与现有 CPU `LocalFields` 使用同一规约。

投影使用分组归约和两字 uint64 运算，统计已 resolve 的局部输入积分、重建积分和有符号量化残差；不需要 shaderInt64 原子。Maximum 的积分/残差为 0，表示不适用。积分单位为“细单元整数值 × 单元数”，既不包含之前的钳位损失，也不是空间混叠或物理流体守恒误差。前述损失账本单独保留。

`GpuLocalProjection` 不可变，包含：

- owner、source、epoch、localVersion、sourceTick、publishTick；发布时间不能伪装来源时间。
- 细/粗覆盖、米制网格、比例，以及每平面的完整通道单位/域/上限/合并元数据。
- 每通道粗值 SRV、32 字节 GPU 归约指标，以及该源版本的不可变诊断副本。

相同局部版本/发布时间重复请求会复用现有投影而不重复派发；LEF 保留自己的细数据。后续 tick 可以重写局部累加器，但旧投影及旧损失计数不变。`maxWorldPages` 目前只检查未来粗覆盖的 16³ 砖预算，尚未实际申请 WF 页；输出仍是稠密粗数组，不能将其计为 GPU 稀疏 WF 已完成。

所有输出分配/预算检查完成后才记录投影命令，失败不发布部分通道。当前协议没有把这些输出写回 CPU World，也没有形成 GPU WF 的分源重建账本、覆盖遮罩或 Strong 查询版本。

## Fence 与内存

管理器拥有真实 `ID3D12Fence`，校验设备、列表类型、未来且单调的 fence。生产者、LEF 和消费者须使用同一队列时间线；本模块不推测跨队列依赖、不等待 GPU、不读回值。

每次借用粒子输入保留 attributes、alive、counter 和 drawArgs 四个资源引用，包括 prepareDraw 记录的 drawArgs barrier；生产者仍负责保留自己未完成的模拟命令所需对象。输入引用、退休局部实例和投影缓存在真实 fence 完成后回收。退休句柄立即失效，旧/新代际可并存且共同计费。投影视图另有 `markUsed(fence)`，外部视图或在途工作会阻止 shutdown；析构不代替调用者的队列完成协议。

预算按 `GetResourceAllocationInfo` 的对齐字节计入两个局部平面、诊断、各投影值/分组 scratch/指标和诊断快照。借用的粒子资源由其原 owner 计费，不重复计入。driver 的 PSO/root/scratch 元数据不属于此 committed buffer 数值。

超过一维 Dispatch 的 Clear 使用二维组索引，受维度/样本预算约束，不裁剪高通道平面。默认最多 16 个在途投影视图、128 个输入批次；全满时明确拒绝。

## 增量验证

```powershell
npm run fields:local:gpu -- -Adapter AMD
npm run fields:local:gpu -- -SkipBuild -Adapter AMD
```

只构建本模块、粒子池和 CPU 投影参考依赖；专项编译 5 个新 LEF shader 和 8 个已有粒子池 shader。完整清单版本 14、80 个 blob；本轮没有运行全量回归、完整 native/JS/CI、`npm test` 或 `npm run verify`。

正式报告：[fields-local-gpu-evidence.json](../reports/0.11.0/31c00bc53c0b/20261024T045636Z/fields-local-gpu-evidence.json)。冻结 34 项输入、生成 shader/header、测试二进制和空警告编译日志。890M，实际调试层开启，**9 组/2363 次检查/274 次 CPU 粗单元比较，debugErrors=0**。

- 5 种比例、负坐标、非零米制原点和 local/world space。真实 GPU 池产生 79 粒子，每通道 41 个落入覆盖；同通道多批次交换顺序，GPU 细值与整数 oracle、粗值/积分/残差与 CPU LocalFields 一致，源粒子数据保持原样。
- 128³×3 通道、6291456 个局部样本，验证第二维 Clear 真正清除了旧高通道贡献。输入积分 **4294966784**，重建积分 **4294967296**，残差 **−512**，验证 UInt64 进位与负残差；下一 tick 清空不修改旧投影或诊断。
- GPU 输入计数/索引/NaN 的验证性故障注入，pending 输入拒绝后重试、累计 UInt32 边界拒绝、计数饱和不回绕、钳位/覆盖外/无效损失分离及不可变诊断通过。
- 真实队列 gate 阻塞下，借用资源保留、未完成 shutdown 拒绝、源对象结束后读取、退休/复用代际和外部投影视图寿命通过。对齐内存、样本、投影覆盖、视图和输入批次预算失败均无部分发布。

最初边界夹具误将既有 Box.extent 当半宽，未生成覆盖外粒子，断言拒绝；纠正夹具尺寸而非删除覆盖断言，原日志和测试源保存在 `reports/field-local-gpu-boundary-fixture-20261024/`。

仍需 GPU WF 分源聚合/撤销/稀疏页与查询、LEF 移动和多分辨率 halo、所有权迁移、加权温度/速度、流体求解、资产模块与宿主调度、统一体积及联合预算。A–G 原目标继续 active，本专项不证明这些目标已完成。


2026-10-24 后续：[GPU 视觉 WF](FIELD-WORLD-GPU.md) 已消费这些投影实现分源替换、撤销、稀疏快照聚合与版本查询。投影新增设备及实际 timeline 身份，LEF 专项重新构建通过 9 组/2363 检查/274 次 CPU 对拍（正式 reports/0.11.0/31c00bc53c0b/20261024T100140Z/fields-local-gpu-evidence.json，debugErrors=0）。以上“尚无 WF”表述是本模块首次交付边界；本次完整 shader 清单为 v15/82，仍只选编 13 个 LEF/池 blob，未运行全量回归。GPU 页池、多级分辨率、流体、迁移及宿主资产/统一体积集成仍待完成。


2026-10-24 后续：[认证粒子场沉积模块与原生实例驱动](EFFECT-FIELD-DEPOSITS.md) 已保留作者整数 quantum，并通过显式 GPU runtime 回调连接真实池/LEF/WF。默认 renderer/宿主生产驱动、完整资产映射和体积接入仍待完成。
