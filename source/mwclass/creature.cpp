// Creatures (OpenMW's MWClass::Creature): activating one
#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Creature::activate(Session& s, int ref)
{
	World& w = s.w;
	Ref& r = w.refs[ref];
	// Search the body
	if (r.dead)
	{
		Actor::search(s, ref);
		return;
	}
	// Creatures that trade or have words of their own are talked to like people (Creeper, the mudcrab
	// merchant, Vivec, Yagrum Bagarn); other living creatures do nothing
	// (one that is only running away still is: OpenMW bug 7631, Creeper and the mudcrab merchant hit and fleeing)
	bool talking = r.actor >= 0 && (r.ai != AI_COMBAT || r.fleeing)
		&& ((w.game.actors[r.actor].services & 0x3FFFF) || w.game.speakers.count(r.idLower));
	logf("activate: creature %s: services %x, speaker %d, ai %d -> %s", r.idLower.c_str(),
		r.actor >= 0 ? w.game.actors[r.actor].services : 0, (int)w.game.speakers.count(r.idLower), r.ai,
		talking ? "talk" : "nothing");
	// One with no words whose script says it is a companion opens the share straight away (OpenMW's DialogueWindow
	// with no greeting: Bloodmoon's wounded bear, BM_bear_be_unique, whose arrow is taken that way)
	if (!talking && r.ai != AI_COMBAT && s.isCompanion(ref))
	{
		s.containerRef = ref;
		s.companionSharing = true;
		s.companionWarning = s.pickpocketing = s.containerPutting = false;
		s.grid = UiGrid();
		s.screen = SCR_CONTAINER;
		return;
	}
	if (!talking)
		return;
	Actor::talk(s, ref);
}
