# 渲染输出契约（H07 / 创新 INN-20）

- 状态：**契约冻结草案**。本文只冻结"渲染器把一帧交给宿主"的接口约束，不代表 S2 已开工（见 `docs/CHROMIUM-START.md`、`docs/STATUS.md` §2 的 S2 行）。
- 依据代码：`renderer/d3d12_renderer.h`、`renderer/d3d12_renderer_internal.h`、`renderer/d3d12_renderer.cpp`、`host/main.cpp`（0.6.0 工作树）。
- 标记约定：**已实现**＝当前代码能直接读到的行为（附文件＋符号）；**S2 要求（当前未实现）**＝契约需要但代码里还没有；**待 S2 验证**＝无法从现有代码判定，必须实测。

## 1. 范围与非目标

### 1.1 本契约覆盖

- 合成完成的一帧如何被宿主取走：一份 GPU 资源、一个可等待的同步点、一组尺寸/格式字段。
- 时机语义：渲染器在什么时刻之后保证像素可见、宿主在什么时刻之前必须归还资源。
- 故障语义：resize、最小化、设备丢失、宿主停消费时，双方各自看到什么。
- 一对一关系：**一次 `render()` 成功返回 = 最多一帧可被合成**，GPU→GPU 传递，图像数据不经 CPU。

### 1.2 明确不覆盖

| 不覆盖 | 说明 |
| --- | --- |
| CPU 回读 | 不接受"把 composite 拷到 `READBACK` 堆再上传"的路径；现有截图 API 是诊断工具，不是输出通道 |
| 音频 | 本仓库无音频系统（`docs/DEV-TASKS.md` E03 记录） |
| 输入 | 契约只定义输出方向；输入归 S2 的 H05，不在本文 |
| 多窗口 | `Renderer` 绑一个 `HWND`、一条交换链（`initialize(HWND, ...)`），无多视口能力 |
| 场景/资产所有权 | 宿主每帧传 `DrawItem`/`LightDesc`；渲染器不持有场景图 |
| 编辑器 UI / 合成器策略 | 谁在上面叠 UI、何时刷新由 Chromium 决定 |

## 2. 资源身份

### 2.1 当前实现的资源画像

| 项 | 契约值 | 代码依据 |
| --- | --- | --- |
| 像素格式 | `DXGI_FORMAT_R8G8B8A8_UNORM` | `detail::ColorFormat`（internal.h:23）；`createTargets()` 用它建 `composite[index]` |
| 尺寸 | 交换链缓冲的物理像素 `width × height` | `Renderer::resize(width, height)` 设定；`createTargets()` 按 `Impl::width/height` 建纹理 |
| 维度 | `TEXTURE2D`，`MipLevels=1`，`DepthOrArraySize=1`，`SampleDesc.Count=1` | `detail::textureDescription2D()` |
| 堆 | `D3D12_HEAP_TYPE_DEFAULT` | `Impl::createTexture2D()` |
| 标志 | `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET`；**无** `ALLOW_SIMULTANEOUS_ACCESS`，**无** `ALLOW_UNORDERED_ACCESS` | `createTargets()` 的 composite 分支 |
| 初始状态 | `D3D12_RESOURCE_STATE_RENDER_TARGET` | 同上 |
| 帧末状态 | `D3D12_RESOURCE_STATE_COPY_SOURCE` | `render()` 中 `transitionTo(state.composite[index], COPY_SOURCE)` |
| 轮转数量 | **3**（`detail::FrameCount`） | internal.h:20；对外见 `capabilitiesJson().bufferedFrames` 与 `probesJson().renderTargets.frames` |
| 每帧归属 | `composite[index]`，`index = swapChain->GetCurrentBackBufferIndex()` | `Renderer::render` 开头取 index，`lastFrameIndex` 记录 |
| 内容 | `saturate(base.rgb + key.rgb * saturate(visibility))`，alpha 取 `base.a` | 内联 `CompositeShaderSource::PSMain` |
| 颜色空间 | **未声明**：无 `SetColorSpace1`、无 `_SRGB` 格式、shader 不做 gamma 编码 | 全文件 0 处 `DXGI_COLOR_SPACE` / `SetColorSpace` |
| 与交换链关系 | composite 是离屏资源，**不是**后备缓冲；帧末 `CopyResource(renderTargets[index], composite[index])` | `render()` 第 1572–1581 行 |

