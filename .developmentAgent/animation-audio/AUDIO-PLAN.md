# 游戏音频系统：开发方案（对标 UE5 + 全景声对象音频）

- 版本：v0.2（A0–A3 已实现，A4–A9 待开发）
- 日期：2026-10-21
- 基线：NativeGPU 0.11.0（D3D12、DXR 1.2、120 Hz 固定步长物理、确定性场景事务层、Lua 5.4.9 玩法、MCP 工具面、可重放 CI）
- 关系：本文把 [DEV-TASKS.md](DEV-TASKS.md) E03 的“音频组件”从一句缺口展开成可并行开发的工作包；声学部分复用 [RT-SHADOWS.md](RT-SHADOWS.md) 的 TLAS 与场景碰撞体，不另建一套几何；预算与证据沿用 [BUDGET.md](BUDGET.md)、[DETERMINISM.md](DETERMINISM.md)、[IMAGE-REGRESSION.md](IMAGE-REGRESSION.md) 的收据格式。
- 目标：做出**能对标 UE5 音频栈主干能力**、并且**具备对象音频（bed + objects）与双耳渲染**的音频系统；不以“接了 WASAPI 能响”为完成标准。

---

## 实现状态（2026-10-21）

### 已落地：A0 + A1 + A2 + A3（首个可听切片）

| 工作包 | 内容 | 落点 |
| --- | --- | --- |
| A0 资产与设备 | `.ngsnd` 容器、WAV 导入 + 64 相位/24 抽头 Blackman-sinc 重采样、响度/峰值元数据、`bank.json`、WASAPI 共享模式事件驱动后端 | `engine/audio/audio_asset.cpp`、`engine/audio/audio_device.cpp`、`tools/audio.mjs` |
| A1 混音图 | 固定 48 kHz、256 帧块、无锁 SPSC 命令环（1024）、6 总线树、零分配实时路径、命令提前量 `commandLeadSamples` | `engine/audio/audio.cpp` |
| A2 空间化 | 距离衰减、VBAP 声像（7.1.2 环 + 高度层）、多普勒、程序化 HRTF（Woodworth ITD + 头影 ILD）、遮挡射线 + 双缓冲 + 平滑 | `engine/audio/audio.cpp` |
| A3 集成 | `PlaySoundW` 已删除；场景 `audio` 块三向同步（Schema / C++ loader / Node 校验器）；宿主 `--audio` / `--audio-quality` / `--audio-layout` / `--audio-bank` / `--audio-demo`；监听器跟随相机；碰撞体发布为遮挡体；`report.audio` 收据 | `host/main.cpp`、`host/scene_loader.cpp`、`assets/scene.schema.json`、`engine/scene/index.mjs` |

**可听结果**：`npm run audio` 里 `--audio wasapi --audio-demo` 会依次触发门/钥匙/完成/投掷/爆破五个事件，每个事件按场景发射器定位、按总线混音、按遮挡衰减。`--audio offline` 在没有声卡的机器上渲染同样的命令流并给出 `pcmHash`，两次运行必须一致。

**已删除的旧路径**：`host/main.cpp` 原先在 `audio.*` 事件分支里调用 `PlaySoundW` 异步播一个 WAV，无混音、无空间化、无总线、无遮挡、无时钟。该分支已被 `audioTrigger()` 取代，`grep -r PlaySoundW host/ engine/ tests/` 为空。

### 证据

- 原生套件：`build/native_audio_tests.exe`，**40 项检查**（null 路径透明、BS.1770 响度、VBAP 恒功率、多普勒、HRTF ITD/ILD、遮挡衰减与平滑、虚拟 voice、循环接缝、块大小无关的 PCM 哈希、对象槽位、下混、64 voice 预算、10 秒稳定性），逐项写 `reports/audio/*.json`。
- 宿主证据：`npm run audio` 共 **59 项检查**，分四层——混音器自身、`--audio offline` 的宿主路由（含两次运行哈希一致）、真实 WASAPI 端点（采样率/通道/周期/欠载/命令延迟/CPU）、以及"音频不改画面"的逐字节截图对比。归档到 `reports/<version>/<commit12>/<stamp>/`。

一次典型通过的实测值（0.11.0，本机 Realtek 端点）：

| 指标 | 值 | 说明 |
| --- | --- | --- |
| 设备格式 | 48000 Hz / 2 ch / float / 1056 帧周期 | 共享模式，事件驱动 |
| 欠载 | 0 | 286 656 帧渲染期间 |
| 迟到命令 | 0 | 命令提前量 = 设备周期 + 2 块 |
| 混音 CPU | 0.07 ms / 块 | 预算 5.33 ms |
| 试听响度 | -19.8 LUFS（短时峰值） | 目标 -16，`fogtown/blast` 循环试听 |
| 试听峰值 | 0.246（-12 dBFS） | 无削波 |
| `pcmHash` | 两次离线运行一致 | 288 000 帧，7.1.2 |
| 画面 | `--audio off` 与 `--audio wasapi` 截图 SHA-256 相同 | 音频不进帧 |

### 尚未实现：A4–A9

对象音频的 `Atmos` 提升路径（`Quality::Atmos` 目前只影响床布局与对象槽位分配，未接 `ISpatialAudioClient` 元数据流）、ADM BWF 导出、SOFA HRTF 导入、`.ngpatch`/`.nggraph` 图资产、编辑器音频面板与电平表、`audio_patch` MCP 工具、预算降档阶梯的音频档位。这些在 §5 仍按原计划列出。

### 历史记录（迁移前）

迁移前本仓库唯一的音频代码是 `host/main.cpp:1230` 的一行：

```cpp
if(!app.smoke && !app.replayEnabled && name.rfind("audio.",0)==0){
    const auto file = std::filesystem::u8path(app.document.scene.assetRoot)/"fogtown/audio"/(name.substr(6)+".wav");
    PlaySoundW(file.c_str(), nullptr, SND_FILENAME|SND_ASYNC|SND_NODEFAULT);
}
```

它把玩法事件名 `audio.blast` 直接映射到 `assets/fogtown/audio/blast.wav`，用系统 `PlaySoundW` 异步播放：无混音、无空间化、无总线、无遮挡、无时钟、无回放、无法与渲染/物理共享帧预算。这段记录保留下来，是因为 §12 的迁移路线图以它为起点。

现有可复用的资产与设施：

| 已有 | 位置 | 音频侧价值 |
| --- | --- | --- |
| 5 条 WAV 提示音（blast/complete/door/fail/power） | `assets/fogtown/audio/` | 首个迁移用例与冒烟素材 |
| 场景 JSON + Schema + Node 校验器 + 事务层 | `assets/scene.schema.json`、`engine/scene/*.mjs` | 音频发射器/混响区可直接进同一套事务与回滚 |
| 120 Hz 固定步长与确定性摘要 | `engine/native/game.cpp`、`.incrementalTesting/core/determinism.ps1` | 音频命令按采样时钟调度，天然帧率无关 |
| DXR TLAS + 场景 AABB 碰撞体 | `renderer/rt_shadows.cpp`、`engine/native/physics.cpp` | 遮挡/声学射线不需要新几何 |
| Lua 5.4.9 玩法运行时 | `assets/scripts/fogtown/*.lua` | `audio.play/setParameter/snapshot` 直接挂到现有事件系统 |
| MCP 8 工具 + `material_patch` 模式 | `ai/tool-service.mjs` | `audio_patch` 复用“预览 → 差异 → 提交”闭环 |
| 帧预算阶梯 + 图像/JSON 证据 | `.incrementalTesting/rendering/budget.ps1`、`tools/report-lib.ps1` | 音频降档与音频证据沿用同一格式 |
| `png_writer` | `renderer/png_writer.cpp` | 输出频谱图/声场俯视图，作为可看的证据 |

---

## 0. 结论先行

