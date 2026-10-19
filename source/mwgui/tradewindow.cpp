// Bartering: the merchant's goods, offers and haggling
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// Buying and selling at the merchant's price: a stack asks how many first (as OpenMW's count dialog)
void Session::drawBarter()
{
	u32 keys = uiIn().down;
	bool counting = barterCount > 0;
	Ref& m = w.refs[barterRef];
	int gold = w.itemCount("gold_001");
	header(w.game.actors[m.actor].name, "Their " + std::to_string(m.gold) + "  You " + std::to_string(gold));

	// Buy from their goods or sell yours (L / R switch)
	bool sell = barterSell;
	if (keys & (KEY_L | KEY_R) && !counting)
		sell = !sell;
	if (uiButton(4, 24, 154, 18, w.game.gmst("sbuy", "Buy"), !sell) && !counting)
		sell = false;
	if (uiButton(162, 24, 154, 18, w.game.gmst("ssell", "Sell"), sell) && !counting)
		sell = true;
	if (sell != barterSell)
	{
		barterSell = sell;
		barterCount = 0;
		grid = UiGrid();
		playSound(-1, "Menu Click");
	}

	// cells: (index into merchant contents or player inventory, price)
	std::vector<std::pair<int, int>> items;
	std::vector<UiGridItem> cells;
	std::vector<std::pair<int, int>> goods;       // buying: (reference holding it, index in its contents)
	if (!barterSell)
		for (auto& g : merchantGoods(barterRef))
		{
			ContentItem good = merchantGood(g);
			const Object* o = w.game.object(good.second);
			if (!merchantTrades(barterRef, o))
				continue;
			int price = barterPrice(barterRef, o->value, true);
			items.emplace_back((int)goods.size(), price);
			goods.push_back(g);
			UiGridItem c = gridItem(o, good.second, stockCount(good.first), false);
			if (price > gold)
				c.flags |= UIGRID_DIM;
			cells.push_back(c);
		}
	else
		for (size_t i = 0; i < w.inventory.size(); i++)
		{
			const Object* o = w.game.object(w.inventory[i].id);
			if (!merchantTrades(barterRef, o))
				continue;
			int price = barterPrice(barterRef, o->value, false);
			items.emplace_back(i, price);
			cells.push_back(gridItem(o, w.inventory[i].id, w.inventory[i].count, w.inventory[i].equipped));
		}
	if (cells.empty())
		uiTextCentered(160, 110, 0.5f, col::textDim, barterSell ? "Nothing they would buy" : "Nothing for sale");
	int before = grid.selected;
	bool aKey = keys & KEY_A;
	int pick = uiItemGrid(grid, 4, 45, 312, 144, cells);
	if (counting)
	{
		grid.selected = before;           // the D-pad picks the count now, not the item
		pick = -1;
	}
	int sel = grid.selected >= 0 && grid.selected < (int)items.size() ? grid.selected : -1;
	// How many of the picked item there are
	int have = 0;
	if (sel >= 0)
		have = barterSell ? w.inventory[items[sel].first].count
			: stockCount(merchantGood(goods[items[sel].first]).first);
	if (!counting || sel < 0)
		barterCount = 0;
	else
	{
		// D-pad left / right: one fewer / more, L / R: ten
		if (keys & KEY_LEFT) barterCount--;
		if (keys & KEY_RIGHT) barterCount++;
		if (keys & KEY_L) barterCount -= 10;
		if (keys & KEY_R) barterCount += 10;
		barterCount = std::max(1, std::min(barterCount, have));
	}
	int total = sel >= 0 ? items[sel].second * std::max(1, barterCount) : 0;
	if (sel >= 0)
	{
		const Object* o = w.game.object(barterSell ? w.inventory[items[sel].first].id
			: merchantGood(goods[items[sel].first]).second);
		itemInfo(o, 191, counting ? std::to_string(barterCount) + " of " + std::to_string(have) + ": " + std::to_string(total) + " gold"
			: std::to_string(items[sel].second) + " gold");
		ContentItem e = barterSell ? ContentItem() : merchantGood(goods[items[sel].first]);
		InventoryItem state = !barterSell ? InventoryItem{ lower(e.second), have, false, e.condition, e.soul, e.charge } : InventoryItem{};
		itemTip(o, barterSell ? &w.inventory[items[sel].first] : &state);
	}
	uiConsume(KEY_A | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
	int focusNone = -1;
	int& focus = focusNone;
	std::string action = barterSell ? w.game.gmstLabel("ssell", "Sell") : w.game.gmstLabel("sbuy", "Buy");
	// Haggling: Y / X lower or raise the offer 5% of the price at a time (the merchant may refuse)
	const UiInput& hin = uiIn();
	if (hin.down & KEY_Y) haggle--;
	if (hin.down & KEY_X) haggle++;
	haggle = haggle < -19 ? -19 : haggle > 19 ? 19 : haggle;
	int offer = 0, price0 = 0;
	if (sel >= 0)
	{
		int price = counting ? total : items[sel].second;       // (the whole lot while choosing a count)
		price0 = price;
		offer = std::max(1, price + (int)roundf(price * 0.05f * haggle));
		// "Buy (45 of 50g)" while haggling; Y / X change it
		if (counting)
			action += " " + std::to_string(barterCount);
		action += offer != price ? " (" + std::to_string(offer) + " of " + std::to_string(price) + "g)"
			: " (" + std::to_string(price) + (counting ? "g)" : "g, Y/X haggle)");
	}
	int b = -1;
	if (!counting)
		b = buttonRow({ action, w.game.gmst("sdone", "Done") }, focus);
	else
	{
		// [-]  Buy 5 (50g)  [+]  Cancel: A buys, B (or Cancel) goes back to the grid
		if (uiButton(4, 208, 40, 28, "-", false))
			barterCount = std::max(1, barterCount - 1);
		if (uiButton(48, 208, 160, 28, action, true))
			b = 0;
		if (uiButton(212, 208, 40, 28, "+", false))
			barterCount = std::min(have, barterCount + 1);
		if (uiButton(256, 208, 60, 28, w.game.gmst("scancel", "Cancel"), false) || (hin.down & KEY_B))
		{
			barterCount = 0;
			playSound(-1, "Menu Click");
			return;
		}
	}
	// A, or tapping the selected item again, buys / sells it
	if (sel >= 0 && pick >= 0 && (aKey || pick == before))
		b = 0;
	if (counting && aKey)
		b = 0;
	// A stack asks how many first
	if (b == 0 && sel >= 0 && !counting && have > 1)
	{
		barterCount = 1;
		haggle = 0;
		playSound(-1, "Menu Click");
		return;
	}
	bool refused = false;
	if (b == 0 && sel >= 0 && offer != price0)
	{
		refused = !haggleAccepted(price0, offer);
		if (refused)
		{
			notify(w.game.gmst("snotifymessage9", "Your offer has been refused."));
			playSound(-1, "Menu Click");
		}
	}
	if (b == 0 && sel >= 0 && !refused)
	{
		int n = std::max(1, barterCount);
		bool done = barterSell ? barterTrade(true, items[sel].first, offer, -1, n)
			: barterTrade(false, goods[items[sel].first].second, offer, goods[items[sel].first].first, n);
		if (done)
			barterCount = 0;
	}
	else if (!counting && (b == 1 || (hin.down & KEY_B)))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
		playSound(-1, "Menu Click");
	}
}

