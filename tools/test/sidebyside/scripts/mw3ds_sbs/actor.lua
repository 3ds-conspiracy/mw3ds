-- MW3DS side by side, an actor's half: on MW3DS_SBS_Pose it stops its AI and holds an animation group at a point
-- (hold.lua), then tells the world.
local anim = require('openmw.animation')
local core = require('openmw.core')
local self = require('openmw.self')
local types = require('openmw.types')
local I = require('openmw.interfaces')
local hold = require('scripts.mw3ds_sbs.hold')

local h = nil

return {
	eventHandlers = {
		-- a scene step: this group played as the game would (the Heart of Lorkhan struck)
		MW3DS_SBS_Kill = function()
			types.Actor.stats.dynamic.health(self).current = 0
		end,
		MW3DS_SBS_Play = function(d)
			anim.playQueued(self, d.group, {})
		end,
		-- a scene step: it goes for the player (d.target) on its own AI
		MW3DS_SBS_Attack = function(d)
			I.AI.removePackages()
			I.AI.startPackage({ type = 'Combat', target = d.target })
		end,
		-- a scene step: calm (Fight 0: no turning on the player) and walking to d.pos on its own AI
		MW3DS_SBS_Travel = function(d)
			types.Actor.stats.ai.fight(self).base = 0
			I.AI.removePackages()
			I.AI.startPackage({ type = 'Travel', destPosition = d.pos })
		end,
		MW3DS_SBS_Pose = function(d)
			I.AI.removePackages()
			h = d
			-- (one with no animation at all, a disabled actor such as Glenmoril's raven before its quest, stands as it is:
			-- an error here stopped the shots after it)
			local ok, has = pcall(anim.hasGroup, self, h.group or '')
			if h.group and not (ok and has) then
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
