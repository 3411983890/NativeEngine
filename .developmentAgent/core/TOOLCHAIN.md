# 工具链冻结表

- 冻结日期：2026-10-20
- 适用版本：0.3.1 起（工作包 1）
- 原则：构建只依赖仓库内 `.tools/` 中已校验的二进制；不读取系统 PATH 上的 DXC、Visual Studio 或 Windows SDK。

## 1. 版本表

| 组件 | 版本 / 标识 | 获取方式 | 校验 |
| --- | --- | --- | --- |
| 操作系统 | Windows 11 家庭中文版 10.0.26200 | 宿主机 | — |
| C++ 工具链 | LLVM-MinGW 20261006（clang 23.1.3，UCRT，x86_64-w64-mingw32） | `tools/bootstrap.ps1` 下载 | 归档 SHA-256 `317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a` |
| 着色器编译器 | DXC 1.9.2609（`dxcompiler.dll 1.9(5512-01b62ad4)(1.9.2609.5)`） | `tools/bootstrap-dxc.ps1` 下载 `dxc_2026_09_29.zip` | 归档 SHA-256 `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`；`dxcompiler.dll` / `dxil.dll` 的哈希写入 `.tools/dxc/1.9.2609/dxc.json` |
| Node.js | v24.18.0（要求 ≥ 22） | 宿主机 PATH | 报告记录 `node` 字段 |
| PowerShell | 5.1.26100.9444 | 系统 | 报告记录 `powershell` 字段 |
| Git | 2.56.0.windows.2 | 系统 | 报告记录 `commit` 与 `worktreeDirty` |
| Windows SDK | 不使用 | — | D3D12 头文件与导入库来自 LLVM-MinGW 自带副本（含 DXR / Mesh Shader 定义） |
| GPU / 驱动（本机证据机） | AMD Radeon(TM) 890M Graphics，驱动 32.0.21036.1002（2025-12-01） | — | 报告记录 `adapter.name` / `driverVersion` |
| Chromium | **待冻结**（S2 未开始） | 见第 4 节 | — |

`.tools/` 全部被 `.gitignore` 排除：仓库里只有下载地址与哈希，没有二进制。

## 2. 复现命令

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap.ps1      # C++ 工具链
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap-dxc.ps1  # DXC（固定 1.9.2609）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/compile-shaders.ps1 # 编译并内嵌着色器
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1           # 构建（含着色器）
```

`tools/bootstrap-dxc.ps1` 的行为：

- 已安装且 `dxc.json` 中的版本/哈希匹配时直接返回，不联网；
- 归档已存在但哈希不符时删除归档并报错，不会静默使用损坏文件；
- `-VerifyOnly` 只做检查，供 CI 在离线环境判断可用性；
- `-Force` 强制重新解包。

## 3. DXC 不可用时的契约

`tools/compile-shaders.ps1` 在找不到 DXC 时**不会**让构建失败，而是生成一份
`kDxcAvailable = false`、所有 blob 大小为 0 的 `build/generated/nativegpu_shaders.h`。
此后：

- 渲染器必须报告 `dxcUnavailable`，`probeAvailable` 仍可为 true（适配器支持 DXR），
  但 `effectImplemented`、`rtpsoCreated`、`sbtCreated` 必须为 false，`fallbackUsed` 必须为 true；
- `capabilities.implemented.raytracing` 保持 false；
- Mesh Shader 与 DXR 都不会退回 `D3DCompile` 生成的 SM 5.1 着色器来假装实现。

着色器源码编译失败（语法/类型错误）**会**让构建失败：那是代码错误，不是环境限制。
`tools/compile-shaders.ps1 -SelfTest` 会故意编译一个坏着色器，验证 DXC 真的拒绝它并保留诊断文本，
该检查已接入 `.incrementalTesting/core/verify.ps1` 与 `.incrementalTesting/core/ci.ps1`。

## 4. 版本与报告冻结

- `VERSION` 是唯一版本来源；`package.json` 的 `version` 必须与之一致，`tools/package.ps1` 会校验。
- 构建时把版本、commit、是否 dirty 写入 `build/generated/nativegpu_version.h`，原生报告里带
  `version` 与 `build.commit` / `build.dirty`。
- 证据目录为 `reports/<version>/<commit12>/<UTC时间戳>/`，外加 `reports/latest/` 别名；
  同一次输入重跑会产生新目录，不覆盖历史证据。`run.json` 记录环境、DXC、适配器、场景哈希与每个产物的 SHA-256。
  布局与失败日志见 [CI.md](CI.md)。

## 5. Chromium 冻结（S2 前置，尚未执行）

S2 开始前必须完成，且只允许在用户确认后进行：

1. 选定并记录 Chromium 版本与 commit（`git -C <chromium> rev-parse HEAD`），写入本文件第 1 节表格；
2. 记录 `depot_tools` 版本、`args.gn` 全文哈希、target_cpu 与 `is_component_build`；
3. 生成 `docs/CHROMIUM-FREEZE.md`，包含构建机器、构建耗时、产物哈希；
4. 冻结完成前不得把 Chromium 引入 `.incrementalTesting/core/ci.ps1`，否则 CI 会因缺少 30 GB 级依赖而不可重放。

当前状态：**未冻结**。第 1 节表格中的 Chromium 行必须在本项启动后由真实 commit 替换，不得预填。

## 6. 仓库本地配置

本项目仓库使用仓库级 Git 身份（无全局身份时也能提交）：

```text
user.name  = NativeGPU Dev
user.email = dev@nativegpu.local
core.autocrlf = false
```

这些值只影响提交署名，不参与构建。若接管仓库，请改成真实维护者身份并重打 tag。
