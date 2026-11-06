-- MW3DS golden run, an NPC's half: a few frames after it exists, prints what OpenMW worked out for it (an
-- autocalculated NPC: from its race, class and level), one line, then tells the world.
local core = require('openmw.core')
local self = require('openmw.self')
local types = require('openmw.types')
local dump = require('scripts.mw3ds_golden.dump')

local kFrames = 4
local frames = 0
local done = false

local function onUpdate()
	if done then
		return
	end
	frames = frames + 1
	if frames < kFrames then
		return
	end
	done = true
	local s = {}
	for i, n in ipairs(dump.skills) do
		s[i] = types.NPC.stats.skills[n](self).base
	end
	local rec = types.NPC.record(self)
	local f = dump.common()
	print('MW3DS-GOLDEN npc\t' .. self.recordId .. '\t' .. table.concat({ rec.isAutocalc and 1 or 0, f[1], f[2],
		table.concat(s, ','), f[3], f[4], f[5], f[6], f[7], f[8], f[9], rec.baseGold }, '\t'))
	-- what it has on after OpenMW filled its inventory and equipped it (slot number, item), its factions' ranks
	-- (counted from 1) and its reputation
	for slot, item in pairs(types.Actor.getEquipment(self)) do
		print('MW3DS-GOLDEN equip\t' .. self.recordId .. '\t' .. tostring(slot) .. '\t' .. item.recordId)
	end
	for _, f in ipairs(types.NPC.getFactions(self)) do
		print('MW3DS-GOLDEN faction\t' .. self.recordId .. '\t' .. f .. '\t' .. tostring(types.NPC.getFactionRank(self, f)))
	end
	print('MW3DS-GOLDEN rep\t' .. self.recordId .. '\t' .. tostring(types.NPC.stats.reputation(self).current))
	-- its spells (the record's, an autocalc NPC's picks, its race's powers and abilities), one line each with the type
	for _, sp in pairs(types.Actor.spells(self)) do
		print('MW3DS-GOLDEN spell\t' .. self.recordId .. '\t' .. sp.id .. '\t' .. tostring(sp.type))
	end
	core.sendGlobalEvent('MW3DS_GOLDEN_Done', { id = self.recordId })
end

return { engineHandlers = { onUpdate = onUpdate } }
