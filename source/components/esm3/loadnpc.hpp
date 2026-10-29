#pragma once

// NPCs and creatures (OpenMW's ESM::NPC, ESM::Creature): services, travel, stats, AI
#include "components/misc/atom.hpp"
#include <3ds/types.h>
#include <vector>

// Merchant services (NPC AIDT flags): which item types they trade
enum
{
	SERVICE_WEAPON = 0x1, SERVICE_ARMOR = 0x2, SERVICE_CLOTHING = 0x4, SERVICE_BOOKS = 0x8,
	SERVICE_INGREDIENTS = 0x10, SERVICE_PICKS = 0x20, SERVICE_PROBES = 0x40, SERVICE_LIGHTS = 0x80,
	SERVICE_APPARATUS = 0x100, SERVICE_REPAIR_ITEMS = 0x200, SERVICE_MISC = 0x400, SERVICE_POTIONS = 0x2000,
	SERVICE_BARTER = 0x27FF,
};

struct TravelDest
{
	Atom cell;                           // display name
	bool hasGrid = false;                // outdoors: grid cell to load
	int grid[2] = {};
	float pos[3], rot[3];
};

struct ActorDef
{
	Atom id, name;                       // (interned: 4 bytes each where a std::string is 24)
	Atom race, cls, faction, script;
	int rank, female, level, disposition, reputation;
	int attributes[8] = { 40, 40, 40, 40, 40, 40, 40, 40 };
	int skills[27] = {};
	int health = 50, fatigue = 200, gold = 0, fight = 30;
	unsigned services = 0;
	Atom weapon;                         // lowercase WEAP id they fight with, "" = hand-to-hand
	float armor = 0.0f;                  // armor rating
	std::vector<TravelDest> travel;      // caravaner / boatman destinations inside the level
	int wander = 0;                      // AI_W distance: strolls this far from their spot
	// First AI package when it isn't wandering (AIPKG_*, -1 none): target id, destination, hours
	int aiPackage = -1;
	Atom lib;                            // mesh library (drawing it in a cell it wasn't placed in)
	Atom head, hair;                     // NPCs: their head and hair body parts, and the armor and clothing their library
	std::vector<Atom> worn;              // has on (dressed again from the third-person pieces once one leaves the pack)
	int skel = -1;                       // shared skeleton index
	Atom aiTarget;
	float aiDest[3] = {}, aiDuration = 0.0f;
	bool creature = false;
	std::vector<Atom> spells;            // spell merchants: what they sell (lowercase ids)
	std::vector<std::pair<int, int>> attack;   // creatures: damage min, max of each attack
	int magicka = 0, flee = 0;           // AI flee rating (0..100)
	int alarm = 0;                       // AI alarm (0..100): how readily they report crimes
	int blood = 0;                                     // 0 red, 1 skeleton (white), 2 metal (gold sparks)
	u8 afloat = 0;                       // creatures: 0x10 swims, 0x20 flies (they don't fall to the floor); 0x40 walks, 0x1 biped
	// creatures: their model's collision box (center x, y, half extents x, y, the origin's height over its bottom),
	// rested on the ground under all of it (World::actorBoxLift); hasBox false: the floor under the feet
	float box[5] = {};
	bool hasBox = false;
	bool respawn = false, essential = false;   // come back after dying / their death severs the prophecy
	bool autocalc = false;               // their spells are the engine's pick (GameData::autoCalcSpells)
	int soul = 0;                        // creatures: soul size (Soultrap fills a gem that holds it)
	int resistNormal = 0;                // percent: blows from ordinary weapons do that much less (ghosts)
	int hello = 30;                      // AI Hello: greets within hello x iGreetDistanceMultiplier (0: never)
	Atom ammo, shield;                   // lowercase ids: arrows / bolts for their bow, the shield they carry
	std::vector<Atom> combatSpells;          // lowercase: harmful spells they cast at the player, heals
	std::vector<Atom> diseases;              // lowercase: diseases their blows can pass on
	std::vector<std::pair<int, float>> constEffects;   // effect, magnitude: resistances, shields ... their abilities keep on them
};
