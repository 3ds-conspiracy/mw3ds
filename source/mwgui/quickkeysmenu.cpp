// Quick keys: set from Inventory / Magic, used from the HUD
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwbase/session.hpp"
#include "mwgui/inventoryhelpers.hpp"

// Known spells and powers; the selected one is cast with Y
static const char* kQuickDir[4] = { "Up", "Right", "Down", "Left" };
// (GoldenEye's D-pad moves: its quick keys are the touch strip's slots)
static std::string quickKeyName(int k)
{
	if (g_controlLayout == CONTROLS_GOLDENEYE)
		return std::string(kQuickDir[k]) + " (tap its slot)";
	return std::string(g_controlLayout != CONTROLS_CLASSIC ? "D-Pad " : "ZL + ") + kQuickDir[k];
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
		notify((io ? io->name.str() : w.game.spells[id].name) + " is ready (" + castHint() + ").");
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
		notify("You have no " + (o ? o->name.str() : id) + ".");
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
