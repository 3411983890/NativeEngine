# 玩法运行时

数据驱动玩法运行时由 `engine/native/gameplay/`、原生适配层 `gameplay_bridge.cpp` 和完整快照 `gameplay_snapshot.cpp` 组成。使用固定 Lua 5.4.9。按用户要求不接入编辑器，不新增验收关卡；测试使用内存场景。

## 内容与扩展

`assets/gameplay/components.json` 是组件字段契约。原生 Registry、场景工具与 AI 共用它。原生模块也可以调用 `world.registry.add(name, definition)` 注册新类型。未知类型/字段会拒绝，不会静默丢弃。

实体使用持久 ID；原生高频访问使用 index/generation Handle。销毁后的 ID 不允许重新分配，旧句柄失效。查询按实体 ID 排序。

现有场景字段继续有效。可选的 `components` 覆盖对应的初始组件；组件状态保存在运行时，不会自动写回场景。钥匙/门/终点旧类型会自动获得默认 Lua 行为。显式配置 `behavior.instances` 时使用该列表，包括空列表。

```json
{
  "components": {
    "interactable": {"range": 2.5, "prompt": "打开门"},
    "behavior": {
      "instances": [
        {"id": "lock", "script": "gameplay/locked_door", "properties": {"requiredItem": "silver_key"}}
      ]
    }
  }
}
```

同一脚本的多个实例各自拥有 properties/state/RNG；行为 ID 在同一实体上唯一。第二把钥匙和第二扇门不需要修改 C++。

新增能力的步骤：登记组件字段及版本 → 注册原生系统或添加 Lua 行为 → 通过命令/事件交互 → 添加恢复/确定性与字段校验测试。新增原生系统使用 `Runtime::registerSystem`，声明名称、Phase、after 和接收 `Runtime&` 的回调；回调不得捕获临时 World 指针。原生系统的持久数据也应进入组件，不能藏在外部全局变量。

## 行为描述文件

`assets/scripts/<name>.behavior.json` 声明 `apiVersion:1`、正整数 `version`、properties/state 的字段 schema。持续更新行为还需声明 `update:true`。运行中可能创建的其他行为模块须在 `dependencies: ["path/to/behavior"]` 中显式声明；引擎和打包工具递归解析依赖并拒绝循环，确保录制开始时即可锁定完整内容。此字段声明行为资源依赖，不提供 Lua require。schema 支持 object/array/boolean/string/integer/number、oneOf、required、default、enum、范围和数量上限。默认严格拒绝未知字段。状态字段应提供初始化默认值。

Lua 文件返回回调表。生命周期：`on_start`、`on_update`、`on_interact`、`on_trigger_enter`、`on_trigger_exit`、`on_event`、`on_destroy`、`on_reload`。事件名通过 `event.name` 传入。

```lua
return {
  on_start = function(ctx)
    ctx:timer("pulse", 120, "pulse", {}, 120)
  end,
  on_event = function(ctx, event)
    if event.name == "pulse" then
      ctx.state.count = ctx.state.count + 1
    end
  end
}
```

每次回调创建受限 Lua 环境。全局变量和闭包局部变量不会跨回调保留；影响未来模拟的数据必须写入已声明的 `ctx.state`。不支持持久协程或任意 Lua 对象图快照。

## 脚本 API

| API | 用途 |
| --- | --- |
| `ctx.entity / properties / state / tick / dt / input` | 当前实例、配置、状态及模拟输入 |
| `ctx:get(id, component)` | 组件副本；修改副本必须再提交 |
| `ctx:query(component)` | 按 ID 排序的实体列表 |
| `ctx:set(id, component, value)` | 替换组件，补全 schema 默认值 |
| `ctx:remove(id, component)` | 移除组件 |
| `ctx:spawn(id, components)` / `destroy(id)` | 在结构提交阶段创建/销毁实体 |
| `ctx:emit(target, name, payload)` | 下一 tick 的事件；空目标为广播 |
| `ctx:timer(id, delay, name, payload, interval)` | 整数 tick 计时器；interval=0 为一次 |
| `ctx:cancel_timer(id)` | 取消本实例的同名计时器 |
| `ctx:random(name, bound)` | 独立命名随机流，整数范围 [0,bound) |
| `ctx:hud(actor, text, ticks)` | 设置 HUD 文本状态 |
| `ctx:play(id, clip)` | 请求原生动画蒙太奇 |
| `ctx:presentation(name, payload)` | 表现请求，不能反馈决定模拟 |
| `ctx:keys(table)` / `ctx:array()` | 排序键列表／明确标记的空数组 |

`set` 是组件替换，部分字段省略会恢复默认值。需要保留旧字段时，先 get，再修改，再 set。Lua 查询按需读取组件，不会每次复制整张场景。

默认不开放文件、系统时间、动态加载、debug、协程、无序 pairs/next、进程随机数、pcall/xpcall。排序遍历使用 query/keys/ipairs。RNG 使用有状态整数 LCG，每个实体/行为/名字独立派生；其状态参与快照与摘要，失败回调不消耗随机流。

## 调度与失败

