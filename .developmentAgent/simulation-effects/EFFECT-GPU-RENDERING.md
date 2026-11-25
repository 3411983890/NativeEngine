# GPU 粒子绘制与宿主接入

依据 [完整开发方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) C0–C2；模拟与实例合同见 [EFFECT-GPU-REFERENCE.md](EFFECT-GPU-REFERENCE.md)。这是 unlit sprite 的实际 GPU 模拟/排序/绘制通路，六类渲染器与完整 C 阶段尚未完成。

## 数据与顺序

`renderer/gpu_effect_sprites.*`、`shaders/gpu_effect_sprites.hlsl` 消费 GpuRuntime::prepareDraw 产生的 GPU SoA、存活列表与计数。CPU 不读回粒子位置或存活数来制作绘制列表。

1. GPU 重置有界输出计数，按每个发射器/renderer 从存活计数生成 ExecuteIndirect 汇集参数。
2. GPU 将 position/color/size 与局部原点写入 64 字节 sprite 记录，保留每粒子的贴图、blend、来源批次和槽号。多个 renderer 可重复消费同一模拟池，按绘制记录容量计算成本。
3. 全局排序先排列 cutout、premultiplied、additive。预乘透明按投影中心的 z/w 从远到近排序，跨实例、发射器和贴图；cutout 从近到远；additive 可按贴图归组。相同键以批次与槽号打破平局，不承诺跨 GPU 的浮点图像逐位确定性。
4. GPU 将真实数量填充到下一 2 的幂，补入哨兵；bitonic 比较交换只调度实际填充范围，超出当前数量的阶段间接派发数为 0。输出容量本身必须是 2 的幂。
5. GPU 生成三份 `{6, count, 0, 0}` 绘制参数与显式分段偏移。VS 按 GPU 偏移读取自己的 blend 段，不能把 StartInstanceLocation 当成结构化缓冲区的通用偏移。该参数的官方定义是对顶点缓冲区逐实例索引加偏移：[DrawInstanced](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawinstanced)。
6. 三次 ExecuteIndirect 实际执行 billboard VS 与贴图 PS。cutout 裁剪并写深度；预乘透明和 additive 测试深度、不写深度。两份 HDR MRT 沿用现有 unlit 合成合同。

每条记录携带贴图索引，PS 使用 NonUniformResourceIndex，切换贴图不会打断透明深度顺序。没有 CPU 粒子上传或逐粒子描述符分配。与其他透明场景几何的共同排序尚未实现；这里的“全局”限定于这一 GPU 粒子绘制集合。

## 资源与失败行为

- 默认最多 262,144 条绘制记录，输出容量可配置为 `[1,262144]` 内的 2 的幂；汇集最多 8,192 个来源，来源容量之和最多 8,000,000。超出来源预算明确拒绝，输出记录超限在 GPU 上丢弃并计数，不越界。
- 四份输出 buffer：64 字节记录、64 字节计数/分段状态、12 字节派发参数、48 字节绘制参数。默认实际 committed allocation 合计 **16,973,824 字节**，包含分配对齐；构造前完整核对 maxAllocationBytes。PSO/根签名内部驱动内存不包含在这个 buffer 数字中。
- GPU 状态词为 count、三种 blend 数量、dropped、faults、padded、reserved，以及三种 blend 的分段起点。宿主当前汇报的 dropped 来自模拟实例预算，尚未异步汇总这些绘制丢弃计数，不能混用两者。
- 输入池必须已 prepareDraw，属性/存活列表保持 NON_PIXEL_SHADER_RESOURCE。模块临时将输入计数从 UAV 转为 SRV，汇集后恢复 UAV。输出 ready 时记录和状态为 NON_PIXEL_SHADER_RESOURCE，draws 为 INDIRECT_ARGUMENT。
- 调用者在 prepare 之前绑定正确的 shader-visible 贴图 heap，在 draw 前绑定两份 RTV、DSV、viewport、scissor。贴图索引必须属于该 heap；宿主配置层验证作者贴图槽，模块拒绝缺失的贴图映射。
- prepare/draw 必须使用同一队列与单调提交 fence，全部输入与输出资源保留到最后使用完成。shutdown 拒绝未完成 fence，析构不隐式等待。共享输出没有 CPU 写入，下一帧通过同队列 barrier 保证上一帧读取完成后再重用。
- 全局 bitonic 排序暂未替换为分组 radix/更大规模排序服务；目前不宣称 128 个高负载实例或完整游戏帧预算已通过。

## 宿主与创作调用

`Renderer::configureGpuEffects` 在初始化前注册不可变资产、贴图槽与帧回调。回调收到 GpuRuntime 和只在本次回调有效的 step 函数；由渲染器提供真实图形队列、命令列表与 fence，宿主只作逻辑实例和 tick 决策。step/实例操作必须按逻辑顺序执行，不能先批量推进时间再提前应用未来 tick 的创建/停止。

`renderer/gpu_effect_bridge.cpp` 在帧中先调用驱动，再准备并排序 GPU 记录，随后在场景深度完成后绘制。CPU 与 GPU 效果后端不能同时向同一 renderer 提交粒子，以免把两个独立的透明集合误标为共同排序。

```powershell
build/effects-host/NativeGPU.exe --effect sprite_demo --effect-backend gpu --effect-count 4
build/effects-host/NativeGPU.exe --effect sprite_demo --effect-backend cpu --effect-count 4
```

