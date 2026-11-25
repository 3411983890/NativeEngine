# 外部资产导入

2026-10-26。目标是直接导入常见游戏资源。当前已接通模型到原生画面的链路；以下矩阵保留未完成项，不把文件复制或离线烘焙等同于运行时支持。

## 使用

```powershell
npm run assets -- import --in "D:/Resources/model.glb"
npm run assets -- import --in "D:/Resources/model.fbx"
npm run assets -- import --in "D:/Resources/model.glb" --size 1024
npm run assets -- list
npm run assets -- verify --in "assets/imported/<导入ID>"
build/assets-host/NativeGPU.exe --asset "assets/imported/<导入ID>"
```

编辑器顶部的“外部资产”可输入本机完整路径、导入、查看诊断，把模型或角色加入当前场景，也可将角色设为玩家。导入对话框的“贴图分辨率”默认“保留源分辨率”。加入场景使用当前 revision 和原有事务历史，可以撤销；模型与贴图先发布为不可变文件再保存引用。

```powershell
npm run assets -- attach --in "assets/imported/<导入ID>" --scene .screen/courtyard.scene.json --revision 0 --collider true
```

`collider true` 明确使用每个模型分段的包围盒碰撞代理。阴影也沿用包围盒 BLAS，不代表实际网格三角形阴影。默认加入场景不启用碰撞。

## 贴图分辨率、宽高比与滤波

2026-10-27。此前模型导入把每张贴图都压到 `size×size`（默认 512）的正方形，并用最近邻取样合成材质通道。结果是 2048² 源贴图丢掉 15/16 的纹素、非正方形贴图被拉伸，表现为“贴图混乱且不清晰”。当前行为：

- `--size` 默认 `auto`：保留源分辨率，最长边上限 4096，**从不放大**（64² 细节图仍是 64²）。显式给 16–4096 的 2 的幂可硬性限制最长边。
- 宽高比按源图保留，`cookDirectory({preserveAspect:true})` 逐贴图计算目标尺寸并向下取偶；`manifest.textures[]` 增加 `sourceWidth` / `sourceHeight`，`mips` 记录真实层数。
- 缩小改用**面积平均**（颜色在线性光下平均，法线平均后重归一化），放大保持双线性。2048²→512² 与 lanczos3 参考的 PSNR 由 26.43 dB 提升到 35.67 dB。
- 每个语义按自身源分辨率生成：`albedo`/`normal`/`orm`/`occlusion`/`emissive`/`coat`/`coatRoughness`/`coatNormal` 不再被拉进同一张合成图，各槽的源 UV 变换仍由运行时按 `textureMappings` 施加。
- 内容相同的源贴图在多个材质间只存一份（按通道字节哈希去重）；由纯因子合成的常量贴图仍按材质各留一份，保持材质命名稳定。
- 法线/清漆法线按**角度分位数**判定 BC5 是否可接受（`mean ≤ 1.5°`、`p95 ≤ 3°`、`p99 ≤ 8°`、`max ≤ 45°`），不再因 2048² 里个别块的离群值退回未压缩 RGBA8。遮蔽与两个清漆因子只被读取单通道，改用 BC5 存储该通道（每像素 1 字节而非 4 字节），质量按实际读取的通道计算。
- 作者切线不再要求严格垂直。glTF 2.0 规定客户端须把 `TANGENT` 对 `NORMAL` 正交化，此前偏离几度的 DCC 导出会被整包拒绝；现在做 Gram-Schmidt，切线平行于法线时退回稳定基，手性 `w` 保留。

`DamagedHelmet` 公开样例（2048² 五张图）在默认 `auto` 下由 512² 全量重采样变为原分辨率，体积 53.33 MiB → 21.33 MiB，法线与遮蔽均落到 BC5。新增 8 项 `tests/glb-texture-quality.test.mjs` 用例覆盖比例保留、不放大、面积平均、法线归一化、显式上限、非正方形导入、去重与切线正交化。

## FBX

FBX 几何仍由 Assimp 转成 glTF，但该转换**不携带**两个决定摆放的文档级设置，也不保证贴图能落地；此前因此出现“FBX 不支持”。现在补充：

- `engine/assets/fbx-metadata.mjs` 直接读取 FBX 容器（二进制 7.x 与 ASCII）的 `GlobalSettings`，取出 `UnitScaleFactor`、`UpAxis`/`FrontAxis`/`CoordAxis` 及其符号。`metersPerUnit = UnitScaleFactor / 100`，因此按厘米建模的 Maya 导出不再大 100 倍。
- 轴向转换成 `coordinates` 覆盖并写入资产身份：`+Y/+Z` 的正规文件保持默认身份，`+Z` 向上（3ds Max 等）会记录 `upAxis/forwardAxis/handedness`。`handedness` 由文件自己的 `CoordAxis` 判定，避免 Z 向上时左右镜像。显式 `--options coordinates` 优先于自动检测。
- 转换后的 glTF 会被修正：图片按 basename 在源目录（含三层子目录）内解析，绝对路径与内嵌贴图不再直接报错；仍找不到时只断开受影响的材质槽并给出警告，其余模型照常发布。
- Assimp 给 n-gon 网格写的 `FB_ngon_encoding` 从 `extensionsUsed` 移除（几何已是三角面），否则角色导入的严格动画路径会拒绝该文档。
- 导入诊断记录 FBX 容器格式、版本、单位与轴向，例如 `Converted binary FBX 7400: 1 cm per unit, +Y up, +Z front.`。
- `append-animation` 对 FBX / DAE 动作源使用同一份元数据，未显式给 `coordinates` 时按文件声明转换。

`tests/fbx-import.test.mjs` 覆盖二进制与 ASCII 元数据解析、厘米单位缩放、贴图按名解析与缺失降级、Z 向上转换和能力声明。真实厂商 FBX（复杂蒙皮、多层材质、嵌入式视频贴图）仍需按内容包增量验收。

### 单位、上方向和朝向

编辑器“外部资产”中的“调整模型或动作的单位与轴向”可设置长度单位、向上轴、角色正面轴、左右手系和导入后的转向角度。默认遵循格式解码结果；glTF 使用米、+Y 向上、+Z 正面，定义见 [glTF 2.0 规范](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)。引擎角色正面为 −Z，默认通过 Z 镜像转换。显式默认坐标与省略坐标参数产生相同的当前版本资产身份；预览或其他导入配置变化仍会产生新身份。

CLI 的 `import --options import-options.json` 支持：

```json
{
  "coordinates": {
    "unit": "cm",
    "upAxis": "+Z",
    "forwardAxis": "-Y",
    "handedness": "right",
    "yawDegrees": 0
  }
}
```

单位可为 `m/cm/mm/ft/in`，或用 `metersPerUnit` 指定每源单位的米数（范围 0.000001–1000）；两者互斥。轴可为正负 X/Y/Z，上方向与正面须垂直。`yawDegrees` 围绕转换后的竖直轴转向，范围 ±3600°。覆盖作用于解码或 Assimp 转换后的数值，不会猜测或再次自动套用 DCC 元数据中的单位。

坐标配置同时变换顶点、法线、三角形绕序、节点平移/旋转、inverse bind、动画及其三次曲线切线、形变位移、显式 Rig/软组织长度和动作速度。静态网格的碰撞代理由转换后几何生成；角色可按变换后的静止姿态拟合，玩家控制器读取实体代理尺寸。绑定骨骼 scale 保持独立的无量纲语义。旋转使用同一基变换，避免只旋转网格造成绑姿脱离。源参数未声明的运行时默认 Rig 长度仍使用引擎米制默认值。当前量化、非均匀骨骼缩放和资源预算限制仍适用。

`append-animation --options motion-options.json` 的 `coordinates` 使用同一结构，先把源动作转换到引擎空间，再执行静止姿态重定向、骨长比例和可选 `orientation` 校正。模型/角色导入把配置记录在不可变身份及角色头中；动作追加将源动作配置写入新身份和重定向报告，已有角色的几何不会再次缩放。

### 角色与动作分开导入

```powershell
npm run assets -- append-animation --in "D:/Resources/walk.bvh" --target "assets/imported/<角色ID>"
npm run assets -- append-animation --in "D:/Resources/actions.fbx" --target "assets/imported/<角色ID>" --options "D:/Resources/motion-options.json"
```

支持 glTF / GLB、FBX / DAE 转换，以及专用 BVH 读取器。BVH 保留 OFFSET、位置通道和声明的欧拉旋转顺序，不依赖虚拟骨架网格。文件名前缀自动避免与现有动作重名；同一输入与选项产生同一新包。原角色包保持不变，网格、材质和旧动作字节原样保留，新包记录 `parentAsset` 与 `character/retarget.json`。

默认按静止姿态重定向：源动作的全局旋转变化作用于目标静止骨架，保留目标骨长；位移按匹配骨长的中位比例转换。常见 namespace / Mixamo / 人形别名可自动匹配。重名歧义或跨分支映射会拒绝；未匹配的目标骨骼保留局部静止姿态，诊断列出省略的源分支。比例与朝向需要作者确认时可以显式覆盖。

可选配置示例（目标骨名与片段名应按实际资源填写）：

```json
{
  "mode": "rest-pose",
  "prefix": "shared_",
  "translationScale": "auto",
  "orientation": [0, 0, 0, 1],
  "boneMap": { "hips": "Hips" },
  "motionRoot": "hips",
  "rootMotionAxes": "xz",
  "controller": { "walk": "shared_Motion" }
}
```

`boneMap` 是目标名到源名/节点索引的映射；`null` 跳过目标骨骼。`mode: exact` 要求映射骨骼的静止变换、父级、比例与朝向兼容。`clips` 可选择源片段，`morphMap` 可关联表情名称，`loop` 可覆盖循环；未知选项拒绝发布。归一化整数旋转/形变、LINEAR / STEP / CUBICSPLINE、通知和动作匹配元数据进入同一编译链路。

骨盆上的根位移默认把水平分量转移到根骨骼，保持全局姿态与垂直起伏；`rootMotionAxes: xyz` 包含垂直分量，`motionRoot: false` 关闭转移。生成的根位移按原有运行时合同提取，由玩法驱动实体移动。追加动作不会自动替换原有 locomotion 绑定；`controller` 可显式改绑定。

原生资产预览使用 `character/preview.json` 播放第一个新增片段；该预览关闭动作匹配选择，避免切回旧片段。正式角色仍是 `character/character.json`。编辑器“外部资产”中选择“追加动作到角色”，可填写骨骼对应、比例、朝向和根位移来源；新版本出现在资产列表中。

### 多个角色进入场景

