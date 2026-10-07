// Talking and the services NPCs offer: dialogue, barter, spells, training, travel, arrest
#include <cmath>
#include <cstdio>
#include <cstring>
#include "audio.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screens.h"
#include "session.h"

void Session::layoutDialogue(float width)
{
	words.clear();
	linkTopics = dlg.topics;
	std::vector<std::string> linkNames;
	for (auto& t : linkTopics)
		linkNames.push_back(lower(t));
	float lh = uiLineHeight(0.5f), space = uiTextWidth(" ", 0.5f);
	float y = 0.0f, lastEntryY = 0.0f;

	auto addText = [&](const std::string& text, u32 color, int forcedAction) {
		std::string l = lower(text);
		std::vector<int> linkAt(text.size(), -1);
		if (forcedAction == -1)
			for (auto& m : dialogueFindKeywords(text, linkNames))
				for (size_t k = m.begin; k < m.end; k++)
					linkAt[k] = m.keyword;
		float x = 0.0f;
		size_t i = 0;
		while (i < text.size())
		{
			if (text[i] == '\n')
			{
				x = 0.0f;
				y += lh;
				i++;
				continue;
			}
			if (text[i] == ' ')
			{
				i++;
				continue;
			}
			size_t e = i;
			while (e < text.size() && text[e] != ' ' && text[e] != '\n')
				e++;
			std::string word = text.substr(i, e - i);
			float ww = uiTextWidth(word, 0.5f);
			if (x > 0.0f && x + ww > width)
			{
				x = 0.0f;
				y += lh;
			}
			int action = forcedAction != -1 ? forcedAction : linkAt[i];
			bool link = action >= 0 || (action <= -2 && action > -1000);
			words.push_back({ word, x, y, ww, link ? col::link : color, action });
			x += ww + space;
			i = e;
		}
		y += lh;
	};

	for (auto& e : dlg.history)
	{
		lastEntryY = y;
		if (!e.header.empty())
			addText(e.header, col::header, -1000);
		addText(e.text, col::text, -1);
		y += lh * 0.5f;
	}
	for (size_t i = 0; i < dlg.choices.size(); i++)
		addText(std::to_string(i + 1) + ". " + dlg.choices[i].first, col::link, -2 - (int)i);
	wordsHeight = y;

	// Show the newest response from its first line
	dlgScroll = lastEntryY;
	wordsRevision = dlg.revision;
}

