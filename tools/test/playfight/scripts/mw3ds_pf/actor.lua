-- MW3DS play fight, a fighter's half: on MW3DS_PF_Fight it goes for the target on OpenMW's own AI, and every frame after
-- that logs where it is, which way it faces, the animation it plays and its health (the "trace:" lines of the 3DS side)
local anim = require('openmw.animation')
local core = require('openmw.core')
local self = require('openmw.self')
local types = require('openmw.types')
local I = require('openmw.interfaces')

local on = false
local t0 = 0
local function group(bg)
	local g = anim.getActiveGroup(self, bg)
	return (g and g ~= '') and g or '-'
end


return {
	eventHandlers = {
		MW3DS_PF_Fight = function(d)
			I.AI.removePackages()
			I.AI.startPackage({ type = 'Combat', target = d.target })
			on = true
			t0 = core.getSimulationTime()
		end,
	},
	engineHandlers = {
		onUpdate = function()
			if not on then
				return
			end
			local p = self.position
			print(string.format('MW3DS-PF trace %s %.3f %.2f %.2f %.2f %.4f %s %s %.0f', self.recordId,
				core.getSimulationTime() - t0, p.x, p.y, p.z, self.rotation:getYaw(), group(anim.BONE_GROUP.LowerBody), group(anim.BONE_GROUP.Torso),
				types.Actor.stats.dynamic.health(self).current))
		end,
	},
}
