# 植被编辑器（Vegetation Forge）

- 日期：2026-10-26（第六批：原生内容协议、修订号/ACK、撤销/重做、草走结构化网格器）。
- 定位：把 `engine/vegetation/species.mjs` 的参数化生成能力做成可交互工具。**视口里看到的就是烘焙出来的东西**——预览与 cook 走同一个模块，不存在第二套生成器。
- 入口：`npm run editor:web` 后打开 `/vegetation`（场景编辑器仍是 `/`）。两份工作台共用同一个本地服务与令牌，动作按 `vegetation.` 前缀分发。

## 1. 为什么预览绕服务一圈

浏览器里没有生成器：`species.mjs` 依赖 `canonical.mjs`（`node:crypto`）与二进制打包，直接搬到前端等于fork一份几何代码。所以每次参数改动都是一次 `vegetation.preview` 往返，服务用**同一个** `buildMesh` 生成网格，返回 base64 的顶点/索引。

代价是每次预览多几十毫秒和一次传输；换来的是“视口与烘焙一致”这条性质不需要靠测试维持——它由构造保证。

## 2. 生成器图（新增数据层）

`engine/vegetation/generators.mjs` 定义 `spec.generators`：

```json
{
  "schema": "nativegpu.vegetation-generators/1",
  "levels": [
    { "id": "trunk", "type": "trunk", "level": 0, "startHeightRatio": 0.42, "steps": 6, "lean": 0.05, "radiusRatio": 0.86, "wind": 0.08 },
    { "id": "branch_1", "type": "branch", "level": 1, "count": 3, "spread": [0.48, 0.9], "lengthRatio": [0.58, 0.72], "radiusRatio": 0.62, "wind": 0.32 }
  ],
  "fronds": [
    { "id": "frond_1", "type": "frond", "parentLevel": 4, "cardsPerSegment": 64, "cardWidth": 0.17, "cardHeight": 0.23, "droop": [0.01, 0.09], "wind": 0.95 }
  ]
}
```

- **可选字段**。没有 `generators` 的物种仍走原来的配方，字节不变——既有包、既有指纹、既有原生测试都不受影响。
- 每个分支层级带自己的风权。段顶点在父级风权与本级目标之间插值，落在 `WIND_VERTEX` 段里；这正是编辑器“骨骼风”页签在调的东西，不是贴图。
- `fronds[].parentLevel` 决定哪一级挂叶。默认取倒数第二级：叶片只挂最末端枝条会得到一片盖在裸骨架上的平顶（见 `docs/VEGETATION-HAIR-STATUS.md` §2.3）。
- 归一化时把 `count` 夹到 1–16、`spread` 夹到 0–1.5、层级夹到 0–8，并拒绝没有 trunk 的图。越界不是靠 UI 拦，数据层自己拒绝。

## 3. 服务动作

| 动作 | 作用 |
| --- | --- |
| `vegetation.list` | 列出物种（`build/vegetation/source` 夹具 + `assets/vegetation/species` 已编辑），附烘焙三角数 |
| `vegetation.load` | 读源文件，补齐完整生成器图后返回 |
| `vegetation.preview` | 单个 LOD 的几何 + 统计；`analysis:true` 额外构建全部 LOD 测误差 |
| `vegetation.save` | 校验后写入 `assets/vegetation/species/<id>.species.json` |
| `vegetation.cook` | `compileSpecies` → `<hash>.ngveg` + 合并进 `build/vegetation/cooked/library.json` |
| `vegetation.publish` | `compileRegion` → `cooked/regions/<hash>.ngvrg` + 合并进同一 `library.json` 的 `regions`，并把区域源写回 `assets/vegetation/regions/<id>.region.json` |
| `vegetation.impostor` | 图集声明信息（图集本身在 bake 时生成） |
| `vegetation.defaults` | 按物种档案生成默认图，供「重置生成器」使用 |

烘焙会**合并**进内容库：只替换同 id 条目，不删其他物种。

`vegetation.publish` 是“已烘焙物种”到“有场景”之间那一步：在此之前 `library.json` 里只有包，没有任何 `.ngvrg` 页集可加载。它只替换同 id 的区域条目、不动物种条目；引用前会逐包重算 sha256 并重解析，所以不会发布一个指向已变字节的区域。`expectedRevision` 语义与保存物种一致——回传上一次发布得到的 `hash`，不匹配即 `NG-E-VEG-CONFLICT`。区域源最后写入：包已经与清单一致时重发是幂等的，而“有源无包”会让区域看起来已编写却加载不了。

## 4. 界面覆盖