void Session::drawDialogue()
{
	if (!dlg.open)
	{
		closeScreen();
		return;
	}
	const Ref& speaker = w.refs[dlg.ref];
	header(speaker.actor >= 0 ? w.game.actors[speaker.actor].name : speaker.id);

	const float tx = 4, ty = 24, tw = 206, th = 212;
	if (wordsRevision != dlg.revision)
		layoutDialogue(tw - 6);
	const UiInput& in = uiIn();
	if (in.touching && in.touchX < tx + tw && in.touchY > ty)
		dlgScroll -= in.dragDY;
	if (in.held & KEY_R) dlgScroll += 3;
	if (in.held & KEY_L) dlgScroll -= 3;
	dlgScroll += uiStickScroll();
	dlgScroll = fmaxf(0.0f, fminf(dlgScroll, fmaxf(0.0f, wordsHeight - th)));

	uiPanel(tx - 2, ty - 2, tw + 2, th + 2);
	int clicked = -1000;
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (ty + th)), (u32)(320 - (tx + tw)), (u32)(240 - ty), (u32)(320 - tx));
	for (auto& wd : words)
	{
		float y = ty + 2 + wd.y - dlgScroll;
		if (y + 16 < ty || y > ty + th)
			continue;
		uiText(tx + 3 + wd.x, y, 0.5f, wd.color, wd.text);
		if (wd.action != -1 && wd.action != -1000 && uiHit(tx + 3 + wd.x, y, wd.w, 16))
			clicked = wd.action;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

	bool choosing = !dlg.choices.empty();
	std::vector<std::string> rows = dlg.goodbye || choosing ? std::vector<std::string>() : dlg.topics;
	// Services sit between the topics and Goodbye
	bool merchant = isMerchant(dlg.ref);
	bool travels = speaker.actor >= 0 && !w.game.actors[speaker.actor].travel.empty();
	bool sells = speaker.actor >= 0 && !w.game.actors[speaker.actor].spells.empty();
	bool trains = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x4000);
	bool persuades = speaker.type == "NPC_";
	bool makesSpells = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x8000);
	bool enchants = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x10000);
	bool repairs = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x20000);
	int services = merchant + travels + sells + trains + persuades + makesSpells + enchants + repairs;
	int pick = uiWrapList(list, 214, 24, 104, 180 - 26 * services, rows, 0.38f, !choosing);
	float sy = 206 - 26 * services;
	if (persuades && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("spersuasion", "Persuasion")))
	{
		list2 = UiList();
		screen = SCR_PERSUADE;
		playSound(-1, "Menu Click");
		return;
	}
	if (persuades)
		sy += 26;
	if (merchant && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("sbarter", "Barter"))
		&& !serviceRefused())
	{
		openBarter(dlg.ref);
		playSound(-1, "Menu Click");
		return;
	}
	if (merchant)
		sy += 26;
	if (travels && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("stravel", "Travel"))
		&& !serviceRefused())
	{
		barterRef = dlg.ref;
		list2 = UiList();
		screen = SCR_TRAVEL;
		playSound(-1, "Menu Click");
		return;
	}
	if (travels)
		sy += 26;
	if (sells && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("sspells", "Spells"))
		&& !serviceRefused())
	{
		barterRef = dlg.ref;
		list2 = UiList();
		screen = SCR_SPELLS;
		playSound(-1, "Menu Click");
		return;
	}
	if (sells)
		sy += 26;
	if (trains && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("strain", "Training"))
		&& !serviceRefused())
	{
		barterRef = dlg.ref;
		list2 = UiList();
		screen = SCR_TRAINING;
		playSound(-1, "Menu Click");
		return;
	}
	if (trains)
		sy += 26;
	if (makesSpells && !choosing && !dlg.goodbye
		&& uiButton(216, sy, 100, 24, w.game.gmst("sspellmakingmenutitle", "Spellmaking")) && !serviceRefused())
	{
		list2 = UiList();
		makeEffects.clear();
		makeSel = -1;
		barterRef = dlg.ref;               // (the price is their barter offer)
		screen = SCR_SPELLMAKE;
		playSound(-1, "Menu Click");
		return;
	}
	if (makesSpells)
		sy += 26;
	if (enchants && !choosing && !dlg.goodbye
		&& uiButton(216, sy, 100, 24, w.game.gmst("senchanting", "Enchanting")) && !serviceRefused())
	{
		openEnchanting(dlg.ref);
		playSound(-1, "Menu Click");
		return;
	}
	if (enchants)
		sy += 26;
	if (repairs && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("srepair", "Repair"))
		&& !serviceRefused())
	{
		openRepair(dlg.ref, -1);
		playSound(-1, "Menu Click");
		return;
	}
	if (repairs)
		sy += 26;
	if (choosing)
	{
		// choices: tap them in the text, or press 1-3 style with D-pad + A
		std::vector<std::string> texts;
		for (auto& ch : dlg.choices)
			texts.push_back(ch.first);
		static UiList choiceList;
		if (choiceList.selected < 0 || choiceList.selected >= (int)texts.size())
			choiceList.selected = 0;
		int c = uiWrapList(choiceList, 214, 24, 104, 180, texts, 0.38f, true);
		focus = choiceList.selected;
		if (c >= 0)
			clicked = -2 - c;
	}
	if (pick >= 0 && pick < (int)rows.size())
		clicked = -3000 - pick;
	bool bye = uiButton(216, 208, 100, 28, w.game.gmst("sgoodbye", "Goodbye"), false, !choosing) ||
		(!choosing && (in.down & KEY_B));

	if (clicked <= -3000)
	{
		playSound(-1, "Menu Click");
		dialogueTopic(dlg, w, *this, rows[-3000 - clicked]);
	}
	else if (clicked >= 0 && clicked < (int)linkTopics.size() && !choosing && !dlg.goodbye)
	{
		playSound(-1, "Menu Click");
		dialogueTopic(dlg, w, *this, linkTopics[clicked]);
	}
	else if (clicked <= -2 && clicked > -1000 && choosing)
	{
		int i = -2 - clicked;
		playSound(-1, "Menu Click");
		focus = 0;
		dialogueChoose(dlg, w, *this, dlg.choices[i].second);
	}
	else if (bye)
		closeScreen();
}

