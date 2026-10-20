// Weapons (OpenMW's MWClass::Weapon): their tooltip
#include <cstdio>

#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"
#include "mwgui/windowmanagerimp.hpp"

void MWClass::Weapon::toolTipStats(Session& s, const Object& o, const InventoryItem* it)
{
	World& w = s.w;
	char buf[96];
	// Its kind, as OpenMW's tooltips name it: the weapon's skill, one or two handed for the melee ones ("Type: Long
	// Blade, Two Handed")
	if (o.subtype >= 0 && o.subtype <= WEAP_BOLT)
	{
		std::string typeLabel = w.game.gmst("stype", "Type");
		if (typeLabel.empty() || typeLabel.back() != ':')
			typeLabel += ":";
		static const int kSkill[14] = { 22, 5, 5, 4, 4, 4, 7, 6, 6, 23, 23, 23, 23, 23 };
		std::string kind = typeLabel + " " + skillName(w, kSkill[o.subtype]);
		if (o.subtype < WEAP_BOW)
			kind += ", " + (o.subtype == 0 || o.subtype == 1 || o.subtype == 3 || o.subtype == 7
				? w.game.gmst("sonehanded", "One Handed") : w.game.gmst("stwohanded", "Two Handed"));
		s.tip.emplace_back(kind, col::textDim);
	}
	bool ranged = o.subtype >= WEAP_BOW && o.subtype <= WEAP_BOLT;
	if (ranged)
		snprintf(buf, sizeof(buf), "Attack: %d - %d", o.chop[0], o.chop[1]), s.tip.emplace_back(buf, col::text);
	else
	{
		snprintf(buf, sizeof(buf), "Chop: %d - %d", o.chop[0], o.chop[1]), s.tip.emplace_back(buf, col::text);
		snprintf(buf, sizeof(buf), "Slash: %d - %d", o.slash[0], o.slash[1]), s.tip.emplace_back(buf, col::text);
		snprintf(buf, sizeof(buf), "Thrust: %d - %d", o.thrust[0], o.thrust[1]), s.tip.emplace_back(buf, col::text);
	}
	conditionTip(s, o, it);
}
