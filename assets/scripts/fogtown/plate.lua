return {
 on_update = function(ctx)
  if ctx.state.latched or ctx.tick % 12 ~= 0 then return end
  local plate = ctx:get(ctx.entity, 'transform').position
  for _, id in ipairs(ctx.properties.weights) do
   local transform = ctx:get(id, 'transform')
   if transform then
    local p = transform.position
    if math.abs(p[1]-plate[1]) < 0.85 and math.abs(p[3]-plate[3]) < 0.85 and p[2] < 1.2 then
     ctx.state.latched = true
     local progress = ctx:get(ctx.properties.actor, 'progress')
     progress.flags.plate = true
     ctx:set(ctx.properties.actor, 'progress', progress)
     ctx:emit('', 'fogtown.power', {})
     ctx:emit('', 'plate.latched', {actor=ctx.properties.actor})
     ctx:hud(ctx.properties.actor, '配重已就位，两道仓库闸门开启。进入后室取维修工具。', 600)
     ctx:presentation('audio.power', {})
     return
    end
   end
  end
 end
}
