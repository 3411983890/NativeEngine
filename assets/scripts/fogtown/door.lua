local function open(ctx)
 ctx.state.opened = true
 ctx:set(ctx.entity, 'collider', {enabled=false})
 ctx:set(ctx.entity, 'renderable', {offset={0,3.4,0}})
 ctx:set(ctx.entity, 'interactable', {enabled=false})
 ctx:emit('', 'door.opened', {entity=ctx.entity, item=ctx.properties.item})
end
return {
 on_interact = function(ctx, event)
  if ctx.state.opened then return end
  local inventory = ctx:get(event.actor, 'inventory') or {items={}}
  local progress = ctx:get(event.actor, 'progress') or {flags={}}
  if ctx.properties.item ~= '' and (inventory.items[ctx.properties.item] or 0) > 0 then
   open(ctx)
   ctx:hud(event.actor, ctx.properties.message, 480)
   ctx:presentation('audio.door', {})
  elseif ctx.properties.flag ~= '' and progress.flags[ctx.properties.flag] then
   open(ctx)
  else
   ctx:hud(event.actor, ctx.properties.lockedMessage, 480)
  end
 end,
 on_event = function(ctx, event)
  if event.name ~= 'fogtown.power' or ctx.state.opened then return end
  local progress = ctx:get(ctx.properties.actor, 'progress') or {flags={}}
  if ctx.properties.flag ~= '' and progress.flags[ctx.properties.flag] then open(ctx) end
 end
}
