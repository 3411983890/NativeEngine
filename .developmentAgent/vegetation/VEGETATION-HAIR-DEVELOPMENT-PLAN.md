# 千万级植被与高真实感毛发系统开发计划

- 版本：v1.0，架构设计与开发计划；**本文新增能力均未实现，不是性能或画质达标声明**。
- 日期：2026-10-25。
- 基线：NativeGPU 0.11.0，包含当前工作区的外部资产、材质、GPU 粒子、共享场和布料增量；工作树存在其他开发中的改动。
- 状态：计划已编制；各阶段必须凭实际代码、图像和报告逐项转为完成。逐阶段真实完成度登记在 [VEGETATION-HAIR-STATUS.md](VEGETATION-HAIR-STATUS.md)（2026-10-26 首次登记：S0/S1/S2/S4 基础合同、V0、V1、H0 已实现；V2/V4/H1/H3/H5 部分；其余未开始。同日第二批补材质贴图，第三批补 V3 主光阴影与叶片两级分布，第四批补贴图 mip 链与遮罩覆盖率；本文的验收口径未因这几批而放宽）。
- 目标：支持千万级世界植被数据、稳定的大视野森林与草地渲染，以及可从近景发丝扩展到多人角色的毛发系统。
- 阅读关系：本计划是本次开发的总体依据；[原植被方案](VEGETATION-PLAN.md) 保留为 2026-10-21 的历史设计参考，其中旧基线、禁用项和估时不能直接作为当前开发约束。

## 1. 总体决策与目标口径

采用两条有明确交付顺序的植被路径，以及一套共源资产的毛发路径：

1. 植被基础路径：区块实例、GPU 分层剔除、间接实例化、网格 LOD、远景 impostor、分层阴影。先形成可玩的森林与草地。
2. 植被高质量路径：枝叶部件复用、误差驱动 cluster 层级、远景聚合体素、受控骨骼风动。用于提升密集林冠和远景体积感，逐阶段替换表现不足的基础表示。
3. 毛发路径：统一 Groom 源资产，少量 guide 模拟、渲染发丝插值、覆盖率与透射阴影、近景 strands / 中景 cards / 远景 mesh 或 impostor；特写离线参考用于校准。

“最真实”在本计划中指：在已声明帧预算内，轮廓、光照、透射、运动和时间稳定性接近参考图像。提高几何数量只能改善其中一部分。实时生产方案与离线参考渲染分别验收。

### 1.1 千万级的计数合同

| 名称 | 定义 | 必须独立报告的原因 |
| --- | --- | --- |
| 世界实例 `worldInstances` | 世界资产索引内有稳定身份的树、灌木、草丛数量 | 表示内容规模，不表示同时驻留或可见 |
| 驻留实例 `residentInstances` | 当前 CPU/GPU 已加载的实例数量 | 决定内存、上传和工作队列规模 |
| 候选实例 `candidateInstances` | 粗剔除后进入细剔除的实例数量 | 决定剔除带宽与调度开销 |
| 可见实例 `visibleInstances` | 实际进入相机绘制列表的实例数量 | 不包含仅参与阴影的实例 |
| 草叶/叶片/枝叶部件 | 实例内生成或复用的细部数量 | 不能与整株植物混加冒充实例总数 |
| 绘制工作量 | draw、三角形、cluster、体素砖、覆盖样本、着色像素 | 判断是否真正减少了执行成本 |

最终容量验收使用 **30,000,000 个世界实例**，至少含 1,000,000 个木本实例，其余可以是草丛和地被；物种比例、区域面积、密度必须进入报告。另设 30M 木本索引压力实验，独立列出驻留与可见量，不能替代真实森林画质验收。

大视野镜头可以让数百万株植物共同贡献画面，但远处以聚合表示贡献覆盖和光照。**30M 株全部保持独立近景网格、全精度模拟与逐叶阴影，不属于本计划的实时性能承诺。** 30M 完全展开实例的 GPU 吞吐实验可做独立研究，必须如实报告机器和结果。

### 1.2 默认硬件与质量基线

暂沿用仓库设计基线：RTX 5060 Laptop、约 8 GB VRAM、Windows/D3D12，输出与内部渲染均为 1920×1080、目标 60 Hz。正式基准前记录实际 GPU、显存、TGP/功耗模式、驱动、温度、CPU、内存和磁盘；笔记本型号相同不等于性能相同。

另设 Radeon 890M 一类集成显卡为功能回退目标；高端桌面卡用于上限研究，不用其数据替代主目标。1440p/4K、120 Hz、动态分辨率和超分辨率分别建档，内部降分辨率必须公开记录。

## 2. 当前项目代码审计与可复用能力

本节来自 2026-10-25 工作区静态阅读，未运行原生性能验证。历史状态文档与活动工作树有差异时，以实际路径和运行证据为准。

| 能力 | 当前依据 | 本计划处理 |
| --- | --- | --- |
| 外部静态网格、角色导入 | `engine/assets/`、`engine/native/assets/`、`host/scene_loader.cpp`、`docs/EXTERNAL-ASSETS.md` | 复用导入、坐标、不可变身份与场景挂接；补物种/曲线资产适配 |
| VB/IB 与网格分段 | `renderer/d3d12_renderer.h` 的 `Vertex`、`DrawItem.mesh/firstIndex/indexCount`，`configureConvexMeshes` | 已有自定义网格基础；需适合物种共享、分页与批量实例的新资源接口 |
| 顶点 UV、多贴图 UV、顶点色 | `Vertex.textureUvs/color`，`shaders/material.hlsl` 输入和 UV 选择 | 已实现基础，不能再写成“只有平面 UV” |
| MASK/透明材质 | `MaterialUpload.alphaMasked/alphaBlended`、`material.hlsl` 的 `clip`、透明合成 include | 复用 MASK；另补叶片双面法线、厚度透射、覆盖率 mip 和专用 PSO |
| SM6.6 材质堆索引 | `material.hlsl` 的 `ResourceDescriptorHeap/SamplerDescriptorHeap` | 已有路径，仍须确认新管线描述符布局、容量、代际和回退 |
| 间接命令 | `renderer/gpu_effect_sprites.cpp`、`renderer/particles.cpp` 已调用 `ExecuteIndirect` | 复用签名/屏障/生命周期经验；通用植被 GPU Scene 和索引实例间接命令尚需建设 |
| 普通场景绘制 | `renderer/d3d12_renderer.cpp` 仍逐 DrawItem 调用 `DrawIndexedInstanced(...,1,...)` | 增加专用批处理路径，不能把千万实例推入 DrawItem 列表 |
| Mesh Shader | `renderer/mesh_bindless.cpp`、`shaders/mesh_shaders.hlsl` | 存在示例，不等于完整虚拟几何；先校准与顶点路径材质/绕序一致性 |
| TLAS | `renderer/d3d12_renderer_internal.h` 的 `MaxInstances=1024`；外部资产当前使用代理阴影 | 需明确共享几何与静态/动态结构管理，不能只增加常量 |
| GPU 模拟与共享风 | `engine/native/fields/`、`gpu_field_bridge.cpp`、`gpu_cloth_bridge.cpp` | 复用租约、epoch、fence、风速单位及不可用回退合同 |
| 布料 XPBD | `engine/native/cloth/`、`shaders/cloth*.hlsl` | 可复用资源调度与数值纪律；一维发丝约束和扭转模型需要独立实现 |
| HZB、通用表面 motion vector/TAA | 当前未找到成熟的全场景合同与接入证据 | 新建共享阶段；粒子局部时域累积不能代替表面 TAA |
| 植被/Groom 运行时 | 未找到专用物种实例、Groom 包或 strand 渲染链路 | 待开发；纹理或通用粒子数量不计为完成 |

