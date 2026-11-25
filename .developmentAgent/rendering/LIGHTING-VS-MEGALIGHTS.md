# 当前灯光系统 vs UE MegaLights：差距评估

状态：2026-10-21，基于当前工作树（未提交）与 `--smoke` 实测。
结论先说：**小场景观感能打，规模不能打。** 当前系统是"64 槽、逐像素全量求值"的确定性前向/延迟混合光路，MegaLights 是"固定样本、随机采样、逐灯 RT 阴影"的随机直接光照。两者差的是成本模型，不是调参。

## 1. MegaLights 是什么（事实基线）

UE5 的新直接光照路径（5.5 起实验性，后续版本继续完善）：

- 随机直接光照（stochastic direct lighting）：每像素固定条数光线，按重要性对灯采样，命中即累加；**成本基本恒定，质量随灯光复杂度下降**。
- 所有局部灯都由它接管，默认每盏灯都能投 RT 阴影；也支持 Virtual Shadow Map 作为逐灯回退。
- 支持矩形/纹理面光源、Light Function、粒子灯、半透明前层、体积雾/半透明体积共享采样。
- 自带时域降噪（SVGF 系：时间方差、5x5 邻域夹、YCoCg 方差裁剪、稀疏空间滤波），并与 TSR 协作。
- 典型预算（SIGGRAPH 2025，PS5，1080p，1 spp，关闭异步计算）：屏幕内 **900+ 盏灯**、每像素 **20–80 盏**，总成本约 **5.5 ms**。

