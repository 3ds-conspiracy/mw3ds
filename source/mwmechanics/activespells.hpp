#pragma once

// A magic effect active on the player
#include <string>

// A spell / potion effect running on the player (mwmechanics/spellcasting.cpp)
struct ActiveEffect
{
	int effect, attribute, skill;
	float magnitude, remaining;       // remaining seconds
	std::string source;
	int ref = -1;                     // summons: the creature, gone when the effect ends
	std::string item, prev;           // bound items: the item given, and what it replaced
};
