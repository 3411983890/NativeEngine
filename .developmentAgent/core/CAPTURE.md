# GPU 捕获（PIX / RenderDoc）状态与操作说明

- 版本：0.5.0
- 检测脚本：`.incrementalTesting/rendering/capture.ps1`
- 机器状态：见 `reports/capture-status.json`

## 1. 本机结论（诚实记录）

| 工具 | 是否可用 | 说明 |
| --- | --- | --- |
| RenderDoc | **不可用** | `renderdoccmd.exe` 不在 PATH、标准安装路径或卸载注册表项中 |
| PIX for Windows | **不可用** | `WinPixGpuCapturer.dll` 未安装 |
| DXCap（VS Graphics Diagnostics） | 存在（`C:\Windows\system32\DXCap.exe`） | 用 `-capture -file` 探测本 D3D12 进程时未产生 `.vsglog`，不能作为捕获证据 |
| D3D12 调试层 | **不可用** | `D3D12GetDebugInterface` 在本机失败（Graphics Tools 未完整安装），因此 `--validation` 报告 `validationLayer=false` |

**没有 PIX 或 RenderDoc 捕获文件。** 报告字段写明 `renderDoc.available=false`、`pix.available=false`，
不伪造捕获路径；`.incrementalTesting/rendering/capture.ps1` 每次运行都会重写 `reports/capture-status.json`。

自动化替代证据（每次 `.incrementalTesting/rendering/rt-shadows.ps1` / `.incrementalTesting/core/g1-reliability.ps1` 都会产出）：

- 帧截图：`artifacts/shadow-on.png`、`shadow-off.png`、`shadow-mask.png`、`shadow-key.png`；
- 逐像素对比：`diff.changedPixels/changedPercent/maxChannelDelta`；
- CPU/GPU 时序：`timing` 与 `performance`（P50/P95、`gpuRtMs`）；
- 资源与字节数：`shadowTextureBytes`、`tlasInstances`、`videoMemoryBytes`。

这些足以重放“阴影是否存在、是否比光栅基线慢多少”，但不能替代 PIX/RenderDoc 的逐 drawcall 资源视图。

## 2. 装好工具后的捕获步骤

### RenderDoc

1. 安装 RenderDoc（<https://renderdoc.org>），确认 `qrenderdoc.exe` 可用。
2. `qrenderdoc` → **File → Launch Application**：
   - Executable：`<repo>\build\NativeGPU.exe`
   - Working Directory：`<repo>`
   - Command line：
     ```
     --probe --frames 240 --render-size 1280x720 --camera 9,5.5,10,-2,0.8,0 --rt-shadows on
     ```
3. 等窗口出现阴影帧后按 **F12** 捕获；在 **Pipeline State** 里应能看到：
   - `DispatchRays`（shape: `1280x720x1`，raygen `ShadowRayGen`）；
   - 光栅 pass 两个 `OMSetRenderTargets` 目标（`R16G16B16A16_FLOAT` ×2）；
   - 合成 pass 一个 `DrawInstanced(3,1,0,0)`；
   - `CopyResource` 到交换链后备缓冲。
4. 把 `.rdc` 存到 `reports/captures/`，并在提交说明里记路径。

命令行等价（本机装好后）：

```powershell
& 'C:\Program Files\RenderDoc\renderdoccmd.exe' capture --wait-for-exit `
  --capture-file reports\captures\nativegpu.rdc `
  build\NativeGPU.exe --probe --frames 60 --render-size 960x540
```

`.incrementalTesting/rendering/capture.ps1` 检测到 `renderdoccmd.exe` 时会自动尝试上面的命令并把结果写进状态 JSON。

### PIX

1. 安装 **PIX on Windows**（Microsoft Store / GitHub releases）。
2. PIX → **New Capture** → Launch `build\NativeGPU.exe`，参数同上。
3. 目标进程出现后按捕获按钮（或 `Win+P`）抓取一帧，保存 `.wpix` 到 `reports/captures/`。
4. 在 PIX 里检查：`DispatchRays` 的 SBT 大小 192 字节、TLAS 实例数 33、可见性贴图格式 `R16_FLOAT`。

### 命令行/自动化限制

- PIX 的自动捕获需要 `WinPixGpuCapturer.dll` 注入或 `pixcapture` 工具，本机没有安装，脚本不会伪造成功。
- D3D12 调试层需要「图形工具」可选功能；装上后 `--validation` 会启用 `ID3D12InfoQueue`，
  并把校验消息写进 `probes.renderTargets.validationMessages`。

## 3. 没有捕获工具时的最小复现包

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/rt-shadows.ps1   # 四张截图 + 对比 + 时序
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/g1-reliability.ps1 # 设备丢失/生命周期/错误码/性能
```

两者都会在 `reports/<version>/<commit>/<timestamp>/` 下留下完整证据，并在 `reports/latest/` 刷新别名。
