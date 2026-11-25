# DXR 静态阴影 pass

- 版本：0.4.0 起
- 状态：**已实现、可切换、可回退、有截图与时序证据**
- 证据入口：`reports/latest/rt-shadows-evidence.json` 与 `docs/STATUS.md`

## 1. 效果定义（不要超出这个范围）

- **一条光线 / 像素**：从光栅深度重建世界坐标，向固定 key light 方向发射一条遮挡光线。
- **硬阴影**：可见性只有 0/1，没有半影、软阴影、降噪或时间累积。
- **静态几何**：加速结构由刚体方盒实例组成，没有蒙皮、顶点动画或曲面细分。
- **TLAS 每帧重建**：`tlasPolicy = "per-frame-rigid"`，因为场景里的方盒会移动；几何本身仍然是刚体。

## 2. 渲染管线

```text
光栅 pass  ──► SV_TARGET0 sceneBaseColor : ambient + 场景点光
           ──► SV_TARGET1 sceneKeyLight  : albedo * max(dot(n, L), 0) * 0.95
           ──► 深度缓冲 depth            : R32_TYPELESS / DSV D32_FLOAT / SRV R32_FLOAT

DXR pass   ──► DispatchRays（深度 SRV + TLAS + 可见性 UAV）
           ──► shadowMask : R16_FLOAT，0 = 被遮挡，1 = 受光

合成 pass  ──► composite = saturate(sceneBaseColor + sceneKeyLight * shadowMask)
           ──► CopyResource 到交换链后备缓冲；截图也从 composite 复制
```

这样做的好处是**回退与开关不需要第二条渲染路径**：关闭阴影时把 `shadowMask` 清成 1，
合成结果与 0.3.0 的纯光栅图像逐像素一致；`--rt-shadows off` 就是这样实现的。

Key light 从 0.45 提到 0.95 是一次**刻意的观感调整**：0.45 时 key light 只占总光照约 30%，
阴影几乎看不出来；0.95 时受光面与阴影面的差异足以在固定机位截图中辨认。环境光同时从
(0.34, 0.36, 0.40) 保持原值，因此回退路径与开启路径的差异只来自阴影本身。

## 3. 资源与状态

| 资源 | 格式 | 数量 | 说明 |
| --- | --- | --- | --- |
| sceneBaseColor | R16G16B16A16_FLOAT | 3 | 环境光 + 点光，每帧一帧缓冲 |
| sceneKeyLight | R16G16B16A16_FLOAT | 3 | 仅 key light 项 |
| shadowMask | R16_FLOAT | 3 | UAV 写入，SRV 读取 |
| composite | R8G8B8A8_UNORM | 3 | 最终合成，截图来源 |
| depth | R32_TYPELESS | 3 | DSV 为 D32_FLOAT，SRV 为 R32_FLOAT |
| BLAS | — | 1 | 单位方盒，24 顶点 / 36 索引 |
| TLAS + scratch + 实例缓冲 | — | 3 组 | 每帧索引独立，避免跨帧竞争 |
| Shader Binding Table | UPLOAD | 1 | 192 字节，3 条 64 字节对齐记录 |

RTPSO 子对象：DXIL library（`lib_6_5`）、hit group `ShadowHitGroup`、shader config
（payload 4 字节、attribute 8 字节）、pipeline config（递归深度 1）、global root signature。
曝光名：`ShadowRayGen`、`ShadowMiss`、`ShadowClosestHit`。

## 4. 报告字段

`probes.raytracingShadows`：

| 字段 | 含义 |
| --- | --- |
| `probeAvailable` | 适配器 DXR tier ≥ 1.0（**能力**，不代表实现） |
| `dxcAvailable` / `dxcVersion` | 固定 DXC 是否可用及其版本 |
| `requested` | 本次运行是否请求开启 |
| `enabled` | 请求开启且 RTPSO + SBT 都创建成功 |
| `rtpsoCreated` / `sbtCreated` | 状态对象与 SBT 的真实创建结果 |
| `effectImplemented` | RTPSO + SBT + **至少一次成功 DispatchRays** |
| `fallbackUsed` | 最近一帧是否走了光栅回退（未派发） |
| `shadowTextureBytes` | 3 帧 R16_FLOAT 可见性贴图总字节 |
| `dispatchedFrames` / `fallbackFrames` / `dispatchFailures` | 计数 |
| `gpuRtMs` | 仅 DispatchRays 窗口的 GPU 时间（时间戳可分离时） |
| `tlasInstances` / `tlasRebuilds` | 实例数与 TLAS 重建次数 |
| `error` | 失败原因文本（DSO 创建、SBT、矩阵奇异等） |

