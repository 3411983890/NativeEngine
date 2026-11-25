# 交给 DeepSeek：生产级植被编辑器重构与验收完整提示词

- 编制日期：2026-10-26。
- 工作区：`C:\Users\34119\Documents\AI-Native Game Engine`。
- 目的：修复当前树枝断开、叶片悬空、材质接线错误及原生编辑不同步，完成可制作、可预览、可保存、可复现和可验证的生产植被编辑流程。
- 使用：将本文完整交给能读取和修改本工作区的 DeepSeek 编程代理。本文要求执行实现，不以重复写方案作为完成。
- 证据范围：基于当日源代码及已有 `build/viewport-host/shots/oak.png` 的静态审阅；未运行新的原生构建或测试，未宣称读取实时窗口。执行代理必须先复核工作区变化。

> 以下从“你的任务”开始均为可直接执行的提示词。文中当前缺陷是审阅时快照；如果代码已修复，先核对并记录，不重复引入旧实现。

## 1. 你的任务、授权范围与工作方式

你是负责几何生成、PBR 材质、D3D12 和 Electron 编辑器集成的工程师。请直接修改并完成当前工程中的植被编辑器，使它能生成拓扑和外观合理的树，所有可编辑参数实际作用于正确资源，并在原生视口中看到当前版本。

用户已经明确反馈：树很假，枝条分叉不相连，叶片位置不对，UV 与深度等贴图未正确接线。你需要沿着“源参数 → 结构图 → 网格/附件 → 材质资产 → cook → 原生视口 → 保存/重开”的链路修复根因，不通过增加叶量、隐藏枝条、调暗曝光、增大 alpha cutoff 或只改 UI 来掩盖错误。

具体交付包括：

1. 正确的枝干父子结构、连续曲面与近景分叉接合。
2. 具有稳定身份和叶柄挂点的叶片/叶簇，位置、尺寸、朝向与物种相符。
3. 正确且可检查的 UV、TBN、BaseColor、Opacity、Normal、ORM、Thickness、Height 语义与采样。
4. 原生视口的物种选择、几何/材质编辑、LOD、风动、调试模式和当前版本回执。
5. 生产编辑流程：非破坏编辑、撤销/重做、保存/重开、版本冲突、构建诊断、错误恢复和资产重用。
6. 同场景、同版本的几何证据、材质通道证据、编辑同步证据、画质截图和专项性能报告。

本任务优先完成单株/物种创作闭环，再补完整区域绘制的正确性。30M 世界索引、Nanite 全部机制、体素远景和毛发不作为本次新增主项目；已有接口不得破坏。生产级在此定义为本文件验收矩阵全部满足，不等于宣称已具备 UE 所有植被功能。

### 1.1 必须遵守的工作区约定

- 先读根目录及子目录适用的 `AGENTS.md`。工作区存在其他开发改动，不重置、不覆盖、不清理无关文件，不执行 `git reset --hard`、`git clean` 或整目录替换。
- 只运行依赖范围内增量测试。未经用户新明确要求，不运行全量测试/回归、`npm test`、`npm run verify` 或完整 CI。
- 资产导入关联检查沿用 `npm run test:assets`；需要验证原生资产绘制时沿用 `npm run assets:verify`，保留独立 `build/assets-host`。
- 植被原生宿主使用专用输出目录，首先阅读 `tools/viewport-host.ps1`、`.incrementalTesting/vegetation/vegetation-hair-verify.ps1`，按其真实参数执行；不要编造不存在的构建入口。
- 原创测试夹具放在本专项目录；保存/构建测试使用临时工程副本或隔离输出，不修改用户的正式物种。
- 不要求用户再次批准已经授权的常规实现与专项验证。缺少艺术资源时使用清楚标识的当前资源或原创诊断夹具，不伪造苔藓/秋叶贴图。
- 持续实现并提供阶段成果；发现未满足项如实登记，不能把“函数存在”“有 UI 控件”“有一张截图”作为生产完成。

## 2. 当前链路与必须复核的文件

实际已有编辑器与原生视口，不能继续沿用“植被完全没有接入编辑器”的旧判断。

| 层 | 文件 | 当前作用 |
| --- | --- | --- |
| 骨架/网格 | `engine/vegetation/species.mjs` | `buildTree`、`tube`、`card`、`buildMesh`、`compileSpecies`、`buildPreview` |
| 生成器 | `engine/vegetation/generators.mjs` | 默认层级、branch/frond 参数、规范化 |
| 材质 cook | `engine/vegetation/texture.mjs`、`tools/vegetation-textures.mjs` | NGT3、mip、coverage、normal/color/data 过滤 |
| 角色映射 | `assets/vegetation/textures/roles.json` | 源图片到纹理角色 |
| 源物种 | `assets/vegetation/species/`、`tools/vegetation-fixtures.mjs` | 作者数据与默认夹具 |
| 编辑服务 | `editor/vegetation-service.mjs` | list/load/preview/save/cook |
| 页面预览 | `editor/ui/vegetation.mjs`、`.html`、`.css` | WebGL 预览、参数面板、原生视口开关 |
| Electron 桥 | `editor/main.mjs`、`editor/preload.cjs` | 子窗口宿主、管道、IPC |
| 原生预览 | `host/viewport_host.cpp` | 读取 cooked 物种、纹理，创建真实 D3D12 子窗口 |
| GPU 绘制 | `renderer/vegetation_draw.hpp/.cpp`、`shaders/vegetation.hlsl` | 视锥剔除、间接绘制、颜色/阴影 pass |
| 二进制 ABI | `engine/assets/binary-package.mjs`、`engine/native/vegetation/asset.*`、`texture.hpp` | 网格/材质/纹理格式与原生解析 |
| 验证 | `tests/vegetation*.test.mjs`、`tests/native_vegetation*.cpp` | 当前专项覆盖 |
| 历史参考 | `docs/VEGETATION-EDITOR.md`、`VEGETATION-HAIR-STATUS.md`、`VEGETATION-DEFECTS-AND-PLAN.md` | 只作参考，部分结论已落后于代码 |