### 2.2 尺寸语义（物理 vs 逻辑）

- 渲染器只认**物理像素**：`Renderer::resize(unsigned width, unsigned height)` 的入参直接进 `createTargets()`，内部不保存 DPI 或 scale，也没有"逻辑尺寸"概念。
- 宿主侧现状：`host/main.cpp` 只调用 `SetProcessDPIAware()`，用 `GetClientRect` 的像素值减面板宽（`layout()`），全程无 `GetDpiForWindow`/无缩放系数上报。
- 契约：宿主负责 `逻辑视口 × deviceScaleFactor → 物理像素`；渲染器不感知 DPI。S2 要求（当前未实现）：`Resize(width, height, scale)` 回传实际采用的 scale，避免两侧各算一次。

### 2.3 单共享纹理变体（"one shared target + duplicate"）

若 Chromium 侧只能绑定**一张**共享纹理（单缓冲 `SharedImage`），契约退化为：

1. 渲染器保留内部 3 个 `composite[]` 轮转，**不得**把渲染直接写进共享纹理（否则 resize / 设备丢失时句柄无法重建）。
2. 每次 `FrameReady` 前多做一次 GPU copy：`composite[index] → 唯一共享纹理`，`copiesPerFrame` 从 1 变 2，多出的成本必须计入报告。
3. 共享纹理只有 1 帧深度：宿主必须在下一个 `FrameReady` 之前完成合成，否则渲染器**丢弃该帧**，不得等待。
4. 该变体下 `waitMs` 必然上升，属可接受退化；但不得用"跳过 copy 直接渲染进驻留纹理"来省这一步。

S2 要求（当前未实现）。当前代码 0 处 `CreateSharedHandle` / `OpenSharedHandle` / `ALLOW_SIMULTANEOUS_ACCESS`，`composite[]` 是普通 `DEFAULT` 堆纹理，无法被其他进程/设备打开。

## 3. 生命周期与同步

### 3.1 当前实现的时序

| 步骤 | 执行者 | 代码位置 |
| --- | --- | --- |
| 1. 等该 back buffer 上一帧的 GPU 完成 | 渲染器 | `render()` 开头 `waitFor(state.frameFences[index])`，超时 `detail::FenceTimeoutMs = 5000` |
| 2. 记录并提交命令列表 | 渲染器 | `list->Close()` → `queue->ExecuteCommandLists()` |
| 3. 呈现 | 渲染器 | `swapChain->Present(1, 0)`，同步间隔 1，**阻塞到 vsync** |
| 4. GPU 信号 fence | 渲染器 | `queue->Signal(state.fence, fenceValue)`，`nextFence++`，写入 `frameFences[index]` |
| 5. 失败检查 | 渲染器 | `presentResult` 失败时追加 `device->GetDeviceRemovedReason()` |

关键点：fence 在 `Present` **之后** signal，所以 `frameFences[index]` 完成 ≈ 该帧已提交且已呈现；宿主可合成点是"该 fence 值完成、且期间未发生 resize"。

### 3.2 同步对象归属