参考：[UE 官方文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine)、[SIGGRAPH 2025 课程整理](https://gerigory.github.io/Siggraph2025-MegaLights-Stochastic-Direct-Lighting-in-Unreal-Engine-5/)。

## 2. 我们现在的实现

| 层 | 现状 | 位置 |
| --- | --- | --- |
| 光源存储 | `QualityLight lights[64]` 常量缓冲，host 侧 `min(count,64)` 静默截断 | `renderer/lighting_quality.h`、`renderer/lighting_quality.cpp` |
| 不透明光照 | 光栅 G-Buffer 上逐像素 `for i < lightCount` 全量求值，范围外靠衰减归零，不做 tile/cluster 剔除 | `shaders/material.hlsl` |
| 局部阴影 | quality=1 时预算 2 盏 RT 阴影灯（时域权重渐入渐出）+ 4 盏静态灯 128² 立方体缓存；quality=2 时 RT 灯 4 盏 | `QualityConstants.lights[].shadow`、`CSLocalShadowCache` |
| 主光阴影 | 关键方向光 1 条 DXR 阴影射线（`QVisibility`） | `lighting_quality_common.hlsli` |
| GI | DDGI 16×8×16 探针，64 条射线/探针，每帧 64（quality=2 为 256）个探针轮转更新 + 时域滞后 | `CSProbe`、`QIrradiance` |
| 体积雾/云 | froxel 80×45×48（quality=2 为 160×90×64），每 froxel 静态光 + top-4 局部光注入，前 2 盏带 RT 可见性；程序云 + 云影 | `CSFogInject`、`CSFogIntegrate`、`CSCloud` |
| 光类型 | 点光、聚光（车灯/路灯）；无面光源/纹理光/IES | `LightDesc.type` |

当前夜城实际是 **16 盏灯**（4 盏静态路灯 + 12 盏车灯），远未压到 64 槽上限。

## 3. 实测：成本随灯数线性增长

固定场景、机位、1600×900、60 帧，仅增加 `kind:"light"` 实体（几何不变，三角形 7896）：

| 灯数（静态+动态） | GPU P50 | GPU P95 | draw calls |
| --- | --- | --- | --- |
| 16（4+12，当前） | 7.09 ms | 13.87 ms | 644 |
| 40（28+12） | 8.75 ms | 15.40 ms | 668 |
| 64（52+12，上限） | 12.16 ms | 18.86 ms | 692 |

- 平均 **≈0.105 ms / 灯**（1600×900，Radeon 890M，quality=1，含每灯一盏灯具网格的绘制成本）。
- 64 是硬上限：65 盏的场景不会报错，但只有前 64 盏进入常量缓冲，**静默截断**。
- 外推：900 盏 ≈ 90–100 ms（同一 iGPU、同一分辨率），而 MegaLights 在 PS5 1080p 上是 5.5 ms/900 盏。
- 数据：`reports/bench/light-scaling.json`，单次运行报告在 `reports/bench/{16,40,64}.json`。

> 注意：`reports/night/evidence.json` 里的 4.29 ms 是加入 DDGI/局部阴影缓存/froxel 之前的验收数据；当前基线更高，绝对值不可直接对比。

## 4. 逐项对比

| 维度 | 当前 | MegaLights | 差距 |
| --- | --- | --- | --- |
| 单帧灯数 | 64（常量缓冲） | 屏幕 900+，场景可到百万级 | **3–4 个数量级** |
| 成本模型 | 每像素 × 每灯，线性 | 固定 spp，成本≈常数 | 结构性 |
| 灯剔除 | 仅距离衰减，无 tile/cluster | GPU light grid，两级剔除 + HZB | 缺一整层 |
| 阴影 | ≤2 盏 RT + 4 盏静态缓存 + 主光 | 每盏灯默认 RT 阴影 | 数量级 |
| 采样 | 全量确定性 | 加权蓄水池 + 可见光列表 + 20% 隐藏灯预算 | 缺算法核心 |
| 降噪 | 无随机光照降噪；只有时域历史 | SVGF 系专用降噪 + TSR | 缺一整个 Pass |
| 光类型 | 点/聚光 | 点/聚光/矩形/纹理/IES/粒子 | 内容表现力 |
| 体积 | froxel + top-4 注入，部分 RT | 与不透明共享采样/射线的随机体积光 | 近似层级不同 |
| GI | DDGI 探针（有，且不差） | Lumen HWRT | 路线不同 |

## 5. 为什么"效果"上仍能看

MegaLights 的强项是**密度**：几百盏小灯、每盏都有阴影、每像素只追固定几根线。夜城目前 16 盏灯，全部在 64 槽内，逐像素全量求值反而"每个灯都算得准"；加上 DXR 局部阴影、DDGI 和 froxel 雾，单帧观感并不输同灯数的 MegaLights 场景。真正露馅的是把灯加到几百盏：成本线性上涨、阴影只覆盖极少数灯、常量缓冲截断。

## 6. 要做到"比肩"需要什么

不是优化现有循环，而是新增一条随机直接光路（可保留现有路径做低端回退）：

1. **P0 光源与网格**：灯改 `StructuredBuffer`（去 64 上限）；froxel/tile light grid，GPU 两级剔除（粗网格→主网格），每 cluster 压缩索引。已有 froxel 基础可复用。
2. **P1 采样**：8×8 tile 可见光列表（时域重投影 + 随机双线性查找），加权蓄水池抽样，隐藏灯 20% 预算；每像素 1 spp 的 `RayQuery` 可见性。现有 TLAS/内联 RayQuery 可直接用。
3. **P2 降噪**：SVGF（矩、方差裁剪、YCoCg、5×5 邻域夹、稀疏空间滤波），STBN 采样序列；与现有 TAA/时域历史打通。
4. **P3 光类型**：矩形/球面面光源采样（2×2 可见性掩码）、纹理光/IES。
5. **P4 体积与半透明**：体积雾与不透明共享射线；半透明前层单独pass。

工作量估计（单人、复用现有 DXR/DDGI/froxel 基础）：P0+P1 约 2–3 个月，P2 约 1.5 个月，P3/P4 再 2 个月；**4–6 个月**能得到 1 spp、数百盏灯的可玩原型。890M 这类核显上现实目标约 800×600 / 1 spp / 8–12 ms；UE 演示级的稳定性需要更强 RT 硬件和内容侧的 BVH 优化。

## 7. 复现

```powershell
# 以当前夜城为底，克隆 light 实体到 16/40/64 盏后运行（脚本已清理，可用 git 历史或
# reports/bench/*.json 中的同机位参数复跑）
build\NativeGPU.exe --scene .screen\town.scene.json --textures assets\textures `
  --smoke --frames 60 --render-size 1600x900 `
  --camera 0,2.4,30,0,2.2,5 --report reports\bench\run.json
```

当前实测环境：AMD Radeon(TM) 890M Graphics，D3D12，quality=1，localShadows 开，DDGI 开，体积雾开。
