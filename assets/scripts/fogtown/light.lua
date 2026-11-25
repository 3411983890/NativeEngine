local function refresh(ctx)
 local progress = ctx:get(ctx.properties.actor, 'progress') or {flags={}}
 local enabled = progress.flags[ctx.properties.flag] or false
 local light = ctx:get(ctx.entity, 'illumination')
 light.enabled = enabled
 ctx:set(ctx.entity, 'illumination', light)
 ctx:set(ctx.entity, 'renderable', {enabled=enabled})
end
return {
 on_start = refresh,
 on_event = function(ctx, event)
  if event.name == 'fogtown.power' then refresh(ctx) end
 end
}
