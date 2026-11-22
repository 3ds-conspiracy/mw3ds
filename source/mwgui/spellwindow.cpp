// The magic menu: spells, powers and enchanted items to cast
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/inventoryhelpers.hpp"
#include "components/debug/debuglog.hpp"

// Next / Previous Spell (OpenMW's SpellWindow::cycle; the Classic layout's Y + L / R): through what the magic menu lists,
// the powers and spells then the enchanted items that are cast, round from the last to the first. Not during a cast,
// nor as a werewolf, nor while a script has magic off
void Session::cycleSpell(int dir)
{
	if (castBusy() || vm.action == VM_CAST || !w.magicEnabled || werewolfRefused())
		return;
	std::vector<std::string> ids = knownSpells();
	for (auto& it : w.inventory)
		if (const SpellDef* en = w.enchantmentOf(it))
			if ((en->type == ENCH_USE || en->type == ENCH_ONCE) && w.game.object(it.id)
				&& std::find(ids.begin(), ids.end(), "item:" + it.id) == ids.end())
				ids.push_back("item:" + it.id);
	if (ids.empty())
	{
		notify("You know no spells.");
		return;
	}
	int n = ids.size();
	int at = std::find(ids.begin(), ids.end(), w.stats.selectedSpell) - ids.begin();
	int k = at >= n ? (dir > 0 ? 0 : n - 1) : ((at + dir) % n + n) % n;
	w.stats.selectedSpell = ids[k];
	const Object* io = ids[k].compare(0, 5, "item:") == 0 ? w.game.object(ids[k].substr(5)) : nullptr;
	notify(io ? io->name.str() : w.game.spells[ids[k]].name);
}

