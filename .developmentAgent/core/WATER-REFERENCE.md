# 水体参考实现：合同、查询与证据边界

- 日期：2026-10-22；状态：解析/频谱/浅水三种 CPU 后端已实现，局部 PBF、GPU 求解与水下渲染未实现。
- 依据：[完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) §8、§11 F、§12。
- 开发台账：[SIMULATION-DEVELOPMENT.md](SIMULATION-DEVELOPMENT.md)。
- 2026-10-23：增加高度/深度/XZ 流速的四通道原子 CPU 权威投影，保留 Body/source 身份；合同及耦合专项见 [共享场参考](FIELD-REFERENCE.md#布料风与水体发布适配器)。这是查询代理，尚无水量转移、GPU 水或宿主接入。
- 2026-10-27：修复超 CFL 积分。子步超过 `maxSubsteps` 时不再以过大的 `h` 强行推进整段权威时间，而是推进预算内可稳定的部分并记录时间欠账；非有限单元清零改为累计诊断。CPU 专项新增超预算大步长与剧烈冲量用例。

## 1. 资产合同

源资产 `assets/water/*.water.json`，schema `nativegpu.water/1`；烘焙产物 `nativegpu.water-cook/1`，由 `tools/water.mjs` 内容寻址。

```json
{
  "schema": "nativegpu.water/1",
  "id": "river_shallow",
  "backend": "shallow",
  "extentMetres": [24, 8],
  "cellMetres": 0.1,
  "stillHeightMetres": 0.35,
  "bathymetry": { "type": "slope", "heightMetres": 0, "slopeX": 0.004, "slopeZ": 0 },
  "boundaries": { "type": "open", "inflowCubicMetresPerSecond": 0.6, "outflowCubicMetresPerSecond": 0.6 },
  "waves": { "amplitudeMetres": 0, "wavelengthMetres": 1, "speedMetresPerSecond": 0, "direction": [1, 0], "components": 1 },
  "material": { "refractionIndex": 1.333, "absorptionPerMetre": 0.6, "foamThreshold": 0.4 },
  "budget": { "maxCells": 262144, "maxSubsteps": 8 }
}
```

- 网格尺寸由 `floor(extent/cell)` 派生并写入 cook 的 `derived`，运行时不重新取整；`columns×rows` 必须 ≤ `budget.maxCells`。
- 底床支持 `flat`、`slope`、`basin` 三种解析形式。
- 边界 `closed` 为反射墙；`open` 为零梯度透射边界。入流/出流以 m³/s 声明并进入质量账本。

## 2. 后端

| 后端 | 状态 | 说明 |
|---|---|---|
| `analytic` | 已实现 | 多分量 Gerstner 叠加，查询高度/法线；不做体积守恒 |
| `shallow` | 已实现 | 守恒有限体积 + 静水重构 + Rusanov 通量，CPU 权威参考 |
| `spectrum` | 已实现（非 FFT） | 确定性谱分量求和，`fft=false`；不是 GPU FFT 海面 |
| `pbf` | 明确拒绝 | 资产编译返回 `NG-E-WATER-UNSUPPORTED`，原生加载同样拒绝 |

不要把 `spectrum` 当作频谱海面完成证据：它没有 FFT、没有非重复采样、没有近岸叠加。

## 3. 数值合同

- 时间推进：CFL 条件 `dt ≤ 0.4·dx/max(|u|+√(gh))`。实际子步长 `h` 永不超过该界：所需子步超过 `maxSubsteps` 时只推进预算内可稳定覆盖的时间，其余记为 `timeDebtSeconds`，后续 tick 在有容量时偿还（每 tick 最多补一个 `dt`）；欠账超过 0.25 s 的部分丢弃并累计到 `droppedSeconds`。`cflClamped` 表示该 tick 未推进完整请求时间，`lastCflNumber=h/CFL界` 应恒 ≤ 1。权威时间因此可能暂时落后于墙钟，但不会用过大的子步伪造推进。
- 数值故障诊断：非有限单元，或速度使 CFL 子步低于可解析下限 `1e-6 s` 的单元，其动量被清零并累计 `sanitizedCells`；`nonFinite` 只报告诊断时刻仍非有限的单元。清零是运行时保护，不是稳定性证据。`timeDebtSeconds`、`droppedSeconds`、`lastCflNumber` 一起构成“预算不足”的显式账本。
- 封闭墙的通量：质量通量为零，但**必须保留静水压力项** `0.5·g·h²`；否则边界压力不平衡会把水池整体泵起（本实现早期即因此失败，已修正并有回归）。
- 静水保持：平坦底床与常数水深时，一步更新后水深逐位不变。
- 质量账本：`volume`、`initial`、`added`、`removed` 分开记录，`massErrorFraction` 只由这四项计算。
- 干湿：水深低于 `1e-5 m` 时清零并清除动量，不产生负水深。
- 查询返回 `valid`、`surfaceHeight`、`depth`、`velocity`、`normal`、`tick`、`errorBoundMetres`；域外与 NaN 一律 `valid=false`，绝不返回无限水深。
- 浮力查询按箱体足迹采样，返回排水体积、排水质量、浮心与误差界；它报告几何排水量，不替调用方决定质量。

## 4. 复现与阈值

```powershell
npm run water -- cook --in assets/water --out build/water
npm run water -- validate --in assets/water/river_shallow.water.json
node --test tests/water.test.mjs
npm run test:water          # CPU 交互、表面重放与 CFL 预算专项
.\build\native_water_tests.exe
```

| 项目 | 阈值 | 结果 |
|---|---|---|
| 封闭 10 m 浅水池 60 s 质量误差 | ≤ 1% | 通过 |
| 静水 60 s 水面漂移 | ≤ 1 mm | 通过（实测 0） |
| 水深非负、无 NaN | 必须 | 通过 |
| 规定箱体静水排水体积误差 | ≤ 5% | 通过 |
| 100 次重放 digest | 完全一致 | 通过 |
| 域外/NaN 查询 | 显式 invalid | 通过 |
| 0.25 s 超预算大步长 | 子步 h ≤ CFL 界、欠账可偿还、质量误差 ≤ 0.1% | 通过（实测欠账 0.048 s） |
| 9×10⁴ N·s 剧烈冲量 5 s | `lastCflNumber` ≤ 1、无 NaN、质量误差 < 5%、重放一致 | 通过（实测丢弃 0.175 s，无 NaN） |

## 5. 明确未完成

- 局部三维液体（PBF）求解、表面重建、与浅水的质量/动量交换。
- GPU 守恒浅水后端、FFT 海面、GPU 尾流与浮力/阻力耦合。
- 局部水花/泡沫事件、水下相机与水线、岸线湿润；当前泡沫只是表面速度阈值映射。
- 浮力查询尚未接入刚体/角色与双向质量交换；布料湿润、火熄灭等场联动未接线。
- 多水域 WaterBody 组件、重叠选择规则、编辑预览/保存/撤销；当前仍是单场景顶层 `water` 块。

方案 F 阶段因此**未完成**；已交付的是资产合同、三种 CPU 查询/权威后端与守恒/浮力数值证据。
