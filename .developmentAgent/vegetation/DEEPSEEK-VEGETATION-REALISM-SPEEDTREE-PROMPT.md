# DeepSeek 优化提示词：SpeedTree 类真实树木生成与现有资产/材质系统融合

- 日期：2026-10-26。
- 工作区：`C:\Users\34119\Documents\AI-Native Game Engine`。
- 阶段：上一轮生产编辑闭环之后的**形态真实性与混合资产制作升级**。
- 使用方式：将本文完整交给可以操作该工作区的 DeepSeek 编程代理。要求实际实现、生成资产、连接现有系统并给出证据，不能以另一份建议文档替代交付。
- 本文依据：当前代码、`reports/vegetation-production/acceptance.md`、已有树木截图与官方/论文资料。此次编写仅静态审阅，未运行新构建/测试；历史验收表中的通过状态不自动代表照片级画质。
- 与上一轮关系：[上一轮修复提示词](DEEPSEEK-VEGETATION-PRODUCTION-PROMPT.md) 中已实现的 BranchGraph、连续扫掠、叶柄挂点、材质通道、撤销/重做与原生同步应保留。本轮不是从零重写，也不能继续把已经修好的旧问题作为当前结论。
- 强制补充：[AI 贴图、自动导入与程序化损伤规范](VEGETATION-AI-TEXTURE-AND-DAMAGE-SPEC.md)。根部朝下、叶尖朝上、固定带安全留白的 pivot；虫洞/虫咬/病斑/树皮贴花作为独立程序化层。执行时同时落实该规范 T0–T6，不可只写提示词而不接入导入、材质与原生视口。

## 1. 你的目标

你是植物程序化建模、计算机图形学、资产管线与 AI 创作工具的实现工程师。请将当前“结构正确但仍像算法示意树”的植被生成器，提升为能制作可信树种、近景英雄树和批量自然变体的创作系统。

用户明确希望：

1. 获得 SpeedTree 类的程序化生成、局部手工控制和高质量资产复用能力。
2. 可以用自然语言、参考图和已有模型帮助生成极其真实的树。
3. 树干、枝条、根部、树叶能够正确展开/部署 UV，使用法线、粗糙度、AO、厚度、高度/视差等贴图。
4. 实际调用项目正在开发的通用材质系统、外部资产导入系统、环境光照等有用能力。
5. 原生视口、编辑源、材质实例、外部组件、烘焙包和游戏发布保持一致。

这里的 SpeedTree 类指作者工作流与能力方向，不要求读取专有 `.spm/.st9` 数据、不复制专有代码，也不声称在一次改动中达到所有商业软件功能。原生 PBR 通道可用、几何连接正确、断言数量多，都只是基础条件；必须进一步证明树种轮廓、尺度、枝条节奏、根干细节、叶簇与光照真实。

## 2. 已有成果：先复核，再增量升级

当前已经存在：

- `engine/vegetation/branch-graph.mjs`：稳定 ID、显式 attachment、父坐标系、运输框架。
- `branch-mesh.mjs`：连续枝条扫掠、UV seam、局部分叉 collar。
- `leaf-placement.mjs`、`atlas.mjs`：叶柄、pivot、曲面叶片和图集 metadata。
- `material-contract.mjs`：BaseColor/Normal/ORM/Height/Thickness/Opacity 与语义约定。
- `editor/vegetation-service.mjs`：preview/save/cook/undo/redo/publish，以及 revision 与缓存。
- Electron 原生 viewport 的内容同步、ACK、调试显示和资源切换。
- per-cell mip、语义贴图处理、若干原生材质见证、风动和时间戳证据。

入口资料：

```text
reports/vegetation-production/acceptance.md
reports/vegetation-production/geometry.json
reports/vegetation-production/materials.json
reports/vegetation-production/performance.json
reports/vegetation-production/images/species-oak-shaded.png
reports/vegetation-production/next-steps.md
docs/VEGETATION-EDITOR.md
```

先记录哪些能力在当前 HEAD/工作树实际可用。验收文档存在更新不一致、旧统计混杂的可能，必须读取代码和对应摘要，不通过搜索“通过”决定完成度。

### 2.1 本轮审阅发现的真实性限制

以下是应优先核对的具体证据，不是要求不经判断照改：

