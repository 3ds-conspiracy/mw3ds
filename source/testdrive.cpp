// Test driver: see include/testdrive.h
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive.h"
#include "session.h"
#include "log.h"
#include "audio.h"

extern const char* g_playerBlock;
// Where SNAP:moved was taken (EXPECT:moved measures from it)
static float s_movedFrom[2] = { 0.0f, 0.0f };

static std::string spaced(std::string s)
{
	for (auto& c : s)
		if (c == '_')
			c = ' ';
	return s;
}

// Tokens split at every ':'; a '|' inside a part stands for a colon of the name ("Wolverine_Hall|_Mage's_Guild")
static std::vector<std::string> split(const std::string& s)
{
	std::vector<std::string> out;
	size_t p = 0;
	while (true)
	{
		size_t q = s.find(':', p);
		out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
		for (auto& c : out.back())
			if (c == '|')
				c = ':';
		if (q == std::string::npos)
			return out;
		p = q + 1;
	}
}

// A cell name against a wanted start: exact, or the wanted text as a whole-word start of it ("Balmora" is not
// "Balmoral"; "Wolverine Hall" is the hall and its rooms). A wanted name ending in '$' must match exactly.
static bool cellNameMatches(const std::string& cell, std::string want)
{
	std::string here = lower(cell);
	want = lower(want);
	bool exact = !want.empty() && want.back() == '$';
	if (exact)
		want.pop_back();
	if (here == want)
		return true;
	if (exact || here.compare(0, want.size(), want) != 0)
		return false;
	char next = here[want.size()];
	return !isalnum((unsigned char)next) || !isalnum((unsigned char)want.back());
}

// ---- mechanics tests: names, numbers, snapshots

#include <map>
#include "screens.h"
#include "collision.h"
#include "linear.h"

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

// How far a player has to walk to a door: the grid route's length (squared, to compare like a 3D distance squared).
// A door the grid can't lead to (another floor, behind a wall, hidden under a stair) ranks after every reachable one,
// nearest in 3D among those.
static float doorWalkRank(Session& s, const Ref& r, float d3)
{
	std::vector<int> route;
	if (s.w.findPath(s.w.player.feet, r.pos, route) && !route.empty())
	{
		const float* q = s.w.pathPoints[route.back()].pos;
		float ex = q[0] - r.pos[0], ey = q[1] - r.pos[1], ez = q[2] - r.pos[2];
		float len = sqrtf(ex * ex + ey * ey + ez * ez);
		if (len < 400.0f)
		{
			const float* a = s.w.player.feet;
			for (int k : route)
			{
				const float* b = s.w.pathPoints[k].pos;
				float sx = b[0] - a[0], sy = b[1] - a[1], sz = b[2] - a[2];
				len += sqrtf(sx * sx + sy * sy + sz * sz);
				a = b;
			}
			return len * len;
		}
	}
	return d3 + 1e12f;
}