1. **先做设备与混音图，再做空间化**。没有稳定的 48 kHz 时钟、无锁命令环和总线树，后面所有“空间感”都是不可验证的玄学。A0/A1 是所有线的前置。
2. **音频线程与渲染/物理线程解耦，但时钟必须共享**：音频以采样为单位计时（`sampleTime`），玩法命令带目标采样点；主线程帧率从 8 ms 抖到 33 ms，事件落点不允许漂移。这是“对标 UE”里最容易被忽略、也最影响手感的一条。
3. **空间化分两条渲染路径，共用一套声源模型**：扬声器路径（bed 7.1.2 + VBAP + 对象）与双耳路径（HRTF 卷积 + 虚拟扬声器床）。声源、衰减、遮挡、多普勒只算一次，末端才分叉。
4. **对象音频不是“多声道”**：它是 `bed（固定声道）+ objects（带 3D 位置/尺寸/优先级的单声道元素）`。我们把对象上限定在 **32**（Dolby 规范允许更多，但预算按 32 设计），超出的按优先级“snap 到 bed”。
5. **Atmos 兼容输出与自研参考渲染器并行**：Windows 上走 `ISpatialAudioClient` 的元数据流可把对象交给系统端（Dolby Access / 家庭影院），但**授权与终端不可控**；因此必须同时有自研对象渲染器 + ADM BWF 导出，保证没有 Dolby 授权也能验收“对象位置、增益、下混、响度”。
6. **UE 的能力按“主干必做 / 可选 / 明确不做”三分**：MetaSound 类图、Submix、Attenuation、Occlusion、Concurrency、Virtual Voice、Quartz 式采样精确调度是主干；AudioLink、Audio Insights 的完整版、Wwise/FMOD 集成明确不做。
7. **确定性只承诺到离线渲染**：同一命令流 + 同一种子 → PCM 哈希一致；设备输出受 WASAPI 抖动影响，不进哈希。这条边界写死，避免“音频不确定所以整个引擎不确定”的误读。
8. **首个可听切片 = A0+A1+A2+A3（4–6 人周）**：48 kHz 混音图、总线、距离衰减、5.1/7.1 声像、遮挡射线、`audio.play` 迁移掉 `PlaySoundW`。完整 A0–A9 约 **22–26 人周**。
9. **最值得做的差异点**：逐 voice 音频收据、场景 BVH 驱动的声学（复用 TLAS）、帧率无关的采样精确事件、AI 可编辑的类型化音频补丁、无授权也能验收的对象音频参考渲染器。

---

## 1. 范围与定义

### 1.1 术语

| 概念 | 定义 | 本文负责 |
| --- | --- | --- |
| 混音图（mix graph） | 声源 → 总线 → 主输出的有向图，含增益/滤波/动态/卷积节点 | **是** |
| 总线（bus） | UE 的 Sound Class + Submix 的合并物；有独立 DSP 链与电平表 | **是** |
| 声源（emitter） | 场景里发声的实体/组件，持有资产、衰减、总线、优先级 | **是** |
| 衰减（attenuation） | 距离曲线、空气吸收、锥形、形状（球/盒/胶囊） | **是** |
| 空间化（spatialization） | 把单声道/多声道声源定位到扬声器或双耳 | **是** |
| 对象音频（object audio） | bed 声道 + 带元数据的独立对象元素 | **是** |
| 全景声（immersive/3D audio） | 含高度层的还音；本文用“全景声”指代对象+高度层的听感，不指某一商标 | **是（听感层）** |
| Dolby Atmos | Dolby 的对象音频格式/渲染器/认证体系 | **兼容路径**，不声称认证 |
| 声学（acoustics） | 遮挡、绕射、门户、早期反射、混响 | **是（分档）** |
| MetaSound 类图 | 采样级可编程 DSP 图，参数可调制 | **是（P1，A7）** |
| 音乐/节奏同步 | Quartz 式采样精确的节拍调度 | **部分**（A1 的采样时钟即可支撑，独立编辑器不做） |

### 1.2 明确不做（本轮/本文）

- Dolby Atmos **认证**与杜比实验室的正式授权谈判；本文只做技术兼容与自研参考渲染。
- Wwise / FMOD 集成、第三方中间件适配层。
- 完整 MetaSound 图形编辑器（Web 端只做参数面板与预设，图编辑留 P2）。
- 语音识别/生成、TTS、音乐生成模型。
- VR 头显的 6DoF 头动追踪（接口预留 `headPose`，不实现设备接入）。
- 音频驱动的玩法（节拍判定）先只提供采样精确事件回调，不做判定规则。

### 1.3 质量档位

| 档 | 内容 | 目标设备 | 备注 |
| --- | --- | --- | --- |
| `audio-off` | 不初始化设备，命令直接丢弃 | 全部 | CI/无音频设备机器默认 |
| `audio-stereo` | 2.0、距离衰减、简单左右声像 | 全部 | 最低保障，笔记本扬声器 |
| `audio-spatial` | 5.1/7.1 bed、VBAP、锥形、遮挡射线 | 桌面 | 首个“能听出方向”的档 |
| `audio-binaural` | HRTF 双耳 + 虚拟 7.1.2 床 + 头部相关滤波 | 耳机 | 无环绕设备时的主路径 |
| `audio-atmos` | 7.1.2 bed + 32 objects + 元数据输出/直通 | 家庭影院/Atmos 终端 | 失败自动回落 `audio-binaural` |

档位由 `--audio-quality` 或设备能力探测决定，**每次降档写原因**（沿用 `BUDGET.md` 的阶梯收据）。

---

## 2. 对标 UE5：能力矩阵

UE5 的音频栈可以拆成“资产/图/空间/混音/调度/工具”六层。下表逐项给出等价物、优先级与差异说明。P0 = 首个可听切片必须；P1 = 完整版必须；P2 = 可延后。

| UE5 能力 | UE 中的形态 | 本引擎等价物 | 优先级 | 差异/取舍 |
| --- | --- | --- | --- | --- |
| Sound Wave | `USoundWave`，压缩/流送/循环 | `.ngsnd`（PCM16/24/f32 + 循环点 + 响度元数据） | P0 | 不做 Vorbis/Opus 解码；压缩留 P2 |
| Sound Cue | 节点式随机/调制/衰减组合 | **类型化 JSON `SoundPatch`**（随机容器/序列/切换/参数） | P0 | 不做图形节点连线，用声明式补丁 + 预览 |
| MetaSounds | 采样级 DSP 图、Pages、参数 | `AudioGraph`（A7）：节点 DAG、块内 32 样本粒度参数 | P1 | 不做可视化图编辑器；AI 直接生成图 JSON |
| Attenuation | 距离/形状/锥形/空气吸收/聚焦 | `AttenuationAsset`（球/盒/胶囊/锥） | P0 | 增加“材质相关空气吸收” |
| Spatialization | Panning/HRTF/Binaural，非空间化开关 | `Spatializer`：VBAP / HRTF / 非空间化 | P0 | 双耳自研，不依赖第三方 |
| Occlusion | 多通道 trace、低通、衰减 | 射线 + 材质吸收 + 平滑 | P0 | 复用 TLAS，CPU BVH 回退 |
| Reverb / Audio Volume | 混响体积、门户、IR | `ReverbZone` + `Portal` + 混合混响（卷积 ER + FDN 尾） | P1 | 不做完整 portal 图求解，先做单跳 |
| Submix / Sound Class | 总线树 + 效果链 + 快照 | `Bus` 树 + `MixSnapshot` | P0 | 合并为一个概念，减少层级 |
| Audio Modulation | 参数总线、LFO/包络 | `ModulationBus`（A7） | P1 | 与玩法参数共用命名空间 |
| Concurrency | 分组并发上限、抢占规则 | `ConcurrencyGroup`（按总线/资产组限流） | P0 | 规则用 JSON 声明，可回放 |
| Virtual Voice | 超出上限的声源虚拟化，按优先级复活 | `VoiceManager`：物理 64 / 虚拟 1024 | P0 | 提升/降级带 20 ms 交叉淡化 |
| Quartz | 采样精确时钟、节拍回调 | `AudioClock` + `scheduleAt(sample)` | P0 | 不做节拍网格编辑器 |
| Audio Bus（Atmos） | 送主混音或送对象 | `OutputBus`：`bed` / `object` / `binaural` | P1 | 与 Dolby 对象语义对齐 |
| Source Bus | 外部音频输入（如视频） | `ExternalSource` 接口 | P2 | 只留接口 |
| Audio Insights | 实时 profiler、voice 表 | `audio` 报告段 + 编辑器电平/voice 表 | P1 | 走现有 JSON 报告，不做独立工具 |
| AudioLink | 跨进程/跨设备传输 | 不做 | — | 明确不做 |
| Soundscape / 程序化环境 | 基于空间查询的环境声 | `AmbienceDirector`（A7 可选） | P2 | 用场景查询驱动，不是独立插件 |

