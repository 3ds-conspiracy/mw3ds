-- MW3DS cast demo, an NPC's half: a guard says what its AI package is whenever that changes (Pursue, Combat, ...)
local self = require('openmw.self')
local types = require('openmw.types')
local core = require('openmw.core')
local I = require('openmw.interfaces')

local last = nil
local timer = 0
local hold = false     -- the target stands still until the spell has landed (the 3DS replay's Dralas stood too)

return {
	eventHandlers = {
		MW3DS_CD_Hold = function()
			I.AI.removePackages()
			hold = true
		end,
		MW3DS_CD_Release = function()
			hold = false
		end,
	},
	engineHandlers = {
		onUpdate = function(dt)
			if hold then
				self.controls.movement = 0
				self.controls.sideMovement = 0
			end
			timer = timer + dt
			if timer < 0.2 then
				return
			end
			timer = 0
			if types.NPC.record(self).class ~= 'guard' then
				return
			end
			local p = I.AI.getActivePackage()
			local now = p and p.type or 'none'
			if now ~= last then
				last = now
				print(string.format('MW3DS-CD guard %s %s at %.2f', (self.recordId:gsub(' ', '_')), now, core.getSimulationTime()))
			end
		end,
	},
}
