// The recharge screen (a filled soul gem into an enchanted item)
#include <cmath>
#include <functional>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// Recharging: enchanted items that aren't full; tapping one pours the chosen gem's soul into it
void Session::drawRecharge()
{
	header(w.game.gmst("srechargeenchantment", "Recharge Enchanted Item"));
	int gem = -1;
	for (size_t i = 0; i < w.inventory.size(); i++)
		if (w.inventory[i].soul == rechargeGem)
			gem = i;
	std::vector<int> items;
	std::vector<std::string> rows;
	for (size_t i = 0; i < w.inventory.size(); i++)
		if (const SpellDef* en = w.enchantmentOf(w.inventory[i]))
			if ((en->type == ENCH_USE || en->type == ENCH_STRIKE) && w.chargeOf(w.inventory[i]) < en->charge)
			{
				const Object* o = w.game.object(w.inventory[i].id);
				items.push_back(i);
				rows.push_back((o ? o->name.str() : w.inventory[i].id) + "   " + std::to_string((int)w.chargeOf(w.inventory[i]))
					+ "/" + std::to_string(en->charge));
			}
	if (rows.empty())
		uiTextCentered(160, 100, 0.45f, col::textDim, "Nothing needs recharging");
	int pick = uiList(list2, 4, 24, 312, 176, rows, 0.45f, true);
	if (pick >= 0 && pick < (int)items.size() && gem >= 0)
	{
		rechargeItem(items[pick], gem);
		screen = SCR_INVENTORY;
		return;
	}
	int noFocus = -1;
	int b = buttonRow({ w.game.gmst("sclose", "Close") }, noFocus);
	if (b == 0 || (uiIn().down & KEY_B) || gem < 0)
		screen = SCR_INVENTORY;
}
