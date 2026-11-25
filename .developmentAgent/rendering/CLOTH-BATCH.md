# 多实例 GPU 布料批处理

2026-10-24，E/A1 的性能增量。原 A–G 目标继续 active。

`engine/native/cloth/batch_gpu.*` 的 `GpuBatch` 在一次 `Dispatch(N,1,1)` 中运行 N 块独立布料：每块仍是 256 线程、30 KiB TGSM，容量 ≤512 顶点和 ≤4608 个标量约束。保留所有子步、迭代、离线颜色、气动力和接触。每个线程组从自己的元数据和描述符读取资源；不拼接、裁剪或共享实例状态。

均匀风的 PrepareWind、完整 tick 和法线/切线更新合为一次派发。含共享场的实例先沿原路径执行 PrepareWind、SampleWind、CommitWind 三次派发，再参加共享求解派发；整布提交/不可用回退和 CPU 权威隔离不变。共享的法线辅助函数在批处理内读取最终 LDS 位置，其他入口继续读取原 UAV；15 个布料 compute blob 全部按 `-Gis` 严格编译。绘制继续使用各实例原来的 buffer。

`GpuBatchStep` 支持每实例 dt、fieldTick 和 solver。所有请求先完整验证，包含重复/空实例、时间、设备/实际 fence、字段时间和原有派发预算；随后才记录任何命令。暂停实例跳过，较大网格或显式 Dispatch 实例走已验证的原路径。任一请求无效或元数据环占满时，整个批次拒绝，所有实例状态及命令列表保持不变。每实例 `maxDispatchesPerStep` 仍保守使用单独运行时的 3/5 次组内预算，方便安全回退。

## 生命周期和内存

默认最多 32 实例、32 个在途槽；可设置为 1–256 实例、1–128 槽，超限明确拒绝。每槽只写入自己的 176 字节/实例记录和 12 个描述符/实例。实际拥有的队列 fence 完成后才能复用槽；同一 fence 的多次调用仍占用不同槽。每槽还持有资源 COM 引用，完成后非阻塞回收。没有每 tick GPU 分配、队列等待或顶点读回。

`shutdown` 遇到在途使用会拒绝；与现有 solver 一样，调用者须在实际队列完成后销毁对象。描述符和元数据必须与传入的 solver 使用同一设备和 fence 时间线；不能用调用者自报的完成值释放。

- `metadataAllocationBytes`：通过 `GetResourceAllocationInfo` 取得 committed upload buffer 的实际对齐分配量。
- `descriptorEstimateBytes`：描述符数量乘设备给出的 descriptor stride，**是预算估算**；不代表驱动内部 heap 的精确显存分配。
- `accountedBytes`：前两者之和，构造前按预算准入。默认 32×32 服务为 196608+393216=589824 字节，不重复计算实例已有的 12 个 buffer。TGSM 和驱动 PSO/scratch 也不属于 committed buffer 统计。
- `GpuBatchStats.dispatches` 只记录共享求解的真实派发；各 solver 的 `dispatches` 继续记录初始化、场准备和一般回退派发。每 solver 另有 `batchedTicks`，不能把共享派发再乘实例数累加。

## 宿主接入

`GpuClothFrame.stepAll` 将同一游戏 tick 的实例一次交给服务。预览先对所有实例应用暂停/传送/reset，再调用它；旧的逐实例回调仍兼容。设备恢复重建服务和 solver，原游戏年龄与事件合同保持不变。全为 Dispatch 的配置不分配批处理服务。

宿主报告新增 `batchAvailable`、`batchDispatches`、`batchedSteps`、`batchFallbackSteps`、元数据/描述符预算、槽数和引用计数。`simulationBytes` 仍是实例 buffer；`totalAccountedBytes` 包含服务预算，按原 64 MiB 上限准入。共享派发在 `dispatches` 中只计一次，仍被原有帧内布料时间戳覆盖。

## 增量验证

```powershell
npm run cloth:batch -- -Adapter AMD
npm run cloth:batch -- -SkipDrawBuild -Adapter AMD
npm run cloth:batch -- -SkipBuild -Adapter AMD
npm run cloth:host
```

