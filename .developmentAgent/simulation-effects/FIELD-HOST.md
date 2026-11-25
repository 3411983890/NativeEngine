# 共享速度场的宿主发布与 GPU 消费

依据 [完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) B3/B5、C1，延续 [只读速度场合同](EFFECT-FIELD-CONSUMER.md)。本增量把显式场租约、CPU 发布、GPU 快照与粒子绘制接入同一宿主帧；通用场求解器和玩法权威调度仍是后续工作。

## 通用渲染器接口

初始化前调用 `Renderer::configureGpuEffectFields(sharedWorld, storageLimits, vectorLimits, error)`。World 由 CPU 生产者/调度器拥有，接口接受共享所有权，读取视图保留 World 的生命周期；渲染器不会生成或回写玩法数据。

`GpuEffectFrame` 新增四个仅在回调期间有效的函数：

| 回调 | 合同 |
|---|---|
| acquireField(request) | 取得只读空间租约，含区域、通道、一致性、读取 halo 与页预算 |
| releaseField(lease) | 撤销空间需求，已有读取 view 仍保留到其实际使用 fence |
| publishField(lease, spec, fieldTick) | 要求 World 已提交同一 fieldTick；记录缓存发布，返回不可变向量绑定 |
| stepField(fieldTick, error) | 按显式场时间执行一个粒子 tick，拒绝时间/绑定不匹配 |

```cpp
renderer.configureGpuEffectFields(world, storageLimits, vectorLimits, error);
// configureGpuEffects 的帧驱动中，检查每次调用结果：
auto lease = frame.acquireField(readRequest);
// CPU 生产者先提交全部分量，调度器执行 World::commit(fieldTick)。
auto view = frame.publishField(lease.lease, metreGridAndChannels, fieldTick);
auto instance = frame.runtime.spawn(assetId, origin, seed, view.view);
frame.stepField(fieldTick, error);
// 后续 tick 显式发布新绑定，再对每个实例 runtime.bindField(...)
// 最后需求结束：frame.releaseField(lease.lease)。
```

回调不向宿主暴露 command list 或猜测提交 fence。上传、模拟、汇集、排序和绘制使用渲染器的同一图形队列，切换场 heap 后恢复贴图 heap。计时覆盖场上传和相关 GPU 工作；CPU 发布成本仍由 CPU 帧统计反映，不能把 GPU 时间当作总成本。

配置在 resize/renderer 重建中保留 World 和预算，新设备重新建立缓存；驱动按 epoch 重新申请租约，旧 GPU 视图不能转移到新设备。原生 storage 增加共享 World 构造形式，避免外部 view 比渲染器活得更久时留下悬空 World。调用者仍须履行 fence 生命周期；析构不隐式等待。

## 可创作的 CPU 向量生产者

`fields/vector_source.*` 加载 `nativegpu.vector-field/1` JSON，示例 `assets/fields/gust_wind.field.json`。它按显式米制网格，在有界区域及读取 halo 上发布空间均匀、随 tick 变化的 windVelocity / waterVelocity。空间非均匀生产者可直接通过上面的 World 接口发布，不受这个示例生产者形式限制。

资产包含 id、domain、originMetres、cellMetres、region、haloCells、maxAgeTicks、loopTicks 和 tracks。每个向量键是 `{tick, metresPerSecond:[x,y,z]}`，解析时量化为整数毫米/秒，运行时使用带符号整数有理插值和四舍五入（中点远离零）。每个标量声明 `m/s`、unitsPerInteger=1000，不将未知分量当作零。

范围：最多两个向量、每个最多 256 个键、最多 432000 tick 的键/循环、每轴速度绝对值最多 10000 m/s、halo 为 0/1；含 halo 的区域最多 32768 个采样点，单轴最多 64 个点。循环轨迹的首尾值一致，避免隐式跳变；非循环轨迹保留最后一个键。插值不使用渲染帧浮点累积。1 mm/s 是量化步长；相对作者线性轨迹还存在键值量化和中间结果舍入，不能将步长当作完整流体误差界。

`VectorSource::stage(tick)` 原子发布本资产全部分量，调用者负责 World.commit。失败不发布半个向量或留下首次申请的写租约。构造只注册通道，不申请页；stage 取得写需求，release 撤销贡献与需求。没有消费者且没有生产者写需求时，CPU committedBytes 回到零。通道定义和作者键保留，后续可重新申请区域。

该 CPU 类可由权威调度器使用 CpuAuthority 数据域，并有 CPU 重放参考；当前宿主预览驱动只接受 VisualReference 资产。预览按已消费的视觉逻辑 tick 推进独立单调场时钟，保留 renderer 重建前的时间；已经撤销的贡献重新出现时必须使用未提交过的新 tick。它尚未进入 Game 存档/回放摘要，报告明确标记 gameAuthorityIntegrated=false，不能通过修改 domain 字段伪装成玩法权威更新。

