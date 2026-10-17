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

-- A scene step's actions (do=, sidebyside.py), ';' between them
local function act(s, player)
	for step in string.gmatch(s.act, '[^;]+') do
		local a = {}
		for w in string.gmatch(step, '[^:]+') do
			a[#a + 1] = w
		end
		if a[1] == 'goto' then
			player:teleport(a[2], util.vector3(tonumber(a[3]), tonumber(a[4]), tonumber(a[5])), { onGround = true })
		elseif a[1] == 'journal' then
			types.Player.quests(player)[a[2]]:addJournalEntry(tonumber(a[3]), player)
		elseif a[1] == 'give' then
			world.createObject(a[2], 1):moveInto(types.Actor.inventory(player))
		elseif a[1] == 'global' then
			world.mwscript.getGlobalVariables(player)[a[2]] = tonumber(a[3])
		elseif a[1] == 'kill' or a[1] == 'group' then
			local found = nil
			for _, list in ipairs({ player.cell:getAll(types.NPC), player.cell:getAll(types.Creature) }) do
				for _, o in ipairs(list) do
					if o.recordId == a[2] then
						found = o
					end
				end
			end
			if not found then
				print(string.format('MW3DS-SBS scene: no %s here', a[2]))
			elseif a[1] == 'kill' then
				found:sendEvent('MW3DS_SBS_Kill', {})       -- (health is changed by its own script only)
			else
				found:sendEvent('MW3DS_SBS_Play', { group = a[3] })
			end
		end
		print(string.format('MW3DS-SBS scene: %s', step))
	end
end

local function setup(data)
	local s = poses[data.index]
	local player = world.players[1]
	if s.act then
		-- (a scene step: the world goes on from the last one, the clock running)
		act(s, player)
		pending = { shot = s, index = data.index, frames = 0 }
		return
	end
	world.setGameTimeScale(0)
	local globals = world.mwscript.getGlobalVariables(player)
	-- --start skips character creation but its scripts still run (the map menu's message box): -1 says it's done
	globals.chargenstate = -1
	if s.hour then
		globals.gamehour = s.hour
	end
	if s.weather and not core.weather then
		print('MW3DS-SBS no core.weather in this OpenMW')
	end
	if s.weather and core.weather then
		-- the weather of the shot's region (an exterior: the cell under the camera)
		local c = s.cell and world.getCellByName(s.cell) or world.getExteriorCell(math.floor(s.x / 8192), math.floor(s.y / 8192))
		local rec = core.weather.records[s.weather]
		if c and c.region and rec then
			-- (it only starts a transition, run on game time: the clock starts three hours early and three hours go
			-- by for it, the sky ending at the shot's hour; set back afterwards it kept the later sky)
			globals.gamehour = ((s.hour or globals.gamehour) - 3) % 24
			core.weather.changeWeather(c.region, rec)
			world.advanceTime(3)
		else
			print(string.format('MW3DS-SBS no weather %s for shot %d', s.weather, data.index))
		end
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
		if s.vfx then
			hideActors = true          -- (a spell visual on its own: nobody near)
		end
	end
	if s.hide then
		hideActors = true          -- (hide=1: nobody near, no one casting at the player)
	end
	-- the shot's weapon, ammunition, shield and worn armor and clothing (player.lua equips them)
	local inv = types.Actor.inventory(player)
	local ids = { s.weapon or false, s.ammo or false, s.shield or false }
	for _, id in ipairs(s.wear or {}) do
		ids[#ids + 1] = id
	end
	for _, id in ipairs(ids) do
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
	-- (a flyer stays at the height Morrowind.esm gives it: onGround would drop it to the floor)
	local flies = types.Creature.objectIsInstance(a) and types.Creature.record(a).canFly
	a:teleport(a.cell, a.startingPosition, { rotation = rot, onGround = not flies })
	a:sendEvent('MW3DS_SBS_Pose', { group = s.group, startKey = s.startKey, stopKey = s.stopKey, at = s.at or 0,
		index = p.index })
end

return {
	eventHandlers = {
		MW3DS_SBS_Setup = setup,
		-- a vfx= shot: the visual played on its own at its spot (as the 3DS side's VFX step)
		MW3DS_SBS_Vfx = function(d)
			local s = poses[d.index]
			local opts = { mwMagicVfx = true }
			if s.ptex then
				opts.particleTextureOverride = s.ptex
			end
			world.vfx.spawn(s.vfx, util.vector3(s.vfxX, s.vfxY, s.vfxZ), opts)
			print(string.format('MW3DS-SBS vfx %s', s.vfx))
		end,
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
				-- a few frames for the cell's objects to come in (a change of weather: more, for its transition)
				if pending.frames == (pending.shot.weather and 90 or 20) then
					local p = pending
					pending = nil
					poseActor(p)
				end
			end
		end,
	},
}
