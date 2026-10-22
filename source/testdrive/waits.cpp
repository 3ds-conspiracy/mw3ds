// Driver steps that wait for something: UNSEEN, UNTIL, LOOTWAIT, TALKWAIT, DOORWAIT, TAKE, SEARCH
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
#include "testdrive/testhelpers.hpp"
#include <malloc.h>

// The steps that wait for something (UNSEEN, UNTIL, LOOTWAIT, TALKWAIT, DOORWAIT, TAKE, SEARCH): std::nullopt when
// this step is none of them
std::optional<bool> TestDriver::updateWaits(Session& s, PlayerInput& in, u32& down, float dt)
{
	World& w = s.w;
	if (kind == UNSEEN)
	{
		int seer = s.crimeWitness();
		unseenTimer = seer < 0 ? unseenTimer + dt : 0.0f;
		if (unseenTimer >= 1.0f)
		{
			logf("drive: nobody watching after %.1f s", elapsed);
			kind = NONE;
		}
		else if (elapsed > unseenLimit)
		{
			logf("drive: FAIL unseen: %s still watching after %.0f s", seer >= 0 ? w.refs[seer].id.c_str() : "someone", elapsed);
			kind = NONE;
		}
		return kind != NONE;
	}
	if (kind == UNTIL)
	{
		if (expect(s, untilSpec, true))
		{
			logf("drive: until %s: held after %.1f s", untilSpec.c_str(), elapsed);
			kind = NONE;
		}
		// (a levitation that does not end: what holds it up, once)
		else if (elapsed >= 4.0f && elapsed - dt < 4.0f && untilSpec.compare(0, 9, "effect:10") == 0)
			for (const ActiveEffect& e : w.effects)
				if (e.effect == 10)
					logf("drive: until %s: Levitate %.0f from %s, %.1f s left", untilSpec.c_str(), e.magnitude, e.source.c_str(), e.remaining);
		return kind != NONE;
	}
	if (kind == LOOTWAIT)
	{
		// the container (or body) screen is open: take the item as a tap on it does
		takeTimer += dt;
		if (s.screen == SCR_CONTAINER && s.containerRef == ref && lootPut)
		{
			int at = -1;
			for (size_t i = 0; i < w.inventory.size() && at < 0; i++)
				if (w.inventory[i].id == lootItem || w.inventory[i].id == lower(spaced(lootItem)))
					at = (int)i;
			if (at < 0)
				logf("drive: FAIL put %s: not carried", lootItem.c_str());
			else if (s.containerPut(at))
				logf("drive: put %s into %s", lootItem.c_str(), id.c_str());
			else
				logf("drive: FAIL put %s into %s: refused", lootItem.c_str(), id.c_str());
			s.closeScreen();
			kind = NONE;
		}
		else if (s.screen == SCR_CONTAINER && s.containerRef == ref)
		{
			const Ref& c = w.refs[ref];
			for (size_t i = 0; i < c.contents.size(); i++)
			{
				std::string cid = lower(c.contents[i].second);
				if (cid == lootItem || cid == lower(spaced(lootItem)))
				{
					s.containerTake((int)i, true);
					logf("drive: took %s from %s", lootItem.c_str(), id.c_str());
					s.closeScreen();
					kind = NONE;
					return false;
				}
			}
			logf("drive: FAIL loot %s: no %s inside", id.c_str(), lootItem.c_str());
			s.closeScreen();
			kind = NONE;
		}
		else if (takeTimer > 1.5f)
		{
			// the press found nothing (the body's small pick box moved off the crosshair): look and press again, as a player does
			if (++talkTries <= 3)
			{
				logf("drive: %s: nothing opened, again", id.c_str());
				kind = ACTIVATE;
				lookTimer = 0.0f;
				return true;
			}
			logf("drive: FAIL loot %s: it didn't open", id.c_str());
			kind = NONE;
		}
		return kind != NONE;
	}
	if (kind == TALKWAIT)
	{
		// a person: the talk opens, or the press found nothing (they stepped off the crosshair): look and press again
		takeTimer += dt;
		// v2 kit: the talk opened with someone else (they stepped into the crosshair as A went in, Nilvyn Drothan before Uvoo
		// Llaren): closed, as a player backs out, and pressed again
		if (s.kitV2() && s.dlg.open && s.screen == SCR_DIALOGUE && s.dlg.ref >= 0 && s.dlg.ref != ref && s.dlg.choices.empty()
			&& talkTries < 5)
		{
			logf("drive: the talk opened with %s, not %s: closing it, again", w.refs[s.dlg.ref].id.c_str(), id.c_str());
			s.closeScreen();
			talkTries++;
			kind = ACTIVATE;
			lookTimer = 0.0f;
			return true;
		}
		// (or a container: the press from out there, still walking, went to the chest beside them: Adusamsi Assurnarairan)
		if (s.kitV2() && s.screen == SCR_CONTAINER && talkTries < 5 && w.refs[ref].recType == REC_NPC_ && !w.refs[ref].dead)
		{
			logf("drive: the press opened a container, not a talk with %s: closing it, again", id.c_str());
			s.closeScreen();
			talkTries++;
			kind = ACTIVATE;
			lookTimer = 0.0f;
			return true;
		}
		// (someone in combat with the player refuses to talk, with the sActorInCombat message: nothing to press again)
		if (s.menuOpen() || w.refs[ref].ai == AI_COMBAT)
		{
			kind = NONE;
			return false;
		}
		if (takeTimer > 0.4f)
		{
			if (++talkTries > 5)
			{
				fail("pressed A five times, no talk opened");
				return false;
			}
			logf("drive: %s: no talk opened, again", id.c_str());
			kind = ACTIVATE;
			lookTimer = 0.0f;
		}
		return true;
	}
	if (kind == DOORWAIT)
	{
		// a trapped door: the trap took the press and the door stayed shut; press again (OpenMW: an unlocked trapped
		// door fires its trap instead of opening, the next activation opens it)
		takeTimer += dt;
		if (takeTimer > 0.6f)
		{
			kind = NONE;
			if (!w.active(ref) || w.current != doorCell || s.menuOpen())
				return false;
			// (a door to elsewhere in the same cell took us there: Simine Fralinie's back room, the Gnisis barracks' cellar
			// trapdoor 530 up: moved 200 or more since the press)
			const float* pf = w.player.feet;
			float mx = pf[0] - pressAt[0], my = pf[1] - pressAt[1], mz = pf[2] - pressAt[2];
			if (w.refs[ref].hasDest && mx * mx + my * my + mz * mz > 200.0f * 200.0f)
				return false;
			// (a door that stood open: the press shut it, as it should; not a press that did nothing. Pressed again it
			// opened once more: Barenziah's Chambers' inner doors, left open, kept TR_Assassins' night from coming)
			if (doorWasOpen)
				return false;
			if (earlyTried && w.refs[ref].doorTarget == 0.0f && w.refs[ref].trap.empty())
			{
				logf("drive: %s: the press from out there did nothing, walking on", id.c_str());
				kind = ACTIVATE;
				lookTimer = 0.0f;
				return true;
			}
			const Ref& d = w.refs[ref];
			if (d.disarmed && d.doorTarget == 0.0f && ++talkTries <= 3)
			{
				logf("drive: %s: the trap used the press, again", id.c_str());
				kind = ACTIVATE;
				lookTimer = 0.0f;
				return true;
			}
			return false;
		}
		return true;
	}
	if (kind == TAKE)
	{
		// a book or scroll opened to read: its Take button (the reading screen's own path)
		takeTimer += dt;
		if (s.screen == SCR_BOOK && s.bookRef == ref)
		{
			s.closeScreen();
			s.pickUp(ref);
			logf("drive: took %s from the reading screen", id.c_str());
			kind = NONE;
		}
		else if (takeTimer > 1.0f && ref >= 0 && w.active(ref) && w.refs[ref].visible() && takeTries < 3 && !s.menuOpen())
		{
			// still lying there: the press found nothing (the player drifted, the item just past reach on that frame); a
			// player presses again
			takeTries++;
			logf("drive: %s still there after the press, pressing again", id.c_str());
			kind = ACTIVATE;
		}
		else if (takeTimer > 1.0f)
			kind = NONE;
		return kind != NONE;
	}
	if (kind == SEARCH)
	{
		searchTimer += dt;
		int r = findTarget(s, searchVerb, searchArg);
		if (r >= 0)
		{
			std::string tok = searchVerb + ":" + searchArg;
			std::string keep = searchArg;
			start(s, tok);
			logf("drive: found %s after %.1f s", keep.c_str(), searchTimer);
			return kind != NONE;
		}
		if (searchTimer > 40.0f)
		{
			kind = searchVerb == "WALKTO" ? WALK : searchVerb == "KILL" ? KILL : ACTIVATE;
			fail(searchVerb == "DOORTO" ? "no door there in the loaded cells" : "not in the loaded cells");
		}
		return kind != NONE;
	}
	return std::nullopt;
}
