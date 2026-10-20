// The repair screen: at a smith or with a repair tool
#include <cmath>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

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
	if (repairRef < 0 && (!to || to->recType != REC_REPA))
	{
		screen = SCR_INVENTORY;              // the tool is used up
		return;
	}
	std::string note = repairRef >= 0 ? w.game.gmstLabel("sgold", "Gold") + " " + std::to_string(gold)
		: to->name + " (" + std::to_string(tool->condition < 0 ? to->uses : tool->condition) + " uses)";
	header(w.game.gmst("srepair", "Repair"), note);
	std::vector<int> items;
	std::vector<std::string> rows;
	for (size_t i = 0; i < w.inventory.size(); i++)
	{
		const InventoryItem& it = w.inventory[i];
		const Object* o = w.game.object(it.id);
		if (!o || o->health <= 0 || (o->recType != REC_WEAP && o->recType != REC_ARMO) || itemCondition(it) >= o->health)
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
		repairOne(items[pick]);
	int noFocus = -1;
	int b = buttonRow({ w.game.gmst("sclose", "Close") }, noFocus);
	if (b == 0 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		screen = repairRef >= 0 ? SCR_DIALOGUE : SCR_INVENTORY;
		repairTool = -1;
	}
}