| 现状 | 真实性影响 | 下一步 |
| --- | --- | --- |
| `branch-graph.mjs` 多数生成仍按 level、固定 count/区间、简单 upBias/droop | 拓扑重复、同层枝条节奏相似，缺少生长历史和局部竞争 | 引入树种/年龄规则、局部生长空间和层次化变异 |
| 椭球 envelope 对超界预测点做硬投影与高度 clamp | 末梢堆在包络上，枝条出现不自然拐折、齐顶和规则外壳 | 用软约束、抑制芽/裁枝、局部趋光替代默认硬裁剪 |
| clamp 后 `arcLength` 仍填 `stepLength * (step+1)` | 真实曲线长度与挂点、UV、密度采样可能不一致 | 从最终点差累积真实弧长，区分目标 length 与实际 length |
| 根干仍主要是圆截面扫掠，半径近似线性衰减 | 像圆管，缺少根颈、板根、偏心、分叉隆起和自然树皮尺度 | 扩展截面/根部/接合细节并支持扫描部件 |
| collar 覆盖父子交界 | 可无空气缝，但不能据此证明为单一无自交流形 | 对英雄树增加真实接合/局部重网格与外观验收 |
| `placeLeaves()` 按 host.length 线性计数，最多 200/branch | 叶片均匀铺在枝条上，容易像羽毛/刷子；缺少短枝、芽、节间和叶团空隙 | 引入 Shoot/LeafCluster，按叶序与有效叶面积决定密度 |
| `bladeWidth()` 使用通用正弦形叶片轮廓 | 不同图集叶形可能被通用网格二次裁切，树种叶形趋同 | 用 alpha contour/cutout mesh 或导入叶网格，实测纹理覆盖损失 |
| 专用 `vegetation-material/1` 与 MIR/通用材质接口分开 | 通用材质升级不能自然作用于树，易产生第二套光照/UV/缓存合同 | 通过公共材料接口和薄叶 shading model 建立适配 |
| `vegetation.hlsl` 仍有 sky/ground ambient、height-based `depthTint` 等专用近似 | 环境与主场景不一致，冠内缺乏合理空间遮蔽，材质显塑料 | 复用现有环境、光照服务，保留明确低档回退 |
| 当前 AI 贴图为分张推断原型 | 配准、光照残留、真实粗糙度/厚度不保证 | 引入扫描/作者资产与统一烘焙、校准和来源标签 |
| 现有性能证据记录单 LOD 1k/10k 负载 | 增加几何会放大成本；高面数不能当画质证明 | 多表示、实例 LOD、叶簇简化与真实性预算同步设计 |

已有截图中的橡树仍能辨认出重复细枝和细碎叶带，树冠的主要空隙、枝团比例与成熟橡树参考差异明显。这属于形态和尺度问题，不能仅靠调色/阴影修复。请在新基线中用相同镜头重现，勿把历史截图当实时界面。

## 3. 必须采用的总体路线：程序规则＋真实部件＋可控 AI

实现三种可共存的制作方式，输出同一个 TreeAsset：

### A. 完全程序化

树种描述 → 骨架与短枝 → 根干/分叉曲面 → 叶簇 → UV/材质 → LOD/风绑定。用于不依赖外部资源的基础树、批量变体与可解释编辑。

### B. 扫描/作者部件混合（优先高真实感路线）

外部导入的扫描根干或手工树干 → 标记插接 socket/参考曲线 → 程序化粗枝/细枝 → 导入叶片/枝叶簇 → 统一材质实例与烘焙。保留扫描根干的轮廓、UV 和可信细节，同时具备枝冠程序化编辑。

### C. AI 辅助创作

自然语言或参考图片 → 结构化 TreeRecipe/局部 edit operations/候选参数 → 确定性求值 → 校验 → 原生候选预览 → 选择并保存。可使用 AI 生成纹理原型或外部三维生成结果，但必须通过现有资产导入和验证，不能绕过资产语义。

AI 负责解释目标、检索/选择部件、提出参数与局部编辑；几何、UV、绑定、单位与可复现性由确定性工具负责。没有接入可用模型服务时，程序化与导入两条路线照常可用，并明确 AI provider 未启用，不能用固定模板输出冒充模型推理。

## 4. 建立真实的树种与生长描述

### 4.1 SpeciesProfile 与 TreeRecipe

引入版本化可序列化的树种 profile，至少区分：

- 树种/俗名/可选学名、成熟度与年龄区间、健康状况、孤立生长/林内竞争/修剪风格。
- 树高、胸径、冠幅、枝下高、冠形纵剖面、主干延续或分叉方式。
- 主枝方位与夹角分布、节间/轮生规则、枝序、短枝/长枝比例、顶端优势。
- 重力下垂、向光、避障、向空间生长、枯枝/落枝、受损再生与局部缺叶。
- 芽/节/短枝的身份、叶序、叶面倾角分布、单叶真实尺寸、叶面积目标。
- 根颈宽度、根分布、偏心截面、树皮纹理方向与年龄相关粗糙度。
- 环境约束、局部 art overrides、材质实例集合、允许使用的外部部件集合。

profile 是相关参数与分布，不是把所有参数独立 uniform 随机。成熟度改变根干、枝条粗细和树冠结构，不能只缩放同一棵树；同树种的变体必须共享身份特征。

第一批使用有明确形态参考的三类：阔叶成熟树、具有顶端优势的针叶树、多干灌木。对每类挑选明确物种作为 hero preset，现有橡树素材可作为第一套；不要把所有阔叶默认叫同一种 oak。

### 4.2 生长策略可组合

以当前 BranchGraph 为派生骨架，新增生长策略接口：

1. **规则式生成**：interval、phyllotaxis、bifurcation/leader、along-parent profile curves。先完成可控、快速、可编辑的作者工作流。
2. **空间占据/吸引子**：Space Colonization 或等价空间竞争，用于冠形、空隙与趋向分配；使用空间索引避免 O(branch×attractor) 全量扫描。
3. **简化生长竞争**：光/空间资源影响芽的存活、长度、粗细与自疏。参考自组织树论文，实现可控近似，不要求完整生理学模拟。

