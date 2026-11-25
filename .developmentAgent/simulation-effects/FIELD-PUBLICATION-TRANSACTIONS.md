# GPU 视觉 WF 发布事务与容量恢复

2026-10-25。[共享控制器](FIELD-SHARING-GPU.md)、[粒子写场服务](EFFECT-DEPOSIT-SERVICE.md) 的增量；保持私有 LEF、共享 WF 与 CPU 玩法权威分离。

## 提交结果

GpuVisualWorld.tryCommit 返回 Committed、Deferred、Invalid 或 ResourceFailure。既有 bool commit 保留，只有 Committed 返回 true。

- 快照数量、投影行数、对齐快照字节/描述符估算不足返回 Deferred，记录 deferredCommits；不记录这一聚合版本的 GPU 命令，也不替换已提交视图。
- 非法时间、命令列表、设备/fence 或未来生产版本返回 Invalid。设备/真实资源创建异常返回 ResourceFailure，不伪装成可无限重试的容量限制。
- publications/duplicates/rejections 是元数据接口尝试计数；容量提交另计 deferredCommits。commits/dispatches 只计实际成功发布及 GPU 派发，不能用提交数乘二推定所有派发数。

单独调用 tryCommit 不会撤销调用者已暂存的来源。需要整批恢复时使用下面的控制器事务。

2026-10-26 新增 `GpuFieldTransaction`，使多个控制器使用同一个世界场检查点和共同提交。它拒绝未参与者、原始写接口旁路、事务前暂存和提前确认；现有生产服务迁移尚未完成。完整合同与真实粒子/烟联合证据见 [共同发布](FIELD-JOINT-PUBLICATION.md)。

## 原子元数据批次

GpuFieldSharing.beginTransaction 保存有界槽状态、共享计数、观察时钟和配额，并让 WF 保存租约/代际与暂存来源。嵌套事务拒绝；未结束事务时 WF shutdown 拒绝。

调用者在批次内执行 promote/project/demote/retire/observe，随后 commitTransaction 使用同一真实队列、fence 和 tick 提交。成功后批次完成，再逐源 settle 确认已提交版本。Deferred 或其他提交拒绝会自动恢复全部暂存来源、写租约、共享状态、epoch、配额和观察时间；显式 rollbackTransaction 也可取消尚未提交批次。来源 ID 仍严格单调，不因取消注册而重用。

已有显式 publish → commit → settle 调用方式继续支持，其 Pending/Busy 合同不变。生产写场服务采用事务作用域守卫，异常或提前退出也会回滚未提交批次；它不在 beginTransaction 内开始另一批次。

事务复制的是 CPU 元数据和资源引用，不复制 GPU 场值，也不等待 GPU。局部粒子模拟和 LEF 重写发生在发布事务之前，不随 WF 容量回滚；可恢复提交拒绝仍推进视觉模拟 tick。旧 WF 保持实际 sourceTick，查询年龄会增长，不能用新局部时间刷新旧共享数据。

## 来源退出、移动与覆盖交接

Shared 来源退出时，服务保留其旧代际和退休队列，直到撤销批次提交并确认。容量失败不会丢失退出记录，也不会让暂存的新写者残留。批次取消后，已停止实例的旧共享值可能继续被查询；其来源时间和年龄保持真实，后续提交撤销。

移动创建新 LEF 代际；旧 Shared 撤销与最新代际提升位于同一批次。失败后保留旧共享代际，连续移动可以替换尚未共享的局部代际，但不能丢掉继承共享标记及必要投影预留。成功后才清除该标记并回收退休源。

控制器撤销写者时，先把其覆盖标记为下一版本退出。新写者申请下一版本页额度时不再计算这份退出覆盖，因此无订阅读者时可在固定 8 页额度中原子交换两个相离的 8 页区域。旧 GPU 快照/投影仍计入真实字节与在途快照额度，按 fence 回收；不是提前释放旧物理资源。旧区域若仍有读租约，其覆盖继续计费。

没有任何下一版本覆盖时，提交仅保存来源墓碑的版本/epoch/时间确认，不分配空 GPU 场、也不派发 clear/aggregate。settle 据此释放最后写租约和来源；墓碑不会成为随历史来源无限增长的表。旧读取视图仍由其自身版本和实际 fence 保护。

