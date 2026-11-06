-- MW3DS golden tour, an actor's half: when asked, one line of what involves no dice: its AI package and combat targets,
-- what scripts left its AI settings at, its pools, speeds, load and barter gold, and its disposition to the player.
local self = require('openmw.self')
local types = require('openmw.types')
local I = require('openmw.interfaces')

local function report(d)
	local p = I.AI.getActivePackage()
	local foes = {}
	for i, o in ipairs(I.AI.getTargets('Combat')) do
		foes[i] = o.recordId
	end
	table.sort(foes)
	local ai, dyn = types.Actor.stats.ai, types.Actor.stats.dynamic
	local disp = ''
	if types.NPC.objectIsInstance(self) then
		disp = types.NPC.getDisposition(self, d.player)
	end
	print(table.concat({ 'MW3DS-GOLDEN actor', d.key, self.recordId, p and p.type or '', table.concat(foes, ','),
		tostring(disp), string.format('%.0f,%.0f,%.0f,%.0f', ai.fight(self).modified, ai.flee(self).modified,
			ai.alarm(self).modified, ai.hello(self).modified),
		string.format('%.0f,%.0f,%.0f', dyn.health(self).base, dyn.magicka(self).base, dyn.fatigue(self).base),
		string.format('%.2f,%.2f', types.Actor.getRunSpeed(self), types.Actor.getWalkSpeed(self)),
		string.format('%.2f,%.2f', types.Actor.getCapacity(self), types.Actor.getEncumbrance(self)),
		tostring(types.Actor.getBarterGold(self)), types.Actor.isDead(self) and 1 or 0 }, '\t'))
end

return { eventHandlers = { MW3DS_Report = report } }
