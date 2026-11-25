# 增量测试命令索引

所有可执行的专项测试入口按领域存放；日常优先运行对应领域，不默认执行全量测试。

## 真实水体

- `npm run test:water`：水体运行时位移/冲量、质量账本、边界与输入拒绝、连续表面重放、同 tick 多接触与 sequence/epoch 身份、双足接触状态机、方向旋转、坡床静水、局部进出口账本、静态障碍、反作用冲量、泡沫搬运、瀑布转移账本、LOD 隔离以及原有水资产合同。仅编译水体 CPU 依赖。
- `npm run test:water:gpu`：独立编译水体着色器与 GPU 依赖，验证残差高度回读、同 tick 批次容量续传、方向拉伸、长条域比例、真实绘制纹理绑定与流向平流。不构建完整宿主。
- `npm run water:verify -- --project <项目路径>`：仅构建 `build/pond-host` 宿主依赖，在指定水塘项目内验证连续水面、HDR 折射/吸收/反射绘制及固定 tick 扰动。可用 `--skip-build` 复用来源与二进制哈希均匹配的宿主。
- 默认项目为 `Documents/nativeEngineProject/InteractivePond`；数值与画面报告写入该项目的 `reports/water-realism/`。CPU 数值检查不启动项目。
- 水体画面入口同时运行 `simulation/water-ripple-gpu.cpp`，回读真实 GPU 高度和绘制结果，检查首帧注入、按 sequence 的事件去重与批次容量续传、重放、四方向网格偏差、方向拉伸、长条水域比例、流速平流、波高纹理绑定与 D3D12 错误。它验证有界细波纹层；浮力/阻力、编辑器与发布仍须各自验收。

## 大场景世界生成

- `npm run world:build`：只构建 `build/world-host`（`world_cook.exe` 与 `native_world_tests.exe`）。不构建宿主、不编译着色器。
- `npm run test:world`：世界 authoring 契约（分块取整、上限、占位哈希、规范化输入摘要、清单校验、分块头、笔迹按世界距离重采样、路径诊断、项目路径穿越）。纯 JS，不启动 GPU。
- `npm run test:world-native`：先跑上面的契约测试，再构建并运行 `native_world_tests.exe`。覆盖确定性（同输入逐字节一致、不同块顺序一致）、负坐标、跨块接缝、河流单调下降、桥梁检测、树间距跨块、分块序列化与损坏拒绝、预算削减、运行时状态机与上传握手、聚落入口连通、清单不携带作者源。
- `npm run world:verify -- --project <项目路径> [--world <id>]`：在项目内 cook 一次，然后用原生视口渲染并截图，再检查像素（颜色种类、主导色占比、亮度标准差）。只有画面不是空白且原生统计非零才通过。证据写入项目的 `reports/world-generation/`。
- `npm run world -- new|cook|info|verify|package --project <路径> --world <id>`：生产入口。`new` 生成一份可直接 cook 的古风山谷示例（地形、河流、道路、森林、草地、岩石、聚落），`cook` 产出 `assets/world-cooked/<manifestHash>/`，`package` 只导出清单与分块。

世界源文件位于项目的 `.screen/world/`，组件库位于 `assets/world-libraries/`，cooked 内容位于 `assets/world-cooked/`。编辑器内的“世界生成”工作区通过同一套 `world_*` 受控调用操作这些文件。

## 多层材质契约

- `npm run test:layered-material`：版本化多层材质校验、贴图依赖哈希、UV、覆盖/高度与 CPU 混合参考测试。
- `npm run test:surface-relief`：`nativegpu.layered-material/2` 的 `surfaceRelief` 契约、v1→v2 显式迁移、96 字节 GPU 记录布局、平面/坡面/凹槽求交参考、零幅度回退，以及 R16 高度纹理 cook/读取/平均 mip 往返。不启动 GPU。
- `npm run test:heightfield`：高度场采样/法线、刚体落地，以及角色上下坡贴地、CDM 高度场接触、起跳释放脚锁和台阶保护期间跳跃。编译物理、角色与动画所需原生依赖，不启动 GPU；需要已有 `build/gameplay/nativegpu-gameplay.a`。可用 `NG_HEIGHTFIELD_SCENE` 指定实际项目场景，用 `NG_HEIGHTFIELD_REPORT` 将 720 tick 的行走/跳跃证据写入该项目报告目录。
- `.incrementalTesting/rendering/layered-material.test.mjs`：验证数据、参考计算、原生读取、shader 契约与原生画面（`NG_LAYER_SCENE_TEST=1`；硬件参考 `NG_LAYER_GPU_TEST=1`）。
- `.incrementalTesting/rendering/layered-budget.mjs`：1–4 层GPU/CPU 实测脚本，结果写入 `build/layered-budget/summary.json`，不纳入 CI。
- 可在支持 SM 6.6 的 Windows 硬件上设置 `NG_LAYER_GPU_TEST=1` 后运行上述专项，额外执行 336 组 GPU 与 CPU 混合函数对照；未设置时该硬件测试会明确标记为跳过。

