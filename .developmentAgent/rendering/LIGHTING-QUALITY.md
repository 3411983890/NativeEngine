# 光照、雾和云质量实现

日期：2026-10-21。实现入口：`renderer/lighting_quality.cpp`、`shaders/lighting_quality.hlsl`、`shaders/lighting_quality_common.hlsli`。本文件描述运行代码；早期研究文档中的完整 Lumen、10k 灯和粒子统一计划仍属于后续范围。

## 五项交付

1. **光照正确性**：cooked 材质的 key 光只写入独立直射通道；合成只应用一次阴影。DXR 从 `Environment.keyDirection` 获取太阳/月亮方向，与表面和体积使用同一参数。场景拒绝零方向。帧常量改成按在途帧独立分配，避免 CPU 覆盖尚在使用的光照数据。
2. **局部灯投影**：点光和聚光分别查询自身的世界空间可见性，阴影作用于对应灯的漫反射与高光。按亮度、范围和距离选择重点灯，保留稳定 ID、25% 选择滞后及八帧进出淡化。平衡档最多两盏，高档最多四盏，其余局部灯仍不投表面阴影。最多四盏静态灯缓存六面 128² 径向深度图，静态几何或灯的位置/范围改变时重建；车辆和门通过当前 TLAS 查询动态遮挡，不触发静态缓存重建。PCF 按接收面在各采样射线上的真实距离比较，控制自阴影条纹。
3. **Froxel 雾与光轴**：对数深度网格，范围到 96 m；高度雾与最多八个带软边界的局部密度盒相加。太阳/月亮通过 froxel 世界坐标查询 DXR 可见性；每格按实际散射贡献选择四盏局部灯，其中前两盏可查询体积阴影。使用 HG 相位、Beer–Lambert 积分，在 HDR 空间按 `scene*T+L` 合成。稳定光照重投影并裁剪历史，移动车灯每帧重算，避免把车灯光柱写进长期历史。
4. **天空 IBL 与 DDGI**：同一程序化天空生成漫反射 SH、GGX 预过滤立方体和 split-sum BRDF LUT。DDGI 使用 16×8×16 探针、4 m 间距、每探针 64 条射线；命中读取实例变换、纹理、材质颜色、表面状态、自发光和带遮挡的直接光。八面体图集存储辐照度及距离一、二阶矩，并进行探针迁移、无效探针排除、可见性加权插值和变化自适应历史累积。天空可见比例单独保存，避免把天空补光算两次。
5. **体积云光照**：持久 64³ 三维噪声和天气纹理，世界空间风场平移。沿光方向次级积分提供自阴影，128² Beer 透射图向表面和雾投影；三档衰减项近似多次散射。云在半分辨率积分，按不透明深度截断，使用首个云样本的距离、风运动和前帧矩阵重投影，最后深度感知重建。切镜、场景参数变化和资源重建使历史失效。

## 质量与回退

| `lighting.quality` | 雾网格 | 表面阴影灯上限 | 每帧探针更新 | 云视线/光线步数 |
| --- | --- | --- | --- | --- |
| 0 | 兼容全屏雾/云 | 仅既有 key 阴影 | 无 | 既有 14 步云 |
| 1 | 80×45×48 | 2 | 64（4096 条主射线，另有命中点可见性射线） | 24 / 4 |
| 2 | 160×90×64 | 4 | 256（16384 条主射线，另有命中点可见性射线） | 64 / 12 |

`town.scene.json` 默认使用 1 档。新路径需要 cooked 材质、SM 6.6、支持 inline ray query 的 DXR 1.1 和 TLAS。创建失败时保留兼容渲染，原因写入 `probes.lightingQuality.error`；能力和实际执行状态分别报告。改变质量档、尺寸或设备时重建资源。

场景 `lighting` 支持独立布尔开关 `localShadows`、`ibl`、`ddgi`、`cloudShadows`、`temporal`，以及整数 `maxShadowLights`（0–4，仍受质量档限制）。`cloudShadows` 同时控制云自阴影与投影；`temporal` 控制雾和云的视图历史，DDGI 自身仍使用探针历史。

局部雾示例：

```json
"fogVolumes": [
  {"center": [0, 2, 8], "extent": [20, 6, 8], "density": 0.03}
]
```

`center` 和 `extent` 均使用世界空间，`extent` 为完整尺寸。密度按距离的倒数计量。`fogColor` 保留给兼容档；froxel 档采用接近白色的散射反照率，由实际入射光决定雾色。

静态灯的 `light` 对象支持 `castSurfaceShadow`、`castVolumetricShadow` 和 `volumetricIntensity`（0–10）。渲染器 `LightDesc` 暴露同名字段，移动灯也可独立配置。体积阴影选择与表面阴影选择分开；两者均有数量预算。

## 验收与复现

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/lighting-quality.ps1
```

默认构建到独立的 `build/lighting-quality/`，可与正在运行的主程序共存。已有构建可传 `-SkipBuild`。脚本生成固定场景、PNG、运行报告和 `reports/lighting-quality/evidence.json`。

检查包括：局部表面遮挡、独立体积遮挡、太阳/月亮光轴、IBL、DDGI 漫反射反弹、云自阴影和地面投影、均匀密度解析透射率、局部密度盒、相同输入截图哈希，以及切镜第一帧与禁用历史参考一致。另用既有庭院图像基线与尺寸/设备重建测试检查兼容性。

调试参数：`--debug-view fog` 显示透射率，`scatter` 显示体积散射，`cloud` 显示云透射率，`cloud-shadow` 显示地面云阴影；原有 `mask`、`key`、`base` 视图继续可用。`--camera-cut frame,x,y,z,targetX,targetY,targetZ` 用于固定帧切镜验收。

`probes.lightingQuality.gpuPasses` 在预热后报告 DDGI、静态灯缓存、云阴影、froxel 和云的 P50/P95 及样本数。主报告的 GPU 时间是既有图形主渲染区间，包含表面光照、体积与合成，不含前面的 TLAS 构建或独立 compute 队列；不能把几项新增 pass 时间当作整帧成本。最终实测见上述 evidence 文件。

## 实现边界

- 当前 TLAS 使用单位盒 BLAS 实例；静态遮挡物、动态遮挡物与灯外壳分别使用位 1/2/4。灯外壳仍可被 GI 与 key 光查询，局部灯可见性排除外壳，避免灯挡住自己的光源。车辆由刚体盒组成，可以遮挡。蒙皮角色不在 TLAS 中。新增任意网格之前需要对应 BLAS 和命中材质读取。
- DDGI 当前交付一次漫反射反弹；镜面由 IBL 提供。探针覆盖相机附近约 60×28×60 m，超出覆盖后重新定位并收敛；这不是完整 Lumen 或多级世界探针缓存。
- 云使用有真实密度消光与光向积分的实时体积模型，多次散射仍为近似；高档提高采样量，不宣称离线路径追踪精度。
- 既有粒子烟仍有独立渲染路径，本次没有把粒子模拟密度并入大气 froxel。大量灯的 clustered 分箱与完整天空大气另行推进。
- 确定性验证限定同一适配器和相同输入/帧数；D3D12 调试层是否可用会随报告一同记录。

技术对照：[Epic 体积雾](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine)、[Epic 体积云](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine)、[NVIDIA DDGI](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Algorithms.md)。
