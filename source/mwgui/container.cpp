// A container's (or a corpse's, or a pickpocketed NPC's) items: take and put
#include <cmath>
#include <cstdio>
#include <cstring>
#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// A companion's "minimumprofit" (their script's local; Tribunal's mercenaries): what they have been given less what was
// taken from them, at the items' values (OpenMW's CompanionItemModel); nullptr when their script has none
static float* companionProfit(World& w, int ref)
{
	if (ref < 0 || w.refs[ref].script < 0 || w.refs[ref].script >= (int)w.scripts.size())
		return nullptr;
	return w.scripts[w.refs[ref].script].local("minimumprofit");
}

// Talking to someone whose script says they are a companion ("companion" not 0): Share (OpenMW's DialogueWindow)
bool Session::isCompanion(int ref)
{
	if (ref < 0 || w.refs[ref].script < 0 || w.refs[ref].script >= (int)w.scripts.size())
		return false;
	const float* v = w.scripts[w.refs[ref].script].local("companion");
	return v && (int)*v != 0;
}

// One stack out of the open container into the player's inventory (the owner's claim when it has one)
void Session::containerTake(int i, bool checkOwner)
{
	Ref& c = w.refs[containerRef];
	if (i < 0 || i >= (int)c.contents.size())
		return;
	if (companionSharing)
	{
		checkOwner = false;              // (a companion's share is the player's to take)
		const Object* o = w.game.object(c.contents[i].second);
		if (float* p = companionProfit(w, containerRef))
			*p -= (float)((o ? o->value : 0) * stockCount(c.contents[i].first));
	}
	// (what a living, knocked-down NPC carries is theirs: taking it is a theft, with them as the owner)
	if (checkOwner && (w.ownedByOther(containerRef) || (c.recType == REC_NPC_ && !c.dead)))
		takeOwned(containerRef, c.contents[i].second, stockCount(c.contents[i].first));
	w.takeStack(c.contents[i], stockCount(c.contents[i].first));   // (its wear, soul and charge come with it)
	playSound(-1, "Item Misc Up");
	c.contents.erase(c.contents.begin() + i);
}

// One inventory stack into the open container (OpenMW's ContainerItemModel::onDropItem and DragAndDrop::drop): not
// into pockets being picked, a plant, or past a chest's capacity; a conjured item stays with the player
bool Session::containerPut(int i)
{
	Ref& c = w.refs[containerRef];
	if (i < 0 || i >= (int)w.inventory.size())
		return false;
	InventoryItem it = w.inventory[i];
	const Object* o = w.game.object(it.id);
	for (auto& a : w.effects)
		if (!a.item.empty() && a.item == it.id)
		{
			notify(w.game.gmst("sbarterdialog12", "You cannot sell or give away conjured items."));
			return false;
		}
	if (c.recType == REC_CONT && c.obj)
	{
		if (c.obj->organic)
		{
			notify(w.game.gmst("scontentsmessage2", "You cannot place items in this container."));
			return false;
		}
		if (c.obj->capacity >= 0.0f)
		{
			float load = 0.0f;
			for (auto& e : c.contents)
				if (const Object* eo = w.game.object(e.second))
					load += eo->weight * stockCount(e.first);
			if (c.obj->capacity <= 0.0f || std::nextafterf(c.obj->capacity, 1e30f) < load + (o ? o->weight : 0.0f) * it.count)
			{
				notify(w.game.gmst("scontentsmessage3", "Your item does not fit in the container."));
				return false;
			}
		}
	}
	if (it.equipped)
	{
		w.inventory[i].equipped = false;
		w.refreshStats();
	}
	w.inventory.erase(w.inventory.begin() + i);
	// with the last one gone its script stops (as World::removeItem)
	if (w.itemCount(it.id) == 0)
		for (auto& sc : w.scripts)
			if (sc.item == it.id)
				sc.running = false;
	// onto a stack of the same that is the player's (not one the container restocks), its wear, soul and charge kept
	w.refAddStack(c, it);
	if (companionSharing)
		if (float* p = companionProfit(w, containerRef))
			*p += (float)((o ? o->value : 0) * it.count);
	playSound(-1, "Item Misc Down");
	logf("container: put %s x%d into %s", it.id.c_str(), it.count, c.id.c_str());
	return true;
}

