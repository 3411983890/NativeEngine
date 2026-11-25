# 通用粒子 GPU 池与实例：接口与当前边界

- 日期：2026-10-23；依据 [完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) C0/C1/C2。
- 实现：`engine/native/effects/gpu_pool.*`、`gpu_runtime.*`、`shaders/effect_pool.hlsl`。
- 状态：真实 D3D12 GPU 模拟、视觉实例管理及可选宿主 GPU 绘制路径。默认宿主仍使用 CPU Runtime + GPU sprite；显式 `--effect-backend gpu` 采用新路径，绘制合同见 [EFFECT-GPU-RENDERING.md](EFFECT-GPU-RENDERING.md)。

## 数据与 GPU 工作流

每池七个 SoA 属性按当前 sprite cook 布局分配：position float3、velocity float3、age uint、lifetime uint、spawnId uint、color float4、size float2。各段按 16 字节对齐，capacity=3 的总净负载为 224 字节；正常大容量约 60 字节/粒子。没有每帧修改 stride。

另有两份存活索引、一份空闲索引、GPU 计数、DISPATCH 参数和 DRAW 参数。模块与曲线是构造时上传的只读数据。所有九个 buffer 的实际 committed allocation 都通过 GetResourceAllocationInfo 计入 maxAllocationBytes，包括上传与分配对齐；管线/根签名的驱动内部内存不在这个 buffer 统计内。构造前检查完整预算，失败不留下半个可用池。

一次 step 固定为 1/120 s：

1. GPU 读取 alive count，生成模拟 DISPATCH 参数；通过 ExecuteIndirect 只处理该存活列表。
2. 老化、模块和积分；存活粒子紧致写入下一份列表，死亡索引进入空闲列表。
3. GPU 在空闲容量内预留本 tick 的发射量，生成发射 DISPATCH 参数；同 tick 死亡的槽位可立即复用。
4. ExecuteIndirect 发射并追加存活索引，最后生成 `{6, aliveCount, 0, 0}` 绘制参数。

GPU 工作量由真实存活/空闲计数决定。池没有队列 wait 或 CPU readback，不用 CPU 获取当前粒子数来控制模拟。实例层另有完成后读取的有界异步遥测，只用于诊断与排空回收。计数、数据和参数之间有 UAV barrier 与 INDIRECT/SRV 状态转换。该实现与 legacy 粒子的 configured-capacity 派发是两条不同通路。

## 支持的模拟与调用合同

- 点、球体积、锥、盒、线发射；生命周期区间，局部/世界坐标。
- initialize、acceleration、drag、colorOverLife、sizeOverLife、现有 CPU 参考形式的 curlNoise；曲线支持多个节点和精确节点时刻。
- GPU 紧致顺序不承诺确定性；seed 由资产/实例/发射器身份派生，再由 64 位请求序号生成随机流。HLSL 用 32 位字模拟 splitmix64，保留与 CPU 的整数随机值一致。
- CPU 调用者必须按完整 requested 推进 sequence，包括被预算/容量丢弃的请求；不能按 accepted 推进。接口拒绝 64 位序号回绕。spawnId 属性保存低 32 位序号，不代表已实现完整跨回绕稳定对象 ID。
- emitted/expired/dropped/faults 使用 32 位饱和统计，不回绕伪装成较小值；长期精确 64 位遥测仍待实例/统计层补充。

```cpp
GpuParticlePool pool(device, emitter, capacity, limits);
pool.initialize(commands, submissionFence, error);
auto seed = determinism::deriveSeed(
    instanceSeed ^ assetSeed ^ determinism::mix64(instanceSerial), emitter.id);
pool.step(commands, submissionFence, age, requested, sequence, seed, origin, error);
pool.prepareDraw(commands, submissionFence, error);
auto binding = pool.buffers(); // 属性、当前存活索引、GPU 绘制参数与布局
```

