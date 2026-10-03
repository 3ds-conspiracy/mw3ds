// Items: books, containers, the inventory, the magic menu
#include <cmath>
#include <cstdio>
#include <cstring>
#include "audio.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screens.h"
#include "session.h"

// How a quick key is set from Inventory / Magic: the D-pad moves the selection there, so both layouts hold ZL
static const char* kQuickHint = "ZL + D-pad: set quick key";

void Session::drawBook()
{
	const Object* o = bookRef >= 0 ? w.refs[bookRef].obj : w.game.object(bookItem);
	if (!o)
	{
		closeScreen();
		return;
	}
	header(o->name);
	readBook(o);
	uiPanel(4, 24, 312, 180);
	// with its pictures (bookart), where the text has them
	uiTextBoxPictures(scroll, 6, 26, 308, 176, dialogueSubstitute(w, o->text, -1),
		[](const std::string& key, void* ctx) {
			const GameData& g = *(const GameData*)ctx;
			std::string k = lower(key);
			if (k.rfind("bookart\\", 0) != 0)
				k = "bookart\\" + k;
			auto it = g.art.bookart.find(k);
			UiPicture p;
			if (it != g.art.bookart.end())
				p = { it->second.file, it->second.w, it->second.h };
			return p;
		}, &w.game);
	std::vector<std::string> labels;
	bool canTake = bookRef >= 0 && w.refs[bookRef].visible();
	if (canTake)
		labels.push_back(w.game.gmst("stake", "Take"));
	labels.push_back(w.game.gmst("sclose", "Close"));
	int b = buttonRow(labels, focus);
	if (canTake && b == 0)
	{
		int ref = bookRef;
		closeScreen();
		pickUp(ref);
	}
	else if (b == (int)labels.size() - 1 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Book Close");
		closeScreen();
	}
}

// One stack out of the open container into the player's inventory (the owner's claim when it has one)
void Session::containerTake(int i, bool checkOwner)
{
	Ref& c = w.refs[containerRef];
	if (i < 0 || i >= (int)c.contents.size())
		return;
	// (what a living, knocked-down NPC carries is theirs: taking it is a theft, with them as the owner)
	if (checkOwner && (w.ownedByOther(containerRef) || (c.type == "NPC_" && !c.dead)))
		takeOwned(containerRef, c.contents[i].second, stockCount(c.contents[i].first));
	w.addItem(c.contents[i].second, stockCount(c.contents[i].first));
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
	// (what lies in a container is only an id and a count: a soul would be lost, so a filled gem stays)
	if (!it.soul.empty())
	{
		notify("A soul gem holding a soul can not be put away.");
		return false;
	}
	if (c.type == "CONT" && c.obj)
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
	// onto a stack of the same that is the player's (not one the container restocks)
	bool merged = false;
	for (auto& e : c.contents)
		if (e.first > 0 && lower(e.second) == it.id)
		{
			e.first += it.count;
			merged = true;
			break;
		}
	if (!merged)
		c.contents.emplace_back(it.count, it.id);
	playSound(-1, "Item Misc Down");
	logf("container: put %s x%d into %s", it.id.c_str(), it.count, c.id.c_str());
	return true;
}