- 只有**一个** `ID3D12Fence`（`Impl::fence`），用递增的 `nextFence` 值区分帧，每个帧索引记录在 `frameFences[FrameCount]` 里。
- 渲染器是唯一 signal 方；宿主只等待，从不 signal。渲染器**从不**等待宿主（S2 契约要求；当前实现的阻塞来自 `Present`，不是宿主）。
- S2 要求（当前未实现）：暴露一个可共享 fence（`CreateSharedHandle(..., D3D12_HEAP_FLAG_SHARED)`），随 `FrameReady(fence_handle, frame_index)` 一起给出；等待发生在 `composite` 写完之后，而不是 `Present` 之后。
- 待 S2 验证：跨进程 `ID3D12Fence` 共享句柄在 Chromium 侧（D3D11/ANGLE compositor）的等待方式与 `waitMs` 量级。

### 3.3 队列深度

- 结构上 3 帧（`FrameCount = 3`，交换链 `BufferCount = detail::FrameCount`，`DXGI_SWAP_EFFECT_FLIP_DISCARD`）。
- 实际上由于 `Present(1,0)` 同步等待 vsync，在飞帧数被压到 ≤1；`capabilities.bufferedFrames` 报的是 3（容量，不是实测并发）。
- 契约上限：**宿主最多积压 `FrameCount` 个未消费帧**；超过即触发第 4 节的丢弃策略。

### 3.4 设备丢失

- 检测点一：`Impl::waitFor()` 看到 `fence->GetCompletedValue() == UINT64_MAX` 或等待后仍未达标，报 `"D3D12 device removed while waiting for the GPU"`。
- 检测点二：`Present` HRESULT 失败，报 `"Present frame failed (...); device status ..."`。
- 诊断码 `NG-E-DEVICE-REMOVED` 由 `injectDeviceRemovalAtFrame()` 的注入路径产生（`Impl::addDiagnostic`）；`host/main.cpp` 只在 `deviceRemovalMode` 且错误串含 `DEVICE_REMOVED` 时 `restartRenderer()`。
- 契约：设备丢失后渲染器必须（a）停止 signal 该 fence，（b）让共享纹理句柄失效并上报 `OnSessionLost(reason)`，（c）允许宿主保留上一有效画面。当前实现只有 (a) 的前半（`render()` 返回 false 且不再提交）。

### 3.5 宿主归还规则

S2 要求（当前未实现），按 `FrameReady(fence_handle, frame_index)` 的语义：

1. 宿主在 `frame_index` 的 fence 完成后才可采样该纹理；采样结束（合成命令提交）即视为归还。
2. 渲染器在收到归还确认或轮转满 `FrameCount` 帧之前，不得覆写同一 `frame_index` 的纹理；因此宿主最长可持有 `FrameCount` 帧。
3. 归还缺失不算错误：第 4 节的丢弃策略接管，只计数不阻塞。
4. `frame_index` 在同一会话内单调递增，不复用以避免歧义；**跨 `Resize()` / 会话重建必须重新从 0 开始编号**，旧编号一律作废。

### 3.6 不阻塞规则

- 契约原文：**渲染器绝不阻塞在合成器上**。渲染器可以为"复用 3 帧前的资源"等待自己的 GPU，但不得为"宿主还没取走这一帧"等待。
- 现状**违反**：`Present(1,0)` 自带 vsync 等待；`resize()` 里 `waitIdle()` 全量排空 GPU；两者都让渲染线程的推进速度受外部节奏影响。
- S2 要求（当前未实现）：去掉 `CreateSwapChainForHwnd` 依赖，改为离屏合成 + `FrameReady` 上报，把"呈现"整体交给宿主合成器。

## 4. 帧节奏与 resize

### 4.1 `Renderer::resize` 现状真值表

| 输入 | 行为 |
| --- | --- |
| `width == 0 || height == 0` | 立即 `return true`，不重建（最小化路径） |
| 与当前 `width/height` 相同 | 立即 `return true`，不重建 |
| 其他 | `waitIdle()` 全量排空 → `releaseTargets()` 销毁全部目标 → `ResizeBuffers(FrameCount, w, h, ColorFormat, 0)` → `frameFences[]=0`、`timestampPending[]=false`、`gpuValid/gpuRtValid=false` → `createTargets()` |

含义：