## 第三人称基础项目

- `npm run test:starter`：新建项目与隔离测试，以及机器人、行走/奔跑/跳跃/落地、推动刚体、墙体碰撞、默认网格与发布包的原生验证。
- `.incrementalTesting/editor/starter-project-check.mjs`：仅构建 `build/assets-host` 所需依赖；可用 `--skip-build` 复用本次已构建的宿主。
- 验收项目位于 `%USERPROFILE%/Documents/nativeEngineProject/StarterAcceptance-*`，画面与报告保存在项目内的 `reports/starter/`。
- 原生契约源：`.incrementalTesting/editor/starter-controller.cpp`。
- `.incrementalTesting/editor/starter-paths.test.mjs`：验证 `.screen/` 场景的引用、拟合、导入、更新和打包均使用项目自身的 `assets/`。

## 资产导入与格式

### npm 入口

- `npm run assets:verify` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/assets/asset-import-check.ps1`
- `npm run test:assets` → `node --test tests/asset-import.test.mjs tests/character-import.test.mjs tests/gltf-compression.test.mjs tests/animation-retarget.test.mjs tests/character-instances.test.mjs tests/asset-coordinates.test.mjs tests/character-fit.test.mjs tests/audio-import.test.mjs tests/scene-import.test.mjs tests/texture-import.test.mjs tests/emissive-import.test.mjs tests/texture-mapping.test.mjs tests/texture-coordinates.test.mjs tests/vertex-color-unlit.test.mjs tests/transparency-import.test.mjs tests/occlusion-import.test.mjs tests/environment-import.test.mjs tests/tangent-import.test.mjs tests/tangent-morph.test.mjs tests/mikktspace-import.test.mjs tests/ior-import.test.mjs tests/clearcoat-import.test.mjs tests/asset-source-identity.test.mjs tests/asset-reimport.test.mjs`

### 独立脚本

- `.incrementalTesting/assets/asset-audio-check.mjs`
- `.incrementalTesting/assets/asset-clearcoat-check.mjs`
- `.incrementalTesting/assets/asset-clearcoat-public-check.mjs`
- `.incrementalTesting/assets/asset-coordinates-check.mjs`
- `.incrementalTesting/assets/asset-emissive-check.mjs`
- `.incrementalTesting/assets/asset-environment-check.mjs`
- `.incrementalTesting/assets/asset-environment-public-check.mjs`
- `.incrementalTesting/assets/asset-import-check.ps1`
- `.incrementalTesting/assets/asset-ior-check.mjs`
- `.incrementalTesting/assets/asset-ior-hot-check.mjs`
- `.incrementalTesting/assets/asset-mapping-check.mjs`
- `.incrementalTesting/assets/asset-mapping-public-check.mjs`
- `.incrementalTesting/assets/asset-mikktspace-check.mjs`
- `.incrementalTesting/assets/asset-mikktspace-public-check.mjs`
- `.incrementalTesting/assets/asset-occlusion-check.mjs`
- `.incrementalTesting/assets/asset-occlusion-public-check.mjs`
- `.incrementalTesting/assets/asset-reimport-check.mjs`
- `.incrementalTesting/assets/asset-scene-check.mjs`
- `.incrementalTesting/assets/asset-tangent-check.mjs`
- `.incrementalTesting/assets/asset-tangent-morph-check.mjs`
- `.incrementalTesting/assets/asset-tangent-public-check.mjs`
- `.incrementalTesting/assets/asset-texture-check.mjs`
- `.incrementalTesting/assets/asset-texture-coordinates-check.mjs`
- `.incrementalTesting/assets/asset-texture-coordinates-public-check.mjs`
- `.incrementalTesting/assets/asset-transparency-check.mjs`
- `.incrementalTesting/assets/asset-transparency-public-check.mjs`
- `.incrementalTesting/assets/asset-vertex-color-check.mjs`
- `.incrementalTesting/assets/asset-vertex-color-public-check.mjs`
- `.incrementalTesting/assets/assets-public-check.mjs`
- `.incrementalTesting/assets/character-controller-check.mjs`
- `.incrementalTesting/assets/character-instances-check.mjs`
- `.incrementalTesting/assets/generate-texture-fixtures.mjs`

## 编辑器

### npm 入口

- `npm run test:editor` → `node --test tests/editor-*.test.mjs`
- `npm run test:editor-camera` → `node .incrementalTesting/editor/editor-camera-check.mjs`（编译并运行编辑相机契约测试；不链接 D3D12，不启动编辑器）
- `npm run test:editor-scene` → `node .incrementalTesting/editor/scene-preview-check.mjs`（需要先运行 `tools/viewport-host.ps1` 构建宿主，并需要可见桌面与 D3D12；验证草稿覆盖、清除、物理模拟、停止恢复和正交视图）

### 独立脚本

- `.incrementalTesting/editor/editor-camera.cpp`：编辑相机的原生契约源，覆盖眼睛位置、手性、平移平面、聚焦距离、正交投影和极点限制。
- `.incrementalTesting/editor/editor-camera-check.mjs`：编译上述契约并校验 JSON 结果。
- `.incrementalTesting/editor/scene-preview-check.mjs`：通过命名管道驱动原生视口，比较真实帧像素；截图与报告写入 `build/viewport-host/scene-preview-smoke/`。

## 渲染与画面证据

### npm 入口

- `npm run budget` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/budget.ps1`
- `npm run compute` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/compute.ps1`
- `npm run contract` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/error-contract.ps1`
- `npm run image-regression` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/image-regression.ps1`
- `npm run night` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/night.ps1`
- `npm run probe` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/probe.ps1`
- `npm run smoke:host` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/rendering/smoke-host.ps1`