默认仍为 CPU reference + GPU sprite。GPU 模式支持现有 count、repeat-ticks、stop-tick、origin 控制；含场资产可通过 [宿主场绑定与作者预览源](FIELD-HOST.md) 运行，未绑定时及未实现的事件/受光/其他 renderer 会拒绝，不静默转成另一个效果。独立 GpuRuntime 的速度模块接口见 [场消费合同](EFFECT-FIELD-CONSUMER.md)。设备或 renderer 重建后视觉状态从 seed 重启，CPU 场源保留独立时间，不回写游戏权威状态。暂停/移动可由帧回调调用 GpuRuntime，完整编辑器命令桥与保存恢复仍待开发。

模拟/汇集/排序与绘制有独立的、按帧 fence 完成后读取的 GPU 时间戳；整帧时间戳起点位于这些工作之前，避免预算统计漏掉 GPU 预处理。不将此短时测试当成性能达标结论。

## 增量验证

```powershell
npm run effects:gpu:draw -- -Adapter AMD
npm run effects:gpu:draw -- -WithHost -Adapter AMD
npm run effects:gpu:draw -- -SkipBuild -WithHost -Adapter AMD
```

默认只运行绘制专项；WithHost 增加受影响宿主路径。独立宿主构建只构建宿主及所需依赖，不构建其他测试或运行全量回归。shader 专项编译两个相关文件；加入速度场变体后现为 17 个 blob，旧报告的 16 个计数保留。宿主构建使用全套 shader 依赖并保留缓存。C++ 对象缓存核对编译器、选项、真实头文件依赖与对象 SHA，未通过核对的对象重新编译。测试前后验证输入和二进制来源，报告冻结二进制、源码输入与截图。

已通过的绘制专项为 **5 组、604 次检查**：257 条非 2 次幂记录跨实例/贴图全局排序；实际 VS/PS pipeline 查询；三种 blend、裁剪孔洞、深度遮挡共 16 项解析像素对拍；输出溢出计数、空帧清理、局部坐标、分配拒绝、缺贴图与真实 fence 下的提前释放拒绝。AMD 890M，debugLayer=true、debugErrors=0。修复前的混合模式分段偏移缺陷在 `reports/gpu-effect-draw-failure-20261023/` 冻结，旧二进制实际复现第 575 次检查失败。

宿主验证还发现初始化会丢失 validation 开关，导致即使传入 --validation 也没有启用调试层；已保留开关并按帧累计完整 errors/warnings，保留前 64 条消息用于诊断。现有默认场景有 buffer 初始状态与未提供优化 clear value 的 warning；它们与 error 分开记录，不将 warning 数字写成 0。

最终版本报告：`reports/0.11.0/31c00bc53c0b/20261023T063945Z/effects-gpu-draw-evidence.json`。期间又检测到材质/mesh shader 与主渲染源码更新，保留改动后重新构建及验证相同增量范围；旧报告不冒充新源码通过。绘制专项 **5 组/604 次检查**，入口 **25/25 项检查**（含专项结果及受影响宿主路径）通过：

- off、默认 CPU、GPU additive、GPU premultiplied、GPU cutout、立即停止、窗口/renderer 恢复均完成；七类正常运行都实际启用 validationLayer，errors=0。
- GPU 模式 **240 帧、960 次汇集间接命令、21,840 次排序间接命令、720 次绘制间接命令**；其中较大排序阶段可能由 GPU 派发 0 组，不将这个 CPU 记录数写成有效粒子工作量。CPU 精灵上传绘制帧为 0，效果截图不同于 off。
- 立即停止后的截图 SHA 与 off 完全一致，实例与 alive 均为 0。原 CPU 路径仍绘制 240 帧。
- 一次窗口尺寸变化、一次 renderer 重建后，GPU 通路又绘制 **80 帧**；视觉状态按声明从 seed 重启。
- smoke_reference 的未接入 GPU 场声明返回 NG-E-EFFECT-UNSUPPORTED，没有被帧提交层覆盖成普通绘制错误。
- GPU 模式有 **237 次已完成的计时样本**；最后一个模拟/排序样本 0.70008 ms、绘制 0.00216 ms，均纳入整帧计时。这是 AMD 890M、四个 sprite_demo 实例的单次样本，不能代替压力分位数或整帧性能验收。
- off 为 723 条 warning，GPU 为 760 条；均 errors=0。warning 包括缓冲区初始状态提示和缺少优化 clear value 的性能提示；没有声称宿主调试消息全部为 0。

本轮只执行绘制专项与上述宿主路径，没有运行完整原生/JS 回归或 full verify。宿主依赖构建共 57 个 shader blob；最后一次 C++ 构建重编 10 个对象，复用已核对真实依赖 SHA 的 23 个对象。报告目录的 source-snapshot.json 与 host-source-snapshot/ 另行冻结本次宿主全部源码输入与依赖。

后续只读速度场增量按最新源码重新构建宿主（58 个 shader blob），绘制专项 **5 组/604 次**和受影响宿主 **26/26** 通过，报告 `reports/0.11.0/31c00bc53c0b/20261023T102116Z/effects-gpu-draw-evidence.json`。新增未绑定风场的明确拒绝，并在独立目录按当前贴图依赖烘焙宿主测试库；不覆盖默认 cook。源码、二进制和资产均冻结并核对 SHA，编译期间来源变化会拒绝通过。这里仍未接入宿主场生产者。

尚缺：受光/软粒子、flipbook/轴锁/拉伸、与其他透明几何共排、其余五类 renderer、GPU 碰撞/事件/场写入、玩法权威与场景/编辑器生产者、活跃热迁移、绘制丢弃遥测接入宿主、完整创作控制桥及大规模性能门禁。标准构建与可选 GPU 测试入口已登记本专项，本轮未执行这些完整入口。
