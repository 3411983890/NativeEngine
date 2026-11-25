# 粒子写场准入、预留与增量容量恢复

2026-10-25。[生产服务](EFFECT-DEPOSIT-SERVICE.md) 的增量合同，范围是 GPU 粒子快照写入视觉 LEF/WF；完整方案 A–G 继续实施。

## 发射前预留

GpuRuntime 的可选第四个回调 GpuFieldDepositAdmission 接收拟分配 serial、不可变资产和实际原点。写资产通过检查并取得 GpuDepositPermit 后，才分配实例预算及粒子池。实际 renderer 同时配置写驱动和准入回调；单独接入第三个写驱动参数的旧调用仍需自行管理其准入，不能据此承诺预留。

GpuFieldDeposits 按实际原点的 floor/ceil 网格、通道数量、GetResourceAllocationInfo 对齐、未回收代际及其他未兑现预留计算容量。GpuLocalFields.estimate 与 create/project 使用同一份不可变 footprint；估算本身不创建场数据 buffer。mutableBytes 包含两份量化场及诊断，projectionBytes 包含每通道粗数据、归约暂存、指标与诊断。worldPages 是投影覆盖的逻辑 16³ 砖块数，不表示已经分配 WF 页。

拒绝网格/通道等不支持能力返回 UnsupportedFeature，累计资源容量不足返回 Capacity；失败不推进实例 serial 或 slot generation，不创建该实例的粒子池，也不使录制器进入失败状态。效果库里未选中的后端不支持资产可以与可运行资产共存；认证或结构损坏仍然拒绝加载。

permit 只持有服务的弱引用。粒子池分配回滚、未执行实例取消、实例立即停止/自然退休会释放 CPU 预留；真实在途粒子与场资源仍独立按 fence 计费。正常 shutdown 拒绝尚有预留，防止服务先退出后继续分配。服务与运行时仍按单录制拥有者约定使用，不提供并发准入。

## 移动的原子性

move 先计算对齐新范围及 world-space 已有扫掠范围，再检查旧/新代际共存的样本、字节、实例槽和必要粗投影容量。成功后才更改预留、实例原点和各池原点；拒绝或回调抛出 std::exception 时返回 false，记录 moveRejected/lastAdmissionError，原位置及源保持有效。

已 Shared 来源重建继续采用同次提交的旧源撤销/新源发布。它只重建粒子快照，不是一般流体历史迁移。move 成功后如果 GPU 仍持有其他资源，写入可能延迟；预留不表示任意时刻都可立刻提交。

## 输入与投影容量恢复

粗投影不得占用已承诺给尚未创建私有场的容量。新提升延迟记 deferredPromotions，已有共享源更新延迟记 deferredUpdates；旧快照仍可供读取，查询使用实际 sourceTick 计算陈旧年龄。

同一 fence 的多 simulation tick 可能耗尽 LEF 输入引用批次。服务在 begin/clear 前计算该源全部 fieldDeposit 模块的批次数；不足时跳过整次写入并记 deferredWrites，避免只注入一半发射器。粒子模拟仍前进，旧 LEF/WF 来源时间不刷新；fence 完成后 collect 回收输入引用，后续 tick 从当前粒子快照追上。

报告区分 reservations、reservedMutableBytes、reservedSamples、unmaterializedBytes 和实际 allocationBytes。预留与已兑现资源有重叠，不能把 reservedMutableBytes 直接叠加到实际显存。unmaterializedBytes 是尚缺的私有增量及待移动粗投影额度，也不是 committed GPU 内存。

## 验证与范围

```powershell
npm run effects:deposits:service -- -Adapter AMD
npm run effects:deposits:host -- -AdmissionOnly
```

新增原生场景覆盖：不支持网格与写者上限、执行前取消重用、池分配失败回滚、暂停实例预留不被投影挤占、样本和移动峰值拒绝、输入背压的陈旧时间/恢复/旧视图不变，以及准入回调异常。宿主 AdmissionOnly 只运行四个相关场景：基础精灵、混合库未用非法网格、64 个写者请求的有界准入、选中非法网格。

正式 [原生报告](../reports/0.11.0/31c00bc53c0b/20261025T025629Z/effects-deposit-service-evidence.json)：**13 组/188 检查**，实际 890M debugLayer=true、debugErrors=0。单个 8³ density 的对齐 mutable 为 196608 B、投影为 262144 B；将 local 上限设为 655359 B 时，两份私有预留仍可接受，但第一份投影会延迟，取消另一份暂停预留后恢复。输入批次上限为 2、同一 fence 连记 3 tick，后两 tick 整源延迟，查询 age=2/Stale；完成后下一 tick 更新至 sourceTick=3，旧视图不变。

正式 [宿主原始报告](../reports/0.11.0/31c00bc53c0b/20261025T025441Z/effects-deposit-host-evidence.json) 保留 **11/14**。三个正常运行全部成功、实际 240 帧；检查脚本误要求 80 帧，而冻结的 host/main.cpp 将 --frames 限制为至少 240。只修正该预期后，[独立复核](../reports/0.11.0/31c00bc53c0b/20261025T025441Z/effects-deposit-admission-recheck.json) 对原始 JSON/图像重新计算全部断言为 **14/14**，核验 248 个冻结文件，没有改写原报告或重复 renderer 运行。

32 个写者接受、32 个 Capacity 拒绝；478 tick 共 15296 次重写、32 个 Shared 来源，场拥有资源 23988480 B。混合库选中基础精灵时场分配/预留均为 0，截图与单独基础库 SHA-256 相同。选中不支持网格时在第 0 帧拒绝，场分配/预留为 0、录制器未标记 failed。三个正常运行实际调试层错误为 0，既有 sampler 警告分别为 869/869/1093，原样保留。

相关 [私有场专项](../reports/0.11.0/31c00bc53c0b/20261024T152936Z/fields-local-gpu-evidence.json) **9 组/2363 检查/274 次 CPU 粗格比较**；[运行时专项](../reports/0.11.0/31c00bc53c0b/20261025T025636Z/effects-gpu-evidence.json) 只运行 RuntimeOnly，**7 组/1158 检查**。均为真实调试层 errors=0。前者源码一致性与后者最终运行时的来源仍需按各报告 hash 对照，不能按报告日期或提交号推定相同。

[收尾来源核对](../reports/0.11.0/31c00bc53c0b/20261025T030209Z-admission-final/effect-deposit-admission-finalization.json) 完成 361 次当前输入比较、583 次冻结核验、4 个二进制核验，并保存 251 项匹配输入。粒子/场专项的依赖仍与报告一致；宿主验证结束后有 12 项材质、动画、玩法等并行输入发生变化，逐项 hash 与当时内容已单列保留，hostCurrentMatches=false。宿主结果只证明原冻结版本，不代表这些后续改动已重新构建或验证；未覆盖、还原或清理并行改动。基础与容量场景实际截图已人工查看，均有直接精灵绘制。

可独立重放上述宿主断言复核：

```powershell
node .incrementalTesting/simulation/check-effect-deposit-admission.mjs reports/0.11.0/31c00bc53c0b/20261025T025441Z/effects-deposit-host-evidence.json build/effects-deposits/admission-recheck.json
```

这是可恢复私有容量管理的一部分。后续已补充 [WF 快照/字节/投影行预算下的发布事务与恢复](FIELD-PUBLICATION-TRANSACTIONS.md)；逐候选公平预算裁剪、GPU 页池/多级分辨率、动态质量降档、四系统联合性能、玩法权威接入和存档尚未完成。没有运行全量回归。
