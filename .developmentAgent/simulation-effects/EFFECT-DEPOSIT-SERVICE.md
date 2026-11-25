# 生产粒子写场服务与 renderer 接入

2026-10-24，`engine/native/effects/field_deposits.*` 将 [认证 fieldDeposit 模块](EFFECT-FIELD-DEPOSITS.md) 接入生产 `GpuRuntime` 回调和 renderer。该服务从 GPU 粒子快照重建私有 LEF，按观察需求向 GPU 视觉 WF 发布低频投影；不包含流体求解历史或 CPU 权威数据。

## 资产、网格与目录

服务复制明确的非负视觉标量目录，按模块 channel 名找到 ChannelId、单位、owner 和 Sum/Maximum 规则。一个 LEF 源的所有通道必须属于同一 owner；CPU 权威域拒绝进入写服务。默认 density 使用 Sum、上限 INT32_MAX、比例 1000，moisture 使用 Maximum、上限/比例 65535；这些是声明的视觉整数单位，不自动代表质量或真实湿度。

`field.voxelMetres` 相对配置的世界 cell 必须为 1/2/4/8/16 比例。场覆盖从 `instance.origin + asset.bounds.min` 起，extent 来自声明的 field.dimensions；它不是通过 GPU 位置同步读回求包围盒。负坐标按 floor 处理，小数原点向外覆盖，可能每轴增加一格对齐 padding，实际尺寸和预算一并扩大，不截断边界。实际每轴最多 128，样本上限也在创建前检查。

全 local-space 效果随实例原点重建场。含 world-space 的效果保留过去作者场范围与新范围的并集，使旧粒子和新发射位置可以继续参与；超出有界网格时在 move 准入中拒绝，保留原实例位置与场。粒子若自行运动到作者覆盖以外，仍由 LEF 诊断记录覆盖外损失。

这些场每 tick 都从当前粒子重建，因此移动时可以生成新 source/generation，同时在同一次 WF 提交中撤销旧源；旧 GPU 快照保持原样。已经 Shared 的网格重建继续共享，不重新等待 minimumLocalTicks 或占用新效果的观察提升额度，避免共享空档。这里不是一般 LEF 流体状态的守恒搬迁或求解权转移。

## 按需共享与生命周期

服务创建时只有管线，没有 LEF/WF 数据 buffer。首次收到未暂停写者时才创建私有场；最初就暂停的实例可以只登记元数据。local 资产在没有观察需求时不生成粗投影、不申请 WF 写租约、不执行 WF 派发。

worldVisible 资产、显式观察、区域读租约（含实际读取 halo）可以触发共享。观察使用既有 GpuFieldSharing 的迟滞、最短驻留和每 tick 配额；候选先稳定按优先级/source 排序，配额满后不再生成候选投影。WF 准入容量失败保留 LocalOnly 并报告 deferredPromotions，其他错误按失败协议处理。Shared 在未暂停 tick 更新来源投影；暂停保留旧值和 sourceTick，不能刷新年龄。

公开 acquire/release/view 只提供读者权限，Strong/权限/通道/年龄仍由 WF 检查。observe、inspect、localBuffers 与 markLocalUsed 提供对象观察、局部表现和实际 fence 使用登记；借用 buffer 不能超越 owner 或标记期限。

完整活跃集合中的缺失实例会退休。flush 暂存退出但不重复 splat 或推进模拟发布 tick，下一真实 tick 提交撤销并确认；重复 flush 幂等。最后读者离开可降级，保留私有数据；实例退出则连私有数据一起在实际 fence 完成后回收。shutdown 在活跃实例、读者或有资源在途时拒绝，避免部分拆除后台后继续录制。

同一实际 fence 下可记录多个 simulation tick。退休的私有 buffer、输入四资源引用、粗投影与 WF 快照由原有实际 fence 管理，不能依据 CPU 状态已经 Retired 提前释放。服务不保留 GPU 池裸指针，也不新增运行时粒子属性或场值读回。

## 预算与当前拒绝策略

默认 32 个活跃写者，最多配置 128；私有管理器默认 64 个代际槽、256 个投影视图、512 输入批次、1M 样本和 64 MiB。WF 默认 128 砖块、8 快照、64 MiB；服务上限 128 MiB，构造时要求两个预算分区之和不超过总额。重建的旧/新代际与在途快照均计费。

accountedBytes 为 LEF 实际对齐 committed buffer 加 WF 对齐 buffer/描述符 stride 估算；借用粒子资源由 GPU runtime 计费，不重复加入。PSO/root/driver 内部内存不等同于该 buffer 数值。这个分区预算尚不是粒子、场、布料、水的完整联合预算。

2026-10-25 新增 spawn 前准入：在分配 GPU 粒子池之前验证所选写资产的网格/通道、写者数量、实际对齐私有字节/样本/槽，并持有 RAII permit。未执行或暂停实例只预留 CPU 元数据，尚不创建场数据 buffer；池分配失败或立即取消释放预留。move 先验证外扩网格及旧/新场共存峰值，拒绝时保留旧位置与来源，运行时仍可继续。准入异常、缺失 permit 不会被当成成功。

粗投影分配尊重未兑现私有预留；不足时延迟提升/更新。输入批次不足时跳过该源本 tick 的整次重写，保留来源 sourceTick 和旧 WF 值，实际 fence 完成后重试。它不等待 GPU，不把旧结果伪装成本 tick 数据。详见 [准入与背压合同](EFFECT-DEPOSIT-ADMISSION.md)。

