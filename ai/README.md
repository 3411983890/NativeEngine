# 确定性制作工具与 AI 工具面

版本 0.3.0。当前没有内置语言模型客户端，也不把自然语言解析伪装成 AI；本目录提供的是**模型无关、可独立调用**的工具契约。编辑器、脚本、CLI 和未来 Agent 都调用同一场景事务模块 `engine/scene/index.mjs`，不允许另开一条写场景的路径。

Node.js 22 或更高版本即可运行，不依赖第三方 npm 包。

## 命令约定

所有 CLI 输出 JSON。成功为 `{ "ok": true, "result": ... }`，失败为 `{ "ok": false, "error": { "code", "message", "details" } }` 并返回退出码 1。调用方应设置 30 秒超时；超时或失败不得推断操作已成功，必须重新查询 revision。工具不接受任意 shell 文本、外部 URL 或模型凭证。

| 工具 | 输入 | 结果 | 副作用 |
| --- | --- | --- | --- |
| `validate` | 场景文件路径 | 验证结果、项目 ID、revision、实体数 | 无 |
| `query` | 场景路径，可选 `--kind`、`--name`、`--id` | 匹配实体副本 | 无 |
| `preview` | 场景路径、事务 JSON 路径 | 候选场景、变更与 revision | 无 |
| `apply` | 场景路径、事务 JSON 路径 | 提交结果及新 revision | 原子替换场景、更新快照与撤销历史 |
| `undo` | 场景路径、`--project UUID --revision N` | 恢复场景及新 revision | 一步线性撤销 |
| `recover` | 场景路径 | 恢复结果 | 完成已提交的中断写入，或恢复最后有效快照 |
| `build` | 场景路径、输出目录 | 内容寻址包及 manifest | 在输出目录生成包 |

```powershell
node tools/scene.mjs validate assets/courtyard.scene.json
node tools/scene.mjs query assets/courtyard.scene.json --kind prop
node tools/scene.mjs preview assets/courtyard.scene.json ai/example-transaction.json
node tools/scene.mjs apply assets/courtyard.scene.json ai/example-transaction.json
node tools/scene.mjs build assets/courtyard.scene.json outputs/packages
node --test tests/*.test.mjs
```

`preview` 和 `apply` 的唯一事务格式：

```json
{
  "projectId": "a1e00000-0000-4000-8000-000000000001",
  "expectedRevision": 0,
  "commands": [
    {
      "op": "set",
      "entityId": "a1e00000-0000-4000-8000-000000000030",
      "changes": { "position": [-4, 0.6, 1] }
    }
  ]
}
```

命令支持 `create {entity}`、`set {entityId, changes}` 和 `remove {entityId}`。创建必须提供完整实体；UUID 由调用者生成，重放时保持不变。`set` 不能修改 UUID。批内命令依次应用到副本，全部验证通过后才提交一次，revision 只增加一次。项目不匹配、旧 revision、未知字段、重复 UUID、非有限数值及无效维度均拒绝。整批最终必须恰有一个玩家出生点、至少一个地面。当前只支持方盒几何、无旋转的碰撞体；位置单位为米，Y 向上。普通实体位置为盒体中心，玩家位置为脚底。

字段与范围由 `assets/scene.schema.json` 正式声明，运行时另验证 UUID 唯一、恰有一个玩家、至少一个地面和跑速不低于走速：

- 场景名称、实体名称：1–128 个字符，不含控制字符。
- 最多 10000 个实体、1000 条命令；所有文件读取上限 16 MiB。
- 位置每轴 ±100000；尺寸每轴 0.001–10000；颜色通道 0–1。
- `walkSpeed` 0.1–30、`runSpeed` 0.1–60、`jumpSpeed` 0.1–30、`gravity` 0.1–100；`runSpeed >= walkSpeed`。
- 实体类型：`ground`、`wall`、`prop`、`player`、`key`、`door`、`goal`、`light`。
- 可选 `material`：`albedo` RGB、`roughness` 0–1、`metalness` 0–1、`pattern` 为 `checker`/`brick`/`metal`。
- `light` 实体必须有 `light` 对象：`color` RGB、`intensity` (0,100]、`range` (0,1000]；最多 4 盏灯。
- 材质表是 3 个固定纹理槽；这不是完整的 Bindless 资源堆。

## 保存、撤销与恢复

`saveScene()` 仅新建项目；已有项目必须通过 `commitScene()` 修改。写入使用同目录临时文件、flush 与替换。场景替换是提交点。`.pending.json` 记录中断事务；下一次提交会补齐已提交的历史，打开时若发现中断则要求显式恢复。`.snapshot.json` 保留上一次有效场景，`.history.json` 最多保留 100 次撤销。撤销恢复内容但 revision 继续增加，避免旧请求再次生效。`.lock` 阻止同一文件的并行写入；崩溃遗留锁只应在确认对应进程停止后手动移除。读取者不能把锁文件当作可靠的系统进程存活证明。

