-- MW3DS play fight, the world's half (OpenMW 0.51). Per fight in fights.lua (written by tools/test/playfight.py): the
-- player goes to the fight's spot, everyone near is hidden, the fighter is made facing the player, and after a few frames
-- its Combat package is started against the player. The fight lasts `seconds` of game time; "MW3DS-PF done" follows.
local types = require('openmw.types')
local util = require('openmw.util')
local world = require('openmw.world')
local core = require('openmw.core')
local fights = require('scripts.mw3ds_pf.fights')

local kHideRadius = 4000
local frame = 0
local fighter = nil
local startedAt = nil
local finished = false

local function hideNear(player)
	local cells = {}
	for dx = -1, 1 do
		for dy = -1, 1 do
			cells[#cells + 1] = world.getExteriorCell(player.cell.gridX + dx, player.cell.gridY + dy)
		end
	end
	for _, cell in ipairs(cells) do
		for _, list in ipairs({ cell:getAll(types.NPC), cell:getAll(types.Creature) }) do
			for _, o in ipairs(list) do
				local d = o.position - player.position
				if o.enabled and util.vector2(d.x, d.y):length() < kHideRadius then
					o.enabled = false
				end
			end
		end
	end
end

return {
	engineHandlers = {
		onUpdate = function()
			if finished then
				return
			end
			frame = frame + 1
			local player = world.players[1]
			local f = fights[1]
			world.mwscript.getGlobalVariables(player).chargenstate = -1
			if frame == 60 then
				world.setGameTimeScale(0)
				world.mwscript.getGlobalVariables(player).gamehour = f.hour or 12
				player:teleport('', util.vector3(f.px, f.py, f.pz), { rotation = util.transform.rotateZ(f.pyaw), onGround = true })
			elseif frame == 120 then
				hideNear(player)
				-- the fighter dist ahead of the player, facing them
				local at = util.vector3(f.px + math.sin(f.pyaw) * f.dist, f.py + math.cos(f.pyaw) * f.dist, f.pz)
				fighter = world.createObject(f.fighter, 1)
				fighter:teleport('', at, { rotation = util.transform.rotateZ(f.pyaw + math.pi), onGround = true })
			elseif frame == 180 then
				player:sendEvent('MW3DS_PF_Prepare', { health = f.health, cam = f.cam, retreat = f.retreat })
				fighter:sendEvent('MW3DS_PF_Fight', { target = player })
				startedAt = core.getSimulationTime()
				print('MW3DS-PF begin ' .. f.fighter)
			elseif startedAt and core.getSimulationTime() - startedAt >= f.seconds then
				finished = true
				print('MW3DS-PF done')
			end
		end,
	},
}
