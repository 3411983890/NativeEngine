# 雾镇重启 · 第一人称示例

在现有 Windows D3D12 原生引擎中运行的短篇冒险。五个相连区域共用一份场景与 Lua 玩法：镇口领取钥匙、仓库推箱取工具、维修院爆破供电、广场顺序机关、信号塔限时连接并完成结局。

## 运行

在工程根目录运行 `npm run fogtown`，首次会构建原生程序。也可以双击本目录的 `启动雾镇.cmd`。点击游戏画面后进入鼠标视角。

- `WASD` 移动，鼠标看向四周，`Shift` 跑，`Space` 跳。
- 对准目标时出现 `[E]`，按 `E` 交互；墙体会遮挡交互。
- `Esc` 暂停并释放鼠标；点击画面继续。关闭窗口退出。
- `F7` 快速保存，`F12` 恢复快速保存，`F5` 从头开始。
- 取工具、修复发电机、接通广场、完成结局时自动保存检查点。
- `npm run fogtown:continue` 恢复上次退出进度；没有退出存档时尝试检查点。

## 导览、验证和交付

`npm run fogtown:tour` 打开开发者导览：`F1` 显示或隐藏诊断侧栏，`F2` 循环加载镇口、工具已取得、维修完成、广场恢复四份完整模拟快照。跳转同时恢复任务、背包、门、物理、计时器和动画状态。首次导览会生成这些快照。

`npm run fogtown:verify` 通过真实移动和交互输入通关，验证错误顺序复位、原生爆破、压力板双门、保存重启续跑、超时重试以及同输入摘要一致，并生成原生画面。证据在 `reports/fogtown/`。

`npm run fogtown:package` 创建 `outputs/Fogtown-FirstPerson-win64/` 与同名 ZIP。分发包内 `Play.cmd` 新开游戏，`Continue.cmd` 继续，`Tour.cmd` 进入导览；独立运行不需要 Node、编译器或本工程目录。

## 内容入口

- `project.json`：场景、行为和稳定实体 ID。
- `../../.screen/fogtown.scene.json`：335 个实体、材质、雾与初始内容。
- `../../assets/scripts/fogtown/`：任务、交互、门、压力板、供能灯行为及字段描述。
- `../../tools/fogtown/generate.mjs`：可重复生成场景、行为描述和原始提示音。
- [游玩路线](docs/walkthrough.md)、[制作说明](docs/authoring-guide.md)、[功能覆盖](docs/feature-coverage.md)、[已知限制](docs/known-limitations.md)。
