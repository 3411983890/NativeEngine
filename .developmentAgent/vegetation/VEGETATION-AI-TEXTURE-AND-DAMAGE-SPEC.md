# 植被 AI 贴图、自动导入与程序化损伤规范

- 日期：2026-10-26。
- 版本：设计合同 v1；本文新增字段、自动规范化器与损伤节点均为待实现要求，不是当前能力声明。
- 用途：给 DeepSeek 执行的强制开发补充，与 [SpeedTree 类优化提示词](DEEPSEEK-VEGETATION-REALISM-SPEEDTREE-PROMPT.md) 一起使用。
- 目标：AI/DCC/扫描叶片一键导入后自动朝向正确、根点连接、尺度与通道正确；虫洞、虫咬、病斑和树皮贴花进入可复现的程序化生成图。
- 工作方式：复用已有资产导入、材质、atlas、cook、稳定 ID 与原生 revision/ACK，实际实现并验证，不以生成提示词替代导入功能。

## 1. 总规则

1. 单叶根部朝下、叶尖朝上，中脉主要沿竖直方向；默认一图一叶，工具自动打图集。
2. **叶片不紧贴图片底边**。根部位于固定安全留白内，由 metadata 的 pivot 连接模型。贴底会造成 mip、过滤、压缩和图集边缘裁切。
3. 图像留白不应造成模型悬空：模型用 root pivot 作为局部原点，不能继续使用图片底部中心代替真实根点。
4. AI 未必严格满足像素尺寸/根尖位置，必须自动规范化与校验；不能声称仅靠提示词即可保证引擎无需调整。
5. 颜色、遮罩、法线、ORM、Height、Thickness 使用同一 UV 和几何参考；独立 AI 推断图不能直接标为像素级配准或物理测量。
6. 健康基础叶与损伤分开；默认健康叶完整无洞，损伤在独立程序化层生成。
7. 正常规范资产不需要作者手工旋转、拖 pivot 或猜通道；不合格图自动修复或明确拒绝，不能暗中猜错。

## 2. 单叶 canonical 坐标与留白

### 2.1 坐标约定

- 叶片局部 UV：左下为原点，U 向右、V 向上；根到尖为 +V，叶片正面朝观察者。
- PNG 像素：左上为原点，X 向右、Y 向下。坐标转换在指定边界发生一次。
- 背面图采用同一规范化局部方向，模型背面采样/翻转由材质合同统一，不随便再翻 V。
- 复叶、针叶束、枝叶簇不强套单叶中脉规则，应有自己的 kind、pivot、socket 和方向。

### 2.2 标准布局

默认单叶源槽为 1024×1024 RGBA；支持 2048² 同比例。其他分辨率经规范化接入，记录真实尺寸，不能把放大图标为原生高细节。

| 参数 | 默认合同 |
| --- | --- |
| 根部 `rootPivotUV` | `[0.5, 0.0625]`，在底边上方 1/16 |
| 根到尖方向 | +V，主轴竖直 |
| 叶尖上限 | V ≤ 0.9375，保留至少 1/16 顶部安全区 |
| 左右主体范围 | U 位于 [0.0625, 0.9375] 内 |
| 底部 | 根点以下保留透明区，不截断叶基/叶柄 |
| 长宽比 | 等比缩放，不能把窄叶拉成宽叶铺满正方形 |
| 物理尺寸 | 明确叶身长宽、叶柄长度，单位米，不从像素猜 |
| 图像类型 | 真 RGBA/真实透明，无棋盘格图案、阴影或背景 |

1024² 的连续像素边界坐标中，根点为 (512,960)，顶部安全界 Y=64；这不是整数像素索引，metadata/采样须使用明确 texel-center 约定，不混用 W 和 W−1。

宽叶按默认高度缩放后若超出左右边界，应继续等比缩小；叶尖允许低于上限。记录实际 tipUV、contentBoundsUV 与 pixelsPerMetre，不为了固定尖点扭曲植物比例。

### 2.3 叶柄必须声明

- **bladeOnly（默认）**：只画叶身与叶基，不画长叶柄；root pivot 是叶身基部，连接到程序生成叶柄的末端。
- **bladeWithPetiole**：画完整短叶柄；root pivot 是图中叶柄最下端，额外记录 bladeBaseUV。程序不重复生成同一段叶柄，或只生成明确剩余段。

