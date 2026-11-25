# 原生三维编辑视口接入方案

- 日期：2026-10-21。
- 用户要求：在 Web 编辑器内部的真实三维场景中选中、移动物体，并作为动画、材质等内容制作的共同基础。
- 优先级：**P0，首版编辑器必需**。二维顶视图与静态快照保留为辅助，不能代替该工作流。
- 状态：**C 路径（原生子窗口）已实现并验证**，见 §9.6；共享纹理（B 路径）仍未实现。**2026-10-26 复核**：宿主 Electron 44.9.1 已安装；全仓共享纹理代码 0 行；`Renderer` 只接受 HWND、无离屏模式；见 §9。
- 宿主：优先当前锁定的 Electron 44.9.1；不要求修改或编译 Chromium 源码。

## 1. 目标体验

1. 打开编辑器后，中央视口持续显示当前原生引擎的场景。
2. 环绕、平移或飞行相机，点击物体，显示选中高亮与变换操纵柄。
3. 拖动 X/Y/Z 轴或平面手柄，物体即时移动，属性面板同步数值。
4. 松开鼠标得到一步可撤销的编辑；Esc 或捕获丢失可取消本次拖动。
5. 保存时进入现有版本受控事务；关闭前可恢复草稿。
6. 切换到动画或材质工作区时复用相同的原生渲染能力，保留对应相机、时间和预览状态。

## 2. 两条通道

### 图像通道：原生 GPU 到 Electron

```text
原生引擎：场景、动画、材质、灯光
  → D3D12 离屏最终画面
  → 可共享 GPU 纹理 + 就绪/归还同步
  → Electron 主进程导入与转交
  → 页面中的 VideoFrame / GPU 绘制表面
  → 编辑器中央视口
```

引擎仍然计算场景并渲染像素。页面负责布局、面板和输入；传递的是 GPU 资源及元数据，避免每帧截图编码、CPU 回读和重新上传。

此处首先指 Electron 承载的页面。普通 Chrome/Edge 网页没有 Electron 的共享纹理 API，不能直接接收操作系统纹理句柄。浏览器独立版本若需要相同画面，要另外实现视频流等传输，再评估延迟和画质；不能将其视为同一条已支持的路径。

### 控制通道：页面到原生编辑会话

```text
鼠标 / 键盘 / 属性面板 / 动画时间轴
  → 受控 Electron 桥接
  → 原生编辑会话与预览状态
  → 相机 / 拾取 / 临时变换 / 动画时间 / 材质参数
  → 下一帧原生画面与编辑状态回报
```

高频控制使用持续会话传输，例如受控 IPC + pipe。当前 JSON HTTP 场景服务继续承担查询、校验、保存、撤销等低频操作；不把每次 pointermove 都转换成场景写盘。

## 3. Electron 接入的具体边界

Electron 的 `sharedTexture.importSharedTexture` 可将外部共享纹理导入为可转交的资源，在页面侧取得 `VideoFrame`。当前官方 API 标为实验性，必须用锁定版本做最小互操作实验。

Windows 下应逐项验证：

1. D3D12 画面先进入可供 Chromium 导入的 DXGI/D3D11 共享资源；D3D12 与 D3D11/ANGLE 的兼容资源、拷贝和同步路径需要实测，不能假设任意 D3D12 资源句柄都能导入。
2. 使用 NT HANDLE；句柄需要复制到调用导入 API 的进程。仅把原进程里的整数值传给 JS 不成立。
3. 生产完成与消费完成是两回事。原生 GPU 完成写入后才发布帧；页面释放和 GPU 使用结束后才允许复用缓冲。
4. 按官方释放/回调规则管理 `VideoFrame` 和导入对象；不能以“IPC 发送成功”或“绘图函数返回”判断纹理已可复用。
5. 优先双/三缓冲与有界队列；消费慢时丢旧帧，不积压输入和画面。
6. resize、DPI、最小化、设备丢失使用资源代次更新；旧句柄不能继续用于新尺寸。
7. 保持同一适配器优先，先验证本机 D3D12 与 Chromium GPU 设备互操作；混合显卡不能默认能够无额外代价共享。

