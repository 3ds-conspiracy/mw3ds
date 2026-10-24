// Bartering: the merchant's goods, offers and haggling
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// Bartering as OpenMW's trade window: what to buy and what to sell is put together first (A, or tapping the picked item
// again; a stack asks how many, 0 taking it back out of the deal), and one offer settles it. Only the balance is paid, by
// whichever side owes it: a sword sold for 5000 pays for one bought for 5000, whatever gold either side has
void Session::drawBarter()
{
	u32 keys = uiIn().down;
	bool counting = barterCounting;
	Ref& m = w.refs[barterRef];
	int gold = w.itemCount("gold_001");
	header(w.game.actors[m.actor].name, "Their " + std::to_string(m.gold) + "  You " + std::to_string(gold));

	// Buy from their goods or sell yours (L / R switch); the deal keeps both sides
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
		grid = UiGrid();
		playSound(-1, "Menu Click");
	}
	auto inDeal = [&](bool sold, int from, int index) {
		for (const BarterLine& l : barterDeal)
			if (l.sell == sold && l.from == from && l.index == index)
				return l.count;
		return 0;
	};

	// cells: (from, index) of each, and its price
	std::vector<std::pair<int, int>> where;
	std::vector<int> prices;
	std::vector<UiGridItem> cells;
	if (!barterSell)
		for (auto& g : merchantGoods(barterRef))
		{
			ContentItem good = merchantGood(g);
			const Object* o = w.game.object(good.second);
			if (!merchantTrades(barterRef, o))
				continue;
			where.push_back(g);
			prices.push_back(barterPrice(barterRef, o->value, true));
			int left = stockCount(good.first) - inDeal(false, g.first, g.second);
			UiGridItem c = gridItem(o, good.second, left, false);
			if (left <= 0)
				c.flags |= UIGRID_DIM;
			cells.push_back(c);
		}
	else
		for (size_t i = 0; i < w.inventory.size(); i++)
		{
			const Object* o = w.game.object(w.inventory[i].id);
			if (!merchantTrades(barterRef, o))
				continue;
			where.emplace_back(-1, (int)i);
			prices.push_back(barterPrice(barterRef, o->value, false));
			int left = w.inventory[i].count - inDeal(true, -1, (int)i);
			UiGridItem c = gridItem(o, w.inventory[i].id, left, w.inventory[i].equipped);
			if (left <= 0)
				c.flags |= UIGRID_DIM;
			cells.push_back(c);
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
	int sel = grid.selected >= 0 && grid.selected < (int)where.size() ? grid.selected : -1;
	if (sel < 0)
		barterCounting = counting = false;
	// How many of the picked item there are, and how many of them are in the deal
	int have = 0, dealt = 0;
	if (sel >= 0)
	{
		have = barterSell ? w.inventory[where[sel].second].count : stockCount(merchantGood(where[sel]).first);
		dealt = inDeal(barterSell, where[sel].first, where[sel].second);
	}
	if (counting)
	{
		// D-pad left / right: one fewer / more, L / R: ten
		if (keys & KEY_LEFT) barterCount--;
		if (keys & KEY_RIGHT) barterCount++;
		if (keys & KEY_L) barterCount -= 10;
		if (keys & KEY_R) barterCount += 10;
		barterCount = std::max(dealt > 0 ? 0 : 1, std::min(barterCount, have));   // (0 takes one already in the deal out)
	}
	int balance = barterBalance();
	if (sel >= 0)
	{
		const Object* o = w.game.object(barterSell ? w.inventory[where[sel].second].id : merchantGood(where[sel]).second);
		std::string info = std::to_string(prices[sel]) + " gold";
		if (counting)
			info = std::to_string(barterCount) + " of " + std::to_string(have) + ": " + std::to_string(prices[sel] * barterCount) + " gold";
		else if (dealt > 0)
			info += ", " + std::to_string(dealt) + " in the deal";
		itemInfo(o, 191, info);
		ContentItem e = barterSell ? ContentItem() : merchantGood(where[sel]);
		InventoryItem state = !barterSell ? InventoryItem{ lower(e.second), have, false, e.condition, e.soul, e.charge } : InventoryItem{};
		itemTip(o, barterSell ? &w.inventory[where[sel].second] : &state);
	}
	else if (!barterDeal.empty())
		uiTextCentered(160, 194, 0.42f, col::textDim, "Y / X haggle, tap the total to type one, SELECT offers");
	uiConsume(KEY_A | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
	// Haggling: Y / X (or - / +) lower or raise the offer 5% of the balance at a time, or a number is typed on the number
	// pad (the merchant may refuse)
	const UiInput& hin = uiIn();
	int price0 = abs(balance), offer = barterHaggled(price0);
	bool dealOn = !barterDeal.empty();
	auto nudge = [&](int dir) {
		// (in 5% steps of the balance from it, as Morrowind's; a typed offer goes to the nearest step first)
		float step = std::max(1.0f, price0 * 0.05f);
		int k = (int)roundf((offer - price0) / step) + dir;
		haggleOffer = std::max(0, price0 + (int)roundf(step * k));
		offer = haggleOffer;
	};
	if (dealOn && (hin.down & KEY_Y)) nudge(-1);
	if (dealOn && (hin.down & KEY_X)) nudge(1);
	auto setDealt = [&](int n) {
		for (size_t k = 0; k < barterDeal.size(); k++)
			if (barterDeal[k].sell == barterSell && barterDeal[k].from == where[sel].first && barterDeal[k].index == where[sel].second)
			{
				if (n > 0)
					barterDeal[k].count = n;
				else
					barterDeal.erase(barterDeal.begin() + k);
				haggleOffer = -1;                         // (a new deal: the offer is its balance again, as OpenMW's)
				return;
			}
		if (n > 0)
			barterDeal.push_back({ barterSell, where[sel].first, where[sel].second, n });
		haggleOffer = -1;
	};
	if (counting)
	{
		// [-]  Buy 5  [+]  Cancel: A sets how many go in the deal (0 takes them out), B (or Cancel) goes back to the grid
		std::string label = barterCount == 0 ? std::string("None")
			: (barterSell ? w.game.gmstLabel("ssell", "Sell") : w.game.gmstLabel("sbuy", "Buy")) + " " + std::to_string(barterCount);
		bool set = false;
		if (uiButton(4, 208, 40, 28, "-", false))
			barterCount = std::max(dealt > 0 ? 0 : 1, barterCount - 1);
		if (uiButton(48, 208, 160, 28, label, true))
			set = true;
		if (uiButton(212, 208, 40, 28, "+", false))
			barterCount = std::min(have, barterCount + 1);
		if (uiButton(256, 208, 60, 28, w.game.gmst("scancel", "Cancel"), false) || (hin.down & KEY_B))
		{
			barterCounting = false;
			playSound(-1, "Menu Click");
			return;
		}
		if (set || aKey)
		{
			setDealt(barterCount);
			barterCounting = false;
			playSound(-1, "Menu Click");
		}
		return;
	}
	// The balance, as OpenMW's TOTAL COST / TOTAL SOLD with its - / + haggle buttons and the number typed into it, then
	// Offer, Clear, Done
	if (uiButton(4, 208, 22, 28, "-", false, dealOn))
		nudge(-1);
	if (uiButton(114, 208, 22, 28, "+", false, dealOn))
		nudge(1);
	if (uiButton(28, 208, 84, 28, "", false, dealOn))
	{
		wantText = TEXT_OFFER;
		playSound(-1, "Menu Click");
	}
	// (the label and the number fit inside the box's frame, 3 high top and bottom)
	float labelH = uiLineHeight(0.28f) * 0.8f, numH = uiLineHeight(0.55f), top = 208 + (28 - labelH - numH) / 2;
	uiTextCentered(70, top, 0.28f, col::textDim, balance < 0 ? w.game.gmst("stotalcost", "TOTAL COST") : w.game.gmst("stotalsold", "TOTAL SOLD"));
	uiTextCentered(70, top + labelH, 0.55f, offer != price0 ? col::textOver : col::text, std::to_string(offer));
	bool offerB = uiButton(140, 208, 58, 28, w.game.gmst("soffer", "Offer"));
	bool clearB = uiButton(202, 208, 56, 28, "Clear");
	bool doneB = uiButton(262, 208, 54, 28, w.game.gmst("sdone", "Done"));
	int b = offerB ? 0 : clearB ? 1 : doneB ? 2 : -1;
	if (b == 0 || (hin.down & KEY_SELECT))
	{
		if (dealOn)
			barterOffer();
		else
			notify(w.game.gmst("sbarterdialog11", "You need to trade items before making an offer."));
		return;
	}
	if (b == 1)
	{
		barterDeal.clear();
		haggleOffer = -1;
		playSound(-1, "Menu Click");
		return;
	}
	// A, or tapping the selected item again, puts it in the deal (or takes it out); a stack asks how many
	if (sel >= 0 && pick >= 0 && (aKey || pick == before))
	{
		if (have > 1)
		{
			barterCounting = true;
			barterCount = dealt > 0 ? dealt : 1;
		}
		else
			setDealt(dealt > 0 ? 0 : 1);
		playSound(-1, "Menu Click");
		return;
	}
	if (b == 2 || (hin.down & KEY_B))
	{
		barterRef = -1;
		barterDeal.clear();
		screen = SCR_DIALOGUE;
		playSound(-1, "Menu Click");
	}
}

// What the deal is worth to the player: the price of what they sell less the price of what they buy (a line whose stack
// has gone is dropped, one with fewer left than it holds comes down to what is left)
int Session::barterBalance()
{
	int balance = 0;
	for (size_t k = 0; k < barterDeal.size();)
	{
		const BarterLine& l = barterDeal[k];
		const Object* o = nullptr;
		int have = 0;
		if (l.sell && l.index >= 0 && l.index < (int)w.inventory.size())
		{
			o = w.game.object(w.inventory[l.index].id);
			have = w.inventory[l.index].count;
		}
		else if (!l.sell && l.from >= 0 && l.from < (int)w.refs.size() && l.index < (int)w.refs[l.from].contents.size())
		{
			ContentItem it = merchantGood({ l.from, l.index });
			o = w.game.object(it.second);
			have = stockCount(it.first);
		}
		if (!o || have <= 0)
		{
			barterDeal.erase(barterDeal.begin() + k);
			continue;
		}
		barterDeal[k].count = std::min(barterDeal[k].count, have);
		balance += (barterDeal[k].sell ? 1 : -1) * barterPrice(barterRef, o->value, !barterDeal[k].sell) * barterDeal[k].count;
		k++;
	}
	return balance;
}

// What the player offers for a balance of price0: the balance itself until it is haggled or typed
int Session::barterHaggled(int price0)
{
	return haggleOffer >= 0 ? haggleOffer : price0;
}

// The Offer button: the deal at the haggled balance (OpenMW's TradeWindow::onOfferButtonClicked). Only the balance has to
// be there, with whichever side pays it; one of the merchant's own stolen goods refuses the lot, and a haggled offer may be
// refused. Then everything changes hands and the balance moves once
bool Session::barterOffer()
{
	Ref& m = w.refs[barterRef];
	int balance = barterBalance();
	if (barterDeal.empty())
		return false;
	int price0 = abs(balance), offer = barterHaggled(price0);
	bool playerPays = balance < 0;
	for (const BarterLine& l : barterDeal)
	{
		if (!l.sell || m.actor < 0)
			continue;
		auto sf = w.stolenFrom.find(w.inventory[l.index].id);
		if (sf == w.stolenFrom.end())
			continue;
		std::string fac = lower(w.game.actors[m.actor].faction);
		for (auto& o : sf->second)
			if (o == m.idLower || (!fac.empty() && o == fac))
			{
				notify("That was stolen from me! I won't buy it back.");
				return false;
			}
	}
	if (playerPays ? w.itemCount("gold_001") < offer : m.gold < offer)
	{
		notify(playerPays ? w.game.gmst("sbarterdialog1", "You don't have enough gold.")
			: w.game.gmst("sbarterdialog2", "The trader can't afford that."));
		logf("barter: refused: %s (balance %d, offer %d)", playerPays ? "not enough gold" : "the merchant can't afford it", balance, offer);
		return false;
	}
	if (offer != price0 && price0 > 0)
	{
		bool side = barterSell;
		barterSell = !playerPays;                     // (judged as a sale when the merchant pays, a purchase when the player does)
		bool ok = haggleAccepted(price0, offer);
		barterSell = side;
		if (!ok)
		{
			notify(w.game.gmst("snotifymessage9", "Your offer has been refused."));
			playSound(-1, "Menu Click");
			return false;
		}
	}
	// (what is sold first, the last of the inventory first, then what is bought, the last of each holder's stacks first:
	// the indices of the rest hold)
	std::vector<BarterLine> lines = barterDeal;
	std::sort(lines.begin(), lines.end(), [](const BarterLine& a, const BarterLine& b) {
		if (a.sell != b.sell)
			return a.sell;
		if (a.from != b.from)
			return a.from < b.from;
		return a.index > b.index;
	});
	for (const BarterLine& l : lines)
		barterTrade(l.sell, l.index, 0, l.from, l.count, false);
	if (playerPays)
	{
		w.removeItem("gold_001", offer);
		m.gold += offer;
	}
	else
	{
		w.addItem("gold_001", offer);
		m.gold -= offer;
	}
	playSound(-1, playerPays ? "Item Gold Down" : "Item Gold Up");
	logf("barter: deal of %d lines, the player %s %d (balance %d)", (int)lines.size(), playerPays ? "pays" : "gets", offer, balance);
	barterDeal.clear();
	haggleOffer = -1;
	// A deal done: the speaker's thanks join the talk (OpenMW's onTradeComplete)
	dlg.history.push_back({ "", w.game.gmst("sbarterdialog5", "Thank you.") });
	dlg.revision++;
	return true;
}

void Session::openBarter(int ref)
{
	barterRef = ref;
	barterSell = false;
	barterDeal.clear();
	barterCounting = false;
	haggleOffer = -1;
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
		if (c.recType != REC_CONT || c.contents.empty() || w.placeOf(i) != place || lower(c.owner) != m.idLower)
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

bool Session::barterTrade(bool sell, int index, int price, int from, int count, bool thanks)
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
				w.itemLeft(id);
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
	if (thanks)
	{
		dlg.history.push_back({ "", w.game.gmst("sbarterdialog5", "Thank you.") });
		dlg.revision++;
	}
	return true;
}
