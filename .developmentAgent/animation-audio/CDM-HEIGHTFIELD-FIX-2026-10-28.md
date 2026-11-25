# CDM 高度场接地与坡面跳跃修复

日期：2026-10-28。实际项目：`C:/Users/34119/Documents/nativeEngineProject/Newproject`。

## 原因与改动

- `Game::setCharacter` 原本只对物理体包围盒做向下射线。高度场的包围盒并非真实地面，脚部会命中错误高度或返回 `no-ground`。现在高度场分支使用同一碰撞高度采样和法线，检查射线范围与 XZ 边界，按最近命中与普通碰撞体竞争；静态高度场锚点使用世界坐标。
- `tryStepUp` 曾对前方高度场采样并持续更新台阶保护，平缓坡面也被当作台阶。这会让身体提前悬在地面上方。连续地形现在交给移动后的真实高度解析，箱体台阶保留原有踏步逻辑。
- 成功起跳清除 `stepGrace_`，防止台阶保护随后把垂直速度设回零。
- 连续下坡原本靠重力追赶每帧下降的地面，造成短暂离地和 CDM 反复重置。新增有界贴地距离，仅用于此前确实站在地形上且非上升状态的移动；距离不超过当 tick 水平位移加接触容差。起跳和明显落差不使用该贴地路径，普通箱体地面命中优先保留。

## 增量验证

沿用 `npm run test:heightfield`，增加 `.incrementalTesting/simulation/heightfield-character.cpp`。修复前已复现 CDM 未读取高度场的失败；修复后验证真实坡面法线、非零高程接触、上下坡持续贴地、坡上跳跃、起跳脚锁释放及台阶保护期间跳跃。CI 仍调用同一入口，无需启动 GPU。

实际项目通过 `NG_HEIGHTFIELD_SCENE` 额外运行 720 tick（行走、反向、三次跳跃），报告位于项目 `reports/map/cdm-terrain-validation.json`。首次修复验证中三次跳跃均成功，身体接地时 `no-ground` 为零，并建立有效脚锁。滑动与穿透峰值保留在报告中，不能以这组路径测试代表全地图所有坡度与动作均已验收。

补充验证：当前源码构建的项目原生程序运行 240 帧成功，最终 `fallback=none`，双脚接触与脚锁均有效，水体仍启用；报告为项目 `reports/map/cdm-terrain-render.json`。直接运行已有原生动画和移动专项检查均通过，报告/日志为 `cdm-animation-regression.json`、`cdm-animation-regression.log` 和 `cdm-simulation-regression.log`。编辑器项目专项 4 项通过。

当前 720 tick 轨迹的接触滑动峰值约 0.602 m/s、脚部穿透度量峰值约 0.0425 m，包含起落与接触转换；本次修复消除接地查询和跳跃冲突，不将这些非零指标描述为零滑动或全地图动作验收。

只运行本次变更相关的增量检查，不执行全量测试、verify 或完整 CI。项目使用独立的 `build/NativeGPU.exe` 与 `build/viewport-host/nativegpu_viewport_host.exe`；编辑器优先使用项目中的运行程序。
