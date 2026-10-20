#pragma once

// Inventory stacks: what a container or an actor holds, and the player's items
#include <string>

// One stack in a container's or an actor's contents: (count, id) as the record lists it (a negative count restocks),
// with the same per-item state an inventory stack carries (InventoryItem): wear, a soul, a charge
struct ContentItem : std::pair<int, std::string>
{
	using std::pair<int, std::string>::pair;
	int condition = -1;           // -1: as new
	std::string soul;             // a soul gem's soul (the creature's id)
	float charge = -1.0f;         // -1: full
	bool plain() const { return condition < 0 && soul.empty() && charge < 0.0f; }
};

// condition: wear of weapons / armor (-1 = as new; stacks with different wear are separate entries)
// soul: soul gems holding one (the creature's id); such entries stack only with the same soul
// charge: enchanted items' charge left (-1: full)
struct InventoryItem { std::string id; int count; bool equipped; int condition = -1; std::string soul; float charge = -1.0f; };