- resize 是**同步硬切换**：调用返回时旧 `composite[]` 已销毁、新的一套已建好，不存在"半新半旧"画布；代价是调用线程阻塞整个 GPU 排空。
- `frameFences[]` 被清零而 `nextFence` 继续递增，因此**宿主不能跨 resize 比较 fence 值**；S2 的 `Resize()` 必须同时返回新句柄与新的基准 fence。
- 最小化：`WM_SIZE` 置 `app.minimized`，主循环 `if(app.minimized){Sleep(30);continue;}` 跳过 `render()`，也跳过 `resize()`；恢复时 `layout()` 用新客户区调用 `resize()`。**最小化期间不产生帧**，宿主必须保留最后一帧，不得等新帧、不得黑屏。
- 宿主不会请求 0 尺寸：`layout()` 用 `std::max(160L, ...)` 兜底；`resize(0,0)` 只在直接调 API 时出现（契约要求此时也必须返回成功且不破坏现有画布）。
- 视口稳定性：宽高比来自 `app.width/app.height`（`perspectiveLH(1.05f, width/height, ...)`），resize 后重算投影，因此不会把旧宽高比的画面拉伸；`--render-size` 会把这两个值钉死以复现截图。

### 4.2 停消费的失败模式

S2 要求（当前未实现）：

- 有界队列 = `FrameCount`（3），不得无限增长，不得靠加内存掩盖。
- 队列满时丢**最旧**未消费帧（不是最新帧），并 `++droppedFrames`。
- 延迟计数 = 最新 `frameIndex` 与宿主最近确认 `frameIndex` 之差，上限即队列长度；超过阈值上报 `sessionState=stalled`。
- 字段名沿用 `docs/CHROMIUM-START.md` §3：`copiesPerFrame`、`waitMs`、`compositeMs`、`droppedFrames`、`sessionState`。
- 宿主进程被杀时，渲染器应在 ≤3 帧内进入 `stalled` 并继续可被重新连接，而不是卡在 fence 等待上（当前 `FenceTimeoutMs=5000` 是唯一的兜底）。

## 5. 宿主不得假设的事

