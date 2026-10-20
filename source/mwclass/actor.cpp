// What people and creatures have in common (OpenMW's MWClass::Actor): their body or pockets, and talking to them
#include <cmath>

#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Actor::search(Session& s, int ref)
{
	s.containerRef = ref;
	s.openScreen(SCR_CONTAINER);
}

void MWClass::Actor::talk(Session& s, int ref)
{
	World& w = s.w;
	Ref& r = w.refs[ref];
	// Face the speaker's head while talking
	float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
	float dz = r.pos[2] + 115.0f - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
	w.player.yaw = atan2f(dx, dy);
	w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
	// With no greeting that fits there is no conversation (OpenMW)
	if (dialogueStart(s.dlg, w, s, ref))
	{
		s.wordsRevision = -1;
		s.openScreen(SCR_DIALOGUE);
	}
}