实际调用必须检查返回值。调用者拥有 device、command list 和同一提交 fence 时间线，保证池存活到最后使用完成。可以在一个命令列表中记录多个 tick；不记录 step 即保持粒子状态。clear 为立即清空，正常停止发射由 requested=0 排空。

prepareDraw 只准备绑定与状态，没有调用绘制。localSpace 的 origin 由绑定携带，move 不改写局部粒子的模拟坐标；世界粒子出生后不随 origin 移动。下一次 step 恢复计算状态。后续读取/绘制必须登记使用 fence；shutdown 对未完成 fence 返回失败。析构不隐式阻塞 GPU，调用者必须履行此生命周期合同。

## 实例、共享预算与异步回收

`GpuRuntime` 接受已验证的原生 Asset，注册时复制为不可变快照。安装后的同 ID 不同 hash 替换会明确拒绝；暂不支持活跃 GPU 参数热迁移。GPU 事件、通用场写入和 lit/non-sprite 等未接入合同返回 UnsupportedFeature，不吞掉声明继续运行。只读 wind/flow 的 `velocityField` 模块已有显式绑定接口，见 [速度场消费合同](EFFECT-FIELD-CONSUMER.md)；无绑定仍拒绝。

实例支持固定 120 Hz 的整数 rateMilli、burst、循环、暂停/恢复、移动、优雅停止与立即停止。generation 区分复用槽；创建/分配失败不消耗实例序列，不发布半个实例。请求序号按完整请求量递增，预算不足不会改变未来随机值。不同的渲染提交可以合批多个 tick，变速/距离发射/预热/骨骼挂点尚未实现。

每实例和全局各有一份 32 字节 GPU 账本：alive、remainingSpawns、emitted、expired、dropped、faults、killed、reserved（uint32；累计统计饱和）。调度器先初始化，再每 tick 重置发射余量，先模拟所有实例，再按槽序/作者顺序发射。发射预留取池空位、实例/全局剩余存活容量和发射余量的最小值，余量按接受数递减。共享资源之间使用 UAV barrier，不由读回计数控制 admission。

直接组装共享池时，传入不同的 instanceBudget/globalBudget，并对每份账本每 tick 调用一次 prepareBudgets；然后所有 simulate 完成后调用每池 spawn。半 tick 状态不允许 draw/clear/再次 simulate。从共享账本移除仍存活的池前，必须 clear 并等待其 fence；析构不会自动向队列记录资源清理。池的 allocationBytes 不重复包含外部共享账本，Runtime 汇总时只计各账本一次。

实例层始终分配三份 `(maxInstances+1)*32` 字节的 readback buffer，按实际 committed allocation 对齐统计。collect 只映射调用者提供的 completedFence 已完成的槽，满环则跳过本次遥测；没有 queue wait。自然停止只有在已完成的实例快照 alive=0 且 generation/出生序列均匹配时才回收。相同提交 fence 内也可能先采集空快照再发射，不能仅比较 fence 判断快照新旧。

立即停止立刻使句柄失效，flush、step 或 prepareDraw 记录 GPU clear 并归还共享存活额度；资源在最后使用 fence 完成后释放。clear 待执行或在途资源仍计入 maxAllocationBytes，新实例可因此返回 Capacity。readback/绘制的使用 fence 也必须登记；调用者不得把新工作登记到已完成的 fence。析构仍不隐式等待。

```cpp
GpuRuntime runtime(device, limits);
runtime.install(asset);
auto spawned = runtime.spawn(asset->id, origin, seed);
// 检查 spawn status 以及下面所有 bool 返回值。
runtime.step(commands, submissionFence, completedFence, error);
runtime.prepareDraw(commands, submissionFence, error);
auto batches = runtime.drawBatches(); // 借用 GPU 绑定；调用者仍须实际绘制
// 提交后，未来帧收集已完成的 fence：
runtime.collect(completedFence);
```

Runtime 内存上限覆盖每池九份 buffer、每实例账本、全局账本与三槽 readback 的实际对齐分配，包括等待 fence 的旧资源。每发射器目前独立创建 PSO，未提供管线共享缓存、GPU 时间归属或完整帧性能门禁。