运行时阶段为 Input → Events → Behaviors → Simulation → Collect → Commit。系统名唯一，依赖必须存在且不能逆阶段；循环依赖拒绝执行。原生 Game 的角色/物理/动画保持在现有固定步长路径，适配层同步组件状态。脚本创建的 transform/collider/rigidbody/renderable 实体自动获得方盒显示与原生物理生命周期；移除/禁用碰撞体和销毁实体会停用对应物理体。修改尺寸/质量时重算惯量，物理速度与变换回写组件。玩家 transform 位置修改按传送处理。

事件按 due tick、递增序号排序。碰撞/触发采集产生下一 tick 事件，避免同步递归。触发器保存重叠集合、once/fired 状态。计时器身份是实体 ID + 行为 ID + timer ID，同名设置替换旧计时器。

每次 Lua 回调先生成状态、RNG 和命令副本，全部校验成功后提交；失败禁用该实例，并记录实体、行为、tick、错误码。其他实例继续运行。创建/销毁延迟提交，销毁时执行受限 on_destroy，并清除该实体计时器。

默认预算：每回调 100000 条 Lua 指令、4 MiB Lua 堆、128 条命令；回调结果展开最多 32768 个节点、1 MiB 字符串/键，防止共享表放大占用；事件/计时器各 4096 条，结构请求 1024 条。新建实体需要在 spawn 的组件载荷内配置。on_destroy 不允许继续创建/销毁实体。

原生扩展代码仍需遵守内存安全约束；该限制机制不构成进程级不可信代码隔离。宿主 C++ 故障不属于单实例隔离承诺。

## 热重载

交互运行时自动检查已加载脚本。候选模块编译、字段验证及所有受影响实例迁移成功，才在 tick 边界一起替换；失败保留旧模块和实例状态。

状态 version 改变时，候选模块可提供 `migrate(ctx,event)`，`event.oldVersion` 为旧版本；迁移后按新 schema 检查。相同版本直接保留登记状态。`on_reload` 用于无世界副作用的实例修整。迁移和 reload 回调不允许提交世界命令。

第一版模块自包含，不提供 require；一个模块组可通过原生 `reload(replacements)` 原子替换。通过同一注册 API 重新启用修复后的故障行为。

## 存档与回放

F7 保存 `saves/quicksave.json`，F12 恢复；也支持 `--save <path>`（退出时保存）、`--load-save <path>`（启动时恢复）。

完整模拟快照保存组件、实例状态、RNG、事件、计时器、触发重叠、待提交结构、待处理输入边沿、固定步累积余量、车辆、刚体内部状态、破坏状态及动画过渡/蒙太奇状态。派生物理接触缓存在下一步重建，墙钟性能统计不恢复。

快照使用版本化 JSON、校验和、临时文件和原子替换。恢复先在候选状态上验证；不匹配的场景/脚本/组件/动画内容拒绝恢复。当前热重载支持状态迁移，磁盘旧内容版本存档明确拒绝，尚未提供跨版本游戏进度升级器。

回放 v4 保存源码构建指纹、场景身份、Lua/组件/动画内容契约；record 和 hash-log 生成 `.initial.json` 初始模拟快照，逐帧记录精确 dt，支持变帧率录制。正式录制/回放关闭自动重载，回放在模拟前验证内容。旧版本日志可解析，但不同模拟契约下的摘要不能作为兼容性保证。

第一版确定性承诺为同机、同构建、同内容和输入；不承诺跨平台浮点结果一致。错误码属于诊断契约，错误文本不属于稳定协议。

## 工具与验证

- `gameplay_catalog`：组件与行为字段描述。
- `gameplay_validate`：场景行为参数和依赖校验（不执行 Lua）；原生场景加载还会受限编译 Lua。
- `gameplay_inspect`：读取 reports/ 内的状态/错误报告。
- 场景组件修改沿用 `scene_preview` / `scene_apply` / `scene_undo` 的 revision 与原子事务。
- 运行中的状态报告位于 `reports/gameplay/live.json`；原生运行报告也包含 gameplay 段。
- 场景构建包包含组件目录、Lua、行为描述文件及其 SHA-256；修改脚本会改变构建键，篡改缓存包会被拒绝。

运行 `npm run gameplay` 或 `.incrementalTesting/core/gameplay.ps1`。测试覆盖真实文件热重载、依赖锁定与循环拒绝、运行时物理实体创建/变更/销毁、损坏快照拒绝、多个独立门钥匙、指令/内存预算、回调回滚、系统依赖、结构生命周期、触发器、计时器、命名 RNG、热重载迁移及完整重启续跑。无十分钟关卡要求。

## 扩展边界

原生渲染/碰撞仍使用当前方盒与既有角色控制器；场景作者格式保留旧字段并增加组件。现有适配层逐步拆分，未来更换物理或增加 glTF/Prefab 时沿组件和系统接口接入。Lua 回调重建环境优先保证显式状态；大量持续更新行为需先测预算，再增加编译缓存或批处理。实体 ID 墓碑与原生物理槽位当前保留到场景重置；超长会话的压缩/回收需要后续性能工作。

