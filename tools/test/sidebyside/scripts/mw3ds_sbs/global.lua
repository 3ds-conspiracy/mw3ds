-- MW3DS side by side, the world's half (OpenMW 0.51). For each shot in poses.lua (written by tools/test/sidebyside.py):
-- the clock stops at the shot's hour, the player goes behind the camera (out of the shot), and the shot's actor goes
-- back to where Morrowind.esm placed it and holds its animation group at a point (actor.lua). Then the player script
-- poses the camera. A cast shot puts the player at its from= spot with the spell (player.lua makes the cast sure to
-- succeed), and the world stands still while it is shot (MW3DS_SBS_Halt).
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local world = require('openmw.world')
local poses = require('scripts.mw3ds_sbs.poses')

local pending = nil      -- { shot, frames }: a shot whose cell is loading
local hideActors = false -- a cast shot: everyone near the player hidden once the cell is in (sidebyside.py HIDE_RADIUS)
local kHideRadius = 4000
local started = false
local spawned = {}       -- spawn= shots: the actor made for each id, kept for the next shot

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
	local cam = util.vector3(s.x, s.y, s.z)
	if s.cast then
		player:teleport(s.cell or '', util.vector3(s.fromX, s.fromY, s.fromZ),
			{ rotation = util.transform.rotateZ(s.fromYaw), onGround = true })
		types.Actor.spells(player):add(s.cast)
		hideActors = true
	elseif s.body then
		-- the player's own body in the picture, seen from the camera: nobody else near
		player:teleport(s.cell or '', util.vector3(s.fromX, s.fromY, s.fromZ),
			{ rotation = util.transform.rotateZ(s.fromYaw), onGround = true })
		hideActors = true
	elseif s.fp then
		-- first person: the player stands there (cam is their feet), a spell to ready
		player:teleport(s.cell or '', cam, { rotation = util.transform.rotateZ(s.yaw), onGround = true })
		types.Actor.spells(player):add(s.spell or 'fire bite')
	else
		-- 300 units behind the camera: in the cell, never in the picture
		local back = util.vector3(-math.sin(s.yaw), -math.cos(s.yaw), 0) * 300
		player:teleport(s.cell or '', cam + back)
	end
	-- the shot's weapon, ammunition and shield (player.lua equips them)
	local inv = types.Actor.inventory(player)
	for _, id in ipairs({ s.weapon or false, s.ammo or false, s.shield or false }) do
		if id and inv:countOf(id) == 0 then
			world.createObject(id, id == s.ammo and 50 or 1):moveInto(inv)
		end
	end
	if s.spawn then
		-- a new one (made once per id), s.spawn units ahead of the camera, facing it (s.face)
		local a = spawned[s.actor]
		if not a or not a:isValid() then
			a = world.createObject(s.actor, 1)
			spawned[s.actor] = a
		end
		local at = util.vector3(s.x + math.sin(s.yaw) * s.spawn, s.y + math.cos(s.yaw) * s.spawn, s.z)
		a:teleport(s.cell or '', at, { rotation = util.transform.rotateZ(s.face), onGround = true })
		hideActors = true
	end
	pending = { shot = s, index = data.index, frames = 0 }
end

local function hideNear(player)
	local cells = { player.cell }
	if player.cell.isExterior then
		cells = {}
		for dx = -1, 1 do
			for dy = -1, 1 do
				cells[#cells + 1] = world.getExteriorCell(player.cell.gridX + dx, player.cell.gridY + dy)
			end
		end
	end
	local n = 0
	for _, cell in ipairs(cells) do
		for _, list in ipairs({ cell:getAll(types.NPC), cell:getAll(types.Creature) }) do
			for _, o in ipairs(list) do
				local d = o.position - player.position
				if o.enabled and util.vector2(d.x, d.y):length() < kHideRadius then
					o.enabled = false
					n = n + 1
				end
			end
		end
	end
	print(string.format('MW3DS-SBS %d actors hidden', n))
end

local function poseActor(p)
	local s = p.shot
	local player = world.players[1]
	if hideActors then
		hideActors = false
		hideNear(player)
	end
	if s.fp or not s.actor then
		player:sendEvent('MW3DS_SBS_Posed', { index = p.index })
		return
	end
	if s.spawn then
		-- (everyone else near hidden, this one shown again)
		local a = spawned[s.actor]
		a.enabled = true
		a:sendEvent('MW3DS_SBS_Pose', { group = s.group, startKey = s.startKey, stopKey = s.stopKey, at = s.at or 0,
			index = p.index })
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
	a:sendEvent('MW3DS_SBS_Pose', { group = s.group, startKey = s.startKey, stopKey = s.stopKey, at = s.at or 0,
		index = p.index })
end

return {
	eventHandlers = {
		MW3DS_SBS_Setup = setup,
		MW3DS_SBS_Halt = function(d)
			world.setSimulationTimeScale(d.on and 0 or 1)
		end,
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
