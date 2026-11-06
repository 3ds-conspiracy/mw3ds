// Spells chosen for actors and the player by their skills (autocalc)
#include "mwworld/esmstore.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <strings.h>


// School of each magic skill: alteration, conjuration, destruction, illusion, mysticism, restoration
static const int kSchoolSkillIdx[6] = { 11, 13, 10, 12, 14, 15 };

// What autocalc needs of each candidate spell, worked out once (the lookups and costs don't depend
// on who casts): per effect its school's skill and its cost term (calcWeakestSchool's x); skill / attribute: the one
// it works on, -1 unless the effect is one on a skill / an attribute (OpenMW's TargetSkill / TargetAttribute flags)
struct CalcEffect { int school, skill, attribute; float x; };

// The effects on a skill and on an attribute (Drain, Damage, Restore, Fortify, Absorb): OpenMW's hard-coded
// TargetSkill and TargetAttribute flags (components/esm3/loadmgef.cpp HardcodedFlags)
static bool targetsSkill(int effect) { return effect == 21 || effect == 26 || effect == 78 || effect == 83 || effect == 89; }
static bool targetsAttribute(int effect) { return effect == 17 || effect == 22 || effect == 74 || effect == 79 || effect == 85; }
struct CalcSpell { const SpellDef* sp; std::vector<CalcEffect> effects; };

u32 g_gameGeneration = 1;

static const std::vector<CalcSpell>& calcSpells(const GameData& g, bool player)
{
	static std::vector<CalcSpell> lists[2];
	static bool built[2] = { false, false };
	// another session's data (a save loaded): the old lists point into freed spells. The generation, not the
	// address: a new GameData can be allocated right where the freed one was
	static u32 builtGen = 0;
	if (builtGen != g_gameGeneration)
	{
		builtGen = g_gameGeneration;
		lists[0].clear();
		lists[1].clear();
		built[0] = built[1] = false;
	}
	std::vector<CalcSpell>& out = lists[player ? 1 : 0];
	if (built[player ? 1 : 0])
		return out;
	built[player ? 1 : 0] = true;
	float costMult = g.gmstf("feffectcostmult", 0.5f);
	for (auto& kv : g.spells)
	{
		const SpellDef& sp = kv.second;
		if (sp.type != 0 || !(sp.flags & (player ? 2 : 1)))
			continue;
		CalcSpell cs;
		cs.sp = &sp;
		for (auto& e : sp.effects)
		{
			auto me = g.magicEffects.find(e.effect);
			if (me == g.magicEffects.end())
				continue;
			const MagicEffectDef& d = me->second;
			int mn = 1, mx = 1;
			if (!(d.flags & 0x8)) { mn = e.min; mx = e.max; }
			// (no duration: 0; at least 1 unless the effect is applied once, 0x1000; area at least 1)
			int dur = (d.flags & 0x4) ? 0 : e.duration;
			if (!(d.flags & 0x1000))
				dur = dur < 1 ? 1 : dur;
			float x = 0.5f * ((mn > 1 ? mn : 1) + (mx > 1 ? mx : 1)) * 0.1f * d.cost * (1 + dur)
				+ 0.05f * (e.area > 1 ? e.area : 1) * d.cost;
			x *= costMult;
			if (e.range == 2)
				x *= 1.5f;
			int school = d.school >= 0 && d.school < 6 ? d.school : 0;
			cs.effects.push_back({ school, targetsSkill(e.effect) ? e.skill : -1,
				targetsAttribute(e.effect) ? e.attribute : -1, x });
		}
		// (a spell with no effect has no school: never picked)
		if (!cs.effects.empty())
			out.push_back(cs);
	}
	// The order of the spell records in the data files: who is picked when a school is full depends on it
	// (OpenMW's note: "the algorithm heavily depends on the traversal order of the spells")
	std::sort(out.begin(), out.end(), [](const CalcSpell& a, const CalcSpell& b) {
		return a.sp->order != b.sp->order ? a.sp->order < b.sp->order : a.sp->id < b.sp->id;
	});
	return out;
}

// The school a spell is hardest in for these skills, and 2 x that skill (calcWeakestSchool)
static int weakestSchool(const CalcSpell& cs, const int* skills, float* skillTerm)
{
	float minChance = 1e9f;
	int school = -1;
	*skillTerm = 0.0f;
	for (auto& e : cs.effects)
	{
		float s = 2.0f * skills[kSchoolSkillIdx[e.school]];
		if (s - e.x < minChance)
		{
			minChance = s - e.x;
			school = e.school;
			*skillTerm = s;
		}
	}
	return school;
}

