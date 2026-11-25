# Mesh Shader 样例与 Bindless 槽位管理

- 版本：0.6.0
- 证据：`reports/latest/mesh-bindless-evidence.json`、`runtime` 截图在 `artifacts/`
- 相关文件：`shaders/mesh_shaders.hlsl`、`renderer/mesh_bindless.cpp`

## 1. Mesh Shader

### 编译与管线

| 阶段 | 入口 | profile | 字节 | 编译方式 |
| --- | --- | --- | --- | --- |
| Amplification | `ASMain` | `as_6_5` | 5552 | 固定 DXC 1.9.2609 |
| Mesh | `MSMain` | `ms_6_5` | 7160 | 同上 |
| Pixel | `PSMain` | `ps_6_5` | 6568 | 同上（与光栅路径同一光照模型） |

管线状态必须用 **stream** 描述（`D3D12_PIPELINE_STATE_STREAM_DESC`），因为旧的
`D3D12_GRAPHICS_PIPELINE_STATE_DESC` 无法描述 AS/MS。子对象序列：
`ROOT_SIGNATURE, AS, MS, PS, BLEND, RASTERIZER, DEPTH_STENCIL, RENDER_TARGET_FORMATS,
DEPTH_STENCIL_FORMAT, SAMPLE_DESC, SAMPLE_MASK, PRIMITIVE_TOPOLOGY, NODE_MASK`。

### 几何

单位立方体按面切成 **6 个 meshlet**（每面 4 顶点 / 2 三角形），顶点与索引放在
`StructuredBuffer` 里，meshlet 描述（顶点/三角形区间、中心、法线）放在第三个 `StructuredBuffer`。
每个实例一次 `DispatchMesh(1,1,1)`：放大着色器为该实例准备 payload，网格着色器按 payload
中的 meshlet 索引发射顶点与三角形；像素着色器输出 `SV_TARGET0`（环境光 + 点光）与
`SV_TARGET1`（key light），与光栅路径一致，因此阴影合成路径不变。

### 回退

`F11` 或 `--mesh-shaders on/off`。关闭时使用传统顶点路径（`IASetVertexBuffer` +
`DrawIndexedInstanced`），报告字段 `meshShader.fallbackUsed=true`、`frames=0`。
能力查询（`meshShaderTier`）、编译日志（`build/shaders/manifest.json`）、运行截图
（`artifacts/mesh-on.png` / `mesh-off.png`）与回退路径四者都有。

### 已知差异（必须如实记录）

- **放大着色器的背面剔除当前禁用**：payload 需要组内所有线程完全一致，逐线程构造 payload 会让
  `DispatchMesh` 的组数不一致，运行时直接不渲染。现在改为从 groupshared 数组构造一致 payload
  并转发全部 6 个 meshlet，报告 `amplificationCulling=false`、`meshletsCulled=0`。
  这是**成本优化缺失**，不是功能缺失：AS 仍参与并逐实例派发。
- **Mesh 路径整体比顶点路径暗**（同机位平均亮度约 29 vs 53）：几何与材质正确，但 key light
  项在 mesh 路径上偏弱，法线/绕序一致性仍未对齐。报告用 `imageDiff` 记录差异，不假装“同画质”。
  在修正之前，`implemented=true` 只表示“AS+MS+PS 管线跑通并可回退”，不表示“与光栅逐像素一致”。

### 帧时（1280x720，同机位，关闭 DXR 阴影）

| 路径 | GPU P95 | CPU 提交 P95 |
| --- | --- | --- |
| Mesh Shader | 见 `timing.meshGpuP95Ms` | 见 `timing.meshCpuSubmitP95Ms` |
| 顶点路径 | 见 `timing.vertexGpuP95Ms` | 见 `timing.vertexCpuSubmitP95Ms` |

数值随机器/驱动变化，以证据 JSON 为准，不在文档里写死。

## 2. Bindless 槽位管理

模式：**`bounded-table`**（有界表 + 代际），不是 SM 6.6 `ResourceDescriptorHeap` 直接索引。

| 特性 | 实现 |
| --- | --- |
| 槽位数 | 8（0 = 缺材质默认槽，1..3 = 常驻 checker/brick/metal，4..7 可分配） |
| 槽位代际 | 每槽 `generation`，release 与复用都会自增 |
| 缺纹理默认槽 | 槽 0 指向独立的品红棋盘纹理，缺失/失效时可见 |
| 槽位复用与释放 | `acquire` 优先找空闲槽，满时复用最低代际槽 |
| 在途帧更新 | 代际校验在 CPU 端解析，旧代际引用被拒绝并回退默认槽 |
| 诊断 | `NG-E-MAT-MISSING`（缺纹理）、`NG-E-MAT-STALE`（越界/已释放/代际过期）、`NG-E-MAT-TABLE`（表自检失败） |

`DrawItem.texture` 现在是**槽位索引**（宿主把场景材质 0/1/2 映射到槽 1/2/3），
`textureGeneration` 为 0 时跳过代际校验。

### SM 6.6 评估

本机 `shaderModel=6.8`、`resourceBindingTier=3`，技术上具备 `ResourceDescriptorHeap[]`
直接索引条件；本版本**未实现**，原因是需要额外的 `ps_6_6` 管线与无界描述符范围，
收益（槽位上限）与当前 8 槽需求不匹配。报告字段：
`bindless.sm66HeapIndexing = {supported: true, implemented: false, reason: ...}`，
`bindless.mode = "bounded-table"`。要升级时按此顺序：无界范围 → ps_6_6 变体 → 运行时能力探测 → 回退。

### 自检

初始化时执行一次表自检：分配两个槽 → 代际校验 → 旧代际应被拒绝 → 释放 → 释放后应失效 →
再分配应复用同一槽且代际自增。任何一步失败都会写入 `NG-E-MAT-TABLE` 诊断，
证据 JSON 的 `bindless.acquired/released/reused/missingFallbacks/staleRejections` 记录结果。
