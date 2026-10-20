#pragma once

// Spells and enchantments (OpenMW's ESM::Spell, ESM::Enchantment)
#include <string>
#include <vector>
#include "components/esm3/effectlist.hpp"

// type: 0 spell, 1 ability, 2 blight, 3 disease, 4 curse, 5 power
// type: 0 spell, 1 ability, 2 blight, 3 disease, 4 curse, 5 power; enchantments (game.json "enchantments")
// are 10 + their type: 10 cast once, 11 on strike, 12 when used, 13 constant; charge: theirs when full
enum { ENCH_ONCE = 10, ENCH_STRIKE = 11, ENCH_USE = 12, ENCH_CONSTANT = 13 };
struct SpellDef { std::string id, name; int type, cost = 0, flags = 0, charge = 0; std::vector<SpellEffect> effects; };