- 顶栏：产品名、重新载入、重置生成器、视口工具、风动开关、保存源文件、烘焙物种、发布场景。
- 左栏：生成器层级（Trunk / Branch N / Frond N，含段数与卡片数）、物种库（已编辑 vs 构建夹具）。
- 视口：WebGL，轨道拖拽/滚轮缩放/F 适配；实体、线框、**风权着色**三种模式；风动实时摆动（用烘焙进顶点的风权，不是假动画）。
- 右栏：生成（分支数/展开角/长度比/半径比/起始高度/分段/倾斜）、曲线（同参数按曲线语义分组）、叶片（挂载层级/卡片数/宽高/下垂）、风（全局振幅频率 + 每级风权）、LOD（级别数/屏幕误差/预算）。
- 底部：LOD 阶梯（可点选切换视口级别）+ 统计（顶点/三角/传输量/预算占用条/烘焙状态）。
- 状态栏与烘焙结果：真实哈希、字节数、耗时。

## 5. 原生视口协议（第六批新增）

原生视口不再是“固定画 oak 的一个窗口”。宿主进程通过命名管道接收结构化命令，并在每次应用后回执：

| 方向 | 一行一条消息 |
| --- | --- |
| 页面 → main → 宿主 | `req=<id>;cmd=load;rev=<n>;species=<id>;lod=<n>;digest=<sha256>` |
| | `req;cmd=view;debug=<name>;wind=<0..2>;shadow=<0..1>` |
| | `req;cmd=capture;path=<png>` / `cmd=status` |
| | `x=..;y=..;w=..;h=..;v=0/1`（矩形，仍沿用） |
| 宿主 → main → 页面 | `ack=1;req;want;rev;state=ok\|error;species;lod;digest;tri;debug;wind;frames;err=` |

要点：

- **`rev` 是"已经画出来的修订号"**，`want` 是页面请求的。两者不一致时页面显示“待应用”，不显示“已更新”。
- **`digest` 由宿主自己算**（cooked 包的 sha256）。页面传来不匹配的 digest 时宿主拒绝应用，`state=error` 且 `rev` 不前进——旧包配新修订号这条路被堵住。
- **命令队列**：两帧之间写入的多条命令逐条应用、逐条回执。此前的“只保留最新一条”会让一个请求永远收不到 ACK，页面读起来像卡死。
- **宿主崩溃可恢复**：子进程退出后 500 ms 重启，恢复矩形并重放最后一条内容命令。
- 页面侧能力只有三个：`apply(rect)`、`command(cmd)`、`onAck(cb)`（`editor/preload.cjs`）。页面不能自己拼管道行；`main.mjs` 从封闭的键集合构造命令行并限制长度。
- 调试通道下拉框的 15 个名字与 `renderer/vegetation_draw.hpp` 的 `VegetationDebugView` 一一对应，宿主对未知名字回 `state=error` 而不是静默保持上一帧。

端到端证据：`node .incrementalTesting/vegetation/vegetation-sync-check.mjs` 会真的拉起宿主进程、走真实管道，跑 A→B→C→D→A 切物种、错误 digest、5 个调试通道和 latest-wins，结果写 `reports/vegetation-production/editor-sync.json`。

## 6. 修订号、冲突与撤销（第六批新增）

- **修订号是内容哈希**，不是计数器：同一份 spec 存两次得到同一个 revision。`vegetation.load` 返回它，`vegetation.save` 要求 `expectedRevision`。
- **冲突**：磁盘上的内容与 `expectedRevision` 不一致时返回 `REVISION_CONFLICT`（details 带 `currentRevision`），页面会重新载入磁盘版本而不是覆盖。
- **原子写**：写 `<file>.<pid>.<time>.tmp` 再 rename；失败清理临时文件。测试断言目录里没有 `.tmp` 残留。
- **撤销/重做**：保存前把旧字节压入撤销栈；撤销写回原字节（首次保存的撤销是删除 authored 文件，回到夹具状态）。快捷键 `Ctrl+Z` / `Ctrl+Shift+Z` / `Ctrl+Y`。栈是进程内的，重启后不可撤销——这是真实限制，写在 §7。
- **`textureRoles`**：`vegetation.load` 现在返回 cooked 纹理库的真实角色表（digest / semantic / colorSpace / channels / 分辨率 / mip / filter / coverageCutoff），不再是 `library ? null : null`。
- **构建缓存**：`vegetation.cook` 的指纹由三部分组成——源文档的内容哈希、cooked 贴图库的 `packageHash`、cook 代码本身的哈希（`species.mjs`/`branch-graph.mjs`/`branch-mesh.mjs`/`leaf-placement.mjs`/`atlas.mjs`/`material-contract.mjs`/`generators.mjs`/`texture.mjs` 的 sha256）。指纹一致且包文件存在时返回 `cached: true`，不重写字节；`force: true` 强制重烘。指纹写进 `library.json` 条目，因此“改了 cook 代码却复用旧包”不会发生。

