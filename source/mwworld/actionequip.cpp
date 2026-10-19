// Equipping items: what can be worn or held, and putting it on
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/inventoryhelpers.hpp"

static bool isTwoHanded(const Object* x)
{
	// long blade / blunt close / blunt wide / spear / axe two-handed, bow, crossbow
	int t = x->subtype;
	return x->type == "WEAP" && (t == 2 || t == 4 || t == 5 || t == 6 || t == 8 || t == 9 || t == 10);
}

// OpenMW's canBeEquipped: a broken weapon or armor piece is refused, a beast race (Khajiit, Argonian: the race's flag)
// takes no full helmet, boots or shoes (by the parts the item's model has, not its name); a two-handed weapon is code 2, a
// shield with one in hand code 3 (neither stops the equip)
int Session::canEquip(const InventoryItem& it, std::string* why)
{
	const Object* o = w.game.object(it.id);
	if (!o)
		return 0;
	auto no = [&](const char* id, const char* text) {
		if (why)
			*why = w.game.gmst(id, text);
		return 0;
	};
	if ((o->type == "ARMO" || o->type == "WEAP") && o->health > 0 && itemCondition(it) <= 0)
		return no("sinventorymessage1", "This object is broken and cannot be equipped until fixed.");
	if (o->type == "ARMO" || o->type == "CLOT")
		for (auto& r : w.game.races)
			if (r.beast && lower(r.id) == lower(w.stats.race))
			{
				if (o->flags & 1)
					return no("snotifymessage13", "Beast races cannot wear full helmets.");
				if (o->flags & 2)
					return o->type == "ARMO" ? no("snotifymessage14", "Beast races cannot wear boots.")
						: no("snotifymessage15", "Beast races cannot wear shoes.");
			}
	if (isTwoHanded(o))
		return 2;
	if (o->type == "ARMO" && o->subtype == ARMO_SHIELD)
		for (auto& p : w.inventory)
			if (p.equipped)
			{
				const Object* q = w.game.object(p.id);
				if (q && isTwoHanded(q))
					return 3;
			}
	return 1;
}

// Equipping takes off what held the same place: one thing in hand (weapon, lockpick, probe), one ammunition, armor
// by slot (a gauntlet or bracer and a glove share a hand; boots and shoes a foot; a shield and a light the left hand),
// up to two rings. A two-handed weapon and a shield are both worn (OpenMW). The player's own equip can be refused
// (canEquip); a script's Equip is forced
bool Session::equipItem(int index, bool force)
{
	InventoryItem& it = w.inventory[index];
	const Object* o = w.game.object(it.id);
	if (!o)
		return false;
	std::string why;
	if (!force && canEquip(it, &why) == 0)
	{
		if (!why.empty())
			notify(why);
		return false;
	}
	auto hand = [](const Object* x) {
		return (x->type == "WEAP" && x->subtype < WEAP_ARROW) || x->type == "LOCK" || x->type == "PROB";
	};
	auto shield = [](const Object* x) { return x->type == "ARMO" && x->subtype == ARMO_SHIELD; };
	// Which hand armor / gloves cover (-1 none, 0 left, 1 right)
	auto handSide = [](const Object* x) {
		if (x->type == "ARMO")
			return x->subtype == 6 || x->subtype == 9 ? 0 : x->subtype == 7 || x->subtype == 10 ? 1 : -1;
		if (x->type == "CLOT")
			return x->subtype == 6 ? 0 : x->subtype == 5 ? 1 : -1;
		return -1;
	};
	// Boots (armor 5) and shoes (clothing 1) are the one foot slot
	auto foot = [](const Object* x) { return (x->type == "ARMO" && x->subtype == 5) || (x->type == "CLOT" && x->subtype == 1); };
	int rings = 0, firstRing = -1;
	for (size_t k = 0; k < w.inventory.size(); k++)
	{
		InventoryItem& other = w.inventory[k];
		const Object* p = w.game.object(other.id);
		if (!other.equipped || (int)k == index || !p)
			continue;
		bool off = false;
		if (hand(o) && hand(p))
			off = true;
		else if ((shield(o) && p->type == "LIGH") || (o->type == "LIGH" && shield(p)))
			off = true;
		else if (foot(o) && foot(p))
			off = true;
		else if (o->type == "WEAP" && o->subtype >= WEAP_ARROW && p->type == "WEAP" && p->subtype >= WEAP_ARROW)
			off = true;
		else if (handSide(o) >= 0 && handSide(o) == handSide(p))
			off = true;
		else if (o->type == "ARMO" && p->type == "ARMO" && o->subtype == p->subtype)
			off = true;
		else if (o->type == "CLOT" && p->type == "CLOT" && o->subtype == p->subtype)
		{
			if (o->subtype == 8)           // rings: two may be worn
			{
				if (firstRing < 0)
					firstRing = k;
				rings++;
			}
			else
				off = true;
		}
		else if (o->type == "LIGH" && p->type == "LIGH")
			off = true;
		if (off)
			other.equipped = false;
	}
	if (rings >= 2)
		w.inventory[firstRing].equipped = false;
	it.equipped = true;
	playSound(-1, "Item Misc Up");
	w.setItemScriptLocal(it.id, "onpcequip", 1.0f);
	w.refreshStats();                  // worn constant effects
	return true;
}

// Equip on the player from a script: the item is put in the pack first when it isn't there, then used as
// from the inventory (worn, drunk, eaten, read)
void Session::equipPlayerItem(const std::string& id)
{
	const Object* o = w.game.object(id);
	if (!o)
		return;
	int idx = -1;
	for (size_t i = 0; i < w.inventory.size() && idx < 0; i++)
		if (w.inventory[i].id == id)
			idx = i;
	if (idx < 0)
	{
		w.addItem(id, 1);
		for (size_t i = 0; i < w.inventory.size() && idx < 0; i++)
			if (w.inventory[i].id == id)
				idx = i;
	}
	if (idx < 0)
		return;
	if ((o->type == "ALCH" || o->type == "INGR") && !o->effects.empty())
		consume(id);
	else if (o->type == "BOOK")
	{
		openScreen(SCR_BOOK);
		bookItem = id;
		playSound(-1, o->scroll ? "scroll" : "Book Open");
	}
	else if (o->type == "WEAP" || o->type == "ARMO" || o->type == "CLOT" || o->type == "LIGH"
		|| o->type == "LOCK" || o->type == "PROB")
	{
		if (!w.inventory[idx].equipped)
			equipItem(idx, true);
	}
}