// ---- persuasion

bool Session::serviceRefused()
{
	if (!dialogueReact(dlg, w, *this, "Service Refusal"))
		return false;
	wordsRevision = -1;
	return true;
}

// Admire / intimidate / taunt / bribe, then back to the talk with their reaction
void Session::drawPersuade()
{
	const Ref& npc = w.refs[dlg.ref];
	header(w.game.gmst("spersuasion", "Persuasion"), "Disposition " + std::to_string(w.disposition(dlg.ref)));
	int gold = w.itemCount("gold_001");
	static const char* gm[6] = { "sadmire", "sintimidate", "staunt", "sbribe 10 gold", "sbribe 100 gold", "sbribe 1000 gold" };
	static const char* fallback[6] = { "Admire", "Intimidate", "Taunt", "Bribe 10 Gold", "Bribe 100 Gold", "Bribe 1000 Gold" };
	static const int cost[6] = { 0, 0, 0, 10, 100, 1000 };
	for (int k = 0; k < 6; k++)
		if (uiButton(60, 30 + k * 28, 200, 24, w.game.gmst(gm[k], fallback[k]), false, cost[k] <= gold) && cost[k] <= gold)
		{
			playSound(-1, "Menu Click");
			persuade(k);
			return;
		}
	uiText(8, 202, 0.42f, col::textDim, "Gold " + std::to_string(gold));
	if (uiButton(216, 208, 100, 28, w.game.gmst("sback", "Back")) || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		screen = SCR_DIALOGUE;
	}
}

