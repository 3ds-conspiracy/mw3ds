// How much a fighter wants a spell or potion effect (OpenMW's rateEffect)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwbase/session.hpp"

// How much an effect is worth to an actor fighting the player (OpenMW's rateEffect, spellpriority.cpp; withEnemy false:
// a potion, rated with no enemy). 1 x the effect's cost (calcEffectCost, GameSpell), made negative when harmful (then x
// the player's resistance left: 1 - resist / 100) and negative again aimed at someone else, so a harmful one at the player
// rates its cost and a heal of the player less than nothing. 0 for what the AI can't use: Soul Trap, interventions, Calm,
// Frenzy, Demoralize, Rally, Charm, detections, Telekinesis, Mark, Recall, Jump, the water effects, Slow Fall, Light,
// Lock, Open, Turn Undead, the disease effects, Invisibility, Chameleon, Night Eye, Vampirism, Stunted Magicka, Remove
// Curse, Command; the resistances, Spell Absorption and Reflect (no telling what the enemy will cast); the fortifies (cast
// over and over); Levitate; Restore Attribute / Skill. Blind only at a player with a weapon out and able to use it; Sound
// and Silence only at one with a spell ready; Burden only when it overburdens; Absorb Magicka at a player with none left
// 0.5 x the restore-magicka priority; Drain / Damage / Absorb of an attribute weighed by the attribute (Strength 1 ..
// Luck 0.3), not of one at 0; Disintegrate Armor / Weapon only when there is some; Cure Paralysis / Poison 1001 for each
// such effect on itself lasting over 3 s; Dispel by how many harmful and helpful effects it would take off; a summon 3,
// none while its own is about. Target effects 0 under water (either of them). A no-magnitude effect already on whom it
// is for: 0. Restore Health / Magicka / Fatigue on self by need: with p 4 for health, 2 for fatigue, and for magicka 2
// when a spell it knows costs more than it has left (and no more than its maximum), else 0; heal = the mean magnitude x
// the duration, the damage = maximum - now, and over = heal - damage when the damage is under heal and more than half
// is left (then heal = the damage): (p - 1) / 2 x (damage / maximum + 0.6)^(2p) + p x (heal - 2 over) / maximum - 0.5.
// Bound items rate 0 here: an NPC's bound weapon or armour isn't done (the player's only)
float Session::npcEffectRating(int ri, const SpellEffect& e, bool withEnemy)
{
	static const int kUseless[] = { 58, 63, 62, 49, 50, 51, 52, 53, 54, 55, 56, 44, 64, 65, 66, 59, 60, 61, 9, 0, 1, 2, 11, 41,
		12, 13, 101, 32, 33, 34, 69, 70, 71, 94, 95, 96, 39, 40, 43, 133, 136, 126, 100, 118, 119,
		90, 91, 92, 93, 97, 98, 99, 67, 68, 79, 80, 81, 82, 83, 84, 117, 10, 74, 78,
		120, 121, 122, 123, 124, 125, 127, 128, 129, 130, 131 };
	if (std::find(std::begin(kUseless), std::end(kUseless), e.effect) != std::end(kUseless))
		return 0.0f;
	const Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	const bool helpless = w.player.knockTimer > 0.0f || w.effectTotal(45) > 0.0f;
	// (the player's stance: a weapon out, else a spell ready when one is chosen: the 3DS casts from either)
	const bool spellReady = !weaponDrawn && !w.stats.selectedSpell.empty();
	// a spell it knows costs more than it has left (and no more than its maximum): magicka is worth restoring
	auto magickaPriority = [&]() {
		for (const std::vector<Atom>* list : { &def.combatSpells, &def.spells })
			for (const Atom& id : *list)
			{
				auto it = w.game.spells.find(lower(id));
				if (it != w.game.spells.end() && it->second.type == 0 && it->second.cost > r.magicka && it->second.cost < r.magickaMax)
					return 2.0f;
			}
		return 0.0f;
	};
	// effects lasting over 3 s more, harmful or helpful, on itself or on the player (only: of that effect)
	auto lasting = [&](bool onSelf, bool harmful, int only) {
		int n = 0;
		if (onSelf)
		{
			for (const auto& t : r.effects)
				if ((only < 0 || t.effect == only) && t.remaining > 3.0f && ((effectFlags(t.effect) & 0x10) != 0) == harmful)
					n++;
		}
		else
			for (const ActiveEffect& t : w.effects)
				if ((only < 0 || t.effect == only) && t.remaining > 3.0f && ((effectFlags(t.effect) & 0x10) != 0) == harmful)
					n++;
		return n;
	};
	float rating = 1.0f;
	switch (e.effect)
	{
	case 47:                // Blind
		if (!withEnemy || helpless || !weaponDrawn)
			return 0.0f;
		break;
	case 48:                // Sound
		if (!withEnemy || w.effectTotal(46) > 0.0f || helpless || !spellReady)
			return 0.0f;
		break;
	case 46:                // Silence
		if (!withEnemy || helpless || !spellReady)
			return 0.0f;
		break;
	case 7:                 // Burden: only to overburden the player
	{
		if (!withEnemy)
			return 0.0f;
		float weight = w.effectTotal(7) - w.effectTotal(8);
		for (auto& it : w.inventory)
			if (const Object* o = w.game.object(it.id))
				weight += o->weight * it.count;
		float burden = weight - w.game.gmstf("fencumbrancestrmult", 5.0f) * w.stats.attributes[ATTR_STRENGTH];
		if (burden > 0.0f || (e.min + e.max) * 0.5f <= -burden)
			return 0.0f;
		rating *= 3.0f;
		break;
	}
	case 8:                 // Feather: only for an overburdened caster (actors carry no load here)
		return 0.0f;
	case 87:                // Absorb Magicka
		if (withEnemy && w.stats.magicka <= 0.0f)
			rating = 0.5f * magickaPriority();
		break;
	case 75: case 76: case 77:
		if (e.range == 0)
		{
			float now = e.effect == 75 ? r.health : e.effect == 76 ? r.magicka : r.fatigue;
			float max = e.effect == 75 ? r.healthMax : e.effect == 76 ? r.magickaMax : r.fatigueMax;
			if (max <= 0.0f)
				return 0.0f;
			float p = e.effect == 75 ? 4.0f : e.effect == 76 ? magickaPriority() : 2.0f;
			float heal = (e.min + e.max) * 0.5f * fmaxf(1.0f, (float)e.duration), damage = fmaxf(max - now, 0.0f), over = 0.0f;
			if (damage < heal && now > max * 0.5f)
			{
				over = heal - damage;
				heal = damage;
			}
			rating = (p - 1.0f) / 2.0f * powf(damage / max + 0.6f, p * 2.0f) + p * (heal - 2.0f * over) / max - 0.5f;
		}
		break;
	case 57:                // Dispel
	{
		int diff = e.range == 0 ? lasting(true, true, -1) - lasting(true, false, -1)
			: (withEnemy ? lasting(false, false, -1) - lasting(false, true, -1) : 0);
		if (e.range != 0)
			rating = -rating;
		if (diff <= 0)
			return 0.0f;
		rating *= diff / 5.0f;
		break;
	}
	case 73:                // Cure Paralysis / Poison (over Dispel, which takes the good ones too)
		return 1001.0f * lasting(true, true, 45);
	case 72:
		return 1001.0f * lasting(true, true, 27);
	case 38:                // Disintegrate Armor: only at armour worn
	{
		if (!withEnemy)
			return 0.0f;
		bool armor = false;
		for (auto& it : w.inventory)
			if (const Object* o = it.equipped ? w.game.object(it.id) : nullptr)
				armor |= o->type == "ARMO";
		if (!armor)
			return 0.0f;
		break;
	}
	case 37:                // Disintegrate Weapon: only at a weapon held
		if (!withEnemy || !playerWeapon())
			return 0.0f;
		break;
	case 85: case 22: case 17:
	{
		static const float kAttrPriority[8] = { 1.0f, 0.5f, 0.6f, 0.7f, 0.5f, 0.8f, 0.7f, 0.3f };
		if (withEnemy && e.attribute >= 0 && e.attribute < 8 && w.stats.attributes[e.attribute] <= 0)
			return 0.0f;
		if (e.attribute >= 0 && e.attribute < 8)
			rating *= kAttrPriority[e.attribute];
		break;
	}
	case 89: case 26: case 21:
		if (!withEnemy || (e.skill >= 0 && e.skill < 27 && w.stats.skills[e.skill] <= 0))
			return 0.0f;
		break;
	default:
		break;
	}
	if (summonCreatureId(e.effect))
	{
		for (const NpcSummon& n : npcSummons)
			if (n.caster == ri)
				return 0.0f;                           // one summoned creature at a time
		rating = 3.0f;                                 // but summons rate above other effects
	}
	// (under water: three quarters of the way up the body, OpenMW's isUnderwater(actor, 0.75))
	if (e.range == 2 && (!withEnemy || w.underWater(r.pos[2] + 96.0f) || w.underWater(w.player.feet[2] + 96.0f)))
		return 0.0f;
	int flags = effectFlags(e.effect);
	if (flags & 0x10)
	{
		rating = -rating;
		if (!withEnemy)
			return 0.0f;
		rating *= 1.0f - fminf(resistBase(e.effect), 100.0f) / 100.0f;
	}
	if ((flags & 0x8) && ((flags & 0x10) ? w.effectTotal(e.effect) : w.actorEffect(ri, e.effect)) > 0.0f)
		return 0.0f;
	auto me = w.game.magicEffects.find(e.effect);
	float base = me != w.game.magicEffects.end() ? me->second.cost : 1.0f;
	bool magnitude = !(flags & 0x8), duration = !(flags & 0x4);
	float dur = duration ? (float)e.duration : 1.0f;
	if (!(flags & 0x1000))
		dur = fmaxf(1.0f, dur);
	float lo = magnitude ? fmaxf(1.0f, (float)e.min) : 1.0f, hi = magnitude ? fmaxf(1.0f, (float)e.max) : 1.0f;
	rating *= (0.5f * (lo + hi) * 0.1f * base * dur + 0.05f * fmaxf(0.0f, (float)e.area) * base) * w.game.gmstf("feffectcostmult", 0.5f);
	if (e.range != 0)
		rating = -rating;
	return rating;
}

// A list of effects (OpenMW's rateEffects): each one's rating x fAIRangeMagicSpellMult on target, fAIMagicSpellMult else
// (spellMult false: neither, for a weapon's on-strike enchantment)
float Session::npcEffectsRating(int ri, const std::vector<SpellEffect>& effects, bool withEnemy, bool spellMult)
{
	float rating = 0.0f;
	for (const SpellEffect& e : effects)
		rating += npcEffectRating(ri, e, withEnemy) * (!spellMult ? 1.0f
			: e.range == 2 ? w.game.gmstf("fairangemagicspellmult", 5.0f) : w.game.gmstf("faimagicspellmult", 3.0f));
	return rating;
}