初步实施可允许额外 GPU copy，并如实记录次数。目标是避免逐帧 CPU 像素往返；未测量前不宣称“零拷贝”或“零延迟”。生产同步若需等待，应在原生工作线程/会话层完成，避免阻塞 Electron UI 主线程。

## 4. 接通画面后还必须做什么

| 能力 | 方案 |
| --- | --- |
| 编辑相机 | 独立于玩家相机，支持环绕、平移、缩放、聚焦；控制不推进游戏角色 |
| 对象拾取 | 首版用屏幕射线与现有方盒/AABB 相交；复杂模型后续增加精确几何或 GPU Object ID |
| 稳定选择 | 原生返回实体 UUID；场景列表、属性与视口共用该选择，不能依赖临时 draw 下标 |
| 操纵柄 | 在原生画面中绘制并命中测试 X/Y/Z 轴与平面；用三维约束计算位移，拖动沿初始状态求值 |
| 深度与遮挡 | 场景原生拾取负责深度关系；选择高亮与手柄的遮挡规则明确，不能把二维矩形命中直接当三维选择 |
| 输入坐标 | 传递视口局部坐标、物理尺寸、DPI/缩放与资源代次；相机矩阵和命中计算保持一致 |
| 图像与输入一致 | 拾取携带所见 frameId/cameraVersion；发生 resize 或视图变化时重新校验，避免点到旧画面对应的位置 |
| 输入焦点 | 只有视口获得焦点时发送导航/玩法输入；文本框输入不驱动角色，离焦清空按键和拖动状态 |
| 生命周期 | 资源丢失时保留编辑草稿、展示状态并重建会话；重连使用最新保存场景和仍有效的本地预览状态 |

旋转不能只增加一个可见手柄：当前实体持久化模型主要是 position/scale，必须先补旋转字段、矩阵/碰撞语义、事务和保存兼容。首个操作闭环先做选中、移动和缩放。

## 5. 实时拖动与保存分开

三种状态必须明确：

- **已保存场景**：带 revision 的权威项目数据。
- **编辑草稿/预览覆盖**：正在拖动或输入的临时改动，可丢弃，不直接写项目。
- **运行时状态**：角色位置、动画时间、粒子等模拟结果，不自动写回场景。

一次拖动的建议生命周期：

```text
BeginEdit(entityId, baseRevision, initialTransform)
  → UpdatePreview(sessionId, seq, candidateTransform) 多次
  → EndEdit：记录一个本地撤销步骤
  → Save：同一场景事务校验、提交一次

CancelEdit / Esc / 丢失捕获
  → 清除本次临时覆盖，恢复拖动前的变换
```

以上接口名称为方案，不是已存在的 API。预览也必须检查有限数值、边界和版本；更新带递增序号，重连或晚到消息不能覆盖新编辑。

保存发生外部 revision 冲突时保留草稿，先刷新和检查影响，再决定重新应用。不能在后端拒绝后继续把未保存画面显示成“已保存”。

现有宿主 `pollScene → adoptScene → Game::initialize` 会重新初始化玩法。拖动、动画拖帧和材质调参需要独立的预览更新路径；将文件轮询加快不能解决这个问题。

## 6. 动画与材质如何共用

| 预览模式 | 需要控制的状态 | 仍然共用的能力 |
| --- | --- | --- |
| 场景 | 编辑相机、对象选择、临时变换、辅助显示 | D3D12 渲染器、资源加载、GPU 输出与输入会话 |
| 动画 | 角色、片段、播放时间、暂停/拖帧、骨架显示、状态/权重 | 相同的骨骼采样、蒙皮与材质路径；预览模式控制时间，不强制启动完整玩法 |
| 材质 | 预览网格、灯光、材质实例、参数草稿、固定机位 | 与实际运行使用相同的材质/纹理和光照求值 |
| 效果 | 效果资源、seed、播放/重置、局部参数、调试切片 | 相同渲染与资源生命周期，按已实现能力开放 |

