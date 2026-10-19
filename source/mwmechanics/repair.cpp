// Repairing worn weapons and armor: at a smith for gold, or with a repair tool (hammer / prongs) by the
// player's own Armorer skill (Morrowind's rules as OpenMW has them)
#include <cmath>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/widgets.hpp"

// One repair of a carried weapon or armor piece, as the screen's row does: at the smith (repairRef) for his price,
// else a try with the tool (repairTool), which wears one use
void Session::repairOne(int item)
{
	const InventoryItem* tool = repairTool >= 0 && repairTool < (int)w.inventory.size() ? &w.inventory[repairTool] : nullptr;
	const Object* to = tool ? w.game.object(tool->id) : nullptr;
	int gold = w.itemCount("gold_001");
	InventoryItem& it = w.inventory[item];
	const Object* o = w.game.object(it.id);
	if (repairRef >= 0)
	{
		int price = repairPrice(repairRef, it);
		if (price > gold)
			notify(w.game.gmst("snotifymessage18", "You don't have enough gold."));
		else
		{
			w.removeItem("gold_001", price);
			w.refs[repairRef].gold += price;
			it.condition = -1;
			playSound(-1, "Repair");
			logf("repair: %s at %s for %d", it.id.c_str(), w.refs[repairRef].id.c_str(), price);
		}
	}
	else
	{
		float x = repairChance();
		int roll = rand() % 100;
		InventoryItem& t = w.inventory[repairTool];
		if (t.condition < 0)
			t.condition = to->uses > 0 ? to->uses : 10;
		if (roll <= x)
		{
			int y = repairAmount(to->quality, roll);
			int cond = std::min(o->health, itemCondition(it) + y);
			it.condition = cond >= o->health ? -1 : cond;
			useSkill(1, 0);               // Armorer
			playSound(-1, "Repair");
			notify(w.game.gmst("srepairsuccess", "You have successfully repaired the item."));
		}
		else
		{
			playSound(-1, "Repair Fail");
			notify(w.game.gmst("srepairfailed", "You failed to repair the item."));
		}
		logf("repair: %s with %s, chance %.0f, roll %d", it.id.c_str(), to->id.c_str(), x, roll);
		if (--t.condition <= 0)
		{
			notify(to->name + " is used up.");
			w.removeItem(t.id, 1);
			repairTool = -1;
		}
	}
}
