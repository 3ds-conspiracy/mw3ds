#pragma once

// Items, lights, doors, containers and statics: one record for all (OpenMW keeps each in its load*.hpp)
#include "components/esm/defs.hpp"
#include "components/misc/atom.hpp"
#include "components/misc/cardtext.hpp"
#include <3ds/types.h>
#include <vector>
#include "components/esm3/effectlist.hpp"

struct Object
{
	std::string id;
	Atom type, name, script, icon, sound, openSound, closeSound;
	RecName recType = REC_OTHER;           // type as a number (components/esm/defs.hpp)
	CardText text;                                     // a book's (read from the card when opened)
	int iconIx = -1;                                   // item icon (ui/icons_*.t3x), -1 = none
	float weight = 0.0f;
	int value = 0, subtype = -1, armor = 0, flags = 0;
	bool scroll = false;
	bool respawn = false;                // containers: their contents come back (World::respawnCell)
	bool organic = false;                // containers: plants and the like (no chest sound when opened)
	float capacity = -1.0f;              // containers: the weight they hold (-1: data from before it was exported, no limit)
	int lightRadius = 0;                 // lights: how far they reach, and their colour (0..255)
	float lightTime = 0.0f;              // lights: the seconds one burns while held (0: for ever, a candle)
	u8 lightColor[3] = { 0, 0, 0 };
	bool magic = false;                                // enchanted
	std::vector<std::pair<int, Atom>> items;           // container / NPC contents: count, id
	std::vector<SpellEffect> effects;                  // potions, ingredients (first effect)
	// weapons
	u8 chop[2] = {}, slash[2] = {}, thrust[2] = {};    // min, max damage
	float speed = 1.0f, reach = 1.0f;
	int health = 0;                                    // weapons / armor: condition when new (0 = doesn't wear)
	int uses = 0;                                      // repair tools
	float quality = 0.0f;
	int bookSkill = -1;                                // skill books: the skill the first read raises
	Atom ench;                                         // enchanted: its enchantment (a SpellDef id)
	Atom iconBase;                                     // enchanted by the player: the item it was made from
	int enchant = 0;                                   // enchantment capacity
	struct IngredientEffect { int effect, skill, attribute; };
	std::vector<IngredientEffect> ingredient;          // ingredients: their (up to) four effects, for alchemy
};

// Weapon types (WPDT)
enum { WEAP_SHORT_BLADE = 0, WEAP_BOW = 9, WEAP_CROSSBOW = 10, WEAP_THROWN = 11, WEAP_ARROW = 12, WEAP_BOLT = 13 };
enum { ARMO_SHIELD = 8 };
