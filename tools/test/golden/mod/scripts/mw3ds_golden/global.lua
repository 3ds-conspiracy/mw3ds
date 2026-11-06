-- MW3DS golden run, the world's half (OpenMW 0.51). Makes each NPC record, then each creature record, of the loaded
-- data in turn next to the player; npc.lua / creature.lua dump what OpenMW worked out and send MW3DS_GOLDEN_Done. Prints "MW3DS-GOLDEN done <n>" at
-- the end and quits. Written for tools/test/golden_openmw.py.
local core = require('openmw.core')
local types = require('openmw.types')
local world = require('openmw.world')
local util = require('openmw.util')

local kTimeout = 120    -- frames to wait for one NPC's dump before skipping it
local ids = nil
local nextIndex = 1
local current = nil     -- the object being dumped
local waited = 0
local settle = 0        -- frames the world has had since the player appeared

local function bit(b)
	return b and 1 or 0
end

-- A cell as golden_compare.py names it: an interior by its name, an exterior by its grid
local function cellKey(cell)
	if cell == nil then
		return ''
	end
	if cell.isExterior then
		return 'ext ' .. cell.gridX .. ',' .. cell.gridY
	end
	return string.lower(cell.name or '')
end

-- Each cell's objects: how many of each record stand in it, and each door with where it leads. Returns true once all
-- cells are done
local kCellsPerFrame = 25
local cellIndex = 1
local function dumpCells(n)
	local cells = world.cells
	for _ = 1, n do
		if cellIndex > #cells then
			return true
		end
		local cell = cells[cellIndex]
		cellIndex = cellIndex + 1
		local key = cellKey(cell)
		local counts = {}
		for _, o in ipairs(cell:getAll()) do
			counts[o.recordId] = (counts[o.recordId] or 0) + 1
			if types.Door.objectIsInstance(o) and types.Door.isTeleport(o) then
				local p, d = o.position, types.Door.destPosition(o)
				print(string.format('MW3DS-GOLDEN door\t%s\t%s\t%.0f,%.0f,%.0f\t%s\t%.0f,%.0f,%.0f', key, o.recordId, p.x,
					p.y, p.z, cellKey(types.Door.destCell(o)), d.x, d.y, d.z))
			end
		end
		for id, c in pairs(counts) do
			print('MW3DS-GOLDEN cellobj\t' .. key .. '\t' .. id .. '\t' .. c)
		end
	end
	return cellIndex > #cells
end