**结论**：P0 覆盖 UE 音频栈“能做出一个商业游戏的基本盘”；P1 补齐 MetaSound 类图与混响；P2 及“不做”项在文档里写清，不用“已对标”含糊带过。

---

## 3. 架构

### 3.1 分层

```text
┌──────────────────────────── 游戏层（120 Hz 固定步长） ────────────────────────────┐
│ gameplay/Lua: audio.play / stop / setParameter / snapshot / scheduleAt            │
│ 物理/材质事件: 撞击冲量 → SurfaceType → 撞击音；脚步 → 地面材质                    │
│ Listener: 相机变换 + 速度（每 tick 一次，插值到音频块）                            │
└───────────────────────────────────┬──────────────────────────────────────────────┘
                                    │ 无锁 SPSC 命令环（带 sampleTime）
┌───────────────────────────────────▼──────────────────────────────────────────────┐
│                          音频线程（WASAPI 事件驱动）                              │
│  命令应用 → VoiceManager（物理/虚拟）→ 解码/流送 → DSP 图 → 总线树 → 末端渲染       │
│                                   │                                              │
│      ┌────────────────────────────┼─────────────────────────────┐                │
│      ▼                            ▼                             ▼                │
│  扬声器 bed 渲染           对象渲染 / 元数据流              双耳渲染               │
│  (VBAP 5.1/7.1.2)        (bed 7.1.2 + objects)        (HRTF + 虚拟床)            │
└───────────────────────────────────┬──────────────────────────────────────────────┘
                                    ▼
        WASAPI shared / exclusive / ISpatialAudioClient / null(offline)
```

### 3.2 线程与时钟

| 线程 | 职责 | 硬规则 |
| --- | --- | --- |
| 主/游戏线程 | 发命令、更新 listener、读电平 | **不阻塞**、不做文件 IO、不调设备 |
| 音频线程（实时） | 混音、DSP、空间化、写设备缓冲 | **不分配、不加锁、不打日志、不读文件**；超时算 underrun |
| IO/流送线程 | 解码、预取、银行加载 | 通过环形缓冲交付，绝不回调音频线程 |
| 声学线程（工作线程池） | 遮挡/反射射线、IR 烘焙 | 结果带时间戳，音频线程只做平滑 |

时钟约定：

- 采样率固定 **48 kHz**（设备不支持则重采样到 48 kHz，内部永远 48 kHz）。
- 块大小：`low=256`（5.33 ms）、`balanced=480`（10 ms）、`safe=1024`（21.3 ms）。
- `AudioClock::sampleTime` 单调递增，`uint64`，约 12 万年不回绕。
- 所有命令带 `targetSample`；`targetSample <= now` 时立即生效并记 `lateCommands`。
- **禁止用帧号做音频调度**。帧率变化时事件落点必须逐样本一致——这是验收项，不是建议。

### 3.3 混音图与总线

```text
Voice(物理) ──► [Source DSP] ──► [Attenuation] ──► [Spatializer] ──┐
                                                                   ├─► SFX   ─┐
Voice(物理) ──► ... ────────────────────────────────────────────────┤          │
                                                                   ├─► Ambience├─► Master ─► Output
Voice(物理) ──► ... ────────────────────────────────────────────────┤          │
                                                                   ├─► Music ─┤
                                                                   └─► Voice ─┘
```

默认总线（与 UE Sound Class 对齐，但扁平化）：

| 总线 | 默认 DSP | 典型用途 |
| --- | --- | --- |
| `Master` | 增益、响度计、真峰值限制器 | 最终输出 |
| `SFX` | 增益、可选压缩 | 世界音效 |
| `Ambience` | 增益、低频管理 | 环境/风声/虫鸣 |
| `Music` | 增益、ducking 侧链 | 音乐 |
| `Voice` | 增益、去齿音（P2） | 对白 |
| `UI` | 增益、非空间化强制 | 界面音 |
| `ReverbSend` | 卷积 ER + FDN 尾 | 所有总线的辅助送出 |

节点类型（A1 只做前 8 个，A7 补后 4 个）：

`Gain`、`Biquad`（LP/HP/BP/Peak）、`Delay`、`Compressor`、`Limiter`、`Panner`（VBAP）、`Spatializer`、`Meter`、`Convolver`、`FDN Reverb`、`Splitter/Merger`、`WaveShaper`、`Envelope`、`LFO`。

规则：

- 图在**编译期**定长：节点数、每节点输入数、每块最大通道数在加载时确定，实时路径不 `new`。
- 参数变更走无锁参数槽（`std::atomic<float>` + 每块读取），不做消息队列抖动。
- 每个总线有 `peak/rms/lufsShort` 三个表，供编辑器与报告读取。

### 3.4 输出后端

| 后端 | 用途 | 关键点 |
| --- | --- | --- |
| `null` | CI、无声卡、`--smoke` | 仍完整跑混音图，输出丢弃；用于确定性与 CPU 预算 |
| `offline` | 证据、回归、ADM 导出 | 固定块、固定种子，渲染到内存/WAV；**唯一承诺确定性的后端** |
| `wasapi-shared` | 默认 | 事件驱动、10 ms 周期；设备格式协商 + 重采样 |
| `wasapi-exclusive` | 低延迟选项 | 独占格式探测，失败自动回 shared 并记录原因 |
| `spatial-objects` | Atmos 兼容 | `ISpatialAudioClient` 元数据流；静态 bed 通道 + 动态对象；终端不支持则回落 |
| `binaural` | 耳机 | 自研 HRTF 渲染到 2.0，走 shared 输出 |

设备丢失（拔耳机、切默认设备、驱动重启）必须：暂停时钟 → 重建 → 恢复时 `sampleTime` 连续（补静音），**不允许**重置 voice 状态导致音乐从头播。

### 3.5 声源模型

```cpp
// audio/contracts.hpp（设计稿）
struct EmitterDesc {
    uint32_t id;
    uint32_t entityId;
    uint32_t soundId;        // .ngsnd / SoundPatch
    uint32_t attenuationId;
    uint32_t bus;            // enum
    float    gainDb;
    float    pitchScale;
    uint8_t  loop;
    uint8_t  spatialized;    // 0 = 2D（UI/音乐）
    uint8_t  occlusion;      // 0/1
    uint8_t  priority;       // 0..255，越大越不易被抢占
    uint32_t concurrencyGroup;
    uint32_t maxInstances;
};

struct AttenuationDesc {
    uint8_t  shape;          // 0 sphere, 1 box, 2 capsule, 3 cone
    uint8_t  distanceModel;  // 0 linear, 1 logarithmic, 2 inverse, 3 natural
    float    innerRadius;
    float    falloffDistance;
    float    minGain, maxGain;
    float    airAbsorption[3];   // 500/2k/8k Hz 的 dB/100m
    float    coneInnerDeg, coneOuterDeg, coneOuterGainDb;
    float    focus;          // 0..1，非空间化聚焦
    uint8_t  reverbSendCurve;    // 0 none, 1 linear, 2 inverse
};
```

### 3.6 空间化

**共用前置**：世界坐标 → 听者局部坐标（右手 Y-up，与渲染一致）→ 距离衰减 → 空气吸收（按距离插值三段 LPF）→ 锥形增益 → 多普勒。

**扬声器路径（bed）**：