首个交付应形成当前链路图、读写所有者和已确认/待验证问题清单，然后继续实现。

## 3. 已定位的问题与根因

### G01：子枝被主动移离父枝

`species.mjs` 的 `buildTree()` 目前有：

```js
const start = add(position, mul(childDir, length * 0.08));
grow(start, childDir, ...);
```

这会产生真实的骨架间隙，间隙随着父枝长度增大。删除偏移只能修复中心线断开，不能自动解决曲面接缝。最终必须同时修中心线连接、分叉接合面和风动连续性。

### G02：子枝方向没有在父枝局部坐标中生成

当前 `childDir` 主要由世界水平 `side` 与世界 Y 组成，没有围绕当前父枝切线偏转。于是参数“展开角”与实际父枝夹角不一致，深层枝条仍趋向世界竖直，呈规则扇形/扫帚状。`crownRadiusMetres` 也需要核对是否真正约束树冠；不能让 UI 参数仅被校验而不消费。

### G03：相邻段用独立圆柱，缺少连续扫掠与分叉拓扑

`tube()` 为每个 a/b 段独立调用 `perpendicular(axis)` 构造两圈，段与段不共享几何边界。弯折处圆环方向可突变，产生裂缝、内部相交面和法线接缝。圆周使用 `i < radial` 与最后一面回连第 0 顶点，UV 从接近 1 跳回 0，缺少 seam duplicate，会跨整张纹理插值。

### G04：叶片中心在枝条外随机漂浮

当前按段选 `anchor` 后执行：

```js
card(mesh, add(anchor, mul(normal, offsetScale)), ...);
// offsetScale ≈ 0.05–0.22 m
```

卡片以中心对齐，未保存叶柄 UV/pivot；没有实际叶柄或短枝连接这个随机偏移。因此叶面即便贴图正确，视觉上仍悬空。当前四顶点、统一法线与下垂形变不匹配，也会增强纸片感。

### G05：生成器图的语义没有完整执行

- `parent` 虽保存在数据里，树生成主要按 `byLevel.get(depth + 1)` 递归；不是完整有向父子图。
- `graph.fronds[0]` 只消费第一个叶片生成器。
- `parentLevel` 用 `depth >= parentLevel` 选宿主，与“精确挂在某层”的常见作者理解不同。
- `radiusRatio` 同时被当作段内衰减和子枝基部比例，作者难以分别控制。
- 某些无 steps 的 branch 规范化成 2，和导入/默认规则可能不一致；NaN、重复 ID、无效 parent、缺层和环路需要严格验证，不能只 clamp。

### M01：仍存在具体贴图语义接错

审阅时 `roles.json` 仍包含下列源引用；文件名来自先前生成记录，需以文件哈希复核：

| 配置角色 | 实际图片 | 必须改正 |
| --- | --- | --- |
| `bark_albedo_moss` | `exec-585414c1-...png`，树皮 ORM | 禁止作为苔藓颜色；移除此假变体或替换真实颜色源 |
| `bark_roughness` | `exec-7cf19cc6-...png`，树皮 Height | 使用独立 roughness 或 ORM.G |
| `leaf_atlas_autumn` | `exec-6aec4b36-...png`，叶片 ORM | 禁止当秋叶颜色 |
| `leaf_mask` 及 albedo 的 mask 引用 | `exec-87652e26-...png`，叶片 Thickness | 使用原 Alpha 或独立 opacity；厚度只服务透射 |

可用的完整作者贴图在：

```text
assets/source/vegetation/oak-imagegen-v1/complete/
```

其中已有 `oak_bark_basecolor/normal_dx/orm/height/ao/roughness/metallic.png`，以及 `oak_leaves_basecolor_alpha/normal_dx/orm/thickness/opacity/ao/roughness/metallic.png`。尺寸是 1254×1254，源为 AI 推断原型，非测量 PBR 资产。不得以新名字重复错误用途。

### M02：材质字段没有贯穿完整数据链路

`resolveTextures()` 目前只允许 albedo/normal/roughness/mask；原生 `loadTextures()` 和 `VegetationTextureSet` 只有树皮三张、叶片三张入口。PS 叶片 roughness 取常量，Thickness 取 `constants.material.y * 260`；没有 Thickness、AO、ORM、Height 的完整资源绑定。添加 UI 槽位不能代替 cook、ABI、上传、shader 和调试显示。

### M03：UV 与 TBN 都存在结构性错误