优先让一个视口切换预览模式，接口预留 session/viewport 身份；不要求首版同时运行多套视口。材质热更新也需要可靠资源发布/回收，而不是每次重新启动游戏。

## 7. 当前代码要改的边界

| 位置 | 当前情况 | 所需变更 |
| --- | --- | --- |
| `renderer/d3d12_renderer.*` | HWND/交换链呈现，普通离屏 composite 后复制到交换链 | 分开“渲染到目标”和“窗口呈现”，增加共享输出模式及生命周期 |
| `renderer/rhi.*` | initialize/resize/submit 以窗口与逐帧提交为主 | 增加输出目标/帧资源契约，保留原生独立窗口路径 |
| `host/main.cpp` | Win32 输入、玩家相机、玩法循环、文件热重载耦合 | 抽出原生编辑会话、编辑相机、预览状态和控制入口 |
| 新增 native editor session | 目前不存在 | 命中测试、选择、操纵柄、临时覆盖、预览时钟、会话协议 |
| `editor/main.mjs` 与桥接 | 当前仅加载本地网页和 HTTP 服务 | 受控导入共享纹理、转交页面、状态/输入 IPC 与来源检查 |
| `editor/ui/` | 二维 canvas 与静态快照 | 原生帧绘制表面、视口输入、选择/属性双向同步；二维视图成为辅助 |
| `engine/scene/` | 版本受控场景事务 | 继续作为保存/撤销权威；新持久化字段显式扩展，临时预览不另设无校验写盘路径 |

## 8. 实施顺序与完成标准

1. **静态 GPU 资源互操作**：Electron 显示原生产生的测试纹理，验证句柄归属、格式和释放；使用锁定版本。
2. **连续真实场景**：原生循环直接输出到网页视口；覆盖 resize、DPI、最小化/恢复和进程异常。记录复制、等待、帧率、队列与内存。
3. **编辑相机与选择**：在网页视口中绕场景观察，点击物体，实体列表和属性正确同步。
4. **移动闭环**：拖动三维轴/平面即时更新原生画面，松开为一步撤销，Esc 恢复；保存重开一致，外部冲突不丢草稿。
5. **专业预览**：先接已有动画片段的播放/暂停/拖帧，再接材质参数与资源预览；复用同一原生 GPU 输出。

首版视觉验收必须包含：**直接在编辑器三维画面中选中并移动物体**。只做到“网页收到一张图”或“能启动原生窗口”不算完成。

## 9. 2026-10-26 复核：能不能把引擎内嵌到视图里

结论：**能，三条路，代价差一个数量级。但三条路有同一个前置条件——`VegetationDraw` 目前没有接入 `Renderer`**（`grep -rln "VegetationDraw" renderer engine` 只命中它自己两个文件），所以无论先做哪条传输，内嵌进来的画面里都还没有植被。先接入，再谈传。

### 9.1 实测到的现状（不是推断）

| 事实 | 证据 |
| --- | --- |
| Electron 44.9.1 已安装，`editor/main.mjs` 已在 Electron 里加载本地页面 | `node_modules/electron/package.json` 版本 = 44.9.1 |
| 共享纹理链路**一行都没有** | 全仓 `CreateSharedHandle` / `D3D12_HEAP_FLAG_SHARED` / `importSharedTexture` / `sharedTexture` 0 命中 |
| `Renderer` 没有离屏模式，只能渲染到窗口 | `Renderer::initialize(HWND window, …)`；`swapChain->Present(1, 0)` |
| 已有阻塞式读回 | `requestScreenshot()` 建 READBACK 缓冲，`captureScreenshot()` `waitIdle` → Map → 写 PNG |
| 编辑器服务没有 WebSocket | `editor/server.mjs` 只有 HTTP 与 `/api/call`，无 `upgrade` 处理 |

### 9.2 三条路