// Morrowind's persuasion (OpenMW's getPersuasionDispositionChange): ratings from Speechcraft / Mercantile,
// Personality, Luck, reputation and level, the player's against the NPC's, scaled by how far their disposition is
// from 50. The result is a temporary change (gone at Goodbye) and a permanent one; Intimidate and Taunt also
// move Fight and Flee, a bribe costs its gold only when it works, and the NPC keeps it
void Session::persuade(int kind)
{
	Ref& npc = w.refs[dlg.ref];
	if (npc.actor < 0)
		return;
	const ActorDef& def = w.game.actors[npc.actor];
	auto g = [&](const char* name, float fb) { return w.game.gmstf(name, fb); };
	int disp = w.disposition(dlg.ref);     // what they feel now
	float t = persuadeChance(dlg.ref, kind);              // (formulas.cpp)
	float minChange = g("iperminchange", 5.0f), dieMult = g("fperdierollmult", 0.15f), tempMult = g("fpertempmult", 1.0f);
	int roll = persuadeRoll >= 0 ? persuadeRoll : rand() % 100;
	bool ok = roll <= t;
	float x = 0.0f, y = 0.0f;
	int fight = npc.fight >= 0 ? npc.fight : def.fight;
	int flee = npc.flee >= 0 ? npc.flee : def.flee;
	if (kind == 0)
	{
		float c = floorf(dieMult * (t - roll));
		x = ok ? fmaxf(minChange, c) : c;
	}
	else if (kind == 1)
	{
		float r = roll != (int)t ? floorf(t - roll) : 1.0f;
		if (ok)
		{
			float s = floorf(r * dieMult * tempMult);
			flee = std::max(0, std::min(100, flee + (int)fmaxf(minChange, s)));
			fight = std::max(0, std::min(100, fight + (int)fminf(-minChange, -s)));
		}
		float c = -fabsf(floorf(r * dieMult));
		if (ok)
		{
			if (fabsf(c) < minChange)
				y = x = minChange;
			else
			{
				x = -floorf(c * tempMult);
				y = c;
			}
		}
		else
		{
			x = floorf(c * tempMult);
			y = c;
		}
	}
	else if (kind == 2)
	{
		float c = fabsf(floorf(t - roll));
		if (ok)
		{
			float s = c * dieMult * tempMult;
			flee = std::max(0, std::min(100, flee + std::min(-(int)minChange, (int)-s)));
			fight = std::max(0, std::min(100, fight + std::max((int)minChange, (int)s)));
		}
		x = floorf(-c * dieMult);
		if (ok && fabsf(x) < minChange)
			x = -minChange;
	}
	else
	{
		float c = floorf((t - roll) * dieMult);
		x = ok ? fmaxf(minChange, c) : c;
		static const int bribe[3] = { 10, 100, 1000 };
		if (ok)
		{
			w.removeItem("gold_001", bribe[kind - 3]);
			npc.gold += bribe[kind - 3];
		}
	}
	int temp = (int)(kind == 1 ? x : x * tempMult);
	temp = std::max(-disp, std::min(100 - disp, temp));
	int perm = kind == 1 ? (ok ? -(int)(temp / tempMult) : (int)y) : (int)(temp / tempMult);
	npc.fight = fight;
	npc.flee = flee;
	dialoguePersuaded(dlg, w, temp, perm);
	useSkill(25, ok ? 0 : 1);             // Speechcraft: a success, or (less) a failure
	logf("persuade: %d on %s, roll %d, %s, disposition %d -> %d (temp %d, perm %d), fight %d", kind, npc.id.c_str(), roll,
		ok ? "success" : "fail", disp, w.disposition(dlg.ref), temp, perm, fight);
	static const char* topics[6][2] = { { "Admire Fail", "Admire Success" }, { "Intimidate Fail", "Intimidate Success" },
		{ "Taunt Fail", "Taunt Success" }, { "Bribe Fail", "Bribe Success" }, { "Bribe Fail", "Bribe Success" },
		{ "Bribe Fail", "Bribe Success" } };
	dialogueReact(dlg, w, *this, topics[kind][ok]);
	wordsRevision = -1;
	screen = SCR_DIALOGUE;
}

