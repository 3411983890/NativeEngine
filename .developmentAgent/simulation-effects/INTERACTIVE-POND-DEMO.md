# 可交互水塘原型

项目位于 `Documents/nativeEngineProject/InteractivePond`，基于第三人称基础项目，复用原有机器人，未创建植被。项目场景为 `.screen/main.scene.json`，运行入口为 `tools/run-pond.cmd`；运行程序放在项目 `build/NativeGPU.exe`。

`host/pond_demo.hpp` 由项目内 `assets/water/pond-demo.enabled` 显式启用：CPU 浅水模型每游戏 tick 更新，角色入水和移动时在相邻网格间转移少量水深，水面使用 16×16 方块采样绘制。这是仅影响画面的原型，不向角色或刚体施力；没有浮力、透明折射、编辑器预览、正式水体组件、存档或打包支持。角色落水不会产生游泳或阻力。重置 tick 时重置视觉水体。

`assets/source/pond/pond-bed-imagegen.png` 和 `water-normal-imagegen.png` 由内置 imagegen 生成；前者为平铺的俯视砂砾池底颜色，后者为蓝紫色切线空间浅波纹法线。运行时贴图经项目内纹理烘焙生成；池底平法线、ORM 和水体基色、ORM 是常量辅助贴图。法线图是生成的美术近似，不是由实测高度场转换得到。

入口之外的引擎项目默认不启用此原型；编辑器预览不显示运行时水面。验证需单独检查水体模型扰动、项目场景和实际宿主画面，不应把视觉演示当成生产级双向水体。

本次验证：水资产 JavaScript 专项 4/4；原型原生检查覆盖入水、静止、移动扰动、域外查询和 256 个绘制单元；场景校验通过。项目独立宿主 240 帧运行成功，自动行走触发 18 次扰动，画面和报告位于项目 `reports/pond/`。启动脚本已验证可以启动该项目内的可执行程序。
