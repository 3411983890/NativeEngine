return {
 on_interact = function(ctx, event)
  if ctx.state.collected then return end
  local inventory = ctx:get(event.actor, 'inventory') or {items={}}
  inventory.items[ctx.properties.item] = (inventory.items[ctx.properties.item] or 0) + 1
  ctx:set(event.actor, 'inventory', inventory)
  ctx:set(ctx.entity, 'renderable', {enabled=false})
  ctx:set(ctx.entity, 'interactable', {enabled=false})
  ctx:hud(event.actor, ctx.properties.message, 300)
  ctx.state.collected = true
  ctx:emit('', 'item.collected', {actor=event.actor, item=ctx.properties.item})
 end
}
