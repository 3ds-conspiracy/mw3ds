-- MW3DS side by side, an actor's half: on MW3DS_SBS_Pose it stops its AI and holds an animation group at a point
-- (hold.lua), then tells the world.
local anim = require('openmw.animation')
local core = require('openmw.core')
local self = require('openmw.self')
local I = require('openmw.interfaces')
local hold = require('scripts.mw3ds_sbs.hold')

local h = nil

return {
	eventHandlers = {
		MW3DS_SBS_Pose = function(d)
			I.AI.removePackages()
			h = d
			if h.group and not anim.hasGroup(self, h.group) then
				print(string.format('MW3DS-SBS %s has no group %s', self.recordId, h.group))
				h.group = nil
			end
			if h.group then
				hold.start(h)
			else
				core.sendGlobalEvent('MW3DS_SBS_ActorPosed', { index = h.index })
			end
		end,
	},
	engineHandlers = {
		onUpdate = function()
			if not h then
				return
			end
			self.controls.movement = 0
			self.controls.sideMovement = 0
			self.controls.run = false
			if h.group and not h.told and hold.update(h) then
				h.told = true
				core.sendGlobalEvent('MW3DS_SBS_ActorPosed', { index = h.index })
			end
		end,
	},
}
