// Script functions: stats, spells, factions, crime, disposition, diseases
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

bool statFunction(Ctx& c, const Node& n, bool onPlayer, Ref* r, int ref, float* out)
{
	World& w = c.w;
	const std::string& name = n.b;
	int op = name.compare(0, 10, "modcurrent") == 0 ? 3 : name.compare(0, 3, "get") == 0 ? 0
		: name.compare(0, 3, "set") == 0 ? 1 : name.compare(0, 3, "mod") == 0 ? 2 : -1;
	if (op < 0)
		return false;
	std::string stat = name.substr(op == 3 ? 10 : 3);
	bool player = onPlayer || !r;
	PlayerStats& s = w.stats;
	float arg = op == 0 ? 0.0f : argNum(c, n, 0);
	// ModPCCrimeLevel: the bounty never goes below nothing
	if (stat == "pccrimelevel" && op == 2)
	{
		w.bounty = std::max(0, w.bounty + (int)arg);
		w.globals["pccrimelevel"] = (float)w.bounty;
		*out = 0.0f;
		return true;
	}
	// GetLevel / SetLevel: the player's, or an actor's (the record's until a script sets it; -1 for objects)
	if (stat == "level" && op <= 1)
	{
		if (player)
		{
			if (op == 1)
				s.level = (int)arg;
			*out = (float)s.level;
			return true;
		}
		if (r->actor < 0)
		{
			*out = -1.0f;
			return true;
		}
		ActorDef& a = w.game.actors[r->actor];
		if (op == 1)
			r->level = a.level = (int)arg;      // the record too: dialogue conditions read it there
		*out = (float)a.level;
		return true;
	}
	// GetHealthGetRatio and its kin: current over maximum
	bool ratio = stat.size() > 8 && stat.compare(stat.size() - 8, 8, "getratio") == 0;
	if (ratio)
	{
		if (op != 0)
			return false;
		stat.resize(stat.size() - 8);
	}
	// Health / magicka / fatigue: Set / Mod change the maximum, ModCurrent the current value
	int dyn = stat == "health" ? 0 : stat == "magicka" ? 1 : stat == "fatigue" ? 2 : -1;
	if (dyn >= 0)
	{
		if (!player && r->actor < 0)
		{
			*out = 0.0f;
			return true;
		}
		float* cur = player ? (dyn == 0 ? &s.health : dyn == 1 ? &s.magicka : &s.fatigue)
			: (dyn == 0 ? &r->health : dyn == 1 ? &r->magicka : &r->fatigue);
		float* max = player ? (dyn == 0 ? &s.healthMax : dyn == 1 ? &s.magickaMax : &s.fatigueMax)
			: (dyn == 0 ? &r->healthMax : dyn == 1 ? &r->magickaMax : &r->fatigueMax);
		if (ratio)
		{
			*out = *max > 0.0f ? *cur / *max : 1.0f;
			return true;
		}
		if (op == 0)
		{
			*out = dyn == 1 && *cur < 0.0f ? 0.0f : *cur;       // magicka never reads below nothing
			return true;
		}
		if (op == 3)
		{
			// Up to the maximum and down to nothing; fatigue may go below, and at nothing they fall down
			float v = *cur + arg;
			if (v > *cur)
			{
				if (v <= *max)
					*cur = v;
				else if (*cur <= *max)
					*cur = *max;
			}
			else if (v > 0.0f || dyn == 2)
				*cur = v;
			else if (*cur > 0.0f)
				*cur = 0.0f;
			if (dyn == 2 && v <= 0.0f && !player && !r->dead)
				c.host.knockDownActor(ref);
		}
		else if (op == 1)
		{
			if (player && dyn == 0)
				s.healthBonus += arg - *max;
			*max = *cur = arg;
		}
		else
		{
			// The maximum moves by the amount (fatigue alone may go below nothing), and so does the current value
			float target = *max + arg;
			if (dyn != 2)
				target = fmaxf(target, 0.0f);
			if (player && dyn == 0)
				s.healthBonus += target - *max;
			*max = target;
			*cur += arg;
		}
		*out = *cur;
		if (dyn == 0 && *cur <= 0.0f && !player && r->actor >= 0 && !r->dead)
			c.host.killActor(ref);
		return true;
	}
	if (ratio || op == 3)
		return false;
	int attr = -1, skill = -1;
	for (int i = 0; i < 8; i++)
		if (stat == kAttrFuncs[i])
			attr = i;
	for (int i = 0; i < 27; i++)
		if (stat == kSkillFuncs[i])
			skill = i;
	if (attr < 0 && skill < 0)
		return false;
	if (!player)
	{
		// NPCs and creatures: the record's value plus what scripts changed (skills only for NPCs)
		if (r->actor < 0)
		{
			*out = 0.0f;
			return true;
		}
		const ActorDef& a = w.game.actors[r->actor];
		float base = (float)(attr >= 0 ? a.attributes[attr] : a.skills[skill]);
		int slot = attr >= 0 ? attr : 8 + skill;
		float value = fmaxf(0.0f, base + (r->statDelta.empty() ? 0.0f : r->statDelta[slot]));
		if (op != 0 && (attr >= 0 || !a.creature))
		{
			if (r->statDelta.empty())
				r->statDelta.assign(35, 0.0f);
			float& delta = r->statDelta[slot];
			// Set takes any value; Mod stays between 0 and 100
			if (op == 1)
				value = fmaxf(0.0f, arg);
			else
				value = fmaxf(0.0f, value + (arg > 0.0f ? fminf(arg, fmaxf(0.0f, 100.0f - value)) : arg));
			delta = value - base;
		}
		*out = value;
		return true;
	}
	int value = attr >= 0 ? s.attributes[attr] : s.skills[skill];
	if (op != 0)
	{
		// Mod stays between 0 and 100
		int delta = (int)arg;
		if (op == 1)
			delta = (int)arg - value;
		else if (delta > 0)
			delta = std::min(delta, std::max(0, 100 - value));
		else if (value + delta < 0)
			delta = -value;
		(attr >= 0 ? s.attrBonus[attr] : s.skillBonus[skill]) += delta;
		float health = s.health, magicka = s.magicka, fatigue = s.fatigue;
		w.recomputeStats();
		s.health = fminf(s.healthMax, health);
		s.magicka = fminf(s.magickaMax, magicka);
		s.fatigue = fminf(s.fatigueMax, fatigue);
		value = attr >= 0 ? s.attributes[attr] : s.skills[skill];
	}
	*out = (float)value;
	return true;
}

