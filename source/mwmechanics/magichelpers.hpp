#pragma once

// What the magic files share
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "mwmechanics/magiceffects.hpp"


static const int kSchoolSkill[6] = { 11, 13, 10, 12, 14, 15 };   // alteration .. restoration

bool isPresence(int e);
bool resistedBy(int e, int& res, int& weak);
void applyFortify(World& w, ActiveEffect& a, float sign);

static inline float frand() { return (rand() % 1000) / 1000.0f; }

static inline bool harmful(int e)
{
	return e == EFF_FIRE_DAMAGE || e == EFF_SHOCK_DAMAGE || e == EFF_FROST_DAMAGE || e == EFF_DRAIN_HEALTH
		|| e == EFF_DRAIN_MAGICKA || e == EFF_DRAIN_FATIGUE || e == EFF_DAMAGE_HEALTH || e == EFF_DAMAGE_MAGICKA
		|| e == EFF_DAMAGE_FATIGUE || e == EFF_POISON || e == EFF_DRAIN_ATTRIBUTE || e == EFF_DRAIN_SKILL
		|| e == EFF_DAMAGE_ATTRIBUTE || e == EFF_DAMAGE_SKILL || (e >= EFF_WEAKNESS_FIRE && e <= EFF_DISINTEGRATE_ARMOR)
		|| e == EFF_PARALYZE || e == EFF_SILENCE || e == EFF_BLIND || e == EFF_SOUND || e == EFF_BURDEN
		|| e == EFF_FRENZY_HUMANOID || e == EFF_FRENZY_CREATURE || e == EFF_DEMORALIZE_HUMANOID
		|| e == EFF_DEMORALIZE_CREATURE || (e >= EFF_ABSORB_ATTRIBUTE && e <= EFF_ABSORB_SKILL);
}

// The player's resistance to an incoming effect, percent (OpenMW's getEffectResistance; negative is weakness). The
// resisting effects less the weakening ones, shields for fire, frost and shock; then Willpower's roll: an effect
// with a magnitude is cut by a fraction of a percent when the roll under (Willpower + Luck / 10) x fatigue x 50 /
// the cast chance (taken as 100) lands, one without a magnitude (paralysis, silence ...) is turned aside whole
// with that chance, the player's resistance moving the roll. resistable false: this effect can't be resisted
// The roll itself, for any target: `total(effect)` gives the target's magnitude of an effect; castChance is the
// caster's (uncapped) chance for the spell, 100 for potions, enchantments and traps (x = ... x 50 / castChance)
template <class Total>
static float resistRoll(const World& w, int e, bool noMagnitude, Total total, float willpower, float luck, float fatigue,
	float fatigueMax, float castChance, bool& resistable, int die = -1)
{
	int res, weak;
	resistable = resistedBy(e, res, weak);
	if (!resistable)
		return 0.0f;
	float r = total(res) - (weak >= 0 ? total(weak) : 0.0f);
	if (e == EFF_FIRE_DAMAGE) r += total(EFF_FIRE_SHIELD);
	if (e == EFF_FROST_DAMAGE) r += total(EFF_FROST_SHIELD);
	if (e == EFF_SHOCK_DAMAGE) r += total(EFF_LIGHTNING_SHIELD);
	float normalised = floorf(fatigueMax) == 0.0f ? 1.0f : fmaxf(0.0f, fatigue / fatigueMax);
	float ft = w.game.gmstf("ffatiguebase", 1.25f) - w.game.gmstf("ffatiguemult", 0.5f) * (1.0f - normalised);
	float x = (willpower + 0.1f * luck) * ft;
	if (castChance > 0.0f)
		x *= 50.0f / castChance;
	float roll = die >= 0 ? (float)die : frand() * 100.0f;       // (tests fix the die)
	if (noMagnitude)
		roll -= r;
	if (x <= roll)
		x = 0.0f;
	else
		x = noMagnitude ? 100.0f : roll / fminf(x, 100.0f);
	return fminf(x + r, 100.0f);
}

static inline float resistance(const World& w, int e, bool noMagnitude, float castChance, bool& resistable)
{
	const PlayerStats& s = w.stats;
	return resistRoll(w, e, noMagnitude, [&](int k) { return w.effectTotal(k); }, (float)s.attributes[ATTR_WILLPOWER],
		(float)s.attributes[ATTR_LUCK], s.fatigue, s.fatigueMax, castChance, resistable);
}

// Whether the effect changes a stat for as long as it lasts (undone when it ends)
static inline bool isFortify(int e)
{
	return e == EFF_FORTIFY_ATTRIBUTE || e == EFF_FORTIFY_SKILL || e == EFF_FORTIFY_HEALTH || e == EFF_FORTIFY_MAGICKA
		|| e == EFF_FORTIFY_FATIGUE || e == EFF_FORTIFY_ATTACK || e == EFF_SHIELD || e == EFF_JUMP
		|| e == EFF_LEVITATE || e == EFF_SWIFT_SWIM || e == EFF_DRAIN_ATTRIBUTE || e == EFF_DRAIN_SKILL
		|| e == EFF_ABSORB_ATTRIBUTE || e == EFF_ABSORB_SKILL || e == EFF_FORTIFY_MAX_MAGICKA;
}