void Session::openBarter(int ref)
{
	barterRef = ref;
	barterSell = false;
	list2 = UiList();
	grid = UiGrid();
	screen = SCR_BARTER;
}

// count items bought (index into the merchant's goods) or sold (into the inventory) for price in all
// What a merchant sells: their own goods, what lies in the containers they own in the same cell (Morrowind keeps
// most shop stock in the shopkeeper's chests) and the loose items they own there, the books on their shelves (OpenMW's
// TradeWindow::setPtr: the actor, getContainersOwnedBy, getItemsOwnedBy): (reference, index into its contents; -1:
// the reference is the item itself)
std::vector<std::pair<int, int>> Session::merchantGoods(int ref)
{
	std::vector<std::pair<int, int>> out;
	const Ref& m = w.refs[ref];
	for (size_t i = 0; i < m.contents.size(); i++)
		out.emplace_back(ref, (int)i);
	int place = w.placeOf(ref);
	w.forLoadedRefs([&](int i) {
		const Ref& c = w.refs[i];
		if (c.type != "CONT" || c.contents.empty() || w.placeOf(i) != place || lower(c.owner) != m.idLower)
			return;
		for (size_t k = 0; k < c.contents.size(); k++)
			out.emplace_back(i, (int)k);
	});
	// the loose items they own in the cell: not taken, and not an object of another kind that merely has an owner
	static const char* kItemTypes[] = { "BOOK", "ALCH", "INGR", "WEAP", "ARMO", "CLOT", "MISC", "LOCK", "PROB", "REPA", "APPA" };
	w.forLoadedRefs([&](int i) {
		const Ref& c = w.refs[i];
		if (i == ref || !c.visible() || c.owner.empty() || w.placeOf(i) != place || lower(c.owner) != m.idLower)
			return;
		for (const char* t : kItemTypes)
			if (c.type == t)
			{
				out.emplace_back(i, -1);
				return;
			}
	});
	return out;
}

