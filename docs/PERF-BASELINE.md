# 性能基线（G1）

- 版本：0.5.0
- 采集命令：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/g1-reliability.ps1`
- 证据：`reports/latest/g1-reliability.json` 的 `performance` 字段、`rt-shadows-evidence.json` 的 `timing`
- 机器：Windows 11 26200，AMD Radeon 890M（驱动 32.0.21036.1002），DXC 1.9.2609

## 1. 固定条件

| 条件 | 值 |
| --- | --- |
| 分辨率 | 1280 × 720 |
| 机位 | `--camera 9,5.5,10,-2,0.8,0`（固定，不跟随输入） |
| 场景 | `assets/courtyard.scene.json`（SHA-256 见 `run.json` 的 `sceneHash`） |
| 帧数 | 300（`--no-stress`，不做冒烟 resize / 重启，统计连续） |
| 采样 | 第 31 帧之后的所有帧（跳过首帧 PSO/缓存预热） |
| 方法 | CPU 提交时间用 `steady_clock` 包住命令录制直到 `Present` 之前；GPU 时间用队列时间戳查询，帧结束后读取 |
| 缓冲 | 3 帧在途，垂直同步 `Present(1,0)` |

## 2. 结果

| 指标 | P50 | P95 | 说明 |
| --- | --- | --- | --- |
| CPU 提交（不含 Present） | 0.456 ms | 1.080 ms | 真实 CPU 成本 |
| CPU 总时长（含 Present 等待） | 16.612 ms | — | 受 60 Hz 垂直同步支配，**不代表 CPU 成本** |
| GPU 整帧 | 0.472 ms | 0.717 ms | 整条命令列表（光栅 + DXR + 合成 + 复制） |
| GPU DXR 窗口 | 0.102 ms | 0.318 ms | 仅 `DispatchRays` |
| 帧间隔 | — | 16.7 ms | 垂直同步上限 |

几何与资源：

| 项目 | 值 |
| --- | --- |
| 绘制调用 | 33 |
| 三角形 | 396 |
| 材质槽 | 3（有界纹理表） |
| TLAS 实例 | 33（每帧按刚体变换重建） |
| 显存占用（驱动当前用量） | ≈ 96–101 MB |
| 可见性贴图 | 5,529,600 字节（3 × 1280 × 720 × R16） |

DXR 阴影开关对比（同一机位、同一帧号，`tools/rt-shadows.ps1`）：

| 指标 | 开启 | 关闭 | 差值 |
| --- | --- | --- | --- |
| GPU P95 | 0.717 ms | 0.467 ms | +0.250 ms（约 +53%） |
| 变化像素 | 3.30 % | — | 受光且被遮挡的像素 |

## 3. 怎么读这些数字

- **GPU 时间远小于 16.7 ms 的帧间隔**：这是 60 Hz 垂直同步下的空转，不是性能瓶颈。要看真实开销用 `gpuP95Ms`。
- **CPU 提交 P50 0.456 ms** 才是每帧的 CPU 成本；`cpuP50Ms≈16.6 ms` 包含 `Present` 的等待，不能拿去和 GPU 时间比较。
- **DXR 阴影成本 ≈ 0.25 ms/帧**（整帧 P95 差值），其中 `DispatchRays` 本身约 0.10 ms，其余是每帧 TLAS 重建与额外的合成 pass。
- 减少阴影成本的顺序：先降可见性贴图分辨率（当前全分辨率），再考虑降 TLAS 重建频率，最后才是降光线数（当前已是 1 条/像素）。

## 4. 与上一版本对比

| 版本 | GPU P95 | 变化 |
| --- | --- | --- |
| 0.3.1（无 DXR，单目标直绘） | — | 未用同一命令采集，不虚构对比 |
| 0.4.0（DXR 阴影 + 双目标 + 合成） | 0.717 ms | 本表基线 |
| 0.5.0（本次，含诊断与生命周期改造） | 0.717 ms | 渲染路径未变，数字一致 |

0.3.1 没有同口径基线，因此不写“提升/退化 X%”。需要跨版本对比时，用同一 `tools/g1-reliability.ps1` 在各自提交上重跑。

## 5. 复现

```powershell
# 完整 G1 可靠性 + 性能
powershell -NoProfile -ExecutionPolicy Bypass -File tools/g1-reliability.ps1

# 只要性能：固定机位、关闭冒烟 resize/重启
build\NativeGPU.exe --probe --no-stress --frames 300 --render-size 1280x720 `
  --camera 9,5.5,10,-2,0.8,0 --rt-shadows on --report reports/perf.json

# 阴影开/关对比
powershell -NoProfile -ExecutionPolicy Bypass -File tools/rt-shadows.ps1
```

数值随驱动与机器变化；换机器必须重跑并把 `run.json` 的 `adapter`、`driverVersion`、commit 一起归档。