两种 profile 在 AI 提示词、metadata、叶片 mesh 和 placement 中保持一致。禁止“贴图含柄＋几何再加同长度柄”导致叶子离枝太远。

### 2.4 模型连接公式

```text
leafLocalPosition = mappedSurface(UV) − mappedSurface(rootPivotUV)
worldPivot = hostSocket 或 petioleEnd
```

因此透明安全区保留，但根部在几何空间仍精确相连。当前 `atlas.mjs` 的 legacy 默认 `[0.5,0]` 应由版本化 migration/每 cell metadata 处理，不能直接全局改默认值导致旧图错位。

## 3. AI 叶片生成模板

模板由 SpeciesProfile/TreeRecipe 填充，生成结果交给 canonicalizer，不要求语言模型精确填写像素测量值。

```text
资产：游戏用单片植物叶片 Base Color + 真实 Alpha。
物种：{明确物种/叶型}；生长阶段：{成熟/幼叶}；季节：{季节}。
对象：仅一片叶子，正面，采用 {bladeOnly / bladeWithPetiole}。
视角：正交、垂直俯视，叶身展开近似平面，无透视、无严重翻卷。
方向：叶基/叶柄连接端朝正下方，叶尖朝正上方，中脉沿竖直中轴。
定位：根点水平居中，位于底边上方约 6.25%，不触底，不截断。
留白：四周真实透明安全区，叶尖不触顶，左右不越界，保持自然比例。
叶柄：{仅叶身基部、不画长柄；或完整短柄不截断}。
颜色：去光照自然本色，保留叶脉和表皮色差，避免高饱和假绿色。
光照：均匀漫射/交叉偏振式采集，无方向阴影、环境投影和镜面亮斑。
健康：完整健康，无虫洞、虫咬、病斑；损伤由引擎独立程序化添加。
背景：真正透明 Alpha，不画黑/白底、棋盘格、接地阴影。
禁止：文字、标签、边框、标尺、多叶拼图、树枝、花、景深。
输出：独立高分辨率 RGBA；不以放大尺寸冒充真实原生细节。
```

多主体、切断根/尖、不透明背景等问题自动检测，可靠修复不了则拒绝并输出明确再生成建议。不要把自动识别失败变成作者每次手工调素材的常规步骤。

### 3.1 树皮与数据图规则

树皮默认单张 tileable 材质源，生长/纵向纹理沿 +V，记录米制 tile size；无树干轮廓、烘焙灯影、强高光和文字。根颈/分叉 uniquePatch 单独标识，不当作平铺图。seamless 需实际边界检查，不能只看提示词。

优先同一高模/扫描统一烘焙 Normal/AO/Height/Thickness。必须由 AI 推断时参照 canonical 基图编辑，不改变轮廓、叶脉、孔位和布局，仍标 inferred。

Normal 是线性切线空间，声明 +Y/-Y；ORM 是线性 R=AO、G=roughness、B=metallic；Height 白高、Thickness 白厚并带米制 scale。数据图不写入颜色/阴影/文字。

AI 不能保证 ORM 精确通道时，使用现有数据打包工具保证 metallic=0 等合同并记录处理。所有 maps 共享 canonical transform；Normal 除像素位置旋转/镜像外，还必须正确变换切线空间 XY 分量，绿通道转换只一次。

## 4. 自动导入与 metadata

### 4.1 规范化流程

```text
原图＋来源 metadata
 → 现有导入器解码/Alpha/主体数量检查
 → profile、root/tip/中轴识别（明确 landmarks 优先）
 → 共用旋转/翻转/裁剪/等比缩放/平移
 → rootPivot 与安全留白对齐
 → 关联 maps 和 Normal 向量同步变换
 → 语义/配准/尺度/置信度验证
 → canonical 单叶包
 → 自动 atlas/texture array、每 cell metadata
 → 叶片几何/attachment/MaterialRef
```

不要简单用最低 Alpha 像素识别根：裂叶、复叶、噪点和倾斜可能误判。PCA 只能给主轴，不保证根尖极性；优先显式 landmarks 和已声明 profile，自动推断须有方法与置信度。

方向不确定、根尖截断、多主体、跨 map 明显错位时 fail/待修复，不能标“无需调整”却在后续树生成里猜位置。规范化后正常资产一键导入，低置信异常不静默进入生产库。

复用 `engine/assets` 的解码/材质/文件验证，扩展 `engine/vegetation/atlas.mjs` 与现有材质合同，避免新建平行导入系统。

