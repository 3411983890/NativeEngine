# 粒子资产的 GPU 场沉积模块

2026-10-24，C/B 的资产与原生运行时增量。`fieldDeposit` 将整数沉积量从作者资产保留到认证 cook、原生模块和 GPU 实例驱动，复用已有 GPU LEF/WF。宿主默认驱动与编辑器写场配置尚未接入；现有未配置驱动的宿主仍明确拒绝写场效果。

## 作者合同与上界

```json
{
  "id": "density",
  "type": "fieldDeposit",
  "after": ["initial"],
  "params": { "channel": "density", "quantum": 100 }
}
```

效果必须通过既有 `field` 声明相应通道、level、dimensions 和 voxelMetres。当前仅接受 density、moisture 非负标量，以及 [1, UINT32_MAX] 的整数 quantum。温度的热容量合并和速度质量加权没有相同含义，不允许借用本模块冒充。原 `fieldCoupling` 仅声明通道，缺少沉积规则，仍不自动执行。

quantum 表示“每个实际存活粒子对目标细单元沉积的目标通道整数单位”，不自动等于物理质量或每秒流量。每 tick 用存活粒子重新生成该源的当前场；WF 发布为替换，不能将每 tick 沉积继续加到旧来源版本。目标单位、缩放、owner、域与归约由写驱动解析的场目录定义，并由 LEF/WF 的完整元数据检查约束。

JS 编译器和原生认证读者分别证明同一通道的 `asset.maxAlive × 所有发射器/模块的 quantum 之和 ≤UINT32_MAX`。各通道独立核算；没有通过同步 GPU alive 回读放宽上界。该界采用每发射器池容量，可能比实例共享存活额度保守。实际写入仍经过 GpuLocalFields 每批次的容量累计准入、最终统一钳位和损失诊断。

访问合同声明 reads=position、fieldWrites=指定通道、phase=postSimulation。所有沉积读取本 tick 模拟与 spawn 完成后的最终位置。普通模拟模块不能依赖更晚的 fieldDeposit；JS 和原生解析器都会拒绝这种跨阶段依赖。沉积之间可保持稳定作者顺序；同源同通道的整数和受既有上界保护。

## 原生实例驱动

`GpuRuntime(device,limits,GpuFieldDepositDriver)` 在构造时显式配置驱动。不配置时，写场资产返回 UnsupportedFeature；CPU fallback 和独立 GPU 池也默认拒绝。低层池只有调用者显式设置 externalFieldDeposits 才允许把该模块委托给外部执行，不会自行声称已经写场。

驱动接收 `GpuDepositFrame`：当前命令列表、真实提交 fence 值、固定 120 Hz 视觉 runtime tick、simulated 标志，以及全部活跃写场实例（包括暂停实例）。每项含 generation 句柄、实例 serial、不可变 Asset、原点、paused 和按资产顺序借用的 GPU 池。serial 在本次 runtime 生命周期内稳定唯一；重建 runtime 后，宿主需同步重建/重置对应驱动身份域。

`step` 在所有池完成 simulate/spawn 后、实例年龄与全局 tick 推进前调用驱动一次。驱动可直接将 GPU 池交给 LEF.splat，随后 resolve/project、通过共享控制器发布并提交 WF。输入不用 CPU 粒子数组、位置读回或场值上传。池指针只在回调内借用，不能保留；LEF 自己通过实际 fence 保留借用资源。

暂停实例仍在完整集合内，驱动应保留其已发布贡献，不重新 splat 或刷新 sourceTick。自然耗尽、stop/clear 导致实例消失，驱动通过集合差异撤销对应来源。`flush` 也发送完整集合，simulated=false：只同步生命周期，不能再次沉积或推进模拟发布时钟。可以暂存下一 tick 的撤销，在下次真实模拟提交确认，重复 flush 应幂等。写者的最终退出和独立 LEF 退休仍由驱动实现。

