-- MW3DS golden tour, the player's half: straight-down rays where it is told, what the world's collision says is under
-- each point (actors left out, they move), and any menu a script opened is closed so the tour goes on.
local nearby = require('openmw.nearby')
local util = require('openmw.util')
local I = require('openmw.interfaces')

local function rays(d)
	local kinds = nearby.COLLISION_TYPE.World + nearby.COLLISION_TYPE.Door + nearby.COLLISION_TYPE.HeightMap
	for _, p in ipairs(d.points) do
		local r = nearby.castRay(p + util.vector3(0, 0, d.above), p + util.vector3(0, 0, -d.below),
			{ collisionType = kinds })
		print(string.format('MW3DS-GOLDEN ray\t%s\t%.0f,%.0f,%.0f\t%s\t%s', d.key, p.x, p.y, p.z,
			r.hit and string.format('%.1f', r.hitPos.z) or '', r.hitObject and r.hitObject.recordId or ''))
	end
end

local function onFrame()
	local mode = I.UI.getMode()
	if mode then
		print('MW3DS-GOLDEN menu\t' .. tostring(mode))
		I.UI.setMode()
	end
end

return { engineHandlers = { onFrame = onFrame }, eventHandlers = { MW3DS_Rays = rays } }
