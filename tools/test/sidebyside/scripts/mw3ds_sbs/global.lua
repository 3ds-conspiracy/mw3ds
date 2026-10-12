-- MW3DS side by side, the world's half (OpenMW 0.51). For each shot in poses.lua (written by tools/test/sidebyside.py):
-- the clock stops at the shot's hour, the player goes behind the camera (out of the shot), and the shot's actor goes
-- back to where Morrowind.esm placed it and holds its animation group at a point (actor.lua). Then the player script
-- poses the camera.
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local world = require('openmw.world')
local poses = require('scripts.mw3ds_sbs.poses')

local pending = nil      -- { shot, frames }: a shot whose cell is loading
local started = false

local function findActor(cell, id, near)
	local best, bestD = nil, math.huge
	for _, list in ipairs({ cell:getAll(types.NPC), cell:getAll(types.Creature) }) do
		for _, o in ipairs(list) do
			if o.recordId == id then
				local d = (o.startingPosition - near):length()
				if d < bestD then
					best, bestD = o, d
				end
			end
		end
	end
	return best
end

-- Character creation's local scripts (CharGenFatigueBarrel's map menu box, ...) fire on a player who skipped it,
-- whenever they come near: each is set done, as it sets itself once it has run. The objects stay
local function silenceChargen()
	local player = world.players[1]
	local cells = { player.cell }
	if player.cell.isExterior then
		cells = {}
		for dx = -1, 1 do
			for dy = -1, 1 do
				cells[#cells + 1] = world.getExteriorCell(player.cell.gridX + dx, player.cell.gridY + dy)
			end
		end
	end
	for _, cell in ipairs(cells) do
		for _, o in ipairs(cell:getAll()) do
			local s = world.mwscript.getLocalScript(o)
			if s and s.recordId:sub(1, 7) == 'chargen' then
				pcall(function() s.variables.done = 1 end)
			end
		end
	end
end

local function setup(data)
	local s = poses[data.index]
	local player = world.players[1]
	world.setGameTimeScale(0)
	local globals = world.mwscript.getGlobalVariables(player)
	-- --start skips character creation but its scripts still run (the map menu's message box): -1 says it's done
	globals.chargenstate = -1
	if s.hour then
		globals.gamehour = s.hour
	end
	-- 300 units behind the camera: in the cell, never in the picture
	local cam = util.vector3(s.x, s.y, s.z)
	local back = util.vector3(-math.sin(s.yaw), -math.cos(s.yaw), 0) * 300
	player:teleport(s.cell or '', cam + back)
	pending = { shot = s, index = data.index, frames = 0 }
end

local function poseActor(p)
	local s = p.shot
	local player = world.players[1]
	if not s.actor then
		player:sendEvent('MW3DS_SBS_Posed', { index = p.index })
		return
	end
	local a = findActor(player.cell, s.actor, util.vector3(s.x, s.y, s.z))
	if not a then
		print(string.format('MW3DS-SBS no actor %s near shot %d', s.actor, p.index))
		player:sendEvent('MW3DS_SBS_Posed', { index = p.index })
		return
	end
	local rot = a.startingRotation
	if s.face then
		rot = util.transform.rotateZ(s.face)
	end
	a:teleport(a.cell, a.startingPosition, { rotation = rot, onGround = true })
	a:sendEvent('MW3DS_SBS_Pose', { group = s.group, at = s.at or 0, index = p.index })
end

return {
	eventHandlers = {
		MW3DS_SBS_Setup = setup,
		MW3DS_SBS_ActorPosed = function(d)
			world.players[1]:sendEvent('MW3DS_SBS_Posed', d)
		end,
	},
	engineHandlers = {
		onUpdate = function()
			if not started then
				started = true
				world.mwscript.getGlobalVariables(world.players[1]).chargenstate = -1
			end
			silenceChargen()
			if pending then
				pending.frames = pending.frames + 1
				-- a few frames for the cell's objects to come in
				if pending.frames == 20 then
					local p = pending
					pending = nil
					poseActor(p)
				end
			end
		end,
	},
}