### 2.1 对旧植被方案的修订

- “任意网格、顶点 UV、MASK、所有 ExecuteIndirect 都不存在”已不适用于当前工作树；前置应改为复用现有能力并构建通用批处理。
- 原来的 10k 木本 + 50k 草是早期可见切片门槛，扩展到 30M 世界实例时，分区、分页和 I/O 预算进入主干。
- “草不投影、树冠椭球代理”仅适合初始回退档。高质量档必须解决近景接触阴影、林下斑驳光与动态风影。
- “叶片永远不用 cluster/体素”和“远景始终卡片”不作为终局限制；需比较几何叶片、部件实例和聚合体素的画质/成本。
- 区块合并阴影代理可降低 TLAS 实例数，但仍有三角形、BLAS 存储和更新成本；共享物种 BLAS 与合并代理必须实测比较。
- 未经验证不能声称“写入深度即可免费获得正确阴影”；深度顺序、重建、双面偏置、接触和薄表面自阴影均需验证。

## 3. UE、MetaHuman、Blender 与论文选择

### 3.1 技术比较与采用决定

| 技术来源 | 公开机制 | 本项目采用方式 | 边界 |
| --- | --- | --- | --- |
| UE ISM/HISM [R1] | 共享网格实例与静态层级剔除 | 木本实例树、GPU Scene、每实例屏幕误差 | 不复制 Actor 开销；不假定普通 ISM 自动解决叶片 overdraw |
| UE Nanite Foliage [R2] | 部件复用、远景聚合体素、骨骼驱动形变 | 作为高质量路径的三项研究与交付方向 | 官方仍标实验；本计划独立实现数据和管线，不声称复刻 UE 全部能力 |
| UE Groom [R3] | guide 模拟、插值、密度体素、主可见性、光照和合成 | 作为专用毛发渲染阶段划分依据 | 每个阶段独立计时；透明度不是普通不透明 GGX 的附加开关 |
| MetaHuman [R4/R5] | Groom 资产、strand/card/mesh 质量档、卡片生成 | 同源 LOD、分组、头皮绑定和卡片生成工具 | 不承诺 MetaHuman DNA、RigLogic 或专有角色资产完整兼容 |
| Blender 粒子 Hair [R6] | 父发、children、曲线、造型与离线渲染 | 兼容已有制作内容，导出求值后的曲线和属性 | 粒子上限不是游戏实时吞吐量；不移植粒子生命周期作为长发拓扑 |
| Blender Curves/Geometry Nodes [R7] | 头皮表面、UV、guide 插值与曲线变形 | 新内容优先采用，建立版本固定的 exporter | 保存求值资产和必要属性，不要求运行时执行完整节点树 |
| Frostbite strand 管线 [R8] | 发丝可见性与 Deep Opacity Map 等 | 参考覆盖样本、延迟着色和自阴影组织 | 演讲场景时延不能直接作为本机预算 |
| Marschner 2003 [R9] | 纤维反射/透射的物理模型 | R/TT/TRT 方向散射模型与验证依据 | 单纤维模型不等于整束多次散射 |
| Chiang 等 2016 [R10] | 生产级可控毛发/皮毛散射 | 离线参考与实时近似校准依据 | 完整路径追踪模型不直接等价于实时实现 |
| XPBD 2016 [R11] | 带 compliance 的约束动力学 | guide 长度/弯曲和碰撞求解基础 | 不能因此声称具备完整扭转杆、自碰撞与 CCD |
| Strand LOD 2024 [R12] | 面向发丝的 LOD 与外观保持研究 | H5 比较 strand 简化后的覆盖和散射误差 | 先复现实验，再决定是否替换基础 LOD |
| Hair Meshes 2024 [R13] | 紧凑发型表示与 GPU 即时发丝生成 | 多角色性能研究支线 | 作者页面明确列出专利与使用联系要求；研究比较不默认引入产品主干 |

### 3.2 研究结论

UE 新植被路线的重要启发是：重复枝叶应复用，远处细碎几何需要聚合，风动必须有可界定的形变范围。基础实例化卡片适合先交付；几何部件与体素值得进入后续主线，不能把早期代理当作最高画质方案。[R2]

毛发同时需要正确的覆盖率、切线方向散射、束内遮挡和运动。仅把草 shader 换颜色，或增加透明卡片数量，无法完整实现这些目标。UE 公布的阶段划分给出了可分析成本的工程参照。[R3]

Blender 用于制作发型和离线参考；引擎接收求值后的明确数据。新曲线工作流提供表面与 UV 绑定入口，旧粒子内容通过适配器转换，两种来源统一进入 Groom 包。[R6/R7]

## 4. 共用运行时架构

