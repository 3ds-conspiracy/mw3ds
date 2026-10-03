#pragma once

#include <3ds/types.h>
#include <string>

// Something in flight (projectile.cpp): an arrow, bolt or thrown weapon, or a spell bolt.
struct Projectile
{
	float pos[3], vel[3];
	int owner = -1;           // ref index of whoever let it go, -1 = the player
	std::string item;         // arrow / bolt / thrown weapon id ("" = a spell)
	std::string weapon;       // the bow or crossbow it left (thrown weapons: the item itself)
	float charge = 1.0f;      // how far the shot was drawn (damage)
	std::string spell;        // spell id: its target-range effects land on whoever it hits
	float life = 4.0f;        // seconds before it drops out of the world
	u32 glow = 0;             // spell bolts: colour (0xAABBGGRR)
};