### 4.2 最低 metadata 示例

以下是拟实现 schema，示例尺寸/置信度不是所有叶子的测量值：

```json
{
  "schema": "nativegpu.leaf-texture-source/1",
  "assetKind": "leaf",
  "leafProfile": "bladeOnly",
  "speciesId": "oak_reference_profile",
  "side": "front",
  "imageSize": [1024, 1024],
  "coordinates": {"origin": "bottomLeft", "u": "right", "v": "up"},
  "rootPivotUV": [0.5, 0.0625],
  "tipDirectionUV": [0, 1],
  "bladeLengthMetres": 0.12,
  "bladeWidthMetres": 0.07,
  "safeMarginFraction": 0.0625,
  "normalConvention": "directX-",
  "baseDamageState": "healthy",
  "maps": {
    "baseColor": {"file": "leaf.basecolor.png", "colorSpace": "srgb"},
    "opacity": {"file": "leaf.opacity.png", "channel": "R", "colorSpace": "linear"},
    "normal": {"file": "leaf.normal.png", "colorSpace": "linear"},
    "orm": {"file": "leaf.orm.png", "colorSpace": "linear"},
    "thickness": {"file": "leaf.thickness.png", "channel": "R", "colorSpace": "linear"}
  },
  "provenance": {"provider": "actual", "quality": "inferred"}
}
```

最终还须记录 contentBoundsUV、tipUV、petiole/bladeBase landmarks、root detection confidence、source/canonical transform、normal vector transform、源/工具/prompt 哈希、cutoff 和 validation 结果。

### 4.3 图集合同

- 默认单叶自动 pack，不要求 AI 直接生成精确 3×4 图集。
- 每 cell 有自己的 pivot、有效内容框、真实尺寸、profile、损伤变体身份，不能只有全图一个默认 pivot。
- canonical safe margin 与 atlas gutter 是两回事，不重复扣边。
- 使用整数像素 rect/明确归一化 rect，避免 1254×1254 分 4 行时静默截断。
- legacy 图集拆分、规范化、重打包应保留来源/迁移哈希，原文件不覆盖，相关 mesh/cook 正确失效。
- 打包后所有 maps、UV、pivot 与几何一致，不能再次自动移动根。
- per-cell mip、扩边、coverage、小 mip 退化可验证；纹理数组路径记录能力和显存限制。

## 5. 程序化损伤与表面贴花

在生成器图中新增受控 DamageGenerator / SurfaceDetailGenerator。损伤应影响正确通道，而非只给最终颜色画几个黑圈。

| 类型 | 覆盖与几何 | 材质联动 |
| --- | --- | --- |
| throughHole 内部虫洞 | 真正减去 Opacity，可选近景几何孔 | 孔边干枯环、normal/roughness/thickness |
| edgeBite 边缘虫咬 | 从外轮廓切入 | 边缘干燥/卷曲，必要时几何变形 |
| tear 撕裂 | 连续裂口，可沿纤维方向 | 裂口边缘变化，近景可分离曲面 |
| spot/necrosis 病斑坏死 | 默认不改 coverage | 颜色、roughness、透射与轻微 normal |
| leafMiner 潜叶虫道 | 默认保留叶面 | 弯曲纹路、颜色/厚度，不当通孔 |
| 灰尘/水渍/蜡质斑 | 通常不改 coverage | 通用材质层与遮罩 |
| 苔藓/地衣/树皮斑 | 表面贴花/层材质 | 颜色、normal、roughness、AO/Height |
| 树疤/剥皮/折断 | 小细节贴花，大轮廓需几何 | 裸木/树皮过渡与几何/表面细节 |

虫洞与普通贴花在 UI 中分开：黑色圆心不是孔，病斑也不默认变洞或改变 metallic。

### 5.1 随机身份和分布

损伤 key = 物种/树实例 stable ID＋LeafAttachment ID＋damage generator ID＋seed。相机、帧号、LOD 与遍历顺序不参与；same ID/seed 得到同样损伤。

参数至少包含：健康叶比例、受损叶比例、孔数与孔径米制/相对范围、边缘咬伤比例、病斑尺度/色环宽、轻中重档、叶龄/冠层位置/枝组掩码、局部 painted mask、variant 数和 seed。

相关分布使同枝/同环境损伤相近但不完全重复；保留健康叶，禁止整树每片叶同样破损。AI/手工局部编辑共享 undo/事务。