```text
资产制作：Blender / glTF / Alembic 适配器 / 原创程序化资产
       ↓ 单位、轴向、拓扑、属性与许可检查
离线 cook：Species / Region / Groom / Binding / Cards / Reference
       ↓ 内容寻址包、分块目录、版本与哈希
宿主：场景事务、区域请求、角色实例、共享风、权威交互
       ↓ 稳定句柄与本帧参数
GPU Scene：常驻页、实例表、物种表、描述符、上一帧状态
       ↓
区域粗剔除 → 实例/cluster 细剔除 → LOD/表示选择 → 间接工作列表
       ↓                                       ↓
植被几何/聚合体素                      Groom 模拟/插值/strand/card
       ↓                                       ↓
深度、运动矢量、可见性、阴影 → 表面/纤维光照 → 透明合成 → 时域重建
       ↓
非阻塞计时、容量、缺页、画质与回放证据
```

### 4.1 GPU Scene 与间接绘制

- 资源表保存网格、材质、实例页、包围体、变换和上一帧数据；CPU 只更新脏页、相机、交互与区域请求。
- 可见列表按 PSO、材质类、物种/网格、LOD 分桶。间接命令使用 GPU 写入的实例计数；不逐帧回读再提交。
- 区域剔除先决定需要访问哪些页；细剔除工作随候选集合增长，不每帧扫描全部 30M 世界实例。
- wave/group 局部压缩配合前缀和，减少全局原子争用。零工作量、桶容量和索引溢出必须有处理。
- 网格顶点路径是可靠基线；Mesh Shader 路径使用独立能力开关，与基线比较几何、材质和图像。
- 新增跨队列工作必须明确 graphics/compute fence 与资源状态；先允许单 graphics 队列实现，再依据重叠收益引入异步。

### 4.2 HZB 与形变保守性

先对上一帧深度金字塔做保守剔除，未确认可见性或受相机位移影响的对象进入重测；当前帧深度可用于第二阶段。必须明确定义正向/反向 Z、mip 选择、深度归约与越屏处理。

相机切换、传送、分辨率变化和大运动时清除遮挡历史。植被包围体包含最大风动，毛发包围体包含模拟位移；使用静止包围体造成的漏绘属于错误。遮挡效率低只影响性能，不能以错误剔除换取指标。

### 4.3 时域与透明合同

表面 motion vector 必须包含相机、刚体、上一帧风动、骨骼与 guide 变形。LOD/页替换、透明覆盖突变和新出现区域写入 reactive/disocclusion 信息。历史包含曝光与抖动约定，不能只做静态指数混合。

TAA 是时间稳定性工具，不能修补错误的 alpha mip 或粗劣发丝覆盖。建立静止、平移、旋转、风动、逆光五类序列检查闪烁、拖影和细节损失。

## 5. 植被资产、世界分区与流送

### 5.1 内容组织

```text
assets/vegetation/species/<digest>/
  manifest.json / materials.json / meshes.bin / lods.bin
  assemblies.bin / wind-rig.bin / impostors/ / voxels/
  collision.bin / shadow-proxies.bin
assets/vegetation/regions/<digest>/
  manifest.json / spatial-index.bin / pages/*.bin
  terrain-reference.json / placement-rules.json
assets/grooms/<digest>/
  manifest.json / curves.bin / guides.bin / interpolation.bin
  bindings/ / cards/ / lods.bin / materials.json
```

未实现的 section 不填伪数据：manifest 显式声明 representations，加载器拒绝声明与内容不一致的包。相同源资产、选项与工具版本生成相同字节；记录散布种子、LOD 工具版本和参考烘焙配置。

### 5.2 场景与编辑器合同

场景保存 patch/region 引用、物种集合、密度、种子、掩码、LOD 档位和区域变换。可用 vegetation kind 或受版本控制的组件实现，进入 V0 时对照活动 schema 决定；**不提前占用 schemaVersion=3**，防止与其他专项迁移冲突。

场景字段变动同步 JSON Schema、Node 校验器、原生 loader、编辑器/AI 工具面、文档与增量测试。百万实例不展开成实体数组；局部删除、砍伐、手工移动使用稳定实例 ID 的稀疏覆盖层。

编辑器支持笔刷、密度/坡度/高度/湿度掩码、物种预览、区域统计、构建进度、撤销和 cook 诊断。构建产物先发布后原子切引用；失败保留旧包。

### 5.3 地表与生态散布

先定义统一 `SurfaceSampler`：高度、法线、表面类型、遮罩与版本。平面适配器用于最早切片，静态网格 BVH 投射适配器用于已有场景，terrain 高度场由独立地形模块提供。

散布采用分区固定种子与局部稳定序号；局部重建不能改变邻区。支持物种间距、成簇与空隙、坡向、林缘、道路排除、落叶/枯草和生长阶段。检查跨区边界重复与密度接缝。

### 5.4 实例压缩与内存

建议区域局部量化位置、量化 yaw/倾斜、缩放、物种索引、稳定 ID、风相位和颜色。目标平均 **24–32 B/实例**，具体布局在 V0 冻结并给出单位与误差上限。64 B 浮点参考格式保留用于离线对照。

以十进制 MB 计算：30M × 32 B = 960 MB，64 B 则为 1,920 MB，尚不含可见列表、网格、纹理、阴影和双缓冲。不能以“共享网格”推导总显存很小。

世界完整索引留在磁盘/CPU 分层目录；GPU 只驻留工作集。细页建议以 64–256 KiB 开始实测，区域尺寸随密度调整；目录分页与几何页独立。8 GB 档暂给植被内容工作集 1.2 GB 软上限、1.6 GB 硬上限，毛发 256 MB 软上限，最终受整机可用显存与其他系统预算约束。

### 5.5 流送状态与防卡顿

页状态明确为 `Unloaded → Requested → Reading → Decoding → UploadQueued → Resident → Retiring`；失败和取消单独记录。I/O、解码和上传均有每帧预算，临时缓冲、在途页与 retired 页计入峰值内存。

相机附近预取、运动方向预测、LRU/重要度淘汰并用滞回防止往返抖动。缺页保留父级聚合表示或 impostor；禁止同步磁盘等待和同步 GPU 回读阻塞主帧。fence 完成前不能复用页槽或描述符。

预取/退化事件与页错误必须报告；冷启动允许流送画质逐步收敛，但不能将有洞的画面记成完成帧。相机瞬移场景分别统计恢复时间、缺页数和最低表示质量。

## 6. 植被表示、材质、风动与光照

### 6.1 基础表示与 LOD

树干/粗枝采用几何 LOD；叶片采用实心薄几何与 MASK 卡片两个变体对照。草按 clump 管理，近景少量几何叶片，中景简化，远景通过密度/地面覆盖表示。

LOD 依据屏幕误差与覆盖率，不把固定距离作为唯一规则：