- 布局：`5.1`（L R C LFE Ls Rs）、`7.1`（+ Lrs Rrs）、`7.1.2`（+ Tfl Tfr，Atmos 家庭版 bed 上限 10 声道）。
- 声像：VBAP（矢量基幅度声像）。三角化后的扬声器对/三元组求增益，能量守恒；水平面用对，含高度用三元组。
- LFE：仅对 `bLFE` 标记的声源送 LFE，默认 **不** 从普通声源低切生成（避免低频堆积）。
- 对象：当 `audio-atmos` 且对象槽未满，声源升格为对象（单声道 + 元数据），否则并入 bed 的 VBAP。

**双耳路径**：

- 虚拟床：7.1.2 的 10 个虚拟扬声器各自过 HRTF，再求和（等价于“把房间折叠到耳机”）。
- 对象：直接 HRTF 卷积（不经过虚拟扬声器），保留高度线索。
- 头部：`headPose`（yaw/pitch/roll）可选；无追踪时用 listener 朝向，加 ±3° 的缓慢漂移（去“颅内定位”）可选开关。
- HRTF：SOFA（AES69）导入，默认 256 tap / 48 kHz；距离 > 1.5 m 时按“远场 HRTF + 距离衰减”处理，避免近场数据集缺失导致的染色。

**多普勒**：

```text
f' = f * c / (c + v_source·r̂ - v_listener·r̂)   // c = 343 m/s，r̂ 为听者→声源单位向量
```

- 只取径向速度；`|f'/f - 1|` 上限 1.5 倍，防止瞬移爆音。
- 每块更新一次，参数平滑 30 ms。

### 3.7 声学

分三档，逐档可回退：

| 档 | 内容 | 成本 | 依赖 |
| --- | --- | --- | --- |
| `ac-none` | 无遮挡，只有距离 | 0 | — |
| `ac-ray` | 单射线遮挡：命中 → 增益衰减 + LPF + 混响送出增加；按材质吸收系数 | 每 voice 每 50 ms 一条 | 场景 BVH / TLAS |
| `ac-portal` | 门户：射线先打门/窗，命中则按“开启度”衰减而非全阻；支持单跳 | 2–3 条射线 | `ac-ray` + portal 实体 |
| `ac-full` | 早期反射（镜像源 1 阶）+ 混响区 + 烘焙 IR | 每 200 ms 一批 | `ac-portal` + `ReverbZone` |

规则：

- **遮挡不是开关**：命中后目标值为 `gain -X dB / cutoff Y Hz`，用 80 ms 一阶平滑过渡，禁止逐块跳变。
- 遮挡射线**错峰**：每个 voice 有固定相位偏移（`voiceId % 8`），避免同一块内 64 条射线同时打。
- 材质吸收：复用材质表的 `SurfaceType`（brick/metal/checker/…），给每类一个 `{gainDb, cutoffHz, decay}` 三元组，写进 `audio.surfaces`。
- 混响：短 IR（≤0.5 s）用分区卷积做早期反射；尾音用 8 线 FDN（反馈延迟网络），参数（RT60、预延迟、湿度）可实时改，避免每次换 IR 都重新加载。

### 3.8 对象音频与 Atmos 兼容路径

**元素模型**（与 Dolby 语义对齐）：

```cpp
struct ObjectMeta {
    float   position[3];   // 归一化房间坐标，[-1,1]
    float   extent[3];     // 声源尺寸（0 = 点源）
    float   gainDb;
    float   spread;        // 0..1，扩散度（去相关）
    uint16_t priority;     // 差分优先级
    uint8_t  snap;         // 0 none, 1 snap-to-bed
    uint8_t  binauralMode; // 0 off, 1 near, 2 far
    uint32_t trim;         // 对象微调
};
```

**bed 与对象的分配规则**：

1. 固定声道（C/LFE/环绕底）永远走 bed。
2. 有明确 3D 位置、且不在扬声器方向 ±10° 内的声源优先做对象。
3. 对象槽满（32）→ 按 `priority × 距离权重` 淘汰，被淘汰者 snap 到最近 bed 声道（增益按 VBAP 重算，避免响度跳变）。
4. 静态声源（如环境火堆）可标记 `snap=1`，直接常驻 bed，省对象槽。
5. 每个对象的元数据每块更新一次；位置量化到 1/4096，保证元数据可复现。

**输出路径**：

| 路径 | 条件 | 行为 |
| --- | --- | --- |
| 自研渲染器 | 默认，任何设备 | bed VBAP + 对象按扬声器布局渲染；耳机则走双耳 |
| 系统空间音频 | 终端声明支持且用户开启 | `ISpatialAudioClient` 元数据流；bed 静态通道 + 动态对象 |
| Dolby 直通 | 终端为 Dolby Atmos for Home Theater 且已授权 | 交给系统渲染器；我们只保证元数据正确 |
| ADM 导出 | 离线/交付 | 按 ITU-R BS.2076 写 BWF（`axml` + `chna`），用于与参考渲染器比对 |

**下混**（必须有，且要有门限）：

```text
7.1.2 → 5.1：Lrs/Rrs 以 -3 dB 折叠进 Ls/Rs；Tfl/Tfr 以 -6 dB 混入 L/R（或按配置丢弃）
5.1   → 2.0：C 以 -3 dB 进 L/R；Ls/Rs 以 -3 dB 进 L/R 并加 10 ms 去相关延迟
```

- 下混后必须过真峰值限制器（≤ -1 dBTP），否则家庭影院切立体声会削波。
- 下混矩阵写进配置，**不允许**在代码里散落魔数。

**响度**：

- 测量：ITU-R BS.1770-4（K 加权、门限 -70 LUFS 绝对 / -10 LU 相对），真峰值按 4× 过采样。
- 目标：交互默认 **-16 LUFS**（可配），过场/剪辑 **-23 LUFS**；真峰值 ≤ **-1 dBTP**。
- 每条 `.ngsnd` 烘焙时写入自身 `loudnessLufs` 与 `peakDbtp`，混音器可做“响度对齐”（避免某条音效比别的响 12 dB）。

---

## 4. 数据契约

### 4.1 C++ 接口（`audio/contracts.hpp`，先冻结再并行）

```cpp
namespace nativegpu::audio {

struct AudioConfig {
    uint32_t sampleRate;      // 固定 48000
    uint32_t blockFrames;     // 256 / 480 / 1024
    uint8_t  quality;         // 0 off,1 stereo,2 spatial,3 binaural,4 atmos
    uint8_t  backend;         // 0 null,1 offline,2 shared,3 exclusive,4 spatial,5 binaural
    uint8_t  layout;          // 0 stereo,1 5.1,2 7.1,3 7.1.2
    uint8_t  maxObjects;      // 默认 32
    uint16_t maxPhysicalVoices;// 默认 64
    uint32_t maxVirtualVoices; // 默认 1024
};

struct AudioCommand {          // 16/32 字节定长，SPSC 环
    uint64_t targetSample;
    uint32_t type;             // play/stop/param/snapshot/listener
    uint32_t id;
    float    f[4];
};

struct VoiceReceipt {          // 每帧可选输出（诊断/预算）
    uint32_t voiceId, soundId, bus;
    uint8_t  physical, virtualized, occluded, isObject;
    uint16_t priority;
    float    distance, gainDb, cutoffHz;
    uint32_t hrtfTaps, cpuMicros;
};

class Device;      // 设备生命周期、时钟、underrun 计数
class MixGraph;    // 图编译与块渲染
class VoiceManager;// 物理/虚拟、抢占、交叉淡化
class Spatializer; // VBAP / HRTF
class Acoustics;   // 射线、门户、混响
class ObjectRenderer; // bed+objects、元数据、下混
}
```

冻结标志：字段名、单位（米/dB/Hz/采样）、坐标系（右手 Y-up，与 `engine/native/vec3.hpp` 一致）、枚举值、块大小假设，全部有 `static_assert`。

### 4.2 场景 JSON（`audio` 顶层块）

