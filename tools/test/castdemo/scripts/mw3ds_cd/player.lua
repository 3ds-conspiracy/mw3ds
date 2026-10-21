-- MW3DS cast demo, the player's half: skills up so the cast succeeds, the made spell selected, the camera on the target, Use.
local self = require('openmw.self')
local types = require('openmw.types')
local camera = require('openmw.camera')
local I = require('openmw.interfaces')

local job = nil
local frames = 0

return {
	eventHandlers = {
		MW3DS_CD_Cast = function(d)
			job = d
			frames = 0
			for _, k in ipairs({ 'destruction', 'alteration', 'illusion', 'conjuration', 'mysticism', 'restoration' }) do
				types.NPC.stats.skills[k](self).base = 100
			end
			types.Actor.stats.attributes.willpower(self).base = 100
			types.Actor.stats.attributes.luck(self).base = 100
			types.Actor.stats.dynamic.magicka(self).current = 1000
			types.Actor.setSelectedSpell(self, d.spell)
			types.Actor.setStance(self, types.Actor.STANCE.Spell)
		end,
	},
	engineHandlers = {
		onFrame = function()
			I.UI.setHudVisibility(false)
			-- (a guard's arrest opens its dialogue: closed, so the run goes on)
			if I.UI.getMode() then
				I.UI.setMode()
			end
			if not job then
				return
			end
			frames = frames + 1
			camera.setMode(camera.MODE.FirstPerson)
			camera.setYaw(job.yaw)
			camera.setPitch(0)
			if frames >= 5 and frames < 55 and types.Actor.getStance(self) ~= types.Actor.STANCE.Spell then
				types.Actor.setSelectedSpell(self, job.spell)
				types.Actor.setStance(self, types.Actor.STANCE.Spell)
			end
			if frames == 54 then
				local sel = types.Actor.getSelectedSpell(self)
				print(string.format('MW3DS-CD stance %s selected %s', tostring(types.Actor.getStance(self)), sel and sel.id or 'none'))
			end
			if frames == 60 then
				self.controls.use = 1
				print(string.format('MW3DS-CD cast magicka %.0f yaw self %.2f camera %.2f want %.2f pitch %.2f', types.Actor.stats.dynamic.magicka(self).current, self.rotation:getYaw(), camera.getYaw(), job.yaw, camera.getPitch()))
			elseif frames == 62 then
				self.controls.use = 0
			end
		end,
	},
}