```text
errorPixels ≈ worldError × instanceScale × viewportHeight
              / (2 × tan(fovY/2) × conservativeViewDepth)
```

`worldError` 来自受材质/边界约束的实际简化偏差，不使用 AABB 对角线代替。另记录叶面积、轮廓与透射误差；仅保几何距离仍可能让树冠变稀。切换加滞回，过渡双绘的额外工作量进入报告。

草密度使用持久哈希排序与嵌套子集，阈值随屏幕尺度改变；移动相机不能改变每株的随机身份。减少密度时补偿覆盖必须受上限控制，避免少量草叶变成宽片。

### 6.2 部件实例与聚合体素

离线把可重复枝叶定义为部件，在树级与部件级共享数据。部件层级保持变换、材质、动画关联和误差界；运行时按需求值，避免把每片叶子都物化成全局实例。

远景体素保存占据/覆盖、消光或透射、平均色、法线分布或方向统计、材质混合与动画关联；不能简单以实心立方体替代半透明林冠。三角形/体素误差统一到屏幕覆盖和外观评估，分别分桶。

体素研究必须检查低太阳角、林冠空隙、侧视轮廓、风动过渡和多物种交界。允许基础 impostor 作为设备回退，但高质量档是否采用体素由 V6 证据决定。[R2]

### 6.3 叶片与草材质

- 增加明确双面 PSO 与正反面法线处理，背面透射来自厚度/吸收参数；不得只把背面改成正面 GGX。
- 叶色、厚度、粗糙度、蜡质高光、叶脉法线和季节变化分开存储；法线 mip 与 alpha coverage mip 保持稳定。
- 保留 albedo 线性/色彩空间语义，卡片边缘 dilation，BC 压缩前后比较 coverage。
- 近景接触 AO、地面混色与叶簇遮蔽不能与 GI/阴影重复相乘造成过黑。
- alpha 深度预 pass 是否启用由 overdraw/带宽实测决定；深度、颜色与阴影 pass 的 alpha/风动结果一致。

### 6.4 风动与交互

基础档使用有界程序化摆动；高质量木本采用树干/枝条层级骨骼或等价刚性段，叶片附属部件只处理高频细动。共享风以 m/s 输入，树种参数控制刚度、阻尼和方向响应。

相同风快照供草、树、毛发和布料采样，遵循现有租约/epoch/fence 合同。缺场使用显式 uniform fallback，并清除过时历史。距离降低模拟频率、骨骼层级与叶片细动，过渡时保存连续速度。

玩家交互先用有限交互体/局部位移场推动草与灌木。砍树、燃烧和倒伏是稀疏权威事件：转换附近个体为独立玩法对象，不能让全部植物持续进入物理世界。接触与视觉弯曲的权威边界写入存档合同。

### 6.5 阴影与 GI 分层

| 层级 | 方案 | 验收重点 |
| --- | --- | --- |
| 草/近景叶片 | 有预算的局部光栅阴影或细化代理；必要时主光专用 | 叶片缝隙、接触阴影、风动一致 |
| 中景树冠 | 缓存阴影页/级联阴影原型，或共享物种 BLAS + 受限精细 RT | 更新率、斑驳光、偏置和时域稳定 |
| 远景林冠 | 聚合消光、低频缓存和大尺度云/地形遮挡 | 不把林冠涂成实心黑团 |
| GI | 近景精细几何/代理，远景低频冠层 | 能量、色溢、空隙与更新成本 |

先实现主光局部精细阴影，保留原 DXR 代理回退。缓存光栅与共享 BLAS 在 V3 做同资产 A/B，再冻结主干选择；完整虚拟阴影贴图是独立后续项目。

合并 chunk BLAS、共享 species BLAS、不同 LOD shadow mesh 三者测总显存、构建/更新与 trace 时间。单纯减少 TLAS 实例数不是唯一优化指标；动态风动不能强迫全林每帧重建。

## 7. Groom 数据、制作与导入

### 7.1 统一曲线资产

Groom 包至少保存 curve offsets/counts、曲线点位置、根到尖半径、弧长参数、分组、稳定 ID、头皮 root UV、切线/参考方向、材质参数、guide 关联、插值权重、LOD 关系和包围体。

区分源控制点、重采样曲线点、求解 guide 点与渲染段。长度使用米，半径与直径不可混用；单位转换同时作用于发丝宽度、碰撞半径、绑定偏移与物理长度。拒绝 NaN、重复 offsets、越界、全零长度和异常密度。

材质分组支持头发、眉毛、睫毛、胡须与绒毛。各自允许不同模拟/阴影档位；眉睫首先遵循面部绑定与碰撞，避免用长发参数。

### 7.2 Blender 与 Alembic 适配器

固定 Blender 可执行版本与 exporter 版本；以 evaluated Curves 和最终世界/对象矩阵为输入，导出头皮 UV、根位置、宽度和分组。旧粒子 Hair 的父发/children 经受控求值转成同一曲线结构，记录求值配置和 seed。

Alembic curves 作为 DCC 交换格式适配，不假设现有 glTF 导入天然携带 Groom。第一阶段可用明确 JSON + 二进制 exporter，随后加入固定版本 Alembic worker。缺失半径、root UV 或分组要给出诊断；默认值必须记录。

制作工具支持密度、分缝、主/次发束、卷曲、frizz、flyaway、根尖色和宽度变化；离线生成稳定渲染发丝与 cards。镜像/负缩放须同时修正切线参考方向。

### 7.3 头皮绑定与插值

绑定记录目标网格内容哈希、LOD/拓扑版本、三角形索引、重心坐标、根偏移和局部参考框架。角色骨骼、morph 和局部软组织完成后更新根，guide 再求解，最后插值渲染发丝。

每根渲染发丝使用少量邻近 guide 权重，分缝和组边界限制插值，避免跨分缝平均。处理不同弧长、长度保持、卷曲参考框架和根附近刚度。绑定失效禁止静默吸附到错误三角形，可明确降为骨骼刚性绑定并报告。

## 8. 毛发渲染、模拟与 LOD

### 8.1 主可见性和透明度

H1 从静态 strands 扩展为相机朝向 ribbon/解析段；每段携带切线、宽度、稳定 ID、根尖坐标和深度。亚像素覆盖按解析估计或多样本处理，不能无条件把每根发丝扩宽到 1 px。