// ---- items

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
			const Ref& holder = w.refs[g.first];
			const Object* o = w.game.object(holder.contents[g.second].second);
			if (!merchantTrades(barterRef, o))
				continue;
			int price = barterPrice(barterRef, o->value, true);
			items.emplace_back((int)goods.size(), price);
			goods.push_back(g);
			UiGridItem c = gridItem(o, holder.contents[g.second].second, stockCount(holder.contents[g.second].first), false);
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
			: stockCount(w.refs[goods[items[sel].first].first].contents[goods[items[sel].first].second].first);
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
			: w.refs[goods[items[sel].first].first].contents[goods[items[sel].first].second].second);
		itemInfo(o, 191, counting ? std::to_string(barterCount) + " of " + std::to_string(have) + ": " + std::to_string(total) + " gold"
			: std::to_string(items[sel].second) + " gold");
		const ContentItem* e = barterSell ? nullptr : &w.refs[goods[items[sel].first].first].contents[goods[items[sel].first].second];
		InventoryItem state = e ? InventoryItem{ lower(e->second), have, false, e->condition, e->soul, e->charge } : InventoryItem{};
		itemTip(o, barterSell ? &w.inventory[items[sel].first] : &state);
	}
	uiConsume(KEY_A | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
	int focusNone = -1;
	int& focus = focusNone;
	std::string action = barterSell ? "Sell" : "Buy";
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
// What a merchant sells: their own goods and what lies in the containers they own in the same cell
// (Morrowind keeps most shop stock in the shopkeeper's chests): (reference, index into its contents)
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
	return out;
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

// Spell merchants: fSpellValueMult x spell cost at their barter price
void Session::drawSpells()
{
	const Ref& r = w.refs[barterRef];
	const ActorDef& def = w.game.actors[r.actor];
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("sspells", "Spells") + " - " + def.name, "Gold: " + std::to_string(gold));
	std::vector<std::string> known = knownSpells(), ids, rows;
	for (auto& id : def.spells)
	{
		auto it = w.game.spells.find(id);
		if (it == w.game.spells.end() || it->second.type != 0)
			continue;
		bool have = false;
		for (auto& k : known)
			have |= k == id;
		if (have)
			continue;
		int price = barterPrice(barterRef, std::max(1, (int)(it->second.cost * w.game.gmstf("fspellvaluemult", 10.0f))), true);
		ids.push_back(id);
		rows.push_back(it->second.name + "   " + std::to_string(price) + "g");
	}
	if (rows.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "Nothing you don't already know");
	uiList(list2, 4, 24, 312, 176, rows, 0.45f, true);
	int sel = list2.selected < (int)ids.size() ? list2.selected : -1;
	int b = buttonRow({ "Buy", w.game.gmst("sdone", "Done") }, focus);
	if (b == 0 && sel >= 0)
	{
		const SpellDef& sp = w.game.spells[ids[sel]];
		int price = barterPrice(barterRef, std::max(1, (int)(sp.cost * w.game.gmstf("fspellvaluemult", 10.0f))), true);
		if (gold < price)
			notify("You don't have enough gold.");
		else
		{
			w.removeItem("gold_001", price);
			w.refs[barterRef].gold += price;          // (OpenMW: services pay into their gold)
			w.stats.spells.push_back(ids[sel]);
			playSound(-1, "Item Gold Down");
			notify("You learned " + sp.name + ".");
		}
	}
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
	}
}

// The trainer's (barterRef) three best skills, best first
void Session::trainerSkills(int best[3])
{
	const ActorDef& def = w.game.actors[w.refs[barterRef].actor];
	best[0] = best[1] = best[2] = -1;
	for (int k = 0; k < 27; k++)
		for (int j = 0; j < 3; j++)
			if (best[j] < 0 || def.skills[k] > def.skills[best[j]])
			{
				for (int m = 2; m > j; m--)
					best[m] = best[m - 1];
				best[j] = k;
				break;
			}
}

int Session::trainPrice(int skill)
{
	return trainPriceFor(barterRef, skill);
}

// One point of a skill from the trainer (barterRef): Morrowind's refusals, else paid, raised, two hours
bool Session::trainSkill(int k)
{
	const ActorDef& def = w.game.actors[w.refs[barterRef].actor];
	const PlayerStats& s = w.stats;
	int price = trainPrice(k);
	std::string msg;
	if (w.itemCount("gold_001") < price)
		msg = "You don't have enough gold.";
	else if (baseSkill(k) >= def.skills[k])
		msg = "You are already as skilled as your trainer.";
	else if (k < (int)w.game.skills.size() && w.game.skills[k].attribute >= 0 && w.game.skills[k].attribute < 8
		&& baseSkill(k) >= s.attributes[w.game.skills[k].attribute])
		msg = w.game.gmst("snotifymessage17", "Your governing attribute is too low to be trained further.");
	if (!msg.empty())
	{
		notify(msg);
		logf("training: refused: %s", msg.c_str());
		return false;
	}
	w.removeItem("gold_001", price);
	w.refs[barterRef].gold += price;
	// Exactly one point, counted towards the next level like any increase
	raiseSkill(k);
	w.gameHour += 2.0f;
	fade = 1.0f;
	logf("training: skill %d for %d gold", k, price);
	return true;
}