- `barkAlong` 是遍历所有分支的全局累加器，不是每条分支的根到尖弧长。
- `tube` 每段重算切向基，UV 绕轴可翻转；圆周 seam 没有正确复制顶点。
- `card` 的真实 tangent 没写入顶点，shader 用实例 `axisX` 代替叶片切线。
- 图集 row 使用 `1-(row+1)/rows`，但 PNG 上传、D3D 采样和 WebGL flip 约定未形成统一合同；是否实际倒置需用角标诊断图验证，不能仅凭 OpenGL 经验再翻一次。
- 图集无明确每叶 pivot、真实尺寸、padding 和有效内容矩形；全图 mip 可混入邻单元。

### E01：原生子窗口存在，但编辑同步尚不完整

当前 `editor/main.mjs` 启动参数固定 `--species oak_broadleaf`；`preload.cjs` 暴露的接口只发送 viewport 矩形。`host/viewport_host.cpp` 管道控制主要接受 x/y/w/h/v，没有当前物种、spec revision、材质、LOD 或 rebuild ACK。

`vegetation.preview` 响应主要 `viewer.upload()` 到 WebGL。切换为原生视口后，页面上的树和当前修改的数据可能不是同一版本。原生窗已接入这个事实应保留，但必须补内容同步。

### E02：预览与 cook 不完全同源

页面使用程序绘制的 stand-in atlas，原生使用 cooked 纹理；服务 `preview()` 未传入真实 bark tile 规则。`loadSpecies()` 为 legacy 资产注入默认 generators，原已 cooked 资产可能仍走 legacy recipe。两端即使复用 `buildMesh` 也可能生成不同树，必须消除隐式迁移和规则差异。

`requestPreview()` 遇到 `state.busy` 直接返回，可丢掉用户最后一次修改；需要 latest-wins 队列和 revision 关联。

### E03：持久化和生产工具合同不足

save 和 library 更新主要直接 `writeFile`；需补原子写、expectedRevision、并发冲突、撤销/重做和 cook/host 版本一致性。现有接口 `textureRoles: library ? null : null` 是占位，不可用作完整材质面板数据。

### R01：已有部分阴影和 mip，不能继续声称它们完全不存在

当前 shader 已有 `PSShadow`、PCF，纹理 cook 已升级 NGT3 mip，原生宿主也提供光源矩阵。仍需复核 shadow caster 列表、mip 接线、coverageCutoff 一致、法线方向和合成色彩空间；保留已有正确部分。

保存的 `build/viewport-host/shots/oak.png` 可见分段枝干、空隙和成团冠形；只能作为已有快照，不代表执行时实时窗口。请按固定机位重建新的前后对照。

## 4. 实施架构：先显式结构，再派生网格与附件

将当前混在递归中的几何意图拆成可检查中间表示。建议独立模块，具体名称可结合现有代码调整：

```text
normalizeSource / migration
  → evaluateGeneratorGraph
  → BranchGraph（稳定 ID、父子、attachment、曲线、半径、风绑定）
  → validateStructure
  → sweepWood + buildJunctions
  → placeLeaves / clusters（LeafAttachment）
  → bakeUV / normals / tangents / AO
  → buildLOD（身份/轮廓/覆盖约束）
  → cookPackage / previewPackage（共用材质与坐标合同）
  → upload + native content ACK
```

建议 BranchRecord 最少含：

```text
id / generatorId / parentBranchId
attachment: parentArcFraction, localAzimuth, surfaceOffset, junctionId
centerline[], arcLengths[], transportedFrames[], radiusProfile[]
rootFrame / length / materialId / windBinding / randomKey
```

LeafAttachment 最少含：

```text
id / generatorId / hostBranchId / hostArcFraction
hostFrame / petioleLength / orientation / scale
atlasAssetId / cellId / pivotUV / rectUV / sourceLeafSizeMetres
materialId / windBinding / variationKey
```

稳定 ID 来自拓扑身份与局部序号，不由总顶点下标或随机调用次数决定。修改某枝局部参数，不应重排整棵树所有无关叶片。所有 LOD 共享原始 BranchGraph 和 attachment 身份。

## 5. 枝干必须怎样修

### 5.1 父子挂接与生长

1. 子枝根中心从父曲线在 attachment 参数处采样，不能添加无连接依据的空隙。
2. 建立父切线 T、局部 U/V；子方向在该框架中由方位角和偏转角构造，再叠加有界趋光/重力/随机影响，不能每层重新参考世界 Y 生成全新树。
3. 支持沿父枝不同弧长位置分枝、节间距、交互/对生/轮生规则、顶端优势；由 profile 决定，避免所有分叉集中在末端。
4. 半径参数拆开：`segmentTaper`、`childRadiusScale`、`lengthScale`、`branchAngle`。父子半径不突然放大，枝端 taper 与树种轮廓有关。
5. 宽度、树冠包络、枝条高度和分枝稀疏区真实进入求值；对没有消费者的参数禁用或报错，不能装饰性展示。
6. 先用确定性局部生长修好基础形态。Space Colonization 可作为后续冠形/趋光选项，不要求首次修复就重写成单一论文算法。

### 5.2 连续管状扫掠

沿整条 branch 曲线构造共享 ring 的扫掠网格，使用 parallel transport / rotation-minimizing frame 减少扭转；端点、近反向切线有明确退化处理。

