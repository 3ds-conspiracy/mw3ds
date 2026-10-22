-- MW3DS blood demo, an NPC's half: the target stands still until struck.
local self = require('openmw.self')
local I = require('openmw.interfaces')

local hold = false

return {
	eventHandlers = {
		MW3DS_CD_Hold = function()
			I.AI.removePackages()
			hold = true
		end,
	},
	engineHandlers = {
		onUpdate = function()
			if hold then
				self.controls.movement = 0
				self.controls.sideMovement = 0
			end
		end,
	},
}
