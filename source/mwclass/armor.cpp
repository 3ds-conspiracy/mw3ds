// Armor (OpenMW's MWClass::Armor): its tooltip
#include <cstdio>

#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"
#include "mwgui/windowmanagerimp.hpp"

void MWClass::Armor::toolTipStats(Session& s, const Object& o, const InventoryItem* it)
{
	World& w = s.w;
	// Its weight class, as OpenMW's tooltips name it (Light / Medium / Heavy Armor)
	if (o.subtype >= 0 && o.subtype < 11)
	{
		std::string typeLabel = w.game.gmst("stype", "Type");
		if (typeLabel.empty() || typeLabel.back() != ':')
			typeLabel += ":";
		s.tip.emplace_back(typeLabel + " " + skillName(w, s.armorSkill(&o)), col::textDim);
	}
	char buf[96];
	snprintf(buf, sizeof(buf), "Armor Rating: %d", o.armor);
	s.tip.emplace_back(buf, col::text);
	conditionTip(s, o, it);
}
