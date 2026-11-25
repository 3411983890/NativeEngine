return {
 on_interact = function(ctx, event)
  if ctx.state.opened then return end
  local inventory = ctx:get(event.actor, 'inventory') or {items={}}
  if (inventory.items[ctx.properties.requiredItem] or 0) < 1 then
   ctx:hud(event.actor, 'The gate is locked. Find the gold key.', 300)
   return
  end
  if ctx.properties.consumeItem then
   inventory.items[ctx.properties.requiredItem] = inventory.items[ctx.properties.requiredItem] - 1
   ctx:set(event.actor, 'inventory', inventory)
  end
  ctx.state.opened = true
  ctx:set(ctx.entity, 'collider', {enabled=false})
  ctx:set(ctx.entity, 'renderable', {offset={0,3.2,0}})
  ctx:set(ctx.entity, 'interactable', {enabled=false})
  ctx:hud(event.actor, 'Gate open. Reach the green exit.', 300)
  ctx:emit('', 'door.opened', {entity=ctx.entity, actor=event.actor})
 end
}
