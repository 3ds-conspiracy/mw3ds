// EXPECT values: stats, effects, formulas, prices, items
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

std::optional<bool> numberOfStats(Session& s, const std::string& what, const std::string& arg, float& v)
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
	// attr: the base value; attrdamage: what Damage Attribute has taken off it until restored (a shrine's Restore Attributes)
	if (what == "attr" || what == "attrups" || what == "attrdamage")
	{
		int k = attrIndex(w, arg);
		if (k < 0 || k >= 8)
			return false;
		v = what == "attr" ? (float)st.attributes[k] : what == "attrdamage" ? st.attrDamage[k] : (float)st.attrSkillUps[k];
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
	if (what == "levitating") { v = w.effectTotal(10) > 0.0f ? 1.0f : 0.0f; return true; }
	// where the player's feet are (SNAP one, then EXPECT it moved at most / at least so much with "@")
	if (what == "playerx" || what == "playery" || what == "playerz") { v = w.player.feet[what[6] - 'x']; return true; }
	// which way the player looks, in degrees (yaw: + toward east, not wrapped; pitch: + up): SNAP one, EXPECT how far it turned
	if (what == "yaw") { v = w.player.yaw * 180.0f / 3.14159265f; return true; }
	if (what == "pitch") { v = w.player.pitch * 180.0f / 3.14159265f; return true; }
	// how far the player went on the flat since SNAP:moved
	// playerdist:<x>,<y>: how far the player stands from that spot on the ground (units)
	if (what == "playerdist")
	{
		float p[2] = {};
		sscanf(arg.c_str(), "%f,%f", &p[0], &p[1]);
		v = hypotf(w.player.feet[0] - p[0], w.player.feet[1] - p[1]);
		return true;
	}
	if (what == "moved") { v = hypotf(w.player.feet[0] - s_movedFrom[0], w.player.feet[1] - s_movedFrom[1]); return true; }
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
	// The formula pieces of mwmechanics/formulas.cpp (spec/combat.md, spec/derived.md, spec/crime.md); "a,b": two arguments
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
	// poseevery / skinevery:<distance>: the frames between an actor's poses / re-skins that far from the camera
	if (what == "poseevery" || what == "skinevery")
	{
		float d = (float)atof(arg.c_str());
		v = (float)(what == "poseevery" ? actorPoseEvery(d * d) : actorSkinEvery(d * d));
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
	return std::nullopt;
}

std::optional<bool> numberOfRules(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
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
	if (what == "conhas")            // conhas:<text>:eq:1: 1 when what the last console line printed holds the text ('_' a space)
	{
		v = 0;
		std::string want = lower(spaced(arg));
		for (size_t k = s.console.runStart; k < s.console.lines.size(); k++)
			if (lower(s.console.lines[k].text).find(want) != std::string::npos)
				v = 1;
		return true;
	}
	if (what == "mapknown")          // mapknown:<place>: 1 when the place is on the map (visited, ShowMap, FillMap)
	{
		v = w.mapKnown.count(lower(spaced(arg))) ? 1.0f : 0.0f;
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
	return std::nullopt;
}

std::optional<bool> numberOfItems(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
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
		v = (o->recType == REC_ARMO || o->recType == REC_CLOT) ? (float)o->flags : 0.0f;
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
	return std::nullopt;
}