策略只改变源结构与生长状态；材质、附件、LOD、编辑 identity 共用。允许混合：主干与一级枝人工锁定，外围短枝自动生长。

### 4.3 宏观/中观/微观三层控制

| 层级 | 必须看得见的控制 | 应避免 |
| --- | --- | --- |
| 宏观 | 冠幅/枝下高/偏冠/主干分叉/主要负空间/年龄 | 同一椭球外壳加随机 seed |
| 中观 | 一级枝、次枝、叶团、短枝、局部稀疏和枯枝 | 每层完全相同子枝数和细碎均匀叶带 |
| 微观 | 根皮、枝瘤、叶柄、叶形、虫咬、卷曲、叶脉 | 在错误大轮廓上堆噪声或数十万叶片 |

按照宏观 → 中观 → 微观校准，只有上一层过关才增加下一层细节。

### 4.4 自然变化需要相关性

用低频、空间相关的变化控制弯曲、枝团方向、局部年龄/光照；高频噪声只用于小尺度细节。对同一 shoot 的叶子建立尺寸/颜色/朝向相关性，树冠外/内和新/老叶允许不同分布。

在固定种子下保存主枝和叶团身份。局部增叶不会使整棵树换图集；局部修剪只更新受影响子树与预算，不全树重新抽签。

## 5. 根干、分叉与树皮几何提升

### 5.1 根颈与截面

增加根颈 flare、若干主根、地面接触展开、偏心与低阶截面扰动。扰动尺度与树种/胸径相关，保留根/枝 socket 与运输框架。树根支持 SurfaceSampler 的平面或当前可用地表适配器，不能假定完整 terrain 已存在。

每条 branch 的 radiusProfile 用显式曲线表达；使用有依据的父子粗细约束或经验 pipe-like 关系作为可调先验，不能把某个半径指数宣称为所有树种的生理真值。主干、一级枝、末梢的弯曲与锥度分别调。

### 5.2 接合、枝瘤与树皮过渡

保留现有 ring/frame 正确性，对近景接合增加连续表面、枝领/分叉隆起、局部过渡 UV 与法线。collar 叠加“看不到洞”不自动意味着无内部面；诊断图区分是否只是视觉接合。

优先实现局部 stitch/trim/重网格；不将全部扫描树干体素化以致丢失原始细节和 UV。外部部件的接合区允许局部烘焙纹理，保留其他区域的原纹理。

### 5.3 层次细节

树皮大隆起/根部/枝瘤影响轮廓，用几何或离线位移；小裂纹用法线；浅视角可用受限 Height/POM。明确作者级高分辨率源和游戏级优化网格，不能用 POM 冒充真实根干形状。

Hero 树与普通实例允许不同几何预算、材质 LOD 和树皮细节级；预算以实际轮廓收益与屏幕像素评价，不预设越多越真。

## 6. 叶片、短枝和叶簇的真实层次

新增 `ShootAsset` / `LeafClusterAsset`，不要只在粗 branch 上放单叶。

```text
一级枝 → 次枝 → 末梢短枝/当年生枝 → 芽/节/叶柄 → 叶片
                                  ↘ 可复用真实枝叶簇
```

每个部件保存 pivot、方向、真实尺寸、socket/宿主范围、材质引用、局部风绑定、bounds 和 LOD；区分针叶簇、宽叶枝簇和单叶。使用当前外部资产系统导入手工/扫描 branchlet，不靠在所有表面均匀放叶子填冠。

### 6.1 摆放与冠层空隙

- 叶序和叶片朝向按物种与短枝局部轴，日照/重力/风影响受控。
- 根据有效叶面积、枝条年龄、枝径、光照与最小间距决定可挂叶区域；树干和粗枝通常不直接密铺大叶。
- 支持叶团边缘和内部不同密度，以及确定性的局部缺叶/空隙。检查枝团尺度而不是只有全树 coverage。
- 用空间网格/近邻约束限制严重叶叶穿插，允许符合物种的适度重叠；不以“0 重叠”制造稀疏盆景。
- 叶片基部继承真实 petiole pivot，风动、弯曲、局部剪枝后仍连接；叶簇不能像独立花球粘在树上。

### 6.2 几何叶形与图集一致

提供三种来源：从 alpha contour 生成 cutout mesh、导入现成叶网格、完整图集矩形曲面卡片。生成的网格应覆盖真实图像轮廓，不能在 alpha 外再套通用正弦杏仁形导致橡树裂叶被切掉。

轮廓抽取保留叶柄、明显裂叶和必要虫孔，简化容差与三角预算可调；小孔可由 alpha 表达，不强制挖成碎网格。UV 保留原 texel 对应，叶脉法线和几何主脉对齐。

单叶、叶簇、针叶部件可分别烘焙 normals/AO/depth，保留实际尺度。对正反面颜色/粗糙度差异提供可选属性；不得通过重复翻 TBN 伪造背面。

