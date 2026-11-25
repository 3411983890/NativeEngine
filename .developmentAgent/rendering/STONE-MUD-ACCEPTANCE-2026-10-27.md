# 多层 PBR 验收与石板湿泥预览

日期：2026-10-27。

## 验收结论与范围

本次多层 PBR 专项 **38 项通过、0 失败、0 跳过**。启用了 `NG_LAYER_SCENE_TEST=1` 和 `NG_LAYER_GPU_TEST=1`，验证了常量覆盖、逐像素遮罩、高度混合、四层顺序、独立贴图、镜像/退化 UV 法线、UV 流选择、蒙皮、场景保存/撤销、纹理去重、打包画面一致性，以及 CPU/C++/GPU 数值参考。

验收前修复 `engine/native/gameplay_snapshot.cpp` 中缺失的 `Json` 类型别名。并行动画开发使普通构建的输入校验多次中止，因此用现有 `tools/asset-host-snapshot.mjs` 捕获源码后构建，保留了输入哈希；没有绕过输入变化保护，也没有运行全量测试或完整 CI。

受测宿主：`build/assets-host/frozen-297fa922-3cf6-4c5a-8a90-e5bbffe7bce4/NativeGPU.exe`。

SHA-256：`3d29b32f78a426c2e5ca5b2c4595ee0a900a8f6c772c408b4b1f88c7256525c0`。

本次结论针对运行时多层 PBR 参数混合。物理涂层闭包仍为参考/研究功能，不能据此宣称已有完整运行时 Substrate。高度贴图参与覆盖混合，不产生几何置换；原始 16-bit 高度文件保留，当前运行时高度纹理为 8-bit。

## 交付工程

工程：`C:/Users/34119/Documents/nativeEngineProject/StoneBrick-WetMud-Atmosphere`。

- 可编辑场景：`.screen/main.scene.json`。
- 全景/近景截图：`reports/overview.png`、`reports/detail.png`，1440×900。
- 双击 `Preview.cmd` 或 `Preview-Detail.cmd` 启动实时预览；实时窗口使用 960×600。
- 预览启动器现已改为自由编辑相机（替换最初锁定镜头的截图预览）：Alt＋左键环绕、中键平移、滚轮缩放、右键＋WASD 漫游、Q/E 升降、Shift 加速、F 聚焦地面、R 复位。窗口标题也显示操作说明。`reports/free-camera-check.json` 记录原生窗口消息触发后的相机状态变化，实际绘制截图为 `reports/free-camera-check.png`；编辑相机基础专项另有 26 项通过。
- 来源与用途清单：`reports/source-material-receipt.json`，包含所有原图的 SHA-256 和运行时层绑定。
- 验收数据和测试日志：`reports/acceptance.json`、`reports/material-acceptance.log`。
- 项目自己的构建产物：`outputs/<buildKey>/`，原始贴图保存在 `assets/source/`。

该真实 PBR 工程的打包运行已单独复核：`reports/packaged.png` 与源场景全景逐像素一致，结果写入 `reports/acceptance.json` 的 `packagePixelParity`。

## 所有贴图的用途

用户给定 `StoneBrick`、`WetMud` 各 8 张图，共 16 张，全部读取、保存并参与材质生成与运行时采样。

| 来源 | 用途 |
| --- | --- |
| BaseColor | 对应材质两层的 sRGB 基色 |
| Normal OpenGL | 对应 ORM 层的法线 |
| Normal DirectX | 翻转绿色通道后，供对应独立贴图层采样；泥巴随机旋转时同步旋转法线 XY |
| ORM | 对应 ORM 层的 AO、粗糙度、金属度 |
| AO / Roughness / Metallic | 对应独立贴图层各自采样，不与 ORM 数值相乘而重复强化 |
| Height 16-bit | 保留原文件；转换为线性运行时高度，用于石板和泥巴交界混合 |

四层依次为 `Stone_ORM_OpenGL`、`Stone_Independent_DirectX`、`Mud_ORM_OpenGL`、`Mud_Independent_DirectX`。石板两种表达以 0.5 覆盖组合；泥巴两次覆盖使用补偿后的同一遮罩。ORM 与独立标量来源核对一致；两种法线转换后的量化误差记录在来源清单。

运行报告确认：4 个活动层、22 个贴图采样槽、17 个去重纹理（包含新增泥巴分布遮罩），分层纹理约 234.7 MiB。

## 随机分布与光照

- 石板保留规则重复，源贴图每 3 米重复一次。
- 泥巴烘焙成覆盖 16×16 米地面的整幅 2048×2048 图集，运行时 clamp 采样，避免把小块泥巴机械平铺到整个地面。
- 固定种子 `27012026`，随机单元位置、连续旋转、0.65–1.55 倍尺度与偏移；混合近邻样本以软化接缝。所有 PBR 通道共享坐标，颜色在烘焙时按线性光混合，法线同步变换后归一化。
- 23 个非均匀斑块与多频噪声构成扩散遮罩，层交界结合高度纹理。
- 使用项目自己的 HDR 天空、低角度暖阳、蓝色天空补光、环境反射和轻雾。没有使用图片合成替代原生渲染。

设备为 AMD Radeon 890M。1440×900 的最终全景/近景 GPU P50 分别约 48.44/78.39 ms；这是完整场景测量，不是对所有设备的帧率承诺。