// preferLocked: of shared door / container ids the locked ones first (a lockpick is held up to what is locked)
static int findRef(Session& s, const std::string& id, bool byWalk = true, bool preferLocked = false)
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
	// several share the id (a creature placed again after one died, a field of identical plants, shared door ids): the
	// nearest living one in the loaded cells, as a player takes the one in front of them (a dead one only if all are)
	if (ri >= 0 && ri != s.testPlaced)
	{
		const std::string want = s.w.refs[ri].idLower;
		const bool wantDead = s.w.refs[ri].dead;
		const float* pf = s.w.player.feet;
		float best = 1e30f;
		int pick = -1;
		s.w.forLoadedRefs([&](int i) {
			const Ref& c = s.w.refs[i];
			if (c.idLower != want || !s.w.active(i) || (wantDead && !c.dead))
				return;
			if (c.dead && !wantDead)
				return;
			float dx = c.pos[0] - pf[0], dy = c.pos[1] - pf[1], dz = c.pos[2] - pf[2], d = dx * dx + dy * dy + dz * dz;
			// (shared door ids: the one the player can walk to soonest, not the one nearest through a wall or a floor)
			if (byWalk && c.type == "DOOR" && d < 1e11f)
				d = doorWalkRank(s, c, d);
			if (d < best)
			{
				best = d;
				pick = i;
			}
		});
		if (pick >= 0)
			ri = pick;
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
		|| what == "refcell" || what == "refforcesneak" || what == "refpkg" || what == "refpkgdone" || what == "refcombat" || what == "refrun" || what == "refdying" || what == "refknock" || what == "refsoultrap" || what == "refally" || what == "refdist" || what == "refaimed" || what == "refabove" || what == "refwater" || what == "refaimable" || what == "refbox" || what == "refitem" || what == "reflock" || what == "reftrap" || what == "scripttarget" || what == "reflocal" || what == "reflocalfrac" || what == "scriptlocal" || what == "refloopvol" || what == "soundstarted" || what == "spellcost" || what == "castchance" || what == "brewedmag" || what == "brewedduration" || what == "charge"
		|| what == "count" || what == "repairamount" || what == "rechargegain" || what == "lockchance" || what == "trapchance"
		|| what == "persuadechance" || what == "persuadepart" || what == "enchantcastcost" || what == "resistbase"
		|| what == "crimebounty" || what == "skillneed" || what == "attackterm" || what == "knockodds" || what == "falldamage" || what == "poseevery" || what == "skinevery"
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
	if (what == "levitating") { v = w.effectTotal(10) > 0.0f ? 1.0f : 0.0f; return true; }
	// where the player's feet are (SNAP one, then EXPECT it moved at most / at least so much with "@")
	if (what == "playerx" || what == "playery" || what == "playerz") { v = w.player.feet[what[6] - 'x']; return true; }
	// how far the player went on the flat since SNAP:moved
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
	// the crosshair name's widest line in pixels (-1: nothing aimed at), and its lines; the bottom prompt's lines
	if (what == "popupwidth" || what == "popuplines" || what == "promptlines")
	{
		if (s.target < 0)
			v = -1.0f;
		else if (what == "promptlines")
			v = (float)s.targetPromptLines(s.target).size();
		else
		{
			std::vector<std::string> lines = s.targetPopupLines(s.target);
			v = what == "popuplines" ? (float)lines.size() : 0.0f;
			if (what == "popupwidth")
				for (auto& l : lines)
					v = fmaxf(v, uiTextWidth(l, 0.55f));
		}
		return true;
	}
	if (what == "deletingsave") { v = s.deletingSave ? 1.0f : 0.0f; return true; }    // the Saves screen asks "Delete ...?"
	if (what == "vfxcount") { v = (float)s.vfx.size(); return true; }      // spell visuals showing now
	// how far above the player's feet the first spell visual stands (-999 with none): a self spell's wraps the body from the feet
	if (what == "vfxheight") { v = s.vfx.empty() ? -999.0f : s.vfx[0].pos[2] - w.player.feet[2]; return true; }
	// how far (flat) the first spell visual stands from the player (-999 with none): a self spell's travels with the caster
	if (what == "vfxdist") { v = s.vfx.empty() ? -999.0f : hypotf(s.vfx[0].pos[0] - w.player.feet[0], s.vfx[0].pos[1] - w.player.feet[1]); return true; }
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
		const bool byDest = ri >= 0;
		if (ri < 0)
			ri = findRef(s, arg);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(arg));
		if (ri < 0)
			return false;
		// (several share the id: the most locked one, so a case names the door it means whichever the player stands nearer)
		int most = ri;
		w.forLoadedRefs([&](int i) {
			if (!byDest && w.refs[i].idLower == w.refs[ri].idLower && abs(w.refs[i].lockLevel) > abs(w.refs[most].lockLevel))
				most = i;
		});
		v = (float)w.refs[most].lockLevel;
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
	if (what == "vmmeshes")           // meshes the first-person view model draws now (arms, then what they hold)
	{
		v = s.vm.ready && !s.vm.set.actors.empty() ? (float)s.vm.set.actors[0].meshes.size() : 0.0f;
		return true;
	}
	if (what == "bowstring")          // how far back the held bow's string is drawn (0 at rest, 1 fully back)
	{
		v = s.vm.ready && !s.vm.set.actors.empty() ? actorMorphWeight(s.vm.set.actors[0]) : 0.0f;
		return true;
	}
	if (what == "swimming")
	{
		v = s.w.player.swimming ? 1.0f : 0.0f;
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
		else if (what == "refaimed") v = s.target == ri ? 1.0f : 0.0f;   // the crosshair is on it
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
		else if (what == "refbox") v = r.hasBox ? 1.0f : 0.0f;       // it has a mesh to aim at (a banner's cloth)
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
	else if (what == "quizbottom") v = s.quizBottom;      // the class quiz: the bottom of its focused answer
	else if (what == "notesright") v = s.notesRight;      // the right edge of the notices on the top screen
	else if (what == "messagesidebar") v = s.messageSidebar;   // a message box's buttons are in the right column
	else if (what == "shimmerframes") v = (float)rendererCausticFrames();   // enchanted items' shimmer frames read
	else if (what == "linearfree") v = (float)(linearSpaceFree() / 1024);   // KB of linear memory free
	else if (what == "uiintact") v = uiThemeIntact() ? 1.0f : 0.0f;  // the UI's font and frames still as loaded
	else if (what == "groundgaps") { extern int g_groundGaps; v = (float)g_groundGaps; }   // cracks in the last SHOT (native only, -1 on the 3DS)
	else if (what == "missingtex")
	{
		// textures of the player's cell that aren't there (drawn plain, in their vertex colours)
		v = 0;
		if (w.current >= 0 && w.cells[w.current].live)
		{
			const Cell& c = w.cells[w.current].live->cell;
			for (size_t i = 0; i < c.textures.size(); i++)
				v += !c.textures[i] && !c.textureNames[i].empty();
		}
	}
	else if (what == "waterscroll") v = rendererWaterScroll();       // water's slide, texture repeats a second
	else if (what == "controls") v = w.controlsEnabled ? 1.0f : 0.0f;   // DisablePlayerControls / EnablePlayerControls
	else if (what == "jumping") v = w.jumpingEnabled ? 1.0f : 0.0f;     // DisablePlayerJumping / EnablePlayerJumping
	else if (what == "skillsum") { v = 0; for (int k = 0; k < 27; k++) v += st.skills[k]; }   // all 27 skills (jail time)
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
		{ "classmethod", SCR_CLASS_METHOD }, { "classquiz", SCR_CLASS_QUIZ }, { "options", SCR_OPTIONS },
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
	// LEGIT: a service is had through the dialogue of whoever gives it (its buttons), never from across the map
	auto notTalking = [&](int ri) { return s.testLegit && !s.talkingTo(ri); };
	if (verb == "SNAP" && a.size() >= 2)
	{
		if (a[1] == "moved")
			memcpy(s_movedFrom, w.player.feet, sizeof(s_movedFrom));
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
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
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
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
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
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
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
		if (notTalking(ri) && !(s.screen == SCR_BARTER && s.barterRef == ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		// (the trade happens as the Barter screen's; then back to what was open: the dialogue, as its Done button does)
		Screen before = s.screen;
		s.openBarter(ri);
		s.screen = before;
		Ref& m = w.refs[ri];
		int index = -1;
		const Object* o = nullptr;
		if (verb == "SELL")
		{
			index = carried(w, a[2]);
			o = index >= 0 ? w.game.object(w.inventory[index].id) : nullptr;
		}
		int from = -1;
		bool found = verb == "SELL" && index >= 0;
		if (verb == "BUY")
			for (auto& g : s.merchantGoods(ri))
			{
				std::string id = s.merchantGood(g).second;
				if (lower(id) == lower(a[2]) || lower(id) == lower(spaced(a[2])))
				{
					from = g.first;
					index = g.second;
					o = w.game.object(id);
					found = true;
				}
			}
		if (!found || !o)
		{
			// (what they do have, to pick from)
			std::string have;
			if (verb == "BUY")
				for (auto& g : s.merchantGoods(ri))
					if (have.size() < 300)
						have += " " + s.merchantGood(g).second;
			return fail(a[2] + (verb == "SELL" ? " not carried" : " not among their goods (they have:" + have + ")"));
		}
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
	// FACE:@x,y: turn to look at that spot (level), as the stick would
	if (verb == "FACE" && a.size() >= 2 && a[1][0] == '@')
	{
		float x, y;
		if (sscanf(a[1].c_str() + 1, "%f,%f", &x, &y) != 2)
			return fail("FACE:@x,y wants two numbers");
		w.player.yaw = atan2f(x - w.player.feet[0], y - w.player.feet[1]);
		w.player.pitch = 0.0f;
		return true;
	}
	if ((verb == "FACE" || verb == "CASTAT") && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !w.active(ri))
			return fail(a[1] + " not loaded");
		const Ref& r = w.refs[ri];
		// (FACE:<ref>:mid: the middle of its box, as the driver aims at a thing to take it; else a person's chest)
		bool mid = verb == "FACE" && a.size() >= 3 && a[2] == "mid";
		float dx = (mid ? (r.boxMin[0] + r.boxMax[0]) * 0.5f : r.pos[0]) - w.player.feet[0];
		float dy = (mid ? (r.boxMin[1] + r.boxMax[1]) * 0.5f : r.pos[1]) - w.player.feet[1];
		float dz = (mid ? (r.boxMin[2] + r.boxMax[2]) * 0.5f : r.pos[2] + 80.0f) - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
		w.player.yaw = atan2f(dx, dy);
		w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
		if (verb == "CASTAT" && a.size() >= 3)
		{
			std::string sp = a[2];
			for (size_t k = 3; k < a.size(); k++)
				sp += ":" + a[k];
			// the id as given when there's such a spell (mw3ds_test_x), else with '_' as spaces ("fire_bite")
			w.stats.selectedSpell = sp.compare(0, 5, "item:") == 0 || w.game.spells.count(lower(sp)) ? lower(sp) : lower(spaced(sp));
			if (s.testLegit && !TestDriver::canCast(s, w.stats.selectedSpell))
				return fail(sp + " isn't a known spell or a carried item (LEGIT)");
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
		// the one in the crosshair when it has that id (shared door ids), else the nearest in 3D: a tool is held up to what is
		// in front of the player, not to the door the shortest walk leads to
		int ri = findRef(s, a[1], false, verb == "USELOCKPICK"), ti = carried(w, a[2]);
		if (s.target >= 0 && s.target < (int)w.refs.size() && (w.refs[s.target].idLower == lower(a[1]) || w.refs[s.target].idLower == lower(spaced(a[1]))) &&
			(verb != "USELOCKPICK" || ri < 0 || w.refs[s.target].lockLevel > 0 || w.refs[ri].lockLevel <= 0))
			ri = s.target;
		if (ri < 0)
			return fail(a[1] + " not loaded");
		if (ti < 0)
			return fail(a[2] + " not carried");
		if (!w.inventory[ti].equipped)
			s.equipItem(ti, true);
		{
			const Ref& t = w.refs[ri];
			logf("drive: %s on %s at %.0f %.0f %.0f (%s%d)", a[0].c_str(), t.id.c_str(), t.pos[0], t.pos[1], t.pos[2],
				(t.type == "DOOR" || t.type == "CONT") ? "lock " : "not a door or container, lock ", t.lockLevel);
		}
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
	if (verb == "MEMHOLD" && a.size() >= 2)
	{
		// MEMHOLD:<KB>: hold that much linear memory (in 256 KB pieces) to play as the device does when it is nearly
		// full; MEMHOLD:0 lets it go
		static std::vector<void*> held;
		for (void* p : held)
			lockedLinearFree(p);
		held.clear();
		for (int kb = atoi(a[1].c_str()); kb > 0; kb -= 256)
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

// What a player has on the Magic screen: a spell they know (race and sign spells too), or "item:<id>" carried
bool TestDriver::canCast(Session& s, const std::string& selected)
{
	if (selected.compare(0, 5, "item:") == 0)
		return s.w.itemCount(lower(selected.substr(5))) > 0;
	std::vector<std::string> known = s.knownSpells();
	return std::find(known.begin(), known.end(), lower(selected)) != known.end()
		|| std::find(known.begin(), known.end(), lower(spaced(selected))) != known.end();
}

// Tokens a LEGIT run may still use: what a player does with the buttons, the touch screen and the menus, and checks
// that only look (EXPECT, SNAP, CHECK, PROBE, SHOT). Anything else sets the world up and is refused after LEGIT
// (an allow list: a new token is refused until it is added here as a player action).
bool TestDriver::playerToken(const std::string& tok)
{
	static const char* const kPlayer[] = {
		"A", "B", "X", "Y", "L", "R", "UP", "DOWN", "LEFT", "RIGHT", "SELECT", "START", "ZL", "TAP", "DRAG", "STICKDOWN",
		"STICKUP", "MSG", "SHOT", "SAVE", "LOAD", "SAVESEL", "LEGIT",
		"EXPECT", "SNAP", "CHECK", "PROBE",
		"WALKTO", "FLYTO", "HOPTO", "ESCORT", "DOORTO", "ACTIVATE", "PICKUP", "LOOT", "PUT", "KILL", "STRIKE", "FACE", "SNEAK",
		"TOPIC", "CHOICE", "CHOICEVAL", "PERSUADE", "TRAVEL", "BARTER", "BARTERSEL", "BUY", "SELL", "TRAIN", "SPELLMAKE",
		"ENCHANTAT", "ENCHANT", "ENCHITEM", "ENCHGEM", "ENCHTYPE", "ADDEFFECT", "CONFIRM", "TYPE", "LEVELUP",
		"EQUIP", "USE", "READ", "DROP", "QUICKSET", "CAST", "CASTAT", "CASTMADESPELL", "USEMADE", "EQUIPMADE",
		"RECHARGEMADE", "RECHARGE", "REPAIR", "BREW", "DRINKBREWED", "USELOCKPICK", "USEPROBE", "SLEEP", "SLEEPUNTIL",
	};
	size_t colon = tok.find(':');
	std::string name = tok.substr(0, colon);
	// SLEEP:h only: SLEEP:h:1 / :2 decide whether a creature comes
	if (name == "SLEEP" && colon != std::string::npos && tok.find(':', colon + 1) != std::string::npos)
		return false;
	for (const char* k : kPlayer)
		if (name == k)
			return true;
	return false;
}

// The path grid of a cave can be split in two pieces with no link between them: findPath then has no route. A player
// walks as far as the grid goes and then crosses by sight: to the point of the goal's piece that can be seen from a
// point of the start's piece (a gap, a step down), else along the start's piece to the point nearest the goal.
static void gridPiece(World& w, int from, std::vector<char>& in)
{
	in.assign(w.pathPoints.size(), 0);
	std::vector<int> todo(1, from);
	in[from] = 1;
	while (!todo.empty())
	{
		int c = todo.back();
		todo.pop_back();
		for (int nb : w.pathPoints[c].links)
		{
			if (nb < 0 || nb >= (int)in.size() || in[nb] || w.pathPoints[nb].cell < 0 || !w.cells[w.pathPoints[nb].cell].live)
				continue;
			in[nb] = 1;
			todo.push_back(nb);
		}
	}
}

static float gridDist(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static bool splitGridPath(Session& s, const float from[3], const float to[3], std::vector<int>& path)
{
	World& w = s.w;
	int st = w.nearestPathPoint(from), gl = w.nearestPathPoint(to);
	if (st < 0 || gl < 0 || st == gl)
		return false;
	std::vector<char> inS, inG;
	gridPiece(w, st, inS);
	gridPiece(w, gl, inG);
	std::vector<std::pair<float, int>> sp, gp;
	for (size_t i = 0; i < inS.size(); i++)
	{
		if (inS[i])
			sp.push_back({ gridDist(w.pathPoints[i].pos, to), (int)i });
		if (inG[i])
			gp.push_back({ gridDist(w.pathPoints[i].pos, to), (int)i });
	}
	std::sort(sp.begin(), sp.end());
	std::sort(gp.begin(), gp.end());
	// (the goal's points nearest the goal first; for each the start's points nearest to it, seen from one another)
	int bestA = -1, bestB = -1;
	for (size_t k = 0; k < gp.size() && k < 12 && bestA < 0; k++)
	{
		const float* b = w.pathPoints[gp[k].second].pos;
		std::vector<std::pair<float, int>> near;
		for (size_t i = 0; i < sp.size(); i++)
			near.push_back({ gridDist(w.pathPoints[sp[i].second].pos, b), sp[i].second });
		std::sort(near.begin(), near.end());
		for (size_t j = 0; j < near.size() && j < 40; j++)
		{
			const float* a = w.pathPoints[near[j].second].pos;
			float pa[3] = { a[0], a[1], a[2] + 50.0f }, pb[3] = { b[0], b[1], b[2] + 50.0f };
			if (near[j].first < 3000.0f && fabsf(a[2] - b[2]) < 1500.0f && w.lineOfSight(pa, pb))
			{
				bestA = near[j].second;
				bestB = gp[k].second;
				break;
			}
		}
	}
	std::vector<int> head, tail;
	if (bestA >= 0)
	{
		if (!w.findPath(from, w.pathPoints[bestA].pos, head))
			head.clear();
		// (the end of the goal's piece nearest the goal on the map and in height: not the pool under a door's ledge)
		int end = bestB;
		float endCost = 1e30f;
		for (size_t i = 0; i < inG.size(); i++)
		{
			const float* q = w.pathPoints[i].pos;
			float c = hypotf(q[0] - to[0], q[1] - to[1]) + 2.0f * fabsf(q[2] - to[2]);
			if (inG[i] && c < endCost)
			{
				endCost = c;
				end = (int)i;
			}
		}
		if (!w.findPath(w.pathPoints[bestB].pos, w.pathPoints[end].pos, tail))
			tail.clear();
		path = head;
		path.push_back(bestB);
		for (int t : tail)
			if (t != bestB)
				path.push_back(t);
		logf("drive: split path grid: crossing from point %d to %d by sight", bestA, bestB);
		return true;
	}
	// (a cave only: outdoors the grid is patchy, the land between is open, and the straight walk is the better guess)
	if (w.current < 0 || !w.cells[w.current].interior || sp.empty() || !w.findPath(from, w.pathPoints[sp[0].second].pos, path))
		return false;
	logf("drive: split path grid: along the start's piece to point %d, nearest the goal", sp[0].second);
	return true;
}

// The grid route for a walk. World::findPath snaps both ends to the nearest grid point up to 2500 away, a goal in plain
// sight included, so a walk to a spot would first head for a point far off its way. A route is kept only when it
// helps: its last points on another floor than the goal (a walkway above a door, a stair landing) are dropped, and a
// walk to a spot goes straight unless the route still ends near it.
// Where a walk to a thing ends: its origin, but for a door whose origin sits well outside its own mesh (an Ashlander yurt's:
// the model is placed 336 units from the reference, inside the tent) the middle of its box, where the door shows (other
// doors keep their origin: a cave door's box takes in the rock round it)
static void walkTarget(const Ref& r, bool activate, float out[3])
{
	out[0] = r.pos[0];
	out[1] = r.pos[1];
	out[2] = r.pos[2];
	if (activate && r.type == "DOOR" && r.boxMax[0] > r.boxMin[0])
	{
		float ox = fmaxf(r.boxMin[0] - r.pos[0], fmaxf(0.0f, r.pos[0] - r.boxMax[0]));
		float oy = fmaxf(r.boxMin[1] - r.pos[1], fmaxf(0.0f, r.pos[1] - r.boxMax[1]));
		if (ox * ox + oy * oy > 100.0f * 100.0f)
		{
			out[0] = (r.boxMin[0] + r.boxMax[0]) * 0.5f;
			out[1] = (r.boxMin[1] + r.boxMax[1]) * 0.5f;
		}
	}
}

static bool planPath(Session& s, const float from[3], const float to[3], bool spot, std::vector<int>& path)
{
	World& w = s.w;
	if (!w.findPath(from, to, path))
	{
		path.clear();
		// the grid is in two pieces (Urshilaku Karma Burial): no route from here to the goal's piece
		if (!splitGridPath(s, from, to, path))
			return false;
		return true;
	}
	// trailing points stacked over or under the goal (a walkway above a door) lead to the wrong floor: drop them. Only
	// those near it on the map: a route that climbs a ramp to a door set higher or lower than its origin is kept whole
	// (the Vivec cantons: dropping every point by height alone left the walk going straight at a wall)
	std::vector<int> whole = path;
	while (!path.empty())
	{
		const float* q = w.pathPoints[path.back()].pos;
		float dx = q[0] - to[0], dy = q[1] - to[1];
		if (dx * dx + dy * dy > 200.0f * 200.0f || fabsf(q[2] - to[2]) <= (spot ? 250.0f : 150.0f))
			break;
		path.pop_back();
	}
	if (path.empty())
		path = whole;
	if (spot)
	{
		const float* q = w.pathPoints[path.back()].pos;
		float dx = q[0] - to[0], dy = q[1] - to[1];
		if (dx * dx + dy * dy > 350.0f * 350.0f)
		{
			path.clear();
			return false;
		}
	}
	return true;
}

// Hovering over a floor it can stand on (a levitating player at the foot of a stair, in a hall): collision floor within
// 60 units under the feet. Outdoors the land is not in the cells' collision: no floor found counts as hanging in the air.
static bool standsOnFloor(Session& s)
{
	const Player& p = s.w.player;
	Scene sc = s.w.scene();
	for (Cell* c : sc.cells)
	{
		float fz;
		if (collisionFloor(c->collision, p.feet[0], p.feet[1], p.feet[2] + 30.0f, p.feet[2] - 60.0f, &fz))
			return true;
	}
	return false;
}

// Whether the crosshair is on that reference now, from the view as it is this frame (the session's own pick of last frame is
// the view before the driver turned it)
static bool aimsAt(Session& s, int ref)
{
	const Player& p = s.w.player;
	float cp = cosf(p.pitch);
	float eye[3] = { p.feet[0], p.feet[1], playerEyeZ(p) };
	float dir[3] = { sinf(p.yaw) * cp, cosf(p.yaw) * cp, sinf(p.pitch) };
	return worldPick(s.w, eye, dir, 192.0f + s.w.effectTotal(59) * 22.0f) == ref;
}

// An escort fallen behind (ESCORT:<npc>): the player stands and lets them catch up, as a player waits. Not when they
// no longer follow (a quest script stopped them: their package is the script's idle, or they wander)
static bool escortLagging(Session& s, const std::string& escort, float limit, int* who = nullptr)
{
	if (escort.empty())
		return false;
	int e = findRef(s, escort);
	if (who)
		*who = e;
	if (e < 0 || !s.w.active(e) || s.w.refs[e].dead)
		return false;
	int pkg = s.w.refs[e].aiPackage;
	if (pkg == AIPKG_IDLE || pkg == AIPKG_NONE || pkg == AIPKG_WANDER)
		return false;
	float d = s.w.distanceToPlayer(e);
	return d > limit && d < 20000.0f;
}

// The weapon slot holds a lockpick or a probe (USELOCKPICK / USEPROBE equip it, as in OpenMW): a swing with it does nothing.
// A player takes the best carried melee weapon back in hand before a fight. Returns what is in hand afterwards ("" none).
static std::string handItem(Session& s, bool takeWeapon)
{
	World& w = s.w;
	int held = -1;
	for (size_t k = 0; k < w.inventory.size(); k++)
		if (w.inventory[k].equipped)
			if (const Object* o = w.game.object(w.inventory[k].id))
				if ((o->type == "WEAP" && o->subtype < WEAP_ARROW) || o->type == "LOCK" || o->type == "PROB")
					held = (int)k;
	if (held < 0)
		return "";
	const Object* ho = w.game.object(w.inventory[held].id);
	if (takeWeapon && ho && (ho->type == "LOCK" || ho->type == "PROB"))
	{
		int best = -1;
		float bestDmg = 0.0f;
		for (size_t k = 0; k < w.inventory.size(); k++)
		{
			const Object* o = w.game.object(w.inventory[k].id);
			if (!o || o->type != "WEAP" || o->subtype >= WEAP_BOW)
				continue;
			float dmg = (float)std::max(o->chop[1], std::max(o->slash[1], o->thrust[1]));
			if (dmg > bestDmg)
			{
				bestDmg = dmg;
				best = (int)k;
			}
		}
		if (best >= 0)
		{
			std::string was = w.inventory[held].id;
			if (s.equipItem(best))
			{
				logf("drive: %s was in hand: took %s for the fight", was.c_str(), w.inventory[best].id.c_str());
				return w.inventory[best].id;
			}
		}
	}
	return w.inventory[held].id;
}

// A route out of where the player is stuck, found by trying the player's own steps (the game's playerUpdate on a copy: run or
// running jump, sixteen headings, swimming up / level / down) and following the cheapest-looking reached spot towards the goal.
// Gives the spots reached on the way, from the first step. Stops after maxStates or 90 s of the machine's time.
static bool floodRoute(Session& s, const float goal[3], float radius, std::vector<TestDriver::SwimLeg>& out)
{
	struct Node { Player p; int parent; bool jump; int depth; float yaw, pitch; int frames; };
	Scene sc = s.w.scene();
	std::vector<Node> nodes;
	nodes.reserve(6000);
	std::map<std::tuple<int, int, int>, int> seen;
	auto key = [](const Player& p) { return std::make_tuple((int)floorf(p.feet[0] / 64), (int)floorf(p.feet[1] / 64), (int)floorf(p.feet[2] / 48)); };
	auto cost = [&](const Node& n) {
		return hypotf(n.p.feet[0] - goal[0], n.p.feet[1] - goal[1]) + fabsf(n.p.feet[2] - goal[2]) * 0.5f + n.depth * 25.0f;
	};
	typedef std::pair<float, int> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > open;
	nodes.push_back({ s.w.player, -1, false, 0, 0.0f, 0.0f, 0 });
	nodes[0].p.levitate = 0.0f;
	seen[key(nodes[0].p)] = 0;
	open.push(Entry(cost(nodes[0]), 0));
	float minZ = fminf(s.w.player.feet[2], goal[2]) - 1500.0f;
	u64 t0 = osGetTime();
	int found = -1;
	while (!open.empty() && found < 0 && (int)nodes.size() < 6000 && osGetTime() - t0 < 90000)
	{
		int bi = open.top().second;
		open.pop();
		for (int h = 0; h < 16 && found < 0; h++)
			for (int jmp = 0; jmp < (nodes[bi].p.onGround ? 2 : 1) && found < 0; jmp++)
				for (int pi = 0; pi < (nodes[bi].p.swimming ? 3 : 1) && found < 0; pi++)
				{
					Player p = nodes[bi].p;
					p.yaw = h * 3.14159265f / 8.0f;
					p.pitch = pi == 1 ? -0.7f : pi == 2 ? 0.7f : 0.0f;
					p.vz = 0.0f;
					for (int f = 0; f < 24 && found < 0; f++)
					{
						PlayerInput in = {};
						in.moveY = 1.0f;
						in.jump = jmp && f == 0;
						playerUpdate(p, sc, in, 1.0f / 30.0f);
						if (f % 4 == 3 && (p.onGround || p.swimming) && p.feet[2] > minZ)
						{
							auto k = key(p);
							if (!seen.count(k))
							{
								int ni = (int)nodes.size();
								nodes.push_back({ p, bi, jmp != 0, nodes[bi].depth + 1, h * 3.14159265f / 8.0f, pi == 1 ? -0.7f : pi == 2 ? 0.7f : 0.0f, f + 1 });
								seen[k] = ni;
								open.push(Entry(cost(nodes[ni]), ni));
								if (hypotf(p.feet[0] - goal[0], p.feet[1] - goal[1]) < radius && fabsf(p.feet[2] - goal[2]) < 200.0f)
									found = ni;
							}
						}
					}
				}
	}
	logf("drive: route by trial steps: %d spots tried in %.1f s, %s", (int)nodes.size(), (osGetTime() - t0) / 1000.0f, found >= 0 ? "found" : "none");
	if (found < 0)
		return false;
	out.clear();
	for (int i = found; nodes[i].parent >= 0; i = nodes[i].parent)
	{
		TestDriver::SwimLeg leg = { { nodes[i].p.feet[0], nodes[i].p.feet[1], nodes[i].p.feet[2] }, nodes[i].p.swimming, nodes[i].jump, nodes[i].yaw, nodes[i].pitch, nodes[i].frames / 30.0f };
		out.push_back(leg);
	}
	std::reverse(out.begin(), out.end());
	return true;
}

static bool spotBeside(Session& s, const float aim[3], float fromZ, float out[2]);

void TestDriver::fail(const char* why)
{
	logf("drive: FAIL %s %s: %s", kind == KILL ? "kill" : kind == WALK ? "walk to" : "activate", id.c_str(), why);
	kind = NONE;
}

bool TestDriver::start(Session& s, const std::string& token)
{
	if (!retrying)
	{
		viewTries = talkTries = 0;
		thenDo.clear();
	}
	std::vector<std::string> a = split(token);
	const std::string& verb = a[0];
	if (verb == "GOD")
	{
		// GOD:2: only kept alive; the player's blows roll, hurt and wear as usual (uber quest tests)
		s.testGod = a.size() < 2 || a[1] != "0";
		s.testGodBlows = a.size() < 2 || a[1] != "2";
		logf("test: god mode %s", !s.testGod ? "off" : s.testGodBlows ? "on" : "on (kept alive only)");
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
	if (verb == "ESCORT" && a.size() >= 2)
	{
		escort = a[1] == "-" ? "" : a[1];
		logf("drive: escort %s", escort.empty() ? "ended" : escort.c_str());
		return false;
	}
	if ((verb == "FLYTO" || verb == "HOPTO") && a.size() >= 2)
	{
		id = a[1];
		if (a[1][0] != '@' || sscanf(a[1].c_str() + 1, "%f,%f,%f", &goal[0], &goal[1], &goal[2]) != 3)
		{
			logf("drive: FAIL %s %s: wants @x,y,z", verb.c_str(), a[1].c_str());
			return false;
		}
		kind = verb == "FLYTO" ? FLY : HOP;
		memcpy(aim, goal, sizeof(aim));
		const Player& p = s.w.player;
		if (kind == HOP && p.levitate > 0.0f)
		{
			fail("levitating: no ground to jump from (let it run out, or fly)");
			return false;
		}
		lowFly = a.size() >= 3 && a[2] == "low";
		bool beside = a.size() >= 3 && a[2] == "beside";
		cruise = lowFly ? fmaxf(p.feet[2], goal[2]) + 120.0f : a.size() >= 3 && !beside ? (float)atof(a[2].c_str()) : fmaxf(p.feet[2], goal[2]) + 1500.0f;
		// FLYTO:@x,y,z:beside (a spot on a roof, a tower top or a ledge): land a few hundred units out, at the nearest place with
		// clear air down to the spot's height from which it can be seen, and walk the rest (a player does not hover over a roof)
		if (beside)
		{
			float spot[2];
			if (spotBeside(s, goal, cruise, spot))
			{
				logf("drive: FLYTO %s:beside: landing at %.0f %.0f, %.0f from it", id.c_str(), spot[0], spot[1], hypotf(spot[0] - goal[0], spot[1] - goal[1]));
				goal[0] = spot[0];
				goal[1] = spot[1];
			}
		}
		phase = climbs = hops = 0;
		outTimer = 0.0f;
		outTries = 0;
		inAir = false;
		phaseTimer = elapsed = progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		lastGoal = 1e9f;
		// seconds the leg needs at fly speed (up, over, down), a third more for detours, and a margin
		flyAllow = ((hypotf(goal[0] - p.feet[0], goal[1] - p.feet[1]) + 2.0f * fmaxf(0.0f, cruise - p.feet[2])) / fmaxf(1.0f, p.flySpeed)) * 1.35f + 30.0f;
		logf("drive: %s %s from %.0f %.0f %.0f (Acrobatics %d, Fortify Skill %.0f, Levitate %.0f)", verb.c_str(), id.c_str(), p.feet[0],
			p.feet[1], p.feet[2], s.w.stats.skills[20], s.w.effectTotal(83), p.levitate);
		return true;
	}
	if ((verb == "WALKTO" || verb == "KILL" || verb == "ACTIVATE" || verb == "PICKUP" || verb == "DOORTO" || verb == "LOOT"
		|| verb == "PUT" || verb == "STRIKE") && a.size() >= 2)
	{
		// a talk left open (an answer like "travel together" ends with the dialogue still up): a player says goodbye before
		// walking off, and every walk would time out under it
		if (s.dlg.open && s.screen == SCR_DIALOGUE && !retrying)
		{
			// (a guard's crime greeting is answered first, as a player would: pay the fine when there is gold for it)
			int pay = -1;
			for (size_t i = 0; i < s.dlg.choices.size() && pay < 0; i++)
				if (s.dlg.choices[i].second == 1)
					pay = (int)i;
			if (s.testLegit && s.w.bounty > 0 && pay >= 0 && s.w.itemCount("gold_001") >= s.w.bounty)
			{
				logf("drive: paying the fine (%d) of the crime greeting left open before %s %s", s.w.bounty, verb.c_str(), a[1].c_str());
				dialogueChoose(s.dlg, s.w, s, s.dlg.choices[pay].second);
			}
			if (s.dlg.open && s.screen == SCR_DIALOGUE)
			{
				logf("drive: closing a talk left open before %s %s", verb.c_str(), a[1].c_str());
				s.closeScreen();
			}
		}
		// PUT:<container>:<item>: open it as LOOT does, then put the carried item in (the Inventory page's tap)
		strikeFor = verb == "STRIKE" ? (a.size() >= 3 ? (float)atof(a[2].c_str()) : 5.0f) : 0.0f;
		lootItem = (verb == "LOOT" || verb == "PUT") && a.size() >= 3 ? lower(a[2]) : "";
		lootPut = verb == "PUT";
		id = a[1];
		kind = verb == "WALKTO" ? WALK : (verb == "KILL" || verb == "STRIKE") ? KILL : ACTIVATE;
		diveOk = verb == "WALKTO" && a.size() >= 3 && a[2] == "dive";
		pickup = verb == "PICKUP";
		if (verb != "WALKTO")
			retryToken = token;
		pointGoal = verb == "WALKTO" && a[1][0] == '@';
		if (pointGoal)
		{
			if (sscanf(a[1].c_str() + 1, "%f,%f,%f", &goal[0], &goal[1], &goal[2]) != 3)
			{
				fail("WALKTO:@x,y,z wants three numbers");
				return false;
			}
			ref = -1;
			path.clear();
			if (planPath(s, s.w.player.feet, goal, true, path))
				logf("drive: WALKTO %s, %d path points", id.c_str(), (int)path.size());
			else
				logf("drive: WALKTO %s, straight", id.c_str());
			memcpy(progressAt, s.w.player.feet, sizeof(progressAt));
			progressTimer = swingTimer = lookTimer = elapsed = repathTimer = 0.0f;
			stuckTries = 0;
			hoverOn = false;
			hoverTimer = 0.0f;
			descTimer = followWait = 0.0f;
			swimRoute.clear();
			floodTries = 0;
			descGaveUp = false;
			lastGoal = 1e9f;
			sidestepTimer = 0.0f;
			doorTried = false;
			doorLast = -1;
			lineTimer = 0.0f;
			pressA = false;
			return true;
		}
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
		if (planPath(s, s.w.player.feet, t, false, path))
			logf("drive: %s %s, %d path points", verb.c_str(), id.c_str(), (int)path.size());
		else
			logf("drive: %s %s, straight", verb.c_str(), id.c_str());
		memcpy(progressAt, s.w.player.feet, sizeof(progressAt));
		progressTimer = swingTimer = lookTimer = elapsed = repathTimer = 0.0f;
		stuckTries = 0;
		hoverOn = false;
		hoverTimer = 0.0f;
		descTimer = followWait = 0.0f;
			swimRoute.clear();
			floodTries = 0;
		descGaveUp = false;
		lastGoal = 1e9f;
		sidestepTimer = 0.0f;
		doorTried = false;
		doorLast = -1;
		lineTimer = 0.0f;
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
		int r = findRef(s, arg);
		return r >= 0 && s.w.active(r) ? r : -1;
	}
	std::string want = lower(spaced(arg));
	// DOORTO:<cell>@<n>: the n-th door going there in the ranking below (0 = the best, as without it; 1 = the next): for a cell
	// with several doors to one place that lead to different parts of it (Therana's Chamber: a closed vestibule, and the hall)
	int nth = 0;
	size_t at = want.rfind('@');
	if (at != std::string::npos && at + 1 < want.size() && isdigit((unsigned char)want[at + 1]))
	{
		nth = atoi(want.c_str() + at + 1);
		want.resize(at);
	}
	bool outside = want == "outside";
	// the door whose destination is exactly that cell first, else one that starts so; of those the one nearest by the
	// route a player would walk (the path grid), else in 3D (a door on another floor is not "near": a stair or a ceiling
	// between). Doors the grid can't reach from here come after the reachable ones.
	struct Cand { int i; bool exact; float d3; };
	std::vector<Cand> cands;
	for (size_t i = 0; i < s.w.refs.size(); i++)
	{
		const Ref& r = s.w.refs[i];
		if (!r.hasDest || !s.w.active((int)i) || !r.visible())
			continue;
		if (r.idLower == "prisonmarker")      // the jail's drop-off mark, no door anyone can use
			continue;
		bool match = outside ? r.destHasGrid || r.destCell.empty() : cellNameMatches(r.destCell, want);
		if (!match)
			continue;
		bool exact = !outside && lower(r.destCell) == want;
		float dx = r.pos[0] - s.w.player.feet[0], dy = r.pos[1] - s.w.player.feet[1], dz = r.pos[2] - s.w.player.feet[2];
		cands.push_back({(int)i, exact, dx * dx + dy * dy + dz * dz});
	}
	int found = -1;
	float best = 1e30f;
	bool bestExact = false;
	std::vector<std::pair<double, int>> ranked;
	for (const Cand& c : cands)
	{
		float d = c.d3;
		if (cands.size() > 1)
		{
			const Ref& r = s.w.refs[c.i];
			d = doorWalkRank(s, r, c.d3);
			// a locked door the player can't open (no key, lock still on) after the doors that open: stacked doors to one
			// place are often a locked upper door and the open one below
			if (r.lockLevel > 0 && (r.key.empty() || s.w.itemCount(r.key) <= 0))
				d += 1e13f;
			// the character generation doors are shut by their scripts until the intro is done: the ordinary doors first
			if (r.idLower.compare(0, 7, "chargen") == 0)
				d += 1e13f;
			logf("drive: DOORTO candidate %s (%d) at %.0f %.0f %.0f: 3D %.0f, ranked %.0f, lock %d%s", r.id.c_str(), c.i, r.pos[0], r.pos[1],
				r.pos[2], sqrtf(c.d3), sqrtf(d), r.lockLevel, r.trap.empty() ? "" : ", trapped");
		}
		ranked.push_back({ (c.exact ? 0.0 : 1e14) + d, c.i });
		if ((c.exact && !bestExact) || (c.exact == bestExact && d < best))
		{
			best = d;
			bestExact = c.exact;
			found = c.i;
		}
	}
	if (nth > 0 && !ranked.empty())
	{
		std::sort(ranked.begin(), ranked.end());
		found = ranked[std::min((size_t)nth, ranked.size() - 1)].second;
	}
	if (found >= 0)
		id = s.w.refs[found].id + " -> " + (s.w.refs[found].destCell.empty() ? "outside" : s.w.refs[found].destCell);
	return found;
}

// Beside a spot that a roof, an arch or a wall top hangs over: the nearest place whose column is clear down to the spot's
// height (nothing over it) and from which the spot can be seen at ground level (not behind a wall from it). A player
// drifts off the edge to it. fromZ is where the search looks down from.
static bool spotBeside(Session& s, const float aim[3], float fromZ, float out[2])
{
	Scene sc = s.w.scene();
	// (first with ground clear a body's width all round the spot, not on a ledge's lip; then without)
	for (int pass = 0; pass < 2; pass++)
	for (int k = 0; k < 48; k++)
	{
		float ang = k * 0.3927f, rr = 120.0f + 90.0f * (k / 16);
		float cx = aim[0] + sinf(ang) * rr, cy = aim[1] + cosf(ang) * rr, top = -1e9f;
		bool even = true;
		for (int q = 0; q < (pass == 0 ? 5 : 1) && even; q++)
		{
			static const float kAt[5][2] = { { 0, 0 }, { 50, 0 }, { -50, 0 }, { 0, 50 }, { 0, -50 } };
			float qt = -1e9f;
			for (Cell* c : sc.cells)
			{
				float z;
				if (collisionFloor(c->collision, cx + kAt[q][0], cy + kAt[q][1], fromZ + 50.0f, aim[2] - 300.0f, &z) && z > qt)
					qt = z;
			}
			if (q == 0)
				top = qt;
			even = qt > aim[2] - 200.0f && qt < aim[2] + 150.0f;     // (a door's z is its middle)
		}
		float from[3] = { cx, cy, top + 60.0f }, to[3] = { aim[0], aim[1], aim[2] + 60.0f };
		if (even && s.w.lineOfSight(from, to))
		{
			out[0] = cx;
			out[1] = cy;
			return true;
		}
	}
	return false;
}

// FLYTO: levitating, as a player flies: up (R) to the cruising height, straight at the spot, then down (L). A wall in
// the way: climb higher (three times at most). Levitation running out on the way: a fall, and the leg fails.
bool TestDriver::updateFly(Session& s, PlayerInput& in, float dt)
{
	Player& p = s.w.player;
	float dx = goal[0] - p.feet[0], dy = goal[1] - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	if (p.levitate <= 0.0f)
	{
		if (phase == 2 && p.onGround)
		{
			logf("drive: flew to %s in %.1f s (%.0f off)", id.c_str(), elapsed, dist);
			kind = NONE;
			return false;
		}
		fail("not levitating (or it ran out on the way)");
		return false;
	}
	p.pitch = 0.0f;
	if (elapsed <= dt)
	{
		float left = 0.0f;
		for (const ActiveEffect& e : s.w.effects)
			if (e.effect == 10)
				left = fmaxf(left, e.remaining);
		float need = (dist + fmaxf(0.0f, cruise - p.feet[2]) + fmaxf(0.0f, cruise - goal[2])) / fmaxf(1.0f, p.flySpeed);
		if (need > left)
		{
			char why[120];
			snprintf(why, sizeof(why), "levitation runs out first (%.0f s left, the flight takes %.0f s)", left, need);
			fail(why);
			return false;
		}
	}
	phaseTimer += dt;
	progressTimer += dt;
	if (phase == 0)
	{
		// up to the cruising height (or down to it: inside, a blocked flight tries lower under the roof before higher)
		bool lowering = lowFly && cruise < p.feet[2] - 20.0f;
		in.up = !lowering;
		in.down = lowering;
		// out from under an overhang (a bridge, an eave): a stretch sideways, a different way each time
		if (outTimer > 0.0f)
		{
			outTimer -= dt;
			p.yaw = atan2f(dx, dy) + outTries * 1.5708f;
			in.moveY = 1.0f;
		}
		if (lowering ? p.feet[2] <= cruise + 20.0f : p.feet[2] >= cruise)
		{
			logf("drive: FLYTO %s: at %.0f up after %.1f s, %.0f to go", id.c_str(), p.feet[2], elapsed, dist);
			phase = 1;
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		else if (lowering ? p.feet[2] <= progressAt[2] - 20.0f : p.feet[2] >= progressAt[2] + 20.0f)
		{
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		else if (progressTimer >= 3.0f && outTimer <= 0.0f)
		{
			// no rise at all in 3 s: a ceiling over us. Step out from under it (toward the goal, then the other ways round);
			// four ways tried without a rise is a roof the flight can't get out from under
			if (++outTries > 4)
			{
				fail("can't climb (a ceiling): fly out of the building first");
				return false;
			}
			logf("drive: FLYTO %s: no rise at %.0f %.0f %.0f, stepping out sideways (way %d)", id.c_str(), p.feet[0], p.feet[1], p.feet[2], outTries);
			outTimer = 4.0f;
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		return true;
	}
	if (phase == 1)
	{
		// straight at it, at height (the last stretch slows down so a frame's step doesn't overshoot)
		p.yaw = atan2f(dx, dy);
		float step = p.flySpeed * p.loadSpeed * dt;
		in.moveY = dist > step * 2.0f ? 1.0f : fmaxf(0.05f, dist / fmaxf(1.0f, step * 2.0f));
		if (dist < 40.0f)
		{
			logf("drive: FLYTO %s: over it after %.1f s, coming down from %.0f", id.c_str(), elapsed, p.feet[2]);
			phase = 2;
			phaseTimer = progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		if (progressTimer >= 2.0f)
		{
			bool closer = dist < lastGoal - 40.0f;
			lastGoal = dist;
			progressTimer = 0.0f;
			if (!closer)
			{
				if (++climbs > 3)
				{
					fail("blocked in the air, even higher up");
					return false;
				}
				// a wall or a tower: go over it; FLYTO ...:low (a cave), with the spot below, under it first (every other block, up)
				cruise = lowFly ? (goal[2] < p.feet[2] - 100.0f && climbs % 2 == 1 ? fmaxf(goal[2] + 80.0f, p.feet[2] - 250.0f) : p.feet[2] + 250.0f) : p.feet[2] + 800.0f;
				phase = 0;
				logf("drive: FLYTO %s: blocked at %.0f %.0f %.0f, climbing to %.0f", id.c_str(), p.feet[0], p.feet[1], p.feet[2],
					cruise);
			}
		}
		return true;
	}
	// down to the spot's height
	in.down = true;
	bool stopped = progressTimer >= 1.0f && p.feet[2] > progressAt[2] - 20.0f;     // (not coming down any more)
	if (stopped && p.feet[2] > goal[2] + 250.0f && climbs < 12)     // (within 250: the ground, uneven; higher: a roof)
	{
		// a roof, an arch or a wall top under us: come down beside it, as a player would drift off its edge
		climbs++;
		// beside it: the nearest spot whose column is clear down to the spot's height (no roof over it) and from which
		// the spot can be seen at ground level (not behind a wall from it)
		bool found = false;
		Scene sc = s.w.scene();
		float spot[2];
		if (spotBeside(s, aim, p.feet[2], spot))
		{
			goal[0] = spot[0];
			goal[1] = spot[1];
			found = true;
		}
		if (!found)
		{
			// a street under a big roof (the Vivec canton doors: a plaza 450 over them, stairs and a sloped roof between): the
			// roof is wider than the ring above. A player flies off its edge to where the street opens and walks in, so make
			// for the nearest path grid point of the street whose column is clear down to it (its open end)
			float bestD = 2000.0f * 2000.0f;
			for (const PathPoint& pp : s.w.pathPoints)
			{
				float ex = pp.pos[0] - aim[0], ey = pp.pos[1] - aim[1], d = ex * ex + ey * ey;
				if (d >= bestD || fabsf(pp.pos[2] - aim[2]) > 400.0f)
					continue;
				float top = -1e9f;
				for (Cell* c : sc.cells)
				{
					float z;
					if (collisionFloor(c->collision, pp.pos[0], pp.pos[1], p.feet[2] + 50.0f, pp.pos[2] - 300.0f, &z) && z > top)
						top = z;
				}
				if (top > pp.pos[2] - 150.0f && top < pp.pos[2] + 150.0f)
				{
					bestD = d;
					goal[0] = pp.pos[0];
					goal[1] = pp.pos[1];
					found = true;
				}
			}
		}
		if (!found)
		{
			// (a covered street: stop here, still levitating; the walk that follows comes down the way, L held)
			logf("drive: FLYTO %s: no clear spot to come down beside it: stopping at %.0f %.0f %.0f", id.c_str(), p.feet[0], p.feet[1],
				p.feet[2]);
			kind = NONE;
			return false;
		}
		cruise = p.feet[2];
		phase = 1;
		progressTimer = 0.0f;
		lastGoal = 1e9f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		logf("drive: FLYTO %s: something under us at %.0f (%.0f over the spot), trying beside it", id.c_str(), p.feet[2], p.feet[2] - goal[2]);
		return true;
	}
	if (p.feet[2] <= goal[2] + 5.0f || stopped)
	{
		logf("drive: flew to %s in %.1f s (%.0f off, %.0f up)", id.c_str(), elapsed, dist, p.feet[2] - goal[2]);
		kind = NONE;
		return false;
	}
	if (progressTimer >= 1.0f)
	{
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
	}
	return true;
}

// HOPTO: the speedrunners' way (Fortify Speed, Jump, run and jump): face the spot, run, jump; in the air steer toward
// it while the take-off would fall short, back against it while it would carry past. Landed within 200: done, else jump
// again (40 jumps at most).
bool TestDriver::updateHop(Session& s, PlayerInput& in, float dt)
{
	Player& p = s.w.player;
	float dx = goal[0] - p.feet[0], dy = goal[1] - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	p.pitch = 0.0f;
	p.yaw = atan2f(dx, dy);
	phaseTimer += dt;
	if (p.onGround || p.swimming)
	{
		if (dist < 200.0f && phaseTimer > 0.2f)
		{
			logf("drive: hopped to %s in %.1f s, %d jumps (%.0f off)", id.c_str(), elapsed, hops, dist);
			kind = NONE;
			return false;
		}
		if (hops >= 40)
		{
			fail("40 jumps and not there");
			return false;
		}
		// each landing: closer by 50 at least, else something is in the way (three times: give up, the route is wrong)
		if (inAir)
		{
			inAir = false;
			if (dist > lastGoal - 50.0f && ++climbs >= 3)
			{
				fail("blocked: three jumps without getting closer");
				return false;
			}
			lastGoal = fminf(lastGoal, dist);
		}
		// a short run-up, then the jump (a running take-off: 45 degrees, OpenMW), unless one jump would carry past it
		// (Icarian Flight's jump goes cells): then run the rest, as a player would
		in.moveY = 1.0f;
		// (in the air the pad can hold back airControl of the run speed for the whole flight, 2 v / g)
		float v = p.jumpSpeed * 0.707f, range = 2.0f * v * v / 627.0f;
		float brake = p.runSpeed * p.loadSpeed * p.airControl * 2.0f * v / 627.0f;
		// (running the rest: a run that stalls half a second, a rock in the way, gets a jump after all)
		progressTimer += dt;
		if (progressTimer >= 0.5f)
		{
			bool stalled = lastGoal - dist < 20.0f;
			lastGoal = dist;
			progressTimer = 0.0f;
			if (stalled && p.onGround)
			{
				logf("drive: hop %d toward %s: the run stalled, jumping (range %.0f, %.0f to go)", hops + 1, id.c_str(), range, dist);
				in.jump = true;
				hops++;
				phaseTimer = 0.0f;
				return true;
			}
		}
		if (range - brake > dist + 300.0f)
			return true;
		if (phaseTimer > 0.15f && p.onGround)
		{
			logf("drive: hop %d toward %s: jump speed %.0f (Acrobatics %d), range %.0f, %.0f to go", hops + 1, id.c_str(), p.jumpSpeed, s.w.stats.skills[20], range, dist);
			in.jump = true;
			hops++;
			phaseTimer = 0.0f;
		}
		return true;
	}
	inAir = true;
	// in the air: where would the take-off's own speed set us down? (gravity 627, from this height to the spot's)
	float h = fmaxf(0.0f, p.feet[2] - goal[2]);
	float tLeft = (p.vz + sqrtf(fmaxf(0.0f, p.vz * p.vz + 2.0f * 627.0f * h))) / 627.0f;
	float carry = sqrtf(p.inertia[0] * p.inertia[0] + p.inertia[1] * p.inertia[1]) * tLeft;
	in.moveY = carry > dist + 50.0f ? -1.0f : carry < dist - 50.0f ? 1.0f : 0.0f;
	return true;
}

bool TestDriver::update(Session& s, PlayerInput& in, u32& down, float dt)
{
	if (kind == NONE)
		return false;
	elapsed += dt;
	World& w = s.w;
	if (kind == FLY)
		return updateFly(s, in, dt);
	if (kind == HOP)
		return updateHop(s, in, dt);
	// A guard's crime greeting (pay the fine / go to jail / resist) that opens on its own during a step that is not
	// a talk with that guard: in a LEGIT run pay it if there is gold for it, close the dialogue and carry on
	if (s.testLegit && s.dlg.open && s.screen == SCR_DIALOGUE && s.dlg.ref >= 0 && s.dlg.ref != ref && s.dlg.ref < (int)w.refs.size()
		&& w.refs[s.dlg.ref].actor >= 0 && lower(w.game.actors[w.refs[s.dlg.ref].actor].cls) == "guard")
	{
		const std::string who = w.refs[s.dlg.ref].idLower;
		int pay = -1;
		for (size_t i = 0; i < s.dlg.choices.size() && pay < 0; i++)
			if (s.dlg.choices[i].second == 1)
				pay = (int)i;
		if (w.bounty > 0 && pay >= 0 && w.itemCount("gold_001") >= w.bounty)
		{
			logf("drive: %s's crime greeting during %s: paying the fine (%d)", who.c_str(), id.c_str(), w.bounty);
			dialogueChoose(s.dlg, w, s, s.dlg.choices[pay].second);
			return true;
		}
		if (s.dlg.choices.empty() && w.bounty <= 0)
		{
			logf("drive: closing %s's talk, back to %s", who.c_str(), id.c_str());
			s.closeScreen();
			return true;
		}
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
	if (!pointGoal && (ref < 0 || !w.active(ref)))
	{
		fail("gone from the loaded cells");
		return false;
	}
	// a spot to walk to stands in for a reference there (WALKTO:@x,y,z)
	static Ref spot;
	if (pointGoal)
	{
		memcpy(spot.pos, goal, sizeof(goal));
		memcpy(spot.boxMin, goal, sizeof(goal));
		memcpy(spot.boxMax, goal, sizeof(goal));
	}
	Ref& r = pointGoal ? spot : w.refs[ref];
	Player& p = w.player;
	const bool trappedDoor = r.type == "DOOR" && !r.trap.empty() && !r.disarmed;
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
	// paralysed: nothing moves, however long; said so, and not counted as being stuck
	if (w.effectTotal(45) > 0.0f)
	{
		if (paralysedLog <= 0.0f)
		{
			logf("drive: paralysed (%s %s)", kind == KILL ? "killing" : kind == WALK ? "walking to" : "going to", id.c_str());
			paralysedLog = 5.0f;
		}
		paralysedLog -= dt;
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	paralysedLog = 0.0f;
	if (kind == KILL && r.dead)
	{
		logf("drive: killed %s in %.1f s", id.c_str(), elapsed);
		kind = NONE;
		return false;
	}
	float tx = r.pos[0], ty = r.pos[1];
	// a door whose origin sits well outside its own mesh (an Ashlander yurt's: the model is placed 336 units from the
	// reference, inside the tent): walk to where the door shows, its box's middle (other doors keep their origin: a cave
	// door's box takes in the rock round it)
	if (kind == ACTIVATE && !pointGoal && r.type == "DOOR" && r.boxMax[0] > r.boxMin[0])
	{
		float ox = fmaxf(r.boxMin[0] - r.pos[0], fmaxf(0.0f, r.pos[0] - r.boxMax[0]));
		float oy = fmaxf(r.boxMin[1] - r.pos[1], fmaxf(0.0f, r.pos[1] - r.boxMax[1]));
		if (ox * ox + oy * oy > 100.0f * 100.0f)
		{
			tx = (r.boxMin[0] + r.boxMax[0]) * 0.5f;
			ty = (r.boxMin[1] + r.boxMax[1]) * 0.5f;
		}
	}
	float dx = tx - p.feet[0], dy = ty - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	float want = kind == WALK ? (pointGoal ? 30.0f : 120.0f) : kind == KILL ? 90.0f : 100.0f;
	// a big door (a Daedric ruin's oval one): its own mesh stops us at the face, a long way from the origin; at the box is there
	if (kind == ACTIVATE && !pointGoal && r.type == "DOOR" && r.hasBox && dist > want && dist < 300.0f)
	{
		float bx = fmaxf(r.boxMin[0] - p.feet[0], fmaxf(0.0f, p.feet[0] - r.boxMax[0]));
		float by = fmaxf(r.boxMin[1] - p.feet[1], fmaxf(0.0f, p.feet[1] - r.boxMax[1]));
		if (bx * bx + by * by < 40.0f * 40.0f)
			dist = want;
	}
	// a big statue (Mehrunes Dagon's in Ald Sotha: a box 570 wide and 700 high, its origin 440 above the floor): its mesh stops
	// us at its foot, far from the origin. Standing at the box, on a floor within its height, is at the statue
	bool atStatue = false;
	if (kind == ACTIVATE && !pointGoal && r.type == "ACTI" && r.hasBox && r.boxMax[0] - r.boxMin[0] > 400.0f && dist > want)
	{
		float bx = fmaxf(r.boxMin[0] - p.feet[0], fmaxf(0.0f, p.feet[0] - r.boxMax[0]));
		float by = fmaxf(r.boxMin[1] - p.feet[1], fmaxf(0.0f, p.feet[1] - r.boxMax[1]));
		if (bx * bx + by * by < 60.0f * 60.0f && p.feet[2] >= r.boxMin[2] - 80.0f && p.feet[2] <= r.boxMax[2])
		{
			dist = want;
			atStatue = true;
		}
	}
	// on another floor (ruins, towers): not there yet, however close it looks from above. Things to
	// activate count as reached within Morrowind's activation reach (iMaxActivateDist 192) in 3D.
	float dz = r.pos[2] - p.feet[2];
	if (atStatue)
		dz = 0.0f;
	// (a planned spot outdoors: its height is the land's, give or take a rock: there by the map, within 400 up or down)
	bool otherFloor = kind == ACTIVATE ? dist * dist + dz * dz > 190.0f * 190.0f && fabsf(dz) > 160.0f
		: fabsf(dz) > (pointGoal ? 400.0f : 160.0f);
	// a hatch in the ceiling (Sorkvild's, over the hall): its origin is a floor up, its box is within the crosshair's reach
	// of the eye from underneath it
	if (otherFloor && kind == ACTIVATE && !pointGoal && r.type == "DOOR" && r.hasBox && dz > 0.0f && dist < 60.0f)
	{
		float ez = p.feet[2] + PLAYER_EYE_HEIGHT - p.eyeDrop;
		float gap = fmaxf(0.0f, r.boxMin[2] - ez);
		otherFloor = gap > 150.0f;
	}
	// (levitating over it on a roof that holds us up: the descent at the door gave up, so the hover below brings us off its edge)
	bool heldAbove = kind == ACTIVATE && descGaveUp && p.levitate > 0.0f && !p.onGround && dz < -60.0f;
	if (dist > want || otherFloor || heldAbove)
	{
		// hovering right over it (a flight's potion not run out, the FLYTO ended above it): down (L) on the spot. The grid's
		// route would lead away to the floor it is on, off the plateau, and the walk out of it would time out
		if (p.levitate > 0.0f && !p.onGround && !pointGoal && dist <= 220.0f && dz < -60.0f && elapsed < 60.0f)
		{
			// a ledge or a roof under us holds us up: come down beside it, as the FLYTO does (the spot found after 1.5 s without
			// getting lower), else straight at it
			hoverTimer += dt;
			if (hoverTimer > 1.5f)
			{
				float at[3] = { tx, ty, r.pos[2] };
				if (!hoverOn && p.feet[2] > hoverZ - 10.0f && spotBeside(s, at, p.feet[2], hoverSpot))
				{
					hoverOn = true;
					logf("drive: over %s on something at %.0f: coming down beside it at %.0f %.0f", id.c_str(), p.feet[2], hoverSpot[0], hoverSpot[1]);
				}
				hoverZ = p.feet[2];
				hoverTimer = 0.0f;
			}
			float hx = (hoverOn ? hoverSpot[0] : tx) - p.feet[0], hy = (hoverOn ? hoverSpot[1] : ty) - p.feet[1];
			in.down = true;
			p.yaw = atan2f(hx, hy);
			p.pitch = 0.0f;
			in.moveY = hypotf(hx, hy) > (hoverOn ? 10.0f : 40.0f) ? 0.6f : 0.0f;
			path.clear();          // (planned from up here: replanned from the floor)
			repathTimer = 3.1f;
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		// an escort fallen behind: stand and let them catch up, as a player waits (not counted as being stuck)
		int e = -1;
		if ((kind == WALK || kind == ACTIVATE) && escortLagging(s, escort, 600.0f, &e))
		{
			if ((int)(elapsed / 5.0f) != (int)((elapsed - dt) / 5.0f))
				logf("drive: waiting for %s (%.0f away, at %.0f %.0f %.0f, ai %d)", escort.c_str(), w.distanceToPlayer(e), w.refs[e].pos[0],
					w.refs[e].pos[1], w.refs[e].pos[2], (int)w.refs[e].ai);
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		float floodGoal[3] = { pointGoal ? goal[0] : tx, pointGoal ? goal[1] : ty, pointGoal ? goal[2] : r.pos[2] };
		// a route by trial steps (a dive, a run of jumps): leg by leg
		if (!swimRoute.empty())
		{
			// each step as the trial made it: its heading held for its time (a jump on the first frame), then the next
			if (legTimer >= swimRoute.front().seconds)
			{
				swimRoute.erase(swimRoute.begin());
				legTimer = 0.0f;
				legJumped = false;
				if (swimRoute.empty())
				{
					logf("drive: route by trial steps ended at %.0f %.0f %.0f", p.feet[0], p.feet[1], p.feet[2]);
					stuckTries = 0;
					path.clear();
					repathTimer = 3.1f;
				}
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			legTimer += dt;
			p.yaw = swimRoute.front().yaw;
			p.pitch = swimRoute.front().pitch;
			in.moveY = 1.0f;
			if (swimRoute.front().jump && !legJumped && p.onGround)
			{
				in.jump = true;
				legJumped = true;
			}
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		// no route yet (the cells ahead were still loading): try again now and then
		repathTimer += dt;
		if (path.empty() && repathTimer > 3.0f)
		{
			repathTimer = 0.0f;
			float to[3];
			walkTarget(r, kind == ACTIVATE && !pointGoal, to);
			if (planPath(s, p.feet, to, pointGoal, path))
				logf("drive: route to %s found, %d path points", id.c_str(), (int)path.size());
		}
		// the next path point on the way (passed ones dropped), else straight at it
		float gx = tx, gy = ty, gz = r.pos[2];
		while (!path.empty())
		{
			const PathPoint& pp = w.pathPoints[path.front()];
			float px = pp.pos[0] - p.feet[0], py = pp.pos[1] - p.feet[1];
			if (px * px + py * py > 60.0f * 60.0f)
			{
				gx = pp.pos[0];
				gy = pp.pos[1];
				gz = pp.pos[2];
				break;
			}
			path.erase(path.begin());
		}
		p.yaw = atan2f(gx - p.feet[0], gy - p.feet[1]);
		p.pitch = 0.0f;
		in.moveY = 1.0f;
		// swimming: the stroke follows the view (a flooded tunnel, a sunken door): look down or up to the way's depth
		if (p.swimming && p.levitate <= 0.0f && fabsf(gz - p.feet[2]) > 60.0f)
		{
			float hd = hypotf(gx - p.feet[0], gy - p.feet[1]);
			p.pitch = fmaxf(-0.9f, fminf(0.9f, atan2f(gz - p.feet[2], fmaxf(hd, 1.0f))));
		}
		// still levitating (a flight's potion not run out): down (L) or up (R) to the way's height, as a player would
		if (p.levitate > 0.0f)
		{
			in.down = gz < p.feet[2] - 30.0f;
			in.up = gz > p.feet[2] + 30.0f;
			// the way is at or below us and we hang in the air: come down first (L), or the walk climbs onto roofs
			if (in.down && !p.onGround && elapsed < 15.0f && !standsOnFloor(s))
				in.moveY = 0.0f;
			// close to a door (a tower's or a temple's) and still hovering: land at its floor first, as a player does before the
			// last steps, or the walk stops short of it and the crosshair passes over its top (Ghostgate, Vivec)
			if (kind == ACTIVATE && !pointGoal && r.hasDest && dist < 500.0f && !p.onGround && p.feet[2] > r.pos[2] - 60.0f)
			{
				in.down = true;
				in.up = false;
				if (dist < 300.0f)
					in.moveY = 0.0f;
			}
		}
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
				// (each inner door once: a walk may have several in a row; one whose trap ate the press, again)
				if (door >= 0 && door != doorLast)
				{
					logf("drive: opening %s on the way to %s", w.refs[door].id.c_str(), id.c_str());
					bool trapped = !w.refs[door].trap.empty() && !w.refs[door].disarmed;
					s.playerActivate(door);
					if (trapped && w.refs[door].doorTarget == 0.0f)
					{
						logf("drive: the trap on %s used the press, again", w.refs[door].id.c_str());
						s.playerActivate(door);
					}
					doorLast = door;
					doorTried = true;
					progressTimer = 0.0f;
					memcpy(progressAt, p.feet, sizeof(progressAt));
					return true;
				}
				else if (s.testLegit && stuckTries == 2 && diveOk && floodTries < 3 && (floodTries++, true)
					&& floodRoute(s, floodGoal, pointGoal ? 80.0f : 150.0f, swimRoute))
				{
					logf("drive: stuck at %.0f %.0f %.0f going to %s: following a route of %d trial steps", p.feet[0], p.feet[1], p.feet[2], id.c_str(), (int)swimRoute.size());
					legTimer = 0.0f;
					legJumped = false;
					progressTimer = 0.0f;
					memcpy(progressAt, p.feet, sizeof(progressAt));
					return true;
				}
				else if (++stuckTries <= 2)
				{
					// step aside (left, then right) for a moment, as a player would around a post or a person
					sidestep = stuckTries == 1 ? -1.0f : 1.0f;
					sidestepTimer = 1.0f;
				}
				else if (stuckTries == 3)
				{
					in.jump = true;
					in.moveY = 0.0f;      // (from standing: a running jump is a 45 degree hop, half as high; OpenMW)
				}
				else if (s.testLegit && stuckTries <= 12)
				{
					// LEGIT (no warp to fall back on): keep working round it as a player would, wider each time:
					// left, right, a running jump, by turns (a shrub, a rock, a rail)
					int round = (stuckTries - 1) / 3, step = (stuckTries - 1) % 3;
					if (step == 2)
					{
						in.jump = true;
						in.moveY = 0.0f;
					}
					else
					{
						sidestep = step == 0 ? -1.0f : 1.0f;
						sidestepTimer = 1.0f + round * 0.8f;
					}
				}
				else if (s.testLegit)
				{
					// LEGIT: no warp past it (a player can't); the run stops being one a player could have played
					{
						// what could be holding a player: tired, slowed, burdened, or a screen up
						std::string fx;
						for (const ActiveEffect& e : w.effects)
							fx += " " + std::to_string(e.effect) + "(" + std::to_string((int)e.magnitude) + ")";
						logf("drive: stuck diagnostics: fatigue %.0f of %.0f, effects:%s, screen %d, swimming %d, levitate %.0f", w.stats.fatigue, w.stats.fatigueMax,
							fx.empty() ? " none" : fx.c_str(), (int)s.screen, (int)p.swimming, p.levitate);
						logf("drive: stuck diagnostics: target at %.0f %.0f %.0f, last pushed back by %s", r.pos[0], r.pos[1], r.pos[2], g_playerBlock);
						// who stands close (a person in the way of a doorway shows here)
						for (int i : w.loadedActors)
						{
							const Ref& a = w.refs[i];
							float ex = a.pos[0] - p.feet[0], ey = a.pos[1] - p.feet[1];
							if (a.visible() && !a.dead && ex * ex + ey * ey < 250.0f * 250.0f && fabsf(a.pos[2] - p.feet[2]) < 200.0f)
								logf("drive: stuck diagnostics: %s at %.0f %.0f %.0f, %.0f away, ai %d, package %d", a.id.c_str(), a.pos[0], a.pos[1], a.pos[2],
									hypotf(ex, ey), a.ai, a.aiPackage);
						}
					}
char why[160];
					snprintf(why, sizeof(why), "stuck at %.0f %.0f %.0f (%.0f away, %.0f up), no warp in a LEGIT run", p.feet[0], p.feet[1],
						p.feet[2], dist, dz);
					fail(why);
					return false;
				}
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
	// a door with the follower far behind: wait until they are near before going through (they come through behind us)
	if (kind == ACTIVATE && r.hasDest)
	{
		int e = -1;
		if (escortLagging(s, escort, 800.0f, &e))
		{
			if ((int)(elapsed / 5.0f) != (int)((elapsed - dt) / 5.0f))
				logf("drive: waiting for %s at the door (%.0f away)", escort.c_str(), w.distanceToPlayer(e));
			progressTimer = 0.0f;
			return true;
		}
		// a follower the case did not name (the Fighters Guild's Ulyne Henim, hired by a topic): left more than 800 units behind,
		// it stays in the cell (OpenMW's getFollowers), so a player waits for it too, up to 20 s
		if (escort.empty() && followWait < 20.0f)
			for (int i : w.loadedActors)
			{
				float d = w.distanceToPlayer(i);
				if (w.followsPlayer(i) && w.refs[i].aiPackage != AIPKG_ESCORT && !w.refs[i].dead && d > 600.0f && d < 20000.0f)
				{
					if ((int)(followWait / 5.0f) != (int)((followWait - dt) / 5.0f))
						logf("drive: waiting for follower %s at the door (%.0f away)", w.refs[i].id.c_str(), d);
					followWait += dt;
					progressTimer = 0.0f;
					return true;
				}
			}
	}
	// there, and the crosshair already on it (last frame's aim): press A with the aim left as it is (moving it now
	// would put the press on whatever the new aim finds)
	if (kind == ACTIVATE && s.target == ref && !s.menuOpen() && aimsAt(s, ref))
	{
		down |= KEY_A;
		logf("drive: activated %s (crosshair)", id.c_str());
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : (r.type == "NPC_" || r.type == "CREA") ? TALKWAIT : trappedDoor ? DOORWAIT : NONE;
		doorCell = w.current;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	// there: face it
	p.yaw = atan2f(dx, dy);
	if (kind == WALK)
	{
		logf("drive: reached %s in %.1f s", id.c_str(), elapsed);
		kind = NONE;
		if (!thenDo.empty())
		{
			std::string t = thenDo;
			thenDo.clear();
			retrying = true;
			start(s, t);
			retrying = false;
			return kind != NONE;
		}
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
		// someone else in the line (the crosshair is on a living person who is not the target): no swing, step aside
		// until the line is clear, as a player does not hit a bystander
		if (s.target >= 0 && s.target != ref && (w.refs[s.target].type == "NPC_" || w.refs[s.target].type == "CREA") && !w.refs[s.target].dead)
		{
			if (lineTimer <= 0.0f)
				logf("drive: %s is in the way to %s, stepping aside", w.refs[s.target].id.c_str(), id.c_str());
			lineTimer += dt;
			// (a short step either way does not clear a big one, a queen: after 3 s walk round it, facing the target, one way for
			// 6 s then the other, as a player goes round to another side)
			if (lineTimer < 3.0f)
				in.moveX = fmodf(lineTimer, 1.6f) < 0.8f ? -1.0f : 1.0f;
			else
				in.moveX = fmodf(lineTimer - 3.0f, 12.0f) < 6.0f ? -1.0f : 1.0f;
			return true;
		}
		lineTimer = 0.0f;
		// (a lockpick or probe in the weapon slot: the best carried weapon first, as a player would)
		std::string inHand = handItem(s, true);
		// wind up (attack held), then let go: the strike
		swingTimer += dt;
		if (swingTimer > 6.0f && swingTimer - dt <= 6.0f)
			logf("drive: %s: 6 s of swinging, in hand: %s (fatigue %.0f of %.0f)", id.c_str(), inHand.empty() ? "no weapon" : inHand.c_str(),
				w.stats.fatigue, w.stats.fatigueMax);
		in.attack = fmodf(swingTimer, 0.7f) < 0.4f;
		return true;
	}
	// ACTIVATE: look at its middle, then press A on what the crosshair finds. Something in front of the middle (a gem
	// lying on the desk, a basket before the book): look over the rest of it, as a player moves the crosshair
	float cz = (r.boxMin[2] + r.boxMax[2]) * 0.5f;
	float eyeZ = playerEyeZ(p);      // (sneaking lowers the eye: aim from where it is)
	p.pitch = atan2f(cz - eyeZ, fmaxf(dist, 1.0f));
	// still levitating (the flight's potion not run out) and hanging over it: down (L) to its floor first, as a player
	// does, or it is out of the crosshair's reach from up there (the Ghostgate plateau, right after the FLYTO)
	// (held up by something under us, no lower after 1.5 s: aim from here, the door stacked over another one at Balmora)
	if (p.levitate > 0.0f && !p.onGround && p.feet[2] > r.boxMin[2] + 40.0f && elapsed < 60.0f && !descGaveUp)
	{
		descTimer += dt;
		if (descTimer > 1.5f)
		{
			descGaveUp = p.feet[2] > descZ - 10.0f;
			descZ = p.feet[2];
			descTimer = 0.0f;
		}
		if (!descGaveUp)
		{
			in.down = true;
			lookTimer = 0.0f;
			return true;
		}
	}
	lookTimer += dt;
	if (lookTimer > 0.3f && !aimsAt(s, ref))
	{
		static const float kAim[][3] = { { 0.5f, 0.5f, 0.85f }, { 0.2f, 0.5f, 0.5f }, { 0.8f, 0.5f, 0.5f }, { 0.5f, 0.2f, 0.5f },
			{ 0.5f, 0.8f, 0.5f }, { 0.2f, 0.2f, 0.85f }, { 0.8f, 0.8f, 0.85f }, { 0.2f, 0.8f, 0.85f }, { 0.8f, 0.2f, 0.85f },
			{ 0.5f, 0.5f, 0.2f }, { 0.2f, 0.5f, 0.2f }, { 0.8f, 0.5f, 0.2f }, { 0.5f, 0.2f, 0.2f }, { 0.5f, 0.8f, 0.2f } };
		const float* f = kAim[(int)((lookTimer - 0.3f) / 0.08f) % 14];
		float ax = r.boxMin[0] + (r.boxMax[0] - r.boxMin[0]) * f[0], ay = r.boxMin[1] + (r.boxMax[1] - r.boxMin[1]) * f[1];
		float az = r.boxMin[2] + (r.boxMax[2] - r.boxMin[2]) * f[2];
		float hx = ax - p.feet[0], hy = ay - p.feet[1];
		p.yaw = atan2f(hx, hy);
		p.pitch = atan2f(az - eyeZ, fmaxf(sqrtf(hx * hx + hy * hy), 1.0f));
	}
	if (!s.menuOpen() && aimsAt(s, ref))
	{
		down |= KEY_A;
		logf("drive: activated %s (crosshair)", id.c_str());
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : (r.type == "NPC_" || r.type == "CREA") ? TALKWAIT : trappedDoor ? DOORWAIT : NONE;
		doorCell = w.current;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	if (lookTimer > 1.6f && s.testLegit && viewTries < 3)
	{
		// hidden from here: stand on another side of it (a quarter turn round each time) and look again
		viewTries++;
		float bx = (r.boxMin[0] + r.boxMax[0]) * 0.5f, by = (r.boxMin[1] + r.boxMax[1]) * 0.5f;
		float half = fmaxf(r.boxMax[0] - r.boxMin[0], r.boxMax[1] - r.boxMin[1]) * 0.5f;
		// a spot round it with floor at our height from which the crosshair (worldPick, as the game aims) finds it:
		// two rings, sixteen ways round, each looked at over the same points of it as the sweep above
		static const float kLook[][3] = { { 0.5f, 0.5f, 0.85f }, { 0.5f, 0.5f, 0.5f }, { 0.3f, 0.5f, 0.7f }, { 0.7f, 0.5f, 0.7f },
			{ 0.5f, 0.3f, 0.7f }, { 0.5f, 0.7f, 0.7f } };
		float base = atan2f(p.feet[0] - bx, p.feet[1] - by), sx = 0.0f, sy = 0.0f, sz = p.feet[2], reach = 192.0f + w.effectTotal(59) * 22.0f;
		bool found = false;
		Scene sc = w.scene();
		// first at our height; then at the door's own (we stand on a roof or a slope above it: the yurt's skirt at Mila-Nipal,
		// whose flap is at the foot of the tent), where the floor in front of it is
		for (int pass = 0; pass < 2 && !found; pass++)
		for (int k = 0; k < 32 && !found; k++)
		{
			float zTop = pass == 0 ? p.feet[2] + 60.0f : r.boxMax[2] + 60.0f, zBot = pass == 0 ? p.feet[2] - 60.0f : r.boxMin[2] - 120.0f;
			float ang = base + 0.3927f * (k % 16) * (viewTries == 2 ? -1.0f : 1.0f), rad = half + (k < 16 ? 70.0f : 130.0f);
			float cx = bx + sinf(ang) * rad, cy = by + cosf(ang) * rad, fz = -1e9f;
			for (Cell* c : sc.cells)
			{
				float z;
				if (collisionFloor(c->collision, cx, cy, zTop, zBot, &z) && z > fz)
					fz = z;
			}
			if (fz < -1e8f)
				continue;
			// (a spot right under our feet, on a roof, is not one to walk to)
			if (pass == 1 && fz < p.feet[2] - 60.0f && (cx - p.feet[0]) * (cx - p.feet[0]) + (cy - p.feet[1]) * (cy - p.feet[1]) < 60.0f * 60.0f)
				continue;
			float eye[3] = { cx, cy, fz + PLAYER_EYE_HEIGHT - p.eyeDrop };
			for (const float* f : kLook)
			{
				float d[3] = { r.boxMin[0] + (r.boxMax[0] - r.boxMin[0]) * f[0] - eye[0], r.boxMin[1] + (r.boxMax[1] - r.boxMin[1]) * f[1] - eye[1],
					r.boxMin[2] + (r.boxMax[2] - r.boxMin[2]) * f[2] - eye[2] };
				float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				if (len < 1.0f)
					continue;
				for (float& c : d)
					c /= len;
				if (worldPick(w, eye, d, reach) == ref)
				{
					sx = cx;
					sy = cy;
					sz = fz;
					found = true;
					break;
				}
			}
		}
		if (!found)
		{
			fail("hidden from here, and no spot round it where the crosshair finds it");
			return false;
		}
		char walk[96];
		snprintf(walk, sizeof(walk), "WALKTO:@%.0f,%.0f,%.0f", sx, sy, sz);
		logf("drive: %s hidden from here (the crosshair is on %s): trying from another side", id.c_str(),
			s.target >= 0 ? w.refs[s.target].id.c_str() : "nothing");
		std::string again = retryToken;
		retrying = true;
		start(s, walk);
		retrying = false;
		thenDo = again;
		return true;
	}
	if (lookTimer > 1.6f && s.testLegit)
	{
		std::string why = std::string("the crosshair is on ") + (s.target >= 0 ? w.refs[s.target].id : "nothing") + ", no direct activation in a LEGIT run";
		fail(why.c_str());
		return false;
	}
	if (lookTimer > 1.0f && !s.testLegit)
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
		ok = cellNameMatches(w.cellName(), spaced(a[1])) == (what == "cell");
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
		// EXPECT:journaltopic:<topic>: the topic open in the journal's Topics index ("none": the index itself, "any": some topic)
		ok = a[1] == "none" ? s.journalTopic.empty() : a[1] == "any" ? !s.journalTopic.empty() : lower(s.journalTopic) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", s.journalTopic.empty() ? "none" : s.journalTopic.c_str());
	}
	else if (what == "journalsearch" && a.size() >= 2)
	{
		// EXPECT:journalsearch:<text>: what the journal's search holds ("none": nothing, all shown)
		ok = a[1] == "none" ? s.journalSearch.empty() : lower(s.journalSearch) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", s.journalSearch.empty() ? "none" : s.journalSearch.c_str());
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
	else if ((what == "doorpick" || what == "doorpickclosed") && a.size() >= 2)
	{
		// EXPECT:doorpick:<door>: the crosshair, aimed from the player's eyes at the middle of the door as it is drawn now
		// (turned about its origin by its opening angle), picks that door; doorpickclosed: aimed at where it stood closed,
		// it does not (an open door is activated where it is, not where it was)
		int ri = findRef(s, a[1], false);
		if (ri < 0)
			return false;
		const Ref& r = w.refs[ri];
		float mid[3], to[3];
		for (int k = 0; k < 3; k++)
			mid[k] = (r.boxMin[k] + r.boxMax[k]) * 0.5f - r.pos[k];
		if (what == "doorpick")
			doorSwing(r, -r.doorAngle, mid, mid);       // (drawn turned by -doorAngle about its own up: renderer.cpp)
		for (int k = 0; k < 3; k++)
			to[k] = r.pos[k] + mid[k];
		float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
		float d[3] = { to[0] - eye[0], to[1] - eye[1], to[2] - eye[2] }, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int k = 0; k < 3; k++)
			d[k] /= len > 0.0f ? len : 1.0f;
		int hit = worldPick(w, eye, d, 1000.0f);
		ok = what == "doorpick" ? hit == ri : hit != ri;
		snprintf(got, sizeof(got), "door angle %.2f, picked %s", r.doorAngle, hit < 0 ? "nothing" : hit == ri ? "the door" : w.refs[hit].id.c_str());
	}
	else if (what == "bodygroup" && a.size() >= 2)
	{
		// EXPECT:bodygroup:<group>: in third person, the player's body plays a group whose name starts with it
		// ("Attack1h" also takes its follow-throughs "Attack1hM" / "Attack1hS")
		const char* g = s.thirdPerson ? s.body.playing() : "";
		ok = g[0] && strncmp(g, a[1].c_str(), a[1].size()) == 0;
		snprintf(got, sizeof(got), "%s", !s.thirdPerson ? "first person" : g[0] ? g : "no group");
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
