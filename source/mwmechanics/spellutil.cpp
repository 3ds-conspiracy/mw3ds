// Spell costs, schools and the cast chance
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "mwbase/session.hpp"
#include "mwmechanics/magichelpers.hpp"

// One effect's cost the way the success chance counts it (OpenMW's calcSpellBaseSuccessChance: no
// duration offset, area as given, unlike the magicka cost of a spell being made)
static float successEffectCost(const World& w, const SpellEffect& e, int* school)
{
	auto it = w.game.magicEffects.find(e.effect);
	if (it == w.game.magicEffects.end())
		return 0.0f;
	float x = (float)e.duration;
	if (!(it->second.flags & 0x1000))                  // applied once: the duration as it is, else at least 1
		x = fmaxf(1.0f, x);
	x *= 0.1f * it->second.cost;
	x *= 0.5f * (e.min + e.max);
	x += e.area * 0.05f * it->second.cost;
	if (e.range == 2)
		x *= 1.5f;
	if (school)
		*school = it->second.school;
	return x * w.game.gmstf("feffectcostmult", 0.5f);
}

// The school a spell counts as (its chance and the skill it trains): the effect with the lowest
// twice-the-skill minus its cost (OpenMW's calcSpellBaseSuccessChance). lowest: that effect's 2 x skill
int Session::spellSchool(const SpellDef& sp, float* lowest)
{
	int school = 5;
	float y = 1e30f, lowestSkill = 0.0f;
	for (auto& e : sp.effects)
	{
		int sch = -1;
		float x = successEffectCost(w, e, &sch);
		if (sch < 0 || sch >= 6)
			continue;
		float s = 2.0f * w.stats.skills[kSchoolSkill[sch]];
		if (s - x < y)
		{
			y = s - x;
			school = sch;
			lowestSkill = s;
		}
	}
	if (lowest)
		*lowest = lowestSkill;
	return school;
}

int Session::castChance(const SpellDef& sp)
{
	// Powers always work; an enchantment is no spell of the caster's, so its target's resistance roll takes 100
	// (OpenMW's getEffectMultiplier: castChance only for an ESM::Spell)
	if (sp.type == 5 || sp.type >= ENCH_ONCE)
		return 100;
	// A spell flagged "always succeeds" (SPDT flags 4: Bloodmoon's Call Wolf and Call Bear) too (OpenMW's F_Always)
	if (sp.flags & 4)
		return 100;
	const PlayerStats& s = w.stats;
	float lowest = 0.0f;
	spellSchool(sp, &lowest);
	// (twice the skill of the weakest effect - the cost + Willpower / 5 + Luck / 10 - Sound) x the fatigue term
	float chance = (lowest - sp.cost + s.attributes[ATTR_WILLPOWER] * 0.2f + s.attributes[ATTR_LUCK] * 0.1f
		- w.effectTotal(EFF_SOUND)) * (1.25f - 0.5f * (1.0f - s.fatigue / fmaxf(1.0f, s.fatigueMax)));
	return chance < 0 ? 0 : chance > 100 ? 100 : (int)chance;
}

// An enchanted item's cast: from its charge (scrolls are used up); no chance roll, no magicka
// What an enchanted item's cast takes from its charge: each point of Enchant over 10 takes a percent off, down
// to 1 (OpenMW's getEffectiveEnchantmentCastCost)
float Session::enchantCastCost(float cost)
{
	float r = cost - (cost / 100.0f) * (w.stats.skills[9] - 10);
	return (float)(int)(r < 1.0f ? 1.0f : r);
}