std::optional<float> statsExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// An object's health is the item's condition (0 for things that don't wear)
	case F_GETHEALTH:
		return onPlayer || !r ? w.stats.health : r->actor >= 0 ? r->health : r->obj ? (float)r->obj->health : 0.0f;
	case F_ONDEATH:
		if (r && r->died)
		{
			r->died = false;
			return 1.0f;
		}
		return 0.0f;
	case F_ONMURDER:
		if (r && r->murdered)
		{
			r->murdered = false;
			return 1.0f;
		}
		return 0.0f;
	case F_ONKNOCKOUT:
		if (r && r->knockedOut)
		{
			r->knockedOut = false;
			return 1.0f;
		}
		return 0.0f;
	case F_RESURRECT:
		if (onPlayer && w.stats.health <= 0.0f)
			w.stats.health = w.stats.healthMax;
		// A dead actor comes back as new: what scripts changed on it is gone
		if (r && r->actor >= 0 && r->dead)
		{
			r->dead = r->died = false;
			r->statDelta.clear();
			r->spells.clear();
			r->noSpells.clear();
			r->health = r->healthMax;
			r->ai = AI_IDLE;
			if (r->anim >= 0)
				w.syncActor(ref);
		}
		return 0.0f;
	// Lava and the like: hurts whoever stands on it, the player and the living actors (health per second; a
	// negative one heals). Nothing while a menu is up
	case F_HURTSTANDINGACTOR:
	{
		if (!r || !r->hasBox || c.host.menuMode())
			return 0.0f;
		// The lava's walk-on collision rises up to 64 above its flat picture (Arkngthand's in_lava_1024): standing
		// from just under the picture to that high counts as standing on it
		float rate = argNum(c, n, 0), top = r->boxMax[2];
		auto over = [&](const float* f) {
			return f[0] >= r->boxMin[0] && f[0] <= r->boxMax[0] && f[1] >= r->boxMin[1] && f[1] <= r->boxMax[1]
				&& f[2] > top - 20.0f && f[2] < top + 70.0f;
		};
		if (over(w.player.feet) && w.player.onGround)
		{
			if (rate > 0.0f)
				c.host.hurtPlayer(rate * c.dt);
			else
				w.stats.health = fminf(w.stats.healthMax, w.stats.health - rate * c.dt);
		}
		w.forLoadedActors([&](int i) {
			Ref& a = w.refs[i];
			if (i == ref || a.dead || !a.visible() || a.actor < 0 || !over(a.pos))
				return;
			a.health = fminf(a.healthMax > 0.0f ? a.healthMax : a.health, a.health - rate * c.dt);
			if (a.health <= 0.0f)
				c.host.killActor(i);
		});
		return 0.0f;
	}
	// An NPC's own rank in its faction (not the player's: that does nothing). Raising stops at the top rank,
	// lowering at the bottom one. The record changes too: dialogue reads the rank there
	case F_RAISERANK: case F_LOWERRANK:
	{
		if (onPlayer || !r || r->actor < 0)
			return 0.0f;
		ActorDef& a = w.game.actors[r->actor];
		auto fit = w.game.factions.find(lower(a.faction));
		if (a.creature || a.faction.empty() || fit == w.game.factions.end())
			return 0.0f;
		int top = std::max(0, std::min(9, (int)fit->second.ranks.size() - 1));
		int next = n.func == F_RAISERANK ? std::min(a.rank + 1, top) : std::max(a.rank - 1, 0);
		r->rank = a.rank = next;
		return 0.0f;
	}
	case F_GETCOMMONDISEASE: case F_GETBLIGHTDISEASE:
	{
		int type = n.func == F_GETCOMMONDISEASE ? 3 : 2;
		if (onPlayer || !r)
			return w.pcHasDisease(type) ? 1.0f : 0.0f;
		return r->actor >= 0 && w.actorHasSpellType(*r, type) ? 1.0f : 0.0f;
	}
	case F_GETDEADCOUNT:
	{
		auto dead = w.deadCounts.find(lower(argStr(c, n, 0)));
		return dead != w.deadCounts.end() ? (float)dead->second : 0.0f;
	}
	// Whose race: the player's, or an NPC's (creatures have none)
	case F_GETRACE:
	{
		std::string race = lower(argStr(c, n, 0));
		if (onPlayer || !r)
			return race == lower(w.stats.race) ? 1.0f : 0.0f;
		const ActorDef* a = r->actor >= 0 ? &w.game.actors[r->actor] : nullptr;
		return a && !a->creature && race == lower(a->race) ? 1.0f : 0.0f;
	}
	// Disposition belongs to NPCs: creatures have none (reads 0, changes are ignored)
	case F_MODDISPOSITION: case F_SETDISPOSITION: case F_GETDISPOSITION:
	{
		if (!r || r->actor < 0 || w.game.actors[r->actor].creature)
			return 0.0f;
		if (n.func == F_MODDISPOSITION)
			r->disposition += (int)argNum(c, n, 0);
		else if (n.func == F_SETDISPOSITION)
			r->disposition = (int)argNum(c, n, 0);
		else
			return (float)w.disposition(ref);
		return 0.0f;
	}
	case F_ADDSPELL:
		if (!onPlayer && r && r->actor >= 0)
		{
			std::string id = lower(argStr(c, n, 0));
			r->noSpells.erase(std::remove(r->noSpells.begin(), r->noSpells.end(), id), r->noSpells.end());
			if (!w.actorHasSpell(*r, id))
				r->spells.push_back(id);
		}
		if (onPlayer)
		{
			std::string id = lower(argStr(c, n, 0));
			bool have = false;
			for (auto& s : w.stats.spells)
				have |= s == id;
			if (!have)
			{
				w.stats.spells.push_back(id);
				float hp = w.stats.health, mp = w.stats.magicka, fp = w.stats.fatigue;
				w.recomputeStats();
				w.stats.health = fminf(w.stats.healthMax, hp);
				w.stats.magicka = fminf(w.stats.magickaMax, mp);
				w.stats.fatigue = fminf(w.stats.fatigueMax, fp);
			}
		}
		return 0.0f;
	case F_REMOVESPELL:
		if (!onPlayer && r && r->actor >= 0)
		{
			std::string id = lower(argStr(c, n, 0));
			bool had = w.actorHasSpell(*r, id);
			r->spells.erase(std::remove(r->spells.begin(), r->spells.end(), id), r->spells.end());
			if (had && w.actorHasSpell(*r, id))
				r->noSpells.push_back(id);          // theirs from the record: marked as taken away
		}
		if (onPlayer)
		{
			std::string id = lower(argStr(c, n, 0));
			for (size_t i = 0; i < w.stats.spells.size(); i++)
				if (w.stats.spells[i] == id)
					w.stats.spells.erase(w.stats.spells.begin() + i--);
			float hp = w.stats.health, mp = w.stats.magicka, fp = w.stats.fatigue;
			w.recomputeStats();
			w.stats.health = fminf(w.stats.healthMax, hp);
			w.stats.magicka = fminf(w.stats.magickaMax, mp);
			w.stats.fatigue = fminf(w.stats.fatigueMax, fp);
		}
		return 0.0f;
	case F_GETSPELL:
	{
		std::string id = lower(argStr(c, n, 0));
		if (!onPlayer && r)
			return r->actor >= 0 && w.actorHasSpell(*r, id) ? 1.0f : 0.0f;
		for (auto& s : w.stats.spells)
			if (s == id)
				return 1.0f;
		return 0.0f;
	}
	case F_GETPCCRIMELEVEL: return (float)w.bounty;
	case F_SETPCCRIMELEVEL:
		w.bounty = (int)argNum(c, n, 0);
		w.globals["pccrimelevel"] = (float)w.bounty;
		return 0.0f;
	// PayFine: the bounty goes, the stolen goods are taken back and the weapon is put away; PayFineThief only clears
	// the bounty
	case F_PAYFINE: case F_PAYFINETHIEF:
		w.bounty = 0;
		w.globals["pccrimelevel"] = 0.0f;
		if (n.func == F_PAYFINE)
		{
			w.confiscateStolen();
			c.host.sheathe();
		}
		return 0.0f;
	// Factions: the faction argument, or the faction of whoever is speaking / running the script
	case F_PCJOINFACTION: case F_PCRAISERANK: case F_PCLOWERRANK: case F_GETPCRANK: case F_PCEXPELLED:
	case F_MODPCFACREP: case F_GETPCFACREP: case F_SETPCFACREP: case F_PCEXPELL: case F_PCCLEAREXPELLED:
	{
		bool repFunc = n.func == F_MODPCFACREP || n.func == F_SETPCFACREP;
		std::string f = lower(argStr(c, n, repFunc ? 1 : 0));
		if (f.empty() || (f[0] >= '0' && f[0] <= '9') || f[0] == '-')
		{
			const Ref* speaker = r ? r : (c.self >= 0 ? &w.refs[c.self] : nullptr);
			f = speaker && speaker->actor >= 0 ? lower(w.game.actors[speaker->actor].faction) : "";
		}
		if (f.empty())
			return n.func == F_GETPCRANK ? -1.0f : 0.0f;
		int rank = w.pcRankIn(f);
		auto fit = w.game.factions.find(f);
		int top = fit != w.game.factions.end() ? (int)fit->second.ranks.size() - 1 : 9;
		switch (n.func)
		{
		case F_PCJOINFACTION:
			if (rank < 0)
				w.pcRank[f] = 0;
			logf("faction: joined %s", f.c_str());
			return 0.0f;
		case F_PCRAISERANK:
			w.pcRank[f] = rank < 0 ? 0 : std::min(rank + 1, std::max(top, 0));
			logf("faction: %s rank %d", f.c_str(), w.pcRank[f]);
			return 0.0f;
		case F_PCLOWERRANK:
			// Below the lowest rank they leave the faction (and are no longer expelled from it)
			if (rank > 0)
				w.pcRank[f] = rank - 1;
			else if (rank == 0)
			{
				w.pcRank.erase(f);
				w.pcExpelled.erase(f);
			}
			return 0.0f;
		case F_GETPCRANK: return (float)rank;
		case F_PCEXPELLED: return w.pcExpelled.count(f) ? 1.0f : 0.0f;
		case F_PCEXPELL:
			if (w.pcExpelled.insert(f).second)      // told once, the first time
				c.host.notify(w.game.gmst("sexpelledmessage", "You have been expelled from the ") + w.game.factionName(f));
			return 0.0f;
		case F_PCCLEAREXPELLED: w.pcExpelled.erase(f); return 0.0f;
		case F_MODPCFACREP: w.pcFacRep[f] += (int)argNum(c, n, 0); return 0.0f;
		case F_SETPCFACREP: w.pcFacRep[f] = (int)argNum(c, n, 0); return 0.0f;
		case F_GETPCFACREP: return (float)w.pcFacRep[f];
		}
		return 0.0f;
	}
	// How one faction feels about another: kept (dialogue disposition doesn't use it yet)
	case F_MODFACTIONREACTION: case F_SETFACTIONREACTION: case F_GETFACTIONREACTION:
	{
		// the map holds scripts' changes on top of the factions' own reactions
		std::string a = lower(argStr(c, n, 0)), b = lower(argStr(c, n, 1));
		int& v = w.factionReactions[a + "|" + b];
		if (n.func == F_MODFACTIONREACTION)
			v += (int)argNum(c, n, 2);
		else if (n.func == F_SETFACTIONREACTION)
			v += (int)argNum(c, n, 2) - w.factionReaction(a, b);
		return (float)w.factionReaction(a, b);
	}
	default:
		return std::nullopt;
	}
}
