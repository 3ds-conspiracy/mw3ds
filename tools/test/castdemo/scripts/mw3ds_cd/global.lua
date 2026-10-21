-- MW3DS cast demo, the world's half (OpenMW 0.51): the player goes to Pelagiad, a made spell (Frost Damage 12 - 15 for 10 s on
-- Target, as a reporter's save had it) is added and cast at Dralas Gilu from 250 units, and every quarter second the log
-- gets "MW3DS-CD t health bounty guards" (t: game seconds since the cast; guards: each one in range, its distance).
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local world = require('openmw.world')
local I = require('openmw.interfaces')

local frame = 0
local dralas = nil
local castAt = nil
local lastLog = -1
local kSeconds = 25
local released = false
local castReal = 0

local function cells(player)
	local out = {}
	for dx = -1, 1 do
		for dy = -1, 1 do
			out[#out + 1] = world.getExteriorCell(player.cell.gridX + dx, player.cell.gridY + dy)
		end
	end
	return out
end

local function findDralas(player)
	for _, cell in ipairs(cells(player)) do
		for _, o in ipairs(cell:getAll(types.NPC)) do
			if o.recordId == 'dralas gilu' then
				return o
			end
		end
	end
end

local function guards(player)
	local s = {}
	for _, cell in ipairs(cells(player)) do
		for _, o in ipairs(cell:getAll(types.NPC)) do
			if o.enabled and types.NPC.record(o).class == 'guard' and not types.Actor.isDead(o) then
				local d = (o.position - player.position):length()
				if d < 3000 then
					s[#s + 1] = string.format('%s:%.0f', (o.recordId:gsub(' ', '_')), d)
				end
			end
		end
	end
	return table.concat(s, ',')
end

return {
	engineHandlers = {
		onUpdate = function()
			local player = world.players[1]
			frame = frame + 1
			world.mwscript.getGlobalVariables(player).chargenstate = -1
			if frame == 60 then
				world.setGameTimeScale(0)
				world.mwscript.getGlobalVariables(player).gamehour = 17
				player:teleport('', util.vector3(1519, -57400, 1500), { onGround = true })
			elseif frame == 400 then
				dralas = findDralas(player)
				if not dralas then
					print('MW3DS-CD no Dralas')
					print('MW3DS-CD done')
					return
				end
				-- 250 units from him, on the side the player stands on now, facing him
				dralas:sendEvent('MW3DS_CD_Hold')
				local d = player.position - dralas.position
				d = util.vector3(d.x, d.y, 0):normalize()
				local at = dralas.position + d * 250
				local yaw = math.atan2(-d.x, -d.y)
				player:teleport('', at, { rotation = util.transform.rotateZ(yaw), onGround = true })
				local spell = world.createRecord(core.magic.spells.createRecordDraft({
					name = 'Grand Frostbite', cost = 55, isAutocalc = false, type = core.magic.SPELL_TYPE.Spell,
					effects = { { id = core.magic.EFFECT_TYPE.FrostDamage, range = core.magic.RANGE.Target, area = 0,
						duration = 10, magnitudeMin = 12, magnitudeMax = 15 } },
				}))
				types.Actor.spells(player):add(spell.id)
				player:sendEvent('MW3DS_CD_Cast', { spell = spell.id, yaw = yaw })
				print('MW3DS-CD begin spell ' .. spell.id)
			elseif dralas and frame > 400 then
				local t = core.getSimulationTime()
				if not castAt then
					castAt = t
					castReal = core.getRealTime()
				end
				if t - lastLog >= 0.25 then
					lastLog = t
					print(string.format('MW3DS-CD %.2f %.0f %d %s', t - castAt,
						types.Actor.stats.dynamic.health(dralas).current, types.Player.getCrimeLevel(player),
						string.format('away:%.0f,mag:%.0f,fx:%d,%s', (dralas.position - player.position):length(), types.Actor.stats.dynamic.magicka(player).current, (function() local n = 0 for _ in pairs(types.Actor.activeSpells(dralas)) do n = n + 1 end return n end)(), guards(player))))
				end
				if not released and t - castAt > 1.5 then
					released = true
					dralas:sendEvent('MW3DS_CD_Release')
				end
				-- (real time: an arrest's dialogue stops the simulation clock, and the run would wait on it for ever)
				if core.getRealTime() - castReal > kSeconds then
					print('MW3DS-CD done')
					dralas = nil
				end
			end
		end,
	},
}
