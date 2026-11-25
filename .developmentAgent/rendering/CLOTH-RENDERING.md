# GPU 布料绘制与宿主预览

2026-10-24；本批推进完整方案 E2/E4 的绘制和产品生命周期，E 与 A–G 仍未完成。求解模型和共享风合同见 [CLOTH-GPU.md](CLOTH-GPU.md)。

后续新增 [组内求解与数值编译合同](CLOTH-GROUPED.md) 和 `--cloth-execution auto|dispatch|grouped`。随后多实例批处理见 [CLOTH-BATCH.md](CLOTH-BATCH.md)，当前完整 shader 清单 80 个、独立布料专项 17 个；本页原先的 73/15 个及计时表保留为相应冻结版本的历史证据。

## 真实绘制路径

`renderer/gpu_cloth_draw.*` 从求解器的 80 字节 GPU 顶点和 48 字节膜记录读取位置/法线/切线与三角索引。VS 使用 `SV_VertexID` 展开三角形，不把形变顶点读回 CPU，也不逐帧上传 CPU 形变副本。静止网格/质量等初始化上传仍存在。

`draw` 在记录命令前验证设备、真实 fence、材质、光照与矩阵；求解器和绘制者必须使用**同一个实际 fence 对象**。`prepareRead` 转换顶点到 NON_PIXEL_SHADER_RESOURCE，并把本次绘制使用登记到求解器。下次模拟恢复 UAV；两者 shutdown 都拒绝在途使用。绘制类不创建 buffer、描述符 heap、队列或读回资源，root/pipeline 等驱动内部开销未计入布料 buffer 预算。调用者必须绑定两张 RTV、深度 DSV 与 viewport/scissor。

graphics root 为 48 DWORD（44 个常量加两个 SRV），`cloth_draw.hlsl` 新增两个 VS/PS blob。单独绘制门禁只编译 13 个求解加 2 个绘制 blob，标准完整 shader 清单当前为 73 个；标准清单只在宿主依赖构建中使用，不表示执行了完整测试。

## 双面、UV 与着色