`SkipDrawBuild` 复用并校验已有求解/绘制构建，重新构建批处理；`SkipBuild` 校验并复用全部专项构建。入口只运行受影响的布料求解/绘制、批处理和宿主专项，没有运行完整 native/JS 回归、`npm test`、`npm run verify` 或 CI。

专项涵盖 280/512 点 600 tick 多实例、528 点一般回退、不同 dt/原点、暂停恢复、传送/reset、改风、共享场整布回退与过期、字段时间拒绝、原子拒绝、真实队列阻塞下的环占满/引用保留/释放/复用。采样逐位比较 position、previous、velocity、normal、tangent 及诊断计数。本机严格算术一致不代表跨硬件 GPU 确定性。

性能采用 4×512 顶点、每样本两次固定 tick，热身 8、测量 60，交替先后顺序；保存四份原始 GPU/CPU 记录时间数组及实际队列频率。只测布料求解区间，不能替代粒子＋场＋布料＋水的完整游戏场景 P95。CPU 记录耗时包含元数据/描述符写入，不能只凭派发次数断言 CPU 更快。

正式证据：[cloth-batch-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T234305Z/cloth-batch-evidence.json)。890M、实际调试层开启、errors=0；**4 组/22939 次检查/6712 次顶点采样比较**，位置/速度最大差为 0，整个顶点结构逐位相同。受影响求解 7 组/2905 次、实际绘制与预览时钟 5 组/255 次也通过，保留各自冻结源码和二进制。

| 2048 点、两次 tick | 逐实例组内 | 批处理 |
|---|---:|---:|
| GPU P50 / P95 ms | 3.31976 / 4.16772 | 0.96452 / 1.01220 |
| CPU 命令记录 P95 ms | 0.3026 | 0.2894 |

上一轮相同夹具记录 `20261023T234146Z/cloth-batch-evidence.json`：GPU P95 4.25436→0.99324 ms；CPU P95 0.3590→0.3799 ms。两轮均保留；CPU 小幅差异和计时尾部波动不构成完整游戏帧的改善承诺，未设定未经方案批准的新生产验收目标。

宿主证据：[cloth-host-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T234725Z/cloth-host-evidence.json)，**89/89、21 个运行**。构建后立即冻结 195 项依赖和二进制，再从冻结可执行文件测试；同时冻结 36 项夹具/资产输入。该次测试期间 `sourceChangesAfterFreeze=[]`。各正常运行实际调试层启用、errors=0，自动批处理与显式 Dispatch 截图 SHA 相同。2048 点负载真实绘制 960 次、892800 个三角形；478 个共享 tick 共 486 次实际派发（含每实例两次初始化），1912 次实例步进。实例 buffer 3145728 字节，服务元数据 65536＋描述符估算 49152，共计准入 3260416 字节。

测试前两次宿主构建/归档被源码一致性检查阻止：一次是本批补充 fence 计数时文件变化，另一次是用户并行修改 scene_loader。保留原日志，不将对应运行归为当前代码验证。宿主入口已改为先冻结编译输入及可执行文件，再执行较长测试；后续共享工作区变化单独记录，不覆盖用户修改，也不把冻结版本结果归给新的修改。

骨骼驱动、自碰撞/CCD、湿润/热、LOD、宿主共享场生产/租约、完整材质/工具与四系统联合验收仍未完成。当前模块只是原方案的一项增量。


归档后检测到 host/main.cpp 的后续修改，保留并重建后立即冻结最新依赖，仅补跑 off/batch/dispatch 三项 240 帧兼容检查，均通过、调试层实际开启 errors=0、两种布料路径截图 SHA 相同、三项玩法摘要一致。补充证据 [cloth-batch-host-delta.json](../reports/0.11.0/31c00bc53c0b/20261024T035413Z/cloth-batch-host-delta.json)；不将这三项兼容性检查冒充用户新功能分支的独立回归，也不将原 89 项结果归给之后的修改。


2026-10-24 后续共享场接入：新增 `--simulation-field`，无需 GPU 粒子也可为布料提供风，或让粒子/多布共享一份快照。旧入口与本页历史报告保留，最新时间/租约/后绑定/根签名合同及 100 项宿主证据见 [SIMULATION-FIELDS-HOST.md](SIMULATION-FIELDS-HOST.md)。
