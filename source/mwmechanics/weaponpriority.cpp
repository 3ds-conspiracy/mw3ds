// How good a weapon is to a fighter (OpenMW's rateWeapon)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

// What a fighter does next, as OpenMW's prepareNextAction weighs it (aicombataction.cpp): its weapon against its
// potions (and its spells, below: only the healing ones are weighed so)

// Its weapon against the player (OpenMW's rateWeapon): the blow's mean damage (slash, thrust and chop each weighed by
// itself; a bow's chop; a throwing weapon's x 2) x (fDamageStrengthBase + Strength x fDamageStrengthMult / 10), less the
// player's Resist Normal Weapons for a plain one; a bow or crossbow adds its arrows' or bolts' own rating (none: 0); an
// on-strike enchantment adds its effects' rating (no spell multiplier); x the hit chance (0.01 .. 1) x its speed x
// fAIMeleeWeaponMult (fAIRangeMeleeWeaponMult for a ranged one out of the player's reach). Fists rate 0, and a ranged
// weapon under water (either of them). (Its wear is left out: actors' weapons don't wear here)
float Session::npcWeaponRating(int ri, float dist)
{
	const Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	const Object* wpn = w.game.object(def.weapon);
	if (!wpn || wpn->type != "WEAP" || wpn->subtype == WEAP_ARROW || wpn->subtype == WEAP_BOLT)
		return 0.0f;
	if (isRanged(wpn) && r.ammo <= 0)
		return 0.0f;
	return npcWeaponRatingOf(ri, wpn, dist);
}

float Session::npcWeaponRatingOf(int ri, const Object* wpn, float dist)
{
	const Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	bool ammo = wpn->subtype == WEAP_ARROW || wpn->subtype == WEAP_BOLT, ranged = isRanged(wpn) || ammo;
	float chop = (wpn->chop[0] + wpn->chop[1]) * 0.5f, rating = chop;
	float mult = w.game.gmstf("faimeleeweaponmult", 2.0f);
	if (ranged)
	{
		if (w.underWater(r.pos[2] + 96.0f) || w.underWater(w.player.feet[2] + 96.0f))
			return 0.0f;
		if (wpn->subtype == WEAP_THROWN)
			rating = chop * 2.0f;
		const Object* pw = playerWeapon();
		float foeReach = !pw ? kCombatDistance : isRanged(pw) ? 1e9f : fmaxf(1.0f, pw->reach) * kCombatDistance;
		if (dist - actorHalfLength(ri) - kPlayerHalfLength >= foeReach)
			mult = w.game.gmstf("fairangemeleeweaponmult", 5.0f);
	}
	else
	{
		float slash = (wpn->slash[0] + wpn->slash[1]) * 0.5f, thrust = (wpn->thrust[0] + wpn->thrust[1]) * 0.5f;
		float sum = slash + thrust + chop;
		rating = sum > 0.0f ? (slash * slash + thrust * thrust + chop * chop) / sum : 0.0f;
	}
	rating *= w.game.gmstf("fdamagestrengthbase", 0.5f)
		+ def.attributes[ATTR_STRENGTH] * w.game.gmstf("fdamagestrengthmult", 0.1f) * 0.1f;
	if (isRanged(wpn) && wpn->subtype != WEAP_THROWN)
	{
		const Object* am = w.game.object(def.ammo);
		float arrows = am && am->type == "WEAP" && (am->subtype == WEAP_ARROW || am->subtype == WEAP_BOLT) ? npcWeaponRatingOf(ri, am, dist) : 0.0f;
		if (arrows <= 0.0f)
			return 0.0f;
		rating += arrows;
	}
	else if (!((wpn->flags & 3) || wpn->magic))
		rating *= 1.0f - fminf(1.0f, (w.effectTotal(98) - w.effectTotal(36)) / 100.0f);
	if (!wpn->ench.empty())
	{
		auto en = w.game.spells.find(lower(wpn->ench));
		if (en != w.game.spells.end() && en->second.type == ENCH_STRIKE)
			rating += npcEffectsRating(ri, en->second.effects, true, false);
	}
	// (whether to take it up at all by the weapon's own skill, a creature's Combat, Magic or Stealth for it; the hit chance by
	// the skill its blows roll with: OpenMW's rateWeapon)
	if (def.skills[weaponSkill(wpn)] <= 0)
		return 0.0f;
	float chance = attackTermOf(attackSkill(def, wpn), def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK], r.fatigue, r.fatigueMax,
		w.actorEffect(ri, 117), w.actorEffect(ri, 47)) - playerDefense(false);
	rating *= fmaxf(0.01f, fminf(1.0f, chance / 100.0f));
	return ammo ? rating * mult : rating * wpn->speed * mult;
}
