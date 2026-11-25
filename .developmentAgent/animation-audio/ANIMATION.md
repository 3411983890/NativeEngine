# 动作系统实现与验证

2026-10-20，工作包 E04，基于 `ANIMATION-PLAN.md` 的第一条可运行纵向链路。

实现更新（2026-10-22）：M1–M4 的运行链路已接入原生角色：传统 Motion Matching、Rig / IK 与脚接触、GPU morph / 规则肌肉、局部 XPBD 软组织，均不依赖机器学习。接口、资产元数据、验收范围和已知限制见 [ANIMATION-M1-M4.md](ANIMATION-M1-M4.md)。M5 面部仍为计划，历史 E04 验收记录保留于下文。

## 运行

```powershell
npm run build:native
npm start
npm run animation
```

默认小院角色使用导入骨架、GPU morph 与 VS 蒙皮。WASD/Shift 由动作匹配选择方向片段，Space 使用 jump / fall，E 触发上半身 interact slot；通知进入动画事件队列，不直接修改钥匙或门的玩法状态。当前为原创人偶和 11 段工程动作，供工程验证，不代表生产动作库质量。

换内容：

```powershell
node tools/offline/animation.mjs import --in character.gltf --out build/animation/custom
build/NativeGPU.exe --character build/animation/custom/character.json
```

默认控制器要求五个同名 locomotion 片段；交互片段 `interact` 为可选。小院 Game 要求 `chest`（上半身 slot 根）与 `right_hand`（钥匙附着）两个骨骼；运行时 Controller 本身不依赖人形命名，`ControllerConfig` 可映射片段名字。加载失败返回 `NG-E-ANIM-*` 并写入运行报告；不静默换成默认角色。打包会带上默认角色的 JSON 和二进制，离线启动无需 Node 或导入工具。

## 已实现的边界

| 方案阶段 | 当前实现 | 验收边界 |
| --- | --- | --- |
| A0 | glTF 单 skin 导入、父先骨架、inverse bind、60 Hz 量化片段、四权重 VS 蒙皮 | 原创 16 骨骼模型；重复导入字节/hash 相同；实际 GPU draw 和截图；不是通用 glTF 导入器 |
| A1 | 256 骨骼 SoA 双缓冲、Trait DAG、稳定顺序、通道掩码检查、五阶段哈希/计时、采样/分层/解析惯性化 | 已接 Select/Blend/Correct/Deform；外部形变历史进入阶段摘要。Emit 仍为扩展点 |
| A2 | 五状态控制器、1D 走跑混合、2D 三角形混合 API、Montage/Slot、逐骨骼 profile、固定 tick Notify、根位移提取 | 2D 是已测 API，未接小院输入；未有脚部 IK，**不能宣称无脚滑**；无 Sync Marker 对齐 |
| A3–A4 / M1–M2 | 已接入传统动作匹配、26 维量化特征、Rig 双骨 IK / Look-at、脚锁定 / 骨盆 / 地面对齐及移动反馈 | 精确穷举，默认 480 帧；固定平台 / 斜坡接触门禁。通用重定向、大型索引、距离匹配和全身运动扭曲仍为后续扩展 |
| A5 | 导入/运行时契约、D0、严格回放，新增平台 / 斜坡接触、软组织位移 / 体积及 LOD / 传送检查 | jerk、生产内容覆盖与跨角色质量门禁仍待扩展；不能泛化为所有移动无脚滑 |
| A6–A7 / M3–M4 | 已接入 GPU morph、姿态驱动肌肉、局部 XPBD / 胶囊碰撞 / 顶点映射、LOD / 传送 / 存档恢复 | 原创两组肌肉和两区八代理点；局部视觉形变，不包含生理肌力、全身 FEM 或自碰撞 |
| A8–A12 / M5 | 尚未实现 | 共享 morph 底座可复用；面部绑定、校正和渲染仍待开发 |

`Game` 的无角色资产用法仍支持旧测试样板；原生宿主启动必加载角色资产，不会因为资产错误走旧样板。动画只存在于运行态，不写场景事务或 `assets/`。

## 数据与导入子集

参考 [Khronos glTF 2.0 规范](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html) 的 skin、动画采样、JOINTS/WEIGHTS、inverse bind 语义。

- `.gltf` JSON，buffer 为 base64 或同目录 `.bin`，延用 B7 路径策略；不支持 GLB、扩展、压缩或稀疏 accessor；新增 morph POSITION / NORMAL，见 M1–M4 合同。
- 一个 scene、一个 identity 根 mesh node、一个 skin、一个 indexed TRIANGLES primitive；POSITION/NORMAL 必需，UV 可选，JOINTS_0 为 U8/U16，WEIGHTS_0 为 FLOAT 或归一化 U8/U16。
- 最多 256 个骨骼/辅助节点，单根、父先、无环、名字唯一；保留 joint 的全部祖先节点，重排后同步重映射 joints 与 inverse bind。
- 节点使用 TRS；正的均匀 scale；LINEAR/STEP translation/rotation/scale 轨道；旋转用最短弧 slerp；其他内容明确拒绝。
- 动作固定 60 Hz，最后一个样本是端点；变换十分量乘 10,000 后编码为 `int32le`。运行时将四元数归一化。片段最大 120 秒、最多 64 段、总包上限 64 MiB。
- `animation.extras.nativegpu` 支持 `{loop: boolean, notifies: [{frame, name}]}`；元数据进入文本头。
- 输出为 `character.json` + `animation.bin`。几何顶点为 64 字节（position/normal/UV/u32 joints/float weights），索引 `uint32le`；片段为 frame-major。头包含骨架、片段偏移、通知与二进制长度及 FNV-1a 检错 hash。
- 以上 glTF 子集说明保留 E04 历史；当前新增 POSITION / NORMAL morph、Rig / 肌肉 / 软组织与匹配元数据，写出 `nativegpu.animation/2` 并兼容读取 `/1`，详见 M1–M4 资产合同。glTF 稀疏 accessor、GLB 和面部功能仍未支持。
- 工具返回整个头+二进制的 SHA-256 `contentHash`；运行时另计算实际头+二进制的 64 位 `runtimeFingerprint`，进入 gameplay digest。两者用途和算法不同。
- 导入只写显式 `--out`，默认构建产物在 `build/animation/`。生成器是 `tools/offline/animation-fixture.mjs`；不修改源 glTF，不下载内容。