| 路径 | 工作量 | 延迟 | 能支撑的操作 | 主要风险 |
| --- | --- | --- | --- | --- |
| **A 读回流**（截图 → WebSocket → canvas） | 1–2 天 | 60–150 ms | 看形状、看外观、引擎侧离线对照 | `waitIdle` 每帧阻塞；PNG 编码；**不能拖操纵柄** |
| **B 共享纹理**（Electron `sharedTexture`） | 3–6 天 | < 10 ms | 交互、操纵柄、拾取 | API 仍标实验性；D3D12 资源需与 Chromium（D3D11/ANGLE）兼容，必须实测；NT 句柄跨进程复制；生产/消费同步与回收 |
| **C 原生子窗口**（`SetParent` + swapchain） | 2–3 天 | ≈ 0 | 交互 | 层级、DPI、裁剪、焦点、遮挡全要自己同步；沙箱与导航策略下更脆 |

推荐顺序：**A 先做**（用一天拿到"引擎画面确实能出现在视图里"这个事实，且不依赖 Electron 特有 API，纯浏览器 `npm run editor:web` 也能用）；**B 作为正式路径**（只有它支撑操纵柄级别的交互）；**C 不推荐**，除非以后要整体换成原生 UI。

### 9.3 前置顺序（最容易被跳过的一步）

1. `VegetationDraw` 接入帧图（见 [VEGETATION-DEFECTS-AND-PLAN.md](VEGETATION-DEFECTS-AND-PLAN.md) A1）。
2. 把 `Renderer` 拆成"渲染到目标"与"呈现到窗口"，增加离屏输出模式——B 必需，A 也需要能不给 HWND（否则无法在后台渲染）。
3. 做 A 或 B。
4. 输入回传：相机、拾取、操纵柄，并携带 `frameId` 与 `cameraVersion`，避免点到旧画面对应的位置。

### 9.4 A 的最小实现（可直接落地）

- `requestScreenshot()` 的写盘换成"写内存并返回指针/长度"；编码从 PNG 换成 JPEG（`sharp` 已在依赖里），或直接推 BGRA 由页面 `createImageBitmap` 处理。
- 原生侧把 `Present(1, 0)` 降为 `Present(0, …)` 或改手动节拍；流送分辨率降到 960×540（约 2 MB/帧 BGRA）。
- `editor/server.mjs` 增加 `upgrade` 处理，二进制帧 + 单调 `frameId`；慢消费时丢旧帧，不排队。
- 页面把 `<canvas id="gl">` 换成接收流的绘制面，参数面板与 LOD 阶梯保持不动。

验收：改一个参数，原生画面在 ≤ 150 ms 内反映；连续 60 s 无句柄泄漏、无帧积压；断流后编辑器回退到 WebGL 视口并提示。

### 9.5 共享纹理的实测契约（读自 `node_modules/electron/electron.d.ts`，44.9.1）

这些是类型定义里写明的约束，实施时不必再猜：

| 项 | 合同 | 对原生侧的含义 |
| --- | --- | --- |
| 句柄 | `SharedTextureHandle.ntHandle: Buffer`，且注明"**NT HANDLE is local to current process**" | 必须 `DuplicateHandle` 到 Electron 的目标进程；不能把整数句柄值塞进 JSON 传过去 |
| 像素格式 | `'bgra' \| 'rgba' \| 'rgbaf16' \| 'nv12' \| 'nv16' \| 'p010le'` | 用 BGRA8（`DXGI_FORMAT_B8G8R8A8_UNORM`），无需格式转换 |
| 互斥 | "`rgba`/`bgra`/`rgbaf16` 的句柄**没有 keyed mutex**；只有 `nv12` 有" | 不能依赖 D3D 自动互斥。必须自己做生产/消费同步：原生侧 GPU 完成后才发布，配双/三缓冲，且消费者未归还前不覆写 |
| 生命周期 | `getVideoFrame()` 产 `VideoFrame`（内部等 GPU 完成）；`release()` 逐份释放；`allReferencesReleased` 在全部引用释放后回调 | 每帧的 import 对象要显式 `release`；纹理本体活到 `allReferencesReleased` 才可回收 |
| 尺寸 | `codedSize: Size` + 可选 `visibleRect` | resize 时改 codedSize 并重建资源；旧句柄不能用于新尺寸 |
| 跨进程 | `webContents.sendSharedTexture()` + `setSharedTextureReceiver()`，或主进程导入后经 `startTransferSharedTexture()` 转交 | 原生输出若在独立进程，走 transfer；若在 Electron 主进程内，可直接 import |

