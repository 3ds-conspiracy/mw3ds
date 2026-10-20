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
	for (auto& e : o->effects)
		applyEffectToPlayer(e, o->name);
	playSound(-1, o->recType == REC_INGR ? "Swallow" : "Drink");
	w.removeItem(itemId, 1);
	if (o->recType == REC_INGR)
		useSkill(16, 1);              // Alchemy: eating ingredients teaches their effects (SKDT use 1)
}
