# 布料组内求解与编译数值合同

2026-10-24。对应 E2/A1 的性能增量，原 A–G 目标保持不变。本批减少派发开销，尚未完成组合负载预算、自碰撞、蒙皮或完整布料产品。

## 两条路径与选择

`GpuParameters.execution` 支持 `Automatic`、`Dispatch`、`Grouped`。Automatic 在顶点 ≤512 且标量约束 ≤4608 时使用组内路径，其他有效网格使用原来的逐颜色派发；强制 Grouped 超出容量时明确拒绝，不裁剪网格或约束。Dispatch 可显式保留，便于数值和计时对照。

组内路径由一个 256 线程工作组完成一件布料的完整 tick，保留作者声明的子步数、迭代数、全部离线颜色与执行顺序。三角膜、二面角、接触和气动力/积分函数由两条路径共用，不用减少迭代换取速度。

- 位置、速度各三条 float SoA 数组，共 512×6×4 字节；4608 个 λ 占 18432 字节，总 TGSM 为 **30720 字节 / 30 KiB**。
- `id^(id>>5)` 对 [0,512) 的位置/速度槽做双射，分散 32 点行跨度的共享内存 bank 访问；逻辑顶点 ID 和拓扑不变。
- 每线程最多持有两份上一子步位置和气动力；每色之后、邻居气动力读取结束后与下一子步之前都有整个工作组的 barrier，不使用跨工作组同步。
- 颜色仍由离线 cook 提供；加载器检查同色的所有读写顶点互斥。运行时将已验证颜色的 offset/count 附在不可变 schedule buffer 后，不重新推导颜色。
- 当前状态、上一子步位置、速度与 λ 在 tick 结束时写回原资源，后续法线/切线与绘制路径继续复用。外部共享风采样和整批提交保持独立，有界/不可用/时间/fence 合同不变。

单独调用 GpuSolver 时，无场每 tick 由 PrepareWind、GroupedTick、Normals 三次派发组成；含场另加采样/整批提交，共五次。原旗帜每 tick 726 次。600 tick 连同初始化由 **435602 次降为 1802 次**。这减少了 CPU 命令和 GPU 派发开销，不代表约束计算成本消失；`maxDispatchesPerStep` 仍只是派发预算。

12 个 committed buffer 的数量和对齐预算不变；schedule 增加少量颜色元数据，现有小布仍各分配 786432 字节。TGSM 是工作组执行资源，不是新增的每实例 committed buffer；驱动内部 pipeline/scratch 开销不计入该 buffer 数值。

## 浮点编译与失败证据

初版普通优化编译的两条路径在 120 tick 后超出 3 mm 位移差门槛。相同失败两次复现：3.62214 mm；局部 `precise` 与共用算式仍未解决全部跨入口漂移。失败二进制、shader/header、源码与日志保存在 `reports/cloth-grouped-difference-20261024/`。64 线程严格模式的数值对拍通过，但 2048 点性能改善未通过本批相对门槛，原始 trace 另保存在 `reports/cloth-grouped-gis64-20261024/`。

最终采用统一 IEEE 严格编译：shader manifest 的布尔 `ieeeStrict` 为 true 时，`compile-shaders.ps1` 给该 blob 加 `-Gis`，生成清单也记录此值。该轮布料求解的 14 个 blob 都启用（加入批处理后为 15 个）；其他 shader 保留自己的设置。省略或 null 视作 false，其他非布尔值被拒绝。局部重要算式仍标记 precise。

[Microsoft 对 precise 的说明](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/precise)涵盖逐指令重排/融合限制；[IEEE 严格编译说明](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/d3dcompile-constants)描述避免违反 IEEE 规则的优化。项目实际 pinned DXC 的帮助也声明 `-Gis` 为 Force IEEE strictness。加入该策略后，两条路径在本机采样中位置/速度差为 0；这是本机、该编译器和夹具的证据，不是跨硬件 GPU 确定性承诺。

## 增量门禁与实测

```powershell
npm run cloth:grouped -- -Adapter AMD
npm run cloth:grouped -- -SkipBuild -Adapter AMD
npm run cloth:host
```

组内入口先构建/运行受影响的布料求解、绘制与预览时钟专项，再生成 512 点容量边界和 528 点回退夹具，执行双路径对拍与配对计时。该轮编译布料的 14 个 compute 和 2 个绘制 blob，共 16 个；当轮完整 shader 清单为 74 个。加入批处理后，当前为 15＋2 个布料 blob、完整清单 80 个。本批没有运行全量回归、`npm test`、完整 simulation suite 或 CI。

正式报告：[cloth-grouped-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T231022Z/cloth-grouped-evidence.json)。AMD 890M、实际启用调试层、errors=0：