// A companion's share closed: back to the conversation, unless the mercenary is owed (their profit below 0: the warning)
void Session::companionShareClose()
{
	float* p = companionProfit(w, containerRef);
	if (p && *p < 0.0f)
		companionWarning = true;
	else
	{
		companionSharing = false;
		containerRef = -1;
		screen = SCR_DIALOGUE;
	}
	playSound(-1, "Menu Click");
}

void Session::drawContainer()
{
	Ref& c = w.refs[containerRef];
	std::string title = c.obj ? c.obj->name.str() : c.id.str();
	// Two pages (L / R, as Barter's Buy / Sell): what it holds, to take, and the player's items, to put in. Pockets
	// being picked take nothing back (OpenMW: no reverse pickpocket)
	bool canPut = !pickpocketing;
	bool putting = canPut && containerPutting;
	if (canPut)
	{
		if (uiIn().down & (KEY_L | KEY_R))
			putting = !putting;
		if (uiButton(4, 24, 154, 18, title, !putting))
			putting = false;
		if (uiButton(162, 24, 154, 18, w.game.gmst("sinventory", "Inventory"), putting))
			putting = true;
		if (putting != containerPutting)
		{
			containerPutting = putting;
			grid = UiGrid();
			playSound(-1, "Menu Click");
		}
	}
	std::string load;
	if (c.recType == REC_CONT && c.obj && c.obj->capacity > 0.0f)
	{
		float weight = 0.0f;
		for (auto& e : c.contents)
			if (const Object* eo = w.game.object(e.second))
				weight += eo->weight * stockCount(e.first);
		load = std::to_string((int)weight) + "/" + std::to_string((int)c.obj->capacity);
	}
	// A companion's share: their profit so far, when their script keeps one (sProfitValue)
	if (companionSharing)
		if (float* p = companionProfit(w, containerRef))
			load = w.game.gmst("sprofitvalue", "Profit:") + " " + std::to_string((int)*p);
	header(title, load);
	// Leaving with the profit below 0: the mercenary warns they will quit (OpenMW's CompanionWindow::exit); they quit,
	// or the share stays open
	if (companionSharing && companionWarning)
	{
		uiPanel(20, 60, 280, 120);
		std::vector<std::string> lines = uiWrap(w.game.gmst("scompanionwarningmessage",
			"Your mercenary is poorer now than when he contracted with you.  Your mercenary will quit if you do not give him "
			"gold or goods to bring his Profit to a positive value."), 268, 0.42f);
		float ly = 66;
		for (auto& l : lines)
		{
			uiText(26, ly, 0.42f, col::text, l);
			ly += uiLineHeight(0.42f);
		}
		std::vector<std::string> labels = { w.game.gmst("scompanionwarningbuttonone", "Let the mercenary quit."),
			w.game.gmst("scompanionwarningbuttontwo", "Return to Companion Share display.") };
		if (uiButton(24, 150, 134, 24, labels[0]))
		{
			// (they quit: the share and the conversation close, their contract script sees it)
			companionWarning = companionSharing = false;
			screen = SCR_DIALOGUE;
			closeScreen();
			playSound(-1, "Menu Click");
		}
		else if (uiButton(162, 150, 134, 24, labels[1]) || (uiIn().down & KEY_B))
		{
			companionWarning = false;
			playSound(-1, "Menu Click");
		}
		uiConsume(KEY_A | KEY_B | KEY_X | KEY_L | KEY_R | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
		return;
	}
	int gridY = canPut ? 44 : 26;
	std::vector<UiGridItem> cells;
	std::vector<int> mine;             // putting: the inventory index of each cell
	if (putting)
		for (size_t i = 0; i < w.inventory.size(); i++)
		{
			mine.push_back((int)i);
			cells.push_back(gridItem(w.game.object(w.inventory[i].id), w.inventory[i].id, w.inventory[i].count, w.inventory[i].equipped));
		}
	else
		for (auto& it : c.contents)
			cells.push_back(gridItem(w.game.object(it.second), it.second, stockCount(it.first), false));
	if (cells.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, putting ? "Nothing" : "Empty");
	bool aKey = uiIn().down & KEY_A;
	// Tap an item (or A on the selected one) to take it, or on the Inventory page to put it in, as a click does in
	// Morrowind
	int pick = uiItemGrid(grid, 4, gridY, 312, 170 - gridY, cells);
	const Object* shown = nullptr;
	InventoryItem state = { "", 0, false };         // the selected one's wear, soul and charge, for its tip
	if (putting && grid.selected >= 0 && grid.selected < (int)mine.size())
	{
		state = w.inventory[mine[grid.selected]];
		shown = w.game.object(state.id);
	}
	else if (!putting && grid.selected >= 0 && grid.selected < (int)c.contents.size())
	{
		const ContentItem& e = c.contents[grid.selected];
		state = { lower(e.second), stockCount(e.first), false, e.condition, e.soul, e.charge };
		shown = w.game.object(e.second);
	}
	if (shown)
	{
		itemInfo(shown, 176);
		itemTip(shown, &state);
	}
	bool caught = false;
	auto take = [&](int i) {
		if (caught)
			return;
		if (pickpocketing)
		{
			// Each take is a roll against the victim, harder the more it's worth
			const Object* o = w.game.object(c.contents[i].second);
			float value = o ? (float)(o->value * stockCount(c.contents[i].first)) : 0.0f;
			if (pickpocketCaught(10.0f * w.game.gmstf("fpickpocketmod", 0.3f) * value))
			{
				caught = true;
				logf("pickpocket: caught taking %s from %s", c.contents[i].second.c_str(), c.id.c_str());
				say(containerRef, "", w.game.gmst("scaughtstealingmessage", "Hey he's stealing my stuff!"));
				crimeSeen(CRIME_PICKPOCKET, 0, containerRef), reportCrime(containerRef, (int)w.game.gmstf("icrimepickpocket", 25));
				pickpocketing = false;          // no second roll on the way out
				closeScreen();
				return;
			}
			w.markStolen(c.contents[i].second, stockCount(c.contents[i].first), c.idLower);
			useSkill(19, 1);                    // Sneak: a successful pick
			logf("pickpocket: took %s x%d from %s", c.contents[i].second.c_str(), stockCount(c.contents[i].first), c.id.c_str());
		}
		containerTake(i, !pickpocketing);
	};
	uiConsume(KEY_A | KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
	int noFocus = -1;
	// A body can be disposed of (not one with a script: a quest may need it)
	Ref& owner = w.refs[containerRef];
	bool corpse = (owner.recType == REC_NPC_ || owner.recType == REC_CREA) && owner.dead;
	const std::string takeAll = w.game.gmst("stakeall", "Take All") + " (X)", dispose = w.game.gmst("sdisposeofcorpse", "Dispose of Corpse");
	std::vector<std::string> labels;
	if (!putting)
		labels.push_back(takeAll);
	if (corpse)
		labels.push_back(dispose);
	labels.push_back(w.game.gmst("sclose", "Close"));
	int b = buttonRow(labels, noFocus);
	const std::string pressed = b >= 0 && b < (int)labels.size() ? labels[b] : "";
	if (pressed == dispose)
	{
		if (owner.obj && !owner.obj->script.empty())
			notify(w.game.gmst("sdisposecorpsefail", "You can not remove this corpse"));
		else
		{
			w.setEnabled(containerRef, false);
			closeScreen();
			return;
		}
	}
	bool closing = b == (int)labels.size() - 1 || (uiIn().down & KEY_B);
	if (putting && pick >= 0 && pick < (int)mine.size())
	{
		if (containerPut(mine[pick]))
			grid.selected = -1;
	}
	else if (!putting && pick >= 0 && pick < (int)c.contents.size())
		take(pick);
	else if (!putting && (pressed == takeAll || (uiIn().down & KEY_X)))
		while (!c.contents.empty() && !caught && screen == SCR_CONTAINER)
			take(0);
	if (caught)
		return;
	else if (closing && companionSharing)
		companionShareClose();
	else if (closing)
	{
		if (c.recType == REC_CONT && (!c.obj || !c.obj->organic))   // plants have no lid to close, bodies and pockets none
			playSound(-1, "chest close");
		closeScreen();
	}
	(void)aKey;
}
