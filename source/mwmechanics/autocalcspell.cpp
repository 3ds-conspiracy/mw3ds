// Spells chosen for actors and the player by their skills (autocalc)
#include "mwworld/esmstore.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>


// School of each magic skill: alteration, conjuration, destruction, illusion, mysticism, restoration
static const int kSchoolSkillIdx[6] = { 11, 13, 10, 12, 14, 15 };

// What autocalc needs of each candidate spell, worked out once (the lookups and costs don't depend
// on who casts): per effect its school's skill and its cost term (calcWeakestSchool's x)
struct CalcEffect { int school, skill, attribute; float x; };
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
			int dur = (d.flags & 0x4) ? 0 : e.duration;
			dur = dur < 1 ? 1 : dur;
			float x = 0.5f * ((mn > 1 ? mn : 1) + (mx > 1 ? mx : 1)) * 0.1f * d.cost * (1 + dur) + 0.05f * d.cost;
			x *= costMult;
			if (e.range == 2)
				x *= 1.5f;
			int school = d.school >= 0 && d.school < 6 ? d.school : 0;
			cs.effects.push_back({ school, e.skill, e.attribute, x });
		}
		out.push_back(cs);
	}
	std::sort(out.begin(), out.end(), [](const CalcSpell& a, const CalcSpell& b) { return a.sp->id < b.sp->id; });
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

std::vector<std::string> GameData::autoCalcSpells(const int* attributes, const int* skills, bool player) const
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
		caps[k].limit = capLimit[k];
	pc.limit = pcLimit;
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
			// the cheapest of that school goes, and the next cheapest is the one to beat
			auto gone = std::find(picked.begin(), picked.end(), cap.weakest);
			if (gone != picked.end())
				picked.erase(gone);
			cap.minCost = 1 << 30;
			cap.weakest = nullptr;
			for (const CalcSpell* p : picked)
			{
				float st;
				if ((player || weakestSchool(*p, skills, &st) == school) && p->sp->cost < cap.minCost)
				{
					cap.minCost = p->sp->cost;
					cap.weakest = p;
				}
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
