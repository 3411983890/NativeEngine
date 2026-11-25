# 粒子与烟的真实生产服务共同发布

2026-10-26。接续 [底层共同事务](FIELD-JOINT-PUBLICATION.md) 和 [粒子服务阶段](FIELD-DEPOSIT-PHASES.md)。本增量完成两项生产服务的阶段接口及真实原生联合验证；renderer 的共同调度尚未接入。

## 烟服务的外部世界场合同

`GpuSmokeSharing(world,producer,densityChannel,limits)` 使用外部世界场的同一目录、device/fence 和网格。显式通道可以采用全局唯一名称，但 owner、kg/m³ 单位、视觉域、Sum 合并、整数范围和 1024 比例必须与既有物理烟密度合同完全一致；不同单位或比例在构造时拒绝。

服务只计自身量化 LEF/投影分配，`ownsWorld=false`，世界场由所属协调者计费一次。关闭烟共享服务不会销毁外部世界场或其他服务的消费者。目录中粒子量化密度与物理烟密度保持两个通道，不因整数值相同就相加。

新增准备、暂存和确认阶段：

1. `prepare(list,fence,tick,simulated,observations,error)` 验证生产输出和观察，更新存活/退休集合，按需求登记来源与创建本地资源。保存输出的 spec/layout/sourceTick/version 等元数据后清除 scalars/velocity/pressure/diagnostics 四个借用指针；需求来自世界场全部只读租约，包含通道交集和 halo。
2. `simulated=false` 只同步生命周期，不建立待发布帧、不推进发布 tick、不生成局部投影；不能把 runtime flush 的下一 tick 标签当成已完成的仿真。已有待发布准备或活动共同事务时拒绝重新准备。
3. `stagePublication(list,fence,error)` 仅在共同事务内接受与准备时相同的命令列表和真实 fence。所有当前生产者的 serial/sourceTick/version 必须与准备签名一致；源已停止或推进时在导入前拒绝。通过后执行原预算/优先级/驻留/配额、GPU 密度导入/投影、来源更新/撤销，不自行 commit。
4. 协调者共同 commit/rollback，然后分别调用两项服务的 `finishPublication`。共同拒绝时本地求解继续保留；成功才确认输入版本。烟域有实际修改时校验提交 snapshotTick 等于准备 tick。每个服务应收到原提交原因的独立字符串副本，避免先完成的服务清空后续服务的诊断。

原构造函数与 `sync` 继续提供独立服务路径，内部通过单参与者共同事务使用这些阶段。外部共同模式拒绝独立 sync，防止提前提交其他域。原 same-tick 停止、旧视图保活、尾烟、预算延期和按源光学选择合同保留。

## 真实双用途资产与调用顺序

新增 `field-services-fixtures.mjs` 生成并认证 cook 一份同时包含 fieldDeposit 和 smoke 的资产。作者层采用现有支持的 density 名称及合法 8 倍尺寸；全局目录将粒子 density/density-q/effects 与 smokeDensity/kg/m³/smoke 明确分开。

真实 `GpuRuntime` 的后模拟回调先调用粒子 `prepare`，随后烟求解原 hooks 生成实际密度，再调用烟共享 `prepare`。全部准备使用同一个真实 runtime tick。随后测试协调者建立一个 `GpuFieldTransaction`、分别暂存、共同提交一次并分别确认；没有把帧末的 runtime.tick=T+1 串行拼到粒子 tick=T。

| 过程 | 已提交版本 | 粒子整数值 | 烟整数值 | 已提交源 tick |
|---|---:|---:|---:|---:|
| 首步 tick 0 | 1 | 2 | 2 | 0 |
| 旧视图固定预算，tick 1 拒绝 | 保留 1 | 保留 2 | 保留 2 | 保留 0；两个实际求解者继续推进 |
| 释放旧视图，tick 2 恢复 | 2 | 2 | 6 | 2 |
| 暂停，tick 3 形成共同快照 | 3 | 2 | 6 | 保留 2；不重复重写/导入 |
| 停止后 flush | 保留 3 | 旧视图保留 | 旧视图保留 | 不虚构新仿真 tick |
| 旧视图仍固定预算，tick 4 撤销拒绝 | 保留 3 | 两域一起保留 | 两域一起保留 | 两域退休元数据仍在 |
| 释放旧视图，tick 5 撤销成功 | 4 | Unavailable | Unavailable | 退休后新视图不含旧贡献 |

另一个场景在两个域已经准备后停止实际源，先暂存粒子发布，再验证烟服务拒绝过期生产签名；共同取消后无部分提交，生命周期刷新后可以正常清理。这不是用空指针代替资源生命期：GPU 命令和真实资源仍由既有 fence 引用保护。

## 增量证据与边界

- [真实生产服务 20261026T114614Z](../reports/0.11.0/31c00bc53c0b/20261026T114614Z/fields-services-gpu-evidence.json)：2 组/124 检查、14 次 GPU 数值比较，实际 AMD 890M，D3D12 调试错误 0。执行前冻结二进制、递归来源、作者资产/cook 与生成 shader，运行期间输入零变化。
- [原烟共享回归 20261026T114226Z](../reports/0.11.0/31c00bc53c0b/20261026T114226Z/smoke-sharing-evidence.json)：原 6 组/240 检查、16 次密度/WF 比较全部通过，实际调试错误 0。包含暂停、停止、尾烟、观察/配额、真实队列与背压；同样执行前冻结，输入零变化。
- [联合服务来源审计](../reports/0.11.0/31c00bc53c0b/20261026T114737Z-field-services-final/field-services-finalization.json)：149 项检查、132 项冻结来源核对通过，两项专项当前输入均匹配；数值 shader 保持不变。
- [原字段/粒子服务来源复核](../reports/0.11.0/31c00bc53c0b/20261026T114737Z-fields-joint-final/fields-joint-finalization.json)：250 项检查通过，场核心、原生双求解者、原 28 组粒子服务及最新烟共享依赖仍匹配。没有为未改变的依赖重复运行 GPU 专项。

入口为 `npm run fields:services:gpu` 和 `npm run smoke:sharing`；标准 build/verify 只登记未来入口，本次没有运行全量构建、全量测试/回归、verify 或 CI，也未借用资产导入构建目录。

当前 renderer 仍走原独立世界场和帧末烟 sync。全局调度协调器、跨域时间/总内存预算、消费者统一路由及 same-tick 新视图失效、CPU 权威整合仍未完成。联合测试只验证烟服务包装器在退休时拒绝新烟视图；直接访问底层世界场仍可取得旧提交快照，不能声称全局查询失效已完成。本增量不构成 B/D 或全部 A–G 验收。
