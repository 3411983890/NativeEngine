# 共享场参考：区域、局部投影与 GPU 读取

这是 [完整开发方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) B0/B1/B2 与部分 B3/B4/B5 的数值/生命周期参考。CPU 实现为 `engine/native/fields/world.*`、`local.*`，D3D12 只读稀疏快照为 `gpu_storage.*`。布料风和水查询代理通过 `engine/native/simulation/field_adapters.*` 接入；Game tick、存档、效果宿主与既有 96³ 烟火求解器仍未接入。

## 发布与读取

`World::publish` 沿用来源 tick 和发布 tick 相同的接口。`publishAt` 显式分开 `sourceTick` 与 `publishTick`：延迟到达的数据可以发布到新快照，但不能倒写已经提交的 tick，也不能用较新的发布时间伪装数据新鲜度。来源 tick 不得回退，且不能晚于发布时间。

`publishAtomic` 对一个或多个来源/通道执行同一事务，包括撤销旧区域后在新区域写入。任何语义、权限或容量失败都保留原来的暂存内容，只增加拒绝计数。相同 source/epoch/载荷重发返回 Duplicate；载荷或发布时间不同则冲突。跨通道发布在 `commit` 后一起可见，空撤销也受发布时间屏障约束。

`sourceView` 读取已提交的来源 epoch、来源/发布时间、快照版本和驻留状态，不泄露暂存版本。`sampleSource` 读取单个来源，`sample` 读取规定合并规约下的聚合。两者都保留 Unknown/Stale/TickMismatch；缺数据不会自动变成安全的零值。

`World` 仍由单线程拥有。原子发布使用 CPU 深拷贝暂存视图，随后交换成功结果，适合数值参考；这不是 GPU 事务或高吞吐发布方案。临时拷贝与容器元数据不包含在 `committedBytes` 的 payload 统计内。`commit` 是值与来源版本的可见性屏障，租约释放仍会影响资源可用性；没有任意历史 tick 的 MVCC。

## LEF 保留求解权，WF 接收粗投影

`LocalFields` 使用效果私有的稠密整数网格，只有提升时才申请 WF 区域写租约。未共享的局部场不分配 WF 页。网格比例 `finePerWorld` 为 1/2/4/8/16，坐标是整数单元坐标，没有隐含的米制世界映射。数据按 x 最快、再 y、再 z 排列。

当前仅允许 `VisualReference` 通道，拒绝 CPU 权威通道，避免把视觉投影当成玩法真相。支持两种显式规约：

| 规约 | 条件 | 粗单元值 |
|---|---|---|
| VolumeAverage | 非负 Sum 通道，范围含零 | 细单元贡献总和除以完整粗单元体积，确定性最近取整，正数半格向上 |
| Maximum | Maximum 通道，范围含零 | 覆盖的细单元最大值 |

密度投影报告输入积分、重建积分和有符号量化残差，残差单位为“细单元整数值 × 单元数”。它只描述量化，不能推导空间混叠误差，也不能当作真实流体质量守恒验收。LEF 保留原数据，因此这不是求解所有权转移；温度热容量加权、速度质量加权、真正守恒接管、跨分辨率 halo 仍待实现。

最小用法：

```cpp
using namespace engine::fields;
World world;
world.registerChannel({"density", "fixed-density", "smoke",
    Domain::VisualReference, Merge::Sum, 0, 1000, 1000});
LocalFields local(world);
LocalSpec spec;
spec.owner = "smoke";
spec.source = 1;
spec.dimensions = {2,2,2};
spec.channels = {{0, ProjectionReduction::VolumeAverage}};
auto created = local.create(spec); // 检查 created.result.accepted()
auto h = created.handle;
local.replace(h, {{0, std::vector<std::int32_t>(8,250)}}, 1);
local.promote(h, 1);              // PromotePending
world.commit(1);                  // 一次提交服务所有生产者与消费者
local.settle(h);                  // Shared，确认来源 epoch
auto value = local.sample(h, 0, {0,0,0}, 1, Representation::SharedProxy);
local.retire(h, 2);               // DemotePending，先发布撤销
world.commit(2);
local.settle(h);                  // 代际失效，局部 payload 可回收
world.collect(0);                 // 此参考示例没有在途 GPU 使用
```

实际调用必须检查每一步返回状态。source ID 由所属生产者统一分配，在同一 owner 的发布域中唯一；控制器要求单调递增，拒绝本控制器复用及已暂存的同通道来源。局部句柄和区域租约均含 generation，陈旧任务不能修改复用的槽位。

## 移动、可见性与资源回收

状态机为 `LocalOnly → PromotePending → Shared → DemotePending → LocalOnly/Retired`。投影、移动或撤销在途时会返回 Busy，调用方应在世界提交后执行 `settle`。正常销毁先完成 retire/commit/settle；析构仅释放租约，不擅自推进或提交世界时间。