- 同一枝的相邻段共享几何圆环，不独立拼任意朝向圆柱。
- 圆周 seam 复制位置/法线相同、UV 不同的顶点；禁止最后三角形从 U≈1 跨到 U=0 插值整张图。
- 半径变化纳入法线，避免所有法线都假定恒定半径圆柱。
- 分叉区域消除内部封口和 z-fighting；树根和末端可显式封口，分叉处不保留内部圆盘。
- 数据保持有界，检查三角形退化、索引、面积、包围体、法线与切线有限性。

### 5.3 分叉接合分两级交付

**第一阶段正确性补丁**：中心线连续，子枝与父枝至少有有界交叠，没有可见空气缝；记录临时结构的局限。此阶段不能标为生产近景完成。

**生产交付**：对分叉局部做 collar/bridge 或局部隐式融合后重拓扑，使外表面连续，无可见内壁、洞和重叠闪烁。根据父子半径和角度选择可验证的稳定算法；不要求整棵树体素重网格。保留独立 branch ID 与风绑定。

分叉 UV 允许明确 seam 和少量局部 chart，几何 seam 复制顶点不等于物理裂缝；要求接合位置一致、法线合理、纹理密度平稳。验收在“按位置与连通规则合并”的拓扑上检查，不错误要求所有 UV seam 顶点焊死。

### 5.4 风动后的连接

静止接上还不够。子枝继承父枝挂点变换，叶柄继承宿主，局部风动在继承变换后计算。接合顶点共享一致变换或权重；不能继续用各层独立平移把接缝拉开。

风强、暂停、时间重置和 LOD 切换保持连续；风速确实改变受力/响应，不以固定正弦中乘任意数冒充物理正确。先保证视觉稳定的有界模型，再逐步增加弹性细节。

## 6. 叶片必须怎样修

### 6.1 挂点以叶柄为原点

给每个图集单元保存 `pivotUV`，一般位于叶柄末端，但必须从实际图片内容确定。局部几何以 pivot 为原点，变换后 pivot 精确落在叶柄/细枝端点。需要离开枝条时生成相应长度的真实叶柄或短枝，不能用中心随机偏移制造悬空叶面。

在挂点局部框架内控制朝向：枝条切线、叶柄方向、叶面法线、roll 与趋光；采用物种叶序，保留受控差异。主叶与叶簇、针叶部件分别建模，不能让 pine 直接使用宽叶规则。

### 6.2 图集有效内容和尺度

图集 metadata 保存 columns/rows、每单元有效 rect、padding、pivot、真实宽高、正反面语义。3×4 只是当前橡树源图的一个配置，不写死在所有物种里。

1260 左右尺寸与非整除 cell 的边界使用规范化 rect 或显式像素 rect 定义；当前源实际为 1254×1254，4 行可能非整数切分，不静默截断。允许重排/重烘图集，但保留内容、关联图与元数据一致性。

用不同颜色和编号的四角/单元诊断图核对 V 方向、半 texel、纹理上传和采样。每个单元的所有数据图同一坐标；不能对 normal、opacity 各自自动翻转。

### 6.3 叶形与法线

近景叶片用有限分段表达弯折、卷曲和主脉，法线来自实际曲面；中远景可用更少顶点。不要采用 `normalize(normal + normal * scalar)` 一类只缩放原法线、实际不改变方向的假曲率公式。

保留正确切线和 handedness；正反面着色采用一致 TBN 翻转约定，不能只翻 N 后让高光方向随机。草 material kind 也走 blade 类，不根据材质数组第 1 项猜 leaf。

### 6.4 不能靠无限增加卡片填树冠

密度按有效枝长、叶面积、物种冠形与预算制定。多个 frond generator 都须独立生效，有明确宿主 selector（精确层级、末梢、范围需可区分）。叶片嵌套子集与 stable ID 用于 LOD，不能降低 LOD 时整棵树重新随机。

## 7. 材质、UV 和“深度贴图”的完整合同

### 7.1 必须分清的材质量

| 项目 | 语义与通道 | 接入要求 |
| --- | --- | --- |
| BaseColor | sRGB RGB，无预烘焙主光；Alpha 可含 coverage | 树皮/叶片分材质，真实颜色图 |
| Opacity/Mask | 线性 coverage，白保留、黑裁掉 | 独立 R 或明确 albedo.A，cutoff 与 mip 一致 |
| Normal | 线性切线空间 RGB | explicit normalConvention、normalScale、TANGENT.w |
| ORM | 线性 R=AO、G=roughness、B=metallic | 植物 metallic=0，shader 正确拆通道 |
| Thickness | 线性相对厚度，需 thicknessScaleMetres | 薄表面透射，厚处一般透光更少 |
| Height | 线性相对表面高度，需 heightScaleMetres | 树皮局部视差/可选离线位移，不负责连接枝干 |
| Scene Depth | 相机空间/投影深度，来自真实深度目标 | 编辑器深度调试、遮挡/阴影相关，不是木头 PNG |
| Impostor Depth | 离线代理每视角相对深度与范围 | 只用于远景代理及深度重建，不与 Height 混用 |

用户提到的“UV 深度等贴图”必须在 UI 中按以上分类命名，避免一个“Depth”槽同时承载四种用途。

### 7.2 源到 GPU 的材质模型

扩展统一材质记录：material ID/kind、颜色/roughness 因子、normalScale/convention、ORM channel、alphaMode/cutoff、thickness/absorption/transmission、heightScale、UV set/transform、sampler、纹理 digest 和 atlas rect。

