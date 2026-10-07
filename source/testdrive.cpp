// Test driver: see include/testdrive.h
#include <3ds.h>
#include <cmath>
#include <cstring>
#include "testdrive.h"
#include "session.h"
#include "log.h"
#include "audio.h"

static std::string spaced(std::string s)
{
	for (auto& c : s)
		if (c == '_')
			c = ' ';
	return s;
}

static std::vector<std::string> split(const std::string& s)
{
	std::vector<std::string> out;
	size_t p = 0;
	while (true)
	{
		size_t q = s.find(':', p);
		out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
		if (q == std::string::npos)
			return out;
		p = q + 1;
	}
}

// ---- mechanics tests: names, numbers, snapshots

#include <map>
#include "screens.h"
#include "collision.h"

static std::string squash(const std::string& s)
{
	std::string out;
	for (char c : s)
		if (c != ' ' && c != '_' && c != '-')
			out += (char)tolower((unsigned char)c);
	return out;
}

// A skill / attribute by number or by name ("longblade", "Long_Blade")
static int skillIndex(const World& w, const std::string& name)
{
	if (!name.empty() && isdigit((unsigned char)name[0]))
		return atoi(name.c_str());
	for (int k = 0; k < 27; k++)
		if (squash(skillName(w, k)) == squash(name))
			return k;
	return -1;
}

static int attrIndex(const World& w, const std::string& name)
{
	if (!name.empty() && isdigit((unsigned char)name[0]))
		return atoi(name.c_str());
	for (int k = 0; k < 8; k++)
		if (squash(attrName(w, k)) == squash(name))
			return k;
	return -1;
}

static int findRef(Session& s, const std::string& id)
{
	int ri = s.testFindRef(id);
	if (ri < 0)
		ri = s.testFindRef(spaced(id));
	if (ri < 0)
	{
		// an id with both underscores and spaces (ghost_npc_conoon chodal): a token can't hold the space
		const std::string want = lower(spaced(id));
		s.w.forLoadedRefs([&](int i) {
			if (ri < 0 && lower(spaced(s.w.refs[i].idLower)) == want)
				ri = i;
		});
	}
	// several share the id (a creature placed again after one died): a living one in the loaded cells
	if (ri >= 0 && ri != s.testPlaced && s.w.refs[ri].dead)
	{
		const std::string want = s.w.refs[ri].idLower;
		s.w.forLoadedRefs([&](int i) {
			if (s.w.refs[i].idLower == want && !s.w.refs[i].dead && s.w.refs[ri].dead)
				ri = i;
		});
	}
	return ri;
}

static int carried(const World& w, const std::string& id)
{
	std::string a = lower(id), b = lower(spaced(id));
	for (size_t k = 0; k < w.inventory.size(); k++)
		if (lower(w.inventory[k].id) == a || lower(w.inventory[k].id) == b)
			return (int)k;
	return -1;
}

// The last item the player enchanted that they still carry
static const InventoryItem* lastMade(const World& w)
{
	for (int k = (int)w.madeItems.size() - 1; k >= 0; k--)
	{
		int i = carried(w, w.madeItems[k]);
		if (i >= 0)
			return &w.inventory[i];
	}
	return nullptr;
}

// Numbers the checks compare. Kinds with an argument (a[1]) and those without; false: not a number kind
static bool hasArg(const std::string& what)
{
	return what == "levcandidates" || what == "levpick" || what == "levrolls" || what == "soulcapacity" || what == "stackcount"
		|| what == "refexists" || what == "refenabled" || what == "refstock" || what == "merchantgold" || what == "followset"
		|| what == "journalentries" || what == "questfinished" || what == "skill" || what == "skillprog" || what == "attr" || what == "attrups" || what == "effect"
		|| what == "refhealth" || what == "refrank" || what == "reflevel" || what == "refspell" || what == "reffatigue" || what == "refmagicka" || what == "refdisp" || what == "refflee"
		|| what == "refattr" || what == "refskill" || what == "refhealthmax" || what == "refmagickamax" || what == "refcommanded"
		|| what == "effectarg" || what == "locklevel" || what == "summoned" || what == "itemhealth" || what == "spellmagicka"
		|| what == "refcell" || what == "refforcesneak" || what == "refpkg" || what == "refpkgdone" || what == "refcombat" || what == "refrun" || what == "refdying" || what == "refknock" || what == "refsoultrap" || what == "refally" || what == "refdist" || what == "refabove" || what == "refwater" || what == "refaimable" || what == "refitem" || what == "reflock" || what == "reftrap" || what == "scripttarget" || what == "reflocal" || what == "reflocalfrac" || what == "scriptlocal" || what == "refloopvol" || what == "soundstarted" || what == "spellcost" || what == "castchance" || what == "brewedmag" || what == "brewedduration" || what == "charge"
		|| what == "count" || what == "repairamount" || what == "rechargegain" || what == "lockchance" || what == "trapchance"
		|| what == "persuadechance" || what == "persuadepart" || what == "enchantcastcost" || what == "resistbase"
		|| what == "crimebounty" || what == "skillneed" || what == "attackterm" || what == "knockodds" || what == "falldamage"
		|| what == "blockchance" || what == "elemshield" || what == "runspeedfor" || what == "jumpspeedfor" || what == "npcwalk"
		|| what == "npcrun" || what == "refeffect" || what == "fleerating" || what == "fightterm" || what == "hagglechance" || what == "barterprice" || what == "trainprice" || what == "travelprice" || what == "mapseen"
		|| what == "wcolor" || what == "regionchance" || what == "regionweather"
		|| what == "equipped" || what == "canequip" || what == "armorparts" || what == "pickpocketchance" || what == "tooluses";
}

// "<a>,<b>" as two numbers
static void twoNumbers(const std::string& arg, float& a, float& b)
{
	a = (float)atof(arg.c_str());
	size_t c = arg.find(',');
	b = c == std::string::npos ? 0.0f : (float)atof(arg.c_str() + c + 1);
}