生产路径先实现 tile 分桶和有上限的多层样本缓冲，按深度组合透射；`K=4/8` 作为实验参数。溢出采用后层统计合并或明确低档近似并计数，不能直接丢弃。Weighted blended OIT 可作预览回退，不能视为高保真有序透射。

全屏 PPLL 只作参考实验，严格计入节点池显存。1080p 的 4 层 × 16 B 样本约 132.7 MB，8 层约 265.4 MB，尚不含计数与历史，因此优先评估紧凑样本、tile 池或稀疏分配。保留完整内存账本。

植被 MASK 写入不透明深度；头发半透明层保留多层深度。两者与玻璃、烟和粒子合成时明确前后关系，不用“头发永远最后画”掩盖穿插。

### 8.2 纤维光照

近景以 Marschner/Chiang 思路建立实时纤维 BSDF，包含 R、TT、TRT 主分量、纵向/方位粗糙度、切鳞倾角、吸收、切线方向和根尖参数。使用离线 LUT 或拟合时记录生成脚本、误差范围和参数域。[R9/R10]

黑发/金发/红发、顺光/逆光分别对照离线参考；浅发需要束内多次散射近似。增加多次散射前先检查能量守恒与透射，避免只加常量亮度。卡片 shader 读取 strand direction/深度/coverage，并沿用同一材质参数语义。

### 8.3 自阴影与体积

先对主光实现 Deep Opacity Map 或分层透射阴影，记录随深度累计的消光；随后评估每 Groom 稀疏密度体积服务其他灯与 AO。[R3/R8]

头发投影到脸部、眉睫细影、束内遮挡分别验收；阴影精度随屏幕尺寸缩放。不可见 Groom 是否继续更新根据阴影贡献选择，不能直接用视锥结果关闭所有模拟/阴影。

逐发丝 DXR 几何仅作为受限高质量实验；默认不要求每帧全量构建动态发丝 BLAS。毛发密度体积可以复用场服务生命周期，但其方向统计、消光和坐标合同独立于烟密度。

### 8.4 Guide 模拟

CPU 小规模参考求解先覆盖固定根、长度、弯曲、阻尼、重力和解析胶囊/球碰撞。GPU 求解按 groom/group 批处理，固定 tick，图着色或 Jacobi 双缓冲防止约束写入竞争。

长发增加扭转/参考框架和局部形状保持，再评估离散弹性杆类模型；XPBD 只作为首版基础。碰撞使用头、肩、胸部、手臂代理与后续 SDF；快速运动需要 swept/CCD 子步，不能用最终位置投影宣称 CCD 完成。

自碰撞先用 guide 空间网格与束级排斥，控制团块和穿束；不做所有渲染发丝两两碰撞。传送、暂停、恢复、时间大步长、LOD 切换和设备重建必须定义状态迁移。

GPU 视觉模拟不默认承诺跨硬件逐位确定；权威状态、固定输入、guide 初始化和参考 CPU 路径单独管理。若需要严格存档续跑，保存完整 guide 位置/速度/约束历史及资产指纹，并验证实际保存后端的合同。

### 8.5 LOD 与多人角色

| 表示 | 暂定负载 | 切换依据 |
| --- | --- | --- |
| Hero strands | 50k–150k 渲染曲线、每条 8–32 段；1k–4k guides | 发型屏幕大小、覆盖复杂度、总预算 |
| Reduced strands | 嵌套采样 + 段数简化 + 宽度/散射补偿 | 覆盖与高光误差 |
| Cards | 离线多层卡片、切线/覆盖/深度图 | 中景体积与轮廓误差 |
| Mesh / impostor | 简化体积与头皮覆盖 | 远景或低端 |

这些数量是测试输入，不是全部发型必须满足的硬编码范围。短发、卷发、辫子与长直发各有 profile；按总屏幕覆盖分配预算，近景角色优先，远景降低模拟频率和 guide 层级。

切换前后比对发际线、发束轮廓、覆盖率、能量与动态连续性；不能只检查“发丝数量变少”。H5 研究外观保持的 strand LOD，[R12] 作为主要论文候选；Hair Meshes [R13] 保持独立研究支线，不作为默认导入格式或交付前置。

## 9. 性能预算与验收场景

### 9.1 联合帧预算（暂定目标，非实测）

| 项目 | 1080p / 60 Hz 主目标 |
| --- | --- |
| 全帧 GPU P95 / P99 | ≤16.67 ms / ≤20 ms |
| 植被总 GPU P95 | ≤5.0 ms，包含剔除、几何/体素、材质与植被阴影增量 |
| 1 个 Hero Groom 总 GPU P95 | ≤2.5 ms，包含模拟、插值、覆盖、阴影、着色与合成 |
| 公共时域重建 P95 | ≤1.5 ms，公共 pass 不重复计入子系统总和 |
| 其余场景与余量 | 约 7.67 ms，必须由真实联合场景复核 |
| 植被 CPU 提交增量 P95 | ≤0.5 ms；无全世界逐实例循环 |
| 稳态主线程同步磁盘/GPU 读回等待 | 0 次；Present 等待单独记录 |
| 内容导致的单帧卡顿 | 预热路线无 >50 ms 帧；20–50 ms 帧逐项解释 |

子 pass 的 P95 不能直接相加证明整帧 P95；报告保留逐帧时间序列和依赖图。冷启动、shader/PSO 创建、初始 BLAS 构建和流送路线独立统计，不用预热数据掩盖首帧卡顿。

### 9.2 负载矩阵

| 编号 | 场景 | 目标与必要证据 |
| --- | --- | --- |
| V-A | 单物种 1k，近/中/远三机位 | UV、MASK、双面、实例变换、阴影和原路径一致 |
| V-B | 10k 木本 + 50k 草丛 | GPU 批处理与 LOD 首次规模门槛 |
| V-C | 1M 混合实例 | HZB 林下遮挡、山顶视野、转向时的时间稳定 |
| V-D | 30M 世界实例与受限工作集 | 地图覆盖、驻留/可见口径、冷/热流送、显存峰值 |
| V-E | 开阔草原、混交林、密针叶林 | 草 overdraw、树冠聚合、不同物种成本 |
| H-A | 静态曲线与旋转灯 | 发丝 coverage、切线高光、透射与参考对比 |
| H-B | 短发/长直发/卷发/辫子 | guide、分缝、碰撞、肩部接触与形状保持 |
| H-C | 1 hero + 8 中景 + 64 远景角色 | 总预算、表示分配、发际线与 LOD 连续性 |
| J-A | 森林 + 草 + 角色 + 毛发 + 布料 + 粒子 | 风快照共享、遮挡/透明合成、联合帧预算 |
| J-B | 快速转向/传送/暂停恢复/设备重建 | 无失效资源、历史污染、持续漏绘或旧页访问 |