void Session::drawMagic()
{
	char buf[96];
	snprintf(buf, sizeof(buf), "Magicka %d/%d", (int)w.stats.magicka, (int)w.stats.magickaMax);
	header(w.game.gmst("smagic", "Magic"), buf);
	// Filter by school (L / R, or the school's icon): all, alteration, conjuration, destruction, illusion, mysticism, restoration
	const UiInput& in = uiIn();
	int tab = magicTab;
	if (in.down & KEY_L) tab = (tab + 6) % 7;
	if (in.down & KEY_R) tab = (tab + 1) % 7;
	for (int t = 0; t < 7; t++)
		if (schoolButton(w.game, 4 + t * 44.5f, 24, 42.5f, 18, t, t == tab))
			tab = t;
	if (tab != magicTab)
	{
		magicTab = tab;
		list = UiList();
		playSound(-1, "Menu Click");
	}
	auto inSchool = [&](const SpellDef& sp) { return magicTab == 0 || spellSchool(sp) == magicTab - 1; };
	auto firstIcon = [&](const SpellDef& sp) {
		auto me = sp.effects.empty() ? w.game.magicEffects.end() : w.game.magicEffects.find(sp.effects[0].effect);
		return me != w.game.magicEffects.end() ? me->second.iconIx : -1;
	};
	// (ids: every spell and item entry that is known, shown or not; the rows and what follows them: the shown ones)
	std::vector<std::string> all = knownSpells(), ids, rows;
	std::vector<int> icons;
	for (auto& id : all)
	{
		const SpellDef& sp = w.game.spells[id];
		if (!inSchool(sp))
			continue;
		std::string row = (id == w.stats.selectedSpell ? "* " : "") + sp.name;
		row += sp.type == 5 ? "   (power)" : "   " + std::to_string(sp.cost) + " mp, " + std::to_string(castChance(sp)) + "%";
		ids.push_back(id);
		rows.push_back(row);
		icons.push_back(firstIcon(sp));
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
				if (have || !o || !inSchool(*en))
					continue;
				ids.push_back(id);
				std::string row = (id == w.stats.selectedSpell ? "* " : "") + o->name;
				row += en->type == ENCH_ONCE ? "   (scroll)"
					: "   " + std::to_string((int)w.chargeOf(it)) + "/" + std::to_string(en->charge) + " charge";
				rows.push_back(row);
				icons.push_back(firstIcon(*en));
			}
	if (rows.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, magicTab ? "No spells of this school" : "You know no spells");
	if (list.selected >= 0 && list.selected < (int)ids.size())
	{
		const std::string& qid = ids[list.selected];
		const Object* io = qid.compare(0, 5, "item:") == 0 ? w.game.object(qid.substr(5)) : nullptr;
		assignQuickKey("magic:" + qid, io ? io->name.str() : w.game.spells[qid].name);
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
	magicShown = (int)rows.size();
	magicIcons = 0;
	for (int ic : icons)
		magicIcons += ic >= 0;
	uiList(list, 4, 46, 312, 110, rows, 0.45f, true, nullptr, &icons);
	uiText(6, 190, 0.36f, col::textDim, kQuickHint);
	// Active effects
	std::string active;
	for (auto& e : w.effects)
		if (active.find(e.source) == std::string::npos)
			active += (active.empty() ? "" : ", ") + e.source;
	if (!active.empty())
		uiText(8, 160, 0.42f, col::textDim, "Active: " + active.substr(0, 60));
	int sel = list.selected < (int)ids.size() ? list.selected : -1;
	if (deletingSpell)
	{
		// "Delete <name>?" Yes / No (OpenMW's sQuestionDeleteSpell)
		if (sel < 0 || ids[sel].compare(0, 5, "item:") == 0)
			deletingSpell = false;
		else
		{
			std::string q = w.game.gmst("squestiondeletespell", "Delete %s?");
			size_t at = q.find("%s");
			if (at != std::string::npos)
				q.replace(at, 2, w.game.spells[ids[sel]].name);
			uiRect(20, 70, 280, 100, C2D_Color32(0, 0, 0, 255));
			uiPanel(20, 70, 280, 100);
			uiTextBox(scroll, 26, 76, 268, 50, q, 0.5f, false);
			if (uiIn().down & (KEY_LEFT | KEY_RIGHT))
				focus = 1 - focus;
			int yn = -1;
			const std::string kAnswer[2] = { w.game.gmstLabel("syes", "Yes"), w.game.gmstLabel("sno", "No") };
			for (int i = 0; i < 2; i++)
				if (uiButton(34 + i * 132, 132, 120, 28, kAnswer[i], i == focus))
					yn = i;
			if (yn == 0)
				deleteSpell(ids[sel]);
			if (yn >= 0 || (uiIn().down & KEY_B))
			{
				deletingSpell = false;
				focus = 0;
			}
		}
		return;
	}
	int b = buttonRow({ "Select", w.game.gmstLabel("sdelete", "Delete"), w.game.gmst("sclose", "Close") }, focus);
	if (b == 1 && sel >= 0 && ids[sel].compare(0, 5, "item:") != 0)
	{
		// A spell bought or made goes; the race's and the birthsign's, and powers, can't (OpenMW's askDeleteSpell:
		// sDeleteSpellError). An enchanted item's entry isn't a spell to delete
		if (inherentSpell(ids[sel]))
			notify(w.game.gmst("sdeletespellerror", "You cannot delete this spell."));
		else
		{
			deletingSpell = true;
			focus = 1;               // (No)
		}
	}
	else if (b == 0 && sel >= 0)
	{
		w.stats.selectedSpell = ids[sel];
		const Object* io = ids[sel].compare(0, 5, "item:") == 0 ? w.game.object(ids[sel].substr(5)) : nullptr;
		notify((io ? io->name.str() : w.game.spells[ids[sel]].name) + " is ready (" + castHint() + ").");
		closeScreen();
	}
	else if (b == 2 || (uiIn().down & KEY_B))
		closeScreen();
}

// A spell the player can't delete: a power, or one the race or birthsign gives (OpenMW's askDeleteSpell)
bool Session::inherentSpell(const std::string& id)
{
	auto sp = w.game.spells.find(id);
	if (sp == w.game.spells.end() || sp->second.type == 5)
		return true;
	if (const RaceDef* r = w.race())
		for (auto& s : r->spells)
			if (lower(s) == id)
				return true;
	if (const BirthDef* b = w.birthsign())
		for (auto& s : b->spells)
			if (lower(s) == id)
				return true;
	return false;
}

// The player forgets the spell; the one ready to cast goes with it (OpenMW's onDeleteSpellAccept)
void Session::deleteSpell(const std::string& id)
{
	w.stats.spells.erase(std::remove_if(w.stats.spells.begin(), w.stats.spells.end(),
		[&](const std::string& s) { return lower(s) == id; }), w.stats.spells.end());
	if (w.stats.selectedSpell == id)
		w.stats.selectedSpell.clear();
	logf("magic: deleted spell %s", id.c_str());
	playSound(-1, "Menu Click");
}