## 生产服务行为与范围

GpuFieldDeposits 报告 deferredCommits 和 lastCommitDeferral。共享更新、退休、新提升作为一个发布批次；不能只提交移动的新源而漏掉旧源撤销。粗投影/私有输入自身的容量限制仍使用已有 deferredUpdates/deferredWrites/deferredPromotions。

事务仍按原子批次提交，后续 [逐来源发布预算](FIELD-PUBLICATION-SCHEDULER.md) 已在投影前隔离持续超预算候选，让其他可承受来源继续更新；整批既有快照/在途资源本身不适配时仍延期。已有同优先级等待排序和明确超期诊断，尚无抢占、帧截止保证或动态质量降档。真实分配异常和协议错误仍按运行时失败协议处理。它不提供流体求解历史迁移、跨队列生产、多级页池、CPU 权威 tick/存档或 A–G 联合性能验收。

## 增量验证

```powershell
npm run effects:deposits:service -- -Adapter AMD
npm run fields:world:gpu -- -Adapter AMD
npm run effects:deposits:host -- -TransactionOnly
```

生产服务新增七组恢复场景：单快照读者占用、压力期间退休及取消、投影行额度、真实对齐字节峰值、连续移动、页覆盖准入，以及精确页额度下的移动交接/无数据页撤销。直接控制器新增显式/非法提交回滚、嵌套拒绝、额度恢复和旧撤销/新提升一起回滚及重试。所有数值测试使用真实 GPU 粒子池、LEF/WF 与调试层。

正式原生报告：[写场服务 20 组/314 检查](../reports/0.11.0/31c00bc53c0b/20261025T031344Z/effects-deposit-service-evidence.json)，[WF 14 组/1642 检查/128 次 CPU 查询比较](../reports/0.11.0/31c00bc53c0b/20261025T031350Z/fields-world-gpu-evidence.json)。均为 890M、debugLayer=true、debugErrors=0，源与二进制已冻结。没有运行全量回归。

随后只增加真实 queue gate 背压场景，最终 [服务报告](../reports/0.11.0/31c00bc53c0b/20261025T031846Z/effects-deposit-service-evidence.json) 为 **21 组/329 检查**。队列实际阻塞时，同一 fence 仍完成 4 个 tick 的 CPU 命令记录，其中 3 次 WF 提交延期；停止后的退休队列、原 WF 与输入引用保持。解除 gate 后旧视图值仍为 250，释放读者占用后下一 tick 正确撤销。调试层错误为 0，没有把测试提供的假 completedFence 当作已完成。

真实 [宿主增量报告](../reports/0.11.0/31c00bc53c0b/20261025T031420Z/effects-deposit-host-evidence.json)：只运行正常发布/停止退出两个场景，**8/8**。宿主依赖增量编译 30 单元、复用 21 单元，二进制、完整依赖和夹具在实际运行前冻结，结束时 sourceChangesAfterFreeze 为空。每场景 240 帧/478 tick；正常四源 1912 次重写、476 次 WF 提交/952 次聚合派发，拥有资源 4850176 B。stop20 后 19 次提交中最后一次为元数据撤销，所以只有 36 次 GPU 派发，私有/世界/来源/预留/租约/快照/退休队列全归零。CPU 摘要相同，实际 debug layer 错误 0；既有 sampler 警告各 869 保留，宿主依赖的既有编译警告也原样保留。

[正常实际画面](../reports/0.11.0/31c00bc53c0b/20261025T031420Z/transaction-world.png) 已查看，直接精灵仍绘制；该画面不表示统一体积合成已完成。

[最终来源核对](../reports/0.11.0/31c00bc53c0b/20261025T032021Z-publication-final/field-publication-finalization.json) 完成 343 次当前比较、598 次冻结核验、3 个二进制核验并保存 259 项匹配输入，concurrentChanges 为空。相关六份池/LEF/WF/查询 shader 数学源码与前一准入批次一致；本轮修改的是提交/共享生命周期与预算恢复，未重复执行不受影响的独立 LEF、粒子池或完整仿真套件。