**推论**：B 路径的难点不在"导入"，而在**背压与回收**——没有 keyed mutex 时，"这一帧 GPU 写完了吗""Chromium 还在读吗"两个问题都要自己答。用 3 缓冲 + 原生 fence + `allReferencesReleased` 做归还信号，是成本最低的可行方案。

### 9.6 C 路径已落地（2026-10-26）

按用户选择，直接实现了原生子窗口方案。它现在不是方案，是可以跑起来的东西：

| 文件 | 作用 |
| --- | --- |
| `host/viewport_host.cpp` | 视口宿主进程：`WS_CHILD` 窗口、D3D12 交换链、植被绘制（真实材质 + 主光阴影）、鼠标环绕/缩放、命名管道控制 |
| `tools/viewport-host.ps1` | 编译与运行；`-Probe` 复测交换链能力，`-Screenshot` 出单帧证据 |
| `editor/preload.cjs` | 唯一通道：把视口矩形发给主进程（沙箱内，无文件/进程能力） |
| `editor/main.mjs` | 起停宿主、经命名管道转发矩形、退出时回收 |
| `editor/ui/vegetation.{html,css,mjs}` | `原生视口` 开关；开启时隐藏 WebGL 画布与 HUD，按 `devicePixelRatio` 上报矩形 |

实测（本机，AMD Radeon 890M，Windows/D3D12）：

| 项 | 结果 |
| --- | --- |
| 页面切到原生 | `native: true`，画布 `display: none`，状态栏「原生视口 · 引擎渲染」 |
| 上报矩形 | CSS `811×525 @ (272,84)`，dpr 2 → 宿主 1622×1050 @ (544,168) |
| 画面 | 引擎实际渲染的橡树（70,008 tri、6 张贴图、3×3 PCF 阴影）出现在视口，非 WebGL 替身 |
| 切回 WebGL | 再次点击 → 画布恢复、宿主收到 `v=0` 隐藏 |
| 生命周期 | 关窗后 `nativegpu_viewport_host.exe` 自动消失（`before-quit` → kill） |
| 单帧证据 | `build/viewport-host/shots/oak.png`（1280×720）；桌面合成截图 `build/viewport-host/shots/desktop.png` |
| 缩放跟随 | 布局视口 900×640 时页面重报 CSS 284×300 → 宿主子窗口实测 568×600（设备像素，`tests/host_rect_probe.cpp` 读数），恢复后回到 1622×1050；`ResizeBuffers` 与客户区同步 |
| 隐藏/重显循环 | 关→开（矩形未变）后宿主仍能重新收到矩形并显示（主进程在 `v=0` 时重置去重记录） |

**三个踩到的问题，都会重复踩，所以记下来：**

1. **ESM 主进程顶层 `await app.whenReady()` 会死锁**（Electron 44.9.1）。模块在 app ready 之前被求值，loader 占着事件循环，`ready` 永远送不到。表现是进程活着、没有窗口、没有任何日志。仓库原来的 `editor/main.mjs` 就是这个写法——**编辑器在本版本 Electron 下从未成功启动过**。已改为 `app.whenReady().then(start)`。
2. **交换链格式**：flip 模型拒绝 `_SRGB`（`0x887A0001`）；bitblt（`DXGI_SWAP_EFFECT_DISCARD`）在这台机上所有组合都被拒。可用组合只有 `flip + R16G16B16A16_FLOAT`，并且必须 `SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)`，否则线性辐射度被当成 sRGB 数据，画面暗约两档。复测：`tools/viewport-host.ps1 -Probe`。
3. **`setTextures` 与 `upload` 不能录在同一条命令列表**：`upload()` 开头清空 staging 数组，会销毁贴图上传缓冲，然后设备挂起（`0x887A0006`）。必须分两批提交。这是 `VegetationDraw` 的接口契约，之前只在测试里偶然满足。
4. **隐藏路径不能把 `{visible:false}` 直接铺成 `x=..;y=..` 协议行**：页面发 `{ visible: false }` 时主进程曾写出 `x=undefined;...;v=0`。宿主靠 `v=0` 提前返回侥幸没事，但协议行里出现 `undefined` 迟早咬人。已改为隐藏只发 `v=0`，且同时重置主进程的去重记录——否则"关→开（矩形没变）"会被去重吞掉，宿主永远停在隐藏态（2026-10-26 缩放验证时发现）。

