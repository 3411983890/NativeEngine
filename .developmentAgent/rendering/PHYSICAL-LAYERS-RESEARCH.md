# 物理层叠参考闭包研究（L7）

- 日期：2026-10-27。
- 状态：研究阶段产出；**未接入运行时**。运行时仍使用第一阶段参数混合。
- 关联：[多层材质与物理层叠开发方案](LAYERED-MATERIAL-DEVELOPMENT-PLAN.md) 第 8 节。

## 1. 结论

1. 第一阶段（参数混合）用一个 PBR 参数集表达所有层，无法同时保留两个反射峰；这是方法本身的近似，不是实现缺陷。
2. 参考闭包「不透明基底＋非吸收清漆」可以用一个 Fresnel 权重把覆盖（coverage）与透射（transmission）分离：
   - 反射份额 `F = fresnel(f0, cosθ_view) * coverage`
   - 透射份额 `(1 - F) * transmission`
   - 清漆高光 `D(coatRoughness, cosθ_half) * F`
   - 基底漫反射/高光按透射份额衰减
3. 该闭包与现有 Clearcoat 的语义可直接对应（`f0 = 0.04`，`clearcoat` 标量即 coverage，基底乘以 `1 - F`），可作为后续运行时扩展的兼容基线。
4. 本阶段只到参考实现与契约编译；是否进入运行时需要单独的能力开关、预算与画面对照，不在本次范围内承诺。

## 2. 语义边界

| 概念 | 本参考闭包 | 说明 |
| --- | --- | --- |
| 覆盖 coverage | 清漆层是否存在及其比例 | 0 时完全返回基底 |
| 透射 transmission | 基底光穿过清漆的比例 | 与覆盖独立，允许 0（完全不透）而仍有反射 |
| 吸收/厚度 | 不支持 | 仅非吸收薄层；带颜色吸收或厚度是后续工作 |
| 多次散射 | 不支持 | 单次 Fresnel 分配能量 |
| 阴影/环境光 | 未定义 | 进入运行时前需分别定义直接光、阴影与 IBL 的处理 |

能量约束：对非吸收层，`reflectedShare + transmittedShare <= 1`（相等时 transmission = 1）。

## 3. 参考实现

- CPU：`engine/material/physical-layers.mjs`
  - `fresnelSchlick(f0, cosθ)`
  - `specularLobe(cosHalf, roughness)`：与第一阶段共享形状，便于比较
  - `evaluateCoatClosure({base, coat, viewDirection, lightDirection, coverage, transmission})`
  - `evaluateParameterMix(...)`：第一阶段对照
  - `physicalLayerIdentity()`：版本化身份，`nativegpu.physical-layers/1`
- GPU 契约：`shaders/physical_layer_blend.hlsli`（`PhysicalCoatClosure`），只做编译验证，未被生产着色器引用。
- 契约测试：`.incrementalTesting/rendering/physical-shader-contract.hlsl`，以 SM 6.6 compute 编译。

## 4. 与第一阶段的差异（可用数值复现）

固定基底 `roughness = 0.9`、清漆 `roughness = 0.05`、`f0 = 0.04`、正视角：

- 参数混合在 coverage = 0.5 时得到单一 `roughness = 0.475` 的 GGX 峰；
- 闭包在峰值方向同时给出清漆峰与基底峰。参考实现中清漆峰值约是混合峰的 4 倍，且半高宽不同（峰值受 1e-8 数值钳制，比值有界），说明二者不是同一种视觉模型。
- 该差异由 `coat closure keeps two reflection peaks that parameter mixing cannot express` 测试锁定，避免把第一阶段结果误标为真实层叠。

## 5. 与现有 Clearcoat 的兼容

现有像素着色器（`shaders/material.hlsl`）的清漆是独立 lobe：

- `coatFresnel = clearcoat * (0.04 + 0.96 * pow(1 - |N·V|, 5))`
- 基底与发光乘 `1 - coatFresnel`
- 环境清漆高光按 `clearcoatRoughness` 单独求值

参考闭包在 `coverage = clearcoat`、`transmission = 1`、`f0 = 0.04` 时给出相同的能量分配，区别在于本闭包把「覆盖」与「透射」显式分开，为后续带吸收薄层留出字段。两者可以在同一场景中对照，不需要迁移旧资产。

## 6. 风险与未决设计

| 风险 | 处理 |
| --- | --- |
| 被误认为已支持物理层叠 | 文档、UI 与报告必须区分「参数混合」和「闭包参考」；运行时没有开关前不对外显示为可用 |
| 直接光/阴影/IBL 的层叠语义不完整 | 扩展前分别定义；本阶段只定义能量分配 |
| 性能与层数预算 | 进入运行时前先做 1–2 层实测；本阶段不给帧率承诺 |
| 透明排序与透射混淆 | 整体 alpha、层覆盖与物理透射字段分离，沿用第一阶段约定 |

### 6.1 性能证据（参考实现的算术量级）

- 每次闭包求值比第一阶段多：1 次 Schlick Fresnel、1 次 GGX 形状函数、2 次乘法，无额外纹理采样。
- CPU 参考（JS，含输入校验）约 0.85 µs/次（1,000,000 次约 853 ms）。该数字只说明算术量级，不能当作 GPU 预算。
- 运行时未接入，因此**没有 GPU 时间**。扩展前必须在目标硬件上使用与计划文档 7.4 节相同的场景、分辨率与帧数测量，并记录与第一阶段的差值；在此之前维持「研究结论、不进入运行时」的决定。

## 7. 证据

- `.incrementalTesting/rendering/layered-material.test.mjs`
  - `physical coat closure separates coverage, transmission and Fresnel energy`
  - `coat closure keeps two reflection peaks that parameter mixing cannot express`
  - `physical coat shader contract compiles as an SM 6.6 compute shader`

## 8. 下一步（需单独决定）

1. 在渲染器中加入显式能力开关，只在支持路径启用 `physical-layers/1`。
2. 增加 1–2 层闭包的 CPU/GPU 数值对照与画面截图。
3. 定义直接光、阴影、IBL 与层叠的结合方式，并给出性能实测与降级策略。