```json
"audio": {
  "schema": 1,
  "masterGainDb": -3.0,
  "loudnessTargetLufs": -16.0,
  "output": { "mode": "auto", "layout": "7.1.2", "maxObjects": 32, "binaural": "hrtf" },
  "listener": { "entity": "a1e00000-0000-4000-8000-000000000020", "headTracking": false },
  "surfaces": {
    "brick":  { "gainDb": -9.0, "cutoffHz": 1200, "rt60": 1.1 },
    "metal":  { "gainDb": -4.0, "cutoffHz": 3500, "rt60": 1.8 },
    "checker":{ "gainDb": -11.0,"cutoffHz": 900,  "rt60": 0.9 }
  },
  "attenuations": {
    "default": { "shape": "sphere", "distanceModel": "inverse",
                 "innerRadius": 1.5, "falloffDistance": 28.0,
                 "airAbsorption": [0.6, 2.2, 8.0], "priority": 128 }
  },
  "emitters": [
    { "id": "e-door", "entity": "…", "sound": "fogtown/door",
      "attenuation": "default", "bus": "SFX", "loop": false,
      "occlusion": true, "autoPlay": false, "gainDb": 0.0 }
  ],
  "reverbZones": [
    { "shape": "box", "min": [-9, 0, -9], "max": [9, 4, 9],
      "ir": "ir/courtyard.ngir", "wet": 0.35, "portal": false }
  ],
  "snapshots": {
    "underwater": { "lowpassHz": 800, "wet": 0.8, "pitch": 0.96, "fadeMs": 250 }
  }
}
```

落地要求（沿用仓库既有约定）：

1. **三处同步**：`assets/scene.schema.json`、C++ loader（`host/scene_loader.cpp`）、Node 校验器（`engine/scene/lint.mjs`）。少一处就是“能加载但不合法”，会重演材质 wetness/wear 的迁移问题。
2. `additionalProperties: false` 是现状，因此 `audio` 必须显式加进 `properties` 与 `$defs`；实体级音频走 `components` 或新增 `audio` 字段，二选一并写进 schema。
3. `schemaVersion` 是否从 1 升到 2：**建议不升**，`audio` 为可选块，缺省即 `audio-off`；这样 0.11.0 的场景与截图基线不受影响（对齐 `vol-off` 逐像素不变的原则）。
4. 事务层：音频块的修改必须能 `preview → diff → apply → undo`，并触发运行时热重载；音频热重载**不允许**打断正在播放的音乐（保留 voice，只换参数）。

### 4.3 资产格式

| 扩展名 | 内容 | 说明 |
| --- | --- | --- |
| `.ngsnd` | 头 + PCM 数据（16/24/f32）+ 循环点 + 响度元数据 + 声道布局 | 48 kHz 内部统一；导入时重采样 |
| `.ngbnk` | 声音银行：ID 表、偏移、依赖、SHA-256 | 内容寻址，进运行包 manifest |
| `.ngir` | 脉冲响应：采样率、长度、声道布局、归一化增益 | 用于早期反射卷积 |
| `.ngpatch` | 类型化音频补丁（随机/序列/切换/参数） | 对应 Sound Cue，JSON 可读 |
| `.nggraph` | 音频图（A7） | 节点 DAG + 参数表，JSON |

`.ngsnd` 头（小端）：

```text
magic "NGS1" | version u16 | channels u16 | sampleRate u32 | frames u32
format u16 (0=i16,1=i24,2=f32) | flags u16 (loop/stream/loudness)
loopStart u32 | loopEnd u32 | loudnessLufs f32 | peakDbtp f32
dataOffset u32 | dataBytes u32
```

### 4.4 Lua API

```lua
-- 播放（返回句柄，可用于停止/改参）
local h = audio.play("fogtown/door", {
  at = self.entity,          -- 省略则 2D
  bus = "SFX",
  gainDb = 0,
  attenuation = "default",
  loop = false,
})

audio.stop(h, 0.15)                    -- 150 ms 淡出
audio.setParameter("wind.intensity", 0.6)
audio.setEmitterParameter(h, "speed", 0.8)
audio.snapshot("underwater", 0.25)      -- 250 ms 过渡
audio.scheduleAt(sampleTime + 24000, function() audio.play("ui/click") end)
audio.setListener(self.entity)          -- 通常由相机自动设置
```

约束：

- 所有调用只写命令环，**立即返回**；不返回播放结果（需要结果用事件回调）。
- `audio.play` 的资产缺失 → 记 `NG-A-MISSING-SOUND` 到事务错误流，不崩溃、不静默（沿用 [ERROR-CONTRACTS.md](ERROR-CONTRACTS.md) 的错误码风格）。
- 与现有 `audio.blast` 事件名兼容：迁移期 `audio.blast` 映射到 `audio.play("fogtown/blast")`，跑通后删除 `PlaySoundW` 分支。

### 4.5 MCP 工具

新增 `audio_patch`，与 `material_patch` 同构：

```json
{
  "tool": "audio_patch",
  "scene": ".screen/courtyard.scene.json",
  "preview": true,
  "ops": [
    { "op": "setEmitter", "id": "e-door", "gainDb": -3 },
    { "op": "setAttenuation", "id": "default", "falloffDistance": 35 },
    { "op": "addReverbZone", "shape": "box", "min": [-9,0,-9], "max": [9,4,9], "ir": "ir/courtyard.ngir" }
  ]
}
```

返回：差异、受影响 voice 数、预估 CPU 变化、是否需要重烘焙 IR；`preview=false` 才写盘并提交 revision。

---

## 5. 并行工作包

### 5.1 总表

| 线 | 范围 | 依赖 | 交付 | 验收 | 粗估 |
| --- | --- | --- | --- | --- | --- |
| **A0 设备与时钟** | WASAPI 共享/独占、设备丢失、null/offline 后端、无锁命令环、`AudioClock` | 无 | `Device` + `AudioClock` + 三种后端 | 10 分钟 0 underrun；帧率抖动下事件落点 0 样本漂移 | 1.5 周 |
| **A1 混音图与 voice** | 图编译、总线树、物理/虚拟 voice、抢占与交叉淡化、并发组 | A0 | `MixGraph` + `VoiceManager` | 1024 声源 ≤64 物理；升降级无爆音 | 2 周 |
| **A2 资产与流送** | `.ngsnd`/`.ngbnk` 读写、重采样、循环、流送、响度元数据 | A0 | 导入器 + 银行 + 流送 | 循环点无咔哒；重采样 THD 达标 | 1.5 周 |
| **A3 空间化与衰减** | listener、距离/锥形/空气吸收、VBAP 5.1/7.1、多普勒、非空间化 | A1+A2 | `Spatializer`（bed 路径） | VBAP 增益误差 ≤0.3 dB；多普勒 ≤1 cent | 2 周 |
| **A4 双耳 HRTF** | SOFA 导入、分区卷积、虚拟 7.1.2 床、对象 HRTF、头动接口 | A3 | `BinauralRenderer` | ITD ≤30 µs；ILD 单调；切换无爆音 | 2.5 周 |
| **A5 对象音频 / Atmos 兼容** | bed+objects、元数据、snap、下混、响度、ADM 导出、系统空间音频后端 | A3（+A4 可选） | `ObjectRenderer` + ADM writer | 32 对象位置 ≤0.1°；下混能量 ≤1 dB | 3 周 |
| **A6 声学** | 遮挡射线、材质吸收、门户、混响区、IR 烘焙、FDN 尾 | A3 + 场景 BVH | `Acoustics` | 全墙 -12±3 dB、截止 800 Hz±20%；无跳变 | 3.5 周 |
| **A7 音频图 / 参数** | 节点 DAG、块内参数、包络/LFO、`SoundPatch`、程序化合成 | A1 | `AudioGraph` + `.ngpatch` | 参数阶跃无 zipper 噪声；图可确定性渲染 | 3 周 |
| **A8 工具 / 编辑器 / AI** | 场景块、`audio_patch`、编辑器面板、电平表、声场俯视图、试听渲染 | A1–A3 | 面板 + MCP 工具 + 试听 | 预览/提交/撤销闭环；试听与运行时一致 | 2 周 |
| **A9 证据 / CI / 预算** | `.incrementalTesting/core/audio.ps1`、指标、PCM 哈希、频谱图、降档阶梯 | A0–A6 | 报告 + 门禁 | 全部断言来自真实运行 | 1.5 周 |

