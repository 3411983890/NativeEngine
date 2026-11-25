local function objective(ctx, title, detail, position, step)
 ctx:set(ctx.properties.actor, 'objective', {title=title, detail=detail, position=position, step=step, total=5})
end
local function power(ctx, progress)
 ctx:set(ctx.properties.actor, 'progress', progress)
 ctx:emit('', 'fogtown.power', {})
end
local function checkpoint(ctx, name)
 ctx:presentation('checkpoint', {name=name})
end
local function towerReady(ctx)
 objective(ctx, '信号塔', '启动塔下控制台，再在 45 秒内依次连接西侧、东侧、塔基中继。', {0,1,-46}, 5)
end
return {
 on_start = function(ctx)
  objective(ctx, '镇口值守站', '前方是值守员。靠近并对准他，按 E 询问停电原因。', {0,1.3,8}, 1)
  ctx:hud(ctx.properties.actor, '雾镇失去联络了。沿街恢复供能，重新启动信号塔。', 840)
 end,
 on_event = function(ctx, event)
  if event.name == 'door.opened' and event.item == 'warehouse_key' and ctx.state.stage == 1 then
   objective(ctx, '老街与仓库', '站到右侧木箱后面，向黄色压力板推动，开启后室的两道闸门。', {-6.6,0.5,-7.8}, 2)
   return
  end
  if event.name == 'plate.latched' and event.actor == ctx.properties.actor and ctx.state.stage == 1 then
   objective(ctx, '老街与仓库', '两道闸门已经打开。进入后室，拿取橙色维修工具箱。', {-8,1,-11}, 2)
   return
  end
  if event.name == 'tower.timeout' then
   if not ctx.state.running then return end
   ctx.state.running = false
   ctx.state.relay = 0
   local progress = ctx:get(ctx.properties.actor, 'progress')
   progress.flags.tower_running = false
   power(ctx, progress)
   towerReady(ctx)
   ctx:hud(ctx.properties.actor, '启动超时。供能已经保留，回到塔下控制台即可重试。', 840)
   ctx:presentation('audio.fail', {})
   return
  end
  if event.name ~= 'fogtown.action' then return end
  local actor = ctx.properties.actor
  if event.actor ~= actor then return end
  local action = event.action
  local progress = ctx:get(actor, 'progress')
  local inventory = ctx:get(actor, 'inventory')
  if action == 'briefing' then
   if ctx.state.stage == 0 then
    inventory.items.warehouse_key = 1
    ctx:set(actor, 'inventory', inventory)
    progress.flags.briefed = true
    power(ctx, progress)
    ctx.state.stage = 1
    objective(ctx, '老街与仓库', '左侧仓库：用钥匙开门，把木箱推到黄色压力板，再去后室取工具。', {-8,1,-2}, 2)
    ctx:hud(actor, '值守员：钥匙给你。仓库后室有工具，黄色压力板需要一只木箱压住。', 1080)
    ctx:presentation('audio.power', {})
   elseif ctx.state.stage == 5 then
    ctx:hud(actor, '值守员：灯亮了，信号也恢复了。谢谢你，让雾镇重新与外界相连。', 1200)
   else
    ctx:hud(actor, '值守员：广场供能顺序是「水井、钟楼、风车」，记住蓝、金、绿。', 840)
   end
  elseif action == 'tool' then
   if ctx.state.stage ~= 1 then return end
   inventory.items.repair_tool = 1
   ctx:set(actor, 'inventory', inventory)
   ctx:set(event.source, 'renderable', {enabled=false})
   ctx:set(event.source, 'interactable', {enabled=false})
   progress.flags.tools = true
   power(ctx, progress)
   ctx.state.stage = 2
   objective(ctx, '维修院', '返回主街向前，在红色起爆器上按 E 清理塌方，再修复后方发电机。', {3.5,1,-20}, 3)
   ctx:hud(actor, '获得维修工具。维修院设有受控爆破装置；对准红色起爆器按 E。', 840)
   checkpoint(ctx, 'warehouse')
   ctx:presentation('audio.power', {})
  elseif action == 'demolish' then
   if ctx.state.stage < 2 then ctx:hud(actor, '需要先到仓库取维修工具。', 480); return end
   if ctx.state.demolished then return end
   ctx.state.demolished = true
   ctx:set(actor, 'blast_request', {center={0,0.8,-23},radius=6.8,speed=18})
   objective(ctx, '维修院', '塌方已清理。越过碎石，修复左前方的发电机。', {-3,1,-28}, 3)
   ctx:hud(actor, '受控爆破完成。小心碎石，前往发电机恢复供能。', 660)
   ctx:presentation('audio.blast', {})
  elseif action == 'repair' then
   if ctx.state.stage ~= 2 then
    ctx:hud(actor, ctx.state.stage > 2 and '发电机正在稳定供电。' or '缺少维修工具。', 420)
    return
   end
   for _, id in ipairs(ctx.properties.rubble) do
    local state = ctx:get(id, 'destruction')
    if state and not state.fractured then ctx:hud(actor, '先使用起爆器清理塌方，避免供电短路。', 480); return end
   end
   progress.flags.repaired = true
   power(ctx, progress)
   ctx.state.stage = 3
   objective(ctx, '雾中广场', '按线索启动三个节点：水井（蓝）→ 钟楼（金）→ 风车（绿）。', {-3.6,1,-35}, 4)
   ctx:hud(actor, '维修院恢复供能，街灯亮起。前往广场，按水井、钟楼、风车的顺序接通节点。', 960)
   checkpoint(ctx, 'repair')
   ctx:presentation('audio.power', {})
  elseif action == 'node' then
   if ctx.state.stage ~= 3 then
    ctx:hud(actor, ctx.state.stage > 3 and '广场供能已经稳定。' or '先修复维修院发电机。', 420)
    return
   end
   local order = {2,1,3}
   local value = event.value
   if value ~= order[ctx.state.node+1] then
    ctx.state.node = 0
    progress.flags.node1=false; progress.flags.node2=false; progress.flags.node3=false
    power(ctx, progress)
    objective(ctx, '雾中广场', '节点已复位。重新按水井（蓝）→ 钟楼（金）→ 风车（绿）的顺序接通。', {-3.6,1,-35}, 4)
    ctx:hud(actor, '顺序不匹配，节点已复位。线索：水井（蓝）→ 钟楼（金）→ 风车（绿）。', 720)
    ctx:presentation('audio.fail', {})
    return
   end
   ctx.state.node = ctx.state.node + 1
   progress.flags['node'..value] = true
   if ctx.state.node == 3 then
    progress.flags.square = true
    ctx.state.stage = 4
    power(ctx, progress)
    towerReady(ctx)
    ctx:hud(actor, '广场点亮，信号塔闸门解锁。启动控制台，准备最后的中继连接。', 840)
    checkpoint(ctx, 'square')
   else
    power(ctx, progress)
    local nextPosition = ctx.state.node == 1 and {0,1,-38} or {3.6,1,-35}
    objective(ctx, '雾中广场', ctx.state.node == 1 and '水井已接通。下一个：钟楼（金）。' or '钟楼已接通。下一个：风车（绿）。', nextPosition, 4)
    ctx:hud(actor, '节点接通（'..ctx.state.node..'/3）。', 420)
   end
   ctx:presentation('audio.power', {})
  elseif action == 'start' then
   if ctx.state.stage ~= 4 then ctx:hud(actor, '需要先恢复广场的三个供能节点。', 480); return end
   if ctx.state.running then ctx:hud(actor, '中继连接进行中，请按目标提示依次操作。', 360); return end
   ctx.state.running = true
   ctx.state.relay = 0
   ctx.state.deadline = ctx.tick + ctx.properties.challengeTicks
   progress.flags.tower_running = true
   power(ctx, progress)
   ctx:timer('tower', ctx.properties.challengeTicks, 'tower.timeout', {})
   objective(ctx, '信号塔 · 中继连接', '先连接左前方的西侧中继。', {-4,1,-49}, 5)
   ctx:hud(actor, '45 秒启动窗口已开启：西侧 → 东侧 → 塔基。', 480)
   ctx:presentation('audio.power', {})
  elseif action == 'relay' then
   if not ctx.state.running then ctx:hud(actor, '先在塔下控制台启动连接流程。', 480); return end
   if event.value ~= ctx.state.relay + 1 then ctx:hud(actor, '连接顺序：西侧 → 东侧 → 塔基。', 360); return end
   ctx.state.relay = ctx.state.relay + 1
   if ctx.state.relay == 3 then
    ctx.state.running = false
    ctx.state.stage = 5
    ctx:cancel_timer('tower')
    progress.flags.tower_running = false
    progress.flags.complete = true
    power(ctx, progress)
    objective(ctx, '雾镇重启 · 联络恢复', '信号已发出，小镇重新亮起。你也可以回到镇口，与值守员告别。', {0,1,8}, 5)
    ctx:hud(actor, '雾镇重启成功。整条街恢复照明，信号塔已向外界发出联络。', 3600)
    checkpoint(ctx, 'complete')
    ctx:presentation('audio.complete', {})
   else
    objective(ctx, '信号塔 · 中继连接', ctx.state.relay == 1 and '西侧已连接。前往右前方的东侧中继。' or '东侧已连接。前往塔基，发出信号。', ctx.state.relay == 1 and {4,1,-52} or {0,1,-57}, 5)
    ctx:presentation('audio.power', {})
   end
  elseif action == 'cache' then
   if ctx.state.cache then return end
   ctx.state.cache = true
   inventory.items.service_key = 1
   ctx:set(actor, 'inventory', inventory)
   ctx:set(event.source, 'renderable', {enabled=false})
   ctx:set(event.source, 'interactable', {enabled=false})
   ctx:hud(actor, '找到备用钥匙，可打开广场右侧的维修柜。两把钥匙互相独立。', 720)
  end
 end
}
