// Tokens: made spells and items, actors' state, journal, NPC casting
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
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "testdrive/opshelpers.hpp"

std::optional<bool> opsItems(Session& s, const std::vector<std::string>& a)
{
	World& w = s.w;
	const std::string& verb = a[0];
	auto fail = [&](const std::string& why) { logf("drive: FAIL %s: %s", verb.c_str(), why.c_str()); return true; };
	if (verb == "MAKESPELL" && a.size() >= 2)
	{
		// MAKESPELL:name: the effects ADDEFFECT put together become a spell the player knows, selected (and a made spell:
		// a checkpoint carries it)
		makeTestSpell(s, a[1], s.makeEffects);
		w.stats.selectedSpell = "mw3ds_test_" + lower(a[1]);
		s.makeEffects.clear();
		return true;
	}
	// MAKEITEM:<base item>:<id>[:use|constant|once][:charge]: the effects ADDEFFECT put together become an enchantment on a
	// copy of the base item, carried (CAST:item:<id> casts it). Enchantment effects are never dispelled; "use" ones are
	// active effects (a Fortify goes past 100), "constant" ones count as abilities while worn (capped at 100)
	if (verb == "MAKEITEM" && a.size() >= 3)
	{
		if (s.makeEffects.empty())
			return fail("no effects (ADDEFFECT first)");
		std::string kind = a.size() >= 4 ? lower(a[3]) : "use";
		int type = kind == "constant" ? ENCH_CONSTANT : kind == "once" ? ENCH_ONCE : ENCH_USE;
		if (!makeTestItem(s, a[1], a[2], type, a.size() >= 5 ? atoi(a[4].c_str()) : 999999, s.makeEffects))
			return fail("no item " + a[1]);
		s.makeEffects.clear();
		return true;
	}
	// ENHANCEV2[:key=value ...]: the v2 test kit (internal/uber-v2-handoff.md): a cast-when-used ring, cast once, that
	// fortifies Strength, Speed, the six schools and the magicka pool for the whole run, and the fly item. Keys:
	// speed strength schools magicka (the ring's Fortify points), levitate fly (the fly item's magnitude and seconds a use),
	// charge, ring (the base item). Sets the kit flag the driver's v2 behaviours read (kitV2)
	if (verb == "ENHANCEV2")
	{
		int speed = 400, strength = 900, schools = 900, magicka = 500000, levitate = 500, fly = 1, charge = 999999;   // (fly: seconds a use)
		std::string ring = "expensive_ring_01";
		for (size_t k = 1; k < a.size(); k++)
		{
			size_t eq = a[k].find('=');
			std::string key = a[k].substr(0, eq), val = eq == std::string::npos ? "" : a[k].substr(eq + 1);
			if (key == "speed") speed = atoi(val.c_str());
			else if (key == "strength") strength = atoi(val.c_str());
			else if (key == "schools") schools = atoi(val.c_str());
			else if (key == "magicka") magicka = atoi(val.c_str());
			else if (key == "levitate") levitate = atoi(val.c_str());
			else if (key == "fly") fly = atoi(val.c_str());
			else if (key == "charge") charge = atoi(val.c_str());
			else if (key == "ring") ring = val;
			else return fail("unknown key " + key);
		}
		const int kForever = 1000000;        // seconds: 277 hours of game time, past any run's sleeps and travels
		std::vector<SpellEffect> fx;
		auto add = [&](int effect, int magnitude, int seconds, int attribute, int skill) {
			SpellEffect e = {};
			e.effect = effect;
			e.min = e.max = magnitude;
			e.duration = seconds;
			e.range = 0;
			e.attribute = attribute;
			e.skill = skill;
			fx.push_back(e);
		};
		add(79, strength, kForever, ATTR_STRENGTH, -1);
		add(79, speed, kForever, ATTR_SPEED, -1);
		static const int kSchools[6] = { 11, 13, 10, 12, 14, 15 };   // alteration .. restoration (mwmechanics/spellcasting.cpp kSchoolSkill)
		for (int sk : kSchools)
			add(83, schools, kForever, -1, sk);
		add(81, magicka, kForever, -1, -1);                            // Fortify Magicka: the pool and its maximum at once
		// common and blight disease resisted outright: a blighted creature's hit gave one by the roll, and every talk after it
		// was "keep your distance" (not Corprus: the main quest needs it)
		add(94, 100, kForever, -1, -1);
		add(95, 100, kForever, -1, -1);
		// and paralysis: a paralysing blade (Ranes Ienith's jinkblade) held the player still blow after blow, past the kill's time
		add(99, 100, kForever, -1, -1);
		// and Silence and the rest Resist Magicka covers: a silencing blade (Navil Ienith's) left the Recall after the kill uncast
		add(93, 100, kForever, -1, -1);
		if (!makeTestItem(s, ring, "uber_ring", ENCH_USE, charge, fx))
			return fail("no item " + ring);
		s.castItem("uber_ring");                                       // (an item's cast lands at once: no animation)
		// the fly item: Levitate for a second a use (cast when used: no roll, no magicka, no animation). FLYTO uses it again
		// whenever less than half a second is left, so a flight never runs out and the levitation is gone a second after it
		fx.clear();
		add(10, levitate, fly, -1, -1);
		if (!makeTestItem(s, ring, "uber_fly", ENCH_USE, charge, fx))
			return fail("no item " + ring);
		// the hide item: Chameleon 300 for a second a use (the awareness check takes Chameleon uncapped: v1 stacked ten
		// potions, 200, for the guarded chests) (HIDE keeps it on, as FLOAT the fly item: v1's chameleon potions
		// lasted their 60 s and a caravaner, talked to still hidden, said "Who's there?" and offered no travel)
		fx.clear();
		add(40, 300, 1, -1, -1);
		if (!makeTestItem(s, ring, "uber_hide", ENCH_USE, charge, fx))
			return fail("no item " + ring);
		// the command item: Command Humanoid 100 for 120 s on touch, a use; KILL:<id>:unseen leads the one to kill away with it
		// from a busy place (no blow while anyone is within earshot, and Ald-ruhn's gate never emptied round Braynas Hlervu)
		fx.clear();
		add(119, 100, 120, -1, -1);
		fx.back().range = 1;
		if (!makeTestItem(s, ring, "uber_command", ENCH_USE, charge, fx))
			return fail("no item " + ring);
		s.testKit = 2;
		float flySpeed = w.game.gmstf("fminflyspeed", 5.0f) + 0.01f * (w.stats.attributes[ATTR_SPEED] + levitate)
			* (w.game.gmstf("fmaxflyspeed", 300.0f) - w.game.gmstf("fminflyspeed", 5.0f));
		logf("test: ENHANCEV2: Strength %d, Speed %d, schools +%d, magicka %.0f; run %.0f, fly %.0f units a second",
			(int)w.stats.attributes[ATTR_STRENGTH], (int)w.stats.attributes[ATTR_SPEED], schools, (float)w.stats.magickaMax,
			s.runSpeedFor((int)w.stats.attributes[ATTR_SPEED], (int)w.stats.skills[8]), flySpeed);
		return true;
	}
	if (verb == "USEMADE" || verb == "EQUIPMADE" || verb == "RECHARGEMADE")
	{
		const InventoryItem* it = lastMade(w);
		if (!it)
			return fail("no made item carried");
		std::string id = it->id;
		int index = carried(w, id);
		if (verb == "USEMADE")
			s.castItem(id);
		else if (verb == "EQUIPMADE")
		{
			if (!w.inventory[index].equipped)
				s.equipItem(index);
		}
		else
		{
			int gem = -1;
			for (size_t k = 0; k < w.inventory.size(); k++)
				if (!w.inventory[k].soul.empty() && gem < 0)
					gem = (int)k;
			if (gem < 0)
				return fail("no soul gem holding a soul");
			s.rechargeItem(index, gem);
		}
		logf("test: %s %s", verb.c_str(), id.c_str());
		return true;
	}
	if (verb == "DRINKBREWED")
	{
		for (int k = (int)w.brewed.size() - 1; k >= 0; k--)
			if (w.itemCount(w.brewed[k]) > 0)
			{
				std::string id = w.brewed[k];
				s.consume(id);
				logf("test: drank %s", id.c_str());
				return true;
			}
		return fail("no brewed potion carried");
	}
	if (verb == "CASTMADESPELL")
	{
		if (w.madeSpells.empty())
			return fail("no spell made");
		w.stats.selectedSpell = w.madeSpells.back();
		s.castSpell();
		logf("test: cast %s", w.madeSpells.back().c_str());
		return true;
	}
	if (verb == "ATTRUPS" && a.size() >= 3)
	{
		int k = attrIndex(w, a[1]);
		if (k < 0)
			return fail("no attribute " + a[1]);
		w.stats.attrSkillUps[k] = atoi(a[2].c_str());
		return true;
	}
	if (verb == "ACTIVE" && a.size() >= 3)
	{
		// an active effect on the player at once, with no resistance roll: ACTIVE:<effect>:<magnitude>[:<seconds>]
		ActiveEffect e = { atoi(a[1].c_str()), -1, -1, (float)atof(a[2].c_str()), a.size() >= 4 ? (float)atof(a[3].c_str()) : 1e6f, "test" };
		w.effects.push_back(e);
		return true;
	}
	if (verb == "SETFATIGUE" && a.size() >= 2)
	{
		w.stats.fatigue = w.stats.fatigueMax * (float)atof(a[1].c_str());     // a fraction of the maximum
		s.testFatiguePin = w.stats.fatigue;     // held there: the checks that follow expect this level
		return true;
	}
	// SETMAGICKA:<points>: the player's Magicka now (above the maximum too: a run of casts for side-by-side shots)
	if (verb == "SETMAGICKA" && a.size() >= 2)
	{
		w.stats.magicka = (float)atof(a[1].c_str());
		return true;
	}
	// SETHEALTH:<fraction>: the player's Health, a fraction of the maximum
	if (verb == "HOSTILE" && a.size() >= 2)
	{
		// HOSTILE:<id>: that actor goes for the player, as if provoked (its AI's combat with the player)
		int ri = findRef(s, a[1]);
		if (ri < 0 || w.refs[ri].actor < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].aggressor = true;
		s.makeHostile(ri);
		return true;
	}
	// REFHEALTH:<id>:<fraction>: that actor's health, a fraction of its maximum (hurt before a fight)
	if (verb == "REFHEALTH" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || w.refs[ri].actor < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].health = w.refs[ri].healthMax * (float)atof(a[2].c_str());
		return true;
	}
	// REFADDITEM:<id>:<item>[:<count>]: into that actor's or container's own contents (AddItem on it)
	if (verb == "REFADDITEM" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		std::string id = w.game.object(a[2]) ? a[2] : spaced(a[2]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		if (!w.game.object(id))
			return fail("no item " + a[2]);
		w.refAddItem(w.refs[ri], id, a.size() >= 4 ? atoi(a[3].c_str()) : 1);
		return true;
	}
	if (verb == "TRUNCSAVE" && a.size() >= 3)
	{
		// TRUNCSAVE:<name>:<bytes>: the test save sdmc:/3ds/mw3ds/test_<name>.sav cut that much short (a damaged save)
		std::string p = "sdmc:/3ds/mw3ds/test_" + a[1] + ".sav";
		FILE* f = fopen(p.c_str(), "rb");
		if (!f)
			return fail(p + " missing");
		std::vector<char> data;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			data.insert(data.end(), buf, buf + n);
		fclose(f);
		size_t cut = (size_t)atoi(a[2].c_str());
		data.resize(data.size() > cut ? data.size() - cut : 0);
		f = fopen(p.c_str(), "wb");
		if (!f || fwrite(data.data(), 1, data.size(), f) != data.size())
			return fail("can't write " + p);
		fclose(f);
		return true;
	}
	if (verb == "SAYTOPIC" && a.size() >= 3)
	{
		// SAYTOPIC:<id>:<voice topic>: that actor says a line of Idle / Hello / Attack / Hit / Flee / Thief now
		int ri = findRef(s, a[1]);
		if (ri < 0 || w.refs[ri].actor < 0)
			return fail(a[1] + " not loaded");
		if (!s.npcSayTopic(ri, a[2].c_str()))
			return fail(a[1] + " has no " + a[2] + " line");
		return true;
	}
	if (verb == "SLAY" && a.size() >= 2)
	{
		// SLAY:<id>: that actor dies where it is (its death as from any blow, nobody's murder)
		int ri = findRef(s, a[1]);
		if (ri < 0 || w.refs[ri].actor < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].health = 0.0f;
		s.killNpc(ri, false);
		return true;
	}
	if (verb == "SLAYALL" && a.size() >= 2)
	{
		// SLAYALL:<id>: every loaded actor with that id dies where it stands, none loaded is no failure (the water's
		// creatures a walk through shallows meets on one profile and not another)
		std::string want = lower(spaced(a[1])), want2 = lower(a[1]);
		std::vector<int> found;
		w.forLoadedRefs([&](int i) {
			if (w.refs[i].actor >= 0 && !w.refs[i].dead && (w.refs[i].idLower == want || w.refs[i].idLower == want2))
				found.push_back(i);
		});
		for (int i : found)
		{
			w.refs[i].health = 0.0f;
			s.killNpc(i, false);
		}
		logf("test: %d %s slain", (int)found.size(), a[1].c_str());
		return true;
	}
	if (verb == "SETHEALTH" && a.size() >= 2)
	{
		w.stats.health = w.stats.healthMax * (float)atof(a[1].c_str());
		return true;
	}
	if (verb == "KNOW" && a.size() >= 2)
	{
		// KNOW:<topic>: the player knows the topic, as dialogue the test skipped would have taught it (TOPIC
		// asks only what the list offers: this says where a test leans on history it didn't play)
		std::string t = lower(spaced(a[1]));
		w.knownTopics.insert(t);
		if (s.dlg.open)
			dialogueRefreshTopics(s.dlg, w);      // a conversation already open lists it now
		logf("test: knows topic %s", t.c_str());
		return true;
	}
	if (verb == "SETBOUNTY" && a.size() >= 2)
	{
		w.bounty = atoi(a[1].c_str());
		return true;
	}
	// JOURNALADD:<quest>:<index>: the script's Journal command   SETJOURNALINDEX:<quest>:<index>: SetJournalIndex
	if ((verb == "JOURNALADD" || verb == "SETJOURNALINDEX") && a.size() >= 3)
	{
		int index = atoi(a[2].c_str());
		if (verb == "SETJOURNALINDEX")
			w.journalIndex[lower(a[1])] = index;
		else if (w.journalAdd(a[1], index))
			s.notify(w.game.gmst("sjournalentry", "Your journal has been updated."));
		return true;
	}
	// ADVANCE:<hours>: time passes at once, any size, with none of a rest's effects (the calendar moves with it)
	if (verb == "ADVANCE" && a.size() >= 2)
	{
		w.gameHour += (float)atof(a[1].c_str());
		w.syncTime();
		logf("test: advanced %s hours, %s", a[1].c_str(), s.dateText().c_str());
		return true;
	}
	// ENABLE:<ref> / DISABLE:<ref>: the script functions on a reference in the loaded cells
	// HIDEACTORS:<radius>: every person and creature that near the player disabled (side-by-side spell shots, tools/test/
	// sidebyside.py: nobody walks through the picture or catches the bolt)
	// NPCCAST:<npc>:<spell>[:<target npc>]: that person casts the spell as an NPC's cast goes (its school's cast sound and
	// cast visual on them; target effects as a bolt from their hands at the target, self / touch ones landing at once
	// with their hit sound and visual), turned to face the target. Sights and sounds only on another person (a touch
	// spell's effects aren't applied, and an NPC's bolt passes through NPCs): the game would count harm to them as the
	// player's crime. No cast animation: they may be held in a POSE (stress tests: many casters at once,
	// tools/test/cases/pfx-stress-o3ds.txt)
	if (verb == "NPCCAST" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		auto sit = w.game.spells.find(lower(spaced(a[2])));
		if (ri < 0 || !w.active(ri))
			return fail(a[1] + " not loaded");
		if (sit == w.game.spells.end())
			return fail("no spell " + a[2]);
		const SpellDef& sp = sit->second;
		int ti = a.size() >= 4 ? findRef(s, a[3]) : -1;
		Ref& r = w.refs[ri];
		if (ti >= 0)
		{
			r.rot[2] = atan2f(w.refs[ti].pos[0] - r.pos[0], w.refs[ti].pos[1] - r.pos[1]);
			w.syncActor(ri);
		}
		static const char* kSchool[6] = { "alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration" };
		s.playSound(ri, std::string(kSchool[s.spellSchool(sp)]) + " cast");
		s.castVfx(sp, ri);
		bool bolt = false;
		for (auto& e : sp.effects)
		{
			if (e.range == 2)
			{
				bolt = true;
				continue;
			}
			if (e.range == 0 || ti < 0)
				s.applyEffectToActor(ri, e, false, (float)sp.cost, 100.0f, sp.name);
		}
		for (int who : { ri, ti })
		{
			bool hit = false;
			for (auto& e : sp.effects)
				hit |= (e.range == 0 && who == ri) || (e.range == 1 && who == (ti >= 0 ? ti : ri));
			if (who >= 0 && hit)
			{
				s.playSound(who, s.spellSound(sp, 2));
				s.spellVfx(sp, 1, w.refs[who].pos, who, w.refs[who].rot[2]);
			}
		}
		if (bolt && ti >= 0)
		{
			const Ref& t = w.refs[ti];
			float from[3] = { r.pos[0] + sinf(r.rot[2]) * 40.0f, r.pos[1] + cosf(r.rot[2]) * 40.0f, r.pos[2] + 110.0f };
			float d[3] = { t.pos[0] - from[0], t.pos[1] - from[1], t.pos[2] + 80.0f - from[2] };
			float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			for (int k = 0; k < 3; k++)
				d[k] /= fmaxf(1.0f, len);
			s.fireProjectile(ri, from, d, 1200.0f, "", "", 1.0f, sp.id);
		}
		logf("test: %s casts %s%s%s", r.id.c_str(), sp.name.c_str(), ti >= 0 ? " at " : "", ti >= 0 ? w.refs[ti].id.c_str() : "");
		return true;
	}
	return std::nullopt;
}
