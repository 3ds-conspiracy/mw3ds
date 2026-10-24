// A scripted run's step each frame: input, setup, checks and the test driver
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "components/debug/debuglog.hpp"
#include "platform/platform.hpp"
#include "mwinput/inputmanagerimp.hpp"
#include "testdrive/steprunner.hpp"

extern "C" u32 __ctru_heap_size, __ctru_linear_heap_size;

// One frame of a scripted run (autoinput.txt): the step's input, what it sets up and checks, the test driver
void runScriptedStep(Session* session, ScriptedSteps& autoinput, size_t& inputIndex, float& inputTime, float& dt, PlayerInput& in, u32& down, u32& held, UiInput& ui, bool& touching, bool& autoShotNow, TestDriver& driver, bool& chainMode, bool& stepBlocking, bool& stepShot)
{
	ScriptedInput& s = autoinput[inputIndex];
	static float testDt = 1.0f / 30.0f;      // fixed steps so scripted runs are repeatable (DT: sets them)
	if (s.dt > 0.0f)
		testDt = s.dt;
	dt = testDt;
	in = {};
	in.moveX = s.moveX;
	in.moveY = s.moveY;
	in.lookX = s.lookX * lookScale();       // (the step's look is the C-stick's)
	in.attack = (s.keys & KEY_X) && !(s.keys & KEY_ZL);     // ZL + X puts the weapon away
	in.cast = inputTime == 0.0f && (s.keys & KEY_Y);
	in.sneak = s.keys & KEY_L;
	in.up = s.keys & KEY_R;
	in.down = s.keys & KEY_L;
	bool first = inputTime == 0.0f;
	in.jump = s.jump && first;
	down = first ? s.keys : 0;
	held = s.keys;
	in.togglePov = down & KEY_SELECT;
	in.sheathe = (held & KEY_ZL) && (down & KEY_X);
	if (held & KEY_ZL)
	{
		const u32 dirs[4] = { KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT };
		for (int k = 0; k < 4; k++)
			if (down & dirs[k])
				in.quick = k;
	}
	else
	{
		if (held & KEY_DUP) in.lookY += lookScale();
		if (held & KEY_DDOWN) in.lookY -= lookScale();
	}
	// CONTROLS:goldeneye: the step's stick is the Circle Pad and its keys the buttons, through that layout;
	// GYRO:yaw:pitch: the console turning that fast, through gyro aim's switch and sensitivity
	if (first && s.layout >= 0)
	{
		g_testLayout = g_controlLayout = s.layout;
		logf("test: controls %s", s.layout == CONTROLS_GOLDENEYE ? "goldeneye" : s.layout == CONTROLS_XBOX ? "xbox" : "classic");
	}
	if (first && s.frameCap >= 0)
		g_testFrameCap = s.frameCap == 1;
	if (first && s.skin >= 0)
	{
		g_gpuSkinning = s.skin == 1;
		logf("test: actors loaded from here skinned on the %s", s.skin ? "GPU" : "CPU");
	}
	if (first && s.gyroAim >= 0)
	{
		g_gyroAim = s.gyroAim;
		logf("test: gyro aim %d%%", g_gyroAim);
	}
	if (g_controlLayout == CONTROLS_GOLDENEYE)
	{
		PlayerInput ge = {};
		ge.quick = -1;
		ge.lookX = s.lookX * lookScale();
		static u32 lastHeld = 0;
		u32 released = first ? lastHeld & ~held : 0;
		lastHeld = held;
		static GoldenEyeState state;      // (its own: the console's pass above runs every frame too)
		goldenEyeInput(state, ge, s.moveX, s.moveY, held, down, released, dt);
		ge.jump |= s.jump && first;
		in = ge;
	}
	in = invertLook(in);
	if (g_gyroAim > 0)
	{
		in.gyroYaw = s.gyroYaw * g_gyroAim / 100.0f;
		in.gyroPitch = s.gyroPitch * g_gyroAim / 100.0f;
	}
	if (first && !s.door.empty())
	{
		int ref = session->testFindRef(s.door);
		logf("autoinput: door %s %s", s.door.c_str(), session->w.active(ref) ? "used" : "not in this cell");
		if (session->w.active(ref))
			session->playerActivate(ref);
	}
	if (first && s.boost > 0)
		session->testBoost(s.boost);
	if (first && s.jail)
		session->goToJail();
	if (first && s.shot)
		stepShot = true;
	if (first && s.legit)
	{
		session->testLegit = true;
		logf("test: LEGIT: from here only what a player can do");
	}
	if (first && s.chain)
	{
		chainMode = true;
		logf("test: chained run (journal setup only raises)");
		// A bounty the last chapter's driving ran up (a book taken in sight) is paid, as a player would
		// between chapters: else the first guard's arrest screen holds every step after it
		if (session->w.bounty > 0)
		{
			logf("test: chained run: bounty %d paid", session->w.bounty);
			session->w.bounty = 0;
			session->w.arrestDeclined = -1;
		}
		if (session->screen == SCR_ARREST)
			session->closeScreen();
	}
	if (first && !s.startScript.empty())
	{
		// STARTSCRIPT:name:ref: started on an object, as ref->StartScript does
		std::string nm = s.startScript, on;
		size_t colon = nm.find(':');
		if (colon != std::string::npos)
		{
			on = nm.substr(colon + 1);
			nm = nm.substr(0, colon);
		}
		session->w.startGlobalScript(nm, on.empty() ? -1 : session->w.findRef(on));
		logf("test: started script %s", s.startScript.c_str());
	}
	if (first && !s.saveAs.empty())
	{
		std::string p = "sdmc:/3ds/mw3ds/test_" + s.saveAs + ".sav";
		logf("test: saved %s%s: %s", p.c_str(), s.saveLegacy ? " (version 1)" : "",
			session->w.saveGame(p.c_str(), s.saveLegacy) ? "ok" : "FAILED");
	}
	if (first && !s.loadFrom.empty())
	{
		session->reloadPath = "sdmc:/3ds/mw3ds/test_" + s.loadFrom + ".sav";
		session->wantReload = true;
		logf("test: loading %s", session->reloadPath.c_str());
	}
	if (first)
	{
		stepBlocking = false;
		for (auto& act : s.actions)
			stepBlocking |= driver.start(*session, act);
	}
	if (first && s.enchant)
		session->openEnchanting(-1);
	if (first && !s.wear.empty())
		for (auto& it : session->w.inventory)
			if (it.id == s.wear)
				it.condition = s.wearTo;
	if (first && s.repair)
		for (size_t i = 0; i < session->w.inventory.size(); i++)
			if (const Object* o = session->w.game.object(session->w.inventory[i].id))
				if (o->recType == REC_REPA)
				{
					session->openRepair(-1, i);
					break;
				}
	if (first && !s.recharge.empty())
	{
		int item = -1, gem = -1;
		for (size_t i = 0; i < session->w.inventory.size(); i++)
		{
			if (session->w.inventory[i].id == s.recharge)
				item = i;
			if (!session->w.inventory[i].soul.empty() && gem < 0)
				gem = i;
		}
		if (item >= 0 && gem >= 0)
			session->rechargeItem(item, gem);
		else
			logf("test: nothing to recharge %s with", s.recharge.c_str());
	}
	if (first && !s.join.empty())
	{
		if (s.joinRank < 0)
		{
			// JOIN:faction:-1 leaves it (the sweeps undo a quest's setup)
			session->w.pcRank.erase(s.join);
			session->w.pcExpelled.erase(s.join);
			logf("test: left %s", s.join.c_str());
		}
		else
		{
			// (chained: never below the rank the last chapter's story gave)
			auto had = session->w.pcRank.find(s.join);
			if (!chainMode || had == session->w.pcRank.end() || had->second < s.joinRank)
				session->w.pcRank[s.join] = s.joinRank;
			logf("test: joined %s at rank %d", s.join.c_str(), session->w.pcRank[s.join]);
		}
	}
	if (first && !s.cast.empty())
	{
		if (session->testLegit && !TestDriver::canCast(*session, s.cast))
			logf("drive: FAIL cast %s: not a known spell or a carried item (LEGIT)", s.cast.c_str());
		else
		{
			// the id as given when there is such a spell (or item), else with '_' as spaces
			std::string sel = s.cast;
			if (sel.compare(0, 5, "item:") != 0 && !session->w.game.spells.count(lower(sel)))
			{
				std::string sp = lower(sel);
				std::replace(sp.begin(), sp.end(), '_', ' ');
				if (session->w.game.spells.count(sp))
					sel = sp;
			}
			session->w.stats.selectedSpell = sel;
			session->castSpell();
		}
	}
	if (first && !s.soulKill.empty())
	{
		int ref = session->testFindRef(s.soulKill);
		// a living one (several can share the id), when the one found (the test's own placed one first) isn't
		std::string want = session->w.refs.size() && ref >= 0 ? session->w.refs[ref].idLower : "";
		bool alive = session->w.active(ref) && !session->w.refs[ref].dead;
		for (size_t i = 0; i < session->w.refs.size() && ref >= 0 && !alive; i++)
			if (session->w.refs[i].idLower == want && !session->w.refs[i].dead && session->w.active(i))
			{
				ref = i;
				break;
			}
		if (session->w.active(ref) && !session->w.refs[ref].dead)
		{
			session->w.refs[ref].soulTrapUntil = session->w.time + 30.0f;
			session->killNpc(ref, false);          // no witness, no crime: the test isn't about the law
		}
		logf("test: soulkill %s %s", s.soulKill.c_str(), session->w.active(ref) ? "done" : "not loaded");
	}
	if (first && !s.brew.empty())
	{
		session->alchemySlots.clear();
		std::string rest = s.brew;
		while (!rest.empty())
		{
			size_t c = rest.find(':');
			session->alchemySlots.push_back(rest.substr(0, c));
			rest = c == std::string::npos ? "" : rest.substr(c + 1);
		}
		// (the screen offers only what is carried: one that isn't is the case's mistake, not a potion)
		for (auto& id : session->alchemySlots)
			if (session->w.itemCount(id) <= 0)
				logf("drive: FAIL BREW: %s not carried", id.c_str());
		session->brewPotion();
	}
	// SLEEPUNTIL:h: whole hours from now to the next time the clock reads h (a full day when it already does)
	int sleepHours = s.sleep;
	if (first && s.sleepUntil >= 0)
	{
		int now = (int)floorf(fmodf(session->w.gameHour, 24.0f));
		sleepHours = ((s.sleepUntil % 24) - now + 24) % 24;
		if (sleepHours == 0)
			sleepHours = 24;
	}
	// (a rest still running from the step before, whose seconds were shorter: not started over)
	if (first && sleepHours > 0 && session->restDone >= 0 && session->screen == SCR_REST)
		logf("drive: SLEEP %d ignored: a rest is still running (%d of %d hours)", sleepHours, session->restDone, session->restTotal);
	else if (first && sleepHours > 0)
	{
		// (a bed's rest menu already open: its Rest, asleep there even where sleeping is illegal)
		if (!(session->screen == SCR_REST && session->restBed))
			session->openRest(false);
		// (SLEEP sleeps where it is, as it always has: in a town's streets, where the rest menu only waits since the
		// exteriors' no-sleep flag is read, too. RESTMENU opens the menu as the player does)
		if (session->screen == SCR_REST)
			session->restSleep = true;
		session->startRest(sleepHours);
		if (s.sleepForce == 2)
			session->restInterruptAt = -1;     // 2: nothing comes
		else if (s.sleepForce && session->restInterruptList.empty())
		{
			const LevelCell& lc = session->w.cells[session->w.current];
			auto rg = session->w.game.regions.find(lc.region);
			if (rg != session->w.game.regions.end())
				session->restInterruptList = rg->second.sleep;
		}
		if (s.sleepForce == 1)
			session->restInterruptAt = 1;
	}
	if (first && s.level > 0)
		session->w.stats.level = s.level;
	if (first && s.weather >= 0 && session->w.current >= 0)
	{
		World& ww = session->w;
		ww.setWeatherHere(s.weather);
		logf("test: weather %d in %s", s.weather, ww.cells[ww.current].region.c_str());
	}
	if (first && s.persuade >= 0 && session->dlg.open && !session->dlg.choices.empty())
		logf("drive: FAIL persuade %d ignored: a choice is open (%d choices): answer it first (CHOICE)", s.persuade, (int)session->dlg.choices.size());
	else if (first && s.persuade >= 0 && session->dlg.open)
		session->persuade(s.persuade);
	if (first && !s.tool.empty())
	{
		int ref = session->testFindRef(s.tool);
		InventoryItem* t = session->playerToolItem();
		if (session->w.active(ref) && t)
		{
			session->target = ref;
			session->useTool(*t);
		}
		else
			logf("test: no tool or %s not loaded", s.tool.c_str());
	}
	for (int k = 0; first && k < s.skillUps; k++)
		session->raiseSkill(s.skillUp);
	if (first && s.days > 0)
		session->w.gameHour += 24.0f * s.days;
	if (first && s.hour >= 0.0f)
	{
		session->w.gameHour = floorf(session->w.gameHour / 24.0f) * 24.0f + s.hour;
		session->w.weatherSeen = session->w.gameHour;      // (setting the hour is not time passing)
	}
	if (first && s.rep >= 0)
		session->w.pcReputation = s.rep;
	if (first && !s.setLocal.empty())
	{
		// id:var:value (id may hold underscores for spaces)
		size_t a = s.setLocal.find(':'), b = s.setLocal.rfind(':');
		if (a != std::string::npos && b > a)
		{
			std::string id = s.setLocal.substr(0, a), var = s.setLocal.substr(a + 1, b - a - 1);
			float v = atof(s.setLocal.c_str() + b + 1);
			int ri = session->w.findRefAnywhere(id);
			if (ri < 0)
			{
				std::string spaced = id;
				std::replace(spaced.begin(), spaced.end(), '_', ' ');
				ri = session->w.findRefAnywhere(spaced);
			}
			float* p = ri >= 0 && session->w.refs[ri].script >= 0 ? session->w.scripts[session->w.refs[ri].script].local(var) : nullptr;
			if (p)
				*p = v;
			logf("test: local %s.%s = %g%s", id.c_str(), var.c_str(), v, p ? "" : " (no such local)");
		}
	}
	if (first && !s.drop.empty())
	{
		World& ww = session->w;
		std::string id = lower(s.drop);
		if (ww.itemCount(id) > 0)
		{
			ww.removeItem(id, 1);
			ww.dropItemAt({ id, 1, false }, ww.player.feet, ww.player.yaw, 50.0f);
		}
		else
			logf("test: no %s to drop", id.c_str());
	}
	if (first && !s.setGlobal.empty())
	{
		session->w.setGlobal(s.setGlobal, s.globalValue);
		logf("test: global %s = %g", s.setGlobal.c_str(), s.globalValue);
	}
	if (first && !s.talk.empty())
	{
		World& ww = session->w;
		int ri = session->testFindRef(s.talk);
		if (ri < 0 || !ww.active(ri))
			logf("test: talk %s: not here", s.talk.c_str());
		else if (!ww.refs[ri].visible() || ww.refs[ri].dead)
			logf("test: talk %s: disabled or dead", s.talk.c_str());
		else
		{
			Ref& r = ww.refs[ri];
			if (r.ai == AI_COMBAT)
			{
				// an earlier test left them fighting (the sweep runs quests back to back)
				logf("test: talk %s: was fighting, calmed", s.talk.c_str());
				r.ai = AI_IDLE;
				r.fight = 0;
			}
			float fx = sinf(r.rot[2]), fy = cosf(r.rot[2]);
			float to[4] = { r.pos[0] + fx * 90.0f, r.pos[1] + fy * 90.0f, r.pos[2] + 10.0f, 0.0f };
			Player& p = ww.player;
			memcpy(p.feet, to, sizeof(p.feet));
			p.yaw = atan2f(r.pos[0] - to[0], r.pos[1] - to[1]);
			p.vz = 0.0f;
			p.fallTop = p.feet[2];
			p.landedFall = 0.0f;
			r.disposition = 100;
			session->playerActivate(ri);
			logf("test: talk %s (%s)", s.talk.c_str(), session->dlg.open ? "talking" : "no talk");
		}
	}
	if (first && s.quickSet.size() > 2)
	{
		session->w.stats.quickKeys[(s.quickSet[0] - '0') & 3] = s.quickSet.substr(2);
		logf("test: quick key %s", s.quickSet.c_str());
	}
	if (first && !s.spell.empty())
	{
		PlayerStats& ps = session->w.stats;
		std::string sid = lower(s.spell);
		if (!session->w.game.spells.count(sid))
			std::replace(sid.begin(), sid.end(), '_', ' ');    // ids with spaces: "vampire_blood_aundae"
		if (std::find(ps.spells.begin(), ps.spells.end(), sid) == ps.spells.end())     // (known once, as AddSpell)
			ps.spells.push_back(sid);
		float hp = ps.health;
		session->w.recomputeStats();
		ps.health = fminf(ps.healthMax, hp);
		logf("test: spell %s", sid.c_str());
	}
	if (first && s.effect >= 0)
	{
		// (the last number is the skill for the skill effects: fortify, drain, damage, restore, absorb skill)
		bool skillEffect = s.effect == 83 || s.effect == 21 || s.effect == 26 || s.effect == 78 || s.effect == 89;
		SpellEffect e = { s.effect, skillEffect ? s.effectAttr : -1, skillEffect ? -1 : s.effectAttr, s.effectMag, s.effectMag, s.effectSecs, 0 };
		session->applyEffectToPlayer(e, "test");
	}
	if (first && !s.moveTo.empty())
	{
		World& w = session->w;
		int ri = w.findRefAnywhere(s.moveTo);
		if (ri < 0)
			ri = w.findRefAnywhere(std::string(s.moveTo).replace(s.moveTo.find('_') == std::string::npos ? 0 : s.moveTo.find('_'), s.moveTo.find('_') == std::string::npos ? 0 : 1, " "));
		float pos[3] = { w.player.feet[0] + sinf(w.player.yaw) * 200.0f, w.player.feet[1] + cosf(w.player.yaw) * 200.0f,
			w.player.feet[2] };
		if (ri >= 0)
			w.relocate(ri, w.current, pos, w.player.yaw + 3.14159265f);
		else
			logf("test: %s not found", s.moveTo.c_str());
	}
	if (first && !s.placePC.empty())
		session->w.placeAt(s.placePC, 1, 256.0f, 0, session->w.player.feet, session->w.player.yaw);
	if (first && !s.follow.empty())
	{
		int ri = session->testFindRef(s.follow);
		if (ri >= 0)
		{
			session->w.refs[ri].aiPackage = AIPKG_FOLLOW;
			session->w.refs[ri].aiTarget = "player";
			session->w.refs[ri].aiDone = false;
			session->w.refs[ri].fight = 0;       // a companion, not a creature about to attack
			session->w.refs[ri].ai = AI_IDLE;
		}
	}
	if (first && s.evict >= 0)
		session->evictFarCells(s.evict, true);
	if (first && !s.check.empty())
	{
		int ri = session->testFindRef(s.check);
		if (s.check.compare(0, 8, "journal:") == 0)
		{
			std::string q = s.check.substr(8);
			for (auto& ch : q)
				ch = tolower(ch);
			auto it = session->w.journalIndex.find(q);
			logf("check: journal %s = %d", q.c_str(), it != session->w.journalIndex.end() ? it->second : 0);
		}
		else if (s.check.compare(0, 7, "global:") == 0)
		{
			std::string g = s.check.substr(7);
			for (auto& ch : g)
				ch = tolower(ch);
			auto it = session->w.globals.find(g);
			logf("check: global %s = %g", g.c_str(), it != session->w.globals.end() ? it->second : 0.0f);
		}
		else if (s.check == "player")
		{
			const PlayerStats& ps = session->w.stats;
			logf("check: player in %s at %.0f %.0f %.0f str %d int %d health %.0f/%.0f effects %d",
				session->w.cellName().c_str(), session->w.player.feet[0], session->w.player.feet[1],
				session->w.player.feet[2], ps.attributes[0], ps.attributes[1], ps.health, ps.healthMax,
				(int)session->w.effects.size());
		}
		else if (ri < 0)
			logf("check: %s not in memory", s.check.c_str());
		else
			logf("check: %s cell %s enabled %d taken %d dead %d at %.0f %.0f %.0f rot %.2f %.2f %.2f ai %d done %d", s.check.c_str(),
				session->w.cells[session->w.refs[ri].cell].file.c_str(), session->w.refs[ri].enabled,
				session->w.refs[ri].pickedUp, session->w.refs[ri].dead, session->w.refs[ri].pos[0],
				session->w.refs[ri].pos[1], session->w.refs[ri].pos[2], session->w.refs[ri].rot[0], session->w.refs[ri].rot[1],
				session->w.refs[ri].rot[2], session->w.refs[ri].aiPackage,
				session->w.refs[ri].aiDone);
		if (ri >= 0 && session->w.refs[ri].actor >= 0)
		{
			const Ref& cr = session->w.refs[ri];
			std::string p;
			for (int k : cr.path)
				p += " " + std::to_string((int)session->w.pathPoints[k].pos[0]) + "," + std::to_string((int)session->w.pathPoints[k].pos[1]);
			logf("check: %s moving: path%s stuck %.2f grid %d ai-state %d", s.check.c_str(), p.empty() ? " (straight)" : p.c_str(),
				cr.stuckTimer, session->w.time < cr.gridUntil, cr.ai);
		}
		// What it holds (containers, NPCs)
		if (ri >= 0 && !session->w.refs[ri].contents.empty())
		{
			std::string held;
			for (auto& c : session->w.refs[ri].contents)
				held += " " + std::to_string(c.first) + "x" + c.second;
			logf("check: %s holds%s", s.check.c_str(), held.substr(0, 200).c_str());
		}
		// Its script's locals (what a scripted scene counted)
		int sc = ri >= 0 ? session->w.refs[ri].script : -1;
		if (sc >= 0 && sc < (int)session->w.scripts.size() && session->w.scripts[sc].script)
		{
			const ScriptInstance& si = session->w.scripts[sc];
			std::string text;
			for (size_t k = 0; k < si.locals.size() && k < si.script->localNames.size(); k++)
				text += " " + si.script->localNames[k] + "=" + std::to_string((int)si.locals[k]);
			logf("check: %s script %s%s", s.check.c_str(), si.script->name.c_str(), text.c_str());
		}
	}
	if (first && !s.give.empty())
		session->testGive(s.give, s.giveCount, chainMode);
	if (first && !s.place.empty())
		session->testPlace(s.place, s.placeDist);
	if (first && !s.hit.empty())
		session->testHit(s.hit);
	if (first && !s.journal.empty())
	{
		// CHAIN (a chapter run from the last one's save): setup only raises, never undoes progress
		if (!chainMode || session->w.getJournal(s.journal) < s.journalIndex)
			session->w.setJournal(s.journal, s.journalIndex);
		logf("test: journal %s %d", s.journal.c_str(), s.journalIndex);
	}
	if (first && !s.gotoCell.empty())
		session->testGoto(s.gotoCell);
	if (first && s.choiceValue > 0 && session->dlg.open)
	{
		bool found = false;
		for (auto& ch : session->dlg.choices)
			if (ch.second == s.choiceValue)
			{
				logf("autoinput: choice value %d (%s)", s.choiceValue, ch.first.c_str());
				dialogueChoose(session->dlg, session->w, *session, ch.second);
				found = true;
				break;
			}
		if (!found)
			logf("autoinput: choice value %d not offered (%d choices)", s.choiceValue, (int)session->dlg.choices.size());
	}
	if (first && s.choice > 0 && session->dlg.open)
	{
		// As in the game: only a choice the talk offers (the k-th button)
		if (s.choice <= (int)session->dlg.choices.size())
		{
			int value = session->dlg.choices[s.choice - 1].second;
			logf("autoinput: choice %d (%s)", s.choice, session->dlg.choices[s.choice - 1].first.c_str());
			dialogueChoose(session->dlg, session->w, *session, value);
		}
		else
			logf("autoinput: choice %d not offered (%d choices)", s.choice, (int)session->dlg.choices.size());
	}
	if (first && !s.topic.empty() && session->dlg.open)
	{
		logf("autoinput: topic %s (available: %d)", s.topic.c_str(), (int)session->dlg.topics.size());
		// A player can only pick what the list shows: a topic not in it is a failure (asked anyway, so
		// the rest of the test still runs)
		const Topic* t = session->w.game.topic(s.topic);
		bool listed = false;
		for (auto& name : session->dlg.topics)
			listed |= t && name == t->name;
		if (t && !listed)
		{
			std::string by = dialogueNamedBy(t->lower);
			logf("drive: FAIL topic %s not offered (%s%s%s, %s)", t->name.c_str(),
				session->w.knownTopics.count(t->lower) ? "known" : "never learned",
				by.empty() ? "" : "; named by ", by.empty() ? "" : (by + ", who had no answer").c_str(),
				dialogueFindInfo(session->w, *t, session->dlg.ref, -1) ? "the speaker has an answer" : "no answer from the speaker");
		}
		if (!session->dlg.choices.empty())
			logf("drive: FAIL topic %s ignored: a choice is open (%d choices): answer it first (CHOICE)", s.topic.c_str(),
				(int)session->dlg.choices.size());
		// (LEGIT: not asked; a player has no way to)
		if (listed || !t || !session->testLegit)
			dialogueTopic(session->dlg, session->w, *session, s.topic);
	}
	if (s.waitMessage && !session->messages.empty())
	{
		logf("autoinput: dismiss \"%.50s\"", session->messages.front().text.c_str());
		down = held = KEY_A;
		inputTime = s.secs;
	}
	if (first && s.camSet >= 0)
	{
		g_camHeld = s.camSet >= 1;
		const float* at = s.camSet == 2 ? session->w.player.feet : nullptr;     // (CAMREL: from the player's feet)
		g_heldCam = RenderCamera{ { s.cam[0] + (at ? at[0] : 0.0f), s.cam[1] + (at ? at[1] : 0.0f), s.cam[2] + (at ? at[2] : 0.0f) },
			C3D_AngleFromDegrees(s.cam[3]), C3D_AngleFromDegrees(s.cam[4]) };
	}
	if (first && s.hud >= 0)
		g_hudOff = s.hud == 0;
	if (first && s.halt >= 0)
		g_halt = s.halt == 1;
	if (first && s.teleport && !s.tpInside && session->w.current >= 0 && session->w.cells[session->w.current].interior)
	{
		// from inside to world coordinates: out to the exterior cell there (as a door would)
		int dest = session->w.gridCell((int)floorf(s.tp[0] / 8192.0f), (int)floorf(s.tp[1] / 8192.0f));
		if (dest >= 0)
		{
			session->travelCell = dest;
			memcpy(session->travelPos, s.tp, sizeof(session->travelPos));
			session->travelYaw = C3D_AngleFromDegrees(s.tp[3]);
			logf("test: out to %s", session->w.cells[dest].name.c_str());
		}
	}
	else if (first && s.teleport)
	{
		Player& p = session->w.player;
		p.feet[0] = s.tp[0];
		p.feet[1] = s.tp[1];
		p.feet[2] = s.tp[2];
		p.yaw = C3D_AngleFromDegrees(s.tp[3]);
		p.pitch = 0.0f;
		p.vz = 0.0f;
		p.fallTop = p.feet[2];         // a test teleport isn't a fall
		p.landedFall = 0.0f;
	}
	driver.keepFloating(*session);
	if (driver.busy())
		driver.update(*session, in, down, dt);
	touching = false;
	if (first && s.tapX >= 0)
	{
		ui.tapped = true;
		ui.tapX = s.tapX;
		ui.tapY = s.tapY;
	}
	if (first && s.drag)
	{
		ui.touching = true;
		ui.touchX = 160;
		ui.touchY = 100;
		ui.dragDX = s.dragX;
		ui.dragDY = s.dragY;
	}
	// A finger held (TOUCH): down at the step's spot every frame, slid there from where the step before held it;
	// a step without one lifts it
	static int heldX = -1, heldY = -1;
	if (s.touchX >= 0)
	{
		ui.touching = true;
		ui.touchX = s.touchX;
		ui.touchY = s.touchY;
		if (heldX >= 0)
		{
			ui.dragDX = s.touchX - heldX;
			ui.dragDY = s.touchY - heldY;
		}
	}
	heldX = s.touchX;
	heldY = s.touchY;
	inputTime += dt;
	if (first)
	{
		Player& p = session->w.player;
		logf("autoinput: step %d [%s] feet %.0f %.0f %.0f %s screen %d msgs %d ctrl %d target %s pending %d", (int)inputIndex,
			session->w.cellName().c_str(), p.feet[0], p.feet[1], p.feet[2],
			p.swimming ? "water" : p.onGround ? "ground" : "air", session->screen, (int)session->messages.size(),
			session->w.controlsEnabled, session->target >= 0 ? session->w.refs[session->target].id.c_str() : "-",
			(int)session->pendingMenus.size());
	}
	// (a flight takes as long as its distance needs: the step's seconds are a guess, FLYTO knows better)
	float stepSecs = s.secs;
	if (driver.busy() && driver.kind == TestDriver::FLY && driver.flyAllow > stepSecs)
		stepSecs = driver.flyAllow;
	if (driver.busy() && inputTime >= stepSecs)
		driver.fail("timeout");
	if (inputTime >= stepSecs || (stepBlocking && !driver.busy()))
	{
		if (inputTime >= 15.0f)
			logf("autoinput: step %zu took %.1f s (limit %.0f)", (size_t)inputIndex, inputTime, stepSecs);
		stepBlocking = false;
		inputIndex++;
		inputTime = 0.0f;
		if (inputIndex == autoinput.size())
		{
			logf("autoinput: end feet %.0f %.0f %.0f screen %d", session->w.player.feet[0], session->w.player.feet[1],
				session->w.player.feet[2], session->screen);
			autoShotNow = true;
			session->save();
			platformRunOver();         // (the PC test build ends here; the console carries on, into QUIT below)
			// QUIT: the game menu's Save and quit from here, the way a player leaves. The quit on the console ran
			// code no test did: a crash at exit froze the console on a black screen
			if (s.quit)
			{
				session->openScreen(SCR_GAMEMENU);
				session->quitSaving = 1;
			}
		}
	}
}
