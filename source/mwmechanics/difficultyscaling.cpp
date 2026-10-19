// The difficulty setting's scaling of damage
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <malloc.h>

// Starting somewhere other than the Census office: character creation counts as done, with
// the first playable class and birthsign, and the player stands outside a door into this cell
// (the office's exit door when there is one).
// Adds items to the inventory and wears / wields them (one weapon, clothes and armor)
// Morrowind's difficulty (OpenMW's reading): x = 1% of the slider; hits and harmful spells on the player
// scale by 1 + fDifficultyMult x (or / when easier), the player's own by the mirror of it
float Session::difficultyScale(bool toPlayer) const
{
	float term = 0.01f * difficulty, m = w.game.gmstf("fdifficultymult", 5.0f);
	float x = toPlayer ? (term > 0.0f ? m * term : term / m) : (term > 0.0f ? -term / m : m * -term);
	return fmaxf(0.0f, 1.0f + x);
}