### 6.3 远景外观保持

近景用细叶/枝簇，中景用少量弯曲 card/cluster，远景使用有深度的 impostor 或现有已验收表示。几何、coverage、transmission 与高光统计同时评估。

现有“留面率”能暴露简化损失，但不等于轮廓/叶面积保持。新增多角度 coverage、空隙分布和深度对照，选择 LOD 切换阈值；禁止纯随机删 70% 叶子后只凭距离误差称冠形连续。

## 7. 复用外部资产导入：建立可编辑部件库

### 7.1 当前可用入口，必须先读实现

```text
engine/assets/import-service.mjs
  inspectAsset / importAsset / verifyImport / listImports
  attachModel / attachTexture / attachEnvironment
engine/assets/model-import.mjs
  readStaticModel / cookModelMaterials / cookStaticModel
engine/assets/coordinates.mjs
engine/assets/material-source.mjs
engine/assets/tangent-generation.mjs
engine/assets/texture-decode.mjs
engine/assets/texture-coordinates.mjs
tools/assets.mjs
docs/EXTERNAL-ASSETS.md
```

模型/纹理/环境已有导入、单位轴向转换、材质解析、贴图与不可变资源身份。通过新增**植被部件适配层**连接这些接口，不重写另一套 glTF/OBJ/FBX、图片、HDR/EXR 解码器。

当前 `importAsset` 等函数的参数和输出以源码为准；本文列的是审阅时实际存在的符号，不保证后续迁移不变。不得杜撰 connector 或没有实现的 API。

### 7.2 ComponentAsset 合同

```text
schemaVersion / componentId / importedAssetDigest / source provenance
kind: rootTrunk | branch | twig | leaf | needleCluster | leafCluster
localBounds / metresPerUnit / canonicalAxes
meshRefs / materialRefs / uvSets / tangentConvention
pivot / forwardAxis / attachmentSockets[]
optionalCenterline / deformationCage / windBinding
variants / LODs / authoringOverrides / bakeDependencies
```

导入后编辑器可指定 pivot、两端 socket、长度方向、枝条曲线与变形约束，预览实际测量尺寸。扫描干没有自动骨架时提供手工曲线，不把“文件已导入”宣传成已自动恢复可程序化生长的拓扑。

### 7.3 扫描根干的接枝

优先支持保形的静态扫描根干＋在 socket 生长枝冠；随后可增加中心线/cage 变形。变形保留 UV，正确更新几何法线/TBN、bounds、碰撞和风绑定。

需要恢复中心线时，可用确定性近似/交互描线；有真实模型服务才提供 AI 建议，并用多个截面/表面贴合和用户可修正的 confidence 表达。根干不应因套入标准圆管而失去照片级细节。

导入整树支持“作为完整不可分解资产”与“人工分段转组件”两条模式。无法恢复的父子层级明确标 static/imported，不伪造 BranchGraph。

### 7.4 资产身份与发布

引用 immutable digest，局部覆写形成新版本。保留来源、许可、转换参数与工具指纹；不修改外部原文件。批量替换部件或贴图是可撤销事务，影响 cook 指纹与候选身份。

导出/发布复用现有 `vegetation.publish`、场景事务与 imported asset 依赖收集，明确哪些是作者图、运行时网格、材质实例、纹理与外部源，不允许场景只在开发机的绝对路径可用。

## 8. 调用现有材质系统，而非再做一套孤立材质

### 8.1 当前接口与实际限制

复核以下接口及其调用者：

```text
engine/material/mir.mjs
  materialToMir / validateMir / mirHash / compileMir
engine/material/graph.mjs
  validateGraph / compileGraph
engine/material/texture-mapping.mjs
editor/service.mjs 与既有材质实例编辑动作
tools/material.mjs / tools/textures.mjs / tools/bc.mjs
renderer/d3d12_renderer.*
shaders/material.hlsl / lighting_quality_common.hlsli
```

当前 MIR/Graph 是有限类型/节点的编译管线，不是任意 HLSL shader graph。已有 base/normal/ORM/occlusion、贴图映射、coat 等能力；植被的 Thickness/Height/薄叶特性不应假定已被 MIR 全部支持。

输出一份**实际适配矩阵**，逐项写：当前谁校验、谁 cook、谁上传、哪个 shader 消费、缺口如何最小扩展。优先统一材质引用/实例、纹理语义与缓存，再抽取共享 surface evaluation/光照函数；不要要求先完成一整套通用节点编辑器才能画树。

### 8.2 MaterialRef 与薄叶模型

TreeAsset 和 ComponentAsset 引用通用 MaterialAsset/MaterialInstance 的 ID/digest。保留植被作者参数，但通过适配形成同一底层材质：

```text
MaterialRef → resolve canonical material/MIR → surface model
  surface model = standard | thinFoliage
  geometry inputs = UV / tangent / normal / vertex color / stable instance data
  vegetation inputs = leaf thickness / wind / atlas slice / season masks
```

`thinFoliage` 是明确的扩展语义，兼容标准 PBR 材质；不把整个通用 renderer 的所有 opaque 都改成叶片。旧 `vegetation-material/1` 提供迁移/适配，不立即删除用户资产。

