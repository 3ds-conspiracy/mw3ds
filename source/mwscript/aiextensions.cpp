// Script functions: AI packages, combat, fight / flee / alarm / hello, line of sight
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

std::optional<float> aiExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// The player is the one thing fought here, so "player" is the only target: fighting them, or (OpenMW) with no
	// fight on, being in the middle of talking to them
	// Face x y: a person or creature turns to look at the point, then goes on (OpenMW's AiFace; the player can't be
	// made to). Its third number, which some scripts give, is not read
	case F_FACE:
		if (r && !onPlayer && r->actor >= 0)
		{
			r->faceAt[0] = argNum(c, n, 0);
			r->faceAt[1] = argNum(c, n, 1);
			r->facing = true;
		}
		return 0.0f;
	case F_GETTARGET:
		return r && r->actor >= 0 && lower(argStr(c, n, 0)) == "player" && (r->ai == AI_COMBAT || c.host.talkingTo(ref))
			? 1.0f : 0.0f;
	case F_GETLOS: case F_GETLINEOFSIGHT: case F_GETDETECTED:
	{
		std::string t = lower(argStr(c, n, 0));
		// The player has noticed them lately (they notice by line of sight and awareness)
		if (n.func == F_GETDETECTED && t == "player")
			return r && w.time - r->lastSeen < 3.0f ? 1.0f : 0.0f;
		// Both must be actors, enabled, and loaded (OpenMW); GetDetected of another actor is line of sight
		// (their awareness check always passes unless sneaking or invisible)
		if (r && (r->actor < 0 || !r->visible() || !w.active(ref)))
			return 0.0f;
		const float* from = r ? r->pos : w.player.feet;
		const float* to = t == "player" ? w.player.feet : nullptr;
		int ti = to ? -1 : w.findRef(t);
		if (ti >= 0)
		{
			if (w.refs[ti].actor < 0 || !w.refs[ti].visible() || !w.active(ti))
				return 0.0f;
			to = w.refs[ti].pos;
		}
		if (!to)
			return 0.0f;
		float a[3] = { from[0], from[1], from[2] + 100.0f }, b[3] = { to[0], to[1], to[2] + 100.0f };
		return w.lineOfSight(a, b) ? 1.0f : 0.0f;
	}
	case F_GETATTACKED: return r && r->attacked ? 1.0f : 0.0f;
	case F_MODFLEE: case F_SETFLEE: case F_GETFLEE:
	{
		if (!r || r->actor < 0)
			return 0.0f;
		if (r->flee < 0)
			r->flee = w.game.actors[r->actor].flee;
		// (no clamping, as in OpenMW: SetFlee 150 reads back 150)
		if (n.func == F_MODFLEE) r->flee += (int)argNum(c, n, 0);
		if (n.func == F_SETFLEE) r->flee = (int)argNum(c, n, 0);
		return (float)r->flee;
	}
	case F_SETALARM: case F_GETALARM: case F_MODALARM:
		if (!r || r->actor < 0)
			return 0.0f;
		if (r->alarm < 0) r->alarm = w.game.actors[r->actor].alarm;
		if (n.func == F_SETALARM) r->alarm = (int)argNum(c, n, 0);
		if (n.func == F_MODALARM) r->alarm += (int)argNum(c, n, 0);
		return (float)r->alarm;
	// StartCombat target: the player, or another actor (OpenMW starts an AiCombat package on it): the actor goes for that
	// one, who fights back unless already busy with a fight
	case F_STARTCOMBAT:
		if (r && r->actor >= 0 && !r->dead)
		{
			std::string to = lower(argStr(c, n, 0));
			if (to == "player")
			{
				r->aggressor = true;
				c.host.startCombat(ref);
			}
			else
			{
				int t = w.findRef(to);
				if (t >= 0 && t != ref && w.refs[t].actor >= 0 && !w.refs[t].dead)
				{
					r->duelFoe = t;
					if (w.refs[t].duelFoe < 0)
						w.refs[t].duelFoe = ref;
				}
			}
		}
		return 0.0f;
	case F_STOPCOMBAT:
		if (r && r->actor >= 0)
		{
			r->duelFoe = -1;
			c.host.stopCombat(ref);
		}
		return 0.0f;
	case F_SETHELLO:
		if (r && r->actor >= 0)
			r->hello = (int)argNum(c, n, 0);
		return 0.0f;
	case F_MODFIGHT: case F_SETFIGHT: case F_GETFIGHT:
	{
		if (!r || r->actor < 0)
			return 0.0f;
		int& fight = r->fight;
		if (fight < 0)
			fight = w.game.actors[r->actor].fight;
		// (no clamping, as in OpenMW)
		if (n.func == F_MODFIGHT)
			fight += (int)argNum(c, n, 0);
		else if (n.func == F_SETFIGHT)
			fight = (int)argNum(c, n, 0);
		return (float)fight;
	}
	// AI packages. Arguments as Morrowind has them: AIWander range duration time [idles],
	// AITravel x y z, AIFollow / AIEscort target duration x y z, the -Cell forms with a cell name
	// after the target, AIActivate target
	case F_AIWANDER: case F_AITRAVEL: case F_AIFOLLOW: case F_AIFOLLOWCELL: case F_AIESCORT: case F_AIESCORTCELL:
	case F_AIACTIVATE:
	{
		if (!r || r->actor < 0)
			return 0.0f;
		// AIEscortCell with no such cell is ignored (OpenMW), the actor keeps what it was doing
		if (n.func == F_AIESCORTCELL && w.cellIndex(argStr(c, n, 1)) < 0)
			return 0.0f;
		// A package of its own ends a fight (the new one is stacked on a cleared combat)
		if (r->ai == AI_COMBAT)
			c.host.stopCombat(ref);
		r->aiDone = false;
		r->aiRepeat = false;
		r->aiActive = true;
		r->aiStart = w.gameHour;
		r->aiDuration = 0.0f;
		r->aiTarget.clear();
		r->aiCell.clear();
		r->aiDest[0] = r->aiDest[1] = r->aiDest[2] = 0.0f;
		r->path.clear();
		r->wandering = false;
		switch (n.func)
		{
		case F_AIWANDER:
			r->aiPackage = AIPKG_WANDER;
			r->aiRange = (int)argNum(c, n, 0);
			r->aiDuration = (float)(int)argNum(c, n, 1);
			// after the time of day come the idle chances (the first unused): any of them, or a last argument, repeats
			if (n.kids.size() > 4)
				r->aiRepeat = n.kids.size() > 12 ? argNum(c, n, 12) != 0.0f : true;
			break;
		case F_AITRAVEL:
			r->aiPackage = AIPKG_TRAVEL;
			for (int k = 0; k < 3; k++)
				r->aiDest[k] = argNum(c, n, k);
			r->aiRepeat = n.kids.size() > 3;
			break;
		case F_AIACTIVATE:
			r->aiPackage = AIPKG_ACTIVATE;
			r->aiTarget = lower(argStr(c, n, 0));
			r->aiRepeat = n.kids.size() > 1;
			break;
		default:
		{
			bool cell = n.func == F_AIFOLLOWCELL || n.func == F_AIESCORTCELL;
			r->aiPackage = n.func == F_AIFOLLOW || n.func == F_AIFOLLOWCELL ? AIPKG_FOLLOW : AIPKG_ESCORT;
			r->aiTarget = lower(argStr(c, n, 0));
			if (cell)
				r->aiCell = lower(argStr(c, n, 1));
			int k = cell ? 2 : 1;
			r->aiDuration = argNum(c, n, k);
			// (an escort's hours are a whole number)
			if (r->aiPackage == AIPKG_ESCORT)
				r->aiDuration = (float)(int)r->aiDuration;
			for (int i = 0; i < 3; i++)
				r->aiDest[i] = argNum(c, n, k + 1 + i);
			r->aiRepeat = n.kids.size() > (size_t)(k + 4);
			// a follower waits to see who it follows before it starts (and counts its hours from then)
			r->aiActive = r->aiPackage != AIPKG_FOLLOW;
			break;
		}
		}
		logf("ai: %s package %d target %s", r->idLower.c_str(), r->aiPackage, r->aiTarget.c_str());
		return 0.0f;
	}
	case F_GETAIPACKAGEDONE: return r && r->aiDone ? 1.0f : 0.0f;
	// The package running now: -1 when the last one ended, or once dead and the death animation is over (OpenMW:
	// while it plays the package still counts, so 'GetCurrentAiPackage == 3 ... OnDeath' sees a follower die; the
	// animation taken as 2 seconds); an actor with a wander distance of its own has a wander package (OpenMW lists
	// it from the record)
	case F_GETCURRENTAIPACKAGE:
		if (!r || r->actor < 0 || r->aiPackage == AIPKG_IDLE)
			return -1.0f;
		if (r->dead && (r->diedAt < 0.0f || w.gameHour - r->diedAt >= 2.0f * w.timescale() / 3600.0f))
			return -1.0f;
		return r->aiPackage == AIPKG_NONE ? (w.game.actors[r->actor].wander > 0 ? (float)AIPKG_WANDER : -1.0f)
			: (float)r->aiPackage;
	// ToggleAI: actors stand where they are, their packages and fights held (OpenMW's MechanicsManager::toggleAI)
	case F_TAI: case F_TOGGLEAI:
		w.aiEnabled = !w.aiEnabled;
		c.host.report(w.aiEnabled ? "AI -> On" : "AI -> Off");
		return 0.0f;
	default:
		return std::nullopt;
	}
}