驱动必须和生产者使用同一设备/实际队列/fence，遵守 LEF 与 WF 的所有权和预算合同，并避免重入 runtime。它还负责目录映射、网格/原点、按需创建 LEF、观察策略、WF 读者及代际清理；本接口不自动生成第二份来源账本或取代共享控制器。

## 录制失败

驱动可能在粒子命令已经记录后失败，不能把该 tick 当成没有模拟过而原地重试。返回 false 或抛出异常会设置 fieldDepositFailed，保留错误并拒绝后续 step/flush/prepareDraw 及新 spawn，防止重复模拟或部分来源被当成成功。调用者放弃或排空已录制工作后重建 runtime，并处理驱动自己的资源状态；这里不宣称 GPU 命令能回滚。

新增 fieldDepositCalls、fieldDepositFlushCalls 和 fieldDepositFailed 遥测。既有运行时仍只异步读取有界的粒子计数遥测；沉积不新增同步粒子属性或场值读回。GPU 视觉输出不进入 CPU 权威玩法判断。

## 增量验证

```powershell
node --test tests/effect-deposits.test.mjs tests/effects.test.mjs
npm run effects:deposits -- -Adapter AMD
npm run effects:deposits -- -SkipBuild -Adapter AMD
```

独立输出为 `build/effects-deposits`。专项生成认证资产夹具，只选编已有 16 个池/LEF/WF/查询 shader；没有修改 shader 数学或增加 blob。标准构建只登记新测试目标，本轮没有执行完整构建、全量回归、完整 native/JS/CI、npm test 或 npm run verify。

`tests/native_effect_deposits.cpp` 中 Writer 是验证用协调器，通过生产 API 驱动真实 GPU 数据，不是 renderer 的默认生产生命周期服务。夹具为固定 8³、density、1m 网格，用于验证资产传输和运行时调用顺序，不能据此宣称任意网格/移动/温度/体积或宿主已经完成。

正式 [沉积链路报告](../reports/0.11.0/31c00bc53c0b/20261024T141840Z/effects-deposits-evidence.json)：**5 组/121 检查**，890M，实际 debugLayer=true、debugErrors=0，冻结来源/认证夹具、生成 shader/header、二进制、编译及运行日志。

覆盖认证量化参数及容量证明、无驱动明确拒绝、后模拟阶段依赖拒绝、多发射器/实例实际 GPU 存活贡献、暂停不刷新来源时间、local-space 位置、替换而非累加、立即停止/重复 flush、自然退出的空活跃集合、真实 queue gate 下退休 LEF 和旧 WF 视图保留，以及返回错误/异常后的失败隔离。

相关增量回归：

- [粒子池/实例](../reports/0.11.0/31c00bc53c0b/20261024T141900Z-7d98c5/effects-gpu-evidence.json)：8 组/49842 检查、7 组/1158 检查，实际调试层 errors=0。
- [WF/共享控制器](../reports/0.11.0/31c00bc53c0b/20261024T141900Z/fields-world-gpu-evidence.json)：13 组/1601 检查/128 次 CPU 数值查询比较，实际调试层 errors=0。
- [JS 资产与新模块](../reports/0.11.0/31c00bc53c0b/20261024T142129Z/effect-deposits-js-evidence.json)：两个相关文件、15/15，9 项预先冻结输入，前后哈希相同。未运行全部 JS 测试。

## 仍需完成

生产写驱动及 renderer/宿主配置、资产声明到目录/单位/网格和生命周期的完整映射、移动 LEF、按观察和配额生产候选投影、动态 GPU 浓度遥测、覆盖去重体积、温度/速度算子、GPU 事件队列、编辑器与联合预算仍未完成。原 A–G goal 保持 active。


2026-10-24 后续：[生产沉积服务与 renderer](EFFECT-DEPOSIT-SERVICE.md) 已加入目录/网格/实例映射、观察调度、粒子快照移动重建、停止回收及默认 renderer 回调。上文“生产驱动尚未接入”是首次交付边界；统一体积、一般求解历史迁移、完整预算准入与降档仍未完成。正式记录区分首轮功能/计数失败和修复后的增量生命周期结果。
