# 材质管线方案与创新机会

> 副标题：从 Blender↔UE 材质桥接（“ueblender”）到 AI 原生引擎的材质编译器

- 版本：v0.1（调研稿）
- 日期：2026-10-20
- 基线：NativeGPU 0.10.0
- 关系：本文是 [TEXTURE-MATERIAL-PLAN.md](TEXTURE-MATERIAL-PLAN.md) 的上游调研，回答“方案空间有多大、别人怎么选、我们能在哪里做出差异”；不替代其中 TX0–TX5 的工作包与验收门禁。

---

## 0. 结论先行

1. **“ueblender”不是一个可检索到的论文或主流项目名。** 公开检索只找到 `aarmbruster/UEBlenderLighting`（一个 UE4 光照烘焙插件），没有同名材质管线论文。它更可能指 **Blender↔Unreal 的材质桥接工具族**，或某个团队内部代号。本文按前者分析；如果你指的是具体产品，第 1 节可以替换，第 2–8 节不受影响。
2. **这类桥接工具的本质是“资产搬运”，不是“材质编译”。** 它们把 Blender 的 Principled BSDF 通道映射到 UE 的材质参数，程序化节点、节点组合、UV 变换和参数动画通常在无声中丢失。根本原因是两边没有共同的语义 IR，也没有能力协商和损失报告。
3. **引擎材质管线没有单一最优解，只有五种原型加四种正交能力。** 五种原型是：固定参数 PBR、Uber-shader、节点图→代码生成、DSL/标准 IR→多后端、数据驱动 bindless + 统一求值。四种正交能力是：分层栈、闭包树、虚拟纹理、神经编码。选型取决于“表达力 / 编译成本 / 运行时成本 / 确定性”四角。
4. **本引擎的机会不是复刻 UE 的节点图，而是把材质做成“可编译、可计价、可回放、可被 AI 安全编辑”的 IR。** UE 的强项是生态和工具，弱项是变体爆炸与编译时间；小引擎的强项恰恰是能对每一份材质给出确定性的成本与证据。
5. **最值得下注的五个创新**：类型化材质 IR（MIR）、预算/质量求解器（含回放驱动的纹理分配）、表面经历算子（磨损/氧化/受潮的耦合语义）、跨四条渲染路径的统一求值 ABI、带收据的 AI 编辑闭环。**两个高风险高回报的研究赌注**：可编辑逆渲染拟合、把神经编码当作 cook 候选。

---

## 1. 先厘清 “ueblender”：它是一族工具，不是一篇论文

### 1.1 检索结论（事实）

