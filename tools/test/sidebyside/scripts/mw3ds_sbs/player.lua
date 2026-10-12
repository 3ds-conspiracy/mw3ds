-- MW3DS side by side, the player's half: runs the shots in poses.lua one after another. Each shot: the world sets
-- itself up (global.lua), the camera goes to the shot's place (static: the player isn't in it), the HUD is hidden,
-- and once things settle "MW3DS-SBS ready <n>" goes to openmw.log. sidebyside.py then presses F12 (the screenshot key)
-- while the shot holds (kHoldSecs); the next shot follows. "MW3DS-SBS done" after the last.
-- Angles as in the 3DS engine: yaw 0 looks along +Y, 90 along +X; pitch up is positive (OpenMW's is down).
local camera = require('openmw.camera')
local core = require('openmw.core')
local input = require('openmw.input')
local util = require('openmw.util')
local I = require('openmw.interfaces')
local poses = require('scripts.mw3ds_sbs.poses')

local kStartFrames = 60      -- after the game loads, before the first shot
local kSettleFrames = 90     -- after a shot is posed: textures, the actor's pose, the sky
local kHoldSecs = 3.0        -- a ready shot holds this long (real time) for sidebyside.py's F12, unless F12 comes first
local kAfterShotFrames = 20  -- after F12, before the next shot moves the camera

local index = 0
local state = 'start'
local frames = 0
local posed = false
local readyAt = 0

local function placeCamera()
	local s = poses[index]
	camera.setMode(camera.MODE.Static)
	camera.setStaticPosition(util.vector3(s.x, s.y, s.z))
	camera.setYaw(s.yaw)
	camera.setPitch(-s.pitch)
	camera.showCrosshair(false)
end

local function nextShot()
	index = index + 1
	frames = 0
	posed = false
	if index > #poses then
		state = 'done'
		print('MW3DS-SBS done')
		return
	end
	state = 'setup'
	core.sendGlobalEvent('MW3DS_SBS_Setup', { index = index })
end

return {
	eventHandlers = {
		MW3DS_SBS_Posed = function(d)
			if d.index == index then
				posed = true
				frames = 0
			end
		end,
	},
	engineHandlers = {
		onFrame = function()
			frames = frames + 1
			if state == 'start' then
				I.UI.setHudVisibility(false)
				if frames >= kStartFrames then
					nextShot()
				end
			elseif state == 'setup' or state == 'ready' then
				placeCamera()
				-- a message box or menu left open would be in the shot
				if I.UI.getMode() then
					I.UI.setMode()
				end
				if state == 'setup' and posed and frames >= kSettleFrames then
					state = 'ready'
					readyAt = core.getRealTime()
					print(string.format('MW3DS-SBS ready %d %s', index, poses[index].name))
				elseif state == 'ready' and core.getRealTime() - readyAt >= kHoldSecs then
					state = 'shot'
					frames = 0
				end
			elseif state == 'shot' then
				placeCamera()
				if frames >= kAfterShotFrames then
					nextShot()
				end
			end
		end,
		onKeyPress = function(key)
			if key.code == input.KEY.F12 and state == 'ready' then
				state = 'shot'
				frames = 0
			end
		end,
	},
}
