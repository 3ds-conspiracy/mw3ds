// Items: books, containers, the inventory, the magic menu
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwphysics/physicssystem.hpp"

// How a quick key is set from Inventory / Magic: the D-pad moves the selection there, so a button is held with it. Y,
// which an Old 3DS has (no ZL there); ZL still does it on a New 3DS (Session::assignQuickKey)
const char* kQuickHint = "Hold Y + D-pad: set quick key";

void Session::drawInventory()
{
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("sinventory", "Inventory"), "Gold " + std::to_string(gold));
	// Filter tabs (L / R), and the load against what the player can carry
	const UiInput& in = uiIn();
	int tab = invTab;
	bool counting = dropCounting;
	if ((in.down & KEY_L) && !counting) tab = (tab + 4) % 5;
	if ((in.down & KEY_R) && !counting) tab = (tab + 1) % 5;
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
	}
	// (in OpenMW's order: what is worn and held first, then by kind and name)
	sortListed(w.game, index, [&](int i) {
		const InventoryItem& it = w.inventory[i];
		return ItemListed{ w.game.object(it.id), it.equipped, it.condition, it.charge };
	});
	gridShown.clear();
	for (int i : index)
	{
		const InventoryItem& it = w.inventory[i];
		cells.push_back(gridItem(w.game.object(it.id), it.id, it.count, it.equipped));
		gridShown.push_back(it.id);
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
	if (counting)
	{
		grid.selected = before;           // the D-pad picks the count now, not the item
		pick = -1;
	}
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

	// How many of a stack to drop (OpenMW's count dialog, starting at the whole stack): [-] Drop 5 [+] Cancel, left and
	// right one at a time, up and down ten; A drops them, B goes back to the item's buttons
	if (counting)
	{
		if (sel < 0)
		{
			dropCounting = false;
			return;
		}
		int have = w.inventory[sel].count;
		if (in.down & KEY_LEFT) dropCount -= 1;
		if (in.down & KEY_RIGHT) dropCount += 1;
		if (in.down & KEY_DOWN) dropCount -= 10;
		if (in.down & KEY_UP) dropCount += 10;
		bool set = false;
		if (uiButton(4, 208, 40, 28, "-", false))
			dropCount--;
		if (uiButton(212, 208, 40, 28, "+", false))
			dropCount++;
		dropCount = std::max(1, std::min(have, dropCount));
		if (uiButton(48, 208, 160, 28, w.game.gmst("sdrop", "Drop") + " " + std::to_string(dropCount), true))
			set = true;
		bool cancel = uiButton(256, 208, 60, 28, w.game.gmst("scancel", "Cancel"), false) || (in.down & KEY_B);
		uiConsume(KEY_A | KEY_B | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
		if (cancel)
		{
			dropCounting = false;
			playSound(-1, "Menu Click");
		}
		else if (set || aKey)
		{
			dropFromInventory(sel, dropCount);
			dropCounting = false;
			grid.selected = -1;
		}
		return;
	}
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
		if (labels[b] == w.game.gmst("sdrop", "Drop"))
		{
			// one goes at once; a stack asks how many first
			if (w.inventory[sel].count > 1)
			{
				dropCounting = true;
				dropCount = w.inventory[sel].count;
				playSound(-1, "Menu Click");
			}
			else
			{
				dropFromInventory(sel, 1);
				grid.selected = -1;
			}
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
			// its script holds PCSkipEquip 1: not read, only OnPCEquip set for the script to act (OpenMW's useItem: the
			// sealed Hlaalu treasury papers, Bloodmoon's Sovngarde book)
			if (float* skip = w.itemScriptLocal(id, "pcskipequip"); skip && *skip == 1.0f)
			{
				w.setItemScriptLocal(id, "onpcequip", 1.0f);
				return;
			}
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

// The inventory's Drop: that many of the stack go into the world (they can be picked up again)
void Session::dropFromInventory(int sel, int count)
{
	if (sel < 0 || sel >= (int)w.inventory.size() || count <= 0)
		return;
	// a conjured item can't go to the ground (OpenMW's DragAndDrop::drop): left there, it would outlast its spell
	if (isBoundItem(w.inventory[sel].id))
	{
		notify(w.game.gmst("sbarterdialog12", "You cannot drop summoned items!"));
		return;
	}
	InventoryItem it = w.inventory[sel];
	int n = std::min(count, it.count);
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
	// Where the crosshair points, when that is a surface within 200 units flat enough to stand it on (OpenMW's
	// placeObject / canPlaceObject: a ray from the camera, a face no steeper than 30 degrees): a table, a shelf, the
	// floor. Else at the player's feet (OpenMW's dropObjectOnGround)
	float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float cp = cosf(w.player.pitch);
	float end[3] = { eye[0] + sinf(w.player.yaw) * cp * 200.0f, eye[1] + cosf(w.player.yaw) * cp * 200.0f,
		eye[2] + sinf(w.player.pitch) * 200.0f };
	float best = 2.0f;
	for (LoadedCell* l : w.loaded)
	{
		float t;
		if (collisionRaycast(l->cell.collision, eye, end, &t) && t < best)
			best = t;
	}
	if (best <= 1.0f)
	{
		float hit[3] = { eye[0] + (end[0] - eye[0]) * best, eye[1] + (end[1] - eye[1]) * best, eye[2] + (end[2] - eye[2]) * best };
		// a flat face there: its top within a few units of the hit, found looking down (30 degrees at most); a wall
		// struck from the side has none so near
		float top = -1e9f;
		for (LoadedCell* l : w.loaded)
		{
			float z;
			if (collisionFloor(l->cell.collision, hit[0], hit[1], hit[2] + 4.0f, hit[2] - 8.0f, &z, 30.0f, 30.0f) && z > top)
				top = z;
		}
		if (top > -1e9f)
		{
			hit[2] = top;
			w.dropItem(it, hit, w.player.yaw);
			playSound(-1, "Item Misc Down");
			return;
		}
	}
	w.dropItemAt(it, w.player.feet, w.player.yaw, 50.0f);
	playSound(-1, "Item Misc Down");
}