资产列表可选择动作并点击“加入当前场景”或“设为玩家角色”。角色属性面板可编辑动作、播放速度、起始时间、模型转向、缩放、偏移及各材质分段引用，再通过原有预览/保存事务提交。每个分段可修改材质参数或复制为独立材质实例，统计按实际角色绑定计算；重复加入同一资产保留已经编辑的场景材质。玩家动作留空时使用移动控制器；NPC 留空时播放资源默认动作。玩家使用速度或时间偏移时须明确指定动作。

命令行沿用 `attach`，通过 `--options character-options.json` 指定可选配置，例如：

```json
{
  "replacePlayer": false,
  "clip": "vendor_Raise",
  "speed": 0.5,
  "timeOffset": 0.25,
  "position": [2, 0, 0],
  "modelScale": [1, 1, 1],
  "yaw": 0,
  "rootMotion": "asset",
  "fitCollider": "humanoid",
  "anchor": "feet"
}
```

每个实体的 `character` 引用正式头文件和相邻二进制的 SHA-256，保留独立动作、模型变换与材质映射；预览专用头不会进入正式场景。模型缩放与实体碰撞代理尺寸分开；`offset` 是缩放/转向后的局部模型偏移，玩法 `renderable.offset` 仍为世界偏移。`rootMotion` 可为 `asset`、`preserve` 或 `extract`；NPC 的提取模式生成原地播放，移动由玩法/物理驱动。

场景最多 64 个角色，包含默认玩家。资源按校验后的内容复用；渲染器共享静态顶点/索引/形变数据，每帧为各实例提供独立调色板、形变权重和软组织数据。动画源数据与 GPU 蒙皮上传各有 256 MiB 预算，GPU 几何缓存最多 128 项。NPC 的片段播放时间由固定模拟 tick 推导，参与场景内容身份检查，可随快照和严格回放恢复。程序性动作匹配、IK、肌肉与软组织控制仍用于主玩家，NPC 使用作者片段及形变曲线。

场景构建会收集各角色的正式头、二进制和重命名贴图；编辑器截图也基于冻结后的完整依赖包。重复路径的冲突摘要、损坏文件与包缓存均会拒绝。

运行中的预览检测到场景材质表变化时重建渲染器并重新上传，避免使用旧材质索引；当前尚非局部 GPU 替换。新增贴图也进入编辑器的可选贴图列表。全局烹制贴图堆上限仍为 256 张，64 实例不代表可同时容纳 64 套任意大小的独占贴图。

### 角色体型、脚底位置与控制器

新角色加入场景时会按静止姿态计算代理，包含所有 skin 的 inverse bind、作者提供的默认形变、模型缩放和转向。NPC 默认 `fitCollider: "bounds"`，玩家默认 `"humanoid"`；`false` 保留手动代理尺寸。完整范围模式使用模型包围范围，玩家还使用覆盖转向的径向范围；人形模式将半径限制为身高约六分之一，是站立角色的近似代理。当前控制器仍为扫掠 AABB，没有胶囊体或精细蒙皮碰撞。

拟合会写入实体 `scale`，并在未显式提供 `offset` 时对齐模型位置。新 NPC 默认以脚底为 `position` 基准：`character.anchor: "feet"`；其刚体中心上移半个身高，物理位置回传与交互遮挡使用同一范围。`center` 使用碰撞体中心，早期未写入 anchor 的 NPC 保留这个旧基准。玩家始终以脚底定位。

玩家高度取 `entity.scale.y`，水平代理半径取 `max(scale.x,scale.z)/2`。头顶扫掠、穿行、台阶、眼高、交互偏移和第三人称相机随体型变化；相机近裁剪也随小体型缩短。`character.controller` 可覆盖 `stepHeight`、`eyeHeight`、`interactionHeight`、`orbitDistance`、`orbitHeight`、`targetHeight`，单位为米；实际台阶高度会限制在身高以下。省略字段使用自动值（编辑器留空即省略），已有显式相机眼高仍可保留，controller 的眼高覆盖优先。

模型缩放与代理尺寸保持独立。修改缩放后可在角色属性面板点击“按人形／完整模型重新拟合代理”，或使用：

```powershell
npm run assets -- fit-character --scene .screen/courtyard.scene.json --entity <实体UUID> --revision <当前版本>
```

`--options fit-options.json` 接受 `mode: "bounds" | "humanoid"` 和 `alignModel: false`（保留现有模型偏移）。拟合走原有 revision、保存和撤销事务；依赖 SHA-256 不符时拒绝修改。显式控制器参数保留。缩放若由玩法修改，控制器会读取新的 ECS 尺寸；重新载入快照会恢复尺寸和锚点。

新角色预览场景也保存拟合尺寸、模型对齐与验证后的角色引用；追加动作会同步预览头和二进制摘要。预览配置进入内容身份，因此这些新包的 ID 可以改变，旧包仍可验证加载。

## 已接通范围

| 资源 | 当前入口及运行能力 | 尚有边界 |
| --- | --- | --- |
| 静态模型 | glTF / GLB / OBJ；通过 Assimp 转换 FBX / DAE / STL / PLY / 3DS / 3MF，并读取 FBX 自身的单位与轴向；顶点、索引、节点层级与变换、多 primitive/材质、UV0、法线进入实际光栅绘制；Draco、EXT/KHR meshopt 解码；单位/轴向及绕序转换 | 转换格式不等于所有源软件特性；复杂蒙皮、多层材质与嵌入式视频贴图的厂商 FBX 还需逐个内容包验收；没有网格 streaming、精确碰撞/阴影 |
| 角色与动作 | glTF / GLB；可转换骨骼源格式；多 mesh/skin/primitive、独立 inverse bind、多材质分段、刚性骨骼挂件、矩阵根节点、多骨架根；60 Hz 动画包支持 sparse、CUBICSPLINE TRS；压缩几何共用解码器；64 个独立场景实例、玩家替换与依赖打包 | 最多 256 调色板骨骼、100000 顶点、256 分段；正均匀骨骼 scale；最多四个影响，超过时给出丢失权重警告；NPC 尚无独立的完整状态控制器 |
| 独立动作与重定向 | 追加 glTF / GLB / FBX / DAE / BVH；名称/显式骨骼映射、静止姿态与轴向转换、比例、根位移路由、独立表情曲线、通知及控制绑定；网格与材质复用 | 每包最多 64 片段、单片段 120 秒、二进制 64 MiB；层级映射须相容；没有自动语义识别任意未知骨架、非同构骨架约束求解或重定向专用足部接触优化 |
| 形变与表情 | POSITION/NORMAL/TANGENT 任意非空组合、每节点独立默认 morph 权重、形变独占片段、LINEAR/STEP/CUBICSPLINE 权重曲线；混合、蒙皮与快照沿用统一曲线；无骨骼源网格可生成刚性骨架 | 最多 16 个合并形变目标，权重范围 [-8,8]；自动生成切线的目标不能改变手性；未提供面部 Rig/口型制作器；相同形变名的独立节点自动命名隔离 |
| 角色控制绑定 | 五个 locomotion 状态可由 `extras.nativegpu.controller` 映射到真实片段名；`extras.nativegpu.attachments` 配置 chest/rightHand；常用名字可匹配 | 缺状态时只为预览绑定到现有片段并给出警告，不代表自动生成动作；缺挂点需要作者补充 |
| 材质 | base color 纹理/因子、线性 RGB/RGBA 顶点颜色、KHR_materials_unlit、作者切线/正负手性、MikkTSpace 自动生成、切线形变、法线/强度、独立 AO 贴图/强度、metallic/roughness 通道、KHR_materials_ior、clearcoat 因子/强度图/粗糙度图/表层法线；多套源 UV 各贴图独立选择，偏移/旋转/缩放，重复/镜像/边缘限制和六种缩小/两种放大过滤；独立 RGB 发光贴图/颜色因子及 KHR_materials_emissive_strength；按需生成三至八张语义纹理；双面用反面几何实现；MASK / BLEND、alphaCutoff / opacity 贯通场景与材质参数 | 切线、MASK / BLEND、独立发光/AO 和多套 UV 要求 SM 6.6 材质路径；透明效果穿插、皮肤、透射、HDR 源贴图与完整材质图仍待完成；不支持时明确拒绝 |
| 图片贴图 | PNG / JPEG / WebP / TIFF / AVIF；TGA / BMP；DDS 的常用 RGBA/打包整数与 BC1–5/BC7；KTX2 的常用 RGBA/BC、ETC1S、UASTC（含 Zstd），独立 Basis；按颜色/法线/ORM/emissive 语义进入 mip/压缩 `.ngtex`；默认保留源分辨率与宽高比、缩小时面积滤波、不放大；独立预览及 albedo/normal/ORM/emissive/occlusion 材质绑定；模型/角色共用解码器，支持 GLB 内嵌与 KHR_texture_basisu / MSFT_texture_dds / EXT_texture_webp / EXT_texture_avif | 材质贴图当前为 2D SDR；HDR 材质贴图、原始 ETC2/ASTC、数组/立方体/体积纹理仍待接通；UI/Sprite 系统仍待完成 |
| 环境 | HDR/RGBE、平面单部件 EXR；浮点 DDS/KTX2 的 2:1 全景图；线性浮点天空、SH 漫反射与 GGX 预过滤反射；亮度/转向、原生预览、场景加入/撤销、依赖搬迁、打包与 GPU 热更新 | 256 MiB 解码预算；需要 SM 6.6 / DXR 1.1 世界光照路径；立方体/数组、deep/multipart EXR 与图层选择仍待接通 |
| 音频 | WAV；MP3 / OGG / FLAC / M4A / AAC / AIFF / Opus 经随项目安装的 FFmpeg 解码到 48 kHz PCM；输出可搬迁的 `.ngsnd` 和 bank；支持原生预览、场景声音引用、实体跟随和场景依赖打包 | 压缩源转换为整段 PCM，未实现长音乐 streaming、SOFA/ADM、外部 DSP 图 |
| 特效 | 自有 `.effect.json` 校验和 cook，实际贴图依赖 hash 锁定并收集进包 | 不支持 Niagara/VFX Graph/Houdini 资产直接转换，复杂 GPU 场/mesh/ribbon 等以现有运行时合同为限 |
| 布料/水体 | 自有 `.cloth.json` / `.water.json` 复用已有校验器和内容寻址库 | 这些是库导入，尚无外部服装缝线/布料缓存转换、完整服装运行绘制或生产水面制作流程 |

静态模型把 glTF 右手坐标转换为引擎左手坐标，变换烘焙进顶点，再按 AABB 规范化；实体 position/scale 恢复其大小与位置。统一角色入口同步镜像顶点、法线、骨架、inverse bind、动画与形变。底层动画导出 API 保留默认严格旧合同；统一入口通过 `normalize:true` 启用转换。剪切、反射和非均匀骨骼 scale 仍会拒绝。

