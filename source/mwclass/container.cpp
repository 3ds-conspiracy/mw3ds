// Containers (OpenMW's MWClass::Container): opening one
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Container::activate(Session& s, int ref)
{
	if (s.lockGate(ref, "LockedChest") != 2)
		return;
	Ref& r = s.w.refs[ref];
	s.containerRef = ref;
	s.openScreen(SCR_CONTAINER);
	// plants and other organic containers open without a chest's creak
	if (!r.obj || !r.obj->organic)
		s.playSound(ref, "chest open");
}
