// Ingredients (OpenMW's MWClass::Ingredient): their tooltip
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Ingredient::toolTipContents(Session& s, const Object& o)
{
	// as many of its effects as Alchemy reveals (fWortChanceValue each)
	int known = (int)(s.w.stats.skills[16] / s.w.game.gmstf("fwortchancevalue", 15.0f));
	for (size_t k = 0; k < o.ingredient.size(); k++)
		s.tip.emplace_back((int)k < known ? s.effectLabel(o.ingredient[k].effect, o.ingredient[k].skill, o.ingredient[k].attribute)
			: std::string("?"), (int)k < known ? col::text : col::textDim);
}
