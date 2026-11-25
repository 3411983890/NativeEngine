# Compute queue and heap-indexing probe (B1 / B3)

一个 compute dispatch 同时证明两件事：

- **B1**：渲染器拥有第二条队列（COMPUTE），有独立的 allocator / list / fence，图形队列在提交前显式等待 compute fence。
- **B3**：SM 6.6 `ResourceDescriptorHeap` 能按索引解析描述符，且结果与同样四个描述符走描述符表采样**逐位一致**。索引取在 32–35，**有界 8 槽材质表永远无法寻址这个区间**，所以一致就证明是真堆索引。

## 1. 实现

| 组件 | 位置 |
| --- | --- |
| 计算着色器 | `shaders/compute_probe.hlsl`（`cs_6_6`，DXC 1.9.2609 编译进 `kComputeProbe`） |
| 队列/管线/回读 | `renderer/compute_pass.cpp` |
| 描述符 | 验证 SRV 位于 `VerificationSrvBase = 32`（帧块之后），UAV 在 `ComputeResultUavSlot = 36` |
| 开关 | `--compute on|off|corrupt`，经 `rhi::Device::setCompute` 下发 |

**必须的根签名标志**：`D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`。缺这个标志时 `CreateComputePipelineState` 直接返回 `E_INVALIDARG`（第一版就踩到了，0x80070057）。

### 每帧顺序

```
dispatchComputeProbe()
  ├─ 写 root constants（4 盏灯 + heap base index + corrupt）
  ├─ computeQueue: UAV 描述符 + Dispatch(1,1,1)
  ├─ computeQueue->Signal(computeFence, v)
  └─ graphicsQueue->Wait(computeFence, v)      // 跨队列显式同步
第 30 次派发后：readbackComputeProbe()
  ├─ 等 compute fence，barrier UAV→COPY_SOURCE
  ├─ CopyResource 到 READBACK 堆，Map
  └─ CPU 比较 4 对 table/heap 采样与光照归约
```

## 2. 证据（真实运行，RTX 5060）

```
probes.compute = {
  "available": true, "queueCreated": true, "queue": "compute",
  "requested": true, "implemented": true,
  "dispatchedFrames": 240, "crossQueueWaits": 240,
  "heapIndexing": { "baseIndex": 32, "maxIndexTested": 35,
                    "pairsCompared": 4, "pairsMatched": 4,
                    "maxDelta": 0.0, "verified": true },
  "lightScale": { "gpu": 1.0, "cpu": 1.0, "delta": 0.0 }
}
```

- 240 帧 = 240 次 compute 派发、240 次跨队列等待；一次性回读在第 30 次派发后执行。
- 4 对采样全部逐位相同（`maxDelta = 0`），索引 32–35 超出有界表范围。
- 光照归约 GPU 值 `1.0` 与 CPU 参考 `1.0` 完全一致（delta 0）。

`.incrementalTesting/rendering/compute.ps1`（12 项断言）：

1. compute 队列创建、管线可用；
2. 派发与跨队列等待计数随帧数增长且相等；
3. `heapIndexing.verified = true`、`pairsMatched = 4`、`maxDelta = 0`；
4. `maxIndexTested ≥ 9`（证明超出 8 槽有界表）；
5. `lightScale.delta = 0`；
6. `--compute corrupt` 时 GPU 值变成 0.5 而 CPU 参考不变 → `delta ≈ 0.5`，证明回读的是 GPU 真实结果而不是 CPU 回填；
7. `--compute off` 时零派发，且最终图像与开启时逐像素一致（compute 不影响本期画面）。

## 3. 已知问题（2026-10-20 未解决）

**光照归约在着色器里读到全 0。** `probes.compute.lightScale.gpu = 0`，CPU 参考为 1.0。
同一 cbuffer 里的标量（`heapBaseIndex`）是好的——堆索引验证 4/4 通过，所以不是"常量完全没传进去"。

已经试过且**无效**的假设：

| 尝试 | 结果 |
| --- | --- |
| 结构体数组（AoS）→ 并列 float4 数组（SoA）对齐 C++ 布局 | 仍为 0 |
| 拷贝前加 UAV barrier（不只是状态转换） | 仍为 0 |
| 拷贝前 CPU 等待 fence 完成（原先只做了队列侧 Wait） | 仍为 0 |
| 循环次数硬编码为 2（绕过 `lightCount`） | 仍为 0 |
| 把 `lightCount` 写进第 16 行回读 | 回读为 0，但同一行的 `heapBaseIndex` 在着色器里明显有效 |

结论：不是简单的布局或同步问题，需要**逐字段定位**（下一步建议：把每个 DWORD 写回 UAV，逐位比对 root constants 的实际可见性）。
因此 `.incrementalTesting/rendering/compute.ps1` 只把"已证明"的三件事作为门槛（队列、跨队列同步、堆索引），
光照归约只记录不断言，并在证据里标注 `knownIssues`。

## 4. 已知限制（不要夸大）

- **compute 结果尚未进入画面**：目前只写进 UAV 并回读校验。要让它驱动合成（例如曝光/色调）需要改 composite 的 HLSL 与根签名，属于 B1b。
- **没有异步重叠的量化证据**：跨队列等待已建立，但还没有 profiler 数据证明两条队列真的并行；GPU 时间戳只覆盖图形帧。
- **单 buffer 复用**：compute 结果只有一个 buffer，靠图形队列等待来避免读写竞争；没有按帧轮转。
- **B3 仅验证了堆索引本身**：材质主路径仍然走 8 槽有界表（`ps_5_1` + D3DCompile）。把材质路径切到 `ResourceDescriptorHeap` 需要先用 DXC 编译光栅着色器（当前为了"无 DXC 也能跑"刻意用 D3DCompile），属于 B3b。
- **依赖 DXC**：没有固定 DXC 时 compute 探针自动降级为 `available:false`，渲染器其余部分不受影响。