### 独立脚本

- `.incrementalTesting/rendering/budget.ps1`
- `.incrementalTesting/rendering/campfire-night.ps1`
- `.incrementalTesting/rendering/campfire.ps1`
- `.incrementalTesting/rendering/capture.ps1`
- `.incrementalTesting/rendering/compute.ps1`
- `.incrementalTesting/rendering/error-contract.ps1`
- `.incrementalTesting/rendering/image-regression.ps1`
- `.incrementalTesting/rendering/lighting-quality.ps1`
- `.incrementalTesting/rendering/mesh-bindless.ps1`
- `.incrementalTesting/rendering/night.ps1`
- `.incrementalTesting/rendering/probe.ps1`
- `.incrementalTesting/rendering/rt-shadows.ps1`
- `.incrementalTesting/rendering/smoke-host-snapshot.mjs`
- `.incrementalTesting/rendering/smoke-host.ps1`
- `.incrementalTesting/rendering/smoke-render-audit.mjs`

## 模拟、物理、特效、烟雾与布料

### npm 入口

- `npm run cloth:batch` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/cloth-batch.ps1`
- `npm run cloth:draw` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/cloth-draw.ps1`
- `npm run cloth:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/cloth-gpu.ps1`
- `npm run cloth:grouped` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/cloth-grouped.ps1`
- `npm run cloth:host` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/cloth-host.ps1`
- `npm run effects:cpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-cpu.ps1`
- `npm run effects:deposits` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-deposits.ps1`
- `npm run effects:deposits:host` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-deposit-host.ps1`
- `npm run effects:deposits:service` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-deposit-service.ps1`
- `npm run effects:fields` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-fields.ps1`
- `npm run effects:fields:host` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/field-host.ps1`
- `npm run effects:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-gpu.ps1`
- `npm run effects:gpu:draw` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-gpu-draw.ps1`
- `npm run effects:runtime` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/effects-runtime.ps1`
- `npm run fields` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/fields.ps1`
- `npm run fields:adapters` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/field-adapters.ps1`
- `npm run fields:coordinator:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/field-coordinator-gpu.ps1`
- `npm run fields:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/fields-gpu.ps1`
- `npm run fields:joint:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/fields-joint-gpu.ps1`
- `npm run fields:local:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/fields-local-gpu.ps1`
- `npm run fields:services:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/fields-services-gpu.ps1`
- `npm run fields:shared-wind` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/shared-wind.ps1`
- `npm run fields:simulation` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/simulation-fields.ps1`
- `npm run fields:world:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/fields-world-gpu.ps1`
- `npm run physics` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/physics.ps1`
- `npm run simulation:baseline` → `node .incrementalTesting/simulation/simulation-baseline.mjs`
- `npm run simulation:suite` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/simulation-suite.ps1`
- `npm run smoke:fields` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/smoke-fields.ps1`
- `npm run smoke:gpu` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/smoke-gpu.ps1`
- `npm run smoke:particles` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/smoke-particles.ps1`
- `npm run smoke:service` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/smoke-service.ps1`
- `npm run smoke:sharing` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/simulation/smoke-sharing.ps1`

