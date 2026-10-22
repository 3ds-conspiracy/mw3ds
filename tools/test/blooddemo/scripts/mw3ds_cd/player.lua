-- MW3DS blood demo, the player's half: the dagger in the right hand, weapon out, one blow (the attack held half a second, then let go).
local self = require('openmw.self')
local types = require('openmw.types')
local I = require('openmw.interfaces')

local job = nil
local frames = 0

return {
	eventHandlers = {
		MW3DS_CD_Swing = function(d)
			job = d
			frames = 0
		end,
	},
	engineHandlers = {
		onFrame = function()
			I.UI.setHudVisibility(false)
			if I.UI.getMode() then
				I.UI.setMode()
			end
			if not job then
				return
			end
			frames = frames + 1
			if frames == 2 then
				-- a blow that cannot miss (the 3DS test harness lands every one): skill, Agility and Luck at 100
				types.NPC.stats.skills.shortblade(self).base = 100
				types.NPC.stats.skills.longblade(self).base = 100
				types.Actor.stats.attributes.agility(self).base = 100
				types.Actor.stats.attributes.luck(self).base = 100
				types.Actor.stats.attributes.strength(self).base = 60
			end
			if frames == 5 then
				local eq = types.Actor.getEquipment(self)
				eq[types.Actor.EQUIPMENT_SLOT.CarriedRight] = types.Actor.inventory(self):find('iron dagger')
				types.Actor.setEquipment(self, eq)
			elseif frames == 15 then
				types.Actor.setStance(self, types.Actor.STANCE.Weapon)
			elseif frames == 90 then
				self.controls.use = 1
				print('MW3DS-CD swing')
			elseif frames == 150 then
				self.controls.use = 0
			end
		end,
	},
}
