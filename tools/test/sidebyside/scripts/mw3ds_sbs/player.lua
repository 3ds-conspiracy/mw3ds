-- MW3DS side by side, the player's half: runs the shots in poses.lua one after another. Each shot: the world sets
-- itself up (global.lua), the camera goes to the shot's place (static, the player out of the picture, or first person
-- for an fp shot: the player stands there with a spell readied and their own arms hold the group), the HUD is hidden,
-- and once things settle "MW3DS-SBS ready <n>" goes to openmw.log. sidebyside.py then presses F12 (the screenshot key)
-- while the shot holds (kHoldSecs); the next shot follows. "MW3DS-SBS done" after the last. A cast shot: the spell
-- readied, Use pressed, and the world stopped (global.lua) `after` seconds of game time later; after the shot the world
-- goes on for kClearSecs, the bolt and its visuals gone before the next cast.
-- Angles as in the 3DS engine: yaw 0 looks along +Y, 90 along +X; pitch up is positive (OpenMW's is down).
local camera = require('openmw.camera')
local core = require('openmw.core')
local input = require('openmw.input')
local nearby = require('openmw.nearby')
local self = require('openmw.self')
local types = require('openmw.types')
local util = require('openmw.util')
local I = require('openmw.interfaces')
local hold = require('scripts.mw3ds_sbs.hold')
local poses = require('scripts.mw3ds_sbs.poses')

local kStartFrames = 60      -- after the game loads, before the first shot
local kStanceFrames = 45     -- fp: the spell readied (its equip played) before the arms take the group
local kSettleFrames = 90     -- after a shot is posed: textures, the actor's pose, the sky
local kHoldSecs = 3.0        -- a ready shot holds this long (real time) for sidebyside.py's F12, unless F12 comes first
local kAfterShotFrames = 20  -- after F12, before the next shot moves the camera
local kClearSecs = 3.0       -- a cast shot: the world going on again before the next one

local index = 0
local state = 'start'
local frames = 0
local posed = false          -- the world (and the shot's actor) is set
local armsFrames = -1        -- fp: frames since the stance, -1 before the world is set
local armsHold = nil
local readyAt = 0
local castFrames = -1        -- a cast shot: frames since the world was set, -1 before
local castStart = nil        -- game time Use was pressed
local resumedAt = nil        -- real time the world went on again after a cast shot

local function placeCamera()
	local s = poses[index]
	if s.fp then
		camera.setMode(camera.MODE.FirstPerson)
	else
		camera.setMode(camera.MODE.Static)
		camera.setStaticPosition(util.vector3(s.x, s.y, s.z))
	end
	camera.setYaw(s.yaw)
	camera.setPitch(-s.pitch)
	camera.showCrosshair(false)
end

local function nextShot()
	index = index + 1
	frames = 0
	posed = false
	armsFrames = -1
	armsHold = nil
	castFrames = -1
	castStart = nil
	if index > #poses then
		state = 'done'
		print('MW3DS-SBS done')
		return
	end
	state = 'setup'
	core.sendGlobalEvent('MW3DS_SBS_Setup', { index = index })
end

-- The shot's worn armor and clothing (wear=), each in its slot by its type
local kArmorSlots = { 'Helmet', 'Cuirass', 'LeftPauldron', 'RightPauldron', 'Greaves', 'Boots', 'LeftGauntlet',
	'RightGauntlet', 'CarriedLeft', 'LeftGauntlet', 'RightGauntlet' }
local kClothingSlots = { 'Pants', 'Boots', 'Shirt', 'Belt', 'Robe', 'RightGauntlet', 'LeftGauntlet', 'Skirt',
	'RightRing', 'Amulet' }
local function wear(s, eq)
	if not s.wear then
		return
	end
	local inv = types.Actor.inventory(self)
	for _, id in ipairs(s.wear) do
		local o = inv:find(id)
		if o then
			local name = types.Armor.objectIsInstance(o) and kArmorSlots[types.Armor.record(o).type + 1]
				or types.Clothing.objectIsInstance(o) and kClothingSlots[types.Clothing.record(o).type + 1]
			if name then
				eq[types.Actor.EQUIPMENT_SLOT[name]] = o
			end
		end
	end
end

-- fp: the spell readied, then the arms hold the group
local function updateArms()
	local s = poses[index]
	if armsFrames < 0 then
		return false
	end
	armsFrames = armsFrames + 1
	if armsFrames == 1 then
		-- (one setEquipment: a second one in the same frame starts from the equipment before the first and undoes it)
		local eq = types.Actor.getEquipment(self)
		wear(s, eq)
		if s.stance == 'weapon' then
			-- the shot's weapon in hand (none: fists), its ammunition, the shield
			local slot = types.Actor.EQUIPMENT_SLOT
			eq[slot.CarriedRight] = s.weapon
			eq[slot.Ammunition] = s.ammo
			eq[slot.CarriedLeft] = s.shield
			types.Actor.setEquipment(self, eq)
			types.Actor.setStance(self, types.Actor.STANCE.Weapon)
		else
			types.Actor.setEquipment(self, eq)
			types.Actor.setSelectedSpell(self, s.spell or 'fire bite')
			types.Actor.setStance(self, types.Actor.STANCE.Spell)
		end
	end
	if armsFrames == 2 and s.stance == 'weapon' and s.shield and s.weapon then
		-- (a torch beside the weapon: given in the same setEquipment as the weapon it stayed out of the hand)
		local eq = types.Actor.getEquipment(self)
		eq[types.Actor.EQUIPMENT_SLOT.CarriedLeft] = s.shield
		types.Actor.setEquipment(self, eq)
	end
	if armsFrames < kStanceFrames then
		return false
	end
	if armsFrames == kStanceFrames and s.stance == 'weapon' then
		-- what is in hand (openmw.log)
		local eq = types.Actor.getEquipment(self)
		local slot = types.Actor.EQUIPMENT_SLOT
		local function id(o) return o and o.recordId or '-' end
		print(string.format('MW3DS-SBS equipped %s / %s / %s', id(eq[slot.CarriedRight]), id(eq[slot.CarriedLeft]),
			id(eq[slot.Ammunition])))
	end
	if not armsHold then
		armsHold = { group = s.group, startKey = s.startKey, stopKey = s.stopKey, at = s.at or 0 }
		hold.start(armsHold)
	end
	return hold.update(armsHold)
end

-- a cast shot: the spell readied, Use pressed, then the world stopped `after` seconds of game time later
local function updateCast()
	local s = poses[index]
	if not (s.cast or s.vfx or s.act) or castFrames < 0 then
		return false
	end
	castFrames = castFrames + 1
	if s.act then
		-- a scene step: done by the world (global.lua); the shot `after` seconds of game time later
		if not castStart then
			castStart = core.getSimulationTime()
			return false
		end
		if core.getSimulationTime() - castStart >= s.after then
			core.sendGlobalEvent('MW3DS_SBS_Halt', { on = true })
			castFrames = -1
			return true
		end
		return false
	end
	if s.vfx then
		-- a spell visual on its own: played, then the world stopped `after` seconds of game time later
		if not castStart then
			core.sendGlobalEvent('MW3DS_SBS_Vfx', { index = index })
			castStart = core.getSimulationTime()
			return false
		end
		if core.getSimulationTime() - castStart >= s.after then
			core.sendGlobalEvent('MW3DS_SBS_Halt', { on = true })
			castFrames = -1
			return true
		end
		return false
	end
	if castFrames == 1 then
		-- every cast succeeds and none runs short (sidebyside.py's CAST_SKILLS on the 3DS side)
		for _, k in ipairs({ 'destruction', 'alteration', 'illusion', 'conjuration', 'mysticism', 'restoration' }) do
			types.NPC.stats.skills[k](self).base = 100
		end
		types.Actor.stats.attributes.willpower(self).base = 100
		types.Actor.stats.attributes.luck(self).base = 100
		types.Actor.stats.dynamic.magicka(self).current = 1000
		types.Actor.setSelectedSpell(self, s.cast)
		types.Actor.setStance(self, types.Actor.STANCE.Spell)
	end
	if castFrames < kStanceFrames then
		return false
	end
	if not castStart then
		self.controls.use = 1
		castStart = core.getSimulationTime()
		print(string.format('MW3DS-SBS cast %s', s.cast))
		return false
	end
	self.controls.use = 0
	if core.getSimulationTime() - castStart >= s.after then
		core.sendGlobalEvent('MW3DS_SBS_Halt', { on = true })
		castFrames = -1
		return true
	end
	return false
end

return {
	eventHandlers = {
		MW3DS_SBS_Posed = function(d)
			if d.index ~= index then
				return
			end
			if poses[index].cast or poses[index].vfx or poses[index].act then
				castFrames = 0
			elseif (poses[index].fp or poses[index].body) and poses[index].group then
				armsFrames = 0
			else
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
				-- a message box or menu left open would be in the shot; a menu shot's own menu (ui=) opened and kept
				local s = poses[index]
				if s.ui and posed then
					if I.UI.getMode() ~= s.ui then
						local target = nil
						for _, a in ipairs(nearby.actors) do
							if s.target and a.recordId == s.target then
								target = a
							end
						end
						I.UI.setMode(s.ui, target and { target = target } or nil)
					end
				elseif I.UI.getMode() then
					I.UI.setMode()
				end
				if not posed and (updateArms() or updateCast()) then
					posed = true
					frames = 0
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
					if poses[index].cast or poses[index].vfx or poses[index].act then
						if not resumedAt then
							core.sendGlobalEvent('MW3DS_SBS_Halt', { on = false })
							resumedAt = core.getRealTime()
						end
						if core.getRealTime() - resumedAt < (poses[index].act and 0.1 or kClearSecs) then
							return
						end
						resumedAt = nil
					end
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
