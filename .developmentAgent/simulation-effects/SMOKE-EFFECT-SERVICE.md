# 烟火效果资产与 GPU 实例服务

2026-10-25。效果资产新增可选 `smoke`，由原有 effect cook/2 容器认证；`GpuSmokeService` 通过显式 hooks 接入 `GpuRuntime` 的 spawn、simulate/spawn 后、flush、暂停和退休流程。新资产模板为 [smoke_solver_reference.effect.json](../assets/effects/smoke_solver_reference.effect.json)。旧 `smoke_reference` 是共享场模块参考，保持原有内容与用途。

原生实例服务之后，已补充 [宿主渲染与介质积分](SMOKE-RENDERING.md) 及 [密度自动共享与按源选择](SMOKE-SHARING.md)。新模板里的精灵用于标记粒子源。温度/速度联合共享、完整时域及云烟混合仍未完成。

## 作者合同

```json
"smoke": {
  "dimensions": [16, 16, 16],
  "cellMetres": 0.25,
  "originOffset": [-2, -0.5, -2],
  "channels": ["density", "temperatureKelvin", "fuel"],
  "tailTicks": 240,
  "sources": [{
    "emitter": "source",
    "densityMassRate": 0.005,
    "heatVolumeRate": 1,
    "fuelMassRate": 0.002
  }],
  "parameters": { "maxSpeed": 4, "pressureIterations": 32 }
}
```

每个 source 引用唯一的 emitter ID，数值单位依次为 kg/s、K·m³/s、kg/s，均为每个存活粒子的持续源贡献。温度源是艺术模型的热体积速率，不是焓。默认 `unitsPerSource=[1000000,1000,1000000]`，可显式覆盖三个比例为 1–10⁹ 整数；正源以最近整数舍入，拒绝溢出及舍入为零，误差至多每粒子每秒 0.5/unit。Cook 记录量化结果，原生认证加载器从已认证 source 独立重建并校验。

量化后的 conservative `sum(asset.maxAlive*quantum)` 每通道不得超过 uint32，最坏单格速率不得超过 10⁶。未启用通道的非零源、重复/未知 emitter、全零源、非有限值、未知键均拒绝。网格外贡献仍由 [源项诊断](SMOKE-PARTICLE-SOURCES.md) 记录，不自动扩大网格。

dimensions 各轴 2–128，cellMetres 为 0.01–10m；originOffset 是相对生成位置的世界坐标偏移，范围 ±100000m。channels 可以按需启用 density、temperatureKelvin、fuel，至少一个，不需要额外的 WF 租约来拥有此私有求解器。

parameters 可覆盖 `Spec` 的 ambientKelvin、maxSpeed、courant、densityDecay、cooling、fuelDecay、burnRate、ignitionKelvin、heatPerFuel、sootPerFuel、maxDensity、maxTemperature、maxFuel、buoyancy、smokeWeight、vorticity、pressureRelaxation、pressureIterations、maxSubsteps、macCormack；未指定项使用 [求解器默认值及限制](SMOKE-FIRE-SOLVER.md)。Cook 与 native 校验 float 存储后的温度关系、正数下溢、固定 120 Hz 的 CFL 子步数；非法资产不等到首次 GPU tick 才失败。求解器、原生资产和服务使用相同纯配置校验，不增加 CPU 资产工具对 D3D12 的链接要求。

`previewEffectPatch` 允许 smoke 路径；即使粒子 SoA 不变，只要归一化后的烟火配置/源改变就返回 reset-required。GPU 实例热迁移仍未实现，不能据此宣称编辑器已在线应用。

## 接入与生命周期

```cpp
engine::smoke::GpuSmokeService smoke(device, actualFence);
engine::effects::GpuRuntime runtime(device, runtimeLimits, fieldDriver,
                                   fieldAdmission, smoke.hooks());
// install 认证资产、spawn，按原有 step/flush 接口录制同一队列上的命令。
// smoke.outputs() 返回含 serial、generation handle、sourceTick 的借用输出；
// 每次消费前 smoke.prepareRead(serial, list, submissionFence, error)。
```

hooks 同时要求 admission 与 record；未配置服务时含 smoke 的 spawn 明确拒绝。CPU fallback 也明确拒绝，不只绘制精灵而漏掉烟火。一个服务对应一个 GPU Runtime、设备、实际队列/fence，不能在另一运行时重用 serial 命名空间。