- GitHub 仓库搜索 `ueblender` 只返回 `aarmbruster/UEBlenderLighting`，描述是 “A light baking plugin for Unreal Engine 4 using Blender”，1 个 star，与材质无关。
- 公开搜索没有以 “UEBlender” 为标题的材质/渲染论文。
- 搜索结果里真正相关的是这些工具与工作流：Epic 官方的 [BlenderTools（Send to Unreal）](https://github.com/EpicGames/BlenderTools)、商业的 UEBridge、UE 官方的 [Datasmith](https://www.unrealengine.com/en-US/datasmith)、glTF/FBX 往返，以及社区里的各类 “Blender material to Unreal” 转换插件。

因此本文把 “ueblender” 定义为：**把 Blender 的材质/纹理/节点树搬到 Unreal（或反过来）的工具与流程的总称**。这个定义下，它的技术问题、失败模式和改进方向都很清楚，而且和本引擎要做的材质管线是同一类问题。

### 1.2 这一族工具的典型管线

```text
Blender 节点树
   ↓  识别 Principled BSDF，提取少数通道
baseColor / metallic / roughness / normal / emission / alpha
   ↓  导出贴图，或把程序化节点烘焙成贴图
PNG/TGA/EXR + 一组标量
   ↓  在 UE 端重建 Material 或 Material Instance
手工修：接法线、改混合模式、补 UV、调参数
```

关键点：中间**没有语义 IR**，只有一张“通道映射表”。映射表之外的信息——节点类型、连接关系、参数范围、程序化逻辑、UV 变换——没有地方存放，也就无法被检查、比较或回滚。

### 1.3 七个反复出现的失真

| 失真 | 典型例子 | 后果 |
| --- | --- | --- |
| 程序化节点丢失 | Noise、Voronoi、Wave、Musgrave 直接不被导出 | 失去参数化，只剩一张固定贴图 |
| 节点组合被截断 | Mix、ColorRamp、Bump、Layer Weight 只取一个输入 | 外观“差不多”，但无法解释差在哪 |
| 颜色空间错位 | sRGB 当线性、法线贴图被当颜色、ORM 通道顺序不一致 | 发灰、发黑、法线方向反 |
| UV 语义丢失 | tiling/offset、第二套 UV、UDIM、镜像 | 平铺错、接缝、细节错位 |
| 混合模式错配 | Blend Mode、Two Sided、Alpha Clip、Dithered | 透明物体排序错或直接不透明 |
| 参数动画/驱动丢失 | Blender driver、关键帧、几何属性驱动 | 动态效果消失 |
| 预算不可见 | Blender 节点可无限嵌套，UE 有指令数和采样数限制 | 转换后编译失败或超预算，且没有提前警告 |

### 1.4 根因不是“写得不够好”，而是四个结构性缺口

1. **IR 不同构**：Blender 是通用 shader DAG（Cycles/EEVEE 求值），UE 是受限材质图再翻译成 HLSL。两者没有共同的类型系统和语义。
2. **没有能力协商**：目标端不支持某个节点时，工具通常静默降级，而不是给出“保留 / 近似 / 丢弃”的三态结果。
3. **没有损失度量**：没有报告说明哪些节点被保留、哪些被烘焙、哪些被丢弃，也没有 round-trip 对比。
4. **没有预算模型**：转换结果是否可编译、采样数多少、寄存器压力多大，在导入前不可知。

### 1.5 正确的桥接形态：把“转换”做成“编译”

如果目标是“材质语义尽可能等价地跨引擎”，桥接工具应该具备编译器的形状：

```text
DCC 前端（Blender / Max / Maya）
        ↓  解析节点树，绑定到 canonical profile
Canonical 材质 IR（MaterialX 子集 + OpenPBR 闭包 + 显式扩展）
        ↓  能力协商：目标端支持矩阵
三态输出：preserved / approximated（附烘焙产物）/ dropped（附原因）
        ↓
目标后端（UE 材质图 / 本引擎 MIR / glTF / 离线渲染器）
        ↓
损失报告 + round-trip diff + 固定光照渲染对比
```

相关标准与论文线索：

- [MaterialX](https://materialx.org/)：节点图 + 标准表面 + GLSL/OSL/MDL 代码生成，是目前最接近“材质交换 IR”的开放标准。
- [OpenPBR](https://academysoftwarefoundation.github.io/OpenPBR/)：ASWF 的统一表面模型，把 base、coat、fuzz、transmission、thin-film 等分层语义标准化，适合作为闭包层。
- [MDL](https://github.com/NVIDIA/MDL-SDK)：NVIDIA 的声明式材质语言，强在离线渲染与多后端。
- glTF 的 metallic-roughness：运行时最低公分母，扩展有限。
- SPIR-V / DXC / SPIRV-Cross：着色器级可移植性，但**不解决材质语义**——它只能搬运已经写好的 shader。

**判断：** 对“ueblender”这类工具，正确的期待不是“无损转换”，而是“带损失报告的可编译转换”。这与本引擎第 7 节的 MIR 是同一件事的两端：进口和出口都需要一个类型化的中间层。

---

## 2. 引擎材质管线的方案空间

### 2.0 总图

```text
作者层：参数面板 / 节点图 / 分层栈 / 代码 / 程序化 / 生成式
   ↓
表示层：编辑器图 / AST / 字节码 / 闭包树 / 标准 IR
   ↓
分析层：类型与语义校验、依赖与 UV 需求、成本估算、变体枚举
   ↓
计划层：质量档、纹理候选、材质 LOD、变体集合（受预算约束）
   ↓
生成层：HLSL/GLSL、DXIL/SPIR-V、PSO、描述符布局
   ↓
绑定层：逐 draw 常量 / 有界表 / bindless / GPU 场景缓冲
   ↓
运行时：forward / deferred / visibility buffer + resolve / RT
   ↓
证据层：缓存键、收据、像素级诊断、回放
```

### 2.1 五种原型

| 原型 | 代表 | 表达力 | 编译成本 | 运行时成本 | 确定性 | 适合阶段 |
| --- | --- | --- | --- | --- | --- | --- |
| A. 固定参数 PBR | glTF 查看器、早期引擎 | 低 | 零 | 低 | 高 | 原型、移动端、最低公分母 |
| B. Uber-shader + 参数 | 经典 forward 引擎 | 中 | 低（一个 shader） | 中高（分支/寄存器） | 高 | 材质种类有限、硬件固定 |
| C. 节点图 → 代码生成 + 变体 | UE5、Unity | 高 | 高（变体爆炸） | 低（每变体特化） | 中 | 大型团队、需要美术自助 |
| D. DSL / 标准 IR → 多后端 | Filament、MaterialX、MDL | 中高 | 中 | 低到中 | 高 | 小引擎、跨工具、离线/实时共用 |
| E. 数据驱动 bindless + 统一求值 | GPU-driven 引擎、visibility buffer | 高 | 中 | 中（divergence） | 中高 | 大世界、材质多样、GPU 主导 |

正交能力（可以叠加在任一原型上）：

- **分层栈**：base + coat + damage + moisture，天然适配磨损/污渍，运行时成本随层数线性增长。
- **闭包树**：把表面写成 diffuse/GGX/clearcoat/sheen/transmission 的组合，物理可组合，但 G-buffer 编码和求值成本高。
- **虚拟纹理 / 稀疏驻留**：解决“纹理比显存大”，代价是页表、流式和确定性。
- **神经编码**：NeuMIP/NTC 一类，把材质纹理压进小网络，代价是解码算力和硬件依赖。

### 2.2 八个决策维度

| 维度 | 选项 | 主要代价 | 本引擎倾向（判断） |
| --- | --- | --- | --- |
| 作者模型 | 固定参数 / 节点图 / 分层栈 / 闭包树 / 代码 / 程序化 / 生成式 | 表达力越强，工具与编译越重 | 分层栈 + 小节点集，节点图后置 |
| 中间表示 | 编辑器图 / AST / 字节码 / 闭包树 / MaterialX / OSL / MDL | 标准 IR 利于交换，不利于引擎内成本分析 | 自有 MIR，可导出 MaterialX 子集 |
| 特化策略 | Uber-shader / 变体集 / 特化常量 / 运行时数据分支 / megakernel | 变体爆炸 vs 运行时分支 | 少量变体 + 严格变体闭包 |
| 绑定模型 | 逐 draw CB / 有界表 / bindless / 堆索引 / GPU 场景缓冲 | 灵活性与 root signature 预算 | MaterialRecord + bindless，先有界表兼容 |
| 求值架构 | forward / deferred / visibility buffer / deferred texturing / RT | 带宽、MSAA、材质多样性 | 先 forward + G-buffer 过渡，visibility buffer 后置 |
| 纹理策略 | 通道打包 / mip / 压缩 / 虚拟纹理 / 稀疏驻留 / 神经编码 | 内存、带宽、确定性 | BC + mip + 候选 + 预算；VT 后置 |
| 缓存与发布 | shader cache / PSO precache / 内容寻址 / 字节级复现 | 构建时间与磁盘 | 内容寻址 + 收据，复用现有事务层 |
| 交换与工具 | glTF / MaterialX / USD / MDL / DCC 桥 / 调试视图 / AI | 生态兼容与维护成本 | 先 glTF 进、MaterialX 出，桥接带损失报告 |

### 2.3 为什么“没有免费午餐”

- **表达力 ↑ ⇒ 变体数 ↑ ⇒ 编译时间 ↑ ⇒ 迭代速度 ↓。** UE 的 shader 编译卡顿是这条链的直接后果。
- **运行时灵活 ↑ ⇒ 分支/寄存器压力 ↑ ⇒ 占用率 ↓。** 数据驱动材质在 GPU-driven 管线里很自然，但在 forward 里会让每个像素都付分支成本。
- **物理正确 ↑ ⇒ G-buffer 加宽 ⇒ 带宽 ↑。** Substrate 一类闭包模型需要更宽的存储或更贵的 resolve。
- **确定性 ↑ ⇒ 动态特化 ↓。** 一旦允许运行时任意分支/异步编译，字节级复现就很难。

小引擎的正确策略不是“全都想要”，而是**在每一维上明确选择一个点，并让这个选择可被证据验证**。

### 2.4 三种典型组合

单看维度容易“什么都想要”；实际项目通常会落到三种组合之一：

| 组合 | 作者模型 | 编译/变体 | 绑定与求值 | 代表 | 代价 |
| --- | --- | --- | --- | --- | --- |
| 轻量确定性 | 固定参数 + 少量分层 | 无变体或极少变体 | 有界表 + forward | 原型、移动端、教学引擎 | 表达力有限，复杂表面要靠烘焙 |
| 工具友好 | 节点图 + 材质实例 | 变体集 + PSO 预缓存 | bindless + deferred | UE5、Unity | 编译时间、变体治理、工具成本 |
| 数据驱动 / AI 原生 | 分层栈 + 小节点集（MIR） | 变体闭包证明 + 预算求解 | MaterialRecord + 统一求值 | 本文建议 | 需要自建 IR、成本模型与证据链 |

这三条不是互斥的“门派”，而是**演进路径**：轻量组合可以先跑通像素，工具组合可以后补节点图，数据驱动组合则从第一天就把“成本、变体、证据”放进架构。对本引擎，建议直接按第三条设计数据模型，但用第一条的范围实现第一版。

---

## 3. 工业界与标准盘点

### 3.1 Unreal Engine 5

- **作者模型**：Material Editor 节点图，支持 Material Function、Material Layer、Material Instance（参数集）、静态开关。
- **编译**：Material Translator 把图翻译成 HLSL，再由 DXC 编译为 DXIL；静态开关产生变体；PSO 需要预缓存。
- **运行时**：deferred + forward 混合；G-buffer 布局与着色模型绑定；Virtual Texture 用于大世界。
- **Substrate**：把材质表达为 closure/slab 的组合，允许更物理的分层（清漆、薄片、次表面等），代价是更重的求值和迁移成本。
- **优点**：生态、工具链、平台覆盖、美术熟悉度。
- **缺点**：变体爆炸、编译时间、图与 HLSL 强耦合、Substrate 迁移不是免费的。

参考：[Substrate Materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/substrate-materials-in-unreal-engine)

### 3.2 Unity

- **作者模型**：Shader Graph（节点图）或手写 HLSL；Material Variants 支持继承与覆盖。
- **编译**：Shader Graph 生成 HLSL；keyword 组合产生变体；SRP Batcher 降低 set pass 开销；构建时做 shader stripping。
- **运行时**：URP/HDRP 两套管线；GPU Resident Drawer 降低 CPU 提交。
- **优点**：上手快、跨平台；**缺点**：变体管理仍是痛点，URP/HDRP 的能力分裂增加材质迁移成本。

### 3.3 Godot 4

- **作者模型**：类 GLSL 的 `shader_type spatial` 手写着色器为主，VisualShader 生成代码；材质资源 + uniform。
- **编译**：运行时/加载时编译，缺少 UE 那种集中式变体管理。
- **优点**：轻、透明、易改；**缺点**：大型项目的材质治理（依赖、预算、诊断）较弱。

### 3.4 Filament

- **作者模型**：`.mat` DSL，分 `material` / `vertex` / `fragment` 块，声明参数、变体和特化常量。
- **编译**：`matc` 编译为 `.filamat` 材质包；variant + specConstant 在编译期特化。
- **优点**：小而完整、确定性较好、适合作为“DSL + 变体 + 特化常量”的参考实现。
- **缺点**：节点图/美术工具链弱，扩展靠写 DSL。

参考：[Filament Materials Guide](https://google.github.io/filament/Materials.html)

### 3.5 标准与语言

| 标准 | 定位 | 强项 | 局限 |
| --- | --- | --- | --- |
| [MaterialX](https://materialx.org/) | 节点图与标准表面交换 | DCC 支持、GLSL/OSL/MDL 代码生成、Hydra | 运行时成本/变体/预算不是它的目标 |
| [OpenPBR](https://academysoftwarefoundation.github.io/OpenPBR/) | 统一表面模型 | 分层语义标准化、离线/实时共用 | 实时实现仍需简化，G-buffer 成本高 |
| [MDL](https://github.com/NVIDIA/MDL-SDK) | 声明式材质语言 | 离线渲染、多后端、强类型 | 实时引擎集成复杂，生态偏 NVIDIA |
| glTF metallic-roughness | 运行时交换 | 简单、广泛 | 表达力有限，扩展碎片化 |
| USD / UsdShade | 场景与着色网络 | 大管线协作、Hydra | 偏离线，实时引擎需要裁剪 |

### 3.6 Blender 与内容工具

- **Blender**：Principled BSDF 是“作者友好的统一入口”，但节点图是通用的，程序化节点不会被 glTF 导出。它是很好的**前端**，不是好的**运行时 IR**。
- **Substance Designer/Painter**：程序化图离线烘焙成贴图，运行时便宜；代价是参数化在烘焙后丢失。
- **Datasmith / Send to Unreal / UEBridge**：解决“搬过去”，不解决“语义等价”；见第 1 节。

### 3.7 横向对比

| 方案 | 表达力 | 编译/变体 | 运行时 | 确定性 | 生态 | 适合 |
| --- | --- | --- | --- | --- | --- | --- |
| UE5 节点图 + Substrate | 很高 | 变体多、编译重 | deferred，重 | 中 | 很强 | 3A、大团队 |
| Unity Shader Graph | 高 | keyword 变体多 | URP/HDRP | 中 | 强 | 中小团队、跨平台 |
| Godot 手写 shader | 中高 | 运行时编译 | forward/clustered | 中高 | 中 | 独立项目、教学 |
| Filament DSL | 中高 | 变体 + 特化常量 | forward/clustered | 高 | 中 | 移动/嵌入式、参考实现 |
| MaterialX + 离线 | 高 | 离线编译 | 离线/烘焙 | 高 | 强（DCC） | 影视、资产交换 |
| 本引擎（建议） | 中高（分层 + 小节点集） | 少量变体 + 闭包证明 | forward → visibility buffer | 高 | 自建 | AI 原生、确定性优先 |

---

## 4. 论文地图：四个簇

### 4.1 表示与编译

| 工作 | 年份 | 贡献 | 能落到哪一层 | 局限 |
| --- | --- | --- | --- | --- |
| MaterialX | 2017 起 | 开放节点图/标准表面规范 + 多后端代码生成 | 交换层、导出层 | 不管运行时预算与变体 |
| OpenPBR | 2024 起（1.0） | 统一分层表面模型 | 闭包层、外观模型 | 实时简化需自己做 |
| MDL | 持续 | 声明式材质语言与编译后端 | 离线/多后端 | 实时集成重 |
| SPIR-V / DXC / SPIRV-Cross | 持续 | 着色器级可移植 | 生成层 | 不解决材质语义 |

**可用的结论**：标准 IR 解决“交换”，不解决“引擎内的成本、变体和确定性”。本引擎应该**借用它们的类型系统和闭包语义，但保留自己的 MIR 作为运行时真相**。

### 4.2 外观模型

- **分层微表面**：Weidlich & Wilkie 的任意层微表面模型（2007）给出了层间多次散射的近似，是“清漆/磨损/氧化”一类效果的理论基础。
- **Substrate / OpenPBR**：工业界把“闭包 + 算子”工程化，证明分层表面可以进入实时管线，但代价是求值与存储。
- **判断**：本引擎不需要完整的分层微表面；需要的是**少量闭包 + 明确耦合规则的算子**，并把“物理正确”限制在能解释的范围内。

### 4.3 神经表示与压缩

| 工作 | 年份 | 贡献 | 能落到哪一层 | 风险 |
| --- | --- | --- | --- | --- |
| [NeuMIP](https://dl.acm.org/doi/10.1145/3450626.3459795) | 2021 | 多分辨率神经材质，缓解 minification 的闪烁与过模糊 | 纹理层、材质 LOD | 训练/解码成本、平台依赖 |
| [NTC（Random-Access Neural Compression of Material Textures）](https://dl.acm.org/doi/10.1145/3592407) | 2023 | 随机访问的神经材质压缩，显著降低材质纹理占用 | 纹理 cook 候选 | 张量核心依赖、确定性、工具链 |

**判断**：神经编码适合作为 **cook 的一个候选**，由预算求解器在“质量/显存/解码算力”之间选择；不应成为默认路径，也不应在运行时依赖模型服务。

### 4.4 逆向与生成

| 工作 | 年份 | 贡献 | 能落到哪一层 | 局限 |
| --- | --- | --- | --- | --- |
| [MaterialGAN](https://arxiv.org/abs/2010.00114) | 2020 | 用 GAN 先验从照片捕获 SVBRDF，缓解高光/光照歧义 | 逆渲染、材质拟合 | 输出是贴图，不是可编辑图 |
| [Material Anything](https://arxiv.org/abs/2411.15138) | 2025 | 扩散模型为任意 3D 对象生成 PBR 材质 | 生成层、贴图候选 | 一致性/可编辑性/预算仍需后处理 |
| 可微渲染（[Mitsuba 3](https://mitsuba-renderer.org)、[nvdiffrast](https://nvlabs.github.io/nvdiffrast/)） | 持续 | 把渲染变成可优化目标 | 参数拟合、验证 | 病态问题、需要先验与约束 |

行业信号：像 [ShaderGPT](https://www.slanted.de/shadergpt/) 这样的“文本→着色器”产品已经出现，说明“生成”不是难点，**“生成结果可验证、可编辑、可预算”才是难点**。

### 4.5 还没被论文解决的空白

1. **图级生成**：现有生成多在图像/参数层，直接生成“节点图/闭包树”的工作很少；而这正是 LLM + 类型系统能发挥的地方。
2. **成本模型**：没有公开的、可校准的“材质图 → ALU/采样/寄存器/带宽”成本模型；引擎只能靠实测。
3. **跨引擎语义等价**：MaterialX 解决语法交换，但没有“保真度合同”（哪些语义保证保留、哪些允许近似）。
4. **确定性编译**：着色器编译的字节级复现、变体闭包证明，工程实践多、论文少。
5. **材质 LOD**：从图自动推导简化版本并保证视觉误差，缺少系统化方法。

这些空白就是第 6 节创新点的来源。

---

## 5. 本引擎的现状、约束与差距

### 5.1 现状（事实）

| 项 | 当前 | 影响 |
| --- | --- | --- |
| 材质表示 | 场景 JSON 内联 `albedo / roughness / metalness / pattern` | 没有资产引用、没有实例参数、没有节点 |
| 纹理 | 64×64、单层 mip 的纹理数组，8 个槽 | 无法导入真实图片，无法表达多贴图材质 |
| 绑定 | 有界 8 槽材质表 + 逐 draw 常量 | 材质数量受限；root signature 已用 61/64 DWORD |
| 求值 | 光栅 PS、Mesh PS、RT 各自一份光照/采样代码 | 新能力容易只落在一条路径上 |
| 几何 | 静态 glTF 只有 position/normal；蒙皮有 UV 无 tangent | 法线贴图还缺 UV/切线/接缝处理 |
| 光照 | 近似模型（ambient + 点光 + 简化高光） | 接 PBR 贴图不等于完成 PBR 光照 |
| 证据 | 内容寻址构建、场景事务、收据、回放、图像回归 | 可复用为材质依赖闭包与验收 |

### 5.2 硬约束

- **Root signature 61/64 DWORD**：扩展材质绑定前必须把大块逐 draw 常量移入 GPU 缓冲，根参数只留索引和少量绑定。
- **8 槽有界表**：新材质库不能假设“槽位够用”；要么扩表，要么走 bindless/堆索引（B3b 路径）。
- **三条几何路径 + RT**：材质求值必须共用，否则会出现“光栅对、Mesh 不对、RT 又不一样”的分裂。
- **确定性文化**：任何“运行时随机/异步/模型调用”都要能被收据和回放解释。

### 5.3 与 TX0–TX5 的对应

| 本文概念 | 对应工作包 | 说明 |
| --- | --- | --- |
| MIR v0 | TX0 + TX2 | 先落类型与资产引用，再落多贴图与共用求值 |
| 纹理 cook | TX1 | PNG → 语义校验 → mip → DDS → 采样 |
| MaterialRecord + bindless | TX1/TX2 | 先有界表兼容，B3b 后切堆索引 |
| 表面经历算子 | TX4 | wetness/wear 先做，氧化后置 |
| 预算求解器 | TX2/TX4 | 质量候选 + 预算冲突报告 |
| 收据/回放 | TX3/TX5 | 依赖闭包、热重载、失败保留旧版本 |
| 体检台 | TX2 | 调试视图与像素溯源 |

---

## 6. 创新点清单

> 排序按“对本引擎的性价比”，不是按学术新颖度。每项都给验收方式，避免“听起来很好”。

### I1. 类型化材质 IR（MIR）作为唯一真相

**主张**：引擎里不存在“编辑器图”和“生成的 HLSL”两套真相，只有一份带类型、带成本注解、可规范化哈希的 MIR；HLSL 只是它的一个后端。

**机制**：
- 节点集小而有版本：常量、纹理采样、重映射、混合、法线混合、闭包构造、层算子。
- 每个端口有语义类型：`color-linear`、`color-srgb`、`scalar`、`normal-ts`、`mask`、`uv`，以及 UV 集、切线需求、单位。
- 每个节点声明成本（ALU/采样/寄存器）和确定性等级。
- 规范化：常量折叠、公共子表达式消除、死代码消除、代数化简，得到稳定 hash。
- AI 编辑只允许对 MIR 做类型化 patch，不允许直接写 HLSL。

**为什么值得做**：这是后面所有能力（成本、预算、变体、跨后端、AI 安全编辑、可回放）的地基。没有它，每加一个功能都要在编辑器、编译器、运行时三处各写一遍。

**验收**：同一 MIR 两次编译得到字节一致的 DXIL；MIR 的规范化 hash 与源文件路径/时间无关；删掉一个未连接节点不改变产物 hash。

**风险**：节点集设计过小会限制美术，过大又回到变体爆炸。建议先覆盖 80% 场景，缺口用“烘焙贴图”逃生门。

---

### I2. 预算/质量求解器：把“材质开销”变成可计算的计划

**主张**：编译不是“把图翻译成 shader”，而是“在预算约束下选一组产物”：纹理分辨率/格式、材质 LOD、闭包数、变体集合。

**机制**：
- 每个材质给出候选（512/1K/2K、BC7/BC5/未压缩、闭包 1/2/3）。
- 估计每个候选的质量误差（离线渲染对比）和成本（显存、采样、ALU、带宽）。
- 全局约束：纹理预算（MiB）、帧时间预算、每对象重要性。
- 求解：先贪心，后 Pareto 前沿；记录每一步和冲突，不宣称全局最优。
- 重要性来源可以不只是“手动标记”，而是**回放里的屏幕覆盖率与停留时间**（本引擎已有回放基建，这是差异化点）。

**为什么值得做**：UE/Unity 的预算主要靠项目经验和事后优化；把预算放进编译器，能让“这个场景能不能跑”在构建时就回答。

**验收**：给定 96 MiB 预算与 12 个道具，求解器输出一份可解释的计划；改一个重要性标记只影响相关材质的候选；冲突时列出“降谁、降到多少、损失多少”。

**风险**：成本模型不准会让计划失真。先用手工校准的小模型，逐版本用实测回归。

---

### I3. 表面经历算子：把“磨损/氧化/受潮”做成有耦合规则的层

**主张**：材质不是一组独立滑块，而是“表面经历过什么”的算子序列；算子之间有权重和依赖，不能产生物理上不可能的组合。

**机制**：
- 层栈：基底 → 涂层 → 磨损 → 氧化 → 受潮。
- 每个算子有：覆盖率遮罩、强度、seed、作用域、耦合规则。
- 耦合示例：磨损去掉涂层后暴露基底；氧化只作用于暴露的金属；受潮降低粗糙度、压暗 albedo、改变 IOR。
- 离线烘焙遮罩与派生贴图；运行时只保留少量标量和 2–4 张遮罩。
- seed 由实体 UUID + 配方版本导出，保证可复现的个体差异。

**为什么值得做**：这是美术能直接感知的差异，也是“AI 编辑”最容易验证的语义层（“再旧 20%”比“把 roughness 改成 0.42”更自然）。

**验收**：12 件道具共享基础贴图；全局湿度变化时所有指定区域连贯变化；撤销恢复材质引用与参数；每件道具给出显存与构建耗时。

**风险**：算子多了会变成另一种节点图。限制在 5 个算子、每个算子固定语义，避免无限扩张。

---

### I4. 闭包预算：Substrate 的思想，按“每像素瓣数”裁剪

**主张**：采用少量闭包（diffuse、GGX 电介质、GGX 金属、clearcoat、sheen、transmission、emission）加层算子，但给每个材质一个**闭包预算**（例如 ≤2 或 ≤3），编译期自动简化。

**机制**：
- 材质声明需要的闭包；编译器计算闭包数，超预算时按规则合并/丢弃（远处丢 sheen、把 coat 并入基础粗糙度）。
- G-buffer 存紧凑的闭包头 + 参数；resolve 时按头求值。
- 透明/折射走 forward 分支，避免 G-buffer 无法表达。
- 简化决策进入收据，图像差异有门禁。

**为什么值得做**：完整 Substrate 对小引擎太重；但“闭包 + 预算”能在表达力和成本之间给出可解释的中间点。

**验收**：同一材质在 1/2/3 闭包三档下的图像差异在阈值内；超预算时有明确诊断；光栅与 RT 的闭包求值一致。

**风险**：G-buffer 布局一旦定下就很难改；先做 2 闭包的窄版本。

---

### I5. 跨路径统一求值 ABI

**主张**：光栅、Mesh Shader、蒙皮、RT 四条路径调用同一个 `EvaluateMaterial(record, surface, uv)`，并用逐像素对比证明一致。

**机制**：
- `MaterialRecord` 放 StructuredBuffer：flags、闭包数、纹理索引（bindless）、标量参数、颜色。
- 四条路径只负责准备 `SurfaceInput`（世界位置、法线、切线、UV、屏幕导数），求值逻辑共用。
- 差异测试：同一场景分别走四条路径，比较输出（允许浮点误差，不允许语义差异）。
- 调试构建里记录节点 ID，支持“这个像素来自哪个节点”。

**为什么值得做**：本引擎已经有“光栅 vs Mesh 逐像素比较”的传统，把它扩展到材质是自然且少见的工程纪律。

**验收**：四条路径的材质输出在固定场景下逐像素一致（或差异有解释）；新增一个节点只改一处求值代码。

**风险**：RT 路径的递归/采样限制可能迫使某些节点降级；需要显式的能力矩阵。

---

### I6. 变体闭包证明：CI 保证“运行时不会缺变体”

**主张**：从场景的材质集合反推需要的变体集合，构建期全部编译；CI 断言运行时不会命中未编译的变体。

**机制**：
- 从 MIR 静态分析出 feature bits（是否用 normal、clearcoat、透明、UV 集等）。
- 变体集合 = 场景中实际出现的组合，而不是全幂集。
- 构建期编译 + PSO 预缓存；运行期命中未编译变体时，走 fallback 并记录诊断（CI 视为失败）。
- 缓存键 = MIR hash + feature bits + 后端 + 编译器版本。

**为什么值得做**：大多数引擎靠“预缓存 + 祈祷”；把“闭包完整性”做成门禁，能把 PSO 卡顿变成构建期问题。

**验收**：新增一个使用 clearcoat 的材质，CI 自动多编译对应变体；删除后变体集合收缩；运行日志中 `missing variant` 恒为 0。

**风险**：场景动态生成材质时闭包不可静态确定；需要“运行时编译 + 降级”逃生门，并记录。

---

### I7. 像素溯源体检台

**主张**：点一个像素，能看到它来自哪个材质、哪个节点、哪张贴图、哪个 mip、哪个参数，以及“为什么是这个颜色”。

**机制**：
- 调试视图：最终着色、baseColor、世界法线、粗糙度、金属度、AO、mip、UV 密度、切线。
- 调试构建在求值时写入节点 ID / 纹理 ID（或走单独的 debug pass）。
- 常见错误启发式：sRGB 双重转换、法线 Y 翻转、金属度接粗糙度、缺 UV、采样器不匹配、mip 过模糊。
- 与收据联动：定位到构建步骤与源图。

**为什么值得做**：材质问题 80% 是“语义接错”，不是“算法不对”；把诊断做成产品能力，比多一个节点更有价值。

**验收**：故意导入一张按颜色空间处理的法线图，体检台能定位到语义声明；修正后给出前后图与构建差异。

**风险**：调试信息的存储与性能开销；只在 debug/QA 构建开启。

---

### I8. 收据与反事实 diff

**主张**：每次材质构建都有一份收据；两份收据可以 diff 出“哪个输入/参数/版本导致画面变化”，并支持二分定位。

**机制**：
- 收据：输入 hash、处理器版本、参数、seed、依赖、产物 hash、显存、耗时、验证结果。
- diff：结构化比较两份收据，标出 changed/added/removed，映射到受影响的材质实例与场景对象。
- 反事实：保留旧产物，支持“如果当时用另一张图会怎样”的离线重放。

**为什么值得做**：这是本引擎“证据文化”的自然延伸，也是 AI 训练数据（patch → 结果）的来源。

**验收**：改一张源图，diff 只列出真正受影响的产物；二分定位到具体构建步骤；旧版本可回放。

**风险**：收据字段膨胀；需要 schema 版本化与裁剪策略。

---

### I9. 类型化 AI 编辑闭环

**主张**：自然语言不直接生成 shader，而是生成对 MIR 的类型化 `AssetPatch`；引擎校验、预览、比较、再提交。

**机制**：
- LLM 通过工具调用读 MIR、查节点库、提出 patch（加节点、改参数、换贴图、加层算子）。
- 引擎做类型/范围/预算校验，生成预览，渲染对比（与参考图或上一版）。
- 失败则回退并给出结构化错误；成功才提交事务，写收据。
- 检索增强：从项目里找相似材质/子图，减少“凭空造节点”。

**为什么值得做**：这是“AI 原生引擎”和“给引擎加个聊天框”的分水岭——前者要求 AI 的修改可验证、可撤销、可计价。

**验收**：“把铜门做得更旧、但保留绿色漆”能落到一组可解释的 patch；预览与提交一致；撤销恢复；预算不超。

**风险**：LLM 的“审美”不可靠；必须有人审或强验证，不能自动提交高风险修改。

---

### I10. 可编辑逆渲染拟合（研究赌注）

**主张**：给一张参考照片和固定光照，拟合出**可编辑的 MIR 参数**（而不是一张烘焙贴图），并给出置信度与残差热图。

**机制**：
- 低维参数化（baseColor、roughness、金属度、层算子强度、遮罩范围）。
- 可微渲染（Mitsuba 3 / nvdiffrast）或导数无关优化（CMA-ES）。
- 先验：层算子语义、材质库、颜色空间约束。
- 输出 patch + 残差 + “哪些参数不可信”的报告。

**为什么值得做**：MaterialGAN/Material Anything 输出的是贴图/参数，不是可编辑图；把逆渲染接到 MIR 上是明确的空白。

**验收**：在合成数据上恢复已知参数（误差有界）；在真实照片上给出可编辑结果与残差；拟合结果能进入预算求解器。

**风险**：病态、慢、对光照假设敏感；先做离线工具，不做运行时。

---

### I11. 神经编码作为 cook 候选（研究赌注）

**主张**：把 NTC/NeuMIP 一类神经表示放进纹理候选集，由求解器按“质量/显存/解码算力”选择，而不是默认开启。

**机制**：
- 候选：BC7/BC5、未压缩、神经编码。
- 质量：固定视角渲染对比；成本：显存 + 解码 ALU/张量核心占用。
- 仅在目标 GPU 支持且收益为正时选择；否则回退 BC。
- 解码器版本进入收据，保证可复现。

**为什么值得做**：如果神经编码真能省 4–8 倍显存，它值得成为“预算求解器的一个选项”，而不是一个孤立的研究 demo。

**风险**：硬件依赖、解码确定性、工具链不成熟；短期只做离线实验。

---

### I12. 跨引擎桥的“损失报告”（把 ueblender 做对）

**主张**：导入/导出不是“尽量像”，而是给出三态结果和可测量的保真度合同。

**机制**：
- Canonical profile：MaterialX 子集 + OpenPBR 闭包 + 显式扩展。
- 每个 DCC 一张映射表；不支持时输出 `approximated`（附烘焙贴图）或 `dropped`（附原因）。
- Round-trip：导出再导入，比较 MIR 结构与固定光照渲染。
- 报告进入收据：preserved/approximated/dropped 计数、误差、预算影响。

**为什么值得做**：这正是第 1 节那类工具最缺的东西；对本引擎，它同时是“入口”（接 Blender/Substance）和“出口”（给 DCC 预览）。

**验收**：一个含 Noise + Mix + Bump 的 Blender 材质，导入后报告“Noise 被烘焙为 1K、Mix 被近似、Bump 保留”；round-trip 渲染差异在阈值内。

**风险**：映射表维护成本高；先支持 20 个常用节点，其余走烘焙。

---

### 优先级矩阵（I1–I12）

| 项 | 影响 | 工作量 | 风险 | 建议 |
| --- | --- | --- | --- | --- |
| I1 MIR | 极高 | 中 | 低 | P0，立刻 |
| I2 预算求解器 | 高 | 中高 | 中 | P0，随 TX2 |
| I3 表面经历 | 高 | 中 | 低 | P1，TX4 |
| I4 闭包预算 | 高 | 高 | 中高 | P1，窄版本 |
| I5 统一 ABI | 极高 | 中 | 低 | P0，随 TX2 |
| I6 变体闭包证明 | 中高 | 中 | 低 | P1 |
| I7 体检台 | 高 | 中 | 低 | P1 |
| I8 收据 diff | 中高 | 低中 | 低 | P1 |
| I9 AI 闭环 | 极高 | 高 | 中 | P2，依赖 I1/I7 |
| I10 逆渲染 | 高 | 高 | 高 | P2，研究 |
| I11 神经编码 | 中高 | 高 | 高 | P2，实验 |
| I12 损失报告 | 中高 | 中 | 中 | P2，工具 |

---

## 7. 推荐架构

### 7.1 分层

```text
作者入口                标准入口                 AI 入口
Blender / 编辑器        glTF / MaterialX         NL / 图像 / MCP
      \                    |                      /
       \                   v                     /
        +-------->  MIR（类型化 DAG + 闭包树） <--+
                          |
        +-----------------+------------------+
        |                 |                  |
   语义/类型校验      成本与依赖分析      变体枚举
        |                 |                  |
        +---------> 计划求解（预算/质量/重要性）
                          |
        +---------+-------+--------+
        |         |                |
     烘焙/打包   代码生成         神经编码候选
        |         |                |
        +----> 内容寻址产物 + 收据 <+
                          |
                 运行时帧快照（材质表）
                          |
        光栅 / Mesh / 蒙皮 / RT 共用求值
```

### 7.2 MIR 草案

```json
{
  "mir": 1,
  "id": "9f2c1d7a-3b44-4c1e-9a10-6d2f8e5b7c31",
  "name": "painted_copper_door",
  "surface": {
    "type": "layer_stack",
    "layers": [
      {
        "op": "base",
        "model": "metal",
        "params": { "baseColor": [0.72, 0.45, 0.20], "roughness": 0.34 }
      },
      {
        "op": "coat",
        "params": { "color": [0.06, 0.18, 0.12], "roughness": 0.16, "ior": 1.5 },
        "coverage": { "texture": "mask_paint", "channel": "r" }
      },
      {
        "op": "wear",
        "amount": 0.35,
        "mask": { "texture": "mask_wear", "channel": "g" },
        "exposes": 0
      },
      {
        "op": "oxidize",
        "amount": 0.40,
        "target": "exposed_metal"
      },
      {
        "op": "wet",
        "amount": 0.60,
        "mask": { "texture": "mask_wet", "channel": "b" }
      }
    ]
  },
  "textures": {
    "mask_paint": { "source": "art/door_paint.png", "semantic": "mask", "colorSpace": "linear" },
    "mask_wear": { "source": "art/door_wear.png", "semantic": "mask", "colorSpace": "linear" },
    "mask_wet": { "source": "art/door_wet.png", "semantic": "mask", "colorSpace": "linear" }
  },
  "budget": { "maxBytes": 4194304, "maxLobes": 2, "maxSamples": 8 },
  "variants": { "quality": ["high", "low"], "platform": ["d3d12"] }
}
```

字段说明：

- `op` 是**算子**，不是任意节点；算子有固定语义和版本。
- `coverage` / `mask` 只接受已声明语义的遮罩纹理；缺省值由算子定义。
- `exposes` 指向层索引，声明“磨损后暴露哪一层”，这是耦合规则的落点。
- `budget` 是材质的局部约束，全局约束由求解器注入。
- `variants` 只是声明；实际变体集合由场景分析决定（I6）。

### 7.3 编译流水线

| 阶段 | 输入 | 输出 | 失败诊断 |
| --- | --- | --- | --- |
| 1. 解析/校验 | MIR JSON | 类型检查后的 AST | `NG-E-MAT-TYPE`、`NG-E-MAT-RANGE` |
| 2. 规范化 | AST | 折叠/CSE/DCE 后的 AST + hash | 循环依赖、未连接端口 |
| 3. 分析 | AST | 需求集（UV/切线）、成本、变体位、纹理依赖 | 缺 UV、缺切线、超采样预算 |
| 4. 计划 | 分析结果 + 预算 | 候选选择、材质 LOD、纹理规格 | 预算冲突 |
| 5. 烘焙 | 源图 + 计划 | 遮罩、通道打包、mip、压缩 | 损坏图、颜色语义错 |
| 6. 代码生成 | AST + 变体位 | HLSL → DXIL | 编译错误、寄存器超限 |
| 7. 链接 | DXIL + 根签名/描述符布局 | PSO、绑定布局 | 根参数超限 |
| 8. 打包 | 全部产物 | 内容寻址包 + 收据 | 依赖缺失 |

### 7.4 运行时数据布局

```text
struct MaterialRecord {          // 建议 128 字节对齐
  uint   flags;                  // 透明、双面、变体位
  uint   closureCount;           // 1..3
  uint   tex[6];                 // bindless 索引：base/normal/orm/mask0/mask1/mask2
  float  params[10];             // roughness 偏移、metalness、wet、wear、oxidize、ior...
  float4 baseColor;              // 线性
  float4 extra;                  // emissive + 强度、clearcoat 等
};
```

- 光栅/Mesh/蒙皮：draw 常量里只放 `recordIndex`，记录从 StructuredBuffer 取。
- RT：closest-hit 用 `instanceID` 映射到 `recordIndex`。
- 纹理走 bindless（SM 6.6 堆索引）或 8 槽兼容表；两者共用 `EvaluateMaterial` 的资源获取接口。
- 帧快照：材质表按帧版本化，旧版本等 fence 后回收（对应 TX3）。

### 7.5 示例：铜门的一次调节

输入：“把铜门氧化加重一点，但别动绿漆。”

1. AI 生成 patch：`oxidize.amount: 0.40 → 0.55`，作用域 `exposed_metal`（不碰 coat）。
2. 引擎校验：`oxidize` 只作用于暴露金属，绿漆覆盖区域不受影响；预算不变。
3. 预览：只重建氧化相关的遮罩/派生贴图，其他产物命中缓存。
4. 收据：记录 patch、受影响产物、显存/耗时、前后对比图。
5. 提交：场景事务写入新 revision；撤销恢复旧参数与旧产物引用。

---

## 8. 路线图与验收

| 阶段 | 目标 | 交付 | 门禁 |
| --- | --- | --- | --- |
| A（TX0–TX1） | 打通“一张图 → 一个像素” | MIR v0、PNG→DDS、MaterialRecord、体检台骨架 | 字节一致重建；依赖闭包完整；缺图/坏图有诊断 |
| B（TX2–TX4） | 多贴图、共用求值、表面经历 | normal/ORM、UV/tangent、I3 算子、I2 求解器、收据 diff | 四条路径一致；12 道具共享贴图；预算冲突可解释 |
| C（研究） | AI 与神经编码 | I9 patch 闭环、I10 拟合、I11 候选、I12 损失报告 | 预览=提交；拟合误差有界；神经候选不劣化默认路径 |

**阶段性指标（建议，需实测冻结）**：

- 基础色压缩 SSIM ≥ 0.99，并记录线性颜色误差。
- 法线解码角误差 P95 ≤ 2°。
- 同一 MIR 两次清缓存重建的产物与 manifest 字节一致。
- 变体闭包完整率 100%（运行时 `missing variant` = 0）。
- 纹理预算命中率 100%，冲突时有可执行建议。
- 材质编辑的预览-提交一致率 100%（同一 patch 同一结果）。

---

## 9. 反模式

1. **一开始就做任意节点图。** 变体爆炸、工具成本、调试成本会同时到来；先用分层栈 + 小节点集覆盖 80%。
2. **直接搬 Substrate。** 闭包模型是方向，但完整实现对小引擎过重；先做 2 闭包窄版本。
3. **运行时调大模型或神经推理。** 破坏确定性、可回放与离线构建；生成应发生在 cook/编辑器侧。
4. **让 AI 生成 HLSL 字符串。** 无法类型检查、无法计价、无法安全撤销；应生成 MIR patch。
5. **用槽号当材质身份。** 槽号是会话态；资产身份必须是 UUID + artifact hash。
6. **只做编辑器预览。** 预览不是像素证据；必须落到原生截图与逐像素对比。
7. **把“贴图接上了”当成 PBR 完成。** 光照、IBL、能量守恒、色调映射都还没做。
8. **用单一 SSIM 代替视觉验收。** 压缩伪影、法线方向、高光变化需要不同指标与固定光照复核。

---

## 10. 开放问题

1. 闭包预算取 2 还是 3？取决于目标场景的材质复杂度与 G-buffer 预算，需要基准集实测。
2. MIR 的粒度：节点级（灵活但图大）还是算子级（可控但表达力受限）？建议混合：算子为主，节点为逃生门。
3. 预算求解用贪心还是 ILP/CP？贪心可解释、够快；ILP 只在离线批量构建时值得。
4. 神经编码在 D3D12 上的解码确定性如何保证？需要固定解码器与逐字节回归。
5. 逆渲染的评测基准从哪里来？合成数据可验参数，真实照片只能验残差与主观。
6. 跨引擎损失报告的最小可用节点集是多少？建议先覆盖 Principled BSDF 的 12 个常用输入 + Noise/Voronoi/Bump/Mix。

---

## 11. 参考

> 方法与局限：检索于 2026-10-20 进行，以公开搜索结果与本仓库文档为准；论文结论基于摘要与公开描述，未逐篇复现实验；厂商实现细节可能随版本变化。文中“事实 / 判断 / 推测”已尽量标注，未标注处默认为工程判断。

**标准与工具**

- [MaterialX](https://materialx.org/) · [GitHub](https://github.com/AcademySoftwareFoundation/MaterialX)
- [OpenPBR](https://academysoftwarefoundation.github.io/OpenPBR/)
- [NVIDIA MDL SDK](https://github.com/NVIDIA/MDL-SDK)
- [glTF 2.0 规范](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
- [Filament Materials Guide](https://google.github.io/filament/Materials.html)
- [Unreal Engine Substrate Materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/substrate-materials-in-unreal-engine)
- [Epic BlenderTools（Send to Unreal）](https://github.com/EpicGames/BlenderTools)
- [Unreal Datasmith](https://www.unrealengine.com/en-US/datasmith)
- [Blender Principled BSDF](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/principled.html)

**论文**

- [NeuMIP: Multi-Resolution Neural Materials](https://dl.acm.org/doi/10.1145/3450626.3459795)（SIGGRAPH 2021）
- [Random-Access Neural Compression of Material Textures](https://dl.acm.org/doi/10.1145/3592407)（SIGGRAPH 2023）
- [MaterialGAN: Reflectance Capture using a Generative SVBRDF Model](https://arxiv.org/abs/2010.00114)（SIGGRAPH Asia 2020）
- [Material Anything: Generating Materials for Any 3D Object via Diffusion](https://arxiv.org/abs/2411.15138)（CVPR 2025）
- Weidlich & Wilkie, *Arbitrarily Layered Micro-Facet Surfaces*（GRAPHITE 2007）

**渲染架构**

- [The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading](http://jcgt.org/published/0002/02/04/)（JCGT 2013）
- [GPU-Driven Rendering Pipelines](https://advances.realtimerendering.com/s2015/)（SIGGRAPH 2015 course）
- [Mitsuba 3](https://mitsuba-renderer.org/) · [nvdiffrast](https://nvlabs.github.io/nvdiffrast/)

**本仓库**

- [TEXTURE-MATERIAL-PLAN.md](TEXTURE-MATERIAL-PLAN.md)（材质与纹理工作包）
- [GEOMETRY-LAYOUT.md](GEOMETRY-LAYOUT.md)（材质句柄与几何包）
- [STATUS.md](STATUS.md)（当前能力与约束）

---

## 附：一句话回答

**引擎材质管线有五类方案**：固定参数、Uber-shader、节点图编译、DSL/标准 IR、数据驱动 bindless；**真正决定成败的是 IR、变体策略、绑定模型、求值架构四者的组合**。**“ueblender”这类工具缺的不是转换脚本，而是带能力协商和损失报告的语义 IR**。对本引擎，最值得创新的不是“再做一个 UE 节点图”，而是把材质做成**可编译、可计价、可回放、可被 AI 安全编辑**的 MIR，并在它之上做预算求解、表面经历算子和跨路径统一求值。