共享状态下移动需要同时保有旧/新覆盖的峰值容量。事务先用旧租约撤销该来源，再用新租约以更高 epoch 写入；不申请两地之间的空白区域。新区域容量不足时，局部位置、版本与旧投影均保持不变。旧租约在提交确认后释放，回收等待所有读者中最大的 last-use fence。这里验证的是 CPU fence 账本，尚不是实际 GPU 同步证据。

`sample(..., LocalDetail)` 与 `sample(..., SharedProxy)` 每次只返回一种表示和一个 source ID，不将局部细节与同来源的世界贡献相加。共享代理陈旧时尝试局部值，局部也陈旧则显式不可用。移动等待提交期间使用新位置的局部表示，避免误选旧位置的共享投影。这个 CPU 路由合同不等于已经实现 GPU 覆盖遮罩或体积合成。

`observe` 参考调度器按优先级降序、source ID 升序决定提升。浓度阈值分离，支持最短局部/共享驻留；订阅者与 worldVisible 都可保留共享。每 tick 的提升配额跨重复调用累计，失败会明确返回容量/预算状态。调用方应在调度阶段收齐观察请求；完整 solver DAG、权威层预算隔离与 GPU 提升调度仍未实现。

撤销墓碑由该来源最后使用的写租约代际保活，不再由“任何相同 owner 的写租约”保活。这样长时间存在的环境效果不会把同一子系统中已结束的发射器元数据一直留住。已提交的来源元数据仍保留到下一次 commit，以维护快照语义。

## 验证

```powershell
npm run fields
npm run fields -- -SkipBuild
```

专项编译只依赖场、固定时钟、确定性工具及测试，不依赖正在开发的物理模块。运行前校验全部输入和测试二进制 SHA256；修改来源后不能用 SkipBuild 套用旧结果。投影增量时覆盖 20 组、37,252 次检查，包括既有 10,000 次租约循环、100 次权威聚合顺序对拍、100 次观察请求排列对拍、1,000 次投影生命周期，以及常驻来源存在时另外 1,000 次短生命周期；下文采样增量扩至 23 组。

检查还覆盖多通道失败回滚、延迟数据的新鲜度、负坐标、量化残差、远距离移动不分配中间页、旧/新足迹的提交屏障、源级互斥采样、饱和聚合后的正确撤销、提升风暴配额与迟滞。它们不替代 GPU、画面、性能或完整生产工具验收。

## 跨砖块采样与读取 halo（2026-10-23）

`sampleLinear` 与 `sampleSourceLinear` 接受 `SamplePosition{cell, fraction}`。cell 是格点坐标，各轴 fraction 为 0–255，实际位置为 `cell + fraction/256`，没有隐含半格偏移。负坐标使用 floor。插值采用八角 Q24 权重、完整有符号累加后一次取整，半整数向远离零的方向取整，支持完整 int32 值域。

只有非零权重的角点参与查询。任一必需角点未知则整次 Unavailable；不会把缺页当零。sourceTick 取参与角点的最旧来源 tick，clamped 是参与角点的并集。Strong、Eventual、Stale 与整数采样一致；过期数据保留 Stale 状态。此接口由调用者对适合线性插值的标量/分量使用，不能据此插值类别或对象 ID。

只读 `LeaseRequest.haloCells=1` 为相邻读取保留一层单元，0 为关闭；写租约拒绝 halo。查询中心仍必须位于原 region，halo 只授予插值角点访问。相邻消费者即使覆盖所需页，也不能扩大本租约的权限。扩展坐标溢出、页预算不足或不支持的 halo 大小会在 acquire 时拒绝。

当前实现通过同一快照的相邻 16³ 砖读取 halo，所有受覆盖砖按整页计入预算。它没有为每个算子制作 18³ 副本；没有 GPU 写算子、迭代 halo 交换或多分辨率接缝。这是设计 §4.5 的只读采样增量，不能将 B3 标为全部完成。

CPU 专项新增三组，当前 **23 组、37,290 次检查**；报告：`reports/0.11.0/31c00bc53c0b/20261023T045841Z/simulation/fields-reference-evidence.json`。

## D3D12 稀疏只读快照

`GpuFieldStorage` 从已提交的 World 打包每个被租用的 channel/brick。重叠消费者共用页表、描述符与不可变快照；原生库没有 GPU 回读或队列等待。CPU 权威通道可以镜像到 GPU，GPU 没有回写 CPU 权威的能力。

每页包含值、逐单元 sourceTick、known/clamped 位图。页、上传、页表按 `GetResourceAllocationInfo` 的实际分配尺寸计费，快照并存也计入总额；descriptorSlots 单列。当前使用独立 committed buffer，不是 reserved/tiled 物理页池。最多页数、在途快照、租约、视图与总分配字节均有拒绝路径。

空间租约与视图句柄各带 generation。旧视图保留旧快照，空间租约释放后仍可完成已取得的读取。调用者必须使用同一 GPU fence 时间线，为上传与后续读取登记实际提交值；只有完成 fence 且没有视图引用时才回收。shutdown 拒绝活租约/视图或未完成 GPU 使用。