### 5.2 有效区与保护区

- insideHole 在有效 Alpha 内且距边缘足够远；edgeBite 明确选外轮廓边界，两种模式不混用。
- 默认保护 root/petiole，建议初值为叶长 3%；主脉保护和绕脉倾向可调，不声称自然绝无主脉损伤。
- 用 vein/stem 语义 mask 或受控算法近似，不能把参考图灰度当所有保护区。
- 限制默认损伤的保留面积与主体连通，重伤模式显式声明；断开的碎片删除或成为明确独立附件，不能无绑定漂浮。
- damage 后检查 pivot 与有效叶身仍有声明的连接；避免一个洞意外吃掉根部。

### 5.3 多通道合成

约定 holeMask=1 表示移除：

```text
coverageFinal = coverageBase × (1 − holeMask)
edgeRing = ringAround(holeMask/edgeBite, physicalWidth)
baseColorFinal = layerBlend(baseColorBase, lesionColor, edgeRing/spotMask)
roughnessFinal = boundedLayer(roughnessBase, damageRoughness, damageMask)
normalFinal = tangentCorrectBlend(normalBase, edgeNormal, edgeRing)
thicknessFinal = remainingSurfaceThickness(thicknessBase, damageProfile)
```

Opacity 只合成一次，避免 albedo.A 和独立 mask 重复相乘。孔内 Thickness 只为采样回退，不控制 coverage；灰度 mask 不能直接作为 Normal RGB。孔边方向与 TBN 一致。

病斑、虫道、苔藓通过现有 MaterialRef/MIR 适配。MIR 若缺运行时 damage 语义，可先离线编译受控变体，不要求完成任意 shader graph 才能交付。

### 5.4 几何与 LOD

近景大孔/缺口可生成几何，小孔用 MASK，阈值依世界尺度和屏幕贡献。不要按每个 mask 像素生成碎三角。

同一叶片跨 LOD 保留 damage identity；亚像素孔允许按 coverage/透射积分退化，不强求每个小孔永远二值可见，也不能突然变成黑色病斑。depth/color/shadow 共享 damage UV/coverage，风动后同样随宿主附着。

损伤几何烘焙与最终 LOD 顺序受控，修改孔参数正确失效叶 mesh；仅改病斑颜色不重建全树骨架。

## 6. AI 损伤素材规则

健康基图与独立损伤库分开。孔轮廓优先算法/SDF，AI 提供自然斑点、地衣或干枯环外观；不要每棵树都向远端生成独立贴图。

### 6.1 Stamp 标准

单 stamp 默认中心 pivot [0.5,0.5]，透明背景、四周至少 1/16 安全区；定向虫道/撕裂从下往上沿 +V，无方向斑点标 isotropic。

stamp 记录真实尺寸、影响 mask、各通道、方向、normal convention。树皮贴花记录 branch-local UV/socket/投影深度，限制背面泄漏并跟随风动，不能漂在世界坐标。

### 6.2 提示词模板

```text
资产：单个可程序化盖印的 {病斑/虫咬干枯边缘/地衣斑}。
只画损伤外观，不画整片叶、整段树干、背景和阴影。
主体居中，完整轮廓，四周透明安全区，不碰边。
有方向图案沿 +V 自下向上；无方向斑点标 isotropic。
表面：{特征与真实尺度}，去光照，无镜面亮点/环境投影/景深。
背景是真 Alpha，无棋盘格、文字、格线、边框。
这是表面外观，不把黑色圆心当透明虫洞。
通孔由独立形状 mask/SDF 生成，本素材只表达孔边/病斑外观。
单图单 stamp；关联 normal/roughness 等沿用同一定位和 UV。
```

insideHole 与 edgeBite 的形状库使用有界噪声轮廓/SDF；不要把带叶片背景的“虫洞照片”直接当透明贴花。

## 7. 预算与缓存

支持两条路径：

1. **离线有限变体库**：每健康 cell 烘焙有限 damage variants，多通道一起打包 atlas/array，stable ID 选片，作为生产默认。
2. **受限运行时层**：每叶少量 variant/seed/strength，从共享 shape/stamp atlas 求值，限制采样数和层数；大量动态损伤另评估局部更新页。

禁止每片叶一张 1K 图、一个材质实例或一个 draw call；英雄 unique patch 受预算限制。基图、mask/stamp 共用，不按整树复制完整高分辨率资源。