## 运行与资源回收

```powershell
npm run effects:fields:host
npm run effects:fields:host -- -SkipBuild

build/effects-host/NativeGPU.exe --effect wind_drift --effect-backend gpu --effect-count 4 --effect-library build/field-host/cooked/library.json --effect-field assets/fields/gust_wind.field.json
```

未指定 effect-field 时保持原有 CPU/GPU 精灵入口；含场资产没有绑定时明确拒绝。网格大小不匹配、非法 JSON 和将预览配置为 cpu-authority 也明确拒绝。

默认预览上限为 GPU 场存储 **16 MiB / 16 个快照**、向量元数据 **2 MiB / 32 个绑定**，另受 64 页及租约/view 上限限制。这些是预算，不是启动时全部分配。四实例共享每 tick 的一个绑定；示例区域及 halo 只需三个标量砖页。报告分列 CPU 页、GPU 页、在途版本、上传资源、元数据和拒绝数量，粒子池预算不重复包含共享场内存。

立即停止和自然排空结束空间需求；GPU 旧版本在已完成 fence 后回收。CPU 生产者同时释放写租约，CPU 的在途页按同一队列完成值回收。宿主配置使用弱引用，不把旧读取 view 固定在旧设备上。该回收不意味着删除作者资产或通道定义。

当前预算耗尽会显式拒绝新场发布/绑定；统一的视觉降档、继续旧版本及帧预算阶梯尚未接入。当前逐 tick 打包 CPU 场，没有 GPU 场求解器、脏砖增量上传或跨队列所有权转移。示例 waterVelocity 是作者三分量场，不是现有水体高度/XZ 查询代理，也不代表水体质量/动量交互已经完成。

## 增量验收

独立入口只编译向量源 CPU 参考及宿主依赖，运行受影响场消费、默认后端、停止、重建和错误路径；不运行全量原生、JS、simulation:suite 或 verify。输入、编译器和二进制 SHA 在编译/运行/归档前后核对，来源不匹配拒绝 SkipBuild。报告冻结独立 cook、作者资产、源码、二进制、真实截图和原始宿主输出。

验收覆盖：零风与动画风的同 seed 画面差异、实际间接绘制、默认 CPU/GPU 不分配场、世界摘要不受视觉风场影响、source/field tick 匹配、配置预算、停止后 CPU/GPU 页和元数据归零、反复自然排空再申请、renderer 重建后的租约恢复、流场读取，以及未绑定/网格不匹配/非法资产/错误权威域的拒绝。

最终专项为 **41/41**，CPU 源 **6 组/2829 次**；Windows PowerShell 入口实际运行并覆盖中文资产路径。AMD 890M 的所有正常宿主运行实际启用调试层，errors=0。报告：`reports/0.11.0/31c00bc53c0b/20261023T151631Z/field-host-evidence.json`。四个风场实例实际绘制 240 帧、720 次间接 draw，共享 479 次发布/绑定，当前三页，六个在途快照；默认预算内，没有拒绝。

停止后 CPU/GPU 页、CPU 租约/来源、上传资源和向量元数据均为零。自然排空/重新发射运行有 242 次源发布，最后无租约；这个视觉生命周期依据异步退休完成时间，次数不承诺跨 GPU 确定性。重建后场 tick 476、重建粒子 tick 158，证明场源没有随视觉池重启。零风/动画风/关闭效果的 Game 摘要均为 `d31992613e8c6981`。

本批发现字段页以 COPY_DEST 申请时被驱动强制按 COMMON 创建，每 tick 增加三条提示。现按 COMMON 创建并显式转入复制状态，原来的 2197 条 warning 降为 760，与同场景普通 GPU 粒子相同；现有清除/其他资源提示保留，没有过滤消息。修正前报告和旧二进制保留在 `20261023T151016Z`，并未改写历史结果。

受影响的共享场 GPU 专项重新通过 **7 组/25760 次**（`20261023T151523Z`）；速度消费及共享 World 所有权专项 **9 组/1675 次/827 次向量对拍**（`20261023T151535Z`）。只运行这些增量，没有全量回归。打包配方已加入字段资产和文档；本批未执行完整打包/发布门禁。

收尾审计为 `reports/0.11.0/31c00bc53c0b/20261023T152440Z/field-host-finalization.json`：36 项字段输入与冻结副本、176 项宿主冻结输入一致。随后用户在导入资产分支新增了身份验证，字段集成段逐字未变；已保留并编译这些新修改，最新构建另作 build-only 归档，没有将旧 41 项报告套为新增导入验证通过。


2026-10-24 后续共享场接入：新增 `--simulation-field`，无需 GPU 粒子也可为布料提供风，或让粒子/多布共享一份快照。旧入口与本页历史报告保留，最新时间/租约/后绑定/根签名合同及 100 项宿主证据见 [SIMULATION-FIELDS-HOST.md](SIMULATION-FIELDS-HOST.md)。
