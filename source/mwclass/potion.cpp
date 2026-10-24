// Potions (OpenMW's MWClass::Potion): their tooltip
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Potion::toolTipContents(Session& s, const Object& o)
{
	// its effects, those the player's Alchemy doesn't know as "?"
	for (size_t k = 0; k < o.effects.size(); k++)
		if (s.alchemyEffectKnown(k))
			s.tip.emplace_back(s.effectLine(o.effects[k], false), col::text);
		else
			s.tip.emplace_back("?", col::textDim);
}
