// Drinking potions and eating ingredients
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "mwbase/session.hpp"

void Session::consume(const std::string& itemId)
{
	const Object* o = w.game.object(itemId);
	if (!o || o->effects.empty())
		return;
	playSound(-1, o->recType == REC_INGR ? "Swallow" : "Drink");
	w.removeItem(itemId, 1);
	if (o->recType != REC_INGR)
	{
		for (auto& e : o->effects)
			applyEffectToPlayer(e, o->name);
		return;
	}
	// Eating an ingredient (OpenMW's rollIngredientEffect): only its first effect, on a roll 0..99 at or under
	// x = (Alchemy + 0.2 Intelligence + 0.1 Luck) x the fatigue term, for int(roll / min(x, 100) x 0.25 x) seconds at a
	// magnitude of 0.05 x that / (0.1 x the effect's base cost), at least 1 (one with no duration: 1 s at that / (0.1 x
	// its cost); one with no magnitude: 1). Over x it does nothing, and teaches no Alchemy
	const PlayerStats& s = w.stats;
	float x = (s.skills[16] + 0.2f * s.attributes[ATTR_INTELLIGENCE] + 0.1f * s.attributes[ATTR_LUCK])
		* fatigueTermOf(s.fatigue, s.fatigueMax);
	int roll = rand() % 100;
	if (roll > x)
	{
		std::string msg = w.game.gmst("snotifymessage50", "%s has no effect on you.");
		size_t at = msg.find("%s");
		if (at != std::string::npos)
			msg.replace(at, 2, o->name.str());
		notify(msg);
		return;
	}
	SpellEffect e = o->effects[0];
	auto me = w.game.magicEffects.find(e.effect);
	int flags = me != w.game.magicEffects.end() ? me->second.flags : 0;
	float cost = me != w.game.magicEffects.end() && me->second.cost > 0.0f ? me->second.cost : 1.0f;
	float y = roll / fminf(x, 100.0f) * 0.25f * x;
	e.duration = (flags & 0x4) ? 1 : (int)y;
	float magnitude = 1.0f;
	if (!(flags & 0x8))
		magnitude = fmaxf(1.0f, floorf((flags & 0x4) ? y / (0.1f * cost) : 0.05f * y / (0.1f * cost)));
	e.min = e.max = (int)magnitude;
	applyEffectToPlayer(e, o->name);
	useSkill(16, 1);              // Alchemy: an ingredient that worked teaches (SKDT use 1)
}