static bool numberOf(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
	PlayerStats& st = w.stats;
	if (what == "skill" || what == "skillprog")
	{
		int k = skillIndex(w, arg);
		if (k < 0 || k >= 27)
			return false;
		v = what == "skill" ? (float)st.skills[k] : st.skillProgress[k];
		return true;
	}
	if (what == "attr" || what == "attrups")
	{
		int k = attrIndex(w, arg);
		if (k < 0 || k >= 8)
			return false;
		v = what == "attr" ? (float)st.attributes[k] : (float)st.attrSkillUps[k];
		return true;
	}
	if (what == "effect") { v = w.effectTotal(atoi(arg.c_str())); return true; }
	// Magic effect checks (spec/findings/magic-effects.md)
	if (what == "effectarg")
	{
		// effectarg:<effect>,<attribute or skill>: the total of that effect for one attribute or skill
		size_t c = arg.find(',');
		if (c == std::string::npos)
			return false;
		int e = atoi(arg.c_str());
		std::string key = arg.substr(c + 1);
		bool skillEffect = e == 21 || e == 26 || e == 78 || e == 83 || e == 89;
		int k = skillEffect ? skillIndex(w, key) : attrIndex(w, key);
		if (k < 0)
			return false;
		v = 0;
		for (auto& a : w.effects)
			if (a.effect == e && (skillEffect ? a.skill : a.attribute) == k)
				v += a.magnitude;
		return true;
	}
	if (what == "refattr" || what == "refskill")
	{
		// refattr:<actor>,<attribute> / refskill:<actor>,<skill>: the actor's own value with the drains, fortifies and
		// absorbs on it (not below 0; skills are people's only)
		size_t c = arg.find(',');
		int ri = c == std::string::npos ? -1 : findRef(s, arg.substr(0, c));
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		bool skill = what == "refskill";
		int k = skill ? skillIndex(w, arg.substr(c + 1)) : attrIndex(w, arg.substr(c + 1));
		if (k < 0 || k >= (skill ? 27 : 8))
			return false;
		const ActorDef& def = w.game.actors[w.refs[ri].actor];
		v = (float)(skill ? def.skills[k] : def.attributes[k]);
		for (auto& t : w.refs[ri].effects)
		{
			if (t.key != k)
				continue;
			bool sk = t.effect == 21 || t.effect == 83 || t.effect == 89;
			if (sk != skill)
				continue;
			bool fort = t.effect == 79 || t.effect == 83;
			bool drain = t.effect == 17 || t.effect == 21 || t.effect == 85 || t.effect == 89;
			v += fort ? t.magnitude : drain ? -t.magnitude : 0.0f;
		}
		v = fmaxf(0.0f, v);
		return true;
	}
	if (what == "spellmagicka") { v = s.testCastTaken; return true; }
	if (what == "capacity")
	{
		v = fmaxf(0.0f, w.game.gmstf("fencumbrancestrmult", 5.0f) * st.attributes[ATTR_STRENGTH] + w.effectTotal(8) - w.effectTotal(7));
		return true;
	}
	if (what == "hitchance") { v = s.playerDefense(false); return true; }              // Sanctuary, Chameleon, Invisibility
	if (what == "detectchance") { v = w.effectTotal(40) + (w.effectTotal(39) > 0.0f ? 100.0f : 0.0f); return true; }
	if (what == "levitating") { v = w.player.flying && w.effectTotal(10) > 0.0f ? 1.0f : 0.0f; return true; }
	if (what == "fallmult") { v = w.effectTotal(11) > 0.0f ? 0.25f : 1.0f; return true; }     // gravity while falling
	if (what == "swimspeed") { v = w.effectTotal(1); return true; }                           // Swift Swim's magnitude as read
	if (what == "jumpheight") { v = s.jumpSpeedFor(st.skills[20], w.effectTotal(9), 0.0f, false, 1.0f); return true; }
	if (what == "waterwalking") { v = w.effectTotal(2) > 0.0f ? 1.0f : 0.0f; return true; }
	if (what == "marked") { v = w.markCell >= 0 ? 1.0f : 0.0f; return true; }
	if (what == "corprus") { v = w.effectTotal(132) > 0.0f || w.pcHasCorprus() ? 1.0f : 0.0f; return true; }
	if (what == "activespells")          // distinct sources (spells, potions, items) with an effect running
	{
		std::vector<std::string> seen;
		for (auto& a : w.effects)
			if (std::find(seen.begin(), seen.end(), a.source) == seen.end())
				seen.push_back(a.source);
		v = (float)seen.size();
		return true;
	}
	if (what == "diseases")              // common diseases, blights and curses known
	{
		v = 0;
		for (auto& id : st.spells)
		{
			auto sp = w.game.spells.find(lower(id));
			v += sp != w.game.spells.end() && sp->second.type >= 2 && sp->second.type <= 4;
		}
		return true;
	}
	if (what == "summoned")              // live creatures called by that summon effect
	{
		v = 0;
		for (auto& a : w.effects)
			v += a.effect == atoi(arg.c_str()) && a.ref >= 0 && !w.refs[a.ref].dead;
		return true;
	}
	if (what == "itemhealth")            // the condition of a carried item
	{
		int i = carried(w, arg);
		if (i < 0)
			return false;
		v = (float)s.itemCondition(w.inventory[i]);
		return true;
	}
	if (what == "spellcost" || what == "castchance")
	{
		// a spell by id, or one MAKESPELL made (mw3ds_test_<name>)
		auto sp = w.game.spells.find(lower(arg));
		if (sp == w.game.spells.end())
			sp = w.game.spells.find("mw3ds_test_" + lower(arg));
		if (sp == w.game.spells.end())
			sp = w.game.spells.find(lower(spaced(arg)));
		if (sp == w.game.spells.end())
			return false;
		v = what == "spellcost" ? (float)sp->second.cost : (float)s.castChance(sp->second);
		return true;
	}
	if (what == "brewedmag" || what == "brewedduration")
	{
		// the last potion brewed: effect n's magnitude or duration
		const Object* o = w.game.object(s.lastBrewed);
		size_t n = (size_t)atoi(arg.c_str());
		if (!o || n >= o->effects.size())
			return false;
		v = what == "brewedmag" ? (float)o->effects[n].min : (float)o->effects[n].duration;
		return true;
	}
	// The formula pieces of formulas.cpp (spec/combat.md, spec/derived.md, spec/crime.md); "a,b": two arguments
	if (what == "repairchance") { v = s.repairChance(); return true; }
	if (what == "attackterm" || what == "knockodds" || what == "falldamage" || what == "blockchance" || what == "elemshield")
	{
		// comma-separated numbers (fatigue given as a fraction of a maximum of 100)
		float n[12] = {};
		int k = 0;
		for (size_t p = 0; k < 12 && p <= arg.size(); k++)
		{
			n[k] = (float)atof(arg.c_str() + p);
			size_t c = arg.find(',', p);
			if (c == std::string::npos) { k++; break; }
			p = c + 1;
		}
		const PlayerStats& st = w.stats;
		if (what == "attackterm")          // <skill value>: the player's attack term with it
			v = s.attackTermOf((int)n[0], st.attributes[ATTR_AGILITY], st.attributes[ATTR_LUCK], st.fatigue, st.fatigueMax,
				w.effectTotal(117), w.effectTotal(47));
		else if (what == "knockodds")      // <agility>
			v = s.knockdownOdds((int)n[0]);
		else if (what == "falldamage")     // <height>: with the player's Acrobatics and Jump
			v = s.fallDamage(n[0], st.skills[20], w.effectTotal(9));
		else if (what == "blockchance")    // block, agi, luck, fatigue, still, attack skill, agi, luck, fatigue, swing
			v = s.blockChance(n[0], n[1], n[2], n[3] * 100.0f, 100.0f, n[4] != 0.0f, n[5], n[6], n[7], n[8] * 100.0f, 100.0f, n[9]);
		else                               // magnitude, destruction, willpower, luck, fatigue, resistance, roll
			v = s.elementalShieldDamage(n[0], (int)n[1], (int)n[2], (int)n[3], n[4] * 100.0f, 100.0f, n[5], (int)n[6]);
		return true;
	}
	if (what == "playerdefense") { v = s.playerDefense(false); return true; }
	if (what == "runspeedfor" || what == "jumpspeedfor")
	{
		// "<speed>,<athletics>" / "<acrobatics>,<jump>,<load>,<running>,<fatigue term>"
		float n[5] = {};
		sscanf(arg.c_str(), "%f,%f,%f,%f,%f", &n[0], &n[1], &n[2], &n[3], &n[4]);
		v = what == "runspeedfor" ? s.runSpeedFor((int)n[0], (int)n[1]) : s.jumpSpeedFor((int)n[0], n[1], n[2], n[3] != 0.0f, n[4]);
		return true;
	}
	if (what == "hagglechance")
	{
		// "<npc>,<price>,<offer>,<1 selling>"
		size_t c = arg.find(',');
		int ri = c == std::string::npos ? -1 : findRef(s, arg.substr(0, c));
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		float n[3] = {};
		sscanf(arg.c_str() + c + 1, "%f,%f,%f", &n[0], &n[1], &n[2]);
		v = s.haggleChance(ri, (int)n[0], (int)n[1], n[2] != 0.0f);
		return true;
	}
	if (what == "fleerating" || what == "fightterm")
	{
		// "<id>,<distance>"
		size_t c = arg.find(',');
		int ri = findRef(s, c == std::string::npos ? arg : arg.substr(0, c));
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		float d = c == std::string::npos ? 0.0f : (float)atof(arg.c_str() + c + 1);
		v = what == "fleerating" ? s.fleeRatingOf(ri, d) : s.fightTermOf(ri, d);
		return true;
	}
	if (what == "refeffect")
	{
		// "<id>,<effect>": that effect's magnitude on the actor (abilities + spells on it)
		size_t c = arg.find(',');
		int ri = c == std::string::npos ? -1 : findRef(s, arg.substr(0, c));
		if (ri < 0)
			return false;
		v = w.actorEffect(ri, atoi(arg.c_str() + c + 1));
		return true;
	}
	if (what == "npcwalk" || what == "npcrun")
	{
		int ri = findRef(s, arg);
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		v = what == "npcwalk" ? s.actorWalkSpeed(ri) : s.actorRunSpeed(ri);
		return true;
	}
	if (what == "skillneed") { int k = skillIndex(w, arg); if (k < 0) return false; v = s.skillNeed(k); return true; }

	if (what == "rechargechance") { v = s.rechargeChance(); return true; }
	if (what == "vfxcount") { v = (float)s.vfx.size(); return true; }      // spell visuals showing now
	if (what == "untextured")           // first-person and body meshes drawn white for lack of a texture
	{
		v = 0;
		for (auto& p : s.vm.pieces)
			v += fpUntextured(p.second);
		for (auto& p : s.body.pieces)
			v += fpUntextured(p.second);
		return true;
	}
	if (what == "resistx") { v = s.resistX(); return true; }
	if (what == "resistbase") { v = s.resistBase(atoi(arg.c_str())); return true; }
	if (what == "enchantcastcost") { v = s.enchantCastCost((float)atof(arg.c_str())); return true; }
	if (what == "repairamount" || what == "rechargegain" || what == "lockchance" || what == "trapchance" || what == "crimebounty")
	{
		float a, b;
		twoNumbers(arg, a, b);
		if (what == "repairamount") v = (float)s.repairAmount(a, (int)b);          // quality, roll
		else if (what == "rechargegain") v = s.rechargeGain((int)a, (int)b, s.rechargeChance());   // soul, roll
		else if (what == "lockchance") v = s.lockChance((int)a, b);                 // lock level, quality
		else if (what == "trapchance") v = s.trapChance(a, b);                      // trap spell cost, quality
		else v = (float)s.crimeBounty((int)a, (int)b);                              // kind (0 theft .. 4 murder), value
		return true;
	}
	if (what == "barterprice" || what == "trainprice" || what == "travelprice")
	{
		// "<npc>,<value>,<1 buying / 0 selling>", "<npc>,<skill index>", "<npc>,<distance>,<followers>,<1 indoors>"
		size_t c = arg.find(',');
		int ri = findRef(s, c == std::string::npos ? arg : arg.substr(0, c));
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		float n[3] = {};
		if (c != std::string::npos)
			sscanf(arg.c_str() + c + 1, "%f,%f,%f", &n[0], &n[1], &n[2]);
		if (what == "barterprice") v = (float)s.barterPrice(ri, (int)n[0], n[1] != 0.0f);
		else if (what == "trainprice") v = (float)s.trainPriceFor(ri, (int)n[0]);
		else v = (float)s.travelPriceFor(ri, n[0], (int)n[1], n[2] != 0.0f);
		return true;
	}
	if (what == "persuadechance" || what == "persuadepart")
	{
		// "<npc id>,<kind>" (chance: 0 admire, 1 intimidate, 2 taunt, 3..5 bribe) or "<npc id>,<part>" (p1 p2 p3 n1 n2 n3 d raw)
		size_t c = arg.find(',');
		int ri = findRef(s, c == std::string::npos ? arg : arg.substr(0, c));
		int k = c == std::string::npos ? 0 : atoi(arg.c_str() + c + 1);
		if (ri < 0 || w.refs[ri].actor < 0 || k < 0 || k > 7)
			return false;
		float parts[8];
		float chance = s.persuadeChance(ri, what == "persuadechance" ? std::min(k, 5) : 0, parts);
		v = what == "persuadechance" ? chance : parts[k];
		return true;
	}
	if (what == "enchantpoints" || what == "enchantchance")
	{
		Session::EnchantCalc c = s.enchantCalc();
		v = what == "enchantpoints" ? c.points : c.chance;
		return true;
	}
	if (what == "mapseen")           // EXPECT:mapseen:<gx>,<gy>:eq:1: the map shows that exterior cell as explored
	{
		float gx, gy;
		twoNumbers(arg, gx, gy);
		v = w.mapCellSeen((int)gx, (int)gy) ? 1.0f : 0.0f;
		return true;
	}
	// World rules (spec/world-rules.md)
	if (what == "levcandidates" || what == "levpick" || what == "levrolls")
	{
		// levcandidates:<list>,<level>   levpick:<list>,<level>,<chance-none roll>,<index>   levrolls:<list>,<count>
		// (how many entries a roll can land on / the position of the index-th of them, -1: nothing / how many separate
		// rolls a top-level line of that count makes)
		size_t c = arg.find(',');
		if (c == std::string::npos)
			return false;
		std::string name = lower(arg.substr(0, c));
		auto lv = w.game.leveled.find(name);
		if (lv == w.game.leveled.end())
			lv = w.game.leveled.find(lower(spaced(name)));
		if (lv == w.game.leveled.end())
			return false;
		float n[3] = {};
		sscanf(arg.c_str() + c + 1, "%f,%f,%f", &n[0], &n[1], &n[2]);
		if (what == "levrolls")
		{
			v = (float)levRolls(lv->second.each, (int)n[0]);
			return true;
		}
		std::vector<int> cand;
		GameData::levCandidates(lv->second, (int)n[0], cand);
		if (what == "levcandidates")
			v = (float)cand.size();
		else if (lv->second.none > 0 && (int)n[1] < lv->second.none)
			v = -1.0f;
		else
			v = cand.empty() ? -1.0f : (float)cand[(size_t)(int)n[2] % cand.size()];
		return true;
	}
	if (what == "respawninterval") { v = 24.0f * 30.0f * w.game.gmstf("imonthstorespawn", 4.0f); return true; }
	if (what == "corpsedelay") { v = w.game.gmstf("fcorpsecleardelay", 72.0f); return true; }
	if (what == "goldresetdelay") { v = w.game.gmstf("fbartergoldresetdelay", 24.0f); return true; }
	if (what == "soulcapacity")          // a gem's value x fSoulgemMult: the largest soul it holds
	{
		const Object* o = w.game.object(arg);
		if (!o)
			o = w.game.object(spaced(arg));
		if (!o)
			return false;
		v = (float)o->value * w.game.gmstf("fsoulgemmult", 3.0f);
		return true;
	}
	if (what == "stackcount")            // inventory stacks of that item
	{
		v = 0;
		for (auto& it : w.inventory)
			v += it.id == lower(arg) || it.id == lower(spaced(arg));
		return true;
	}
	if (what == "refexists")             // the creature the test placed (even once cleared), else any placed one with that id
	{
		if (s.testPlaced >= 0)
			v = w.refs[s.testPlaced].cell >= 0 && !w.refs[s.testPlaced].pickedUp ? 1.0f : 0.0f;
		else
		{
			int ri = w.findRefAnywhere(spaced(arg));
			v = ri >= 0 && w.refs[ri].cell >= 0 && !w.refs[ri].pickedUp ? 1.0f : 0.0f;
		}
		return true;
	}
	if (what == "refenabled")
	{
		int ri = findRef(s, arg);
		if (ri < 0)
			return false;
		v = w.refs[ri].enabled ? 1.0f : 0.0f;
		return true;
	}
	if (what == "refstock" || what == "merchantgold")
	{
		// refstock:<npc>,<item>: their own count of it (the size of a restocking stack, whatever its sign)   merchantgold:<npc>
		size_t c = arg.find(',');
		int ri = findRef(s, c == std::string::npos ? arg : arg.substr(0, c));
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		if (what == "merchantgold")
			v = (float)w.refs[ri].gold;
		else
			v = c == std::string::npos ? 0.0f : (float)w.refItemCount(w.refs[ri], arg.substr(c + 1));
		return true;
	}
	if (what == "followset")
	{
		// followset:<distance>,<stayoutside 0|1>,<to exterior 0|1>: 1 when the rule takes a follower (outdoors now)
		float n[3] = {};
		sscanf(arg.c_str(), "%f,%f,%f", &n[0], &n[1], &n[2]);
		v = followerTaken(n[0] * n[0], n[1] != 0.0f, true, n[2] != 0.0f) ? 1.0f : 0.0f;
		return true;
	}
	if (what == "count") { v = (float)std::max(w.itemCount(arg), w.itemCount(spaced(arg))); return true; }
	if (what == "charge")
	{
		const InventoryItem* it = arg == "made" ? lastMade(w) : nullptr;
		int i = it ? -1 : carried(w, arg);
		if (!it && i >= 0)
			it = &w.inventory[i];
		if (!it)
			return false;
		v = w.chargeOf(*it);
		return true;
	}
	if (what == "reftrap")        // reftrap:<door or container id>: 1 while its trap is armed
	{
		int ri = findRef(s, arg);
		if (ri < 0)
			return false;
		v = !w.refs[ri].trap.empty() && !w.refs[ri].disarmed ? 1.0f : 0.0f;
		return true;
	}
	if (what == "reflock" || what == "locklevel")
	{
		// reflock:<door or container id>: its lock level (negative: unlocked, the level kept for Lock)
		// reflock:<cell>: the door leading to that cell (door ids repeat: ex_nord_door_01)
		int ri = -1;
		std::string dest = lower(spaced(arg));
		w.forLoadedRefs([&](int i) {
			if (ri < 0 && w.refs[i].type == "DOOR" && w.refs[i].hasDest && lower(w.refs[i].destCell) == dest)
				ri = i;
		});
		if (ri < 0)
			ri = findRef(s, arg);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(arg));
		if (ri < 0)
			return false;
		v = (float)w.refs[ri].lockLevel;
		return true;
	}
	if (what == "scripttarget")
	{
		// scripttarget:<global script>,<reference id>: 1 when that script is running on that reference (StartScript from it)
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string name = lower(arg.substr(0, comma)), id = lower(arg.substr(comma + 1));
		v = 0;
		for (auto& sc : w.scripts)
			if (sc.ref < 0 && sc.item.empty() && lower(sc.script->name) == name && sc.target >= 0
				&& w.refs[sc.target].idLower == id)
				v = 1;
		return true;
	}
	if (what == "reflocal" || what == "reflocalfrac" || what == "scriptlocal")
	{
		// reflocal:<reference id>,<variable>: a local of that object's script (of the first with the id that isn't 0:
		// several share one id); reflocalfrac: what follows its decimal point (0 for a whole number); scriptlocal:<global
		// script>,<variable>
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = lower(arg.substr(0, comma)), var = lower(arg.substr(comma + 1));
		bool found = false;
		v = 0;
		if (what == "scriptlocal")
		{
			for (auto& sc : w.scripts)
				if (!found && sc.ref < 0 && sc.item.empty() && lower(sc.script->name) == id)
					if (float* p = sc.local(var))
						v = *p, found = true;
			return found;
		}
		for (auto& r : w.refs)
			if ((r.idLower == id || r.idLower == lower(spaced(id))) && r.script >= 0)
				if (float* p = w.scripts[r.script].local(var))
				{
					found = true;
					v = *p;
					if (v != 0.0f)
						break;
				}
		if (what == "reflocalfrac")
			v -= truncf(v);
		return found;
	}
	if (what == "soundstarted")
	{
		// soundstarted:<sound id>: how many times that sound has been started (a count even with no audio device)
		auto rec = w.game.sounds.find(lower(spaced(arg)));
		// (or a voice line's file name, as a script says it: vo\w\m\hit_wm006.mp3)
		auto voice = w.game.voices.find(lower(arg));
		v = rec != w.game.sounds.end() ? audioStartedCount(rec->second.file)
			: voice != w.game.voices.end() ? audioStartedCount(voice->second) : -1;
		return true;
	}
	if (what == "movieplaying")
	{
		v = s.moviePlaying() ? 1.0f : 0.0f;
		return true;
	}
	if (what == "weapondrawn")
	{
		v = s.weaponDrawn ? 1.0f : 0.0f;
		return true;
	}
	// sounddenied: sounds that found every channel busy; loopsounds / loopheld: loops going / holding a channel
	if (what == "sounddenied")
	{
		v = audioDeniedCount();
		return true;
	}
	if (what == "loopsounds" || what == "loopheld")
	{
		v = 0;
		for (auto& l : s.loopSounds)
			if (what == "loopsounds" || l.channel >= 0)
				v++;
		return true;
	}
	if (what == "refloopvol")
	{
		// refloopvol:<reference id>,<sound id>: the volume (x100, before distance) of the sound it loops (PlayLoopSound3D[VP]); -1 none
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = lower(arg.substr(0, comma)), snd = lower(spaced(arg.substr(comma + 1)));
		auto rec = w.game.sounds.find(snd);
		v = -1;
		for (auto& l : s.loopSounds)
			if (l.id == snd && w.refs[l.ref].idLower == id)
				v = l.volume * (rec != w.game.sounds.end() ? rec->second.volume : 1.0f) * 100.0f;
		return true;
	}
	if (what == "refitem")
	{
		// refitem:<container or actor>,<item>: how many of the item it holds (AddItem / RemoveItem on it)
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = arg.substr(0, comma);
		int ri = s.testFindRef(id);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(id));
		if (ri < 0)
			return false;
		std::string item = lower(arg.substr(comma + 1)), item2 = spaced(item);
		v = 0;
		for (auto& it : w.refs[ri].contents)
			if (lower(it.second) == item || lower(it.second) == item2)
				v += stockCount(it.first);
		return true;
	}
	if (what == "refcell")
	{
		// refcell:<actor>,<cell>: 1 when that reference stands in a cell whose name begins with that (PositionCell)
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = arg.substr(0, comma);
		int ri = s.testFindRef(id);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(id));
		if (ri < 0)
			return false;
		std::string here = lower(w.cells[w.placeOf(ri)].name), want = lower(spaced(arg.substr(comma + 1)));
		v = here.compare(0, want.size(), want) == 0 ? 1.0f : 0.0f;
		return true;
	}
	if (what == "refspell")
	{
		// refspell:<actor>,<spell>: 1 when that NPC or creature has the spell (AddSpell / RemoveSpell on it)
		size_t comma = arg.find(',');
		int ri = comma == std::string::npos ? -1 : findRef(s, arg.substr(0, comma));
		if (ri < 0)
			return false;
		v = w.actorHasSpell(w.refs[ri], lower(arg.substr(comma + 1))) ? 1.0f : 0.0f;
		return true;
	}
	if (what.compare(0, 3, "ref") == 0)
	{
		int ri = findRef(s, arg);
		if (ri < 0)
			return false;
		const Ref& r = w.refs[ri];
		if (what == "refhealth") v = r.health;
		else if (what == "refrank") v = r.actor >= 0 ? (float)w.game.actors[r.actor].rank : -1.0f;   // RaiseRank / LowerRank
		else if (what == "reflevel") v = r.actor >= 0 ? (float)w.game.actors[r.actor].level : -1.0f;
		else if (what == "reffatigue") v = r.fatigue;
		else if (what == "refmagicka") v = r.magicka;
		else if (what == "refhealthmax") v = r.healthMax;
		else if (what == "refmagickamax") v = r.magickaMax;
		else if (what == "refcommanded") v = r.aiPackage == AIPKG_FOLLOW && r.aiTarget == "player" && !r.aiDone ? 1.0f : 0.0f;
		else if (what == "refdisp") v = (float)w.disposition(ri);
		else if (what == "refflee") v = r.fleeing ? 1.0f : 0.0f;
		else if (what == "refcombat") v = r.ai == AI_COMBAT ? 1.0f : 0.0f;
		else if (what == "refdying")     // dead, and its fall to the ground still playing
		{
			Actor* ra = w.actorOf(ri);
			v = r.dead && ra && ra->time < actorSkeleton(*w.actorsOf(ri), ra->skeleton).groups[ra->group].stop - 0.05f ? 1.0f : 0.0f;
		}
		else if (what == "refrun")       // running (its run animation plays), not walking
		{
			Actor* ra = w.actorOf(ri);
			v = ra && ra->group == actorFindGroup(actorSkeleton(*w.actorsOf(ri), ra->skeleton), "RunForward") ? 1.0f : 0.0f;
		}
		else if (what == "refpkg") v = r.aiPackage == AIPKG_IDLE ? -1.0f : (float)r.aiPackage;   // GetCurrentAIPackage
		else if (what == "refpkgdone") v = r.aiDone ? 1.0f : 0.0f;                              // GetAIPackageDone
		else if (what == "refknock") v = r.knockTimer;
		else if (what == "refsoultrap") v = r.soulTrapUntil > w.time ? 1.0f : 0.0f;
		else if (what == "refally") v = r.ally ? 1.0f : 0.0f;
		else if (what == "refdist") v = w.distanceToPlayer(ri);      // 100000: in another interior
		else if (what == "refabove")        // units over the floor under it (99999: no floor)
		{
			float floorZ = -1e9f;
			for (LoadedCell* l : w.loaded)
			{
				float z;
				if (collisionFloor(l->cell.collision, r.pos[0], r.pos[1], r.pos[2] + 40.0f, r.pos[2] - 6000.0f, &z) && z > floorZ)
					floorZ = z;
			}
			v = floorZ > -1e8f ? r.pos[2] - floorZ : 99999.0f;
		}
		else if (what == "refwater") v = w.here().hasWater() ? r.pos[2] - w.here().waterZ : 99999.0f;   // units over the water here
		else if (what == "refaimable") v = actorAimable(w, ri) ? 1.0f : 0.0f;   // the crosshair finds them where they stand
		else if (what == "refforcesneak")                                          // ForceSneak on that actor
		{
			auto f = w.moveFlags.find(ri);
			v = f != w.moveFlags.end() && (f->second & 8u) ? 1.0f : 0.0f;
		}
		else return false;
		return true;
	}
	if (what == "equipped")              // equipped:<item>: 1 when worn / held (any of that id), else 0
	{
		v = 0;
		for (auto& it : w.inventory)
			if (it.equipped && (lower(it.id) == lower(arg) || lower(it.id) == lower(spaced(arg))))
				v = 1;
		return true;
	}
	if (what == "canequip")              // canequip:<item>: 0 refused, 1 ok, 2 two-handed, 3 a shield with a two-hander (a refusal says why)
	{
		int i = carried(w, arg);
		if (i < 0)
			return false;
		std::string why;
		v = (float)s.canEquip(w.inventory[i], &why);
		if (!why.empty())
			s.notify(why);
		return true;
	}
	if (what == "armorparts")            // armorparts:<item>: 1 it has a head part (a full helm), 2 a foot part (boots, shoes)
	{
		const Object* o = w.game.object(arg);
		if (!o)
			o = w.game.object(spaced(arg));
		if (!o)
			return false;
		v = (o->type == "ARMO" || o->type == "CLOT") ? (float)o->flags : 0.0f;
		return true;
	}
	if (what == "pickpocketchance")      // pickpocketchance:<npc>,<value term>: percent the victim does not notice (the player's Sneak and the rest)
	{
		size_t comma = arg.find(',');
		int ri = findRef(s, comma == std::string::npos ? arg : arg.substr(0, comma));
		if (ri < 0 || w.refs[ri].actor < 0)
			return false;
		v = (float)(s.pickpocketBar(ri, comma == std::string::npos ? 0.0f : (float)atof(arg.c_str() + comma + 1)) + 1);
		return true;
	}
	if (what == "tooluses")              // tooluses:<lockpick or probe>: uses it has left
	{
		int i = carried(w, arg);
		const Object* o = i >= 0 ? w.game.object(w.inventory[i].id) : nullptr;
		if (!o)
			return false;
		v = (float)(w.inventory[i].condition < 0 ? o->uses : w.inventory[i].condition);
		return true;
	}
	if (what == "pickpocketing") { v = s.screen == SCR_CONTAINER && s.pickpocketing ? 1.0f : 0.0f; return true; }   // the open window is a pickpocket's
	if (what == "journalentries") { v = 0; for (auto& l : w.journal) v += l.quest == lower(arg); }   // lines written for a quest
	else if (what == "questfinished") v = w.questFinished.count(lower(arg)) ? 1.0f : 0.0f;
	else if (what == "weathernow") v = (float)w.weatherNow;
	else if (what == "wnext") v = w.weatherNext != w.weatherNow ? (float)w.weatherNext : -1.0f;     // -1: none changing
	else if (what == "wqueued") v = (float)w.weatherQueued;
	else if (what == "wtrans") v = w.weatherNext != w.weatherNow ? w.weatherBlend : 0.0f;
	else if (what == "wcolor")           // <sky|fog|ambient|sun>,<r|g|b>: the weather's colour for this hour
	{
		size_t c = arg.find(',');
		std::string kind = arg.substr(0, c);
		int k = kind == "sky" ? 0 : kind == "fog" ? 1 : kind == "ambient" ? 2 : kind == "sun" ? 3 : -1;
		int ch = c == std::string::npos || c + 1 >= arg.size() ? -1 : arg[c + 1] == 'r' ? 0 : arg[c + 1] == 'g' ? 1 : arg[c + 1] == 'b' ? 2 : -1;
		if (k < 0 || ch < 0)
			return false;
		v = w.weatherColour(k, ch);
	}
	else if (what == "wfogdepth") v = w.weatherFogDepth();
	else if (what == "wwind") v = w.windNow;
	else if (what == "wstorm") v = w.weatherStorm() ? 1.0f : 0.0f;
	else if (what == "wprecip") v = w.weatherPrecip() ? 1.0f : 0.0f;
	else if (what == "wtorches") v = w.useTorches() ? 1.0f : 0.0f;
	else if (what == "wsunon") v = w.sunOn() ? 1.0f : 0.0f;
	else if (what == "wskyon") v = w.current >= 0 && !w.cells[w.current].interior ? 1.0f : 0.0f;    // the sky is drawn outdoors only
	else if (what == "wsunvis") v = w.sunVisibility();
	else if (what == "wglarefade") v = w.glareFade();
	else if (what == "wnightday") v = (float)w.nightDay();
	else if (what == "wloop") v = s.weatherChannel >= 0 && audioPlaying(s.weatherChannel) ? 1.0f : 0.0f;   // the weather's loop sound
	else if (what == "regionchance")     // <region>,<weather>: the stored chance
	{
		size_t c = arg.rfind(',');
		v = (float)w.regionChance(lower(arg.substr(0, c)), c == std::string::npos ? 0 : atoi(arg.c_str() + c + 1));
	}
	else if (what == "regionweather")    // that region's weather now, -1 if none yet
	{
		auto f = w.forcedWeather.find(lower(arg));
		v = f == w.forcedWeather.end() ? -1.0f : (float)f->second;
	}
	else if (what == "level") v = (float)st.level;
	else if (what == "levelprog") v = (float)st.levelProgress;
	else if (what == "health") v = st.health;
	else if (what == "healthmax") v = st.healthMax;
	else if (what == "magicka") v = st.magicka;
	else if (what == "magickamax") v = st.magickaMax;
	else if (what == "fatigue") v = st.fatigue;
	else if (what == "fatiguemax") v = st.fatigueMax;
	else if (what == "gold") v = (float)w.itemCount("gold_001");
	else if (what == "saves")
	{
		// the player's own saves on the card (not the autosave or the starting points)
		v = 0;
		for (auto& sv : savesList(s.dataDir))
			if (!sv.autosave && !sv.bundled)
				v++;
	}
	else if (what == "bounty") v = (float)w.bounty;
	else if (what == "armor") v = s.playerArmor();              // the player's armor rating
	else if (what == "target") v = s.target >= 0 ? 1.0f : 0.0f;       // something under the crosshair
	else if (what == "forcesneak") v = w.forceSneak ? 1.0f : 0.0f;    // the player forced to sneak
	else if (what == "unaimable")
	{
		// loaded people (and bodies) within 2000 units the crosshair can't find where they stand
		v = 0;
		for (int i : w.loadedActors)
		{
			const Ref& r = w.refs[i];
			if (r.visible() && (r.type == "NPC_" || r.dead) && w.distanceToPlayer(i) < 2000.0f && !actorAimable(w, i))
			{
				logf("check: %s can't be aimed at (at %.0f %.0f %.0f)", r.id.c_str(), r.pos[0], r.pos[1], r.pos[2]);
				v++;
			}
		}
	}
	else if (what == "brewedvalue") { const Object* o = w.game.object(s.lastBrewed); if (!o) return false; v = (float)o->value; }
	else if (what == "made") { v = 0; for (auto& id : w.madeItems) v += carried(w, id) >= 0; }
	else if (what == "notesright") v = s.notesRight;      // the right edge of the notices on the top screen
	else if (what == "messagesidebar") v = s.messageSidebar;   // a message box's buttons are in the right column
	else if (what == "messagetop") v = s.messageTop;      // its first text line shown
	else if (what == "messageleft") v = s.messageLeft;    // its text lines below what is shown
	else if (what == "maplocal") v = s.mapLocal;          // the map screen shows the local map
	else if (what == "mapzoom") v = s.mapZoomIdx;         // its zoom step
	else if (what == "madespells") { v = 0; for (auto& id : w.madeSpells) for (auto& sp : st.spells) v += lower(sp) == lower(id); }
	else if (what == "effects") v = (float)w.effects.size();
	else if (what == "brewed") { v = 0; for (auto& id : w.brewed) v += std::max(0, w.itemCount(id)); }
	else if (what == "hour") v = fmodf(w.gameHour, 24.0f);     // the hour of the day (gameHour counts on past 24)
	else if (what == "flying") v = w.player.flying ? 1.0f : 0.0f;
	else if (what == "swings") v = (float)s.swingCount;       // blows the player let go
	else if (what == "casts") v = (float)s.castCount;         // spells the player cast
	else if (what == "vmaction") v = (float)s.vm.action;
	else if (what == "spells") v = (float)st.spells.size();
	else return false;
	return true;
}

