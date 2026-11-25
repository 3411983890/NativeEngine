# GPU LEF/WF 可观察性共享控制器

2026-10-24，`engine/native/fields/gpu_sharing.*` 在 [GPU 视觉 WF](FIELD-WORLD-GPU.md) 上实现 B4 的状态机、迟滞、驻留与提升配额。它管理视觉来源的共享生命周期；局部 LEF 的模拟、细数据和求解所有权仍归生产者。后续已通过 [生产粒子写场服务](EFFECT-DEPOSIT-SERVICE.md) 接入资产/renderer；体积覆盖合成仍未完成。

## 注册与状态

`GpuFieldSharing` 借用一个必须比它活得更久的 `GpuVisualWorld`。单 CPU 录制 owner 调用接口，不支持并发修改。`create({owner,source,maxWorldPages,maxAgeTicks},createdAt)` 仅登记有界 CPU 元数据，不申请 WF 写租约、GPU buffer 或派发工作。

默认最多 128 个注册实例，可配置至 4096；没有随历史来源不断增长的 owner 墓碑表。source ID 在控制器内全局严格递增，退休后不复用；槽位句柄带 generation。首次投影才确定实际粗区域、通道与写权限，声明 owner 不会绕过 WF 的域/单位/设备/时间线检查。

状态遵循方案：

```text
LocalOnly -> PromotePending -> Shared -> DemotePending -> LocalOnly / Retired
```

- `promote(handle,immutableProjection,publishTick)` 校验来源身份，申请投影范围的写租约，暂存整组通道，进入 PromotePending。失败释放新申请的租约，不推进 epoch，不留下部分来源。
- `project` 在 Shared 中暂存下一次替换，状态保持 Shared 且 pending=true；当前已提交版本仍可读。固定共享期间的区域/通道变化明确拒绝；移动是后续独立转换。
- `demote` 在 Shared 中暂存整源撤销并进入 DemotePending。`retire` 对 Shared 执行同一撤销并标记提交后退休；LocalOnly 可直接退休。Pending 中的新转换返回 Busy。
- 调用者提交 WF 后，`settle` 必须确认已提交的 owner/source、epoch、publishTick、sourceTick 和贡献存在状态完全匹配。旧快照的确认不能误认成新发布。确认前保持 Pending，提交失败可用相同暂存内容重试。
- 提升/降级的驻留起点使用确认版本的 **snapshotTick**，不使用提出请求的时间。普通 Shared 更新不重置最短共享驻留。
- 降级确认后释放写租约与 WF 来源声明；退休确认后使句柄失效。LEF 不被清除、重采样或移交。WF epoch 独立于局部 projection epoch，多次共享同一个局部版本也能正确推进。

状态确认是 GPU 命令记录/版本发布屏障，不意味着 GPU 已执行完成；没有 CPU 等待。WF arena、借用投影、上传与旧视图的实际释放仍由原有真实 fence 管理，不能依据 LocalOnly 或 Retired 提前释放资源。

正常退出应 `retire -> commit WF -> settle`。析构兜底仅释放仍持有的写租约：未提交来源取消，已发布来源在下一次 WF 提交中撤销，已取得的旧快照继续受视图/fence 保护。析构不替调用者提交队列或等待 GPU。

2026-10-26 新增 [多控制器共同发布](FIELD-JOINT-PUBLICATION.md)：同一世界场的多个求解者可在一个 tick 原子发布；活动共同事务内禁止未参与控制器修改状态或原始写接口旁路，任一控制器析构先取消共同事务再执行上述释放。现有两项生产服务与宿主尚未迁移。

2026-10-25 新增可选 beginTransaction/commitTransaction/rollbackTransaction：容量提交失败时整批恢复写租约、暂存来源、共享状态、观察时钟与配额，避免生产运行时因一次视觉预算不足永久停录。控制器撤销覆盖可参与下一版本的原子页额度交接，零覆盖提交只保存撤销确认。既有显式 Pending 调用方式保持；详见 [发布事务合同](FIELD-PUBLICATION-TRANSACTIONS.md)。

## 可观察性与配额

`observe(observations,tick,PromotionPolicy)` 使用与 CPU 参考相同的政策类型：默认 promoteAt=600、demoteAt=300、minimumSharedTicks=30、minimumLocalTicks=2、promotionsPerTick=4。

LocalOnly 满足最短驻留后，worldVisible、subscribers>0 或 concentration>=promoteAt 任一条件触发提升。Shared 满足最短驻留，且无 worldVisible、无 subscribers、concentration<=demoteAt 时触发降级。迟滞区间保持现有表示。没有出现在批次中的来源不会被隐式删除。

每个批次稳定按 priority 降序、source ID 升序，再按 owner 排序。成功提升消耗当前 tick 的额度；同 tick 分多次调用不重置额度。容量拒绝、投影缺失或发布失败不消耗额度，可在同 tick 修复后重试。优先级比较只覆盖已提交的当前批次，不能预知后续调用的更高优先级观察。

重复句柄、负浓度、倒退观察时钟、非法阈值/零配额或超容量批次在决策前整体拒绝，不改变观察时钟、额度或任何来源。单个失效句柄在输出中返回 InvalidHandle，不消耗其他有效项的提升额度。显式 `promote/project/demote/retire` 是调用者直接操作；自动配额只约束 `observe` 的提升。