## 确定性与扩展规则

固定 120 Hz 控制器与现有 Game 相同步；渲染不推进动画。片段 tick/状态/蒙太奇/弹簧偏移与速度/双缓冲的必要历史/内容指纹都进入 `digest()`。计时只供报告，绝不进入摘要。

Graph 的排序键为 `(stage, priority, id)`；`after` 必须引用排序中更早的节点，违反顺序或成环直接拒绝，不根据依赖悄悄改顺序。Select/Blend/Correct 必须确定；Select/Emit 不得写骨骼；Deform 不得写根。每次 trait 后检查全部 256 骨骼和 32 曲线，超出声明立即 `NG-E-ANIM-MASK`。reads 是数据流声明，不能对直接数组读取做动态拦截；writes 是实际强制检查。

S1 哈希包含 Intent 与选择；S2–S4 累积量化后的确定性写通道；S5 不进入哈希。空阶段也有可定位的链节点，但没有对应效果。当前业务 trait 全确定；未来非确定性形变不得反向进入 Select/Blend。

惯性化采用绝对 tick 年龄的临界阻尼解析解；旋转偏移用 quaternion log/exp，不做逐帧弹簧积分。解析尾部延续至低于量化网格。Montage 使用逐骨骼 mask/profile，100 ms smoothstep 权重包络，结束恢复 locomotion。Notify 按跨过的固定 tick 派发，`Controller::events()` 提供单 tick 事件，Game 跨子步聚合到队列，宿主用 `Game::takeAnimationEvents()` 消费，不会丢失一个渲染帧第一个子步的事件，也不会调用 gameplay 写接口。

根位移通过 `Clip::rootDelta()` 跨循环提取，当前小院采用 in-place 模式，角色平移仍由扫掠碰撞控制器决定。尚未实现根旋转提取、距离匹配或自动用根位移推动碰撞。

## 渲染与报告

最多同时一个蒙皮角色，256 bones、每顶点四权重；静态物体继续走原 Mesh Shader/VS，角色走独立 VS。几何在每个在途帧首次遇到内容时上传一次；palette 使用三套 fence 隔离的 upload buffer，每帧仅更新骨骼矩阵。生命周期重建释放所有资源。

DXR TLAS 仅包含 rigid objects，角色可接收静态物体阴影；角色自己的变形阴影未实现，报告明确 `dynamicBlas=false`。禁止把蒙皮角色当单位方盒加入 TLAS。

运行报告包含 `animation`（状态/频率/指纹/五阶段 hash 和 µs、匹配候选代价与有界决策记录、接触 / 肌肉 / 软组织指标）以及 `probes.skinning`（实际 skin / morph / soft draw 次数）。功能关闭对照明确报告 false；面部能力仍为 false。

`.incrementalTesting/core/animation.ps1` 是独立验收入口，也接入 `verify.ps1`。证据遵循 `reports/<version>/<commit>/<timestamp>/`，`reports/latest/animation-evidence.json` 为快捷别名。门禁包含损坏输入拒绝、重复导入、原生图契约、720 tick 跨运行对拍、固定 tick 通知、60/120 Hz 更新等价、实际 GPU skin draw、截图和严格回放。

现有图像基线不会自动更新；角色外观有变化，若旧基线失败须人工检查并显式接受。

本次已核对旧图、新图与差异图，旧基线变化为 3,208 / 921,600 像素（0.35%），集中于替换的人偶。通过显式 `-Accept` 更新角色基线，`tests/image-baselines.json` 的四个阈值与 measured 值全部保留；接受证据保存在 `reports/0.10.0/31c00bc53c0b/20261020T153454Z/`。设备恢复/50 次生命周期、DXR、Mesh、预算、图像误差与热重载仍走原验证入口。

专项实测：17 项离线测试、5,350 条原生断言、720 tick / 3,600 次阶段哈希对拍；240 帧 GPU 蒙皮录制与严格回放零不符。运行包从自己的目录启动成功，18 个清单文件的 SHA-256 全部匹配，见 `reports/latest/animation-package-evidence.json`。D3D12 validation layer 本机未启用，报告为 false；不能把这次 GPU 冒烟当成 validation layer 检查通过。

完整构建已成功；最终 `verify.ps1 -SkipBuild` 共 21 / 21 步通过，包含 99 项 JavaScript 测试、全部原生测试和既有图形/回放/恢复/热重载门禁。版本化总报告：`reports/0.10.0/31c00bc53c0b/20261020T153544Z/verify.json`（工作区未提交，报告记录 dirty=true）。