## 证据

```powershell
npm run effects:gpu -- -Adapter AMD
npm run effects:gpu -- -SkipBuild -Adapter AMD
npm run effects:gpu -- -RuntimeOnly -Adapter AMD
```

2026-10-23 AMD Radeon(TM) 890M Graphics 实机专项扩至 **7 组、49,793 次检查**通过，D3D12 debugLayer=true、debugErrors=0。报告：`reports/0.11.0/31c00bc53c0b/20261023T053350Z/effects-gpu-evidence.json`。此前六组 41,592 次的报告保留为历史范围，不等于覆盖新增的一小时寿命回归。其中包含：

- 257 容量的满池/丢弃、同 tick 回收再发射、列表不重复/不丢槽、GPU 绘制计数一致。
- 388 次 CPU/GPU 粒子状态对拍，覆盖半隐式积分、重力、阻力、噪声、多节点颜色曲线和大小曲线，位置/属性容差 2e-5。
- 五种形状各 129 粒子，按请求序号匹配重复结果，跨 2³² 序号的整数随机生命周期对拍。
- UINT_MAX 请求、统计饱和、局部/世界移动、序号溢出拒绝、clear、绘制/计算状态切换。
- 4,097 个完整一小时生命周期区间样本，跨 2⁴⁰ 请求序号，对拍 CPU double 区间映射。旧 GPU float 乘法会把接近整数的值向上舍入，实际复现多一 tick；现采用精确整数乘积再右移 24 位，与 CPU 的 floor 结果一致。
- 4,097 容量下 128 tick 回收，以及真实 queue gate 未完成时拒绝释放、完成后释放全部池 buffer。

池测试中的同步 readback 只用于验证；Runtime 使用的异步读回按上节合同执行。独立工具记录 shader、源文件、测试、编译器与二进制 SHA，SkipBuild 拒绝过期输入。只编译 `effect_pool.hlsl`，输出独立 header，不覆盖完整宿主的生成头；加入只读速度场变体后现为八个 blob，旧报告的七个计数保留。

寿命缺陷的修正前可执行文件、旧着色器/字节码与失败日志已冻结在上述版本报告的 `regressions/lifetime/`，`lifetime-regression.json` 连接修正后的结果；没有只保留通过结果。

实例增量最新实机证据为 `reports/0.11.0/31c00bc53c0b/20261023T060229Z/effects-gpu-evidence.json`：池 **8 组/49,842 次检查/388 次 CPU 对拍**，实例 **7 组/1,158 次检查/219 次 CPU 对拍**；均为 AMD 890M、debugLayer=true、debugErrors=0。实例覆盖两层预算与跨实例先回收再发射、rate/burst/循环/暂停/移动/停止、同 fence 旧空快照、三槽耗尽、真实 queue gate 下拒绝释放、在途内存拒绝新分配、generation 复用；混合压力为 128 实例、18 tick、累计 175 次创建。报告目录冻结两个二进制、编译日志和 shader 清单。没有运行全量回归，也未据此声称宿主绘制或长期性能通过。

## 尚未完成

独立实例管理及共享预算已实现；GPU 活跃实例热重载、完整时间策略、宿主编辑器命令桥仍缺。宿主可选 GPU 模式经帧回调按游戏 tick 驱动，设备重建时视觉 seed 重启。

GPU SoA 绑定已经进入实际绘制，并对 GPU 精灵集合跨实例/贴图排序；受光/软精灵、其余渲染器未增加。碰撞、事件队列、网格表面采样、场写入、扩展属性与活跃实例 GPU 热迁移仍缺。只读速度模块已有 [宿主发布/绑定入口及作者预览源](FIELD-HOST.md)，玩法权威、场景组件与编辑器生产者仍待接入。传入 mesh 发射、fieldCoupling 或需要额外属性的 renderer 会明确拒绝，不会静默替换成点/sprite。