void Session::drawContainer()
{
	Ref& c = w.refs[containerRef];
	std::string title = c.obj ? c.obj->name : c.id;
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
	if (c.type == "CONT" && c.obj && c.obj->capacity > 0.0f)
	{
		float weight = 0.0f;
		for (auto& e : c.contents)
			if (const Object* eo = w.game.object(e.second))
				weight += eo->weight * stockCount(e.first);
		load = std::to_string((int)weight) + "/" + std::to_string((int)c.obj->capacity);
	}
	header(title, load);
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
	if (putting && grid.selected >= 0 && grid.selected < (int)mine.size())
		shown = w.game.object(w.inventory[mine[grid.selected]].id);
	else if (!putting && grid.selected >= 0 && grid.selected < (int)c.contents.size())
		shown = w.game.object(c.contents[grid.selected].second);
	if (shown)
	{
		itemInfo(shown, 176);
		itemTip(shown, nullptr);
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
	bool corpse = (owner.type == "NPC_" || owner.type == "CREA") && owner.dead;
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
	else if (closing)
	{
		if (c.type == "CONT" && (!c.obj || !c.obj->organic))   // plants have no lid to close, bodies and pockets none
			playSound(-1, "chest close");
		closeScreen();
	}
	(void)aKey;
}

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
		if (uiButton(4 + t * 50, 24, 48, 18, kTabs[t], t == tab))
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
		assignQuickKey("inv:" + qi.id, qo ? qo->name : qi.id);
	}
	int pick = uiItemGrid(grid, 4, 45, 312, 132, cells);
	uiText(6, 178, 0.36f, col::textDim, kQuickHint);
	int sel = grid.selected >= 0 && grid.selected < (int)index.size() ? index[grid.selected] : -1;
	const Object* o = sel >= 0 ? w.game.object(w.inventory[sel].id) : nullptr;
	std::string soulNote;
	if (sel >= 0 && !w.inventory[sel].soul.empty())
	{
		soulNote = "Soul: " + w.inventory[sel].soul;
		for (auto& a : w.game.actors)
			if (lower(a.id) == w.inventory[sel].soul)
				soulNote = "Soul: " + a.name;
	}
	itemInfo(o, 191, soulNote);
	if (sel >= 0)
		itemTip(o, &w.inventory[sel]);

	std::vector<std::string> labels;
	if (o)
	{
		if (o->type == "BOOK")
			labels.push_back("Read");
		else if (!o->effects.empty() && (o->type == "ALCH" || o->type == "INGR"))
			labels.push_back(o->type == "ALCH" ? "Drink" : "Eat");
		else if (o->type == "APPA" || o->type == "REPA" || !w.inventory[sel].soul.empty())
			labels.push_back("Use");
		if (!w.inventory[sel].soul.empty())
			labels.push_back("Recharge");
		else if (o->type == "WEAP" || o->type == "ARMO" || o->type == "CLOT" || o->type == "LIGH"
			|| o->type == "LOCK" || o->type == "PROB")
			labels.push_back(w.inventory[sel].equipped ? "Unequip" : "Equip");
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
		else if (labels[b] == "Use" && o->type == "REPA")
		{
			openRepair(-1, sel);
			playSound(-1, "Menu Click");
		}
		else if (labels[b] == "Use" && !w.inventory[sel].soul.empty())
		{
			openEnchanting(-1);
			playSound(-1, "Menu Click");
		}
		else if (labels[b] == "Use")
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
			openScreen(SCR_BOOK);
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

// Known spells and powers; the selected one is cast with Y
static const char* kQuickDir[4] = { "Up", "Right", "Down", "Left" };

// The button that casts, and how a quick key is pressed, in the active layout
static std::string castHint() { return std::string(g_controlLayout == CONTROLS_XBOX ? "L" : "Y") + " to cast"; }
static std::string quickKeyName(int k) { return std::string(g_controlLayout == CONTROLS_XBOX ? "D-Pad " : "ZL + ") + kQuickDir[k]; }

void Session::drawMagic()
{
	char buf[96];
	snprintf(buf, sizeof(buf), "Magicka %d/%d", (int)w.stats.magicka, (int)w.stats.magickaMax);
	header(w.game.gmst("smagic", "Magic"), buf);
	std::vector<std::string> ids = knownSpells(), rows;
	for (auto& id : ids)
	{
		const SpellDef& sp = w.game.spells[id];
		std::string row = (id == w.stats.selectedSpell ? "* " : "") + sp.name;
		row += sp.type == 5 ? "   (power)" : "   " + std::to_string(sp.cost) + " mp, " + std::to_string(castChance(sp)) + "%";
		rows.push_back(row);
	}
	for (auto& it : w.inventory)
		if (const SpellDef* en = w.enchantmentOf(it))
			if (en->type == ENCH_USE || en->type == ENCH_ONCE)
			{
				std::string id = "item:" + it.id;
				const Object* o = w.game.object(it.id);
				bool have = false;
				for (auto& x : ids)
					have |= x == id;
				if (have || !o)
					continue;
				ids.push_back(id);
				std::string row = (id == w.stats.selectedSpell ? "* " : "") + o->name;
				row += en->type == ENCH_ONCE ? "   (scroll)"
					: "   " + std::to_string((int)w.chargeOf(it)) + "/" + std::to_string(en->charge) + " charge";
				rows.push_back(row);
			}
	if (rows.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "You know no spells");
	if (list.selected >= 0 && list.selected < (int)ids.size())
	{
		const std::string& qid = ids[list.selected];
		const Object* io = qid.compare(0, 5, "item:") == 0 ? w.game.object(qid.substr(5)) : nullptr;
		assignQuickKey("magic:" + qid, io ? io->name : w.game.spells[qid].name);
		if (io)
		{
			const InventoryItem* ii = nullptr;
			for (auto& x : w.inventory)
				if (x.id == qid.substr(5))
					ii = &x;
			itemTip(io, ii);
		}
		else if (w.game.spells.count(qid))
			spellTip(w.game.spells[qid]);
	}
	uiList(list, 4, 24, 312, 130, rows, 0.45f, true);
	uiText(6, 190, 0.36f, col::textDim, kQuickHint);
	// Active effects
	std::string active;
	for (auto& e : w.effects)
		if (active.find(e.source) == std::string::npos)
			active += (active.empty() ? "" : ", ") + e.source;
	if (!active.empty())
		uiText(8, 160, 0.42f, col::textDim, "Active: " + active.substr(0, 60));
	int sel = list.selected < (int)ids.size() ? list.selected : -1;
	int b = buttonRow({ "Select", w.game.gmst("sclose", "Close") }, focus);
	if (b == 0 && sel >= 0)
	{
		w.stats.selectedSpell = ids[sel];
		const Object* io = ids[sel].compare(0, 5, "item:") == 0 ? w.game.object(ids[sel].substr(5)) : nullptr;
		notify((io ? io->name : w.game.spells[ids[sel]].name) + " is ready (" + castHint() + ").");
		closeScreen();
	}
	else if (b == 1 || (uiIn().down & KEY_B))
		closeScreen();
}

bool Session::assignQuickKey(const std::string& entry, const std::string& name)
{
	const UiInput& in = uiIn();
	if (!(in.held & KEY_ZL))
		return false;
	static const u32 keys[4] = { KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT };
	for (int k = 0; k < 4; k++)
		if (in.down & keys[k])
		{
			w.stats.quickKeys[k] = entry;
			uiConsume(KEY_DUP | KEY_DRIGHT | KEY_DDOWN | KEY_DLEFT);
			notify("Quick key " + quickKeyName(k) + ": " + name);
			playSound(-1, "Menu Click");
			return true;
		}
	return false;
}

void Session::useQuickKey(int k)
{
	const std::string& q = w.stats.quickKeys[k];
	if (q.empty())
	{
		notify("Quick key " + quickKeyName(k) + " is empty: highlight an item or spell in "
			"Inventory or Magic and press ZL + " + kQuickDir[k] + ".");
		return;
	}
	if (q.compare(0, 6, "magic:") == 0)
	{
		std::string id = q.substr(6);
		const Object* io = id.compare(0, 5, "item:") == 0 ? w.game.object(id.substr(5)) : nullptr;
		std::vector<std::string> known = knownSpells();
		bool have = io ? w.itemCount(id.substr(5)) > 0 : std::find(known.begin(), known.end(), id) != known.end();
		if (!have)
		{
			notify(io ? "You no longer have " + io->name + "." : "You don't know that spell.");
			return;
		}
		w.stats.selectedSpell = id;
		notify((io ? io->name : w.game.spells[id].name) + " is ready (" + castHint() + ").");
		return;
	}
	std::string id = q.compare(0, 4, "inv:") == 0 ? q.substr(4) : q;
	int idx = -1;
	for (size_t i = 0; i < w.inventory.size(); i++)
		if (w.inventory[i].id == id)
		{
			idx = i;
			break;
		}
	const Object* o = w.game.object(id);
	if (idx < 0 || !o)
	{
		notify("You have no " + (o ? o->name : id) + ".");
		return;
	}
	if ((o->type == "ALCH" || o->type == "INGR") && !o->effects.empty())
		consume(id);
	else if (o->type == "BOOK")
	{
		openScreen(SCR_BOOK);
		bookItem = id;
		playSound(-1, o->scroll ? "scroll" : "Book Open");
	}
	else if (o->type == "WEAP" || o->type == "ARMO" || o->type == "CLOT" || o->type == "LIGH"
		|| o->type == "LOCK" || o->type == "PROB")
	{
		if (!w.inventory[idx].equipped && !equipItem(idx))
			return;
		notify(o->name + " equipped.");
	}
	else
		notify(o->name + " can't be used from a quick key.");
}

static bool isTwoHanded(const Object* x)
{
	// long blade / blunt close / blunt wide / spear / axe two-handed, bow, crossbow
	int t = x->subtype;
	return x->type == "WEAP" && (t == 2 || t == 4 || t == 5 || t == 6 || t == 8 || t == 9 || t == 10);
}

// OpenMW's canBeEquipped: a broken weapon or armor piece is refused, a beast race (Khajiit, Argonian: the race's flag)
// takes no full helmet, boots or shoes (by the parts the item's model has, not its name); a two-handed weapon is code 2, a
// shield with one in hand code 3 (neither stops the equip)
int Session::canEquip(const InventoryItem& it, std::string* why)
{
	const Object* o = w.game.object(it.id);
	if (!o)
		return 0;
	auto no = [&](const char* id, const char* text) {
		if (why)
			*why = w.game.gmst(id, text);
		return 0;
	};
	if ((o->type == "ARMO" || o->type == "WEAP") && o->health > 0 && itemCondition(it) <= 0)
		return no("sinventorymessage1", "This object is broken and cannot be equipped until fixed.");
	if (o->type == "ARMO" || o->type == "CLOT")
		for (auto& r : w.game.races)
			if (r.beast && lower(r.id) == lower(w.stats.race))
			{
				if (o->flags & 1)
					return no("snotifymessage13", "Beast races cannot wear full helmets.");
				if (o->flags & 2)
					return o->type == "ARMO" ? no("snotifymessage14", "Beast races cannot wear boots.")
						: no("snotifymessage15", "Beast races cannot wear shoes.");
			}
	if (isTwoHanded(o))
		return 2;
	if (o->type == "ARMO" && o->subtype == ARMO_SHIELD)
		for (auto& p : w.inventory)
			if (p.equipped)
			{
				const Object* q = w.game.object(p.id);
				if (q && isTwoHanded(q))
					return 3;
			}
	return 1;
}

// Equipping takes off what held the same place: one thing in hand (weapon, lockpick, probe), one ammunition, armor
// by slot (a gauntlet or bracer and a glove share a hand; boots and shoes a foot; a shield and a light the left hand),
// up to two rings. A two-handed weapon and a shield are both worn (OpenMW). The player's own equip can be refused
// (canEquip); a script's Equip is forced
bool Session::equipItem(int index, bool force)
{
	InventoryItem& it = w.inventory[index];
	const Object* o = w.game.object(it.id);
	if (!o)
		return false;
	std::string why;
	if (!force && canEquip(it, &why) == 0)
	{
		if (!why.empty())
			notify(why);
		return false;
	}
	auto hand = [](const Object* x) {
		return (x->type == "WEAP" && x->subtype < WEAP_ARROW) || x->type == "LOCK" || x->type == "PROB";
	};
	auto shield = [](const Object* x) { return x->type == "ARMO" && x->subtype == ARMO_SHIELD; };
	// Which hand armor / gloves cover (-1 none, 0 left, 1 right)
	auto handSide = [](const Object* x) {
		if (x->type == "ARMO")
			return x->subtype == 6 || x->subtype == 9 ? 0 : x->subtype == 7 || x->subtype == 10 ? 1 : -1;
		if (x->type == "CLOT")
			return x->subtype == 6 ? 0 : x->subtype == 5 ? 1 : -1;
		return -1;
	};
	// Boots (armor 5) and shoes (clothing 1) are the one foot slot
	auto foot = [](const Object* x) { return (x->type == "ARMO" && x->subtype == 5) || (x->type == "CLOT" && x->subtype == 1); };
	int rings = 0, firstRing = -1;
	for (size_t k = 0; k < w.inventory.size(); k++)
	{
		InventoryItem& other = w.inventory[k];
		const Object* p = w.game.object(other.id);
		if (!other.equipped || (int)k == index || !p)
			continue;
		bool off = false;
		if (hand(o) && hand(p))
			off = true;
		else if ((shield(o) && p->type == "LIGH") || (o->type == "LIGH" && shield(p)))
			off = true;
		else if (foot(o) && foot(p))
			off = true;
		else if (o->type == "WEAP" && o->subtype >= WEAP_ARROW && p->type == "WEAP" && p->subtype >= WEAP_ARROW)
			off = true;
		else if (handSide(o) >= 0 && handSide(o) == handSide(p))
			off = true;
		else if (o->type == "ARMO" && p->type == "ARMO" && o->subtype == p->subtype)
			off = true;
		else if (o->type == "CLOT" && p->type == "CLOT" && o->subtype == p->subtype)
		{
			if (o->subtype == 8)           // rings: two may be worn
			{
				if (firstRing < 0)
					firstRing = k;
				rings++;
			}
			else
				off = true;
		}
		else if (o->type == "LIGH" && p->type == "LIGH")
			off = true;
		if (off)
			other.equipped = false;
	}
	if (rings >= 2)
		w.inventory[firstRing].equipped = false;
	it.equipped = true;
	playSound(-1, "Item Misc Up");
	w.setItemScriptLocal(it.id, "onpcequip", 1.0f);
	w.refreshStats();                  // worn constant effects
	return true;
}

// Equip on the player from a script: the item is put in the pack first when it isn't there, then used as
// from the inventory (worn, drunk, eaten, read)
void Session::equipPlayerItem(const std::string& id)
{
	const Object* o = w.game.object(id);
	if (!o)
		return;
	int idx = -1;
	for (size_t i = 0; i < w.inventory.size() && idx < 0; i++)
		if (w.inventory[i].id == id)
			idx = i;
	if (idx < 0)
	{
		w.addItem(id, 1);
		for (size_t i = 0; i < w.inventory.size() && idx < 0; i++)
			if (w.inventory[i].id == id)
				idx = i;
	}
	if (idx < 0)
		return;
	if ((o->type == "ALCH" || o->type == "INGR") && !o->effects.empty())
		consume(id);
	else if (o->type == "BOOK")
	{
		openScreen(SCR_BOOK);
		bookItem = id;
		playSound(-1, o->scroll ? "scroll" : "Book Open");
	}
	else if (o->type == "WEAP" || o->type == "ARMO" || o->type == "CLOT" || o->type == "LIGH"
		|| o->type == "LOCK" || o->type == "PROB")
	{
		if (!w.inventory[idx].equipped)
			equipItem(idx, true);
	}
}
