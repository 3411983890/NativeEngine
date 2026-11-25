# 渲染器边界（R06）

R06 不是"写一个 Vulkan 后端"，而是让帧路径依赖一个可被第二个后端实现的接口，并给出一套两个后端都必须通过的契约检查。当前只有一个实现（`d3d12`），第二个实现属于 X2。

## 1. 边界在哪里

```
host/main.cpp  ──(帧路径)──>  nativegpu::rhi::Device  ──>  nativegpu::Renderer (D3D12)
                                     │
                                     └─ 契约检查 conformance()
```

**在边界内**（`renderer/rhi.hpp` 的 `Device` 接口）：

| 方法 | 说明 |
| --- | --- |
| `backend()` / `initialized()` | 后端名与状态机 |
| `initialize(descriptor)` / `shutdown()` | 设备生命周期；`descriptor` 含 window、宽高、validation 开关 |
| `resize(w,h)` | 0 尺寸被拒绝 |
| `submit(viewProjection, eye, draws, lights)` | 每帧提交，宿主不再直接调 `Renderer::render` |
| `setRtShadows` / `setMeshShaders` / `setCompositeDebugView` | 质量与调试开关 |
| `requestScreenshot` / `captureScreenshot` | 帧内复制、帧后读取 |
| `stats()` / `capabilitiesJson()` / `probesJson()` / `videoMemoryBytes()` | 证据与预算输入 |

**在边界外**（有意的）：

- `injectDeviceRemovalAtFrame`、`selfTestInvalidPipeline`、`liveObjectCount`/`objectsCreatedCount` —— 这些是 G1 故障注入与对象计数测试钩子，属于 D3D12 后端的测试面。通过 `nativeHandle()` 取回 `Renderer*` 使用，第二个后端允许不实现。
- 场景、玩法、确定性摘要完全不经过边界。

## 2. 工厂与错误码

```cpp
auto device = nativegpu::rhi::wrap("d3d12", renderer, error);   // 当前唯一可用后端
nativegpu::rhi::backends();    // {"d3d12"}
nativegpu::rhi::supports("vulkan");  // false
```

| 错误码 | 触发条件 |
| --- | --- |
| `NG-E-RHI-BACKEND` | 请求了未编译进来的后端 |
| `NG-E-RHI-WINDOW` | 没有窗口句柄 |
| `NG-E-RHI-SIZE` | 宽或高为 0 |
| `NG-E-RHI-STATE` | 未初始化就 `submit`/`resize` |

## 3. 契约检查

`renderer/rhi.cpp` 的 `conformance(Device&, failures)` 对任意后端断言四条规则：

1. `backend()` 非空；
2. 新建设备不得自称 `initialized()`；
3. `submit` 早于 `initialize` 必须失败，且错误码以 `NG-E-RHI-` 开头；
4. 0 尺寸 `resize` 必须失败，且错误码以 `NG-E-RHI-` 开头；没有成功的 `initialize` 之后不得变成已初始化。

`tests/rhi_conformance.cpp`（13 项）用一个 `MockDevice` 通过全部规则，并用两个故意违规的后端（接受早提交、自称已初始化）证明检查真的会失败——否则这套检查只是装饰。

```
build\rhi_conformance_tests.exe     # 13 checks, 0 failed
```

## 4. 证据

- 报告新增 `rhi` 段：`{ backend: "d3d12", supported: ["d3d12"], framePathThroughInterface: true, nativeHandleUsedForTestHooks: true }`。
- 本机 smoke（RTX 5060）通过接口跑完 240 帧，`success=true`，确定性摘要 `c919164811d2d65a` 与迁移前逐位一致——说明迁移没有改变模拟与画面行为。

## 5. 已知限制

- **只有一个后端**：`supports("vulkan")` 为 false，X2 才实现第二个；契约检查目前只在 mock 上验证过，真实 D3D12 设备只验证了状态机前两项（smoke 隐式覆盖）。
- **`Device` 不是完整 RHI**：没有资源/描述符/管线对象、没有多队列、没有绑定模型。它只覆盖"帧路径 + 生命周期"，这是 R06 的最小目标，不是 X2 的全部工作。
- **adapter 仍是直通转发**：`D3D12Device` 每个方法都是一次转发，不引入策略。策略（预算、确定性）在宿主与引擎层，因此不会因为换后端而改变。