### 独立脚本

- `.incrementalTesting/simulation/check-effect-deposit-admission.mjs`
- `.incrementalTesting/simulation/cloth-batch.ps1`
- `.incrementalTesting/simulation/cloth-draw.ps1`
- `.incrementalTesting/simulation/cloth-gpu.ps1`
- `.incrementalTesting/simulation/cloth-grouped.ps1`
- `.incrementalTesting/simulation/cloth-host.ps1`
- `.incrementalTesting/simulation/effects-cpu.ps1`
- `.incrementalTesting/simulation/effects-deposit-host.ps1`
- `.incrementalTesting/simulation/effects-deposit-service.ps1`
- `.incrementalTesting/simulation/effects-deposits.ps1`
- `.incrementalTesting/simulation/effects-fields.ps1`
- `.incrementalTesting/simulation/effects-gpu-draw.ps1`
- `.incrementalTesting/simulation/effects-gpu.ps1`
- `.incrementalTesting/simulation/effects-runtime.ps1`
- `.incrementalTesting/simulation/field-adapters.ps1`
- `.incrementalTesting/simulation/field-coordinator-gpu.ps1`
- `.incrementalTesting/simulation/field-host.ps1`
- `.incrementalTesting/simulation/field-service-phase-audit.mjs`
- `.incrementalTesting/simulation/field-services-audit.mjs`
- `.incrementalTesting/simulation/fields-gpu.ps1`
- `.incrementalTesting/simulation/fields-joint-audit.mjs`
- `.incrementalTesting/simulation/fields-joint-gpu.ps1`
- `.incrementalTesting/simulation/fields-local-gpu.ps1`
- `.incrementalTesting/simulation/fields-services-gpu.ps1`
- `.incrementalTesting/simulation/fields-world-gpu.ps1`
- `.incrementalTesting/simulation/fields.ps1`
- `.incrementalTesting/simulation/particles.ps1`
- `.incrementalTesting/simulation/physics.ps1`
- `.incrementalTesting/simulation/shared-wind.ps1`
- `.incrementalTesting/simulation/simulation-baseline.mjs`
- `.incrementalTesting/simulation/simulation-fields.ps1`
- `.incrementalTesting/simulation/simulation-suite.ps1`
- `.incrementalTesting/simulation/smoke-fields-audit.mjs`
- `.incrementalTesting/simulation/smoke-fields.ps1`
- `.incrementalTesting/simulation/smoke-gpu.ps1`
- `.incrementalTesting/simulation/smoke-particles.ps1`
- `.incrementalTesting/simulation/smoke-service.ps1`
- `.incrementalTesting/simulation/smoke-sharing-audit.mjs`
- `.incrementalTesting/simulation/smoke-sharing.ps1`

## 毛发与角色成衣

### npm 入口

