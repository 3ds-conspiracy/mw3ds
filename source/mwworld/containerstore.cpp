// Items in the player's and references' inventories: count, add, remove, stack
#include "mwworld/worldimp.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>
#include "mwworld/worldhelpers.hpp"

// Opening dialogue: the purse is full again once fBarterGoldResetDelay hours have passed since the last time
void World::restockGold(int ri)
{
	Ref& r = refs[ri];
	if (r.actor < 0 || gameHour - r.lastBarter < game.gmstf("fbartergoldresetdelay", 24.0f))
		return;
	r.gold = game.actors[r.actor].gold;
	r.lastBarter = gameHour;
	// The restocking stacks (negative counts in the record) are back to their quantity too: a plain item the sales used up
	// is added again (leveled lines keep what was picked)
	if (!r.obj)
		return;
	for (auto& item : r.obj->items)
	{
		std::string id = lower(item.second);
		if (item.first >= 0 || game.leveled.count(id) || !game.object(id))
			continue;
		bool found = false;
		for (auto& c : r.contents)
			if (c.second == id && c.first < 0 && c.plain())
			{
				c.first = item.first;
				found = true;
				break;
			}
		if (!found)
			r.contents.emplace_back(item.first, id);
	}
}

void World::pickUp(int ri)
{
	Ref& ref = refs[ri];
	if (ref.dropped || !ref.dropState.soul.empty() || ref.dropState.charge >= 0.0f)
	{
		// Dropped items, and placed ones with a soul or a charge, come back whole
		InventoryItem it = { ref.idLower, ref.count > 0 ? ref.count : 1, false, ref.dropState.condition,
			ref.dropState.soul, ref.dropState.charge };
		if (ref.dropped)
			despawn(ri);
		else
		{
			ref.pickedUp = true;
			hideRanges(*this, ref);
		}
		if (it.condition < 0 && it.soul.empty() && it.charge < 0.0f)
			addItem(it.id, it.count);
		else
			addStack(it);
		return;
	}
	ref.pickedUp = true;
	hideRanges(*this, ref);
	// The object's script keeps running while it is carried (mark it before addItem,
	// which would otherwise start a second copy for the item)
	if (ref.script >= 0)
	{
		ScriptInstance& s = scripts[ref.script];
		s.item = ref.idLower;
		if (float* v = s.local("onpcadd"))
			*v = 1.0f;
	}
	addItem(ref.id, ref.count > 0 ? ref.count : 1);
}

int World::itemCount(const std::string& id) const
{
	std::string l = lower(id);
	int n = 0;
	for (auto& it : inventory)
		if (it.id == l)
			n += it.count;           // filled soul gems count too
	return n;
}

void World::addItem(const std::string& id, int count)
{
	std::string l = lower(id);
	// Gold of any size is gold_001 in an inventory (a gold_100 added is one gold)
	if (l == "gold_005" || l == "gold_010" || l == "gold_025" || l == "gold_100")
		l = "gold_001";
	// A new one stacks with plain ones only: not onto a worn stack, nor a worn-down or part-charged one
	for (auto& it : inventory)
		if (it.id == l && it.soul.empty() && !it.equipped && it.condition < 0 && it.charge < 0.0f)
		{
			it.count += count;
			return;
		}
	inventory.push_back({ l, count, false });
	// Items handed over by scripts or dialogue start their own scripts
	const Object* o = game.object(l);
	if (o && !o->script.empty())
	{
		for (auto& s : scripts)
			if (s.item == l)
			{
				s.running = true;         // (it stopped when the last one left)
				return;
			}
		const Script* sc = game.script(o->script);
		if (sc)
		{
			ScriptInstance inst;
			inst.script = sc;
			inst.locals.assign(sc->localNames.size(), 0.0f);
			inst.item = l;
			if (float* v = inst.local("onpcadd"))
				*v = 1.0f;
			scripts.push_back(std::move(inst));
		}
	}
}