每场景至少 300 帧预热和 1,800 帧采样，重复 3 次；流送场景另含冷缓存路线。30 分钟稳定性属于最终专项，首次开发不自动执行。异常分布和完整采样文件保留。

### 9.3 画质门槛

- 建立原创/可分发测试资产，固定曝光、灯光、相机和参考渲染配置；Cycles 路径追踪结果作为参考，说明 BSDF 差异。
- 静态图评价轮廓 IoU、覆盖率、ROI 色差/SSIM，以及关键局部裁剪；动态序列评价闪烁、拖影、LOD pop、风动相位与反遮挡。
- 几何误差、覆盖误差、透射误差和主观评审分别记录。参考阈值在阶段开工时基于夹具冻结，失败后不能临时放宽。
- “没有 NaN”“截图发生变化”“实例达到 30M”均不能单独证明真实感或流畅。

## 10. 分阶段工作包与依赖

### 10.1 公共底座

| 阶段 | 工作 | 完成证据 | 依赖 | 工程工作量估计 |
| --- | --- | --- | --- | --- |
| S0 基线与计量 | 更新能力审计，冻结计数、机器、报告和参考资产 | 基线报告、固定机位、资源账本 | 无 | 1–2 人周 |
| S1 GPU Scene | 共享物种资源、页句柄、实例表、GPU 索引实例间接命令 | 1k/10k 实例正确，零计数与溢出合同 | S0 | 3–5 人周 |
| S2 深度/HZB | 深度金字塔、保守细剔除、两阶段重测 | CPU 几何可见参考对照，无错误漏绘 | S1 | 2–4 人周 |
| S3 运动与时域 | 表面 motion vector、风/蒙皮历史、透明 reactive、TAA | 五类运动序列与重置证据 | S1；S2 联合 | 4–7 人周 |
| S4 预算服务 | 可见/阴影/模拟预算，异步统计，PSO 预热和联合账本 | 无读回阻塞，档位原因可追溯 | S1–S3 | 2–3 人周 |

### 10.2 植被主线

| 阶段 | 工作 | 可交付结果与验收 | 依赖 | 工程工作量估计 |
| --- | --- | --- | --- | --- |
| V0 包与散布 | species/region 包、SurfaceSampler、稳定 ID、schema 接入 | 可复现包、篡改拒绝、区域接缝和局部重建 | S0 | 2–4 人周 |
| V1 基础可见切片 | 叶片双面/透射、1k GPU 实例、三种 LOD | 三距离真实图像与逐实例参考一致 | S1、V0 | 2–4 人周 |
| V2 草与规模 | 草 clump、覆盖 mip、稳定降密度、1M 压力 | V-B/V-C 无列表溢出，overdraw 可解释 | V1、S2、S3 | 3–5 人周 |
| V3 阴影与风 | 层级风、共享场、主光精细阴影、GI 代理 | 林下斑驳光、风影和阴影方案 A/B | V2 | 4–7 人周 |
| V4 分页流送 | 区域索引、异步读取/解码/上传、缺页回退 | V-D 30M 世界实例与工作集约束 | V2、S4 | 4–7 人周 |
| V5 部件层级 | 枝叶共享、真实误差 cluster、受控形变界 | 几何体积/磁盘/显存/剔除成本对比 | V3/V4 | 5–9 人周 |
| V6 聚合体素 | 体素 cook、方向统计、表示切换、专用光栅 | 远景林冠与卡片基线 A/B，画质通过 | V5、S3 | 6–10 人周 |
| V7 工具与最终验收 | 笔刷/稀疏覆盖/预览、30M 路线与联合场景 | 世界规模、画质、P95/P99 和恢复证据齐全 | V3/V4；高质量档另需 V6 | 3–5 人周 |

### 10.3 毛发主线

| 阶段 | 工作 | 可交付结果与验收 | 依赖 | 工程工作量估计 |
| --- | --- | --- | --- | --- |
| H0 Groom 工具 | 曲线包、Blender exporter、重采样、头皮绑定、静态 cards | 单位/根尖/分组和角色绑定可见 | S0、现有资产链路 | 3–5 人周 |
| H1 静态 strands | 段/ribbon、coverage、tile 多层样本与合成 | H-A 的轮廓与透明覆盖稳定 | H0、S1 | 4–7 人周 |
| H2 纤维光照 | R/TT/TRT 近似、吸收、Deep Opacity、参考图 | 黑/金/红发顺逆光和脸部投影 | H1、S3 | 4–7 人周 |
| H3 Guide 动态 | CPU 参考、GPU 求解、插值、胶囊/肩部碰撞、状态迁移 | H-B 无持续爆炸/脱根，报告穿透与伸长 | H0/H1、共享风 | 4–7 人周 |
| H4 高级交互 | 扭转/形状保持、束级自碰撞、CCD、体积透射 | 长发快速转向与卷发结构保存 | H2/H3 | 4–8 人周 |
| H5 外观 LOD | strands/cards/mesh 共源生成、guide 降档、多角色预算 | H-C 发际线、覆盖、能量与时间连续 | H2/H3、S4 | 4–7 人周 |
| H6 生产工具 | 参数面板、Groom 预览、诊断和联合验收 | J-A/J-B，受限显存与设备恢复 | H4/H5、V3 | 3–5 人周 |

工作量含实现、专项验证、报告与修复，不是日历承诺。公共底座约 12–21 人周，完整植被约 29–51 人周，完整毛发约 26–46 人周；范围合计约 67–118 人周，不含大规模内容制作、完整地形系统、完整虚拟阴影系统和离线路径追踪器开发。

首个生产基础版可以在 V3/V4 + H2/H3/H5 的有限范围交付，高质量几何/体素与高级毛发碰撞随后推进。若未来多人并行，必须明确模块所有权与集成关口；本文件不自动授权启动其他代理或任务。

### 10.4 阶段决策门