材质/图节点扩展同步 Node validator、MIR/Graph、C++ 上传、ABI、shader、UI、报告和缓存。未支持输入报错，不静默丢弃。并行材质开发的活动接口须先检查，新增适配器尽量局部落地。

### 8.3 UV 与贴图要求

| 输入 | 作者合同 | 几何与渲染要求 |
| --- | --- | --- |
| BaseColor | sRGB，去光照色；植物不应涂入场景投影 | 共享颜色空间，支持局部 tint 而非写死乘色 |
| Normal | 线性，明确 +Y/-Y 和 tangent basis | UV/TBN/非均匀缩放/背面变换正确，支持 detail normal |
| ORM | R=AO、G=roughness、B=metallic | 与通用材质约定一致，植物金属度为 0 |
| Opacity | 线性 R 或显式 albedo.A | MASK、按单元隔离 mip、cutoff 与 cook 一致 |
| Thickness | 相对厚度＋米制 scale | 薄叶消光/透射，厚度不负责裁切 |
| Height | 相对表面高度＋米制 scale | 树皮视差或离线位移，不能替代主干轮廓 |
| Detail/Blend Mask | 线性控制数据 | 树皮层、苔藓/地衣/湿润/伤痕的局部控制 |
| Impostor Depth | 带视角与深度范围的代理深度 | 专用远景重建，与 Height/Camera Depth 区别显示 |

树皮 V 由最终几何的真实弧长得到。扫描部件保留原 UV；程序枝采用弧长/周向参数、seam 与可选接合 chart。支持局部 UV 旋转、tileMetres/texel density、seam 显示、UV checker 和材质球/枝干对照。

HeightScale 为米时，POM 转 UV 要通过局部表面度量/纹理实际 tileMetres；不能直接把米作为 UV 偏移。离线 displacement 后重建 normals/tangents；POM 不改变真实碰撞和大轮廓，调试视图明确当前路径。

### 8.4 层材质和环境表现

通过现有 material layers/graph 能力支持基础树皮＋局部苔藓/地衣/湿润/伤痕；新增层输入只限本任务必需。normal 混合采用保持方向的方式，roughness 按声明模型混合，不简单 alpha 叠一张彩色照片。

叶片的蜡质高光、正反面差异、叶脉/厚度、轻微根尖色和年龄变化分别控制。不得用随机 RGB 噪声或全局高饱和绿色制造假变化。

优先复用已有环境导入、天空 SH/预滤波反射、主光/局部灯和可用 GI/shadow 服务。以能力查询决定哪些在当前 viewport 可用。当前植被专用 ambient/heightTint 只作为明确回退，不能与真实环境/GI 重复乘光。

提供同一材质在通用静态资产、程序树皮和导入树干上的对照。相同 UV/法线/灯光/表面模型时结果一致，薄叶例外通过显式 shading model 解释，不以“差不多”验收。

## 9. AI 创作能力：实际接线与可信边界

### 9.1 分层输出

AI 可输出：

1. **TreeIntent**：树种、年龄、形态、环境、风格、预算与参考资产选择。
2. **TreeRecipe**：有效 profile、生成器图、部件集合、材质引用与 seed。
3. **EditOperations**：仅对选中子树剪枝、弯曲、换皮、换叶簇、局部增减密度等操作。
4. 可选图像/模型来源：经资产管线导入的纹理或三维部件，并记录 generator/provider/source。

不默认让语言模型逐顶点写几十万顶点，也不把单张生成图当作多视角一致的 3D 树。

### 9.2 API 与可用模式

先检查 `ai/tool-service.mjs`、现有编辑器请求层和已配置模型调用路径。复用其事务、诊断与授权，新增有边界的树创作工具，不把 API key 放入网页或源资产。

建议工具动作（名称最终按现有服务规范）：

```text
tree.describe_capabilities
tree.generate_recipe(intent, allowedAssets, constraints)
tree.preview_candidates(recipe, seeds, budget)
tree.edit_selection(assetRevision, selectionIds, operations)
tree.validate_recipe / tree.publish_candidate
```

提交的 recipe/operations 严格 schema、有限范围、无任意脚本和路径；先 preview/diff，再通过现有编辑事务应用。提供 revision 冲突处理、取消/超时、失败保留当前树、错误反馈与重试上限。

模型服务未配置时交付 adapter、schema、调用测试与明确 disabled 状态；程序化 recipe 编辑和候选预览可使用规则 parser，但必须标“规则模式”，不能说是真实 AI。不要为通过演示悄悄新增收费远端服务或下载大模型。

### 9.3 参考图与候选选择

用户参考图作为外观目标，拆解轮廓、分叉高度、冠幅、叶团尺度、干皮类型和光照；保存目标置信度。单视角无法确定隐藏枝条，系统生成可编辑的合理结构，并用 360° 预览校验，不能承诺精确复原。

支持例如 4–8 个有限候选，使用同曝光的正/侧/背/俯视缩略图与预算报告选择；整树、仅枝干、冠层 mask 三种视图辅助判断。候选多样性应改变有意义形态，不只是 seed 引起细叶抖动。

