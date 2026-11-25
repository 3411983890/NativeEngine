# 工作区协作约定

- 测试按本次改动的依赖范围增量运行。用户明确要求：未经新的明确要求，不运行全量测试、全量回归、`npm test`、`npm run verify` 或完整 CI。
- 资产导入的增量入口为 `npm run test:assets`；需要验证原生绘制时使用 `npm run assets:verify`。此入口只构建宿主依赖并运行资产、动画、编辑器传输相关测试。
- 工作区包含其他开发中的改动；不要重置、覆盖或清理无关文件。原生资产验证使用独立的 `build/assets-host`，避免与其他专项共用构建产物。
- 开发说明、设计、计划与 AI 记忆统一放在 `.developmentAgent/`；测试与验证入口统一放在 `.incrementalTesting/`，不再向 `tools/` 根目录新增测试命令。
- 场景源文件统一放在 `.screen/`；`assets/` 只保留运行时资源、导入包和发布所需资源。

## 新增文件落位规则（所有 AI Agent 必须遵守）

创建任何文件前先判断类型，不得把说明、测试、临时产物或实验文件直接写到工程根目录。

### 1. Markdown 与开发说明

所有 AI 生成或开发过程用的说明、方案、调研、状态、报告、提示词、验收记录和 Agent 记忆都放入 `.developmentAgent/<分类>/`：

- `core/`：CI、构建、工具链、场景规范、通用契约
- `editor/`：编辑器、视口与 UI
- `rendering/`：渲染、光照、材质、纹理、后期、图像回归
- `assets/`：资产导入、格式与外部资源
- `animation-audio/`：动画、音频
- `simulation-effects/`：物理、场、烟雾、粒子、布料、水体、特效
- `vegetation/`：植被、毛发与植物编辑器
- `samples/`：样例工程说明
- `project/`：项目级 README 或入口说明
- `agent-memory/`：Agent 历史记忆

新增后同步更新 `.developmentAgent/README.md` 索引。仅 `AGENTS.md` 保留在项目根目录。

### 2. 测试与验证命令

所有测试、验证、截图、smoke、evidence、benchmark 和审计入口都放入 `.incrementalTesting/<分类>/`：

- `assets/`
- `editor/`
- `rendering/`
- `simulation/`
- `vegetation/`
- `core/`

不得在 `tools/` 根目录新增测试脚本。生产构建、运行、打包、导入和运行时依赖仍留在 `tools/`。新增或移动命令时必须同步更新：

- `package.json` 中的对应命令
- `.github/workflows/windows.yml` 或 `.incrementalTesting/core/ci.ps1`
- `.incrementalTesting/commands.json` 与 `.incrementalTesting/README.md`
- 脚本内的项目根目录计算、相对 import 和交叉调用

### 3. 场景与测试资产

- 可编辑场景、测试场景和演示场景统一放入 `.screen/`。
- `assets/` 只保留运行时资源、导入包、蓝图、纹理、音频和发布内容。
- 临时测试场景必须写入系统临时目录或 `build/` 下的专用目录，测试结束即清理，不得提交到 `assets/` 或根目录。
- 移动场景时必须同步其 `.history.json`、`.snapshot.json` 旁文件，并按需修复测试、工具、编辑器和宿主引用。

### 4. 生成物与临时文件

- 截图、报告、日志、缓存、实验输出和构建产物放入已有的 `reports/`、`build/`、`outputs/` 或专用临时目录。
- 不得在项目根目录创建临时 `.md`、脚本、截图、JSON 或测试夹具。
- 无法确定归属时，先放入对应隐藏分类目录，不要放在根目录。
- 引擎内创建的运行时项目必须位于 `%USERPROFILE%\Documents\nativeEngineProject\<项目名>`。项目场景、导入资产、报告、构建、保存和输出只能写入该项目目录，不得写回引擎源码目录。

### 5. 创建前检查

1. 这是生产代码、运行时资产还是发布输入？
2. 这是开发说明、测试命令、测试场景还是生成物？
3. 是否已有对应分类目录和索引？
4. 是否会影响 `package.json`、CI、编辑器、宿主或打包路径？
5. 完成后是否清理了真正的临时文件？

每次改动后运行与改动范围对应的增量测试；未经明确要求不得执行全量测试、`npm test`、`npm run verify` 或完整 CI。
