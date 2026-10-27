// What a fighter does next besides a blow: drink a potion, cast a spell
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

// A potion it carries whose rating beats its weapon's is drunk (OpenMW's ActionPotion: taken when chosen, 0.75 s before
// the next choice). NPCs never drank one
bool Session::npcDrinkPotion(int ri, float dist)
{
	Ref& r = w.refs[ri];
	if (r.contents.empty())
		return false;
	float best = npcWeaponRating(ri, dist);
	int pick = -1;
	for (size_t i = 0; i < r.contents.size(); i++)
	{
		const ContentItem& c = r.contents[i];
		const Object* o = w.game.object(c.second);
		if (c.first == 0 || !o || o->recType != REC_ALCH)
			continue;
		float rating = npcEffectsRating(ri, o->effects, false);      // (OpenMW's ratePotion: with no enemy)
		if (rating > best)
		{
			best = rating;
			pick = (int)i;
		}
	}
	if (pick < 0)
		return false;
	std::string id = r.contents[pick].second;
	const Object* o = w.game.object(id);
	w.refRemoveItem(r, id, 1);
	for (const SpellEffect& e : o->effects)
		applyEffectToActor(ri, e, false, 0.0f, 100.0f, id);
	playSound(ri, "Drink");
	logf("combat: %s drinks %s", r.id.c_str(), id.c_str());
	return true;
}

// A spell from the actor's lists when it rates above its weapon (OpenMW's prepareNextAction / rateSpell: its effects'
// ratings, npcEffectsRating, x the cast chance; 0 for anything but a spell, short of magicka, or one still on its
// target: the player for a touch or target one, itself for a self one). A touch spell waits for the player within its
// reach. A spell all on itself (a summon, a shield, a heal) lands as it leaves the hands (castSpell -2, npcSummonRelease),
// others go through its combat list (castSpell). Starts the cast; npcCombat releases it.
void Session::npcCastSpell(int ri)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	float dist = w.distanceToPlayer(ri);
	float reach = kCombatDistance + actorHalfLength(ri) + kPlayerHalfLength;   // a touch spell's (OpenMW: fCombatDistance)
	float best = npcWeaponRating(ri, dist);
	std::string pick;
	int range = -1;
	std::vector<std::string> ids;
	for (const std::vector<Atom>* list : { &def.combatSpells, &def.spells })
		for (const Atom& id : *list)
			if (std::find(ids.begin(), ids.end(), lower(id)) == ids.end())
				ids.push_back(lower(id));
	for (const std::string& id : ids)
	{
		auto it = w.game.spells.find(id);
		if (it == w.game.spells.end() || it->second.type != 0 || it->second.cost > r.magicka)
			continue;
		const SpellDef& sp = it->second;
		bool self = true, touch = false, onPlayer = false;
		for (auto& e : sp.effects)
		{
			self &= e.range == 0;
			touch |= e.range == 1;
		}
		if (touch && dist > reach)
			continue;
		if (!self && std::find(def.combatSpells.begin(), def.combatSpells.end(), id) == def.combatSpells.end())
			continue;
		// (spells don't stack: none still on whom it is for)
		for (const ActiveEffect& t : w.effects)
			onPlayer |= !self && t.source == sp.name;
		bool onSelf = false;
		for (auto& e : sp.effects)
			onSelf |= e.range == 0 && !summonCreatureId(e.effect) && (e.effect < 75 || e.effect > 77) && w.actorEffect(ri, e.effect) > 0.0f;
		if (onPlayer || onSelf)
			continue;
		static const int kSchoolSkill[6] = { 11, 13, 10, 12, 14, 15 };
		// (a spell flagged "always succeeds", SPDT flags 4: 100, as the player's castChance)
		float chance = (sp.flags & 4) ? 100.0f : fmaxf(0.0f, fminf(100.0f, (def.skills[kSchoolSkill[spellSchool(sp)]] * 2.0f
			+ def.attributes[ATTR_WILLPOWER] / 5.0f + def.attributes[ATTR_LUCK] / 10.0f - sp.cost - w.actorEffect(ri, 48))
			* fatigueTerm(r.fatigue, r.fatigueMax)));
		float rating = npcEffectsRating(ri, sp.effects, true) * chance / 100.0f;
		if (rating > best)
		{
			best = rating;
			pick = id;
			range = self ? 0 : touch ? 1 : 2;
		}
	}
	if (pick.empty() || (range == 2 && rand() % 3 == 0))
		return;
	const SpellDef& sp = w.game.spells[pick];
	r.magicka -= sp.cost;
	if (range == 0)
	{
		r.castSpell = -2;
		pendingSummon[ri] = pick;
	}
	else
		r.castSpell = (int)(std::find(def.combatSpells.begin(), def.combatSpells.end(), pick) - def.combatSpells.begin());
	r.shooting = false;
	float windup = 0.6f;
	if (Actor* a = w.actorOf(ri))
	{
		ActorSet& set = *w.actorsOf(ri);
		a->showWeapon = false;
		if (actorPlay(set, *a, range == 0 ? "CastSelf" : range == 1 ? "CastTouch" : "CastTarget", ANIM_ONCE))
		{
			const AnimGroup& g = actorSkeleton(set, a->skeleton).groups[a->group];
			windup = g.loopStart - g.start;
		}
	}
	r.hitAt = w.time + windup;
	r.attackTimer = windup + 1.0f;
	static const char* school[6] = { "alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration" };
	playSound(ri, std::string(school[spellSchool(sp)]) + " cast");
	castVfx(sp, ri);
	logf("combat: %s casts %s", r.id.c_str(), sp.name.c_str());
}