每个新增字段须贯穿：作者 JSON/schema → validator → cook metadata/sections → 原生解析 → 上传/SRV → C++/HLSL ABI → shader 使用 → 编辑器控件 → debug view → 报告。添加字段却无消费者视为未完成。

优先复用当前材质/纹理基础设施。若植被必须使用专用记录，统一语义并提供转换，避免维护两套相互矛盾的 roughness、颜色和 UV 规则。NGT3 已有 mip，不无理由降级或再造 NGT4；新增格式版本须有明确 ABI 需求和旧包读取/重建策略。

角色定义加入 `semantic`、`sourceChannel`、`colorSpace`、`normalConvention` 等机器可读信息。角色引用需验证语义，而非只验证文件名存在；禁止自动把黄色图判作秋叶，或把高反差灰度图猜成 roughness。

对“苔藓/秋季”目前不存在合格颜色源的情况，删除假角色并修正所有物种引用，或者提供有来源的真实变体。可以使用明确命名的材质 tint 做作者效果，不能把它描述成已提供实拍变体贴图。

### 7.3 UV 与 tangent 实施要求

- 树皮 V=该 branch 的弧长/米制平铺尺寸；父子接合可继承相位，独立枝不能被遍历顺序影响。
- U 沿稳定运输框架的周向参数，接缝显式重复；改变 radial 分辨率不改变主纹理朝向。
- 使用实际顶点 UV 与三角形生成 tangent/bitangent，优先与现有导入使用的约定兼容。切线带符号，处理镜像 UV、负缩放和非均匀缩放。
- 如果短期使用导数 TBN，要覆盖退化 UV 与正反面，并与目标烘焙切线基比较。不能把“八面体 tangent 存 1 float”当作自动解决 handedness；需要明确位打包与 CPU/HLSL 解码一致。
- 文件名 `_dx` 只说明作者意图，不能证明图像符号正确。用合成有方向法线与旋转灯验证；正常绿通道翻转只发生一次。

### 7.4 厚度、AO、粗糙度、Height 的真实使用

厚度通过 `thicknessMap * thicknessScaleMetres` 得到有效厚度，再以吸收/散射近似得到透射。例如光学深度含 `sigma * thickness / abs(N·L)`，对掠射分母限幅，越厚透射不应无条件越强。当前 `thickness * 260` 与常量加亮不能作为最终厚度实现。

AO 作用于间接光或指定微遮蔽项，避免把所有直射和高光统一乘黑。新建独立 AO 通道或明确定义的顶点属性，**不要占用仍被当作 alpha cutout 的 color.a**，否则会误裁叶片。

叶片 roughness 真正读取该叶材质数据；树皮 roughness 不再做未经说明的 `r*0.85+0.2` 二次偏移。材质 factor 与贴图组合方式在 UI 可见，metallic 默认严格为 0。

Height 先完成显式槽位、导入语义、数据预览与小幅树皮视差（若选择 POM，限制步数和掠射退化），必须记录实际启用路径。POM 不改变树干轮廓与真实碰撞；若启用 pixel-depth-offset，颜色/深度/阴影一致性另做专项。大起伏和分叉必须用几何，不能靠 Height 隐藏结构错误。

### 7.5 图集过滤与颜色管理

保留 color 在 linear light 下生成 mip、normal mip 归一化、coverage mip 保持裁切面积等已有能力。修复初始 resize 的 gamma/normal/data 处理，避免先对 sRGB 字节做错误缩小再正确 mip。

图集按单元扩边和生成隔离 mip，或者采用纹理数组。不能跨叶片取颜色平均而引入串色。以最终 cooked mask 和 runtime cutoff 测 coverage；不能保持了错误厚度图的 coverage 就宣称正确。

核对 `dilateIntoMask()`：如果每轮邻居仍只认原 mask，第二轮未必扩散到更远像素。用已知单色/多单元边界夹具证明实际扩边半径，不以“循环两次”自证。

原生 viewport 已有线性半浮点合成说明，必须复核 swapchain、SRV sRGB、曝光、tone map 和最终显示传递函数。不要重复 gamma 校正，也不要用曝光修错误贴图。

## 8. 原生编辑器必须完成的闭环

### 8.1 原生内容协议

保留当前 Electron + 真实 Win32 子窗口架构。扩展受控 IPC/管道，不只是传窗口矩形：

```text
requestId / sessionId / protocolVersion / documentRevision / assetDigest
selectSpecies / applyPreview / loadCookedAsset / setMaterial / setLOD
setCamera / setLighting / setWind / setDebugView / capture
cancelRequest / queryStatus
ACK: appliedRevision / appliedDigest / resourceState / error / stats
```

协议可用有边界长度的结构化消息和临时资源句柄；二进制大网格不要经无限长度行文本。renderer 进程只接受已发布包/受控预览资源，验证路径、版本、payload 长度，渲染线程不执行任意脚本或主线程同步 cook。

支持 A→B→A 切物种、快速连续编辑、过时结果丢弃、取消、宿主重启恢复。页面显示“当前编辑 revision / 已显示 revision / 构建中/失败”，原生 ACK 未到不能标“已经更新”。

### 8.2 共源预览与资源切换