**已知边界（当前行为，不是待办清单）：**

1. **物种在宿主启动时固定**：`--species oak_broadleaf`。UI 里切换物种，原生画面不变——控制行还没有 `s=<id>`，热重载物种未实现。
2. **仍是单页单物种**：宿主用一页合成实例（默认 1 株），区域 25 页一次绘制仍受 A2/A3 限制，见 [VEGETATION-DEFECTS-AND-PLAN.md](VEGETATION-DEFECTS-AND-PLAN.md)。
3. **输入单向**：鼠标环绕/缩放由宿主自己实现，事件不回传页面，页面不知道相机状态（因此相机数值 HUD 在原生模式下隐藏）。
4. **z 序靠定时抬升，弹窗遮挡由页面隐藏处理**：Chromium 的子窗口是同级窗口，宿主每 500 ms `SetWindowPos(HWND_TOP)` 一次防止被页面重绘盖住。HTML 侧的 z-index 对 HWND 无效，因此页面在出现 `dialog[open]` 时（模态框的 `::backdrop` 覆盖整个窗口；非模态框只在几何上与视口相交时）上报 `{visible:false}`，由主进程转发 `v=0`，宿主调用 `ShowWindow(SW_HIDE)`。最后一个弹窗关闭后页面重新推送矩形，宿主恢复可见——见 `editor/ui/app.mjs` 的 `nativeOccluded()`/`hideNative()`。
5. **DPI 用 `devicePixelRatio` 换算**：100%/200% 缩放下实测正常，125%/150% 未测。
6. **受限环境需要放开 Chromium 沙箱**：本会话所在的沙箱里 GPU 进程与 OS 沙箱都起不来，验证时用 `NG_ELECTRON_FLAGS="--no-sandbox --disable-gpu"`。正常桌面不需要。

### 9.7 与"两套着色器"问题的关系

内嵌真正的价值不是"画面更好看"，而是消掉 `editor/ui/vegetation.mjs` 与 `shaders/vegetation.hlsl` 的**两套着色器**：编辑器里的外观与运行时由同一份 HLSL 决定，所见即所得。但在 A 完成前，WebGL 视口应保留作为零依赖回退（纯浏览器路径），不能直接删除。

## 10. 来源

- [Electron sharedTexture API](https://www.electronjs.org/docs/latest/api/shared-texture)：外部资源导入、转交、释放接口与实验性状态。
- [Windows SharedTextureHandle](https://www.electronjs.org/docs/latest/api/structures/shared-texture-handle)：NT HANDLE 的进程归属。
- [Electron 44.9.1 共享纹理设计](https://github.com/electron/electron/blob/v44.9.1/shell/common/api/shared_texture/README.md)：Windows 句柄复制、资源所有权与 GPU 完成释放语义。
- 本仓库：`renderer/d3d12_renderer.*`、`renderer/rhi.hpp`、`host/main.cpp`、`engine/native/game.hpp`、`editor/main.mjs`、`editor/ui/app.mjs`。

此方案是对 [OUTPUT-CONTRACT.md](OUTPUT-CONTRACT.md) 的具体宿主与编辑工作流补充。旧输出契约中的实现推断、同步规则及行号在实施时必须重新核对；例如共享 fence 的创建标志不能照搬旧文档中资源堆标志的写法。
