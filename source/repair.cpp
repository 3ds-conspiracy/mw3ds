// Repairing worn weapons and armor: at a smith for gold, or with a repair tool (hammer / prongs) by the
// player's own Armorer skill (Morrowind's rules as OpenMW has them)
#include <cmath>

#include "log.h"
#include "screens.h"
#include "session.h"
#include "ui.h"

void Session::openRepair(int smith, int tool)
{
	repairRef = smith;
	repairTool = tool;
	list2 = UiList();
	screen = SCR_REPAIR;
}

void Session::drawRepair()
{
	int gold = w.itemCount("gold_001");
	const InventoryItem* tool = repairTool >= 0 && repairTool < (int)w.inventory.size() ? &w.inventory[repairTool] : nullptr;
	const Object* to = tool ? w.game.object(tool->id) : nullptr;
	if (repairRef < 0 && (!to || to->type != "REPA"))
	{
		screen = SCR_INVENTORY;              // the tool is used up
		return;
	}
	std::string note = repairRef >= 0 ? "Gold " + std::to_string(gold)
		: to->name + " (" + std::to_string(tool->condition < 0 ? to->uses : tool->condition) + " uses)";
	header(w.game.gmst("srepair", "Repair"), note);
	std::vector<int> items;
	std::vector<std::string> rows;
	for (size_t i = 0; i < w.inventory.size(); i++)
	{
		const InventoryItem& it = w.inventory[i];
		const Object* o = w.game.object(it.id);
		if (!o || o->health <= 0 || (o->type != "WEAP" && o->type != "ARMO") || itemCondition(it) >= o->health)
			continue;
		items.push_back(i);
		std::string row = o->name + "   " + std::to_string(itemCondition(it)) + "/" + std::to_string(o->health);
		if (repairRef >= 0)
			row += "   " + std::to_string(repairPrice(repairRef, it)) + "g";
		rows.push_back(row);
	}
	if (rows.empty())
		uiTextCentered(160, 100, 0.45f, col::textDim, "Nothing needs repair");
	int pick = uiList(list2, 4, 24, 312, 176, rows, 0.45f, true);
	if (pick >= 0 && pick < (int)items.size())
	{
		InventoryItem& it = w.inventory[items[pick]];
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
	int noFocus = -1;
	int b = buttonRow({ w.game.gmst("sclose", "Close") }, noFocus);
	if (b == 0 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		screen = repairRef >= 0 ? SCR_DIALOGUE : SCR_INVENTORY;
		repairTool = -1;
	}
}