1. S1 不能消除逐实例 CPU 提交时，不扩大实例规模。
2. 叶片成本被像素/阴影主导时，优先优化覆盖与阴影，不以增加 cluster 隐藏瓶颈。
3. 流送压力不能靠无限增大缓存解决；缺页表示与上传预算必须同时成立。
4. V6 若没有比 impostor/几何基线更好的质量与成本组合，保留研究结果并暂不启用默认档。
5. H1 的覆盖、溢出和显存不达标时，先保留 cards 可见成果，不宣称高保真 strand 完成。
6. 所有研究分支均以同资产、同镜头、同曝光比较；发现基础假设不成立时记录方案修订和剩余工作。

## 11. 拟新增文件与接口边界

以下是计划落点，均需阶段开始时对照活动工作树确认，不代表文件已存在。

| 层 | 拟新增/修改 | 职责 |
| --- | --- | --- |
| 离线植被 | `engine/vegetation/`、`tools/vegetation.mjs`、`tools/offline/vegetation/` | species、散布、区域页、LOD/impostor/体素 cook |
| 离线毛发 | `engine/groom/`、`tools/groom.mjs`、`tools/blender/export-groom.py` | 曲线验证、绑定、guide、cards、参考烘焙 |
| 原生资产 | `engine/native/vegetation/asset.*`、`engine/native/groom/asset.*` | 有界解析、哈希、格式与坐标合同 |
| 世界运行时 | `engine/native/vegetation/world.*`、`streaming.*` | 分区、覆盖层、请求优先级和实例句柄 |
| 毛发求解 | `engine/native/groom/reference.*`、`gpu_solver.*` | CPU 参考、GPU guide、碰撞与状态 |
| 公共渲染 | `renderer/gpu_scene.*`、`indirect_draw.*`、`hzb.*`、`temporal.*` | 资源页、命令、深度与历史 |
| 植被绘制 | `renderer/vegetation.*`、`vegetation_shadow.*`、`vegetation_voxels.*` | 实例、部件、阴影与聚合表示 |
| 毛发绘制 | `renderer/groom.*`、`groom_visibility.*`、`groom_shadow.*` | 覆盖样本、纤维光照、透射合成 |
| 着色器 | `shaders/vegetation*.hlsl`、`groom*.hlsl`、公共 include | 数据布局、CS/VS/MS/PS；登记 manifest |
| 宿主 | `host/vegetation_scene.*`、`groom_scene.*` 与 `main.cpp` | 场景配置、共享风、实际队列上的调度 |
| 场景/编辑器 | schema、Node validator、C++ loader、editor、AI 工具 schema | 事务、属性和诊断一致性 |
| 专项证据 | `tools/vegetation.ps1`、`tools/groom.ps1`、`tools/vegetation-hair-joint.ps1` | 隔离构建、固定负载、报告与截图 |

资源接口必须使用代际句柄/共享所有权或明确租约，保存 last-used fence。停止消费者、取消 I/O、重建 renderer 与退出程序时按依赖顺序释放，不把裸 GPU 指针跨越所有者寿命。

## 12. 报告、确定性、错误与回退

### 12.1 报告结构

```json
{
  "planVersion": "vegetation-hair/1",
  "status": "measured",
  "machine": {"adapter": "actual", "driver": "actual", "internalSize": [1920,1080]},
  "content": {"sceneDigest": "...", "speciesDigests": [], "groomDigests": []},
  "vegetation": {
    "worldInstances": 0, "residentInstances": 0, "candidateInstances": 0,
    "visibleInstances": 0, "shadowOnlyInstances": 0,
    "meshInstances": 0, "impostorInstances": 0, "aggregateNodes": 0,
    "residentBytes": 0, "peakBytesIncludingInflight": 0,
    "pageFaults": 0, "fallbackFrames": 0, "overflow": 0
  },
  "groom": {
    "instances": 0, "guides": 0, "renderCurves": 0, "renderSegments": 0,
    "samplePoolBytes": 0, "overflowSamples": 0,
    "representations": {"strands": 0, "cards": 0, "mesh": 0}
  },
  "timing": {"gpuValidSamples": 0, "gpuP95Ms": null, "gpuP99Ms": null},
  "quality": {"reference": "...", "metricsFile": "...", "sequences": []},
  "fallbacks": [], "errors": []
}
```

示例值仅表示结构，不是实测。时间戳无效/未采样必须为 null 或显式 unavailable；不能用 0 ms 表示成功。逐 pass 记录模拟、插值、剔除、深度、阴影、体素、可见性、着色、合成和公共时域时间，避免统计重叠。

### 12.2 确定性与恢复

- 散布、LOD/cook、cards 构建使用固定版本、固定 seed 和稳定排序；内容包不写墙钟时间。
- GPU 原子追加顺序不稳定时，稳定 ID 决定随机数与降采样；透明同深度样本规定稳定 tie-break。不能笼统声称所有绘制顺序与图像无关。
- 世界稀疏修改、倒伏/砍伐状态进入权威存档；纯视觉风动和 GPU LOD 不反馈玩法摘要。
- 存档续跑、视觉重启和严格同后端续跑分别定义。设备重建期间重建派生缓存，恢复有效资源后再启用。

### 12.3 建议错误码

`NG-E-VEG-FORMAT/HASH/PAGE/CAPACITY/SPECIES/SURFACE`，`NG-E-GROOM-FORMAT/BINDING/ATTRIBUTE/CAPACITY/SIMULATION`，`NG-E-GPU-SCENE-STALE`；错误码在首个格式阶段统一冻结。

包损坏拒绝发布并保留上个有效版本；缺页使用已声明低级表示；列表/样本池溢出计数并走可追溯的预算回退；禁止越界写与无报告静默丢失。

低端路径使用基础实例化、较少草密度、cards 与代理阴影。调整原因、触发帧、前后数量与图像质量进入报告；高质量目标仍未满足时明确标注。

## 13. 增量验证与工作区约定

遵守根目录 `AGENTS.md`：按依赖范围运行专项。未经用户新的明确要求，**不运行全量测试、全量回归、`npm test`、`npm run verify` 或完整 CI**。

本次只编写开发计划，检查 Markdown 内容、链接和代码事实，不执行原生构建或测试。

实现期拟新增专项入口，只有对应脚本实际落地后才可执行：

| 改动范围 | 增量验证计划 |
| --- | --- |
| vegetation 包/散布 | 仅运行对应 Node 文件，独立几何关系、统计和篡改用例 |
| Groom 包/绑定 | 对照解析曲线、重心坐标、坐标变换与半径，不镜像实现写无效测试 |
| 公共 GPU Scene/HZB | 小规模 CPU 可见参考、边界相机和 GPU 容量检查 |
| 资产导入受影响 | 使用现有 `npm run test:assets`；必要原生绘制用 `npm run assets:verify` |
| 叶片/strand 绘制 | 运行拟新增 vegetation/groom 专项机位和报告 |
| 联合预算 | 只跑选定 J-A/J-B 场景，不扩展到全工程回归 |

