# 植被与毛发：当前实现台账（v5）

- 日期：2026-10-26（v5 为生产级植被编辑重构：结构中间表示、连续扫掠、叶柄挂点、完整材质槽、原生同步与证据；v4 为三条结构性缺陷与缺陷分析文档；v3 为贴图 mip 链与遮罩覆盖率；v2 为主光阴影与叶片分布）。
- 缺陷归因与改进排序另见 [VEGETATION-DEFECTS-AND-PLAN.md](VEGETATION-DEFECTS-AND-PLAN.md)：本文回答“做过什么”，该文档回答“哪里不对、先修哪个”。
- 依据：[VEGETATION-HAIR-DEVELOPMENT-PLAN.md](VEGETATION-HAIR-DEVELOPMENT-PLAN.md)（v1.0，2026-10-25）。
- 口径：**只登记本次真正落地并有可复现证据的条目**。计划里未被实现的部分逐项列为未完成，不用“支持植被/毛发”这类布尔值概括。
- 机器：AMD Radeon 890M（集显，计划 §1.2 的功能回退目标），Windows/D3D12，D3D12 调试层开启。**未在 RTX 5060 Laptop 主目标上测量**，因此本文不含任何帧预算结论。
- v5 的逐项验收在 `reports/vegetation-production/acceptance.md`；基线审计与根因在 `reports/vegetation-production/audit.md`。

## 1. 结论摘要

本次交付了一条可运行、可复现、字节确定的植被与 Groom **内容 + 运行时 + GPU 间接绘制**纵向切片：

- 离线：物种包（LOD/误差/风权/ impostor 图集/碰撞/阴影代理）、区域包（32 B/实例、分页、空间索引）、Groom 包（guide/strand/卡片/头皮绑定）。
- 原生：包解析与哈希校验、分页流送状态机、CPU 参考剔除、LOD 误差投影、HZB 保守遮挡、XPBD guide 求解与插值。
- GPU：`ExecuteIndirect` 实例化绘制，剔除与可见列表压缩全部在 GPU 完成，**CPU 从不回读可见计数**；双面叶片 + 厚度透射 + 遮罩裁切；**主光阴影贴图（alpha 裁切 caster + 3×3 PCF）**；真实像素证据 5 张 PNG。

**没有做到**的（同样是结论的一部分）：30M 实例从未被物化；没有任何一帧的 P95/P99 计时；没有运动矢量/TAA、没有级联/缓存阴影、没有体素/部件复用、没有 strand/card 的 GPU 渲染器。按计划 §10.4 的决策门，这些阶段仍处于“未开始”。

