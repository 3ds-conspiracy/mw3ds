// Items: books, containers, the inventory, the magic menu
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// How a quick key is set from Inventory / Magic: the D-pad moves the selection there, so both layouts hold ZL
const char* kQuickHint = "ZL + D-pad: set quick key";

void Session::drawInventory()
{
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("sinventory", "Inventory"), "Gold " + std::to_string(gold));
	// Filter tabs (L / R), and the load against what the player can carry
	const UiInput& in = uiIn();
	int tab = invTab;
	if (in.down & KEY_L) tab = (tab + 4) % 5;
	if (in.down & KEY_R) tab = (tab + 1) % 5;
	for (int t = 0; t < 5; t++)
		if (uiButton(4 + t * 50, 24, 48, 18, w.game.gmstLabel(kTabKeys[t], kTabs[t]), t == tab))
			tab = t;
	if (tab != invTab)
	{
		invTab = tab;
		grid = UiGrid();
		playSound(-1, "Menu Click");
	}
	float weight = 0.0f;
	for (auto& it : w.inventory)
		if (const Object* o = w.game.object(it.id))
			weight += o->weight * it.count;
	float carry = w.game.gmstf("fencumbrancestrmult", 5.0f) * w.stats.attributes[ATTR_STRENGTH];
	char load[32];
	snprintf(load, sizeof(load), "%d/%d", (int)weight, (int)carry);
	uiTextRight(316, 26, 0.4f, weight > carry ? col::health : col::textDim, load);

	std::vector<int> index;
	std::vector<UiGridItem> cells;
	for (size_t i = 0; i < w.inventory.size(); i++)
	{
		const InventoryItem& it = w.inventory[i];
		if (it.id == "gold_001")
			continue;
		const Object* o = w.game.object(it.id);
		if (!inTab(o, invTab))
			continue;
		index.push_back(i);
		cells.push_back(gridItem(o, it.id, it.count, it.equipped));
	}
	if (cells.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "Nothing");
	int before = grid.selected;
	bool aKey = in.down & KEY_A;
	if (grid.selected >= 0 && grid.selected < (int)index.size())
	{
		const InventoryItem& qi = w.inventory[index[grid.selected]];
		const Object* qo = w.game.object(qi.id);
		assignQuickKey("inv:" + qi.id, qo ? qo->name.str() : qi.id);
	}
	int pick = uiItemGrid(grid, 4, 45, 312, 132, cells);
	uiText(6, 178, 0.36f, col::textDim, kQuickHint);
	int sel = grid.selected >= 0 && grid.selected < (int)index.size() ? index[grid.selected] : -1;
	const Object* o = sel >= 0 ? w.game.object(w.inventory[sel].id) : nullptr;
	std::string soulNote;
	if (sel >= 0 && !w.inventory[sel].soul.empty())
	{
		soulNote = w.game.gmstLabel("ssoulgem", "Soul") + ": " + w.inventory[sel].soul;
		for (auto& a : w.game.actors)
			if (lower(a.id) == w.inventory[sel].soul)
				soulNote = w.game.gmstLabel("ssoulgem", "Soul") + ": " + a.name;
	}
	itemInfo(o, 191, soulNote);
	if (sel >= 0)
		itemTip(o, &w.inventory[sel]);

	std::vector<std::string> labels;
	const std::string useLabel = w.game.gmstLabel("suse", "Use");
	if (o)
	{
		if (o->recType == REC_BOOK)
			labels.push_back("Read");
		else if (!o->effects.empty() && (o->recType == REC_ALCH || o->recType == REC_INGR))
			labels.push_back(o->recType == REC_ALCH ? "Drink" : "Eat");
		else if (o->recType == REC_APPA || o->recType == REC_REPA || !w.inventory[sel].soul.empty())
			labels.push_back(useLabel);
		if (!w.inventory[sel].soul.empty())
			labels.push_back("Recharge");
		else if (o->recType == REC_WEAP || o->recType == REC_ARMO || o->recType == REC_CLOT || o->recType == REC_LIGH
			|| o->recType == REC_LOCK || o->recType == REC_PROB)
			labels.push_back(w.inventory[sel].equipped ? w.game.gmstLabel("sunequip", "Unequip") : w.game.gmstLabel("sequip", "Equip"));
	}
	if (o)
	{
		labels.push_back(w.game.gmst("sdrop", "Drop"));
		if (w.inventory[sel].count > 1)
			labels.push_back("Drop all");
	}
	labels.push_back(w.game.gmst("sclose", "Close"));
	uiConsume(KEY_A | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
	int noFocus = -1;
	int b = buttonRow(labels, noFocus);
	// A, or tapping the selected item again, uses it (equip, drink, read)
	if (labels.size() > 1 && ((aKey && pick >= 0) || (!aKey && pick >= 0 && pick == before)))
		b = 0;
	if (b >= 0 && b < (int)labels.size() - 1 && o)
	{
		if (labels[b] == w.game.gmst("sdrop", "Drop") || labels[b] == "Drop all")
		{
			// Onto the floor in front of the player (it can be picked up again)
			InventoryItem it = w.inventory[sel];
			int n = labels[b] == "Drop all" ? it.count : 1;
			it.count = n;
			it.equipped = false;
			if (w.inventory[sel].equipped)
			{
				w.inventory[sel].equipped = false;
				w.refreshStats();
			}
			if ((w.inventory[sel].count -= n) <= 0)
				w.inventory.erase(w.inventory.begin() + sel);
			w.itemLeft(it.id);
			w.dropItemAt(it, w.player.feet, w.player.yaw, 50.0f);
			playSound(-1, "Item Misc Down");
			grid.selected = -1;
		}
		else if (labels[b] == "Drink" || labels[b] == "Eat")
			consume(w.inventory[sel].id);
		else if (labels[b] == "Recharge")
		{
			rechargeGem = w.inventory[sel].soul;
			list2 = UiList();
			screen = SCR_RECHARGE;
			playSound(-1, "Menu Click");
		}
		else if (labels[b] == useLabel && o->recType == REC_REPA)
		{
			openRepair(-1, sel);
			playSound(-1, "Menu Click");
		}
		else if (labels[b] == useLabel && !w.inventory[sel].soul.empty())
		{
			openEnchanting(-1);
			playSound(-1, "Menu Click");
		}
		else if (labels[b] == useLabel)
		{
			alchemySlots.clear();
			list2 = UiList();
			screen = SCR_ALCHEMY;
			playSound(-1, "Menu Click");
		}
		else if (labels[b] == "Read")
		{
			if (readRefused())
				return;
			std::string id = w.inventory[sel].id;
			bookItemsGrid = grid;
			openScreen(SCR_BOOK);
			bookFromItems = true;
			bookItem = id;
			playSound(-1, o->scroll ? "scroll" : "Book Open");
		}
		else
		{
			if (w.inventory[sel].equipped)
			{
				w.inventory[sel].equipped = false;
				playSound(-1, "Item Misc Up");
				w.refreshStats();
			}
			else
				equipItem(sel);
		}
	}
	else if (b == (int)labels.size() - 1 || (in.down & KEY_B))
		closeScreen();
}