ContentItem Session::merchantGood(const std::pair<int, int>& g)
{
	const Ref& h = w.refs[g.first];
	if (g.second >= 0)
		return h.contents[g.second];
	ContentItem it(h.count > 0 ? h.count : 1, h.idLower);
	it.condition = h.dropState.condition;
	it.soul = h.dropState.soul;
	it.charge = h.dropState.charge;
	return it;
}

bool Session::barterTrade(bool sell, int index, int price, int from, int count)
{
	Ref& m = w.refs[barterRef];
	Ref& holder = w.refs[from >= 0 ? from : barterRef];
	bool can = sell ? m.gold >= price : w.itemCount("gold_001") >= price;
	bool barterSell = sell;
	int sel = 0;
	std::vector<std::pair<int, int>> items = { { index, price } };
	{
		if (!can)
		{
			notify(barterSell ? "The merchant can't afford that." : "You don't have enough gold.");
			logf("barter: refused: %s", barterSell ? "the merchant can't afford it" : "not enough gold");
			return false;
		}
		else if (!barterSell && items[sel].first < 0)
		{
			// a loose item of theirs from the shop floor: it leaves the cell with the buyer, no theft (OpenMW's
			// ContainerItemModel::removeItem takes it from the world)
			w.removeItem("gold_001", price);
			m.gold += price;
			if (count >= std::max(1, holder.count))
				w.pickUp(from);
			else
			{
				w.addItem(holder.idLower, count);
				holder.count -= count;
			}
			playSound(-1, "Item Gold Down");
		}
		else if (!barterSell)
		{
			auto& it = holder.contents[items[sel].first];
			w.removeItem("gold_001", price);
			m.gold += price;
			w.takeStack(it, count);             // (a worn or part-charged one, or a filled gem, as it is)
			// (a restocking quantity, a negative count, is never used up by a sale: OpenMW's removeItem)
			if (it.first > 0)
			{
				it.first -= count;
				if (it.first <= 0)
					holder.contents.erase(holder.contents.begin() + items[sel].first);
			}
			playSound(-1, "Item Gold Down");
			// (Mercantile only rises with a haggled offer, in haggleAccepted: as in OpenMW)
		}
		else if ([&] {
					// Their own stolen goods: they won't buy them back
					auto sf = w.stolenFrom.find(w.inventory[items[sel].first].id);
					if (sf == w.stolenFrom.end() || m.actor < 0)
						return false;
					std::string fac = lower(w.game.actors[m.actor].faction);
					for (auto& o : sf->second)
						if (o == m.idLower || (!fac.empty() && o == fac))
							return true;
					return false;
				}())
		{
			notify("That was stolen from me! I won't buy it back.");
			return false;
		}
		else
		{
			InventoryItem& it = w.inventory[items[sel].first];
			std::string id = it.id;
			InventoryItem sold = it;            // the picked stack goes, its wear, soul and charge with it
			sold.count = count;
			sold.equipped = false;
			if (sold.condition < 0 && sold.soul.empty() && sold.charge < 0.0f)
				w.removeItem(id, count);
			else
			{
				bool worn = it.equipped && it.count <= count;
				it.count -= count;
				if (it.count <= 0)
					w.inventory.erase(w.inventory.begin() + items[sel].first);
				if (worn)
					w.refreshStats();
				if (w.itemCount(id) == 0)
					for (auto& sc : w.scripts)
						if (sc.item == id)
							sc.running = false;
			}
			w.addItem("gold_001", price);
			m.gold -= price;
			if (sold.condition < 0 && sold.soul.empty() && sold.charge < 0.0f)
			{
				bool stacked = false;
				for (auto& c : m.contents)
					if (lower(c.second) == id && c.plain())
					{
						c.first += c.first < 0 ? -count : count;
						stacked = true;
						break;
					}
				if (!stacked)
					m.contents.emplace_back(count, id);
			}
			else
				w.refAddStack(m, sold);
			playSound(-1, "Item Gold Up");
		}
	}
	// A deal done: the speaker's thanks join the talk (OpenMW's onTradeComplete)
	dlg.history.push_back({ "", w.game.gmst("sbarterdialog5", "Thank you.") });
	dlg.revision++;
	return true;
}