static std::map<std::string, float> s_snap;

// "@": the value SNAP recorded for the same check; "@1" / "@-2.5": that plus / minus so much
static float wanted(const std::string& spec, const std::string& key, bool& ok)
{
	ok = true;
	if (spec.empty() || spec[0] != '@')
		return (float)atof(spec.c_str());
	auto it = s_snap.find(key);
	if (it == s_snap.end())
	{
		ok = false;
		return 0.0f;
	}
	return it->second + (spec.size() > 1 ? (float)atof(spec.c_str() + 1) : 0.0f);
}

static int screenByName(const std::string& n)
{
	static const struct { const char* name; int scr; } names[] = {
		{ "none", SCR_NONE }, { "dialogue", SCR_DIALOGUE }, { "book", SCR_BOOK }, { "container", SCR_CONTAINER },
		{ "inventory", SCR_INVENTORY }, { "rest", SCR_REST }, { "barter", SCR_BARTER }, { "levelup", SCR_LEVELUP },
		{ "training", SCR_TRAINING }, { "spellmake", SCR_SPELLMAKE }, { "enchant", SCR_ENCHANT },
		{ "alchemy", SCR_ALCHEMY }, { "recharge", SCR_RECHARGE }, { "persuade", SCR_PERSUADE }, { "spells", SCR_SPELLS },
		{ "race", SCR_RACE }, { "birth", SCR_BIRTH }, { "stats", SCR_STATS }, { "saves", SCR_SAVES },
		{ "journal", SCR_JOURNAL }, { "gamemenu", SCR_GAMEMENU }, { "map", SCR_MAP },
	};
	for (auto& e : names)
		if (n == e.name)
			return e.scr;
	return -1;
}

