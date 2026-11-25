# GPU 视觉 WF：分源聚合与不可变查询

2026-10-24，B2/B4 的原生 GPU 增量。`engine/native/fields/gpu_world.*` 与 `shaders/field_world.hlsl` 消费 [GPU LEF 粗投影](FIELD-LOCAL-GPU.md)，建立按来源可替换、可撤销的视觉 WF。LEF 保留细节和求解所有权；发布粗投影不转移求解权。后续已接入 [资产写场与宿主提升](EFFECT-DEPOSIT-SERVICE.md)，统一体积仍待完成。

## 数据与发布合同

`GpuVisualWorld` 复制通道目录，只接受 VisualReference、非负、Sum/Maximum 通道。CPU 处理租约、来源元数据与页覆盖，GPU 处理来源值和聚合结果。没有运行时数值读回或 CPU 体素上传，也不会修改 CPU World。CPU 权威通道与 Strong 租约明确拒绝，不能静默降为视觉数据。

来源键为 `(owner,sourceId)`，由精确的写租约 generation 持有。多平面投影在发布前原子校验 owner、设备、实际 fence、米制网格、覆盖和完整单位/域/归约定义，失败不留下部分通道。相同 owner 的另一个写者也不能夺取来源。

`publish(writer,projection,wfEpoch,publishTick)` 暂存替换。WF epoch 与生产者局部 epoch 独立，以允许撤销后重新共享。相同 epoch/投影对象/发布时间重发返回 Duplicate；旧 epoch、同 epoch 冲突、来源或发布时间倒退、修改已提交 tick 明确拒绝。WF 发布时间不能早于投影自身的发布时间。

`withdraw(writer,source,wfEpoch,publishTick)` 撤销该源全部通道，保留有界墓碑；重复撤销幂等。释放写租约自动删除它的暂存贡献及来源声明，在下一次提交重建时生效。已经发布的旧快照保持不变，直到读者和真实 GPU fence 均结束。短生命周期租约退出会释放墓碑配额，不依赖 owner 字符串的生命周期。

`commit(list,fence,tick)` 对所有当前读/写租约的覆盖并集生成新版本；提交前检查时间、生产者 fence、来源行数、快照数和峰值内存预算。`sourceView` 返回已提交版本，未提交暂存不可见；撤销墓碑的 resident 表示元数据驻留，hasContribution=false。所有通道在同一快照发布，来源时间与发布时间分别保存。

2026-10-25 新增 tryCommit 的 Committed/Deferred/Invalid/ResourceFailure 分类，以及 [控制器发布事务](FIELD-PUBLICATION-TRANSACTIONS.md)。控制器已暂存退出的写覆盖不再占下一版本页额度，实际旧快照字节与 fence 寿命保持计费。无剩余覆盖时只提交撤销确认元数据，不生成空 GPU 场；容量失败可整批恢复来源、租约和控制器状态后稍后重试。直接 commit 继续支持既有 bool 合同。

后续新增纯 CPU 提议预算 GpuVisualWorld.budget，按区域/通道和原子撤销组预估页、写槽、来源记录、快照、投影行、描述符与真实对齐峰值，不创建投影或授予写权限。实际 commit 核对预估和布局一致；生产服务据此跳过不可承受候选，见 [逐来源预算合同](FIELD-PUBLICATION-SCHEDULER.md)。

## 聚合与稀疏覆盖

逻辑页为每通道 16³ 砖块，稳定按通道及有符号砖坐标排序。重叠租约共享砖块，支持负坐标。每个快照拥有一个 DEFAULT 原始 buffer arena，页表通过独立 SRV 切片寻址；这是按需求覆盖构造的稀疏砖块负载，**还不是 D3D tiled physical page、GPU 空闲页表或增量脏页池**。每次提交完整重建当前需求覆盖，未做跨版本页复用。

每砖包含 4096 个 int32 值、每格双字 sourceTick、known 与 clamped 位图，共 50176 字节，再按实际 committed 资源要求对齐。读取 halo 通过扩大覆盖并集提供；此处没有压力迭代使用的 18³ 独立 halo 存储或跨砖同步。

每格遍历与它相交的来源投影，非负 Sum 使用两个 uint 的 UInt64 和，所有贡献完成后才按通道上限钳位；Maximum 按声明取最大值。来源总数最多 256，量化值由 LEF 的正 int32 上限约束，累加不会 UInt64 溢出。不采用对已饱和值做减法的撤销方式。

有来源覆盖的 0 是已知零；无来源覆盖的格子保留 unknown，查询返回 Unavailable。每格保留所有覆盖来源中最早的 sourceTick，包括值为零的来源，避免新发布掩盖陈旧数据；钳位位每次重新计算。此合同只覆盖非负 Sum/Maximum，有符号速度与热容量加权温度仍需后续算子。

## 查询与实际资源寿命

`view(readLease)` 提供现有 `GpuBindings` ABI，复用已有查询 shader 和 Q8 三线性实现。视图复制区域、通道 mask、一致性与 maxAgeTicks；别的租约使通道驻留也不能绕过当前读者权限。新覆盖必须等待新提交，不能读旧快照中不存在的砖块。