导出器产生的亚量化缩放噪声会归一为同一均匀值，避免三个轴分别舍入后在原生端变成非法非均匀缩放；真正的非均匀缩放仍拒绝。Cesium Man 公开样例触发过最大约 1.43e-6 的轴差异，已纳入增量回归。

无骨骼动画道具默认保留根节点动画；骨骼角色默认提取根位移。可在 `extras.nativegpu.rootMotion` 中设置 `preserve` / `extract`。表情曲线使用公共姿态曲线缓冲的 16–31 槽，并参与混合、变形与确定性摘要。关闭程序性肌肉/软组织不会关闭作者提供的形变动画。

每份导入包记录源文件、选项、内容身份、诊断、全部输出文件与 SHA-256。导入先写临时目录，成功才发布；依赖越界、损坏 buffer、非法 accessor、超预算等拒绝发布。相同资源和选项产生相同身份。版本 2 身份包含预览场景配置和规范化身份文本；Node 和原生入口同时核对配置、派生 UUID、输出清单及文件内容，修改预览不会复用旧包身份。压缩解码总输出限制为 256 MiB。

原生预览要求版本 2 内容身份；早期导入包需要重新导入。裁切参数也进入材质编辑器、MIR、最小材质图和场景身份；彩色贴图自动选择保留 Alpha 的格式。`alpha-pixels.json` 将同一面板的不透明/裁切画面进行面积对比。

运行时打包脚本收集 `assets/imported` 的清单依赖及顶层自定义场景。自定义场景可通过场景整体导入搬迁完整依赖；编辑场景的位置应使用“打开为新场景”，随后按场景依赖构建发布包。

## 重新导入与场景引用更新

编辑器资产列表的“更新外部资源”可输入修改后的源文件，生成独立新版本，预览后更新当前场景。列表显示版本短标识，旧包保留。也可分两步运行：

```powershell
npm run assets -- reimport --in "D:/Resources/updated.glb" --target "assets/imported/<旧资产ID>"
npm run assets -- update --in "assets/imported/<新资产ID>" --target "assets/imported/<旧资产ID>" --scene assets/main.scene.json --revision <当前版本>
```

重新导入沿用原纹理尺寸、语义与单位/轴向设置；已追加动作的派生角色会重新生成最后追加的动作，保留父版本几何、已有动作及重定向设置，要求原父包仍在同级资源库。比较计划记录于资源库 `reimports/`，不会改写旧包。资源种类改变、损坏旧包或非法源文件拒绝发布新版本；输出比较计划采用不可覆盖的原子发布。

glTF/GLB 模型和角色记录来源分段、材质身份。命名唯一的节点在重排后仍可匹配，命名唯一的材质在数组重排后仍可匹配。需要跨重命名或换父节点保持身份时，可在节点、primitive、材质写入 `extras.nativegpu.sourceId`（非空、最多128字符，同类适用范围内不得重复）。没有唯一名称的分段、多份无名材质、重名兄弟节点会标记不可靠；OBJ 当前同样标记为不可靠，不会靠网格哈希猜测分段。单一候选和旧包唯一名称的匹配会另行注明依据。

新加入场景的模型/角色实体带有 `imported` 来源标识和实例标识；预览包不加入随机实例标识，保持内容身份确定。已有场景可在几何引用唯一时更新；共用同一网格而缺失来源标识的旧实例会拒绝自动匹配。

当前更新覆盖静态模型、角色、独立贴图、声音和环境图。模型更新保留实体ID、名称、碰撞设置，以及相对于源边界的位移/缩放；角色保留姿态、播放选项、碰撞体和已有拟合偏移，体型变化后可另行使用“拟合角色”。材质按旧来源、新来源、场景现值逐字段合并，保留已修改参数和主动删除的贴图；手动改绑的自定义材质保持绑定。已明确匹配的分段可确定其材质对应关系，因而也支持多份无名材质重排和作者更换分段材质；角色源材质合并后若存在不同场景覆盖，当前会拒绝不兼容的合并。局部实例更新创建独立材质版本，不会改写仍使用旧资产的实例。声音更新保留发声器与声音键，环境更新保留强度和转向。

应用前核对场景版本、引用内容、依赖目标与合并结果；验证通过后才发布本地依赖并一次提交场景，撤销可恢复旧引用和参数。未变化的版本不增加撤销记录。当前仍拒绝静态分段增删、无法明确匹配的来源和已删除的选中动作；尚未实现结构变更映射界面、对派生角色重新导入基础模型并自动重放全部动作链、整体场景合并更新、特效/布料/水体引用更新及局部GPU热替换。

重新导入原生证据位于 `build/asset-import-smoke/reimport/latest.json`（指向独立运行目录）：24项原生断言通过，包含真实超过260字符的角色路径、非法来源标识原子拒绝及目录联接越界拒绝；相关玩法存档/继续运行定向检查也通过。移走原始源目录和全局资源库后，更新的模型/角色场景仍可运行和打包，打包前后截图逐像素相同，两组240帧严格回放摘要一致；模型更新产生45615个明显变化的通道，角色实际蒙皮绘制480次，验证层错误为0。此次同时修复 Windows 长路径在规范化后失去扩展前缀，以及嵌套脚本分隔符导致读取失败的问题。证据绑定独立宿主二进制 SHA-256 `b2775a43142606ebf5865d2f8d5cb8438a9a5feb234ef7de3f1ee67ffbf31737`，没有运行全量测试。

## 纹理导入、预览与材质绑定

```powershell
npm run assets -- import --in "D:/Resources/brick.dds" --semantic albedo
npm run assets -- import --in "D:/Resources/normal.ktx2" --semantic normal
npm run assets -- attach --in "assets/imported/<贴图ID>" --scene .screen/courtyard.scene.json --revision 0 --material m0
build/assets-host/NativeGPU.exe --asset "assets/imported/<贴图ID>"
```

编辑器“外部资产”可预览独立贴图，选择当前场景已有材质后绑定颜色、法线、ORM 或独立发光槽；绑定走 revision、保存与撤销事务。只复制校验后的主贴图，场景打包会收集新引用；同名不同内容、语义错配和目录越界会拒绝。预览配置参与不可变身份，旧版没有预览的纹理包仍可验证。`emissive` 使用独立发光通道预览并可绑定场景材质。

DDS 解码包含 DX10、常见整数通道布局、BC1/2/3/4/5/7、BC4/5 SNORM、预乘 Alpha 还原和边缘块裁切。KTX2 校验头、DFD、mip 范围与每级解压长度，处理方向、通道 swizzle、zlib/Zstd；ETC1S/UASTC 和独立 Basis 使用固定版本的 Basis WASM 转码器。颜色槽将声明的线性 RGB 转为 sRGB，法线/ORM 保留数据语义；双通道法线重建 Z。DDS 基层支持声明的行跨度，较低 mip 支持紧密排列或传统 DWORD 行对齐，按完整字节长度核对。损坏数据、无效 BC7、HDR 材质、数组/立方体会明确拒绝。

glTF 的压缩扩展优先选择扩展图片，不读取缺失的备用 PNG；KHR_texture_basisu 还核对真实压缩模式、尺寸、方向、色彩空间与预乘状态，遵循 [KHR_texture_basisu](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_basisu) 和 [KTX2 规范](https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html)。静态模型、多材质角色和 GLB 内嵌图片使用同一路径。

