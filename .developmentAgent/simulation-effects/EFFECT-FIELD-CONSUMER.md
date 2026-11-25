# GPU 粒子读取共享速度场

依据 [完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) B3/B5、C1，以及 [场数据底座](FIELD-PLAN.md) 的通道租约和 CPU 权威规则。本增量实现只读风场/流场消费；场保存空间数据，粒子模块负责自己的速度响应，没有将 GPU 反馈作为玩法真相。

## 资产与响应

新增 `velocityField` 模块，参数是 `channel`（`windVelocity` 或 `waterVelocity`）与 `response`（每秒响应系数，0–10000）。固定 120 Hz 中按作者模块顺序执行：

```text
velocity += (sampledVelocity - velocity) * (1 - exp(-response / 120))
position += velocity / 120
```

位置积分在全部模块之后。风和水流可以顺序作用于同一粒子，两个名字分别绑定独立通道。作者必须在 `field.channels` 声明读取权限；原生保留 level、channels、dimensions、voxelMetres。示例资产为 `assets/effects/wind_drift.effect.json`。

此后端要求 `field.level=worldVisible`，voxelMetres 与绑定网格大小一致。dimensions 是作者声明，实际可读空间由区域租约限制，不会隐式分配整个世界。现有 `fieldCoupling` 包含不同的读写语义，仍明确拒绝，没有将密度沉积偷换成速度模块。CPU 粒子参考后端尚未接入速度场，spawn 时明确拒绝该资产。

## 显式绑定与时间

`engine/native/fields/gpu_vector.*` 将逻辑速度映射到三个标量通道，检查它们已由同一 GPU 读取租约授权，单位均为 `m/s`，三个分量属于同一 Domain。wind 与 water 两个向量可以来自不同 Domain；CPU 权威数据仅作为只读镜像。各分量分别按 `1 / unitsPerInteger` 换算，不要求量化尺度相同。

```cpp
fields::GpuVectorSpec spec;
spec.originMetres = gridOrigin;
spec.cellMetres = cellMetres;
spec.channels["windVelocity"] = {windX, windY, windZ};
fields::GpuVectorFields vectors(storage, metadataLimits);

// World 已提交，storage 已在同一图形队列记录 publish。
auto binding = vectors.bind(spatialLease, spec, fieldTick);
// 检查 binding.accepted()、spawned.status 和以下所有 bool 返回值。
auto spawned = runtime.spawn(assetId, origin, seed, binding.view);
runtime.step(commands, submissionFence, completedFence, error, fieldTick);

// 后续 fieldTick / 场快照变化时显式创建并替换绑定：
auto next = vectors.bind(spatialLease, spec, nextFieldTick);
runtime.bindField(spawned.handle, next.view, error);
runtime.step(commands, submissionFence, completedFence, error, nextFieldTick);
vectors.collect(completedFence);
```

effect age 与 fieldTick 是独立时钟。含场的活跃实例要求 step 显式提供 fieldTick，且与不可变绑定中的 requestedTick 相同；运行时拒绝倒退。多个逻辑 tick 可以明确选择同一 fieldTick，不会自动将效果年龄当作场的新鲜度。Strong 要求请求等于快照 tick；请求早于快照一律拒绝。缺少映射时，在记录本次 GPU 工作前失败，实例 tick 不递增。

空间映射为 `(worldPosition - originMetres) / cellMetres`，cell 取 floor，余量向下量化为 Q8。局部粒子先加实例世界原点；世界粒子直接使用世界位置。cellMetres 在 `[0.001,100]`，映射坐标绝对值最多 8191，非有限值和超界显式不可用。Q8 使用场查询共用的整数三线性实现，跨砖与读取 halo 共用寻址。浮点米制映射不承诺与 CPU double 的任意输入逐位一致；本次对拍使用相同 float 映射及 Q8 格点。

## 缺数据、过期与权限

采样必须得到三个完整分量；任意缺失、越出租约、无权限或不允许的过期数据都会跳过本次速度模块，保留进入模块时的速度，继续后续模块和积分。未知值不会被解释为零风。Eventual 超过 maxAge 拒绝；Stale 按租约明确允许旧值，同时记录过期使用。温度/密度等通道不能重命名成速度使用。

池原有 0–8 状态词不变，新增饱和诊断计数：

| 状态词 | 含义 |
|---|---|
| 9 | 成功应用的向量采样次数，每模块每粒子一次 |
| 10 | 不可用采样次数 |
| 11 | 超龄采样次数，含 Stale 允许的使用；结合 9/10 区分 |
| 12 | 越出租约采样中心/halo |
| 13 | 已应用采样携带来源聚合截断标记 |
| 14 | 坐标映射、访问或查询合同异常 |

