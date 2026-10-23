// Tokens: memory, movies, alarms, dice, map, weather, console, screens, travel, barter
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
#include "platform/linear.hpp"
#include <malloc.h>
#include "testdrive/testhelpers.hpp"

std::optional<bool> opsSystem(Session& s, const std::vector<std::string>& a)
{
	World& w = s.w;
	const std::string& verb = a[0];
	auto fail = [&](const std::string& why) { logf("drive: FAIL %s: %s", verb.c_str(), why.c_str()); return true; };
	auto notTalking = [&](int ri) { return s.testLegit && !s.talkingTo(ri); };
	if (verb == "STREAM" && a.size() >= 2)
	{
		// STREAM:off / STREAM:on: the neighbouring exterior cells stop streaming in (the player's own cell still loads
		// as it is entered): a walk into a cell that isn't there yet, as on the console with a slow card
		w.streamPaused = a[1] == "off";
		logf("test: cell streaming %s", w.streamPaused ? "off" : "on");
		return true;
	}
	// RESTMENU: the rest menu opened as the player opens it (T): its refusals, and sleep or only waiting (EXPECT:restsleep)
	if (verb == "RESTMENU")
	{
		s.openRest(false);
		return true;
	}
	if ((verb == "MEMHOLD" || verb == "MEMLEAVE") && a.size() >= 2)
	{
		// MEMHOLD:<KB>: hold that much linear memory (in 256 KB pieces) to play as the device does when it is nearly
		// full; MEMHOLD:0 lets it go. MEMLEAVE:<KB>: hold all but that much (the same squeeze natively and on the
		// device, whatever each has free)
		static std::vector<void*> held;
		for (void* p : held)
			lockedLinearFree(p);
		held.clear();
		int want = atoi(a[1].c_str());
		if (verb == "MEMLEAVE")
			want = std::max(0, (int)(linearSpaceFree() / 1024) - want);
		for (int kb = want; kb > 0; kb -= 256)
			if (void* p = lockedLinearAlloc(256 * 1024))
				held.push_back(p);
		logf("test: holding %d KB of linear memory, %lu KB free", (int)held.size() * 256, linearSpaceFree() / 1024);
		return true;
	}
	if (verb == "MOVIE" && a.size() >= 2)
	{
		// MOVIE:<name>: plays that movie (the intro movie is skipped in tests); EXPECT movieplaying says if it still runs.
		// MOVIE:none: from now on no movie opens, as when the converter could not decode them
		if (a[1] == "none")
		{
			s.testNoMovies = true;
			return true;
		}
		return s.playMovie(a[1]) ? true : fail("no movie " + a[1]);
	}
	if (verb == "ALARM" && a.size() >= 2)
	{
		// ALARM:<npc>: they are alarmed (fighting), for the filters that ask
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].ai = AI_COMBAT;
		return true;
	}
	// SETALARM:<npc>:<n>: their AI Alarm (100 and over: they report a crime they see)
	if (verb == "SETALARM" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].alarm = atoi(a[2].c_str());
		return true;
	}
	// FATIGUEREGEN:<0|1>: fatigue comes back by itself or not (a test of per-second Fatigue effects needs it off)
	if (verb == "FATIGUEREGEN" && a.size() >= 2)
	{
		s.testNoFatigueRegen = atoi(a[1].c_str()) == 0;
		return true;
	}
	// HEALTHMAX:<n>: the player's Health and its maximum now n (a passive target that outlasts a long fight)
	if (verb == "HEALTHMAX" && a.size() >= 2)
	{
		float n = (float)atof(a[1].c_str());
		w.stats.healthBonus += n - w.stats.healthMax;
		w.stats.healthMax = n;
		w.stats.health = n;
		return true;
	}
	// TRACE:<id>: log that actor every frame ("trace:" lines); TRACE:off stops
	if (verb == "TRACE" && a.size() >= 2)
	{
		s.traceActor = a[1] == "off" ? "" : a[1];     // (% is a space; an id's own underscores stay)
		for (auto& ch : s.traceActor)
			if (ch == '%')
				ch = ' ';
		for (auto& ch : s.traceActor)
			ch = (char)tolower((unsigned char)ch);
		s.traceTime = 0.0f;
		if (!s.traceActor.empty())
			logf("trace: begin %s", s.traceActor.c_str());
		return true;
	}
	// SEED:<n>: the random rolls (magnitudes, resistance, reflect, dispel) follow that seed from here
	if (verb == "SEED" && a.size() >= 2)
	{
		srand((unsigned)atoi(a[1].c_str()));
		return true;
	}
	// GIVEPOTION:<id>[:<n>]: the player carries n more   USE:<id>: drinks / eats one (each drink is its own application)
	if ((verb == "GIVEPOTION" || verb == "USE") && a.size() >= 2)
	{
		std::string id = w.game.object(a[1]) ? a[1] : spaced(a[1]);
		if (!w.game.object(id))
			return fail("no item " + a[1]);
		if (verb == "GIVEPOTION")
			w.addItem(id, a.size() >= 3 ? atoi(a[2].c_str()) : 1);
		else if (w.itemCount(id) > 0)
			s.consume(id);
		else
			return fail(a[1] + " not carried");
		return true;
	}
	if (verb == "ROLL" && a.size() >= 2)
	{
		s.persuadeRoll = atoi(a[1].c_str());       // the die roll persuasion and enchanting use until ROLL:-1
		return true;
	}
	// RESISTROLL:<0..99>: the die the player, a creature or an NPC resists the spell effects they are hit by with, until
	// RESISTROLL:-1
	if (verb == "RESISTROLL" && a.size() >= 2)
	{
		s.resistDie = atoi(a[1].c_str());
		return true;
	}
	// SETWEATHER:<id>: this region's weather at once. CHANGEWEATHER:<region>:<id> and MODREGION:<region>:<c0,c1,..>
	// are the script functions ('%' in the region stands for a space; a short list leaves the rest 0)
	// MAPCELL:gx:gy:file: the player to that outdoor cell's middle, then (its neighbours loaded) the cell from above,
	// as the local map draws it, saved to file (tools/convert/maprender.py: the world map baked from these)
	// MAPSEEN:gx0:gy0:gx1:gy1: those outdoor cells explored on the world map (a shot of the map over towns not walked)
	if (verb == "MAPSEEN" && a.size() >= 5)
	{
		for (int gy = atoi(a[2].c_str()); gy <= atoi(a[4].c_str()); gy++)
			for (int gx = atoi(a[1].c_str()); gx <= atoi(a[3].c_str()); gx++)
				w.mapCells.insert(w.mapCellKey(gx, gy));
		return true;
	}
	if (verb == "MAPCELL" && a.size() >= 4)
	{
		int gx = atoi(a[1].c_str()), gy = atoi(a[2].c_str());
		int ci = w.gridCell(gx, gy);
		if (ci < 0)
			return fail("MAPCELL: no cell " + a[1] + " " + a[2]);
		float pos[3] = { gx * 8192.0f + 4096.0f, gy * 8192.0f + 4096.0f, 4000.0f };
		s.teleportPlayer(ci, pos, 0.0f);
		s.mapCellJob.gx = gx;
		s.mapCellJob.gy = gy;
		s.mapCellJob.path = a[3];
		for (size_t k = 4; k < a.size(); k++)
			s.mapCellJob.path += ":" + a[k];       // (the path's own colon, sdmc:/...)
		s.mapCellJob.frames = 12;
		s.mapCellJob.on = true;
		return true;
	}
	if (verb == "SETWEATHER" && a.size() >= 2 && w.current >= 0)
	{
		w.setWeatherHere(atoi(a[1].c_str()));
		return true;
	}
	if (verb == "CHANGEWEATHER" && a.size() >= 3)
	{
		w.changeWeather(lower(a[1]), atoi(a[2].c_str()));
		return true;
	}
	if (verb == "MODREGION" && a.size() >= 3)
	{
		std::vector<int> ch(10, 0);
		const char* p = a[2].c_str();
		for (int k = 0; k < 10 && *p; k++)
		{
			ch[k] = std::max(0, std::min(100, atoi(p)));
			const char* comma = strchr(p, ',');
			p = comma ? comma + 1 : p + strlen(p);
		}
		w.modRegion(lower(a[1]), ch);
		return true;
	}
	// CON:<line>: one line typed into the console (Options), run as the console runs it: '%' for a space, '^' for a '+'
	// (a harness token can't hold one); what it prints goes to the log as "console:" lines
	if (verb == "CON" && a.size() >= 2)
	{
		std::string line;
		for (size_t k = 1; k < a.size(); k++)
			line += (k > 1 ? ":" : "") + a[k];
		for (auto& ch : line)
			if (ch == '^')
				ch = '+';
		s.console.run(w, s, line);
		return true;
	}
	// CONSEL:<id>: the object the console's lines act on (OpenMW's click on it); CONSEL alone: none
	if (verb == "CONSEL")
	{
		s.console.selected = a.size() >= 2 && !a[1].empty() ? findRef(s, a[1]) : -1;
		if (a.size() >= 2 && !a[1].empty() && s.console.selected < 0)
			return fail(a[1] + " not loaded");
		logf("test: console selects %s", s.console.selected < 0 ? "nothing" : s.console.selectedName(w).c_str());
		return true;
	}
	if (verb == "SETREP" && a.size() >= 2)
	{
		w.pcReputation = atoi(a[1].c_str());
		return true;
	}
	if (verb == "SETDISP" && a.size() >= 3)
	{
		// SETDISP:<npc>:<n>: their disposition towards the player becomes n (their own part moves)
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		for (int pass = 0; pass < 4; pass++)                 // (a value clamped at 0 or 100 takes a second pass)
			w.refs[ri].disposition += atoi(a[2].c_str()) - w.disposition(ri);
		if (s.dlg.open)
			dialogueRefreshTopics(s.dlg, w);      // the open list follows the new mood, as after a persuasion
		logf("test: %s disposition %d", a[1].c_str(), w.disposition(ri));
		return true;
	}
	if (verb == "FILL")
	{
		w.stats.health = w.stats.healthMax;
		w.stats.magicka = w.stats.magickaMax;
		w.stats.fatigue = w.stats.fatigueMax;
		s.testFatiguePin = -1.0f;
		return true;
	}
	if (verb == "PROBE" && a.size() >= 4)
	{
		// PROBE:x:y:z: the floor under that spot in the loaded cells (collision), logged
		float x = (float)atof(a[1].c_str()), y = (float)atof(a[2].c_str()), z = (float)atof(a[3].c_str());
		Scene sc = w.scene();
		std::string found;
		for (Cell* c : sc.cells)
		{
			float fz;
			if (collisionFloor(c->collision, x, y, z + 60.0f, z - 3000.0f, &fz))
				found += " " + std::to_string((int)fz);
		}
		logf("test: probe %.0f %.0f %.0f: floor%s", x, y, z, found.empty() ? " none" : found.c_str());
		return true;
	}
	if (verb == "PROBEUP" && a.size() >= 4)
	{
		// PROBEUP:x:y:z: the first surface straight up from that spot in the loaded cells (collision, up to 4000 over it),
		// logged: what roofs a spot over
		float from[3] = { (float)atof(a[1].c_str()), (float)atof(a[2].c_str()), (float)atof(a[3].c_str()) };
		float to[3] = { from[0], from[1], from[2] + 4000.0f };
		Scene sc = w.scene();
		float best = 1.0f;
		for (Cell* c : sc.cells)
		{
			float t;
			if (collisionRaycast(c->collision, from, to, &t) && t < best)
				best = t;
		}
		if (best < 1.0f)
			logf("test: probeup %.0f %.0f %.0f: roof %.0f", from[0], from[1], from[2], from[2] + best * 4000.0f);
		else
			logf("test: probeup %.0f %.0f %.0f: roof none", from[0], from[1], from[2]);
		return true;
	}
	if (verb == "SCREEN" && a.size() >= 2)
	{
		int scr = screenByName(a[1]);
		if (scr < 0)
			return fail("no screen " + a[1]);
		if (scr == SCR_NONE)
			for (int k = 0; k < 6 && s.screen != SCR_NONE; k++)
				s.closeScreen();
		else if (scr == SCR_SAVES)
		{
			s.saveList = savesList(s.dataDir);       // as the Saves button does
			s.openScreen(SCR_SAVES);
		}
		else
			s.screen = (Screen)scr;
		return true;
	}
	// CLOCKSHIFT:<hours>: the clock saves record moves by that much (negative: back), as a console's clock reset or set back
	if (verb == "CLOCKSHIFT" && a.size() >= 2)
	{
		g_saveClockShiftMs = (s64)(atof(a[1].c_str()) * 3600000.0);
		return true;
	}
	// SAVESEL:newest: pick the newest of the player's own saves in the Saves list (a finger on the row)
	if (verb == "SAVESEL")
	{
		s.saveList = savesList(s.dataDir);
		for (size_t i = 0; i < s.saveList.size(); i++)
			if (!s.saveList[i].autosave && !s.saveList[i].bundled)
			{
				s.list.selected = (int)i;
				return true;
			}
		return fail("no saves of the player's own");
	}
	// TRAVEL:<npc>:<destination>: in their dialogue, the Travel button, then Go on the destination whose name starts so
	// (silt striders, boats, gondolas, the Mages Guild's guides); the move itself happens next frame
	if (verb == "TRAVEL" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !s.talkingTo(ri))
			return fail("not talking to " + a[1]);
		if (w.refs[ri].actor < 0 || w.game.actors[w.refs[ri].actor].travel.empty())
			return fail(a[1] + " doesn't take travellers");
		if (!s.dlg.choices.empty() || s.dlg.goodbye)
			return fail("the dialogue shows no services now");
		if (s.serviceRefused())
			return fail(a[1] + " refuses the service");
		s.openTravel();
		const ActorDef& def = w.game.actors[w.refs[ri].actor];
		std::string want = lower(spaced(a[2])), offered;
		int sel = -1;
		for (size_t k = 0; k < def.travel.size(); k++)
		{
			offered += (k ? ", " : "") + def.travel[k].cell;
			if (sel < 0 && lower(def.travel[k].cell).compare(0, want.size(), want) == 0)
				sel = (int)k;
		}
		if (sel < 0)
		{
			s.barterRef = -1;
			s.screen = SCR_DIALOGUE;
			return fail(a[2] + " not offered (" + offered + ")");
		}
		if (!s.travelGo(sel))
		{
			s.barterRef = -1;
			s.screen = SCR_DIALOGUE;
			return fail("travel to " + def.travel[sel].cell + " refused");
		}
		return true;
	}
	// BARTER:<ref>: the merchant's Barter screen opens, as the dialogue's Barter button does
	if (verb == "BARTER" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !s.isMerchant(ri))
			return fail(a[1] + " doesn't trade");
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		s.openBarter(ri);
		return true;
	}
	// INVSEL:<id>: the Items menu's All tab, its selection on that item (the D-pad walking to it)
	if (verb == "INVSEL" && a.size() >= 2 && s.screen == SCR_INVENTORY)
	{
		s.invTab = 0;
		int at = 0;
		std::string want = lower(spaced(a[1])), want2 = lower(a[1]);
		for (auto& it : w.inventory)
		{
			if (it.id == "gold_001")
				continue;
			if (lower(it.id) == want || lower(it.id) == want2)
			{
				s.grid.selected = at;
				return true;
			}
			at++;
		}
		return fail(a[1] + " is not carried");
	}
	// BARTERSEL:<id>: the barter grid's selection moves to that item (the D-pad walking to it)
	if (verb == "BARTERSEL" && a.size() >= 2 && s.screen == SCR_BARTER && s.barterRef >= 0)
	{
		int at = 0;
		std::string want = lower(spaced(a[1])), want2 = lower(a[1]);
		if (s.barterSell)
			for (auto& it : w.inventory)
			{
				if (!s.merchantTrades(s.barterRef, w.game.object(it.id)))
					continue;
				if (lower(it.id) == want || lower(it.id) == want2)
				{
					s.grid.selected = at;
					return true;
				}
				at++;
			}
		else
			for (auto& g : s.merchantGoods(s.barterRef))
			{
				std::string id = s.merchantGood(g).second;
				if (!s.merchantTrades(s.barterRef, w.game.object(id)))
					continue;
				if (lower(id) == want || lower(id) == want2)
				{
					s.grid.selected = at;
					return true;
				}
				at++;
			}
		return fail(a[1] + " is not in the barter list");
	}
	return false;
}
