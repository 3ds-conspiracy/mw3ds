// The magic menu: spells, powers and enchanted items to cast
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/inventoryhelpers.hpp"

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
	int b = buttonRow({ "Select", w.game.gmst("sclose", "Close") }, focus);
	if (b == 0 && sel >= 0)
	{
		w.stats.selectedSpell = ids[sel];
		const Object* io = ids[sel].compare(0, 5, "item:") == 0 ? w.game.object(ids[sel].substr(5)) : nullptr;
		notify((io ? io->name.str() : w.game.spells[ids[sel]].name) + " is ready (" + castHint() + ").");
		closeScreen();
	}
	else if (b == 1 || (uiIn().down & KEY_B))
		closeScreen();
}