合计 **22–26 人周**（含缓冲）。首切片 A0–A3 约 **4–6 人周**。

### 5.2 依赖图

```text
A0 ──┬── A1 ──┬── A3 ──┬── A4 ──┐
     │        │        ├── A5 ──┤
     │        │        └── A6 ──┤
     │        └── A7 ───────────┤
     └── A2 ──┘                 ├── A8 ── A9
                                 └────────┘
```

- **可立即并行**：A0 完成后，A1 与 A2 同时开工；A3 依赖两者。
- **A4 与 A5 可并行**：双耳与对象共享 bed 渲染，但末端不同；先约定 `Spatializer` 输出接口（每声道/每对象）。
- **A6 可与 A4/A5 并行**：声学只产出 `{gain, cutoff, send}`，不关心末端。
- **A8 必须等 A3**：没有真实空间化，编辑器面板只能调空参数。

### 5.3 排期（并行）

| 周 | A0 | A1 | A2 | A3 | A4 | A5 | A6 | A7 | A8 | A9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1–2 | ● | ● | ● |  |  |  |  |  |  |  |
| 3–4 |  | ● | ● | ● |  |  |  | ● |  |  |
| 5–6 |  |  |  | ● | ● | ● | ● | ● | ● | ● |
| 7–8 |  |  |  |  | ● | ● | ● | ● | ● | ● |
| 9–11 |  |  |  |  |  | ● | ● |  | ● | ● |

### 5.4 首个可听切片（A0+A1+A2+A3）

范围：

- 48 kHz、256/480 块、shared 后端 + null/offline。
- 5 条 fogtown WAV 导入 `.ngsnd`，银行加载。
- 4 条总线（Master/SFX/Ambience/UI）+ 距离衰减 + 7.1 VBAP + 单射线遮挡。
- `audio.play` 替换 `PlaySoundW`；`npm run fogtown` 听到有方向的门声/爆炸声。
- 最小证据：`.incrementalTesting/core/audio.ps1` 跑 null 后端的空测试、响度、声像、遮挡、确定性 5 组断言。

**不做**：HRTF、对象、混响、音频图、编辑器面板。

门禁：0 underrun、`audio-off` 不影响任何现有截图/摘要、两次离线渲染 PCM 哈希一致、声像误差 ≤0.3 dB、遮挡 -12±3 dB。

---

## 6. 技术细节

### 6.1 重采样

- 内部 48 kHz；设备 44.1/48/96 kHz 一律在**设备边界**重采样，混音图内不混采样率。
- 算法：多相 FIR（Kaiser 窗，β=8.6，64 相位 × 32 tap），阻带 ≥ 90 dB；不做线性插值（会混叠）。
- 流送源允许原始采样率，解码线程重采样后入环形缓冲；**音频线程不做重采样**。
- 变调（pitch）用独立的双线性/多相重采样器，与设备重采样分开，避免参数互相污染。

### 6.2 虚拟 voice 与抢占

评分（越小越该被抢占）：

```text
score = priority * w1 + audibleGain * w2 - distance * w3 + playingTime * w4 + isLoop * w5
```

规则：

- 新声源到达且物理槽满 → 找 score 最低的**非受保护** voice（音乐/对白/玩家自身永不抢占）。
- 抢占必须 **20 ms 交叉淡化**：旧 voice 淡出、新 voice 淡入；禁止硬切（硬切是爆音的第一来源）。
- 虚拟 voice 仍推进播放位置（按采样），复活时从当前位置进入，不从 0 开始——否则远处的循环环境声会“每次复活都重头”。
- 并发组：同一组最多 N 个物理实例（如“脚步”最多 4 个），超出按距离淘汰。

### 6.3 遮挡与平滑

```text
每 voice：
  if (now - lastTrace >= traceInterval[voiceId % 8]) {
      hit = trace(listener → emitter)            // TLAS 优先，CPU BVH 回退
      target = hit ? surfaceTable[hit.material] : open
  }
  gain   = onePole(gain,   target.gain,   80ms)
  cutoff = onePole(cutoff, target.cutoff, 80ms)
  send   = onePole(send,   target.send,  120ms)
```

- 只对 `occlusion=1` 且 `distance < 40 m` 的 voice 追射线；远处不追（听不出来，纯浪费）。
- 命中点材质取最近三角形的 `SurfaceType`；没有材质则用默认 `brick`。
- 门户：若射线先命中 portal 且 `open > 0.1`，按 `open` 插值 `gain/cutoff`，并允许穿过后继续追一条（单跳上限）。
- 平滑用一阶低通而不是线性插值：线性插值在目标突变时会有折角，听感是“咔”。

### 6.4 HRTF 分区卷积

- 256 tap @48 kHz ≈ 5.3 ms，覆盖 ITD（≤0.8 ms）+ 耳廓早期反射。
- 均匀分区：块 256 → 每分区 256 tap，共 1 个分区（256 tap 正好一块）；块 480/1024 时按块长分区，FFT 大小 = 2×块长。
- 每个对象 2 个卷积器（左/右），HRIR 按方位插值（相邻 5° 网格双线性），仰角用最近邻 + 增益补偿。
- 对象数 32 时，双耳卷积是最大开销；降档策略：对象 >16 时，非关键对象改走“虚拟床 + VBAP 预混”，只保留 8 个关键对象做独立 HRTF。
- 近场（<1 m）不做距离相关 HRTF（数据集不足），改用“远场 HRTF + 近场增益/低频提升”。

### 6.5 混响

- 早期反射：`.ngir` 分区卷积，长度 ≤0.5 s，单声道 IR → 多声道去相关（每个输出声道用不同起始偏移，偏移量固定，保证确定性）。
- 尾音：8 线 FDN，Hadamard 反馈矩阵，每线内插延迟 + 一阶低通（频率相关 RT60）；参数 `rt60/preDelay/wet/damping` 可实时调。
- 混响区：盒体（先做盒体，胶囊/网格 P2），进入/离开用 300 ms 交叉淡化；重叠区域按“最近边界距离”加权混合。
- 送量：每条总线有 `reverbSend`；遮挡命中时送量 ×1.5（“隔墙听到更多混响”）。

### 6.6 确定性

| 项 | 规则 |
| --- | --- |
| 随机 | 命名流 `audio.variation`，注册进现有 RNG 流表；变体选择只用该流 |
| 调度 | 命令按 `targetSample` 排序；同采样点按入队序号稳定排序 |
| 浮点 | 混音用 f32；同一编译器/同一构建下确定；跨编译器不承诺 |
| 哈希 | 离线渲染输出 f32 → 量化 i16 → FNV-1a；报告写 `pcmHash` 与 `frames` |
| 边界 | 设备输出不进哈希；`underruns` 与 `lateCommands` 单独统计，出现即判失败 |
| 回放 | replay 记录音频命令流（类型/ID/采样点/参数），回放时用 offline 后端重渲染并比对哈希 |

**硬规则：音频只读玩法状态，绝不写回。** 玩法摘要不包含音频状态；否则一次音量调整就会让 replay 判定失败。

### 6.7 内存与分配

- 启动时一次性分配：voice 池、图节点、每块临时缓冲（对齐 64 B）、卷积 FFT 工作区。
- 流送缓冲：每流 2×1 s 环形；银行常驻 ≤ 32 MB（可配）。
- IR 池：≤ 8 条 × 0.5 s × 2 ch × f32 ≈ 1.5 MB。
- 实时线程禁止 `malloc`：用 `AudioAllocator`（线性 + 固定块），调试构建下挂钩检测。

---

## 7. 性能预算

基准：48 kHz、块 256（5.33 ms）、单核、桌面级 CPU；目标 **≤ 2.2 ms/块（约 41% 单核）**，P95 不超 3.0 ms。

