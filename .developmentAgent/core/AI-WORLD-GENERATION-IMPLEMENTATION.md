# 大场景世界生成：实现记录（2026-10-28）

- 对应方案：[AI 地图编辑与大场景程序化生成：原生功能开发文档](AI-WORLD-GENERATION-DEVELOPMENT-PLAN.md)
- 状态：P0–P5 的主体已实现并可验证；P6（完整客栈室内与住宅区深化）未实现，编辑器与清单均明确标注。
- 证据：`npm run test:world`（26 项）、`npm run test:world-native`（15 组 112 项）、`npm run world:verify`（原生画面 + 像素分析）。

## 1. 已实现模块

| 位置 | 内容 |
| --- | --- |
| `engine/world/` | 契约层：错误码、recipe/library/layer/override 校验、规范化输入摘要、分块坐标数学、清单校验、cooked 分块头解析、项目路径约束、笔迹重采样与路径诊断、默认古风组件库、cook 管线（`cook.mjs`） |
| `engine/native/world/` | C++ 编译器与运行时：确定性哈希与 xoshiro256\*\*、噪声、地形高度场、路径网络（样条、河流落差、桥梁）、散布（蓝噪声优先级筛选）、聚落与地块、NGRC 分块容器、编译管线、流式运行时与空间查询 |
| `editor/world-service.mjs` | 任务状态机、epoch、草稿与暂存、预览/应用事务、撤销重做、锁定与 tombstone、项目隔离 |
| `editor/ui/world.mjs` | 世界工作区：俯视画布、分块网格、区域笔刷、路径工具、图层显隐与锁定、任务与诊断面板 |
| `host/world_bridge.cpp/.hpp` | 清单加载、运行时调度、上传握手、fence 后释放 |
| `renderer/world_draw.cpp/.hpp`、`shaders/world.hlsl` | 分块地形网格、实例批次、共享光照与雾 |
| `tools/world.mjs`、`tools/build-world.ps1` | 生产 cook/info/verify/package 入口与 `build/world-host` 构建 |
| `host/world_cook_main.cpp` | 离线 cook worker（无 GPU、无 Node 依赖） |

## 2. 关键实现决定

1. **地形是纯函数。** `TerrainSurface::height(x, z)` 只依赖世界坐标与配置，不按块累积。相邻块在同一条边上采样同一个函数，接缝不可能出现差异。`native_world_tests` 用逐顶点比较固定这一点。
2. **散布候选定义在全局网格。** 最小间距用「优先级更高的邻居获胜」的确定性筛选，优先级是哈希，因此与块的生成顺序无关。测试以相反顺序生成相邻块并比较实例集合。
3. **笔迹按世界距离重采样。** `resampleStroke` 用米制间距，而不是指针事件数量；否则同样的手势在不同鼠标轮询率下会画出不同的世界。
4. **预览不落盘。** 预览把草稿图层写进 `build/world-cache/<taskId>/layers/`，应用时才写回 `.screen/world/layers/` 并更新 recipe。取消的笔迹不会留在磁盘上。
5. **revision 与 draftSeq 分离。** 预览只推进 `draftSeq`；`revision` 只在应用时 +1。这样预览的 `baseRevision` 在应用时仍然成立，冲突检查才有意义。
6. **cooked 清单携带组件调色板。** 发布包没有组件库文件，但运行时必须知道组件索引对应的形状与颜色，因此 `components` 随清单发布。
7. **世界与场景分离。** 世界没有实体、没有场景 revision；`world` 只是 scene 的一个可选引用字段，切换走现有事务边界并产生 undo 记录。

## 3. 明确未实现 / 已标注的能力缺口

- **建筑室内**：聚落只放置带占地、入口与碰撞的占位外壳。`world_capabilities` 返回 `buildingInteriors: "not-implemented"`，默认库中客栈组件带 `notes` 说明其仅为外壳。P6 的柱网、多层房间图、楼梯与家具尚未实现。
- **河流水动力**：河流有真实水面高度、连续河床与河岸，但未接入现有水体求解器；远景为静态水面。
- **密度区域只能抑制不能提升**：散布网格按作者密度定尺寸，一个格子最多一个候选，因此区域笔刷只能减少密度。超预算时编译器按比例缩减并在任务诊断里报告。
- **8 km 压力档**：契约与运行时支持，但未做长距离漫游实测。

## 4. 验证方式

```text
npm run test:world          # JS 契约，26 项
npm run test:world-native   # 契约 + 原生编译器/运行时，15 组 112 项
npm run world:verify -- --project <项目>   # cook + 原生渲染 + 像素证据
```

`world:verify` 检查的不是「代码路径存在」，而是画面：颜色种类、主导色占比、亮度标准差，加上原生 ACK 里的驻留块数、实例数与三角形数。空白画面会失败。

## 5. 已知取舍

- `tools/viewport-host.ps1` 原先缺少 `engine/native/water/coupling.cpp`、`transfer.cpp`、`renderer/water_ripple.cpp`，链接会失败；本次补齐（与 `tools/build.ps1` 的宿主链接集对齐）。
- 世界模式的宿主在 `windowsHide`（`CREATE_NO_WINDOW`）下不会推进帧，因为窗口收不到绘制消息。`world:verify` 因此显式允许创建窗口。