生产运行时尚未异步汇总这些词到宿主报告。验证程序中的同步读取仅用于数值对拍。

## 快照、显存与 fence

场快照描述符 heap 在原页 SRV 后追加页表 SRV，数量为 pages+1；原页索引不变。读取元数据为 192 字节合同、256 字节 CBV，实际 committed allocation 在本次硬件为 65536 字节。服务默认最多三个在途绑定、196608 字节，可在构造时调整，最多 256 个绑定；分配前核对实际大小。

基础池根签名仍为 62 DWORD；含速度模块的变体新增一个 CBV，达到 64 DWORD。只有 simulate 使用新增着色器变体，其余流程保持共用代码。

`GpuVectorFields` 保留替换后的绑定直到最后使用 fence 完成。已获得的读取 view 是独立租约，空间租约释放后仍可完成在途读取；发布新快照不会改变旧 view。collect 释放无人引用且 fence 已完成的元数据，再回收旧场页。World、storage 和向量服务必须存活到在途使用结束；shutdown 拒绝活跃客户/未完成 fence，析构不隐式等待。这里只使用同一图形队列时间线，未包含跨队列所有权转移。

元数据与共享页由场服务单独计数，不重复算进粒子池的九份 buffer 或 GpuRuntime 粒子预算。调用者需要合计两类预算；统一游戏预算阶梯尚未接入。GPU 模块没有向 World 发布数据，没有求解器写入、权威回读或水体质量交换。

## 增量证据与限制

```powershell
npm run effects:fields -- -Adapter AMD
npm run effects:fields -- -SkipBuild -Adapter AMD
```

仅烘焙独立风场夹具、编译两个相关 shader 文件（9 个 blob）、运行 `tests/effects.test.mjs` 与独立 GPU 场消费测试。不覆盖宿主的完整生成头，不运行全量回归。编译/运行/归档前后核对输入和二进制 SHA，SkipBuild 拒绝过期输入。完整 shader manifest 新增一个变体，现为 58 个 blob。

AMD Radeon(TM) 890M Graphics 实机 **8 组、1659 次检查、826 次 CPU 向量对拍**通过，debugLayer=true、debugErrors=0；作者合同 **9/9** 通过。报告 `reports/0.11.0/31c00bc53c0b/20261023T102327Z/effects-fields-evidence.json` 冻结源文件、编译头、独立 cook、日志和二进制。覆盖恒定响应与积分、负坐标/跨砖/局部坐标、分量单位、缺失/过期/越界/截断、wind+water 模块顺序、权限/Strong/预算拒绝、显式时钟、新旧快照并存及真实 queue gate 下的延迟释放；CPU 权威摘要未被视觉计算改变。报告分别标明无场读回、运行时计数的有界异步读回、验证程序的同步读回，不混淆三者。

受影响共享场 GPU 专项 **7 组/25760 次**、原 GPU 池 **8 组/49842 次**、实例 **7 组/1158 次**、绘制 **5 组/604 次**均有本轮独立报告。宿主受影响路径 **26/26** 通过，包含未绑定 wind_drift 的明确拒绝；报告 `reports/0.11.0/31c00bc53c0b/20261023T102116Z/effects-gpu-draw-evidence.json`。它们不代表完整游戏画面或高负载性能验收。

后续 [宿主发布与消费](FIELD-HOST.md) 已接入：帧回调支持空间租约、已提交 CPU World 发布、显式 fieldTick 与实际 GPU 绘制，并提供作者风场预览生产者。未配置生产者时 CLI 仍明确拒绝。既有水查询代理只发布高度/深度/XZ 流速，接入三维 waterVelocity 仍需要生产者明确提供 Y 分量和完整体积覆盖；作者三分量流场不代表水体适配完成。GPU 场写求解、LEF/WF 求解权迁移、场调试与创作面板等继续按完整方案开发。

共享 World 所有权扩展后，消费专项增至 **9 组/1675 次检查/827 次向量对拍**；页初始化状态修正后重新通过的报告为 `reports/0.11.0/31c00bc53c0b/20261023T151535Z/effects-fields-evidence.json`。新增外部 World/服务引用释放后，旧 view 继续真实采样，以及最后已完成 view 释放后 World 析构的检查。该所有权形式仍要求调用者正确保留最后 GPU 使用；服务负责自动延迟回收时必须存活到回收结束。