- `npm run test:hair` → `node --test .incrementalTesting/vegetation/hair-generator-check.mjs`：发型源规格、真实头皮绑定、分区生成与锁定、LOD 身份、cook 字节确定性、截图选择与过期拒绝、多视图分析冲突、纤维着色与透明预算。纯 JS，约 7 秒，不构建原生宿主。
- `npm run test:garment` → `node --test .incrementalTesting/simulation/garment-weights-check.mjs`：成衣几何诊断、分区转移、左右侧别、锁定值、影响数裁剪、拓扑过期拒绝、显式镜像、物理报告与网格穿透测量。纯 JS，约 20 秒（模拟姿态占大头）。
- `npm run test:garment:native` → `.incrementalTesting/simulation/garment-skinned.ps1`：原生蒙皮布料求解。先用 `tools/garment.mjs` cook 两件夹具成衣（`nativegpu.cloth-cook/3`），再编译 `tests/native_garment.cpp` 与原生布料依赖，运行绑定姿态复现、缝合缝焊接、位移上限、七个姿态的求值序列、姿态切换与重坐、参数与篡改拒绝、确定性。约 40 秒，只编译 5 个翻译单元，不调用 `tools/build.ps1`。
- `npm run test:hair:draw` → `node --test .incrementalTesting/vegetation/hair-render-check.mjs .incrementalTesting/vegetation/hair-gpu-pack-check.mjs`：发丝绘制的 CPU 参考渲染（亚像素覆盖率、有界透明层与溢出兜底、深遮罩自阴影、背光透射、LOD 与距离稳定性）、GPU 上传契约（固定步长、切线/宽度/根尖属性、引导表原样传递、非法曲线与预算拒绝）以及磁盘包 `HGP1` 的读写与篡改拒绝。约 8 秒，输出 PNG 到 `build/hair-render/`。
- `npm run test:hair:gpu` → `node --test .incrementalTesting/vegetation/hair-draw-native-check.mjs`：真实的 D3D12 发丝绘制。把发型 cook 成 `HGP1` 包，生成三视图相机与灯光夹具，编译 `shaders/hair.hlsl` 与 `tests/native_hair_draw.cpp`，在离屏浮点目标上执行一次非索引绘制（6 顶点/段），断言 VS/PS 计数、覆盖率、三视图差异、确定性、五个调试通道、深遮罩自阴影压暗、背光按发色透射、根尖染色，以及 10 类损坏包的拒绝；输出 13 张 PNG 与 `build/hair-draw/evidence.json`。需要硬件 D3D12 GPU（`HAIR_GPU_ADAPTER` 默认 `any`，可选择任意非软件适配器），约 10 秒，仅编译 6 个翻译单元。
- `npm run test:hair:host` → `node --test .incrementalTesting/vegetation/hair-host-check.mjs`：把同一份 `HGP1` 包接进**宿主场景帧**。构建 `build/hair-host/NativeGPU.exe`，用 `.screen/hair-preview.scene.json` 在同一相机下跑四组 240 帧 smoke（无发型 / 无风 / `--groom-wind 5,0,2` / `--groom-head-turn 25`），断言 `groomRuntime`（绘制帧数、提交发丝数、上传字节、深遮罩覆盖、模拟 tick、拉伸 ≤1.05、最终穿透 ≤1 mm、根漂移 0、陈旧上传 0）以及截图像素差：发型改变 >1000 像素且压暗 >500，风再改变 >1000 像素。实测 4,459 / 1,425 / 风 3,417 / 转头 6,437，模拟拉伸 1.000016。输出 `with-groom.png`、`with-groom-wind.png`、`with-groom-head-turn.png`、`without-groom.png`、`evidence.json`。需要硬件 D3D12 GPU，因此未接入无 GPU 的 CI；首次构建宿主较慢，之后走对象缓存（`HAIR_HOST_SKIP_BUILD=1` 仅用于本地已构建二进制，不应作为发布证据）。
- `npm run hair:groom` → `node tools/hair-groom.mjs`：把发型夹具 cook 成原生可读的 `nativegpu.groom/1` 兼容包，并打印 `toCompatibleGroomSource` 的三条已知损失。
- `npm run hair:sample` → `node tools/hair-sample-record.mjs`：一次跑完 cook → 校验 → GPU 打包 → 截图计划 → CPU 参考出图 → 局部 patch，写出 `build/hair-sample/record.json` 与 `frames/*.png`，供 AI 手册引用真实数值。

### 覆盖范围与边界