AI 自动评价可作为排序建议，不能独自宣布照片级；几何/材质/可编辑性检查与参考对照仍是硬门。

### 9.4 纹理生成和高低模烘焙

当前 imagegen 原型可保留，不将其全部当作测量材质。优先接入同一扫描/高模的 BaseColor、Normal、Height、AO 与 roughness source；高到低烘焙需 cage、法线基和偏移配置，并提供错配诊断。

AI 生成的 albedo/normal/thickness 独立出图只作为 authoring 近似。需要 production 标签时验证轮廓配准、seam、物理尺度、颜色空间与旋转灯响应；无法校准的项明确标 inferred，不伪造“扫描厚度”。

不要求本工程立即拥有完整扫描或路径追踪器：可使用现有 Blender/DCC 离线 worker 或可验证的外部工具适配，记录版本和产物；先确认工具存在，命令失败给出可定位错误。

## 10. 面向作者的编辑器升级

保留当前原生 child viewport 和 revision/ACK。将已有参数 UI 提升为可创作的树工作区：

- **生成器图/层次树**：Trunk、Branch、Root、Shoot、LeafMesh、LeafCluster、ImportedPart、Force、Prune/Mask、LOD/Bake。
- **Profile curves**：长度、半径、角度、分布、密度随父弧长/枝序/年龄变化，曲线是真实消费者。
- **局部编辑**：选中枝条/部件、锁定 seed、拖弯控制点、修剪/恢复、局部 regenerate、绘制生长/排除区域。
- **部件库**：从外部资产库选择、测量尺寸、设置 pivot/socket/轴、材质和 LOD，查看导入来源。
- **材质工作区**：调用现有材质实例编辑器，UV/贴图拖放、通道预览、材质实例覆写和 hot reload。
- **AI 面板**：自然语言目标、参考图、候选、差异预览、局部操作与来源/限制。
- **质量诊断**：silhouette、leaf clusters、branch order、年龄、叶面积、负空间、材质 ID、UV/TBN/各贴图、overdraw/LOD。
- **版本与发布**：草稿、撤销/重做、局部编辑与 AI 编辑同一操作历史，保存/重开、依赖收集与场景发布。

预览/最终 cook 使用同一 normalized recipe、组件库和材质 resolver，预览降质必须明确标识。latest-wins 保留，视口 ACK 携带树和材质/组件依赖摘要；不能只几何更新而材质仍旧版本。

不要先重做编辑器主题或引入巨大通用节点框架。每项 UI 先连真实求值/渲染，再完善操作体验。

## 11. 实施工作包与关口

| 阶段 | 实际开发内容 | 最少交付证据 |
| --- | --- | --- |
| R0 真实性基线 | 当前代码/资产/报告审计、真实参考板、固定曝光和预算 | 当前与目标多视角差异，确认/怀疑分开 |
| R1 形态与尺度 | SpeciesProfile、profile curves、真实弧长、软空间约束、相关变化 | 3 类物种 silhouette/枝架与多个 seed |
| R2 生长与叶簇 | shoot/芽/叶序、负空间、leaf area、可选空间竞争 | 不靠增加面数显著改善枝团/冠形 |
| R3 近景几何 | 根颈/偏心/枝瘤/接合、alpha cutout 叶形、作者/游戏双网格 | 根干、分叉、单叶近景与 TBN/UV 证据 |
| R4 外部部件集成 | 使用现有 import API、socket/curve/pivot、扫描根干＋程序冠 | 至少一棵真实混合树，可编辑/保存/发布 |
| R5 统一材质与光照 | MaterialRef/MIR 适配、薄叶模型、贴图/层材质/环境复用 | 同材质跨通用资产和植被一致性、逐槽见证 |
| R6 AI 与作者工具 | 已配置 provider 接线、结构化 recipe、候选/局部编辑、取消/撤销 | 实际调用记录或诚实 disabled 状态，操作可复现 |
| R7 运行时预算 | 多表示 LOD、GPU 分桶/页数据、叶簇简化、缓存与纹理预算 | 不再 10k 全 LOD0，真实计时/计数 |
| R8 最终资产集 | Hero preset、变体库、同机位参考对照、依赖发布 | 下述验收清单逐项有新证据 |

R4/R5 的基础适配可与 R1–R3 交错推进，但先查明接口再修改。若用户重点是照片级英雄树，优先交付“扫描/作者根干＋真实枝叶部件＋统一材质”的垂直切片，不用等待完整生长模拟。

R7 不能因为画质升级无限推后：本轮新增的细节必须有对应中远景表示。性能不足时减少不可见/亚像素工作，而不是在所有镜头一刀切降低主角画质。

## 12. 必须证明的质量，而不只是“无错误”

### 12.1 资产验收集

1. 一棵完全程序化的成熟阔叶 hero 树。
2. 一棵完全程序化的针叶 hero 树，主干优势与针叶部件可辨认。
3. 一棵导入扫描/作者根干＋程序化枝冠的混合 hero 树。
4. 每个 hero 至少 6 个固定 seed 变体，保持同树种身份而非相同剪影。
5. 如已配置 AI，至少 3 个自然语言任务：整树生成、局部偏冠/修剪、保几何换材质；未配置则单列未执行，不伪造日志。

