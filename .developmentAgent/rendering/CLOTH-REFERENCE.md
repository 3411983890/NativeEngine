# 布料参考实现：合同、用法与证据边界

- 更新：2026-10-24；状态：CPU 参考、GPU 三角布料和宿主真实绘制已实现，编辑器面板未实现。
- 依据：[粒子、共享场、布料与水完整方案](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md) §7、§11 E、§12。
- 开发台账：[SIMULATION-DEVELOPMENT.md](SIMULATION-DEVELOPMENT.md)。
- 2026-10-23：增加 `setWindSamples` 与逐顶点共享风适配器，失败整批保留、传送清理和 digest `/2`；合同及耦合专项见 [共享场参考](FIELD-REFERENCE.md#布料风与水体发布适配器)。
- 2026-10-24：源/cook `/2` 改为真正的三角膜/二面角模型，CPU/GPU 共用离线着色与物理合同，并输出 GPU 法线/切线。新参数、单位、迁移与专项入口见 [CLOTH-GPU.md](CLOTH-GPU.md)。下文 `/1` 示例及三类距离边描述的是保留的旧版 CPU 行为；当前五个作者资产均为 `/2`，不能把两种弯曲参数直接改名互换。
- 2026-10-24 后续：[真实绘制与宿主](CLOTH-RENDERING.md) 直接读取 GPU 三角数据，新增双面基础 sheen/经纬着色、UV/法线视图和 `--cloth` 生命周期控制，仍无蒙皮、编辑器及完整材质资产路径。

本文描述**已经存在**的接口与测试入口。没有在本文和台账中同时出现的布料能力，不得从类型名或文件路径推断为已实现。

## 1. 资产合同

当前源资产 `assets/cloth/*.cloth.json` 使用 `nativegpu.cloth/2`，烘焙产物为 `nativegpu.cloth-cook/2`，仍做 SHA-256 内容寻址。下面的旧版 `nativegpu.cloth/1` 与 cook `/1` 继续受 CPU 支持，供兼容与显式迁移使用。

```json
{
  "schema": "nativegpu.cloth/1",
  "id": "flag_banner",
  "revision": 0,
  "seed": 11,
  "kind": "environment",
  "mesh": { "type": "grid", "columns": 20, "rows": 14, "widthMetres": 1.2, "heightMetres": 0.8 },
  "material": {
    "arealDensityKgM2": 0.22, "thicknessMetres": 0.003,
    "stretchCompliance": 1e-8, "shearCompliance": 2e-7, "bendCompliance": 4e-5,
    "damping": 0.35, "dragCoefficient": 1.1
  },
  "pins": [{ "column": 0, "row": 0 }],
  "collision": { "thicknessMetres": 0.004, "plane": null, "spheres": [], "capsules": [] },
  "lod": [{ "maxDistanceMetres": 8, "columns": 20, "rows": 14 }],
  "budget": { "maxVertices": 4096, "maxSubsteps": 4, "iterations": 8 }
}
```

烘焙期完成、运行时不再推导的内容：

| 烘焙产物 | 规则 |
|---|---|
| `positions` | 网格位于 XY 平面，第 0 行在下边缘，第 0 列在左边缘，+Z 为正面 |
| `triangles` | 每格两个三角形，逆时针朝 +Z |
| `masses` | 由三角形面积/3 累加后乘面密度，单位为 kg；不是平均分配 |
| `stretch` | 水平/垂直相邻边，静止长度 = 格距 |
| `shear` | 两条对角线，静止长度 = √(dx²+dy²) |
| `bend` | 跨一格的水平/垂直边，静止长度 = 2×格距 |
| `pins` | 网格坐标 → 顶点索引 + 锚点 |

单位固定为米、千克、秒。旧版三类距离 `compliance` 是 XPBD 的顺应度（m/N），不是刚度；`0` 表示刚性约束。/2 的 `bendAngleCompliance` 单位为 rad/(N·m)，膜的 stretch/shear 顺应度按三角面积归一化。

## 2. 求解器

`engine/native/cloth/solver_cpu.hpp` 的 `Solver` 是 CPU XPBD 参考与回退后端：

- 固定 tick 驱动；子步数 = `min(budget.maxSubsteps, ceil(dt × 480))`，与渲染帧率无关。
- 每次子步：重力与风加速 → 阻尼 → 预测位置 → `iterations` 次约束投影（拉伸/剪切/弯曲，带 λ 累积）→ 锚点回写 → 碰撞投影 → 由位置差反算速度。
- /1 风按相对速度与顶点面积计算；/2 先从同一状态按三角面积与法向相对风速计算面压力，再均分给顶点。ρ=1.225 kg/m³，加速度上限 4000 m/s²，详见 GPU/CPU 共同模型文档。
- 碰撞支持平面、球、胶囊，保留布料半厚 + 声明间隙；穿透在**提交态**测量，不把迭代中间值当成最终结果。
- `setSelfCollision(true)` 明确抛出 `NG-E-CLOTH-UNSUPPORTED`，不会返回互相穿插的布。
- `setOrigin` 是传送：整体平移并清零速度，避免大位移注入能量。
- `digest()` 对位置/速度做定点量化后哈希，用于 100 次重放对拍。

## 3. 复现与阈值

```powershell
npm run cloth -- cook --in assets/cloth --out build/cloth
npm run cloth -- validate --in assets/cloth/flag_banner.cloth.json
node --test tests/cloth.test.mjs
```

原生专项（构建后）：

```powershell
.\build\native_cloth_tests.exe
```

当前门槛与实测：

| 项目 | 阈值 | 结果 |
|---|---|---|
| 旗帜（1.2×0.8 m）悬挂 5 s 边长相对误差 | P95 ≤ 5% | 通过 |
| 球体接触穿透（提交态） | P99 ≤ 2 mm、最大 ≤ 10 mm | 通过 |
| 锚点漂移 | ≤ 1e-6 m | 通过 |
| 100 次重放 digest | 完全一致 | 通过 |
| 暂停/传送/重置/非法 tick | 行为显式 | 通过 |

## 4. 明确未完成

- GPU 自碰撞、连续碰撞、蒙皮骨骼求值与绑定空间映射；GPU 约束分组求解已实现。
- 渲染网格与模拟网格的三角重心映射、完整纹理材质/局部光/织物着色；双面基础 sheen 与独立 GPU 法线/切线输出已实现。
- 存档、编辑器面板与事务提交；宿主 `--cloth` 预览、暂停/恢复/传送/reset 与只读运行报告已实现，GPU 形变不进入玩法摘要。
- 破损、缝合边、非网格拓扑与导入器；当前只接受烘焙网格。

这些缺项意味着方案 E 阶段**未完成**；上表是 CPU 参考的历史门槛，独立 GPU 实测见新文档和台账，两者均不代表生产布料管线已完成。
