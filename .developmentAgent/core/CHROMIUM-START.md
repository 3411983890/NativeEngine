# 第 5 项 Chromium（S2）启动清单

- 状态：**定制 Chromium 路线暂缓，本文保留为历史参考**。2026-10-20 用户明确选择“Web 编辑器 + 原生引擎”；当前先使用成熟 Electron 宿主，独立原生预览窗口，后续单独验证共享纹理。现行启动与架构说明见 [EDITOR.md](EDITOR.md)。
- 下列源码构建、Mojo 与 S2 顺序仅适用于以后重新选择定制 Chromium 的情况，不是当前 Web 编辑器的前置条件，也不应阻止界面开发。当前没有编译或修改 Chromium 源码，G2 共享纹理关口尚未通过。
- 依据：`docs/NEXT-PHASE.md` 第 5 节；G1 证据见 `reports/g1.json`。

## 0. 前置条件（必须全部满足）

- [ ] `.incrementalTesting/core/g1.ps1` 输出 `G1 PASSED`，且 `reports/g1.json` 的 `worktreeDirty=false`。
- [ ] G1 证据四件套齐全：`rt-shadows-evidence.json`、`mesh-bindless-evidence.json`、
      `g1-reliability.json`、`verify.json`，均在同一 commit 下生成。
- [ ] 用户明确同意开始 S2。
- [ ] 仓库有干净的工作树与 `v0.6.0` tag；之后 S2 的每一步单独提交。

## 1. 冻结（第一步只做这个）

- [ ] 选定 Chromium 版本与 commit，写入 `docs/TOOLCHAIN.md` 第 1 节表格（替换“待冻结”）。
- [ ] 记录 `depot_tools` 版本、`args.gn` 全文 SHA-256、`target_cpu`、`is_component_build`、`is_debug`。
- [ ] 新建 `docs/CHROMIUM-FREEZE.md`：构建机器、构建耗时、`chrome.exe` 与 `content_shell.exe` 哈希。
- [ ] 冻结前不要把 Chromium 引入 `.incrementalTesting/core/ci.ps1`（缺少 30 GB 级依赖时 CI 不可重放）。

## 2. 最小宿主

- [ ] 最小补丁集：只加一个 `content/browser` 侧的 embedder 入口与原生窗口宿主，不改渲染栈。
- [ ] 定义 `NativeGpuSession` Mojo 接口：
  - `CreateSession(handle) -> (session_token, shared_texture_handle, size, format)`
  - `Resize(width, height) -> (shared_texture_handle, size)`
  - `FrameReady(fence_handle, frame_index)`（单向上报，不带 CPU 像素）
  - `Stop()` / `OnSessionLost(reason)`
- [ ] Browser Host 权限代理：只有受信本地页面（`chrome://nativegpu` 或打包的本地 origin）能创建会话；
      普通页面调用必须被拒绝并记录审计日志。
- [ ] 原生核心迁入隔离进程：启动、停止、崩溃重连各有一个可复现命令与证据。

## 3. 共享纹理（先静态，再三缓冲）

- [ ] 先传一张静态共享纹理（`D3D11Texture2D` / `DXGI` 共享句柄 → Chromium 侧 compositor 采样）。
- [ ] 再接入三缓冲 + Fence：每帧 `FrameReady` 携带 fence，Chromium 在合成前等待。
- [ ] 记录每帧复制次数、同步等待时长、合成耗时；**禁止**用 CPU 回读或截图刷新冒充实时合成。
- [ ] 报告字段：`copiesPerFrame`、`waitMs`、`compositeMs`、`droppedFrames`、`sessionState`。

## 4. S2 第一个验收（不是“能显示”）

- [ ] 受信本地页面可稳定显示连续渲染视口 ≥ 60 秒，无撕裂、无逐帧 CPU 回读。
- [ ] 普通页面调用 `NativeGpuSession` 被拒绝（有错误码与审计记录）。
- [ ] 强制停止原生服务后，宿主能重建会话（有恢复证据与耗时）。
- [ ] 失败时保留上一有效画面，不得黑屏或崩溃。

## 5. 明确不做（直到 S2 验收通过）

- 不做编辑器 UI、资产导入、动画、音频、脚本、模型接入（属于 S3–S5）。
- 不为了让画面“看起来对”而在 Chromium 侧重写渲染逻辑。
- 不在 CI 里强制构建 Chromium；S2 的 CI 只跑宿主小工程与接口契约测试。

## 6. 启动第一条命令（确认后执行）

```powershell
# 1. 确认 G1
powershell -NoProfile -ExecutionPolicy Bypass -File .incrementalTesting/core/g1.ps1
# 2. 冻结 Chromium（示例，实际 commit 以当日稳定分支为准并写入 TOOLCHAIN.md）
#    git clone --filter=blob:none https://chromium.googlesource.com/chromium/src
#    git -C src checkout <frozen-commit>
# 3. 只做最小宿主，不改渲染栈
```
