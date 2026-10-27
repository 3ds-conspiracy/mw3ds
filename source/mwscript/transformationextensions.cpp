// Script functions: position, angle, movement, placing objects
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "components/debug/debuglog.hpp"
#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> transformationExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// GetScale / SetScale / ModScale: the object's size (OpenMW's; World::scaleObject); the player stays as it is
	case F_GETSCALE: return r ? r->scale : 1.0f;
	case F_SETSCALE: case F_MODSCALE:
		if (r && !onPlayer)
			w.scaleObject(ref, (n.func == F_MODSCALE ? r->scale : 0.0f) + argNum(c, n, 0));
		return 0.0f;
	// PlaceAtPC / PlaceAtMe id count distance direction (0 front, 1 back, 2 left, 3 right)
	// PlaceItem id x y z zRot: a new one at that spot in the player's cell (outdoors the cell under it); PlaceItemCell id
	// cell x y z zRot: in that cell (OpenMW's PlaceItem / PlaceItemCell; the angle in degrees)
	case F_PLACEITEM: case F_PLACEITEMCELL:
	{
		bool inCell = n.func == F_PLACEITEMCELL;
		int k = inCell ? 2 : 1;
		int cell = -1;
		if (inCell)
		{
			cell = w.cellIndex(argStr(c, n, 1));
			if (cell < 0)
			{
				c.host.report("unknown cell (" + argStr(c, n, 1) + ")");
				logf("script: PlaceItemCell: unknown cell %s", argStr(c, n, 1).c_str());
				return 0.0f;
			}
		}
		float pos[3] = { argNum(c, n, k), argNum(c, n, k + 1), argNum(c, n, k + 2) };
		w.placeObject(argStr(c, n, 0), cell, pos, argNum(c, n, k + 3) * 3.14159265f / 180.0f);
		return 0.0f;
	}
	case F_PLACEATPC: case F_PLACEATME:
	{
		bool atPC = n.func == F_PLACEATPC || !r;
		w.placeAt(argStr(c, n, 0), (int)argNum(c, n, 1, 1), argNum(c, n, 2, 0), (int)argNum(c, n, 3, 0),
			atPC ? w.player.feet : r->pos, atPC ? w.player.yaw : r->rot[2]);
		return 0.0f;
	}
	// Placement: angles in degrees, axes "x" / "y" / "z" (moving what the level baked moves only actors and doors)
	case F_SETANGLE: case F_SETPOS: case F_MOVE: case F_MOVEWORLD: case F_ROTATEWORLD: case F_GETSTARTINGANGLE:
	case F_GETSTARTINGPOS:
	{
		std::string ax = lower(argStr(c, n, 0));
		// Any other axis does nothing (and reads 0); SetAngle also takes u / v / w (the other rotation order, the
		// same angles here)
		int k = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : -1;
		if (n.func == F_SETANGLE && k < 0)
			k = ax == "u" ? 0 : ax == "v" ? 1 : ax == "w" ? 2 : -1;
		if (k < 0)
			return 0.0f;
		float v = argNum(c, n, 1);
		float* pos = onPlayer || !r ? w.player.feet : r->pos;
		switch (n.func)
		{
		// the start angles kept are the turn about Z only
		case F_GETSTARTINGANGLE: return r && k == 2 ? r->start[3] * 57.29578f : 0.0f;
		case F_GETSTARTINGPOS: return r ? r->start[k] : 0.0f;
		case F_SETPOS: pos[k] = v; break;
		case F_MOVE: case F_MOVEWORLD:
		{
			float d[3] = { 0.0f, 0.0f, 0.0f };
			d[k] = v * c.dt;
			if (n.func == F_MOVE)
			{
				// Move goes along the object's own axes; a disabled object isn't moved
				if (r && !onPlayer && !r->visible())
					return 0.0f;
				bool mover = r && !onPlayer;
				float rot[3] = { 0.0f, 0.0f, w.player.yaw };
				if (mover)
					memcpy(rot, r->rot, sizeof(rot));
				localToWorld(rot, !mover || r->actor >= 0, d);
			}
			for (int i = 0; i < 3; i++)
				pos[i] += d[i];
			break;
		}
		case F_SETANGLE:
			if (onPlayer || !r)
			{
				if (k == 2) w.player.yaw = v / 57.29578f;
				else if (k == 0) w.player.pitch = fmaxf(-1.5f, fminf(1.5f, -v / 57.29578f));   // ours turns up, OpenMW's down
			}
			else
				r->rot[k] = v / 57.29578f;
			break;
		case F_ROTATEWORLD:
			// actors turn about Z only
			if (r && !onPlayer && (k == 2 || r->actor < 0))
				r->rot[k] += v / 57.29578f * c.dt;
			break;
		}
		if (r && !onPlayer)
		{
			r->moved = true;
			r->fitBox();
			if (r->actor >= 0)
				w.syncActor(ref);
		}
		return 0.0f;
	}
	case F_SETATSTART:
		if (r)
		{
			memcpy(r->pos, r->start, sizeof(r->pos));
			r->rot[2] = r->start[3];
			r->fitBox();
			r->moved = true;
			if (r->actor >= 0)
				w.syncActor(ref);
		}
		return 0.0f;
	// Rotating objects are baked into the cell's batches
	// Rotate axis degrees-per-second (the object's own axis; objects scripts turn have their own mesh)
	case F_ROTATE:
	{
		std::string ax = lower(argStr(c, n, 0));
		float turn = argNum(c, n, 1) / 57.29578f * c.dt;
		if (onPlayer || !r)
			w.player.yaw += turn;       // the player turns about Z whatever the axis
		else if (ax == "x" || ax == "y" || ax == "z")
		{
			r->rot[ax == "x" ? 0 : ax == "y" ? 1 : 2] += turn;
			r->moved = true;
		}
		return 0.0f;
	}
	case F_GETDISTANCE:
	{
		std::string to = lower(argStr(c, n, 0));
		// Player->GetDistance <object>: the player's distance to it (Dream of Hircine's hunter spawner picks its spot so)
		if (onPlayer)
		{
			int o = to == "player" ? -1 : w.findRef(to);
			return to == "player" ? 0.0f : o < 0 ? 100000.0f : w.distanceToPlayer(o);
		}
		if (!r)
			return 100000.0f;
		if (to == "player")
			return w.distanceToPlayer(ref);
		int o = w.findRef(to);
		if (o < 0)
			return 100000.0f;
		if (!w.sameSpace(o, ref))
			return 100000.0f;
		float d[3] = { r->pos[0] - w.refs[o].pos[0], r->pos[1] - w.refs[o].pos[1], r->pos[2] - w.refs[o].pos[2] };
		return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	}
	case F_GETPOS:
	{
		std::string axis = lower(argStr(c, n, 0));
		const float* p = onPlayer ? w.player.feet : (r ? r->pos : nullptr);
		if (!p) return 0.0f;
		return axis == "x" ? p[0] : axis == "y" ? p[1] : axis == "z" ? p[2] : 0.0f;
	}
	case F_GETANGLE:
	{
		std::string axis = lower(argStr(c, n, 0));
		if (onPlayer)
			return axis == "z" ? w.player.yaw * 57.29578f : axis == "x" ? -w.player.pitch * 57.29578f : 0.0f;
		if (!r) return 0.0f;
		return axis == "x" ? r->rot[0] * 57.29578f : axis == "y" ? r->rot[1] * 57.29578f
			: axis == "z" ? r->rot[2] * 57.29578f : 0.0f;
	}
	// PositionCell x y z zRot "cell" / Position x y z zRot: the player travels there (a loading screen
	// like a door's), anyone else moves there (drawn in that cell from now on)
	case F_GETPOS_: return 0.0f;
	case F_POSITIONCELL: case F_POSITION:
	{
		float pos[3] = { argNum(c, n, 0), argNum(c, n, 1), argNum(c, n, 2) };
		const bool toPlayer = onPlayer || !r;
		float yaw = argNum(c, n, 3) * 3.14159265f / 180.0f;
		// OpenMW copies a Morrowind.exe bug on purpose: anyone but the player has the angle turned into radians twice
		if (!toPlayer)
			yaw *= 3.14159265f / 180.0f;
		int grid = w.gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		int cell;
		if (n.func == F_POSITION)
		{
			// the player goes to the exterior cell there (from an interior too); anyone else outdoors goes to the exterior
			// cell there as well, and an actor indoors only when the player is outdoors and that cell is in play
			// (OpenMW's World::moveObject); otherwise they stay in their cell. Bloodmoon's werewolf attack brings Korst
			// from Lake Fjalding to Skaal Village so
			if (toPlayer)
				cell = grid >= 0 ? grid : w.current;
			else
			{
				int here = w.placeOf(ref);
				bool outdoors = here >= 0 && !w.cells[here].interior;
				bool toActive = r->actor >= 0 && grid >= 0 && w.current >= 0 && !w.cells[w.current].interior && w.cells[grid].live;
				cell = grid >= 0 && (outdoors || toActive) ? grid : here;
			}
		}
		else
		{
			std::string name = argStr(c, n, 4);
			cell = w.cellIndex(name);
			std::string ln = lower(name);
			if (cell < 0 && !toPlayer && !name.empty() && ln != "wilderness" && !w.game.regions.count(ln))
			{
				// not an interior or a named exterior: only the player is moved (to the exterior), OpenMW leaves others
				monitorOnce(("poscell:" + name).c_str(), "PositionCell into %s: not in the level", name.c_str());
				return 0.0f;
			}
			if (cell < 0 || !w.cells[cell].interior)
				cell = grid;
			if (cell < 0)
			{
				logf("script: PositionCell into %s: not in the level", name.c_str());
				monitorOnce(("poscell:" + name).c_str(), "PositionCell into %s: not in the level", name.c_str());
				return 0.0f;
			}
		}
		if (toPlayer)
			c.host.teleportPlayer(cell, pos, yaw);
		else if (w.ensureRefs(cell))
		{
			// set down on the floor there (OpenMW adjustPosition)
			if (r->actor >= 0 && w.cells[cell].live)
				w.snapToFloor(pos);
			w.relocate(ref, cell, pos, yaw);
		}
		return 0.0f;
	}
	default:
		return std::nullopt;
	}
}