- 三个入口都基于内置角色 `assets/builtin/robot/character.json` 与**程序生成的成衣**（按角色实测截面外扩 12 mm），因此证明的是转移、约束与报告契约，不是某件导入成衣的最终美术效果。
- 发丝绘制现在有两级证据，边界必须按证据写清楚：
  - `test:hair:gpu` 离屏渲染**真实 D3D12 帧**并统计 IA/VS/PS 与像素，证明绘制模块本身工作；它使用独立浮点目标，不是宿主场景。
  - `test:hair:host` 证明**宿主场景帧**已接通：`--groom` 加载 `HGP1`，渲染器把发丝画进与角色相同的 HDR 目标与深度缓冲，光照取场景 `Environment`，截图走正常 capture 路径。有/无发型的同相机像素差是这份证据的核心。
  - 透明合成是**按根点视深的后到前排序 alpha**：每帧稳定排序 14,448 个 strand，索引写入 58 KB order 缓冲，几何不重排；`HairDrawStats::transparency` 报告 `depth-sorted-alpha`。它不是 OIT 或逐像素有界缓存，交叠发丝仍可能次序错误。
  - 深遮罩网格由 CPU 从发丝构建并上传，只提供**发丝自阴影**，不向身体或场景投影。
  - guide 模拟已实现，但在 CPU：固定 1/60 s tick、4 子步、根固定、头球碰撞、风作为目标速度；每帧插值 14,448 条发丝并写入三份轮换 UPLOAD 点缓冲。没有 GPU guide solve。
  - 发根已绑定到角色 head 骨骼的 skin matrix：头网格与发丝共用同一变换，`--groom-head-turn 25` 改变 6,437 像素；模拟在静止空间，重力/风按逆变换旋转。仍没有走跑/跳跃动作序列。
  - 宿主帧是有风/无风的 240 帧 smoke 序列，不是走跑/跳跃或镜头运动闪烁验收。
  - 硬件闪烁、GPU 时序与显存仍未测。
- 成衣 JS 入口的自碰撞恒为未支持（`SUPPORTED.selfCollision === false`），长裙与宽袖不因本入口通过而获得认证。
- 原生蒙皮入口只覆盖 CPU 求解：没有 GPU 蒙皮布料路径，也没有蒙皮网格绘制通道，所以它证明的是数值契约，不是画面。碰撞代理是**内切**于身体的骨骼分段椭球，因此报告里的穿透深度是“到骨骼核心”的距离，不等于到身体网格的距离；身体网格精确穿透仍未实现。
- 需要新增分类或移动入口时，同步 `package.json`、`.incrementalTesting/commands.json` 与本索引。

## 植被

### 独立脚本

- `.incrementalTesting/vegetation/hair-draw-native-check.mjs`
- `.incrementalTesting/vegetation/hair-generator-check.mjs`
- `.incrementalTesting/vegetation/hair-host-check.mjs`
- `.incrementalTesting/vegetation/vegetation-atlas-diagnostics.mjs`
- `.incrementalTesting/vegetation/vegetation-browser-check.mjs`
- `.incrementalTesting/vegetation/vegetation-evidence-check.mjs`
- `.incrementalTesting/vegetation/vegetation-hair-verify.ps1`
- `.incrementalTesting/vegetation/vegetation-morphology.mjs`
- `.incrementalTesting/vegetation/vegetation-production-report.mjs`
- `.incrementalTesting/vegetation/vegetation-silhouette.mjs`
- `.incrementalTesting/vegetation/vegetation-sync-check.mjs`

## 核心构建、原生测试、CI 与回放

### npm 入口

- `npm run animation` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/animation.ps1`
- `npm run audio` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/audio.ps1`
- `npm run ci` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/ci.ps1`
- `npm run determinism` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/determinism.ps1`
- `npm run gameplay` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/gameplay.ps1`
- `npm run replay` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/replay.ps1`
- `npm run test` → `node --test tests/*.test.mjs`
- `npm run test:native` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/test-native.ps1`
- `npm run verify` → `powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/verify.ps1`

### 独立脚本

- `.incrementalTesting/core/animation.ps1`
- `.incrementalTesting/core/audio.ps1`
- `.incrementalTesting/core/ci.ps1`
- `.incrementalTesting/core/determinism.ps1`
- `.incrementalTesting/core/g1-reliability.ps1`
- `.incrementalTesting/core/g1.ps1`
- `.incrementalTesting/core/gameplay.ps1`
- `.incrementalTesting/core/replay.ps1`
- `.incrementalTesting/core/test-native.ps1`
- `.incrementalTesting/core/verify.ps1`

未分类的 Node 单元测试仍位于 `tests/`，通过 `npm test`、`npm run test:editor`、`npm run test:assets` 运行。完整来源见 `moved-scripts.json`。