int World::removeItem(const std::string& id, int count)
{
	std::string l = lower(id);
	int want = count;
	bool wornGone = false;
	// Empty soul gems go before filled ones
	for (int pass = 0; pass < 2 && count > 0; pass++)
		for (size_t i = 0; i < inventory.size() && count > 0;)
			if (inventory[i].id == l && inventory[i].soul.empty() == (pass == 0))
			{
				int n = std::min(count, inventory[i].count);
				inventory[i].count -= n;
				count -= n;
				if (inventory[i].count <= 0)
				{
					wornGone |= inventory[i].equipped;
					inventory.erase(inventory.begin() + i);
				}
				else
					i++;
			}
			else
				i++;
	if (wornGone)
		refreshStats();                // its constant effects go with it
	// An item's script runs while it is carried: with the last one gone it stops
	if (want > count && itemCount(l) == 0)
		for (auto& s : scripts)
			if (s.item == l)
				s.running = false;
	return want - count;
}

int World::refItemCount(const Ref& r, const std::string& id) const
{
	std::string l = lower(id);
	int n = 0;
	for (auto& c : r.contents)
		if (lower(c.second) == l)
			n += stockCount(c.first);
	return n;
}

void World::refAddItem(Ref& r, const std::string& id, int count)
{
	std::string l = lower(id);
	// (a new one joins only a stack that is whole: not a worn one, a filled gem or a part-charged one)
	for (auto& c : r.contents)
		if (lower(c.second) == l && c.plain())
		{
			c.first += c.first < 0 ? -count : count;       // (a restocking quantity stays one)
			return;
		}
	r.contents.emplace_back(count, l);
}

// A whole item into a container (OpenMW's ContainerStore::stacks): onto a stack of the player's (not a restocking
// one) with the same id and soul when both are whole (no wear, full charge), else a stack of its own
void World::refAddStack(Ref& r, const InventoryItem& item)
{
	bool whole = item.condition < 0 && item.charge < 0.0f;
	if (whole)
		for (auto& c : r.contents)
			if (c.first > 0 && lower(c.second) == item.id && c.soul == item.soul && c.condition < 0 && c.charge < 0.0f)
			{
				c.first += item.count;
				return;
			}
	ContentItem c(item.count, item.id);
	c.condition = item.condition;
	c.soul = item.soul;
	c.charge = item.charge;
	r.contents.push_back(std::move(c));
}

// Count of a container's stack into the inventory: a plain one as addItem (gold, the item's script), one with wear,
// a soul or a charge whole (addStack)
void World::takeStack(const ContentItem& c, int count)
{
	if (c.plain())
		addItem(c.second, count);
	else
		addStack({ lower(c.second), count, false, c.condition, c.soul, c.charge });
}

int World::refRemoveItem(Ref& r, const std::string& id, int count)
{
	std::string l = lower(id);
	int removed = 0;
	// (empty soul gems go before filled ones, as World::removeItem)
	for (int pass = 0; pass < 2 && removed < count; pass++)
		for (size_t i = 0; i < r.contents.size() && removed < count;)
			if (lower(r.contents[i].second) == l && r.contents[i].soul.empty() == (pass == 0))
			{
				int n = std::min(count - removed, stockCount(r.contents[i].first));
				r.contents[i].first += r.contents[i].first < 0 ? n : -n;
				removed += n;
				if (r.contents[i].first == 0)
					r.contents.erase(r.contents.begin() + i);
				else
					i++;
			}
			else
				i++;
	return removed;
}

// A whole item joins the inventory: onto a stack of the same id and soul that is whole and not worn, else a new one
void World::addStack(const InventoryItem& item)
{
	for (auto& it : inventory)
		if (it.id == item.id && it.soul == item.soul && !it.equipped && it.condition < 0 && it.charge < 0.0f
			&& item.condition < 0 && item.charge < 0.0f)
		{
			it.count += item.count;
			return;
		}
	inventory.push_back(item);
}

// The soul size a gem holds (0 = empty or not a gem)
int World::soulValue(const InventoryItem& it) const
{
	if (it.soul.empty())
		return 0;
	for (auto& a : game.actors)
		if (lower(a.id) == it.soul)
			return a.soul;
	return 0;
}

void World::setItemScriptLocal(const std::string& itemId, const char* name, float value)
{
	for (auto& s : scripts)
		if (s.item == itemId)
			if (float* v = s.local(name))
				*v = value;
}
