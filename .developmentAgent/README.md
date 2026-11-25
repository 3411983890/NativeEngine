# 开发文档索引

- [引擎功能之旅独立演示项目](samples/ENGINE-FEATURE-TOUR.md)

- [AI 原生游戏引擎创业计划宣讲 PPT 策划案](project/AI-NATIVE-ENGINE-PITCH-PLAN.md)

- [AI 地图编辑与大场景程序化生成：原生功能开发文档](core/AI-WORLD-GENERATION-DEVELOPMENT-PLAN.md)

- [大场景世界生成：实现记录（2026-10-28）](core/AI-WORLD-GENERATION-IMPLEMENTATION.md)

- [真实水体详细开发方案与 DeepSeek 提示词（2026-10-28）](simulation-effects/WATER-REALISM-DEEPSEEK-PROMPT-2026-10-28.md)

- [水体波纹修复与流动、落差、绕石演进方案（2026-10-28）](simulation-effects/WATER-REALISM-2026-10-28.md)

- [真实水体 M0–M7 审计与实现记录（2026-10-28）](simulation-effects/WATER-REALISM-MILESTONES-2026-10-28.md)

- [多层材质与物理层叠开发方案](rendering/LAYERED-MATERIAL-DEVELOPMENT-PLAN.md)
- [多层 PBR 验收与石板湿泥预览（2026-10-27）](rendering/STONE-MUD-ACCEPTANCE-2026-10-27.md)
- [地面高度表现与真实置换开发方案（2026-10-28）](rendering/SURFACE-RELIEF-DEVELOPMENT-PLAN-2026-10-28.md)
- [物理层叠参考闭包研究](rendering/PHYSICAL-LAYERS-RESEARCH.md)

- [编辑器视角、场景编辑与运行交互开发计划（2026-10-27）](editor/EDITOR-INTERACTION-DEVELOPMENT-PLAN.md)

- [第三人称基础项目与默认角色/材质](editor/STARTER-PROJECT.md)

- [黑色角色四视图与实际分辨率](assets/character-orthographic-views-2026-10-27.md)

本目录集中保存开发过程中的说明、设计、计划、样例文档与 AI 记忆。项目源码入口保留 AGENTS.md。

## 动画与音频

- [CDM 高度场接地与坡面跳跃修复（2026-10-28）](animation-audio/CDM-HEIGHTFIELD-FIX-2026-10-28.md)
- [CDM 内置角色测试场景与操作说明](animation-audio/CDM-MOTION-LAB.md)
- [AI 模块化角色头部绑定与 iPhone 16 Pro Max 面捕开发方案](animation-audio/AI-MODULAR-HEAD-RIG-AND-IPHONE-MOCAP-PLAN.md)
- [接触驱动动作系统：动态支撑与动作参考库开发方案](animation-audio/CONTACT-DRIVEN-MOTION-DEVELOPMENT.md)
- [接触驱动动作系统实现（P1 单支撑 · P2 双支撑与转向）](animation-audio/CONTACT-DRIVEN-MOTION-IMPLEMENTATION.md)
- [接触驱动动作系统 · 支撑脚滑动收敛（P1.1）](animation-audio/CONTACT-DRIVEN-MOTION-STANCE-BUDGET.md)
- [接触驱动动作系统 · 多接触层（阶段四：手臂链与滑移接触）](animation-audio/CONTACT-DRIVEN-MOTION-MULTI-CONTACT.md)
- [《动态支撑点与多接触约束》开发完成度验收](animation-audio/CONTACT-DRIVEN-MOTION-TXT-ACCEPTANCE.md)
- [图片驱动发型生成器与角色服装权重系统开发方案](animation-audio/AI-HAIR-GENERATOR-AND-GARMENT-WEIGHTS-PLAN.md)
- [原生蒙皮布料（阶段 E 第一增量）](animation-audio/GARMENT-SKINNED-CLOTH-NATIVE.md)
- [发型生成器 AI 使用手册](animation-audio/AI-HAIR-GENERATOR-GUIDE.md)
- [成衣刷权重与布料 AI 使用手册](animation-audio/AI-GARMENT-WEIGHTS-GUIDE.md)
- [通用实时角色 Rig 与脚步控制开发方案](animation-audio/REALTIME-CHARACTER-RIG-DEVELOPMENT.md)
- [实时角色 Rig 首版实现（阶段 A–D）](animation-audio/REALTIME-CHARACTER-RIG-IMPLEMENTATION.md)
- ANIMATION-M1-M4.md
- ANIMATION-PLAN.md
- ANIMATION.md
- AUDIO-PLAN.md

## 核心架构与流程

- BUDGET.md
- CAPTURE.md
- CHROMIUM-START.md
- CI.md
- COMPUTE.md
- DETERMINISM.md
- DEV-TASKS.md
- ERROR-CONTRACTS.md
- IDEA-TRIAGE.md
- NEXT-PHASE.md
- OUTPUT-CONTRACT.md
- PERF-BASELINE.md
- REPLAY.md
- SCENE-LINT.md
- SCENE-QUERY.md
- STATUS.md
- TOOLCHAIN.md
- WATER-REFERENCE.md