std::vector<std::string> GameData::autoCalcSpells(const int* attributes, const int* skills, bool player,
	const RaceDef* race) const
{
	static float magickaMult[2], chanceNeeded[2];
	static int timesCanCast, attSkillMin, capLimit[6], pcLimit;
	static bool init = false;
	if (!init)
	{
		init = true;
		magickaMult[0] = gmstf("fnpcbasemagickamult", 2.0f);
		magickaMult[1] = gmstf("fpcbasemagickamult", 1.0f);
		chanceNeeded[0] = gmstf("fautospellchance", 80.0f);
		chanceNeeded[1] = gmstf("fautopcspellchance", 50.0f);
		timesCanCast = (int)gmstf("iautospelltimescancast", 3.0f);
		attSkillMin = (int)gmstf("iautospellattskillmin", 70.0f);
		static const char* capNames[6] = { "iautospellalterationmax", "iautospellconjurationmax", "iautospelldestructionmax",
			"iautospellillusionmax", "iautospellmysticismmax", "iautospellrestorationmax" };
		for (int k = 0; k < 6; k++)
			capLimit[k] = (int)gmstf(capNames[k], 5.0f);
		pcLimit = (int)gmstf("iautopcspellmax", 100.0f);
	}
	int pi = player ? 1 : 0;
	float magicka = magickaMult[pi] * attributes[1];
	struct Cap { int count = 0, limit = 0, minCost = 1 << 30; const CalcSpell* weakest = nullptr; bool full = false; } caps[6], pc;
	for (int k = 0; k < 6; k++)
	{
		caps[k].limit = capLimit[k];
		caps[k].full = capLimit[k] <= 0;
	}
	pc.limit = pcLimit;
	auto racesOwn = [&](const std::string& id) {
		if (!race)
			return false;
		for (auto& r : race->spells)
			if (strcasecmp(r.c_str(), id.c_str()) == 0)
				return true;
		return false;
	};
	std::vector<const CalcSpell*> picked;
	for (const CalcSpell& cs : calcSpells(*this, player))
	{
		const SpellDef* sp = cs.sp;
		float skillTerm;
		int school = weakestSchool(cs, skills, &skillTerm);
		Cap& cap = player ? pc : caps[school >= 0 ? school : 0];
		if (cap.full && sp->cost <= cap.minCost)
			continue;
		if (magicka < (player ? 1 : timesCanCast) * sp->cost)
			continue;
		if (racesOwn(sp->id))
			continue;
		bool ok = true;
		for (auto& e : cs.effects)
			if ((e.skill >= 0 && e.skill < 27 && skills[e.skill] < attSkillMin)
				|| (e.attribute >= 0 && e.attribute < 8 && attributes[e.attribute] < attSkillMin))
				ok = false;
		if (!ok)
			continue;
		float chance = (sp->flags & 4) ? 100.0f : skillTerm - sp->cost + 0.2f * attributes[2] + 0.1f * attributes[7];
		if (chance < chanceNeeded[pi])
			continue;
		picked.push_back(&cs);
		if (cap.full)
		{
			// the school's cheapest goes, and the next cheapest is the one to beat. Morrowind's bug, kept as OpenMW
			// keeps it: that next cheapest is looked for among all the picks, of any school, so two schools can name the
			// same spell and one of them then removes nothing (an NPC ends with more than the schools' limits)
			auto gone = std::find(picked.begin(), picked.end(), cap.weakest);
			if (gone != picked.end())
				picked.erase(gone);
			cap.minCost = 1 << 30;
			for (const CalcSpell* p : picked)
				if (p->sp->cost < cap.minCost)
				{
					cap.minCost = p->sp->cost;
					cap.weakest = p;
				}
		}
		else
		{
			if (sp->cost < cap.minCost)
			{
				cap.minCost = sp->cost;
				cap.weakest = &cs;
			}
			if (++cap.count >= cap.limit)
				cap.full = true;
		}
	}
	std::vector<std::string> out;
	for (const CalcSpell* p : picked)
		out.push_back(p->sp->id);
	return out;
}