预设源、材质实例、组件依赖与许可/来源可追溯。不强制大量新素材先入库，也不从不明来源下载“SpeedTree 原版树”冒充自制生成。

### 12.2 外观验收矩阵

| 维度 | 方法 | 判定重点 |
| --- | --- | --- |
| 树种轮廓 | 无纹理黑色剪影，至少 8 方位＋2 俯仰，与目标参考板对照 | 冠幅、冠形、枝下高、主枝层次和负空间 |
| 枝干结构 | 隐藏叶片、灰模旋转灯 | 不是重复叉棍，无包络硬折线，分叉/根颈自然 |
| 叶团 | 仅叶片 mask、cluster ID 和局部特写 | 叶团层次/空隙，不是均匀刷毛或球团 |
| 单叶 | 近景 UV/alpha contour 对照，真实尺标 | 图片轮廓未被通用几何截掉，叶脉/叶柄方向一致 |
| 根干微细节 | 相同尺度灰模/法线/Height 对照 | 轮廓几何与表面细节分工合理，不是噪声圆柱 |
| 材质 | 通用静态网格、导入干、程序干同材质同灯 | 颜色空间/TBN/roughness 一致，薄叶例外明确 |
| 光照 | 阴天、晴天顺光、逆光、侧光，固定曝光 | 叶片不塑料、不自发光，冠内层次与环境一致 |
| 风动 | 固定风与阵风序列 | 枝团有层次、叶片不脱根、扫描部件不变橡皮 |
| LOD | 固定相机路径、同帧投影覆盖/深度 | 冠形/负空间/颜色不突然崩塌，不靠暴涨宽度补叶 |
| 可编辑性 | 局部剪枝/锁定/换部件/改材质/撤销/重开 | 修改范围可预测，依赖和 ACK 正确 |

只改贴图产生的“好看”不得掩盖黑色剪影仍不自然。silhouette 相似度只能评价特定目标，不能单独当作所有树的真实性分数；参考拍摄视角未知时说明配准误差，不能把照片背景误算为枝叶。

使用可解释指标辅助评审：高/冠幅/胸径比、枝序长度分布、曲率尖峰、实际/目标弧长、主枝半径关系、叶面积/冠体积、叶倾角分布、叶团空隙分布、单位屏幕像素工作量。阈值针对物种/夹具开工前冻结，不失败后随意放宽。

### 12.3 资产/材质接口验收

- 同一个导入部件的单位、轴向、负缩放、UV、法线、材质在资产预览与树内一致。
- 材质实例改一张 normal/roughness/height/厚度图，树与通用材质观察器同步更新，几何身份不乱变。
- 无法支持的 MIR/导入输入明确诊断；不因字段未识别静默变默认。
- 贴图替换改变依赖摘要并失效正确缓存；切回旧实例可复现原结果。
- 原纹理可用时不自动重烘损失扫描细节；需要重烘的接合区提供配置/误差图。
- 发布后移动整个包到不同目录仍能加载，不依赖绝对源路径。

### 12.4 AI 验收

- 实际 provider/model/version、输入摘要、输出 recipe、校验结果和资产来源记录；不保存秘钥。
- 固定 recipe 的 deterministic bake 可复现；不要求每次模型响应字节一样。
- 对非法参数、未知部件、预算过大、超时和过时响应，拒绝或受控修正，记录修正。
- 局部 AI 编辑与手工编辑使用同一事务/undo；所选树干和材质 lock 被尊重。
- 没有模型调用就不标 AI 已完成；无参考图三维重建证据就不标“照片重建”。

### 12.5 性能与资源

记录实际适配器/驱动/分辨率/功耗模式、CPU 生成、纹理/几何上传、GPU 各 pass、交互延迟和显存峰值。已有单株 640×400 数据不能外推为 1080p 生产性能。

先完成单株 hero 与固定画质档，再测 1k/10k 有效 LOD 场景。单株面数、叶数、材质数、draw、过绘和阴影工作量公开；不得以单 LOD 一次几亿三角形的压力测试表示“生产可用”。

逐实例 LOD 的统计用 GPU 分桶计数异步回读并带帧号，不能用 LOD0×提交株数冒充实际绘制量。已有 `StartInstanceLocation` 的合同与测试以当前 D3D 路径为准，新增分桶必须实测，不照搬其他 API 的实例 ID 语义。

## 13. 工程约束、测试与输出

遵守所有适用 `AGENTS.md`。保护现有开发中的材质/导入改动，先检查工作树与相关 API，再增量扩展；不重置、不清理、不复制覆盖整个子系统。新增模块不要启动第二套资产数据库、另一个材质图解释器或另一个图像解码器。

禁止未经用户新明确要求运行全量测试/回归、`npm test`、`npm run verify` 或完整 CI。复用现有植被与材质/导入专项，只运行受影响集合；资产导入验证使用 `npm run test:assets`，必要时 `npm run assets:verify`，保留 `build/assets-host` 独立输出。