- **5 组、15209 次检查、5544 次顶点采样对拍**。280 点旗帜与 512 点方布各持续 600 tick / 5 s，在 tick 1、2、10、30、60、120、600 采样，位置/速度差最大值均为 0。原 3 mm/0.5 m/s 门槛未放宽。
- 512 点选择组内，528 点选择原路径；强制超容量、非法模式被拒绝；3 次派发预算允许组内 tick、拒绝超预算原路径且拒绝时状态不变。
- 受影响求解 **7 组/2905 次/160 次 CPU 对拍**与实际绘制/预览时钟 **5 组/255 次/36 个解析像素分量**通过，报告 [cloth-draw-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T231001Z/cloth-draw-evidence.json)。CPU/GPU 最大位置差 0.2596 mm，旗帜最终态边长误差 P95 0.7976%，1 m 方布 0.01815%，锚点漂移均为 0。

配对计时每样本执行两次 120 Hz tick，先热身 8 个样本，再测 60 个；按奇偶交替先后顺序，用同一实际队列的时间戳测 GPU 区间。报告保存四组完整原始样本数组及 100000000 Hz 的实际时间戳频率，时间戳必须正向且非空。测试仅映射已完成的验证/时间戳资源；生产求解器不读回或等待。CPU 指标只含命令记录，不含等待和游戏逻辑。

| 负载 | 原路径 GPU P50/P95 ms | 组内 GPU P50/P95 ms | 原/组内 CPU 记录 P95 ms |
|---|---|---|---|
| 1×280 点旗帜 | 2.63220 / 9.46152 | 0.75980 / 0.76804 | 1.9364 / 0.2192 |
| 4×512，共 2048 点 | 9.71868 / 16.80896 | 3.39772 / 9.15800 | 6.2310 / 0.6714 |

两个夹具的 GPU P95 均比原路径改善至少 25%，本批相对门槛通过。2048 点 P95 仍高于 Low 档四系统合计 4 ms 的目标；该表没有粒子、LEF、水、绘制或完整游戏帧，不能宣称组合预算达标。继续需要实例批处理、工作组占用和完整场景时间分布优化。

上一轮相同范围的测量保存在 `20261023T230107Z/cloth-grouped-evidence.json`，2048 点原/组内 P95 为 17.16976/9.20204 ms。不同批次存在尾部波动，两轮都保留；不使用最优一次结果声明完整生产预算达标。

## 宿主与剩余边界

新增 `--cloth-execution auto|dispatch|grouped`。默认 Automatic 选择实际可用路径；报告包含 `groupedInstances`、`dispatchInstances`、`groupThreads` 和 `sharedBytesPerGroup`。显式模式同样遵循真实 fence、暂停、传送/reset、设备重建与共享 buffer 总预算。预览依旧只有均匀风，宿主场租约/发布没有在本批补齐。

宿主增量 **76/76**，21 个运行，报告 [cloth-host-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T230320Z/cloth-host-evidence.json)。自动组内与显式 Dispatch 的截图 SHA 相同；512 点四实例实际绘制 240 帧、960 次 draw、892800 个三角形，committed 模拟 buffer 共 3145728 字节，没有缩小拓扑。暂停/恢复/传送/reset、renderer 重建、中文路径、GPU 粒子同屏与非法模式拒绝继续通过。正常运行均实际启用调试层、errors=0；off/组内/2048 点的已有 warning 均为 723，原始消息保留。宿主只报告最新完成计时样本，不把它冒充该表的 P95。

归档后工作区继续新增相机/场景修改。保留用户改动，重建后立即冻结最新依赖源码和二进制，再从冻结可执行文件运行 off/grouped/dispatch 三项 240 帧兼容检查：验证层错误 0，组内与原路径截图 SHA 相同，三项玩法摘要一致。补充报告 [cloth-grouped-host-delta.json](../reports/0.11.0/31c00bc53c0b/20261023T232241Z/cloth-grouped-host-delta.json)及重放脚本一并保存；它不替代相机/场景新增分支的独立测试，原 76 项报告仍对应其自身冻结版本。

标准构建登记独立 benchmark 目标；生产性能门禁使用上述增量入口。全量 native GPU 门禁保留既有布料数值/绘制目标，本批未执行该完整入口。更大网格的派发路径仍保留且数值有效，尚未完成大网格、蒙皮/自碰撞/CCD、湿润、LOD、完整材质/工具和四系统联合预算验收。


2026-10-24 后续：已新增多实例完整 tick 与法线的统一派发、真实 fence 保护的有界元数据/描述符环，并接入宿主 `stepAll`。单独调用 solver 仍使用本页原路径；最新合同、逐位对拍和配对计时见 [CLOTH-BATCH.md](CLOTH-BATCH.md)。当前布料专项编译 15 个 compute＋2 个绘制 blob（17 个），完整清单在当轮为 75 个（GPU LEF 增量后当前为 80 个）。历史报告数值保持对应其冻结版本，不据此宣称完整 A–G 验收。
