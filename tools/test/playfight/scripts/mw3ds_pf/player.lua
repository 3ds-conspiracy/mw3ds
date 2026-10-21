-- MW3DS play fight, the player's half: stands still (no input) with the fight's health, and logs its health every frame
-- once the fight began (same fields as the fighter's lines; the fighter's lines are the ones judged)
local camera = require('openmw.camera')
local core = require('openmw.core')
local util = require('openmw.util')
local self = require('openmw.self')
local types = require('openmw.types')
local I = require('openmw.interfaces')

local on = false
local t0 = 0
local retreat = 0       -- the player backs away at this fraction of full speed (walking)
local cam = nil     -- { x, y, z, yaw, pitch }: a static camera for the fight (degrees on the python side, radians here)

return {
	eventHandlers = {
		MW3DS_PF_Prepare = function(d)
			local h = types.Actor.stats.dynamic.health(self)
			h.base = d.health
			h.current = d.health
			cam = d.cam
			retreat = d.retreat or 0
			on = true
			t0 = core.getSimulationTime()
		end,
	},
	engineHandlers = {
		onFrame = function()
			I.UI.setHudVisibility(false)
			if not on then
				return
			end
			self.controls.run = false
			self.controls.movement = -retreat
			if cam then
				-- (angles as in the 3DS engine: yaw 0 looks along +Y, pitch up positive; OpenMW's pitch is down)
				camera.setMode(camera.MODE.Static)
				-- (follow: cam is an offset from the player's feet, in world axes; else a place)
				local at = cam.follow and self.position or util.vector3(0, 0, 0)
				camera.setStaticPosition(util.vector3(cam.x + at.x, cam.y + at.y, cam.z + at.z))
				camera.setYaw(cam.yaw)
				camera.setPitch(-cam.pitch)
				camera.showCrosshair(false)
			end
			local p = self.position
			print(string.format('MW3DS-PF trace player %.3f %.2f %.2f %.2f %.4f - - %.0f', core.getSimulationTime() - t0,
				p.x, p.y, p.z, self.rotation:getYaw(), types.Actor.stats.dynamic.health(self).current))
		end,
	},
}
