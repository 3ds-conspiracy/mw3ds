-- MW3DS side by side, an actor's half: on MW3DS_SBS_Pose it stops its AI and holds an animation group at a point
-- (0 its start key, 1 its stop key). OpenMW 0.51's playBlended leaves startPoint at 0 here, so the group plays up to
-- the point and stops there: full speed, then a tenth of it for the last stretch (overshoot under 2 ms of animation at
-- 60 fps).
local anim = require('openmw.animation')
local core = require('openmw.core')
local self = require('openmw.self')
local I = require('openmw.interfaces')

local kNear = 0.05     -- completion left when it slows down
local kSlow = 0.1

local hold = nil

local function play()
	anim.clearAnimationQueue(self, true)
	-- (playQueued: MWScript's PlayGroup. A playBlended movement group follows how fast the actor really moves: 0)
	anim.playQueued(self, hold.group, { speed = hold.at > 0 and 1 or 0, loops = 0 })
	hold.stopped = hold.at <= 0
	hold.started = core.getRealTime()
end

local function posed()
	print(string.format('MW3DS-SBS %s holds %s at %.3f', self.recordId, tostring(hold.group),
		hold.group and (anim.getCompletion(self, hold.group) or -1) or -1))
	core.sendGlobalEvent('MW3DS_SBS_ActorPosed', { index = hold.index })
	hold.reported = true
end

return {
	eventHandlers = {
		MW3DS_SBS_Pose = function(d)
			I.AI.removePackages()
			hold = d
			if hold.group and not anim.hasGroup(self, hold.group) then
				print(string.format('MW3DS-SBS %s has no group %s', self.recordId, hold.group))
				hold.group = nil
			end
			if hold.group then
				play()
			else
				posed()
			end
		end,
	},
	engineHandlers = {
		onUpdate = function()
			if not hold then
				return
			end
			self.controls.movement = 0
			self.controls.sideMovement = 0
			self.controls.run = false
			if not hold.group then
				return
			end
			-- the character controller can drop a scripted group (a hit, a turn): put it back
			if not anim.isPlaying(self, hold.group) then
				play()
			end
			if not hold.stopped then
				local c = anim.getCompletion(self, hold.group) or 0
				if core.getRealTime() - hold.started > 10 then
					print(string.format('MW3DS-SBS %s: %s stuck at %.3f', self.recordId, hold.group, c))
					anim.setSpeed(self, hold.group, 0)
					hold.stopped = true
				elseif c >= hold.at then
					anim.setSpeed(self, hold.group, 0)
					hold.stopped = true
				elseif c >= hold.at - kNear then
					anim.setSpeed(self, hold.group, kSlow)
				end
			end
			if hold.stopped and not hold.reported then
				posed()
			end
		end,
	},
}