源 spec 先统一迁移与 normalize，再分别生成 preview/cooked；二者使用同一 BranchGraph、材质库、真实图集和米制 UV 规则。legacy 资产不能在加载编辑器时无声变成另一棵树，迁移结果需可预览和保存。

编辑过程允许低质量快速预览，停止拖动后自动提交最终质量。每个请求带 revision，busy 时记录最新待处理输入，不丢最后一次编辑；worker/后台任务隔离长 cook，UI 保持响应。

GPU 上传采用新资源准备 → fence 安全切换 → ACK → 旧资源延迟退休。预览失败继续显示上一有效版本，并标出过期状态；禁止半上传资源直接替换或提前释放 SRV。

### 8.3 调试视图

原生视口至少支持：

- shaded、unlit BaseColor、wireframe、只显示枝干、只显示叶片。
- branch ID/parent、junction、leaf pivot/petiole、宿主 attachment、局部 frame。
- UV checker、UV0、atlas cell/padding、几何法线、切线/handedness、mapped normal。
- AO、roughness、metallic、opacity、thickness、height、camera depth、shadow depth。
- 每个纹理的真实文件/digest、语义、色彩空间、分辨率、mip、通道选择、采样器、占位/缺失状态。

WebGL 作为明确标识的兼容预览；若保留 stand-in 图集，不能把它标作生产材质预览。优先让原生视口完成判定，浏览器近似实现不重复发明另一套物种规则。

### 8.4 生产编辑功能

- 生成器父子树、可靠选择/增删/重排、所有 frond 生效、局部 seed 与 lock。
- 曲线/分枝 attachment、分叉过渡、叶柄/pivot、物种轮廓与叶序可编辑。
- 材质槽、通道、UV/纹理密度、法线方向、cutoff、厚度尺度、Height 尺度可编辑。
- 非破坏草稿、撤销/重做、dirty 提示、expectedRevision 冲突、原子保存和失败恢复。
- 保存、关闭、重开后 spec、结构、材质、seed 一致；撤销后预览 revision 也一致。
- cook 带进度与诊断，构建缓存按源、生成器、材质、图集和工具指纹失效，不无限重用旧 digest。
- 向现有场景资产/事务入口发布物种，做少量实例验证；不为本任务重做通用地形/世界编辑器。
- 切物种、拖参数时相机不反复重置；支持定向、聚焦选中与固定机位截图。

## 9. 渲染正确性与规模接口

现有独立物种视口是单页单物种用途，先把它做正确。区域绘制不得用同一个映射常量地址连续覆盖所有 draw；采用 fence 管理的 per-frame/per-draw 常量环或 GPU 页表。

主颜色、depth-only 和 shadow pass 共用同一静态/风动位置、UV 与 opacity 规则。相机可见列表和光源 caster 列表分开，避免相机外植株不投影。

每实例 LOD 按物种、材质、网格资源正确分桶，不能用固定数组索引判断 leaf。impostor 尚未实现时明确禁用该表示，不用数据已 cook 宣称可绘制。

统计拆分 submitted、candidate、visible、shadowOnly、overflow、triangles 和 resident/inflight bytes。异步遥测延迟读回可用，但不能每帧等待结果再决定 draw。

**禁止将 overflow 写进 `D3D12_DRAW_INDEXED_ARGUMENTS[4]`**：该字段为 `StartInstanceLocation`，会改变实例寻址；使用独立 UAV/telemetry buffer。旧缺陷文档中的这个建议不能照搬。

`discard` 对 early depth 的影响依硬件/编译器而定，不可直接断言所有 early-Z 都关闭。深度预 pass、A2C/MSAA、TAA 按实测选择；MSAA 必须同时匹配 RTV/DSV/PSO/resolve，不能只改 SampleDesc。时域重建先有包含风动的正确 velocity，不能用历史混合糊掉断枝。

## 10. 分阶段执行顺序与文件落点

| 阶段 | 必须实现 | 退出证据 |
| --- | --- | --- |
| P0 基线和语义止血 | 当前代码审计、错误贴图角色修复、固定截图、当前 spec/digest 关联 | 贴图用途表正确，单树错误能重现，保留原始资产 |
| P1 结构中间表示 | BranchGraph、局部父框架、真实 parent、稳定 ID、严格参数校验 | 无孤儿枝/无环，attachment 正确，方向参数真实作用 |
| P2 木质连续曲面 | 运输框架、共享 rings、seam、分叉生产接合、风动继承 | 静止及风动近景无断口、无内面闪烁，UV/tangent 正确 |
| P3 叶片挂点 | pivot/petiole、叶序、物种尺度、多个 frond、叶形和图集 metadata | 叶柄贴枝、叶片不悬空、不跨错 cell，LOD 保持身份 |
| P4 完整材质 | 统一语义记录、各 map 实际消费、调试视图、mip/色彩合同 | sentinel 逐槽验证，粗糙度/厚度/Height 有正确响应 |
| P5 原生编辑同步 | 受控协议、revision/ACK、latest-wins、后台构建、安全换资源 | 编辑/切物种/撤销/重开看到相同版本和材质 |
| P6 生产工作流 | 原子保存、冲突、故障恢复、缓存、场景发布、交互工具 | 无数据丢失，坏包/过时结果不覆盖有效资产 |
| P7 质量与性能 | 多物种多 seed、近中远、风/阴影、专项计时与内存 | 下述验收矩阵全部完成并更新状态文档 |