先检查 `tests/vegetation*.test.mjs`、`tests/vegetation-editor.test.mjs`、现有原生 draw 与 viewport 专项。新增测试关注独立几何/接口不变量：真实弧长、局部变异稳定、图集轮廓覆盖、扫描 socket、MIR 适配和材质实例热更新；不靠遍历每像素数百万断言制造“测试强度”。

本轮新证据放：

```text
reports/vegetation-realism/
  audit.md                         当前能力、具体限制、已确认/待验证
  architecture.md                  实际接口/所有者/适配矩阵
  references.json                  外观参考与组件/贴图来源
  recipes/                         hero 与候选源、局部 edit operations
  morphology.json                  结构/尺度/真实弧长/叶团指标
  asset-integration.json           imported digest、socket、材质/依赖
  material-integration.json        MIR/ref → GPU consumer 逐项见证
  ai-runs.json                     真实调用/disabled/错误与结果
  images/                          多视角/灰模/剪影/近景/通道对照
  sequences/                       风动、旋转灯、LOD 路径
  performance.json                 同画质同硬件工作量与帧时间
  acceptance.md                    R0–R8 逐项通过/部分/未执行
```

指标、截图与日志都关联源码/工具/shader/组件/材质/recipe 摘要。新旧图使用匹配的曝光、尺寸、相机与预算；未测用 null 或 unavailable。艺术评审仍不足时明确写“结构/接口已通过，真实感未达目标”，不能将断言全过包装成照片级。

## 14. 最终交付与执行要求

请直接持续开发，按 R0–R8 顺序给出可用成果，不在完成审计后就结束，不以“以后可加入 AI/扫描/材质系统”代替本任务的集成。

最终报告至少提供：

1. 当前树“不真实”的形态、资产和光照原因，以及实际修复代码。
2. 一条完整程序化路线、一条扫描/作者部件混合路线的可编辑树。
3. 实际复用的材质/导入函数、适配矩阵和新增最小接口；未支持项列表。
4. 树干/叶片的 UV、Normal、ORM、Opacity、Thickness、Height 的真实接线及调试图。
5. 多视角剪影/灰模/最终材质对照，树种与 seed 变体，扫描干特写。
6. AI provider 实际状态、已运行调用与可撤销 recipe/操作；未接服务则明确不算 AI 完成。
7. 原生编辑 revision/资产/材质依赖 ACK、保存/重开/发布证据。
8. 实际增量命令、性能/内存、失败或未执行项，更新当前状态文档并链接新证据。
9. AI 素材自动规范化的 root/tip/留白与多通道对齐证据；程序化虫洞、虫咬、病斑和树皮贴花的节点、材质、LOD/阴影一致性与编辑器操作证据，遵守强制补充规范。

不要承诺“输入一句话就一定生成照片级树”。应交付一个由可验证程序化结构、可信艺术资产和受控 AI 共同工作的工具，使照片级结果可制作、可修改、可部署，并清楚说明达到的质量与剩余限制。

## 15. 一手参考资料

参考核对日期：2026-10-26。SpeedTree 文档使用可访问的官方历史/9 版页面说明工作流，不将其当成当前所有版本的功能清单。以下只作设计依据，不复制其代码或未经授权的商业资产。

- [SpeedTree：旧生成器升级与新工作流](https://docs9.speedtree.com/modeler/doku.php?id=toolupgrade)：参考分开的枝/叶/部件生成、形态和材质创作能力。
- [SpeedTree：Card Generator](https://docs9.speedtree.com/modeler/doku.php?id=card_generator)：参考材质/mesh/pivot、分辨率和叶簇着色控制；普通 leaf mesh 与 camera-facing card 不应混淆。
- [SpeedTree：Bifurcation](https://docs9.speedtree.com/modeler/doku.php?id=genmode_bifurcation)：参考分叉作为显式生成方式。
- [SpeedTree 9 官方发布：扫描转换与程序化编辑](https://store.speedtree.com/speedtree9ishere/)：参考真实扫描资产与程序编辑的混合路线，不推定项目已有同等 Mesh Converter。
- [Runions 等，2007：Modeling Trees with a Space Colonization Algorithm](https://www.algorithmicbotany.org/papers/colonization.egwnp2007.html)：参考空间驱动的枝冠构造。
- [Palubicki 等，2009：Self-organizing Tree Models for Image Synthesis](https://www.algorithmicbotany.org/papers/selforg.sig2009.html)：参考光/空间竞争、芽存活与交互生长。
- [Longay 等，2012：TreeSketch](https://algorithmicbotany.org/papers/TreeSketch.SBM2012.html)：参考自主生长与画笔、弯曲、修剪组合的作者工作流。
- 仓库：[外部资产接口](EXTERNAL-ASSETS.md)、[材质管线](MATERIAL-PIPELINE.md)、[材质路线](MATERIAL-ROADMAP.md)、[原始总体计划](VEGETATION-HAIR-DEVELOPMENT-PLAN.md)。仓库文档可能晚于或早于代码，执行前复核。
