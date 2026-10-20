// Lockpicks, and probes and repair tools alike (OpenMW's MWClass::Lockpick, Probe, Repair): their tooltip
#include <cstdio>

#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Lockpick::toolTipStats(Session& s, const Object& o, const InventoryItem* it)
{
	char buf[96];
	int uses = it && it->condition >= 0 ? it->condition : o.uses;
	snprintf(buf, sizeof(buf), "Quality: %.2f   Uses: %d", o.quality, uses);
	s.tip.emplace_back(buf, col::text);
}
