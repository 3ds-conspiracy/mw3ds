-- MW3DS golden run, what NPCs and creatures share: the numbers read off an actor, as one tab-separated line.
local self = require('openmw.self')
local types = require('openmw.types')

local M = {}

M.attributes = { 'strength', 'intelligence', 'willpower', 'agility', 'speed', 'endurance', 'personality', 'luck' }
M.skills = { 'block', 'armorer', 'mediumarmor', 'heavyarmor', 'bluntweapon', 'longblade', 'axe', 'spear',
	'athletics', 'enchant', 'destruction', 'alteration', 'illusion', 'conjuration', 'mysticism', 'restoration',
	'alchemy', 'unarmored', 'security', 'sneak', 'acrobatics', 'lightarmor', 'shortblade', 'marksman', 'mercantile',
	'speechcraft', 'handtohand' }

-- Pools are base + modifier: OpenMW keeps an NPC's magicka in the modifier (NpcStats::recalculateMagicka)
local function pool(stat)
	return stat.base + stat.modifier
end

-- level, attributes, health, magicka, fatigue, then the AI settings fight, flee, alarm, hello
function M.common()
	local a = {}
	for i, n in ipairs(M.attributes) do
		a[i] = types.Actor.stats.attributes[n](self).base
	end
	local dyn, ai = types.Actor.stats.dynamic, types.Actor.stats.ai
	return { types.Actor.stats.level(self).current, table.concat(a, ','), pool(dyn.health(self)),
		pool(dyn.magicka(self)), pool(dyn.fatigue(self)), ai.fight(self).base, ai.flee(self).base,
		ai.alarm(self).base, ai.hello(self).base }
end

return M