建议新增 `engine/vegetation/branch-graph.mjs`、`branch-mesh.mjs`、`junction.mjs`、`leaf-placement.mjs`、`material-contract.mjs`、`atlas.mjs` 等职责模块；避免将数千行继续塞入 `species.mjs`。具体拆分服从现有依赖，不增加无消费者框架。

变更包格式与顶点布局时同步 Node pack、SectionKind、C++ reader、renderer vertex、HLSL 输入、shader manifest 和相关工具。旧包要么明确支持，要么报稳定版本错误并提供重建入口，不能 ABI 静默错读。

旧文档保持历史属性，更新当前台账并链接新证据；不能把其中不成立的旧判断整段复制为新报告。

## 11. 必须执行的增量验收矩阵

### 11.1 几何与附件

至少覆盖 broadleaf、conifer、shrub、grass，以及小型诊断 Y 分叉/弯枝/单叶夹具。各类使用固定多个 seed；极端但合法参数也检查。

| 用例 | 通过条件 |
| --- | --- |
| 根/父引用 | 每条非根枝有唯一有效 parent，无环；任何 parent 修改可见地改变其子树 |
| 挂点 | 父曲线采样与子枝根的偏差在预定浮点容差内；以米报告，不用屏幕截图猜距离 |
| 相邻 rings | 按几何 seam 规则连接，无非预期裸边、退化和内部封口 |
| 分叉 | 正侧背近景均无空气缝、相交闪烁或黑洞；UV seam 可存在但不能是物理断口 |
| 叶柄 | pivot 世界点与对应 petiole 端点/枝挂点一致，距离分布可量化 |
| 朝向 | 改父枝方向后子枝/叶片跟随局部框架；枝角参数与测得角相符 |
| 图集 | 角标、编号、cell rect 与真实采样一致，正反面不随机翻转 |
| 重复性 | 相同源/工具生成相同包；局部编辑不重排无关稳定身份 |
| 风动 | 静止及连续至少 300 帧，接合点误差不增长，叶片不脱根、无 NaN |
| 多 frond | 修改每个生成器均只作用于声明宿主；删除某一生成器其他附件保持身份 |

建议小夹具 attachment 容差以 `max(1e-5 m, branchLength * 1e-6)` 起步，结合数值精度冻结。量化后单独报告量化误差；不要为通过测试扩大肉眼可见的断口容差。

### 11.2 材质逐槽“见证”验证

用原创合成数据图替换单一槽，其余固定，验证其真实消费者：

- BaseColor 红/绿分区只改变颜色；数据贴图不按 sRGB 解码。
- Opacity 黑白半区只改变 coverage；Thickness 黑白半区不改变轮廓。
- Roughness 两极值在旋转灯下高光宽度不同；Metallic=0 不产生金属色反射。
- Normal 方向图随叶片旋转正确转动，并验证翻 G 后只有预期方向改变。
- AO 影响约定间接项；默认不把全直射统一乘黑。
- Thickness 梯度在逆光下越厚透射越少或符合冻结模型，正面不变成发光材质。
- Height 开关对局部视差有声明内的影响，不改变无关几何 attachment；Camera Depth 对应真实遮挡关系。
- 每个 debug view 和最终 shaded 读取同一材质记录/纹理 digest。
- 深度、颜色、阴影裁切一致；更改 runtime cutoff 使 coverage mip 失效/重建，而不是沿用旧阈值。

缺贴图回退明确显示和报告；例如临时 flat normal 可以，但不能标作用户法线已加载。

### 11.3 编辑交互与持久化

1. 切换 oak → pine → shrub → oak，原生 ACK 的 species/digest 与 UI 一致。
2. 快速拖动参数后停止，最后一次值必须被应用；旧请求晚返回不覆盖新结果。
3. 修改图集/粗糙度/厚度并保存，重开后渲染同样版本。
4. 撤销/重做同时更新 spec、几何、材质和 ACK；相机保持稳定。
5. 无效参数/坏包/编译失败保留最后有效内容并展示错误，不静默空场景。
6. 重启原生宿主后恢复当前文档、相机和状态，不回退固定 oak。
7. 保存冲突按 expectedRevision 拒绝，不覆盖其他窗口/工具写入。
8. 窗口 DPI、resize、最小化和切页面不产生失控子窗口或已释放资源访问。
9. 正式源文件、临时 preview 和不可变 cooked package 的所有者明确；预览失败不污染正式库。

### 11.4 视觉质量与性能

固定曝光/灯光/相机拍摄：整树正侧背、分叉特写、叶柄特写、树皮 checker、单叶顺光/逆光、阴影、近中远 LOD；包含修复前后与调试通道。

至少覆盖橡树自然冠形、松树主干优势/针叶结构、灌木多干和草 clump。不要让四个 profile 共用一棵树只换高度。植物参考或离线渲染图须注明来源，AI 推断贴图未校准的画质限制单列。

先记录实际机器/适配器、分辨率、三角形、叶片数、draw、mip/纹理与 GPU 显存。单株编辑期间不靠无限放宽预算增加叶片。以匹配预算的 V0/V1 画质为第一门槛，再测 1k/10k 实例。

建议交互目标：轻量参数最终可见延迟 P95 ≤250 ms；需要重拓扑或完整 cook 的任务允许异步，但立即显示状态、可取消、不阻塞 UI。目标属于待测标准，若未达到必须报告原因与任务耗时，不写成已实测。

