# 第三人称基础项目

新建项目保留 `template: blank` 接口兼容性，实际复制 `.screen/basic.scene.json`。项目包含 40 米网格地面、1.8 米机器人、三个动态方块、两个跳跃台与静态碰撞墙。实体与项目 ID 每次创建重新生成。

基础角色来自用户提供的 `futuristic+robot+3d+model.glb`，原始输入保存在 `assets/source/builtin/robot.glb`。行走、奔跑、跳跃使用原始动画；待机取行走循环平均姿态，坠落取跳跃中段姿态。髋骨 X/Z 平移烘焙为原地动作，避免视觉模型偏离碰撞体。`npm run builtin:prepare` 重建 `assets/builtin` 及场景中的角色哈希；不访问用户 Downloads 路径。

基础贴图保存用户选定的 imagegen 白底灰线原图，位于 `assets/source/builtin/grid-white-gray.png`。运行时使用带 mip 的 `engine_grid` 基色、平法线与非金属 ORM，世界投影每 5 米重复一张，每格约 1 米。未指定材质的绘制使用宿主追加的内部网格材质；用户明确指定的材质照常绑定。

项目的角色与纹理均复制到其 `assets/` 内。`.screen/` 场景的资源引用统一相对于项目 `assets/`；导入包预览和发布包仍相对于场景目录。新建、导入引用、角色边界拟合、资源目录显示、原生读取与打包使用一致规则。项目的场景、构建、报告和保存均写入 `Documents/nativeEngineProject/<名称>`。

编辑器“运行”启动原生第三人称窗口，“停止运行”关闭其拥有的进程。WASD 行走、Shift 奔跑、空格跳跃、右键拖动相机、F5 重置；运行前保存草稿。编辑预览继续在内嵌视口显示。

增量验证入口为 `npm run test:starter`，资产依赖测试为 `npm run test:assets`。不运行全量测试或完整 CI。验收画面、原生报告及 `acceptance.json` 均保存在验收项目内的 `reports/starter/`。
