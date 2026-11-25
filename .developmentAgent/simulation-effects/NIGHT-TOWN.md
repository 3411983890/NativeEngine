# 雾夜小镇：全实时光照验收场景

- 版本：v0.3
- 日期：2026-10-21
- 场景：`.screen/town.scene.json`
- 关系：这是 [LIGHTING-SYSTEM-RESEARCH.md](LIGHTING-SYSTEM-RESEARCH.md) 与 [VOLUMETRICS-PLAN.md](VOLUMETRICS-PLAN.md) 的可运行验收切片。

## 1. 场景内容

| 项 | 值 |
| --- | --- |
| 实体 | 608 |
| 材质 | 16 |
| 纹理 | 45 个 `.ngtex`，BC1/BC3/BC5 + RGBA8 混合，**4.49 MiB** |
| 动态灯 | 4 辆车 ×（2 前照灯 spot + 1 尾灯 point）= 12 盏 |
| 静态灯 | 4 盏路灯 |
| 灯上限 | 64（`GpuLight` buffer，点光/聚光） |
| Draw calls | 644 |
| 三角形 | 7,896 |

夜间参数在场景的 `lighting` 块里：

```json
"lighting": {
  "ambient": [0.015, 0.022, 0.045],
  "skyColor": [0.004, 0.008, 0.018],
  "keyDirection": [0.15, 0.38, -0.91],
  "keyColor": [0.40, 0.52, 0.90],
  "keyIntensity": 0.08,
  "fogColor": [0.065, 0.085, 0.125],
  "fogDensity": 0.010,
  "fogHeightFalloff": 0.040,
  "toneMap": true,
  "exposure": 1.45,
  "cloudCoverage": 0.55,
  "cloudHeight": 92.0,
  "cloudThickness": 36.0,
  "cloudSpeed": 0.02
}
```

## 2. 车辆与动态灯

- 车辆是 `kind: "vehicle"` 的实体，`vehicle.speed` 与 `vehicle.direction` 决定沿 Z 轴循环行驶。
- `Game::dynamicLights()` 每帧从车辆位置生成 2 盏前照灯 spot light 和 1 盏尾灯 point light；主循环把它们追加到场景静态灯之后。
- 前照灯的锥角在材质着色器里做 `dot(forward, lightDirection)` 判断，只有车头前方被照亮。
- 车身、车顶/玻璃、四个轮子、前灯/尾灯都是程序化盒体；没有额外网格资产。

## 3. 体积雾与体积云

当前是 **composite 里的屏幕空间体积 pass**，不是 froxel：

- 从深度重建世界坐标与视线方向。
- 雾：12 步高度密度积分，`density = fogDensity * exp(-heightFalloff * (y - baseHeight))`，月光相位近似，输出 `color * T + inscatter`。
- 车灯散射：雾积分内最多注入 8 盏聚光灯（4 辆车的前照灯），按锥角、衰减和相位累加；超过 18 m 截断，避免整屏发白。
- 云：14 步云层 ray march，3D 值噪声 FBM，云层高度 92 m、厚度 36 m；月光/环境照亮云底。
- 夜空：方向梯度 + 星星 + 月亮盘 + 云层。
- 所有参数来自场景 `lighting`，`toneMap` 决定是否做 gamma 编码。

![雾夜街道](images/town-night-street.jpg)

![夜间车流（6 帧 GIF）](images/night-drive.gif)

## 4. 运行

```powershell
build\NativeGPU.exe --scene .screen\town.scene.json --textures assets\textures
```

交互：

- `WASD` 移动，`Shift` 跑，`Space` 跳
- 右键拖动转镜头
- `F8` 重建渲染器（重读纹理）
- `F10` 开关 DXR 阴影
- `F11` 开关 Mesh Shader 路径（cooked 材质会自动回退光栅）

固定机位截图：

```powershell
build\NativeGPU.exe --scene .screen\town.scene.json --textures assets\textures `
  --smoke --frames 60 --render-size 1600x900 `
  --camera 0,2.4,30,0,2.2,5 `
  --screenshot reports/town/acceptance-round2b.png --screenshot-frame 30
```

一键生成 + 证据：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/town.ps1
```

## 5. 证据

| 检查 | 结果 |
| --- | --- |
| 构建 | `build.ps1` 通过 |
| Node 测试 | 122 项通过（含 5 项小镇） |
| 原生测试 | simulation / animation / budget / RHI / scene / renderer math 全过 |
| 庭院图像回归 | meanAbsError=0、changedPercent=0、SSIM=1（legacy 路径未变） |
| 夜景截图 | `reports/night/acceptance.png`、`reports/town/acceptance-night-late.png` |
| 性能 | 1600×900，AMD Radeon 890M：GPU P50 ≈ 4.3 ms、P95 ≈ 10.9 ms |
| 纹理预算 | 45 纹理 / 4.49 MiB / BC1+BC3+BC5+RGBA8 |

`reports/night/evidence.json` 记录实体/材质/纹理/格式混合/预算/截图 hash/GPU 时间；`.incrementalTesting/rendering/night.ps1` 可一键重跑验收。

## 6. 已知限制

1. **分档体积**：`lighting.quality=0` 保留屏幕空间雾；1/2 档已实现 froxel、世界空间遮挡光轴与局部灯散射，小镇默认 1 档。
2. **实时体积云近似**：1/2 档已有三维密度、光向自阴影、Beer 云投影和时域重建；多次散射为衰减 octave 近似，高档提高采样量。
3. **局部灯按预算投影**：key 光走 DXR；1 档最多两盏、2 档最多四盏重点局部灯投表面阴影，其余局部灯仍无表面阴影。体积阴影单独选择。
4. **GI 范围**：1/2 档已有天空 SH/GGX IBL 和一次漫反射反弹 DDGI；探针覆盖相机附近，蒙皮角色尚未进入 TLAS。实现与验收见 [LIGHTING-QUALITY.md](LIGHTING-QUALITY.md)。
5. **车辆是盒体**：没有转向、车轮旋转、刹车灯逻辑；速度/方向是场景参数。
6. **没有视频编码**：验收用固定帧截图 + GIF；MP4 需要额外采集/编码步骤。

## 7. 下一步

- 粒子烟密度接入大气 froxel，避免重叠体积的独立合成误差。
- 扩展静态灯缓存覆盖，并加入更多动态几何的 BLAS 与命中材质读取。
- 探针滚动更新、多级覆盖，以及云采样/重建的进一步质量优化。
- 视频采集；现有新体积与 DDGI pass 已报告 P50/P95 GPU 时间戳。