Eventual 超期显式返回 Stale，显式 Stale 租约可读取陈旧表现数据；早于 snapshotTick 查询返回 TickMismatch。64 位 sourceTick、版本与 age 均保留。这里的整数归约顺序无关不意味着 GPU 粒子运动或最终图像跨设备确定。

生产者和读者使用同一设备、同一实际队列/fence；接口拒绝不同 fence 对象，即使它属于同一设备。CPU 录制端单 owner，元数据服务不支持并发修改。本模块不执行跨队列等待或生产 CPU 阻塞。

提交持有生产者不可变投影、工作行与临时上传资源，直到聚合 fence 真正完成。完成后可销毁生产者，WF 仍由自己的 arena 提供查询。外部视图在使用前调用 `markUsed(fence)`，快照最后读取 fence 完成前禁止回收。释放空间租约不使已取得的读取视图失效；shutdown 会拒绝未结束的租约、视图和在途快照。析构不能替代调用者的队列完成协议。

没有租约时提交不分配 buffer、不派发计算；最后租约退出后，资源在视图和 fence 允许时归零。仅一份快照额度也可前进：旧 current 已完成且无读者时，新版本分配成功后原子替换；峰值预算仍计算旧+新，不在原地覆盖读者数据。分配失败保留原版本。

默认上限：128 个需求砖块、8 个驻留快照、256 租约、256 视图、128 来源记录、65536 条来源/页相交行、64 MiB。配置硬上限分别为 4096/32/4096/4096/256/1048576；覆盖在枚举前做范围和数量验证，巨大区域直接拒绝。不存在静默缩小区域或降分辨率。

预算分别报告 DEFAULT、UPLOAD 实际对齐 committed 字节，以及描述符数量×设备 stride 的**估算**。所有活跃/在途/读者保留快照一起计入峰值。生产者投影的借用资源由 LEF owner 计费，不重复加入 WF 数值；全引擎联合预算接入仍待完成。

## 增量验证与证据

```powershell
npm run fields:world:gpu -- -Adapter AMD
npm run fields:world:gpu -- -SkipBuild -Adapter AMD
```

入口仅构建相关 WF/LEF、粒子池、CPU 参考和查询依赖，独立使用 `build/fields-world-gpu`。编译 2 个新 WF、5 个 LEF、8 个池、1 个查询 shader；完整清单为 v15/82 blobs。SkipBuild 会拒绝来源、生成头文件或二进制哈希变化。标准构建仅登记目标，本轮没有运行完整构建或全量回归。

正式 [WF 报告](../reports/0.11.0/31c00bc53c0b/20261024T100133Z/fields-world-gpu-evidence.json)：890M，实际 D3D12 debugLayer=true，**8 组/1124 检查/125 次 CPU World 查询比较，debugErrors=0**。冻结源、生成头文件/清单、二进制及编译/运行日志。生产代码无读回；验证程序显式读回测试答案。

- 两源饱和后替换、撤销、重发，旧快照逐字保持；三路 INT32_MAX 累加跨越 UInt32 后无回绕，撤销重建清除旧饱和位。
- 格点与 Q8 三线性、负坐标/跨砖、缺贡献、覆盖外、通道权限、64 位来源时间和年龄、延迟发布、未来版本拒绝与 CPU World 对拍。
- 真实队列 gate 下保留输入、输出及退休 LEF，完成后释放生产者仍能查询 WF；没有用调用者伪造 completedFence。
- 无订阅零分配/零派发、重叠租约页共享、反序发布结果一致、单快照前进、保留读者阻止覆盖、内存/行/来源/页/租约/视图容量拒绝、坏设备时间线/单位/网格/权限原子拒绝。
- 64 次短生命周期写者发布/撤销/退出，持久来源继续存在，来源记录不持续增长；代际复用拒绝旧句柄，所有读者退出后资源归零。

因 `GpuLocalProjection` 增加设备与实际 timeline 身份，相关 [LEF 回归](../reports/0.11.0/31c00bc53c0b/20261024T100140Z/fields-local-gpu-evidence.json) 重新构建通过：**9 组/2363 检查/274 次 CPU 粗单元比较，debugErrors=0**。

首次参考对拍在写租约释放后发现生命周期语义不同：CPU World.release 仅释放页引用，读者还在时保留贡献；GPU 接口约定自动撤销写能力。测试参考侧显式执行等价 withdraw，再 release/collect；没有改 CPU World 或放宽 Unavailable 断言。原始失败源/二进制/日志保存在 `reports/field-world-gpu-first-comparison-20261024/`。

## 仍需完成

可观察性迟滞/配额/稳定优先级控制器、资产写场模块与宿主调度、覆盖遮罩下 LEF/WF 去重合成、GPU 空闲/脏砖和多级分辨率、移动 LEF、流体算子与迭代 halo、真实求解权迁移、加权/有符号通道、玩法权威集成/存档、统一体积与完整联合性能预算仍未完成。原 A–G goal 保持 active，不以本专项替代全方案验收。


2026-10-24 后续：[GPU 可观察性共享控制器](FIELD-SHARING-GPU.md) 已加入原生状态机、迟滞/驻留、稳定优先级与同 tick 提升配额、精确提交确认和退出清理。上文“可观察性控制器仍需完成”是本模块首次交付边界；资产/宿主自动调度、实际 GPU 浓度观察和覆盖去重体积仍待完成，未将 B 或全方案标记完成。
