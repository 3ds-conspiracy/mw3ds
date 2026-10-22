-- MW3DS blood demo, the world's half (OpenMW 0.51): the player stands in Seyda Neen with an iron dagger, a Fargoth is made 90 units
-- ahead facing them and held still; the player's script swings. "MW3DS-CD begin" when he is there, "hit" when his health first drops
-- (and what he has left), "done" a few real seconds after.
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local world = require('openmw.world')

local frame = 0
local npc = nil
local full = nil
local hitSeen = false
local doneAt = nil
local beginAt = nil
local lastLog = -1

return {
	engineHandlers = {
		onUpdate = function()
			local player = world.players[1]
			frame = frame + 1
			world.mwscript.getGlobalVariables(player).chargenstate = -1
			if frame == 60 then
				world.setGameTimeScale(0)
				world.mwscript.getGlobalVariables(player).gamehour = 12
				player:teleport('', util.vector3(-10300, -71300, 200), { onGround = true })
			elseif frame == 200 then
				local dagger = world.createObject('iron dagger', 1)
				dagger:moveInto(types.Actor.inventory(player))
				npc = world.createObject('fargoth', 1)
				-- yaw 0 faces +Y: 90 ahead, turned to face the player
				npc:teleport('', player.position + util.vector3(0, 90, 0), { rotation = util.transform.rotateZ(math.pi), onGround = true })
				npc:sendEvent('MW3DS_CD_Hold')
				player:sendEvent('MW3DS_CD_Swing', {})
				beginAt = core.getRealTime()
				print('MW3DS-CD begin')
			elseif npc and frame > 200 then
				local h = types.Actor.stats.dynamic.health(npc).current
				full = full or h
				if core.getRealTime() - lastLog >= 0.25 then
					lastLog = core.getRealTime()
					print(string.format('MW3DS-CD health %.1f', h))
				end
				if not doneAt and not hitSeen and core.getRealTime() - beginAt > 10 then
					doneAt = core.getRealTime()
				end
				if not hitSeen and h < full then
					hitSeen = true
					doneAt = core.getRealTime() + 3
					print(string.format('MW3DS-CD hit health %.1f of %.1f', h, full))
				end
				if doneAt and core.getRealTime() > doneAt then
					print('MW3DS-CD done')
					doneAt = nil
				end
			end
		end,
	},
}