// Trainers teach their three best skills, one point at a time: fTrainingMod x the current
// level, at their barter price; two hours pass
void Session::drawTraining()
{
	const Ref& r = w.refs[barterRef];
	const ActorDef& def = w.game.actors[r.actor];
	PlayerStats& s = w.stats;
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("strain", "Training") + " - " + def.name, "Gold: " + std::to_string(gold));
	int best[3];
	trainerSkills(best);
	std::vector<std::string> rows;
	int prices[3];
	for (int j = 0; j < 3; j++)
	{
		prices[j] = trainPrice(best[j]);
		rows.push_back(skillName(w, best[j]) + " " + std::to_string(s.skills[best[j]]) + " -> "
			+ std::to_string(s.skills[best[j]] + 1) + "   " + std::to_string(prices[j]) + "g");
	}
	uiList(list2, 4, 24, 312, 176, rows, 0.5f, true);
	int sel = list2.selected < 3 ? list2.selected : -1;
	int b = buttonRow({ "Train", w.game.gmst("sdone", "Done") }, focus);
	if (b == 0 && sel >= 0)
		trainSkill(best[sel]);
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
	}
}

void Session::drawArrest()
{
	header("Guard");
	int gold = w.itemCount("gold_001");
	uiPanel(4, 24, 312, 120);
	uiTextBox(scroll, 10, 30, 300, 110, "Stop right there, criminal scum! You violated the law. Pay the court a fine of "
		+ std::to_string(w.bounty) + " gold or serve your sentence.", 0.5f, false);
	std::vector<std::string> labels;
	if (gold >= w.bounty)
		labels.push_back("Pay " + std::to_string(w.bounty) + "g");
	labels.push_back("Go to jail");
	labels.push_back("Resist");
	int b = buttonRow(labels, focus, 170, 30);
	if (b < 0)
		return;
	auto calm = [&]() {
		w.forLoadedRefs([&](int i) {
			Ref& r = w.refs[i];
			if (r.ai == AI_COMBAT && !r.aggressor)
			{
				r.ai = AI_IDLE;
				if (Actor* a = w.actorOf(i))
					a->showWeapon = false;
			}
		});
	};
	if (labels[b].rfind("Pay", 0) == 0)
	{
		w.removeItem("gold_001", w.bounty);
		w.bounty = 0;
		calm();
		playSound(-1, "Item Gold Down");
		closeScreen();
	}
	else if (labels[b] == "Go to jail")
	{
		calm();
		closeScreen();
		goToJail();
	}
	else
	{
		w.arrestDeclined = w.bounty;
		if (arrestingGuard >= 0)
			makeHostile(arrestingGuard);
		closeScreen();
	}
	w.globals["pccrimelevel"] = (float)w.bounty;
}

// OpenMW: the distance from the player to the destination (3D) / fTravelMult, times one more for each follower who
// comes along, at the caravaner's barter price; a guide standing indoors (the Mages Guild) charges fMagesGuildTravel
int Session::travelPrice(int ref, const TravelDest& d)
{
	float dx = d.pos[0] - w.player.feet[0], dy = d.pos[1] - w.player.feet[1], dz = d.pos[2] - w.player.feet[2];
	int followers = 0;
	for (int i : w.loadedActors)
		if (w.refs[i].aiPackage == AIPKG_FOLLOW && w.refs[i].aiTarget == "player" && !w.refs[i].dead && w.active(i))
			followers++;
	return travelPriceFor(ref, sqrtf(dx * dx + dy * dy + dz * dz), followers, w.cells[w.current].interior);
}