- 关闭背面剔除，根据实际光栅正反面翻转世界法线，并修正背面的切线手性。测试使用与引擎一致的左手相机；相机位于 +Z 向 −Z 看时视图 X/Z 会翻转。
- 当前模拟网格直接作为渲染网格。U/V 分别沿烘焙材料 +X/+Y，底部 V=0，取网格列/行坐标。它是有效的规则网格 UV，不是导入网格的 UV 接缝或重心映射。
- 经/纬两个粗糙度、sheen、线密度和有界线法线幅度由 `ClothSurface` 给出；使用导数淡出亚像素织纹。默认不从纹理资产采样。
- 环境光写入现有 surface MRT；主光漫反射与柔和 sheen 写入 key MRT，沿用后续阴影/合成。sheen 采用 [Filament 文档的 Charlie 分布与 Neubelt 可见性](https://google.github.io/filament/main/filament.html#materialsystem/clothmodel)；经/纬方向的粗糙度插值是本项目的有界近似，未声称实现归一化各向异性分布或完整能量补偿。
- 光照参数遵守现有 scene 的线性 RGB [0,1]、主光强度 [0,100] 合同，避免共享 FP16 MRT 溢出。粗糙度范围 [0.05,1]，非法参数直接拒绝。
- 仅支持不透明布。暂未接入本地灯、IBL/DDGI、纹理材质图、透射/次表面、alpha 镂空或布料 BLAS 投影；静态遮挡可经现有 key composite 影响接收面，不等于布料能向场景投影。

调试模式 `0=布料着色、1=朝向世界法线、2=网格UV、3=材料切线`。调试输出用于检查数据，不代表织物外观验收。

## 渲染器与宿主

`configureGpuCloth(configs, driver)` 在 renderer 停止时配置 1–32 个环境布实例，复制不可变资产。所有实例的 committed 模拟 buffer 合计受 64 MiB 上限限制。`GpuClothFrame` 在 renderer 实际图形命令时间线上提供 solver 及 step/teleport/reset 回调；宿主控制逻辑 tick，自己不持有命令列表。

模拟位于该帧图形命令起点，绘制位于不透明场景之后、体积/精灵之前，写入共享深度；后续透明绘制能被布料遮挡。模拟和绘制分别有已完成 fence 下读取的时间戳，并包含在整帧 GPU 时间中。尚无统一异步 compute/RHI 调度。

`host/cloth_preview.*` 提供固定 120 Hz 视觉驱动，CLI 支持：

| 参数 | 合同 |
|---|---|
| `--cloth id`、`--cloth-library path` | 从认证 cook 库选择资产；仅 /2 环境布 |
| `--cloth-count 1..16` | 沿 X 分开摆放，同一资产的独立实例 |
| `--cloth-origin x,y,z`、`--cloth-wind x,y,z` | 初始米制原点与均匀 m/s 风 |
| `--cloth-debug 0..3` | 选择上述绘制模式 |
| `--cloth-pause-tick N`、`--cloth-resume-tick M` | 暂停区间 [N,M)，无恢复 tick 时一直暂停；tick 0 表示禁用事件 |
| `--cloth-teleport-tick N`、`--cloth-teleport-origin x,y,z` | 一次传送，保留形状、清零速度 |
| `--cloth-reset-tick N` | 当 tick 到达时先重置，再执行该 tick；游戏时钟继续 |

向量必须完整、有限，整数参数拒绝非数字尾缀。暂停只冻结布料物理年龄，绘制与游戏时钟继续。一次事件按独立游戏年龄消费，不随物理 reset 重放。

resize 保留 solver；设备/renderer 重建重新从当前原点的静止态开始视觉模拟。重建保留游戏年龄、已发生的传送目标和当前暂停区间，旧事件不重复计数。不是 GPU 形变的精确恢复；不保存布料变形，也不将 GPU 布料加入玩法摘要。游戏时钟倒退明确拒绝，回放/存档下的完整布料策略仍待实现。

独立 solver 可读取共享风快照；本批宿主预览只支持均匀风，尚未为布料提供场租约/发布回调，不能从粒子已有的 `--effect-field` 推断布料也已接入同一作者场。

## 增量使用

```powershell
npm run cloth:draw -- -Adapter AMD
npm run cloth:host
```

两者可用 `-SkipBuild` 复用经 SHA 核对的产物；任何来源变化都会拒绝复用。绘制专项位于 `build/cloth-draw`，宿主构建/烘焙位于 `build/cloth-host`，不使用资产专项的 `build/assets-host`，不覆盖全局布料库。

宿主门禁构建**宿主依赖**及全部必要 shader，不构建其他测试；运行本批真实 CLI、截图、生命周期与错误夹具。独立绘制门禁运行本次绘制/驱动和受影响的 GPU 求解专项，不执行 CPU/JS 全量或完整 CI。标准构建和可选 native GPU 测试目标已登记，本批没有运行那些完整入口。

手动查看已构建的预览：

```powershell
.\build\cloth-host\NativeGPU.exe --scene build/cloth-host/preview.scene.json --cloth flag_banner --cloth-library build/cloth-host/cooked/library.json --camera 2,2.2,4,0,1.3,0 --no-watch --audio off
```

## 验收范围

独立绘制检查实际 VS/PS 工作量、正反面/UV/切线解析像素、两张 MRT、深度前后顺序、GPU 形变后的画面与 reset 的精确恢复，以及真实 queue gate 下的资源拒绝。预览驱动另用分组 tick、重复 tick、倒退 tick、暂停/传送/reset 与设备 epoch 的 GPU 夹具验证。

宿主夹具覆盖 off、零风/有风、四实例、法线视图、暂停/恢复、传送/reset、resize/重建、GPU 粒子同屏、中文库路径和六种拒绝情况。截图不同只证明功能变化；解析像素负责小规模渲染对拍，不能将这些检查写成织物真实感、角色穿透或生产性能达标。

2026-10-24 最终证据：

| 专项 | 真实结果 | 冻结报告 |
|---|---|---|
| GPU 绘制与预览时钟 | 5 组、255 次检查，36 个解析像素分量；实际 486 个 IA 顶点、8502 次 PS invocation | [cloth-draw-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T182726Z/cloth-draw-evidence.json) |
| 受影响 GPU 求解 | 7 组、2905 次，160 次 CPU/GPU 顶点对拍 | 同上 solver 字段 |
| 真实宿主 | 66/66，18 个运行；正常运行均 debugLayer=true、errors=0 | [cloth-host-evidence.json](../reports/0.11.0/31c00bc53c0b/20261023T182726Z-1620b7/cloth-host-evidence.json) |

AMD 890M；单布绘制 240 帧、118560 个三角形，四布 960 次 draw、474240 个三角形。每布 committed buffer 为 786432 字节，四布 3145728 字节。暂停夹具的游戏年龄 478、物理 tick 9；暂停/恢复夹具跳过 30 个物理 tick；reset 夹具不改游戏年龄。resize 与一次 renderer 重建后继续绘制 80 帧，事件没有重复消费。中文库与 ASCII 库截图 SHA 相同。

同输入下，零/有风、四布、调试视图、暂停/恢复、传送/reset、粒子同屏和中文库的游戏摘要与 off 均为 `cf6559390267c935`。GPU 粒子同屏仍有 240 帧与 720 次间接 draw，布料在同一深度/合成管线中显示。

已有宿主 warning 保留原文：off/单布均为 723，GPU 粒子同屏 736；没有将 warning 当作 error 或过滤计数。独立绘制与求解均实际开启调试层、errors=0。报告冻结 65 项绘制输入、186 项宿主输入、32 项宿主资产/门禁输入及二进制、日志和截图，报告对应所冻结版本。

最后已完成的计时样本：单布求解 3.4778 ms、绘制 0.00344 ms；四布求解 11.5052 ms、绘制 0.01184 ms。这是该夹具的最新样本，**不是标准负载 P95**。每色、每子步、每迭代和每实例独立派发的开销很高；不能据此宣布 Low 档四系统合计 4 ms 的目标通过，下一步需优化调度和求解。入口没有执行全量回归。

归档后工作区又新增角色 `renderable.offset` 与空 skin hash 的缓存回退。保留这些用户修改，单独重建了最新依赖，并从已冻结的新二进制运行 off/单布/布料＋GPU 粒子三项 240 帧兼容性检查：验证层错误 0，三项截图 SHA 均与先前对应夹具相同，游戏摘要一致。证据：[cloth-host-dependency-delta.json](../reports/0.11.0/31c00bc53c0b/20261023T183434Z/cloth-host-dependency-delta.json)。新构建源码在测试前冻结，66 项报告仍对应原来的完整布料专项版本；没有把这三项兼容性检查当作新角色位移/空缓存分支的独立回归。

仍需优化按颜色/实例的大量 dispatch，完成标准负载 P95 预算，接入宿主共享场、蒙皮/重心映射、自碰撞/CCD、动态碰撞体、湿润/热耦合、LOD 迁移、完整材质资产、保存/回放与编辑器创作流程。完整方案 E 保持部分完成。


2026-10-24 后续：已新增多实例完整 tick 与法线的统一派发、真实 fence 保护的有界元数据/描述符环，并接入宿主 `stepAll`。单独调用 solver 仍使用本页原路径；最新合同、逐位对拍和配对计时见 [CLOTH-BATCH.md](CLOTH-BATCH.md)。当前布料专项编译 15 个 compute＋2 个绘制 blob（17 个），完整清单在当轮为 75 个（GPU LEF 增量后当前为 80 个）。历史报告数值保持对应其冻结版本，不据此宣称完整 A–G 验收。


2026-10-24 后续共享场接入：新增 `--simulation-field`，无需 GPU 粒子也可为布料提供风，或让粒子/多布共享一份快照。旧入口与本页历史报告保留，最新时间/租约/后绑定/根签名合同及 100 项宿主证据见 [SIMULATION-FIELDS-HOST.md](SIMULATION-FIELDS-HOST.md)。
