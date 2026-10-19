// Script functions: items (AddItem, RemoveItem, GetItemCount, Equip, Drop, soul gems)
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "mwworld/esmstore.hpp"
#include "mwrender/animation.hpp"
#include "components/debug/debuglog.hpp"
#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> containerExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// Drop: the item leaves the inventory and lies at their feet (freed slaves' bracers). OpenMW: the whole amount
	// is dropped, what the actor doesn't carry is made; 0 or less does nothing, and only actors drop things
	case F_DROP:
	{
		std::string id = lower(argStr(c, n, 0));
		int want = (int)argNum(c, n, 1, 1);
		if (want <= 0 || !w.game.object(id) || (!onPlayer && r && r->actor < 0))
			return 0.0f;
		if (onPlayer || !r)
		{
			int have = std::min(want, w.itemCount(id));
			if (have > 0)
				w.removeItem(id, have);
			w.dropItemAt({ id, want, false }, w.player.feet, w.player.yaw, 50.0f);
			return 0.0f;
		}
		for (size_t k = 0; k < r->contents.size(); k++)
			if (r->contents[k].second == id)
			{
				int have = std::min(want, stockCount(r->contents[k].first));
				r->contents[k].first += r->contents[k].first < 0 ? have : -have;
				if (r->contents[k].first == 0)
					r->contents.erase(r->contents.begin() + k);
				break;
			}
		w.dropItemAt({ id, want, false }, r->pos, r->rot[2], 30.0f);
		return 0.0f;
	}
	// HasSoulGem creature: how many gems hold that creature's soul; RemoveSoulGem takes one
	case F_HASSOULGEM: case F_REMOVESOULGEM:
	{
		if (!onPlayer && r)
			return 0.0f;           // other actors' and containers' gems aren't kept with their souls
		std::string soul = lower(argStr(c, n, 0));
		int count = 0;
		for (size_t i = 0; i < w.inventory.size(); i++)
			if (w.inventory[i].soul == soul)
			{
				count += w.inventory[i].count;
				if (n.func == F_REMOVESOULGEM)
				{
					if (--w.inventory[i].count <= 0)
						w.inventory.erase(w.inventory.begin() + i);
					return 1.0f;
				}
			}
		return (float)count;
	}

	// Items, as OpenMW's containerextensions.cpp: on the player, or on the container / actor named or running
	// the script; the gold coins of other sizes are gold_001; the messages (the game's own) only in conversation
	case F_GETITEMCOUNT:
	{
		std::string id = goldId(argStr(c, n, 0));
		if (onPlayer)
			return (float)w.itemCount(id);
		return r && (r->type == "CONT" || r->actor >= 0) ? (float)w.refItemCount(*r, id) : 0.0f;
	}
	case F_ADDITEM:
	case F_REMOVEITEM:
	{
		std::string id = goldId(argStr(c, n, 0));
		int count = (int)argNum(c, n, 1, 1.0f);
		if (count < 0)
			count = (u16)count;
		const Object* o = w.game.object(id);
		bool leveled = !o && w.game.leveled.count(id);
		if ((!o && !leveled) || count == 0 || (!onPlayer && (!r || (r->type != "CONT" && r->actor < 0))))
			return 0.0f;
		if (n.func == F_ADDITEM)
		{
			for (int k = 0; k < (leveled ? count : 1); k++)
			{
				std::string what = leveled ? w.game.pickLeveled(id, onPlayer ? w.stats.level : 1) : id;
				if (what.empty() || what == id && leveled)
					continue;
				if (onPlayer)
					w.addItem(what, leveled ? 1 : count);
				else
					w.refAddItem(*r, what, leveled ? 1 : count);
			}
			if (onPlayer && o && c.host.talking())
				c.host.notify(gmstFormat(w.game.gmst(count == 1 ? "sNotifyMessage60" : "sNotifyMessage61"), count, o->name));
			return 0.0f;
		}
		int removed = onPlayer ? w.removeItem(id, count) : w.refRemoveItem(*r, id, count);
		if (onPlayer && o && removed > 0 && c.host.talking())
			c.host.notify(gmstFormat(w.game.gmst(removed > 1 ? "sNotifyMessage63" : "sNotifyMessage62"), removed, o->name));
		return 0.0f;
	}
	case F_HASITEMEQUIPPED:
	{
		std::string id = lower(argStr(c, n, 0));
		if (!onPlayer && r)
		{
			// an NPC wears what it carries of armor, clothes and weapons
			const Object* o = r->actor >= 0 ? w.game.object(id) : nullptr;
			return o && (o->type == "ARMO" || o->type == "CLOT" || o->type == "WEAP") && w.refItemCount(*r, id) > 0
				? 1.0f : 0.0f;
		}
		for (auto& it : w.inventory)
			if (it.id == id && it.equipped)
				return 1.0f;
		return 0.0f;
	}
	// Equip item: on the player it is used like from the inventory (added first when missing); an NPC wears
	// what it carries, so it is given the item when it has none
	case F_EQUIP:
		if (onPlayer || !r)
			c.host.equipPlayerItem(lower(argStr(c, n, 0)));
		else if (r->actor >= 0)
		{
			std::string id = lower(argStr(c, n, 0));
			if (w.game.object(id) && w.refItemCount(*r, id) == 0)
				w.refAddItem(*r, id, 1);
		}
		return 0.0f;
	default:
		return std::nullopt;
	}
}