void Session::drawTravel()
{
	const Ref& r = w.refs[barterRef];
	const ActorDef& def = w.game.actors[r.actor];
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("stravel", "Travel") + " - " + def.name, "Gold: " + std::to_string(gold));
	std::vector<std::string> rows;
	for (auto& d : def.travel)
		rows.push_back(d.cell + "   " + std::to_string(travelPrice(barterRef, d)) + "g");
	uiList(list2, 4, 24, 312, 176, rows, 0.5f, true);
	int sel = list2.selected < (int)def.travel.size() ? list2.selected : -1;
	int b = buttonRow({ "Go", w.game.gmst("sdone", "Done") }, focus);
	if (b == 0 && sel >= 0)
	{
		const TravelDest& d = def.travel[sel];
		int price = travelPrice(barterRef, d);
		int dest = d.hasGrid ? w.gridCell(d.grid[0], d.grid[1]) : w.cellIndex(d.cell);
		if (gold < price)
			notify("You don't have enough gold.");
		else if (dest < 0)
			notify(d.cell + " is not converted yet.");
		else
		{
			w.removeItem("gold_001", price);
			w.refs[barterRef].gold += price;
			// OpenMW: whole hours of the flat distance from the player, and only from outdoors
			float dx = d.pos[0] - w.player.feet[0], dy = d.pos[1] - w.player.feet[1];
			if (!w.cells[w.current].interior)
				w.gameHour += (float)(int)(sqrtf(dx * dx + dy * dy) / w.game.gmstf("ftraveltimemult", 16000.0f));
			travelCell = dest;
			memcpy(travelPos, d.pos, sizeof(travelPos));
			travelYaw = d.rot[2];
			logf("travel: %s to %s for %d gold", def.name.c_str(), d.cell.c_str(), price);
			closeScreen();
		}
	}
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
	}
}

// The land map (tools/convert/worldmap.py), centred on the player, with town names and an arrow for them
// ---- maps: the world map outdoors, the local map (a top-down render) indoors

// Jail: a day per hundred gold of bounty passes, stolen goods are taken, a skill a day changes (Security
// and Sneak may rise, the others fall), and the player is let out at the nearest prison's release spot
void Session::goToJail()
{
	int days = w.bounty / 100 > 1 ? w.bounty / 100 : 1;
	w.gameHour += 24.0f * days;
	w.bounty = 0;
	w.globals["pccrimelevel"] = 0.0f;
	w.confiscateStolen();
	for (int d = 0; d < days; d++)
	{
		int k = rand() % 27;
		bool rises = (k == 18 || k == 19) && rand() % 2;       // Security, Sneak: learned inside
		if (rises)
			w.stats.skillBonus[k]++;
		else if (w.stats.skills[k] > 0)
			w.stats.skillBonus[k]--;
	}
	w.recomputeStats();
	fade = 1.0f;
	char msg[160];
	snprintf(msg, sizeof(msg), w.game.gmst(days == 1 ? "snotifymessage42" : "snotifymessage43",
		days == 1 ? "You have been released after %d day." : "You have been released after %d days.").c_str(), days);
	notify(msg);
	// Nearest prison (from the last spot outdoors when inside), let out at the marker itself, outdoors
	// (OpenMW teleportToClosestMarker). Its door destination only says where the stolen goods go: Ebonheart's
	// lies outside the garrison's walls, under every floor.
	const float* at = w.cells[w.current].interior ? w.lastOutside : w.player.feet;
	const GameData::PrisonMarker* best = nullptr;
	int bestCell = -1;
	float bestD = 1e30f;
	for (auto& m : w.game.prisonMarkers)
	{
		float dx = m.pos[0] - at[0], dy = m.pos[1] - at[1], d = dx * dx + dy * dy;
		int c = w.gridCell((int)floorf(m.pos[0] / 8192.0f), (int)floorf(m.pos[1] / 8192.0f));
		if (d < bestD && c >= 0)
		{
			bestD = d;
			best = &m;
			bestCell = c;
		}
	}
	if (best)
		teleportPlayer(bestCell, best->pos, 0.0f);
	logf("jail: %d days, released outside %s", days, best ? best->cell.c_str() : "nowhere (stays in place)");
}
