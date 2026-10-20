// Potions (OpenMW's MWClass::Potion): their tooltip
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Potion::toolTipContents(Session& s, const Object& o)
{
	// its effects
	for (auto& e : o.effects)
		s.tip.emplace_back(s.effectLine(e, false), col::text);
}