- **生成**：先按真实对齐分配预留完整烟场/不可变输入峰值，再构建求解器和默认零输入，最后由 Runtime 创建粒子池。粒子分配失败时 RAII permit 退还烟火资源及尚未提交的 serial；可以正常重试。
- **正常 tick**：Runtime 完成所有粒子 simulate/spawn 后提交完整 live set。服务按各资产来源读取 GPU 池，生成同 tick 不可变速率并推进自己的历史，dt 固定为 1/120s。
- **移动**：烟场网格锚定生成位置；移动实例改变后续粒子源位置，不平移历史烟团。超出网格会丢失新的源贡献并计数；当前没有滚动网格、守恒搬迁或自动扩大范围。
- **暂停**：粒子和对应烟场均停止演化；继续提供旧版本输出，sourceTick 不刷新。恢复只推进新的模拟 tick，不补算暂停时长。
- **停发**：非立即 stop 先让现有粒子自然耗尽。Runtime 的已完成异步视觉计数确认结束后释放实例，烟场转为独立零源尾烟，保留 tailTicks 个 simulation tick。tailTicks 可为 0；不是按画面帧数计时。
- **立即终止/clear**：下一次 step/flush 同步退休烟场，立即退出可见输出集合；仍等待已记录 GPU 使用完成后释放资源。flush 只同步生命周期，不重复注入、不推进尾烟年龄。
- **异常**：服务中途录制失败或调用方 driver 在录制后报告失败，会让 Runtime 拒绝后续 step/draw/spawn，防止重放半个 tick。先提交/排空已经录制的命令，再销毁重建。服务/runtimes 必须活到对应 shutdown 成功。

外部消费者得到的是可在下一次 step 更新的求解器缓冲，不能当作永久历史快照。prepareRead 登记每次消费的真实 fence，同一服务跨实例的消费 fence 也不得倒退。正常关闭需 clear/flush 同步退休、释放所有消费并等实际队列完成；自然尾烟尚在演化时不能关闭服务。

## 预算和回退

ServiceLimits 限定实例数（包括尾烟、未完成退休）、真实分配字节和每 tick 派发数。每实例预留为：求解状态/临时缓冲 + 一个默认不可变 CPU 输入 + maxFrames 份完整 rate/accumulator/diagnostics；尚未实际分配的部分也占准入额度。stats 分别报告 reservedBytes 与 allocationBytes。生产器和求解器各自的字节、输入槽、派发限制仍执行。

调度优先最近成功步进最早的来源，同级按稳定 serial；不会因固定首位来源持续占满派发配额而饿死其他来源。资源/派发预算不足时跳过该 tick 的视觉烟火步进，粒子继续模拟；`skippedSteps`、每源 lastAttemptTick 和旧 buffers.sourceTick 显式反映降级。恢复后使用当前源执行一个固定 tick，不用新位置补算旧贡献，不刷新被跳过版本的时间。这是允许视觉质量下降的回退，非守恒补算、非延期输入队列，也不影响 CPU 权威状态。

这里还没有四系统联合 GPU 时间预算、质量分辨率降档或玩法粗代理保障；不能把派发上限当作已达到标准场景性能门禁。

## 验证范围

`npm run smoke:service` 只构建 `build/smoke-service` 的依赖，编译既有 24 个相关 shader，不新增数值 shader。执行前冻结二进制/递归来源/夹具/生成 shader，运行 13 项相关 JS 资产测试及原生专项。原生覆盖认证资产及重新签名的非法 source、CPU/GPU 未配置拒绝、CPU 数值参考、暂停/移动/重复 flush、自然尾烟/立即终止、取消未初始化实例、发射前准入、粒子分配失败回收/重试、真实 queue gate 下跳步与恢复、派发公平性、跨实例 fence 倒退和录制后 driver 失败。

本批另按依赖回归 CPU 效果、GPU Runtime、原烟火求解器及共享场发布服务；最终报告见 [开发记录](SIMULATION-DEVELOPMENT.md)。没有执行全量测试、全量构建、verify 或 CI。未完成宿主与统一体积、场发布/覆盖去重、时域/热浪/玩法及其余 A–G 目标。