## 2. 可复现证据

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/vegetation/vegetation-hair-verify.ps1
```

该脚本只做本专项：生成夹具 → cook → 编译 3 个原生测试 → 运行 Node 测试 → 写 `reports/vegetation-hair/evidence.json`。它不运行全量测试、`npm test`、`npm run verify` 或 CI（遵守 `AGENTS.md`）。

| 证据 | 结果 |
| --- | --- |
| `tests/vegetation.test.mjs` + `tests/groom.test.mjs` | 20/20 通过 |
| `native_vegetation_tests.exe` | 1,057,057 次断言，7 组 |
| `native_groom_tests.exe` | 5,381 次断言，7 组 |
| `native_vegetation_draw_tests.exe` | 15,728,709 次断言，9 组，D3D12 调试错误 0 |
| GPU 覆盖像素（512×512，单株 13.5 m 橡树 @22 m） | 前视 13,377、五株 46,450、背视 13,600、距离剔除 0 |
| 主光阴影（同机位，只改阴影开关） | 覆盖 13,377 → 13,377（本阈值下无漂移）、覆盖像素平均亮度 0.254 → 0.157、压暗 38.1% |
| 贴图 mip 链 | 8 张各 10 级（512→1），单张 1,398,132 B；其中 6 张绑定到 GPU，约 8.4 MB 常驻（含 mip 约 +33%，尚未压成 BC） |
| 遮罩覆盖率（阈值 0.25，red 通道） | 512:0.2959 → 256:0.3009 → 128:0.3015 → 64:0.2976 → 32:0.2979 → 16:0.2969 → 8:0.2969（最差 −1.7%；4 级起 0.375/1.0，见 §2.4） |
| 远景（45 m，叶片约 3 px） | 覆盖 2,320 px；相机横移 50 mm（约半像素）覆盖 2,349 px，漂移 1.25% |
| 物种包 | oak 70,008 tri / LOD 误差 0.085 m、0.138 m；pine 28,368 / 0.169、0.232；hazel 1,780 / 0.140、0.205；grass 84 / 0.135、0.351 |
| 区域包 `meadow_ridge` | 5,223 实例、25 页、167,136 B（32 B/实例）、2.56 ha、2,040 株/ha |
| 量化误差 | 位置 0.478 mm、高度 0.0076 mm、偏航 9.6e-5 rad、缩放 2.2% |
| 30M 容量（仅计算，未物化） | 960,000,000 B 实例 + 234,432 B 页表 = 960.23 MB，在 1.2 GB 软上限内 |

生成的图像：`build/vegetation-draw/{single-front,five-front,single-back,culled,single-shadow,single-distant}.png`。

> 表中数字与 v1 的差异不是“画质提升”的证据，而是测试口径修正：v1 记的是 96×96 目标上的覆盖数，当前测试按 512×512 渲染；物种三角形数随 §2.3 的叶片分布改动，覆盖数随 §2.4 的 mip 链改动。跨版本对比请只比同一口径。

## 2.1 材质贴图（2026-10-26 第二批）

用户提供的 8 张 1254×1254 PNG 已进入资产管线并接线到绘制：

| 角色 | 来源 | 用途 |
| --- | --- | --- |
| `bark_albedo` / `bark_albedo_moss` | 树皮彩色 / 苔藓树皮 | 树皮反照率，`tileMetres = 1.6` |
| `bark_normal` / `bark_roughness` | 树皮法线 / 灰度粗糙度 | 树皮法线与粗糙度 |
| `leaf_atlas_summer` / `leaf_atlas_autumn` | 橡树叶图集（3×4）/ 秋色图集 | 叶片反照率，按单元采样 |
| `leaf_mask` / `leaf_normal` | 叶片遮罩 / 叶片法线 | 遮罩裁切与叶片法线 |

- cook：`engine/vegetation/texture.mjs` + `tools/vegetation-textures.mjs`，把 PNG 转成 `NGT2` 原始包（32 B 头 + RGBA8），原生侧不需要 PNG 解码器。带 `mask` 的角色会做两遍颜色膨胀，避免双线性采样在卡片边缘读到图集背景。
- 图集 UV：物种 cook 把每张叶片卡片绑定到图集的一个单元（`u0/v0/du/dv` 烘焙进顶点 UV），遮罩与法线图共用同一套 UV。
- 物种包记录 role→digest；加载器校验 `textures.<group>.roles.<slot>.digest` 存在，渲染器按 digest 找文件，不按名字猜。
- **已接线**：`VegetationDraw::setTextures` 一次上传 6 张纹理（bark albedo/normal/roughness、leaf albedo/mask/normal），根签名用「根 CBV + 2 根 SRV + UAV 表 + 材质 SRV 表」= 7 DWORD。像素着色器按顶点材质号选纹理，遮罩裁切、法线贴图、天空/地面双向环境光、厚度透射、GGX 高光全部生效。
- 修复的关键缺陷：材质号原先以整数位写进 float 顶点属性，GPU 把非规格化数清零后所有叶片都被当成树皮（整棵树呈棕色）。改为普通浮点后叶片才正确使用图集。
- 当前画面（512×512，24 m 机位）：`build/vegetation-draw/{single-front,five-front,single-back,culled,single-shadow}.png`。**仍未达到照片级**：树冠轮廓偏薄、叶片覆盖有空洞，属于 V5（部件/误差驱动层级）与覆盖 mip 的缺口，不是贴图问题；阴影已按 §2.2 补齐主光路径。

## 2.2 主光阴影（2026-10-26 第三批，V3 部分）

计划 §6.5 要求“先主光局部精细阴影”。本次只做这一层，级联、缓存阴影页与 DXR 代理回退仍未做。

| 项 | 落地内容 |
| --- | --- |
| caster | 独立深度 PSO：`VSShadow` 与可见网格共用实例解码、风位移和材质属性；`PSShadow` 用同一张叶片遮罩做 alpha 裁切，**一片叶投的是叶形阴影，不是矩形卡片** |
| 矩阵 | `lightViewProjection` 与相机矩阵放在同一个常量块（76 DWORD）里。两趟 pass 记录进同一命令列表，共用一个 CPU 写入的 CB，分成两个 CB 反而会让第一趟读到第二次写入的值 |
| 接收 | `t9` 深度 SRV + `s1` 比较采样器（LESS_EQUAL，border 白色 = 视锥外不算被遮挡），3×3 PCF，法线偏移 + 深度比较偏移，不走光栅 DepthBias（一个角度调对的 bias 在另一个角度静默失效） |
| 能量 | 阴影只乘反射项与高光；透射项按 0.35 权重部分衰减——叶片挡叶片时背后那层仍然透光 |
| 显存 | 2048² D32 = 16 MiB，单张，随 `VegetationDraw` 生命周期 |

证据（同机位、同页面，只改阴影强度）：覆盖像素平均亮度 0.254 → 0.157（压暗 38.1%），覆盖像素 13,377 → 13,377。这一版两者相等是阈值巧合，不是不变量：覆盖判据是亮度阈值，阴影可以把极少数刚过阈值的像素压到线下（上一版是 10 px / 15,615），所以测试断言的是“漂移 ≤ 0.5% 帧面积”并且把实测漂移写进证据。关掉阴影后亮度回到 0.254（断言 < 1e-4 差值），因此压暗来自阴影贴图而不是别的光照改动。

**已知限制**（不是待办清单，是当前行为）：

1. caster 复用**相机**剔除后的可见列表，没有按光源视锥重新剔除。主光在相机背面时，相机看不到的植株不会投影。
2. 单张固定 2048²、固定覆盖区，没有级联、没有按屏幕尺度分配精度；远景林冠的阴影会很粗。
3. 没有风影一致性验证（caster 与接收者是同一帧同一风相位，但没有跑序列看闪烁），也没有“阴影方案 A/B”（§6.5 要求的缓存光栅 vs 共享 BLAS）。

## 2.3 叶片分布修正（内容侧，同批）

`buildMesh` 的注释写着“叶片沿最后两级枝条分布”，代码却是 `leafDepth = maxDepth - 1`，只挂最外层。这正是“树冠偏平”的一处直接来源：所有叶卡落在同一层末端枝上，从侧面看是一条叶带而不是一个有厚度的冠。

- 改为“最外层满密度 + 向内一层半密度”，并写成两个具名层级（`denseDepth` / `interiorDepth`）。第一版实现把半密度错加到了最外层，导致只有两级的灌木（hazel）反而**减少**了叶片（1,780 → 1,076 tri）；现形态对灌木是恒等变换。
- 结果：oak 122,744 → 132,984 顶点、64,888 → 70,008 三角形；LOD1 误差 0.119 → 0.085 m、LOD2 0.143 → 0.138 m（同物种同 seed，误差表仍单调）。hazel 1,780 tri、grass 84 tri、pine 26,688 → 28,368 tri。
- **没有解决**：冠形仍偏薄、枝条分布仍以直立为主，真正的分层与部件复用属 V5，本次不声称改善轮廓。
- 连带修正 `tests/vegetation.test.mjs` 的夹具：`budget.maxVertices` 从 60,000 提到 160,000。该夹具此前已因叶片密度增加而**红着**（`lod0 exceeds budget.maxVertices`，连带 scatter/region/stable-id 共 4 项失败），本次不是放宽断言，而是把声明改成生成器实际产出的量级；预算校验本身仍生效（另有 `maxVertices: 4` 的拒绝用例）。

## 2.4 贴图 mip 链与遮罩覆盖率（2026-10-26 第四批，V2 覆盖 mip）

计划 §6.3 要求“法线 mip 与 alpha coverage mip 保持稳定”。上一批的 6 张材质都是**单 mip**，远景叶片因此会走样甚至掉叶子。

| 项 | 落地内容 |
| --- | --- |
| 格式 | `NGT2` → `NGT3`：magic/版本加一，头 32 B 的第 24 字段改为 mip 级数，各级按 `w>>L`×`h>>L` 顺序拼接，无偏移表（偏移可由级数推出，少一处会和头不一致的地方）。`parseTexture` 按头推导每级长度，读不出就拒绝，不信任头里的字节数 |
| 过滤方式 | 每角色显式声明 `filter`：`color`（线性光域平均，8-bit sRGB 直接平均会压暗）、`normal`（平均后重新归一化）、`coverage`（保持 alpha 裁切覆盖率）、`data`（普通盒滤波）。**不按角色名猜**语义 |
| 遮罩修正 | 每级求一个**偏移量**把覆盖率拉回基准级。这里最初写成了“围绕 cutoff 缩放”，实测覆盖率纹丝不动——`cutoff + (a-cutoff)·s ≥ cutoff` 等价于 `a ≥ cutoff`，任何正 s 都不改变哪些纹素通过。阈值只认偏移，不认缩放 |
| 上传 | `MipLevels` 与 SRV 级数取实际链长，逐级 `CopyTextureRegion`（每级独立 256 B 行对齐），一块 staging 装整链 |

可复现命令与实测覆盖率（`--in` 指向 cook 输出）：

```powershell
node tools/vegetation-textures.mjs mips --in build/vegetation/textures
```

| 级 | 512 | 256 | 128 | 64 | 32 | 16 | 8 | 4 | 2 | 1 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 覆盖率 | 0.2959 | 0.3009 | 0.3015 | 0.2976 | 0.2979 | 0.2969 | 0.2969 | 0.375 | 1.000 | 1.000 |
| 与基准比 | 1.000 | 1.017 | 1.019 | 1.006 | 1.007 | 1.003 | 1.003 | 1.267 | 3.379 | 3.379 |

- 512→8 级最差 −1.7%，远好于普通盒滤波在同一张图上“逐级侵蚀到 0”的表现（对照用例 `tests/vegetation.test.mjs` 的合成细线遮罩：普通盒滤波在 4 px 级直接归零，覆盖滤波仍有内容）。
- 4/2/1 级是**刻意向上取整**：3×4 图集在 4 px 以下已经分辨不出一片叶，单值只能取“有”或“无”。取“有”，因为亚像素叶片从轮廓里消失读作洞，多一个亚像素点不读作错误。代价是最近两级会把整张图集压成实心，远景叶团因此偏实。
- 按图集单元抽查（3×4 共 12 格）：512 级各格 0.20–0.33，32 级 0.24–0.36，没有出现“这一格补到实心、那一格被抽空”的跨格挪移。

**没有解决 / 是新引入的**：

1. **近景轮廓薄了 14%**：22 m 机位覆盖从 15,615 降到 13,377。单级上传时亚像素叶片会走样成“整块通过”，计数偏高；mip 过滤后边缘变成软过渡，在 0.25 阈值处被切掉一部分。这是抗锯齿与轮廓厚度之间的真实取舍，本批选择抗锯齿。要同时拿到两者，需要着色器侧随机化阈值（stochastic alpha）或 coverage-to-alpha，不是继续在 cook 侧加偏移。
2. **45 m 处仍有叶片变稀**：覆盖 2,320 px，相对 22 m 机位按 1/d² 折算应为 3,200，实际少了约 28%。这与“整片叶子掉没”的洞不是一回事（轮廓连续，横移 50 mm 覆盖只变 1.25%），但远景叶量确实不足，根因是叶片几何本身稀疏，不是 mip。
3. 覆盖率的度量是“阈值计数”而非面积：细结构在下采样过程中会先升后降，因此本批不对中间级断言“等于基准”，只断言“不掉到 0 / 不越过 0.5% 帧面积漂移”。

## 2.5 编辑器（2026-10-26 第五批，V7 部分）
计划 §5.2/§10.2 把笔刷、预览、cook 诊断放在 V7。本次交付的是“参数 → 几何 → 字节”这一段闭环，细节见 [VEGETATION-EDITOR.md](VEGETATION-EDITOR.md)。

- **新增数据层**：`engine/vegetation/generators.mjs` 定义 `spec.generators`（层级分支数/展开角/长度比/半径比/起始高度/每级风权 + frond 卡片）。字段可选：没有它的物种仍走原配方，字节不变（既有 18 项测试与 `vegetation-hair-verify.ps1` 全绿）。
- **风权成为真参数**：段顶点在父级风权与本级目标之间插值写进 `WIND_VERTEX`。测试断言“把所有层级风权降到 0.05，顶点风权峰值必须落到 0.2 以下”，否则说明滑杆是装饰。
- **预览走服务**：`buildPreview` 导出分 LOD 的几何与统计，不打包。浏览器不复制几何代码——每次改动往返一次 `species.mjs`，所以视口与烘焙不可能不一致。
- **烘焙落库**：`vegetation.cook` 复用 `compileSpecies`，写 `<hash>.ngveg` 并**合并**进 `library.json`（只替换同 id）。
- 实测：oak 默认图 LOD0 33,168 tri / LOD1 21,370 tri，预览 LOD1 往返 payload 2.77 MB、`primitives` 合并为 2 段（未合并时是 9,357 段，逐段 draw call 不能当预览）；烘焙 1.7 MB 包、8 视角 128² 图集、耗时约 320 ms。

**没有做到**：笔刷/稀疏覆盖、视口用真实图集、impostor 与 mesh 的 A/B、真实浏览器里的截图验收。这四条写在文档 §5，不是“待办清单”而是当前行为。（撤销栈已在 §2.6 补齐。）

## 2.6 生产级植被编辑闭环（2026-10-26 第六批，v5）

本批按 `docs/DEEPSEEK-VEGETATION-PRODUCTION-PROMPT.md` 重构了“源参数 → 结构 → 网格 → 材质 → cook → 原生视口 → 保存/重开”的整条链，并把每一项的实测数字写入 `reports/vegetation-production/`。

| 项 | 落地内容 | 实测证据 |
| --- | --- | --- |
| 结构中间表示 | 新增 `branch-graph.mjs`（BranchGraph/稳定 ID/父引用/attachment/运输框架/弧长表/半径曲线）、`branch-mesh.mjs`（连续扫掠 + 分叉 collar）、`leaf-placement.mjs`（叶柄 + pivot + blade 曲面 + 草叶片）、`atlas.mjs`、`material-contract.mjs` | `geometry.json`：四物种 × 3 seed，`junctionGapMaxMetres = 0` |
| 枝干连接 | 子枝根 = 父曲线采样点（不再是“中心线 + 8% 偏移”）；子方向在父切线框架内构造 | 同上；`attachment.meanAngleDeviationRadians` oak 0.079 |
| 连续曲面 | 整枝共享 ring、seam 复制顶点（U 不同、位置/法线相同）、`dr/ds` 进入法线 | `seam.seamPositionMismatch = 0`；`degenerateTriangles = 0` |
| 叶柄挂点 | pivot 在叶柄末端；叶柄是真实几何；叶序按 divergence | `leaves[].pivotDistanceMetres.max` = 叶柄长度（oak 0.0274 m） |
| 草不再走 legacy 网格器 | 删除整段 `buildLegacyMesh`/`buildTree`/`tube`/`card`；草改走 BranchGraph + blade ribbon，按 rank 取子集保证 LOD 嵌套 | `geometry.json` grass：14 枝 / 252 tri / 0 退化 |
| 材质槽 | 九槽统一 ABI：bark baseColor/normal/ORM/height + leaf baseColor/normal/ORM/thickness/opacity；位域逐槽推导 | `materials.json`：全部 `bound`；原生测试断言 cutoff 一致 |
| 调试视图 | 着色器内 15 个通道（baseColor/UV/normal/mappednormal/AO/roughness/metallic/opacity/thickness/height/wood/leaves/depth/shadow） | `images/oak-debug-*.png` 14 张 |
| 原生内容协议 | 管道结构化命令 `load/view/capture/status` + `rev/want/digest` ACK + 命令队列（不丢 ACK）+ 宿主崩溃重启重放 | `editor-sync.json`：11 步全过（含 A→B→C→D→A、错误 digest 拒绝、latest-wins） |
| 生产保存 | 内容寻址 revision、`expectedRevision` 冲突拒绝、临时文件 + rename 原子写、撤销/重做、真实 `textureRoles` | `tests/vegetation-editor.test.mjs` 9 项全绿 |

同时修掉三个**此前未登记**的原生缺陷：根签名描述符表比着色器少一个槽（PSO 直接创建失败）、纹理位域恒为 15（无贴图物种渲染成白卡片）、重叠命令丢 ACK。

**仍然未完成**（详见 `reports/vegetation-production/acceptance.md` §7）：无风动 300 帧序列断言、图集单元隔离 mip、GPU 时间戳与 1k/10k 实例、构建缓存指纹、浏览器端截图验收、场景发布 UI。

### 2.6.1 第二轮（同批补完）

| 项 | 修复 | 实测 |
| --- | --- | --- |
| 灌木冠幅 | 灌木改为多干（`trunk.count = 5`），一级子枝长度比提到 0.9–1.08；`crownRadiusMetres` 从“校验但不消费”变成真正约束 | hazel 冠幅 0.77 → **1.49 m**（声明 1.5）；新测试对 tree/shrub/grass 断言比值 ∈ [0.8, 1.02] |
| 默认物种裸露枝梢 | 默认 frond 的 `hostLevels` 从 `depthLimit-3` 改为最外两层；针叶树取第 2 层到顶层 | 灌木/松树截图中枝梢被叶片覆盖 |
| 针叶无贴图 | 新增 `tools/vegetation-needle-atlas.mjs` 生成原创 4×4 针叶图集（baseColor + opacity 同源 alpha），角色带 `directory` 字段与作者图集分开存放 | pine 6 个材质绑定全部 `bound`；`pine-closeup-needles.png` 有针叶轮廓 |
| 粗 LOD 越界 | `meta.bounds`/阴影代理/碰撞改用全 LOD 并集 | 原生 `vertex outside the species bounds` 断言由失败转全过（1,483,221 断言） |
| 风动接合 | 新增“子树根风权 == 父枝同弧长处风权”和“风权沿枝单调”两条断言 | `tests/vegetation.test.mjs` 24 项全绿 |

### 2.6.2 第三轮：材质见证

`tests/native_vegetation_draw.cpp` 新增 material-witness 组：同机位、同几何、同光照，只改一个材质字段，比较同一帧的像素（结果在 `reports/vegetation-production/native-draw.json`）。

| 字段 | 改动 | 实测 |
| --- | --- | --- |
| `leafRoughness` | 0.05 vs 0.95 | 峰值亮度 1.259 → 0.971 |
| `normalScale` | 0 vs 1 | 平均亮度 0.2401 → 0.2449 |
| `occlusionStrength` | 0 vs 1 | 0.24500 → 0.24492（只影响环境项） |
| `alphaCutoff` | 0.35 vs 0.85 | 覆盖 12,499 → 3,199 px |
| `leafThicknessMetres` | 0.00002 vs 0.02（逆光） | 0.1856 → 0.1286 |
| 调试过滤器 | wood / leaves / shaded | 2,860 / 10,995 / 12,499，未归属像素 0 |

同时修正：`wood`/`leaves` 两个调试视图此前落进 debug 输出分支返回黑色（两个视图都“看起来是空的”），已改为过滤后按正常着色输出。

### 2.6.3 第四轮：风动序列与构建缓存

| 项 | 落地 | 证据 |
| --- | --- | --- |
| 风动 300 帧 | `engine/vegetation/wind.mjs` 把 VSMain 的风模型集中一处（注释指明 HLSL 的三行），测试逐帧比较子树根与父枝同弧长点的位移 | 300 帧最大位移差 **< 1e-6 m**；风权故意偏 0.05 时升到 **> 1e-4 m**（证明能看见缺陷）；风权沿枝单调、无 NaN |
| 构建缓存 | `vegetation.cook` 指纹 = 源内容哈希 + 贴图库 `packageHash` + cook 代码哈希（8 文件）；命中返回 `cached: true`，`force` 强制重烘 | 首次 `cached=false` → 再烘 `cached=true` 且哈希相同 → 改一个尺寸后 `cached=false` 且指纹变化 → `force` 得到相同字节 |

### 2.6.4 第五轮：法线 convention 接线

审计发现 `normalConvention` 只在 Node 侧校验、写进包，原生与着色器都不读——把 DirectX 法线图换成 OpenGL 图，画面不会有任何变化。已补全整条链：`asset.cpp` 解析为 `Material::flipNormalGreen`（未知拼写报错），`VegetationMaterial` 增两个 per-material 标志，常量块新增 `materialSurface`（92 DWORD），着色器只在法线采样路径翻一次绿通道。见证测试：翻绿后画面均值变化 > 1e-5，而 `normalScale=0` 时变化 < 1e-4（说明标志只作用于该路径）；包内两个材质各自的 convention 分别保留。

### 2.6.5 第六轮：图集按单元隔离 mip

`texture.mjs` 的 mip 链此前是整图盒滤波：3×4 叶片图集与 4×4 针叶图集下采样时把相邻单元平均在一起，远处每张叶片卡片都带上邻居的颜色。

- 角色声明 `atlas` 时，每个目标纹素的 2×2 足迹夹在所属单元矩形内（单元边界按归一化坐标推导，1254 px 分 3 列也不会因非整除错位）。
- 覆盖率修正改为**逐单元**求解：每个单元用自己的基准覆盖率作目标，而不是让整图平均去迁就一个全局偏移。
- 合成 2×2 夹具（一格实心白、三格全透明）：隔离链的不透明格每级 ≥ 250，整图链在 1×1 级掉到约 137——否则这条测试什么也没证明。
- oak `leaf_opacity` 实测：逐单元最差漂移 128 px 级 3.7%、32 px 级 13.0%（整图链 3.3% / 13.0%，差异由量化主导）；近景覆盖像素 12,499 → **12,742**，单元边界不再互相侵蚀。

### 2.6.6 第七轮：GPU 时间戳

宿主现在每帧记录四个 D3D12 timestamp query（命令表开始 / 阴影 pass 后 / 颜色 pass 后 / 结束），解析进 8 槽 readback 环，下一帧在既有 fence 等待之后读取——测量不阻塞帧。`--timing-json <path>` 输出 P50/P95/max 与 Present 的 CPU 时间（后者明确不是 GPU 时间）。

| 场景（640×400） | 采样 | GPU 中位 | P95 | 阴影中位 |
| --- | --- | --- | --- | --- |
| 单株 oak LOD0（185,976 tri） | 599 | **0.708 ms** | 0.999 ms | 0.444 ms |
| 1,000 株（每株 LOD0） | 399 | **81.6 ms** | 83.1 ms | 40.8 ms |
| 10,000 株（每株 LOD0） | 399 | **717 ms** | 804 ms | 359 ms |

第三条不是“能画一万株”：每株都是 LOD0，10k × 185,976 ≈ 1.86 G 三角形，因为**逐实例 LOD 选择还没实现**（宿主一页只绑一份网格）。这组数字把这个缺口从一句话变成了可测量的事实。

### 2.6.7 第八轮：Height 视差合同与图集 cell 诊断

- **Height 只移动贴图查找**：同一帧只改 `heightScaleMetres`（0 → 0.05）。着色均值 0.239354 → 0.239426（视差确实被应用），相机深度均值 0.2382131 → 0.2382131、覆盖变动 **0 像素**（几何与深度没有被位移）。这是 §7.4 “Parallax 不改变树干轮廓”的可测形式。
- **图集 cell 诊断（数值）**：用真实 1254 px / 3 列的尺寸断言 `pixelX ↔ u0*width` 在 1 texel 内一致、cell 中心落在自己的矩形内、row 0 拥有最高 v（`v1 = 1`）、最下行从 `v0 = 0` 起、pivot 在每格底部中心。带编号的截图仍未做。

### 2.6.8 第九轮：可复现的风动取证

宿主新增 `--time <秒>` 钉住风时钟（`frame.seconds` 原本取自 `GetTickCount64()`，两次截图必然不同）。同一相机 + 同一 time 两次截图**字节相同**，不同 time 画面不同。取了三帧（t = 0.00 / 0.45 / 0.90 s）的整树与分叉特写：特写里树干纹理逐像素对齐、树冠叶片位移——“风动只动叶子、不动接合”的视觉对照。300 帧的数据级断言在 `tests/vegetation.test.mjs`，但没有 300 帧渲染序列。

### 2.6.9 第十轮：LOD 误差表不再对“删叶子”失明

§4 第 12 条登记的缺陷——降级靠丢叶片卡片，而误差只量“稠密顶点到粗网格的距离”，删卡片不动剩余顶点，所以 LOD1 只报 0.085 m——本轮改成两个量并列发布：

| 物种 | LOD1 距离 | LOD1 留面率 | LOD2 距离 | LOD2 留面率 |
| --- | --- | --- | --- | --- |
| oak | 0.137 m | **64.5%** | 0.197 m | 31.5% |
| pine | 0.135 m | 65.5% | 0.220 m | 34.0% |
| hazel | 0.090 m | 63.7% | 0.129 m | 32.6% |
| grass | 0.067 m | 62.9% | 0.128 m | 28.2% |

留面率写进 `meta.lod`（JSON，不动打包记录以免为一个报表字段改原生 ABI）、`buildPreview.lodTable`、`geometry.json` 与编辑器 LOD 阶梯的新列。运行时仍没有逐实例按此选择 LOD——那是下一项。

### 2.6.10 第十一轮：逐实例 LOD 的尝试（已回退）

尝试在宿主层用“一 LOD 一个 draw 对象”实现逐实例 LOD，写到一半回退：`page` 是索引式的，且 `firstInstance/instanceCount` 只能描述**连续**切片，而按距离分桶后实例在环上是交错的。绕开它要么 CPU 每帧重排实例表（违反“CPU 从不遍历实例”的契约），要么改 ABI。

正确路径在 GPU：`CSCull` 按距离把实例追加进每个 LOD 的桶，`CSArgs` 为每个 LOD 写一条 `DrawIndexed` 参数（`StartIndexLocation` + `StartInstanceLocation` 指向该桶），一次 `ExecuteIndirect` 提交 N 条；阴影 pass 同样按桶提交；`upload` 把所有 LOD 拼进同一份 VB/IB。这跨着色器、ABI、绘制与阴影四处，规模超过本轮能安全验证的范围，因此整体回退——宿主重新编译并跑通 ACK 与 GPU 计时，digest 与三角形数不变。1k/10k 的 81.6 ms / 717 ms 仍是单 LOD 路径的实价。

### 2.6.11 第十二轮：编号角标诊断图

`.incrementalTesting/vegetation/vegetation-atlas-diagnostics.mjs` 生成 3×4 编号图集（每格底色 + 0–11 数字 + 四角标记 + 1 px 边框），并生成只用于该检查的物种与独立 cooked 目录 `build/vegetation/cooked-diag`。渲染后格 10 的数字正立、四角颜色在预期端（左上红 / 右上绿 / 左下蓝 / 右下黄），说明图集行序 → UV v → 卡片 v 三段约定一致。

**必须独立 cook**：`compileRegion` 的物种表取的是整个库，诊断物种若进主库会让每个区域记录 5 个物种，并破坏原生断言“region must reference four species”。

### 2.6.12 第十三轮：来源指纹

§12 要求证据文件写明命令、源码/二进制指纹、shader 与包摘要。`.incrementalTesting/vegetation/vegetation-production-report.mjs` 现额外产出 `reports/vegetation-production/provenance.json`：17 个源文件（引擎植被模块、着色器、绘制、宿主、服务、两个证据工具）的 sha256、git HEAD 与工作区改动数、cooked 包哈希（物种 + 区域）、贴图库 `packageHash` 与角色数、机器与 Node 版本、以及本轮实际执行的命令表。理由很直接：这些植被文件在当前工作区是**未跟踪**的，源码 sha256 是唯一能把一个数字绑到一份代码上的指纹。

同轮确认 ACK 与包一致：`editor-sync.json` 的初始 ACK digest `54fb909a…` 与 `provenance.json` 里 oak 的包哈希完全相同。

### 2.6.13 第十四轮：证据自检

`.incrementalTesting/vegetation/vegetation-evidence-check.mjs` 交叉校验六份证据互相一致（几何不变量、材质槽全绑定、图集角色必须声明 cell grid、ACK digest 必须能在包里找到、GPU 字段是正数而非 0、原生见证的四组关系、`acceptance.md` 引用的图片都必须存在）。当前 **57 项全过**。它不重新测量，只抓“数字互相打架”。这条检查也写进了 `provenance.json` 的命令表。

### 2.6.14 第十六轮：300 帧渲染序列

宿主新增 `--time-step`（按固定步长推进被钉住的风时钟）与 `--frame-stats <path>`：每帧读回后缓冲一次，按“与清屏色不同”统计覆盖像素与平均亮度，退出时写 `reports/vegetation-production/wind-sequence.json`。

300 帧 / 256×192 / 步长 1/60 s：**0 个 NaN**，覆盖 2,843–2,914 px（5.9% 画面），单帧最大跳变 **15 px**（约覆盖面积 0.5%），平均亮度 68.289–69.243 且 **300 帧取值全不同**——树冠确实在动，轮廓没有跳变。证据检查器据此新增四条断言（共 68 项）。

仍未做：从 GPU 读回接合点世界坐标（需要一个投影接合点的 compute pass 与 cook 期接合点对表）。

### 2.6.15 第二十五轮：逐实例 LOD 的先验问题定论

第十一轮整体回退时留下的“关键风险”——`StartInstanceLocation` 是否计入 `SV_InstanceID`——在不写一行引擎代码的前提下被查清：**不计入**。依据 HLSL 规范提案 0015 原文 *“SV_VertexID doesn't include the StartVertexLocation and SV_InstanceID doesn't include the StartInstanceLocation values provided to the API”*（<https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/>）；该提案的存在本身就是为了补上这两个值，另见 microsoft/DirectXShaderCompiler#2946（Vulkan/SPIR-V 的 `gl_InstanceIndex` 含 base，行为不同）。

这否掉了原方案里“`StartInstanceLocation` 指向桶基址 + 一次 `ExecuteIndirect` 提交 N 条”的写法：VS 看到的 instanceId 恒从 0 开始，无法判断自己属于哪个桶。修订后的方案是桶基址走根常量、**每 LOD 一次** `ExecuteIndirect(MaxCommandCount=1)`，`StartInstanceLocation` 恒 0；同时 `vegetation.hlsl:212` 的越界钳位要改成针对桶容量，否则桶尾读到相邻桶。修订已同步进 `reports/vegetation-production/audit.md` §3.11/§4.1 与 `next-steps.md` §1。

本轮无代码改动，`vegetation-evidence-check`（71 项）与 `vegetation-sync-check`（11 步）仍全过。逐实例 LOD 本身**仍未实现**。

### 2.6.16 第二十七轮：发布链路的前半段

新增 `vegetation.publish` 动作与顶栏「发布场景」按钮：把已烘焙物种按一份区域源编译成 `cooked/regions/<hash>.ngvrg`，合并进同一个 `library.json` 的 `regions`，并把区域源回写 `assets/vegetation/regions/<id>.region.json`。这是"已烘焙物种"到"有场景"之间一直缺的那一步——在此之前清单里只有包，没有任何页集可加载。

实现上刻意与 CLI cook 对齐：发布前逐包重算 sha256 并重解析（`readPackage` 校验 `schema` 与 `meta.id`），因此不会发布一个指向已变字节的区域；`expectedRevision` 回传上一次发布的 `hash`，不匹配报 `NG-E-VEG-CONFLICT`；包先写临时文件再 rename，源最后写（重发幂等，且不会留下"有源无包"）。区域只替换同 id 条目，物种条目不受影响。

新增测试 1 条（`tests/vegetation-editor.test.mjs`，11 条全过）：未烘焙时 `NG-E-VEG-SPECIES`、发布后包字节数与清单一致、`library.json` 只有一条区域且物种条目未被扰动、源已回写、重发哈希不变且不重复追加、`expectedRevision` 冲突、引用未烘焙物种被拒且清单不变。

仍未做：区域规则没有编辑界面（固定 32 m 见方 / 120 株每公顷，界面上写明）；宿主仍只画物种、不画区域页。

## 3. 逐阶段台账
状态口径：**已实现**＝有代码且有通过的断言；**部分**＝有代码但缺少计划要求的验收证据；**未开始**＝没有代码。

### 3.1 公共底座

| 阶段 | 状态 | 实际落地 | 缺口 |
| --- | --- | --- | --- |
| S0 基线与计量 | 部分 | 计数合同（world/resident/candidate/visible/shadowOnly）在 `WorldStats` 逐字段实现；报告结构与 `planVersion` 落地；夹具与机器信息写入证据 | 未在 RTX 5060 主目标建档；无功耗/温度记录 |
| S1 GPU Scene | 部分 | 共享物种网格池、32 B 实例表、GPU 剔除压缩、`ExecuteIndirect` 绘制；零计数与容量溢出有显式分支 | 只有单页单 LOD；无分桶、无前缀和、无多 PSO 批处理 |
| S2 深度/HZB | 部分 | `DepthPyramid` 最大值金字塔 + 保守遮挡、`Frustum` 六平面、越屏裁剪；CPU 参考剔除接入世界 | 只有 CPU 参考，没有把 HZB 接到真实渲染器的上一帧深度；未定义正/反向 Z 切换 |
| S3 运动与时域 | 未开始 | — | 无 motion vector、无 TAA、无 reactive |
| S4 预算服务 | 部分 | 每帧读/解码/上传预算、硬上限准入、LRU 淘汰与 `peakBytesIncludingInflight`；失败页计入 `failedPages` | 无 PSO 预热、无异步统计、无联合账本 |

### 3.2 植被主线

| 阶段 | 状态 | 实际落地 | 缺口 |
| --- | --- | --- | --- |
| V0 包与散布 | 已实现 | 内容寻址二进制包（magic/版本/节表/记录步长校验）、`SurfaceSampler`（平面 + 网格 BVH 垂直投射）、分区稳定散布、稳定 ID、32 B 量化页、空间索引、篡改与截断拒绝、字节级可复现 | 场景 schema/编辑器接入未做；百万实例未物化 |
| V1 基础可见切片 | 已实现 | 叶片双面 PSO + 遮罩裁切 + 厚度透射 + 包裹漫反射；三级 LOD 与实测误差；GPU 实例化与间接绘制；6 张材质贴图接线（树皮 albedo/normal/roughness、叶片 atlas/mask/normal）各带 10 级 mip；真实像素与背视证据 | impostor 已 cook 但**未绘制**；近景轮廓因 mip 变薄 14%（§2.4） |
| V2 草与规模 | 部分 | 草 clump 物种、稳定嵌套降密度（`densityKeeps`）；alpha 覆盖 mip 已做并按图集单元抽查 | 1M 压力未跑；overdraw 未测；45 m 处叶量仍偏少 |
| V3 阴影与风 | 部分 | 有界程序化风（顶点风权 + 相位）、风位移进入剔除包围盒；**主光阴影贴图**（alpha 裁切 caster、3×3 PCF、法线偏移、透射部分衰减），见 §2.2 | 无级联/缓存阴影、无光源视锥剔除、无共享场接入、无骨骼风、无风影序列与 A/B 证据 |
| V4 分页流送 | 部分 | `RegionFile` 按页 seek、状态机 `Unloaded→Requested→Reading→Decoding→UploadQueued→Resident→Retiring`、缺页计数与回退 | 未接真实 I/O 线程与解码预算；未跑 30M 冷/热路线 |
| V5 部件层级 | 未开始 | — | — |
| V6 聚合体素 | 未开始 | — | — |
| V7 工具与最终验收 | 部分 | `/vegetation` 编辑器（见 §2.5）：生成器层级编辑、WebGL 视口（实体/线框/风权三态 + 实时风动）、五组属性页、LOD 阶梯可点选切换、保存源文件、烘焙进内容库；服务端 6 项测试 | 无撤销栈、无笔刷与稀疏覆盖编辑、视口图集是替身、未做 impostor A/B、未在真实浏览器里截图验收 |

### 3.3 毛发主线

| 阶段 | 状态 | 实际落地 | 缺口 |
| --- | --- | --- | --- |
| H0 Groom 工具 | 已实现 | 曲线包（点/半径/分组/稳定 ID/根 UV/guide 权重/卡片）、弧长重采样、头皮三角形重心绑定、卡片分块覆盖、LOD 关系、单位与半径校验 | 无 Blender/Alembic 适配器；卡片是合并 ribbon 几何，不是多层卡片贴图 |
| H1 静态 strands | 部分 | 段/ribbon 数据、亚像素覆盖率与 tile 覆盖率解析、guide 插值 | **没有 strand 的 GPU 渲染器**；无 tile 多层样本缓冲 |
| H2 纤维光照 | 未开始 | 材质参数（melanin/redness/吸收/双粗糙度）已进包 | 无 R/TT/TRT 实现、无 Deep Opacity |
| H3 Guide 动态 | 部分 | CPU XPBD（距离 + 弯曲 + 球/胶囊碰撞 + 阻尼 + 速度上限）、固定根、reset 复位、风速方向性与对称性验证 | 无 GPU 求解、无肩部/手臂代理、无 CCD、无状态存档 |
| H4 高级交互 | 未开始 | — | — |
| H5 外观 LOD | 部分 | 同源 LOD（strand 子集 + 段数缩减 + 卡片覆盖），根部与 guide 权重跨 LOD 一致 | 无 cards/mesh 绘制路径、无多人预算 |
| H6 生产工具 | 未开始 | — | — |

## 4. 已知缺陷（不是待办，是当前行为）

1. **strand 根部有 ≤1.8 mm 近似**：cook 给每根发丝加了根部抖动，但 32 B 的 `GUIDE_WEIGHT` 没有位置留给它，运行时用 guide 根代替。测试按该上限断言，不掩盖。
2. **impostor 只 cook 不绘制**：`representations` 声明了 `impostor`，渲染器尚未接入；远景仍走 mesh LOD。按计划 §5.1，这属于“已声明但未消费”，不是数据造假，但也不能算完成。
3. **区域包整读**：`loadRegion` 会整文件读入，只有 `RegionFile` 是真正分页的。工具链与编辑器走前者，运行时走后者。
4. **`vegetation_draw` 单页单物种**：常量缓冲承载一页的量化原点，多页/多物种需要每页一次 draw 或把页表搬上 GPU。
5. **构建脚本未在共享 `build/` 目录验证**：本次验证期间该目录被另一个构建进程占用（互斥量拒绝），因此新目标只在 `build/vegetation-hair` 与手工命令行下编译通过。
6. **阴影 caster 复用相机可见列表**：没有独立的按光源视锥剔除。主光在相机背后时，相机看不见的植株不投影；这是“先主光局部阴影”的实现代价，见 §2.2。
7. **`shaders/vegetation.hlsl` 的常量块已 88 DWORD**（v5 从 84 增加 `debug`）：根签名仍按“根 CBV + 2 根 SRV + UAV 表 + 材质表 = 7 DWORD”预算，常量走 CBV 所以 CB 体积不受根签名预算限制，但再加字段要同步改 C++ 的 `static_assert`。
8. **阴影 PSO 与主 PSO 共用同一份常量与同一份可见列表**：两趟 pass 必须同帧同相位调用（`drawShadow` 先于 `draw`，同一命令列表），否则 caster 与接收者会描述不同的世界。这是接口契约，不是可选项。
9. **mip 链让近景叶片轮廓变薄**：22 m 机位覆盖 15,615 → 13,377（−14%）。原因与取舍见 §2.4，不是 bug，但在补齐着色器侧覆盖率合成之前会一直在。
10. **`VegetationDraw` 只被原生视口宿主使用**：`host/viewport_host.cpp` 用它画出真实植被（v5 起可切物种/LOD/调试通道），但 `renderer/d3d12_renderer.*` 仍没有把植被接进引擎帧图。也就是说“编辑器视口里有一帧真的植被”，而“游戏画面里没有”。
11. **LOD 只有数据没有执行**：`lodBias` 写进 32 B 实例记录并在 `asset.cpp:325` 解码，但 `shaders/vegetation.hlsl:73` 只读前 4 个 DWORD；`lod.hpp:30 selectLod()` 只被 `tests/native_vegetation.cpp` 调用。`VegetationDraw::draw()` 一次只绑一份索引/顶点缓冲。
12. **LOD 误差度量对“删叶子”不可见**：`QUALITY` 的降级方式是随机丢弃 38%/72% 的叶片卡片，而误差是“稠密顶点到粗网格的最近距离”——删卡片不移动任何剩余顶点，所以 oak LOD1 只有 0.085 m。用它反解屏幕误差阈值，会在冠形已经变薄时仍判定无需升级。

## 5. 建议的下一步顺序

**本节已被 [VEGETATION-DEFECTS-AND-PLAN.md](VEGETATION-DEFECTS-AND-PLAN.md) §5 的四波路线取代。** 原第 1 项（着色器侧覆盖率合成）并入该文件 Wave 2：轮廓变薄的根因是 alpha 硬阈值，而硬阈值是否够用又依赖误差度量；先修度量再修着色器，返工更少。原第 2 项（接入帧图）升为 Wave 1 的第一件事——它不是优化，是“植被系统是否存在”的前提（见 §4 第 10 条）。

以下为原顺序，保留用于对照：

1. 着色器侧覆盖率合成：`stochastic alpha` 或 coverage-to-alpha，用来同时拿到抗锯齿与不掉的轮廓厚度（§2.4 第 1 条的取舍），并把 45 m 处的叶量缺口一起补上。这是纯 shader + 采样器改动，不动 cook 格式。
2. 把 `VegetationDraw` 接进 `d3d12_renderer` 的帧图（深度预 pass + 主 pass + 阴影 pass 的槽位），并用现有 `reports/` 图像门禁建档 V-A/V-B。阴影通路与 mip 链已就绪，接入时不必再改绘制模块内部。
3. strand 渲染器：ribbon 扩展 + tile 多层样本 + 覆盖合成，先做 H-A 静态曲线与旋转灯。
4. 阴影第二层：级联或按屏幕尺度分配精度、光源视锥剔除、共享物种 BLAS 与缓存光栅的 A/B（计划 §6.5）。
5. 帧预算：在 RTX 5060 主目标上跑 300 预热 + 1800 采样的 P95/P99，替换本文的“无计时”状态。
6. 30M：先跑索引容量与缺页恢复（不需要 960 MB 常驻），再谈工作集与画质。

## 6. 与计划正文的关系

计划 §10 的估时是 67–118 人周。本次交付覆盖其中 **S0/S1/S2/S4 的基础合同、V0、V1、H0，以及 V2/V3/V4/H1/H3/H5 的数据与 CPU 侧**；其余阶段保持“未开始”。本文不修改计划的验收口径，只把每一项的真实状态登记下来。

v2 相对 v1 的增量只有三条，都是计划里已定义、不改验收口径的条目：主光阴影（§6.5 第一层）、叶片沿两级枝条分布（§6.1 LOD 与覆盖的前置）、以及把之前红着的 Node 夹具预算改对。v3 再加一条：贴图 mip 链与遮罩覆盖率（§6.3）。**没有**新增任何“已完成”的阶段，也没有因为这两批放宽任何验收口径。
