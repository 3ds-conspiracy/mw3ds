// Pickpocketing: the chance and getting caught
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwmechanics/combathelpers.hpp"

// Pickpocketing (OpenMW's reading of Morrowind): the thief's and the victim's
// (Sneak + Agility / 5 + Luck / 10) x fatigue, the victim's raised by what the item is worth
int Session::pickpocketBar(int victim, float valueTerm)
{
	const Ref& v = w.refs[victim];
	const ActorDef& def = w.game.actors[v.actor];
	const PlayerStats& s = w.stats;
	float x = (0.2f * s.attributes[ATTR_AGILITY] + 0.1f * s.attributes[ATTR_LUCK] + s.skills[SKILL_SNEAK])
		* fatigueTerm(s.fatigue, s.fatigueMax);
	float y = (valueTerm + 0.2f * def.attributes[ATTR_AGILITY] + 0.1f * def.attributes[ATTR_LUCK] + def.skills[SKILL_SNEAK])
		* fatigueTerm(v.fatigue, v.fatigueMax);
	float t = 2.0f * x - y;
	float pcSneak = (float)s.skills[SKILL_SNEAK];
	float minChance = w.game.gmstf("ipickminchance", 5), maxChance = w.game.gmstf("ipickmaxchance", 75);
	if (t < pcSneak / minChance)
		return (int)(pcSneak / minChance);
	return (int)fminf(maxChance, t);
}

bool Session::pickpocketCaught(float valueTerm)
{
	if (containerRef < 0 || w.refs[containerRef].actor < 0)
		return false;
	int roll = persuadeRoll >= 0 ? persuadeRoll : rand() % 100;       // (tests fix the die)
	return roll > pickpocketBar(containerRef, valueTerm);
}