-- The records' numbers as OpenMW has them after loading (its hard-coded flags included): each magic effect's
-- school, base cost and flags, one line each
local function dumpRecords()
	-- the game settings the engine reads (gmst_names.lua, written by golden_openmw.py from the engine's source)
	local ok, names = pcall(require, 'scripts.mw3ds_golden.gmst_names')
	if ok then
		for _, name in ipairs(names) do
			print('MW3DS-GOLDEN gmst\t' .. name .. '\t' .. tostring(core.getGMST(name)))
		end
	end
	for key, e in pairs(core.magic.effects.records) do
		print('MW3DS-GOLDEN effect\t' .. tostring(key) .. '\t' .. e.id .. '\t' .. table.concat({ e.school or '',
			e.baseCost, bit(e.hasDuration), bit(e.hasMagnitude), bit(e.harmful), bit(e.continuousVfx), bit(e.onSelf),
			bit(e.onTouch), bit(e.onTarget), bit(e.isAppliedOnce), bit(e.allowsSpellmaking), bit(e.allowsEnchanting),
			bit(e.negativeLight), bit(e.hasSkill), bit(e.hasAttribute), bit(e.casterLinked), bit(e.nonRecastable),
			bit(e.unreflectable) }, '\t'))
	end
	-- items: the numbers each kind's record carries, as key=value pairs (golden_compare.py items maps them)
	local fields = {
		Weapon = { 'value', 'weight', 'health', 'speed', 'reach', 'enchantCapacity', 'type', 'isMagical', 'isSilver',
			'chopMinDamage', 'chopMaxDamage', 'slashMinDamage', 'slashMaxDamage', 'thrustMinDamage', 'thrustMaxDamage' },
		Armor = { 'value', 'weight', 'health', 'baseArmor', 'enchantCapacity', 'type' },
		Clothing = { 'value', 'weight', 'enchantCapacity', 'type' },
		Book = { 'value', 'weight', 'enchantCapacity', 'isScroll', 'skill' },
		Light = { 'value', 'weight', 'duration', 'radius' },
		Lockpick = { 'value', 'weight', 'quality', 'maxCondition' },
		Probe = { 'value', 'weight', 'quality', 'maxCondition' },
		Repair = { 'value', 'weight', 'quality', 'maxCondition' },
		Apparatus = { 'value', 'weight', 'quality', 'type' },
		Miscellaneous = { 'value', 'weight', 'isKey' },
		Potion = { 'value', 'weight' },
		Ingredient = { 'value', 'weight' },
		Container = { 'weight', 'isOrganic', 'isRespawning' },
	}
	for kind, names in pairs(fields) do
		for _, rec in ipairs(types[kind].records) do
			local kv = {}
			for _, n in ipairs(names) do
				local v = rec[n]
				if type(v) == 'boolean' then
					v = bit(v)
				end
				kv[#kv + 1] = n .. '=' .. tostring(v)
			end
			print('MW3DS-GOLDEN item\t' .. kind .. '\t' .. rec.id .. '\t' .. table.concat(kv, ';'))
		end
	end
	-- potions' and ingredients' effects: effect, skill, attribute, magnitude min / max, duration, range, area
	for _, kind in ipairs({ 'Potion', 'Ingredient' }) do
		for _, rec in ipairs(types[kind].records) do
			local list = {}
			for _, e in ipairs(rec.effects) do
				list[#list + 1] = table.concat({ e.id, e.affectedSkill or '', e.affectedAttribute or '', e.magnitudeMin or 0,
					e.magnitudeMax or 0, e.duration or 0, e.range or 0, e.area or 0 }, ',')
			end
			print('MW3DS-GOLDEN itemfx\t' .. kind .. '\t' .. rec.id .. '\t' .. table.concat(list, ';'))
		end
	end
	-- spells and enchantments: type, cost, charge / flags, and effects as the potions' above
	local function effectList(effects)
		local list = {}
		for _, e in ipairs(effects) do
			list[#list + 1] = table.concat({ e.id, e.affectedSkill or '', e.affectedAttribute or '', e.magnitudeMin or 0,
				e.magnitudeMax or 0, e.duration or 0, e.range or 0, e.area or 0 }, ',')
		end
		return table.concat(list, ';')
	end
	for _, sp in pairs(core.magic.spells.records) do
		print('MW3DS-GOLDEN spellrec\t' .. sp.id .. '\t' .. table.concat({ sp.type, sp.cost, bit(sp.isAutocalc),
			bit(sp.starterSpellFlag), bit(sp.alwaysSucceedFlag) }, ',') .. '\t' .. effectList(sp.effects))
	end
	for _, en in pairs(core.magic.enchantments.records) do
		print('MW3DS-GOLDEN enchrec\t' .. en.id .. '\t' .. table.concat({ en.type, en.cost, en.charge, bit(en.isAutocalc) },
			',') .. '\t' .. effectList(en.effects))
	end
	-- factions: favoured attributes and skills, reactions, each rank's requirements
	-- (attribute 1, attribute 2, primary skill, favoured skill, reputation)
	local function join(list, sep)
		local out = {}
		for _, v in ipairs(list) do
			out[#out + 1] = tostring(v)
		end
		return table.concat(out, sep or ',')
	end
	for _, f in pairs(core.factions.records) do
		local reactions = {}
		for other, v in pairs(f.reactions) do
			reactions[#reactions + 1] = other .. ':' .. v
		end
		local ranks = {}
		for _, r in ipairs(f.ranks) do
			ranks[#ranks + 1] = join({ r.attributeValues[1], r.attributeValues[2], r.primarySkillValue,
				r.favouredSkillValue, r.factionReputation }, ':')
		end
		print('MW3DS-GOLDEN factionrec\t' .. f.id .. '\t' .. join(f.attributes) .. '\t' .. join(f.skills) .. '\t'
			.. table.concat(reactions, ',') .. '\t' .. table.concat(ranks, ','))
	end
	-- classes and races
	for _, c in ipairs(types.NPC.classes.records) do
		print('MW3DS-GOLDEN classrec\t' .. c.id .. '\t' .. c.specialization .. '\t' .. join(c.attributes) .. '\t'
			.. join(c.majorSkills) .. '\t' .. join(c.minorSkills) .. '\t' .. bit(c.isPlayable))
	end
	for _, r in ipairs(types.NPC.races.records) do
		local skills, attrs = {}, {}
		for k, v in pairs(r.skills) do
			skills[#skills + 1] = k .. ':' .. v
		end
		for k, v in pairs(r.attributes) do
			attrs[#attrs + 1] = k .. ':' .. v.male .. ':' .. v.female
		end
		print('MW3DS-GOLDEN racerec\t' .. r.id .. '\t' .. bit(r.isPlayable) .. '\t' .. bit(r.isBeast) .. '\t'
			.. table.concat(skills, ',') .. '\t' .. table.concat(attrs, ',') .. '\t' .. r.height.male .. ':'
			.. r.height.female .. '\t' .. r.weight.male .. ':' .. r.weight.female .. '\t' .. join(r.spells))
	end
	-- journal: each quest's stages, finished / restart flags
	for _, d in pairs(core.dialogue.journal.records) do
		for _, info in ipairs(d.infos) do
			if not info.isQuestName then
				print('MW3DS-GOLDEN jinfo\t' .. d.id .. '\t' .. tostring(info.questStage) .. '\t'
					.. bit(info.isQuestFinished) .. '\t' .. bit(info.isQuestRestart))
			end
		end
	end
	-- regions: weather chances, sleep list
	for _, r in pairs(core.regions.records) do
		local w = {}
		for k, v in pairs(r.weatherProbabilities) do
			w[#w + 1] = k .. ':' .. v
		end
		print('MW3DS-GOLDEN regionrec\t' .. r.id .. '\t' .. table.concat(w, ',') .. '\t' .. tostring(r.sleepList or ''))
	end
	-- birthsigns: their spells
	for _, rec in ipairs(types.Player.birthSigns.records) do
		local spells = {}
		for _, sp in ipairs(rec.spells) do
			spells[#spells + 1] = sp
		end
		print('MW3DS-GOLDEN sign\t' .. rec.id .. '\t' .. table.concat(spells, ','))
	end
	-- creature leveled lists: the record, then at each of these player levels every creature kSamples picks gave
	-- (OpenMW's own getRandomId, nested lists followed; "" when none comes)
	local kLevels = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 18, 21, 25, 30 }
	local kSamples = 150
	for _, rec in ipairs(types.LevelledCreature.records) do
		local entries = {}
		for _, c in ipairs(rec.creatures) do
			entries[#entries + 1] = c.id .. ':' .. c.level
		end
		print('MW3DS-GOLDEN levrec\t' .. rec.id .. '\t' .. tostring(rec.chanceNone) .. '\t'
			.. bit(rec.calculateFromAllLevels) .. '\t' .. table.concat(entries, ','))
		for _, level in ipairs(kLevels) do
			local seen, list = {}, {}
			for _ = 1, kSamples do
				local ok, id = pcall(function() return rec:getRandomId(level) end)
				if not ok then
					print('MW3DS-GOLDEN error getRandomId ' .. tostring(id))
					return
				end
				id = tostring(id or '')
				if not seen[id] then
					seen[id] = true
					list[#list + 1] = id
				end
			end
			print('MW3DS-GOLDEN levpick\t' .. rec.id .. '\t' .. level .. '\t' .. table.concat(list, ','))
		end
	end
end

local function removeCurrent()
	if current and current:isValid() then
		current:remove()
	end
	current = nil
end

local function onUpdate()
	local player = world.players[1]
	if not player or not player.cell then
		return
	end
	settle = settle + 1
	if settle < 30 then
		return
	end
	if not ids then
		dumpRecords()
		ids = {}
		for _, rec in ipairs(types.NPC.records) do
			ids[#ids + 1] = rec.id
		end
		for _, rec in ipairs(types.Creature.records) do
			ids[#ids + 1] = rec.id
		end
		print(string.format('MW3DS-GOLDEN records %d', #ids))
	end
	if current then
		waited = waited + 1
		if waited > kTimeout then
			print(string.format('MW3DS-GOLDEN skipped %s', current.recordId))
			removeCurrent()
		end
		return
	end
	if nextIndex > #ids then
		-- then every cell, kCellsPerFrame a frame
		if dumpCells(kCellsPerFrame) then
			print(string.format('MW3DS-GOLDEN done %d', #ids))
			core.quit()
		end
		return
	end
	local id = ids[nextIndex]
	nextIndex = nextIndex + 1
	waited = 0
	current = world.createObject(id, 1)
	current:teleport(player.cell, player.position + util.vector3(300, 0, 0))
end

return {
	engineHandlers = { onUpdate = onUpdate },
	eventHandlers = {
		MW3DS_GOLDEN_Done = function(d)
			if current and current.recordId == d.id then
				removeCurrent()
			end
		end,
	},
}