`capabilities.implemented.raytracing` 只有在 `effectImplemented = true` 时才为 true。
关闭阴影的运行里它是 **false**，同时 `fallbackUsed = true` —— 这是有意的：
同一条命令的不同开关状态必须分别如实报告。

## 5. 操作方式

```powershell
# 交互：F10 切换，状态显示在右侧面板「DXR 阴影 开启 / 关闭 · 回退」
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1

# 命令行
build\NativeGPU.exe --rt-shadows off                     # 纯光栅回退
build\NativeGPU.exe --rt-shadows on  --camera 9,5.5,10,-2,0.8,0

# 一键证据（开/关/mask/key + 对比 + 时序 + 断言）
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/rt-shadows.ps1
```

`--camera x,y,z,tx,ty,tz` 固定机位；`--render-size 1280x720` 固定分辨率；
`--debug-view mask|key|base|solid` 输出诊断视图；`--screenshot` + `--screenshot-frame`
在指定帧内复制 composite 到 READBACK 缓冲并写 PNG。

## 6. 本机证据（AMD Radeon 890M，DXC 1.9.2609，1280x720，33 个实例）

`.incrementalTesting/rendering/rt-shadows.ps1` 的 23 项检查全部通过，关键数字：

| 指标 | 开启 | 关闭 | 说明 |
| --- | --- | --- | --- |
| GPU P95（整条命令列表） | 0.717 ms | 0.467 ms | +0.250 ms，约 +53% |
| DispatchRays 窗口 | 0.105 ms | — | 时间戳可分离 |
| CPU P95 | 17.20 ms | 17.28 ms | 含 `Present(1,0)` 垂直同步等待，不代表 CPU 成本 |
| 变化像素 | 3.30 % | — | 受光且被遮挡的像素；最大通道差 394 |
| 可见性贴图 | 5,529,600 B | — | 3 × 1280 × 720 × 2 |

截图（`reports/latest/rt-shadows-evidence.json` 的 `images` 字段指向同一目录的 `artifacts/`）：

- `shadow-on.png` / `shadow-off.png`：同一机位、同一帧号，可直接对比。
- `shadow-mask.png`：白 = 受光、黑 = 被遮挡；能看清墙体、方盒与角色的投影。
- `shadow-key.png`：仅 key light 项，阴影边界最清晰。

## 7. 验收对照

| 要求 | 结果 |
| --- | --- |
| 用固定 DXC 编译 ray tracing library | `shaders/rt_shadows.hlsl` → `lib_6_5`，8456 字节，DXC 1.9.2609 |
| 创建 RTPSO 与 SBT | `rtpsoCreated=true`、`sbtCreated=true`、`sbtBytes=192` |
| 使用现有 BLAS/TLAS 输出阴影纹理 | 单位方盒 BLAS + 33 实例 TLAS，`shadowTextureBytes=5529600` |
| 光栅 pass 后合成，保留纯光栅回退 | `base + key * mask`，关闭时 mask=1，逐像素一致 |
| 运行时开关 | `F10` 与 `--rt-shadows on/off`，报告区分 `enabled` |
| 固定机位截图证明阴影存在 | `shadow-mask.png` 与 `shadow-on/off.png` 对比 |
| 报告区分能力/实现/回退 | `probeAvailable` / `effectImplemented` / `fallbackUsed` 独立字段 |
| 与光栅基线帧时对比 | 上表 +0.250 ms GPU P95 |
| 设备不支持时显示 unsupported | `probeAvailable=false`，`fallbackUsed=true`，不创建 RTPSO |
| 编译失败回退且保留错误文本 | `dxcUnavailable` 或 `error` 字段带编译器诊断，光栅路径继续运行 |

## 8. 已知限制与陷阱

- 阴影只有 0/1，没有软阴影、降噪、透明阴影或二次反射；不在本阶段目标内。
- 只有一盏固定方向的 key light 产生阴影，场景点光不投影。
- 世界坐标由深度重建，几何法线取自相邻像素；极端斜面轮廓可能有 1 像素级锯齿。
- 两个 D3D12 陷阱（本次踩到并已修复，后续不要再犯）：
  1. 渲染目标资源必须以 `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` 创建并以
     `D3D12_RESOURCE_STATE_RENDER_TARGET` 初始化，否则 `CreateCommittedResource` 返回 `E_INVALIDARG`。
  2. `ClearRenderTargetView` 之后**必须** `SetPipelineState`，否则会沿用命令列表重置时绑定的
     光栅 PSO，绘制被静默丢弃，只留下清屏颜色。
- 截图必须在**帧内**记录 `CopyTextureRegion`（composite 已是 COPY_SOURCE），在帧栅栏之后映射读取；
  用独立命令列表另开一次提交读不到该帧的写入。
