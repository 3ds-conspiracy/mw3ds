// People (OpenMW's MWClass::Npc): activating one
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Npc::activate(Session& s, int ref)
{
	Ref& r = s.w.refs[ref];
	// Search the body
	if (r.dead)
	{
		Actor::search(s, ref);
		return;
	}
	if (r.ai == AI_COMBAT)
	{
		s.notify(s.w.game.gmst("sactorincombat", "This character is in combat."));
		return;
	}
	// Knocked down (not fighting): their pockets are open to anyone, no roll against them. Sneaking up on someone: their
	// pockets, with the pickpocket rolls (OpenMW checks both before talking)
	if (r.knockTimer > 0.0f && s.w.actorEffect(ref, 45) <= 0.0f)
	{
		s.pickpocketing = false;
		Actor::search(s, ref);
		return;
	}
	if (s.w.player.sneaking && !r.ally)
	{
		s.pickpocketing = true;
		Actor::search(s, ref);
		return;
	}
	Actor::talk(s, ref);
}
