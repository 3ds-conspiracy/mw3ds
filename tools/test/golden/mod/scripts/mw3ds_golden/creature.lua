-- MW3DS golden run, a creature's half: its numbers as OpenMW has them, one line, then tells the world.
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
	local rec = types.Creature.record(self)
	local attack = {}   -- (rec.attack is a read-only list, not a table)
	for i = 1, 6 do
		attack[i] = rec.attack[i]
	end
	local f = dump.common()
	print('MW3DS-GOLDEN creature\t' .. self.recordId .. '\t' .. table.concat({ f[1], f[2], f[3], f[4], f[5], f[6],
		f[7], f[8], f[9], rec.soulValue, table.concat(attack, ',') }, '\t'))
	-- its spells (castable ones, abilities, diseases), one line each with the type
	for _, sp in pairs(types.Actor.spells(self)) do
		print('MW3DS-GOLDEN cspell\t' .. self.recordId .. '\t' .. sp.id .. '\t' .. tostring(sp.type))
	end
	core.sendGlobalEvent('MW3DS_GOLDEN_Done', { id = self.recordId })
end

return { engineHandlers = { onUpdate = onUpdate } }
