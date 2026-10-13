-- MW3DS side by side: holds an animation group at a point on self (an NPC's local script or the player's). The group plays
-- from startKey to stopKey (the 3DS's CastSelf is spellcast's "self start" to "self stop") and stops at the point: full
-- speed, then a tenth of it for the last stretch (overshoot under 2 ms of animation at 60 fps). OpenMW 0.51's playBlended
-- leaves startPoint at 0 and its movement groups follow how fast the actor really moves, so this goes through playQueued
-- (MWScript's PlayGroup).
local anim = require('openmw.animation')
local core = require('openmw.core')
local self = require('openmw.self')

local kNear = 0.05     -- completion left when it slows down
local kSlow = 0.1

local M = {}

-- (a group played to its stop key ends there, completion short of 1, and goes back to idling: the last point is held
-- just before it, 4 ms of a 2 s cast)
local kLast = 0.998

-- h: { group, startKey, stopKey, at }
function M.start(h)
	anim.clearAnimationQueue(self, true)
	h.at = math.min(h.at, kLast)
	anim.playQueued(self, h.group, { startKey = h.startKey or 'start', stopKey = h.stopKey or 'stop',
		speed = h.at > 0 and 1 or 0, loops = 0 })
	h.stopped = h.at <= 0
	h.started = h.started or core.getRealTime()
end

-- Each frame: true once it holds (and the line saying where went to openmw.log)
function M.update(h)
	if not anim.isPlaying(self, h.group) then
		M.start(h)
	end
	if not h.stopped then
		local c = anim.getCompletion(self, h.group) or 0
		if core.getRealTime() - h.started > 25 then
			print(string.format('MW3DS-SBS %s: %s stuck at %.3f', self.recordId, h.group, c))
			anim.setSpeed(self, h.group, 0)
			h.stopped = true
		elseif c >= h.at then
			anim.setSpeed(self, h.group, 0)
			h.stopped = true
		elseif c >= h.at - kNear then
			anim.setSpeed(self, h.group, kSlow)
		end
	end
	if h.stopped and not h.reported then
		h.reported = true
		print(string.format('MW3DS-SBS %s holds %s (%s - %s) at %.3f, at %.1f %.1f %.1f', self.recordId, h.group,
			h.startKey or 'start', h.stopKey or 'stop', anim.getCompletion(self, h.group) or -1, self.position.x,
			self.position.y, self.position.z))
	end
	return h.stopped
end

return M