// Harness operations for the mechanics tests (at once, never blocking). True if the token was one of them
static bool mechanicsOp(Session& s, const std::vector<std::string>& a)
{
	World& w = s.w;
	const std::string& verb = a[0];
	auto fail = [&](const std::string& why) { logf("drive: FAIL %s: %s", verb.c_str(), why.c_str()); return true; };
	auto service = [&](int ri, unsigned flag) {
		return ri >= 0 && w.refs[ri].actor >= 0 && (w.game.actors[w.refs[ri].actor].services & flag) != 0;
	};
	if (verb == "SNAP" && a.size() >= 2)
	{
		float v;
		std::string arg = a.size() >= 3 ? a[2] : "";
		if (!numberOf(s, a[1], arg, v))
			return fail("no such number " + a[1] + " " + arg);
		s_snap[a[1] + ":" + arg] = v;
		logf("test: snap %s %s = %g", a[1].c_str(), arg.c_str(), v);
		return true;
	}
	if (verb == "CLASS" && a.size() >= 2)
	{
		w.stats.cls = lower(spaced(a[1]));
		if (!w.playerClass())
			return fail("no class " + a[1]);
		w.recomputeStats();
		logf("test: class %s", w.stats.cls.c_str());
		return true;
	}
	if ((verb == "SETSKILL" || verb == "SETATTR") && a.size() >= 3)
	{
		bool skill = verb == "SETSKILL";
		int k = skill ? skillIndex(w, a[1]) : attrIndex(w, a[1]);
		if (k < 0)
			return fail("no such " + a[1]);
		int want = atoi(a[2].c_str());
		for (int pass = 0; pass < 2; pass++)
		{
			if (skill)
				w.stats.skillBonus[k] += want - w.stats.skills[k];
			else
				w.stats.attrBonus[k] += want - w.stats.attributes[k];
			w.recomputeStats();
		}
		logf("test: %s %s = %d", skill ? "skill" : "attribute", a[1].c_str(), skill ? w.stats.skills[k] : w.stats.attributes[k]);
		return true;
	}
	if (verb == "SKILLPROG" && a.size() >= 3)
	{
		int k = skillIndex(w, a[1]);
		if (k < 0)
			return fail("no such skill " + a[1]);
		// absolute (the need itself is under test: skillNeed isn't used to set it)
		w.stats.skillProgress[k] = (float)atof(a[2].c_str());
		logf("test: skill %d progress %.2f of %.2f", k, w.stats.skillProgress[k], s.skillNeed(k));
		return true;
	}
	if (verb == "LEVELPROG" && a.size() >= 2)
	{
		w.stats.levelProgress = atoi(a[1].c_str());
		return true;
	}
	if (verb == "ENCHANTAT" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (!service(ri, 0x10000))
			return fail(a[1] + " doesn't enchant");
		s.openEnchanting(ri);
		return true;
	}
	if (verb == "ENCHITEM" && a.size() >= 2)
	{
		std::vector<int> items, gems;
		s.enchantChoices(items, gems);
		int i = carried(w, a[1]);
		if (i < 0 || std::find(items.begin(), items.end(), i) == items.end())
			return fail(a[1] + " can't be enchanted (or isn't carried)");
		s.enchantItem = i;
		return true;
	}
	if (verb == "ENCHGEM" && a.size() >= 2)
	{
		s.enchantGem = -1;
		for (size_t k = 0; k < w.inventory.size(); k++)
			if (lower(w.inventory[k].id) == lower(a[1]) && !w.inventory[k].soul.empty())
				s.enchantGem = (int)k;
		if (s.enchantGem < 0)
			return fail("no " + a[1] + " holding a soul");
		return true;
	}
	if (verb == "ENCHTYPE" && a.size() >= 2)
	{
		s.enchantType = atoi(a[1].c_str());
		return true;
	}
	if (verb == "ADDEFFECT" && a.size() >= 6)
	{
		// ADDEFFECT:effect:min:max:duration:range[:attribute or skill or "-"[:area]]
		SpellEffect e = {};
		e.effect = atoi(a[1].c_str());
		e.min = atoi(a[2].c_str());
		e.max = atoi(a[3].c_str());
		e.duration = atoi(a[4].c_str());
		e.range = atoi(a[5].c_str());
		e.attribute = e.skill = -1;
		if (a.size() >= 8)
			e.area = atoi(a[7].c_str());
		if (a.size() >= 7 && a[6] != "-")
		{
			bool skillEffect = e.effect == 21 || e.effect == 26 || e.effect == 78 || e.effect == 83 || e.effect == 89;
			if (skillEffect)
				e.skill = skillIndex(w, a[6]);
			else
				e.attribute = attrIndex(w, a[6]);
		}
		s.makeEffects.push_back(e);
		return true;
	}
	if (verb == "CONFIRM")
	{
		bool done = s.screen == SCR_ENCHANT ? s.enchantConfirm() : s.screen == SCR_SPELLMAKE ? s.spellmakeConfirm() : false;
		logf("test: confirm on screen %d: %s", (int)s.screen, done ? "done" : "refused");
		return true;
	}
	if (verb == "SPELLMAKE" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (!service(ri, 0x8000))
			return fail(a[1] + " doesn't make spells");
		s.makeEffects.clear();
		s.makeSel = -1;
		s.makeName.clear();
		s.barterRef = ri;                  // (the spellmaker's barter price)
		s.screen = SCR_SPELLMAKE;
		return true;
	}
	if (verb == "TRAIN" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		int k = skillIndex(w, a[2]);
		if (!service(ri, 0x4000))
			return fail(a[1] + " doesn't train");
		s.barterRef = ri;
		int best[3];
		s.trainerSkills(best);
		if (k < 0 || (best[0] != k && best[1] != k && best[2] != k))
			return fail(a[1] + " doesn't teach " + a[2]);
		s.trainSkill(k);
		s.barterRef = -1;
		return true;
	}
	if ((verb == "BUY" || verb == "SELL") && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !s.isMerchant(ri))
			return fail(a[1] + " doesn't trade");
		s.openBarter(ri);
		s.screen = SCR_NONE;
		Ref& m = w.refs[ri];
		int index = -1;
		const Object* o = nullptr;
		if (verb == "SELL")
		{
			index = carried(w, a[2]);
			o = index >= 0 ? w.game.object(w.inventory[index].id) : nullptr;
		}
		int from = -1;
		if (verb == "BUY")
			for (auto& g : s.merchantGoods(ri))
			{
				const std::string& id = w.refs[g.first].contents[g.second].second;
				if (lower(id) == lower(a[2]) || lower(id) == lower(spaced(a[2])))
				{
					from = g.first;
					index = g.second;
					o = w.game.object(id);
				}
			}
		if (index < 0 || !o)
			return fail(a[2] + (verb == "SELL" ? " not carried" : " not among their goods"));
		if (!s.merchantTrades(ri, o))
			return fail(a[1] + " doesn't trade in " + a[2]);
		int price = s.barterPrice(ri, o->value, verb == "BUY");
		bool done = s.barterTrade(verb == "SELL", index, price, from);
		logf("test: %s %s %s for %d: %s", verb.c_str(), a[1].c_str(), a[2].c_str(), price, done ? "done" : "refused");
		s.barterRef = -1;
		return true;
	}
	if (verb == "LEVELUP" && a.size() >= 2)
	{
		if (s.screen != SCR_LEVELUP)
			return fail("the level up screen isn't open");
		s.levelPicks.clear();
		for (size_t k = 1; k < a.size(); k++)
			s.levelPicks.push_back(attrIndex(w, a[k]));
		s.applyLevelUp();
		s.closeScreen();
		return true;
	}
	if (verb == "READ" && a.size() >= 2)
	{
		int i = carried(w, a[1]);
		if (i < 0)
			return fail(a[1] + " not carried");
		// the inventory's Read (screens_items.cpp): the book screen, which teaches its skill the first time
		const Object* o = w.game.object(w.inventory[i].id);
		if (!o || o->type != "BOOK")
			return fail(a[1] + " isn't a book");
		s.openScreen(SCR_BOOK);
		s.bookItem = w.inventory[i].id;
		logf("test: reading %s", a[1].c_str());
		return true;
	}
	if ((verb == "FACE" || verb == "CASTAT") && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !w.active(ri))
			return fail(a[1] + " not loaded");
		const Ref& r = w.refs[ri];
		float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
		float dz = r.pos[2] + 80.0f - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
		w.player.yaw = atan2f(dx, dy);
		w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
		if (verb == "CASTAT" && a.size() >= 3)
		{
			std::string sp = a[2];
			for (size_t k = 3; k < a.size(); k++)
				sp += ":" + a[k];
			// the id as given when there's such a spell (mw3ds_test_x), else with '_' as spaces ("fire_bite")
			w.stats.selectedSpell = sp.compare(0, 5, "item:") == 0 || w.game.spells.count(lower(sp)) ? lower(sp) : lower(spaced(sp));
			s.castSpell();
			logf("test: cast %s at %s", sp.c_str(), a[1].c_str());
		}
		return true;
	}
	if (verb == "MAKESPELL" && a.size() >= 2)
	{
		// MAKESPELL:name: the effects ADDEFFECT put together become a spell the player knows, selected
		SpellDef sp;
		sp.type = 0;
		sp.effects = s.makeEffects;
		sp.cost = std::max(1, (int)s.madeSpellCost(sp.effects));
		sp.id = "mw3ds_test_" + lower(a[1]);
		sp.name = a[1];
		w.game.spells[sp.id] = sp;
		if (std::find(w.stats.spells.begin(), w.stats.spells.end(), sp.id) == w.stats.spells.end())
			w.stats.spells.push_back(sp.id);
		w.stats.selectedSpell = sp.id;
		s.makeEffects.clear();
		logf("test: spell %s, %zu effects, cost %d", sp.id.c_str(), sp.effects.size(), sp.cost);
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
	// SETHEALTH:<fraction>: the player's Health, a fraction of the maximum
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
	if ((verb == "ENABLE" || verb == "DISABLE") && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.setEnabled(ri, verb == "ENABLE");
		return true;
	}
	// PCRACE:<race id>: the player's race (Same Race, %PCRace)   PCSEX:<0|1>: male / female (Same Sex, PC Gender)
	// PCNAME:<name>: the player's name (%PCName)   (underscores for spaces)
	if (verb == "PCRACE" && a.size() >= 2)
	{
		w.stats.race = lower(spaced(a[1]));
		return true;
	}
	// SETRACE:<race id>: as PCRACE (a Khajiit tries the helmet)
	if (verb == "SETRACE" && a.size() >= 2)
	{
		w.stats.race = lower(spaced(a[1]));
		return true;
	}
	// SNEAK:<0|1>: the player sneaks (or not)   WEREWOLF:<0|1>: the player is a werewolf (what it refuses)
	if (verb == "SNEAK" && a.size() >= 2)
	{
		s.testSneak = atoi(a[1].c_str()) != 0;
		return true;
	}
	if (verb == "WEREWOLF" && a.size() >= 2)
	{
		s.werewolf = atoi(a[1].c_str()) != 0;
		return true;
	}
	// KNOCKDOWN:<npc>: they lie knocked down for a long while (their pockets open to anyone)
	if (verb == "KNOCKDOWN" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].knockTimer = 3600.0f;
		return true;
	}
	// SETITEMHEALTH:<id>:<n>: the carried piece's condition (0: broken)
	if (verb == "SETITEMHEALTH" && a.size() >= 3)
	{
		int i = carried(w, a[1]);
		if (i < 0)
			return fail(a[1] + " not carried");
		w.inventory[i].condition = atoi(a[2].c_str());
		return true;
	}
	// SETITEMCHARGE:<id>:<n>: the carried enchanted item's charge left
	if (verb == "SETITEMCHARGE" && a.size() >= 3)
	{
		int i = carried(w, a[1]);
		if (i < 0)
			return fail(a[1] + " not carried");
		w.inventory[i].charge = (float)atof(a[2].c_str());
		return true;
	}
	// USELOCKPICK:<door or container>:<tool> / USEPROBE: the tool is held up to that object (it faces it), one try;
	// ROLL holds the die
	if ((verb == "USELOCKPICK" || verb == "USEPROBE") && a.size() >= 3)
	{
		int ri = findRef(s, a[1]), ti = carried(w, a[2]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		if (ti < 0)
			return fail(a[2] + " not carried");
		if (!w.inventory[ti].equipped)
			s.equipItem(ti, true);
		int was = s.target;
		s.target = ri;
		s.useTool(w.inventory[ti]);
		s.target = was;
		return true;
	}
	if (verb == "PCSEX" && a.size() >= 2)
	{
		w.stats.female = atoi(a[1].c_str()) != 0;
		return true;
	}
	if (verb == "PCNAME" && a.size() >= 2)
	{
		w.stats.name = spaced(a[1]);
		return true;
	}
	// TYPE:<text>: what the system keyboard returns when a name is asked for (the made spell's / item's); % for spaces
	if (verb == "TYPE" && a.size() >= 2)
	{
		s.typedText = spaced(a[1]);
		return true;
	}
	// NOTIFY:<text>: a notice on the top screen, as a script's or the game's (underscores for spaces)
	if (verb == "NOTIFY" && a.size() >= 2)
	{
		s.notify(spaced(a[1]));
		return true;
	}
	// MSGBOX:<text>:<button>...: a message box with those buttons, as a script's MessageBox
	if (verb == "MSGBOX" && a.size() >= 3)
	{
		MessageState m;
		m.text = spaced(a[1]);
		for (size_t k = 2; k < a.size(); k++)
			m.buttons.push_back(spaced(a[k]));
		m.fromScript = true;
		s.messages.push_back(m);
		return true;
	}
	// TOPICLOG:<topic>:<line>: the journal's Topics index has that line about the topic (underscores for spaces)
	if (verb == "TOPICLOG" && a.size() >= 3)
	{
		w.topicLog[lower(spaced(a[1]))].push_back(spaced(a[2]));
		return true;
	}
	// SETITEM:<id>:<n>: the player holds exactly n of the item (Item conditions)
	if (verb == "SETITEM" && a.size() >= 3)
	{
		std::string id = w.game.object(a[1]) ? a[1] : spaced(a[1]);
		int have = w.itemCount(id), want = atoi(a[2].c_str());
		if (want > have)
			w.addItem(id, want - have);
		else if (want < have)
			w.removeItem(id, have - want);
		return true;
	}
	// SETDEAD:<id>:<n>: how many of that id have died (Dead conditions)
	if (verb == "SETDEAD" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		w.deadCounts[ri >= 0 ? w.refs[ri].idLower : lower(a[1])] = atoi(a[2].c_str());
		return true;
	}
	// SETTALKED:<npc>:<0|1>: the NPC's real talked-to-player flag (a talk in progress keeps the one it began with)
	if (verb == "SETTALKED" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].talkedToPC = atoi(a[2].c_str()) != 0;
		return true;
	}
	// CLOTHVALUE:<n>: the Clothing Modifier reads n whatever is worn (-1: what is worn again)
	if (verb == "CLOTHVALUE" && a.size() >= 2)
	{
		g_dialogueClothValue = atoi(a[1].c_str());
		return true;
	}
	if (verb == "MOVIE" && a.size() >= 2)
	{
		// MOVIE:<name>: plays that movie (the intro movie is skipped in tests); EXPECT movieplaying says if it still runs
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
	// SETWEATHER:<id>: this region's weather at once. CHANGEWEATHER:<region>:<id> and MODREGION:<region>:<c0,c1,..>
	// are the script functions ('%' in the region stands for a space; a short list leaves the rest 0)
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
	// BARTER:<ref>: the merchant's Barter screen opens, as the dialogue's Barter button does
	if (verb == "BARTER" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !s.isMerchant(ri))
			return fail(a[1] + " doesn't trade");
		s.openBarter(ri);
		return true;
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
				const std::string& id = w.refs[g.first].contents[g.second].second;
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

// ge / gt / eq / ne / lt / le (words: '+' and '>' can't go in a harness token)
static bool compare(float a, const std::string& op, float b)
{
	if (op == "ge") return a >= b;
	if (op == "gt") return a > b;
	if (op == "ne") return a != b;
	if (op == "lt") return a < b;
	if (op == "le") return a <= b;
	return a == b;
}

void TestDriver::fail(const char* why)
{
	logf("drive: FAIL %s %s: %s", kind == KILL ? "kill" : kind == WALK ? "walk to" : "activate", id.c_str(), why);
	kind = NONE;
}

bool TestDriver::start(Session& s, const std::string& token)
{
	std::vector<std::string> a = split(token);
	const std::string& verb = a[0];
	if (verb == "GOD")
	{
		s.testGod = a.size() < 2 || a[1] != "0";
		logf("test: god mode %s", s.testGod ? "on" : "off");
		return false;
	}
	if (verb == "EXPECT")
	{
		expect(s, token.substr(7));
		return false;
	}
	if (mechanicsOp(s, a))
		return false;
	if (verb == "EQUIP" && a.size() >= 2)
	{
		std::string want = lower(spaced(a[1])), want2 = lower(a[1]);
		for (size_t k = 0; k < s.w.inventory.size(); k++)
			if (lower(s.w.inventory[k].id) == want || lower(s.w.inventory[k].id) == want2)
			{
				const Object* o = s.w.game.object(s.w.inventory[k].id);
				if (o && (o->type == "ALCH" || o->type == "INGR"))
				{
					// the inventory's Drink / Eat
					std::string item = s.w.inventory[k].id;
					s.consume(item);
					logf("test: drank / ate %s", item.c_str());
					return false;
				}
				if (!s.w.inventory[k].equipped && !s.equipItem((int)k))
				{
					logf("test: %s was refused", s.w.inventory[k].id.c_str());       // (EXPECT:message says why)
					return false;
				}
				logf("test: equipped %s", s.w.inventory[k].id.c_str());
				return false;
			}
		logf("drive: FAIL equip %s: not carried", a[1].c_str());
		return false;
	}
	if ((verb == "WALKTO" || verb == "KILL" || verb == "ACTIVATE" || verb == "PICKUP" || verb == "DOORTO" || verb == "LOOT"
		|| verb == "PUT" || verb == "STRIKE") && a.size() >= 2)
	{
		// PUT:<container>:<item>: open it as LOOT does, then put the carried item in (the Inventory page's tap)
		strikeFor = verb == "STRIKE" ? (a.size() >= 3 ? (float)atof(a[2].c_str()) : 5.0f) : 0.0f;
		lootItem = (verb == "LOOT" || verb == "PUT") && a.size() >= 3 ? lower(a[2]) : "";
		lootPut = verb == "PUT";
		id = a[1];
		kind = verb == "WALKTO" ? WALK : (verb == "KILL" || verb == "STRIKE") ? KILL : ACTIVATE;
		pickup = verb == "PICKUP";
		ref = findTarget(s, verb, a[1]);
		if (ref < 0)
		{
			// maybe not streamed in yet (a neighbouring exterior, just through a door): look again a while
			searchVerb = verb;
			searchArg = a[1];
			searchTimer = 0.0f;
			kind = SEARCH;
			logf("drive: %s %s not in the loaded cells yet, searching", verb.c_str(), a[1].c_str());
			return true;
		}
		path.clear();
		const float* t = s.w.refs[ref].pos;
		if (s.w.findPath(s.w.player.feet, t, path))
			logf("drive: %s %s, %d path points", verb.c_str(), id.c_str(), (int)path.size());
		else
			logf("drive: %s %s, straight", verb.c_str(), id.c_str());
		memcpy(progressAt, s.w.player.feet, sizeof(progressAt));
		progressTimer = swingTimer = lookTimer = elapsed = repathTimer = 0.0f;
		stuckTries = 0;
		lastGoal = 1e9f;
		sidestepTimer = 0.0f;
		doorTried = false;
		pressA = false;
		return true;
	}
	return false;
}

// The reference an action is about: an id, or for DOORTO the nearest door going there (-1: not loaded)
int TestDriver::findTarget(Session& s, const std::string& verb, const std::string& arg)
{
	if (verb != "DOORTO")
	{
		int r = s.testFindRef(arg);
		return r >= 0 && s.w.active(r) ? r : -1;
	}
	std::string want = lower(spaced(arg));
	bool outside = want == "outside";
	float best = 1e30f;
	int found = -1;
	for (size_t i = 0; i < s.w.refs.size(); i++)
	{
		const Ref& r = s.w.refs[i];
		if (!r.hasDest || !s.w.active((int)i) || !r.visible())
			continue;
		if (r.idLower == "prisonmarker")      // the jail's drop-off mark, no door anyone can use
			continue;
		bool match = outside ? r.destHasGrid || r.destCell.empty() : lower(r.destCell).compare(0, want.size(), want) == 0;
		if (!match)
			continue;
		float dx = r.pos[0] - s.w.player.feet[0], dy = r.pos[1] - s.w.player.feet[1];
		float d = dx * dx + dy * dy;
		if (d < best)
		{
			best = d;
			found = (int)i;
		}
	}
	if (found >= 0)
		id = s.w.refs[found].id + " -> " + (s.w.refs[found].destCell.empty() ? "outside" : s.w.refs[found].destCell);
	return found;
}

bool TestDriver::update(Session& s, PlayerInput& in, u32& down, float dt)
{
	if (kind == NONE)
		return false;
	elapsed += dt;
	World& w = s.w;
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
			logf("drive: FAIL loot %s: it didn't open", id.c_str());
			kind = NONE;
		}
		return kind != NONE;
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
	if (ref < 0 || !w.active(ref))
	{
		fail("gone from the loaded cells");
		return false;
	}
	Ref& r = w.refs[ref];
	Player& p = w.player;
	// they came to us: a ForceGreeting (Dagoth Gares) opened their dialogue on the way
	if ((kind == ACTIVATE || kind == WALK) && s.dlg.open && s.dlg.ref == ref)
	{
		logf("drive: %s greeted us on the way", id.c_str());
		kind = NONE;
		return false;
	}
	// killing: a conversation that opened on its own (ForceGreeting: Dagoth Gares's speech) is left, as a
	// player says goodbye and fights on
	if (kind == KILL && s.dlg.open && s.screen == SCR_DIALOGUE)
	{
		logf("drive: leaving a conversation to fight %s", id.c_str());
		s.closeScreen();
		return true;
	}
	// a message box in the way (a crime noticed, a script's message): click it away, as a player would
	if (!s.messages.empty())
	{
		if (!pressA)
		{
			logf("drive: dismissing \"%.60s\"", s.messages.front().text.c_str());
			down |= KEY_A;
		}
		pressA = !pressA;                // A on one frame, released the next
		return true;
	}
	if (kind == KILL && r.dead)
	{
		logf("drive: killed %s in %.1f s", id.c_str(), elapsed);
		kind = NONE;
		return false;
	}
	float tx = r.pos[0], ty = r.pos[1];
	float dx = tx - p.feet[0], dy = ty - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	float want = kind == WALK ? 120.0f : kind == KILL ? 90.0f : 100.0f;
	// on another floor (ruins, towers): not there yet, however close it looks from above. Things to
	// activate count as reached within Morrowind's activation reach (iMaxActivateDist 192) in 3D.
	float dz = r.pos[2] - p.feet[2];
	bool otherFloor = kind == ACTIVATE ? dist * dist + dz * dz > 190.0f * 190.0f && fabsf(dz) > 160.0f : fabsf(dz) > 160.0f;
	if (dist > want || otherFloor)
	{
		// no route yet (the cells ahead were still loading): try again now and then
		repathTimer += dt;
		if (path.empty() && repathTimer > 3.0f)
		{
			repathTimer = 0.0f;
			if (w.findPath(p.feet, r.pos, path))
				logf("drive: route to %s found, %d path points", id.c_str(), (int)path.size());
		}
		// the next path point on the way (passed ones dropped), else straight at it
		float gx = tx, gy = ty;
		while (!path.empty())
		{
			const PathPoint& pp = w.pathPoints[path.front()];
			float px = pp.pos[0] - p.feet[0], py = pp.pos[1] - p.feet[1];
			if (px * px + py * py > 60.0f * 60.0f)
			{
				gx = pp.pos[0];
				gy = pp.pos[1];
				break;
			}
			path.erase(path.begin());
		}
		p.yaw = atan2f(gx - p.feet[0], gy - p.feet[1]);
		p.pitch = 0.0f;
		in.moveY = 1.0f;
		if (sidestepTimer > 0.0f)
		{
			sidestepTimer -= dt;
			in.moveX = sidestep;
			in.moveY = 0.3f;
		}
		// no headway for 2 s: jump once, then warp beside it (logged: the run still fails on it)
		// (off the route: no closer to it either, e.g. shuttling between the last two grid points)
		progressTimer += dt;
		if (progressTimer >= 2.0f)
		{
			float mx = p.feet[0] - progressAt[0], my = p.feet[1] - progressAt[1];
			float goal = sqrtf(dist * dist + dz * dz);
			bool noCloser = path.empty() && goal > lastGoal - 40.0f;
			lastGoal = goal;
			if (mx * mx + my * my < 40.0f * 40.0f || noCloser)
			{
				// a door in the way (inner doors swing shut): open it, as a player would
				int door = -1;
				float bd = 250.0f * 250.0f;
				for (int i : w.loadedDoors)
				{
					const Ref& d = w.refs[i];
					if (d.type != "DOOR" || d.hasDest || !d.visible())
						continue;
					float ex = d.pos[0] - p.feet[0], ey = d.pos[1] - p.feet[1];
					if (ex * ex + ey * ey < bd && fabsf(d.pos[2] - p.feet[2]) < 200.0f)
					{
						bd = ex * ex + ey * ey;
						door = i;
					}
				}
				if (door >= 0 && !doorTried)
				{
					logf("drive: opening %s on the way to %s", w.refs[door].id.c_str(), id.c_str());
					s.playerActivate(door);
					doorTried = true;          // then jump, then warp
				}
				else if (++stuckTries <= 2)
				{
					// step aside (left, then right) for a moment, as a player would around a post or a person
					sidestep = stuckTries == 1 ? -1.0f : 1.0f;
					sidestepTimer = 1.0f;
				}
				else if (stuckTries == 3)
					in.jump = true;
				else
				{
					logf("drive: stuck at %.0f %.0f %.0f going to %s (%.0f away, %.0f up), warped", p.feet[0], p.feet[1], p.feet[2],
						id.c_str(), dist, dz);
					float k = dist > 1.0f ? (want - 20.0f) / dist : 0.0f;
					float ux = dist > 1.0f ? dx / dist : 1.0f, uy = dist > 1.0f ? dy / dist : 0.0f;
					p.feet[0] = tx - dx * k;
					p.feet[1] = ty - dy * k;
					p.feet[2] = r.pos[2] + 20.0f;
					// land where it can be seen (not behind a wall or a gate): turn the approach round until it can
					float eye[3] = { tx, ty, r.pos[2] + 100.0f };
					for (int a = 0; a < 8; a++)
					{
						float ang = a * 0.7854f, c = cosf(ang), sn = sinf(ang);
						float ox = tx - (ux * c - uy * sn) * (want - 20.0f), oy = ty - (ux * sn + uy * c) * (want - 20.0f);
						float from[3] = { ox, oy, r.pos[2] + 120.0f };
						if (w.lineOfSight(from, eye))
						{
							p.feet[0] = ox;
							p.feet[1] = oy;
							break;
						}
					}
					p.vz = 0.0f;
					p.fallTop = p.feet[2];
					p.landedFall = 0.0f;
					path.clear();
					stuckTries = 0;
				}
			}
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		return true;
	}
	// there: face it
	p.yaw = atan2f(dx, dy);
	if (kind == WALK)
	{
		logf("drive: reached %s in %.1f s", id.c_str(), elapsed);
		kind = NONE;
		return false;
	}
	if (kind == KILL)
	{
		p.pitch = 0.0f;
		if (strikeFor > 0.0f && swingTimer >= strikeFor)
		{
			logf("drive: struck %s for %.1f s", id.c_str(), swingTimer);
			kind = NONE;
			return false;
		}
		// wind up (attack held), then let go: the strike
		swingTimer += dt;
		in.attack = fmodf(swingTimer, 0.7f) < 0.4f;
		return true;
	}
	// ACTIVATE: look at its middle, then press A on what the crosshair finds
	float cz = (r.boxMin[2] + r.boxMax[2]) * 0.5f;
	float eyeZ = p.feet[2] + PLAYER_EYE_HEIGHT;
	p.pitch = atan2f(cz - eyeZ, fmaxf(dist, 1.0f));
	lookTimer += dt;
	if (s.target == ref && !s.menuOpen())
	{
		down |= KEY_A;
		logf("drive: activated %s (crosshair)", id.c_str());
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : NONE;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	if (lookTimer > 1.0f)
	{
		// (a player couldn't have: counted apart in the verdict, as warps are)
		logf("drive: crosshair miss: on %s, not %s: activating it directly", s.target >= 0 ? w.refs[s.target].id.c_str() : "nothing",
			id.c_str());
		s.playerActivate(ref);
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : NONE;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	return true;
}

// EXPECT:journal:<quest>:<op>:<n>   EXPECT:dead:<id>   EXPECT:alive:<id>   EXPECT:item:<id>:<op>:<n>
// EXPECT:global:<name>:<op>:<v>     EXPECT:rank:<faction>:<op>:<n>   EXPECT:cell:<name start>   EXPECT:talking
// EXPECT:spell:<id> (the player has it: spells, abilities, diseases)
// (op: ge gt eq ne lt le; underscores for spaces)
bool TestDriver::expect(Session& s, const std::string& spec)
{
	std::vector<std::string> a = split(spec);
	World& w = s.w;
	bool ok = false;
	char got[96] = "";
	const std::string& what = a[0];
	if (what == "journal" && a.size() >= 4)
	{
		int v = w.getJournal(a[1]);
		ok = compare((float)v, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%d", v);
	}
	else if ((what == "dead" || what == "alive") && a.size() >= 2)
	{
		int ri = s.testFindRef(a[1]);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(a[1]));
		bool dead = ri >= 0 && w.refs[ri].dead;
		if (what == "dead" && ri >= 0 && !dead)
		{
			// testFindRef prefers a living one; another with the id may be the one that was killed
			const std::string want = w.refs[ri].idLower;
			w.forLoadedRefs([&](int i) {
				if (w.active(i) && w.refs[i].idLower == want && w.refs[i].dead)
					dead = true;
			});
		}
		ok = ri >= 0 && dead == (what == "dead");
		snprintf(got, sizeof(got), "%s", ri < 0 ? "no such ref" : dead ? "dead" : "alive");
	}
	else if (what == "item" && a.size() >= 4)
	{
		int n = std::max(w.itemCount(a[1]), w.itemCount(spaced(a[1])));
		ok = compare((float)n, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%d", n);
	}
	else if (what == "global" && a.size() >= 4)
	{
		auto g = w.globals.find(lower(a[1]));
		float v = g == w.globals.end() ? 0.0f : g->second;
		ok = compare(v, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%g", v);
	}
	else if (what == "rank" && a.size() >= 4)
	{
		int r = w.pcRankIn(lower(spaced(a[1])));
		ok = compare((float)r, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%d", r);
	}
	else if ((what == "cell" || what == "notcell") && a.size() >= 2)
	{
		std::string here = lower(w.cellName()), want = lower(spaced(a[1]));
		ok = (here.compare(0, want.size(), want) == 0) == (what == "cell");
		snprintf(got, sizeof(got), "%.80s", w.cellName().c_str());
	}
	else if (what == "spell" && a.size() >= 2)
	{
		std::string want = lower(spaced(a[1]));
		bool has = false;
		for (auto& sp : w.stats.spells)
			has |= lower(sp) == want || lower(sp) == lower(a[1]);
		ok = has;
		snprintf(got, sizeof(got), "%s", has ? "known" : "not known");
	}
	// EXPECT:selspell:<id>: that spell is the one ready to cast
	else if (what == "selspell" && a.size() >= 2)
	{
		ok = w.stats.selectedSpell == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", w.stats.selectedSpell.c_str());
	}
	else if (what == "talking")
	{
		ok = s.dlg.open;
		snprintf(got, sizeof(got), "%s", ok ? "talking" : "not talking");
	}
	else if (what == "soul" && a.size() >= 3)
	{
		// EXPECT:soul:<gem>:<creature>: carries that gem holding that creature's soul ("none": an empty one)
		std::string want = a[2] == "none" ? "" : lower(spaced(a[2]));
		ok = false;
		std::string souls;
		for (auto& it : w.inventory)
			if (lower(it.id) == lower(a[1]))
			{
				ok |= lower(it.soul) == want || lower(it.soul) == lower(a[2]);
				souls += (souls.empty() ? "" : ",") + (it.soul.empty() ? std::string("none") : it.soul);
			}
		snprintf(got, sizeof(got), "%.80s", souls.empty() ? "no such gem" : souls.c_str());
	}
	else if (what == "madeeffect" && a.size() >= 2)
	{
		// EXPECT:madeeffect:<effect>[:<cast type>]: the last item made carries an enchantment with it
		const InventoryItem* it = lastMade(w);
		const SpellDef* en = it ? w.enchantmentOf(*it) : nullptr;
		ok = false;
		if (en)
			for (auto& e : en->effects)
				ok |= e.effect == atoi(a[1].c_str());
		if (en && a.size() >= 3)
			ok &= en->type == ENCH_ONCE + atoi(a[2].c_str());
		snprintf(got, sizeof(got), "%s", !it ? "no made item carried" : !en ? "no enchantment"
			: (std::to_string(en->effects.size()) + " effects, type " + std::to_string(en->type - ENCH_ONCE)).c_str());
	}
	else if (what == "maptip" && a.size() >= 2)
	{
		// EXPECT:maptip:<cell>: the local map shows where the tapped door leads ("none": no door tapped)
		std::string tip = s.mapDoorTip >= 0 && s.mapDoorTip < (int)w.refs.size() ? w.refs[s.mapDoorTip].destCell : "";
		ok = a[1] == "none" ? tip.empty() : lower(tip) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", tip.empty() ? "none" : tip.c_str());
	}
	else if (what == "journaltopic" && a.size() >= 2)
	{
		// EXPECT:journaltopic:<topic>: the topic open in the journal's Topics index ("none": the index itself)
		ok = a[1] == "none" ? s.journalTopic.empty() : lower(s.journalTopic) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", s.journalTopic.empty() ? "none" : s.journalTopic.c_str());
	}
	else if ((what == "makename" || what == "madename" || what == "madespellname") && a.size() >= 2)
	{
		// EXPECT:makename:<name>: the name in the spellmaking / enchanting screen; madename: the last enchanted
		// item carried; madespellname: the last spell made (underscores for spaces)
		std::string name;
		const InventoryItem* it = what == "madename" ? lastMade(w) : nullptr;
		const Object* o = it ? w.game.object(it->id) : nullptr;
		auto sp = what == "madespellname" && !w.madeSpells.empty() ? w.game.spells.find(w.madeSpells.back()) : w.game.spells.end();
		if (what == "makename")
			name = s.makeName;
		else if (o)
			name = o->name;
		else if (sp != w.game.spells.end())
			name = sp->second.name;
		ok = name == spaced(a[1]);
		snprintf(got, sizeof(got), "%.80s", name.empty() ? "no name" : name.c_str());
	}
	else if (what == "said" && a.size() >= 2)
	{
		// EXPECT:said:<text, underscores for spaces>: the last line said in this conversation holds it
		std::string want = lower(spaced(a[1])), line = s.dlg.history.empty() ? "" : lower(s.dlg.history.back().text);
		ok = line.find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%.80s", line.empty() ? "nothing said" : line.c_str());
	}
	else if (what == "greeting" && a.size() >= 3)
	{
		// EXPECT:greeting:<npc>:<text>: the line they would greet with now (as said) holds the text
		int ri = findRef(s, a[1]);
		std::string want = lower(spaced(a[2])), line = ri >= 0 ? lower(dialogueGreetingText(w, ri)) : "";
		ok = ri >= 0 && line.find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%.80s", ri < 0 ? "speaker not loaded" : line.empty() ? "no greeting" : line.c_str());
	}
	else if ((what == "topiclisted" || what == "topicnotlisted") && a.size() >= 2)
	{
		// EXPECT:topiclisted:<topic>: the open conversation offers the topic; topicnotlisted: it does not
		if (!s.dlg.open)
		{
			// TALK found the speaker disabled or elsewhere (a quest enables them, they wander): there is no list
			// to read; the answer: checks still test the filter
			logf("expect: SKIP %s (no conversation open)", spec.c_str());
			return true;
		}
		std::string want = lower(spaced(a[1]));
		bool listed = false;
		for (auto& t : s.dlg.topics)
			listed |= lower(t) == want;
		ok = listed == (what == "topiclisted");
		snprintf(got, sizeof(got), "%s", listed ? "listed" : "not listed");
	}
	else if (what == "topicflag" && a.size() >= 5)
	{
		// EXPECT:topicflag:<topic>:<exhausted|specific>:<op>:<0|1>: the flag the open talk's topic list gives it
		const Topic* t = w.game.topic(spaced(a[1]));
		int flags = t && s.dlg.open ? dialogueTopicFlags(w, s.dlg, *t) : 0;
		float v = (flags & (a[2] == "exhausted" ? 1 : 2)) ? 1.0f : 0.0f;
		ok = t && s.dlg.open && compare(v, a[3], (float)atof(a[4].c_str()));
		snprintf(got, sizeof(got), "%s", !t ? "no such topic" : !s.dlg.open ? "not talking" : v > 0.0f ? "set" : "not set");
	}
	else if (what == "learned" && a.size() >= 2)
	{
		// EXPECT:learned:<topic>: the player knows the topic (listed or not)
		ok = w.knownTopics.count(lower(spaced(a[1]))) > 0;
		snprintf(got, sizeof(got), "%s", ok ? "known" : "not known");
	}
	else if (what == "talked" && a.size() >= 2)
	{
		// EXPECT:talked:<npc>[:<op>:<0|1>]: the NPC's real talked-to-player flag (alone: it is set)
		int ri = findRef(s, a[1]);
		bool talked = ri >= 0 && w.refs[ri].talkedToPC;
		ok = ri >= 0 && (a.size() >= 4 ? compare(talked ? 1.0f : 0.0f, a[2], (float)atof(a[3].c_str())) : talked);
		snprintf(got, sizeof(got), "%s", ri < 0 ? "speaker not loaded" : talked ? "talked" : "not talked");
	}
	else if (what == "greetingtopic" && a.size() >= 3)
	{
		// EXPECT:greetingtopic:<npc>:[<eq|ne>:]<topic>: the Greeting topic that wins for them now
		int ri = findRef(s, a[1]);
		bool op = a.size() >= 4 && (a[2] == "eq" || a[2] == "ne");
		std::string want = lower(spaced(a[op ? 3 : 2])), topic = ri >= 0 ? lower(dialogueGreetingTopic(w, ri)) : "";
		ok = ri >= 0 && (topic == want) == (!op || a[2] == "eq");
		snprintf(got, sizeof(got), "%s", ri < 0 ? "speaker not loaded" : topic.empty() ? "no greeting" : topic.c_str());
	}
	else if (what == "voiced" && a.size() >= 5)
	{
		// EXPECT:voiced:<npc>:<topic>:<eq|ne>:<text>: the response a voiced reaction would pick (choice 0, no Info
		// Refusal fallback) holds the text (eq) or does not (ne); no answer holds nothing
		const Topic* t = w.game.topic(spaced(a[2]));
		int ri = findRef(s, a[1]);
		std::string want = lower(spaced(a[4])), line;
		if (t && ri >= 0)
			if (const Info* info = dialogueVoiced(w, *t, ri))
				line = lower(info->text);
		ok = t && ri >= 0 && (line.find(want) != std::string::npos) == (a[3] == "eq");
		snprintf(got, sizeof(got), "%.80s", !t ? "no such topic" : ri < 0 ? "speaker not loaded" : line.empty() ? "no answer" : line.c_str());
	}
	else if ((what == "answer" && a.size() >= 5) || (what == "answerchoice" && a.size() >= 6))
	{
		// EXPECT:answer:<topic>:<npc>:<eq|ne>:<text>: what the filter picks for that speaker (topics fall back to
		// Info Refusal as clicking does) holds the text (eq) or does not (ne); no answer holds nothing
		// EXPECT:answerchoice:<topic>:<npc>:<n>:<eq|ne>:<text>: the same with the filter's choice set to n (Service
		// Refusal: the service number; it is asked inverted and with no Info Refusal fallback)
		bool withChoice = what == "answerchoice";
		const Topic* t = w.game.topic(spaced(a[1]));
		int ri = findRef(s, a[2]);
		int choice = withChoice ? atoi(a[3].c_str()) : -1;
		const std::string& op = a[withChoice ? 4 : 3];
		std::string want = lower(spaced(a[withChoice ? 5 : 4])), line;
		if (t && ri >= 0)
		{
			bool refusal = t->name == "Service Refusal";
			if (const Info* info = dialogueFindInfo(w, *t, ri, choice, refusal, !refusal))
				line = lower(info->text);
		}
		bool has = line.find(want) != std::string::npos;
		ok = t && ri >= 0 && has == (op == "eq");
		snprintf(got, sizeof(got), "%.80s", !t ? "no such topic" : ri < 0 ? "speaker not loaded" : line.empty() ? "no answer" : line.c_str());
	}
	else if (what == "answerid" && a.size() >= 4)
	{
		// EXPECT:answerid:<topic>:<npc>:[<op>:]<n>: the 0-based position of the response the filter picks within its
		// topic (an Info Refusal one counts 1000 + its place there); -1 for no answer
		const Topic* t = w.game.topic(spaced(a[1]));
		int ri = findRef(s, a[2]);
		bool op = a.size() >= 5;
		int at = -1;
		if (t && ri >= 0)
			if (const Info* info = dialogueFindInfo(w, *t, ri, -1, t->name == "Service Refusal", t->name != "Service Refusal"))
			{
				const Topic* ref = w.game.topic("Info Refusal");
				if (!t->infos.empty() && info >= &t->infos.front() && info <= &t->infos.back())
					at = (int)(info - &t->infos.front());
				else if (ref && !ref->infos.empty() && info >= &ref->infos.front() && info <= &ref->infos.back())
					at = 1000 + (int)(info - &ref->infos.front());
			}
		ok = t && ri >= 0 && compare((float)at, op ? a[3] : "eq", (float)atof(a[op ? 4 : 3].c_str()));
		snprintf(got, sizeof(got), "%s", !t ? "no such topic" : ri < 0 ? "speaker not loaded" : std::to_string(at).c_str());
	}
	else if (what == "message" && a.size() >= 2)
	{
		// EXPECT:message:<text, underscores for spaces>: a message box or a notice on screen holds it
		std::string want = lower(spaced(a[1]));
		ok = false;
		for (auto& m : s.messages)
			ok |= lower(m.text).find(want) != std::string::npos;
		for (auto& nt : s.notes)
			ok |= lower(nt.text).find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%s", ok ? "shown" : "not shown");
	}
	else if (what == "screen" && a.size() >= 2)
	{
		ok = (int)s.screen == screenByName(a[1]);
		snprintf(got, sizeof(got), "screen %d", (int)s.screen);
	}
	else
	{
		bool arg = hasArg(what);
		std::string key = what + ":" + (arg && a.size() >= 2 ? a[1] : "");
		size_t op = arg ? 2 : 1;
		float v = 0.0f;
		bool known = a.size() > op + 1 && numberOf(s, what, arg ? a[1] : "", v);
		bool snapped = true;
		float target = known ? wanted(a[op + 1], key, snapped) : 0.0f;
		ok = known && snapped && compare(v, a[op], target);
		if (!known)
			snprintf(got, sizeof(got), "unknown check");
		else if (!snapped)
			snprintf(got, sizeof(got), "%g, nothing snapped for %s", v, key.c_str());
		else
			snprintf(got, sizeof(got), "%g, wanted %s %g", v, a[op].c_str(), target);
	}
	logf("expect: %s %s (got %s)", ok ? "PASS" : "FAIL", spec.c_str(), got);
	return ok;
}