cache key 覆盖基图 digest、damage profile/seed/保护规则/尺度、生成器与打包布局、cutoff、normal convention、cook 工具指纹。更新通过现有异步 cook、revision/ACK 与 fence-safe 资源切换，不逐帧阻塞等待。

## 8. 编辑器和 AI 接入

- 导入预览显示 root/tip、profile、safe margin、尺标、规范化前后、inferred/baked 状态和诊断。
- “导入并规范化”正常无需手工旋转/pivot；自动修复可追溯，低置信资产明确拒绝。
- 生成图选择 DamageProfile：健康比例、孔径/强度、保护区、seed、变体数，按枝组/叶簇/冠层局部作用。
- 表面节点控制树皮苔藓/地衣/湿痕/树疤，与通用材质实例共享。
- debug views：base coverage、hole mask、edge ring、spot mask、final coverage、protected regions、variant ID。
- AI 指令如“只让外侧老叶少量虫咬、保护叶柄”“这根枝加褐斑但不要孔”生成 schema 校验的局部 operations。
- 修改可撤销/重做，保存重开/发布保持一致；原生 ACK 包含损伤与材质依赖版本，不能仅 WebGL 假更新。

## 9. 实施与验收

| 阶段 | 实际工作 | 退出证据 |
| --- | --- | --- |
| T0 | 叶片/树皮/stamp schema、坐标/尺度/语义、legacy 迁移 | 规范 metadata、拒绝夹具 |
| T1 | canonicalizer、root/tip、共用 map/normal transform | 多方向/留白输入同 attachment |
| T2 | 单叶自动 pack、每 cell pivot、mip | 原生角标/连接对照，零手工偏移 |
| T3 | SDF 孔/咬边、病斑/虫道、保护区/稳定 seed | 多通道一致损伤与有效连通 |
| T4 | MaterialRef、离线变体/受限 shader 层、缓存 | 真实消费者、无资源数量爆炸 |
| T5 | 节点/UI/AI operations、ACK/undo/保存/发布 | 实际交互与重开 |
| T6 | 近远景、风/阴影/LOD、纹理与 GPU 预算 | 增量验收矩阵 |

拟新增 canonical-leaf、texture-source、damage、decal-placement 等小模块；按现有架构落地，复用通用导入器和材质系统。

### 9.1 自动导入验收

- 同一叶片的 0/90/180/270°、镜像、不同留白/分辨率输入，规范化后 root 相同、tip 向 +V、比例不失真；切断/多主体被拒。
- 1024² 的 canonical root 误差建议 ≤1 px；分辨率变化按 texel 比例报告，低置信推断不能假记通过。
- 主体不越 safe margin；pack 后 pivot 与单叶一致，模型根确实连接宿主/叶柄，不靠相机 framing 遮错。
- bladeOnly 无贴图/几何重复柄，bladeWithPetiole 不重复同一柄段。
- 合成方向 normal 图验证旋转/镜像的分量变换、V/绿通道只翻一次。
- source→canonical 变换、实际尺寸、哈希、修复/拒绝理由可追溯。

### 9.2 损伤验收

- healthy 没洞没病斑；fixed ID/seed 字节可复现，改变相机/LOD 不重新随机。
- spot/miner 不改 coverage；hole 真透后景，depth/color/shadow 孔位一致。
- edgeBite 与轮廓相交，insideHole 位于内部；独立夹具验证。
- root/petiole 保护与主体连通符合档位，重伤与碎片处理有报告。
- thickness 不代 opacity；孔边 normal/roughness/颜色位置一致。
- 正反面、风、LOD、多视角无损伤漂移，小孔 mip 无明显闪烁。
- 程序叶、导入叶和叶簇使用统一规则，不支持的资产类型给明确错误。

### 9.3 工程纪律和交付

遵守根 AGENTS.md，增量测试，不运行未经新明确要求的全量测试、npm test、npm run verify 或完整 CI。资产导入变化按需使用现有资产专项与隔离输出目录。

新证据放 `reports/vegetation-realism/texture-damage/`：source/canonical metadata、变换、通道见证、root 误差、健康/损伤对照、opacity/thickness/孔边调试图、近远景/风/阴影、性能和验收表。

最终区分“规范已定义、规范化已实现、程序损伤已接线、原生已验证、画质/预算达标”。只写提示词或生成图片，不算开发完成。
