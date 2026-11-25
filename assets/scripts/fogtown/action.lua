return {
 on_interact = function(ctx, event)
  ctx:emit(ctx.properties.quest, 'fogtown.action', {
   actor=event.actor, action=ctx.properties.action, source=ctx.entity, value=ctx.properties.value
  })
 end
}