此处覆盖私有资源预留和若干可恢复容量限制。后续已补充 [WF 发布事务](FIELD-PUBLICATION-TRANSACTIONS.md)：快照/字节/投影行预算不足时整批回滚元数据、保留旧共享版本，并在容量释放后重试最新局部快照；退休和连续移动也保留恢复状态。分配器真实失败与录制协议错误仍可能要求排空并重建；动态分辨率降档、完整四系统联合预算尚未实现。不能将这一增量概括为完整 G 阶段预算回退已完成。

随后新增 [逐来源预算预估与等待调度](FIELD-PUBLICATION-SCHEDULER.md)：不适配候选在粗投影之前拒绝，不消耗提升配额；其他可承受共享更新继续。移动的旧源撤销与新源发布仍属于同组。inspect/publicationBacklog 提供每个等待来源的实际原因、优先级、首次等待 tick 和超期状态；暂停者能补发已计算局部版本，保留真实来源时间。

## renderer 与宿主

`renderer/gpu_effect_bridge.cpp` 在配置资产中存在 fieldDeposit 时创建服务，并给 GpuRuntime 配置生产驱动和准入回调。使用 renderer 自己的 device、graphics queue/fence。写后端能力在选中 spawn 时验证；效果库中未使用的不受支持写资产不会阻止其他效果运行。资产自身的认证/结构校验仍在 install 时执行。没有写模块的库不创建服务。当前 renderer 使用默认目录、1m 世界网格和默认政策；自定义服务参数已有原生 API，尚未全部暴露到场景/编辑器。

GPU 效果时间戳覆盖写场命令，因此 deposits.includedInEffectsGpuTime=true。报告分别列出私有/世界字节、派发、来源、共享数、延迟提升、代际重建与退出状态。shader/heap 切换后恢复到现有精灵准备路径；写场不会自行改变粒子运动或直接精灵绘制。

宿主沿用既有 effect/library/origin/count/repeat/stop CLI，不新增另一套效果时钟。renderer 重建创建新 service/runtime 身份域，queue 等待完成后先释放 runtime 再释放服务。当前新 WF 尚未接入统一体积消光或温度/风/水读写耦合，直接精灵图像保持一致是本次预期行为。

## 增量命令与正式证据

```powershell
npm run effects:deposits:service -- -Adapter AMD
npm run effects:deposits:host
```

原生专项只选编相关 16 个池/LEF/WF/查询 shader，使用 build/effects-deposit-service。宿主仅构建自身依赖到 build/effects-deposit-host，没有构建/执行完整 native/JS/CI 或全量回归；没有运行 npm test/npm run verify。资产验证专用 build/assets-host 保持独立。

正式 [原生报告](../reports/0.11.0/31c00bc53c0b/20261024T144711Z/effects-deposit-service-evidence.json)：**7 组/98 检查**，冻结 50 项来源，890M 实际调试层 errors=0。覆盖局部按需、区域订阅/降级、暂停、小数移动原子重建、world-space 扫掠、4:1 与非零米制原点、density/moisture 两种归约、提前跳过超配额候选、权限与预算分区。真实队列阻塞下同 fence 记录 5 tick，退休后保留 1 个私有实例和 32 个输入资源引用，完成后旧 WF 视图仍可读且最终分配归零。

正式 [宿主功能记录](../reports/0.11.0/31c00bc53c0b/20261024T144503Z/effects-deposit-host-evidence.json) 保留首次 **31/32**：9 个实际运行，210 项宿主依赖和 26 项资产/夹具在长运行前冻结。功能检查通过，唯一失败为生命周期 tracked 对象计数；没有将原失败报告改成成功。

- 4 实例、240 帧、478 个视觉 tick，1912 次局部重写、1904 个粗投影、476 次 WF 提交/952 派发；4 个来源共享 4 砖块，报告拥有的 buffer/描述符估算共 **4850176 B**。
- local 模式同样 1912 次重写，私有 **1835008 B**，粗投影、WF 字节/页/派发全为 0。
- stop tick20 后只提交 19 次 WF（包含撤销），最终私有/世界/来源/租约/快照/在途退休字节均归零。重复短效果保持有界；中文路径、重建和非法网格拒绝通过。
- base、local、world 的实际截图 SHA-256 相同，CPU 权威摘要相同；[实际画面](../reports/0.11.0/31c00bc53c0b/20261024T144503Z/world.png) 已检查。所有正常原运行实际 validationLayer=true、validationErrors=0；既有 sampler 警告保留在原始报告中。

生命周期失败被同一二进制的无写场库复现为 −1/−2。原因是既有验证层 ID3D12InfoQueue 创建漏记 track，而 shutdown 会 destroy。仅补一处 track 后，增量重跑 base/写场两组生命周期：[修复后记录](../reports/0.11.0/31c00bc53c0b/20261024T144947Z/effects-deposit-host-evidence.json)，**7/7**，各两次关闭的 tracked 数均为 0，写场运行 VRAM 两次均 66248704 B。未因这个计数修复再次跑全部宿主场景；其余功能结果对应前述冻结版本，源差异归档核对为该记账行与构建来源标记。

## 全方案剩余项

统一体积与同源覆盖遮罩、独立 SmokeFireSolver、CPU 权威 tick/存档、多层/脏页池、压力/迭代 halo、一般 LEF 移动及求解权迁移、温度/速度算子、完整 spawn 准入/降档、其余粒子渲染器、布料/水剩余后端、编辑器和 A–G 联合负载验收仍未完成。原 goal 保持 active。