**浓度是调用者显式提供的观察量。** 本模块没有同步 GPU 密度读回，也没有实现 GPU 阈值归约/异步遥测管线。订阅和 worldVisible 可直接来自对象系统；若浓度来自 GPU，调用者仍需提供有来源时间与延迟合同的异步观察。不能将一个作者提示值当成实际 GPU 全场阈值检测已完成。

提升需要已有的不可变 `GpuLocalProjection`，缺少时明确拒绝；其生成由生产者管理，控制器不创建或销毁 LEF。当前观察 API 不会自动按配额延迟构造所有候选投影，调用者应控制候选生成成本。后续宿主调度需要把候选选择、实际生产与 WF 提交连成明确阶段。

## 调用顺序

1. 注册来源，局部生产者独立执行 begin/splat/resolve。
2. 收集外部订阅、worldVisible、浓度观察与稳定优先级；需要共享时生成不可变投影。
3. observe 或显式转换；检查每项返回值，不能把 Capacity/Busy 当成功。
4. 记录并提交 WF 新版本；失败保留 Pending，以原暂存数据重试。
5. settle 确认版本；消费者取得对应 WF 视图，使用前登记实际 markUsed fence。
6. 结束来源时 retire/commit/settle；局部 LEF 的退休由其 owner 另行完成。

控制器状态没有实现渲染覆盖遮罩。WF 是多源聚合，不能把它当作某个源的独立细节替身直接叠加；LEF/WF 同源去重消光仍需体积合成阶段实现。这里也没有真实求解权接管、CPU 权威降级或玩法数据删除。

## 增量验证

沿用 `npm run fields:world:gpu -- -Adapter AMD`，只增加该控制器及其相关测试依赖；独立 `build/fields-world-gpu`，不运行完整 native/JS/CI、npm test、npm run verify 或全量回归。原 WF 八组数值/租约专项仍在同一依赖范围内。

正式 [fields-world-gpu-evidence.json](../reports/0.11.0/31c00bc53c0b/20261024T101517Z/fields-world-gpu-evidence.json)：890M，**13 组/1601 次检查/128 次 CPU World 数值查询比较，实际 debugLayer=true、debugErrors=0**。冻结 40 项构建输入、生成 shader/header、二进制、空警告编译日志及运行输出。包括原 WF 八组与本次控制器五组：

- CPU LocalFields 状态、epoch、Pending、迟滞、最短驻留决策对拍；低浓度下订阅和 worldVisible 保持共享，中间迟滞区间不会抖动。
- 配额为 1 时按 priority/source 顺序选中；同 tick 再调用不可重置。重复/负浓度/坏政策的未来批次被拒绝后，仍可在原 tick 按原配额工作。失效高优先级句柄不消耗额度，缺少投影可补齐后重试。
- 容量拒绝和通道单位不匹配发布失败后，没有写租约/来源/epoch 泄漏；同 tick 重试通过。单快照额度被旧读者占用时提交失败，控制器保持 Pending，读者退出后原暂存内容可重试确认。
- 真实 queue gate 下，撤销的元数据已确认而 GPU 尚未完成时，资源仍被实际 fence 和读取视图保护；退出不提前释放。LEF 保持生产者所有。
- 同一局部 epoch=1 的投影经过 32 次提升/降级，WF 确认到 epoch=64，无写租约/来源记录残留，保留旧视图不变。未提交和 Shared 析构均清理写能力，当前快照保留旧值、下一提交撤销。
- 提升请求 tick=4、提交 tick=9，驻留从 9 开始；降级请求 12、提交 15，从 15 开始局部驻留；拒绝在已确认状态存在之前退休。

本次没有修改 LEF 数值实现或 shader，没有重复运行不受影响的 LEF 独立九组门禁。上一批冻结 LEF 证据的全部 34 项来源在归档审计中重新核对，保持相同；本次新 WF 构建与运行仍使用当前依赖。

## 剩余工作

效果资产和宿主自动接入、候选投影生产调度、真实 GPU 浓度观察、预算驱动视觉分辨率调整、覆盖去重体积、移动/多级分辨率、GPU 页池与流体/迭代 halo、真实求解权迁移、权威集成/存档及完整 A–G 联合预算仍未完成。可观察性控制器的原生通过不能替代这些验收。


2026-10-24 后续：[认证粒子场沉积模块与原生实例驱动](EFFECT-FIELD-DEPOSITS.md) 已保留作者整数 quantum，并通过显式 GPU runtime 回调连接真实池/LEF/WF。默认 renderer/宿主生产驱动、完整资产映射和体积接入仍待完成。


2026-10-24 后续：[生产沉积服务与 renderer](EFFECT-DEPOSIT-SERVICE.md) 已加入目录/网格/实例映射、观察调度、粒子快照移动重建、停止回收及默认 renderer 回调。上文“生产驱动尚未接入”是首次交付边界；统一体积、一般求解历史迁移、完整预算准入与降档仍未完成。正式记录区分首轮功能/计数失败和修复后的增量生命周期结果。