| 项 | 预算 | 说明 |
| --- | --- | --- |
| 命令应用 + voice 管理 | 0.15 ms | 64 物理 / 1024 虚拟 |
| 64 物理 voice（增益 + 1 biquad + 距离） | 0.55 ms | 每 voice ~8.6 µs |
| 空间化（VBAP 7.1.2） | 0.20 ms | 每 voice ~3 µs |
| 双耳（10 虚拟床 + 8 对象 HRTF） | 0.90 ms | 最贵项；对象 >16 时降档 |
| 混响（2 路卷积 ER + 1 路 FDN） | 0.45 ms | 卷积长度 ≤0.5 s |
| 总线 DSP（压缩/限制/表计） | 0.15 ms | 6 条总线 |
| 输出/重采样 | 0.10 ms | 设备 44.1 kHz 时 |
| **合计** | **2.50 ms** | 略超目标 → 降档到 1.8 ms（见下） |

> 上表是**估算**，A9 必须用真实测量替换；任何“预算内”结论都必须带 `reports/audio/*.json` 的 P50/P95。

降档阶梯（与 `BUDGET.md` 同构，单向、每档只走一次、写原因）：

| 档 | 动作 | 预期收益 |
| --- | --- | --- |
| 1 | 双耳对象 8 → 4，其余并入虚拟床 | -0.35 ms |
| 2 | 混响 2 路 → 1 路，FDN 8 线 → 4 线 | -0.20 ms |
| 3 | 物理 voice 64 → 40 | -0.20 ms |
| 4 | 遮挡射线间隔 50 ms → 100 ms | -0.10 ms |
| 5 | 回退 `audio-spatial`（关 HRTF） | -0.90 ms |

内存预算：

| 资源 | 预算 |
| --- | --- |
| 常驻银行 | 32 MB |
| 流送环形 | 8 路 × 2 s × 48 kHz × 2 ch × f32 ≈ 6 MB |
| IR + FDN 状态 | 4 MB |
| 图/voice/元数据 | 3 MB |
| **合计** | **≤ 45 MB** |

延迟目标：

| 模式 | 块 | 目标 P50 端到端 |
| --- | --- | --- |
| `low` | 256 | ≤ 12 ms（IAudioClient3） |
| `balanced` | 480 | ≤ 22 ms |
| `safe` | 1024 | ≤ 40 ms |
| 对象/空间音频 | 系统决定 | ≤ 45 ms（元数据流本身有缓冲） |

---

## 8. 验收门禁

每条都必须有**可复现命令 + 报告字段 + 阈值**，不允许“听着差不多”。

| # | 检查 | 方法 | 门限 |
| --- | --- | --- | --- |
| 1 | 空测试 | 旁路图（输入→输出），f32 路径 | 最大绝对误差 ≤ 1e-6（-120 dBFS） |
| 2 | 响度 | 离线渲染 60 s 粉噪 + 节目素材，BS.1770-4 | 积分 -16.0 ±0.5 LUFS；真峰值 ≤ -1.0 dBTP |
| 3 | 声像 | 8 个方位 × 7.1.2，VBAP 增益 | 单声道源增益误差 ≤0.3 dB；总能量 ≤0.5 dB |
| 4 | 多普勒 | 10 m/s 迎面，343 m/s | 频比 1.0300 ±1 cent |
| 5 | HRTF ITD | 与 Woodworth 模型对比（±90°） | ≤30 µs |
| 6 | HRTF ILD | 4 kHz 处 -90…+90° | 单调，与数据集偏差 ≤2 dB |
| 7 | 遮挡 | 全墙 vs 无墙，1 kHz 倍频程 | 宽带 -12 ±3 dB；截止 800 Hz ±20% |
| 8 | 遮挡平滑 | 目标突变时逐样本差分 | 无 > -60 dBFS 的阶跃；无爆音 |
| 9 | 虚拟化 | 1024 发射器，64 物理 | 提升/降级 20 ms 交叉淡化，无跳变 |
| 10 | 并发组 | 同组 32 个实例，上限 4 | 物理实例 ≤4；淘汰按距离稳定 |
| 11 | 循环 | 1 s 循环素材，边界 ±10 ms | 无咔哒（边界样本差 ≤ -80 dBFS） |
| 12 | 确定性 | 同命令流离线渲染两遍 | `pcmHash` 相同；`frames` 相同 |
| 13 | 帧率无关 | 帧时间 8/16/33 ms 三组 | 事件采样点偏差 0；PCM 哈希相同 |
| 14 | 对象位置 | 32 对象已知方位，元数据回读 | 角度误差 ≤0.1°；增益 ≤0.1 dB |
| 15 | 下混 | 7.1.2 → 5.1 → 2.0，粉噪逐声道 | 能量偏差 ≤1 dB；无削波 |
| 16 | 延迟 | 环回测量（输出→输入） | 符合 §7 表 |
| 17 | 稳定性 | 10 分钟连续播放 + 设备切换 | `underruns=0`；切换后 500 ms 内恢复 |
| 18 | 回退 | `audio-off` / `audio-stereo` | 现有截图与玩法摘要**逐字节不变** |
| 19 | 预算 | 1080p 场景 + 64 voice | P50 ≤2.2 ms、P95 ≤3.0 ms；降档有原因 |
| 20 | 迁移 | `npm run fogtown` 全流程 | 5 条提示音全部经混音器播放；无 `PlaySoundW` 调用 |

证据产物：

```text
reports/audio/
  null-test.json           # 空测试逐样本误差
  loudness.json            # LUFS / dBTP / LRA
  panning.json             # 8 方位增益表
  hrtf.json                # ITD/ILD 表 + 数据集哈希
  occlusion.json           # 有/无墙能量与频谱
  atmos.json               # 对象元数据 + 下混矩阵 + ADM 校验
  determinism.json         # pcmHash / frames / 两组输入
  latency.json             # 环回样本
  budget.json              # voice 数 / CPU P50/P95 / 内存
  spectrogram-*.png        # 频谱图（png_writer）
  field-topdown.png        # 声场俯视图（对象/床/遮挡）
```

命令：

```powershell
# 全量音频证据（null + offline 后端，不需要声卡）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/audio.ps1

# 单组
powershell -File .incrementalTesting/core/audio.ps1 -Only loudness,panning,occlusion

# 运行时
build\NativeGPU.exe --scene .screen/fogtown.scene.json --audio on `
  --audio-backend wasapi --audio-quality binaural --audio-latency low
```

---

## 9. 编辑器与 AI 工作流

| 能力 | 形态 | 依赖 |
| --- | --- | --- |
| 音频面板 | 发射器列表、增益/衰减/总线、试听、solo/mute | A8 |
| 实时电平 | 每总线 peak/RMS/LUFS 短时表 | A1 |
| 声场俯视图 | 俯视 2D：listener、发射器、对象槽占用、遮挡射线命中 | A3/A6 |
| 遮挡可视化 | 命中点 + 材质 + 当前 gain/cutoff | A6 |
| 混响区 | 盒体 gizmo、IR 选择、湿度 | A6 |
| 对象槽表 | 32 槽占用、优先级、snap 状态 | A5 |
| 试听渲染 | 离线渲染 5 s → WAV → 浏览器 `<audio>` 播放 | A9 |
| `audio_patch` | 预览差异、预估 CPU、提交/撤销 | A8 |

AI 闭环（与现有 `material_patch` 一致）：

```text
自然语言 → 类型化 AudioPatch（emitter/attenuation/zone/snapshot）
        → 校验（schema + lint + 预算）
        → 预览（差异 + 预估 CPU + 试听 WAV）
        → 提交（revision+1，原子写）
        → 运行时热重载（不打断音乐）