| 不得假设 | 事实 | 依据 |
| --- | --- | --- |
| 有 sRGB 编码 | 无 `SetColorSpace1`、无 `_SRGB` 格式、`PSMain` 不做 gamma 编码，UNORM 直写 | 全文件无 `DXGI_COLOR_SPACE`；`CompositeShaderSource::PSMain` |
| alpha 有混合语义 | alpha 直写 `base.a`（宿主当前恒填 1），无 premultiply、未定义混合；清屏 alpha=1 | `PSMain` 返回 `base.a`；`compositeClear[4]={0,0,0,1}` |
| 帧可跨帧累积 | 每帧 `ClearRenderTargetView` 重画；3 个 `composite[]` 轮转，同一 index 的内容会被下一次覆盖 | `render()` 合成段 |
| 交换链后备缓冲内容稳定 | `FLIP_DISCARD`，Present 后内容丢弃；且它是 composite 的**副本**，不是输出本身 | `swapDescription.SwapEffect`；`CopyResource()` |
| 渲染器持有场景 | 每帧由宿主传 `DrawItem`/`LightDesc`；渲染器不存场景图 | `Renderer::render` 签名 |
| CPU 时间等于帧成本 | `cpuMs` 含 `Present` 的 vsync 等待；`cpuSubmitMs` 才是提交前成本 | `d3d12_renderer.h` 的 `FrameStats` 注释 |
| 可多窗口/多实例 | 一个 `Renderer` 绑一个 `HWND`、一条交换链；`Impl` 是单实例状态 | `initialize(HWND, ...)`、`CreateSwapChainForHwnd` |
| 支持 tearing | 未设 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`，固定 `Present(1,0)` | `DXGI_SWAP_CHAIN_DESC1` 初始化无 `Flags` |

另外：`probes.renderTargets` 的 `mode` 是 `offscreen-mrt-plus-composite`，只有 `composite` 是契约输出；`baseColor`（`R16G16B16A16_FLOAT`）、`keyLight`、`shadowMask`（`R16_FLOAT`）是内部中间件，宿主不得采样。

## 6. S2 验收清单

| # | 条件 | 判定方法 | 证据产物 |
| --- | --- | --- | --- |
| A1 | 无 CPU 回读 | 会话期间 `copiesPerFrame` 恒为 1（单纹理变体为 2），且进程内无 `D3D12_HEAP_TYPE_READBACK` 纹理创建 | 帧捕获（本机 PIX/RenderDoc 不可用，见 `docs/CAPTURE.md` §1）＋`copiesPerFrame`/`readbackBytes=0` 报告 |
| A2 | resize 下视口稳定 | 连续 resize 期间每帧画布尺寸与 `Resize()` 返回值一致，无拉伸、无黑边、无 1 帧旧尺寸 | `resizeEvents`（`host/main.cpp` `report()`）＋逐帧 `frameSize` 序列＋前后截图逐像素对比 |
| A3 | 强制 kill 可恢复 | `taskkill /F` 原生进程后宿主重建会话并继续出帧 | 仿 `deviceRemoval` 字段：`recovered=true`、`recoveries>=1`、`frameAfterRecovery` |
| A4 | 普通页面拿不到句柄 | 非受信 origin 调 `NativeGpuSession` 被拒，且有审计记录 | 稳定错误码＋审计日志条目（`docs/CHROMIUM-START.md` §4） |
| A5 | 60 秒连续稳定 | 固定机位 60s，无撕裂、无逐帧回读 | `frames`、`droppedFrames`、`frameIntervalP95Ms`、`compositeMs` |
| A6 | 句柄与对象生命周期 | 会话结束后 `liveObjectCount()==0`，显存回落到基线 | `probesJson().reliability.liveObjects` / `videoMemoryBytes` |
| A7 | 不阻塞合成器 | 宿主故意停消费 10s，渲染器仍在 ≤3 帧内推进并计数，不卡死 | `droppedFrames`、`waitMs`、`sessionState=stalled` |

A1/A5 的抓帧判定工具本机不具备（`docs/CAPTURE.md` 记录 RenderDoc/PIX/D3D12 调试层均不可用），首轮只能用报告字段＋代码路径审查，结论标注**待 S2 验证**，不得写成已通过。

## 7. 已知缺口（今天没有任何一项是已实现）

| 缺口 | 现状 | 归属 |
| --- | --- | --- |
| 共享纹理导出 | 全仓 0 处 `CreateSharedHandle`/`OpenSharedHandle`/`ALLOW_SIMULTANEOUS_ACCESS`；`composite[]` 不可跨进程打开 | H04 |
| Mojo 会话 | 只有 `docs/CHROMIUM-START.md` §2 的接口草案，无 `.mojom`、无 `NativeGpuSession` 实现 | H02 |
| 隔离进程 | 无；渲染器与 Win32 宿主同进程，崩溃即全丢 | H03 |
| 帧节奏队列 | 无队列、无 `droppedFrames` 计数；`Present(1,0)` 同步 | H05/H06 |
| resize 换句柄 | `resize()` 不返回句柄；`ResizeBuffers` 后旧资源已销毁，旧句柄必然失效 | H04/H05 |
| D3D11 interop | Chromium compositor 侧需要 `D3D11Texture2D`/`SharedImage` 可导入的共享句柄，未做 | H04 |
| DPI 上报 | 只有 `SetProcessDPIAware()`；无 `GetDpiForWindow`、无 scale 回传 | H05 |
| 抓帧证据 | RenderDoc / PIX / D3D12 调试层均不可用 | 见 `docs/CAPTURE.md` §1 |

G2 未开始：`docs/STATUS.md` §2 的 S2 行仍是"未开始、无证据"。本文的所有"要求"都不得被解读为"已具备"。
