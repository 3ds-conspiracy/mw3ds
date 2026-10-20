// Lights (OpenMW's MWClass::Light): taking one
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Light::activate(Session& s, int ref)
{
	// (a light that can't be carried is part of the scenery: nothing happens)
	Ref& r = s.w.refs[ref];
	if (r.obj && !(r.obj->flags & 2))
		return;
	s.pickUp(ref);
}
