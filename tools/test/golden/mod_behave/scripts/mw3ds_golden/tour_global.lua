-- MW3DS golden tour, the world's half (OpenMW 0.51). Dumps every dialogue line once, then takes the player to each cell
-- of plan.lua in turn, the game clock stopped, and after the cell's scripts have run dumps what involves no dice: each
-- object's state, the local variables of its script, the terrain, and asks the actors and the player for their parts.
-- Globals are dumped every kGlobalsEvery cells and at the end. Written for tools/test/golden/golden_behave.py.
local core = require('openmw.core')
local types = require('openmw.types')
local world = require('openmw.world')
local util = require('openmw.util')
local plan = require('scripts.mw3ds_golden.plan')

local kMinFrames = 14       -- in a cell before the dump (a script that waits a frame or two has run)
local kMinSeconds = 0.6
local kReportFrames = 3     -- for the actors' and the player's answers
local kGlobalsEvery = 100
local kGrid = 8             -- terrain samples per side of an exterior cell
local kRaysPerCell = 40

local index = plan.start - 1
local frames = 0
local started = nil
local dumpedAt = nil
local settle = 0

local function clean(s)
	return (tostring(s or ''):gsub('[\t\r\n]', ' '))
end

local function dumpDialogue()
	local stores = { 'journal', 'topic', 'greeting', 'persuasion', 'voice' }
	for _, store in ipairs(stores) do
		for _, rec in pairs(core.dialogue[store].records) do
			print('MW3DS-GOLDEN dial\t' .. store .. '\t' .. rec.id .. '\t' .. clean(rec.questName))
			for n, i in ipairs(rec.infos) do
				local conds = {}
				for c, cond in ipairs(i.conditions or {}) do
					conds[c] = clean(table.concat({ tostring(cond.type), tostring(cond.operator), tostring(cond.value),
						tostring(cond.recordId), tostring(cond.variableName), tostring(cond.cellName) }, ':'))
				end
				print(table.concat({ 'MW3DS-GOLDEN info', store, rec.id, n, i.id, tostring(i.questStage),
					tostring(i.isQuestFinished), tostring(i.isQuestRestart), tostring(i.isQuestName),
					clean(i.filterActorId), clean(i.filterActorRace), clean(i.filterActorClass),
					clean(i.filterActorFaction), tostring(i.filterActorFactionRank), clean(i.filterPlayerCell),
					tostring(i.filterActorDisposition), clean(i.filterActorGender), clean(i.filterPlayerFaction),
					tostring(i.filterPlayerFactionRank), clean(i.sound), clean(i.text), clean(i.resultScript),
					table.concat(conds, ';') }, '\t'))
			end
		end
	end
end

local function dumpGlobals()
	for name, value in pairs(world.mwscript.getGlobalVariables()) do
		print('MW3DS-GOLDEN global\t' .. index .. '\t' .. name .. '\t' .. tostring(value))
	end
end

