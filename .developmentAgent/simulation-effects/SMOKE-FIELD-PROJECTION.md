# 求解后烟密度的共享场投影

2026-10-26，接续 [烟火实际渲染](SMOKE-RENDERING.md) 与 [GPU 世界场](FIELD-WORLD-GPU.md)。本记录对应实际 GPU 烟场到不可变 LEF 投影和视觉 WF 的数值桥。后续已补充 [自动发布和按源渲染选择](SMOKE-SHARING.md)；下面的原生证据仍只证明数值桥本身。

## 所有权与来源

`smoke::densityFieldInput` 获取求解器当前 Density 标量缓冲、SoA 偏移、米制网格、真实 fence 时间线、sourceTick 和 version。读取的是已经平流/衰减/燃烧后的 kg/m³ 状态，不是粒子个数或 kg/s 源率。求解所有权继续属于烟火服务；场适配不接管压力、速度、燃料和临时通道。

`GpuLocalFields::replaceScalar` 只接受单通道 LEF、同设备和同真实队列/fence 时间线、严格递增 sourceTick。输入网格必须完整覆盖且对齐到目标细格，不进行隐含的缩放、平移或补零裁剪。原始求解版本保存在 `GpuLocalInfo.inputVersion` 与 `GpuProjectionBindings.inputVersion`；LEF 本身的 localVersion 独立计数。

来源读取在全部元数据/容量检查通过后才调用 `prepareRead`；求解器更新其真实消费 fence，并进入 non-pixel 只读状态。输入缓冲另外保留一个 COM 引用直到该 fence 完成。适配描述符的纯元数据 `current` 检查在解引用资源之前拒绝已推进或关闭的生产者；未准入的描述符不会额外持有烟场分配，避免退休后仍在预算之外保留缓冲。描述符只能同步借用，求解器对象须活到导入调用结束；不是任意历史 GPU 快照。

## 单位、量化与守恒诊断

场 Channel 的 `unit` 应为 kg/m³，固定整数乘子为 `unitsPerInteger`，读取时除以乘子恢复浓度。每个细格采用 `floor(density*unitsPerInteger+0.5)`；输入上限乘量化比例、比例本身及通道整数上限不超过 2²³−1，超过时在录制前拒绝。该限制让 float 舍入/整数转换具有有界定义，不能静默截断高动态范围。

非有限、负值或大于生产者声明上限的 GPU 单元记为 invalid 并贡献零；有效细格计入 accepted。量化后复用既有 Resolve 的通道上限钳制、钳制格数和整数损失统计。之后复用体积平均投影：不完整粗格按完整粗格体积作分母，域外细格为空；输入积分、重构积分和有符号残差由既有 64 位归约记录。

常用密度通道为 VisualReference、Sum、VolumeAverage。此流程不把温度按来源直接相加；温度需要质量/热容量联合合同，速度也需要单独的加权规则。GPU 视觉值不写入 CPU 权威 World。

## 发布、查询和撤销

输出是原有 `GpuLocalProjection`，可直接提交给 `GpuVisualWorld.publish`。相同 source/epoch 重发为 Duplicate；新 epoch 替换该来源，而不是把新旧烟量累加。多个独立来源按注册通道规约聚合，提交后由现有版本化 GPU 查询读取；旧投影和旧 WF 视图保持不可变，直到读取 fence 和外部引用都结束。

停止共享使用原有 withdraw + commit。若仍有读取租约，其请求区域可以继续保留未知页；最后来源撤销后查询返回 Unavailable，不能误报为“已知零烟”。若同时退还所有覆盖租约，则当前 WF 可无数据页；旧读取视图仍按真实 fence 保留。若仍有其他来源，该来源继续贡献。CPU 玩法消费者须使用各自权威粗场合同，不可将视觉读取结果用作确定性伤害输入。

## 增量验证与未完成项

入口 `npm run smoke:fields` 仅构建所需原生依赖和相关 shader，并在执行前冻结输入与二进制。包含实际求解器的五种细/粗分辨率比例、负坐标与部分边界、独立体积平均和残差对拍、双源 WF 发布/替换/重复/撤销、旧快照、失效借用、准入失败及真实队列阻塞寿命；另有 GPU 非法数值注入。相关原 LEF/WF 专项作为增量回归。

最终 [20261026T004900Z](../reports/0.11.0/31c00bc53c0b/20261026T004900Z/smoke-fields-evidence.json)：8 组/216 检查、87 次独立密度/WF 比较，实际 AMD 890M 调试错误 0。[来源审计](../reports/0.11.0/31c00bc53c0b/20261026T004939Z-smoke-fields-final/smoke-fields-finalization.json) 为 129 次当前比较、129 次冻结核验及 143 项检查通过；相关原 LEF 为 9/2363、WF 为 15/1681，均实际调试错误 0。原 107 个 shader 定义保留，只新增 ImportScalar；原场数值核和烟火数值后端未改。

后续自动共享服务已接入租约/观察提升/降级、预算延期和按源 LEF/WF 二选一；详见独立 [共享合同](SMOKE-SHARING.md)。任意网格守恒重采样、温度/速度联合投影、可编辑渐变 LOD 和标准联合负载仍待完成，没有将数值桥验收扩展为 D/B/A–G 全部完成。