`shaders/field_sampling.hlsli` 提供共享读取函数，`field_query.hlsl` 是验证驱动。query ABI 为 32 字节（int3 cell、channel、uint3 fraction、filter），filter 0 为整数格点、1 为 Q8 三线性；answer 为 48 字节。无效 filter/fraction 返回 InvalidPosition。HLSL 用 32 位字模拟精确 64 位累加，不增加 shaderInt64 硬件依赖。

```powershell
npm run fields:gpu -- -Adapter AMD
npm run fields -- -OutputDirectory build/fields-reference
```

实机 **7 组、25,760 次检查、4,150 次 CPU/GPU 对拍**通过，AMD Radeon(TM) 890M Graphics，D3D12 debugLayer=true、debugErrors=0；报告：`reports/0.11.0/31c00bc53c0b/20261023T045142Z/fields-gpu-evidence.json`。包含跨三轴砖边界、负坐标、4,096 组分数位置、INT_MIN/MAX、signed half、零权重缺页、最旧角点新鲜度、64 位 tick、64 次 GPU 生命周期与真实未完成 queue fence 的资源保留。DXGI 当时只枚举到 AMD 与软件适配器，没有 NVIDIA 实测证据。

GPU 求解器写入、通用页池、LOD/原点迁移、渲染器适配与统一体积仍待实现。该专项的 GPU readback 只用于验证结果，不是运行时反馈路径。

2026-10-23 后续增量增加页表 SRV（描述符 pages+1），把共享 HLSL 常量片段复用到粒子速度场变体；同范围 GPU 专项重新通过 **7 组/25,760 次**，报告 `reports/0.11.0/31c00bc53c0b/20261023T100302Z/fields-gpu-evidence.json`。读取绑定、显式 fieldTick、米制网格与场诊断见 [GPU 速度场消费](EFFECT-FIELD-CONSUMER.md)。只读消费者不获得场求解权，也不会回写玩法通道。

后续宿主发布/租约/时钟回调和作者预览生产者见 [FIELD-HOST.md](FIELD-HOST.md)。缓存可以保留共享 World 所有权，CPU 生产者按需申请/撤销写需求。字段页改为 COMMON 创建，再显式进入复制状态，避免每次发布产生被驱动忽略的初始状态提示；最新共享场专项 **7 组/25760 次**通过，报告 `reports/0.11.0/31c00bc53c0b/20261023T151523Z/fields-gpu-evidence.json`。求解器写入、权威 Game 接入和统一体积仍未据此完成。

## 布料风与水体发布适配器

`FieldGrid` 显式绑定米制原点和 cellMetres；WindField 的三个通道都须声明 `m/s` 和一致的数据域。坐标量化为 floor Q8，每轴位置量化误差小于一个 cell/256；这不是速度误差界。权威模式同时要求 CPU 域、指定 committed tick、来源在该 tick 新鲜，即便调用者传入 Stale 租约也不能绕过。

`applyWindField` 在全部布料模拟顶点的位置采样，再一次替换 Solver 的 windSamples。任何顶点的任何分量缺失、过期或越界都不修改原输入，返回失败数量与首个状态，由调用者选择保留、降级或停用。常量 setWind、reset 与传送清除空间样本；digest `/2` 纳入样本。该输入在一个求解 tick 的子步中保持固定，不在子步之间隐式重读。

`publishWater` 把指定 Body 的高度、深度、X/Z 流速原子发布到一个水平格点层，只允许 `m`/`m/s` 的 CPU 权威通道。Body 仍拥有求解和水量；这是查询代理，不是质量/动量转移。Body 的本地 tick 与调度器传入的 sourceTick/publishTick 分别记录，调用者负责它们的时间映射。函数不调用 World.commit。

量化超通道范围、权限、采样预算或任一通道失败都会拒绝整批。域外样本不写入，已知零流速则写显式零。重叠水体须先由对象规则选择 Body/source，然后通过 sampleSource/sampleSourceLinear 读取四个量；不能把四个通道的聚合分别当作同一水体，也不发布可插值的 waterBodyId。发布报告分别保留量化误差和 Body 查询误差，未声称完整代理误差界。

```powershell
npm run fields:adapters
```

专项 **5 组、900 次检查**通过，含 120 tick 实际布料求解对拍、缺风/旧风/部分覆盖的整批保留、传送清理、四通道失败回滚、重叠水体 source 隔离、替换不双计与 32 次水→权威场重放。报告：`reports/0.11.0/31c00bc53c0b/20261023T045946Z/field-adapters-evidence.json`。宿主、GPU 布料/水、湿润/热量/双向质量反馈均未接入。


2026-10-24 后续：[GPU 私有 LEF 与粗投影](FIELD-LOCAL-GPU.md) 已直接消费 GPU 粒子池，并有 CPU 数值/真实 fence 对拍；只读 GPU 快照和 CPU 参考保持原合同。后续 [GPU 视觉 WF](FIELD-WORLD-GPU.md) 已增量实现分源替换/撤销、稀疏快照聚合和版本查询；GPU 页池/多级分辨率、流体及所有权迁移仍未完成。