local function dumpObject(s, o, rayFrom)
	local p = o.position
	local isActor = types.Actor.objectIsInstance(o)
	local lock, locked, trap = '', '', ''
	if types.Lockable.objectIsInstance(o) then
		lock = types.Lockable.getLockLevel(o)
		locked = types.Lockable.isLocked(o) and 1 or 0
		local t = types.Lockable.getTrapSpell(o)
		trap = t and t.id or ''
	end
	print(table.concat({ 'MW3DS-GOLDEN obj', s.key, o.recordId, o.enabled and 1 or 0, isActor and 1 or 0,
		string.format('%.0f,%.0f,%.0f', p.x, p.y, p.z), string.format('%.3f', o.scale), o.count, tostring(lock),
		tostring(locked), trap, clean(o.owner.recordId), clean(o.owner.factionId),
		tostring(o.owner.factionRank or ''), o.contentFile and 1 or 0 }, '\t'))
	local script = world.mwscript.getLocalScript(o)
	if script then
		local vars = {}
		for name, value in pairs(script.variables) do
			vars[#vars + 1] = name .. '=' .. tostring(value)
		end
		table.sort(vars)
		print(table.concat({ 'MW3DS-GOLDEN sv', s.key, o.recordId, script.recordId, script.isRunning and 1 or 0,
			table.concat(vars, ';') }, '\t'))
	end
	if o.enabled and not isActor then
		rayFrom[#rayFrom + 1] = p
	end
end

local function dumpCell(s, player)
	local cell = player.cell
	local rayFrom = {}
	for _, o in ipairs(cell:getAll()) do
		dumpObject(s, o, rayFrom)
	end
	local rays = {}
	if s.interior then
		local step = math.max(1, math.floor(#rayFrom / kRaysPerCell))
		for i = 1, #rayFrom, step do
			rays[#rays + 1] = rayFrom[i]
		end
	else
		for gx = 0, kGrid - 1 do
			for gy = 0, kGrid - 1 do
				local at = util.vector3((s.x + (gx + 0.5) / kGrid) * 8192, (s.y + (gy + 0.5) / kGrid) * 8192, 0)
				local h = core.land.getHeightAt(at, cell)
				local tex = core.land.getTextureAt(at, cell)
				print(string.format('MW3DS-GOLDEN land\t%s\t%d,%d\t%.1f\t%s', s.key, gx, gy, h, clean(tex)))
				rays[#rays + 1] = util.vector3(at.x, at.y, h)
			end
		end
	end
	-- (an interior: from just above each object; an exterior: from high up, the top surface over the terrain)
	player:sendEvent('MW3DS_Rays', { key = s.key, points = rays, above = s.interior and 128 or 4096,
		below = s.interior and 1024 or 64 })
	for _, a in ipairs(world.activeActors) do
		if a ~= player and a.cell == cell then
			a:sendEvent('MW3DS_Report', { key = s.key, player = player })
		end
	end
end

local function nextCell(player)
	if index >= plan.start then
		print('MW3DS-GOLDEN cellend\t' .. index)
		if index % kGlobalsEvery == 0 then
			dumpGlobals()
		end
	end
	index = index + 1
	started, dumpedAt, frames = nil, nil, 0
	local s = plan.cells[index]
	if not s then
		dumpGlobals()
		print('MW3DS-GOLDEN done ' .. (index - 1))
		core.quit()
		return
	end
	print('MW3DS-GOLDEN goto\t' .. index .. '\t' .. s.key)
	local z = s.z
	local cell
	if s.interior then
		cell = world.getCellByName(s.cell)
	else
		cell = world.getExteriorCell(s.x, s.y)
		local h = core.land.getHeightAt(util.vector3((s.x + 0.5) * 8192, (s.y + 0.5) * 8192, 0), cell)
		z = math.max(h, 0) + 64
	end
	player:teleport(cell, util.vector3(s.px, s.py, z))
end

local function onUpdate()
	local player = world.players[1]
	if not player or not player.cell then
		return
	end
	if settle < 30 then
		settle = settle + 1
		return
	end
	if index < plan.start then
		world.setGameTimeScale(0)
		print('MW3DS-GOLDEN records ' .. #plan.cells)
		local rec = types.NPC.record(player)
		local attrs = {}
		for i, n in ipairs({ 'strength', 'intelligence', 'willpower', 'agility', 'speed', 'endurance', 'personality',
				'luck' }) do
			attrs[i] = types.Actor.stats.attributes[n](player).base
		end
		print(table.concat({ 'MW3DS-GOLDEN player', rec.race, rec.class, rec.isMale and 'male' or 'female',
			types.Player.getBirthSign(player) or '', tostring(world.mwscript.getGlobalScript('startup') ~= nil),
			table.concat(attrs, ',') }, '\t'))
		if plan.dialogue then
			dumpDialogue()
		end
		nextCell(player)
		return
	end
	local s = plan.cells[index]
	if not s then
		return
	end
	frames = frames + 1
	if not started then
		if frames >= 2 then
			started = core.getSimulationTime()
			print('MW3DS-GOLDEN cell\t' .. index .. '\t' .. s.key .. '\t' .. clean(player.cell.name))
		end
	elseif not dumpedAt then
		if frames >= kMinFrames and core.getSimulationTime() - started >= kMinSeconds then
			dumpCell(s, player)
			dumpedAt = frames
		end
	elseif frames >= dumpedAt + kReportFrames then
		nextCell(player)
	end
end

return { engineHandlers = { onUpdate = onUpdate } }