原生性能使用有效 GPU 时间戳；区分 CPU 生成、传输、上传、GPU 绘制与 Present 等待。参考采样可用 300 帧预热 + 1800 帧，必要时按实际专项范围缩短并注明；禁止仅靠 FPS 文字判断性能。

## 12. 测试、构建与证据要求

只增加能揭示真实错误的测试，不写与实现逐行同构的自证测试。用解析 Y 分叉、已知 UV checker、单叶 attachment、不同材质哨兵图、异步乱序响应与临时文档验证不变量。

现有可复用专项文件：

```text
tests/vegetation.test.mjs
tests/vegetation-editor.test.mjs
tests/native_vegetation.cpp
tests/native_vegetation_draw.cpp
.incrementalTesting/vegetation/vegetation-hair-verify.ps1
tools/viewport-host.ps1
```

先检查脚本是否只覆盖本次依赖，必要时拆出更窄的命令或新增 `tools/vegetation-production-verify.ps1`，参数和执行范围写清楚。涉及资产导入时才增加相应资产专项；不自动运行完整毛发/全工程测试。

新增证据放 `reports/vegetation-production/`，最少：

```text
audit.md                         当前基线、确认根因、未确认项
geometry.json                    graph/接合/挂点/UV/TBN 统计
materials.json                   源文件、语义、通道、digest、GPU 槽与生效证据
editor-sync.json                 request/revision/ACK/取消/乱序/恢复
performance.json                 机器、帧时间、交互延迟、工作量与内存
images/                          固定机位前后图、各材质调试图
acceptance.md                    本文件逐项映射到通过/失败/未执行
```

报告保存实际执行命令、二进制/源码指纹、shader 和包摘要、时间、机器和文件路径。缺测用 null/unavailable，不用 0。历史截图不冒充新测试，未执行不能记 pass。

## 13. 禁止的“假完成”与危险修法

- 只删除分支偏移，仍留独立圆柱断缝，就称生产级接合完成。
- 让叶片中心贴枝但叶柄仍悬空；用更多随机叶子遮挡错误。
- 把 Thickness 当 Opacity、Height 当 Roughness、ORM 当秋叶/苔藓颜色。
- 只在 WebGL 更新，原生窗口仍显示固定 species 或旧 digest。
- UI 新增槽位但 cook/C++/HLSL 无消费者；报告仍填写占位文字。
- 全部叶片复用实例 X 轴作为 TBN；仅通过改法线强度隐藏方向错误。
- 将 AO 写入仍参与 alpha test 的 color.a；将 telemetry 填入 DrawIndexed 参数保留字段。
- 双重翻 V/绿通道/双重 gamma；用曝光修错误颜色空间。
- 把平铺接缝、骨架断口、alpha 裁切错误都归因于“AI 贴图不够好”。
- 为了 golden image 通过删除失败视角、换成无枝条视角或放宽阈值。
- 以几十万断言数量、30M 容量公式或单张缩略图替代连接、材质和编辑闭环验收。
- 未经验证承诺完整 UE/Nanite 生产能力或固定高帧率。

## 14. 最终交付格式

完成实现后给出：

1. 主要根因及实际修改文件，解释为什么树枝和叶片现在结构正确。
2. 一张“材质输入 → 通道/色彩空间 → cooked digest → GPU consumer”的实际映射表。
3. 当前 spec revision 与原生 applied revision/digest 一致的证据。
4. 修复前后整树、分叉、叶柄、树皮 UV、法线与逆光厚度截图。
5. 已运行增量命令及结果，机器与性能数据，未执行项和限制。
6. 更新 `docs/VEGETATION-HAIR-STATUS.md`、`docs/VEGETATION-EDITOR.md`，将旧缺陷分析标明过时/已修复，不删除历史证据。
7. `reports/vegetation-production/acceptance.md` 对 P0–P7 和所有验收项逐项给结论。

请持续实施至本任务的生产编辑闭环达到验收。若某项实际未实现，明确保留未完成状态和影响，不以“后续可做”作为全部工作已完成的结论。

## 15. 技术参考及使用边界

以下是作者/官方一手资料，作为结构或语义依据，不要求照抄完整系统：

- Runions、Lane、Prusinkiewicz，2007：[Modeling Trees with a Space Colonization Algorithm](https://www.algorithmicbotany.org/papers/colonization.egwnp2007.html)。参考树冠空间、生长方向与趋性；可作为可选生成器，不代替当前连接错误修复。
- Khronos：[glTF 2.0 Specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)。参考材质线性/sRGB、法线、切线和 ORM 相关通道语义；以实际 glTF normal 约定转换，不从 D3D 名称推导图片符号。
- Epic：[Shading Models in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/shading-models-in-unreal-engine)。参考薄叶片的双面光照和透射语义；项目应实现自己的明确近似与校准，不宣称完全复制 UE。
- 本仓库：[总体开发计划](VEGETATION-HAIR-DEVELOPMENT-PLAN.md)、[现有贴图说明](../assets/source/vegetation/oak-imagegen-v1/complete/README.md)、[编辑器现状](VEGETATION-EDITOR.md)。总体计划中的百万/千万级验收继续独立，不因本次单株编辑器完善就提前标完成。