## 渲染

- CLOTH-BATCH.md
- CLOTH-GPU.md
- CLOTH-GROUPED.md
- CLOTH-REFERENCE.md
- CLOTH-RENDERING.md
- GEOMETRY-LAYOUT.md
- GEOMETRY-PACKAGE.md
- IMAGE-REGRESSION.md
- LIGHTING-QUALITY.md
- LIGHTING-SYSTEM-RESEARCH.md
- LIGHTING-VS-MEGALIGHTS.md
- MATERIAL-FIXES-2026-10-23.md
- MATERIAL-PIPELINE-RESEARCH.md
- MATERIAL-PIPELINE.md
- MATERIAL-ROADMAP.md
- MESH-BINDLESS.md
- NANITE-LUMEN-PLAN.md（Weave 簇织 / Xihe 曦和 命名与方案）
- RHI.md
- RT-SHADOWS.md
- SDF-BAKE.md
- TEXTURE-MATERIAL-PLAN.md
- VOLUMETRICS-PLAN.md

## 植被

- [青岚村 PBR 生成状态与接续入口](vegetation/QINGLAN-PBR-GENERATION-STATUS.md)
- [单叶生成与调用基本规范 TXT](vegetation/VEGETATION-LEAF-GENERATION-STANDARD-v1.txt)
- [其他植被贴图基本规范 TXT](vegetation/VEGETATION-OTHER-TEXTURE-STANDARDS-v1.txt)
- [植被系统扩展开发文档：全类型植物与 AI 地图笔刷集成（2026-10-28）](vegetation/VEGETATION-SYSTEM-EXTENSION-DEVELOPMENT-2026-10-28.md)
- [植被系统扩展实施记录：部件层、排布规则与笔刷合同（2026-10-28）](vegetation/VEGETATION-EXTENSION-IMPLEMENTATION-2026-10-28.md)
- [青岚村植被贴图与模型需求表：63 项清单（2026-10-28）](vegetation/VEGETATION-ASSET-REQUIREMENTS-2026-10-28.md)
- DEEPSEEK-VEGETATION-PRODUCTION-PROMPT.md
- DEEPSEEK-VEGETATION-REALISM-SPEEDTREE-PROMPT.md
- VEGETATION-AI-TEXTURE-AND-DAMAGE-SPEC.md
- VEGETATION-DEFECTS-AND-PLAN.md
- VEGETATION-EDITOR.md
- VEGETATION-HAIR-DEVELOPMENT-PLAN.md
- VEGETATION-HAIR-STATUS.md
- VEGETATION-PLAN.md

## 编辑器

- EDITOR-FEATURES.md
- EDITOR-VIEWPORT.md
- EDITOR.md

## 模拟、特效与物理

- EFFECT-DEPOSIT-ADMISSION.md
- EFFECT-DEPOSIT-SERVICE.md
- EFFECT-FIELD-CONSUMER.md
- EFFECT-FIELD-DEPOSITS.md
- EFFECT-GPU-REFERENCE.md
- EFFECT-GPU-RENDERING.md
- FIELD-DEPOSIT-PHASES.md
- FIELD-EFFECT-COORDINATOR.md
- FIELD-HOST.md
- FIELD-JOINT-PUBLICATION.md
- FIELD-LOCAL-GPU.md
- FIELD-PLAN.md
- FIELD-PUBLICATION-SCHEDULER.md
- FIELD-PUBLICATION-TRANSACTIONS.md
- FIELD-REALISM-GOAL.md
- FIELD-REFERENCE.md
- FIELD-SERVICE-JOINT-PUBLICATION.md
- FIELD-SHARING-GPU.md
- FIELD-WORLD-GPU.md
- GAMEPLAY-DEVELOPMENT.md
- GAMEPLAY.md
- INTERACTIVE-POND-DEMO.md
- WATER-REALISM-SYSTEM-PLAN.md
- WATER-REALISM-DEVELOPMENT.md
- WATER-REALISM-SUMMARY.md
- WATER-REALISM-MILESTONES-2026-10-28.md
- NIGHT-TOWN.md
- PARTICLES-FIELDS-CLOTH-WATER-PLAN.md
- PHYSICS-DESTRUCTION.md
- PHYSICS-SCALE-PLAN.md
- SIMULATION-DEVELOPMENT.md
- SIMULATION-FIELDS-HOST.md
- SMOKE-EFFECT-SERVICE.md
- SMOKE-FIELD-PROJECTION.md
- SMOKE-FIRE-SOLVER.md
- SMOKE-PARTICLE-SOURCES.md
- SMOKE-RENDERING.md
- SMOKE-SHARING.md

## 资产与材质

- EXTERNAL-ASSETS.md

## 项目入口说明

- README.md
- README.md
- ai-readme.md
- root-readme.md
- shaders-readme.md

## 样例文档

- README.md
- authoring-guide.md
- feature-coverage.md
- known-limitations.md
- walkthrough.md

## AI 开发记忆

- 2026-10-21.md
- 2026-10-22.md
- 2026-10-23.md
- 2026-10-25.md
- 2026-10-26.md
- 2026-10-26.md

完整迁移来源见 `moved-files.json`。