BC 解码器采用 [bcdec](https://github.com/iOrange/bcdec) 的固定提交 `80859ed3b7afb1c527a2a99d70c61457bea72d0c`，源文件、许可和摘要在 `third_party/bcdec/`。首次解码使用项目 LLVM 编译小型工具，缓存于 `build/assets-host/texture-codec/<内容摘要>/`，校验源、编译器和产物摘要；缺少工具链时运行现有 `tools/bootstrap.ps1`，不需要构建或测试整个引擎。Basis/Zstd 来自固定版本 Three 包中的转码器，Basis 许可保留于 `third_party/BASIS-UNIVERSAL-LICENSE.txt`。这些仅在导入期使用，发布场景使用 `.ngtex`。

纹理专项新增 15 项离线 Node 用例，资产增量入口共 106 项通过。`build/asset-import-smoke/textures/public-acceptance.json` 记录 DDS、ETC1S、UASTC、TGA 原生预览、材质绑定、源目录之外的依赖包运行、相同截图和 240 帧严格回放。BC7 全部 8 种模式以独立 GPU 硬件解码作对照，最大通道差为 0。9 份公开 KTX2 样例覆盖 RGBA/sRGB/线性、BC1/3/4/5/7、ETC1S 和 UASTC，均实际解码导入；BC7/ETC1S/UASTC 另作原生绘制。来源固定为 Three 提交 `9b4a2ac29c63ccb43fd51c5661f2f873ac2c39b8`，SHA-256 和 MIT 许可记录保留在证据中。

```powershell
# 使用已经校验的公开纹理缓存，不联网
node .incrementalTesting/assets/asset-texture-check.mjs --public
# 首次获取固定版本公开纹理（可选）
node tools/texture-public-fixtures.mjs --download
# 复核自制 Basis 测试图的生成结果；需要本地固定版本编码器缓存
node .incrementalTesting/assets/generate-texture-fixtures.mjs --check
```

普通 `test:assets` 使用仓库内自制的 ETC1S/UASTC/Basis 测试图，不下载公开资源或运行编码器。

### 独立彩色发光与强度

模型和角色可直接导入 glTF 的 `emissiveTexture`、线性 RGB `emissiveFactor` 和 [KHR_materials_emissive_strength](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_emissive_strength)。发光贴图使用独立 sRGB 颜色槽，Alpha 不参与发光；着色时先解码为线性 RGB，再乘颜色因子与强度，保留红、绿、蓝各通道，不受底色、湿度或磨损染色。没有发光贴图时用白色贴图承载常量发光因子；glTF 未声明因子时仍遵循默认黑色发光。新包的 ORM Alpha 不再重复保存发光。

场景命名材质可使用以下字段；未写独立槽的旧材质继续使用原有 ORM Alpha 发光方式：

```json
{
  "albedo": "surface",
  "normal": "surface",
  "orm": "surface",
  "emissive": "neon",
  "emissiveFactor": [0.1, 0.5, 0.9],
  "emissiveStrength": 16
}
```

颜色因子每通道范围为 0–1，强度范围为 0–1000000。独立槽省略因子/强度时使用白色和 1；贴图仍为 SDR，强度在浮点材质记录中保留。FP16 场景累积前将独立发光表面限制到 65504，避免强发光产生无穷值。显示沿用场景的 `lighting.toneMap`、`exposure` 与光照品质配置；关闭映射时高亮直接裁到显示范围。场景照明继续使用灯光和 GI 配置。

编辑器材质面板可选择发光贴图、编辑三通道颜色和强度，选择空槽可断开独立发光。`material_patch`、MCP、MIR 和最小材质图保留这些参数；API 用 `emissive: null` 断开。材质实例复制颜色数组，避免两个实例互相改色。场景构建收集 `.emissive.ngtex`，角色/模型加入场景时一并重命名引用。颜色/强度/贴图进入快照身份与预览材质变更检测，非法因子或损坏语义会明确拒绝。

新增 6 项离线 Node 用例，资产入口共 112 项通过；相关材质/MIR/图/编辑器/MCP 定向运行共 38 项通过。原生专项结果在 `build/asset-import-smoke/emissive/`，含 24 项原生加载/快照/身份断言、黑色底材上的独立 RGB、同色强度 1/4 对照、角色两个蒙皮材质分段、依赖包像素对照和 240 帧严格回放。公开 [Emissive Strength Test](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/EmissiveStrengthTest) 使用固定 GLB SHA-256 `074898ec4c3636230faffe0cacac7403c3f6135db61f943da28ffaa88ca2a39f`，作者 Ed Mackey / AGI，© 2022 AGI，CC-BY-4.0；来源、许可及原始 README 随证据保留。

`public-acceptance.json` 实测公开样例 1/2/4/8/16 倍强度的平均显示亮度依次为约 127.33/187/204/238/255；`character-acceptance.json` 记录 480 次实际蒙皮分段绘制、6664 个红色和 4115 个蓝色像素。原有无独立发光槽的场景与旧截图逐像素相同，最终模拟摘要仍为 `3f46a0ebee906ce4`；使用当前构建重新录制后的 240 帧严格回放通过。回放头继续绑定构建身份，旧构建录制文件不会绕过该检查。

```powershell
# 默认专项使用自制样例，不联网；已纳入 assets:verify
node .incrementalTesting/assets/asset-emissive-check.mjs
# 已有公开样例缓存的验证；首次可加 --download
node .incrementalTesting/assets/asset-emissive-check.mjs --public
```

### 纹理变换与独立采样

模型和角色导入支持 [KHR_texture_transform](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_transform) 的偏移、弧度旋转和缩放，按 `平移 × 旋转 × 缩放` 作用于所选源 UV。颜色、法线、ORM、发光和 AO 五个槽各自保存变换和采样方式，法线的导数基使用实际采样 UV。重复、边缘限制、镜像重复可分别设置 U/V；放大支持近邻/线性，缩小支持近邻、线性及各自的最近 mip/线性 mip 组合。

```json
{
  "textureMappings": {
    "albedo": {
      "offset": [0.25, 0.75],
      "scale": [2, -0.5],
      "rotation": 1.5707963267948966,
      "wrapS": "clamp",
      "wrapT": "mirror",
      "magFilter": "nearest",
      "minFilter": "linear-mipmap-linear"
    }
  }
}
```

偏移、缩放和旋转范围为 ±1000000，零/负缩放有效。省略各设置时使用 `[0,0]`、`[1,1]`、0、重复边界和线性/mip 混合。未声明整个槽映射的旧材质继续使用原有 8 倍各向异性重复采样器。原生使用有界的 109 项 sampler heap：1 项旧默认加 108 项标准边界/过滤组合；各材质记录保存每槽矩阵及 sampler 索引。新映射需要 SM 6.6 路径，降级时明确拒绝，避免丢失采样语义。

编辑器各贴图下的“图案位置与采样”可启用/关闭单独设置。`material_patch`、MCP、MIR、最小材质图、材质实例复制、撤销和场景打包保留该对象；`textureMappings: null` 清除覆盖。预览地面及玩家高度随静态模型最低点调整，原点下方的源模型也能完整查看；场景加入操作继续保留源几何坐标。

多套源 UV 的选择与传输见下节，`KHR_texture_transform.texCoord` 优先于纹理信息的 `texCoord`。AO 已改用第五个独立槽，可与 metallic/roughness 使用不同的变换和采样；相关用法见“独立 AO 贴图”一节。

新增 9 项离线用例，当前资产增量入口共 121 项通过。`build/asset-import-smoke/mapping/acceptance.json` 记录 22 项原生加载/快照/身份断言；四种语义的运行时变换与独立写入源顶点 UV 的对照截图，最大通道差均为 0；108 种组合实际绘制出 29527 个彩色像素，依赖包截图相同，240 帧严格回放一致。

本地映射专项启动先核对宿主与当前输入匹配，随后逐次核对二进制和构建清单摘要，原生契约编译前再次核对头文件 ABI。`acceptance.json` 保存该轮构建指纹；若其他任务重新构建了宿主，本轮会拒绝混用产物。源文件继续编辑时，不会把不同构建的画面对照混在同一结果中。

`mapping/character-acceptance.json` 另核对两个材质分段的角色 UV 变换：480 次蒙皮分段绘制、10167 个彩色像素，与源顶点烘焙控制的最大 RGB 通道差为 1、平均约 0.00000217。资产增量入口中的场景 CLI 搬迁测试也核对了新增共享校验模块的发布依赖。

公开内容固定为 Khronos Sample Assets 提交 `8f492390ddc7a2ae221a1369ef560589cf8c70fb`。`TextureTransformTest` 为 © 2018 Microsoft，CC0-1.0；`TextureSettingsTest` 为 © 2017 Analytical Graphics Inc.，Ed Mackey，CC-BY-4.0。源文件、依赖和 README 的字节数及 SHA-256 存于 `tests/fixtures/texture-mapping-public.json`，验证缓存后才导入；许可与来源随证据保留。完整模型预览中，变换样例与独立烘焙顶点 UV 的最大 RGB 通道差为 1，平均约 0.00142。重复/镜像测试显示绿色对勾，边缘限制测试显示绿色实心块；截图在 `build/asset-import-smoke/mapping/public/`。

```powershell
# 只验证本次纹理映射依赖范围，使用仓库内自制样例，不联网
node .incrementalTesting/assets/asset-mapping-check.mjs
# 公开样例：使用已校验缓存，首次可加 --download
node .incrementalTesting/assets/asset-mapping-public-check.mjs
```

### 多套源 UV

静态模型和角色可为颜色、法线、ORM、发光、AO 分别选择 `TEXCOORD_n`，按材质的 `texCoord` 读取，`KHR_texture_transform.texCoord` 可覆盖该选择。所需 UV 集缺失、顶点数量不一致、索引非法或 UV 超过 ±1000000 时，在发布导入包前拒绝。浮点、规范化整数以及解压后的 UV 使用同一读取链路。未被材质引用的额外 UV 集不会增加运行时流数量。

导入把五个槽实际选择的坐标写入几何，之后再由各槽独立应用偏移、旋转、缩放和采样方式。`parts[].textureCoordinates` 或角色 `mesh.parts[].textureCoordinates` 记录来源索引。多套 UV 使用静态 `nativegpu.mesh/2` 的 16 分量顶点；角色沿用原有 64 字节磁盘顶点前缀，并添加每顶点 32 字节的可选 UV 区段。加入顶点颜色后，原生顶点池分别为 80/112 字节，预算按实际分配计算。旧静态 `/1`、`/2` 和无可选区段的角色继续加载。

蒙皮合并、双面背面、形变、角色代理拟合、追加动作、场景引用与依赖包保留该数据。原生使用每槽实际 UV 计算法线导数基；SM 6.6 不可用时明确拒绝多 UV 几何。编辑器可修改各槽变换和采样；切换源 UV 集需修改源材质后重新导入，当前没有 UV 展开编辑器。

新增 6 项定向用例覆盖五槽不同坐标、扩展覆盖、双面/蒙皮/形变、动作追加、旧格式、缺失数据、稀疏 UV 及 meshopt 压缩的规范化整数 UV。原生专项入口如下；证据保存在 `build/asset-import-smoke/texture-coordinates/`。

`acceptance.json` 记录 27 项原生断言，核对每个槽的实际顶点坐标、旧角色读取、缺损区段/NaN/非法网格的原子拒绝及控制器执行。五种语义分别在静态和蒙皮模型上与源 UV0 烘焙控制比较，共 10 组最大/平均 RGB 差均为 0；每组角色实际绘制 480 次材质分段。五槽同用不同 UV 的角色打包前后截图相同，240 帧严格回放通过。蒙皮管线改为显式选择蒙皮着色器，避免扩展输入布局后因旧字段数量判断误选静态路径。

`skinning.json` 进一步核对实际 GPU 蒙皮：只修改 inverse bind 调色板，保持几何、片段、形变权重及实体位置相同，旧格式和多 UV 角色分别改变 18041 / 18133 个可见像素；每次绘制 480 个材质分段。该检查可单独运行 `node .incrementalTesting/assets/asset-texture-coordinates-check.mjs --skin-probe-only`。

公开 [MultiUVTest](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/MultiUVTest/README.md) 使用 UV0 颜色与 UV1 发光图案。固定提交的原始 GLB 为 43004 字节，SHA-256 为 `f5352cced402b595ab7f1e4971852fbf33cef8d22351e45caafb072e31d5008a`，与 README、标志使用说明一起记录在 `tests/fixtures/texture-coordinates-public.json`。原样导入后，对比故意改为 UV0 的控制内容，第二套 UV 改变 4433 个像素，最大 RGB 差 237；打包画面相同，240 帧严格回放通过。证据和许可保存在 `texture-coordinates/public/`。模型为 © 2017 Hilo 3D、CC-BY-4.0；glTF 标志另遵循 Khronos 的标志使用说明。原始文件仅保存在忽略的本地缓存。

```powershell
node .incrementalTesting/assets/asset-texture-coordinates-check.mjs
# 公开 MultiUVTest，使用校验后的本地缓存，首次可加 --download
node .incrementalTesting/assets/asset-texture-coordinates-public-check.mjs
```

### 无光照材质和顶点颜色

模型和角色支持 [KHR_materials_unlit](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_unlit/README.md)。导入为具名材质的 `unlit: true`，按基础颜色纹理 × 基础颜色因子 × 顶点颜色计算颜色。光照、AO、法线、金属度/粗糙度、发光和 clearcoat 不参与该材质的着色；导入也不会为忽略的 PBR 回退贴图读取文件或要求额外 UV 集。透明裁切、半透明混合、双面几何、基础颜色的 UV 选择/变换/采样继续有效。全局显示映射仍由场景控制。

`COLOR_0` 支持 RGB / RGBA、32 位浮点或规范化无符号 8/16 位整数，按线性数值使用，不再做 sRGB 解码。RGB 自动补 alpha=1，RGBA 的 alpha 与基础颜色 alpha 相乘后参与 MASK 裁切。OBJ 的 XYZ RGB 顶点扩展也会保留原始线性 RGB，支持负索引和混合顶点的默认白色。顶点数须匹配，颜色范围为 0–1；非法类型、规范化标记、范围和计数在发布前拒绝。稀疏颜色及 meshopt 解压后的颜色使用同一读取链路。

带颜色的静态网格使用 `nativegpu.mesh/3`：20 分量包括位置、法线、五槽 UV 和 RGBA。角色在原有磁盘顶点/可选 UV 区段之后，添加每顶点 16 字节的可选颜色区段；追加动作、形变、皮肤合并、代理拟合和依赖打包保留该区段。旧网格和旧角色的原生顶点默认白色。无光照材质和顶点颜色要求 SM 6.6 材质路径，不满足时明确拒绝。

编辑器材质面板“着色方式”可选择“接受光照”或“不受光照影响”；`material_patch` / MCP、MIR、材质图根属性 `unlit`、实例复制、撤销、场景热重载身份和依赖包使用同一布尔字段。新增 8 项 Node 用例覆盖这些链路以及颜色/UV 同用、双面、形变、动作追加、整数/稀疏/压缩颜色和失败原子性。

`build/asset-import-smoke/vertex-colors/acceptance.json` 记录 32 项原生断言，包含新旧格式加载、颜色与无光照标记的身份、快照和非法区段的原子拒绝。关闭显示映射后，三个颜色渐变采样点与 CPU 线性数值预期的最大通道差为 0.5；顶点常色与独立烘焙基础颜色因子对照的最大 RGB 差为 1。无光照材质在零光照和满环境光/100 倍主光下逐像素相同，顶点 alpha 的 MASK 可见覆盖率为 0.5。彩色角色实际绘制 480 个材质分段，7592 个彩色像素可见；依赖包截图相同，240 帧严格回放通过。

公开 [UnlitTest](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/UnlitTest/README.md) 和 [BoxVertexColors](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/BoxVertexColors/README.md) 固定于同一 Khronos 提交，原 GLB、README 的大小及 SHA-256 保存在 `tests/fixtures/vertex-color-public.json`。前者为 © 2019 Analytical Graphics, Inc. / Ed Mackey、CC-BY-4.0；后者为 © 2023 Public / Marco Hutter、CC0-1.0。UnlitTest 原样导入的橙/蓝形体分别有 8548 / 11699 个可见像素，改变光照后的整帧画面相同。BoxVertexColors 的原始顶点颜色相对移除颜色的控制内容改变 31303 个像素。两个原样模型均通过依赖包画面对照与 240 帧严格回放；证据及许可位于 `vertex-colors/public/`，原始文件只保存在忽略的本地缓存。

```powershell
node .incrementalTesting/assets/asset-vertex-color-check.mjs
# 公开样例使用校验后的本地缓存，首次可加 --download
node .incrementalTesting/assets/asset-vertex-color-public-check.mjs
```

### 半透明混合

静态模型、骨骼角色和无光照材质支持 glTF `alphaMode: BLEND`；基础颜色纹理 alpha × 基础颜色因子 alpha × 顶点 alpha 作为最终覆盖率，按线性空间的 over 运算混合。源基础颜色因子的 alpha 保存为浮点 `opacity`（0–1，默认 1），不提前量化到 8 位纹理。OPAQUE 继续忽略 alpha，MASK 使用二值裁切。规则遵循 [glTF Alpha Coverage](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#alpha-coverage)。OBJ/MTL 的 `d` 和反向 `Tr` 同样转为透明度，支持它们出现在 `Kd` 前后；不支持的 `d -halo` 形式明确拒绝。

原生使用逐像素片段链表，保留深度、未预乘 RGB/alpha 和稳定的绘制/三角形序号。透明表面不写不透明深度；最后按每个像素的实际前后关系合成，交叉几何不依赖对象中心排序。不透明深度会剔除后方透明片段。透明表面的雾按各自深度求值，在显示映射之前合成；旧合成路径和质量光照路径均接入。透明物体从不透明包围盒 TLAS 中排除，避免变成实心盒子遮光；支持接收不透明代理的主光遮挡，尚无彩色透射阴影。

片段池按视口的 8 倍像素分配，最多 8,388,608 个 32 字节片段，每像素最多 64 层。仅出现 BLEND 绘制时分配。超过任一容量会以 `NG-E-MAT-TRANSPARENCY-BUDGET` 失败并记录诊断，不把缺失片段当作成功画面。SM 6.6 材质路径是必需条件。传统精灵、GPU 粒子与体积效果仍在原有合成通道，它们与透明模型的完整前后穿插继续属于剩余工作；折射、透射和头发着色也未由 alpha 混合实现。

编辑器“透明方式 → 半透明叠加”和“不透明度”直接编辑相同具名材质参数。材质补丁/MCP、MIR、材质图 `opacity` 输入、实例复制、撤销、热重载身份、快照与依赖包保留透明方式及浮点透明度。新增 Node 用例覆盖静态/骨骼的纹理与顶点 alpha、MTL、参数校验、编辑器历史及打包。

```powershell
node .incrementalTesting/assets/asset-transparency-check.mjs
# 首次准备公开样例可加 --download，后续使用 SHA-256 校验的本地缓存
node .incrementalTesting/assets/asset-transparency-public-check.mjs
```

`build/asset-import-smoke/transparency/acceptance.json` 记录 19 项原生材质/身份/快照/失败原子性断言。线性叠加采样为 `[128,1,66]`，CPU 预期为 `[128.25,1.25,65.75]`；反向提交、交叉面、alpha=0/1、不透明遮挡及纹理/顶点/因子 alpha 的组合通过。启用原生校验层后，默认光追与关闭光追两种路径均无校验错误；透明 PBR 主光在加入旁侧遮挡代理后从 `[161,4,84]` 降为 `[2,1,4]`。两种雾合成路径、480 次骨骼分段绘制、依赖包像素对照及 240 帧严格回放通过。9 层满视口片段池超限与 65 层局部叠加均在首帧明确失败，宿主顶层错误码与诊断均为 `NG-E-MAT-TRANSPARENCY-BUDGET`。

公开 [AlphaBlendModeTest](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/AlphaBlendModeTest/README.md) 使用固定 Khronos 提交，源文件大小、SHA-256 和许可保存在 `tests/fixtures/transparency-public.json`；© 2018 Analytical Graphics, Inc. / Ed Mackey，CC-BY-4.0。原样导入保留 OPAQUE、BLEND 与三个 MASK 阈值，透明效果相对强制不透明的控制画面改变 4742 个像素；绿色检查标记可见，原图红色失败标记为 0，关闭混合的控制图出现 7 个红色像素。反向提交、依赖包截图及 240 帧严格回放一致，证据与许可保存在 `transparency/public/`。

### 独立 AO 贴图

模型和角色的 AO 使用第五个独立贴图槽 `occlusion`，读取线性红通道；绿色、蓝色和透明通道不参与遮蔽。`occlusionStrength` 范围为 0–1，默认 1，按 `1 + strength × (AO - 1)` 混合。AO 可以与 ORM 使用不同的源 UV、变换和采样设置。新导入的 ORM 红通道写入 1；未连接独立 AO 的旧材质继续读取原有 ORM 红通道。

```powershell
npm run assets -- import --in "D:/Resources/ao.png" --semantic occlusion
```

```json
{
  "albedo": "stone",
  "normal": "stone",
  "orm": "stone",
  "occlusion": "stone_ao",
  "occlusionStrength": 0.6,
  "textureMappings": { "occlusion": { "scale": [2, 2], "wrapS": "mirror" } }
}
```

独立 AO 已贯通静态模型、蒙皮角色、追加动作后的资产、编辑器绑定/强度/采样编辑、MIR、材质图、MCP、撤销和场景依赖包。纹理语义码追加为 5，旧容器语义码保持不变；原生加载核对线性数据语义，独立 AO 需要 SM 6.6 材质路径。嵌套场景的贴图列表、绑定校验和预算使用该场景自己的纹理目录，包含新绑定但尚未列入旧 manifest 的纹理，并拒绝越界文件链接或目录联接。

`build/asset-import-smoke/occlusion/acceptance.json` 记录 19 项原生断言；运行时独立 AO 与写入 ORM 红通道的独立控制图最大 RGB 通道差为 1，强度开关改变 27556 个像素。蒙皮角色实际绘制 480 次分段，与烘焙 UV 对照图最大通道差为 1。依赖包画面逐像素相同，240 帧严格回放通过。

公开 DamagedHelmet 固定为 Khronos Sample Assets 提交 `8f492390ddc7a2ae221a1369ef560589cf8c70fb`，原 GLB 和 README 的大小及 SHA-256 保存在 `tests/fixtures/occlusion-public.json`。原始模型保留所有材质输入；AO 开关改变 7158 个可见像素，最大通道差 35，依赖包画面相同，严格回放通过。证据与许可位于 `build/asset-import-smoke/occlusion/public/`。该资产同时包含 © 2018 ctxwing 的 CC-BY-4.0 和 © 2016 theblueturtle_ 的 CC-BY-NC-4.0 贡献，需遵守[原始许可说明](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/DamagedHelmet/README.md)；原始资源只保存在忽略的本地缓存中。

```powershell
# 自制样例，已纳入 assets:verify
node .incrementalTesting/assets/asset-occlusion-check.mjs
# 公开样例使用校验后的本地缓存，首次可加 --download
node .incrementalTesting/assets/asset-occlusion-public-check.mjs
```

## 作者切线与镜像法线

glTF/GLB 的 `TANGENT` 作为 VEC4 保留，支持 float 与声明 `KHR_mesh_quantization` 的规范化 signed byte/short；方向、数量、单位长度和 ±1 手性在发布前检查。没有作者法线时按 [glTF 规范](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#meshes) 忽略切线并提示。静态模型的节点变换、左右手转换、单位/轴向覆盖和 AABB 规范化同时处理切线；反射改变手性，双面副本翻转法线及切线手性，保持 UV 不变。

带切线的静态网格使用 `nativegpu.mesh/4`，每顶点 24 个数：位置、法线、五套 UV、RGBA 顶点色、XYZW 切线；清漆独立 UV 使用下节的 /5 扩展。角色二进制可在 UV/颜色之后、索引之前附加 `tangentOffset/tangentBytes`；多个 primitive 中没有切线的部分填入零向量，显式选择生成基。动作追加、代理拟合、场景引用和依赖包保留该区段，原生几何内容身份覆盖切线字节。旧网格 /1–/4 与旧角色文件保持可读；当前 GPU 静态顶点为 120 字节，蒙皮顶点为 152 字节，旧文件由加载器补齐缺省表层 UV，沿用原有内存预算检查。

原生切线在蒙皮调色板和模型变换中变换，在实际表面法线处正交化，并按 W 恢复副切线；法线贴图独立 UV 变换的旋转和镜像同步调整切线基。光栅、蒙皮、BLEND 以及网格着色器的输入/输出接口保持一致。具有法线贴图但缺少切线的源模型自动生成 MikkTSpace 切线；旧包或不使用法线贴图的零切线区段仍可使用导数基。切线形变见下节。

```powershell
# 原创样例，已纳入 assets:verify
node .incrementalTesting/assets/asset-tangent-check.mjs
# 公开样例使用校验后的本地缓存，首次可加 --download
node .incrementalTesting/assets/asset-tangent-public-check.mjs
```

`build/asset-import-smoke/tangents/acceptance.json` 记录 35 项原生加载、区段/数值拒绝、身份和快照断言。静态与角色的 90 度作者切线对照旋转后的法线贴图，最大通道差为 0；移除作者切线后分别改变 18769、9204 个像素，角色使用自动生成基，实际完成 480 次蒙皮绘制。源几何镜像/非均匀缩放、背面、静态与蒙皮透明绘制，以及三种正负 UV 缩放加旋转，对照均逐像素一致。依赖包截图和 240 帧严格回放一致，原生校验错误为 0。6 项新增 Node 用例还覆盖 signed 量化、meshopt、sparse 和混合 primitive 的零切线回退。

公开 [NormalTangentMirrorTest](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/NormalTangentMirrorTest/README.md) 固定 Khronos 提交，GLB/README 的大小、SHA-256 和许可保存在 `tests/fixtures/tangent-public.json`。© 2018 Analytical Graphics, Inc. / Ed Mackey，CC-BY-4.0。2770 个源切线含两种手性，双面导入后为 5540 个；正反面均完成原生绘制。正面真实几何、普通法线、V 镜像、U 镜像四列的水平线探针一致，上方 `[230,222,181]`、下方 `[47,50,37]`；忽略切线和错误手性的控制图均出现反转，分别改变 16307、5762 个像素。画面、打包/严格回放、探针分析和许可位于 `tangents/public/`。

共享源码并发修改时，可用 `tools/asset-host-snapshot.mjs` 的 `buildFrozenAssetHost()` 构建固定源码副本，将返回的 `context` 交给受影响的专项检查。副本在复制前后检查内容，原生构建继续核对所有输入；合同测试单独冻结当前测试源码，其引擎头文件与链接对象始终来自同一宿主副本，`contract-inputs.json` 保留测试摘要。产物位于 `build/assets-host/frozen-*/`，`source-snapshot.json` 保留来源与文件 SHA-256，证据记录实际二进制及输入清单哈希。切线形变、MikkTSpace 合成、顶点色和多套 UV 的验证使用 `c0074614…f121b3`；作者切线、透明混合和 HDR/EXR 环境在 IOR 改动后以 `7db07268…5a29b9` 复核，后续热更新修复使用下节的新宿主。各专项以证据的 `host` 字段为准，没有把并发编辑导致一致性检查失败的构建计作通过。构建入口先链接到临时文件，通过输入检查后再发布，检测到编译期间输入改变时不会覆盖上次宿主文件。

### MikkTSpace 与切线形变

导入器调用固定 `three@0.186.1` 内含的 MikkTSpace WASM，在源坐标空间生成切线，并按 glTF 约定翻转 W。使用法线贴图实际选择的 UV 集；后续纹理变换由着色器调整切线基。没有作者法线时，为三角形生成平面法线，不沿接缝平均。普通材质也遵循这一缺省法线规则。作者提供的有效法线与切线优先保留，无光照材质不生成未使用的法线贴图切线。许可保存在 `third_party/MIKKTSPACE-LICENSE.txt`。

三角列表、条带与扇形先统一为三角列表。生成器按基础及各形变目标的法线/切线接缝拆点，同步重排所有 UV、颜色、骨骼权重和形变；整数归一化属性保留源存储类型。软组织源顶点选择扩展到每个接缝及背面副本；选择未参与绘制的顶点会明确拒绝。只处理当前选中场景引用的网格。生成工作预算 256 MiB，角色仍沿用 100000 顶点和 16 目标限制。

形变允许 POSITION、NORMAL、TANGENT 的任意非空组合，省略的位移记零。切线 XYZ 位移按方向转换，不乘长度单位；W 保持基础手性，位移先于蒙皮执行，最终与实际法线正交化。含切线的稀疏记录为 40 字节，旧位置/法线记录仍为 28 字节。GPU 形变元素为 36 字节，软组织位移继续独立使用 24 字节，防止两个区段误用步长。自动生成还覆盖目标姿态的法线与切线；若目标折叠导致手性改变，会要求拆分或重新烘焙，不能用 XYZ 位移表达 W 改变。

`tangent-morph/acceptance.json` 包含 1455 项原生断言。权重 −0.5、0.25、0.5、1 的 GPU 形变与预烘焙切线逐像素一致；动态曲线改变 9022 个像素，每次对照完成 480 次蒙皮绘制。`mikktspace/acceptance.json` 的缺法线/切线、接缝和位置形变样例与 CPU 烘焙参考逐像素一致。两项均完成依赖包相同画面、240 帧严格回放，原生校验错误为零。

公开 [NormalTangentTest](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/NormalTangentTest/README.md) 未提供切线：3983 个源顶点自动生成后，双面烹制为 7966 个。© 2018 Analytical Graphics, Inc. / Ed Mackey，CC0-1.0。固定来源、GLB/README SHA-256 与许可记录在 `tests/fixtures/mikktspace-public.json`。真实几何和法线贴图的原生反射探针同为上方 `[230,222,181]`、下方 `[47,50,37]`；关闭切线及错误 W 控制分别改变 8125、8127 个像素并反转探针。正反面绘制、依赖包相同画面和 240 帧严格回放已通过，IOR 与热更新修复后以 `5238dbd0…8c5595` 再次复核，证据在 `mikktspace/public/acceptance.json`。

```powershell
# 已纳入资产增量入口；公开内容检查仅使用校验后的本地缓存，首次可加 --download
node .incrementalTesting/assets/asset-tangent-morph-check.mjs
node .incrementalTesting/assets/asset-mikktspace-check.mjs
node .incrementalTesting/assets/asset-mikktspace-public-check.mjs
```

## 清漆强度、粗糙度与表层法线

`KHR_materials_clearcoat` 的因子及三种贴图已接入。强度读取线性 R，粗糙度读取线性 G，各自乘作者因子；表层法线按独立 scale 烘制并进行法线 mip 重归一化，可使用 BC5。表层法线没有声明时使用几何法线，保留基础法线贴图在涂层下面的凹凸。清漆自身 IOR 为 1.5，基础材质与发光按清漆 Fresnel 权重衰减。实现依据为 [Khronos clearcoat 扩展](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat)。

场景贴图槽为 `coat`、`coatRoughness`、`coatNormal`；标量继续使用 `clearcoat`、`clearcoatRoughness`。三张图可各选源 UV、变换和采样器，连同五个基础槽共八套独立 UV。仅声明表层贴图的模型也可生成所需切线。静态 `nativegpu.mesh/5` 的前 24 个数沿用 /4，后附三套 UV 共六个数；没有切线时写入零帧。角色在切线后可选附加 `layerUvOffset/layerUvBytes`，每顶点 24 字节，旧二进制偏移规则保持可读。新增区段纳入几何身份，动作追加、双面副本、角色拟合及依赖包保留它。

原生使用 120 字节静态顶点、152 字节蒙皮顶点和 352 字节材质记录。64 MiB 静态池、256 MiB 蒙皮缓存和 256 张贴图上限继续按实际字节核对，单材质现在按需烹制三至八张图。清漆贴图要求 SM 6.6；光栅、骨骼、透明和网格着色器的字段接口同步扩展。透明管线的输入元素数量曾遗漏新增 UV，导致创建失败；所有材质管线现在直接从相应布局推导数量，失败报告保留在 `clearcoat/before-pipeline-fix-report.json`。

编辑器提供三个独立贴图槽、各自的位置/采样设置，以及原有清漆因子。单张外部图可按 `coat/coatRoughness/coatNormal` 导入、预览和绑定；绑定到尚未配置清漆的材质时初始化可见的默认因子，已有显式因子保留。重命名材质纹理、MIR/图、CLI/MCP、撤销和场景依赖收集使用相同字段。

`clearcoat/acceptance.json` 包含 8670 项原生断言，主要为旧角色缺省 UV 的逐分量检查，并覆盖新区段、身份、快照和损坏拒绝。八套 UV 的静态与角色原版/烘焙参考逐像素相同；去掉强度、粗糙度、表层法线分别改变 20994、4165、6098 个通道。角色实际完成 480 次蒙皮绘制，透明、依赖包相同画面及 240 帧严格回放通过，校验错误为零。

公开 [ClearCoatTest](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ClearCoatTest/README.md) 的 27 个分段和六种涂层设置完整导入，没有删去扩展。© 2020 Analytical Graphics, Inc. / Ed Mackey，CC-BY-4.0；来源与 SHA-256 固定在 `tests/fixtures/clearcoat-public.json`。关闭任一清漆贴图分别改变 9643、16060、21232 个像素；关闭所有清漆后，六行中间列与相应原始基础材质逐像素一致。错误继承基础法线的控制图改变 11362 个像素。公开包的截图和 240 帧严格回放一致，校验错误为零；画面及许可位于 `clearcoat/public/`。

```powershell
# 原创样例纳入 assets:verify；公开样例默认只使用已校验的缓存，首次可加 --download
node .incrementalTesting/assets/asset-clearcoat-check.mjs
node .incrementalTesting/assets/asset-clearcoat-public-check.mjs
npm run assets -- import --in "D:/Resources/coat-normal.png" --semantic coatNormal
```

## 介质折射率 IOR

`KHR_materials_ior` 已贯通静态模型、骨骼角色、场景材质、MIR/材质图、编辑器、动作追加和依赖包。编辑器材质面板可修改“折射率”；场景及材质补丁字段为 `ior`。省略时为 1.5，扩展空对象也为 1.5。按 [Khronos IOR 规范](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_ior) 接受 ≥1 的有限数，特殊值 0 表示无限折射率兼容模式；介于 0 和 1 之间的值拒绝。烹制与运行时统一使用 Float32 上限，异常类型、超限及未知扩展成员在发布前拒绝。

直接光和 GGX 环境反射使用 `F0=((ior−1)/(ior+1))²`，IOR=0 固定 F0=1；漫反射能量相应减少。金属继续由自身颜色决定反射，清漆表层保持自身的 1.5 IOR。F0 存入原材质前缀的填充位置；材质记录现随清漆扩展为 352 字节。非默认 IOR 要求 SM 6.6。默认 1.5 不增加旧场景的快照身份字段，非默认值纳入身份，防止跨材质恢复；运行中的材质变更也比较 IOR。透射和体积折射仍在下方未完成项中。

`ior/acceptance.json` 记录 35 项原生断言与直接光解析对照。空气、水、默认介质、钻石和 IOR=0 的中心像素分别为 0、28、39、75、168，与 GGX/显示编码计算的误差小于一个通道值；环境反射分别为 3、26、35、67、134。金属更改 IOR 后画面逐像素不变，清漆保留独立反射。相同动画时刻下角色对照改变 238 个像素，480 次实际蒙皮绘制；透明模型改变 27556 个像素。依赖包原生画面相同，240 帧严格回放一致，原生校验错误为零。此处为原创定向样例；带透射/体积/高光等尚未支持扩展的公开 IOR 综合模型仍会明确拒绝，不将删掉其扩展后的渲染计作完整兼容。

`ior/hot/acceptance.json` 验证 IOR=0 的黑/白基色画面完全一致，运行中从 1 改为 2.42 的探针由 0 变为 75，更新后的完整画面与新载入场景逐像素一致，校验错误为零。该专项曾触发 `0x887A0006`：原生网格替换在 GPU 仍引用旧缓冲时释放了它。网格重新上传现在先等待已提交帧完成，再释放旧缓冲及重置上传分配器。修复后的宿主二进制为 `5238dbd0…8c5595`、输入清单 `167950fd…bf698c`；IOR 静态/角色/透明及打包专项使用修复前的同一材质实现 `7db07268…5a29b9`，各证据保留实际哈希。

介质漫反射能量修正也影响没有显式 IOR 的旧材质亮度；旧贴图包与历史构建截图的最大通道差为 2。本次原生专项为旧包重新录制当前构建的回放，同一构建仍要求画面与最终摘要完全一致；历史截图及摘要差异独立保留，不把不同构建的结果混为同一次严格回放。

```powershell
node .incrementalTesting/assets/asset-ior-check.mjs
node .incrementalTesting/assets/asset-ior-hot-check.mjs
```

## HDR/EXR 天空与环境光

```powershell
npm run assets -- import --in "D:/Resources/studio.exr" --size 512
build/assets-host/NativeGPU.exe --asset "assets/imported/<环境ID>"
npm run assets -- attach --in "assets/imported/<环境ID>" --scene .screen/courtyard.scene.json --revision 0 --options environment-options.json
```

`environment-options.json` 例如 `{"intensity":1,"rotation":90}`。编辑器“外部资产”支持环境原生预览、“用作场景天空与环境光”、亮度与水平转向；加入使用相同的版本检查和撤销事务。2:1 浮点 DDS/KTX2 自动识别为环境，其他 SDR 图片继续走贴图用途入口。

导入器将源数据转换为线性 Rec.709 RGBA32F，输出 `.ngenv`；源全景图按球面立体角缩小，默认宽 512，`--size` 最大 4096。漫反射 SH9 从缩小前的源辐亮度积分，并做余弦卷积；原生天空和 GGX 立方体预过滤使用 RGBA32F，支持具名材质、程序化材质与蒙皮绘制。PNG 仅供缩略预览，显示映射不改变浮点环境数据。场景显示默认启用 tone mapping；环境强度为 0–10000，水平转向为 −3600 至 3600 度。没有显式云量时关闭程序化云覆盖。

RGBE 支持新旧 RLE 与所有正负 X/Y 扫描顺序，也接受 XYZE。EXR 在第三方解码前检查通道、窗口、压缩块范围和解压预算，支持完整分辨率 RGB/Y 的 HALF/FLOAT；不支持的深度、多部件、分层颜色和多级 tile 明确拒绝。EXR chromaticities、RGBE PRIMARIES 以及浮点 KTX2 的 Rec.2020、ACES AP0/AP1、Display P3、Adobe RGB 声明进入色域和白点转换；KTX2 CIE XYZ 转为 RGB，未知声明拒绝。未声明色域按线性 Rec.709 处理，KTX2 unspecified 会给出警告。非线性 HDR 传递函数目前拒绝。负颜色通道钳为零并警告，非有限值与大于 1e12 的辐亮度拒绝，源 alpha 不作为天空覆盖率并给出警告。格式依据见 [Radiance 图像格式](https://radsite.lbl.gov/radiance/refer/Notes/picture_format.html)、[OpenEXR 场景线性说明](https://openexr.com/en/latest/SceneLinear.html)、[Khronos 色域定义](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.pdf)。

输入文件与解码浮点图分别最多 256 MiB，2:1、宽度不超过 16384；实际浮点预算允许 4096×2048，8192×4096 超预算会拒绝。运行时需要当前 SM 6.6、DXR 1.1 世界光照路径，能力不足会明确失败。HDR 材质贴图、立方体/数组环境、外部预过滤缓存、EXR 图层选择与完整色彩管理继续属于未完成项。

`scene.lighting.environment` 使用 `{file,sha256,intensity?,rotation?}`。相对路径、SHA-256、NGE1 布局、浮点与 SH 数值同时校验；坏依赖不替换现有场景。构建包收集环境依赖，整体场景导入搬迁到 `environment/<内容ID>.ngenv`，嵌套场景从全局库加入时先发布本地不可变副本。原生热更新重新上传天空、SH 和反射资源；环境内容、亮度和转向进入回放身份，磁盘搬迁路径不改变身份。

```powershell
# 原创样例，已纳入 assets:verify
node .incrementalTesting/assets/asset-environment-check.mjs
# 真实样例使用校验后的本地缓存，首次可加 --download
node .incrementalTesting/assets/asset-environment-public-check.mjs
```

`build/asset-import-smoke/environment/acceptance.json` 包含 44 项原生解码、场景身份、快照与失败原子性断言。HDR/EXR 对照像素一致；天空全周旋转一致，零亮度天空和表面全黑，HDR `[2,3,4]` 经显示映射得到 `[212,224,230]`，与 CPU 预期相差不到一个通道值。普通材质改变 36426 个像素，240 帧实际蒙皮绘制的角色改变 2900 个像素，镜面与粗糙金属改变 10506 个像素。依赖包截图一致，240 帧严格回放、实际 resize/restart、两次生命周期释放与基于已绘制帧进度的 GPU 环境热替换通过，原生校验错误为 0。

真实 [Royal Esplanade](https://polyhaven.com/a/royal_esplanade) 由 Greg Zaal / Poly Haven 提供，采用 [CC0](https://polyhaven.com/license)；来源、文件大小与 SHA-256 固定在 `tests/fixtures/environment-public.json`。HDR 和 EXR 的源峰值均为 49408，缩小后浮点峰值为 30185.625，两种格式原生画面逐像素一致；各自的零亮度控制、搬迁打包与 240 帧严格回放通过。证据、来源和署名保存在 `environment/public/`，原文件仅位于忽略的本地构建缓存。Powered by Poly Haven。

## 导入音频并加入场景

导入音频后，编辑器“外部资产”提供原生试听和加入当前场景。可选跟随实体、空间音效/环境声/音乐/对白/UI，以及单次或循环播放。音乐和 UI 默认非空间播放。CLI 继续使用 `assets attach`，通过 `--options` 指定 `entity`、`bus`、`spatial`、`loop`、`trigger`、`gainDb`、`pitch`、`offset` 和距离范围；`offset` 是相对实体原点的世界米制偏移。

`scene.audio.sounds` 将声音名映射到 `{file, sha256}`；文件相对场景目录，原生加载前校验路径、SHA-256、NGS1 头和有限样本。每个声音最多 64 MiB，场景最多 64 个声音和 256 MiB 唯一 PCM。导入的预览同样锁定场景引用和派生实体 UUID。场景构建会收集 `.ngsnd`，运行时从包内加载，无需原始音频目录或默认 bank。

声源位置读取实时 ECS；所属实体销毁后停止播放，碰撞遮挡使用实时物理代理。场景切换先停止设备线程，再替换声音库、响度目标和声源；声音引用与设置进入回放内容身份。长音乐仍为整段 PCM，流式播放和第三方音频工程转换继续属于剩余工作。

音频专项 `node .incrementalTesting/assets/asset-audio-check.mjs` 的证据位于 `build/asset-import-smoke/audio/acceptance.json`：19 项新增原生断言、40 项原有混音器检查通过；原生预览、声音依赖包、篡改拒绝和单声道/立体声库热重载通过。录制与严格回放均混音 192000 帧采样，PCM 哈希 `ba50e64543680201` 一致，回放 240 帧无差异。验证包在独立工作目录运行；通用宿主的人偶资源由 `--character` 明确提供，声音从包内读取。当前 `test:assets` 共 82 项通过；另有场景和编辑器定向用例通过。

## 场景整体导入与搬迁

```powershell
npm run assets -- import --in "D:/Resources/level.scene.json"
npm run assets -- import --in "D:/Resources/package/scene.json"
npm run assets -- open-scene --in "assets/imported/<场景ID>" --out assets/scenes
```

场景包保留项目/实体 UUID、关节和脚本属性中的实体引用；模型、骨骼角色、动作二进制、具名材质贴图、声音和传递脚本依赖一起收集。几何、角色和声音搬迁到较短的内容目录，SHA-256 保留在场景引用中，`scene-relocation.json` 记录原路径与目标路径。旧声音 bank 中被引用的声音转为直接场景声音引用；Voronoi 数据也随场景收集。整个导入使用临时目录，依赖失败不会发布半份场景。输入为引擎构建包时，先核对源清单声明的每个文件。

编辑器“外部资产”提供原生预览和“打开为新场景”。打开操作创建可编辑副本，原始导入包保持不可变。自定义组件与行为校验使用场景最近的 `gameplay/components.json`，并在并发请求中隔离依赖目录；命令、材质编辑、撤销、MCP 校验和场景 CLI 使用相同目录规则。脚本模块名称和实体引用在整体搬迁时保留，无需修改 Lua 中的名称。把外部场景合并为当前场景的预制体仍属于后续工作。

嵌套场景可直接从项目全局资源库加入模型、角色和音频。位于场景目录外的导入包会先逐文件校验并原子发布到该场景的 `imported/<资产ID>/`，再提交本地引用；具名材质贴图进入场景自己的 `textures/`。相同内容复用已有包，目标篡改、越界目录联接或文件链接会拒绝，过期场景版本及非法参数不会先搬迁包。撤销恢复场景引用；可复用的内容包保留。声音预检使用场景自己的自定义组件目录。

场景专项 `node .incrementalTesting/assets/asset-scene-check.mjs` 的证据位于 `build/asset-import-smoke/scenes/acceptance.json`：44 项原生断言验证自定义 Lua 执行、模型/角色/声音、48 块烘焙破坏拓扑和快照；移走源目录后，导入包、编辑副本与再打包场景均可原生运行，截图逐像素相同，240 帧严格回放和 PCM 哈希一致。Node 专项覆盖源包清单篡改、目录联接越界、并发自定义组件目录、旧声音 bank、烘焙破坏依赖与独立打包 CLI。修复了 Voronoi 快照误用网格拓扑，以及碎块脱离时丢失凸包的问题；爆破后存档继续 30 步与原运行逐步一致，烘焙几何内容参与存档身份，损坏拓扑被原子拒绝。原有玩法存档/继续运行定向检查通过；相关 30 项角色控制器检查、原有移动测试和严格回放也重新通过。

该专项现先从外部全局资源库加入资产，随后移走全局库，再进行整体导入和源目录移走验证；`externalCatalogRemoved` 与 `nestedAttachment` 均通过，证明包内的模型、角色、音频和脚本不依赖全局库。新增 3 项 Node 用例覆盖搬迁复用、编辑器声音加入/撤销、旧版本、非法设置和目标篡改/链接拒绝。

## 达到完整目标仍需完成

- 模型与角色：非同构骨架约束与接触优化、非均匀骨骼缩放、完整 Rig 编辑、更大面部目标预算与 NPC 状态控制器。单位/轴向转换、独立动作追加、静止姿态重定向和多个独立角色入场景已接通。
- 材质与环境：透明模型与精灵/粒子/体积效果的统一穿插、彩色透射阴影、皮肤/头发/透射、HDR 材质贴图、更多材质扩展、原始 ETC2/ASTC、数组/立方体纹理与分层 EXR。作者切线、MikkTSpace 自动生成与切线形变、HDR/EXR 全景环境光与预过滤、模型/角色的 alpha 混合、完整清漆贴图以及 DDS/KTX2/Basis/TGA/BMP 的常用 2D SDR 路径已接通。
- 场景：DCC 灯光与相机、父子实体、合并式预制体与实例化。引擎 `.scene.json` 和构建包 `scene.json` 的依赖搬迁、自定义脚本和编辑副本已接通。
- 物理：网格碰撞烘焙、凸分解、实际三角形 BLAS、胶囊体/精细角色碰撞、任意模型破坏与受约束布料绑定。角色代理拟合与控制器体型自适应已接通。
- 地形与植被：高度图、分层地形材质、散布、实例 LOD、裁切叶片/草、风与阴影代理。导入一棵树的网格不等于植被系统完成。
- UI：Sprite、图集、九宫格、字体导入/回退/排版；字体文件入口目前明确拒绝。
- 特效与模拟：第三方格式适配、曲线/缓存导入、特效挂点、布料/水体可视制作与运行组件。
- 工具：文件选择/拖入、批量导入、依赖视图、缩略图、重新导入的结构变更映射与基础模型/动作派生链更新、整体场景与模拟资产更新、局部 GPU 热替换和大型资源后台任务。模型/角色/贴图/声音/环境的基础版本生成、引用保持和撤销已接通。

资产增量入口 `test:assets` 最近一次运行 213 项全部通过，包含 6 项清漆用例、6 项来源身份和19项重新导入用例；场景校验定向10项通过。纹理/清漆定向 21 项、材质图等定向 33 项，以及基础 UV/切线/IOR/动画/纹理定向 52 项通过（这些定向用例与入口有重叠，不相加）。完整目标仍在进行中，未运行全量测试或完整发布打包。

## 增量验证

用户要求在提出新需求前只运行增量测试。资产入口不调用全量测试或完整 CI。

```powershell
npm run test:assets
npm run assets:verify
```

第二条只运行资产、动画、重定向、压缩几何、角色实例、材质与编辑器服务/传输相关 Node 用例，使用 `build/assets-host` 的依赖缓存构建宿主。原生部分覆盖双材质模型、resize/restart、mesh 路径回退、包篡改拒绝、多 skin/材质角色、形变曲线、压缩模型、裁切像素、动作追加、BVH 根位移及多角色同场景/64 实例预算。结果位于 `build/asset-import-smoke/`。原始 FBX 等复杂商店内容仍需逐个内容包增量验收。

2026-10-23 基线：108 项不同用例通过。`character-contracts.json` 有 74 项原生断言；`native-multi-character.json` 有 32 骨骼、2 材质分段及 480 次实际蒙皮/形变绘制；`character-pixels.json` 核对两种材质可见像素；`native-compressed.json` 记录压缩源的实际网格上传。`alpha-pixels.json` 中不透明 104652 像素、裁切 52326 像素，比例 0.5。`native-incremental.json` / `native-rejected.json` 保留静态路径恢复和篡改拒绝证据。完整目标仍在进行中，没有运行全量测试、完整 CI 或完整发布打包。

2026-10-27 贴图分辨率与 FBX：新增 8 项 `glb-texture-quality` 与 5 项 `fbx-import` 用例，`test:assets` 共 232 项，231 项通过。唯一失败项为 `scene-import` 的“packaged scene CLI runs with only its declared JavaScript modules”：工作区中他人进行中的 `engine/scene/index.mjs` 改动新增了对 `engine/material/layered.mjs` 的引用，而该用例的运行时副本清单尚未包含它，与本次改动无关。本次未改动原生与着色器代码，因此没有重新构建 `build/assets-host`，也未运行 `assets:verify`、全量测试或完整 CI；`DamagedHelmet` 的实际导入以 Node 管线复核（原分辨率、21.33 MiB、BC5 法线与遮蔽）。

2026-10-24 新增 15 项动作追加/重定向用例及 1 项缩放量化回归；此前 121 项合并增量入口通过，后续新增用例通过定向运行，共覆盖 124 项不同用例。`retarget-contracts.json` 有 79 项角色/动作原生断言；`retarget.json` 确认原生实际播放 `vendor_Raise`，`retarget-pixels.json` 记录相同场景中新动作改变 5560 个可见像素。`bvh-contracts.json` 有 82 项断言，`bvh.json` 确认原生播放 `walk_Motion` 并提取根位移。`native-preview-rejected.json` 确认即使其他身份字段一致，未列入依赖清单的预览头也会在加载前被拒绝。

同日补齐角色场景实例：新增 7 项 Node 用例，`test:assets` 共 63 项通过；资产合并入口共 131 项通过，另有场景定向 10 项通过。`instances-contracts.json` 有 37 项原生断言，覆盖独立片段/速度/时间偏移、根位移策略、模型与玩法偏移组合、共享几何、快照恢复、内容变更和失败加载的原子拒绝。

`instances-acceptance.json` 汇总 4 个独立角色、3 份角色包、2 份 GPU 几何、严格回放、依赖包原生运行及材质热重载结果；`instances.png` 核对四个角色区域均有双材质像素。`instances-native.json` / `instances-replay.json` 经 resize/restart 后的实例姿态一致，240 帧比较无差异。`instances-64.json` 实际绘制 64 个实例、30720 次分段调用，仍仅上传 2 份共享几何。`instances-hot.json` 验证新增角色分段材质后重新建立 GPU 材质表。最终原生资产专项使用有效的 `build/assets-host` 缓存通过；期间其他专项修改布料依赖时，过期构建被拒绝并重新构建，没有绕过输入校验。

单位/轴向专项新增 8 项 Node 用例，资产入口共 71 项、合并资产增量入口共 139 项通过。96 组正负轴、左右手系与转向组合验证同一基变换；覆盖非均匀静态节点、OBJ 绕序、Draco/meshopt 共用路径、多个 skin/inverse bind、形变、三次旋转切线、Rig/软组织长度、独立动作与 CLI/编辑器配置及身份兼容。

`build/asset-import-smoke/coordinates/acceptance.json` 与 `public-acceptance.json` 保存厘米/+Z-up/−Y-front 数据转换回引擎空间的对照。静态模型、32 骨骼合成角色及 21 骨骼 Cesium Man 的原版/转换版截图均逐像素相同（RGB 最大通道差 0）；各自 240 帧严格回放一致。合成角色有 877 项原生断言，公开角色有 615 项，覆盖 native inverse bind、形变后的蒙皮顶点及控制器。显式默认坐标与省略参数的身份一致；新角色预览配置仍按内容生成新包身份。原始来源 SHA-256、许可与标志声明继续保留。

体型拟合专项新增 7 项 Node 用例，当前资产入口共 78 项、合并资产增量入口共 146 项通过。`build/asset-import-smoke/controller/acceptance.json` 记录 30 项原生断言和原有移动测试，覆盖身高/宽度通行、顶头、台阶限制、相机与交互高度、ECS 缩放、NPC 脚底锚点、第一人称交互遮挡及快照恢复。拟合场景录制/严格回放一致；身高约 0.01806 米的模型在自适应相机下仍有 53963 个彩色像素。`legacy-package.json` 验证旧版 Cesium Man 包仍以原 1.8 米/0.3 米控制器尺寸加载成功。

```powershell
# 只使用已有公开样例缓存，不联网；同时复核自制和公开内容的坐标转换
node .incrementalTesting/assets/asset-coordinates-check.mjs --public
```

### 官方公开内容验收

```powershell
# 首次下载，保存来源、固定版本、SHA-256、作者和许可记录，再导入、追加动作、绘制和严格回放
node .incrementalTesting/assets/assets-public-check.mjs --download --native
# 后续使用已校验的本地缓存，不联网
node .incrementalTesting/assets/assets-public-check.mjs --native
```

该可选专项不进入默认增量入口，也不自动联网。当前真实内容是 [Cesium Man](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/CesiumMan)：© 2017 Cesium，CC-BY-4.0；Cesium 标志的单独声明保留在缓存中。来源版本 `8f492390ddc7a2ae221a1369ef560589cf8c70fb`，GLB SHA-256 `b7001eaeea8254bd44773bcd247e78696d94169388fbb2a1800fc69434e777d9`。21 骨骼角色的独立动作已追加并原生绘制 240 次；录制与严格回放摘要一致。`build/asset-import-smoke/public/acceptance.json` 包含完整记录，`native.png` 为原生画面。

实现依据：[glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)、[Assimp.js](https://github.com/kovacsv/assimpjs)、[Draco](https://github.com/google/draco)、[meshoptimizer](https://github.com/zeux/meshoptimizer/blob/master/js/README.md)、[sharp](https://sharp.pixelplumbing.com/)、[ffmpeg-static](https://github.com/eugeneware/ffmpeg-static)。转换器是编辑期依赖，原生运行使用烹制数据。