损坏场景不会静默重置。`recover` 在无法完成中断提交时恢复快照并分配更大的 revision。恢复存在快照时会修改项目，因此调用者应仅在有明确恢复意图时使用。不要手工编辑已有历史的场景；需要外部导入时复制成新项目路径。

原生编辑器默认每 400 ms 检查 `assets/*.scene.json` 的大小与写入时间。成功加载后采用新 revision 并重置玩法；加载失败或被拒绝的项目 ID 变更会保留上一有效场景，错误写入冒烟报告。`F9` 可开关热重载，`--no-watch` 可关闭。当前不迁移运行中的玩家状态。

## 构建契约

构建仅导出经过验证的场景和 `manifest.json`。没有目录递归打包、DCC 源文件、开发缓存或凭证扫描；未知场景字段直接拒绝。构建键由场景内容 SHA-256、工具版本和目标组成，不包含时间或机器路径。同输入得到同包。构建包可作为运行时资产目录；完整可执行发布包由 `tools/package.ps1` 生成。

## MCP / AI 工具面

`ai/mcp-server.mjs` 通过 stdio 提供本地 MCP 服务，不监听网络端口，不需要模型凭证：

| 工具 | 作用 |
| --- | --- |
| `scene_validate` | 只读验证，返回结构化错误 |
| `scene_query` | 按 kind/name/id 查询实体，只读 |
| `scene_preview` | 在副本上应用事务，返回 diff，不写入 |
| `scene_apply` | 原子提交，运行中的编辑器自动热重载 |
| `scene_undo` | 撤销最后一步，revision 继续增加 |
| `scene_build` | 在 `outputs/` 下生成内容寻址包 |
| `runtime_smoke` | 启动 `build/NativeGPU.exe --smoke`，返回 JSON 报告 |
| `engine_probe` | 启动 `--probe`，返回材质表、场景灯光、DXR 加速结构和 Mesh Shader 证据 |

路径策略：场景只能位于 `assets/`，构建输出只能位于 `outputs/`，冒烟报告只能位于 `reports/`；拒绝 `..`、根目录外绝对路径和符号链接逃逸。`runtime_smoke` 只接受整数帧数，固定 180 秒上限，不接受 shell 文本。

MCP 客户端注册示例（格式随客户端版本可能变化）：

```toml
[mcp_servers.nativegpu]
command = "node"
args = ["ai/mcp-server.mjs"]
cwd = "C:\\Users\\34119\\Documents\\AI-Native Game Engine"
```

也可以用 stdin 直接测试：

```powershell
'{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | node ai/mcp-server.mjs
```

工具的 `ok:false` 只表示契约内失败，不是传输失败；调用方仍应把 `error.code` 作为主要判断依据。`scene_apply` 成功后必须用新 revision 继续，不能用旧事务重试。

## 测试与验证

```powershell
npm test                         # 13 项 Node 测试
npm run test:native              # 构建并运行原生测试
npm run probe                    # S1 探针，写 reports/probe.json
npm run verify                   # 全链路：Node + 原生 + 冒烟 + 探针 + 热重载 + MCP
npm run package                  # 生成 outputs/NativeGPU-0.3.0-win64{,.zip}
```

`tools/verify.ps1` 的输出写入 `reports/verify.json`；原生报告写入 `reports/verify-native-smoke.json`。热重载测试在临时目录中提交真实事务，并要求运行时报告 `hotReloads >= 1`、`revision >= 1`。

常见错误：`INVALID_SCENE`、`INVALID_TRANSACTION`、`INVALID_COMMAND`、`ENTITY_NOT_FOUND`、`DUPLICATE_ENTITY`、`PROJECT_MISMATCH`、`REVISION_CONFLICT`、`REVISION_EXHAUSTED`、`PROJECT_BUSY`、`HISTORY_CONFLICT`、`RECOVERY_REQUIRED`、`NO_SNAPSHOT`、`NOTHING_TO_UNDO`、`BUILD_CONFLICT`、`FILE_TOO_LARGE`、`INVALID_JSON`、`ALREADY_EXISTS`。MCP 工具还会返回 `INVALID_PATH`、`INVALID_ARGUMENTS`、`FILE_NOT_FOUND`、`NATIVE_BINARY_MISSING`、`NATIVE_TIMEOUT`、`NATIVE_REPORT_MISSING`。

## 模型接入待办

当前只是受信本地工具，不是网络服务或安全沙箱。真正接入模型前必须补：项目目录限制、用户确认预览、可取消执行、调用额度、超时、审计日志、模型响应 JSON schema、失败恢复与费用记录。MCP 只解决“工具可被发现和调用”，不自动解决模型输出质量、授权或成本问题。