## 7. 已知边界（不是待办清单）

1. **图集是替身**（仅 WebGL 视口）：视口用程序生成的叶片图集，网格的 UV 单元与真实图集一致（cook 把单元烘进顶点 UV），但像素不是 `assets/vegetation/textures` 里那张。烘焙用的是真图集。
2. **针叶图集是程序化生成的**：`assets/vegetation/textures/generated/needle_atlas.png` 由 `tools/vegetation-needle-atlas.mjs` 绘制（4×4 格、baseColor 与 opacity 同源 alpha），角色里的 `encoding` 字段写明“不是实拍资产”。换真实针叶图集只需替换 PNG 并重 cook。
3. **撤销栈只在进程内**：重启编辑器后不能撤销上一次会话的保存。
4. **无笔刷/覆盖层**：计划 §5.2 的稀疏覆盖编辑（砍伐、局部删除、手工移动）没有做，编辑器只编辑物种本身。
5. **无 impostor 视口对照**：烘焙产出图集（8 视角 × 128²），但编辑器不做 impostor 与 mesh 的 A/B 对照。
6. **预览为单 LOD**：同一时刻只上传一个 LOD 的几何，切换是重传，不做多 LOD 同时驻留的对比。
7. **LOD 误差表要手动重算**：改几何后误差表会标「已过期」而不是自动重测。
8. **构建缓存只在单进程内复用**：指纹写进 `library.json`，跨会话可用；但缓存只覆盖“源/贴图/代码都没变”这一种命中，改了参数就会完整重烘（oak 约 6.6 s），没有增量/后台构建。
9. **场景发布未接**：没有把物种实例发布进场景资产的 UI。
10. **浏览器端截图验收未做**：本轮的原生截图全部来自宿主；页面新增的调试通道下拉、ACK 状态行、撤销快捷键只在代码层验证（`node --check`）与服务层测试，没有在真实 Electron 窗口里截图。

## 8. 验证

```powershell
# 服务端行为 + 修订号/冲突/原子写/撤销重做（9 项）
node --test tests/vegetation-editor.test.mjs
# 生成器图与几何不变量（21 项，含草与多 frond）
node --test tests/vegetation.test.mjs
# 原生内容协议端到端（真实宿主进程、真实管道）
node .incrementalTesting/vegetation/vegetation-sync-check.mjs
# 几何/材质/性能证据
node .incrementalTesting/vegetation/vegetation-production-report.mjs --seeds 3
```

浏览器验收（2026-10-26，Edge 无头 + CDP，`--use-angle=swiftshader`；**第五批**的实测，第六批的页面改动未重新截图）：

| 项 | 实测 |
| --- | --- |
| `/vegetation`、`/vegetation.css`、`/vegetation.mjs` | 200 / 200 / 200 |
| WebGL | `WebGL 2.0 (OpenGL ES 3.0 Chromium)`，画布 1078×643 |
| 首屏（oak 默认图，LOD1） | 23,392 三角 / 40,864 顶点 / 预览 1.98 MB / 1,362 ms（含 analysis 三级别误差测量） |
| 生成器树 | 7 行（Trunk + 5 级 Branch + Frond 1），物种库 4 条 |
| 真实编辑（Branch 2 分支数 2 → 6） | 视口 23,392 → **69,610 三角**，状态栏 `预览 LOD1 · 69,610 三角 · 273 ms`，树行同步显示 ×6 |
| 越权请求 | 错误令牌 → 403 |

保存与烘焙在临时根目录验证过（第六批改为自动化）：写源文件 → `compileSpecies` 出包 → `library.json` 出现该条目；原子写与冲突拒绝由 `tests/vegetation-editor.test.mjs` 覆盖。当前 oak 烘焙为 185,976 三角 / 19.5 MB / 约 7.7 s（`performance.json`）。

## 9. 与烘焙器的一致性

预览与 cook 共用 `buildMesh`/`buildGrassMesh`，因此二者的顶点、风权、UV 单元必然相同；差异只在打包（哈希、图集、碰撞/阴影代理）与 LOD 级别。这也意味着**预览里能看到的形状错误，烘焙后一定还在**——不是两套实现互相掩盖。
