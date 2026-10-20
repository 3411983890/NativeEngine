# 回放与黑匣子（E08 / INN-02）

E07 让模拟可摘要，E08 让会话可重放：bug report 从"一段文字描述"变成"一个能逐帧复现的文件"。黑匣子在卡顿发生的当帧就把现场写成 sidecar，指向那段可回放的记录。

## 1. 记录格式（v2）

```
# nativegpu-replay v2 seed=20261020 step=1/120 mode=record
0,2,00547564ae417a3f,00000000,00000000,0,0,0,00000000,00000000
1,4,7ffb536f0751fd2d,00000000,00000000,0,0,0,00000000,00000000
```

| 列 | 含义 |
| --- | --- |
| `frame` | 渲染帧号 |
| `tick` | 累计固定步数（120 Hz） |
| `hash` | 该帧更新后的模拟摘要（FNV-1a 64，量化） |
| `forward` / `right` | 输入位的**精确 float 位模式**（8 位十六进制） |
| `run` / `jump` / `interact` | 布尔输入 0/1 |
| `yaw` / `pitch` | 镜头增量位的精确 float 位模式 |

**为什么存位模式而不是小数**：毫单位或十进制编码会让回放输入与原始输入差一个舍入步，模拟轨迹随之漂移，摘要就对不上。位模式往返是无损的。

## 2. CLI

| 参数 | 作用 |
| --- | --- |
| `--record <path>` | 记录会话（等价于 `--hash-log`，但语义是黑匣子记录） |
| `--hash-log <path>` | 只写摘要（`mode=hash`），确定性证据用 |
| `--replay <path>` | 用记录里的输入驱动模拟；自动进入 smoke/固定 1/60 步长，并采用记录头里的 seed |
| `--replay-strict` | 任何一帧摘要不符即失败（`NG-E-REPLAY-MISMATCH`） |
| `--stall-frame N` | 在第 N 帧注入真实墙钟停顿（故障注入，用于验证黑匣子） |
| `--stall-ms M` | 停顿毫秒数 |
| `--blackbox-stall-ms` | 触发阈值（默认 25 ms） |

报告新增两段：

```json
"replay":  { "enabled": true, "frames": 240, "compared": 240, "mismatches": 0, "firstMismatchFrame": 0, "strict": true },
"blackbox":{ "enabled": true, "triggered": true, "frame": 120, "dtMs": 63.2, "thresholdMs": 25, "anomalyWritten": true }
```

## 3. 黑匣子行为

- 记录从第一帧开始持续写入（逐行 flush），崩溃时也能留下已写内容。
- 第一次超过 `--blackbox-stall-ms` 的帧触发一次，写 `<record>.anomaly.json`：
  包含 `trigger`、`frame`、`tick`、`dtMs`、`thresholdMs`、`hash`、`recording`、`replayCommand`。
- 只写一次，不重复刷盘；记录本身仍是普通回放文件，可直接 `--replay --replay-strict`。

## 4. 证据

`tools/replay.ps1`（25 项断言，全部来自真实运行）：

```
powershell -File tools\replay.ps1 -Frames 240
```

1. 记录 240 帧 → 头部格式、行数、输入列数、帧号连续性；
2. 严格回放 → 比较 240 帧、0 不符、采用记录 seed；
3. 篡改第 100 帧摘要 → 严格回放退出非零、错误码 `NG-E-REPLAY-MISMATCH`、
   `firstMismatchFrame` 精确等于被篡改帧（证明回放能定位分歧，而不是笼统失败）；
4. 注入 60 ms 停顿 → 黑匣子触发、anomaly sidecar 帧号与阈值正确、指向记录路径；
5. 回放该记录 → 仍然 0 不符（证明故障现场是可用的证据，不是一次性的日志）。

产物：`reports/latest/replay-evidence.json`。

## 5. 已知限制

- **不是环形缓冲**：记录从头写到尾，长会话文件会线性增长；真正的环形缓冲（只保留最近 N 秒）未做。
- **只记录输入与摘要**：不记录 RNG 抽取、渲染状态或场景版本；场景变了以后回放会如实报不符（这是期望行为，但没有"记录场景哈希并自动加载"）。
- **回放不驱动真实输入栈**：录制交互会话时，回放会绕过键盘/鼠标，直接注入记录值。
- **黑匣子只认卡顿**：设备丢失、崩溃、断言失败还没有自动落盘（设备丢失已有 G1 注入路径，但未与 recorder 联动）。
