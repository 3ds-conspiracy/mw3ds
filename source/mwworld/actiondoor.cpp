// Doors: locks, load doors, opening and closing
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include <malloc.h>

// The lock and the trap of a door or container, as OpenMW's Door::activate / Container::activate: the key (carried by the
// player) unlocks it, says so, and disarms a trap; with no key a locked one only gives its sound; an unlocked trapped one
// fires its trap instead of opening
int Session::lockGate(int ref, const char* lockedSound)
{
	Ref& r = w.refs[ref];
	bool locked = r.lockLevel > 0, trapped = !r.trap.empty() && !r.disarmed;
	bool key = !r.key.empty() && w.itemCount(lower(r.key)) > 0;
	if (locked && key)
	{
		const Object* k = w.game.object(r.key);
		notify((k ? k->name.str() : r.key.str()) + " " + w.game.gmst("skeyused", "was used to open the lock."));
		r.lockLevel = -r.lockLevel;       // (the level is kept for Lock)
		logf("lock: %s opened with %s", r.id.c_str(), r.key.c_str());
		if (trapped)
		{
			r.disarmed = true;            // the key disarms it too
			trapped = false;
			playSound(ref, "Disarm Trap");
		}
	}
	else if (locked)
	{
		playSound(ref, lockedSound);
		return 0;
	}
	if (trapped)
	{
		springTrap(ref);
		return 1;
	}
	return 2;
}

void Session::useDoor(int ref)
{
	Ref& r = w.refs[ref];
	if (lockGate(ref, "LockedDoor") != 2)
		return;
	if (r.hasDest)
	{
		int dest = r.destUnconverted ? -1 : r.destHasGrid ? w.gridCell(r.destGrid[0], r.destGrid[1]) : w.cellIndex(r.destCell);
		if (dest < 0 && !r.destEnd)
		{
			notify(w.game.cellLabel(r.destCell) + " is not converted yet.");
			return;
		}
		if (r.obj && !r.obj->openSound.empty())
			playSound(-1, r.obj->openSound);
		if (r.destEnd)
		{
			ended = true;
			openScreen(SCR_END);
			return;
		}
		if (dest == w.current)
		{
			// Followers within 800 come along to elsewhere in the same cell too (OpenMW's ActionTeleport, every
			// teleport door: Rabinna up the trapdoor of Fatleg's Drop Off), by the rule of a door to another cell
			std::vector<int> followers;
			for (int i : w.loadedActors)
				if (w.followsPlayer(i) && w.refs[i].aiPackage != AIPKG_ESCORT)
				{
					Ref& f = w.refs[i];
					float* stay = f.script >= 0 ? w.scripts[f.script].local("stayoutside") : nullptr;
					float dx = f.pos[0] - w.player.feet[0], dy = f.pos[1] - w.player.feet[1], dz = f.pos[2] - w.player.feet[2];
					if (!followerTaken(dx * dx + dy * dy + dz * dz, stay && *stay == 1.0f, !w.cells[w.placeOf(i)].interior,
						!w.cells[dest].interior))
						continue;
					if (f.ai == AI_COMBAT)
					{
						f.ai = AI_IDLE;
						continue;
					}
					followers.push_back(i);
				}
			memcpy(w.player.feet, r.destPos, sizeof(r.destPos));
			w.player.yaw = r.destRot[2];
			w.player.pitch = 0.0f;
			w.player.vz = 0.0f;
			w.player.fallTop = w.player.feet[2];   // a door isn't a fall
			w.player.landedFall = 0.0f;
			w.player.inertia[0] = w.player.inertia[1] = 0.0f;
			for (size_t k = 0; k < followers.size(); k++)
			{
				// just ahead of the player, side by side, as a door to another cell sets them down
				float back = -72.0f, side = ((int)k - (int)followers.size() / 2) * 64.0f + (k % 2 ? 40.0f : -40.0f);
				float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
				float pos[3] = { w.player.feet[0] - fx * back + fy * side, w.player.feet[1] - fy * back - fx * side,
					w.player.feet[2] };
				w.relocate(followers[k], w.placeOf(followers[k]), pos, w.player.yaw);
			}
			fade = 1.0f;
		}
		else
		{
			travelCell = dest;
			memcpy(travelPos, r.destPos, sizeof(travelPos));
			travelYaw = r.destRot[2];
		}
		return;
	}
	bool opening = r.doorTarget == 0.0f;
	r.doorTarget = opening ? 1.5708f : 0.0f;
	if (r.obj)
		playSound(ref, opening ? r.obj->openSound : r.obj->closeSound);
}