```

规则：

- AI **不能**直接写 `.ngsnd`/`.ngir` 二进制；只能引用已有资产或生成 `.ngpatch`/`.nggraph` JSON。
- 任何“更响/更近/更多混响”的修改必须给出**可测后果**（LUFS、对象数、CPU 变化），不能只说“更有氛围”。
- 试听 WAV 与运行时用**同一套 offline 后端**，否则“编辑器好听、游戏里不一样”。

---

## 10. 创新点

### A-I1. 逐 voice 音频收据

每个物理 voice 输出：资产、总线、距离、遮挡状态、HRTF tap 数、CPU 微秒、提升/降级次数。预算器按收据决定“哪条声音值得占物理槽”，而不是只看整帧音频耗时。对应渲染侧的“逐灯收据”。

### A-I2. 帧率无关的采样精确事件

命令带 `targetSample` 而非帧号；8 ms 与 33 ms 帧率下事件落点逐样本一致。这让“音频不参与玩法确定性”与“音频自身可回归”同时成立，多数引擎只做到前者。

### A-I3. 复用渲染 TLAS 的声学

遮挡/早期反射直接打 DXR TLAS（CPU BVH 回退），不新建声学几何、不做手工 occluder 标记。场景一改，声学自动跟着改——这是“场景是唯一真相”的直接收益。

### A-I4. 无授权也能验收的对象音频

自研对象渲染器 + ADM BWF 导出 + 参考渲染器比对，使对象位置/增益/下混/响度在没有 Dolby 授权的机器上也可验收；Dolby 直通只是可选输出，不是唯一真相。

### A-I5. 类型化音频补丁 + AI 提交闭环

音频不是“丢一堆 WAV 让策划手调”，而是可 diff、可回滚、可预估成本的 JSON 补丁；AI 生成的修改必须带度量后果。

### A-I6. 混响的“双引擎”

短 IR 卷积负责早期反射（空间感、材质感），FDN 负责可实时调的尾音（RT60/湿度）；避免“换混响就要重载 IR”的卡顿，也避免纯算法的“金属盒子味”。

### A-I7. 材质驱动的音效

复用材质表 `SurfaceType`：脚步、撞击、子弹命中自动选音色与遮挡吸收，不需要每个物件手挂音效。场景材质一改，声音跟着改。

### A-I8. 音频降档收据

对象 32→16→8、HRTF→虚拟床、64→40 voice，每次降档写原因与频谱差异，和渲染的 `vol-off` 回退同一套证据格式。

### A-I9. 离线可回放的音频回归

`pcmHash` 进 CI：改混音器、改重采样、改 HRTF 插值，只要输出变了就必须解释。音频第一次拥有和图像回归同级的门禁。

### A-I10. 声场俯视图作为一等证据

对象槽、遮挡射线、混响区、听者朝向画成一张俯视图，和 `reports/town/hero.png` 一样进报告——让“空间感”可被看见，而不是只能靠耳朵争论。

---

## 11. 风险与反模式

### 11.1 风险

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| Dolby 授权/终端不确定 | `audio-atmos` 在多数机器不可用 | 自研渲染器为默认；直通为可选；文档不声称认证 |
| HRTF 数据集许可 | 不能随包分发 | 只用可再分发数据集（如 MIT KEMAR 需确认条款）或自录；记录数据集哈希与许可证 |
| WASAPI 独占模式冲突 | 其他程序占设备 | 失败自动回 shared，写原因，不弹窗 |
| 实时线程分配/锁 | 爆音、underrun | 启动期全量分配；调试构建挂钩检测；CI 跑 10 分钟稳定性 |
| 遮挡射线成本 | 64 voice × 20 Hz 射线 | 错峰 + 距离剔除 + TLAS |
| 双耳 CPU 超预算 | 掉帧/降档 | 对象 8 上限 + 虚拟床预混 + 阶梯降档 |
| 音频影响玩法摘要 | replay 误判 | 单向数据流，音频状态不进摘要 |
| 场景 schema 三处不同步 | 能加载但不合法 | 与材质字段同样的三处同步 + lint 门禁 |
| 循环点/重采样瑕疵 | 咔哒声 | 边界样本门限 + 循环交叉淡化 |

### 11.2 反模式（禁止）

1. **在主线程 `PlaySound`/同步解码**：现在的 `PlaySoundW` 就是这条路，必须删。
2. **用帧号调度音频**：帧率一变，节奏全乱。
3. **遮挡做成布尔开关**：听感是“咔”，必须是平滑的 gain/cutoff。
4. **抢占硬切**：爆音第一来源；必须交叉淡化。
5. **虚拟 voice 复活从 0 播**：远处环境声会“每次复活都重头”。
6. **对象数无上限**：32 是预算，不是建议；超了必须 snap。
7. **下混不做限制器**：家庭影院切立体声直接削波。
8. **把音频写回玩法状态**：确定性契约立刻破。
9. **混响全用卷积**：换参数要重载 IR，实时调不动；用 FDN 尾。
10. **只在编辑器里好听**：试听必须走运行时同一后端。
11. **静默降档**：每次降档必须有原因与频谱/哈希差异。
12. **在实时线程打日志**：IO 抖动 = underrun。

---

## 12. 迁移与版本路线

| 版本 | 内容 | 判定 |
| --- | --- | --- |
| 0.12.0 | A0+A1+A2+A3：混音图、总线、5 条 WAV 迁移、7.1 VBAP、单射线遮挡 | 首个可听切片 |
| 0.13.0 | A4+A6：双耳 HRTF、混响区、门户单跳、IR 烘焙 | 耳机与室内场景可用 |
| 0.14.0 | A5+A8：对象音频、下混、ADM 导出、编辑器面板、`audio_patch` | 全景声与 AI 闭环 |
| 0.15.0 | A7+A9：音频图、参数调制、完整证据门禁、预算阶梯 | 对标 UE 主干完成 |

迁移步骤（0.12.0 内）：

1. `assets/fogtown/audio/*.wav` → `tools/audio.mjs import` → `.ngsnd` + `.ngbnk`。
2. `host/main.cpp` 的 `audio.*` 分支改为 `audio.play("fogtown/<name>")`，保留事件名兼容。
3. `samples/fogtown` 增加 `audio` 块（门、爆炸、供能、完成、失败各一个发射器）。
4. 删除 `PlaySoundW`，`grep -r PlaySoundW` 必须为空（进 CI）。
5. `tools/fogtown.ps1` 增加一条断言：混音器 voice 计数 > 0 且 `underruns=0`。

---

## 13. 参考

**本仓库**

- [DEV-TASKS.md](DEV-TASKS.md) E03：音频缺口原始记录。
- [BUDGET.md](BUDGET.md)：降档阶梯与收据格式。
- [DETERMINISM.md](DETERMINISM.md)：命名 RNG 流、FNV-1a 摘要、回放契约。
- [RT-SHADOWS.md](RT-SHADOWS.md)：TLAS 与射线基础设施。
- [MATERIAL-PIPELINE.md](MATERIAL-PIPELINE.md)：材质表与 `SurfaceType` 来源。
- [ERROR-CONTRACTS.md](ERROR-CONTRACTS.md)：错误码风格（`NG-A-*`）。
- [VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md)：并行工作包与验收门禁的写法基线。

**外部**

- ITU-R BS.1770-4, *Algorithms to measure audio programme loudness and true-peak audio level*.
- ITU-R BS.2076, *Audio Definition Model*（ADM BWF）。
- EBU R128, *Loudness normalisation and permitted maximum level of audio signals*.
- Dolby, *Dolby Atmos Home Entertainment Technical Guidelines*（bed 7.1.2、对象上限与元数据语义）。
- Microsoft, *Spatial Sound for Windows*（`ISpatialAudioClient`、动态对象/静态 bed/元数据流）。
- Microsoft, *WASAPI* / *IAudioClient3*（低延迟共享模式）。
- J. Blauert, *Spatial Hearing*（ITD/ILD/仰角线索）。
- V. Pulkki, *Virtual Sound Source Positioning Using Vector Base Amplitude Panning*（JAES 1997）。
- J. O. Smith, *Physical Audio Signal Processing*（FDN 与延迟线混响）。
- UE5 文档：*Audio Overview*、*MetaSounds*、*Submixes*、*Attenuation*、*Audio Bus*、*Quartz*、*Concurrency*、*Virtual Voices*。

---

## 附：一句话回答

**先做 48 kHz 混音图与采样精确时钟（A0/A1），再做 bed 空间化与遮挡（A3/A6），末端分叉成扬声器/双耳/对象三条路径（A4/A5）**；UE 的主干能力按 P0/P1 逐项对齐，Atmos 走“自研参考渲染器 + 可选直通 + ADM 导出”的兼容路线，不声称认证。首个可听切片 4–6 人周，完整 22–26 人周，每条结论都必须有 `reports/audio/*.json` 与 `pcmHash` 支撑。
