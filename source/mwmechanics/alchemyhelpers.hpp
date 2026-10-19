#pragma once

// What alchemy and its screen share
#include <cmath>
#include <functional>


extern const World* s_alchemyWorld;
bool sameEffect(const Object::IngredientEffect& a, const Object::IngredientEffect& b);
std::vector<Object::IngredientEffect> commonEffects(const World& w, const std::vector<std::string>& ids);

// The apparatus the player carries: best quality of each kind (0 mortar, 1 alembic, 2 calcinator, 3 retort)
static inline void bestApparatus(const World& w, float quality[4])
{
	for (int k = 0; k < 4; k++)
		quality[k] = 0.0f;
	for (auto& it : w.inventory)
		if (const Object* o = w.game.object(it.id))
			if (o->type == "APPA" && o->subtype >= 0 && o->subtype < 4)
				quality[o->subtype] = fmaxf(quality[o->subtype], o->quality);
}