原生资产验证继续使用 `build/assets-host`；拟新增植被、Groom 与联合专项分别使用 `build/vegetation-host`、`build/groom-host`、`build/vegetation-hair-host`。构建依赖、输出与报告目录隔离，复用现有构建脚本公共函数，避免复制失去维护。

任何共享 renderer、shader ABI 或场景契约变更，列出受影响的资产/布料/效果增量集合。完成必要检查后不反复扩大范围；发现新失败才追加关联验证。

## 14. 风险、未完成项与最终完成条件

| 风险 | 处理与退出条件 |
| --- | --- |
| 8 GB 显存不足 | 分页与纹理/样本池总预算；先降远景资源，再降近景质量；所有退化记录 |
| 密集草地像素成本 | 分析 overdraw、覆盖和阴影；分别比较几何叶片与卡片 |
| 体素画质/复杂度超预期 | 保留几何/impostor 生产档，V6 独立验收，不以未经证明的研究阻塞基础版 |
| 长发碰撞失真 | 逐步增加肩部/手臂代理、CCD、束级排斥；记录穿透量和失败动作 |
| 浅发过暗/过亮 | 使用物理参考、消光和多次散射误差，禁止用曝光补材质错误 |
| 参考资产偏差 | 多发型、多树种、多灯光评估；单一演示资产不代表通用支持 |
| 并行工作树变化 | 每阶段重新确认 ABI/schema；不重置、覆盖或清理无关文件 |
| 第三方格式/研究可用性 | 固定版本与来源；明确受限技术的研究和产品集成边界 |

本计划不包含完整 MetaHuman 角色系统、完整 Blender 节点解释器、全身生理毛发模型、每根毛发的全量自碰撞、完整 Nanite/Lumen 克隆或无限视距全精度渲染。它们如成为后续需求，需新增可验收工作包。

最终基础版完成须同时具备：可复现资产；正确 GPU 实例化；30M 世界索引与受限工作集；森林/草原/角色联合机位；Groom 的 strands/cards 质量档；明确的动态碰撞范围；有效 P95/P99、显存和缺页报告；关闭/回退、存档与设备恢复证据。

高质量版还须具备：部件复用与聚合表示的实测收益；近景植被精细透射阴影；浅色毛发与多次散射近似通过参考对比；运动序列的闪烁/拖影和 LOD 连续性达标。完成状态逐项登记，不用一个“支持植被/毛发”布尔值代替。

## 15. 参考资料与适用范围

资料核对日期：2026-10-25。UE 文档对应当日展示的 5.8 页面，实验标签保留；Blender 以 4.5 LTS 文档作为第一版可复现制作基线。以下技术说明均为工程归纳，论文图表性能不转写为本机承诺。

- **[R1]** Epic：[Instanced Static Mesh Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine)，用于实例化与 HISM/ISM 取舍。
- **[R2]** Epic：[Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)，用于 Assemblies、Voxels、Skinning 高质量路径研究；实验功能。
- **[R3]** Epic：[Groom Scalability and Performance](https://dev.epicgames.com/documentation/en-us/unreal-engine/groom-scalability-and-performance-with-unreal-engine)，用于分阶段毛发管线、体积阴影、样本成本和回退。
- **[R4]** Epic：[Hair Rendering and Simulation](https://dev.epicgames.com/documentation/unreal-engine/hair-rendering-and-simulation-in-unreal-engine)，用于 Groom 资产与 strand 工作流。
- **[R5]** Epic：[Hair Card Generator in Dataflow](https://dev.epicgames.com/documentation/metahuman/hair-card-generator-in-dataflow-in-unreal-engine)，用于从 Groom 生成卡片、分组和多层发束的制作参考；实验工具。
- **[R6]** Blender：[Particle Introduction](https://docs.blender.org/manual/en/4.5/physics/particles/introduction.html)、[Hair Shape](https://docs.blender.org/manual/en/4.5/physics/particles/hair/shape.html)，用于旧粒子曲线及宽度适配。
- **[R7]** Blender：[Interpolate Hair Curves](https://docs.blender.org/manual/en/4.5/modeling/geometry_nodes/hair/generation/interpolate_hair_curves.html)，用于 Curves、Surface UV 与 guide 插值制作路径。
- **[R8]** EA/DICE，2019：[Strand-based Hair Rendering in Frostbite](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf)，用于可见性与 Deep Opacity 渲染组织。
- **[R9]** Marschner 等，SIGGRAPH 2003：[Light Scattering from Human Hair Fibers](https://www.cs.cornell.edu/~srm/publications/SG03-hair-abstract.html)，用于单纤维方向散射。
- **[R10]** Chiang 等，2016：[A Practical and Controllable Hair and Fur Model for Production Path Tracing](https://www.disneyanimation.com/publications/a-practical-and-controllable-hair-and-fur-model-for-production-path-tracing/)，用于离线参考与散射参数校准。
- **[R11]** Macklin、Müller、Chentanez，2016：[XPBD: Position-Based Simulation of Compliant Constrained Dynamics](https://github.com/matthias-research/pages/blob/master/publications/XPBD.pdf)，用于 guide 约束求解基础。
- **[R12]** Huang 等，2024：[Real-time Level-of-Detail Strand-based Hair Rendering](https://arxiv.org/abs/2405.10565)，用于外观保持 LOD 研究。
- **[R13]** Bhokare 等，SIGGRAPH 2024：[Real-Time Hair Rendering with Hair Meshes](https://www.cemyuksel.com/research/hairmesh_rendering/)，用于紧凑程序化发型和多角色研究；页面声明专利与使用条件，未选为默认产品依赖。

仓库关联文档：[外部资产](EXTERNAL-ASSETS.md)、[几何布局](GEOMETRY-LAYOUT.md)、[虚拟几何/GI 计划](NANITE-LUMEN-PLAN.md)、[粒子/共享场/布料/水](PARTICLES-FIELDS-CLOTH-WATER-PLAN.md)、[共享风](FIELD-REFERENCE.md)、[GPU 布料](CLOTH-GPU.md)、[动画与软组织](ANIMATION-M1-M4.md)、[性能预算](BUDGET.md)。
