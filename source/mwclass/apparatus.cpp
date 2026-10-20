// Alchemy apparatus (OpenMW's MWClass::Apparatus): its tooltip
#include <cstdio>

#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Apparatus::toolTipStats(Session& s, const Object& o)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "Quality: %.2f", o.quality);
	s.tip.emplace_back(buf, col::text);
}
