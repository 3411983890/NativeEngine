return {
 on_trigger_enter = function(ctx, event)
  if ctx.state.complete then return end
  if ctx.properties.requiredDoor ~= '' then
   local door = ctx:get(ctx.properties.requiredDoor, 'collider')
   if not door or door.enabled then return end
  end
  local progress = ctx:get(event.actor, 'progress') or {flags={}}
  progress.flags.complete = true
  ctx:set(event.actor, 'progress', progress)
  ctx.state.complete = true
  ctx:hud(event.actor, 'Courtyard complete.', 1200)
  ctx:presentation('game.complete', {actor=event.actor})
 end
}
